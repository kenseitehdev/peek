// peek.c
// A tiny ncurses pager with multi-buffer, search, wrap toggle, line numbers, copy-mode,
// HTTP request support, wget, w3m -dump, SQL query support, and extended language highlighting.
#define _POSIX_C_SOURCE 200809L
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <locale.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <strings.h>
#define MAX_BUFFERS 50
#define MAX_LINES 10000
#define MAX_LINE_LEN 2048

typedef enum {
    LANG_NONE = 0,
    LANG_C,
    LANG_CPP,
    LANG_PYTHON,
    LANG_JAVA,
    LANG_JS,
    LANG_TS,
    LANG_HTML,
    LANG_CSS,
    LANG_SHELL,
    LANG_MARKDOWN,
    LANG_MAN,
    LANG_RUST,
    LANG_GO,
    LANG_RUBY,
    LANG_PHP,
    LANG_SQL,
    LANG_JSON,
    LANG_XML,
    LANG_YAML
} Language;

typedef struct {
    char *lines[MAX_LINES];
    int line_count;
    char filepath[1024];
    char http_request[512];  // Store the HTTP request command if this is an HTTP buffer
    Language lang;
    int scroll_offset;
    int is_active;
    int is_http_buffer;      // Flag to indicate this is an HTTP response buffer
} Buffer;

typedef struct {
    Buffer buffers[MAX_BUFFERS];
    int buffer_count;
    int current_buffer;
    char search_term[256];
    int search_line;
    int search_match_count;
    int current_match;
    int show_line_numbers;
    int wrap_enabled;

    // Copy/visual
    int copy_mode;
    int copy_start_line;
    int copy_end_line;
    int horiz_scroll_offset;    // Horizontal scroll position
    int horiz_scroll_step;   
} ViewerState;

// Color pairs
#define COLOR_NORMAL 1
#define COLOR_KEYWORD 2
#define COLOR_STRING 3
#define COLOR_COMMENT 4
#define COLOR_NUMBER 5
#define COLOR_TYPE 6
#define COLOR_FUNCTION 7
#define COLOR_TABBAR 8
#define COLOR_STATUS 9
#define COLOR_LINENR 10
#define COLOR_COPY_SELECT 11
 static int is_pdf_file(const char *filepath);
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s [OPTIONS] <file1> [file2 ...]\n"
        "  %s -                       (read from stdin)\n"
        "  cmd | %s                   (read from stdin)\n"
        "  cmd | %s - file            (stdin + file)\n"
        "\n"
        "Options:\n"
        "  --no-wrap                  Disable line wrapping on startup\n"
        "\n"
        "Man buffers (AUTO-DETECT):\n"
        "  %s \"man grep\" \"man sed\" file1 \"man awk\" file2\n"
        "\n"
        "Optional explicit command mode:\n"
        "  %s -m \"man grep\" -m \"man sed\" file1\n"
        "  %s -m \"wget -qO- https://example.com\" file1\n"
        "  %s -m \"w3m -dump https://example.com\" file2\n"
        "\n"
        "Keybindings:\n"
        "  j/k           Scroll down/up\n"
        "  h/l           Scroll left/right (when wrap is OFF)\n"
        "  0/$           Jump to start/end of line (when wrap is OFF)\n"
        "  g/G           Go to top/bottom\n"
        "  d/u           Half-page down/up\n"
        "  /             Search\n"
        "  n/N           Next/previous match\n"
        "  r             Make HTTP request (opens popup)\n"
        "  R             Reload current HTTP buffer\n"
        "  w             Fetch URL with wget (opens popup)\n"
        "  W             Fetch URL with w3m -dump (opens popup)\n"
        "  f             Fetch RSS/Atom feed (opens popup)\n"
        "  s             SQL query (opens popup)\n"
        "  x             Close current buffer\n"
        "  o             Open file with fzf\n"
        "  Tab/Shift-Tab Switch buffers\n"
        "  L             Toggle line numbers\n"
        "  T             Toggle line wrapping\n"
        "  v             Enter visual/copy mode\n"
        "  y             Copy selection (in copy mode)\n"
        "  Esc           Exit copy mode / Cancel popup\n"
        "  q             Quit\n",
        prog, prog, prog, prog, prog, prog, prog, prog
    );
}
static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static int contains_substr(const char *s, const char *needle) {
    return strstr(s, needle) != NULL;
}

static int is_man_command_arg(const char *arg) {
    if (!arg || !*arg) return 0;
    if (starts_with(arg, "man ")) return 1;

    // crude: if it contains "man " anywhere
    if (contains_substr(arg, "man ")) {
        const char *p = strstr(arg, "man ");
        return (p && p[4] != '\0');
    }
    return 0;
}

