/* np_toolinstall.c — S1: T3 下载校验 / T4 隔离安装 / T5 健康检查 / T7 降级链. */
#include "np_toolinstall.h"
#include "np_integrity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* ------------------------------------------------------------------ */
/* T3 默认下载器（主程序注册，单测不注册 → 保持无网络依赖）             */
/* ------------------------------------------------------------------ */

static bool (*g_default_downloader)(const char *, const char *, void *) = NULL;

void np_ti_set_default_downloader(
        bool (*downloader)(const char *url, const char *dest, void *user)) {
    g_default_downloader = downloader;
}

/* ------------------------------------------------------------------ */
/* T4 安装根目录                                                        */
/* ------------------------------------------------------------------ */

static char g_install_root[260];

static void path_normalize_slashes(char *p) {
#ifdef _WIN32
    for (; *p; ++p) if (*p == '/') *p = '\\';
#endif
}

static bool mkdir_one(const char *dir) {
#ifdef _WIN32
    return _mkdir(dir) == 0 || errno == EEXIST;
#else
    return mkdir(dir, 0755) == 0 || errno == EEXIST;
#endif
}

static bool mkdir_p(char *path) {
    /* 逐级创建：每个分隔符前的前缀都尝试一次 mkdir，幂等 */
    size_t n = strlen(path);
    for (size_t i = 1; i < n; ++i) {
        if (path[i] == '/' || path[i] == '\\') {
            char saved = path[i];
            path[i] = '\0';
            mkdir_one(path);
            path[i] = saved;
        }
    }
    return mkdir_one(path);
}

const char *np_ti_install_root(const char *user_override) {
    if (user_override && *user_override) {
        snprintf(g_install_root, sizeof(g_install_root), "%s", user_override);
        return g_install_root;
    }
#ifdef _WIN32
    const char *lad = getenv("LOCALAPPDATA");
    if (lad && *lad)
        snprintf(g_install_root, sizeof(g_install_root), "%s\\RapidEditor\\tools", lad);
    else
        snprintf(g_install_root, sizeof(g_install_root), "RapidEditor\\tools");
#else
    const char *home = getenv("HOME");
    if (home && *home)
        snprintf(g_install_root, sizeof(g_install_root), "%s/.rapideditor/tools", home);
    else
        snprintf(g_install_root, sizeof(g_install_root), ".rapideditor/tools");
#endif
    path_normalize_slashes(g_install_root);
    return g_install_root;
}

/* ------------------------------------------------------------------ */
/* T3 下载计划与校验                                                    */
/* ------------------------------------------------------------------ */

const char *np_ti_dl_status_name(np_ti_dl_status_t s) {
    switch (s) {
    case NP_TI_DL_OK: return "ok";
    case NP_TI_DL_ERR_URL_NOT_ALLOWED: return "url-not-allowed";
    case NP_TI_DL_ERR_HASH_MISMATCH: return "hash-mismatch";
    case NP_TI_DL_ERR_NO_EXPECTED_HASH: return "no-expected-hash";
    case NP_TI_DL_ERR_NETWORK: return "network";
    case NP_TI_DL_ERR_QUARANTINED: return "quarantined";
    default: return "io";
    }
}

np_ti_dl_status_t np_ti_plan_download(const np_tool_entry_t *e,
                                      const char *install_root,
                                      np_ti_download_t *plan) {
    if (!e || !plan) return NP_TI_DL_ERR_IO;
    memset(plan, 0, sizeof(*plan));
    if (!np_whitelist_url_allowed(e->source_url))
        return NP_TI_DL_ERR_URL_NOT_ALLOWED;
    if (!e->sha256[0])
        return NP_TI_DL_ERR_NO_EXPECTED_HASH; /* 绝不下载无法验证的包 */

    snprintf(plan->url, sizeof(plan->url), "%s", e->source_url);
    /* 文件名取 URL 最后一段 */
    const char *fname = strrchr(e->source_url, '/');
    fname = fname ? fname + 1 : "package.bin";
#ifdef _WIN32
    snprintf(plan->dest_path, sizeof(plan->dest_path), "%s\\%s",
             np_ti_install_root(install_root), fname);
#else
    snprintf(plan->dest_path, sizeof(plan->dest_path), "%s/%s",
             np_ti_install_root(install_root), fname);
#endif
    snprintf(plan->expected_sha256, sizeof(plan->expected_sha256), "%s", e->sha256);
    plan->approx_size_kb = e->approx_size_kb;
    return NP_TI_DL_OK;
}

