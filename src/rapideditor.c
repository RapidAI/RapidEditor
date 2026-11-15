/* RapidEditor - Notepad — 多文档 + 源码语法高亮
 *
 * A single-file, multi-tab notepad built on the CUI immediate-mode UI library:
 *   https://github.com/yappologistic/CUI
 *
 * Build (from this folder):
 *   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build
 *   ./build/notepad.exe [file1] [file2] ...
 *
 * Features: tabbed multi-document editing (new/switch/close/reorder), CUI's
 * code editor with per-document undo, caret, encoding, line endings and
 * syntax language, automatic language detection by extension / filename /
 * shebang, manual language override, line numbers, find/replace, go to line,
 * recent files, drag & drop (several files at once), insert date & time (F5),
 * light/dark theme, accents, font size, CRLF/LF, UTF-8 / UTF-8 BOM / ANSI,
 * status bar and an unsaved-change guard on every dirty document.
 */
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#endif

#define NP_TEXT_INIT  ((size_t)64 * 1024)              /* initial buffer per tab */
#define NP_TEXT_MAX   ((size_t)256 * 1024 * 1024)       /* hard ceiling per tab */
#define NP_LOAD_MAX   ((size_t)240 * 1024 * 1024)       /* largest file read in full */
#define NP_BIG_FILE   ((size_t)8 * 1024 * 1024)         /* show a size hint above this */
#define NP_PATH_CAP   1024
#define NP_DOC_MAX    16
#define NP_RECENT_MAX 6
#define NP_FONT_MIN   10
#define NP_FONT_MAX   28
#define NP_FONT_DEF   14
#define NP_DIR_MAX    400                              /* entries in the directory pane */
#define NP_LOG_MAX    120                              /* lines kept in the output log */
#define NP_HIT_MAX    300                              /* find results shown */

/* CUI stores an editor's caret/selection/undo history in its state table under
 * cui_make_id(id) ^ CUI__ED_SALT (see CUI/src/cui_editor.c). States untouched
 * for ~300 frames are collected, so a multi-tab app must touch every open
 * document's state each frame or a background tab loses its undo history. */
#define NP_ED_SALT    0xED17C0DEu

#define NP_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NP_MAX(a, b) ((a) > (b) ? (a) : (b))

enum { ENC_UTF8 = 0, ENC_UTF8_BOM, ENC_ANSI };
enum { ACT_NONE = 0, ACT_CLOSE_DOC, ACT_QUIT };

/* ------------------------------------------------------------------ */
/* Languages understood by CUI's highlighter                            */
/* ------------------------------------------------------------------ */

typedef struct np_lang {
    const char *id;                 /* the .lang value for cui_code_editor */
    const char *label;              /* shown in menus and the status bar */
    const char *exts;               /* ';' separated, lowercase */
} np_lang;

static const np_lang NP_LANGS[] = {
    { "text",   "纯文本",     "" },
    { "c",      "C",          "c;h;i" },
    { "cpp",    "C++",        "cpp;cc;cxx;c++;hpp;hh;hxx;ipp;tpp;inl" },
    { "js",     "JavaScript", "js;mjs;cjs;jsx" },
    { "ts",     "TypeScript", "ts;tsx;mts;cts" },
    { "python", "Python",     "py;pyw;pyi" },
    { "rust",   "Rust",       "rs" },
    { "go",     "Go",         "go" },
    { "json",   "JSON",       "json;jsonc;json5;geojson;webmanifest" },
    { "shell",  "Shell",      "sh;bash;zsh;ksh;fish;command" },
    { "sql",    "SQL",        "sql;ddl;dml;psql" }
};
#define NP_LANG_COUNT ((int)(sizeof NP_LANGS / sizeof NP_LANGS[0]))
#define NP_LANG_TEXT  0

static int np_lang_index(const char *id) {
    for (int i = 0; i < NP_LANG_COUNT; i++)
        if (strcmp(NP_LANGS[i].id, id) == 0) return i;
    return NP_LANG_TEXT;
}

static bool np_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static const char *np_base(const char *path) {
    const char *r = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') r = p + 1;
    return r;
}

