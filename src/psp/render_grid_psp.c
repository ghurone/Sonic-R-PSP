/**
 * render_grid_psp.c — PSP RenderPlayfieldGridD3D (0x0045E1BC): the playfield
 * tile grid (the ground and the sea under every track but Radiant Emerald)
 * drawn by the GE's own transform; the desktop translation
 * (sdl/src/render_grid_d3d.c, included here unchanged as
 * RenderPlayfieldGridD3D_CPU) until it is built. A lightning flash tints it
 * as the desktop path does: the lift added to its base colour (GU_Tint), the
 * texture added instead of modulated (Add Signed).
 *
 * The desktop path walks all 128x128 cells of the tile map every frame and
 * projects every visible tile on the CPU — ~6-7ms of a race frame on the
 * PSP. The grid is a flat plane (y = 0) whose tiles never change during a
 * race, so here they are built once into a GE vertex buffer, in tile units —
 * (column, 0, row), the grid's corner and step a model matrix, since
 * ComputeParallaxAndPlayfieldState recomputes those every frame — and cut
 * into chunks of 8x8 tiles, ordered by texture page so neighbouring chunks
 * draw as one.
 *
 * Per frame, as the track's chunks (render_track_psp.c): a chunk wholly in
 * the opaque part of the fog ramp, clear of the near plane and inside the
 * guard band goes as built; any other a triangle at a time — fog alpha per
 * vertex, and the triangles at the near plane clipped on the CPU.
 *
 * The same picture as the desktop path: its tile test (the track's colour
 * table), texture page and UVs per tile, base colour, the fog ramp on the
 * depth biased by GRID_DEPTH_BIAS against the far plane plus GRID_FAR_OFFSET,
 * the far cull, its depth bias (RenderPlayfieldGridD3D below). A quad culled
 * for a vertex past the far plane is culled per triangle here, at alpha 0
 * either way.
 */

#define RenderPlayfieldGridD3D RenderPlayfieldGridD3D_CPU
#include "render_grid_d3d.c"
#undef RenderPlayfieldGridD3D

#include <pspkernel.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "gu_backend.h"

#define GRID_CELLS     128
#define GRID_BLOCK     8          /* tiles a chunk side */
#define GRID_Z_MIN     33.0f      /* (past render_gu.c DEPTH_3D: the GE near plane clear) */
/* How far from the screen centre, in the game's 640x480 pixels, a vertex may
 * project: the GE's guard band is 2048 PSP pixels (3600 game ones). */
#define GRID_GUARD     3000.0f
#define GRID_COLOR     0xE0       /* the desktop path's base colour, each channel */

typedef struct {
    int first, count;             /* in s_gverts */
    unsigned char page;           /* texture page past g_tpageUIAlt */
    float x, z, r;                /* bounding circle, tile units (y = 0) */
} GridChunk;

/* A tile corner as the GE reads it in 16 bits — the corners are whole tile
 * units, the UVs fractions — 16 bytes where a GuVertex is 24: Radical City's
 * grid was 2.2 MB of a PSP-1000's 24. The GE takes both as value / 32768
 * (GRID_UNIT): the model matrix scales by it back. */
typedef struct {
    unsigned short u, v;
    uint32_t color;
    short x, y, z, pad;
} GridVertex;

#define GRID_VTYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_3D)
#define GRID_UNIT  32768.0f

static GridVertex *s_gverts;
/* Where they are built: a block of its own for good, not the heap's — a race's
 * loading leaves the heap in pieces (its files' buffers, the pages' sources
 * let go), and a 1.5 MB block in one piece wasn't there on a PSP-1000. */
static GridVertex __attribute__((aligned(16))) s_gpool[GRID_CELLS * GRID_CELLS * 6];
static GridChunk *s_gchunks;
static int s_gchunkCount, s_gvertCount;
static int s_gbuilt, s_gtrack, s_gfailed;
static unsigned int s_glastFrame;

static void grid_free(void);

/* (as render_track_psp.c's PSP_TrackFreeGE) */
void PSP_GridFreeGE(void)
{
    grid_free();
    GU_VramReset(GU_VRAM_GRID);   /* (a track with no grid — Radiant Emerald — wants none of the last one's pages) */
}

