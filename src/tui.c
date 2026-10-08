#include "tui.h"

#include "terminal.h"

#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define TUI_CACHE_ROWS 256
#define TUI_PREVIEW_BYTES 1024
#define TUI_QUERY_BYTES 256

extern volatile sig_atomic_t logview_stop_requested;

typedef struct {
    uint64_t sequence;
    uint64_t line_number;
    size_t length;
    unsigned char text[TUI_PREVIEW_BYTES];
} TuiRow;

typedef struct {
    TerminalState *terminal;
    TuiRow rows[TUI_CACHE_ROWS];
    uint64_t total_rows;
    uint64_t cursor;
    uint64_t physical_records;
    Regex *search_regex;
    char search_text[TUI_QUERY_BYTES];
    char status[160];
    int scanning;
    int help_visible;
} TuiState;

static int parse_record(InputFormat format, const char *custom_pattern,
                        JsonScratch *scratch, const unsigned char *line,
                        size_t length, uint64_t number, uint64_t offset,
                        LogEntry *entry)
{
    entry->raw.data = line;
    entry->raw.length = length;
    entry->line_number = number;
    entry->byte_offset = offset;
    entry->field_count = 0;
    entry->parse_ok = format == FORMAT_RAW;
    if (memchr(line, 0, length) != NULL) return 0;
    if (format == FORMAT_JSONL) return jsonl_parse(entry, scratch);
    if (format == FORMAT_CUSTOM) return parser_custom_line(custom_pattern, entry);
    if (format != FORMAT_RAW) return parser_format_line(format, entry);
    return 1;
}

static int record_selected(const LogEntry *entry, const unsigned char *line,
                           size_t length, Filter *filter, Regex *raw_regex,
                           Regex *field_regex, const char *field_name)
{
    int selected = 1;
    if (filter != NULL) {
        RegexResult filter_result;
        selected = filter_matches(filter, entry, &filter_result);
        if (filter_result == REGEX_WORK_LIMIT) return -1;
    }
    if (selected && raw_regex != NULL) {
        RegexResult result = regex_search(raw_regex, line, length);
        if (result == REGEX_WORK_LIMIT) return -1;
        selected = result == REGEX_MATCH;
    }
    if (selected && field_regex != NULL) {
        ByteSpan value;
        if (!log_entry_find_field(entry, field_name, &value)) return 0;
        {
            RegexResult result = regex_search(field_regex, value.data, value.length);
            if (result == REGEX_WORK_LIMIT) return -1;
            selected = result == REGEX_MATCH;
        }
    }
    return selected;
}

static uint64_t oldest_sequence(const TuiState *state)
{
    return state->total_rows > TUI_CACHE_ROWS ? state->total_rows - TUI_CACHE_ROWS : 0;
}

static TuiRow *row_at(TuiState *state, uint64_t sequence)
{
    TuiRow *row = &state->rows[sequence % TUI_CACHE_ROWS];
    return row->sequence == sequence ? row : NULL;
}

static void add_row(TuiState *state, uint64_t line_number,
                    const unsigned char *text, size_t length)
{
    uint64_t sequence = state->total_rows++;
    TuiRow *row = &state->rows[sequence % TUI_CACHE_ROWS];
    size_t copy_length = length < TUI_PREVIEW_BYTES ? length : TUI_PREVIEW_BYTES;
    row->sequence = sequence;
    row->line_number = line_number;
    row->length = copy_length;
    if (copy_length != 0) memcpy(row->text, text, copy_length);
    if (state->total_rows == 1 || state->cursor == sequence - 1) {
        state->cursor = sequence;
    } else if (state->cursor < oldest_sequence(state)) {
        state->cursor = oldest_sequence(state);
    }
}

static int row_matches(const TuiState *state, const TuiRow *row)
{
    return state->search_regex != NULL &&
           regex_search(state->search_regex, row->text, row->length) == REGEX_MATCH;
}

static void print_safe(const unsigned char *text, size_t length, size_t maximum)
{
    size_t index;
    size_t limit = length < maximum ? length : maximum;
    for (index = 0; index < limit; index++) {
        unsigned char byte = text[index];
        size_t sequence_length = 1;
        int valid = 1;
        if (byte < 0x20 || byte == 0x7f) {
            fputc('.', stdout);
            continue;
        }
        if (byte < 0x80) {
            fputc(byte, stdout);
            continue;
        }
        if (byte >= 0xc2 && byte <= 0xdf) sequence_length = 2;
        else if (byte >= 0xe0 && byte <= 0xef) sequence_length = 3;
        else if (byte >= 0xf0 && byte <= 0xf4) sequence_length = 4;
        else valid = 0;
        if (valid && index + sequence_length > limit) valid = 0;
        if (valid) {
            size_t offset;
            for (offset = 1; offset < sequence_length; offset++) {
                if ((text[index + offset] & 0xc0) != 0x80) valid = 0;
            }
            if (sequence_length == 2 && byte == 0xc2 && text[index + 1] <= 0x9f) valid = 0;
            if (sequence_length == 3 && ((byte == 0xe0 && text[index + 1] < 0xa0) ||
                                         (byte == 0xed && text[index + 1] >= 0xa0))) valid = 0;
            if (sequence_length == 4 && ((byte == 0xf0 && text[index + 1] < 0x90) ||
                                         (byte == 0xf4 && text[index + 1] >= 0x90))) valid = 0;
        }
        if (!valid) {
            fputc('.', stdout);
            continue;
        }
        fwrite(text + index, 1, sequence_length, stdout);
        index += sequence_length - 1;
    }
    if (length > maximum) fputs("...", stdout);
}