static const char *np_ext(const char *path) {
    const char *base = np_base(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

static bool np_ext_in(const char *list, const char *ext) {
    size_t n = strlen(ext);
    if (!n) return false;
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && strncmp(p, ext, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* Extension first, then a few well known filenames, then a shebang. Anything
 * CUI cannot highlight falls back to plain text instead of a wrong grammar. */
static int np_detect_lang(const char *path, const char *text) {
    char ext[32];
    size_t i = 0;
    for (const char *p = np_ext(path); *p && i + 1 < sizeof ext; p++, i++)
        ext[i] = (char)tolower((unsigned char)*p);
    ext[i] = 0;
    if (ext[0]) {
        for (int k = 1; k < NP_LANG_COUNT; k++)
            if (np_ext_in(NP_LANGS[k].exts, ext)) return k;
    }

    const char *base = np_base(path);
    if (np_ieq(base, "Makefile") || np_ieq(base, "GNUmakefile") ||
        np_ieq(base, "makefile") || np_ieq(base, "Dockerfile") ||
        np_ieq(base, ".bashrc") || np_ieq(base, ".zshrc") || np_ieq(base, ".profile"))
        return np_lang_index("shell");
    if (!np_ieq(base, "CMakeLists.txt") && (
            np_ieq(ext, "cmake") || np_ieq(base, "CMakeCache.txt")))
        return np_lang_index("shell");

    if (text && text[0] == '#' && text[1] == '!') {
        char line[160];
        size_t n = 0;
        for (const char *p = text; *p && *p != '\n' && n + 1 < sizeof line; p++, n++)
            line[n] = (char)tolower((unsigned char)*p);
        line[n] = 0;
        if (strstr(line, "python")) return np_lang_index("python");
        if (strstr(line, "bash") || strstr(line, "/sh") || strstr(line, "zsh") ||
            strstr(line, "fish") || strstr(line, "ksh")) return np_lang_index("shell");
        if (strstr(line, "node")) return np_lang_index("js");
    }
    return NP_LANG_TEXT;
}

/* ------------------------------------------------------------------ */
/* Application state                                                   */
/* ------------------------------------------------------------------ */

typedef struct np_doc {
    char *text;                         /* dynamically sized, NUL terminated */
    size_t cap;                         /* allocated bytes for text */
    int len;                            /* current text length (CUI offsets are int) */
    uint64_t rev;                       /* CUI line-index revision: external edits only */
    uint64_t content_rev;               /* any text change: drives stats/match caches */
    char *tmp;                          /* scratch for replace, allocated on demand */
    size_t tmp_cap;
    size_t file_size;                   /* bytes on disk at load time */
    uint64_t stats_rev;                 /* cached word/char counts */
    uint64_t matches_key;               /* cached find-match count */
    int match_count;
    char path[NP_PATH_CAP];
    char editor_id[48];                 /* unique: CUI keeps undo/caret per id */
    char tab_title[NP_PATH_CAP + 64];   /* "name###key" for cui_doc_tabs */
    char tab_tip[NP_PATH_CAP + 128];
    unsigned uid;                       /* stable tab identity */
    bool dirty;
    bool crlf;
    bool lang_manual;                   /* user picked a language explicitly */
    int  lang;
    int  enc;
    /* last frame's editor state (used when switching back to this tab) */
    int caret, anchor, line, column, line_count;
    int chars, words, find_matches, find_index;
    bool editor_focused;
} np_doc;

typedef struct np_hit {
    int line;
    int offset;
    char text[160];
} np_hit;

typedef struct notepad {
    np_doc *docs[NP_DOC_MAX];
    int  count, active;
    unsigned next_uid;
    int  editor_generation;

    char title[NP_PATH_CAP + 128];
    char confirm_desc[NP_PATH_CAP + 128];
    char startup[NP_DOC_MAX][NP_PATH_CAP];
    int  startup_count;
    char recent[NP_RECENT_MAX][NP_PATH_CAP];
    int  recent_count;

    bool boot_done, fonts_ready, dark, line_numbers, show_status, default_crlf;
    bool quiet;                         /* no "opened" toasts while loading startup files */
    int  accent, font_size;
    int  pending_action, pending_doc;

    /* dialogs */
    bool dlg_confirm, dlg_replace, dlg_goto, dlg_help, dlg_about, dlg_lang;
    char find_query[256];
    char replace_with[256];
    char goto_buf[32];
    bool match_case;
    int  lang_pick;

    cui_ctx *ui;
    char toast_desc[256];

    /* EditPlus-style workspace ---------------------------------------- */
    bool show_sidebar, show_output, follow_active;
    int  last_active;               /* to make the directory pane follow tab switches */
    float side_ratio, out_ratio;    /* first pane's share of each split */
    int  side_tab, out_tab;         /* 目录/文档/最近 · 查找结果/消息 */
    char side_dir[NP_PATH_CAP];
    bool side_reload;
    cui_file_entry side_entries[NP_DIR_MAX];
    char side_names[NP_DIR_MAX * 96];
    int  side_count;

    char msgs[NP_LOG_MAX][256];
    int  msg_count;
    np_hit hits[NP_HIT_MAX];
    int  hit_count;
    uint64_t hits_rev, hits_key;
} notepad;

static notepad g;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *np_enc_name(int enc) {
    return enc == ENC_UTF8_BOM ? "UTF-8 BOM" : enc == ENC_ANSI ? "ANSI" : "UTF-8";
}

static void np_size_str(char *out, size_t cap, size_t n) {
    if (n >= (size_t)1024 * 1024)
        snprintf(out, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
    else if (n >= 1024)
        snprintf(out, cap, "%.1f KB", (double)n / 1024.0);
    else
        snprintf(out, cap, "%zu B", n);
}

/* A small rolling log for the output pane, like EditPlus's output window. */
static void np_logf(notepad *a, const char *fmt, ...) {
    if (a->msg_count == NP_LOG_MAX) {
        memmove(a->msgs[0], a->msgs[1], (size_t)(NP_LOG_MAX - 1) * sizeof a->msgs[0]);
        a->msg_count--;
    }
    char *line = a->msgs[a->msg_count++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof a->msgs[0], fmt, ap);
    va_end(ap);
}

static bool np_any_dialog(const notepad *a) {
    return a->dlg_confirm || a->dlg_replace || a->dlg_goto ||
           a->dlg_help || a->dlg_about || a->dlg_lang;
}

static np_doc *np_cur(notepad *a) {
    if (a->count == 0) return NULL;
    if (a->active < 0 || a->active >= a->count) a->active = a->count - 1;
    return a->docs[a->active];
}

static np_doc *np_doc_at(notepad *a, int idx) {
    return idx >= 0 && idx < a->count ? a->docs[idx] : NULL;
}

static void np_toast(notepad *a, const char *title, const char *desc, cui_tone tone) {
    if (!a->ui) return;
    snprintf(a->toast_desc, sizeof a->toast_desc, "%s", desc ? desc : "");
    cui_toast(a->ui, title, .description = a->toast_desc, .tone = tone);
}

static void np_update_title(notepad *a) {
    np_doc *d = np_cur(a);
    char title[NP_PATH_CAP + 128];
    snprintf(title, sizeof title, "%s%s - 记事本 (CUI)",
             d && d->dirty ? "*" : "",
             d && d->path[0] ? np_base(d->path) : "未命名");
    if (strcmp(title, a->title) != 0) {
        snprintf(a->title, sizeof a->title, "%s", title);
        cui_app_set_title(NULL, a->title);
    }
}

static void np_bump_editor(notepad *a, np_doc *d) {
    a->editor_generation++;
    snprintf(d->editor_id, sizeof d->editor_id,
             "npdoc#%u#%d", d->uid, a->editor_generation);
    d->caret = d->anchor = 0;
    d->line = d->column = 1;
    d->find_matches = d->find_index = 0;
}

/* CUI edits a plain NUL-terminated buffer, so the buffer capacity is the
 * only limit on editing. Grow geometrically but never past the ceiling;
 * a refused edit grows the buffer for the next frame. */
static bool np_ensure_cap(np_doc *d, size_t need) {
    if (need <= d->cap) return true;
    if (need > NP_TEXT_MAX) return false;
    size_t nc = d->cap;
    while (nc < need) {
        size_t step = nc / 4;
        if (step < 65536) step = 65536;
        if (nc > NP_TEXT_MAX - step) { nc = NP_TEXT_MAX; break; }
        nc += step;
    }
    if (nc > NP_TEXT_MAX) nc = NP_TEXT_MAX;
    if (nc < need) return false;
    char *p = (char *)realloc(d->text, nc);
    if (!p) return false;
    d->text = p;
    d->cap = nc;
    return true;
}

static bool np_ensure_tmp(np_doc *d, size_t need) {
    if (need <= d->tmp_cap) return true;
    size_t nc = d->tmp_cap ? d->tmp_cap : 65536;
    while (nc < need) nc += nc / 2 + 65536;
    char *p = (char *)realloc(d->tmp, nc);
    if (!p) return false;
    d->tmp = p;
    d->tmp_cap = nc;
    return true;
}

/* Call after any change made outside cui_code_editor (load, replace, insert).
 * The full length scan happens here only, never on every frame. */
static void np_text_changed(notepad *a, np_doc *d) {
    d->len = (int)strlen(d->text);
    d->rev++;                  /* CUI must rebuild its cached line index */
    d->content_rev++;
    d->dirty = true;
    np_update_title(a);
}

static np_doc *np_doc_new(notepad *a) {
    if (a->count >= NP_DOC_MAX) {
        np_toast(a, "标签页已达上限", "最多同时打开 16 个文档", CUI_TONE_WARNING);
        return NULL;
    }
    np_doc *d = (np_doc *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->text = (char *)malloc(NP_TEXT_INIT);
    if (!d->text) { free(d); return NULL; }
    d->cap = NP_TEXT_INIT;
    d->text[0] = 0;
    d->rev = 1;
    d->content_rev = 1;
    d->uid = ++a->next_uid;
    d->enc = ENC_UTF8;
    d->crlf = a->default_crlf;
    d->lang = NP_LANG_TEXT;
    d->line = d->column = 1;
    np_bump_editor(a, d);
    a->docs[a->count] = d;
    return a->docs[a->count++];
}

static void np_doc_free(np_doc *d) {
    if (!d) return;
    free(d->text); free(d->tmp); free(d);
}

static void np_close_doc(notepad *a, int idx) {
    if (idx < 0 || idx >= a->count) return;
    np_doc_free(a->docs[idx]);
    for (int i = idx; i < a->count - 1; i++) a->docs[i] = a->docs[i + 1];
    a->count--;
    if (a->count == 0) {
        np_doc_new(a);              /* always keep one editable tab */
        a->active = 0;
    } else if (a->active > idx) {
        a->active--;
    } else if (a->active >= a->count) {
        a->active = a->count - 1;
    }
    np_update_title(a);
}

/* focus_editor gives the keyboard back to the editor after a user-initiated
 * switch. It must stay false while opening files at startup: CUI's pending
 * select sets ui->focus, and a stale Enter key in the first frame would then
 * be typed into the freshly loaded document. */
static void np_activate(notepad *a, int idx, bool focus_editor) {
    if (idx < 0 || idx >= a->count) return;
    a->active = idx;
    np_doc *d = a->docs[idx];
    if (a->ui) {
        if (focus_editor)
            cui_code_editor_select(a->ui, d->editor_id, d->anchor, d->caret);
        cui_request_redraw(a->ui);
    }
    np_update_title(a);
}

static void np_move_doc(notepad *a, int from, int to) {
    if (from < 0 || from >= a->count || to < 0 || to >= a->count || from == to) return;
    np_doc *d = a->docs[from];
    if (from < to) {
        for (int i = from; i < to; i++) a->docs[i] = a->docs[i + 1];
    } else {
        for (int i = from; i > to; i--) a->docs[i] = a->docs[i - 1];
    }
    a->docs[to] = d;
}

static int np_first_dirty(notepad *a) {
    for (int i = 0; i < a->count; i++)
        if (a->docs[i]->dirty) return i;
    return -1;
}

static void np_add_recent(notepad *a, const char *path) {
    if (!path || !path[0]) return;
    char t[NP_RECENT_MAX][NP_PATH_CAP];
    int n = 0;
    snprintf(t[n++], NP_PATH_CAP, "%s", path);
    for (int i = 0; i < a->recent_count && n < NP_RECENT_MAX; i++)
        if (strcmp(a->recent[i], path) != 0)
            snprintf(t[n++], NP_PATH_CAP, "%s", a->recent[i]);
    memcpy(a->recent, t, sizeof t);
    a->recent_count = n;
}

static void np_stats(np_doc *d) {
    if (!d || d->stats_rev == d->content_rev) return;   /* 100 MB is not counted every frame */
    int chars = 0, words = 0;
    bool in_word = false;
    for (const unsigned char *p = (const unsigned char *)d->text; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;   /* UTF-8 continuation byte */
        chars++;
        bool space = (*p < 0x80) && isspace(*p);
        if (!space && !in_word) words++;
        in_word = !space;
    }
    d->chars = chars;
    d->words = words;
    d->stats_rev = d->content_rev;
}

/* Touch every tab's editor state so CUI's state GC cannot reclaim the undo
 * history / caret of a background document. Must be called in the same id
 * scope as cui_code_editor (cui_make_id hashes the current id stack). */
static void np_keep_editors(notepad *a, cui_ctx *ui) {
    for (int i = 0; i < a->count; i++)
        cui_state(ui, cui_make_id(ui, a->docs[i]->editor_id) ^ NP_ED_SALT, 0);
}

/* ------------------------------------------------------------------ */
/* Windows: UTF-8 paths and code-page conversions                      */
/* ------------------------------------------------------------------ */

static FILE *np_fopen_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wp[2048], wm[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048) <= 0) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16) <= 0) return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

#ifdef _WIN32
static unsigned char *np_ansi_to_utf8(const unsigned char *src, size_t n, size_t *out_n) {
    int wl = MultiByteToWideChar(CP_ACP, 0, (const char *)src, (int)n, NULL, 0);
    if (wl <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)(wl + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_ACP, 0, (const char *)src, (int)n, w, wl);
    int ul = WideCharToMultiByte(CP_UTF8, 0, w, wl, NULL, 0, NULL, NULL);
    unsigned char *out = (unsigned char *)malloc((size_t)ul + 1);
    if (out) {
        WideCharToMultiByte(CP_UTF8, 0, w, wl, (char *)out, ul, NULL, NULL);
        out[ul] = 0;
        *out_n = (size_t)ul;
    }
    free(w);
    return out;
}

static unsigned char *np_utf8_to_ansi(const char *src, size_t n, size_t *out_n) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, src, (int)n, NULL, 0);
    if (wl <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)(wl + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, src, (int)n, w, wl);
    int al = WideCharToMultiByte(CP_ACP, 0, w, wl, NULL, 0, NULL, NULL);
    unsigned char *out = (unsigned char *)malloc((size_t)al + 1);
    if (out) {
        WideCharToMultiByte(CP_ACP, 0, w, wl, (char *)out, al, NULL, NULL);
        out[al] = 0;
        *out_n = (size_t)al;
    }
    free(w);
    return out;
}
#endif

static bool np_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        int need;
        if      ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + (size_t)need >= n) return false;
        for (int k = 1; k <= need; k++)
            if ((s[i + (size_t)k] & 0xC0) != 0x80) return false;
        i += (size_t)need + 1;
    }
    return true;
}

