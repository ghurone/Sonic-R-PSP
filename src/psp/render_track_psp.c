/**
 * render_track_psp.c — PSP RenderTrackD3D (0x004533C4): the track's objects
 * drawn by the GE's own transform where that gives the same picture, the
 * desktop translation (sdl/src/render_track_sdl.c, included here unchanged as
 * RenderTrackD3D_CPU) for everything else.
 *
 * The desktop path transforms and projects every vertex of every visible
 * object on the CPU each frame (integer view matrix, divides, the float
 * re-projection, the fog ramp), then the backend converts each one again —
 * ~12ms of a race frame on the PSP. Here each object's polygons are built
 * once (after the track loads) into a GE vertex buffer in model space, and a
 * visible object costs a matrix and a draw per texture page.
 *
 * Which objects go to the GE: those the desktop path would draw with no
 * per-vertex effect the GE can't reproduce —
 *   - wholly inside the opaque part of the fog ramp (depth <= FOG_NEAR of the
 *     far plane; past it the ramp fades the polygons out through alpha, and
 *     the GE's fog blends colour, not alpha);
 *   - far enough in front of the camera that nothing reaches the near plane,
 *     and with a projected size inside the GE's guard band (it drops whole
 *     triangles that stray past it, where the CPU path clips them);
 * (Radiant Emerald's per-tick colours: emerald_recolour. A lightning flash lifts every vertex colour: GU_Tint, on the GE.)
 * Every other object — the fog band, the ones at the camera — still takes the
 * desktop path, untouched. Of the desktop pass over the objects only the
 * rings (their sub-entries) are left, drawn as it draws them (track_rings).
 *
 * Polygons the track animations change (UV swaps, the Factory reels) are
 * reported through TRACK_POLY_CHANGED and their object rebuilt on its next
 * draw.
 *
 * An object is culled on its real bounding sphere (TrackObj) against the
 * screen's edges, the far plane and the camera, in floats — the desktop
 * path's integer tests on the game's radius took a division or three an
 * object, a thousand objects a frame. The same front face as it (clockwise on
 * screen, flipped in mirror mode) unless the polygon is double-sided (flags &
 * 4). Quads go as two triangles culled each on its own, where the desktop
 * path decides the whole quad on its first — the same for every planar quad.
 */

#define RenderTrackD3D RenderTrackD3D_CPU
#include "render_track_sdl.c"
#undef RenderTrackD3D

#include <pspkernel.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "gu_backend.h"

/* Opaque part of the fog ramp: TrackClipEmitVert gives alpha 0xFF up to
 * depth / farSafe = FOG_NEAR. */
#define GE_FOG_LIMIT   ((float)FOG_NEAR)
/* Nearest an object's bounding sphere may come (camera-space depth) */
#define GE_NEAR_MIN    128
/* How far from the screen centre, in the game's 640x480 pixels, the projected
 * sphere may reach: the GE's guard band is 2048 PSP pixels (3600 game ones). */
#define GE_GUARD       3000.0f

typedef struct {
    int first, count;           /* in s_verts */
    int chunkFirst, chunkCount; /* in s_chunks */
    unsigned char tpage, cull;
} TrackGroup;

/* A group's triangles in runs of neighbours (the polygons sorted along a
 * Morton curve), each with its bounding sphere: the frame decides per chunk
 * whether it can go to the GE as built — wholly opaque, clear of the near
 * plane, inside the guard band — or needs a triangle at a time (fog alpha,
 * the camera). An object reaching into the fog band used to be copied whole. */
typedef struct {
    int first, count;           /* in s_verts */
    float x, y, z, r;           /* model space */
} TrackChunk;

#define CHUNK_TRIS 24

typedef struct {
    int groupFirst, groupCount;
    int vertCount;
    unsigned char dirty;
    /* Its real bounding sphere, from its vertices: world space for track
     * parts, around the pivot (rotation-proof) for decorations. The game's
     * own radius (the visibility word) doesn't always enclose them, and a
     * vertex at the camera on the GE drops its whole triangle. */
    float bx, by, bz, br;
    /* What it was built from (object_hash), checked a few objects a frame
     * against changes nothing reported; and set once a change moved its
     * layout (pages, flags): drawn from the current data every frame then. */
    uint32_t hash;
    unsigned char dynamic;
    unsigned char hashStale;    /* polygons updated in place since: hash taken again, not a change */
    int pending;                /* the first polygon of its TRACK_POLY_CHANGED list + 1; 0 none */
    uint32_t emeraldPhase;      /* the phase its colours were last made for + 1 (emerald_recolour); 0 none */
    unsigned char origin;       /* a track part's: s_origins (its vertices from there); 0 a decoration's */
} TrackObj;

/* A built vertex as the GE reads it, in 16 bits: 16 bytes where a GuVertex
 * is 24 — Radical City's 86022 were 2 MB of a PSP-1000's 24. The positions
 * are the source's whole numbers, from an origin (below); the UVs are
 * (i + 0.5) / 256 (TrackUV), 128 i + 64 as the GE's / 32768. Exact, both. */
typedef struct {
    unsigned short u, v;
    uint32_t color;
    short x, y, z, pad;
} TrackVertex;

#define TRACK_VTYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_3D)
#define TRACK_UNIT  32768.0f

/* The origins a track part's vertices are given from, 16 bits reaching only
 * ±32767 of one: the track's centre and the cells of 32768 around it (Regal
 * Ruin spans 141000 across, Reactive Factory 63000; the others fit the
 * centre's), or a part's own centre if it fits no cell. The GE has an
 * origin's matrix (T(origin) S(32768)) for its parts; runs of chunks join
 * only within one. Origin 0 is the decorations': their own coordinates, small. */
#define TRACK_CELL  32768
#define MAX_ORIGINS 64
typedef struct { int x, y, z; } TrackOrigin;
static TrackOrigin s_origins[MAX_ORIGINS];
static ScePspFMatrix4 s_originModel[MAX_ORIGINS];
static int s_originCount;

static TrackVertex *s_verts;
/* Where they are built: a block of its own for good, not the heap's — a race's
 * loading leaves the heap in pieces (its files' buffers, the pages' sources
 * let go), and Radical City's 2 MB in one piece wasn't there on a PSP-1000.
 * (Its 86022 vertices the most of the five; past this, the heap's.) */
#define TRACK_POOL_VERTS (88 * 1024)
static TrackVertex __attribute__((aligned(16))) s_vpool[TRACK_POOL_VERTS];
static GuVertex *s_objTemp;               /* one object's vertices, built before they go to their sections */

/* A built vertex (floats, world units) into its 16 bits from the origin */
static inline void pack_vertex(TrackVertex *o, const GuVertex *v, const TrackOrigin *org)
{
    o->u = (unsigned short)(v->u * TRACK_UNIT + 0.5f);
    o->v = (unsigned short)(v->v * TRACK_UNIT + 0.5f);
    o->color = v->color;
    o->x = (short)((int)v->x - org->x);
    o->y = (short)((int)v->y - org->y);
    o->z = (short)((int)v->z - org->z);
    o->pad = 0;
}

static void pack_vertices(TrackVertex *o, const GuVertex *v, int n, const TrackOrigin *org)
{
    for (int i = 0; i < n; i++) {
        pack_vertex(&o[i], &v[i], org);
    }
}
static TrackGroup *s_groups;
static TrackChunk *s_chunks;
static unsigned short *s_polyOrder;      /* each object's polygons in emit order (absolute indices) */
static TrackObj *s_objs;
static unsigned short *s_polyOwner;      /* polygon index → object */
static int s_objCount, s_polyCount;
static int s_built;
static int s_buildFailed;   /* (no memory for it: not tried again every frame, until the next load) */
static const void *s_builtObjs, *s_builtPolys;
static const SrcVertex *s_builtVerts;
static int s_builtTotal, s_builtTrack;
static unsigned int s_lastFrame;
extern int PSP_LargestFreeKB(void);   /* platform_psp.c */
extern void PSP_LogOpenFiles(const char *when);

#ifdef SONICR_PROF
int g_pspTrackDynamic;   /* test switch (platform_psp.c) */
#endif

#ifdef SONICR_DEBUG_LOG
unsigned int g_trackGeObjects, g_trackCpuObjects;   /* reported by FlipD3D */
unsigned int g_trackObjSeen, g_trackObjWhole, g_trackDecoDrawn, g_trackRunsDrawn;
unsigned int g_trackCpuNear, g_trackCpuGuard, g_trackCpuFog;   /* why the CPU ones weren't the GE's */
unsigned int g_trackUsPass1, g_trackUsFog, g_trackUsNear, g_trackUsDesktop;   /* where its time goes */
unsigned int g_trackUsCheck, g_trackUsDrawObj;   /* (the safety net; draw_object, of pass 1) */
#define TRACK_T0() unsigned int _t0 = sceKernelGetSystemTimeLow()
#define TRACK_ADD(acc) (acc += sceKernelGetSystemTimeLow() - _t0)
#else
#define TRACK_T0() ((void)0)
#define TRACK_ADD(acc) ((void)0)
#endif
/* the ones an object or a chunk, in the profile build only: a timer is a
 * kernel call, and hundreds a frame skewed what they measured */
#ifdef SONICR_PROF
#define TRACK_FINE_T0() TRACK_T0()
#define TRACK_FINE_ADD(acc) TRACK_ADD(acc)
#else
#define TRACK_FINE_T0() ((void)0)
#define TRACK_FINE_ADD(acc) ((void)0)
#endif

static inline int obj_vis(const int *obj)   { return *(const short *)((const char *)obj + 0x2C); }
static inline int obj_mode(const int *obj)  { return *(const short *)((const char *)obj + 0x2E); }
static inline int obj_pstart(const int *obj) { return *(const unsigned short *)((const char *)obj + 0x30); }
static inline int obj_pcount(const int *obj) { return *(const unsigned short *)((const char *)obj + 0x32); }

static inline int poly_verts(const char *pp)
{
    return (*(const unsigned char *)(pp + 0x2E) & 1) ? 4 : 3;
}

static inline int poly_key(const char *pp)
{
    int cull = (*(const unsigned char *)(pp + 0x2E) & 4) == 0;
    return *(const unsigned char *)(pp + 0x28) * 2 + cull;
}

static inline void emit(GuVertex *o, const SrcVertex *v, const int *face, int idx)
{
    int r = v->colorR >> 13, g = v->colorG >> 13, b = v->colorB >> 13;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    o->u = TrackUV(face[idx * 2]);
    o->v = TrackUV(face[idx * 2 + 1]);
    o->color = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    o->x = (float)v->posX;
    o->y = (float)v->posY;
    o->z = (float)v->posZ;
}

/* =====================================================================
 * T-junctions
 *
 * The track's pieces don't always meet corner to corner: a corner of one
 * often sits partway along an edge of its neighbour. The two edges cover the
 * same line, but rasterized as different triangles they round apart, and a
 * line of single pixels shows the sky or the sea through the floor. The
 * desktop path snapped its corners to whole pixels at 640x480, which hid most
 * of them. Here each such corner is found once, after the load, and split
 * into the edge it touches: the triangles then share that edge exactly.
 * ===================================================================== */

#define TJ_CELL_SHIFT  8        /* 256-unit cells */
#define TJ_TOL         1.0f     /* how far off the edge a corner may sit */
#define TJ_MAX_EDGE    8        /* points split into one edge */
/* Off: the floor's lines turned out to be the grid's tiles bleeding
 * (render_grid_d3d.c), not these — and a split polygon is triangulated
 * unlike a decal laid over it (the Reactive Factory floor's stains): the
 * same plane, depths rounded a step apart from triangle to triangle, the two
 * striping through each other. */
#define TJ_SPLIT       0

typedef struct {
    int poly;
    unsigned short vtx;         /* the corner (g_vertexArrayBase) */
    unsigned char edge;         /* the polygon's edge k: corners k → k + 1 */
    float t;                    /* along it */
} TPoint;

static TPoint *s_tpts;
static int *s_tFirst;           /* polygon → its first TPoint; s_polyCount + 1 */

static int tpoint_cmp(const void *pa, const void *pb)
{
    const TPoint *a = pa, *b = pb;
    if (a->poly != b->poly) return a->poly - b->poly;
    if (a->edge != b->edge) return a->edge - b->edge;
    return a->t < b->t ? -1 : a->t > b->t;
}

static inline unsigned int tj_hash(int cx, int cy, int cz, unsigned int mask)
{
    return ((unsigned int)cx * 73856093u ^ (unsigned int)cy * 19349663u ^ (unsigned int)cz * 83492791u) & mask;
}

static inline const SrcVertex *poly_vertex(const char *pp, int k)
{
    return &g_vertexArrayBase[*(const unsigned short *)(pp + 0x20 + k * 2)];
}

/* Every track part's polygon corner that lies inside some polygon's edge
 * (decorations have their own space and turn: left out). */
