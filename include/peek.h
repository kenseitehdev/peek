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
    char http_request[512];
    Language lang;
    int scroll_offset;
    int is_active;
    int is_http_buffer;     
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
    int copy_mode;
    int copy_start_line;
    int copy_end_line;
    int horiz_scroll_offset; 
    int horiz_scroll_step;
} ViewerState;

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