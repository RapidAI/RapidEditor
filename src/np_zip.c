/* Minimal, self-contained ZIP reader for importing LaTeX project templates.
 *
 * Reads the central directory, supports STORE (0) and DEFLATE (8) entries,
 * and never writes outside dest_dir: absolute paths, drive letters and `..`
 * segments are rejected. Sizes and entry counts are capped so a hostile
 * archive cannot exhaust memory or disk. DEFLATE uses CUI's bundled raw
 * inflate (the same decoder already linked for the embedded fonts). */
#include "np_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

/* Raw DEFLATE decoder from vendor/CUI/src/cui_inflate.c (linked into every
 * build that has the embedded fonts; that is the default and every shipped
 * configuration). Declared here rather than pulling in CUI's private header. */
extern size_t cui__inflate(const void *src, size_t src_len, void *dst, size_t dst_cap);

#define NPZ_SIG_LOCAL   0x04034b50u
#define NPZ_SIG_CENTRAL 0x02014b50u
#define NPZ_SIG_EOCD    0x06054b50u
#define NPZ_MAX_ENTRIES 2048
#define NPZ_MAX_FILE    (64u * 1024 * 1024)   /* per-entry uncompressed cap */
#define NPZ_PATH_CAP    1024

/* All paths in this module are UTF-8. Windows' plain fopen()/stat() use the
 * process ANSI code page, so a CJK directory (or a CJK filename inside the
 * archive) silently fails to open. Route file I/O through wide APIs. */
static FILE *npz_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wp[NPZ_PATH_CAP], wm[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, NPZ_PATH_CAP) <= 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16) <= 0) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

typedef struct {
    unsigned method;
    unsigned long comp_size, uncomp_size, local_off;
    const char *name;
    unsigned name_len;
} npz_entry;

static unsigned npz_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned long npz_le32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void npz_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

static bool npz_mkdir_p(const char *path) {
    if (!path || !*path) return false;
    char tmp[NPZ_PATH_CAP];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return false;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char save = *p;
        *p = 0;
#ifdef _WIN32
        if (save != ':' || p != tmp + 2) {   /* don't try to create "C:" */
            wchar_t w[NPZ_PATH_CAP];
            if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, w, NPZ_PATH_CAP) > 0)
                CreateDirectoryW(w, NULL);   /* fails harmlessly if it exists */
        }
#else
        mkdir(tmp, 0755);
#endif
        *p = save;
    }
#ifdef _WIN32
    {
        wchar_t w[NPZ_PATH_CAP];
        if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, w, NPZ_PATH_CAP) <= 0) return false;
        if (!CreateDirectoryW(w, NULL)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
#else
    if (mkdir(tmp, 0755) == 0) return true;
    struct stat st;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Turn a raw entry name into a safe relative path (forward slashes), or
 * return false. Rejects absolute paths, drive prefixes and traversal. */
static bool npz_safe_name(const char *raw, unsigned len, char *out, size_t cap) {
    if (!raw || len == 0 || len >= cap) return false;
    char seg[NPZ_PATH_CAP];
    if (len >= sizeof seg) return false;
    memcpy(seg, raw, len);
    seg[len] = 0;
    for (char *p = seg; *p; p++) if (*p == '\\') *p = '/';
    if (seg[0] == '/') return false;
#ifdef _WIN32
    if (strlen(seg) >= 2 && seg[1] == ':') return false;
#endif
    size_t o = 0;
    const char *p = seg;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return false;
        if (o + n + 2 > cap) return false;
        if (o) out[o++] = '/';
        memcpy(out + o, p, n);
        o += n;
        if (!slash) break;
        p = slash + 1;
        if (!*p) break;   /* trailing slash: directory marker */
    }
    out[o] = 0;
    return o > 0;
}

static bool npz_is_tex(const char *name) {
    size_t n = strlen(name);
    return n >= 4 && name[n - 4] == '.' &&
           (name[n - 3] == 't' || name[n - 3] == 'T') &&
           (name[n - 2] == 'e' || name[n - 2] == 'E') &&
           (name[n - 1] == 'x' || name[n - 1] == 'X');
}

/* Lower score wins as a main-file candidate. */
static int npz_main_rank(const char *name, const char *text) {
    if (!npz_is_tex(name) || !strstr(text, "\\documentclass")) return 99;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    /* Template repos ship dozens of compilable test fixtures. They must not
     * win over the real entry point just because they appear first in the
     * ZIP. Same for nested example/sample trees. */
    if (strstr(name, "/test") || strstr(name, "/example/") ||
        strstr(name, "/sample") || strstr(name, "/doc/")) return 90;
    /* Real projects almost always keep the entry .tex at the archive root. */
    int depth = 0;
    for (const char *p = name; *p; p++) if (*p == '/') depth++;
    static const char *preferred[] = { "main", "index", "master", "paper",
                                       "thesis", "report", "cv", "resume",
                                       "example", "sample", "demo" };
    for (unsigned i = 0; i < sizeof preferred / sizeof *preferred; i++) {
        size_t l = strlen(preferred[i]);
        if (strncmp(base, preferred[i], l) == 0) return (int)i;
    }
    return 50 + depth * 5;
}