static void render(TuiState *state)
{
    int columns;
    int rows;
    int visible;
    int index;
    uint64_t oldest = oldest_sequence(state);
    uint64_t first;
    terminal_get_size(state->terminal, &columns, &rows);
    if (columns < 24) columns = 24;
    if (rows < 5) rows = 5;
    visible = rows - 3;
    first = state->cursor > (uint64_t)(visible / 2) ?
            state->cursor - (uint64_t)(visible / 2) : oldest;
    if (first < oldest) first = oldest;
    if (first + (uint64_t)visible > state->total_rows && state->total_rows > (uint64_t)visible) {
        first = state->total_rows - (uint64_t)visible;
    }
    if (first < oldest) first = oldest;
    fputs("\033[H\033[2J\033[?25l", stdout);
    printf("GLogViewer | %s | records: %llu | cached: %llu-%llu\n",
           state->scanning ? "scanning" : "complete",
           (unsigned long long)state->physical_records,
           (unsigned long long)(state->total_rows == 0 ? 0 : oldest + 1),
           (unsigned long long)state->total_rows);
    if (state->help_visible) {
        fputs("\n  Up/Down: move one row     PgUp/PgDn: move one page\n"
              "  Home/End: oldest/newest  /: regex search  n/N: next/previous match\n"
              "  f: edit field filter and rescan  r: edit raw regex and rescan\n"
              "  ?: close help  q: quit\n\n"
              "Search and filtering use the built-in regular expression syntax.\n", stdout);
        for (index = 0; index < rows - 10; index++) fputc('\n', stdout);
    } else {
        for (index = 0; index < visible; index++) {
            uint64_t sequence = first + (uint64_t)index;
            TuiRow *row = sequence < state->total_rows ? row_at(state, sequence) : NULL;
            if (row == NULL) {
                fputc('\n', stdout);
                continue;
            }
            if (sequence == state->cursor) fputs("\033[7m", stdout);
            else if (row_matches(state, row)) fputs("\033[33m", stdout);
            printf("%8llu ", (unsigned long long)row->line_number);
            print_safe(row->text, row->length, (size_t)(columns - 12));
            if (row->length == TUI_PREVIEW_BYTES) fputs(" [preview clipped]", stdout);
            fputs("\033[0m\n", stdout);
        }
    }
    printf("\033[%d;1H\033[2K%s%s", rows - 1,
           state->search_text[0] ? "Search: " : "Search: (none) | ",
           state->search_text[0] ? state->search_text : "? help  / search  f filter  r regex  q quit");
    printf("\033[%d;1H\033[2K%s", rows, state->status);
    fflush(stdout);
}

static void navigate_match(TuiState *state, int direction)
{
    uint64_t oldest = oldest_sequence(state);
    uint64_t sequence = state->cursor;
    if (state->search_regex == NULL || state->total_rows == 0) return;
    for (;;) {
        if (direction > 0) {
            if (++sequence >= state->total_rows) sequence = oldest;
        } else {
            if (sequence <= oldest) sequence = state->total_rows - 1;
            else sequence--;
        }
        if (row_matches(state, row_at(state, sequence))) {
            state->cursor = sequence;
            return;
        }
        if (sequence == state->cursor) return;
    }
}

static int prompt_search(TuiState *state)
{
    char text[TUI_QUERY_BYTES];
    char error[160];
    Regex *replacement = NULL;
    if (!terminal_read_line(state->terminal, "/", text, sizeof(text))) return 0;
    if (text[0] != '\0') {
        replacement = regex_compile(text, error, sizeof(error));
        if (replacement == NULL) {
            snprintf(state->status, sizeof(state->status), "Invalid search: %.130s", error);
            return 0;
        }
    }
    regex_free(state->search_regex);
    state->search_regex = replacement;
    strcpy(state->search_text, text);
    state->status[0] = '\0';
    return 0;
}

