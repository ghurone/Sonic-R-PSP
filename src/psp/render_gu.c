/**
 * render_gu.c — PSP GE (sceGu) implementation of the immediate-mode render API
 * (r_state.h, r_draw.h, r_texture.h) and of the frame/tpage functions the SDL
 * build keeps in render_gl.c.
 *
 * PSP counterpart to sdl/src/r_gl_backend.c + sdl/src/render_gl.c. The R_*
 * surface and the lazy state tracker are the same; only the submission changes.
 *
 * Vertices arrive pre-transformed (D3DTLVERTEX: screen x/y in the game's
 * 640x480 space, sz, rhw = 1/z). The GE is fed them in 3D mode as
 * (sx*W, sy*W, W) with W = 1/rhw, under a projection whose clip w is that W,
 * so it divides back to the same screen position while seeing each vertex's
 * real depth: textures are perspective-correct in hardware, as with the GL
 * build's glVertex4f(sx*w, sy*w, sz*w, w). Depth is the GE's z/w, i.e. linear
 * in rhw — the depth the Dreamcast build uses (r_pvr_backend.c passes rhw as
 * the PVR's z) rather than the GL build's linear sz.
 *
 * The 640x480 frame is drawn 4:3 in the middle of the 480x272 screen
 * (362x272, black bars at the sides).
 *
 * Textures live in main RAM (52 tpages do not fit the 2MB of VRAM) as
 * 5551 (colour-keyed), 5650 (no key) or 8888 (RGBA sources); anything over the
 * GE's 512 limit is halved until it fits, and non-power-of-two sizes are padded
 * with sceGuTexScale covering the used part.
 */

#include <pspkernel.h>
#include <pspgu.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <malloc.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "r_types.h"
#include "r_state.h"
#include "r_draw.h"
#include "r_texture.h"
#include "net_transport.h"
#include "gu_backend.h"

#define SCR_W    480
#define SCR_H    272
#define FB_STRIDE 512
#define NUM_TPAGES 52

/* Display list: the whole frame's commands and vertices. When a frame gets
 * close to the end, the list is run to completion and started over. */
#define LIST_SIZE   (160 * 1024)   /* (a race frame's list was 71 KB at most; past LIST_SIZE - LIST_MARGIN it runs and starts over) */
#define LIST_MARGIN (64 * 1024)
static unsigned int __attribute__((aligned(64))) s_list[LIST_SIZE / 4];
static int s_listOpen;

static int s_wide;   /* widescreen race view this frame (PSP_WideView) */
static void wide_set(int on);
void PSP_WideView(int on);

/* Frame counter, for "was this texture used by the frame being drawn" */
static unsigned int s_frameNo = 1;

#ifdef SONICR_DEBUG_LOG
/* Per-window counters for the frame profile (FlipD3D). */
static unsigned int s_profUploads, s_profUploadUs, s_profDraws, s_profVerts, s_profListRuns, s_profBatches;
#define PROF(x) x
#else
#define PROF(x)
#endif

/* Backing size / 4:3 offsets, as render_gl.c publishes them (hud_full.c builds
 * its scissor rects from these, in GL window coordinates). */
int g_glBackingWidth  = 362;
int g_glBackingHeight = 272;
int g_glViewportOffsetX = 59;
int g_glViewportOffsetY = 0;

/* =====================================================================
 * Display list
 * ===================================================================== */

/* Texture buffers replaced while the GE may still read them this frame: freed
 * once the list has run. */
#define DEFERRED_MAX 64
static void *s_deferredFree[DEFERRED_MAX];
static int s_deferredCount;

static void free_deferred(void)
{
    for (int i = 0; i < s_deferredCount; i++) {
        free(s_deferredFree[i]);
    }
    s_deferredCount = 0;
}

/* The list is written through the CPU's cache. pspgu writes it uncached
 * (sceGuStart ORs in 0x40000000), a bus write per command word, and in a
 * direct list it moves the GE's stall address — a syscall — after every
 * draw: ~20us a draw between them, 5ms of a race frame. Here, once
 * sceGuStart has queued the list, its pointers are put back on the cached
 * address and pspgu's own stall updates are kept off (they're skipped while
 * an object is open: gu_object_stack_depth, which nothing else here uses);
 * list_kick writes the new commands out of the cache and lets the GE run up
 * to them, every few draws and before the list is waited on.
 * (libpspgu internals, guInternal.h) */
typedef struct {
    unsigned int *start;
    unsigned int *current;
    int parent_context;
} PspGuDisplayList;
extern PspGuDisplayList *gu_list;
extern int gu_object_stack_depth;
extern int ge_list_executed[2];

#define KICK_DRAWS 16
static unsigned int *s_listStart, *s_listKicked;
static int s_kickDraws;

/* The last swap (FlipD3D) and the blank count it was made at: it reaches the
 * screen at the next blank, and until then the buffer the GE draws in next is
 * still the one on it. A frame late past its blank swapped after it, and the
 * next frame's draws went into the buffer on the screen: a tear across it.
 * So the GE is let on with a frame only once its swap is on the screen
 * (list_kick_to, run_list) — the CPU goes on with the frame meanwhile; waiting
 * for that blank before the frame (as first done) cost a late frame a whole
 * blank, the heavy stretches down to 45 fps. */
static unsigned int s_swapVcount;
static int s_swapPending;

static int swap_on_screen(void)
{
    if (s_swapPending && sceDisplayGetVcount() == s_swapVcount) {
        return 0;
    }
    s_swapPending = 0;
    return 1;
}

static void list_kick_to(unsigned int *end)
{
    if (!swap_on_screen()) {
        return;   /* (kept: the next kick or run_list sends it) */
    }
    if (end > s_listKicked) {
        uintptr_t from = (uintptr_t)s_listKicked & ~(uintptr_t)63;
        sceKernelDcacheWritebackRange((void *)from, (unsigned int)((uintptr_t)end - from));
        sceGeListUpdateStallAddr(ge_list_executed[0], (void *)((uintptr_t)end | 0x40000000));
        s_listKicked = end;
    }
    s_kickDraws = 0;
}

static void list_kick(void)
{
    if (s_listOpen) {
        list_kick_to(gu_list->current);
    }
}

/* after a draw: the GE let on every KICK_DRAWS of them */
static inline void list_drawn(void)
{
    if (++s_kickDraws >= KICK_DRAWS) {
        list_kick();
    }
}

/* sceGuGetMemory (sceGuClear's vertices, list_alloc), linked in place of
 * pspgu's (-Wl,--wrap): the same jump over the block, without its own stall
 * update — in a direct list it let the GE run on to the end of the block at
 * once, through commands still in the cache: it read stale ones (the
 * background flickered). The block goes with the next list_kick. */
void *__wrap_sceGuGetMemory(int size)
{
    size = (size + 3) & ~3;
    unsigned int *orig = gu_list->current;
    unsigned int *next = (unsigned int *)((char *)orig + size + 8);
    orig[0] = (16u << 24) | (((unsigned int)(uintptr_t)next >> 8) & 0xf0000);   /* BASE */
    orig[1] = (8u << 24) | ((unsigned int)(uintptr_t)next & 0xffffff);          /* JUMP */
    gu_list->current = next;
    return orig + 2;
}

static void ensure_list(void)
{
    if (!s_listOpen) {
        sceGuStart(GU_DIRECT, s_list);
        gu_object_stack_depth = 1;
        /* what sceGuStart wrote went around the cache: drop any old copy of
         * those lines, or a cached write next to it would bring it back */
        unsigned int *cur = (unsigned int *)((uintptr_t)gu_list->current & ~(uintptr_t)0x40000000);
        s_listStart = (unsigned int *)((uintptr_t)gu_list->start & ~(uintptr_t)0x40000000);
        uintptr_t lineEnd = ((uintptr_t)cur + 63) & ~(uintptr_t)63;
        sceKernelDcacheInvalidateRange(s_listStart, (unsigned int)(lineEnd - (uintptr_t)s_listStart));
        gu_list->start = s_listStart;
        gu_list->current = cur;
        s_listKicked = cur;
        s_kickDraws = 0;
        s_listOpen = 1;
    }
}

/* Run the list to completion (the GE's state carries over to the next one). */
#ifdef SONICR_DEBUG_LOG
/* (the most a frame's list and vertex arena held — FlipD3D reports them) */
static unsigned int s_hwList, s_hwArena;
#define ARENA_HW() (s_hwArena = (unsigned int)s_arenaUsed > s_hwArena ? (unsigned int)s_arenaUsed : s_hwArena)
#else
#define ARENA_HW() ((void)0)
#endif

static void run_list(void)
{
    if (s_listOpen) {
        int size = sceGuFinish();
#ifdef SONICR_DEBUG_LOG
        s_hwList = (unsigned int)size > s_hwList ? (unsigned int)size : s_hwList;
#endif
        /* (the list must run now: its buffer off the screen first — a blank
         * or so; bounded, so a display that stopped counting can't hold it) */
        for (int waits = 0; !swap_on_screen() && waits < 4; waits++) {
            sceDisplayWaitVblankStart();
        }
        s_swapPending = 0;
        list_kick_to((unsigned int *)((char *)s_listStart + size));
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
        s_listOpen = 0;
    }
    free_deferred();
}

static void *list_alloc(int bytes)
{
    ensure_list();
    if (sceGuCheckList() > LIST_SIZE - LIST_MARGIN - bytes) {
        PROF(s_profListRuns++);
        run_list();
        ensure_list();
    }
    return sceGuGetMemory(bytes);
}

/* =====================================================================
 * Textures
 * ===================================================================== */

typedef struct {
    void *data;            /* GE texture, main RAM — or VRAM (vram) */
    int   psm;             /* GU_PSM_5551 / 5650 / 8888 */
    int   pw, ph;          /* power-of-two size given to the GE */
    int   w, h;            /* used part (after any halving) */
    int   shift;           /* source pixels per texel, log2 */
    int   mipOffset;       /* bytes to mip level 1 (half size), 0 = none */
    int   bytes;           /* both levels */
    unsigned int usedFrame;
    int   vram;            /* data is a VRAM slot's (GU_VramCommit), not malloc's */
} GuTex;

static GuTex s_tex[NUM_TPAGES];
static int s_texDirty[NUM_TPAGES];
static int s_noColorKey[NUM_TPAGES];
static int s_green6[NUM_TPAGES];
static unsigned char *s_pendingRGBA[NUM_TPAGES];
static int s_pendingW[NUM_TPAGES], s_pendingH[NUM_TPAGES];
static unsigned char *s_rgba8Buf[NUM_TPAGES];
static int s_texInited;
/* Frozen tpages (R_FreezeTexture): the 16bpp source of their last upload. */
static int s_frozen[NUM_TPAGES];
static unsigned short *s_frozenSrc[NUM_TPAGES];
static size_t s_frozenSize[NUM_TPAGES];

/* The pages read most get a slot of the VRAM the frame and depth buffers
 * leave: the GE reads its own memory several times as fast as main RAM, and
 * texture reads were nearly all of its time in a race at 2x the draw
 * distance. The track and the ground grid say what they use (GU_VramWant, a
 * weight a page — their vertices on it); the heaviest that fit get a slot,
 * their textures uploaded into it from then on. */
#define VRAM_TEX_BASE  0xCC000                       /* after fb0, fb1, depth */
#define VRAM_TEX_SLOT  0x28000                       /* 256x256 16-bit and its level 1 */
#define VRAM_TEX_SLOTS ((0x200000 - VRAM_TEX_BASE) / VRAM_TEX_SLOT)
static int s_vramSlot[NUM_TPAGES];                   /* slot + 1; 0 none */
/* A race's pages, read by the GE alone once made: their 16-bit pixels let go
 * (GU_TexturesSettle) unless the game keeps them (GL_KeepPixels). */
static unsigned char s_keepPixels[NUM_TPAGES], s_srcFreed[NUM_TPAGES];
static int s_boundTex = -2;      /* tpage whose image the GE has, -2 = none */
static unsigned int s_vramWant[2][NUM_TPAGES];       /* by who wants it: the track, the grid */

