/* jni_classes.c -- the Java classes the game touches, as method/field tables.
 *
 *   java.lang      Object, Class, String, boxes, Throwable, ClassLoader
 *   Android        Activity/Context, NativeActivity, DefoldActivity, Build,
 *                  Locale, Environment, File, ApplicationInfo, Display
 *
 * Defold's extensions (ads, IAP, Firebase, Play Games...) load their Java
 * classes through the activity's ClassLoader; none of them is modelled, so
 * every call on them returns the type's default -- "not available".
 *
 * MIT licensed, see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "config.h"
#include "jni_env.h"
#include "log.h"
#include "paths.h"

#define IMPL(fn) static jvalue fn(JNIEnvPtr env, jobject self, const jvalue *args, JMethod *m)
#define GETTER(fn) static jvalue fn(JNIEnvPtr env, jobject self, JField *f)

static jvalue v_none(void)           { jvalue v; memset(&v, 0, sizeof(v)); return v; }
static jvalue v_bool(int b)          { jvalue v = v_none(); v.z = (jboolean)(b != 0); return v; }
static jvalue v_int(jint i)          { jvalue v = v_none(); v.i = i; return v; }
static jvalue v_double(jdouble d)    { jvalue v = v_none(); v.d = d; return v; }
static jvalue v_float(jfloat f)      { jvalue v = v_none(); v.f = f; return v; }
static jvalue v_obj(jobject o)       { jvalue v = v_none(); v.l = o; return v; }
static jvalue v_str(const char *s)   { return v_obj(jni_new_string(s)); }

static const char *arg_str(const jvalue *args, int i)
{
    const char *s = args ? jni_string_utf8(args[i].l) : NULL;
    return s ? s : "";
}

static void dotted(const char *slash, char *out, size_t n)
{
    size_t i;
    snprintf(out, n, "%s", slash);
    for (i = 0; out[i]; i++)
        if (out[i] == '/')
            out[i] = '.';
}

/* ============================================================ java.lang === */

static char box_code(const JClass *c)
{
    static const struct { const char *name; char code; } boxes[] = {
        { "java/lang/Boolean", 'Z' }, { "java/lang/Byte", 'B' },    { "java/lang/Character", 'C' },
        { "java/lang/Short", 'S' },   { "java/lang/Integer", 'I' }, { "java/lang/Long", 'J' },
        { "java/lang/Float", 'F' },   { "java/lang/Double", 'D' },
    };
    size_t i;
    for (i = 0; c && i < sizeof(boxes) / sizeof(boxes[0]); i++)
        if (!strcmp(c->name, boxes[i].name))
            return boxes[i].code;
    return 0;
}

static void box_store(jobject o, char argtype, jvalue a)
{
    long long iv = 0;
    double dv = 0;
    switch (argtype) {
    case 'Z': iv = a.z; dv = a.z; break;
    case 'B': iv = a.b; dv = a.b; break;
    case 'C': iv = a.c; dv = a.c; break;
    case 'S': iv = a.s; dv = a.s; break;
    case 'I': iv = a.i; dv = a.i; break;
    case 'J': iv = a.j; dv = (double)a.j; break;
    case 'F': dv = a.f; iv = (long long)a.f; break;
    case 'D': dv = a.d; iv = (long long)a.d; break;
    case 'L': {
        const char *s = jni_string_utf8(a.l);
        dv = s ? strtod(s, NULL) : 0;
        iv = s ? (!strcmp(s, "true") ? 1 : strtoll(s, NULL, 10)) : 0;
        break;
    }
    default: break;
    }
    memset(&o->u.box, 0, sizeof(o->u.box));
    switch (box_code(o->cls)) {
    case 'Z': o->u.box.z = iv != 0; break;
    case 'B': o->u.box.b = (jbyte)iv; break;
    case 'C': o->u.box.c = (jchar)iv; break;
    case 'S': o->u.box.s = (jshort)iv; break;
    case 'I': o->u.box.i = (jint)iv; break;
    case 'J': o->u.box.j = iv; break;
    case 'F': o->u.box.f = (jfloat)dv; break;
    case 'D': o->u.box.d = dv; break;
    default: break;
    }
}

static double box_double(jobject o)
{
    switch (box_code(o->cls)) {
    case 'Z': return o->u.box.z;
    case 'B': return o->u.box.b;
    case 'C': return o->u.box.c;
    case 'S': return o->u.box.s;
    case 'I': return o->u.box.i;
    case 'J': return (double)o->u.box.j;
    case 'F': return o->u.box.f;
    case 'D': return o->u.box.d;
    default:  return 0;
    }
}

