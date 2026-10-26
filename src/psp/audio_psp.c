/**
 * audio_psp.c — Music and sound effects for the PSP build.
 *
 * PSP counterpart to sdl/src/sound/music_sdl.c + sdl/src/sound/sfx_sdl.c: the
 * same entry points and the same game-facing logic (track table, fanfares,
 * medley clock, slot table, distance attenuation, loop pitch), with SDL_mixer
 * replaced by one mixer thread feeding a sceAudio channel at 44.1kHz stereo.
 *
 * Music: MUSIC/track<N>.at3 (N = the CD track, 2..21; tools/convert_music.py),
 * ATRAC3 decoded on the Media Engine (sceAtrac) and streamed from the Memory
 * Stick: the main CPU only copies it into the mix.
 *
 * Effects: the WAVs in SOUND/SFX, loaded as 16-bit mono at 22050 Hz (the rate the
 * game's DirectSound frequencies are relative to), played by voices with a
 * live pitch and gain — what sfx_sdl.c builds out of SDL_mixer effects.
 */

#include <pspkernel.h>
#include <pspaudio.h>
#include <pspatrac3.h>
#include <psputility.h>
#include <pspiofilemgr.h>
#include <pspsdk.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "sonicr_paths.h"
#include "sound/replay_voice.h"

extern void SetAllSoundVolumes(void);        /* 0x4D0760 */
extern FILE *sr_fOpen(const char *path, const char *mode);   /* platform_psp.c */
extern int sr_fClose(FILE *file);
extern int  g_soundActive[64];               /* 0x006DA080 */
extern volatile int g_pspResumeCount;        /* platform_psp.c */

#define MIX_FRAMES     1024                  /* per sceAudio output: ~23ms */
#define OUT_RATE       44100
#define SFX_MAX_SLOTS  64
#define SFX_BASE_RATE  22050

/* The mixer's and the music's threads log through here, never stdio: its
 * locks are the game thread's alone (platform_psp.c's log). Integers only — a
 * float through vsnprintf can allocate. */
#ifdef SONICR_DEBUG_LOG
#include <stdarg.h>
extern void PSP_LogAppend(const char *s, int n);   /* platform_psp.c */
static void thread_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    PSP_LogAppend(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
}
#else
#define thread_log(...) ((void)0)
#endif

static SceUID s_lock = -1;                   /* game thread vs mixer thread */
static int s_audioCh = -1;
static volatile int s_running;
static SceUID s_thread = -1;

static inline void lock(void)   { sceKernelWaitSema(s_lock, 1, NULL); }
static inline void unlock(void) { sceKernelSignalSema(s_lock, 1); }

/* =====================================================================
 * Effects: slots and voices
 * ===================================================================== */

typedef struct {
    short *pcm;          /* mono, SFX_BASE_RATE */
    int    frames;
} SfxSlot;

typedef struct {
    int      active;
    int      loop;
    unsigned pos;        /* integer frame */
    unsigned frac;       /* 16.16 fraction of the next frame */
    int      freqHz;     /* playback rate; SFX_BASE_RATE = natural pitch */
    int      gain;       /* 0..256 */
    int      gainL, gainR;   /* pan, 0..256 each */
} SfxVoice;

static SfxSlot  s_slot[SFX_MAX_SLOTS];
static SfxVoice s_voice[SFX_MAX_SLOTS];
static int s_sfxReady;

/* =====================================================================
 * Music: one ATRAC3 stream (as in Sonic Mania's PSP port, RSDK Audio.cpp)
 * ===================================================================== */

#define AT3_BUFFER (64 * 1024)    /* ~4s at 132kbps */
#define AT3_READ   (16 * 1024)    /* the most a refill reads at once */

static unsigned char __attribute__((aligned(64))) s_at3Buffer[AT3_BUFFER];
static short __attribute__((aligned(64))) s_at3Pcm[2048];   /* one frame: 1024 stereo samples */
static int  s_at3PcmPos, s_at3PcmCount;
static int  s_at3ID = -1;
static SceUID s_at3File = -1;
static int  s_at3Size;
static int  s_at3Streaming;      /* the file is larger than the buffer: kept topped up */
static int  s_at3Ended;          /* the decoder returned the last frame */
static int  s_at3Loop;
static unsigned s_at3NextSample; /* where the decoder is, to resume after sleep */
static int  s_at3Modules;
static int  s_at3FileResume, s_at3DecoderResume;
static char s_at3Path[256];
static char s_dataDir[192];

static volatile int s_musicGain = 256;   /* 0..256: slider and duck (music_sdl.c) */
static volatile int s_musicPlaying;      /* Mix_PlayingMusic */

static void at3_close(void)
{
    if (s_at3ID >= 0) {
        sceAtracReleaseAtracID(s_at3ID);
    }
    if (s_at3File >= 0) {
        sceIoClose(s_at3File);
    }
    s_at3ID = -1;
    s_at3File = -1;
    s_musicPlaying = 0;
}

