/* np_ooxml.c — F2 OOXML (OPC) 容器解析与结构树（只读）. 见 np_ooxml.h. */
#include "np_ooxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

/* Raw DEFLATE decoder from vendor/CUI/src/cui_inflate.c (linked wherever the
 * embedded fonts are, i.e. every shipped build). Declared locally to avoid
 * pulling in CUI's private header, same pattern as np_zip.c. */
extern size_t cui__inflate(const void *src, size_t src_len, void *dst, size_t dst_cap);

#define OX_SIG_LOCAL   0x04034b50u
#define OX_SIG_CENTRAL 0x02014b50u
#define OX_SIG_EOCD    0x06054b50u

static unsigned ox_u16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static uint64_t ox_le32(const unsigned char *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24);
}

const char *np_ox_status_name(np_ox_status_t s) {
    switch (s) {
        case NP_OX_OK: return "OK";
        case NP_OX_ERR_ARG: return "参数无效";
        case NP_OX_ERR_OPEN: return "无法打开";
        case NP_OX_ERR_NOT_ZIP: return "不是 ZIP 归档";
        case NP_OX_ERR_NOT_OPC: return "不是 OPC 包（缺 [Content_Types].xml）";
        case NP_OX_ERR_ENCRYPTED: return "加密包不支持";
        case NP_OX_ERR_CORRUPT: return "结构损坏";
        case NP_OX_ERR_TOO_BIG: return "超出大小上限";
        case NP_OX_ERR_UNSUPPORTED: return "不支持的条目（ZIP64/加密/未知压缩）";
        default: return "未知错误";
    }
}

const char *np_ox_kind_name(np_ox_kind_t k) {
    switch (k) {
        case NP_OX_WORD: return "Word";
        case NP_OX_EXCEL: return "Excel";
        case NP_OX_POWERPOINT: return "PowerPoint";
        default: return "未知 OOXML";
    }
}

const char *np_ox_node_type_name(np_ox_node_type_t t) {
    switch (t) {
        case NP_OX_NODE_PART: return "部件";
        case NP_OX_NODE_PARAGRAPH: return "段落";
        case NP_OX_NODE_TABLE: return "表格";
        case NP_OX_NODE_ROW: return "行";
        case NP_OX_NODE_CELL: return "单元格";
        case NP_OX_NODE_STYLE: return "样式";
        case NP_OX_NODE_FORMULA: return "公式";
        case NP_OX_NODE_COMMENT: return "批注";
        case NP_OX_NODE_REVISION: return "修订";
        case NP_OX_NODE_SECTION: return "节";
        case NP_OX_NODE_SHEET: return "工作表";
        default: return "元素";
    }
}

/* ---- ZIP 目录解析（只读，不落盘） ------------------------------------- */

typedef struct {
    unsigned method;
    uint64_t comp_size, usize, local_off;
    const char *name;
    unsigned name_len;
} ox_ent_t;

static void ox_set_status(np_ox_status_t *s, np_ox_status_t v) {
    if (s && *s == NP_OX_OK) *s = v;
}

/* 把部件名规整为正斜杠相对路径；拒绝绝对/盘符/.. 。 */
static bool ox_safe_name(const char *raw, unsigned len, char *out, size_t cap) {
    if (!raw || len == 0 || len >= cap) return false;
    char seg[NP_OX_MAX_PATH];
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
        if (!*p) break;
    }
    out[o] = 0;
    return o > 0;
}

/* 在部件表中查名（大小写敏感，OPC 部件路径区分大小写）。 */
const np_ox_part_t *np_ox_find_part(const np_ox_doc_t *d, const char *path) {
    if (!d || !path) return NULL;
    for (int i = 0; i < d->nparts; i++)
        if (strcmp(d->parts[i].path, path) == 0) return &d->parts[i];
    return NULL;
}

int np_ox_children(const np_ox_doc_t *d, int parent, int *out, int cap) {
    if (!d || !out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < d->nnodes && n < cap; i++)
        if (d->nodes[i].parent == parent) out[n++] = i;
    return n;
}

/* ---- XML 扫描：构建结构树 -------------------------------------------- */

typedef struct {
    char     tag[NP_OX_MAX_TAG];
    char     path[NP_OX_MAX_PATH];
    int      node;          /* 已发出的节点下标，-1=未发出 */
    int      parent_node;   /* 最近的已发出祖先节点 */
    bool     self_closing;
} ox_frame_t;

