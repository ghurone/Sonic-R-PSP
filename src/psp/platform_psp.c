/**
 * platform_psp.c — PSP implementation of platform.h.
 *
 * PSP counterpart to sdl/src/platform/platform_sdl.c: the module header, the
 * exit/power callbacks, timing, and the pad. The renderer (GU) lives in
 * render_gu.c and the sound in audio_psp.c — this file only handles the
 * non-render platform concerns.
 *
 * The pad is handed to the game as one SDL_GameController-shaped device in
 * slot 0, so the desktop defaults in InitJoystickConfig (init.c) and the remap
 * UI apply unchanged: Cross is A (south), Circle B, Square X, Triangle Y.
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psprtc.h>
#include <pspsdk.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pspdebug.h>
#include <pspgu.h>
#include <pspge.h>
#include <unistd.h>
#ifdef SONICR_GPROF
#include <pspprof.h>
#endif
#include <malloc.h>
#include "platform.h"
#include "pad_bits.h"
#include "gamepad_buttons.h"
#include "sonicr_functions.h"

PSP_MODULE_INFO("SonicR", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
/* All of user memory but 4MB: the threads' stacks, and what the AV modules
 * (sceUtilityLoadModule) and the ATRAC3 decoder allocate outside the heap. */
#ifdef SONICR_PSP_SIM24
/* (test builds, psp/Makefile SIM24=1: the heap a PSP-1000 leaves, on a PSP
 * with more — its 24 MB of user memory less the module (psp-size SonicR.elf:
 * 10935 KB; update with it), the 4 MB above and a margin for what the system
 * takes there. The runtime took a negative size larger than 4 MB as -4096.) */
PSP_HEAP_SIZE_KB(24576 - 10935 - 4096 - 472);
#else
PSP_HEAP_SIZE_KB(-4096);
#endif

/* The game's keyboard state (DIK_* indices). The PSP has no keyboard; main.c
 * copies this into g_diKeyboardState every frame, so it stays all zeros. */
unsigned char s_keystate[256];

/* =====================================================================
 * Startup: before main()
 * ===================================================================== */

static int s_platformUp;
static void startup_failure_screen(void);
/* libcglue's descriptor table (fdman.h): only each entry's first field,
 * the kernel's descriptor, is read here */
typedef struct { int descriptor; } DescriptorHead;
extern DescriptorHead *__descriptormap[1024];
static SceUID s_mainThread = -1, s_callbackThread = -1, s_logThread = -1;
#ifdef SONICR_DEBUG_LOG
static SceUID s_logFd;   /* (the log, below) */
static void log_write_out(void);
#endif
static SceUID s_packFd;   /* (SONICR.PAK, below) */

/* Real hardware traps invalid float ops, which the game (PC semantics) does
 * not expect; PPSSPP does not trap. Mask them before any game code runs, and
 * clock the CPU up for the game. */
__attribute__((constructor)) static void platform_psp_early(void)
{
    pspSdkDisableFPUExceptions();
    scePowerSetClockFrequency(333, 333, 166);
    atexit(startup_failure_screen);
    s_mainThread = sceKernelGetThreadId();
}

#ifdef SONICR_PRX
/* PSPLink's modstop: give back everything the game holds, so modunld and
 * the next build's load work without resetting PSPLink — the threads (this
 * runs in the module manager's), the audio channel and the music file, the
 * GE's queue (the stopped frame's list stalls it: every later list, the next
 * build's too, would wait behind it) and callbacks (sceGuTerm: an interrupt
 * into an unloaded module would crash), the log file (left open, the next
 * build can't open it), the heap. */
extern void PSPAudio_Kill(void);
extern void __psp_free_heap(void);
int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    SceUID threads[3] = { s_mainThread, s_callbackThread, s_logThread };
    for (int i = 0; i < 3; i++) {
        if (threads[i] >= 0) {
            sceKernelTerminateDeleteThread(threads[i]);
        }
    }
    PSPAudio_Kill();
#ifdef SONICR_DEBUG_LOG
    if (s_logFd >= 0) {   /* (the log's last second; a kernel descriptor, not libcglue's below) */
        log_write_out();
        sceIoClose(s_logFd);
        s_logFd = -1;
    }