IMPL(obj_init) { (void)env; (void)self; (void)args; (void)m; return v_none(); }

IMPL(obj_toString)
{
    char buf[256], name[160];
    (void)env; (void)args; (void)m;
    if (self->kind == JK_STRING)
        return v_obj(jni_ref(self));
    if (self->kind == JK_CLASS) {
        dotted(((JClass *)self)->name, name, sizeof(name));
        snprintf(buf, sizeof(buf), "class %s", name);
    } else if (box_code(self->cls)) {
        switch (box_code(self->cls)) {
        case 'Z': snprintf(buf, sizeof(buf), "%s", self->u.box.z ? "true" : "false"); break;
        case 'C': snprintf(buf, sizeof(buf), "%c", (char)self->u.box.c); break;
        case 'F': case 'D': snprintf(buf, sizeof(buf), "%g", box_double(self)); break;
        case 'J': snprintf(buf, sizeof(buf), "%lld", (long long)self->u.box.j); break;
        default:  snprintf(buf, sizeof(buf), "%d", (int)box_double(self)); break;
        }
    } else {
        dotted(self->cls->name, name, sizeof(name));
        snprintf(buf, sizeof(buf), "%s@%lx", name, (unsigned long)((uintptr_t)self >> 4));
    }
    return v_str(buf);
}

IMPL(obj_hashCode) { (void)env; (void)args; (void)m; return v_int((jint)((uintptr_t)self >> 4)); }

IMPL(obj_equals)
{
    const char *a = jni_string_utf8(self), *b = args ? jni_string_utf8(args[0].l) : NULL;
    (void)env; (void)m;
    if (args && self == args[0].l)
        return v_bool(1);
    return v_bool(a && b && !strcmp(a, b));
}

IMPL(obj_getClass) { (void)env; (void)args; (void)m; return v_obj((jobject)self->cls); }

static const JMethodDef object_methods[] = {
    { "<init>", "()V", obj_init },
    { "toString", "()Ljava/lang/String;", obj_toString },
    { "hashCode", "()I", obj_hashCode },
    { "equals", "(Ljava/lang/Object;)Z", obj_equals },
    { "getClass", "()Ljava/lang/Class;", obj_getClass },
    { NULL, NULL, NULL },
};
static const JClassDef class_Object = { "java/lang/Object", NULL, NULL, object_methods, NULL };

IMPL(class_getName)
{
    char name[160];
    (void)env; (void)args; (void)m;
    if (self->kind != JK_CLASS)
        return v_str("");
    dotted(((JClass *)self)->name, name, sizeof(name));
    return v_str(name);
}

IMPL(class_getSimpleName)
{
    const char *n, *slash;
    (void)env; (void)args; (void)m;
    if (self->kind != JK_CLASS)
        return v_str("");
    n = ((JClass *)self)->name;
    slash = strrchr(n, '/');
    return v_str(slash ? slash + 1 : n);
}

IMPL(class_isArray)
{
    (void)env; (void)args; (void)m;
    return v_bool(self->kind == JK_CLASS && ((JClass *)self)->name[0] == '[');
}

static const JMethodDef class_methods[] = {
    { "getName", "()Ljava/lang/String;", class_getName },
    { "getSimpleName", "()Ljava/lang/String;", class_getSimpleName },
    { "isArray", "()Z", class_isArray },
    { NULL, NULL, NULL },
};
static const JClassDef class_Class = { "java/lang/Class", NULL, NULL, class_methods, NULL };

IMPL(str_length)
{
    const char *s = jni_string_utf8(self);
    jint n = 0;
    (void)env; (void)args; (void)m;
    for (; s && *s; s++)                    /* UTF-16 units: count lead bytes, 4-byte = 2 */
        if ((*s & 0xC0) != 0x80)
            n += ((*s & 0xF8) == 0xF0) ? 2 : 1;
    return v_int(n);
}

IMPL(str_isEmpty)
{
    const char *s = jni_string_utf8(self);
    (void)env; (void)args; (void)m;
    return v_bool(!s || !*s);
}