static int pow2_at_least(int v)
{
    int p = 8;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/* Make room in s_tex[tp] for a w x h source in the given format. The old
 * buffer is freed now, or after this frame if the GE may still be reading it. */
static GuTex *tex_prepare(int tp, int srcW, int srcH, int psm)
{
    GuTex *t = &s_tex[tp];
    int shift = 0;
    while ((srcW >> shift) > 512 || (srcH >> shift) > 512) {
        shift++;
    }
    int w = srcW >> shift, h = srcH >> shift;
    int pw = pow2_at_least(w), ph = pow2_at_least(h);
    int bpp = (psm == GU_PSM_8888) ? 4 : 2;
    /* Level 1 at half size: the 640x480 frame is drawn at 57%, so menus and
     * HUD are minified — without it their text loses whole strokes. */
    int mipOffset = (pw >= 16 && ph >= 16) ? pw * ph * bpp : 0;
    int bytes = pw * ph * bpp + (mipOffset ? (pw / 2) * (ph / 2) * bpp : 0);

    if (t->vram) {
        t->data = NULL;   /* (its slot stays; built in main RAM, then copied there) */
        t->vram = 0;
    }
    int inUse = t->data && t->usedFrame == s_frameNo && s_listOpen;
    if (t->data && (t->bytes != bytes || inUse)) {
        if (inUse && s_deferredCount < DEFERRED_MAX) {
            s_deferredFree[s_deferredCount++] = t->data;
        } else {
            if (inUse) {
                run_list();
            }
            free(t->data);
        }
        t->data = NULL;
    }
    if (!t->data) {
        t->data = memalign(16, bytes);
        if (!t->data) {
            DebugLog("render_gu: out of memory for tpage %d (%dx%d)\n", tp, pw, ph);
            t->bytes = 0;
            return NULL;
        }
        memset(t->data, 0, bytes);
        if (bytes > 256 * 1024) {
            DebugLog("render_gu: tpage %d made %dx%d psm %d from %dx%d, %d KB\n", tp, pw, ph, psm, srcW, srcH,
                     bytes / 1024);
        }
    }
    t->psm = psm;
    t->pw = pw;
    t->ph = ph;
    t->w = w;
    t->h = h;
    t->shift = shift;
    t->mipOffset = mipOffset;
    t->bytes = bytes;
    return t;
}

/* Level 1 from level 0, 2x2 averages over the used part. Colour-keyed (5551)
 * texels average only the opaque ones and stay opaque if any one is, so thin
 * strokes (text) survive; 8888 is weighted by alpha. */
static void tex_build_mip(GuTex *t)
{
    if (!t->mipOffset) {
        return;
    }
    int mw = t->pw / 2;
    int uw = (t->w + 1) / 2, uh = (t->h + 1) / 2;
    int lastX = t->w - 1, lastY = t->h - 1;
    for (int y = 0; y < uh; y++) {
        int y0 = y * 2, y1 = y0 + 1 > lastY ? lastY : y0 + 1;
        for (int x = 0; x < uw; x++) {
            int x0 = x * 2, x1 = x0 + 1 > lastX ? lastX : x0 + 1;
            if (t->psm == GU_PSM_8888) {
                const uint32_t *l0 = (const uint32_t *)t->data;
                uint32_t p[4] = { l0[y0 * t->pw + x0], l0[y0 * t->pw + x1], l0[y1 * t->pw + x0], l0[y1 * t->pw + x1] };
                unsigned r = 0, g = 0, b = 0, a = 0;
                for (int i = 0; i < 4; i++) {
                    unsigned pa = p[i] >> 24;
                    r += (p[i] & 0xFF) * pa;
                    g += ((p[i] >> 8) & 0xFF) * pa;
                    b += ((p[i] >> 16) & 0xFF) * pa;
                    a += pa;
                }
                uint32_t out = 0;
                if (a) {
                    out = (r / a) | ((g / a) << 8) | ((b / a) << 16) | ((a / 4) << 24);
                }
                ((uint32_t *)((char *)t->data + t->mipOffset))[y * mw + x] = out;
            } else {
                const unsigned short *l0 = (const unsigned short *)t->data;
                unsigned short p[4] = { l0[y0 * t->pw + x0], l0[y0 * t->pw + x1], l0[y1 * t->pw + x0], l0[y1 * t->pw + x1] };
                unsigned short out;
                if (t->psm == GU_PSM_5650) {
                    unsigned r = 0, g = 0, b = 0;
                    for (int i = 0; i < 4; i++) {
                        r += p[i] & 0x1F;
                        g += (p[i] >> 5) & 0x3F;
                        b += p[i] >> 11;
                    }
                    out = (unsigned short)(((r + 2) / 4) | (((g + 2) / 4) << 5) | (((b + 2) / 4) << 11));
                } else {
                    unsigned r = 0, g = 0, b = 0, n = 0;
                    for (int i = 0; i < 4; i++) {
                        if (p[i] & 0x8000) {
                            r += p[i] & 0x1F;
                            g += (p[i] >> 5) & 0x1F;
                            b += (p[i] >> 10) & 0x1F;
                            n++;
                        }
                    }
                    out = n ? (unsigned short)((r / n) | ((g / n) << 5) | ((b / n) << 10) | 0x8000) : 0;
                }
                ((unsigned short *)((char *)t->data + t->mipOffset))[y * mw + x] = out;
            }
        }
    }
}

/* Rearrange a level into the GE's swizzled layout: blocks of 16 bytes x 8
 * rows, stored one after another. The texture cache works in those blocks, so
 * a linear texture in main RAM costs it many times the memory traffic —
 * the logo and title screens' wallpapers took the GE ~19ms a frame.
 * A band of 8 rows is swizzled into the same bytes it was: one band at a time
 * through a band's scratch (a whole level's, kept, was the wallpaper's 512 KB
 * for good — of a PSP-1000's 24 MB). */
static void swizzle_level(unsigned char *level, int widthBytes, int height)
{
    static uint32_t s_band[512 * 4 * 8 / 4];   /* (the widest: 512 texels of 8888) */
    if (widthBytes * 8 > (int)sizeof(s_band)) {
        return;
    }
    uint32_t *dst = (uint32_t *)level;
    int rowStride = widthBytes / 4;
    for (int by = 0; by < height; by += 8) {
        memcpy(s_band, level + by * widthBytes, (size_t)widthBytes * 8);
        for (int bx = 0; bx < widthBytes; bx += 16) {
            const uint32_t *src = (const uint32_t *)((const unsigned char *)s_band + bx);
            for (int r = 0; r < 8; r++) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
                dst += 4;
                src += rowStride;
            }
        }
    }
}

/* Give each transparent texel the average colour of its opaque neighbours
 * (alpha stays 0). Filtering reads the texels around an edge: left alone they
 * are black or the colour key's green, and showed as a dark fringe. */
static void bleed_level(void *level, int psm, int w, int h, int stride)
{
    if (psm == GU_PSM_5650) {
        return;
    }
    static const int dx[4] = { -1, 1, 0, 0 }, dy[4] = { 0, 0, -1, 1 };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned r = 0, g = 0, b = 0, n = 0;
            if (psm == GU_PSM_5551) {
                unsigned short *p = (unsigned short *)level;
                if (p[y * stride + x] & 0x8000) {
                    continue;
                }
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    unsigned q = p[ny * stride + nx];
                    if (q & 0x8000) {
                        r += q & 0x1F; g += (q >> 5) & 0x1F; b += (q >> 10) & 0x1F; n++;
                    }
                }
                p[y * stride + x] = n ? (unsigned short)((r / n) | ((g / n) << 5) | ((b / n) << 10)) : 0;
            } else {
                uint32_t *p = (uint32_t *)level;
                if (p[y * stride + x] >> 24) {
                    continue;
                }
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    uint32_t q = p[ny * stride + nx];
                    if (q >> 24) {
                        r += q & 0xFF; g += (q >> 8) & 0xFF; b += (q >> 16) & 0xFF; n++;
                    }
                }
                p[y * stride + x] = n ? (r / n) | ((g / n) << 8) | ((b / n) << 16) : 0;
            }
        }
    }
}

static void tex_done(GuTex *t)
{
    int bpp = (t->psm == GU_PSM_8888) ? 4 : 2;
    bleed_level(t->data, t->psm, t->w, t->h, t->pw);
    tex_build_mip(t);
    if (t->mipOffset) {
        bleed_level((char *)t->data + t->mipOffset, t->psm, (t->w + 1) / 2, (t->h + 1) / 2, t->pw / 2);
    }
    swizzle_level((unsigned char *)t->data, t->pw * bpp, t->ph);
    if (t->mipOffset) {
        swizzle_level((unsigned char *)t->data + t->mipOffset, (t->pw / 2) * bpp, t->ph / 2);
    }
    sceKernelDcacheWritebackRange(t->data, t->bytes);
    int tp = (int)(t - s_tex);
    if (s_vramSlot[tp] && t->bytes <= VRAM_TEX_SLOT) {
        unsigned int off = VRAM_TEX_BASE + (unsigned int)(s_vramSlot[tp] - 1) * VRAM_TEX_SLOT;
        memcpy((void *)(0x44000000u + off), t->data, (size_t)t->bytes);   /* (uncached) */
        free(t->data);
        t->data = (void *)(0x04000000u + off);
        t->vram = 1;
    }
}

void GU_VramWant(int who, int tp, unsigned int weight)
{
    if (tp >= 0 && tp < NUM_TPAGES) {
        s_vramWant[who][tp] += weight;
    }
}

void GU_VramReset(int who)
{
    memset(s_vramWant[who], 0, sizeof(s_vramWant[who]));
}

/* The heaviest wanted pages into the slots; a page moving in or out uploaded
 * again on its next bind. */
#ifdef SONICR_PROF
int g_pspNoVramTex;   /* (test, Select+L+Up: every page in main RAM — platform_psp.c) */
#endif

void GU_VramCommit(void)
{
    int next[NUM_TPAGES];
    unsigned int want[NUM_TPAGES];
    memset(next, 0, sizeof(next));
    for (int tp = 0; tp < NUM_TPAGES; tp++) {
        want[tp] = s_vramWant[0][tp] + s_vramWant[1][tp];
#ifdef SONICR_PROF
        want[tp] = g_pspNoVramTex ? 0 : want[tp];
#endif
    }
    for (int slot = 0; slot < VRAM_TEX_SLOTS; slot++) {
        int best = -1;
        for (int tp = 0; tp < NUM_TPAGES; tp++) {
            if (!next[tp] && want[tp] && (best < 0 || want[tp] > want[best])) {
                best = tp;
            }
        }
        if (best < 0) {
            break;
        }
        next[best] = slot + 1;
    }
    /* The textures already made move as they are — their pixels may be gone
     * (GU_TexturesSettle): out of the slots they lose first, then into the
     * ones they get. */
    run_list();   /* (the GE done with them) */
    for (int tp = 0; tp < NUM_TPAGES; tp++) {
        GuTex *t = &s_tex[tp];
        if (next[tp] != s_vramSlot[tp] && t->data && t->vram) {
            void *ram = memalign(16, (size_t)t->bytes);
            if (ram) {
                memcpy(ram, (void *)((uintptr_t)t->data | 0x40000000u), (size_t)t->bytes);
                sceKernelDcacheWritebackRange(ram, (unsigned)t->bytes);
            }
            t->data = ram;   /* (NULL: made again from its pixels, if any) */
            t->vram = 0;
            s_texDirty[tp] = ram == NULL;
        }
    }
    for (int tp = 0; tp < NUM_TPAGES; tp++) {
        GuTex *t = &s_tex[tp];
        s_vramSlot[tp] = next[tp];
        if (next[tp] && t->data && !t->vram && t->bytes <= VRAM_TEX_SLOT) {
            unsigned int off = VRAM_TEX_BASE + (unsigned int)(next[tp] - 1) * VRAM_TEX_SLOT;
            memcpy((void *)(0x44000000u + off), t->data, (size_t)t->bytes);   /* (uncached) */
            free(t->data);
            t->data = (void *)(0x04000000u + off);
            t->vram = 1;
        }
        if (next[tp]) {
            DebugLog("render_gu: tpage %d in VRAM slot %d (weight %u)\n", tp, next[tp] - 1, want[tp]);
        }
    }
    s_boundTex = -2;   /* (images moved) */
}

/* The game's 16bpp pixel: R5 at 15-11, G5 at 10-6 (true G6 at 10-5 when
 * green6), B5 at 4-0. GE 5551: R 0-4, G 5-9, B 10-14, A 15. GE 5650: R 0-4,
 * G 5-10, B 11-15. */
static inline unsigned short conv_5551(unsigned short p)
{
    unsigned r5 = (p >> 11) & 0x1F, g5 = (p >> 6) & 0x1F, b5 = p & 0x1F;
    unsigned a = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 0x8000;
    return (unsigned short)(r5 | (g5 << 5) | (b5 << 10) | a);
}

static inline unsigned short conv_5650(unsigned short p, int green6)
{
    unsigned r5 = (p >> 11) & 0x1F, b5 = p & 0x1F, g6;
    if (green6) {
        g6 = (p >> 5) & 0x3F;
    } else {
        unsigned g5 = (p >> 6) & 0x1F;
        g6 = (g5 << 1) | (g5 >> 4);
    }
    return (unsigned short)(r5 | (g6 << 5) | (b5 << 11));
}

/* Convert the rect [x0,x1) x [y0,y1) of a tpage's 16bpp source (texel
 * coordinates) into its GE texture. */