static void at3_reopen(void)
{
    if (s_at3File >= 0) {
        sceIoClose(s_at3File);
    }
    s_at3FileResume = g_pspResumeCount;
    s_at3File = sceIoOpen(s_at3Path, PSP_O_RDONLY, 0);
}

static int at3_seek(unsigned sample)
{
    s_at3PcmPos = s_at3PcmCount = 0;
    s_at3Ended = 0;
    s_at3NextSample = sample;
    if (!s_at3Streaming) {
        return sceAtracResetPlayPosition(s_at3ID, sample, 0, 0) >= 0;
    }
    if (s_at3FileResume != g_pspResumeCount) {
        at3_reopen();
    }
    PspBufferInfo info;
    if (sceAtracGetBufferInfoForReseting(s_at3ID, sample, &info) < 0) {
        return 0;
    }
    int first = 0, second = 0;
    if (info.uiWritableByteFirstBuf > 0) {
        sceIoLseek32(s_at3File, info.uiReadPositionFirstBuf, PSP_SEEK_SET);
        first = sceIoRead(s_at3File, info.pucWritePositionFirstBuf, info.uiWritableByteFirstBuf);
    }
    if (info.uiWritableByteSecondBuf > 0) {
        sceIoLseek32(s_at3File, info.uiReadPositionSecondBuf, PSP_SEEK_SET);
        second = sceIoRead(s_at3File, info.pucWritePositionSecondBuf, info.uiWritableByteSecondBuf);
    }
    sceKernelDcacheWritebackRange(s_at3Buffer, sizeof(s_at3Buffer));   /* (the decoder reads memory) */
    return first >= 0 && second >= 0 &&
           sceAtracResetPlayPosition(s_at3ID, sample, first, second) >= 0;
}

/* Mixer thread only (music_service), like every sceAtrac call. */
static int at3_open(int trackNum, unsigned startSample, int loop)
{
    at3_close();
    if (!s_at3Modules) {
        return 0;
    }
    snprintf(s_at3Path, sizeof(s_at3Path), "%s/MUSIC/track%d.at3", s_dataDir, trackNum);
    SceUID file = sceIoOpen(s_at3Path, PSP_O_RDONLY, 0);
    if (file < 0) {
        thread_log("PSP music: can't open %s (%08X)\n", s_at3Path, file);
        return 0;
    }
    int size = sceIoLseek32(file, 0, PSP_SEEK_END);
    sceIoLseek32(file, 0, PSP_SEEK_SET);
    int bytes = sceIoRead(file, s_at3Buffer, size < AT3_BUFFER ? size : AT3_BUFFER);
    /* The Media Engine reads memory, not the CPU's cache: write the file data
     * out before handing it over (as the Balatro port's music_psp.c does). */
    sceKernelDcacheWritebackRange(s_at3Buffer, sizeof(s_at3Buffer));
    int id = bytes > 0 ? sceAtracSetDataAndGetID(s_at3Buffer, size <= AT3_BUFFER ? size : AT3_BUFFER) : -1;
    if (id < 0) {
        thread_log("PSP music: %s rejected (%08X)\n", s_at3Path, id);
        sceIoClose(file);
        return 0;
    }
    s_at3ID = id;
    s_at3Size = size;
    s_at3Streaming = size > AT3_BUFFER;
    s_at3File = file;
    s_at3FileResume = s_at3DecoderResume = g_pspResumeCount;
    if (!s_at3Streaming) {
        sceIoClose(file);
        s_at3File = -1;
    }
    s_at3PcmPos = s_at3PcmCount = 0;
    s_at3Ended = 0;
    s_at3NextSample = 0;
    s_at3Loop = loop;
    if (startSample) {
        at3_seek(startSample);
    }
    s_musicPlaying = 1;
    thread_log("PSP music: track%d.at3 open (id %d, %d bytes, %s, %s)\n", trackNum, id, size,
             s_at3Streaming ? "streamed" : "in memory", loop ? "loops" : "once");
    return 1;
}

/* Mixer thread: keep the decoder's buffer topped up, and set the
 * track up again after the PSP slept (which powers the Media Engine down and
 * closes the file). */
static int s_at3Track;
static void at3_refill(void)
{
    if (s_at3ID >= 0 && s_at3DecoderResume != g_pspResumeCount) {
        unsigned at = s_at3NextSample;
        int loop = s_at3Loop;
        if (!at3_open(s_at3Track, at, loop)) {
            return;   /* (tried again on the next mix, the Memory Stick coming back) */
        }
    }
    if (s_at3ID < 0 || !s_at3Streaming) {
        return;
    }
    u8 *writePos = NULL;
    u32 available = 0, readOffset = 0;
    if (sceAtracGetStreamDataInfo(s_at3ID, &writePos, &available, &readOffset) < 0 || available == 0) {
        return;
    }
    if (s_at3FileResume != g_pspResumeCount) {
        at3_reopen();
    }
    int got = -1;
    if (s_at3File >= 0 && sceIoLseek32(s_at3File, readOffset, PSP_SEEK_SET) >= 0) {
        got = sceIoRead(s_at3File, writePos, available < AT3_READ ? available : AT3_READ);
    }
    if (got < 0) {
        at3_reopen();
    }
    if (got > 0) {
        sceKernelDcacheWritebackRange(writePos, got);   /* (the decoder reads memory) */
        sceAtracAddStreamData(s_at3ID, got);
    }
}