/* UTF-16 (BOM, LE or BE) to UTF-8, including surrogate pairs. */
static size_t np_utf16_to_utf8(const unsigned char *p, size_t n, bool le,
                               unsigned char *out, size_t cap) {
    size_t i = 2, o = 0;
    while (i + 1 < n && o + 5 < cap) {
        unsigned u = le ? (unsigned)(p[i] | (p[i + 1] << 8))
                        : (unsigned)((p[i] << 8) | p[i + 1]);
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            unsigned v = le ? (unsigned)(p[i] | (p[i + 1] << 8))
                            : (unsigned)((p[i] << 8) | p[i + 1]);
            if (v >= 0xDC00 && v <= 0xDFFF) {
                u = 0x10000u + ((u - 0xD800u) << 10) + (v - 0xDC00u);
                i += 2;
            }
        }
        if (u < 0x80) {
            out[o++] = (unsigned char)u;
        } else if (u < 0x800) {
            out[o++] = (unsigned char)(0xC0 | (u >> 6));
            out[o++] = (unsigned char)(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            out[o++] = (unsigned char)(0xE0 | (u >> 12));
            out[o++] = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (unsigned char)(0x80 | (u & 0x3F));
        } else {
            out[o++] = (unsigned char)(0xF0 | (u >> 18));
            out[o++] = (unsigned char)(0x80 | ((u >> 12) & 0x3F));
            out[o++] = (unsigned char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (unsigned char)(0x80 | (u & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

/* CRLF/CR -> LF, NUL -> space (CUI owns NUL terminated buffers). */
static size_t np_normalize(const unsigned char *src, size_t n,
                           unsigned char *dst, size_t cap, bool *had_crlf) {
    size_t i = 0, o = 0;
    *had_crlf = false;
    while (i < n && o + 2 < cap) {
        if (src[i] == '\r') {
            if (i + 1 < n && src[i + 1] == '\n') { *had_crlf = true; i++; }
            dst[o++] = '\n';
            i++;
        } else if (src[i] == 0) {
            dst[o++] = ' ';
            i++;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ */
/* File IO                                                             */
/* ------------------------------------------------------------------ */

static bool np_load_into(notepad *a, np_doc *d, const char *path) {
    FILE *f = np_fopen_utf8(path, "rb");
    if (!f) {
        np_toast(a, "无法打开文件", path, CUI_TONE_DANGER);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }

    size_t to_read = (size_t)n;
    bool truncated = to_read > NP_LOAD_MAX;
    if (truncated) to_read = NP_LOAD_MAX;
    unsigned char *raw = (unsigned char *)malloc(to_read + 8);
    if (!raw) {
        fclose(f);
        np_toast(a, "内存不足", "无法读取该文件", CUI_TONE_DANGER);
        return false;
    }
    size_t got = fread(raw, 1, to_read, f);
    fclose(f);
    raw[got] = 0;
    d->file_size = (size_t)n;

    const unsigned char *src = raw;
    size_t len = got;
    unsigned char *owned = NULL;
    int enc = ENC_UTF8;

    if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        src += 3;
        len -= 3;
        enc = ENC_UTF8_BOM;
    } else if (len >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) ||
                           (raw[0] == 0xFE && raw[1] == 0xFF))) {
        /* UTF-16 -> UTF-8 needs at most 1.5x the bytes, not 3x. */
        owned = (unsigned char *)malloc(len * 2 + 8);
        if (owned) {
            len = np_utf16_to_utf8(raw, len, raw[0] == 0xFF, owned, len * 2 + 4);
            src = owned;
        }
        enc = ENC_UTF8_BOM;
    } else if (!np_utf8_valid(raw, len)) {
#ifdef _WIN32
        size_t ul = 0;
        owned = np_ansi_to_utf8(raw, len, &ul);
        if (owned) { src = owned; len = ul; enc = ENC_ANSI; }
#else
        /* No system code page here: replace invalid bytes so the text stays valid UTF-8. */
        for (size_t i = 0; i < len; i++)
            if (raw[i] >= 0x80) raw[i] = '?';
#endif
    }

    /* Text + headroom for editing, all in one buffer; normalizing writes
     * straight into it, so a 100 MB file never needs a 2x scratch copy. */
    size_t slack = len / 8;
    if (slack < (size_t)1024 * 1024) slack = (size_t)1024 * 1024;
    if (slack > (size_t)32 * 1024 * 1024) slack = (size_t)32 * 1024 * 1024;
    size_t want = len + slack + 2;
    if (want > NP_TEXT_MAX) want = NP_TEXT_MAX;
    if (!np_ensure_cap(d, want)) {
        np_toast(a, "内存不足", "无法为该文件分配文本缓冲区", CUI_TONE_DANGER);
        free(owned); free(raw);
        return false;
    }

    bool had_crlf = false;
    size_t o = np_normalize(src, len, (unsigned char *)d->text, d->cap, &had_crlf);
    free(owned);
    free(raw);

    snprintf(d->path, sizeof d->path, "%s", path);
    d->dirty = false;
    d->enc = enc;
    d->crlf = had_crlf;
    d->len = (int)o;
    d->rev++;
    d->content_rev++;
    d->lang = np_detect_lang(path, d->text);
    d->lang_manual = false;
    np_bump_editor(a, d);
    np_add_recent(a, path);
    np_update_title(a);
    {
        char sz[48];
        np_size_str(sz, sizeof sz, (size_t)n);
        np_logf(a, "打开  %s  ·  %s  ·  %s", np_base(path), sz, NP_LANGS[d->lang].label);
    }

    if (truncated) {
        np_toast(a, "文件过大，已截断", "单次最多载入 240 MB", CUI_TONE_WARNING);
    } else if (!a->quiet) {
        if ((size_t)n >= NP_BIG_FILE) {
            char msg[160];
            snprintf(msg, sizeof msg, "%.1f MB · %s语法", (double)n / (1024.0 * 1024.0),
                     NP_LANGS[d->lang].label);
            np_toast(a, "已打开大文件", msg, CUI_TONE_SUCCESS);
        } else {
            np_toast(a, "已打开", path, CUI_TONE_SUCCESS);
        }
    }
    return true;
}

static bool np_save(notepad *a, np_doc *d, const char *path) {
    size_t n = (size_t)d->len;
    FILE *f = np_fopen_utf8(path, "wb");
    if (!f) {
        np_toast(a, "无法保存文件", path, CUI_TONE_DANGER);
        return false;
    }
    if (d->enc == ENC_UTF8_BOM) fwrite("\xEF\xBB\xBF", 1, 3, f);
    bool ok = true;

    if (d->enc == ENC_ANSI) {
        /* The ANSI code-page conversion needs one contiguous staged buffer. */
        size_t nl = 0;
        if (d->crlf)
            for (size_t i = 0; i < n; i++) if (d->text[i] == '\n') nl++;
        unsigned char *staged = (unsigned char *)malloc(n + nl + 1);
        unsigned char *converted = NULL;
        if (!staged) {
            ok = false;
        } else {
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (d->text[i] == '\n' && d->crlf) staged[m++] = '\r';
                staged[m++] = (unsigned char)d->text[i];
            }
            const unsigned char *out = staged;
            size_t out_n = m;
#ifdef _WIN32
            size_t an = 0;
            converted = np_utf8_to_ansi((const char *)staged, m, &an);
            if (converted) { out = converted; out_n = an; }
#endif
            if (out_n && fwrite(out, 1, out_n, f) != out_n) ok = false;
            free(converted);
            free(staged);
        }
    } else {
        /* UTF-8: stream through a small stack buffer, no full-size copy. */
        char chunk[64 * 1024];
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            if (m + 2 >= sizeof chunk) {
                if (fwrite(chunk, 1, m, f) != m) { ok = false; break; }
                m = 0;
            }
            if (d->text[i] == '\n' && d->crlf) chunk[m++] = '\r';
            chunk[m++] = d->text[i];
        }
        if (ok && m && fwrite(chunk, 1, m, f) != m) ok = false;
    }
    fclose(f);
    if (!ok) {
        np_toast(a, "保存失败", "写入文件时出错", CUI_TONE_DANGER);
        return false;
    }

    bool renamed = strcmp(d->path, path) != 0;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->dirty = false;
    d->file_size = n;
    if (renamed && !d->lang_manual) d->lang = np_detect_lang(path, d->text);
    np_add_recent(a, path);
    np_update_title(a);
    {
        char sz[48];
        np_size_str(sz, sizeof sz, n);
        np_logf(a, "保存  %s  ·  %s  ·  %s / %s", np_base(path), sz,
                np_enc_name(d->enc), d->crlf ? "CRLF" : "LF");
    }
    np_toast(a, "已保存", path, CUI_TONE_SUCCESS);
    return true;
}

static const char *NP_FILE_FILTER =
    "txt;log;md;c;h;i;cpp;cc;cxx;hpp;hh;hxx;js;mjs;cjs;jsx;ts;tsx;"
    "py;pyw;rs;go;json;jsonc;sh;bash;zsh;bat;cmd;ps1;sql;csv;ini;xml;html;css;"
    "yml;yaml;toml;java;cs;kt";

static bool np_save_as(notepad *a, np_doc *d) {
    char path[NP_PATH_CAP];
    char name[256];
    snprintf(name, sizeof name, "%s", d->path[0] ? np_base(d->path) : "未命名.txt");
    bool ok = cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
        .kind = CUI_FILE_SAVE,
        .title = "另存为",
        .filter_name = "文本文件",
        .filter = NP_FILE_FILTER,
        .default_name = name
    }, path, sizeof path);
    return ok && np_save(a, d, path);
}

static bool np_save_current(notepad *a, np_doc *d) {
    return d->path[0] ? np_save(a, d, d->path) : np_save_as(a, d);
}

static void np_save_all(notepad *a) {
    int saved = 0, skipped = 0;
    for (int i = 0; i < a->count; i++) {
        np_doc *d = a->docs[i];
        if (!d->dirty) continue;
        if (!d->path[0]) { skipped++; continue; }   /* needs a "save as" */
        if (np_save(a, d, d->path)) saved++;
    }
    char msg[128];
    snprintf(msg, sizeof msg, "已保存 %d 个文档%s", saved,
             skipped ? "，未命名文档请用「另存为」" : "");
    np_toast(a, "全部保存", msg, saved ? CUI_TONE_SUCCESS : CUI_TONE_WARNING);
}

static bool np_blank_untitled(const np_doc *d) {
    return !d->path[0] && !d->dirty && !d->text[0];
}

/* Open a path in a new tab (or switch to the tab that already has it). */
static bool np_open_path(notepad *a, const char *path, bool focus_editor) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < a->count; i++) {
        if (a->docs[i]->path[0] && strcmp(a->docs[i]->path, path) == 0) {
            np_activate(a, i, focus_editor);
            np_toast(a, "已切换到已打开的文档", path, CUI_TONE_INFO);
            return true;
        }
    }

    np_doc *d = NULL;
    bool fresh = false;
    int idx = -1;
    if (a->count == 1 && np_blank_untitled(a->docs[0])) {
        d = a->docs[0];
        idx = 0;                        /* replace the initial empty tab */
    } else {
        d = np_doc_new(a);
        if (!d) return false;
        fresh = true;
        idx = a->count - 1;
    }

    if (!np_load_into(a, d, path)) {
        if (fresh) np_close_doc(a, idx);
        return false;
    }
    np_activate(a, idx, focus_editor);
    return true;
}

static void np_open_dialog(notepad *a) {
    char p[NP_PATH_CAP];
    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
            .kind = CUI_FILE_OPEN, .title = "打开文件",
            .filter_name = "文本文件", .filter = NP_FILE_FILTER },
            p, sizeof p))
        np_open_path(a, p, true);
}