static void tex_convert16(int tp, const unsigned short *src, GuTex *t, int x0, int y0, int x1, int y1)
{
    unsigned short *dst = (unsigned short *)t->data;
    int srcW = g_tpageWidth[tp] ? g_tpageWidth[tp] : 256;
    int s = t->shift;
    if (t->psm == GU_PSM_5650) {
        int green6 = s_green6[tp];
        for (int y = y0; y < y1; y++) {
            const unsigned short *row = src + (size_t)(y << s) * srcW;
            unsigned short *out = dst + y * t->pw;
            for (int x = x0; x < x1; x++) {
                out[x] = conv_5650(row[x << s], green6);
            }
        }
    } else {
        for (int y = y0; y < y1; y++) {
            const unsigned short *row = src + (size_t)(y << s) * srcW;
            unsigned short *out = dst + y * t->pw;
            for (int x = x0; x < x1; x++) {
                out[x] = conv_5551(row[x << s]);
            }
        }
    }
}

/* RGBA8 bytes (R,G,B,A in memory) are the GE's 8888 layout already. */
static void tex_upload_rgba(int tp, const unsigned char *rgba, int w, int h)
{
    GuTex *t = tex_prepare(tp, w, h, GU_PSM_8888);
    if (!t) {
        return;
    }
    const uint32_t *src = (const uint32_t *)rgba;
    uint32_t *dst = (uint32_t *)t->data;
    for (int y = 0; y < t->h; y++) {
        const uint32_t *row = src + (size_t)(y << t->shift) * w;
        for (int x = 0; x < t->w; x++) {
            dst[y * t->pw + x] = row[x << t->shift];
        }
    }
    tex_done(t);
}

static void tex_upload_body(int tp);

static void tex_upload(int tp)
{
#ifdef SONICR_DEBUG_LOG
    unsigned int t0 = sceKernelGetSystemTimeLow();
    tex_upload_body(tp);
    s_profUploadUs += sceKernelGetSystemTimeLow() - t0;
    s_profUploads++;
#else
    tex_upload_body(tp);
#endif
}

static void tex_upload_body(int tp)
{
    s_texDirty[tp] = 0;
    int w = g_tpageWidth[tp] ? g_tpageWidth[tp] : 256;
    int h = g_tpageHeight[tp] ? g_tpageHeight[tp] : 256;

    /* Persistent RGBA8 override (the desktop's 32-bit sky): preferred, kept. */
    if (s_rgba8Buf[tp]) {
        tex_upload_rgba(tp, s_rgba8Buf[tp], w, h);
        return;
    }
    /* Pending full-quality RGBA (TintBackgroundTPage): uploaded once, freed. */
    if (s_pendingRGBA[tp]) {
        tex_upload_rgba(tp, s_pendingRGBA[tp], s_pendingW[tp], s_pendingH[tp]);
        free(s_pendingRGBA[tp]);
        s_pendingRGBA[tp] = NULL;
        return;
    }
    if (g_tpagePixelBuf[tp] == NULL) {
        return;
    }
    const unsigned short *src = (const unsigned short *)g_tpagePixelBuf[tp];
    /* A frozen tpage keeps a copy of what was uploaded: its later sub-rect
     * patches go onto that copy, not onto whatever the pixel buffer holds by
     * then (R_UploadTextureSubRect). */
    if (s_frozen[tp]) {
        size_t bytes = (size_t)w * h * 2;
        if (s_frozenSize[tp] != bytes) {
            free(s_frozenSrc[tp]);
            s_frozenSrc[tp] = (unsigned short *)malloc(bytes);
            s_frozenSize[tp] = s_frozenSrc[tp] ? bytes : 0;
        }
        if (s_frozenSrc[tp]) {
            memcpy(s_frozenSrc[tp], src, bytes);
            src = s_frozenSrc[tp];
        }
    }
    GuTex *t = tex_prepare(tp, w, h, s_noColorKey[tp] ? GU_PSM_5650 : GU_PSM_5551);
    if (!t) {
        return;
    }
    tex_convert16(tp, src, t, 0, 0, t->w, t->h);
    tex_done(t);
}

/* At a race's start, before its track and grid are built (render_grid_psp.c):
 * every page loaded made for the GE now, and its 16-bit pixels let go unless
 * the game keeps them (GL_KeepPixels) — before the builds, so the two copies
 * of every page are never there with them at the peak, the PSP-1000's 24 MB
 * being what it is. Such a page stays textured (apply_desired) and moves to
 * and from VRAM as made (GU_VramCommit); nothing in a race uploads it again.
 * A page's pixels let go as soon as it is made: the two copies of one page at
 * a time, not of all of them (made all first, that was the heap's peak). */
void GU_TexturesSettle(void)
{
    for (int tp = 0; tp < NUM_TPAGES; tp++) {
        if (g_tpageStateArray[tp] != 4 || g_tpagePixelBuf[tp] == NULL) {
            continue;
        }
        if (s_texDirty[tp] || s_tex[tp].data == NULL) {
            tex_upload(tp);
        }
        if (s_tex[tp].data && !s_keepPixels[tp] && !s_frozen[tp] && !s_rgba8Buf[tp]) {
            free(g_tpagePixelBuf[tp]);
            g_tpagePixelBuf[tp] = NULL;
            s_srcFreed[tp] = 1;
        }
    }
}

/* =====================================================================
 * Render state
 * ===================================================================== */

typedef struct {
    int          textureId;
    R_BlendMode  blendMode;
    int          depthTest;
    R_DepthFunc  depthFunc;
    int          depthWrite;
    R_TexEnvMode texEnv;
    R_FilterMode filter;
    R_CullMode   cullMode;
    int          alphaTest;
    float        alphaRef;
    int          scissorEnabled;
    int          scissorX, scissorY, scissorW, scissorH;
} R_StateSnapshot;

static R_StateSnapshot s_desired;
static R_StateSnapshot s_current;

#define R_STATE_STACK_DEPTH 4
static R_StateSnapshot s_stateStack[R_STATE_STACK_DEPTH];
static int s_stackDepth;

static const R_StateSnapshot s_defaults = {
    .textureId      = -1,
    .blendMode      = R_BLEND_ALPHA,
    .depthTest      = 1,
    .depthFunc      = R_DEPTH_LEQUAL,
    .depthWrite     = 1,
    .texEnv         = R_TEXENV_MODULATE,
    .filter         = R_FILTER_NEAREST,
    .cullMode       = R_CULL_NONE,
    .alphaTest      = 1,
    .alphaRef       = 0.01f,
    .scissorEnabled = 0,
    .scissorX       = 0,
    .scissorY       = 0,
    .scissorW       = 640,
    .scissorH       = 480,
};

static unsigned char s_tpageFilter[NUM_TPAGES];  /* 0 = none, else mode + 1 */
/* Level 0 only (GU_TrackTextures, while g_pspTrackNoMip): the track's pages
 * are atlases of tiles, and a bilinear sample of level 1 at a tile's edge
 * takes a quarter of the next tile. It was thought to be the floor's seams
 * (they were the grid's), and without level 1 the distant track — twice as
 * much of it at 2x the draw distance — read its full-size pages from main
 * RAM a texel apart: the GE's slowest work at 60 fps. */
static int s_noMip;
int g_pspTrackNoMip;   /* (test, Select+L+Down — platform_psp.c) */
#ifdef SONICR_PROF
int g_pspFogPasses = 1;  /* (test, Select+L+Square: the fog band a vertex at a time — platform_psp.c) */
#endif
static int s_appliedFilter = -1;
static int s_texEnabled = -1;    /* GU_TEXTURE_2D as last sent, -1 = unknown */

/* Bind tpage tp's texture (uploading it first if dirty). 0 if it has none. */
static int bind_texture(int tp)
{
    if (s_texDirty[tp] || (s_tex[tp].data == NULL && g_tpagePixelBuf[tp] != NULL)) {
        tex_upload(tp);
        s_boundTex = -2;   /* new data: set the image again and flush the texture cache */
    }
    GuTex *t = &s_tex[tp];
    if (!t->data) {
        return 0;
    }
    int mip = t->mipOffset && !s_noMip;
    if (s_boundTex != tp) {
        sceGuTexMode(t->psm, mip ? 1 : 0, 0, 1);   /* swizzled (tex_done) */
        sceGuTexImage(0, t->pw, t->ph, t->pw, t->data);
        if (mip) {
            sceGuTexImage(1, t->pw / 2, t->ph / 2, t->pw / 2, (char *)t->data + t->mipOffset);
        }
        sceGuTexScale((float)t->w / (float)t->pw, (float)t->h / (float)t->ph);
        sceGuTexOffset(0.0f, 0.0f);
        sceGuTexFlush();
        s_boundTex = tp;
    }
    t->usedFrame = s_frameNo;

    /* Magnification is the game's choice; minification goes through the mip
     * level when there is one — the whole 640x480 frame is shrunk to 362x272. */
    int want = s_tpageFilter[tp] ? s_tpageFilter[tp] - 1 : (int)s_desired.filter;
    /* Tpages the game pins to nearest (texture.c: sprite and font atlases,
     * TITLES0's "PRESS START") stay point-sampled, from the mip level that
     * fits — a linear filter pulls the neighbouring atlas cells into each
     * quad's border. */
    int pinnedNearest = s_tpageFilter[tp] == R_FILTER_NEAREST + 1;
    int key = want * 4 + pinnedNearest * 2 + mip;
    if (key != s_appliedFilter) {
        int mag = (want == R_FILTER_LINEAR) ? GU_LINEAR : GU_NEAREST;
        int min;
        if (pinnedNearest) {
            min = mip ? GU_NEAREST_MIPMAP_NEAREST : GU_NEAREST;
        } else {
            min = mip ? GU_LINEAR_MIPMAP_LINEAR : GU_LINEAR;
        }
        sceGuTexFilter(min, mag);
        s_appliedFilter = key;
    }
    return 1;
}

static void flush_batch(void);

/* Send the GE whatever of s_desired differs from what it has. */
static void apply_desired(void)
{
    ensure_list();

    /* Texture: rebound every flush, since binding is also where a dirty tpage
     * is uploaded; bind_texture skips the GE commands when nothing changed. */
    int tp = s_desired.textureId;
    int textured = (tp >= 0 && tp < NUM_TPAGES && (g_tpagePixelBuf[tp] != NULL || s_srcFreed[tp])) ? bind_texture(tp) : 0;
#ifdef SONICR_PROF
    extern int g_pspNoTextures;   /* Select+L+Right: the GE reads no textures (bus contention test) */
    if (g_pspNoTextures) {
        textured = 0;
    }
#endif
    if (textured != s_texEnabled) {
        if (textured) {
            sceGuEnable(GU_TEXTURE_2D);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
        s_texEnabled = textured;
    }
    s_current.textureId = textured ? tp : -1;
    s_current.filter = s_desired.filter;

    if (s_desired.depthTest != s_current.depthTest) {
        if (s_desired.depthTest) {
            sceGuEnable(GU_DEPTH_TEST);
        } else {
            sceGuDisable(GU_DEPTH_TEST);
        }
        s_current.depthTest = s_desired.depthTest;
    }

    /* Depth is inverted on the GE (near = 65535): LEQUAL becomes GEQUAL. */
    if (s_desired.depthFunc != s_current.depthFunc) {
        switch (s_desired.depthFunc) {
            case R_DEPTH_LEQUAL: sceGuDepthFunc(GU_GEQUAL);  break;
            case R_DEPTH_LESS:   sceGuDepthFunc(GU_GREATER); break;
            case R_DEPTH_ALWAYS: sceGuDepthFunc(GU_ALWAYS);  break;
        }
        s_current.depthFunc = s_desired.depthFunc;
    }

    if (s_desired.depthWrite != s_current.depthWrite) {
        sceGuDepthMask(s_desired.depthWrite ? GU_FALSE : GU_TRUE);
        s_current.depthWrite = s_desired.depthWrite;
    }

    /* Add Signed (texture + colour - 0.5) has no GE equivalent; modulate with
     * colour doubling (texture * colour * 2) is neutral at the same 0.5 and
     * brightens/darkens the same way either side of it. */
    if (s_desired.texEnv != s_current.texEnv) {
        if (s_desired.texEnv == R_TEXENV_ADD_SIGNED) {
            sceGuEnable(GU_FRAGMENT_2X);
        } else {
            sceGuDisable(GU_FRAGMENT_2X);
        }
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        s_current.texEnv = s_desired.texEnv;
    }

    if (s_desired.blendMode != s_current.blendMode) {
        switch (s_desired.blendMode) {
            case R_BLEND_NONE:
                sceGuDisable(GU_BLEND);
                break;
            case R_BLEND_ALPHA:
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
                break;
            case R_BLEND_ADDITIVE:
                sceGuEnable(GU_BLEND);
                sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
                break;
        }
        s_current.blendMode = s_desired.blendMode;
    }

    if (s_desired.alphaTest != s_current.alphaTest) {
        if (s_desired.alphaTest) {
            sceGuEnable(GU_ALPHA_TEST);
        } else {
            sceGuDisable(GU_ALPHA_TEST);
        }
        s_current.alphaTest = s_desired.alphaTest;
    }

    if (s_desired.alphaRef != s_current.alphaRef) {
        int ref = (int)(s_desired.alphaRef * 255.0f);
        if (ref < 0) ref = 0;
        if (ref > 255) ref = 255;
        sceGuAlphaFunc(GU_GREATER, ref, 0xFF);
        s_current.alphaRef = s_desired.alphaRef;
    }

    /* Scissor: GL window coordinates (origin bottom-left) → GE (top-left),
     * always within the 4:3 area — the GL build's glViewport clips every draw
     * to it, and the game's geometry runs past the 640x480 edges (it showed
     * in the side bars). Off means the whole 4:3 area. */
    if (s_desired.scissorEnabled != s_current.scissorEnabled ||
        (s_desired.scissorEnabled &&
         (s_desired.scissorX != s_current.scissorX || s_desired.scissorY != s_current.scissorY ||
          s_desired.scissorW != s_current.scissorW || s_desired.scissorH != s_current.scissorH)))
    {
        int ax0 = g_glViewportOffsetX, ay0 = SCR_H - (g_glViewportOffsetY + g_glBackingHeight);
        int ax1 = ax0 + g_glBackingWidth, ay1 = ay0 + g_glBackingHeight;
        if (s_wide) {
            ax0 = 0;   /* (widescreen: the race view runs into the sides) */
            ax1 = SCR_W;
        }
        int x0 = ax0, y0 = ay0, x1 = ax1, y1 = ay1;
        if (s_desired.scissorEnabled) {
            x0 = s_desired.scissorX;
            y0 = SCR_H - (s_desired.scissorY + s_desired.scissorH);
            x1 = x0 + s_desired.scissorW;
            y1 = y0 + s_desired.scissorH;
            if (x0 < ax0) x0 = ax0;
            if (y0 < ay0) y0 = ay0;
            if (x1 > ax1) x1 = ax1;
            if (y1 > ay1) y1 = ay1;
        }
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }   /* (nothing visible) */
        sceGuScissor(x0, y0, x1 - x0, y1 - y0);
        s_current.scissorEnabled = s_desired.scissorEnabled;
        s_current.scissorX = s_desired.scissorX;
        s_current.scissorY = s_desired.scissorY;
        s_current.scissorW = s_desired.scissorW;
        s_current.scissorH = s_desired.scissorH;
    }
}