static int cmd_exists(const char *name) {
    if (!name || !*name) return 0;
    const char *path = getenv("PATH");
    if (!path) return 0;

    char buf[4096];
    strncpy(buf, path, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    char *save = NULL;
    for (char *dir = strtok_r(buf, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

// --- Cleanup helpers ---------------------------------------------------------

// Strip classic man overstrikes (bold/underline via backspace patterns)
static void strip_overstrikes(char *s) {
    char *dst = s;
    for (char *src = s; *src; src++) {
        if (*src == '\b') {
            if (dst > s) dst--;
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
}

// Strip ANSI/VT escape sequences (colors, cursor moves, etc.)
static void strip_ansi(char *s) {
    char *d = s;
    for (char *p = s; *p; ) {
        if ((unsigned char)*p == 0x1B) { // ESC
            p++;
            if (*p == '[') { // CSI
                p++;
                while (*p && !(*p >= '@' && *p <= '~')) p++;
                if (*p) p++;
                continue;
            } else if (*p == ']') { // OSC ... BEL
                p++;
                while (*p && (unsigned char)*p != 0x07) p++;
                if (*p) p++;
                continue;
            } else {
                if (*p) p++; // other ESC X
                continue;
            }
        }
        *d++ = *p++;
    }
    *d = '\0';
}

// Trim trailing whitespace (useful for man output)
static void rtrim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

// --- Language detection ------------------------------------------------------

Language detect_language(const char *filepath) {
    const char *ext = strrchr(filepath, '.');
    if (!ext) return LANG_NONE;

    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return LANG_C;
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".hpp") == 0 || strcmp(ext, ".cxx") == 0) return LANG_CPP;
    if (strcmp(ext, ".py") == 0) return LANG_PYTHON;
    if (strcmp(ext, ".java") == 0) return LANG_JAVA;
    if (strcmp(ext, ".js") == 0) return LANG_JS;
    if (strcmp(ext, ".ts") == 0 || strcmp(ext, ".tsx") == 0) return LANG_TS;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return LANG_HTML;
    if (strcmp(ext, ".css") == 0) return LANG_CSS;
    if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0 || strcmp(ext, ".zsh") == 0) return LANG_SHELL;
    if (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0) return LANG_MARKDOWN;
    if (strcmp(ext, ".rs") == 0) return LANG_RUST;
    if (strcmp(ext, ".go") == 0) return LANG_GO;
    if (strcmp(ext, ".rb") == 0) return LANG_RUBY;
    if (strcmp(ext, ".php") == 0) return LANG_PHP;
    if (strcmp(ext, ".sql") == 0) return LANG_SQL;
    if (strcmp(ext, ".json") == 0) return LANG_JSON;
    if (strcmp(ext, ".xml") == 0) return LANG_XML;
    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return LANG_YAML;

    if (strstr(filepath, "/man/") || strstr(filepath, ".man")) return LANG_MAN;
    return LANG_NONE;
}

int is_c_keyword(const char *word) {
    const char *keywords[] = {
        "auto","break","case","char","const","continue","default","do",
        "double","else","enum","extern","float","for","goto","if",
        "int","long","register","return","short","signed","sizeof","static",
        "struct","switch","typedef","union","unsigned","void","volatile","while",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_python_keyword(const char *word) {
    const char *keywords[] = {
        "False","None","True","and","as","assert","async","await",
        "break","class","continue","def","del","elif","else","except",
        "finally","for","from","global","if","import","in","is",
        "lambda","nonlocal","not","or","pass","raise","return","try",
        "while","with","yield",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_js_keyword(const char *word) {
    const char *keywords[] = {
        "async","await","break","case","catch","class","const","continue",
        "debugger","default","delete","do","else","export","extends","finally",
        "for","function","if","import","in","instanceof","let","new",
        "return","super","switch","this","throw","try","typeof","var",
        "void","while","with","yield",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_rust_keyword(const char *word) {
    const char *keywords[] = {
        "as","async","await","break","const","continue","crate","dyn",
        "else","enum","extern","false","fn","for","if","impl","in",
        "let","loop","match","mod","move","mut","pub","ref","return",
        "self","Self","static","struct","super","trait","true","type",
        "unsafe","use","where","while",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_go_keyword(const char *word) {
    const char *keywords[] = {
        "break","case","chan","const","continue","default","defer","else",
        "fallthrough","for","func","go","goto","if","import","interface",
        "map","package","range","return","select","struct","switch","type",
        "var",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_ruby_keyword(const char *word) {
    const char *keywords[] = {
        "BEGIN","END","alias","and","begin","break","case","class",
        "def","defined?","do","else","elsif","end","ensure","false",
        "for","if","in","module","next","nil","not","or","redo",
        "rescue","retry","return","self","super","then","true","undef",
        "unless","until","when","while","yield",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_php_keyword(const char *word) {
    const char *keywords[] = {
        "abstract","and","array","as","break","callable","case","catch",
        "class","clone","const","continue","declare","default","die","do",
        "echo","else","elseif","empty","enddeclare","endfor","endforeach",
        "endif","endswitch","endwhile","eval","exit","extends","final",
        "finally","for","foreach","function","global","goto","if","implements",
        "include","include_once","instanceof","insteadof","interface","isset",
        "list","namespace","new","or","print","private","protected","public",
        "require","require_once","return","static","switch","throw","trait",
        "try","unset","use","var","while","xor","yield",
        NULL
    };
    for (int i = 0; keywords[i]; i++) if (strcmp(word, keywords[i]) == 0) return 1;
    return 0;
}

int is_sql_keyword(const char *word) {
    const char *keywords[] = {
        "SELECT","FROM","WHERE","INSERT","UPDATE","DELETE","CREATE","DROP",
        "ALTER","TABLE","INDEX","VIEW","JOIN","INNER","LEFT","RIGHT","OUTER",
        "ON","AND","OR","NOT","NULL","IS","IN","LIKE","BETWEEN","ORDER","BY",
        "GROUP","HAVING","LIMIT","OFFSET","AS","DISTINCT","COUNT","SUM","AVG",
        "MAX","MIN","UNION","ALL","EXISTS","CASE","WHEN","THEN","ELSE","END",
        NULL
    };
    for (int i = 0; keywords[i]; i++) {
        if (strcasecmp(word, keywords[i]) == 0) return 1;
    }
    return 0;
}

// --- MAN "syntax highlighting" ----------------------------------------------

// "SECTION" headers in rendered man pages are usually all caps words on a line.
static int is_man_section_header(const char *s) {
    int letters = 0;
    for (; *s; s++) {
        if (*s == ' ') continue;
        if (!isalpha((unsigned char)*s)) return 0;
        letters++;
        if (!isupper((unsigned char)*s)) return 0;
    }
    return letters >= 3;
}

// Helper: highlight a token region [i, j)
static void draw_tok(int y, int x, const char *s, int i, int j, int max_x, int pair, int bold) {
    if (x >= max_x) return;
    if (bold) attron(COLOR_PAIR(pair) | A_BOLD);
    else attron(COLOR_PAIR(pair));

    for (int k = i; k < j && x < max_x; k++) {
        mvaddch(y, x++, s[k]);
    }

    if (bold) attroff(COLOR_PAIR(pair) | A_BOLD);
    else attroff(COLOR_PAIR(pair));
}

// --- Wrapping ---------------------------------------------------------------

typedef struct {
    int count;
    char **segments;
} WrappedLine;

WrappedLine wrap_line(const char *line, int width) {
    WrappedLine result = {0, NULL};
    if (!line || width <= 0) return result;

    int len = (int)strlen(line);
    if (len == 0) {
        result.segments = malloc(sizeof(char*));
        result.segments[0] = strdup("");
        result.count = 1;
        return result;
    }

    int max_segments = (len / width) + 2;
    result.segments = malloc(max_segments * sizeof(char*));

    int pos = 0;
    while (pos < len) {
        int remaining = len - pos;
        int take = (remaining > width) ? width : remaining;

        char *seg = malloc(take + 1);
        strncpy(seg, line + pos, take);
        seg[take] = '\0';

        result.segments[result.count++] = seg;
        pos += take;
    }
    return result;
}

void free_wrapped_line(WrappedLine *wl) {
    if (!wl || !wl->segments) return;
    for (int i = 0; i < wl->count; i++) free(wl->segments[i]);
    free(wl->segments);
    wl->segments = NULL;
    wl->count = 0;
}

// --- Highlighter ------------------------------------------------------------

void highlight_line(const char *line, Language lang, int y, int start_x, int line_width) {
    if (!line) return;

    // MAN highlighting (rendered man text)
    if (lang == LANG_MAN) {
        const char *p = line;
        while (*p == ' ') p++;

        // Section headers (NAME, SYNOPSIS, DESCRIPTION, OPTIONS, etc.)
        if (is_man_section_header(p)) {
            attron(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            mvaddnstr(y, start_x, line, line_width - start_x);
            attroff(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            return;
        }

        int len = (int)strlen(line);
        int i = 0;
        int x = start_x;

        while (i < len && x < line_width) {
            unsigned char ch = (unsigned char)line[i];

            // whitespace
            if (isspace(ch)) {
                mvaddch(y, x++, line[i++]);
                continue;
            }

            // options like -a, -rf, --color=auto
            if (line[i] == '-') {
                int j = i;
                while (j < len && !isspace((unsigned char)line[j])) j++;

                // highlight option flags
                draw_tok(y, x, line, i, j, line_width, COLOR_NUMBER, 1);
                x += (j - i);
                i = j;
                continue;
            }

            // function-ish tokens like printf(3), open(2), etc.
            if (isalpha((unsigned char)line[i]) || line[i] == '_') {
                int j = i;
                while (j < len && (isalnum((unsigned char)line[j]) || line[j] == '_' || line[j] == '-')) j++;

                // check for "(digit)" right after
                int k = j;
                if (k + 2 < len && line[k] == '(' && isdigit((unsigned char)line[k+1])) {
                    int kk = k+2;
                    while (kk < len && isdigit((unsigned char)line[kk])) kk++;
                    if (kk < len && line[kk] == ')') {
                        // highlight name as function/type and the section part too
                        draw_tok(y, x, line, i, j, line_width, COLOR_FUNCTION, 1);
                        x += (j - i);
                        draw_tok(y, x, line, k, kk+1, line_width, COLOR_TYPE, 0);
                        x += (kk+1 - k);
                        i = kk + 1;
                        continue;
                    }
                }

                // default: normal word
                while (i < j && x < line_width) mvaddch(y, x++, line[i++]);
                continue;
            }

            // everything else
            mvaddch(y, x++, line[i++]);
        }
        return;
    }

    // --- Code-ish highlighting for other languages --------------------------
    int len = (int)strlen(line);
    int i = 0;
    int col = start_x;

    while (i < len && col < line_width) {
        char ch = line[i];

        // C-style comments
        if ((lang == LANG_C || lang == LANG_CPP || lang == LANG_JAVA || lang == LANG_JS ||
             lang == LANG_TS || lang == LANG_CSS || lang == LANG_RUST || lang == LANG_GO || lang == LANG_PHP) &&
            i + 1 < len && line[i] == '/' && line[i+1] == '/') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            int j = i;
            while (j < len && col < line_width) mvaddch(y, col++, line[j++]);
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        // Hash comments
        if ((lang == LANG_PYTHON || lang == LANG_SHELL || lang == LANG_RUBY ||
             lang == LANG_YAML || lang == LANG_PHP) && ch == '#') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            int j = i;
            while (j < len && col < line_width) mvaddch(y, col++, line[j++]);
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        // SQL comments
        if (lang == LANG_SQL && i + 1 < len && line[i] == '-' && line[i+1] == '-') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            int j = i;
            while (j < len && col < line_width) mvaddch(y, col++, line[j++]);
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        // String literals
        if (ch == '"' || ch == '\'') {
            char quote = ch;
            attron(COLOR_PAIR(COLOR_STRING));
            mvaddch(y, col++, ch);
            i++;
            while (i < len && col < line_width) {
                ch = line[i];
                mvaddch(y, col++, ch);
                if (ch == quote && (i == 0 || line[i-1] != '\\')) { i++; break; }
                i++;
            }
            attroff(COLOR_PAIR(COLOR_STRING));
            continue;
        }

        // Numbers
        if (isdigit((unsigned char)ch)) {
            attron(COLOR_PAIR(COLOR_NUMBER));
            while (i < len && col < line_width &&
                   (isdigit((unsigned char)line[i]) || line[i] == '.' ||
                    line[i] == 'x' || line[i] == 'X' ||
                    (line[i] >= 'a' && line[i] <= 'f') ||
                    (line[i] >= 'A' && line[i] <= 'F'))) {
                mvaddch(y, col++, line[i++]);
            }
            attroff(COLOR_PAIR(COLOR_NUMBER));
            continue;
        }

        // Keywords and identifiers
        if (isalpha((unsigned char)ch) || ch == '_') {
            char word[128] = {0};
            int w = 0;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_') && w < 127) {
                word[w++] = line[i++];
            }
            word[w] = '\0';

            int is_keyword = 0;
            if (lang == LANG_C || lang == LANG_CPP) is_keyword = is_c_keyword(word);
            else if (lang == LANG_PYTHON) is_keyword = is_python_keyword(word);
            else if (lang == LANG_JS || lang == LANG_TS) is_keyword = is_js_keyword(word);
            else if (lang == LANG_RUST) is_keyword = is_rust_keyword(word);
            else if (lang == LANG_GO) is_keyword = is_go_keyword(word);
            else if (lang == LANG_RUBY) is_keyword = is_ruby_keyword(word);
            else if (lang == LANG_PHP) is_keyword = is_php_keyword(word);
            else if (lang == LANG_SQL) is_keyword = is_sql_keyword(word);

            if (is_keyword) attron(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);

            for (int k = 0; k < w && col < line_width; k++) mvaddch(y, col++, word[k]);

            if (is_keyword) attroff(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            continue;
        }

        mvaddch(y, col++, ch);
        i++;
    }
}

// --- Loaders ----------------------------------------------------------------

int load_file(Buffer *buf, const char *filepath) {
    // First check if this is a PDF file BEFORE opening
    if (is_pdf_file(filepath)) {
        int have_pdftotext = cmd_exists("pdftotext");
        if (have_pdftotext) {
            buf->line_count = 0;
            buf->scroll_offset = 0;
            strncpy(buf->filepath, filepath, sizeof(buf->filepath) - 1);
            buf->filepath[sizeof(buf->filepath) - 1] = '\0';
            buf->lang = LANG_NONE;
            buf->is_active = 1;
            buf->is_http_buffer = 0;
            buf->http_request[0] = '\0';
            
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "pdftotext -layout '%s' - 2>/dev/null", filepath);
            
            FILE *p = popen(cmd, "r");
            if (!p) return -1;
            
            char line[MAX_LINE_LEN];
            while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                
                strip_overstrikes(line);
                strip_ansi(line);
                rtrim(line);
                
                buf->lines[buf->line_count++] = strdup(line);
            }
            
            int rc = pclose(p);
            (void)rc;
            
            return (buf->line_count > 0) ? 0 : -1;
        } else {
            fprintf(stderr, "Warning: pdftotext not found. Install poppler-utils to view PDFs.\n");
            return -1;
        }
    }
    
    // Regular file handling
    FILE *f = fopen(filepath, "r");
    if (!f) return -1;

    buf->line_count = 0;
    buf->scroll_offset = 0;
    strncpy(buf->filepath, filepath, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';
    buf->lang = detect_language(filepath);
    buf->is_active = 1;
    buf->is_http_buffer = 0;
    buf->http_request[0] = '\0';

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f) && buf->line_count < MAX_LINES) {
        line[strcspn(line, "\n")] = 0;
        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);
        buf->lines[buf->line_count++] = strdup(line);
    }

    fclose(f);
    return 0;
}
int load_stdin(Buffer *buf) {
    buf->line_count = 0;
    buf->scroll_offset = 0;
    strncpy(buf->filepath, "<stdin>", sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';
    buf->lang = LANG_NONE;
    buf->is_active = 1;
    buf->is_http_buffer = 0;
    buf->http_request[0] = '\0';

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), stdin) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    return buf->line_count > 0 ? 0 : -1;
}

static int load_command(Buffer *buf, const char *label, const char *cmd, Language lang) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    buf->line_count = 0;
    buf->scroll_offset = 0;
    buf->is_active = 1;
    buf->lang = lang;
    buf->is_http_buffer = 0;
    buf->http_request[0] = '\0';

    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}

// Wrap a detected "man foo" command so output is plain text (no ANSI pager junk).
static void build_man_cmd_plain(char *out, size_t outsz, const char *man_cmd) {
    // Force MANPAGER=cat, and if 'col' exists, pass through col -bx (optional).
    // We still strip ANSI regardless, but this reduces environment-dependent surprises.
    int have_col = cmd_exists("col");

    if (have_col) {
        snprintf(out, outsz, "MANPAGER=cat %s 2>/dev/null | col -bx", man_cmd);
    } else {
        snprintf(out, outsz, "MANPAGER=cat %s 2>/dev/null", man_cmd);
    }
}

void free_buffer(Buffer *buf) {
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }
    buf->line_count = 0;
    buf->is_active = 0;
}

// --- HTTP Request Functions -------------------------------------------------

int load_http_response(Buffer *buf, const char *request_input) {
    // Free existing buffer content
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }

    buf->line_count = 0;
    buf->scroll_offset = 0;
    buf->is_active = 1;
    buf->is_http_buffer = 1;
    buf->lang = LANG_NONE;

    // Store the request for reload functionality
    strncpy(buf->http_request, request_input, sizeof(buf->http_request) - 1);
    buf->http_request[sizeof(buf->http_request) - 1] = '\0';

    // Build the command: Use xh with pretty printing for both headers and body
    // --print=hb shows headers and body, --pretty=format forces formatting
    // Then try to format JSON with jq if present, otherwise show as-is
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "OUTPUT=$(xh --print=hb --pretty=format %s 2>&1); "
             "echo \"$OUTPUT\" | jq -C . 2>/dev/null || echo \"$OUTPUT\"",
             request_input);

    // Create a label for the buffer
    char label[256];
    snprintf(label, sizeof(label), "[HTTP: %s]", request_input);
    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    // Execute the command and capture output
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}
// --- RSS Feed Support -------------------------------------------------------

int load_rss_feed(Buffer *buf, const char *url) {
    // Free existing buffer content
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }

    buf->line_count = 0;
    buf->scroll_offset = 0;
    buf->is_active = 1;
    buf->is_http_buffer = 1;
    buf->lang = LANG_NONE;

    // Store the URL for reload functionality
    strncpy(buf->http_request, url, sizeof(buf->http_request) - 1);
    buf->http_request[sizeof(buf->http_request) - 1] = '\0';

    // Build RSS formatting command
    // This fetches the feed, formats the XML, then extracts key fields
    char cmd[4096];
    int have_xmllint = cmd_exists("xmllint");

    if (have_xmllint) {
        snprintf(cmd, sizeof(cmd),
                 "curl -sL '%s' 2>&1 | xmllint --format - 2>/dev/null | "
                 "awk 'BEGIN{RS=\"<item>\"; FS=\"\\n\"} "
                 "NR>1 { "
                 "  print \"\\n═══════════════════════════════════════════════════════════════════\"; "
                 "  for(i=1; i<=NF; i++) { "
                 "    if ($i ~ /<title>/) { gsub(/<[^>]*>/, \"\", $i); gsub(/^[ \\t]+|[ \\t]+$/, \"\", $i); if($i) print \"\\n■ \" $i \" \\n\" } "
                 "    if ($i ~ /<link>/) { gsub(/<[^>]*>/, \"\", $i); gsub(/^[ \\t]+|[ \\t]+$/, \"\", $i); if($i) print \"🔗 \" $i } "
                 "    if ($i ~ /<pubDate>/) { gsub(/<[^>]*>/, \"\", $i); gsub(/^[ \\t]+|[ \\t]+$/, \"\", $i); if($i) print \"📅 \" $i } "
                 "    if ($i ~ /<description>/) { gsub(/<[^>]*>/, \"\", $i); gsub(/^[ \\t]+|[ \\t]+$/, \"\", $i); if($i) print \"\\n\" $i \"\\n\" } "
                 "  } "
                 "}'",
                 url);
    } else {
        // Fallback without xmllint - just basic formatting
        snprintf(cmd, sizeof(cmd),
                 "curl -sL '%s' 2>&1 | "
                 "sed 's/></>\\\n</g' | "
                 "grep -E '(title>|link>|pubDate>|description>)' | "
                 "sed 's/<title>/\\n=== /g; s/<\\/title>/ ===/g; "
                 "s/<link>/Link: /g; s/<\\/link>//g; "
                 "s/<pubDate>/Date: /g; s/<\\/pubDate>//g; "
                 "s/<description>//g; s/<\\/description>/\\n/g'",
                 url);
    }

    // Create a label for the buffer
    char label[256];
    snprintf(label, sizeof(label), "[RSS: %s]", url);
    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    // Execute the command and capture output
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}
// --- wget support -----------------------------------------------------------
static int is_pdf_url(const char *url) {
    if (!url) return 0;
    int len = strlen(url);
    if (len < 4) return 0;
    
    // Check for .pdf extension (case insensitive)
    const char *ext = url + len - 4;
    if (strcasecmp(ext, ".pdf") == 0) return 1;
    
    // Also check if there's .pdf followed by query params
    if (strstr(url, ".pdf?") || strstr(url, ".PDF?")) return 1;
    
    return 0;
}

static int is_pdf_file(const char *filepath) {
    if (!filepath) return 0;
    const char *ext = strrchr(filepath, '.');
    if (!ext) return 0;
    return (strcasecmp(ext, ".pdf") == 0);
}

// ============================================================================
// Modified load_wget_response() - handles PDF URLs
// ============================================================================

int load_wget_response(Buffer *buf, const char *url) {
    // Free existing buffer content
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }

    buf->line_count = 0;
    buf->scroll_offset = 0;
    buf->is_active = 1;
    buf->is_http_buffer = 1;
    buf->lang = LANG_NONE;

    // Store the URL for reload functionality
    strncpy(buf->http_request, url, sizeof(buf->http_request) - 1);
    buf->http_request[sizeof(buf->http_request) - 1] = '\0';

    // Build wget command with PDF support
    char cmd[2048];
    
    // Check if URL points to a PDF
    int have_pdftotext = cmd_exists("pdftotext");
    if (is_pdf_url(url) && have_pdftotext) {
        // Download PDF and convert to text
        snprintf(cmd, sizeof(cmd), 
                 "wget -qO- '%s' 2>/dev/null | pdftotext -layout - - 2>&1 || "
                 "echo 'Failed to fetch or convert PDF'", 
                 url);
    } else if (is_pdf_url(url) && !have_pdftotext) {
        // Can't convert, show error
        snprintf(cmd, sizeof(cmd), 
                 "echo 'Error: PDF detected but pdftotext not found. Install poppler-utils.'");
    } else {
        // Regular wget
        snprintf(cmd, sizeof(cmd), "wget -qO- '%s' 2>&1", url);
    }

    // Create a label for the buffer
    char label[256];
    if (is_pdf_url(url)) {
        snprintf(label, sizeof(label), "[wget-PDF: %s]", url);
    } else {
        snprintf(label, sizeof(label), "[wget: %s]", url);
    }
    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    // Execute the command and capture output
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}

// ============================================================================
// Modified load_w3m_response() - handles PDF URLs
// ============================================================================

int load_w3m_response(Buffer *buf, const char *url) {
    // Free existing buffer content
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }

    buf->line_count = 0;
    buf->scroll_offset = 0;
    buf->is_active = 1;
    buf->is_http_buffer = 1;
    buf->lang = LANG_NONE;

    // Store the URL for reload functionality
    strncpy(buf->http_request, url, sizeof(buf->http_request) - 1);
    buf->http_request[sizeof(buf->http_request) - 1] = '\0';

    // Build w3m command with PDF support
    char cmd[2048];
    
    // Check if URL points to a PDF
    int have_pdftotext = cmd_exists("pdftotext");
    if (is_pdf_url(url) && have_pdftotext) {
        // Download PDF and convert to text (using wget since w3m won't help with binary)
        snprintf(cmd, sizeof(cmd), 
                 "wget -qO- '%s' 2>/dev/null | pdftotext -layout - - 2>&1 || "
                 "echo 'Failed to fetch or convert PDF'", 
                 url);
    } else if (is_pdf_url(url) && !have_pdftotext) {
        // Can't convert, show error
        snprintf(cmd, sizeof(cmd), 
                 "echo 'Error: PDF detected but pdftotext not found. Install poppler-utils.'");
    } else {
        // Regular w3m
        snprintf(cmd, sizeof(cmd), "w3m -dump '%s' 2>&1", url);
    }

    // Create a label for the buffer
    char label[256];
    if (is_pdf_url(url)) {
        snprintf(label, sizeof(label), "[w3m-PDF: %s]", url);
    } else {
        snprintf(label, sizeof(label), "[w3m: %s]", url);
    }
    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    // Execute the command and capture output
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}
// --- SQL Database Support ---------------------------------------------------

int load_sql_response(Buffer *buf, const char *db_type, const char *connection, const char *query) {
    // Free existing buffer content
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }

    buf->line_count = 0;
    buf->scroll_offset = 0;
    buf->is_active = 1;
    buf->is_http_buffer = 0;
    buf->lang = LANG_NONE;

    char cmd[4096];
    int have_vd = cmd_exists("vd");

    if (strcmp(db_type, "sqlite") == 0) {
        if (have_vd) {
            // VisiData with nice table formatting
            // Create temp file for query
            char tmpfile[] = "/tmp/peek_sql_XXXXXX";
            int fd = mkstemp(tmpfile);
            if (fd != -1) {
                FILE *f = fdopen(fd, "w");
                if (f) {
                    fprintf(f, "%s", query);
                    fclose(f);

                    // Use vd in batch mode with txt output format
                    snprintf(cmd, sizeof(cmd),
                             "vd --batch --output-encoding=utf-8 -f txt "
                             "'sqlite:///%s' --exec ':source %s' 2>&1 || "
                             "sqlite3 -header -box '%s' \"%s\" 2>&1",
                             connection, tmpfile, connection, query);
                    unlink(tmpfile);
                } else {
                    close(fd);
                    unlink(tmpfile);
                    // Fallback to sqlite3
                    snprintf(cmd, sizeof(cmd),
                             "sqlite3 -header -box '%s' \"%s\" 2>&1",
                             connection, query);
                }
            } else {
                // Fallback to sqlite3
                snprintf(cmd, sizeof(cmd),
                         "sqlite3 -header -box '%s' \"%s\" 2>&1",
                         connection, query);
            }
        } else {
            // Fallback: use sqlite3 with box borders
            snprintf(cmd, sizeof(cmd),
                     "sqlite3 -header -box '%s' \"%s\" 2>&1",
                     connection, query);
        }
    } else if (strcmp(db_type, "postgres") == 0 || strcmp(db_type, "postgresql") == 0) {
        if (have_vd) {
            // VisiData with PostgreSQL
            char tmpfile[] = "/tmp/peek_sql_XXXXXX";
            int fd = mkstemp(tmpfile);
            if (fd != -1) {
                FILE *f = fdopen(fd, "w");
                if (f) {
                    fprintf(f, "%s", query);
                    fclose(f);

                    snprintf(cmd, sizeof(cmd),
                             "vd --batch --output-encoding=utf-8 -f txt "
                             "'%s' --exec ':source %s' 2>&1 || "
                             "psql '%s' --pset=border=2 -c \"%s\" 2>&1",
                             connection, tmpfile, connection, query);
                    unlink(tmpfile);
                } else {
                    close(fd);
                    unlink(tmpfile);
                    // Fallback to psql
                    snprintf(cmd, sizeof(cmd),
                             "psql '%s' --pset=border=2 -c \"%s\" 2>&1",
                             connection, query);
                }
            } else {
                // Fallback to psql
                snprintf(cmd, sizeof(cmd),
                         "psql '%s' --pset=border=2 -c \"%s\" 2>&1",
                         connection, query);
            }
        } else {
            // Fallback: use psql with border=2
            snprintf(cmd, sizeof(cmd),
                     "psql '%s' --pset=border=2 -c \"%s\" 2>&1",
                     connection, query);
        }
    } else {
        return -1;
    }

    // Create a label for the buffer
    char label[256];
    snprintf(label, sizeof(label), "[SQL:%s]", db_type);
    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    // Store query for potential reload
    strncpy(buf->http_request, query, sizeof(buf->http_request) - 1);
    buf->http_request[sizeof(buf->http_request) - 1] = '\0';

    // Execute the command and capture output
    FILE *p = popen(cmd, "r");
    if (!p) return -1;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        strip_overstrikes(line);
        strip_ansi(line);
        rtrim(line);

        buf->lines[buf->line_count++] = strdup(line);
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}

void prompt_sql_query(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    // Create popup window
    int popup_height = 24;
    int popup_width = 70;
    int start_y = (max_y - popup_height) / 2;
    int start_x = (max_x - popup_width) / 2;

    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) return;

    box(popup, 0, 0);

    // Title
    wattron(popup, A_BOLD);
    mvwprintw(popup, 0, 2, " SQL Query ");
    wattroff(popup, A_BOLD);

    // Help text
    mvwprintw(popup, 2, 2, "Database Type (1=SQLite, 2=PostgreSQL):");
    mvwprintw(popup, 4, 2, "Connection:");
    mvwprintw(popup, 5, 4, "SQLite: /path/to/database.db");
    mvwprintw(popup, 6, 4, "PostgreSQL: postgresql://user:pass@host/db");
    mvwprintw(popup, 8, 2, "SQL Query (Ctrl+E to execute, ESC to cancel):");

    curs_set(1);
    keypad(popup, TRUE);

    // Get database type
    mvwprintw(popup, 2, 44, ">");
    wrefresh(popup);

    int db_choice = wgetch(popup);
    char db_type[32] = "sqlite";

    if (db_choice == '2') {
        strcpy(db_type, "postgres");
        mvwprintw(popup, 2, 46, "PostgreSQL");
    } else {
        mvwprintw(popup, 2, 46, "SQLite");
    }
    wrefresh(popup);

    // Get connection string
    mvwprintw(popup, 4, 14, ">");
    wmove(popup, 4, 16);
    wrefresh(popup);

    char connection[512] = {0};
    int conn_pos = 0;

    while (1) {
        int ch = wgetch(popup);
        if (ch == '\n' || ch == KEY_ENTER) break;
        else if (ch == 27) { // ESC
            curs_set(0);
            delwin(popup);
            touchwin(stdscr);
            refresh();
            return;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (conn_pos > 0) {
                conn_pos--;
                connection[conn_pos] = '\0';
                mvwhline(popup, 4, 16, ' ', popup_width - 18);
                mvwprintw(popup, 4, 16, "%s", connection);
                wmove(popup, 4, 16 + conn_pos);
                wrefresh(popup);
            }
        } else if (isprint(ch) && conn_pos < 500) {
            connection[conn_pos++] = ch;
            connection[conn_pos] = '\0';
            mvwprintw(popup, 4, 16, "%s", connection);
            wrefresh(popup);
        }
    }

    // Get SQL query (multi-line)
    #define MAX_SQL_LINES 12
    char lines[MAX_SQL_LINES][256];
    for (int i = 0; i < MAX_SQL_LINES; i++) {
        lines[i][0] = '\0';
    }

    int current_line = 0;
    int pos = 0;
    int scroll_offset = 0;
    int visible_lines = 12;

    // Redraw query area
    for (int i = 0; i < visible_lines; i++) {
        int y = 10 + i;
        mvwhline(popup, y, 2, ' ', popup_width - 3);
        mvwprintw(popup, y, 2, ">");
    }
    wmove(popup, 10, 4);
    wrefresh(popup);

    while (1) {
        int ch = wgetch(popup);

        if (ch == 5) { // Ctrl+E
            break;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (current_line < MAX_SQL_LINES - 1) {
                current_line++;
                pos = strlen(lines[current_line]);
                if (current_line - scroll_offset >= visible_lines) {
                    scroll_offset++;
                }

                // Redraw
                for (int i = 0; i < visible_lines; i++) {
                    int line_idx = scroll_offset + i;
                    int y = 10 + i;
                    mvwhline(popup, y, 2, ' ', popup_width - 3);
                    if (line_idx < MAX_SQL_LINES) {
                        mvwprintw(popup, y, 2, ">");
                        if (lines[line_idx][0] != '\0') {
                            mvwprintw(popup, y, 4, "%s", lines[line_idx]);
                        }
                    }
                }
                wmove(popup, 10 + (current_line - scroll_offset), 4 + pos);
                wrefresh(popup);
            }
        } else if (ch == 27) { // ESC
            curs_set(0);
            delwin(popup);
            touchwin(stdscr);
            refresh();
            return;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                lines[current_line][pos] = '\0';
            } else if (current_line > 0) {
                current_line--;
                pos = strlen(lines[current_line]);
                if (current_line < scroll_offset) scroll_offset--;
            }

            // Redraw
            for (int i = 0; i < visible_lines; i++) {
                int line_idx = scroll_offset + i;
                int y = 10 + i;
                mvwhline(popup, y, 2, ' ', popup_width - 3);
                if (line_idx < MAX_SQL_LINES) {
                    mvwprintw(popup, y, 2, ">");
                    if (lines[line_idx][0] != '\0') {
                        mvwprintw(popup, y, 4, "%s", lines[line_idx]);
                    }
                }
            }
            wmove(popup, 10 + (current_line - scroll_offset), 4 + pos);
            wrefresh(popup);
        } else if (isprint(ch) && pos < 250) {
            lines[current_line][pos++] = ch;
            lines[current_line][pos] = '\0';
            mvwprintw(popup, 10 + (current_line - scroll_offset), 4, "%s", lines[current_line]);
            wrefresh(popup);
        }
    }

    curs_set(0);
    delwin(popup);
    touchwin(stdscr);
    refresh();

    // Combine all lines into one query
    char query[2048] = {0};
    for (int i = 0; i < MAX_SQL_LINES; i++) {
        char *line = lines[i];
        while (*line && isspace((unsigned char)*line)) line++;
        if (*line == '\0') continue;

        int len = strlen(line);
        while (len > 0 && isspace((unsigned char)line[len-1])) line[--len] = '\0';

        if (query[0] != '\0') strncat(query, " ", sizeof(query) - strlen(query) - 1);
        strncat(query, line, sizeof(query) - strlen(query) - 1);
    }

    if (query[0] == '\0' || connection[0] == '\0') return;

    // Execute SQL query
    if (state->buffer_count < MAX_BUFFERS) {
        if (load_sql_response(&state->buffers[state->buffer_count], db_type, connection, query) == 0) {
            state->current_buffer = state->buffer_count;
            state->buffer_count++;
        } else {
            attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
            mvhline(max_y - 2, 0, ' ', max_x);
            mvprintw(max_y - 2, 1, "Failed to execute SQL query");
            attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
            refresh();
            napms(1500);
        }
    }
}

// --- URL input popup --------------------------------------------------------

void prompt_url(ViewerState *state, const char *tool_name,
                int (*loader_func)(Buffer*, const char*)) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    // Create popup window
    int popup_height = 10;
    int popup_width = 70;
    int start_y = (max_y - popup_height) / 2;
    int start_x = (max_x - popup_width) / 2;

    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) return;

    box(popup, 0, 0);

    // Title
    wattron(popup, A_BOLD);
    char title[64];
    snprintf(title, sizeof(title), " %s URL Fetch ", tool_name);
    mvwprintw(popup, 0, 2, "%s", title);
    wattroff(popup, A_BOLD);

    // Help text
    mvwprintw(popup, 2, 2, "Examples:");
    mvwprintw(popup, 3, 4, "https://example.com");
    mvwprintw(popup, 4, 4, "http://httpbin.org/get");

    mvwprintw(popup, 6, 2, "Enter URL (Enter to fetch, ESC to cancel):");
    mvwprintw(popup, 8, 2, ">");

    curs_set(1);
    keypad(popup, TRUE);

    char input[512] = {0};
    int pos = 0;

    wmove(popup, 8, 4);
    wrefresh(popup);

    while (1) {
        int ch = wgetch(popup);

        if (ch == '\n' || ch == KEY_ENTER) {
            break; // Execute
        } else if (ch == 27) { // ESC
            curs_set(0);
            delwin(popup);
            touchwin(stdscr);
            refresh();
            return;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                input[pos] = '\0';
                mvwhline(popup, 8, 4, ' ', popup_width - 6);
                mvwprintw(popup, 8, 4, "%s", input);
                wmove(popup, 8, 4 + pos);
                wrefresh(popup);
            }
        } else if (isprint(ch) && pos < 500) {
            input[pos++] = ch;
            input[pos] = '\0';
            mvwprintw(popup, 8, 4, "%s", input);
            wrefresh(popup);
        }
    }

    curs_set(0);
    delwin(popup);
    touchwin(stdscr);
    refresh();

    // Trim whitespace
    char *url = input;
    while (*url && isspace((unsigned char)*url)) url++;
    if (*url == '\0') return;

    int len = strlen(url);
    while (len > 0 && isspace((unsigned char)url[len-1])) url[--len] = '\0';

    // Create new buffer for response
    if (state->buffer_count < MAX_BUFFERS) {
        if (loader_func(&state->buffers[state->buffer_count], url) == 0) {
            state->current_buffer = state->buffer_count;
            state->buffer_count++;
        } else {
            // Show error message briefly
            attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
            mvhline(max_y - 2, 0, ' ', max_x);
            mvprintw(max_y - 2, 1, "Failed to fetch URL with %s", tool_name);
            attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
            refresh();
            napms(1500);
        }
    } else {
        // Buffer limit reached
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        mvhline(max_y - 2, 0, ' ', max_x);
        mvprintw(max_y - 2, 1, "Maximum buffer limit reached");
        attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        refresh();
        napms(1500);
    }
}