static void grid_free(void)
{
    if (s_gverts != s_gpool) {
        free(s_gverts);
    }
    free(s_gchunks);
    s_gverts = NULL;
    s_gchunks = NULL;
    s_gchunkCount = s_gvertCount = 0;
    s_gbuilt = 0;
}

/* One tile's quad, (0,1,2) and (0,2,3) as the desktop path's fan: corners
 * (c, r), (c+1, r), (c+1, r+1), (c, r+1) with the tile's UVs. */
static void grid_emit_tile(GridVertex *o, int c, int r, const float *uv)
{
    static const int corner[6] = { 0, 1, 2, 0, 2, 3 };
    const short cx[4] = { (short)c, (short)(c + 1), (short)(c + 1), (short)c };
    const short rz[4] = { (short)r, (short)r, (short)(r + 1), (short)(r + 1) };
    for (int i = 0; i < 6; i++) {
        int k = corner[i];
        o[i].u = (unsigned short)(uv[k * 2] * GRID_UNIT + 0.5f);
        o[i].v = (unsigned short)(uv[k * 2 + 1] * GRID_UNIT + 0.5f);
        o[i].color = 0xFF000000u | (GRID_COLOR << 16) | (GRID_COLOR << 8) | GRID_COLOR;
        o[i].x = cx[k];
        o[i].y = 0;
        o[i].z = rz[k];
        o[i].pad = 0;
    }
}

/* The tiles, built on the first grid frame after a gap (the menus and
 * loading between races) or a change of track. */
static int grid_ensure_built(void)
{
    unsigned int frame = GU_FrameNo();
    int gap = frame - s_glastFrame > 2;
    s_glastFrame = frame;
    if ((s_gbuilt || s_gfailed) && !gap && s_gtrack == g_trackId) {
        return s_gbuilt;
    }
    grid_free();
    s_gfailed = 0;
    s_gtrack = g_trackId;
    if (g_trackId < 1) {
        return 0;
    }
    GU_TexturesSettle();   /* (the pages made before this allocates: render_gu.c) */
    unsigned int t0 = sceKernelGetSystemTimeLow();
    const uint8_t *shown = &g_gridColorTable[(g_trackId - 1) * 120];

    int tiles = 0;
    for (int r = 0; r < GRID_CELLS; r++) {
        for (int c = 0; c < GRID_CELLS; c++) {
            tiles += shown[g_tileMap[r * 256 + c]] != 0;
        }
    }
    int maxChunks = 4 * (GRID_CELLS / GRID_BLOCK) * (GRID_CELLS / GRID_BLOCK);
    s_gverts = s_gpool;   /* (every grid fits: GRID_CELLS^2 tiles) */
    s_gchunks = (GridChunk *)malloc((size_t)maxChunks * sizeof(GridChunk));
    if (!s_gverts || !s_gchunks) {
        DebugLog("render_grid_psp: no memory for %d tiles\n", tiles);
        grid_free();
        s_gfailed = 1;   /* (not tried again every frame, until the next load) */
        return 0;
    }

    /* page by page (a tile's page is its value >> 6), block by block */
    int w = 0;
    for (int page = 0; page < 4; page++) {
        for (int br = 0; br < GRID_CELLS; br += GRID_BLOCK) {
            for (int bc = 0; bc < GRID_CELLS; bc += GRID_BLOCK) {
                int start = w;
                int lo[2] = { 1 << 30, 1 << 30 }, hi[2] = { -1, -1 };
                for (int r = br; r < br + GRID_BLOCK; r++) {
                    for (int c = bc; c < bc + GRID_BLOCK; c++) {
                        unsigned int tile = g_tileMap[r * 256 + c];
                        if (!shown[tile] || (int)(tile >> 6) != page) {
                            continue;
                        }
                        grid_emit_tile(&s_gverts[w], c, r, &g_gridUVBuffer[tile * 8]);
                        w += 6;
                        if (c < lo[0]) lo[0] = c;
                        if (r < lo[1]) lo[1] = r;
                        if (c + 1 > hi[0]) hi[0] = c + 1;
                        if (r + 1 > hi[1]) hi[1] = r + 1;
                    }
                }
                if (w > start) {
                    GridChunk *ch = &s_gchunks[s_gchunkCount++];
                    ch->first = start;
                    ch->count = w - start;
                    ch->page = (unsigned char)page;
                    ch->x = (lo[0] + hi[0]) * 0.5f;
                    ch->z = (lo[1] + hi[1]) * 0.5f;
                    float ex = (hi[0] - lo[0]) * 0.5f, ez = (hi[1] - lo[1]) * 0.5f;
                    ch->r = sqrtf(ex * ex + ez * ez);
                }
            }
        }
    }
    s_gvertCount = w;
    sceKernelDcacheWritebackRange(s_gverts, (unsigned)w * sizeof(GridVertex));
    s_gbuilt = 1;
    /* (its pages for VRAM, by the vertices on each — render_gu.c) */
    GU_VramReset(GU_VRAM_GRID);
    for (int c = 0; c < s_gchunkCount; c++) {
        GU_VramWant(GU_VRAM_GRID, g_tpageUIAlt + s_gchunks[c].page, (unsigned)s_gchunks[c].count);
    }
    GU_VramCommit();
    DebugLog("render_grid_psp: track %d grid built for the GE: %d tiles in %d chunks (%d KB), %u ms\n",
             g_trackId, tiles, s_gchunkCount, (int)(w * sizeof(GridVertex) / 1024),
             (sceKernelGetSystemTimeLow() - t0) / 1000);
    return 1;
}

