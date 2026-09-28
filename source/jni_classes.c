/* jni_classes.c -- the Java classes the game touches, as method/field tables.
 *
 * Grouped as:
 *   java.lang      Object, Class, String, boxes, Throwable -- what liblime's
 *                  JNI bridge needs to convert values between Java and Haxe
 *   Lime           HaxeObject, Value, GameActivity, Extension
 *   Android        Activity/Context, Build, Locale, Environment, File
 *   SDKs           com.androidnative.Native (preferences, vibration) and four
 *                  services this port cannot offer: AdMob, Unity Ads, Google
 *                  Play Games, Google Play Billing. Those answer the way the
 *                  real SDKs do on a device with no network or no Play
 *                  services: "not available", "failed", no ads to show.
 *
 * Signatures are taken from the strings in libApplicationMain.so and the
 * decompiled classes.dex, so each entry matches what the game asks for.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "app.h"
#include "config.h"
#include "input.h"
#include "jni_env.h"
#include "log.h"
#include "paths.h"
#include "prefs.h"

#define IMPL(fn) static jvalue fn(JNIEnvPtr env, jobject self, const jvalue *args, JMethod *m)
#define GETTER(fn) static jvalue fn(JNIEnvPtr env, jobject self, JField *f)

static jvalue v_none(void)           { jvalue v; memset(&v, 0, sizeof(v)); return v; }
static jvalue v_bool(int b)          { jvalue v = v_none(); v.z = (jboolean)(b != 0); return v; }
static jvalue v_int(jint i)          { jvalue v = v_none(); v.i = i; return v; }
static jvalue v_double(jdouble d)    { jvalue v = v_none(); v.d = d; return v; }
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

/* Retain a callback object, releasing the previous one. */
static void keep(jobject *slot, jobject o)
{
    jobject old = *slot;
    *slot = jni_ref(o);
    if (old)
        jni_unref(old);
}

static void post0(jobject cb, const char *fn)
{
    if (cb)
        jni_post_haxe_call(cb, fn, 0, NULL);
}

static void post_str(jobject cb, const char *fn, const char *s)
{
    jobject a;
    if (!cb)
        return;
    a = jni_new_string(s);
    jni_post_haxe_call(cb, fn, 1, &a);
}

/* ============================================================ java.lang === */