static void find_tjunctions(void)
{
    unsigned int t0 = sceKernelGetSystemTimeLow();
    free(s_tpts);
    free(s_tFirst);
    s_tpts = NULL;
    s_tFirst = NULL;
#if !TJ_SPLIT
    (void)t0;
    return;
#endif

    int maxV = -1;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        if (obj_mode(obj) == 2) continue;
        const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
        for (int pi = 0; pi < obj_pcount(obj); pi++) {
            const char *pp = polys + pi * 0x30;
            for (int k = 0; k < poly_verts(pp); k++) {
                int idx = *(const unsigned short *)(pp + 0x20 + k * 2);
                if (idx > maxV) maxV = idx;
            }
        }
    }
    if (maxV < 0) {
        return;
    }
    unsigned char *used = calloc((size_t)maxV + 1, 1);
    if (!used) return;
    int nUsed = 0;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        if (obj_mode(obj) == 2) continue;
        const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
        for (int pi = 0; pi < obj_pcount(obj); pi++) {
            const char *pp = polys + pi * 0x30;
            for (int k = 0; k < poly_verts(pp); k++) {
                int idx = *(const unsigned short *)(pp + 0x20 + k * 2);
                nUsed += !used[idx];
                used[idx] = 1;
            }
        }
    }

    /* the corners by cell, hashed */
    unsigned int buckets = 1;
    while (buckets < (unsigned int)nUsed * 2) buckets <<= 1;
    unsigned int mask = buckets - 1;
    int *start = calloc(buckets + 1, sizeof(int));
    int *items = malloc((size_t)nUsed * sizeof(int));
    int *cursor = malloc((size_t)buckets * sizeof(int));
    int cap = 1024, count = 0;
    TPoint *pts = malloc((size_t)cap * sizeof(TPoint));
    if (!start || !items || !cursor || !pts) {
        free(used); free(start); free(items); free(cursor); free(pts);
        return;
    }
    for (int v = 0; v <= maxV; v++) {
        if (used[v]) {
            const SrcVertex *p = &g_vertexArrayBase[v];
            start[tj_hash(p->posX >> TJ_CELL_SHIFT, p->posY >> TJ_CELL_SHIFT, p->posZ >> TJ_CELL_SHIFT, mask) + 1]++;
        }
    }
    for (unsigned int h = 0; h < buckets; h++) {
        start[h + 1] += start[h];
        cursor[h] = start[h];
    }
    for (int v = 0; v <= maxV; v++) {
        if (used[v]) {
            const SrcVertex *p = &g_vertexArrayBase[v];
            items[cursor[tj_hash(p->posX >> TJ_CELL_SHIFT, p->posY >> TJ_CELL_SHIFT, p->posZ >> TJ_CELL_SHIFT, mask)]++] = v;
        }
    }
    free(cursor);
    free(used);

    int skipped = 0;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        if (obj_mode(obj) == 2) continue;
        for (int pi = obj_pstart(obj); pi < obj_pstart(obj) + obj_pcount(obj); pi++) {
            const char *pp = (const char *)g_polygonArrayBase + pi * 0x30;
            int nv = poly_verts(pp);
            for (int e = 0; e < nv; e++) {
                const SrcVertex *a = poly_vertex(pp, e), *b = poly_vertex(pp, (e + 1) % nv);
                int ex = b->posX - a->posX, ey = b->posY - a->posY, ez = b->posZ - a->posZ;
                float len2 = (float)ex * ex + (float)ey * ey + (float)ez * ez;
                if (len2 < 4.0f) continue;
                float len = sqrtf(len2);
                int lo[3] = { a->posX < b->posX ? a->posX : b->posX, a->posY < b->posY ? a->posY : b->posY,
                              a->posZ < b->posZ ? a->posZ : b->posZ };
                int hi[3] = { a->posX > b->posX ? a->posX : b->posX, a->posY > b->posY ? a->posY : b->posY,
                              a->posZ > b->posZ ? a->posZ : b->posZ };
                for (int k = 0; k < 3; k++) {
                    lo[k] -= 2;
                    hi[k] += 2;
                }
                int c0[3], c1[3];
                for (int k = 0; k < 3; k++) {
                    c0[k] = lo[k] >> TJ_CELL_SHIFT;
                    c1[k] = hi[k] >> TJ_CELL_SHIFT;
                }
                if ((c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1) > 4096) {
                    skipped++;
                    continue;
                }
                for (int cz = c0[2]; cz <= c1[2]; cz++)
                for (int cy = c0[1]; cy <= c1[1]; cy++)
                for (int cx = c0[0]; cx <= c1[0]; cx++) {
                    unsigned int h = tj_hash(cx, cy, cz, mask);
                    for (int j = start[h]; j < start[h + 1]; j++) {
                        const SrcVertex *p = &g_vertexArrayBase[items[j]];
                        if (p->posX < lo[0] || p->posX > hi[0] || p->posY < lo[1] || p->posY > hi[1] ||
                            p->posZ < lo[2] || p->posZ > hi[2]) {
                            continue;
                        }
                        float dx = (float)(p->posX - a->posX), dy = (float)(p->posY - a->posY),
                              dz = (float)(p->posZ - a->posZ);
                        float t = (dx * ex + dy * ey + dz * ez) / len2;
                        if (t * len < 0.5f || (1.0f - t) * len < 0.5f) {
                            continue;   /* (at a corner of the edge: nothing to split) */
                        }
                        float qx = dy * ez - dz * ey, qy = dz * ex - dx * ez, qz = dx * ey - dy * ex;
                        if ((qx * qx + qy * qy + qz * qz) / len2 > TJ_TOL * TJ_TOL) {
                            continue;
                        }
                        if (count == cap) {
                            TPoint *grown = realloc(pts, (size_t)cap * 2 * sizeof(TPoint));
                            if (!grown) goto done;
                            pts = grown;
                            cap *= 2;
                        }
                        pts[count].poly = pi;
                        pts[count].vtx = (unsigned short)items[j];
                        pts[count].edge = (unsigned char)e;
                        pts[count].t = t;
                        count++;
                    }
                }
            }
        }
    }
done:
    free(start);
    free(items);
    qsort(pts, (size_t)count, sizeof(TPoint), tpoint_cmp);

    /* the same point twice (corners shared by several pieces, cells sharing a
     * bucket) once, and at most TJ_MAX_EDGE an edge */
    int kept = 0, polys = 0, run = 0;
    for (int i = 0; i < count; i++) {
        TPoint *pt = &pts[i];
        if (kept > 0 && pts[kept - 1].poly == pt->poly && pts[kept - 1].edge == pt->edge) {
            const char *pp = (const char *)g_polygonArrayBase + pt->poly * 0x30;
            const SrcVertex *a = poly_vertex(pp, pt->edge), *b = poly_vertex(pp, (pt->edge + 1) % poly_verts(pp));
            float ex = (float)(b->posX - a->posX), ey = (float)(b->posY - a->posY), ez = (float)(b->posZ - a->posZ);
            float len = sqrtf(ex * ex + ey * ey + ez * ez);
            if ((pt->t - pts[kept - 1].t) * len < 0.5f || run >= TJ_MAX_EDGE) {
                continue;
            }
            run++;
        } else {
            polys += kept == 0 || pts[kept - 1].poly != pt->poly;
            run = 1;
        }
        pts[kept++] = *pt;
    }

    s_tFirst = calloc((size_t)s_polyCount + 1, sizeof(int));
    if (!s_tFirst) {
        free(pts);
        return;
    }
    for (int i = 0; i < kept; i++) {
        s_tFirst[pts[i].poly + 1]++;
    }
    for (int p = 0; p < s_polyCount; p++) {
        s_tFirst[p + 1] += s_tFirst[p];
    }
    s_tpts = pts;

    /* (diagnostic: slivers — triangles under 1.5 units across but long,
     * which the desktop path's whole-pixel corners mostly collapsed) */
    int slivers = 0;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        if (obj_mode(obj) == 2) continue;
        for (int pi = obj_pstart(obj); pi < obj_pstart(obj) + obj_pcount(obj); pi++) {
            const char *pp = (const char *)g_polygonArrayBase + pi * 0x30;
            static const int tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
            for (int tri = 0; tri < (poly_verts(pp) == 4 ? 2 : 1); tri++) {
                const SrcVertex *a = poly_vertex(pp, tris[tri][0]), *b = poly_vertex(pp, tris[tri][1]),
                                *c = poly_vertex(pp, tris[tri][2]);
                float ux = (float)(b->posX - a->posX), uy = (float)(b->posY - a->posY), uz = (float)(b->posZ - a->posZ);
                float vx = (float)(c->posX - a->posX), vy = (float)(c->posY - a->posY), vz = (float)(c->posZ - a->posZ);
                float wx = (float)(c->posX - b->posX), wy = (float)(c->posY - b->posY), wz = (float)(c->posZ - b->posZ);
                float qx = uy * vz - uz * vy, qy = uz * vx - ux * vz, qz = ux * vy - uy * vx;
                float area2 = sqrtf(qx * qx + qy * qy + qz * qz);
                float l = sqrtf(ux * ux + uy * uy + uz * uz), l2 = sqrtf(vx * vx + vy * vy + vz * vz),
                      l3 = sqrtf(wx * wx + wy * wy + wz * wz);
                if (l2 > l) l = l2;
                if (l3 > l) l = l3;
                if (l > 32.0f && area2 / l < 1.5f) {
                    if (slivers < 8) {
                        DebugLog("render_track_psp: sliver: object %d polygon %d (%.1f long, %.2f across)\n",
                                 oi, pi, l, area2 / l);
                    }
                    slivers++;
                }
            }
        }
    }
    DebugLog("render_track_psp: %d T-junctions split into %d polygons' edges (%d edges too long to check), "
             "%d sliver triangles, %u ms\n",
             kept, polys, skipped, slivers, (sceKernelGetSystemTimeLow() - t0) / 1000);
}

/* A polygon corner, or a point split into one of its edges */
typedef struct {
    float x, y, z, u, v, r, g, b;
} PolyVert;

static inline void poly_corner(PolyVert *o, const SrcVertex *v, const int *face, int idx)
{
    int r = v->colorR >> 13, g = v->colorG >> 13, b = v->colorB >> 13;
    o->r = (float)(r > 255 ? 255 : r < 0 ? 0 : r);
    o->g = (float)(g > 255 ? 255 : g < 0 ? 0 : g);
    o->b = (float)(b > 255 ? 255 : b < 0 ? 0 : b);
    o->u = TrackUV(face[idx * 2]);
    o->v = TrackUV(face[idx * 2 + 1]);
    o->x = (float)v->posX;
    o->y = (float)v->posY;
    o->z = (float)v->posZ;
}

static inline void pv_lerp(PolyVert *o, const PolyVert *a, const PolyVert *b, float t)
{
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;
    o->u = a->u + (b->u - a->u) * t;
    o->v = a->v + (b->v - a->v) * t;
    o->r = a->r + (b->r - a->r) * t;
    o->g = a->g + (b->g - a->g) * t;
    o->b = a->b + (b->b - a->b) * t;
}

static inline void pv_emit(GuVertex *o, const PolyVert *p)
{
    o->u = p->u;
    o->v = p->v;
    o->color = 0xFF000000u | ((uint32_t)(p->b + 0.5f) << 16) | ((uint32_t)(p->g + 0.5f) << 8) |
               (uint32_t)(p->r + 0.5f);
    o->x = p->x;
    o->y = p->y;
    o->z = p->z;
}

/* Polygon pi's triangles — (0,1,2), and (0,2,3) for a quad, as the desktop
 * path splits it — with its T-junction points split into their edges: a
 * triangle with points on one edge is fanned from the corner across from
 * it, with points on more from its centre (all in its plane, its UVs and
 * colours at their own values there). Into out when not NULL; returns the
 * vertices. */