/* 实体解码：把 src 写入 dst（容量 cap），返回写入长度。 */
static size_t ox_xml_text(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (src[i] == '&') {
            if (i + 4 < n && src[i+1] == 'a' && src[i+2] == 'm' && src[i+3] == 'p' && src[i+4] == ';') {
                dst[o++] = '&'; i += 4; continue;
            }
            if (i + 3 < n && src[i+1] == 'l' && src[i+2] == 't' && src[i+3] == ';') {
                dst[o++] = '<'; i += 3; continue;
            }
            if (i + 3 < n && src[i+1] == 'g' && src[i+2] == 't' && src[i+3] == ';') {
                dst[o++] = '>'; i += 3; continue;
            }
            if (i + 5 < n && src[i+1] == 'q' && src[i+2] == 'u' && src[i+3] == 'o' && src[i+4] == 't' && src[i+5] == ';') {
                dst[o++] = '"'; i += 5; continue;
            }
            if (i + 5 < n && src[i+1] == 'a' && src[i+2] == 'p' && src[i+3] == 'o' && src[i+4] == 's' && src[i+5] == ';') {
                dst[o++] = '\''; i += 5; continue;
            }
            /* &#NNN; 十进制字符引用 */
            if (i + 2 < n && src[i+1] == '#') {
                size_t k = i + 2; unsigned v = 0;
                while (k < n && src[k] >= '0' && src[k] <= '9') {
                    v = v * 10 + (unsigned)(src[k] - '0'); k++;
                }
                if (k < n && src[k] == ';' && v && v < 0x110000) {
                    unsigned cp = v;
                    if (cp < 0x80) dst[o++] = (char)cp;
                    else {
                        /* 基本多文种平面转 UTF-8 */
                        unsigned c = cp;
                        if (c < 0x800) {
                            if (o + 2 < cap) { dst[o++] = (char)(0xC0 | (c >> 6)); dst[o++] = (char)(0x80 | (c & 0x3F)); }
                        } else if (c < 0x10000) {
                            if (o + 3 < cap) { dst[o++] = (char)(0xE0 | (c >> 12)); dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (c & 0x3F)); }
                        }
                    }
                    i = k; continue;
                }
            }
        }
        dst[o++] = src[i];
    }
    dst[o] = 0;
    return o;
}

/* 判断标签是否值得成为一个结构节点，并返回其类型。 */
static bool ox_tag_type(const char *tag, np_ox_node_type_t *t) {
    struct { const char *s; np_ox_node_type_t t; } map[] = {
        { "w:p",        NP_OX_NODE_PARAGRAPH },
        { "a:p",        NP_OX_NODE_PARAGRAPH },
        { "w:tbl",      NP_OX_NODE_TABLE },
        { "a:tbl",      NP_OX_NODE_TABLE },
        { "w:tr",       NP_OX_NODE_ROW },
        { "a:tr",       NP_OX_NODE_ROW },
        { "w:tc",       NP_OX_NODE_CELL },
        { "a:tc",       NP_OX_NODE_CELL },
        { "w:pStyle",   NP_OX_NODE_STYLE },
        { "w:rStyle",   NP_OX_NODE_STYLE },
        { "m:oMath",    NP_OX_NODE_FORMULA },
        { "m:f",        NP_OX_NODE_FORMULA },
        { "f",          NP_OX_NODE_FORMULA },   /* xlsx 单元格公式 */
        { "w:commentRangeStart", NP_OX_NODE_COMMENT },
        { "w:commentReference",  NP_OX_NODE_COMMENT },
        { "w:ins",      NP_OX_NODE_REVISION },
        { "w:del",      NP_OX_NODE_REVISION },
        { "w:sectPr",   NP_OX_NODE_SECTION },
        { "sheet",      NP_OX_NODE_SHEET }
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (strcmp(tag, map[i].s) == 0) { *t = map[i].t; return true; }
    return false;
}

/* 属性取值：在标签文本中找 name= 后的引号内容（支持 name 与 prefix:name）。 */
static void ox_attr(const char *tagtxt, size_t n, const char *name,
                    char *out, size_t cap) {
    out[0] = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        /* 匹配 "name" 或 ":name" 结尾，避免 val 命中 w:val 之外无关属性 */
        bool hit = false;
        size_t nl = strlen(name);
        if (i + nl <= n && memcmp(tagtxt + i, name, nl) == 0) {
            bool boundary_before = (i == 0) || tagtxt[i-1] == ' ' ||
                                   tagtxt[i-1] == ':' || tagtxt[i-1] == '\t' ||
                                   tagtxt[i-1] == '<';
            size_t e = i + nl;
            bool boundary_after = (e >= n) || tagtxt[e] == ' ' ||
                                  tagtxt[e] == '=' || tagtxt[e] == '\t';
            hit = boundary_before && boundary_after;
        }
        if (!hit) continue;
        size_t k = i + nl;
        while (k < n && (tagtxt[k] == ' ' || tagtxt[k] == '\t')) k++;
        if (k >= n || tagtxt[k] != '=') continue;
        k++;
        while (k < n && (tagtxt[k] == ' ' || tagtxt[k] == '\t')) k++;
        if (k >= n || (tagtxt[k] != '"' && tagtxt[k] != '\'')) continue;
        char q = tagtxt[k++];
        size_t o = 0;
        while (k < n && tagtxt[k] != q && o + 1 < cap) out[o++] = tagtxt[k++];
        out[o] = 0;
        return;
    }
}