#endif
    if (s_packFd >= 0) {   /* (the pack's: a kernel descriptor too) */
        sceIoClose(s_packFd);
        s_packFd = -1;
    }
    PspGeBreakParam brk;
    sceGeBreak(1, &brk);
    sceGuTerm();
    /* Every file the game still holds, closed in the kernel (the log, data
     * files a stopped load left open...): the kernel doesn't close a module's
     * files when it unloads, and they pile up until every open fails — a
     * track file then reads as garbage and the load overruns its buffers.
     * Straight from libcglue's descriptor table, without its locks: a thread
     * stopped above may hold one. */
    {
        SceUID tty[3] = { sceKernelStdin(), sceKernelStdout(), sceKernelStderr() };
        for (int fd = 0; fd < 1024; fd++) {
            /* (the log is stderr's number: all but the kernel's own terminals) */
            if (__descriptormap[fd] && __descriptormap[fd]->descriptor >= 0 &&
                __descriptormap[fd]->descriptor != tty[0] && __descriptormap[fd]->descriptor != tty[1] &&
                __descriptormap[fd]->descriptor != tty[2]) {
                if (sceIoClose(__descriptormap[fd]->descriptor) < 0) {
                    sceIoDclose(__descriptormap[fd]->descriptor);
                }
            }
        }
    }
    __psp_free_heap();
    return 0;
}
#endif

/* =====================================================================
 * Exit / power callbacks
 * ===================================================================== */

static volatile int s_exitRequested;

/* Sleep mode closes every file the game holds open (the music stream): their
 * handles fail after it. Counted on each resume so audio_psp.c reopens it. */
volatile int g_pspResumeCount;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    s_exitRequested = 1;
    return 0;
}

static int power_callback(int unknown, int powerInfo, void *common)
{
    (void)unknown; (void)common;
    if (powerInfo & PSP_POWER_CB_RESUME_COMPLETE) {
        g_pspResumeCount++;
    }
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("exit_cb", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    int pcbid = sceKernelCreateCallback("power_cb", power_callback, NULL);
    if (scePowerRegisterCallback(-1, pcbid) < 0) {
        scePowerRegisterCallback(0, pcbid);   /* older firmware: no "any free slot" */
    }
    sceKernelSleepThreadCB();
    return 0;
}

/* HOME → Exit Game: leave through exit() on the game's own thread, so the
 * atexit handlers (SaveGameSettings) run as on the desktop's window close. */
static void check_exit(void)
{
    if (s_exitRequested) {
        exit(0);
    }
}

static void start_callback_thread(void)
{
    int thid = sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, NULL);
    s_callbackThread = thid;
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
}

/* main() leaves before platform_init only when it can't start: the game data
 * isn't next to the EBOOT (it prints that to stderr and returns, which on the
 * PSP was a silent drop back to the XMB). Say so on screen until HOME. */
static void startup_failure_screen(void)
{
    if (s_platformUp || s_exitRequested) {
        return;
    }
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "?");
    }
    start_callback_thread();
    pspDebugScreenInit();
    pspDebugScreenPrintf("Sonic R could not start.\n\n");
    pspDebugScreenPrintf("The game data was not found in\n  %s\n\n", cwd);
    pspDebugScreenPrintf("Make the folder with tools/make_ms_folder.py from your Sonic R PC CD:\n");
    pspDebugScreenPrintf("  SONICR.PAK (the game data) and MUSIC/track2.at3 ... next to EBOOT.PBP\n");
    pspDebugScreenPrintf("(or the CD's folders AI BIN CITY EMERALD FACTORY GENERAL ISLAND RUIN SOUND)\n\n");
    pspDebugScreenPrintf("Press HOME to quit.\n");
    while (!s_exitRequested) {
        sceKernelDelayThread(100 * 1000);
    }
    sceKernelExitGame();
}

/* =====================================================================
 * Platform API
 * ===================================================================== */

static SceUInt64 s_tickBase;
static unsigned int s_tickRes;

#ifdef SONICR_DEBUG_LOG
/* The log: what's written goes into a ring in memory — a copy, with the
 * interrupts held off for it, from any thread — and a thread of its own
 * writes it out to log.txt every second with the kernel's sceIoWrite. stderr
 * was the file, fully buffered, written out by that thread's fflush: it held
 * stderr's lock through a write to the Memory Stick tens of ms long, at the
 * lowest priority, and twice the PSP froze whole (PSPLink too) with the game
 * thread waiting on that lock. Now nothing waits on it: stderr is a stream
 * into the ring (funopen) the game thread alone uses, the audio threads
 * append to the ring themselves (PSP_LogAppend), and the file is the
 * writer's alone. Past a full ring the rest of a line is dropped. */