static int build_polygon(int pi, const char *pp, GuVertex *out)
{
    static const int tris[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    int nv = poly_verts(pp);
    const int *face = (const int *)pp;
    PolyVert c[4];
    for (int k = 0; k < nv; k++) {
        poly_corner(&c[k], poly_vertex(pp, k), face, k);
    }
    const TPoint *tp = NULL;
    int nt = 0;
    if (s_tFirst && pi < s_polyCount) {
        tp = &s_tpts[s_tFirst[pi]];
        nt = s_tFirst[pi + 1] - s_tFirst[pi];
    }

    int written = 0;
    for (int tri = 0; tri < (nv == 4 ? 2 : 1); tri++) {
        PolyVert ring[3 + 3 * TJ_MAX_EDGE];
        int at[4], L = 0, split = 0, splitSide = 0;
        for (int s = 0; s < 3; s++) {
            int i0 = tris[tri][s], i1 = tris[tri][(s + 1) % 3];
            at[s] = L;
            ring[L++] = c[i0];
            if (i1 != (i0 + 1) % nv) {
                continue;   /* (the quad's diagonal: inside it) */
            }
            int added = 0;
            for (int j = 0; j < nt; j++) {
                if (tp[j].edge == i0) {
                    PolyVert *o = &ring[L++];
                    const SrcVertex *sv = &g_vertexArrayBase[tp[j].vtx];
                    pv_lerp(o, &c[i0], &c[i1], tp[j].t);
                    o->x = (float)sv->posX;   /* (the neighbour's corner exactly) */
                    o->y = (float)sv->posY;
                    o->z = (float)sv->posZ;
                    added++;
                }
            }
            if (added) {
                split++;
                splitSide = s;
            }
        }
        at[3] = L;

        if (split == 0) {
            if (out) {
                for (int k = 0; k < 3; k++) {
                    pv_emit(&out[written + k], &ring[at[k]]);
                }
            }
            written += 3;
        } else if (split == 1) {
            /* the split side's run (A, points..., B), fanned from the corner opposite */
            const PolyVert *pivot = &ring[at[(splitSide + 2) % 3]];
            int first = at[splitSide];
            int len = at[splitSide + 1] - first + 1;
            for (int q = 0; q + 1 < len; q++) {
                if (out) {
                    pv_emit(&out[written + 0], pivot);
                    pv_emit(&out[written + 1], &ring[(first + q) % L]);
                    pv_emit(&out[written + 2], &ring[(first + q + 1) % L]);
                }
                written += 3;
            }
        } else {
            PolyVert centre;
            const PolyVert *a = &ring[at[0]], *b = &ring[at[1]], *d = &ring[at[2]];
            const float third = 1.0f / 3.0f;
            centre.x = (a->x + b->x + d->x) * third;
            centre.y = (a->y + b->y + d->y) * third;
            centre.z = (a->z + b->z + d->z) * third;
            centre.u = (a->u + b->u + d->u) * third;
            centre.v = (a->v + b->v + d->v) * third;
            centre.r = (a->r + b->r + d->r) * third;
            centre.g = (a->g + b->g + d->g) * third;
            centre.b = (a->b + b->b + d->b) * third;
            for (int q = 0; q < L; q++) {
                if (out) {
                    pv_emit(&out[written + 0], &centre);
                    pv_emit(&out[written + 1], &ring[q]);
                    pv_emit(&out[written + 2], &ring[(q + 1) % L]);
                }
                written += 3;
            }
        }
    }
    return written;
}

/* Each polygon's place in s_verts, noted by build_object while
 * s_recordBase >= 0 (the object's first vertex): an animation's change to
 * one (TRACK_POLY_CHANGED) is written there again, alone (apply_object_updates),
 * as long as its page, flags, split and corners are what it was built with. */
static int s_recordBase = -1;
static int *s_polyVert;                 /* first vertex in s_verts */
static unsigned short *s_polyNV;        /* vertices */
static unsigned short *s_polyKey;       /* poly_key */
static uint32_t *s_polyPos;             /* poly_pos_hash */
static unsigned char *s_polyDirty;      /* on its object's pending list */
static int *s_polyNext;                 /* the next on that list, -1 the end */

static uint32_t poly_pos_hash(const char *pp)
{
    uint32_t h = 2166136261u;
    for (int k = 0; k < poly_verts(pp); k++) {
        const SrcVertex *v = poly_vertex(pp, k);
        h = (h ^ (uint32_t)v->posX) * 16777619u;
        h = (h ^ (uint32_t)v->posY) * 16777619u;
        h = (h ^ (uint32_t)v->posZ) * 16777619u;
    }
    return h;
}

/* Object oi's polygons as triangles, grouped by (texture page, culled), in a
 * fixed order — a rebuild after an animation writes the same slots. When out
 * is NULL only the groups are counted. Returns the vertices written. */
static int build_object(int oi, GuVertex *out, TrackGroup *groups, int *groupCount,
                        TrackChunk *chunks, int *chunkCount)
{
    const int *obj = (const int *)g_objectStructArray + oi * 17;
    int pstart = obj_pstart(obj);
    int n = obj_pcount(obj);
    int written = 0, ng = 0, nc = 0;
    int keys[64];
    int nkeys = 0;

    /* the distinct keys, in order of first appearance */
    for (int j = 0; j < n; j++) {
        int k = poly_key((const char *)g_polygonArrayBase + s_polyOrder[pstart + j] * 0x30);
        int seen = 0;
        for (int i = 0; i < nkeys; i++) {
            if (keys[i] == k) { seen = 1; break; }
        }
        if (!seen && nkeys < 64) {
            keys[nkeys++] = k;
        }
    }
    for (int i = 0; i < nkeys; i++) {
        int start = written, chunkStart = written, firstChunk = nc;
        for (int j = 0; j < n; j++) {
            int pi = s_polyOrder[pstart + j];
            const char *pp = (const char *)g_polygonArrayBase + pi * 0x30;
            if (poly_key(pp) != keys[i]) {
                continue;
            }
            int nv = build_polygon(pi, pp, out ? &out[written] : NULL);
            if (out && s_recordBase >= 0) {
                s_polyVert[pi] = s_recordBase + written;
                s_polyNV[pi] = (unsigned short)nv;
                s_polyKey[pi] = (unsigned short)poly_key(pp);
                s_polyPos[pi] = poly_pos_hash(pp);
            }
            written += nv;
            if (written - chunkStart >= CHUNK_TRIS * 3) {
                if (chunks) {
                    chunks[nc].first = chunkStart;
                    chunks[nc].count = written - chunkStart;
                }
                nc++;
                chunkStart = written;
            }
        }
        if (written > chunkStart) {
            if (chunks) {
                chunks[nc].first = chunkStart;
                chunks[nc].count = written - chunkStart;
            }
            nc++;
        }
        if (groups) {
            groups[ng].first = start;
            groups[ng].count = written - start;
            groups[ng].chunkFirst = firstChunk;
            groups[ng].chunkCount = nc - firstChunk;
            groups[ng].tpage = (unsigned char)(keys[i] >> 1);
            groups[ng].cull = (unsigned char)(keys[i] & 1);
        }
        ng++;
    }
    if (groupCount) {
        *groupCount = ng;
    }
    if (chunkCount) {
        *chunkCount = nc;
    }
    return written;
}

/* Each chunk's sphere, from its built vertices (from its object's origin) */
static void chunk_bounds(TrackChunk *c)
{
    const TrackVertex *v = &s_verts[c->first];
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < c->count; i++) {
        float p[3] = { (float)v[i].x, (float)v[i].y, (float)v[i].z };
        for (int a = 0; a < 3; a++) {
            if (p[a] < lo[a]) lo[a] = p[a];
            if (p[a] > hi[a]) hi[a] = p[a];
        }
    }
    c->x = (lo[0] + hi[0]) * 0.5f;
    c->y = (lo[1] + hi[1]) * 0.5f;
    c->z = (lo[2] + hi[2]) * 0.5f;
    float r2 = 0.0f;
    for (int i = 0; i < c->count; i++) {
        float dx = (float)v[i].x - c->x, dy = (float)v[i].y - c->y, dz = (float)v[i].z - c->z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) r2 = d2;
    }
    c->r = sqrtf(r2);
}

/* Interleaved bits of a polygon's centre (64-unit cells): neighbours in
 * space mostly land next to each other in this order. */
static unsigned int *s_mortonKey;

static unsigned int morton_spread(unsigned int v)
{
    v &= 0x3FF;
    v = (v | (v << 16)) & 0x030000FF;
    v = (v | (v << 8)) & 0x0300F00F;
    v = (v | (v << 4)) & 0x030C30C3;
    v = (v | (v << 2)) & 0x09249249;
    return v;
}

static int morton_cmp(const void *pa, const void *pb)
{
    unsigned int a = s_mortonKey[*(const unsigned short *)pa], b = s_mortonKey[*(const unsigned short *)pb];
    if (a != b) return a < b ? -1 : 1;
    return (int)*(const unsigned short *)pa - (int)*(const unsigned short *)pb;
}

/* s_polyOrder: every object's polygons along the curve */
static int sort_polygons(void)
{
    s_polyOrder = malloc((size_t)(s_polyCount ? s_polyCount : 1) * sizeof(unsigned short));
    s_mortonKey = malloc((size_t)(s_polyCount ? s_polyCount : 1) * sizeof(unsigned int));
    if (!s_polyOrder || !s_mortonKey) {
        free(s_mortonKey);
        s_mortonKey = NULL;
        return -1;
    }
    for (int p = 0; p < s_polyCount; p++) {
        s_polyOrder[p] = (unsigned short)p;
        const char *pp = (const char *)g_polygonArrayBase + p * 0x30;
        int nv = poly_verts(pp);
        int sx = 0, sy = 0, sz = 0;
        for (int k = 0; k < nv; k++) {
            const SrcVertex *v = poly_vertex(pp, k);
            sx += v->posX;
            sy += v->posY;
            sz += v->posZ;
        }
        unsigned int cx = (unsigned int)((sx / nv) >> 6) + 512, cy = (unsigned int)((sy / nv) >> 6) + 512,
                     cz = (unsigned int)((sz / nv) >> 6) + 512;
        s_mortonKey[p] = morton_spread(cx) | (morton_spread(cy) << 1) | (morton_spread(cz) << 2);
    }
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        if (obj_pcount(obj) > 1) {
            qsort(&s_polyOrder[obj_pstart(obj)], (size_t)obj_pcount(obj), sizeof(unsigned short), morton_cmp);
        }
    }
    free(s_mortonKey);
    s_mortonKey = NULL;
    return 0;
}

/* FNV-1a over what the object's GE vertices come from: its polygons (UVs,
 * vertex indices, page, flags) and their vertices' positions and colours. */
static uint32_t object_hash(int oi)
{
    const int *obj = (const int *)g_objectStructArray + oi * 17;
    const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
    int n = obj_pcount(obj);
    uint32_t h = 2166136261u;
#define MIX(x) (h = (h ^ (uint32_t)(x)) * 16777619u)
    for (int pi = 0; pi < n; pi++) {
        const int *pw = (const int *)(polys + pi * 0x30);
        for (int k = 0; k < 12; k++) {
            MIX(pw[k]);
        }
        const char *pp = (const char *)pw;
        for (int k = 0; k < poly_verts(pp); k++) {
            const SrcVertex *v = &g_vertexArrayBase[*(const unsigned short *)(pp + 0x20 + k * 2)];
            MIX(v->posX);
            MIX(v->posY);
            MIX(v->posZ);
            if (g_trackId != TRACK_RADIANT_EMERALD) {   /* (its colours change every tick: emerald_recolour) */
                MIX(v->colorR);
                MIX(v->colorG);
                MIX(v->colorB);
            }
        }
    }
#undef MIX
    return h;
}

/* The sphere around every vertex the object's polygons use. */
static void object_bounds(int oi, TrackObj *to)
{
    const int *obj = (const int *)g_objectStructArray + oi * 17;
    const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
    int n = obj_pcount(obj);
    int deco = obj_mode(obj) == 2;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    float maxLocal = 0.0f;
    for (int pi = 0; pi < n; pi++) {
        const char *pp = polys + pi * 0x30;
        for (int k = 0; k < poly_verts(pp); k++) {
            const SrcVertex *v = &g_vertexArrayBase[*(const unsigned short *)(pp + 0x20 + k * 2)];
            float p[3] = { (float)v->posX, (float)v->posY, (float)v->posZ };
            for (int a = 0; a < 3; a++) {
                if (p[a] < lo[a]) lo[a] = p[a];
                if (p[a] > hi[a]) hi[a] = p[a];
            }
            float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (d > maxLocal) maxLocal = d;
        }
    }
    if (n == 0) {
        to->bx = to->by = to->bz = 0.0f;
        to->br = -1.0f;
        return;
    }
    if (deco) {
        /* local coordinates around the pivot, which moves and turns */
        to->bx = to->by = to->bz = 0.0f;
        to->br = maxLocal;
        return;
    }
    to->bx = (lo[0] + hi[0]) * 0.5f;
    to->by = (lo[1] + hi[1]) * 0.5f;
    to->bz = (lo[2] + hi[2]) * 0.5f;
    float ex = hi[0] - to->bx, ey = hi[1] - to->by, ez = hi[2] - to->bz;
    to->br = sqrtf(ex * ex + ey * ey + ez * ez);
}

/* track_rings's list of the track parts' rings */
static unsigned short *s_rings;   /* their entries in g_ringSpawnArray */
static int s_ringCount = -1;      /* -1: not listed for this track yet */
static const void *s_ringArray;

static void free_build(void);

/* Before a screen loads its pages (logo_render.c's SetupD3DTexturesBegin): the
 * last race's GE track let go first, so what loads next reuses its memory
 * rather than going on above it — the heap had grown 4.7 MB past its use. */
void PSP_TrackFreeGE(void)
{
    free_build();
}

static void free_build(void)
{
    if (s_verts != s_vpool) {
        free(s_verts);
    }
    free(s_objTemp);
    s_objTemp = NULL;
    free(s_groups);
    free(s_objs);
    free(s_polyOwner);
    free(s_tpts);
    free(s_tFirst);
    free(s_chunks);
    free(s_polyOrder);
    free(s_polyVert);
    free(s_polyNV);
    free(s_polyKey);
    free(s_polyPos);
    free(s_polyDirty);
    free(s_polyNext);
    s_polyVert = NULL;
    s_polyNV = NULL;
    s_polyKey = NULL;
    s_polyPos = NULL;
    s_polyDirty = NULL;
    s_polyNext = NULL;
    s_ringCount = -1;   /* (track_rings: listed again) */
    s_tpts = NULL;
    s_tFirst = NULL;
    s_chunks = NULL;
    s_polyOrder = NULL;
    s_verts = NULL;
    s_groups = NULL;
    s_objs = NULL;
    s_polyOwner = NULL;
    s_built = 0;
}

/* s_origins' index for o, added if new; -1 when they're all taken */
static int origin_index(const TrackOrigin *o)
{
    for (int i = 1; i < s_originCount; i++) {
        if (s_origins[i].x == o->x && s_origins[i].y == o->y && s_origins[i].z == o->z) {
            return i;
        }
    }
    if (s_originCount >= MAX_ORIGINS) {
        return -1;
    }
    s_origins[s_originCount] = *o;
    return s_originCount++;
}

static int origin_fits(const float *lo, const float *hi, const TrackOrigin *o)
{
    const int at[3] = { o->x, o->y, o->z };
    for (int a = 0; a < 3; a++) {
        if (lo[a] - (float)at[a] < -32767.0f || hi[a] - (float)at[a] > 32767.0f) {
            return 0;
        }
    }
    return 1;
}

/* A track part's origin, from its built vertices (world units): its cell's
 * around the track's centre (s_origins[1]), else its own centre; -1 if
 * neither reaches all of it (it is then drawn from the current data). */
static int object_origin(const GuVertex *v, int n)
{
    if (n == 0) {
        return 1;
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < n; i++) {
        const float p[3] = { v[i].x, v[i].y, v[i].z };
        for (int a = 0; a < 3; a++) {
            lo[a] = p[a] < lo[a] ? p[a] : lo[a];
            hi[a] = p[a] > hi[a] ? p[a] : hi[a];
        }
    }
    if (origin_fits(lo, hi, &s_origins[1])) {
        return 1;   /* (the track's centre's first: one origin, runs joined, wherever it reaches) */
    }
    const int centre[3] = { (int)((lo[0] + hi[0]) * 0.5f), (int)((lo[1] + hi[1]) * 0.5f),
                            (int)((lo[2] + hi[2]) * 0.5f) };
    const int track[3] = { s_origins[1].x, s_origins[1].y, s_origins[1].z };
    int cell[3];
    for (int a = 0; a < 3; a++) {
        cell[a] = track[a] + (int)floorf((float)(centre[a] - track[a]) / TRACK_CELL + 0.5f) * TRACK_CELL;
    }
    TrackOrigin o = { cell[0], cell[1], cell[2] };
    if (origin_fits(lo, hi, &o)) {
        return origin_index(&o);
    }
    o.x = centre[0];
    o.y = centre[1];
    o.z = centre[2];
    return origin_fits(lo, hi, &o) ? origin_index(&o) : -1;
}

/* The track's centre (origin 1), from its parts' vertices; origin 0 the
 * decorations' */