/* 有界复制 NUL 结尾字符串，自动截断并补 NUL。手写以避开 snprintf 的
 * -Wformat-truncation / -Wrestrict 误报（源/目标可能在同一结构体数组内）。 */
static void ox_strcpy_bounded(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!cap) return;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* 有界路径追加：把 "/tag[idx]" 写到路径尾部，绝不越界。手动逐段写以避开
 * snprintf 对深层路径的 -Wformat-truncation 误报，行为等价于截断。 */
static void ox_path_push(char *path, size_t cap, const char *tag, unsigned idx) {
    size_t len = strnlen(path, cap);
    if (len + 1 >= cap) return;                 /* 无空间写 '/' + NUL */
    path[len++] = '/';
    for (const char *p = tag; *p && len + 1 < cap; p++) path[len++] = *p;
    if (len + 1 < cap) path[len++] = '[';
    char num[12];
    int n = snprintf(num, sizeof num, "%u]", idx);
    if (n < 0) n = 0;
    for (int i = 0; i < n && len + 1 < cap; i++) path[len++] = num[i];
    path[len] = 0;
}

/* 文本承载节点：段落/单元格/工作表/节可累加可见文本。 */
static bool ox_text_sink(np_ox_node_type_t t) {
    return t == NP_OX_NODE_PARAGRAPH || t == NP_OX_NODE_CELL ||
           t == NP_OX_NODE_SHEET || t == NP_OX_NODE_SECTION;
}

static void ox_append_label(np_ox_node_t *node, const char *text, size_t n,
                            bool *truncated) {
    if (!node || !n) return;
    size_t len = strnlen(node->label, sizeof node->label);
    size_t room = sizeof node->label - len - 1;   /* 留 NUL */
    if (room == 0) { *truncated = true; return; }
    /* 去掉首尾空白，但保留词间空格 */
    while (n && isspace((unsigned char)text[0])) { text++; n--; }
    while (n && isspace((unsigned char)text[n-1])) n--;
    if (!n) return;
    if (n > room) { n = room; *truncated = true; }
    if (len) node->label[len++] = ' ';
    if (len + n >= sizeof node->label) { n = sizeof node->label - len - 1; *truncated = true; }
    memcpy(node->label + len, text, n);
    node->label[len + n] = 0;
}

