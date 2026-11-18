/* In-app updater: JNI bridge on Android, native HTTP + shell on desktop.
 * See np_updater.h for the states. The C side starts operations, then polls
 * state each frame from the update dialog. */
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
const char *np_updater_asset_url(void)   { return upd.apk_url; }
const char *np_updater_asset_name(void)  { return "RapidEditor-update.apk"; }
const char *np_updater_check_error(void) { return upd.check_err; }
const char *np_updater_dest_path(void) {
    static char dest[1024];
    const char *base = SDL_AndroidGetInternalStoragePath();
    snprintf(dest, sizeof dest, "%s/update.apk", base ? base : "/data/local");
    return dest;
}

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

void np_updater_open_folder(void) {}  /* Android installs from app storage */

#elif !defined(__ANDROID__) /* ---------------- desktop update ------------ */

#include "np_net.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <process.h>
#include <direct.h>
#define UPD_MKDIR(p) _mkdir(p)
#define UPD_THREAD_FN static unsigned __stdcall
#define UPD_THREAD_RET return 0
#else
#include <pthread.h>
#include <unistd.h>
#define UPD_MKDIR(p) mkdir((p), 0700)
#define UPD_THREAD_FN static void *
#define UPD_THREAD_RET return NULL
#endif

#define UPD_API_URL "https://api.github.com/repos/RapidAI/RapidEditor/releases/latest"

static struct {
    volatile long check_state, dl_state, progress;
    char tag[64];
    char name[256];
    char url[1024];
    char dest[1024];
    char check_err[512];
    char dl_err[512];
} pc;

static bool pc_contains_ci(const char *s, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (!m || n < m) return false;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && tolower((unsigned char)s[i + k]) == tolower((unsigned char)needle[k])) k++;
        if (k == m) return true;
    }
    return false;
}

static bool pc_ends_ci(const char *s, size_t n, const char *suffix) {
    size_t m = strlen(suffix);
    return n >= m && pc_contains_ci(s + n - m, m, suffix);
}

/* Score an asset name for this OS/CPU. Highest score wins; 0 = unusable. */
static int pc_asset_score(const char *name, size_t n) {
#if defined(_WIN32)
    if (!pc_ends_ci(name, n, ".exe")) return 0;
    int score = 1;
    if (pc_contains_ci(name, n, "windows")) score += 2;
#if defined(_M_ARM64)
    if (pc_contains_ci(name, n, "arm64")) score += 8;
    if (pc_contains_ci(name, n, "universal")) score += 4;
#else
    if (pc_contains_ci(name, n, "x64")) score += 8;
    if (pc_contains_ci(name, n, "universal")) score += 4;
#endif
    return score;
#elif defined(__APPLE__)
    if (!pc_ends_ci(name, n, ".pkg")) return 0;
    return pc_contains_ci(name, n, "macos") ? 10 : 1;
#else
    if (!pc_ends_ci(name, n, ".appimage")) return 0;
#if defined(__aarch64__) || defined(_M_ARM64)
    const char *arch = "aarch64";
#else
    const char *arch = "x86_64";
#endif
    return pc_contains_ci(name, n, arch) ? 10 : 1;
#endif
}

/* Pull `"key":"value"` out of the release JSON (flat fields only). */
static void pc_json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    out[0] = '\0';
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = '\0';
}

/* Walk the assets array and pick the best installer for this platform. */
static bool pc_pick_asset(const char *json, char *name_out, size_t name_cap,
                          char *url_out, size_t url_cap) {
    const char *assets = strstr(json, "\"assets\":[");
    if (!assets) return false;
    const char *key = "\"browser_download_url\":\"";
    const char *p = assets;
    int best = 0;
    while ((p = strstr(p, "\"name\":\"")) != NULL) {
        p += 8;
        const char *ne = strchr(p, '"');
        if (!ne) break;
        size_t nl = (size_t)(ne - p);
        const char *u = strstr(ne, key);
        if (!u) break;
        u += strlen(key);
        const char *ue = strchr(u, '"');
        if (!ue) break;
        size_t ul = (size_t)(ue - u);
        int score = pc_asset_score(p, nl);
        if (score > best) {
            best = score;
            if (nl >= name_cap) nl = name_cap - 1;
            if (ul >= url_cap) ul = url_cap - 1;
            memcpy(name_out, p, nl); name_out[nl] = '\0';
            memcpy(url_out, u, ul);  url_out[ul] = '\0';
        }
        p = ue + 1;
    }
    return best > 0;
}