/* Decoded music on its way from the music thread to the mixer: a ring the
 * music thread fills and the mixer drains (one writer, one reader). ~93ms. */
#define RING_FRAMES 4096
static short s_ring[RING_FRAMES * 2];
static volatile unsigned s_ringW, s_ringR;   /* frames written / read so far */
static volatile unsigned s_ringUnderruns;

/* Music thread: decode into the ring until it's full. */
static void at3_produce(void)
{
    int rewinds = 0;   /* (one a call: a track that ends again at once can't spin this thread) */
    while (s_at3ID >= 0 && s_ringW - s_ringR < RING_FRAMES) {
        if (s_at3PcmPos >= s_at3PcmCount) {
            if (s_at3Ended) {
                if (s_at3Loop && rewinds++ == 0 && at3_seek(0)) {
                    continue;
                }
                if (s_at3Loop && rewinds > 1) {
                    break;   /* (rewound and ended: tried again on the next call) */
                }
                at3_close();
                break;
            }
            int got = 0, end = 0, remain = 0;
            int r = sceAtracDecodeData(s_at3ID, (u16 *)s_at3Pcm, &got, &end, &remain);
            if (r < 0) {
                if (r == (int)PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED) {
                    s_at3Ended = 1;
                    continue;
                }
                static int s_decodeErrors;
                if (s_decodeErrors < 8) {
                    s_decodeErrors++;
                    thread_log("PSP music: decode error %08X (sample %u)\n", r, s_at3NextSample);
                }
                break;   /* starved: silence until the buffer catches up */
            }
            s_at3PcmPos = 0;
            s_at3PcmCount = got;
            s_at3NextSample += got;
            if (end) {
                s_at3Ended = 1;
            }
            if (got == 0 && !s_at3Ended) {
                break;
            }
        }
        int room = RING_FRAMES - (int)(s_ringW - s_ringR);
        int take = s_at3PcmCount - s_at3PcmPos;
        if (take > room) {
            take = room;
        }
        const short *pcm = &s_at3Pcm[s_at3PcmPos * 2];
        unsigned w = s_ringW;
        for (int i = 0; i < take; i++) {
            unsigned at = ((w + i) & (RING_FRAMES - 1)) * 2;
            s_ring[at] = pcm[i * 2];
            s_ring[at + 1] = pcm[i * 2 + 1];
        }
        s_at3PcmPos += take;
        s_ringW = w + take;   /* (published after the samples) */
    }
}

/* Mixer thread: add the next n frames of music from the ring into mix. */
static void music_mix(int *mix, int n)
{
    int gain = s_musicGain;
    unsigned r = s_ringR;
    int avail = (int)(s_ringW - r);
    int take = avail < n ? avail : n;
    if (take < n && s_musicPlaying) {
        s_ringUnderruns++;
    }
    for (int i = 0; i < take; i++) {
        unsigned at = ((r + i) & (RING_FRAMES - 1)) * 2;
        mix[i * 2]     += (s_ring[at] * gain) >> 8;
        mix[i * 2 + 1] += (s_ring[at + 1] * gain) >> 8;
    }
    s_ringR = r + take;
}

/* Track requests from the game thread (PlayCD/StopCD). Every sceAtrac call is
 * made by the music thread: if the Media Engine stops answering, only the
 * music stops — neither the game thread nor the effects mixer ever wait on it
 * (nor on the Memory Stick read that opens a track). */
static volatile int s_reqTrack;     /* 0 = stop */
static volatile int s_reqLoop;
static volatile unsigned s_reqSeq;
static unsigned s_doneSeq;

static void music_service(void)
{
    unsigned seq = s_reqSeq;
    if (seq == s_doneSeq) {
        return;
    }
    s_doneSeq = seq;
    int track = s_reqTrack;
    if (track) {
        s_at3Track = track;
        if (!at3_open(track, 0, s_reqLoop)) {
            s_musicPlaying = 0;
        }
    } else {
        at3_close();
    }
}

/* =====================================================================
 * Music thread and mixer thread
 * ===================================================================== */

static SceUID s_musicThread = -1;

static int music_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    pspSdkDisableFPUExceptions();
    while (s_running) {
        music_service();
        at3_refill();
        at3_produce();
        sceKernelDelayThread(10 * 1000);   /* the ring holds ~93ms */
    }
    return 0;
}

