/**
 * prof_wrap.c — section timers for the race frame, without touching the game
 * code: `make PROF=1` links with -Wl,--wrap for the functions below, so the
 * calls the game makes from other files land here, are timed, and go on to
 * the real function. (A call made inside the function's own file is not
 * wrapped — those are counted in their caller.)
 *
 * Reported every 60 frames by FlipD3D (render_gu.c): milliseconds per frame
 * in each section, and the whole render (RenderHUD) against the frame's CPU.
 */

#ifdef SONICR_PROF

#include <pspkernel.h>
#include <stdio.h>
#include <string.h>

enum {
    P_RENDER, P_SKY, P_PARALLAX, P_WATER, P_GRID, P_TRACK, P_CHARS, P_SNOW, P_PARTICLES, P_COUNT
};
static const char *const s_names[P_COUNT] = {
    "render", "sky", "parallax", "water", "grid", "track", "chars", "snow", "particles"
};
static unsigned int s_us[P_COUNT];

#define WRAP_VOID(name, slot, params, args)               \
    void __real_##name params;                            \
    void __wrap_##name params                             \
    {                                                     \
        unsigned int t0 = sceKernelGetSystemTimeLow();    \
        __real_##name args;                               \
        s_us[slot] += sceKernelGetSystemTimeLow() - t0;   \
    }

/* (test, Select+L+Triangle: these three skipped — platform_psp.c) */
int g_pspNoBackground;

#define WRAP_BACKGROUND(name, slot, params, args)         \
    void __real_##name params;                            \
    void __wrap_##name params                             \
    {                                                     \
        if (g_pspNoBackground) {                          \
            return;                                       \
        }                                                 \
        unsigned int t0 = sceKernelGetSystemTimeLow();    \
        __real_##name args;                               \
        s_us[slot] += sceKernelGetSystemTimeLow() - t0;   \
    }

WRAP_VOID(RenderHUD, P_RENDER, (void), ())
WRAP_VOID(ComputeParallaxAndPlayfieldState, P_SKY, (int vpIdx, int *cam), (vpIdx, cam))
WRAP_BACKGROUND(RenderParallaxStripsD3D, P_PARALLAX, (int vpIdx), (vpIdx))
WRAP_BACKGROUND(RenderWaterReflectionD3D, P_WATER, (int vpIdx), (vpIdx))
WRAP_BACKGROUND(RenderPlayfieldGridD3D, P_GRID, (float *baseVerts, float *upperVerts, int *vpConfig),
                (baseVerts, upperVerts, vpConfig))
WRAP_VOID(RenderTrackD3D, P_TRACK, (void), ())
WRAP_VOID(RenderAllCharacterModels, P_CHARS, (int numPlayers), (numPlayers))
WRAP_VOID(DrawSnowParticlesGate, P_SNOW, (int viewportIdx), (viewportIdx))
WRAP_VOID(DrawOtherParticles, P_PARTICLES, (void), ())

/* FlipD3D, every `frames` frames: the sections' ms per frame. */
void PSP_ProfReport(unsigned int frames, unsigned int cpuUs)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "    sections (ms/frame): logic+rest %.2f |",
                     (cpuUs > s_us[P_RENDER] ? cpuUs - s_us[P_RENDER] : 0) / 1000.0f / frames);
    for (int i = 0; i < P_COUNT && n < (int)sizeof(line) - 24; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %s %.2f", s_names[i], s_us[i] / 1000.0f / frames);
    }
    fprintf(stderr, "%s\n", line);
    memset(s_us, 0, sizeof(s_us));
}

#endif /* SONICR_PROF */
