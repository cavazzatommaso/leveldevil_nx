/* jni_env.c -- see jni_env.h. MIT licensed, see LICENSE. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "jni_env.h"
#include "log.h"

#define JOBJ_MAGIC 0x4A4F424Au
#define JOBJ_DEAD  0xDEADDEADu
#define QUARANTINE 4096
#define MAX_ARGS   32

extern const JClassDef *const jni_class_defs[];
extern const int jni_class_def_count;

static RMutex  g_lock;                 /* registry, refcounts, field slots */
static JClass *g_classes;
static JClass *g_class_class;          /* java/lang/Class */
static JObj   *g_quarantine[QUARANTINE];
static int     g_quarantine_pos;
static __thread jobject t_pending;     /* pending Java exception */

/* ---------------------------------------------------------------- objects -- */

int jni_valid(jobject o) { return o && o->magic == JOBJ_MAGIC; }

static JObj *obj_alloc(JClass *cls, int kind)
{
    JObj *o = calloc(1, sizeof(JObj));
    if (!o) {
        LOGE("JNI: out of memory");
        return NULL;
    }
    o->magic = JOBJ_MAGIC;
    o->kind = (uint8_t)kind;
    o->refs = 1;
    o->cls = cls;
    return o;
}

jobject jni_ref(jobject o)
{
    if (!o)
        return NULL;
    if (!jni_valid(o)) {
        LOGE("JNI: reference to an invalid object %p", (void *)o);
        return NULL;
    }
    if (!o->permanent) {
        rmutexLock(&g_lock);
        o->refs++;
        rmutexUnlock(&g_lock);
    }
    return o;
}

static void obj_destroy(JObj *o)
{
    JFieldSlot *s = o->fields, *next;
    while (s) {
        next = s->next;
        if ((s->field->type == 'L' || s->field->type == '[') && s->value.l)
            jni_unref(s->value.l);
        free(s);
        s = next;
    }
    o->fields = NULL;
    if (o->kind == JK_STRING) {
        free(o->u.utf8);
        o->u.utf8 = NULL;
    } else if (o->kind == JK_ARRAY) {
        if (o->u.arr.elem == 'L') {
            jobject *el = o->u.arr.data;
            jsize i;
            for (i = 0; i < o->u.arr.len; i++)
                if (el[i])
                    jni_unref(el[i]);
        }
        free(o->u.arr.data);
        o->u.arr.data = NULL;
    }
    o->magic = JOBJ_DEAD;
    /* Keep the husk around for a while: a stale pointer then hits a dead
     * magic and gets logged instead of reading reused memory. */
    if (g_quarantine[g_quarantine_pos])
        free(g_quarantine[g_quarantine_pos]);
    g_quarantine[g_quarantine_pos] = o;
    g_quarantine_pos = (g_quarantine_pos + 1) % QUARANTINE;
}

void jni_unref(jobject o)
{
    if (!o)
        return;
    if (!jni_valid(o)) {
        LOG_ONCE("JNI: DeleteRef on a released object %p (double delete in native code)", (void *)o);
        return;
    }
    if (o->permanent)
        return;
    rmutexLock(&g_lock);
    if (--o->refs <= 0)
        obj_destroy(o);
    rmutexUnlock(&g_lock);
}

/* ---------------------------------------------------------------- classes -- */

static const JClassDef *find_def(const char *name)
{
    int i;
    for (i = 0; i < jni_class_def_count; i++)
        if (!strcmp(jni_class_defs[i]->name, name))
            return jni_class_defs[i];
    return NULL;
}

JClass *jni_find_class(const char *name)
{
    JClass *c;
    const JClassDef *def;
    char buf[160];
    size_t i;

    if (!name)
        return NULL;
    /* accept "java.lang.String" and "Ljava/lang/String;" as well */
    snprintf(buf, sizeof(buf), "%s", name);
    if (buf[0] == 'L' && buf[strlen(buf) - 1] == ';') {
        memmove(buf, buf + 1, strlen(buf));
        buf[strlen(buf) - 1] = '\0';
    }
    for (i = 0; buf[i]; i++)
        if (buf[i] == '.')
            buf[i] = '/';

    rmutexLock(&g_lock);
    for (c = g_classes; c; c = c->next) {
        if (!strcmp(c->name, buf)) {
            rmutexUnlock(&g_lock);
            return c;
        }
    }
    c = calloc(1, sizeof(JClass));
    if (!c) {
        rmutexUnlock(&g_lock);
        return NULL;
    }
    c->obj.magic = JOBJ_MAGIC;
    c->obj.kind = JK_CLASS;
    c->obj.permanent = 1;
    c->obj.refs = 1;
    snprintf(c->name, sizeof(c->name), "%s", buf);
    def = find_def(buf);
    c->def = def;
    c->next = g_classes;
    g_classes = c;
    c->obj.cls = g_class_class ? g_class_class : c;   /* java/lang/Class bootstraps itself */
    if (strcmp(buf, "java/lang/Object") != 0) {
        const char *super = def && def->super ? def->super
                          : (buf[0] == '[' ? "java/lang/Object" : "java/lang/Object");
        c->super = jni_find_class(super);
    }
    rmutexUnlock(&g_lock);
    if (!def && buf[0] != '[')
        LOGI("JNI: class %s is not modelled; calls on it return defaults", buf);
    else
        LOGD("JNI: class %s", buf);
    return c;
}