// Helper function for HTTP request popup to redraw visible lines
static void http_popup_redraw_lines(WINDOW *popup, char lines[][256], int max_lines,
                                     int current_line, int pos, int scroll_offset,
                                     int visible_lines, int popup_width) {
    for (int i = 0; i < visible_lines; i++) {
        int line_idx = scroll_offset + i;
        int y = 9 + i;

        // Clear line
        mvwhline(popup, y, 2, ' ', popup_width - 3);

        if (line_idx < max_lines) {
            mvwprintw(popup, y, 2, ">");
            if (lines[line_idx][0] != '\0') {
                mvwprintw(popup, y, 4, "%s", lines[line_idx]);
            }
        }
    }

    // Show cursor on current line if visible
    int cursor_y = 9 + (current_line - scroll_offset);
    if (cursor_y >= 9 && cursor_y < 9 + visible_lines) {
        wmove(popup, cursor_y, 4 + pos);
    }

    wrefresh(popup);
}

void prompt_http_request(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    // Create popup window - bigger to accommodate scrolling
    int popup_height = 20;
    int popup_width = 70;
    int start_y = (max_y - popup_height) / 2;
    int start_x = (max_x - popup_width) / 2;

    WINDOW *popup = newwin(popup_height, popup_width, start_y, start_x);
    if (!popup) return;

    box(popup, 0, 0);

    // Title
    wattron(popup, A_BOLD);
    mvwprintw(popup, 0, 2, " HTTP Request ");
    wattroff(popup, A_BOLD);

    // Help text
    mvwprintw(popup, 2, 2, "Examples:");
    mvwprintw(popup, 3, 4, "GET httpbin.org/get");
    mvwprintw(popup, 4, 4, "POST httpbin.org/post name=John age:=30");
    mvwprintw(popup, 5, 4, "GET api.example.com Auth:\"Bearer token\"");

    mvwprintw(popup, 7, 2, "Enter request (Ctrl+E to execute, ESC to cancel):");

    // More input lines for scrolling
    #define MAX_REQUEST_LINES 10
    char lines[MAX_REQUEST_LINES][256];
    for (int i = 0; i < MAX_REQUEST_LINES; i++) {
        lines[i][0] = '\0';
    }

    int current_line = 0;
    int pos = 0;
    int scroll_offset = 0;
    int visible_lines = 9; // Lines 9-17 in the popup (9 visible lines)

    // Don't enable echo - we'll manually display characters
    curs_set(1);
    keypad(popup, TRUE);

    http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                             scroll_offset, visible_lines, popup_width);

    while (1) {
        int ch = wgetch(popup);

        if (ch == 5) { // Ctrl+E
            break; // Execute
        } else if (ch == '\n' || ch == KEY_ENTER) {
            // Move to next line
            if (current_line < MAX_REQUEST_LINES - 1) {
                current_line++;
                pos = strlen(lines[current_line]);

                // Scroll if needed
                if (current_line - scroll_offset >= visible_lines) {
                    scroll_offset++;
                }

                http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                                         scroll_offset, visible_lines, popup_width);
            }
        } else if (ch == 27) { // ESC
            curs_set(0);
            delwin(popup);
            touchwin(stdscr);
            refresh();
            return;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                lines[current_line][pos] = '\0';
                http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                                         scroll_offset, visible_lines, popup_width);
            } else if (current_line > 0) {
                // Move to previous line
                current_line--;
                pos = strlen(lines[current_line]);

                // Scroll if needed
                if (current_line < scroll_offset) {
                    scroll_offset--;
                }

                http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                                         scroll_offset, visible_lines, popup_width);
            }
        } else if (ch == KEY_UP) {
            if (current_line > 0) {
                current_line--;
                pos = strlen(lines[current_line]);

                // Scroll if needed
                if (current_line < scroll_offset) {
                    scroll_offset--;
                }

                http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                                         scroll_offset, visible_lines, popup_width);
            }
        } else if (ch == KEY_DOWN) {
            if (current_line < MAX_REQUEST_LINES - 1) {
                current_line++;
                pos = strlen(lines[current_line]);

                // Scroll if needed
                if (current_line - scroll_offset >= visible_lines) {
                    scroll_offset++;
                }

                http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                                         scroll_offset, visible_lines, popup_width);
            }
        } else if (isprint(ch) && pos < 250) {
            lines[current_line][pos++] = ch;
            lines[current_line][pos] = '\0';
            http_popup_redraw_lines(popup, lines, MAX_REQUEST_LINES, current_line, pos,
                                     scroll_offset, visible_lines, popup_width);
        }
    }

    curs_set(0);
    delwin(popup);
    touchwin(stdscr);
    refresh();

    // Combine all non-empty lines into one request
    char input[2048] = {0};
    for (int i = 0; i < MAX_REQUEST_LINES; i++) {
        // Trim whitespace
        char *line = lines[i];
        while (*line && isspace((unsigned char)*line)) line++;
        if (*line == '\0') continue;

        int len = strlen(line);
        while (len > 0 && isspace((unsigned char)line[len-1])) line[--len] = '\0';

        if (input[0] != '\0') strncat(input, " ", sizeof(input) - strlen(input) - 1);
        strncat(input, line, sizeof(input) - strlen(input) - 1);
    }

    // Trim final whitespace
    int len = (int)strlen(input);
    while (len > 0 && isspace((unsigned char)input[len - 1])) input[--len] = '\0';

    if (input[0] == '\0') return;

    // Create new buffer for HTTP response
    if (state->buffer_count < MAX_BUFFERS) {
        if (load_http_response(&state->buffers[state->buffer_count], input) == 0) {
            state->current_buffer = state->buffer_count;
            state->buffer_count++;
        } else {
            // Show error message briefly
            attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
            mvhline(max_y - 2, 0, ' ', max_x);
            mvprintw(max_y - 2, 1, "Failed to execute HTTP request");
            attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
            refresh();
            napms(1500);
        }
    } else {
        // Buffer limit reached
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        mvhline(max_y - 2, 0, ' ', max_x);
        mvprintw(max_y - 2, 1, "Maximum buffer limit reached");
        attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        refresh();
        napms(1500);
    }
}