static void sfx_mix(int *mix, int n)
{
    for (int s = 0; s < SFX_MAX_SLOTS; s++) {
        SfxVoice *v = &s_voice[s];
        SfxSlot *sl = &s_slot[s];
        if (!v->active || sl->pcm == NULL || sl->frames <= 0) {
            continue;
        }
        unsigned step = (unsigned)(((unsigned long long)(v->freqHz > 0 ? v->freqHz : SFX_BASE_RATE) << 16) / OUT_RATE);
        int gl = (v->gain * v->gainL) >> 8, gr = (v->gain * v->gainR) >> 8;
        unsigned pos = v->pos, frac = v->frac, frames = (unsigned)sl->frames;
        const short *pcm = sl->pcm;
        for (int i = 0; i < n; i++) {
            if (pos >= frames) {
                if (v->loop) {
                    pos %= frames;
                } else {
                    v->active = 0;
                    break;
                }
            }
            unsigned next = pos + 1 < frames ? pos + 1 : (v->loop ? 0 : pos);
            int a = pcm[pos], b = pcm[next];
            int smp = a + (((b - a) * (int)frac) >> 16);
            mix[i * 2]     += (smp * gl) >> 8;
            mix[i * 2 + 1] += (smp * gr) >> 8;
            frac += step;
            pos += frac >> 16;
            frac &= 0xFFFF;
        }
        v->pos = pos;
        v->frac = frac;
    }
}

static int mixer_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    pspSdkDisableFPUExceptions();
    static int mix[MIX_FRAMES * 2];
    static short __attribute__((aligned(64))) out[2][MIX_FRAMES * 2];
    int buf = 0;
#ifdef SONICR_DEBUG_LOG
    /* What the mixer costs the main CPU, logged every 200 buffers (~4.6s):
     * music (copy out of the ring; the decoding is the music thread's, on
     * the Media Engine) and effects, per buffer of MIX_FRAMES (23.2ms). */
    unsigned int profMusic = 0, profSfx = 0, profBuffers = 0, profVoices = 0;
#endif
    while (s_running) {
        memset(mix, 0, sizeof(mix));
#ifdef SONICR_DEBUG_LOG
        unsigned int t0 = sceKernelGetSystemTimeLow();
#endif
        music_mix(mix, MIX_FRAMES);
#ifdef SONICR_DEBUG_LOG
        unsigned int t1 = sceKernelGetSystemTimeLow();
        for (int s = 0; s < SFX_MAX_SLOTS; s++) {
            profVoices += s_voice[s].active;
        }
#endif
        lock();
        sfx_mix(mix, MIX_FRAMES);
        unlock();
#ifdef SONICR_DEBUG_LOG
        unsigned int t2 = sceKernelGetSystemTimeLow();
        profMusic += t1 - t0;
        profSfx += t2 - t1;
        if (++profBuffers == 200) {
            unsigned int cpu100 = (profMusic + profSfx) / 2322;   /* (% of the buffer's time, x100) */
            thread_log("[%7u] audio: music %4u us, sfx %4u us (%u.%u voices) per 23.2ms buffer = %u.%02u%% CPU"
                       " | music %s, %u underruns\n",
                       t2 / 1000, profMusic / 200, profSfx / 200, profVoices / 200, profVoices % 200 / 20,
                       cpu100 / 100, cpu100 % 100, s_musicPlaying ? "playing" : "off", s_ringUnderruns);
            profMusic = profSfx = profBuffers = profVoices = 0;
        }
#endif
        short *o = out[buf];
        for (int i = 0; i < MIX_FRAMES * 2; i++) {
            int x = mix[i];
            o[i] = (short)(x < -32768 ? -32768 : (x > 32767 ? 32767 : x));
        }
        int outErr = sceAudioOutputBlocking(s_audioCh, PSP_AUDIO_VOLUME_MAX, o);
        static int s_outErrors;
        if (outErr < 0 && s_outErrors < 4) {
            s_outErrors++;
            thread_log("PSP audio: output error %08X on channel %d\n", outErr, s_audioCh);
        }
        buf ^= 1;
    }
    return 0;
}

int PSPAudio_Init(void)
{
    if (s_lock >= 0) {
        return 0;
    }
    if (!getcwd(s_dataDir, sizeof(s_dataDir))) {
        strcpy(s_dataDir, ".");
    }
    s_lock = sceKernelCreateSema("sonicr_audio", 0, 1, 1, NULL);

    int avcodec = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    int atrac = sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS);
    /* 0x80020139: already loaded */
    s_at3Modules = (avcodec >= 0 || avcodec == (int)0x80020139) && (atrac >= 0 || atrac == (int)0x80020139);
    DebugLog("PSP audio: modules %08X %08X, data %s; free outside the heap %u KB (largest block %u KB)\n",
             avcodec, atrac, s_dataDir, (unsigned)(sceKernelTotalFreeMemSize() / 1024),
             (unsigned)(sceKernelMaxFreeMemSize() / 1024));

    s_audioCh = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, MIX_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (s_audioCh < 0) {
        DebugLog("PSP audio: no channel (%08X)\n", s_audioCh);
        return -1;
    }
    s_running = 1;
    s_thread = sceKernelCreateThread("sonicr_mixer", mixer_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (s_thread >= 0) {
        sceKernelStartThread(s_thread, 0, NULL);
    }
    s_musicThread = sceKernelCreateThread("sonicr_music", music_thread, 0x13, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (s_musicThread >= 0) {
        sceKernelStartThread(s_musicThread, 0, NULL);
    }
    return 0;
}

/* PSPLink's modstop (platform_psp.c module_stop): the threads stopped where
 * they stand — one may be waiting on the Media Engine forever — and the
 * channel given back. */
void PSPAudio_Kill(void)
{
    if (s_musicThread >= 0) {
        sceKernelTerminateDeleteThread(s_musicThread);
        s_musicThread = -1;
    }
    if (s_thread >= 0) {
        sceKernelTerminateDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_audioCh >= 0) {
        sceAudioChRelease(s_audioCh);
        s_audioCh = -1;
    }
    if (s_at3File >= 0) {
        sceIoClose(s_at3File);
        s_at3File = -1;
    }
}

void PSPAudio_Shutdown(void)
{
    s_running = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_musicThread >= 0) {
        /* (bounded: a Media Engine that stopped answering never lets it end) */
        SceUInt timeout = 500 * 1000;
        if (sceKernelWaitThreadEnd(s_musicThread, &timeout) < 0) {
            sceKernelTerminateThread(s_musicThread);
        }
        sceKernelDeleteThread(s_musicThread);
        s_musicThread = -1;
    }
    if (s_audioCh >= 0) {
        sceAudioChRelease(s_audioCh);
        s_audioCh = -1;
    }
}