static void origins_reset(void)
{
    int lo[3] = { 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF }, hi[3] = { -0x7FFFFFFF, -0x7FFFFFFF, -0x7FFFFFFF };
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        if (obj_mode(obj) == 2) {
            continue;
        }
        const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
        for (int pi = 0; pi < obj_pcount(obj); pi++) {
            for (int k = 0; k < poly_verts(polys + pi * 0x30); k++) {
                const SrcVertex *v = poly_vertex(polys + pi * 0x30, k);
                const int p[3] = { v->posX, v->posY, v->posZ };
                for (int a = 0; a < 3; a++) {
                    lo[a] = p[a] < lo[a] ? p[a] : lo[a];
                    hi[a] = p[a] > hi[a] ? p[a] : hi[a];
                }
            }
        }
    }
    memset(s_origins, 0, sizeof(s_origins));
    s_origins[1].x = lo[0] <= hi[0] ? (lo[0] + hi[0]) / 2 : 0;
    s_origins[1].y = lo[1] <= hi[1] ? (lo[1] + hi[1]) / 2 : 0;
    s_origins[1].z = lo[2] <= hi[2] ? (lo[2] + hi[2]) / 2 : 0;
    s_originCount = 2;
}

/* All the track's objects, built after a load: on the first race frame after
 * a gap in RenderTrackD3D calls (the menus and loading between races), or if
 * the track's arrays changed. */
static void ensure_built(void)
{
    unsigned int frame = GU_FrameNo();
    int gap = frame - s_lastFrame > 2;
    s_lastFrame = frame;
    if ((s_built || s_buildFailed) && !gap && s_builtObjs == g_objectStructArray && s_builtPolys == g_polygonArrayBase &&
        s_builtVerts == g_vertexArrayBase && s_builtTotal == g_totalObjects && s_builtTrack == g_trackId) {
        return;
    }
    free_build();
    s_buildFailed = 0;
    s_builtObjs = g_objectStructArray;
    s_builtPolys = g_polygonArrayBase;
    s_builtVerts = g_vertexArrayBase;
    s_builtTotal = g_totalObjects;
    s_builtTrack = g_trackId;
    if (g_objectStructArray == NULL || g_polygonArrayBase == NULL || g_vertexArrayBase == NULL || g_totalObjects <= 0) {
        return;
    }
    GU_TexturesSettle();   /* (the pages made before this allocates — Radiant Emerald has no grid to) */

    unsigned int t0 = sceKernelGetSystemTimeLow();
    s_objCount = g_totalObjects;
    int totalVerts = 0, totalGroups = 0, totalChunks = 0;
    s_polyCount = 0;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        int end = obj_pstart(obj) + obj_pcount(obj);
        if (end > s_polyCount) {
            s_polyCount = end;
        }
    }
    find_tjunctions();
    if (sort_polygons() < 0) {
        DebugLog("render_track_psp: no memory for the GE track's polygon order\n");
        free_build();
        s_buildFailed = 1;
        return;
    }
    /* The track parts' groups go in sections by key (page, culled), the
     * objects in order within each: the chunks of consecutive objects on a
     * page then lie end to end, and draw as one (queue_run). Decorations,
     * each with its own matrix, after them. */
    static int keyBase[512], keyCursor[512];
    memset(keyBase, 0, sizeof(keyBase));
    int decoVerts = 0, maxObjVerts = 0;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        TrackGroup gs[64];
        int ng = 0, nc = 0;
        int n = build_object(oi, NULL, gs, &ng, NULL, &nc);
        totalVerts += n;
        totalGroups += ng;
        totalChunks += nc;
        if (n > maxObjVerts) {
            maxObjVerts = n;
        }
        if (obj_mode(obj) == 2) {
            decoVerts += n;
        } else {
            for (int g = 0; g < ng; g++) {
                keyBase[gs[g].tpage * 2 + gs[g].cull] += gs[g].count;
            }
        }
    }
    int sectionAt = 0;
    for (int k = 0; k < 512; k++) {
        int size = keyBase[k];
        keyBase[k] = keyCursor[k] = sectionAt;
        sectionAt += size;
    }
    int decoCursor = sectionAt;

    s_verts = totalVerts <= TRACK_POOL_VERTS ? s_vpool
                                             : (TrackVertex *)memalign(16, (size_t)totalVerts * sizeof(TrackVertex));
    s_groups = (TrackGroup *)malloc((size_t)(totalGroups ? totalGroups : 1) * sizeof(TrackGroup));
    s_objs = (TrackObj *)calloc((size_t)s_objCount, sizeof(TrackObj));
    s_polyOwner = (unsigned short *)malloc((size_t)(s_polyCount ? s_polyCount : 1) * sizeof(unsigned short));
    s_chunks = (TrackChunk *)malloc((size_t)(totalChunks ? totalChunks : 1) * sizeof(TrackChunk));
    s_objTemp = (GuVertex *)malloc((size_t)(maxObjVerts ? maxObjVerts : 1) * sizeof(GuVertex));
    size_t np = (size_t)(s_polyCount ? s_polyCount : 1);
    s_polyVert = (int *)malloc(np * sizeof(int));
    s_polyNV = (unsigned short *)malloc(np * sizeof(unsigned short));
    s_polyKey = (unsigned short *)malloc(np * sizeof(unsigned short));
    s_polyPos = (uint32_t *)malloc(np * sizeof(uint32_t));
    s_polyDirty = (unsigned char *)calloc(np, 1);
    s_polyNext = (int *)malloc(np * sizeof(int));
    if (!s_verts || !s_groups || !s_objs || !s_polyOwner || !s_chunks || !s_polyVert || !s_polyNV ||
        !s_polyKey || !s_polyPos || !s_polyDirty || !s_polyNext || !s_objTemp) {
        int heapUsed, heapGrown;
        extern void PSP_HeapKB(int *inUse, int *grown);   /* platform_psp.c */
        PSP_HeapKB(&heapUsed, &heapGrown);
        DebugLog("render_track_psp: no memory for the GE track (%d vertices, %d KB): heap %d KB in use (grown to "
                 "%d KB), largest free %d KB\n",
                 totalVerts, (int)(totalVerts * sizeof(TrackVertex) / 1024), heapUsed, heapGrown, PSP_LargestFreeKB());
        free_build();
        s_buildFailed = 1;
        return;
    }
    memset(s_polyOwner, 0xFF, (size_t)s_polyCount * sizeof(unsigned short));
    origins_reset();

    int gi = 0, ci = 0, unplaced = 0;
    for (int oi = 0; oi < s_objCount; oi++) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        TrackObj *to = &s_objs[oi];
        to->groupFirst = gi;
        {   /* (every object: any mode but 2 is a track part, as in the desktop path) */
            int ng = 0, nc = 0;
            s_recordBase = 0;   /* (positions within s_objTemp, moved below) */
            int n = build_object(oi, s_objTemp, &s_groups[gi], &ng, &s_chunks[ci], &nc);
            s_recordBase = -1;
            int deco = obj_mode(obj) == 2;
            int origin = deco ? 0 : object_origin(s_objTemp, n);
            if (origin < 0) {
                to->dynamic = 1;   /* (no origin reaches it: from the current data) */
                origin = 1;
                unplaced++;
            }
            to->origin = (unsigned char)origin;
            int delta[64];
            for (int g = 0; g < ng; g++) {
                TrackGroup *grp = &s_groups[gi + g];
                int dest;
                if (deco) {
                    dest = decoCursor;
                    decoCursor += grp->count;
                } else {
                    int *cur = &keyCursor[grp->tpage * 2 + grp->cull];
                    dest = *cur;
                    *cur += grp->count;
                }
                pack_vertices(&s_verts[dest], &s_objTemp[grp->first], grp->count, &s_origins[origin]);
                delta[g] = dest - grp->first;
                for (int c = 0; c < grp->chunkCount; c++) {
                    TrackChunk *ch = &s_chunks[ci + grp->chunkFirst + c];
                    ch->first += delta[g];
                    chunk_bounds(ch);
                }
                grp->first = dest;
                grp->chunkFirst += ci;
            }
            /* each polygon's vertices moved with its group (one group a key) */
            for (int p = obj_pstart(obj); p < obj_pstart(obj) + obj_pcount(obj); p++) {
                int key = poly_key((const char *)g_polygonArrayBase + p * 0x30);
                for (int g = 0; g < ng; g++) {
                    const TrackGroup *grp = &s_groups[gi + g];
                    if (grp->tpage * 2 + grp->cull == key) {
                        s_polyVert[p] += delta[g];
                        break;
                    }
                }
            }
            ci += nc;
            to->vertCount = n;
            to->groupCount = ng;
            object_bounds(oi, to);
            to->hash = object_hash(oi);
            gi += ng;
        }
        for (int p = obj_pstart(obj); p < obj_pstart(obj) + obj_pcount(obj); p++) {
            s_polyOwner[p] = (unsigned short)oi;
        }
    }
    sceKernelDcacheWritebackRange(s_verts, (unsigned)totalVerts * sizeof(TrackVertex));
    s_built = 1;
    for (int i = 0; i < s_originCount; i++) {   /* (T(origin) S(32768): the 16-bit positions are / 32768 to the GE) */
        ScePspFMatrix4 *m = &s_originModel[i];
        memset(m, 0, sizeof(*m));
        m->x.x = m->y.y = m->z.z = TRACK_UNIT;
        m->w.x = (float)s_origins[i].x;
        m->w.y = (float)s_origins[i].y;
        m->w.z = (float)s_origins[i].z;
        m->w.w = 1.0f;
    }
    DebugLog("render_track_psp: %d origins for the 16-bit vertices, %d parts reached by none (from the current "
             "data)\n", s_originCount - 1, unplaced);
    /* (its pages for VRAM, by the vertices on each — render_gu.c) */
    GU_VramReset(GU_VRAM_TRACK);
    for (int g = 0; g < totalGroups; g++) {
        GU_VramWant(GU_VRAM_TRACK, s_groups[g].tpage, (unsigned)s_groups[g].count);
    }
    GU_VramCommit();
    PSP_LogOpenFiles("track build");
#ifdef SONICR_PROF
    {   /* (each race starts with the test switches off — platform_psp.c) */
        extern int g_pspSkipDraws, g_pspNoTextures, g_pspNoBackground, g_pspFogPasses;
        g_pspTrackDynamic = 0;
        g_pspSkipDraws = g_pspNoTextures = g_pspNoBackground = 0;
        g_pspFogPasses = 1;
    }
#endif
    int heapUsed, heapGrown;
    extern void PSP_HeapKB(int *inUse, int *grown);   /* platform_psp.c */
    PSP_HeapKB(&heapUsed, &heapGrown);
    DebugLog("render_track_psp: track %d built for the GE: %d objects, %d polygons, %d vertices (%d KB), %d groups, "
             "%d chunks, %u ms; heap %d KB in use (grown to %d KB)\n",
             g_trackId, s_objCount, s_polyCount, totalVerts, (int)(totalVerts * sizeof(TrackVertex) / 1024), totalGroups,
             totalChunks, (sceKernelGetSystemTimeLow() - t0) / 1000, heapUsed, heapGrown);
    DebugLog("render_track_psp: the game's arrays used: %d of 32768 vertices, %d of 16384 polygons, %d of 16384 "
             "animation frames\n",
             g_vertexIndexRunning, g_polygonIndexRunning, g_modelFrameCount);
}

/* camera_per_track.c's and animation.c's UV animations (TRACK_POLY_CHANGED):
 * the polygon noted on its object's list, written again when the object is
 * next drawn (apply_object_updates) — the Reactive Factory animates ~440
 * polygons a frame, most of them off the screen. */
void PSP_TrackPolyChanged(const void *poly)
{
    if (!s_built || g_polygonArrayBase == NULL) {
        return;
    }
    int pi = (int)(((const char *)poly - (const char *)g_polygonArrayBase) / 0x30);
    if (pi < 0 || pi >= s_polyCount || s_polyOwner[pi] == 0xFFFF) {
        return;
    }
    TrackObj *to = &s_objs[s_polyOwner[pi]];
    to->hashStale = 1;   /* (for the safety net: not a change nothing reported — again if still pending) */
    if (s_polyDirty[pi]) {
        return;
    }
    s_polyDirty[pi] = 1;
    s_polyNext[pi] = to->pending - 1;
    to->pending = pi + 1;
}

#ifdef SONICR_DEBUG_LOG
unsigned int g_trackPolyUpdates, g_trackObjRebuilds, g_trackUsRebuild;   /* reported by FlipD3D */
#endif

/* Object oi's polygons TRACK_POLY_CHANGED reported, each written again where
 * it was built — an animated UV is one polygon, where rebuilding its object
 * (some are hundreds of polygons) cost the frame that much more every time.
 * The callers only move UVs, so a polygon built whole (no T-junction points)
 * has just its corners' UVs written; one whose page or flags changed takes
 * its object's rebuild instead. Before the object is drawn: each polygon
 * written back from the cache before the GE reads it, its few vertices
 * alone. (In Reactive Factory's demo the updates cost the frame nothing
 * measurable either way, this or the whole cache an object.) */
static void apply_object_updates(int oi)
{
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    TrackObj *to = &s_objs[oi];
    for (int pi = to->pending - 1; pi >= 0; pi = s_polyNext[pi]) {
        s_polyDirty[pi] = 0;
        if (to->dynamic || to->dirty || s_polyVert[pi] < 0) {
            continue;
        }
        const char *pp = (const char *)g_polygonArrayBase + pi * 0x30;
        int nv = poly_verts(pp);
        int whole = (s_tFirst == NULL || s_tFirst[pi + 1] == s_tFirst[pi]) && s_polyNV[pi] == (nv == 4 ? 6 : 3);
        if (poly_key(pp) != s_polyKey[pi]) {
            to->dirty = 1;
            continue;
        }
        if (whole) {
            const int *face = (const int *)pp;
            TrackVertex *out = &s_verts[s_polyVert[pi]];
            for (int i = 0; i < s_polyNV[pi]; i++) {
                out[i].u = (unsigned short)(TrackUV(face[order[i] * 2]) * TRACK_UNIT + 0.5f);
                out[i].v = (unsigned short)(TrackUV(face[order[i] * 2 + 1]) * TRACK_UNIT + 0.5f);
            }
        } else {
            if (poly_pos_hash(pp) != s_polyPos[pi] || build_polygon(pi, pp, NULL) != s_polyNV[pi]) {
                to->dirty = 1;
                continue;
            }
            build_polygon(pi, pp, s_objTemp);   /* (free between builds: as large as any object) */
            pack_vertices(&s_verts[s_polyVert[pi]], s_objTemp, s_polyNV[pi], &s_origins[to->origin]);
        }
        sceKernelDcacheWritebackRange(&s_verts[s_polyVert[pi]], (unsigned)s_polyNV[pi] * sizeof(TrackVertex));
#ifdef SONICR_DEBUG_LOG
        g_trackPolyUpdates++;
#endif
    }
    to->pending = 0;
}