void reload_http_buffer(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    Buffer *buf = &state->buffers[state->current_buffer];

    if (!buf->is_http_buffer || buf->http_request[0] == '\0') {
        // Not an HTTP buffer, show message
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        mvhline(max_y - 2, 0, ' ', max_x);
        mvprintw(max_y - 2, 1, "Current buffer is not an HTTP response");
        attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        refresh();
        napms(1500);
        return;
    }

    // Save the request string before reloading
    char saved_request[512];
    strncpy(saved_request, buf->http_request, sizeof(saved_request) - 1);
    saved_request[sizeof(saved_request) - 1] = '\0';

    // Show loading message
    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    mvhline(max_y - 2, 0, ' ', max_x);
    mvprintw(max_y - 2, 1, "Reloading HTTP request...");
    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    refresh();

    // Determine which loader to use based on the buffer label
    int result = -1;
    if (strstr(buf->filepath, "[wget:") != NULL) {
        result = load_wget_response(buf, saved_request);
    } else if (strstr(buf->filepath, "[w3m:") != NULL) {
        result = load_w3m_response(buf, saved_request);
    } else if (strstr(buf->filepath, "[RSS:") != NULL) {  // ADD THIS
        result = load_rss_feed(buf, saved_request);       // ADD THIS
    }  else {
        result = load_http_response(buf, saved_request);
    }

    if (result != 0) {
        // Show error message
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        mvhline(max_y - 2, 0, ' ', max_x);
        mvprintw(max_y - 2, 1, "Failed to reload HTTP request");
        attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        refresh();
        napms(1500);
    }
}