IMPL(str_getBytes)
{
    const char *s = jni_string_utf8(self);
    size_t n = s ? strlen(s) : 0;
    jobject a = jni_new_array('B', (jsize)n, NULL);
    (void)env; (void)args; (void)m;
    if (a && n)
        memcpy(a->u.arr.data, s, n);
    return v_obj(a);
}

static const JMethodDef string_methods[] = {
    { "length", "()I", str_length },
    { "isEmpty", "()Z", str_isEmpty },
    { "getBytes", NULL, str_getBytes },
    { NULL, NULL, NULL },
};
static const JClassDef class_String = { "java/lang/String", NULL, "java/lang/CharSequence", string_methods, NULL };
static const JClassDef class_CharSequence = { "java/lang/CharSequence", NULL, NULL, NULL, NULL };

IMPL(box_init)
{
    (void)env;
    if (args && m->nargs >= 1)
        box_store(self, m->args[0], args[0]);
    return v_none();
}

IMPL(box_valueOf)
{
    jobject o = jni_new_object((JClass *)self);   /* static: self is the class */
    (void)env;
    if (o && args && m->nargs >= 1)
        box_store(o, m->args[0], args[0]);
    return v_obj(o);
}

IMPL(box_booleanValue) { (void)env; (void)args; (void)m; return v_bool(box_double(self) != 0); }
IMPL(box_charValue)    { jvalue v = v_none(); (void)env; (void)args; (void)m; v.c = (jchar)box_double(self); return v; }
IMPL(box_byteValue)    { jvalue v = v_none(); (void)env; (void)args; (void)m; v.b = (jbyte)box_double(self); return v; }
IMPL(box_shortValue)   { jvalue v = v_none(); (void)env; (void)args; (void)m; v.s = (jshort)box_double(self); return v; }
IMPL(box_intValue)     { (void)env; (void)args; (void)m; return v_int((jint)box_double(self)); }
IMPL(box_floatValue)   { jvalue v = v_none(); (void)env; (void)args; (void)m; v.f = (jfloat)box_double(self); return v; }
IMPL(box_doubleValue)  { (void)env; (void)args; (void)m; return v_double(box_double(self)); }

IMPL(box_longValue)
{
    jvalue v = v_none();
    (void)env; (void)args; (void)m;
    v.j = box_code(self->cls) == 'J' ? self->u.box.j : (jlong)box_double(self);
    return v;
}

static const JMethodDef box_methods[] = {
    { "<init>", NULL, box_init },
    { "valueOf", NULL, box_valueOf },
    { "booleanValue", "()Z", box_booleanValue },
    { "charValue", "()C", box_charValue },
    { "byteValue", "()B", box_byteValue },
    { "shortValue", "()S", box_shortValue },
    { "intValue", "()I", box_intValue },
    { "longValue", "()J", box_longValue },
    { "floatValue", "()F", box_floatValue },
    { "doubleValue", "()D", box_doubleValue },
    { NULL, NULL, NULL },
};
static const JClassDef class_Number    = { "java/lang/Number", NULL, NULL, box_methods, NULL };
static const JClassDef class_Boolean   = { "java/lang/Boolean", NULL, NULL, box_methods, NULL };
static const JClassDef class_Character = { "java/lang/Character", NULL, NULL, box_methods, NULL };
static const JClassDef class_Byte      = { "java/lang/Byte", "java/lang/Number", NULL, box_methods, NULL };
static const JClassDef class_Short     = { "java/lang/Short", "java/lang/Number", NULL, box_methods, NULL };
static const JClassDef class_Integer   = { "java/lang/Integer", "java/lang/Number", NULL, box_methods, NULL };
static const JClassDef class_Long      = { "java/lang/Long", "java/lang/Number", NULL, box_methods, NULL };
static const JClassDef class_Float     = { "java/lang/Float", "java/lang/Number", NULL, box_methods, NULL };
static const JClassDef class_Double    = { "java/lang/Double", "java/lang/Number", NULL, box_methods, NULL };

IMPL(throwable_init)
{
    (void)env;
    if (args && m->nargs >= 1 && m->args[0] == 'L') {
        jvalue v = v_obj(args[0].l);
        jni_set_field(self, jni_field(self->cls, "message", "Ljava/lang/String;", 0), v);
    }
    return v_none();
}

IMPL(throwable_getMessage)
{
    jvalue v = jni_get_field(self, jni_field(self->cls, "message", "Ljava/lang/String;", 0));
    (void)env; (void)args; (void)m;
    return v_obj(jni_ref(v.l));
}