#define LOG_RING (128 * 1024)
static char s_logRing[LOG_RING];
static volatile unsigned int s_logHead, s_logTail;   /* written in, written out: free-running */
static SceUID s_logFd = -1;

void PSP_LogAppend(const char *s, int n)
{
    int intr = sceKernelCpuSuspendIntr();
    unsigned int room = LOG_RING - (s_logHead - s_logTail);
    unsigned int len = (unsigned int)n < room ? (unsigned int)n : room;
    unsigned int at = s_logHead % LOG_RING, first = len < LOG_RING - at ? len : LOG_RING - at;
    memcpy(&s_logRing[at], s, first);
    memcpy(s_logRing, s + first, len - first);
    s_logHead += len;
    sceKernelCpuResumeIntr(intr);
}

static void log_write_out(void)
{
    unsigned int head = s_logHead, tail = s_logTail;
    while (tail != head) {
        unsigned int at = tail % LOG_RING, n = head - tail;
        n = n < LOG_RING - at ? n : LOG_RING - at;
        sceIoWrite(s_logFd, &s_logRing[at], n);
        tail += n;
    }
    s_logTail = tail;
}

static int log_cookie_write(void *cookie, const char *buf, int n)
{
    (void)cookie;
    PSP_LogAppend(buf, n);
    return n;
}

static int log_flush_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        log_write_out();
    }
    return 0;
}
#endif

int platform_init(int width, int height, int fullscreen, const char *title)
{
    (void)width; (void)height; (void)fullscreen; (void)title;
    memset(s_keystate, 0, sizeof(s_keystate));

#ifdef SONICR_DEBUG_LOG
    /* DebugLog writes to stderr, which only PSPLink shows: keep a log.txt next
     * to the EBOOT, written out by a thread of its own every second (above) —
     * a write to the Memory Stick per line cost ~50ms each, which showed up as
     * loading time. */
    /* (log2.txt when log.txt is still held: a PSPLink session's build stopped
     * before module_stop closed it) */
    static const char *const logNames[] = { "log.txt", "log2.txt", "log3.txt" };
    for (int i = 0; i < 3 && s_logFd < 0; i++) {
        SceUID fd = sceIoOpen(logNames[i], PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        s_logFd = fd >= 0 ? fd : -1;
    }
    FILE *ring = s_logFd >= 0 ? funopen(NULL, NULL, log_cookie_write, NULL, NULL) : NULL;
    if (ring) {
        setvbuf(ring, NULL, _IONBF, 0);
        stderr = ring;
        /* (above every other thread: asleep but for its writes, and the log still
         * written out when one of them spins — what a freeze leaves to read) */
        int lt = sceKernelCreateThread("log_flush", log_flush_thread, 0x0F, 0x1000, 0, NULL);
        s_logThread = lt;
        if (lt >= 0) {
            sceKernelStartThread(lt, 0, NULL);
        }
    }
#endif

    s_platformUp = 1;
    start_callback_thread();

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    extern int GU_Init(void);   /* render_gu.c */
    if (GU_Init() != 0) {
        return -1;
    }
    extern int PSPAudio_Init(void);   /* audio_psp.c */
    PSPAudio_Init();
    return 0;
}

void platform_shutdown(void)
{
    extern void PSPAudio_Shutdown(void);
    PSPAudio_Shutdown();
}

/* The EBOOT sits in the data folder, which the PSP starts us in: main.c
 * falls back to chdir(DATA_DIR) = "." */
const char *platform_base_path(void)
{
    return NULL;
}

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
    check_exit();
    int copySize = keystateSize < 256 ? keystateSize : 256;
    memcpy(keystateOut, s_keystate, copySize);
    return 0;
}

void platform_pump_events(void)
{
    check_exit();
}

/* =====================================================================
 * Pad
 * ===================================================================== */

extern unsigned char g_keyPressState[320];     /* 80 per slot, see platform_sdl.c */
extern short g_joystickConfigWords[];
extern char  g_joystickSlots[4][282];
extern char  g_joystickDeviceNames[4][260];
extern short g_joystickDeviceFlags[8];
extern int   g_initFeatureC;