void close_current_buffer(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    if (state->buffer_count <= 1) {
        // Can't close the last buffer
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        mvhline(max_y - 2, 0, ' ', max_x);
        mvprintw(max_y - 2, 1, "Cannot close the last buffer");
        attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        refresh();
        napms(1500);
        return;
    }

    int current = state->current_buffer;

    // Free the buffer
    free_buffer(&state->buffers[current]);

    // Shift all buffers after this one down
    for (int i = current; i < state->buffer_count - 1; i++) {
        state->buffers[i] = state->buffers[i + 1];
    }

    state->buffer_count--;

    // Adjust current buffer index
    if (state->current_buffer >= state->buffer_count) {
        state->current_buffer = state->buffer_count - 1;
    }
}

// --- Search -----------------------------------------------------------------

int search_buffer(ViewerState *state, const char *term, int start_line, int direction) {
    Buffer *buf = &state->buffers[state->current_buffer];
    if (term[0] == '\0') return -1;

    int line = start_line;
    for (int i = 0; i < buf->line_count; i++) {
        if (line < 0) line = buf->line_count - 1;
        if (line >= buf->line_count) line = 0;

        if (strstr(buf->lines[line], term)) return line;
        line += direction;
    }
    return -1;
}

void find_all_matches(ViewerState *state) {
    Buffer *buf = &state->buffers[state->current_buffer];
    state->search_match_count = 0;
    if (state->search_term[0] == '\0') return;

    for (int i = 0; i < buf->line_count; i++)
        if (strstr(buf->lines[i], state->search_term)) state->search_match_count++;
}