/* 扫描一个 XML 部件，把结构节点追加到 doc。返回是否成功。 */
static bool ox_scan_xml(np_ox_doc_t *doc, int part_index, int root_node,
                        const unsigned char *xml, size_t len) {
    ox_frame_t stack[NP_OX_MAX_DEPTH];
    int sp = 0;
    unsigned short *sib = NULL;
    /* 兄弟计数器：按 (parent_node+1, tag hash) 分桶，避免 O(n^2) 全扫描。 */
    enum { NBUCKET = 64 };
    int parent_slots = doc->nnodes + NP_OX_MAX_NODES + 1;
    if (parent_slots > NP_OX_MAX_NODES + 1) parent_slots = NP_OX_MAX_NODES + 1;
    sib = (unsigned short *)calloc((size_t)parent_slots * NBUCKET, sizeof *sib);
    if (!sib) return false;
    /* 节点号可能在解析中增长，下面用 doc->nnodes 动态上限保护索引。 */

    size_t i = 0;
    while (i < len) {
        if (xml[i] != '<') { i++; continue; }
        if (i + 1 >= len) break;
        size_t tag_start = i + 1;
        bool closing = false, declaration = false, comment = false;
        if (xml[tag_start] == '/') { closing = true; tag_start++; }
        else if (xml[tag_start] == '?' || xml[tag_start] == '!') {
            if (xml[tag_start] == '!' && tag_start + 2 < len &&
                xml[tag_start+1] == '-' && xml[tag_start+2] == '-') comment = true;
            else declaration = true;
        }
        size_t k = tag_start;
        while (k < len && !isspace((unsigned char)xml[k]) &&
               xml[k] != '>' && xml[k] != '/') k++;
        char tag[NP_OX_MAX_TAG];
        size_t tl = k - tag_start;
        if (tl >= sizeof tag) tl = sizeof tag - 1;
        memcpy(tag, xml + tag_start, tl);
        tag[tl] = 0;

        /* 找到标签结束 '>'（跳过引号内可能出现 > 的属性值） */
        size_t e = k;
        char quote = 0;
        while (e < len) {
            unsigned char c = xml[e];
            if (quote) { if (c == (unsigned char)quote) quote = 0; }
            else if (c == '"' || c == '\'') quote = (char)c;
            else if (c == '>') break;
            e++;
        }
        if (e >= len) break;   /* 截断 XML */
        bool self_close = false;
        if (e > k && xml[e-1] == '/') self_close = true;

        /* 注释/声明：整体跳过（注释可能含 '<'，直接跳到 '>' 后的内容） */
        if (comment) { i = e + 1; continue; }
        if (declaration) { i = e + 1; continue; }
        (void)declaration;

        if (closing) {
            /* 文本累加发生在标签之间；此处只负责退栈 */
            if (sp > 0 && strcmp(stack[sp-1].tag, tag) == 0) sp--;
            else {
                /* 失配：向后寻找同名帧并弹出其上的所有帧（容错） */
                int found = -1;
                for (int f = sp - 1; f >= 0; f--)
                    if (strcmp(stack[f].tag, tag) == 0) { found = f; break; }
                if (found >= 0) sp = found;
            }
            i = e + 1;
            continue;
        }

        /* 文本节点：<w:t>/<a:t>/<t>/<v> 的文本写到最近的文本承载祖先 */
        bool is_text_tag = (strcmp(tag, "w:t") == 0 || strcmp(tag, "a:t") == 0 ||
                            strcmp(tag, "t") == 0 || strcmp(tag, "v") == 0);
        if (is_text_tag && !self_close) {
            size_t ts = e + 1;
            size_t te = ts;
            while (te < len && xml[te] != '<') te++;
            if (te > ts && sp > 0 && doc->nnodes > 0) {
                int sink = -1;
                for (int f = sp - 1; f >= 0; f--) {
                    int ni = stack[f].node;
                    if (ni >= 0 && ni < doc->nnodes &&
                        ox_text_sink(doc->nodes[ni].type)) { sink = ni; break; }
                }
                char decoded[NP_OX_MAX_LABEL];
                size_t dn = ox_xml_text((const char *)xml + ts, te - ts,
                                        decoded, sizeof decoded);
                if (sink >= 0) {
                    ox_append_label(&doc->nodes[sink], decoded, dn, &doc->truncated);
                    doc->ntext_chars += dn;
                }
            }
            /* 文本标签没有入栈，因此必须整段跳过其闭合标签 </tag>，
             * 否则闭合分支会把承载节点（w:r/w:p）的帧错误弹出。 */
            size_t ce = te;
            while (ce < len && xml[ce] != '>') ce++;
            i = (ce < len) ? ce + 1 : len;
            continue;
        }

        /* 建立帧 */
        if (sp >= NP_OX_MAX_DEPTH) { doc->truncated = true; i = e + 1; continue; }
        ox_frame_t *fr = &stack[sp];
        snprintf(fr->tag, sizeof fr->tag, "%s", tag);
        fr->self_closing = self_close;

        /* 计算结构路径与兄弟序号 */
        int parent_node = (sp > 0) ? stack[sp-1].parent_node : root_node;
        /* 若父帧自身发出了节点，则它成为新的父节点 */
        if (sp > 0 && stack[sp-1].node >= 0) parent_node = stack[sp-1].node;
        fr->parent_node = parent_node;

        unsigned h = 0;
        for (size_t q = 0; q < tl; q++) h = h * 31 + (unsigned char)tag[q];
        h &= (NBUCKET - 1);
        unsigned slots = (unsigned)(parent_node + 1);
        if (slots >= (unsigned)parent_slots) slots = (unsigned)parent_slots - 1;
        unsigned short idx = sib[(size_t)slots * NBUCKET + h]++;

        if (sp == 0) {
            /* 顶层元素：路径以部件名为根 */
            size_t pl = 0, cap = sizeof fr->path;
            fr->path[pl++] = '/';
            const char *pp = doc->parts[part_index].path;
            for (; *pp && pl + 1 < cap; pp++) fr->path[pl++] = *pp;
            if (pl + 1 < cap) fr->path[pl++] = '#';
            fr->path[pl] = 0;
            ox_path_push(fr->path, cap, tag, (unsigned)idx);
        } else {
            ox_strcpy_bounded(fr->path, sizeof fr->path, stack[sp-1].path);
            ox_path_push(fr->path, sizeof fr->path, tag, (unsigned)idx);
        }

        /* 决定是否发出结构节点 */
        np_ox_node_type_t t;
        fr->node = -1;
        if (ox_tag_type(tag, &t)) {
            if (doc->nnodes < NP_OX_MAX_NODES) {
                np_ox_node_t *nd = &doc->nodes[doc->nnodes];
                memset(nd, 0, sizeof *nd);
                nd->type = t;
                nd->part_index = part_index;
                nd->parent = parent_node;
                nd->depth = sp;
                nd->part_offset = (uint64_t)i;
                snprintf(nd->tag, sizeof nd->tag, "%s", tag);
                snprintf(nd->path, sizeof nd->path, "%s", fr->path);
                /* 样式名写入标签 */
                if (t == NP_OX_NODE_STYLE) {
                    char val[64];
                    ox_attr((const char *)xml + k, e - k, "val", val, sizeof val);
                    if (!val[0]) ox_attr((const char *)xml + k, e - k, "w:val", val, sizeof val);
                    snprintf(nd->label, sizeof nd->label, "%s", val[0] ? val : tag);
                } else {
                    snprintf(nd->label, sizeof nd->label, "%s", tag);
                }
                fr->node = doc->nnodes;
                doc->nnodes++;
            } else {
                doc->truncated = true;
            }
        }

        sp++;
        if (self_close) {
            /* 自闭合：立即退栈，但保留已发出的节点 */
            sp--;
        }
        i = e + 1;
    }

    free(sib);
    return true;
}

