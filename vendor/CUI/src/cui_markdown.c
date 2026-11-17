/* Markdown view: a CommonMark-ish subset rendered as native content.
 *
 * The text is parsed every frame straight from the caller's buffer. Blocks are
 * found line by line (containers strip their prefixes into new line lists that
 * still point into the text), inline styles come from a delimiter stack, and
 * each paragraph is wrapped by hand, since one line mixes fonts, into a single
 * painted node with an invisible hit box per link. Code blocks and tables are
 * drawn by cui_code_block and cui_table. Memory comes from the frame arena. */
#include "cui_internal.h"


#define MD_MAX_DEPTH 12      /* containers nested deeper show as text */
#define MD_WINDOW 64         /* how far back a closing * or _ looks for its opener */
#define MD_MAX_RUN 64        /* longer runs of * _ ~ are plain text */
#define MD_MAX_TICKS 32      /* longer runs of backticks are plain text */
#define MD_MAX_COLS 32       /* table columns beyond this are dropped */
#define MD_SHEAR 0.2f        /* slant of the synthesized italic (about 11 degrees) */

/* Local extension: math ($...$ / $$...$$) and Mermaid flowcharts. All buffers
 * are frame-allocated and size-capped; malformed input degrades to plain text. */
#define MD_MM_MAX_NODES  64
#define MD_MM_MAX_EDGES  128
#define MD_MM_ID_CAP     40
#define MD_MM_LABEL_CAP  128
#define MD_MM_EDGE_CAP   64

/* ------------------------------------------------------------------------ */
/* Lines                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct { const char *s; int len, off; } md_line;   /* off: byte offset of s in the text */

static bool md_sp(char c) { return c == ' ' || c == '\t'; }

static int md_indent(md_line L, int *bytes) {
    int col = 0, i = 0;
    while (i < L.len && md_sp(L.s[i])) { col = L.s[i] == '\t' ? (col / 4 + 1) * 4 : col + 1; i++; }
    if (bytes) *bytes = i;
    return col;
}
static bool md_blank(md_line L) {
    for (int i = 0; i < L.len; i++) if (!md_sp(L.s[i])) return false;
    return true;
}
static md_line md_skip(md_line L, int bytes) {
    if (bytes > L.len) bytes = L.len;
    L.s += bytes; L.len -= bytes; L.off += bytes;
    return L;
}
static md_line md_strip_cols(md_line L, int cols) {
    int col = 0, i = 0;
    while (i < L.len && col < cols && md_sp(L.s[i])) { col = L.s[i] == '\t' ? (col / 4 + 1) * 4 : col + 1; i++; }
    return md_skip(L, i);
}
static md_line md_trim(md_line L) {
    int b;
    md_indent(L, &b);
    L = md_skip(L, b);
    while (L.len > 0 && md_sp(L.s[L.len - 1])) L.len--;
    return L;
}
static bool md_has(md_line L, char c) { return L.len > 0 && memchr(L.s, c, (size_t)L.len) != NULL; }

/* ------------------------------------------------------------------------ */
/* Inline parsing                                                           */
/* ------------------------------------------------------------------------ */

enum { MS_STRONG = 1, MS_EM = 2, MS_STRIKE = 4, MS_CODE = 8, MS_BREAK = 16, MS_MATH = 32 };
typedef struct { const char *p; int len; uint8_t style; int link; } md_run;
typedef struct { const char *url; int len; bool www, pending, autolink; } md_link;
typedef struct { md_run *runs; int nruns; md_link *links; int nlinks; } md_inline;

enum { TK_TXT, TK_DELIM, TK_CODE, TK_BR, TK_LOPEN, TK_LCLOSE, TK_AUTO, TK_MATH };
typedef struct {
    const char *p; int len;
    uint8_t kind, ch, can_open, can_close, hidden, image;
    int16_t count, orig;
    int8_t pre[3], post[3];   /* strong, em, strike: closes before / opens after this token */
    int link;
} md_tok;

