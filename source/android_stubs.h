/* android_stubs.h -- see android_stubs.c. MIT licensed, see LICENSE. */
#ifndef HS_ANDROID_STUBS_H
#define HS_ANDROID_STUBS_H

#include <stddef.h>
#include <stdint.h>

void *ax_ALooper_forThread(void);
void *ax_ALooper_prepare(int opts);
int   ax_ALooper_pollAll(int timeout, int *fd, int *events, void **data);
void *ax_ANativeWindow_fromSurface(void *env, void *surface);
int   ax_ANativeWindow_getWidth(void *w);
int   ax_ANativeWindow_getHeight(void *w);
void  ax_ANativeWindow_release(void *w);
int   ax_ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt);
void *ax_ASensorManager_getInstance(void);
int   ax_ASensorManager_getSensorList(void *mgr, void **list);
void *ax_ASensorManager_createEventQueue(void *mgr, void *looper, int ident, void *cb, void *data);
int   ax_ASensorManager_destroyEventQueue(void *mgr, void *q);
int   ax_ASensorEventQueue_enableSensor(void *q, void *s);
int   ax_ASensorEventQueue_disableSensor(void *q, void *s);
long  ax_ASensorEventQueue_getEvents(void *q, void *ev, size_t n);
const char *ax_ASensor_getName(void *s);
int   ax_ASensor_getType(void *s);

extern const void *ax_SL_IID_ANDROIDCONFIGURATION;
extern const void *ax_SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
extern const void *ax_SL_IID_ENGINE;
extern const void *ax_SL_IID_PLAY;
extern const void *ax_SL_IID_RECORD;
extern const void *ax_SL_IID_VOLUME;
uint32_t ax_slCreateEngine(void **engine, uint32_t nopts, const void *opts,
                           uint32_t nifaces, const void *ids, const void *req);

#define GLR(name)
#define GLS(name) unsigned long ax_##name(void);
#include "gl_imports.h"
#undef GLR
#undef GLS

#endif