static int class_is(JClass *c, JClass *target)
{
    for (; c; c = c->super) {
        if (c == target)
            return 1;
        if (c->def && c->def->iface && !strcmp(c->def->iface, target->name))
            return 1;
    }
    return !strcmp(target->name, "java/lang/Object");
}

/* -------------------------------------------------------------- strings -- */

jstring jni_new_string(const char *utf8)
{
    static JClass *string_class;
    JObj *o;
    if (!string_class)
        string_class = jni_find_class("java/lang/String");
    o = obj_alloc(string_class, JK_STRING);
    if (!o)
        return NULL;
    o->u.utf8 = strdup(utf8 ? utf8 : "");
    return o;
}

jobject jni_permanent_string(const char *utf8)
{
    jobject s = jni_new_string(utf8);
    if (s)
        s->permanent = 1;
    return s;
}

const char *jni_string_utf8(jobject s)
{
    return (jni_valid(s) && s->kind == JK_STRING) ? s->u.utf8 : NULL;
}

/* UTF-8 -> UTF-16 code units. Returns the unit count; writes at most max. */
static jsize jutf8_to_utf16(const char *s, jchar *out, jsize max)
{
    jsize n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            cp = 0xFFFD;
            p++;
        }
        if (cp >= 0x10000) {
            if (out && n + 1 < max) {
                out[n] = (jchar)(0xD800 + ((cp - 0x10000) >> 10));
                out[n + 1] = (jchar)(0xDC00 + ((cp - 0x10000) & 0x3FF));
            }
            n += 2;
        } else {
            if (out && n < max)
                out[n] = (jchar)cp;
            n++;
        }
    }
    return n;
}

static char *jutf16_to_utf8(const jchar *s, jsize len)
{
    char *out = malloc((size_t)len * 3 + 1), *p = out;
    jsize i;
    if (!out)
        return NULL;
    for (i = 0; i < len; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
            /* 4 bytes; the 3-per-unit allowance covers the pair */
            *p++ = (char)(0xF0 | (cp >> 18));
            *p++ = (char)(0x80 | ((cp >> 12) & 0x3F));
            *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *p++ = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x80) {
            *p++ = (char)cp;
        } else if (cp < 0x800) {
            *p++ = (char)(0xC0 | (cp >> 6));
            *p++ = (char)(0x80 | (cp & 0x3F));
        } else {
            *p++ = (char)(0xE0 | (cp >> 12));
            *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
            *p++ = (char)(0x80 | (cp & 0x3F));
        }
    }
    *p = '\0';
    return out;
}

/* --------------------------------------------------------------- arrays -- */

static size_t elem_size(char e)
{
    switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default:            return 8;   /* J D L */
    }
}

jarray jni_new_array(char elem, jsize len, const char *elem_class)
{
    char name[160];
    JObj *o;
    if (len < 0)
        len = 0;
    if (elem == '[')
        elem = 'L';
    if (elem == 'L')
        snprintf(name, sizeof(name), "[L%s;", elem_class ? elem_class : "java/lang/Object");
    else
        snprintf(name, sizeof(name), "[%c", elem);
    o = obj_alloc(jni_find_class(name), JK_ARRAY);
    if (!o)
        return NULL;
    o->u.arr.elem = elem;
    o->u.arr.len = len;
    o->u.arr.data = calloc((size_t)(len ? len : 1), elem_size(elem));
    return o;
}

/* ---------------------------------------------------- methods and fields -- */

static int parse_sig(const char *sig, char *args, int max, char *ret, const char **ret_class)
{
    const char *p = sig;
    int n = 0;
    if (!p || *p != '(')
        return -1;
    p++;
    while (*p && *p != ')') {
        char t = *p;
        if (n < max)
            args[n] = t;
        if (t == 'L') {
            p = strchr(p, ';');
            if (!p)
                return -1;
            p++;
        } else if (t == '[') {
            while (*p == '[')
                p++;
            if (*p == 'L') {
                p = strchr(p, ';');
                if (!p)
                    return -1;
            }
            p++;
        } else {
            p++;
        }
        n++;
    }
    if (*p != ')')
        return -1;
    p++;
    *ret = *p ? *p : 'V';
    *ret_class = p;
    return n;
}

static jmethodID find_method(jclass clazz, const char *name, const char *sig)
{
    JClass *cls = (JClass *)clazz, *c;
    JMethod *m;
    if (!jni_valid(clazz) || clazz->kind != JK_CLASS || !name || !sig)
        return NULL;
    rmutexLock(&g_lock);
    for (m = cls->methods; m; m = m->next) {
        if (!strcmp(m->name, name) && !strcmp(m->sig, sig)) {
            rmutexUnlock(&g_lock);
            return m;
        }
    }
    m = calloc(1, sizeof(JMethod));
    if (!m) {
        rmutexUnlock(&g_lock);
        return NULL;
    }
    m->cls = cls;
    m->name = strdup(name);
    m->sig = strdup(sig);
    m->nargs = parse_sig(sig, m->args, MAX_ARGS, &m->ret, &m->ret_class);
    if (m->nargs < 0 || m->nargs > MAX_ARGS) {
        LOGE("JNI: bad signature %s.%s%s", cls->name, name, sig);
        m->nargs = 0;
        m->ret = 'V';
        m->ret_class = "V";
    }
    for (c = cls; c && !m->impl; c = c->super) {
        const JMethodDef *d;
        if (!c->def || !c->def->methods)
            continue;
        for (d = c->def->methods; d->name; d++) {
            if (!strcmp(d->name, name) && (!d->sig || !strcmp(d->sig, sig))) {
                m->impl = d->impl;
                break;
            }
        }
    }
    m->next = cls->methods;
    cls->methods = m;
    rmutexUnlock(&g_lock);
    LOGD("JNI: method %s.%s%s%s", cls->name, name, sig, m->impl ? "" : " (default)");
    return m;
}

