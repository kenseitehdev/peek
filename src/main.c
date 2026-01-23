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

#define MAX_BUFFERS 16
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
    LANG_MAN
} Language;

typedef struct {
    char *lines[MAX_LINES];
    int line_count;
    char filepath[1024];
    Language lang;
    int scroll_offset;
    int is_active;
} Buffer;

typedef struct {
    Buffer buffers[MAX_BUFFERS];
    int buffer_count;
    int current_buffer;
    char search_term[256];
    int search_line;
    int search_match_count;
    int current_match;
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

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <file1> [file2 ...]\n"
        "  %s -                       (read from stdin)\n"
        "  cmd | %s                   (read from stdin)\n"
        "  cmd | %s - file            (stdin + file)\n"
        "\n"
        "Man buffers (AUTO-DETECT):\n"
        "  %s \"man grep\" \"man sed\" file1 \"man awk\" file2\n"
        "\n"
        "Optional explicit command mode:\n"
        "  %s -m \"man grep\" -m \"man sed\" file1\n",
        prog, prog, prog, prog, prog, prog
    );
}

static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static int contains_substr(const char *s, const char *needle) {
    return strstr(s, needle) != NULL;
}

/*
 * AUTO-DETECT rule:
 * - If the arg begins with "man " -> treat as a man command
 * - Also accept env prefix like "MANWIDTH=200 man grep" (common for wrap control)
 */
static int is_man_command_arg(const char *arg) {
    if (!arg || !*arg) return 0;

    if (starts_with(arg, "man ")) return 1;

    // allow env prefix before man: "MANWIDTH=200 man grep", "FOO=bar MANWIDTH=200 man grep"
    // heuristic: contains " man " and later "man " token appears.
    if (contains_substr(arg, " man ") && contains_substr(arg, "man ")) {
        // A tiny extra guard: make sure the last "man " is not at the very end
        const char *p = strstr(arg, "man ");
        return (p && p[4] != '\0');
    }

    return 0;
}

Language detect_language(const char *filepath) {
    const char *ext = strrchr(filepath, '.');
    if (!ext) return LANG_NONE;

    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return LANG_C;
    if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".hpp") == 0) return LANG_CPP;
    if (strcmp(ext, ".py") == 0) return LANG_PYTHON;
    if (strcmp(ext, ".java") == 0) return LANG_JAVA;
    if (strcmp(ext, ".js") == 0) return LANG_JS;
    if (strcmp(ext, ".ts") == 0 || strcmp(ext, ".tsx") == 0) return LANG_TS;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return LANG_HTML;
    if (strcmp(ext, ".css") == 0) return LANG_CSS;
    if (strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0 || strcmp(ext, ".zsh") == 0) return LANG_SHELL;
    if (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0) return LANG_MARKDOWN;

    // Check for man pages (no extension, contains certain patterns)
    if (strstr(filepath, "/man/") || strstr(filepath, ".man")) return LANG_MAN;

    return LANG_NONE;
}

int is_c_keyword(const char *word) {
    const char *keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "int", "long", "register", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
        NULL
    };

    for (int i = 0; keywords[i]; i++) {
        if (strcmp(word, keywords[i]) == 0) return 1;
    }
    return 0;
}

int is_python_keyword(const char *word) {
    const char *keywords[] = {
        "False", "None", "True", "and", "as", "assert", "async", "await",
        "break", "class", "continue", "def", "del", "elif", "else", "except",
        "finally", "for", "from", "global", "if", "import", "in", "is",
        "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
        "while", "with", "yield",
        NULL
    };

    for (int i = 0; keywords[i]; i++) {
        if (strcmp(word, keywords[i]) == 0) return 1;
    }
    return 0;
}

