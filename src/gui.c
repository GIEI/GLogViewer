#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#define SDL_MAIN_HANDLED

#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#endif

#include "filter.h"
#include "follow.h"
#include "gui_nuklear.h"
#include "json_pretty.h"
#include "jsonl.h"
#include "output.h"
#include "parsers.h"
#include "reader.h"
#include "search_query.h"
#include "stats.h"
#include <SDL3/SDL_main.h>
#include <time.h>

#define GUI_PATH_CAPACITY 2048
#define GUI_SEARCH_CAPACITY 256
#define GUI_FILTER_CAPACITY 512
#define GUI_REGEX_CAPACITY 256
#define GUI_PATTERN_CAPACITY 512
#define GUI_ROW_CAPACITY 384
#define GUI_PAGE_ROWS 64
#define GUI_RESULT_INDEX_BUFFER 4096
#define GUI_BOOKMARK_CAPACITY 32
#define GUI_CONTEXT_SIDE_MAX 100
#define GUI_CONTEXT_ROWS (GUI_CONTEXT_SIDE_MAX * 2 + 1)
#define GUI_PREVIEW_CAPACITY 320
#define GUI_PROFILE_VALUE_CAPACITY 4096
#define GUI_DETAIL_PAGE_BYTES 1024u
#define GUI_SCAN_BUDGET_NS 16000000u
#define GUI_SCAN_BUDGET_ROWS 4096u
#define GUI_SCAN_SLICE_NS 80000000u
#define GUI_SCAN_SLICE_ROWS 65536u
#define GUI_SEVERITY_COUNT 7
#define GUI_SEVERITY_ALL ((1u << GUI_SEVERITY_COUNT) - 1u)

static const char *const gui_severity_names[GUI_SEVERITY_COUNT] = {
    "TRACE", "DEBUG", "INFO", "NOTICE", "WARN", "ERROR", "OTHER"
};

volatile sig_atomic_t logview_stop_requested = 0;

typedef struct {
    uint64_t sequence;
    uint64_t line_number;
    uint64_t byte_offset;
    char timestamp[48];
    char level[24];
    char source[64];
    char message[GUI_PREVIEW_CAPACITY];
    char raw[GUI_PREVIEW_CAPACITY];
} GuiRow;

static int copy_span(char *destination, size_t capacity, ByteSpan span);

static void populate_row_fields(GuiRow *row, const LogEntry *entry)
{
    static const char *const source_fields[] = {
        "source", "service", "app", "program", "process", "hostname", "host"
    };
    size_t index;
    for (index = 0; index < SDL_arraysize(source_fields); index++) {
        ByteSpan field;
        if (log_entry_find_field(entry, source_fields[index], &field)) {
            copy_span(row->source, sizeof(row->source), field);
            break;
        }
    }
    {
        ByteSpan field;
        if (log_entry_find_field(entry, "timestamp", &field))
            copy_span(row->timestamp, sizeof(row->timestamp), field);
        if (log_entry_find_field(entry, "level", &field))
            copy_span(row->level, sizeof(row->level), field);
        if (log_entry_find_field(entry, "message", &field))
            copy_span(row->message, sizeof(row->message), field);
    }
    if (row->message[0] == '\0')
        SDL_strlcpy(row->message, row->raw, sizeof(row->message));
}

typedef struct {
    uint64_t line_number;
    uint64_t byte_offset;
} GuiIndexEntry;

typedef struct {
    uint64_t line_number;
    char text[GUI_PREVIEW_CAPACITY];
} GuiContextRow;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    struct nk_context *nk;
    Uint32 file_event;
    char path[GUI_PATH_CAPACITY];
    char search[GUI_SEARCH_CAPACITY];
    char filter_text[GUI_FILTER_CAPACITY];
    char regex_text[GUI_REGEX_CAPACITY];
    char field_regex_text[GUI_REGEX_CAPACITY];
    char exclude_regex_text[GUI_REGEX_CAPACITY];
    char custom_pattern[GUI_PATTERN_CAPACITY];
    char export_columns[GUI_FILTER_CAPACITY];
    char jump_text[32];
    char error[256];
    char status[256];
    char preferences_path[GUI_PATH_CAPACITY];
    FILE *input;
    FILE *result_index;
    GuiIndexEntry result_index_buffer[GUI_RESULT_INDEX_BUFFER];
    size_t result_index_buffer_count;
    FILE *physical_index;
    uint64_t physical_checkpoint_count;
    LineReader reader;
    int reader_ready;
    FILE *enrichment_input;
    LineReader enrichment_reader;
    int enrichment_reader_ready;
    uint64_t enrichment_sequence;
    uint64_t enrichment_start;
    uint64_t enrichment_end;
    int enriching;
    JsonScratch json_scratch;
    int scratch_ready;
    FILE *export_input;
    LineReader export_reader;
    int export_reader_ready;
    JsonScratch export_scratch;
    int export_scratch_ready;
    OutputSink output;
    int output_ready;
    OutputFormat export_format;
    uint64_t export_physical_records;
    uint64_t export_selected_records;
    InputFormat format;
    InputFormat selected_format;
    SearchQuery *query;
    Filter *filter;
    Regex *raw_regex;
    Regex *field_regex;
    Regex *exclude_regex;
    char regex_field_name[128];
    GuiRow rows[GUI_ROW_CAPACITY];
    unsigned char *detail_data;
    size_t detail_length;
    unsigned char *pretty_data;
    size_t pretty_length;
    LogEntry detail_entry;
    int detail_entry_valid;
    int detail_tab;
    int pretty_visible;
    size_t detail_page;
    uint64_t detail_sequence;
    int detail_loaded;
    int show_timestamp_column;
    int show_level_column;
    int show_source_column;
    unsigned syslog_year;
    int syslog_timezone_offset_seconds;
    int window_width;
    int window_height;
    uint64_t physical_records;
    uint64_t selected_records;
    uint64_t selected_sequence;
    uint64_t bytes_read;
    uint64_t input_size;
    Uint64 load_started_ns;
    Uint64 scan_started_ns;
    Uint64 open_elapsed_ns;
    Uint64 scan_elapsed_ns;
    Uint64 enrichment_started_ns;
    Uint64 enrichment_elapsed_ns;
    int follow_enabled;
    uint32_t severity_filter_mask;
    int severity_filter_active;
    int follow_waiting;
    int follow_pending_partial;
    Uint64 follow_last_poll_ns;
    uint64_t follow_observed_end;
    uint64_t follow_resume_offset;
    uint64_t follow_line_number_base;
    int has_selection;
    int scanning;
    int scan_complete;
    int open_dialog_pending;
    int dialog_action;
    int exporting;
    uint64_t view_start;
    uint64_t page_load_next;
    uint64_t page_load_end;
    int page_loading;
    FILE *page_input;
    LineReader page_reader;
    int page_reader_ready;
    uint64_t bookmark_sequences[GUI_BOOKMARK_CAPACITY];
    size_t bookmark_count;
    GuiContextRow context_rows[GUI_CONTEXT_ROWS];
    size_t context_count;
    size_t context_before;
    size_t context_after;
    int context_visible;
    int context_loading;
    uint64_t context_anchor_line;
    uint64_t context_scan_line;
    uint64_t context_target_sequence;
    size_t context_before_count;
    GuiContextRow context_before_rows[GUI_CONTEXT_SIDE_MAX];
    FILE *context_input;
    LineReader context_reader;
    int context_reader_ready;
    int help_visible;
    char analysis_field[128];
    int analysis_view;
    int analysis_active;
    int analysis_phase;
    int analysis_cache_valid;
    uint64_t analysis_generation;
    uint64_t analysis_run_generation;
    uint64_t analysis_cache_generation;
    InputFormat analysis_cache_format;
    unsigned analysis_cache_year;
    int analysis_cache_timezone_offset_seconds;
    char analysis_cache_field[128];
    uint64_t analysis_physical;
    uint64_t analysis_selected;
    uint64_t analysis_result_sequence;
    uint64_t histogram_first_sequence[LOGVIEW_TIMELINE_BINS];
    FILE *analysis_input;
    LineReader analysis_reader;
    int analysis_reader_ready;
    JsonScratch analysis_scratch;
    int analysis_scratch_ready;
    LogStats analysis_stats;
    int level_nav_active;
    int level_nav_target;
    int level_nav_direction;
    uint64_t level_nav_sequence;
    uint64_t level_nav_checked;
    uint64_t level_nav_origin;
    FILE *level_nav_input;
    LineReader level_nav_reader;
    int level_nav_reader_ready;
    JsonScratch level_nav_scratch;
    int level_nav_scratch_ready;
    int quit;
} GuiApp;

static const char *const format_names[] = {
    "Auto detect", "Raw text", "JSON Lines", "Syslog RFC 3164",
    "Syslog RFC 5424", "Kubernetes CRI", "Apache Common",
    "Apache Combined", "Apache Error", "NGINX Combined", "Custom pattern"
};

static const InputFormat format_values[] = {
    FORMAT_AUTO, FORMAT_RAW, FORMAT_JSONL, FORMAT_SYSLOG3164,
    FORMAT_SYSLOG5424, FORMAT_CRI, FORMAT_APACHE_COMMON,
    FORMAT_APACHE_COMBINED, FORMAT_APACHE_ERROR, FORMAT_NGINX_COMBINED,
    FORMAT_CUSTOM
};

static const char *const export_names[] = {"Text export", "CSV export"};

static GuiRow *cached_row(GuiApp *app, uint64_t sequence);
static void start_scan(GuiApp *app);
static void start_enrichment(GuiApp *app);
static void request_page(GuiApp *app, uint64_t start);
static void clear_detail(GuiApp *app);
static int field_equals_case(ByteSpan field, const char *text)
{
    size_t length = SDL_strlen(text);
    return field.length == length &&
           SDL_strncasecmp((const char *)field.data, text, length) == 0;
}

static int record_matches_queries(GuiApp *app, LogEntry *entry,
                                  const unsigned char *line, size_t length,
                                  int apply_field_filter);

static void SDLCALL file_dialog_callback(void *userdata,
                                         const char *const *filelist,
                                         int filter);

static const char *format_label(InputFormat format)
{
    size_t index;
    for (index = 0; index < SDL_arraysize(format_values); index++) {
        if (format_values[index] == format) return format_names[index];
    }
    return "Custom";
}

static void set_error(GuiApp *app, const char *message)
{
    SDL_strlcpy(app->error, message, sizeof(app->error));
}

static int parse_gui_unsigned(const char *text, unsigned maximum,
                              unsigned *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
        return 0;
    *value = (unsigned)parsed;
    return 1;
}

static int parse_gui_timezone(const char *text, int *offset_seconds)
{
    int sign;
    unsigned hours;
    unsigned minutes;
    char hour_text[3];
    char minute_text[3];
    if (text == NULL || strlen(text) != 6 ||
        (text[0] != '+' && text[0] != '-') || text[3] != ':') return 0;
    hour_text[0] = text[1]; hour_text[1] = text[2]; hour_text[2] = '\0';
    minute_text[0] = text[4]; minute_text[1] = text[5]; minute_text[2] = '\0';
    if (!parse_gui_unsigned(hour_text, 23, &hours) ||
        !parse_gui_unsigned(minute_text, 59, &minutes)) return 0;
    sign = text[0] == '-' ? -1 : 1;
    *offset_seconds = sign * (int)(hours * 3600u + minutes * 60u);
    return 1;
}

static int parse_gui_record(GuiApp *app, LogEntry *entry,
                            const unsigned char *line, size_t length,
                            uint64_t line_number, uint64_t byte_offset,
                            JsonScratch *scratch)
{
    int parsed;
    memset(entry, 0, sizeof(*entry));
    entry->raw.data = line;
    entry->raw.length = length;
    entry->line_number = line_number;
    entry->byte_offset = byte_offset;
    entry->parse_ok = app->format == FORMAT_RAW;
    if (app->format == FORMAT_JSONL)
        parsed = jsonl_parse(entry, scratch);
    else if (app->format == FORMAT_CUSTOM)
        parsed = parser_custom_line(app->custom_pattern, entry);
    else if (app->format == FORMAT_RAW)
        parsed = 1;
    else
        parsed = parser_format_line(app->format, entry);
    if (!parsed) entry->field_count = 0;
    return parsed;
}

static void apply_theme(GuiApp *app)
{
    static const struct nk_color palette[NK_COLOR_COUNT] = {
        {226, 232, 240, 255}, {16, 24, 32, 255}, {25, 37, 49, 255},
        {48, 64, 80, 255}, {35, 49, 64, 255}, {49, 68, 88, 255},
        {37, 99, 235, 255}, {31, 48, 65, 255}, {42, 65, 86, 255},
        {96, 165, 250, 255}, {28, 51, 69, 255}, {37, 99, 235, 255},
        {32, 48, 64, 255}, {59, 130, 246, 255}, {96, 165, 250, 255},
        {29, 78, 216, 255}, {22, 34, 46, 255}, {22, 34, 46, 255},
        {226, 232, 240, 255}, {25, 37, 49, 255}, {22, 34, 46, 255},
        {59, 130, 246, 255}, {96, 165, 250, 255}, {22, 34, 46, 255},
        {51, 65, 81, 255}, {71, 85, 105, 255}, {96, 165, 250, 255},
        {25, 37, 49, 255}, {31, 48, 65, 255}, {59, 130, 246, 255},
        {96, 165, 250, 255}, {29, 78, 216, 255}
    };
    nk_style_from_table(app->nk, palette);
    app->nk->style.window.padding = nk_vec2(16.0f, 14.0f);
    app->nk->style.window.spacing = nk_vec2(10.0f, 9.0f);
    app->nk->style.window.group_padding = nk_vec2(10.0f, 8.0f);
    app->nk->style.window.rounding = 6.0f;
    app->nk->style.button.rounding = 5.0f;
    app->nk->style.edit.rounding = 4.0f;
    app->nk->style.text.padding = nk_vec2(4.0f, 3.0f);
}

static void sidebar_section_title(GuiApp *app, const char *title)
{
    nk_layout_row_dynamic(app->nk, 25, 1);
    nk_label_colored(app->nk, title, NK_TEXT_LEFT, nk_rgb(177, 197, 220));
}

static int active_tab_button(GuiApp *app, const char *label, int active)
{
    int clicked;
    if (active) {
        struct nk_color color = nk_rgb(20, 116, 184);
        nk_style_push_style_item(app->nk, &app->nk->style.button.normal,
                                 nk_style_item_color(color));
        nk_style_push_style_item(app->nk, &app->nk->style.button.hover,
                                 nk_style_item_color(nk_rgb(24, 137, 211)));
        nk_style_push_style_item(app->nk, &app->nk->style.button.active,
                                 nk_style_item_color(nk_rgb(16, 101, 165)));
        nk_style_push_color(app->nk, &app->nk->style.button.text_normal,
                            nk_rgb(239, 248, 255));
        nk_style_push_color(app->nk, &app->nk->style.button.text_hover,
                            nk_rgb(255, 255, 255));
        nk_style_push_color(app->nk, &app->nk->style.button.text_active,
                            nk_rgb(255, 255, 255));
    }
    clicked = nk_button_label(app->nk, label);
    if (active) {
        nk_style_pop_color(app->nk);
        nk_style_pop_color(app->nk);
        nk_style_pop_color(app->nk);
        nk_style_pop_style_item(app->nk);
        nk_style_pop_style_item(app->nk);
        nk_style_pop_style_item(app->nk);
    }
    return clicked;
}

static void detail_metadata_row(GuiApp *app, const char *label,
                                const char *value,
                                struct nk_color value_color)
{
    nk_layout_row_template_begin(app->nk, 46);
    nk_layout_row_template_push_static(app->nk, 84);
    nk_layout_row_template_push_dynamic(app->nk);
    nk_layout_row_template_end(app->nk);
    nk_label_colored(app->nk, label, NK_TEXT_LEFT, nk_rgb(145, 163, 184));
    nk_label_colored_wrap(app->nk, value, value_color);
}

static struct nk_font *load_ui_font(struct nk_font_atlas *atlas, float height,
                                    const struct nk_font_config *config)
{
#ifdef _WIN32
    static const char *const paths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf"
    };
#elif defined(__APPLE__)
    static const char *const paths[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf"
    };
#else
    static const char *const paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"
    };
#endif
    size_t index;
    for (index = 0; index < SDL_arraysize(paths); index++) {
        struct nk_font *font = nk_font_atlas_add_from_file(
            atlas, paths[index], height, config);
        if (font != NULL) return font;
    }
    return nk_font_atlas_add_default(atlas, height, config);
}

static FILE *open_utf8_file(const char *path)
{
#ifdef _WIN32
    int wide_length;
    wchar_t *wide_path;
    FILE *file;
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      path, -1, NULL, 0);
    if (wide_length <= 0) return NULL;
    wide_path = (wchar_t *)malloc((size_t)wide_length * sizeof(*wide_path));
    if (wide_path == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, wide_length) <= 0) {
        free(wide_path);
        return NULL;
    }
    file = _wfopen(wide_path, L"rb");
    free(wide_path);
    return file;
#else
    return fopen(path, "rb");
#endif
}

static FILE *open_utf8_file_mode(const char *path, const char *mode)
{
#ifdef _WIN32
    int wide_length;
    wchar_t *wide_path;
    wchar_t wide_mode[4];
    size_t mode_length = strlen(mode);
    FILE *file;
    if (mode_length >= SDL_arraysize(wide_mode)) return NULL;
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      path, -1, NULL, 0);
    if (wide_length <= 0) return NULL;
    wide_path = (wchar_t *)malloc((size_t)wide_length * sizeof(*wide_path));
    if (wide_path == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide_path, wide_length) <= 0) {
        free(wide_path);
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                            wide_mode, (int)SDL_arraysize(wide_mode)) <= 0) {
        free(wide_path);
        return NULL;
    }
    file = _wfopen(wide_path, wide_mode);
    free(wide_path);
    return file;
#else
    return fopen(path, mode);
#endif
}

static const char *gui_profile_format_name(InputFormat format)
{
    switch (format) {
    case FORMAT_AUTO: return "auto";
    case FORMAT_RAW: return "raw";
    case FORMAT_JSONL: return "jsonl";
    case FORMAT_SYSLOG3164: return "syslog3164";
    case FORMAT_SYSLOG5424: return "syslog5424";
    case FORMAT_CRI: return "cri";
    case FORMAT_APACHE_COMMON: return "apache-common";
    case FORMAT_APACHE_COMBINED: return "apache-combined";
    case FORMAT_APACHE_ERROR: return "apache-error";
    case FORMAT_NGINX_COMBINED: return "nginx-combined";
    case FORMAT_CUSTOM: return "custom";
    default: return NULL;
    }
}

static int copy_profile_value(char *destination, size_t capacity,
                              const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity || strchr(value, '\n') != NULL ||
        strchr(value, '\r') != NULL) return 0;
    memcpy(destination, value, length + 1);
    return 1;
}