static void pc_set_dest(void) {
    char base[1024];
#if defined(_WIN32)
    base[0] = 0;
    GetTempPathA((DWORD)sizeof base, base);
    if (!base[0]) snprintf(base, sizeof base, ".");
#else
    const char *tmp = getenv("TMPDIR");
    snprintf(base, sizeof base, "%s", tmp && tmp[0] ? tmp : "/tmp");
#endif
    char dir[1024], dest[1024];
#if defined(_WIN32)
    size_t bl = strlen(base);
    if (bl && (base[bl - 1] == '\\' || base[bl - 1] == '/')) base[bl - 1] = 0;
    snprintf(dir, sizeof dir, "%s\\RapidEditor-Update", base);
#else
    snprintf(dir, sizeof dir, "%s/RapidEditor-Update", base);
#endif
    UPD_MKDIR(dir);  /* already existing is fine */
    /* Asset names come from GitHub; never let them escape the folder. */
    for (char *q = pc.name; *q; q++)
        if (*q == '/' || *q == '\\' || *q == ':') *q = '_';
#if defined(_WIN32)
    snprintf(dest, sizeof dest, "%s\\%s", dir, pc.name);
#else
    snprintf(dest, sizeof dest, "%s/%s", dir, pc.name);
#endif
    snprintf(pc.dest, sizeof pc.dest, "%s", dest);
}

UPD_THREAD_FN pc_check_worker(void *arg) {
    (void)arg;
    np_net_opts o;
    memset(&o, 0, sizeof o);
    o.timeout_sec = 20;
    o.err = pc.check_err;
    o.err_cap = sizeof pc.check_err;
    char *json = np_net_fetch(UPD_API_URL, &o);
    if (!json) {
        if (!pc.check_err[0])
            snprintf(pc.check_err, sizeof pc.check_err, "无法读取 GitHub Releases");
        pc.check_state = NP_UPD_FAILED;
        UPD_THREAD_RET;
    }
    pc_json_str(json, "tag_name", pc.tag, sizeof pc.tag);
    char name[256] = "", url[1024] = "";
    bool found = pc_pick_asset(json, name, sizeof name, url, sizeof url);
    free(json);
    if (!pc.tag[0] || !found) {
        snprintf(pc.check_err, sizeof pc.check_err,
                 !pc.tag[0] ? "Release 缺少版本信息"
                            : "该 Release 没有适用于当前系统的安装包");
        pc.tag[0] = 0;
        pc.check_state = NP_UPD_FAILED;
        UPD_THREAD_RET;
    }
    snprintf(pc.name, sizeof pc.name, "%s", name);
    snprintf(pc.url, sizeof pc.url, "%s", url);
    pc_set_dest();
    pc.check_state = NP_UPD_OK;
    UPD_THREAD_RET;
}

UPD_THREAD_FN pc_dl_worker(void *arg) {
    (void)arg;
    np_net_opts o;
    memset(&o, 0, sizeof o);
    o.progress = &pc.progress;
    o.timeout_sec = 600;
    o.err = pc.dl_err;
    o.err_cap = sizeof pc.dl_err;
    bool ok = np_net_download(pc.url, pc.dest, &o);
    if (ok) {
        struct stat st;
        if (stat(pc.dest, &st) != 0 || st.st_size == 0) {
            ok = false;
            snprintf(pc.dl_err, sizeof pc.dl_err, "下载文件为空");
        }
    }
    pc.progress = ok ? 100 : 0;
    pc.dl_state = ok ? NP_UPD_DL_DONE : NP_UPD_DL_FAILED;
    UPD_THREAD_RET;
}

static void pc_spawn_check(void) {
#if defined(_WIN32)
    if (!_beginthreadex(NULL, 0, pc_check_worker, NULL, 0, NULL)) {
        snprintf(pc.check_err, sizeof pc.check_err, "无法启动检查线程");
        pc.check_state = NP_UPD_FAILED;
    }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, pc_check_worker, NULL) != 0) {
        snprintf(pc.check_err, sizeof pc.check_err, "无法启动检查线程");
        pc.check_state = NP_UPD_FAILED;
    } else pthread_detach(t);
#endif
}