static const JMethodDef throwable_methods[] = {
    { "<init>", NULL, throwable_init },
    { "getMessage", "()Ljava/lang/String;", throwable_getMessage },
    { NULL, NULL, NULL },
};
static const JClassDef class_Throwable = { "java/lang/Throwable", NULL, NULL, throwable_methods, NULL };
static const JClassDef class_Exception = { "java/lang/Exception", "java/lang/Throwable", NULL, NULL, NULL };
static const JClassDef class_RuntimeException = { "java/lang/RuntimeException", "java/lang/Exception", NULL, NULL, NULL };

static jobject g_package;

static jobject package_name(void)
{
    if (!g_package)
        g_package = jni_permanent_string(PB_PACKAGE);
    return g_package;
}


/* ============================================================== Android === */

static jobject new_file(const char *path)
{
    JClass *cls = jni_find_class("java/io/File");
    jobject o = jni_new_object(cls), s;
    if (!o)
        return NULL;
    s = jni_new_string(path);
    jni_set_field(o, jni_field(cls, "path", "Ljava/lang/String;", 0), v_obj(s));
    jni_unref(s);
    return o;
}

IMPL(file_init)
{
    (void)env;
    if (args && m->nargs >= 1 && m->args[0] == 'L')
        jni_set_field(self, jni_field(self->cls, "path", "Ljava/lang/String;", 0), args[0]);
    return v_none();
}

IMPL(file_getPath)
{
    jvalue v = jni_get_field(self, jni_field(self->cls, "path", "Ljava/lang/String;", 0));
    (void)env; (void)args; (void)m;
    return v.l ? v_obj(jni_ref(v.l)) : v_str("");
}

IMPL(file_true) { (void)env; (void)self; (void)args; (void)m; return v_bool(1); }

static const JMethodDef file_methods[] = {
    { "<init>", NULL, file_init },
    { "getPath", "()Ljava/lang/String;", file_getPath },
    { "getAbsolutePath", "()Ljava/lang/String;", file_getPath },
    { "getCanonicalPath", "()Ljava/lang/String;", file_getPath },
    { "toString", "()Ljava/lang/String;", file_getPath },
    { "exists", "()Z", file_true },
    { "mkdirs", "()Z", file_true },
    { NULL, NULL, NULL },
};
static const JClassDef class_File = { "java/io/File", NULL, NULL, file_methods, NULL };

IMPL(ctx_getPackageName) { (void)env; (void)self; (void)args; (void)m; return v_obj(package_name()); }
IMPL(ctx_self)           { (void)env; (void)self; (void)args; (void)m; return v_obj(jni_activity()); }
IMPL(ctx_filesDir)       { (void)env; (void)self; (void)args; (void)m; return v_obj(new_file(paths_save_nodev())); }

IMPL(act_moveTaskToBack)
{
    (void)env; (void)self; (void)args; (void)m;
    LOGI("Activity.moveTaskToBack: the game asked to quit");
    pb_request_exit(0);
    return v_bool(1);
}

static const JMethodDef context_methods[] = {
    { "getPackageName", "()Ljava/lang/String;", ctx_getPackageName },
    { "getApplicationContext", NULL, ctx_self },
    { "getFilesDir", "()Ljava/io/File;", ctx_filesDir },
    { "getCacheDir", "()Ljava/io/File;", ctx_filesDir },
    { "getExternalFilesDir", NULL, ctx_filesDir },
    { "moveTaskToBack", "(Z)Z", act_moveTaskToBack },
    { "finish", "()V", act_moveTaskToBack },
    { NULL, NULL, NULL },
};
static const JClassDef class_Context = { "android/content/Context", NULL, NULL, context_methods, NULL };
static const JClassDef class_Activity = { "android/app/Activity", "android/content/Context", NULL, NULL, NULL };

static jobject perm(const char *s)
{
    return jni_permanent_string(s);
}

GETTER(build_manufacturer) { static jobject s; (void)env; (void)self; (void)f; if (!s) s = perm("Nintendo"); return v_obj(s); }
GETTER(build_model)        { static jobject s; (void)env; (void)self; (void)f; if (!s) s = perm("Switch"); return v_obj(s); }
GETTER(build_device)       { static jobject s; (void)env; (void)self; (void)f; if (!s) s = perm("switch"); return v_obj(s); }
GETTER(build_hardware)     { static jobject s; (void)env; (void)self; (void)f; if (!s) s = perm("nx"); return v_obj(s); }
GETTER(version_release)    { static jobject s; (void)env; (void)self; (void)f; if (!s) s = perm("10"); return v_obj(s); }
GETTER(version_codename)   { static jobject s; (void)env; (void)self; (void)f; if (!s) s = perm("REL"); return v_obj(s); }
GETTER(version_sdk_int)    { (void)env; (void)self; (void)f; return v_int(29); }