static int load_gui_profile(GuiApp *app, const char *path)
{
    FILE *file = open_utf8_file(path);
    char line[GUI_PROFILE_VALUE_CAPACITY + 32];
    char pattern[GUI_PATTERN_CAPACITY] = "";
    char filter[GUI_FILTER_CAPACITY] = "";
    char regex[GUI_REGEX_CAPACITY] = "";
    char field_regex[GUI_REGEX_CAPACITY] = "";
    char exclude_regex[GUI_REGEX_CAPACITY] = "";
    InputFormat format = FORMAT_AUTO;
    unsigned int seen = 0;
    int line_number = 1;
    int format_seen = 0;
    size_t context_before = app->context_before;
    size_t context_after = app->context_after;
    unsigned syslog_year = app->syslog_year;
    int syslog_timezone_offset_seconds =
        app->syslog_timezone_offset_seconds;
    if (file == NULL) {
        SDL_snprintf(app->error, sizeof(app->error),
                     "Cannot open query profile: %s", path);
        return 0;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        SDL_snprintf(app->error, sizeof(app->error),
                     "Unsupported query profile: %s", path);
        return 0;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "GLOGVIEW_PROFILE 1") != 0) {
        fclose(file);
        SDL_snprintf(app->error, sizeof(app->error),
                     "Unsupported query profile: %s", path);
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *separator;
        char *value;
        unsigned int bit;
        size_t length = strlen(line);
        line_number++;
        if (length != 0 && line[length - 1] == '\n') line[--length] = '\0';
        if (length != 0 && line[length - 1] == '\r') line[--length] = '\0';
        if (strchr(line, '\n') != NULL || strchr(line, '\r') != NULL ||
            (length == sizeof(line) - 1 && !feof(file))) goto invalid_profile;
        if (line[0] == '\0') continue;
        separator = strchr(line, '=');
        if (separator == NULL) goto invalid_profile;
        *separator = '\0';
        value = separator + 1;
        if (strcmp(line, "format") == 0) {
            InputFormat parsed = parser_format_from_name(value);
            bit = 1u << 0;
            if ((int)parsed < 0) goto invalid_profile;
            format = parsed;
            format_seen = 1;
        } else if (strcmp(line, "pattern") == 0) bit = 1u << 1;
        else if (strcmp(line, "filter") == 0) bit = 1u << 2;
        else if (strcmp(line, "regex") == 0) bit = 1u << 3;
        else if (strcmp(line, "regex_field") == 0) bit = 1u << 4;
        else if (strcmp(line, "exclude_regex") == 0) bit = 1u << 5;
        else if (strcmp(line, "context_before") == 0) bit = 1u << 6;
        else if (strcmp(line, "context_after") == 0) bit = 1u << 7;
        else if (strcmp(line, "syslog_year") == 0) bit = 1u << 8;
        else if (strcmp(line, "syslog_timezone") == 0) bit = 1u << 9;
        else goto invalid_profile;
        if ((seen & bit) != 0) goto invalid_profile;
        seen |= bit;
        if (bit == (1u << 1) &&
            !copy_profile_value(pattern, sizeof(pattern), value))
            goto invalid_profile;
        if (bit == (1u << 2) &&
            !copy_profile_value(filter, sizeof(filter), value))
            goto invalid_profile;
        if (bit == (1u << 3) &&
            !copy_profile_value(regex, sizeof(regex), value))
            goto invalid_profile;
        if (bit == (1u << 4) &&
            !copy_profile_value(field_regex, sizeof(field_regex), value))
            goto invalid_profile;
        if (bit == (1u << 5) &&
            !copy_profile_value(exclude_regex, sizeof(exclude_regex), value))
            goto invalid_profile;
        if (bit == (1u << 6) || bit == (1u << 7)) {
            char *end = NULL;
            unsigned long parsed;
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' ||
                parsed > GUI_CONTEXT_SIDE_MAX) goto invalid_profile;
            if (bit == (1u << 6)) context_before = (size_t)parsed;
            else context_after = (size_t)parsed;
        }
        if (bit == (1u << 8)) {
            unsigned parsed;
            if (!parse_gui_unsigned(value, 9999, &parsed))
                goto invalid_profile;
            syslog_year = parsed;
        }
        if (bit == (1u << 9)) {
            if (value[0] == '\0') syslog_timezone_offset_seconds = 0;
            else if (!parse_gui_timezone(value,
                                         &syslog_timezone_offset_seconds))
                goto invalid_profile;
        }
    }
    if (ferror(file) || !format_seen) goto invalid_profile;
    fclose(file);
    app->selected_format = format;
    SDL_strlcpy(app->custom_pattern, pattern, sizeof(app->custom_pattern));
    SDL_strlcpy(app->filter_text, filter, sizeof(app->filter_text));
    SDL_strlcpy(app->regex_text, regex, sizeof(app->regex_text));
    SDL_strlcpy(app->field_regex_text, field_regex,
                sizeof(app->field_regex_text));
    SDL_strlcpy(app->exclude_regex_text, exclude_regex,
                sizeof(app->exclude_regex_text));
    app->context_before = context_before;
    app->context_after = context_after;
    app->syslog_year = syslog_year;
    app->syslog_timezone_offset_seconds =
        syslog_timezone_offset_seconds;
    app->search[0] = '\0';
    app->severity_filter_active = 0;
    app->severity_filter_mask = GUI_SEVERITY_ALL;
    app->error[0] = '\0';
    SDL_snprintf(app->status, sizeof(app->status),
                 "Loaded query profile: %s", path);
    if (app->path[0] != '\0' && !app->scanning && !app->exporting)
        start_scan(app);
    return 1;

invalid_profile:
    fclose(file);
    SDL_snprintf(app->error, sizeof(app->error),
                 "Invalid query profile on line %d: %s", line_number, path);
    return 0;
}

static int save_gui_profile(GuiApp *app, const char *path)
{
    FILE *file;
    const char *format = gui_profile_format_name(app->selected_format);
    char timezone[8];
    unsigned absolute_offset;
    int failed;
    if (app->path[0] != '\0' && output_paths_same_file(path, app->path)) {
        SDL_strlcpy(app->error,
                    "The query profile path must not be the active log file.",
                    sizeof(app->error));
        return 0;
    }
    file = open_utf8_file_mode(path, "wb");
    if (file == NULL || format == NULL) {
        if (file != NULL) fclose(file);
        SDL_snprintf(app->error, sizeof(app->error),
                     "Cannot write query profile: %s", path);
        return 0;
    }
    absolute_offset = (unsigned)(app->syslog_timezone_offset_seconds < 0 ?
        -app->syslog_timezone_offset_seconds :
        app->syslog_timezone_offset_seconds);
    SDL_snprintf(timezone, sizeof(timezone), "%c%02u:%02u",
                 app->syslog_timezone_offset_seconds < 0 ? '-' : '+',
                 absolute_offset / 3600u,
                 (absolute_offset % 3600u) / 60u);
    failed = fprintf(file,
        "GLOGVIEW_PROFILE 1\nformat=%s\npattern=%s\nfilter=%s\nregex=%s\nregex_field=%s\nexclude_regex=%s\nsyslog_year=%u\nsyslog_timezone=%s\ncontext_before=%u\ncontext_after=%u\n",
        format, app->custom_pattern, app->filter_text, app->regex_text,
        app->field_regex_text, app->exclude_regex_text,
        app->syslog_year, timezone,
        (unsigned)app->context_before, (unsigned)app->context_after) < 0;
    if (fflush(file) != 0) failed = 1;
    if (fclose(file) != 0) failed = 1;
    if (failed) {
        SDL_snprintf(app->error, sizeof(app->error),
                     "Failed to finish query profile: %s", path);
        return 0;
    }
    app->error[0] = '\0';
    SDL_snprintf(app->status, sizeof(app->status),
                 "Saved query profile: %s", path);
    return 1;
}

static int prepare_preferences_path(GuiApp *app)
{
    char *directory = SDL_GetPrefPath("GIEI", "GLogViewer");
    int length;
    if (directory == NULL) return 0;
    length = SDL_snprintf(app->preferences_path,
                          sizeof(app->preferences_path),
                          "%sgui-preferences.conf", directory);
    SDL_free(directory);
    if (length < 0 || (size_t)length >= sizeof(app->preferences_path)) {
        app->preferences_path[0] = '\0';
        return 0;
    }
    return 1;
}

static int parse_preference_boolean(const char *value, int *parsed)
{
    if (strcmp(value, "0") == 0) *parsed = 0;
    else if (strcmp(value, "1") == 0) *parsed = 1;
    else return 0;
    return 1;
}

static int parse_preference_integer(const char *value, int minimum,
                                    int maximum, int *parsed)
{
    char *end = NULL;
    long number;
    errno = 0;
    number = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        number < minimum || number > maximum) return 0;
    *parsed = (int)number;
    return 1;
}

static void load_gui_preferences(GuiApp *app)
{
    FILE *file;
    char line[GUI_PROFILE_VALUE_CAPACITY + 64];
    char export_columns[GUI_FILTER_CAPACITY];
    unsigned int seen = 0;
    int show_timestamp = app->show_timestamp_column;
    int show_level = app->show_level_column;
    int show_source = app->show_source_column;
    int context_before = (int)app->context_before;
    int context_after = (int)app->context_after;
    int export_format = (int)app->export_format;
    int window_width = app->window_width;
    int window_height = app->window_height;
    int line_number = 1;
    SDL_strlcpy(export_columns, app->export_columns,
                sizeof(export_columns));
    if (!prepare_preferences_path(app)) return;
    file = open_utf8_file(app->preferences_path);
    if (file == NULL) return;
    if (fgets(line, sizeof(line), file) == NULL) goto invalid_preferences;
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "GLOGVIEW_GUI_PREFERENCES 1") != 0)
        goto invalid_preferences;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *separator;
        char *value;
        unsigned int bit;
        size_t length = strlen(line);
        int parsed;
        line_number++;
        if (length != 0 && line[length - 1] == '\n') line[--length] = '\0';
        if (length != 0 && line[length - 1] == '\r') line[--length] = '\0';
        if (length == sizeof(line) - 1 && !feof(file))
            goto invalid_preferences;
        if (strchr(line, '\r') != NULL || strchr(line, '\n') != NULL)
            goto invalid_preferences;
        if (line[0] == '\0') continue;
        separator = strchr(line, '=');
        if (separator == NULL) goto invalid_preferences;
        *separator = '\0';
        value = separator + 1;
        if (strcmp(line, "show_timestamp") == 0) bit = 1u << 0;
        else if (strcmp(line, "show_level") == 0) bit = 1u << 1;
        else if (strcmp(line, "show_source") == 0) bit = 1u << 2;
        else if (strcmp(line, "context_before") == 0) bit = 1u << 3;
        else if (strcmp(line, "context_after") == 0) bit = 1u << 4;
        else if (strcmp(line, "export_format") == 0) bit = 1u << 5;
        else if (strcmp(line, "window_width") == 0) bit = 1u << 6;
        else if (strcmp(line, "window_height") == 0) bit = 1u << 7;
        else if (strcmp(line, "export_columns") == 0) bit = 1u << 8;
        else goto invalid_preferences;
        if ((seen & bit) != 0) goto invalid_preferences;
        seen |= bit;
        if (bit == (1u << 0) &&
            !parse_preference_boolean(value, &show_timestamp))
            goto invalid_preferences;
        if (bit == (1u << 1) &&
            !parse_preference_boolean(value, &show_level))
            goto invalid_preferences;
        if (bit == (1u << 2) &&
            !parse_preference_boolean(value, &show_source))
            goto invalid_preferences;
        if (bit == (1u << 3)) {
            if (!parse_preference_integer(value, 0, GUI_CONTEXT_SIDE_MAX,
                                          &parsed)) goto invalid_preferences;
            context_before = parsed;
        }
        if (bit == (1u << 4)) {
            if (!parse_preference_integer(value, 0, GUI_CONTEXT_SIDE_MAX,
                                          &parsed)) goto invalid_preferences;
            context_after = parsed;
        }
        if (bit == (1u << 5)) {
            if (strcmp(value, "text") == 0) export_format = OUTPUT_TEXT;
            else if (strcmp(value, "csv") == 0) export_format = OUTPUT_CSV;
            else goto invalid_preferences;
        }
        if (bit == (1u << 6) &&
            !parse_preference_integer(value, 960, 8192, &window_width))
            goto invalid_preferences;
        if (bit == (1u << 7) &&
            !parse_preference_integer(value, 640, 8192, &window_height))
            goto invalid_preferences;
        if (bit == (1u << 8) &&
            !copy_profile_value(export_columns, sizeof(export_columns), value))
            goto invalid_preferences;
    }
    if (ferror(file)) goto invalid_preferences;
    fclose(file);
    app->show_timestamp_column = show_timestamp;
    app->show_level_column = show_level;
    app->show_source_column = show_source;
    app->context_before = (size_t)context_before;
    app->context_after = (size_t)context_after;
    app->export_format = (OutputFormat)export_format;
    app->window_width = window_width;
    app->window_height = window_height;
    SDL_strlcpy(app->export_columns, export_columns,
                sizeof(app->export_columns));
    return;

invalid_preferences:
    fclose(file);
    SDL_strlcpy(app->status,
                "Invalid GUI preferences ignored; using defaults",
                sizeof(app->status));
}

static void save_gui_preferences(GuiApp *app)
{
    FILE *file;
    const char *export_format = app->export_format == OUTPUT_CSV ? "csv" : "text";
    int width = app->window_width;
    int height = app->window_height;
    int write_error;
    if (app->window != NULL) SDL_GetWindowSize(app->window, &width, &height);
    if (!prepare_preferences_path(app)) return;
    file = open_utf8_file_mode(app->preferences_path, "wb");
    if (file == NULL) return;
    write_error = fprintf(file,
        "GLOGVIEW_GUI_PREFERENCES 1\nshow_timestamp=%d\nshow_level=%d\nshow_source=%d\ncontext_before=%u\ncontext_after=%u\nexport_format=%s\nexport_columns=%s\nwindow_width=%d\nwindow_height=%d\n",
        app->show_timestamp_column, app->show_level_column,
        app->show_source_column, (unsigned)app->context_before,
        (unsigned)app->context_after, export_format, app->export_columns,
        width, height) < 0;
    if (fflush(file) != 0) write_error = 1;
    if (fclose(file) != 0) write_error = 1;
    if (write_error)
        SDL_strlcpy(app->error, "Could not save GUI preferences",
                    sizeof(app->error));
}

static void close_scan(GuiApp *app)
{
    app->scanning = 0;
    if (app->reader_ready) {
        reader_destroy(&app->reader);
        app->reader_ready = 0;
    }
    if (app->input != NULL) {
        fclose(app->input);
        app->input = NULL;
    }
    if (app->result_index != NULL) {
        fclose(app->result_index);
        app->result_index = NULL;
    }
    if (app->physical_index != NULL) {
        fclose(app->physical_index);
        app->physical_index = NULL;
    }
    if (app->scratch_ready) {
        json_scratch_destroy(&app->json_scratch);
        app->scratch_ready = 0;
    }
}