/* ------------------------------------------------------------------ */
/* Unsaved-change guard                                                */
/* ------------------------------------------------------------------ */

static void np_confirm(notepad *a, int action, int idx) {
    np_doc *d = np_doc_at(a, idx);
    const char *name = d && d->path[0] ? np_base(d->path) : "未命名";
    a->pending_action = action;
    a->pending_doc = idx;
    snprintf(a->confirm_desc, sizeof a->confirm_desc,
             "「%s」有未保存的修改。", name);
    a->dlg_confirm = true;
}

static void np_request_close(notepad *a, int idx) {
    np_doc *d = np_doc_at(a, idx);
    if (!d) return;
    if (d->dirty) np_confirm(a, ACT_CLOSE_DOC, idx);
    else np_close_doc(a, idx);
}

static void np_request_quit(notepad *a) {
    int idx = np_first_dirty(a);
    if (idx >= 0) np_confirm(a, ACT_QUIT, idx);   /* one document at a time */
    else cui_app_quit(NULL);
}

static void np_open_lang_dialog(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d) return;
    a->lang_pick = d->lang;
    a->dlg_lang = true;
}

/* ------------------------------------------------------------------ */
/* Editing helpers                                                     */
/* ------------------------------------------------------------------ */

static bool np_match(const char *p, const char *q, size_t n, bool match_case) {
    for (size_t i = 0; i < n; i++) {
        if (!p[i]) return false;
        if (match_case) {
            if (p[i] != q[i]) return false;
        } else {
            unsigned char a = (unsigned char)p[i], b = (unsigned char)q[i];
            if (a < 0x80 && b < 0x80) {
                if (tolower(a) != tolower(b)) return false;
            } else if (a != b) return false;
        }
    }
    return true;
}

static uint64_t np_str_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* The replace dialog asks for the match count every frame; cache it so a
 * 100 MB buffer is scanned only when the text or the query changes. */
static int np_count_matches(notepad *a) {
    np_doc *d = np_cur(a);
    size_t qlen = strlen(a->find_query);
    if (!d || !qlen) return 0;
    uint64_t key = d->content_rev * 1315423911ULL + np_str_hash(a->find_query)
                   + (uint64_t)a->match_case;
    if (d->matches_key == key) return d->match_count;
    size_t n = (size_t)d->len;
    int count = 0;
    for (size_t i = 0; i + qlen <= n;) {
        if (np_match(d->text + i, a->find_query, qlen, a->match_case)) {
            count++;
            i += qlen;
        } else {
            i++;
        }
    }
    d->matches_key = key;
    d->match_count = count;
    return count;
}

static int np_replace_all(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d) return 0;
    size_t qlen = strlen(a->find_query);
    if (!qlen) return 0;
    size_t rlen = strlen(a->replace_with);
    size_t n = (size_t)d->len;
    int hits = np_count_matches(a);
    size_t upper = n + 2;
    if (rlen > qlen) upper += (size_t)hits * (rlen - qlen);
    if (!np_ensure_tmp(d, upper) || !np_ensure_cap(d, upper)) {
        np_toast(a, "文本过大", "无法分配替换所需的缓冲区", CUI_TONE_WARNING);
        return 0;
    }
    size_t o = 0;
    int count = 0;
    for (size_t i = 0; i < n;) {
        if (i + qlen <= n && np_match(d->text + i, a->find_query, qlen, a->match_case)) {
            memcpy(d->tmp + o, a->replace_with, rlen);
            o += rlen;
            i += qlen;
            count++;
        } else {
            d->tmp[o++] = d->text[i++];
        }
    }
    d->tmp[o] = 0;
    if (count) {
        memcpy(d->text, d->tmp, o + 1);
        np_text_changed(a, d);
        int caret = NP_MIN(d->caret, (int)o);
        cui_code_editor_select(a->ui, d->editor_id, caret, caret);
    }
    return count;
}

static void np_insert_at_caret(notepad *a, const char *ins) {
    np_doc *d = np_cur(a);
    if (!d) return;
    int len = d->len;
    int s0 = NP_MIN(d->caret, d->anchor), s1 = NP_MAX(d->caret, d->anchor);
    s0 = NP_MAX(0, NP_MIN(s0, len));
    s1 = NP_MAX(s0, NP_MIN(s1, len));
    size_t ilen = strlen(ins);
    if (!np_ensure_cap(d, (size_t)(len - (s1 - s0)) + ilen + 1)) {
        np_toast(a, "文本已满", "已达到 256 MB 上限", CUI_TONE_WARNING);
        return;
    }
    memmove(d->text + s0 + ilen, d->text + s1, (size_t)(len - s1) + 1);
    memcpy(d->text + s0, ins, ilen);
    np_text_changed(a, d);
    cui_code_editor_select(a->ui, d->editor_id, s0 + (int)ilen, s0 + (int)ilen);
}

static void np_open_find(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d) return;
    cui_code_editor_find(a->ui, d->editor_id,
                         a->find_query[0] ? a->find_query : "");
}

static void np_do_goto(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d) return;
    int line = atoi(a->goto_buf), col = 1;
    const char *colon = strchr(a->goto_buf, ':');
    if (colon) col = atoi(colon + 1);
    if (line < 1) line = 1;
    if (col < 1) col = 1;
    cui_code_editor_goto(a->ui, d->editor_id, line, col);
    a->dlg_goto = false;
}

/* ------------------------------------------------------------------ */
/* Fonts: CUI's embedded fonts cover Latin, so add a CJK fallback       */
/* ------------------------------------------------------------------ */

static void np_load_fonts(cui_ctx *ui, notepad *a) {
    a->fonts_ready = true;
    static const char *paths[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cui_font f = cui_font_load_file(ui, paths[i]);
        if (f < 0) continue;
        for (int role = CUI_FONT_BODY; role <= CUI_FONT_MONO; role++)
            cui_font_add_fallback(ui, (cui_font)role, f);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Win32: ask before the window closes with unsaved work                */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static WNDPROC g_orig_wndproc = NULL;

static LRESULT CALLBACK np_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE && np_first_dirty(&g) >= 0 && !np_any_dialog(&g)) {
        np_request_quit(&g);
        if (g.ui) cui_request_redraw(g.ui);
        InvalidateRect(hwnd, NULL, FALSE);
        PostMessageW(hwnd, WM_APP + 42, 0, 0);   /* wake the event loop */
        return 0;
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

static void np_install_close_hook(void) {
    if (g_orig_wndproc) return;
    HWND hwnd = (HWND)cui_app_native_window(NULL);
    if (!hwnd) return;
    LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)np_wndproc);
    if (prev) g_orig_wndproc = (WNDPROC)prev;
}
#endif

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
/* ------------------------------------------------------------------ */

