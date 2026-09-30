/* android_stubs.c -- NDK entry points that have nothing behind them here.
 *
 * Sensors: Defold asks for the accelerometer only when use_accelerometer is
 * set, and Level Devil's game.project has it off. Every call reports "no such
 * sensor" the way a device without one does. The GLES1 stubs are for entry
 * points switch-mesa's GLES2 does not export.
 *
 * MIT licensed, see LICENSE.
 */
#include <stddef.h>
#include <stdint.h>

#include "android_stubs.h"
#include "log.h"

void *ax_ASensorManager_getInstance(void) { return NULL; }
void *ax_ASensorManager_getDefaultSensor(void *mgr, int type) { (void)mgr; (void)type; return NULL; }

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
int  ax_ASensorManager_destroyEventQueue(void *mgr, void *q)      { (void)mgr; (void)q; return 0; }
int  ax_ASensorEventQueue_enableSensor(void *q, void *s)          { (void)q; (void)s; return -1; }
int  ax_ASensorEventQueue_disableSensor(void *q, void *s)         { (void)q; (void)s; return -1; }
int  ax_ASensorEventQueue_setEventRate(void *q, void *s, int32_t u) { (void)q; (void)s; (void)u; return -1; }
long ax_ASensorEventQueue_getEvents(void *q, void *ev, size_t n)  { (void)q; (void)ev; (void)n; return 0; }

#define GLR(name)
#define GLS(name) \
    unsigned long ax_##name(void) { LOG_ONCE("stub: " #name " (GLES1 path, unexpected)"); return 0; }
#include "gl_imports.h"
#undef GLR
#undef GLS