static int gui_seek(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    if (offset > (uint64_t)INT64_MAX) return 0;
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    if (offset > (uint64_t)INT64_MAX) return 0;
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static int flush_result_index_buffer(GuiApp *app)
{
    size_t count = app->result_index_buffer_count;
    if (count == 0) return 1;
    if (app->result_index == NULL ||
        fwrite(app->result_index_buffer, sizeof(app->result_index_buffer[0]),
               count, app->result_index) != count) return 0;
    app->result_index_buffer_count = 0;
    return 1;
}

static int read_index_entry(GuiApp *app, uint64_t sequence,
                            GuiIndexEntry *entry)
{
    uint64_t offset;
    if (app->result_index == NULL ||
        sequence > UINT64_MAX / (uint64_t)sizeof(*entry) ||
        !flush_result_index_buffer(app)) return 0;
    offset = sequence * (uint64_t)sizeof(*entry);
    return gui_seek(app->result_index, offset) &&
           fread(entry, sizeof(*entry), 1, app->result_index) == 1;
}

static int read_physical_checkpoint(GuiApp *app, uint64_t checkpoint,
                                    GuiIndexEntry *entry)
{
    uint64_t offset;
    if (app->physical_index == NULL ||
        checkpoint > UINT64_MAX / (uint64_t)sizeof(*entry)) return 0;
    offset = checkpoint * (uint64_t)sizeof(*entry);
    return gui_seek(app->physical_index, offset) &&
           fread(entry, sizeof(*entry), 1, app->physical_index) == 1;
}

static int append_physical_checkpoint(GuiApp *app, uint64_t line_number,
                                      uint64_t byte_offset)
{
    GuiIndexEntry entry;
    if (app->physical_index == NULL) return 0;
    entry.line_number = line_number;
    entry.byte_offset = byte_offset;
    if (fwrite(&entry, sizeof(entry), 1, app->physical_index) != 1) return 0;
    app->physical_checkpoint_count++;
    return 1;
}

static void close_page_load(GuiApp *app)
{
    app->page_loading = 0;
    if (app->page_reader_ready) {
        reader_destroy(&app->page_reader);
        app->page_reader_ready = 0;
    }
    if (app->page_input != NULL) {
        fclose(app->page_input);
        app->page_input = NULL;
    }
}

static void close_context_load(GuiApp *app)
{
    app->context_loading = 0;
    if (app->context_reader_ready) {
        reader_destroy(&app->context_reader);
        app->context_reader_ready = 0;
    }
    if (app->context_input != NULL) {
        fclose(app->context_input);
        app->context_input = NULL;
    }
}

static void close_analysis(GuiApp *app)
{
    app->analysis_active = 0;
    if (app->analysis_reader_ready) {
        reader_destroy(&app->analysis_reader);
        app->analysis_reader_ready = 0;
    }
    if (app->analysis_input != NULL) {
        fclose(app->analysis_input);
        app->analysis_input = NULL;
    }
    if (app->analysis_scratch_ready) {
        json_scratch_destroy(&app->analysis_scratch);
        app->analysis_scratch_ready = 0;
    }
}

static void invalidate_analysis_cache(GuiApp *app)
{
    app->analysis_cache_valid = 0;
    app->analysis_generation++;
}

static void close_level_navigation(GuiApp *app)
{
    app->level_nav_active = 0;
    if (app->level_nav_reader_ready) {
        reader_destroy(&app->level_nav_reader);
        app->level_nav_reader_ready = 0;
    }
    if (app->level_nav_input != NULL) {
        fclose(app->level_nav_input);
        app->level_nav_input = NULL;
    }
    if (app->level_nav_scratch_ready) {
        json_scratch_destroy(&app->level_nav_scratch);
        app->level_nav_scratch_ready = 0;
    }
}

static int analysis_parse_entry(GuiApp *app, LogEntry *entry,
                                const unsigned char *line, size_t length,
                                uint64_t line_number, uint64_t byte_offset)
{
    return parse_gui_record(app, entry, line, length, line_number,
                            byte_offset, &app->analysis_scratch);
}

static void start_analysis(GuiApp *app, int view)
{
    StatsTimestampConfig config;
    time_t now;
    struct tm *local_time;
    if (!app->scan_complete || app->scanning) {
        close_analysis(app);
        app->analysis_view = view;
        set_error(app, "Finish the scan before starting analysis");
        return;
    }
    now = time(NULL);
    local_time = localtime(&now);
    config.syslog_year = app->syslog_year != 0 ? app->syslog_year :
        (local_time != NULL ? (unsigned)(local_time->tm_year + 1900) : 0);
    config.syslog_timezone_offset_seconds =
        app->syslog_timezone_offset_seconds;

    if (app->analysis_cache_valid &&
        app->analysis_cache_generation == app->analysis_generation &&
        app->analysis_cache_format == app->format &&
        app->analysis_cache_year == config.syslog_year &&
        app->analysis_cache_timezone_offset_seconds ==
            config.syslog_timezone_offset_seconds &&
        strcmp(app->analysis_cache_field, app->analysis_field) == 0) {
        close_analysis(app);
        app->analysis_view = view;
        app->error[0] = '\0';
        SDL_strlcpy(app->status, "Analysis results loaded from cache",
                    sizeof(app->status));
        return;
    }

    close_analysis(app);
    app->analysis_cache_valid = 0;
    app->analysis_run_generation = app->analysis_generation;
    app->analysis_view = view;
    app->analysis_physical = 0;
    app->analysis_selected = 0;
    app->analysis_result_sequence = 0;
    {
        size_t bin;
        for (bin = 0; bin < LOGVIEW_TIMELINE_BINS; bin++)
            app->histogram_first_sequence[bin] = UINT64_MAX;
    }
    stats_init(&app->analysis_stats);
    if (app->analysis_field[0] != '\0' &&
        !stats_set_focus_field(&app->analysis_stats,
                               (const unsigned char *)app->analysis_field,
                               SDL_strlen(app->analysis_field))) {
        set_error(app, "Cannot configure the field summary");
        return;
    }
    stats_set_timestamp_config(&app->analysis_stats, &config);
    app->analysis_input = open_utf8_file(app->path);
    if (app->analysis_input == NULL ||
        !reader_init(&app->analysis_reader, app->analysis_input,
                     LOGVIEW_MAX_LINE_BYTES)) {
        if (app->analysis_input != NULL) fclose(app->analysis_input);
        app->analysis_input = NULL;
        set_error(app, "Cannot reopen the input file for analysis");
        return;
    }
    app->analysis_reader_ready = 1;
    if (app->format == FORMAT_JSONL) {
        if (!json_scratch_init(&app->analysis_scratch,
                               LOGVIEW_MAX_LINE_BYTES)) {
            set_error(app, "Cannot allocate JSON analysis memory");
            close_analysis(app);
            return;
        }
        app->analysis_scratch_ready = 1;
    }
    app->analysis_phase = 1;
    app->analysis_active = 1;
    app->error[0] = '\0';
    SDL_strlcpy(app->status, "Analyzing selected records...",
                sizeof(app->status));
}

static void process_analysis_slice(GuiApp *app)
{
    Uint64 started;
    size_t processed = 0;
    if (!app->analysis_active) return;
    started = SDL_GetTicksNS();
    while (processed < GUI_SCAN_BUDGET_ROWS &&
           SDL_GetTicksNS() - started < GUI_SCAN_BUDGET_NS) {
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result = app->format == FORMAT_JSONL ?
            reader_next_json(&app->analysis_reader, &line, &length,
                             &line_number, &byte_offset, NULL, NULL) :
            reader_next(&app->analysis_reader, &line, &length,
                        &line_number, &byte_offset);
        LogEntry entry;
        int matches;
        if (result == READER_EOF) {
            if (app->analysis_phase == 1 && app->analysis_stats.has_timestamp) {
                if (!reader_reposition(&app->analysis_reader, 0, 0)) {
                    set_error(app, "Cannot restart the timeline analysis pass");
                    close_analysis(app);
                    return;
                }
                app->analysis_phase = 2;
                app->analysis_physical = 0;
                SDL_strlcpy(app->status, "Building time histogram...",
                            sizeof(app->status));
                continue;
            }
            close_analysis(app);
            app->analysis_cache_valid =
                app->analysis_run_generation == app->analysis_generation;
            if (app->analysis_cache_valid) {
                app->analysis_cache_generation = app->analysis_generation;
                app->analysis_cache_format = app->format;
                app->analysis_cache_year =
                    app->analysis_stats.timestamp_config.syslog_year;
                app->analysis_cache_timezone_offset_seconds =
                    app->analysis_stats.timestamp_config
                        .syslog_timezone_offset_seconds;
                SDL_strlcpy(app->analysis_cache_field, app->analysis_field,
                            sizeof(app->analysis_cache_field));
            }
            SDL_strlcpy(app->status, app->analysis_stats.has_timestamp ?
                        "Analysis complete" :
                        "Analysis complete; no supported timestamps",
                        sizeof(app->status));
            return;
        }
        if (result == READER_ERROR) {
            set_error(app, "I/O error during record analysis");
            close_analysis(app);
            return;
        }
        processed++;
        app->analysis_physical++;
        if (result == READER_TOO_LONG || memchr(line, 0, length) != NULL) continue;
        if (!analysis_parse_entry(app, &entry, line, length,
                                  line_number, byte_offset)) {
            if (app->analysis_phase == 1) stats_rejected(&app->analysis_stats);
            continue;
        }
        if (app->analysis_phase == 1) stats_parsed(&app->analysis_stats);
        matches = record_matches_queries(app, &entry, line, length, 1);
        if (matches < 0) {
            set_error(app, "Query work limit exceeded during analysis");
            close_analysis(app);
            return;
        }
        if (!matches) continue;
        if (app->analysis_phase == 1) {
            if (!stats_selected(&app->analysis_stats, &entry)) {
                set_error(app, "Cannot update record analysis statistics");
                close_analysis(app);
                return;
            }
            app->analysis_selected++;
        } else {
            ByteSpan timestamp;
            int64_t epoch;
            if (log_entry_find_field(&entry, "timestamp", &timestamp) &&
                stats_timestamp_epoch_configured(
                    timestamp, &app->analysis_stats.timestamp_config, &epoch)) {
                size_t bin = stats_timeline_bin(
                    epoch, app->analysis_stats.earliest_timestamp,
                    app->analysis_stats.latest_timestamp);
                if (app->histogram_first_sequence[bin] == UINT64_MAX)
                    app->histogram_first_sequence[bin] =
                        app->analysis_result_sequence;
            }
            if (!stats_timeline_add(&app->analysis_stats, &entry)) {
                set_error(app, "Cannot update timeline histogram");
                close_analysis(app);
                return;
            }
            app->analysis_result_sequence++;
        }
    }
    SDL_snprintf(app->status, sizeof(app->status), "%s: %llu of %llu records",
                 app->analysis_phase == 1 ? "Analyzing" : "Building histogram",
                 (unsigned long long)app->analysis_physical,
                 (unsigned long long)app->physical_records);
}

static int span_matches_text(ByteSpan span, const char *text)
{
    size_t index;
    size_t length = SDL_strlen(text);
    if (span.length != length) return 0;
    for (index = 0; index < length; index++) {
        if (toupper((unsigned char)span.data[index]) !=
            toupper((unsigned char)text[index])) return 0;
    }
    return 1;
}

static int gui_record_level(const LogEntry *entry)
{
    ByteSpan value;
    if (log_entry_find_field(entry, "level", &value)) {
        if (span_matches_text(value, "ERROR") || span_matches_text(value, "ERR") ||
            span_matches_text(value, "FATAL") || span_matches_text(value, "SEVERE") ||
            span_matches_text(value, "CRIT") || span_matches_text(value, "CRITICAL") ||
            span_matches_text(value, "ALERT") || span_matches_text(value, "EMERG") ||
            span_matches_text(value, "EMERGENCY")) return 2;
        if (span_matches_text(value, "WARN") || span_matches_text(value, "WARNING"))
            return 1;
    }
    if (log_entry_find_field(entry, "status", &value) && value.length == 3 &&
        value.data[0] >= '0' && value.data[0] <= '9' &&
        value.data[1] >= '0' && value.data[1] <= '9' &&
        value.data[2] >= '0' && value.data[2] <= '9') {
        unsigned status = (unsigned)(value.data[0] - '0') * 100u +
                          (unsigned)(value.data[1] - '0') * 10u +
                          (unsigned)(value.data[2] - '0');
        if (status >= 500) return 2;
        if (status >= 400) return 1;
    }
    return 0;
}

static void start_level_navigation(GuiApp *app, int target, int direction)
{
    close_level_navigation(app);
    if (!app->scan_complete || app->selected_records == 0 ||
        !app->has_selection) {
        SDL_strlcpy(app->status, "Complete the scan before level navigation",
                    sizeof(app->status));
        return;
    }
    app->level_nav_input = open_utf8_file(app->path);
    if (app->level_nav_input == NULL ||
        !reader_init(&app->level_nav_reader, app->level_nav_input,
                     LOGVIEW_MAX_LINE_BYTES)) {
        if (app->level_nav_input != NULL) fclose(app->level_nav_input);
        app->level_nav_input = NULL;
        set_error(app, "Cannot reopen the input file for level navigation");
        return;
    }
    app->level_nav_reader_ready = 1;
    if (app->format == FORMAT_JSONL) {
        if (!json_scratch_init(&app->level_nav_scratch,
                               LOGVIEW_MAX_LINE_BYTES)) {
            set_error(app, "Cannot allocate JSON level-navigation memory");
            close_level_navigation(app);
            return;
        }
        app->level_nav_scratch_ready = 1;
    }
    app->level_nav_target = target;
    app->level_nav_direction = direction;
    app->level_nav_origin = app->selected_sequence;
    app->level_nav_sequence = app->selected_sequence;
    app->level_nav_checked = 0;
    app->level_nav_active = 1;
    SDL_strlcpy(app->status, target == 2 ? "Searching for an error record..." :
                "Searching for a warning record...", sizeof(app->status));
}

static void process_level_navigation(GuiApp *app)
{
    Uint64 started;
    size_t processed = 0;
    if (!app->level_nav_active) return;
    started = SDL_GetTicksNS();
    while (processed < 16 && SDL_GetTicksNS() - started < GUI_SCAN_BUDGET_NS) {
        GuiIndexEntry index_entry;
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result;
        LogEntry entry;
        uint64_t sequence;
        processed++;
        if (app->level_nav_direction > 0) {
            app->level_nav_sequence++;
            if (app->level_nav_sequence >= app->selected_records)
                app->level_nav_sequence = 0;
        } else {
            if (app->level_nav_sequence == 0)
                app->level_nav_sequence = app->selected_records - 1;
            else app->level_nav_sequence--;
        }
        sequence = app->level_nav_sequence;
        app->level_nav_checked++;
        if (!read_index_entry(app, sequence, &index_entry) ||
            !reader_reposition(&app->level_nav_reader, index_entry.byte_offset,
                               index_entry.line_number - 1)) {
            close_level_navigation(app);
            set_error(app, "Cannot read the result index for level navigation");
            return;
        }
        result = app->format == FORMAT_JSONL ?
            reader_next_json(&app->level_nav_reader, &line, &length,
                             &line_number, &byte_offset, NULL, NULL) :
            reader_next(&app->level_nav_reader, &line, &length,
                        &line_number, &byte_offset);
        if (result != READER_LINE || length == SIZE_MAX) continue;
        parse_gui_record(app, &entry, line, length, line_number, byte_offset,
                         app->format == FORMAT_JSONL ? &app->level_nav_scratch : NULL);
        if (gui_record_level(&entry) == app->level_nav_target) {
            close_level_navigation(app);
            if (sequence < app->view_start ||
                sequence >= app->view_start + GUI_PAGE_ROWS) {
                request_page(app, (sequence / GUI_PAGE_ROWS) * GUI_PAGE_ROWS);
                app->selected_sequence = sequence;
            } else {
                app->selected_sequence = sequence;
                clear_detail(app);
            }
            SDL_snprintf(app->status, sizeof(app->status),
                         "Jumped to %s record at source line %llu",
                         app->level_nav_target == 2 ? "error" : "warning",
                         (unsigned long long)index_entry.line_number);
            return;
        }
        if (app->level_nav_checked >= app->selected_records) {
            close_level_navigation(app);
            SDL_strlcpy(app->status, "No matching severity in selected records",
                        sizeof(app->status));
            return;
        }
    }
}

static void close_enrichment(GuiApp *app)
{
    app->enriching = 0;
    if (app->enrichment_reader_ready) {
        reader_destroy(&app->enrichment_reader);
        app->enrichment_reader_ready = 0;
    }
    if (app->enrichment_input != NULL) {
        fclose(app->enrichment_input);
        app->enrichment_input = NULL;
    }
}

static void close_export(GuiApp *app)
{
    app->exporting = 0;
    if (app->output_ready) {
        output_finish(&app->output);
        app->output_ready = 0;
    }
    if (app->export_reader_ready) {
        reader_destroy(&app->export_reader);
        app->export_reader_ready = 0;
    }
    if (app->export_input != NULL) {
        fclose(app->export_input);
        app->export_input = NULL;
    }
    if (app->export_scratch_ready) {
        json_scratch_destroy(&app->export_scratch);
        app->export_scratch_ready = 0;
    }
}

static void clear_detail(GuiApp *app)
{
    free(app->detail_data);
    free(app->pretty_data);
    app->detail_data = NULL;
    app->pretty_data = NULL;
    app->detail_length = 0;
    app->pretty_length = 0;
    app->detail_page = 0;
    app->detail_loaded = 0;
    app->detail_entry_valid = 0;
    memset(&app->detail_entry, 0, sizeof(app->detail_entry));
    app->detail_tab = 0;
    app->pretty_visible = 0;
}

static int build_pretty_detail(GuiApp *app)
{
    FILE *temporary;
    long file_size;
    size_t bytes_read;
    if (app->pretty_data != NULL) return 1;
    if (app->detail_data == NULL || app->detail_length == 0) return 0;
    temporary = tmpfile();
    if (temporary == NULL) return 0;
    if (!json_pretty_write(temporary, app->detail_data, app->detail_length,
                           LOGVIEW_MAX_LINE_BYTES) ||
        fseek(temporary, 0, SEEK_END) != 0 ||
        (file_size = ftell(temporary)) < 0 ||
        (uint64_t)file_size > LOGVIEW_MAX_LINE_BYTES ||
        fseek(temporary, 0, SEEK_SET) != 0) {
        fclose(temporary);
        return 0;
    }
    app->pretty_data = (unsigned char *)malloc((size_t)file_size + 1);
    if (app->pretty_data == NULL) {
        fclose(temporary);
        return 0;
    }
    bytes_read = fread(app->pretty_data, 1, (size_t)file_size, temporary);
    fclose(temporary);
    if (bytes_read != (size_t)file_size) {
        free(app->pretty_data);
        app->pretty_data = NULL;
        return 0;
    }
    app->pretty_data[bytes_read] = '\0';
    app->pretty_length = bytes_read;
    return 1;
}

static int detect_format(GuiApp *app, InputFormat *format)
{
    const unsigned char *lines[32];
    size_t lengths[32];
    unsigned char *copies[32] = {0};
    size_t count = 0;
    size_t total = 0;
    int success = 0;

    while (count < 32 && total < 256u * 1024u) {
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result = reader_next(&app->reader, &line, &length,
                                          &line_number, &byte_offset);
        (void)line_number;
        (void)byte_offset;
        if (result == READER_EOF) break;
        if (result == READER_ERROR) goto done;
        if (result == READER_TOO_LONG || length == 0) continue;
        if (length > 256u * 1024u - total) break;
        copies[count] = (unsigned char *)malloc(length);
        if (copies[count] == NULL) goto done;
        memcpy(copies[count], line, length);
        lines[count] = copies[count];
        lengths[count] = length;
        total += length;
        count++;
    }

    *format = parser_detect_format(lines, lengths, count);
    success = reader_reposition(&app->reader, 0, 0);

done:
    while (count != 0) free(copies[--count]);
    return success;
}

static int copy_span(char *destination, size_t capacity, ByteSpan span)
{
    size_t index;
    size_t output = 0;
    if (capacity == 0) return 0;
    for (index = 0; index < span.length && output + 1 < capacity; index++) {
        unsigned char value = span.data[index];
        destination[output++] = value >= 32 && value != 127 ? (char)value : ' ';
    }
    destination[output] = '\0';
    return span.length <= output;
}

static void make_preview(char *destination, size_t capacity,
                         const unsigned char *data, size_t length)
{
    size_t index;
    size_t output = 0;
    if (capacity == 0) return;
    for (index = 0; index < length && output + 1 < capacity; index++) {
        unsigned char value = data[index];
        destination[output++] = value >= 32 && value != 127 ? (char)value : ' ';
    }
    if (index < length && output > 3 && capacity > 4) {
        destination[output - 3] = '.';
        destination[output - 2] = '.';
        destination[output - 1] = '.';
    }
    destination[output] = '\0';
}

static size_t make_record_page(char *destination, size_t capacity,
                               const unsigned char *data, size_t length)
{
    size_t index;
    size_t output = 0;
    if (capacity == 0) return 0;
    for (index = 0; index < length && output + 1 < capacity; index++) {
        unsigned char value = data[index];
        destination[output++] = value == '\n' ? '\n' :
                                value == '\t' ? ' ' :
                                value >= 32 && value != 127 ? (char)value : ' ';
    }
    if (output != 0) {
        size_t start = output;
        unsigned char last = (unsigned char)destination[output - 1];
        while (start != 0 &&
               ((unsigned char)destination[start - 1] & 0xc0u) == 0x80u) {
            start--;
        }
        if (start < output) {
            size_t lead = start - 1;
            unsigned char first = (unsigned char)destination[lead];
            size_t expected = first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
            if (output - lead < expected) output = lead;
        } else if (last >= 0xc0) {
            output--;
        }
    }
    destination[output] = '\0';
    return output;
}

static int compile_queries(GuiApp *app)
{
    SearchQuery *query = NULL;
    Filter *filter = NULL;
    Regex *raw_regex = NULL;
    Regex *field_regex = NULL;
    Regex *exclude_regex = NULL;
    char error[192] = {0};

    if (app->selected_format == FORMAT_CUSTOM &&
        !parser_custom_validate(app->custom_pattern, error, sizeof(error))) {
        set_error(app, error[0] != '\0' ? error : "Invalid custom pattern");
        return 0;
    }

    if (app->search[0] != '\0') {
        query = search_query_compile(app->search, error, sizeof(error));
        if (query == NULL) {
            set_error(app, error[0] != '\0' ? error : "Invalid search query");
            return 0;
        }
    }
    if (app->filter_text[0] != '\0') {
        filter = filter_compile(app->filter_text, error, sizeof(error));
        if (filter == NULL) {
            search_query_free(query);
            set_error(app, error[0] != '\0' ? error : "Invalid field filter");
            return 0;
        }
    }
    if (app->regex_text[0] != '\0') {
        raw_regex = regex_compile(app->regex_text, error, sizeof(error));
        if (raw_regex == NULL) {
            search_query_free(query);
            filter_free(filter);
            set_error(app, error[0] != '\0' ? error : "Invalid regex");
            return 0;
        }
    }
    if (app->field_regex_text[0] != '\0') {
        const char *separator = strchr(app->field_regex_text, '=');
        size_t field_length;
        if (separator == NULL || separator == app->field_regex_text ||
            separator[1] == '\0') {
            search_query_free(query);
            filter_free(filter);
            regex_free(raw_regex);
            set_error(app, "Field regex must use field=pattern syntax");
            return 0;
        }
        field_length = (size_t)(separator - app->field_regex_text);
        if (field_length >= sizeof(app->regex_field_name)) {
            search_query_free(query);
            filter_free(filter);
            regex_free(raw_regex);
            set_error(app, "Field name is too long");
            return 0;
        }
        memcpy(app->regex_field_name, app->field_regex_text, field_length);
        app->regex_field_name[field_length] = '\0';
        field_regex = regex_compile(separator + 1, error, sizeof(error));
        if (field_regex == NULL) {
            search_query_free(query);
            filter_free(filter);
            regex_free(raw_regex);
            set_error(app, error[0] != '\0' ? error : "Invalid field regex");
            return 0;
        }
    } else {
        app->regex_field_name[0] = '\0';
    }
    if (app->exclude_regex_text[0] != '\0') {
        exclude_regex = regex_compile(app->exclude_regex_text, error,
                                      sizeof(error));
        if (exclude_regex == NULL) {
            search_query_free(query);
            filter_free(filter);
            regex_free(raw_regex);
            regex_free(field_regex);
            set_error(app, error[0] != '\0' ? error : "Invalid exclude regex");
            return 0;
        }
    }
    search_query_free(app->query);
    filter_free(app->filter);
    regex_free(app->raw_regex);
    regex_free(app->field_regex);
    regex_free(app->exclude_regex);
    app->query = query;
    app->filter = filter;
    app->raw_regex = raw_regex;
    app->field_regex = field_regex;
    app->exclude_regex = exclude_regex;
    app->error[0] = '\0';
    return 1;
}

static int record_matches_queries(GuiApp *app, LogEntry *entry,
                                  const unsigned char *line, size_t length,
                                  int apply_field_filter)
{
    RegexResult result;
    ByteSpan field;
    if (app->query != NULL) {
        result = search_query_match(app->query, line, length);
        if (result == REGEX_WORK_LIMIT) return -1;
        if (result != REGEX_MATCH) return 0;
    }
    if (app->raw_regex != NULL) {
        result = regex_search(app->raw_regex, line, length);
        if (result == REGEX_WORK_LIMIT) return -1;
        if (result != REGEX_MATCH) return 0;
    }
    if (app->exclude_regex != NULL) {
        result = regex_search(app->exclude_regex, line, length);
        if (result == REGEX_WORK_LIMIT) return -1;
        if (result == REGEX_MATCH) return 0;
    }
    if (apply_field_filter && app->filter != NULL) {
        int matched = filter_matches(app->filter, entry, &result);
        if (result == REGEX_WORK_LIMIT) return -1;
        if (!matched) return 0;
    }
    if (app->severity_filter_active) {
        size_t index;
        int severity = GUI_SEVERITY_COUNT - 1;
        if (log_entry_find_field(entry, "level", &field)) {
            for (index = 0; index < GUI_SEVERITY_COUNT - 1; index++) {
                size_t name_length = SDL_strlen(gui_severity_names[index]);
                if (field.length == name_length &&
                    SDL_strncasecmp((const char *)field.data,
                                    gui_severity_names[index], name_length) == 0) {
                    severity = (int)index;
                    break;
                }
            }
            if (severity == GUI_SEVERITY_COUNT - 1 &&
                field_equals_case(field, "WARNING"))
                severity = 4;
            if (severity == GUI_SEVERITY_COUNT - 1 &&
                (field_equals_case(field, "FATAL") ||
                 field_equals_case(field, "CRITICAL") ||
                 field_equals_case(field, "CRIT") ||
                 field_equals_case(field, "ALERT") ||
                 field_equals_case(field, "EMERG")))
                severity = 5;
        }
        if ((app->severity_filter_mask & (1u << severity)) == 0) return 0;
    }
    if (app->field_regex != NULL) {
        if (!log_entry_find_field(entry, app->regex_field_name, &field)) return 0;
        result = regex_search(app->field_regex, field.data, field.length);
        if (result == REGEX_WORK_LIMIT) return -1;
        if (result != REGEX_MATCH) return 0;
    }
    return 1;
}

static void start_scan(GuiApp *app)
{
    char status[256];
    invalidate_analysis_cache(app);
    close_export(app);
    close_analysis(app);
    close_level_navigation(app);
    close_enrichment(app);
    close_page_load(app);
    close_context_load(app);
    app->analysis_view = 0;
    close_scan(app);
    app->result_index_buffer_count = 0;
    app->scan_complete = 0;
    app->follow_waiting = 0;
    app->physical_records = 0;
    app->selected_records = 0;
    app->has_selection = 0;
    app->bookmark_count = 0;
    app->physical_checkpoint_count = 0;
    app->context_count = 0;
    app->context_visible = 0;
    app->bytes_read = 0;
    app->input_size = 0;
    app->view_start = 0;
    app->load_started_ns = SDL_GetTicksNS();
    app->scan_started_ns = 0;
    app->open_elapsed_ns = 0;
    app->scan_elapsed_ns = 0;
    app->enrichment_started_ns = 0;
    app->enrichment_elapsed_ns = 0;
    clear_detail(app);
    app->error[0] = '\0';

    if (app->path[0] == '\0') {
        set_error(app, "Choose a log file to open");
        return;
    }
    if (!compile_queries(app)) return;

    app->input = open_utf8_file(app->path);
    if (app->input == NULL) {
        SDL_snprintf(status, sizeof(status), "Cannot open file: %s",
                     strerror(errno));
        set_error(app, status);
        return;
    }
    if (!reader_init(&app->reader, app->input, LOGVIEW_MAX_LINE_BYTES)) {
        set_error(app, "Cannot initialize the log reader");
        close_scan(app);
        return;
    }
    app->reader_ready = 1;
    app->result_index = tmpfile();
    app->physical_index = tmpfile();
    if (app->result_index == NULL || app->physical_index == NULL) {
        set_error(app, "Cannot create a temporary result index");
        close_scan(app);
        return;
    }
    setvbuf(app->result_index, NULL, _IOFBF, 1024u * 1024u);
    setvbuf(app->physical_index, NULL, _IOFBF, 256u * 1024u);
    {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(app->path, &info) &&
            info.type == SDL_PATHTYPE_FILE) {
            app->input_size = info.size;
        }
    }
    app->format = app->selected_format;
    if (app->format == FORMAT_AUTO && !detect_format(app, &app->format)) {
        set_error(app, "Cannot detect the input format");
        close_scan(app);
        return;
    }
    if (app->format == FORMAT_APACHE_COMMON ||
        app->format == FORMAT_APACHE_COMBINED) {
        InputFormat detected;
        if (detect_format(app, &detected) && detected == FORMAT_APACHE_ERROR) {
            app->format = detected;
            app->selected_format = detected;
        }
    }
    if (app->format == FORMAT_JSONL) {
        if (!json_scratch_init(&app->json_scratch, LOGVIEW_MAX_LINE_BYTES)) {
            set_error(app, "Cannot allocate JSON parsing memory");
            close_scan(app);
            return;
        }
        app->scratch_ready = 1;
    }
    app->open_elapsed_ns = SDL_GetTicksNS() - app->load_started_ns;
    app->scan_started_ns = SDL_GetTicksNS();
    app->scanning = 1;
    SDL_snprintf(app->status, sizeof(app->status), "Scanning %s",
                 format_label(app->format));
}