static bool md_ws(unsigned char c) { return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool md_punct(unsigned char c) { return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
static bool md_url_lead(unsigned char c) { return md_ws(c) || c == '(' || c == '*' || c == '_' || c == '~' || c == '"' || c == '\''; }

typedef struct { md_tok *t; int n, cap; } md_toks;
static md_tok *md_add(md_toks *T, int kind, const char *p, int len) {
    if (T->n >= T->cap) return NULL;
    md_tok *t = &T->t[T->n++];
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind; t->p = p; t->len = len; t->link = -1;
    return t;
}

/* (destination "title") after a ]. Returns 1 and the end, 0 when it is not a
 * link, -1 when the text ends first. */
static int md_link_dest(const char *s, int n, int at, const char **url, int *ulen, int *end) {
    if (at >= n) return -1;
    if (s[at] != '(') return 0;
    int j = at + 1;
    while (j < n && md_ws((unsigned char)s[j])) j++;
    if (j >= n) return -1;
    int us = j, ue;
    if (s[j] == '<') {
        us = ++j;
        while (j < n && s[j] != '>' && s[j] != '\n') { if (s[j] == '\\' && j + 1 < n) j++; j++; }
        if (j >= n) return -1;
        if (s[j] != '>') return 0;
        ue = j++;
    } else {
        int depth = 0;
        while (j < n && !md_ws((unsigned char)s[j])) {
            if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
            if (s[j] == '(') { if (++depth > 32) return 0; }
            else if (s[j] == ')') { if (depth == 0) break; depth--; }
            j++;
        }
        if (j >= n) return -1;
        ue = j;
    }
    while (j < n && md_ws((unsigned char)s[j])) j++;
    if (j >= n) return -1;
    if (s[j] == '"' || s[j] == '\'' || s[j] == '(') {
        char q = s[j] == '(' ? ')' : s[j];
        j++;
        while (j < n && s[j] != q) { if (s[j] == '\\' && j + 1 < n) j++; j++; }
        if (j >= n) return -1;
        j++;
        while (j < n && md_ws((unsigned char)s[j])) j++;
        if (j >= n) return -1;
    }
    if (s[j] != ')') return 0;
    *url = s + us; *ulen = ue - us; *end = j + 1;
    return 1;
}

/* <scheme:...> autolink: index of the closing >, or -1 */
static int md_autolink(const char *s, int n, int i) {
    int j = i + 1, k = 0;
    if (j >= n || !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) return -1;
    while (j < n && k < 32 && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9') ||
                               s[j] == '+' || s[j] == '.' || s[j] == '-')) { j++; k++; }
    if (k < 2 || j >= n || s[j] != ':') return -1;
    while (j < n && s[j] != '>' && s[j] != '<' && !md_ws((unsigned char)s[j])) j++;
    return j < n && s[j] == '>' ? j : -1;
}

/* <br>, <br/>, <br />: end of the tag, or -1 */
static int md_br_tag(const char *s, int n, int i) {
    if (i + 3 >= n || (s[i + 1] != 'b' && s[i + 1] != 'B') || (s[i + 2] != 'r' && s[i + 2] != 'R')) return -1;
    int j = i + 3;
    while (j < n && s[j] == ' ') j++;
    if (j < n && s[j] == '/') j++;
    return j < n && s[j] == '>' ? j + 1 : -1;
}

/* bare https://, http:// and www. links: end of the URL, or i */
static int md_bare_url(const char *s, int n, int i, bool *www) {
    int pre = 0;
    if (n - i > 8 && !strncmp(s + i, "https://", 8)) pre = 8;
    else if (n - i > 7 && !strncmp(s + i, "http://", 7)) pre = 7;
    else if (n - i > 4 && !strncmp(s + i, "www.", 4)) { pre = 4; *www = true; }
    if (!pre) return i;
    int e = i + pre;
    while (e < n && !md_ws((unsigned char)s[e]) && s[e] != '<') e++;
    while (e > i + pre && strchr("?!.,:*_~'\";", s[e - 1])) e--;
    if (e > i + pre && s[e - 1] == ')') {
        int open = 0, close = 0;
        for (int k = i; k < e; k++) { if (s[k] == '(') open++; else if (s[k] == ')') close++; }
        if (close > open) e--;
    }
    return e > i + pre ? e : i;
}

static void md_emit(md_run *runs, int *nr, const char *p, int len, int style, int link) {
    if (len <= 0 && !(style & MS_BREAK)) return;
    if (*nr > 0 && !(style & MS_BREAK)) {
        md_run *q = &runs[*nr - 1];
        if (q->style == style && q->link == link && q->p + q->len == p) { q->len += len; return; }
    }
    md_run *r = &runs[(*nr)++];
    r->p = p; r->len = len; r->style = (uint8_t)style; r->link = link;
}

/* ---- TeX subset -> readable Unicode ---------------------------------- */
/* The preview is a native CUI surface, not a browser: we intentionally map
 * common TeX to Unicode rather than pulling in a typesetting engine. */
typedef struct { const char *tex; const char *uni; } md_sym;
/* Every mapping below was checked against the cmap of the bundled Inter face
 * and the CJK fallback (msyh). A codepoint neither face carries would render
 * as a blank box, so those commands fall back to readable ASCII instead. */
static const md_sym md_symbols[] = {
    /* arrows */
    { "rightarrow", "→" }, { "leftarrow", "←" }, { "leftrightarrow", "↔" },
    { "uparrow", "↑" }, { "downarrow", "↓" }, { "nearrow", "↗" }, { "searrow", "↘" },
    { "to", "→" }, { "longrightarrow", "→" }, { "longleftarrow", "←" },
    { "longleftrightarrow", "↔" }, { "mapsto", "|->" }, { "longmapsto", "|->" },
    { "Rightarrow", "=>" }, { "Leftarrow", "<=" }, { "Leftrightarrow", "<=>" },
    { "Uparrow", "=>" }, { "Downarrow", "<=" }, { "Updownarrow", "<=>" },
    { "hookleftarrow", "<-" }, { "hookrightarrow", "->" },
    /* operators */
    { "otimes", "×" }, { "oplus", "⊕" }, { "ominus", "-" }, { "oslash", "ø" },
    { "odot", "⊙" }, { "circ", "o" }, { "bullet", "•" }, { "cdot", "·" },
    { "centerdot", "·" }, { "ast", "*" }, { "star", "*" }, { "diamond", "◇" },
    { "times", "×" }, { "div", "÷" }, { "pm", "±" }, { "mp", "∓" },
    { "dagger", "†" }, { "ddagger", "‡" }, { "wedge", "∧" }, { "vee", "∨" },
    /* relations */
    { "leq", "≤" }, { "le", "≤" }, { "geq", "≥" }, { "ge", "≥" },
    { "neq", "≠" }, { "ne", "≠" }, { "approx", "≈" }, { "simeq", "≈" },
    { "cong", "≈" }, { "equiv", "≡" }, { "sim", "~" }, { "propto", "∝" },
    { "asymp", "≈" }, { "doteq", "=" }, { "prec", "<" }, { "succ", ">" },
    { "preceq", "≤" }, { "succeq", "≥" }, { "ll", "<<" }, { "gg", ">>" },
    { "perp", "⊥" }, { "parallel", "∥" }, { "nparallel", "!∥" },
    { "mid", "∣" }, { "nmid", "!∣" }, { "angle", "∠" }, { "measuredangle", "∠" },
    { "models", "|=" }, { "vDash", "|=" }, { "vdash", "|-" }, { "dashv", "-|" },
    { "top", "T" }, { "bot", "⊥" },
    { "forall", "for all" }, { "exists", "exists" }, { "nexists", "not exists" },
    { "neg", "¬" }, { "lnot", "¬" }, { "land", "∧" }, { "lor", "∨" },
    { "therefore", "∴" }, { "because", "∵" }, { "checkmark", "✓" },
    /* sets */
    { "in", "∈" }, { "notin", "!∈" }, { "ni", ">-" },
    { "subset", "<=" }, { "subseteq", "<=" }, { "supset", ">=" },
    { "supseteq", ">=" }, { "nsubseteq", "!<=" }, { "nsupseteq", "!>=" },
    { "emptyset", "{}" }, { "varnothing", "{}" }, { "cup", "∪" }, { "cap", "∩" },
    { "sqcup", "∪" }, { "sqcap", "∩" }, { "uplus", "∪" }, { "veebar", "⊕" },
    /* dots */
    { "ldots", "…" }, { "dots", "…" }, { "cdots", "..." }, { "dotsb", "..." },
    { "dotsc", "..." }, { "dotso", "..." }, { "vdots", ":" }, { "ddots", ".." },
    /* letter-like */
    { "hbar", "ħ" }, { "ell", "ℓ" }, { "aleph", "aleph" }, { "beth", "beth" },
    { "Re", "Re" }, { "Im", "Im" },
    { "partial", "∂" }, { "nabla", "grad" }, { "infty", "∞" },
    { "prime", "′" }, { "dprime", "″" }, { "deg", "deg" },
    /* big operators */
    { "sum", "∑" }, { "prod", "∏" }, { "int", "∫" }, { "iint", "∫∫" },
    { "iiint", "∫∫∫" }, { "oint", "∮" },
    /* greek (rendered through the CJK fallback face) */
    { "alpha", "α" }, { "beta", "β" }, { "gamma", "γ" }, { "delta", "δ" },
    { "epsilon", "ϵ" }, { "varepsilon", "ε" }, { "zeta", "ζ" }, { "eta", "η" },
    { "theta", "θ" }, { "vartheta", "ϑ" }, { "iota", "ι" }, { "kappa", "κ" },
    { "varkappa", "ϰ" }, { "lambda", "λ" }, { "mu", "μ" }, { "nu", "ν" },
    { "xi", "ξ" }, { "omicron", "ο" }, { "pi", "π" }, { "rho", "ρ" },
    { "varrho", "ϱ" }, { "sigma", "σ" }, { "varsigma", "ς" }, { "tau", "τ" },
    { "upsilon", "υ" }, { "phi", "ϕ" }, { "varphi", "φ" }, { "chi", "χ" },
    { "psi", "ψ" }, { "omega", "ω" },
    { "Gamma", "Γ" }, { "Delta", "Δ" }, { "Theta", "Θ" }, { "Lambda", "Λ" },
    { "Xi", "Ξ" }, { "Pi", "Π" }, { "Sigma", "Σ" }, { "Upsilon", "Υ" },
    { "Phi", "Φ" }, { "Psi", "Ψ" }, { "Omega", "Ω" },
    /* named functions */
    { "sin", "sin" }, { "cos", "cos" }, { "tan", "tan" }, { "csc", "csc" },
    { "sec", "sec" }, { "cot", "cot" }, { "sinh", "sinh" }, { "cosh", "cosh" },
    { "tanh", "tanh" }, { "coth", "coth" }, { "arcsin", "arcsin" },
    { "arccos", "arccos" }, { "arctan", "arctan" }, { "exp", "exp" },
    { "log", "log" }, { "ln", "ln" }, { "lg", "lg" }, { "lim", "lim" },
    { "limsup", "lim sup" }, { "liminf", "lim inf" }, { "max", "max" },
    { "min", "min" }, { "sup", "sup" }, { "inf", "inf" }, { "det", "det" },
    { "dim", "dim" }, { "gcd", "gcd" }, { "hom", "hom" }, { "ker", "ker" },
    { "arg", "arg" }, { "Pr", "Pr" }, { "bmod", " mod " },
    /* misc */
    { "S", "§" }, { "P", "¶" }, { "copyright", "©" }, { "backslash", "\\" },
    { "Vert", "‖" }, { "vert", "|" }, { "langle", "<" }, { "rangle", ">" },
    { "lfloor", "[" }, { "rfloor", "]" }, { "lceil", "<" }, { "rceil", ">" },
};

static void md_put(char *out, int *o, int cap, const char *s, int n) {
    if (!s) return;
    if (n < 0) n = (int)strlen(s);
    if (n <= 0 || *o >= cap - 1) return;
    if (*o + n > cap - 1) n = cap - 1 - *o;
    if (n > 0) { memcpy(out + *o, s, (size_t)n); *o += n; }
}

static int md_group_at(const char *s, int n, int i, int *bs, int *bl, int *end) {
    if (i >= n || s[i] != '{') return 0;
    int depth = 0;
    for (int j = i; j < n; j++) {
        if (s[j] == '{') depth++;
        else if (s[j] == '}') {
            if (--depth == 0) { *bs = i + 1; *bl = j - i - 1; *end = j + 1; return 1; }
        }
    }
    return 0;
}

static bool md_cmd_at(const char *s, int n, int i, const char *cmd) {
    int k = (int)strlen(cmd);
    if (i + k > n || memcmp(s + i, cmd, (size_t)k)) return false;
    return i + k >= n || !((s[i + k] >= 'a' && s[i + k] <= 'z') || (s[i + k] >= 'A' && s[i + k] <= 'Z'));
}

static bool md_group_complex(const char *s, int n) {
    for (int i = 0; i < n; i++)
        if (md_ws((unsigned char)s[i]) || strchr("+-=*/<>", s[i])) return true;
    return false;
}

static const char *md_sup_char(unsigned char c) {
    switch (c) {
    case '0': return "⁰"; case '1': return "¹"; case '2': return "²"; case '3': return "³";
    case '4': return "⁴"; case '5': return "⁵"; case '6': return "⁶"; case '7': return "⁷";
    case '8': return "⁸"; case '9': return "⁹"; case '+': return "⁺"; case '-': return "⁻";
    case '=': return "⁼"; case '(': return "⁽"; case ')': return "⁾"; case 'n': return "ⁿ";
    case 'i': return "ⁱ"; case 'a': return "ᵃ"; case 'b': return "ᵇ"; case 'c': return "ᶜ";
    case 'd': return "ᵈ"; case 'e': return "ᵉ"; case 'f': return "ᶠ"; case 'g': return "ᵍ";
    case 'h': return "ʰ"; case 'j': return "ʲ"; case 'k': return "ᵏ"; case 'l': return "ˡ";
    case 'm': return "ᵐ"; case 'o': return "ᵒ"; case 'p': return "ᵖ"; case 'r': return "ʳ";
    case 's': return "ˢ"; case 't': return "ᵗ"; case 'u': return "ᵘ"; case 'v': return "ᵛ";
    case 'w': return "ʷ"; case 'x': return "ˣ"; case 'y': return "ʸ"; case 'z': return "ᶻ";
    default: return NULL;
    }
}

static char *md_math_translate(cui_ctx *ui, const char *s, int n);

/* ---- math IR --------------------------------------------------------------
 * The TeX subset is parsed once into a small item tree. Inline math flattens
 * the tree back to the same Unicode approximation as before, while display
 * math ($$ blocks) is measured and painted from the tree: stacked fractions,
 * raised/lowered scripts, sqrt overlines, spanning matrix delimiters. */
enum { MT_TXT, MT_GAP, MT_FRAC, MT_SQRT, MT_SCRIPT, MT_BINOM, MT_MAT,
       MT_ROWBR, MT_COLBR };

typedef struct md_mit md_mit;
struct md_mit {
    int kind;
    char buf[160]; int tn;       /* MT_TXT: UTF-8 run; MT_GAP: space count */
    md_mit *a, *b, *c;           /* num/den, sqrt arg, base/sup/sub */
    md_mit **cell;               /* MT_MAT: cell chains, [row * ncol + col] */
    int *ncr;                    /* columns actually used per row */
    int nrow, ncol;
    const char *dl, *dr;         /* matrix delimiters ("" = none) */
    md_mit *next;
    float w, as, de;             /* measured box around the baseline */
    float *rh, *mxa, *cw, mh;    /* MT_MAT row/column metrics */
    int lruns;                   /* MT_TXT: letter runs (italic shear) */
};

typedef struct { md_mit **it; int n; float w, as, de; } md_mline;
typedef struct {
    md_mline *line; int nline;
    float fs, u, lh, w, h;
    cui_color fg, bg, border;
} md_mpaint;

static void md_shear(cui_ctx *ui, int v0, float base);   /* defined below */
static char *md_m_flat(cui_ctx *ui, const md_mit *it, int cap);
static float md_m_paint_list(cui_ctx *ui, md_mit *it, float x, float yb,
                             float fs, md_mpaint *p);

static md_mit *md_m_new(cui_ctx *ui, int kind) {
    md_mit *it = cui__frame_alloc(ui, sizeof *it);
    memset(it, 0, sizeof *it);
    it->kind = kind;
    return it;
}

static void md_m_push(md_mit **head, md_mit **tail, md_mit *it) {
    if (!*head) *head = it; else (*tail)->next = it;
    *tail = it;
}

/* Append text to the trailing TXT run, splitting at the buffer cap. */
static void md_m_text(cui_ctx *ui, md_mit **head, md_mit **tail,
                      const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    while (n > 0) {
        md_mit *t = *tail;
        if (!t || t->kind != MT_TXT || t->tn >= (int)sizeof t->buf - 1) {
            t = md_m_new(ui, MT_TXT);
            md_m_push(head, tail, t);
        }
        int k = CUI_MIN(n, (int)sizeof t->buf - 1 - t->tn);
        memcpy(t->buf + t->tn, s, (size_t)k);
        t->tn += k; s += k; n -= k;
    }
}

/* Detach the last cluster as a script base: a whole non-text item, else the
 * final UTF-8 character of the trailing TXT run. */
static md_mit *md_m_pop_base(cui_ctx *ui, md_mit **head, md_mit **tail) {
    md_mit *t = *tail;
    if (!t) return NULL;
    if (t->kind != MT_TXT || t->tn == 0) {
        if (*head == t) { *head = NULL; *tail = NULL; return t; }
        md_mit *p = *head;
        while (p && p->next != t) p = p->next;
        if (p) { p->next = NULL; *tail = p; }
        return t;
    }
    int k = t->tn - 1;
    while (k > 0 && (t->buf[k] & 0xC0) == 0x80) k--;
    md_mit *base = md_m_new(ui, MT_TXT);
    base->tn = t->tn - k;
    memcpy(base->buf, t->buf + k, (size_t)base->tn);
    t->tn = k;
    return base;
}

/* Letter runs get a synthesized italic; letters are ASCII plus the Greek
 * block (0xCE/0xCF lead bytes). Reports the full UTF-8 character length. */
static bool md_m_letter(const char *s, int n, int *len) {
    unsigned char c = (unsigned char)s[0];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { *len = 1; return true; }
    int l = 1 + ((c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 :
                 (c & 0xF8) == 0xF0 ? 3 : 0);
    if (n >= 2 && (c == 0xCE || c == 0xCF)) { *len = 2; return true; }
    *len = CUI_MIN(l, n);
    return false;
}

static int md_m_letter_runs(const char *s, int n) {
    int runs = 0, i = 0;
    while (i < n) {
        int cl; bool let = md_m_letter(s + i, n - i, &cl);
        int j = i;
        while (j < n) {
            int l2; bool let2 = md_m_letter(s + j, n - j, &l2);
            if (let2 != let) break;
            j += l2;
        }
        if (let) runs++;
        i = j;
    }
    return runs;
}

/* Precomposed accented letters. The bundled faces carry Latin-1 and Latin
 * Extended-A; combining marks (U+0302 & co) are NOT covered, so we cannot
 * stack a diacritic onto an arbitrary base. kind: 0 hat, 1 tilde, 2 bar,
 * 8 grave, 9 acute; other kinds have no precomposed forms. */
static const char *md_accent_pre(char c, int kind) {
    switch (kind) {
    case 0:
        switch (c) {
        case 'a': return "â"; case 'e': return "ê"; case 'i': return "î";
        case 'o': return "ô"; case 'u': return "û"; case 'y': return "ŷ";
        case 'c': return "ĉ"; case 'g': return "ĝ"; case 'h': return "ĥ";
        case 'j': return "ĵ"; case 's': return "ŝ"; case 'w': return "ŵ";
        case 'A': return "Â"; case 'E': return "Ê"; case 'I': return "Î";
        case 'O': return "Ô"; case 'U': return "Û"; case 'Y': return "Ŷ";
        case 'C': return "Ĉ"; case 'G': return "Ĝ"; case 'H': return "Ĥ";
        case 'J': return "Ĵ"; case 'S': return "Ŝ"; case 'W': return "Ŵ";
        }
        break;
    case 1:
        switch (c) {
        case 'a': return "ã"; case 'n': return "ñ"; case 'o': return "õ";
        case 'i': return "ĩ"; case 'u': return "ũ";
        case 'A': return "Ã"; case 'N': return "Ñ"; case 'O': return "Õ";
        case 'I': return "Ĩ"; case 'U': return "Ũ";
        }
        break;
    case 2:
        switch (c) {
        case 'a': return "ā"; case 'e': return "ē"; case 'i': return "ī";
        case 'o': return "ō"; case 'u': return "ū";
        case 'A': return "Ā"; case 'E': return "Ē"; case 'I': return "Ī";
        case 'O': return "Ō"; case 'U': return "Ū";
        }
        break;
    case 8:
        switch (c) {
        case 'a': return "à"; case 'e': return "è"; case 'i': return "ì";
        case 'o': return "ò"; case 'u': return "ù";
        case 'A': return "À"; case 'E': return "È"; case 'I': return "Ì";
        case 'O': return "Ò"; case 'U': return "Ù";
        }
        break;
    case 9:
        switch (c) {
        case 'a': return "á"; case 'e': return "é"; case 'i': return "í";
        case 'o': return "ó"; case 'u': return "ú";
        case 'A': return "Á"; case 'E': return "É"; case 'I': return "Í";
        case 'O': return "Ó"; case 'U': return "Ú";
        }
        break;
    }
    return NULL;
}

/* Split a matrix environment body (brace-aware) into rows of cells:
 * \\ separates rows, & separates columns. */
static void md_m_matrix(cui_ctx *ui, md_mit *it, const char *s, int bs, int bl);
static char *md_m_flat(cui_ctx *ui, const md_mit *it, int cap);
static md_mit *md_math_ir(cui_ctx *ui, const char *s, int n);
static float md_m_paint_list(cui_ctx *ui, md_mit *it, float x, float yb, float fs, md_mpaint *p);

static void md_m_flat1(cui_ctx *ui, const md_mit *it, char *out, int *o, int cap) {
    for (; it; it = it->next) {
        switch (it->kind) {
        case MT_TXT: md_put(out, o, cap, it->buf, it->tn); break;
        case MT_GAP:
            for (int k = 0; k < it->tn && k < 8; k++) md_put(out, o, cap, " ", 1);
            break;
        case MT_ROWBR: md_put(out, o, cap, "\n", 1); break;
        case MT_COLBR: md_put(out, o, cap, "  ", 2); break;
        case MT_FRAC: {
            char *nu = md_m_flat(ui, it->a, cap);
            char *de = md_m_flat(ui, it->b, cap);
            int nn = (int)strlen(nu), nd = (int)strlen(de);
            bool pn = md_group_complex(nu, nn), pd = md_group_complex(de, nd);
            if (pn) md_put(out, o, cap, "(", 1);
            md_put(out, o, cap, nu, nn);
            if (pn) md_put(out, o, cap, ")", 1);
            md_put(out, o, cap, "/", 1);
            if (pd) md_put(out, o, cap, "(", 1);
            md_put(out, o, cap, de, nd);
            if (pd) md_put(out, o, cap, ")", 1);
            break;
        }
        case MT_SQRT: {
            md_put(out, o, cap, "√", -1);
            char *v = md_m_flat(ui, it->a, cap);
            int vn = (int)strlen(v);
            if (md_group_complex(v, vn)) {
                md_put(out, o, cap, "(", 1);
                md_put(out, o, cap, v, vn);
                md_put(out, o, cap, ")", 1);
            } else {
                md_put(out, o, cap, v, vn);
            }
            break;
        }
        case MT_SCRIPT: {
            md_m_flat1(ui, it->a, out, o, cap);
            if (it->b) {
                char *sv = md_m_flat(ui, it->b, cap);
                int sn = (int)strlen(sv);
                bool all = sn > 0 && sn <= 8;
                for (int k = 0; all && k < sn; k++)
                    if (!md_sup_char((unsigned char)sv[k])) all = false;
                if (all) {
                    for (int k = 0; k < sn; k++)
                        md_put(out, o, cap, md_sup_char((unsigned char)sv[k]), -1);
                } else {
                    md_put(out, o, cap, "^(", -1);
                    md_put(out, o, cap, sv, sn);
                    md_put(out, o, cap, ")", 1);
                }
            }
            if (it->c) {
                char *sv = md_m_flat(ui, it->c, cap);
                md_put(out, o, cap, "_(", -1);
                md_put(out, o, cap, sv, (int)strlen(sv));
                md_put(out, o, cap, ")", 1);
            }
            break;
        }
        case MT_BINOM: {
            char *a = md_m_flat(ui, it->a, cap);
            char *b = md_m_flat(ui, it->b, cap);
            md_put(out, o, cap, "C(", -1);
            md_put(out, o, cap, a, (int)strlen(a));
            md_put(out, o, cap, ", ", 2);
            md_put(out, o, cap, b, (int)strlen(b));
            md_put(out, o, cap, ")", 1);
            break;
        }
        case MT_MAT: {
            md_put(out, o, cap, it->dl, -1);
            for (int r = 0; r < it->nrow; r++) {
                for (int cc = 0; cc < it->ncr[r]; cc++) {
                    if (cc) md_put(out, o, cap, "  ", 2);
                    md_m_flat1(ui, it->cell[r * it->ncol + cc], out, o, cap);
                }
                if (r < it->nrow - 1) md_put(out, o, cap, "\n", 1);
            }
            md_put(out, o, cap, it->dr, -1);
            break;
        }
        }
    }
}

static char *md_m_flat(cui_ctx *ui, const md_mit *it, int cap) {
    char *out = cui__frame_alloc(ui, (size_t)cap);
    int o = 0;
    md_m_flat1(ui, it, out, &o, cap);
    out[o] = 0;
    return out;
}

/* Parse the TeX subset into an item tree. */
static md_mit *md_math_ir(cui_ctx *ui, const char *s, int n) {
    md_mit *head = NULL, *tail = NULL;
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d == '\\') {
                md_m_push(&head, &tail, md_m_new(ui, MT_ROWBR));
                i += 2; continue;
            }
            if (d == ',' || d == ';' || d == ':' || d == ' ') {
                md_mit *it = md_m_new(ui, MT_GAP); it->tn = 1;
                md_m_push(&head, &tail, it);
                i += 2; continue;
            }
            if (d == '!') { i += 2; continue; }        /* negative thin space */
            if (strchr("{}%&#$_|~^", d)) {             /* literal escapes */
                md_m_text(ui, &head, &tail, s + i + 1, 1);
                i += 2; continue;
            }
            if (d == 'q' && md_cmd_at(s, n, i, "\\quad")) {
                md_mit *it = md_m_new(ui, MT_GAP); it->tn = 2;
                md_m_push(&head, &tail, it);
                i += 5; continue;
            }
            if (d == 'q' && md_cmd_at(s, n, i, "\\qquad")) {
                md_mit *it = md_m_new(ui, MT_GAP); it->tn = 4;
                md_m_push(&head, &tail, it);
                i += 6; continue;
            }
            /* \big..\Bigg and \bigl..\Biggr are sizing hints; drop them. */
            if ((d == 'b' || d == 'B') && i + 2 < n &&
                (s[i + 2] == 'i' || s[i + 2] == 'I')) {
                int j = i + 1;
                if (s[j] == 'B') j++;
                if (j + 2 < n && s[j] == 'b' && s[j + 1] == 'i' && s[j + 2] == 'g') {
                    j += 3;
                    while (j < n && s[j] == 'g') j++;
                    if (j < n && (s[j] == 'l' || s[j] == 'm' || s[j] == 'r')) j++;
                    if (j < n && s[j] == '.') j++;
                    i = j; continue;
                }
            }
            /* \left/\right: keep the delimiter glyph instead of dropping it */
            if (md_cmd_at(s, n, i, "\\left") || md_cmd_at(s, n, i, "\\right")) {
                bool left = s[i + 1] == 'l';
                int j = i + (left ? 5 : 6);
                if (j >= n) { i = j; continue; }
                char c1 = s[j];
                if (c1 == '.') { i = j + 1; continue; }     /* invisible delim */
                if (c1 == '{' || c1 == '}') {
                    md_m_text(ui, &head, &tail, &c1, 1);
                    i = j + 1; continue;
                }
                if (c1 && strchr("()[]|/", c1)) {
                    md_m_text(ui, &head, &tail, &c1, 1);
                    i = j + 1; continue;
                }
                if (c1 == '\\') {
                    static const struct { const char *nm, *g; } dl2[] = {
                        { "langle", "<" }, { "rangle", ">" }, { "lvert", "|" },
                        { "rvert", "|" }, { "lVert", "‖" }, { "rVert", "‖" },
                        { "lfloor", "[" }, { "rfloor", "]" }, { "lceil", "[" },
                        { "rceil", "]" }, { "lbrace", "{" }, { "rbrace", "}" },
                    };
                    int matched = -1, ml = 0;
                    for (int k = 0; k < (int)CUI_COUNTOF(dl2); k++) {
                        int sl = (int)strlen(dl2[k].nm);
                        if (j + 1 + sl <= n &&
                            !memcmp(s + j + 1, dl2[k].nm, (size_t)sl) && sl > ml) {
                            matched = k; ml = sl;
                        }
                    }
                    if (matched >= 0) {
                        md_m_text(ui, &head, &tail, dl2[matched].g, -1);
                        i = j + 1 + ml; continue;
                    }
                }
                i = j; continue;
            }
            if (md_cmd_at(s, n, i, "\\begin") || md_cmd_at(s, n, i, "\\end")) {
                bool beg = s[i + 1] == 'b';
                if (!beg) {                                /* stray \end */
                    int bs, bl, end;
                    if (md_group_at(s, n, i + 4, &bs, &bl, &end)) i = end;
                    else i += 4;
                    continue;
                }
                int j = i + 6, bs, bl, end;
                if (!md_group_at(s, n, j, &bs, &bl, &end)) { i = j; continue; }
                static const struct { const char *nm, *dl, *dr; } envs[] = {
                    { "pmatrix", "(", ")" }, { "bmatrix", "[", "]" },
                    { "Bmatrix", "{", "}" }, { "vmatrix", "|", "|" },
                    { "Vmatrix", "‖", "‖" }, { "cases", "{", "" },
                    { "dcases", "{", "" }, { "matrix", "", "" },
                    { "align", "", "" }, { "aligned", "", "" },
                    { "gather", "", "" }, { "gathered", "", "" },
                    { "array", "", "" }, { "smallmatrix", "", "" },
                };
                const char *dl = NULL, *dr = "";
                for (int k = 0; k < (int)CUI_COUNTOF(envs); k++) {
                    if (bl == (int)strlen(envs[k].nm) &&
                        !memcmp(s + bs, envs[k].nm, (size_t)bl)) {
                        dl = envs[k].dl; dr = envs[k].dr; break;
                    }
                }
                if (!dl) { i = end; continue; }            /* unknown env */
                if (bl == 5 && !memcmp(s + bs, "array", 5)) {
                    int bs2, bl2, end2;                    /* skip column spec */
                    if (md_group_at(s, n, end, &bs2, &bl2, &end2)) end = end2;
                }
                int bend = end, eend = end;                /* find \end{env} */
                for (int k = end; k + 4 <= n; k++) {
                    if (s[k] == '\\' && !memcmp(s + k, "\\end", 4)) {
                        int bs3, bl3, end3;
                        if (md_group_at(s, n, k + 4, &bs3, &bl3, &end3) &&
                            bl3 == bl && !memcmp(s + bs3, s + bs, (size_t)bl)) {
                            bend = k; eend = end3;
                        }
                        break;
                    }
                }
                md_mit *it = md_m_new(ui, MT_MAT);
                it->dl = dl; it->dr = dr;
                md_m_matrix(ui, it, s, end, bend - end);
                md_m_push(&head, &tail, it);
                i = eend; continue;
            }
            if (md_cmd_at(s, n, i, "\\binom")) {
                int j = i + 6, bs, bl, end;
                if (!md_group_at(s, n, j, &bs, &bl, &end)) { i += 2; continue; }
                md_mit *top = md_math_ir(ui, s + bs, bl);
                j = end;
                if (!md_group_at(s, n, j, &bs, &bl, &end)) { i += 2; continue; }
                md_mit *bot = md_math_ir(ui, s + bs, bl);
                md_mit *it = md_m_new(ui, MT_BINOM);
                it->a = top; it->b = bot;
                md_m_push(&head, &tail, it);
                i = end; continue;
            }
            if (md_cmd_at(s, n, i, "\\frac") || md_cmd_at(s, n, i, "\\dfrac") ||
                md_cmd_at(s, n, i, "\\tfrac")) {
                int j = i + (s[i + 1] == 'f' ? 5 : 6), bs, bl, end;
                if (!md_group_at(s, n, j, &bs, &bl, &end)) { i += 2; continue; }
                md_mit *num = md_math_ir(ui, s + bs, bl);
                j = end;
                if (!md_group_at(s, n, j, &bs, &bl, &end)) { i += 2; continue; }
                md_mit *den = md_math_ir(ui, s + bs, bl);
                md_mit *it = md_m_new(ui, MT_FRAC);
                it->a = num; it->b = den;
                md_m_push(&head, &tail, it);
                i = end; continue;
            }
            if (md_cmd_at(s, n, i, "\\sqrt")) {
                int j = i + 5, bs, bl, end;
                if (j < n && s[j] == '[') {                /* optional root index */
                    int k = j + 1;
                    while (k < n && s[k] != ']') k++;
                    if (k < n) {
                        md_m_text(ui, &head, &tail, s + j + 1, k - j - 1);
                        j = k + 1;
                    }
                }
                if (md_group_at(s, n, j, &bs, &bl, &end)) {
                    md_mit *rt = md_m_new(ui, MT_SQRT);
                    rt->a = md_math_ir(ui, s + bs, bl);
                    md_m_push(&head, &tail, rt);
                    i = end;
                } else {
                    md_m_text(ui, &head, &tail, "√", -1);
                    i = j;
                }
                continue;
            }
            if (md_cmd_at(s, n, i, "\\text") || md_cmd_at(s, n, i, "\\mathrm") ||
                md_cmd_at(s, n, i, "\\mathbf") || md_cmd_at(s, n, i, "\\operatorname")) {
                int j = i + 1;
                while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
                int bs, bl, end;
                if (md_group_at(s, n, j, &bs, &bl, &end)) {
                    md_m_text(ui, &head, &tail, s + bs, bl);
                    i = end; continue;
                }
                i = j; continue;
            }
            /* Accents: precomposed Latin letter when one exists in the bundled
             * faces, else the modifier mark follows the operand. */
            int acc = -1;                              /* 0 hat .. 9 acute */
            int j = i + 1;
            {
                static const char *const nm[] = {
                    "\\hat", "\\widehat", "\\tilde", "\\widetilde", "\\bar",
                    "\\overline", "\\dot", "\\ddot", "\\vec", "\\mathring",
                    "\\breve", "\\grave", "\\acute"
                };
                static const int kd[] = { 0, 0, 1, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9 };
                for (int k = 0; k < (int)CUI_COUNTOF(nm); k++) {
                    int sl = (int)strlen(nm[k]);
                    if (i + sl <= n && !memcmp(s + i, nm[k], (size_t)sl) &&
                        (i + sl >= n || !(s[i + sl] >= 'a' && s[i + sl] <= 'z'))) {
                        acc = kd[k]; j = i + sl; break;
                    }
                }
            }
            if (acc >= 0) {
                static const char *suffix[] = { "ˆ", "˜", "ˉ", "˙", "¨", "→", "˚", "˘", "`", "´" };
                const char *inner = NULL; int inl = 0;
                int bs, bl, end;
                if (j < n && s[j] == '{' && md_group_at(s, n, j, &bs, &bl, &end)) {
                    inner = s + bs; inl = bl; i = end;
                } else if (j < n && s[j] == '\\' && j + 1 < n) {
                    int k = j + 1;
                    while (k < n && ((s[k] >= 'a' && s[k] <= 'z') ||
                           (s[k] >= 'A' && s[k] <= 'Z'))) k++;
                    inner = s + j; inl = k - j; i = k;
                } else if (j < n) {
                    int k = j + 1;
                    while (k < n && (s[k] & 0xC0) == 0x80) k++;
                    inner = s + j; inl = k - j; i = k;
                } else { i = j; continue; }
                char *txt = md_m_flat(ui, md_math_ir(ui, inner, inl), 1024);
                int tn2 = (int)strlen(txt);
                if (tn2 == 1) {
                    const char *g = md_accent_pre(txt[0], acc);
                    if (g) { md_m_text(ui, &head, &tail, g, -1); continue; }
                }
                md_m_text(ui, &head, &tail, txt, tn2);
                if (acc < (int)CUI_COUNTOF(suffix))
                    md_m_text(ui, &head, &tail, suffix[acc], -1);
                continue;
            }
            if (md_cmd_at(s, n, i, "\\pmod")) {
                int bs, bl, end;
                int j = i + 5;
                md_m_text(ui, &head, &tail, " (mod ", -1);
                if (md_group_at(s, n, j, &bs, &bl, &end)) {
                    char *m = md_m_flat(ui, md_math_ir(ui, s + bs, bl), 1024);
                    md_m_text(ui, &head, &tail, m, -1);
                    i = end;
                } else i = j;
                md_m_text(ui, &head, &tail, ")", 1);
                continue;
            }
            int best = -1, bestlen = 0;
            for (int k = 0; k < (int)CUI_COUNTOF(md_symbols); k++) {
                int sl = (int)strlen(md_symbols[k].tex);
                if (sl > bestlen && i + sl + 1 <= n && s[i] == '\\' &&
                    !memcmp(s + i + 1, md_symbols[k].tex, (size_t)sl) &&
                    (i + sl + 1 >= n || !((unsigned char)s[i + sl + 1] >= 'a' &&
                     (unsigned char)s[i + sl + 1] <= 'z'))) {
                    best = k; bestlen = sl;
                }
            }
            if (best >= 0) {
                const char *u = md_symbols[best].uni;
                md_m_text(ui, &head, &tail, u, -1);
                i += bestlen + 1;
                /* TeX inserts a thin space after a symbolic command; a plain
                 * space here would read as "∂ u" instead of "∂u". */
                if ((unsigned char)u[0] >= 0x80 && i + 1 < n && s[i] == ' ' &&
                    ((s[i + 1] >= 'a' && s[i + 1] <= 'z') ||
                     (s[i + 1] >= 'A' && s[i + 1] <= 'Z') ||
                     (s[i + 1] >= '0' && s[i + 1] <= '9') || s[i + 1] == '\\')) i++;
                continue;
            }
            i++;                                      /* unknown \cmd: keep the name */
            continue;
        }
        if ((c == '^' || c == '_') && i + 1 < n) {
            bool sup = c == '^';
            md_mit *sc = NULL;
            if (tail && tail->kind == MT_SCRIPT) sc = tail;
            else {
                sc = md_m_new(ui, MT_SCRIPT);
                sc->a = md_m_pop_base(ui, &head, &tail);
                md_m_push(&head, &tail, sc);
            }
            md_mit *oh = NULL;
            int j = i + 1;
            if (j < n && s[j] == '{') {
                int bs, bl, end;
                if (md_group_at(s, n, j, &bs, &bl, &end)) {
                    oh = md_math_ir(ui, s + bs, bl);
                    i = end;
                } else i = j;
            } else if (j < n && s[j] == '\\') {
                int k = j + 1;
                if (k < n && ((s[k] >= 'a' && s[k] <= 'z') ||
                              (s[k] >= 'A' && s[k] <= 'Z'))) {
                    while (k < n && ((s[k] >= 'a' && s[k] <= 'z') ||
                           (s[k] >= 'A' && s[k] <= 'Z'))) k++;
                } else if (k < n) k++;
                oh = md_math_ir(ui, s + j, k - j);
                i = k;
            } else if (j < n) {
                int k = j + 1;
                while (k < n && (s[k] & 0xC0) == 0x80) k++;
                oh = md_math_ir(ui, s + j, k - j);
                i = k;
            } else i = j;
            if (sup) sc->b = oh; else sc->c = oh;
            continue;
        }
        if (c == '{' || c == '}') { i++; continue; }
        if (c == '&') {                                /* column gap */
            md_mit *it = md_m_new(ui, MT_GAP); it->tn = 2;
            md_m_push(&head, &tail, it);
            i++; continue;
        }
        md_m_text(ui, &head, &tail, s + i, 1);
        i++;
    }
    return head;
}

