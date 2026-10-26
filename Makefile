# Sonic R PSP build (pspdev)
#
# Build from this directory with the PSP toolchain on PATH, e.g. (PowerShell):
#   $env:PATH = "C:\pspdev\bin;C:\msys64\usr\bin;C:\msys64\ucrt64\bin;$env:PATH"
#   make -j8
#
# The game is the decompilation's SDL build (src/sdl, see src/sdl/ORIGIN.md),
# with PSP-specific replacements in src/psp for the renderer, platform and
# sound, the same split the decompilation uses for its Dreamcast build. The PSP
# compiles the desktop code paths (no SONICR_DC): #ifdef SONICR_PSP covers the rest.
#
# Only relative paths are used on purpose: the workspace path can contain
# non-ASCII characters, which the native Windows toolchain cannot receive
# from msys make.

.DEFAULT_GOAL := all

PSP_SRCDIR := src/psp
SDL_SRCDIR := src/sdl
BUILDDIR   := build/obj$(if $(filter 1,$(GPROF)),_gprof)$(if $(filter 1,$(PROF)),_prof)

# What's built goes into build/: SonicR.elf (and .prx), PARAM.SFO, EBOOT.PBP.
OUTDIR        := build
TARGET        := $(OUTDIR)/SonicR
PSP_EBOOT     := $(OUTDIR)/EBOOT.PBP
PSP_EBOOT_SFO := $(OUTDIR)/PARAM.SFO

# Files in src/sdl that the PSP replaces with its own version (src/psp), or
# leaves out: the desktop track and grid renderers (src/psp/render_track_psp.c
# and render_grid_psp.c include them), the GL/SDL renderer and platform, SDL_mixer sound, and the online
# matchmaker's HTTPS + UPnP (matchmaker.c itself stays: without
# SONICR_MATCHMAKER it compiles to its offline stubs). The LAN transport
# (net_transport.c) stays: the PSP's newlib has BSD sockets.
SDL_REPLACED := \
    $(SDL_SRCDIR)/render_track_sdl.c \
    $(SDL_SRCDIR)/render_grid_d3d.c \
    $(SDL_SRCDIR)/r_gl_backend.c \
    $(SDL_SRCDIR)/render_gl.c \
    $(SDL_SRCDIR)/r_soft_backend.c \
    $(SDL_SRCDIR)/platform/platform_sdl.c \
    $(SDL_SRCDIR)/sound/music_sdl.c \
    $(SDL_SRCDIR)/sound/sfx_sdl.c \
    $(SDL_SRCDIR)/sound/music_rwops.c \
    $(SDL_SRCDIR)/sound/adp.c \
    $(SDL_SRCDIR)/sound/adx.c \
    $(SDL_SRCDIR)/net/net_https.c \
    $(SDL_SRCDIR)/net/net_upnp.c

SDL_ALL_C := $(shell find $(SDL_SRCDIR) -name '*.c' ! -path '*/bearssl/*' ! -path '*/gles2/*')
SDL_C     := $(filter-out $(SDL_REPLACED), $(SDL_ALL_C))
PSP_C     := $(shell find $(PSP_SRCDIR) -name '*.c')

# Objects mirror the sources under build/obj: 'src/sdl/X.c' -> 'build/obj/sdl/X.o',
# 'src/psp/Y.c' -> 'build/obj/psp/Y.o'.
SDL_OBJS := $(patsubst $(SDL_SRCDIR)/%.c,$(BUILDDIR)/sdl/%.o,$(SDL_C))
PSP_OBJS := $(patsubst $(PSP_SRCDIR)/%.c,$(BUILDDIR)/psp/%.o,$(PSP_C))
OBJS     := $(SDL_OBJS) $(PSP_OBJS)

# Debug logging (DebugLog/DebugLogAlt, see src/sdl/util.c): `make DEBUG_LOG=1`.
DEBUG_LOG ?= 0
ifeq ($(DEBUG_LOG),1)
DEBUG_LOG_CFLAGS = -DSONICR_DEBUG_LOG
endif
# DEMO=n (with DEBUG_LOG=1): the title's attract mode plays demo n every time (save.c's s_demoFiles: 0 Island ...
# 8 Radiant Emerald) — a track to test without unlocking it. (Not tracked: touch src/sdl/save.c when it changes.)
ifneq ($(DEMO),)
DEBUG_LOG_CFLAGS += -DSONICR_PSP_DEMO=$(DEMO)
endif
# SIM24=1: the heap capped at what a PSP-1000 leaves, on a PSP with more (src/psp/platform_psp.c). (Not tracked
# either: touch src/psp/platform_psp.c.)
ifeq ($(SIM24),1)
DEBUG_LOG_CFLAGS += -DSONICR_PSP_SIM24
endif
# UNLOCK=1: every character but Super Sonic and Radiant Emerald selectable, in memory (sonicr_globals.h) — a
# test build's; nothing saved unless the Load/Save screen is used. (touch src/sdl/screen_charsel.c, screen_course.c.)
ifeq ($(UNLOCK),1)
DEBUG_LOG_CFLAGS += -DSONICR_PSP_UNLOCK_ALL
endif
# TEST=n: an experiment's switch, read where one is being tried (SONICR_TEST; touch the file it's read in).
ifneq ($(TEST),)
DEBUG_LOG_CFLAGS += -DSONICR_TEST=$(TEST)
endif
# INTERP_EXTRA=0: the halfway frame without the poses, decorations and particles (src/psp/interp_psp.c; an A/B
# test of their cost — touch it too).
ifneq ($(INTERP_EXTRA),)
DEBUG_LOG_CFLAGS += -DINTERP_EXTRA=$(INTERP_EXTRA)
endif
# MTRACE=1: the allocations of 32 KB or more in the log, with their callers (src/psp/mtrace_psp.c). (touch it too.)
ifeq ($(MTRACE),1)
DEBUG_LOG_CFLAGS += -DSONICR_MTRACE
LDFLAGS += -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign -Wl,--wrap=free
endif