static void start_export(GuiApp *app, const char *destination)
{
    char output_error[192] = {0};
    close_export(app);
    app->export_physical_records = 0;
    app->export_selected_records = 0;
    if (!app->scan_complete || app->path[0] == '\0') {
        set_error(app, "Finish scanning before exporting the selected results");
        return;
    }
    app->export_input = open_utf8_file(app->path);
    if (app->export_input == NULL ||
        !reader_init(&app->export_reader, app->export_input,
                     LOGVIEW_MAX_LINE_BYTES)) {
        if (app->export_input != NULL) fclose(app->export_input);
        app->export_input = NULL;
        set_error(app, "Cannot reopen the input file for export");
        return;
    }
    app->export_reader_ready = 1;
    if (app->format == FORMAT_JSONL) {
        if (!json_scratch_init(&app->export_scratch, LOGVIEW_MAX_LINE_BYTES)) {
            set_error(app, "Cannot allocate JSON parsing memory for export");
            close_export(app);
            return;
        }
        app->export_scratch_ready = 1;
    }
    if (!output_init_utf8(&app->output, destination, app->path,
                          app->export_format,
                          app->export_columns[0] != '\0' ?
                              app->export_columns : NULL,
                          0, 0,
                          output_error, sizeof(output_error))) {
        if (app->output.stream != NULL) {
            output_finish(&app->output);
        }
        set_error(app, output_error[0] != '\0' ? output_error :
                  "Cannot create the export file");
        close_export(app);
        return;
    }
    app->output_ready = 1;
    app->exporting = 1;
    SDL_strlcpy(app->status, "Exporting selected records...",
                sizeof(app->status));
    app->error[0] = '\0';
}

static ReaderResult read_next(GuiApp *app, const unsigned char **line,
                              size_t *length, uint64_t *line_number,
                              uint64_t *byte_offset)
{
    if (app->format == FORMAT_JSONL) {
        return reader_next_json(&app->reader, line, length, line_number,
                                byte_offset, NULL, NULL);
    }
    return reader_next(&app->reader, line, length, line_number, byte_offset);
}

static void process_export_slice(GuiApp *app)
{
    Uint64 start;
    size_t processed = 0;
    if (!app->exporting) return;
    start = SDL_GetTicksNS();
    while (processed < GUI_SCAN_BUDGET_ROWS &&
           SDL_GetTicksNS() - start < GUI_SCAN_BUDGET_NS) {
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result = app->format == FORMAT_JSONL ?
            reader_next_json(&app->export_reader, &line, &length, &line_number,
                             &byte_offset, NULL, NULL) :
            reader_next(&app->export_reader, &line, &length, &line_number,
                        &byte_offset);
        LogEntry entry;
        int matches = 1;

        if (result == READER_EOF) {
            app->exporting = 0;
            if (!output_finish(&app->output)) {
                set_error(app, "Could not finish writing the export file");
            } else {
                SDL_snprintf(app->status, sizeof(app->status),
                             "Export complete: %llu records",
                             (unsigned long long)app->export_selected_records);
            }
            app->output_ready = 0;
            close_export(app);
            return;
        }
        if (result == READER_ERROR) {
            set_error(app, "I/O error while reading records for export");
            close_export(app);
            return;
        }
        processed++;
        app->export_physical_records++;
        if (result == READER_TOO_LONG || memchr(line, 0, length) != NULL) continue;
        parse_gui_record(app, &entry, line, length, line_number, byte_offset,
                         app->format == FORMAT_JSONL ? &app->export_scratch : NULL);
        matches = record_matches_queries(app, &entry, line, length, 1);
        if (matches < 0) {
            set_error(app, "Query work limit exceeded during export");
            close_export(app);
            return;
        }
        if (matches) {
            if (!output_write(&app->output, &entry)) {
                set_error(app, "Could not write the export file");
                close_export(app);
                return;
            }
            app->export_selected_records++;
        }
    }
    SDL_snprintf(app->status, sizeof(app->status),
                 "Exporting: %llu of %llu records read",
                 (unsigned long long)app->export_physical_records,
                 (unsigned long long)app->physical_records);
}

static int cache_record(GuiApp *app, const LogEntry *entry)
{
    uint64_t sequence = app->selected_records;
    GuiRow *row = &app->rows[sequence % GUI_ROW_CAPACITY];
    memset(row, 0, sizeof(*row));
    row->sequence = sequence;
    row->line_number = entry->line_number;
    row->byte_offset = entry->byte_offset;
    make_preview(row->raw, sizeof(row->raw), entry->raw.data, entry->raw.length);
    populate_row_fields(row, entry);
    if (app->result_index == NULL) return 0;
    {
        GuiIndexEntry index_entry;
        index_entry.line_number = entry->line_number;
        index_entry.byte_offset = entry->byte_offset;
        if (app->result_index_buffer_count == GUI_RESULT_INDEX_BUFFER &&
            !flush_result_index_buffer(app)) return 0;
        app->result_index_buffer[app->result_index_buffer_count++] = index_entry;
        if (app->result_index_buffer_count == GUI_RESULT_INDEX_BUFFER &&
            !flush_result_index_buffer(app)) return 0;
    }
    if (!app->has_selection) {
        app->selected_sequence = sequence;
        app->has_selection = 1;
    }
    app->selected_records++;
    if (app->scanning && app->selected_records > GUI_PAGE_ROWS)
        app->view_start = app->selected_records - GUI_PAGE_ROWS;
    return 1;
}

static void process_scan_slice(GuiApp *app)
{
    Uint64 start;
    size_t processed = 0;
    if (!app->scanning) return;
    start = SDL_GetTicksNS();
    while (processed < GUI_SCAN_SLICE_ROWS &&
           SDL_GetTicksNS() - start < GUI_SCAN_SLICE_NS) {
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result = read_next(app, &line, &length, &line_number,
                                        &byte_offset);
        LogEntry entry;
        int matches = 1;

        if (result == READER_EOF) {
            if (!flush_result_index_buffer(app)) {
                set_error(app, "Cannot write the temporary result index");
                app->scanning = 0;
                app->scan_elapsed_ns = SDL_GetTicksNS() - app->scan_started_ns;
                return;
            }
            app->scanning = 0;
            app->scan_complete = 1;
            app->scan_elapsed_ns = SDL_GetTicksNS() - app->scan_started_ns;
            if (app->follow_enabled) {
                app->follow_observed_end = app->reader.next_offset;
                app->follow_resume_offset = app->reader.next_offset;
                app->follow_line_number_base = app->reader.next_line_number;
                app->follow_pending_partial = 0;
                app->follow_waiting = 1;
                app->follow_last_poll_ns = 0;
                SDL_strlcpy(app->status, "Following file; waiting for new records",
                            sizeof(app->status));
            }
            if (app->selected_records != 0) {
                app->selected_sequence = app->selected_records - 1;
                app->view_start = app->selected_records > GUI_PAGE_ROWS ?
                                  app->selected_records - GUI_PAGE_ROWS : 0;
                app->has_selection = 1;
            }
            start_enrichment(app);
            return;
        }
        if (result == READER_ERROR) {
            set_error(app, "An I/O error occurred while scanning the file");
            app->scan_elapsed_ns = SDL_GetTicksNS() - app->scan_started_ns;
            app->scanning = 0;
            return;
        }
        if (app->follow_enabled && result == READER_LINE &&
            !app->reader.line_terminated) {
            app->follow_observed_end = app->reader.next_offset;
            app->follow_resume_offset = byte_offset;
            app->follow_line_number_base = line_number - 1;
            app->follow_pending_partial = 1;
            if (!reader_reposition(&app->reader, byte_offset, line_number - 1)) {
                set_error(app, "Cannot retain an incomplete followed record");
                app->scanning = 0;
                return;
            }
            app->scanning = 0;
            app->scan_complete = 1;
            app->follow_waiting = 1;
            app->follow_last_poll_ns = 0;
            SDL_strlcpy(app->status, "Following file; waiting for the line to finish",
                        sizeof(app->status));
            return;
        }
        processed++;
        app->bytes_read = app->reader.next_offset;
        if (app->input_size != 0 && app->bytes_read > app->input_size)
            app->bytes_read = app->input_size;
        if (result == READER_TOO_LONG || memchr(line, 0, length) != NULL) {
            app->physical_records++;
            if ((app->physical_records - 1) % 128u == 0 &&
                !append_physical_checkpoint(app, line_number, byte_offset)) {
                set_error(app, "Cannot write the temporary context index");
                app->scanning = 0;
                return;
            }
            continue;
        }
        app->physical_records++;
        if ((app->physical_records - 1) % 128u == 0 &&
            !append_physical_checkpoint(app, line_number, byte_offset)) {
            set_error(app, "Cannot write the temporary context index");
            app->scanning = 0;
            return;
        }
        if (app->filter != NULL || app->field_regex != NULL ||
            app->severity_filter_active) {
            parse_gui_record(app, &entry, line, length, line_number, byte_offset,
                             app->format == FORMAT_JSONL ? &app->json_scratch : NULL);
        } else {
            memset(&entry, 0, sizeof(entry));
            entry.raw.data = line;
            entry.raw.length = length;
            entry.line_number = line_number;
            entry.byte_offset = byte_offset;
            entry.parse_ok = app->format == FORMAT_RAW;
        }
        matches = record_matches_queries(app, &entry, line, length, 1);
        if (matches < 0) {
            set_error(app, "Query work limit exceeded");
            app->scan_elapsed_ns = SDL_GetTicksNS() - app->scan_started_ns;
            app->scanning = 0;
            return;
        }
        if (matches && !cache_record(app, &entry)) {
            set_error(app, "Cannot write the temporary result index");
            app->scanning = 0;
            return;
        }
    }
    if (app->input_size != 0) {
        double percent = 100.0 * (double)app->bytes_read /
                         (double)app->input_size;
        SDL_snprintf(app->status, sizeof(app->status),
                     "Scanning %s  |  %.1f%%  |  %llu records",
                     format_label(app->format), percent,
                     (unsigned long long)app->physical_records);
    } else {
        SDL_snprintf(app->status, sizeof(app->status),
                     "Scanning %s  |  %llu records",
                     format_label(app->format),
                     (unsigned long long)app->physical_records);
    }
}

static void format_elapsed(Uint64 elapsed_ns, char *buffer, size_t capacity)
{
    double seconds = (double)elapsed_ns / 1000000000.0;
    if (seconds < 10.0) {
        SDL_snprintf(buffer, capacity, "%.2f s", seconds);
    } else if (seconds < 60.0) {
        SDL_snprintf(buffer, capacity, "%.1f s", seconds);
    } else {
        SDL_snprintf(buffer, capacity, "%u min %02u s",
                     (unsigned)(seconds / 60.0),
                     (unsigned)seconds % 60u);
    }
}

static void process_follow_slice(GuiApp *app)
{
    Uint64 now;
    FollowResult result;
    if (!app->follow_enabled || !app->follow_waiting || app->input == NULL ||
        !app->reader_ready) return;
    now = SDL_GetTicksNS();
    if (app->follow_last_poll_ns != 0 &&
        now - app->follow_last_poll_ns < 250000000u) return;
    app->follow_last_poll_ns = now;
    if (!reader_reposition(&app->reader, app->follow_resume_offset,
                           app->follow_line_number_base)) {
        set_error(app, "Cannot reposition the reader while following");
        app->follow_enabled = 0;
        app->follow_waiting = 0;
        return;
    }
    result = follow_poll(&app->input, app->path, &app->reader,
                         app->follow_observed_end,
                         app->follow_pending_partial,
                         &app->follow_line_number_base);
    if (result == FOLLOW_ERROR || result == FOLLOW_STOPPED) {
        set_error(app, "I/O error while following the log file");
        app->follow_enabled = 0;
        app->follow_waiting = 0;
    } else if (result == FOLLOW_RESTART) {
        app->follow_waiting = 0;
        app->bookmark_count = 0;
        start_scan(app);
        SDL_strlcpy(app->status, "Log rotated or truncated; rescanning",
                    sizeof(app->status));
    } else if (result == FOLLOW_READY) {
        invalidate_analysis_cache(app);
        app->follow_waiting = 0;
        app->follow_pending_partial = 0;
        app->scanning = 1;
        app->scan_complete = 0;
        app->scan_started_ns = now;
        {
            SDL_PathInfo info;
            if (SDL_GetPathInfo(app->path, &info) &&
                info.type == SDL_PATHTYPE_FILE) app->input_size = info.size;
        }
        SDL_strlcpy(app->status, "Reading appended records...",
                    sizeof(app->status));
    } else {
        SDL_strlcpy(app->status, "Following file; waiting for new records",
                    sizeof(app->status));
    }
}