static char *md_math_translate(cui_ctx *ui, const char *s, int n) {
    md_mit *ir = md_math_ir(ui, s, n);
    int cap = n * 8 + 256;
    char *out = cui__frame_alloc(ui, (size_t)cap);
    int o = 0;
    md_m_flat1(ui, ir, out, &o, cap);
    out[o] = 0;
    return out;
}

/* Split a matrix environment body into rows of cells: \\ separates rows,
 * & separates columns (both at brace depth 0). */
static void md_m_matrix(cui_ctx *ui, md_mit *it, const char *s, int bs, int bl) {
    int ncm = 1, nr = 1, depth = 0, cols = 1;
    for (int i = 0; i < bl; i++) {
        char ch = s[bs + i];
        if (ch == '\\' && i + 1 < bl) {
            if (s[bs + i + 1] == '\\' && depth == 0) {
                nr++;
                if (cols > ncm) ncm = cols;
                cols = 1;
            }
            i++; continue;                         /* skip escape / command char */
        }
        if (ch == '{') depth++;
        else if (ch == '}') { if (depth > 0) depth--; }
        else if (depth == 0 && ch == '&') cols++;
    }
    if (cols > ncm) ncm = cols;
    it->nrow = nr; it->ncol = ncm;
    it->ncr = cui__frame_alloc(ui, sizeof(int) * (size_t)nr);
    it->cell = cui__frame_alloc(ui, sizeof(md_mit *) * (size_t)(nr * ncm));
    memset(it->cell, 0, sizeof(md_mit *) * (size_t)(nr * ncm));
    int r = 0, cl = 0, cstart = bs;
    depth = 0;
    for (int i = 0; i <= bl; i++) {
        bool sep_row = false, sep_col = false, fin = i == bl;
        int cur = i;                               /* cell ends here (exclusive) */
        if (!fin) {
            char ch = s[bs + i];
            if (ch == '\\' && i + 1 < bl) {
                if (s[bs + i + 1] == '\\' && depth == 0) { sep_row = true; cur = i; i++; }
                else { i++; continue; }
            } else if (ch == '{') depth++;
            else if (ch == '}') { if (depth > 0) depth--; }
            else if (depth == 0 && ch == '&') sep_col = true;
        }
        if (sep_row || sep_col || fin) {
            md_mit *cell = md_math_ir(ui, s + cstart, bs + cur - cstart);
            if (cell && r < nr && cl < ncm) it->cell[r * ncm + cl] = cell;
            it->ncr[r] = cl + 1;
            if (sep_row) { r++; cl = 0; cstart = bs + i + 1; }
            else if (sep_col) { cl++; cstart = bs + i + 1; }
        }
    }
}

/* Measure an item chain: fills w/as/de (box around the baseline). */
static void md_m_meas(cui_ctx *ui, md_mit *it, float fs);

static void md_m_meas1(cui_ctx *ui, md_mit *it, float fs) {
    float fa = cui__font_ascent(ui, CUI_FONT_BODY, fs);
    float fd = cui__font_line_height(ui, CUI_FONT_BODY, fs) - fa;
    switch (it->kind) {
    case MT_TXT:
        it->w = cui__text_width(ui, CUI_FONT_BODY, fs, it->buf, it->tn, false);
        it->lruns = md_m_letter_runs(it->buf, it->tn);
        it->w += it->lruns * 0.08f * fs;           /* room for the italic shear */
        it->as = fa; it->de = fd;
        break;
    case MT_GAP:
        it->w = (float)it->tn *
                cui__text_width(ui, CUI_FONT_BODY, fs, " ", 1, false);
        it->as = it->de = 0;
        break;
    case MT_ROWBR: case MT_COLBR:
        it->w = it->as = it->de = 0;
        break;
    case MT_FRAC: {
        if (it->a) md_m_meas(ui, it->a, fs);
        if (it->b) md_m_meas(ui, it->b, fs);
        float cw = CUI_MAX(it->a ? it->a->w : 0, it->b ? it->b->w : 0);
        it->w = cw + 6;
        it->as = (it->a ? it->a->as : fa) + 0.47f * fs;
        it->de = (it->b ? it->b->de : fd) + 0.47f * fs;
        break;
    }
    case MT_SQRT: {
        if (it->a) md_m_meas(ui, it->a, fs);
        float sw = cui__text_width(ui, CUI_FONT_BODY, fs, "√", -1, false);
        it->w = sw + (it->a ? it->a->w : 0) + 3;
        it->as = CUI_MAX(fa, (it->a ? it->a->as : fa) + 5);
        it->de = it->a ? it->a->de : fd;
        break;
    }
    case MT_SCRIPT: {
        if (it->a) md_m_meas(ui, it->a, fs);
        float sfs = fs * 0.7f;
        if (it->b) md_m_meas(ui, it->b, sfs);
        if (it->c) md_m_meas(ui, it->c, sfs);
        it->w = (it->a ? it->a->w : 0) + (it->b ? it->b->w : 0) + (it->c ? it->c->w : 0);
        it->as = CUI_MAX(it->a ? it->a->as : fa,
                         it->b ? it->b->as + 0.42f * fs : 0);
        it->de = CUI_MAX(it->a ? it->a->de : fd,
                         it->c ? it->c->de + 0.22f * fs : 0);
        break;
    }
    case MT_BINOM: {
        if (it->a) md_m_meas(ui, it->a, fs);
        if (it->b) md_m_meas(ui, it->b, fs);
        float hs = (it->a ? it->a->as + it->a->de : fa + fd) +
                   (it->b ? it->b->as + it->b->de : fa + fd) + 0.2f * fs;
        float cw = CUI_MAX(it->a ? it->a->w : 0, it->b ? it->b->w : 0);
        it->w = cw + 0.9f * fs;
        it->as = hs * 0.5f + 0.03f * fs;
        it->de = hs * 0.5f - 0.03f * fs;
        break;
    }
    case MT_MAT: {
        it->rh = cui__frame_alloc(ui, sizeof(float) * (size_t)it->nrow);
        it->mxa = cui__frame_alloc(ui, sizeof(float) * (size_t)it->nrow);
        it->cw = cui__frame_alloc(ui, sizeof(float) * (size_t)it->ncol);
        memset(it->cw, 0, sizeof(float) * (size_t)it->ncol);
        float h = 0;
        for (int r = 0; r < it->nrow; r++) {
            float mxa = fa * 0.5f, mde = fd * 0.5f;
            for (int cc = 0; cc < it->ncr[r]; cc++) {
                md_mit *cell = it->cell[r * it->ncol + cc];
                if (!cell) continue;
                for (md_mit *m = cell; m; m = m->next) md_m_meas1(ui, m, fs);
                float w2 = 0;
                for (md_mit *m = cell; m; m = m->next) w2 += m->w;
                if (w2 > it->cw[cc]) it->cw[cc] = w2;
                if (cell->as > mxa) mxa = cell->as;
                if (cell->de > mde) mde = cell->de;
            }
            it->mxa[r] = mxa;
            it->rh[r] = mxa + mde;
            h += it->rh[r];
        }
        h += (it->nrow - 1) * 0.15f * fs;
        it->mh = h;
        float dw = ((it->dl[0] ? 0.55f : 0) + (it->dr[0] ? 0.55f : 0)) * fs;
        float gap = ((it->dl[0] ? 1 : 0) + (it->dr[0] ? 1 : 0)) * 0.3f * fs;
        float tw = dw + gap;
        for (int cc = 0; cc < it->ncol; cc++) tw += it->cw[cc] + 0.75f * fs;
        it->w = tw - 0.75f * fs;
        it->as = h * 0.5f + 0.3f * fs;
        it->de = h * 0.5f;
        break;
    }
    }
}

static void md_m_meas(cui_ctx *ui, md_mit *it, float fs) {
    for (; it; it = it->next) md_m_meas1(ui, it, fs);
}

/* Draw one delimiter spanning [top, bot]: paren/bracket curves or bars. */
static void md_m_delim(cui_ctx *ui, float x, float top, float bot,
                       const char *d, bool left, cui_color col, float fs) {
    float th = CUI_MAX(1.0f, fs / 14.0f);
    float wdt = 0.45f * fs;
    float mid = (top + bot) * 0.5f;
    if (!strcmp(d, "|")) {
        cui_draw_line(ui, (cui_vec2){ x + wdt * 0.5f, top },
                      (cui_vec2){ x + wdt * 0.5f, bot }, th, col);
        return;
    }
    if (!strcmp(d, "‖")) {
        cui_draw_line(ui, (cui_vec2){ x + wdt * 0.3f, top },
                      (cui_vec2){ x + wdt * 0.3f, bot }, th, col);
        cui_draw_line(ui, (cui_vec2){ x + wdt * 0.7f, top },
                      (cui_vec2){ x + wdt * 0.7f, bot }, th, col);
        return;
    }
    bool curvy = d[0] == '(' || d[0] == ')';
    bool curly = d[0] == '{' || d[0] == '}';
    float inner = left ? x + wdt * 0.18f : x + wdt * 0.82f;
    float outer = left ? x + wdt : x;
    if (curly) {
        cui_vec2 v[7] = {
            { outer, top + 1 }, { inner, top + 1 }, { inner, mid - 3 },
            { left ? inner + wdt * 0.5f : inner - wdt * 0.5f, mid },
            { inner, mid + 3 }, { inner, bot - 1 }, { outer, bot - 1 }
        };
        cui_draw_polyline(ui, v, 7, th, col, false);
        return;
    }
    float bend = curvy ? (bot - top) * 0.3f : 1;
    cui_vec2 v[4] = {
        { outer, top + 1 }, { inner, top + bend },
        { inner, bot - bend }, { outer, bot - 1 }
    };
    cui_draw_polyline(ui, v, 4, th, col, false);
}

/* Paint one item with its baseline at yb; returns the next x. */
static float md_m_paint1(cui_ctx *ui, md_mit *it, float x, float yb, float fs,
                         md_mpaint *p) {
    float fa = cui__font_ascent(ui, CUI_FONT_BODY, fs);
    float fd = cui__font_line_height(ui, CUI_FONT_BODY, fs) - fa;
    switch (it->kind) {
    case MT_TXT: {
        int i = 0;
        while (i < it->tn) {
            int cl; bool let = md_m_letter(it->buf + i, it->tn - i, &cl);
            int j = i;
            while (j < it->tn) {
                int l2; bool let2 = md_m_letter(it->buf + j, it->tn - j, &l2);
                if (let2 != let) break;
                j += l2;
            }
            int v0 = ui->dl.vtx_count;
            cui_draw_text(ui, CUI_FONT_BODY, fs,
                          (cui_vec2){ x, yb - fa }, p->fg, it->buf + i, j - i);
            if (let) md_shear(ui, v0, yb);
            x += cui__text_width(ui, CUI_FONT_BODY, fs, it->buf + i, j - i, false)
                 + (let ? 0.08f * fs : 0);
            i = j;
        }
        break;
    }
    case MT_GAP:
        x += it->w;
        break;
    case MT_FRAC: {
        float bar = yb - 0.33f * fs;
        float th = CUI_MAX(1.0f, fs / 14.0f);
        float cw = it->w - 6;
        if (it->a) {
            float w2 = 0;
            for (md_mit *m = it->a; m; m = m->next) w2 += m->w;
            md_m_paint_list(ui, it->a, x + 3 + (cw - w2) * 0.5f,
                            bar - 0.14f * fs - it->a->de, fs, p);
        }
        if (it->b) {
            float w2 = 0;
            for (md_mit *m = it->b; m; m = m->next) w2 += m->w;
            md_m_paint_list(ui, it->b, x + 3 + (cw - w2) * 0.5f,
                            bar + 0.14f * fs + it->b->as, fs, p);
        }
        cui_draw_rect(ui, (cui_rect){ x + 2, bar - th * 0.5f, it->w - 4, th },
                      CUI_CORNERS(0), p->fg);
        x += it->w;
        break;
    }
    case MT_SQRT: {
        float sw = cui__text_width(ui, CUI_FONT_BODY, fs, "√", -1, false);
        cui_draw_text(ui, CUI_FONT_BODY, fs,
                      (cui_vec2){ x, yb - fa }, p->fg, "√", -1);
        float y_ov = yb - CUI_MAX(0.85f * fs, (it->a ? it->a->as : fa) + 4);
        float th = CUI_MAX(1.0f, fs / 14.0f);
        cui_draw_rect(ui, (cui_rect){ x + sw * 0.5f, y_ov,
                                      sw * 0.5f + (it->a ? it->a->w : 0) + 4, th },
                      CUI_CORNERS(0), p->fg);
        if (it->a) md_m_paint_list(ui, it->a, x + sw + 3, yb, fs, p);
        x += it->w;
        break;
    }
    case MT_SCRIPT: {
        float sfs = fs * 0.7f;
        float xa = it->a ? md_m_paint_list(ui, it->a, x, yb, fs, p) : x;
        if (it->b) md_m_paint_list(ui, it->b, xa, yb - 0.42f * fs, sfs, p);
        if (it->c) {
            float xs = xa + (it->b ? it->b->w : 0);
            md_m_paint_list(ui, it->c, xs, yb + 0.22f * fs, sfs, p);
        }
        x += it->w;
        break;
    }
    case MT_BINOM: {
        float dw = 0.45f * fs;
        float top = yb - it->as;
        float hs = it->as + it->de;
        float cwid = it->w - 2 * dw;
        if (it->a) {
            float w2 = 0;
            for (md_mit *m = it->a; m; m = m->next) w2 += m->w;
            md_m_paint_list(ui, it->a, x + dw + (cwid - w2) * 0.5f,
                            top + it->a->as, fs, p);
        }
        if (it->b) {
            float w2 = 0;
            for (md_mit *m = it->b; m; m = m->next) w2 += m->w;
            float btop = top + (it->a ? it->a->as + it->a->de : fa + fd) + 0.2f * fs;
            md_m_paint_list(ui, it->b, x + dw + (cwid - w2) * 0.5f,
                            btop + it->b->as, fs, p);
        }
        md_m_delim(ui, x, top, top + hs, "(", true, p->fg, fs);
        md_m_delim(ui, x + dw + cwid, top, top + hs, ")", false, p->fg, fs);
        x += it->w;
        break;
    }
    case MT_MAT: {
        float dlw = (it->dl[0] ? 0.55f : 0) * fs;
        float drw = (it->dr[0] ? 0.55f : 0) * fs;
        float mgap = ((it->dl[0] ? 1 : 0) + (it->dr[0] ? 1 : 0)) * 0.3f * fs;
        float top = yb - it->as;
        float *cx = cui__frame_alloc(ui, sizeof(float) * (size_t)(it->ncol + 1));
        float cxx = x + dlw + (it->dl[0] ? 0.3f * fs : 0);
        for (int cc = 0; cc < it->ncol; cc++) { cx[cc] = cxx; cxx += it->cw[cc] + 0.75f * fs; }
        float rowtop = top;
        for (int r = 0; r < it->nrow; r++) {
            float rb = rowtop + it->mxa[r];
            for (int cc = 0; cc < it->ncr[r]; cc++) {
                md_mit *cell = it->cell[r * it->ncol + cc];
                if (!cell) continue;
                float cellw = 0;
                for (md_mit *m = cell; m; m = m->next) cellw += m->w;
                md_m_paint_list(ui, cell, cx[cc] + (it->cw[cc] - cellw) * 0.5f, rb, fs, p);
            }
            rowtop += it->rh[r] + 0.15f * fs;
        }
        float bot = rowtop - 0.15f * fs;
        if (it->dl[0]) md_m_delim(ui, x, top, bot, it->dl, true, p->fg, fs);
        if (it->dr[0]) md_m_delim(ui, x + it->w - drw, top, bot, it->dr, false, p->fg, fs);
        x += it->w;
        break;
    }
    case MT_ROWBR: case MT_COLBR: break;
    }
    return x;
}

static float md_m_paint_list(cui_ctx *ui, md_mit *it, float x, float yb, float fs,
                             md_mpaint *p) {
    for (; it; it = it->next) x = md_m_paint1(ui, it, x, yb, fs, p);
    return x;
}

/* Wrap the top-level rows (split at MT_ROWBR) into centered display lines. */
static void md_m_layout(cui_ctx *ui, md_mit *ir, float fs, float avail,
                        md_mpaint *p) {
    float fa = cui__font_ascent(ui, CUI_FONT_BODY, fs);
    float fd = cui__font_line_height(ui, CUI_FONT_BODY, fs) - fa;
    int total = 0;
    for (md_mit *it = ir; it; it = it->next) { md_m_meas1(ui, it, fs); total++; }
    md_mit **all = cui__frame_alloc(ui, sizeof(md_mit *) * (size_t)(total + 1));
    int n = 0;
    for (md_mit *it = ir; it; it = it->next) all[n++] = it;
    md_mline *lines = cui__frame_alloc(ui, sizeof(md_mline) * (size_t)(n + 2));
    int nline = 0, rstart = 0;
    for (int k = 0; k <= n; k++) {
        if (k == n || all[k]->kind == MT_ROWBR) {
            int i = rstart;
            while (i < k) {
                md_mline *L = &lines[nline++];
                L->it = &all[i]; L->n = 0; L->w = L->as = L->de = 0;
                while (i < k) {
                    md_mit *m = all[i];
                    if (L->n > 0 && L->w + m->w > avail) break;
                    L->w += m->w;
                    if (m->as > L->as) L->as = m->as;
                    if (m->de > L->de) L->de = m->de;
                    L->n++; i++;
                }
                if (L->as < fa) L->as = fa;
                if (L->de < fd * 0.6f) L->de = fd * 0.6f;
            }
            rstart = k + 1;
        }
    }
    p->line = lines;
    p->nline = nline;
    float h = 0, w = 0;
    for (int k = 0; k < nline; k++) {
        h += lines[k].as + lines[k].de + p->lh;
        if (lines[k].w > w) w = lines[k].w;
    }
    p->h = h - p->lh;
    p->w = w;
}