int is_js_keyword(const char *word) {
    const char *keywords[] = {
        "async", "await", "break", "case", "catch", "class", "const", "continue",
        "debugger", "default", "delete", "do", "else", "export", "extends", "finally",
        "for", "function", "if", "import", "in", "instanceof", "let", "new",
        "return", "super", "switch", "this", "throw", "try", "typeof", "var",
        "void", "while", "with", "yield",
        NULL
    };

    for (int i = 0; keywords[i]; i++) {
        if (strcmp(word, keywords[i]) == 0) return 1;
    }
    return 0;
}

static void strip_overstrikes(char *s) {
    // Removes classic man-page backspace overstrike formatting:
    // "x\b x" (bold) and "_\b x" (underline) both become just "x".
    char *dst = s;
    for (char *src = s; *src; src++) {
        if (*src == '\b') {
            if (dst > s) dst--; // delete previous character
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
}

static int is_man_section_header(const char *s) {
    // Heuristic for man headings: mostly uppercase words, spaces allowed.
    // Examples: NAME, SYNOPSIS, DESCRIPTION, OPTIONS, EXAMPLES, SEE ALSO
    int letters = 0;
    for (; *s; s++) {
        if (*s == ' ') continue;
        if (!isalpha((unsigned char)*s)) return 0;
        letters++;
        if (!isupper((unsigned char)*s)) return 0;
    }
    return letters >= 3;
}

void highlight_line(const char *line, Language lang, int y, int start_x, int line_width) {
    if (!line) return;

    // Man page highlighting (lightweight but helpful)
    if (lang == LANG_MAN) {
        const char *p = line;
        while (*p == ' ') p++;

        // Section heading
        if (is_man_section_header(p)) {
            attron(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            mvaddnstr(y, start_x, line, line_width - start_x);
            attroff(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            return;
        }

        // Highlight flags: -x, --long
        int len2 = (int)strlen(line);
        int i2 = 0;
        int col2 = start_x;

        while (i2 < len2 && col2 < line_width) {
            if (line[i2] == ' ') {
                mvaddch(y, col2++, line[i2++]);
                continue;
            }

            if (line[i2] == '-') {
                int j = i2;
                while (j < len2 && !isspace((unsigned char)line[j])) j++;

                // Reuse NUMBER color for flags
                attron(COLOR_PAIR(COLOR_NUMBER) | A_BOLD);
                while (i2 < j && col2 < line_width) {
                    mvaddch(y, col2++, line[i2++]);
                }
                attroff(COLOR_PAIR(COLOR_NUMBER) | A_BOLD);
                continue;
            }

            mvaddch(y, col2++, line[i2++]);
        }
        return;
    }

    int len = (int)strlen(line);
    int i = 0;
    int col = start_x;

    // Simple syntax highlighting
    while (i < len && col < line_width) {
        char ch = line[i];

        // Comments
        if ((lang == LANG_C || lang == LANG_CPP || lang == LANG_JAVA || lang == LANG_JS || lang == LANG_TS || lang == LANG_CSS) &&
            i + 1 < len && line[i] == '/' && line[i+1] == '/') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            int j = i;
            while (j < len && col < line_width) {
                mvaddch(y, col++, line[j++]);
            }
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        if ((lang == LANG_PYTHON || lang == LANG_SHELL) && ch == '#') {
            attron(COLOR_PAIR(COLOR_COMMENT));
            int j = i;
            while (j < len && col < line_width) {
                mvaddch(y, col++, line[j++]);
            }
            attroff(COLOR_PAIR(COLOR_COMMENT));
            break;
        }

        // Strings
        if (ch == '"' || ch == '\'') {
            char quote = ch;
            attron(COLOR_PAIR(COLOR_STRING));
            mvaddch(y, col++, ch);
            i++;
            while (i < len && col < line_width) {
                ch = line[i];
                mvaddch(y, col++, ch);
                if (ch == quote && (i == 0 || line[i-1] != '\\')) {
                    i++;
                    break;
                }
                i++;
            }
            attroff(COLOR_PAIR(COLOR_STRING));
            continue;
        }

        // Numbers
        if (isdigit((unsigned char)ch)) {
            attron(COLOR_PAIR(COLOR_NUMBER));
            while (i < len && col < line_width &&
                   (isdigit((unsigned char)line[i]) || line[i] == '.')) {
                mvaddch(y, col++, line[i++]);
            }
            attroff(COLOR_PAIR(COLOR_NUMBER));
            continue;
        }

        // Keywords
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

            if (is_keyword) {
                attron(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            }

            for (int k = 0; k < w && col < line_width; k++) {
                mvaddch(y, col++, word[k]);
            }

            if (is_keyword) {
                attroff(COLOR_PAIR(COLOR_KEYWORD) | A_BOLD);
            }
            continue;
        }

        // Default
        mvaddch(y, col++, ch);
        i++;
    }
}

int load_file(Buffer *buf, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return -1;

    buf->line_count = 0;
    buf->scroll_offset = 0;
    strncpy(buf->filepath, filepath, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';
    buf->lang = detect_language(filepath);
    buf->is_active = 1;

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), f) && buf->line_count < MAX_LINES) {
        line[strcspn(line, "\n")] = 0;
        buf->lines[buf->line_count] = strdup(line);
        buf->line_count++;
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

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), stdin) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        strip_overstrikes(line);

        buf->lines[buf->line_count] = strdup(line);
        buf->line_count++;
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

    strncpy(buf->filepath, label, sizeof(buf->filepath) - 1);
    buf->filepath[sizeof(buf->filepath) - 1] = '\0';

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), p) && buf->line_count < MAX_LINES) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        strip_overstrikes(line);

        buf->lines[buf->line_count] = strdup(line);
        buf->line_count++;
    }

    int rc = pclose(p);
    (void)rc;

    return (buf->line_count > 0) ? 0 : -1;
}

