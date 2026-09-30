/* log.h -- poorbunny.log next to the game files.
 *
 * Everything the port knows about a run ends up here: loader progress, every
 * SDL/JNI call that fell through to a default, Haxe trace() output (which
 * arrives through __android_log_print), and the crash report.
 *
 * Buffered, because Stencyl games trace a lot and the SD card is slow. Lines
 * flush on their own when they start with "!!" (errors), roughly every two
 * seconds from the frame hook, and unconditionally from the crash handler.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef PB_LOG_H
#define PB_LOG_H

#include <stdarg.h>
#include <stddef.h>

#define PB_LOG_OFF   0
#define PB_LOG_INFO  1
#define PB_LOG_DEBUG 2

void log_init(const char *path);
void log_set_level(int level);
int  log_get_level(void);

void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);
void log_flush(void);

/* Unbuffered, lock-free write for the crash handler. */
void log_emergency(const char *s);

/* Record the current start-up milestone. Written to the log immediately,
 * whatever the log level, and repeated by the crash handler. On a port that
 * has never run, knowing the last stage reached is worth more than any
 * register dump. */
void log_stage(const char *name);
/* Called by the crash handler: silences all ordinary logging. */
void log_begin_crash(void);
const char *log_last_stage(void);

#define LOGI(...) log_printf(__VA_ARGS__)
#define LOGE(...) log_printf("!! " __VA_ARGS__)
#define LOGD(...) do { if (log_get_level() >= PB_LOG_DEBUG) log_printf(__VA_ARGS__); } while (0)

/* Log a message the first time a call site is reached, then stay quiet. */
#define LOG_ONCE(...) do { static int once_; if (!once_) { once_ = 1; log_printf(__VA_ARGS__); } } while (0)

#endif