#define JOY_BUTTONS_PER_SLOT 80
#define JOY_CFG_MAX          32
#define STICK_DEADZONE       77    /* 0.6 of the stick's 128 travel, as on SDL */

/* PSP button → GCBTN_* index. GUIDE and the stick clicks have no PSP button. */
static const struct { unsigned int psp; int gc; } s_buttonMap[] = {
    { PSP_CTRL_CROSS,    GCBTN_A },
    { PSP_CTRL_CIRCLE,   GCBTN_B },
    { PSP_CTRL_SQUARE,   GCBTN_X },
    { PSP_CTRL_TRIANGLE, GCBTN_Y },
    { PSP_CTRL_SELECT,   GCBTN_BACK },
    { PSP_CTRL_START,    GCBTN_START },
    { PSP_CTRL_LTRIGGER, GCBTN_LEFTSHOULDER },
    { PSP_CTRL_RTRIGGER, GCBTN_RIGHTSHOULDER },
    { PSP_CTRL_UP,       GCBTN_DPAD_UP },
    { PSP_CTRL_DOWN,     GCBTN_DPAD_DOWN },
    { PSP_CTRL_LEFT,     GCBTN_DPAD_LEFT },
    { PSP_CTRL_RIGHT,    GCBTN_DPAD_RIGHT },
};

int platform_init_gamepads(void)
{
    /* One pad, always present: publish it the way gamepad_open does on SDL so
     * SyncJoystickSlots keeps a saved remap for it across launches. */
    strcpy(g_joystickDeviceNames[0], "PSP");
    g_joystickDeviceFlags[0] = JOY_SLOT_CFG_WORDS;
    g_initFeatureC = 1;
    return 1;
}

/* The draw distance (the far plane, and the fog band at its end) in quarters
 * of what the game sets for the track, set from (track_init.c): twice the
 * desktop's, the fog pushed out to the horizon. */
int g_pspFarBase;
int g_pspFarQuarters = 8;

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    check_exit();
    if (maxSlots <= 0) {
        return 0;
    }

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    /* Select+L+R: screenshot (render_gu.c), on the press only. */
    static int s_shotHeld;
    const unsigned int shotCombo = PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER;
    int shot = (pad.Buttons & shotCombo) == shotCombo;
    if (shot && !s_shotHeld) {
        extern void PSP_SaveScreenshot(void);
        PSP_SaveScreenshot();
    }
    s_shotHeld = shot;

#ifdef SONICR_PROF
    /* Test switches, each on Select+L and one more button (R not held: that's
     * the screenshot): ones the game gives Select alone were set off by play.
     *   Left: draws sent to the GE or not (render_gu.c)
     *   Up: every texture page in main RAM, none in VRAM (render_gu.c)
     *   Down: the track's textures without their mipmap (render_gu.c)
     *   Right: drawn without textures (render_gu.c)
     *   Triangle: no sky, sea or ground behind the track, the clear magenta (prof_wrap.c)
     *   Square: the fog band a vertex at a time instead of by the GE's fog (render_gu.c)
 *   Cross: a race at 30 fps instead of 60 (interp_psp.c) */
    {
        extern int g_pspSkipDraws, g_pspNoVramTex, g_pspTrackNoMip, g_pspNoTextures;
        extern int g_pspNoBackground, g_pspFogPasses, g_psp60;
        static const struct {
            unsigned int button;
            int *flag;
            const char *name;
        } tests[] = {
            { PSP_CTRL_LEFT, &g_pspSkipDraws, "draws skipped" },
            { PSP_CTRL_UP, &g_pspNoVramTex, "textures all in main RAM" },
            { PSP_CTRL_DOWN, &g_pspTrackNoMip, "track textures without their mipmap" },
            { PSP_CTRL_RIGHT, &g_pspNoTextures, "no textures" },
            { PSP_CTRL_TRIANGLE, &g_pspNoBackground, "no sky/sea/ground" },
            { PSP_CTRL_SQUARE, &g_pspFogPasses, "fog band by the GE's fog" },
            { PSP_CTRL_CROSS, &g_psp60, "60 fps" },
        };
        static unsigned int s_testHeld;
        const unsigned int mod = PSP_CTRL_SELECT | PSP_CTRL_LTRIGGER;
        unsigned int held = 0;
        if ((pad.Buttons & (mod | PSP_CTRL_RTRIGGER)) == mod) {
            for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
                if (pad.Buttons & tests[i].button) {
                    held |= tests[i].button;
                    if (!(s_testHeld & tests[i].button)) {
                        *tests[i].flag = !*tests[i].flag;
                        DebugLog("test: %s %s\n", tests[i].name, *tests[i].flag ? "on" : "off");
                        if (tests[i].flag == &g_pspNoVramTex) {
                            extern void GU_VramCommit(void);
                            GU_VramCommit();   /* (the pages moved on their next binds) */
                        }
                    }
                }
            }
        }
        s_testHeld = held;

        /* Circle: the draw distance 2x, 1x, 1.5x */
        static int s_farHeld;
        int far = (pad.Buttons & (mod | PSP_CTRL_RTRIGGER | PSP_CTRL_CIRCLE)) == (mod | PSP_CTRL_CIRCLE);
        if (far && !s_farHeld && g_pspFarBase > 0) {
            extern void InitFarClipAndFog(int farClip);
            g_pspFarQuarters = g_pspFarQuarters >= 8 ? 4 : g_pspFarQuarters + 2;
            InitFarClipAndFog(g_pspFarBase * g_pspFarQuarters / 4);
            DebugLog("test: draw distance %d/4 (far %d)\n", g_pspFarQuarters, g_pspFarBase * g_pspFarQuarters / 4);
        }
        s_farHeld = far;
    }
