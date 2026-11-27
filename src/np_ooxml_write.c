/* np_ooxml_write.c - F8 safe OOXML write-back (see header). */
#include "np_ooxml_write.h"
#include "np_integrity.h"       /* np_crc32 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Self-contained raw DEFLATE decoder (same one np_zip uses). */
extern size_t cui__inflate(const void *src, size_t src_len, void *dst, size_t dst_cap);

#define OXW_SIG_LOCAL   0x04034b50u
#define OXW_SIG_CENTRAL 0x02014b50u
#define OXW_SIG_EOCD    0x06054b50u
#define OXW_MAX_ENTRIES 2048

static unsigned u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long le32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static void put16(unsigned char *p, unsigned v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void put32(unsigned char *p, unsigned long v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8);
    p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}
static void errset(char *err, size_t cap, const char *m) {
    if (err && cap) snprintf(err, cap, "%s", m);
}

/* ---- read ---------------------------------------------------------- */
bool np_oxw_read(const char *zip_path, np_oxw_pkg_t *pkg) {
    if (!zip_path || !pkg) return false;
    memset(pkg, 0, sizeof *pkg);
    FILE *f = fopen(zip_path, "rb");
    if (!f) { errset(pkg->err, sizeof pkg->err, "无法打开包"); return false; }
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 22) { fclose(f); errset(pkg->err, sizeof pkg->err, "包过小"); return false; }
    unsigned char *buf = (unsigned char *)malloc((size_t)fsz);
    if (!buf) { fclose(f); errset(pkg->err, sizeof pkg->err, "内存不足"); return false; }
    if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(buf); errset(pkg->err, sizeof pkg->err, "读取失败"); return false; }
    fclose(f);

    /* locate EOCD from the end */
    long eo = -1;
    for (long i = fsz - 22; i >= 0; --i) {
        if (le32(buf + i) == OXW_SIG_EOCD) { eo = i; break; }
    }
    if (eo < 0) { free(buf); errset(pkg->err, sizeof pkg->err, "未找到 EOCD"); return false; }
    unsigned nentries = u16(buf + eo + 10);
    unsigned long cd_off = le32(buf + eo + 16);
    if (nentries > OXW_MAX_ENTRIES || cd_off + 46 > (unsigned long)fsz) {
        free(buf); errset(pkg->err, sizeof pkg->err, "中央目录异常"); return false;
    }
    unsigned long p = cd_off;
    for (unsigned i = 0; i < nentries && pkg->count < NP_OXW_MAX_PARTS; ++i) {
        if (p + 46 > (unsigned long)fsz || le32(buf + p) != OXW_SIG_CENTRAL) break;
        unsigned method = u16(buf + p + 10);
        unsigned long csize = le32(buf + p + 20);
        unsigned long usize = le32(buf + p + 24);
        unsigned nlen = u16(buf + p + 28), xlen = u16(buf + p + 30), clen = u16(buf + p + 32);
        unsigned long loff = le32(buf + p + 42);
        const unsigned char *name_ptr = buf + p + 46;   /* name right after header */
        p += 46 + nlen + xlen + clen;
        if (nlen == 0 || nlen >= NP_OXW_NAME_CAP || usize > NP_OXW_MAX_PART_B ||
            loff + 30 > (unsigned long)fsz) continue;
        if (le32(buf + loff) != OXW_SIG_LOCAL) continue;
        unsigned lnlen = u16(buf + loff + 26), lxlen = u16(buf + loff + 28);
        unsigned long data_off = loff + 30 + lnlen + lxlen;
        if (data_off + csize > (unsigned long)fsz) continue;
        unsigned char *content = (unsigned char *)malloc(usize ? usize : 1);
        if (!content) continue;
        bool ok = false;
        if (method == 0 && csize == usize) {
            memcpy(content, buf + data_off, usize); ok = true;
        } else if (method == 8) {
            size_t got = cui__inflate(buf + data_off, (size_t)csize, content, (size_t)usize);
            ok = (got == (size_t)usize);
        }
        if (!ok) { free(content); continue; }
        np_oxw_part_t *part = &pkg->parts[pkg->count++];
        memcpy(part->name, name_ptr, nlen);
        part->name[nlen] = 0;
        part->data = content; part->len = usize;
    }
    free(buf);
    if (pkg->count == 0) { errset(pkg->err, sizeof pkg->err, "未读到任何部件"); return false; }
    return true;
}

void np_oxw_free(np_oxw_pkg_t *pkg) {
    if (!pkg) return;
    for (size_t i = 0; i < pkg->count; ++i) free(pkg->parts[i].data);
    memset(pkg, 0, sizeof *pkg);
}

np_oxw_part_t *np_oxw_find(np_oxw_pkg_t *pkg, const char *name) {
    if (!pkg || !name) return NULL;
    for (size_t i = 0; i < pkg->count; ++i)
        if (strcmp(pkg->parts[i].name, name) == 0) return &pkg->parts[i];
    return NULL;
}
const np_oxw_part_t *np_oxw_find_c(const np_oxw_pkg_t *pkg, const char *name) {
    return np_oxw_find((np_oxw_pkg_t *)pkg, name);
}

