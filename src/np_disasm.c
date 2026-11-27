/* np_disasm.c - R2 multi-architecture disassembly view substrate. */
#include "np_disasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_DIS_MAX_INSNS 8192

struct np_disasm_view {
    np_disasm_insn_t *insns;
    size_t count, cap;
    np_disasm_decoder_t decoders[8];
    size_t decoder_count;
    np_disasm_syntax_t syntax;
    np_disasm_armmode_t armmode;
    np_disasm_source_t source;
};

const char *np_disasm_source_name(np_disasm_source_t s) {
    switch (s) {
        case NP_DSRC_CAPSTONE: return "Capstone";
        case NP_DSRC_OBJDUMP:  return "objdump";
        case NP_DSRC_TEST:     return "测试注入";
        default:               return "未知";
    }
}

np_disasm_view_t *np_disasm_view_create(void) {
    np_disasm_view_t *v = (np_disasm_view_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->syntax = NP_DSYNTAX_INTEL;
    v->armmode = NP_DMODE_ARM;
    return v;
}

void np_disasm_view_destroy(np_disasm_view_t *v) {
    if (!v) return;
    free(v->insns);
    free(v);
}

void np_disasm_view_clear(np_disasm_view_t *v) {
    if (!v) return;
    v->count = 0;
    v->source = NP_DSRC_NONE;
}

void np_disasm_view_set_decoders(np_disasm_view_t *v,
                                 const np_disasm_decoder_t *d, size_t n) {
    if (!v) return;
    v->decoder_count = 0;
    if (!d) return;
    for (size_t i = 0; i < n && i < 8; ++i) {
        if (!d[i].fn) continue;
        v->decoders[v->decoder_count++] = d[i];
    }
}

void np_disasm_view_set_syntax(np_disasm_view_t *v, np_disasm_syntax_t s) {
    if (v) v->syntax = s;
}
void np_disasm_view_set_armmode(np_disasm_view_t *v, np_disasm_armmode_t m) {
    if (v) v->armmode = m;
}
np_disasm_syntax_t np_disasm_view_syntax(const np_disasm_view_t *v) {
    return v ? v->syntax : NP_DSYNTAX_INTEL;
}
np_disasm_armmode_t np_disasm_view_armmode(const np_disasm_view_t *v) {
    return v ? v->armmode : NP_DMODE_ARM;
}

bool np_disasm_decode(np_disasm_view_t *v, const unsigned char *data,
                      size_t data_len, size_t off, size_t len,
                      const np_probe_result_t *probe,
                      char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    np_disasm_view_clear(v);
    if (!v || !data || !probe) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return false;
    }
    /* Confidence gate: never decode with an unconfirmed architecture. */
    if (probe->confidence < NP_DISASM_MIN_CONFIDENCE ||
        probe->prog.arch == NP_PARCH_UNKNOWN) {
        if (err && err_cap)
            snprintf(err, err_cap, "架构未确认（置信度 %d），请先选择架构",
                     probe->confidence);
        return false;
    }
    if (off > data_len || len > data_len - off) {
        if (err && err_cap) snprintf(err, err_cap, "反汇编范围越界");
        return false;
    }
    if (v->decoder_count == 0) {
        if (err && err_cap) snprintf(err, err_cap, "无可用反汇编器（Capstone/objdump 未就绪）");
        return false;
    }
    if (v->count + NP_DIS_MAX_INSNS > v->cap) {
        size_t nc = v->cap ? v->cap : 256;
        while (nc < v->count + NP_DIS_MAX_INSNS && nc < NP_DIS_MAX_INSNS * 2) nc *= 2;
        np_disasm_insn_t *p = (np_disasm_insn_t *)realloc(v->insns, nc * sizeof(*p));
        if (!p) {
            if (err && err_cap) snprintf(err, err_cap, "内存不足");
            return false;
        }
        v->insns = p; v->cap = nc;
    }

    /* Fallback chain: first decoder to yield at least one instruction wins. */
    for (size_t d = 0; d < v->decoder_count; ++d) {
        const np_disasm_decoder_t *dec = &v->decoders[d];
        int got = dec->fn(data + off, off, len, probe->prog.arch,
                          v->syntax, v->armmode, probe->prog.image_base,
                          v->insns, NP_DIS_MAX_INSNS, dec->user);
        if (got > 0) {
            v->count = (size_t)got < NP_DIS_MAX_INSNS ? (size_t)got : NP_DIS_MAX_INSNS;
            for (size_t i = 0; i < v->count; ++i)
                v->insns[i].source = dec->source;
            v->source = dec->source;
            return true;
        }
    }
    if (err && err_cap) snprintf(err, err_cap, "所有反汇编器均未能解码该范围");
    return false;
}

