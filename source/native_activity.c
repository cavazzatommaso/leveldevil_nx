/* native_activity.c -- be the Android framework for a NativeActivity.
 *
 * DefoldActivity extends android.app.NativeActivity (android.app.lib_name =
 * LevelDevil). On a phone the framework loads libLevelDevil.so, fills in an
 * ANativeActivity and calls ANativeActivity_onCreate, then drives the
 * lifecycle through the callbacks that function installs. That is all the
 * engine ever sees of Java's side of the startup; everything after is
 * android_main on the glue's own thread.
 *
 * The order below is Android's: onCreate, onStart, onResume, the window, the
 * input queue, focus. Each window/input callback blocks until the app thread
 * has picked the command up from its pipe (android_app_set_window waits for
 * it), so this also paces start-up correctly without any sleeping here.
 *
 * MIT licensed, see LICENSE.
 */
#include <stddef.h>
#include <stdint.h>

#include "android_stubs.h"
#include "jni_env.h"
#include "log.h"
#include "looper.h"
#include "native_activity.h"
#include "nativewindow.h"
#include "paths.h"

typedef struct { int32_t left, top, right, bottom; } ARect;

typedef struct ANativeActivity ANativeActivity;

/* android/native_activity.h, field for field: the glue writes these slots
 * (it stores 16 pointers at offsets 0x00..0x78). */
typedef struct {
    void  (*onStart)(ANativeActivity *);
    void  (*onResume)(ANativeActivity *);
    void *(*onSaveInstanceState)(ANativeActivity *, size_t *);
    void  (*onPause)(ANativeActivity *);
    void  (*onStop)(ANativeActivity *);
    void  (*onDestroy)(ANativeActivity *);
    void  (*onWindowFocusChanged)(ANativeActivity *, int);
    void  (*onNativeWindowCreated)(ANativeActivity *, void *);
    void  (*onNativeWindowResized)(ANativeActivity *, void *);
    void  (*onNativeWindowRedrawNeeded)(ANativeActivity *, void *);
    void  (*onNativeWindowDestroyed)(ANativeActivity *, void *);
    void  (*onInputQueueCreated)(ANativeActivity *, void *);
    void  (*onInputQueueDestroyed)(ANativeActivity *, void *);
    void  (*onContentRectChanged)(ANativeActivity *, const ARect *);
    void  (*onConfigurationChanged)(ANativeActivity *);
    void  (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;

struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    JavaVMPtr   vm;
    JNIEnvPtr   env;
    jobject     clazz;
    const char *internalDataPath;
    const char *externalDataPath;
    int32_t     sdkVersion;
    void       *instance;
    void       *assetManager;
    const char *obbPath;
};

_Static_assert(offsetof(struct ANativeActivity, instance) == 0x38, "ANativeActivity layout");
_Static_assert(sizeof(ANativeActivityCallbacks) == 16 * sizeof(void *), "callbacks layout");

typedef void (*OnCreateFn)(ANativeActivity *, void *saved, size_t saved_size);

static ANativeActivityCallbacks g_callbacks;
static ANativeActivity g_activity;

#define CALL(cb, ...) do { if (g_callbacks.cb) { LOGI("activity: " #cb); g_callbacks.cb(__VA_ARGS__); } } while (0)

int na_start(so_module *engine)
{
    OnCreateFn on_create = (OnCreateFn)so_symbol(engine, "ANativeActivity_onCreate");
    ARect rect;

    if (!on_create) {
        LOGE("activity: libLevelDevil.so has no ANativeActivity_onCreate");
        return 0;
    }

    g_activity.callbacks = &g_callbacks;
    g_activity.vm = jni_get_vm();
    g_activity.env = jni_get_env();
    g_activity.clazz = jni_activity();
    g_activity.internalDataPath = paths_save_nodev();
    g_activity.externalDataPath = paths_save_nodev();
    g_activity.sdkVersion = 29;
    g_activity.assetManager = ax_AAssetManager_fromJava(NULL, NULL);
    g_activity.obbPath = paths_save_nodev();

    LOGI("activity: ANativeActivity_onCreate");
    on_create(&g_activity, NULL, 0);
    LOGI("activity: onCreate returned, the app thread is running");

    CALL(onStart, &g_activity);
    CALL(onResume, &g_activity);
    CALL(onNativeWindowCreated, &g_activity, pbnw_handle());
    CALL(onInputQueueCreated, &g_activity, lp_input_queue());
    rect.left = rect.top = 0;
    rect.right = pbnw_width();
    rect.bottom = pbnw_height();
    CALL(onContentRectChanged, &g_activity, &rect);
    CALL(onWindowFocusChanged, &g_activity, 1);
    return 1;
}

void na_window_resized(void)
{
    ARect rect = { 0, 0, pbnw_width(), pbnw_height() };
    CALL(onNativeWindowResized, &g_activity, pbnw_handle());
    CALL(onContentRectChanged, &g_activity, &rect);
}

void na_focus(int has_focus)
{
    if (has_focus) {
        CALL(onResume, &g_activity);
        CALL(onWindowFocusChanged, &g_activity, 1);
    } else {
        CALL(onWindowFocusChanged, &g_activity, 0);
        CALL(onPause, &g_activity);
    }
}

void ax_ANativeActivity_finish(void *activity)
{
    (void)activity;
    LOGI("activity: the game called ANativeActivity_finish");
    na_on_finish();
}
