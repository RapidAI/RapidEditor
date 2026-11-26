/* np_probe.h — S1: H9 统一格式探测与路由入口 + R1 程序格式/架构识别 + F1 格式自动识别.
 *
 * 文档要求（S1 · P0）：
 *  - H9: 打开文件时不该由用户判断该进文本、程序还是数据格式轨。
 *        统一嗅探魔数与容器，分流到 R1（程序）或 F1（数据格式）；
 *        未识别则保持 Hex 并给线索；本身不实现格式解析。
 *  - R1: 魔数 + ELF/PE/Mach-O 头解析；输出架构/位宽/端序/加载基址与置信度；
 *        龙芯区分 LoongArch 与旧 MIPS。
 *  - F1: 魔数 + 容器部件清单双判定；输出格式/版本/加密状态与置信度；
 *        低置信度要求用户选择。
 *
 * 设计红线：本模块只做"识别与路由"，不做任何解析后的内容展示或写入。
 * 全部输入是内存中的字节缓冲（调用方负责读文件），全部输出是 POD 结构。
 */
#ifndef NP_PROBE_H
#define NP_PROBE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "np_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 架构识别（R1 输出）                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    NP_PARCH_UNKNOWN = 0,
    NP_PARCH_X86,        /* 32-bit */
    NP_PARCH_X64,        /* x86-64 / AMD64 / Intel 64 */
    NP_PARCH_ARM,        /* ARM 32-bit (incl. Thumb capable) */
    NP_PARCH_AARCH64,
    NP_PARCH_MIPS,       /* 旧 MIPS（含 MIPS64） */
    NP_PARCH_LOONGARCH,  /* LoongArch (龙芯) */
    NP_PARCH_RISCV,
    NP_PARCH_POWERPC,
    NP_PARCH_S390X
} np_parch_t;

const char *np_parch_name(np_parch_t a);

typedef enum { NP_ENDIAN_LITTLE = 0, NP_ENDIAN_BIG = 1 } np_endian_t;

/* ------------------------------------------------------------------ */
/* 探测结论                                                             */
/* ------------------------------------------------------------------ */

/* 路由去向（H9 输出） */
typedef enum {
    NP_ROUTE_TEXT = 0,   /* 普通文本轨 */
    NP_ROUTE_PROGRAM,    /* 程序二进制轨（R 系列） */
    NP_ROUTE_FORMAT,     /* 数据格式轨（F 系列） */
    NP_ROUTE_HEX,        /* 未识别 → 保持 Hex 并给线索 */
} np_route_t;

const char *np_route_name(np_route_t r);

/* R1：程序头解析结果 */
typedef struct {
    np_parch_t   arch;
    np_endian_t endian;
    int         bits;             /* 32 / 64 / 0=unknown */
    uint64_t    entry;            /* 入口地址，0=未知 */
    uint64_t    image_base;       /* 加载基址，0=未知 */
    bool        is_pie;           /* ELF e_type==ET_DYN 视作 PIE */
    bool        is_stripped;      /* 无符号表（ELF 判定） */
    bool        thumb_capable;    /* ARM: e_flags 提示 Thumb 互工作 */
} np_prog_info_t;

/* F1：数据格式识别结果 */
typedef enum {
    NP_FMT_UNKNOWN = 0,
    /* 容器类 */
    NP_FMT_OOXML,        /* docx/xlsx/pptx 等 OPC 包（ZIP + [Content_Types].xml） */
    NP_FMT_ODF,          /* OpenDocument（ZIP + mimetype=application/vnd.oasis...） */
    NP_FMT_ZIP_PLAIN,    /* ZIP 但无 OPC/ODF 特征 */
    NP_FMT_SQLITE,       /* SQLite format 3\0 */
    NP_FMT_PDF,
    NP_FMT_PNG, NP_FMT_JPEG, NP_FMT_GIF, NP_FMT_BMP,
    NP_FMT_ELF, NP_FMT_PE, NP_FMT_MACHO,   /* 原样报告：程序容器也是"格式" */
    NP_FMT_XML, NP_FMT_JSON,               /* 文本类结构化 */
    NP_FMT_TAR, NP_FMT_GZIP,
    NP_FMT_OLE2,         /* 旧版 .doc/.xls/.msi（CFB 复合文档） */
    NP_FMT_SQLOLAP       /* msi 判定外的少见值，预留 */
} np_format_kind_t;

const char *np_format_name(np_format_kind_t k);

typedef struct {
    np_format_kind_t kind;
    char             subtype[64];  /* OOXML: 首个部件推断 word/excel/powerpoint；ODF: mimetype 值 */
    bool             encrypted;    /* ODF encryption.xml / CFB 加密 / ZIP AES 标记 */
    uint16_t         ooxml_creator;/* 0=none, 1=word, 2=excel, 3=powerpoint（容器内判断） */
} np_fmt_info_t;

/* H9：统一探测结论 */
typedef struct {
    np_route_t     route;         /* 分流去向 */
    np_track_t     track;         /* 对应三轨枚举 */
    np_prog_info_t prog;          /* route==NP_ROUTE_PROGRAM 时有效 */
    np_fmt_info_t  fmt;           /* route==NP_ROUTE_FORMAT 时有效 */
    bool           has_bom;       /* UTF-8/UTF-16 BOM */
    bool           likely_text;   /* 可打印率与换行率达标 */
    double         printable_ratio; /* 0..1，HEX 线索 */
    int            confidence;    /* 0..100；<70 时 UI 应让用户选择 */
    char           clue[192];     /* 未识别时给用户的线索描述 */
} np_probe_result_t;

/* 主入口：对内存缓冲做统一探测。data==NULL 或 len==0 视作空文件 → TEXT。 */
void np_probe(const unsigned char *data, size_t len, const char *filename_hint,
              np_probe_result_t *out);

/* 单独暴露：R1 程序头解析。返回 true 表示识别出 ELF/PE/Mach-O。
 * 不依赖 filename_hint（扩展名不可信）。 */
bool np_probe_program(const unsigned char *data, size_t len, np_prog_info_t *out);

/* 单独暴露：F1 格式识别。返回 true 表示识别出已知格式。 */
bool np_probe_format(const unsigned char *data, size_t len,
                     const char *filename_hint, np_fmt_info_t *out);

/* ZIP 中央目录/本地头辅助：检查缓冲是否为 ZIP 且包含指定名字的部件。
 * 用于 OPC ([Content_Types].xml) 与 ODF (mimetype) 的容器双判定。
 * 只扫描头 64KB + 尾 64KB 内的本地文件头，不实现完整 ZIP 解析。 */
bool np_probe_zip_contains(const unsigned char *data, size_t len,
                           const char *part_name, size_t *part_size_out);

#ifdef __cplusplus
}
#endif
#endif /* NP_PROBE_H */