/* Tile units to camera space: rows camX, camY, depth */
typedef struct {
    float m[3][4];
} GridAffine;

static inline void grid_xform(const GridAffine *t, float x, float z, float *cx, float *cy, float *cz)
{
    *cx = t->m[0][0] * x + t->m[0][2] * z + t->m[0][3];
    *cy = t->m[1][0] * x + t->m[1][2] * z + t->m[1][3];
    *cz = t->m[2][0] * x + t->m[2][2] * z + t->m[2][3];
}

/* The fog ramp on the biased depth, (GRID_FOG_FAR - (z + bias) / maxZ) *
 * GRID_FOG_SCALE, as a distance from the depth where it reaches 0 */
typedef struct {
    float nearZ, farZ, scale;
} GridRamp;

static inline uint32_t grid_alpha24(float z, const GridRamp *f)
{
    if (z <= f->nearZ) {
        return 0xFF000000u;
    }
    if (z > f->farZ) {
        return 0;
    }
    return (uint32_t)(int)((f->farZ - z) * f->scale) << 24;
}

/* The triangles the GE can't take (at the near plane, past the guard band),
 * noted while it draws, clipped and drawn pre-transformed after */
typedef struct {
    int vert;
    unsigned char page;
} GridCpuTri;

#ifdef SONICR_DEBUG_LOG
unsigned int g_gridAsBuilt, g_gridFog, g_gridTri, g_gridCpuTris;   /* reported by FlipD3D */
#endif

#define MAX_GRID_CPU_TRIS 2048
static GridCpuTri s_gcpu[MAX_GRID_CPU_TRIS];
static int s_gcpuCount;

typedef struct {
    float x, y, z, u, v;
} GridNear;

