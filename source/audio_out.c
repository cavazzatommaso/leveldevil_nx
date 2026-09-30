/* audio_out.c -- sound out, via libnx audout.
 *
 * WHY AUDIO GOES THROUGH JNI HERE
 *
 * liblime.so contains OpenAL Soft (backends: opensl, sdl2, wave, null) and
 * SDL 2.30.12's Android audio drivers (openslES, android, dummy). The Total
 * Party Kill port forced OpenAL onto its sdl2 backend and let switch-sdl2
 * play the result, but there is no switch-sdl2 in this port: SDL's Android
 * backend is the one that runs.
 *
 * That leaves two routes to the speakers, and the choice is about risk
 * rather than taste:
 *
 *   openslES   OpenAL, or SDL, would call slCreateEngine and then dispatch
 *              through OpenSL ES interface structs. Those are vtables whose
 *              slot ORDER is fixed by the OpenSL ES 1.0.1 spec, and a
 *              hand-written struct whose order is off by one field does not
 *              fail to compile -- it calls the wrong function with the wrong
 *              arguments, on the audio thread, in release.
 *
 *   android    SDL's AudioTrack driver, which reaches Java and nothing else:
 *              SDLAudioManager.audioOpen(freq, fmt, chans, samples, dev)
 *              returning int[4], then audioWriteShortBuffer(short[]) once per
 *              buffer. Dispatch is by name and signature through the port's
 *              own JNI layer, which cannot silently land on the wrong slot.
 *
 * So the port takes the second route: SDL_AUDIODRIVER=android, OpenAL on
 * ALSOFT_DRIVERS=sdl2, and jni_classes.c's SDLAudioManager calls into this
 * file. The OpenSL entry points stay stubbed in android_stubs.c and say so if
 * anything ever reaches them.
 *
 * audout is fixed at 48 kHz stereo signed 16-bit, which is what audioOpen
 * reports back, so SDL converts to it once and never resamples per buffer.
 *
 * MIT licensed, see LICENSE.
 */
#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "audio_out.h"
#include "log.h"

#define PB_AUDIO_RATE     48000
#define PB_AUDIO_CHANNELS 2
#define PB_AUDIO_BUFFERS  3

static AudioOutBuffer  g_buf[PB_AUDIO_BUFFERS];
static void           *g_mem[PB_AUDIO_BUFFERS];
static size_t          g_bufsize;          /* aligned byte size of each */
static int             g_next;
static int             g_queued;
static int             g_open;

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

int pbaudio_open(int freq, int channels, int samples)
{
    size_t frame_bytes, want;
    Result rc;
    int i;

    if (g_open)
        return PB_AUDIO_RATE;

    if (freq != PB_AUDIO_RATE || channels != PB_AUDIO_CHANNELS)
        LOGI("audio: the game asked for %d Hz / %d ch; audout is %d Hz / %d ch, "
             "so SDL will convert", freq, channels, PB_AUDIO_RATE, PB_AUDIO_CHANNELS);

    rc = audoutInitialize();
    if (R_FAILED(rc)) {
        LOGE("audio: audoutInitialize failed 0x%x -- the game will run silent", rc);
        return 0;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        LOGE("audio: audoutStartAudioOut failed 0x%x", rc);
        audoutExit();
        return 0;
    }

    frame_bytes = (size_t)PB_AUDIO_CHANNELS * sizeof(s16);
    if (samples <= 0)
        samples = 1024;
    want = (size_t)samples * frame_bytes;
    /* audout wants page-aligned memory and a size it can DMA. */
    g_bufsize = align_up(want, 0x1000);

    for (i = 0; i < PB_AUDIO_BUFFERS; i++) {
        g_mem[i] = memalign(0x1000, g_bufsize);
        if (!g_mem[i]) {
            LOGE("audio: out of memory for a %zu KB mix buffer", g_bufsize / 1024);
            pbaudio_close();
            return 0;
        }
        memset(g_mem[i], 0, g_bufsize);
        memset(&g_buf[i], 0, sizeof(g_buf[i]));
        g_buf[i].next = NULL;
        g_buf[i].buffer = g_mem[i];
        g_buf[i].buffer_size = g_bufsize;
        g_buf[i].data_size = want;
        g_buf[i].data_offset = 0;
    }
    g_next = 0;
    g_queued = 0;
    g_open = 1;
    LOGI("audio: audout open, %d Hz stereo s16, %d buffers of %d frames",
         PB_AUDIO_RATE, PB_AUDIO_BUFFERS, samples);
    return PB_AUDIO_RATE;
}

void pbaudio_write_s16(const int16_t *frames, size_t nframes)
{
    AudioOutBuffer *done = NULL;
    size_t bytes;
    Result rc;
    int slot;

    if (!g_open || !frames || !nframes)
        return;

    bytes = nframes * PB_AUDIO_CHANNELS * sizeof(s16);
    if (bytes > g_bufsize)
        bytes = g_bufsize;

    /* Wait for a free slot once every buffer is in flight. This is the call
     * that paces the game's audio thread, which is exactly what AudioTrack's
     * blocking write does on Android. */
    if (g_queued >= PB_AUDIO_BUFFERS) {
        u32 count = 0;
        rc = audoutWaitPlayFinish(&done, &count, UINT64_MAX);
        if (R_FAILED(rc)) {
            LOG_ONCE("!! audio: audoutWaitPlayFinish failed 0x%x", rc);
            return;
        }
        g_queued -= (int)count;
        if (g_queued < 0)
            g_queued = 0;
    }

    slot = g_next;
    g_next = (g_next + 1) % PB_AUDIO_BUFFERS;

    memcpy(g_mem[slot], frames, bytes);
    if (bytes < g_bufsize)
        memset((char *)g_mem[slot] + bytes, 0, g_bufsize - bytes);
    g_buf[slot].data_size = bytes;

    rc = audoutAppendAudioOutBuffer(&g_buf[slot]);
    if (R_FAILED(rc)) {
        LOG_ONCE("!! audio: audoutAppendAudioOutBuffer failed 0x%x", rc);
        return;
    }
    g_queued++;
}

void pbaudio_close(void)
{
    int i;

    if (g_open) {
        audoutStopAudioOut();
        audoutExit();
        g_open = 0;
        LOGI("audio: closed");
    }
    for (i = 0; i < PB_AUDIO_BUFFERS; i++) {
        free(g_mem[i]);
        g_mem[i] = NULL;
    }
    g_queued = 0;
    g_next = 0;
}

int pbaudio_is_open(void) { return g_open; }
