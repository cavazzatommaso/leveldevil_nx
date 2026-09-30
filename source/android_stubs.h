/* android_stubs.h -- NDK entry points: declarations for imports.c.
 * MIT licensed, see LICENSE. */
#ifndef PB_ANDROID_STUBS_H
#define PB_ANDROID_STUBS_H

#include <stddef.h>
#include <stdint.h>

/* nativewindow.c: the ANativeWindow is the Switch's NWindow. */
void *ax_ANativeWindow_fromSurface(void *env, void *surface);
int   ax_ANativeWindow_getWidth(void *w);
int   ax_ANativeWindow_getHeight(void *w);
int   ax_ANativeWindow_getFormat(void *w);
void  ax_ANativeWindow_release(void *w);
void  ax_ANativeWindow_acquire(void *w);
int   ax_ANativeWindow_setBuffersGeometry(void *w, int width, int height, int fmt);

/* assetmgr.c: the asset manager over the SD card's assets folder. */
void       *ax_AAssetManager_fromJava(void *env, void *assetManager);
void       *ax_AAssetManager_open(void *mgr, const char *filename, int mode);
int         ax_AAsset_read(void *asset, void *buf, size_t count);
int64_t     ax_AAsset_seek64(void *asset, int64_t offset, int whence);
long        ax_AAsset_seek(void *asset, long offset, int whence);
int64_t     ax_AAsset_getLength64(void *asset);
long        ax_AAsset_getLength(void *asset);
const void *ax_AAsset_getBuffer(void *asset);
void        ax_AAsset_close(void *asset);
int         ax_AAsset_openFileDescriptor(void *asset, int64_t *outStart, int64_t *outLength);
void   *ax_AConfiguration_new(void);
void    ax_AConfiguration_delete(void *c);
void    ax_AConfiguration_fromAssetManager(void *c, void *m);
void    ax_AConfiguration_getLanguage(void *c, char *out);
void    ax_AConfiguration_getCountry(void *c, char *out);
int32_t ax_AConfiguration_getDensity(void *c);
int32_t ax_AConfiguration_getKeyboard(void *c);
int32_t ax_AConfiguration_getKeysHidden(void *c);
int32_t ax_AConfiguration_getMcc(void *c);
int32_t ax_AConfiguration_getMnc(void *c);
int32_t ax_AConfiguration_getNavHidden(void *c);
int32_t ax_AConfiguration_getNavigation(void *c);
int32_t ax_AConfiguration_getOrientation(void *c);
int32_t ax_AConfiguration_getScreenLong(void *c);
int32_t ax_AConfiguration_getScreenSize(void *c);
int32_t ax_AConfiguration_getSdkVersion(void *c);
int32_t ax_AConfiguration_getTouchscreen(void *c);
int32_t ax_AConfiguration_getUiModeNight(void *c);
int32_t ax_AConfiguration_getUiModeType(void *c);

/* android_stubs.c: no sensors on a Switch. */
void *ax_ASensorManager_getInstance(void);
void *ax_ASensorManager_getDefaultSensor(void *mgr, int type);
int   ax_ASensorManager_getSensorList(void *mgr, void **list);
void *ax_ASensorManager_createEventQueue(void *mgr, void *looper, int ident, void *cb, void *data);
int   ax_ASensorManager_destroyEventQueue(void *mgr, void *q);
int   ax_ASensorEventQueue_enableSensor(void *q, void *s);
int   ax_ASensorEventQueue_disableSensor(void *q, void *s);
int   ax_ASensorEventQueue_setEventRate(void *q, void *s, int32_t usec);
long  ax_ASensorEventQueue_getEvents(void *q, void *ev, size_t n);

#define GLR(name)
#define GLS(name) unsigned long ax_##name(void);
#include "gl_imports.h"
#undef GLR
#undef GLS

#endif
