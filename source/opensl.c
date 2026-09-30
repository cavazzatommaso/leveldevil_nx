/* opensl.c -- enough OpenSL ES for Defold's sound device, played via audout.
 *
 * Defold's Android sound device (engine/sound/src/devices/device_opensl.cpp)
 * does the textbook thing: slCreateEngine, an output mix, one audio player fed
 * by a buffer queue of 16-bit PCM, and a buffer-queue callback that returns
 * each played buffer to its free list. DeviceOpenSLFreeBufferSlots is how the
 * mixer paces itself, so the callback firing is what keeps sound going.
 *
 * Only what that path needs is real: Engine.CreateOutputMix and
 * CreateAudioPlayer, Object.Realize/GetInterface/Destroy, Play.SetPlayState,
 * BufferQueue.Enqueue/Clear/GetState/RegisterCallback and Volume. Every other
 * slot answers SL_RESULT_FEATURE_UNSUPPORTED.
 *
 * Interface handles are pointers to a pointer to a vtable, and the slot order
 * is fixed by the OpenSL ES 1.0.1 headers (SLES/OpenSLES.h). A slot out of
 * place still compiles and then calls the wrong function, so every vtable
 * below is written out in header order with the header's slot names.
 *
 * One thread per player drains the queue: each buffer is converted to 48 kHz
 * stereo (audout's only format), written to audout (which blocks, and that is
 * the pacing), and only then handed back through the callback -- the same
 * order a phone reports it in.
 *
 * MIT licensed, see LICENSE.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "audio_out.h"
#include "log.h"
#include "opensl.h"
#include "tls.h"

typedef uint32_t SLresult;
#define SL_RESULT_SUCCESS               0x00
#define SL_RESULT_PARAMETER_INVALID     0x02
#define SL_RESULT_MEMORY_FAILURE        0x03
#define SL_RESULT_BUFFER_INSUFFICIENT   0x07
#define SL_RESULT_FEATURE_UNSUPPORTED   0x0C

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3
#define SL_OBJECT_STATE_REALIZED 2

#define OUT_RATE    48000
#define OUT_CHUNK   1024          /* frames per audout write */
#define MAX_QUEUE   16

/* ------------------------------------------------------------- IIDs ---- */
typedef struct { uint32_t a; uint16_t b, c, d; uint8_t e[6]; } SlGuid;