static void pc_spawn_download(void) {
#if defined(_WIN32)
    if (!_beginthreadex(NULL, 0, pc_dl_worker, NULL, 0, NULL)) {
        snprintf(pc.dl_err, sizeof pc.dl_err, "无法启动下载线程");
        pc.dl_state = NP_UPD_DL_FAILED;
    }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, pc_dl_worker, NULL) != 0) {
        snprintf(pc.dl_err, sizeof pc.dl_err, "无法启动下载线程");
        pc.dl_state = NP_UPD_DL_FAILED;
    } else pthread_detach(t);
#endif
}

void np_updater_start_check(void) {
    if (pc.check_state == NP_UPD_RUNNING) return;
    pc.check_state = NP_UPD_RUNNING;
    pc.dl_state = NP_UPD_DL_IDLE;
    pc.progress = 0;
    pc.tag[0] = pc.name[0] = pc.url[0] = pc.dest[0] = 0;
    pc.check_err[0] = pc.dl_err[0] = 0;
    pc_spawn_check();
}

int np_updater_check_state(void) { return (int)pc.check_state; }
const char *np_updater_latest_tag(void)  { return pc.tag; }
const char *np_updater_asset_url(void)   { return pc.url; }
const char *np_updater_asset_name(void)  { return pc.name; }
const char *np_updater_dest_path(void)   { return pc.dest; }
const char *np_updater_apk_url(void)     { return pc.url; }
const char *np_updater_check_error(void) { return pc.check_err; }

void np_updater_start_download(void) {
    if (!pc.url[0] || pc.dl_state == NP_UPD_DL_RUNNING) return;
    pc.dl_state = NP_UPD_DL_RUNNING;
    pc.progress = 0;
    pc.dl_err[0] = 0;
    pc_spawn_download();
}

int np_updater_dl_state(void)   { return (int)pc.dl_state; }
int np_updater_dl_progress(void) { return (int)pc.progress; }
const char *np_updater_dl_error(void) { return pc.dl_err; }

/* Quote a path for the shell used by open/xdg-open. */
static const char *pc_shell_path(const char *path) {
    static char q[2048];
    size_t n = 0;
    q[n++] = '\'';
    for (const char *p = path; *p && n + 5 < sizeof q; p++) {
        if (*p == '\'') { memcpy(q + n, "'\\''", 4); n += 4; }
        else q[n++] = *p;
    }
    q[n++] = '\'';
    q[n] = 0;
    return q;
}

void np_updater_install(void) {
    if (!pc.dest[0]) return;
    struct stat st;
    if (stat(pc.dest, &st) != 0) return;
#if defined(_WIN32)
    WCHAR w[2048];
    MultiByteToWideChar(CP_UTF8, 0, pc.dest, -1, w, 2048);
    if ((intptr_t)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) <= 32)
        snprintf(pc.dl_err, sizeof pc.dl_err, "无法启动安装包（可能被系统拦截）");
#elif defined(__APPLE__)
    char cmd[3072];
    snprintf(cmd, sizeof cmd, "open %s", pc_shell_path(pc.dest));
    if (system(cmd) != 0)
        snprintf(pc.dl_err, sizeof pc.dl_err, "无法打开安装包");
#else
    chmod(pc.dest, 0755);
    char cmd[3072];
    snprintf(cmd, sizeof cmd, "xdg-open %s", pc_shell_path(pc.dest));
    if (system(cmd) != 0)
        snprintf(pc.dl_err, sizeof pc.dl_err, "无法打开安装包，请在下载目录中手动运行");
#endif
}

void np_updater_open_folder(void) {
    if (!pc.dest[0]) return;
#if defined(_WIN32)
    char params_u8[2300];
    WCHAR params[2400];
    snprintf(params_u8, sizeof params_u8, "/select,\"%s\"", pc.dest);
    MultiByteToWideChar(CP_UTF8, 0, params_u8, -1, params, 2400);
    ShellExecuteW(NULL, L"open", L"explorer.exe", params, NULL, SW_SHOWNORMAL);
#else
    char cmd[3072];
#if defined(__APPLE__)
    snprintf(cmd, sizeof cmd, "open -R %s", pc_shell_path(pc.dest));
#else
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", pc.dest);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    snprintf(cmd, sizeof cmd, "xdg-open %s", pc_shell_path(dir));
#endif
    system(cmd);
#endif
}

#endif