/* Radiant Emerald's colours: the desktop path recolours each object's
 * vertices before drawing it (TrackEmeraldRecolourObject), every channel a
 * sine of one coordinate plus the tick's phase (g_emeraldSineOff*). Here the
 * same, on the object's built vertices — their positions are their source
 * vertices' (no T-junction splits, TJ_SPLIT) — once a tick, when it is about
 * to be drawn; a dynamic object's source vertices as the desktop path does.
 * (The GE may still be reading the last frame's: a triangle a tick off, the
 * colour moving ~3% of its range a tick.) */
static unsigned char s_emeraldLevel[4096];   /* (sin << 6) + 0x100020, >> 13, clamped */

static void emerald_recolour(int oi)
{
    TrackObj *to = &s_objs[oi];
    const unsigned ox = (unsigned)g_emeraldSineOffX & 0xFFF, oy = (unsigned)g_emeraldSineOffY & 0xFFF,
                   oz = (unsigned)g_emeraldSineOffZ & 0xFFF;
    uint32_t phase = (ox | (oy << 12) | (oz << 24)) + 1;
    if (to->emeraldPhase == phase) {
        return;
    }
    to->emeraldPhase = phase;
    if (s_emeraldLevel[0] == 0 && s_emeraldLevel[1024] == 0) {
        for (int i = 0; i < 4096; i++) {
            int c = ((g_sinTable[i] << 6) + 0x100020) >> 13;
            s_emeraldLevel[i] = (unsigned char)(c > 255 ? 255 : c < 0 ? 0 : c);
        }
    }
    if (to->dynamic) {
        const int *obj = (const int *)g_objectStructArray + oi * 17;
        const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
        for (int pi = 0; pi < obj_pcount(obj); pi++) {
            const char *pp = polys + pi * 0x30;
            for (int k = 0; k < poly_verts(pp); k++) {
                SrcVertex *v = &g_vertexArrayBase[*(const unsigned short *)(pp + 0x20 + k * 2)];
                v->colorR = (g_sinTable[(v->posX + g_emeraldSineOffX) & 0xFFF] << 6) + 0x100020;
                v->colorG = (g_sinTable[(v->posY + g_emeraldSineOffY) & 0xFFF] << 6) + 0x100020;
                v->colorB = (g_sinTable[(v->posZ + g_emeraldSineOffZ) & 0xFFF] << 6) + 0x100020;
            }
        }
        return;
    }
    const TrackOrigin *org = &s_origins[to->origin];   /* (the positions are from it) */
    const unsigned px = ox + (unsigned)org->x, py = oy + (unsigned)org->y, pz = oz + (unsigned)org->z;
    for (int g = 0; g < to->groupCount; g++) {
        const TrackGroup *grp = &s_groups[to->groupFirst + g];
        TrackVertex *v = &s_verts[grp->first];
        for (int i = 0; i < grp->count; i++) {
            unsigned r = s_emeraldLevel[((unsigned)v[i].x + px) & 0xFFF];
            unsigned gr = s_emeraldLevel[((unsigned)v[i].y + py) & 0xFFF];
            unsigned b = s_emeraldLevel[((unsigned)v[i].z + pz) & 0xFFF];
            v[i].color = 0xFF000000u | (b << 16) | (gr << 8) | r;
        }
        sceKernelDcacheWritebackRange(v, (unsigned)grp->count * sizeof(TrackVertex));
    }
}

/* Built again in place. If the change moved its layout (a polygon's page or
 * flags, so the groups differ), the object can't be: it goes to the
 * current-data path for good. */
static void rebuild_object(int oi)
{
    TrackObj *to = &s_objs[oi];
    to->dirty = 0;
    to->hash = object_hash(oi);
    if (to->vertCount == 0 || to->dynamic) {
        return;
    }
    TrackGroup now[64];
    int ng = 0;
    int n = to->groupCount <= 64 ? build_object(oi, NULL, now, &ng, NULL, NULL) : -1;
    int same = n == to->vertCount && ng == to->groupCount;
    for (int g = 0; same && g < ng; g++) {
        const TrackGroup *was = &s_groups[to->groupFirst + g];
        same = now[g].tpage == was->tpage && now[g].cull == was->cull &&
               now[g].count == was->count && now[g].chunkCount == was->chunkCount;
    }
    if (!same) {
        to->dynamic = 1;
        DebugLog("render_track_psp: object %d changed layout, drawn from current data\n", oi);
        return;
    }
    /* (built whole, then each group to its place: the same layout, so the
     * polygons' recorded places hold) */
    build_object(oi, s_objTemp, NULL, NULL, NULL, NULL);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < n; i++) {
        const float p[3] = { s_objTemp[i].x, s_objTemp[i].y, s_objTemp[i].z };
        for (int a = 0; a < 3; a++) {
            lo[a] = p[a] < lo[a] ? p[a] : lo[a];
            hi[a] = p[a] > hi[a] ? p[a] : hi[a];
        }
    }
    if (n && !origin_fits(lo, hi, &s_origins[to->origin])) {
        to->dynamic = 1;   /* (an animation moved it past its origin's reach) */
        DebugLog("render_track_psp: object %d moved past its origin, drawn from current data\n", oi);
        return;
    }
    int local = 0;
    for (int g = 0; g < to->groupCount; g++) {
        const TrackGroup *grp = &s_groups[to->groupFirst + g];
        pack_vertices(&s_verts[grp->first], &s_objTemp[local], grp->count, &s_origins[to->origin]);
        sceKernelDcacheWritebackRange(&s_verts[grp->first], (unsigned)grp->count * sizeof(TrackVertex));
        local += grp->count;
    }
    to->hashStale = 0;
#ifdef SONICR_DEBUG_LOG
    g_trackObjRebuilds++;
#endif
    for (int g = 0; g < to->groupCount; g++) {
        const TrackGroup *grp = &s_groups[to->groupFirst + g];
        for (int c = 0; c < grp->chunkCount; c++) {
            chunk_bounds(&s_chunks[grp->chunkFirst + c]);   /* (an animation may move them) */
        }
    }
}

/* The desktop path's view transform as a GE matrix: camera-relative
 * position through g_viewMtx / 4096, the X row without its m10 term (the
 * binary drops it). */
static void view_matrix(ScePspFMatrix4 *m)
{
    const float k = 1.0f / 4096.0f;
    float m00 = g_viewMtx00 * k, m01 = g_viewMtx01 * k, m02 = g_viewMtx02 * k;
    float m11 = g_viewMtx11 * k, m12 = g_viewMtx12 * k;
    float m20 = g_viewMtx20 * k, m21 = g_viewMtx21 * k, m22 = g_viewMtx22 * k;
    float cx = (float)g_camIntX, cy = (float)g_camIntY, cz = (float)g_camIntZ;
    memset(m, 0, sizeof(*m));
    m->x.x = m00; m->x.y = m01; m->x.z = m02;
    m->y.x = 0.0f; m->y.y = m11; m->y.z = m12;
    m->z.x = m20; m->z.y = m21; m->z.z = m22;
    m->w.x = -(m00 * cx + m20 * cz);
    m->w.y = -(m01 * cx + m11 * cy + m21 * cz);
    m->w.z = -(m02 * cx + m12 * cy + m22 * cz);
    m->w.w = 1.0f;
}

/* Mode 2 (decorations): the desktop path's local rotation from the object's
 * yaw/pitch/roll, then its pivot — the same integer terms, as a matrix. */
static void decoration_matrix(const int *obj, ScePspFMatrix4 *m)
{
    int yaw   = *(const short *)((const char *)obj + 0x18);
    int pitch = *(const short *)((const char *)obj + 0x1A);
    int roll  = *(const short *)((const char *)obj + 0x1C);
    int sinY  = g_sinTable[yaw] >> 2;
    int cosY  = g_cosTable[yaw] >> 2;
    int cosR  = g_cosTable[roll] >> 2;
    int sinR  = g_sinTable[roll] >> 2;
    int sinP2 = g_sinTable[pitch] >> 2;
    int cosP2 = g_cosTable[pitch] >> 2;
    int cYsR = (-(cosY * sinR)) >> 12;
    int sRsY = (sinR * sinY) >> 12;
    int L00 = (cosR * cosP2) >> 12;
    int L01 = (cYsR * cosP2 + sinY * sinP2) >> 12;
    int L02 = (cosY * sinP2 + sRsY * cosP2) >> 12;
    int L10 = sinR;
    int L11 = (cosY * cosR) >> 12;
    int L12 = (-(sinY * cosR)) >> 12;
    int L20 = (-(cosR * sinP2)) >> 12;
    int L21 = (sinY * cosP2 - cYsR * sinP2) >> 12;
    int L22 = (cosP2 * cosY - sinP2 * sRsY) >> 12;
    const float k = 1.0f / 4096.0f;
    memset(m, 0, sizeof(*m));
    m->x.x = L00 * k; m->x.y = L10 * k; m->x.z = L20 * k;
    m->y.x = L01 * k; m->y.y = L11 * k; m->y.z = L21 * k;
    m->z.x = L02 * k; m->z.y = L12 * k; m->z.z = L22 * k;
    m->w.x = (float)obj[8];
    m->w.y = (float)obj[9];
    m->w.z = (float)obj[10];
    m->w.w = 1.0f;
}

/* =====================================================================
 * Objects at the camera: drawn on the CPU, in float
 *
 * The objects the GE can't take for their nearness (the near plane, the
 * guard band) are drawn here rather than by the desktop path: the same steps
 * (far cull, near-plane clip, screen-space backface test, fog ramp, LUT UVs)
 * but transformed in float through the same matrix the GE uses. The desktop
 * path's integer transform truncates camera space to whole units, which put
 * its edges up to a pixel off the GE-drawn neighbours' — the sky showed
 * through the seams across the road.
 * ===================================================================== */

typedef struct {
    float x, y, z;       /* camera space */
    float u, v, r, g, b;
} NearVert;

typedef struct { float m[3][4]; } Affine;   /* rows: camX, camY, depth */

static Affine affine_from(const ScePspFMatrix4 *a)
{
    Affine o;
    const ScePspFVector4 *c[4] = { &a->x, &a->y, &a->z, &a->w };
    for (int j = 0; j < 4; j++) {
        o.m[0][j] = c[j]->x;
        o.m[1][j] = c[j]->y;
        o.m[2][j] = c[j]->z;
    }
    return o;
}

/* a * b, both affine */
static Affine affine_mul(const Affine *a, const Affine *b)
{
    Affine o;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float s = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
            o.m[i][j] = s + (j == 3 ? a->m[i][3] : 0.0f);
        }
    }
    return o;
}

/* The view's affine for object oi's built vertices: a track part's from its
 * origin, a decoration's through its own matrix */
static Affine object_affine(int oi, const Affine *viewA)
{
    const int *obj = (const int *)g_objectStructArray + oi * 17;
    if (obj_mode(obj) == 2) {
        ScePspFMatrix4 deco;
        decoration_matrix(obj, &deco);
        Affine modelA = affine_from(&deco);
        return affine_mul(viewA, &modelA);
    }
    Affine t = *viewA;
    const TrackOrigin *o = &s_origins[s_objs[oi].origin];
    for (int i = 0; i < 3; i++) {
        t.m[i][3] += t.m[i][0] * (float)o->x + t.m[i][1] * (float)o->y + t.m[i][2] * (float)o->z;
    }
    return t;
}

static inline void near_vertex(NearVert *o, const Affine *t, const SrcVertex *v, const int *face, int idx)
{
    float x = (float)v->posX, y = (float)v->posY, z = (float)v->posZ;
    o->x = t->m[0][0] * x + t->m[0][1] * y + t->m[0][2] * z + t->m[0][3];
    o->y = t->m[1][0] * x + t->m[1][1] * y + t->m[1][2] * z + t->m[1][3];
    o->z = t->m[2][0] * x + t->m[2][1] * y + t->m[2][2] * z + t->m[2][3];
    o->u = TrackUV(face[idx * 2]);
    o->v = TrackUV(face[idx * 2 + 1]);
    int r = v->colorR >> 13, g = v->colorG >> 13, b = v->colorB >> 13;
    o->r = (float)(r > 255 ? 255 : r);
    o->g = (float)(g > 255 ? 255 : g);
    o->b = (float)(b > 255 ? 255 : b);
}

static inline void near_project(const NearVert *n, float *sx, float *sy)
{
    float inv = 1.0f / n->z;
    *sx = (float)g_screenCenterX + (float)g_projScaleXCurrent * n->x * inv;
    *sy = (float)g_screenCenterY - (float)g_projScaleY * n->y * inv;
}

/* Screen-space cross of a, b, c (TrackBackfaceCross's sign, mirror flip
 * included): negative is a back face. */
static float near_cross(const NearVert *a, const NearVert *b, const NearVert *c)
{
    float ax, ay, bx, by, cx, cy;
    near_project(a, &ax, &ay);
    near_project(b, &bx, &by);
    near_project(c, &cx, &cy);
    float cross = (ay - by) * (cx - bx) - (ax - bx) * (cy - by);
    return g_mirrorMode ? -cross : cross;
}