static GuiRow *cached_row(GuiApp *app, uint64_t sequence)
{
    GuiRow *row;
    if (sequence >= app->selected_records) return NULL;
    row = &app->rows[sequence % GUI_ROW_CAPACITY];
    return row->sequence == sequence ? row : NULL;
}

static int sequence_is_bookmarked(const GuiApp *app, uint64_t sequence)
{
    size_t index;
    for (index = 0; index < app->bookmark_count; index++)
        if (app->bookmark_sequences[index] == sequence) return 1;
    return 0;
}

static void toggle_selected_bookmark(GuiApp *app)
{
    size_t index;
    if (!app->has_selection) return;
    for (index = 0; index < app->bookmark_count; index++) {
        if (app->bookmark_sequences[index] == app->selected_sequence) {
            app->bookmark_count--;
            app->bookmark_sequences[index] =
                app->bookmark_sequences[app->bookmark_count];
            SDL_strlcpy(app->status, "Bookmark removed", sizeof(app->status));
            return;
        }
    }
    if (app->bookmark_count == GUI_BOOKMARK_CAPACITY) {
        SDL_strlcpy(app->status, "Bookmark limit reached", sizeof(app->status));
        return;
    }
    app->bookmark_sequences[app->bookmark_count++] = app->selected_sequence;
    SDL_strlcpy(app->status, "Bookmark added", sizeof(app->status));
}

static void request_page(GuiApp *app, uint64_t start)
{
    uint64_t end;
    close_page_load(app);
    if (app->selected_records == 0) return;
    if (start >= app->selected_records) start = app->selected_records - 1;
    app->view_start = start;
    end = start + GUI_PAGE_ROWS;
    if (end < start || end > app->selected_records) end = app->selected_records;
    app->page_load_next = start;
    app->page_load_end = end;
    app->page_input = open_utf8_file(app->path);
    if (app->page_input == NULL ||
        !reader_init(&app->page_reader, app->page_input,
                     LOGVIEW_MAX_LINE_BYTES)) {
        if (app->page_input != NULL) fclose(app->page_input);
        app->page_input = NULL;
        set_error(app, "Cannot open the source file for page navigation");
        return;
    }
    app->page_reader_ready = 1;
    app->page_loading = 1;
    app->selected_sequence = start;
    app->has_selection = 1;
    clear_detail(app);
}

static void start_context_load(GuiApp *app)
{
    GuiRow *selected;
    uint64_t low = 0;
    uint64_t high = app->physical_checkpoint_count;
    uint64_t checkpoint;
    uint64_t context_start_line;
    GuiIndexEntry item;
    close_context_load(app);
    app->context_count = 0;
    if (!app->has_selection || app->scanning) return;
    selected = cached_row(app, app->selected_sequence);
    if (selected == NULL) return;
    context_start_line = selected->line_number > app->context_before ?
                         selected->line_number - app->context_before : 1;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (!read_physical_checkpoint(app, middle, &item)) {
            set_error(app, "Cannot read the temporary context index");
            return;
        }
        if (item.line_number <= context_start_line) low = middle + 1;
        else high = middle;
    }
    checkpoint = low == 0 ? 0 : low - 1;
    if (!read_physical_checkpoint(app, checkpoint, &item)) {
        set_error(app, "Cannot read the temporary context index");
        return;
    }
    app->context_input = open_utf8_file(app->path);
    if (app->context_input == NULL ||
        !reader_init(&app->context_reader, app->context_input,
                     LOGVIEW_MAX_LINE_BYTES)) {
        if (app->context_input != NULL) fclose(app->context_input);
        app->context_input = NULL;
        set_error(app, "Cannot open the source file for context");
        return;
    }
    app->context_reader_ready = 1;
    if (!reader_reposition(&app->context_reader, item.byte_offset,
                           item.line_number - 1)) {
        set_error(app, "Cannot seek to the selected record context");
        close_context_load(app);
        return;
    }
    app->context_anchor_line = selected->line_number;
    app->context_scan_line = item.line_number;
    app->context_target_sequence = app->selected_sequence;
    app->context_before_count = 0;
    app->context_visible = 1;
    app->context_loading = 1;
    SDL_strlcpy(app->status, "Loading nearby source records...",
                sizeof(app->status));
}

static void process_context_load(GuiApp *app)
{
    Uint64 started;
    unsigned int processed = 0;
    if (!app->context_loading) return;
    started = SDL_GetTicksNS();
    while (processed < 128 && SDL_GetTicksNS() - started < GUI_SCAN_BUDGET_NS) {
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result = reader_next(&app->context_reader, &line, &length,
                                          &line_number, &byte_offset);
        GuiContextRow item;
        (void)byte_offset;
        processed++;
        if (result == READER_ERROR || result == READER_EOF) {
            close_context_load(app);
            break;
        }
        if (result == READER_TOO_LONG) continue;
        memset(&item, 0, sizeof(item));
        item.line_number = line_number;
        make_preview(item.text, sizeof(item.text), line, length);
        if (line_number < app->context_anchor_line) {
            if (app->context_before != 0) {
                if (app->context_before_count == app->context_before) {
                    memmove(app->context_before_rows,
                            app->context_before_rows + 1,
                            (app->context_before_count - 1) *
                            sizeof(app->context_before_rows[0]));
                    app->context_before_count--;
                }
                if (app->context_before_count < GUI_CONTEXT_SIDE_MAX)
                    app->context_before_rows[app->context_before_count++] = item;
            }
            continue;
        }
        if (app->context_count == 0) {
            size_t index;
            for (index = 0; index < app->context_before_count; index++)
                app->context_rows[app->context_count++] =
                    app->context_before_rows[index];
        }
        if (app->context_count < GUI_CONTEXT_ROWS)
            app->context_rows[app->context_count++] = item;
        if (line_number >= app->context_anchor_line + app->context_after) {
            close_context_load(app);
            SDL_strlcpy(app->status, "Nearby source records loaded",
                        sizeof(app->status));
            break;
        }
    }
    app->context_scan_line = app->context_reader_ready ?
        app->context_reader.next_line_number : app->context_scan_line;
}

static void process_page_load(GuiApp *app)
{
    Uint64 start;
    unsigned int processed = 0;
    if (!app->page_loading) return;
    start = SDL_GetTicksNS();
    while (app->page_load_next < app->page_load_end && processed < 16 &&
           SDL_GetTicksNS() - start < GUI_SCAN_BUDGET_NS) {
        uint64_t sequence = app->page_load_next++;
        GuiIndexEntry index_entry;
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result;
        LogEntry entry;
        GuiRow *row;
        processed++;
        if (!read_index_entry(app, sequence, &index_entry) ||
            !reader_reposition(&app->page_reader, index_entry.byte_offset,
                               index_entry.line_number - 1)) {
            set_error(app, "Cannot read the temporary result index");
            close_page_load(app);
            return;
        }
        result = app->format == FORMAT_JSONL ?
            reader_next_json(&app->page_reader, &line, &length, &line_number,
                             &byte_offset, NULL, NULL) :
            reader_next(&app->page_reader, &line, &length, &line_number,
                        &byte_offset);
        if (result != READER_LINE || length == SIZE_MAX) continue;
        parse_gui_record(app, &entry, line, length, line_number, byte_offset,
                         app->format == FORMAT_JSONL ? &app->json_scratch : NULL);
        row = &app->rows[sequence % GUI_ROW_CAPACITY];
        memset(row, 0, sizeof(*row));
        row->sequence = sequence;
        row->line_number = line_number;
        row->byte_offset = byte_offset;
        make_preview(row->raw, sizeof(row->raw), line, length);
        populate_row_fields(row, &entry);
    }
    if (app->page_load_next >= app->page_load_end) {
        close_page_load(app);
        SDL_snprintf(app->status, sizeof(app->status),
                     "Showing matching records %llu-%llu of %llu",
                     (unsigned long long)(app->view_start + 1),
                     (unsigned long long)(app->page_load_end),
                     (unsigned long long)app->selected_records);
        if (app->context_visible &&
            app->context_target_sequence != app->selected_sequence)
            start_context_load(app);
    } else {
        SDL_snprintf(app->status, sizeof(app->status),
                     "Loading page: %llu of %llu records",
                     (unsigned long long)(app->page_load_next - app->view_start),
                     (unsigned long long)(app->page_load_end - app->view_start));
    }
}

static void start_enrichment(GuiApp *app)
{
    GuiRow *first_row;
    if (app->format == FORMAT_RAW ||
        app->filter != NULL || app->selected_records == 0) {
        SDL_strlcpy(app->status, "Scan complete", sizeof(app->status));
        return;
    }
    app->enrichment_start = app->selected_records > GUI_ROW_CAPACITY ?
                            app->selected_records - GUI_ROW_CAPACITY : 0;
    app->enrichment_sequence = app->enrichment_start;
    app->enrichment_end = app->selected_records;
    first_row = cached_row(app, app->enrichment_start);
    if (first_row == NULL) {
        SDL_strlcpy(app->status, "Scan complete", sizeof(app->status));
        return;
    }
    app->enrichment_input = open_utf8_file(app->path);
    if (app->enrichment_input == NULL ||
        !reader_init(&app->enrichment_reader, app->enrichment_input,
                     LOGVIEW_MAX_LINE_BYTES)) {
        if (app->enrichment_input != NULL) fclose(app->enrichment_input);
        app->enrichment_input = NULL;
        set_error(app, "Could not prepare the recent record details");
        SDL_strlcpy(app->status, "Scan complete", sizeof(app->status));
        return;
    }
    app->enrichment_reader_ready = 1;
    app->enriching = 1;
    app->enrichment_started_ns = SDL_GetTicksNS();
    SDL_snprintf(app->status, sizeof(app->status),
                 "Preparing recent record details: 0 of %llu",
                 (unsigned long long)(app->enrichment_end -
                                      app->enrichment_start));
}

static void process_enrichment_slice(GuiApp *app)
{
    Uint64 start;
    size_t processed = 0;
    if (!app->enriching) return;
    start = SDL_GetTicksNS();
    while (app->enrichment_sequence < app->enrichment_end && processed < 24 &&
           SDL_GetTicksNS() - start < GUI_SCAN_BUDGET_NS) {
        GuiRow *row = cached_row(app, app->enrichment_sequence);
        const unsigned char *line;
        size_t length;
        uint64_t line_number;
        uint64_t byte_offset;
        ReaderResult result;
        LogEntry entry;

        app->enrichment_sequence++;
        processed++;
        if (row == NULL) continue;
        if (!reader_reposition(&app->enrichment_reader, row->byte_offset,
                               row->line_number - 1)) {
            set_error(app, "Could not seek to a recent matching record");
            close_enrichment(app);
            return;
        }
        result = app->format == FORMAT_JSONL ?
            reader_next_json(&app->enrichment_reader, &line, &length,
                             &line_number, &byte_offset, NULL, NULL) :
            reader_next(&app->enrichment_reader, &line, &length,
                        &line_number, &byte_offset);
        if (result != READER_LINE || length == SIZE_MAX) continue;

        parse_gui_record(app, &entry, line, length, line_number, byte_offset,
                         app->format == FORMAT_JSONL ? &app->json_scratch : NULL);
        populate_row_fields(row, &entry);
    }
    if (app->enrichment_sequence >= app->enrichment_end) {
        app->enrichment_elapsed_ns = SDL_GetTicksNS() -
                                     app->enrichment_started_ns;
        close_enrichment(app);
        SDL_strlcpy(app->status, "Scan complete", sizeof(app->status));
    } else {
        SDL_snprintf(app->status, sizeof(app->status),
                     "Preparing recent record details: %llu of %llu",
                     (unsigned long long)(app->enrichment_sequence -
                                          app->enrichment_start),
                     (unsigned long long)(app->enrichment_end -
                                          app->enrichment_start));
    }
}

static void refresh_selected_detail(GuiApp *app)
{
    GuiRow *row;
    FILE *file;
    LineReader reader;
    LogEntry entry;
    const unsigned char *line;
    size_t length;
    uint64_t line_number;
    uint64_t byte_offset;
    ReaderResult result;

    if (!app->has_selection || app->scanning || app->page_loading ||
        (app->detail_loaded && app->detail_sequence == app->selected_sequence)) return;
    row = cached_row(app, app->selected_sequence);
    if (row == NULL) {
        clear_detail(app);
        set_error(app, "This record has left the bounded recent-record view");
        return;
    }
    clear_detail(app);
    file = open_utf8_file(app->path);
    if (file == NULL || !reader_init(&reader, file, LOGVIEW_MAX_LINE_BYTES)) {
        if (file != NULL) fclose(file);
        set_error(app, "Cannot reopen the input file for full-record inspection");
        return;
    }
    if (!reader_reposition(&reader, row->byte_offset, row->line_number - 1)) {
        reader_destroy(&reader);
        fclose(file);
        set_error(app, "Cannot seek to the selected source record");
        return;
    }
    if (app->format == FORMAT_JSONL) {
        result = reader_next_json(&reader, &line, &length, &line_number,
                                 &byte_offset, NULL, NULL);
    } else {
        result = reader_next(&reader, &line, &length, &line_number, &byte_offset);
    }
    if (result != READER_LINE || length == SIZE_MAX) {
        reader_destroy(&reader);
        fclose(file);
        set_error(app, "Cannot read the complete selected record");
        return;
    }
    app->detail_data = (unsigned char *)malloc(length + 1);
    if (app->detail_data == NULL) {
        reader_destroy(&reader);
        fclose(file);
        set_error(app, "Cannot allocate memory for the selected record");
        return;
    }
    memcpy(app->detail_data, line, length);
    app->detail_data[length] = '\0';
    app->detail_length = length;
    app->detail_sequence = app->selected_sequence;
    app->detail_loaded = 1;
    parse_gui_record(app, &entry, app->detail_data, length, line_number,
                     byte_offset,
                     app->format == FORMAT_JSONL ? &app->json_scratch : NULL);
    app->detail_entry = entry;
    app->detail_entry_valid = 1;
    row = cached_row(app, app->selected_sequence);
    if (row != NULL) {
        populate_row_fields(row, &entry);
    }
    app->error[0] = '\0';
    reader_destroy(&reader);
    fclose(file);
}

static void show_open_dialog(GuiApp *app)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Log and text files", "log;txt;json;jsonl;csv;syslog"},
        {"All files", "*"}
    };
    if (app->open_dialog_pending) return;
    app->open_dialog_pending = 1;
    app->dialog_action = 0;
    SDL_ShowOpenFileDialog(file_dialog_callback, app, app->window, filters,
                           (int)SDL_arraysize(filters), NULL, false);
}

static void show_save_dialog(GuiApp *app)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Text files", "txt;log"},
        {"CSV files", "csv"},
        {"All files", "*"}
    };
    if (app->open_dialog_pending || !app->scan_complete || app->scanning ||
        app->exporting) return;
    app->open_dialog_pending = 1;
    app->dialog_action = 1;
    SDL_ShowSaveFileDialog(file_dialog_callback, app, app->window, filters,
                           (int)SDL_arraysize(filters), NULL);
}

static void show_profile_open_dialog(GuiApp *app)
{
    static const SDL_DialogFileFilter filters[] = {
        {"GLogViewer query profiles", "glogprofile"},
        {"All files", "*"}
    };
    if (app->open_dialog_pending || app->scanning || app->exporting) return;
    app->open_dialog_pending = 1;
    app->dialog_action = 2;
    SDL_ShowOpenFileDialog(file_dialog_callback, app, app->window, filters,
                           (int)SDL_arraysize(filters), NULL, false);
}

static void show_profile_save_dialog(GuiApp *app)
{
    static const SDL_DialogFileFilter filters[] = {
        {"GLogViewer query profiles", "glogprofile"},
        {"All files", "*"}
    };
    if (app->open_dialog_pending || app->scanning || app->exporting) return;
    app->open_dialog_pending = 1;
    app->dialog_action = 3;
    SDL_ShowSaveFileDialog(file_dialog_callback, app, app->window, filters,
                           (int)SDL_arraysize(filters), NULL);
}

static float measure_label(GuiApp *app, const char *text)
{
    const struct nk_user_font *font = app->nk->style.font;
    if (font == NULL || font->width == NULL) return (float)SDL_strlen(text) * 9.0f;
    return font->width(font->userdata, font->height, text,
                       (int)SDL_strlen(text));
}

static struct nk_color level_color(const char *level)
{
    if (SDL_strcasecmp(level, "ERROR") == 0 ||
        SDL_strcasecmp(level, "FATAL") == 0 ||
        SDL_strcasecmp(level, "CRITICAL") == 0)
        return nk_rgb(255, 128, 120);
    if (SDL_strcasecmp(level, "WARN") == 0 ||
        SDL_strcasecmp(level, "WARNING") == 0)
        return nk_rgb(255, 194, 74);
    if (SDL_strcasecmp(level, "DEBUG") == 0 ||
        SDL_strcasecmp(level, "TRACE") == 0)
        return nk_rgb(76, 174, 255);
    return nk_rgb(77, 211, 133);
}

static void push_level_badge(GuiApp *app, const char *level)
{
    struct nk_color background;
    if (SDL_strcasecmp(level, "ERROR") == 0 ||
        SDL_strcasecmp(level, "FATAL") == 0 ||
        SDL_strcasecmp(level, "CRITICAL") == 0)
        background = nk_rgb(91, 43, 48);
    else if (SDL_strcasecmp(level, "WARN") == 0 ||
             SDL_strcasecmp(level, "WARNING") == 0)
        background = nk_rgb(91, 69, 32);
    else if (SDL_strcasecmp(level, "DEBUG") == 0 ||
             SDL_strcasecmp(level, "TRACE") == 0)
        background = nk_rgb(29, 67, 93);
    else
        background = nk_rgb(35, 76, 58);
    nk_style_push_style_item(app->nk, &app->nk->style.button.normal,
                             nk_style_item_color(background));
    nk_style_push_style_item(app->nk, &app->nk->style.button.hover,
                             nk_style_item_color(background));
    nk_style_push_style_item(app->nk, &app->nk->style.button.active,
                             nk_style_item_color(background));
    nk_style_push_color(app->nk, &app->nk->style.button.text_normal,
                        level_color(level));
    nk_style_push_color(app->nk, &app->nk->style.button.text_hover,
                        level_color(level));
    nk_style_push_color(app->nk, &app->nk->style.button.text_active,
                        level_color(level));
}

static void pop_level_badge(GuiApp *app)
{
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    nk_style_pop_style_item(app->nk);
    nk_style_pop_style_item(app->nk);
    nk_style_pop_style_item(app->nk);
}