JField *jni_field(JClass *cls, const char *name, const char *sig, int is_static)
{
    JClass *c;
    JField *f;
    if (!cls || !name || !sig)
        return NULL;
    rmutexLock(&g_lock);
    for (f = cls->fields; f; f = f->next) {
        if (!strcmp(f->name, name) && !strcmp(f->sig, sig) && f->is_static == is_static) {
            rmutexUnlock(&g_lock);
            return f;
        }
    }
    f = calloc(1, sizeof(JField));
    if (!f) {
        rmutexUnlock(&g_lock);
        return NULL;
    }
    f->cls = cls;
    f->name = strdup(name);
    f->sig = strdup(sig);
    f->type = sig[0];
    f->is_static = is_static;
    for (c = cls; c && !f->get; c = c->super) {
        const JFieldDef *d;
        if (!c->def || !c->def->fields)
            continue;
        for (d = c->def->fields; d->name; d++) {
            if (!strcmp(d->name, name) && (!d->sig || !strcmp(d->sig, sig))) {
                f->get = d->get;
                break;
            }
        }
    }
    f->next = cls->fields;
    cls->fields = f;
    rmutexUnlock(&g_lock);
    if (!f->get && is_static)
        LOGD("JNI: static field %s.%s %s has no value (default)", cls->name, name, sig);
    return f;
}

void jni_set_field(jobject o, JField *f, jvalue v)
{
    JFieldSlot *s;
    jobject old = NULL;
    if (!jni_valid(o) || !f)
        return;
    if (f->type == 'L' || f->type == '[')
        jni_ref(v.l);
    rmutexLock(&g_lock);
    for (s = o->fields; s; s = s->next)
        if (s->field == f)
            break;
    if (!s) {
        s = calloc(1, sizeof(JFieldSlot));
        if (!s) {
            rmutexUnlock(&g_lock);
            return;
        }
        s->field = f;
        s->next = o->fields;
        o->fields = s;
    } else if (f->type == 'L' || f->type == '[') {
        old = s->value.l;
    }
    s->value = v;
    rmutexUnlock(&g_lock);
    if (old)
        jni_unref(old);
}

jvalue jni_get_field(jobject o, JField *f)
{
    JFieldSlot *s;
    jvalue v;
    memset(&v, 0, sizeof(v));
    if (!jni_valid(o) || !f)
        return v;
    rmutexLock(&g_lock);
    for (s = o->fields; s; s = s->next) {
        if (s->field == f) {
            v = s->value;
            break;
        }
    }
    rmutexUnlock(&g_lock);
    return v;
}

/* ------------------------------------------------------------ invocation -- */

static jvalue default_result(const JMethod *m)
{
    jvalue r;
    memset(&r, 0, sizeof(r));
    if (m->ret == 'L' && !strncmp(m->ret_class, "Ljava/lang/String;", 18))
        r.l = jni_new_string("");
    else if (m->ret == '[') {
        const char *t = m->ret_class + 1;
        if (*t == 'L') {
            char cls[160];
            const char *semi = strchr(t, ';');
            size_t len = semi ? (size_t)(semi - t - 1) : 0;
            if (len >= sizeof(cls))
                len = sizeof(cls) - 1;
            memcpy(cls, t + 1, len);
            cls[len] = '\0';
            r.l = jni_new_array('L', 0, cls);
        } else {
            r.l = jni_new_array(*t, 0, NULL);
        }
    }
    return r;
}

static jvalue invoke(JNIEnvPtr env, jobject self, jmethodID mid, const jvalue *args)
{
    JMethod *m = mid;
    jvalue r;
    memset(&r, 0, sizeof(r));
    if (!m) {
        LOGE("JNI: call through a NULL method ID");
        return r;
    }
    LOGD("JNI: call %s.%s%s", m->cls->name, m->name, m->sig);
    if (!jni_valid(self)) {
        /* An instance call on null (or on an object native code already
         * released). Java would throw; a default keeps the game running, and
         * the class implementations never see a bad pointer. */
        LOG_ONCE("JNI: %s.%s called on a null/released object; returning a default",
                 m->cls->name, m->name);
        return default_result(m);
    }
    if (m->impl)
        return m->impl(env, self, args, m);
    if (!m->warned) {
        m->warned = 1;
        LOGI("JNI: %s.%s%s not implemented, returning a default", m->cls->name, m->name, m->sig);
    }
    return default_result(m);
}

static void va_to_args(const JMethod *m, va_list ap, jvalue *out)
{
    int i;
    if (!m)
        return;
    for (i = 0; i < m->nargs && i < MAX_ARGS; i++) {
        memset(&out[i], 0, sizeof(jvalue));
        switch (m->args[i]) {
        case 'Z': out[i].z = (jboolean)va_arg(ap, int); break;
        case 'B': out[i].b = (jbyte)va_arg(ap, int); break;
        case 'C': out[i].c = (jchar)va_arg(ap, int); break;
        case 'S': out[i].s = (jshort)va_arg(ap, int); break;
        case 'I': out[i].i = va_arg(ap, jint); break;
        case 'J': out[i].j = va_arg(ap, jlong); break;
        case 'F': out[i].f = (jfloat)va_arg(ap, double); break;
        case 'D': out[i].d = va_arg(ap, double); break;
        default:  out[i].l = va_arg(ap, jobject); break;
        }
    }
}

