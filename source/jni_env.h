/* jni_env.h -- a small Java runtime for the JNI calls the game makes.
 *
 * There is no JVM. liblime.so's JNI bridge (used by the game's Haxe code for
 * Lime's JNI.callStatic/callMember) and the SDL Android glue receive a JNIEnv
 * whose function table is implemented in jni_env.c on top of a tiny object
 * model: strings, arrays, boxed primitives, plain objects with fields, and
 * classes. Behaviour lives in jni_classes.c as tables of methods and fields.
 *
 * Unknown classes, methods and fields never fail. FindClass/GetMethodID
 * create a placeholder, the call returns a type-appropriate default ("" for
 * String, an empty array for arrays), and the first use is logged. That
 * keeps an unanticipated optional SDK call from becoming a Haxe exception.
 *
 * REFERENCES
 * Objects are reference counted. New objects start with one reference (the
 * caller's local ref); NewGlobalRef/NewLocalRef add one; Delete*Ref drop one.
 * Classes and singletons are permanent. Released objects are quarantined
 * before being freed, so a double DeleteLocalRef is logged instead of
 * crashing.
 *
 * THREADS
 * Java-to-Haxe callbacks (Lime.onCallback, HaxeObject.call) run on a
 * dedicated "UI thread", exactly as on Android. Lime's callObjectFunction
 * force-registers the calling thread's stack top with hxcpp's GC, so calling
 * it from the game thread in mid-frame would hide the game thread's outer
 * stack frames from the collector.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_JNI_ENV_H
#define HS_JNI_ENV_H

#include <stdint.h>
#include "so_util.h"

typedef uint8_t  jboolean;
typedef int8_t   jbyte;
typedef uint16_t jchar;
typedef int16_t  jshort;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

typedef struct JObj    *jobject;
typedef jobject         jclass;
typedef jobject         jstring;
typedef jobject         jarray;
typedef jobject         jobjectArray;
typedef jobject         jthrowable;
typedef struct JField  *jfieldID;
typedef struct JMethod *jmethodID;

typedef union {
    jboolean z; jbyte b; jchar c; jshort s; jint i; jlong j; jfloat f; jdouble d; jobject l;
} jvalue;

/* JNIEnv* as seen by native code: a pointer to a pointer to the table. */
typedef void *const *JNIEnvPtr;
typedef void *const *JavaVMPtr;

typedef jvalue (*JImpl)(JNIEnvPtr env, jobject self, const jvalue *args, struct JMethod *m);
typedef jvalue (*JGetter)(JNIEnvPtr env, jobject self, struct JField *f);

typedef struct {
    const char *name;
    const char *sig;     /* NULL matches any signature */
    JImpl       impl;
} JMethodDef;

typedef struct {
    const char *name;
    const char *sig;
    JGetter     get;
} JFieldDef;

typedef struct {
    const char       *name;       /* "java/lang/Integer" */
    const char       *super;      /* NULL = java/lang/Object */
    const char       *iface;      /* one implemented interface, or NULL */
    const JMethodDef *methods;    /* terminated by a {NULL} entry */
    const JFieldDef  *fields;     /* terminated by a {NULL} entry */
} JClassDef;

typedef struct JMethod {
    struct JClass  *cls;
    char           *name, *sig;
    JImpl           impl;
    char            ret;          /* 'V','Z','B','C','S','I','J','F','D','L','[' */
    const char     *ret_class;    /* for 'L' and '[': the type after the ')' */
    int             nargs;
    char            args[32];
    int             warned;
    struct JMethod *next;
} JMethod;

typedef struct JField {
    struct JClass *cls;
    char          *name, *sig;
    JGetter        get;
    char           type;
    int            is_static;
    jvalue         static_value;
    struct JField *next;
} JField;

typedef struct JFieldSlot {
    JField            *field;
    jvalue             value;
    struct JFieldSlot *next;
} JFieldSlot;

enum { JK_PLAIN = 1, JK_CLASS, JK_STRING, JK_ARRAY };

typedef struct JObj {
    uint32_t       magic;
    uint8_t        kind;
    uint8_t        permanent;
    int32_t        refs;
    struct JClass *cls;
    JFieldSlot    *fields;
    union {
        char *utf8;                                         /* JK_STRING */
        struct { char elem; jsize len; void *data; } arr;   /* JK_ARRAY */
        jvalue box;                                         /* boxed primitives */
    } u;
} JObj;

typedef struct JClass {
    JObj              obj;        /* first: a jclass is a jobject */
    char              name[160];
    const JClassDef  *def;
    struct JClass    *super;
    JMethod          *methods;
    JField           *fields;
    struct JClass    *next;
} JClass;

/* ---------------------------------------------------------- lifecycle -- */
void      jni_init(void);
void      jni_bind_lime(so_module *lime);   /* after liblime is mapped */
JNIEnvPtr jni_get_env(void);
JavaVMPtr jni_get_vm(void);

/* ------------------------------------------------------------ objects -- */
JClass     *jni_find_class(const char *slash_name);
jobject     jni_new_object(JClass *cls);
jstring     jni_new_string(const char *utf8);
const char *jni_string_utf8(jobject s);              /* NULL if not a string */
jarray      jni_new_array(char elem, jsize len, const char *elem_class);
jobject     jni_ref(jobject o);
void        jni_unref(jobject o);
int         jni_valid(jobject o);
jobject     jni_activity(void);                       /* permanent */
jobject     jni_permanent_string(const char *utf8);

/* Field slots on plain objects. */
JField *jni_field(JClass *cls, const char *name, const char *sig, int is_static);
void    jni_set_field(jobject o, JField *f, jvalue v);
jvalue  jni_get_field(jobject o, JField *f);

/* ------------------------------------------------ Java -> Haxe callbacks -- */
/* Run Lime.onCallback(handle) on the UI thread (GameActivity.postUICallback). */
void jni_post_ui_callback(jlong handle);
/* HaxeObject.call(name, args) on the UI thread. Up to 4 args; each one is a
 * new reference that the queue takes over. haxeobj gets its own reference. */
void jni_post_haxe_call(jobject haxeobj, const char *function, int nargs, jobject *args);

#endif