static const JFieldDef build_fields[] = {
    { "MANUFACTURER", NULL, build_manufacturer },
    { "BRAND", NULL, build_manufacturer },
    { "MODEL", NULL, build_model },
    { "DEVICE", NULL, build_device },
    { "PRODUCT", NULL, build_device },
    { "HARDWARE", NULL, build_hardware },
    { NULL, NULL, NULL },
};
static const JFieldDef version_fields[] = {
    { "RELEASE", NULL, version_release },
    { "CODENAME", NULL, version_codename },
    { "SDK_INT", "I", version_sdk_int },
    { NULL, NULL, NULL },
};
static const JClassDef class_Build = { "android/os/Build", NULL, NULL, NULL, build_fields };
static const JClassDef class_BuildVersion = { "android/os/Build$VERSION", NULL, NULL, NULL, version_fields };

IMPL(env_storageState) { (void)env; (void)self; (void)args; (void)m; return v_str("mounted"); }

static const JMethodDef environment_methods[] = {
    { "getExternalStorageState", NULL, env_storageState },
    { "getExternalStorageDirectory", NULL, ctx_filesDir },
    { NULL, NULL, NULL },
};
static const JClassDef class_Environment = { "android/os/Environment", NULL, NULL, environment_methods, NULL };

static const char *system_locale(void)
{
    static char locale[16];
    static const char *const names[] = {
        "ja_JP", "en_US", "fr_FR", "de_DE", "it_IT", "es_ES", "zh_CN", "ko_KR", "nl_NL",
        "pt_PT", "ru_RU", "zh_TW", "en_GB", "fr_CA", "es_MX", "zh_CN", "zh_TW", "pt_BR",
    };
    if (!locale[0]) {
        u64 code = 0;
        SetLanguage lang = SetLanguage_ENUS;
        snprintf(locale, sizeof(locale), "en_US");
        if (R_SUCCEEDED(setInitialize())) {
            if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang)) &&
                (unsigned)lang < sizeof(names) / sizeof(names[0]))
                snprintf(locale, sizeof(locale), "%s", names[lang]);
            setExit();
        }
        LOGI("locale: %s", locale);
    }
    return locale;
}

static jobject g_locale;

IMPL(locale_getDefault)
{
    (void)env; (void)self; (void)args; (void)m;
    if (!g_locale) {
        g_locale = jni_new_object(jni_find_class("java/util/Locale"));
        if (g_locale)
            g_locale->permanent = 1;
    }
    return v_obj(g_locale);
}

IMPL(locale_toString) { (void)env; (void)self; (void)args; (void)m; return v_str(system_locale()); }

IMPL(locale_getLanguage)
{
    char lang[4];
    (void)env; (void)self; (void)args; (void)m;
    snprintf(lang, sizeof(lang), "%.2s", system_locale());
    return v_str(lang);
}

IMPL(locale_getCountry) { (void)env; (void)self; (void)args; (void)m; return v_str(system_locale() + 3); }

static const JMethodDef locale_methods[] = {
    { "getDefault", "()Ljava/util/Locale;", locale_getDefault },
    { "toString", "()Ljava/lang/String;", locale_toString },
    { "getLanguage", "()Ljava/lang/String;", locale_getLanguage },
    { "getCountry", "()Ljava/lang/String;", locale_getCountry },
    { NULL, NULL, NULL },
};
static const JClassDef class_Locale = { "java/util/Locale", NULL, NULL, locale_methods, NULL };

/* ================================================================ Defold === */

/* activity.getClassLoader().loadClass("com.defold.iap.IapJNI"): every Defold
 * extension finds its Java half this way. Unmodelled classes come back as
 * placeholders whose methods return defaults, which is what "no ads, no store,
 * no Play services" should look like. */
IMPL(cl_loadClass)
{
    char slash[160];
    size_t i;
    (void)env; (void)self; (void)m;
    snprintf(slash, sizeof(slash), "%s", arg_str(args, 0));
    for (i = 0; slash[i]; i++)
        if (slash[i] == '.')
            slash[i] = '/';
    return v_obj((jobject)jni_find_class(slash));
}