/* Draws are batched (R_DrawTriFan), so state takes effect when a batch is sent;
 * this sends the open batch and then the current state, for the GE commands
 * that follow outside a batch (clears). */
void R_FlushState(void)
{
    flush_batch();
    apply_desired();
}

void R_SetTexture(int tpageIndex)            { s_desired.textureId = tpageIndex; }
void R_SetBlendMode(R_BlendMode mode)        { s_desired.blendMode = mode; }
void R_SetDepthTest(int enable)              { s_desired.depthTest = enable; }
void R_SetDepthFunc(R_DepthFunc func)        { s_desired.depthFunc = func; }
void R_SetDepthWrite(int enable)             { s_desired.depthWrite = enable; }
void R_SetTexEnv(R_TexEnvMode mode)          { s_desired.texEnv = mode; }
void R_SetFilter(R_FilterMode mode)          { s_desired.filter = mode; }
void R_SetCullMode(R_CullMode mode)          { s_desired.cullMode = mode; }   /* the GL build never culls either */
void R_SetAlphaTest(int enable)              { s_desired.alphaTest = enable; }
void R_SetAlphaRef(float ref)                { s_desired.alphaRef = ref; }

void R_SetTpageFilter(int tpage, R_FilterMode mode)
{
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_tpageFilter[tpage] = (unsigned char)(mode + 1);
    }
}

void R_ClearTpageFilter(int tpage)
{
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_tpageFilter[tpage] = 0;
    }
}

/* DC-only upload transform (offsets its additive Add Signed stand-in). */
void R_SetTpageSatBoost(int tpage, int k256)
{
    (void)tpage;
    (void)k256;
}

void R_SetScissor(int x, int y, int w, int h)
{
    s_desired.scissorEnabled = 1;
    s_desired.scissorX = x;
    s_desired.scissorY = y;
    s_desired.scissorW = w;
    s_desired.scissorH = h;
}

void R_DisableScissor(void)
{
    s_desired.scissorEnabled = 0;
}

void R_PushState(void)
{
    if (s_stackDepth < R_STATE_STACK_DEPTH) {
        s_stateStack[s_stackDepth++] = s_desired;
    }
}

void R_PopState(void)
{
    if (s_stackDepth > 0) {
        s_desired = s_stateStack[--s_stackDepth];
    }
}

void R_DebugGetState(int *depthTest, int *depthWrite, int *depthFunc,
                     int *blendMode, int *alphaTest, float *alphaRef)
{
    if (depthTest)  *depthTest  = s_desired.depthTest;
    if (depthWrite) *depthWrite = s_desired.depthWrite;
    if (depthFunc)  *depthFunc  = (int)s_desired.depthFunc;
    if (blendMode)  *blendMode  = (int)s_desired.blendMode;
    if (alphaTest)  *alphaTest  = s_desired.alphaTest;
    if (alphaRef)   *alphaRef   = s_desired.alphaRef;
}

void R_ResetState(void)
{
    flush_batch();
    s_desired = s_defaults;
    /* Invalidate everything so the next flush sends it all (see r_gl_backend.c
     * for why textureId needs a value of its own). */
    memset(&s_current, 0xFF, sizeof(s_current));
    s_current.textureId = -99;
    s_current.alphaRef = -1.0f;
    s_boundTex = -2;
    s_appliedFilter = -1;
    s_texEnabled = -1;
    s_stackDepth = 0;
}

/* =====================================================================
 * Geometry
 * ===================================================================== */

/* Depth. The 16-bit depth is 1/w, its near plane (65535) at w = DEPTH_NEAR
 * rather than the game's depth 1: with it at 1 nearly all its steps went to
 * the first units in front of the camera — ~15 units a step at 1000. Depths
 * from DEPTH_3D on (all the 3D the GE draws: GE_Z_MIN and the like) keep
 * their own w, 30 times finer than before; nearer ones (the 2D at depth 1 to
 * 10, the fade iris at 1.1, the CPU-clipped bits off the bottom of the
 * screen) are squeezed into DEPTH_NEAR..DEPTH_3D in their order, so the iris
 * still covers a course name drawn after it. Their screen position is
 * unchanged (the w scales the position too). rhw <= 0 is the farthest, as on
 * the DC. */
#define RHW_MIN    0.00001f
#define DEPTH_NEAR 30.0f
#define DEPTH_3D   32.0f

static inline float depth_w(float rhw)
{
    if (!(rhw > RHW_MIN)) {
        rhw = RHW_MIN;
    }
    float z = 1.0f / rhw;
    if (z >= DEPTH_3D) {
        return z;
    }
    if (z < 1.0f) {
        z = 1.0f;
    }
    return DEPTH_NEAR + (DEPTH_3D - DEPTH_NEAR) * (z - 1.0f) / (DEPTH_3D - 1.0f);
}

static inline void emit_vertex(GuVertex *o, const RenderVertex *v)
{
    float w = depth_w(v->rhw);
    uint32_t c = v->color;
    o->u = v->u;
    o->v = v->v;
    o->color = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    o->x = v->sx * w;
    o->y = v->sy * w;
    o->z = w;
}

#ifdef SONICR_PROF
int g_pspSkipDraws;   /* Select+L+Left (platform_psp.c): the draws stop here — what the backend costs */
int g_pspNoTextures;  /* Select+L+Right: drawn untextured — what the GE's texture reads cost the CPU */
#endif

/* Batching. A draw per game primitive cost ~4us of CPU each (2000+ a race
 * frame): its vertices and commands were written into the display list, which
 * sceGuStart makes uncached. Now the primitives are turned into triangles in a
 * cached arena, consecutive ones with the same state make one batch, and a
 * batch goes to the GE as one draw: its state applied once, its vertices
 * written back from the cache once.
 *
 * Anything whose order against the draws matters sends the open batch first:
 * clears and R_FlushState, the projection (BeginFrame), the flip, and every
 * change to a texture's pixels (a batch must not see newer ones). */
#define ARENA_VERTS (8 * 1024)     /* (a race frame used under 3000) */
static GuVertex __attribute__((aligned(64))) s_arena[ARENA_VERTS];
static int s_arenaUsed;          /* this frame's vertices so far */
static int s_batchStart, s_batchCount;
static R_StateSnapshot s_batchState;

/* What a batch's state is compared on: everything the GE is given (the cull
 * mode isn't, as in the GL build). */
static int same_batch_state(const R_StateSnapshot *a, const R_StateSnapshot *b)
{
    return a->textureId == b->textureId && a->blendMode == b->blendMode &&
           a->depthTest == b->depthTest && a->depthFunc == b->depthFunc &&
           a->depthWrite == b->depthWrite && a->texEnv == b->texEnv &&
           a->filter == b->filter && a->alphaTest == b->alphaTest &&
           a->alphaRef == b->alphaRef && a->scissorEnabled == b->scissorEnabled &&
           (!a->scissorEnabled ||
            (a->scissorX == b->scissorX && a->scissorY == b->scissorY &&
             a->scissorW == b->scissorW && a->scissorH == b->scissorH));
}

static void flush_batch(void)
{
    if (s_batchCount == 0) {
        return;
    }
    GuVertex *v = &s_arena[s_batchStart];
    int n = s_batchCount;
    s_batchCount = 0;

    R_StateSnapshot saved = s_desired;
    s_desired = s_batchState;
    apply_desired();
    s_desired = saved;

    sceKernelDcacheWritebackRange(v, n * (int)sizeof(GuVertex));
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE, n, NULL, v);
    list_drawn();
    PROF(s_profBatches++);

    /* (the list holds only commands now; near its end, run it — and with the
     * GE done, the arena can start over too) */
    if (sceGuCheckList() > LIST_SIZE - LIST_MARGIN) {
        PROF(s_profListRuns++);
        run_list();
        ARENA_HW();
        s_arenaUsed = 0;
    }
}

/* The GE's guard band. It clips nothing in x/y: a primitive with a vertex past
 * its 4096-pixel drawing area is dropped whole — a floor polygon clipped only
 * at the near plane reaches tens of thousands of pixels off screen, and went
 * missing at the camera (the sea showed through). Such a fan is clipped here
 * to GUARD_PX around the viewport centre (2048 of room), in the game's screen
 * units (set_projection). */
#define GUARD_PX 1700.0f
static float s_guardX0 = -1e30f, s_guardX1 = 1e30f, s_guardY0 = -1e30f, s_guardY1 = 1e30f;

typedef struct {
    float x, y, w;          /* homogeneous: (sx, sy) * w, w = 1 / rhw */
    float u, v;
    float c[4];             /* colour channels, ARGB order */
} ClipVert;

static void clip_lerp(ClipVert *o, const ClipVert *a, const ClipVert *b, float t)
{
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->w = a->w + (b->w - a->w) * t;
    o->u = a->u + (b->u - a->u) * t;
    o->v = a->v + (b->v - a->v) * t;
    for (int k = 0; k < 4; k++) {
        o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
    }
}

/* Sutherland-Hodgman against s * (coord - bound * w) <= 0, coord x (axis 0)
 * or y (axis 1). */
static int clip_plane(const ClipVert *in, int n, ClipVert *out, int axis, float bound, float s)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const ClipVert *a = &in[i], *b = &in[(i + 1) % n];
        float da = s * ((axis ? a->y : a->x) - bound * a->w);
        float db = s * ((axis ? b->y : b->x) - bound * b->w);
        if (da <= 0.0f) {
            out[m++] = *a;
        }
        if ((da <= 0.0f) != (db <= 0.0f)) {
            /* from the inside vertex out, whichever way the edge is walked:
             * the neighbour sharing it gets the very same point */
            if (da <= 0.0f) {
                clip_lerp(&out[m++], a, b, da / (da - db));
            } else {
                clip_lerp(&out[m++], b, a, db / (db - da));
            }
        }
    }
    return m;
}