#endif

#ifdef SONICR_GPROF
    /* Select+Up: the profile starts over; Select+Down: written as gmon.out. */
    static unsigned int s_profHeld;
    unsigned int profKeys = pad.Buttons & (PSP_CTRL_SELECT | PSP_CTRL_UP | PSP_CTRL_DOWN);
    if (profKeys != s_profHeld) {
        if (profKeys == (PSP_CTRL_SELECT | PSP_CTRL_UP)) {
            gprof_stop(NULL, 0);
            gprof_start();
            DebugLog("gprof: started\n");
        } else if (profKeys == (PSP_CTRL_SELECT | PSP_CTRL_DOWN)) {
            gprof_stop("gmon.out", 1);
            DebugLog("gprof: gmon.out written\n");
        }
    }
    s_profHeld = profKeys;
#endif

    unsigned short bits = 0;
    int lx = (int)pad.Lx - 128, ly = (int)pad.Ly - 128;
    if (lx < -STICK_DEADZONE) bits |= PAD_LEFT;
    if (lx >  STICK_DEADZONE) bits |= PAD_RIGHT;
    if (ly < -STICK_DEADZONE) bits |= PAD_UP;
    if (ly >  STICK_DEADZONE) bits |= PAD_DOWN;

    /* Same lookup as platform_sdl.c: the per-slot remap below
     * JOY_SLOT_CFG_WORDS, the shared table past it. */
    unsigned char *pressBase = &g_keyPressState[0];
    const short *slotCfg = (const short *)&g_joystickSlots[0][0x104];
    memset(pressBase, 0, JOY_BUTTONS_PER_SLOT);
    for (unsigned i = 0; i < sizeof(s_buttonMap) / sizeof(s_buttonMap[0]); i++) {
        if (!(pad.Buttons & s_buttonMap[i].psp)) {
            continue;
        }
        int b = s_buttonMap[i].gc;
        pressBase[b] = 0x80;
        if (b < JOY_SLOT_CFG_WORDS) {
            bits |= (unsigned short)slotCfg[b];
        } else if (b < JOY_CFG_MAX) {
            bits |= (unsigned short)g_joystickConfigWords[b];
        }
    }

    joySlotState[0] = bits;
    for (int i = 1; i < maxSlots; i++) {
        joySlotState[i] = 0;
    }
    memset(&g_keyPressState[JOY_BUTTONS_PER_SLOT], 0, 3 * JOY_BUTTONS_PER_SLOT);
    return 1;
}

/* =====================================================================
 * File I/O (fileio.h)
 * ===================================================================== */

#define FILE_BUFFER (32 * 1024)

#ifdef SONICR_DEBUG_LOG
/* Load profile: each file's name and how long it stayed open. */
static struct { FILE *f; uint32_t t0; char name[48]; } s_openFiles[16];
#endif

/* The largest block malloc gives now, in KB (a halving search; for the log) */
/* The heap's use, from newlib's own count (mallinfo): what malloc holds
 * now, and how far the heap has grown. */