/* ======================================================= JNI functions ===== */

#define UNSUPPORTED(name) LOG_ONCE("JNI: " #name " is not supported")

static jint reserved0(void) { UNSUPPORTED(reserved0); return 0; }
static jint reserved1(void) { UNSUPPORTED(reserved1); return 0; }
static jint reserved2(void) { UNSUPPORTED(reserved2); return 0; }
static jint reserved3(void) { UNSUPPORTED(reserved3); return 0; }

static jint GetVersion(JNIEnvPtr env) { (void)env; return 0x00010006; }

static jclass DefineClass(JNIEnvPtr env, const char *name, jobject loader, const jbyte *buf, jsize len)
{
    (void)env; (void)loader; (void)buf; (void)len;
    LOGI("JNI: DefineClass(%s) refused", name ? name : "");
    return NULL;
}

static jclass FindClass(JNIEnvPtr env, const char *name) { (void)env; return (jclass)jni_find_class(name); }

static jmethodID FromReflectedMethod(JNIEnvPtr env, jobject m) { (void)env; (void)m; UNSUPPORTED(FromReflectedMethod); return NULL; }
static jfieldID FromReflectedField(JNIEnvPtr env, jobject f) { (void)env; (void)f; UNSUPPORTED(FromReflectedField); return NULL; }
static jobject ToReflectedMethod(JNIEnvPtr env, jclass c, jmethodID m, jboolean s) { (void)env; (void)c; (void)m; (void)s; UNSUPPORTED(ToReflectedMethod); return NULL; }
static jobject ToReflectedField(JNIEnvPtr env, jclass c, jfieldID f, jboolean s) { (void)env; (void)c; (void)f; (void)s; UNSUPPORTED(ToReflectedField); return NULL; }

static jclass GetSuperclass(JNIEnvPtr env, jclass c)
{
    (void)env;
    return (jni_valid(c) && c->kind == JK_CLASS) ? (jclass)((JClass *)c)->super : NULL;
}

static jboolean IsAssignableFrom(JNIEnvPtr env, jclass c1, jclass c2)
{
    (void)env;
    if (!jni_valid(c1) || !jni_valid(c2))
        return 0;
    return (jboolean)class_is((JClass *)c1, (JClass *)c2);
}

static jint Throw(JNIEnvPtr env, jthrowable t)
{
    (void)env;
    if (t_pending)
        jni_unref(t_pending);
    t_pending = jni_ref(t);
    return 0;
}

static jint ThrowNew(JNIEnvPtr env, jclass c, const char *msg)
{
    jobject t;
    (void)env;
    t = jni_new_object(jni_valid(c) ? (JClass *)c : jni_find_class("java/lang/RuntimeException"));
    if (t) {
        JField *f = jni_field(t->cls, "message", "Ljava/lang/String;", 0);
        jobject s = jni_new_string(msg);
        jvalue v;
        v.l = s;
        jni_set_field(t, f, v);
        jni_unref(s);
    }
    LOGI("JNI: exception thrown: %s: %s", t ? t->cls->name : "?", msg ? msg : "");
    if (t_pending)
        jni_unref(t_pending);
    t_pending = t;
    return 0;
}

static jthrowable ExceptionOccurred(JNIEnvPtr env) { (void)env; return jni_ref(t_pending); }

static void ExceptionDescribe(JNIEnvPtr env)
{
    (void)env;
    if (t_pending) {
        JField *f = jni_field(t_pending->cls, "message", "Ljava/lang/String;", 0);
        const char *msg = jni_string_utf8(jni_get_field(t_pending, f).l);
        LOGI("JNI: pending exception %s: %s", t_pending->cls->name, msg ? msg : "");
    }
}

static void ExceptionClear(JNIEnvPtr env)
{
    (void)env;
    if (t_pending) {
        jni_unref(t_pending);
        t_pending = NULL;
    }
}

static jboolean ExceptionCheck(JNIEnvPtr env) { (void)env; return t_pending != NULL; }

