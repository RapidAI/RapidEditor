/* np_skillmgr.c — S1: X3 Skill/MCP 安装注册 + X4 手动管理. */
#include "np_skillmgr.h"
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

struct np_skill_store {
    char      store_dir[260];
    np_skill_t items[NP_SKILL_MAX];
    size_t    count;
};

/* ---------------- 小工具 ---------------- */

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

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static char *read_whole_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    if (len_out) *len_out = rd;
    return buf;
}

static bool sha256_path(const char *path, char hex_out[65]) {
    size_t n = 0;
    char *data = read_whole_file(path, &n);
    if (!data) return false;
    unsigned char dg[32];
    np_sha256(data, n, dg);
    np_sha256_hex(dg, hex_out);
    free(data);
    return true;
}

/* ---------------- manifest 解析 ---------------- */

/* 极简 front-matter/JSON 字段提取：
 * 支持 "key: value"（YAML）与 "key": "value"（JSON）两种写法。 */
static bool extract_field(const char *text, const char *key,
                          char *out, size_t cap) {
    char pat1[64], pat2[64];
    snprintf(pat1, sizeof(pat1), "%s:", key);          /* YAML */
    snprintf(pat2, sizeof(pat2), "\"%s\"", key);       /* JSON "key" */

    const char *p = strstr(text, pat1);
    if (p) {
        p += strlen(pat1);
        while (*p == ' ' || *p == '\t') p++;
        /* 去掉可能的引号 */
        if (*p == '"') p++;
        size_t i = 0;
        while (*p && *p != '\n' && *p != '\r' && *p != '"' && i + 1 < cap)
            out[i++] = *p++;
        out[i] = '\0';
        /* 去掉行尾逗号（JSON） */
        size_t n = strlen(out);
        if (n && out[n-1] == ',') out[n-1] = '\0';
        return i > 0;
    }
    p = strstr(text, pat2);
    if (p) {
        p += strlen(pat2);
        while (*p == ' ' || *p == ':') p++;
        if (*p == '"') p++;
        size_t i = 0;
        while (*p && *p != '"' && *p != '\n' && i + 1 < cap)
            out[i++] = *p++;
        out[i] = '\0';
        return i > 0;
    }
    return false;
}

np_skill_status_t np_skill_parse_manifest(const char *dir, np_skill_t *out) {
    if (!dir || !out) return NP_SKILL_ERR_IO;
    memset(out, 0, sizeof(*out));

    char path[320];
    char *text = NULL;
    /* 优先 SKILL.md，其次 manifest.json */
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\SKILL.md", dir);
#else
    snprintf(path, sizeof(path), "%s/SKILL.md", dir);
#endif
    text = read_whole_file(path, NULL);
    if (!text) {
#ifdef _WIN32
        snprintf(path, sizeof(path), "%s\\manifest.json", dir);
#else
        snprintf(path, sizeof(path), "%s/manifest.json", dir);
#endif
        text = read_whole_file(path, NULL);
    }
    if (!text) return NP_SKILL_ERR_NO_MANIFEST;

    bool ok = true;
    ok = extract_field(text, "name", out->name, sizeof(out->name)) && ok;
    ok = extract_field(text, "version", out->version, sizeof(out->version)) && ok;
    ok = extract_field(text, "description", out->description, sizeof(out->description)) && ok;
    extract_field(text, "entry", out->entry, sizeof(out->entry));
    extract_field(text, "sha256", out->sha256, sizeof(out->sha256));
    out->mcp = strstr(text, "\"mcp\"") != NULL || strstr(text, "mcp:") != NULL;
    free(text);
    if (!out->name[0] || !out->version[0]) return NP_SKILL_ERR_BAD_MANIFEST;
    (void)ok;
    return NP_SKILL_OK;
}

/* ---------------- store 持久化 ---------------- */

static void store_path(const np_skill_store_t *st, char *out, size_t cap) {
#ifdef _WIN32
    snprintf(out, cap, "%s\\installed.json", st->store_dir);
#else
    snprintf(out, cap, "%s/installed.json", st->store_dir);
#endif
}