static void np_dialogs(notepad *a, cui_ctx *ui) {
    cui_dialog(ui, "有未保存的修改", &a->dlg_confirm,
               .description = a->confirm_desc, .width = 420) {
        cui_text(ui, "要先保存这份文档吗？", .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) {
                a->dlg_confirm = false;
                a->pending_action = ACT_NONE;
                a->pending_doc = -1;
            }
            if (cui_button(ui, "不保存", .variant = CUI_BUTTON_GHOST)) {
                int act = a->pending_action, idx = a->pending_doc;
                np_doc *d = np_doc_at(a, idx);
                a->dlg_confirm = false;
                a->pending_action = ACT_NONE;
                a->pending_doc = -1;
                if (act == ACT_CLOSE_DOC) {
                    np_close_doc(a, idx);
                } else {
                    if (d) d->dirty = false;   /* discarded, do not ask again */
                    np_request_quit(a);
                }
            }
            if (cui_button(ui, "保存", .variant = CUI_BUTTON_PRIMARY)) {
                int act = a->pending_action, idx = a->pending_doc;
                np_doc *d = np_doc_at(a, idx);
                if (d && np_save_current(a, d)) {
                    a->dlg_confirm = false;
                    a->pending_action = ACT_NONE;
                    a->pending_doc = -1;
                    if (act == ACT_CLOSE_DOC) np_close_doc(a, idx);
                    else np_request_quit(a);
                }
                /* save failed (or "save as" cancelled): keep the dialog open */
            }
        }
    }

    cui_dialog(ui, "查找和替换", &a->dlg_replace,
               .description = "在当前文档中替换文字", .width = 440) {
        cui_column(ui, .width = CUI_FILL, .gap = 10) {
            cui_input(ui, "replace-query", a->find_query, sizeof a->find_query,
                      .label = "查找", .placeholder = "要查找的内容",
                      .width = CUI_FILL, .autofocus = true);
            cui_input(ui, "replace-with", a->replace_with, sizeof a->replace_with,
                      .label = "替换为", .placeholder = "留空表示删除",
                      .width = CUI_FILL);
            cui_checkbox(ui, "区分大小写", &a->match_case);
            char info[96];
            snprintf(info, sizeof info, "共找到 %d 处匹配", np_count_matches(a));
            cui_text(ui, info, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "关闭")) a->dlg_replace = false;
            if (cui_button(ui, "查找", .variant = CUI_BUTTON_GHOST)) {
                a->dlg_replace = false;
                np_open_find(a);
            }
            if (cui_button(ui, "全部替换", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->find_query[0])) {
                int n = np_replace_all(a);
                char msg[96];
                snprintf(msg, sizeof msg, "共替换 %d 处", n);
                np_logf(a, "替换  “%s” → “%s”  ·  %d 处",
                        a->find_query, a->replace_with, n);
                np_toast(a, n ? "替换完成" : "没有找到匹配", msg,
                         n ? CUI_TONE_SUCCESS : CUI_TONE_WARNING);
            }
        }
    }

    cui_dialog(ui, "转到行", &a->dlg_goto,
               .description = "输入行号，或 行:列", .width = 360) {
        int flags = cui_input(ui, "goto-line", a->goto_buf, sizeof a->goto_buf,
                              .label = "行号", .placeholder = "例如 42 或 42:7",
                              .width = CUI_FILL, .autofocus = true);
        if (flags & CUI_SUBMITTED) np_do_goto(a);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_goto = false;
            if (cui_button(ui, "跳转", .variant = CUI_BUTTON_PRIMARY)) np_do_goto(a);
        }
    }

    cui_dialog(ui, "语法高亮", &a->dlg_lang,
               .description = "为当前文档选择语言，或按扩展名自动识别", .width = 400) {
        const char *items[NP_LANG_COUNT];
        for (int i = 0; i < NP_LANG_COUNT; i++) items[i] = NP_LANGS[i].label;
        cui_select(ui, "lang-pick", items, NP_LANG_COUNT, &a->lang_pick,
                   .label = "语言", .placeholder = "选择语言", .width = CUI_FILL);
        cui_text(ui, "CUI 内置语法：C / C++ / JavaScript / TypeScript / Python /",
                 .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        cui_text(ui, "Rust / Go / JSON / Shell / SQL；其余文件按纯文本显示。",
                 .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "自动识别", .variant = CUI_BUTTON_GHOST)) {
                np_doc *d = np_cur(a);
                if (d) { d->lang = np_detect_lang(d->path, d->text); d->lang_manual = false; }
                a->dlg_lang = false;
            }
            if (cui_button(ui, "取消")) a->dlg_lang = false;
            if (cui_button(ui, "应用", .variant = CUI_BUTTON_PRIMARY)) {
                np_doc *d = np_cur(a);
                if (d) {
                    d->lang = NP_MAX(0, NP_MIN(NP_LANG_COUNT - 1, a->lang_pick));
                    d->lang_manual = true;
                }
                a->dlg_lang = false;
            }
        }
    }

    cui_dialog(ui, "快捷键", &a->dlg_help, .width = 480) {
        static const char *lines[] = {
            "Ctrl N          新建标签",
            "Ctrl O          打开文件（新标签）",
            "Ctrl W          关闭当前标签",
            "Ctrl Tab        下一个标签   / Ctrl Shift Tab 上一个",
            "Ctrl PageDn/Up  下一个 / 上一个标签",
            "Ctrl 1 .. 9     跳到第 N 个标签",
            "Ctrl S          保存     / Ctrl Shift S 另存为…",
            "Ctrl Q          退出",
            "Ctrl F          查找（编辑器内置查找栏）",
            "Ctrl H          查找和替换",
            "Ctrl G          转到行",
            "F5              插入日期和时间",
            "Ctrl Z / Y      撤销 / 重做",
            "Ctrl +/-        放大 / 缩小编辑器字号，Ctrl 0 重置",
            "Alt             激活菜单栏，方向键选择菜单项",
            "编辑器内：Ctrl+A 全选、Ctrl+C/X/V 复制/剪切/粘贴、Tab 缩进"
        };
        cui_column(ui, .width = CUI_FILL, .gap = 5) {
            for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
                cui_text(ui, lines[i], .size = 12, .font = CUI_FONT_MONO);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "知道了", .variant = CUI_BUTTON_PRIMARY)) a->dlg_help = false;
        }
    }

    cui_dialog(ui, "关于 记事本", &a->dlg_about,
               .description = "用 CUI 写的多文档记事本", .width = 440) {
        cui_column(ui, .width = CUI_FILL, .gap = 6) {
            cui_text(ui, "CUI Notepad 2.0", .size = 16, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "界面：yappologistic/CUI（C99, MIT, 自绘组件）", .size = 12);
            cui_text(ui, "多标签编辑：cui_doc_tabs + 每个文档独立的 cui_code_editor 状态；", .size = 12);
            cui_text(ui, "打开文件时按扩展名 / 文件名 / shebang 自动选择语法，可手动覆盖。", .size = 12);
            cui_text(ui, "支持 UTF-8 / UTF-8 BOM / ANSI(GBK) 编码、CRLF/LF 换行、", .size = 12);
            cui_text(ui, "中文路径、拖放多个文件、命令行同时打开多个文件。", .size = 12);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "关闭", .variant = CUI_BUTTON_PRIMARY)) a->dlg_about = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* EditPlus-style workspace: directory pane, output pane, editor pane    */
/* ------------------------------------------------------------------ */

/* The folder holding a file path (UTF-8, both separators accepted). */
static void np_dir_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    char *slash = NULL;
    for (char *p = out; *p; p++)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) { snprintf(out, cap, "."); return; }
    if (slash == out) out[1] = 0;
    else if (slash == out + 2 && out[1] == ':') out[3] = 0;
    else *slash = 0;
}

static void np_dir_parent(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path);
    size_t n = strlen(out);
    while (n > 1 && (out[n - 1] == '\\' || out[n - 1] == '/') &&
           !(n == 3 && out[1] == ':'))
        out[--n] = 0;
    char *slash = NULL;
    for (char *p = out; *p; p++)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return;
    if (slash == out) out[1] = 0;
    else if (slash == out + 2 && out[1] == ':') out[3] = 0;
    else *slash = 0;
}

static void np_dir_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '\\' || dir[n - 1] == '/');
#ifdef _WIN32
    snprintf(out, cap, "%s%s%s", dir, sep ? "" : "\\", name);
#else
    snprintf(out, cap, "%s%s%s", dir, sep ? "" : "/", name);
#endif
}

static void np_init_side_dir(notepad *a) {
#ifdef _WIN32
    wchar_t w[1024];
    DWORD n = GetCurrentDirectoryW(1024, w);
    if (n > 0 && n < 1024)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, a->side_dir,
                            sizeof a->side_dir, NULL, NULL);
    else
        snprintf(a->side_dir, sizeof a->side_dir, ".");
#else
    if (!getcwd(a->side_dir, sizeof a->side_dir))
        snprintf(a->side_dir, sizeof a->side_dir, ".");
#endif
    a->side_reload = true;
}

static void np_dir_set(notepad *a, const char *dir) {
    if (!dir || !*dir) return;
    snprintf(a->side_dir, sizeof a->side_dir, "%s", dir);
    a->side_reload = true;
}

/* Folders first, then a case-insensitive name sort. */
static int np_dir_cmp(const void *x, const void *y) {
    const cui_file_entry *p = (const cui_file_entry *)x;
    const cui_file_entry *q = (const cui_file_entry *)y;
    if (p->dir != q->dir) return p->dir ? -1 : 1;
    const unsigned char *s = (const unsigned char *)p->name;
    const unsigned char *t = (const unsigned char *)q->name;
    for (; *s && *t; s++, t++) {
        int aa = tolower(*s), bb = tolower(*t);
        if (aa != bb) return aa < bb ? -1 : 1;
    }
    return *s == *t ? 0 : (*s ? 1 : -1);
}

/* cui_fs_list only runs on navigation or refresh, never once per frame. */
static void np_dir_refresh(notepad *a) {
    int n = cui_fs_list(NULL, a->side_dir, a->side_entries, NP_DIR_MAX,
                        a->side_names, sizeof a->side_names, false);
    a->side_count = n > 0 ? n : 0;
    if (n < 0) np_logf(a, "无法读取目录  %s", a->side_dir);
    if (a->side_count > 1)
        qsort(a->side_entries, (size_t)a->side_count,
              sizeof a->side_entries[0], np_dir_cmp);
    a->side_reload = false;
}

static void np_sidebar(cui_ctx *ui, notepad *a) {
    if (a->side_reload) np_dir_refresh(a);
    np_doc *cur = np_cur(a);
    static const char *tab_names[3] = { "目录", "文档", "最近" };

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_SURFACE_MUTED, .gap = 0, .pad = 0) {
        /* A compact caption bar, like the title strip of EditPlus's directory pane. */
        cui_row(ui, .width = CUI_FILL, .pad = 8, .gap = 2) {
            cui_text(ui, "工作区", .font = CUI_FONT_MEDIUM, .size = 12, .nowrap = true);
            cui_spacer(ui);
            if (cui_icon_button(ui, CUI_ICON_BOOKMARK, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST,
                                .selected = a->follow_active,
                                .tooltip = "目录跟随当前文件"))
                a->follow_active = !a->follow_active;
            if (cui_icon_button(ui, CUI_ICON_ARROW_UP, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST, .tooltip = "上级目录")) {
                char up[NP_PATH_CAP];
                np_dir_parent(a->side_dir, up, sizeof up);
                np_dir_set(a, up);
            }
            if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST, .tooltip = "刷新目录"))
                a->side_reload = true;
            if (cui_icon_button(ui, CUI_ICON_FOLDER_OPEN, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST, .tooltip = "选择目录…")) {
                char dir[NP_PATH_CAP];
                if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                        .kind = CUI_FILE_FOLDER, .title = "选择目录" },
                        dir, sizeof dir))
                    np_dir_set(a, dir);
            }
        }

        cui_tabs(ui, "side_tabs", tab_names, 3, &a->side_tab, .full_width = true);

        if (a->side_tab == 0) {
            /* cui_text is plain text: it does not strip an "###id" suffix. */
            cui_row(ui, .width = CUI_FILL, .pad = 8, .gap = 0) {
                cui_text(ui, a->side_dir, .size = 11, .nowrap = true,
                         .color = CUI_COLOR_TEXT_MUTED);
            }
        }

        cui_scroll(ui, .id = "side_list", .width = CUI_FILL, .height = CUI_FILL,
                   .pad = 6, .gap = 1) {
            if (a->side_tab == 0) {
                if (cui_list_item(ui, "..  上级目录###up", .icon = CUI_ICON_ARROW_UP)) {
                    char up[NP_PATH_CAP];
                    np_dir_parent(a->side_dir, up, sizeof up);
                    np_dir_set(a, up);
                }
                for (int i = 0; i < a->side_count; i++) {
                    cui_file_entry *e = &a->side_entries[i];
                    char full[NP_PATH_CAP], label[320], sz[48];
                    np_dir_join(full, sizeof full, a->side_dir, e->name);
                    snprintf(label, sizeof label, "%s###dir%d", e->name, i);
                    np_size_str(sz, sizeof sz, (size_t)e->size);
                    bool here = cur && cur->path[0] && strcmp(full, cur->path) == 0;
                    if (cui_list_item(ui, label,
                                      .icon = e->dir ? CUI_ICON_FOLDER : CUI_ICON_FILE_TEXT,
                                      .trailing = e->dir ? NULL : sz,
                                      .selected = here)) {
                        if (e->dir) np_dir_set(a, full);
                        else np_open_path(a, full, true);
                    }
                }
                if (!a->side_count)
                    cui_text(ui, "目录为空或无法读取", .size = 12,
                             .color = CUI_COLOR_TEXT_MUTED);
            } else if (a->side_tab == 1) {
                for (int i = 0; i < a->count; i++) {
                    np_doc *t = a->docs[i];
                    char label[NP_PATH_CAP + 64], tail[64];
                    snprintf(label, sizeof label, "%s%s###doc%u",
                             t->dirty ? "● " : "",
                             t->path[0] ? np_base(t->path) : "未命名", t->uid);
                    snprintf(tail, sizeof tail, "%s", NP_LANGS[t->lang].label);
                    if (cui_list_item(ui, label, .icon = CUI_ICON_FILE_TEXT,
                                      .trailing = tail, .selected = (i == a->active)))
                        np_activate(a, i, true);
                }
            } else {
                for (int i = 0; i < a->recent_count; i++) {
                    char label[NP_PATH_CAP + 64];
                    snprintf(label, sizeof label, "%s###recent%d",
                             np_base(a->recent[i]), i);
                    if (cui_list_item(ui, label, .icon = CUI_ICON_HISTORY))
                        np_open_path(a, a->recent[i], true);
                }
                if (!a->recent_count)
                    cui_text(ui, "暂无最近文件", .size = 12,
                             .color = CUI_COLOR_TEXT_MUTED);
            }
        }
    }
}