void free_buffer(Buffer *buf) {
    for (int i = 0; i < buf->line_count; i++) {
        free(buf->lines[i]);
        buf->lines[i] = NULL;
    }
    buf->line_count = 0;
    buf->is_active = 0;
}

// Search functions
int search_buffer(ViewerState *state, const char *term, int start_line, int direction) {
    Buffer *buf = &state->buffers[state->current_buffer];

    if (term[0] == '\0') return -1;

    int line = start_line;

    for (int i = 0; i < buf->line_count; i++) {
        if (line < 0) line = buf->line_count - 1;
        if (line >= buf->line_count) line = 0;

        if (strstr(buf->lines[line], term)) {
            return line;
        }

        line += direction;
    }

    return -1;
}

void find_all_matches(ViewerState *state) {
    Buffer *buf = &state->buffers[state->current_buffer];
    state->search_match_count = 0;

    if (state->search_term[0] == '\0') return;

    for (int i = 0; i < buf->line_count; i++) {
        if (strstr(buf->lines[i], state->search_term)) {
            state->search_match_count++;
        }
    }
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
            for (int i = 0; i < match; i++) {
                if (strstr(state->buffers[state->current_buffer].lines[i], state->search_term)) {
                    count++;
                }
            }
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
        for (int i = 0; i < match; i++) {
            if (strstr(buf->lines[i], state->search_term)) {
                count++;
            }
        }
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
        for (int i = 0; i < match; i++) {
            if (strstr(buf->lines[i], state->search_term)) {
                count++;
            }
        }
        state->current_match = count;
    }
}

void draw_tabbar(ViewerState *state) {
    int max_x = getmaxx(stdscr);

    attron(COLOR_PAIR(COLOR_TABBAR));
    mvhline(0, 0, ' ', max_x);

    int x = 1;
    for (int i = 0; i < state->buffer_count; i++) {
        if (!state->buffers[i].is_active) continue;

        const char *name = strrchr(state->buffers[i].filepath, '/');
        if (!name) name = state->buffers[i].filepath;
        else name++;

        if (i == state->current_buffer) {
            attron(A_REVERSE | A_BOLD);
        }

        mvprintw(0, x, " %s ", name);
        x += (int)strlen(name) + 2;

        if (i == state->current_buffer) {
            attroff(A_REVERSE | A_BOLD);
        }

        mvaddch(0, x++, '|');
    }

    mvprintw(0, max_x - 10, " [%d/%d] ", state->current_buffer + 1, state->buffer_count);
    attroff(COLOR_PAIR(COLOR_TABBAR));
}

