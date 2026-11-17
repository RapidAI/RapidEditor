/* Android in-app updater: thin JNI bridge over the Updater Java helper.
 * See np_updater.h for the states. Everything heavy (HTTPS, JSON hosting,
 * the package installer) lives on the Java side; this file only starts
 * operations, caches the parsed release metadata and exposes pollers. */
#include "np_updater.h"

#ifdef __ANDROID__

#include <SDL_system.h>
#include <jni.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPD_CLASS "com/rapidaire/rapideditor/Updater"

/* Parsed release metadata (filled once when the check lands OK). */
static struct {
    int  check_state;      /* last observed Java check state */
    int  dl_state;         /* last observed Java download state */
    char tag[64];          /* latest release tag, e.g. "v1.3.0" */
    char apk_url[1024];    /* browser_download_url of the first .apk asset */
    char check_err[512];
    char dl_err[512];
    bool parsed;           /* tag/apk_url extracted from the body */
} upd;

static JNIEnv *upd_env(void) {
    return (JNIEnv *)SDL_AndroidGetJNIEnv();
}

static jclass upd_class(JNIEnv *env) {
    jclass cls = (*env)->FindClass(env, UPD_CLASS);
    if (!cls || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return cls;
}

static jmethodID upd_static(JNIEnv *env, jclass cls, const char *name, const char *sig) {
    jmethodID m = (*env)->GetStaticMethodID(env, cls, name, sig);
    if (!m || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    return m;
}

static jint upd_int_method(JNIEnv *env, jclass cls, const char *name, jint fallback) {
    jmethodID m = upd_static(env, cls, name, "()I");
    return m ? (*env)->CallStaticIntMethod(env, cls, m) : fallback;
}

static jstring upd_str_method(JNIEnv *env, jclass cls, const char *name, const char *sig) {
    jmethodID m = upd_static(env, cls, name, sig);
    return m ? (jstring)(*env)->CallStaticObjectMethod(env, cls, m) : NULL;
}

static void upd_copy_str(JNIEnv *env, jstring js, char *out, int cap) {
    if (!js || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
    const char *s = (*env)->GetStringUTFChars(env, js, NULL);
    if (s) {
        snprintf(out, (size_t)cap, "%s", s);
        (*env)->ReleaseStringUTFChars(env, js, s);
    }
    (*env)->DeleteLocalRef(env, js);
}

/* Pull `"key":"value"` out of the release JSON (no full parser: GitHub
 * replies are flat for the fields we need; escapes are unrolled). */
static void upd_json_str(const char *json, const char *key, char *out, int cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    out[0] = '\0';
    if (!p) return;
    p += strlen(pat);
    int n = 0;
    while (*p && *p != '"' && n < cap - 1) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
}

/* Pick the first release asset whose download URL ends in .apk. */
static void upd_pick_apk(const char *json, char *out, int cap) {
    out[0] = '\0';
    const char *key = "\"browser_download_url\":\"";
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        p += strlen(key);
        const char *e = strchr(p, '"');
        if (!e) break;
        size_t n = (size_t)(e - p);
        if (n > 4 && strncmp(p + n - 4, ".apk", 4) == 0) {
            if (n >= (size_t)cap) n = (size_t)cap - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            return;
        }
        p = e;
    }
}

void np_updater_start_check(void) {
    JNIEnv *env = upd_env();
    if (!env) return;
    jclass cls = upd_class(env);
    if (!cls) return;
    jmethodID m = upd_static(env, cls, "checkAsync", "()V");
    if (m) (*env)->CallStaticVoidMethod(env, cls, m);
}

int np_updater_check_state(void) {
    JNIEnv *env = upd_env();
    if (!env) return NP_UPD_IDLE;
    jclass cls = upd_class(env);
    if (!cls) return NP_UPD_IDLE;
    int st = upd_int_method(env, cls, "getCheckState", NP_UPD_IDLE);
    if (st == NP_UPD_OK && !upd.parsed) {
        /* first frame after the request landed: fetch the body and parse */
        jstring body = upd_str_method(env, cls, "getBody", "()Ljava/lang/String;");
        char *json = NULL;
        if (body && !(*env)->ExceptionCheck(env)) {
            const char *s = (*env)->GetStringUTFChars(env, body, NULL);
            if (s) {
                json = strdup(s);
                (*env)->ReleaseStringUTFChars(env, body, s);
            }
        }
        (*env)->ExceptionClear(env);
        if (json) {
            upd_json_str(json, "tag_name", upd.tag, (int)sizeof upd.tag);
            upd_pick_apk(json, upd.apk_url, (int)sizeof upd.apk_url);
            free(json);
        }
        upd.parsed = true;
    }
    if (st != NP_UPD_OK) upd.parsed = false;
    upd.check_state = st;
    if (st == NP_UPD_FAILED) {
        jstring e = upd_str_method(env, cls, "getError", "()Ljava/lang/String;");
        upd_copy_str(env, e, upd.check_err, (int)sizeof upd.check_err);
    } else {
        upd.check_err[0] = '\0';
    }
    return upd.check_state;
}

const char *np_updater_latest_tag(void)  { return upd.tag; }
const char *np_updater_apk_url(void)     { return upd.apk_url; }
const char *np_updater_check_error(void) { return upd.check_err; }

void np_updater_start_download(void) {
    if (!upd.apk_url[0]) return;
    JNIEnv *env = upd_env();
    if (!env) return;
    jclass cls = upd_class(env);
    if (!cls) return;
    /* Destination: the app's internal files dir (no storage permission
     * needed; FileProvider shares it with the installer). */
    jmethodID m = upd_static(env, cls, "startDownload",
                             "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!m) return;
    const char *base = SDL_AndroidGetInternalStoragePath();
    char dest[1024];
    snprintf(dest, sizeof dest, "%s/update.apk", base ? base : "/data/local");
    jstring jurl = (*env)->NewStringUTF(env, upd.apk_url);
    jstring jdst = (*env)->NewStringUTF(env, dest);
    (*env)->CallStaticVoidMethod(env, cls, m, jurl, jdst);
    (*env)->DeleteLocalRef(env, jurl);
    (*env)->DeleteLocalRef(env, jdst);
}

int np_updater_dl_state(void) {
    JNIEnv *env = upd_env();
    if (!env) return NP_UPD_DL_IDLE;
    jclass cls = upd_class(env);
    if (!cls) return NP_UPD_DL_IDLE;
    upd.dl_state = upd_int_method(env, cls, "getDlState", NP_UPD_DL_IDLE);
    if (upd.dl_state == NP_UPD_DL_FAILED) {
        jstring e = upd_str_method(env, cls, "getError", "()Ljava/lang/String;");
        upd_copy_str(env, e, upd.dl_err, (int)sizeof upd.dl_err);
    }
    return upd.dl_state;
}

int np_updater_dl_progress(void) {
    JNIEnv *env = upd_env();
    if (!env) return 0;
    jclass cls = upd_class(env);
    return cls ? upd_int_method(env, cls, "getProgress", 0) : 0;
}

const char *np_updater_dl_error(void) { return upd.dl_err; }

void np_updater_install(void) {
    JNIEnv *env = upd_env();
    if (!env) return;
    jclass cls = upd_class(env);
    if (!cls) return;
    jmethodID m = upd_static(env, cls, "installApk", "(Ljava/lang/String;)V");
    if (!m) return;
    const char *base = SDL_AndroidGetInternalStoragePath();
    char dest[1024];
    snprintf(dest, sizeof dest, "%s/update.apk", base ? base : "/data/local");
    jstring jdst = (*env)->NewStringUTF(env, dest);
    (*env)->CallStaticVoidMethod(env, cls, m, jdst);
    (*env)->DeleteLocalRef(env, jdst);
}

#else /* !__ANDROID__: inert stubs so the editor builds unchanged */

void        np_updater_start_check(void) {}
int         np_updater_check_state(void) { return NP_UPD_IDLE; }
const char *np_updater_latest_tag(void)  { return ""; }
const char *np_updater_apk_url(void)     { return ""; }
const char *np_updater_check_error(void) { return ""; }
void        np_updater_start_download(void) {}
int         np_updater_dl_state(void)   { return NP_UPD_DL_IDLE; }
int         np_updater_dl_progress(void) { return 0; }
const char *np_updater_dl_error(void)   { return ""; }
void        np_updater_install(void) {}

#endif