/* Find results for the output window. Cached by content revision + query,
 * so a 100 MB document is scanned at most once per edit, and only while the
 * results tab is actually showing. Line starts are tracked incrementally so
 * even a single enormous line stays O(n) instead of O(n * hits). */
static void np_build_hits(notepad *a, np_doc *d) {
    if (!d) return;
    uint64_t key = np_str_hash(a->find_query) ^ np_str_hash(d->editor_id) ^
                   (a->match_case ? 0x9e3779b9ULL : 0x85ebca6bULL);
    if (a->hits_rev == d->content_rev && a->hits_key == key) return;
    a->hits_rev = d->content_rev;
    a->hits_key = key;
    a->hit_count = 0;

    size_t qlen = strlen(a->find_query);
    if (!qlen) return;
    const unsigned char *t = (const unsigned char *)d->text;
    int n = d->len;
    unsigned char f0 = (unsigned char)a->find_query[0];
    bool first_ok[256];
    for (int k = 0; k < 256; k++)
        first_ok[k] = (f0 < 0x80 && !a->match_case && k < 0x80)
                        ? ((unsigned char)tolower(k) == (unsigned char)tolower(f0))
                        : (k == f0);

    int line = 1, line_start = 0;
    for (int i = 0; i + (int)qlen <= n; i++) {
        if (first_ok[t[i]] &&
            np_match(d->text + i, a->find_query, qlen, a->match_case)) {
            int s = i > line_start + 40 ? i - 40 : line_start;
            while (s < i && (t[s] & 0xC0) == 0x80) s++;
            int e = i + (int)qlen;
            while (e < n && t[e] != '\n' && e - s < 140) e++;
            int len = e - s;
            while (len > 0 && (t[s + len - 1] & 0xC0) == 0x80) len--;
            np_hit *h = &a->hits[a->hit_count++];
            h->line = line;
            h->offset = i;
            snprintf(h->text, sizeof h->text, "%.*s", len, d->text + s);
            if (a->hit_count == NP_HIT_MAX) break;
        }
        if (t[i] == '\n') { line++; line_start = i + 1; }
    }
}

static void np_output_panel(cui_ctx *ui, notepad *a) {
    np_doc *d = np_cur(a);
    if (a->out_tab == 0) np_build_hits(a, d);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_SURFACE_MUTED, .gap = 0, .pad = 0) {
        cui_row(ui, .width = CUI_FILL, .pad = 6, .gap = 4) {
            if (cui_button(ui, "查找结果", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_SEARCH,
                           .selected = a->out_tab == 0)) a->out_tab = 0;
            if (cui_button(ui, "消息", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_TERMINAL,
                           .selected = a->out_tab == 1)) a->out_tab = 1;
            cui_spacer(ui);
            char info[128];
            if (a->out_tab == 0)
                snprintf(info, sizeof info, "“%s”  %d 处",
                         a->find_query[0] ? a->find_query : "未设置查找词", a->hit_count);
            else
                snprintf(info, sizeof info, "%d 条", a->msg_count);
            cui_text(ui, info, .size = 11, .nowrap = true,
                     .color = CUI_COLOR_TEXT_MUTED);
            if (cui_icon_button(ui, CUI_ICON_TRASH_2, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST, .tooltip = "清空")) {
                if (a->out_tab == 1) a->msg_count = 0;
                else { a->hit_count = 0; a->hits_rev = 0; }
            }
            if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST,
                                .tooltip = "关闭输出窗口  F7"))
                a->show_output = false;
        }
        cui_separator(ui);

        cui_scroll(ui, .id = "output_list", .width = CUI_FILL,
                   .height = CUI_FILL, .pad = 4, .gap = 0, .horizontal = true) {
            if (a->out_tab == 0) {
                if (!a->find_query[0]) {
                    cui_text(ui, "按 Ctrl+F 输入查找词，或用「搜索 → 在当前文件中查找」列出所有匹配行。",
                             .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                } else if (!a->hit_count) {
                    cui_text(ui, "没有找到匹配行。", .size = 12,
                             .color = CUI_COLOR_TEXT_MUTED);
                } else {
                    for (int i = 0; i < a->hit_count; i++) {
                        np_hit *h = &a->hits[i];
                        char label[256], num[32];
                        snprintf(label, sizeof label, "%d: %s###hit%d",
                                 h->line, h->text, i);
                        snprintf(num, sizeof num, "第 %d 行", h->line);
                        if (cui_list_item(ui, label, .icon = CUI_ICON_SEARCH,
                                          .trailing = num) && d)
                            cui_code_editor_select(a->ui, d->editor_id, h->offset,
                                                   h->offset + (int)strlen(a->find_query));
                    }
                }
            } else {
                if (!a->msg_count)
                    cui_text(ui, "暂无消息。打开、保存和替换文件时会在这里留下记录。",
                             .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                for (int i = 0; i < a->msg_count; i++)
                    cui_text(ui, a->msgs[i], .size = 12, .nowrap = true,
                             .font = CUI_FONT_MONO);
            }
        }
    }
}

static void np_editor_pane(cui_ctx *ui, notepad *a, np_doc *d) {
    /* Keep a little headroom so ordinary typing never hits the ceiling. */
    if ((size_t)d->len + 8192 > d->cap)
        np_ensure_cap(d, (size_t)d->len + 65536 + 1);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 0) {
        cui_code_editor_result r = cui_code_editor(
            ui, d->editor_id, d->text, d->cap,
            .lang = NP_LANGS[d->lang].id,
            .height = -1,
            .size = (float)a->font_size,
            .length = d->len,          /* skip a full strlen on every frame */
            .revision = d->rev,        /* rebuild the line index only when needed */
            .no_line_numbers = !a->line_numbers,
            .placeholder = "在这里输入文字…  Ctrl+N 新建标签，Ctrl+O 打开文件，Ctrl+S 保存",
            .read_only = false);
        if (r.changed) {
            d->dirty = true;
            d->len = r.length;
            d->content_rev++;          /* CUI already rebuilt its own index */
            np_update_title(a);
        } else {
            d->len = r.length;
        }
        if (r.refused) {
            if (np_ensure_cap(d, d->cap + (size_t)4 * 1024 * 1024))
                np_toast(a, "缓冲区已自动扩容", "可以继续输入", CUI_TONE_INFO);
            else
                np_toast(a, "文本已满", "已达到 256 MB 上限", CUI_TONE_WARNING);
        }
        d->caret = r.cursor;
        d->anchor = r.anchor;
        d->line = r.line;
        d->column = r.column;
        d->line_count = r.line_count;
        d->find_matches = r.find_matches;
        d->find_index = r.find_index;
        d->editor_focused = r.focused;

        /* Same id scope as the editor above: keep the other tabs' editor
         * states alive so their undo stacks and carets survive. */
        np_keep_editors(a, ui);
    }
}

/* Document strip + editor + the optional output pane. Returns the document
 * that ended up active (the tab strip can change it this frame). */
static np_doc *np_workspace(cui_ctx *ui, notepad *a, bool busy) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_doc_tab tabs[NP_DOC_MAX];
        for (int i = 0; i < a->count; i++) {
            np_doc *t = a->docs[i];
            const char *name = t->path[0] ? np_base(t->path) : "未命名";
            snprintf(t->tab_title, sizeof t->tab_title, "%s###npdoc%u", name, t->uid);
            char sz[48];
            np_size_str(sz, sizeof sz, t->path[0] ? t->file_size : (size_t)t->len);
            snprintf(t->tab_tip, sizeof t->tab_tip, "%s  ·  %s  ·  %s%s",
                     t->path[0] ? t->path : "尚未保存到文件", sz,
                     NP_LANGS[t->lang].label, t->dirty ? "  ·  未保存" : "");
            tabs[i] = (cui_doc_tab){ .title = t->tab_title,
                                     .icon = CUI_ICON_FILE_TEXT,
                                     .tooltip = t->tab_tip,
                                     .dirty = t->dirty };
        }
        cui_doc_tabs_result tr = cui_doc_tabs(ui, "doctabs", tabs, a->count,
                                              &a->active, .shortcuts = !busy);
        if (!busy) {
            if (tr.changed) np_activate(a, a->active, true);
            if (tr.move_from >= 0 && tr.move_to >= 0) {
                np_move_doc(a, tr.move_from, tr.move_to);
                np_update_title(a);
            } else if (tr.close >= 0) {
                np_request_close(a, tr.close);
            }
        }

        np_doc *d = np_cur(a);
        if (a->show_output) {
            cui_split(ui, "np_out_split", &a->out_ratio, .vertical = true,
                      .min_first = 140, .min_second = 100) {
                np_editor_pane(ui, a, d);
                cui_split_next(ui);
                np_output_panel(ui, a);
            }
        } else {
            np_editor_pane(ui, a, d);
        }
    }
    return np_cur(a);
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