static void begin_result_table_row(GuiApp *app, float row_height,
                                   float time_width, float level_width,
                                   float source_width)
{
    nk_layout_row_template_begin(app->nk, row_height);
    if (app->show_timestamp_column)
        nk_layout_row_template_push_static(app->nk, time_width);
    if (app->show_level_column)
        nk_layout_row_template_push_static(app->nk, level_width);
    if (app->show_source_column)
        nk_layout_row_template_push_static(app->nk, source_width);
    nk_layout_row_template_push_dynamic(app->nk);
    nk_layout_row_template_end(app->nk);
}

static nk_bool selectable_bookmark_label(GuiApp *app, const char *text,
                                         nk_bool *selected, int bookmarked)
{
    nk_bool clicked;
    if (!bookmarked)
        return nk_selectable_label(app->nk, text, NK_TEXT_LEFT, selected);
    nk_style_push_color(app->nk, &app->nk->style.selectable.text_normal,
                        nk_rgb(187, 247, 208));
    nk_style_push_color(app->nk, &app->nk->style.selectable.text_hover,
                        nk_rgb(220, 252, 231));
    nk_style_push_color(app->nk, &app->nk->style.selectable.text_pressed,
                        nk_rgb(220, 252, 231));
    nk_style_push_color(app->nk,
                        &app->nk->style.selectable.text_normal_active,
                        nk_rgb(187, 247, 208));
    nk_style_push_color(app->nk,
                        &app->nk->style.selectable.text_hover_active,
                        nk_rgb(220, 252, 231));
    nk_style_push_color(app->nk,
                        &app->nk->style.selectable.text_pressed_active,
                        nk_rgb(220, 252, 231));
    clicked = nk_selectable_label(app->nk, text, NK_TEXT_LEFT, selected);
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    nk_style_pop_color(app->nk);
    return clicked;
}

static void jump_to_source_line(GuiApp *app)
{
    char *tail;
    unsigned long long source_line;
    uint64_t low = 0;
    uint64_t high = app->selected_records;
    errno = 0;
    source_line = strtoull(app->jump_text, &tail, 10);
    if (errno != 0 || tail == app->jump_text || *tail != '\0' || source_line == 0) {
        set_error(app, "Enter a positive source line number");
        return;
    }
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        GuiIndexEntry index_entry;
        if (!read_index_entry(app, middle, &index_entry)) {
            set_error(app, "Cannot read the temporary result index");
            return;
        }
        if (index_entry.line_number < (uint64_t)source_line) low = middle + 1;
        else high = middle;
    }
    if (low >= app->selected_records) low = app->selected_records - 1;
    request_page(app, (low / GUI_PAGE_ROWS) * GUI_PAGE_ROWS);
    app->selected_sequence = low;
    app->jump_text[0] = '\0';
}

static void move_selected_match(GuiApp *app, int direction)
{
    uint64_t target;
    if (app->selected_records == 0 || !app->has_selection) return;
    target = app->selected_sequence;
    if (direction < 0) {
        if (target == 0) return;
        target--;
    } else {
        if (target + 1 >= app->selected_records) return;
        target++;
    }
    if (target < app->view_start || target >= app->view_start + GUI_PAGE_ROWS) {
        request_page(app, (target / GUI_PAGE_ROWS) * GUI_PAGE_ROWS);
        app->selected_sequence = target;
    } else {
        app->selected_sequence = target;
        clear_detail(app);
        if (app->context_visible) start_context_load(app);
    }
}

static void clear_all_queries(GuiApp *app)
{
    app->search[0] = '\0';
    app->filter_text[0] = '\0';
    app->regex_text[0] = '\0';
    app->field_regex_text[0] = '\0';
    app->exclude_regex_text[0] = '\0';
    start_scan(app);
}

static void render_row_list(GuiApp *app)
{
    uint64_t first;
    uint64_t end;
    uint64_t sequence;
    float time_width;
    float source_width;
    float level_width;
    if (!nk_group_begin(app->nk, "Matching records", NK_WINDOW_BORDER)) {
        nk_group_end(app->nk);
        return;
    }
    time_width = measure_label(app, "TIME") + 18.0f;
    source_width = measure_label(app, "SOURCE") + 18.0f;
    level_width = measure_label(app, "LEVEL") + 18.0f;
    first = app->view_start;
    if (first > app->selected_records) first = app->selected_records;
    end = first + GUI_PAGE_ROWS;
    if (end < first || end > app->selected_records) end = app->selected_records;
    for (sequence = first; sequence < end; sequence++) {
        GuiRow *row = cached_row(app, sequence);
        float measured;
        if (row == NULL) continue;
        measured = measure_label(app, row->timestamp[0] != '\0' ?
                                 row->timestamp : "-") + 18.0f;
        if (measured > time_width) time_width = measured;
        measured = measure_label(app, row->level[0] != '\0' ? row->level : "-") +
                   18.0f;
        if (measured > level_width) level_width = measured;
        measured = measure_label(app, row->source[0] != '\0' ? row->source : "-") +
                   18.0f;
        if (measured > source_width) source_width = measured;
    }
    if (time_width < 130.0f) time_width = 130.0f;
    if (time_width > 220.0f) time_width = 220.0f;
    if (source_width < 76.0f) source_width = 76.0f;
    if (source_width > 170.0f) source_width = 170.0f;
    if (level_width < 76.0f) level_width = 76.0f;
    if (level_width > 180.0f) level_width = 180.0f;
    if (!app->scanning && app->selected_records != 0) {
        char range[96];
        nk_layout_row_dynamic(app->nk, 27, 5);
        SDL_snprintf(range, sizeof(range), "%llu-%llu / %llu",
                     (unsigned long long)(first + 1),
                     (unsigned long long)end,
                     (unsigned long long)app->selected_records);
        nk_label(app->nk, range, NK_TEXT_LEFT);
        if (nk_button_label(app->nk, "First") && !app->page_loading)
            request_page(app, 0);
        if (nk_button_label(app->nk, "Previous") && !app->page_loading && first > 0)
            request_page(app, first > GUI_PAGE_ROWS ? first - GUI_PAGE_ROWS : 0);
        if (nk_button_label(app->nk, "Next") && !app->page_loading && end < app->selected_records)
            request_page(app, end);
        if (nk_button_label(app->nk, "Last") && !app->page_loading)
            request_page(app, app->selected_records > GUI_PAGE_ROWS ?
                         app->selected_records - GUI_PAGE_ROWS : 0);
        nk_layout_row_dynamic(app->nk, 27, 5);
        if (nk_button_label(app->nk, app->has_selection &&
                            sequence_is_bookmarked(app, app->selected_sequence) ?
                            "Unbookmark" : "Bookmark"))
            toggle_selected_bookmark(app);
        if (nk_button_label(app->nk, "Next bookmark") &&
            app->bookmark_count != 0 && !app->page_loading) {
            size_t bookmark;
            uint64_t target = app->bookmark_sequences[0];
            for (bookmark = 0; bookmark < app->bookmark_count; bookmark++) {
                if (app->bookmark_sequences[bookmark] > app->selected_sequence) {
                    target = app->bookmark_sequences[bookmark];
                    break;
                }
            }
            request_page(app, (target / GUI_PAGE_ROWS) * GUI_PAGE_ROWS);
            app->selected_sequence = target;
        }
        nk_label(app->nk, "Jump to source line", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD, app->jump_text,
                                       (int)sizeof(app->jump_text), NULL);
        if (nk_button_label(app->nk, "Jump") && !app->page_loading)
            jump_to_source_line(app);
    }
    begin_result_table_row(app, 24, time_width, level_width, source_width);
    if (app->show_timestamp_column)
        nk_label_colored(app->nk, "Timestamp", NK_TEXT_LEFT,
                         nk_rgb(177, 197, 220));
    if (app->show_level_column)
        nk_label_colored(app->nk, "Level", NK_TEXT_LEFT,
                         nk_rgb(177, 197, 220));
    if (app->show_source_column)
        nk_label_colored(app->nk, "Source", NK_TEXT_LEFT,
                         nk_rgb(177, 197, 220));
    nk_label_colored(app->nk, "Message", NK_TEXT_LEFT,
                     nk_rgb(177, 197, 220));
    if (app->selected_records == 0) {
        nk_layout_row_dynamic(app->nk, 32, 1);
        nk_label(app->nk, app->scanning ? "Waiting for matching records..." :
                 "No matching records", NK_TEXT_LEFT);
    } else {
        for (sequence = first; sequence < end; sequence++) {
            GuiRow *row = cached_row(app, sequence);
            nk_bool selected;
            if (row == NULL) continue;
            selected = app->has_selection && sequence == app->selected_sequence;
            begin_result_table_row(app, 25, time_width,
                                   level_width, source_width);
            {
                char marked_time[64];
                const char *time_text = row->timestamp[0] != '\0' ?
                                        row->timestamp : "-";
                int bookmarked = sequence_is_bookmarked(app, sequence);
                if (bookmarked && app->show_timestamp_column)
                    SDL_snprintf(marked_time, sizeof(marked_time), "* %s", time_text);
                else SDL_strlcpy(marked_time, time_text, sizeof(marked_time));
                nk_bool clicked;
                if (app->show_timestamp_column) {
                    clicked = selectable_bookmark_label(app, marked_time,
                                                        &selected, bookmarked);
                } else {
                    clicked = nk_false;
                }
                if (app->show_level_column) {
                    push_level_badge(app, row->level[0] != '\0' ?
                                     row->level : "INFO");
                    if (nk_button_label(app->nk,
                        row->level[0] != '\0' ? row->level : "-")) {
                        selected = nk_true;
                        clicked = nk_true;
                    }
                    pop_level_badge(app);
                }
                if (app->show_source_column)
                    clicked = nk_selectable_label(
                        app->nk, row->source[0] != '\0' ? row->source : "-",
                        NK_TEXT_LEFT, &selected) || clicked;
                {
                    char marked_message[GUI_PREVIEW_CAPACITY + 4];
                    const char *message = row->message[0] != '\0' ?
                                          row->message : row->raw;
                    if (bookmarked && !app->show_timestamp_column) {
                        SDL_snprintf(marked_message, sizeof(marked_message),
                                     "* %s", message);
                        message = marked_message;
                    }
                    clicked = selectable_bookmark_label(
                        app, message, &selected,
                        bookmarked && !app->show_timestamp_column) || clicked;
                }
                if (clicked) {
                    app->selected_sequence = sequence;
                    app->has_selection = 1;
                    clear_detail(app);
                    if (app->context_visible) start_context_load(app);
                }
            }
            if (selected) {
                app->selected_sequence = sequence;
                app->has_selection = 1;
            }
        }
    }
    nk_group_end(app->nk);
}

static void render_details(GuiApp *app)
{
    GuiRow *row;
    if (!nk_group_begin(app->nk, "Log details", NK_WINDOW_BORDER)) {
        nk_group_end(app->nk);
        return;
    }
    if (app->analysis_view != 0) {
        size_t index;
        char label[160];
        nk_layout_row_dynamic(app->nk, 30, 2);
        nk_label(app->nk, app->analysis_view == 1 ?
                 "Field values and levels" : "Time histogram",
                 NK_TEXT_LEFT);
        if (nk_button_label(app->nk, "Back to record")) app->analysis_view = 0;
        SDL_snprintf(label, sizeof(label),
                     "Physical: %llu   Parsed: %llu   Rejected: %llu%s",
                     (unsigned long long)app->analysis_stats.physical,
                     (unsigned long long)app->analysis_stats.parsed,
                     (unsigned long long)app->analysis_stats.rejected,
                     app->analysis_active ? "   (working)" : "");
        nk_layout_row_dynamic(app->nk, 26, 1);
        nk_label(app->nk, label, NK_TEXT_LEFT);
        SDL_snprintf(label, sizeof(label), "Selected: %llu",
                     (unsigned long long)app->analysis_stats.selected);
        nk_layout_row_dynamic(app->nk, 24, 1);
        nk_label(app->nk, label, NK_TEXT_LEFT);
        if (app->analysis_view == 1) {
            nk_layout_row_dynamic(app->nk, 24, 1);
            nk_label(app->nk, "Records by level", NK_TEXT_LEFT);
            for (index = 0; index < app->analysis_stats.level_count; index++) {
                const LevelCount *item = &app->analysis_stats.levels[index];
                char value[192];
                size_t copy_length = item->length < 100 ? item->length : 100;
                SDL_snprintf(value, sizeof(value), "%.*s  |  %llu",
                             (int)copy_length, item->value,
                             (unsigned long long)item->count);
                nk_layout_row_dynamic(app->nk, 22, 1);
                nk_label(app->nk, value, NK_TEXT_LEFT);
            }
            nk_layout_row_dynamic(app->nk, 24, 1);
            nk_label(app->nk, "Most frequent field values", NK_TEXT_LEFT);
            {
                size_t selected[20];
                size_t count = stats_top_candidates(&app->analysis_stats,
                                                     selected,
                                                     SDL_arraysize(selected));
                for (index = 0; index < count; index++) {
                    const HeavyCandidate *item =
                        &app->analysis_stats.candidates[selected[index]];
                    char value[240];
                    size_t name_length = item->name_length < 48 ?
                                         item->name_length : 48;
                    size_t value_length = item->value_length < 96 ?
                                          item->value_length : 96;
                    SDL_snprintf(value, sizeof(value), "%.*s = %.*s  |  %llu%s",
                                 (int)name_length, item->name,
                                 (int)value_length, item->value,
                                 (unsigned long long)item->count,
                                 item->error != 0 ? " (estimated)" : "");
                    nk_layout_row_dynamic(app->nk, 22, 1);
                    nk_label(app->nk, value, NK_TEXT_LEFT);
                }
            }
        } else {
            SDL_snprintf(label, sizeof(label),
                         "Timestamped: %llu   Unknown: %llu",
                         (unsigned long long)app->analysis_stats.timestamped,
                         (unsigned long long)app->analysis_stats.unknown_time);
            nk_layout_row_dynamic(app->nk, 24, 1);
            nk_label(app->nk, label, NK_TEXT_LEFT);
            if (app->analysis_stats.has_timestamp) {
                time_t earliest = (time_t)app->analysis_stats.earliest_timestamp;
                time_t latest = (time_t)app->analysis_stats.latest_timestamp;
                struct tm *first_time = gmtime(&earliest);
                struct tm first_copy;
                struct tm last_copy;
                struct tm *last_time;
                char first_label[48] = "unknown";
                char last_label[48] = "unknown";
                if (first_time != NULL) {
                    first_copy = *first_time;
                    strftime(first_label, sizeof(first_label),
                             "%Y-%m-%d %H:%M:%SZ", &first_copy);
                }
                last_time = gmtime(&latest);
                if (last_time != NULL) {
                    last_copy = *last_time;
                    strftime(last_label, sizeof(last_label),
                             "%Y-%m-%d %H:%M:%SZ", &last_copy);
                }
                SDL_snprintf(label, sizeof(label), "UTC range: %s to %s",
                             first_label, last_label);
                nk_layout_row_dynamic(app->nk, 24, 1);
                nk_label(app->nk, label, NK_TEXT_LEFT);
            }
            {
                unsigned int group;
                uint64_t largest_group = 0;
                for (group = 0; group < 32; group++) {
                    uint64_t count = 0;
                    size_t bin;
                    for (bin = group * 8; bin < group * 8 + 8; bin++)
                        count += app->analysis_stats.timeline[bin];
                    if (count > largest_group) largest_group = count;
                }
                for (group = 0; group < 32; group++) {
                    uint64_t count = 0;
                    size_t bin;
                    char bar[48];
                    char row_label[128];
                    uint64_t first_sequence = UINT64_MAX;
                    for (bin = group * 8; bin < group * 8 + 8; bin++) {
                        count += app->analysis_stats.timeline[bin];
                        if (app->histogram_first_sequence[bin] < first_sequence)
                            first_sequence = app->histogram_first_sequence[bin];
                    }
                    {
                        size_t marks = largest_group != 0 ?
                            (size_t)(20.0 * (double)count /
                                     (double)largest_group) : 0;
                        if (marks > 20) marks = 20;
                        memset(bar, '#', marks);
                        bar[marks] = '\0';
                    }
                    if (app->analysis_stats.has_timestamp) {
                        int64_t span = app->analysis_stats.latest_timestamp -
                                       app->analysis_stats.earliest_timestamp;
                        time_t bucket_start = (time_t)(app->analysis_stats.earliest_timestamp +
                            (span * (int64_t)group) / 32);
                        time_t bucket_end = (time_t)(app->analysis_stats.earliest_timestamp +
                            (span * (int64_t)(group + 1)) / 32);
                        struct tm *start_tm = gmtime(&bucket_start);
                        struct tm start_copy;
                        struct tm end_copy;
                        struct tm *end_tm;
                        char time_label[40] = "time unavailable";
                        char start_label[20] = "?";
                        char end_label[20] = "?";
                        if (start_tm != NULL) {
                            start_copy = *start_tm;
                            strftime(start_label, sizeof(start_label),
                                     "%m-%d %H:%M", &start_copy);
                        }
                        end_tm = gmtime(&bucket_end);
                        if (end_tm != NULL) {
                            end_copy = *end_tm;
                            strftime(end_label, sizeof(end_label),
                                     "%m-%d %H:%M", &end_copy);
                        }
                        SDL_snprintf(time_label, sizeof(time_label), "%s-%s",
                                     start_label, end_label);
                        SDL_snprintf(row_label, sizeof(row_label),
                                     "%-24s %-20s %llu", time_label, bar,
                                     (unsigned long long)count);
                    } else {
                        SDL_snprintf(row_label, sizeof(row_label),
                                     "Bin %02u  %-20s %llu", group + 1, bar,
                                     (unsigned long long)count);
                    }
                    nk_layout_row_dynamic(app->nk, 20, 1);
                    {
                        nk_bool selected_bucket = nk_false;
                        if (nk_selectable_label(app->nk, row_label,
                                                NK_TEXT_LEFT,
                                                &selected_bucket) &&
                            first_sequence != UINT64_MAX &&
                            first_sequence < app->selected_records) {
                            uint64_t target_sequence = first_sequence;
                            app->analysis_view = 0;
                            request_page(app,
                                (target_sequence / GUI_PAGE_ROWS) * GUI_PAGE_ROWS);
                            app->selected_sequence = target_sequence;
                            SDL_snprintf(app->status, sizeof(app->status),
                                         "Jumped to first record in histogram bucket %u",
                                         group + 1);
                        }
                    }
                }
            }
        }
        nk_group_end(app->nk);
        return;
    }
    row = app->has_selection ? cached_row(app, app->selected_sequence) : NULL;
    if (row == NULL) {
        nk_layout_row_dynamic(app->nk, 24, 1);
        nk_label(app->nk, "Select a matching record to inspect it.", NK_TEXT_LEFT);
    } else {
        char value[96];
        const unsigned char *detail_view_data;
        size_t detail_view_length;
        nk_layout_row_dynamic(app->nk, 28, 1);
        nk_label_colored(app->nk, "Log details", NK_TEXT_LEFT,
                         nk_rgb(80, 185, 255));
        detail_metadata_row(app, "Timestamp",
                            row->timestamp[0] != '\0' ? row->timestamp : "-",
                            nk_rgb(224, 235, 248));
        detail_metadata_row(app, "Severity",
                            row->level[0] != '\0' ? row->level : "LOG",
                            level_color(row->level));
        detail_metadata_row(app, "Source",
                            row->source[0] != '\0' ? row->source : "-",
                            nk_rgb(116, 202, 255));
        SDL_snprintf(value, sizeof(value), "%llu",
                     (unsigned long long)row->line_number);
        detail_metadata_row(app, "Source line", value,
                            nk_rgb(224, 235, 248));
        nk_layout_row_dynamic(app->nk, 30, 1);
        if (active_tab_button(app,
                sequence_is_bookmarked(app, app->selected_sequence) ?
                "Bookmarked" : "Add bookmark",
                sequence_is_bookmarked(app, app->selected_sequence)))
            toggle_selected_bookmark(app);
        nk_layout_row_dynamic(app->nk, 30, 3);
        if (active_tab_button(app, "Structured", app->detail_tab == 0))
            app->detail_tab = 0;
        if (active_tab_button(app, "Raw", app->detail_tab == 1))
            app->detail_tab = 1;
        if (active_tab_button(app, "Context", app->detail_tab == 2)) {
            app->detail_tab = 2;
            if (!app->context_visible) start_context_load(app);
        }
        if (app->detail_tab == 0) {
            size_t field_index;
            nk_layout_row_dynamic(app->nk, 25, 1);
            nk_label_colored(app->nk, "MESSAGE", NK_TEXT_LEFT,
                             nk_rgb(145, 163, 184));
            nk_layout_row_dynamic(app->nk, 72, 1);
            nk_label_wrap(app->nk, row->message);
            nk_layout_row_dynamic(app->nk, 24, 1);
            nk_label_colored(app->nk, "STRUCTURED FIELDS", NK_TEXT_LEFT,
                             nk_rgb(145, 163, 184));
            if (app->detail_entry_valid) {
                for (field_index = 0;
                     field_index < app->detail_entry.field_count;
                     field_index++) {
                    const LogField *field = &app->detail_entry.fields[field_index];
                    char field_name[128];
                    char field_value[1024];
                    int name_length = (int)(field->name.length < sizeof(field_name) - 1 ?
                                             field->name.length : sizeof(field_name) - 1);
                    int value_length = (int)(field->value.length < sizeof(field_value) - 1 ?
                                              field->value.length : sizeof(field_value) - 1);
                    SDL_snprintf(field_name, sizeof(field_name), "%.*s",
                                 name_length, field->name.data);
                    SDL_snprintf(field_value, sizeof(field_value), "%.*s",
                                 value_length, field->value.data);
                    nk_layout_row_template_begin(app->nk, 52);
                    nk_layout_row_template_push_static(app->nk, 105);
                    nk_layout_row_template_push_dynamic(app->nk);
                    nk_layout_row_template_end(app->nk);
                    nk_label_colored_wrap(app->nk, field_name,
                                          nk_rgb(171, 195, 222));
                    nk_label_wrap(app->nk, field_value);
                }
            }
            nk_group_end(app->nk);
            return;
        }
        if (app->detail_tab == 2) {
            size_t context_index;
            nk_layout_row_dynamic(app->nk, 28, 2);
            if (nk_button_label(app->nk, "Before -") &&
                app->context_before > 0) {
                app->context_before--;
                if (app->context_visible) start_context_load(app);
            }
            if (nk_button_label(app->nk, "Before +") &&
                app->context_before < GUI_CONTEXT_SIDE_MAX) {
                app->context_before++;
                if (app->context_visible) start_context_load(app);
            }
            nk_layout_row_dynamic(app->nk, 28, 2);
            if (nk_button_label(app->nk, "After -") &&
                app->context_after > 0) {
                app->context_after--;
                if (app->context_visible) start_context_load(app);
            }
            if (nk_button_label(app->nk, "After +") &&
                app->context_after < GUI_CONTEXT_SIDE_MAX) {
                app->context_after++;
                if (app->context_visible) start_context_load(app);
            }
            nk_layout_row_dynamic(app->nk, 28, 3);
            if (nk_button_label(app->nk, "Both -") &&
                app->context_before > 0 && app->context_after > 0) {
                app->context_before--;
                app->context_after--;
                if (app->context_visible) start_context_load(app);
            }
            if (nk_button_label(app->nk, "Both +") &&
                app->context_before < GUI_CONTEXT_SIDE_MAX &&
                app->context_after < GUI_CONTEXT_SIDE_MAX) {
                app->context_before++;
                app->context_after++;
                if (app->context_visible) start_context_load(app);
            }
            if (nk_button_label(app->nk, app->context_visible ?
                                "Reload" : "Load context"))
                start_context_load(app);
            {
                char context_sizes[64];
                SDL_snprintf(context_sizes, sizeof(context_sizes),
                             "Before: %u   After: %u",
                             (unsigned)app->context_before,
                             (unsigned)app->context_after);
                nk_layout_row_dynamic(app->nk, 24, 1);
                nk_label(app->nk, context_sizes, NK_TEXT_LEFT);
            }
            nk_layout_row_dynamic(app->nk, 24, 1);
            nk_label(app->nk, app->context_loading ? "Loading..." :
                     "Nearby source records", NK_TEXT_LEFT);
            if (app->context_visible &&
                app->context_target_sequence == app->selected_sequence) {
                for (context_index = 0; context_index < app->context_count;
                     context_index++) {
                    char context_label[GUI_PREVIEW_CAPACITY + 48];
                    SDL_snprintf(context_label, sizeof(context_label), "%llu  %s",
                                 (unsigned long long)app->context_rows[context_index].line_number,
                                 app->context_rows[context_index].text);
                    nk_layout_row_dynamic(app->nk, 26, 1);
                    nk_label_wrap(app->nk, context_label);
                }
            }
            nk_group_end(app->nk);
            return;
        }
        nk_layout_row_dynamic(app->nk, 24, 1);
        SDL_snprintf(value, sizeof(value), "Source line %llu | %llu bytes",
                     (unsigned long long)row->line_number,
                     (unsigned long long)app->detail_length);
        nk_label(app->nk, value, NK_TEXT_LEFT);
        if (app->format == FORMAT_JSONL) {
            nk_layout_row_dynamic(app->nk, 28, 1);
            if (nk_button_label(app->nk, app->pretty_visible ?
                                "Show raw JSON" : "Format JSON")) {
                if (!app->pretty_visible && !build_pretty_detail(app)) {
                    set_error(app, "Cannot format this JSON record");
                } else {
                    app->pretty_visible = !app->pretty_visible;
                    app->detail_page = 0;
                }
            }
        }
        detail_view_data = app->pretty_visible ? app->pretty_data : app->detail_data;
        detail_view_length = app->pretty_visible ? app->pretty_length : app->detail_length;
        nk_layout_row_dynamic(app->nk, 28, 1);
        nk_label(app->nk, app->pretty_visible ? "Formatted JSON record" :
                 "Raw record preview", NK_TEXT_LEFT);
        nk_layout_row_dynamic(app->nk, 28, 3);
        if (nk_button_label(app->nk, "Previous part") &&
            app->detail_page >= GUI_DETAIL_PAGE_BYTES) {
            app->detail_page -= GUI_DETAIL_PAGE_BYTES;
        }
        SDL_snprintf(value, sizeof(value), "%llu bytes",
                     (unsigned long long)detail_view_length);
        nk_label(app->nk, value, NK_TEXT_CENTERED);
        if (nk_button_label(app->nk, "Next part") &&
            app->detail_page + GUI_DETAIL_PAGE_BYTES < detail_view_length) {
            app->detail_page += GUI_DETAIL_PAGE_BYTES;
        }
        nk_layout_row_dynamic(app->nk, 215, 1);
        if (app->detail_loaded && detail_view_data != NULL) {
            char page[GUI_DETAIL_PAGE_BYTES + 1];
            size_t page_start = app->detail_page;
            size_t page_length;
            if (page_start > detail_view_length) page_start = detail_view_length;
            while (page_start < detail_view_length &&
                   (detail_view_data[page_start] & 0xc0u) == 0x80u) page_start++;
            page_length = detail_view_length - page_start;
            if (page_length > GUI_DETAIL_PAGE_BYTES) page_length = GUI_DETAIL_PAGE_BYTES;
            page_length = make_record_page(page, sizeof(page),
                                           detail_view_data + page_start,
                                           page_length);
            nk_text_wrap(app->nk, page, (int)page_length);
        } else {
            nk_label_wrap(app->nk, row->raw);
        }
        nk_layout_row_dynamic(app->nk, 23, 1);
        nk_label(app->nk, "Full record, paged to keep rendering responsive.",
                 NK_TEXT_LEFT);
    }
    nk_group_end(app->nk);
}