void prompt_search(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    mvhline(max_y - 2, 0, ' ', max_x);
    mvprintw(max_y - 2, 1, "Search: ");
    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    move(max_y - 2, 9);
    refresh();

    echo();
    curs_set(1);

    char input[256] = {0};
    getnstr(input, sizeof(input) - 1);

    noecho();
    curs_set(0);

    int len = (int)strlen(input);
    while (len > 0 && isspace((unsigned char)input[len - 1])) input[--len] = '\0';

    if (input[0] != '\0') {
        strncpy(state->search_term, input, sizeof(state->search_term) - 1);
        state->search_term[sizeof(state->search_term) - 1] = '\0';
        find_all_matches(state);

        int match = search_buffer(state, state->search_term, 0, 1);
        if (match >= 0) {
            state->buffers[state->current_buffer].scroll_offset = match;

            int count = 0;
            for (int i = 0; i < match; i++)
                if (strstr(state->buffers[state->current_buffer].lines[i], state->search_term)) count++;

            state->current_match = count;
        }
    }
}

void next_match(ViewerState *state) {
    if (state->search_term[0] == '\0') return;
    Buffer *buf = &state->buffers[state->current_buffer];
    int match = search_buffer(state, state->search_term, buf->scroll_offset + 1, 1);
    if (match >= 0) {
        buf->scroll_offset = match;
        int count = 0;
        for (int i = 0; i < match; i++) if (strstr(buf->lines[i], state->search_term)) count++;
        state->current_match = count;
    }
}

void prev_match(ViewerState *state) {
    if (state->search_term[0] == '\0') return;
    Buffer *buf = &state->buffers[state->current_buffer];
    int match = search_buffer(state, state->search_term, buf->scroll_offset - 1, -1);
    if (match >= 0) {
        buf->scroll_offset = match;
        int count = 0;
        for (int i = 0; i < match; i++) if (strstr(buf->lines[i], state->search_term)) count++;
        state->current_match = count;
    }
}

// --- Copy -------------------------------------------------------------------

void copy_selection_to_clipboard(ViewerState *state) {
    Buffer *buf = &state->buffers[state->current_buffer];

    int start = state->copy_start_line;
    int end = state->copy_end_line;
    if (start > end) { int t = start; start = end; end = t; }

    // Best-effort clipboard: xclip OR pbcopy. If neither exists, we silently do nothing.
    FILE *pipe = popen("xclip -selection clipboard 2>/dev/null || pbcopy 2>/dev/null", "w");
    if (!pipe) return;

    for (int i = start; i <= end && i < buf->line_count; i++)
        fprintf(pipe, "%s\n", buf->lines[i]);

    pclose(pipe);
}

// --- UI ---------------------------------------------------------------------

void draw_tabbar(ViewerState *state) {
    int max_x = getmaxx(stdscr);

    // Don't fill the line - let it be transparent
    // Just move to the line and draw content
    move(0, 0);
    clrtoeol();  // Clear to end of line without filling

    attron(COLOR_PAIR(COLOR_TABBAR));
    int x = 1;
    for (int i = 0; i < state->buffer_count; i++) {
        if (!state->buffers[i].is_active) continue;

        const char *name = strrchr(state->buffers[i].filepath, '/');
        if (!name) name = state->buffers[i].filepath;
        else name++;

        if (i == state->current_buffer) attron(A_REVERSE | A_BOLD);

        mvprintw(0, x, " %s ", name);
        x += (int)strlen(name) + 2;

        if (i == state->current_buffer) attroff(A_REVERSE | A_BOLD);

        if (x < max_x - 1) mvaddch(0, x++, '|');
        if (x >= max_x - 12) break;
    }

    mvprintw(0, max_x - 10, " [%d/%d] ", state->current_buffer + 1, state->buffer_count);
    attroff(COLOR_PAIR(COLOR_TABBAR));

    // Draw white horizontal line below tabbar
    attron(COLOR_PAIR(COLOR_NORMAL));
    mvhline(1, 0, ACS_HLINE, max_x);
    attroff(COLOR_PAIR(COLOR_NORMAL));
}

void draw_status_bar(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    Buffer *buf = &state->buffers[state->current_buffer];

    // Draw white horizontal line above status bar
    attron(COLOR_PAIR(COLOR_NORMAL));
    mvhline(max_y - 2, 0, ACS_HLINE, max_x);
    attroff(COLOR_PAIR(COLOR_NORMAL));

    // Clear the status line without filling
    move(max_y - 1, 0);
    clrtoeol();

    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);

    const char *name = strrchr(buf->filepath, '/');
    if (!name) name = buf->filepath;
    else name++;

    int percent = buf->line_count > 0 ? (buf->scroll_offset * 100) / buf->line_count : 0;
    const char *mode = state->copy_mode ? "VISUAL" : "NORMAL";

    char left[512];
    // ADD horizontal scroll position to status (HScroll:X)
    if (state->wrap_enabled) {
        snprintf(left, sizeof(left), "NBL Peek | %s | %s | %d%% | %d/%d lines | L:%s W:%s%s",
                 mode, name, percent,
                 buf->scroll_offset + 1, buf->line_count,
                 state->show_line_numbers ? "ON" : "OFF",
                 state->wrap_enabled ? "ON" : "OFF",
                 buf->is_http_buffer ? " | HTTP" : "");
    } else {
        snprintf(left, sizeof(left), "NBL Peek | %s | %s | %d%% | %d/%d lines | L:%s W:%s | HScroll:%d%s",
                 mode, name, percent,
                 buf->scroll_offset + 1, buf->line_count,
                 state->show_line_numbers ? "ON" : "OFF",
                 state->wrap_enabled ? "ON" : "OFF",
                 state->horiz_scroll_offset,
                 buf->is_http_buffer ? " | HTTP" : "");
    }

    mvprintw(max_y - 1, 1, "%s", left);

    if (state->search_term[0] != '\0') {
        char right[256];
        snprintf(right, sizeof(right), "Search: \"%s\" [%d/%d] ",
                 state->search_term,
                 state->current_match + 1,
                 state->search_match_count);
        mvprintw(max_y - 1, max_x - (int)strlen(right) - 1, "%s", right);
    }

    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
}