/* ---- XML well-formedness gate -------------------------------------- */
bool np_oxw_xml_wellformed(const char *xml, size_t len, char *err, size_t err_cap) {
    if (!xml || len == 0) { errset(err, err_cap, "空 XML"); return false; }
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)xml[i];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            errset(err, err_cap, "非法控制字符"); return false;
        }
    }
    /* Tag stack: track element names (ignore declarations/comments/PIs). */
    char stack[256][128];
    int top = 0;
    size_t i = 0;
    while (i < len) {
        size_t lt = i;
        while (lt < len && xml[lt] != '<') lt++;
        if (lt >= len) break;
        i = lt + 1;
        if (i >= len) { errset(err, err_cap, "标签未闭合"); return false; }
        if (xml[i] == '?' || xml[i] == '!') {  /* decl / comment / CDATA */
            const char *close = (xml[i] == '!') ? "-->" : "?>";
            const char *e = strstr(xml + i, close);
            if (!e) { errset(err, err_cap, "声明未闭合"); return false; }
            i = (size_t)(e - xml) + strlen(close);
            continue;
        }
        bool closing = (xml[i] == '/');
        if (closing) i++;
        size_t ns = i;
        while (i < len && xml[i] != '>' && xml[i] != ' ' && xml[i] != '/') i++;
        size_t nl = i - ns;
        if (nl == 0 || nl >= 128) { errset(err, err_cap, "标签名非法"); return false; }
        char name[128];
        memcpy(name, xml + ns, nl); name[nl] = 0;
        /* move to '>' and detect self-closing */
        bool selfclose = false;
        while (i < len && xml[i] != '>') {
            if (xml[i] == '/' && i + 1 < len && xml[i+1] == '>') selfclose = true;
            i++;
        }
        if (i >= len) { errset(err, err_cap, "标签未闭合"); return false; }
        i++;  /* skip '>' */
        if (selfclose) continue;
        if (closing) {
            if (top == 0 || strcmp(stack[top-1], name) != 0) {
                errset(err, err_cap, "标签错配"); return false;
            }
            top--;
        } else {
            if (top >= 256) { errset(err, err_cap, "嵌套过深"); return false; }
            strcpy(stack[top++], name);
        }
    }
    if (top != 0) { errset(err, err_cap, "存在未闭合标签"); return false; }
    return true;
}

/* ---- set part ------------------------------------------------------ */
bool np_oxw_set_part(np_oxw_pkg_t *pkg, const char *name,
                     const unsigned char *data, size_t len) {
    if (!pkg || !name || !data || len > NP_OXW_MAX_PART_B) return false;
    np_oxw_part_t *p = np_oxw_find(pkg, name);
    if (!p) return false;
    unsigned char *copy = (unsigned char *)malloc(len ? len : 1);
    if (!copy) return false;
    if (len) memcpy(copy, data, len);
    free(p->data);
    p->data = copy; p->len = len;
    return true;
}

/* ---- write STORE ZIP ---------------------------------------------- */
static bool write_all(FILE *f, const void *p, size_t n) {
    return n == 0 || fwrite(p, 1, n, f) == n;
}