static void md_math_paint_cb(cui_ctx *ui, cui_rect rect, void *user) {
    md_mpaint *p = user;
    cui_draw_rect(ui, rect, CUI_CORNERS(8 * p->u), p->bg);
    cui_draw_rect_border(ui, rect, CUI_CORNERS(8 * p->u), cui__rgba(0, 0, 0, 0),
                         p->border, 1);
    float pad = 13 * p->u;
    float y = rect.y + pad;
    for (int k = 0; k < p->nline; k++) {
        md_mline *L = &p->line[k];
        float x = rect.x + (rect.w - L->w) * 0.5f;
        float yb = y + L->as;
        for (int q = 0; q < L->n; q++)
            x = md_m_paint1(ui, L->it[q], x, yb, p->fs, p);
        y = yb + L->de + p->lh;
    }
}

/* $...$ only when both sides are tight: "$5 and $10" stays prose. */
static int md_math_span(const char *s, int n, int at) {
    if (at + 1 >= n || md_ws((unsigned char)s[at + 1])) return 0;
    for (int j = at + 1; j < n; j++) {
        if (s[j] == '\\' && j + 1 < n) { j++; continue; }
        if (s[j] != '$') continue;
        if (j == at + 1 || md_ws((unsigned char)s[j - 1])) return 0;
        return j;
    }
    return 0;
}

/* Parses inline markdown into styled runs. tail: the text is still arriving
 * and this is its end, so unclosed markers render as if closed. */
static md_inline md_parse_inline(cui_ctx *ui, const char *s, int n, bool tail) {
    md_inline out;
    memset(&out, 0, sizeof out);
    if (n <= 0) return out;
    int specials = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '*' || c == '_' || c == '~' || c == '`' || c == '$' || c == '[' || c == ']' || c == '!' || c == '\\' || c == '<' || c == '&' || c == '\n') specials++;
        else if ((c == 'h' || c == 'w') && (i == 0 || md_url_lead((unsigned char)s[i - 1]))) specials++;
    }
    md_toks T;
    T.cap = specials * 2 + 2; T.n = 0;
    T.t = cui__frame_alloc(ui, sizeof(md_tok) * (size_t)T.cap);
    int link_cap = specials + 1, nl = 0;
    md_link *links = cui__frame_alloc(ui, sizeof(md_link) * (size_t)link_cap);
    int stack[32], nb = 0;            /* open [ and ![ tokens */
    int no_tick[MD_MAX_TICKS + 1];    /* no closing run of this length at or after */
    for (int k = 0; k <= MD_MAX_TICKS; k++) no_tick[k] = n + 1;
    int i = 0, ts = 0;
#define MD_FLUSH() do { if (i > ts) md_add(&T, TK_TXT, s + ts, i - ts); } while (0)
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n && md_punct((unsigned char)s[i + 1])) {
            MD_FLUSH();
            md_add(&T, TK_TXT, s + i + 1, 1);
            i += 2; ts = i;
            continue;
        }
        if (c == '\n') { MD_FLUSH(); md_add(&T, TK_BR, s + i, 0); i++; ts = i; continue; }
        if (c == '$' && !(i + 1 < n && s[i + 1] == '$')) {
            int close = md_math_span(s, n, i);
            if (close > i + 1) {
                char *expr = md_math_translate(ui, s + i + 1, close - i - 1);
                int el = (int)strlen(expr);
                if (el > 0) {
                    MD_FLUSH();
                    md_add(&T, TK_MATH, expr, el);
                    i = close + 1; ts = i;
                    continue;
                }
            }
        }
        if (c == '`') {
            int k = 0;
            while (i + k < n && s[i + k] == '`') k++;
            MD_FLUSH();
            int close = -1;
            if (k <= MD_MAX_TICKS && i + k < no_tick[k]) {
                int j = i + k;
                while (j < n) {
                    if (s[j] != '`') { j++; continue; }
                    int m = j;
                    while (m < n && s[m] == '`') m++;
                    if (m - j == k) { close = j; break; }
                    j = m;
                }
                if (close < 0) no_tick[k] = i + k;
            }
            int cs = i + k, ce = close >= 0 ? close : n;
            if (close >= 0 || (tail && k <= MD_MAX_TICKS)) {
                bool all_sp = true;
                for (int q = cs; q < ce; q++) if (s[q] != ' ') { all_sp = false; break; }
                if (close >= 0 && ce - cs >= 2 && s[cs] == ' ' && s[ce - 1] == ' ' && !all_sp) { cs++; ce--; }
                md_add(&T, TK_CODE, s + cs, ce - cs);
                i = close >= 0 ? close + k : n;
            } else {
                md_add(&T, TK_TXT, s + i, k);
                i += k;
            }
            ts = i;
            continue;
        }
        if (c == '*' || c == '_' || c == '~') {
            int k = 0;
            while (i + k < n && s[i + k] == (char)c) k++;
            MD_FLUSH();
            md_tok *t = md_add(&T, TK_DELIM, s + i, k);
            if (t) {
                unsigned char before = i > 0 ? (unsigned char)s[i - 1] : 0, after = i + k < n ? (unsigned char)s[i + k] : 0;
                bool lf = !md_ws(after) && (!md_punct(after) || md_ws(before) || md_punct(before));
                bool rf = !md_ws(before) && (!md_punct(before) || md_ws(after) || md_punct(after));
                t->ch = c;
                t->count = t->orig = (int16_t)CUI_MIN(k, 127);
                if (c == '_') { t->can_open = lf && (!rf || md_punct(before)); t->can_close = rf && (!lf || md_punct(after)); }
                else { t->can_open = lf; t->can_close = rf; }
                if (k > MD_MAX_RUN || (c == '~' && k != 2)) t->can_open = t->can_close = 0;
                if (tail && i + k >= n) { t->hidden = 1; t->can_open = t->can_close = 0; }   /* may still grow */
            }
            i += k; ts = i;
            continue;
        }
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            int k = c == '!' ? 2 : 1;
            MD_FLUSH();
            md_tok *t = md_add(&T, TK_LOPEN, s + i, k);
            if (t) {
                t->image = c == '!';
                if (nb < 32) stack[nb++] = T.n - 1;
                else t->kind = TK_TXT;
            }
            i += k; ts = i;
            continue;
        }
        if (c == ']') {
            MD_FLUSH();
            const char *url = NULL;
            int ulen = 0, end = i + 1;
            int st = md_link_dest(s, n, i + 1, &url, &ulen, &end);
            int ob = nb > 0 ? stack[nb - 1] : -1;
            bool pending = tail && st < 0;
            if (ob >= 0 && nl < link_cap && (st == 1 || pending)) {
                md_link *L = &links[nl];
                L->url = url; L->len = ulen; L->pending = st != 1;
                T.t[ob].link = nl;
                md_tok *t = md_add(&T, TK_LCLOSE, s + i, 0);
                if (t) t->link = nl;
                nl++;
                bool image = T.t[ob].image;
                nb--;
                if (!image) {   /* links do not nest: earlier [ become text */
                    int keep = 0;
                    for (int q = 0; q < nb; q++) if (T.t[stack[q]].image) stack[keep++] = stack[q];
                    nb = keep;
                }
                i = st == 1 ? end : n;
            } else {
                if (ob >= 0) nb--;
                md_add(&T, TK_TXT, s + i, 1);
                i++;
            }
            ts = i;
            continue;
        }
        if (c == '<') {
            int e = md_br_tag(s, n, i);
            if (e > 0) { MD_FLUSH(); md_add(&T, TK_BR, s + i, 0); i = e; ts = i; continue; }
            e = md_autolink(s, n, i);
            if (e > 0 && nl < link_cap) {
                MD_FLUSH();
                md_tok *t = md_add(&T, TK_AUTO, s + i + 1, e - i - 1);
                if (t) {
                    links[nl].url = s + i + 1; links[nl].len = e - i - 1; links[nl].autolink = true;
                    t->link = nl++;
                }
                i = e + 1; ts = i;
                continue;
            }
        }
        if (c == '&') {
            static const char *const ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
                                                 { "&#39;", "'" }, { "&nbsp;", "\xc2\xa0" } };
            int hit = -1;
            for (int k = 0; k < CUI_COUNTOF(ent); k++) {
                int el = (int)strlen(ent[k][0]);
                if (n - i >= el && !strncmp(s + i, ent[k][0], (size_t)el)) { hit = k; break; }
            }
            if (hit >= 0) {
                MD_FLUSH();
                md_add(&T, TK_TXT, ent[hit][1], (int)strlen(ent[hit][1]));
                i += (int)strlen(ent[hit][0]); ts = i;
                continue;
            }
        }
        if ((c == 'h' || c == 'w') && (i == 0 || md_url_lead((unsigned char)s[i - 1]))) {
            bool www = false;
            int e = md_bare_url(s, n, i, &www);
            if (e > i && nl < link_cap) {
                MD_FLUSH();
                md_tok *t = md_add(&T, TK_AUTO, s + i, e - i);
                if (t) {
                    links[nl].url = s + i; links[nl].len = e - i; links[nl].www = www; links[nl].autolink = true;
                    t->link = nl++;
                }
                i = e; ts = i;
                continue;
            }
        }
        i++;
    }
    MD_FLUSH();
#undef MD_FLUSH
    /* an unclosed [ at the end may still become a link: hold it back */
    if (tail) for (int q = 0; q < nb; q++) T.t[stack[q]].hidden = 1;

    /* emphasis: each closer looks back for the nearest opener of its kind */
    int *dl = cui__frame_alloc(ui, sizeof(int) * (size_t)(T.n + 1)), nd = 0;
    for (int k = 0; k < T.n; k++) if (T.t[k].kind == TK_DELIM && (T.t[k].can_open || T.t[k].can_close)) dl[nd++] = k;
    for (int ci = 0; ci < nd; ci++) {
        md_tok *C = &T.t[dl[ci]];
        if (!C->can_close) continue;
        while (C->count > 0) {
            int found = -1;
            for (int oi = ci - 1; oi >= 0 && ci - oi <= MD_WINDOW; oi--) {
                md_tok *O = &T.t[dl[oi]];
                if (O->ch != C->ch || !O->can_open || O->count <= 0) continue;
                if (C->ch == '~') { if (O->count < 2 || C->count < 2) continue; }
                else if ((O->can_close || C->can_open) && (O->orig + C->orig) % 3 == 0 && !(O->orig % 3 == 0 && C->orig % 3 == 0)) continue;
                found = oi;
                break;
            }
            if (found < 0) break;
            md_tok *O = &T.t[dl[found]];
            int use = C->ch == '~' ? 2 : (O->count >= 2 && C->count >= 2 ? 2 : 1);
            int st = C->ch == '~' ? 2 : (use == 2 ? 0 : 1);
            O->post[st]++; C->pre[st]++;
            O->count = (int16_t)(O->count - use); C->count = (int16_t)(C->count - use);
            for (int k = found + 1; k < ci; k++) T.t[dl[k]].can_open = T.t[dl[k]].can_close = 0;
        }
    }
    if (tail) {   /* openers still waiting for their closer style the rest of the text */
        for (int k = 0; k < nd; k++) {
            md_tok *O = &T.t[dl[k]];
            if (!O->can_open || O->count <= 0) continue;
            if (O->ch == '~') { if (O->count >= 2) O->post[2]++; }
            else { if (O->count >= 2) O->post[0]++; if (O->count % 2 == 1) O->post[1]++; }
            O->count = 0;
        }
    }

    /* runs */
    md_run *runs = cui__frame_alloc(ui, sizeof(md_run) * (size_t)(T.n + 1));
    int nr = 0, cnt[3] = { 0, 0, 0 }, cur = -1;
    for (int k = 0; k < T.n; k++) {
        md_tok *t = &T.t[k];
        for (int q = 0; q < 3; q++) cnt[q] -= t->pre[q];
        int style = (cnt[0] > 0 ? MS_STRONG : 0) | (cnt[1] > 0 ? MS_EM : 0) | (cnt[2] > 0 ? MS_STRIKE : 0);
        switch (t->kind) {
        case TK_TXT: md_emit(runs, &nr, t->p, t->len, style, cur); break;
        case TK_DELIM: if (!t->hidden && t->count > 0) md_emit(runs, &nr, t->p, t->count, style, cur); break;
        case TK_CODE: md_emit(runs, &nr, t->p, t->len, style | MS_CODE, cur); break;
        case TK_MATH: md_emit(runs, &nr, t->p, t->len, style | MS_MATH, cur); break;
        case TK_BR: md_emit(runs, &nr, t->p, 0, MS_BREAK, -1); break;
        case TK_LOPEN:
            if (t->link >= 0) cur = t->link;
            else if (!t->hidden) md_emit(runs, &nr, t->p, t->len, style, cur);
            break;
        case TK_LCLOSE: cur = -1; break;
        case TK_AUTO: md_emit(runs, &nr, t->p, t->len, style, cur >= 0 ? cur : t->link); break;
        default: break;
        }
        for (int q = 0; q < 3; q++) cnt[q] += t->post[q];
    }
    out.runs = runs; out.nruns = nr; out.links = links; out.nlinks = nl;
    return out;
}

static char *md_plain(cui_ctx *ui, const md_inline *in) {
    int total = 0;
    for (int k = 0; k < in->nruns; k++) total += (in->runs[k].style & MS_BREAK) ? 1 : in->runs[k].len;
    char *out = cui__frame_alloc(ui, (size_t)total + 1);
    int o = 0;
    for (int k = 0; k < in->nruns; k++) {
        if (in->runs[k].style & MS_BREAK) { out[o++] = ' '; continue; }
        memcpy(out + o, in->runs[k].p, (size_t)in->runs[k].len);
        o += in->runs[k].len;
    }
    out[o] = 0;
    return out;
}

/* ------------------------------------------------------------------------ */
/* Inline layout                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    cui_font reg, bold, mono;
    float size, mono_size, lh;   /* points, scaled */
    float pad;                   /* inline code: padding inside its tint */
} md_font;

typedef struct { const char *p; int len; float x, w; int line, run, link; uint8_t style, lead; } md_seg;
typedef struct { md_seg *seg; int n, cap; int lines; float widest; } md_lay;

static cui_font md_font_of(const md_font *f, int style) { return (style & MS_CODE) ? f->mono : (style & MS_STRONG) ? f->bold : f->reg; }
static float md_size_of(const md_font *f, int style) { return (style & MS_CODE) ? f->mono_size : f->size; }
static bool md_space_char(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static float md_piece_w(cui_ctx *ui, const md_font *f, const md_run *R, int b, int e) {
    float w = cui__text_width(ui, md_font_of(f, R->style), md_size_of(f, R->style), R->p + b, e - b, false);
    if (R->style & MS_CODE) { if (b == 0) w += f->pad; if (e == R->len) w += f->pad; }
    return w;
}

/* One line width per hard break, nothing wrapped: the natural width. */
static float md_runs_width(cui_ctx *ui, const md_inline *in, const md_font *f) {
    float w = 0, best = 0;
    for (int k = 0; k < in->nruns; k++) {
        const md_run *R = &in->runs[k];
        if (R->style & MS_BREAK) { best = CUI_MAX(best, w); w = 0; continue; }
        w += md_piece_w(ui, f, R, 0, R->len);
    }
    return CUI_MAX(best, w);
}

static void md_seg_add(cui_ctx *ui, md_lay *L, const md_inline *in, int run, const char *p, int len, float x, float w) {
    const md_run *R = &in->runs[run];
    if (L->n > 0) {
        md_seg *q = &L->seg[L->n - 1];
        if (q->line == L->lines && q->run == run && q->p + q->len == p) { q->len += len; q->w = x + w - q->x; return; }
    }
    if (L->n == L->cap) {
        int ncap = L->cap ? L->cap * 2 : 32;
        md_seg *ns = cui__frame_alloc(ui, sizeof(md_seg) * (size_t)ncap);
        if (L->n) memcpy(ns, L->seg, sizeof(md_seg) * (size_t)L->n);
        L->seg = ns; L->cap = ncap;
    }
    md_seg *s = &L->seg[L->n++];
    s->p = p; s->len = len; s->x = x; s->w = w; s->line = L->lines; s->run = run; s->link = R->link;
    s->style = R->style; s->lead = (R->style & MS_CODE) && p == R->p;
}

static void md_finish_line(md_lay *L, int *first, float x, float W, int align) {
    if ((align == CUI_TEXT_CENTER || align == CUI_TEXT_RIGHT) && W > x) {   /* left and start stay put */
        float dx = (W - x) * (align == CUI_TEXT_CENTER ? 0.5f : 1.0f);
        for (int k = *first; k < L->n; k++) L->seg[k].x += dx;
    }
    L->widest = CUI_MAX(L->widest, x);
    L->lines++;
    *first = L->n;
}

/* Greedy word wrap across runs. Words wider than the line break between characters. */
static void md_layout(cui_ctx *ui, const md_inline *in, const md_font *f, float W, int align, md_lay *L) {
    memset(L, 0, sizeof *L);
    float x = 0;
    int first = 0, ri = 0, bi = 0, sp_run = -1;
    const char *sp_p = NULL;
    while (ri < in->nruns) {
        const md_run *R = &in->runs[ri];
        if (R->style & MS_BREAK) { md_finish_line(L, &first, x, W, align); x = 0; sp_run = -1; ri++; bi = 0; continue; }
        if (bi >= R->len) { ri++; bi = 0; continue; }
        if (md_space_char(R->p[bi])) {
            if (sp_run < 0) { sp_run = ri; sp_p = R->p + bi; }
            while (bi < R->len && md_space_char(R->p[bi])) bi++;
            continue;
        }
        float ww = 0;
        for (int rj = ri, bj = bi; rj < in->nruns; rj++, bj = 0) {
            const md_run *Q = &in->runs[rj];
            if (Q->style & MS_BREAK) break;
            int e = bj;
            while (e < Q->len && !md_space_char(Q->p[e])) e++;
            if (e > bj) ww += md_piece_w(ui, f, Q, bj, e);
            if (e < Q->len) break;
        }
        float sw = 0;
        if (sp_run >= 0 && x > 0) {
            const md_run *S = &in->runs[sp_run];
            sw = cui__text_width(ui, md_font_of(f, S->style), md_size_of(f, S->style), " ", 1, false);
        }
        if (x > 0 && x + sw + ww > W) { md_finish_line(L, &first, x, W, align); x = 0; sw = 0; }
        if (sw > 0) { md_seg_add(ui, L, in, sp_run, *sp_p == ' ' ? sp_p : " ", 1, x, sw); x += sw; }
        sp_run = -1;
        bool split = ww > W - x;
        while (ri < in->nruns) {
            const md_run *Q = &in->runs[ri];
            if (Q->style & MS_BREAK) break;
            int e = bi;
            while (e < Q->len && !md_space_char(Q->p[e])) e++;
            if (e > bi) {
                if (!split) {
                    float pw = md_piece_w(ui, f, Q, bi, e);
                    md_seg_add(ui, L, in, ri, Q->p + bi, e - bi, x, pw);
                    x += pw;
                } else {
                    cui_font qf = md_font_of(f, Q->style);
                    float qs = md_size_of(f, Q->style);
                    bool code = (Q->style & MS_CODE) != 0;
                    float x0 = x;
                    if (code && bi == 0) x += f->pad;
                    int chunk = bi, k = bi;
                    while (k < e) {
                        int nk = cui__utf8_next(Q->p, e, k);
                        if (nk <= k) nk = k + 1;
                        float cw = cui__text_width(ui, qf, qs, Q->p + k, nk - k, false);
                        if (x + cw > W && x > 0 && k > chunk) {
                            md_seg_add(ui, L, in, ri, Q->p + chunk, k - chunk, x0, x - x0);
                            md_finish_line(L, &first, x, W, align);
                            x = 0; x0 = 0; chunk = k;
                        }
                        x += cw;
                        k = nk;
                    }
                    if (code && e == Q->len) x += f->pad;
                    md_seg_add(ui, L, in, ri, Q->p + chunk, e - chunk, x0, x - x0);
                }
            }
            if (e < Q->len) { bi = e; break; }
            ri++; bi = 0;
        }
    }
    if (L->n > first) md_finish_line(L, &first, x, W, align);
}

/* ------------------------------------------------------------------------ */
/* Painting                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    const md_seg *seg; int n;
    md_font f;
    cui_color fg, link_c, code_bg, math_c;
    const float *hover, *focus;   /* per link of the block */
    bool underline_rest;          /* links keep a full underline (text on a colored surface) */
} md_paint;