void draw_buffer(ViewerState *state) {
    Buffer *buf = &state->buffers[state->current_buffer];
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    int content_start_y = 2;
    int content_height = max_y - 4;
    int line_nr_width = state->show_line_numbers ? 6 : 0;

    if (state->wrap_enabled) {
        // WRAPPING MODE - ignore horizontal scroll, reset it
        state->horiz_scroll_offset = 0;
        
        int y = content_start_y;
        int logical_line = buf->scroll_offset;

        while (y < content_start_y + content_height && logical_line < buf->line_count) {
            mvhline(y, 0, ' ', max_x);

            int text_width = max_x - line_nr_width - 1;
            if (text_width <= 0) text_width = max_x;

            WrappedLine wl = wrap_line(buf->lines[logical_line], text_width);

            for (int seg = 0; seg < wl.count && y < content_start_y + content_height; seg++) {
                if (state->show_line_numbers && seg == 0) {
                    attron(COLOR_PAIR(COLOR_LINENR));
                    mvprintw(y, 1, "%4d ", logical_line + 1);
                    attroff(COLOR_PAIR(COLOR_LINENR));
                } else if (state->show_line_numbers) {
                    mvprintw(y, 1, "     ");
                }

                int a = state->copy_start_line;
                int b = state->copy_end_line;
                int lo = (a < b) ? a : b;
                int hi = (a > b) ? a : b;

                int in_selection = state->copy_mode && logical_line >= lo && logical_line <= hi;

                if (in_selection) attron(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);
                highlight_line(wl.segments[seg], buf->lang, y, line_nr_width + 1, max_x);
                if (in_selection) attroff(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);

                y++;
            }

            free_wrapped_line(&wl);
            logical_line++;
        }

        while (y < content_start_y + content_height) mvhline(y++, 0, ' ', max_x);

    } else {
        // NO WRAPPING MODE - apply horizontal scrolling
        for (int i = 0; i < content_height; i++) {
            int line_idx = buf->scroll_offset + i;
            int y = content_start_y + i;

            mvhline(y, 0, ' ', max_x);
            if (line_idx >= buf->line_count) continue;

            if (state->show_line_numbers) {
                attron(COLOR_PAIR(COLOR_LINENR));
                mvprintw(y, 1, "%4d ", line_idx + 1);
                attroff(COLOR_PAIR(COLOR_LINENR));
            }

            int a = state->copy_start_line;
            int b = state->copy_end_line;
            int lo = (a < b) ? a : b;
            int hi = (a > b) ? a : b;

            int in_selection = state->copy_mode && line_idx >= lo && line_idx <= hi;

            // NEW: Apply horizontal scrolling
            const char *line = buf->lines[line_idx];
            int line_len = strlen(line);
            
            // Skip characters based on horizontal scroll offset
            int start_col = state->horiz_scroll_offset;
            if (start_col >= line_len) {
                // Scrolled past end of line, show nothing
                continue;
            }
            
            // Create a substring starting from the horizontal offset
            char visible_line[MAX_LINE_LEN];
            strncpy(visible_line, line + start_col, sizeof(visible_line) - 1);
            visible_line[sizeof(visible_line) - 1] = '\0';

            if (in_selection) attron(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);
            highlight_line(visible_line, buf->lang, y, line_nr_width + 1, max_x);
            if (in_selection) attroff(COLOR_PAIR(COLOR_COPY_SELECT) | A_REVERSE);
        }
    }
}
void draw_ui(ViewerState *state) {
    clear();
    draw_tabbar(state);
    draw_buffer(state);
    draw_status_bar(state);
    refresh();
}
static void cmd_show_help(void) {
    if (system("command -v fzf >/dev/null 2>&1") != 0) {
        int max_y = getmaxy(stdscr);
        int max_x = getmaxx(stdscr);
        attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        mvhline(max_y - 2, 0, ' ', max_x);
        mvprintw(max_y - 2, 1, "fzf is required for the help menu");
        attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
        refresh();
        napms(1500);
        return;
    }

    char help_template[] = "/tmp/peek_help_XXXXXX";
    int fd = mkstemp(help_template);
    if (fd < 0) return;

    FILE *help_file = fdopen(fd, "w");
    if (!help_file) {
        close(fd);
        unlink(help_template);
        return;
    }

    // Write help content organized by category
    fprintf(help_file, "=== NAVIGATION ===\n");
    fprintf(help_file, "j / DOWN        | Scroll down one line\n");
    fprintf(help_file, "k / UP          | Scroll up one line\n");
    fprintf(help_file, "d / Ctrl+D      | Half page down\n");
    fprintf(help_file, "u / Ctrl+U      | Half page up\n");
    fprintf(help_file, "g               | Jump to top\n");
    fprintf(help_file, "G               | Jump to bottom\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== SEARCH ===\n");
    fprintf(help_file, "/               | Search forward\n");
    fprintf(help_file, "n               | Next search match\n");
    fprintf(help_file, "N               | Previous search match\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== BUFFERS ===\n");
    fprintf(help_file, "Tab             | Next buffer\n");
    fprintf(help_file, "Shift+Tab       | Previous buffer\n");
    fprintf(help_file, "x               | Close current buffer\n");
    fprintf(help_file, "o               | Open file with fzf picker\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== HTTP & NETWORK ===\n");
    fprintf(help_file, "r               | Make HTTP request (xh)\n");
    fprintf(help_file, "R               | Reload current HTTP buffer\n");
    fprintf(help_file, "w               | Fetch URL with wget\n");
    fprintf(help_file, "W               | Fetch URL with w3m -dump\n");
    fprintf(help_file, "f               | Fetch RSS/Atom feed\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== SQL DATABASE ===\n");
    fprintf(help_file, "s               | Execute SQL query\n");
    fprintf(help_file, "                | (supports SQLite & PostgreSQL)\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== VISUAL/COPY MODE ===\n");
    fprintf(help_file, "v               | Enter visual/copy mode\n");
    fprintf(help_file, "j/k (in visual) | Extend selection\n");
    fprintf(help_file, "y (in visual)   | Copy selection to clipboard\n");
    fprintf(help_file, "ESC (in visual) | Exit visual mode\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== SETTINGS ===\n");
    fprintf(help_file, "l / L           | Toggle line numbers\n");
    fprintf(help_file, "t / T           | Toggle line wrapping\n");
    fprintf(help_file, "\n");

    fprintf(help_file, "=== OTHER ===\n");
    fprintf(help_file, "q               | Quit\n");
    fprintf(help_file, "?               | Show this help\n");

    fflush(help_file);
    fclose(help_file);

    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "fzf --height=100%% --layout=reverse --border "
        "--header='Peek Help - Search commands (ESC to close)' "
        "< \"%s\" > /dev/null 2> /dev/tty",
        help_template);

    endwin();
    system(cmd);
    refresh();
    clear();

    unlink(help_template);
}
// --- Input ------------------------------------------------------------------
// Complete handle_input() function with horizontal scrolling support