/* =====================================================================
 * Music API (music_sdl.c)
 * ===================================================================== */

static int s_currentTrack;
static int s_musicReady;
static int s_currentTrackIsFanfare;   /* don't auto-replay finished fanfares */

/* Per-track durations in ms — ROM table at 0x5041AC, indexed by CD track. */
static const int s_trackDurationMs[23] = {
         0,      0,  43210,   6993,  10170,  55580, 305305, 283904,
    270474, 237737, 295113, 241929, 240949, 164269, 208959, 210448,
    202700, 210360, 238142,   3921,   6451,   5910, 5448657
};

static int   s_logicalTrack;     /* 0x6DA2A4 */
static DWORD s_trackStartMs;     /* 0x6DA2A8 */

static int s_musicDucked;
static int s_musicLevel = 8;

static void music_apply_volume(void)
{
    int base = s_musicDucked ? 128 : 256;
    s_musicGain = base * s_musicLevel / 8;
}

void Music_SetVolume(int level)
{
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    s_musicLevel = level;
    music_apply_volume();
}

void Music_SetDucked(int ducked)
{
    s_musicDucked = ducked ? 1 : 0;
    music_apply_volume();
}

int OpenCDDevice(void)
{
    DebugLog("OpenCDDevice (PSP ATRAC3)\n");
    s_musicReady = 1;
    g_mciDeviceId = 1;
    return 1;
}

static void music_request(int track, int loop)
{
    s_reqTrack = track;
    s_reqLoop = loop;
    s_musicPlaying = track != 0;   /* until the mixer thread says otherwise */
    s_reqSeq++;
}

static void music_stop_stream(void)
{
    music_request(0, 0);
}

void CloseCDDevice(void)
{
    music_stop_stream();
    s_currentTrack = 0;
    s_musicReady = 0;
    g_mciDeviceId = 0;
}

void StopCD(void);

/* PlayCD — replaces MCI play. trackNum = CD track number (2-21). */
void PlayCD(int trackNum)
{
    if (trackNum < 2 || trackNum > 21) {
        return;
    }
    if (g_musicEnabled == 0) {
        DebugLog("PlayCD(%d): music off\n", trackNum);
        StopCD();
        return;
    }

    /* Already playing this track — don't restart (see music_sdl.c) */
    if (s_currentTrack == trackNum) {
        if (s_musicPlaying) {
            s_logicalTrack = trackNum;
            s_trackStartMs = timeGetTime();
            return;
        }
        if (s_currentTrackIsFanfare) {
            return;
        }
    }

    int isFanfare = (trackNum == 2 || trackNum == 3 || trackNum == 4 ||
                     trackNum == 0x13 || trackNum == 0x14 || trackNum == 0x15);

    music_apply_volume();
    DebugLog("PlayCD(%d): gain %d\n", trackNum, s_musicGain);
    music_request(trackNum, !isFanfare);   /* opened by the mixer thread */
    s_currentTrack = trackNum;
    s_currentTrackIsFanfare = isFanfare;
    s_logicalTrack = trackNum;              /* 0x4D01AC sets [0x6DA2A4] */
    s_trackStartMs = timeGetTime();         /* and [0x6DA2A8] */
}

/* StopCD - 0x004D0264 */
void StopCD(void)
{
    music_stop_stream();
    s_currentTrack = 0;
    s_currentTrackIsFanfare = 0;
    s_logicalTrack = 0;
    s_trackStartMs = 0;
    SFX_DuckStop();
}

/* GetLogicalCDTrack — 0x004D0100; see music_sdl.c */
int GetLogicalCDTrack(void)
{
    if (!s_musicReady || s_logicalTrack == 0) {
        return 0;
    }
    int elapsed = (int)(timeGetTime() - s_trackStartMs);
    if (elapsed < 0) {
        elapsed = -elapsed;
    }
    if (s_logicalTrack > 0 && s_logicalTrack < 23 &&
        elapsed > s_trackDurationMs[s_logicalTrack]) {
        s_logicalTrack++;
    }
    return s_logicalTrack;
}