void draw_status_bar(ViewerState *state) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    Buffer *buf = &state->buffers[state->current_buffer];

    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    mvhline(max_y - 2, 0, ' ', max_x);

    const char *name = strrchr(buf->filepath, '/');
    if (!name) name = buf->filepath;
    else name++;

    int percent = buf->line_count > 0 ? (buf->scroll_offset * 100) / buf->line_count : 0;

    char left[512];
    snprintf(left, sizeof(left), " NORMAL | %s | %d%% | %d/%d lines",
             name, percent,
             buf->scroll_offset + 1, buf->line_count);

    mvprintw(max_y - 2, 1, "%s", left);

    if (state->search_term[0] != '\0') {
        char right[256];
        snprintf(right, sizeof(right), "Search: \"%s\" [%d/%d] ",
                 state->search_term,
                 state->current_match + 1,
                 state->search_match_count);
        mvprintw(max_y - 2, max_x - (int)strlen(right) - 1, "%s", right);
    }

    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
}

void draw_help_line() {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    attron(COLOR_PAIR(COLOR_NORMAL));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 1, "j/k:scroll  g/G:top/bottom  /:search  n/N:next/prev  o:fzf-open  Tab:next-buf  q:quit");
    attroff(COLOR_PAIR(COLOR_NORMAL));
}

void draw_buffer(Buffer *buf) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    int content_start_y = 1;
    int content_height = max_y - 3;
    int line_nr_width = 6;

    for (int i = 0; i < content_height; i++) {
        int line_idx = buf->scroll_offset + i;
        int y = content_start_y + i;

        mvhline(y, 0, ' ', max_x);

        if (line_idx >= buf->line_count) continue;

        attron(COLOR_PAIR(COLOR_LINENR));
        mvprintw(y, 1, "%4d ", line_idx + 1);
        attroff(COLOR_PAIR(COLOR_LINENR));

        highlight_line(buf->lines[line_idx], buf->lang, y, line_nr_width + 1, max_x);
    }
}

void draw_ui(ViewerState *state) {
    clear();
    draw_tabbar(state);
    draw_buffer(&state->buffers[state->current_buffer]);
    draw_status_bar(state);
    draw_help_line();
    refresh();
}