/* ---- 部件解压 -------------------------------------------------------- */

static bool ox_read_part(const unsigned char *buf, size_t fsize,
                         const ox_ent_t *ents, unsigned nent,
                         const char *name, unsigned char **out,
                         size_t *out_len) {
    *out = NULL; *out_len = 0;
    for (unsigned i = 0; i < nent; i++) {
        const ox_ent_t *e = &ents[i];
        unsigned nl = e->name_len;
        if (nl != strlen(name)) continue;
        if (memcmp(e->name, name, nl) != 0) continue;
        if (e->usize > NP_OX_MAX_XML) return false;
        const unsigned char *lh = buf + e->local_off;
        if ((uint64_t)e->local_off + 30 > fsize || ox_le32(lh) != OX_SIG_LOCAL) return false;
        unsigned lname = ox_u16(lh + 26), lextra = ox_u16(lh + 28);
        const unsigned char *data = lh + 30 + lname + lextra;
        if ((uint64_t)(data - buf) + e->comp_size > fsize) return false;
        if (e->method == 0) {
            unsigned char *p = (unsigned char *)malloc((size_t)e->usize ? (size_t)e->usize : 1);
            if (!p) return false;
            if (e->usize) memcpy(p, data, (size_t)e->usize);
            *out = p; *out_len = (size_t)e->usize;
            return true;
        } else if (e->method == 8) {
            unsigned char *p = (unsigned char *)malloc((size_t)e->usize ? (size_t)e->usize : 1);
            if (!p) return false;
            size_t got = cui__inflate(data, (size_t)e->comp_size, p, (size_t)e->usize);
            if (got != (size_t)e->usize) { free(p); return false; }
            *out = p; *out_len = got;
            return true;
        }
        return false;
    }
    return false;
}

