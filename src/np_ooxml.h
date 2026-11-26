/* np_ooxml.h — F2: OOXML (OPC) 容器解析与结构树.
 *
 * 文档要求（F2 · S1 · P0）：Word/Excel/PDF 在编辑器里只是裸字节；
 * OOXML OPC 包 + XML 解析；段落/样式/表格/公式/批注/修订；
 * 复用 cui_tree 展示；点击节点定位部件与偏移。
 *
 * 安全红线：本模块<b>只读解析</b>，绝不修改任何字节，也不解压任何
 * 部件到磁盘。加密包（EncryptedPackage）直接拒绝。所有条目数、部件
 * 路径、XML 节点数、解压后大小均有硬上限，畸形/超大模型不会耗尽内存。
 *
 * 证据回跳：每个结构节点携带 part_index（ZIP 部件）与 part_offset
 * （部件内 XML 字节偏移）以及 path（形如 /word/document.xml#/w:tbl[0]），
 * 与 B5/F5 的 file#结构路径 引用协议一致。
 */
#ifndef NP_OOXML_H
#define NP_OOXML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_OX_MAX_PARTS   128
#define NP_OX_MAX_NODES   512
#define NP_OX_MAX_PATH    256   /* 部件路径 / 结构路径 */
#define NP_OX_MAX_LABEL   220   /* 节点可读标签 */
#define NP_OX_MAX_TAG     40
#define NP_OX_MAX_XML     (8u * 1024 * 1024)  /* 单个 XML 部件解压上限 */
#define NP_OX_MAX_ZIP     (512u * 1024 * 1024) /* 包文件上限 */
#define NP_OX_MAX_DEPTH   24

typedef enum {
    NP_OX_OK = 0,
    NP_OX_ERR_ARG,
    NP_OX_ERR_OPEN,        /* 无法打开文件 */
    NP_OX_ERR_NOT_ZIP,
    NP_OX_ERR_NOT_OPC,     /* 缺 [Content_Types].xml，不是 OPC 包 */
    NP_OX_ERR_ENCRYPTED,   /* 含 EncryptedPackage，加密不支持 */
    NP_OX_ERR_CORRUPT,
    NP_OX_ERR_TOO_BIG,
    NP_OX_ERR_UNSUPPORTED  /* ZIP64 / 未知压缩 / 加密条目 */
} np_ox_status_t;

const char *np_ox_status_name(np_ox_status_t s);

typedef enum {
    NP_OX_UNKNOWN = 0,
    NP_OX_WORD,
    NP_OX_EXCEL,
    NP_OX_POWERPOINT
} np_ox_kind_t;

const char *np_ox_kind_name(np_ox_kind_t k);

typedef enum {
    NP_OX_NODE_PART = 0,   /* ZIP 部件根节点 */
    NP_OX_NODE_PARAGRAPH,
    NP_OX_NODE_TABLE,
    NP_OX_NODE_ROW,
    NP_OX_NODE_CELL,
    NP_OX_NODE_STYLE,
    NP_OX_NODE_FORMULA,
    NP_OX_NODE_COMMENT,
    NP_OX_NODE_REVISION,
    NP_OX_NODE_SECTION,
    NP_OX_NODE_SHEET
} np_ox_node_type_t;

const char *np_ox_node_type_name(np_ox_node_type_t t);

typedef struct {
    char     path[NP_OX_MAX_PATH];  /* OPC 部件路径，如 word/document.xml */
    uint64_t local_off;             /* 本地头偏移（ZIP 内定位） */
    uint64_t usize;                 /* 解压后大小 */
    unsigned method;                /* 0=STORE 8=DEFLATE */
} np_ox_part_t;

typedef struct {
    np_ox_node_type_t type;
    int      part_index;            /* parts[] 下标 */
    int      parent;                /* 父节点下标，-1=根 */
    int      depth;
    uint64_t part_offset;           /* 部件内 XML 字节偏移（证据回跳） */
    char     tag[NP_OX_MAX_TAG];    /* 元素名，如 w:tbl */
    char     path[NP_OX_MAX_PATH];  /* /word/document.xml#/w:tbl[0]/... */
    char     label[NP_OX_MAX_LABEL];/* 可读标签（含截取文本/样式名） */
} np_ox_node_t;

typedef struct {
    bool         valid;
    bool         encrypted;
    np_ox_kind_t kind;
    char         main_part[NP_OX_MAX_PATH];
    int          nparts;
    np_ox_part_t parts[NP_OX_MAX_PARTS];
    int          nnodes;
    np_ox_node_t nodes[NP_OX_MAX_NODES];
    bool         truncated;         /* 触达节点/文本上限 */
    uint64_t     ntext_chars;       /* 抽取的字符总数（特征统计用） */
} np_ox_doc_t;

/* 从磁盘解析一个 OOXML 包（只读）。 */
np_ox_status_t np_ox_parse_file(const char *path, np_ox_doc_t *out);

/* 从内存缓冲解析（只读；调用方保留 data 所有权）。 */
np_ox_status_t np_ox_parse_mem(const unsigned char *data, size_t len,
                               np_ox_doc_t *out);

/* 按 OPC 部件路径查找；未找到返回 NULL。 */
const np_ox_part_t *np_ox_find_part(const np_ox_doc_t *d, const char *path);

/* 列出 parent 的直接子节点下标到 out（返回写入数量，最多 cap）。 */
int np_ox_children(const np_ox_doc_t *d, int parent, int *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_OOXML_H */