# GPROF=1: sampled profile (pspprof). Select+L+Up starts it over, Select+L+Down writes gmon.out next to the EBOOT;
# read it with `psp-gprof -p -b build/SonicR.elf gmon.out` (call counts and tiny functions' times are inflated by -pg).
# Its objects go to a build dir of their own (BUILDDIR above).
ifeq ($(GPROF),1)
GPROF_CFLAGS = -pg -DSONICR_GPROF
endif

# PROF=1: section timers in the log (src/psp/prof_wrap.c), the game's render functions timed through --wrap; with
# DEBUG_LOG=1. Select+L+Left turns sending the draws to the GE off and on (what the backend costs per draw).
PROF_WRAPPED = RenderHUD ComputeParallaxAndPlayfieldState RenderParallaxStripsD3D RenderWaterReflectionD3D \
               RenderPlayfieldGridD3D RenderTrackD3D RenderAllCharacterModels DrawSnowParticlesGate DrawOtherParticles
comma := ,
# src/psp/render_gu.c's sceGuGetMemory replaces pspgu's in every build (the display list is written through the cache:
# pspgu's moves the GE's stall address past commands not yet written out of it)
LDFLAGS += -Wl,--wrap=sceGuGetMemory
ifeq ($(PROF),1)
PROF_CFLAGS = -DSONICR_PROF
LDFLAGS += $(addprefix -Wl$(comma)--wrap=,$(PROF_WRAPPED))
endif

# BUILD_PRX=1: relocatable module, needed to run under PSPLink (which refuses static ELFs). The PSP loader pairs each
# %hi relocation with the %lo that follows it, so keep them adjacent (explicit relocs let GCC hoist and spill a %hi).
# (The exports, src/psp/exports.exp, are copied into the build dir: the SDK makes exports.c and .o next to them.)
ifeq ($(BUILD_PRX),1)
PRX_CFLAGS = -mno-explicit-relocs -DSONICR_PRX
PRX_EXPORTS = $(BUILDDIR)/exports.exp
endif

OPT ?= -O2

# SONICR_FLOAT32: the game's sr_double is float. The PSP's FPU is single precision only, so a double is a libgcc
# call; -fsingle-precision-constant keeps unsuffixed literals from promoting float math to double.
INCDIR = $(PSP_SRCDIR) $(SDL_SRCDIR)
CFLAGS = $(OPT) -G0 -std=gnu11 -fno-common -fno-strict-aliasing $(if $(GPROF_CFLAGS),,-fomit-frame-pointer) -ffast-math \
         -fsingle-precision-constant -mno-check-zero-division $(PRX_CFLAGS) \
         -Wall -Wno-unused -Wno-parentheses -Wno-misleading-indentation -Wno-array-bounds \
         -Wno-maybe-uninitialized -Wno-type-limits -Wno-stringop-overflow -Wno-implicit-fallthrough \
         -Wno-pointer-sign -Wno-stringop-truncation -Wno-format-truncation \
         -DSONICR_SDL -DSONICR_PSP -DSONICR_FLOAT32 -DDATA_DIR='"."' $(DEBUG_LOG_CFLAGS) $(GPROF_CFLAGS) $(PROF_CFLAGS)
ASFLAGS = $(CFLAGS)

LIBS = $(if $(filter 1,$(GPROF)),-lpspprof) -lpspatrac3 -lpsputility -lpspgum -lpspgu -lpspaudio -lpsppower -lpsprtc -lm

EXTRA_TARGETS   = $(PSP_EBOOT)
PSP_EBOOT_TITLE = Sonic R
# icon (144x80), background (480x272) and menu sound of the XMB: used when present in assets/ (see assets/README.md;
# not in the repository: they would be the game's art and music)
XMB_ASSET = $(or $(firstword $(wildcard assets/$(1) assets/$(2))),NULL)
PSP_EBOOT_ICON  = $(call XMB_ASSET,ICON0.PNG,ICON0.png)
PSP_EBOOT_PIC1  = $(call XMB_ASSET,PIC1.PNG,PIC1.png)
PSP_EBOOT_SND0  = $(call XMB_ASSET,SND0.AT3,SND0.at3)
PSP_LARGE_MEMORY = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

$(BUILDDIR)/sdl/%.o: $(SDL_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/psp/%.o: $(PSP_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/exports.exp: $(PSP_SRCDIR)/exports.exp
	@mkdir -p $(dir $@)
	cp $< $@

$(PSP_EBOOT_SFO): | $(OUTDIR)
$(OUTDIR):
	mkdir -p $@

EXTRA_CLEAN += $(BUILDDIR)