/* Sutherland-Hodgman against the near plane, depth >= 1 */
static int near_clip(const NearVert *in, int n, NearVert *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const NearVert *a = &in[i], *b = &in[(i + 1) % n];
        int ina = a->z >= 1.0f, inb = b->z >= 1.0f;
        if (ina) {
            out[m++] = *a;
        }
        if (ina != inb) {
            /* Always from the vertex in front to the one behind: the
             * neighbour sharing this edge walks it the other way, and a
             * lerp from the other end rounds to a slightly different point —
             * far off screen, where it tilted the whole edge apart from its
             * neighbour's (a line of sky along the seam). */
            const NearVert *p = ina ? a : b, *q = ina ? b : a;
            float t = (1.0f - p->z) / (q->z - p->z);
            NearVert *o = &out[m++];
            o->x = p->x + (q->x - p->x) * t;
            o->y = p->y + (q->y - p->y) * t;
            o->z = 1.0f;
            o->u = p->u + (q->u - p->u) * t;
            o->v = p->v + (q->v - p->v) * t;
            o->r = p->r + (q->r - p->r) * t;
            o->g = p->g + (q->g - p->g) * t;
            o->b = p->b + (q->b - p->b) * t;
        }
    }
    return m;
}

static int fog_alpha(float depth, float farSafe)
{
    float fogD = depth / farSafe;
    if (fogD > (float)FOG_FAR) {
        return 0;
    }
    if (fogD <= (float)FOG_NEAR) {
        return 0xFF;
    }
    int raw = (int)((fogD + (float)FOG_NEG_FAR) * (float)FOG_MUL);
    return raw < 0 ? -raw : raw;
}

/* fog_alpha without its divide: (FOG_FAR - depth / far) * FOG_MUL, as a
 * distance from the FOG_FAR depth times FOG_MUL / far */
typedef struct {
    float nearZ, farZ, scale;
} FogRamp;

static FogRamp fog_ramp(float farSafe)
{
    FogRamp f;
    f.nearZ = (float)FOG_NEAR * farSafe;
    f.farZ = (float)FOG_FAR * farSafe;
    f.scale = (float)FOG_MUL / farSafe;
    return f;
}

static inline uint32_t fog_alpha24(float z, const FogRamp *f)
{
    if (z <= f->nearZ) {
        return 0xFF000000u;
    }
    if (z > f->farZ) {
        return 0;
    }
    return (uint32_t)(int)((f->farZ - z) * f->scale) << 24;
}