bool np_oxw_commit(np_oxw_pkg_t *pkg, const char *out_path,
                   const char *original_path, const char *backup_path,
                   np_oxw_verify_fn verify, void *user,
                   char *err, size_t err_cap) {
    if (!pkg || !out_path || pkg->count == 0) { errset(err, err_cap, "参数缺失"); return false; }
    /* Backup original bytes first (one-step Ctrl+Z). */
    if (backup_path && original_path) {
        FILE *src = fopen(original_path, "rb"), *dst = fopen(backup_path, "wb");
        if (!src || !dst) {
            if (src) fclose(src);
            if (dst) fclose(dst);
            errset(err, err_cap, "无法创建回滚快照");
            return false;
        }
        char b[8192]; size_t n;
        while ((n = fread(b, 1, sizeof b, src)) > 0) write_all(dst, b, n);
        fclose(src); fclose(dst);
    }
    /* Build STORE zip in memory: local headers + data, then central dir + EOCD. */
    size_t local_total = 0, central_total = 0;
    for (size_t i = 0; i < pkg->count; ++i) {
        size_t nl = strlen(pkg->parts[i].name);
        local_total += 30 + nl + pkg->parts[i].len;
        central_total += 46 + nl;
    }
    size_t total = local_total + central_total + 22;
    unsigned char *zip = (unsigned char *)malloc(total);
    if (!zip) { errset(err, err_cap, "内存不足"); return false; }
    size_t at = 0;
    unsigned long *offsets = (unsigned long *)calloc(pkg->count, sizeof(unsigned long));
    if (!offsets) { free(zip); errset(err, err_cap, "内存不足"); return false; }
    for (size_t i = 0; i < pkg->count; ++i) {
        const np_oxw_part_t *p = &pkg->parts[i];
        size_t nl = strlen(p->name);
        uint32_t crc = np_crc32(p->data, p->len);
        offsets[i] = (unsigned long)at;
        put32(zip + at, OXW_SIG_LOCAL); at += 4;
        put16(zip + at, 20); at += 2;              /* version needed */
        put16(zip + at, 0); at += 2;               /* flags */
        put16(zip + at, 0); at += 2;               /* method = STORE */
        put16(zip + at, 0); at += 2;               /* time */
        put16(zip + at, 0); at += 2;               /* date */
        put32(zip + at, crc); at += 4;
        put32(zip + at, (unsigned long)p->len); at += 4;   /* comp size */
        put32(zip + at, (unsigned long)p->len); at += 4;   /* uncomp size */
        put16(zip + at, (unsigned)nl); at += 2;
        put16(zip + at, 0); at += 2;               /* extra len */
        memcpy(zip + at, p->name, nl); at += nl;
        if (p->len) { memcpy(zip + at, p->data, p->len); at += p->len; }
    }
    unsigned long cd_start = (unsigned long)at;
    for (size_t i = 0; i < pkg->count; ++i) {
        const np_oxw_part_t *p = &pkg->parts[i];
        size_t nl = strlen(p->name);
        uint32_t crc = np_crc32(p->data, p->len);
        put32(zip + at, OXW_SIG_CENTRAL); at += 4;
        put16(zip + at, 20); at += 2;              /* version made by */
        put16(zip + at, 20); at += 2;              /* version needed */
        put16(zip + at, 0); at += 2;               /* flags */
        put16(zip + at, 0); at += 2;               /* method STORE */
        put16(zip + at, 0); at += 2;
        put16(zip + at, 0); at += 2;
        put32(zip + at, crc); at += 4;
        put32(zip + at, (unsigned long)p->len); at += 4;
        put32(zip + at, (unsigned long)p->len); at += 4;
        put16(zip + at, (unsigned)nl); at += 2;    /* name len */
        put16(zip + at, 0); at += 2;               /* extra */
        put16(zip + at, 0); at += 2;               /* comment */
        put16(zip + at, 0); at += 2;               /* disk */
        put16(zip + at, 0); at += 2;               /* internal attr */
        put32(zip + at, 0); at += 4;               /* external attr */
        put32(zip + at, offsets[i]); at += 4;      /* local offset */
        memcpy(zip + at, p->name, nl); at += nl;
    }
    unsigned long cd_size = (unsigned long)at - cd_start;
    put32(zip + at, OXW_SIG_EOCD); at += 4;
    put16(zip + at, 0); at += 2;
    put16(zip + at, 0); at += 2;
    put16(zip + at, (unsigned)pkg->count); at += 2;
    put16(zip + at, (unsigned)pkg->count); at += 2;
    put32(zip + at, cd_size); at += 4;
    put32(zip + at, cd_start); at += 4;
    put16(zip + at, 0); at += 2;

    /* Temp file + verify + atomic rename. */
    char tmp[NP_OXW_NAME_CAP];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(zip); free(offsets); errset(err, err_cap, "无法写临时文件"); return false; }
    bool ok = write_all(f, zip, total);
    if (fclose(f) != 0) ok = false;
    free(zip); free(offsets);
    if (!ok) { remove(tmp); errset(err, err_cap, "写包失败"); return false; }
    if (verify && !verify(tmp, user)) { remove(tmp); errset(err, err_cap, "回读校验失败"); return false; }
    remove(out_path);   /* MSVCRT rename() rejects an existing target */
    if (rename(tmp, out_path) != 0) { remove(tmp); errset(err, err_cap, "原子替换失败"); return false; }
    return true;
}

bool np_oxw_restore(const char *backup_path, const char *target_path,
                    char *err, size_t err_cap) {
    if (!backup_path || !target_path) { errset(err, err_cap, "参数缺失"); return false; }
    FILE *src = fopen(backup_path, "rb");
    if (!src) { errset(err, err_cap, "快照不存在"); return false; }
    char tmp[NP_OXW_NAME_CAP];
    snprintf(tmp, sizeof tmp, "%s.tmp", target_path);
    FILE *dst = fopen(tmp, "wb");
    if (!dst) { fclose(src); errset(err, err_cap, "无法写临时文件"); return false; }
    char b[8192]; size_t n;
    while ((n = fread(b, 1, sizeof b, src)) > 0) write_all(dst, b, n);
    fclose(src);
    if (fclose(dst) != 0) { remove(tmp); errset(err, err_cap, "快照写入失败"); return false; }
    /* MSVCRT rename() fails when the target exists; remove it first. */
    remove(target_path);
    if (rename(tmp, target_path) != 0) { remove(tmp); errset(err, err_cap, "回滚替换失败"); return false; }
    return true;
}