void handle_input(ViewerState *state, int *running) {
    int ch = getch();
    int max_y = getmaxy(stdscr);
    int visible_lines = max_y - 3;

    Buffer *buf = &state->buffers[state->current_buffer];

    switch (ch) {
        case 'q':
        case 'Q':
            *running = 0;
            break;

        case '/':
            prompt_search(state);
            break;

        case 'n':
            next_match(state);
            break;

        case 'N':
            prev_match(state);
            break;

        case 'o':
        case 'O': {
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
            break;
        }

        case 'j':
        case KEY_DOWN:
            if (buf->scroll_offset < buf->line_count - 1) buf->scroll_offset++;
            break;

        case 'k':
        case KEY_UP:
            if (buf->scroll_offset > 0) buf->scroll_offset--;
            break;

        case 'g':
            buf->scroll_offset = 0;
            break;

        case 'G':
            buf->scroll_offset = buf->line_count - visible_lines;
            if (buf->scroll_offset < 0) buf->scroll_offset = 0;
            break;

        case 'd':
        case 4: // Ctrl+D
            buf->scroll_offset += visible_lines / 2;
            if (buf->scroll_offset > buf->line_count - visible_lines) {
                buf->scroll_offset = buf->line_count - visible_lines;
            }
            if (buf->scroll_offset < 0) buf->scroll_offset = 0;
            break;

        case 'u':
        case 21: // Ctrl+U
            buf->scroll_offset -= visible_lines / 2;
            if (buf->scroll_offset < 0) buf->scroll_offset = 0;
            break;

        case '\t':
            if (state->buffer_count > 1) {
                state->current_buffer = (state->current_buffer + 1) % state->buffer_count;
            }
            break;

        case KEY_BTAB:
            if (state->buffer_count > 1) {
                state->current_buffer--;
                if (state->current_buffer < 0) state->current_buffer = state->buffer_count - 1;
            }
            break;
    }
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    ViewerState *state = calloc(1, sizeof(ViewerState));
    if (!state) {
        fprintf(stderr, "Failed to allocate memory\n");
        return 1;
    }

    int stdin_is_pipe = !isatty(STDIN_FILENO);
    int loaded_anything = 0;

    // If no args: only valid when stdin is piped
    if (argc < 2) {
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
        // Args provided.
        // Supported:
        // - "-" -> stdin buffer
        // - "-m <cmd>" -> explicit command buffer (kept, but now optional)
        // - AUTO: args that look like "man ..." -> command buffer as LANG_MAN
        // - otherwise: file
        for (int i = 1; i < argc && state->buffer_count < MAX_BUFFERS; i++) {
            // Explicit command buffer mode
            if (strcmp(argv[i], "-m") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "peek: -m requires a command string\n");
                    usage(argv[0]);
                    break;
                }

                const char *cmd = argv[++i];
                char label[1024];
                snprintf(label, sizeof(label), "[%s]", cmd);

                if (load_command(&state->buffers[state->buffer_count], label, cmd, LANG_MAN) == 0) {
                    state->buffer_count++;
                    loaded_anything = 1;
                } else {
                    fprintf(stderr, "peek: failed to run command: %s\n", cmd);
                }
                continue;
            }

            // stdin buffer
            if (strcmp(argv[i], "-") == 0) {
                if (load_stdin(&state->buffers[state->buffer_count]) == 0) {
                    state->buffer_count++;
                    loaded_anything = 1;
                } else {
                    fprintf(stderr, "Failed to read stdin\n");
                }
                continue;
            }

            // AUTO-DETECT man commands (no -m required)
            if (is_man_command_arg(argv[i])) {
                const char *cmd = argv[i];
                char label[1024];
                snprintf(label, sizeof(label), "[%s]", cmd);

                if (load_command(&state->buffers[state->buffer_count], label, cmd, LANG_MAN) == 0) {
                    state->buffer_count++;
                    loaded_anything = 1;
                } else {
                    fprintf(stderr, "peek: failed to run man command: %s\n", cmd);
                }
                continue;
            }

            // Otherwise: normal file
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

    // --- Initialize ncurses ---
    // If stdin is a pipe, ncurses cannot use stdin for keyboard input.
    FILE *tty_in = NULL;
    SCREEN *screen = NULL;

    if (stdin_is_pipe) {
        tty_in = fopen("/dev/tty", "r");
        if (!tty_in) {
            fprintf(stderr, "Failed to open /dev/tty for input: %s\n", strerror(errno));
            free(state);
            return 1;
        }

        screen = newterm(NULL, stdout, tty_in); // output: stdout, input: /dev/tty
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
        init_pair(COLOR_TABBAR,  COLOR_BLACK,  COLOR_CYAN);
        init_pair(COLOR_STATUS,  COLOR_BLACK,  COLOR_CYAN);
        init_pair(COLOR_LINENR,  COLOR_YELLOW, -1);
    }

    int running = 1;
    while (running) {
        draw_ui(state);
        handle_input(state, &running);
    }

    for (int i = 0; i < state->buffer_count; i++) {
        free_buffer(&state->buffers[i]);
    }

    endwin();
    if (screen) delscreen(screen);
    if (tty_in) fclose(tty_in);

    free(state);
    return 0;
}