static void draw_fan_guarded(const RenderVertex *v, int count)
{
    ClipVert buf[2][24];
    int n = count;
    for (int i = 0; i < n; i++) {
        /* (the true w: emit_vertex squeezes the near ones once the fan is cut) */
        float rhw = v[i].rhw;
        if (!(rhw > RHW_MIN)) rhw = RHW_MIN;
        if (rhw > 1.0f) rhw = 1.0f;
        float w = 1.0f / rhw;
        uint32_t c = v[i].color;
        ClipVert *o = &buf[0][i];
        o->x = v[i].sx * w;
        o->y = v[i].sy * w;
        o->w = w;
        o->u = v[i].u;
        o->v = v[i].v;
        o->c[0] = (float)(c >> 24);
        o->c[1] = (float)((c >> 16) & 0xFF);
        o->c[2] = (float)((c >> 8) & 0xFF);
        o->c[3] = (float)(c & 0xFF);
    }
    n = clip_plane(buf[0], n, buf[1], 0, s_guardX1, 1.0f);
    n = clip_plane(buf[1], n, buf[0], 0, s_guardX0, -1.0f);
    n = clip_plane(buf[0], n, buf[1], 1, s_guardY1, 1.0f);
    n = clip_plane(buf[1], n, buf[0], 1, s_guardY0, -1.0f);
    if (n < 3) {
        return;
    }
    RenderVertex rv[24];
    for (int i = 0; i < n; i++) {
        const ClipVert *c = &buf[0][i];
        float rhw = 1.0f / c->w;
        rv[i].sx = c->x * rhw;
        rv[i].sy = c->y * rhw;
        rv[i].sz = 0.0f;
        rv[i].rhw = rhw;
        uint32_t ch[4];
        for (int k = 0; k < 4; k++) {
            int q = (int)(c->c[k] + 0.5f);
            ch[k] = (uint32_t)(q < 0 ? 0 : q > 255 ? 255 : q);
        }
        rv[i].color = (ch[0] << 24) | (ch[1] << 16) | (ch[2] << 8) | ch[3];
        rv[i].specular = 0;
        rv[i].u = c->u;
        rv[i].v = c->v;
    }
    R_DrawTriFan(rv, n);
}

/* The race HUD in widescreen (PSP_HudWide, around DrawTimerAndStatus): it
 * places itself on the 640x480 screen, so an element in its left third goes
 * to the wide screen's left edge, one in its right third to the right edge
 * (the clip margin either way), one in the middle stays centred — decided
 * per primitive, on its centre. */
static int s_hudShift;
static float s_hudMargin;

/* Add Signed done exactly (PSP_ExactAddSigned): texture + colour - 0.5,
 * which the doubled modulate only matches at mid colours — the env-mapped
 * "R" of the title went black on its dark side, where Add Signed leaves the
 * texture dimmed. Two passes: the colour above 0.5 added on the GE
 * (GU_TFX_ADD), then the part below it subtracted from the frame. For the
 * env-mapped models only: opaque, and few. */
static int s_exactAddSigned;

static void draw_fan_add_signed(const RenderVertex *v, int count)
{
    flush_batch();
    int n = (count - 2) * 3;
    if (s_arenaUsed + 2 * n > ARENA_VERTS) {
        run_list();
        ARENA_HW();
        s_arenaUsed = 0;
    }
    GuVertex *a = &s_arena[s_arenaUsed];
    GuVertex *b = a + n;
    s_arenaUsed += 2 * n;
    GuVertex fan[8];
    for (int i = 0; i < count; i++) {
        emit_vertex(&fan[i], &v[i]);
    }
    for (int t = 0; t < count - 2; t++) {
        const GuVertex *tri[3] = { &fan[0], &fan[t + 1], &fan[t + 2] };
        for (int k = 0; k < 3; k++) {
            GuVertex *pa = &a[t * 3 + k], *pb = &b[t * 3 + k];
            *pa = *tri[k];
            *pb = *tri[k];
            uint32_t c = tri[k]->color, ca = c & 0xFF000000u, cb = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) {
                int ch = (int)((c >> sh) & 0xFF);
                ca |= (uint32_t)(ch > 128 ? ch - 128 : 0) << sh;   /* (colour - 0.5) above zero */
                cb |= (uint32_t)(ch < 128 ? 128 - ch : 0) << sh;   /* and below it */
            }
            pa->color = ca;
            pb->color = cb;
        }
    }
    sceKernelDcacheWritebackRange(a, 2 * n * (int)sizeof(GuVertex));

    apply_desired();
    sceGuDisable(GU_FRAGMENT_2X);
    sceGuTexFunc(GU_TFX_ADD, GU_TCC_RGBA);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE, n, NULL, a);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
    sceGuDrawArray(GU_TRIANGLES, GU_VTYPE, n, NULL, b);
    list_drawn();
    s_current.texEnv = -1;      /* (the next draw sets its own again) */
    s_current.blendMode = -1;
    s_texEnabled = -1;
}

void PSP_ExactAddSigned(int on)
{
    s_exactAddSigned = on;
}

void PSP_HudWide(int on)
{
    s_hudShift = on && s_wide && g_numHumans <= 1 && g_postRaceCameraMode == 0;
    s_hudMargin = (float)g_glViewportOffsetX * (float)g_screenWidth / (float)g_glBackingWidth;
}

int PSP_WideActive(void)
{
    return s_wide;
}

void R_DrawTriFan(const RenderVertex *v, int count)
{
    if (count < 3) {
        return;
    }
    RenderVertex shifted[8];
    if (s_hudShift && count <= 8) {
        float cx = 0.0f;
        for (int i = 0; i < count; i++) {
            cx += v[i].sx;
        }
        cx /= (float)count;
        float third = (float)g_screenWidth / 3.0f;
        float dx = cx < third ? -s_hudMargin : cx > 2.0f * third ? s_hudMargin : 0.0f;
        if (dx != 0.0f) {
            for (int i = 0; i < count; i++) {
                shifted[i] = v[i];
                shifted[i].sx += dx;
            }
            v = shifted;
        }
    }
    if (s_exactAddSigned && s_desired.texEnv == R_TEXENV_ADD_SIGNED && count <= 8) {
        draw_fan_add_signed(v, count);
        return;
    }
    if (count <= 8) {
        for (int i = 0; i < count; i++) {
            /* (the clipped fan comes back inside, a hair's margin aside) */
            if (v[i].sx < s_guardX0 - 1.0f || v[i].sx > s_guardX1 + 1.0f ||
                v[i].sy < s_guardY0 - 1.0f || v[i].sy > s_guardY1 + 1.0f) {
                draw_fan_guarded(v, count);
                return;
            }
        }
    }
    PROF(s_profDraws++);
#ifdef SONICR_PROF
    if (g_pspSkipDraws) {
        return;
    }
#endif
    int n = (count - 2) * 3;
    if (s_batchCount && !same_batch_state(&s_batchState, &s_desired)) {
        flush_batch();
    }
    if (s_arenaUsed + n > ARENA_VERTS) {
        /* full: send what's open, let the GE finish with the arena, reuse it */
        flush_batch();
        PROF(s_profListRuns++);
        run_list();
        ARENA_HW();
        s_arenaUsed = 0;
    }
    if (s_batchCount == 0) {
        s_batchState = s_desired;
        s_batchStart = s_arenaUsed;
    }
    PROF(s_profVerts += n);

    /* the fan as triangles (0, i, i+1) */
    GuVertex first, prev, cur;
    emit_vertex(&first, &v[0]);
    emit_vertex(&prev, &v[1]);
    GuVertex *o = &s_arena[s_arenaUsed];
    for (int i = 2; i < count; i++) {
        emit_vertex(&cur, &v[i]);
        o[0] = first;
        o[1] = prev;
        o[2] = cur;
        o += 3;
        prev = cur;
    }
    s_arenaUsed += n;
    s_batchCount += n;
}

void R_DrawTri(const RenderVertex v[3])
{
    R_DrawTriFan(v, 3);
}

void R_DrawQuad(const RenderVertex v[4])
{
    R_DrawTriFan(v, 4);
}

void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color)
{
    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, u0, v0 };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, u1, v0 };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, u1, v1 };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, u0, v1 };
    R_DrawQuad(v);
}

void R_DrawQuad2DSolid(float x0, float y0, float x1, float y1,
                       float z, uint32_t color)
{
    int savedTex = s_desired.textureId;
    s_desired.textureId = -1;

    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    R_DrawQuad(v);

    s_desired.textureId = savedTex;
}

/* glClear of the given buffers, inside the current scissor — or the whole
 * screen, side bars included, with the scissor off (glClear ignores the
 * viewport that draws are clipped to). */
static void clear_buffers(int color, int depth)
{
    R_FlushState();
    if (!s_desired.scissorEnabled) {
        sceGuScissor(0, 0, SCR_W, SCR_H);
        s_current.scissorEnabled = -1;   /* the 4:3 rect goes back on the next flush */
    }
#ifdef SONICR_PROF
    extern int g_pspNoBackground;   /* (test: gaps in the track show magenta) */
    sceGuClearColor(g_pspNoBackground ? 0xFFFF00FF : 0xFF000000);
#else
    sceGuClearColor(0xFF000000);
#endif
    sceGuClearDepth(0);
    sceGuClear((color ? GU_COLOR_BUFFER_BIT : 0) | (depth ? GU_DEPTH_BUFFER_BIT : 0));
    list_drawn();
}

void R_ClearDepth(void)
{
    clear_buffers(0, 1);
}

/* =====================================================================
 * Texture API
 * ===================================================================== */

void R_InitTextures(void)
{
    if (s_texInited) {
        return;
    }
    for (int i = 0; i < NUM_TPAGES; i++) {
        s_texDirty[i] = 1;
    }
    s_texInited = 1;
}

void R_UploadTexture(int tpage)
{
    flush_batch();
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        tex_upload(tpage);
        s_boundTex = -2;
    }
}

void R_MarkTextureDirty(int tpage)
{
    flush_batch();
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_texDirty[tpage] = 1;
    }
}

void R_ClearTextureDirty(int tpage)
{
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_texDirty[tpage] = 0;
    }
}

/* Upload now; the caller then patches sub-rects without re-uploading the rest
 * (render_gl.c GL_FreezeTpage). */
void R_FreezeTexture(int tpage)
{
    if (tpage >= 0 && tpage < NUM_TPAGES && g_tpagePixelBuf[tpage] != NULL) {
        s_frozen[tpage] = 1;
        R_UploadTexture(tpage);
    }
}

void R_ThawTexture(int tpage)
{
    (void)tpage;
}

void R_UploadTextureSubRect(int tpage, int x, int y, int w, int h)
{
    flush_batch();
    if (tpage < 0 || tpage >= NUM_TPAGES || g_tpagePixelBuf[tpage] == NULL) {
        return;
    }
    /* The GE texture is swizzled (and has a mip level), so a patch can't be
     * written in place. The rect goes onto the copy kept at the freeze (what
     * glTexSubImage2D does to the GL texture), and the texture is built again
     * from that copy — never from the rest of the pixel buffer, which the
     * game reuses after freezing (ICON01's portraits got its stray contents). */
    int tw = g_tpageWidth[tpage] ? g_tpageWidth[tpage] : 256;
    int th = g_tpageHeight[tpage] ? g_tpageHeight[tpage] : 256;
    unsigned short *snap = s_frozenSrc[tpage];
    if (!s_frozen[tpage] || snap == NULL || s_frozenSize[tpage] != (size_t)tw * th * 2) {
        R_UploadTexture(tpage);
        return;
    }
    const unsigned short *live = (const unsigned short *)g_tpagePixelBuf[tpage];
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > tw) w = tw - x;
    if (y + h > th) h = th - y;
    for (int row = 0; row < h; row++) {
        memcpy(snap + (size_t)(y + row) * tw + x, live + (size_t)(y + row) * tw + x, (size_t)w * 2);
    }
    GuTex *t = tex_prepare(tpage, tw, th, s_noColorKey[tpage] ? GU_PSM_5650 : GU_PSM_5551);
    if (!t) {
        return;
    }
    tex_convert16(tpage, snap, t, 0, 0, t->w, t->h);
    tex_done(t);
    s_texDirty[tpage] = 0;
    s_boundTex = -2;
}

void R_SetNoColorKey(int tpage)
{
    flush_batch();
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_noColorKey[tpage] = 1;
        s_texDirty[tpage] = 1;
    }
}

void R_ClearNoColorKey(int tpage)
{
    flush_batch();
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_noColorKey[tpage] = 0;
        s_texDirty[tpage] = 1;
    }
}

void R_SetTpageGreen6(int tpage, int on)
{
    flush_batch();
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_green6[tpage] = on ? 1 : 0;
    }
}

void R_SetTpageRGBA8(int tpage, unsigned char *rgba)
{
    flush_batch();
    if (tpage < 0 || tpage >= NUM_TPAGES) {
        return;
    }
    if (s_rgba8Buf[tpage] && s_rgba8Buf[tpage] != rgba) {
        free(s_rgba8Buf[tpage]);
    }
    s_rgba8Buf[tpage] = rgba;
    s_texDirty[tpage] = 1;
}

void R_UploadTextureRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    flush_batch();
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        tex_upload_rgba(tpage, rgba, w, h);
        s_texDirty[tpage] = 0;
        s_boundTex = -2;
    }
}