static void near_emit(const NearVert *nv, int n, int tpage, float farSafe)
{
    RenderVertex rv[8];
    int tr = 0, tg = 0, tb = 0;   /* (a lightning flash's lift, as the desktop path adds it) */
    if (g_colorTintEnable) {
        tr = g_colorTintR >> 13;
        tg = g_colorTintG >> 13;
        tb = g_colorTintB >> 13;
    }
    for (int i = 0; i < n; i++) {
        near_project(&nv[i], &rv[i].sx, &rv[i].sy);
        rv[i].sz = nv[i].z / farSafe;
        rv[i].rhw = 1.0f / nv[i].z;
        int r = (int)nv[i].r + tr, g = (int)nv[i].g + tg, b = (int)nv[i].b + tb;
        r = r > 255 ? 255 : r;
        g = g > 255 ? 255 : g;
        b = b > 255 ? 255 : b;
        rv[i].color = ((uint32_t)fog_alpha(nv[i].z, farSafe) << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
                      (uint32_t)b;
        rv[i].specular = 0;
        rv[i].u = nv[i].u;
        rv[i].v = nv[i].v;
    }
    if (tpage >= 0) {   /* (-1: set by the caller) */
        R_SetTexture(tpage);
        R_SetTexEnv(R_TEXENV_ADD_SIGNED);
    }
    R_DrawTriFan(rv, n);
}

static void near_draw_object(const int *obj, const Affine *t, float farSafe, int farClipTimes8)
{
    const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
    int n = obj_pcount(obj);
    for (int pi = 0; pi < n; pi++) {
        const char *pp = polys + pi * 0x30;
        int tpage = *(const unsigned char *)(pp + 0x28);
        if (g_tpageStateArray[tpage] != 0x04) {
            continue;
        }
        unsigned char flags = *(const unsigned char *)(pp + 0x2E);
        int nv = (flags & 1) ? 4 : 3;
        const int *face = (const int *)pp;
        NearVert v[4];
        int behind = 0, fogged = 0, beyond = 0;
        for (int k = 0; k < nv; k++) {
            near_vertex(&v[k], t, &g_vertexArrayBase[*(const unsigned short *)(pp + 0x20 + k * 2)], face, k);
            behind += v[k].z < 1.0f;
            beyond += v[k].z > (float)farClipTimes8;
            fogged += v[k].z / farSafe > (float)FOG_FAR;
        }
        if (beyond || behind == nv) {
            continue;   /* far cull: any vertex past the far plane; all behind the camera */
        }
#if TRACK_FULL_FOG_CULL
        if (fogged == nv) {
            continue;
        }
#endif
        if (behind == 0) {
            if ((flags & 4) == 0 && near_cross(&v[0], &v[1], &v[2]) < 0.0f) {
                /* flags & 2: the quad's other triangle decides too */
                if (nv == 3 || (flags & 2) == 0 || near_cross(&v[0], &v[2], &v[3]) < 0.0f) {
                    continue;
                }
            }
            near_emit(v, nv, tpage, farSafe);
        } else {
            NearVert clipped[8];
            int m = near_clip(v, nv, clipped);
            if (m < 3) {
                continue;
            }
            if ((flags & 4) == 0) {
                float area = 0.0f;   /* (the whole polygon's: see near_draw_built) */
                for (int k = 1; k + 1 < m; k++) {
                    area += near_cross(&clipped[0], &clipped[k], &clipped[k + 1]);
                }
                if (area < 0.0f) {
                    continue;
                }
            }
            near_emit(clipped, m, tpage, farSafe);
        }
    }
}

/* =====================================================================
 * Objects in the fog band, on the GE
 *
 * Past FOG_NEAR of the far plane the desktop path fades polygons out by
 * their vertices' alpha, and skips a polygon with any vertex past the far
 * plane or all of them past FOG_FAR. Those need each vertex's depth, not its
 * projection: one row of the transform, then the vertices go to the GE with
 * that alpha, in this frame's vertex arena.
 * ===================================================================== */

/* 0: drawn; -1: no room in the arena (the caller draws it another way) */
static int fog_draw_object(const int *obj, const ScePspFMatrix4 *model, const Affine *t,
                           float farSafe, int farClipTimes8, int frontFace)
{
    const char *polys = (const char *)g_polygonArrayBase + obj_pstart(obj) * 0x30;
    int n = obj_pcount(obj);
    GuVertex *out = GU_TransformAlloc(n * 6);
    if (out == NULL) {
        return -1;
    }
    const float *zr = t->m[2];
    int w = 0, runStart = 0, runKey = -1;
    const ScePspFMatrix4 *m = model;

    for (int pi = 0; pi <= n; pi++) {
        const char *pp = pi < n ? polys + pi * 0x30 : NULL;
        int key = -2;
        float z[4];
        const SrcVertex *sv[4];
        int nv = 0;
        if (pp) {
            int tpage = *(const unsigned char *)(pp + 0x28);
            if (g_tpageStateArray[tpage] != 0x04) {
                continue;
            }
            nv = poly_verts(pp);
            int beyond = 0, fogged = 0;
            for (int k = 0; k < nv; k++) {
                sv[k] = &g_vertexArrayBase[*(const unsigned short *)(pp + 0x20 + k * 2)];
                z[k] = zr[0] * (float)sv[k]->posX + zr[1] * (float)sv[k]->posY + zr[2] * (float)sv[k]->posZ + zr[3];
                beyond += z[k] > (float)farClipTimes8;
                fogged += z[k] / farSafe > (float)FOG_FAR;
            }
            if (beyond) {
                continue;
            }
#if TRACK_FULL_FOG_CULL
            if (fogged == nv) {
                continue;
            }
#endif
            key = poly_key(pp);
        }
        /* a run of one (texture page, culled) ends: draw it */
        if (key != runKey && w > runStart) {
            R_SetTexture(runKey >> 1);
            R_SetTexEnv(R_TEXENV_ADD_SIGNED);
            GU_TransformDrawFresh(m, runKey & 1, frontFace, &out[runStart], w - runStart);
            m = NULL;
            runStart = w;
        }
        if (!pp) {
            break;
        }
        runKey = key;
        static const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
        const int *face = (const int *)pp;
        for (int tri = 0; tri < (nv == 4 ? 2 : 1); tri++) {
            for (int c = 0; c < 3; c++) {
                int k = order[tri][c];
                emit(&out[w], sv[k], face, k);
                out[w].color = (out[w].color & 0x00FFFFFFu) | ((uint32_t)fog_alpha(z[k], farSafe) << 24);
                w++;
            }
        }
    }
    GU_TransformUnalloc(n * 6 - w);
    return 0;
}

/* =====================================================================
 * The same two, from the built triangles
 *
 * What every object not 'dynamic' draws from: its GE vertices, T-junctions
 * split, so the pieces at the camera and in the fog band meet their
 * neighbours as exactly as the GE-drawn ones (and the vertices come ready:
 * floats, UVs looked up, a third of the source's size). Culled per triangle,
 * as on the GE, where the desktop path decides a polygon at a time.
 * ===================================================================== */

/* (t from the vertices' origin: object_affine) */
static inline void built_near_vertex(NearVert *o, const Affine *t, const TrackVertex *v)
{
    const float x = (float)v->x, y = (float)v->y, z = (float)v->z;
    o->x = t->m[0][0] * x + t->m[0][1] * y + t->m[0][2] * z + t->m[0][3];
    o->y = t->m[1][0] * x + t->m[1][1] * y + t->m[1][2] * z + t->m[1][3];
    o->z = t->m[2][0] * x + t->m[2][1] * y + t->m[2][2] * z + t->m[2][3];
    o->u = (float)v->u * (1.0f / TRACK_UNIT);
    o->v = (float)v->v * (1.0f / TRACK_UNIT);
    o->r = (float)(v->color & 0xFF);
    o->g = (float)((v->color >> 8) & 0xFF);
    o->b = (float)((v->color >> 16) & 0xFF);
}

/* One built triangle on the CPU: the far and fog culls, the near-plane clip,
 * the screen-space cull, drawn pre-transformed (the texture set by the
 * caller). */
static void near_tri(const Affine *t, const TrackVertex *src, int cull, float farSafe, int farClipTimes8)
{
    NearVert v[3];
    int behind = 0, fogged = 0, beyond = 0;
    for (int k = 0; k < 3; k++) {
        built_near_vertex(&v[k], t, &src[k]);
        behind += v[k].z < 1.0f;
        beyond += v[k].z > (float)farClipTimes8;
        fogged += v[k].z / farSafe > (float)FOG_FAR;
    }
    if (beyond || behind == 3) {
        return;
    }
#if TRACK_FULL_FOG_CULL
    if (fogged == 3) {
        return;
    }
#endif
    if (behind == 0) {
        if (cull && near_cross(&v[0], &v[1], &v[2]) < 0.0f) {
            return;
        }
        near_emit(v, 3, -1, farSafe);
        return;
    }
    NearVert clipped[8];
    int m = near_clip(v, 3, clipped);
    if (m < 3) {
        return;
    }
    if (cull) {
        /* the whole clipped polygon's area: a corner the clip lands next to
         * makes its first three about collinear */
        float area = 0.0f;
        for (int k = 1; k + 1 < m; k++) {
            area += near_cross(&clipped[0], &clipped[k], &clipped[k + 1]);
        }
        if (area < 0.0f) {
            return;
        }
    }
    near_emit(clipped, m, -1, farSafe);
}

/* A whole object on the CPU (when the arena has no room for draw_object) */
static void near_draw_built(int oi, const Affine *t, float farSafe, int farClipTimes8)
{
    const TrackObj *to = &s_objs[oi];
    for (int g = 0; g < to->groupCount; g++) {
        const TrackGroup *grp = &s_groups[to->groupFirst + g];
        if (g_tpageStateArray[grp->tpage] != 0x04) {
            continue;
        }
        R_SetTexture(grp->tpage);
        R_SetTexEnv(R_TEXENV_ADD_SIGNED);
        for (int i = 0; i < grp->count; i += 3) {
            near_tri(t, &s_verts[grp->first + i], grp->cull, farSafe, farClipTimes8);
        }
    }
}

/* An object at the camera is mostly triangles the GE can take as they are:
 * wholly in front of the near plane and inside the guard band. Only the
 * rest, a few along the bottom of the screen, are clipped on the CPU — noted
 * by draw_object, drawn by near_draw_cpu_tris once the GE's transform is
 * ended. */
typedef struct {
    int vert;                   /* the triangle's first, in s_verts */
    unsigned short oi;
    unsigned char tpage, cull;
} CpuTri;

#define MAX_CPU_TRIS 4096
#define GE_Z_MIN     33.0f      /* (past render_gu.c DEPTH_3D: the GE near plane clear) */
static CpuTri s_cpuTris[MAX_CPU_TRIS];
static int s_cpuTriCount;

static void near_draw_cpu_tris(const int *objBase, const Affine *viewA, float farSafe, int farClipTimes8)
{
    int lastOi = -1, lastPage = -1;
    Affine t = *viewA;
    for (int i = 0; i < s_cpuTriCount; i++) {
        const CpuTri *c = &s_cpuTris[i];
        if (c->oi != lastOi) {
            t = object_affine(c->oi, viewA);
            lastOi = c->oi;
        }
        if (c->tpage != lastPage) {
            R_SetTexture(c->tpage);
            R_SetTexEnv(R_TEXENV_ADD_SIGNED);
            lastPage = c->tpage;
        }
        near_tri(&t, &s_verts[c->vert], c->cull, farSafe, farClipTimes8);
    }
    s_cpuTriCount = 0;
}

/* =====================================================================
 * An object on the GE, a chunk at a time
 *
 * A chunk clear of the near plane and inside the guard band goes as built
 * (runs of such chunks in one draw), faded by the GE's fog passes where it
 * reaches into the fog band (GU_TransformDrawFog); any other goes a triangle
 * at a time: the far and fog culls, each vertex's fog alpha into the frame's
 * arena — and the triangles the GE can't take (at the near plane, past the
 * guard band) noted for the CPU (near_draw_cpu_tris).
 * ===================================================================== */

/* The track parts' as-built chunks, joined across objects: kept by key
 * (page, culled, fog) as runs of chunks end to end (the sections built by
 * ensure_built make consecutive objects' chunks so), and all drawn at the end
 * of the pass a key at a time — the page set once for all its runs, where
 * drawing each run as the next broke it set the pages over and over, half of
 * a draw's cost; ~240 draws a frame in Reactive Factory's busiest views, 3 ms.
 * Their matrix is their origin's (s_originModel), runs joined only within
 * one; s_modelOrigin says which the GE has (-1: a decoration's or another). */
typedef struct {
    int start, count;
    short origin, next;         /* (next: the key's next run, -1 none) */
} Run;

#define MAX_RUNS 2048
static GuFrustum s_frustum;   /* (the frame's screen edges, set by RenderTrackD3D) */
static Run s_runs[MAX_RUNS];
static int s_runCount;
static short s_keyFirst[512 * 2], s_keyLast[512 * 2];   /* a key's runs, -1 none */
static short s_runActive[512 * 2];
static int s_runActiveCount;
static int s_modelOrigin = -1, s_runFrontFace;

static const ScePspFMatrix4 *origin_model(int origin)
{
    if (s_modelOrigin == origin) {
        return NULL;
    }
    s_modelOrigin = origin;
    return &s_originModel[origin];
}

static void flush_runs(void);

static void queue_run(int key, int start, int count, int origin)
{
    if (s_runActiveCount == 0 && s_runCount == 0) {
        memset(s_keyFirst, 0xFF, sizeof(s_keyFirst));   /* (the frame's first: none yet) */
    }
    int last = s_keyFirst[key] < 0 ? -1 : s_keyLast[key];
    if (last >= 0) {
        Run *r = &s_runs[last];
        if (r->origin == origin && r->start + r->count == start) {
            r->count += count;
            return;
        }
    }
    if (s_runCount >= MAX_RUNS) {
        flush_runs();   /* (full: what there is drawn now) */
        memset(s_keyFirst, 0xFF, sizeof(s_keyFirst));
        last = -1;
    }
    Run *r = &s_runs[s_runCount];
    r->start = start;
    r->count = count;
    r->origin = (short)origin;
    r->next = -1;
    if (last >= 0) {
        s_runs[last].next = (short)s_runCount;
    } else {
        s_keyFirst[key] = (short)s_runCount;
        s_runActive[s_runActiveCount++] = (short)key;
    }
    s_keyLast[key] = (short)s_runCount;
    s_runCount++;
}

/* The pass's runs, the keys in order (a page's together) */
static void flush_runs(void)
{
    for (int i = 1; i < s_runActiveCount; i++) {   /* (a few dozen keys) */
        short k = s_runActive[i];
        int j = i - 1;
        while (j >= 0 && s_runActive[j] > k) {
            s_runActive[j + 1] = s_runActive[j];
            j--;
        }
        s_runActive[j + 1] = k;
    }
    R_PushState();   /* (the object drawn last keeps its page) */
    for (int i = 0; i < s_runActiveCount; i++) {
        int key = s_runActive[i];
        int tpage = key >> 2, cull = (key >> 1) & 1, fog = key & 1;
        R_SetTexture(tpage);
        R_SetTexEnv(R_TEXENV_ADD_SIGNED);
        for (int r = s_keyFirst[key]; r >= 0; r = s_runs[r].next) {
            const Run *run = &s_runs[r];
#if defined(SONICR_TEST) && SONICR_TEST == 2
            (void)run;
            continue;   /* (experiment: the runs queued, none sent) */
#endif
#ifdef SONICR_DEBUG_LOG
            g_trackRunsDrawn++;
#endif
            if (fog) {
                GU_TransformDrawFogV(origin_model(run->origin), cull, s_runFrontFace, TRACK_VTYPE,
                                     &s_verts[run->start], run->count);
            } else {
                GU_TransformDrawV(origin_model(run->origin), cull, s_runFrontFace, TRACK_VTYPE,
                                  &s_verts[run->start], run->count);
            }
        }
        s_keyFirst[key] = -1;
    }
    R_PopState();
    s_runActiveCount = 0;
    s_runCount = 0;
}

/* 0: drawn; -1: no room in the arena (nothing drawn). model: a decoration's
 * (its matrix, S(32768) in it), NULL a track part's (its origin's); t: the
 * view's for its built vertices (object_affine). */
static int draw_object(int oi, const ScePspFMatrix4 *model, const Affine *t,
                       float farSafe, int farClipTimes8, int frontFace, const float *sphere)
{
    const TrackObj *to = &s_objs[oi];
    int part = model == NULL;
    int modelSent = 0;
    /* A track part wholly on the screen, clear of the near plane and inside
     * the guard band, wholly before the fog band or wholly in it: every chunk
     * of it as built — its groups queued whole, none of its chunks looked at
     * (sphere: its real one in the view, RenderTrackD3D's). In a busy view
     * most are, and their chunks a frame were most of the pass. */
    if (part && sphere) {
        const FogRamp ramp = fog_ramp(farSafe);
        const float x = sphere[0], y = sphere[1], z = sphere[2], r = sphere[3];
        const float zNear = z - r;
        const float gx = GE_GUARD / (float)g_projScaleXCurrent, gy = GE_GUARD / (float)g_projScaleY;
        int fog = z + r > ramp.nearZ;
        int fogOk = 1;
#ifdef SONICR_PROF
        extern int g_pspFogPasses;   /* (test: the fog band a vertex at a time instead) */
        fogOk = g_pspFogPasses;
#endif
        if (zNear >= GE_Z_MIN && fabsf(x) + r <= gx * zNear && fabsf(y) + r <= gy * zNear &&
            (!fog || (fogOk && zNear > ramp.nearZ && z + r <= ramp.farZ)) &&
            GU_FrustumInSphere(&s_frustum, x, y, z, r)) {
            GU_FogRamp(ramp.nearZ, ramp.farZ);   /* (the fog runs' band) */
#ifdef SONICR_DEBUG_LOG
            g_trackObjWhole++;
#endif
            for (int g = 0; g < to->groupCount; g++) {
                const TrackGroup *grp = &s_groups[to->groupFirst + g];
                if (g_tpageStateArray[grp->tpage] != 0x04) {
                    continue;
                }
                queue_run((grp->tpage * 2 + grp->cull) * 2 + fog, grp->first, grp->count, to->origin);
#ifdef SONICR_DEBUG_LOG
                g_trackGeObjects += (unsigned int)grp->chunkCount;
#endif
            }
            return 0;
        }
    }
    /* the matrix for this object's next draw: NULL when the GE has it */
#define OBJ_MODEL() (part ? origin_model(to->origin) : modelSent ? NULL : (modelSent = 1, s_modelOrigin = -1, model))
    GuVertex *out = GU_TransformAlloc(to->vertCount);
    if (out == NULL) {
        return -1;
    }
    const FogRamp ramp = fog_ramp(farSafe);
    const float farClip = (float)farClipTimes8;
    /* inside the guard band: |projScale * cam / depth| <= GE_GUARD */
    const float gx = GE_GUARD / (float)g_projScaleXCurrent, gy = GE_GUARD / (float)g_projScaleY;
    int fogPasses = 1;
#ifdef SONICR_PROF
    extern int g_pspFogPasses;   /* (test, Select+L+Square: the fog band a vertex at a time instead) */
    fogPasses = g_pspFogPasses;
#endif
    GU_FogRamp(ramp.nearZ, ramp.farZ);
    const GuFrustum fr = s_frustum;
    int w = 0;
    for (int g = 0; g < to->groupCount; g++) {
        const TrackGroup *grp = &s_groups[to->groupFirst + g];
        if (g_tpageStateArray[grp->tpage] != 0x04) {
            continue;   /* (the desktop path's per-polygon check) */
        }
        R_SetTexture(grp->tpage);
        R_SetTexEnv(R_TEXENV_ADD_SIGNED);
        int runFirst = 0, runCount = 0, runFog = 0;   /* as-built chunks in a row */
        int triStart = w;
        for (int c = 0; c < grp->chunkCount; c++) {
            const TrackChunk *ch = &s_chunks[grp->chunkFirst + c];
            float cx = t->m[0][0] * ch->x + t->m[0][1] * ch->y + t->m[0][2] * ch->z + t->m[0][3];
            float cy = t->m[1][0] * ch->x + t->m[1][1] * ch->y + t->m[1][2] * ch->z + t->m[1][3];
            float cz = t->m[2][0] * ch->x + t->m[2][1] * ch->y + t->m[2][2] * ch->z + t->m[2][3];
            float zNear = cz - ch->r;
            if (cz + ch->r < 1.0f) {
                continue;   /* all behind the camera */
            }
#if TRACK_FULL_FOG_CULL
            if (zNear > ramp.farZ) {
                continue;   /* all past FOG_FAR */
            }
#endif
            if (GU_FrustumOutSphere(&fr, cx, cy, cz, ch->r)) {
                continue;   /* off the screen */
            }
            int clear = zNear >= GE_Z_MIN && fabsf(cx) + ch->r <= gx * zNear && fabsf(cy) + ch->r <= gy * zNear;
            int fog = cz + ch->r > ramp.nearZ;
            if (clear && (!fog || fogPasses)) {
#ifdef SONICR_DEBUG_LOG
                g_trackGeObjects++;
#endif
                if (part) {
                    queue_run((grp->tpage * 2 + grp->cull) * 2 + fog, ch->first, ch->count, to->origin);
                    continue;
                }
                if (runCount && runFog == fog && runFirst + runCount == ch->first) {
                    runCount += ch->count;
                } else {
                    if (runCount) {
                        if (runFog) {
                            GU_TransformDrawFogV(OBJ_MODEL(), grp->cull, frontFace, TRACK_VTYPE, &s_verts[runFirst],
                                                 runCount);
                        } else {
                            GU_TransformDrawV(OBJ_MODEL(), grp->cull, frontFace, TRACK_VTYPE, &s_verts[runFirst],
                                              runCount);
                        }
                    }
                    runFirst = ch->first;
                    runCount = ch->count;
                    runFog = fog;
                }
                continue;
            }

#ifdef SONICR_DEBUG_LOG
            g_trackCpuFog++;
#endif
            TRACK_FINE_T0();
            const TrackVertex *src = &s_verts[ch->first];
            for (int i = 0; i < ch->count; i += 3) {
                float x[3], y[3], z[3];
                int behind = 0, fogged = 0, beyond = 0, ge = 1;
                for (int k = 0; k < 3; k++) {
                    const TrackVertex *v = &src[i + k];
                    const float vx = (float)v->x, vy = (float)v->y, vz = (float)v->z;
                    x[k] = t->m[0][0] * vx + t->m[0][1] * vy + t->m[0][2] * vz + t->m[0][3];
                    y[k] = t->m[1][0] * vx + t->m[1][1] * vy + t->m[1][2] * vz + t->m[1][3];
                    z[k] = t->m[2][0] * vx + t->m[2][1] * vy + t->m[2][2] * vz + t->m[2][3];
                    behind += z[k] < 1.0f;
                    beyond += z[k] > farClip;
                    fogged += z[k] > ramp.farZ;
                    ge &= z[k] >= GE_Z_MIN && fabsf(x[k]) <= gx * z[k] && fabsf(y[k]) <= gy * z[k];
                }
                if (beyond || behind == 3 || GU_FrustumOutTri(&fr, x, y, z)) {
                    continue;
                }
#if TRACK_FULL_FOG_CULL
                if (fogged == 3) {
                    continue;
                }
#endif
                if (!ge) {
                    if (s_cpuTriCount < MAX_CPU_TRIS) {
                        CpuTri *cpu = &s_cpuTris[s_cpuTriCount++];
                        cpu->vert = ch->first + i;
                        cpu->oi = (unsigned short)oi;
                        cpu->tpage = grp->tpage;
                        cpu->cull = grp->cull;
                    }
                    continue;
                }
                for (int k = 0; k < 3; k++) {
                    /* (in floats, in the 16-bit ones' units: the model matrix is theirs) */
                    const TrackVertex *v = &src[i + k];
                    out[w].u = (float)v->u * (1.0f / TRACK_UNIT);
                    out[w].v = (float)v->v * (1.0f / TRACK_UNIT);
                    out[w].color = (v->color & 0x00FFFFFFu) | fog_alpha24(z[k], &ramp);
                    out[w].x = (float)v->x * (1.0f / TRACK_UNIT);
                    out[w].y = (float)v->y * (1.0f / TRACK_UNIT);
                    out[w].z = (float)v->z * (1.0f / TRACK_UNIT);
                    w++;
                }
            }
            TRACK_FINE_ADD(g_trackUsFog);
        }
        if (runCount) {
            if (runFog) {
                GU_TransformDrawFogV(OBJ_MODEL(), grp->cull, frontFace, TRACK_VTYPE, &s_verts[runFirst], runCount);
            } else {
                GU_TransformDrawV(OBJ_MODEL(), grp->cull, frontFace, TRACK_VTYPE, &s_verts[runFirst], runCount);
            }
        }
        if (w > triStart) {
            GU_TransformDrawFresh(OBJ_MODEL(), grp->cull, frontFace, &out[triStart], w - triStart);
        }
    }
#undef OBJ_MODEL
    GU_TransformUnalloc(to->vertCount - w);
    return 0;
}

/* =====================================================================
 * RenderTrackD3D
 * ===================================================================== */

#define MAX_NEAR 1024

/* The desktop pass's rings: every track part's sub-entries still to collect
 * (render_track_sdl.c, after its next_object), as RenderHiddenSubEntry
 * (render_hidden_obj.c) draws them — a square of ±RING_HALF in camera space
 * at the ring's depth, its frame of the 16 in the sprite page, faded by the
 * fog ramp — but in floats and all in one draw, the squares in camera space
 * through the GE's projection: its integer path took four divisions and a
 * draw a ring, a few hundred rings a frame. */
#define RING_HALF   12.0f      /* BILLBOARD_HALF */
#define RING_UV     0.0625f    /* UV_STEP */


static void track_rings(const ScePspFMatrix4 *proj, float farSafe, float farCull)
{
    int tp = g_tpageCharBase;
    if (g_raceType == RACE_TIMEATTACK || g_raceSubMode == SUBMODE_TAG || g_ringSpawnArray == NULL ||
        g_tpageStateArray[tp] != 4) {
        return;
    }
    /* the integer path's camera space: (dx, dz, dy) through g_viewMtx / 4096 */
    const float s = 1.0f / 4096.0f;
    const float m00 = g_viewMtx00 * s, m10 = g_viewMtx10 * s, m20 = g_viewMtx20 * s;
    const float m01 = g_viewMtx01 * s, m11 = g_viewMtx11 * s, m21 = g_viewMtx21 * s;
    const float m02 = g_viewMtx02 * s, m12 = g_viewMtx12 * s, m22 = g_viewMtx22 * s;
    const FogRamp ramp = fog_ramp(farSafe);
    const float u0 = (float)g_ringAnimFrame * RING_UV, u1 = u0 + RING_UV;
    GuVertex *first = NULL;
    int n = 0;
    /* the track parts' rings, listed once a track (walking every object for
     * them was a millisecond a frame) */
    if (s_ringCount < 0 || s_ringArray != g_ringSpawnArray) {
        free(s_rings);
        s_rings = NULL;
        s_ringCount = 0;
        s_ringArray = g_ringSpawnArray;
        int total = 0;
        const int *obj = (const int *)g_objectStructArray;
        for (int oi = 0; oi < g_totalObjects; oi++, obj += 17) {
            if (obj_mode(obj) == 0) {
                total += *(const unsigned short *)((const char *)obj + 0x36);
            }
        }
        s_rings = malloc((size_t)(total ? total : 1) * sizeof(unsigned short));
        if (s_rings == NULL) {
            return;
        }
        obj = (const int *)g_objectStructArray;
        for (int oi = 0; oi < g_totalObjects; oi++, obj += 17) {
            unsigned short subCount = *(const unsigned short *)((const char *)obj + 0x36);
            unsigned short subStart = *(const unsigned short *)((const char *)obj + 0x34);
            for (int ri = 0; ri < subCount && obj_mode(obj) == 0; ri++) {
                s_rings[s_ringCount++] = (unsigned short)(subStart + ri);
            }
        }
    }
    for (int k = 0; k < s_ringCount; k++) {
        const int *entry = (const int *)((const char *)g_ringSpawnArray + s_rings[k] * 16);
        if (entry[3] != 0) {
            continue;   /* (collected) */
        }
        float dx = (float)(entry[0] - g_camIntX), dz = (float)(entry[1] - g_camIntY);
        float dy = (float)(entry[2] - g_camIntZ);
        float z = m02 * dx + m12 * dz + m22 * dy;
        if (z < GE_Z_MIN || z > farCull || z > ramp.farZ) {
            continue;   /* (at the camera — short of the GE's depth range —, past the far plane, faded out) */
        }
        float x = m00 * dx + m10 * dz + m20 * dy;
        float y = m01 * dx + m11 * dz + m21 * dy;
        if (GU_FrustumOutSphere(&s_frustum, x, y, z, RING_HALF * 1.415f)) {
            continue;
        }
        GuVertex *v = GU_TransformAlloc(6);
        if (v == NULL) {
            break;
        }
        if (first == NULL) {
            first = v;
        }
        uint32_t colour = fog_alpha24(z, &ramp) | 0x00FFFFFFu;
        static const float cx[6] = { -1, 1, 1, -1, 1, -1 }, cy[6] = { 1, 1, -1, 1, -1, -1 };
        for (int c = 0; c < 6; c++) {
            v[c].x = x + cx[c] * RING_HALF;
            v[c].y = y + cy[c] * RING_HALF;
            v[c].z = z;
            v[c].u = cx[c] < 0 ? u0 : u1;
            v[c].v = cy[c] > 0 ? 0.0f : RING_UV;
            v[c].color = colour;
        }
        n += 6;
    }
    if (n == 0) {
        return;
    }
    ScePspFMatrix4 identity;
    memset(&identity, 0, sizeof(identity));
    identity.x.x = identity.y.y = identity.z.z = identity.w.w = 1.0f;
    GU_TransformBegin(&identity, proj);
    R_SetTexture(tp);
    R_SetTexEnv(R_TEXENV_MODULATE);
    GU_TransformDrawFresh(&identity, 0, GU_CW, first, n);
    GU_TransformEnd();
}

void RenderTrackD3D(void)
{
    ensure_built();
    GU_TrackTextures(1);
    if (!s_built || g_totalObjects != s_objCount) {
        RenderTrackD3D_CPU();
        GU_TrackTextures(0);
        return;
    }

    int *objBase = (int *)g_objectStructArray;
    int farClipTimes8 = (g_farClipDepth << 3) > 0 ? (g_farClipDepth << 3) : 0x15800;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 88064.0f;
    int frontFace = g_mirrorMode ? GU_CCW : GU_CW;
#ifdef SONICR_PROF
    extern int g_pspTrackDynamic;   /* (test: Select+L+Up every object from the current data) */
#endif

    static int s_nearIdx[MAX_NEAR];
    int nears = 0;

    ScePspFMatrix4 view, proj, identity, deco;
    view_matrix(&view);
    GU_CameraProjection(&proj);
    memset(&identity, 0, sizeof(identity));
    identity.x.x = identity.y.y = identity.z.z = identity.w.w = 1.0f;
    Affine viewA = affine_from(&view);
    int began = 0;
    s_cpuTriCount = 0;
    s_modelOrigin = -1;
    s_runFrontFace = frontFace;
    s_runActiveCount = 0;
    GU_FrustumInit(&s_frustum, (float)g_projScaleXCurrent, (float)g_projScaleY, (float)g_screenCenterX,
                   (float)g_screenCenterY, (float)g_clipLeft, (float)g_clipRight, (float)g_clipTop,
                   (float)g_clipBottom);

    /* The safety net: a few objects a frame checked against what their GE
     * vertices were built from, for changes no TRACK_POLY_CHANGED reported
     * (every object within ~8 seconds at 60 fps; 8 a frame took 0.4 ms). */
    static int s_checkNext;
    TRACK_T0();
    for (int k = 0; k < 2; k++) {
        int oi = s_checkNext;
        s_checkNext = (s_checkNext + 1) % s_objCount;
        TrackObj *to = &s_objs[oi];
        if (to->hashStale) {
            to->hash = object_hash(oi);   /* (its polygons were updated in place) */
            to->hashStale = 0;
        } else if (!to->dynamic && to->vertCount && !to->dirty && object_hash(oi) != to->hash) {
            to->dirty = 1;
            static int s_reported;
            if (s_reported < 16) {
                s_reported++;
                DebugLog("render_track_psp: object %d changed with no TRACK_POLY_CHANGED\n", oi);
            }
        }
    }
    TRACK_ADD(g_trackUsCheck);

#ifdef SONICR_DEBUG_LOG
    unsigned int tPass1 = sceKernelGetSystemTimeLow();
#endif
    /* pass 1: every object on the GE, the triangles it can't take noted */
    const float farCull = (float)farClipTimes8;
    for (int oi = 0; oi < s_objCount && nears < MAX_NEAR; oi++) {
        int *obj = objBase + oi * 17;
        TrackObj *to = &s_objs[oi];
        if (obj_vis(obj) == -1 || to->br < 0.0f) {
            continue;   /* (hidden by the game; no polygons) */
        }
        int mode = obj_mode(obj);

        /* its real sphere (TrackObj) in the view the GE will use: behind the
         * camera, past the far plane, off the screen */
        float wx = to->bx, wy = to->by, wz = to->bz;
        if (mode == 2) {
            wx += (float)obj[8];
            wy += (float)obj[9];
            wz += (float)obj[10];
        }
        float vcz = view.x.z * wx + view.y.z * wy + view.z.z * wz + view.w.z;
        if (vcz + to->br < 1.0f || vcz - to->br > farCull) {
            continue;
        }
        float vcx = view.x.x * wx + view.y.x * wy + view.z.x * wz + view.w.x;
        float vcy = view.x.y * wx + view.y.y * wy + view.z.y * wz + view.w.y;
        if (GU_FrustumOutSphere(&s_frustum, vcx, vcy, vcz, to->br)) {
            continue;
        }
        if (to->pending || to->dirty) {
            TRACK_T0();
            if (to->pending) {
                apply_object_updates(oi);
            }
            if (to->dirty) {
                rebuild_object(oi);
            }
            to->emeraldPhase = 0;   /* (written from the source colours again) */
            TRACK_ADD(g_trackUsRebuild);
        }
        if (g_trackId == TRACK_RADIANT_EMERALD) {
            emerald_recolour(oi);
        }

        /* every polygon past FOG_FAR: the desktop path's fog cull skips them all */
#if TRACK_FULL_FOG_CULL
        if (vcz - to->br > (float)FOG_FAR * farSafe) {
            continue;
        }
#endif

        /* each object's first draw sets its model matrix */
        const ScePspFMatrix4 *model = &identity;
        Affine t = viewA;
        if (mode == 2) {
            decoration_matrix(obj, &deco);
            model = &deco;
            Affine modelA = affine_from(&deco);
            t = affine_mul(&viewA, &modelA);
        }
        if (!began) {
            GU_TransformBegin(&view, &proj);
            if (g_colorTintEnable) {
                GU_Tint(g_colorTintR >> 13, g_colorTintG >> 13, g_colorTintB >> 13);
            }
            began = 1;
        }

        int fromData = to->dynamic;
#ifdef SONICR_PROF
        fromData |= g_pspTrackDynamic;
#endif
        if (fromData) {
            /* its layout changed: drawn from the current data, a polygon at
             * a time — on the CPU at the camera (pass 2), else with fog alpha */
#ifdef SONICR_DEBUG_LOG
            g_trackCpuGuard++;
#endif
            float nearZ = vcz - to->br;
            int atCamera = nearZ < (float)GE_NEAR_MIN;
            if (!atCamera) {
                float reachX = (float)g_projScaleXCurrent * (fabsf(vcx) + to->br) / nearZ;
                float reachY = (float)g_projScaleY * (fabsf(vcy) + to->br) / nearZ;
                atCamera = reachX > GE_GUARD || reachY > GE_GUARD;
            }
            if (atCamera || fog_draw_object(obj, model, &t, farSafe, farClipTimes8, frontFace) < 0) {
                s_nearIdx[nears++] = oi;
            } else {
                g_processedObjectCount++;
            }
            s_modelOrigin = -1;   /* (whatever it drew with) */
            continue;
        }

        /* the built vertices: a part's from its origin, a decoration's in its
         * matrix with S(32768) (16 bits, / 32768 to the GE) */
        ScePspFMatrix4 decoS;
        const ScePspFMatrix4 *builtModel = NULL;
        Affine builtT = t;
        if (mode == 2) {
            decoS = deco;
            decoS.x.x *= TRACK_UNIT; decoS.x.y *= TRACK_UNIT; decoS.x.z *= TRACK_UNIT;
            decoS.y.x *= TRACK_UNIT; decoS.y.y *= TRACK_UNIT; decoS.y.z *= TRACK_UNIT;
            decoS.z.x *= TRACK_UNIT; decoS.z.y *= TRACK_UNIT; decoS.z.z *= TRACK_UNIT;
            builtModel = &decoS;
        } else {
            builtT = object_affine(oi, &viewA);
        }
        TRACK_FINE_T0();
#if defined(SONICR_TEST) && SONICR_TEST == 1
        int drawn = 0;   /* (experiment: the loop alone) */
        (void)builtModel;
#else
        const float sphere[4] = { vcx, vcy, vcz, to->br };   /* (a part's, in the view) */
#ifdef SONICR_DEBUG_LOG
        g_trackObjSeen++;
        g_trackDecoDrawn += mode == 2;
#endif
        int drawn = draw_object(oi, builtModel, &builtT, farSafe, farClipTimes8, frontFace, mode == 2 ? NULL : sphere);
#endif
        TRACK_FINE_ADD(g_trackUsDrawObj);
        if (drawn < 0) {
            s_nearIdx[nears++] = oi;   /* (no room in the arena: on the CPU, pass 2) */
        } else {
            g_processedObjectCount++;
        }
    }
    if (began) {
        flush_runs();
        GU_Tint(0, 0, 0);
        GU_TransformEnd();
    }

#ifdef SONICR_DEBUG_LOG
    unsigned int tPass2 = sceKernelGetSystemTimeLow();
    g_trackUsPass1 += tPass2 - tPass1;
    g_trackCpuNear += s_cpuTriCount;
#endif
    /* pass 2: on the CPU, in the GE's float transform — the triangles noted,
     * and the objects that went whole */
    near_draw_cpu_tris(objBase, &viewA, farSafe, farClipTimes8);
    for (int i = 0; i < nears; i++) {
        int oi = s_nearIdx[i];
        int *obj = objBase + oi * 17;
        Affine t = viewA;
        if (obj_mode(obj) == 2) {
            decoration_matrix(obj, &deco);
            Affine modelA = affine_from(&deco);
            t = affine_mul(&viewA, &modelA);
        }
        if (s_objs[oi].dynamic) {
            near_draw_object(obj, &t, farSafe, farClipTimes8);
        } else {
            Affine built = object_affine(oi, &viewA);   /* (from its vertices' origin) */
            near_draw_built(oi, &built, farSafe, farClipTimes8);
        }
        g_processedObjectCount++;
    }

#ifdef SONICR_DEBUG_LOG
    unsigned int tPass3 = sceKernelGetSystemTimeLow();
    g_trackUsNear += tPass3 - tPass2;
#endif
    /* pass 3: the rings, all the desktop pass has left to draw */
    GU_TrackTextures(0);
    {
        TRACK_T0();
        track_rings(&proj, farSafe, (float)farClipTimes8);
        TRACK_ADD(g_trackUsDesktop);
    }
}