static void np_frame(cui_ctx *ui, void *user) {
    notepad *a = (notepad *)user;
    a->ui = ui;

    if (!a->boot_done) {
        a->boot_done = true;
        np_load_fonts(ui, a);
        a->dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
#ifdef _WIN32
        np_install_close_hook();
#endif
        a->quiet = true;
        for (int i = 0; i < a->startup_count; i++)
            np_open_path(a, a->startup[i], false);
        a->quiet = false;
        a->last_active = a->active;
        np_logf(a, "就绪  ·  %d 个文档  ·  目录 %s", a->count, a->side_dir);
    }

    /* EditPlus-style "directory follows the active file". */
    if (a->follow_active && a->active != a->last_active) {
        np_doc *cur = np_cur(a);
        if (cur && cur->path[0]) {
            char dir[NP_PATH_CAP];
            np_dir_of(cur->path, dir, sizeof dir);
            if (strcmp(dir, a->side_dir) != 0) np_dir_set(a, dir);
        }
    }
    a->last_active = a->active;

    np_update_title(a);
    np_doc *d = np_cur(a);
    np_stats(d);

    bool busy = np_any_dialog(a);
    static const int digit_keys[9] = {
        CUI_KEY_1, CUI_KEY_2, CUI_KEY_3, CUI_KEY_4, CUI_KEY_5,
        CUI_KEY_6, CUI_KEY_7, CUI_KEY_8, CUI_KEY_9
    };

    /* Global shortcuts. Ctrl+S still works while the editor has the keyboard. */
    if (!busy) {
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_N)) {
            np_doc *fresh = np_doc_new(a);
            if (fresh) np_activate(a, a->count - 1, true);
        }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O)) np_open_dialog(a);
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_S)) np_save_current(a, np_cur(a));
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_S))
            np_save_as(a, np_cur(a));
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_Q)) np_request_quit(a);
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_H)) a->dlg_replace = true;

        if (!cui_wants_keyboard(ui)) {
            if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_F)) np_open_find(a);
            if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_G)) a->dlg_goto = true;
        }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_EQUAL) ||
            cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_EQUAL))
            a->font_size = NP_MIN(NP_FONT_MAX, a->font_size + 1);
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_MINUS))
            a->font_size = NP_MAX(NP_FONT_MIN, a->font_size - 1);
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_0)) a->font_size = NP_FONT_DEF;
        for (int k = 0; k < 9 && k < a->count; k++)
            if (cui_shortcut(ui, CUI_MOD_PRIMARY, digit_keys[k])) np_activate(a, k, true);

        /* Workspace toggles, in the spirit of EditPlus's View menu. */
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_B)) a->show_sidebar = !a->show_sidebar;
        if (cui_key_pressed(ui, CUI_KEY_F7)) a->show_output = !a->show_output;
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_F)) {
            a->show_output = true;
            a->out_tab = 0;
            a->hits_rev = 0;
            if (!a->find_query[0]) np_open_find(a);
        }

        if (cui_key_pressed(ui, CUI_KEY_F5)) {
            char stamp[64];
            time_t now = time(NULL);
            struct tm *lt = localtime(&now);
            if (lt && strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", lt))
                np_insert_at_caret(a, stamp);
        }

        int dropped = cui_dropped_file_count(ui);
        for (int i = 0; i < dropped && i < NP_DOC_MAX; i++) {
            const char *p = cui_dropped_file(ui, i);
            if (p && *p) np_open_path(a, p, true);
        }
    }

    /* Files handed over by the OS after launch: macOS Finder "Open With",
     * Quick Actions and Dock drops arrive as Apple Events, not as argv.
     * Drained outside the !busy block so they work even while a dialog is up. */
    int pending = cui_app_pending_file_count();
    for (int i = 0; i < pending && i < NP_DOC_MAX; i++) {
        const char *p = cui_app_pending_file(i);
        if (p && *p) np_open_path(a, p, true);
    }
    if (pending) cui_app_clear_pending_files();

    /* Menu bar -------------------------------------------------------- */
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_BACKGROUND, .gap = 0) {
        cui_menu_bar(ui, "menubar") {
            cui_menu_bar_menu(ui, "文件") {
                if (cui_menu_item(ui, "新建标签", .icon = CUI_ICON_SQUARE_PEN, .shortcut = "Ctrl N")) {
                    if (np_doc_new(a)) np_activate(a, a->count - 1, true);
                }
                if (cui_menu_item(ui, "打开…", .icon = CUI_ICON_FOLDER_OPEN, .shortcut = "Ctrl O"))
                    np_open_dialog(a);
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "保存", .icon = CUI_ICON_SAVE, .shortcut = "Ctrl S",
                                  .disabled = !d->dirty))
                    np_save_current(a, d);
                if (cui_menu_item(ui, "另存为…", .shortcut = "Ctrl Shift S"))
                    np_save_as(a, d);
                if (cui_menu_item(ui, "全部保存", .icon = CUI_ICON_LAYERS))
                    np_save_all(a);
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "关闭当前标签", .icon = CUI_ICON_X, .shortcut = "Ctrl W"))
                    np_request_close(a, a->active);
                if (a->recent_count) {
                    cui_menu_separator(ui);
                    cui_menu_label(ui, "最近打开");
                    for (int i = 0; i < a->recent_count; i++) {
                        char label[NP_PATH_CAP + 32];
                        snprintf(label, sizeof label, "%s##recent%d", np_base(a->recent[i]), i);
                        if (cui_menu_item(ui, label, .icon = CUI_ICON_FILE_TEXT))
                            np_open_path(a, a->recent[i], true);
                    }
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "退出", .icon = CUI_ICON_LOG_OUT, .shortcut = "Ctrl Q"))
                    np_request_quit(a);
            }

            cui_menu_bar_menu(ui, "编辑") {
                if (cui_menu_item(ui, "全选", .shortcut = "Ctrl A"))
                    cui_code_editor_select(ui, d->editor_id, 0, d->len);
                if (cui_menu_item(ui, "插入日期/时间", .icon = CUI_ICON_CLOCK, .shortcut = "F5")) {
                    char stamp[64];
                    time_t now = time(NULL);
                    struct tm *lt = localtime(&now);
                    if (lt && strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", lt))
                        np_insert_at_caret(a, stamp);
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "清空内容", .icon = CUI_ICON_TRASH_2,
                                  .danger = true, .disabled = !d->text[0])) {
                    d->text[0] = 0;
                    np_text_changed(a, d);
                    np_bump_editor(a, d);
                }
                cui_menu_separator(ui);
                cui_menu_label(ui, "撤销 Ctrl Z   重做 Ctrl Y / Ctrl Shift Z");
            }

            cui_menu_bar_menu(ui, "搜索") {
                if (cui_menu_item(ui, "查找…", .icon = CUI_ICON_SEARCH, .shortcut = "Ctrl F"))
                    np_open_find(a);
                if (cui_menu_item(ui, "查找和替换…", .icon = CUI_ICON_REFRESH_CW, .shortcut = "Ctrl H"))
                    a->dlg_replace = true;
                if (cui_menu_item(ui, "转到行…", .icon = CUI_ICON_ARROW_RIGHT, .shortcut = "Ctrl G"))
                    a->dlg_goto = true;
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "在当前文件中查找", .icon = CUI_ICON_LIST,
                                  .shortcut = "Ctrl Shift F")) {
                    a->show_output = true;
                    a->out_tab = 0;
                    a->hits_rev = 0;
                    if (!a->find_query[0]) np_open_find(a);
                }
                if (cui_menu_item(ui, "显示输出窗口", .icon = CUI_ICON_TERMINAL,
                                  .shortcut = "F7", .checked = a->show_output))
                    a->show_output = !a->show_output;
                if (cui_menu_item(ui, "清空输出", .icon = CUI_ICON_TRASH_2)) {
                    a->msg_count = 0;
                    a->hit_count = 0;
                    a->hits_rev = 0;
                }
            }

            cui_menu_bar_menu(ui, "视图") {
                if (cui_menu_item(ui, "目录 / 工作区侧栏", .icon = CUI_ICON_PANEL_LEFT,
                                  .shortcut = "Ctrl B", .checked = a->show_sidebar))
                    a->show_sidebar = !a->show_sidebar;
                if (cui_menu_item(ui, "目录跟随当前文件", .icon = CUI_ICON_BOOKMARK,
                                  .checked = a->follow_active))
                    a->follow_active = !a->follow_active;
                if (cui_menu_item(ui, "刷新目录", .icon = CUI_ICON_REFRESH_CW))
                    a->side_reload = true;
                if (cui_menu_item(ui, "选择工作目录…", .icon = CUI_ICON_FOLDER_OPEN)) {
                    char dir[NP_PATH_CAP];
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_FOLDER, .title = "选择工作目录" },
                            dir, sizeof dir))
                        np_dir_set(a, dir);
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "深色模式", .icon = CUI_ICON_MOON, .checked = a->dark)) {
                    a->dark = !a->dark;
                    cui_app_follow_system_theme(NULL, false);
                    cui_set_theme(ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
                }
                if (cui_menu_item(ui, "切换强调色", .icon = CUI_ICON_PALETTE)) {
                    a->accent = (a->accent + 1) % CUI_ACCENT_COUNT;
                    cui_set_accent(ui, (cui_accent)a->accent);
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "放大字号", .icon = CUI_ICON_ZOOM_IN, .shortcut = "Ctrl +"))
                    a->font_size = NP_MIN(NP_FONT_MAX, a->font_size + 1);
                if (cui_menu_item(ui, "缩小字号", .icon = CUI_ICON_ZOOM_OUT, .shortcut = "Ctrl -"))
                    a->font_size = NP_MAX(NP_FONT_MIN, a->font_size - 1);
                if (cui_menu_item(ui, "重置字号", .shortcut = "Ctrl 0")) a->font_size = NP_FONT_DEF;
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "显示行号", .checked = a->line_numbers))
                    a->line_numbers = !a->line_numbers;
                if (cui_menu_item(ui, "显示状态栏", .checked = a->show_status))
                    a->show_status = !a->show_status;
            }

            cui_menu_bar_menu(ui, "文档") {
                char cur[64];
                snprintf(cur, sizeof cur, "当前：%s%s", NP_LANGS[d->lang].label,
                         d->lang_manual ? "（手动）" : "（自动）");
                cui_menu_label(ui, cur);
                cui_menu_separator(ui);
                for (int i = 0; i < NP_LANG_COUNT; i++) {
                    char label[64];
                    snprintf(label, sizeof label, "%s##lang%d", NP_LANGS[i].label, i);
                    if (cui_menu_item(ui, label, .checked = (d->lang == i),
                                      .icon = i == 0 ? CUI_ICON_FILE_TEXT : CUI_ICON_CODE)) {
                        d->lang = i;
                        d->lang_manual = true;
                    }
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "选择语言…", .icon = CUI_ICON_PALETTE))
                    np_open_lang_dialog(a);
                if (cui_menu_item(ui, "按扩展名重新识别", .icon = CUI_ICON_REFRESH_CW)) {
                    d->lang = np_detect_lang(d->path, d->text);
                    d->lang_manual = false;
                }
                if (cui_menu_item(ui, "恢复自动识别", .checked = !d->lang_manual)) {
                    d->lang = np_detect_lang(d->path, d->text);
                    d->lang_manual = false;
                }
            }

            cui_menu_bar_menu(ui, "格式") {
                cui_menu_label(ui, "等宽网格：长行横向滚动，不自动换行");
                cui_menu_separator(ui);
                char nl[64];
                snprintf(nl, sizeof nl, "换行符：%s", d->crlf ? "CRLF (Windows)" : "LF (Unix)");
                if (cui_menu_item(ui, nl)) d->crlf = !d->crlf;
                char en[64];
                snprintf(en, sizeof en, "保存编码：%s（点击切换）", np_enc_name(d->enc));
                if (cui_menu_item(ui, en)) d->enc = (d->enc + 1) % 3;
                cui_menu_separator(ui);
                cui_menu_label(ui, "以上两项只作用于当前标签");
            }

            cui_menu_bar_menu(ui, "帮助") {
                if (cui_menu_item(ui, "快捷键", .icon = CUI_ICON_KEYBOARD)) a->dlg_help = true;
                if (cui_menu_item(ui, "关于", .icon = CUI_ICON_INFO)) a->dlg_about = true;
            }
        }

        /* Toolbar ----------------------------------------------------- */
        cui_toolbar(ui, "tools") {
            if (cui_toolbar_button(ui, CUI_ICON_PLUS, .tooltip = "新建标签", .shortcut = "Ctrl N")) {
                if (np_doc_new(a)) np_activate(a, a->count - 1, true);
            }
            if (cui_toolbar_button(ui, CUI_ICON_FOLDER_OPEN, .tooltip = "打开", .shortcut = "Ctrl O"))
                np_open_dialog(a);
            if (cui_toolbar_button(ui, CUI_ICON_SAVE, .tooltip = "保存", .shortcut = "Ctrl S",
                                   .disabled = !d->dirty))
                np_save_current(a, d);
            if (cui_toolbar_button(ui, CUI_ICON_LAYERS, .tooltip = "全部保存"))
                np_save_all(a);
            if (cui_toolbar_button(ui, CUI_ICON_X, .tooltip = "关闭当前标签", .shortcut = "Ctrl W"))
                np_request_close(a, a->active);
            cui_toolbar_separator(ui);
            if (cui_toolbar_button(ui, CUI_ICON_SEARCH, .tooltip = "查找", .shortcut = "Ctrl F"))
                np_open_find(a);
            if (cui_toolbar_button(ui, CUI_ICON_REFRESH_CW, .tooltip = "替换", .shortcut = "Ctrl H"))
                a->dlg_replace = true;
            if (cui_toolbar_button(ui, CUI_ICON_LIST, .tooltip = "在当前文件中查找…",
                                   .shortcut = "Ctrl Shift F")) {
                a->show_output = true;
                a->out_tab = 0;
                a->hits_rev = 0;
                if (!a->find_query[0]) np_open_find(a);
            }
            if (cui_toolbar_button(ui, CUI_ICON_CODE, .tooltip = "语法高亮语言…"))
                np_open_lang_dialog(a);
            cui_toolbar_separator(ui);
            cui_toolbar_button(ui, CUI_ICON_PANEL_LEFT,
                               .tooltip = "目录 / 工作区侧栏  Ctrl B",
                               .toggle = &a->show_sidebar);
            cui_toolbar_button(ui, CUI_ICON_TERMINAL, .tooltip = "输出窗口  F7",
                               .toggle = &a->show_output);
            cui_toolbar_separator(ui);
            if (cui_toolbar_button(ui, CUI_ICON_ZOOM_OUT, .tooltip = "缩小字号"))
                a->font_size = NP_MAX(NP_FONT_MIN, a->font_size - 1);
            if (cui_toolbar_button(ui, CUI_ICON_ZOOM_IN, .tooltip = "放大字号"))
                a->font_size = NP_MIN(NP_FONT_MAX, a->font_size + 1);
            cui_toolbar_spacer(ui);
            if (cui_toolbar_button(ui, a->dark ? CUI_ICON_SUN : CUI_ICON_MOON,
                                   .tooltip = a->dark ? "浅色模式" : "深色模式")) {
                a->dark = !a->dark;
                cui_app_follow_system_theme(NULL, false);
                cui_set_theme(ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
            }
        }

        /* Main workspace: directory pane | (tabs + editor + output pane).
         * Both dividers are draggable; the ratios live in the app state. */
        if (a->show_sidebar) {
            cui_split(ui, "np_main_split", &a->side_ratio,
                      .min_first = 175, .min_second = 400) {
                np_sidebar(ui, a);
                cui_split_next(ui);
                d = np_workspace(ui, a, busy);
            }
        } else {
            d = np_workspace(ui, a, busy);
        }

        /* Status bar -------------------------------------------------- */
        if (a->show_status) {
            cui_status_bar(ui, "status") {
                if (d->dirty)
                    cui_status_bar_text(ui, "未保存", .icon = CUI_ICON_DOT, .tone = CUI_TONE_WARNING);
                else
                    cui_status_bar_text(ui, "已保存", .icon = CUI_ICON_CHECK, .tone = CUI_TONE_SUCCESS);
                char path[256 + 32];
                snprintf(path, sizeof path, "%s###path",
                         d->path[0] ? np_base(d->path) : "未命名文档");
                cui_status_bar_text(ui, path,
                                    .tooltip = d->path[0] ? d->path : "尚未保存到文件");
                char tabcnt[48];
                snprintf(tabcnt, sizeof tabcnt, "· %d/%d 标签###tabs", a->active + 1, a->count);
                cui_status_bar_text(ui, tabcnt, .tone = CUI_TONE_INFO);
                char panes[64];
                snprintf(panes, sizeof panes, "%s%s%s###panes",
                         a->show_sidebar ? "侧栏" : "",
                         a->show_sidebar && a->show_output ? " · " : "",
                         a->show_output ? "输出" : "");
                if (!a->show_sidebar && !a->show_output)
                    snprintf(panes, sizeof panes, "单栏###panes");
                if (cui_status_bar_item(ui, panes, .tooltip = "点击：切换侧栏；Ctrl+B / F7 分别控制侧栏和输出窗口"))
                    a->show_sidebar = !a->show_sidebar;
                cui_status_bar_spacer(ui);
                if (d->find_matches > 0) {
                    char find[64];
                    snprintf(find, sizeof find, "匹配 %d/%d###find",
                             d->find_index, d->find_matches);
                    cui_status_bar_text(ui, find, .tone = CUI_TONE_INFO);
                }
                int sel = d->anchor > d->caret ? d->anchor - d->caret
                                               : d->caret - d->anchor;
                if (sel > 0) {
                    char selbuf[48];
                    snprintf(selbuf, sizeof selbuf, "已选 %d 字符###sel", sel);
                    cui_status_bar_text(ui, selbuf, .tone = CUI_TONE_INFO);
                }
                char pos[64];
                snprintf(pos, sizeof pos, "第 %d 行, 第 %d 列###pos", d->line, d->column);
                if (cui_status_bar_item(ui, pos, .tooltip = "转到行…")) a->dlg_goto = true;
                char count[96];
                if ((size_t)d->len >= NP_BIG_FILE) {
                    char sz[48];
                    np_size_str(sz, sizeof sz, (size_t)d->len);
                    snprintf(count, sizeof count, "%d 行 · %s · %d 字符###count",
                             d->line_count, sz, d->chars);
                } else {
                    snprintf(count, sizeof count, "%d 行 · %d 字符 · %d 词###count",
                             d->line_count, d->chars, d->words);
                }
                cui_status_bar_text(ui, count);
                char lg[64];
                snprintf(lg, sizeof lg, "%s###lang", NP_LANGS[d->lang].label);
                if (cui_status_bar_item(ui, lg, .tooltip = "点击选择语法高亮语言"))
                    np_open_lang_dialog(a);
                char enc[48];
                snprintf(enc, sizeof enc, "%s###enc", np_enc_name(d->enc));
                if (cui_status_bar_item(ui, enc, .tooltip = "点击切换保存编码"))
                    d->enc = (d->enc + 1) % 3;
                char nl[32];
                snprintf(nl, sizeof nl, "%s###nl", d->crlf ? "CRLF" : "LF");
                if (cui_status_bar_item(ui, nl, .tooltip = "点击切换换行符"))
                    d->crlf = !d->crlf;
                char fs[32];
                snprintf(fs, sizeof fs, "%d pt###font", a->font_size);
                if (cui_status_bar_item(ui, fs, .tooltip = "点击重置字号"))
                    a->font_size = NP_FONT_DEF;
            }
        }
    }

    np_dialogs(a, ui);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    g.font_size = NP_FONT_DEF;
    g.line_numbers = true;
    g.show_status = true;
    g.accent = CUI_ACCENT_BLUE;
    g.match_case = false;
    g.pending_action = ACT_NONE;
    g.pending_doc = -1;
    g.show_sidebar = true;
    g.show_output = true;
    g.follow_active = true;
    g.side_ratio = 0.22f;
    g.out_ratio = 0.76f;
    g.side_tab = 0;
    g.out_tab = 1;           /* start on the message log */
    g.last_active = -1;
#ifdef _WIN32
    g.default_crlf = true;
#endif
    np_init_side_dir(&g);
    np_doc_new(&g);          /* the initial empty tab */

#ifdef _WIN32
    /* argv from the CRT is in the ANSI code page; use the wide command line
     * instead so a path with Chinese characters round-trips as UTF-8. */
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc && g.startup_count < NP_DOC_MAX; i++)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                g.startup[g.startup_count++], NP_PATH_CAP, NULL, NULL);
        LocalFree(wargv);
    }
#else
    for (int i = 1; i < argc && g.startup_count < NP_DOC_MAX; i++)
        snprintf(g.startup[g.startup_count++], NP_PATH_CAP, "%s", argv[i]);
#endif

    return cui_app_run(&(cui_app_desc){
        .title = "未命名 - RapidEditor",
        .width = 1120, .height = 760,
        .min_width = 620, .min_height = 460,
        .app_id = "com.rapidaire.rapideditor",
        .accent = CUI_ACCENT_BLUE,
        .follow_system_theme = true
    }, np_frame, &g);
}