void R_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    flush_batch();
    if (tpage < 0 || tpage >= NUM_TPAGES) {
        free(rgba);
        return;
    }
    free(s_pendingRGBA[tpage]);
    s_pendingRGBA[tpage] = rgba;
    s_pendingW[tpage] = w;
    s_pendingH[tpage] = h;
    s_texDirty[tpage] = 1;
}

/* GL keeps the pixel buffer for read-back on these; ours always stays. */
/* The pages whose 16-bit pixels the game reads again after loading them
 * (the ground tiles for the off-track test, the sky, the menus' backdrop...):
 * it says so here. Kept for good once said. */
void GL_KeepPixels(int tpage)
{
    if (tpage >= 0 && tpage < NUM_TPAGES) {
        s_keepPixels[tpage] = 1;
    }
}

/* =====================================================================
 * GE setup
 * ===================================================================== */

int GU_Init(void)
{
    /* VRAM (offsets): two 16-bit framebuffers, then the depth buffer. */
    void *fbp0 = (void *)0;
    void *fbp1 = (void *)(FB_STRIDE * SCR_H * 2);
    void *zbp  = (void *)(FB_STRIDE * SCR_H * 4);

#ifdef SONICR_PRX
    /* Under PSPLink, a build stopped mid-frame can leave its list stalled in
     * the GE's queue, where ours would wait behind it: start from empty. */
    PspGeBreakParam brk;
    sceGeBreak(1, &brk);
#endif
    sceGuInit();
    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, FB_STRIDE);
    sceGuDispBuffer(SCR_W, SCR_H, fbp1, FB_STRIDE);
    sceGuDepthBuffer(zbp, FB_STRIDE);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_DITHER);
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);   /* mip level from each primitive's texel/pixel ratio */
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

    ScePspFMatrix4 identity;
    memset(&identity, 0, sizeof(identity));
    identity.x.x = identity.y.y = identity.z.z = identity.w.w = 1.0f;
    sceGuSetMatrix(GU_VIEW, &identity);
    sceGuSetMatrix(GU_MODEL, &identity);
    sceGuSetMatrix(GU_PROJECTION, &identity);

    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTFRAME);   /* (a swap shown from the next blank: swap_on_screen) */

    R_InitTextures();
    R_ResetState();
    return 0;
}

/* The projection that undoes the W the vertices were multiplied by: clip
 * (x, y) / w lands on the game's screen position mapped into the 4:3 area,
 * clip z / w = 1 - 2 * rhw (near plane rhw 1 → -1 → depth 65535, rhw 0 → +1
 * → depth 0), clip w = W. */
static void set_projection(void)
{
    float sx = (float)g_glBackingWidth / (float)(g_screenWidth > 0 ? g_screenWidth : 640);
    float sy = (float)g_glBackingHeight / (float)(g_screenHeight > 0 ? g_screenHeight : 480);
    float hw = SCR_W * 0.5f, hh = SCR_H * 0.5f;

    ScePspFMatrix4 m;
    memset(&m, 0, sizeof(m));
    m.x.x = sx / hw;
    m.y.y = -sy / hh;
    m.z.x = ((float)g_glViewportOffsetX - hw) / hw;
    m.z.y = (hh - (float)g_glViewportOffsetY) / hh;
    m.z.z = 1.0f;
    m.z.w = 1.0f;
    m.w.z = -2.0f * DEPTH_NEAR;
    sceGuSetMatrix(GU_PROJECTION, &m);

    /* the guard band (R_DrawTriFan) back in game units: framebuffer pixel
     * = s * game + offset, within GUARD_PX of the viewport centre */
    s_guardX0 = (hw - GUARD_PX - (float)g_glViewportOffsetX) / sx;
    s_guardX1 = (hw + GUARD_PX - (float)g_glViewportOffsetX) / sx;
    s_guardY0 = (hh - GUARD_PX - (float)g_glViewportOffsetY) / sy;
    s_guardY1 = (hh + GUARD_PX - (float)g_glViewportOffsetY) / sy;
}

/* =====================================================================
 * Drawing in the GE's transform (gu_backend.h)
 * ===================================================================== */

unsigned int GU_FrameNo(void)
{
    return s_frameNo;
}

/* Camera space → screen: screen = centre + projScale * cam / depth (the
 * game's projection, y down), into the 4:3 area; clip w = depth, clip z / w =
 * 1 - 2 / depth — the depth the pre-transformed vertices get from rhw. */
void GU_CameraProjection(ScePspFMatrix4 *m)
{
    float sx = (float)g_glBackingWidth / (float)(g_screenWidth > 0 ? g_screenWidth : 640);
    float sy = (float)g_glBackingHeight / (float)(g_screenHeight > 0 ? g_screenHeight : 480);
    float hw = SCR_W * 0.5f, hh = SCR_H * 0.5f;

    memset(m, 0, sizeof(*m));
    m->x.x = sx * (float)g_projScaleXCurrent / hw;
    m->y.y = sy * (float)g_projScaleY / hh;
    m->z.x = ((float)g_glViewportOffsetX - hw + sx * (float)g_screenCenterX) / hw;
    m->z.y = (hh - (float)g_glViewportOffsetY - sy * (float)g_screenCenterY) / hh;
    m->z.z = 1.0f;
    m->z.w = 1.0f;
    m->w.z = -2.0f * DEPTH_NEAR;
}

void GU_TransformBegin(const ScePspFMatrix4 *view, const ScePspFMatrix4 *proj)
{
    flush_batch();
    ensure_list();
    sceGuSetMatrix(GU_VIEW, view);
    sceGuSetMatrix(GU_PROJECTION, proj);
}

#ifdef SONICR_PROF
/* (time inside the transformed draws, and its state part — FlipD3D) */
static unsigned int s_profXDrawUs, s_profXApplyUs, s_profXDraws;
#define XPROF_T0() unsigned int _xt0 = sceKernelGetSystemTimeLow()
#define XPROF_APPLY() unsigned int _xt1 = sceKernelGetSystemTimeLow(); s_profXApplyUs += _xt1 - _xt0
#define XPROF_END() s_profXDrawUs += sceKernelGetSystemTimeLow() - _xt0; s_profXDraws++
#else
#define XPROF_T0() ((void)0)
#define XPROF_APPLY() ((void)0)
#define XPROF_END() ((void)0)
#endif

/* The lightning flash's lift (GU_Tint): the GE's light with the vertex colour
 * as the ambient material and the lift as the emissive one, which add, clamped
 * at white, as the desktop path adds it to each vertex colour. */
static uint32_t s_tint;

void GU_Tint(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    s_tint = (r | g | b) ? 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r : 0;
}

static void tint_on(void)
{
    if (s_tint) {
        sceGuEnable(GU_LIGHTING);
        sceGuAmbient(0xFFFFFFFF);
        sceGuColorMaterial(GU_AMBIENT);
        sceGuModelColor(s_tint, 0xFFFFFFFF, 0xFFFFFFFF, 0);
    }
}

static void tint_off(void)
{
    if (s_tint) {
        sceGuDisable(GU_LIGHTING);
        sceGuModelColor(0, 0xFFFFFFFF, 0xFFFFFFFF, 0);
    }
}

void GU_TransformDraw(const ScePspFMatrix4 *model, int cull, int frontFace, const GuVertex *verts, int count)
{
    GU_TransformDrawV(model, cull, frontFace, GU_VTYPE, verts, count);
}

void GU_TransformDrawV(const ScePspFMatrix4 *model, int cull, int frontFace, int vtype, const void *verts, int count)
{
    if (count <= 0) {
        return;
    }
    XPROF_T0();
    apply_desired();
    XPROF_APPLY();
    if (cull) {
        sceGuFrontFace(frontFace);
        sceGuEnable(GU_CULL_FACE);
    } else {
        sceGuDisable(GU_CULL_FACE);
    }
    if (model) {
        sceGuSetMatrix(GU_MODEL, model);
    }
    tint_on();
    sceGuDrawArray(GU_TRIANGLES, vtype, count, NULL, verts);
    tint_off();
    list_drawn();
    PROF(s_profBatches++);
    PROF(s_profVerts += count);
    XPROF_END();
}

void GU_TransformDrawSkinned(const ScePspFMatrix4 *bones, int nb, int cull, int frontFace, const void *verts,
                             int count)
{
    if (count <= 0 || nb < 1 || nb > 8) {
        return;
    }
    XPROF_T0();
    apply_desired();
    XPROF_APPLY();
    if (cull) {
        sceGuFrontFace(frontFace);
        sceGuEnable(GU_CULL_FACE);
    } else {
        sceGuDisable(GU_CULL_FACE);
    }
    for (int i = 0; i < nb; i++) {
        sceGuBoneMatrix(i, &bones[i]);
    }
    tint_on();
    sceGuDrawArray(GU_TRIANGLES, GU_WEIGHTS(nb) | GU_WEIGHT_8BIT | GU_VTYPE, count, NULL, verts);
    tint_off();
    list_drawn();
    PROF(s_profBatches++);
    PROF(s_profVerts += count);
    XPROF_END();
}

GuVertex *GU_TransformAlloc(int count)
{
    if (count <= 0 || s_arenaUsed + count > ARENA_VERTS) {
        return NULL;
    }
    GuVertex *v = &s_arena[s_arenaUsed];
    s_arenaUsed += count;
    return v;
}

void GU_TransformUnalloc(int count)
{
    if (count > 0 && count <= s_arenaUsed) {
        s_arenaUsed -= count;
    }
}

void GU_TransformDrawFresh(const ScePspFMatrix4 *model, int cull, int frontFace, GuVertex *verts, int count)
{
    if (count <= 0) {
        return;
    }
    sceKernelDcacheWritebackRange(verts, count * (int)sizeof(GuVertex));
    GU_TransformDraw(model, cull, frontFace, verts, count);
}

/* The fog band on the GE. The game fades it out through vertex alpha —
 * alpha * colour + (1 - alpha) * what's behind, alpha falling linearly with
 * depth — which needed every vertex rewritten each frame. The GE's own fog
 * is that same ramp, but towards a colour, not the background; two passes
 * with it fogging to black give the blend exactly:
 *   1. a white copy (ambient light, the texture added: its alpha still cuts
 *      the colour-keyed texels), fogged to f, blended dst * (1 - src);
 *   2. the real colours, fogged to f * colour, added.
 * The GE's fog factor is (z + FOG1) * FOG2 on the view-space z, which is
 * +depth here (the game's camera space): sceGuFog(-near, -far) makes it
 * (far - depth) / (far - near). */
static float s_fogNear, s_fogFar;

/* GU_CameraProjection with the depth of everything k times as far (the
 * screen position unchanged): its near plane k times nearer in the depth
 * row. For the grid, which the game draws GRID_DEPTH_BIAS behind the track
 * (its floor runs along the ground plane in places, the Reactive Factory
 * loop): k = 1 + bias / depth for each chunk. */
void GU_CameraProjectionPushed(ScePspFMatrix4 *m, float k)
{
    GU_CameraProjection(m);
    m->w.z = -2.0f * DEPTH_NEAR / k;
}

void GU_FogRamp(float nearZ, float farZ)
{
    s_fogNear = nearZ;
    s_fogFar = farZ;
}

void GU_TransformDrawFog(const ScePspFMatrix4 *model, int cull, int frontFace, const GuVertex *verts, int count)
{
    GU_TransformDrawFogV(model, cull, frontFace, GU_VTYPE, verts, count);
}

void GU_TransformDrawFogV(const ScePspFMatrix4 *model, int cull, int frontFace, int vtype, const void *verts,
                          int count)
{
    if (count <= 0) {
        return;
    }
    XPROF_T0();
    apply_desired();
    XPROF_APPLY();
    if (cull) {
        sceGuFrontFace(frontFace);
        sceGuEnable(GU_CULL_FACE);
    } else {
        sceGuDisable(GU_CULL_FACE);
    }
    if (model) {
        sceGuSetMatrix(GU_MODEL, model);
    }
    sceGuEnable(GU_FOG);
    sceGuFog(-s_fogNear, -s_fogFar, 0);

    sceGuEnable(GU_LIGHTING);
    sceGuAmbient(0xFFFFFFFF);
    sceGuAmbientColor(0xFFFFFFFF);
    sceGuColorMaterial(0);
    sceGuDisable(GU_FRAGMENT_2X);
    sceGuTexFunc(GU_TFX_ADD, GU_TCC_RGBA);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_COLOR, 0, 0);
    sceGuDrawArray(GU_TRIANGLES, vtype, count, NULL, verts);

    sceGuDisable(GU_LIGHTING);
    if (s_current.texEnv == R_TEXENV_ADD_SIGNED) {
        sceGuEnable(GU_FRAGMENT_2X);
    }
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
    tint_on();
    sceGuDrawArray(GU_TRIANGLES, vtype, count, NULL, verts);
    tint_off();

    sceGuDisable(GU_FOG);
    s_current.blendMode = -1;   /* (the next draw sets its own again) */
    list_drawn();
    PROF(s_profBatches += 2);
    PROF(s_profVerts += count * 2);
    XPROF_END();
}