static void grid_cpu_tri(const GridAffine *t, const GridVertex *src, const GridRamp *ramp, uint32_t rgb)
{
    GridNear v[3];
    int behind = 0;
    for (int k = 0; k < 3; k++) {
        grid_xform(t, (float)src[k].x, (float)src[k].z, &v[k].x, &v[k].y, &v[k].z);
        v[k].u = (float)src[k].u * (1.0f / GRID_UNIT);
        v[k].v = (float)src[k].v * (1.0f / GRID_UNIT);
        if (v[k].z > g_farClipFloat) {
            return;
        }
        behind += v[k].z < 1.0f;
    }
    if (behind == 3) {
        return;
    }
    GridNear poly[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const GridNear *a = &v[i], *b = &v[(i + 1) % 3];
        int ina = a->z >= 1.0f, inb = b->z >= 1.0f;
        if (ina) {
            poly[n++] = *a;
        }
        if (ina != inb) {
            /* from the vertex in front, whichever way the edge is walked: the
             * neighbour sharing it lands on the very same point */
            const GridNear *p = ina ? a : b, *q = ina ? b : a;
            float s = (1.0f - p->z) / (q->z - p->z);
            GridNear *o = &poly[n++];
            o->x = p->x + (q->x - p->x) * s;
            o->y = p->y + (q->y - p->y) * s;
            o->z = 1.0f;
            o->u = p->u + (q->u - p->u) * s;
            o->v = p->v + (q->v - p->v) * s;
        }
    }
    if (n < 3) {
        return;
    }
    RenderVertex rv[4];
    for (int i = 0; i < n; i++) {
        float inv = 1.0f / poly[i].z;
        rv[i].sx = (float)g_screenCenterX + (float)g_projScaleXCurrent * poly[i].x * inv;
        rv[i].sy = (float)g_screenCenterY - (float)g_projScaleY * poly[i].y * inv;
        rv[i].sz = 0.0f;
        rv[i].rhw = 1.0f / (poly[i].z + GRID_DEPTH_BIAS);   /* (behind the track, as the game's sz; the DC's rhw) */
        rv[i].color = rgb | grid_alpha24(poly[i].z, ramp);
        rv[i].specular = 0;
        rv[i].u = poly[i].u;
        rv[i].v = poly[i].v;
    }
    R_DrawTriFan(rv, n);
}