/* Slants the glyphs drawn since v0 around the baseline: a synthesized italic. */
static void md_shear(cui_ctx *ui, int v0, float base) {
    cui_xform x = ui->dl.xform_stack[ui->dl.xform_depth - 1];
    float by = base * ui->scale * x.scale + x.oy;
    for (int v = v0; v < ui->dl.vtx_count; v++) ui->dl.vtx[v].x += MD_SHEAR * (by - ui->dl.vtx[v].y);
}

static void md_paint_inline(cui_ctx *ui, cui_node *n, void *data) {
    md_paint *p = data;
    const md_font *f = &p->f;
    float fh = cui__font_line_height(ui, f->reg, f->size), asc = cui__font_ascent(ui, f->reg, f->size);
    float th = CUI_MAX(1.0f, f->size / 13.0f);
    for (int k = 0; k < p->n; k++) {
        const md_seg *s = &p->seg[k];
        float top = n->rect.y + (float)s->line * f->lh;
        float base = top + (f->lh - fh) * 0.5f + asc;
        float x = n->rect.x + s->x;
        cui_font font = md_font_of(f, s->style);
        float size = md_size_of(f, s->style);
        cui_color col = s->link >= 0 ? p->link_c : p->fg;
        if ((s->style & MS_MATH) && s->link < 0) col = p->math_c;
        float hv = s->link >= 0 ? p->hover[s->link] : 0;
        if (s->style & MS_CODE) {
            float ch = f->mono_size * 1.5f;
            cui_draw_rect(ui, (cui_rect){ x, top + (f->lh - ch) * 0.5f, s->w, ch }, CUI_CORNERS(4), p->code_bg);
        }
        float tx = x + (s->lead ? f->pad : 0);
        int v0 = ui->dl.vtx_count;
        cui__draw_text_line(ui, font, size, tx, base - cui__font_ascent(ui, font, size), col, s->p, s->len, false);
        if (s->style & MS_EM) md_shear(ui, v0, base);
        if (s->style & MS_STRIKE) cui_draw_rect(ui, (cui_rect){ x, base - f->size * 0.3f, s->w, th }, CUI_CORNERS(0), col);
        if (s->link >= 0) {
            float a = p->underline_rest ? 0.8f + 0.2f * hv : 0.35f + 0.65f * hv;
            cui_draw_rect(ui, (cui_rect){ x, base + CUI_MAX(1.5f, size * 0.14f), s->w, th }, CUI_CORNERS(0), cui_alpha(col, a));
            float fo = p->focus[s->link];
            if (fo > 0.01f) cui__draw_focus_ring(ui, (cui_rect){ x - 2, top + 1, s->w + 4, f->lh - 2 }, CUI_CORNERS(4), fo);
        }
    }
}

typedef struct { int kind; int depth; cui_color c; const char *num; md_font f; } md_mark_paint;   /* kind 0 bullet, 1 number */

static void md_paint_marker(cui_ctx *ui, cui_node *n, void *data) {
    md_mark_paint *p = data;
    cui_rect r = n->rect;
    float u = p->f.size / 13.0f;
    if (p->kind == 1) {
        float w = cui__text_width(ui, p->f.reg, p->f.size, p->num, -1, true);
        float fh = cui__font_line_height(ui, p->f.reg, p->f.size);
        cui__draw_text_line(ui, p->f.reg, p->f.size, r.x + r.w - 7 * u - w, r.y + (p->f.lh - fh) * 0.5f, p->c, p->num, -1, true);
        return;
    }
    cui_vec2 c = { r.x + 6 * u, r.y + p->f.lh * 0.5f };
    int shape = p->depth % 3;
    if (shape == 0) cui_draw_circle(ui, c, 2.5f * u, p->c);
    else if (shape == 1) {
        float rr = 2.75f * u;
        cui_draw_rect_border(ui, (cui_rect){ c.x - rr, c.y - rr, rr * 2, rr * 2 }, CUI_CORNERS(rr), cui__rgba(0, 0, 0, 0), p->c, 1.1f * u);
    } else {
        float rr = 2.25f * u;
        cui_draw_rect(ui, (cui_rect){ c.x - rr, c.y - rr, rr * 2, rr * 2 }, CUI_CORNERS(0.75f), p->c);
    }
}

static void md_paint_rail(cui_ctx *ui, cui_node *n, void *data) {
    cui_color *c = data;
    float w = 3.0f * CUI_MAX(0.8f, n->pad[3] / 14.0f);
    cui_draw_rect(ui, (cui_rect){ n->rect.x, n->rect.y, w, n->rect.h }, CUI_CORNERS(w * 0.5f), *c);
}

typedef struct { float w; } md_root_paint;
static void md_paint_root(cui_ctx *ui, cui_node *n, void *data) {
    md_root_paint *p = data;
    /* laid out for another width than it got: build once more at this one */
    if (fabsf(n->rect.w - p->w) > 0.5f) ui->animating = true;
}

/* ------------------------------------------------------------------------ */
/* Blocks                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    cui_ctx *ui;
    cui_id id;
    const char *src;
    int len, content_end;
    bool streaming, compact, fit, custom_color;
    float body;                  /* unscaled body size */
    float u;                     /* spacing unit: body / 13 */
    cui_color fg, fg2, link_c, code_bg, marker_c, rule_c, rail_c;
    int link_seq, code_seq, table_seq;
    cui_markdown_result *res;
} md_ctx;

typedef struct { float w; int depth; bool tight; cui_color fg; } md_box;

enum { BK_NONE, BK_PARA, BK_HEADING, BK_CODE, BK_LIST, BK_QUOTE, BK_HR, BK_TABLE, BK_MATH, BK_MERMAID };

static md_font md_make_font(md_ctx *c, float size, cui_font reg, cui_font bold, float lh_mul) {
    md_font f;
    f.reg = reg; f.bold = bold; f.mono = CUI_FONT_MONO;
    f.size = cui__fs(c->ui, size);
    f.mono_size = f.size * (12.0f / 13.0f);
    f.lh = f.size * lh_mul;
    f.pad = 4.0f * f.size / 13.0f;
    return f;
}
static md_font md_body(md_ctx *c) { return md_make_font(c, c->body, CUI_FONT_BODY, CUI_FONT_MEDIUM, c->compact ? 1.5f : 1.6f); }

static bool md_tail(md_ctx *c, md_line last) { return c->streaming && last.off + last.len >= c->content_end; }
static bool md_open_line(md_ctx *c, md_line L) { return c->streaming && L.off + L.len >= c->len; }

static void md_space(md_ctx *c, float h) {
    if (h > 0) cui__leaf(c->ui, 0, CUI_PX(1), CUI_PX(h));
}

static float md_gap_before(md_ctx *c, const md_box *b, int prev, int kind, int level) {
    if (prev == BK_NONE) return 0;
    float g = b->tight ? 4 : (c->compact ? 8 : 12);
    if (kind == BK_HEADING) g = c->compact ? 12 : (level <= 2 ? 24 : 18);
    else if (prev == BK_HEADING) g = c->compact ? 4 : 8;
    if (kind == BK_HR || prev == BK_HR) g = c->compact ? 10 : 16;
    return g * c->u;
}

static cui_id md_link_id(md_ctx *c, int seq) { return cui__hash(&seq, sizeof seq, c->id ^ 0x3D11Au); }