size_t np_disasm_count(const np_disasm_view_t *v) { return v ? v->count : 0; }
const np_disasm_insn_t *np_disasm_at(const np_disasm_view_t *v, size_t i) {
    return (v && i < v->count) ? &v->insns[i] : NULL;
}
np_disasm_source_t np_disasm_source(const np_disasm_view_t *v) {
    return v ? v->source : NP_DSRC_NONE;
}

int np_disasm_insn_for_offset(const np_disasm_view_t *v, size_t off) {
    if (!v) return -1;
    for (size_t i = 0; i < v->count; ++i) {
        const np_disasm_insn_t *ins = &v->insns[i];
        if (off >= ins->offset && off < ins->offset + ins->size) return (int)i;
    }
    return -1;
}

size_t np_disasm_offset_for_insn(const np_disasm_view_t *v, size_t i) {
    if (!v || i >= v->count) return (size_t)-1;
    return v->insns[i].offset;
}

/* -------- objdump text parser -------- */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool np_disasm_parse_objdump_line(const char *line, np_disasm_insn_t *ins) {
    if (!line || !ins) return false;
    memset(ins, 0, sizeof(*ins));
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    /* Optional "401000:" address prefix. */
    if (*p && hex_val(*p) >= 0) {
        uint64_t addr = 0;
        int digits = 0;
        while (*p && hex_val(*p) >= 0) {
            addr = (addr << 4) | (uint64_t)hex_val(*p);
            p++; digits++;
        }
        if (*p == ':' && digits > 0) {
            ins->addr = addr;
            p++;
            while (*p == ' ' || *p == '\t') p++;
        } else {
            /* Ambiguous token: treat as start of bytes/mnemonic only if it is
             * not followed by ':' — otherwise reject (avoid garbage text). */
            p = line;
            while (*p == ' ' || *p == '\t') p++;
        }
    }

    /* Byte column. objdump prints two styles:
     *   x86:              "48 89 5c 24 08"  (space-separated 1-2 digit tokens)
     *   fixed-width ISAs: "e59f0000"        (one contiguous 4/8/16-digit group)
     * A token is a maximal run of hex digits whose length is a supported byte
     * width (1/2/4/8/16) AND that is followed by whitespace/end. A mnemonic
     * such as "call"/"addi.d" fails that rule (odd length or trailing letter)
     * and therefore terminates the byte column naturally. A tab after a byte
     * token is objdump's column separator: the rest is the mnemonic column. */
    size_t nbytes = 0;
    while (*p) {
        const char *hs = p;
        size_t hn = 0;
        while (hs[hn] && hex_val(hs[hn]) >= 0) hn++;
        char after = hn ? hs[hn] : 0;
        if (!hn || (after && after != ' ' && after != '\t')) break;
        if (!(hn == 1 || hn == 2 || hn == 4 || hn == 8 || hn == 16)) break;
        for (size_t k = 0; k < hn && nbytes < 16; k += 2)
            ins->size++;                /* one byte per hex pair (odd 1-digit = 1) */
        p = hs + hn;
        if (*p == '\t') { p++; break; } /* column separator -> mnemonic column */
        while (*p == ' ' || *p == '\t') p++;
    }
    if (!ins->size) return false;

    /* Remainder is mnemonic + operands. Trim leading spaces; reject empty.
     * A leading '<' means this is a symbol label line ("0000 <main>:"), not
     * an instruction — reject it so label headers never enter the listing. */
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '<') return false;
    size_t tl = strlen(p);
    while (tl && (p[tl - 1] == '\n' || p[tl - 1] == '\r')) tl--;  /* strip EOL */
    if (!tl) return false;
    if (tl >= sizeof ins->text) tl = sizeof ins->text - 1;
    memcpy(ins->text, p, tl);
    ins->text[tl] = 0;
    ins->source = NP_DSRC_OBJDUMP;
    return true;
}