/* ---- 主入口 ---------------------------------------------------------- */

static np_ox_status_t ox_parse(const unsigned char *buf, size_t fsize,
                               np_ox_doc_t *out) {
    memset(out, 0, sizeof *out);
    if (!buf || fsize < 22) return NP_OX_ERR_NOT_ZIP;
    if (fsize > NP_OX_MAX_ZIP) return NP_OX_ERR_TOO_BIG;

    /* EOCD */
    long eocd = -1;
    long floor = (long)fsize - 22 - 65535;
    if (floor < 0) floor = 0;
    for (long i = (long)fsize - 22; i >= floor; i--) {
        if (i >= 0 && ox_le32(buf + i) == OX_SIG_EOCD) {
            unsigned clen = ox_u16(buf + i + 20);
            if ((uint64_t)i + 22 + clen <= fsize) { eocd = i; break; }
        }
    }
    if (eocd < 0) return NP_OX_ERR_NOT_ZIP;

    unsigned count = ox_u16(buf + eocd + 10);
    uint64_t cd_off = ox_le32(buf + eocd + 16);
    if (count == 0 || count > 4096 || cd_off + 46 > fsize) return NP_OX_ERR_CORRUPT;

    ox_ent_t *ents = (ox_ent_t *)calloc(count, sizeof *ents);
    if (!ents) return NP_OX_ERR_CORRUPT;
    const unsigned char *p = buf + cd_off;
    unsigned nent = 0;
    np_ox_status_t status = NP_OX_OK;
    for (unsigned i = 0; i < count; i++) {
        if ((uint64_t)(p - buf) + 46 > fsize || ox_le32(p) != OX_SIG_CENTRAL) break;
        unsigned nlen = ox_u16(p + 28), xlen = ox_u16(p + 30), clen = ox_u16(p + 32);
        uint64_t loff = ox_le32(p + 42);
        uint64_t cs = ox_le32(p + 20), us = ox_le32(p + 24);
        unsigned method = ox_u16(p + 10);
        unsigned flags = ox_u16(p + 8);
        if ((uint64_t)(p - buf) + 46 + nlen + xlen + clen > fsize || loff + 30 > fsize) break;
        if (us == 0xFFFFFFFFu || cs == 0xFFFFFFFFu) { ox_set_status(&status, NP_OX_ERR_UNSUPPORTED); goto done; }
        if (flags & 0x1) { ox_set_status(&status, NP_OX_ERR_UNSUPPORTED); goto done; } /* 加密条目 */
        if (method != 0 && method != 8) { ox_set_status(&status, NP_OX_ERR_UNSUPPORTED); goto done; }
        if (nent < count) {
            ents[nent].method = method;
            ents[nent].comp_size = cs;
            ents[nent].usize = us;
            ents[nent].local_off = loff;
            ents[nent].name = (const char *)p + 46;
            ents[nent].name_len = nlen;
            nent++;
        }
        p += 46 + nlen + xlen + clen;
    }
    if (nent == 0) { status = NP_OX_ERR_CORRUPT; goto done; }

    /* 先判加密包 */
    for (unsigned i = 0; i < nent; i++) {
        const char *n = ents[i].name; unsigned l = ents[i].name_len;
        if ((l == 16 && memcmp(n, "EncryptedPackage", 16) == 0) ||
            (l >= 13 && memcmp(n, "EncryptedPackage", 13) == 0)) {
            out->encrypted = true;
            status = NP_OX_ERR_ENCRYPTED;
            goto done;
        }
    }
    /* OPC 强制部件 */
    bool has_ct = false;
    for (unsigned i = 0; i < nent; i++)
        if (ents[i].name_len == 19 && memcmp(ents[i].name, "[Content_Types].xml", 19) == 0) {
            has_ct = true; break;
        }
    if (!has_ct) { status = NP_OX_ERR_NOT_OPC; goto done; }

    /* 枚举 OPC 部件为根节点 */
    for (unsigned i = 0; i < nent && out->nparts < NP_OX_MAX_PARTS; i++) {
        char rel[NP_OX_MAX_PATH];
        if (!ox_safe_name(ents[i].name, ents[i].name_len, rel, sizeof rel)) continue;
        if (ents[i].name_len > 0 && ents[i].name[ents[i].name_len - 1] == '/') continue; /* 目录 */
        np_ox_part_t *pt = &out->parts[out->nparts];
        snprintf(pt->path, sizeof pt->path, "%s", rel);
        pt->local_off = ents[i].local_off;
        pt->usize = ents[i].usize;
        pt->method = ents[i].method;
        int ni = out->nnodes;
        if (ni < NP_OX_MAX_NODES) {
            np_ox_node_t *nd = &out->nodes[ni];
            memset(nd, 0, sizeof *nd);
            nd->type = NP_OX_NODE_PART;
            nd->part_index = out->nparts;
            nd->parent = -1;
            nd->depth = 0;
            nd->part_offset = 0;
            ox_strcpy_bounded(nd->tag, sizeof nd->tag, "part");
            nd->path[0] = '/';
            ox_strcpy_bounded(nd->path + 1, sizeof nd->path - 1, rel);
            ox_strcpy_bounded(nd->label, sizeof nd->label, rel);
            out->nnodes++;
        } else out->truncated = true;
        out->nparts++;
    }

    /* 判定主部件 */
    static const char *main_candidates[] = {
        "word/document.xml", "xl/workbook.xml", "ppt/presentation.xml"
    };
    int main_idx = -1;
    for (unsigned c = 0; c < sizeof main_candidates / sizeof *main_candidates; c++) {
        if (np_ox_find_part(out, main_candidates[c])) {
            snprintf(out->main_part, sizeof out->main_part, "%s", main_candidates[c]);
            out->kind = (c == 0) ? NP_OX_WORD : (c == 1) ? NP_OX_EXCEL : NP_OX_POWERPOINT;
            main_idx = c;
            break;
        }
    }
    if (main_idx < 0) {
        out->kind = NP_OX_UNKNOWN;
        status = NP_OX_OK;   /* 是 OPC 包，但无已知主文档：仅列部件 */
        goto done;
    }

    /* 解析主部件的 XML 结构 */
    {
        unsigned char *xml = NULL; size_t xlen = 0;
        if (!ox_read_part(buf, fsize, ents, nent, out->main_part, &xml, &xlen)) {
            /* 主部件读取失败：保留部件清单，标注截断，不致命 */
            out->truncated = true;
            status = NP_OX_OK;
            goto done;
        }
        {
            int mpi = (int)(np_ox_find_part(out, out->main_part) - out->parts);
            /* 部件根节点按枚举顺序创建：node i == part i。 */
            int root_node = (mpi >= 0 && mpi < out->nnodes) ? mpi : -1;
            if (!ox_scan_xml(out, mpi, root_node, xml, xlen)) {
                out->truncated = true;
            }
        }
        free(xml);
    }

    /* Excel 额外解析 workbook 的 sheet 名已由主部件 tag 覆盖；PowerPoint 同理。 */
done:
    free(ents);
    if (status == NP_OX_OK && (out->nparts > 0)) out->valid = true;
    return status;
}

np_ox_status_t np_ox_parse_mem(const unsigned char *data, size_t len, np_ox_doc_t *out) {
    if (!out) return NP_OX_ERR_ARG;
    return ox_parse(data, len, out);
}

np_ox_status_t np_ox_parse_file(const char *path, np_ox_doc_t *out) {
    if (!path || !out) return NP_OX_ERR_ARG;
    FILE *f = NULL;
#ifdef _WIN32
    {
        wchar_t wp[NP_OX_MAX_PATH];
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, NP_OX_MAX_PATH) <= 0)
            return NP_OX_ERR_OPEN;
        f = _wfopen(wp, L"rb");
    }
#else
    f = fopen(path, "rb");
#endif
    if (!f) return NP_OX_ERR_OPEN;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NP_OX_ERR_OPEN; }
    long sz = ftell(f);
    if (sz < 0 || (uint64_t)sz > NP_OX_MAX_ZIP) { fclose(f); return NP_OX_ERR_TOO_BIG; }
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NP_OX_ERR_CORRUPT; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NP_OX_ERR_OPEN;
    }
    fclose(f);
    np_ox_status_t s = ox_parse(buf, (size_t)sz, out);
    free(buf);
    return s;
}