/* UpdateCDPlayback — 0x004D01AC: in the original, this IS PlayCD. */
void UpdateCDPlayback(int trackNum)
{
    if (g_musicEnabled == 0) {
        return;
    }
    PlayCD(trackNum);
}

/* =====================================================================
 * Effects: WAV loading
 * ===================================================================== */

/* Slot-to-filename mapping — extracted from FUN_004d07d4 in SONICR.EXE. */
static const struct { int slot; const char *filename; } s_sfxTable[] = {
    { 0x00, "PAUSE.WAV"    }, { 0x01, "CHOOSE.WAV"   }, { 0x02, "SELECT.WAV"   },
    { 0x03, "RUNLEFT.WAV"  }, { 0x04, "RUNRIGHT.WAV" }, { 0x05, "AMY.WAV"      },
    { 0x06, "JET.WAV"      }, { 0x07, "JUMP.WAV"     }, { 0x08, "SPIN.WAV"     },
    { 0x09, "SPINGO.WAV"   }, { 0x0A, "SPINREV.WAV"  }, { 0x0B, "TAILS.WAV"    },
    { 0x0D, "JUMP.WAV"     }, { 0x0E, "FIRE.WAV"     }, { 0x0F, "EXPLODE.WAV"  },
    { 0x10, "AMYSKID.WAV"  }, { 0x11, "AMYWATER.WAV" }, { 0x12, "WATERRUN.WAV" },
    { 0x13, "WATERRUN.WAV" }, { 0x14, "BUBBLE.WAV"   }, { 0x15, "SPLASH.WAV"   },
    { 0x16, "POP.WAV"      }, { 0x18, "HITCHAR.WAV"  }, { 0x1A, "BONUS.WAV"    },
    { 0x1B, "GETTOKEN.WAV" }, { 0x1C, "GETCHAOS.WAV" }, { 0x1D, "RING1.WAV"    },
    { 0x1E, "RING1.WAV"    }, { 0x1F, "WARP.WAV"     }, { 0x20, "SKID1.WAV"    },
    { 0x21, "DOOR.WAV"     }, { 0x22, "RECORD.WAV"   }, { 0x23, "GOTALL.WAV"   },
    { 0x24, "BONUS.WAV"    }, { 0x27, "TAG.WAV"      }, { 0x2D, "THUNDER.WAV"  },
    { 0x32, "SPRING.WAV"   }, { 0x33, "BUMPER1.WAV"  }, { 0x34, "BUMPER2.WAV"  },
    { 0x35, "READY.WAV"    }, { 0x36, "SET.WAV"      }, { 0x37, "GO.WAV"       },
    { -1,   NULL           }
};

/* Read a PCM WAV (8 or 16 bit, mono or stereo, any rate) as 16-bit mono at
 * SFX_BASE_RATE — the 1998 files are 22050 mono but for AMY.WAV (44100) and
 * THUNDER.WAV (8-bit). Mix_LoadWAV likewise converts every file so it plays
 * at its natural pitch. */
static short *load_wav(const char *path, int *outFrames)
{
    FILE *f = sr_fOpen(path, "rb");   /* (from SONICR.PAK, as the game's files: platform_psp.c) */
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *file = (unsigned char *)malloc(size);
    if (!file || fread(file, 1, size, f) != (size_t)size || size < 12 ||
        memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) {
        free(file);
        sr_fClose(f);
        return NULL;
    }
    sr_fClose(f);

    int channels = 0, rate = 0, bits = 0, fmtTag = 0;
    const unsigned char *data = NULL;
    unsigned dataSize = 0;
    long i = 12;
    while (i + 8 <= size) {
        unsigned csz = file[i + 4] | (file[i + 5] << 8) | (file[i + 6] << 16) | ((unsigned)file[i + 7] << 24);
        if (memcmp(file + i, "fmt ", 4) == 0 && i + 24 <= size) {
            fmtTag   = file[i + 8] | (file[i + 9] << 8);
            channels = file[i + 10] | (file[i + 11] << 8);
            rate     = file[i + 12] | (file[i + 13] << 8) | (file[i + 14] << 16) | (file[i + 15] << 24);
            bits     = file[i + 22] | (file[i + 23] << 8);
        } else if (memcmp(file + i, "data", 4) == 0) {
            data = file + i + 8;
            dataSize = csz;
            if (i + 8 + (long)dataSize > size) {
                dataSize = (unsigned)(size - i - 8);
            }
            break;
        }
        i += 8 + csz + (csz & 1);
    }
    if (!data || fmtTag != 1 || channels < 1 || (bits != 8 && bits != 16) || rate <= 0) {
        free(file);
        return NULL;
    }

    int bytesPerFrame = channels * bits / 8;
    int srcFrames = (int)(dataSize / bytesPerFrame);
    int frames = (int)((long long)srcFrames * SFX_BASE_RATE / rate);
    short *pcm = (short *)malloc((frames > 0 ? frames : 1) * sizeof(short));
    if (!pcm) {
        free(file);
        return NULL;
    }
    for (int o = 0; o < frames; o++) {
        int s = (int)((long long)o * rate / SFX_BASE_RATE);
        const unsigned char *p = data + (size_t)s * bytesPerFrame;
        int acc = 0;
        for (int c = 0; c < channels; c++) {
            acc += (bits == 8) ? ((int)p[c] - 128) << 8 : (short)(p[c * 2] | (p[c * 2 + 1] << 8));
        }
        pcm[o] = (short)(acc / channels);
    }
    free(file);
    *outFrames = frames;
    return pcm;
}

