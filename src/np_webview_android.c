/* Android embedded WebView backend: drives WebViewBridge.java over JNI.
 * The Java side owns the WebView and all UI-thread work; this file only
 * forwards the np_webview.h API and reports readiness back on the SDL thread
 * (the CUI callback must not run on Android's UI thread). */
#include "np_webview.h"

#ifdef __ANDROID__

#include <SDL_system.h>
#include <jni.h>
#include <stdbool.h>
#include <string.h>

#define WB_CLASS "com/rapidaire/rapideditor/WebViewBridge"

static void (*wb_cb)(int);
static bool wb_ready;   /* set by nativeReady on the UI thread */
static bool wb_notified;

static JNIEnv *wb_env(void) { return (JNIEnv *)SDL_AndroidGetJNIEnv(); }

static jclass wb_class(JNIEnv *env) {
    jclass cls = (*env)->FindClass(env, WB_CLASS);
    if (!cls || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return cls;
}

static void wb_call_void(JNIEnv *env, const char *name, const char *sig) {
    jclass cls = wb_class(env);
    if (!cls) return;
    jmethodID m = (*env)->GetStaticMethodID(env, cls, name, sig);
    if (m && !(*env)->ExceptionCheck(env))
        (*env)->CallStaticVoidMethod(env, cls, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
}

int np_webview_init(void *native_window, void (*cb)(int event)) {
    (void)native_window;   /* the WebView attaches to SDL's Java root layout */
    wb_cb = cb;
    wb_ready = false;
    wb_notified = false;
    JNIEnv *env = wb_env();
    if (!env) return 0;
    wb_call_void(env, "init", "()V");
    return 1;
}

/* Deliver the ready transition on the SDL thread, not the Java UI thread. */
bool np_webview_ready(void) {
    if (wb_ready != wb_notified) {
        wb_notified = wb_ready;
        if (wb_cb) wb_cb(wb_ready ? 0 : 1);
    }
    return wb_ready;
}

void np_webview_set_bounds(int x, int y, int w, int h) {
    JNIEnv *env = wb_env();
    if (!env) return;
    jclass cls = wb_class(env);
    if (!cls) return;
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "setBounds", "(IIII)V");
    if (m && !(*env)->ExceptionCheck(env))
        (*env)->CallStaticVoidMethod(env, cls, m, (jint)x, (jint)y, (jint)w, (jint)h);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
}

void np_webview_set_visible(bool visible) {
    JNIEnv *env = wb_env();
    if (!env) return;
    jclass cls = wb_class(env);
    if (!cls) return;
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "setVisible", "(Z)V");
    if (m && !(*env)->ExceptionCheck(env))
        (*env)->CallStaticVoidMethod(env, cls, m, visible ? JNI_TRUE : JNI_FALSE);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
}

void np_webview_navigate(const char *url) {
    if (!url) return;
    JNIEnv *env = wb_env();
    if (!env) return;
    jclass cls = wb_class(env);
    if (!cls) return;
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "navigate", "(Ljava/lang/String;)V");
    if (m && !(*env)->ExceptionCheck(env)) {
        jstring s = (*env)->NewStringUTF(env, url);
        if (s) {
            (*env)->CallStaticVoidMethod(env, cls, m, s);
            (*env)->DeleteLocalRef(env, s);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
}

void np_webview_reload(void) {
    JNIEnv *env = wb_env();
    if (env) wb_call_void(env, "reload", "()V");
}

void np_webview_shutdown(void) {
    JNIEnv *env = wb_env();
    if (env) wb_call_void(env, "shutdown", "()V");
    wb_cb = NULL;
    wb_ready = false;
    wb_notified = false;
}

JNIEXPORT void JNICALL
Java_com_rapidaire_rapideditor_WebViewBridge_nativeReady(JNIEnv *env, jclass cls, jboolean ok) {
    (void)env; (void)cls;
    wb_ready = ok == JNI_TRUE;
}

#else /* !__ANDROID__ */

/* Keep the stub implementations for a non-Android build that still compiles
 * this file (the normal desktop build uses np_webview_stub.cpp instead). */
int np_webview_init(void *w, void (*cb)(int)) { (void)w; if (cb) cb(1); return 0; }
bool np_webview_ready(void) { return false; }
void np_webview_set_bounds(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
void np_webview_set_visible(bool v) { (void)v; }
void np_webview_navigate(const char *u) { (void)u; }
void np_webview_reload(void) {}
void np_webview_shutdown(void) {}

#endif