static int handle_key(TuiState *state, TerminalKey key, Filter **filter,
                      Regex **raw_regex, Regex *field_regex,
                      const char *regex_field_name, FILE *input,
                      LineReader *reader)
{
    uint64_t oldest = oldest_sequence(state);
    uint64_t page;
    int columns;
    int rows;
    char text[TUI_QUERY_BYTES];
    char error[160];
    terminal_get_size(state->terminal, &columns, &rows);
    (void)columns;
    page = (uint64_t)(rows > 5 ? rows - 4 : 1);
    if (state->help_visible) {
        state->help_visible = 0;
        return 0;
    }
    if (key.type == TERMINAL_KEY_CHARACTER) {
        if (key.character == 'q') return 1;
        if (key.character == '?') state->help_visible = 1;
        else if (key.character == '/') prompt_search(state);
        else if (key.character == 'n') navigate_match(state, 1);
        else if (key.character == 'N') navigate_match(state, -1);
        else if (key.character == 'f' || key.character == 'r') {
            int filter_edit = key.character == 'f';
            if (!terminal_read_line(state->terminal, filter_edit ? "Filter: " : "Regex: ",
                                    text, sizeof(text))) return 0;
            if (filter_edit) {
                Filter *replacement = NULL;
                if (text[0] != '\0') {
                    replacement = filter_compile(text, error, sizeof(error));
                    if (replacement == NULL) {
                        snprintf(state->status, sizeof(state->status), "Invalid filter: %.130s", error);
                        return 0;
                    }
                }
                filter_free(*filter);
                *filter = replacement;
            } else {
                Regex *replacement = NULL;
                if (text[0] != '\0') {
                    replacement = regex_compile(text, error, sizeof(error));
                    if (replacement == NULL) {
                        snprintf(state->status, sizeof(state->status), "Invalid regex: %.130s", error);
                        return 0;
                    }
                }
                regex_free(*raw_regex);
                *raw_regex = replacement;
            }
            if (!reader_reposition(reader, 0, 0)) {
                snprintf(state->status, sizeof(state->status), "Cannot restart the input scan");
                return 0;
            }
            state->status[0] = '\0';
            state->total_rows = 0;
            state->cursor = 0;
            state->physical_records = 0;
            state->scanning = 1;
            return 2;
        }
    } else if (key.type == TERMINAL_KEY_UP && state->cursor > oldest) state->cursor--;
    else if (key.type == TERMINAL_KEY_DOWN && state->cursor + 1 < state->total_rows) state->cursor++;
    else if (key.type == TERMINAL_KEY_PAGE_UP) state->cursor = state->cursor > oldest + page ? state->cursor - page : oldest;
    else if (key.type == TERMINAL_KEY_PAGE_DOWN) state->cursor = state->cursor + page < state->total_rows ? state->cursor + page : state->total_rows - (state->total_rows != 0);
    else if (key.type == TERMINAL_KEY_HOME) state->cursor = oldest;
    else if (key.type == TERMINAL_KEY_END && state->total_rows != 0) state->cursor = state->total_rows - 1;
    (void)field_regex;
    (void)regex_field_name;
    (void)input;
    return 0;
}

int tui_run(FILE *input, LineReader *reader, InputFormat format,
            const char *custom_pattern, JsonScratch *scratch,
            Filter **field_filter, Regex **raw_regex, Regex *field_regex,
            const char *regex_field_name)
{
    TuiState state;
    int should_quit = 0;
    memset(&state, 0, sizeof(state));
    if (!terminal_enter(&state.terminal)) {
        fputs("logview: --tui requires an interactive input and output terminal.\n", stderr);
        return 2;
    }
    if (!reader_reposition(reader, 0, 0)) {
        terminal_leave(state.terminal);
        return 2;
    }
    state.scanning = 1;
    state.status[0] = '\0';
    render(&state);
    while (!should_quit && !logview_stop_requested) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = reader_next(reader, &line, &length, &number, &offset);
        if (result == READER_ERROR) {
            snprintf(state.status, sizeof(state.status), "Input read error");
            state.scanning = 0;
            render(&state);
            break;
        }
        if (result == READER_EOF) {
            state.scanning = 0;
            snprintf(state.status, sizeof(state.status), "Scan complete");
            render(&state);
            while (!should_quit && !logview_stop_requested) {
                int action = handle_key(&state,
                                        terminal_read_key(state.terminal, 250),
                                        field_filter, raw_regex, field_regex,
                                        regex_field_name, input, reader);
                if (action == 1) should_quit = 1;
                else if (action == 2) break;
                else render(&state);
            }
            continue;
        }
        state.physical_records = number;
        if (result == READER_LINE) {
            LogEntry entry;
            int selected;
            if (parse_record(format, custom_pattern, scratch, line, length,
                             number, offset, &entry)) {
                selected = record_selected(&entry, line, length, *field_filter,
                                           *raw_regex, field_regex,
                                           regex_field_name);
                if (selected < 0) {
                    snprintf(state.status, sizeof(state.status), "Regex work limit exceeded at line %llu",
                             (unsigned long long)number);
                    state.scanning = 0;
                    render(&state);
                    terminal_leave(state.terminal);
                    regex_free(state.search_regex);
                    return 2;
                }
                if (selected) add_row(&state, number, line, length);
            }
        }
        if ((number & 127u) == 0) {
            int action = handle_key(&state, terminal_read_key(state.terminal, 0),
                                    field_filter, raw_regex, field_regex,
                                    regex_field_name, input, reader);
            if (action == 1) should_quit = 1;
            else if (action == 2) continue;
            render(&state);
        }
    }
    terminal_leave(state.terminal);
    regex_free(state.search_regex);
    return 0;
}