static char box_code(const JClass *c)
{
    static const struct { const char *name; char code; } boxes[] = {
        { "java/lang/Boolean", 'Z' }, { "java/lang/Byte", 'B' },    { "java/lang/Character", 'C' },
        { "java/lang/Short", 'S' },   { "java/lang/Integer", 'I' }, { "java/lang/Long", 'J' },
        { "java/lang/Float", 'F' },   { "java/lang/Double", 'D' },  { "org/haxe/lime/Value", 'D' },
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
    { "getDouble", "()D", box_doubleValue },          /* org.haxe.lime.Value */
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
static const JClassDef class_Value     = { "org/haxe/lime/Value", NULL, NULL, box_methods, NULL };

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

/* ================================================================= Lime === */

IMPL(haxeobject_create)
{
    JClass *cls = jni_find_class("org/haxe/lime/HaxeObject");
    jobject o = jni_new_object(cls);
    (void)env; (void)self; (void)m;
    if (o && args) {
        jvalue v = v_none();
        v.j = args[0].j;
        jni_set_field(o, jni_field(cls, "__haxeHandle", "J", 0), v);
    }
    return v_obj(o);
}

static const JMethodDef haxeobject_methods[] = {
    { "create", "(J)Lorg/haxe/lime/HaxeObject;", haxeobject_create },
    { NULL, NULL, NULL },
};
static const JClassDef class_HaxeObject = { "org/haxe/lime/HaxeObject", NULL, NULL, haxeobject_methods, NULL };
static const JClassDef class_Lime = { "org/haxe/lime/Lime", NULL, NULL, NULL, NULL };

IMPL(ga_postUICallback)
{
    (void)env; (void)self; (void)m;
    if (args)
        jni_post_ui_callback(args[0].j);
    return v_none();
}

IMPL(ga_getDisplayXDPI) { (void)env; (void)self; (void)args; (void)m; return v_double((jdouble)g_cfg.dpi); }
IMPL(ga_zero)           { (void)env; (void)self; (void)args; (void)m; return v_int(0); }

IMPL(ga_openURL)
{
    (void)env; (void)self; (void)m;
    LOGI("GameActivity.openURL(%s) -- no browser on this port", arg_str(args, 0));
    return v_none();
}

IMPL(ga_openFile)
{
    (void)env; (void)self; (void)m;
    LOGI("GameActivity.openFile(%s) ignored", arg_str(args, 0));
    return v_none();
}

IMPL(ga_vibrate)
{
    (void)env; (void)self;
    if (args && m->nargs >= 2)
        input_vibrate(args[1].i);           /* (period, duration) */
    else if (args && m->nargs == 1)
        input_vibrate(args[0].i);
    return v_none();
}

IMPL(ga_getContext) { (void)env; (void)self; (void)args; (void)m; return v_obj(jni_activity()); }

static const JMethodDef gameactivity_methods[] = {
    { "postUICallback", "(J)V", ga_postUICallback },
    { "getDisplayXDPI", "()D", ga_getDisplayXDPI },
    { "getSafeInsetLeft", "()I", ga_zero },
    { "getSafeInsetTop", "()I", ga_zero },
    { "getSafeInsetRight", "()I", ga_zero },
    { "getSafeInsetBottom", "()I", ga_zero },
    { "openURL", NULL, ga_openURL },
    { "openFile", NULL, ga_openFile },
    { "vibrate", NULL, ga_vibrate },
    { "getContext", NULL, ga_getContext },
    { NULL, NULL, NULL },
};
static const JClassDef class_GameActivity = { "org/haxe/lime/GameActivity", "org/libsdl/app/SDLActivity", NULL, gameactivity_methods, NULL };
static const JClassDef class_SDLActivity = { "org/libsdl/app/SDLActivity", "android/app/Activity", NULL, NULL, NULL };
static const JClassDef class_MainActivity = { "com/adventureislands/leveldevil/MainActivity", "org/haxe/lime/GameActivity", NULL, NULL, NULL };

static jobject g_view, g_handler, g_package;

GETTER(ext_activity) { (void)env; (void)self; (void)f; return v_obj(jni_activity()); }

GETTER(ext_view)
{
    (void)env; (void)self; (void)f;
    if (!g_view) {
        g_view = jni_new_object(jni_find_class("android/view/View"));
        if (g_view)
            g_view->permanent = 1;
    }
    return v_obj(g_view);
}

GETTER(ext_handler)
{
    (void)env; (void)self; (void)f;
    if (!g_handler) {
        g_handler = jni_new_object(jni_find_class("android/os/Handler"));
        if (g_handler)
            g_handler->permanent = 1;
    }
    return v_obj(g_handler);
}

static jobject package_name(void)
{
    if (!g_package)
        g_package = jni_permanent_string(HS_PACKAGE);
    return g_package;
}

GETTER(ext_package) { (void)env; (void)self; (void)f; return v_obj(package_name()); }

static const JFieldDef extension_fields[] = {
    { "mainActivity", NULL, ext_activity },
    { "mainContext", NULL, ext_activity },
    { "mainView", NULL, ext_view },
    { "callbackHandler", NULL, ext_handler },
    { "packageName", NULL, ext_package },
    { NULL, NULL, NULL },
};
static const JClassDef class_Extension = { "org/haxe/extension/Extension", NULL, NULL, NULL, extension_fields };

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
    hs_request_exit(0);
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

/* ============================================================== the SDKs === */

/* ---- com.androidnative.Native: preferences, vibration, alerts ---- */

static jobject g_native_cb;

IMPL(native_initialize) { (void)env; (void)self; (void)m; if (args) keep(&g_native_cb, args[0].l); return v_none(); }
IMPL(native_vibrate)    { (void)env; (void)self; (void)m; if (args) input_vibrate(args[0].i); return v_none(); }
IMPL(native_nothing)    { (void)env; (void)self; (void)args; (void)m; return v_none(); }

IMPL(native_showAlert)
{
    (void)env; (void)self; (void)m;
    LOGI("alert: %s: %s", arg_str(args, 0), arg_str(args, 1));
    return v_none();
}

IMPL(native_getPref)
{
    char *v = prefs_get(arg_str(args, 0));
    jvalue r = v_str(v ? v : "");
    (void)env; (void)self; (void)m;
    free(v);
    return r;
}

IMPL(native_setPref)
{
    (void)env; (void)self; (void)m;
    prefs_set(arg_str(args, 0), arg_str(args, 1));
    return v_none();
}

IMPL(native_clearPref)
{
    (void)env; (void)self; (void)m;
    prefs_set(arg_str(args, 0), "");        /* Android version stores "" */
    return v_none();
}

/* Launched from a home screen icon, never from a deep link. */
IMPL(native_no_extras)
{
    (void)env; (void)self; (void)args; (void)m;
    return v_obj(jni_new_array('L', 0, "java/lang/String"));
}

static const JMethodDef native_methods[] = {
    { "initialize", "(Lorg/haxe/lime/HaxeObject;)V", native_initialize },
    { "vibrate", "(I)V", native_vibrate },
    { "setText", "(Ljava/lang/String;)V", native_nothing },
    { "showKeyboard", "()V", native_nothing },
    { "hideKeyboard", "()V", native_nothing },
    { "showAlert", "(Ljava/lang/String;Ljava/lang/String;)V", native_showAlert },
    { "getUserPreference", "(Ljava/lang/String;)Ljava/lang/String;", native_getPref },
    { "setUserPreference", "(Ljava/lang/String;Ljava/lang/String;)V", native_setPref },
    { "clearUserPreference", "(Ljava/lang/String;)V", native_clearPref },
    { "getIntentExtraStrings", "()[Ljava/lang/String;", native_no_extras },
    { NULL, NULL, NULL },
};
static const JClassDef class_Native = { "com/androidnative/Native", NULL, NULL, native_methods, NULL };

/* ---- com.byrobin.admobex.AdMobEx: no ads load, so none ever show ---- */

static jobject g_admob_cb;

IMPL(admob_init)
{
    (void)env; (void)self; (void)m;
    if (args)
        keep(&g_admob_cb, args[0].l);
    LOGI("AdMob: not available on this port");
    return v_none();
}

IMPL(admob_loadInterstitial) { (void)env; (void)self; (void)args; (void)m; post0(g_admob_cb, "onAdmobInterstitialFailed"); return v_none(); }
IMPL(admob_showBanner)       { (void)env; (void)self; (void)args; (void)m; post0(g_admob_cb, "onAdmobBannerFailed"); return v_none(); }
IMPL(sdk_nothing)            { (void)env; (void)self; (void)args; (void)m; return v_none(); }
IMPL(sdk_false)              { (void)env; (void)self; (void)args; (void)m; return v_bool(0); }

static const JMethodDef admob_methods[] = {
    { "init", NULL, admob_init },
    { "loadInterstitial", "()V", admob_loadInterstitial },
    { "showInterstitial", "()V", sdk_nothing },     /* real SDK: no-op unless loaded */
    { "showBanner", "()V", admob_showBanner },
    { "hideBanner", "()V", sdk_nothing },
    { "onResize", "()V", sdk_nothing },
    { "setBannerPosition", NULL, sdk_nothing },
    { "setPrivacyURL", NULL, sdk_nothing },
    { "showConsentForm", NULL, sdk_nothing },
    { NULL, NULL, NULL },
};
static const JClassDef class_AdMobEx = { "com/byrobin/admobex/AdMobEx", NULL, NULL, admob_methods, NULL };

/* ---- com.byrobin.unityads.UnityAdsEx ---- */

static jobject g_unity_cb;

IMPL(unity_init)
{
    (void)env; (void)self; (void)m;
    if (args)
        keep(&g_unity_cb, args[0].l);
    LOGI("Unity Ads: not available on this port");
    return v_none();
}

IMPL(unity_show) { (void)env; (void)self; (void)args; (void)m; post0(g_unity_cb, "onAdFailedToFetch"); return v_none(); }
IMPL(unity_load) { (void)env; (void)self; (void)args; (void)m; post0(g_unity_cb, "onAdFailedToFetch"); return v_none(); }

IMPL(unity_setConsent)
{
    (void)env; (void)self; (void)m;
    prefs_set("__hs_unityads_consent", args && args[0].z ? "1" : "0");
    return v_none();
}

IMPL(unity_getConsent)
{
    char *v = prefs_get("__hs_unityads_consent");
    int r = v && v[0] == '1';
    (void)env; (void)self; (void)args; (void)m;
    free(v);
    return v_bool(r);
}

static const JMethodDef unity_methods[] = {
    { "init", NULL, unity_init },
    /* This build loads an ad before showing one; report the load failing so
     * nothing ever reaches showVideo in the first place. */
    { "loadVideo", NULL, unity_load },
    { "loadRewarded", NULL, unity_load },
    { "canShowUnityAds", NULL, sdk_false },
    { "isSupportedUnityAds", NULL, sdk_false },
    { "showVideo", NULL, unity_show },
    { "showRewarded", NULL, unity_show },
    { "showBanner", NULL, sdk_nothing },
    { "hideBanner", NULL, sdk_nothing },
    { "moveBanner", NULL, sdk_nothing },
    { "destroyBanner", NULL, sdk_nothing },
    { "setUsersConsent", "(Z)V", unity_setConsent },
    { "getUsersConsent", "()Z", unity_getConsent },
    { NULL, NULL, NULL },
};
static const JClassDef class_UnityAdsEx = { "com/byrobin/unityads/UnityAdsEx", NULL, NULL, unity_methods, NULL };

/* ---- com.stencyl.GoogleServices.GooglePlayGames: never signed in ---- */

static jobject g_gpg_cb;

IMPL(gpg_init)
{
    (void)env; (void)self; (void)m;
    if (args)
        keep(&g_gpg_cb, args[0].l);
    LOGI("Google Play Games: not available on this port");
    return v_none();
}

IMPL(gpg_questReward) { (void)env; (void)self; (void)args; (void)m; return v_str(""); }

IMPL(gpg_questList)
{
    (void)env; (void)self; (void)args; (void)m;
    return v_obj(jni_new_array('L', 0, "java/lang/String"));
}

static const JMethodDef gpg_methods[] = {
    { "initGooglePlayGames", NULL, gpg_init },
    { "signOutGooglePlayGames", NULL, sdk_nothing },
    { "isSignedIn", NULL, sdk_false },
    { "isConnecting", NULL, sdk_false },
    { "hasSignInError", NULL, sdk_false },
    { "hasUserCancellation", NULL, sdk_false },
    { "showAchievements", NULL, sdk_nothing },
    { "unlockAchievement", NULL, sdk_nothing },
    { "incrementAchievement", NULL, sdk_nothing },
    { "unlockAchievementImmediate", NULL, sdk_nothing },
    { "incrementAchievementImmediate", NULL, sdk_nothing },
    { "showAllLeaderboards", NULL, sdk_nothing },
    { "showLeaderboard", NULL, sdk_nothing },
    { "submitScore", NULL, sdk_nothing },
    { "showQuests", NULL, sdk_nothing },
    { "updateEvent", NULL, sdk_nothing },
    { "hasNewQuestCompleted", NULL, sdk_false },
    { "getQuestReward", NULL, gpg_questReward },
    { "getCompletedQuestList", NULL, gpg_questList },
    { NULL, NULL, NULL },
};
static const JClassDef class_GooglePlayGames = { "com/stencyl/GoogleServices/GooglePlayGames", NULL, NULL, gpg_methods, NULL };

/* ---- com.stencyl.android.AndroidBilling: the store never starts ---- */

static jobject g_billing_cb;

IMPL(billing_initialize)
{
    (void)env; (void)self; (void)m;
    if (args && m->nargs >= 2)
        keep(&g_billing_cb, args[1].l);
    LOGI("Billing: not available on this port, reporting onStarted(Failure)");
    post_str(g_billing_cb, "onStarted", "Failure");
    return v_none();
}

IMPL(billing_buy)
{
    (void)env; (void)self; (void)m;
    LOGI("Billing: purchase of '%s' refused", arg_str(args, 0));
    post_str(g_billing_cb, "onFailedPurchase", arg_str(args, 0));
    return v_none();
}

static const JMethodDef billing_methods[] = {
    { "initialize", NULL, billing_initialize },
    { "buy", NULL, billing_buy },
    { "consume", NULL, sdk_nothing },
    { "acknowledge", NULL, sdk_nothing },
    { "restore", NULL, sdk_nothing },
    { "purchaseInfo", NULL, sdk_nothing },
    { "release", NULL, sdk_nothing },
    { NULL, NULL, NULL },
};
static const JClassDef class_AndroidBilling = { "com/stencyl/android/AndroidBilling", NULL, NULL, billing_methods, NULL };

/* ============================================================ registry ==== */

const JClassDef *const jni_class_defs[] = {
    &class_Object, &class_Class, &class_String, &class_CharSequence, &class_Number,
    &class_Boolean, &class_Character, &class_Byte, &class_Short, &class_Integer, &class_Long,
    &class_Float, &class_Double, &class_Throwable, &class_Exception, &class_RuntimeException,
    &class_HaxeObject, &class_Value, &class_Lime, &class_GameActivity, &class_SDLActivity,
    &class_MainActivity, &class_Extension, &class_File, &class_Context, &class_Activity,
    &class_Build, &class_BuildVersion, &class_Environment, &class_Locale,
    &class_Native, &class_AdMobEx, &class_UnityAdsEx, &class_GooglePlayGames, &class_AndroidBilling,
};
const int jni_class_def_count = (int)(sizeof(jni_class_defs) / sizeof(jni_class_defs[0]));
