/* android_stubs.c -- Android NDK entry points liblime.so imports but never
 * needs on this port.
 *
 * All of these are referenced by SDL's Android backend, OpenAL Soft's OpenSL
 * backend, or SDL's GLES1 renderer, all compiled into liblime.so. The port
 * sends every SDL call to switch-sdl2 and forces OpenAL onto its sdl2
 * backend, so none of this code runs -- but the modules are linked
 * BIND_NOW, so every symbol still needs an address. Each stub reports
 * "unavailable" the way the real API does on a device without the feature,
 * and logs the first time it is hit, which would mean one of those
 * assumptions broke.
 *
 * MIT licensed, see LICENSE.
 */
#include <stddef.h>
#include <stdint.h>

#include "android_stubs.h"   /* the declarations imports.c uses: including them
                             * here makes any mismatch a compile error */
#include "log.h"

#define HIT(name) LOG_ONCE("stub: %s called (unexpected on this port)", name)

/* ------------------------------------------------ ALooper / ANativeWindow -- */
void *ax_ALooper_forThread(void) { HIT("ALooper_forThread"); return NULL; }
void *ax_ALooper_prepare(int opts) { (void)opts; HIT("ALooper_prepare"); return NULL; }

int ax_ALooper_pollAll(int timeout, int *fd, int *events, void **data)
{
    (void)timeout; (void)fd; (void)events; (void)data;
    HIT("ALooper_pollAll");
    return -3;                                  /* ALOOPER_POLL_TIMEOUT */
}

void *ax_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    HIT("ANativeWindow_fromSurface");
    return NULL;
}
int  ax_ANativeWindow_getWidth(void *w)  { (void)w; return 1280; }
int  ax_ANativeWindow_getHeight(void *w) { (void)w; return 720; }
void ax_ANativeWindow_release(void *w)   { (void)w; }
int  ax_ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt)
{
    (void)w; (void)width; (void)height; (void)fmt;
    return 0;
}

/* ------------------------------------------------------------- ASensor -- */
void *ax_ASensorManager_getInstance(void) { HIT("ASensorManager_getInstance"); return NULL; }

int ax_ASensorManager_getSensorList(void *mgr, void **list)
{
    (void)mgr;
    if (list)
        *list = NULL;
    return 0;
}

void *ax_ASensorManager_createEventQueue(void *mgr, void *looper, int ident, void *cb, void *data)
{
    (void)mgr; (void)looper; (void)ident; (void)cb; (void)data;
    return NULL;
}
int ax_ASensorManager_destroyEventQueue(void *mgr, void *q) { (void)mgr; (void)q; return 0; }
int ax_ASensorEventQueue_enableSensor(void *q, void *s)     { (void)q; (void)s; return -1; }
int ax_ASensorEventQueue_disableSensor(void *q, void *s)    { (void)q; (void)s; return -1; }
long ax_ASensorEventQueue_getEvents(void *q, void *ev, size_t n) { (void)q; (void)ev; (void)n; return 0; }
const char *ax_ASensor_getName(void *s) { (void)s; return "none"; }
int ax_ASensor_getType(void *s) { (void)s; return 0; }

/* ----------------------------------------------------------- OpenSL ES -- */
/* SL_IID_* are exported as `const SLInterfaceID` -- pointers to 16-byte
 * GUIDs. Distinct GUIDs so nothing compares equal by accident. */
typedef struct { uint32_t a; uint16_t b, c, d; uint8_t e[6]; } ax_guid;

static const ax_guid g_iid_cfg    = { 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const ax_guid g_iid_sbq    = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const ax_guid g_iid_engine = { 0x8d97c260, 0xddd4, 0x11db, 0x958f, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const ax_guid g_iid_play   = { 0xef0bd9c0, 0xddd7, 0x11db, 0xbf49, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const ax_guid g_iid_record = { 0xc5657aa0, 0xdddb, 0x11db, 0x82f7, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const ax_guid g_iid_volume = { 0x6a23fc60, 0xddd4, 0x11db, 0xaf87, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

const void *ax_SL_IID_ANDROIDCONFIGURATION    = &g_iid_cfg;
const void *ax_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &g_iid_sbq;
const void *ax_SL_IID_ENGINE                  = &g_iid_engine;
const void *ax_SL_IID_PLAY                    = &g_iid_play;
const void *ax_SL_IID_RECORD                  = &g_iid_record;
const void *ax_SL_IID_VOLUME                  = &g_iid_volume;

uint32_t ax_slCreateEngine(void **engine, uint32_t nopts, const void *opts,
                           uint32_t nifaces, const void *ids, const void *req)
{
    (void)nopts; (void)opts; (void)nifaces; (void)ids; (void)req;
    LOG_ONCE("stub: slCreateEngine refused (audio goes through OpenAL's sdl2 backend)");
    if (engine)
        *engine = NULL;
    return 0x0000000C;                          /* SL_RESULT_FEATURE_UNSUPPORTED */
}

/* ------------------------------------------------ OpenGL ES 1.x / OES -- */
#define GLR(name)
#define GLS(name) \
    unsigned long ax_##name(void) { LOG_ONCE("stub: " #name " (GLES1 path, unexpected)"); return 0; }
#include "gl_imports.h"
#undef GLR
#undef GLS