int np_zip_extract(const char *zip_path, const char *dest_dir, bool strip_root,
                   size_t max_total_bytes, int max_files,
                   char *main_tex, size_t main_tex_cap,
                   char *err, size_t err_cap) {
    if (main_tex && main_tex_cap) main_tex[0] = 0;
    if (err && err_cap) err[0] = 0;
    if (!zip_path || !dest_dir) { npz_err(err, err_cap, "参数无效"); return -1; }

    FILE *f = npz_fopen(zip_path, "rb");
    if (!f) { npz_err(err, err_cap, "无法打开 ZIP 文件"); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); npz_err(err, err_cap, "无法读取 ZIP 文件"); return -1; }
    long fsize = ftell(f);
    if (fsize < 22 || fsize > (long)max_total_bytes) {
        fclose(f); npz_err(err, err_cap, "ZIP 文件过大或为空"); return -1;
    }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)fsize);
    if (!buf) { fclose(f); npz_err(err, err_cap, "内存不足"); return -1; }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf); fclose(f); npz_err(err, err_cap, "读取 ZIP 失败"); return -1;
    }
    fclose(f);

    /* Locate the end-of-central-directory record (comment may be up to 64K). */
    long scan_start = fsize - 22;
    long floor = fsize - 22 - 65535;
    if (floor < 0) floor = 0;
    long eocd = -1;
    for (long i = scan_start; i >= floor; i--) {
        if (npz_le32(buf + i) == NPZ_SIG_EOCD) {
            unsigned clen = npz_u16(buf + i + 20);
            if (i + 22 + clen <= fsize) { eocd = i; break; }
        }
    }
    if (eocd < 0) { free(buf); npz_err(err, err_cap, "不是有效的 ZIP 归档"); return -1; }

    unsigned count = npz_u16(buf + eocd + 10);
    unsigned long cd_off = npz_le32(buf + eocd + 16);
    if (count == 0 || count > NPZ_MAX_ENTRIES || cd_off + 46 > (unsigned long)fsize) {
        free(buf); npz_err(err, err_cap, "ZIP 目录损坏或条目过多"); return -1;
    }

    npz_entry *ents = (npz_entry *)calloc(count, sizeof *ents);
    if (!ents) { free(buf); npz_err(err, err_cap, "内存不足"); return -1; }
    const unsigned char *p = buf + cd_off;
    unsigned valid = 0;
    for (unsigned i = 0; i < count; i++) {
        if ((unsigned long)(p - buf) + 46 > (unsigned long)fsize ||
            npz_le32(p) != NPZ_SIG_CENTRAL) break;
        unsigned nlen = npz_u16(p + 28), xlen = npz_u16(p + 30), clen = npz_u16(p + 32);
        unsigned long loff = npz_le32(p + 42);
        unsigned long cs = npz_le32(p + 20), us = npz_le32(p + 24);
        unsigned method = npz_u16(p + 10);
        if ((unsigned long)(p - buf) + 46 + nlen + xlen + clen > (unsigned long)fsize ||
            loff + 30 > (unsigned long)fsize) break;
        if (us == 0xFFFFFFFF || cs == 0xFFFFFFFF || us > NPZ_MAX_FILE ||
            (method != 0 && method != 8)) {
            free(ents); free(buf);
            npz_err(err, err_cap, "ZIP 含不支持的条目（加密/ZIP64/未知压缩）");
            return -1;
        }
        ents[valid].method = method;
        ents[valid].comp_size = cs;
        ents[valid].uncomp_size = us;
        ents[valid].local_off = loff;
        ents[valid].name = (const char *)p + 46;
        ents[valid].name_len = nlen;
        valid++;
        p += 46 + nlen + xlen + clen;
    }
    if (valid == 0) { free(ents); free(buf); npz_err(err, err_cap, "ZIP 目录为空"); return -1; }

    /* Detect one common top folder and strip it (GitHub/Overleaf layout). */
    int strip_len = 0;
    if (strip_root) {
        int root_len = 0;
        for (unsigned i = 0; i < valid; i++) {
            const char *n = ents[i].name;
            unsigned l = ents[i].name_len;
            const char *slash = NULL;
            for (unsigned k = 0; k < l; k++) if (n[k] == '/' || n[k] == '\\') { slash = n + k; break; }
            if (!slash || slash == n) { root_len = -1; break; }
            int len = (int)(slash - n);
            if (!root_len) root_len = len;
            else if (root_len != len || strncmp(n, ents[0].name, (size_t)len) != 0) { root_len = -1; break; }
        }
        if (root_len > 0) {
            /* every entry must share the root prefix */
            for (unsigned i = 0; i < valid; i++) {
                if (strncmp(ents[i].name, ents[0].name, (size_t)root_len) != 0 ||
                    (ents[i].name[root_len] != '/' && ents[i].name[root_len] != '\\')) {
                    root_len = -1; break;
                }
            }
            if (root_len > 0) strip_len = root_len + 1;
        }
    }

    int written = 0, best_rank = 99;
    size_t total = 0;
    char rel[NPZ_PATH_CAP], outpath[NPZ_PATH_CAP];
    unsigned char *plain = NULL;

    for (unsigned i = 0; i < valid; i++) {
        npz_entry *e = &ents[i];
        const char *name = e->name + strip_len;
        unsigned nlen = e->name_len > (unsigned)strip_len ? e->name_len - (unsigned)strip_len : 0;
        if (nlen == 0) continue;                       /* the root folder itself */
        if (!npz_safe_name(name, nlen, rel, sizeof rel)) {
            npz_err(err, err_cap, "ZIP 含不安全的路径，已拒绝");
            goto fail;
        }
        size_t rl = strlen(rel);
        if (e->name_len > strip_len && e->name[e->name_len - 1] == '/') continue; /* directory */
        if (written >= max_files || total + e->uncomp_size > max_total_bytes) {
            npz_err(err, err_cap, "ZIP 解压后超出文件数/大小上限");
            goto fail;
        }

        const unsigned char *lh = buf + e->local_off;
        if (npz_le32(lh) != NPZ_SIG_LOCAL) { npz_err(err, err_cap, "ZIP 本地文件头损坏"); goto fail; }
        unsigned lname = npz_u16(lh + 26), lextra = npz_u16(lh + 28);
        const unsigned char *data = lh + 30 + lname + lextra;
        if ((unsigned long)(data - buf) + e->comp_size > (unsigned long)fsize) {
            npz_err(err, err_cap, "ZIP 数据不完整"); goto fail;
        }

        unsigned char *content = NULL;
        size_t content_len = 0;
        if (e->method == 0) {
            content = (unsigned char *)data;
            content_len = e->comp_size;
        } else {
            plain = (unsigned char *)malloc((size_t)e->uncomp_size ? (size_t)e->uncomp_size : 1);
            if (!plain) { npz_err(err, err_cap, "内存不足"); goto fail; }
            size_t got = cui__inflate(data, e->comp_size, plain, (size_t)e->uncomp_size);
            if (got != (size_t)e->uncomp_size) {
                npz_err(err, err_cap, "ZIP 数据解压失败"); goto fail;
            }
            content = plain; content_len = got;
        }

        {
            size_t dl = strlen(dest_dir);
            bool sep = dl && (dest_dir[dl - 1] == '/' || dest_dir[dl - 1] == '\\');
            if (dl + (sep ? 0 : 1) + rl + 1 > sizeof outpath) {
                npz_err(err, err_cap, "路径过长"); goto fail;
            }
            snprintf(outpath, sizeof outpath, "%s%s%s", dest_dir, sep ? "" : "/", rel);
        }
        char *slash = NULL;
        for (char *q = outpath; *q; q++) if (*q == '/' || *q == '\\') slash = q;
        if (slash && slash != outpath && !(slash == outpath + 2 && outpath[1] == ':')) {
            char save = *slash; *slash = 0;
            if (!npz_mkdir_p(outpath)) { *slash = save; npz_err(err, err_cap, "无法创建项目目录"); goto fail; }
            *slash = save;
        }
        FILE *o = npz_fopen(outpath, "wb");
        if (!o) { npz_err(err, err_cap, "无法写入项目文件"); goto fail; }
        bool ok = (!content_len || fwrite(content, 1, content_len, o) == content_len);
        fclose(o);
        if (!ok) { npz_err(err, err_cap, "写入项目文件失败"); goto fail; }

        /* Rank a candidate main .tex by content, preferring common stems. */
        if (content_len) {
            char probe[8192];
            size_t plen = content_len < sizeof probe - 1 ? content_len : sizeof probe - 1;
            memcpy(probe, content, plen);
            probe[plen] = 0;
            int rank = npz_main_rank(rel, probe);
            if (rank < best_rank) {
                best_rank = rank;
                if (main_tex && main_tex_cap)
                    snprintf(main_tex, main_tex_cap, "%s", outpath);
            }
        }

        total += e->uncomp_size;
        written++;
        if (plain) { free(plain); plain = NULL; }
    }

    free(plain); free(ents); free(buf);
    return written;

fail:
    free(plain); free(ents); free(buf);
    if (err && err_cap && !err[0]) snprintf(err, err_cap, "未知解压错误");
    return -1;
}
