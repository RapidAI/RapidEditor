/* test_f2_ooxml.c — F2: OOXML OPC 容器解析与结构树单元测试（黄金样本）. */
#include "np_ooxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* 统计某类节点数量 */
static int count_type(const np_ox_doc_t *d, np_ox_node_type_t t) {
    int n = 0;
    for (int i = 0; i < d->nnodes; i++) if (d->nodes[i].type == t) n++;
    return n;
}

/* 找第一个含指定子串 label 的节点 */
static int find_label(const np_ox_doc_t *d, const char *sub) {
    for (int i = 0; i < d->nnodes; i++)
        if (strstr(d->nodes[i].label, sub)) return i;
    return -1;
}

static int test_docx(void) {
    printf("== F2: golden.docx (Word) ==\n");
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file("tests/golden/golden.docx", &d);
    CHECK(st == NP_OX_OK);
    CHECK(d.valid == true);
    CHECK(d.kind == NP_OX_WORD);
    CHECK(strcmp(np_ox_kind_name(d.kind), "Word") == 0);
    CHECK(strcmp(d.main_part, "word/document.xml") == 0);

    /* 部件清单：至少 4 个 OPC 部件，含 [Content_Types].xml 与 word/document.xml */
    CHECK(d.nparts >= 4);
    CHECK(np_ox_find_part(&d, "[Content_Types].xml") != NULL);
    CHECK(np_ox_find_part(&d, "word/document.xml") != NULL);
    CHECK(np_ox_find_part(&d, "_rels/.rels") != NULL);
    CHECK(np_ox_find_part(&d, "word/styles.xml") != NULL);
    /* 根部件节点 */
    CHECK(count_type(&d, NP_OX_NODE_PART) == d.nparts);

    /* 段落：Heading1 标题 + 正文 + 表格单元格内段落 x4 + 2 正文 = 多个 */
    int npara = count_type(&d, NP_OX_NODE_PARAGRAPH);
    CHECK(npara >= 6);
    /* 标题段落的 label 含标题文本（实体 &amp; 已解码为 &） */
    int title = find_label(&d, "Quarterly Report & Outlook");
    CHECK(title >= 0);
    if (title >= 0) {
        CHECK(d.nodes[title].type == NP_OX_NODE_PARAGRAPH);
        CHECK(strstr(d.nodes[title].path, "/word/document.xml#") != NULL);
        CHECK(strstr(d.nodes[title].path, "w:p[") != NULL);
    }
    /* 表格：1 个 table，2 行，4 单元格 */
    CHECK(count_type(&d, NP_OX_NODE_TABLE) == 1);
    CHECK(count_type(&d, NP_OX_NODE_ROW) == 2);
    CHECK(count_type(&d, NP_OX_NODE_CELL) == 4);
    /* 样式 Heading1 */
    int style = find_label(&d, "Heading1");
    CHECK(style >= 0);
    if (style >= 0) CHECK(d.nodes[style].type == NP_OX_NODE_STYLE);
    /* 修订 ins */
    CHECK(count_type(&d, NP_OX_NODE_REVISION) >= 1);
    /* 批注引用 */
    CHECK(count_type(&d, NP_OX_NODE_COMMENT) >= 1);
    /* 节属性 */
    CHECK(count_type(&d, NP_OX_NODE_SECTION) >= 1);
    /* 文本抽取总量 > 0 */
    CHECK(d.ntext_chars > 0);

    /* 结构路径带部件与偏移定位 */
    int tbl = -1;
    for (int i = 0; i < d.nnodes; i++)
        if (d.nodes[i].type == NP_OX_NODE_TABLE) { tbl = i; break; }
    CHECK(tbl >= 0);
    if (tbl >= 0) {
        CHECK(strstr(d.nodes[tbl].path, "w:tbl[0]") != NULL);
        CHECK(d.nodes[tbl].part_index >= 0);
        CHECK(d.nodes[tbl].part_offset > 0);
    }
    return 0;
}

static int test_xlsx(void) {
    printf("== F2: golden.xlsx (Excel) ==\n");
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file("tests/golden/golden.xlsx", &d);
    CHECK(st == NP_OX_OK);
    CHECK(d.kind == NP_OX_EXCEL);
    CHECK(strcmp(d.main_part, "xl/workbook.xml") == 0);
    CHECK(count_type(&d, NP_OX_NODE_SHEET) == 2);
    int s1 = find_label(&d, "sheet");
    CHECK(s1 >= 0 || count_type(&d, NP_OX_NODE_SHEET) == 2);
    return 0;
}

static int test_reject_encrypted(void) {
    printf("== F2: encrypted.docx 拒绝 ==\n");
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file("tests/golden/encrypted.docx", &d);
    CHECK(st == NP_OX_ERR_ENCRYPTED);
    CHECK(d.encrypted == true);
    CHECK(strcmp(np_ox_status_name(st), "加密包不支持") == 0);
    return 0;
}

static int test_reject_not_opc(void) {
    printf("== F2: plain.zip 非 OPC 拒绝 ==\n");
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file("tests/golden/plain.zip", &d);
    CHECK(st == NP_OX_ERR_NOT_OPC);
    return 0;
}

static int test_malformed(void) {
    printf("== F2: 畸形/边界输入 ==\n");
    np_ox_doc_t d;
    /* Null / 空 */
    CHECK(np_ox_parse_mem(NULL, 0, &d) == NP_OX_ERR_NOT_ZIP);
    unsigned char tiny[] = { 'P','K',3,4 };
    CHECK(np_ox_parse_mem(tiny, sizeof tiny, &d) == NP_OX_ERR_NOT_ZIP);
    /* 不存在文件 */
    CHECK(np_ox_parse_file("tests/golden/__no_such__.docx", &d) == NP_OX_ERR_OPEN);
    /* 参数 */
    CHECK(np_ox_parse_file(NULL, &d) == NP_OX_ERR_ARG);
    CHECK(np_ox_parse_mem(tiny, sizeof tiny, NULL) == NP_OX_ERR_ARG);
    /* 子节点列举 */
    CHECK(np_ox_children(&d, -1, NULL, 0) == 0);
    return 0;
}

static int test_children(void) {
    printf("== F2: 结构树子节点关系 ==\n");
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file("tests/golden/golden.docx", &d);
    CHECK(st == NP_OX_OK);
    int roots = np_ox_children(&d, -1, NULL, 0); /* cap<=0 安全 */
    CHECK(roots == 0);
    int buf[64];
    int n = np_ox_children(&d, -1, buf, 64);
    CHECK(n == d.nparts);  /* 所有部件是根节点 */
    for (int i = 0; i < n; i++) CHECK(d.nodes[buf[i]].type == NP_OX_NODE_PART);
    return 0;
}

int main(void) {
    printf("F2 OOXML 结构树单元测试\n");
    test_docx();
    test_xlsx();
    test_reject_encrypted();
    test_reject_not_opc();
    test_malformed();
    test_children();
    if (failures) { printf("\n%d failure(s)\n", failures); return 1; }
    printf("\nALL F2 TESTS PASSED\n");
    return 0;
}