void PSP_HeapKB(int *inUse, int *grown)
{
    struct mallinfo mi = mallinfo();
    *inUse = (int)(mi.uordblks / 1024);
    *grown = (int)(mi.arena / 1024);
}

int PSP_LargestFreeKB(void)
{
    unsigned int lo = 0, hi = 64 * 1024;
    while (lo < hi) {
        unsigned int mid = (lo + hi + 1) / 2;
        void *p = malloc((size_t)mid * 1024);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return (int)lo;
}

/* A game file opened for reading: the kernel's descriptor under a stdio
 * stream of its own (funopen), so the loaders' fread/fseek reach sceIoRead
 * with nothing of libcglue's between — and the log can say what of a load
 * is the Memory Stick (opening, reading) and what the game's own work. */
typedef struct {
    SceUID fd;                  /* its own; or the pack's (packed) */
    int packed;
    unsigned int base, size, pos;   /* (packed: where it is in the pack, and how far read) */
#ifdef SONICR_DEBUG_LOG
    unsigned int openUs, readUs, reads;
#endif
} GameFile;

#ifdef SONICR_DEBUG_LOG
static GameFile *s_lastClosed;   /* (its times, for sr_fClose's line) */
static GameFile s_closedCopy;
#endif

/* SONICR.PAK (tools/make_ms_folder.py): the game's data in one file, opened
 * once — a file opened on the Memory Stick cost ~22 ms, and a race opened
 * 27. Its index (~15 KB) in memory, the data read from it as asked: no more
 * memory than the files. A file it hasn't, or no pack, is opened as a file. */
typedef struct { unsigned int name, offset, size; } PackEntry;
static SceUID s_packFd = -1;
static int s_packTried, s_packCount;
static PackEntry *s_packEntries;
static char *s_packNames;
static unsigned int s_packAt;   /* where the pack's descriptor is (a read seeks only when another moved it) */

static void pack_open(void)
{
    s_packTried = 1;
    char path[512];
    if (!getcwd(path, sizeof(path) - 16)) {
        return;
    }
    strcat(path, "/SONICR.PAK");
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    unsigned int head[4];
    if (sceIoRead(fd, head, sizeof(head)) != (int)sizeof(head) || memcmp(head, "SRPK", 4) != 0 || head[1] != 1) {
        sceIoClose(fd);
        return;
    }
    s_packEntries = malloc(head[2] * sizeof(PackEntry));
    s_packNames = malloc(head[3]);
    if (!s_packEntries || !s_packNames ||
        sceIoRead(fd, s_packEntries, head[2] * sizeof(PackEntry)) != (int)(head[2] * sizeof(PackEntry)) ||
        sceIoRead(fd, s_packNames, head[3]) != (int)head[3]) {
        free(s_packEntries);
        free(s_packNames);
        s_packEntries = NULL;
        s_packNames = NULL;
        sceIoClose(fd);
        return;
    }
    s_packFd = fd;
    s_packCount = (int)head[2];
    s_packAt = sizeof(head) + head[2] * sizeof(PackEntry) + head[3];
    DebugLog("SONICR.PAK: %d files\n", s_packCount);
}

/* path's name in the pack: from the game's folder ("./" off), upper case,
 * '/' between the parts — as make_ms_folder.py's pack_name; 0 if it can't be */
static int pack_key(const char *path, char *key, int keySize)
{
    if (strchr(path, ':')) {
        return 0;   /* (a device path: not the game's folder's) */
    }
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) {
        path += 2;
    }
    int n = 0;
    for (; *path && n < keySize - 1; path++) {
        char c = *path == '\\' ? '/' : *path;
        key[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    key[n] = '\0';
    return *path == '\0';
}

static const PackEntry *pack_find(const char *path)
{
    char key[256];
    if (!s_packTried) {
        pack_open();
    }
    if (s_packFd < 0 || !pack_key(path, key, sizeof(key))) {
        return NULL;
    }
    int lo = 0, hi = s_packCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(key, s_packNames + s_packEntries[mid].name);
        if (c == 0) {
            return &s_packEntries[mid];
        }
        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

/* Files the game looked for and wasn't there (the ten ghosts of a track it
 * tries every race, KEYS.BIN...): not asked of the Memory Stick again until
 * something is written (sr_fOpen) — ~20 ms each. */
#define MISSING_MAX 64
static uint32_t s_missing[MISSING_MAX];
static int s_missingCount;

static uint32_t name_hash(const char *path)
{
    char key[256];
    if (!pack_key(path, key, sizeof(key))) {
        return 0;
    }
    uint32_t h = 2166136261u;
    for (const char *k = key; *k; k++) {
        h = (h ^ (unsigned char)*k) * 16777619u;
    }
    return h ? h : 1;
}

static int gamefile_read(void *cookie, char *buf, int n)
{
    GameFile *g = cookie;
#ifdef SONICR_DEBUG_LOG
    unsigned int t0 = sceKernelGetSystemTimeLow();
#endif
    int r;
    if (g->packed) {
        unsigned int left = g->pos < g->size ? g->size - g->pos : 0;
        n = (unsigned int)n < left ? n : (int)left;
        if (s_packAt != g->base + g->pos) {
            sceIoLseek32(g->fd, (int)(g->base + g->pos), PSP_SEEK_SET);
        }
        r = n > 0 ? sceIoRead(g->fd, buf, (SceSize)n) : 0;
        g->pos += r > 0 ? (unsigned int)r : 0;
        s_packAt = r >= 0 ? g->base + g->pos : 0xFFFFFFFFu;
    } else {
        r = sceIoRead(g->fd, buf, (SceSize)n);
    }
#ifdef SONICR_DEBUG_LOG
    g->readUs += sceKernelGetSystemTimeLow() - t0;
    g->reads++;
#endif
    return r < 0 ? -1 : r;
}

static fpos_t gamefile_seek(void *cookie, fpos_t offset, int whence)
{
    GameFile *g = cookie;
    if (g->packed) {
        long at = whence == SEEK_CUR ? (long)g->pos + (long)offset
                : whence == SEEK_END ? (long)g->size + (long)offset : (long)offset;
        if (at < 0) {
            errno = EINVAL;
            return -1;
        }
        g->pos = (unsigned int)at;   /* (past the end reads nothing, as a file's) */
        return (fpos_t)at;
    }
    int w = whence == SEEK_CUR ? PSP_SEEK_CUR : whence == SEEK_END ? PSP_SEEK_END : PSP_SEEK_SET;
    return (fpos_t)sceIoLseek32(g->fd, (int)offset, w);
}

static int gamefile_close(void *cookie)
{
    GameFile *g = cookie;
    if (!g->packed) {
        sceIoClose(g->fd);
    }
#ifdef SONICR_DEBUG_LOG
    s_closedCopy = *g;
    s_lastClosed = &s_closedCopy;
#endif
    free(g);
    return 0;
}

static FILE *gamefile_open(const char *path)
{
#ifdef SONICR_DEBUG_LOG
    unsigned int t0 = sceKernelGetSystemTimeLow();
#endif
    const PackEntry *e = pack_find(path);
    uint32_t h = e ? 0 : name_hash(path);
    for (int i = 0; h && i < s_missingCount; i++) {
        if (s_missing[i] == h) {
            errno = ENOENT;
            return NULL;   /* (not there last time, nothing written since) */
        }
    }
    SceUID fd = s_packFd;
    if (e == NULL) {
        char full[512];
        const char *open = path;
        if (strchr(path, ':') == NULL) {   /* (relative: from the EBOOT's folder, as fopen would) */
            if (!getcwd(full, sizeof(full) - 1)) {
                return NULL;
            }
            size_t at = strlen(full);
            const char *rest = path[0] == '.' && path[1] == '/' ? path + 2 : path;
            snprintf(full + at, sizeof(full) - at, "/%s", rest);
            open = full;
        }
        fd = sceIoOpen(open, PSP_O_RDONLY, 0);
        if (fd < 0) {
            if (h && s_missingCount < MISSING_MAX) {
                s_missing[s_missingCount++] = h;
            }
            errno = ENOENT;
            return NULL;
        }
    }
    GameFile *g = calloc(1, sizeof(GameFile));
    FILE *f = g ? funopen(g, gamefile_read, NULL, gamefile_seek, gamefile_close) : NULL;
    if (f == NULL) {
        free(g);
        if (e == NULL) {
            sceIoClose(fd);
        }
        return NULL;
    }
    g->fd = fd;
    if (e) {
        g->packed = 1;
        g->base = e->offset;
        g->size = e->size;
    }
#ifdef SONICR_DEBUG_LOG
    g->openUs = sceKernelGetSystemTimeLow() - t0;
#endif
    return f;
}

FILE *sr_fOpen(const char *path, const char *mode)
{
#ifdef SONICR_DEBUG_LOG
    uint32_t t0 = platform_get_time_ms();   /* the open itself counts */
#endif
    int readOnly = mode[0] == 'r' && strchr(mode, '+') == NULL;
    if (!readOnly) {
        s_missingCount = 0;   /* (a write: what was missing may be there now) */
    }
    FILE *f = readOnly ? gamefile_open(path) : fopen(path, mode);
    if (f == NULL) {
#ifdef SONICR_DEBUG_LOG
        /* (written out at once: the game reads on from a file that didn't
         * open as if it had, and the crash that follows loses the buffer) */
        extern int PSP_LargestFreeKB(void);
        fprintf(stderr, "sr_fOpen: cannot open %s (errno %d, largest free block %d KB)\n", path, errno,
                PSP_LargestFreeKB());
        fflush(stderr);
#endif
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, FILE_BUFFER);
#ifdef SONICR_DEBUG_LOG
    for (int i = 0; i < 16; i++) {
        if (s_openFiles[i].f == NULL) {
            s_openFiles[i].f = f;
            s_openFiles[i].t0 = t0;
            strncpy(s_openFiles[i].name, path, sizeof(s_openFiles[i].name) - 1);
            s_openFiles[i].name[sizeof(s_openFiles[i].name) - 1] = '\0';
            break;
        }
    }
#endif
    return f;
}

/* (log: the descriptors open now, and the game files sr_fOpen opened and
 * nothing closed yet — a leak's name) */
void PSP_LogOpenFiles(const char *when)
{
#ifdef SONICR_DEBUG_LOG
    int n = 0;
    for (int fd = 3; fd < 1024; fd++) {
        n += __descriptormap[fd] != NULL;
    }
    fprintf(stderr, "open files at %s: %d descriptors;", when, n);
    for (int i = 0; i < 16; i++) {
        if (s_openFiles[i].f) {
            fprintf(stderr, " %s", s_openFiles[i].name);
        }
    }
    fputc(10, stderr);
#else
    (void)when;
#endif
}

int sr_fClose(FILE *file)
{
#ifdef SONICR_DEBUG_LOG
    for (int i = 0; i < 16; i++) {
        if (s_openFiles[i].f == file) {
            long size = ftell(file);
            s_lastClosed = NULL;
            int r = fclose(file);
            unsigned int ms = (unsigned)(platform_get_time_ms() - s_openFiles[i].t0);
            if (s_lastClosed) {   /* (a game file: the Memory Stick's part of it) */
                fprintf(stderr, "[%7u] load %-40s %7ld bytes %5u ms (open %u, %u reads %u ms)\n",
                        (unsigned)platform_get_time_ms(), s_openFiles[i].name, size, ms,
                        s_lastClosed->openUs / 1000, s_lastClosed->reads, s_lastClosed->readUs / 1000);
            } else {
                fprintf(stderr, "[%7u] load %-40s %7ld bytes %5u ms\n", (unsigned)platform_get_time_ms(),
                        s_openFiles[i].name, size, ms);
            }
            s_openFiles[i].f = NULL;
            return r;
        }
    }
#endif
    return fclose(file);
}

/* =====================================================================
 * Timing
 * ===================================================================== */

uint32_t platform_get_time_ms(void)
{
    SceUInt64 now;
    sceRtcGetCurrentTick(&now);
    if (s_tickRes == 0) {
        s_tickRes = sceRtcGetTickResolution();   /* 1000000 */
        s_tickBase = now;
    }
    return (uint32_t)((now - s_tickBase) / (s_tickRes / 1000));
}

void platform_sleep_ms(int ms)
{
    if (ms > 0) {
        sceKernelDelayThread(ms * 1000);
    }
}

int platform_audio_init(void)
{
    return 0;   /* PSPAudio_Init runs from platform_init */
}

void platform_audio_shutdown(void)
{
}

void platform_gl_swap(void)
{
}

void platform_get_drawable_size(int *w, int *h)
{
    *w = 480;
    *h = 272;
}

/* =====================================================================
 * Networking — no Wi-Fi bring-up yet: the LAN modes report no network.
 * ===================================================================== */

int platform_net_init(void)
{
    return -1;
}

void platform_net_shutdown(void)
{
}

int platform_net_is_modem(void)
{
    return 0;
}

int platform_get_region(void)
{
    return 0;
}