static const JMethodDef classloader_methods[] = {
    { "loadClass", NULL, cl_loadClass },
    { "findClass", NULL, cl_loadClass },
    { NULL, NULL, NULL },
};
static const JClassDef class_ClassLoader = { "java/lang/ClassLoader", NULL, NULL, classloader_methods, NULL };

static jobject singleton(jobject *slot, const char *cls)
{
    if (!*slot) {
        *slot = jni_new_object(jni_find_class(cls));
        if (*slot)
            (*slot)->permanent = 1;
    }
    return *slot;
}

static jobject g_loader, g_appinfo, g_wm, g_display, g_intent, g_resolver;

IMPL(act_getClassLoader)     { (void)env; (void)self; (void)args; (void)m; return v_obj(singleton(&g_loader, "java/lang/ClassLoader")); }
IMPL(act_getApplicationInfo) { (void)env; (void)self; (void)args; (void)m; return v_obj(singleton(&g_appinfo, "android/content/pm/ApplicationInfo")); }
IMPL(act_getWindowManager)   { (void)env; (void)self; (void)args; (void)m; return v_obj(singleton(&g_wm, "android/view/WindowManager")); }
IMPL(act_getIntent)          { (void)env; (void)self; (void)args; (void)m; return v_obj(singleton(&g_intent, "android/content/Intent")); }
IMPL(act_getContentResolver) { (void)env; (void)self; (void)args; (void)m; return v_obj(singleton(&g_resolver, "android/content/ContentResolver")); }
IMPL(wm_getDefaultDisplay)   { (void)env; (void)self; (void)args; (void)m; return v_obj(singleton(&g_display, "android/view/Display")); }
IMPL(disp_refreshRate)       { (void)env; (void)self; (void)args; (void)m; return v_float(60.0f); }

static const JMethodDef defoldactivity_methods[] = {
    { "getClassLoader", NULL, act_getClassLoader },
    { "getApplicationInfo", NULL, act_getApplicationInfo },
    { "getWindowManager", NULL, act_getWindowManager },
    { "getIntent", NULL, act_getIntent },
    { "getContentResolver", NULL, act_getContentResolver },
    { NULL, NULL, NULL },
};
static const JClassDef class_NativeActivity =
    { "android/app/NativeActivity", "android/app/Activity", NULL, defoldactivity_methods, NULL };
static const JClassDef class_DefoldActivity =
    { "com/dynamo/android/DefoldActivity", "android/app/NativeActivity", NULL, NULL, NULL };

GETTER(ai_dataDir)   { (void)env; (void)self; (void)f; return v_str(paths_save_nodev()); }
GETTER(ai_sourceDir) { (void)env; (void)self; (void)f; return v_str(paths_assets_nodev()); }

static const JFieldDef appinfo_fields[] = {
    { "dataDir", NULL, ai_dataDir },
    { "sourceDir", NULL, ai_sourceDir },
    { "publicSourceDir", NULL, ai_sourceDir },
    { "nativeLibraryDir", NULL, ai_sourceDir },
    { NULL, NULL, NULL },
};
static const JClassDef class_ApplicationInfo =
    { "android/content/pm/ApplicationInfo", NULL, NULL, NULL, appinfo_fields };

static const JMethodDef wm_methods[] = {
    { "getDefaultDisplay", NULL, wm_getDefaultDisplay },
    { NULL, NULL, NULL },
};
static const JClassDef class_WindowManager = { "android/view/WindowManager", NULL, NULL, wm_methods, NULL };

static const JMethodDef display_methods[] = {
    { "getRefreshRate", "()F", disp_refreshRate },
    { NULL, NULL, NULL },
};
static const JClassDef class_Display = { "android/view/Display", NULL, NULL, display_methods, NULL };

/* ---- Defold extensions that answer asynchronously ----
 *
 * The game's start-up chain (_services/platform.lua) waits, step by step, for
 * the Java half of three SDKs to call back into native code:
 *
 *   Play Games   GpgsJNI.silentLogin -> gpgsAddToQueue(MSG_SILENT_SIGN_IN, json)
 *   AppLovin     MaxDefoldPlugin.initialize -> appLovinAddToQueue(
 *                "OnSdkInitializedEvent", json)
 *   Play Billing IapGooglePlay.listItems -> IapJNI.nativeOnProductsResult(...)
 *
 * With no Java those never come and the loading screen waits forever. So the
 * answers a phone without Play services / network would give are sent from
 * here: sign-in failed, ads SDK ready (it never loads an ad), no products.
 * They are queued and delivered from the main loop (jni_pump), as a Java
 * thread would, never from inside the call that asked. */