static int LoadWAVIntoSlot(int slot, const char *filename)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return 0;
    }

    /* Release whatever the slot holds (reloaded once per replay). */
    if (s_slot[slot].pcm != NULL) {
        lock();
        s_voice[slot].active = 0;
        free(s_slot[slot].pcm);
        s_slot[slot].pcm = NULL;
        s_slot[slot].frames = 0;
        unlock();
        g_soundBuffers[slot] = NULL;
        g_soundActive[slot] = 0;
    }

    char path[256];
    snprintf(path, sizeof(path), DATA_DIR "/SOUND/SFX/%s", filename);
    int frames = 0;
    short *pcm = load_wav(path, &frames);
    if (pcm == NULL) {
        DebugLog("SFX: failed to load slot 0x%02X (%s)\n", slot, filename);
        return 0;
    }

    lock();
    s_slot[slot].pcm = pcm;
    s_slot[slot].frames = frames;
    unlock();

    g_soundBuffers[slot] = (void *)(intptr_t)1;
    g_soundActive[slot] = 1;
    return 1;
}

/* =====================================================================
 * Effects API (sfx_sdl.c)
 * ===================================================================== */

/* g_masterVolume -> linear 0..1, as sfx_sdl.c: a linear mixer, so the same
 * taper as SDL_mixer's. */
#define SFX_CURVE_EXP 1.5f
static float sfx_ds_linear(int dsVolume)
{
    if (dsVolume <= g_volumeBase) {
        return 0.0f;
    }
    if (dsVolume >= 0) {
        return 1.0f;
    }
    float t = (float)(dsVolume - g_volumeBase) / (float)(-g_volumeBase);
    return powf(t, SFX_CURVE_EXP);
}

/* Per-slot mix trim, 256 = unity (see sfx_sdl.c): the Tails rotor loop at 60%. */
#define SFX_TRIM_UNITY 256
static const short s_sfxSlotTrim[SFX_MAX_SLOTS] = {
    [0x0B] = 154,
};

static float sfx_slot_trim(int slot)
{
    int t = s_sfxSlotTrim[slot];
    return t <= 0 ? 1.0f : (float)t / (float)SFX_TRIM_UNITY;
}

void InitDirectSound(void)
{
    DebugLog("InitDirectSound (PSP)\n");
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_voice[i].gainL = s_voice[i].gainR = 256;
    }
    s_sfxReady = 1;
    g_lpDirectSound = (void *)(intptr_t)1;
    g_initFeatureB = 1;                                /* 0x6D9AE8 */

    for (int i = 0; s_sfxTable[i].slot >= 0; i++) {
        LoadWAVIntoSlot(s_sfxTable[i].slot, s_sfxTable[i].filename);
    }
    SetAllSoundVolumes();
}

void SFX_Tick(void)
{
    /* Finished one-shots stop themselves in the mixer. */
}

static void voice_start(int slot, int gain256, int freqHz, int loop)
{
    SfxVoice *v = &s_voice[slot];
    static int s_logged;
    if (s_logged < 6) {
        s_logged++;
        DebugLog("SFX: slot 0x%02X gain %d freq %d%s (%d frames)\n", slot, gain256, freqHz,
                 loop ? " loop" : "", s_slot[slot].frames);
    }
    lock();
    v->pos = 0;
    v->frac = 0;
    v->loop = loop ? 1 : 0;
    v->freqHz = freqHz > 0 ? freqHz : SFX_BASE_RATE;
    v->gain = gain256;
    v->active = 1;
    unlock();
}

void SFX_Play(int slot, int loop, int freq)
{
    if (!s_sfxReady || slot < 0 || slot >= SFX_MAX_SLOTS || s_slot[slot].pcm == NULL) {
        return;
    }

    float vol = sfx_ds_linear(g_masterVolume);
    if (IS_REPLAY_VOICE_SLOT(slot)) {
        vol = 1.0f;   /* binary exempts the announcer slot from attenuation */
    }
    int gain256 = (int)(vol * 256.0f);

    if (loop) {
        /* Start the loop once; thereafter update pitch live in place (never
         * restart, see sfx_sdl.c). Volume is owned by SFX_SetVolume. */
        if (!s_voice[slot].active || !s_voice[slot].loop) {
            voice_start(slot, gain256, freq, 1);
            return;
        }
        if (freq) {
            s_voice[slot].freqHz = freq;
        }
        return;
    }

    /* The double-jump (slot 0x0D) is pitched up, as on DC and SDL. */
    if (slot == 0xD) {
        freq = 22050 + 5512;
    }
    voice_start(slot, gain256, freq, 0);
}

