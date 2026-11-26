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
#include "np_preview.h"
#include "np_webview.h"
#include "np_font_bundle.h"
#include "np_updater.h"
#include "np_latex_templates.h"
#include "np_zip.h"
#include "np_net.h"
#include "np_toolinstall.h"      /* T3/T4: 工具链下载与隔离安装 */
#include "np_toolinstall_zip.h"  /* T4: 已校验 zip → 隔离 tool_dir */
#include "np_tinytex.h"
#include "np_templates_online.h"
#include "np_bt.h"
#include "np_bt_templates.h"
#include "np_latex_table.h"
#include "np_latex_tidy.h"
#include "np_latex_html.h"
#include "np_font_scan.h"
#include "np_ai.h"
#include "np_qr.h"
#include "np_maclaw.h"
#include "np_openai.h"
#include "np_review_panel.h"   /* M2/B3: 统一 Patch Review 面板 */
#include "np_plancard_ui.h"    /* M4/B2: 计划卡面板（X2 工具路由 + F3 schema 树） */
#include "np_composer_ui.h"    /* M3/B1: AI 工作台（意图表单 → Job 卡 → B3） */
#include "np_ooxml_ui.h"       /* F2: OOXML(OPC) 结构树面板 */
#include "np_byteintel.h"      /* H3/H4: 字节解释 + 结构化字节搜索 */
#include "np_bytediff.h"       /* H7: 字节 Diff（对照文件 → B3 审查） */
#include "np_searchprofile.h"  /* X9: 可配置知识搜索接口（SearchProfile） */
#include "np_mcp.h"            /* X6: MCP 工具服务器（动态发现/健康检查/不可信输出） */

/* App version for the update check. Keep in sync with versionName in
 * android/app/build.gradle (single source of truth for the Android build
 * is there; this macro drives the GitHub tag comparison). */
#define NP_APP_VERSION "1.3.9"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <pthread.h>   /* background template/TinyTeX workers */
#endif
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <direct.h>         /* _wmkdir */
#include <wincrypt.h>       /* DPAPI: encrypt saved LLM API keys */
#endif

#ifdef __ANDROID__
#include <SDL.h> /* SDL_OpenURL: ACTION_VIEW intent for files and links */
#endif

#define NP_TEXT_INIT  ((size_t)64 * 1024)              /* initial buffer per tab */
#define NP_TEXT_MAX   ((size_t)256 * 1024 * 1024)       /* hard ceiling per tab */
#define NP_LOAD_MAX   ((size_t)240 * 1024 * 1024)       /* largest file read in full */
#define NP_BIG_FILE   ((size_t)8 * 1024 * 1024)         /* show a size hint above this */
#define NP_PATH_CAP   1024
#define BT_SRC_CAP    (32 * 1024)                       /* binary template script size */
#define NP_DOC_MAX    16
#define NP_RECENT_MAX 6
#define NP_FONT_MIN   10
#define NP_FONT_MAX   28
#define NP_FONT_DEF   14
#define NP_DIR_MAX    400                              /* entries in the directory pane */
#define NP_LOG_MAX    120                              /* lines kept in the output log */
#define NP_HIT_MAX    300                              /* find results shown */
#define NP_SESSION_MAX 16                              /* files restored from last run */
#define NP_FF_DIR_MAX  64                              /* directories scanned per search */
#define NP_FF_FILE_MAX 64                              /* file paths kept in results */
#define NP_FF_MAX      400                             /* find-in-files result lines */
#define NP_FF_SCAN_MAX 4000                            /* files opened at most per search */
#define NP_FF_BYTES    (2 * 1024 * 1024)               /* per-file scan limit */
#define NP_FF_ROW_MAX  512                             /* entries read per directory */
#define NP_TL_MAX      8192                            /* lines per text transform */
#define NP_AI_HUNK_MAX 128                             /* diff decisions per result */
#define NP_AI_HISTORY_MAX 4                            /* AI results kept per document */
#define NP_AI_SNIP_CAP 8192                            /* per-document AI text snapshot */

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

static void np_dir_of(const char *path, char *out, size_t cap);  /* defined below */

/* Resolve a path against the process cwd; absolute input passes through.
 * Document paths are stored absolute so the tree, follow-mode and reveal
 * agree no matter where the app was launched from. */
static void np_abs_path(const char *path, char *out, size_t cap) {
    bool absolute =
#ifdef _WIN32
        (path[0] && path[1] == ':') || path[0] == '/' || path[0] == '\\';
#else
        path[0] == '/';
#endif
    if (absolute) {
        snprintf(out, cap, "%s", path);
        return;
    }
    char cwd[NP_PATH_CAP];
#ifdef _WIN32
    wchar_t w[1024];
    DWORD n = GetCurrentDirectoryW(1024, w);
    if (n > 0 && n < 1024)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, cwd, sizeof cwd, NULL, NULL);
    else
        snprintf(cwd, sizeof cwd, ".");
#else
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
#endif
    snprintf(out, cap, "%s%s%s", cwd,
             cwd[strlen(cwd) - 1] == '/' || cwd[strlen(cwd) - 1] == '\\' ? "" : "/",
             path);
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

/* P2 #7/#8: one file-type look shared by the sidebar, doc tabs and the
 * breadcrumb. Families are coarse on purpose; tints reuse the categorical
 * series palette (the same family the editor marks tint with), which stays
 * legible on every style preset in both light and dark. */
enum {
    NP_FK_TEXT = 0, NP_FK_C, NP_FK_WEB, NP_FK_PY, NP_FK_RS, NP_FK_GO,
    NP_FK_JSON, NP_FK_SH, NP_FK_SQL, NP_FK_MD, NP_FK_TEX, NP_FK_HEX,
};

static int np_lang_family(int lang) {
    const char *id = NP_LANGS[lang].id;
    if (!strcmp(id, "c") || !strcmp(id, "cpp")) return NP_FK_C;
    if (!strcmp(id, "js") || !strcmp(id, "ts")) return NP_FK_WEB;
    if (!strcmp(id, "python")) return NP_FK_PY;
    if (!strcmp(id, "rust")) return NP_FK_RS;
    if (!strcmp(id, "go")) return NP_FK_GO;
    if (!strcmp(id, "json")) return NP_FK_JSON;
    if (!strcmp(id, "shell")) return NP_FK_SH;
    if (!strcmp(id, "sql")) return NP_FK_SQL;
    return NP_FK_TEXT;
}

/* families NP_LANGS does not know: docs, TeX, web markup/styles, config */
static int np_file_family(const char *path) {
    char ext[32];
    size_t i = 0;
    for (const char *p = np_ext(path); *p && i + 1 < sizeof ext; p++, i++)
        ext[i] = (char)tolower((unsigned char)*p);
    ext[i] = 0;
    if (ext[0]) {
        if (np_ext_in("html;htm;xhtml;css;scss;sass;less;vue;svelte;astro", ext)) return NP_FK_WEB;
        if (np_ext_in("md;markdown;mdx;rst;adoc", ext)) return NP_FK_MD;
        if (np_ext_in("tex;sty;cls;bib;dtx;ins", ext)) return NP_FK_TEX;
        if (np_ext_in("ini;cfg;conf;toml;yaml;yml;properties;env;bat;cmd;ps1;reg;cmake", ext)) return NP_FK_SH;
        for (int k = 1; k < NP_LANG_COUNT; k++)
            if (np_ext_in(NP_LANGS[k].exts, ext)) return np_lang_family(k);
    }
    const char *base = np_base(path);
    if (np_ieq(base, "Makefile") || np_ieq(base, "GNUmakefile") ||
        np_ieq(base, "makefile") || np_ieq(base, "Dockerfile") ||
        np_ieq(base, "CMakeLists.txt") || np_ieq(base, ".bashrc") ||
        np_ieq(base, ".zshrc") || np_ieq(base, ".profile"))
        return NP_FK_SH;
    return NP_FK_TEXT;
}

static const char *np_family_icon(int fk) {
    return fk == NP_FK_HEX ? CUI_ICON_CPU
         : (fk == NP_FK_TEXT || fk == NP_FK_MD) ? CUI_ICON_FILE_TEXT
         : CUI_ICON_CODE;
}

static cui_color np_family_tint(cui_ctx *ui, int fk) {
    const cui_theme *t = cui_theme_get(ui);
    switch (fk) {
    case NP_FK_C:    return t->colors[CUI_TOKEN_SERIES_1];
    case NP_FK_PY:   return t->colors[CUI_TOKEN_SERIES_2];
    case NP_FK_WEB:  return t->colors[CUI_TOKEN_SERIES_3];
    case NP_FK_RS: case NP_FK_GO: return t->colors[CUI_TOKEN_SERIES_4];
    case NP_FK_SH: case NP_FK_TEX: return t->colors[CUI_TOKEN_SERIES_5];
    case NP_FK_JSON: case NP_FK_SQL: return t->colors[CUI_TOKEN_SERIES_6];
    case NP_FK_HEX:  return cui_alpha(t->colors[CUI_TOKEN_FOREGROUND], 0.55f);
    case NP_FK_MD:   return t->colors[CUI_TOKEN_FOREGROUND];   /* docs read bright */
    default:         return (cui_color){0, 0, 0, 0}; /* keep the default */
    }
}

/* ------------------------------------------------------------------ */
/* Application state                                                   */
/* ------------------------------------------------------------------ */

/* One AI result kept alongside a document. Enough context to restore it into
 * the side panel later, including the exact range it was generated for. */
typedef struct np_ai_history {
    char result[NP_AI_SNIP_CAP];
    char orig[NP_AI_SNIP_CAP];
    char inst[256];
    char target_path[NP_PATH_CAP];
    unsigned target_uid;                /* stable document identity */
    uint64_t target_rev;                /* content_rev at generation time */
    int  target_len;                    /* length at generation time */
    int  task;
    int  target_s, target_e;
} np_ai_history;

/* A user-saved AI instruction. Tasks >= 7 index into this array. */
#define NP_AI_ACTION_MAX 8
typedef struct np_ai_action {
    char name[48];
    char inst[512];
} np_ai_action;

/* Unsaved document snapshot written on exit / periodic autosave. */
typedef struct np_draft_entry {
    char path[NP_PATH_CAP];             /* original file, empty for untitled */
    char file[NP_PATH_CAP];             /* UTF-8 snapshot in the config dir */
    int  enc;
    bool crlf;
} np_draft_entry;

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
    time_t disk_mtime;                  /* file mtime at load/save; 0 = unknown */
    time_t disk_mtime_seen;             /* last externally-changed mtime handled */
    bool disk_missing;                  /* the backing file vanished from disk */
    char tex_root[NP_PATH_CAP];         /* multi-file LaTeX: main .tex to compile */
    bool tex_root_manual;               /* user explicitly pinned a main file */
    bool tex_root_probed;               /* auto-detect already ran for this tab */
    char editor_id[48];                 /* unique: CUI keeps undo/caret per id */
    char tab_title[NP_PATH_CAP + 64];   /* "name###key" for cui_doc_tabs */
    char tab_tip[NP_PATH_CAP + 128];
    unsigned uid;                       /* stable tab identity */
    bool dirty;
    bool crlf;
    bool is_hex;                        /* binary file: shown in the hex editor */
    cui_hex_selection hexsel;           /* hex view selection (per document) */
    /* Binary template (010 Editor style) applied to this hex document */
    char      bt_name[96];              /* active template name, empty = none */
    char      bt_source[BT_SRC_CAP];    /* active template script (loaded/embedded) */
    np_bt_result bt_result;             /* last successful parse: fields to highlight */
    uint64_t  bt_content_rev;           /* content_rev the result was parsed at */
    int       bt_first;                 /* first visible field for the tree's virtual list */
    int       bt_pick;                  /* selected field index */
    bool      bt_show_tree;             /* field tree pane visible */
    float     bt_ratio;                 /* hex/tree split ratio */
    bool lang_manual;                   /* user picked a language explicitly */
    int  lang;
    int  enc;
    /* last frame's editor state (used when switching back to this tab) */
    int caret, anchor, line, column, line_count;
    int chars, words, find_matches, find_index;
    bool editor_focused;
    /* Per-document AI result history (session-scoped). */
    np_ai_history ai_hist[NP_AI_HISTORY_MAX];
    int  ai_hist_count, ai_hist_sel;
} np_doc;

/* family for an open document: hex wins, then the path's extension, and a
 * content-detected language as the fallback for unnamed buffers */
static int np_doc_family(const np_doc *d) {
    if (d->is_hex) return NP_FK_HEX;
    int fk = np_file_family(d->path);
    if (fk == NP_FK_TEXT && d->lang != NP_LANG_TEXT) fk = np_lang_family(d->lang);
    return fk;
}

typedef struct np_hit {
    int line;
    int offset;
    char text[160];
} np_hit;

/* One matching line reported by "find in files". `file` indexes ff_paths,
 * or is -1 when the file table is full (line kept only as a count). */
typedef struct np_fhit {
    int file;
    int line;
    int col;
    char text[160];
} np_fhit;

/* Appearance style presets. Each entry is a complete look: light/dark mode,
 * accent color and a neutral tint (OKLCH hue/chroma). To add a style, add one
 * row here — the settings dialog and persistence pick it up automatically. */
typedef struct {
    const char *name;         /* UI label */
    const char *desc;         /* one-line description on the card */
    const char *icon;         /* CUI icon glyph */
    const char *badge;        /* short badge, e.g. "Light" / "Dark" / NULL */
    cui_theme_mode mode;
    cui_accent accent;
    float hue, chroma;        /* neutral tint */
} np_style;

static const np_style np_styles[] = {
    { "极光蓝",   "冷调岩灰 · 沉稳蓝，推荐日常使用", CUI_ICON_SPARKLES, "推荐", CUI_THEME_LIGHT, CUI_ACCENT_BLUE,   255.f, 0.011f },
    { "纯银灰",   "经典中性灰 · 冷静克制",           CUI_ICON_CIRCLE,   "浅色", CUI_THEME_LIGHT, CUI_ACCENT_BLUE,   255.f, 0.0f   },
    { "暖砂",     "暖调米灰 · 琥珀点缀",             CUI_ICON_SUN,      "浅色", CUI_THEME_LIGHT, CUI_ACCENT_AMBER,   70.f,  0.014f },
    { "苔绿",     "自然绿灰 · 护眼柔和",             CUI_ICON_SPARKLES, "浅色", CUI_THEME_LIGHT, CUI_ACCENT_GREEN,   155.f, 0.013f },
    { "紫罗兰",   "淡紫冷灰 · 优雅专注",             CUI_ICON_ZAP,      "浅色", CUI_THEME_LIGHT, CUI_ACCENT_VIOLET,  290.f, 0.013f },
    { "深海",     "冷调深灰 · 深海蓝，长时间编码",   CUI_ICON_MOON,     "深色", CUI_THEME_DARK,  CUI_ACCENT_BLUE,   255.f, 0.012f },
    { "曜石",     "中性纯黑灰 · 极致沉稳",           CUI_ICON_CIRCLE,   "深色", CUI_THEME_DARK,  CUI_ACCENT_NEUTRAL, 255.f, 0.0f   },
    { "午夜紫",   "深紫灰 · 靛紫点缀",               CUI_ICON_STAR,     "深色", CUI_THEME_DARK,  CUI_ACCENT_VIOLET,  290.f, 0.014f },
    { "炭火",     "暖调炭灰 · 琥珀暖意",             CUI_ICON_SUN,      "深色", CUI_THEME_DARK,  CUI_ACCENT_AMBER,   60.f,  0.013f },
};
#define NP_STYLE_COUNT ((int)(sizeof np_styles / sizeof np_styles[0]))

typedef struct notepad {
    np_doc *docs[NP_DOC_MAX];
    int  count, active;
    unsigned next_uid;
    int  editor_generation;
    time_t disk_check_at;               /* last external-change poll */

    char title[NP_PATH_CAP + 128];
    char confirm_desc[NP_PATH_CAP + 128];
    char startup[NP_DOC_MAX][NP_PATH_CAP];
    int  startup_count;
    int  startup_cur[NP_DOC_MAX];       /* session caret per startup path, -1 = none */
    char startup_bt[64];                /* --bt=<builtin> applied to opened hex docs */
    char recent[NP_RECENT_MAX][NP_PATH_CAP];
    int  recent_count;
    np_draft_entry drafts[NP_DOC_MAX];  /* recoverable unsaved buffers */
    int  draft_count;

    bool boot_done, fonts_ready, dark, line_numbers, show_status, default_crlf;
    bool indent_guides;                 /* vertical indentation guides in the editor */
    bool editor_lig;                    /* experimental programming ligatures (needs a ligature face) */
    int  whitespace;                    /* whitespace glyphs: 0 off, 1 leading, 2 all */
    bool quiet;                         /* no "opened" toasts while loading startup files */
    int  accent, font_size;
    int  density;                       /* 0 comfortable, 1 compact, 2 spacious */
    int  style;                         /* index into np_styles; -1 = custom/legacy */

    /* Editor font ------------------------------------------------------ */
    char font_ident[128];               /* bundled id, "default", or "custom" */
    char font_custom_path[NP_PATH_CAP]; /* last custom font file (for restore) */
    cui_font font_cjk;                  /* loaded CJK fallback font, reattached on switch */
    cui_font font_default_mono;         /* cached original mono role font, for reset */
    cui_font font_active;               /* the cui_font currently set on the mono role */
    bool welcome_dismissed;             /* hide the start screen on an empty session */
    int  pending_action, pending_doc;

    /* dialogs */
    bool dlg_confirm, dlg_replace, dlg_goto, dlg_help, dlg_about, dlg_lang;
    bool dlg_settings;                  /* appearance / theme style settings */
    bool dlg_latex_tpl;                 /* new LaTeX document from a template */
    bool dlg_reload;                    /* reload from disk, discarding edits */
    int  latex_tpl_pick;

    /* Online LaTeX template library ----------------------------------- */
    bool dlg_tpl_online;
    bool dlg_tinytex;
    np_online_template tpl_items[NP_ONT_MAX];
    int  tpl_count, tpl_pick;
    char tpl_query[128];                /* GitHub search input */
    char tpl_status[256];               /* progress / error line */
    volatile long tpl_progress;         /* 0..100, written by worker thread */
    volatile bool tpl_cancel;           /* set by UI to abort worker */
    int  tpl_mode;                      /* 0 curated, 1 search results */

    /* TinyTeX auto-install -------------------------------------------- */
    volatile long tt_progress;
    volatile bool tt_cancel;
    bool tt_busy;
    char tt_status[256];

    /* Background template download: worker fills these, UI polls and imports */
    char tpl_dl_url[NP_PATH_CAP];       /* set by UI before starting worker */
    char tpl_dl_stem[128];              /* suggested project folder name */
    char tpl_dl_result[NP_PATH_CAP];    /* downloaded zip path, set by worker */
    char tpl_dl_error[256];
    int  tpl_dl_kind;                   /* 1 download, 2 search */
    volatile long tpl_dl_done;          /* 0 running, 1 ok, 2 error, 3 cancelled */
    bool tpl_dl_busy;

    bool dlg_update;                    /* Online update: check GitHub Releases */
    char update_buf[512];               /* scratch lines for the update dialog */
    int  upd_last_check, upd_last_dl;   /* poll caches: request redraw on flip */
    char find_query[256];
    char replace_with[256];
    char goto_buf[32];
    bool match_case;
    bool find_word;                     /* whole-word matching, shared by find surfaces */
    int  lang_pick;

    cui_ctx *ui;
    char toast_desc[256];

    /* EditPlus-style workspace ---------------------------------------- */
    bool show_sidebar, show_output, follow_active;
    bool show_ai;                   /* AI assistant docked as a side panel */
    bool show_review;               /* B3 补丁审查面板（与 AI 面板互斥显示） */
    bool show_plan;                 /* B2 计划卡面板（X2 路由预告 + F3 schema 树） */
    bool show_workbench;            /* B1 AI 工作台（Composer 意图表单） */
    bool show_ooxml;                /* F2 OOXML 结构树面板（与 AI/计划卡/审查互斥） */
    bool review_allow_write;        /* H8 写入开关持久位（每文件独立由面板管理） */
    bool zen_prev_ai;
    int  last_active;               /* to make the directory pane follow tab switches */
    float side_ratio, out_ratio;    /* first pane's share of each split */
    float ai_ratio;                 /* workspace share when the AI panel is docked */
    float review_ratio;             /* workspace share when the review panel is docked */
    float plan_ratio;               /* workspace share when the plan card is docked */
    float workbench_ratio;          /* workspace share when the workbench is docked */
    float ooxml_ratio;              /* workspace share when the OOXML panel is docked */
    int  side_tab, out_tab;         /* 目录/文档/最近 · 查找结果/消息 */
    char side_dir[NP_PATH_CAP];
    bool side_reload;               /* rebuild the sidebar tree next frame */

    char msgs[NP_LOG_MAX][256];
    int  msg_count;
    np_hit hits[NP_HIT_MAX];
    int  hit_count;
    uint64_t hits_rev, hits_key;
    bool dlg_hexgoto;
    char hexgoto_buf[24];
    bool dlg_hexfind;                   /* binary search: hex bytes or ASCII or structured */
    char hexfind_buf[256];
    bool hexfind_ascii;                 /* treat the query as text, not hex */
    bool hexfind_struct;                /* H4: structured query (u32le=1000 / png:IHDR / fill:... / str:...) */
    bool show_byteinspect;              /* H3: inline byte-interpretation panel in the hex pane */
    size_t hexfind_at;                  /* last hit offset; SIZE_MAX = none */
    size_t hexfind_len;                 /* bytes of the last hit */
    size_t hexfind_hits[400];           /* all hits of the last query: scrollbar ticks */
    int hexfind_hit_count;
    unsigned hexfind_doc_uid;           /* the hits belong to this document */

    /* Binary template (010 Editor style) ------------------------------- */
    bool dlg_bt;                        /* choose a binary template */
    int  bt_pick;                       /* selected builtin in the dialog */
    char bt_error[256];                 /* last parse error to show */
    char bt_custom_path[NP_PATH_CAP];   /* last custom template file loaded */

    /* LaTeX table wizard (paste CSV/TSV -> three-line table) ---------- */
    bool dlg_latex_table;
    char ltable_src[16384];             /* pasted/typed grid text */
    char ltable_caption[256];
    char ltable_label[128];
    char ltable_align[32];
    int  ltable_style;                  /* 0 booktabs, 1 hline grid */
    bool ltable_header, ltable_longtable, ltable_resize;
    float ltable_width;
    int  ltable_rows, ltable_cols;      /* last parsed dims, for the preview line */
    char ltable_preview[8192];          /* last rendered LaTeX, shown read-only */
    char ltable_err[256];

    /* LaTeX source tidying + font setup ------------------------------- */
    bool dlg_latex_tidy;
    bool tidy_indent, tidy_join, tidy_trim;
    int  tidy_spaces;
    bool dlg_latex_fonts;
    char font_latin_name[128], font_cjk_name[128];
    char font_list[262144];             /* flat NUL-separated family names */
    int  font_count;
    int  font_latin_pick, font_cjk_pick;
    int  font_scan_state;               /* 0 not scanned, 1 done */

    /* LaTeX live math preview ----------------------------------------- */
    bool dlg_math_preview;
    char math_expr[1024];               /* formula being previewed (no $ delims) */

    /* AI native panel (local agents + OpenAI-compatible providers) ----- */
    char ai_cmd[512];                   /* user-editable command template */
    char ai_inst[512];                  /* instruction / task */
    char ai_prev[8192];                 /* previous Q&A for follow-up prompts */
    bool ai_followup;                   /* multi-turn: carry previous Q&A */
    int  ai_task;                       /* 0 polish 1 translate 2 check 3 explain 4 custom 5 complete 6 template */
    int  ai_ctx;                        /* 0 selection 1 whole file */
    int  ai_backend;                    /* 0 auto 1 claude 2 codex 3 custom */
    char ai_ctx_path[NP_PATH_CAP];
    char ai_out_path[NP_PATH_CAP];
    char ai_result[65536];              /* agent output (owned copy) */
    char ai_error[512];
    volatile long ai_done;              /* 0 idle 1 running 2 ok 3 error */
    int  ai_last_state;
    char ai_orig[65536];                /* original text that was sent (for diff) */
    int  ai_target_s, ai_target_e;      /* byte range in the doc to apply a rewrite */
    char ai_target_path[NP_PATH_CAP];   /* doc path the range refers to */
    unsigned ai_target_uid;             /* stable doc identity: untitled docs share path="" */
    uint64_t ai_target_rev;             /* doc content_rev captured at request time */
    int  ai_target_len;                 /* doc length captured at request time */
    char ai_ctx_label[64];              /* "LaTeX 源码" / "Python 源码" / ... */
    char ai_extra[8192];                /* extra context: compile log, etc. */
    /* Controllable context: the user opts in to each extra source. All
     * sources are assembled on the UI thread so the panel can preview them. */
    bool ai_ctx_tabs;                   /* include other open tabs */
    bool ai_ctx_finds;                  /* include current / in-files results */
    char ai_ctx_sources[16384];         /* assembled optional context block */
    char ai_ctx_summary[256];           /* pre-send "what will be included" */
    int  ai_ctx_src_doc;                /* doc index the preview was built for */
    np_ai_action ai_actions[NP_AI_ACTION_MAX];
    int  ai_action_count;
    bool dlg_ai_actions;
    int  ai_action_edit;                /* -1 = new, >=0 = index being edited */
    char ai_act_name[48], ai_act_inst[512];

    /* Smart paste: clipboard is detected before insertion, never silently
     * rewritten. The user chooses converted or raw at confirm time. */
    bool dlg_smart_paste;
    char sp_src[32768], sp_out[32768];
    char sp_kind[48];
    int  sp_mode;                       /* 0 table 1 JSON 2 log 3 plain */

    char ai_staged[65536];              /* accepted/rejected hunks start here */
    int  ai_diff_current;               /* selected change in the diff view */
    volatile bool ai_cancel;            /* UI sets this to stop a worker */
    volatile long ai_stream_len;        /* bytes received so far (streaming) */
    bool ai_hist_pushed;                /* result already copied into history */

    /* Inline completion (ghost text after the caret). The editor renders and
     * accepts it (Tab) on its own; here we only decide when to ask the model.
     * Results are validated against the generation counter before showing. */
    bool ai_inline;                     /* enabled from the AI panel */
    int  inline_delay;                  /* idle seconds before firing (1..5) */
    int  inline_fail_streak;            /* consecutive failed completions */
    bool inline_fail_notified;          /* already toasted about the streak */
    volatile long inline_done;          /* 0 idle 1 running 2 ok 3 fail */
    int  inline_gen;                    /* bumped on every edit / caret move */
    int  inline_res_gen;                /* generation the pending result belongs to */
    unsigned inline_last_uid;           /* doc the last activity happened in */
    int  inline_last_cursor;            /* caret at last activity */
    time_t inline_last_edit;            /* wall time of the last edit / caret move */
    char inline_url[NP_LLM_PROV_URL_CAP], inline_key[NP_LLM_PROV_KEY_CAP],
         inline_model[NP_LLM_PROV_MODEL_CAP];       /* worker's provider snapshot */
    char inline_prompt[8192];
    char inline_result[1280];           /* worker-owned until shown */

    /* maclaw official-service login (scan QR with phone) --------------- */
    bool dlg_maclaw;
    char maclaw_center[512];            /* active HubCenter base URL */
    char maclaw_centers[1024];          /* HA candidates, CSV (persisted) */
    char maclaw_hub[512];               /* Hub base URL, resolved at login (persisted, hidden) */
    char maclaw_token[512];             /* viewer_token (persisted) */
    char maclaw_email[256];
    char maclaw_err[256];
    np_maclaw_session maclaw_sess;
    np_qr_code maclaw_qr;
    bool maclaw_qr_ready;
    volatile long maclaw_done;          /* 0 idle 1 creating 2 polling 3 ok 4 error */
    int  maclaw_polls;
    int  maclaw_last_state;

    /* Custom OpenAI-compatible LLM providers (user-managed) ------------ */
    np_llm_provider llm_prov[8];
    int  llm_prov_count;
    bool dlg_llm_prov;
    int  llm_prov_edit;                 /* index being edited, -1 = adding */
    int  llm_prov_gen;                  /* bumped whenever the list/form changes */
    char llm_temp_buf[16];              /* temperature input buffer (form) */
    char llm_tok_buf[16];               /* max_tokens input buffer (form) */
    bool secret_load_failed;            /* dpapi: value could not be decrypted */
    char llm_prov_err[256];
    volatile long llm_models_done;      /* 0 idle 1 fetching 2 ok 3 fail */
    int  llm_models_prov;               /* provider the fetch targets */
    int  llm_models_gen;                /* generation captured when started */
    /* UI-thread snapshots: workers never read the editable provider slots,
     * so typing in the form cannot change a request mid-flight. */
    char llm_models_url[NP_LLM_PROV_URL_CAP];
    char llm_models_key[NP_LLM_PROV_KEY_CAP];
    volatile long llm_test_done;        /* 0 idle 1 testing 2 ok 3 fail */
    int  llm_test_prov;                 /* provider the test targets */
    int  llm_test_gen;
    char llm_test_url[NP_LLM_PROV_URL_CAP];
    char llm_test_key[NP_LLM_PROV_KEY_CAP];
    char llm_test_model[NP_LLM_PROV_MODEL_CAP];
    char llm_test_result[192];          /* success summary: latency + reply */
    char llm_test_err[256];

    /* X9: SearchProfile — 可配置知识搜索接口（只召回，不写） ----------- */
    np_sp_store_t *sp_store;            /* 配置注册表（懒加载） */
    bool dlg_searchprof;                /* 配置对话框 */
    int  sp_edit;                       /* -1 无表单 / -2 新增 / >=0 编辑索引 */
    np_sp_profile_t sp_form;            /* 编辑缓冲 */
    char sp_test_query[128];            /* 本地召回测试词 */
    char sp_test_result[512];           /* 测试输出（脱敏后） */
    bool sp_test_done;

    /* X6: MCP 工具服务器 — 动态发现/健康检查（工具输出不可信） ----------- */
    np_mcp_store_t *mcp_store;          /* 配置注册表（懒加载） */
    bool dlg_mcp;                       /* 扩展管理器对话框 */
    int  mcp_edit;                      /* -1 无表单 / -2 新增 / >=0 编辑索引 */
    np_mcp_server_t mcp_form;           /* 编辑缓冲 */
    char mcp_test_result[512];          /* 发现/健康检查输出（脱敏后） */
    bool mcp_test_done;

    /* EditPlus-style find in files ------------------------------------ */
    /* Case and whole-word toggles are the shared a->match_case /
     * a->find_word, persisted with the rest of the settings. */
    bool dlg_find_files;
    char ff_query[256];
    char ff_pattern[256];
    char ff_dir[NP_PATH_CAP];
    char ff_paths[NP_FF_FILE_MAX][NP_PATH_CAP];
    int  ff_count;                  /* files with at least one hit */
    int  ff_files_seen;             /* files scanned in the last run */
    int  ff_dirs_seen;              /* directories visited in the last run */
    np_fhit ff_hits[NP_FF_MAX];
    int  ff_hit_count;

    /* AI semantic workspace search: natural-language query over the same
     * directory/pattern as find-in-files, results reuse ff_hits so every hit
     * keeps a filename, a source snippet and a jump-to-line target. */
    char sem_prompt[32768];
    /* Snapshot of the candidate table used to build the prompt. A later
     * keyword search rewrites ff_paths; AI result ids must resolve against
     * this frozen copy, otherwise "file 1" points at an unrelated file. */
    char sem_paths[NP_FF_FILE_MAX][NP_PATH_CAP];
    int  sem_count;
    char sem_result[16384];             /* worker-owned: "id | source line" */
    char sem_answer[16384];             /* QA mode: answer body without hit lines */
    int  sem_mode;                      /* 0 locate files, 1 workspace Q&A */
    char sem_error[512];
    volatile long sem_done;             /* 0 idle 1 running 2 ok 3 fail */
    volatile bool sem_cancel;
    char sem_ctx_path[NP_PATH_CAP], sem_out_path[NP_PATH_CAP];
    char sem_summary[256];
    int  sem_backend;                   /* snapshot taken when search starts */
    np_llm_provider sem_prov;           /* provider snapshot for the worker */
    int  sem_last_state;                /* UI: apply results once when done */

    /* Unified preview panel -------------------------------------------- */
    bool show_preview;             /* right-hand preview visible */
    float preview_ratio;           /* editor's share when preview is open */
    np_preview_kind preview_kind;  /* resolved from the active document */
    np_tex_job tex_job;            /* LaTeX compile / PDF render worker */
    np_http_server web_server;     /* built-in loopback server for HTML */
    cui_texture preview_tex;       /* current PDF page texture */
    int  preview_tex_page;         /* page index currently loaded */
    char preview_owner[NP_PATH_CAP]; /* doc path the preview belongs to */

    /* Android lightweight LaTeX preview (KaTeX in the WebView). */
    uint64_t lite_rev;             /* content_rev rendered last */
    unsigned lite_due;             /* SDL tick when a debounced regen fires */

    /* Immersive ("zen") edit mode: hides the menu bar, toolbar, side panes
     * and status bar so only the tab strip and editor remain.  Panel state
     * is saved here and restored when the mode is left again. */
    bool zen;
    bool zen_prev_sidebar, zen_prev_output, zen_prev_preview;
    float zen_chrome_h, zen_status_h;   /* natural heights for the F11 collapse easing */

    /* Command palette (Ctrl+Shift+P / F1): one searchable list of every
     * menu command plus recent files. */
    bool palette_open;

    /* Embedded WebView2 (Win32, HTML preview only) --------------------- */
    bool wv_init_done;             /* np_webview_init() attempted once */
    bool wv_available;             /* runtime present + controller created */
    char wv_url[512];              /* last URL pushed to the WebView */

    /* Persisted between runs (config file) ----------------------------- */
    char session[NP_SESSION_MAX][NP_PATH_CAP];
    int  session_count;
    int  session_cur[NP_SESSION_MAX];   /* caret offset per session file */
    bool make_backup;               /* keep a .bak copy when saving */
    bool auto_save;                 /* save dirty text docs after 5 idle s */
    bool session_restore;           /* reopen last session at startup */
    bool dark_valid;                /* dark came from the config file */
} notepad;

static notepad g;

/* Forward decls for the unified preview (defined near np_workspace). */
static void np_preview_release_tex_image(notepad *a);

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

/* Enter/leave immersive edit mode; panes come back on exit. */
static void np_zen_set(notepad *a, bool on) {
    if (a->zen == on) return;
    a->zen = on;
    if (on) {
        a->zen_prev_sidebar = a->show_sidebar;
        a->zen_prev_output  = a->show_output;
        a->zen_prev_preview = a->show_preview;
        a->zen_prev_ai      = a->show_ai;
        a->show_sidebar = a->show_output = a->show_preview = false;
        a->show_ai = false;
    } else {
        a->show_sidebar = a->zen_prev_sidebar;
        a->show_output  = a->zen_prev_output;
        a->show_preview = a->zen_prev_preview;
        a->show_ai      = a->zen_prev_ai;
    }
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

/* Binary template helpers (defined near np_hex_pane, used by dialogs earlier). */
static void np_bt_apply(notepad *a, np_doc *d, const char *name, const char *src);
static void np_bt_clear(np_doc *d);
static void np_bt_reparse(notepad *a, np_doc *d);
static bool np_bt_load_file(const char *path, char *src, size_t src_cap,
                            char *name, size_t name_cap);
/* AI assistant helpers (defined with the background workers, used by dialogs). */
static void np_ai_start(notepad *a);
static void np_maclaw_login_start(notepad *a);
static void np_maclaw_qr_paint(cui_ctx *ui, cui_rect r, void *user);
static bool np_ai_apply(notepad *a);
static void np_llm_models_start(notepad *a, int prov_idx);
static void np_llm_test_start(notepad *a, int prov_idx);
static void np_ai_quick(notepad *a, int task);
static size_t np_hexdump_snippet(const unsigned char *p, size_t n,
                                 char *out, size_t cap);
static void np_ai_fix_compile(notepad *a);
static void np_sem_start(notepad *a);
static void np_config_save(notepad *a);
static bool np_smart_paste_insert(notepad *a, const char *text);
static void np_smart_paste_open(notepad *a);
static void np_system_open(notepad *a, const char *path, bool locate);  /* defined below */

/* sidebar tree file-op dialog (state + confirm live with the tree below) */
enum { NP_TREE_OP_NONE = 0, NP_TREE_OP_NEWFILE, NP_TREE_OP_NEWDIR, NP_TREE_OP_RENAME };
static int  np_tree_op;                     /* which file op the dialog performs */
static bool np_tree_op_open;                /* dialog visible */
static char np_tree_op_path[NP_PATH_CAP];   /* target dir (new) or node (rename) */
static char np_tree_op_name[NP_PATH_CAP];   /* name text input */
static void np_tree_confirm(notepad *a);

static bool np_any_dialog(const notepad *a) {
    return a->dlg_confirm || a->dlg_replace || a->dlg_goto || a->dlg_hexgoto ||
           a->dlg_hexfind || np_tree_op_open ||
           a->dlg_settings || a->dlg_help || a->dlg_about || a->dlg_lang || a->dlg_find_files ||
           a->dlg_update || a->dlg_latex_tpl || a->dlg_tpl_online || a->dlg_tinytex ||
           a->dlg_bt || a->dlg_latex_table || a->dlg_latex_tidy || a->dlg_latex_fonts ||
           a->dlg_math_preview || a->dlg_maclaw || a->dlg_reload ||
           a->dlg_llm_prov || a->dlg_ai_actions || a->dlg_smart_paste ||
           a->dlg_searchprof || a->dlg_mcp;
}

/* Compare a release tag ("v1.3.0" / "1.3.0") against NP_APP_VERSION.
 * Unknown shapes (letters beyond the v prefix) fall back to inequality. */
static bool np_update_tag_is_newer(const char *tag) {
    if (!tag || !tag[0]) return false;
    int cur[3] = {0, 0, 0}, lat[3] = {0, 0, 0};
    const char *v = NP_APP_VERSION;
    if (*v == 'v' || *v == 'V') v++;
    if (*tag == 'v' || *tag == 'V') tag++;
    bool cur_ok = sscanf(v, "%d.%d.%d", &cur[0], &cur[1], &cur[2]) >= 1;
    bool lat_ok = sscanf(tag, "%d.%d.%d", &lat[0], &lat[1], &lat[2]) >= 1;
    if (!cur_ok || !lat_ok) return strcmp(v, tag) != 0;
    for (int i = 0; i < 3; i++)
        if (lat[i] != cur[i]) return lat[i] > cur[i];
    return false;
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
    snprintf(title, sizeof title, "%s%s%s%s - RapidEditor",
             d && d->dirty ? "*" : "",
             d && d->path[0] ? np_base(d->path) : "未命名",
             d && d->is_hex ? " [十六进制]" : "",
             d && d->disk_missing ? " [文件已删除]" : "");
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

/* Create a new untitled tab pre-filled with a built-in LaTeX template. The
 * text goes through the same buffer setup as a file load, so the existing
 * "\\documentclass" sniff turns the preview into LaTeX automatically. */
static void np_close_doc(notepad *a, int idx);   /* defined just below */

static np_doc *np_doc_from_latex_template(notepad *a, int index);
static void np_import_latex_zip(notepad *a);   /* defined near the LaTeX project helpers */
static bool np_unpack_and_open(notepad *a, const char *zip, const char *stem);
static void np_start_template_download(notepad *a, const np_online_template *t);
static void np_start_tpl_search(notepad *a);
static void np_start_tinytex_install(notepad *a);

static np_doc *np_doc_from_latex_template(notepad *a, int index) {
    const np_latex_template *t = np_latex_template_at(index);
    if (!t || !t->text) return NULL;
    np_doc *d = np_doc_new(a);
    if (!d) return NULL;
    size_t n = strlen(t->text);
    if (!np_ensure_cap(d, n + 1)) {
        np_close_doc(a, a->count - 1);
        np_toast(a, "内存不足", "无法载入模板", CUI_TONE_DANGER);
        return NULL;
    }
    memcpy(d->text, t->text, n + 1);
    d->len = (int)n;
    d->rev++;
    d->content_rev++;
    d->dirty = true;                       /* a fresh template is unsaved work */
    d->lang = NP_LANG_TEXT;                /* no LaTeX highlighter in CUI */
    d->lang_manual = false;
    d->line = d->column = 1;
    snprintf(d->path, sizeof d->path, "%s", t->filename); /* suggested save name */
    np_text_changed(a, d);
    np_bump_editor(a, d);
    np_logf(a, "从模板新建  %s", t->name);
    return d;
}

static void np_doc_free(np_doc *d) {
    if (!d) return;
    np_bt_result_free(&d->bt_result);
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
    if (d->is_hex) {                    /* binary: word/char counts are meaningless */
        d->chars = d->words = 0;
        d->stats_rev = d->content_rev;
        return;
    }
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

/* mtime of a UTF-8 path, 0 when the file cannot be stat'ed (or was deleted). */
static time_t np_file_mtime(const char *path) {
#ifdef _WIN32
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048) <= 0) return 0;
    struct _stat st;
    return _wstat(w, &st) == 0 ? st.st_mtime : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
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

/* lossy (optional) is set when some characters have no representation in
 * the system ANSI code page and were replaced by the default ('?'). */
static unsigned char *np_utf8_to_ansi(const char *src, size_t n, size_t *out_n,
                                      bool *lossy) {
    if (lossy) *lossy = false;
    int wl = MultiByteToWideChar(CP_UTF8, 0, src, (int)n, NULL, 0);
    if (wl <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)(wl + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, src, (int)n, w, wl);
    int al = WideCharToMultiByte(CP_ACP, 0, w, wl, NULL, 0, NULL, NULL);
    unsigned char *out = (unsigned char *)malloc((size_t)al + 1);
    if (out) {
        BOOL used_default = FALSE;
        WideCharToMultiByte(CP_ACP, 0, w, wl, (char *)out, al, NULL,
                            &used_default);
        if (lossy) *lossy = used_default != FALSE;
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
        np_toast(a, "编码已转换",
                 "文件为 UTF-16，已转为 UTF-8（BOM）显示；保存时将写为 UTF-8",
                 CUI_TONE_INFO);
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
    d->disk_mtime = d->disk_mtime_seen = np_file_mtime(path);
    d->disk_missing = false;
    d->dirty = false;
    d->is_hex = false;
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

/* Heuristic: a NUL byte anywhere in the first 8 KiB, or more than a tenth of
 * control bytes that never appear in text, marks the file as binary. */
static bool np_sniff_binary(const char *path) {
    FILE *f = np_fopen_utf8(path, "rb");
    if (!f) return false;
    unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0) return false;
    int ctrl = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0D && c < 0x20 && c != 0x1B)) ctrl++;
    }
    return ctrl * 10 > (int)n;
}

/* Load a file byte-for-byte for the hex editor: no encoding conversion, no
 * line normalization, no syntax. The buffer stays NUL terminated at len. */
static bool np_load_binary(notepad *a, np_doc *d, const char *path) {
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
    if (!np_ensure_cap(d, to_read + 2)) {
        fclose(f);
        np_toast(a, "内存不足", "无法为该文件分配缓冲区", CUI_TONE_DANGER);
        return false;
    }
    size_t got = fread(d->text, 1, to_read, f);
    fclose(f);
    d->text[got] = 0;
    d->file_size = (size_t)n;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->disk_mtime = d->disk_mtime_seen = np_file_mtime(path);
    d->disk_missing = false;
    d->is_hex = true;
    d->dirty = false;
    d->enc = ENC_UTF8;
    d->crlf = false;
    d->lang = NP_LANG_TEXT;
    d->lang_manual = false;
    d->len = (int)got;
    d->rev++;
    d->content_rev++;
    memset(&d->hexsel, 0, sizeof d->hexsel);
    np_bump_editor(a, d);
    np_add_recent(a, path);
    np_update_title(a);
    {
        char sz[48];
        np_size_str(sz, sizeof sz, (size_t)n);
        np_logf(a, "打开（十六进制）  %s  ·  %s", np_base(path), sz);
    }
    if (truncated)
        np_toast(a, "文件过大，已截断", "单次最多载入 240 MB", CUI_TONE_WARNING);
    else if (!a->quiet)
        np_toast(a, "已按十六进制打开", path, CUI_TONE_SUCCESS);
    return true;
}

/* Defined with the EditPlus-style additions further down. */
static bool np_file_exists(const char *path);
static void np_backup_file(const char *path);
static void np_ff_run(notepad *a);

static bool np_save(notepad *a, np_doc *d, const char *path) {
    size_t n = (size_t)d->len;
    /* EditPlus-style backup: keep the previous content as path.bak. */
    if (a->make_backup && np_file_exists(path)) np_backup_file(path);
    FILE *f = np_fopen_utf8(path, "wb");
    if (!f) {
        np_toast(a, "无法保存文件", path, CUI_TONE_DANGER);
        return false;
    }
    if (d->is_hex) {
        /* Binary documents: write the raw bytes exactly as they are. */
        bool ok = n == 0 || fwrite(d->text, 1, n, f) == n;
        fclose(f);
        if (!ok) {
            np_toast(a, "保存失败", "写入文件时出错", CUI_TONE_DANGER);
            return false;
        }
        bool renamed = strcmp(d->path, path) != 0;
        (void)renamed;
        snprintf(d->path, sizeof d->path, "%s", path);
        d->disk_mtime = d->disk_mtime_seen = np_file_mtime(path);
    d->disk_missing = false;
        d->dirty = false;
        d->file_size = n;
        np_add_recent(a, path);
        np_update_title(a);
        char sz[48];
        np_size_str(sz, sizeof sz, n);
        np_logf(a, "保存（十六进制）  %s  ·  %s", np_base(path), sz);
        np_toast(a, "已保存", path, CUI_TONE_SUCCESS);
        return true;
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
            bool lossy = false;
            converted = np_utf8_to_ansi((const char *)staged, m, &an, &lossy);
            if (converted) { out = converted; out_n = an; }
            if (lossy)
                np_toast(a, "部分字符无法保存",
                         "当前编码为 ANSI，超出系统代码页的字符（如 emoji）"
                         "已被替换为 ?；建议「另存为」UTF-8",
                         CUI_TONE_WARNING);
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
    d->disk_mtime = d->disk_mtime_seen = np_file_mtime(path);
    d->disk_missing = false;
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
    /* "Save to recompile": force the LaTeX preview to rebuild. For multi-file
     * projects, saving a .sty/.cls/.bib/chapter in the same directory as the
     * compiled main file must also trigger a rebuild. */
    if (a->show_preview) {
        bool rebuild = false;
        if (np_preview_detect(path) == NP_PREVIEW_LATEX) rebuild = true;
        else if (a->preview_owner[0] && d->path[0]) {
            const char *dot = strrchr(path, '.');
            if (dot && np_ext_in("cls;sty;bib;bst", dot + 1)) {
                char sd[NP_PATH_CAP], od[NP_PATH_CAP];
                np_dir_of(path, sd, sizeof sd);
                np_dir_of(a->preview_owner, od, sizeof od);
                rebuild = strcmp(sd, od) == 0;
            }
        }
        if (rebuild) {
            np_preview_release_tex_image(a);
            np_tex_job_free(&a->tex_job);
            np_tex_job_init(&a->tex_job);
            a->preview_owner[0] = 0;          /* sync() will start a new compile */
        }
    }
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

/* ---- external file change detection -------------------------------- */

/* Every couple of seconds, stat each open document's file. Clean documents
 * reload silently (with the caret kept), dirty ones warn instead - saving
 * them would overwrite the external changes. */
static void np_disk_check(notepad *a) {
    time_t now = time(NULL);
    if (now - a->disk_check_at < 2) return;
    a->disk_check_at = now;
    for (int i = 0; i < a->count; i++) {
        np_doc *d = a->docs[i];
        if (!d->path[0]) continue;
        time_t mt = np_file_mtime(d->path);
        if (mt == 0) {                  /* deleted or unreadable */
            d->disk_missing = true;
            if (d->disk_mtime != 0 && d->disk_mtime_seen != 0) {
                d->disk_mtime_seen = 0;
                np_toast(a, "文件已从磁盘删除",
                         "保存本文档将以当前内容重建该文件", CUI_TONE_WARNING);
            }
            continue;
        }
        if (d->disk_missing) d->disk_missing = false;   /* file came back */
        if (mt == d->disk_mtime || mt == d->disk_mtime_seen) continue;
        d->disk_mtime_seen = mt;        /* handle each change once */
        if (d->dirty) {
            np_toast(a, "文件已在磁盘上被修改",
                     "当前有未保存修改：直接保存将覆盖外部内容；"
                     "要改用磁盘版本请 Ctrl R 重新加载（放弃本地修改），"
                     "或先「另存为」保留本地内容", CUI_TONE_WARNING);
        } else if (d->is_hex) {
            np_toast(a, "文件已在磁盘上被修改（十六进制视图未刷新）",
                     "Ctrl R 可从磁盘重新加载", CUI_TONE_WARNING);
        } else {
            int c = d->caret, sel = d->anchor;
            /* quiet during the reload: the "loaded" toast below replaces
             * np_load_into's own one */
            bool was_quiet = a->quiet;
            a->quiet = true;
            bool ok = np_load_into(a, d, d->path);
            a->quiet = was_quiet;
            if (ok) {
                c = NP_MAX(0, NP_MIN(c, d->len));
                sel = NP_MAX(0, NP_MIN(sel, d->len));
                cui_code_editor_select(a->ui, d->editor_id, c, sel);
                char msg[NP_PATH_CAP + 48];
                snprintf(msg, sizeof msg, "%s（光标位置已保留）", np_base(d->path));
                np_toast(a, "文件已被外部修改，已重新加载", msg, CUI_TONE_INFO);
            }
        }
    }
}

static bool np_blank_untitled(const np_doc *d) {
    return !d->path[0] && !d->dirty && !d->text[0];
}

/* Idle auto-save for every dirty text document (opt-in). Reuses
 * inline_last_edit (bumped on every edit / caret move) as the idle
 * clock - saving all tabs once the user goes idle, not just the
 * active one. A failed save backs off by the full idle window. Hex
 * docs are excluded - writing binaries back silently is too risky. */
static void np_auto_save_tick(notepad *a) {
    if (!a->auto_save || np_any_dialog(a)) return;
    time_t now = time(NULL);
    if (now - a->inline_last_edit < 5) return;
    int saved = 0, blocked = 0, failed = 0;
    for (int i = 0; i < a->count; i++) {
        np_doc *d = a->docs[i];
        if (!d || d->is_hex || !d->dirty || !d->path[0]) continue;
        /* The disk copy changed while we hold unsaved edits: auto-saving
         * would silently overwrite it, which is exactly what the
         * np_disk_check warning is about. Leave the decision to the user. */
        if (d->disk_mtime_seen != 0 && d->disk_mtime_seen != d->disk_mtime) {
            blocked++;
            continue;
        }
        if (np_save(a, d, d->path)) saved++;
        else failed++;
    }
    if (failed > 0) {
        a->inline_last_edit = now;      /* back off before retrying */
        return;
    }
    if (saved > 0) {
        char msg[64];
        if (blocked > 0)
            snprintf(msg, sizeof msg, "已保存 %d 个文档（%d 个因外部修改跳过）",
                     saved, blocked);
        else
            snprintf(msg, sizeof msg, "已保存 %d 个文档", saved);
        np_toast(a, "自动保存", msg, CUI_TONE_SUCCESS);
    }
}

/* Open a path in a new tab (or switch to the tab that already has it). */
static bool np_open_path(notepad *a, const char *path, bool focus_editor) {
    if (!path || !path[0]) return false;
    char abspath[NP_PATH_CAP];
    np_abs_path(path, abspath, sizeof abspath);
    path = abspath;
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

    if (np_sniff_binary(path)) {
        if (!np_load_binary(a, d, path)) {
            if (fresh) np_close_doc(a, idx);
            return false;
        }
    } else if (!np_load_into(a, d, path)) {
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

/* With auto-save on, a dirty text document that could have been
 * auto-saved anyway is flushed instead of prompting - matching editor
 * conventions where autoSave users are never asked. Documents outside
 * auto-save's reach (untitled, hex, disk changed underneath us) still
 * go through the normal guard. */
static bool np_try_autosave_close(notepad *a, np_doc *d) {
    if (!a->auto_save || !d || d->is_hex || !d->dirty || !d->path[0]) return false;
    if (d->disk_mtime_seen != 0 && d->disk_mtime_seen != d->disk_mtime) return false;
    return np_save(a, d, d->path);
}

static void np_request_close(notepad *a, int idx) {
    np_doc *d = np_doc_at(a, idx);
    if (!d) return;
    if (d->dirty) {
        if (np_try_autosave_close(a, d)) {
            np_close_doc(a, idx);
            return;
        }
        np_confirm(a, ACT_CLOSE_DOC, idx);
    } else {
        np_close_doc(a, idx);
    }
}

/* Reload the current document from disk, keeping the caret/selection.
 * This is the manual counterpart of the automatic reload in
 * np_disk_check: it also covers hex documents and dirty ones (whose
 * local changes are intentionally discarded by the user). */
static void np_do_reload(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || !d->path[0]) return;
    int c = d->caret, sel = d->anchor;
    bool was_quiet = a->quiet;
    a->quiet = true;
    bool ok = d->is_hex ? np_load_binary(a, d, d->path)
                        : np_load_into(a, d, d->path);
    a->quiet = was_quiet;
    if (ok) {
        if (!d->is_hex) {
            c = NP_MAX(0, NP_MIN(c, d->len));
            sel = NP_MAX(0, NP_MIN(sel, d->len));
            cui_code_editor_select(a->ui, d->editor_id, c, sel);
        }
        char msg[NP_PATH_CAP + 48];
        snprintf(msg, sizeof msg, "%s（光标位置已保留）", np_base(d->path));
        np_toast(a, "已从磁盘重新加载", msg, CUI_TONE_INFO);
    } else {
        np_toast(a, "重新加载失败", "文件可能已被删除或移动", CUI_TONE_DANGER);
    }
}

static void np_request_reload(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || !d->path[0]) return;
    if (d->dirty) {
        snprintf(a->confirm_desc, sizeof a->confirm_desc, "%s", np_base(d->path));
        a->dlg_reload = true;
    } else {
        np_do_reload(a);
    }
}

static void np_request_quit(notepad *a) {
    if (a->auto_save) {
        /* flush everything auto-save could have handled on its own */
        for (int i = 0; i < a->count; i++)
            np_try_autosave_close(a, a->docs[i]);
    }
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

/* Word character for whole-word matching (same definition as the editor
 * find bar: ASCII letters, digits, '_'; UTF-8 continuation bytes never
 * count, so non-word queries simply never match at their edges). */
static bool np_word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Whole-word edge check for a match of n bytes at text[i]. ww=false is
 * always true so callers can gate unconditionally. */
static bool np_word_edge(const char *text, size_t len, size_t i, size_t n, bool ww) {
    if (!ww) return true;
    if (i > 0 && np_word_char((unsigned char)text[i - 1])) return false;
    if (i + n < len && np_word_char((unsigned char)text[i + n])) return false;
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
                   + (uint64_t)a->match_case
                   + (a->find_word ? 0x9e3779b9ULL : 0);
    if (d->matches_key == key) return d->match_count;
    size_t n = (size_t)d->len;
    int count = 0;
    for (size_t i = 0; i + qlen <= n;) {
        if (np_match(d->text + i, a->find_query, qlen, a->match_case) &&
            np_word_edge(d->text, n, i, qlen, a->find_word)) {
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
        if (i + qlen <= n && np_match(d->text + i, a->find_query, qlen, a->match_case) &&
            np_word_edge(d->text, n, i, qlen, a->find_word)) {
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
        /* Write through the editor as ONE undo step: a raw buffer copy here
         * would leave CUI's delta history holding stale byte offsets, and
         * the next Ctrl+Z would corrupt unrelated text. Ctrl+Z now reverts
         * the whole replacement. */
        int applied_len = (int)d->len;
        if (!cui_code_editor_replace_external(a->ui, d->editor_id,
                                              d->text, d->cap, d->len,
                                              0, d->len, d->tmp, (int)o,
                                              &applied_len)) {
            np_toast(a, "写入编辑器失败", "替换未生效", CUI_TONE_WARNING);
            return 0;
        }
        d->len = applied_len;
        np_text_changed(a, d);
        int caret = NP_MIN(d->caret, applied_len);
        cui_code_editor_select(a->ui, d->editor_id, caret, caret);
    }
    return count;
}

/* Programmatic replace: defined later, declared here so every insert /
 * transform helper writes through the editor's undo history. */
static void np_splice(notepad *a, np_doc *d, int s, int e,
                      const char *ins, size_t n);

/* Replace the first match at or after the caret (wrapping once to the
 * top), then move the caret past the inserted text so the next call
 * finds the following match. np_splice writes through the editor's undo
 * history, so one Ctrl+Z reverts one replacement. */
static bool np_replace_next(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return false;
    size_t qlen = strlen(a->find_query);
    if (!qlen) return false;
    size_t n = (size_t)d->len;
    if (n < qlen) return false;
    int start = NP_MAX(0, NP_MIN(d->caret, (int)n));
    for (int pass = 0; pass < 2; pass++) {
        int lo = pass == 0 ? start : 0;
        int hi = pass == 0 ? (int)(n - qlen) + 1 : start;
        for (int i = lo; i < hi; i++) {
            if (np_match(d->text + i, a->find_query, qlen, a->match_case) &&
                np_word_edge(d->text, n, (size_t)i, qlen, a->find_word)) {
                np_splice(a, d, i, i + (int)qlen, a->replace_with,
                          strlen(a->replace_with));
                return true;
            }
        }
    }
    return false;
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
    np_splice(a, d, s0, s1, ins, ilen);
}

/* LaTeX paragraph menu: wrap the current selection (or insert a template) with
 * a structural command. `%s` in `fmt` is replaced by the selected text; without
 * a selection the placeholder becomes an empty `{}` and the caret lands inside. */
static void np_latex_wrap(notepad *a, const char *fmt) {
    np_doc *d = np_cur(a);
    if (!d) return;
    int len = d->len;
    int s0 = NP_MIN(d->caret, d->anchor), s1 = NP_MAX(d->caret, d->anchor);
    s0 = NP_MAX(0, NP_MIN(s0, len)); s1 = NP_MAX(s0, NP_MIN(s1, len));
    int sel_len = s1 - s0;
    const char *ph = strstr(fmt, "%s");
    const char *before = fmt;
    char after[128] = {0};
    size_t before_len = ph ? (size_t)(ph - fmt) : strlen(fmt);
    if (ph) snprintf(after, sizeof after, "%s", ph + 2);

    /* Build: before + [selection] + after. Caret returns to right after the
     * inserted body so typing continues inside the command. */
    char buf[1024];
    size_t need = before_len + (size_t)sel_len + strlen(after) + 1;
    if (need > sizeof buf) { np_toast(a, "内容过长", "选区太大，无法套用格式", CUI_TONE_WARNING); return; }
    memcpy(buf, before, before_len);
    if (sel_len) memcpy(buf + before_len, d->text + s0, (size_t)sel_len);
    memcpy(buf + before_len + (size_t)sel_len, after, strlen(after) + 1);

    size_t ilen = strlen(buf);
    if (!np_ensure_cap(d, (size_t)(len - sel_len) + ilen + 1)) {
        np_toast(a, "文本已满", "已达到 256 MB 上限", CUI_TONE_WARNING); return;
    }
    np_splice(a, d, s0, s1, buf, ilen);
    int caret = s0 + (int)before_len + sel_len;
    cui_code_editor_select(a->ui, d->editor_id, caret, caret);
}

/* Insert a whole block (environment, list, table) at the caret. */
static void np_latex_block(notepad *a, const char *text) {
    np_insert_at_caret(a, text);
}

/* Whether the active document is a LaTeX source (by extension or by content
 * for untitled buffers) — drives the visibility of the LaTeX menu. */
static bool np_doc_is_latex(const np_doc *d) {
    if (!d || d->is_hex) return false;
    if (d->path[0]) {
        const char *dot = strrchr(d->path, '.');
        if (dot && np_preview_detect(d->path) == NP_PREVIEW_LATEX) return true;
        if (dot && np_ext_in("cls;sty;bib;bst;aux", dot + 1)) return true;
        return false;
    }
    return d->len > 0 && (strstr(d->text, "\\documentclass") ||
                          strstr(d->text, "\\begin{document}"));
}

/* One LaTeX quick-insert entry. kind 0 = wrap selection with a command using
 * %s as the body placeholder, 1 = insert a literal block at the caret. */
typedef struct {
    const char *label;
    const char *shortcut;
    int  kind;
    const char *text;
} np_latex_snip;

#define NP_LS_WRAP 0
#define NP_LS_BLOCK 1
static const np_latex_snip NP_LATEX_SNIPS[] = {
    { "一级标题 \\section",       NULL, NP_LS_WRAP,  "\\section{%s}" },
    { "二级标题 \\subsection",    NULL, NP_LS_WRAP,  "\\subsection{%s}" },
    { "三级标题 \\subsubsection", NULL, NP_LS_WRAP,  "\\subsubsection{%s}" },
    { "四级标题 \\paragraph",     NULL, NP_LS_WRAP,  "\\paragraph{%s}" },
    { "加粗 \\textbf",     NULL, NP_LS_WRAP, "\\textbf{%s}" },
    { "斜体 \\emph",       NULL, NP_LS_WRAP, "\\emph{%s}" },
    { "下划线 \\underline",NULL, NP_LS_WRAP, "\\underline{%s}" },
    { "等宽 \\texttt",     NULL, NP_LS_WRAP, "\\texttt{%s}" },
    { "行内公式 $...$",    NULL, NP_LS_WRAP, "$%s$" },
    { "引用 \\cite",       NULL, NP_LS_WRAP, "\\cite{%s}" },
    { "交叉引用 \\ref",    NULL, NP_LS_WRAP, "\\ref{%s}" },
    { "标签 \\label",      NULL, NP_LS_WRAP, "\\label{%s}" },
    { "超链接 \\href",     NULL, NP_LS_WRAP, "\\href{https://}{%s}" },
    { "强调式公式块", NULL, NP_LS_BLOCK,
      "\\[\n\t%s\n\\]\n" },
    { "行间公式 equation", NULL, NP_LS_BLOCK,
      "\\begin{equation}\n\t%s\n\\label{eq:}\n\\end{equation}\n" },
    { "多行公式 align", NULL, NP_LS_BLOCK,
      "\\begin{align}\n\t%s &= \\\\\n\t\n\\end{align}\n" },
    { "无序列表 itemize", NULL, NP_LS_BLOCK,
      "\\begin{itemize}\n\t\\item %s\n\\end{itemize}\n" },
    { "有序列表 enumerate", NULL, NP_LS_BLOCK,
      "\\begin{enumerate}\n\t\\item %s\n\\end{enumerate}\n" },
    { "表格 tabular", NULL, NP_LS_BLOCK,
      "\\begin{table}[htbp]\n\t\\centering\n\t\\begin{tabular}{lcr}\n\t\t%s &  & \\\\\n\t\t &  & \\\\\n\t\\end{tabular}\n\t\\caption{}\n\t\\label{tab:}\n\\end{table}\n" },
    { "图片 figure", NULL, NP_LS_BLOCK,
      "\\begin{figure}[htbp]\n\t\\centering\n\t\\includegraphics[width=0.8\\linewidth]{}\n\t\\caption{%s}\n\t\\label{fig:}\n\\end{figure}\n" },
    { "定理 theorem", NULL, NP_LS_BLOCK,
      "\\begin{theorem}\n\t%s\n\\end{theorem}\n" },
    { "证明 proof", NULL, NP_LS_BLOCK,
      "\\begin{proof}\n\t%s\n\\end{proof}\n" },
    { "代码块 verbatim", NULL, NP_LS_BLOCK,
      "\\begin{verbatim}\n%s\n\\end{verbatim}\n" },
    { "算法块 algorithm", NULL, NP_LS_BLOCK,
      "\\begin{algorithm}\n\t\\caption{}\n\t\\begin{algorithmic}[1]\n\t\t%s\n\t\\end{algorithmic}\n\\end{algorithm}\n" },
    { "参考文献段 thebibliography", NULL, NP_LS_BLOCK,
      "\\begin{thebibliography}{99}\n\t\\bibitem{key} %s\n\\end{thebibliography}\n" },
    { "注释 %", NULL, NP_LS_WRAP, "%% %s" },
    { "强制换行 \\\\", NULL, NP_LS_BLOCK, "\\\\\n" },
    { "分页 \\newpage", NULL, NP_LS_BLOCK, "\\newpage\n" },
};
#define NP_LATEX_SNIP_COUNT ((int)(sizeof NP_LATEX_SNIPS / sizeof NP_LATEX_SNIPS[0]))

static void np_apply_latex_snip(notepad *a, int i) {
    if (i < 0 || i >= NP_LATEX_SNIP_COUNT) return;
    const np_latex_snip *s = &NP_LATEX_SNIPS[i];
    if (s->kind == NP_LS_WRAP) np_latex_wrap(a, s->text);
    else np_latex_block(a, s->text);
}

/* Ensure a \usepackage{...} line exists in the document preamble. Scans only
 * the text before \begin{document} (or the first 32 KB), and inserts the
 * package right after \documentclass if missing. Returns true if it inserted
 * something (caller may warn the user that the preamble was touched). */
static bool np_latex_ensure_package(notepad *a, np_doc *d, const char *pkg) {
    if (!d || !pkg || !*pkg || d->is_hex) return false;
    char needle[128];
    snprintf(needle, sizeof needle, "\\usepackage{%s}", pkg);
    /* Only look at the preamble. */
    char *doc = strstr(d->text, "\\begin{document}");
    size_t scan = doc ? (size_t)(doc - d->text) : (size_t)d->len;
    if (scan > 32768) scan = 32768;
    char head[32769];
    memcpy(head, d->text, scan); head[scan] = 0;
    if (strstr(head, needle)) return false;
    /* Also skip the "\usepackage[opt]{pkg}" form. */
    char bracket[160];
    snprintf(bracket, sizeof bracket, "{%s}", pkg);
    if (strstr(head, bracket)) return false;
    /* Insert after the \documentclass{...} line. */
    char *dc = strstr(d->text, "\\documentclass");
    int at;
    if (dc) {
        char *nl = strchr(dc, '\n');
        at = nl ? (int)(nl - d->text + 1) : (int)strlen(d->text);
    } else {
        at = 0;
    }
    char ins[160];
    snprintf(ins, sizeof ins, "%s\n", needle);
    size_t ilen = strlen(ins);
    int len = d->len;
    if (!np_ensure_cap(d, (size_t)len + ilen + 1)) return false;
    np_splice(a, d, at, at, ins, ilen);
    return true;
}

/* Re-parse the table wizard's source text and render a preview. Updates the
 * a->ltable_* preview/dims fields. Returns true when the source is valid. */
static bool np_latex_table_repreview(notepad *a) {
    a->ltable_err[0] = 0;
    a->ltable_preview[0] = 0;
    a->ltable_rows = a->ltable_cols = 0;
    if (!a->ltable_src[0]) return false;
    np_table_parse_opts po;
    memset(&po, 0, sizeof po);
    po.delim = NP_TBL_DELIM_AUTO;
    np_latex_table tbl;
    if (np_latex_table_parse(a->ltable_src, &po, &tbl,
                             a->ltable_err, sizeof a->ltable_err) != 0)
        return false;
    np_table_render_opts ro;
    memset(&ro, 0, sizeof ro);
    ro.booktabs = a->ltable_style == 0;
    ro.first_row_header = a->ltable_header;
    ro.longtable = a->ltable_longtable;
    ro.caption = a->ltable_caption;
    ro.label = a->ltable_label;
    ro.align = a->ltable_align;
    ro.width = a->ltable_resize ? a->ltable_width : 0.0f;
    int n = np_latex_table_render(&tbl, &ro, a->ltable_preview, sizeof a->ltable_preview);
    a->ltable_rows = tbl.rows;
    a->ltable_cols = tbl.cols;
    np_latex_table_free(&tbl);
    if (n < 0) {
        snprintf(a->ltable_err, sizeof a->ltable_err, "表格过大，预览缓冲区不足");
        a->ltable_preview[0] = 0;
        return false;
    }
    return true;
}

/* Insert the rendered table at the caret and add required packages. */
static bool np_latex_table_insert(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d) return false;
    if (!np_latex_table_repreview(a)) return false;
    if (!a->ltable_preview[0]) return false;
    bool needs_bt = a->ltable_style == 0;
    bool needs_lt = a->ltable_longtable;
    bool needs_gfx = a->ltable_resize;
    np_insert_at_caret(a, a->ltable_preview);
    if (needs_bt && np_latex_ensure_package(a, d, "booktabs"))
        np_toast(a, "已添加宏包", "导言区已加入 \\usepackage{booktabs}", CUI_TONE_INFO);
    if (needs_lt && np_latex_ensure_package(a, d, "longtable"))
        np_toast(a, "已添加宏包", "导言区已加入 \\usepackage{longtable}", CUI_TONE_INFO);
    if (needs_gfx && np_latex_ensure_package(a, d, "graphicx"))
        np_toast(a, "已添加宏包", "导言区已加入 \\usepackage{graphicx}（resizebox 需要）", CUI_TONE_INFO);
    return true;
}

/* Apply source tidying to the whole current document. */
static bool np_latex_tidy_apply(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex || d->len <= 0) return false;
    np_tidy_opts o;
    memset(&o, 0, sizeof o);
    o.normalize_indent = a->tidy_indent;
    o.join_hard_wraps = a->tidy_join;
    o.trim_trailing_ws = a->tidy_trim;
    o.indent_spaces = a->tidy_spaces;
    char *tidied = NULL;
    if (np_latex_tidy(d->text, (size_t)d->len, &o, &tidied) != 0 || !tidied) {
        np_toast(a, "整理失败", "内存不足", CUI_TONE_WARNING);
        return false;
    }
    size_t nlen = strlen(tidied);
    int len = d->len;
    if (nlen >= (size_t)len) {
        if (!np_ensure_cap(d, nlen + 1)) { free(tidied); np_toast(a, "文本已满", "无法整理", CUI_TONE_WARNING); return false; }
    }
    int applied_len = (int)d->len;
    bool written = cui_code_editor_replace_external(a->ui, d->editor_id,
                                                    d->text, d->cap, d->len,
                                                    0, d->len, tidied, (int)nlen,
                                                    &applied_len);
    free(tidied);
    if (!written) {
        np_toast(a, "文本已满", "无法写入整理结果", CUI_TONE_WARNING);
        return false;
    }
    d->len = applied_len;
    np_text_changed(a, d);
    if (d->caret > d->len) d->caret = d->len;
    if (d->anchor > d->len) d->anchor = d->len;
    cui_code_editor_select(a->ui, d->editor_id, d->caret, d->caret);
    return true;
}

/* Insert one line just before \begin{document} (preamble), or at the very
 * start for untitled buffers. Used by the font setup command. */
static void np_latex_preamble_line(notepad *a, np_doc *d, const char *line) {
    if (!d || !line || !*line) return;
    char *doc = strstr(d->text, "\\begin{document}");
    int at = doc ? (int)(doc - d->text) : 0;
    /* Back up over the indentation/blank line right before \begin{document}
     * so our line is not glued to an existing indented doc start. */
    size_t ilen = strlen(line);
    int len = d->len;
    if (!np_ensure_cap(d, (size_t)len + ilen + 2)) return;
    np_splice(a, d, at, at, line, ilen);
}

/* Insert fontspec / xeCJK setup inferred from the font picker. */
static bool np_latex_fonts_apply(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return false;
    if (!a->font_latin_name[0] && !a->font_cjk_name[0]) {
        np_toast(a, "未选择字体", "请至少选择一种字体", CUI_TONE_WARNING);
        return false;
    }
    /* The document must be compiled with xelatex for fontspec/xeCJK. */
    np_latex_ensure_package(a, d, "fontspec");
    if (a->font_latin_name[0]) {
        char line[288];
        snprintf(line, sizeof line, "\\setmainfont{%s}\n", a->font_latin_name);
        np_latex_preamble_line(a, d, line);
    }
    if (a->font_cjk_name[0]) {
        np_latex_ensure_package(a, d, "xeCJK");
        char line[288];
        snprintf(line, sizeof line, "\\setCJKmainfont{%s}\n", a->font_cjk_name);
        np_latex_preamble_line(a, d, line);
    }
    np_toast(a, "字体已写入", "导言区已加入 fontspec/xeCJK 设置；请用 XeLaTeX 编译", CUI_TONE_INFO);
    return true;
}

/* Extract the math expression under the caret (the nearest $...$ / $$...$$
 * span enclosing or right before the caret) into a->math_expr. Returns true
 * when something was found. Used to seed the live preview from the editor. */
static bool np_math_expr_from_caret(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex || d->len <= 1) return false;
    int pos = NP_MAX(0, NP_MIN(d->caret, d->len - 1));
    /* Search backwards for an unescaped '$'. */
    int open = -1;
    for (int i = pos; i >= 0; i--) {
        if (d->text[i] != '$') continue;
        int bs = 0;
        for (int k = i - 1; k >= 0 && d->text[k] == '\\'; k--) bs++;
        if (bs % 2 == 0) { open = i; break; }
    }
    if (open < 0) return false;
    bool display = (open + 1 < d->len && d->text[open + 1] == '$');
    int start = open + (display ? 2 : 1);
    /* Search forward for the matching delimiter. */
    char *end = NULL;
    if (display) end = strstr(d->text + start, "$$");
    else {
        for (int i = start; i < d->len; i++) {
            if (d->text[i] != '$') continue;
            int bs = 0;
            for (int k = i - 1; k >= 0 && d->text[k] == '\\'; k--) bs++;
            if (bs % 2 == 0) { end = d->text + i; break; }
        }
    }
    int stop = end ? (int)(end - d->text) : d->len;
    if (stop <= start) return false;
    size_t n = (size_t)(stop - start);
    if (n >= sizeof a->math_expr) n = sizeof a->math_expr - 1;
    memcpy(a->math_expr, d->text + start, n);
    a->math_expr[n] = 0;
    return true;
}

/* Wrap the current preview expression as $...$ and insert at the caret. */
static void np_math_expr_insert(notepad *a) {
    if (!a->math_expr[0]) return;
    char buf[1100];
    snprintf(buf, sizeof buf, "$%s$", a->math_expr);
    np_insert_at_caret(a, buf);
}

/* ------------------------------------------------------------------ */
/* Markdown paragraph menu                                             */
/* ------------------------------------------------------------------ */

/* Whether the active document is Markdown (by extension, or by content
 * for untitled buffers) — drives the visibility of the 段落 menu. */
static bool np_doc_is_markdown(const np_doc *d) {
    if (!d || d->is_hex) return false;
    if (d->path[0]) return np_preview_detect(d->path) == NP_PREVIEW_MARKDOWN;
    return d->len > 0 && (strstr(d->text, "# ") || strstr(d->text, "## ") ||
                          strstr(d->text, "- ") || strstr(d->text, "```"));
}

/* Byte offset of the start of the line containing `pos`. */
static int np_line_start(const np_doc *d, int pos) {
    int p = NP_MAX(0, NP_MIN(pos, d->len));
    while (p > 0 && d->text[p - 1] != '\n') p--;
    return p;
}

/* Byte offset just past the end of the line containing `pos` (before '\n'). */
static int np_line_end(const np_doc *d, int pos) {
    int p = NP_MAX(0, NP_MIN(pos, d->len));
    while (p < d->len && d->text[p] != '\n') p++;
    return p;
}

/* Heading level (0..6) of the caret's line, for check marks in the menu. */
static int np_md_line_hlevel(const np_doc *d) {
    if (!d || d->is_hex) return 0;
    int p = NP_MAX(0, NP_MIN(NP_MIN(d->caret, d->anchor), d->len));
    int ls = np_line_start(d, p), le = np_line_end(d, p);
    int h = 0;
    while (ls + h < le && d->text[ls + h] == '#' && h < 6) h++;
    return (h > 0 && ls + h < le && d->text[ls + h] == ' ') ? h : 0;
}

/* Set the heading level of the caret's line (0 = plain paragraph). Any
 * existing leading "#{1,6} " marker is replaced. */
static void np_md_set_heading(notepad *a, int level) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    int p = NP_MAX(0, NP_MIN(NP_MIN(d->caret, d->anchor), d->len));
    int ls = np_line_start(d, p), le = np_line_end(d, p);
    int i = ls, h = 0;
    while (i < le && d->text[i] == '#' && h < 7) { i++; h++; }
    if (h >= 1 && h <= 6 && i < le && d->text[i] == ' ') i++;
    else i = ls;                       /* not a heading: keep the whole line */
    char pre[9]; int pn = 0;
    for (int k = 0; k < level; k++) pre[pn++] = '#';
    if (level > 0) pre[pn++] = ' ';
    pre[pn] = 0;
    size_t ilen = (size_t)pn;
    int removed = i - ls;
    if (!np_ensure_cap(d, (size_t)(d->len - removed) + ilen + 1)) return;
    np_splice(a, d, ls, i, pre, ilen);
    int off = d->caret - i; if (off < 0) off = 0;
    int caret = ls + (int)ilen + off;
    caret = NP_MAX(ls, NP_MIN(caret, d->len));
    cui_code_editor_select(a->ui, d->editor_id, caret, caret);
}

/* Raise/lower the caret line's heading level by `delta`, clamped to 0..6. */
static void np_md_adjust_heading(notepad *a, int delta) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    int p = NP_MAX(0, NP_MIN(NP_MIN(d->caret, d->anchor), d->len));
    int ls = np_line_start(d, p), le = np_line_end(d, p);
    int h = 0;
    while (ls + h < le && d->text[ls + h] == '#' && h < 6) h++;
    int level = (h > 0 && ls + h < le && d->text[ls + h] == ' ') ? h : 0;
    int nl = NP_MAX(0, NP_MIN(6, level + delta));
    np_md_set_heading(a, nl);
}

/* Indent (+) or outdent (-) every line touched by the selection by 2 spaces. */
static void np_md_indent(notepad *a, int dir) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    int c0 = NP_MIN(d->caret, d->anchor), c1 = NP_MAX(d->caret, d->anchor);
    int ls = np_line_start(d, c0), le = np_line_end(d, c1);
    int n = le - ls;
    if (n <= 0) return;
    char *out = (char *)malloc((size_t)n * 2 + 8);
    if (!out) return;
    int o = 0, line = ls, caret_new = 0;
    while (1) {
        int end = line;
        while (end < le && d->text[end] != '\n') end++;
        int content_start = line;
        if (dir > 0) { out[o++] = ' '; out[o++] = ' '; }
        else {
            int k = line;
            while (k < end && k - line < 2 && d->text[k] == ' ') k++;
            content_start = k;
        }
        int copied_at = o;
        memcpy(out + o, d->text + content_start, (size_t)(end - content_start));
        o += end - content_start;
        if (c0 >= line && c0 <= end) {
            int rel = c0 - content_start;
            if (rel < 0) rel = 0;
            if (rel > end - content_start) rel = end - content_start;
            caret_new = copied_at + rel;
        }
        if (end >= le) break;
        out[o++] = '\n';
        line = end + 1;
    }
    out[o] = 0;
    size_t ilen = (size_t)o;
    if (!np_ensure_cap(d, (size_t)(d->len - n) + ilen + 1)) { free(out); return; }
    np_splice(a, d, ls, le, out, ilen);
    int caret = NP_MAX(ls, NP_MIN(ls + caret_new, d->len));
    cui_code_editor_select(a->ui, d->editor_id, caret, caret);
    free(out);
}

/* Toggle a Markdown task checkbox on the caret's line. If the line is a
 * plain "- item", it becomes "- [ ] item" first. */
static void np_md_toggle_task(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    int p = NP_MAX(0, NP_MIN(NP_MIN(d->caret, d->anchor), d->len));
    int ls = np_line_start(d, p), le = np_line_end(d, p);
    for (int i = ls; i + 2 < le; i++) {
        if (d->text[i] == '[' && d->text[i + 1] == ' ' && d->text[i + 2] == ']') {
            np_splice(a, d, i + 1, i + 2, "x", 1); return;
        }
        if (d->text[i] == '[' && (d->text[i + 1] == 'x' || d->text[i + 1] == 'X') &&
            d->text[i + 2] == ']') {
            np_splice(a, d, i + 1, i + 2, " ", 1); return;
        }
    }
    int i = ls;
    while (i < le && d->text[i] == ' ') i++;
    if (i < le && (d->text[i] == '-' || d->text[i] == '*' || d->text[i] == '+') &&
        i + 1 < le && d->text[i + 1] == ' ') {
        np_splice(a, d, i + 2, i + 2, "[ ] ", 4);
    }
}

/* Insert a blank paragraph above or below the caret's line. */
static void np_md_insert_para(notepad *a, bool above) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    int p = NP_MAX(0, NP_MIN(NP_MIN(d->caret, d->anchor), d->len));
    int at = above ? np_line_start(d, p) : np_line_end(d, p);
    if (!np_ensure_cap(d, (size_t)d->len + 1 + 1)) return;
    np_splice(a, d, at, at, "\n", 1);
    int caret = above ? at : at + 1;
    cui_code_editor_select(a->ui, d->editor_id, caret, caret);
}

/* One Markdown quick-insert entry. `%s` is the selection / caret placeholder.
 * `block` items start on a fresh line so tables/lists never land mid-line. */
typedef struct {
    const char *label;
    const char *shortcut;
    const char *fmt;
    bool block;
} np_md_snip;

static const np_md_snip NP_MD_SNIPS[] = {
    /* inline formatting */
    { "加粗",          "Ctrl Alt B",  "**%s**",        false },
    { "斜体",          "Ctrl Alt I",  "*%s*",          false },
    { "删除线",        NULL,          "~~%s~~",        false },
    { "行内代码",      NULL,          "`%s`",          false },
    { "高亮",          NULL,          "==%s==",        false },
    { "链接",          NULL,          "[%s](https://)", false },
    { "脚注引用",      NULL,          "[^%s]",         false },
    { "图片",          NULL,          "![%s](https://)", false },
    /* blocks */
    { "公式块",        "Ctrl Shift M", "$$\n%s\n$$\n",  true },
    { "代码块",        "Ctrl Shift K", "```\n%s\n```\n", true },
    { "引用",          "Ctrl Shift Q", "> %s\n",        true },
    { "表格",          "Ctrl Shift T",
      "| 列 1 | 列 2 | 列 3 |\n| :--- | :---: | ---: |\n| %s | 内容 | 内容 |\n| 内容 | 内容 | 内容 |\n", true },
    { "无序列表",      "Ctrl Alt 8",  "- %s\n",         true },
    { "有序列表",      "Ctrl Alt 9",  "1. %s\n",        true },
    { "任务列表",      "Ctrl Shift X", "- [ ] %s\n",     true },
    { "水平分割线",    NULL,          "---\n%s",        true },
    { "内容目录",      NULL,          "[TOC]\n%s",      true },
    { "YAML Front Matter", NULL,      "---\ntitle: %s\n---\n", true },
    { "脚注定义",      NULL,          "[^%s]: 脚注内容\n", true },
    /* GitHub alert callouts */
    { "提示 NOTE",     NULL,          "> [!NOTE]\n> %s\n",    true },
    { "技巧 TIP",      NULL,          "> [!TIP]\n> %s\n",     true },
    { "重要 IMPORTANT",NULL,          "> [!IMPORTANT]\n> %s\n", true },
    { "警告 WARNING",  NULL,          "> [!WARNING]\n> %s\n",  true },
    { "注意 CAUTION",  NULL,          "> [!CAUTION]\n> %s\n",  true },
};
#define NP_MD_SNIP_COUNT ((int)(sizeof NP_MD_SNIPS / sizeof NP_MD_SNIPS[0]))

/* Named lookup so menu shortcuts can call a snippet by label. */
static int np_md_snip_index(const char *label) {
    for (int i = 0; i < NP_MD_SNIP_COUNT; i++)
        if (!strcmp(NP_MD_SNIPS[i].label, label)) return i;
    return -1;
}

static void np_apply_md_snip(notepad *a, int i) {
    if (i < 0 || i >= NP_MD_SNIP_COUNT) return;
    const np_md_snip *s = &NP_MD_SNIPS[i];
    np_doc *d = np_cur(a);
    if (s->block && d && d->caret == d->anchor) {
        /* Only break the line when there is no selection to preserve. */
        int p = NP_MAX(0, NP_MIN(d->caret, d->len));
        if (p > np_line_start(d, p)) np_insert_at_caret(a, "\n");
    }
    np_latex_wrap(a, s->fmt);
}

static void np_open_find(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d) return;
    if (d->is_hex) {
        /* The hex view has no find bar of its own: Ctrl F opens the byte
         * search dialog instead (was a silently dead shortcut). */
        a->dlg_hexfind = true;
        return;
    }
    /* Seed the query with the current selection (capped), like most
     * editors do - Ctrl F right after selecting a word just works. */
    if (d->caret != d->anchor) {
        int s0 = NP_MIN(d->caret, d->anchor), s1 = NP_MAX(d->caret, d->anchor);
        int n = NP_MIN(s1 - s0, (int)sizeof a->find_query - 1);
        if (n > 0 && n < 128 && !memchr(d->text + s0, '\n', (size_t)n)) {
            memcpy(a->find_query, d->text + s0, (size_t)n);
            a->find_query[n] = 0;
        }
    }
    /* The find bar follows the dialog's match settings, so both find
     * surfaces agree on what a match is. */
    cui_code_editor_find_opts(a->ui, d->editor_id,
                              a->find_query[0] ? a->find_query : "",
                              a->match_case, a->find_word);
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
/* Hex editor helpers (010 Editor style)                               */
/* ------------------------------------------------------------------ */

/* "1F", "1f2e", "0x1F2E" — hex, like 010 Editor. Leading/trailing
 * whitespace is ignored. */
static bool np_hex_parse_offset(const char *s, size_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (!n) return false;
    if (n >= 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) { s += 2; n -= 2; }
    unsigned long long v = 0;
    for (size_t i = 0; i < n; i++) {
        int c;
        char ch = s[i];
        if (ch >= '0' && ch <= '9') c = ch - '0';
        else if (ch >= 'a' && ch <= 'f') c = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') c = ch - 'A' + 10;
        else return false;
        if (v > (0xFFFFFFFFFFFFFFFFULL - c) / 16) return false;   /* overflow */
        v = v * 16 + (unsigned)c;
    }
    if (!out) return false;
    *out = (size_t)v;
    return true;
}

static void np_hex_do_goto(notepad *a) {
    np_doc *d = np_cur(a);
    if (!d || !d->is_hex) return;
    size_t off;
    if (!np_hex_parse_offset(a->hexgoto_buf, &off)) {
        np_toast(a, "偏移格式不对", "请输入十六进制，如 1A2B 或 0x1A2B", CUI_TONE_WARNING);
        return;
    }
    if (d->len > 0 && off >= (size_t)d->len) {
        off = (size_t)d->len - 1;
        np_toast(a, "偏移超出文件末尾", "已跳到最后一个字节", CUI_TONE_INFO);
    }
    if (a->ui) cui_hex_view_goto(a->ui, d->editor_id, off);
    a->dlg_hexgoto = false;
}

/* Parse the hex-find query. ASCII mode takes the whole string (trimmed) as
 * the bytes to find; hex mode accepts "1F 2E CC", "1F2ECC" or "0x1F2ECC".
 * Returns false on an odd digit count or an empty query. */
static bool np_hex_parse_bytes(const char *s, bool ascii, unsigned char *out, int cap, int *out_n) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (ascii) {
        if (n == 0 || (int)n > cap) return false;
        memcpy(out, s, n);
        *out_n = (int)n;
        return true;
    }
    if (n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; n -= 2; }
    int val = -1, cnt = 0;              /* val: pending high nibble */
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int d;
        if (c == ' ' || c == '\t' || c == ',') continue;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (val < 0) {
            val = d;
        } else {
            if (cnt >= cap) return false;
            out[cnt++] = (unsigned char)((val << 4) | d);
            val = -1;
        }
    }
    if (val >= 0) return false;         /* odd digit count */
    if (cnt == 0) return false;
    *out_n = cnt;
    return true;
}

/* Run the hex find: scan the buffer for the parsed query, jumping to and
 * selecting the hit. next continues after the last hit and wraps once. */
static void np_hex_find_run(notepad *a, bool next) {
    np_doc *d = np_cur(a);
    if (!d || !d->is_hex) return;

    /* H4 结构化查询：解析查询 → 本地扫描命中 → 复用 hexfind 命中刻度与跳转。 */
    if (a->hexfind_struct) {
        np_bi_query_t q;
        char perr[128];
        if (!np_bi_query_parse(a->hexfind_buf, &q, perr, sizeof perr)) {
            np_toast(a, "查询格式不对", perr, CUI_TONE_WARNING);
            return;
        }
        const unsigned char *buf = (const unsigned char *)d->text;
        size_t n = (size_t)d->len;
        /* 全量扫描到局部数组（上限 512 与滚动条刻度一致）；total 用于提示。 */
        np_bi_hit_t hbuf[NP_BI_MAX_HITS];
        int total = 0;
        int found = np_bi_search(buf, n, &q, hbuf, NP_BI_MAX_HITS, &total);
        if (found <= 0) {
            a->hexfind_at = (size_t)-1; a->hexfind_len = 0; a->hexfind_hit_count = 0;
            np_toast(a, "未找到", "没有符合结构化条件的字节", CUI_TONE_INFO);
            return;
        }
        /* 写回滚动条 tick（k 上限 400） */
        int k = 0;
        for (int i = 0; i < found && k < (int)(sizeof a->hexfind_hits / sizeof a->hexfind_hits[0]); i++)
            a->hexfind_hits[k++] = hbuf[i].offset;
        a->hexfind_hit_count = k;
        a->hexfind_doc_uid = d->uid;
        /* 选择命中的字节（next 从上次偏移之后取，否则取第一个） */
        int pick = 0;
        if (next && a->hexfind_at != (size_t)-1) {
            for (int i = 0; i < found; i++)
                if (hbuf[i].offset > a->hexfind_at) { pick = i; break; }
        }
        a->hexfind_at = hbuf[pick].offset;
        a->hexfind_len = hbuf[pick].length;
        if (a->ui) cui_hex_view_goto(a->ui, d->editor_id, a->hexfind_at);
        char msg[128];
        snprintf(msg, sizeof msg, "%s · 0x%X 命中 %zu 字节 · 共 %d 处%s",
                 q.desc, (unsigned)a->hexfind_at, a->hexfind_len, total,
                 total > found ? "（刻度已截断）" : "");
        np_toast(a, next ? "下一个命中" : "已找到", msg, CUI_TONE_SUCCESS);
        a->dlg_hexfind = false;
        return;
    }

    unsigned char pat[256];
    int pn = 0;
    if (!np_hex_parse_bytes(a->hexfind_buf, a->hexfind_ascii, pat,
                            (int)sizeof pat, &pn)) {
        np_toast(a, "查询格式不对",
                 a->hexfind_ascii ? "请输入要查找的文本"
                                  : "请输入十六进制字节，如 1F 2E CC",
                 CUI_TONE_WARNING);
        return;
    }
    const char *t = d->text;
    size_t n = (size_t)d->len;
    if (pn <= 0 || n < (size_t)pn) {
        np_toast(a, "未找到", "文件中没有匹配的字节序列", CUI_TONE_INFO);
        return;
    }
    if (a->hexfind_at == (size_t)-1 || a->hexfind_at >= n) next = false;
    size_t from = next ? a->hexfind_at + (a->hexfind_len ? a->hexfind_len : 1) : 0;
    for (int pass = 0; pass < 2; pass++) {
        size_t lo = pass == 0 ? from : 0;
        size_t hi = pass == 0 ? n - (size_t)pn + 1 : from;
        for (size_t i = lo; i < hi; i++) {
            if (t[i] != (char)pat[0]) continue;   /* cheap first-byte gate */
            if (memcmp(t + i, pat, (size_t)pn) == 0) {
                /* count every match and which one this is, so the toast
                 * reads "2 of 7" like the text find bar does; also keep
                 * them all for the scrollbar ticks (capped) */
                int total = 0, index = 0, kept = 0;
                for (size_t j = 0; j + (size_t)pn <= n; j++) {
                    if (t[j] != (char)pat[0]) continue;
                    if (memcmp(t + j, pat, (size_t)pn) == 0) {
                        total++;
                        if (j == i) index = total;
                        if (kept < (int)(sizeof a->hexfind_hits / sizeof a->hexfind_hits[0]))
                            a->hexfind_hits[kept++] = j;
                    }
                }
                a->hexfind_hit_count = kept;
                a->hexfind_doc_uid = d->uid;
                a->hexfind_at = i;
                a->hexfind_len = (size_t)pn;
                if (a->ui) cui_hex_view_goto(a->ui, d->editor_id, i);
                char msg[96];
                snprintf(msg, sizeof msg, "%d of %d · 0x%X 处命中 %d 字节",
                         index, total, (unsigned)i, pn);
                np_toast(a, next ? "下一个命中" : "已找到", msg, CUI_TONE_SUCCESS);
                a->dlg_hexfind = false;
                return;
            }
        }
        if (pass == 0 && !next) break;    /* a fresh search never wraps */
    }
    a->hexfind_at = (size_t)-1;
    a->hexfind_len = 0;
    a->hexfind_hit_count = 0;
    np_toast(a, "未找到", "文件中没有匹配的字节序列", CUI_TONE_INFO);
}

/* Copy the hex selection to the clipboard: as "AA BB CC" or as ASCII with
 * dots for the bytes that cannot be printed. */
static void np_hex_copy(notepad *a, np_doc *d, bool ascii) {
    if (!a->ui) return;
    size_t start = d->hexsel.start, len = d->hexsel.length;
    if (start >= (size_t)d->len) len = 0;
    if (start + len > (size_t)d->len) len = (size_t)d->len - start;
    if (!len) {
        np_toast(a, "没有选中的字节", "先在十六进制视图中点击或拖选", CUI_TONE_INFO);
        return;
    }
    char *buf = (char *)malloc(len * 4 + 4);
    if (!buf) return;
    size_t o = 0;
    if (ascii) {
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)d->text[start + i];
            buf[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
    } else {
        for (size_t i = 0; i < len; i++)
            o += (size_t)snprintf(buf + o, len * 4 + 4 - o, "%02X%s",
                                  (unsigned char)d->text[start + i],
                                  i + 1 < len ? " " : "");
    }
    buf[o] = 0;
    cui_set_clipboard_text(a->ui, buf);
    free(buf);
    char msg[96];
    snprintf(msg, sizeof msg, "%zu 字节已复制为%s", len, ascii ? "ASCII" : "十六进制");
    np_toast(a, "复制成功", msg, CUI_TONE_SUCCESS);
}

/* Switch the current document between the text editor and the hex editor by
 * reloading it from disk. Refuses unsaved changes so nothing is lost. */
static void np_hex_reopen(notepad *a, bool as_hex) {
    np_doc *d = np_cur(a);
    if (!d) return;
    if (!d->path[0]) {
        np_toast(a, "尚未保存到文件", "只有已保存的文件能在两种模式间切换", CUI_TONE_WARNING);
        return;
    }
    if (d->dirty) {
        np_toast(a, "有未保存的修改", "请先保存（Ctrl S）再切换编辑模式", CUI_TONE_WARNING);
        return;
    }
    if (as_hex == d->is_hex) return;
    bool ok = as_hex ? np_load_binary(a, d, d->path) : np_load_into(a, d, d->path);
    if (ok) np_toast(a, as_hex ? "已切换到十六进制编辑" : "已切换到文本编辑",
                     d->path, CUI_TONE_SUCCESS);
}

/* Open dialog that forces hex mode (the normal Open dialog auto-detects). */
static void np_open_hex_dialog(notepad *a) {
    char p[NP_PATH_CAP];
    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
            .kind = CUI_FILE_OPEN, .title = "以十六进制方式打开文件",
            .filter_name = "所有文件", .filter = "*" },
            p, sizeof p) && np_open_path(a, p, true))
        np_hex_reopen(a, true);
}

/* ------------------------------------------------------------------ */
/* Editor fonts: bundled open-source faces + user font + CJK fallback   */
/* ------------------------------------------------------------------ */

/* Cache a CJK font so it can be re-attached to whichever font currently
 * occupies the mono role: fallbacks live on the font object, not the role,
 * so cui_font_set_role() drops them and we must add them back each switch. */
static void np_font_attach_cjk(cui_ctx *ui, notepad *a, cui_font font) {
    if (a->font_cjk >= 0 && font >= 0)
        cui_font_add_fallback(ui, font, a->font_cjk);
}

/* Apply a face to the editor (mono) role and keep the CJK fallback intact. */
static void np_font_set_active(cui_ctx *ui, notepad *a, cui_font font) {
    if (font < 0) return;
    a->font_active = font;
    cui_font_set_role(ui, CUI_FONT_MONO, font);
    np_font_attach_cjk(ui, a, font);
}

/* Restore CUI's built-in monospace face (Geist Mono). */
static void np_font_reset(cui_ctx *ui, notepad *a) {
    if (a->font_default_mono < 0) return;
    np_font_set_active(ui, a, a->font_default_mono);
    snprintf(a->font_ident, sizeof a->font_ident, "default");
}

/* Apply one of the bundled fonts by index. */
static bool np_font_apply_bundled(cui_ctx *ui, notepad *a, int idx) {
    if (idx < 0 || idx >= np_bundled_font_count) return false;
    const np_bundled_font *bf = &np_bundled_fonts[idx];
    cui_font f = cui_font_load_deflated(ui, bf->data, bf->packed_size, bf->raw_size);
    if (f < 0) {
        np_logf(a, "字体加载失败  ·  %s", bf->label);
        return false;
    }
    np_font_set_active(ui, a, f);
    snprintf(a->font_ident, sizeof a->font_ident, "%s", bf->ident);
    np_logf(a, "编辑器字体  ·  %s", bf->label);
    return true;
}

/* Apply a user-picked TrueType/OpenType file. */
static bool np_font_apply_file(cui_ctx *ui, notepad *a, const char *path) {
    if (!path || !*path) return false;
    cui_font f = cui_font_load_file(ui, path);
    if (f < 0) {
        np_toast(a, "无法加载字体", "不是有效的 TrueType / OpenType 文件", CUI_TONE_WARNING);
        np_logf(a, "字体加载失败  ·  %s", np_base(path));
        return false;
    }
    np_font_set_active(ui, a, f);
    snprintf(a->font_ident, sizeof a->font_ident, "custom");
    snprintf(a->font_custom_path, sizeof a->font_custom_path, "%s", path);
    np_toast(a, "编辑器字体已更新", np_base(path), CUI_TONE_SUCCESS);
    np_logf(a, "编辑器字体（自定义）  ·  %s", np_base(path));
    return true;
}

/* Human-readable name of the active face, for menus and the status bar. */
static const char *np_font_label(const notepad *a) {
    if (!strcmp(a->font_ident, "default")) return "默认 (Geist Mono)";
    if (!strcmp(a->font_ident, "custom"))
        return a->font_custom_path[0] ? np_base(a->font_custom_path) : "自定义";
    for (int i = 0; i < np_bundled_font_count; i++)
        if (!strcmp(a->font_ident, np_bundled_fonts[i].ident))
            return np_bundled_fonts[i].label;
    return a->font_ident;
}

/* Restore the face selected in config.ini at startup. */
static void np_font_restore(cui_ctx *ui, notepad *a) {
    if (!strcmp(a->font_ident, "default") || !a->font_ident[0]) {
        np_font_reset(ui, a);
        return;
    }
    if (!strcmp(a->font_ident, "custom")) {
        if (a->font_custom_path[0] && np_font_apply_file(ui, a, a->font_custom_path)) return;
        np_font_reset(ui, a);   /* custom file gone -> fall back to default */
        return;
    }
    for (int i = 0; i < np_bundled_font_count; i++)
        if (!strcmp(a->font_ident, np_bundled_fonts[i].ident))
            if (np_font_apply_bundled(ui, a, i)) return;
    np_font_reset(ui, a);
}

static void np_load_fonts(cui_ctx *ui, notepad *a) {
    a->fonts_ready = true;

    /* Remember the built-in mono font before we ever replace its role. */
    a->font_default_mono = cui_font_role_font(ui, CUI_FONT_MONO);
    a->font_active = a->font_default_mono;
    a->font_cjk = -1;

    /* CUI's embedded fonts cover Latin; add one system CJK face as fallback
     * so Chinese/kanji text renders in every role, including our editor font. */
    static const char *paths[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/system/fonts/NotoSansCJK-Regular.ttc",      /* Android */
        "/system/fonts/NotoSerifCJK-Regular.ttc",     /* Android variant */
        "/system/fonts/DroidSansFallback.ttf"         /* older Android */
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        cui_font f = cui_font_load_file(ui, paths[i]);
        if (f < 0) continue;
        a->font_cjk = f;
        for (int role = CUI_FONT_BODY; role <= CUI_FONT_MONO; role++)
            cui_font_add_fallback(ui, (cui_font)role, f);
        break;
    }

    /* Apply the persisted choice (or the default). */
    np_font_restore(ui, a);
}

/* Apply an appearance style preset in one shot. Uses CUI's animated theme
 * setters so a switch cross-fades rather than snapping. */
static void np_apply_style(cui_ctx *ui, notepad *a, int idx) {
    if (idx < 0 || idx >= NP_STYLE_COUNT) return;
    const np_style *s = &np_styles[idx];
    a->style = idx;
    a->dark = s->mode == CUI_THEME_DARK;
    a->dark_valid = true;               /* explicit style wins over "follow OS" */
    a->accent = (int)s->accent;
    cui_app_follow_system_theme(NULL, false);
    cui_set_theme(ui, s->mode);
    cui_set_accent(ui, s->accent);
    cui_set_neutral_tint(ui, s->hue, s->chroma);
    cui_request_redraw(ui);
}

/* Tighter radii, calmer elevation and a controlled type scale give the stock
 * CUI widgets a professional editor feel. Applied every frame so a system
 * theme change or a set_theme() call cannot silently reset the settings. */
static void np_apply_theme(cui_ctx *ui, const notepad *a) {
    cui_theme *t = cui_theme_get(ui);
    if (!t) return;
    t->radius_control = 8;
    t->radius_panel = 13;
    t->radius_surface = 18;
    t->control_sm = a->density == 1 ? 26 : a->density == 2 ? 34 : 29;
    t->control_md = a->density == 1 ? 32 : a->density == 2 ? 42 : 36;
    t->control_lg = a->density == 1 ? 40 : a->density == 2 ? 50 : 44;
    t->text_xs = 11;
    t->text_sm = 12;
    t->text_body = 13;
    t->text_lg = 15;
    t->heading[0] = 32; t->heading[1] = 26; t->heading[2] = 21; t->heading[3] = 16;
    t->shadow_resting[0] = (cui_shadow){ 0, 2, 0, 0.04f };
    t->shadow_resting[1] = (cui_shadow){ 2, 8, 0, 0.05f };
    t->shadow_raised[0]   = (cui_shadow){ 2, 8, -2, 0.08f };
    t->shadow_raised[1]   = (cui_shadow){ 8, 20, -6, 0.10f };
    t->shadow_floating[0] = (cui_shadow){ 6, 18, -4, 0.12f };
    t->shadow_floating[1] = (cui_shadow){ 16, 44, -12, 0.18f };
}

/* Appearance settings: visual style picker + density/font-size controls.
 * Selecting a style applies it live (animated); changes persist on exit. */
static void np_settings_dialog(cui_ctx *ui, notepad *a) {
    cui_dialog(ui, "外观设置", &a->dlg_settings,
               .description = "选择界面风格，切换即时生效", .width = 620,
               .resizable = true, .draggable = true) {

    cui_text(ui, "界面风格", .size = 13, .font = CUI_FONT_MEDIUM);
    cui_text(ui, "每套风格包含独立的明暗模式、强调色与中性色调。",
             .size = 11, .color = CUI_COLOR_TEXT_MUTED);

    cui_choice choices[NP_STYLE_COUNT];
    for (int i = 0; i < NP_STYLE_COUNT; i++) {
        choices[i].title = np_styles[i].name;
        choices[i].description = np_styles[i].desc;
        choices[i].icon = np_styles[i].icon;
        choices[i].badge = np_styles[i].badge;
    }
    int sel = a->style >= 0 && a->style < NP_STYLE_COUNT ? a->style : 0;
    if (cui_radio_cards(ui, "np-style-cards", choices, NP_STYLE_COUNT, &sel,
                        .columns = 3)) {
        np_apply_style(ui, a, sel);
    }

    cui_separator(ui);
    cui_text(ui, "界面密度", .size = 13, .font = CUI_FONT_MEDIUM);
    {
        static const char *dens[] = { "舒适", "紧凑", "宽松" };
        if (cui_segmented(ui, "np-density", dens, 3, &a->density, .full_width = true))
            cui_request_redraw(ui);
    }

    cui_separator(ui);
    cui_text(ui, "编辑器字号", .size = 13, .font = CUI_FONT_MEDIUM);
    {
        float fs = (float)a->font_size;
        if (cui_slider(ui, "字号", &fs, NP_FONT_MIN, NP_FONT_MAX,
                       .step = 1, .show_value = true, .format = "%.0f pt"))
            a->font_size = (int)(fs + 0.5f);
    }
    cui_text(ui, "提示：外观改动会在退出时自动保存，下次启动保持。",
             .size = 11, .color = CUI_COLOR_TEXT_MUTED);
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

/* User-saved AI instructions. They are persisted immediately so a saved
 * action survives even if the app is closed without a clean exit. */
static void np_ai_actions_dialog(cui_ctx *ui, notepad *a) {
    cui_dialog(ui, "自定义 AI 动作", &a->dlg_ai_actions,
               .description = "保存常用指令，之后在 AI 面板的「任务」中一键调用",
               .width = 640, .resizable = true, .draggable = true) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            cui_text(ui, "已保存的动作", .size = 13, .font = CUI_FONT_MEDIUM);
            if (a->ai_action_count == 0)
                cui_empty_state(ui, "还没有动作", .compact = true,
                                .icon = CUI_ICON_SLIDERS_HORIZONTAL,
                                .description = "填好下面的名称与指令，点「新建」即可保存第一个动作。");
            for (int i = 0; i < a->ai_action_count; i++) {
                char line[192];
                snprintf(line, sizeof line, "%d. %s — %.60s", i + 1,
                         a->ai_actions[i].name, a->ai_actions[i].inst);
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, line, .size = 11, .nowrap = false);
                    if (cui_button(ui, "编辑", .size = CUI_SIZE_SM,
                                   .variant = CUI_BUTTON_GHOST)) {
                        a->ai_action_edit = i;
                        snprintf(a->ai_act_name, sizeof a->ai_act_name, "%s",
                                 a->ai_actions[i].name);
                        snprintf(a->ai_act_inst, sizeof a->ai_act_inst, "%s",
                                 a->ai_actions[i].inst);
                    }
                    if (cui_button(ui, "删除", .size = CUI_SIZE_SM,
                                   .variant = CUI_BUTTON_GHOST)) {
                        if (i + 1 < a->ai_action_count)
                            memmove(&a->ai_actions[i], &a->ai_actions[i + 1],
                                    (size_t)(a->ai_action_count - i - 1) *
                                    sizeof a->ai_actions[0]);
                        a->ai_action_count--;
                        if (a->ai_action_edit >= a->ai_action_count) a->ai_action_edit = -1;
                        if (a->ai_task >= 7 + a->ai_action_count) a->ai_task = 6;
                        np_config_save(a);
                    }
                }
            }
            cui_separator(ui);
            cui_input(ui, "ai-act-name", a->ai_act_name, sizeof a->ai_act_name,
                      .label = "动作名称", .placeholder = "例如：整理成周报",
                      .width = CUI_FILL);
            cui_textarea(ui, "ai-act-inst", a->ai_act_inst, sizeof a->ai_act_inst,
                         .label = "指令（当前选区或全文会附在指令之后）", .height = 90);
            cui_text(ui, "动作只回答不改正文；结果可在 AI 面板插入光标处或复制。",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "新建", .variant = CUI_BUTTON_GHOST)) {
                a->ai_action_edit = -1;
                a->ai_act_name[0] = 0;
                a->ai_act_inst[0] = 0;
            }
            if (cui_button(ui, "保存动作", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->ai_act_name[0] || !a->ai_act_inst[0] ||
                                       (a->ai_action_count >= NP_AI_ACTION_MAX &&
                                        a->ai_action_edit < 0))) {
                int idx = a->ai_action_edit;
                if (idx < 0 || idx >= a->ai_action_count) idx = a->ai_action_count++;
                snprintf(a->ai_actions[idx].name, sizeof a->ai_actions[idx].name,
                         "%s", a->ai_act_name);
                snprintf(a->ai_actions[idx].inst, sizeof a->ai_actions[idx].inst,
                         "%s", a->ai_act_inst);
                a->ai_action_edit = -1;
                np_config_save(a);
                np_toast(a, "已保存", "动作已加入 AI 面板的「任务」列表",
                         CUI_TONE_SUCCESS);
            }
            if (cui_button(ui, "关闭")) a->dlg_ai_actions = false;
        }
    }
}

/* Preview before insertion. Raw paste stays one click away. */
static void np_smart_paste_dialog(cui_ctx *ui, notepad *a) {
    cui_dialog(ui, "智能粘贴", &a->dlg_smart_paste,
               .description = a->sp_kind, .width = 680,
               .resizable = true, .draggable = true) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            cui_text(ui, "转换预览", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "内容将插入到光标处；选「原样粘贴」则完全不转换。",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_textarea(ui, "sp-preview", a->sp_out, sizeof a->sp_out,
                         .height = 220, .read_only = true);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_smart_paste = false;
            if (cui_button(ui, "原样粘贴", .variant = CUI_BUTTON_GHOST)) {
                if (np_smart_paste_insert(a, a->sp_src)) {
                    a->dlg_smart_paste = false;
                    np_toast(a, "已粘贴", "剪贴板内容已原样插入", CUI_TONE_INFO);
                }
            }
            if (cui_button(ui, "转换并插入", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->sp_out[0])) {
                if (np_smart_paste_insert(a, a->sp_out)) {
                    a->dlg_smart_paste = false;
                    np_toast(a, "已粘贴", "转换后的内容已插入光标处", CUI_TONE_SUCCESS);
                }
            }
        }
    }
}

static void np_searchprof_dialog(notepad *a, cui_ctx *ui); /* X9: 定义在 np_config_path 之后 */
static void np_mcp_dialog(notepad *a, cui_ctx *ui);        /* X6: 定义在 np_config_path 之后 */

static void np_dialogs(notepad *a, cui_ctx *ui) {
    np_settings_dialog(ui, a);
    np_ai_actions_dialog(ui, a);
    np_smart_paste_dialog(ui, a);

    /* Sidebar tree file ops: new file, new folder, rename. */
    if (np_tree_op_open) {
        const char *title = np_tree_op == NP_TREE_OP_NEWFILE ? "新建文件"
                          : np_tree_op == NP_TREE_OP_NEWDIR ? "新建文件夹"
                                                            : "重命名";
        char where[NP_PATH_CAP + 32];
        snprintf(where, sizeof where, "位置：%s", np_tree_op_path);
        cui_dialog(ui, title, &np_tree_op_open,
                   .description = where, .width = 420) {
            int flags = cui_input(ui, "tree-op-name", np_tree_op_name,
                                  sizeof np_tree_op_name,
                                  .label = np_tree_op == NP_TREE_OP_RENAME ? "新名称" : "名称",
                                  .placeholder = np_tree_op == NP_TREE_OP_NEWDIR
                                                     ? "新文件夹"
                                                     : "新文件.c",
                                  .width = CUI_FILL, .autofocus = true);
            if (flags & CUI_SUBMITTED) np_tree_confirm(a);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "取消")) {
                    np_tree_op_open = false;
                    np_tree_op = NP_TREE_OP_NONE;
                }
                if (cui_button(ui, "确定", .variant = CUI_BUTTON_PRIMARY,
                               .disabled = !np_tree_op_name[0]))
                    np_tree_confirm(a);
            }
        }
    }

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

    cui_dialog(ui, "从磁盘重新加载", &a->dlg_reload,
               .description = a->confirm_desc, .width = 420) {
        cui_text(ui, "当前文档有未保存的修改，重新加载将放弃这些修改、"
                     "以磁盘上的内容为准。", .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) {
                a->dlg_reload = false;
            }
            if (cui_button(ui, "放弃修改并重新加载",
                           .variant = CUI_BUTTON_PRIMARY)) {
                a->dlg_reload = false;
                np_do_reload(a);
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
            cui_checkbox(ui, "全词匹配", &a->find_word);
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
            if (cui_button(ui, "替换", .variant = CUI_BUTTON_GHOST,
                           .disabled = !a->find_query[0])) {
                if (!np_replace_next(a))
                    np_toast(a, "没有找到匹配", "文档中没有可替换的匹配",
                             CUI_TONE_INFO);
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

    cui_dialog(ui, "在文件中查找", &a->dlg_find_files,
               .description = "在目录里的所有文件中搜索文字", .width = 480) {
        cui_column(ui, .width = CUI_FILL, .gap = 10) {
            cui_input(ui, "ff-query", a->ff_query, sizeof a->ff_query,
                      .label = "查找", .placeholder = "要查找的内容",
                      .width = CUI_FILL, .autofocus = true);
            cui_input(ui, "ff-dir", a->ff_dir, sizeof a->ff_dir,
                      .label = "目录", .placeholder = "搜索的文件夹",
                      .width = CUI_FILL);
            cui_input(ui, "ff-pattern", a->ff_pattern, sizeof a->ff_pattern,
                      .label = "文件类型", .placeholder = "例如 *.c;*.h（留空=全部文件）",
                      .width = CUI_FILL);
            cui_checkbox(ui, "区分大小写", &a->match_case);
            cui_checkbox(ui, "全词匹配", &a->find_word);
            cui_text(ui, "自动跳过 .git、node_modules、build 等构建目录。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, "想用自然语言找文件？填好上面的描述、目录和文件类型后，点「AI 语义搜索」；返回的是真实文件名、原文行和可跳转位置。",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_find_files = false;
            if (cui_button(ui, a->sem_mode == 1 ? "问答模式" : "找文件模式",
                           .variant = CUI_BUTTON_GHOST,
                           .tooltip = a->sem_mode == 1
                               ? "当前：AI 基于工作区文件回答问题\n点击切换为：只找相关文件"
                               : "当前：AI 只找相关文件\n点击切换为：基于工作区内容回答问题")) {
                a->sem_mode = a->sem_mode == 1 ? 0 : 1;
            }
            if (cui_button(ui,
                           a->sem_done == 1 ? "AI 分析中…"
                             : (a->sem_mode == 1 ? "AI 问答" : "AI 语义搜索"),
                           .variant = CUI_BUTTON_GHOST,
                           .disabled = !a->ff_query[0] || a->sem_done == 1)) {
                np_sem_start(a);
            }
            if (cui_button(ui, "搜索", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->ff_query[0] || a->sem_done == 1)) {
                a->sem_done = 0; a->sem_last_state = 0; a->sem_cancel = false;
                a->sem_result[0] = 0; a->sem_summary[0] = 0;
                np_ff_run(a);
                a->show_output = true;
                a->out_tab = 2;
                a->dlg_find_files = false;
                char msg[160];
                snprintf(msg, sizeof msg, "扫描 %d 个目录 / %d 个文件，找到 %d 处",
                         a->ff_dirs_seen, a->ff_files_seen, a->ff_hit_count);
                np_logf(a, "文件搜索  “%s” 于 %s  ·  %s",
                        a->ff_query, a->ff_dir, msg);
                np_toast(a, a->ff_hit_count ? "文件搜索完成" : "没有找到匹配", msg,
                         a->ff_hit_count ? CUI_TONE_SUCCESS : CUI_TONE_WARNING);
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

    cui_dialog(ui, "转到偏移", &a->dlg_hexgoto,
               .description = "跳到十六进制视图的指定字节（十六进制）", .width = 360) {
        int flags = cui_input(ui, "hexgoto-off", a->hexgoto_buf, sizeof a->hexgoto_buf,
                              .label = "偏移", .placeholder = "例如 1A2B 或 0x1A2B",
                              .width = CUI_FILL, .autofocus = true);
        if (flags & CUI_SUBMITTED) np_hex_do_goto(a);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_hexgoto = false;
            if (cui_button(ui, "跳转", .variant = CUI_BUTTON_PRIMARY)) np_hex_do_goto(a);
        }
    }

    cui_dialog(ui, "查找字节", &a->dlg_hexfind,
               .description = "在十六进制视图中查找字节序列（下一个命中会滚动到视野内并选中）",
               .width = 440) {
        int flags = cui_input(ui, "hexfind-q", a->hexfind_buf, sizeof a->hexfind_buf,
                              .label = "查找内容",
                              .placeholder = a->hexfind_struct
                                  ? "例如 u32le=1000 / png:IHDR / fill:0x00:16 / str:hello"
                                  : "例如 1F 2E CC 或 0x1F2ECC",
                              .width = CUI_FILL, .autofocus = true);
        if (flags & CUI_SUBMITTED) np_hex_find_run(a, false);
        cui_checkbox(ui, "结构化查询（整数/签名/填充区/文本）", &a->hexfind_struct);
        if (!a->hexfind_struct)
            cui_checkbox(ui, "按 ASCII 文本查找", &a->hexfind_ascii);
        if (a->hexfind_struct) {
            cui_text(ui, "u32le=1000 · u16be>256 · i32le=-5 · elf/png/jpg/zip ·",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, "png:IHDR · fill:0x00:16 · str:hello（本地预筛，命中复用滚动条刻度）。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            cui_text(ui, "十六进制模式忽略空格与逗号；勾选后把输入当作普通文本逐字节匹配。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_hexfind = false;
            if (cui_button(ui, "下一个", .variant = CUI_BUTTON_GHOST,
                           .disabled = !a->hexfind_buf[0]))
                np_hex_find_run(a, true);
            if (cui_button(ui, "查找", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->hexfind_buf[0]))
                np_hex_find_run(a, false);
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

    /* LaTeX source tidying: normalize indent, join wraps, trim trailing WS. */
    cui_dialog(ui, "整理 LaTeX 源码", &a->dlg_latex_tidy,
               .description = "规范缩进、合并段内硬回车、清理行尾空白；严格保护公式/代码/注释",
               .width = 440) {
        cui_checkbox(ui, "规范缩进（按环境层级）", &a->tidy_indent);
        cui_checkbox(ui, "合并段内硬回车", &a->tidy_join);
        cui_checkbox(ui, "清理行尾空白", &a->tidy_trim);
        double sp = a->tidy_spaces;
        if (cui_number(ui, "tidy-spaces", &sp, 0, 8, .label = "每层缩进空格数（0=制表符）", .decimals = 0, .step = 1))
            a->tidy_spaces = (int)sp;
        cui_text(ui, "verbatim / lstlisting / minted / 数学环境 / 注释行会原样保留。",
                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_latex_tidy = false;
            if (cui_button(ui, "整理全文", .variant = CUI_BUTTON_PRIMARY)) {
                if (np_latex_tidy_apply(a)) {
                    np_toast(a, "整理完成", "源码已按所选规则规范化", CUI_TONE_INFO);
                    a->dlg_latex_tidy = false;
                } else {
                    np_toast(a, "整理失败", "当前文档无内容", CUI_TONE_WARNING);
                }
            }
        }
    }

    /* maclaw official-service login: the QR is generated automatically when
     * the dialog opens — no button click needed. */
    cui_dialog(ui, "maclaw 官方服务", &a->dlg_maclaw,
               .description = "扫码后使用关联 maclaw 用户的官方 LLM 服务；token 会保存在本机配置中",
               .width = 520) {
        if (a->maclaw_done == 0) np_maclaw_login_start(a); /* auto-generate */
        long st = a->maclaw_done;
        if (st != a->maclaw_last_state) {
            a->maclaw_last_state = (int)st;
            /* 授权成功即自动切到 maclaw 后端，省去用户手动选择；同时清掉
             * 之前走本地 CLI 失败留下的错误，避免面板一直显示旧错误。 */
            if (st == 3 && a->maclaw_token[0]) {
                a->ai_backend = 4;
                a->ai_cmd[0] = 0;
                a->ai_error[0] = 0;
                a->ai_result[0] = 0;
                a->ai_done = 0;
                np_config_save(a);   /* 立即持久化，重启后仍走 maclaw */
            }
            cui_request_redraw(ui);
        }
        if (st == 1 || st == 2) cui_request_redraw(ui); /* animate spinner / poll count */
        cui_text(ui, "在手机 MaClawBot 中打开「我的 → 右上角扫码」授权；服务地址由 maclaw hubcenter 自动路由，无需填写。",
                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);

        if (st == 1) {
            cui_spinner(ui, 22);
            cui_text(ui, "正在生成授权二维码…", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "关闭", .variant = CUI_BUTTON_PRIMARY)) a->dlg_maclaw = false;
            }
        } else if (st == 2 && a->maclaw_qr_ready) {
            char status[160];
            snprintf(status, sizeof status, "等待手机授权…（已轮询 %d 次，二维码 5 分钟内有效）",
                     a->maclaw_polls);
            cui_text(ui, status, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_canvas(ui, "maclaw-auth-qr", CUI_FILL,
                       (cui_size){ CUI_SIZE_FIXED, 280 },
                       np_maclaw_qr_paint, &a->maclaw_qr, sizeof a->maclaw_qr);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "关闭", .variant = CUI_BUTTON_PRIMARY)) a->dlg_maclaw = false;
            }
        } else if (st == 3) {
            cui_text(ui, "授权成功", .size = 14, .font = CUI_FONT_MEDIUM,
                     .color = CUI_COLOR_SUCCESS);
            if (a->maclaw_email[0]) {
                char who[320];
                snprintf(who, sizeof who, "maclaw 账号：%s", a->maclaw_email);
                cui_text(ui, who, .size = 12);
            }
            char saved[160];
            snprintf(saved, sizeof saved, "已保存 viewer_token（%d 字符），已自动切换到 MaClaw官方，直接点 Ask AI 即可使用。",
                     (int)strlen(a->maclaw_token));
            cui_text(ui, saved, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "重新授权", .variant = CUI_BUTTON_GHOST)) np_maclaw_login_start(a);
                if (cui_button(ui, "完成", .variant = CUI_BUTTON_PRIMARY)) a->dlg_maclaw = false;
            }
        } else {
            if (st == 4 && a->maclaw_err[0]) {
                cui_text(ui, a->maclaw_err, .size = 12, .color = CUI_COLOR_DANGER);
            }
            cui_dialog_footer(ui) {
                if (cui_button(ui, "关闭")) a->dlg_maclaw = false;
                if (cui_button(ui, "重新生成二维码", .variant = CUI_BUTTON_PRIMARY)) {
                    np_maclaw_login_start(a);
                }
            }
        }
    }

    /* LLM 服务管理: maclaw official service + user-configured
     * OpenAI-compatible providers; the AI dialog's backend select switches
     * between them. */
    cui_dialog(ui, "LLM 服务", &a->dlg_llm_prov,
               .description = "管理 maclaw 官方服务与自定义 LLM 服务商；API Key 以圆点显示，Windows 下用系统加密保存",
               .width = 660) {
        /* --- maclaw official service --- */
        cui_text(ui, "maclaw 官方服务", .size = 13, .font = CUI_FONT_MEDIUM);
        if (a->maclaw_token[0]) {
            char who[400];
            snprintf(who, sizeof who, "已授权：%s",
                     a->maclaw_email[0] ? a->maclaw_email : "（未知账号）");
            cui_text(ui, who, .size = 12, .color = CUI_COLOR_SUCCESS);
            if (cui_button(ui, "重新扫码授权", .variant = CUI_BUTTON_GHOST)) {
                a->maclaw_done = 0;
                a->maclaw_qr_ready = false;
                a->dlg_maclaw = true;
            }
        } else {
            cui_text(ui, "未授权。扫码后可使用官方 LLM，按 maclaw 用户额度计费。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            if (cui_button(ui, "扫码授权…", .variant = CUI_BUTTON_PRIMARY)) {
                a->maclaw_done = 0;
                a->maclaw_qr_ready = false;
                a->dlg_maclaw = true;
            }
        }
        cui_separator(ui);

        /* --- custom OpenAI-compatible providers --- */
        cui_text(ui, "自定义 LLM 服务商（OpenAI 兼容）", .size = 13, .font = CUI_FONT_MEDIUM);
        for (int i = 0; i < a->llm_prov_count; i++) {
            const np_llm_provider *p = &a->llm_prov[i];
            char line[640];
            snprintf(line, sizeof line, "%s — %s · %s",
                     p->name[0] ? p->name : "（未命名）",
                     p->url[0] ? p->url : "（未填写地址）",
                     p->model[0] ? p->model : "未选模型");
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                cui_text(ui, line, .size = 12);
                if (cui_button(ui, "编辑")) {
                    a->llm_prov_gen++;          /* invalidate in-flight tasks */
                    a->llm_prov_edit = i;
                    snprintf(a->llm_temp_buf, sizeof a->llm_temp_buf, "%.2f",
                             (double)p->temperature);
                    snprintf(a->llm_tok_buf, sizeof a->llm_tok_buf, "%d",
                             p->max_tokens);
                    a->llm_test_done = 0;
                    a->llm_test_result[0] = 0;
                    a->llm_test_err[0] = 0;
                }
                if (cui_button(ui, "删除")) {
                    a->llm_prov_gen++;
                    for (int j = i; j < a->llm_prov_count - 1; j++)
                        a->llm_prov[j] = a->llm_prov[j + 1];
                    a->llm_prov_count--;
                    memset(&a->llm_prov[a->llm_prov_count], 0, sizeof(np_llm_provider));
                    if (a->llm_prov_edit == i) a->llm_prov_edit = -1;
                }
            }
        }
        if (a->llm_prov_count == 0 && a->llm_prov_edit < 0)
            cui_empty_state(ui, "还没有自定义服务商", .compact = true,
                            .icon = CUI_ICON_CLOUD,
                            .description = "填写服务地址与 API Key 即可使用自己的模型服务。");
        if (a->llm_prov_count < 8 && a->llm_prov_edit < 0) {
            if (cui_button(ui, "添加服务商")) {
                a->llm_prov_gen++;
                int idx = a->llm_prov_count;
                memset(&a->llm_prov[idx], 0, sizeof(np_llm_provider));
                a->llm_prov_edit = idx;
                a->llm_temp_buf[0] = 0;
                a->llm_tok_buf[0] = 0;
                a->llm_models_done = 0;
                a->llm_test_done = 0;
                a->llm_test_result[0] = 0;
                a->llm_test_err[0] = 0;
            }
        }

        /* --- add / edit form --- */
        if (a->llm_prov_edit >= 0 && a->llm_prov_edit < 8) {
            int idx = a->llm_prov_edit;
            np_llm_provider *p = &a->llm_prov[idx];
            bool is_new = (idx == a->llm_prov_count);
            cui_text(ui, is_new ? "添加服务商" : "编辑服务商",
                     .size = 12, .font = CUI_FONT_MEDIUM);
            cui_input(ui, "llm-pname", p->name, sizeof p->name,
                      .label = "名称", .placeholder = "例如 DeepSeek", .width = CUI_FILL);
            cui_input(ui, "llm-purl", p->url, sizeof p->url,
                      .label = "服务地址", .placeholder = "例如 https://api.deepseek.com",
                      .width = CUI_FILL);
            cui_input(ui, "llm-pkey", p->api_key, sizeof p->api_key,
                      .label = "API Key", .password = true, .width = CUI_FILL);
            cui_text(ui, "Windows 下密钥加密后仅本机当前账户可用；换机器需重新填写。",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
            if (a->secret_load_failed)
                cui_text(ui, "有已保存的密钥无法在本机解密（换了账户或配置文件被移动），请重新填写。",
                         .size = 11, .color = CUI_COLOR_WARNING, .nowrap = false);
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                cui_input(ui, "llm-pmodel", p->model, sizeof p->model,
                          .label = "模型", .placeholder = "例如 deepseek-chat（可手动输入，或在下方点选）",
                          .width = CUI_FILL);
                if (cui_button(ui, a->llm_models_done == 1 ? "获取中…" : "获取模型列表",
                               .disabled = a->llm_models_done == 1 || !p->url[0])) {
                    np_llm_models_start(a, idx);
                }
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_input(ui, "llm-ptemp", a->llm_temp_buf, sizeof a->llm_temp_buf,
                          .label = "温度（0=默认）", .placeholder = "例如 0.7",
                          .width = CUI_FILL);
                cui_input(ui, "llm-ptok", a->llm_tok_buf, sizeof a->llm_tok_buf,
                          .label = "max_tokens（0=默认）", .placeholder = "例如 8192",
                          .width = CUI_FILL);
            }
            if (a->llm_models_done == 1) {
                cui_spinner(ui, 16);
            } else if (a->llm_models_done == 3 && a->llm_prov_err[0]) {
                cui_text(ui, a->llm_prov_err, .size = 11, .color = CUI_COLOR_DANGER);
            }
            if (p->models[0] && a->llm_models_done != 1) {
                char buf[NP_LLM_PROV_MODELS_CAP];
                snprintf(buf, sizeof buf, "%s", p->models);
                int mtotal = 0;
                for (char *t = strtok(buf, ","); t && *t; t = strtok(NULL, ",")) mtotal++;
                char cap[64];
                snprintf(cap, sizeof cap, "共 %d 个模型，点击填入：", mtotal);
                cui_text(ui, cap, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                snprintf(buf, sizeof buf, "%s", p->models);
                cui_row(ui, .width = CUI_FILL, .gap = 6, .wrap = true) {
                    int shown = 0;
                    for (char *tok = strtok(buf, ","); tok && *tok && shown < 24;
                         tok = strtok(NULL, ",")) {
                        if (cui_button(ui, tok, .size = CUI_SIZE_SM))
                            snprintf(p->model, sizeof p->model, "%s", tok);
                        shown++;
                    }
                }
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                if (cui_button(ui, a->llm_test_done == 1 ? "测试中…" : "测试连接",
                               .disabled = a->llm_test_done == 1 || !p->url[0] || !p->model[0]))
                    np_llm_test_start(a, idx);
                if (a->llm_test_done == 1) cui_spinner(ui, 16);
                if (a->llm_test_done == 2 && a->llm_test_result[0])
                    cui_text(ui, a->llm_test_result, .size = 11, .color = CUI_COLOR_SUCCESS);
            }
            if (a->llm_test_done == 3 && a->llm_test_err[0])
                cui_text(ui, a->llm_test_err, .size = 11, .color = CUI_COLOR_DANGER);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "取消")) {
                    a->llm_prov_gen++;
                    a->llm_prov_edit = -1;
                }
                if (cui_button(ui, "保存", .variant = CUI_BUTTON_PRIMARY,
                               .disabled = !p->url[0] || !p->model[0])) {
                    if (!p->name[0])
                        snprintf(p->name, sizeof p->name, "服务商 %d", idx + 1);
                    p->temperature = (float)atof(a->llm_temp_buf);
                    if (p->temperature < 0.f || p->temperature > 2.f)
                        p->temperature = 0.f;
                    p->max_tokens = atoi(a->llm_tok_buf);
                    if (p->max_tokens < 0) p->max_tokens = 0;
                    a->llm_prov_gen++;
                    if (is_new) a->llm_prov_count++;
                    a->llm_prov_edit = -1;
                    a->llm_models_done = 0;
                    a->secret_load_failed = false;   /* user acknowledged/re-entered */
                }
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "关闭", .variant = CUI_BUTTON_PRIMARY)) a->dlg_llm_prov = false;
        }
    }

    /* Live math formula preview: type a TeX expression, see it rendered. */
    cui_dialog(ui, "公式实时预览", &a->dlg_math_preview,
               .description = "输入 LaTeX 公式（不含 $ 符号），所见即所得实时渲染",
               .width = 560) {
        cui_input(ui, "math-expr", a->math_expr, sizeof a->math_expr,
                  .label = "公式", .placeholder = "\\frac{a}{b}+\\sqrt{x^2+y^2}",
                  .autofocus = true, .width = CUI_FILL);
        if (a->math_expr[0]) {
            char cap[64];
            snprintf(cap, sizeof cap, "渲染结果（%d 字符）", (int)strlen(a->math_expr));
            cui_text(ui, cap, .size = 12, .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_TEXT_MUTED);
            cui_math_render(ui, "math-live-preview", a->math_expr, -1, CUI_FILL,
                            &(cui_math_opts){ .boxed = true });
        } else {
            cui_text(ui, "支持 \\frac、\\sqrt、上下标、矩阵、希腊字母、重音等（与 $$ 块公式同一引擎）。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "关闭")) a->dlg_math_preview = false;
            if (cui_button(ui, "从光标提取", .variant = CUI_BUTTON_GHOST,
                           .tooltip = "取当前光标所在的 $...$ 公式填入")) {
                if (!np_math_expr_from_caret(a))
                    np_toast(a, "未找到公式", "光标不在 $...$ 公式内", CUI_TONE_WARNING);
            }
            if (cui_button(ui, "插入 $...$", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->math_expr[0])) {
                np_math_expr_insert(a);
                np_toast(a, "已插入", "公式已作为 $...$ 插入光标处", CUI_TONE_INFO);
                a->dlg_math_preview = false;
            }
        }
    }

    /* LaTeX fonts: enumerate installed fonts, write fontspec/xeCJK setup. */
    if (a->dlg_latex_fonts && a->font_scan_state == 0) {
        a->font_scan_state = 1;
        a->font_count = np_font_scan_families(a->font_list, sizeof a->font_list, 128);
        if (a->font_count < 0) a->font_count = 0;
    }
    cui_dialog(ui, "LaTeX 字体设置（XeLaTeX）", &a->dlg_latex_fonts,
               .description = "选择本机字体，写入 fontspec / xeCJK 设置到导言区",
               .width = 500) {
        const char *names[512];
        int nn = a->font_count;
        if (nn > 512) nn = 512;
        const char *p = a->font_list;
        for (int i = 0; i < nn; i++) { names[i] = p; p += strlen(p) + 1; }
        if (nn > 0) {
            a->font_latin_pick = NP_MAX(0, NP_MIN(a->font_latin_pick, nn - 1));
            a->font_cjk_pick = NP_MAX(0, NP_MIN(a->font_cjk_pick, nn - 1));
            cui_row(ui, .width = CUI_FILL, .gap = 10) {
                if (cui_select(ui, "font-latin", names, nn, &a->font_latin_pick,
                               .label = "西文字体 (mainfont)", .searchable = true, .width = CUI_FILL))
                    snprintf(a->font_latin_name, sizeof a->font_latin_name, "%s", names[a->font_latin_pick]);
                if (cui_select(ui, "font-cjk", names, nn, &a->font_cjk_pick,
                               .label = "中文字体 (CJKmainfont)", .searchable = true, .width = CUI_FILL))
                    snprintf(a->font_cjk_name, sizeof a->font_cjk_name, "%s", names[a->font_cjk_pick]);
            }
        } else {
            cui_text(ui, "未能枚举本机字体（此平台不支持），请手动输入字体名。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_input(ui, "font-latin-manual", a->font_latin_name, sizeof a->font_latin_name,
                  .label = "西文字体名", .placeholder = "例如 Times New Roman / Latin Modern Roman");
        cui_input(ui, "font-cjk-manual", a->font_cjk_name, sizeof a->font_cjk_name,
                  .label = "中文字体名", .placeholder = "例如 Noto Sans CJK SC / SimSun / Microsoft YaHei");
        cui_text(ui, "字体名须与系统安装的名称一致；配置将写入导言区，编译时请用 XeLaTeX。",
                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_latex_fonts = false;
            if (cui_button(ui, "写入导言区", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->font_latin_name[0] && !a->font_cjk_name[0])) {
                if (np_latex_fonts_apply(a)) a->dlg_latex_fonts = false;
            }
        }
    }

    /* LaTeX table wizard: paste CSV/TSV (or Excel cells) -> LaTeX table. */
    cui_dialog(ui, "插入表格", &a->dlg_latex_table,
               .description = "粘贴 Excel 单元格 / CSV / TSV，实时生成 LaTeX 三线表代码",
               .width = 560) {
        int ta = cui_textarea(ui, "ltable-src", a->ltable_src, sizeof a->ltable_src,
                              .label = "表格数据（逗号 / 制表符 / 分号分隔，可用引号包裹含分隔符的单元格）",
                              .placeholder = "姓名,成绩,备注\n张三,92,优秀\n李四,88,良好",
                              .height = 110);
        cui_row(ui, .width = CUI_FILL, .gap = 8) {
            if (cui_button(ui, "从剪贴板粘贴", .icon = CUI_ICON_CLIPBOARD,
                           .tooltip = "直接粘贴 Excel / WPS 复制的单元格或 CSV 文本")) {
                const char *clip = cui_get_clipboard_text(ui);
                if (clip && *clip) {
                    snprintf(a->ltable_src, sizeof a->ltable_src, "%s", clip);
                    ta = CUI_CHANGED;
                } else {
                    np_toast(a, "剪贴板为空", "请先复制表格内容", CUI_TONE_WARNING);
                }
            }
            if (cui_button(ui, "清空")) { a->ltable_src[0] = 0; ta = CUI_CHANGED; }
        }
        const char *styles[] = { "三线表 (booktabs)", "横线表格 (hline)" };
        cui_row(ui, .width = CUI_FILL, .gap = 10) {
            cui_select(ui, "ltable-style", styles, 2, &a->ltable_style, .label = "样式", .width = 200);
            cui_input(ui, "ltable-align", a->ltable_align, sizeof a->ltable_align,
                      .label = "列对齐 (l/c/r)", .placeholder = "lcr", .width = 140);
        }
        cui_row(ui, .width = CUI_FILL, .gap = 10) {
            cui_input(ui, "ltable-caption", a->ltable_caption, sizeof a->ltable_caption,
                      .label = "标题", .placeholder = "可留空", .width = CUI_FILL);
            cui_input(ui, "ltable-label", a->ltable_label, sizeof a->ltable_label,
                      .label = "标签", .placeholder = "tab:xxx", .width = 140);
        }
        cui_row(ui, .width = CUI_FILL, .gap = 14) {
            cui_checkbox(ui, "首行为表头", &a->ltable_header);
            cui_checkbox(ui, "长表格 longtable", &a->ltable_longtable);
            cui_checkbox(ui, "缩放适应页宽", &a->ltable_resize);
        }
        if (a->ltable_resize) {
            double w = a->ltable_width;
            if (cui_number(ui, "ltable-width", &w, 0.3, 1.0,
                           .label = "宽度 (占行宽比例)", .decimals = 2, .width = 180))
                a->ltable_width = (float)w;
        }
        /* Re-render whenever any source or option changes. */
        if ((ta & CUI_CHANGED) || (ta & CUI_SUBMITTED))
            np_latex_table_repreview(a);
        if (a->ltable_preview[0]) {
            char info[96];
            snprintf(info, sizeof info, "预览 · %d 行 × %d 列", a->ltable_rows, a->ltable_cols);
            cui_text(ui, info, .size = 12, .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_TEXT_MUTED);
            cui_textarea(ui, "ltable-preview", a->ltable_preview, sizeof a->ltable_preview,
                         .height = 120, .read_only = true);
        } else if (a->ltable_err[0]) {
            cui_text(ui, a->ltable_err, .size = 12, .color = CUI_COLOR_DANGER);
        } else {
            cui_text(ui, "粘贴数据后这里显示生成的 LaTeX 代码。", .size = 12,
                     .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "取消")) a->dlg_latex_table = false;
            if (cui_button(ui, "插入", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !a->ltable_preview[0])) {
                if (np_latex_table_insert(a)) {
                    np_toast(a, "表格已插入", "所需的 booktabs/longtable/graphicx 宏包已自动补齐", CUI_TONE_INFO);
                    a->dlg_latex_table = false;
                }
            }
        }
    }

    /* LaTeX template picker ------------------------------------------- */
    cui_dialog(ui, "从模板新建 LaTeX 文档", &a->dlg_latex_tpl,
               .description = "选择一个自包含模板，编译预览会立即启用",
               .width = 460) {
        int n = np_latex_template_count();
        const char *names[32];
        for (int i = 0; i < n && i < 32; i++)
            names[i] = np_latex_template_at(i)->name;
        if (n > 0) {
            a->latex_tpl_pick = NP_MAX(0, NP_MIN(a->latex_tpl_pick, n - 1));
            cui_select(ui, "latex-tpl-pick", names, n, &a->latex_tpl_pick,
                       .label = "模板", .placeholder = "选择模板", .width = CUI_FILL);
            const np_latex_template *t = np_latex_template_at(a->latex_tpl_pick);
            if (t) {
                cui_text(ui, t->desc, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                char hint[200];
                snprintf(hint, sizeof hint, "新建后保存为 %s；模板来自 MIT 开源库，可自由修改。",
                         t->filename);
                cui_text(ui, hint, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
        cui_text(ui, "更多在线模板：Overleaf Gallery 的 ZIP 项目可直接导入（支持多文件）。",
                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "导入 ZIP 项目…", .variant = CUI_BUTTON_GHOST,
                           .tooltip = "从 Overleaf/GitHub 下载的 ZIP 模板创建多文件项目")) {
                a->dlg_latex_tpl = false;
                np_import_latex_zip(a);
            }
            if (cui_button(ui, "在线模板库…", .variant = CUI_BUTTON_GHOST,
                           .tooltip = "从 GitHub 搜索并直接下载导入模板")) {
                a->dlg_latex_tpl = false;
                a->dlg_tpl_online = true;
            }
            if (cui_button(ui, "取消")) a->dlg_latex_tpl = false;
            if (cui_button(ui, "新建", .variant = CUI_BUTTON_PRIMARY, .disabled = n <= 0)) {
                np_doc *fresh = np_doc_from_latex_template(a, a->latex_tpl_pick);
                if (fresh) np_activate(a, a->count - 1, true);
                a->dlg_latex_tpl = false;
            }
        }
    }

    /* Binary template picker (010 Editor style) ----------------------- */
    if (a->dlg_bt) {
        int nb = 0;
        while (np_bt_builtins[nb].name) nb++;
        const char *names[64];
        for (int i = 0; i < nb && i < 64; i++) names[i] = np_bt_builtins[i].label;
        cui_dialog(ui, "二进制格式模板", &a->dlg_bt,
                   .description = "选择格式模板解析当前十六进制文件，字段会在字节区高亮并可点击定位",
                   .width = 480) {
            np_doc *bd = np_cur(a);
            if (nb > 0) {
                a->bt_pick = NP_MAX(0, NP_MIN(a->bt_pick, nb - 1));
                if (cui_select(ui, "bt-pick", names, nb, &a->bt_pick,
                               .label = "内置格式", .width = CUI_FILL)) {
                    np_bt_validate(np_bt_builtins[a->bt_pick].source,
                                   a->bt_error, sizeof a->bt_error);
                }
                const np_bt_builtin *bt = &np_bt_builtins[a->bt_pick];
                char hint[240];
                hint[0] = 0;
                const char *ln = strchr(bt->source, '\n');
                if (ln) {
                    size_t l = (size_t)(ln - bt->source);
                    if (l > sizeof hint - 1) l = sizeof hint - 1;
                    memcpy(hint, bt->source, l); hint[l] = 0;
                }
                if (hint[0]) cui_text(ui, hint, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
            if (a->bt_error[0]) cui_text(ui, a->bt_error, .size = 11, .color = CUI_COLOR_DANGER);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "加载 .bt 文件…", .variant = CUI_BUTTON_GHOST,
                               .tooltip = "加载 010 Editor 风格的自定义模板脚本")) {
                    char path[NP_PATH_CAP];
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_OPEN, .title = "打开二进制模板脚本",
                            .filter_name = "二进制模板 (*.bt)", .filter = "*.bt" },
                            path, sizeof path)) {
                        char src[BT_SRC_CAP], name[128];
                        if (np_bt_load_file(path, src, sizeof src, name, sizeof name)) {
                            if (bd) np_bt_apply(a, bd, name, src);
                            a->bt_error[0] = 0;
                            a->dlg_bt = false;
                        } else {
                            snprintf(a->bt_error, sizeof a->bt_error, "无法读取模板文件");
                        }
                    }
                }
                if (bd && bd->bt_name[0] && cui_button(ui, "清除模板", .variant = CUI_BUTTON_GHOST)) {
                    np_bt_clear(bd);
                    a->dlg_bt = false;
                }
                if (cui_button(ui, "取消")) a->dlg_bt = false;
                if (cui_button(ui, "应用", .variant = CUI_BUTTON_PRIMARY, .disabled = nb <= 0 || !bd)) {
                    const np_bt_builtin *bt = &np_bt_builtins[a->bt_pick];
                    np_bt_apply(a, bd, bt->label, bt->source);
                    a->dlg_bt = false;
                }
            }
        }
    }

    /* Online LaTeX template library ----------------------------------- */
    /* Poll the worker once per frame; import on completion, redraw on change. */
    if (a->dlg_tpl_online && a->tpl_dl_busy) {
        long done = a->tpl_dl_done;
        if (done == 1 && a->tpl_dl_kind == 1 && a->tpl_dl_result[0]) {
            char zip[NP_PATH_CAP];
            snprintf(zip, sizeof zip, "%s", a->tpl_dl_result);
            a->tpl_dl_busy = false;
            a->tpl_dl_result[0] = 0;
            np_unpack_and_open(a, zip, a->tpl_dl_stem);
            snprintf(a->tpl_status, sizeof a->tpl_status, "模板已导入并打开");
            if (a->ui) cui_request_redraw(a->ui);
        } else if (done == 1 && a->tpl_dl_kind == 2) {
            a->tpl_dl_busy = false;   /* search finished */
            if (a->ui) cui_request_redraw(a->ui);
        } else if (done == 2 || done == 3) {
            if (done == 2 && a->tpl_dl_error[0])
                snprintf(a->tpl_status, sizeof a->tpl_status, "%s", a->tpl_dl_error);
            a->tpl_dl_busy = false;
            if (a->ui) cui_request_redraw(a->ui);
        } else if (a->ui) cui_request_redraw(a->ui);  /* live progress */
    }

    cui_dialog(ui, "在线 LaTeX 模板库", &a->dlg_tpl_online,
               .description = "精选模板 + GitHub 搜索，选中后自动下载并作为多文件项目打开",
               .width = 560) {
        /* Search row */
        cui_row(ui, .gap = 8, .width = CUI_FILL) {
            cui_input(ui, "tpl-search", a->tpl_query, sizeof a->tpl_query,
                      .label = "搜索 GitHub", .placeholder = "例如：beamer / thesis / resume",
                      .width = CUI_FILL);
            if (cui_button(ui, "搜索", .disabled = a->tpl_dl_busy || !a->tpl_query[0]))
                np_start_tpl_search(a);
            if (cui_button(ui, "精选", .variant = CUI_BUTTON_GHOST,
                           .disabled = a->tpl_dl_busy)) {
                a->tpl_count = np_online_curated(a->tpl_items, NP_ONT_MAX);
                a->tpl_pick = 0;
                a->tpl_mode = 0;
                snprintf(a->tpl_status, sizeof a->tpl_status, "显示 %d 个精选模板", a->tpl_count);
            }
        }
        if (!a->tpl_items[0].name[0] && a->tpl_count == 0) {
            a->tpl_count = np_online_curated(a->tpl_items, NP_ONT_MAX);
            a->tpl_mode = 0;
            snprintf(a->tpl_status, sizeof a->tpl_status, "显示 %d 个精选模板", a->tpl_count);
        }
        if (a->tpl_count > 0) {
            const char *names[NP_ONT_MAX];
            char labels[NP_ONT_MAX][160];
            for (int i = 0; i < a->tpl_count && i < NP_ONT_MAX; i++) {
                const np_online_template *t = &a->tpl_items[i];
                if (t->stars >= 0)
                    snprintf(labels[i], sizeof labels[i], "%s  (★ %d)  %s", t->name, t->stars, t->full_name);
                else
                    snprintf(labels[i], sizeof labels[i], "%s  %s", t->name, t->full_name);
                names[i] = labels[i];
            }
            a->tpl_pick = NP_MAX(0, NP_MIN(a->tpl_pick, a->tpl_count - 1));
            cui_select(ui, "tpl-list", names, a->tpl_count, &a->tpl_pick,
                       .label = "模板", .width = CUI_FILL);
            const np_online_template *t = &a->tpl_items[a->tpl_pick];
            if (t) {
                cui_text(ui, t->desc, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                char repo[200];
                snprintf(repo, sizeof repo, "仓库：%s  ·  分支：%s", t->full_name, t->branch);
                cui_text(ui, repo, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
        if (a->tpl_dl_busy) cui_progress(ui, (float)a->tpl_progress / 100.0f);
        if (a->tpl_status[0]) cui_text(ui, a->tpl_status, .size = 12,
                                       .color = CUI_COLOR_TEXT_MUTED);

        /* TinyTeX hint */
        {
            char engine[NP_PATH_CAP];
            bool has = np_tinytex_engine(engine, sizeof engine);
            if (!has)
                cui_text(ui, "未检测到 LaTeX 引擎：点「安装 TinyTeX」可自动部署（约 24MB）。",
                         .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }

        cui_dialog_footer(ui) {
            if (cui_button(ui, "安装 TinyTeX…", .variant = CUI_BUTTON_GHOST)) {
                a->dlg_tpl_online = false;
                a->dlg_tinytex = true;
            }
            if (a->tpl_dl_busy && cui_button(ui, "取消", .variant = CUI_BUTTON_GHOST))
                a->tpl_cancel = true;
            if (cui_button(ui, "关闭")) a->dlg_tpl_online = false;
            if (cui_button(ui, "下载并导入", .variant = CUI_BUTTON_PRIMARY,
                           .disabled = a->tpl_count <= 0 || a->tpl_dl_busy)) {
                np_start_template_download(a, &a->tpl_items[a->tpl_pick]);
            }
        }
    }

    /* TinyTeX auto-install dialog -------------------------------------- */
    if (a->dlg_tinytex && a->tt_busy) {
        if (np_tinytex_state() == 2 || np_tinytex_state() == 3)
            a->tt_busy = false;
        if (a->ui) cui_request_redraw(a->ui);  /* live progress */
    }
    cui_dialog(ui, "安装 TinyTeX", &a->dlg_tinytex,
               .description = "自动下载并安装轻量 LaTeX 环境到应用私有目录",
               .width = 480) {
        char engine[NP_PATH_CAP], dir[NP_PATH_CAP];
        bool has = np_tinytex_engine(engine, sizeof engine);
        np_tinytex_dir(dir, sizeof dir);
        if (has) {
            cui_text(ui, "已检测到 LaTeX 引擎：", .size = 12);
            cui_text(ui, engine, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, "无需安装。编译会优先使用它。", .size = 12);
        } else {
            cui_text(ui, "未检测到 LaTeX 引擎。点击下方按钮自动下载 TinyTeX 基础包", .size = 12);
            cui_text(ui, "（Windows 约 24MB / Linux 约 1.4MB），安装到：", .size = 12);
            cui_text(ui, dir, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, "首次编译缺宏包时会自动联网补装。", .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        if (a->tt_busy) cui_progress(ui, (float)a->tt_progress / 100.0f);
        if (a->tt_status[0]) cui_text(ui, a->tt_status, .size = 12,
                                      .color = CUI_COLOR_TEXT_MUTED);
        cui_dialog_footer(ui) {
            if (a->tt_busy && cui_button(ui, "取消", .variant = CUI_BUTTON_GHOST))
                a->tt_cancel = true;
            if (cui_button(ui, has ? "关闭" : "稍后")) a->dlg_tinytex = false;
            if (cui_button(ui, has ? "重新检测" : "下载并安装",
                           .variant = CUI_BUTTON_PRIMARY,
                           .disabled = a->tt_busy)) {
                if (has) {
                    np_tinytex_reset();
                    snprintf(a->tt_status, sizeof a->tt_status, "已检测到引擎：%s", engine);
                } else np_start_tinytex_install(a);
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
            "Ctrl Shift F    在文件中查找（目录搜索）",
            "F1 / Ctrl Shift P  命令面板（搜索并执行全部命令、打开最近文件）",
            "F7              输出窗口",
            "F11             沉浸式编辑（隐藏菜单栏/工具栏/状态栏，再按退出）",
            "Ctrl Alt P      预览面板（Markdown 实时 / LaTeX·PDF 编译 / HTML Web）",
            "Ctrl Shift L    在当前文件中列出所有匹配行",
            "Ctrl G          转到行",
            "F5              插入日期和时间",
            "Ctrl Z / Y      撤销 / 重做",
            "Ctrl +/-        放大 / 缩小编辑器字号，Ctrl 0 重置",
            "视图 → 编辑器字体：内置开源编程字体 + 自定义 TTF / OTF 文件",
            "Alt             激活菜单栏，方向键选择菜单项",
            "编辑器内：Ctrl+A 全选、Ctrl+C/X/V 复制/剪切/粘贴、Tab 缩进",
            "编辑器内：Ctrl D 复制当前行、Alt Up / Alt Down 上下移动选中的行",
            "菜单「编辑 → 转换」：大小写、行排序、去重、删空行、剪裁空白、Tab↔空格、合并行、括号跳转",
            "Ctrl ,          外观设置（界面风格 / 深浅色 / 强调色 / 字号密度）",
            "Ctrl R          从磁盘重新加载当前文件（有未保存修改时先确认）",
            "Alt \\           行内 AI 补全：光标处触发续写建议，Tab 接受 / Esc 忽略",
            "Ctrl Alt 1..6   Markdown 标题级别（仅 .md），Ctrl Alt 0 恢复正文",
            "Ctrl Alt B/I    Markdown 加粗 / 斜体（仅 .md）",
            "Ctrl Alt T/M    LaTeX 表格向导 / 公式预览（仅 .tex）",
            "十六进制模式：二进制文件自动打开，或「文件 → 以十六进制模式重新打开」",
            "十六进制内：点击/拖选字节、方向键移动、键入十六进制覆盖、Ctrl C 复制",
            "十六进制内：Ctrl G 转到偏移、Ctrl A 全选、Ctrl Shift C 复制为 ASCII"
        };
        cui_column(ui, .width = CUI_FILL, .gap = 5) {
            for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
                cui_text(ui, lines[i], .size = 12, .font = CUI_FONT_MONO);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "知道了", .variant = CUI_BUTTON_PRIMARY)) a->dlg_help = false;
        }
    }

    cui_dialog(ui, "关于 RapidEditor", &a->dlg_about,
               .description = "由 Dr.Daniel（RapidAI）开发的多文档编辑器", .width = 440) {
        cui_column(ui, .width = CUI_FILL, .gap = 6) {
            char about_ver[64];
            snprintf(about_ver, sizeof about_ver, "RapidEditor %s", NP_APP_VERSION);
            cui_text(ui, about_ver, .size = 16, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "作者：Dr.Daniel（RapidAI）", .size = 12);
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

    /* Software update: poll the np_updater state machine each frame. The
     * network and the installer run on a worker thread; the dialog just
     * mirrors their state. */
    cui_dialog(ui, "在线更新", &a->dlg_update,
               .description = "从 GitHub Releases 获取 RapidEditor 最新版", .width = 480) {
        cui_column(ui, .width = CUI_FILL, .gap = 6) {
            int cs = np_updater_check_state();
            int ds = np_updater_dl_state();
            /* Worker threads flip these states; request a redraw on change. */
            if (cs != a->upd_last_check || ds != a->upd_last_dl) {
                a->upd_last_check = cs;
                a->upd_last_dl = ds;
                cui_request_redraw(ui);
            }
            if (ds == NP_UPD_DL_RUNNING) cui_request_redraw(ui);  /* live progress */
            if (cs == NP_UPD_IDLE) {
                cui_text(ui, "点击下方「检查更新」获取最新版本。", .size = 12);
            } else if (cs == NP_UPD_RUNNING) {
                cui_text(ui, "正在连接 GitHub…", .size = 12);
            } else if (cs == NP_UPD_FAILED) {
                cui_text(ui, "检查失败", .size = 14, .font = CUI_FONT_MEDIUM);
                cui_text(ui, np_updater_check_error(), .size = 12);
                cui_text(ui, "请确认可以访问 github.com。", .size = 12);
            } else if (cs == NP_UPD_OK) {
                const char *tag = np_updater_latest_tag();
                if (!tag[0]) {
                    cui_text(ui, "Release 数据不完整（未找到 tag_name）。", .size = 12);
                } else if (!np_update_tag_is_newer(tag)) {
                    cui_text(ui, "已是最新版本", .size = 14, .font = CUI_FONT_MEDIUM);
                    snprintf(a->update_buf, sizeof a->update_buf,
                             "当前版本 %s（最新 %s）", NP_APP_VERSION, tag);
                    cui_text(ui, a->update_buf, .size = 12);
                } else if (!np_updater_asset_url()[0]) {
                    snprintf(a->update_buf, sizeof a->update_buf,
                             "发现新版本 %s，但 Release 未附带适用于本系统的安装包。", tag);
                    cui_text(ui, a->update_buf, .size = 12);
                } else {
                    snprintf(a->update_buf, sizeof a->update_buf,
                             "发现新版本 %s（当前 %s）", tag, NP_APP_VERSION);
                    cui_text(ui, a->update_buf, .size = 14, .font = CUI_FONT_MEDIUM);
#ifndef __ANDROID__
                    if (np_updater_asset_name()[0]) {
                        snprintf(a->update_buf, sizeof a->update_buf,
                                 "安装包：%s", np_updater_asset_name());
                        cui_text(ui, a->update_buf, .size = 12);
                    }
#endif
                    if (ds == NP_UPD_DL_IDLE) {
                        cui_text(ui, "点击下方按钮下载安装包。", .size = 12);
                    } else if (ds == NP_UPD_DL_RUNNING) {
                        int pc = np_updater_dl_progress();
                        snprintf(a->update_buf, sizeof a->update_buf, "下载中 %d%%", pc);
                        cui_text(ui, a->update_buf, .size = 12);
                        cui_progress(ui, (float)pc / 100.0f);
                    } else if (ds == NP_UPD_DL_DONE) {
#ifdef __ANDROID__
                        cui_text(ui, "下载完成。点击「安装」后请在系统弹窗中确认。", .size = 12);
                        cui_text(ui, "首次安装会要求允许「安装未知应用」。", .size = 12);
#else
                        snprintf(a->update_buf, sizeof a->update_buf,
                                 "下载完成：%s", np_updater_dest_path());
                        cui_text(ui, a->update_buf, .size = 12);
                        cui_text(ui, "点击「安装」运行安装包；也可打开所在文件夹。", .size = 12);
#endif
                    } else if (ds == NP_UPD_DL_FAILED) {
                        cui_text(ui, "下载失败", .size = 14, .font = CUI_FONT_MEDIUM);
                        cui_text(ui, np_updater_dl_error(), .size = 12);
                    }
                }
            }
        }
        cui_dialog_footer(ui) {
            int cs2 = np_updater_check_state();
            int ds2 = np_updater_dl_state();
            /* Re-check is offered whenever the background job has settled. */
            if (cs2 != NP_UPD_RUNNING && ds2 != NP_UPD_DL_RUNNING) {
                if (cui_button(ui, cs2 == NP_UPD_IDLE ? "检查更新" : "重新检查"))
                    np_updater_start_check();
            }
#ifndef __ANDROID__
            if (ds2 == NP_UPD_DL_DONE && cui_button(ui, "打开所在文件夹"))
                np_updater_open_folder();
#endif
            if (cs2 == NP_UPD_OK && np_update_tag_is_newer(np_updater_latest_tag())
                && np_updater_asset_url()[0]
                && (ds2 == NP_UPD_DL_IDLE || ds2 == NP_UPD_DL_FAILED)
                && cui_button(ui, "下载安装包", .variant = CUI_BUTTON_PRIMARY)) {
                np_updater_start_download();
            }
            if (cs2 == NP_UPD_OK && np_update_tag_is_newer(np_updater_latest_tag())
                && np_updater_asset_url()[0] && ds2 == NP_UPD_DL_DONE
                && cui_button(ui, "安装", .variant = CUI_BUTTON_PRIMARY)) {
                np_updater_install();
            }
            if (cui_button(ui, "关闭")) a->dlg_update = false;
        }
    }

    np_searchprof_dialog(a, ui);   /* X9: 知识搜索源配置（只召回，不写） */
    np_mcp_dialog(a, ui);          /* X6: MCP 工具服务器管理（发现/健康/不可信输出） */
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

/* True when a .tex file looks like a compilable entry point (has a
 * documentclass declaration). Reads only the first 8 KB. */
static bool np_tex_file_is_main(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "\\documentclass") != NULL;
}

/* Multi-file LaTeX: find the main .tex of the project a document belongs to.
 * A file in a LaTeX project (main .tex, chapters, .cls/.sty/.bib) should
 * compile through the project's main entry, not as a standalone fragment.
 * Heuristic: prefer a .tex whose stem matches a common main name, else the
 * first .tex in the directory containing \\documentclass. Cached per tab so
 * this directory scan happens once, not every frame. */
static void np_tex_detect_root(notepad *a, np_doc *d) {
    if (!d || !d->path[0] || d->tex_root[0] || d->tex_root_probed) return;
    d->tex_root_probed = true;   /* probe once even if nothing is found */
    char dir[NP_PATH_CAP];
    np_dir_of(d->path, dir, sizeof dir);

    /* A .tex that itself is a standalone document is its own root. */
    bool self_is_tex = false;
    const char *dot = strrchr(d->path, '.');
    if (dot) {
        const char *ext = dot + 1;
        self_is_tex = (np_ext_in("tex;latex;ltx", ext));
    }
    if (self_is_tex && d->len > 0 &&
        (strstr(d->text, "\\documentclass") || strstr(d->text, "\\begin{document}"))) {
        snprintf(d->tex_root, sizeof d->tex_root, "%s", d->path);
        return;
    }

    cui_file_entry ents[NP_DIR_MAX];
    char names[NP_DIR_MAX * 96];
    int n = cui_fs_list(NULL, dir, ents, NP_DIR_MAX, names, sizeof names, false);
    if (n <= 0) return;

    /* Priority stems for the likely main file. */
    static const char *preferred[] = { "main", "index", "master", "paper", "thesis", "report" };
    char candidate[NP_PATH_CAP];
    for (int p = 0; p < (int)(sizeof preferred / sizeof preferred[0]); p++) {
        for (int i = 0; i < n; i++) {
            if (ents[i].dir) continue;
            const char *dot2 = strrchr(ents[i].name, '.');
            if (!dot2 || !np_ext_in("tex;latex;ltx", dot2 + 1)) continue;
            const char *pref = preferred[p];
            size_t plen = strlen(pref);
            if (strncmp(ents[i].name, pref, plen) != 0) continue;
            np_dir_join(candidate, sizeof candidate, dir, ents[i].name);
            if (np_tex_file_is_main(candidate)) {
                snprintf(d->tex_root, sizeof d->tex_root, "%s", candidate);
                return;
            }
        }
    }
    /* Otherwise the first .tex in the directory with \\documentclass. */
    for (int i = 0; i < n; i++) {
        if (ents[i].dir) continue;
        const char *dot2 = strrchr(ents[i].name, '.');
        if (!dot2 || !np_ext_in("tex;latex;ltx", dot2 + 1)) continue;
        np_dir_join(candidate, sizeof candidate, dir, ents[i].name);
        if (np_tex_file_is_main(candidate)) {
            snprintf(d->tex_root, sizeof d->tex_root, "%s", candidate);
            return;
        }
    }
}

/* Import a LaTeX project distributed as a ZIP (Overleaf/GitHub downloads).
 * Unpack next to the archive into a unique folder, then open its main .tex
 * with tex_root pinned so the multi-file compile pipeline picks it up. */
static bool np_file_exists(const char *path);  /* defined below; UTF-8 aware */

/* Extract a project ZIP into a fresh sibling folder of `zip`, open its main
 * .tex and wire up the multi-file preview. Shared by the local import dialog
 * and the online template downloader. Returns true on success. */
static bool np_unpack_and_open(notepad *a, const char *zip, const char *stem) {
    if (!zip || !*zip) return false;
    char dir[NP_PATH_CAP], use_stem[NP_PATH_CAP], dest[NP_PATH_CAP];
    np_dir_of(zip, dir, sizeof dir);
    snprintf(use_stem, sizeof use_stem, "%s", stem && *stem ? stem : np_base(zip));
    char *dot = strrchr(use_stem, '.');
    if (dot) *dot = 0;
    if (!use_stem[0]) snprintf(use_stem, sizeof use_stem, "latex-project");

    /* np_file_exists is UTF-8 aware on Windows (wide stat). Plain stat()
     * cannot see a CJK directory and would make every retry reuse a name
     * that already exists. */
    for (int i = 0; i < 1000; i++) {
        char leaf[NP_PATH_CAP];
        if (i == 0) snprintf(leaf, sizeof leaf, "%s", use_stem);
        else snprintf(leaf, sizeof leaf, "%s-%d", use_stem, i + 1);
        np_dir_join(dest, sizeof dest, dir, leaf);
        if (!np_file_exists(dest)) break;
    }
#ifdef _WIN32
    {
        wchar_t w[NP_PATH_CAP];
        if (MultiByteToWideChar(CP_UTF8, 0, dest, -1, w, NP_PATH_CAP) <= 0 ||
            (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) {
            np_toast(a, "导入失败", "无法创建项目目录", CUI_TONE_DANGER);
            return false;
        }
    }
#else
    if (mkdir(dest, 0755) != 0 && !np_file_exists(dest)) {
        np_toast(a, "导入失败", "无法创建项目目录", CUI_TONE_DANGER);
        return false;
    }
#endif

    char main_tex[NP_PATH_CAP], err[256];
    int n = np_zip_extract(zip, dest, true, 128u * 1024 * 1024, 512,
                           main_tex, sizeof main_tex, err, sizeof err);
    if (n < 0) {
        np_logf(a, "ZIP 导入失败  %s  ·  %s", np_base(zip), err);
        np_toast(a, "导入失败", err, CUI_TONE_DANGER);
        return false;
    }
    if (!main_tex[0]) {
        np_toast(a, "导入失败", "ZIP 中没有找到含 \\documentclass 的 .tex 主文件", CUI_TONE_DANGER);
        return false;
    }

    np_open_path(a, main_tex, true);
    np_doc *d = np_cur(a);
    if (d && strcmp(d->path, main_tex) == 0) {
        snprintf(d->tex_root, sizeof d->tex_root, "%s", main_tex);
        d->tex_root_manual = false;
        d->tex_root_probed = true;
        a->preview_owner[0] = 0;
    }
    a->show_preview = true;
    snprintf(a->side_dir, sizeof a->side_dir, "%s", dest);
    a->side_reload = true;

    char msg[256];
    snprintf(msg, sizeof msg, "已展开 %d 个文件 · 主文件 %s", n, np_base(main_tex));
    np_logf(a, "导入 LaTeX 项目  %s  ·  %s", np_base(zip), msg);
    np_toast(a, "项目已导入", msg, CUI_TONE_SUCCESS);
    return true;
}

static void np_import_latex_zip(notepad *a) {
    char zip[NP_PATH_CAP];
    if (!cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
            .kind = CUI_FILE_OPEN, .title = "导入 LaTeX 项目 (ZIP)",
            .filter_name = "ZIP 项目模板", .filter = "zip" },
            zip, sizeof zip))
        return;
    np_unpack_and_open(a, zip, NULL);
}

/* ---- Background workers (download / search / TinyTeX install) --------- */

#ifdef _WIN32
#include <process.h>
/* _beginthreadex returns a handle that must be closed by the creator; the
 * worker itself only calls _endthreadex, which does NOT release the handle.
 * Closing right after creation is safe — the thread keeps running. */
#define NP_BEGIN_THREAD(fn, arg) do { \
    HANDLE _th = (HANDLE)_beginthreadex(NULL, 0, \
        (unsigned (__stdcall *)(void *))(fn), (arg), 0, NULL); \
    if (_th) CloseHandle(_th); \
} while (0)
#else
#define NP_BEGIN_THREAD(fn, arg) ({ pthread_t _t; \
    pthread_create(&_t, NULL, (void *(*)(void *))(fn), (arg)); \
    pthread_detach(_t); (void)0; })
#endif

/* Download one template ZIP to a temp file. Only touches worker-owned buffers
 * and the shared progress/cancel/done fields — never CUI state. */
static void np_tpl_download_worker(void *arg) {
    notepad *a = (notepad *)arg;
    char err[256] = {0};
    long prog = 0;
    np_net_opts o = {0};
    o.progress = &prog; o.cancel = &a->tpl_cancel; o.err = err; o.err_cap = sizeof err;
    o.timeout_sec = 180;
    bool ok = np_net_download(a->tpl_dl_url, a->tpl_dl_result, &o);
    a->tpl_progress = prog;
    snprintf(a->tpl_status, sizeof a->tpl_status, "%s",
             a->tpl_cancel ? "已取消" : (ok ? "下载完成，正在导入…" : err));
    a->tpl_dl_done = a->tpl_cancel ? 3 : (ok ? 1 : 2);
    if (!ok) snprintf(a->tpl_dl_error, sizeof a->tpl_dl_error, "%s",
                      a->tpl_cancel ? "已取消" : (err[0] ? err : "下载失败"));
#ifdef _WIN32
    _endthreadex(0);
#endif
}

/* GitHub search on a worker thread; result count lands in tpl_search_done. */
static void np_tpl_search_worker(void *arg) {
    notepad *a = (notepad *)arg;
    char err[256] = {0};
    /* Search into a worker-local buffer, then copy on completion. */
    np_online_template buf[NP_ONT_MAX];
    long prog = 0;
    int n = np_online_search(a->tpl_query, buf, NP_ONT_MAX,
                             &prog, &a->tpl_cancel, err, sizeof err);
    a->tpl_progress = prog;
    if (n > 0) {
        memcpy(a->tpl_items, buf, (size_t)n * sizeof buf[0]);
        a->tpl_count = n;
        a->tpl_pick = 0;
        a->tpl_mode = 1;
        snprintf(a->tpl_status, sizeof a->tpl_status, "找到 %d 个模板", n);
    } else {
        snprintf(a->tpl_status, sizeof a->tpl_status, "%s",
                 n < 0 ? (err[0] ? err : "搜索失败") : "没有找到匹配的模板");
    }
    a->tpl_dl_done = (n > 0) ? 1 : 2;   /* reuse done flag for search result */
#ifdef _WIN32
    _endthreadex(0);
#endif
}

/* TinyTeX install on a worker thread. */
static void np_tt_install_worker(void *arg) {
    notepad *a = (notepad *)arg;
    long prog = 0;
    np_tinytex_opts o = {0};
    o.progress = &prog; o.cancel = &a->tt_cancel;
    o.err = a->tt_status; o.err_cap = sizeof a->tt_status;
    /* Common packages so Chinese + beamer templates compile first try. */
    bool ok = np_tinytex_install("ctex ctexcap xecjk beamer enumitem geometry graphics xcolor bookmark", &o);
    a->tt_progress = prog;
    if (ok) snprintf(a->tt_status, sizeof a->tt_status, "TinyTeX 安装完成");
    a->tt_busy = false;
#ifdef _WIN32
    _endthreadex(0);
#endif
}

/* Strip ONE wrapping markdown code fence ("```lang\n...\n```") from an AI
 * result that will be written into a document — models habitually wrap
 * rewrites / completions in a fence even when asked not to. Conservative:
 * only strips when the whole result is a single fenced block (opens on the
 * first line, closes on the last line, no other fence lines in between),
 * so a markdown document ABOUT code keeps its fences. */
static void np_ai_strip_code_fence(char *buf, size_t cap) {
    size_t n = strlen(buf);
    if (n < 8 || strncmp(buf, "```", 3) != 0) return;
    char *first_nl = strchr(buf, '\n');
    if (!first_nl) return;
    /* Trim trailing newline bytes, then require the last line to be ``` */
    char *end = buf + n;
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r')) end--;
    if (end - buf < 3 || strncmp(end - 3, "```", 3) != 0) return;
    char *last = end - 3;                /* start of the closing fence line */
    if (last <= first_nl || last[-1] != '\n') return;
    /* Any other fence-looking line in between (```anything) means the
     * fences may be real content — do not unwrap at all. */
    for (char *p = first_nl + 1; p + 3 < last; p++) {
        if (p[0] == '\n' && strncmp(p + 1, "```", 3) == 0)
            return;
    }
    /* Also check the very first inner line (right after the language tag). */
    if (first_nl + 4 <= last && strncmp(first_nl + 1, "```", 3) == 0) return;
    /* Non-empty content between the fences only. */
    if (first_nl + 1 >= last - 1) return;
    size_t clen = (size_t)(last - 1 - (first_nl + 1));
    memmove(buf, first_nl + 1, clen);
    buf[clen] = 0;
}

/* Shared completion bookkeeping: stage the result for hunk decisions and
 * publish it to the UI thread via the volatile state flag. */
static void np_ai_finish(notepad *a, bool ok) {
    if (ok) {
        /* Rewrites / completions / templates are about to be written into
         * the document: unwrap a single surrounding code fence. Q&A results
         * (tasks 2/3) stay verbatim for the panel. */
        int t = a->ai_task;
        if (t == 0 || t == 1 || t == 4 || t == 5 || t == 6)
            np_ai_strip_code_fence(a->ai_result, sizeof a->ai_result);
        /* The staged buffer starts as the ORIGINAL text sent to the model:
         * the diff view compares staged vs result, and each accepted hunk
         * advances staged toward the result. Seeding it with the result
         * would make the two sides identical and the hunk buttons dead. */
        snprintf(a->ai_staged, sizeof a->ai_staged, "%s", a->ai_orig);
        a->ai_done = 2;
    } else {
        /* Different layers word cancellation differently ("已停止" in the
         * stream decoder, "已取消" in the HTTP layer). Treat both as a user
         * stop; only an empty error means a plain failure. */
        bool stopped = a->ai_cancel &&
            (!a->ai_error[0] || !strcmp(a->ai_error, "已停止") ||
             !strcmp(a->ai_error, "已取消"));
        a->ai_done = stopped ? 5 : 3;
    }
}

static bool np_ai_is_rewrite(int task) {
    return task == 0 || task == 1 || task == 4;
}

/* Short human-readable context label used in every AI prompt. Keeping this
 * out of the workers means all three backends describe the same content. */
static void np_ai_context_label(const np_doc *d, char *out, size_t cap) {
    if (!d) { snprintf(out, cap, "文本"); return; }
    if (d->is_hex) { snprintf(out, cap, "二进制（hexdump 字节片段）"); return; }
    if (np_doc_is_latex(d)) snprintf(out, cap, "LaTeX 源码");
    else if (np_doc_is_markdown(d)) snprintf(out, cap, "Markdown 文档");
    else if (d->lang == np_lang_index("json")) snprintf(out, cap, "JSON 数据");
    else if (d->lang == np_lang_index("sql")) snprintf(out, cap, "SQL 代码");
    else if (d->lang > NP_LANG_TEXT && d->lang < NP_LANG_COUNT)
        snprintf(out, cap, "%s 代码", NP_LANGS[d->lang].label);
    else snprintf(out, cap, "文本");
}

/* Streaming callback: the OpenAI helper has already appended the delta to
 * ai_result; publish its length so the panel can show partial output. */
static bool np_ai_delta_cb(const char *delta, size_t len, void *user) {
    notepad *a = (notepad *)user;
    (void)delta; (void)len;
    a->ai_stream_len = (long)strlen(a->ai_result);
    return !(a->ai_cancel);
}

/* ---- controllable context ----------------------------------------- */
/* Append into a fixed buffer, always leaving room for the NUL. */
static size_t np_ctx_app(char *out, size_t cap, size_t o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
    if (w < 0) return o;
    return (o + (size_t)w >= cap) ? cap - 1 : o + (size_t)w;
}

/* Forward declarations: defined with the file-scan helpers below, but needed
 * here when assembling the opt-in context on the UI thread. */
static void np_ignore_load(const char *root);
static bool np_ignore_check(const char *root, const char *dir,
                            const char *name, bool is_dir);

/* Assemble every opt-in source into one block. Runs on the UI thread so the
 * preview and the request use exactly the same bytes. Long tabs and result
 * lists are capped; the summary tells the user where content was cut. */
static void np_ai_refresh_sources(notepad *a) {
    if (!a) return;
    np_doc *cur = np_cur(a);
    size_t o = 0;
    int ntabs = 0, nfinds = 0, nign = 0;
    bool cut = false;
    const char *ign_root = a->side_dir[0] ? a->side_dir : ".";
    np_ignore_load(ign_root);

    if (a->ai_ctx_tabs) {
        o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                       "--- 其他已打开标签页 ---\n");
        for (int i = 0; i < a->count; i++) {
            np_doc *t = a->docs[i];
            if (!t || t == cur || !t->text) continue;
            if (t->path[0] && np_ignore_check(ign_root, "", np_base(t->path), false)) {
                nign++;              /* private file: never send it to the AI */
                continue;
            }
            const char *name = t->path[0] ? t->path : t->tab_title;
            if (t->is_hex) {
                /* 二进制轨标签页：贡献带偏移的 hexdump 字节片段（M3 按轨路由） */
                size_t hn = (size_t)t->len < 256 ? (size_t)t->len : 256;
                char hx[4096];
                np_hexdump_snippet((const unsigned char *)t->text, hn,
                                   hx, sizeof hx);
                o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                               "【%s】(二进制 · 前 %zu 字节)\n%s\n",
                               name, hn, hx);
                ntabs++;
                if (o >= sizeof a->ai_ctx_sources - 256) { cut = true; break; }
                continue;
            }
            size_t n = strlen(t->text);
            if (n > 4000) { n = 4000; cut = true; }
            o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                           "【%s】(前 %zu 字节)\n%.*s\n", name, n, (int)n, t->text);
            ntabs++;
            if (o >= sizeof a->ai_ctx_sources - 256) { cut = true; break; }
        }
    }
    if (a->ai_ctx_finds) {
        o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                       "%s--- 查找结果 ---\n", o ? "\n" : "");
        if (a->hit_count > 0 && a->find_query[0]) {
            nfinds = a->hit_count;
            o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                           "当前文件「%s」共 %d 处，附前 40 处：\n",
                           a->find_query, a->hit_count);
            for (int i = 0; i < a->hit_count && i < 40; i++)
                o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                               "第%d行: %s\n", a->hits[i].line, a->hits[i].text);
            if (a->hit_count > 40) cut = true;
        } else if (a->ff_hit_count > 0) {
            nfinds = a->ff_hit_count;
            o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                           "目录搜索「%s」共 %d 处，附前 40 处：\n",
                           a->ff_query, a->ff_hit_count);
            for (int i = 0; i < a->ff_hit_count && i < 40; i++) {
                const np_fhit *h = &a->ff_hits[i];
                const char *p = (h->file >= 0 && h->file < a->ff_count)
                                    ? a->ff_paths[h->file] : "";
                o = np_ctx_app(a->ai_ctx_sources, sizeof a->ai_ctx_sources, o,
                               "%s:%d: %s\n", p, h->line, h->text);
            }
            if (a->ff_hit_count > 40) cut = true;
        }
    }
    if (o >= sizeof a->ai_ctx_sources - 256) cut = true;
    a->ai_ctx_sources[o] = 0;

    /* Pre-send summary: what is included, roughly how big, where cut. */
    size_t main_len = 0;
    const char *main_label = "全文";
    if (cur) {
        int s0 = NP_MIN(cur->caret, cur->anchor), s1 = NP_MAX(cur->caret, cur->anchor);
        s0 = NP_MAX(0, NP_MIN(s0, cur->len));
        s1 = NP_MAX(s0, NP_MIN(s1, cur->len));
        if (a->ai_task == 5) { main_label = "光标附近"; main_len = (size_t)cur->len; }
        else if (a->ai_ctx == 0 && s1 > s0) {
            main_label = "当前选区"; main_len = (size_t)(s1 - s0);
        } else main_len = (size_t)cur->len;
    }
    size_t total = main_len + strlen(a->ai_ctx_sources) + strlen(a->ai_extra) +
                   strlen(a->ai_inst) + strlen(a->ai_prev);
    size_t so = np_ctx_app(a->ai_ctx_summary, sizeof a->ai_ctx_summary, 0,
                           "本次发送：%s", main_label);
    if (ntabs) so = np_ctx_app(a->ai_ctx_summary, sizeof a->ai_ctx_summary, so,
                               " + 其他标签页(%d)", ntabs);
    if (nign) so = np_ctx_app(a->ai_ctx_summary, sizeof a->ai_ctx_summary, so,
                              " [.aiignore 排除 %d]", nign);
    if (nfinds) so = np_ctx_app(a->ai_ctx_summary, sizeof a->ai_ctx_summary, so,
                                " + 查找结果(%d)", nfinds);
    if (a->ai_extra[0]) so = np_ctx_app(a->ai_ctx_summary, sizeof a->ai_ctx_summary, so,
                                        " + 编译日志");
    np_ctx_app(a->ai_ctx_summary, sizeof a->ai_ctx_summary, so,
               " · 约 %zu KB%s", (total + 1023) / 1024,
               cut ? "（过长内容已截取）" : "");
    a->ai_ctx_src_doc = a->active;
}

/* One prompt builder shared by all three backends, so an extra context
 * block (compiler log, search results, opted-in sources) reaches every
 * agent unchanged. */
static size_t np_ai_prompt_size(const notepad *a) {
    return strlen(a->ai_prev) + strlen(a->ai_inst) + strlen(a->ai_extra) +
           strlen(a->ai_ctx_sources) + strlen(a->ai_orig) + 256;
}
static void np_ai_build_prompt(const notepad *a, char *out, size_t cap) {
    const char *prev  = a->ai_prev[0]  ? a->ai_prev  : "";
    const char *extra = a->ai_extra[0] ? a->ai_extra : "";
    const char *srcs  = a->ai_ctx_sources[0] ? a->ai_ctx_sources : "";
    snprintf(out, cap, "%s%s%s%s%s%s%s\n\n--- 待处理内容（%s）---\n%s\n",
             prev, prev[0] ? "\n" : "",
             a->ai_inst[0] ? a->ai_inst : "请处理以下内容",
             extra[0] ? "\n\n" : "", extra,
             srcs[0] ? "\n\n" : "", srcs,
             a->ai_ctx_label[0] ? a->ai_ctx_label : "文本", a->ai_orig);
}

/* AI worker: build the prompt file from the instruction + captured context,
 * run the user's agent command, read back its answer. Touches only worker-
 * owned paths and the shared volatile done flag; never CUI state. */
static void np_ai_worker(void *arg) {
    notepad *a = (notepad *)arg;
    /* prompt = instruction + context, all in one file the agent reads. */
    size_t need = np_ai_prompt_size(a);
    char *prompt = (char *)malloc(need);
    if (!prompt) { a->ai_done = 3; snprintf(a->ai_error, sizeof a->ai_error, "内存不足"); goto done; }
    np_ai_build_prompt(a, prompt, need);
    bool wrote = np_ai_write_context(a->ai_ctx_path, prompt);
    free(prompt);
    if (!wrote) { a->ai_done = 3; snprintf(a->ai_error, sizeof a->ai_error, "无法写入临时提示文件"); goto done; }
    if (!np_ai_exec_cancel(a->ai_cmd, a->ai_ctx_path, a->ai_out_path, NULL,
                           &a->ai_cancel, a->ai_error, sizeof a->ai_error)) {
        np_ai_finish(a, false);
        goto done;
    }
    long n = np_ai_read_result(a->ai_out_path, a->ai_result, sizeof a->ai_result);
    if (n < 0) {
        snprintf(a->ai_error, sizeof a->ai_error, "无法读取 AI 输出");
        np_ai_finish(a, false);
        goto done;
    }
    np_ai_finish(a, true);
done:
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

/* maclaw official LLM worker: use the viewer_token obtained by scanning the
 * QR with MaClawBot. The Hub bills this request to the linked maclaw user. */
static void np_ai_maclaw_worker(void *arg) {
    notepad *a = (notepad *)arg;
    if (!a->maclaw_token[0]) {
        a->ai_done = 3;
        snprintf(a->ai_error, sizeof a->ai_error,
                 "请先在「maclaw 官方服务」中扫码登录");
        goto done;
    }
    size_t prompt_cap = np_ai_prompt_size(a);
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) {
        a->ai_done = 3;
        snprintf(a->ai_error, sizeof a->ai_error, "内存不足");
        goto done;
    }
    np_ai_build_prompt(a, prompt, prompt_cap);
    bool ok = np_maclaw_llm_chat(a->maclaw_hub, a->maclaw_token, prompt,
                                 a->ai_result, sizeof a->ai_result,
                                 a->ai_error, sizeof a->ai_error);
    free(prompt);
    if (a->ai_cancel && ok) {
        snprintf(a->ai_error, sizeof a->ai_error, "已停止");
        ok = false;
    }
    np_ai_finish(a, ok);
done:
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

/* maclaw device-authorization worker: create a session, encode its QR, then
 * poll until the phone authorizes (or timeout). Only touches worker-owned /
 * volatile shared fields, never CUI state. */
static void np_maclaw_login_worker(void *arg) {
    notepad *a = (notepad *)arg;
    np_maclaw_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.center_url, sizeof cfg.center_url, "%s", a->maclaw_center);
    snprintf(cfg.client_name, sizeof cfg.client_name, "RapidEditor AI 助手");
    snprintf(cfg.client_id, sizeof cfg.client_id, "rapideditor-ai-assistant");
    cfg.timeout_sec = 20;

    a->maclaw_done = 1; /* creating */
    if (!np_maclaw_create_session(&cfg, &a->maclaw_sess)) {
        snprintf(a->maclaw_err, sizeof a->maclaw_err, "%s",
                 a->maclaw_sess.err[0] ? a->maclaw_sess.err : "创建授权会话失败");
        a->maclaw_done = 4;
        goto done;
    }
    if (!np_qr_encode(a->maclaw_sess.qr_payload, strlen(a->maclaw_sess.qr_payload),
                      &a->maclaw_qr)) {
        snprintf(a->maclaw_err, sizeof a->maclaw_err, "二维码编码失败（payload 过长）");
        a->maclaw_done = 4;
        goto done;
    }
    a->maclaw_qr_ready = true;
    a->maclaw_done = 2; /* polling, QR ready */

    for (int i = 0; i < 90; i++) { /* up to 3 minutes */
#ifdef _WIN32
        Sleep(2000);
#else
        usleep(2000000);
#endif
        char tok[512] = {0}, email[256] = {0}, hub[512] = {0}, err[256] = {0};
        const char *st = np_maclaw_poll(&cfg, &a->maclaw_sess,
                                        tok, sizeof tok, hub, sizeof hub,
                                        email, sizeof email,
                                        err, sizeof err);
        a->maclaw_polls = i + 1;
        if (st && !strcmp(st, "authorized") && tok[0]) {
            snprintf(a->maclaw_token, sizeof a->maclaw_token, "%s", tok);
            snprintf(a->maclaw_email, sizeof a->maclaw_email, "%s", email);
            /* The relay resolved which Hub the account lives on; keep it for
             * the official LLM endpoint. The user never has to enter it. */
            if (hub[0]) {
                np_maclaw_normalize_hub(hub, a->maclaw_hub, sizeof a->maclaw_hub);
            }
            a->maclaw_done = 3; /* ok */
            goto done;
        }
        if (st && !strcmp(st, "expired")) {
            snprintf(a->maclaw_err, sizeof a->maclaw_err, "二维码已过期，请重新生成");
            a->maclaw_done = 4;
            goto done;
        }
        if (!st) { /* transient error: keep polling a few times */
            if (i > 3) {
                snprintf(a->maclaw_err, sizeof a->maclaw_err, "%s",
                         err[0] ? err : "轮询失败");
                a->maclaw_done = 4;
                goto done;
            }
        }
    }
    snprintf(a->maclaw_err, sizeof a->maclaw_err, "等待授权超时（3 分钟）");
    a->maclaw_done = 4;
done:
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

static void np_maclaw_login_start(notepad *a) {
    /* HA: probe the candidate HubCenters, use the first healthy one. */
    char picked[512], perr[256] = {0};
    np_maclaw_pick_center(a->maclaw_centers, picked, sizeof picked,
                          perr, sizeof perr);
    snprintf(a->maclaw_center, sizeof a->maclaw_center, "%s", picked);
    np_maclaw_normalize_hub(a->maclaw_center, a->maclaw_center, sizeof a->maclaw_center);
    if (!a->maclaw_center[0]) return;
    a->maclaw_token[0] = 0; a->maclaw_email[0] = 0; a->maclaw_err[0] = 0;
    a->maclaw_qr_ready = false; a->maclaw_polls = 0;
    memset(&a->maclaw_sess, 0, sizeof a->maclaw_sess);
    a->maclaw_done = 1;
    NP_BEGIN_THREAD(np_maclaw_login_worker, a);
}

/* Find a CLI on PATH (no shell, no console window): scan PATH segments for an
 * executable with the given name. Used to pick a default command template. */
static bool np_ai_has_tool(const char *name) {
    if (!name || !*name) return false;
    const char *path = getenv("PATH");
    if (!path) return false;
    char seg[2048];
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, ';');
        if (!end) end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 0 || n >= sizeof seg - 64) { p = end ? end + 1 : p + strlen(p); if (!end) break; continue; }
        memcpy(seg, p, n); seg[n] = 0;
#ifdef _WIN32
        char exe[512];
        snprintf(exe, sizeof exe, "%s\\%s.exe", seg, name);
        if (np_file_exists(exe)) return true;
        snprintf(exe, sizeof exe, "%s\\%s.cmd", seg, name);
        if (np_file_exists(exe)) return true;
#else
        char exe[512];
        snprintf(exe, sizeof exe, "%s/%s", seg, name);
        if (np_file_exists(exe)) return true;
#endif
        if (!end) break;
        p = end + 1;
    }
    return false;
}

/* Custom OpenAI-compatible provider chat worker: used when the AI backend
 * selects one of the user-configured providers (ai_backend >= 5). */
static void np_ai_openai_worker(void *arg) {
    notepad *a = (notepad *)arg;
    int idx = a->ai_backend - 5;
    if (idx < 0 || idx >= a->llm_prov_count) {
        snprintf(a->ai_error, sizeof a->ai_error, "LLM 服务商不存在");
        a->ai_done = 3;
        goto done;
    }
    const np_llm_provider *p = &a->llm_prov[idx];
    np_openai_set_params(p->temperature, p->max_tokens);
    size_t prompt_cap = np_ai_prompt_size(a);
    char *prompt = (char *)malloc(prompt_cap);
    if (!prompt) {
        snprintf(a->ai_error, sizeof a->ai_error, "内存不足");
        a->ai_done = 3;
        goto done;
    }
    np_ai_build_prompt(a, prompt, prompt_cap);
    bool ok = np_openai_chat_stream(p->url, p->api_key, p->model, prompt,
                                    a->ai_result, sizeof a->ai_result,
                                    a->ai_error, sizeof a->ai_error,
                                    &a->ai_cancel, np_ai_delta_cb, a);
    /* Non-Windows builds currently have no streaming POST transport; keep the
     * original non-streaming path working there. */
    if (!ok && !a->ai_cancel && !a->ai_result[0] &&
        (strstr(a->ai_error, "curl CLI") || strstr(a->ai_error, "Android"))) {
        a->ai_error[0] = 0;
        ok = np_openai_chat(p->url, p->api_key, p->model, prompt,
                            a->ai_result, sizeof a->ai_result,
                            a->ai_error, sizeof a->ai_error);
    }
    free(prompt);
    if (a->ai_cancel && !a->ai_result[0]) {
        snprintf(a->ai_error, sizeof a->ai_error, "已停止");
        ok = false;
    }
    np_ai_finish(a, ok);
done:
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

/* Fetch the model list for one user-configured provider in the background
 * and cache the comma-separated ids on the provider entry. */
static void np_llm_models_worker(void *arg) {
    notepad *a = (notepad *)arg;
    int idx = a->llm_models_prov;
    int gen = a->llm_models_gen;
    char csv[NP_LLM_PROV_MODELS_CAP] = {0}, err[256] = {0};
    int n = -1;
    /* Read only the UI-thread snapshot, never the editable provider slot. */
    if (idx >= 0 && idx < 8) {
        n = np_openai_list_models(a->llm_models_url, a->llm_models_key,
                                  csv, sizeof csv, err, sizeof err);
    }
    /* The dialog may have deleted or re-opened the provider while we waited.
     * A stale result must not land in an unrelated slot. idx == count is the
     * "being added right now" entry, which still has a valid slot. */
    bool stale = (a->llm_prov_gen != gen);
    if (!stale && n > 0 && idx >= 0 && idx <= a->llm_prov_count)
        snprintf(a->llm_prov[idx].models, sizeof a->llm_prov[idx].models, "%s", csv);
    if (!stale && n < 0 && err[0])
        snprintf(a->llm_prov_err, sizeof a->llm_prov_err, "%s", err);
    a->llm_models_done = stale ? 0 : ((n > 0) ? 2 : 3);
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

static void np_llm_models_start(notepad *a, int prov_idx) {
    if (a->llm_models_done == 1) return;
    if (prov_idx < 0 || prov_idx >= 8) return;
    const np_llm_provider *p = &a->llm_prov[prov_idx];
    snprintf(a->llm_models_url, sizeof a->llm_models_url, "%s", p->url);
    snprintf(a->llm_models_key, sizeof a->llm_models_key, "%s", p->api_key);
    a->llm_models_prov = prov_idx;
    a->llm_models_gen = a->llm_prov_gen;
    a->llm_prov_err[0] = 0;
    a->llm_models_done = 1;
    NP_BEGIN_THREAD(np_llm_models_worker, a);
}

/* Send one short chat completion to the provider being edited, to verify
 * url + key + model actually work before saving. */
static void np_llm_test_worker(void *arg) {
    notepad *a = (notepad *)arg;
    int gen = a->llm_test_gen;
    char reply[96] = {0}, err[256] = {0};
    bool ok = false;
    if (a->llm_test_url[0] && a->llm_test_model[0]) {
        /* clock() measures CPU time; a network wait needs wall-clock time. */
        time_t t0 = time(NULL);
        ok = np_openai_chat(a->llm_test_url, a->llm_test_key, a->llm_test_model,
                            "请只回复两个字：正常",
                            reply, sizeof reply, err, sizeof err);
        double sec = difftime(time(NULL), t0);
        if (ok)
            snprintf(a->llm_test_result, sizeof a->llm_test_result,
                     "连接正常 · %.1fs · 回复：%.60s", sec, reply);
        else if (!err[0])
            snprintf(err, sizeof err, "请求失败");
    } else {
        snprintf(err, sizeof err, "请先填写服务地址与模型");
    }
    /* Discard results from a form that is no longer being shown. */
    bool stale = (a->llm_prov_gen != gen);
    if (!stale) {
        if (ok) a->llm_test_err[0] = 0;
        else snprintf(a->llm_test_err, sizeof a->llm_test_err, "%s", err);
        a->llm_test_done = ok ? 2 : 3;
    } else {
        a->llm_test_done = 0;
    }
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

static void np_llm_test_start(notepad *a, int prov_idx) {
    if (a->llm_test_done == 1) return;
    if (prov_idx < 0 || prov_idx >= 8) return;
    const np_llm_provider *p = &a->llm_prov[prov_idx];
    snprintf(a->llm_test_url, sizeof a->llm_test_url, "%s", p->url);
    snprintf(a->llm_test_key, sizeof a->llm_test_key, "%s", p->api_key);
    snprintf(a->llm_test_model, sizeof a->llm_test_model, "%s", p->model);
    a->llm_test_prov = prov_idx;
    a->llm_test_gen = a->llm_prov_gen;
    a->llm_test_result[0] = 0;
    a->llm_test_err[0] = 0;
    a->llm_test_done = 1;
    NP_BEGIN_THREAD(np_llm_test_worker, a);
}

/* Prepare and launch an AI request from the dialog state. */
/* 按轨路由（M3 清理）：二进制文档的 AI 不再一刀切跳过 —— 只读任务
 * （解释/检查/自定义回答）照常可用，上下文用带偏移的 hexdump 片段；
 * 改写/补全类任务没有文本编辑器落点，引导走 AI 工作台 → B3 审查。 */
static size_t np_hexdump_snippet(const unsigned char *p, size_t n,
                                 char *out, size_t cap) {
    /* 每行最多 8("0x%04zX:") + 16*3(hex) + 3("  |") + 16(ASCII) + 2("|\n")
     * ≈ 77 字节 + NUL。所有写入前先验剩余空间，杜绝 o 越过 cap。
     * （旧实现的 `o < cap - 80` 只在行首检查、ASCII 区 break 后仍无条件
     * 写 '|''\n'，cap 小或行中溢出时会 out[cap] 越界写一字节。） */
    size_t o = 0;
    if (!out || cap < 16) { if (out && cap) out[0] = 0; return 0; }
    size_t rows = (n + 15) / 16;
    for (size_t r = 0; r < rows; ++r) {
        size_t from = r * 16;
        size_t to = NP_MIN(from + 16, n);
        int w = snprintf(out + o, cap - o, "0x%04zX:", from);
        if (w <= 0) break;
        o += (size_t)w;
        if (o >= cap) o = cap - 1;   /* snprintf 截断时 w 是"本应写入"长度 */
        if (o + 1 >= cap) break;
        for (size_t i = from; i < to; ++i) {
            w = snprintf(out + o, cap - o, " %02X", p[i]);
            if (w <= 0) goto done;
            o += (size_t)w;
            if (o >= cap) o = cap - 1;
            if (o + 1 >= cap) goto done;
        }
        /* ASCII 区 + 行尾 "|" "\n" 至少要 4 字节余量（含 NUL） */
        if (o + 4 >= cap) goto done;
        w = snprintf(out + o, cap - o, "  |");
        if (w <= 0) break;
        o += (size_t)w;
        if (o >= cap) o = cap - 1;
        for (size_t i = from; i < to; ++i) {
            unsigned char c = p[i];
            if (o + 4 >= cap) break;   /* 剩余字符 + '|' + '\n' + NUL */
            out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        out[o++] = '|';
        out[o++] = '\n';
    }
done:
    out[o] = 0;
    return o;
}

static void np_ai_start(notepad *a) {
    if (a->ai_done == 1) return;                 /* already running */
    np_doc *d = np_cur(a);
    if (!d) return;
    if (d->is_hex) {
        if (np_ai_is_rewrite(a->ai_task) || a->ai_task == 5) {
            np_toast(a, "二进制文档",
                     "字节改动请走「AI 工作台」生成补丁并经补丁审查写入；此处仅支持解释/检查等只读任务",
                     CUI_TONE_WARNING);
            return;
        }
    }
    a->ai_cancel = false;
    a->ai_stream_len = 0;
    a->ai_staged[0] = 0;
    a->ai_diff_current = 0;
    a->ai_hist_pushed = false;
    a->ai_error[0] = 0;
    np_ai_context_label(d, a->ai_ctx_label, sizeof a->ai_ctx_label);
    int s0 = NP_MIN(d->caret, d->anchor), s1 = NP_MAX(d->caret, d->anchor);
    s0 = NP_MAX(0, NP_MIN(s0, d->len)); s1 = NP_MAX(s0, NP_MIN(s1, d->len));
    bool src_truncated = false;
    if (d->is_hex) {
        /* 二进制轨上下文：带偏移的 hexdump（选区优先，否则头 4KB）。
         * 结果只作回答展示，不回写 —— 见 np_ai_apply 的按轨路由。 */
        size_t from = (a->ai_ctx == 0 && s1 > s0) ? (size_t)s0 : 0;
        size_t to = (a->ai_ctx == 0 && s1 > s0) ? (size_t)s1
                                                : (size_t)NP_MIN(d->len, 4096);
        if (to > from + 4096) { to = from + 4096; src_truncated = true; }
        np_hexdump_snippet((const unsigned char *)d->text + from, to - from,
                           a->ai_orig, sizeof a->ai_orig);
        a->ai_target_s = (int)from;
        a->ai_target_e = (int)to;
    } else if (a->ai_task == 5) {
        /* Completion: feed the text around the caret, and insert the result
         * AT THE CARET. Using s0 here would insert at the selection's start
         * when the user selected backwards/forwards and then asked to
         * continue writing — the visible caret is the intended position. */
        int c = NP_MAX(0, NP_MIN(d->caret, d->len));
        int before = 3000, after = 1000;
        int from = c > before ? c - before : 0;
        int to = c + after < d->len ? c + after : d->len;
        size_t n = (size_t)(to - from);
        if (n >= sizeof a->ai_orig) { n = sizeof a->ai_orig - 1; src_truncated = true; }
        memcpy(a->ai_orig, d->text + from, n); a->ai_orig[n] = 0;
        a->ai_target_s = c; a->ai_target_e = c;      /* empty range = insert */
    } else if (a->ai_ctx == 0 && s1 > s0) {
        size_t n = (size_t)(s1 - s0);
        if (n >= sizeof a->ai_orig) { n = sizeof a->ai_orig - 1; src_truncated = true; }
        memcpy(a->ai_orig, d->text + s0, n); a->ai_orig[n] = 0;
        a->ai_target_s = s0;
        /* Only claim the range actually shown to the AI. Otherwise a result
         * based on the first 64 KB would delete the unseen tail on accept. */
        a->ai_target_e = src_truncated ? s0 + (int)n : s1;
    } else {
        size_t n = (size_t)d->len;
        if (n >= sizeof a->ai_orig) { n = sizeof a->ai_orig - 1; src_truncated = true; }
        memcpy(a->ai_orig, d->text, n); a->ai_orig[n] = 0;
        a->ai_target_s = 0;
        a->ai_target_e = src_truncated ? (int)n : d->len;
    }
    if (src_truncated)
        np_toast(a, d->is_hex ? "字节数超限" : "内容超过 64 KB",
                 d->is_hex ? "仅发送部分字节的 hexdump；建议缩小选区。"
                           : "仅发送前 64 KB；结果只会替换已发送的部分，其余内容保留。建议缩小选区。",
                 CUI_TONE_WARNING);
    snprintf(a->ai_target_path, sizeof a->ai_target_path, "%s", d->path);
    a->ai_target_uid = d->uid;
    a->ai_target_rev = d->content_rev;
    a->ai_target_len = d->len;
    np_ai_refresh_sources(a);            /* snapshot exactly what we send */

    /* Multi-turn: carry the previous Q&A into this prompt when enabled. */
    if (a->ai_followup && a->ai_result[0]) {
        char q[512], prev_a[6000];
        snprintf(q, sizeof q, "%s", a->ai_inst[0] ? a->ai_inst : "（无）");
        snprintf(prev_a, sizeof prev_a, "%s", a->ai_result);
        snprintf(a->ai_prev, sizeof a->ai_prev,
                 "--- 上一轮指令 ---\n%s\n--- 上一轮回答 ---\n%s", q, prev_a);
    } else {
        a->ai_prev[0] = 0;
    }

    /* maclaw official service uses the Hub LLM proxy, not a local CLI. */
    if (a->ai_backend == 4) {
        if (!a->maclaw_token[0]) {
            np_toast(a, "尚未登录 maclaw",
                     "请先打开「LLM 服务」用手机扫码授权", CUI_TONE_WARNING);
            return;
        }
        a->ai_result[0] = 0; a->ai_error[0] = 0;
        a->ai_done = 1;                          /* running */
        NP_BEGIN_THREAD(np_ai_maclaw_worker, a);
        return;
    }

    /* User-configured OpenAI-compatible providers (backend >= 5). */
    if (a->ai_backend >= 5) {
        int idx = a->ai_backend - 5;
        if (idx >= a->llm_prov_count) {
            np_toast(a, "服务商不存在", "请在「LLM 服务」中重新选择", CUI_TONE_WARNING);
            return;
        }
        const np_llm_provider *p = &a->llm_prov[idx];
        if (!p->url[0] || !p->model[0]) {
            np_toast(a, "服务商未配置完整",
                     "请在「LLM 服务」中填写服务地址并选择模型", CUI_TONE_WARNING);
            return;
        }
        a->ai_result[0] = 0; a->ai_error[0] = 0;
        a->ai_done = 1;
        NP_BEGIN_THREAD(np_ai_openai_worker, a);
        return;
    }

    /* Pick a default command template when the field is empty or the backend
     * selection changed. Users can edit it freely in the dialog. */
    if (!a->ai_cmd[0] || a->ai_backend < 3 || a->ai_backend > 4) {
        bool claude = np_ai_has_tool("claude");
        bool codex = np_ai_has_tool("codex");
        if (a->ai_backend == 1 || (a->ai_backend == 0 && claude))
            snprintf(a->ai_cmd, sizeof a->ai_cmd,
                     "claude -p \"Read {file} and follow its instructions. Output ONLY the result, no explanation.\" > {out}");
        else if (a->ai_backend == 2 || (a->ai_backend == 0 && codex))
            snprintf(a->ai_cmd, sizeof a->ai_cmd,
                     "codex exec \"Read {file} and follow its instructions. Output ONLY the result, no explanation.\" > {out}");
        else
            snprintf(a->ai_cmd, sizeof a->ai_cmd, "claude -p \"Read {file} and follow its instructions.\" > {out}");
    }

    const char *dir = (a->side_dir[0]) ? a->side_dir : ".";
#ifdef _WIN32
    snprintf(a->ai_ctx_path, sizeof a->ai_ctx_path, "%s\\rapideditor-ai-ctx.txt", dir);
    snprintf(a->ai_out_path, sizeof a->ai_out_path, "%s\\rapideditor-ai-out.txt", dir);
#else
    snprintf(a->ai_ctx_path, sizeof a->ai_ctx_path, "%s/rapideditor-ai-ctx.txt", dir);
    snprintf(a->ai_out_path, sizeof a->ai_out_path, "%s/rapideditor-ai-out.txt", dir);
#endif
    a->ai_result[0] = 0; a->ai_error[0] = 0;
    a->ai_done = 1;                              /* running */
    NP_BEGIN_THREAD(np_ai_worker, a);
}

/* Apply the AI rewrite: replace the captured range in the document the
 * request was made for. Identity is the stable uid, not the path (every
 * untitled tab has path="", so a path compare can write into the wrong tab).
 * The range is only valid if the document has not changed since capture;
 * otherwise refuse instead of silently corrupting text. */
static bool np_ai_apply(notepad *a) {
    if (a->ai_done != 2 || !a->ai_result[0]) return false;
    np_doc *d = NULL;
    for (int i = 0; i < a->count; i++) {
        if (a->docs[i] && a->docs[i]->uid == a->ai_target_uid) { d = a->docs[i]; break; }
    }
    if (!d || d != np_cur(a)) {
        np_toast(a, "文档已切换", "AI 结果对应的文档不再是当前标签，已取消写入", CUI_TONE_WARNING);
        return false;
    }
    if (d->is_hex) {
        /* 按轨路由：二进制改动不直接写，引导走 AI 工作台 → B3 审查 */
        np_toast(a, "二进制文档",
                 "字节改动请走「AI 工作台」→「送入补丁审查」，经人工确认后写入",
                 CUI_TONE_WARNING);
        return false;
    }
    if (d->content_rev != a->ai_target_rev || d->len != a->ai_target_len) {
        np_toast(a, "文档已改动", "生成结果后正文发生变化，为避免写错位置已取消；请重新生成", CUI_TONE_WARNING);
        return false;
    }
    int s0 = NP_MAX(0, NP_MIN(a->ai_target_s, d->len));
    int s1 = NP_MAX(s0, NP_MIN(a->ai_target_e, d->len));
    const char *src = np_ai_is_rewrite(a->ai_task) ? a->ai_staged : a->ai_result;
    if (!src[0]) src = a->ai_result;
    size_t rl = strlen(src);
    int len = d->len;
    int newlen = len - (s1 - s0) + (int)rl;
    if (!np_ensure_cap(d, (size_t)newlen + 1)) {
        np_toast(a, "文本已满", "无法写入 AI 结果", CUI_TONE_WARNING);
        return false;
    }
    /* Write through the editor so this becomes ONE undo step. A plain
     * memmove here leaves CUI's delta history holding stale byte offsets;
     * the next Ctrl+Z would then edit unrelated text. On failure the buffer
     * is untouched. */
    int applied_len = len;
    if (!cui_code_editor_replace_external(a->ui, d->editor_id,
                                          d->text, d->cap, d->len,
                                          s0, s1, src, (int)rl, &applied_len) ||
        applied_len != newlen) {
        np_toast(a, "文本已满", "无法写入 AI 结果", CUI_TONE_WARNING);
        return false;
    }
    d->len = applied_len;
    np_text_changed(a, d);
    int caret = s0 + (int)rl;
    if (caret > d->len) caret = d->len;
    cui_code_editor_select(a->ui, d->editor_id, caret, caret);
    return true;
}

static void np_start_template_download(notepad *a, const np_online_template *t) {
    if (a->tpl_dl_busy) return;
    char url[NP_PATH_CAP];
    np_online_zip_url(t, url, sizeof url);
    snprintf(a->tpl_dl_url, sizeof a->tpl_dl_url, "%s", url);
    /* Use the ASCII repo name ("tuna/thuthesis" -> "thuthesis") as the
     * project folder, never the Chinese display name. On Windows the LaTeX
     * compiler and ZIP writer take ANSI paths; a CJK folder breaks both. */
    {
        const char *repo = strrchr(t->full_name, '/');
        repo = repo ? repo + 1 : t->full_name;
        if (!repo[0]) repo = t->name;
        snprintf(a->tpl_dl_stem, sizeof a->tpl_dl_stem, "%s", repo);
    }
    /* temp zip under the config/side directory */
    char dir[NP_PATH_CAP];
    if (a->side_dir[0]) np_dir_of(a->side_dir, dir, sizeof dir);
    else snprintf(dir, sizeof dir, ".");
#ifdef _WIN32
    snprintf(a->tpl_dl_result, sizeof a->tpl_dl_result, "%s\\rapideditor-template.zip", dir);
#else
    snprintf(a->tpl_dl_result, sizeof a->tpl_dl_result, "%s/rapideditor-template.zip", dir);
#endif
    a->tpl_cancel = false;
    a->tpl_progress = 0;
    a->tpl_dl_done = 0;
    a->tpl_dl_error[0] = 0;
    a->tpl_dl_kind = 1;                 /* download */
    a->tpl_dl_busy = true;
    snprintf(a->tpl_status, sizeof a->tpl_status, "正在下载 %s…", t->name);
    NP_BEGIN_THREAD(np_tpl_download_worker, a);
}

static void np_start_tpl_search(notepad *a) {
    if (a->tpl_dl_busy || !a->tpl_query[0]) return;
    a->tpl_cancel = false;
    a->tpl_progress = 0;
    a->tpl_dl_done = 0;
    a->tpl_dl_kind = 2;                 /* search */
    a->tpl_dl_busy = true;
    snprintf(a->tpl_status, sizeof a->tpl_status, "正在搜索 GitHub…");
    NP_BEGIN_THREAD(np_tpl_search_worker, a);
}

static void np_start_tinytex_install(notepad *a) {
    if (a->tt_busy) return;
    a->tt_cancel = false;
    a->tt_progress = 0;
    a->tt_busy = true;
    np_tinytex_reset();
    snprintf(a->tt_status, sizeof a->tt_status, "准备安装…");
    NP_BEGIN_THREAD(np_tt_install_worker, a);
}

static void np_init_side_dir(notepad *a) {
    if (a->side_dir[0]) {               /* restored from the config file */
        a->side_reload = true;
        return;
    }
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

/* ------------------------------------------------------------------ */
/* EditPlus-style additions: find in files, text transforms, cliptext,  */
/* config persistence, backup and "open with system"                    */
/* ------------------------------------------------------------------ */

enum { TR_UPPER = 0, TR_LOWER, TR_SORT, TR_UNIQ, TR_STRIP, TR_NOEMPTY,
       TR_TAB2SP, TR_SP2TAB, TR_JOIN };

static bool np_file_exists(const char *path) {
#ifdef _WIN32
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048) <= 0) return false;
    struct _stat st;
    return _wstat(w, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

/* ---- sidebar directory tree (UI_PLAN P2 #6) ------------------------ */
/* A lazily-loaded tree over a fixed node pool. Children of a directory are
 * scanned once, when the directory is first expanded; expansion state is
 * remembered by path, so a rebuild (root change, refresh, file op) restores
 * every open node without rescanning the disk. The tree only ever draws
 * nodes of open ancestors, so an idle frame costs nothing. */

enum { NP_TREE_MAX = 3072, NP_TREE_CHILD_CAP = 400, NP_TREE_DEPTH = 24,
       NP_OPENSET_MAX = 160, NP_TREE_SELCAP = 320 };

typedef struct np_tnode {
    const char *name;           /* one path segment, into np_tree_names */
    int parent, child, next;    /* pool indices; -1 = none */
    bool dir, loaded, open;
} np_tnode;

static np_tnode np_tree[NP_TREE_MAX];
static int      np_tree_count;
static char     np_tree_names[NP_TREE_MAX * 80];
static size_t   np_tree_nused;
static char     np_tree_root[NP_PATH_CAP];      /* the dir the pool was built for */
static char     np_tree_sel_rel[NP_TREE_SELCAP];/* selected node, path relative to the root */
static char     np_reveal_pending[NP_PATH_CAP]; /* path to expand + select next frame */

/* Expansion memory, relative paths under np_tree_root; cleared on a root change. */
static char np_open_root[NP_PATH_CAP];
static char np_open_set[NP_OPENSET_MAX][NP_TREE_SELCAP];
static int  np_open_count;

static const char *np_tree_name_add(const char *name) {
    size_t n = strlen(name) + 1;
    if (np_tree_nused + n > sizeof np_tree_names) return "?";
    char *p = np_tree_names + np_tree_nused;
    memcpy(p, name, n);
    np_tree_nused += n;
    return p;
}

/* Path of a node: with_root gives the absolute path, otherwise relative to
 * the tree root (the root's own relative path is the empty string). */
static void np_tree_path_ex(int idx, char *out, size_t cap, bool with_root) {
    const char *segs[NP_TREE_DEPTH + 8];
    int n = 0;
    for (int i = idx; i > 0 && n < (int)(sizeof segs / sizeof segs[0]);
         i = np_tree[i].parent)
        segs[n++] = np_tree[i].name;
    out[0] = 0;
    if (with_root) snprintf(out, cap, "%s", np_tree_root);
    size_t used = strlen(out);
    for (int i = n - 1; i >= 0; i--) {
        size_t l = strlen(segs[i]);
        if (used + l + 2 >= cap) break;
        if (used && out[used - 1] != '/' && out[used - 1] != '\\')
            out[used++] = '/';
        memcpy(out + used, segs[i], l + 1);
        used += l;
    }
}

static void np_tree_path(int idx, char *out, size_t cap) {
    np_tree_path_ex(idx, out, cap, true);
}

static void np_tree_rel(int idx, char *out, size_t cap) {
    np_tree_path_ex(idx, out, cap, false);
}

static void np_tree_sel_set(int idx) {
    np_tree_rel(idx, np_tree_sel_rel, sizeof np_tree_sel_rel);
}

static bool np_tree_sel_is(int idx) {
    if (!np_tree_sel_rel[0]) return false;
    char rel[NP_TREE_SELCAP];
    np_tree_rel(idx, rel, sizeof rel);
    return strcmp(rel, np_tree_sel_rel) == 0;
}

/* Expansion-memory primitives. Paths are stored with '/' separators. */
static void np_tree_norm_rel(char *s) {
    for (; *s; s++) if (*s == '\\') *s = '/';
}

static bool np_tree_open_has(const char *rel) {
    for (int i = 0; i < np_open_count; i++)
        if (strcmp(np_open_set[i], rel) == 0) return true;
    return false;
}

static void np_tree_open_add(const char *rel) {
    if (np_tree_open_has(rel)) return;
    if (np_open_count < NP_OPENSET_MAX)
        snprintf(np_open_set[np_open_count++], NP_TREE_SELCAP, "%s", rel);
}

static void np_tree_open_del(const char *rel) {
    for (int i = 0; i < np_open_count; i++)
        if (strcmp(np_open_set[i], rel) == 0) {
            memmove(np_open_set[i], np_open_set[i + 1],
                    (size_t)(np_open_count - i - 1) * NP_TREE_SELCAP);
            np_open_count--;
            return;
        }
}

/* Scan one directory into the pool; runs once per directory per build. */
static void np_tree_load(int idx) {
    np_tnode *n = &np_tree[idx];
    if (n->loaded || !n->dir) return;
    n->loaded = true;
    static cui_file_entry ents[NP_TREE_CHILD_CAP];      /* scratch, UI thread only */
    static char names[NP_TREE_CHILD_CAP * 96];
    char full[NP_PATH_CAP], prel[NP_TREE_SELCAP];
    np_tree_path(idx, full, sizeof full);
    np_tree_rel(idx, prel, sizeof prel);
    int cnt = cui_fs_list(NULL, full, ents, NP_TREE_CHILD_CAP,
                          names, sizeof names, false);
    if (cnt < 0) return;                                /* unreadable: no children */
    if (cnt > 1) qsort(ents, (size_t)cnt, sizeof ents[0], np_dir_cmp);
    int last = -1;
    for (int i = 0; i < cnt && np_tree_count < NP_TREE_MAX; i++) {
        int c = np_tree_count++;
        char crel[NP_TREE_SELCAP];
        snprintf(crel, sizeof crel, "%s%s%s", prel,
                 prel[0] && prel[strlen(prel) - 1] != '/' ? "/" : "", ents[i].name);
        np_tree[c] = (np_tnode){ .name = np_tree_name_add(ents[i].name),
                                 .parent = idx, .child = -1, .next = -1,
                                 .dir = ents[i].dir,
                                 .open = ents[i].dir && np_tree_open_has(crel) };
        if (last < 0) np_tree[idx].child = c;
        else np_tree[last].next = c;
        last = c;
    }
}

/* Rebuild the pool for a->side_dir. A root change resets expansion memory. */
static void np_tree_build(notepad *a) {
    if (strcmp(np_open_root, a->side_dir) != 0) {
        snprintf(np_open_root, sizeof np_open_root, "%s", a->side_dir);
        np_open_count = 0;
        np_tree_sel_rel[0] = 0;
    }
    snprintf(np_tree_root, sizeof np_tree_root, "%s", a->side_dir);
    np_tree_count = 0;
    np_tree_nused = 0;
    const char *nm = np_base(a->side_dir);
    if (!nm[0] || nm[strlen(nm) - 1] == '/' || nm[strlen(nm) - 1] == '\\')
        nm = a->side_dir;               /* "C:\" and kin have no usable base name */
    np_tree[0] = (np_tnode){ .name = np_tree_name_add(nm),
                             .parent = -1, .child = -1, .next = -1,
                             .dir = true, .loaded = false, .open = true };
    np_tree_count = 1;
    np_tree_open_add("");
}

/* Expand the ancestors of `full` (already saved on disk) and select it. */
static void np_tree_reveal(int idx_root, const char *full) {
    size_t rl = strlen(np_tree_root);
    if (!rl || strncmp(full, np_tree_root, rl) != 0) return;
    const char *q = full + rl;
    if (*q == '/' || *q == '\\') q++;
    int node = idx_root;
    while (*q) {
        const char *e = q;
        while (*e && *e != '/' && *e != '\\') e++;
        char seg[NP_TREE_SELCAP];
        size_t l = (size_t)(e - q);
        if (l == 0 || l >= sizeof seg) break;
        memcpy(seg, q, l); seg[l] = 0;
        if (!np_tree[node].loaded) np_tree_load(node);
        int c = np_tree[node].child, found = -1;
        for (; c >= 0; c = np_tree[c].next)
            if (strcmp(np_tree[c].name, seg) == 0) { found = c; break; }
        if (found < 0) break;           /* not on disk (yet): select nothing */
        if (np_tree[found].dir && !np_tree[found].open) {
            np_tree[found].open = true;
            np_tree_rel(found, seg, sizeof seg);
            np_tree_open_add(seg);
        }
        node = found;
        q = *e ? e + 1 : e;
    }
    if (node != idx_root) np_tree_sel_set(node);
}

static void np_tree_queue_reveal(const char *path) {
    snprintf(np_reveal_pending, sizeof np_reveal_pending, "%s", path);
}

/* File ops behind the tree's context menu. UTF-8 safe on Windows. */
static bool np_fs_mkdir(const char *path) {
#ifdef _WIN32
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048) <= 0) return false;
    return _wmkdir(w) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

static bool np_fs_touch(const char *path) {
#ifdef _WIN32
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048) <= 0) return false;
    FILE *f = _wfopen(w, L"ab");        /* fresh file: zero bytes */
#else
    FILE *f = fopen(path, "ab");
#endif
    if (!f) return false;
    fclose(f);
    return true;
}

static bool np_fs_rename(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t a[2048], b[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, from, -1, a, 2048) <= 0) return false;
    if (MultiByteToWideChar(CP_UTF8, 0, to, -1, b, 2048) <= 0) return false;
    return _wrename(a, b) == 0;
#else
    return rename(from, to) == 0;
#endif
}

/* Confirm button / Enter of the tree's new-file, new-folder and rename dialog. */
static void np_tree_confirm(notepad *a) {
    char path[NP_PATH_CAP];
    if (np_tree_op == NP_TREE_OP_RENAME) {
        np_dir_of(np_tree_op_path, path, sizeof path);
        np_dir_join(path, sizeof path, path, np_tree_op_name);
        if (!strcmp(path, np_tree_op_path)) goto done;
        if (np_file_exists(path)) {
            np_toast(a, "无法重命名", "目标名称已存在", CUI_TONE_DANGER);
            return;
        }
        if (!np_fs_rename(np_tree_op_path, path)) {
            np_toast(a, "重命名失败", "文件可能被其他程序占用", CUI_TONE_DANGER);
            return;
        }
        for (int i = 0; i < a->count; i++)      /* keep open docs on the moved file */
            if (!strcmp(a->docs[i]->path, np_tree_op_path))
                snprintf(a->docs[i]->path, NP_PATH_CAP, "%s", path);
        np_toast(a, "已重命名", np_tree_op_name, CUI_TONE_SUCCESS);
        np_logf(a, "重命名  %s -> %s", np_tree_op_path, path);
    } else {
        np_dir_join(path, sizeof path, np_tree_op_path, np_tree_op_name);
        if (np_file_exists(path)) {
            np_toast(a, "无法创建", "同名文件或文件夹已存在", CUI_TONE_DANGER);
            return;
        }
        bool ok = np_tree_op == NP_TREE_OP_NEWDIR
                      ? np_fs_mkdir(path) : np_fs_touch(path);
        if (!ok) {
            np_toast(a, "创建失败", "无法写入目标目录", CUI_TONE_DANGER);
            return;
        }
        np_toast(a, np_tree_op == NP_TREE_OP_NEWDIR ? "已新建文件夹" : "已新建文件",
                 path, CUI_TONE_SUCCESS);
        np_logf(a, "%s  %s",
                np_tree_op == NP_TREE_OP_NEWDIR ? "新建文件夹" : "新建文件", path);
    }
done:
    np_tree_op_open = false;
    np_tree_op = NP_TREE_OP_NONE;
    a->side_reload = true;
    np_tree_queue_reveal(path);
}

/* Depth-first draw of one node; recursion only reaches open ancestors. */
static void np_tree_draw_node(cui_ctx *ui, notepad *a, int idx, int depth) {
    np_tnode *n = &np_tree[idx];
    if (n->dir && n->open && !n->loaded) np_tree_load(idx);
    char label[NP_PATH_CAP + 16], full[NP_PATH_CAP];
    snprintf(label, sizeof label, "%s###t%d", n->name, idx);
    np_tree_path(idx, full, sizeof full);
    int fk = n->dir ? NP_FK_TEXT : np_file_family(full);
    if (depth >= NP_TREE_DEPTH) n->dir = false;     /* guard runaway recursion */
    cui_tree_opts o = { .leaf = !n->dir,
                        .icon = n->dir ? NULL : np_family_icon(fk),
                        .icon_color = np_family_tint(ui, fk),
                        .selected = np_tree_sel_is(idx) };
    bool expanded = cui_tree_begin(ui, label, n->dir ? &n->open : NULL, &o);
    if (cui_tree_clicked(ui)) {
        np_tree_sel_set(idx);
        if (!n->dir) {
            np_open_path(a, full, true);
        } else if (n->open) {
            char rel[NP_TREE_SELCAP];
            np_tree_rel(idx, rel, sizeof rel);
            np_tree_open_add(rel);
        } else {
            char rel[NP_TREE_SELCAP];
            np_tree_rel(idx, rel, sizeof rel);
            np_tree_open_del(rel);
        }
    }
    /* Context menu binds to the tree row created above. */
    cui_context_menu(ui, "side-tree-ctx") {
        char dir[NP_PATH_CAP];
        if (n->dir) snprintf(dir, sizeof dir, "%s", full);
        else np_dir_of(full, dir, sizeof dir);
        if (cui_menu_item(ui, "新建文件", .icon = CUI_ICON_FILE_TEXT)) {
            np_tree_op = NP_TREE_OP_NEWFILE;
            snprintf(np_tree_op_path, sizeof np_tree_op_path, "%s", dir);
            np_tree_op_name[0] = 0;
            np_tree_op_open = true;
        }
        if (cui_menu_item(ui, "新建文件夹", .icon = CUI_ICON_FOLDER)) {
            np_tree_op = NP_TREE_OP_NEWDIR;
            snprintf(np_tree_op_path, sizeof np_tree_op_path, "%s", dir);
            np_tree_op_name[0] = 0;
            np_tree_op_open = true;
        }
        if (cui_menu_item(ui, "重命名", .icon = CUI_ICON_PENCIL,
                          .disabled = idx == 0)) {
            np_tree_op = NP_TREE_OP_RENAME;
            snprintf(np_tree_op_path, sizeof np_tree_op_path, "%s", full);
            snprintf(np_tree_op_name, sizeof np_tree_op_name, "%s", n->name);
            np_tree_op_open = true;
        }
        cui_menu_separator(ui);
        if (cui_menu_item(ui, "在系统中打开", .icon = CUI_ICON_EXTERNAL_LINK))
            np_system_open(a, full, false);
        if (cui_menu_item(ui, "打开所在文件夹", .icon = CUI_ICON_FOLDER_OPEN))
            np_system_open(a, full, true);
    }
    if (n->dir && expanded) {
        for (int c = n->child; c >= 0; c = np_tree[c].next)
            np_tree_draw_node(ui, a, c, depth + 1);
        cui_tree_end(ui);
    }
}

/* ---- find in files ------------------------------------------------ */

/* A single "pat" with '*' wildcards, case-insensitive. */
static bool np_wild(const char *pat, const char *s) {
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*pat == '*') { star = ++pat; mark = s; }
        else if (*pat && tolower((unsigned char)*pat) == tolower((unsigned char)*s)) {
            pat++; s++;
        } else if (star) {
            pat = star; s = ++mark;
        } else return false;
    }
    while (*pat == '*') pat++;
    return !*pat;
}

/* ';'-separated patterns like "*.c;*.h". Empty means "every file". */
static bool np_pattern_match(const char *pats, const char *name) {
    if (!pats[0]) return true;
    char buf[128];
    const char *p = pats;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l < sizeof buf) {
            memcpy(buf, p, l); buf[l] = 0;
            if (np_wild(buf, name)) return true;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* ---- .aiignore ----------------------------------------------------- */
/* A small ignore file in the scanned workspace root keeps private files out
 * of AI-bound scans (semantic search, find-in-files) and out of the opt-in
 * "other tabs" context. Supported syntax: # comments, a trailing / to mark a
 * directory-only rule, *.ext, and * / ? wildcards. A bare name matches at any
 * depth, which is what most people expect from a compact ignore file. */
#define NP_IGNORE_MAX 160
typedef struct {
    char pat[256];
    bool dir_only;
} np_ignore_rule;

static np_ignore_rule ai_ignore[NP_IGNORE_MAX];
static int ai_ignore_count;
static char ai_ignore_root[NP_PATH_CAP];
static time_t ai_ignore_mtime;          /* reload if .aiignore is edited in place */

/* Normalize path separators to '/' and lowercase so Windows and POSIX scans
 * match the same rules; rules are written with '/' in .aiignore by convention. */
static void np_ignore_norm(char *s) {
    for (; *s; s++) {
        if (*s == '\\') *s = '/';
        else *s = (char)tolower((unsigned char)*s);
    }
}

static const char *np_rel_path(const char *root, const char *full) {
    size_t n = root ? strlen(root) : 0;
    if (n && full && !strncmp(full, root, n) &&
        (full[n] == '/' || full[n] == '\\')) {
        const char *p = full + n;
        while (*p == '/' || *p == '\\') p++;
        return p;
    }
    return full ? full : "";
}

static void np_ignore_load(const char *root) {
    if (!root || !*root) root = ".";
    char path[NP_PATH_CAP];
    np_dir_join(path, sizeof path, root, ".aiignore");
    struct stat st;
    time_t mt = stat(path, &st) == 0 ? st.st_mtime : 0;
    /* Reload when the workspace changes OR the ignore file is edited. */
    if (ai_ignore_root[0] && !strcmp(ai_ignore_root, root) &&
        ai_ignore_mtime == mt) return;
    snprintf(ai_ignore_root, sizeof ai_ignore_root, "%s", root);
    ai_ignore_mtime = mt;
    ai_ignore_count = 0;
    FILE *f = np_fopen_utf8(path, "rb");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        while (n && (line[0] == ' ' || line[0] == '\t')) {
            memmove(line, line + 1, n);
            n--;
        }
        if (!n || line[0] == '#') continue;
        bool dir_only = false;
        if (line[n - 1] == '/') { line[--n] = 0; dir_only = true; }
        if (!n || ai_ignore_count >= NP_IGNORE_MAX) continue;
        np_ignore_rule *r = &ai_ignore[ai_ignore_count++];
        snprintf(r->pat, sizeof r->pat, "%s", line);
        np_ignore_norm(r->pat);
        r->dir_only = dir_only;
    }
    fclose(f);
}

static bool np_ignore_check(const char *root, const char *dir,
                            const char *name, bool is_dir) {
    if (!ai_ignore_count || !name) return false;
    char rel[NP_PATH_CAP];
    const char *rd = np_rel_path(root, dir ? dir : "");
    if (rd[0]) snprintf(rel, sizeof rel, "%s/%s", rd, name);
    else snprintf(rel, sizeof rel, "%s", name);
    np_ignore_norm(rel);
    char lname[NP_PATH_CAP];
    snprintf(lname, sizeof lname, "%s", name);
    np_ignore_norm(lname);

    for (int i = 0; i < ai_ignore_count; i++) {
        const np_ignore_rule *r = &ai_ignore[i];
        if (r->dir_only && !is_dir) continue;
        const char *p = r->pat;
        if (p[0] == '*' && p[1] == '.') {
            size_t ext = strlen(p + 1), nl = strlen(lname);
            if (nl >= ext && !strcmp(lname + nl - ext, p + 1)) return true;
        } else if (strchr(p, '/')) {
            size_t pl = strlen(p), rl = strlen(rel);
            if (!strcmp(rel, p)) return true;
            if (rl > pl && rel[rl - pl - 1] == '/' && !strcmp(rel + rl - pl, p)) return true;
        } else if (strchr(p, '*') || strchr(p, '?')) {
            /* np_wild is already case-insensitive; rules are lowercased. */
            if (np_wild(p, lname) || np_wild(p, rel)) return true;
        } else if (!strcmp(lname, p)) {
            return true;
        }
    }
    return false;
}

/* Directories whose contents are skipped during a find-in-files sweep. */
static const char *NP_FF_SKIP[] = {
    ".git", ".svn", ".hg", ".idea", ".vs", "node_modules", "__pycache__",
    ".venv", "venv", "build", "dist", "target", "Debug", "Release", NULL
};

/* Scan one file for the current query, appending hits (one per line). */
static void np_ff_scan_file(notepad *a, const char *dir, const char *name) {
    static char *ffbuf;                 /* reused across calls */
    if (!ffbuf) {
        ffbuf = (char *)malloc(NP_FF_BYTES + 1);
        if (!ffbuf) return;
    }
    char full[NP_PATH_CAP];
    np_dir_join(full, sizeof full, dir, name);
    FILE *f = np_fopen_utf8(full, "rb");
    if (!f) return;
    size_t n = fread(ffbuf, 1, NP_FF_BYTES, f);
    fclose(f);
    ffbuf[n] = 0;
    if (memchr(ffbuf, 0, n)) return;    /* binary: skip silently */

    size_t qlen = strlen(a->ff_query);
    int file_idx = -1;
    bool overflow = false;
    int line = 1, hit_line = 0;
    size_t ls = 0;
    for (size_t i = 0; i + qlen <= n; i++) {
        if (np_match(ffbuf + i, a->ff_query, qlen, a->match_case) &&
            np_word_edge(ffbuf, n, i, qlen, a->find_word) && line != hit_line) {
            hit_line = line;
            if (file_idx < 0) {
                if (a->ff_count < NP_FF_FILE_MAX) {
                    file_idx = a->ff_count++;
                    snprintf(a->ff_paths[file_idx], NP_PATH_CAP, "%s", full);
                } else {
                    overflow = true;
                }
            }
            if (!overflow && a->ff_hit_count < NP_FF_MAX) {
                np_fhit *h = &a->ff_hits[a->ff_hit_count++];
                h->file = file_idx;
                h->line = line;
                h->col = (int)(i - ls) + 1;
                size_t s2 = ls;
                while (s2 < i && (ffbuf[s2] == ' ' || ffbuf[s2] == '\t')) s2++;
                size_t e2 = s2;
                while (e2 < n && ffbuf[e2] != '\n' && e2 - s2 < 140) e2++;
                while (e2 > s2 && ((unsigned char)ffbuf[e2 - 1] & 0xC0) == 0x80) e2--;
                snprintf(h->text, sizeof h->text, "%.*s", (int)(e2 - s2), ffbuf + s2);
            }
        }
        if (ffbuf[i] == '\n') { line++; ls = i + 1; hit_line = 0; }
        if (a->ff_hit_count >= NP_FF_MAX) break;
    }
}

/* Breadth-first sweep of ff_dir. Stops at the result/file ceilings. */
static void np_ff_run(notepad *a) {
    a->ff_count = 0;
    a->ff_hit_count = 0;
    a->ff_files_seen = 0;
    a->ff_dirs_seen = 0;
    if (!a->ff_query[0]) return;
    const char *root = a->ff_dir[0] ? a->ff_dir : ".";
    np_ignore_load(root);

    char dirs[NP_FF_DIR_MAX][NP_PATH_CAP];
    snprintf(dirs[0], NP_PATH_CAP, "%s", root);
    int nd = 1, head = 0;

    while (head < nd && a->ff_hit_count < NP_FF_MAX &&
           a->ff_files_seen < NP_FF_SCAN_MAX) {
        char dir[NP_PATH_CAP];
        snprintf(dir, sizeof dir, "%s", dirs[head++]);
        a->ff_dirs_seen++;

        cui_file_entry ents[NP_FF_ROW_MAX];
        static char names[NP_FF_ROW_MAX * 256];
        int n = cui_fs_list(NULL, dir, ents, NP_FF_ROW_MAX,
                            names, sizeof names, false);
        if (n < 0) continue;
        for (int i = 0; i < n; i++) {
            if (a->ff_hit_count >= NP_FF_MAX ||
                a->ff_files_seen >= NP_FF_SCAN_MAX) break;
            if (ents[i].dir) {
                bool skip = false;
                for (int k = 0; NP_FF_SKIP[k]; k++)
                    if (strcmp(ents[i].name, NP_FF_SKIP[k]) == 0) { skip = true; break; }
                if (skip || np_ignore_check(root, dir, ents[i].name, true)) continue;
                if (nd < NP_FF_DIR_MAX)
                    np_dir_join(dirs[nd++], NP_PATH_CAP, dir, ents[i].name);
                continue;
            }
            if (np_ignore_check(root, dir, ents[i].name, false)) continue;
            if (!np_pattern_match(a->ff_pattern, ents[i].name)) continue;
            a->ff_files_seen++;
            np_ff_scan_file(a, dir, ents[i].name);
            if (a->ff_hit_count >= NP_FF_MAX) break;
        }
    }
}

/* Jump to a find-in-files result: open the file and go to the line. */
static void np_open_fhit(notepad *a, int idx) {
    if (idx < 0 || idx >= a->ff_hit_count) return;
    np_fhit *h = &a->ff_hits[idx];
    if (h->file < 0 || h->file >= a->ff_count) return;
    if (!np_open_path(a, a->ff_paths[h->file], true)) return;
    np_doc *d = np_cur(a);
    if (d) cui_code_editor_goto(a->ui, d->editor_id, h->line, h->col);
}

/* ---- AI semantic workspace search --------------------------------- */
/* Collect a bounded file list using the same BFS rules as find-in-files,
 * then build a prompt that shows every candidate by number with a short
 * content excerpt. The AI only returns numbers + line numbers; the source
 * snippet shown to the user is always read back from the real file, so a
 * hallucinated snippet can never end up in the results. */
static void np_sem_build_prompt(notepad *a) {
    a->ff_count = 0;
    a->ff_files_seen = 0;
    a->ff_dirs_seen = 0;
    a->ff_hit_count = 0;
    a->sem_prompt[0] = 0;
    if (!a->ff_query[0]) return;
    const char *root = a->ff_dir[0] ? a->ff_dir : ".";
    np_ignore_load(root);

    size_t po;
    if (a->sem_mode == 1) {
        po = np_ctx_app(a->sem_prompt, sizeof a->sem_prompt, 0,
            "你在帮用户分析他的工作区。用户的问题：%s\n"
            "下面是工作区里的文本文件，每个都有编号、路径和开头几行内容。\n"
            "请只依据这些内容回答问题，不要编造。回答中引用文件时用 [编号] 标记。\n"
            "回答写完后，另起一行输出「引用：」，之后每行一条引用，格式严格为：\n"
            "编号|行号|一句话理由\n"
            "编号必须取自清单，行号是最能支撑该引用的行（不确定填 1），最多 8 条。\n"
            "没有相关信息就回答：未在工作区找到相关信息。\n\n文件清单：\n",
            a->ff_query);
    } else {
        po = np_ctx_app(a->sem_prompt, sizeof a->sem_prompt, 0,
            "你在帮用户做代码与文档检索。用户想找：%s\n"
            "下面是工作区里的文本文件，每个都有编号、路径和开头几行内容。\n"
            "请选出与用户需求最相关的文件，最多 12 个。严格按每行一条输出，格式：\n"
            "编号|行号|一句话理由\n"
            "编号必须取自清单，行号填最相关的代码所在行（不确定就填 1）。\n"
            "不要输出标题、代码块或其它文字。没有相关文件就输出：无\n\n文件清单：\n",
            a->ff_query);
    }

    char dirs[NP_FF_DIR_MAX][NP_PATH_CAP];
    snprintf(dirs[0], NP_PATH_CAP, "%s", root);
    int nd = 1, head = 0;
    while (head < nd && a->ff_files_seen < NP_FF_SCAN_MAX &&
           a->ff_count < NP_FF_FILE_MAX && po < sizeof(a->sem_prompt) - 512) {
        char dir[NP_PATH_CAP];
        snprintf(dir, sizeof dir, "%s", dirs[head++]);
        a->ff_dirs_seen++;

        cui_file_entry ents[NP_FF_ROW_MAX];
        static char names[NP_FF_ROW_MAX * 256];
        int n = cui_fs_list(NULL, dir, ents, NP_FF_ROW_MAX,
                            names, sizeof names, false);
        if (n < 0) continue;
        for (int i = 0; i < n; i++) {
            if (a->ff_count >= NP_FF_FILE_MAX ||
                a->ff_files_seen >= NP_FF_SCAN_MAX) break;
            if (ents[i].dir) {
                bool skip = false;
                for (int k = 0; NP_FF_SKIP[k]; k++)
                    if (strcmp(ents[i].name, NP_FF_SKIP[k]) == 0) { skip = true; break; }
                if (skip || np_ignore_check(root, dir, ents[i].name, true)) continue;
                if (nd < NP_FF_DIR_MAX)
                    np_dir_join(dirs[nd++], NP_PATH_CAP, dir, ents[i].name);
                continue;
            }
            if (np_ignore_check(root, dir, ents[i].name, false)) continue;
            if (!np_pattern_match(a->ff_pattern, ents[i].name)) continue;
            a->ff_files_seen++;
            char full[NP_PATH_CAP];
            np_dir_join(full, sizeof full, dir, ents[i].name);
            FILE *f = np_fopen_utf8(full, "rb");
            if (!f) continue;
            char buf[8192];
            size_t nb = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[nb] = 0;
            if (memchr(buf, 0, nb)) continue;      /* binary: skip silently */

            int idx = a->ff_count++;
            snprintf(a->ff_paths[idx], NP_PATH_CAP, "%s", full);
            po = np_ctx_app(a->sem_prompt, sizeof a->sem_prompt, po,
                            "[%d] %s\n", idx + 1, a->ff_paths[idx]);
            int shown = 0;
            char *p = buf;
            while (*p && shown < 6 && po < sizeof(a->sem_prompt) - 256) {
                char *e = strchr(p, '\n');
                if (e) *e = 0;
                char *s = p;
                while (*s == ' ' || *s == '\t' || *s == '\r') s++;
                if (*s && s[0] != '\n') {
                    if (strlen(s) > 160) s[160] = 0;
                    po = np_ctx_app(a->sem_prompt, sizeof a->sem_prompt, po,
                                    "    %s\n", s);
                    shown++;
                }
                if (!e) break;
                p = e + 1;
            }
            po = np_ctx_app(a->sem_prompt, sizeof a->sem_prompt, po, "\n");
        }
    }
    /* Freeze the candidate table for result parsing (ff_paths is shared with
     * keyword search and may be rebuilt before this worker finishes). */
    a->sem_count = a->ff_count;
    for (int i = 0; i < a->ff_count && i < NP_FF_FILE_MAX; i++)
        snprintf(a->sem_paths[i], NP_PATH_CAP, "%s", a->ff_paths[i]);
    snprintf(a->sem_summary, sizeof a->sem_summary,
             "扫描 %d 个目录 / %d 个文件，送入 %d 个候选文件%s",
             a->ff_dirs_seen, a->ff_files_seen, a->ff_count,
             ai_ignore_count ? " · .aiignore 已生效" : "");
}

/* Read the real text of line N from a candidate file (1-based). */
static void np_sem_line_text(const char *path, int line, char *out, size_t cap) {
    out[0] = 0;
    if (!path[0] || line < 1) return;
    static char *buf;                   /* reused, UI thread only */
    if (!buf) buf = (char *)malloc(2 * 1024 * 1024 + 1);
    if (!buf) return;
    FILE *f = np_fopen_utf8(path, "rb");
    if (!f) return;
    size_t nb = fread(buf, 1, 2 * 1024 * 1024, f);
    fclose(f);
    buf[nb] = 0;
    if (memchr(buf, 0, nb)) return;      /* binary */
    int cur = 1;
    char *p = buf;
    while (*p && cur < line) { if (*p == '\n') cur++; p++; }
    if (cur != line) return;
    char *e = strchr(p, '\n');
    if (e) *e = 0;
    size_t s = 0;
    while (p[s] == ' ' || p[s] == '\t' || p[s] == '\r') s++;
    snprintf(out, cap, "%s", p + s);
}

/* Semantic search worker: three backends, no CUI calls. */
static void np_sem_worker(void *arg) {
    notepad *a = (notepad *)arg;
    bool ok = false;
    if (a->sem_backend == 4) {
        if (!a->maclaw_token[0]) {
            snprintf(a->sem_error, sizeof a->sem_error, "请先登录 MaClaw");
            goto done;
        }
        ok = np_maclaw_llm_chat(a->maclaw_hub, a->maclaw_token, a->sem_prompt,
                                a->sem_result, sizeof a->sem_result,
                                a->sem_error, sizeof a->sem_error);
    } else if (a->sem_backend >= 5) {
        const np_llm_provider *p = &a->sem_prov;
        np_openai_set_params(p->temperature, p->max_tokens);
        ok = np_openai_chat(p->url, p->api_key, p->model, a->sem_prompt,
                            a->sem_result, sizeof a->sem_result,
                            a->sem_error, sizeof a->sem_error);
    } else {
        if (!np_ai_write_context(a->sem_ctx_path, a->sem_prompt)) {
            snprintf(a->sem_error, sizeof a->sem_error, "无法写入临时提示文件");
            goto done;
        }
        ok = np_ai_exec_cancel(a->ai_cmd, a->sem_ctx_path, a->sem_out_path, NULL,
                               &a->sem_cancel, a->sem_error, sizeof a->sem_error);
        if (ok && np_ai_read_result(a->sem_out_path, a->sem_result,
                                    sizeof a->sem_result) < 0) {
            snprintf(a->sem_error, sizeof a->sem_error, "无法读取 AI 输出");
            ok = false;
        }
    }
    if (a->sem_cancel && ok) {
        snprintf(a->sem_error, sizeof a->sem_error, "已停止");
        ok = false;
    }
done:
    a->sem_done = ok ? 2 : 3;
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

/* Parse "id|line|reason" lines into ff_hits. The displayed snippet comes
 * from np_sem_line_text(), never from the AI answer. */
static void np_sem_apply(notepad *a) {
    /* Rebuild the file table from the frozen snapshot and point ff_hits at it
     * so filenames, snippets and jump targets stay consistent even if a
     * keyword search ran while the AI worker was in flight. */
    a->ff_count = a->sem_count;
    for (int i = 0; i < a->sem_count && i < NP_FF_FILE_MAX; i++)
        snprintf(a->ff_paths[i], NP_PATH_CAP, "%s", a->sem_paths[i]);
    a->ff_hit_count = 0;
    a->sem_answer[0] = 0;
    size_t ao = 0;
    char *p = a->sem_result;
    while (*p && a->ff_hit_count < NP_FF_MAX) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *s = p;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        char *bar1 = strchr(s, '|');
        bool was_hit = false;
        if (bar1 && s != bar1) {
            int id = atoi(s);
            int line = atoi(bar1 + 1);
            if (id >= 1 && id <= a->sem_count && line >= 1) {
                np_fhit *h = &a->ff_hits[a->ff_hit_count++];
                h->file = id - 1;
                h->line = line;
                h->col = 1;
                np_sem_line_text(a->ff_paths[id - 1], line,
                                 h->text, sizeof h->text);
                if (!h->text[0])
                    snprintf(h->text, sizeof h->text, "%s（第 %d 行）",
                             np_base(a->ff_paths[id - 1]), line);
                was_hit = true;
            }
        }
        /* Q&A mode: keep every non-reference line as the answer body. */
        if (a->sem_mode == 1 && !was_hit && s[0] && ao < sizeof a->sem_answer - 2) {
            size_t sl = strlen(s);
            if (sl >= sizeof a->sem_answer - 1 - ao)
                sl = sizeof a->sem_answer - 1 - ao;
            memcpy(a->sem_answer + ao, s, sl);
            ao += sl;
            a->sem_answer[ao++] = '\n';
            a->sem_answer[ao] = 0;
        }
        if (!e) break;
        p = e + 1;
    }
    if (a->sem_mode == 1)
        snprintf(a->sem_summary, sizeof a->sem_summary,
                 "工作区问答 “%s”：%d 条引用（基于 %d 个候选文件）",
                 a->ff_query, a->ff_hit_count, a->sem_count);
    else
        snprintf(a->sem_summary, sizeof a->sem_summary,
                 "语义搜索 “%s”：%d 个相关位置（基于 %d 个候选文件）",
                 a->ff_query, a->ff_hit_count, a->sem_count);
    np_logf(a, "%s", a->sem_summary);
    np_toast(a, a->ff_hit_count ? "语义搜索完成" : "没有相关结果", a->sem_summary,
             a->ff_hit_count ? CUI_TONE_SUCCESS : CUI_TONE_WARNING);
}

/* UI entry: validate backend, snapshot config, launch the worker. */
static void np_sem_start(notepad *a) {
    if (a->sem_done == 1 || !a->ff_query[0]) return;
    np_sem_build_prompt(a);
    if (!a->ff_count) {
        np_toast(a, "没有可搜索的文件", "请检查目录和文件类型设置", CUI_TONE_WARNING);
        return;
    }
    int backend = a->ai_backend;
    if (backend == 4 && !a->maclaw_token[0]) {
        np_toast(a, "尚未登录 MaClaw", "请先在「LLM 服务」中扫码授权", CUI_TONE_WARNING);
        return;
    }
    if (backend >= 5) {
        int idx = backend - 5;
        if (idx < 0 || idx >= a->llm_prov_count ||
            !a->llm_prov[idx].url[0] || !a->llm_prov[idx].model[0]) {
            np_toast(a, "服务商未配置完整", "请在「LLM 服务」中设置地址和模型", CUI_TONE_WARNING);
            return;
        }
        a->sem_prov = a->llm_prov[idx];
    } else if (backend < 3 && !a->ai_cmd[0]) {
        bool claude = np_ai_has_tool("claude");
        bool codex = np_ai_has_tool("codex");
        if (!a->ai_cmd[0]) {
            if (claude)
                snprintf(a->ai_cmd, sizeof a->ai_cmd,
                         "claude -p \"Read {file} and follow its instructions. Output ONLY the result, no explanation.\" > {out}");
            else if (codex)
                snprintf(a->ai_cmd, sizeof a->ai_cmd,
                         "codex exec \"Read {file} and follow its instructions. Output ONLY the result, no explanation.\" > {out}");
        }
        if (!a->ai_cmd[0]) {
            np_toast(a, "未配置 AI 后端",
                     "请在「LLM 服务」中登录或添加服务商", CUI_TONE_WARNING);
            return;
        }
    }
    a->sem_backend = backend;
    a->sem_cancel = false;
    a->sem_result[0] = 0; a->sem_error[0] = 0;
    a->ff_hit_count = 0;
    const char *dir = a->side_dir[0] ? a->side_dir : ".";
#ifdef _WIN32
    snprintf(a->sem_ctx_path, sizeof a->sem_ctx_path, "%s\\rapideditor-sem-ctx.txt", dir);
    snprintf(a->sem_out_path, sizeof a->sem_out_path, "%s\\rapideditor-sem-out.txt", dir);
#else
    snprintf(a->sem_ctx_path, sizeof a->sem_ctx_path, "%s/rapideditor-sem-ctx.txt", dir);
    snprintf(a->sem_out_path, sizeof a->sem_out_path, "%s/rapideditor-sem-out.txt", dir);
#endif
    a->sem_done = 1;
    a->sem_last_state = 1;
    a->show_output = true;
    a->out_tab = 2;
    a->dlg_find_files = false;
    NP_BEGIN_THREAD(np_sem_worker, a);
    np_logf(a, "AI 语义搜索  “%s”  ·  %s", a->ff_query, a->sem_summary);
}

/* ---- text transforms ---------------------------------------------- */

/* Replace [s,e) with `ins` as ONE undoable editor step and place the caret.
 * Writing through cui_code_editor_replace_external keeps CUI's delta history
 * consistent; a plain memmove would leave stale byte offsets so the next
 * Ctrl+Z would edit unrelated text (same reasoning as np_ai_apply). */
static void np_splice(notepad *a, np_doc *d, int s, int e,
                      const char *ins, size_t n) {
    int len = d->len;
    s = NP_MAX(0, NP_MIN(s, len));
    e = NP_MAX(s, NP_MIN(e, len));
    if (!np_ensure_cap(d, (size_t)len - (size_t)(e - s) + n + 1)) {
        np_toast(a, "文本已满", "已达到 256 MB 上限", CUI_TONE_WARNING);
        return;
    }
    int applied_len = len;
    if (!cui_code_editor_replace_external(a->ui, d->editor_id,
                                          d->text, d->cap, d->len,
                                          s, e, ins, (int)n, &applied_len) ||
        applied_len != len - (e - s) + (int)n) {
        np_toast(a, "文本已满", "写入编辑器失败", CUI_TONE_WARNING);
        return;
    }
    d->len = applied_len;
    np_text_changed(a, d);
    cui_code_editor_select(a->ui, d->editor_id, s + (int)n, s + (int)n);
}

/* Current selection, if any. */
static bool np_selection(const np_doc *d, int *out_s, int *out_e) {
    if (d->caret == d->anchor) return false;
    int s = NP_MIN(d->caret, d->anchor), e = NP_MAX(d->caret, d->anchor);
    int len = d->len;
    s = NP_MAX(0, NP_MIN(s, len));
    e = NP_MAX(s, NP_MIN(e, len));
    if (s == e) return false;
    *out_s = s; *out_e = e;
    return true;
}

/* Whole lines covering the selection, or the caret's line. The range
 * always includes the trailing newline when the line has one. */
static bool np_line_range(const np_doc *d, int *out_s, int *out_e) {
    int len = d->len;
    int s, e;
    if (d->caret != d->anchor) {
        s = NP_MIN(d->caret, d->anchor);
        e = NP_MAX(d->caret, d->anchor);
        s = NP_MAX(0, NP_MIN(s, len));
        e = NP_MAX(s, NP_MIN(e, len));
    } else {
        s = e = NP_MAX(0, NP_MIN(d->caret, len));
    }
    while (s > 0 && d->text[s - 1] != '\n') s--;
    while (e < len && d->text[e] != '\n') e++;
    if (e < len) e++;
    if (s >= e) return false;
    *out_s = s; *out_e = e;
    return true;
}

static bool np_is_word(unsigned char c) {
    return c >= 0x80 || isalnum(c) || c == '_';
}

/* Uppercase / lowercase the selection, or the word under the caret. */
static void np_case_transform(notepad *a, np_doc *d, bool upper) {
    int s, e;
    if (!np_selection(d, &s, &e)) {
        int i = NP_MAX(0, NP_MIN(d->caret, d->len));
        s = i;
        while (s > 0 && np_is_word((unsigned char)d->text[s - 1])) s--;
        e = i;
        while (e < d->len && np_is_word((unsigned char)d->text[e])) e++;
        if (s == e) {
            np_toast(a, "没有可转换的文字", "选中一段文字，或把光标放在单词上",
                     CUI_TONE_INFO);
            return;
        }
    }
    if (!np_ensure_tmp(d, (size_t)(e - s) + 1)) return;
    for (int i = s; i < e; i++) {
        unsigned char c = (unsigned char)d->text[i];
        d->tmp[i - s] = c < 0x80 ? (char)(upper ? toupper(c) : tolower(c)) : (char)c;
    }
    np_splice(a, d, s, e, d->tmp, (size_t)(e - s));
}

static int np_row_cmp(const void *x, const void *y) {
    return strcmp(*(const char *const *)x, *(const char *const *)y);
}

/* Expand every tab in row to spaces up to the next tab stop. Columns are
 * counted in bytes, matching the editor's cui__ed_col semantics (UTF-8
 * multibyte chars advance one column per byte). Returns bytes written. */
static size_t np_row_tab2sp(const char *row, char *out, int tab) {
    size_t o = 0;
    int col = 0;
    for (const char *p = row; *p; p++) {
        if (*p == '\t') {
            int stop = (col / tab + 1) * tab;
            while (col < stop) { out[o++] = ' '; col++; }
        } else {
            out[o++] = *p;
            col++;
        }
    }
    return o;
}

/* Rewrite leading whitespace with tabs aligned to tab stops; spaces beyond
 * the body are left alone. A tab absorbs the pending partial run of spaces
 * before it (same final alignment), trailing runs shorter than a stop are
 * re-emitted as spaces. Returns bytes written. */
static size_t np_row_sp2tab(const char *row, char *out, int tab) {
    size_t o = 0;
    int col = 0, pend = 0;
    const char *p = row;
    while (*p == ' ' || *p == '\t') {
        if (*p == ' ') {
            col++;
            if (++pend == tab) { out[o++] = '\t'; pend = 0; }
        } else {
            out[o++] = '\t';
            pend = 0;
            col = (col / tab + 1) * tab;
        }
        p++;
    }
    while (pend-- > 0) out[o++] = ' ';
    size_t rest = strlen(p);
    memcpy(out + o, p, rest);
    return o + rest;
}

/* Sort / dedupe / trim / remove empty lines. Sort and dedupe need a
 * selection; trim and remove-empty fall back to the whole document. */
static void np_line_transform(notepad *a, np_doc *d, int mode) {
    int ls, le;
    if (!np_line_range(d, &ls, &le)) {
        np_toast(a, "没有可处理的行", "文档为空", CUI_TONE_INFO);
        return;
    }
    if (d->caret == d->anchor) {
        if (mode == TR_SORT || mode == TR_UNIQ || mode == TR_JOIN) {
            np_toast(a, "先选中要处理的行", "合并与排序、去重一样作用于选中的行",
                     CUI_TONE_INFO);
            return;
        }
        ls = 0; le = d->len;                /* whole document */
    }
    int span = le - ls;
    if (!np_ensure_tmp(d, (size_t)span + 2)) {
        np_toast(a, "内存不足", "无法分配转换缓冲区", CUI_TONE_WARNING);
        return;
    }
    char *work = d->tmp;
    memcpy(work, d->text + ls, (size_t)span);
    work[span] = 0;
    int nlines = 1;
    for (int i = 0; i < span; i++)
        if (work[i] == '\n') nlines++;
    if (nlines > NP_TL_MAX) {
        np_toast(a, "选区太大", "最多支持 8192 行", CUI_TONE_WARNING);
        return;
    }
    char *rows[NP_TL_MAX];
    int nr = 0;
    char *scratch = NULL;               /* tab conversion backing buffer */
    rows[nr++] = work;
    for (int i = 0; i < span; i++)
        if (work[i] == '\n') { work[i] = 0; rows[nr++] = work + i + 1; }

    if (mode == TR_SORT) {
        qsort(rows, (size_t)nr, sizeof rows[0], np_row_cmp);
    } else if (mode == TR_UNIQ) {
        int w = 1;
        for (int i = 1; i < nr; i++)
            if (strcmp(rows[i], rows[w - 1]) != 0) rows[w++] = rows[i];
        nr = w;
    } else if (mode == TR_STRIP) {
        for (int i = 0; i < nr; i++) {
            size_t l = strlen(rows[i]);
            while (l && (rows[i][l - 1] == ' ' || rows[i][l - 1] == '\t'))
                rows[i][--l] = 0;
        }
    } else if (mode == TR_NOEMPTY) {
        int w = 0;
        for (int i = 0; i < nr; i++)
            if (rows[i][0]) rows[w++] = rows[i];
        nr = w;
    } else if (mode == TR_TAB2SP || mode == TR_SP2TAB) {
        /* Rows live in work and must stay put for the rejoin below, so the
         * converted rows are written to scratch and the pointers replaced. */
        char *scr = (char *)malloc((size_t)span * 4 + 4);
        if (!scr) {
            np_toast(a, "内存不足", "无法分配转换缓冲区", CUI_TONE_WARNING);
            return;
        }
        char *np2[NP_TL_MAX];
        char *out = scr;
        for (int i = 0; i < nr; i++) {
            np2[i] = out;
            if (mode == TR_TAB2SP) out += np_row_tab2sp(rows[i], out, 4);
            else                   out += np_row_sp2tab(rows[i], out, 4);
            *out++ = 0;
        }
        memcpy(rows, np2, sizeof rows[0] * (size_t)nr);
        scratch = scr;
    } else if (mode == TR_JOIN) {
        int w = 0;
        for (int i = 0; i < nr; i++) {
            char *r = rows[i];
            while (r[0] == ' ' || r[0] == '\t') r++;
            size_t l = strlen(r);
            while (l && (r[l - 1] == ' ' || r[l - 1] == '\t')) r[--l] = 0;
            if (r[0]) rows[w++] = r;
        }
        if (w == 0) { rows[0] = (char *)""; w = 1; }
        nr = w;                             /* empty rows drop out of the join */
    }

    char sep = mode == TR_JOIN ? ' ' : '\n';
    bool had_nl = d->text[le - 1] == '\n';
    size_t total = 0;
    for (int i = 0; i < nr; i++) total += strlen(rows[i]) + 1;
    if (nr && !had_nl) total--;
    char *join = (char *)malloc(total + 1);
    if (!join) {
        np_toast(a, "内存不足", "无法分配转换缓冲区", CUI_TONE_WARNING);
        free(scratch);
        return;
    }
    size_t o = 0;
    for (int i = 0; i < nr; i++) {
        size_t l = strlen(rows[i]);
        memcpy(join + o, rows[i], l);
        o += l;
        if (i + 1 < nr) join[o++] = sep;    /* JOIN: space; else newline */
        else if (had_nl) join[o++] = '\n';
    }
    join[o] = 0;
    np_splice(a, d, ls, le, join, o);
    free(join);
    free(scratch);
}

/* Copy the whole line(s) under the caret or selection to just below them
 * (one undo step through np_splice). The caret lands at the copy's start. */
static void np_duplicate_line(notepad *a, np_doc *d) {
    int ls, le;
    if (!np_line_range(d, &ls, &le)) return;
    int len = d->len;
    bool ends_nl = le < len || d->text[le - 1] == '\n';
    int bl = le - ls;
    char *ins = (char *)malloc((size_t)bl + 2);
    if (!ins) {
        np_toast(a, "内存不足", "无法分配复制缓冲区", CUI_TONE_WARNING);
        return;
    }
    size_t n;
    if (ends_nl) {
        memcpy(ins, d->text + ls, (size_t)bl);
        n = (size_t)bl;
    } else {
        ins[0] = '\n';                      /* last line: copy below it */
        memcpy(ins + 1, d->text + ls, (size_t)bl);
        n = (size_t)bl + 1;
    }
    int at = le;
    np_splice(a, d, le, le, ins, n);
    free(ins);
    if (d->len == len + (int)n)             /* splice succeeded */
        cui_code_editor_select(a->ui, d->editor_id, at + (ends_nl ? 0 : 1),
                               at + (ends_nl ? 0 : 1));
}

/* Move the whole line(s) under the caret or selection up or down, swapping
 * them with the adjacent line block. The caret / selection keeps its shape
 * and rides along with the block. */
static void np_move_lines(notepad *a, np_doc *d, int dir) {
    int ls, le;
    if (!np_line_range(d, &ls, &le)) return;
    int len = d->len;
    int bl = le - ls, other, at, delta;
    char *buf;
    if (dir < 0) {
        if (ls == 0) return;                /* already the first line */
        other = ls - 1;
        while (other > 0 && d->text[other - 1] != '\n') other--;
        delta = -(ls - other);
        buf = (char *)malloc((size_t)bl + (size_t)(ls - other));
        if (!buf) goto oom;
        memcpy(buf, d->text + ls, (size_t)bl);
        memcpy(buf + bl, d->text + other, (size_t)(ls - other));
        at = other;
        np_splice(a, d, other, le, buf, (size_t)bl + (size_t)(ls - other));
    } else {
        if (le >= len) return;              /* last line: nothing below */
        int ne = le;
        while (ne < len && d->text[ne] != '\n') ne++;
        if (ne < len) ne++;
        delta = ne - le;
        buf = (char *)malloc((size_t)bl + (size_t)(ne - le));
        if (!buf) goto oom;
        memcpy(buf, d->text + le, (size_t)(ne - le));
        memcpy(buf + (ne - le), d->text + ls, (size_t)bl);
        at = ls;
        np_splice(a, d, ls, ne, buf, (size_t)(ne - le) + (size_t)bl);
    }
    free(buf);
    if (d->len != len) return;              /* splice failed; keep it simple */
    d->caret = NP_MAX(0, NP_MIN(d->caret + delta, d->len));
    d->anchor = NP_MAX(0, NP_MIN(d->anchor + delta, d->len));
    cui_code_editor_select(a->ui, d->editor_id, d->anchor, d->caret);
    return;
oom:
    np_toast(a, "内存不足", "无法分配移动缓冲区", CUI_TONE_WARNING);
}

/* Jump between matching (), [] and {} around the caret. */
static void np_goto_bracket(notepad *a, np_doc *d) {
    static const char open[] = "([{", close[] = ")]}";
    int len = d->len;
    if (!len) return;
    int pos = -1;
    for (int k = 0; k < 2 && pos < 0; k++)
        if (d->caret + k < len && strchr("([{)]}", d->text[d->caret + k]))
            pos = d->caret + k;
    if (pos < 0 && d->caret > 0 && strchr("([{)]}", d->text[d->caret - 1]))
        pos = d->caret - 1;
    if (pos < 0) {
        np_toast(a, "光标附近没有括号", "把光标放在 ( [ { ) ] } 上再试",
                 CUI_TONE_INFO);
        return;
    }
    char c = d->text[pos];
    int depth = 0, match = -1;
    const char *io = strchr(open, c);
    if (io) {
        char want = close[io - open];
        for (int i = pos; i < len && match < 0; i++) {
            char t = d->text[i];
            if (t == c) depth++;
            else if (t == want && --depth == 0) match = i;
        }
    } else {
        char want = open[strchr(close, c) - close];
        for (int i = pos; i >= 0 && match < 0; i--) {
            char t = d->text[i];
            if (t == c) depth++;
            else if (t == want && --depth == 0) match = i;
        }
    }
    if (match < 0) {
        np_toast(a, "没有找到配对的括号", "括号不匹配或未闭合", CUI_TONE_INFO);
        return;
    }
    cui_code_editor_select(a->ui, d->editor_id, pos, match + 1);
}

/* ---- cliptext (snippet library) ------------------------------------ */

typedef struct np_clip {
    const char *group;
    const char *label;
    const char *text;
} np_clip;

static const np_clip NP_CLIPS[] = {
    { "通用", "分隔线", "/* ------------------------------------------------------------------ */" },
    { "通用", "TODO 注释", "// TODO: " },
    { "通用", "文件头注释", "/* 文件名：\n * 说明：\n * 日期：" },
    { "HTML", "页面骨架",
      "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n  <meta charset=\"UTF-8\">\n  <title>标题</title>\n</head>\n<body>\n\n</body>\n</html>" },
    { "HTML", "表格", "<table>\n  <tr><th>列</th></tr>\n  <tr><td>值</td></tr>\n</table>" },
    { "HTML", "链接", "<a href=\"#\"></a>" },
    { "C", "main 函数", "int main(int argc, char **argv) {\n\n    return 0;\n}" },
    { "C", "for 循环", "for (int i = 0; i < n; i++) {\n\n}" },
    { "C", "函数模板", "static int func(int x) {\n\n}" },
    { "Python", "main 入口", "def main():\n    pass\n\n\nif __name__ == \"__main__\":\n    main()" },
    { "Python", "class 模板", "class Name:\n    def __init__(self):\n        pass" },
    { "Python", "try/except", "try:\n    pass\nexcept Exception as e:\n    print(e)" },
    { "Shell", "脚本头", "#!/bin/sh\nset -e" },
    { "Shell", "case 分支", "case \"$1\" in\n    start)\n        ;;\n    *)\n        ;;\nesac" },
};
#define NP_CLIP_COUNT ((int)(sizeof NP_CLIPS / sizeof NP_CLIPS[0]))

/* ---- config file (settings, recents, session) ----------------------- */

static void np_config_dir(char *out, size_t cap) {
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base) base = ".";
    snprintf(out, cap, "%s\\RapidEditor", base);
    wchar_t w[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, out, -1, w, 2048) > 0)
        CreateDirectoryW(w, NULL);      /* fails harmlessly if it exists */
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) snprintf(out, cap, "%s/rapideditor", xdg);
    else {
        const char *h = getenv("HOME");
        if (!h || !*h) h = ".";
        snprintf(out, cap, "%s/.config/rapideditor", h);
    }
    mkdir(out, 0755);                   /* fails harmlessly if it exists */
#endif
}

static void np_config_path(char *out, size_t cap) {
    char dir[NP_PATH_CAP];
    np_config_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(out, cap, "%s\\config.ini", dir);
#else
    snprintf(out, cap, "%s/config.ini", dir);
#endif
}

/* ---- X9 SearchProfile: 配置存储 + 对话框 -------------------------------- */
/* 独立存 searchprofiles.ini（auth_ref 只含引用名，明文密钥永不落盘）。 */
static void np_sp_cfg_path(char *out, size_t cap) {
    char dir[NP_PATH_CAP];
    np_config_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(out, cap, "%s\\searchprofiles.ini", dir);
#else
    snprintf(out, cap, "%s/searchprofiles.ini", dir);
#endif
}
static np_sp_store_t *np_sp_store_get(notepad *a) {
    if (!a->sp_store) {
        a->sp_store = np_sp_store_create();
        char path[NP_PATH_CAP];
        np_sp_cfg_path(path, sizeof path);
        np_sp_store_load(a->sp_store, path);
    }
    return a->sp_store;
}
static void np_sp_store_persist(notepad *a) {
    if (!a->sp_store) return;
    char path[NP_PATH_CAP];
    np_sp_cfg_path(path, sizeof path);
    if (!np_sp_store_save(a->sp_store, path))
        np_toast(a, "保存搜索源失败", "检查 auth_ref 是否含明文密钥，或目录不可写", CUI_TONE_WARNING);
}
/* 本地召回回调：从当前打开文档路径中做大小写不敏感子串匹配。 */
static size_t np_sp_local_cb(const char *query, char out[][260], size_t cap, void *user) {
    notepad *a = (notepad *)user;
    size_t n = 0;
    char q[128]; snprintf(q, sizeof q, "%s", query ? query : "");
    for (int i = 0; i < a->count && n < cap; ++i) {
        const char *p = a->docs[i]->path;
        if (!p[0]) continue;
        const char *hit = NULL;
        if (!q[0]) hit = p;
        else {
            /* strstr 大小写不敏感 */
            size_t pl = strlen(p), ql = strlen(q);
            for (size_t j = 0; j + ql <= pl; ++j) {
                size_t k = 0;
                for (; k < ql; ++k)
                    if (tolower((unsigned char)p[j+k]) != tolower((unsigned char)q[k])) break;
                if (k == ql) { hit = p; break; }
            }
        }
        if (hit) snprintf(out[n++], 260, "%s", p);
    }
    return n;
}
/* HTTP 传输适配：把 np_sp_fetch_fn 接到 np_net_get_json（调用方在 UI 线程
 * 同步触发，短超时；真实自动召回应放后台线程）。 */
static char *np_sp_ui_fetch(const char *url, const char *method, const char *secret,
                            int timeout_sec, void *user, int *status_out) {
    (void)method; (void)user;
    np_net_opts o; memset(&o, 0, sizeof o);
    o.timeout_sec = timeout_sec > 0 ? timeout_sec : 8;
    return np_net_get_json(url, secret, &o, status_out);
}
/* 鉴权引用解析：仅识别 env:NAME 从环境变量取明文（不落盘、不进日志）；
 * keychain:/vault: 等引用本机无系统钥匙串绑定时返回 NULL（匿名请求）。 */
static const char *np_sp_ui_resolve(const char *auth_ref, void *user) {
    (void)user;
    if (auth_ref && strncmp(auth_ref, "env:", 4) == 0 && auth_ref[4])
        return getenv(auth_ref + 4);
    return NULL;
}
static void np_searchprof_dialog(notepad *a, cui_ctx *ui) {
    if (!a->dlg_searchprof) return;
    np_sp_store_t *st = np_sp_store_get(a);
    cui_dialog(ui, "知识搜索源（SearchProfile）", &a->dlg_searchprof,
               .description = "配置外部/本地知识检索：端点、鉴权引用、域名白名单、字段映射与降级链。"
                              "鉴权只存引用名（keychain:/env:），明文密钥永不写入配置；域名白名单为空即拒绝全部请求。只召回，不写入文档。",
               .width = 780) {
        /* === 已配置列表 === */
        cui_text(ui, "已配置的搜索源", .size = 13, .font = CUI_FONT_MEDIUM);
        size_t cnt = np_sp_count(st);
        if (cnt == 0)
            cui_empty_state(ui, "还没有搜索源", .compact = true, .icon = CUI_ICON_SEARCH,
                            .description = "点击「新增」配置一个 HTTP JSON 或本地索引检索源。");
        for (size_t i = 0; i < cnt; ++i) {
            const np_sp_profile_t *p = np_sp_at(st, i);
            if (!p) continue;
            char line[600];
            snprintf(line, sizeof line, "%s  ·  %s  ·  %s  ·  %s",
                     p->name, np_sp_backend_name((np_sp_backend_t)p->backend),
                     p->enabled ? "启用" : "停用",
                     p->endpoint[0] ? p->endpoint : "（本地索引）");
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                cui_text(ui, line, .size = 12, .width = CUI_FILL);
                if (cui_button(ui, p->enabled ? "停用" : "启用", .size = CUI_SIZE_SM)) {
                    np_sp_set_enabled(st, p->name, !p->enabled);
                    np_sp_store_persist(a);
                }
                if (cui_button(ui, "编辑", .size = CUI_SIZE_SM)) {
                    a->sp_form = *p;
                    a->sp_edit = (int)i;
                    a->sp_test_done = false;
                }
                if (cui_button(ui, "删除", .size = CUI_SIZE_SM)) {
                    np_sp_remove(st, p->name);
                    np_sp_store_persist(a);
                    if (a->sp_edit == (int)i) a->sp_edit = -1;
                    break;
                }
            }
        }
        if (cui_button(ui, "＋ 新增搜索源", .size = CUI_SIZE_SM)) {
            np_sp_profile_t np; memset(&np, 0, sizeof np);
            np.backend = NP_SP_HTTP_JSON; np.enabled = true;
            snprintf(np.method, sizeof np.method, "GET");
            snprintf(np.query_param, sizeof np.query_param, "q");
            snprintf(np.field_title, sizeof np.field_title, "title");
            snprintf(np.field_url, sizeof np.field_url, "url");
            snprintf(np.field_snippet, sizeof np.field_snippet, "snippet");
            np.max_results = 10; np.timeout_sec = 15;
            a->sp_form = np; a->sp_edit = -2;
            a->sp_test_done = false;
        }

        cui_separator(ui);

        /* === 编辑表单 === */
        if (a->sp_edit >= -2) {
            np_sp_profile_t *f = &a->sp_form;
            cui_text(ui, a->sp_edit == -2 ? "新增搜索源" : "编辑搜索源",
                     .size = 13, .font = CUI_FONT_MEDIUM);
            cui_input(ui, "sp-name", f->name, sizeof f->name, .label = "名称", .width = CUI_FILL);
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                if (cui_button(ui, f->backend == NP_SP_HTTP_JSON ? "后端：HTTP JSON（点击切换）"
                                                                 : "后端：本地索引（点击切换）",
                               .size = CUI_SIZE_SM))
                    f->backend = f->backend == NP_SP_HTTP_JSON ? NP_SP_LOCAL_FTS : NP_SP_HTTP_JSON;
                cui_checkbox(ui, "启用", &f->enabled);
            }
            if (f->backend == NP_SP_HTTP_JSON) {
                cui_input(ui, "sp-endpoint", f->endpoint, sizeof f->endpoint,
                          .label = "端点 URL", .placeholder = "https://api.example.com/search",
                          .width = CUI_FILL);
                cui_row(ui, .width = CUI_FILL, .gap = 6) {
                    cui_input(ui, "sp-method", f->method, sizeof f->method,
                              .label = "方法", .placeholder = "GET", .width = 120);
                    cui_input(ui, "sp-qp", f->query_param, sizeof f->query_param,
                              .label = "查询参数名", .placeholder = "q", .width = 200);
                }
                cui_input(ui, "sp-auth", f->auth_ref, sizeof f->auth_ref,
                          .label = "鉴权引用（仅引用名，禁明文 Key）",
                          .placeholder = "env:MY_API_KEY 或 keychain:..., 留空=匿名", .width = CUI_FILL);
                const char *sec_reason = NULL;
                if (np_sp_looks_like_secret(f->auth_ref, &sec_reason))
                    cui_text(ui, sec_reason, .size = 12, .color = CUI_COLOR_WARNING);
            }
            cui_input(ui, "sp-rpath", f->result_path, sizeof f->result_path,
                      .label = "结果数组路径（点分）", .placeholder = "data.items 或留空=根数组", .width = CUI_FILL);
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                cui_input(ui, "sp-ftitle", f->field_title, sizeof f->field_title,
                          .label = "标题字段", .placeholder = "title", .width = 200);
                cui_input(ui, "sp-furl", f->field_url, sizeof f->field_url,
                          .label = "链接字段", .placeholder = "url", .width = 200);
                cui_input(ui, "sp-fsnip", f->field_snippet, sizeof f->field_snippet,
                          .label = "摘要字段", .placeholder = "snippet", .width = 200);
            }
            cui_input(ui, "sp-allow", f->domain_allow, sizeof f->domain_allow,
                      .label = "域名白名单（逗号分隔；空=拒绝全部）",
                      .placeholder = "api.example.com,*.wiki.example.org", .width = CUI_FILL);
            cui_input(ui, "sp-block", f->domain_block, sizeof f->domain_block,
                      .label = "域名黑名单（优先于白名单）", .placeholder = "evil.example.com", .width = CUI_FILL);
            cui_input(ui, "sp-fallback", f->fallback, sizeof f->fallback,
                      .label = "失败降级到（搜索源名称，留空=无）", .width = CUI_FILL);
            /* 数字字段用 +/- 调整，避免跨帧输入缓冲 */
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                char lb[64];
                snprintf(lb, sizeof lb, "最多结果 %d", f->max_results);
                cui_text(ui, lb, .size = 12);
                if (cui_button(ui, "−", .size = CUI_SIZE_SM)) f->max_results = NP_MAX(1, f->max_results - 1);
                if (cui_button(ui, "＋", .size = CUI_SIZE_SM)) f->max_results = NP_MIN(NP_SP_MAX_RESULTS, f->max_results + 1);
                snprintf(lb, sizeof lb, "超时 %ds", f->timeout_sec);
                cui_text(ui, lb, .size = 12);
                if (cui_button(ui, "−", .size = CUI_SIZE_SM)) f->timeout_sec = NP_MAX(1, f->timeout_sec - 1);
                if (cui_button(ui, "＋", .size = CUI_SIZE_SM)) f->timeout_sec = NP_MIN(120, f->timeout_sec + 1);
                snprintf(lb, sizeof lb, "优先级 %d", f->priority);
                cui_text(ui, lb, .size = 12);
                if (cui_button(ui, "−", .size = CUI_SIZE_SM)) f->priority--;
                if (cui_button(ui, "＋", .size = CUI_SIZE_SM)) f->priority++;
            }
            /* 校验提示 */
            {
                char why[160];
                np_sp_status_t vs = np_sp_validate(f, why, sizeof why);
                if (vs == NP_SP_OK)
                    cui_text(ui, "配置有效", .size = 12, .color = CUI_COLOR_SUCCESS);
                else
                    cui_text(ui, why, .size = 12, .color = CUI_COLOR_WARNING);
            }
            /* 测试召回（本地同步、HTTP 短超时同步） */
            cui_input(ui, "sp-testq", a->sp_test_query, sizeof a->sp_test_query,
                      .label = "测试查询词", .placeholder = "输入关键词测试召回", .width = CUI_FILL);
            if (cui_button(ui, "测试召回", .size = CUI_SIZE_SM)) {
                char why[160];
                np_sp_status_t vs = np_sp_validate(f, why, sizeof why);
                if (vs != NP_SP_OK) {
                    snprintf(a->sp_test_result, sizeof a->sp_test_result, "配置无效：%s", why);
                } else if (f->backend == NP_SP_LOCAL_FTS) {
                    np_sp_hit_t hits[8]; int total = 0;
                    np_sp_status_t rs = np_sp_recall_local(f, a->sp_test_query, np_sp_local_cb, a, hits, 8, &total);
                    if (rs == NP_SP_OK)
                        snprintf(a->sp_test_result, sizeof a->sp_test_result,
                                 "本地召回 %d 条：%s", total, hits[0].title);
                    else
                        snprintf(a->sp_test_result, sizeof a->sp_test_result, "无命中或失败：%s", np_sp_status_name(rs));
                } else {
                    if (!np_sp_domain_allowed(f, f->endpoint)) {
                        snprintf(a->sp_test_result, sizeof a->sp_test_result,
                                 "域名门禁拒绝：请检查白名单/黑名单（白名单空=拒绝全部）");
                    } else {
                        np_sp_hit_t hits[8]; int total = 0;
                        np_sp_status_t rs = np_sp_recall_http(f, a->sp_test_query, np_sp_ui_fetch,
                                            np_sp_ui_resolve, a, hits, 8, &total);
                        if (rs == NP_SP_OK)
                            snprintf(a->sp_test_result, sizeof a->sp_test_result,
                                     "HTTP 召回 %d 条（已脱敏）：%s", total, hits[0].title);
                        else
                            snprintf(a->sp_test_result, sizeof a->sp_test_result, "HTTP 失败：%s", np_sp_status_name(rs));
                    }
                }
                a->sp_test_done = true;
            }
            if (a->sp_test_done)
                cui_text(ui, a->sp_test_result, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                if (cui_button(ui, "保存", .variant = CUI_BUTTON_PRIMARY)) {
                    char why[160];
                    np_sp_status_t as = np_sp_add(st, f);
                    if (as == NP_SP_OK) {
                        np_sp_store_persist(a);
                        a->sp_edit = -1;
                        a->sp_test_done = false;
                    } else {
                        np_sp_validate(f, why, sizeof why);
                        np_toast(a, "保存失败", why, CUI_TONE_WARNING);
                    }
                }
                if (cui_button(ui, "取消")) { a->sp_edit = -1; a->sp_test_done = false; }
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "关闭", .variant = CUI_BUTTON_PRIMARY))
                a->dlg_searchprof = false;
        }
    }
}

/* ---- X6 MCP: 工具服务器管理（动态发现/健康检查/不可信输出） ------------ */
/* 独立存 mcp_servers.ini（auth_ref 只含引用名，明文密钥永不落盘）。 */
static void np_mcp_cfg_path(char *out, size_t cap) {
    char dir[NP_PATH_CAP];
    np_config_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(out, cap, "%s\\mcp_servers.ini", dir);
#else
    snprintf(out, cap, "%s/mcp_servers.ini", dir);
#endif
}
static np_mcp_store_t *np_mcp_store_get(notepad *a) {
    if (!a->mcp_store) {
        a->mcp_store = np_mcp_store_create();
        char path[NP_PATH_CAP];
        np_mcp_cfg_path(path, sizeof path);
        np_mcp_store_load(a->mcp_store, path);
    }
    return a->mcp_store;
}
static void np_mcp_store_persist(notepad *a) {
    if (!a->mcp_store) return;
    char path[NP_PATH_CAP];
    np_mcp_cfg_path(path, sizeof path);
    if (!np_mcp_store_save(a->mcp_store, path))
        np_toast(a, "保存 MCP 配置失败", "检查 auth_ref 是否含明文密钥，或目录不可写", CUI_TONE_WARNING);
}
/* MCP 传输适配：stdio/HTTP 服务器均无内置进程/网络直连权限，UI 层只提供
 * 注入点。真实 stdio 子进程与 HTTP 调用需在后台线程按 profile 实现；本适配
 * 默认失败（fail-closed），避免 UI 线程阻塞与未验工具被误调用。 */
static bool np_mcp_ui_transport(const np_mcp_server_t *srv, const char *req,
                                char *out_resp, size_t out_cap, void *user) {
    (void)srv; (void)req; (void)user;
    if (out_resp && out_cap) out_resp[0] = 0;
    return false;   /* 未连接真实 MCP 服务器：健康检查/发现会安全失败并禁用 */
}
/* 鉴权引用解析：仅 env:NAME 从环境变量取（不落盘/不进日志）；其余 NULL。 */
static const char *np_mcp_ui_resolve(const char *auth_ref, void *user) {
    (void)user;
    if (auth_ref && strncmp(auth_ref, "env:", 4) == 0 && auth_ref[4])
        return getenv(auth_ref + 4);
    return NULL;
}
static void np_mcp_dialog(notepad *a, cui_ctx *ui) {
    if (!a->dlg_mcp) return;
    np_mcp_store_t *st = np_mcp_store_get(a);
    cui_dialog(ui, "MCP 工具服务器（扩展管理）", &a->dlg_mcp,
               .description = "配置本地 stdio 或远程 HTTP 工具服务器：启动时发现工具、周期健康检查；"
                              "工具输出按不可信文本脱敏处理，写工具默认禁调用。鉴权只存引用名（keychain:/env:/vault:），"
                              "明文密钥永不写入配置；HTTP 域名白名单为空即拒绝。",
               .width = 800) {
        cui_text(ui, "已配置的 MCP 服务器", .size = 13, .font = CUI_FONT_MEDIUM);
        size_t cnt = np_mcp_count(st);
        if (cnt == 0)
            cui_empty_state(ui, "还没有 MCP 服务器", .compact = true, .icon = CUI_ICON_SEARCH,
                            .description = "点击「新增」配置一个 stdio 命令或 HTTP 端点。");
        for (size_t i = 0; i < cnt; ++i) {
            const np_mcp_server_t *s = np_mcp_at(st, i);
            if (!s) continue;
            char line[600];
            snprintf(line, sizeof line, "%s  ·  %s  ·  %s  ·  %s  ·  工具 %zu  ·  %s",
                     s->name, np_mcp_transport_name(s->transport),
                     s->enabled ? "启用" : "停用",
                     s->health ? "健康✓" : (s->fail_streak ? "降级" : "未检查"),
                     np_mcp_tool_count(st, s->name),
                     s->allow_write ? "允许写" : "只读");
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                cui_text(ui, line, .size = 12, .width = CUI_FILL);
                if (cui_button(ui, s->enabled ? "停用" : "启用", .size = CUI_SIZE_SM)) {
                    np_mcp_set_enabled(st, s->name, !s->enabled);
                    np_mcp_store_persist(a);
                }
                if (cui_button(ui, "编辑", .size = CUI_SIZE_SM)) {
                    a->mcp_form = *s;
                    a->mcp_edit = (int)i;
                    a->mcp_test_done = false;
                }
                if (cui_button(ui, "删除", .size = CUI_SIZE_SM)) {
                    np_mcp_remove(st, s->name);
                    np_mcp_store_persist(a);
                    if (a->mcp_edit == (int)i) a->mcp_edit = -1;
                    break;
                }
            }
        }
        if (cui_button(ui, "＋ 新增 MCP 服务器", .size = CUI_SIZE_SM)) {
            np_mcp_server_t ns; memset(&ns, 0, sizeof ns);
            ns.transport = NP_MCP_STDIO; ns.enabled = true; ns.timeout_sec = 15;
            a->mcp_form = ns; a->mcp_edit = -2; a->mcp_test_done = false;
        }

        cui_separator(ui);

        if (a->mcp_edit >= -2) {
            np_mcp_server_t *f = &a->mcp_form;
            cui_text(ui, a->mcp_edit == -2 ? "新增 MCP 服务器" : "编辑 MCP 服务器",
                     .size = 13, .font = CUI_FONT_MEDIUM);
            cui_input(ui, "mcp-name", f->name, sizeof f->name, .label = "名称", .width = CUI_FILL);
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                if (cui_button(ui, f->transport == NP_MCP_STDIO ? "传输：stdio（点击切换）"
                                                                : "传输：HTTP（点击切换)",
                               .size = CUI_SIZE_SM))
                    f->transport = f->transport == NP_MCP_STDIO ? NP_MCP_HTTP : NP_MCP_STDIO;
                cui_checkbox(ui, "启用", &f->enabled);
                cui_checkbox(ui, "允许写工具（默认禁）", &f->allow_write);
            }
            if (f->transport == NP_MCP_STDIO) {
                cui_input(ui, "mcp-cmd", f->command, sizeof f->command,
                          .label = "启动命令（禁 shell 元字符）", .placeholder = "python mcp_server.py --stdio",
                          .width = CUI_FILL);
            } else {
                cui_input(ui, "mcp-endpoint", f->endpoint, sizeof f->endpoint,
                          .label = "HTTP 端点 URL", .placeholder = "https://kb.example.com/mcp", .width = CUI_FILL);
                cui_input(ui, "mcp-allow", f->domain_allow, sizeof f->domain_allow,
                          .label = "域名白名单（逗号分隔；空=拒绝全部）",
                          .placeholder = "kb.example.com,*.internal.example.org", .width = CUI_FILL);
                cui_input(ui, "mcp-block", f->domain_block, sizeof f->domain_block,
                          .label = "域名黑名单（优先于白名单）", .placeholder = "evil.example.com", .width = CUI_FILL);
            }
            cui_input(ui, "mcp-auth", f->auth_ref, sizeof f->auth_ref,
                      .label = "鉴权引用（仅引用名，禁明文 Key）",
                      .placeholder = "env:MCP_TOKEN 或 keychain:..., 留空=匿名", .width = CUI_FILL);
            {
                const char *sec_reason = NULL;
                if (np_mcp_looks_like_secret(f->auth_ref, &sec_reason))
                    cui_text(ui, sec_reason, .size = 12, .color = CUI_COLOR_WARNING);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                char lb[64];
                snprintf(lb, sizeof lb, "超时 %ds", f->timeout_sec);
                cui_text(ui, lb, .size = 12);
                if (cui_button(ui, "−", .size = CUI_SIZE_SM)) f->timeout_sec = NP_MAX(1, f->timeout_sec - 1);
                if (cui_button(ui, "＋", .size = CUI_SIZE_SM)) f->timeout_sec = NP_MIN(120, f->timeout_sec + 1);
            }
            {
                char why[160];
                np_mcp_status_t vs = np_mcp_validate(f, why, sizeof why);
                if (vs == NP_MCP_OK)
                    cui_text(ui, "配置有效", .size = 12, .color = CUI_COLOR_SUCCESS);
                else
                    cui_text(ui, why, .size = 12, .color = CUI_COLOR_WARNING);
            }
            if (cui_button(ui, "测试发现 + 健康检查", .size = CUI_SIZE_SM)) {
                char why[160];
                np_mcp_status_t vs = np_mcp_validate(f, why, sizeof why);
                if (vs != NP_MCP_OK) {
                    snprintf(a->mcp_test_result, sizeof a->mcp_test_result, "配置无效：%s", why);
                } else if (f->transport == NP_MCP_HTTP && !np_mcp_domain_allowed(f, f->endpoint)) {
                    snprintf(a->mcp_test_result, sizeof a->mcp_test_result,
                             "域名门禁拒绝：检查白名单/黑名单（白名单空=拒绝全部）");
                } else {
                    /* 临时 store 里放一份，避免污染未保存的编辑缓冲状态。 */
                    np_mcp_store_t *tmp = np_mcp_store_create();
                    np_mcp_add(tmp, f);
                    np_mcp_status_t ds = NP_MCP_ERR_ARG;
                    int tools = np_mcp_discover(tmp, f->name, np_mcp_ui_transport,
                                                np_mcp_ui_resolve, a, &ds);
                    bool healthy = np_mcp_health_check(tmp, f->name, np_mcp_ui_transport,
                                                       np_mcp_ui_resolve, a);
                    snprintf(a->mcp_test_result, sizeof a->mcp_test_result,
                             "发现 %d 个工具（%s）；健康 %s。未连接真实服务器时安全失败。",
                             tools, np_mcp_status_name(ds), healthy ? "✓" : "✗（失败会禁用）");
                    np_mcp_store_destroy(tmp);
                }
                a->mcp_test_done = true;
            }
            if (a->mcp_test_done)
                cui_text(ui, a->mcp_test_result, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_row(ui, .width = CUI_FILL, .gap = 6) {
                if (cui_button(ui, "保存", .variant = CUI_BUTTON_PRIMARY)) {
                    char why[160];
                    np_mcp_status_t as = np_mcp_add(st, f);
                    if (as == NP_MCP_OK) {
                        np_mcp_store_persist(a);
                        a->mcp_edit = -1;
                        a->mcp_test_done = false;
                    } else {
                        np_mcp_validate(f, why, sizeof why);
                        np_toast(a, "保存失败", why, CUI_TONE_WARNING);
                    }
                }
                if (cui_button(ui, "取消")) { a->mcp_edit = -1; a->mcp_test_done = false; }
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "关闭", .variant = CUI_BUTTON_PRIMARY))
                a->dlg_mcp = false;
        }
    }
}

/* Snapshot files live next to config.ini. Keeping them as plain UTF-8 makes
 * recovery possible even when the original file was never saved. */
static void np_draft_path(char *out, size_t cap, int idx) {
    char dir[NP_PATH_CAP];
    np_config_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(out, cap, "%s\\draft%d.txt", dir, idx);
#else
    snprintf(out, cap, "%s/draft%d.txt", dir, idx);
#endif
}

/* Metadata sidecar written together with every snapshot. The periodic
 * crash-net saves drafts but cannot rewrite config.ini, so without this the
 * next startup pairs fresh snapshot bytes with stale path/encoding fields —
 * an explicit save could then overwrite the WRONG file. */
static void np_draft_meta_path(char *out, size_t cap, int idx) {
    char dir[NP_PATH_CAP];
    np_config_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(out, cap, "%s\\draft%d.meta", dir, idx);
#else
    snprintf(out, cap, "%s/draft%d.meta", dir, idx);
#endif
}

/* Write every dirty text buffer to the config directory. Hex documents are
 * skipped: their bytes can contain NUL and are not valid UTF-8 text. */
static void np_save_drafts(notepad *a) {
    a->draft_count = 0;
    for (int i = 0; i < a->count && a->draft_count < NP_DOC_MAX; i++) {
        np_doc *d = a->docs[i];
        if (!d->dirty || d->is_hex || d->len <= 0) continue;
        np_draft_entry *e = &a->drafts[a->draft_count];
        np_draft_path(e->file, sizeof e->file, a->draft_count);
        FILE *f = np_fopen_utf8(e->file, "wb");
        if (!f) continue;
        size_t n = (size_t)d->len;
        bool ok = fwrite(d->text, 1, n, f) == n;
        fclose(f);
        if (!ok) continue;
        snprintf(e->path, sizeof e->path, "%s", d->path);
        e->enc = d->enc;
        e->crlf = d->crlf;
        /* Sidecar keeps the metadata next to the bytes it describes. A
         * periodic save then cannot desync them even on a hard crash. */
        {
            char metapath[NP_PATH_CAP];
            np_draft_meta_path(metapath, sizeof metapath, a->draft_count);
            FILE *mf = np_fopen_utf8(metapath, "wb");
            if (mf) {
                fprintf(mf, "path=%s\nenc=%d\ncrlf=%d\n",
                        e->path, e->enc, e->crlf ? 1 : 0);
                fclose(mf);
            }
        }
        a->draft_count++;
    }
}

/* Restore one snapshot into a fresh tab. The document keeps its original
 * path and stays dirty, so an explicit save is still required. */
static void np_restore_draft(notepad *a, int idx) {
    if (idx < 0 || idx >= a->draft_count) return;
    np_draft_entry *e = &a->drafts[idx];
    /* Metadata from the sidecar is authoritative: config.ini may be older
     * than the snapshot if the process exited without a clean shutdown. */
    {
        char metapath[NP_PATH_CAP];
        np_draft_meta_path(metapath, sizeof metapath, idx);
        FILE *mf = np_fopen_utf8(metapath, "rb");
        if (mf) {
            char line[NP_PATH_CAP + 64];
            while (fgets(line, sizeof line, mf)) {
                size_t ln = strlen(line);
                while (ln && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
                if (!strncmp(line, "path=", 5))
                    snprintf(e->path, sizeof e->path, "%s", line + 5);
                else if (!strncmp(line, "enc=", 4)) {
                    int v = atoi(line + 4);
                    if (v >= 0 && v < 3) e->enc = v;   /* UTF8 / UTF8-BOM / ANSI */
                } else if (!strncmp(line, "crlf=", 5))
                    e->crlf = atoi(line + 5) != 0;
            }
            fclose(mf);
        }
    }
    FILE *f = np_fopen_utf8(e->file, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    size_t n = (size_t)sz;
    if (n >= NP_LOAD_MAX) { fclose(f); return; }
    np_doc *d = NULL;
    int tab = -1;
    if (a->count == 1 && np_blank_untitled(a->docs[0])) {
        d = a->docs[0]; tab = 0;
    } else {
        d = np_doc_new(a); tab = a->count - 1;
    }
    if (!d || !np_ensure_cap(d, n + 1024)) {
        fclose(f);
        if (tab >= 0 && a->count > 1) np_close_doc(a, tab);
        return;
    }
    size_t got = fread(d->text, 1, n, f);
    fclose(f);
    d->text[got] = 0;
    d->len = (int)got;
    d->rev++;
    d->content_rev++;
    d->dirty = true;
    d->is_hex = false;
    d->enc = e->enc;
    d->crlf = e->crlf;
    snprintf(d->path, sizeof d->path, "%s", e->path);
    d->lang = np_detect_lang(e->path, d->text);
    d->lang_manual = false;
    d->file_size = 0;
    np_bump_editor(a, d);
    np_activate(a, tab, false);
    np_logf(a, "恢复未保存草稿  %s", e->path[0] ? np_base(e->path) : "未命名文档");
    np_toast(a, "已恢复草稿", e->path[0] ? np_base(e->path) : "未命名文档",
             CUI_TONE_SUCCESS);
}

/* Collapse CR/LF/TAB to spaces so a value stays on one config line. */
static const char *np_cfg_one_line(const char *s, char *buf, size_t cap) {
    size_t o = 0;
    if (!s || !buf || cap == 0) return "";
    for (; *s && o + 1 < cap; s++)
        buf[o++] = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s;
    buf[o] = 0;
    return buf;
}

/* ---- API key storage ----------------------------------------------- */
/* Windows: DPAPI encrypts the key under the current user account before it
 * touches config.ini, so the plaintext key never sits on disk. Values are
 * stored as "dpapi:<base64>"; anything without the prefix is legacy plaintext
 * and keeps working. Other platforms fall back to plaintext (documented in
 * README) because there is no equivalent per-user keystore in this build. */
#define NP_SECRET_PREFIX "dpapi:"

static void np_secret_save(const char *plain, char *out, size_t cap) {
    out[0] = 0;
    if (!plain || !*plain || cap < 16) return;
#ifdef _WIN32
    DATA_BLOB in, enc;
    in.pbData = (BYTE *)(uintptr_t)plain;
    in.cbData = (DWORD)strlen(plain);
    enc.pbData = NULL;
    enc.cbData = 0;
    if (CryptProtectData(&in, L"RapidEditor LLM API Key", NULL, NULL, NULL,
                         CRYPTPROTECT_UI_FORBIDDEN, &enc) && enc.pbData) {
        DWORD n = 0;
        if (CryptBinaryToStringA(enc.pbData, enc.cbData,
                                 CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 NULL, &n) && n > 0 &&
            n + sizeof(NP_SECRET_PREFIX) < cap) {
            char *b64 = (char *)malloc(n);
            if (b64 && CryptBinaryToStringA(enc.pbData, enc.cbData,
                        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &n)) {
                snprintf(out, cap, NP_SECRET_PREFIX "%s", b64);
            }
            free(b64);
        }
        SecureZeroMemory(enc.pbData, enc.cbData);
        LocalFree(enc.pbData);
    }
#endif
    if (!out[0]) snprintf(out, cap, "%s", plain);  /* non-Windows / fallback */
}

static void np_secret_load(const char *stored, char *out, size_t cap) {
    out[0] = 0;
    if (!stored || !*stored || cap == 0) return;
    size_t plen = strlen(NP_SECRET_PREFIX);
    if (strncmp(stored, NP_SECRET_PREFIX, plen) != 0) {
        snprintf(out, cap, "%s", stored);          /* legacy plaintext */
        return;
    }
#ifdef _WIN32
    const char *b64 = stored + plen;
    DWORD n = 0;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, NULL, &n, NULL, NULL) ||
        n == 0) return;
    BYTE *bin = (BYTE *)malloc(n);
    if (!bin) return;
    if (CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, bin, &n, NULL, NULL)) {
        DATA_BLOB in, dec;
        in.pbData = bin;
        in.cbData = n;
        dec.pbData = NULL;
        dec.cbData = 0;
        if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                               CRYPTPROTECT_UI_FORBIDDEN, &dec) && dec.pbData) {
            size_t len = NP_MIN((size_t)dec.cbData, cap - 1);
            memcpy(out, dec.pbData, len);
            out[len] = 0;
            SecureZeroMemory(dec.pbData, dec.cbData);
            LocalFree(dec.pbData);
        }
    }
    SecureZeroMemory(bin, n);
    free(bin);
#endif
    /* A dpapi: value on another platform / user account cannot be decrypted:
     * leave the field empty so the user is asked to re-enter the key. */
}

static void np_config_load(notepad *a) {
    char path[NP_PATH_CAP];
    np_config_path(path, sizeof path);
    FILE *f = np_fopen_utf8(path, "rb");
    if (!f) return;
    a->style = -1;   /* legacy config without a style key keeps dark/accent behaviour */
    a->inline_delay = 1;   /* config missing the key keeps the default debounce */
    char line[NP_PATH_CAP + 128];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        if (!strcmp(key, "dark")) {
            a->dark = atoi(val) != 0;
            a->dark_valid = true;
        } else if (!strcmp(key, "system_theme")) {
            if (atoi(val)) a->dark_valid = false;   /* follow OS, ignore saved dark */
        } else if (!strcmp(key, "accent")) {
            int v = atoi(val);
            if (v >= 0 && v < CUI_ACCENT_COUNT) a->accent = v;
        } else if (!strcmp(key, "style")) {
            int v = atoi(val);
            a->style = (v >= 0 && v < NP_STYLE_COUNT) ? v : -1;
        } else if (!strcmp(key, "font")) {
            int v = atoi(val);
            if (v >= NP_FONT_MIN && v <= NP_FONT_MAX) a->font_size = v;
        } else if (!strcmp(key, "editor_font")) {
            snprintf(a->font_ident, sizeof a->font_ident, "%s", val);
        } else if (!strcmp(key, "editor_font_path")) {
            snprintf(a->font_custom_path, sizeof a->font_custom_path, "%s", val);
        } else if (!strcmp(key, "density")) {
            int v = atoi(val);
            if (v >= 0 && v <= 2) a->density = v;
        } else if (!strcmp(key, "line_numbers")) a->line_numbers = atoi(val) != 0;
        else if (!strcmp(key, "indent_guides")) a->indent_guides = atoi(val) != 0;
        else if (!strcmp(key, "editor_lig")) a->editor_lig = atoi(val) != 0;
        else if (!strcmp(key, "whitespace")) {
            int v = atoi(val);
            if (v >= 0 && v <= 2) a->whitespace = v;
        }
        else if (!strcmp(key, "show_status")) a->show_status = atoi(val) != 0;
        else if (!strcmp(key, "show_sidebar")) a->show_sidebar = atoi(val) != 0;
        else if (!strcmp(key, "show_output")) a->show_output = atoi(val) != 0;
        else if (!strcmp(key, "show_ai")) a->show_ai = atoi(val) != 0;
        else if (!strcmp(key, "show_preview")) a->show_preview = atoi(val) != 0;
        else if (!strcmp(key, "preview_ratio")) {
            float r = (float)atof(val);
            if (r >= 0.2f && r <= 0.85f) a->preview_ratio = r;
        }
        else if (!strcmp(key, "ai_ratio")) {
            float r = (float)atof(val);
            if (r >= 0.4f && r <= 0.92f) a->ai_ratio = r;
        }
        else if (!strcmp(key, "follow_active")) a->follow_active = atoi(val) != 0;
        else if (!strcmp(key, "default_crlf")) a->default_crlf = atoi(val) != 0;
        else if (!strcmp(key, "make_backup")) a->make_backup = atoi(val) != 0;
        else if (!strcmp(key, "auto_save")) a->auto_save = atoi(val) != 0;
        else if (!strcmp(key, "session_restore")) a->session_restore = atoi(val) != 0;
        else if (!strcmp(key, "match_case")) a->match_case = atoi(val) != 0;
        else if (!strcmp(key, "find_word")) a->find_word = atoi(val) != 0;
        else if (!strcmp(key, "lt_align")) snprintf(a->ltable_align, sizeof a->ltable_align, "%s", val);
        else if (!strcmp(key, "lt_style")) { int v = atoi(val); if (v >= 0 && v <= 1) a->ltable_style = v; }
        else if (!strcmp(key, "lt_header")) a->ltable_header = atoi(val) != 0;
        else if (!strcmp(key, "lt_long")) a->ltable_longtable = atoi(val) != 0;
        else if (!strcmp(key, "lt_resize")) a->ltable_resize = atoi(val) != 0;
        else if (!strcmp(key, "lt_width")) { float w = (float)atof(val); if (w >= 0.1f && w <= 1.0f) a->ltable_width = w; }
        else if (!strcmp(key, "tidy_indent")) a->tidy_indent = atoi(val) != 0;
        else if (!strcmp(key, "tidy_join")) a->tidy_join = atoi(val) != 0;
        else if (!strcmp(key, "tidy_trim")) a->tidy_trim = atoi(val) != 0;
        else if (!strcmp(key, "tidy_spaces")) { int v = atoi(val); if (v >= 2 && v <= 8) a->tidy_spaces = v; }
        else if (!strcmp(key, "font_latin")) snprintf(a->font_latin_name, sizeof a->font_latin_name, "%s", val);
        else if (!strcmp(key, "font_cjk")) snprintf(a->font_cjk_name, sizeof a->font_cjk_name, "%s", val);
        else if (!strcmp(key, "ai_cmd")) snprintf(a->ai_cmd, sizeof a->ai_cmd, "%s", val);
        else if (!strcmp(key, "ai_inst")) snprintf(a->ai_inst, sizeof a->ai_inst, "%s", val);
        else if (!strcmp(key, "ai_task")) { int v = atoi(val); if (v >= 0 && v <= 6 + NP_AI_ACTION_MAX) a->ai_task = v; }
        else if (!strcmp(key, "ai_ctx")) { int v = atoi(val); if (v >= 0 && v <= 1) a->ai_ctx = v; }
        else if (!strcmp(key, "ai_ctx_tabs")) a->ai_ctx_tabs = atoi(val) != 0;
        else if (!strcmp(key, "ai_inline")) a->ai_inline = atoi(val) != 0;
        else if (!strcmp(key, "ai_inline_delay")) {
            a->inline_delay = atoi(val);
            if (a->inline_delay < 1 || a->inline_delay > 5) a->inline_delay = 1;
        }
        else if (!strcmp(key, "ai_ctx_finds")) a->ai_ctx_finds = atoi(val) != 0;
        else if (!strcmp(key, "ai_backend")) { int v = atoi(val); if (v >= 0 && v <= 12) a->ai_backend = v; }
        else if (!strncmp(key, "aiact", 5) && strchr(key + 5, '_')) {
            int idx = atoi(key + 5);
            const char *field = strchr(key + 5, '_') + 1;
            if (idx >= 0 && idx < NP_AI_ACTION_MAX) {
                if (!strcmp(field, "name"))
                    snprintf(a->ai_actions[idx].name, sizeof a->ai_actions[idx].name, "%s", val);
                else if (!strcmp(field, "inst"))
                    snprintf(a->ai_actions[idx].inst, sizeof a->ai_actions[idx].inst, "%s", val);
                if (a->ai_actions[idx].name[0] && idx + 1 > a->ai_action_count)
                    a->ai_action_count = idx + 1;
            }
        }
        else if (!strcmp(key, "maclaw_centers")) snprintf(a->maclaw_centers, sizeof a->maclaw_centers, "%s", val);
        else if (!strcmp(key, "maclaw_center")) snprintf(a->maclaw_center, sizeof a->maclaw_center, "%s", val);
        else if (!strncmp(key, "llm_p", 5) && strchr(key + 5, '_')) {
            /* llm_p<N>_<field>=value lines hold the custom LLM providers. */
            int idx = atoi(key + 5);
            const char *field = strchr(key + 5, '_') + 1;
            if (idx >= 0 && idx < 8) {
                np_llm_provider *p = &a->llm_prov[idx];
                if (!strcmp(field, "name")) snprintf(p->name, sizeof p->name, "%s", val);
                else if (!strcmp(field, "url")) snprintf(p->url, sizeof p->url, "%s", val);
                else if (!strcmp(field, "key")) {
                    np_secret_load(val, p->api_key, sizeof p->api_key);
                    /* dpapi: value that cannot be decrypted leaves the field
                     * empty — tell the user instead of silent 401s. */
                    if (!strncmp(val, NP_SECRET_PREFIX, strlen(NP_SECRET_PREFIX)) &&
                        !p->api_key[0]) a->secret_load_failed = true;
                }
                else if (!strcmp(field, "model")) snprintf(p->model, sizeof p->model, "%s", val);
                else if (!strcmp(field, "models")) snprintf(p->models, sizeof p->models, "%s", val);
                else if (!strcmp(field, "temp")) { float v = (float)atof(val); if (v >= 0.f && v <= 2.f) p->temperature = v; }
                else if (!strcmp(field, "maxtok")) { int v = atoi(val); if (v >= 0) p->max_tokens = v; }
                if (idx + 1 > a->llm_prov_count) a->llm_prov_count = idx + 1;
            }
        }
        else if (!strcmp(key, "maclaw_hub")) snprintf(a->maclaw_hub, sizeof a->maclaw_hub, "%s", val);
        else if (!strcmp(key, "maclaw_token")) snprintf(a->maclaw_token, sizeof a->maclaw_token, "%s", val);
        else if (!strcmp(key, "maclaw_email")) snprintf(a->maclaw_email, sizeof a->maclaw_email, "%s", val);
        else if (!strcmp(key, "find")) snprintf(a->find_query, sizeof a->find_query, "%s", val);
        else if (!strcmp(key, "replace")) snprintf(a->replace_with, sizeof a->replace_with, "%s", val);
        else if (!strcmp(key, "workdir")) np_dir_set(a, val);
        else if (!strncmp(key, "recent", 6)) {
            int i = atoi(key + 6);
            if (i >= 0 && i < NP_RECENT_MAX && val[0]) {
                snprintf(a->recent[i], NP_PATH_CAP, "%s", val);
                if (i + 1 > a->recent_count) a->recent_count = i + 1;
            }
        } else if (!strncmp(key, "session_cur", 11)) {
            int i = atoi(key + 11);
            if (i >= 0 && i < NP_SESSION_MAX && val[0]) {
                a->session_cur[i] = atoi(val);
                if (i + 1 > a->session_count) a->session_count = i + 1;
            }
        } else if (!strncmp(key, "session", 7)) {
            int i = atoi(key + 7);
            if (i >= 0 && i < NP_SESSION_MAX && val[0]) {
                snprintf(a->session[i], NP_PATH_CAP, "%s", val);
                if (i + 1 > a->session_count) a->session_count = i + 1;
            }
        } else if (!strncmp(key, "draft_path", 10)) {
            int i = atoi(key + 10);
            if (i >= 0 && i < NP_DOC_MAX) {
                snprintf(a->drafts[i].path, NP_PATH_CAP, "%s", val);
                if (i + 1 > a->draft_count) a->draft_count = i + 1;
            }
        } else if (!strncmp(key, "draft_file", 10)) {
            int i = atoi(key + 10);
            if (i >= 0 && i < NP_DOC_MAX)
                snprintf(a->drafts[i].file, NP_PATH_CAP, "%s", val);
        } else if (!strncmp(key, "draft_enc", 9)) {
            int i = atoi(key + 9);
            if (i >= 0 && i < NP_DOC_MAX) a->drafts[i].enc = atoi(val);
        } else if (!strncmp(key, "draft_crlf", 10)) {
            int i = atoi(key + 10);
            if (i >= 0 && i < NP_DOC_MAX) a->drafts[i].crlf = atoi(val) != 0;
        }
    }
    fclose(f);
    /* 迁移：已保存 maclaw token 但后端仍是「自动检测」的旧配置，自动切到
     * MaClaw官方——否则会走本机并不存在的 claude/codex CLI 而失败。 */
    if (a->maclaw_token[0] && a->ai_backend == 0) a->ai_backend = 4;
    if (a->ai_task >= 7 && a->ai_task - 7 >= a->ai_action_count) a->ai_task = 6;
}

/* Called once when the app loop exits (any exit path: menu, window close). */
static void np_config_save(notepad *a) {
    char path[NP_PATH_CAP];
    char cfgline1[2048], cfgline2[2048], cfgline3[2048];
    np_config_path(path, sizeof path);
    FILE *f = np_fopen_utf8(path, "wb");
    if (!f) return;
    np_save_drafts(a);
    a->session_count = 0;
    for (int i = 0; i < a->count && a->session_count < NP_SESSION_MAX; i++) {
        np_doc *d = a->docs[i];
        if (d->path[0] && !d->dirty) {
            snprintf(a->session[a->session_count], NP_PATH_CAP, "%s", d->path);
            a->session_cur[a->session_count] =
                NP_MAX(0, NP_MIN(d->caret, d->len));
            a->session_count++;
        }
    }
    fprintf(f, "dark=%d\naccent=%d\nfont=%d\ndensity=%d\nstyle=%d\n",
            a->dark ? 1 : 0, a->accent, a->font_size, a->density, a->style);
    fprintf(f, "line_numbers=%d\nindent_guides=%d\neditor_lig=%d\nwhitespace=%d\nshow_status=%d\nshow_sidebar=%d\nshow_output=%d\n"
               "show_preview=%d\npreview_ratio=%.4f\nshow_ai=%d\nai_ratio=%.4f\n",
            a->line_numbers ? 1 : 0, a->indent_guides ? 1 : 0, a->editor_lig ? 1 : 0, a->whitespace,
            a->show_status ? 1 : 0,
            a->show_sidebar ? 1 : 0, a->show_output ? 1 : 0,
            a->show_preview ? 1 : 0, a->preview_ratio,
            a->show_ai ? 1 : 0, a->ai_ratio);
    fprintf(f, "follow_active=%d\ndefault_crlf=%d\nmake_backup=%d\n"
               "session_restore=%d\nmatch_case=%d\nfind_word=%d\nsystem_theme=%d\n"
               "auto_save=%d\n",
            a->follow_active ? 1 : 0, a->default_crlf ? 1 : 0,
            a->make_backup ? 1 : 0, a->session_restore ? 1 : 0,
            a->match_case ? 1 : 0, a->find_word ? 1 : 0, a->dark_valid ? 0 : 1,
            a->auto_save ? 1 : 0);
    fprintf(f, "editor_font=%s\neditor_font_path=%s\n",
            a->font_ident[0] ? a->font_ident : "default", a->font_custom_path);
    fprintf(f, "find=%s\n",
            np_cfg_one_line(a->find_query, cfgline1, sizeof cfgline1));
    fprintf(f, "replace=%s\n",
            np_cfg_one_line(a->replace_with, cfgline2, sizeof cfgline2));
    fprintf(f, "workdir=%s\n",
            np_cfg_one_line(a->side_dir, cfgline3, sizeof cfgline3));
    /* LaTeX / AI settings added for the TeXora-inspired feature set. */
    fprintf(f, "lt_align=%s\nlt_style=%d\nlt_header=%d\nlt_long=%d\nlt_resize=%d\nlt_width=%.4f\n",
            a->ltable_align[0] ? a->ltable_align : "lcr", a->ltable_style,
            a->ltable_header ? 1 : 0, a->ltable_longtable ? 1 : 0,
            a->ltable_resize ? 1 : 0, a->ltable_width);
    fprintf(f, "tidy_indent=%d\ntidy_join=%d\ntidy_trim=%d\ntidy_spaces=%d\n",
            a->tidy_indent ? 1 : 0, a->tidy_join ? 1 : 0,
            a->tidy_trim ? 1 : 0, a->tidy_spaces);
    fprintf(f, "font_latin=%s\nfont_cjk=%s\n", a->font_latin_name, a->font_cjk_name);
    fprintf(f, "ai_cmd=%s\nai_inst=%s\nai_task=%d\nai_ctx=%d\nai_backend=%d\nai_ctx_tabs=%d\nai_ctx_finds=%d\nai_inline=%d\nai_inline_delay=%d\n",
            np_cfg_one_line(a->ai_cmd, cfgline1, sizeof cfgline1),
            np_cfg_one_line(a->ai_inst, cfgline2, sizeof cfgline2),
            a->ai_task, a->ai_ctx, a->ai_backend,
            a->ai_ctx_tabs ? 1 : 0, a->ai_ctx_finds ? 1 : 0,
            a->ai_inline ? 1 : 0, a->inline_delay);
    for (int i = 0; i < a->ai_action_count && i < NP_AI_ACTION_MAX; i++) {
        const np_ai_action *act = &a->ai_actions[i];
        fprintf(f, "aiact%d_name=%s\naiact%d_inst=%s\n", i,
                np_cfg_one_line(act->name, cfgline1, sizeof cfgline1), i,
                np_cfg_one_line(act->inst, cfgline2, sizeof cfgline2));
    }
    /* IMPORTANT: np_cfg_one_line returns the buffer it writes into. Reusing
     * the same buffer for two %s arguments of ONE fprintf makes both print
     * the LAST value written (argument evaluation order does not help — all
     * args are evaluated before the call). Use a fresh buffer per field. */
    fprintf(f, "maclaw_centers=%s\n",
            np_cfg_one_line(a->maclaw_centers, cfgline1, sizeof cfgline1));
    fprintf(f, "maclaw_center=%s\n",
            np_cfg_one_line(a->maclaw_center, cfgline2, sizeof cfgline2));
    fprintf(f, "maclaw_hub=%s\n",
            np_cfg_one_line(a->maclaw_hub, cfgline3, sizeof cfgline3));
    fprintf(f, "maclaw_token=%s\n",
            np_cfg_one_line(a->maclaw_token, cfgline1, sizeof cfgline1));
    fprintf(f, "maclaw_email=%s\n",
            np_cfg_one_line(a->maclaw_email, cfgline2, sizeof cfgline2));
    for (int i = 0; i < a->llm_prov_count && i < 8; i++) {
        const np_llm_provider *p = &a->llm_prov[i];
        np_secret_save(p->api_key, cfgline3, sizeof cfgline3);
        fprintf(f, "llm_p%d_name=%s\n", i,
                np_cfg_one_line(p->name, cfgline1, sizeof cfgline1));
        fprintf(f, "llm_p%d_url=%s\n", i,
                np_cfg_one_line(p->url, cfgline2, sizeof cfgline2));
        fprintf(f, "llm_p%d_key=%s\n", i, cfgline3);
        fprintf(f, "llm_p%d_model=%s\n", i,
                np_cfg_one_line(p->model, cfgline1, sizeof cfgline1));
        fprintf(f, "llm_p%d_models=%s\n", i,
                np_cfg_one_line(p->models, cfgline2, sizeof cfgline2));
        fprintf(f, "llm_p%d_temp=%.2f\nllm_p%d_maxtok=%d\n", i,
                (double)p->temperature, i, p->max_tokens);
    }
    for (int i = 0; i < a->recent_count; i++)
        fprintf(f, "recent%d=%s\n", i, a->recent[i]);
    for (int i = 0; i < a->session_count; i++) {
        fprintf(f, "session%d=%s\n", i, a->session[i]);
        fprintf(f, "session_cur%d=%d\n", i, a->session_cur[i]);
    }
    for (int i = 0; i < a->draft_count; i++) {
        const np_draft_entry *e = &a->drafts[i];
        fprintf(f, "draft_path%d=%s\ndraft_file%d=%s\ndraft_enc%d=%d\ndraft_crlf%d=%d\n",
                i, e->path, i, e->file, i, e->enc, i, e->crlf ? 1 : 0);
    }
    fclose(f);
}

/* ---- backup & system open ------------------------------------------- */

/* Copy `path` to `path.bak` before it is overwritten by a save. */
static void np_backup_file(const char *path) {
    char bak[NP_PATH_CAP];
    snprintf(bak, sizeof bak, "%s.bak", path);
    FILE *in = np_fopen_utf8(path, "rb");
    if (!in) return;
    FILE *out = np_fopen_utf8(bak, "wb");
    if (!out) { fclose(in); return; }
    char chunk[64 * 1024];
    size_t m;
    while ((m = fread(chunk, 1, sizeof chunk, in)) > 0)
        fwrite(chunk, 1, m, out);
    fclose(in);
    fclose(out);
}

/* Single-quote for sh; Windows goes through ShellExecuteW instead. */
static void np_shell_quote(const char *s, char *out, size_t cap) {
    size_t o = 0;
    if (o < cap) out[o++] = '\'';
    for (; *s && o + 5 < cap; s++) {
        if (*s == '\'') {
            out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\'';
        } else {
            out[o++] = *s;
        }
    }
    if (o < cap) out[o++] = '\'';
    out[o] = 0;
}

/* Open with the system default program, or reveal the file in its folder. */
static void np_system_open(notepad *a, const char *path, bool locate) {
    if (!path || !path[0]) return;
#ifdef _WIN32
    wchar_t w[2048], params[2200];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048) <= 0) return;
    if (locate) {
        lstrcpynW(params, L"/select,\"", 2100);
        lstrcatW(params, w);
        lstrcatW(params, L"\"");
        ShellExecuteW(NULL, L"open", L"explorer.exe", params, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    }
#elif defined(__ANDROID__)
    /* Android: no shell and no "reveal in folder". SDL_OpenURL fires an
     * ACTION_VIEW intent, which opens the file/URL with the matching handler. */
    (void)locate;
    SDL_OpenURL(path);
#else
    char q[NP_PATH_CAP + 8], cmd[NP_PATH_CAP * 2 + 64];
    if (locate) {
        char dir[NP_PATH_CAP];
        np_dir_of(path, dir, sizeof dir);
        np_shell_quote(dir, q, sizeof q);
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd, "open -R %s", q);
#else
        snprintf(cmd, sizeof cmd, "xdg-open %s", q);
#endif
    } else {
        np_shell_quote(path, q, sizeof q);
#if defined(__APPLE__)
        snprintf(cmd, sizeof cmd, "open %s", q);
#else
        snprintf(cmd, sizeof cmd, "xdg-open %s", q);
#endif
    }
    if (system(cmd) == -1)
        np_toast(a, "无法打开", "调用系统命令失败", CUI_TONE_DANGER);
#endif
    np_logf(a, "%s  %s", locate ? "定位到目录" : "系统打开", path);
}

static void np_sidebar(cui_ctx *ui, notepad *a) {
    if (a->side_reload) {
        np_tree_build(a);
        a->side_reload = false;
    }
    if (np_reveal_pending[0]) {
        np_tree_reveal(0, np_reveal_pending);
        np_reveal_pending[0] = 0;
    }
    static const char *tab_names[4] = { "目录", "文档", "最近", "片段" };

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

        cui_tabs(ui, "side_tabs", tab_names, 4, &a->side_tab, .full_width = true);

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
                np_tree_draw_node(ui, a, 0, 0);
                if (np_tree[0].loaded && np_tree[0].child < 0)
                    cui_empty_state(ui, "目录为空或无法读取", .compact = true,
                            .icon = CUI_ICON_FOLDER_OPEN,
                            .description = "换一个目录，或从「文件 → 打开目录」重新选择。");
            } else if (a->side_tab == 1) {
                for (int i = 0; i < a->count; i++) {
                    np_doc *t = a->docs[i];
                    char label[NP_PATH_CAP + 64], tail[64];
                    snprintf(label, sizeof label, "%s%s###doc%u",
                             t->dirty ? "● " : "",
                             t->path[0] ? np_base(t->path) : "未命名", t->uid);
                    snprintf(tail, sizeof tail, "%s", NP_LANGS[t->lang].label);
                    int fk = np_doc_family(t);
                    if (cui_list_item(ui, label, .icon = np_family_icon(fk),
                                      .icon_color = np_family_tint(ui, fk),
                                      .trailing = tail, .selected = (i == a->active)))
                        np_activate(a, i, true);
                }
            } else if (a->side_tab == 3) {
                /* EditPlus's cliptext window: click a snippet to insert it. */
                const char *last_group = NULL;
                for (int i = 0; i < NP_CLIP_COUNT; i++) {
                    const np_clip *c = &NP_CLIPS[i];
                    if (last_group == NULL || strcmp(last_group, c->group) != 0) {
                        last_group = c->group;
                        cui_text(ui, c->group, .size = 11,
                                 .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                    }
                    char label[160];
                    snprintf(label, sizeof label, "%s###clip%d", c->label, i);
                    if (cui_list_item(ui, label, .icon = CUI_ICON_COPY))
                        np_insert_at_caret(a, c->text);
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
            np_match(d->text + i, a->find_query, qlen, a->match_case) &&
            np_word_edge((const char *)d->text, (size_t)n, (size_t)i,
                         qlen, a->find_word)) {
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
    /* Apply semantic results exactly once when the worker finishes. */
    if (a->sem_done != a->sem_last_state) {
        if (a->sem_done == 2) np_sem_apply(a);
        a->sem_last_state = a->sem_done;
        cui_request_redraw(ui);
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_SURFACE_MUTED, .gap = 0, .pad = 0) {
        cui_row(ui, .width = CUI_FILL, .pad = 6, .gap = 4) {
            if (cui_button(ui, "查找结果", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_SEARCH,
                           .selected = a->out_tab == 0)) a->out_tab = 0;
            if (a->hit_count) {
                char n[16];
                snprintf(n, sizeof n, "%d", a->hit_count);
                cui_badge(ui, n, .size = CUI_SIZE_SM, .tone = CUI_TONE_INFO);
            }
            if (cui_button(ui, "文件搜索", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_FOLDER_OPEN,
                           .selected = a->out_tab == 2)) a->out_tab = 2;
            if (a->ff_hit_count) {
                char n[16];
                snprintf(n, sizeof n, "%d", a->ff_hit_count);
                cui_badge(ui, n, .size = CUI_SIZE_SM, .tone = CUI_TONE_INFO);
            }
            if (cui_button(ui, "消息", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_TERMINAL,
                           .selected = a->out_tab == 1)) a->out_tab = 1;
            if (a->msg_count) {
                char n[16];
                snprintf(n, sizeof n, "%d", a->msg_count);
                cui_badge(ui, n, .size = CUI_SIZE_SM, .tone = CUI_TONE_INFO);
            }
            cui_spacer(ui);
            char info[192];
            if (a->out_tab == 0)
                snprintf(info, sizeof info, "“%s”  %d 处",
                         a->find_query[0] ? a->find_query : "未设置查找词", a->hit_count);
            else if (a->out_tab == 2)
                snprintf(info, sizeof info, "%s“%s”  %d 处  ·  %d 个文件 / %d 个目录",
                         a->sem_done == 2 ? "AI 语义  " : "",
                         a->ff_query[0] ? a->ff_query : "未设置查找词",
                         a->ff_hit_count, a->ff_files_seen, a->ff_dirs_seen);
            else
                snprintf(info, sizeof info, "%d 条", a->msg_count);
            cui_text(ui, info, .size = 11, .nowrap = true,
                     .color = CUI_COLOR_TEXT_MUTED);
            if (cui_icon_button(ui, CUI_ICON_TRASH_2, .size = CUI_SIZE_SM,
                                .variant = CUI_BUTTON_GHOST, .tooltip = "清空")) {
                if (a->out_tab == 1) a->msg_count = 0;
                else if (a->out_tab == 2) {
                    a->ff_hit_count = 0; a->ff_count = 0;
                    if (a->sem_done == 1) {
                        /* A search worker is still running: only request a
                         * cancel and clear the shown text. Resetting the
                         * state machine here would let a second search run
                         * concurrently on the same temp files and result
                         * buffers, corrupting both. */
                        a->sem_cancel = true;
                        a->sem_result[0] = 0;
                    } else {
                        a->sem_done = 0; a->sem_last_state = 0;
                        a->sem_result[0] = 0;
                    }
                }
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
                    if (cui_empty_state(ui, "在当前文件中查找",
                            .icon = CUI_ICON_SEARCH, .compact = true,
                            .description = "输入查找词，文档中所有匹配行会列在这里。",
                            .action = "开始查找  Ctrl+F",
                            .action_icon = CUI_ICON_SEARCH))
                        np_open_find(a);
                } else if (!a->hit_count) {
                    cui_empty_state(ui, "没有找到匹配行", .compact = true,
                            .icon = CUI_ICON_SEARCH,
                            .description = "换个查找词试试，或检查“区分大小写”“全词匹配”开关。");
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
            } else if (a->out_tab == 2) {
                if (a->sem_done == 1) {
                    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                        cui_spinner(ui, 14);
                        cui_text(ui, "AI 正在理解查询并筛选文件…", .size = 12,
                                 .color = CUI_COLOR_TEXT_MUTED);
                        if (cui_button(ui, "停止", .size = CUI_SIZE_SM,
                                       .variant = CUI_BUTTON_GHOST))
                            a->sem_cancel = true;
                    }
                    cui_text(ui, a->sem_summary, .size = 11,
                             .color = CUI_COLOR_TEXT_MUTED);
                } else if (a->sem_done == 3) {
                    cui_text(ui, "AI 语义搜索失败", .size = 13,
                             .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_DANGER);
                    cui_text(ui, a->sem_error[0] ? a->sem_error : "未知错误",
                             .size = 12, .nowrap = false);
                } else if (!a->ff_query[0]) {
                    if (cui_empty_state(ui, "在多个文件中查找",
                            .icon = CUI_ICON_FOLDER_OPEN, .compact = true,
                            .description = "跨目录搜索文字，或用 AI 语义搜索以自然语言描述要找的内容。",
                            .action = "打开文件搜索  Ctrl+Shift+F",
                            .action_icon = CUI_ICON_FOLDER_OPEN)) {
                        a->dlg_find_files = true;
                        if (!a->ff_dir[0]) np_dir_set(a, a->side_dir);
                    }
                } else if (a->sem_mode == 1 && a->sem_done == 2 && a->sem_answer[0]) {
                    cui_text(ui, a->sem_answer, .size = 12, .nowrap = false);
                    if (a->ff_hit_count) {
                        char cap[128];
                        snprintf(cap, sizeof cap, "引用（点击跳转）· %d 条",
                                 a->ff_hit_count);
                        cui_text(ui, cap, .size = 11,
                                 .color = CUI_COLOR_TEXT_MUTED);
                    } else {
                        cui_text(ui, "AI 未给出可跳转的引用。",
                                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                    }
                    for (int i = 0; i < a->ff_hit_count; i++) {
                        np_fhit *h = &a->ff_hits[i];
                        const char *fname = h->file >= 0 && h->file < a->ff_count
                                            ? np_base(a->ff_paths[h->file]) : "?";
                        char label[384], num[48];
                        snprintf(label, sizeof label, "%s:%d: %s###ffhit%d",
                                 fname, h->line, h->text, i);
                        snprintf(num, sizeof num, "第 %d 行", h->line);
                        if (cui_list_item(ui, label, .icon = CUI_ICON_SEARCH,
                                          .trailing = num))
                            np_open_fhit(a, i);
                    }
                } else if (!a->ff_hit_count) {
                    cui_empty_state(ui, a->sem_done == 2 ? "AI 未找到相关文件"
                                                         : "没有找到匹配行", .compact = true,
                            .icon = CUI_ICON_SEARCH,
                            .description = a->sem_done == 2
                             ? "换一种描述试试，或放宽文件类型后重新搜索。"
                             : "换个查找词试试，或检查“区分大小写”“全词匹配”开关。");
                } else {
                    for (int i = 0; i < a->ff_hit_count; i++) {
                        np_fhit *h = &a->ff_hits[i];
                        const char *fname = h->file >= 0 && h->file < a->ff_count
                                            ? np_base(a->ff_paths[h->file]) : "?";
                        char label[384], num[48];
                        snprintf(label, sizeof label, "%s:%d: %s###ffhit%d",
                                 fname, h->line, h->text, i);
                        snprintf(num, sizeof num, "第 %d 行", h->line);
                        if (cui_list_item(ui, label, .icon = CUI_ICON_SEARCH,
                                          .trailing = num))
                            np_open_fhit(a, i);
                    }
                }
            } else {
                if (!a->msg_count)
                    cui_empty_state(ui, "暂无消息", .compact = true,
                            .icon = CUI_ICON_INBOX,
                            .description = "打开、保存和替换文件时会在这里留下记录。");
                for (int i = 0; i < a->msg_count; i++)
                    cui_text(ui, a->msgs[i], .size = 12, .nowrap = true,
                             .font = CUI_FONT_MONO);
            }
        }
    }
}

/* Thin IDE-style breadcrumb above the content area. */
static void np_doc_breadcrumb(cui_ctx *ui, notepad *a, np_doc *d) {
    (void)a;
    cui_row(ui, .width = CUI_FILL, .gap = 8, .pad_x = 6, .pad_y = 2) {
        int fk = np_doc_family(d);
        cui_color ic = np_family_tint(ui, fk);
        if (!ic.a) ic = cui_theme_get(ui)->colors[CUI_TOKEN_TEXT_MUTED];
        cui_icon(ui, np_family_icon(fk), 12, ic);
        if (d->path[0]) {
            char dir[NP_PATH_CAP], crumb[NP_PATH_CAP + 64];
            np_dir_of(d->path, dir, sizeof dir);
            const char *base = np_base(dir);
            if (base[0])
                snprintf(crumb, sizeof crumb, "%s  /  %s", base, np_base(d->path));
            else
                snprintf(crumb, sizeof crumb, "%s", d->path);
            cui_text(ui, crumb, .size = 11, .color = CUI_COLOR_TEXT_MUTED,
                     .nowrap = true, .font = CUI_FONT_MONO);
        } else {
            cui_text(ui, "未命名文档", .size = 11,
                     .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_spacer(ui);
        if (d->dirty)
            cui_badge(ui, "未保存", .tone = CUI_TONE_WARNING, .size = CUI_SIZE_SM,
                      .dot = true);
        if (d->is_hex)
            cui_badge(ui, "HEX", .tone = CUI_TONE_VIOLET, .size = CUI_SIZE_SM);
        else
            cui_badge(ui, NP_LANGS[d->lang].label, .tone = CUI_TONE_INFO,
                      .size = CUI_SIZE_SM);
    }
}

/* Start screen shown for an empty, untouched session — quick entry points
 * instead of a blank editor with a tiny placeholder. */
static void np_welcome_pane(cui_ctx *ui, notepad *a) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .gap = 14, .align = CUI_ALIGN_CENTER,
               .justify = CUI_JUSTIFY_CENTER, .pad = 24) {
        cui_box(ui, .width = CUI_PX(56), .height = CUI_PX(56),
                .bg = CUI_COLOR_ACCENT, .radius = 16,
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, "R", .size = 28, .font = CUI_FONT_MEDIUM,
                     .color = CUI_COLOR_ACCENT_FOREGROUND);
        }
        cui_text(ui, "RapidEditor", .size = 26, .font = CUI_FONT_MEDIUM);
        cui_text(ui, "专业的文本与十六进制编辑器 · 大文件、UTF-8、跨平台、右键打开",
                 .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        cui_gap(ui, 6);
        cui_row(ui, .gap = 10) {
            if (cui_button(ui, "打开文件", .icon = CUI_ICON_FOLDER_OPEN,
                           .variant = CUI_BUTTON_PRIMARY))
                np_open_dialog(a);
            if (cui_button(ui, "十六进制打开", .icon = CUI_ICON_CPU))
                np_open_hex_dialog(a);
            if (cui_button(ui, "直接开始输入", .icon = CUI_ICON_SQUARE_PEN)) {
                a->welcome_dismissed = true;
                np_activate(a, a->active, true);
            }
        }
        cui_text(ui, "Ctrl+O 打开   ·   Ctrl+N 新建   ·   Ctrl+Shift+F 在文件中查找   ·   F1 快捷键",
                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        if (a->recent_count) {
            cui_separator(ui);
            cui_text(ui, "最近打开", .size = 12, .font = CUI_FONT_MEDIUM);
            for (int i = 0; i < a->recent_count && i < 5; i++) {
                char label[NP_PATH_CAP + 32];
                snprintf(label, sizeof label, "%s##welcome-recent%d",
                         np_base(a->recent[i]), i);
                if (cui_button(ui, label, .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_HISTORY,
                               .full_width = true))
                    np_open_path(a, a->recent[i], true);
            }
        }
    }
}

/* ---- smart paste --------------------------------------------------- */
/* Clipboard content is classified before insertion; the user always gets
 * a preview plus a raw-paste option, so no conversion happens silently. */

static size_t np_sp_app(char *out, size_t cap, size_t o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
    if (w < 0) return o;
    return (o + (size_t)w >= cap) ? cap - 1 : o + (size_t)w;
}

static int np_sp_split(char *line, char delim, char **f, int max) {
    int n = 0;
    if (!line || max < 1) return 0;
    f[n++] = line;
    for (char *p = line; *p; p++) {
        if (*p == delim) {
            *p = 0;
            if (n < max) f[n++] = p + 1;
            else *p = delim;
        }
    }
    return n;
}

static int np_sp_detect(const char *s, char *kind, size_t cap, char *delim_out) {
    const char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '{' || *p == '[') {
        snprintf(kind, cap, "JSON · 将重新缩进，键与值更清晰");
        *delim_out = 0;
        return 1;
    }
    char buf[16384];
    size_t n = strlen(p);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, p, n); buf[n] = 0;
    int lines = 0, tabs = 0, commas = 0, sems = 0, logs = 0;
    char *cur = buf;
    while (*cur) {
        char *end = strchr(cur, '\n');
        if (end) *end = 0;
        if (*cur) {
            lines++;
            if (strchr(cur, '\t')) tabs++;
            if (strchr(cur, ',')) commas++;
            if (strchr(cur, ';')) sems++;
            if (strstr(cur, "ERROR") || strstr(cur, "error") ||
                strstr(cur, "WARN")  || strstr(cur, "warn")  ||
                strstr(cur, "FATAL") || strstr(cur, "Exception") ||
                strstr(cur, "失败")  || strstr(cur, "错误")) logs++;
        }
        if (!end) break;
        cur = end + 1;
    }
    if (lines >= 2 && (tabs == lines || commas == lines || sems == lines)) {
        char d = (tabs == lines) ? '\t' : (commas == lines) ? ',' : ';';
        *delim_out = d;
        snprintf(kind, cap, "表格 · %d 行（%s）", lines,
                 d == '\t' ? "Tab / Excel 单元格" : d == ',' ? "CSV" : "分号分隔");
        return 0;
    }
    *delim_out = 0;
    if (logs > 0) {
        snprintf(kind, cap, "日志 · 找到 %d 行错误或警告，将提取摘要", logs);
        return 2;
    }
    snprintf(kind, cap, "纯文本 · 建议原样粘贴");
    return 3;
}

static void np_sp_make_table(notepad *a, const char *src, bool latex, char delim) {
    char buf[16384];
    size_t n = strlen(src);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, src, n); buf[n] = 0;
    char *rows[256];
    int nr = 0;
    char *cur = buf;
    while (*cur && nr < 256) {
        char *end = strchr(cur, '\n');
        if (end) *end = 0;
        if (*cur) rows[nr++] = cur;
        if (!end) break;
        cur = end + 1;
    }
    a->sp_out[0] = 0;
    if (nr <= 0) return;
    char *f[32];
    int cols = np_sp_split(rows[0], delim, f, 32);
    if (cols < 1) cols = 1;
    size_t o = 0;
    if (latex) {
        o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "\\begin{table}[htbp]\n\\centering\n\\begin{tabular}{");
        for (int c = 0; c < cols; c++)
            if (o + 1 < sizeof a->sp_out) a->sp_out[o++] = 'l';
        o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "}\n\\hline\n");
        /* rows[0] was already split into f[] when computing cols; don't split
         * it again (the delimiter bytes are now NULs). */
        for (int c = 0; c < cols; c++)
            o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "%s%s",
                          c ? " & " : "", f[c] ? f[c] : "");
        o = np_sp_app(a->sp_out, sizeof a->sp_out, o, " \\\\\n\\hline\n");
        for (int r = 1; r < nr; r++) {
            int cn = np_sp_split(rows[r], delim, f, 32);
            for (int c = 0; c < cols; c++)
                o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "%s%s",
                              c ? " & " : "", (c < cn && f[c]) ? f[c] : "");
            o = np_sp_app(a->sp_out, sizeof a->sp_out, o, " \\\\\n");
        }
        o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "\\hline\n\\end{tabular}\n\\end{table}\n");
    } else {
        for (int c = 0; c < cols; c++)
            o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "| %s ",
                          (c < cols && f[c]) ? f[c] : "");
        o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "|\n|");
        for (int c = 0; c < cols; c++)
            o = np_sp_app(a->sp_out, sizeof a->sp_out, o, " --- |");
        o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "\n");
        for (int r = 1; r < nr; r++) {
            int cn = np_sp_split(rows[r], delim, f, 32);
            for (int c = 0; c < cols; c++)
                o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "| %s ",
                              (c < cn && f[c]) ? f[c] : "");
            o = np_sp_app(a->sp_out, sizeof a->sp_out, o, "|\n");
        }
    }
    (void)o;
}

static void np_sp_json_pretty(const char *src, char *out, size_t cap) {
    size_t o = 0;
    int depth = 0;
    bool in_str = false, esc = false;
    for (const char *p = src; *p && o + 6 < cap; p++) {
        char c = *p;
        if (in_str) {
            out[o++] = c;
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; out[o++] = c; }
        else if (c == '{' || c == '[') {
            out[o++] = c; depth++; out[o++] = '\n';
            for (int i = 0; i < depth && o + 1 < cap; i++) out[o++] = ' ';
        } else if (c == '}' || c == ']') {
            depth--; if (depth < 0) depth = 0;
            out[o++] = '\n';
            for (int i = 0; i < depth && o + 1 < cap; i++) out[o++] = ' ';
            out[o++] = c;
        } else if (c == ',') {
            out[o++] = c; out[o++] = '\n';
            for (int i = 0; i < depth && o + 1 < cap; i++) out[o++] = ' ';
        } else if (c == ':') { out[o++] = c; out[o++] = ' '; }
        else if (!isspace((unsigned char)c)) out[o++] = c;
    }
    out[o] = 0;
}

static void np_sp_log_summary(const char *src, char *out, size_t cap) {
    char buf[32768];
    size_t n = strlen(src);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, src, n); buf[n] = 0;
    size_t o = 0;
    int kept = 0;
    char *cur = buf;
    while (*cur && kept < 40 && o + 8 < cap) {
        char *end = strchr(cur, '\n');
        if (end) *end = 0;
        if (strstr(cur, "ERROR") || strstr(cur, "error") ||
            strstr(cur, "WARN")  || strstr(cur, "warn")  ||
            strstr(cur, "FATAL") || strstr(cur, "Exception") ||
            strstr(cur, "失败")  || strstr(cur, "错误")) {
            size_t l = strlen(cur);
            if (l > 4 && !strstr(out, cur)) {
                int w = snprintf(out + o, cap - o, "%s\n", cur);
                if (w > 0) { o += (size_t)w; if (o >= cap) o = cap - 1; kept++; }
            }
        }
        if (!end) break;
        cur = end + 1;
    }
    out[o] = 0;
}

static bool np_smart_paste_insert(notepad *a, const char *text) {
    np_doc *d = np_cur(a);
    if (!d || d->is_hex || !text) return false;
    int pos = NP_MAX(0, NP_MIN(d->caret, d->len));
    size_t n = strlen(text);
    if (!np_ensure_cap(d, (size_t)d->len + n + 1)) return false;
    /* Write through the editor so the insertion becomes ONE undo step.
     * A plain memmove leaves CUI's delta history with stale byte offsets;
     * the next Ctrl+Z would then edit unrelated text (see np_ai_apply). */
    int applied_len = d->len;
    if (!cui_code_editor_replace_external(a->ui, d->editor_id,
                                          d->text, d->cap, d->len,
                                          pos, pos, text, (int)n,
                                          &applied_len) ||
        applied_len != d->len + (int)n) {
        return false;
    }
    d->len = applied_len;
    np_text_changed(a, d);
    cui_code_editor_select(a->ui, d->editor_id, pos + (int)n, pos + (int)n);
    return true;
}

static void np_smart_paste_open(notepad *a) {
    const char *clip = cui_get_clipboard_text(a->ui);
    if (!clip || !*clip) {
        np_toast(a, "剪贴板为空", "请先复制要粘贴的内容", CUI_TONE_WARNING);
        return;
    }
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    snprintf(a->sp_src, sizeof a->sp_src, "%s", clip);
    char delim = 0;
    a->sp_mode = np_sp_detect(a->sp_src, a->sp_kind, sizeof a->sp_kind, &delim);
    if (a->sp_mode == 0)
        np_sp_make_table(a, a->sp_src, np_doc_is_latex(d), delim);
    else if (a->sp_mode == 1)
        np_sp_json_pretty(a->sp_src, a->sp_out, sizeof a->sp_out);
    else if (a->sp_mode == 2)
        np_sp_log_summary(a->sp_src, a->sp_out, sizeof a->sp_out);
    else
        snprintf(a->sp_out, sizeof a->sp_out, "%s", a->sp_src);
    if (!a->sp_out[0] && a->sp_mode == 2) {
        snprintf(a->sp_out, sizeof a->sp_out, "%s", a->sp_src);
        snprintf(a->sp_kind, sizeof a->sp_kind, "日志 · 未发现错误行，可原样粘贴");
    }
    a->dlg_smart_paste = true;
}

/* ---- inline completion (ghost text) --------------------------------- */

/* Clean a raw model completion for inline display: trim whitespace,
 * normalize \r line breaks to \n, keep the whole (possibly multi-line)
 * text and unwrap a wrapping code fence (language tag lines dropped).
 * Returns a pointer into buf, or NULL when nothing usable remains. Kept
 * as a pure function so the regression test extracts and exercises the
 * real implementation. */
static char *np_inline_clean(char *buf) {
    char *s = buf, *e = buf + strlen(buf);
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    /* normalize line breaks in place: CRLF becomes one LF, lone CR too */
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '\r') {           /* CR: dropped when followed by LF */
            if (p[1] != '\n') *w++ = '\n';
        } else *w++ = *p;
    }
    *w = 0;
    /* models occasionally wrap the suggestion in a code fence anyway;
     * unwrap only when the fences enclose the whole text */
    size_t ln = strlen(s);
    if (ln >= 6 && !strncmp(s, "```", 3) &&
        !strncmp(s + ln - 3, "```", 3)) {
        char *after = s + 3;                 /* rest of the opening line */
        char *fnl = strchr(after, '\n');     /* opening line's newline */
        char *tail = s + ln - 3;             /* closing fence */
        s[ln - 3] = 0;
        s = after;
        /* a standalone opening line (``` or ```lang) is dropped; a line
         * that also holds content (```text```) stays untouched */
        if (fnl && fnl < tail &&
            !memchr(after, '`', (size_t)(fnl - after)))
            s = fnl + 1;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
    }
    return *s ? s : NULL;
}

static void np_inline_worker(void *arg) {
    notepad *a = (notepad *)arg;
    char out[2048] = {0}, err[256] = {0};
    /* per-call params: short suggestions with a fast timeout, independent
     * of the (possibly expensive) global provider settings; 600 tokens
     * covers the 300-character / 4-line budget of the prompt in Chinese */
    bool ok = np_openai_chat_ex(a->inline_url, a->inline_key, a->inline_model,
                                a->inline_prompt, 0.f, 600, 30,
                                out, sizeof out, err, sizeof err);
    a->inline_result[0] = 0;
    if (ok) {
        char *s = np_inline_clean(out);
        /* usable only when short enough to read at a glance */
        if (s && strlen(s) < 1100) snprintf(a->inline_result, sizeof a->inline_result, "%s", s);
    }
    a->inline_done = ok ? 2 : 3;
#ifdef _WIN32
    _endthreadex(0);
#else
    return;
#endif
}

/* Fire an inline continuation request from the current caret. Called from
 * the editor pane when the user has been idle for a moment and no request
 * is in flight. The provider snapshot lets the worker run without touching
 * the provider list, which the user may edit concurrently. */
static void np_inline_start(notepad *a, np_doc *d) {
    const np_llm_provider *p = &a->llm_prov[a->ai_backend - 5];
    if (!p->url[0] || !p->model[0]) return;
    int s0 = NP_MIN(d->caret, d->len);
    int before = 2000, after = 400;
    int from = s0 > before ? s0 - before : 0;
    int to = s0 + after < d->len ? s0 + after : d->len;
    /* land on UTF-8 character boundaries so the model sees valid text */
    while (from < s0 && ((unsigned char)d->text[from] & 0xC0) == 0x80) from++;
    while (to < d->len && ((unsigned char)d->text[to] & 0xC0) == 0x80) to++;
    char head[2100], tail[500];
    int hn = s0 - from, tn = to - s0;
    if (hn >= (int)sizeof head) hn = (int)sizeof head - 1;
    if (tn >= (int)sizeof tail) tn = (int)sizeof tail - 1;
    memcpy(head, d->text + from, (size_t)hn); head[hn] = 0;
    memcpy(tail, d->text + s0, (size_t)tn); tail[tn] = 0;
    snprintf(a->inline_prompt, sizeof a->inline_prompt,
             "你正在文本/代码编辑器里为用户提供行内续写。文件类型：%s。\n"
             "在 <CURSOR> 标记处续写。规则：\n"
             "1. 只输出要插入的续写文字本身，不要解释，不要 Markdown 代码块。\n"
             "2. 最多四行、不超过 300 字符，宁短勿长；需要多行时直接用换行。\n"
             "3. 不要重复 <CURSOR> 之后已有的内容。\n"
             "4. 没有合理续写时只输出一个空行。\n"
             "---<CURSOR> 之前---\n%s\n---<CURSOR>---\n%s\n---结束---",
             NP_LANGS[d->lang].label, head, tail);
    snprintf(a->inline_url, sizeof a->inline_url, "%s", p->url);
    snprintf(a->inline_key, sizeof a->inline_key, "%s", p->api_key);
    snprintf(a->inline_model, sizeof a->inline_model, "%s", p->model);
    a->inline_res_gen = a->inline_gen;
    a->inline_done = 1;
    NP_BEGIN_THREAD(np_inline_worker, a);
}

/* Manual trigger from the toolbar / context menu. Only fires when no request
 * is in flight - the one-second idle window makes waiting harmless, and two
 * workers would race on the shared result buffer. */
static void np_inline_manual(notepad *a, np_doc *d) {
    if (a->ai_backend < 5 || a->ai_backend - 5 >= a->llm_prov_count) return;
    if (a->inline_done != 0 || a->ai_done == 1) return;
    a->inline_last_edit = time(NULL);
    np_inline_start(a, d);
}

static void np_editor_pane(cui_ctx *ui, notepad *a, np_doc *d) {
    /* Keep a little headroom so ordinary typing never hits the ceiling. */
    if ((size_t)d->len + 8192 > d->cap)
        np_ensure_cap(d, (size_t)d->len + 65536 + 1);

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 0) {
        np_doc_breadcrumb(ui, a, d);
        /* Editor toolbar: quick AI actions on a selection, plus smart paste
         * which is useful with or without one. */
        if (!d->is_hex && a->ai_done != 1) {
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                if (d->caret != d->anchor) {
                cui_text(ui, "选中 AI", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED);
                if (cui_button(ui, "润色", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    np_ai_quick(a, 0);
                if (cui_button(ui, "翻译", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    np_ai_quick(a, 1);
                if (cui_button(ui, "检查", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    np_ai_quick(a, 2);
                if (cui_button(ui, "解释", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    np_ai_quick(a, 3);
                if (cui_button(ui, "续写", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    np_ai_quick(a, 5);
                }
                if (a->ai_backend >= 5 && a->ai_backend - 5 < a->llm_prov_count) {
                    if (cui_button(ui, "补全", .size = CUI_SIZE_SM,
                                   .variant = CUI_BUTTON_GHOST,
                                   .tooltip = "在光标处请求 AI 行内续写（Alt+\\）"))
                        np_inline_manual(a, d);
                }
                cui_spacer(ui);
                if (cui_code_editor_ghost_active(a->ui, d->editor_id)) {
                    cui_text(ui, "Tab 接受补全 · Esc 忽略", .size = 11,
                             .color = CUI_COLOR_TEXT_MUTED);
                    cui_spacer(ui);
                }
                if (cui_button(ui, "智能粘贴", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST,
                               .tooltip = "识别表格 / JSON / 日志，转换前可预览"))
                    np_smart_paste_open(a);
            }
        }
        cui_code_editor_result r = cui_code_editor(
            ui, d->editor_id, d->text, d->cap,
            .lang = NP_LANGS[d->lang].id,
            .height = -1,
            .size = (float)a->font_size,
            .length = d->len,          /* skip a full strlen on every frame */
            .revision = d->rev,        /* rebuild the line index only when needed */
            .no_line_numbers = !a->line_numbers,
            .no_indent_guides = !a->indent_guides,
            .show_whitespace = a->whitespace,
            .ligatures = a->editor_lig,
            .placeholder = "在这里输入文字…  Ctrl+N 新建标签，Ctrl+O 打开文件，Ctrl+S 保存",
            .read_only = false);

        /* Native AI actions on right click over the editor. */
        cui_context_menu(ui, "editor-ai-ctx") {
            bool has_sel = d->caret != d->anchor;
            cui_menu_label(ui, "AI 助手");
            if (cui_menu_item(ui, "润色选区", .icon = CUI_ICON_SPARKLES,
                              .disabled = !has_sel))
                np_ai_quick(a, 0);
            if (cui_menu_item(ui, "翻译选区", .icon = CUI_ICON_SPARKLES,
                              .disabled = !has_sel))
                np_ai_quick(a, 1);
            if (cui_menu_item(ui, "检查错误", .icon = CUI_ICON_SPARKLES,
                              .disabled = !has_sel))
                np_ai_quick(a, 2);
            if (cui_menu_item(ui, "解释选区", .icon = CUI_ICON_SPARKLES,
                              .disabled = !has_sel))
                np_ai_quick(a, 3);
            if (cui_menu_item(ui, "生成模板…", .icon = CUI_ICON_SPARKLES))
                np_ai_quick(a, 6);
            if (cui_menu_item(ui, "续写（光标处）", .icon = CUI_ICON_SPARKLES))
                np_ai_quick(a, 5);
            if (cui_menu_item(ui, "行内补全（光标处）", .icon = CUI_ICON_SPARKLES,
                              .disabled = a->ai_backend < 5 ||
                                          a->ai_backend - 5 >= a->llm_prov_count))
                np_inline_manual(a, d);
            for (int i = 0; i < a->ai_action_count && i < 4; i++) {
                char label[80];
                snprintf(label, sizeof label, "%s（自定义）", a->ai_actions[i].name);
                if (cui_menu_item(ui, label, .icon = CUI_ICON_SPARKLES))
                    np_ai_quick(a, 7 + i);
            }
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "智能粘贴…", .icon = CUI_ICON_CLIPBOARD))
                np_smart_paste_open(a);
            if (cui_menu_item(ui, "打开 AI 面板", .icon = CUI_ICON_SPARKLES))
                a->show_ai = true;
        }

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

        /* ---- inline completion: debounce, fire, show ------------------- */
        bool inline_activity = r.changed || a->inline_last_uid != d->uid ||
                               a->inline_last_cursor != r.cursor;
        if (inline_activity) {
            a->inline_gen++;                /* invalidates in-flight results */
            a->inline_last_uid = d->uid;
            a->inline_last_cursor = r.cursor;
            a->inline_last_edit = time(NULL);
        }
        if (a->inline_done == 2) {
            a->inline_done = 0;             /* the editor keeps its own ghost state */
            a->inline_fail_streak = 0;
            a->inline_fail_notified = false;
            if (a->inline_res_gen == a->inline_gen && a->inline_result[0])
                cui_code_editor_ghost(a->ui, d->editor_id, a->inline_result);
        } else if (a->inline_done == 3) {
            a->inline_done = 0;             /* failures stay silent: convenience feature */
            if (++a->inline_fail_streak >= 3 && !a->inline_fail_notified) {
                np_toast(a, "行内补全连续失败",
                         "请检查 AI 服务商地址 / Key / 模型设置",
                         CUI_TONE_WARNING);
                a->inline_fail_notified = true;
            }
        }
        if (a->ai_inline && r.focused && !d->is_hex && d->caret == d->anchor &&
            a->inline_done == 0 && a->ai_done == 0 &&
            a->ai_backend >= 5 && a->ai_backend - 5 < a->llm_prov_count &&
            !cui_code_editor_ghost_active(a->ui, d->editor_id) &&
            time(NULL) - a->inline_last_edit >= a->inline_delay) {
            np_inline_start(a, d);
        }

        /* Same id scope as the editor above: keep the other tabs' editor
         * states alive so their undo stacks and carets survive. */
        np_keep_editors(a, ui);
    }
}

/* ---------------- Binary templates (010 Editor style) ---------------- */

static void np_bt_reparse(notepad *a, np_doc *d);

/* Read a .bt template file into a NUL-terminated buffer. Returns true on
 * success. `name` receives a short display name derived from the filename. */
static bool np_bt_load_file(const char *path, char *src, size_t src_cap,
                            char *name, size_t name_cap) {
    if (!path || !*path) return false;
    FILE *f = np_fopen_utf8(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz >= src_cap) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    size_t got = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[got] = 0;
    const char *base = np_base(path);
    snprintf(name, name_cap, "custom:%s", base);
    return true;
}

/* Apply a template script to a hex document and parse it now. `name` is a
 * display name (builtin id or "custom:file"). Keeps the script on the doc so
 * edits can be re-parsed later. */
static void np_bt_apply(notepad *a, np_doc *d, const char *name, const char *src) {
    if (!d || !src) return;
    np_bt_result_free(&d->bt_result);
    memset(&d->bt_result, 0, sizeof d->bt_result);
    snprintf(d->bt_name, sizeof d->bt_name, "%s", name);
    snprintf(d->bt_source, sizeof d->bt_source, "%s", src);
    d->bt_content_rev = 0;
    d->bt_pick = 0;
    d->bt_first = 0;
    d->bt_show_tree = true;
    np_bt_reparse(a, d);
}

/* Re-run the active template if the buffer changed since the last parse.
 * Forward-declared because np_bt_apply is defined before it. */
static void np_bt_reparse(notepad *a, np_doc *d) {
    (void)a;
    if (!d || !d->bt_name[0] || !d->bt_source[0]) return;
    if (d->bt_content_rev == d->content_rev && d->bt_result.ok) return;
    bool ok = np_bt_execute(d->bt_source, (const uint8_t *)d->text,
                            (size_t)d->len, &d->bt_result);
    d->bt_content_rev = d->content_rev;
    if (!ok) {
        snprintf(a->bt_error, sizeof a->bt_error, "%s", d->bt_result.error);
    } else {
        a->bt_error[0] = 0;
    }
    if (a->ui) cui_request_redraw(a->ui);
}

static void np_bt_clear(np_doc *d) {
    if (!d) return;
    np_bt_result_free(&d->bt_result);
    memset(&d->bt_result, 0, sizeof d->bt_result);
    d->bt_name[0] = 0;
    d->bt_source[0] = 0;
    d->bt_content_rev = 0;
}

/* Build cui_hex_range[] from the parsed fields. Only leaf fields with a
 * nonzero size are marked; parent/struct nodes are skipped to avoid painting
 * a solid block. Caller provides a buffer; returns the count actually used. */
static int np_bt_ranges(np_doc *d, cui_hex_range *out, int cap) {
    if (!d || !d->bt_result.ok) return 0;
    int n = 0;
    np_bt_result *r = &d->bt_result;
    for (int i = 0; i < r->count && n < cap; i++) {
        np_bt_field *f = &r->fields[i];
        if (f->size == 0 || f->offset + f->size > (size_t)d->len) continue;
        /* skip aggregate parents (their children carry the useful ranges) */
        bool has_children = false;
        for (int j = i + 1; j < r->count; j++)
            if (r->fields[j].parent == i) { has_children = true; break; }
        if (has_children) continue;
        out[n].offset = f->offset;
        out[n].length = f->size;
        out[n].tone = (cui_tone)(f->color % 8);
        out[n].label = f->name;
        n++;
    }
    return n;
}

/* Hex field-tree canvas ------------------------------------------------ */
#define NP_BT_ROWH 22

static void np_maclaw_qr_paint(cui_ctx *ui, cui_rect r, void *user) {
    const np_qr_code *q = (const np_qr_code *)user;
    if (!q || q->size <= 0) return;
    /* Use pure black/white, not theme tokens: QR scanners need a stable,
     * high-contrast image even when the app uses a dark theme or a coloured
     * accent. */
    const cui_color qr_white = CUI_RGB(0xFFFFFF);
    const cui_color qr_black = CUI_RGB(0x000000);
    cui_draw_rect(ui, r, CUI_CORNERS(0), qr_white);
    int total = q->size + 8; /* 4-module quiet zone on each side */
    float side = r.w < r.h ? r.w : r.h;
    float cell = side / (float)total;
    if (cell < 0.5f) cell = 0.5f;
    float used = cell * total;
    float ox = r.x + (r.w - used) * 0.5f + cell * 4.0f;
    float oy = r.y + (r.h - used) * 0.5f + cell * 4.0f;
    for (int y = 0; y < q->size; y++) {
        for (int x = 0; x < q->size; x++) {
            if (!q->modules[y][x]) continue;
            cui_draw_rect(ui, (cui_rect){ ox + x * cell, oy + y * cell, cell + 0.6f, cell + 0.6f },
                          CUI_CORNERS(0), qr_black);
        }
    }
}

static void np_bt_tree_paint(cui_ctx *ui, cui_rect r, void *user) {
    np_doc *d = (np_doc *)user;
    if (!d || !d->bt_result.ok) return;
    np_bt_result *res = &d->bt_result;
    int n = res->count > 2000 ? 2000 : res->count;  /* guard runaway trees */
    float fs = 13.0f;
    for (int i = 0; i < n; i++) {
        np_bt_field *f = &res->fields[i];
        float y = r.y + (float)i * NP_BT_ROWH;
        if (i == d->bt_pick) {
            cui_draw_rect(ui, (cui_rect){ r.x, y, r.w, NP_BT_ROWH },
                          CUI_CORNERS(3), CUI_COLOR_ACCENT_SUBTLE);
        }
        int indent = f->depth * 12;
        char line[320];
        snprintf(line, sizeof line, "%s  :  %s", f->name,
                 f->value[0] ? f->value : f->type);
        cui_draw_text(ui, CUI_FONT_MONO, fs,
                      (cui_vec2){ r.x + 6 + (float)indent, y + 3 },
                      i == d->bt_pick ? CUI_COLOR_ACCENT : CUI_COLOR_FOREGROUND,
                      line, -1);
        char off[32];
        snprintf(off, sizeof off, "@%llX", (unsigned long long)f->offset);
        cui_draw_text(ui, CUI_FONT_MONO, fs - 1,
                      (cui_vec2){ r.x + r.w - 74, y + 3 },
                      CUI_COLOR_TEXT_MUTED, off, -1);
    }
}

/* H7: 选对照文件 → 与当前 hex 文档做字节 Diff → 送入 B3 审查面板。
 * before = 当前文档（审查基线），after = 对照文件（目标版本）。 */
static void np_hex_diff_dialog(notepad *a, np_doc *d) {
    if (!d) return;
    char p[NP_PATH_CAP];
    if (!cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
            .kind = CUI_FILE_OPEN, .title = "选择对照版本（字节 Diff 目标）",
            .filter_name = "所有文件", .filter = "*" },
            p, sizeof p))
        return;
    FILE *f = fopen(p, "rb");
    if (!f) {
        np_logf(a, "字节 Diff：无法打开 %s", np_base(p));
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (long)64 * 1024 * 1024) {   /* 64 MB 上限防御 */
        fclose(f);
        np_logf(a, "字节 Diff：文件大小超出 64 MB 上限");
        return;
    }
    unsigned char *after = (unsigned char *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!after) { fclose(f); return; }
    size_t got = fread(after, 1, (size_t)sz, f);
    fclose(f);
    size_t n = np_review_ui_inject_byte(np_review_ui_get(),
                                        (const unsigned char *)d->text, (size_t)d->len,
                                        after, got, d->path);
    free(after);
    if (n) {
        a->show_review = true;
        a->show_byteinspect = false;
        np_logf(a, "字节 Diff：%zu 处改动已送入 B3 审查 · 对照 %s", n, np_base(p));
    } else {
        np_logf(a, "字节 Diff：两版本无差异或无法生成 hunk");
    }
}

/* H3 字节解释：把当前选区解释为多类型候选字段，渲染成可点芯片。
 * 点芯片把选区收窄为该字段宽度并跳回 hex（"字段芯片可跳 hex"）；只读。 */
static void np_byte_inspect_panel(cui_ctx *ui, notepad *a, np_doc *d) {
    np_bi_result_t r;
    size_t off = d->hexsel.start;
    size_t len = d->hexsel.length ? d->hexsel.length : 8;
    if (len > 16) len = 16;
    np_bi_interpret((const unsigned char *)d->text, (size_t)d->len, off, len, &r);

    char head[160];
    snprintf(head, sizeof head, "选区 0x%X · %zu 字节 · %d 种类型候选%s",
             (unsigned)off, d->hexsel.length ? d->hexsel.length : 0,
             r.nfields, r.nfields >= NP_BI_MAX_FIELDS ? "（已截断）" : "");
    cui_text(ui, head, .size = 11, .font = CUI_FONT_MEDIUM);

    for (int i = 0; i < r.nfields; i++) {
        const np_bi_field_t *f = &r.fields[i];
        char chip[128];
        const char *en = np_bi_type_endian(f->type);
        if (en[0])
            snprintf(chip, sizeof chip, "%s = %s · %s###bi%d",
                     np_bi_type_name(f->type), f->value, en, i);
        else
            snprintf(chip, sizeof chip, "%s = %s###bi%d",
                     np_bi_type_name(f->type), f->value, i);
        if (cui_button(ui, chip, .size = CUI_SIZE_SM,
                       .variant = f->plausible ? CUI_BUTTON_GHOST : CUI_BUTTON_GHOST,
                       .tooltip = "点选：把 hex 选区收窄为该字段宽度")) {
            d->hexsel.cursor = f->offset;
            d->hexsel.start = f->offset;
            d->hexsel.length = f->size;
            if (a->ui) cui_hex_view_goto(a->ui, d->editor_id, f->offset);
        }
    }
    /* B5 引用：file:0xOFFSET（本地读回由 Citation 模块负责；此处只生成串） */
    char cite[280];
    const char *base = d->path;
    const char *slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    for (const char *p = base; *p; p++) if (*p == '\\') base = p + 1;
    snprintf(cite, sizeof cite, "%s:0x%X", base, (unsigned)off);
    cui_text(ui, cite, .size = 11, .color = CUI_COLOR_SUCCESS);
}

/* 010 Editor style pane: offsets, hex bytes and ASCII, fully editable —
 * typing hex digits overwrites the byte under the cursor. cui_hex_view owns
 * all of the interaction (selection, arrows, copy, virtual scrolling); we
 * only feed it our buffer, track edits and wire the goto field. */
static void np_hex_pane(cui_ctx *ui, notepad *a, np_doc *d) {
    np_bt_reparse(a, d);               /* re-parse after byte edits, cached by rev */

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 6) {
        np_doc_breadcrumb(ui, a, d);
        cui_row(ui, .width = CUI_FILL, .gap = 6) {
            int flags = cui_input(ui, "hex-goto", a->hexgoto_buf, sizeof a->hexgoto_buf,
                                  .placeholder = "偏移（十六进制，如 1A2B）",
                                  .width = 200);
            if (flags & CUI_SUBMITTED) np_hex_do_goto(a);
            if (cui_button(ui, "跳转", .size = CUI_SIZE_SM)) np_hex_do_goto(a);
            cui_spacer(ui);
            if (cui_button(ui, a->show_byteinspect ? "隐藏字节解释" : "字节解释",
                           .size = CUI_SIZE_SM, .icon = CUI_ICON_CODE,
                           .tooltip = "本地把选区字节解释为多类型候选（只读，字段芯片可跳 hex）"))
                a->show_byteinspect = !a->show_byteinspect;
            if (cui_button(ui, "字节 Diff…", .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_GIT_BRANCH,
                           .tooltip = "选择对照版本，与当前字节做偏移对齐 Diff 并送入 B3 逐块审查"))
                np_hex_diff_dialog(a, d);
            if (cui_button(ui, "AI 解释选区", .size = CUI_SIZE_SM,
                           .icon = CUI_ICON_SPARKLES,
                           .tooltip = "把当前字节选区的 hexdump 发给 AI 解释（只读，不写入）"))
                np_ai_quick(a, 3);
            if (cui_button(ui, d->bt_name[0] ? "模板…" : "应用模板…",
                           .size = CUI_SIZE_SM, .icon = CUI_ICON_CODE)) {
                a->dlg_bt = true;
                a->bt_pick = 0;
            }
            if (d->bt_name[0] && cui_button(ui, "清除模板", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST)) {
                np_bt_clear(d);
            }
            if (cui_button(ui, "转到文本编辑", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_FILE_TEXT))
                np_hex_reopen(a, false);
        }
        if (a->bt_error[0]) {
            cui_text(ui, a->bt_error, .size = 11, .color = CUI_COLOR_DANGER);
        } else if (d->bt_name[0]) {
            char info[160];
            snprintf(info, sizeof info, "模板 %s · %d 字段", d->bt_name, d->bt_result.count);
            cui_text(ui, info, .size = 11, .color = CUI_COLOR_SUCCESS);
        } else {
            cui_text(ui, "点击/拖选字节 · 方向键移动 · 键入十六进制覆盖 · Ctrl C 复制 · Ctrl Home/End 首尾",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }

        /* H3 字节解释面板（点按钮切换；只读，字段芯片可跳 hex） */
        if (a->show_byteinspect) {
            cui_spacer(ui);
            np_byte_inspect_panel(ui, a, d);
            cui_spacer(ui);
        }

        /* Build highlight ranges from the template fields (frame-stable),
         * then append the hex find's hits: they show both as in-view tints
         * and as scrollbar ticks (capped to the room the array has left). */
        static cui_hex_range ranges[512];
        int nr = np_bt_ranges(d, ranges, 512);
        for (int i = 0; nr < 512 && i < a->hexfind_hit_count
                       && a->hexfind_doc_uid == d->uid; i++) {
            ranges[nr].offset = a->hexfind_hits[i];
            ranges[nr].length = (size_t)(a->hexfind_len ? a->hexfind_len : 1);
            ranges[nr].tone = CUI_TONE_INFO;
            ranges[nr].label = NULL;
            nr++;
        }

        if (d->bt_show_tree && d->bt_name[0] && d->bt_result.ok) {
            if (d->bt_ratio < 0.2f || d->bt_ratio > 0.85f) d->bt_ratio = 0.58f;
            cui_split(ui, "hex-bt-split", &d->bt_ratio, .min_first = 0.25f, .min_second = 0.2f) {
                /* left: hex view */
                cui_hex_result r = cui_hex_view(ui, d->editor_id,
                                    (const uint8_t *)d->text, (size_t)d->len,
                                    &d->hexsel,
                                    .edit = (uint8_t *)d->text,
                                    .ranges = nr ? ranges : NULL, .range_count = nr,
                                    .height = -1, .size = (float)a->font_size - 2);
                if (r.edited) {
                    d->dirty = true;
                    d->content_rev++;
                    np_update_title(a);
                }
                /* right: field tree */
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .bg = CUI_COLOR_SURFACE, .pad = 4, .gap = 2) {
                    char th[96];
                    snprintf(th, sizeof th, "字段结构 · %d###bttree", d->bt_result.count);
                    cui_label(ui, th);
                    int n = d->bt_result.count > 2000 ? 2000 : d->bt_result.count;
                    if (n > 0) {
                        float canvas_h = (float)n * NP_BT_ROWH;
                        cui_scroll(ui, .id = "bt-tree-scroll", .width = CUI_FILL,
                                   .height = CUI_FILL) {
                            cui_canvas_result cr = cui_canvas(ui, "bt-tree-canvas",
                                CUI_FILL, (cui_size){ CUI_SIZE_FIXED, canvas_h },
                                np_bt_tree_paint, d, sizeof d);
                            if (cr.clicked) {
                                int row = (int)(cr.mouse.y / NP_BT_ROWH);
                                if (row >= 0 && row < n) {
                                    np_bt_field *f = &d->bt_result.fields[row];
                                    d->bt_pick = row;
                                    d->hexsel.cursor = f->offset;
                                    d->hexsel.start = f->offset;
                                    d->hexsel.length = f->size;
                                    if (a->ui) cui_hex_view_goto(a->ui, d->editor_id, f->offset);
                                }
                            }
                        }
                    } else {
                        cui_text(ui, "模板未产生字段", .size = 12,
                                 .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            }
        } else {
            cui_hex_result r = cui_hex_view(ui, d->editor_id,
                                (const uint8_t *)d->text, (size_t)d->len,
                                &d->hexsel,
                                .edit = (uint8_t *)d->text,
                                .ranges = nr ? ranges : NULL, .range_count = nr,
                                .height = -1, .size = (float)a->font_size - 2);
            if (r.edited) {
                d->dirty = true;
                d->content_rev++;
                np_update_title(a);
            }
        }
        np_keep_editors(a, ui);        /* keep the sibling tabs' text editors alive */
    }
}

/* ---- Unified preview ------------------------------------------------ */

/* Embedded WebView callback: event 0 = ready, 1 = runtime missing/failed. */
static void np_webview_event(int event) {
    g.wv_available = (event == 0);
    if (g.ui) cui_request_redraw(g.ui);
}

/* Create the embedded WebView once, bound to the top-level editor window.
 * Each platform uses its OS-provided engine (WebView2 / WKWebView / WebKitGTK);
 * no heavy runtime is bundled. */
static void np_webview_boot(notepad *a) {
    if (a->wv_init_done) return;
    a->wv_init_done = true;
    void *native = cui_app_native_window(NULL);
    if (!native) return;
    np_webview_init(native, np_webview_event);
}

/* ---- Android lightweight LaTeX preview (KaTeX in the WebView) ------- */

static void np_preview_stop_http(notepad *a);   /* defined below */

/* True when LaTeX preview should render through the embedded WebView
 * instead of compiling with a TeX engine: Android has no TeX toolchain,
 * so the compile-based preview can never work there. */
static bool np_tex_lite_active(void) {
#ifdef __ANDROID__
    return true;
#else
    return false;
#endif
}

#ifdef __ANDROID__
/* KaTeX files bundled into the APK (android/app/src/main/assets/katex). */
static const char *const np_katex_assets[] = {
    "katex/katex.min.js",
    "katex/katex.min.css",
    "katex/fonts/KaTeX_AMS-Regular.woff2",
    "katex/fonts/KaTeX_Caligraphic-Bold.woff2",
    "katex/fonts/KaTeX_Caligraphic-Regular.woff2",
    "katex/fonts/KaTeX_Fraktur-Bold.woff2",
    "katex/fonts/KaTeX_Fraktur-Regular.woff2",
    "katex/fonts/KaTeX_Main-Bold.woff2",
    "katex/fonts/KaTeX_Main-BoldItalic.woff2",
    "katex/fonts/KaTeX_Main-Italic.woff2",
    "katex/fonts/KaTeX_Main-Regular.woff2",
    "katex/fonts/KaTeX_Math-BoldItalic.woff2",
    "katex/fonts/KaTeX_Math-Italic.woff2",
    "katex/fonts/KaTeX_SansSerif-Bold.woff2",
    "katex/fonts/KaTeX_SansSerif-Italic.woff2",
    "katex/fonts/KaTeX_SansSerif-Regular.woff2",
    "katex/fonts/KaTeX_Script-Regular.woff2",
    "katex/fonts/KaTeX_Size1-Regular.woff2",
    "katex/fonts/KaTeX_Size2-Regular.woff2",
    "katex/fonts/KaTeX_Size3-Regular.woff2",
    "katex/fonts/KaTeX_Size4-Regular.woff2",
    "katex/fonts/KaTeX_Typewriter-Regular.woff2",
};

/* Extract KaTeX assets from the APK into <base>/latex_lite on first use. */
static bool np_tex_lite_extract_assets(const char *dir) {
    bool js_ok = false;
    char dst[NP_PATH_CAP];
    for (size_t i = 0; i < sizeof np_katex_assets / sizeof *np_katex_assets; i++) {
        const char *asset = np_katex_assets[i];
        snprintf(dst, sizeof dst, "%s/%s", dir, asset);
        struct stat st;
        if (stat(dst, &st) == 0) {
            js_ok |= strstr(asset, "katex.min.js") != NULL;
            continue;
        }
        SDL_RWops *src = SDL_RWFromFile(asset, "rb");   /* APK asset */
        if (!src) continue;
        FILE *f = fopen(dst, "wb");
        if (f) {
            char buf[8192];
            size_t n;
            while ((n = SDL_RWread(src, buf, 1, sizeof buf)) > 0)
                fwrite(buf, 1, n, f);
            fclose(f);
            js_ok |= strstr(asset, "katex.min.js") != NULL;
        }
        SDL_RWclose(src);
    }
    return js_ok;
}

/* Regenerate <dir>/index.html from the current buffer. */
static void np_tex_lite_generate(notepad *a, np_doc *d, const char *dir) {
    size_t n = 0;
    char *html = np_latex_html(d->text, d->len, &n);
    if (!html) return;
    char tmp[NP_PATH_CAP], final[NP_PATH_CAP];
    snprintf(tmp, sizeof tmp, "%s/index.tmp", dir);
    snprintf(final, sizeof final, "%s/index.html", dir);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fwrite(html, 1, n, f);
        fclose(f);
        rename(tmp, final);                      /* atomic-ish swap */
    }
    free(html);
    a->lite_rev = d->content_rev;
}

/* Drive the lightweight preview: start the server on first open, then
 * regenerate with a short debounce when the buffer changes (reload is
 * issued by the pane via the WebView). */
static void np_tex_lite_sync(notepad *a, np_doc *d, bool owner_changed) {
    const char *base = SDL_AndroidGetInternalStoragePath();
    if (!base || !base[0]) return;
    char dir[NP_PATH_CAP];
    snprintf(dir, sizeof dir, "%s/latex_lite", base);

    if (owner_changed || !np_http_running(&a->web_server)) {
        np_preview_stop_http(a);
        char sub[NP_PATH_CAP];
        snprintf(sub, sizeof sub, "%s/latex_lite", base);
        mkdir(sub, 0755);
        snprintf(sub, sizeof sub, "%s/latex_lite/katex", base);
        mkdir(sub, 0755);
        snprintf(sub, sizeof sub, "%s/latex_lite/katex/fonts", base);
        mkdir(sub, 0755);
        np_tex_lite_extract_assets(dir);
        np_tex_lite_generate(a, d, dir);
        np_http_start(&a->web_server, dir);
        snprintf(a->preview_owner, sizeof a->preview_owner, "%s", d->path);
        a->wv_url[0] = 0;                        /* force a fresh navigate */
        a->lite_due = 0;
        return;
    }
    if (d->content_rev != a->lite_rev && a->lite_due == 0)
        a->lite_due = SDL_GetTicks() + 1500;     /* debounce while typing */
    if (a->lite_due && SDL_GetTicks() >= a->lite_due) {
        np_tex_lite_generate(a, d, dir);
        a->lite_due = 0;
        if (a->wv_url[0]) np_webview_reload();
    }
}
#endif /* __ANDROID__ */

/* Every frame: place the native WebView view exactly over the reserved host
 * box when an HTML (or lightweight LaTeX) document is being previewed,
 * otherwise hide it. CUI reports
 * rectangles in logical points; the bridge expects physical pixels with the
 * origin at the top-left (each backend flips its own coordinate system). */
static void np_sync_webview(notepad *a, cui_ctx *ui) {
    if (!a->wv_init_done) return;
    bool lite = a->preview_kind == NP_PREVIEW_LATEX && np_tex_lite_active();
    /* The WebView is a native child window: the OS composites it above this
     * window's CUI-drawn client area, so any modal dialog / menu / popover
     * would be painted over by the page. Hide it while an overlay is open so
     * the drawn dialog stays on top; it reappears on the next frame. */
    bool want = a->show_preview && np_webview_ready() &&
                !cui_overlay_open(ui) &&
                (a->preview_kind == NP_PREVIEW_HTML || lite);
    cui_id host = cui_make_id(ui, "wv-host");
    cui_rect r = cui_get_rect(ui, host);
    if (!want || r.w <= 0 || r.h <= 0) {
        np_webview_set_visible(false);
        return;
    }
    float s = cui_dpi_scale(ui);
    if (s <= 0) s = 1.0f;
    int x = (int)(r.x * s + 0.5f), y = (int)(r.y * s + 0.5f);
    int w = (int)(r.w * s + 0.5f), h = (int)(r.h * s + 0.5f);
    np_webview_set_bounds(x, y, w, h);
    np_webview_set_visible(true);
}

/* Resolve whether the active document can be previewed, and start/stop the
 * async worker when the document or its content changes. */
static np_preview_kind np_preview_for_doc(notepad *a, np_doc *d) {
    if (!d || d->is_hex) return NP_PREVIEW_OFF;
    np_preview_kind k = np_preview_detect(d->path);
    if (k == NP_PREVIEW_OFF && d->path[0]) {
        /* Multi-file LaTeX: style/class/bib sources belong to a LaTeX project
         * and preview its main .tex, even though their own extension is not .tex. */
        const char *dot = strrchr(d->path, '.');
        if (dot && np_ext_in("cls;sty;bib;bst;aux", dot + 1)) k = NP_PREVIEW_LATEX;
    }
    if (k == NP_PREVIEW_OFF) {
        /* Untitled buffers: sniff markdown/LaTeX from their text. */
        if (d->len > 0) {
            if (strstr(d->text, "\\documentclass") || strstr(d->text, "\\begin{document}"))
                k = NP_PREVIEW_LATEX;
            else if (strstr(d->text, "\n# ") || strstr(d->text, "**") ||
                     strstr(d->text, "```"))
                k = NP_PREVIEW_MARKDOWN;
        }
    }
    return k;
}

static void np_preview_stop_http(notepad *a) {
    if (np_http_running(&a->web_server)) np_http_stop(&a->web_server);
}

static void np_preview_release_tex_image(notepad *a) {
    if (a->preview_tex) {
        cui_image_destroy(a->ui, a->preview_tex);
        a->preview_tex = 0;
        a->preview_tex_page = 0;
    }
}

/* Called each frame while the preview is open. Kicks off compilation on
 * demand (first open, file switch, or a save bumped content_rev) and keeps
 * the HTTP server pointed at the active HTML file's directory. */
static void np_preview_sync(notepad *a, np_doc *d) {
    if (!d) return;
    np_preview_kind k = np_preview_for_doc(a, d);
    a->preview_kind = k;

    bool owner_changed = strcmp(a->preview_owner, d->path) != 0;

    if (k == NP_PREVIEW_LATEX && d->path[0]) {
#ifdef __ANDROID__
        /* No TeX toolchain on Android: render a KaTeX-based lightweight
         * HTML preview through the embedded WebView instead of compiling. */
        np_tex_lite_sync(a, d, owner_changed);
#else
        /* Multi-file projects: compile the project's main .tex, not the
         * chapter/style file currently on screen. */
        np_tex_detect_root(a, d);
        const char *compile_path = d->tex_root[0] ? d->tex_root : d->path;
        /* Compile on first open / doc switch only. Subsequent rebuilds are
         * driven by Ctrl+S (np_save resets the job) or the manual button --
         * NOT on every keystroke, per the "save + manual" policy. */
        if (owner_changed || a->tex_job.state == 0) {
            np_preview_release_tex_image(a);
            np_tex_job_free(&a->tex_job);       /* join any old worker */
            np_tex_job_init(&a->tex_job);
            np_tex_compile(&a->tex_job, compile_path);
            a->tex_job.content_seen = d->content_rev;
            snprintf(a->preview_owner, sizeof a->preview_owner, "%s", compile_path);
        }
#endif /* __ANDROID__ */
    } else if (k == NP_PREVIEW_PDF && d->path[0]) {
        if (owner_changed || a->tex_job.state == 0) {
            np_preview_release_tex_image(a);
            np_tex_job_free(&a->tex_job);
            np_tex_job_init(&a->tex_job);
            np_tex_render_pdf(&a->tex_job, d->path);
            a->tex_job.content_seen = d->content_rev;
            snprintf(a->preview_owner, sizeof a->preview_owner, "%s", d->path);
        }
    } else if (k == NP_PREVIEW_HTML && d->path[0]) {
        if (owner_changed || !np_http_running(&a->web_server)) {
            np_preview_stop_http(a);
            char dir[NP_PATH_CAP];
            np_dir_of(d->path, dir, sizeof dir);
            np_http_start(&a->web_server, dir);
            snprintf(a->preview_owner, sizeof a->preview_owner, "%s", d->path);
        }
    } else {
        if (owner_changed) {
            np_preview_stop_http(a);
            /* Release a stale PDF page texture when leaving a tex/pdf doc. */
            np_preview_release_tex_image(a);
            a->preview_owner[0] = 0;
        }
    }
}

/* ---- Preview -> source selection helpers --------------------------- */

static bool np_is_ws_byte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

/* Locate `text` in the document. First an exact byte match, then a match that
 * ignores runs of whitespace (HTML textContent collapses them differently). */
static int np_source_span(const char *src, int len, const char *text, int *span) {
    *span = 0;
    if (!src || !text || !*text) return -1;
    size_t nl = strlen(text);
    if (nl > (size_t)len) return -1;
    for (int i = 0; i + (int)nl <= len; i++) {
        if (!memcmp(src + i, text, nl)) { *span = (int)nl; return i; }
    }
    char *a = (char *)malloc((size_t)len + 1);
    char *b = (char *)malloc(nl + 1);
    int *map = (int *)malloc(((size_t)len + 1) * sizeof(int));
    if (!a || !b || !map) { free(a); free(b); free(map); return -1; }
    int an = 0;
    for (int i = 0; i < len; i++) {
        if (np_is_ws_byte(src[i])) {
            if (an > 0 && !np_is_ws_byte(a[an - 1])) { a[an] = ' '; map[an] = i; an++; }
        } else { a[an] = src[i]; map[an] = i; an++; }
    }
    a[an] = 0;
    int bn = 0;
    for (size_t i = 0; i < nl; i++) {
        if (np_is_ws_byte(text[i])) {
            if (bn > 0 && !np_is_ws_byte(b[bn - 1])) b[bn++] = ' ';
        } else b[bn++] = text[i];
    }
    b[bn] = 0;
    int off = -1;
    if (an > 0 && bn > 0) {
        char *p = strstr(a, b);
        if (p) {
            int bi = (int)(p - a);
            int last = bi + bn - 1;
            off = map[bi];
            *span = map[last] + 1 - off;
        }
    }
    free(a); free(b); free(map);
    return off;
}

static void np_select_span(notepad *a, np_doc *d, int off, int span) {
    if (!d || off < 0 || off > d->len) return;
    if (span <= 0) span = 0;
    if (off + span > d->len) span = d->len - off;
    cui_code_editor_select(a->ui, d->editor_id, off, off + span);
}

static void np_select_line(notepad *a, np_doc *d, int line) {
    if (!d || line < 1) return;
    int cur = 1, start = 0;
    for (int i = 0; i < d->len && cur < line; i++) {
        if (d->text[i] == '\n') { start = i + 1; cur++; }
    }
    int end = start;
    while (end < d->len && d->text[end] != '\n' && d->text[end] != '\r') end++;
    np_select_span(a, d, start, end - start);
}

static bool np_same_path(const char *a, const char *b) {
    if (!a || !b) return false;
    for (; *a && *b; a++, b++) {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
        if (ca != cb) return false;
    }
    return !*a && !*b;
}

static void np_goto_tex_source(notepad *a, const char *file, int line) {
    if (!file || !*file || line < 1) return;
    for (int i = 0; i < a->count; i++) {
        np_doc *d = a->docs[i];
        if (np_same_path(d->path, file) ||
            (!d->path[0] && np_same_path(d->tab_title, file))) {
            np_select_line(a, d, line);
            return;
        }
    }
    char full[NP_PATH_CAP];
    snprintf(full, sizeof full, "%s", file);
    if (full[0] != '/' && !(strlen(full) > 1 && full[1] == ':')) {
        np_doc *d = np_cur(a);
        const char *root = (d && d->tex_root[0]) ? d->tex_root : (d ? d->path : NULL);
        if (root && root[0]) {
            char dir[NP_PATH_CAP];
            np_dir_of(root, dir, sizeof dir);
            snprintf(full, sizeof full, "%s/%s", dir, file);
        }
    }
    if (np_file_exists(full) && np_open_path(a, full, true)) {
        np_doc *d = np_cur(a);
        if (d) np_select_line(a, d, line);
    }
}

static void np_preview_image_paint(cui_ctx *ui, cui_rect r, void *user) {
    cui_texture tex = *(cui_texture *)user;
    if (!tex) return;
    cui_rect uv = { 0, 0, 1, 1 };
    cui_draw_image(ui, tex, r, uv, CUI_CORNERS(0), (cui_color){ 255, 255, 255, 255 });
}

/* Right-hand pane. Renders the preview kinds into the same slot. */

/* Shared WebView pane body: URL row + reserved host box. Used for HTML
 * files and for the Android lightweight LaTeX preview (lite=true: the
 * refresh button regenerates the HTML first). */
static void np_web_preview_pane(cui_ctx *ui, notepad *a, const char *file_name,
                                bool lite) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 6, .pad = 6) {
        if (np_http_running(&a->web_server)) {
            char url[512];
            snprintf(url, sizeof url, "http://127.0.0.1:%d/%s",
                     a->web_server.port, file_name);
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_input(ui, "web-preview-url", url, sizeof url, .read_only = true,
                          .width = CUI_FILL);
                if (cui_button(ui, "刷新", .size = CUI_SIZE_SM,
                               .tooltip = lite ? "重新生成轻量预览" : "重新载入 Web 预览")) {
                    if (lite) a->lite_due = 1;           /* regen next frame */
                    else np_webview_reload();
                }
                if (cui_button(ui, "浏览器打开", .size = CUI_SIZE_SM,
                               .icon = CUI_ICON_EXTERNAL_LINK,
                               .tooltip = "在系统默认浏览器中打开"))
                    np_open_url(url);
            }
            if (a->wv_available) {
                /* Push the URL once per URL change. The native view is
                 * positioned over this box every frame by
                 * np_sync_webview() at the end of np_frame(). */
                if (strcmp(a->wv_url, url) != 0) {
                    np_webview_navigate(url);
                    snprintf(a->wv_url, sizeof a->wv_url, "%s", url);
                }
                /* Reserved host rectangle; the native WebView paints
                 * on top of it. */
                cui_box(ui, .id = "wv-host", .width = CUI_FILL,
                        .height = CUI_FILL, .bg = CUI_COLOR_SURFACE);
            } else {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .align = CUI_ALIGN_CENTER,
                           .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
                    cui_text(ui, "内嵌 Web 预览不可用", .size = 13,
                             .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_WARNING);
                    cui_text(ui, "未检测到系统 Web 引擎。Windows 需 Edge WebView2 运行时；"
                                 "macOS / Linux 需系统 WebKit。也可在下方用系统浏览器打开。",
                             .size = 11, .color = CUI_COLOR_TEXT_MUTED,
                             .nowrap = false);
                    if (cui_button(ui, "在系统浏览器打开", .variant = CUI_BUTTON_PRIMARY))
                        np_open_url(url);
                }
            }
        } else {
            cui_text(ui, "无法启动本地 HTTP 服务器。", .size = 12,
                     .color = CUI_COLOR_DANGER);
        }
    }
}

static void np_preview_pane(cui_ctx *ui, notepad *a, np_doc *d) {
    np_preview_sync(a, d);

    if (a->preview_kind == NP_PREVIEW_HTML ||
        (a->preview_kind == NP_PREVIEW_LATEX && np_tex_lite_active())) {
        char clicked[512];
        if (np_http_take_click(&a->web_server, clicked, sizeof clicked)) {
            int span = 0;
            int off = np_source_span(d->text, d->len, clicked, &span);
            if (off >= 0) np_select_span(a, d, off, span);
        }
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 8, .gap = 6,
               .bg = CUI_COLOR_SURFACE) {
        char head[160];
        snprintf(head, sizeof head, "预览 · %s%s",
                 np_preview_kind_name(a->preview_kind),
                 (a->preview_kind == NP_PREVIEW_LATEX && np_tex_lite_active())
                     ? "（轻量）" : "");
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, head, .size = 12, .font = CUI_FONT_MEDIUM);
            cui_spacer(ui);
            if (cui_button(ui, "折叠", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST, .icon = CUI_ICON_PANEL_LEFT,
                           .tooltip = "折叠预览面板"))
                a->show_preview = false;
        }
        cui_separator(ui);

        np_preview_kind k = a->preview_kind;
        if (k == NP_PREVIEW_OFF) {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                       .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
                cui_text(ui, "此文件类型没有可用预览", .size = 13,
                         .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_TEXT_MUTED);
                cui_text(ui, "支持 Markdown（实时）、LaTeX / PDF（编译预览）、HTML（Web 预览）",
                         .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
        } else if (k == NP_PREVIEW_MARKDOWN) {
            /* cui_markdown re-parses from the frame arena every frame: the
             * view updates live as the left editor's buffer changes. */
            cui_scroll(ui, .id = "md-preview", .width = CUI_FILL, .height = CUI_FILL,
                       .pad = 6) {
            cui_markdown_result mr = cui_markdown_ex(
                ui, "md-preview-doc", d->text,
                &(cui_markdown_opts){ .length = d->len, .width = CUI_FILL });
            if (mr.source_offset >= 0 && mr.source_offset <= d->len)
                np_select_span(a, d, mr.source_offset, mr.source_end - mr.source_offset);
            }
        } else if (k == NP_PREVIEW_LATEX) {
#ifdef __ANDROID__
            if (np_tex_lite_active()) {
                np_web_preview_pane(ui, a, "index.html", true);
            } else
#endif
            {
            int st = a->tex_job.state;
            if (st == 1) {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
                    cui_spinner(ui, 22);
                    cui_text(ui, "正在用 LaTeX 编译并渲染 PDF…", .size = 12);
                    cui_text(ui, "首次编译可能需要几秒，界面仍可操作。", .size = 11,
                             .color = CUI_COLOR_TEXT_MUTED);
                }
            } else if (st == 2 && a->tex_job.pages > 0) {
                int page = NP_MIN(NP_MAX(a->preview_tex_page ? a->preview_tex_page : 1, 1),
                                  a->tex_job.pages);
                char page_path[NP_PATH_CAP];
                if (!a->preview_tex || a->preview_tex_page != page) {
                    if (np_tex_page_path(&a->tex_job, page, page_path, sizeof page_path)) {
                        np_preview_release_tex_image(a);
                        int iw = 0, ih = 0;
                        cui_texture t = cui_image_load(ui, page_path, &iw, &ih);
                        if (t) {
                            a->preview_tex = t;
                            a->preview_tex_page = page;
                        }
                    }
                }
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    if (cui_button(ui, "上一页", .size = CUI_SIZE_SM,
                                   .disabled = page <= 1)) {
                        np_preview_release_tex_image(a);
                        a->preview_tex_page = page - 1;
                    }
                    char pg[48];
                    snprintf(pg, sizeof pg, "%d / %d", page, a->tex_job.pages);
                    cui_text(ui, pg, .size = 12, .font = CUI_FONT_MONO);
                    if (cui_button(ui, "下一页", .size = CUI_SIZE_SM,
                                   .disabled = page >= a->tex_job.pages)) {
                        np_preview_release_tex_image(a);
                        a->preview_tex_page = page + 1;
                    }
                    cui_spacer(ui);
                    if (cui_button(ui, "重新编译", .size = CUI_SIZE_SM,
                                   .icon = CUI_ICON_REFRESH_CW, .disabled = !d->path[0])) {
                        np_preview_release_tex_image(a);
                        np_tex_job_free(&a->tex_job);
                        np_tex_job_init(&a->tex_job);
                        np_tex_compile(&a->tex_job, d->path);
                        a->tex_job.content_seen = d->content_rev;
                    }
                }
                if (a->preview_tex) {
                    /* Clicking the rendered page uses SyncTeX to jump to the
                     * exact source line in the editor. */
                    cui_canvas_result cv = cui_canvas(
                        ui, "tex-preview-page", CUI_FILL, CUI_FILL,
                        np_preview_image_paint, &a->preview_tex,
                        sizeof a->preview_tex);
                    if (cv.clicked && cv.rect.w > 0 && cv.rect.h > 0) {
                        float u = cv.mouse.x / cv.rect.w;
                        float v = cv.mouse.y / cv.rect.h;
                        if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                            float pw = a->tex_job.page_w > 0 ? a->tex_job.page_w : 595.28f;
                            float ph = a->tex_job.page_h > 0 ? a->tex_job.page_h : 841.89f;
                            char file[NP_PATH_CAP];
                            int line = 0;
                            if (np_tex_locate_source(&a->tex_job, page, u * pw, v * ph,
                                                     file, sizeof file, &line))
                                np_goto_tex_source(a, file, line);
                        }
                    }
                }
            } else {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
                    cui_text(ui, "预览失败", .size = 13, .font = CUI_FONT_MEDIUM,
                             .color = CUI_COLOR_DANGER);
                    if (a->tex_job.log[0]) {
                        if (cui_button(ui, "让 AI 分析编译错误",
                                       .variant = CUI_BUTTON_PRIMARY,
                                       .disabled = a->ai_done == 1))
                            np_ai_fix_compile(a);
                        cui_text(ui, "编译日志会随当前文档一起发送，修复结果可逐处接受。",
                                 .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, a->tex_job.log, .size = 11, .nowrap = false,
                                 .font = CUI_FONT_MONO, .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            }
            }
        } else if (k == NP_PREVIEW_PDF) {
            int st = a->tex_job.state;
            if (st == 1) {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
                           .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
                    cui_spinner(ui, 22);
                    cui_text(ui, "正在渲染 PDF…", .size = 12);
                }
            } else if (st == 2 && a->tex_job.pages > 0) {
                int page = NP_MIN(NP_MAX(a->preview_tex_page ? a->preview_tex_page : 1, 1),
                                  a->tex_job.pages);
                char page_path[NP_PATH_CAP];
                if (!a->preview_tex || a->preview_tex_page != page) {
                    np_preview_release_tex_image(a);
                    if (np_tex_page_path(&a->tex_job, page, page_path, sizeof page_path)) {
                        int iw = 0, ih = 0;
                        a->preview_tex = cui_image_load(ui, page_path, &iw, &ih);
                        if (a->preview_tex) a->preview_tex_page = page;
                    }
                }
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    if (cui_button(ui, "上一页", .size = CUI_SIZE_SM, .disabled = page <= 1)) {
                        np_preview_release_tex_image(a); a->preview_tex_page = page - 1;
                    }
                    char pg[48];
                    snprintf(pg, sizeof pg, "%d / %d", page, a->tex_job.pages);
                    cui_text(ui, pg, .size = 12, .font = CUI_FONT_MONO);
                    if (cui_button(ui, "下一页", .size = CUI_SIZE_SM, .disabled = page >= a->tex_job.pages)) {
                        np_preview_release_tex_image(a); a->preview_tex_page = page + 1;
                    }
                }
                if (a->preview_tex)
                    cui_canvas(ui, "pdf-preview-page", CUI_FILL, CUI_FILL,
                               np_preview_image_paint, &a->preview_tex,
                               sizeof a->preview_tex);
            } else {
                cui_text(ui, a->tex_job.log[0] ? a->tex_job.log : "PDF 渲染失败。",
                         .size = 11, .color = CUI_COLOR_TEXT_MUTED, .font = CUI_FONT_MONO);
            }
        } else if (k == NP_PREVIEW_HTML) {
            np_web_preview_pane(ui, a,
                                d->path[0] ? np_base(d->path) : "index.html", false);
        }
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
            snprintf(t->tab_title, sizeof t->tab_title, "%s%s###npdoc%u",
                     t->disk_missing ? "⚠ " : "", name, t->uid);
            char sz[48];
            np_size_str(sz, sizeof sz, t->path[0] ? t->file_size : (size_t)t->len);
            bool stale = t->disk_mtime_seen != 0 &&
                         t->disk_mtime_seen != t->disk_mtime;
            snprintf(t->tab_tip, sizeof t->tab_tip, "%s%s  ·  %s  ·  %s%s%s%s",
                     t->is_hex ? "[十六进制] " : "",
                     t->path[0] ? t->path : "尚未保存到文件", sz,
                     t->is_hex ? "二进制" : NP_LANGS[t->lang].label,
                     t->dirty ? "  ·  未保存" : "",
                     stale ? "  ·  磁盘内容已变化" : "",
                     t->disk_missing ? "  ·  磁盘文件已删除" : "");
            int fk = np_doc_family(t);
            tabs[i] = (cui_doc_tab){ .title = t->tab_title,
                                     .icon = np_family_icon(fk),
                                     .icon_color = np_family_tint(ui, fk),
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

        /* Immersive mode: the menu bar is hidden, so touch devices (no F11)
         * need a small, quiet way out - one ghost icon right of the tabs. */
        if (a->zen) {
            cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 2, .pad_right = 6) {
                cui_spacer(ui);
                if (cui_icon_button(ui, CUI_ICON_MINIMIZE_2,
                                    .tooltip = "退出沉浸式编辑  F11",
                                    .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM))
                    np_zen_set(a, false);
            }
        }

        np_doc *d = np_cur(a);
        bool welcome = a->count == 1 && !d->is_hex && !a->welcome_dismissed &&
                       np_blank_untitled(d);

        /* Foldable preview on the right: editor | preview. Both branches
         * render the editor exactly once so CUI editor state stays stable. */
        bool previewable = a->show_preview && !welcome && !d->is_hex;
        if (previewable) {
            cui_split(ui, "np_preview_split", &a->preview_ratio, .vertical = false,
                      .min_first = 300, .min_second = 260) {
                /* left: editor (+ optional bottom output pane) */
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
                cui_split_next(ui);
                np_preview_pane(ui, a, d);
            }
        } else {
            if (a->show_output) {
                cui_split(ui, "np_out_split", &a->out_ratio, .vertical = true,
                          .min_first = 140, .min_second = 100) {
                    if (welcome) np_welcome_pane(ui, a);
                    else if (d->is_hex) np_hex_pane(ui, a, d);
                    else np_editor_pane(ui, a, d);
                    cui_split_next(ui);
                    np_output_panel(ui, a);
                }
            } else {
                if (welcome) np_welcome_pane(ui, a);
                else if (d->is_hex) np_hex_pane(ui, a, d);
                else np_editor_pane(ui, a, d);
            }
        }
    }
    return np_cur(a);
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

/* ---- AI native panel ---------------------------------------------------- */

/* What happens to a finished AI result, per task. */
enum {                      /* ai_task values:                    */
    NP_AI_DISP_DIFF = 0,    /* 0 polish 1 translate 4 custom      */
    NP_AI_DISP_ANSWER = 1,  /* 2 check 3 explain                  */
    NP_AI_DISP_INSERT = 2,  /* 5 completion (insert at caret)     */
    NP_AI_DISP_TEMPLATE = 3,/* 6 template (new doc or insert)     */
};
static int np_ai_disp(int task) {
    switch (task) {
        case 0: case 1: case 4: return NP_AI_DISP_DIFF;
        case 5: return NP_AI_DISP_INSERT;
        case 6: return NP_AI_DISP_TEMPLATE;
        default: return NP_AI_DISP_ANSWER;
    }
}

/* Default instruction per task and document family. LaTeX gets its original
 * careful prompts; everything else now gets prompts that fit the content. */
static const char *np_ai_default_inst(notepad *a, int task, const np_doc *d) {
    if (task >= 7) {                    /* user-saved custom action */
        int i = task - 7;
        return (a && i >= 0 && i < a->ai_action_count) ? a->ai_actions[i].inst : NULL;
    }
    bool latex = np_doc_is_latex(d);
    bool md = !latex && np_doc_is_markdown(d);
    bool code = !latex && !md && d && d->lang > NP_LANG_TEXT && d->lang < NP_LANG_COUNT;
    switch (task) {
        case 0:
            if (latex) return "请润色这段 LaTeX：改进措辞与表达，保持所有命令、公式和结构与原意不变。只输出润色后的完整源码。";
            if (code) return "请润色这段代码：改善命名与可读性，补全必要注释，不改变行为、接口和依赖。只输出完整代码。";
            if (md) return "请润色这份 Markdown：改进表达与结构，保留标题、链接、列表和代码块。只输出完整内容。";
            return "请润色这段文本：改进措辞与流畅度，保持原意、语气和段落结构。只输出完整文本。";
        case 1:
            if (latex) return "请把这段 LaTeX 翻译成中文，保持所有 LaTeX 命令、公式和环境不变。只输出翻译后的完整源码。";
            if (code) return "请把这段代码中的注释和字符串翻译成中文，保持所有标识符、语法和行为不变。只输出完整代码。";
            return "请把这段内容翻译成中文，保留原有格式、标记和结构。只输出完整内容。";
        case 2:
            if (latex) return "请检查这段 LaTeX 的错误与问题（语法、宏包、引用、公式），逐条列出问题与建议。不要修改原文。";
            if (code) return "请检查这段代码的错误和边界条件（语法、空值、异常、资源释放），逐条列出问题与建议。不要修改原文。";
            if (md) return "请检查这份 Markdown 的格式、链接、错别字和结构问题，逐条列出问题与建议。不要修改原文。";
            return "请检查这段文本中的错别字、语法和逻辑问题，逐条列出问题与建议。不要修改原文。";
        case 3:
            if (code) return "请解释这段代码的作用、关键流程和潜在副作用。不要修改原文。";
            if (latex) return "请解释这段 LaTeX 的作用与结构。不要修改原文。";
            return "请解释这段内容的主旨与结构。不要修改原文。";
        case 5:
            if (latex) return "请续写下面的 LaTeX 文档。只输出紧接在文末之后的续写内容，不要重复已有文本，不要任何解释或代码块标记。";
            if (code) return "请从光标处续写这段代码，风格与上下文保持一致。只输出可直接粘贴的代码，不要解释或代码块标记。";
            return "请从光标处续写这段内容，语气与风格保持一致。只输出续写文本，不要重复已有内容或解释。";
        case 6:
            if (latex) return "请生成一个可直接编译的 LaTeX 模板（从 \\documentclass 到 \\end{document} 的完整文档）。只输出完整源码，不要解释。需求：";
            if (code) return "请生成一个可直接运行的代码文件模板，包含必要导入和清晰的分段注释。只输出完整代码，不要解释。需求：";
            if (md) return "请生成一份结构完整的 Markdown 文档模板（标题、摘要、章节、列表与表格）。只输出完整内容，不要解释。需求：";
            return "请生成一份结构完整的文本模板，带清晰的分区和占位说明。只输出完整内容，不要解释。需求：";
        default:
            return NULL;
    }
}

/* One-click AI action from the editor context menu: sets the task, fills
 * the default instruction, opens the panel and fires the request. */
static void np_ai_quick(notepad *a, int task) {
    a->ai_task = task;
    const char *def = np_ai_default_inst(a, task, np_cur(a));
    if (def) snprintf(a->ai_inst, sizeof a->ai_inst, "%s", def);
    a->ai_ctx = 0;                       /* act on the selection / caret */
    a->ai_ctx_tabs = false;              /* quick actions never pull other sources */
    a->ai_ctx_finds = false;
    a->show_ai = true;
    np_ai_start(a);
}

/* Send a failed LaTeX compile to the AI: the log becomes an extra prompt
 * block and the source is sent in full. The answer follows the normal Diff
 * review path, so nothing is written without an explicit accept. */
static void np_ai_fix_compile(notepad *a) {
    if (a->ai_done == 1 || !a->tex_job.log[0]) return;
    np_doc *d = np_cur(a);
    if (!d || d->is_hex) return;
    a->ai_task = 4;                     /* custom instruction, Diff display */
    a->ai_ctx = 1;                      /* whole document, not just selection */
    a->ai_ctx_tabs = false;             /* only the compile log is extra here */
    a->ai_ctx_finds = false;
    snprintf(a->ai_inst, sizeof a->ai_inst,
             "请根据下面的 LaTeX 编译日志定位并修复源码错误。只输出修复后的完整源码，不要解释或代码块标记。");
    snprintf(a->ai_extra, sizeof a->ai_extra, "--- 编译日志 ---\n%s", a->tex_job.log);
    a->show_ai = true;
    np_ai_start(a);
    np_toast(a, "已提交给 AI", "编译日志已随源码发送，请在面板中逐处确认修改",
             CUI_TONE_INFO);
}

/* Put the AI result into a brand-new document (template generation). */
static bool np_ai_new_doc_from_result(notepad *a) {
    if (a->ai_done != 2 || !a->ai_result[0]) return false;
    np_doc *nd = np_doc_new(a);
    if (!nd) return false;
    size_t n = strlen(a->ai_result);
    if (!np_ensure_cap(nd, n + 1)) {
        np_close_doc(a, a->count - 1);
        np_toast(a, "内存不足", "无法载入 AI 模板", CUI_TONE_DANGER);
        return false;
    }
    memcpy(nd->text, a->ai_result, n + 1);
    nd->len = (int)n;
    np_text_changed(a, nd);
    return true;
}

/* Byte offset of a 0-based line in a NUL-terminated text buffer. */
static size_t np_text_line_offset(const char *s, int line) {
    size_t off = 0;
    int cur = 0;
    while (s[off] && cur < line) {
        if (s[off] == '\n') cur++;
        off++;
    }
    return off;
}

/* Replace `old_count` lines of dst with `new_count` lines from src. Both
 * ranges are 0-based line indices from cui_diff_compute(). */
static bool np_patch_text_hunk(char *dst, size_t cap,
                               int old_start, int old_count,
                               const char *src, int new_start, int new_count) {
    size_t os = np_text_line_offset(dst, old_start);
    size_t oe = np_text_line_offset(dst, old_start + old_count);
    size_t ns = np_text_line_offset(src, new_start);
    size_t ne = np_text_line_offset(src, new_start + new_count);
    size_t tail = strlen(dst + oe);
    size_t need = os + (ne - ns) + tail + 1;
    if (need > cap) return false;
    memmove(dst + os + (ne - ns), dst + oe, tail + 1);
    memcpy(dst + os, src + ns, ne - ns);
    return true;
}

/* Accept or reject the diff hunk currently selected in the AI panel.
 * Accepting patches the staged text toward the AI result; rejecting patches
 * the result back toward the staged text. The diff then shrinks accordingly. */
static bool np_ai_hunk_decision(notepad *a, bool accept) {
    cui_diff_hunk hunks[NP_AI_HUNK_MAX];
    cui_diff_stats stats;
    int n = cui_diff_compute(a->ai_staged, strlen(a->ai_staged),
                             a->ai_result, strlen(a->ai_result), 0,
                             hunks, NP_AI_HUNK_MAX, &stats);
    if (n <= 0) return false;
    /* cui_diff_compute returns the REAL hunk count but only fills the first
     * NP_AI_HUNK_MAX entries; clamp to the filled range, not to n. */
    int usable = NP_MIN(n, NP_AI_HUNK_MAX);
    int idx = NP_MAX(0, NP_MIN(a->ai_diff_current, usable - 1));
    const cui_diff_hunk *h = &hunks[idx];
    bool ok = accept
        ? np_patch_text_hunk(a->ai_staged, sizeof a->ai_staged,
                             h->old_start, h->old_count,
                             a->ai_result, h->new_start, h->new_count)
        : np_patch_text_hunk(a->ai_result, sizeof a->ai_result,
                             h->new_start, h->new_count,
                             a->ai_staged, h->old_start, h->old_count);
    if (ok) a->ai_diff_current = idx;
    return ok;
}

/* Keep recent results on the document they belong to, not in one global
 * buffer that gets overwritten when the user switches tabs. */
static void np_ai_push_history(notepad *a) {
    np_doc *d = NULL;
    for (int i = 0; i < a->count; i++)
        if (a->docs[i] && a->docs[i]->uid == a->ai_target_uid) { d = a->docs[i]; break; }
    if (!d) d = np_cur(a);
    if (!d) return;
    if (d->ai_hist_count == NP_AI_HISTORY_MAX) {
        memmove(&d->ai_hist[0], &d->ai_hist[1],
                (size_t)(NP_AI_HISTORY_MAX - 1) * sizeof d->ai_hist[0]);
        d->ai_hist_count--;
    }
    np_ai_history *h = &d->ai_hist[d->ai_hist_count++];
    memset(h, 0, sizeof *h);
    snprintf(h->result, sizeof h->result, "%s", a->ai_result);
    snprintf(h->orig, sizeof h->orig, "%s", a->ai_orig);
    snprintf(h->inst, sizeof h->inst, "%s", a->ai_inst);
    snprintf(h->target_path, sizeof h->target_path, "%s", a->ai_target_path);
    h->target_uid = a->ai_target_uid;
    h->target_rev = a->ai_target_rev;
    h->target_len = a->ai_target_len;
    h->task = a->ai_task;
    h->target_s = a->ai_target_s;
    h->target_e = a->ai_target_e;
    d->ai_hist_sel = d->ai_hist_count - 1;
}

static void np_ai_restore_history(notepad *a, np_doc *d, int idx) {
    if (!d || idx < 0 || idx >= d->ai_hist_count) return;
    const np_ai_history *h = &d->ai_hist[idx];
    snprintf(a->ai_result, sizeof a->ai_result, "%s", h->result);
    snprintf(a->ai_orig, sizeof a->ai_orig, "%s", h->orig);
    snprintf(a->ai_inst, sizeof a->ai_inst, "%s", h->inst);
    snprintf(a->ai_target_path, sizeof a->ai_target_path, "%s", h->target_path);
    snprintf(a->ai_staged, sizeof a->ai_staged, "%s", h->orig);
    a->ai_task = h->task;
    a->ai_target_uid = h->target_uid;
    a->ai_target_rev = h->target_rev;
    a->ai_target_len = h->target_len;
    a->ai_target_s = h->target_s;
    a->ai_target_e = h->target_e;
    a->ai_diff_current = 0;
    a->ai_error[0] = 0;
    a->ai_cancel = false;
    a->ai_done = 2;
    a->ai_hist_pushed = true;
    a->show_ai = true;
}

/* Insert an AI answer at the caret. Deliberately independent of the original
 * target range: an answer task must never replace the entire document. */
static bool np_ai_insert_here(notepad *a) {
    np_doc *d = np_cur(a);
    if (!a->ai_result[0] || !d || d->is_hex) return false;
    int pos = NP_MAX(0, NP_MIN(d->caret, d->len));
    size_t rl = strlen(a->ai_result);
    /* Insert through the editor so Ctrl+Z can undo it and CUI's delta
     * history keeps valid byte offsets (see np_ai_apply). */
    int applied_len = d->len;
    if (!cui_code_editor_replace_external(a->ui, d->editor_id,
                                          d->text, d->cap, d->len,
                                          pos, pos, a->ai_result, (int)rl,
                                          &applied_len)) return false;
    d->len = applied_len;
    np_text_changed(a, d);
    cui_code_editor_select(a->ui, d->editor_id, pos + (int)rl, pos + (int)rl);
    return true;
}

static void np_ai_panel(cui_ctx *ui, notepad *a, np_doc *d) {
    (void)d;
    long st = a->ai_done;
    int disp = np_ai_disp(a->ai_task);
    if (st != a->ai_last_state) { a->ai_last_state = st; cui_request_redraw(ui); }
    if (st == 1) cui_request_redraw(ui);        /* keep the spinner animating */

    const char *ctxs[] = { "当前选区", "整个文件" };
    static const char *tasks_builtin[] = { "润色改写", "翻译", "检查错误", "解释说明",
                                           "补全续写", "生成模板", "自定义指令" };
    const char *tasks[7 + NP_AI_ACTION_MAX];
    int task_count = 0;
    for (int i = 0; i < 7; i++) tasks[task_count++] = tasks_builtin[i];
    for (int i = 0; i < a->ai_action_count && task_count < (int)(sizeof tasks / sizeof tasks[0]); i++)
        tasks[task_count++] = a->ai_actions[i].name;
    if (a->ai_task >= task_count) a->ai_task = 6;
    const char *backs_static[] = { "自动检测", "Claude Code", "Codex", "自定义命令", "MaClaw官方" };
    const char *backs[5 + 8];
    int back_count = 0;
    for (int i = 0; i < 5; i++) backs[back_count++] = backs_static[i];
    for (int i = 0; i < a->llm_prov_count && back_count < (int)(sizeof backs / sizeof backs[0]); i++)
        backs[back_count++] = a->llm_prov[i].name;
    if (a->ai_backend >= back_count) a->ai_backend = 0;
    if (!a->ai_inst[0]) {
        const char *def = np_ai_default_inst(a, a->ai_task, d ? d : np_cur(a));
        if (def) snprintf(a->ai_inst, sizeof a->ai_inst, "%s", def);
    }

    cui_column(ui, .id = "ai-panel", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 8, .pad = 10) {
        /* Header -------------------------------------------------------- */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "AI 助手", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_spacer(ui);
            /* 显示当前激活的服务商，扫码授权后这里直接显示 MaClaw官方。 */
            {
                const char *label = NULL;
                if (a->ai_backend == 4)
                    label = a->maclaw_token[0] ? "MaClaw官方" : "MaClaw官方(未登录)";
                else if (a->ai_backend >= 5 && a->ai_backend - 5 < a->llm_prov_count)
                    label = a->llm_prov[a->ai_backend - 5].name;
                if (label && label[0])
                    cui_text(ui, label, .size = 11, .color = CUI_COLOR_ACCENT);
            }
            if (cui_button(ui, "LLM 服务…", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST))
                a->dlg_llm_prov = true;
            if (cui_button(ui, "✕", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST))
                a->show_ai = false;
        }

        /* Request builder ------------------------------------------------ */
        cui_row(ui, .width = CUI_FILL, .gap = 8) {
            if (cui_select(ui, "ai-task", tasks, task_count, &a->ai_task,
                           .label = "任务", .width = CUI_FILL)) {
                const char *def = np_ai_default_inst(a, a->ai_task, d ? d : np_cur(a));
                if (def) snprintf(a->ai_inst, sizeof a->ai_inst, "%s", def);
            }
            if (cui_select(ui, "ai-ctx", ctxs, 2, &a->ai_ctx,
                           .label = "上下文", .width = CUI_FILL,
                           .disabled = a->ai_task == 5)) { /* caret-based */ }
        }
        if (!a->ai_ctx_summary[0] || a->ai_ctx_src_doc != a->active)
            np_ai_refresh_sources(a);
        cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
            if (cui_checkbox(ui, "其他标签页", &a->ai_ctx_tabs)) np_ai_refresh_sources(a);
            if (cui_checkbox(ui, "查找结果", &a->ai_ctx_finds)) np_ai_refresh_sources(a);
            cui_checkbox(ui, "行内补全", &a->ai_inline);
            cui_spacer(ui);
            static const char *const inline_delays[3] = { "1 秒", "2 秒", "3 秒" };
            int d_idx = a->inline_delay >= 1 && a->inline_delay <= 3
                        ? a->inline_delay - 1 : 0;
            if (cui_select(ui, "inline-delay", inline_delays, 3, &d_idx,
                           .label = "防抖", .disabled = !a->ai_inline))
                a->inline_delay = d_idx + 1;
        }
        /* Inline suggestion preview: the editor only renders the first line
         * of a multi-line ghost, so the full text is shown here to read
         * before accepting it with Tab. */
        if (d && !d->is_hex) {
            const char *gt = cui_code_editor_ghost_text(a->ui, d->editor_id);
            if (gt) {
                cui_column(ui, .width = CUI_FILL, .gap = 4) {
                    cui_text(ui, "行内建议预览（Tab 接受 · Esc 忽略）", .size = 11,
                             .color = CUI_COLOR_TEXT_MUTED, .font = CUI_FONT_MEDIUM);
                    cui_text(ui, gt, .size = 12, .nowrap = false,
                             .color = CUI_COLOR_TEXT_MUTED);
                }
            }
        }
        cui_text(ui, a->ai_ctx_summary, .size = 11,
                 .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
        cui_text(ui, a->ai_inline
                 ? "行内补全：输入停顿后给出灰色续写建议（防抖时长本行可选），Tab 接受，Esc 忽略；Alt+\\、工具条「补全」或右键菜单可手动触发。"
                 : "行内补全已关闭。开启后输入停顿时 AI 在光标处给出续写建议（Tab 接受）；Alt+\\ 随时可手动触发。",
                 .size = 11, .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, a->ai_action_count
                     ? "常用指令已保存为动作，切换任务即可复用。"
                     : "把常用指令保存为动作，以后一键调用。",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
            cui_spacer(ui);
            if (cui_button(ui, "管理动作…", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_GHOST))
                a->dlg_ai_actions = true;
        }
        if (cui_select(ui, "ai-back", backs, back_count, &a->ai_backend,
                       .label = "后端", .width = CUI_FILL)) {
            a->ai_cmd[0] = 0;   /* regenerate default template on next ask */
        }
        if (a->ai_backend == 4) {
            cui_text(ui, a->maclaw_token[0]
                     ? "maclaw 官方服务已授权；请求将以该 maclaw 用户的额度计费。"
                     : "尚未授权 maclaw：请先在「LLM 服务」中扫码登录。",
                     .size = 11,
                     .color = a->maclaw_token[0] ? CUI_COLOR_SUCCESS : CUI_COLOR_WARNING);
        } else if (a->ai_backend >= 5) {
            const np_llm_provider *p = &a->llm_prov[a->ai_backend - 5];
            char hint[640];
            snprintf(hint, sizeof hint, "服务：%s    模型：%s",
                     p->url[0] ? p->url : "（未填写服务地址）",
                     p->model[0] ? p->model : "（未选择模型）");
            cui_text(ui, hint, .size = 11,
                     .color = (p->url[0] && p->model[0])
                                  ? CUI_COLOR_SUCCESS : CUI_COLOR_WARNING);
        } else if (a->ai_backend == 3) {
            cui_textarea(ui, "ai-cmd", a->ai_cmd, sizeof a->ai_cmd,
                         .label = "命令模板（{file}=上下文文件, {out}=结果文件）",
                         .height = 56);
        }
        cui_input(ui, "ai-inst", a->ai_inst, sizeof a->ai_inst,
                  .label = "指令", .placeholder = "告诉 AI 要做什么（任务切换时自动填入默认指令）",
                  .width = CUI_FILL);

        cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
            cui_checkbox(ui, "带上下文追问（多轮）", &a->ai_followup);
            cui_spacer(ui);
        }
        if (a->ai_extra[0]) {
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "已附加：编译日志（会随请求发送，重试时仍保留）",
                         .size = 11, .color = CUI_COLOR_ACCENT, .nowrap = false);
                cui_spacer(ui);
                if (cui_button(ui, "移除", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    a->ai_extra[0] = 0;
            }
        }
        /* In the idle no-result state the empty state below owns the
         * primary action; hiding this row avoids two "Ask AI" buttons. */
        bool ai_idle = (st == 0 || st == 2) && !a->ai_result[0];
        if (!ai_idle) {
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                if (cui_button(ui, st == 1 ? "处理中…" : "Ask AI",
                               .variant = CUI_BUTTON_PRIMARY,
                               .disabled = st == 1 ||
                                           (a->ai_task == 6 && !a->ai_inst[0]))) {
                    np_ai_start(a);
                }
                if (st == 1) {
                    if (cui_button(ui, "停止", .variant = CUI_BUTTON_GHOST))
                        a->ai_cancel = true;
                } else {
                    if (cui_button(ui, "重试", .variant = CUI_BUTTON_GHOST)) {
                        np_ai_start(a);
                    }
                    if (cui_button(ui, "清空对话", .variant = CUI_BUTTON_GHOST)) {
                        a->ai_result[0] = 0; a->ai_done = 0; a->ai_last_state = 0;
                        a->ai_prev[0] = 0;
                        a->ai_staged[0] = 0; a->ai_error[0] = 0;
                        a->ai_extra[0] = 0;
                    }
                }
            }
        }

        /* Result area: takes the rest of the panel ----------------------- */
        if (st == 1) {
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_spinner(ui, 16);
                cui_text(ui, a->ai_stream_len > 0
                         ? "AI 正在生成…（已收到部分内容，可随时停止）"
                         : "AI 正在处理，请稍候…", .size = 12,
                         .color = CUI_COLOR_TEXT_MUTED);
            }
            if (a->ai_stream_len > 0 && a->ai_result[0])
                cui_textarea(ui, "ai-stream", a->ai_result, sizeof a->ai_result,
                             .height = -1, .read_only = true);
        } else if (st == 3 || st == 5) {
            cui_text(ui, st == 5 ? "已停止生成" : "AI 调用失败",
                     .size = 13, .font = CUI_FONT_MEDIUM,
                     .color = st == 5 ? CUI_COLOR_WARNING : CUI_COLOR_DANGER);
            cui_text(ui, a->ai_error[0] ? a->ai_error : "未知错误", .size = 12,
                     .nowrap = false);
            if (a->ai_result[0]) {
                cui_text(ui, "已收到的内容：", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED);
                cui_textarea(ui, "ai-partial", a->ai_result, sizeof a->ai_result,
                             .height = 160, .read_only = true);
            }
        } else if (st == 2 && a->ai_result[0]) {
            /* Partial-success notes (e.g. a 64 KB output cap) live in
             * ai_error; show them even though the request completed. */
            if (a->ai_error[0])
                cui_text(ui, a->ai_error, .size = 12,
                         .color = CUI_COLOR_WARNING, .nowrap = false);
            if (!a->ai_hist_pushed) {
                np_ai_push_history(a);
                a->ai_hist_pushed = true;
            }
            if (disp == NP_AI_DISP_DIFF) {
                const char *lang = (d && np_doc_is_latex(d)) ? "tex"
                                 : (d && d->lang > NP_LANG_TEXT && d->lang < NP_LANG_COUNT)
                                     ? NP_LANGS[d->lang].id : "text";
                cui_text(ui, "逐处 Diff：接受或拒绝当前修改，确认后才写入",
                         .size = 12, .font = CUI_FONT_MEDIUM);
                cui_diff_result dr = cui_diff_view(ui, "ai-diff", a->ai_staged,
                                                    a->ai_result, .height = -1,
                                                    .context = 3, .language = lang,
                                                    .current = &a->ai_diff_current);
                (void)dr;
                cui_row(ui, .width = CUI_FILL, .gap = 6) {
                    if (cui_button(ui, "接受当前处", .size = CUI_SIZE_SM,
                                   .disabled = dr.changes <= 0)) {
                        if (np_ai_hunk_decision(a, true))
                            np_toast(a, "已接受", "当前修改已移入待写入内容", CUI_TONE_INFO);
                    }
                    if (cui_button(ui, "拒绝当前处", .size = CUI_SIZE_SM,
                                   .disabled = dr.changes <= 0)) {
                        if (np_ai_hunk_decision(a, false))
                            np_toast(a, "已拒绝", "当前修改已从结果中移除", CUI_TONE_INFO);
                    }
                    if (cui_button(ui, "全部应用", .size = CUI_SIZE_SM,
                                   .variant = CUI_BUTTON_PRIMARY)) {
                        if (np_ai_apply(a)) {
                            np_toast(a, "已应用", "已按逐处选择写入文档", CUI_TONE_INFO);
                            a->ai_result[0] = 0; a->ai_done = 0; a->ai_last_state = 0;
                            a->ai_staged[0] = 0;
                        }
                    }
                }
            } else if (disp == NP_AI_DISP_INSERT) {
                cui_text(ui, "续写结果（插入到光标处，不重复已有文本）",
                         .size = 12, .font = CUI_FONT_MEDIUM);
                cui_textarea(ui, "ai-answer", a->ai_result, sizeof a->ai_result,
                             .height = -1, .read_only = true);
                cui_row(ui, .width = CUI_FILL, .gap = 8) {
                    if (cui_button(ui, "插入到光标处", .variant = CUI_BUTTON_PRIMARY)) {
                        if (np_ai_apply(a)) {          /* empty range = insert */
                            np_toast(a, "已插入", "AI 续写已写入光标处", CUI_TONE_INFO);
                            a->ai_result[0] = 0; a->ai_done = 0; a->ai_last_state = 0;
                        }
                    }
                }
            } else if (disp == NP_AI_DISP_TEMPLATE) {
                cui_text(ui, "生成的模板", .size = 12, .font = CUI_FONT_MEDIUM);
                cui_textarea(ui, "ai-answer", a->ai_result, sizeof a->ai_result,
                             .height = -1, .read_only = true);
                cui_row(ui, .width = CUI_FILL, .gap = 8) {
                    if (cui_button(ui, "新建文档", .variant = CUI_BUTTON_PRIMARY)) {
                        if (np_ai_new_doc_from_result(a)) {
                            np_toast(a, "已创建", "模板已在新标签中打开", CUI_TONE_INFO);
                            a->ai_result[0] = 0; a->ai_done = 0; a->ai_last_state = 0;
                        }
                    }
                    if (cui_button(ui, "插入到当前文档")) {
                        if (np_ai_apply(a)) {
                            np_toast(a, "已插入", "模板已写入光标处", CUI_TONE_INFO);
                            a->ai_result[0] = 0; a->ai_done = 0; a->ai_last_state = 0;
                        }
                    }
                }
            } else {
                cui_text(ui, "AI 回复（默认不改动正文）", .size = 12,
                         .font = CUI_FONT_MEDIUM);
                cui_textarea(ui, "ai-answer", a->ai_result, sizeof a->ai_result,
                             .height = -1, .read_only = true);
                cui_row(ui, .width = CUI_FILL, .gap = 8) {
                    if (cui_button(ui, "插入到光标处",
                                   .variant = CUI_BUTTON_PRIMARY)) {
                        if (np_ai_insert_here(a))
                            np_toast(a, "已插入", "AI 回复已写入光标处", CUI_TONE_INFO);
                    }
                    if (cui_button(ui, "复制结果", .variant = CUI_BUTTON_GHOST)) {
                        cui_set_clipboard_text(a->ui, a->ai_result);
                        np_toast(a, "已复制", "AI 回复已复制到剪贴板", CUI_TONE_INFO);
                    }
                }
            }
        } else {
            if (cui_empty_state(ui, "还没有结果",
                                .icon = CUI_ICON_SPARKLES, .compact = true,
                                .description = "选好任务与指令，向 AI 发起第一次请求。",
                                .action = "Ask AI",
                                .action_icon = CUI_ICON_SPARKLES)
                && !(a->ai_task == 6 && !a->ai_inst[0]))
                np_ai_start(a);
            cui_column(ui, .width = CUI_FILL, .gap = 6) {
                cui_text(ui, "Ask AI 会根据任务自动取上下文：", .size = 12,
                         .color = CUI_COLOR_TEXT_MUTED);
                cui_text(ui, "· 润色 / 翻译 / 自定义 → 选区或全文，Diff 确认后写入", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
                cui_text(ui, "· 补全续写 → 光标前后文，结果插入光标处", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
                cui_text(ui, "· 生成模板 → 按指令生成完整文档，可新建标签或插入", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
                cui_text(ui, "· 检查 / 解释 → 只回答，不改动正文", .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = false);
            }
        }

        /* Per-document history ---------------------------------------- */
        if (d && d->ai_hist_count > 0) {
            cui_separator(ui);
            char labels[NP_AI_HISTORY_MAX][64];
            const char *items[NP_AI_HISTORY_MAX];
            for (int i = 0; i < d->ai_hist_count; i++) {
                const np_ai_history *h = &d->ai_hist[i];
                const char *tn = "AI";
                if (h->task >= 7 && h->task - 7 < a->ai_action_count)
                    tn = a->ai_actions[h->task - 7].name;
                else if (h->task >= 0 && h->task < 7) tn = tasks[h->task];
                snprintf(labels[i], sizeof labels[i], "历史 %d · %s", i + 1, tn);
                items[i] = labels[i];
            }
            if (d->ai_hist_sel >= d->ai_hist_count) d->ai_hist_sel = d->ai_hist_count - 1;
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_select(ui, "ai-history", items, d->ai_hist_count,
                           &d->ai_hist_sel, .label = "历史", .width = CUI_FILL);
                if (cui_button(ui, "恢复", .size = CUI_SIZE_SM))
                    np_ai_restore_history(a, d, d->ai_hist_sel);
                if (cui_button(ui, "清空历史", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST))
                    d->ai_hist_count = 0;
            }
        }
    }
}

/* ---- Command palette (#10/#11) --------------------------------------- */

/* One table drives both Ctrl+Shift+P and F1: every stable menu command,
 * searchable, with groups shown while the query is empty.  Indices are
 * the dispatch contract for np_cmd_run below; the palette list is rebuilt
 * each frame so dynamic titles (density, whitespace mode) stay in sync. */
enum {
    NP_CMD_NEW, NP_CMD_OPEN, NP_CMD_SAVE, NP_CMD_SAVE_AS, NP_CMD_SAVE_ALL,
    NP_CMD_RELOAD, NP_CMD_CLOSE, NP_CMD_QUIT,
    NP_CMD_SELECT_ALL, NP_CMD_STAMP, NP_CMD_UPPER, NP_CMD_LOWER,
    NP_CMD_SORT, NP_CMD_UNIQ, NP_CMD_NOEMPTY, NP_CMD_STRIP,
    NP_CMD_TAB2SP, NP_CMD_SP2TAB, NP_CMD_JOIN, NP_CMD_DUP_LINE,
    NP_CMD_MOVE_UP, NP_CMD_MOVE_DOWN, NP_CMD_BRACKET, NP_CMD_CLEAR,
    NP_CMD_FIND, NP_CMD_REPLACE, NP_CMD_GOTO, NP_CMD_FIND_HERE,
    NP_CMD_FIND_FILES, NP_CMD_OUTPUT, NP_CMD_CLEAR_OUT,
    NP_CMD_SIDEBAR, NP_CMD_PREVIEW, NP_CMD_FOLLOW, NP_CMD_RESCAN,
    NP_CMD_PICK_DIR, NP_CMD_SETTINGS, NP_CMD_DARK, NP_CMD_SYS_THEME,
    NP_CMD_ACCENT, NP_CMD_DENSITY, NP_CMD_ZOOM_IN, NP_CMD_ZOOM_OUT,
    NP_CMD_ZOOM_RESET, NP_CMD_LINENUM, NP_CMD_GUIDES, NP_CMD_WS,
    NP_CMD_STATUS, NP_CMD_ZEN,
    NP_CMD_LANG, NP_CMD_LANG_AUTO, NP_CMD_CRLF, NP_CMD_ENC,
    NP_CMD_SYS_OPEN, NP_CMD_SHOW_DIR, NP_CMD_AI, NP_CMD_MACLAW,
    NP_CMD_BAK, NP_CMD_AUTOSAVE, NP_CMD_SESSION,
    NP_CMD_HELP, NP_CMD_UPDATE, NP_CMD_ABOUT,
    NP_CMD_COUNT
};

typedef struct { const char *title, *icon, *group, *shortcut; } np_cmd;

static const np_cmd np_cmds[NP_CMD_COUNT] = {
    /* 文件 */
    { "新建标签",                 CUI_ICON_SQUARE_PEN,      "文件", "Ctrl N" },
    { "打开…",                    CUI_ICON_FOLDER_OPEN,     "文件", "Ctrl O" },
    { "保存",                     CUI_ICON_SAVE,            "文件", "Ctrl S" },
    { "另存为…",                  NULL,                     "文件", "Ctrl Shift S" },
    { "全部保存",                 CUI_ICON_LAYERS,          "文件", NULL },
    { "从磁盘重新加载",           CUI_ICON_REFRESH_CW,      "文件", "Ctrl R" },
    { "关闭当前标签",             CUI_ICON_X,               "文件", "Ctrl W" },
    { "退出",                     CUI_ICON_LOG_OUT,         "文件", "Ctrl Q" },
    /* 编辑 */
    { "全选",                     NULL,                     "编辑", "Ctrl A" },
    { "插入日期/时间",            CUI_ICON_CLOCK,           "编辑", "F5" },
    { "转为大写",                 CUI_ICON_ARROW_UP,        "编辑", NULL },
    { "转为小写",                 CUI_ICON_ARROW_DOWN,      "编辑", NULL },
    { "排序选中的行 (A→Z)",       CUI_ICON_LIST,            "编辑", NULL },
    { "去除相邻重复行",           CUI_ICON_CHECK_CHECK,     "编辑", NULL },
    { "删除空行",                 CUI_ICON_MINUS,           "编辑", NULL },
    { "剪裁行尾空白",             CUI_ICON_SLIDERS_HORIZONTAL, "编辑", NULL },
    { "Tab 转空格",               CUI_ICON_ALIGN_LEFT,      "编辑", NULL },
    { "行首空格转 Tab",           CUI_ICON_GRIP_VERTICAL,   "编辑", NULL },
    { "合并选中的行",             CUI_ICON_CHEVRON_DOWN,    "编辑", NULL },
    { "复制当前行",               CUI_ICON_COPY,            "编辑", "Ctrl D" },
    { "上移选中的行",             CUI_ICON_ARROW_UP,        "编辑", "Alt Up" },
    { "下移选中的行",             CUI_ICON_ARROW_DOWN,      "编辑", "Alt Down" },
    { "转到匹配的括号",           CUI_ICON_CHEVRONS_UP_DOWN, "编辑", NULL },
    { "清空内容",                 CUI_ICON_TRASH_2,         "编辑", NULL },
    /* 搜索 */
    { "查找…",                    CUI_ICON_SEARCH,          "搜索", "Ctrl F" },
    { "查找和替换…",              CUI_ICON_REFRESH_CW,      "搜索", "Ctrl H" },
    { "转到行…",                  CUI_ICON_ARROW_RIGHT,     "搜索", "Ctrl G" },
    { "在当前文件中查找",         CUI_ICON_LIST,            "搜索", "Ctrl Shift L" },
    { "在文件中查找…",            CUI_ICON_FOLDER_OPEN,     "搜索", "Ctrl Shift F" },
    { "显示/隐藏输出窗口",        CUI_ICON_TERMINAL,        "搜索", "F7" },
    { "清空输出",                 CUI_ICON_TRASH_2,         "搜索", NULL },
    /* 视图 */
    { "目录 / 工作区侧栏",        CUI_ICON_PANEL_LEFT,      "视图", "Ctrl B" },
    { "预览面板",                 CUI_ICON_EYE,             "视图", "Ctrl Alt P" },
    { "目录跟随当前文件",         CUI_ICON_BOOKMARK,        "视图", NULL },
    { "刷新目录",                 CUI_ICON_REFRESH_CW,      "视图", NULL },
    { "选择工作目录…",            CUI_ICON_FOLDER_OPEN,     "视图", NULL },
    { "外观设置…",                CUI_ICON_PALETTE,         "视图", "Ctrl ," },
    { "切换深色模式",             CUI_ICON_MOON,            "视图", NULL },
    { "跟随系统主题",             NULL,                     "视图", NULL },
    { "切换强调色",               CUI_ICON_PALETTE,         "视图", NULL },
    { "切换界面密度",             CUI_ICON_SLIDERS_HORIZONTAL, "视图", NULL },
    { "放大字号",                 CUI_ICON_ZOOM_IN,         "视图", "Ctrl +" },
    { "缩小字号",                 CUI_ICON_ZOOM_OUT,        "视图", "Ctrl -" },
    { "重置字号",                 NULL,                     "视图", "Ctrl 0" },
    { "显示/隐藏行号",            NULL,                     "视图", NULL },
    { "显示/隐藏缩进参考线",      NULL,                     "视图", NULL },
    { "空白字符显示",             CUI_ICON_TYPE,            "视图", NULL },
    { "显示/隐藏状态栏",          NULL,                     "视图", NULL },
    { "沉浸式编辑",               CUI_ICON_MAXIMIZE_2,      "视图", "F11" },
    /* 文档 */
    { "选择语言…",                CUI_ICON_PALETTE,         "文档", NULL },
    { "按扩展名重新识别语言",     CUI_ICON_REFRESH_CW,      "文档", NULL },
    { "切换换行符 (LF/CRLF)",     NULL,                     "文档", NULL },
    { "切换保存编码",             NULL,                     "文档", NULL },
    /* 工具 */
    { "用系统默认程序打开",       CUI_ICON_EXTERNAL_LINK,   "工具", NULL },
    { "打开文件所在目录",         CUI_ICON_FOLDER,          "工具", NULL },
    { "AI 助手",                  CUI_ICON_SPARKLES,        "工具", "Ctrl Alt A" },
    { "maclaw 官方服务登录…",     CUI_ICON_SPARKLES,        "工具", NULL },
    { "保存时创建 .bak 备份",     NULL,                     "工具", NULL },
    { "自动保存（空闲 5 秒）",    NULL,                     "工具", NULL },
    { "启动时恢复上次会话",       NULL,                     "工具", NULL },
    /* 帮助 */
    { "快捷键速查",               CUI_ICON_KEYBOARD,        "帮助", NULL },
    { "在线更新…",                CUI_ICON_DOWNLOAD,        "帮助", NULL },
    { "关于",                     CUI_ICON_INFO,            "帮助", NULL },
};

static void np_cmd_run(notepad *a, int i) {
    np_doc *d = np_cur(a);
    if (!d) return;
    switch (i) {
    case NP_CMD_NEW: if (np_doc_new(a)) np_activate(a, a->count - 1, true); break;
    case NP_CMD_OPEN: np_open_dialog(a); break;
    case NP_CMD_SAVE: np_save_current(a, d); break;
    case NP_CMD_SAVE_AS: np_save_as(a, d); break;
    case NP_CMD_SAVE_ALL: np_save_all(a); break;
    case NP_CMD_RELOAD: np_request_reload(a); break;
    case NP_CMD_CLOSE: np_request_close(a, a->active); break;
    case NP_CMD_QUIT: np_request_quit(a); break;
    case NP_CMD_SELECT_ALL:
        if (!d->is_hex) cui_code_editor_select(a->ui, d->editor_id, 0, d->len);
        break;
    case NP_CMD_STAMP: {
        if (d->is_hex) break;
        char stamp[64];
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        if (lt && strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", lt))
            np_insert_at_caret(a, stamp);
        break;
    }
    case NP_CMD_UPPER: if (!d->is_hex) np_case_transform(a, d, true); break;
    case NP_CMD_LOWER: if (!d->is_hex) np_case_transform(a, d, false); break;
    case NP_CMD_SORT: if (!d->is_hex) np_line_transform(a, d, TR_SORT); break;
    case NP_CMD_UNIQ: if (!d->is_hex) np_line_transform(a, d, TR_UNIQ); break;
    case NP_CMD_NOEMPTY: if (!d->is_hex) np_line_transform(a, d, TR_NOEMPTY); break;
    case NP_CMD_STRIP: if (!d->is_hex) np_line_transform(a, d, TR_STRIP); break;
    case NP_CMD_TAB2SP: if (!d->is_hex) np_line_transform(a, d, TR_TAB2SP); break;
    case NP_CMD_SP2TAB: if (!d->is_hex) np_line_transform(a, d, TR_SP2TAB); break;
    case NP_CMD_JOIN: if (!d->is_hex) np_line_transform(a, d, TR_JOIN); break;
    case NP_CMD_DUP_LINE: if (!d->is_hex) np_duplicate_line(a, d); break;
    case NP_CMD_MOVE_UP: if (!d->is_hex) np_move_lines(a, d, -1); break;
    case NP_CMD_MOVE_DOWN: if (!d->is_hex) np_move_lines(a, d, 1); break;
    case NP_CMD_BRACKET: if (!d->is_hex) np_goto_bracket(a, d); break;
    case NP_CMD_CLEAR:
        if (!d->is_hex && d->text[0]) {
            d->text[0] = 0;
            np_text_changed(a, d);
            np_bump_editor(a, d);
        }
        break;
    case NP_CMD_FIND: if (!d->is_hex) np_open_find(a); break;
    case NP_CMD_REPLACE: if (!d->is_hex) a->dlg_replace = true; break;
    case NP_CMD_GOTO: if (!d->is_hex) a->dlg_goto = true; break;
    case NP_CMD_FIND_HERE:
        if (!d->is_hex) {
            a->show_output = true;
            a->out_tab = 0;
            a->hits_rev = 0;
            if (!a->find_query[0]) np_open_find(a);
        }
        break;
    case NP_CMD_FIND_FILES:
        a->dlg_find_files = true;
        if (!a->ff_dir[0]) np_dir_set(a, a->side_dir);
        break;
    case NP_CMD_OUTPUT: a->show_output = !a->show_output; break;
    case NP_CMD_CLEAR_OUT:
        a->msg_count = 0;
        a->hit_count = 0;
        a->hits_rev = 0;
        a->ff_hit_count = 0;
        a->ff_count = 0;
        break;
    case NP_CMD_SIDEBAR: a->show_sidebar = !a->show_sidebar; break;
    case NP_CMD_PREVIEW: a->show_preview = !a->show_preview; break;
    case NP_CMD_FOLLOW: a->follow_active = !a->follow_active; break;
    case NP_CMD_RESCAN: a->side_reload = true; break;
    case NP_CMD_PICK_DIR: {
        char dir[NP_PATH_CAP];
        if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                .kind = CUI_FILE_FOLDER, .title = "选择工作目录" },
                dir, sizeof dir))
            np_dir_set(a, dir);
        break;
    }
    case NP_CMD_SETTINGS: a->dlg_settings = true; break;
    case NP_CMD_DARK:
        a->dark = !a->dark;
        a->dark_valid = true;
        a->style = -1;
        cui_app_follow_system_theme(NULL, false);
        cui_set_theme(a->ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
        break;
    case NP_CMD_SYS_THEME:
        a->dark_valid = false;
        a->style = -1;
        cui_app_follow_system_theme(NULL, true);
        a->dark = cui_get_theme_mode(a->ui) == CUI_THEME_DARK;
        break;
    case NP_CMD_ACCENT:
        a->accent = (a->accent + 1) % CUI_ACCENT_COUNT;
        a->style = -1;
        cui_set_accent(a->ui, (cui_accent)a->accent);
        break;
    case NP_CMD_DENSITY: a->density = (a->density + 1) % 3; break;
    case NP_CMD_ZOOM_IN: a->font_size = NP_MIN(NP_FONT_MAX, a->font_size + 1); break;
    case NP_CMD_ZOOM_OUT: a->font_size = NP_MAX(NP_FONT_MIN, a->font_size - 1); break;
    case NP_CMD_ZOOM_RESET: a->font_size = NP_FONT_DEF; break;
    case NP_CMD_LINENUM: a->line_numbers = !a->line_numbers; break;
    case NP_CMD_GUIDES: if (!d->is_hex) a->indent_guides = !a->indent_guides; break;
    case NP_CMD_WS: if (!d->is_hex) a->whitespace = (a->whitespace + 1) % 3; break;
    case NP_CMD_STATUS: a->show_status = !a->show_status; break;
    case NP_CMD_ZEN: np_zen_set(a, !a->zen); break;
    case NP_CMD_LANG: if (!d->is_hex) np_open_lang_dialog(a); break;
    case NP_CMD_LANG_AUTO:
        if (!d->is_hex) {
            d->lang = np_detect_lang(d->path, d->text);
            d->lang_manual = false;
        }
        break;
    case NP_CMD_CRLF: if (!d->is_hex) d->crlf = !d->crlf; break;
    case NP_CMD_ENC: if (!d->is_hex) d->enc = (d->enc + 1) % 3; break;
    case NP_CMD_SYS_OPEN: if (d->path[0]) np_system_open(a, d->path, false); break;
    case NP_CMD_SHOW_DIR: if (d->path[0]) np_system_open(a, d->path, true); break;
    case NP_CMD_AI: a->show_ai = !a->show_ai; break;
    case NP_CMD_MACLAW: a->dlg_maclaw = true; break;
    case NP_CMD_BAK: a->make_backup = !a->make_backup; break;
    case NP_CMD_AUTOSAVE: a->auto_save = !a->auto_save; break;
    case NP_CMD_SESSION: a->session_restore = !a->session_restore; break;
    case NP_CMD_HELP: a->dlg_help = true; break;
    case NP_CMD_UPDATE:
        a->dlg_update = true;
        np_updater_start_check();
        break;
    case NP_CMD_ABOUT: a->dlg_about = true; break;
    }
}

/* ---- B3 补丁审查面板接线（M2/M3）：主程序只做绑定与写回，逻辑在面板内 ----
 * M3 起双轨：文本轨 hunk 来自 Composer 注入，写回走编辑器（Ctrl+Z 可撤销）；
 * 字节轨保持 M2 行为（捕获选区 + 单一撤销组）。 */
static void np_review_write_back(notepad *a, np_doc *d,
                                 const unsigned char *data, size_t n) {
    if (!d || !data) return;
    if (!np_ensure_cap(d, n + 2)) return;
    if (d->is_hex) {
        memcpy(d->text, data, n);
        d->text[n] = 0;
        d->len = (int)n;
    } else {
        /* 文本轨：整体替换经编辑器执行，保持 CUI delta history 有效
         * （直接 memcpy 会让下一次 Ctrl+Z 编辑到错误位置，见 np_ai_apply）。 */
        int applied_len = d->len;
        if (!cui_code_editor_replace_external(a->ui, d->editor_id,
                                              d->text, d->cap, d->len,
                                              0, d->len, (const char *)data,
                                              (int)n, &applied_len) ||
            applied_len != (int)n)
            return;
        d->len = applied_len;
    }
    d->file_size = n;
    d->dirty = true;
    d->rev++;
    d->content_rev++;
    np_bump_editor(a, d);
    np_update_title(a);
}

static void np_review_panel_dock(cui_ctx *ui, notepad *a, np_doc *d) {
    np_review_ui_t *rui = np_review_ui_get();
    if (d && d->len > 0) {
        np_review_ui_bind_ex(rui, d->path, (const unsigned char *)d->text,
                             (size_t)d->len, a->review_allow_write,
                             d->is_hex ? NP_TRACK_BINARY : NP_TRACK_TEXT);
        if (d->is_hex)
            np_review_ui_set_selection(rui, d->hexsel.start, d->hexsel.length);
    } else {
        np_review_ui_bind_ex(rui, "", NULL, 0, a->review_allow_write,
                             NP_TRACK_BINARY);
    }
    np_rvui_action_t act = np_review_ui_draw(rui, ui);
    if (act == NP_RVUI_CLOSED) {
        a->show_review = false;
        return;
    }
    if ((act == NP_RVUI_APPLY || act == NP_RVUI_UNDO) && d && d->len > 0) {
        size_t n = 0;
        const unsigned char *nd = np_review_ui_data(rui, &n);
        if (nd) {
            np_logf(a, "%s  %s  ·  %zu 字节",
                    act == NP_RVUI_APPLY ? "补丁已应用" : "补丁已撤销",
                    np_base(d->path), n);
            np_review_write_back(a, d, nd, n);
        }
    }
}

/* ---- B1 AI 工作台接线（M3）：意图表单 → Job 卡 → 送入 B3 ---- */
static void np_composer_panel_dock(cui_ctx *ui, notepad *a, np_doc *d) {
    np_composer_ui_t *cpu = np_composer_ui_get();
    np_track_t track = (d && d->is_hex) ? NP_TRACK_BINARY : NP_TRACK_TEXT;
    if (d && d->len > 0) {
        size_t off0, off1;
        if (d->is_hex) {
            off0 = (size_t)d->hexsel.start;
            off1 = off0 + (size_t)d->hexsel.length;
        } else {
            off0 = (size_t)NP_MIN(d->caret, d->anchor);
            off1 = (size_t)NP_MAX(d->caret, d->anchor);
        }
        np_composer_ui_bind(cpu, d->path, track, d->rev,
                            (const unsigned char *)d->text, (size_t)d->len);
        np_composer_ui_set_selection(cpu, off0, off1);
    } else {
        np_composer_ui_bind(cpu, "", track, 0, NULL, 0);
    }
    np_cpui_action_t act = np_composer_ui_draw(cpu, ui);
    if (act == NP_CPUI_CLOSED) {
        a->show_workbench = false;
        return;
    }
    if (act == NP_CPUI_SEND_REVIEW && d) {
        const np_patch_set_t *ps = np_composer_ui_patch(cpu);
        if (ps) {
            /* 面板立即深拷贝（与 Composer 生命周期解耦），同源文档才导入 */
            np_review_ui_inject_text(np_review_ui_get(), ps, d->path);
            a->show_workbench = false;
            a->show_review = true;      /* 自动弹出 B3 审查 */
            np_logf(a, "补丁已送入审查 · %s", np_base(d->path));
        }
    }
}

/* ---- B2 计划卡面板接线（M4）：主程序只做绑定，逻辑在面板内 ---- */
static void np_plan_panel_dock(cui_ctx *ui, notepad *a, np_doc *d) {
    (void)a;
    np_plan_ui_t *pui = np_plan_ui_get();
    if (d && d->len > 0)
        np_plan_ui_bind(pui, d->path, (const unsigned char *)d->text,
                        (size_t)d->len);
    else
        np_plan_ui_bind(pui, "", NULL, 0);
    if (np_plan_ui_draw(pui, ui) == NP_PLANUI_CLOSED)
        a->show_plan = false;
}

/* ---- F2 OOXML 结构树面板接线：主程序只做绑定，逻辑在面板内 ---- */
static void np_ooxml_panel_dock(cui_ctx *ui, notepad *a, np_doc *d) {
    np_ox_ui_t *ou = np_ox_ui_get();
    if (d && d->len > 0)
        np_ox_ui_bind(ou, d->path, (const unsigned char *)d->text, (size_t)d->len);
    else
        np_ox_ui_bind(ou, "", NULL, 0);
    if (np_ox_ui_draw(ou, ui) == NP_OXUI_CLOSED)
        a->show_ooxml = false;
}

static void np_frame(cui_ctx *ui, void *user) {
    notepad *a = (notepad *)user;
    a->ui = ui;
    np_apply_theme(ui, a);

    if (!a->boot_done) {
        a->boot_done = true;
        np_load_fonts(ui, a);
        if (a->style >= 0 && a->style < NP_STYLE_COUNT) {
            /* A saved style is the single source of truth for the look. */
            np_apply_style(ui, a, a->style);
        } else {
            /* Legacy config: honour the old dark/accent keys with the default tint. */
            if (a->dark_valid) {        /* persisted choice wins over system */
                cui_app_follow_system_theme(NULL, false);
                cui_set_theme(ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
            } else {
                a->dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
            }
            cui_set_accent(ui, (cui_accent)a->accent);
            cui_set_neutral_tint(ui, 255.f, 0.011f);
        }
#ifdef _WIN32
        np_install_close_hook();
#endif
        np_webview_boot(a);      /* async: bring up native WebView for HTML preview */
        a->quiet = true;
        for (int i = 0; i < a->startup_count; i++) {
            if (!strncmp(a->startup[i], "draft:", 6)) {
                np_restore_draft(a, atoi(a->startup[i] + 6));
            } else if (np_open_path(a, a->startup[i], false) &&
                       a->startup_cur[i] >= 0) {
                /* Session restore: put the caret back where it was. */
                np_doc *sd = np_cur(a);
                if (sd && !sd->is_hex) {
                    int c = NP_MAX(0, NP_MIN(a->startup_cur[i], sd->len));
                    sd->caret = sd->anchor = c;
                    cui_code_editor_select(a->ui, sd->editor_id, c, c);
                }
            }
        }
        if (a->startup_bt[0]) {
            np_doc *sd = np_cur(a);
            if (sd && sd->is_hex) {
                const np_bt_builtin *bt = NULL;
                for (int i = 0; np_bt_builtins[i].name; i++)
                    if (!strcmp(np_bt_builtins[i].name, a->startup_bt)) {
                        bt = &np_bt_builtins[i]; break;
                    }
                if (bt) np_bt_apply(a, sd, bt->label, bt->source);
            }
            a->startup_bt[0] = 0;       /* apply once */
        }
        a->quiet = false;
        {   /* reveal the file opened from the command line (the sync below
             * would otherwise keep the follow block from ever firing) */
            np_doc *rd = np_cur(a);
            if (rd && rd->path[0]) {
                char rdir[NP_PATH_CAP];
                np_dir_of(rd->path, rdir, sizeof rdir);
                if (a->follow_active && strcmp(rdir, a->side_dir) != 0)
                    np_dir_set(a, rdir);
                np_tree_queue_reveal(rd->path);
            }
        }
        a->last_active = a->active;
        np_logf(a, "就绪  ·  %d 个文档  ·  目录 %s", a->count, a->side_dir);
    }

    /* Periodically mirror unsaved buffers into the config directory. This is
     * a best-effort crash net; exit still writes the authoritative snapshot. */
    {
        static time_t last_draft_save = 0;
        time_t now = time(NULL);
        if (now - last_draft_save >= 60) {
            np_save_drafts(a);
            last_draft_save = now;
        }
    }

    /* Detect files changed outside the editor (git, other tools, builds). */
    np_disk_check(a);

    /* Opt-in idle auto-save (after the disk check: it updates mtime). */
    np_auto_save_tick(a);

    /* The find bar's Aa / W toggles are the live source of truth: mirror
     * them back into the shared settings (and persist on change) so the
     * replace dialog and Ctrl F agree the next time either opens. */
    {
        np_doc *cur = np_cur(a);
        if (cur) {
            bool mc, ww;
            cui_code_editor_find_toggles(a->ui, cur->editor_id, &mc, &ww);
            if (mc != a->match_case || ww != a->find_word) {
                a->match_case = mc;
                a->find_word = ww;
                np_config_save(a);
            }
        }
    }

    /* EditPlus-style "directory follows the active file", now expressed by
     * the tree: retarget the root (follow mode) and reveal + select the
     * active file inside it. */
    if (a->active != a->last_active) {
        np_doc *cur = np_cur(a);
        if (cur && cur->path[0]) {
            char dir[NP_PATH_CAP];
            np_dir_of(cur->path, dir, sizeof dir);
            if (a->follow_active && strcmp(dir, a->side_dir) != 0)
                np_dir_set(a, dir);
            np_tree_queue_reveal(cur->path);
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
            if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_G)) {
                np_doc *cd = np_cur(a);
                if (cd && cd->is_hex) a->dlg_hexgoto = true;
                else a->dlg_goto = true;
            }
        }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_EQUAL) ||
            cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_EQUAL))
            a->font_size = NP_MIN(NP_FONT_MAX, a->font_size + 1);
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_MINUS))
            a->font_size = NP_MAX(NP_FONT_MIN, a->font_size - 1);
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_0)) a->font_size = NP_FONT_DEF;
        for (int k = 0; k < 9 && k < a->count; k++)
            if (cui_shortcut(ui, CUI_MOD_PRIMARY, digit_keys[k])) np_activate(a, k, true);

        /* Alt+\ triggers an inline AI completion at the caret (manual,
         * independent of the auto-trigger switch). */
        if (d && !d->is_hex && d->editor_focused &&
            cui_shortcut(ui, CUI_MOD_ALT, CUI_KEY_BACKSLASH))
            np_inline_manual(a, d);

        /* Line tools: duplicate (Ctrl D) and move up / down (Alt Up/Down).
         * Editor focus required so they never fire while typing elsewhere. */
        if (d && !d->is_hex && d->editor_focused) {
            if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_D))
                np_duplicate_line(a, d);
            if (cui_shortcut(ui, CUI_MOD_ALT, CUI_KEY_UP))
                np_move_lines(a, d, -1);
            if (cui_shortcut(ui, CUI_MOD_ALT, CUI_KEY_DOWN))
                np_move_lines(a, d, 1);
        }

        /* Markdown paragraph shortcuts (only for .md documents). Kept off the
         * plain Ctrl+digit range used for tab switching. */
        if (d && np_doc_is_markdown(d)) {
            static const int md_level_keys[6] = {
                CUI_KEY_1, CUI_KEY_2, CUI_KEY_3, CUI_KEY_4, CUI_KEY_5, CUI_KEY_6
            };
            /* Ctrl+Alt+1..6 → headings; Ctrl+Alt+0 → paragraph */
            for (int k = 0; k < 6; k++)
                if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, md_level_keys[k]))
                    np_md_set_heading(a, k + 1);
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_0))
                np_md_set_heading(a, 0);
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_EQUAL))
                np_md_adjust_heading(a, -1);
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_MINUS))
                np_md_adjust_heading(a, 1);
            /* block inserts */
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_M))
                np_apply_md_snip(a, np_md_snip_index("公式块"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_K))
                np_apply_md_snip(a, np_md_snip_index("代码块"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_Q))
                np_apply_md_snip(a, np_md_snip_index("引用"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_T))
                np_apply_md_snip(a, np_md_snip_index("表格"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_X))
                np_apply_md_snip(a, np_md_snip_index("任务列表"));
            /* inline formatting */
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_B))
                np_apply_md_snip(a, np_md_snip_index("加粗"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_I))
                np_apply_md_snip(a, np_md_snip_index("斜体"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_8))
                np_apply_md_snip(a, np_md_snip_index("无序列表"));
            if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_9))
                np_apply_md_snip(a, np_md_snip_index("有序列表"));
        }

        /* LaTeX table wizard (Ctrl+Alt+T), only while editing a TeX source. */
        if (np_doc_is_latex(np_cur(a)) &&
            cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_T)) {
            a->dlg_latex_table = true;
            if (!a->ltable_align[0]) snprintf(a->ltable_align, sizeof a->ltable_align, "lcr");
            a->ltable_width = 0.9f;
        }
        /* Live formula preview (Ctrl+Alt+M). */
        if (np_doc_is_latex(np_cur(a)) &&
            cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_M)) {
            a->dlg_math_preview = true;
            np_math_expr_from_caret(a);
        }
        /* Native AI panel (Ctrl+Alt+A): docked, persistent, not a dialog. */
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_A)) {
            a->show_ai = !a->show_ai;
        }
        /* Quick AI completion from the caret (Ctrl+Alt+C). */
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_C)) {
            np_doc *cd = np_cur(a);
            if (cd && !cd->is_hex) np_ai_quick(a, 5);
        }

        /* Reload from disk (manual counterpart of np_disk_check). */
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_R)) np_request_reload(a);

        /* Workspace toggles, in the spirit of EditPlus's View menu. */
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_B)) a->show_sidebar = !a->show_sidebar;
        if (cui_key_pressed(ui, CUI_KEY_F7)) a->show_output = !a->show_output;
        if (cui_key_pressed(ui, CUI_KEY_F11)) np_zen_set(a, !a->zen);
        /* F3 repeats the hex byte search (the text find bar uses Enter). */
        {
            np_doc *cd = np_cur(a);
            if (cd && cd->is_hex && a->hexfind_buf[0] &&
                cui_key_pressed(ui, CUI_KEY_F3))
                np_hex_find_run(a, true);
        }
        /* Ctrl+Shift+P is the command palette (#10); the preview panel
         * moves to Ctrl+Alt+P and stays reachable from the palette too. */
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_P))
            a->palette_open = true;
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_ALT, CUI_KEY_P))
            a->show_preview = !a->show_preview;
        /* F1 opens the same palette (#11): the shortcut column plus fuzzy
         * search replaces the static help listing. */
        if (cui_key_pressed(ui, CUI_KEY_F1)) a->palette_open = true;
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_COMMA)) a->dlg_settings = true;
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_F)) {
            a->dlg_find_files = true;
            if (!a->ff_dir[0]) np_dir_set(a, a->side_dir);
        }
        if (cui_shortcut(ui, CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_L)) {
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

    /* Top chrome ------------------------------------------------------ */
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL,
               .bg = CUI_COLOR_BACKGROUND, .gap = 0) {
        /* Brand identity lives in the OS title bar / window icon now; the
         * density and help controls remain in the View menu and toolbar. */
        /* Chrome collapse easing (#12): F11 folds the menu/toolbar strip
         * away with a 150 ms height animation instead of a hard cut.  The
         * natural height is re-measured whenever the strip renders freely. */
        {
            float vis = cui_animate_spring(ui, cui_make_id(ui, "#np-chrome-vis"),
                                           a->zen ? 0.0f : 1.0f, 0.15f, 0.0f);
            bool measure = a->zen_chrome_h <= 0 || (!a->zen && vis > 0.995f);
            if (vis > 0.01f) {
        cui_column(ui, .id = "#np-chrome", .width = CUI_FILL,
                   .max_height = measure ? 1e6f : a->zen_chrome_h * vis,
                   .clip = !measure,
                   .bg = CUI_COLOR_SURFACE, .gap = 0) {
        cui_menu_bar(ui, "menubar", .no_border = true) {
            cui_menu_bar_menu(ui, "文件") {
                if (cui_menu_item(ui, "新建标签", .icon = CUI_ICON_SQUARE_PEN, .shortcut = "Ctrl N")) {
                    if (np_doc_new(a)) np_activate(a, a->count - 1, true);
                }
                if (cui_menu_item(ui, "从 LaTeX 模板新建…", .icon = CUI_ICON_FILE_TEXT)) {
                    a->latex_tpl_pick = 0;
                    a->dlg_latex_tpl = true;
                }
                if (cui_menu_item(ui, "从 ZIP 导入 LaTeX 项目…", .icon = CUI_ICON_ARCHIVE))
                    np_import_latex_zip(a);
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
                if (cui_menu_item(ui, d->is_hex ? "以文本模式重新打开" : "以十六进制模式重新打开",
                                  .icon = CUI_ICON_CPU, .disabled = !d->path[0]))
                    np_hex_reopen(a, !d->is_hex);
                if (cui_menu_item(ui, "从磁盘重新加载", .icon = CUI_ICON_REFRESH_CW,
                                  .shortcut = "Ctrl R", .disabled = !d->path[0]))
                    np_request_reload(a);
                if (cui_menu_item(ui, "关闭当前标签", .icon = CUI_ICON_X, .shortcut = "Ctrl W"))
                    np_request_close(a, a->active);
                if (a->recent_count) {
                    cui_menu_separator(ui);
                    cui_menu_label(ui, "最近打开");
                    for (int i = 0; i < a->recent_count; i++) {
                        char label[NP_PATH_CAP + 32];
                        int fk = np_file_family(a->recent[i]);
                        snprintf(label, sizeof label, "%s##recent%d", np_base(a->recent[i]), i);
                        if (cui_menu_item(ui, label, .icon = np_family_icon(fk),
                                          .icon_color = np_family_tint(ui, fk)))
                            np_open_path(a, a->recent[i], true);
                    }
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "退出", .icon = CUI_ICON_LOG_OUT, .shortcut = "Ctrl Q"))
                    np_request_quit(a);
            }

            cui_menu_bar_menu(ui, "编辑") {
                if (d->is_hex) {
                    char sel[96];
                    size_t sl = d->hexsel.length;
                    if (d->hexsel.start >= (size_t)d->len) sl = 0;
                    snprintf(sel, sizeof sel, "已选 %zu 字节（0x%zX 起）", sl, d->hexsel.start);
                    cui_menu_label(ui, sel);
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, "复制选区（十六进制）", .icon = CUI_ICON_COPY,
                                      .disabled = !sl))
                        np_hex_copy(a, d, false);
                    if (cui_menu_item(ui, "复制选区（ASCII）", .icon = CUI_ICON_COPY,
                                      .disabled = !sl))
                        np_hex_copy(a, d, true);
                    if (cui_menu_item(ui, "转到偏移…", .icon = CUI_ICON_ARROW_RIGHT,
                                      .shortcut = "Ctrl G"))
                        a->dlg_hexgoto = true;
                    if (cui_menu_item(ui, "全选字节", .disabled = !d->len)) {
                        d->hexsel.start = 0;
                        d->hexsel.length = (size_t)d->len;
                        d->hexsel.cursor = 0;
                    }
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, d->bt_name[0] ? "更换二进制模板…" : "应用二进制模板…",
                                      .icon = CUI_ICON_CODE))
                        a->dlg_bt = true;
                    if (d->bt_name[0]) {
                        if (cui_menu_item(ui, "显示/隐藏字段树", .checked = d->bt_show_tree))
                            d->bt_show_tree = !d->bt_show_tree;
                        if (cui_menu_item(ui, "重新解析模板"))
                            np_bt_reparse(a, d);
                        if (cui_menu_item(ui, "清除模板", .icon = CUI_ICON_X))
                            np_bt_clear(d);
                    }
                    cui_menu_label(ui, "十六进制编辑：键入数字直接覆盖字节，不改变文件大小");
                } else {
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
                cui_menu_label(ui, "转换");
                if (cui_menu_item(ui, "转为大写", .icon = CUI_ICON_ARROW_UP))
                    np_case_transform(a, d, true);
                if (cui_menu_item(ui, "转为小写", .icon = CUI_ICON_ARROW_DOWN))
                    np_case_transform(a, d, false);
                if (cui_menu_item(ui, "排序选中的行 (A→Z)", .icon = CUI_ICON_LIST))
                    np_line_transform(a, d, TR_SORT);
                if (cui_menu_item(ui, "去除相邻重复行", .icon = CUI_ICON_CHECK_CHECK))
                    np_line_transform(a, d, TR_UNIQ);
                if (cui_menu_item(ui, "删除空行", .icon = CUI_ICON_MINUS))
                    np_line_transform(a, d, TR_NOEMPTY);
                if (cui_menu_item(ui, "剪裁行尾空白", .icon = CUI_ICON_SLIDERS_HORIZONTAL))
                    np_line_transform(a, d, TR_STRIP);
                if (cui_menu_item(ui, "Tab 转空格", .icon = CUI_ICON_ALIGN_LEFT))
                    np_line_transform(a, d, TR_TAB2SP);
                if (cui_menu_item(ui, "行首空格转 Tab", .icon = CUI_ICON_GRIP_VERTICAL))
                    np_line_transform(a, d, TR_SP2TAB);
                if (cui_menu_item(ui, "合并选中的行", .icon = CUI_ICON_CHEVRON_DOWN))
                    np_line_transform(a, d, TR_JOIN);
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "复制当前行", .icon = CUI_ICON_COPY,
                                  .shortcut = "Ctrl D"))
                    np_duplicate_line(a, d);
                if (cui_menu_item(ui, "上移选中的行", .icon = CUI_ICON_ARROW_UP,
                                  .shortcut = "Alt Up"))
                    np_move_lines(a, d, -1);
                if (cui_menu_item(ui, "下移选中的行", .icon = CUI_ICON_ARROW_DOWN,
                                  .shortcut = "Alt Down"))
                    np_move_lines(a, d, 1);
                if (cui_menu_item(ui, "转到匹配的括号", .icon = CUI_ICON_CHEVRONS_UP_DOWN))
                    np_goto_bracket(a, d);
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
            }

            cui_menu_bar_menu(ui, "搜索") {
                if (d->is_hex) {
                    if (cui_menu_item(ui, "转到偏移…", .icon = CUI_ICON_ARROW_RIGHT,
                                      .shortcut = "Ctrl G"))
                        a->dlg_hexgoto = true;
                } else {
                if (cui_menu_item(ui, "查找…", .icon = CUI_ICON_SEARCH, .shortcut = "Ctrl F"))
                    np_open_find(a);
                if (cui_menu_item(ui, "查找和替换…", .icon = CUI_ICON_REFRESH_CW, .shortcut = "Ctrl H"))
                    a->dlg_replace = true;
                if (cui_menu_item(ui, "转到行…", .icon = CUI_ICON_ARROW_RIGHT, .shortcut = "Ctrl G"))
                    a->dlg_goto = true;
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "在当前文件中查找", .icon = CUI_ICON_LIST,
                                  .shortcut = "Ctrl Shift L")) {
                    a->show_output = true;
                    a->out_tab = 0;
                    a->hits_rev = 0;
                    if (!a->find_query[0]) np_open_find(a);
                }
                }
                if (cui_menu_item(ui, "在文件中查找…", .icon = CUI_ICON_FOLDER_OPEN,
                                  .shortcut = "Ctrl Shift F")) {
                    a->dlg_find_files = true;
                    if (!a->ff_dir[0]) np_dir_set(a, a->side_dir);
                }
                if (cui_menu_item(ui, "显示输出窗口", .icon = CUI_ICON_TERMINAL,
                                  .shortcut = "F7", .checked = a->show_output))
                    a->show_output = !a->show_output;
                if (cui_menu_item(ui, "清空输出", .icon = CUI_ICON_TRASH_2)) {
                    a->msg_count = 0;
                    a->hit_count = 0;
                    a->hits_rev = 0;
                    a->ff_hit_count = 0;
                    a->ff_count = 0;
                }
            }

            cui_menu_bar_menu(ui, "工具") {
                if (cui_menu_item(ui, "用系统默认程序打开", .icon = CUI_ICON_EXTERNAL_LINK,
                                  .disabled = !d->path[0]))
                    np_system_open(a, d->path, false);
                if (cui_menu_item(ui, "打开文件所在目录", .icon = CUI_ICON_FOLDER,
                                  .disabled = !d->path[0]))
                    np_system_open(a, d->path, true);
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "指定 LaTeX 主文件…", .icon = CUI_ICON_FILE_TEXT,
                                  .disabled = !d->path[0])) {
                    char pick[NP_PATH_CAP] = "";
                    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                            .kind = CUI_FILE_OPEN, .title = "选择 LaTeX 主文件（main .tex）",
                            .filter_name = "LaTeX 文档", .filter = "tex;latex;ltx" },
                            pick, sizeof pick)) {
                        snprintf(d->tex_root, sizeof d->tex_root, "%s", pick);
                        d->tex_root_manual = true;
                        d->tex_root_probed = true;
                        snprintf(a->preview_owner, sizeof a->preview_owner, "%s", pick);
                        np_logf(a, "LaTeX 主文件  %s", pick);
                        np_toast(a, "已指定主文件", pick, CUI_TONE_SUCCESS);
                    }
                }
                if (cui_menu_item(ui, "清除 LaTeX 主文件绑定",
                                  .disabled = !d->tex_root[0])) {
                    d->tex_root[0] = 0;
                    d->tex_root_manual = false;
                    d->tex_root_probed = false;
                    a->preview_owner[0] = 0;   /* force re-detect next frame */
                }
                if (cui_menu_item(ui, "在线 LaTeX 模板库…", .icon = CUI_ICON_DOWNLOAD))
                    a->dlg_tpl_online = true;
                {
                    char eng[NP_PATH_CAP];
                    bool ready = np_tinytex_engine(eng, sizeof eng);
                    if (cui_menu_item(ui, ready ? "LaTeX 引擎：已就绪" : "安装 TinyTeX…",
                                      .icon = CUI_ICON_DOWNLOAD))
                        a->dlg_tinytex = true;
                }
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "AI 助手", .icon = CUI_ICON_SPARKLES,
                                  .checked = a->show_ai))
                    a->show_ai = !a->show_ai;
                if (cui_menu_item(ui, "AI 工作台（任务表单）", .icon = CUI_ICON_SPARKLES,
                                  .checked = a->show_workbench))
                    a->show_workbench = !a->show_workbench;
                if (cui_menu_item(ui, "maclaw 官方服务登录…", .icon = CUI_ICON_SPARKLES))
                    a->dlg_maclaw = true;
                if (cui_menu_item(ui, "知识搜索源（SearchProfile）…", .icon = CUI_ICON_SEARCH))
                    a->dlg_searchprof = true;
                if (cui_menu_item(ui, "MCP 工具服务器…", .icon = CUI_ICON_SERVER))
                    a->dlg_mcp = true;
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "保存时创建 .bak 备份", .checked = a->make_backup))
                    a->make_backup = !a->make_backup;
                if (cui_menu_item(ui, "自动保存（空闲 5 秒，文本模式）",
                                  .checked = a->auto_save))
                    a->auto_save = !a->auto_save;
                if (cui_menu_item(ui, "启动时恢复上次会话", .checked = a->session_restore))
                    a->session_restore = !a->session_restore;
            }

            cui_menu_bar_menu(ui, "视图") {
                if (cui_menu_item(ui, "目录 / 工作区侧栏", .icon = CUI_ICON_PANEL_LEFT,
                                  .shortcut = "Ctrl B", .checked = a->show_sidebar))
                    a->show_sidebar = !a->show_sidebar;
                {
                    char pv[96];
                    snprintf(pv, sizeof pv, "预览面板（%s）", np_preview_kind_name(np_preview_for_doc(a, d)));
                    if (cui_menu_item(ui, pv, .icon = CUI_ICON_EYE,
                                      .shortcut = "Ctrl Alt P", .checked = a->show_preview))
                        a->show_preview = !a->show_preview;
                }
                if (cui_menu_item(ui, "补丁审查面板", .icon = CUI_ICON_EYE,
                                  .checked = a->show_review))
                    a->show_review = !a->show_review;
                if (cui_menu_item(ui, "计划卡面板", .icon = CUI_ICON_SPARKLES,
                                  .checked = a->show_plan))
                    a->show_plan = !a->show_plan;
                if (cui_menu_item(ui, "OOXML 结构树", .icon = CUI_ICON_FILE,
                                  .checked = a->show_ooxml))
                    a->show_ooxml = !a->show_ooxml;
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
                {
                    const char *sname = a->style >= 0 && a->style < NP_STYLE_COUNT
                                      ? np_styles[a->style].name : "自定义";
                    char slabel[96];
                    snprintf(slabel, sizeof slabel, "外观设置…（%s）", sname);
                    if (cui_menu_item(ui, slabel, .icon = CUI_ICON_PALETTE, .shortcut = "Ctrl ,"))
                        a->dlg_settings = true;
                }
                if (cui_menu_item(ui, "深色模式", .icon = CUI_ICON_MOON, .checked = a->dark)) {
                    a->dark = !a->dark;
                    a->dark_valid = true;
                    a->style = -1;              /* manual override → custom look */
                    cui_app_follow_system_theme(NULL, false);
                    cui_set_theme(ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
                }
                if (cui_menu_item(ui, "跟随系统主题", .checked = !a->dark_valid)) {
                    a->dark_valid = false;
                    a->style = -1;
                    cui_app_follow_system_theme(NULL, true);
                    a->dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
                }
                if (cui_menu_item(ui, "切换强调色", .icon = CUI_ICON_PALETTE)) {
                    a->accent = (a->accent + 1) % CUI_ACCENT_COUNT;
                    a->style = -1;
                    cui_set_accent(ui, (cui_accent)a->accent);
                }
                {
                    static const char *dens[3] = { "舒适", "紧凑", "宽松" };
                    char label[64];
                    snprintf(label, sizeof label, "界面密度：%s", dens[a->density]);
                    if (cui_menu_item(ui, label, .icon = CUI_ICON_SLIDERS_HORIZONTAL))
                        a->density = (a->density + 1) % 3;
                }
                cui_menu_separator(ui);
                {
                    char flabel[128];
                    snprintf(flabel, sizeof flabel, "编辑器字体（%s）", np_font_label(a));
                    cui_menu_label(ui, flabel);
                    if (cui_menu_item(ui, "默认 (Geist Mono)", .checked = !strcmp(a->font_ident, "default")))
                        np_font_reset(ui, a);
                    for (int fi = 0; fi < np_bundled_font_count; fi++) {
                        const np_bundled_font *bf = &np_bundled_fonts[fi];
                        char mi[128];
                        snprintf(mi, sizeof mi, "%s · %s", bf->label, bf->tag);
                        if (cui_menu_item(ui, mi, .checked = !strcmp(a->font_ident, bf->ident)))
                            np_font_apply_bundled(ui, a, fi);
                    }
                    if (cui_menu_item(ui, "自定义字体文件…", .icon = CUI_ICON_TYPE)) {
                        char fp[NP_PATH_CAP];
                        if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
                                .kind = CUI_FILE_OPEN, .title = "选择编辑器字体（TTF / OTF）",
                                .filter_name = "字体文件", .filter = "ttf;otf;ttc" },
                                fp, sizeof fp))
                            np_font_apply_file(ui, a, fp);
                    }
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
                if (cui_menu_item(ui, "缩进参考线", .checked = a->indent_guides,
                                  .disabled = d->is_hex))
                    a->indent_guides = !a->indent_guides;
                if (cui_menu_item(ui, "编程连字（实验）",
                                  .checked = a->editor_lig, .disabled = d->is_hex))
                    a->editor_lig = !a->editor_lig;
                if (cui_menu_item(ui,
                                  a->whitespace == 0 ? "空白字符：关"
                                  : a->whitespace == 1 ? "空白字符：仅缩进"
                                  : "空白字符：全部",
                                  .disabled = d->is_hex))
                    a->whitespace = (a->whitespace + 1) % 3;
                if (cui_menu_item(ui, "显示状态栏", .checked = a->show_status))
                    a->show_status = !a->show_status;
                cui_menu_separator(ui);
                if (cui_menu_item(ui, "沉浸式编辑（隐藏菜单/工具/状态栏）",
                                  .checked = a->zen, .shortcut = "F11"))
                    np_zen_set(a, !a->zen);
            }

            cui_menu_bar_menu(ui, "文档") {
                if (d->is_hex) {
                    cui_menu_label(ui, "当前：十六进制（二进制文件）");
                    cui_menu_separator(ui);
                    cui_menu_label(ui, "语法高亮在十六进制模式下不可用");
                } else {
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
            }

            /* LaTeX quick input: shows only while editing a TeX source. */
            if (np_doc_is_latex(d)) {
                cui_menu_bar_menu(ui, "LaTeX") {
                    cui_menu_label(ui, "选中文字后套用格式，否则插入占位模板");
                    for (int i = 0; i < NP_LATEX_SNIP_COUNT; i++) {
                        const np_latex_snip *s = &NP_LATEX_SNIPS[i];
                        if (cui_menu_item(ui, s->label, .shortcut = s->shortcut))
                            np_apply_latex_snip(a, i);
                    }
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, "LaTeX 模板新建…", .icon = CUI_ICON_FILE_TEXT)) {
                        a->latex_tpl_pick = 0;
                        a->dlg_latex_tpl = true;
                    }
                    if (cui_menu_item(ui, "在线模板库…", .icon = CUI_ICON_DOWNLOAD))
                        a->dlg_tpl_online = true;
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, "插入表格（Excel/CSV）…", .icon = CUI_ICON_GRID_3X3,
                                      .shortcut = "Ctrl Alt T")) {
                        a->dlg_latex_table = true;
                        if (!a->ltable_align[0]) snprintf(a->ltable_align, sizeof a->ltable_align, "lcr");
                        a->ltable_width = 0.9f;
                    }
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, "整理源码（缩进·换行·空白）…", .icon = CUI_ICON_SPARKLES)) {
                        a->dlg_latex_tidy = true;
                    }
                    if (cui_menu_item(ui, "字体设置（XeLaTeX / xeCJK）…", .icon = CUI_ICON_TYPE)) {
                        a->dlg_latex_fonts = true;
                    }
                    if (cui_menu_item(ui, "公式实时预览…", .icon = CUI_ICON_CODE,
                                      .shortcut = "Ctrl Alt M")) {
                        a->dlg_math_preview = true;
                        np_math_expr_from_caret(a);
                    }
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, "AI 助手", .icon = CUI_ICON_SPARKLES,
                                      .shortcut = "Ctrl Alt A",
                                      .checked = a->show_ai)) {
                        a->show_ai = !a->show_ai;
                    }
                    if (cui_menu_item(ui, "maclaw 官方服务登录…", .icon = CUI_ICON_SPARKLES)) {
                        a->dlg_maclaw = true;
                    }
                }
            }

            /* Markdown 段落 menu: mirrors Typora's paragraph menu, shown only
             * while editing a Markdown source. */
            if (np_doc_is_markdown(d)) {
                cui_menu_bar_menu(ui, "段落") {
                    cui_menu_label(ui, "标题 · 选中文字后套用格式，否则插入占位模板");
                    if (cui_menu_item(ui, "一级标题", .shortcut = "Ctrl Alt 1",
                                      .checked = np_md_line_hlevel(d) == 1)) np_md_set_heading(a, 1);
                    if (cui_menu_item(ui, "二级标题", .shortcut = "Ctrl Alt 2",
                                      .checked = np_md_line_hlevel(d) == 2)) np_md_set_heading(a, 2);
                    if (cui_menu_item(ui, "三级标题", .shortcut = "Ctrl Alt 3",
                                      .checked = np_md_line_hlevel(d) == 3)) np_md_set_heading(a, 3);
                    if (cui_menu_item(ui, "四级标题", .shortcut = "Ctrl Alt 4",
                                      .checked = np_md_line_hlevel(d) == 4)) np_md_set_heading(a, 4);
                    if (cui_menu_item(ui, "五级标题", .shortcut = "Ctrl Alt 5",
                                      .checked = np_md_line_hlevel(d) == 5)) np_md_set_heading(a, 5);
                    if (cui_menu_item(ui, "六级标题", .shortcut = "Ctrl Alt 6",
                                      .checked = np_md_line_hlevel(d) == 6)) np_md_set_heading(a, 6);
                    cui_menu_separator(ui);
                    if (cui_menu_item(ui, "段落", .shortcut = "Ctrl Alt 0",
                                      .checked = np_md_line_hlevel(d) == 0)) np_md_set_heading(a, 0);
                    if (cui_menu_item(ui, "提升标题级别", .shortcut = "Ctrl Alt ="))
                        np_md_adjust_heading(a, -1);
                    if (cui_menu_item(ui, "降低标题级别", .shortcut = "Ctrl Alt -"))
                        np_md_adjust_heading(a, 1);
                    cui_menu_separator(ui);

                    /* Blocks inserted as templates. */
                    const char *blocks[] = { "表格", "公式块", "代码块", "引用" };
                    for (int i = 0; i < 4; i++) {
                        int idx = np_md_snip_index(blocks[i]);
                        if (idx >= 0 && cui_menu_item(ui, NP_MD_SNIPS[idx].label,
                                                       .shortcut = NP_MD_SNIPS[idx].shortcut))
                            np_apply_md_snip(a, idx);
                    }
                    cui_menu(ui, "代码工具") {
                        const char *items[] = { "行内代码", "加粗", "斜体", "删除线", "高亮" };
                        for (int i = 0; i < 5; i++) {
                            int idx = np_md_snip_index(items[i]);
                            if (idx >= 0 && cui_menu_item(ui, NP_MD_SNIPS[idx].label,
                                                           .shortcut = NP_MD_SNIPS[idx].shortcut))
                                np_apply_md_snip(a, idx);
                        }
                    }
                    cui_menu(ui, "警告框") {
                        const char *items[] = { "提示 NOTE", "技巧 TIP", "重要 IMPORTANT",
                                                "警告 WARNING", "注意 CAUTION" };
                        for (int i = 0; i < 5; i++) {
                            int idx = np_md_snip_index(items[i]);
                            if (idx >= 0 && cui_menu_item(ui, NP_MD_SNIPS[idx].label))
                                np_apply_md_snip(a, idx);
                        }
                    }
                    cui_menu_separator(ui);

                    /* Lists + checkboxes + indentation. */
                    const char *lists[] = { "有序列表", "无序列表", "任务列表" };
                    for (int i = 0; i < 3; i++) {
                        int idx = np_md_snip_index(lists[i]);
                        if (idx >= 0 && cui_menu_item(ui, NP_MD_SNIPS[idx].label,
                                                       .shortcut = NP_MD_SNIPS[idx].shortcut))
                            np_apply_md_snip(a, idx);
                    }
                    cui_menu(ui, "任务状态") {
                        if (cui_menu_item(ui, "切换完成状态 [ ] / [x]")) np_md_toggle_task(a);
                    }
                    cui_menu(ui, "列表缩进") {
                        if (cui_menu_item(ui, "缩进")) np_md_indent(a, 1);
                        if (cui_menu_item(ui, "反缩进")) np_md_indent(a, -1);
                    }
                    cui_menu_separator(ui);

                    if (cui_menu_item(ui, "在上方插入段落")) np_md_insert_para(a, true);
                    if (cui_menu_item(ui, "在下方插入段落")) np_md_insert_para(a, false);
                    cui_menu_separator(ui);

                    const char *misc[] = { "链接", "脚注引用", "脚注定义",
                                           "图片", "水平分割线", "内容目录", "YAML Front Matter" };
                    for (int i = 0; i < 7; i++) {
                        int idx = np_md_snip_index(misc[i]);
                        if (idx >= 0 && cui_menu_item(ui, NP_MD_SNIPS[idx].label))
                            np_apply_md_snip(a, idx);
                    }
                }
            }

            cui_menu_bar_menu(ui, "格式") {
                if (d->is_hex) {
                    cui_menu_label(ui, "十六进制模式：字节级编辑，无换行符与编码概念");
                } else {
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
            }

            cui_menu_bar_menu(ui, "帮助") {
                if (cui_menu_item(ui, "快捷键", .icon = CUI_ICON_KEYBOARD)) a->dlg_help = true;
                if (cui_menu_item(ui, "在线更新…", .icon = CUI_ICON_DOWNLOAD)) {
                    a->dlg_update = true;
                    np_updater_start_check();
                }
                if (cui_menu_item(ui, "关于", .icon = CUI_ICON_INFO)) a->dlg_about = true;
            }
        }

        /* Toolbar ----------------------------------------------------- */
        cui_toolbar(ui, "tools", .style = CUI_TOOLBAR_PLAIN, .no_border = true) {
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
            if (cui_toolbar_button(ui, CUI_ICON_LIST, .tooltip = "在当前文件中列出匹配",
                                   .shortcut = "Ctrl Shift L")) {
                a->show_output = true;
                a->out_tab = 0;
                a->hits_rev = 0;
                if (!a->find_query[0]) np_open_find(a);
            }
            if (cui_toolbar_button(ui, CUI_ICON_FOLDER_OPEN, .tooltip = "在文件中查找…",
                                   .shortcut = "Ctrl Shift F")) {
                a->dlg_find_files = true;
                if (!a->ff_dir[0]) np_dir_set(a, a->side_dir);
            }
            if (cui_toolbar_button(ui, CUI_ICON_CODE, .tooltip = "语法高亮语言…"))
                np_open_lang_dialog(a);
            if (cui_toolbar_button(ui, CUI_ICON_CPU,
                                   .tooltip = d->is_hex ? "切换到文本编辑"
                                                        : "以十六进制模式重新打开"))
                np_hex_reopen(a, !d->is_hex);
            cui_toolbar_separator(ui);
            cui_toolbar_button(ui, CUI_ICON_PANEL_LEFT,
                               .tooltip = "目录 / 工作区侧栏  Ctrl B",
                               .toggle = &a->show_sidebar);
            cui_toolbar_button(ui, CUI_ICON_TERMINAL, .tooltip = "输出窗口  F7",
                               .toggle = &a->show_output);
            cui_toolbar_button(ui, CUI_ICON_EYE,
                               .tooltip = "预览面板  Ctrl Alt P（Markdown/LaTeX/PDF/Web）",
                               .toggle = &a->show_preview);
            cui_toolbar_separator(ui);
            if (cui_toolbar_button(ui, CUI_ICON_ZOOM_OUT, .tooltip = "缩小字号"))
                a->font_size = NP_MAX(NP_FONT_MIN, a->font_size - 1);
            if (cui_toolbar_button(ui, CUI_ICON_ZOOM_IN, .tooltip = "放大字号"))
                a->font_size = NP_MIN(NP_FONT_MAX, a->font_size + 1);
            cui_toolbar_spacer(ui);
            if (cui_toolbar_button(ui, CUI_ICON_SLIDERS_HORIZONTAL,
                                   .tooltip = "切换界面密度（舒适/紧凑/宽松）"))
                a->density = (a->density + 1) % 3;
            if (cui_toolbar_button(ui, CUI_ICON_PALETTE,
                                   .tooltip = "外观设置…（界面风格 / 主题）"))
                a->dlg_settings = true;
            if (cui_toolbar_button(ui, CUI_ICON_KEYBOARD,
                                   .tooltip = "快捷键速查（F1 打开命令面板）"))
                a->dlg_help = true;
            if (cui_toolbar_button(ui, a->dark ? CUI_ICON_SUN : CUI_ICON_MOON,
                                   .tooltip = a->dark ? "浅色模式" : "深色模式")) {
                a->dark = !a->dark;
                a->dark_valid = true;
                a->style = -1;
                cui_app_follow_system_theme(NULL, false);
                cui_set_theme(ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
            }
        }
        }   /* top chrome surface */
        if (measure)
            a->zen_chrome_h = cui_get_rect(ui, cui_make_id(ui, "#np-chrome")).h;
        }
        }   /* zen chrome easing: menu bar + toolbar fold with F11 */

        /* Main workspace: directory pane | (tabs + editor + output pane),
         * with the AI assistant docked on the far right when open (it sits
         * outside the sidebar/preview splits so pane toggles never move it). */
        if (a->show_ai) {
            cui_split(ui, "np_ai_split", &a->ai_ratio,
                      .min_first = 400, .min_second = 300) {
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
                cui_split_next(ui);
                np_ai_panel(ui, a, d);
            }
        } else if (a->show_workbench) {
            /* B1 AI 工作台 dock（Composer 意图表单；与 AI/计划卡/审查面板互斥） */
            cui_split(ui, "np_wb_split", &a->workbench_ratio,
                      .min_first = 400, .min_second = 260) {
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
                cui_split_next(ui);
                np_composer_panel_dock(ui, a, d);
            }
        } else if (a->show_plan) {
            /* B2 计划卡面板 dock（与 AI/审查面板互斥：show_ai > show_plan > show_review） */
            cui_split(ui, "np_plan_split", &a->plan_ratio,
                      .min_first = 400, .min_second = 260) {
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
                cui_split_next(ui);
                np_plan_panel_dock(ui, a, d);
            }
        } else if (a->show_ooxml) {
            /* F2 OOXML 结构树面板 dock（与 AI/计划卡/审查互斥） */
            cui_split(ui, "np_ooxml_split", &a->ooxml_ratio,
                      .min_first = 400, .min_second = 260) {
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
                cui_split_next(ui);
                np_ooxml_panel_dock(ui, a, d);
            }
        } else if (a->show_review) {
            /* B3 补丁审查面板 dock 在最右（与 AI 面板互斥：show_ai 优先） */
            cui_split(ui, "np_review_split", &a->review_ratio,
                      .min_first = 400, .min_second = 260) {
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
                cui_split_next(ui);
                np_review_panel_dock(ui, a, d);
            }
        } else if (a->show_sidebar) {
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
        {   /* same F11 easing as the top chrome strip */
            float svis = cui_animate_spring(ui, cui_make_id(ui, "#np-status-vis"),
                                            (a->show_status && !a->zen) ? 1.0f : 0.0f,
                                            0.15f, 0.0f);
            bool smeasure = a->zen_status_h <= 0 || (a->show_status && !a->zen && svis > 0.995f);
            if (svis > 0.01f) {
            cui_box(ui, .id = "#np-status", .width = CUI_FILL,
                    .max_height = smeasure ? 1e6f : a->zen_status_h * svis,
                    .clip = !smeasure, .gap = 0) {
            cui_status_bar(ui, "status") {
                if (d->dirty)
                    cui_status_bar_text(ui, "未保存", .icon = CUI_ICON_DOT, .tone = CUI_TONE_WARNING);
                else
                    cui_status_bar_text(ui, "已保存", .icon = CUI_ICON_CHECK, .tone = CUI_TONE_SUCCESS);
                cui_status_bar_text(ui, d->is_hex ? "HEX###modechip" : "TEXT###modechip",
                                    .icon = d->is_hex ? CUI_ICON_CPU : CUI_ICON_FILE_TEXT,
                                    .tone = d->is_hex ? CUI_TONE_VIOLET : CUI_TONE_INFO);
                char path[256 + 32];
                snprintf(path, sizeof path, "%s###path",
                         d->path[0] ? np_base(d->path) : "未命名文档");
                cui_status_bar_text(ui, path,
                                    .tooltip = d->path[0] ? d->path : "尚未保存到文件");
                char tabcnt[48];
                snprintf(tabcnt, sizeof tabcnt, "· %d/%d 标签###tabs", a->active + 1, a->count);
                cui_status_bar_text(ui, tabcnt, .tone = CUI_TONE_INFO);
                char panes[96];
                snprintf(panes, sizeof panes, "%s%s%s%s%s%s%s%s###panes",
                         a->show_sidebar ? "侧栏" : "",
                         (a->show_sidebar && (a->show_output || a->show_preview)) ? " · " : "",
                         a->show_output ? "输出" : "",
                         (a->show_preview && a->show_output) ? " · " : "",
                         a->show_preview ? "预览" : "",
                         (a->show_ai && (a->show_sidebar || a->show_output || a->show_preview)) ? " · " : "",
                         a->show_ai ? "AI" : "",
                         "");
                if (!a->show_sidebar && !a->show_output && !a->show_preview && !a->show_ai)
                    snprintf(panes, sizeof panes, "单栏###panes");
                if (cui_status_bar_item(ui, panes, .tooltip = "点击：切换侧栏；Ctrl+B 侧栏 / F7 输出 / Ctrl Alt P 预览 / Ctrl Alt A AI"))
                    a->show_ai = !a->show_ai;
                cui_status_bar_spacer(ui);
                if (d->is_hex) {
                    char hx[96];
                    snprintf(hx, sizeof hx, "偏移 0x%zX###hexpos", d->hexsel.cursor);
                    if (cui_status_bar_item(ui, hx, .tooltip = "点击：转到偏移…"))
                        a->dlg_hexgoto = true;
                    if (d->hexsel.length) {
                        char sel[64];
                        snprintf(sel, sizeof sel, "已选 %zu 字节###hexsel", d->hexsel.length);
                        cui_status_bar_text(ui, sel, .tone = CUI_TONE_INFO);
                    }
                    char sz[48];
                    np_size_str(sz, sizeof sz, (size_t)d->len);
                    char count2[128];
                    snprintf(count2, sizeof count2, "共 %d 字节 · %s###hexcount", d->len, sz);
                    cui_status_bar_text(ui, count2);
                } else {
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
                }
                char fn[128];
                snprintf(fn, sizeof fn, "%s###fontname", np_font_label(a));
                cui_status_bar_text(ui, fn);
                char fs[32];
                snprintf(fs, sizeof fs, "%d pt###font", a->font_size);
                if (cui_status_bar_item(ui, fs, .tooltip = "点击重置字号"))
                    a->font_size = NP_FONT_DEF;
            }   /* cui_status_bar */
            }   /* cui_box (height easing wrapper) */
            if (smeasure)
                a->zen_status_h = cui_get_rect(ui, cui_make_id(ui, "#np-status")).h;
            }   /* svis > 0.01 */
        }   /* status bar easing */
    }

    /* Command palette (#10/#11): static table + recent files, rebuilt every
     * frame so dynamic titles stay fresh; the call itself runs every frame
     * to keep the exit transition visible. */
    {
        static const char *dens[3] = { "舒适", "紧凑", "宽松" };
        static const char *wsm[3] = { "关", "仅缩进", "全部" };
        char dyn[2][48];
        snprintf(dyn[0], sizeof dyn[0], "界面密度：%s", dens[a->density]);
        snprintf(dyn[1], sizeof dyn[1], "空白字符显示：%s", wsm[a->whitespace]);
        char rt[NP_RECENT_MAX][NP_PATH_CAP];
        cui_command cmds[NP_CMD_COUNT + NP_RECENT_MAX];
        int n = NP_CMD_COUNT;
        for (int i = 0; i < NP_CMD_COUNT; i++) {
            cmds[i].title = np_cmds[i].title;
            cmds[i].icon = np_cmds[i].icon;
            cmds[i].group = np_cmds[i].group;
            cmds[i].shortcut = np_cmds[i].shortcut;
        }
        cmds[NP_CMD_DENSITY].title = dyn[0];
        cmds[NP_CMD_WS].title = dyn[1];
        for (int i = 0; i < a->recent_count && i < NP_RECENT_MAX; i++) {
            snprintf(rt[i], NP_PATH_CAP, "打开 %s", np_base(a->recent[i]));
            cmds[n].title = rt[i];
            cmds[n].icon = CUI_ICON_CLOCK;
            cmds[n].group = "最近文件";
            cmds[n].shortcut = NULL;
            n++;
        }
        int pick = cui_command_palette_ex(ui, &a->palette_open, cmds, n,
                                          &(cui_palette_opts){
                .placeholder = "输入命令或搜索…",
                .hint_nav = "选择", .hint_select = "执行", .hint_close = "关闭",
                .no_results = "没有匹配的命令",
        });
        if (pick >= 0) {
            if (pick < NP_CMD_COUNT) np_cmd_run(a, pick);
            else np_open_path(a, a->recent[pick - NP_CMD_COUNT], true);
        }
    }

    np_dialogs(a, ui);

    /* Keep the native WebView child view aligned with its host box after
     * this frame's layout has been computed. */
    np_sync_webview(a, ui);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

#ifdef __ANDROID__
/* Seed the app-private documents folder with bundled sample files on first
 * run, and open a sensible default document when launched from the launcher
 * (Android gives SDL_Main no argv). Also asks CUI for a 2x logical scale:
 * the software renderer reports 1:1 pixels, which is unreadable on a dense
 * pad screen. Overridable with the CUI_SCALE environment variable. */
static void np_android_bootstrap(notepad *a) {
    if (!getenv("CUI_SCALE")) setenv("CUI_SCALE", "2", 1);
    /* Renderer: Auto tries OpenGL ES 3 (the SDL backend requests an ES 3.0
     * context on Android and CUI's GL renderer ships ES 3.00 shaders); if the
     * context cannot be created it falls back to the software renderer. Set
     * CUI_RENDERER=software to force the CPU path (see cui_app.c). */
    const char *base = SDL_AndroidGetInternalStoragePath();
    if (!base || !base[0]) return;
    char docs[NP_PATH_CAP];
    snprintf(docs, sizeof docs, "%s/docs", base);
    mkdir(docs, 0755);
    static const char *samples[] = { "欢迎.md", "示例.html", "示例.c" };
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        char dst[NP_PATH_CAP];
        snprintf(dst, sizeof dst, "%s/docs/%s", base, samples[i]);
        struct stat st;
        if (stat(dst, &st) == 0) continue;
        SDL_RWops *src = SDL_RWFromFile(samples[i], "rb");   /* APK asset */
        if (!src) continue;
        FILE *f = fopen(dst, "wb");
        if (f) {
            char buf[8192];
            size_t n;
            while ((n = SDL_RWread(src, buf, 1, sizeof buf)) > 0)
                fwrite(buf, 1, n, f);
            fclose(f);
        }
        SDL_RWclose(src);
    }
    /* The sidebar starts at "/" which scoped storage makes unreadable; point
     * it at the seeded documents folder instead. */
    if (a->side_dir[0] == '\0' || strcmp(a->side_dir, "/") == 0) {
        snprintf(a->side_dir, sizeof a->side_dir, "%s/docs", base);
        a->side_reload = true;
    }
    if (a->startup_count == 0 && a->session_count == 0)
        snprintf(a->startup[a->startup_count++], NP_PATH_CAP,
                 "%s/docs/欢迎.md", base);
}
#endif

/* T3 默认下载器：薄适配 np_net_download（主程序已链 np_net）。
 * 与 np_toolinstall.c 解耦——后者被多个单测链接，不引网络依赖。 */
static bool np_ti_real_download(const char *url, const char *dest, void *user) {
    (void)user;
    return np_net_download(url, dest, NULL);
}

int main(int argc, char **argv) {
    np_ti_set_default_downloader(np_ti_real_download);
    for (int i = 0; i < NP_DOC_MAX; i++) g.startup_cur[i] = -1;
    g.font_size = NP_FONT_DEF;
    snprintf(g.font_ident, sizeof g.font_ident, "default");
    g.line_numbers = true;
    g.indent_guides = true;
    g.whitespace = 1;
    g.show_status = true;
    g.accent = CUI_ACCENT_BLUE;
    g.density = 0;
    g.match_case = false;
    g.find_word = false;
    g.hexfind_at = (size_t)-1;
    g.pending_action = ACT_NONE;
    g.pending_doc = -1;
    g.show_sidebar = true;
    g.show_output = true;
    g.follow_active = true;
    g.show_preview = true;
    g.preview_ratio = 0.58f;
    np_tex_job_init(&g.tex_job);
    np_http_init(&g.web_server);
    g.side_ratio = 0.22f;
    g.ai_ratio = 0.72f;
    g.review_ratio = 0.72f;
    g.plan_ratio = 0.72f;
    g.workbench_ratio = 0.72f;
    g.ooxml_ratio = 0.72f;
    g.inline_delay = 1;
    g.out_ratio = 0.76f;
    g.side_tab = 0;
    g.out_tab = 1;           /* start on the message log */
    g.last_active = -1;
    g.make_backup = true;
    g.auto_save = false;
    g.session_restore = true;
    g.tidy_indent = true; g.tidy_join = true; g.tidy_trim = true;
    g.tidy_spaces = 4;
    /* maclaw HubCenter HA candidates — probe and use the first healthy one.
     * The actual Hub is resolved by the relay at login and kept in maclaw_hub. */
    snprintf(g.maclaw_centers, sizeof g.maclaw_centers, "%s",
             "https://hubs.mypapers.top,https://hubs.maclaw.top,https://hubs2.maclaw.top");
    g.llm_prov_edit = -1;
    g.sp_edit = -1;              /* X9: -1 = 无编辑表单 */
    g.mcp_edit = -1;             /* X6: -1 = 无编辑表单 */
#ifdef _WIN32
    g.default_crlf = true;
#endif
    np_config_load(&g);      /* overrides the defaults above, restores UI state */
    np_init_side_dir(&g);    /* no-op when the config carried a workdir */
    np_doc_new(&g);          /* the initial empty tab */

#ifdef _WIN32
    /* argv from the CRT is in the ANSI code page; use the wide command line
     * instead so a path with Chinese characters round-trips as UTF-8. */
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 1; i < wargc && g.startup_count < NP_DOC_MAX; i++) {
            char arg[NP_PATH_CAP];
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                arg, NP_PATH_CAP, NULL, NULL);
            if (!strncmp(arg, "--bt=", 5))
                snprintf(g.startup_bt, sizeof g.startup_bt, "%s", arg + 5);
            else
                snprintf(g.startup[g.startup_count++], NP_PATH_CAP, "%s", arg);
        }
        LocalFree(wargv);
    }
#else
    for (int i = 1; i < argc && g.startup_count < NP_DOC_MAX; i++) {
        if (!strncmp(argv[i], "--bt=", 5))
            snprintf(g.startup_bt, sizeof g.startup_bt, "%s", argv[i] + 5);
        else
            snprintf(g.startup[g.startup_count++], NP_PATH_CAP, "%s", argv[i]);
    }
#endif

    /* No files on the command line: reopen the last session (unsaved and
     * untitled documents were never recorded, so nothing is lost). */
    if (g.startup_count == 0 && g.session_restore) {
        for (int i = 0; i < g.session_count && g.startup_count < NP_DOC_MAX; i++) {
            g.startup_cur[g.startup_count] = g.session_cur[i];
            snprintf(g.startup[g.startup_count++], NP_PATH_CAP, "%s", g.session[i]);
        }
        for (int i = 0; i < g.draft_count && g.startup_count < NP_DOC_MAX; i++)
            snprintf(g.startup[g.startup_count++], NP_PATH_CAP, "draft:%d", i);
    }

#ifdef __ANDROID__
    np_android_bootstrap(&g);
#endif

    int rc = cui_app_run(&(cui_app_desc){
        .title = "未命名 - RapidEditor",
        .width = 1120, .height = 760,
        .min_width = 620, .min_height = 460,
        .app_id = "com.rapidaire.rapideditor",
        .accent = CUI_ACCENT_BLUE,
        .follow_system_theme = true
    }, np_frame, &g);
    np_config_save(&g);      /* settings, recents and the open-file session */
    np_webview_shutdown();
    np_preview_stop_http(&g);
    np_tex_job_free(&g.tex_job);
    return rc;
}
