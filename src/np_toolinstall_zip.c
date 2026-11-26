/* np_toolinstall_zip.c — T4 隔离安装：T3 已校验 zip → 隔离 tool_dir。
 * 独立文件：np_toolinstall.c 被多个单测链接，不应引入 np_zip/CUI 依赖。 */
#include "np_toolinstall_zip.h"
#include "np_toolinstall.h"
#include "np_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#endif

#define NP_TIZ_DEF_MAX_BYTES  (256ULL * 1024 * 1024)
#define NP_TIZ_DEF_MAX_FILES  2048
#define NP_TIZ_PATH_CAP       1024

const char *np_ti_zip_status_name(np_ti_zip_status_t s) {
    switch (s) {
    case NP_TI_ZIP_OK:            return "ok";
    case NP_TI_ZIP_ERR_ARG:       return "invalid argument";
    case NP_TI_ZIP_ERR_UNPACK:    return "unpack rejected";
    case NP_TI_ZIP_ERR_BIN_MISSING: return "binary missing after unpack";
    case NP_TI_ZIP_ERR_IO:        return "io error";
    default:                      return "unknown";
    }
}

static bool mkdir_one(const char *dir) {
#ifdef _WIN32
    return _mkdir(dir) == 0 || errno == EEXIST;
#else
    return mkdir(dir, 0755) == 0 || errno == EEXIST;
#endif
}

static bool mkdir_p(char *path) {
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

/* 递归删除工具目录（仅限本模块构造出的隔离路径；重复安装清旧）。
 * 路径由 np_ti_plan_install 生成：<root>/<id>/<version>，均来自白名单常量。 */
static void rm_tree(const char *path) {
    char child[NP_TIZ_PATH_CAP];
#ifdef _WIN32
    struct _finddata_t fd;
    snprintf(child, sizeof(child), "%s/*", path);
    intptr_t h = _findfirst(child, &fd);
    if (h != -1) {
        do {
            if (!strcmp(fd.name, ".") || !strcmp(fd.name, "..")) continue;
            snprintf(child, sizeof(child), "%s/%s", path, fd.name);
            if (fd.attrib & _A_SUBDIR) rm_tree(child);
            else remove(child);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
    remove(path);
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            struct stat st;
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree(child);
            else remove(child);
        }
        closedir(d);
    }
    remove(path);
#endif
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* T4 安全：bin_rel 不能逃逸 tool_dir（拒绝绝对路径、盘符、.. 段）。
 * 与 np_zip_extract 的路径校验形成双保险。 */
static bool bin_rel_safe(const char *bin_rel) {
    if (!bin_rel || !*bin_rel) return false;
    if (bin_rel[0] == '/' || bin_rel[0] == '\\') return false;
    if (isalpha((unsigned char)bin_rel[0]) && bin_rel[1] == ':') return false;
    const char *p = bin_rel;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '\0' || p[2] == '/' || p[2] == '\\')) return false;
        p++;
    }
    return true;
}

np_ti_zip_status_t np_ti_install_package(const char *install_root,
        const np_tool_entry_t *e, const char *package_zip, const char *bin_rel,
        bool strip_root, uint64_t max_total_bytes, int max_files,
        np_ti_install_result_t *out, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!e || !package_zip || !out || !bin_rel_safe(bin_rel)) {
        if (err && err_cap)
            snprintf(err, err_cap, "参数缺失或不安全的 bin_rel");
        return NP_TI_ZIP_ERR_ARG;
    }
    if (!file_exists(package_zip)) {
        if (err && err_cap) snprintf(err, err_cap, "安装包不存在：%s", package_zip);
        return NP_TI_ZIP_ERR_IO;
    }
    if (!np_ti_plan_install(install_root, e, bin_rel, out))
        return NP_TI_ZIP_ERR_ARG;

    /* 重复安装：清掉旧隔离目录再解包（不碰系统 PATH / 注册表） */
    rm_tree(out->tool_dir);
    char tmp[NP_TIZ_PATH_CAP];
    snprintf(tmp, sizeof(tmp), "%s", out->tool_dir);
    if (!mkdir_p(tmp)) {
        if (err && err_cap) snprintf(err, err_cap, "无法创建工具目录：%s", tmp);
        return NP_TI_ZIP_ERR_IO;
    }

    uint64_t mb = max_total_bytes ? max_total_bytes : NP_TIZ_DEF_MAX_BYTES;
    int mf = max_files ? max_files : NP_TIZ_DEF_MAX_FILES;
    int n = np_zip_extract(package_zip, out->tool_dir, strip_root,
                           (size_t)mb, mf, NULL, 0, err, err_cap);
    if (n <= 0) return NP_TI_ZIP_ERR_UNPACK;

    if (!file_exists(out->bin_path)) {
        if (err && err_cap)
            snprintf(err, err_cap, "解包成功但二进制不存在：%s", out->bin_path);
        return NP_TI_ZIP_ERR_BIN_MISSING;
    }
    out->installed = true;
    return NP_TI_ZIP_OK;
}

np_ti_zip_status_t np_ti_fetch_and_install(const np_tool_entry_t *e,
        const char *bin_rel, bool strip_root, const char *install_root,
        bool (*downloader)(const char *url, const char *dest, void *user),
        void *user, np_ti_install_result_t *out, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!e || !out || !bin_rel_safe(bin_rel)) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失或不安全的 bin_rel");
        return NP_TI_ZIP_ERR_ARG;
    }

    /* T3: 计划 → 下载 → 校验（失败进隔离区） */
    np_ti_download_t plan;
    memset(&plan, 0, sizeof(plan));
    np_ti_dl_status_t ps = np_ti_plan_download(e, install_root, &plan);
    if (ps != NP_TI_DL_OK) {
        if (err && err_cap)
            snprintf(err, err_cap, "T3 计划失败：%s", np_ti_dl_status_name(ps));
        return NP_TI_ZIP_ERR_UNPACK;
    }
    char final[300];
    ps = np_ti_download_and_verify(&plan, install_root, downloader, user,
                                   final, sizeof(final));
    if (ps != NP_TI_DL_OK) {
        if (err && err_cap)
            snprintf(err, err_cap, "T3 下载/校验失败：%s", np_ti_dl_status_name(ps));
        return NP_TI_ZIP_ERR_UNPACK;
    }

    /* T4: 已校验包解包安装（内部清理旧目录 + bin_rel 校验） */
    return np_ti_install_package(install_root, e, final, bin_rel, strip_root,
                                 0, 0, out, err, err_cap);
}
