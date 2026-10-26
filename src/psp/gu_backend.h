/**
 * gu_backend.h — what render_gu.c offers the PSP's own renderers besides the
 * R_* API: its vertex format, and drawing with the GE's transform (geometry
 * kept in model space, a matrix per object) instead of pre-transformed
 * vertices. render_track_psp.c draws the track this way.
 */
#ifndef GU_BACKEND_H
#define GU_BACKEND_H

#include <pspgu.h>
#include <stdint.h>
#include <math.h>

typedef struct {
    float u, v;
    uint32_t color;      /* ABGR */
    float x, y, z;
} GuVertex;

#define GU_VTYPE (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

/* Frames flipped so far. */
unsigned int GU_FrameNo(void);

/* The projection of the game's camera space (camX, camY, depth — what the
 * track's integer transform produces) onto the screen, as the CPU path's
 * NearClipVert_ProjectFloat does, with the same depth as its vertices (1/w,
 * render_gu.c), so both paths' geometry depth-tests against each other. */
void GU_CameraProjection(ScePspFMatrix4 *out);

/* Draw in the GE's transform: Begin sends the open batch and sets view and
 * projection, Draw uses the current R_* state (texture, blend...) and the
 * given model matrix (NULL: keep the last one), End puts back what the
 * pre-transformed path uses. */
void GU_TransformBegin(const ScePspFMatrix4 *view, const ScePspFMatrix4 *proj);
void GU_TransformDraw(const ScePspFMatrix4 *model, int cull, int frontFace, const GuVertex *verts, int count);
/* the same, the vertices in another format (vtype, with GU_TRANSFORM_3D) */
void GU_TransformDrawV(const ScePspFMatrix4 *model, int cull, int frontFace, int vtype, const void *verts, int count);
void GU_TransformEnd(void);

/* Draw with the GE's skinning: each vertex nb 8-bit weights (1 to 8; 0x80 is
 * 1), padded to a word, before a GuVertex's fields, the bones' matrices the
 * model's (the model matrix left as it is, the identity for the characters). */
void GU_TransformDrawSkinned(const ScePspFMatrix4 *bones, int nb, int cull, int frontFace, const void *verts,
                             int count);

/* Vertices written this frame (between Begin and End): room in the frame's
 * vertex arena (NULL if it is full), drawn with DrawFresh, which writes them
 * out of the CPU's cache first. */
GuVertex *GU_TransformAlloc(int count);
void GU_TransformUnalloc(int count);   /* the last Alloc's unused tail */
void GU_TransformDrawFresh(const ScePspFMatrix4 *model, int cull, int frontFace, GuVertex *verts, int count);

/* Geometry reaching into the fog band, drawn as built: the game's fade by
 * vertex alpha (1 up to nearZ of camera depth, 0 from farZ on, linear
 * between), done by the GE in two passes. GU_FogRamp sets the ramp for the
 * DrawFog calls after it; they take the current R_* state like Draw. */
void GU_FogRamp(float nearZ, float farZ);

/* The lightning flash: r, g, b (0..255) added to the vertex colours of the
 * Draw and DrawFog calls after it, clamped at white; 0, 0, 0 for none. */
void GU_Tint(int r, int g, int b);

/* GU_CameraProjection, but depth-tested as if k times as far (k >= 1) */
void GU_CameraProjectionPushed(ScePspFMatrix4 *out, float k);
void GU_TransformDrawFog(const ScePspFMatrix4 *model, int cull, int frontFace, const GuVertex *verts, int count);
void GU_TransformDrawFogV(const ScePspFMatrix4 *model, int cull, int frontFace, int vtype, const void *verts,
                          int count);

/* The screen's four edges as planes through the eye, in the game's camera
 * space (camX, camY, depth): a point is inside where a*x + b*y + c*z >= 0
 * for all four (no divide, and right for points behind the eye too). From
 * the game's projection (screen = centre + projScale * cam / depth, y down)
 * and its clip rectangle. */
typedef struct {
    float a[4], b[4], c[4];
} GuFrustum;

static inline void GU_FrustumInit(GuFrustum *f, float projX, float projY, float centreX, float centreY,
                                  float left, float right, float top, float bottom)
{
    float p[4][3] = {
        { projX, 0.0f, centreX - left },     /* x >= left */
        { -projX, 0.0f, right - centreX },   /* x <= right */
        { 0.0f, -projY, centreY - top },     /* y >= top */
        { 0.0f, projY, bottom - centreY },   /* y <= bottom */
    };
    for (int i = 0; i < 4; i++) {
        float n = 1.0f / sqrtf(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
        f->a[i] = p[i][0] * n;
        f->b[i] = p[i][1] * n;
        f->c[i] = p[i][2] * n;
    }
}

/* A sphere wholly inside all four edges */
static inline int GU_FrustumInSphere(const GuFrustum *f, float x, float y, float z, float r)
{
    for (int i = 0; i < 4; i++) {
        if (f->a[i] * x + f->b[i] * y + f->c[i] * z < r) {
            return 0;
        }
    }
    return 1;
}

/* A sphere wholly outside one of the edges */
static inline int GU_FrustumOutSphere(const GuFrustum *f, float x, float y, float z, float r)
{
    for (int i = 0; i < 4; i++) {
        if (f->a[i] * x + f->b[i] * y + f->c[i] * z < -r) {
            return 1;
        }
    }
    return 0;
}

/* Three points all outside the same edge */
static inline int GU_FrustumOutTri(const GuFrustum *f, const float x[3], const float y[3], const float z[3])
{
    for (int i = 0; i < 4; i++) {
        if (f->a[i] * x[0] + f->b[i] * y[0] + f->c[i] * z[0] < 0.0f &&
            f->a[i] * x[1] + f->b[i] * y[1] + f->c[i] * z[1] < 0.0f &&
            f->a[i] * x[2] + f->b[i] * y[2] + f->c[i] * z[2] < 0.0f) {
            return 1;
        }
    }
    return 0;
}

/* Textures from their full-size level only, until turned off again (the
 * track: its atlases' tiles bleed into each other at level 1). */
void GU_TrackTextures(int on);

/* The texture pages to keep in VRAM: who (GU_VRAM_TRACK, GU_VRAM_GRID) says
 * how much it reads each (its vertices on it) after a reset of its own; the
 * heaviest of them all get the slots at GU_VramCommit. */
#define GU_VRAM_TRACK 0
#define GU_VRAM_GRID  1
void GU_VramReset(int who);
void GU_VramWant(int who, int tp, unsigned int weight);
void GU_VramCommit(void);

/* At a race's start, before its builds: every loaded page made for the GE,
 * the pixels the game doesn't keep let go (render_gu.c) */
void GU_TexturesSettle(void);

#endif /* GU_BACKEND_H */