static const SlGuid g_iid_engine = { 0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SlGuid g_iid_bq     = { 0x2bc99cc0, 0xddd4, 0x11db, 0x8d99, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SlGuid g_iid_play   = { 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SlGuid g_iid_volume = { 0x6a23fc60, 0xddd4, 0x11db, 0xaf87, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SlGuid g_iid_sbq    = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

const void *ax_SL_IID_ENGINE = &g_iid_engine;
const void *ax_SL_IID_BUFFERQUEUE = &g_iid_bq;
const void *ax_SL_IID_PLAY = &g_iid_play;
const void *ax_SL_IID_VOLUME = &g_iid_volume;
const void *ax_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &g_iid_sbq;

static int iid_is(const void *iid, const SlGuid *g)
{
    return iid == g || (iid && !memcmp(iid, g, sizeof(*g)));
}

/* ------------------------------------------------------------ objects -- */

typedef const void *const *Itf;    /* what the game holds: &obj->vtbl */

static SLresult unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult ok(void)          { return SL_RESULT_SUCCESS; }
#define NA ((void *)unsupported)
#define OK ((void *)ok)

typedef struct Player Player;

typedef struct {
    const void *const *vtbl;       /* SLObjectItf points here */
    int kind;                      /* 0 engine, 1 output mix, 2 player */
    Player *player;
} SlObject;

typedef struct {
    const void *const *vtbl;
    SlObject *owner;
} SlItf;

struct Player {
    SlObject obj;
    SlItf    play, bq, volume;
    int      channels;
    uint32_t rate;
    int      state;
    void   (*cb)(Itf, void *);
    void    *cb_ctx;
    struct { const int16_t *data; uint32_t bytes; } q[MAX_QUEUE];
    int      qhead, qlen, qcap;
    uint32_t played;
    float    gain;
    int      quit;
    Mutex    lock;
    CondVar  cv;
    Thread   thread;
    /* resampler state, carried across buffers */
    double   phase;
    int16_t  last[2];
};

#define OWNER(itf) (((const SlItf *)(itf))->owner)

/* Object: Realize Resume GetState GetInterface RegisterCallback
 *         AbortAsyncOperation Destroy SetPriority GetPriority
 *         SetLossOfControlInterfaces */
static SLresult obj_Realize(Itf self, uint32_t async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(Itf self, uint32_t *st) { (void)self; if (st) *st = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_GetInterface(Itf self, const void *iid, void *out);
static void     obj_Destroy(Itf self);

static const void *const g_object_vt[] = {
    (void *)obj_Realize, OK, (void *)obj_GetState, (void *)obj_GetInterface, OK,
    OK, (void *)obj_Destroy, OK, OK, OK,
};

/* Engine: CreateLEDDevice CreateVibraDevice CreateAudioPlayer
 *         CreateAudioRecorder CreateMidiPlayer CreateListener Create3DGroup
 *         CreateOutputMix CreateMetadataExtractor CreateExtensionObject
 *         QueryNumSupportedInterfaces QuerySupportedInterfaces
 *         QueryNumSupportedExtensions QuerySupportedExtension
 *         IsExtensionSupported */
static SLresult eng_CreateAudioPlayer(Itf self, Itf *out, const void *src, const void *snk,
                                      uint32_t n, const void *ids, const uint32_t *req);
static SLresult eng_CreateOutputMix(Itf self, Itf *out, uint32_t n, const void *ids, const uint32_t *req);

static const void *const g_engine_vt[] = {
    NA, NA, (void *)eng_CreateAudioPlayer, NA, NA, NA, NA, (void *)eng_CreateOutputMix,
    NA, NA, NA, NA, NA, NA, NA,
};

/* Play: SetPlayState GetPlayState GetDuration GetPosition RegisterCallback
 *       SetCallbackEventsMask GetCallbackEventsMask SetMarkerPosition
 *       ClearMarkerPosition GetMarkerPosition SetPositionUpdatePeriod
 *       GetPositionUpdatePeriod */
static SLresult play_SetPlayState(Itf self, uint32_t state);
static SLresult play_GetPlayState(Itf self, uint32_t *state);

static const void *const g_play_vt[] = {
    (void *)play_SetPlayState, (void *)play_GetPlayState, NA, NA, NA, NA, NA, NA, NA, NA, NA, NA,
};

/* BufferQueue: Enqueue Clear GetState RegisterCallback. Android's simple
 * buffer queue has the same four slots in the same order. */
static SLresult bq_Enqueue(Itf self, const void *buf, uint32_t size);
static SLresult bq_Clear(Itf self);
static SLresult bq_GetState(Itf self, uint32_t *state);
static SLresult bq_RegisterCallback(Itf self, void (*cb)(Itf, void *), void *ctx);

static const void *const g_bq_vt[] = {
    (void *)bq_Enqueue, (void *)bq_Clear, (void *)bq_GetState, (void *)bq_RegisterCallback,
};

/* Volume: SetVolumeLevel GetVolumeLevel GetMaxVolumeLevel SetMute GetMute
 *         EnableStereoPosition IsEnabledStereoPosition SetStereoPosition
 *         GetStereoPosition */
static SLresult vol_SetVolumeLevel(Itf self, int16_t mb);
static SLresult vol_GetVolumeLevel(Itf self, int16_t *mb);
static SLresult vol_GetMaxVolumeLevel(Itf self, int16_t *mb) { (void)self; if (mb) *mb = 0; return SL_RESULT_SUCCESS; }

static const void *const g_volume_vt[] = {
    (void *)vol_SetVolumeLevel, (void *)vol_GetVolumeLevel, (void *)vol_GetMaxVolumeLevel,
    OK, OK, OK, OK, OK, OK,
};

typedef struct { SlObject obj; SlItf engine; } Engine;
static Engine   g_engine;
static SlObject g_mix;

static SLresult obj_GetInterface(Itf self, const void *iid, void *out)
{
    SlObject *o = (SlObject *)self;
    void **pout = out;

    if (!pout)
        return SL_RESULT_PARAMETER_INVALID;
    if (o->kind == 0 && iid_is(iid, &g_iid_engine)) {
        *pout = &g_engine.engine;
        return SL_RESULT_SUCCESS;
    }
    if (o->kind == 2) {
        Player *p = o->player;
        if (iid_is(iid, &g_iid_play))   { *pout = &p->play; return SL_RESULT_SUCCESS; }
        if (iid_is(iid, &g_iid_bq) || iid_is(iid, &g_iid_sbq)) { *pout = &p->bq; return SL_RESULT_SUCCESS; }
        if (iid_is(iid, &g_iid_volume)) { *pout = &p->volume; return SL_RESULT_SUCCESS; }
    }
    LOGI("opensl: GetInterface on object kind %d for an interface this port lacks", o->kind);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

/* ------------------------------------------------------------- player -- */

static void emit(Player *p, int16_t *out, int *n)
{
    if (*n) {
        pbaudio_write_s16(out, (size_t)*n);
        *n = 0;
    }
    (void)p;
}

static void frame_at(const Player *p, const int16_t *d, long i, int16_t fr[2])
{
    if (i < 0) {
        fr[0] = p->last[0];
        fr[1] = p->last[1];
    } else if (p->channels == 1) {
        fr[0] = fr[1] = d[i];
    } else {
        fr[0] = d[2 * i];
        fr[1] = d[2 * i + 1];
    }
}

static int16_t scale(int32_t v, float g)
{
    float f = (float)v * g;
    if (f > 32767.f) f = 32767.f;
    if (f < -32768.f) f = -32768.f;
    return (int16_t)f;
}

/* Convert one buffer to 48 kHz stereo and play it (blocking). */
static void play_buffer(Player *p, const int16_t *d, uint32_t bytes)
{
    static int16_t out[OUT_CHUNK * 2];
    long frames = (long)(bytes / (uint32_t)(p->channels * 2));
    double step = (double)p->rate / OUT_RATE;
    float g = p->gain;
    int n = 0;

    if (frames <= 0)
        return;
    while (p->phase < (double)(frames - 1)) {
        long i = (long)floor(p->phase);
        double f = p->phase - (double)i;
        int16_t a[2], b[2];
        frame_at(p, d, i, a);
        frame_at(p, d, i + 1, b);
        out[2 * n]     = scale((int32_t)(a[0] + (b[0] - a[0]) * f), g);
        out[2 * n + 1] = scale((int32_t)(a[1] + (b[1] - a[1]) * f), g);
        if (++n == OUT_CHUNK)
            emit(p, out, &n);
        p->phase += step;
    }
    emit(p, out, &n);
    p->phase -= (double)frames;
    frame_at(p, d, frames - 1, p->last);
}

static void player_main(void *arg)
{
    Player *p = arg;
    pb_tls_attach("the OpenSL player");   /* the callback is module code */

    for (;;) {
        const int16_t *data;
        uint32_t bytes;

        mutexLock(&p->lock);
        while (!p->quit && (p->state != SL_PLAYSTATE_PLAYING || !p->qlen))
            condvarWaitTimeout(&p->cv, &p->lock, 100000000ULL);
        if (p->quit) {
            mutexUnlock(&p->lock);
            break;
        }
        data = p->q[p->qhead].data;
        bytes = p->q[p->qhead].bytes;
        mutexUnlock(&p->lock);

        play_buffer(p, data, bytes);

        mutexLock(&p->lock);
        if (p->qlen && p->q[p->qhead].data == data) {   /* not Cleared meanwhile */
            p->qhead = (p->qhead + 1) % MAX_QUEUE;
            p->qlen--;
            p->played++;
        }
        mutexUnlock(&p->lock);
        if (p->cb)
            p->cb((Itf)&p->bq, p->cb_ctx);
    }
    pb_tls_detach();
}

static SLresult eng_CreateAudioPlayer(Itf self, Itf *out, const void *src, const void *snk,
                                      uint32_t n, const void *ids, const uint32_t *req)
{
    /* SLDataSource { pLocator, pFormat }; SLDataLocator_BufferQueue
     * { locatorType, numBuffers }; SLDataFormat_PCM { formatType,
     * numChannels, samplesPerSec (mHz), bitsPerSample, containerSize,
     * channelMask, endianness } */
    const void *const *source = src;
    const uint32_t *loc, *fmt;
    Player *p;
    Result rc;
    (void)self; (void)snk; (void)n; (void)ids; (void)req;

    if (!out || !source || !source[0] || !source[1])
        return SL_RESULT_PARAMETER_INVALID;
    loc = source[0];
    fmt = source[1];
    if (fmt[0] != 2 || fmt[3] != 16) {
        LOGE("opensl: unsupported source format %u, %u bits (want PCM 16)", fmt[0], fmt[3]);
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }

    p = calloc(1, sizeof(*p));
    if (!p)
        return SL_RESULT_MEMORY_FAILURE;
    p->obj.vtbl = g_object_vt;
    p->obj.kind = 2;
    p->obj.player = p;
    p->play.vtbl = g_play_vt;     p->play.owner = &p->obj;
    p->bq.vtbl = g_bq_vt;         p->bq.owner = &p->obj;
    p->volume.vtbl = g_volume_vt; p->volume.owner = &p->obj;
    p->channels = fmt[1] == 1 ? 1 : 2;
    p->rate = fmt[2] / 1000;
    if (!p->rate)
        p->rate = OUT_RATE;
    p->qcap = loc[1] > 0 && loc[1] <= MAX_QUEUE ? (int)loc[1] : MAX_QUEUE;
    p->state = SL_PLAYSTATE_STOPPED;
    p->gain = 1.0f;
    mutexInit(&p->lock);
    condvarInit(&p->cv);

    if (!pbaudio_is_open() && !pbaudio_open(OUT_RATE, 2, OUT_CHUNK))
        LOGE("opensl: audout did not open; the game will be silent");

    rc = threadCreate(&p->thread, player_main, p, NULL, 64 * 1024, 0x2B, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&p->thread);
    if (R_FAILED(rc)) {
        LOGE("opensl: player thread failed 0x%x", rc);
        free(p);
        return SL_RESULT_MEMORY_FAILURE;
    }
    LOGI("opensl: player %u Hz, %d ch, %d buffers%s", p->rate, p->channels, p->qcap,
         p->rate == OUT_RATE ? "" : " (resampled to 48 kHz)");
    *out = (Itf)&p->obj;
    return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(Itf self, Itf *out, uint32_t n, const void *ids, const uint32_t *req)
{
    (void)self; (void)n; (void)ids; (void)req;
    if (!out)
        return SL_RESULT_PARAMETER_INVALID;
    *out = (Itf)&g_mix;
    return SL_RESULT_SUCCESS;
}

static void obj_Destroy(Itf self)
{
    SlObject *o = (SlObject *)self;
    Player *p;
    if (o->kind != 2)
        return;
    p = o->player;
    mutexLock(&p->lock);
    p->quit = 1;
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
    threadWaitForExit(&p->thread);
    threadClose(&p->thread);
    free(p);
    LOGI("opensl: player destroyed");
}

static SLresult play_SetPlayState(Itf self, uint32_t state)
{
    Player *p = OWNER(self)->player;
    mutexLock(&p->lock);
    p->state = (int)state;
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(Itf self, uint32_t *state)
{
    if (state)
        *state = (uint32_t)OWNER(self)->player->state;
    return SL_RESULT_SUCCESS;
}

static SLresult bq_Enqueue(Itf self, const void *buf, uint32_t size)
{
    Player *p = OWNER(self)->player;
    SLresult r = SL_RESULT_SUCCESS;
    mutexLock(&p->lock);
    if (p->qlen >= p->qcap) {
        r = SL_RESULT_BUFFER_INSUFFICIENT;
    } else {
        int i = (p->qhead + p->qlen) % MAX_QUEUE;
        p->q[i].data = buf;
        p->q[i].bytes = size;
        p->qlen++;
        condvarWakeAll(&p->cv);
    }
    mutexUnlock(&p->lock);
    return r;
}

static SLresult bq_Clear(Itf self)
{
    Player *p = OWNER(self)->player;
    mutexLock(&p->lock);
    p->qlen = 0;
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(Itf self, uint32_t *state)
{
    Player *p = OWNER(self)->player;
    if (!state)
        return SL_RESULT_PARAMETER_INVALID;
    mutexLock(&p->lock);
    state[0] = (uint32_t)p->qlen;       /* count */
    state[1] = p->played;               /* playIndex */
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(Itf self, void (*cb)(Itf, void *), void *ctx)
{
    Player *p = OWNER(self)->player;
    mutexLock(&p->lock);
    p->cb = cb;
    p->cb_ctx = ctx;
    mutexUnlock(&p->lock);
    return SL_RESULT_SUCCESS;
}

static SLresult vol_SetVolumeLevel(Itf self, int16_t mb)
{
    /* millibels: 0 is full volume, -9600 (SL_MILLIBEL_MIN area) silence */
    OWNER(self)->player->gain = mb <= -9600 ? 0.0f : powf(10.0f, (float)mb / 2000.0f);
    return SL_RESULT_SUCCESS;
}

static SLresult vol_GetVolumeLevel(Itf self, int16_t *mb)
{
    (void)self;
    if (mb)
        *mb = 0;
    return SL_RESULT_SUCCESS;
}

/* --------------------------------------------------------- the entry --- */

uint32_t ax_slCreateEngine(void **engine, uint32_t nopts, const void *opts,
                           uint32_t nifaces, const void *ids, const void *req)
{
    (void)nopts; (void)opts; (void)nifaces; (void)ids; (void)req;
    if (!engine)
        return SL_RESULT_PARAMETER_INVALID;
    g_engine.obj.vtbl = g_object_vt;
    g_engine.obj.kind = 0;
    g_engine.engine.vtbl = g_engine_vt;
    g_engine.engine.owner = &g_engine.obj;
    g_mix.vtbl = g_object_vt;
    g_mix.kind = 1;
    *engine = &g_engine.obj;
    LOGI("opensl: engine created");
    return SL_RESULT_SUCCESS;
}