#define GPGS_MSG_SIGN_IN        1   /* values from the extension's LuaInit */
#define GPGS_MSG_SILENT_SIGN_IN 2
#define GPGS_STATUS_FAILED      2

typedef void (*GpgsQueueFn)(JNIEnvPtr, jclass, jint, jstring);
typedef void (*AppLovinQueueFn)(JNIEnvPtr, jclass, jstring, jstring);
typedef void (*IapProductsFn)(JNIEnvPtr, jobject, jint, jstring, jlong);

static GpgsQueueFn     g_gpgs_queue;
static AppLovinQueueFn g_applovin_queue;
static IapProductsFn   g_iap_products;
static int             g_applovin_ready;

void jni_bind_engine(so_module *m)
{
    g_gpgs_queue = (GpgsQueueFn)so_symbol(m, "Java_com_defold_gpgs_GpgsJNI_gpgsAddToQueue");
    g_applovin_queue = (AppLovinQueueFn)so_symbol(m, "Java_com_defold_applovin_MaxDefoldPlugin_appLovinAddToQueue");
    g_iap_products = (IapProductsFn)so_symbol(m, "Java_com_defold_iap_IapJNI_nativeOnProductsResult");
    LOGI("JNI: SDK callbacks: gpgs %s, applovin %s, iap %s", g_gpgs_queue ? "ok" : "missing",
         g_applovin_queue ? "ok" : "missing", g_iap_products ? "ok" : "missing");
}

enum { CB_GPGS, CB_APPLOVIN, CB_IAP_PRODUCTS };
typedef struct { int kind, code; char s1[64]; char s2[160]; jobject obj; jlong ptr; } PendingCb;

static PendingCb g_pending[16];
static int       g_npending;
static Mutex     g_pending_lock;

static void post_cb(int kind, int code, const char *s1, const char *s2, jobject obj, jlong ptr)
{
    mutexLock(&g_pending_lock);
    if (g_npending < (int)(sizeof(g_pending) / sizeof(g_pending[0]))) {
        PendingCb *p = &g_pending[g_npending++];
        p->kind = kind;
        p->code = code;
        snprintf(p->s1, sizeof(p->s1), "%s", s1 ? s1 : "");
        snprintf(p->s2, sizeof(p->s2), "%s", s2 ? s2 : "");
        p->obj = obj ? jni_ref(obj) : NULL;
        p->ptr = ptr;
    }
    mutexUnlock(&g_pending_lock);
}

void jni_pump(void)
{
    PendingCb run[16];
    int i, n;
    JNIEnvPtr env = jni_get_env();

    mutexLock(&g_pending_lock);
    n = g_npending;
    memcpy(run, g_pending, sizeof(run[0]) * (size_t)n);
    g_npending = 0;
    mutexUnlock(&g_pending_lock);

    for (i = 0; i < n; i++) {
        PendingCb *p = &run[i];
        switch (p->kind) {
        case CB_GPGS:
            LOGI("JNI: -> gpgs callback %d %s", p->code, p->s2);
            if (g_gpgs_queue)
                g_gpgs_queue(env, (jclass)jni_find_class("com/defold/gpgs/GpgsJNI"), p->code, jni_new_string(p->s2));
            break;
        case CB_APPLOVIN:
            LOGI("JNI: -> applovin callback %s %s", p->s1, p->s2);
            if (g_applovin_queue)
                g_applovin_queue(env, (jclass)jni_find_class("com/defold/applovin/MaxDefoldPlugin"),
                                 jni_new_string(p->s1), jni_new_string(p->s2));
            break;
        case CB_IAP_PRODUCTS:
            LOGI("JNI: -> iap products result %d %s", p->code, p->s2);
            if (g_iap_products)
                g_iap_products(env, p->obj, p->code, jni_new_string(p->s2), p->ptr);
            break;
        }
        if (p->obj)
            jni_unref(p->obj);
    }
}