static void render_sidebar(GuiApp *app)
{
    int format_index = 0;
    int next_format;
    size_t format_position;
    nk_bool follow_toggle;
    if (!nk_group_begin(app->nk, "Source and filters",
                        NK_WINDOW_BORDER)) {
        nk_group_end(app->nk);
        return;
    }
    sidebar_section_title(app, "Source");
    nk_layout_row_dynamic(app->nk, 30, 1);
    nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD, app->path,
                                   (int)sizeof(app->path), NULL);
    nk_layout_row_dynamic(app->nk, 30, 2);
    if (nk_button_label(app->nk, "Open file")) show_open_dialog(app);
    if (nk_button_label(app->nk, "Scan / Apply") &&
        !app->scanning && !app->exporting) start_scan(app);
    if (app->input_size != 0) {
        char size_label[64];
        if (app->input_size >= 1024u * 1024u * 1024u)
            SDL_snprintf(size_label, sizeof(size_label), "Size: %.2f GiB",
                         (double)app->input_size / (1024.0 * 1024.0 * 1024.0));
        else if (app->input_size >= 1024u * 1024u)
            SDL_snprintf(size_label, sizeof(size_label), "Size: %.1f MiB",
                         (double)app->input_size / (1024.0 * 1024.0));
        else
            SDL_snprintf(size_label, sizeof(size_label), "Size: %llu bytes",
                         (unsigned long long)app->input_size);
        nk_layout_row_dynamic(app->nk, 22, 1);
        nk_label(app->nk, size_label, NK_TEXT_LEFT);
    }

    sidebar_section_title(app, "Quick filters");
    nk_layout_row_dynamic(app->nk, 20, 1);
    nk_label_colored(app->nk, "Log format", NK_TEXT_LEFT,
                     nk_rgb(163, 181, 202));
    for (format_position = 0; format_position < SDL_arraysize(format_values);
         format_position++) {
        if (format_values[format_position] == app->selected_format) {
            format_index = (int)format_position;
            break;
        }
    }
    next_format = nk_combo(app->nk, format_names,
                           (int)SDL_arraysize(format_names), format_index,
                           30, nk_vec2(230, 240));
    if (next_format >= 0 &&
        (size_t)next_format < SDL_arraysize(format_values))
        app->selected_format = format_values[next_format];

    {
        uint32_t severity_mask = app->severity_filter_active ?
                                 app->severity_filter_mask :
                                 GUI_SEVERITY_ALL;
        int severity_changed = 0;
        size_t severity_index;
        sidebar_section_title(app, "Severity");
        nk_layout_row_dynamic(app->nk, 27, 2);
        for (severity_index = 0; severity_index < GUI_SEVERITY_COUNT;
             severity_index++) {
            nk_bool enabled = (severity_mask & (1u << severity_index)) != 0 ?
                              nk_true : nk_false;
            if (nk_checkbox_label(app->nk, gui_severity_names[severity_index],
                                  &enabled)) {
                if (enabled) severity_mask |= 1u << severity_index;
                else severity_mask &= ~(1u << severity_index);
                severity_changed = 1;
            }
        }
        if (severity_changed) {
            app->severity_filter_mask = severity_mask;
            app->severity_filter_active = severity_mask != GUI_SEVERITY_ALL;
            if (app->path[0] != '\0' && !app->scanning && !app->exporting)
                start_scan(app);
        }
    }

    if (nk_tree_push(app->nk, NK_TREE_NODE, "Visible columns", NK_MINIMIZED)) {
        nk_bool show_timestamp = app->show_timestamp_column ? nk_true : nk_false;
        nk_bool show_level = app->show_level_column ? nk_true : nk_false;
        nk_bool show_source = app->show_source_column ? nk_true : nk_false;
        int columns_changed = 0;
        nk_layout_row_dynamic(app->nk, 24, 2);
        if (nk_checkbox_label(app->nk, "Timestamp", &show_timestamp)) {
            app->show_timestamp_column = show_timestamp != nk_false;
            columns_changed = 1;
        }
        if (nk_checkbox_label(app->nk, "Level", &show_level)) {
            app->show_level_column = show_level != nk_false;
            columns_changed = 1;
        }
        nk_layout_row_dynamic(app->nk, 24, 2);
        if (nk_checkbox_label(app->nk, "Source", &show_source)) {
            app->show_source_column = show_source != nk_false;
            columns_changed = 1;
        }
        nk_label(app->nk, "Message is always shown", NK_TEXT_LEFT);
        if (columns_changed)
            SDL_strlcpy(app->status, "Visible columns updated",
                        sizeof(app->status));
        nk_tree_pop(app->nk);
    }

    nk_layout_row_dynamic(app->nk, 20, 1);
    nk_label_colored(app->nk, "Field filter", NK_TEXT_LEFT,
                     nk_rgb(163, 181, 202));
    nk_layout_row_dynamic(app->nk, 29, 1);
    nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD, app->filter_text,
                                   (int)sizeof(app->filter_text), NULL);
    if (nk_tree_push(app->nk, NK_TREE_NODE, "Advanced matching",
                     NK_MINIMIZED)) {
        nk_layout_row_dynamic(app->nk, 20, 1);
        nk_label_colored(app->nk, "Search regex", NK_TEXT_LEFT,
                         nk_rgb(163, 181, 202));
        nk_layout_row_dynamic(app->nk, 29, 1);
        nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD, app->regex_text,
                                       (int)sizeof(app->regex_text), NULL);
        nk_layout_row_dynamic(app->nk, 20, 1);
        nk_label_colored(app->nk, "Field regex (field=pattern)",
                         NK_TEXT_LEFT, nk_rgb(163, 181, 202));
        nk_layout_row_dynamic(app->nk, 29, 1);
        nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD,
                                       app->field_regex_text,
                                       (int)sizeof(app->field_regex_text), NULL);
        nk_layout_row_dynamic(app->nk, 20, 1);
        nk_label_colored(app->nk, "Exclude regex", NK_TEXT_LEFT,
                         nk_rgb(163, 181, 202));
        nk_layout_row_dynamic(app->nk, 29, 1);
        nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD,
                                       app->exclude_regex_text,
                                       (int)sizeof(app->exclude_regex_text), NULL);
        nk_layout_row_dynamic(app->nk, 20, 1);
        nk_label_colored(app->nk, "Custom pattern", NK_TEXT_LEFT,
                         nk_rgb(163, 181, 202));
        nk_layout_row_dynamic(app->nk, 29, 1);
        nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD,
                                       app->custom_pattern,
                                       (int)sizeof(app->custom_pattern), NULL);
        nk_tree_pop(app->nk);
    }
    sidebar_section_title(app, "Export");
    nk_layout_row_dynamic(app->nk, 30, 1);
    app->export_format = nk_combo(app->nk, export_names,
                                  (int)SDL_arraysize(export_names),
                                  (int)app->export_format, 30,
                                  nk_vec2(230, 100)) == 0 ?
                         OUTPUT_TEXT : OUTPUT_CSV;
    if (app->export_format == OUTPUT_CSV) {
        nk_layout_row_dynamic(app->nk, 20, 1);
        nk_label_colored(app->nk, "CSV columns (empty uses defaults)",
                         NK_TEXT_LEFT, nk_rgb(163, 181, 202));
        nk_layout_row_dynamic(app->nk, 29, 1);
        nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD,
                                       app->export_columns,
                                       (int)sizeof(app->export_columns), NULL);
    }
    nk_layout_row_dynamic(app->nk, 30, 2);
    if (nk_button_label(app->nk, "Export")) show_save_dialog(app);
    if (nk_button_label(app->nk, "Clear queries") && !app->scanning)
        clear_all_queries(app);
    sidebar_section_title(app, "Query profile");
    nk_layout_row_dynamic(app->nk, 30, 2);
    if (nk_button_label(app->nk, "Load profile"))
        show_profile_open_dialog(app);
    if (nk_button_label(app->nk, "Save profile"))
        show_profile_save_dialog(app);
    nk_layout_row_dynamic(app->nk, 27, 1);
    follow_toggle = app->follow_enabled ? nk_true : nk_false;
    if (nk_checkbox_label(app->nk, "Follow file", &follow_toggle)) {
        app->follow_enabled = follow_toggle != nk_false;
        if (app->follow_enabled && app->scan_complete && app->reader_ready) {
            app->follow_observed_end = app->reader.next_offset;
            app->follow_resume_offset = app->reader.next_offset;
            app->follow_line_number_base = app->reader.next_line_number;
            app->follow_pending_partial = 0;
            app->follow_waiting = 1;
            app->follow_last_poll_ns = 0;
        } else if (!app->follow_enabled) {
            app->follow_waiting = 0;
            SDL_strlcpy(app->status, "Follow stopped", sizeof(app->status));
        }
    }
    sidebar_section_title(app, "Analysis");
    nk_layout_row_dynamic(app->nk, 28, 1);
    nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD,
                                   app->analysis_field,
                                   (int)sizeof(app->analysis_field), NULL);
    nk_layout_row_dynamic(app->nk, 30, 2);
    if (nk_button_label(app->nk, "Field values")) start_analysis(app, 1);
    if (nk_button_label(app->nk, "Time histogram")) start_analysis(app, 2);
    nk_layout_row_dynamic(app->nk, 30, 1);
    if (nk_button_label(app->nk, "Help and shortcuts")) app->help_visible = 1;
    nk_group_end(app->nk);
}