static void FatalError(JNIEnvPtr env, const char *msg)
{
    (void)env;
    LOGE("JNI FatalError: %s", msg ? msg : "");
    log_flush();
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

static jint PushLocalFrame(JNIEnvPtr env, jint cap) { (void)env; (void)cap; return 0; }
static jobject PopLocalFrame(JNIEnvPtr env, jobject result) { (void)env; return result; }
static jobject NewGlobalRef(JNIEnvPtr env, jobject o) { (void)env; return jni_ref(o); }
static void DeleteGlobalRef(JNIEnvPtr env, jobject o) { (void)env; jni_unref(o); }
static void DeleteLocalRef(JNIEnvPtr env, jobject o) { (void)env; jni_unref(o); }
static jboolean IsSameObject(JNIEnvPtr env, jobject a, jobject b) { (void)env; return a == b; }
static jobject NewLocalRef(JNIEnvPtr env, jobject o) { (void)env; return jni_ref(o); }
static jint EnsureLocalCapacity(JNIEnvPtr env, jint cap) { (void)env; (void)cap; return 0; }
static jobject NewWeakGlobalRef(JNIEnvPtr env, jobject o) { (void)env; return jni_ref(o); }
static void DeleteWeakGlobalRef(JNIEnvPtr env, jobject o) { (void)env; jni_unref(o); }

static jint GetObjectRefType(JNIEnvPtr env, jobject o)
{
    (void)env;
    if (!jni_valid(o))
        return 0;                            /* JNIInvalidRefType */
    return o->permanent ? 2 : 1;             /* global : local */
}

jobject jni_new_object(JClass *cls)
{
    return cls ? obj_alloc(cls, JK_PLAIN) : NULL;
}

static jobject AllocObject(JNIEnvPtr env, jclass c)
{
    (void)env;
    return jni_valid(c) ? jni_new_object((JClass *)c) : NULL;
}

static jobject NewObjectA(JNIEnvPtr env, jclass c, jmethodID ctor, const jvalue *args)
{
    jobject o;
    if (!jni_valid(c))
        return NULL;
    o = jni_new_object((JClass *)c);
    if (o && ctor && ((JMethod *)ctor)->impl)
        ((JMethod *)ctor)->impl(env, o, args, ctor);
    return o;
}

static jobject NewObjectV(JNIEnvPtr env, jclass c, jmethodID ctor, va_list ap)
{
    jvalue args[MAX_ARGS];
    va_to_args(ctor, ap, args);
    return NewObjectA(env, c, ctor, args);
}

static jobject NewObject(JNIEnvPtr env, jclass c, jmethodID ctor, ...)
{
    va_list ap;
    jobject o;
    va_start(ap, ctor);
    o = NewObjectV(env, c, ctor, ap);
    va_end(ap);
    return o;
}

static jclass GetObjectClass(JNIEnvPtr env, jobject o)
{
    (void)env;
    return jni_valid(o) ? (jclass)o->cls : NULL;
}

static jboolean IsInstanceOf(JNIEnvPtr env, jobject o, jclass c)
{
    (void)env;
    if (!o)
        return 1;
    if (!jni_valid(o) || !jni_valid(c))
        return 0;
    return (jboolean)class_is(o->cls, (JClass *)c);
}

static jmethodID GetMethodID(JNIEnvPtr env, jclass c, const char *name, const char *sig)
{
    (void)env;
    return find_method(c, name, sig);
}

static jmethodID GetStaticMethodID(JNIEnvPtr env, jclass c, const char *name, const char *sig)
{
    (void)env;
    return find_method(c, name, sig);
}

/* ---- Call<Type>Method, CallNonvirtual<Type>Method, CallStatic<Type>Method ---- */

#define DEFINE_CALLS(Name, T, fld)                                                                  \
static T Call##Name##MethodA(JNIEnvPtr env, jobject o, jmethodID m, const jvalue *a)                \
    { return invoke(env, o, m, a).fld; }                                                            \
static T Call##Name##MethodV(JNIEnvPtr env, jobject o, jmethodID m, va_list ap)                     \
    { jvalue a[MAX_ARGS]; va_to_args(m, ap, a); return invoke(env, o, m, a).fld; }                  \
static T Call##Name##Method(JNIEnvPtr env, jobject o, jmethodID m, ...)                             \
    { va_list ap; T r; va_start(ap, m); r = Call##Name##MethodV(env, o, m, ap); va_end(ap); return r; } \
static T CallNonvirtual##Name##MethodA(JNIEnvPtr env, jobject o, jclass c, jmethodID m, const jvalue *a) \
    { (void)c; return invoke(env, o, m, a).fld; }                                                   \
static T CallNonvirtual##Name##MethodV(JNIEnvPtr env, jobject o, jclass c, jmethodID m, va_list ap) \
    { jvalue a[MAX_ARGS]; (void)c; va_to_args(m, ap, a); return invoke(env, o, m, a).fld; }         \
static T CallNonvirtual##Name##Method(JNIEnvPtr env, jobject o, jclass c, jmethodID m, ...)         \
    { va_list ap; T r; va_start(ap, m); r = CallNonvirtual##Name##MethodV(env, o, c, m, ap); va_end(ap); return r; } \
static T CallStatic##Name##MethodA(JNIEnvPtr env, jclass c, jmethodID m, const jvalue *a)           \
    { return invoke(env, c, m, a).fld; }                                                            \
static T CallStatic##Name##MethodV(JNIEnvPtr env, jclass c, jmethodID m, va_list ap)                \
    { jvalue a[MAX_ARGS]; va_to_args(m, ap, a); return invoke(env, c, m, a).fld; }                  \
static T CallStatic##Name##Method(JNIEnvPtr env, jclass c, jmethodID m, ...)                        \
    { va_list ap; T r; va_start(ap, m); r = CallStatic##Name##MethodV(env, c, m, ap); va_end(ap); return r; }

DEFINE_CALLS(Object, jobject, l)
DEFINE_CALLS(Boolean, jboolean, z)
DEFINE_CALLS(Byte, jbyte, b)
DEFINE_CALLS(Char, jchar, c)
DEFINE_CALLS(Short, jshort, s)
DEFINE_CALLS(Int, jint, i)
DEFINE_CALLS(Long, jlong, j)
DEFINE_CALLS(Float, jfloat, f)
DEFINE_CALLS(Double, jdouble, d)

static void CallVoidMethodA(JNIEnvPtr env, jobject o, jmethodID m, const jvalue *a) { invoke(env, o, m, a); }
static void CallVoidMethodV(JNIEnvPtr env, jobject o, jmethodID m, va_list ap)
    { jvalue a[MAX_ARGS]; va_to_args(m, ap, a); invoke(env, o, m, a); }
static void CallVoidMethod(JNIEnvPtr env, jobject o, jmethodID m, ...)
    { va_list ap; va_start(ap, m); CallVoidMethodV(env, o, m, ap); va_end(ap); }
static void CallNonvirtualVoidMethodA(JNIEnvPtr env, jobject o, jclass c, jmethodID m, const jvalue *a)
    { (void)c; invoke(env, o, m, a); }
static void CallNonvirtualVoidMethodV(JNIEnvPtr env, jobject o, jclass c, jmethodID m, va_list ap)
    { jvalue a[MAX_ARGS]; (void)c; va_to_args(m, ap, a); invoke(env, o, m, a); }
static void CallNonvirtualVoidMethod(JNIEnvPtr env, jobject o, jclass c, jmethodID m, ...)
    { va_list ap; va_start(ap, m); CallNonvirtualVoidMethodV(env, o, c, m, ap); va_end(ap); }
static void CallStaticVoidMethodA(JNIEnvPtr env, jclass c, jmethodID m, const jvalue *a) { invoke(env, c, m, a); }
static void CallStaticVoidMethodV(JNIEnvPtr env, jclass c, jmethodID m, va_list ap)
    { jvalue a[MAX_ARGS]; va_to_args(m, ap, a); invoke(env, c, m, a); }
static void CallStaticVoidMethod(JNIEnvPtr env, jclass c, jmethodID m, ...)
    { va_list ap; va_start(ap, m); CallStaticVoidMethodV(env, c, m, ap); va_end(ap); }

/* ------------------------------------------------------------ fields ---- */

static jfieldID GetFieldID(JNIEnvPtr env, jclass c, const char *name, const char *sig)
{
    (void)env;
    return (jni_valid(c) && c->kind == JK_CLASS) ? jni_field((JClass *)c, name, sig, 0) : NULL;
}

static jfieldID GetStaticFieldID(JNIEnvPtr env, jclass c, const char *name, const char *sig)
{
    (void)env;
    return (jni_valid(c) && c->kind == JK_CLASS) ? jni_field((JClass *)c, name, sig, 1) : NULL;
}

static jvalue field_get(JNIEnvPtr env, jobject o, JField *f)
{
    jvalue v;
    memset(&v, 0, sizeof(v));
    if (!f)
        return v;
    if (f->get)
        return f->get(env, o, f);             /* getters hand out their own reference */
    v = f->is_static ? f->static_value : jni_get_field(o, f);
    if (f->type == 'L' || f->type == '[')
        v.l = jni_ref(v.l);
    return v;
}

static void static_set(JField *f, jvalue v)
{
    jobject old = NULL;
    if (!f)
        return;
    if (f->type == 'L' || f->type == '[') {
        jni_ref(v.l);
        old = f->static_value.l;
    }
    f->static_value = v;
    if (old)
        jni_unref(old);
}

#define DEFINE_FIELDS(Name, T, fld)                                                                 \
static T Get##Name##Field(JNIEnvPtr env, jobject o, jfieldID f)                                     \
    { return field_get(env, o, f).fld; }                                                            \
static void Set##Name##Field(JNIEnvPtr env, jobject o, jfieldID f, T value)                         \
    { jvalue v; (void)env; memset(&v, 0, sizeof(v)); v.fld = value; jni_set_field(o, f, v); }      \
static T GetStatic##Name##Field(JNIEnvPtr env, jclass c, jfieldID f)                                \
    { (void)c; return field_get(env, NULL, f).fld; }                                                \
static void SetStatic##Name##Field(JNIEnvPtr env, jclass c, jfieldID f, T value)                    \
    { jvalue v; (void)env; (void)c; memset(&v, 0, sizeof(v)); v.fld = value; static_set(f, v); }

DEFINE_FIELDS(Object, jobject, l)
DEFINE_FIELDS(Boolean, jboolean, z)
DEFINE_FIELDS(Byte, jbyte, b)
DEFINE_FIELDS(Char, jchar, c)
DEFINE_FIELDS(Short, jshort, s)
DEFINE_FIELDS(Int, jint, i)
DEFINE_FIELDS(Long, jlong, j)
DEFINE_FIELDS(Float, jfloat, f)
DEFINE_FIELDS(Double, jdouble, d)

/* ----------------------------------------------------------- strings ---- */

static jstring NewString(JNIEnvPtr env, const jchar *chars, jsize len)
{
    char *u = jutf16_to_utf8(chars, len);
    jstring s;
    (void)env;
    s = jni_new_string(u ? u : "");
    free(u);
    return s;
}

static jsize GetStringLength(JNIEnvPtr env, jstring s)
{
    const char *u = jni_string_utf8(s);
    (void)env;
    return u ? jutf8_to_utf16(u, NULL, 0) : 0;
}

static const jchar *GetStringChars(JNIEnvPtr env, jstring s, jboolean *is_copy)
{
    const char *u = jni_string_utf8(s);
    jsize n;
    jchar *out;
    (void)env;
    if (is_copy)
        *is_copy = 1;
    if (!u)
        return NULL;
    n = jutf8_to_utf16(u, NULL, 0);
    out = calloc((size_t)n + 1, sizeof(jchar));
    if (out)
        jutf8_to_utf16(u, out, n);
    return out;
}

static void ReleaseStringChars(JNIEnvPtr env, jstring s, const jchar *chars)
{
    (void)env; (void)s;
    free((void *)chars);
}

static jstring NewStringUTF(JNIEnvPtr env, const char *utf8)
{
    (void)env;
    return utf8 ? jni_new_string(utf8) : NULL;
}

static jsize GetStringUTFLength(JNIEnvPtr env, jstring s)
{
    const char *u = jni_string_utf8(s);
    (void)env;
    return u ? (jsize)strlen(u) : 0;
}

static const char *GetStringUTFChars(JNIEnvPtr env, jstring s, jboolean *is_copy)
{
    (void)env;
    if (is_copy)
        *is_copy = 0;
    return jni_string_utf8(s);      /* strings are immutable; no copy needed */
}

static void ReleaseStringUTFChars(JNIEnvPtr env, jstring s, const char *chars)
{
    (void)env; (void)s; (void)chars;
}

static void GetStringRegion(JNIEnvPtr env, jstring s, jsize start, jsize len, jchar *buf)
{
    const jchar *all = GetStringChars(env, s, NULL);
    jsize n = GetStringLength(env, s);
    if (all && start >= 0 && len >= 0 && start + len <= n)
        memcpy(buf, all + start, (size_t)len * sizeof(jchar));
    free((void *)all);
}

static void GetStringUTFRegion(JNIEnvPtr env, jstring s, jsize start, jsize len, char *buf)
{
    const jchar *all = GetStringChars(env, s, NULL);
    jsize n = GetStringLength(env, s);
    if (all && start >= 0 && len >= 0 && start + len <= n) {
        char *u = jutf16_to_utf8(all + start, len);
        if (u) {
            strcpy(buf, u);
            free(u);
        }
    }
    free((void *)all);
}

static const jchar *GetStringCritical(JNIEnvPtr env, jstring s, jboolean *is_copy)
{
    return GetStringChars(env, s, is_copy);
}

static void ReleaseStringCritical(JNIEnvPtr env, jstring s, const jchar *chars)
{
    ReleaseStringChars(env, s, chars);
}

/* ------------------------------------------------------------ arrays ---- */

static jsize GetArrayLength(JNIEnvPtr env, jarray a)
{
    (void)env;
    return (jni_valid(a) && a->kind == JK_ARRAY) ? a->u.arr.len : 0;
}

static jobjectArray NewObjectArray(JNIEnvPtr env, jsize len, jclass elem, jobject init)
{
    jobject a;
    jsize i;
    (void)env;
    a = jni_new_array('L', len, jni_valid(elem) && elem->kind == JK_CLASS ? ((JClass *)elem)->name : NULL);
    if (a && init) {
        jobject *el = a->u.arr.data;
        for (i = 0; i < len; i++)
            el[i] = jni_ref(init);
    }
    return a;
}

static jobject GetObjectArrayElement(JNIEnvPtr env, jobjectArray a, jsize i)
{
    (void)env;
    if (!jni_valid(a) || a->kind != JK_ARRAY || a->u.arr.elem != 'L' || i < 0 || i >= a->u.arr.len)
        return NULL;
    return jni_ref(((jobject *)a->u.arr.data)[i]);
}

static void SetObjectArrayElement(JNIEnvPtr env, jobjectArray a, jsize i, jobject v)
{
    jobject old;
    (void)env;
    if (!jni_valid(a) || a->kind != JK_ARRAY || a->u.arr.elem != 'L' || i < 0 || i >= a->u.arr.len)
        return;
    old = ((jobject *)a->u.arr.data)[i];
    ((jobject *)a->u.arr.data)[i] = jni_ref(v);
    if (old)
        jni_unref(old);
}

static void *array_data(jarray a, char elem)
{
    if (!jni_valid(a) || a->kind != JK_ARRAY)
        return NULL;
    if (a->u.arr.elem != elem)
        LOG_ONCE("JNI: array element type mismatch (%c vs %c)", a->u.arr.elem, elem);
    return a->u.arr.data;
}

static void array_region(jarray a, char elem, jsize start, jsize len, void *buf, int set)
{
    void *d = array_data(a, elem);
    size_t es = elem_size(elem);
    if (!d || start < 0 || len < 0 || start + len > a->u.arr.len)
        return;
    if (set)
        memcpy((char *)d + (size_t)start * es, buf, (size_t)len * es);
    else
        memcpy(buf, (char *)d + (size_t)start * es, (size_t)len * es);
}

#define DEFINE_ARRAYS(Name, T, code)                                                                \
static jarray New##Name##Array(JNIEnvPtr env, jsize len)                                           \
    { (void)env; return jni_new_array(code, len, NULL); }                                           \
static T *Get##Name##ArrayElements(JNIEnvPtr env, jarray a, jboolean *is_copy)                      \
    { (void)env; if (is_copy) *is_copy = 0; return (T *)array_data(a, code); }                      \
static void Release##Name##ArrayElements(JNIEnvPtr env, jarray a, T *elems, jint mode)              \
    { (void)env; (void)a; (void)elems; (void)mode; }                                                \
static void Get##Name##ArrayRegion(JNIEnvPtr env, jarray a, jsize start, jsize len, T *buf)         \
    { (void)env; array_region(a, code, start, len, buf, 0); }                                       \
static void Set##Name##ArrayRegion(JNIEnvPtr env, jarray a, jsize start, jsize len, const T *buf)   \
    { (void)env; array_region(a, code, start, len, (void *)buf, 1); }

DEFINE_ARRAYS(Boolean, jboolean, 'Z')
DEFINE_ARRAYS(Byte, jbyte, 'B')
DEFINE_ARRAYS(Char, jchar, 'C')
DEFINE_ARRAYS(Short, jshort, 'S')
DEFINE_ARRAYS(Int, jint, 'I')
DEFINE_ARRAYS(Long, jlong, 'J')
DEFINE_ARRAYS(Float, jfloat, 'F')
DEFINE_ARRAYS(Double, jdouble, 'D')

static void *GetPrimitiveArrayCritical(JNIEnvPtr env, jarray a, jboolean *is_copy)
{
    (void)env;
    if (is_copy)
        *is_copy = 0;
    return (jni_valid(a) && a->kind == JK_ARRAY) ? a->u.arr.data : NULL;
}

static void ReleasePrimitiveArrayCritical(JNIEnvPtr env, jarray a, void *data, jint mode)
{
    (void)env; (void)a; (void)data; (void)mode;
}

/* ------------------------------------------------------------- misc ----- */

typedef struct { const char *name; const char *signature; void *fnPtr; } JNINativeMethod;

static jint RegisterNatives(JNIEnvPtr env, jclass c, const JNINativeMethod *methods, jint n)
{
    jint i;
    (void)env;
    for (i = 0; i < n; i++)
        LOGD("JNI: RegisterNatives %s.%s%s", jni_valid(c) ? ((JClass *)c)->name : "?",
             methods[i].name, methods[i].signature);
    return 0;
}

static jint UnregisterNatives(JNIEnvPtr env, jclass c) { (void)env; (void)c; return 0; }
static jint MonitorEnter(JNIEnvPtr env, jobject o) { (void)env; (void)o; return 0; }
static jint MonitorExit(JNIEnvPtr env, jobject o) { (void)env; (void)o; return 0; }

static jint GetJavaVM(JNIEnvPtr env, JavaVMPtr *vm)
{
    (void)env;
    if (vm)
        *vm = jni_get_vm();
    return 0;
}

static jobject NewDirectByteBuffer(JNIEnvPtr env, void *addr, jlong cap)
{
    (void)env; (void)addr; (void)cap;
    UNSUPPORTED(NewDirectByteBuffer);
    return NULL;
}

static void *GetDirectBufferAddress(JNIEnvPtr env, jobject buf) { (void)env; (void)buf; return NULL; }
static jlong GetDirectBufferCapacity(JNIEnvPtr env, jobject buf) { (void)env; (void)buf; return -1; }

/* ---------------------------------------------------------- the table ---- */
/* Built from jni_slots.h: every slot name must exist above as a function, so
 * a missing or misspelled entry is a compile error rather than a NULL slot. */

#define JNI_SLOT(i, Name) [i] = (void *)&Name,
static void *const g_jni_table[] = {
#include "jni_slots.h"
};
#undef JNI_SLOT

static void *const *const g_env = g_jni_table;

JNIEnvPtr jni_get_env(void) { return (JNIEnvPtr)&g_env; }

/* JavaVM invoke interface: reserved0..2, DestroyJavaVM, AttachCurrentThread,
 * DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon. */
static jint vm_destroy(JavaVMPtr vm) { (void)vm; return 0; }

static jint vm_attach(JavaVMPtr vm, JNIEnvPtr *env, void *args)
{
    (void)vm; (void)args;
    if (env)
        *env = jni_get_env();
    return 0;
}

static jint vm_detach(JavaVMPtr vm) { (void)vm; return 0; }

static jint vm_get_env(JavaVMPtr vm, JNIEnvPtr *env, jint version)
{
    (void)vm; (void)version;
    if (env)
        *env = jni_get_env();
    return 0;
}

static void *const g_vm_table[] = {
    NULL, NULL, NULL, (void *)&vm_destroy, (void *)&vm_attach, (void *)&vm_detach,
    (void *)&vm_get_env, (void *)&vm_attach,
};
static void *const *const g_vm = g_vm_table;

JavaVMPtr jni_get_vm(void) { return (JavaVMPtr)&g_vm; }

/* ---------------------------------------------------------- activity ---- */

static jobject g_activity;

jobject jni_activity(void)
{
    if (!g_activity) {
        /* ANativeActivity.clazz: the launcher activity from the manifest. */
        g_activity = jni_new_object(jni_find_class("com/dynamo/android/DefoldActivity"));
        if (g_activity)
            g_activity->permanent = 1;
    }
    return g_activity;
}

/* ----------------------------------------------------------- lifecycle -- */

void jni_init(void)
{
    JClass *c;
    rmutexInit(&g_lock);
    g_class_class = jni_find_class("java/lang/Class");
    /* classes created while bootstrapping java/lang/Class point at themselves */
    for (c = g_classes; c; c = c->next)
        c->obj.cls = g_class_class;
    jni_activity();
    LOGI("JNI: %d table slots, %d modelled classes",
         (int)(sizeof(g_jni_table) / sizeof(g_jni_table[0])), jni_class_def_count);
}