static bool sha256_file(const char *path, char *hex_out /* 65 bytes */) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    np_sha256_stream_t ctx;
    np_sha256_begin(&ctx);
    unsigned char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        np_sha256_stream_update(&ctx, buf, r);
    fclose(f);
    unsigned char digest[32];
    np_sha256_stream_final(&ctx, digest);
    np_sha256_hex(digest, hex_out);
    return true;
}

static bool move_to_quarantine(const char *file, const char *install_root,
                               char *out_path, size_t cap) {
    char qdir[300];
#ifdef _WIN32
    snprintf(qdir, sizeof(qdir), "%s\\quarantine", np_ti_install_root(install_root));
#else
    snprintf(qdir, sizeof(qdir), "%s/quarantine", np_ti_install_root(install_root));
#endif
    /* 逐级创建隔离目录 */
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", qdir);
    mkdir_p(tmp);

    const char *base = strrchr(file, '/');
    const char *b2 = strrchr(file, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : file;
#ifdef _WIN32
    snprintf(out_path, cap, "%s\\%s", qdir, base);
#else
    snprintf(out_path, cap, "%s/%s", qdir, base);
#endif
    remove(out_path); /* 覆盖旧隔离件 */
    return rename(file, out_path) == 0;
}

np_ti_dl_status_t np_ti_verify_package(const char *file,
                                       const char *expected_sha256,
                                       const char *install_root,
                                       char *quarantined_path, size_t cap) {
    if (!file || !expected_sha256 || !*expected_sha256)
        return NP_TI_DL_ERR_NO_EXPECTED_HASH;
    char got[65];
    if (!sha256_file(file, got)) return NP_TI_DL_ERR_IO;
    if (strcmp(got, expected_sha256) == 0) {
        if (quarantined_path && cap) quarantined_path[0] = '\0';
        return NP_TI_DL_OK;
    }
    /* 不匹配 → 隔离，绝不执行 */
    char qpath[300];
    if (quarantined_path && cap) qpath[0] = '\0';
    if (move_to_quarantine(file, install_root, qpath, sizeof(qpath))) {
        if (quarantined_path && cap)
            snprintf(quarantined_path, cap, "%s", qpath);
        return NP_TI_DL_ERR_HASH_MISMATCH;
    }
    return NP_TI_DL_ERR_HASH_MISMATCH;
}

np_ti_dl_status_t np_ti_download_and_verify(
        np_ti_download_t *plan, const char *install_root,
        bool (*downloader)(const char *url, const char *dest, void *user),
        void *user, char *final_path, size_t cap) {
    if (!plan) return NP_TI_DL_ERR_IO;
    if (!np_whitelist_url_allowed(plan->url))
        return NP_TI_DL_ERR_URL_NOT_ALLOWED;
    if (!plan->expected_sha256[0])
        return NP_TI_DL_ERR_NO_EXPECTED_HASH;

    /* 确保目标目录存在 */
    char dirbuf[300];
    snprintf(dirbuf, sizeof(dirbuf), "%s", plan->dest_path);
    char *last_sep = strrchr(dirbuf, '/');
    char *ls2 = strrchr(dirbuf, '\\');
    if (ls2 > last_sep) last_sep = ls2;
    if (last_sep) {
        *last_sep = '\0';
        if (!mkdir_p(dirbuf)) return NP_TI_DL_ERR_IO;
    }

    char qpath[300];
    for (int attempt = 1; attempt <= 2; ++attempt) {
        bool (*dl)(const char *, const char *, void *) =
            downloader ? downloader : g_default_downloader;
        bool ok = dl ? dl(plan->url, plan->dest_path, user) : false;
        if (!ok) return NP_TI_DL_ERR_NETWORK;
        np_ti_dl_status_t st = np_ti_verify_package(
            plan->dest_path, plan->expected_sha256, install_root,
            qpath, sizeof(qpath));
        if (st == NP_TI_DL_OK) {
            if (final_path && cap)
                snprintf(final_path, cap, "%s", plan->dest_path);
            return NP_TI_DL_OK;
        }
        if (st != NP_TI_DL_ERR_HASH_MISMATCH) return st;
        /* 校验失败：重试一次；再失败留在隔离区并上报 */
        plan->attempts = attempt;
        if (attempt == 2) {
            if (final_path && cap) final_path[0] = '\0';
            return NP_TI_DL_ERR_QUARANTINED;
        }
    }
    return NP_TI_DL_ERR_NETWORK;
}

/* ------------------------------------------------------------------ */
/* T4 隔离安装规划                                                      */
/* ------------------------------------------------------------------ */

bool np_ti_plan_install(const char *install_root, const np_tool_entry_t *e,
                        const char *bin_rel, np_ti_install_result_t *out) {
    if (!e || !out) return false;
    memset(out, 0, sizeof(*out));
#ifdef _WIN32
    snprintf(out->tool_dir, sizeof(out->tool_dir), "%s\\%s\\%s",
             np_ti_install_root(install_root), e->id, e->version);
    snprintf(out->bin_path, sizeof(out->bin_path), "%s\\%s",
             out->tool_dir, bin_rel ? bin_rel : "");
    snprintf(out->env_patch, sizeof(out->env_patch), "%s", out->tool_dir);
    /* 若 bin_rel 含目录，PATH 前缀补上其父目录 */
    char *sep = strrchr(out->bin_path, '\\');
    if (sep && sep != out->bin_path) {
        *sep = '\0';
        snprintf(out->env_patch, sizeof(out->env_patch), "%s", out->bin_path);
        *sep = '\\';
    }
#else
    snprintf(out->tool_dir, sizeof(out->tool_dir), "%s/%s/%s",
             np_ti_install_root(install_root), e->id, e->version);
    snprintf(out->bin_path, sizeof(out->bin_path), "%s/%s",
             out->tool_dir, bin_rel ? bin_rel : "");
    snprintf(out->env_patch, sizeof(out->env_patch), "%s", out->tool_dir);
    char *sep = strrchr(out->bin_path, '/');
    if (sep && sep != out->bin_path) {
        *sep = '\0';
        snprintf(out->env_patch, sizeof(out->env_patch), "%s", out->bin_path);
        *sep = '/';
    }
#endif
    /* T4 红线：绝不改全局 PATH / 注册表——这里只产出进程级前缀，供
     * CreateProcess 时临时拼进子进程环境。 */
    out->installed = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* T5 健康检查                                                          */
/* ------------------------------------------------------------------ */

static bool read_first_line(FILE *f, char *out, size_t cap) {
    if (!fgets(out, (int)cap, f)) return false;
    size_t n = strlen(out);
    while (n && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    return true;
}

bool np_ti_health_check(const char *bin_path, const char *version_arg,
                        np_ti_health_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!bin_path || !*bin_path) {
        snprintf(out->error, sizeof(out->error), "%s", "empty bin path");
        return false;
    }
    char cmd[600];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", bin_path, version_arg ? version_arg : "");
    FILE *f = _popen(cmd, "r");
#else
    snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", bin_path, version_arg ? version_arg : "");
    FILE *f = popen(cmd, "r");
#endif
    if (!f) {
        snprintf(out->error, sizeof(out->error), "%s", "popen failed");
        return false;
    }
    char line[128];
    bool has_line = read_first_line(f, line, sizeof(line));
    int exit_code = -1;
#ifdef _WIN32
    exit_code = _pclose(f);
#else
    exit_code = pclose(f);
#endif
    if (!has_line) {
        snprintf(out->error, sizeof(out->error), "%s", "no output");
        return false;
    }
    snprintf(out->version, sizeof(out->version), "%.63s", line);
    /* "装上了≠能跑"：退出码非 0 视作不可用（如缺 JDK 的 ghidraRun） */
    out->runnable = (exit_code == 0);
    if (!out->runnable)
        snprintf(out->error, sizeof(out->error), "exit=%d: %.120s", exit_code, line);
    return out->runnable;
}

/* ------------------------------------------------------------------ */
/* T7 降级链                                                            */
/* ------------------------------------------------------------------ */

const char *np_ti_tier_name(np_ti_tier_t t) {
    switch (t) {
    case NP_TI_TIER_BUNDLED: return "bundled";
    case NP_TI_TIER_INSTALLED: return "installed";
    case NP_TI_TIER_SYSTEM: return "system";
    case NP_TI_TIER_OFFLINE_HINT: return "offline-hint";
    default: return "none";
    }
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* 在 PATH 串中查找可执行文件（T1 同款思路，这里只探测不修改）。 */
static bool find_in_path(const char *pathvar, const char *name,
                         char *out, size_t cap) {
    if (!pathvar) return false;
    const char *p = pathvar;
    while (*p) {
        const char *sep = strchr(p, ';');
        if (!sep) sep = p + strlen(p);
        size_t seg = (size_t)(sep - p);
        if (seg > 0 && seg + strlen(name) + 2 < cap) {
            char cand[300];
            memcpy(cand, p, seg);
            cand[seg] = '\0';
            size_t cl = strlen(cand);
            if (cl && (cand[cl-1] == '/' || cand[cl-1] == '\\'))
                snprintf(cand + cl, sizeof(cand) - cl, "%s", name);
            else
                snprintf(cand + cl, sizeof(cand) - cl,
#ifdef _WIN32
                         "\\%s", name);
#else
                         "/%s", name);
#endif
            if (file_exists(cand)) {
                snprintf(out, cap, "%s", cand);
                return true;
            }
        }
        if (!*sep) break;
        p = sep + 1;
    }
    return false;
}

bool np_ti_pick(const np_platform_t *plat, const char *purpose,
                np_arch_t need_arch, bool bundled_ok,
                const char *system_tool, np_ti_pick_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    (void)plat;

    /* 1) 内置能力（如 Capstone v6 静态链接） */
    if (bundled_ok) {
        out->tier = NP_TI_TIER_BUNDLED;
        snprintf(out->tool_id, sizeof(out->tool_id), "%s", "bundled-capstone");
        snprintf(out->reason, sizeof(out->reason),
                 "bundled engine covers %s for this arch", purpose ? purpose : "task");
        return true;
    }

    /* 2) T4 隔离安装目录里已装且健康检查通过的工具（purpose → 白名单 id 映射） */
    {
        const char *root = np_ti_install_root(NULL);
        /* 文档 T6：decompile→ghidra；disasm 优先内置 Capstone（走 bundled_ok） */
        const char *tool_id = (purpose && strcmp(purpose, "decompile") == 0)
                              ? "ghidra" : NULL;
        if (tool_id) {
            np_whitelist_t *wl = np_whitelist_create();
            if (np_whitelist_load_builtin(wl) > 0) {
                const np_tool_entry_t *e = np_whitelist_find(wl, tool_id);
                if (e) {
                    np_ti_install_result_t ins;
                    const char *bin_rel =
#ifdef _WIN32
                        "analyzeHeadless.bat";
#else
                        "analyzeHeadless";
#endif
                    if (np_ti_plan_install(root, e, bin_rel, &ins) &&
                        file_exists(ins.bin_path) &&
                        np_ti_health_check(ins.bin_path, "--version",
                                           &(np_ti_health_t){0})) {
                        out->tier = NP_TI_TIER_INSTALLED;
                        snprintf(out->tool_id, sizeof(out->tool_id), "%s", tool_id);
                        snprintf(out->bin_path, sizeof(out->bin_path), "%s", ins.bin_path);
                        snprintf(out->reason, sizeof(out->reason),
                                 "installed tool passed health check for %s",
                                 purpose ? purpose : "task");
                        np_whitelist_destroy(wl);
                        return true;
                    }
                }
            }
            np_whitelist_destroy(wl);
        }
    }

    /* 3) 系统 objdump */
    if (system_tool && *system_tool) {
        const char *pathvar = getenv("PATH");
        char found[300];
        if (find_in_path(pathvar, system_tool, found, sizeof(found))) {
            out->tier = NP_TI_TIER_SYSTEM;
            snprintf(out->tool_id, sizeof(out->tool_id), "%s", system_tool);
            snprintf(out->bin_path, sizeof(out->bin_path), "%s", found);
            snprintf(out->reason, sizeof(out->reason),
                     "system tool found on PATH for %s", purpose ? purpose : "task");
            return true;
        }
    }

    /* 4) 全部失败 → 离线提示 */
    out->tier = NP_TI_TIER_OFFLINE_HINT;
    snprintf(out->tool_id, sizeof(out->tool_id), "%s", "none");
    snprintf(out->reason, sizeof(out->reason),
             "no engine for %s (arch %d); suggest offline enterprise mirror",
             purpose ? purpose : "task", (int)need_arch);
    return false;
}