void GU_TrackTextures(int on)
{
    on = on && g_pspTrackNoMip;
    if (on != s_noMip) {
        flush_batch();   /* (the open batch was put together for the other mode) */
        s_noMip = on;
        s_boundTex = -2;
        s_appliedFilter = -1;
    }
}

void GU_TransformEnd(void)
{
    list_kick();
    ScePspFMatrix4 identity;
    memset(&identity, 0, sizeof(identity));
    identity.x.x = identity.y.y = identity.z.z = identity.w.w = 1.0f;
    sceGuSetMatrix(GU_VIEW, &identity);
    sceGuSetMatrix(GU_MODEL, &identity);
    set_projection();
    sceGuDisable(GU_CULL_FACE);
}

/* =====================================================================
 * Frame lifecycle (render_gl.c counterparts)
 * ===================================================================== */

/**
 * BeginFrame — 0x00422AA4
 * Original: IDirect3DDevice::BeginScene().
 */
void BeginFrame(void)
{
    flush_batch();
    /* render_gl.c zeroes the depth of all 32768 source vertices here (a port
     * addition against stale geometry); the DC build doesn't, and on the PSP
     * that 2MB walk — a cache miss per vertex at the 64-byte stride — cost
     * ~20ms of every frame. */
    R_InitTextures();
    ensure_list();
    set_projection();
    R_ResetState();
}

/**
 * EndFrame — 0x00422BA0
 * Original: IDirect3DDevice::EndScene(). The list runs at the flip.
 */
void EndFrame(void)
{
}

/**
 * FlipD3D — 0x004329FC
 * Original: IDirectDrawSurface::Flip(). Runs the frame's list and hands the
 * buffer to the display at the next vertical blank (the frame pacing waits
 * for the blanks: PSP_WaitForVBlank2).
 */
#ifdef SONICR_DEBUG_LOG
/* Frame profile, logged every 60 frames: CPU time from the end of the frame
 * pacing wait to the flip (game logic + building the list), the part of it
 * spent waiting for the GE to finish the list, and the frame rate. */
static unsigned int s_profFrameStart, s_profWindowStart, s_profFrames;
static unsigned int s_profCpuSum, s_profGeSum, s_profCpuMax;
#endif

void FlipD3D(void)
{
    flush_batch();
    ensure_list();
#ifdef SONICR_DEBUG_LOG
    unsigned int t0 = sceKernelGetSystemTimeLow();
#endif
    run_list();
    ARENA_HW();
    s_arenaUsed = 0;   /* (the GE is done with this frame's vertices) */
#ifdef SONICR_DEBUG_LOG
    unsigned int tSync = sceKernelGetSystemTimeLow();
#endif
    sceGuSwapBuffers();
    s_swapVcount = sceDisplayGetVcount();
    s_swapPending = 1;
    PSP_WideView(0);   /* (left open: the 4:3 clip back) */
    wide_set(0);       /* (the next race frame's 3D turns it back on) */
    s_frameNo++;
#ifdef SONICR_DEBUG_LOG
    unsigned int t1 = sceKernelGetSystemTimeLow();
    static unsigned int s_profSwapSum;
    if (s_profFrameStart) {
        unsigned int cpu = t0 - s_profFrameStart;
        s_profCpuSum += cpu;
        s_profGeSum += tSync - t0;
        s_profSwapSum += t1 - tSync;
        if (cpu > s_profCpuMax) {
            s_profCpuMax = cpu;
        }
        if (++s_profFrames == 60) {
            unsigned int span = t1 - s_profWindowStart;
            fprintf(stderr, "[%7u] frame: cpu %5.2f ms (max %5.2f), ge wait %5.2f, swap %5.2f ms, %4.1f fps | "
                            "per frame: %u draws in %u batches, %u verts, %u uploads (%.2f ms), %u list runs\n",
                    (unsigned)(t1 / 1000), s_profCpuSum / 60000.0f, s_profCpuMax / 1000.0f,
                    s_profGeSum / 60000.0f, s_profSwapSum / 60000.0f, span ? 60e6f / span : 0.0f,
                    s_profDraws / 60, s_profBatches / 60, s_profVerts / 60, s_profUploads / 60, s_profUploadUs / 60000.0f,
                    s_profListRuns / 60);
            extern unsigned int g_trackGeObjects, g_trackCpuObjects;   /* render_track_psp.c */
            extern unsigned int g_trackCpuNear, g_trackCpuGuard, g_trackCpuFog;
            if (g_trackGeObjects + g_trackCpuObjects) {
                fprintf(stderr, "    track per frame: chunks %u as built + %u a triangle at a time (fog alpha, camera), "
                                "%u triangles clipped on the CPU, %u objects from current data, desktop path %u\n",
                        g_trackGeObjects / 60, g_trackCpuFog / 60, g_trackCpuNear / 60, g_trackCpuGuard / 60,
                        g_trackCpuObjects / 60);
                extern unsigned int g_trackObjSeen, g_trackObjWhole, g_trackDecoDrawn, g_trackRunsDrawn;
                fprintf(stderr, "    track objects per frame: %u on the screen, %u queued whole, %u decorations; %u runs "
                                "drawn\n",
                        g_trackObjSeen / 60, g_trackObjWhole / 60, g_trackDecoDrawn / 60, g_trackRunsDrawn / 60);
                g_trackObjSeen = g_trackObjWhole = g_trackDecoDrawn = g_trackRunsDrawn = 0;
            }
            extern unsigned int g_trackUsPass1, g_trackUsFog, g_trackUsNear;
            extern unsigned int g_trackPolyUpdates, g_trackObjRebuilds, g_trackUsRebuild;
            if (g_trackUsPass1) {
                fprintf(stderr, "    track ms per frame: GE pass %.2f (triangle-at-a-time chunks %.2f of it), "
                                "CPU-clipped %.2f, animations %.2f (%u polygons updated, %u objects rebuilt)\n",
                        g_trackUsPass1 / 60000.0f, g_trackUsFog / 60000.0f, g_trackUsNear / 60000.0f,
                        g_trackUsRebuild / 60000.0f, g_trackPolyUpdates / 60, g_trackObjRebuilds / 60);
            }
            g_trackPolyUpdates = g_trackObjRebuilds = g_trackUsRebuild = 0;
            extern unsigned int g_trackUsDesktop, g_trackUsCheck, g_trackUsDrawObj;
            fprintf(stderr, "    track's desktop pass (ring sub-entries) %.2f ms, safety net %.2f ms, objects' draws %.2f ms\n",
                    g_trackUsDesktop / 60000.0f, g_trackUsCheck / 60000.0f, g_trackUsDrawObj / 60000.0f);
            g_trackUsDesktop = g_trackUsCheck = g_trackUsDrawObj = 0;
            {
                extern void PSP_HeapKB(int *inUse, int *grown);   /* platform_psp.c */
                int heapUsed, heapGrown;
                PSP_HeapKB(&heapUsed, &heapGrown);
                extern int g_pspCharHw, g_pspSkinDynHw;   /* render_character_d3d.c */
                fprintf(stderr, "    heap %d KB in use (grown to %d KB); most held: list %u of %d KB, arena %u of %d "
                                "vertices, characters %d of 6144, skinned faces %d words; vertices %d, track's %d\n",
                        heapUsed, heapGrown, s_hwList / 1024, LIST_SIZE / 1024, s_hwArena, ARENA_VERTS, g_pspCharHw,
                        g_pspSkinDynHw, g_modelVertexCount, g_trackVertexCount);
                /* (what of it textures are: the game's 16-bit pixels, the GE's copies in main RAM) */
                int srcPages = 0, gePages = 0, geKB = 0, rgbaKB = 0;
                for (int tp = 0; tp < NUM_TPAGES; tp++) {
                    srcPages += g_tpagePixelBuf[tp] != NULL;
                    if (s_tex[tp].data && !s_tex[tp].vram) {
                        gePages++;
                        geKB += s_tex[tp].bytes / 1024;
                    }
                    rgbaKB += (s_rgba8Buf[tp] ? 1 : 0) * (g_tpageWidth[tp] ? g_tpageWidth[tp] : 256) *
                              (g_tpageHeight[tp] ? g_tpageHeight[tp] : 256) * 4 / 1024;
                }
                fprintf(stderr, "    textures: %d pages' pixels (~%d KB), %d GE copies in main RAM %d KB, RGBA8 %d KB\n",
                        srcPages, srcPages * 128, gePages, geKB, rgbaKB);
            }
#ifdef SONICR_PROF
            extern unsigned int g_charProfUs[14];
            fprintf(stderr, "    chars ms per frame: shadows %.2f, sprites/items %.2f, models %.2f (vertices %.2f, "
                            "faces %.2f), item effects %.2f\n",
                    g_charProfUs[0] / 60000.0f, g_charProfUs[1] / 60000.0f, g_charProfUs[2] / 60000.0f,
                    g_charProfUs[3] / 60000.0f, g_charProfUs[4] / 60000.0f, g_charProfUs[5] / 60000.0f);
            fprintf(stderr, "    chars' models: setup %.2f, GE batches sent %.2f, Tails's tail %.2f ms\n",
                    g_charProfUs[6] / 60000.0f, g_charProfUs[7] / 60000.0f, g_charProfUs[8] / 60000.0f);
            fprintf(stderr, "    (of them: vertex lighting %.2f, limb matrices %.2f, GE model draws %.2f, "
                            "colours rewritten %.2f ms, %.2f times)\n",
                    g_charProfUs[9] / 60000.0f, g_charProfUs[10] / 60000.0f, g_charProfUs[11] / 60000.0f,
                    g_charProfUs[12] / 60000.0f, g_charProfUs[13] / 60000.0f);
            memset(g_charProfUs, 0, sizeof(g_charProfUs));
#endif
            extern unsigned int g_gridAsBuilt, g_gridFog, g_gridTri, g_gridCpuTris;   /* render_grid_psp.c */
            if (g_gridAsBuilt + g_gridFog + g_gridTri) {
                fprintf(stderr, "    grid per frame: chunks %u as built + %u by the GE's fog + %u a triangle at a time, "
                                "%u triangles clipped on the CPU\n",
                        g_gridAsBuilt / 60, g_gridFog / 60, g_gridTri / 60, g_gridCpuTris / 60);
            }
            g_gridAsBuilt = g_gridFog = g_gridTri = g_gridCpuTris = 0;
#ifdef SONICR_PROF
            if (s_profXDraws) {
                fprintf(stderr, "    transformed draws per frame: %u, %.2f ms (state %.2f of it)\n",
                        s_profXDraws / 60, s_profXDrawUs / 60000.0f, s_profXApplyUs / 60000.0f);
            }
            s_profXDraws = s_profXDrawUs = s_profXApplyUs = 0;
#endif
            g_trackUsPass1 = g_trackUsFog = g_trackUsNear = 0;
            g_trackGeObjects = g_trackCpuObjects = 0;
            g_trackCpuNear = g_trackCpuGuard = g_trackCpuFog = 0;
#ifdef SONICR_PROF
            extern void PSP_ProfReport(unsigned int frames, unsigned int cpuUs);
            if (g_pspSkipDraws) {
                fprintf(stderr, "    (draws not sent to the GE)\n");
            }
            if (g_pspNoTextures) {
                fprintf(stderr, "    (drawn without textures)\n");
            }
            PSP_ProfReport(60, s_profCpuSum);
#endif
            s_profFrames = s_profCpuSum = s_profGeSum = s_profCpuMax = s_profSwapSum = 0;
            s_profDraws = s_profVerts = s_profUploads = s_profUploadUs = s_profListRuns = s_profBatches = 0;
            s_profWindowStart = t1;
        }
    } else {
        s_profWindowStart = t1;
    }
    s_profFrameStart = t1;
#endif
}

/* 30 Hz pacing, as the DC's DC_WaitForVBlank2: two vertical blanks per frame. */
static unsigned int s_lastVcount;   /* the blank the frame started at */

void PSP_WaitForVBlank2(void)
{
    unsigned int target = s_lastVcount + 2;
    while ((int)(sceDisplayGetVcount() - target) < 0) {
        sceDisplayWaitVblankStart();
    }
    s_lastVcount = sceDisplayGetVcount();
#ifdef SONICR_DEBUG_LOG
    s_profFrameStart = sceKernelGetSystemTimeLow();   /* the frame's work starts now */
#endif
}

/* 60 Hz in a race (interp_psp.c): the halfway frame swapped in, the first
 * blank of the tick's two passed — it is on the screen, and the buffer the
 * tick's own frame is drawn into no longer is. */
void PSP_WaitForVBlank1(void)
{
    unsigned int target = s_lastVcount + 1;
    while ((int)(sceDisplayGetVcount() - target) < 0) {
        sceDisplayWaitVblankStart();
    }
#ifdef SONICR_DEBUG_LOG
    s_profFrameStart = sceKernelGetSystemTimeLow();
#endif
}