/* 写盘：极简 JSON（一行一条） */
static bool store_save(const np_skill_store_t *st) {
    char path[320];
    store_path(st, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "[\n");
    for (size_t i = 0; i < st->count; ++i) {
        const np_skill_t *s = &st->items[i];
        fprintf(f, "  {\"name\":\"%s\",\"version\":\"%s\",\"entry\":\"%s\","
                   "\"source\":\"%s\",\"sha256\":\"%s\",\"mcp\":%s,"
                   "\"enabled\":%s}%s\n",
                s->name, s->version, s->entry, s->source_url, s->sha256,
                s->mcp ? "true" : "false",
                s->enabled ? "true" : "false",
                i + 1 < st->count ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
    return true;
}

/* 读盘：逐行找 name/version/sha256/enabled 字段重建（容错，不求完整 JSON） */
static bool store_load(np_skill_store_t *st) {
    char path[320];
    store_path(st, path, sizeof(path));
    char *text = read_whole_file(path, NULL);
    if (!text) return false;
    /* 按大括号分段提取 */
    const char *p = text;
    while (st->count < NP_SKILL_MAX && (p = strchr(p, '{')) != NULL) {
        const char *end = strchr(p, '}');
        if (!end) break;
        size_t len = (size_t)(end - p + 1);
        char *chunk = (char *)malloc(len + 1);
        if (!chunk) break;
        memcpy(chunk, p, len);
        chunk[len] = '\0';
        np_skill_t *s = &st->items[st->count];
        memset(s, 0, sizeof(*s));
        if (extract_field(chunk, "name", s->name, sizeof(s->name)) &&
            s->name[0]) {
            extract_field(chunk, "version", s->version, sizeof(s->version));
            extract_field(chunk, "entry", s->entry, sizeof(s->entry));
            extract_field(chunk, "source", s->source_url, sizeof(s->source_url));
            extract_field(chunk, "sha256", s->sha256, sizeof(s->sha256));
            s->mcp     = strstr(chunk, "\"mcp\":true") != NULL;
            s->enabled = strstr(chunk, "\"enabled\":true") != NULL;
            st->count++;
        }
        free(chunk);
        p = end + 1;
    }
    free(text);
    return true;
}

np_skill_store_t *np_skill_store_open(const char *store_dir) {
    np_skill_store_t *st = (np_skill_store_t *)calloc(1, sizeof(*st));
    if (!st) return NULL;
    if (store_dir && *store_dir) {
        snprintf(st->store_dir, sizeof(st->store_dir), "%s", store_dir);
    } else {
#ifdef _WIN32
        const char *lad = getenv("LOCALAPPDATA");
        snprintf(st->store_dir, sizeof(st->store_dir), "%s\\RapidEditor\\skills",
                 lad && *lad ? lad : "RapidEditor");
#else
        const char *home = getenv("HOME");
        snprintf(st->store_dir, sizeof(st->store_dir), "%s/.rapideditor/skills",
                 home && *home ? home : ".");
#endif
    }
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", st->store_dir);
    mkdir_p(tmp);
    store_load(st);
    return st;
}

void np_skill_store_close(np_skill_store_t *st) {
    free(st);
}

/* ---------------- X4 安装/启停/删除 ---------------- */

static bool copy_dir_recursive(const char *src, const char *dst);

#ifdef _WIN32
/* Windows: xcopy 递归拷贝（/E 含空目录 /I 目标为目录 /Y 覆盖） */
static bool copy_dir_recursive(const char *src, const char *dst) {
    if (!mkdir_one(dst)) return false;
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "xcopy \"%s\" \"%s\\\" /E /I /Y > NUL 2>&1", src, dst);
    return system(cmd) == 0;
}
#else
#include <dirent.h>
static bool copy_dir_recursive(const char *src, const char *dst) {
    if (!mkdir_one(dst)) return false;
    DIR *d = opendir(src);
    if (!d) return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char sp[320], dp[320];
        snprintf(sp, sizeof(sp), "%s/%s", src, ent->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, ent->d_name);
        /* 简化：全部按文件拷贝；子目录递归交给 stat 判定 */
        FILE *in = fopen(sp, "rb");
        if (in) {
            FILE *out = fopen(dp, "wb");
            if (!out) { fclose(in); ok = false; break; }
            char buf[8192];
            size_t r;
            while ((r = fread(buf, 1, sizeof(buf), in)) > 0)
                fwrite(buf, 1, r, out);
            fclose(in); fclose(out);
        } else {
            ok = copy_dir_recursive(sp, dp) && ok;
        }
    }
    closedir(d);
    return ok;
}
#endif