/* Builds a laid out inline block: one painted node, a hit box per link piece. */
static float md_inline_emit(md_ctx *c, const md_inline *in, const md_font *f, cui_color fg, int align, float W) {
    cui_ctx *ui = c->ui;
    md_lay L;
    md_layout(ui, in, f, CUI_MAX(W, 24.0f), align, &L);
    int base = c->link_seq;
    c->link_seq += in->nlinks;
    float *hover = cui__frame_alloc(ui, sizeof(float) * (size_t)(in->nlinks * 2 + 1)), *focus = hover + in->nlinks;
    cui_id *ids = cui__frame_alloc(ui, sizeof(cui_id) * (size_t)(in->nlinks + 1));
    for (int k = 0; k < in->nlinks; k++) {
        const md_link *lk = &in->links[k];
        bool shown = false;
        for (int q = 0; q < L.n && !shown; q++) shown = L.seg[q].link == k;
        ids[k] = 0;
        if (!shown || lk->pending) continue;   /* nothing to click yet */
        cui_id id = md_link_id(c, base + k);
        ids[k] = id;
        cui_interact r = cui__interact(ui, id, CUI_IF_FOCUSABLE);
        float *st = cui_state(ui, id, sizeof(float) * 2);
        cui__tween(ui, &st[0], r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
        cui__tween(ui, &st[1], r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
        hover[k] = st[0]; focus[k] = st[1];
        const char *url = NULL;
        if (r.hovered || r.clicked) url = lk->www ? cui__frame_printf(ui, "http://%.*s", lk->len, lk->url) : cui__frame_strdup(ui, lk->url, lk->len);
        if (r.hovered) { cui__set_cursor(ui, CUI_CURSOR_HAND); c->res->hovered_link = url; }
        if (r.clicked) c->res->link = url;
        if (!lk->autolink) { ui->last_id = id; cui_tooltip(ui, r.hovered && url ? url : ""); }
    }
    int leaf = cui__leaf(ui, 0, CUI_FILL, CUI_PX((float)L.lines * f->lh));
    md_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->seg = L.seg; p->n = L.n; p->f = *f;
    p->fg = fg; p->link_c = c->custom_color ? fg : c->link_c; p->code_bg = c->code_bg;
    p->math_c = c->custom_color ? fg : cui_mix(c->link_c, c->fg, 0.45f);
    p->hover = hover; p->focus = focus;
    p->underline_rest = c->custom_color;
    ui->nodes[leaf].paint = md_paint_inline;
    ui->nodes[leaf].paint_data = p;
    if (in->nlinks > 0) {
        cui__open(ui, leaf);
        for (int q = 0; q < L.n; q++) {
            const md_seg *s = &L.seg[q];
            if (s->link < 0 || !ids[s->link]) continue;
            float x0 = s->x, x1 = s->x + s->w;
            while (q + 1 < L.n && L.seg[q + 1].link == s->link && L.seg[q + 1].line == s->line) { q++; x1 = L.seg[q].x + L.seg[q].w; }
            int a = cui__absolute(ui, ids[s->link], x0, (float)s->line * f->lh, CUI_MAX(1.0f, x1 - x0), f->lh);
            cui__mark_interactive(ui, a, CUI_IF_FOCUSABLE);
        }
        cui__close(ui);
    }
    return c->fit ? md_runs_width(ui, in, f) : 0;
}

static float md_inline_block(md_ctx *c, const char *s, int n, const md_font *f, cui_color fg, bool tail, float W) {
    md_inline in = md_parse_inline(c->ui, s, n, tail);
    return md_inline_emit(c, &in, f, fg, CUI_TEXT_LEFT, W);
}

/* Joins paragraph lines: soft breaks become spaces, hard breaks '\n'. */
static const char *md_join(md_ctx *c, const md_line *L, int n, int *out_len) {
    if (n == 1) {
        md_line T = md_trim(L[0]);
        *out_len = T.len;
        return T.s;
    }
    int total = n;
    for (int k = 0; k < n; k++) total += L[k].len;
    char *buf = cui__frame_alloc(c->ui, (size_t)total + 1);
    int o = 0;
    for (int k = 0; k < n; k++) {
        int b;
        md_indent(L[k], &b);
        md_line T = md_skip(L[k], b);
        int e = T.len, sp = 0;
        while (e > 0 && md_sp(T.s[e - 1])) { e--; sp++; }
        bool last = k == n - 1, hard = false;
        if (!last) {
            if (sp >= 2) hard = true;
            else if (sp == 0 && e > 0 && T.s[e - 1] == '\\') { hard = true; e--; }
        }
        memcpy(buf + o, T.s, (size_t)e);
        o += e;
        if (!last) buf[o++] = hard ? '\n' : ' ';
    }
    buf[o] = 0;
    *out_len = o;
    return buf;
}

static float md_paragraph(md_ctx *c, const md_box *b, const md_line *L, int n) {
    int len;
    const char *s = md_join(c, L, n, &len);
    md_font f = md_body(c);
    return md_inline_block(c, s, len, &f, b->fg, md_tail(c, L[n - 1]), b->w);
}

static float md_heading(md_ctx *c, const md_box *b, int level, const char *s, int len, bool tail) {
    cui_theme *t = &c->ui->theme;
    float scale = c->body / t->text_body;
    float size;
    if (c->compact) size = level == 1 ? t->heading[3] : level == 2 ? t->text_lg : t->text_body;
    else size = level == 1 ? t->heading[1] : level == 2 ? t->heading[2] : level == 3 ? t->heading[3] : level == 4 ? t->text_lg : t->text_body;
    size *= scale;
    cui_font font = size >= 26 ? CUI_FONT_DISPLAY : CUI_FONT_MEDIUM;
    md_font f = md_make_font(c, size, font, font, size >= 26 ? 1.2f : 1.35f);
    return md_inline_block(c, s, len, &f, level >= 6 ? c->fg2 : b->fg, tail, b->w);
}

static void md_lower(char *d, const char *s, int n) {
    for (int k = 0; k < n; k++) d[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] - 'A' + 'a') : s[k];
    d[n] = 0;
}

static float md_code(md_ctx *c, const md_line *L, int n, int strip, md_line lang) {
    cui_ctx *ui = c->ui;
    int total = n + 1;
    for (int k = 0; k < n; k++) total += L[k].len;
    char *buf = cui__frame_alloc(ui, (size_t)total);
    int o = 0;
    float widest = 0;
    for (int k = 0; k < n; k++) {
        md_line T = md_strip_cols(L[k], strip);
        memcpy(buf + o, T.s, (size_t)T.len);
        o += T.len;
        if (k < n - 1) buf[o++] = '\n';
        if (c->fit) widest = CUI_MAX(widest, cui__text_width(ui, CUI_FONT_MONO, cui__fs(ui, c->body), T.s, T.len, true));
    }
    buf[o] = 0;
    char *lg = cui__frame_alloc(ui, 24);
    if (lang.len > 0 && lang.len < 24) md_lower(lg, lang.s, lang.len);
    else memcpy(lg, "text", 5);
    cui_code_opts co;
    memset(&co, 0, sizeof co);
    co.lang = lg;
    co.size = c->body;
    cui_code_block_ex(ui, cui__frame_printf(ui, "code%d", c->code_seq++), buf, &co);
    return c->fit ? widest + 42 : 0;
}

/* Display math: a tinted, centered panel. The expression is parsed into the
 * math IR and laid out for real — stacked fractions, raised/lowered scripts,
 * sqrt overlines, spanning matrix delimiters, italic letters. */
static float md_math_block(md_ctx *c, const md_box *b, const char *expr, int len, int prev) {
    cui_ctx *ui = c->ui;
    md_mit *ir = md_math_ir(ui, expr, len);
    md_space(c, md_gap_before(c, b, prev, BK_MATH, 0));
    if (!ir) return 0;
    md_font f = md_body(c);
    f.size *= 1.12f;
    md_mpaint mp;
    memset(&mp, 0, sizeof mp);
    mp.fs = f.size;
    mp.u = c->u;
    mp.lh = 0.22f * f.size;
    mp.fg = c->custom_color ? b->fg : cui_mix(c->link_c, b->fg, 0.45f);
    mp.bg = c->code_bg;
    mp.border = c->rule_c;
    float avail = (b->w > 60 ? b->w : 600) - 30 * c->u;
    md_m_layout(ui, ir, f.size, avail, &mp);
    float h = mp.h + 26 * c->u;
    cui_canvas(ui, cui__frame_printf(ui, "math%d", c->code_seq++), CUI_FILL,
               CUI_PX(h), md_math_paint_cb, &mp, sizeof mp);
    return 0;
}

/* ---- Mermaid flowchart ------------------------------------------------ */
/* Supported subset: graph/flowchart TD|LR, node declarations A[..], B(..),
 * C{..}, D((..)), and edges -->, ---, -.->, ==> with optional |label|.
 * Unknown directives are ignored; an empty graph falls back to a code block. */
typedef struct {
    char id[MD_MM_ID_CAP], label[MD_MM_LABEL_CAP];
    int shape, rank, ord;            /* 0 rect, 1 rounded, 2 pill, 3 diamond */
    float w, h, x, y;
} mm_node;
typedef struct {
    int a, b;
    char label[MD_MM_EDGE_CAP];
    bool arrow, back, back_side, dashed, self, side;
    float lane_f, out_frac, in_frac; /* route lane + attach offsets */
} mm_edge;
typedef struct {
    mm_node n[MD_MM_MAX_NODES];
    mm_edge e[MD_MM_MAX_EDGES];
    int nn, ne;
    bool lr;
    float nat_w, nat_h, cross, ch_lo, ch_hi;
} mm_graph;

typedef struct {
    mm_graph *g;
    float fs, u;
    cui_color fg, muted, surface, border, accent, panel, start;
} mm_paint;

static const char *mm_trim(const char *s, int n, int *out) {
    while (n > 0 && md_ws((unsigned char)*s)) { s++; n--; }
    while (n > 0 && md_ws((unsigned char)s[n - 1])) n--;
    *out = n; return s;
}

static bool mm_istart(const char *s, int n, const char *word) {
    int k = (int)strlen(word);
    if (k > n) return false;
    for (int i = 0; i < k; i++) {
        char a = s[i], b = word[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void mm_copy(char *dst, int cap, const char *s, int n) {
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(dst, s, (size_t)n);
    dst[n] = 0;
}

static int mm_add(mm_graph *g, const char *id, int idn, const char *label, int ln, int shape) {
    if (!g || idn <= 0) return -1;
    for (int i = 0; i < g->nn; i++) {
        if (!strncmp(g->n[i].id, id, (size_t)idn) && !g->n[i].id[idn]) {
            if (ln > 0 && !g->n[i].label[0]) mm_copy(g->n[i].label, MD_MM_LABEL_CAP, label, ln);
            if (shape != 0) g->n[i].shape = shape;
            return i;
        }
    }
    if (g->nn >= MD_MM_MAX_NODES) return -1;
    int i = g->nn++;
    mm_copy(g->n[i].id, MD_MM_ID_CAP, id, idn);
    if (ln > 0) mm_copy(g->n[i].label, MD_MM_LABEL_CAP, label, ln);
    else mm_copy(g->n[i].label, MD_MM_LABEL_CAP, id, idn);
    g->n[i].shape = shape;
    return i;
}

static int mm_operand(mm_graph *g, const char *s, int n) {
    int len;
    s = mm_trim(s, n, &len);
    if (len <= 0) return -1;
    int idn = 0;
    while (idn < len && ((s[idn] >= 'a' && s[idn] <= 'z') ||
           (s[idn] >= 'A' && s[idn] <= 'Z') || (s[idn] >= '0' && s[idn] <= '9') ||
           s[idn] == '_' || s[idn] == '-')) idn++;
    if (idn == 0) {
        char tmp[MD_MM_ID_CAP];
        snprintf(tmp, sizeof tmp, "n%d", g->nn);
        return mm_add(g, tmp, (int)strlen(tmp), s, len, 0);
    }
    char id[MD_MM_ID_CAP];
    mm_copy(id, MD_MM_ID_CAP, s, CUI_MIN(idn, MD_MM_ID_CAP - 1));
    idn = (int)strlen(id);
    const char *lb = s + idn;
    int ln = len - idn, shape = 0;
    if (ln >= 4 && lb[0] == '(' && lb[1] == '(' && lb[ln - 1] == ')' && lb[ln - 2] == ')') {
        shape = 2; lb += 2; ln -= 4;
    } else if (ln >= 2 && lb[0] == '[' && lb[ln - 1] == ']') {
        shape = (ln >= 4 && lb[1] == '[' && lb[ln - 2] == ']') ? 4 : 0;
        if (shape == 4) { lb += 2; ln -= 4; } else { lb++; ln -= 2; }
    } else if (ln >= 2 && lb[0] == '(' && lb[ln - 1] == ')') {
        shape = 1; lb++; ln -= 2;
    } else if (ln >= 2 && lb[0] == '{' && lb[ln - 1] == '}') {
        shape = 3; lb++; ln -= 2;
    } else if (ln >= 2 && lb[0] == '>' && lb[ln - 1] == ']') {
        lb++; ln -= 2;
    }
    if (ln >= 2 && *lb == '"' && lb[ln - 1] == '"') { lb++; ln -= 2; }
    if (ln <= 0) return mm_add(g, id, idn, NULL, 0, shape);
    return mm_add(g, id, idn, lb, ln, shape);
}

typedef struct { int left, right, ls, ln; bool arrow, dashed; const char *label_ptr; } mm_arrow;

static int mm_find_arrow(const char *s, int n, mm_arrow *r) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < n - 1; i++) {
        int tok = 0, next = 0;
        bool arrow = false, dashed = false;
        if (s[i] == '-' && s[i + 1] == '-') {
            if (i + 2 < n && s[i + 2] == '>') { tok = 3; arrow = true; }
            else if (i + 4 < n && s[i + 2] == '.' && s[i + 3] == '-' && s[i + 4] == '>') { tok = 5; arrow = true; dashed = true; }
            else if (i + 2 < n && s[i + 2] == '-' && (i + 3 >= n || s[i + 3] != '>')) { tok = 3; }
            else {                              /* "-- label -->" */
                for (int j = i + 2; j + 2 < n; j++) {
                    if (s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>') {
                        r->left = i; r->ls = i + 2; r->ln = j - i - 2;
                        r->right = j + 3; r->arrow = true;
                        return 1;
                    }
                }
                continue;
            }
        } else if (s[i] == '=' && s[i + 1] == '=') {
            if (i + 2 < n && s[i + 2] == '>') { tok = 3; arrow = true; }
            else if (i + 2 < n && s[i + 2] == '=') { tok = 3; }
            else continue;
        } else if (s[i] == '-' && s[i + 1] == '.' && i + 3 < n && s[i + 2] == '-' && s[i + 3] == '>') {
            tok = 4; arrow = true; dashed = true;
        } else continue;
        next = i + tok;
        r->left = i; r->right = next; r->arrow = arrow;
        if (arrow && next < n && s[next] == '|') {   /* -->|label| */
            int j = next + 1;
            while (j < n && s[j] != '|') j++;
            if (j < n) { r->ls = next + 1; r->ln = j - next - 1; r->right = j + 1; }
        }
        r->dashed = dashed;
        return 1;
    }
    return 0;
}

static void mm_add_edge(mm_graph *g, int from, int to, const mm_arrow *a) {
    if (from < 0 || to < 0 || g->ne >= MD_MM_MAX_EDGES) return;
    mm_edge *e = &g->e[g->ne++];
    e->a = from; e->b = to; e->arrow = a->arrow; e->dashed = a->dashed;
    e->self = from == to;
    const char *lp = a->label_ptr ? a->label_ptr : "";
    int ln = a->ln;
    while (ln > 0 && md_ws((unsigned char)*lp)) { lp++; ln--; }
    while (ln > 0 && md_ws((unsigned char)lp[ln - 1])) ln--;
    mm_copy(e->label, MD_MM_EDGE_CAP, lp, ln);
}

/* Accepts chained statements: A --> B --> C creates two edges. */
static void mm_statement(mm_graph *g, const char *s, int n) {
    const char *cur = s;
    int len = n, lhs = -1;
    mm_arrow pending;
    memset(&pending, 0, sizeof pending);
    while (len > 0) {
        mm_arrow a;
        if (mm_find_arrow(cur, len, &a)) {
            int node = mm_operand(g, cur, a.left);
            if (lhs >= 0) mm_add_edge(g, lhs, node, &pending);
            a.label_ptr = a.ln > 0 ? cur + a.ls : NULL;
            pending = a;
            lhs = node;
            cur += a.right; len -= a.right;
        } else {
            int node = mm_operand(g, cur, len);
            if (lhs >= 0) mm_add_edge(g, lhs, node, &pending);
            break;
        }
    }
}

static void mm_parse(mm_graph *g, const md_line *L, int n) {
    for (int i = 0; i < n; i++) {
        int len;
        const char *s = mm_trim(L[i].s, L[i].len, &len);
        if (len == 0) continue;
        if (mm_istart(s, len, "flowchart") || mm_istart(s, len, "graph")) {
            int k = (mm_istart(s, len, "flowchart") ? 9 : 5);
            while (k < len && md_ws((unsigned char)s[k])) k++;
            int b = k;
            while (k < len && ((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z'))) k++;
            g->lr = mm_istart(s + b, k - b, "lr") || mm_istart(s + b, k - b, "rl");
        }
        break;
    }
    for (int i = 0; i < n; i++) {
        int len;
        const char *s = mm_trim(L[i].s, L[i].len, &len);
        if (len == 0 || s[0] == '%') continue;
        if (mm_istart(s, len, "classDef") || mm_istart(s, len, "class ") ||
            mm_istart(s, len, "style ") || mm_istart(s, len, "linkStyle") ||
            mm_istart(s, len, "direction") || mm_istart(s, len, "subgraph") ||
            mm_istart(s, len, "end") || mm_istart(s, len, "graph ") ||
            mm_istart(s, len, "flowchart ")) continue;
        int st = 0;
        bool quote = false;
        for (int k = 0; k <= len; k++) {
            if (k < len) {
                if (s[k] == '"') quote = !quote;
                if (s[k] != ';' || quote) continue;
            }
            if (k > st) mm_statement(g, s + st, k - st);
            st = k + 1;
        }
    }
}

/* Longest-path layering with cycle breaking: a back edge never extends the
 * rank, so a cyclic flowchart stays compact instead of growing each pass. */
static int mm_rank_dfs(mm_graph *g, int i, unsigned char *state) {
    if (i < 0 || i >= g->nn) return 0;
    if (state[i] == 2) return g->n[i].rank;
    if (state[i] == 1) return 0;
    state[i] = 1;
    int rank = 0;
    for (int k = 0; k < g->ne; k++) {
        mm_edge *e = &g->e[k];
        if (e->a != i || e->b < 0 || e->b >= g->nn || state[e->b] == 1) continue;
        int rb = mm_rank_dfs(g, e->b, state) + 1;
        if (rb > rank) rank = rb;
    }
    state[i] = 2;
    g->n[i].rank = rank;
    return rank;
}

/* Stable insertion sort for one rank, keyed by barycenter then node order. */
static void mm_order_rank(int *v, float *key, int n) {
    for (int i = 1; i < n; i++) {
        int vi = v[i]; float ki = key[i]; int j = i - 1;
        while (j >= 0 && (key[j] > ki + 0.001f ||
              (fabsf(key[j] - ki) <= 0.001f && v[j] > vi))) {
            v[j + 1] = v[j]; key[j + 1] = key[j]; j--;
        }
        v[j + 1] = vi; key[j + 1] = ki;
    }
}

/* A few barycenter sweeps untangle edges between nodes on the same rank. */
static void mm_order(mm_graph *g, int byr[MD_MM_MAX_NODES][MD_MM_MAX_NODES],
                     int cnt[], int nr) {
    float key[MD_MM_MAX_NODES];
    for (int r = 0; r < nr; r++)
        for (int k = 0; k < cnt[r]; k++) g->n[byr[r][k]].ord = k;
    for (int pass = 0; pass < 4; pass++) {
        for (int r = 1; r < nr; r++) {
            for (int k = 0; k < cnt[r]; k++) {
                int id = byr[r][k];
                float sum = 0; int tot = 0;
                for (int q = 0; q < g->ne; q++) {
                    mm_edge *e = &g->e[q];
                    if (e->b == id && !e->back && g->n[e->a].rank < r) {
                        sum += (float)g->n[e->a].ord; tot++;
                    }
                }
                key[k] = tot ? sum / tot : (float)k;
            }
            mm_order_rank(byr[r], key, cnt[r]);
            for (int k = 0; k < cnt[r]; k++) g->n[byr[r][k]].ord = k;
        }
        for (int r = nr - 2; r >= 0; r--) {
            for (int k = 0; k < cnt[r]; k++) {
                int id = byr[r][k];
                float sum = 0; int tot = 0;
                for (int q = 0; q < g->ne; q++) {
                    mm_edge *e = &g->e[q];
                    if (e->a == id && !e->back && g->n[e->b].rank > r) {
                        sum += (float)g->n[e->b].ord; tot++;
                    }
                }
                key[k] = tot ? sum / tot : (float)k;
            }
            mm_order_rank(byr[r], key, cnt[r]);
            for (int k = 0; k < cnt[r]; k++) g->n[byr[r][k]].ord = k;
        }
    }
}

static void mm_layout(mm_graph *g, cui_ctx *ui, float fs, float u) {
    for (int i = 0; i < g->nn; i++) {
        mm_node *nd = &g->n[i];
        float tw = cui__text_width(ui, CUI_FONT_BODY, fs, nd->label, -1, false);
        nd->w = tw + 24 * u;
        nd->h = fs * 1.45f + 16 * u;
        nd->w = CUI_MAX(56 * u, CUI_MIN(nd->w, 240 * u));
        nd->h = CUI_MAX(30 * u, nd->h);
        if (nd->shape == 3) { nd->w += 34 * u; nd->h += 14 * u; }
        if (nd->shape == 2) { nd->w = CUI_MAX(nd->w, 68 * u); nd->h = CUI_MAX(nd->h, 34 * u); }
        nd->rank = 0;
    }
    unsigned char state[MD_MM_MAX_NODES];
    memset(state, 0, sizeof state);
    for (int i = 0; i < g->nn; i++) mm_rank_dfs(g, i, state);
    int max_rank = 0;
    for (int i = 0; i < g->nn; i++) max_rank = CUI_MAX(max_rank, g->n[i].rank);
    for (int i = 0; i < g->nn; i++) g->n[i].rank = max_rank - g->n[i].rank;
    for (int k = 0; k < g->ne; k++) {
        mm_edge *e = &g->e[k];
        bool bad = e->a < 0 || e->b < 0 || e->a >= g->nn || e->b >= g->nn;
        e->self = !bad && e->a == e->b;
        e->side = !bad && !e->self && g->n[e->a].rank == g->n[e->b].rank;
        e->back = !bad && !e->self && !e->side &&
                  g->n[e->b].rank < g->n[e->a].rank;
    }
    int byr[MD_MM_MAX_NODES][MD_MM_MAX_NODES];
    int cnt[MD_MM_MAX_NODES];
    memset(cnt, 0, sizeof cnt);
    int nr = 0;
    for (int i = 0; i < g->nn; i++) {
        int r = CUI_MIN(g->n[i].rank, MD_MM_MAX_NODES - 1);
        if (r + 1 > nr) nr = r + 1;
        byr[r][cnt[r]++] = i;
    }
    mm_order(g, byr, cnt, nr);
    float gapn = 20 * u, gapr = g->lr ? 64 * u : 58 * u;
    float total = 0, cross = 0;
    for (int r = 0; r < nr; r++) {
        float sw = 0, sh = 0, mw = 0, mh = 0;
        for (int k = 0; k < cnt[r]; k++) {
            mm_node *nd = &g->n[byr[r][k]];
            sw += nd->w; sh += nd->h;
            mw = CUI_MAX(mw, nd->w); mh = CUI_MAX(mh, nd->h);
        }
        sw += gapn * CUI_MAX(0, cnt[r] - 1);
        sh += gapn * CUI_MAX(0, cnt[r] - 1);
        if (g->lr) { total += mw; cross = CUI_MAX(cross, sh); }
        else { total += mh; cross = CUI_MAX(cross, sw); }
    }
    total += gapr * CUI_MAX(0, nr - 1);
    float m = 26 * u;
    g->cross = cross;
    g->ch_lo = 12 * u;
    g->ch_hi = cross + m + 14 * u;
    if (g->lr) { g->nat_w = total; g->nat_h = cross + 2 * m; }
    else { g->nat_w = cross + 2 * m; g->nat_h = total; }
    float cur = 0;
    for (int r = 0; r < nr; r++) {
        float sw = 0, sh = 0, mw = 0, mh = 0;
        for (int k = 0; k < cnt[r]; k++) {
            mm_node *nd = &g->n[byr[r][k]];
            sw += nd->w; sh += nd->h;
            mw = CUI_MAX(mw, nd->w); mh = CUI_MAX(mh, nd->h);
        }
        sw += gapn * CUI_MAX(0, cnt[r] - 1);
        sh += gapn * CUI_MAX(0, cnt[r] - 1);
        float x = 0, y = 0;
        for (int k = 0; k < cnt[r]; k++) {
            mm_node *nd = &g->n[byr[r][k]];
            if (g->lr) {
                nd->x = cur + (mw - nd->w) * 0.5f;
                nd->y = m + (cross - sh) * 0.5f + y + (mh - nd->h) * 0.5f;
                y += nd->h + gapn;
            } else {
                nd->x = m + (cross - sw) * 0.5f + x + (mw - nd->w) * 0.5f;
                nd->y = cur + (mh - nd->h) * 0.5f;
                x += nd->w + gapn;
            }
        }
        cur += (g->lr ? mw : mh) + gapr;
    }
    for (int k = 0; k < g->ne; k++) {
        mm_edge *e = &g->e[k];
        if (!e->back) continue;
        mm_node *a = &g->n[e->a];
        float pos = g->lr ? a->y + a->h * 0.5f : a->x + a->w * 0.5f;
        e->back_side = pos < m + cross * 0.5f;   /* left for TD, top for LR */
    }
    /* Stagger forward edges: per rank pair, edges share the gap in insertion
     * order of their endpoints; attach points spread over the node side. */
    {
        int pair[MD_MM_MAX_NODES][MD_MM_MAX_NODES];
        int outc[MD_MM_MAX_NODES], inc[MD_MM_MAX_NODES];
        int outidx[MD_MM_MAX_EDGES], inidx[MD_MM_MAX_EDGES];
        memset(pair, 0, sizeof pair);
        memset(outc, 0, sizeof outc);
        memset(inc, 0, sizeof inc);
        int pass;
        for (pass = 0; pass < 2; pass++) {
            for (int k = 0; k < g->ne; k++) {
                mm_edge *e = &g->e[k];
                if (e->back || e->self || e->side) continue;
                if (e->a < 0 || e->a >= g->nn || e->b < 0 || e->b >= g->nn) continue;
                int ra = g->n[e->a].rank, rb = g->n[e->b].rank;
                if (ra < 0 || ra >= MD_MM_MAX_NODES || rb < 0 || rb >= MD_MM_MAX_NODES)
                    continue;
                if (pass == 0) {
                    outidx[k] = outc[e->a]++;
                    inidx[k] = inc[e->b]++;
                    pair[ra][rb]++;
                } else {
                    /* spread attach across 30%..70% of the node side */
                    e->out_frac = outc[e->a] > 1
                        ? 0.30f + 0.40f * outidx[k] / (outc[e->a] - 1) : 0.5f;
                    e->in_frac = inc[e->b] > 1
                        ? 0.30f + 0.40f * inidx[k] / (inc[e->b] - 1) : 0.5f;
                }
            }
        }
        for (int k = 0; k < g->ne; k++) {
            mm_edge *e = &g->e[k];
            if (e->back || e->self || e->side) continue;
            if (e->a < 0 || e->a >= g->nn || e->b < 0 || e->b >= g->nn) continue;
            int ra = g->n[e->a].rank, rb = g->n[e->b].rank;
            if (ra < 0 || ra >= MD_MM_MAX_NODES || rb < 0 || rb >= MD_MM_MAX_NODES)
                continue;
            int total = pair[ra][rb], before = 0;
            for (int q = 0; q < k; q++) {
                mm_edge *f = &g->e[q];
                if (f->back || f->self || f->side) continue;
                if (g->n[f->a].rank == ra && g->n[f->b].rank == rb) before++;
            }
            e->lane_f = total > 0 ? (float)(before + 1) / (total + 1) : 0.5f;
        }
    }
}

static void mm_arrow_head(cui_ctx *ui, cui_vec2 tip, float ang, float scale, cui_color col) {
    float len = 8 * scale, wid = 3.6f * scale;
    cui_vec2 p[3] = {
        tip,
        { tip.x - len * cosf(ang) + wid * sinf(ang), tip.y - len * sinf(ang) - wid * cosf(ang) },
        { tip.x - len * cosf(ang) - wid * sinf(ang), tip.y - len * sinf(ang) + wid * cosf(ang) }
    };
    cui_draw_polygon(ui, p, 3, col);
}

/* Forward edges run orthogonally through the rank gap (staggered lanes),
 * same-rank edges cross the facing sides, cycle (back) edges take a channel
 * outside the graph, and self edges loop around the free side of the node.
 * Besides the polyline this reports the local segment direction at the label
 * anchor (dir, unit or 0,0) and the side the label should move to (nrm):
 * labels sit beside the line instead of straddling it. */
static void mm_edge_points(mm_graph *g, mm_edge *e, float u,
                           cui_vec2 *pts, int *np, cui_vec2 *lp,
                           float dir[2], float nrm[2]) {
    mm_node *a = &g->n[e->a], *b = &g->n[e->b];
    float acx = a->x + a->w * 0.5f, acy = a->y + a->h * 0.5f;
    float bcx = b->x + b->w * 0.5f, bcy = b->y + b->h * 0.5f;
    dir[0] = dir[1] = nrm[0] = nrm[1] = 0;
    if (e->self) {
        float reach = 18 * u;
        if (!g->lr) {
            float x0 = a->x + a->w, y0 = a->y + a->h * 0.30f;
            float y1 = a->y + a->h * 0.72f;
            pts[0] = (cui_vec2){ x0, y0 };
            pts[1] = (cui_vec2){ x0 + reach, y0 };
            pts[2] = (cui_vec2){ x0 + reach, y1 };
            pts[3] = (cui_vec2){ x0, y1 };
            *np = 4;
            *lp = (cui_vec2){ x0 + reach * 0.55f, y1 };
            dir[0] = 1; nrm[1] = 1;             /* below the bottom run */
        } else {
            float y0 = a->y + a->h, x0 = a->x + a->w * 0.30f;
            float x1 = a->x + a->w * 0.72f;
            pts[0] = (cui_vec2){ x0, y0 };
            pts[1] = (cui_vec2){ x0, y0 + reach };
            pts[2] = (cui_vec2){ x1, y0 + reach };
            pts[3] = (cui_vec2){ x1, y0 };
            *np = 4;
            *lp = (cui_vec2){ x1, y0 + reach * 0.55f };
            dir[1] = 1; nrm[0] = 1;             /* right of the outer run */
        }
        return;
    }
    if (e->side) {
        if (g->lr) {
            bool down = bcy > acy;
            pts[0] = (cui_vec2){ acx, down ? a->y + a->h : a->y };
            pts[1] = (cui_vec2){ bcx, down ? b->y : b->y + b->h };
        } else {
            bool right = bcx > acx;
            pts[0] = (cui_vec2){ right ? a->x + a->w : a->x, acy };
            pts[1] = (cui_vec2){ right ? b->x : b->x + b->w, bcy };
        }
        *np = 2;
        *lp = (cui_vec2){ (pts[0].x + pts[1].x) * 0.5f,
                          (pts[0].y + pts[1].y) * 0.5f };
        {   /* perpendicular to the (usually diagonal) run, tilted upward */
            float dx = pts[1].x - pts[0].x, dy = pts[1].y - pts[0].y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.01f) {
                float nx = -dy / len, ny = dx / len;
                if (g->lr ? nx < 0 : ny > 0) { nx = -nx; ny = -ny; }
                dir[0] = dx / len; dir[1] = dy / len;
                nrm[0] = nx; nrm[1] = ny;
            }
        }
        return;
    }
    if (!e->back) {
        float ax = g->lr ? a->y + a->h * e->out_frac
                         : a->x + a->w * e->out_frac;
        float bx = g->lr ? b->y + b->h * e->in_frac
                         : b->x + b->w * e->in_frac;
        if (!g->lr) {
            float a0 = a->y + a->h, b0 = b->y;
            float lane = a0 + (b0 - a0) * e->lane_f;
            if (fabsf(ax - bx) < 1.0f) {
                pts[0] = (cui_vec2){ ax, a0 };
                pts[1] = (cui_vec2){ bx, b0 };
                *np = 2;
                *lp = (cui_vec2){ ax, (a0 + b0) * 0.5f };
                dir[1] = 1; nrm[0] = 1;         /* vertical run: right side */
            } else {
                pts[0] = (cui_vec2){ ax, a0 };
                pts[1] = (cui_vec2){ ax, lane };
                pts[2] = (cui_vec2){ bx, lane };
                pts[3] = (cui_vec2){ bx, b0 };
                *np = 4;
                *lp = (cui_vec2){ (ax + bx) * 0.5f, lane };
                dir[0] = 1; nrm[1] = -1;        /* horizontal lane: above */
            }
        } else {
            float a0 = a->x + a->w, b0 = b->x;
            float lane = a0 + (b0 - a0) * e->lane_f;
            if (fabsf(ax - bx) < 1.0f) {
                pts[0] = (cui_vec2){ a0, ax };
                pts[1] = (cui_vec2){ b0, bx };
                *np = 2;
                *lp = (cui_vec2){ (a0 + b0) * 0.5f, ax };
                dir[0] = 1; nrm[1] = -1;        /* horizontal run: above */
            } else {
                pts[0] = (cui_vec2){ a0, ax };
                pts[1] = (cui_vec2){ lane, ax };
                pts[2] = (cui_vec2){ lane, bx };
                pts[3] = (cui_vec2){ b0, bx };
                *np = 4;
                *lp = (cui_vec2){ lane, (ax + bx) * 0.5f };
                dir[1] = 1; nrm[0] = 1;         /* vertical lane: right side */
            }
        }
        return;
    }
    if (!g->lr) {
        float ch = e->back_side ? g->ch_lo : g->ch_hi;
        float sx = e->back_side ? a->x : a->x + a->w;
        float tx = e->back_side ? b->x : b->x + b->w;
        pts[0] = (cui_vec2){ sx, acy };
        pts[1] = (cui_vec2){ ch, acy };
        pts[2] = (cui_vec2){ ch, bcy };
        pts[3] = (cui_vec2){ tx, bcy };
        *np = 4;
        *lp = (cui_vec2){ (ch + tx) * 0.5f, bcy };
        dir[0] = 1; nrm[1] = -1;                /* horizontal run: above */
    } else {
        float ch = e->back_side ? g->ch_lo : g->ch_hi;
        float sy = e->back_side ? a->y : a->y + a->h;
        float ty = e->back_side ? b->y : b->y + b->h;
        pts[0] = (cui_vec2){ acx, sy };
        pts[1] = (cui_vec2){ acx, ch };
        pts[2] = (cui_vec2){ bcx, ch };
        pts[3] = (cui_vec2){ bcx, ty };
        *np = 4;
        *lp = (cui_vec2){ bcx, (ch + ty) * 0.5f };
        dir[1] = 1; nrm[0] = 1;                 /* vertical run: right side */
    }
}

static void mm_paint_cb(cui_ctx *ui, cui_rect rect, void *user) {
    mm_paint *p = (mm_paint *)user;
    mm_graph *g = p->g;
    cui_draw_rect(ui, rect, CUI_CORNERS(10), p->panel);
    cui_draw_rect_border(ui, rect, CUI_CORNERS(10), cui__rgba(0, 0, 0, 0), p->border, 1);
    if (!g || g->nn == 0) return;
    float pad = 14 * p->u;
    float avail_w = rect.w - 2 * pad, avail_h = rect.h - 2 * pad;
    if (avail_w <= 0 || avail_h <= 0) return;
    float sc = CUI_MIN(avail_w / CUI_MAX(g->nat_w, 1.0f),
                       avail_h / CUI_MAX(g->nat_h, 1.0f));
    if (sc > 1.15f) sc = 1.15f;
    if (sc < 0.15f) sc = 0.15f;
    float ox = rect.x + (rect.w - g->nat_w * sc) * 0.5f;
    float oy = rect.y + (rect.h - g->nat_h * sc) * 0.5f;
#define MMX(v) (ox + (v) * sc)
#define MMY(v) (oy + (v) * sc)
    /* Pass 1: every edge's lines and arrowheads. Labels come last (pass 2)
     * so later edges never draw through an earlier label's background. */
    cui_vec2 lbl_pt[MD_MM_MAX_EDGES];
    const char *lbl_txt[MD_MM_MAX_EDGES];
    cui_color lbl_col[MD_MM_MAX_EDGES];
    float lbl_dir[MD_MM_MAX_EDGES][2], lbl_nrm[MD_MM_MAX_EDGES][2];
    int lbl_n = 0;
    for (int k = 0; k < g->ne; k++) {
        mm_edge *e = &g->e[k];
        if (e->a < 0 || e->a >= g->nn || e->b < 0 || e->b >= g->nn) continue;
        cui_vec2 pts[4], lp;
        float edir[2], enorm[2];
        int np = 0;
        mm_edge_points(g, e, p->u, pts, &np, &lp, edir, enorm);
        cui_color col = p->muted;
        if (e->back) col.a = (uint8_t)(col.a * 0.7f);
        float lw = CUI_MAX(1.0f, 1.6f * sc);
#define MMP(q) (cui_vec2){ MMX(pts[q].x), MMY(pts[q].y) }
        if (!e->dashed) {
            for (int q = 1; q < np; q++)
                cui_draw_line(ui, MMP(q - 1), MMP(q), lw, col);
        } else {                                /* -.-> : hand-drawn dashes */
            float dash = 5.5f * sc, gap = 4 * sc;
            for (int q = 1; q < np; q++) {
                cui_vec2 p0 = MMP(q - 1), p1 = MMP(q);
                float dx = p1.x - p0.x, dy = p1.y - p0.y;
                float len = sqrtf(dx * dx + dy * dy);
                for (float t = 0; len > 0.01f && t < len; t += dash + gap) {
                    float t1 = CUI_MIN(t + dash, len);
                    cui_draw_line(ui,
                                  (cui_vec2){ p0.x + dx * t / len, p0.y + dy * t / len },
                                  (cui_vec2){ p0.x + dx * t1 / len, p0.y + dy * t1 / len },
                                  lw, col);
                }
            }
        }
        if (e->arrow && np >= 2) {
            cui_vec2 tip = MMP(np - 1);
            cui_vec2 prev = MMP(np - 2);
            mm_arrow_head(ui, tip, atan2f(tip.y - prev.y, tip.x - prev.x), sc, col);
        }
#undef MMP
        if (e->label[0] && lbl_n < MD_MM_MAX_EDGES) {
            lbl_pt[lbl_n] = lp;
            lbl_txt[lbl_n] = e->label;
            lbl_col[lbl_n] = col;
            lbl_dir[lbl_n][0] = edir[0]; lbl_dir[lbl_n][1] = edir[1];
            lbl_nrm[lbl_n][0] = enorm[0]; lbl_nrm[lbl_n][1] = enorm[1];
            lbl_n++;
        }
    }
    /* Pass 2: labels beside the line (above horizontal runs, right of
     * vertical ones, offset along the normal for diagonals), on opaque
     * plates, always above every line. */
    for (int k = 0; k < lbl_n; k++) {
        float lf = CUI_MAX(9.0f, p->fs * 0.82f * sc);
        float tw = cui__text_width(ui, CUI_FONT_BODY, lf, lbl_txt[k], -1, false);
        float mx = MMX(lbl_pt[k].x), my = MMY(lbl_pt[k].y);
        float ph = lf * 1.64f, pw = tw + 10, gap = 3;
        float dx = lbl_dir[k][0], dy = lbl_dir[k][1];
        float px, py;                           /* plate top-left */
        if (dx == 0 && dy == 0) {               /* no hint: straddle anchor */
            px = mx - pw * 0.5f; py = my - ph * 0.5f;
        } else if (fabsf(dy) < 0.25f) {         /* horizontal run: above/below */
            px = mx - pw * 0.5f;
            py = lbl_nrm[k][1] < 0 ? my - gap - ph : my + gap;
        } else if (fabsf(dx) < 0.25f) {         /* vertical run: left/right */
            px = lbl_nrm[k][0] < 0 ? mx - gap - pw : mx + gap;
            py = my - ph * 0.5f;
        } else {                                /* diagonal: push off the line */
            float off = ph * 0.5f + gap;
            px = mx + lbl_nrm[k][0] * off - pw * 0.5f;
            py = my + lbl_nrm[k][1] * off - ph * 0.5f;
        }
        cui_rect plate = { px, py, pw, ph };
        cui_draw_rect(ui, plate, CUI_CORNERS(4), p->panel);
        cui_draw_rect_border(ui, plate, CUI_CORNERS(4), cui__rgba(0, 0, 0, 0),
                             p->border, 1);
        cui_draw_text(ui, CUI_FONT_BODY, lf,
                      (cui_vec2){ px + 5, py + lf * 0.16f },
                      lbl_col[k], lbl_txt[k], -1);
    }
    for (int i = 0; i < g->nn; i++) {
        mm_node *nd = &g->n[i];
        cui_rect r = { MMX(nd->x), MMY(nd->y), nd->w * sc, nd->h * sc };
        float bw = CUI_MAX(1.0f, 1.3f * sc);
        bool start = nd->rank == 0;
        cui_color fill = start ? p->start : p->surface;
        cui_color border = start ? p->accent : p->border;
        float shadow_dx = 1.2f * sc, shadow_dy = 2.4f * sc;
        cui_color shadow = cui__rgba(0, 0, 0, 26);
        if (nd->shape == 3) {
            cui_vec2 v[4] = {
                { r.x + r.w * 0.5f, r.y }, { r.x + r.w, r.y + r.h * 0.5f },
                { r.x + r.w * 0.5f, r.y + r.h }, { r.x, r.y + r.h * 0.5f }
            };
            cui_vec2 vs[4];
            for (int q = 0; q < 4; q++)
                vs[q] = (cui_vec2){ v[q].x + shadow_dx, v[q].y + shadow_dy };
            cui_draw_polygon(ui, vs, 4, shadow);
            cui_draw_polygon(ui, v, 4, fill);
            cui_draw_polyline(ui, v, 4, bw, border, true);
        } else if (nd->shape == 4) {
            /* subroutine: rectangle with double side bars */
            cui_draw_rect(ui, (cui_rect){ r.x + shadow_dx, r.y + shadow_dy,
                                          r.w, r.h }, CUI_CORNERS(2 * sc), shadow);
            cui_draw_rect_border(ui, r, CUI_CORNERS(2 * sc), fill, border, bw);
            cui_draw_line(ui, (cui_vec2){ r.x + r.w * 0.12f, r.y },
                          (cui_vec2){ r.x + r.w * 0.12f, r.y + r.h }, bw, border);
            cui_draw_line(ui, (cui_vec2){ r.x + r.w * 0.88f, r.y },
                          (cui_vec2){ r.x + r.w * 0.88f, r.y + r.h }, bw, border);
        } else {
            float rad = (nd->shape == 2 ? r.h * 0.5f :
                         (nd->shape == 1 ? 10 : 6) * sc);
            cui_draw_rect(ui, (cui_rect){ r.x + shadow_dx, r.y + shadow_dy,
                                          r.w, r.h }, CUI_CORNERS(rad), shadow);
            cui_draw_rect_border(ui, r, CUI_CORNERS(rad), fill, border, bw);
        }
        float lf = CUI_MAX(9.0f, p->fs * sc * (nd->shape == 3 ? 0.86f : 1.0f));
        float tw = cui__text_width(ui, CUI_FONT_BODY, lf, nd->label, -1, false);
        cui_draw_text(ui, CUI_FONT_BODY, lf,
                      (cui_vec2){ r.x + (r.w - tw) * 0.5f, r.y + (r.h - lf * 1.2f) * 0.5f },
                      p->fg, nd->label, -1);
    }
#undef MMX
#undef MMY
}

static float md_mermaid(md_ctx *c, const md_box *b, const md_line *L, int n,
                        int strip, int prev) {
    cui_ctx *ui = c->ui;
    int total = 1;
    for (int k = 0; k < n; k++) total += L[k].len + 1;
    md_line *body = cui__frame_alloc(ui, sizeof(md_line) * (size_t)(n + 1));
    char *buf = cui__frame_alloc(ui, (size_t)total + 1);
    int o = 0;
    for (int k = 0; k < n; k++) {
        body[k] = md_strip_cols(L[k], strip);
        memcpy(buf + o, body[k].s, (size_t)body[k].len);
        o += body[k].len;
        if (k < n - 1) buf[o++] = '\n';
    }
    buf[o] = 0;
    mm_graph *g = cui__frame_alloc(ui, sizeof *g);
    memset(g, 0, sizeof *g);
    mm_parse(g, body, n);
    if (g->nn == 0) {                       /* not a flowchart we understand */
        md_space(c, md_gap_before(c, b, prev, BK_CODE, 0));
        md_line none = { "", 0, 0 };
        return md_code(c, body, n, 0, none);
    }
    mm_paint mp;
    memset(&mp, 0, sizeof mp);
    mp.g = g;
    mp.u = c->u;
    mp.fs = cui__fs(ui, c->body * 0.94f);
    mp.fg = b->fg;
    mp.muted = c->fg2;
    mp.surface = CUI_C(ui, SURFACE_RAISED);
    mp.border = c->rule_c;
    mp.accent = c->link_c;
    mp.panel = CUI_C(ui, ACTIVE);
    mp.start = CUI_C(ui, ACCENT_SUBTLE);
    mm_layout(g, ui, mp.fs, c->u);
    float h = g->nat_h + 34 * c->u;
    h = CUI_MAX((g->lr ? 96 : 140) * c->u, CUI_MIN(h, 520 * c->u));
    md_space(c, md_gap_before(c, b, prev, BK_MERMAID, 0));
    cui_canvas(ui, cui__frame_printf(ui, "mermaid%d", c->table_seq++), CUI_FILL,
               CUI_PX(h), mm_paint_cb, &mp, sizeof mp);
    return 0;
}

/* ---- block starts ---- */

typedef struct { bool ordered; char ch; int num, w; } md_marker;

static bool md_list_marker(md_line T, md_marker *m) {
    if (T.len < 1) return false;
    char c0 = T.s[0];
    if (c0 == '-' || c0 == '*' || c0 == '+') {
        if (T.len > 1 && !md_sp(T.s[1])) return false;
        m->ordered = false; m->ch = c0; m->num = 0; m->w = 1;
        return true;
    }
    int i = 0, num = 0;
    while (i < T.len && i < 9 && T.s[i] >= '0' && T.s[i] <= '9') { num = num * 10 + (T.s[i] - '0'); i++; }
    if (i == 0 || i >= T.len || (T.s[i] != '.' && T.s[i] != ')')) return false;
    if (i + 1 < T.len && !md_sp(T.s[i + 1])) return false;
    m->ordered = true; m->ch = T.s[i]; m->num = num; m->w = i + 1;
    return true;
}

static bool md_hrule(md_line T) {
    char ch = 0;
    int k = 0;
    for (int i = 0; i < T.len; i++) {
        char x = T.s[i];
        if (md_sp(x)) continue;
        if (!ch) { if (x != '-' && x != '*' && x != '_') return false; ch = x; }
        else if (x != ch) return false;
        k++;
    }
    return k >= 3;
}

/* setext underline: 1 for ===, 2 for ---, else 0 */
static int md_setext(md_line T) {
    T = md_trim(T);
    if (T.len < 1 || (T.s[0] != '=' && T.s[0] != '-')) return 0;
    for (int i = 0; i < T.len; i++) if (T.s[i] != T.s[0]) return 0;
    return T.s[0] == '=' ? 1 : 2;
}

static bool md_atx(md_line T, int *level, md_line *content) {
    int k = 0;
    while (k < T.len && T.s[k] == '#') k++;
    if (k < 1 || k > 6 || (k < T.len && !md_sp(T.s[k]))) return false;
    md_line C = md_trim(md_skip(T, k));
    int h = C.len;
    while (h > 0 && C.s[h - 1] == '#') h--;
    if (h < C.len && (h == 0 || md_sp(C.s[h - 1]))) { C.len = h; while (C.len > 0 && md_sp(C.s[C.len - 1])) C.len--; }
    *level = k; *content = C;
    return true;
}

static bool md_fence(md_line T, char *ch, int *flen, md_line *info) {
    if (T.len < 3 || (T.s[0] != '`' && T.s[0] != '~')) return false;
    int k = 0;
    while (k < T.len && T.s[k] == T.s[0]) k++;
    if (k < 3) return false;
    md_line I = md_trim(md_skip(T, k));
    if (T.s[0] == '`' && md_has(I, '`')) return false;
    int w = 0;
    while (w < I.len && !md_sp(I.s[w])) w++;
    I.len = w;
    *ch = T.s[0]; *flen = k; *info = I;
    return true;
}

static bool md_fence_close(md_line L, char ch, int flen) {
    int b;
    if (md_indent(L, &b) >= 4) return false;
    md_line T = md_skip(L, b);
    int k = 0;
    while (k < T.len && T.s[k] == ch) k++;
    return k >= flen && md_blank(md_skip(T, k));
}

/* Does line L start a block that ends a paragraph? */
static bool md_interrupts(md_line L) {
    int b;
    if (md_blank(L) || md_indent(L, &b) >= 4) return false;
    md_line T = md_skip(L, b), I;
    char ch;
    int k;
    if (md_fence(T, &ch, &k, &I) || md_atx(T, &k, &I) || T.s[0] == '>' || md_hrule(T)) return true;
    if (T.len >= 2 && T.s[0] == '$' && T.s[1] == '$') return true;   /* display math */
    md_marker m;
    if (md_list_marker(T, &m)) return !md_blank(md_skip(T, m.w)) && (!m.ordered || m.num == 1);
    return false;
}

static int md_cells(md_line T, md_line *cells, int max) {
    T = md_trim(T);
    int i = 0, n = T.len;
    if (i < n && T.s[i] == '|') i++;
    if (n > i && T.s[n - 1] == '|' && !(n - 2 >= i && T.s[n - 2] == '\\')) n--;
    int count = 0, st = i;
    for (int k = i; k <= n; k++) {
        if (k == n || (T.s[k] == '|' && !(k > i && T.s[k - 1] == '\\'))) {
            if (count < max) {
                md_line C = { T.s + st, k - st, T.off + st };
                cells[count] = md_trim(C);
            }
            count++;
            st = k + 1;
        }
    }
    return count;
}

/* Alignment row: column count, or 0. partial accepts an unfinished last cell. */
static int md_delim_row(md_line T, uint8_t *align, int max, bool partial) {
    md_line cells[MD_MAX_COLS];
    if (!md_has(T, '|') || (!md_has(T, '-') && !partial)) return 0;
    int n = md_cells(T, cells, MD_MAX_COLS);
    for (int k = 0; k < n && k < MD_MAX_COLS; k++) {
        md_line C = cells[k];
        bool left = C.len > 0 && C.s[0] == ':', right = C.len > 1 && C.s[C.len - 1] == ':';
        int dashes = 0;
        for (int q = left ? 1 : 0; q < C.len - (right ? 1 : 0); q++) { if (C.s[q] != '-') return 0; dashes++; }
        if (!dashes && !(partial && k == n - 1)) return 0;
        if (k < max) align[k] = (uint8_t)(left && right ? CUI_TEXT_CENTER : right ? CUI_TEXT_RIGHT : CUI_TEXT_LEFT);
    }
    return n;
}

/* A table starts at line i: header cells and their alignments. */
static int md_table_at(md_ctx *c, const md_line *L, int n, int i, uint8_t *align) {
    md_line H = L[i];
    if (md_blank(H) || md_indent(H, NULL) >= 4 || !md_has(H, '|')) return 0;
    md_line cells[MD_MAX_COLS];
    int nh = md_cells(H, cells, MD_MAX_COLS);
    for (int k = 0; k < MD_MAX_COLS; k++) align[k] = CUI_TEXT_LEFT;
    if (i + 1 < n && md_indent(L[i + 1], NULL) < 4) {
        bool partial = md_open_line(c, L[i + 1]);
        int nd = md_delim_row(L[i + 1], align, MD_MAX_COLS, partial);
        if (nd == nh || (partial && nd > 0 && nd <= nh)) return nh;
    }
    /* still arriving: a header row that waits for its alignment row */
    if (md_open_line(c, H) && md_trim(H).s[0] == '|') return nh;
    return 0;
}

static float md_blocks(md_ctx *c, const md_line *L, int n, const md_box *b);

static int md_list(md_ctx *c, const md_box *b, const md_line *L, int n, int i, float *nat) {
    cui_ctx *ui = c->ui;
    typedef struct { int start, end, content_col; md_line first; bool task, checked; int task_off; } md_item;
    md_item *items = cui__frame_alloc(ui, sizeof(md_item) * (size_t)(n - i + 1));
    int count = 0;
    bool loose = false;
    md_marker m0;
    memset(&m0, 0, sizeof m0);
    int j = i;
    while (j < n) {
        int bytes, ind = md_indent(L[j], &bytes);
        md_line T = md_skip(L[j], bytes);
        md_marker m;
        if (ind >= 4 || !md_list_marker(T, &m)) break;
        if (count > 0 && (m.ordered != m0.ordered || m.ch != m0.ch)) break;
        if (count == 0) m0 = m;
        md_item *it = &items[count++];
        md_line R = md_skip(T, m.w);
        int sb, sp = md_indent(R, &sb);
        if (md_blank(R)) { it->content_col = ind + m.w + 1; R = md_skip(R, R.len); }
        else if (sp > 4) { it->content_col = ind + m.w + 1; R = md_skip(R, 1); }
        else { it->content_col = ind + m.w + sp; R = md_skip(R, sb); }
        /* task item: [ ], [x] or [X] then a space or the end */
        it->task = false;
        if (R.len >= 3 && R.s[0] == '[' && R.s[2] == ']' && (R.s[1] == ' ' || R.s[1] == 'x' || R.s[1] == 'X') && (R.len == 3 || md_sp(R.s[3]))) {
            it->task = true; it->checked = R.s[1] != ' '; it->task_off = R.off + 1;
            R = md_skip(R, R.len > 3 ? 4 : 3);
        }
        it->first = R;
        it->start = j;
        int last = j;
        j++;
        while (j < n) {
            if (md_blank(L[j])) { j++; continue; }
            int ib, indj = md_indent(L[j], &ib);
            md_line Tj = md_skip(L[j], ib);
            md_marker mj;
            bool marker = indj < 4 + ind && md_list_marker(Tj, &mj);
            if (indj >= it->content_col) {}
            else if (marker && indj > ind) {}                 /* nested a little short of the content column */
            else if (j == last + 1 && !md_interrupts(L[j]) && !marker) {}   /* lazy continuation */
            else break;
            last = j;
            j++;
        }
        it->end = last + 1;
        for (int q = it->start + 1; q < it->end; q++) if (md_blank(L[q])) loose = true;
        /* the next item follows, maybe after blank lines */
        int k = it->end;
        while (k < n && md_blank(L[k])) k++;
        if (k >= n) { j = it->end; break; }
        int kb, indk = md_indent(L[k], &kb);
        md_marker mk;
        if (indk >= 4 || !md_list_marker(md_skip(L[k], kb), &mk) || mk.ordered != m0.ordered || mk.ch != m0.ch) { j = it->end; break; }
        if (k > it->end) loose = true;
        j = k;
    }
    if (count == 0) return i + 1;
    md_font f = md_body(c);
    float u = c->u;
    bool any_task = false;
    for (int k = 0; k < count; k++) any_task |= items[k].task;
    float colw = 18 * u;
    const char *last_num = NULL;
    if (m0.ordered) {
        last_num = cui__frame_printf(ui, "%d%c", m0.num + count - 1, m0.ch);
        colw = CUI_MAX(colw, cui__text_width(ui, f.reg, f.size, last_num, -1, true) + 9 * u);
    }
    if (any_task) colw = CUI_MAX(colw, 26.0f * CUI_MAX(1.0f, u));
    int list = cui__node_new(ui, 0);
    ui->nodes[list].w = CUI_FILL;
    ui->nodes[list].gap = (loose ? 10 : (c->compact ? 3 : 4)) * u;
    cui__open(ui, list);
    float best = 0;
    for (int k = 0; k < count; k++) {
        md_item *it = &items[k];
        int row = cui__node_new(ui, 0);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->align = CUI_ALIGN_START; rn->w = CUI_FILL;
        cui__open(ui, row);
        int mk = cui__leaf(ui, 0, CUI_PX(colw), CUI_PX(f.lh));
        if (it->task) {
            cui_node *mn = &ui->nodes[mk];
            mn->dir = CUI_ROW; mn->align = CUI_ALIGN_CENTER;
            cui__open(ui, mk);
            bool v = it->checked;
            if (cui_checkbox_ex(ui, cui__frame_printf(ui, "##task%d", it->task_off), &v, NULL)) {
                c->res->task_offset = it->task_off;
                c->res->task_checked = v;
                int line = 1;
                for (int q = 0; q < it->task_off && q < c->len; q++) if (c->src[q] == '\n') line++;
                c->res->task_line = line;
            }
            cui__close(ui);
        } else {
            md_mark_paint *mp = cui__frame_alloc(ui, sizeof *mp);
            mp->kind = m0.ordered ? 1 : 0;
            mp->depth = b->depth;
            mp->c = c->marker_c;
            mp->f = f;
            if (m0.ordered) mp->num = cui__frame_printf(ui, "%d%c", m0.num + k, m0.ch);
            ui->nodes[mk].paint = md_paint_marker;
            ui->nodes[mk].paint_data = mp;
        }
        /* the item's lines with the list prefix removed */
        int nl = it->end - it->start;
        md_line *sub = cui__frame_alloc(ui, sizeof(md_line) * (size_t)nl);
        sub[0] = it->first;
        for (int q = 1; q < nl; q++) {
            md_line Lq = L[it->start + q];
            int qi = md_indent(Lq, NULL);
            sub[q] = md_blank(Lq) ? md_skip(Lq, Lq.len) : md_strip_cols(Lq, CUI_MIN(qi, it->content_col));
        }
        int col = cui__node_new(ui, 0);
        ui->nodes[col].w = CUI_FILL;
        cui__open(ui, col);
        md_box cb = { b->w - colw, b->depth + 1, !loose, it->task && it->checked ? c->fg2 : b->fg };
        float w = md_blocks(c, sub, nl, &cb);
        cui__close(ui);
        cui__close(ui);
        best = CUI_MAX(best, w + colw);
    }
    cui__close(ui);
    if (nat) *nat = best;
    return j;
}

static int md_quote(md_ctx *c, const md_box *b, const md_line *L, int n, int i, float *nat) {
    cui_ctx *ui = c->ui;
    md_line *sub = cui__frame_alloc(ui, sizeof(md_line) * (size_t)(n - i));
    int count = 0, j = i;
    bool prev_text = false;
    while (j < n) {
        int bytes, ind = md_indent(L[j], &bytes);
        md_line T = md_skip(L[j], bytes);
        if (ind < 4 && T.len > 0 && T.s[0] == '>') {
            T = md_skip(T, 1);
            if (T.len > 0 && md_sp(T.s[0])) T = md_skip(T, 1);
            sub[count++] = T;
            prev_text = !md_blank(T);
        } else if (prev_text && !md_blank(L[j]) && !md_interrupts(L[j])) {
            sub[count++] = L[j];   /* lazy continuation of a quoted paragraph */
        } else break;
        j++;
    }
    float u = c->u;
    int q = cui__node_new(ui, 0);
    cui_node *qn = &ui->nodes[q];
    qn->w = CUI_FILL;
    qn->pad[3] = 14 * u;
    cui_color *rc = cui__frame_alloc(ui, sizeof *rc);
    *rc = c->rail_c;
    qn->paint = md_paint_rail;
    qn->paint_data = rc;
    cui__open(ui, q);
    md_box qb = { b->w - 14 * u, b->depth + 1, false, c->fg2 };
    float w = md_blocks(c, sub, count, &qb);
    cui__close(ui);
    if (nat) *nat = w + 14 * u;
    return j;
}

static int md_table(md_ctx *c, const md_box *b, const md_line *L, int n, int i, int ncols, const uint8_t *align, float *nat) {
    cui_ctx *ui = c->ui;
    if (ncols > MD_MAX_COLS) ncols = MD_MAX_COLS;
    bool has_delim = i + 1 < n && md_delim_row(L[i + 1], (uint8_t[MD_MAX_COLS]){ 0 }, MD_MAX_COLS, md_open_line(c, L[i + 1])) > 0;
    int j = i + (has_delim ? 2 : 1);
    while (j < n && !md_blank(L[j]) && md_has(L[j], '|')) j++;
    int nrows = j - i - (has_delim ? 2 : 1);
    md_font f = md_body(c);
    md_font hf = md_make_font(c, c->ui->theme.text_sm * c->body / c->ui->theme.text_body, CUI_FONT_MEDIUM, CUI_FONT_MEDIUM, 1.3f);
    md_inline *cells = cui__frame_alloc(ui, sizeof(md_inline) * (size_t)((nrows + 1) * ncols));
    int *rowc = cui__frame_alloc(ui, sizeof(int) * (size_t)(nrows + 1));
    float *colw = cui__frame_alloc(ui, sizeof(float) * (size_t)ncols * 2), *floor_w = colw + ncols;
    md_line raw[MD_MAX_COLS];
    for (int r = 0; r <= nrows; r++) {
        md_line R = L[r == 0 ? i : i + (has_delim ? 2 : 1) + r - 1];
        int nc = md_cells(R, raw, MD_MAX_COLS);
        rowc[r] = CUI_MIN(nc, ncols);
        bool tail = r == nrows && md_tail(c, R);
        for (int k = 0; k < rowc[r]; k++) {
            md_inline *in = &cells[r * ncols + k];
            *in = md_parse_inline(ui, raw[k].s, raw[k].len, tail && k == rowc[r] - 1);
            float w = md_runs_width(ui, in, r == 0 ? &hf : &f);
            if (r == 0) { floor_w[k] = w + 16; colw[k] = w + 16; }
            else colw[k] = CUI_MAX(colw[k], w + 16);
        }
    }
    float total = 18, u = c->u;
    for (int k = 0; k < ncols; k++) {
        floor_w[k] = CUI_MAX(floor_w[k], 48.0f * u);
        colw[k] = cui__clampf(colw[k], 48.0f * u, 360.0f * u);
        total += colw[k];
    }
    float natural = total;
    float avail = b->w;
    if (total < avail) {   /* fill the width, wider columns taking more */
        float extra = avail - total, sum = total - 18;
        for (int k = 0; k < ncols; k++) colw[k] += extra * colw[k] / sum;
    } else if (total > avail) {   /* narrow: long cells wrap first, then the table scrolls */
        for (int pass = 0; pass < 4 && total > avail + 0.5f; pass++) {
            float give = 0;
            for (int k = 0; k < ncols; k++) give += CUI_MAX(0.0f, colw[k] - CUI_MAX(floor_w[k], 120.0f * u));
            if (give <= 0) break;
            float need = CUI_MIN(give, total - avail);
            total = 18;
            for (int k = 0; k < ncols; k++) {
                float g = CUI_MAX(0.0f, colw[k] - CUI_MAX(floor_w[k], 120.0f * u));
                colw[k] -= need * g / give;
                total += colw[k];
            }
        }
    }
    cui_table_column *cols = cui__frame_alloc(ui, sizeof(cui_table_column) * (size_t)ncols);
    for (int k = 0; k < ncols; k++) {
        cols[k].title = k < rowc[0] ? md_plain(ui, &cells[k]) : "";
        cols[k].width = CUI_PX(colw[k]);
        cols[k].align = (cui_text_align)align[k];
    }
    cui_table_opts to;
    memset(&to, 0, sizeof to);
    to.compact = true;
    cui_table_begin_ex(ui, cui__frame_printf(ui, "table%d", c->table_seq++), cols, ncols, &to);
    for (int r = 1; r <= nrows; r++) {
        cui_table_row(ui, false);
        for (int k = 0; k < ncols; k++) {
            cui_table_cell(ui);
            if (k < rowc[r]) md_inline_emit(c, &cells[r * ncols + k], &f, b->fg, align[k], colw[k] - 16);
        }
    }
    cui_table_end(ui);
    if (nat) *nat = natural;
    return j;
}

static float md_blocks(md_ctx *c, const md_line *L, int n, const md_box *b) {
    cui_ctx *ui = c->ui;
    float best = 0;
    if (b->depth > MD_MAX_DEPTH) {   /* too deep: the rest is text */
        md_line *keep = cui__frame_alloc(ui, sizeof(md_line) * (size_t)(n + 1));
        int k = 0;
        for (int q = 0; q < n; q++) if (!md_blank(L[q])) keep[k++] = L[q];
        if (k) best = md_paragraph(c, b, keep, k);
        return best;
    }
    int i = 0, prev = BK_NONE;
    while (i < n) {
        md_line Li = L[i];
        if (md_blank(Li)) { i++; continue; }
        int bytes, ind = md_indent(Li, &bytes);
        md_line T = md_skip(Li, bytes), info, content;
        float w = 0;
        char fch;
        int flen, level;
        uint8_t align[MD_MAX_COLS];
        md_marker m;
        if (ind >= 4) {   /* indented code */
            int j = i, last = i;
            while (j < n && (md_blank(L[j]) || md_indent(L[j], NULL) >= 4)) { if (!md_blank(L[j])) last = j; j++; }
            md_space(c, md_gap_before(c, b, prev, BK_CODE, 0));
            md_line none = { "", 0, 0 };
            w = md_code(c, L + i, last - i + 1, 4, none);
            prev = BK_CODE;
            i = last + 1;
        } else if (T.len >= 2 && T.s[0] == '$' && T.s[1] == '$') {
            /* Display math: $$ on its own line, or $$expr$$ in one line. */
            md_line first = md_trim(T);
            int total = first.len + 2;
            for (int q = i + 1; q < n; q++) total += L[q].len + 2;
            char *buf = cui__frame_alloc(ui, (size_t)total + 1);
            int o = 0, j = i + 1;            /* j: first line after the opening $$ */
            bool closed = false;
            int cs = 2, ce = first.len;
            if (ce >= 4 && first.s[ce - 1] == '$' && first.s[ce - 2] == '$') {
                ce -= 2; closed = true; j = i + 1;
            }
            if (ce > cs) { memcpy(buf + o, first.s + cs, (size_t)(ce - cs)); o += ce - cs; }
            while (!closed && j < n) {
                md_line ML = md_trim(L[j]);
                int le = ML.len, ls = 0;
                bool end = le >= 2 && ML.s[0] == '$' && ML.s[1] == '$';
                if (end) { ls = 2; if (le >= 4 && ML.s[le - 1] == '$' && ML.s[le - 2] == '$') le -= 2; }
                if (o > 0) buf[o++] = '\n';
                if (le > ls) { memcpy(buf + o, ML.s + ls, (size_t)(le - ls)); o += le - ls; }
                j++;
                if (end) { closed = true; break; }
            }
            buf[o] = 0;
            w = md_math_block(c, b, buf, o, prev);
            prev = BK_MATH;
            i = j;
        } else if (md_fence(T, &fch, &flen, &info)) {
            int j = i + 1;
            while (j < n && !md_fence_close(L[j], fch, flen)) j++;
            if (mm_istart(info.s, info.len, "mermaid")) {
                w = md_mermaid(c, b, L + i + 1, j - i - 1, ind, prev);
                prev = BK_MERMAID;
            } else {
                md_space(c, md_gap_before(c, b, prev, BK_CODE, 0));
                w = md_code(c, L + i + 1, j - i - 1, ind, info);
                prev = BK_CODE;
            }
            i = j < n ? j + 1 : n;
        } else if (md_atx(T, &level, &content)) {
            md_space(c, md_gap_before(c, b, prev, BK_HEADING, level));
            w = md_heading(c, b, level, content.s, content.len, md_tail(c, Li));
            prev = BK_HEADING;
            i++;
        } else if (md_hrule(T)) {
            md_space(c, md_gap_before(c, b, prev, BK_HR, 0));
            int r = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
            ui->nodes[r].bg = c->rule_c;
            prev = BK_HR;
            i++;
        } else if (T.s[0] == '>') {
            md_space(c, md_gap_before(c, b, prev, BK_QUOTE, 0));
            i = md_quote(c, b, L, n, i, &w);
            prev = BK_QUOTE;
        } else if (md_list_marker(T, &m)) {
            md_space(c, md_gap_before(c, b, prev, BK_LIST, 0));
            i = md_list(c, b, L, n, i, &w);
            prev = BK_LIST;
        } else if ((level = md_table_at(c, L, n, i, align)) > 0) {
            md_space(c, md_gap_before(c, b, prev, BK_TABLE, 0));
            i = md_table(c, b, L, n, i, level, align, &w);
            prev = BK_TABLE;
        } else {   /* paragraph, or a setext heading */
            int j = i + 1, setext = 0;
            while (j < n && !md_blank(L[j])) {
                if (md_indent(L[j], NULL) < 4 && (setext = md_setext(L[j])) > 0) break;
                if (md_interrupts(L[j]) || md_table_at(c, L, n, j, align)) break;
                j++;
            }
            if (setext && md_open_line(c, L[j])) {   /* the underline may still be a list or a rule: wait for its line */
                md_space(c, md_gap_before(c, b, prev, BK_PARA, 0));
                w = md_paragraph(c, b, L + i, j - i);
                prev = BK_PARA;
                i = j + 1;
            } else if (setext) {
                int len;
                const char *s = md_join(c, L + i, j - i, &len);
                md_space(c, md_gap_before(c, b, prev, BK_HEADING, setext));
                w = md_heading(c, b, setext, s, len, false);
                prev = BK_HEADING;
                i = j + 1;
            } else {
                md_space(c, md_gap_before(c, b, prev, BK_PARA, 0));
                w = md_paragraph(c, b, L + i, j - i);
                prev = BK_PARA;
                i = j;
            }
        }
        best = CUI_MAX(best, w);
    }
    return best;
}

/* ------------------------------------------------------------------------ */
/* Public                                                                   */
/* ------------------------------------------------------------------------ */

cui_markdown_result cui_markdown_ex(cui_ctx *ui, const char *id_str, const char *text, const cui_markdown_opts *o) {
    cui_markdown_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_markdown_result res;
    memset(&res, 0, sizeof res);
    res.task_offset = -1;
    if (!text) text = "";
    if (!id_str) id_str = "markdown";
    int len = o->length > 0 ? o->length : (int)strlen(text);
    cui_id id = cui_make_id(ui, id_str);

    md_ctx c;
    memset(&c, 0, sizeof c);
    c.ui = ui; c.id = id; c.src = text; c.len = len; c.res = &res;
    c.streaming = o->streaming; c.compact = o->compact; c.fit = o->fit_content;
    c.body = o->size > 0 ? o->size : ui->theme.text_body;
    c.u = c.body / 13.0f;
    c.content_end = len;
    while (c.content_end > 0 && md_ws((unsigned char)text[c.content_end - 1])) c.content_end--;
    c.custom_color = cui__color_set(o->color);
    if (c.custom_color) {
        c.fg = cui_color_of(ui, o->color);
        c.fg2 = cui_alpha(c.fg, 0.78f);
        c.link_c = c.fg;
        c.code_bg = cui_alpha(c.fg, 0.14f);
        c.marker_c = cui_alpha(c.fg, 0.7f);
        c.rule_c = cui_alpha(c.fg, 0.22f);
        c.rail_c = cui_alpha(c.fg, 0.35f);
    } else {
        c.fg = CUI_C(ui, FOREGROUND);
        c.fg2 = CUI_C(ui, TEXT_SECONDARY);
        c.link_c = cui_mix(CUI_C(ui, ACCENT_STRONG), CUI_C(ui, FOREGROUND), ui->accent == CUI_ACCENT_NEUTRAL ? 1.0f : 0.0f);
        c.code_bg = CUI_C(ui, ACTIVE);
        c.marker_c = CUI_C(ui, TEXT_SECONDARY);
        c.rule_c = CUI_C(ui, BORDER);
        c.rail_c = cui_mix(CUI_C(ui, BORDER), CUI_C(ui, BORDER_STRONG), 0.45f);
    }

    /* wrap at the width of the latest layout (exact when the width is fixed) */
    float W;
    cui_rect pr = cui__prev_rect(ui, id);
    if (o->width.kind == CUI_SIZE_FIXED) W = o->width.value;
    else if (cui__has_prev_rect(ui, id) && pr.w > 0) W = pr.w;
    else { W = CUI_MIN(ui->vw, 640.0f); ui->animating = true; }
    res.height = pr.h;

    int root = cui__node_new(ui, id);
    cui_node *rn = &ui->nodes[root];
    if (c.fit) { rn->w = CUI_FIT; rn->self_align = CUI__ALIGN_NOSTRETCH; }
    else rn->w = o->width.kind != CUI_SIZE_FIT ? o->width : CUI_FILL;
    md_root_paint *rp = cui__frame_alloc(ui, sizeof *rp);
    rp->w = W;
    rn->paint = md_paint_root;
    rn->paint_data = rp;
    cui__open(ui, root);
    cui_push_id(ui, id_str);

    int nlines = 1;
    for (int k = 0; k < len; k++) if (text[k] == '\n') nlines++;
    md_line *lines = cui__frame_alloc(ui, sizeof(md_line) * (size_t)nlines);
    int n = 0, st = 0;
    for (int k = 0; k <= len; k++) {
        if (k < len && text[k] != '\n') continue;
        int e = k;
        if (e > st && text[e - 1] == '\r') e--;
        if (!(k == len && st == len && n > 0)) { lines[n].s = text + st; lines[n].len = e - st; lines[n].off = st; n++; }
        st = k + 1;
    }
    md_box box = { W, 0, false, c.fg };
    float natural = md_blocks(&c, lines, len > 0 ? n : 0, &box);

    cui_pop_id(ui);
    cui__close(ui);
    if (c.fit) ui->nodes[root].pref_hint = CUI_MAX(1.0f, CUI_MIN(natural, 1e6f));
    ui->last_id = id;
    return res;
}

bool cui_markdown_apply_task(char *text, const cui_markdown_result *r) {
    if (!text || !r || r->task_offset < 1) return false;
    int off = r->task_offset;
    for (int k = 0; k <= off; k++) if (!text[k]) return false;   /* past the end */
    if (text[off - 1] != '[' || text[off + 1] != ']') return false;
    if (text[off] != ' ' && text[off] != 'x' && text[off] != 'X') return false;
    text[off] = r->task_checked ? 'x' : ' ';
    return true;
}