void RenderPlayfieldGridD3D(float *baseVerts, float *upperVerts, int *vpConfig)
{
    if (!grid_ensure_built()) {
        RenderPlayfieldGridD3D_CPU(baseVerts, upperVerts, vpConfig);
        return;
    }

    /* tile units → world: the desktop path's corner and step */
    float baseX = baseVerts[0], baseZ = baseVerts[1];
    float dxStep = (float)((sr_double)(upperVerts[0] - baseX) * GRID_STEP_SCALE);
    float dzStep = (float)((sr_double)(upperVerts[1] - baseZ) * GRID_STEP_SCALE);
    ScePspFMatrix4 model, view, proj;
    memset(&model, 0, sizeof(model));
    model.x.x = dxStep * GRID_UNIT;   /* (the 16-bit corners' / 32768 undone) */
    model.y.y = GRID_UNIT;
    model.z.z = dzStep * GRID_UNIT;
    model.w.x = baseX;
    model.w.z = baseZ;
    model.w.w = 1.0f;

    /* world → camera: its float view matrix (vpConfig + 0x88, rows of 4)
     * around the float camera, the plane at y = 0 */
    const float *f = (const float *)((char *)vpConfig + 0x88);
    float camX = g_camFloatX, camY = g_camFloatY, camZ = g_camFloatZ;
    memset(&view, 0, sizeof(view));
    view.x.x = f[0]; view.x.y = f[1]; view.x.z = f[2];
    view.y.x = f[4]; view.y.y = f[5]; view.y.z = f[6];
    view.z.x = f[8]; view.z.y = f[9]; view.z.z = f[10];
    view.w.x = -(camX * f[0] + camY * f[4] + camZ * f[8]);
    view.w.y = -(camX * f[1] + camY * f[5] + camZ * f[9]);
    view.w.z = -(camX * f[2] + camY * f[6] + camZ * f[10]);
    view.w.w = 1.0f;
    GU_CameraProjection(&proj);

    /* the same, tile units to camera space, for the CPU's tests */
    GridAffine t;
    const ScePspFVector4 *vc[4] = { &view.x, &view.y, &view.z, &view.w };
    for (int row = 0; row < 3; row++) {
        float r0 = row == 0 ? vc[0]->x : row == 1 ? vc[0]->y : vc[0]->z;
        float r1 = row == 0 ? vc[1]->x : row == 1 ? vc[1]->y : vc[1]->z;
        float r2 = row == 0 ? vc[2]->x : row == 1 ? vc[2]->y : vc[2]->z;
        float r3 = row == 0 ? vc[3]->x : row == 1 ? vc[3]->y : vc[3]->z;
        t.m[row][0] = r0 * dxStep;
        t.m[row][1] = r1;
        t.m[row][2] = r2 * dzStep;
        t.m[row][3] = r0 * baseX + r2 * baseZ + r3;
    }
    float scaleMax = fabsf(dxStep) > fabsf(dzStep) ? fabsf(dxStep) : fabsf(dzStep);

    float maxZ = (float)((sr_double)g_farClipFloat + GRID_FAR_OFFSET);
    GridRamp ramp;
    ramp.nearZ = (float)GRID_FOG_NEAR * maxZ - GRID_DEPTH_BIAS;
    ramp.farZ = (float)GRID_FOG_FAR * maxZ - GRID_DEPTH_BIAS;
    ramp.scale = (float)GRID_FOG_SCALE / maxZ;
    const float farClip = g_farClipFloat;
    const float gx = GRID_GUARD / (float)g_projScaleXCurrent, gy = GRID_GUARD / (float)g_projScaleY;
    int tintR = 0, tintG = 0, tintB = 0;
    if (g_colorTintEnable) {
        tintR = g_gridTintR;
        tintG = g_gridTintG;
        tintB = g_gridTintB;
    }
    const uint32_t rgb = ((uint32_t)(GRID_COLOR + tintR > 255 ? 255 : GRID_COLOR + tintR) << 16) |
                         ((uint32_t)(GRID_COLOR + tintG > 255 ? 255 : GRID_COLOR + tintG) << 8) |
                         (uint32_t)(GRID_COLOR + tintB > 255 ? 255 : GRID_COLOR + tintB);
    /* the desktop path rejects a tile wholly off one side of the screen */
    GuFrustum fr;
    GU_FrustumInit(&fr, (float)g_projScaleXCurrent, (float)g_projScaleY, (float)g_screenCenterX,
                   (float)g_screenCenterY, (float)g_clipLeft, (float)g_clipRight, (float)g_clipTop,
                   (float)g_clipBottom);

    int fogPasses = 1;
#ifdef SONICR_PROF
    extern int g_pspFogPasses;   /* (test, Select+L+Square: the fog band a vertex at a time instead) */
    fogPasses = g_pspFogPasses;
#endif

    GU_TransformBegin(&view, &proj);
    GU_FogRamp(ramp.nearZ, ramp.farZ);
    GU_Tint(tintR, tintG, tintB);
    const ScePspFMatrix4 *pending = &model;   /* (set by the first draw) */
    R_SetTexEnv(g_colorTintEnable ? R_TEXENV_ADD_SIGNED : R_TEXENV_MODULATE);
    int runFirst = 0, runCount = 0, runPage = -1, runFog = 0, boundPage = -1;
    s_gcpuCount = 0;
    /* The game draws the grid GRID_DEPTH_BIAS behind the track (sz), and the
     * track's floor runs along the ground plane in places (the Reactive
     * Factory loop): each chunk depth-tested that much farther at its centre,
     * in proportion over it — a projection pushing depth by 1 + bias / depth.
     * One draw a chunk, then. */
    ScePspFMatrix4 pushed;
#define GRID_PUSH(z)                                                                  \
    do {                                                                              \
        float zz_ = (z) > GRID_Z_MIN ? (z) : GRID_Z_MIN;                              \
        GU_CameraProjectionPushed(&pushed, 1.0f + (float)GRID_DEPTH_BIAS / zz_);      \
        sceGuSetMatrix(GU_PROJECTION, &pushed);                                       \
    } while (0)

#define GRID_FLUSH_RUN()                                                              \
    do {                                                                              \
        if (runCount) {                                                               \
            if (boundPage != runPage) {                                               \
                R_SetTexture(g_tpageUIAlt + runPage);                                 \
                boundPage = runPage;                                                  \
            }                                                                         \
            if (runFog) {                                                             \
                GU_TransformDrawFogV(pending, 0, GU_CW, GRID_VTYPE, &s_gverts[runFirst], runCount); \
            } else {                                                                  \
                GU_TransformDrawV(pending, 0, GU_CW, GRID_VTYPE, &s_gverts[runFirst], runCount); \
            }                                                                         \
            pending = NULL;                                                           \
            runCount = 0;                                                             \
        }                                                                             \
    } while (0)

    for (int i = 0; i < s_gchunkCount; i++) {
        const GridChunk *ch = &s_gchunks[i];
        if (g_tpageStateArray[g_tpageUIAlt + ch->page] != 4) {
            continue;
        }
        float cx, cy, cz;
        grid_xform(&t, ch->x, ch->z, &cx, &cy, &cz);
        float r = ch->r * scaleMax;
        float zNear = cz - r;
        if (cz + r < 1.0f || zNear > farClip || zNear > ramp.farZ) {
            continue;   /* all behind the camera, past the far plane, or fogged out */
        }
        if (GU_FrustumOutSphere(&fr, cx, cy, cz, r)) {
            continue;   /* off the screen */
        }
        int clear = zNear >= GRID_Z_MIN && fabsf(cx) + r <= gx * zNear && fabsf(cy) + r <= gy * zNear;
        int fog = cz + r > ramp.nearZ;
        if (clear && (!fog || fogPasses)) {
#ifdef SONICR_DEBUG_LOG
            if (fog) g_gridFog++; else g_gridAsBuilt++;
#endif
            GRID_PUSH(cz);
            runFirst = ch->first;
            runCount = ch->count;
            runPage = ch->page;
            runFog = fog;
            GRID_FLUSH_RUN();
            continue;
        }

        /* a triangle at a time */
#ifdef SONICR_DEBUG_LOG
        g_gridTri++;
#endif
        GRID_FLUSH_RUN();
        GuVertex *out = GU_TransformAlloc(ch->count);
        const GridVertex *src = &s_gverts[ch->first];
        int w = 0;
        for (int k = 0; k < ch->count; k += 3) {
            float x[3], y[3], z[3];
            int behind = 0, beyond = 0, ge = 1;
            for (int j = 0; j < 3; j++) {
                grid_xform(&t, (float)src[k + j].x, (float)src[k + j].z, &x[j], &y[j], &z[j]);
                behind += z[j] < 1.0f;
                beyond += z[j] > farClip;
                ge &= z[j] >= GRID_Z_MIN && fabsf(x[j]) <= gx * z[j] && fabsf(y[j]) <= gy * z[j];
            }
            if (!beyond && behind < 3 && GU_FrustumOutTri(&fr, x, y, z)) {
                continue;   /* off the screen */
            }
            if (beyond || behind == 3) {
                continue;
            }
            if (!ge || out == NULL) {
                if (s_gcpuCount < MAX_GRID_CPU_TRIS) {
                    s_gcpu[s_gcpuCount].vert = ch->first + k;
                    s_gcpu[s_gcpuCount].page = ch->page;
                    s_gcpuCount++;
                }
                continue;
            }
            for (int j = 0; j < 3; j++) {
                /* (in floats, in the same units as the 16-bit ones: the model
                 * matrix is theirs) */
                const GridVertex *g = &src[k + j];
                out[w].u = (float)g->u * (1.0f / GRID_UNIT);
                out[w].v = (float)g->v * (1.0f / GRID_UNIT);
                out[w].color = (g->color & 0x00FFFFFFu) | grid_alpha24(z[j], &ramp);
                out[w].x = (float)g->x * (1.0f / GRID_UNIT);
                out[w].y = 0.0f;
                out[w].z = (float)g->z * (1.0f / GRID_UNIT);
                w++;
            }
        }
        if (out) {
            if (w) {
                if (boundPage != ch->page) {
                    R_SetTexture(g_tpageUIAlt + ch->page);
                    boundPage = ch->page;
                }
                GRID_PUSH(cz);
                GU_TransformDrawFresh(pending, 0, GU_CW, out, w);
                pending = NULL;
            }
            GU_TransformUnalloc(ch->count - w);
        }
    }
    GRID_FLUSH_RUN();
#undef GRID_FLUSH_RUN
#undef GRID_PUSH
    GU_Tint(0, 0, 0);
    GU_TransformEnd();

#ifdef SONICR_DEBUG_LOG
    g_gridCpuTris += s_gcpuCount;
#endif
    /* the noted triangles, clipped at the near plane */
    int page = -1;
    for (int i = 0; i < s_gcpuCount; i++) {
        if (s_gcpu[i].page != page) {
            page = s_gcpu[i].page;
            R_SetTexture(g_tpageUIAlt + page);
        }
        grid_cpu_tri(&t, &s_gverts[s_gcpu[i].vert], &ramp, rgb);
    }
}