np_skill_status_t np_skill_install(np_skill_store_t *st, const char *dir,
                                   const char *source_url,
                                   const char *expected_sha256,
                                   np_skill_t *out_installed) {
    if (!st || !dir) return NP_SKILL_ERR_IO;
    np_skill_t info;
    np_skill_status_t ps = np_skill_parse_manifest(dir, &info);
    if (ps != NP_SKILL_OK) return ps;

    /* X3 红线：远程安装必须带期望哈希并校验 */
    char actual[65] = "";
    {
        char mfpath[320];
#ifdef _WIN32
        snprintf(mfpath, sizeof(mfpath), "%s\\SKILL.md", dir);
#else
        snprintf(mfpath, sizeof(mfpath), "%s/SKILL.md", dir);
#endif
        if (!file_exists(mfpath)) {
#ifdef _WIN32
            snprintf(mfpath, sizeof(mfpath), "%s\\manifest.json", dir);
#else
            snprintf(mfpath, sizeof(mfpath), "%s/manifest.json", dir);
#endif
        }
        sha256_path(mfpath, actual);
    }
    if (expected_sha256 && *expected_sha256) {
        if (!actual[0] || strcmp(actual, expected_sha256) != 0)
            return NP_SKILL_ERR_HASH_MISMATCH;
        snprintf(info.sha256, sizeof(info.sha256), "%s", actual);
    } else if (info.sha256[0]) {
        /* manifest 自声明哈希：校验 */
        if (!actual[0] || strcmp(actual, info.sha256) != 0)
            return NP_SKILL_ERR_HASH_MISMATCH;
    } else if (source_url && *source_url) {
        /* 远程来源但无任何哈希 → 拒装 */
        return NP_SKILL_ERR_HASH_MISSING;
    } else {
        /* 纯本地安装：记录实际哈希 */
        snprintf(info.sha256, sizeof(info.sha256), "%s", actual);
    }

    /* 重名同版本 */
    for (size_t i = 0; i < st->count; ++i) {
        if (strcmp(st->items[i].name, info.name) == 0 &&
            strcmp(st->items[i].version, info.version) == 0)
            return NP_SKILL_ERR_EXISTS;
    }
    if (st->count >= NP_SKILL_MAX) return NP_SKILL_ERR_FULL;

    /* 拷贝到 <store>/<name>-<version>/ */
    char dst[320];
#ifdef _WIN32
    snprintf(dst, sizeof(dst), "%s\\%s-%s", st->store_dir, info.name, info.version);
#else
    snprintf(dst, sizeof(dst), "%s/%s-%s", st->store_dir, info.name, info.version);
#endif
    if (!copy_dir_recursive(dir, dst)) return NP_SKILL_ERR_IO;

    snprintf(info.source_url, sizeof(info.source_url), "%s",
             source_url ? source_url : "");
    info.enabled = true;
    st->items[st->count++] = info;
    store_save(st);
    if (out_installed) *out_installed = info;
    return NP_SKILL_OK;
}

const np_skill_t *np_skill_find(const np_skill_store_t *st, const char *name) {
    if (!st || !name) return NULL;
    for (size_t i = 0; i < st->count; ++i)
        if (strcmp(st->items[i].name, name) == 0) return &st->items[i];
    return NULL;
}

np_skill_status_t np_skill_set_enabled(np_skill_store_t *st,
                                       const char *name, bool enabled) {
    if (!st || !name) return NP_SKILL_ERR_IO;
    for (size_t i = 0; i < st->count; ++i) {
        if (strcmp(st->items[i].name, name) == 0) {
            st->items[i].enabled = enabled;
            store_save(st);
            return NP_SKILL_OK;
        }
    }
    return NP_SKILL_ERR_NOT_FOUND;
}

np_skill_status_t np_skill_uninstall(np_skill_store_t *st, const char *name) {
    if (!st || !name) return NP_SKILL_ERR_IO;
    size_t found = st->count;
    for (size_t i = 0; i < st->count; ++i) {
        if (strcmp(st->items[i].name, name) == 0) { found = i; break; }
    }
    if (found == st->count) return NP_SKILL_ERR_NOT_FOUND;

    /* 删目录：store_dir/name-<version> */
    char dst[320];
#ifdef _WIN32
    snprintf(dst, sizeof(dst), "%s\\%s-%s", st->store_dir,
             st->items[found].name, st->items[found].version);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rmdir /S /Q \"%s\" > NUL 2>&1", dst);
    system(cmd);
#else
    snprintf(dst, sizeof(dst), "%s/%s-%s", st->store_dir,
             st->items[found].name, st->items[found].version);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' > /dev/null 2>&1", dst);
    system(cmd);
#endif
    /* 移除记录 */
    for (size_t i = found; i + 1 < st->count; ++i)
        st->items[i] = st->items[i + 1];
    st->count--;
    store_save(st);
    return NP_SKILL_OK;
}

size_t np_skill_list(const np_skill_store_t *st, np_skill_t *out, size_t out_cap) {
    if (!st || !out) return 0;
    size_t n = st->count < out_cap ? st->count : out_cap;
    for (size_t i = 0; i < n; ++i) out[i] = st->items[i];
    return n;
}

size_t np_skill_count(const np_skill_store_t *st) {
    return st ? st->count : 0;
}

const char *np_skill_status_name(np_skill_status_t s) {
    switch (s) {
    case NP_SKILL_OK: return "ok";
    case NP_SKILL_ERR_NO_MANIFEST: return "no-manifest";
    case NP_SKILL_ERR_BAD_MANIFEST: return "bad-manifest";
    case NP_SKILL_ERR_HASH_MISSING: return "hash-missing";
    case NP_SKILL_ERR_HASH_MISMATCH: return "hash-mismatch";
    case NP_SKILL_ERR_EXISTS: return "exists";
    case NP_SKILL_ERR_FULL: return "full";
    case NP_SKILL_ERR_NOT_FOUND: return "not-found";
    default: return "io";
    }
}