void SFX_Stop(int slot)
{
    if (!s_sfxReady || slot < 0 || slot >= SFX_MAX_SLOTS) {
        return;
    }
    s_voice[slot].active = 0;
}

void SFX_StopAll(void)
{
    if (!s_sfxReady) {
        return;
    }
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_voice[i].active = 0;
    }
}

void SFX_SetVolume(int slot, int dsVolume)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS || s_slot[slot].pcm == NULL) {
        return;
    }
    s_voice[slot].gain = (int)(sfx_ds_linear(dsVolume) * sfx_slot_trim(slot) * 256.0f);
}

void SFX_SetPan(int slot, int dsPan)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS || s_slot[slot].pcm == NULL) {
        return;
    }
    /* DirectSound pan: -10000..10000 */
    float pan = (float)dsPan / 10000.0f;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    float l = 256.0f * (1.0f - pan), r = 256.0f * (1.0f + pan);
    s_voice[slot].gainL = (int)(l > 256.0f ? 256.0f : l);
    s_voice[slot].gainR = (int)(r > 256.0f ? 256.0f : r);
}

void SFX_SetPosition(int slot, int pos)
{
    (void)slot;
    (void)pos;
}

void CloseDirectSound(void)
{
    s_sfxReady = 0;
    lock();
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_voice[i].active = 0;
        free(s_slot[i].pcm);
        s_slot[i].pcm = NULL;
        s_slot[i].frames = 0;
        g_soundBuffers[i] = NULL;
        g_soundActive[i] = 0;
    }
    unlock();
    g_lpDirectSound = NULL;
    DebugLog("SFX: closed\n");
}

/* FUN_004d02d4 — simple one-shot play: rewind + play, no volume/pan. */
static int PlaySoundSimple(int slot)                     /* 0x4d02d4 */
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return 0;
    }
    if (g_soundActive[slot] == 0) {
        return 0;
    }
    if (g_optSfxVolume == 0) {
        return 1;                     /* 0x4d02e7: sound disabled */
    }
    SFX_Play(slot, 0, 0);
    return 1;
}

/* FUN_004d0330 — play with volume/pan control (looping).
 * EAX = slot, EDX = distance, EBX = frequency offset. */
static int PlaySoundWithParams(int slot, int distance, int freqParam) /* 0x4d0330 */
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return 0;
    }
    if (g_soundActive[slot] == 0) {
        return 0;
    }
    if (g_optSfxVolume == 0) {
        return 1;                     /* 0x4d034d: sound disabled */
    }

    /* Volume attenuation from distance                           0x4d0389 */
    if (distance != 0x100) {
        int d = distance;
        if (d < 0x40) {
            d = 0xFF;                                   /* 0x4d0396 */
        } else {
            d = 0xFF - d;                               /* 0x4d03a2 */
        }
        int volDiff = g_masterVolume - g_volumeBase;    /* 0x4d03a6 */
        if (volDiff < 0) {
            volDiff = -volDiff;
        }
        int divisor = (g_demoMode == 2) ? 0x12c : 0xff; /* 0x4d03bb */
        int attenVol = (d * volDiff) / divisor + g_volumeBase;  /* 0x4d03d7 */
        SFX_SetVolume(slot, attenVol);
    }

    /* Frequency param -> absolute Hz (binary SetFrequency, 0x4d035a). */
    if (freqParam != 0) {
        freqParam = (freqParam * 99900) / 255 + 100;
    }

    SFX_Play(slot, 1, freqParam);   /* looping — binary: Play(buf, 0, 0, 1) 0x4d03ed */
    return 1;
}

static int g_ringAlternate;                              /* 0x00901CBC */

/* PlaySound — 0x00482280 — 111 bytes. Sound effect dispatcher. */
void PlaySoundEffect(int soundCmd, int distance, int freqParam)
{                                                        /* 0x482280 */
    int slot = soundCmd & 0xFFFF;

    if (soundCmd & 0xFFFF0000) {
        PlaySoundWithParams(slot, distance, freqParam);  /* 0x4822a0 */
    } else {
        PlaySoundSimple(slot);                           /* 0x4822a7 */
    }

    if (g_demoMode == DEMO_REPLAY) {
        return;
    }

    /* Ring sound alternation: alternate between 0x1D and 0x1E */
    if ((soundCmd & 0xFFFF) == 0x1D) {
        slot = 0x1D + g_ringAlternate;
        g_ringAlternate = (g_ringAlternate + 1) & 1;
    }
}

/* LoadSoundEffect — 0x004D064C — LoadWAV(EAX=filename, EDX=slot) */
void LoadSoundEffect(const char *filename, int slot)
{
    LoadWAVIntoSlot(slot, filename);
}

int SFX_ClipDurationMs(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS || s_slot[slot].pcm == NULL) {
        return 0;
    }
    return (int)((long long)s_slot[slot].frames * 1000 / SFX_BASE_RATE);
}