void handle_input(ViewerState *state, int *running) {
    int ch = getch();
    int max_y = getmaxy(stdscr);
    int visible_lines = max_y - 3;

    Buffer *buf = &state->buffers[state->current_buffer];

    switch (ch) {
        case '?':
            if (!state->copy_mode) cmd_show_help();
            break;

        case 's':
        case 'S':
            if (!state->copy_mode) prompt_sql_query(state);
            break;

        case 'q':
        case 'Q':
            if (!state->copy_mode) *running = 0;
            break;

        case 27: // ESC
            if (state->copy_mode) state->copy_mode = 0;
            break;

        case 'r':
            if (!state->copy_mode) prompt_http_request(state);
            break;

        case 'R':
            if (!state->copy_mode) reload_http_buffer(state);
            break;

        case 'f':
        case 'F':
            if (!state->copy_mode) prompt_url(state, "RSS", load_rss_feed);
            break;

        case 'w':
            if (!state->copy_mode) prompt_url(state, "wget", load_wget_response);
            break;

        case 'W':
            if (!state->copy_mode) prompt_url(state, "w3m", load_w3m_response);
            break;

        case 'x':
        case 'X':
            if (!state->copy_mode) close_current_buffer(state);
            break;

        case 'v':
            if (!state->copy_mode) {
                state->copy_mode = 1;
                state->copy_start_line = buf->scroll_offset;
                state->copy_end_line = buf->scroll_offset;
            }
            break;

        case 'y':
            if (state->copy_mode) {
                copy_selection_to_clipboard(state);
                state->copy_mode = 0;
            }
            break;

        // HORIZONTAL SCROLLING - only when wrap is OFF and not in copy mode
        case 'h':
        case KEY_LEFT:
            if (!state->wrap_enabled && !state->copy_mode) {
                // Scroll left
                state->horiz_scroll_offset -= state->horiz_scroll_step;
                if (state->horiz_scroll_offset < 0) {
                    state->horiz_scroll_offset = 0;
                }
            }
            break;

        case 'l':
        case KEY_RIGHT:
            if (!state->wrap_enabled && !state->copy_mode) {
                // Scroll right
                state->horiz_scroll_offset += state->horiz_scroll_step;
                // Note: no max limit, user can scroll as far as they want
            }
            break;

        case '0':  // Home - jump to column 0
            if (!state->wrap_enabled && !state->copy_mode) {
                state->horiz_scroll_offset = 0;
            }
            break;

        case '$':  // End - jump to see end of longest line
            if (!state->wrap_enabled && !state->copy_mode) {
                // Find longest line in visible area
                int max_len = 0;
                for (int i = buf->scroll_offset; i < buf->scroll_offset + visible_lines && i < buf->line_count; i++) {
                    int len = strlen(buf->lines[i]);
                    if (len > max_len) max_len = len;
                }
                int max_x = getmaxx(stdscr);
                int line_nr_width = state->show_line_numbers ? 6 : 0;
                int visible_width = max_x - line_nr_width - 1;

                // Scroll to show the end
                state->horiz_scroll_offset = max_len - visible_width;
                if (state->horiz_scroll_offset < 0) {
                    state->horiz_scroll_offset = 0;
                }
            }
            break;

        // LINE NUMBERS - Changed to uppercase L only (lowercase l is now right-scroll)
        case 'L':
            state->show_line_numbers = !state->show_line_numbers;
            break;

        // WRAP TOGGLE - Reset horizontal scroll when enabling wrap
        case 't':
        case 'T':
            state->wrap_enabled = !state->wrap_enabled;
            // When toggling wrap on, reset horizontal scroll
            if (state->wrap_enabled) {
                state->horiz_scroll_offset = 0;
            }
            break;

        case '/':
            if (!state->copy_mode) prompt_search(state);
            break;

        case 'n':
            if (!state->copy_mode) next_match(state);
            break;

        case 'N':
            if (!state->copy_mode) prev_match(state);
            break;

        case 'o':
        case 'O':
            if (!state->copy_mode) {
                endwin();

                char cwd[1024];
                if (!getcwd(cwd, sizeof(cwd))) break;

                char cmd[2048];
                snprintf(cmd, sizeof(cmd),
                         "find '%s' -type f 2>/dev/null | "
                         "fzf --prompt='Open File> ' --height=40%% --reverse",
                         cwd);

                FILE *p = popen(cmd, "r");
                if (p) {
                    char filepath[1024] = {0};
                    if (fgets(filepath, sizeof(filepath), p)) {
                        filepath[strcspn(filepath, "\r\n")] = '\0';
                        if (filepath[0] != '\0' && state->buffer_count < MAX_BUFFERS) {
                            if (load_file(&state->buffers[state->buffer_count], filepath) == 0) {
                                state->current_buffer = state->buffer_count;
                                state->buffer_count++;
                            }
                        }
                    }
                    pclose(p);
                }

                refresh();
                clear();
            }
            break;

        case 'j':
        case KEY_DOWN:
            if (buf->scroll_offset < buf->line_count - 1) {
                buf->scroll_offset++;
                if (state->copy_mode) state->copy_end_line = buf->scroll_offset;
            }
            break;

        case 'k':
        case KEY_UP:
            if (buf->scroll_offset > 0) {
                buf->scroll_offset--;
                if (state->copy_mode) state->copy_end_line = buf->scroll_offset;
            }
            break;

        case 'g':
            buf->scroll_offset = 0;
            if (state->copy_mode) state->copy_end_line = buf->scroll_offset;
            break;

        case 'G':
            buf->scroll_offset = buf->line_count - visible_lines;
            if (buf->scroll_offset < 0) buf->scroll_offset = 0;
            if (state->copy_mode) state->copy_end_line = buf->scroll_offset;
            break;

        case 'd':
        case 4: // Ctrl+D
            buf->scroll_offset += visible_lines / 2;
            if (buf->scroll_offset > buf->line_count - visible_lines)
                buf->scroll_offset = buf->line_count - visible_lines;
            if (buf->scroll_offset < 0) buf->scroll_offset = 0;
            if (state->copy_mode) state->copy_end_line = buf->scroll_offset;
            break;

        case 'u':
        case 21: // Ctrl+U
            buf->scroll_offset -= visible_lines / 2;
            if (buf->scroll_offset < 0) buf->scroll_offset = 0;
            if (state->copy_mode) state->copy_end_line = buf->scroll_offset;
            break;

        case '\t':
            if (!state->copy_mode && state->buffer_count > 1)
                state->current_buffer = (state->current_buffer + 1) % state->buffer_count;
            break;

        case KEY_BTAB:
            if (!state->copy_mode && state->buffer_count > 1) {
                state->current_buffer--;
                if (state->current_buffer < 0) state->current_buffer = state->buffer_count - 1;
            }
            break;
    }
}
// --- main -------------------------------------------------------------------

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    ViewerState *state = calloc(1, sizeof(ViewerState));
    if (!state) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 1;
    }

    // Default settings
    state->show_line_numbers = 1;
    state->wrap_enabled = 1;
    state->copy_mode = 0;
    state->horiz_scroll_offset = 0;
    state->horiz_scroll_step = 8; 

    int stdin_is_pipe = !isatty(STDIN_FILENO);
    int loaded_anything = 0;
    int arg_start = 1;

    // Parse flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-wrap") == 0) {
            state->wrap_enabled = 0;
            arg_start = i + 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            free(state);
            return 0;
        } else {
            break;
        }
    }

    int effective_argc = argc - (arg_start - 1);

    if (effective_argc < 2) {
        if (!stdin_is_pipe) {
            usage(argv[0]);
            free(state);
            return 1;
        }
        if (load_stdin(&state->buffers[state->buffer_count]) == 0) {
            state->buffer_count++;
            loaded_anything = 1;
        } else {
            fprintf(stderr, "No data on stdin\n");
            free(state);
            return 1;
        }
    } else {
        for (int i = arg_start; i < argc && state->buffer_count < MAX_BUFFERS; i++) {

            if (strcmp(argv[i], "-m") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "peek: -m requires a command string\n");
                    usage(argv[0]);
                    break;
                }

                const char *cmd = argv[++i];
                char label[1024];
                snprintf(label, sizeof(label), "[%s]", cmd);

                // If cmd is a man command, force plain output
                if (is_man_command_arg(cmd)) {
                    char cmd2[2048];
                    build_man_cmd_plain(cmd2, sizeof(cmd2), cmd);
                    if (load_command(&state->buffers[state->buffer_count], label, cmd2, LANG_MAN) == 0) {
                        state->buffer_count++;
                        loaded_anything = 1;
                    } else {
                        fprintf(stderr, "peek: failed to run command: %s\n", cmd);
                    }
                } else {
                    if (load_command(&state->buffers[state->buffer_count], label, cmd, LANG_NONE) == 0) {
                        state->buffer_count++;
                        loaded_anything = 1;
                    } else {
                        fprintf(stderr, "peek: failed to run command: %s\n", cmd);
                    }
                }
                continue;
            }

            if (strcmp(argv[i], "-") == 0) {
                if (load_stdin(&state->buffers[state->buffer_count]) == 0) {
                    state->buffer_count++;
                    loaded_anything = 1;
                } else {
                    fprintf(stderr, "Failed to read stdin\n");
                }
                continue;
            }

            if (is_man_command_arg(argv[i])) {
                const char *cmd = argv[i];
                char label[1024];
                snprintf(label, sizeof(label), "[%s]", cmd);

                char cmd2[2048];
                build_man_cmd_plain(cmd2, sizeof(cmd2), cmd);

                if (load_command(&state->buffers[state->buffer_count], label, cmd2, LANG_MAN) == 0) {
                    state->buffer_count++;
                    loaded_anything = 1;
                } else {
                    fprintf(stderr, "peek: failed to run man command: %s\n", cmd);
                }
                continue;
            }

            if (load_file(&state->buffers[state->buffer_count], argv[i]) == 0) {
                state->buffer_count++;
                loaded_anything = 1;
            } else {
                fprintf(stderr, "Failed to load %s\n", argv[i]);
            }
        }
    }

    if (!loaded_anything || state->buffer_count == 0) {
        fprintf(stderr, "Failed to load any files/stdin\n");
        free(state);
        return 1;
    }

    FILE *tty_in = NULL;
    SCREEN *screen = NULL;

    if (stdin_is_pipe) {
        tty_in = fopen("/dev/tty", "r");
        if (!tty_in) {
            fprintf(stderr, "Failed to open /dev/tty for input: %s\n", strerror(errno));
            free(state);
            return 1;
        }

        screen = newterm(NULL, stdout, tty_in);
        if (!screen) {
            fprintf(stderr, "newterm() failed\n");
            fclose(tty_in);
            free(state);
            return 1;
        }
        set_term(screen);
    } else {
        initscr();
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(COLOR_NORMAL,  COLOR_WHITE,  -1);
        init_pair(COLOR_KEYWORD, COLOR_MAGENTA,-1);
        init_pair(COLOR_STRING,  COLOR_GREEN,  -1);
        init_pair(COLOR_COMMENT, COLOR_CYAN,   -1);
        init_pair(COLOR_NUMBER,  COLOR_YELLOW, -1);
        init_pair(COLOR_TYPE,    COLOR_BLUE,   -1);
        init_pair(COLOR_FUNCTION,COLOR_YELLOW, -1);
        init_pair(COLOR_TABBAR,  COLOR_WHITE,  -1);
        init_pair(COLOR_STATUS,  COLOR_WHITE,  -1);
        init_pair(COLOR_LINENR,  COLOR_YELLOW, -1);
        init_pair(COLOR_COPY_SELECT, COLOR_WHITE, COLOR_BLUE);
    }

    int running = 1;
    while (running) {
        draw_ui(state);
        handle_input(state, &running);
    }

    for (int i = 0; i < state->buffer_count; i++) free_buffer(&state->buffers[i]);

    endwin();
    if (screen) delscreen(screen);
    if (tty_in) fclose(tty_in);

    free(state);
    return 0;
}