IMPL(gpgs_silentLogin)
{
    (void)env; (void)self; (void)args; (void)m;
    post_cb(CB_GPGS, GPGS_MSG_SILENT_SIGN_IN, NULL,
            "{\"status\":2,\"error\":\"Google Play Games is not available on Nintendo Switch\"}", NULL, 0);
    return v_none();
}

IMPL(gpgs_login)
{
    (void)env; (void)self; (void)args; (void)m;
    post_cb(CB_GPGS, GPGS_MSG_SIGN_IN, NULL,
            "{\"status\":2,\"error\":\"Google Play Games is not available on Nintendo Switch\"}", NULL, 0);
    return v_none();
}

static const JMethodDef gpgs_methods[] = {
    { "silentLogin", NULL, gpgs_silentLogin },
    { "login", NULL, gpgs_login },
    { NULL, NULL, NULL },
};
static const JClassDef class_GpgsJNI = { "com/defold/gpgs/GpgsJNI", NULL, NULL, gpgs_methods, NULL };

IMPL(max_initialize)
{
    (void)env; (void)self; (void)args; (void)m;
    g_applovin_ready = 1;
    post_cb(CB_APPLOVIN, 0, "OnSdkInitializedEvent", "{\"countryCode\":\"US\"}", NULL, 0);
    return v_none();
}

IMPL(max_isInitialized) { (void)env; (void)self; (void)args; (void)m; return v_bool(g_applovin_ready); }

static const JMethodDef max_methods[] = {
    { "initialize", NULL, max_initialize },
    { "isInitialized", NULL, max_isInitialized },
    { NULL, NULL, NULL },
};
static const JClassDef class_MaxDefoldPlugin = { "com/defold/applovin/MaxDefoldPlugin", NULL, NULL, max_methods, NULL };

/* listItems(String ids, IListProductsListener listener, long commandPtr) */
IMPL(iap_listItems)
{
    (void)env; (void)self;
    if (args && m->nargs >= 3)
        post_cb(CB_IAP_PRODUCTS, 0, NULL, "{}", args[1].l, args[2].j);   /* OK, no products */
    return v_none();
}

static const JMethodDef iap_methods[] = {
    { "listItems", NULL, iap_listItems },
    { NULL, NULL, NULL },
};
static const JClassDef class_IapGooglePlay = { "com/defold/iap/IapGooglePlay", NULL, NULL, iap_methods, NULL };

/* dmDeviceOpenSL::GetSampleRate asks Java for the output rate. audout is
 * 48 kHz, so answering that makes the mix need no resampling at all. */
IMPL(sound_getSampleRate)     { (void)env; (void)self; (void)args; (void)m; return v_int(48000); }
IMPL(sound_getFramesPerBuffer){ (void)env; (void)self; (void)args; (void)m; return v_int(1024); }

static const JMethodDef sound_methods[] = {
    { "getSampleRate", NULL, sound_getSampleRate },
    { "getFramesPerBuffer", NULL, sound_getFramesPerBuffer },
    { NULL, NULL, NULL },
};
static const JClassDef class_DefoldSound = { "com/defold/sound/Sound", NULL, NULL, sound_methods, NULL };

IMPL(secure_getString) { (void)env; (void)self; (void)args; (void)m; return v_str("0123456789abcdef"); }

static const JMethodDef secure_methods[] = {
    { "getString", NULL, secure_getString },
    { NULL, NULL, NULL },
};
static const JClassDef class_SettingsSecure = { "android/provider/Settings$Secure", NULL, NULL, secure_methods, NULL };

/* ============================================================ registry ==== */

const JClassDef *const jni_class_defs[] = {
    &class_Object, &class_Class, &class_String, &class_CharSequence, &class_Number,
    &class_Boolean, &class_Character, &class_Byte, &class_Short, &class_Integer, &class_Long,
    &class_Float, &class_Double, &class_Throwable, &class_Exception, &class_RuntimeException,
    &class_File, &class_Context, &class_Activity,
    &class_Build, &class_BuildVersion, &class_Environment, &class_Locale,
    &class_ClassLoader, &class_NativeActivity, &class_DefoldActivity, &class_ApplicationInfo,
    &class_WindowManager, &class_Display, &class_SettingsSecure, &class_DefoldSound,
    &class_GpgsJNI, &class_MaxDefoldPlugin, &class_IapGooglePlay,
};
const int jni_class_def_count = (int)(sizeof(jni_class_defs) / sizeof(jni_class_defs[0]));