/* Select+L+R (platform_psp.c): the frame on screen as shotNNN.bmp next to the
 * EBOOT — under PSPLink, straight into the PC's host0 folder. */
void PSP_SaveScreenshot(void)
{
    void *top = NULL;
    int bufWidth = 0, fmt = 0;
    sceDisplayGetFrameBuf(&top, &bufWidth, &fmt, PSP_DISPLAY_SETBUF_IMMEDIATE);
    if (top == NULL || fmt != PSP_DISPLAY_PIXEL_FORMAT_565) {
        return;
    }
    const unsigned short *fb = (const unsigned short *)((uintptr_t)top | 0x40000000u);   /* uncached */

    static int s_shotNo;
    char name[32];
    FILE *f = NULL;
    for (; s_shotNo < 1000; s_shotNo++) {
        snprintf(name, sizeof(name), "shot%03d.bmp", s_shotNo);
        FILE *probe = fopen(name, "rb");
        if (probe) {
            fclose(probe);
            continue;
        }
        f = fopen(name, "wb");
        break;   /* (a failed one keeps its number: the next press tries it again) */
    }
    if (f == NULL) {
        DebugLog("screenshot: cannot write %s (errno %d)\n", name, errno);
        return;
    }
    s_shotNo++;

    unsigned char hdr[54] = { 'B', 'M' };
    unsigned int dataSize = SCR_W * SCR_H * 3, fileSize = 54 + dataSize;
    memcpy(hdr + 2, &fileSize, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    unsigned int w = SCR_W, h = SCR_H;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &dataSize, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    static unsigned char row[SCR_W * 3];
    for (int y = SCR_H - 1; y >= 0; y--) {
        const unsigned short *src = fb + y * bufWidth;
        for (int x = 0; x < SCR_W; x++) {
            unsigned p = src[x];
            unsigned r = p & 0x1F, g = (p >> 5) & 0x3F, b = p >> 11;
            row[x * 3 + 0] = (unsigned char)((b << 3) | (b >> 2));
            row[x * 3 + 1] = (unsigned char)((g << 2) | (g >> 4));
            row[x * 3 + 2] = (unsigned char)((r << 3) | (r >> 2));
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
    DebugLog("screenshot: %s\n", name);
}

/**
 * SetViewportFromConfig — FUN_004cc0e8 — 429 bytes
 * Copies viewport config array entry to active rendering globals. Same as
 * render_gl.c: the per-viewport rect only clips (scissor, set by the render
 * loop), the vertices are already in screen space.
 */
/* Widescreen. The game's 640x480 space is drawn at the PSP's 272/480 scale,
 * centred: 4:3 in the middle of the 480-pixel line, the sides black. In a
 * one-player race the 3D view runs on into them instead: around its 3D draws
 * (hud_full.c, PSP_WideView) the viewport's clip rectangle — what the culls,
 * the sky and the water reach to — is widened to the whole line, the
 * projection (centre, scale) untouched: a wider field of view. The 2D
 * between and after them (the HUD places itself from the clip rectangle)
 * gets the 4:3 one back, where it was. From then to the flip nothing is cut
 * back to 4:3 (the scissor "off" area); menus and split screen stay 4:3. */
int g_pspWidescreen = 1;

static void wide_set(int on)
{
    if (on != s_wide) {
        s_wide = on;
        s_current.scissorEnabled = -1;   /* the "off" area changed */
    }
}

static int s_wideClip, s_savedClipLeft, s_savedClipRight;

void PSP_WideView(int on)
{
    if (on) {
        if (s_wideClip || !g_pspWidescreen || g_numHumans > 1 || g_clipLeft > 0 ||
            g_clipRight < g_screenWidth - 1) {
            return;
        }
        float scale = (float)g_glBackingWidth / (float)(g_screenWidth > 0 ? g_screenWidth : 640);
        s_savedClipLeft = g_clipLeft;
        s_savedClipRight = g_clipRight;
        g_clipLeft = (int)floorf(-(float)g_glViewportOffsetX / scale);
        g_clipRight = (int)ceilf((float)(SCR_W - g_glViewportOffsetX) / scale) - 1;
        g_clipLeftDouble = g_clipLeft * 2;
        s_wideClip = 1;
        wide_set(1);
    } else if (s_wideClip) {
        g_clipLeft = s_savedClipLeft;
        g_clipRight = s_savedClipRight;
        g_clipLeftDouble = g_clipLeft * 2;
        s_wideClip = 0;
    }
}

void SetViewportFromConfig(int *config)
{
    if (config == NULL) {
        return;
    }

    g_clipLeft = config[0];
    g_clipLeftDouble = g_clipLeft * 2;
    g_clipTop = config[1];
    g_clipRight = config[2];
    g_clipBottom = config[3];
    g_projScaleX = config[4];
    g_projScaleXCurrent = config[5];
    g_projScaleY = config[6];
    g_screenCenterX = config[7];
    g_screenCenterY = config[8];
    g_screenWidthFull = config[9];
    g_screenHeightFull = config[0xa];
    g_vpClipLeft10 = config[0xb];
    g_vpClipRight10 = config[0xc];
    g_vpClipLeft16 = config[0xd];
    g_vpClipRight16 = config[0xe];
    g_vpParam0F = config[0xf];
    g_vpParam10 = config[0x10];
}

/**
 * ProcessTpageStates — 0x004323cc — 574 bytes
 * Tpage state machine processor; see render_gl.c for the states. Like the GL
 * port it also clears the screen and depth, which its callers rely on.
 */
void ProcessTpageStates(void)                                   /* 0x4323cc */
{
    int changed;

    /* Clear the whole screen (bars included) to black, then confine later
     * clears to the 4:3 area. */
    R_DisableScissor();
    R_SetDepthWrite(1);
    clear_buffers(1, 1);
    R_SetScissor(g_glViewportOffsetX, g_glViewportOffsetY,
                 g_glBackingWidth, g_glBackingHeight);
    R_FlushState();

    do {
        changed = 0;
        for (int i = 0; i < 0x34; i++) {
            unsigned char state = (unsigned char)g_tpageStateArray[i];
            if (state > 7) {
                continue;
            }

            switch (state) {
                case 6:
                case 7:
                    if (state == 6) {
                        if (g_tpagePixelBuf[i] != NULL) {
                            free(g_tpagePixelBuf[i]);
                            g_tpagePixelBuf[i] = NULL;
                        }
                        s_srcFreed[i] = 0;   /* (let go: not textured until loaded again) */
                        s_texDirty[i] = 1;
                        if (s_pendingRGBA[i] != NULL) {
                            free(s_pendingRGBA[i]);
                            s_pendingRGBA[i] = NULL;
                        }
                        s_frozen[i] = 0;
                        free(s_frozenSrc[i]);
                        s_frozenSrc[i] = NULL;
                        s_frozenSize[i] = 0;
                    }
                    g_tpageStateArray[i] = 0;
                    break;

                case 1:
                case 2:
                    g_tpageStateArray[i] = 3;
                    changed = 1;
                    break;

                case 3:
                    g_tpageStateArray[i] = 4;
                    changed = 1;
                    break;

                case 5:
                    g_tpageStateArray[i] = 4;
                    break;

                case 0:
                case 4:
                default:
                    break;
            }
        }
    } while (changed);
}

/**
 * CleanupD3DTPages — 0x004332AC — 59 bytes
 * See render_gl.c: the state-setting loop is D3D-only; ProcessTpageStates runs.
 */
void CleanupD3DTPages(void)                                /* 0x4332ac */
{
    for (int i = 0; i < 0x34; i++) {
        char state = g_tpageStateArray[i];
        if (state == 0 || state == 5 || i == TPAGE_PLATFORM_ICONS) {
            continue;
        }
        g_tpageStateArray[i] = 6;
    }
    ProcessTpageStates();
}

/**
 * RenderBackground — 0x00435868 — 189 bytes
 * Binary does IDirect3DViewport2::Clear (solid colour fill): black, confined
 * to the 4:3 area.
 */
void RenderBackground(void)
{
    R_SetScissor(g_glViewportOffsetX, g_glViewportOffsetY,
                 g_glBackingWidth, g_glBackingHeight);
    clear_buffers(1, 0);
    R_DisableScissor();
    R_FlushState();
}

/**
 * RenderWavingMenuBackground — 0x004C68B8 — 664 bytes
 * The native-software twin of the menu wallpaper; see render_gl.c.
 */
void RenderWavingMenuBackground(void)
{
    int tpage = g_uiTexPage;                                /* 0x8F6C48 */

    if (tpage < 0 || tpage >= NUM_TPAGES) {
        return;
    }
    if (g_tpageStateArray[tpage] != 4) {
        return;
    }
    if (g_tpagePixelBuf[tpage] == NULL) {
        return;
    }

    struct IntVert { int sx, sy, uvU, uvV, depth; };
    struct IntVert buf[8][8];

    int sinAngle = (g_totalFrames & 0x7F) << 5;             /* 0x4C68C9 */
    int yWorld   = 0x483;                                   /* 0x4C68C4 */
    int uvV      = 0x8000;                                  /* 0x4C68CE */

    for (int row = 0; row < 8; row++) {
        int vertAngle = sinAngle;                           /* 0x4C6965 */
        int xWorld    = -0x604;                             /* 0x4C6981 */
        int uvU       = 0x8000;                             /* 0x4C6960 */

        for (int col = 0; col < 8; col++) {
            /* 0x4C6996: depth = 0x8CA - (sin >> 7) */
            int depth = 0x8CA - (g_sinTable[vertAngle] >> 7);

            /* 0x4C698D: centerX + projScaleX * xWorld / depth */
            buf[row][col].sx = g_screenCenterX + (g_projScaleXCurrent * xWorld) / depth;
            /* 0x4C69B8: centerY - projScaleY * yWorld / depth */
            buf[row][col].sy = g_screenCenterY - (g_projScaleY * yWorld) / depth;
            buf[row][col].uvU = uvU;
            buf[row][col].uvV = uvV;
            buf[row][col].depth = depth;

            vertAngle = (vertAngle - 0x14D) & 0xFFF;        /* 0x4C69D0 */
            xWorld += 0x1B8;                                /* 0x4C69F1 */
            uvU += 0x246DB6;                                /* 0x4C69E9 */
        }

        sinAngle = (sinAngle - 0xDE) & 0xFFF;               /* 0x4C6935 */
        yWorld -= 0x14A;                                    /* 0x4C6928 */
        uvV += 0x246DB6;                                    /* 0x4C690E */
    }

    R_PushState();
    R_SetTexture(tpage);
    R_SetTexEnv(R_TEXENV_MODULATE);
    R_SetBlendMode(R_BLEND_ALPHA);
    R_FlushState();

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 7; col++) {
            /* 0x4C6A5C: quad corners [row][col], [row][col+1],
             * [row+1][col+1], [row+1][col] */
            const struct IntVert *src[4] = {
                &buf[row][col],
                &buf[row][col + 1],
                &buf[row + 1][col + 1],
                &buf[row + 1][col]
            };

            /* Depth: the vertex's own (~2250), as the DC build does. The GL
             * build's rhw 1 / sz 0.5 only sorts behind the menu through sz; the
             * depth here is 1/w (rhw), where rhw 1 would be the nearest thing
             * on screen and hide every menu item drawn after it. */
            RenderVertex rv[4];
            float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
            for (int k = 0; k < 4; k++) {
                rv[k].sx = (float)src[k]->sx;
                rv[k].sy = (float)src[k]->sy;
                rv[k].sz = (float)src[k]->depth / farSafe;
                rv[k].rhw = 1.0f / (float)src[k]->depth;
                rv[k].color = VERTEX_WHITE;    /* unlit span */
                rv[k].specular = 0;
                rv[k].u = g_uvLUT256[src[k]->uvU >> 16];
                rv[k].v = g_uvLUT256[src[k]->uvV >> 16];
            }
            R_DrawQuad(rv);
        }
    }

    R_PopState();
}

/**
 * FinalizeMenuTexturesD3D — 0x00438D10 — 787 bytes
 * Original: recreates D3D texture surfaces for all loaded tpages. Here: mark
 * them dirty so they get re-uploaded on next use.
 */
void FinalizeMenuTexturesD3D(void)
{
    flush_batch();
    for (int i = 0; i < NUM_TPAGES; i++) {
        if (g_tpagePixelBuf[i] != NULL) {
            s_texDirty[i] = 1;
        }
    }
}

/* =====================================================================
 * R_* frame wrappers
 * ===================================================================== */

void R_EndFrame(void)
{
    EndFrame();
}

void R_Flip(void)
{
    FlipD3D();
}

void R_ClearAndReset(void)
{
    ProcessTpageStates();
}