static void build_ui(GuiApp *app)
{
    int width;
    int height;
    char title[128];
    char file_tab[160];
    char timing[160];
    char open_time[32];
    char scan_time[32];
    char total_time[32];
    SDL_GetWindowSize(app->window, &width, &height);
    SDL_snprintf(title, sizeof(title), "GLogViewer | %s",
                 app->path[0] != '\0' ? app->path : "Open a log file");
    {
        const char *file_name = app->path;
        const char *cursor;
        for (cursor = app->path; *cursor != '\0'; cursor++) {
            if (*cursor == '/' || *cursor == '\\') file_name = cursor + 1;
        }
        SDL_snprintf(file_tab, sizeof(file_tab), "%s",
                     file_name[0] != '\0' ? file_name : "Log explorer");
    }
    if (nk_begin(app->nk, title, nk_rect(0, 0, (float)width, (float)height),
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        nk_layout_row_begin(app->nk, NK_DYNAMIC, 56, 9);
        nk_layout_row_push(app->nk, 0.04f);
        if (active_tab_button(app, "G", 1)) show_open_dialog(app);
        nk_layout_row_push(app->nk, 0.12f);
        nk_label_colored(app->nk, "GLogViewer", NK_TEXT_LEFT,
                         nk_rgb(80, 185, 255));
        nk_layout_row_push(app->nk, 0.11f);
        if (nk_button_label(app->nk, "Open file...")) show_open_dialog(app);
        nk_layout_row_push(app->nk, 0.05f);
        nk_label_colored(app->nk, "Search", NK_TEXT_LEFT,
                         nk_rgb(177, 197, 220));
        nk_layout_row_push(app->nk, 0.385f);
        {
            nk_flags search_state = nk_edit_string_zero_terminated(
                app->nk, NK_EDIT_FIELD, app->search,
                (int)sizeof(app->search), NULL);
            if ((search_state & NK_EDIT_COMMITTED) != 0 &&
                !app->scanning && !app->exporting)
                start_scan(app);
        }
        nk_layout_row_push(app->nk, 0.08f);
        if (nk_button_label(app->nk, "Search") &&
            !app->scanning && !app->exporting) start_scan(app);
        nk_layout_row_push(app->nk, 0.07f);
        if (nk_button_label(app->nk, "Previous")) move_selected_match(app, -1);
        nk_layout_row_push(app->nk, 0.07f);
        if (nk_button_label(app->nk, "Next")) move_selected_match(app, 1);
        nk_layout_row_push(app->nk, 0.07f);
        if (nk_button_label(app->nk, app->exporting ? "Exporting..." : "Export"))
            show_save_dialog(app);
        nk_layout_row_end(app->nk);

        if (width >= 1180) {
            float body_height = (float)(height - 124);
            if (body_height < 180.0f) body_height = 180.0f;
            nk_layout_row_template_begin(app->nk, body_height);
            nk_layout_row_template_push_static(app->nk,
                                                (float)width * 0.17f);
            nk_layout_row_template_push_dynamic(app->nk);
            nk_layout_row_template_push_static(app->nk,
                                                (float)width * 0.26f);
            nk_layout_row_template_end(app->nk);
            render_sidebar(app);
            if (!nk_group_begin(app->nk, file_tab, NK_WINDOW_BORDER)) {
                nk_group_end(app->nk);
            } else {
                char match_summary[128];
                char progress_summary[160];
                nk_layout_row_dynamic(app->nk, 25, 1);
                nk_label_colored(app->nk, file_tab, NK_TEXT_LEFT,
                                 nk_rgb(80, 185, 255));
                nk_layout_row_dynamic(app->nk, 28, 2);
                SDL_snprintf(match_summary, sizeof(match_summary),
                             "%llu matching records",
                             (unsigned long long)app->selected_records);
                nk_label(app->nk, match_summary, NK_TEXT_LEFT);
                nk_label(app->nk, app->scanning ? "Scanning file..." :
                         (app->scan_complete ? "Scan complete" : "Ready"),
                         NK_TEXT_RIGHT);
                if (app->scanning) {
                    nk_size current = (nk_size)(app->input_size != 0 &&
                        app->bytes_read > app->input_size ? app->input_size :
                        app->bytes_read);
                    nk_size maximum = (nk_size)(app->input_size != 0 ?
                                                app->input_size : 1);
                    SDL_snprintf(progress_summary, sizeof(progress_summary),
                        "Read %.1f / %.1f MiB  |  %llu records matched",
                        (double)app->bytes_read / (1024.0 * 1024.0),
                        (double)app->input_size / (1024.0 * 1024.0),
                        (unsigned long long)app->selected_records);
                    nk_layout_row_dynamic(app->nk, 32, 1);
                    nk_label(app->nk, progress_summary, NK_TEXT_LEFT);
                    nk_layout_row_dynamic(app->nk, 20, 1);
                    nk_progress(app->nk, &current, maximum, nk_true);
                    nk_layout_row_dynamic(app->nk, 28, 1);
                    nk_label(app->nk,
                        "The matching-record table will appear when scanning finishes.",
                        NK_TEXT_LEFT);
                } else {
                    nk_layout_row_dynamic(app->nk, 28, 4);
                    if (nk_button_label(app->nk, "Previous error"))
                        start_level_navigation(app, 2, -1);
                    if (nk_button_label(app->nk, "Next error"))
                        start_level_navigation(app, 2, 1);
                    if (nk_button_label(app->nk, "Previous warning"))
                        start_level_navigation(app, 1, -1);
                    if (nk_button_label(app->nk, "Next warning"))
                        start_level_navigation(app, 1, 1);
                    nk_layout_row_dynamic(app->nk, body_height - 137.0f, 1);
                    render_row_list(app);
                }
                nk_group_end(app->nk);
            }
            if (app->scanning) {
                if (nk_group_begin(app->nk, "Log details", NK_WINDOW_BORDER)) {
                    nk_layout_row_dynamic(app->nk, 36, 1);
                    nk_label(app->nk,
                        "Record details will be available after the scan.",
                        NK_TEXT_LEFT);
                }
                nk_group_end(app->nk);
            } else {
                refresh_selected_detail(app);
                render_details(app);
            }
        } else {
            float body_height = (float)(height - 124);
            if (body_height < 180.0f) body_height = 180.0f;
            nk_layout_row_dynamic(app->nk, body_height * 0.3f, 1);
            render_sidebar(app);
            nk_layout_row_dynamic(app->nk, 30, 1);
            nk_edit_string_zero_terminated(app->nk, NK_EDIT_FIELD,
                                           app->search,
                                           (int)sizeof(app->search), NULL);
            nk_layout_row_dynamic(app->nk, body_height * 0.42f - 30.0f, 1);
            render_row_list(app);
            refresh_selected_detail(app);
            nk_layout_row_dynamic(app->nk, body_height * 0.28f, 1);
            render_details(app);
        }

    }
    nk_end(app->nk);
    if (nk_begin(app->nk, "##status-bar",
                 nk_rect(0, (float)height - 76.0f, (float)width, 76.0f),
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(app->nk, 22, 2);
        nk_label(app->nk, app->error[0] != '\0' ? app->error : app->status,
                 NK_TEXT_LEFT);
        if (app->error[0] != '\0') {
            nk_label(app->nk, "", NK_TEXT_RIGHT);
        } else if (app->load_started_ns != 0) {
            Uint64 scan_elapsed = app->scanning ?
                SDL_GetTicksNS() - app->scan_started_ns : app->scan_elapsed_ns;
            Uint64 enrichment_elapsed = app->enriching ?
                SDL_GetTicksNS() - app->enrichment_started_ns :
                app->enrichment_elapsed_ns;
            format_elapsed(app->open_elapsed_ns, open_time, sizeof(open_time));
            format_elapsed(scan_elapsed, scan_time, sizeof(scan_time));
            format_elapsed(app->open_elapsed_ns + scan_elapsed +
                           enrichment_elapsed, total_time, sizeof(total_time));
            SDL_snprintf(timing, sizeof(timing),
                         "Open %s  |  Scan %s%s  |  Total %s%s",
                         open_time, scan_time,
                         app->scanning ? " (running)" : "",
                         total_time, app->enriching ? " (preparing)" : "");
            nk_label(app->nk, timing, NK_TEXT_RIGHT);
        } else {
            nk_label(app->nk, "", NK_TEXT_RIGHT);
        }
        nk_layout_row_dynamic(app->nk, 24, 2);
        SDL_snprintf(title, sizeof(title),
                     "%llu lines read  |  %llu matches  |  %s",
                     (unsigned long long)app->physical_records,
                     (unsigned long long)app->selected_records,
                     (app->scanning || app->exporting || app->enriching ||
                      app->page_loading || app->context_loading ||
                      app->analysis_active || app->level_nav_active) ?
                     "Work in progress" :
                     (app->scan_complete ? "Scan complete" : "Ready"));
        nk_label(app->nk, title, NK_TEXT_LEFT);
        if (app->scanning && nk_button_label(app->nk, "Cancel scan")) {
            app->scan_elapsed_ns = SDL_GetTicksNS() - app->scan_started_ns;
            app->scanning = 0;
            SDL_strlcpy(app->status, "Scan cancelled", sizeof(app->status));
        } else if (app->exporting && nk_button_label(app->nk, "Cancel export")) {
            close_export(app);
            SDL_strlcpy(app->status, "Export cancelled", sizeof(app->status));
        } else if (app->analysis_active &&
                   nk_button_label(app->nk, "Cancel analysis")) {
            close_analysis(app);
            SDL_strlcpy(app->status, "Analysis cancelled", sizeof(app->status));
        } else if (app->level_nav_active &&
                   nk_button_label(app->nk, "Cancel navigation")) {
            close_level_navigation(app);
            SDL_strlcpy(app->status, "Level navigation cancelled",
                        sizeof(app->status));
        } else if (!(app->exporting || app->analysis_active ||
                     app->level_nav_active)) {
            nk_label(app->nk, "", NK_TEXT_RIGHT);
        }
    }
    nk_end(app->nk);
    if (app->help_visible) {
        nk_bool help_window = nk_begin(
            app->nk, "GLogViewer feature guide",
            nk_rect((float)width * 0.2f, (float)height * 0.18f,
                    (float)width * 0.6f, (float)height * 0.64f),
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
            NK_WINDOW_TITLE);
        if (help_window) {
            nk_layout_row_dynamic(app->nk, 42, 1);
            nk_label_wrap(app->nk,
                "Search accepts Boolean expressions with quoted terms, AND, OR, NOT, and parentheses. Use raw regex, field=pattern regex, and exclude regex to refine the result set.");
            nk_label_wrap(app->nk,
                "Previous/Next match or select a row. First/Previous/Next/Last page through every result. Enter a source line and press Jump to go to its nearest matching record.");
            nk_label_wrap(app->nk,
                "Toggle bookmarks on the selected record and use Next bookmark to traverse them. Use previous/next error or warning to navigate matching records. Show context displays nearby source lines. The details pane reads the full selected record in pages.");
            nk_label_wrap(app->nk,
                "Export writes the active result set as text or CSV. Setup and scan times appear below the results. Field value and UTC histogram views run as incremental analyses. Follow monitors appended records and file rotation.");
            nk_layout_row_dynamic(app->nk, 30, 1);
            if (nk_button_label(app->nk, "Close help")) app->help_visible = 0;
        }
        nk_end(app->nk);
    }
}

static void SDLCALL file_dialog_callback(void *userdata,
                                         const char *const *filelist,
                                         int filter)
{
    GuiApp *app = (GuiApp *)userdata;
    SDL_Event event;
    char *path = NULL;
    (void)filter;
    if (filelist != NULL && filelist[0] != NULL) {
        path = SDL_strdup(filelist[0]);
        if (path == NULL) return;
    }
    SDL_zero(event);
    event.type = app->file_event;
    event.user.code = app->dialog_action;
    event.user.data1 = path;
    if (!SDL_PushEvent(&event)) SDL_free(path);
}

static void process_event(GuiApp *app, SDL_Event *event)
{
    if (event->type == app->file_event) {
        char *path = (char *)event->user.data1;
        if (path != NULL) {
            if (event->user.code == 1) {
                start_export(app, path);
            } else if (event->user.code == 2) {
                load_gui_profile(app, path);
            } else if (event->user.code == 3) {
                save_gui_profile(app, path);
            } else {
                SDL_strlcpy(app->path, path, sizeof(app->path));
                start_scan(app);
            }
            SDL_free(path);
        } else {
            if (app->path[0] == '\0') {
                SDL_strlcpy(app->status, "No file selected",
                            sizeof(app->status));
            }
        }
        app->open_dialog_pending = 0;
        return;
    }
    if (event->type == SDL_EVENT_QUIT) {
        app->quit = 1;
        return;
    }
    if (event->type == SDL_EVENT_KEY_DOWN &&
        event->key.key == SDLK_Q && (event->key.mod & SDL_KMOD_CTRL) != 0) {
        app->quit = 1;
        return;
    }
    SDL_ConvertEventToRenderCoordinates(app->renderer, event);
    nk_sdl_handle_event(app->nk, event);
}

static int gui_run(GuiApp *app)
{
    int result = 0;
    int vsync_enabled = 1;
    int capture_progress;
    const char *capture_path;
    float scale;
    struct nk_font_atlas *atlas;
    struct nk_font_config font_config;
    struct nk_font *font;
    SDL_Event event;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "logview-gui: SDL initialization failed: %s\n",
                SDL_GetError());
        return 2;
    }
    load_gui_preferences(app);
    if (!SDL_CreateWindowAndRenderer("GLogViewer", app->window_width,
                                     app->window_height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &app->window, &app->renderer)) {
        fprintf(stderr, "logview-gui: cannot create the application window: %s\n",
                SDL_GetError());
        SDL_Quit();
        return 2;
    }
    app->file_event = SDL_RegisterEvents(1);
    if (app->file_event == (Uint32)-1) {
        fprintf(stderr, "logview-gui: cannot create the file dialog event: %s\n",
                SDL_GetError());
        result = 2;
        goto cleanup;
    }
    app->nk = nk_sdl_init(app->window, app->renderer, nk_sdl_allocator());
    if (app->nk == NULL) {
        fprintf(stderr, "logview-gui: cannot initialize Nuklear: %s\n",
                SDL_GetError());
        result = 2;
        goto cleanup;
    }
    scale = SDL_GetWindowDisplayScale(app->window);
    if (scale <= 0.0f) scale = 1.0f;
    SDL_SetRenderScale(app->renderer, scale, scale);
    SDL_SetWindowMinimumSize(app->window, 960, 640);
    font_config = nk_font_config(0);
    atlas = nk_sdl_font_stash_begin(app->nk);
    font = load_ui_font(atlas, 18.0f * scale, &font_config);
    nk_sdl_font_stash_end(app->nk);
    if (font != NULL) {
        font->handle.height /= scale;
        nk_style_set_font(app->nk, &font->handle);
    }
    apply_theme(app);
    SDL_SetRenderVSync(app->renderer, 1);
    capture_path = SDL_getenv("GLOGVIEW_GUI_CAPTURE");
    capture_progress = SDL_getenv("GLOGVIEW_GUI_CAPTURE_PROGRESS") != NULL;

    if (app->path[0] != '\0') start_scan(app);
    while (!app->quit) {
        nk_input_begin(app->nk);
        if (app->scanning) {
            if (SDL_PollEvent(&event)) process_event(app, &event);
        } else if (SDL_WaitEventTimeout(&event, 16)) {
            process_event(app, &event);
        }
        while (SDL_PollEvent(&event)) process_event(app, &event);
        nk_input_end(app->nk);

        if ((app->scanning && vsync_enabled) ||
            (!app->scanning && !vsync_enabled)) {
            vsync_enabled = app->scanning ? 0 : 1;
            SDL_SetRenderVSync(app->renderer, vsync_enabled);
        }

        process_scan_slice(app);
        process_enrichment_slice(app);
        process_page_load(app);
        process_context_load(app);
        process_analysis_slice(app);
        process_export_slice(app);
        process_level_navigation(app);
        process_follow_slice(app);
        SDL_SetRenderDrawColor(app->renderer, 17, 22, 29, 255);
        SDL_RenderClear(app->renderer);
        build_ui(app);
        nk_sdl_update_TextInput(app->nk);
        nk_sdl_render(app->nk, NK_ANTI_ALIASING_ON);
        if (capture_path != NULL && capture_path[0] != '\0' &&
            ((capture_progress && app->scanning) ||
             (!capture_progress && app->scan_complete &&
              !app->enriching && !app->page_loading))) {
            SDL_Surface *capture = SDL_RenderReadPixels(app->renderer, NULL);
            if (capture == NULL || !SDL_SaveBMP(capture, capture_path)) {
                fprintf(stderr, "logview-gui: screenshot failed: %s\n",
                        SDL_GetError());
                result = 2;
            }
            if (capture != NULL) SDL_DestroySurface(capture);
            app->quit = 1;
        }
        SDL_RenderPresent(app->renderer);
    }

cleanup:
    save_gui_preferences(app);
    close_export(app);
    close_analysis(app);
    close_level_navigation(app);
    close_page_load(app);
    close_context_load(app);
    close_enrichment(app);
    close_scan(app);
    clear_detail(app);
    search_query_free(app->query);
    filter_free(app->filter);
    regex_free(app->raw_regex);
    regex_free(app->field_regex);
    regex_free(app->exclude_regex);
    if (app->nk != NULL) nk_sdl_shutdown(app->nk);
    if (app->renderer != NULL) SDL_DestroyRenderer(app->renderer);
    if (app->window != NULL) SDL_DestroyWindow(app->window);
    SDL_Quit();
    return result;
}

static int SDLCALL gui_main(int argc, char *argv[])
{
    GuiApp app;
    int index;
    memset(&app, 0, sizeof(app));
    app.context_before = 5;
    app.context_after = 5;
    app.window_width = 1440;
    app.window_height = 920;
    app.selected_format = FORMAT_AUTO;
    app.show_timestamp_column = 1;
    app.show_level_column = 1;
    app.show_source_column = 1;
    SDL_strlcpy(app.status, "Open a log file or enter its path",
                sizeof(app.status));

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--format") == 0 && index + 1 < argc) {
            app.selected_format = parser_format_from_name(argv[++index]);
            if ((int)app.selected_format < 0) {
                fputs("Usage: logview-gui [file] [--format FORMAT] [--pattern PATTERN] [--filter EXPRESSION] [--syslog-year YYYY] [--syslog-timezone +/-HH:MM]\n", stderr);
                return 3;
            }
        } else if (strcmp(argv[index], "--syslog-year") == 0 &&
                   index + 1 < argc) {
            if (!parse_gui_unsigned(argv[++index], 9999, &app.syslog_year) ||
                app.syslog_year == 0) {
                fputs("logview-gui: --syslog-year requires a year from 1 to 9999\n",
                      stderr);
                return 3;
            }
        } else if (strcmp(argv[index], "--syslog-timezone") == 0 &&
                   index + 1 < argc) {
            if (!parse_gui_timezone(argv[++index],
                                    &app.syslog_timezone_offset_seconds)) {
                fputs("logview-gui: --syslog-timezone must use +/-HH:MM\n",
                      stderr);
                return 3;
            }
        } else if (strcmp(argv[index], "--pattern") == 0 && index + 1 < argc) {
            SDL_strlcpy(app.custom_pattern, argv[++index],
                        sizeof(app.custom_pattern));
        } else if (strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
            SDL_strlcpy(app.filter_text, argv[++index],
                        sizeof(app.filter_text));
        } else if (strcmp(argv[index], "--help") == 0) {
            puts("Usage: logview-gui [file] [--format FORMAT] [--pattern PATTERN] [--filter EXPRESSION] [--syslog-year YYYY] [--syslog-timezone +/-HH:MM]");
            puts("Open a log in the optional Nuklear desktop interface.");
            puts("--syslog-year and --syslog-timezone configure RFC 3164 timestamp analysis.");
            return 0;
        } else if (argv[index][0] == '-') {
            fputs("Usage: logview-gui [file] [--format FORMAT] [--pattern PATTERN] [--filter EXPRESSION] [--syslog-year YYYY] [--syslog-timezone +/-HH:MM]\n", stderr);
            return 3;
        } else if (app.path[0] == '\0') {
            SDL_strlcpy(app.path, argv[index], sizeof(app.path));
        } else {
            fputs("logview-gui: only one input file is supported\n", stderr);
            return 3;
        }
    }
    return gui_run(&app);
}

int main(int argc, char **argv)
{
    return SDL_RunApp(argc, argv, gui_main, NULL);
}
