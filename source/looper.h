/* looper.h -- see looper.c. MIT licensed, see LICENSE. */
#ifndef LD_LOOPER_H
#define LD_LOOPER_H

#include <stddef.h>
#include <stdint.h>

/* ---- in-process pipes (fds from LP_FD_BASE up) ---- */
#define LP_FD_BASE 0x4000
int  lp_is_fd(int fd);
int  lp_pipe(int fds[2]);
long lp_read(int fd, void *buf, size_t n);
long lp_write(int fd, const void *buf, size_t n);
int  lp_close(int fd);

/* ---- the one input queue, fed by input.c ---- */
void *lp_input_queue(void);

/* android/input.h constants the feeders use */
#define LP_SOURCE_KEYBOARD    0x00000101
#define LP_SOURCE_TOUCHSCREEN 0x00001002
#define LP_KEY_DOWN 0
#define LP_KEY_UP   1
#define LP_MOTION_DOWN 0
#define LP_MOTION_UP   1
#define LP_MOTION_MOVE 2
#define LP_MOTION_POINTER_DOWN 5
#define LP_MOTION_POINTER_UP   6

void lp_push_key(int keycode, int down);
/* Every finger currently down, in window pixels. `action` already carries the
 * pointer index in bits 8..15 for POINTER_DOWN/UP. */
void lp_push_motion(int action, int count, const int *ids, const float *xs, const float *ys);

/* ---- imported NDK entry points ---- */
void *ax_ALooper_prepare(int opts);
void *ax_ALooper_forThread(void);
int   ax_ALooper_addFd(void *looper, int fd, int ident, int events, void *callback, void *data);
int   ax_ALooper_removeFd(void *looper, int fd);
int   ax_ALooper_pollOnce(int timeout, int *fd, int *events, void **data);
int   ax_ALooper_pollAll(int timeout, int *fd, int *events, void **data);

void    ax_AInputQueue_attachLooper(void *q, void *looper, int ident, void *callback, void *data);
void    ax_AInputQueue_detachLooper(void *q);
int32_t ax_AInputQueue_getEvent(void *q, void **out);
int32_t ax_AInputQueue_preDispatchEvent(void *q, void *ev);
void    ax_AInputQueue_finishEvent(void *q, void *ev, int handled);

int32_t ax_AInputEvent_getType(const void *ev);
int32_t ax_AInputEvent_getSource(const void *ev);
int32_t ax_AInputEvent_getDeviceId(const void *ev);
int32_t ax_AKeyEvent_getAction(const void *ev);
int32_t ax_AKeyEvent_getKeyCode(const void *ev);
int32_t ax_AKeyEvent_getScanCode(const void *ev);
int32_t ax_AKeyEvent_getMetaState(const void *ev);
int32_t ax_AKeyEvent_getFlags(const void *ev);
int32_t ax_AKeyEvent_getRepeatCount(const void *ev);
int64_t ax_AKeyEvent_getDownTime(const void *ev);
int64_t ax_AKeyEvent_getEventTime(const void *ev);
int32_t ax_AMotionEvent_getAction(const void *ev);
size_t  ax_AMotionEvent_getPointerCount(const void *ev);
int32_t ax_AMotionEvent_getPointerId(const void *ev, size_t i);
float   ax_AMotionEvent_getX(const void *ev, size_t i);
float   ax_AMotionEvent_getY(const void *ev, size_t i);
float   ax_AMotionEvent_getAxisValue(const void *ev, int32_t axis, size_t i);

#endif
