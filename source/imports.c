/* imports.c -- see imports.h.
 *
 * Grouped by where the implementation lives. "newlib" entries are functions
 * whose bionic and newlib ABIs are identical (same prototype, same struct
 * layouts, same constants), so the modules call newlib directly.
 *
 * MIT licensed, see LICENSE.
 */
#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <wchar.h>

#include "android_stubs.h"
#include "bionic.h"
#include "dl_bridge.h"
#include <EGL/egl.h>

#include "imports.h"
#include "looper.h"
#include "native_activity.h"
#include "opensl.h"
#include "nativewindow.h"
#include "log.h"

extern int *__errno(void);

#define GLR(name) extern void name(void);
#define GLS(name)
#include "gl_imports.h"
#undef GLR
#undef GLS

typedef struct {
    const char *name;
    uintptr_t   addr;
} ImportEntry;

static ImportEntry g_imports[] = {
    /* ---- newlib, ABI-identical to bionic ---- */
    { "__errno", (uintptr_t)&__errno },
    { "acos", (uintptr_t)&acos },
    { "acosf", (uintptr_t)&acosf },
    { "asin", (uintptr_t)&asin },
    { "asinf", (uintptr_t)&asinf },
    { "atan", (uintptr_t)&atan },
    { "atan2", (uintptr_t)&atan2 },
    { "atan2f", (uintptr_t)&atan2f },
    { "atanf", (uintptr_t)&atanf },
    { "atof", (uintptr_t)&atof },
    { "atoi", (uintptr_t)&atoi },
    { "bsearch", (uintptr_t)&bsearch },
    { "calloc", (uintptr_t)&calloc },
    { "cbrtf", (uintptr_t)&cbrtf },
    { "cos", (uintptr_t)&cos },
    { "cosf", (uintptr_t)&cosf },
    { "exp", (uintptr_t)&exp },
    { "expf", (uintptr_t)&expf },
    { "fmod", (uintptr_t)&fmod },
    { "fmodf", (uintptr_t)&fmodf },
    { "free", (uintptr_t)&free },
    { "frexp", (uintptr_t)&frexp },
    { "gettimeofday", (uintptr_t)&gettimeofday },
    { "hypot", (uintptr_t)&hypot },
    { "isalnum", (uintptr_t)&isalnum },
    { "isalpha", (uintptr_t)&isalpha },
    { "iscntrl", (uintptr_t)&iscntrl },
    { "isgraph", (uintptr_t)&isgraph },
    { "islower", (uintptr_t)&islower },
    { "isprint", (uintptr_t)&isprint },
    { "ispunct", (uintptr_t)&ispunct },
    { "isspace", (uintptr_t)&isspace },
    { "isupper", (uintptr_t)&isupper },
    { "isxdigit", (uintptr_t)&isxdigit },
    { "ldexp", (uintptr_t)&ldexp },
    { "log", (uintptr_t)&log },
    { "log10", (uintptr_t)&log10 },
    { "log10f", (uintptr_t)&log10f },
    { "logf", (uintptr_t)&logf },
    { "longjmp", (uintptr_t)&longjmp },
    { "lrand48", (uintptr_t)&lrand48 },
    { "lrint", (uintptr_t)&lrint },
    { "lrintf", (uintptr_t)&lrintf },
    { "malloc", (uintptr_t)&malloc },
    { "memchr", (uintptr_t)&memchr },
    { "memcmp", (uintptr_t)&memcmp },
    { "memcpy", (uintptr_t)&memcpy },
    { "memmove", (uintptr_t)&memmove },
    { "memset", (uintptr_t)&memset },
    { "modf", (uintptr_t)&modf },
    { "pow", (uintptr_t)&pow },
    { "powf", (uintptr_t)&powf },
    { "qsort", (uintptr_t)&qsort },
    { "rand", (uintptr_t)&rand },
    { "realloc", (uintptr_t)&realloc },
    { "scalbn", (uintptr_t)&scalbn },
    { "scalbnf", (uintptr_t)&scalbnf },
    { "setjmp", (uintptr_t)&setjmp },
    { "sin", (uintptr_t)&sin },
    { "sinf", (uintptr_t)&sinf },
    { "sinhf", (uintptr_t)&sinhf },
    { "snprintf", (uintptr_t)&snprintf },
    { "sprintf", (uintptr_t)&sprintf },
    { "sqrt", (uintptr_t)&sqrt },
    { "sqrtf", (uintptr_t)&sqrtf },
    { "srand", (uintptr_t)&srand },
    { "srand48", (uintptr_t)&srand48 },
    { "sscanf", (uintptr_t)&sscanf },
    { "stpcpy", (uintptr_t)&stpcpy },
    { "strcasecmp", (uintptr_t)&strcasecmp },
    { "strchr", (uintptr_t)&strchr },
    { "strcmp", (uintptr_t)&strcmp },
    { "strcpy", (uintptr_t)&strcpy },
    { "strdup", (uintptr_t)&strdup },
    { "strerror", (uintptr_t)&strerror },
    { "strlcat", (uintptr_t)&strlcat },
    { "strlcpy", (uintptr_t)&strlcpy },
    { "strlen", (uintptr_t)&strlen },
    { "strncasecmp", (uintptr_t)&strncasecmp },
    { "strncmp", (uintptr_t)&strncmp },
    { "strncpy", (uintptr_t)&strncpy },
    { "strpbrk", (uintptr_t)&strpbrk },
    { "strrchr", (uintptr_t)&strrchr },
    { "strstr", (uintptr_t)&strstr },
    { "strtod", (uintptr_t)&strtod },
    { "strtok_r", (uintptr_t)&strtok_r },
    { "strtol", (uintptr_t)&strtol },
    { "strtoll", (uintptr_t)&strtoll },
    { "strtoul", (uintptr_t)&strtoul },
    { "strtoull", (uintptr_t)&strtoull },
    { "tan", (uintptr_t)&tan },
    { "tanf", (uintptr_t)&tanf },
    { "time", (uintptr_t)&time },
    { "tolower", (uintptr_t)&tolower },
    { "toupper", (uintptr_t)&toupper },
    { "vsnprintf", (uintptr_t)&vsnprintf },
    { "vsscanf", (uintptr_t)&vsscanf },
    { "wcslen", (uintptr_t)&wcslen },
    { "wcsncpy", (uintptr_t)&wcsncpy },
    { "wcstombs", (uintptr_t)&wcstombs },
    { "wmemchr", (uintptr_t)&wmemchr },
    { "wmemcmp", (uintptr_t)&wmemcmp },
    { "wmemcpy", (uintptr_t)&wmemcpy },
    { "wmemmove", (uintptr_t)&wmemmove },
    { "wmemset", (uintptr_t)&wmemset },

    /* ---- stdio, file descriptors, directories (bionic_stdio.c) ---- */
    { "access", (uintptr_t)&bx_access },
    { "basename", (uintptr_t)&bx_basename },
    { "chdir", (uintptr_t)&bx_chdir },
    { "clearerr", (uintptr_t)&bx_clearerr },
    { "close", (uintptr_t)&bx_close },
    { "closedir", (uintptr_t)&bx_closedir },
    { "dup2", (uintptr_t)&bx_dup2 },
    { "fclose", (uintptr_t)&bx_fclose },
    { "fcntl", (uintptr_t)&bx_fcntl },
    { "fdopen", (uintptr_t)&bx_fdopen },
    { "feof", (uintptr_t)&bx_feof },
    { "ferror", (uintptr_t)&bx_ferror },
    { "fflush", (uintptr_t)&bx_fflush },
    { "fgetc", (uintptr_t)&bx_fgetc },
    { "fgets", (uintptr_t)&bx_fgets },
    { "fileno", (uintptr_t)&bx_fileno },
    { "fopen", (uintptr_t)&bx_fopen },
    { "fprintf", (uintptr_t)&bx_fprintf },
    { "fputc", (uintptr_t)&bx_fputc },
    { "fputs", (uintptr_t)&bx_fputs },
    { "fread", (uintptr_t)&bx_fread },
    { "fseek", (uintptr_t)&bx_fseek },
    { "fstat", (uintptr_t)&bx_fstat },
    { "ftell", (uintptr_t)&bx_ftell },
    { "fwrite", (uintptr_t)&bx_fwrite },
    { "getchar", (uintptr_t)&bx_getchar },
    { "getcwd", (uintptr_t)&bx_getcwd },
    { "ioctl", (uintptr_t)&bx_ioctl },
    { "lseek", (uintptr_t)&bx_lseek },
    { "mkdir", (uintptr_t)&bx_mkdir },
    { "mmap", (uintptr_t)&bx_mmap },
    { "munmap", (uintptr_t)&bx_munmap },
    { "open", (uintptr_t)&bx_open },
    { "opendir", (uintptr_t)&bx_opendir },
    { "pipe", (uintptr_t)&bx_pipe },
    { "printf", (uintptr_t)&bx_printf },
    { "putchar", (uintptr_t)&bx_putchar },
    { "puts", (uintptr_t)&bx_puts },
    { "read", (uintptr_t)&bx_read },
    { "readdir", (uintptr_t)&bx_readdir },
    { "readlink", (uintptr_t)&bx_readlink },
    { "realpath", (uintptr_t)&bx_realpath },
    { "remove", (uintptr_t)&bx_remove },
    { "rename", (uintptr_t)&bx_rename },
    { "rmdir", (uintptr_t)&bx_rmdir },
    { "setbuf", (uintptr_t)&bx_setbuf },
    { "stat", (uintptr_t)&bx_stat },
    { "unlink", (uintptr_t)&bx_unlink },
    { "vfprintf", (uintptr_t)&bx_vfprintf },
    { "write", (uintptr_t)&bx_write },

    /* ---- environment, time, lifecycle, signals (bionic_libc.c) ---- */
    { "abort", (uintptr_t)&bx_abort },
    { "alarm", (uintptr_t)&bx_alarm },
    { "clock_gettime", (uintptr_t)&bx_clock_gettime },
    { "execvp", (uintptr_t)&bx_execvp },
    { "exit", (uintptr_t)&bx_exit },
    { "fork", (uintptr_t)&bx_fork },
    { "getenv", (uintptr_t)&bx_getenv },
    { "geteuid", (uintptr_t)&bx_geteuid },
    { "getpid", (uintptr_t)&bx_getpid },
    { "getpwuid", (uintptr_t)&bx_getpwuid },
    { "gmtime", (uintptr_t)&bx_gmtime },
    { "gmtime_r", (uintptr_t)&bx_gmtime_r },
    { "localtime_r", (uintptr_t)&bx_localtime_r },
    { "mktime", (uintptr_t)&bx_mktime },
    { "nanosleep", (uintptr_t)&bx_nanosleep },
    { "pthread_sigmask", (uintptr_t)&bx_pthread_sigmask },
    { "raise", (uintptr_t)&bx_raise },
    { "setenv", (uintptr_t)&bx_setenv },
    { "setlocale", (uintptr_t)&bx_setlocale },
    { "sigaction", (uintptr_t)&bx_sigaction },
    { "sigaddset", (uintptr_t)&bx_sigaddset },
    { "sigemptyset", (uintptr_t)&bx_sigemptyset },
    { "signal", (uintptr_t)&bx_signal },
    { "strerror_r", (uintptr_t)&bx_strerror_r },
    { "strftime", (uintptr_t)&bx_strftime },
    { "sysconf", (uintptr_t)&bx_sysconf },
    { "system", (uintptr_t)&bx_system },
    { "times", (uintptr_t)&bx_times },
    { "waitpid", (uintptr_t)&bx_waitpid },
    { "__android_log_print", (uintptr_t)&bx_android_log_print },
    { "__android_log_write", (uintptr_t)&bx_android_log_write },
    { "__cxa_atexit", (uintptr_t)&bx_cxa_atexit },
    { "__cxa_finalize", (uintptr_t)&bx_cxa_finalize },
    { "__google_potentially_blocking_region_begin", (uintptr_t)&bx_google_region },
    { "__google_potentially_blocking_region_end", (uintptr_t)&bx_google_region },
    { "__isfinitef", (uintptr_t)&bx_isfinitef },
    { "__stack_chk_fail", (uintptr_t)&bx_stack_chk_fail },
    { "__system_property_get", (uintptr_t)&bx_system_property_get },
    { "_exit", (uintptr_t)&bx_exit },
    { "siglongjmp", (uintptr_t)&bx_siglongjmp },
    { "sigsetjmp", (uintptr_t)&bx_sigsetjmp },

    /* ---- data symbols: the relocation receives the variable's address ---- */
    { "__sF", (uintptr_t)bx_sF },
    { "__stack_chk_guard", (uintptr_t)&bx_stack_chk_guard },
    { "environ", (uintptr_t)&bx_environ },

    /* ---- threads, locks, futex (bionic_pthread.c) ---- */
    { "pthread_attr_destroy", (uintptr_t)&bx_pthread_attr_destroy },
    { "pthread_attr_init", (uintptr_t)&bx_pthread_attr_init },
    { "pthread_attr_setdetachstate", (uintptr_t)&bx_pthread_attr_setdetachstate },
    { "pthread_attr_setstacksize", (uintptr_t)&bx_pthread_attr_setstacksize },
    { "pthread_cond_broadcast", (uintptr_t)&bx_pthread_cond_broadcast },
    { "pthread_cond_destroy", (uintptr_t)&bx_pthread_cond_destroy },
    { "pthread_cond_init", (uintptr_t)&bx_pthread_cond_init },
    { "pthread_cond_signal", (uintptr_t)&bx_pthread_cond_signal },
    { "pthread_cond_timedwait", (uintptr_t)&bx_pthread_cond_timedwait },
    { "pthread_cond_wait", (uintptr_t)&bx_pthread_cond_wait },
    { "pthread_create", (uintptr_t)&bx_pthread_create },
    { "pthread_detach", (uintptr_t)&bx_pthread_detach },
    { "pthread_equal", (uintptr_t)&bx_pthread_equal },
    { "pthread_exit", (uintptr_t)&bx_pthread_exit },
    { "pthread_getschedparam", (uintptr_t)&bx_pthread_getschedparam },
    { "pthread_getspecific", (uintptr_t)&bx_pthread_getspecific },
    { "pthread_join", (uintptr_t)&bx_pthread_join },
    { "pthread_key_create", (uintptr_t)&bx_pthread_key_create },
    { "pthread_key_delete", (uintptr_t)&bx_pthread_key_delete },
    { "pthread_mutex_destroy", (uintptr_t)&bx_pthread_mutex_destroy },
    { "pthread_mutex_init", (uintptr_t)&bx_pthread_mutex_init },
    { "pthread_mutex_lock", (uintptr_t)&bx_pthread_mutex_lock },
    { "pthread_mutex_trylock", (uintptr_t)&bx_pthread_mutex_trylock },
    { "pthread_mutex_unlock", (uintptr_t)&bx_pthread_mutex_unlock },
    { "pthread_mutexattr_destroy", (uintptr_t)&bx_pthread_mutexattr_destroy },
    { "pthread_mutexattr_init", (uintptr_t)&bx_pthread_mutexattr_init },
    { "pthread_mutexattr_settype", (uintptr_t)&bx_pthread_mutexattr_settype },
    { "pthread_once", (uintptr_t)&bx_pthread_once },
    { "pthread_self", (uintptr_t)&bx_pthread_self },
    { "pthread_setschedparam", (uintptr_t)&bx_pthread_setschedparam },
    { "pthread_setspecific", (uintptr_t)&bx_pthread_setspecific },
    { "sched_get_priority_max", (uintptr_t)&bx_sched_get_priority_max },
    { "sched_get_priority_min", (uintptr_t)&bx_sched_get_priority_min },
    { "sched_yield", (uintptr_t)&bx_sched_yield },
    { "sem_destroy", (uintptr_t)&bx_sem_destroy },
    { "sem_getvalue", (uintptr_t)&bx_sem_getvalue },
    { "sem_init", (uintptr_t)&bx_sem_init },
    { "sem_post", (uintptr_t)&bx_sem_post },
    { "sem_trywait", (uintptr_t)&bx_sem_trywait },
    { "sem_wait", (uintptr_t)&bx_sem_wait },
    { "syscall", (uintptr_t)&bx_syscall },

    /* ---- networking, offline (bionic_net.c) ---- */
    { "freeaddrinfo", (uintptr_t)&bx_freeaddrinfo },
    { "gai_strerror", (uintptr_t)&bx_gai_strerror },
    { "getaddrinfo", (uintptr_t)&bx_getaddrinfo },
    { "gethostbyaddr", (uintptr_t)&bx_gethostbyaddr },
    { "gethostbyname_r", (uintptr_t)&bx_gethostbyname_r },
    { "gethostname", (uintptr_t)&bx_gethostname },
    { "inet_addr", (uintptr_t)&bx_inet_addr },
    { "inet_ntoa", (uintptr_t)&bx_inet_ntoa },
    { "inet_ntop", (uintptr_t)&bx_inet_ntop },
    { "inet_pton", (uintptr_t)&bx_inet_pton },
    { "poll", (uintptr_t)&bx_poll },
    { "select", (uintptr_t)&bx_select },
    { "socket", (uintptr_t)&bx_socket },
    { "accept", (uintptr_t)&bx_net_fail },
    { "bind", (uintptr_t)&bx_net_fail },
    { "connect", (uintptr_t)&bx_net_fail },
    { "getpeername", (uintptr_t)&bx_net_fail },
    { "getsockname", (uintptr_t)&bx_net_fail },
    { "getsockopt", (uintptr_t)&bx_net_fail },
    { "listen", (uintptr_t)&bx_net_fail },
    { "recv", (uintptr_t)&bx_net_fail },
    { "recvfrom", (uintptr_t)&bx_net_fail },
    { "send", (uintptr_t)&bx_net_fail },
    { "sendto", (uintptr_t)&bx_net_fail },
    { "setsockopt", (uintptr_t)&bx_net_fail },
    { "shutdown", (uintptr_t)&bx_net_fail },

    /* ---- dynamic linker (dl_bridge.c) ---- */
    { "dl_iterate_phdr", (uintptr_t)&bx_dl_iterate_phdr },
    { "dlclose", (uintptr_t)&bx_dlclose },
    { "dlerror", (uintptr_t)&bx_dlerror },
    { "dlopen", (uintptr_t)&bx_dlopen },
    { "dlsym", (uintptr_t)&bx_dlsym },

    /* ---- Android NDK (nativewindow.c, android_stubs.c) ---- */
    { "ANativeWindow_fromSurface", (uintptr_t)&ax_ANativeWindow_fromSurface },
    { "ANativeWindow_getHeight", (uintptr_t)&ax_ANativeWindow_getHeight },
    { "ANativeWindow_getWidth", (uintptr_t)&ax_ANativeWindow_getWidth },
    { "ANativeWindow_release", (uintptr_t)&ax_ANativeWindow_release },
    { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ax_ANativeWindow_setBuffersGeometry },
    { "ASensorEventQueue_disableSensor", (uintptr_t)&ax_ASensorEventQueue_disableSensor },
    { "ASensorEventQueue_enableSensor", (uintptr_t)&ax_ASensorEventQueue_enableSensor },
    { "ASensorEventQueue_getEvents", (uintptr_t)&ax_ASensorEventQueue_getEvents },
    { "ASensorManager_createEventQueue", (uintptr_t)&ax_ASensorManager_createEventQueue },
    { "ASensorManager_destroyEventQueue", (uintptr_t)&ax_ASensorManager_destroyEventQueue },
    { "ASensorManager_getInstance", (uintptr_t)&ax_ASensorManager_getInstance },
    { "ASensorManager_getSensorList", (uintptr_t)&ax_ASensorManager_getSensorList },

    /* ---- from the Poor Bunny port (bionic_extra.c, bionic_pthread.c,
     * assetmgr.c, nativewindow.c) ---- */
    { "AAssetManager_fromJava", (uintptr_t)&ax_AAssetManager_fromJava },
    { "AAssetManager_open", (uintptr_t)&ax_AAssetManager_open },
    { "AAsset_close", (uintptr_t)&ax_AAsset_close },
    { "AAsset_getLength64", (uintptr_t)&ax_AAsset_getLength64 },
    { "AAsset_openFileDescriptor", (uintptr_t)&ax_AAsset_openFileDescriptor },
    { "AAsset_read", (uintptr_t)&ax_AAsset_read },
    { "AAsset_seek64", (uintptr_t)&ax_AAsset_seek64 },
    { "AConfiguration_delete", (uintptr_t)&ax_AConfiguration_delete },
    { "AConfiguration_fromAssetManager", (uintptr_t)&ax_AConfiguration_fromAssetManager },
    { "AConfiguration_getCountry", (uintptr_t)&ax_AConfiguration_getCountry },
    { "AConfiguration_getLanguage", (uintptr_t)&ax_AConfiguration_getLanguage },
    { "AConfiguration_new", (uintptr_t)&ax_AConfiguration_new },
    { "ANativeWindow_getFormat", (uintptr_t)&ax_ANativeWindow_getFormat },
    { "ASensorEventQueue_setEventRate", (uintptr_t)&ax_ASensorEventQueue_setEventRate },
    { "__FD_ISSET_chk", (uintptr_t)&bx_FD_ISSET_chk },
    { "__FD_SET_chk", (uintptr_t)&bx_FD_SET_chk },
    { "__assert2", (uintptr_t)&bx_assert2 },
    { "__ctype_get_mb_cur_max", (uintptr_t)&bx_ctype_get_mb_cur_max },
    { "__cxa_thread_atexit_impl", (uintptr_t)&bx_cxa_thread_atexit_impl },
    { "__register_atfork", (uintptr_t)&bx_register_atfork },
    { "android_set_abort_message", (uintptr_t)&bx_android_set_abort_message },
    { "btowc", (uintptr_t)&btowc },
    { "closelog", (uintptr_t)&bx_closelog },
    { "exp2", (uintptr_t)&exp2 },
    { "exp2f", (uintptr_t)&exp2f },
    { "freelocale", (uintptr_t)&bx_freelocale },
    { "fseeko", (uintptr_t)&bx_fseeko },
    { "ftello", (uintptr_t)&bx_ftello },
    { "getauxval", (uintptr_t)&bx_getauxval },
    { "hypotf", (uintptr_t)&hypotf },
    { "iswalpha_l", (uintptr_t)&bx_iswalpha_l },
    { "iswblank_l", (uintptr_t)&bx_iswblank_l },
    { "iswcntrl_l", (uintptr_t)&bx_iswcntrl_l },
    { "iswdigit_l", (uintptr_t)&bx_iswdigit_l },
    { "iswlower_l", (uintptr_t)&bx_iswlower_l },
    { "iswprint_l", (uintptr_t)&bx_iswprint_l },
    { "iswpunct_l", (uintptr_t)&bx_iswpunct_l },
    { "iswspace_l", (uintptr_t)&bx_iswspace_l },
    { "iswupper_l", (uintptr_t)&bx_iswupper_l },
    { "iswxdigit_l", (uintptr_t)&bx_iswxdigit_l },
    { "kill", (uintptr_t)&bx_kill },
    { "ldexpf", (uintptr_t)&ldexpf },
    { "localeconv", (uintptr_t)&bx_localeconv },
    { "log2f", (uintptr_t)&log2f },
    { "mbrlen", (uintptr_t)&mbrlen },
    { "mbrtowc", (uintptr_t)&mbrtowc },
    { "mbsnrtowcs", (uintptr_t)&bx_mbsnrtowcs },
    { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
    { "mbstowcs", (uintptr_t)&mbstowcs },
    { "mbtowc", (uintptr_t)&mbtowc },
    { "memfd_create", (uintptr_t)&bx_memfd_create },
    { "newlocale", (uintptr_t)&bx_newlocale },
    { "openlog", (uintptr_t)&bx_openlog },
    { "posix_memalign", (uintptr_t)&bx_posix_memalign },
    { "pthread_rwlock_rdlock", (uintptr_t)&bx_pthread_rwlock_rdlock },
    { "pthread_rwlock_unlock", (uintptr_t)&bx_pthread_rwlock_unlock },
    { "pthread_rwlock_wrlock", (uintptr_t)&bx_pthread_rwlock_wrlock },
    { "rewind", (uintptr_t)&bx_rewind },
    { "sem_timedwait", (uintptr_t)&bx_sem_timedwait },
    { "setvbuf", (uintptr_t)&bx_setvbuf },
    { "sincos", (uintptr_t)&bx_sincos },
    { "sincosf", (uintptr_t)&bx_sincosf },
    { "sinh", (uintptr_t)&sinh },
    { "socketpair", (uintptr_t)&bx_socketpair },
    { "stderr", (uintptr_t)&bx_stderr_var },
    { "stdin", (uintptr_t)&bx_stdin_var },
    { "stdout", (uintptr_t)&bx_stdout_var },
    { "strcasestr", (uintptr_t)&bx_strcasestr },
    { "strcat", (uintptr_t)&strcat },
    { "strcoll_l", (uintptr_t)&bx_strcoll_l },
    { "strcspn", (uintptr_t)&strcspn },
    { "strftime_l", (uintptr_t)&bx_strftime_l },
    { "strspn", (uintptr_t)&strspn },
    { "strtof", (uintptr_t)&strtof },
    { "strtold", (uintptr_t)&strtold },
    { "strtold_l", (uintptr_t)&bx_strtold_l },
    { "strtoll_l", (uintptr_t)&bx_strtoll_l },
    { "strtoull_l", (uintptr_t)&bx_strtoull_l },
    { "strxfrm_l", (uintptr_t)&bx_strxfrm_l },
    { "swprintf", (uintptr_t)&swprintf },
    { "syslog", (uintptr_t)&bx_syslog },
    { "tmpfile", (uintptr_t)&bx_tmpfile },
    { "towlower_l", (uintptr_t)&bx_towlower_l },
    { "towupper_l", (uintptr_t)&bx_towupper_l },
    { "unsetenv", (uintptr_t)&unsetenv },
    { "uselocale", (uintptr_t)&bx_uselocale },
    { "vasprintf", (uintptr_t)&bx_vasprintf },
    { "wcrtomb", (uintptr_t)&wcrtomb },
    { "wcscoll_l", (uintptr_t)&bx_wcscoll_l },
    { "wcsnrtombs", (uintptr_t)&bx_wcsnrtombs },
    { "wcstod", (uintptr_t)&wcstod },
    { "wcstof", (uintptr_t)&wcstof },
    { "wcstol", (uintptr_t)&wcstol },
    { "wcstold", (uintptr_t)&wcstold },
    { "wcstoll", (uintptr_t)&wcstoll },
    { "wcstoul", (uintptr_t)&wcstoul },
    { "wcstoull", (uintptr_t)&wcstoull },
    { "wcsxfrm_l", (uintptr_t)&bx_wcsxfrm_l },
    { "wctob", (uintptr_t)&wctob },

    /* ---- the Defold build (bionic_defold.c, looper.c, opensl.c,
     * native_activity.c, assetmgr.c) ---- */
    { "__memcpy_chk", (uintptr_t)&bx_memcpy_chk },
    { "__memmove_chk", (uintptr_t)&bx_memmove_chk },
    { "__memset_chk", (uintptr_t)&bx_memset_chk },
    { "__strlen_chk", (uintptr_t)&bx_strlen_chk },
    { "__strchr_chk", (uintptr_t)&bx_strchr_chk },
    { "__strrchr_chk", (uintptr_t)&bx_strrchr_chk },
    { "__read_chk", (uintptr_t)&bx_read_chk },
    { "__open_2", (uintptr_t)&bx_open_2 },
    { "__vsnprintf_chk", (uintptr_t)&bx_vsnprintf_chk },
    { "__vsprintf_chk", (uintptr_t)&bx_vsprintf_chk },
    { "__FD_CLR_chk", (uintptr_t)&bx_FD_CLR_chk },
    { "_ctype_", (uintptr_t)&bx_ctype_ptr },
    { "getc", (uintptr_t)&bx_getc },
    { "ungetc", (uintptr_t)&bx_ungetc },
    { "fscanf", (uintptr_t)&bx_fscanf },
    { "vprintf", (uintptr_t)&bx_vprintf },
    { "popen", (uintptr_t)&bx_popen },
    { "pclose", (uintptr_t)&bx_pclose },
    { "mkstemp", (uintptr_t)&bx_mkstemp },
    { "fstat64", (uintptr_t)&bx_fstat },
    { "lseek64", (uintptr_t)&bx_lseek },
    { "localtime", (uintptr_t)&bx_localtime },
    { "clock", (uintptr_t)&bx_clock },
    { "usleep", (uintptr_t)&bx_usleep },
    { "setitimer", (uintptr_t)&bx_setitimer },
    { "mremap", (uintptr_t)&bx_mremap },
    { "mprotect", (uintptr_t)&bx_mprotect },
    { "difftime", (uintptr_t)&difftime },
    { "cosh", (uintptr_t)&cosh },
    { "tanh", (uintptr_t)&tanh },
    { "strncat", (uintptr_t)&strncat },
    { "memalign", (uintptr_t)&memalign },
    { "pthread_attr_getstacksize", (uintptr_t)&bx_pthread_attr_getstacksize },
    { "pthread_getattr_np", (uintptr_t)&bx_pthread_getattr_np },
    { "pthread_setname_np", (uintptr_t)&bx_pthread_setname_np },
    { "__get_h_errno", (uintptr_t)&bx_get_h_errno },
    { "gethostbyname", (uintptr_t)&bx_gethostbyname },
    { "hstrerror", (uintptr_t)&bx_hstrerror },
    { "inet_aton", (uintptr_t)&bx_inet_aton },
    { "getnameinfo", (uintptr_t)&bx_getnameinfo },
    { "dladdr", (uintptr_t)&bx_dladdr },
    { "glBindVertexArrayOES", (uintptr_t)&bx_glBindVertexArrayOES },
    { "glGenVertexArraysOES", (uintptr_t)&bx_glGenVertexArraysOES },

    { "ALooper_prepare", (uintptr_t)&ax_ALooper_prepare },
    { "ALooper_forThread", (uintptr_t)&ax_ALooper_forThread },
    { "ALooper_addFd", (uintptr_t)&ax_ALooper_addFd },
    { "ALooper_removeFd", (uintptr_t)&ax_ALooper_removeFd },
    { "ALooper_pollOnce", (uintptr_t)&ax_ALooper_pollOnce },
    { "ALooper_pollAll", (uintptr_t)&ax_ALooper_pollAll },
    { "AInputQueue_attachLooper", (uintptr_t)&ax_AInputQueue_attachLooper },
    { "AInputQueue_detachLooper", (uintptr_t)&ax_AInputQueue_detachLooper },
    { "AInputQueue_getEvent", (uintptr_t)&ax_AInputQueue_getEvent },
    { "AInputQueue_preDispatchEvent", (uintptr_t)&ax_AInputQueue_preDispatchEvent },
    { "AInputQueue_finishEvent", (uintptr_t)&ax_AInputQueue_finishEvent },
    { "AInputEvent_getType", (uintptr_t)&ax_AInputEvent_getType },
    { "AInputEvent_getSource", (uintptr_t)&ax_AInputEvent_getSource },
    { "AInputEvent_getDeviceId", (uintptr_t)&ax_AInputEvent_getDeviceId },
    { "AKeyEvent_getAction", (uintptr_t)&ax_AKeyEvent_getAction },
    { "AKeyEvent_getKeyCode", (uintptr_t)&ax_AKeyEvent_getKeyCode },
    { "AKeyEvent_getScanCode", (uintptr_t)&ax_AKeyEvent_getScanCode },
    { "AKeyEvent_getMetaState", (uintptr_t)&ax_AKeyEvent_getMetaState },
    { "AKeyEvent_getFlags", (uintptr_t)&ax_AKeyEvent_getFlags },
    { "AKeyEvent_getRepeatCount", (uintptr_t)&ax_AKeyEvent_getRepeatCount },
    { "AKeyEvent_getDownTime", (uintptr_t)&ax_AKeyEvent_getDownTime },
    { "AKeyEvent_getEventTime", (uintptr_t)&ax_AKeyEvent_getEventTime },
    { "AMotionEvent_getAction", (uintptr_t)&ax_AMotionEvent_getAction },
    { "AMotionEvent_getPointerCount", (uintptr_t)&ax_AMotionEvent_getPointerCount },
    { "AMotionEvent_getPointerId", (uintptr_t)&ax_AMotionEvent_getPointerId },
    { "AMotionEvent_getX", (uintptr_t)&ax_AMotionEvent_getX },
    { "AMotionEvent_getY", (uintptr_t)&ax_AMotionEvent_getY },
    { "AMotionEvent_getAxisValue", (uintptr_t)&ax_AMotionEvent_getAxisValue },

    { "slCreateEngine", (uintptr_t)&ax_slCreateEngine },
    { "SL_IID_ENGINE", (uintptr_t)&ax_SL_IID_ENGINE },
    { "SL_IID_BUFFERQUEUE", (uintptr_t)&ax_SL_IID_BUFFERQUEUE },
    { "SL_IID_PLAY", (uintptr_t)&ax_SL_IID_PLAY },
    { "SL_IID_VOLUME", (uintptr_t)&ax_SL_IID_VOLUME },
    { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&ax_SL_IID_ANDROIDSIMPLEBUFFERQUEUE },

    { "ANativeActivity_finish", (uintptr_t)&ax_ANativeActivity_finish },
    { "ANativeWindow_acquire", (uintptr_t)&ax_ANativeWindow_acquire },
    { "ASensorManager_getDefaultSensor", (uintptr_t)&ax_ASensorManager_getDefaultSensor },
    { "AAsset_getBuffer", (uintptr_t)&ax_AAsset_getBuffer },
    { "AAsset_getLength", (uintptr_t)&ax_AAsset_getLength },
    { "AAsset_seek", (uintptr_t)&ax_AAsset_seek },
    { "AConfiguration_getDensity", (uintptr_t)&ax_AConfiguration_getDensity },
    { "AConfiguration_getKeyboard", (uintptr_t)&ax_AConfiguration_getKeyboard },
    { "AConfiguration_getKeysHidden", (uintptr_t)&ax_AConfiguration_getKeysHidden },
    { "AConfiguration_getMcc", (uintptr_t)&ax_AConfiguration_getMcc },
    { "AConfiguration_getMnc", (uintptr_t)&ax_AConfiguration_getMnc },
    { "AConfiguration_getNavHidden", (uintptr_t)&ax_AConfiguration_getNavHidden },
    { "AConfiguration_getNavigation", (uintptr_t)&ax_AConfiguration_getNavigation },
    { "AConfiguration_getOrientation", (uintptr_t)&ax_AConfiguration_getOrientation },
    { "AConfiguration_getScreenLong", (uintptr_t)&ax_AConfiguration_getScreenLong },
    { "AConfiguration_getScreenSize", (uintptr_t)&ax_AConfiguration_getScreenSize },
    { "AConfiguration_getSdkVersion", (uintptr_t)&ax_AConfiguration_getSdkVersion },
    { "AConfiguration_getTouchscreen", (uintptr_t)&ax_AConfiguration_getTouchscreen },
    { "AConfiguration_getUiModeNight", (uintptr_t)&ax_AConfiguration_getUiModeNight },
    { "AConfiguration_getUiModeType", (uintptr_t)&ax_AConfiguration_getUiModeType },

    /* ---- EGL. Not imported by the ELF: SDL's Android video driver
     * dlopen("libEGL.so")s and dlsym()s these one at a time, and
     * dl_bridge.c answers that from this table. The addresses are
     * switch-mesa's, so the EGLNativeWindowType SDL passes on to
     * eglCreateWindowSurface -- our ANativeWindow, which is the
     * NWindow -- arrives as the type mesa expects. ---- */
    { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
    { "eglInitialize", (uintptr_t)&eglInitialize },
    { "eglTerminate", (uintptr_t)&eglTerminate },
    { "eglGetPlatformDisplay", (uintptr_t)&pbnw_egl_get_platform_display },
    { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
    { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
    { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
    { "eglCreateContext", (uintptr_t)&eglCreateContext },
    { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
    { "eglCreateWindowSurface", (uintptr_t)&pbnw_egl_create_window_surface },
    { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface },
    { "eglDestroySurface", (uintptr_t)&pbnw_egl_destroy_surface },
    { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
    { "eglSwapBuffers", (uintptr_t)&pbnw_egl_swap_buffers },
    { "eglSwapInterval", (uintptr_t)&eglSwapInterval },
    { "eglWaitNative", (uintptr_t)&eglWaitNative },
    { "eglWaitGL", (uintptr_t)&eglWaitGL },
    { "eglBindAPI", (uintptr_t)&eglBindAPI },
    { "eglQueryAPI", (uintptr_t)&eglQueryAPI },
    { "eglQueryString", (uintptr_t)&eglQueryString },
    { "eglGetError", (uintptr_t)&eglGetError },
    { "eglQuerySurface", (uintptr_t)&pbnw_egl_query_surface },
    { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
    { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
    { "eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay },
    { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
    { "eglReleaseThread", (uintptr_t)&eglReleaseThread },

    /* ---- OpenGL ES (gl_imports.h) ---- */
#define GLR(name) { #name, (uintptr_t)&name },
#define GLS(name) { #name, (uintptr_t)&ax_##name },
#include "gl_imports.h"
#undef GLR
#undef GLS
};

#define IMPORT_COUNT (sizeof(g_imports) / sizeof(g_imports[0]))

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const ImportEntry *)a)->name, ((const ImportEntry *)b)->name);
}

void imports_init(void)
{
    size_t i;
    qsort(g_imports, IMPORT_COUNT, sizeof(g_imports[0]), cmp_entry);
    for (i = 1; i < IMPORT_COUNT; i++)
        if (!strcmp(g_imports[i - 1].name, g_imports[i].name))
            LOGE("imports: duplicate entry %s", g_imports[i].name);
    LOGI("imports: %zu entries", IMPORT_COUNT);
}

uintptr_t imports_lookup(const char *name)
{
    size_t lo = 0, hi = IMPORT_COUNT;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, g_imports[mid].name);
        if (c == 0)
            return g_imports[mid].addr;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

uintptr_t imports_resolve(const char *name, so_module *importer)
{
    uintptr_t addr = imports_lookup(name);
    so_module *m;
    if (addr)
        return addr;
    for (m = so_module_list(); m; m = m->next) {
        if (m == importer)
            continue;
        addr = so_symbol(m, name);
        if (addr)
            return addr;
    }
    return 0;
}
