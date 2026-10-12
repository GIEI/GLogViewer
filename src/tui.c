#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "tui.h"

#include "follow.h"
#include "json_pretty.h"
#include "output.h"
#include "stats.h"
#include "terminal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#endif

#define TUI_CACHE_ROWS 256
#define TUI_CHECKPOINT_ROWS 128
#define TUI_PREVIEW_BYTES 1024
#define TUI_QUERY_BYTES 256
#define TUI_CONTEXT_MAX 100
#define TUI_BOOKMARK_CAPACITY 32

enum {
    TUI_LEVEL_NONE,
    TUI_LEVEL_ERROR,
    TUI_LEVEL_WARNING
};

extern volatile sig_atomic_t logview_stop_requested;

typedef struct {
    uint64_t sequence;
    uint64_t line_number;
    uint64_t byte_offset;
    size_t length;
    unsigned char severity;
    unsigned char text[TUI_PREVIEW_BYTES];
} TuiRow;

typedef struct {
    uint64_t line_number;
    size_t length;
    unsigned char text[TUI_PREVIEW_BYTES];
} TuiContextRow;

typedef struct {
    uint64_t sequence;
    uint64_t line_number;
    uint64_t byte_offset;
} TuiCheckpoint;

typedef struct {
    TerminalState *terminal;
    FILE *index_file;
    FILE *physical_index_file;
    FILE *input;
    LineReader *reader;
    InputFormat format;
    const char *custom_pattern;
    JsonScratch *scratch;
    Filter **field_filter;
    Regex **raw_regex;
    Regex **field_regex;
    const char *regex_field_name;
    TuiRow rows[TUI_CACHE_ROWS];
    TuiCheckpoint *limited_rows;
    size_t result_limit;
    size_t limited_count;
    size_t limited_next;
    int bottom_limit;
    uint64_t total_rows;
    uint64_t cursor;
    uint64_t search_navigation_sequence;
    uint64_t search_navigation_total;
    uint64_t search_navigation_checked;
    int search_navigation_direction;
    int search_navigation_active;
    int search_navigation_progress;
    uint64_t physical_records;
    uint64_t physical_checkpoint_count;
    TuiContextRow context_rows[TUI_CONTEXT_MAX * 2 + 1];
    size_t context_count;
    uint64_t context_anchor_line;
    size_t context_before;
    size_t context_after;
    uint64_t bookmark_lines[TUI_BOOKMARK_CAPACITY];
    uint64_t bookmark_sequences[TUI_BOOKMARK_CAPACITY];
    size_t bookmark_count;
    Regex *search_regex;
    char search_text[TUI_QUERY_BYTES];
    char status[160];
    int scanning;
    int follow_enabled;
    int follow_waiting;
    int follow_pending_partial;
    int help_visible;
    int context_visible;
    int values_visible;
    int histogram_visible;
    char summary_field[TUI_QUERY_BYTES];
    LogStats summary_stats;
    StatsTimestampConfig timestamp_config;
    uint64_t histogram_counts[LOGVIEW_TIMELINE_BINS];
    uint64_t histogram_errors[LOGVIEW_TIMELINE_BINS];
    uint64_t histogram_warnings[LOGVIEW_TIMELINE_BINS];
    uint64_t histogram_first_sequence[LOGVIEW_TIMELINE_BINS];
    uint64_t histogram_first_line[LOGVIEW_TIMELINE_BINS];
    uint64_t histogram_timestamped;
    uint64_t histogram_unknown;
    int64_t histogram_earliest;
    int64_t histogram_latest;
    size_t histogram_cursor;
    int histogram_has_range;
    unsigned int progress_frame;
    int terminal_columns;
    int terminal_rows;
    uint64_t follow_observed_end;
    uint64_t follow_resume_offset;
    uint64_t follow_line_number_base;
    const char *follow_path;
    const char *input_path;
} TuiState;

static int reset_indexes(TuiState *state);

static int tui_seek(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    if (offset > (uint64_t)INT64_MAX) return 0;
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    if (offset > (uint64_t)INT64_MAX) return 0;
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static int tui_file_size(FILE *file, uint64_t *size)
{
#ifdef _WIN32
    __int64 position;
    if (_fseeki64(file, 0, SEEK_END) != 0) return 0;
    position = _ftelli64(file);
    if (position < 0) return 0;
    *size = (uint64_t)position;
#else
    off_t position;
    if (fseeko(file, 0, SEEK_END) != 0) return 0;
    position = ftello(file);
    if (position < 0) return 0;
    *size = (uint64_t)position;
#endif
    return tui_seek(file, 0);
}

static int read_checkpoint(TuiState *state, uint64_t sequence,
                           TuiCheckpoint *checkpoint)
{
    uint64_t checkpoint_number = sequence / TUI_CHECKPOINT_ROWS;
    uint64_t index_offset;
    if (checkpoint_number > UINT64_MAX / sizeof(*checkpoint)) return 0;
    index_offset = checkpoint_number * sizeof(*checkpoint);
    if (fflush(state->index_file) != 0 || !tui_seek(state->index_file, index_offset) ||
        fread(checkpoint, sizeof(*checkpoint), 1, state->index_file) != 1) return 0;
    return 1;
}

static int read_physical_checkpoint(TuiState *state, uint64_t line_number,
                                    TuiCheckpoint *checkpoint)
{
    uint64_t low = 0;
    uint64_t high = state->physical_checkpoint_count;
    uint64_t index_offset;
    TuiCheckpoint candidate;
    if (line_number == 0 || high == 0 ||
        fflush(state->physical_index_file) != 0) return 0;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (middle > UINT64_MAX / sizeof(candidate)) return 0;
        index_offset = middle * sizeof(candidate);
        if (!tui_seek(state->physical_index_file, index_offset) ||
            fread(&candidate, sizeof(candidate), 1,
                  state->physical_index_file) != 1) return 0;
        if (candidate.line_number <= line_number) low = middle + 1;
        else high = middle;
    }
    if (low == 0) return 0;
    index_offset = (low - 1) * sizeof(*checkpoint);
    if (!tui_seek(state->physical_index_file, index_offset) ||
        fread(checkpoint, sizeof(*checkpoint), 1,
              state->physical_index_file) != 1) return 0;
    return 1;
}

static int add_physical_checkpoint(TuiState *state, uint64_t line_number,
                                   uint64_t byte_offset)
{
    TuiCheckpoint checkpoint;
    if (line_number == 0 || (line_number - 1) % TUI_CHECKPOINT_ROWS != 0) return 1;
    if (state->format == FORMAT_JSONL && state->reader->json_active) return 1;
    checkpoint.sequence = state->total_rows;
    checkpoint.line_number = line_number;
    checkpoint.byte_offset = byte_offset;
    if (fwrite(&checkpoint, sizeof(checkpoint), 1,
               state->physical_index_file) != 1) return 0;
    state->physical_checkpoint_count++;
    return 1;
}

static int index_physical_line(void *context, uint64_t line_number,
                               uint64_t byte_offset)
{
    return add_physical_checkpoint((TuiState *)context, line_number,
                                   byte_offset);
}

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

static int span_equals_ascii(ByteSpan span, const char *text)
{
    size_t index;
    size_t length = strlen(text);
    if (span.length != length) return 0;
    for (index = 0; index < length; index++) {
        unsigned char value = span.data[index];
        if (value >= 'a' && value <= 'z') value = (unsigned char)(value - 'a' + 'A');
        if (value != (unsigned char)text[index]) return 0;
    }
    return 1;
}

static unsigned char classify_entry(const LogEntry *entry)
{
    ByteSpan value;
    if (log_entry_find_field(entry, "level", &value)) {
        if (span_equals_ascii(value, "ERROR") || span_equals_ascii(value, "ERR") ||
            span_equals_ascii(value, "FATAL") || span_equals_ascii(value, "SEVERE") ||
            span_equals_ascii(value, "CRIT") || span_equals_ascii(value, "CRITICAL") ||
            span_equals_ascii(value, "ALERT") || span_equals_ascii(value, "EMERG") ||
            span_equals_ascii(value, "EMERGENCY")) return TUI_LEVEL_ERROR;
        if (span_equals_ascii(value, "WARN") || span_equals_ascii(value, "WARNING")) {
            return TUI_LEVEL_WARNING;
        }
    }
    if (log_entry_find_field(entry, "status", &value) && value.length == 3 &&
        value.data[0] >= '0' && value.data[0] <= '9' &&
        value.data[1] >= '0' && value.data[1] <= '9' &&
        value.data[2] >= '0' && value.data[2] <= '9') {
        unsigned int status = (unsigned int)(value.data[0] - '0') * 100u +
                              (unsigned int)(value.data[1] - '0') * 10u +
                              (unsigned int)(value.data[2] - '0');
        if (status >= 500) return TUI_LEVEL_ERROR;
        if (status >= 400) return TUI_LEVEL_WARNING;
    }
    return TUI_LEVEL_NONE;
}

static uint64_t oldest_sequence(const TuiState *state)
{
    if (!state->scanning) return 0;
    return state->total_rows > TUI_CACHE_ROWS ? state->total_rows - TUI_CACHE_ROWS : 0;
}

static TuiRow *row_at(TuiState *state, uint64_t sequence)
{
    TuiRow *row = &state->rows[sequence % TUI_CACHE_ROWS];
    return row->sequence == sequence ? row : NULL;
}

static void cache_row(TuiState *state, uint64_t sequence, uint64_t line_number,
                      uint64_t byte_offset, unsigned char severity,
                      const unsigned char *text, size_t length)
{
    TuiRow *row = &state->rows[sequence % TUI_CACHE_ROWS];
    size_t copy_length = length < TUI_PREVIEW_BYTES ? length : TUI_PREVIEW_BYTES;
    row->sequence = sequence;
    row->line_number = line_number;
    row->byte_offset = byte_offset;
    row->length = copy_length;
    row->severity = severity;
    if (copy_length != 0) memcpy(row->text, text, copy_length);
}

static int add_row(TuiState *state, uint64_t line_number, uint64_t byte_offset,
                   unsigned char severity, const unsigned char *text,
                   size_t length)
{
    uint64_t sequence = state->total_rows;
    if (state->result_limit != 0 && state->bottom_limit) {
        TuiCheckpoint *slot = &state->limited_rows[state->limited_next];
        slot->sequence = sequence;
        slot->line_number = line_number;
        slot->byte_offset = byte_offset;
        state->limited_next = (state->limited_next + 1) % state->result_limit;
        if (state->limited_count < state->result_limit) state->limited_count++;
        state->total_rows++;
        return 1;
    }
    if (state->result_limit != 0 && sequence >= state->result_limit) return 1;
    if (sequence % TUI_CHECKPOINT_ROWS == 0) {
        TuiCheckpoint checkpoint;
        checkpoint.sequence = sequence;
        checkpoint.line_number = line_number;
        checkpoint.byte_offset = byte_offset;
        if (fwrite(&checkpoint, sizeof(checkpoint), 1, state->index_file) != 1) return 0;
    }
    state->total_rows++;
    cache_row(state, sequence, line_number, byte_offset, severity, text, length);
    if (sequence == 0 || state->cursor == sequence - 1) {
        state->cursor = sequence;
    } else if (state->cursor < oldest_sequence(state)) {
        state->cursor = oldest_sequence(state);
    }
    return 1;
}

static int finalize_bottom_rows(TuiState *state)
{
    FILE *replacement;
    size_t index;
    size_t first;
    if (state->result_limit == 0 || !state->bottom_limit) return 1;
    replacement = tmpfile();
    if (replacement == NULL) return 0;
    fclose(state->index_file);
    state->index_file = replacement;
    first = state->limited_count == state->result_limit ? state->limited_next : 0;
    memset(state->rows, 0, sizeof(state->rows));
    for (index = 0; index < state->limited_count; index++) {
        TuiCheckpoint checkpoint = state->limited_rows[(first + index) % state->result_limit];
        checkpoint.sequence = index;
        if (index % TUI_CHECKPOINT_ROWS == 0 &&
            fwrite(&checkpoint, sizeof(checkpoint), 1, state->index_file) != 1) return 0;
    }
    state->total_rows = state->limited_count;
    state->cursor = 0;
    return 1;
}

static int load_window(TuiState *state, uint64_t first, uint64_t count)
{
    TuiCheckpoint checkpoint;
    uint64_t sequence;
    uint64_t end;
    if (first >= state->total_rows || count == 0) return 1;
    end = first + count;
    if (end < first || end > state->total_rows) end = state->total_rows;
    if (!read_checkpoint(state, first, &checkpoint) ||
        !reader_reposition(state->reader, checkpoint.byte_offset,
                           checkpoint.line_number - 1)) return 0;
    sequence = checkpoint.sequence;
    while (sequence < end) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = state->format == FORMAT_JSONL ?
            reader_next_json(state->reader, &line, &length, &number, &offset,
                             NULL, NULL) :
            reader_next(state->reader, &line, &length, &number, &offset);
        if (result == READER_EOF || result == READER_ERROR) return 0;
        if (result != READER_LINE) continue;
        {
            LogEntry entry;
            int selected;
            if (!parse_record(state->format, state->custom_pattern, state->scratch,
                              line, length, number, offset, &entry)) continue;
            selected = record_selected(&entry, line, length, *state->field_filter,
                                        *state->raw_regex, *state->field_regex,
                                        state->regex_field_name);
            if (selected < 0) return 0;
            if (selected) {
                cache_row(state, sequence, number, offset, classify_entry(&entry),
                          line, length);
                sequence++;
            }
        }
    }
    return 1;
}

static int ensure_sequence(TuiState *state, uint64_t sequence)
{
    if (row_at(state, sequence) != NULL) return 1;
    return load_window(state, sequence,
                       state->total_rows - sequence < TUI_CACHE_ROWS ?
                       state->total_rows - sequence : TUI_CACHE_ROWS);
}

static int load_context(TuiState *state)
{
    TuiRow *anchor;
    TuiCheckpoint checkpoint;
    uint64_t first_line;
    uint64_t last_line;
    if (state->scanning || state->total_rows == 0 ||
        !ensure_sequence(state, state->cursor)) return 0;
    anchor = row_at(state, state->cursor);
    if (anchor == NULL || anchor->line_number == 0) return 0;
    first_line = anchor->line_number > state->context_before ?
                 anchor->line_number - state->context_before : 1;
    last_line = anchor->line_number > UINT64_MAX - state->context_after ?
                UINT64_MAX : anchor->line_number + state->context_after;
    if (!read_physical_checkpoint(state, first_line, &checkpoint) ||
        !reader_reposition(state->reader, checkpoint.byte_offset,
                           checkpoint.line_number - 1)) return 0;
    state->context_count = 0;
    state->context_anchor_line = anchor->line_number;
    for (;;) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = reader_next(state->reader, &line, &length,
                                          &number, &offset);
        (void)offset;
        if (result == READER_ERROR) return 0;
        if (result == READER_EOF) break;
        if (number < first_line) continue;
        if (number > last_line) break;
        if (state->context_count < TUI_CONTEXT_MAX * 2 + 1) {
            TuiContextRow *row = &state->context_rows[state->context_count++];
            row->line_number = number;
            if (result == READER_TOO_LONG) {
                static const unsigned char message[] = "[record exceeds line limit]";
                row->length = sizeof(message) - 1;
                memcpy(row->text, message, row->length);
            } else {
                row->length = length < TUI_PREVIEW_BYTES ? length : TUI_PREVIEW_BYTES;
                if (row->length != 0) memcpy(row->text, line, row->length);
            }
        }
        if (number >= last_line) break;
    }
    return state->context_count != 0;
}

static int row_matches(const TuiState *state, const TuiRow *row)
{
    return row != NULL && state->search_regex != NULL &&
           regex_search(state->search_regex, row->text, row->length) == REGEX_MATCH;
}

static void start_search_navigation(TuiState *state, int direction,
                                    int include_current)
{
    uint64_t oldest = oldest_sequence(state);
    if (state->scanning) {
        state->search_navigation_active = 0;
        snprintf(state->status, sizeof(state->status),
                 "Search navigation will start when scanning completes");
        return;
    }
    if (state->search_regex == NULL || state->total_rows == 0) {
        state->search_navigation_active = 0;
        return;
    }
    state->search_navigation_total = state->total_rows - oldest;
    state->search_navigation_checked = 0;
    state->search_navigation_direction = direction < 0 ? -1 : 1;
    state->search_navigation_progress = -1;
    if (include_current) {
        state->search_navigation_sequence = oldest;
    } else if (direction < 0) {
        state->search_navigation_sequence = state->cursor <= oldest ?
            state->total_rows - 1 : state->cursor - 1;
    } else {
        state->search_navigation_sequence = state->cursor + 1 < state->total_rows ?
            state->cursor + 1 : oldest;
    }
    state->search_navigation_active = 1;
    snprintf(state->status, sizeof(state->status), "Searching: 0%% (Esc cancels)");
}

static int process_search_navigation(TuiState *state)
{
    unsigned int processed = 0;
    const unsigned int batch_size = TUI_CACHE_ROWS;
    int redraw = 0;
    while (state->search_navigation_active && processed < batch_size) {
        uint64_t oldest = oldest_sequence(state);
        uint64_t sequence = state->search_navigation_sequence;
        int progress;
        if (!ensure_sequence(state, sequence)) {
            state->search_navigation_active = 0;
            snprintf(state->status, sizeof(state->status),
                     "Cannot load records while searching");
            return 1;
        }
        if (row_matches(state, row_at(state, sequence))) {
            state->cursor = sequence;
            state->search_navigation_active = 0;
            state->status[0] = '\0';
            return 1;
        }
        state->search_navigation_checked++;
        processed++;
        sequence = state->search_navigation_direction < 0 ?
            (sequence <= oldest ? state->total_rows - 1 : sequence - 1) :
            (sequence + 1 < state->total_rows ? sequence + 1 : oldest);
        state->search_navigation_sequence = sequence;
        if (state->search_navigation_checked >= state->search_navigation_total) {
            state->search_navigation_active = 0;
            snprintf(state->status, sizeof(state->status), "No matching record");
            return 1;
        }
        progress = (int)((state->search_navigation_checked * 100u) /
                         state->search_navigation_total);
        if (progress != state->search_navigation_progress) {
            state->search_navigation_progress = progress;
            snprintf(state->status, sizeof(state->status),
                     "Searching: %d%% (Esc cancels)", progress);
            redraw = 1;
        }
    }
    return redraw;
}

static int restart_scan(TuiState *state)
{
    if (!reader_reposition(state->reader, 0, 0)) {
        snprintf(state->status, sizeof(state->status), "Cannot restart the input scan");
        return 0;
    }
    if (!reset_indexes(state)) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot create temporary navigation index");
        return 0;
    }
    state->scanning = 1;
    state->follow_waiting = 0;
    if (state->bookmark_count != 0) {
        state->bookmark_count = 0;
        snprintf(state->status, sizeof(state->status),
                 "Filters changed; source-line bookmarks cleared for the new result set");
        return 1;
    }
    snprintf(state->status, sizeof(state->status),
             "Scanning file; progress updates every 65,536 records");
    return 1;
}

static void toggle_bookmark(TuiState *state)
{
    TuiRow *row;
    size_t index;
    if (state->scanning || state->result_limit != 0 ||
        !ensure_sequence(state, state->cursor)) {
        snprintf(state->status, sizeof(state->status),
                 "Bookmarks require a complete, unlimited result view");
        return;
    }
    row = row_at(state, state->cursor);
    if (row == NULL) return;
    for (index = 0; index < state->bookmark_count; index++) {
        if (state->bookmark_lines[index] == row->line_number) {
            size_t move;
            for (move = index + 1; move < state->bookmark_count; move++) {
                state->bookmark_lines[move - 1] = state->bookmark_lines[move];
                state->bookmark_sequences[move - 1] = state->bookmark_sequences[move];
            }
            state->bookmark_count--;
            snprintf(state->status, sizeof(state->status),
                     "Removed bookmark for source line %llu",
                     (unsigned long long)row->line_number);
            return;
        }
    }
    if (state->bookmark_count >= TUI_BOOKMARK_CAPACITY) {
        snprintf(state->status, sizeof(state->status),
                 "Bookmark limit reached (%u)", TUI_BOOKMARK_CAPACITY);
        return;
    }
    state->bookmark_lines[state->bookmark_count] = row->line_number;
    state->bookmark_sequences[state->bookmark_count] = state->cursor;
    state->bookmark_count++;
    snprintf(state->status, sizeof(state->status),
             "Bookmarked source line %llu (%u total; press B for next)",
             (unsigned long long)row->line_number,
             (unsigned)state->bookmark_count);
}

static void jump_to_next_bookmark(TuiState *state)
{
    size_t index;
    size_t target = SIZE_MAX;
    uint64_t next_sequence = UINT64_MAX;
    uint64_t first_sequence = UINT64_MAX;
    size_t first_index = SIZE_MAX;
    if (state->bookmark_count == 0) {
        snprintf(state->status, sizeof(state->status), "No bookmarks; press b to bookmark this source line");
        return;
    }
    for (index = 0; index < state->bookmark_count; index++) {
        uint64_t sequence = state->bookmark_sequences[index];
        if (sequence < oldest_sequence(state) || sequence >= state->total_rows) continue;
        if (sequence < first_sequence) {
            first_sequence = sequence;
            first_index = index;
        }
        if (sequence > state->cursor && sequence < next_sequence) {
            next_sequence = sequence;
            target = index;
        }
    }
    if (target == SIZE_MAX) target = first_index;
    if (target != SIZE_MAX && ensure_sequence(state, state->bookmark_sequences[target])) {
        TuiRow *row = row_at(state, state->bookmark_sequences[target]);
        if (row != NULL && row->line_number == state->bookmark_lines[target]) {
            state->cursor = row->sequence;
            state->context_visible = 0;
            snprintf(state->status, sizeof(state->status),
                     "Jumped to bookmark: source line %llu",
                     (unsigned long long)row->line_number);
            return;
        }
    }
    state->bookmark_count = 0;
    snprintf(state->status, sizeof(state->status),
             "Bookmarks became stale and were cleared");
}

static size_t record_prefix_width(uint64_t line_number)
{
    size_t digits = 1;
    while (line_number >= 10) {
        line_number /= 10;
        digits++;
    }
    return (digits > 8 ? digits : 8) + 1;
}

static void print_safe(const unsigned char *text, size_t length, size_t maximum,
                       int source_truncated, int compact_spaces)
{
    size_t index;
    size_t used = 0;
    const char *marker = " [preview clipped]";
    size_t marker_length = strlen(marker);
    int clipped = source_truncated || length > maximum;
    int previous_space = 0;
    size_t content_limit = maximum;
    if (clipped) {
        if (maximum < marker_length) {
            marker = "...";
            marker_length = maximum < 3 ? maximum : 3;
        }
        content_limit = maximum - marker_length;
    }
    for (index = 0; index < length && used < content_limit; index++) {
        unsigned char byte = text[index];
        size_t sequence_length = 1;
        int valid = 1;
        if (byte == '\t' && !compact_spaces) {
            size_t spaces = 4 - (used % 4);
            size_t space_index;
            if (used + spaces > content_limit) break;
            for (space_index = 0; space_index < spaces; space_index++) {
                fputc(' ', stdout);
            }
            used += spaces;
            previous_space = 0;
            continue;
        }
        if (byte == ' ' && !compact_spaces) {
            fputc(' ', stdout);
            used++;
            previous_space = 0;
            continue;
        }
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            if (compact_spaces && !previous_space) {
                fputc(' ', stdout);
                used++;
                previous_space = 1;
            }
            continue;
        }
        if (byte < 0x20 || byte == 0x7f) {
            fputc('.', stdout);
            used++;
            previous_space = 0;
            continue;
        }
        if (byte < 0x80) {
            fputc(byte, stdout);
            used++;
            previous_space = 0;
            continue;
        }
        if (byte >= 0xc2 && byte <= 0xdf) sequence_length = 2;
        else if (byte >= 0xe0 && byte <= 0xef) sequence_length = 3;
        else if (byte >= 0xf0 && byte <= 0xf4) sequence_length = 4;
        else valid = 0;
        if (valid && index + sequence_length > length) valid = 0;
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
            used++;
            previous_space = 0;
            continue;
        }
        if (used + sequence_length > content_limit) break;
        fwrite(text + index, 1, sequence_length, stdout);
        used += sequence_length;
        previous_space = 0;
        index += sequence_length - 1;
    }
    if (clipped && marker_length != 0) fwrite(marker, 1, marker_length, stdout);
}

static void print_record(uint64_t line_number, const unsigned char *text,
                         size_t length, int source_truncated, int columns)
{
    size_t prefix_width = record_prefix_width(line_number);
    size_t available = columns > (int)prefix_width ?
                       (size_t)columns - prefix_width : 0;
    printf("%8llu ", (unsigned long long)line_number);
    print_safe(text, length, available, source_truncated, 1);
}

static size_t zoom_character_bytes(const unsigned char *text, size_t length,
                                   size_t offset)
{
    unsigned char first = text[offset];
    size_t count = 1;
    size_t index;
    if (first >= 0xc2 && first <= 0xdf) count = 2;
    else if (first >= 0xe0 && first <= 0xef) count = 3;
    else if (first >= 0xf0 && first <= 0xf4) count = 4;
    if (offset + count > length) return 1;
    for (index = 1; index < count; index++) {
        if ((text[offset + index] & 0xc0) != 0x80) return 1;
    }
    return count;
}

static size_t zoom_next_offset(const unsigned char *text, size_t length,
                               size_t start, size_t width)
{
    size_t offset = start;
    size_t used = 0;
    while (offset < length) {
        unsigned char byte = text[offset];
        size_t bytes = 1;
        size_t columns;
        if (byte == '\n') return offset + 1;
        if (byte == '\r') {
            return offset + (offset + 1 < length && text[offset + 1] == '\n' ? 2 : 1);
        }
        if (byte == '\t') columns = 4 - (used % 4);
        else {
            if (byte >= 0xc2 && byte <= 0xdf) bytes = 2;
            else if (byte >= 0xe0 && byte <= 0xef) bytes = 3;
            else if (byte >= 0xf0 && byte <= 0xf4) bytes = 4;
            bytes = zoom_character_bytes(text, length, offset);
            columns = bytes;
        }
        if (used != 0 && used + columns > width) return offset;
        used += columns;
        offset += bytes;
        if (used >= width) return offset;
    }
    return offset;
}

static int zoom_write_offset(FILE *index_file, size_t offset)
{
    uint64_t value = (uint64_t)offset;
    return fwrite(&value, sizeof(value), 1, index_file) == 1;
}

static int zoom_build_index(FILE *index_file, const unsigned char *text,
                            size_t length, size_t width, uint64_t *line_count)
{
    size_t offset = 0;
    uint64_t count = 1;
    if (!zoom_write_offset(index_file, 0)) return 0;
    while (offset < length) {
        size_t next = zoom_next_offset(text, length, offset, width);
        if (next <= offset) return 0;
        if (next < length || (next == length && length != 0 &&
                              text[length - 1] == '\n')) {
            if (!zoom_write_offset(index_file, next)) return 0;
            count++;
        }
        offset = next;
    }
    if (fflush(index_file) != 0) return 0;
    *line_count = count;
    return 1;
}

static int zoom_read_offset(FILE *index_file, uint64_t index,
                            uint64_t *offset)
{
    if (index > UINT64_MAX / sizeof(*offset) ||
        !tui_seek(index_file, index * sizeof(*offset))) return 0;
    return fread(offset, sizeof(*offset), 1, index_file) == 1;
}

static void render_zoom(const unsigned char *text, size_t length,
                        uint64_t line_count, uint64_t top,
                        size_t width, int rows,
                        uint64_t source_line, FILE *zoom_index,
                        int formatted_json)
{
    uint64_t visible = (uint64_t)(rows > 4 ? rows - 4 : 1);
    uint64_t index;
    fputs("\033[H\033[2J\033[?25l", stdout);
    printf("Record zoom | source line %llu | %s | display rows %llu-%llu of %llu\n",
           (unsigned long long)source_line,
           formatted_json ? "formatted JSON" : "source text",
           (unsigned long long)(top + 1),
           (unsigned long long)(top + visible < line_count ? top + visible : line_count),
           (unsigned long long)line_count);
    for (index = 0; index < visible; index++) {
        uint64_t row_index = top + index;
        uint64_t start_value;
        uint64_t end_value;
        size_t start;
        size_t end;
        size_t print_length;
        if (row_index >= line_count) {
            fputc('\n', stdout);
            continue;
        }
        if (!zoom_read_offset(zoom_index, row_index, &start_value)) break;
        if (row_index + 1 < line_count) {
            if (!zoom_read_offset(zoom_index, row_index + 1, &end_value)) break;
        } else {
            end_value = (uint64_t)length;
        }
        start = start_value > (uint64_t)length ? length : (size_t)start_value;
        end = end_value > (uint64_t)length ? length : (size_t)end_value;
        if (end < start) end = start;
        print_length = end - start;
        if (print_length != 0 && text[start + print_length - 1] == '\n') {
            print_length--;
            if (print_length != 0 && text[start + print_length - 1] == '\r') {
                print_length--;
            }
        }
        fputs("  ", stdout);
        print_safe(text + start, print_length, width, 0, 0);
        fputc('\n', stdout);
    }
    printf("\033[%d;1H\033[2KUp/Down: scroll  PgUp/PgDn: page  Home/End: file  z/Enter/Esc: close",
           rows);
    fflush(stdout);
}

static int zoom_selected_record(TuiState *state)
{
    TuiRow *selected;
    TuiRow row_copy;
    FILE *source = NULL;
    FILE *index_file = NULL;
    LineReader zoom_reader;
    const unsigned char *record = NULL;
    const unsigned char *display_record;
    unsigned char *pretty_record = NULL;
    size_t record_length = 0;
    size_t display_length;
    uint64_t line_number = 0;
    uint64_t byte_offset = 0;
    uint64_t line_count = 0;
    uint64_t top = 0;
    size_t width = 0;
    int rendered_rows = 0;
    int redraw = 1;
    int initialized = 0;
    int result_code = 0;
    int formatted_json = 0;
    ReaderResult result;
    if (state->scanning || state->total_rows == 0) {
        snprintf(state->status, sizeof(state->status),
                 "Zoom is available after the scan completes");
        return 0;
    }
    if (!ensure_sequence(state, state->cursor)) {
        snprintf(state->status, sizeof(state->status), "Cannot load selected record");
        return 0;
    }
    selected = row_at(state, state->cursor);
    if (selected == NULL || state->input_path == NULL ||
        strcmp(state->input_path, "-") == 0) {
        snprintf(state->status, sizeof(state->status), "Cannot reopen the input file for zoom");
        return 0;
    }
    row_copy = *selected;
    source = fopen(state->input_path, "rb");
    if (source == NULL || !reader_init(&zoom_reader, source,
                                       state->reader->max_line_bytes)) {
        if (source != NULL) fclose(source);
        snprintf(state->status, sizeof(state->status), "Cannot open selected record for zoom");
        return 0;
    }
    initialized = 1;
    if (!reader_reposition(&zoom_reader, row_copy.byte_offset,
                           row_copy.line_number - 1)) {
        snprintf(state->status, sizeof(state->status), "Cannot seek to selected record");
        goto cleanup;
    }
    result = state->format == FORMAT_JSONL ?
        reader_next_json(&zoom_reader, &record, &record_length, &line_number,
                         &byte_offset, NULL, NULL) :
        reader_next(&zoom_reader, &record, &record_length, &line_number,
                    &byte_offset);
    if (result != READER_LINE) {
        snprintf(state->status, sizeof(state->status), "Cannot read complete selected record");
        goto cleanup;
    }
    display_record = record;
    display_length = record_length;
    if (state->format == FORMAT_JSONL) {
        FILE *pretty_file = tmpfile();
        uint64_t pretty_length = 0;
        if (pretty_file != NULL &&
            json_pretty_write(pretty_file, record, record_length,
                              LOGVIEW_MAX_LINE_BYTES) &&
            tui_file_size(pretty_file, &pretty_length) &&
            pretty_length <= LOGVIEW_MAX_LINE_BYTES) {
            pretty_record = (unsigned char *)malloc(pretty_length == 0 ? 1 :
                                                     (size_t)pretty_length);
            if (pretty_record != NULL &&
                (pretty_length == 0 ||
                 fread(pretty_record, 1, (size_t)pretty_length,
                       pretty_file) == (size_t)pretty_length)) {
                display_record = pretty_record;
                display_length = (size_t)pretty_length;
                formatted_json = 1;
            } else {
                free(pretty_record);
                pretty_record = NULL;
            }
        }
        if (pretty_file != NULL) fclose(pretty_file);
    }
    index_file = tmpfile();
    if (index_file == NULL) {
        snprintf(state->status, sizeof(state->status), "Cannot create zoom line index");
        goto cleanup;
    }
    for (;;) {
        int new_columns;
        int new_rows;
        uint64_t visible;
        terminal_get_size(state->terminal, &new_columns, &new_rows);
        if (new_columns < 24) new_columns = 24;
        if (new_rows < 5) new_rows = 5;
        if (new_rows != rendered_rows) redraw = 1;
        if (width != (size_t)(new_columns - 4)) {
            width = (size_t)(new_columns - 4);
            redraw = 1;
            fclose(index_file);
            index_file = tmpfile();
            if (index_file == NULL ||
            !zoom_build_index(index_file, display_record, display_length,
                                  width, &line_count)) {
                snprintf(state->status, sizeof(state->status), "Cannot index zoom record lines");
                goto cleanup;
            }
            visible = (uint64_t)(new_rows > 4 ? new_rows - 4 : 1);
            if (top + visible > line_count) {
                top = line_count > visible ? line_count - visible : 0;
            }
        }
        if (redraw) {
            render_zoom(display_record, display_length, line_count, top,
                        width, new_rows, row_copy.line_number,
                        index_file, formatted_json);
            rendered_rows = new_rows;
            redraw = 0;
        }
        {
            TerminalKey key = terminal_read_key(state->terminal, 150);
            uint64_t page = (uint64_t)(new_rows > 4 ? new_rows - 4 : 1);
            uint64_t previous_top = top;
            if (key.type == TERMINAL_KEY_ESCAPE ||
                key.type == TERMINAL_KEY_ENTER ||
                key.type == TERMINAL_KEY_SHIFT_ENTER ||
                (key.type == TERMINAL_KEY_CHARACTER && key.character == 'z')) {
                break;
            }
            if (key.type == TERMINAL_KEY_CHARACTER && key.character == 'q') {
                result_code = 1;
                break;
            }
            if (key.type == TERMINAL_KEY_HOME) top = 0;
            else if (key.type == TERMINAL_KEY_END) {
                top = line_count > page ? line_count - page : 0;
            } else if (key.type == TERMINAL_KEY_UP && top != 0) top--;
            else if (key.type == TERMINAL_KEY_DOWN && top + page < line_count) top++;
            else if (key.type == TERMINAL_KEY_PAGE_UP) top = top > page ? top - page : 0;
            else if (key.type == TERMINAL_KEY_PAGE_DOWN) {
                top = top + page + page < line_count ? top + page :
                      (line_count > page ? line_count - page : 0);
            }
            if (top != previous_top) redraw = 1;
        }
    }
cleanup:
    if (index_file != NULL) fclose(index_file);
    free(pretty_record);
    if (initialized) reader_destroy(&zoom_reader);
    if (source != NULL) fclose(source);
    return result_code;
}

static void render_scan_progress(TuiState *state)
{
    static const char spinner[] = "|/-\\";
    int columns;
    int rows;
    terminal_get_size(state->terminal, &columns, &rows);
    (void)columns;
    printf("\033[1;1H\033[2KGLogViewer | scanning | records: %llu | selected: %llu",
           (unsigned long long)state->physical_records,
           (unsigned long long)state->total_rows);
    printf("\033[2;1H\033[2K  Scanning input %c",
           spinner[state->progress_frame++ % (sizeof(spinner) - 1)]);
    printf("\033[3;1H\033[2K  Physical records scanned: %llu",
           (unsigned long long)state->physical_records);
    printf("\033[4;1H\033[2K  Selected records indexed: %llu",
           (unsigned long long)state->total_rows);
    printf("\033[%d;1H\033[2K%s", rows, state->status);
    fflush(stdout);
}

static int record_matches_state(TuiState *state, const unsigned char *line,
                                size_t length, uint64_t number,
                                uint64_t offset, LogEntry *entry)
{
    if (!parse_record(state->format, state->custom_pattern, state->scratch,
                      line, length, number, offset, entry)) return 0;
    return record_selected(entry, line, length, *state->field_filter,
                           *state->raw_regex, *state->field_regex,
                           state->regex_field_name);
}

static int analysis_progress(TuiState *state, const char *task,
                             uint64_t scanned, int force)
{
    int columns;
    int rows;
    TerminalKey key;
    if (!force && (scanned & 65535u) != 0) {
        if ((scanned & 4095u) != 0) return 1;
    }
    terminal_get_size(state->terminal, &columns, &rows);
    (void)columns;
    if (force || (scanned & 65535u) == 0) {
        snprintf(state->status, sizeof(state->status),
                 "%s: %llu records scanned; Esc cancels",
                 task, (unsigned long long)scanned);
        printf("\033[%d;1H\033[2K%s", rows, state->status);
        fflush(stdout);
    }
    key = terminal_read_key(state->terminal, 0);
    if (key.type == TERMINAL_KEY_ESCAPE) return 0;
    if (key.type == TERMINAL_KEY_CHARACTER && key.character == 'q') return -1;
    return 1;
}

static int collect_field_summary(TuiState *state)
{
    uint64_t scanned = 0;
    int progress;
    if (state->scanning || state->follow_enabled || state->result_limit != 0) {
        snprintf(state->status, sizeof(state->status),
                 "Field summary requires a completed, unlimited file scan");
        return 0;
    }
    if (state->input_path == NULL || strcmp(state->input_path, "-") == 0) {
        snprintf(state->status, sizeof(state->status),
                 "Field summary requires a seekable input file");
        return 0;
    }
    if (state->format == FORMAT_RAW) {
        snprintf(state->status, sizeof(state->status),
                 "Field summary requires a structured log format");
        return 0;
    }
    if (!terminal_read_line(state->terminal, "Summarize field: ",
                            state->summary_field,
                            sizeof(state->summary_field)) ||
        state->summary_field[0] == '\0') return 0;
    state->values_visible = 0;
    state->histogram_visible = 0;
    stats_init(&state->summary_stats);
    stats_set_timestamp_config(&state->summary_stats,
                              &state->timestamp_config);
    if (!stats_set_focus_field(&state->summary_stats,
                               (const unsigned char *)state->summary_field,
                               strlen(state->summary_field)) ||
        !reader_reposition(state->reader, 0, 0)) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot prepare field summary scan");
        return 0;
    }
    progress = analysis_progress(state, "Building field summary", 0, 1);
    if (progress <= 0) {
        snprintf(state->status, sizeof(state->status),
                 progress < 0 ? "Field summary cancelled; quitting" :
                                "Field summary cancelled");
        return progress < 0 ? 1 : 0;
    }
    for (;;) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = state->format == FORMAT_JSONL ?
            reader_next_json(state->reader, &line, &length, &number, &offset,
                             NULL, NULL) :
            reader_next(state->reader, &line, &length, &number, &offset);
        if (result == READER_EOF) break;
        if (result == READER_ERROR) {
            snprintf(state->status, sizeof(state->status),
                     "Input error while building field summary");
            return 0;
        }
        if (result == READER_TOO_LONG) {
            scanned++;
            progress = analysis_progress(state, "Building field summary",
                                         scanned, 0);
            if (progress <= 0) {
                snprintf(state->status, sizeof(state->status),
                         progress < 0 ? "Field summary cancelled; quitting" :
                                        "Field summary cancelled");
                return progress < 0 ? 1 : 0;
            }
            continue;
        }
        if (result != READER_LINE) continue;
        {
            LogEntry entry;
            int selected = record_matches_state(state, line, length, number,
                                                offset, &entry);
            if (selected < 0) {
                snprintf(state->status, sizeof(state->status),
                         "Regex work limit exceeded at line %llu",
                         (unsigned long long)number);
                return 0;
            }
            if (selected && !stats_selected(&state->summary_stats, &entry)) {
                snprintf(state->status, sizeof(state->status),
                         "Field summary counter overflow");
                return 0;
            }
        }
        scanned++;
        progress = analysis_progress(state, "Building field summary", scanned, 0);
        if (progress <= 0) {
            state->values_visible = 0;
            snprintf(state->status, sizeof(state->status),
                     progress < 0 ? "Field summary cancelled; quitting" :
                                    "Field summary cancelled");
            return progress < 0 ? 1 : 0;
        }
    }
    reader_reposition(state->reader, 0, 0);
    state->values_visible = 1;
    state->context_visible = 0;
    snprintf(state->status, sizeof(state->status),
             "Esc or v returns to records");
    return 0;
}

static void format_epoch_label(int64_t epoch, char *buffer, size_t capacity)
{
    time_t value = (time_t)epoch;
    struct tm *broken_down = gmtime(&value);
    if (broken_down == NULL || (int64_t)value != epoch ||
        strftime(buffer, capacity, "%Y-%m-%d %H:%M:%SZ", broken_down) == 0) {
        snprintf(buffer, capacity, "%lld", (long long)epoch);
    }
}

static int collect_histogram_pass(TuiState *state, int populate,
                                  uint64_t *scanned)
{
    int progress;
    uint64_t sequence = 0;
    if (!reader_reposition(state->reader, 0, 0)) return 0;
    for (;;) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = state->format == FORMAT_JSONL ?
            reader_next_json(state->reader, &line, &length, &number, &offset,
                             NULL, NULL) :
            reader_next(state->reader, &line, &length, &number, &offset);
        if (result == READER_EOF) return 1;
        if (result == READER_ERROR) return 0;
        if (result == READER_TOO_LONG) {
            (*scanned)++;
            progress = analysis_progress(state,
                                         populate ? "Building histogram" :
                                                    "Finding timestamp range",
                                         *scanned, 0);
            if (progress <= 0) return progress;
            continue;
        }
        if (result != READER_LINE) continue;
        {
            LogEntry entry;
            int selected = record_matches_state(state, line, length, number,
                                                offset, &entry);
            if (selected < 0) return 0;
            if (selected) {
                ByteSpan timestamp;
                int64_t epoch;
                if (log_entry_find_field(&entry, "timestamp", &timestamp) &&
                    stats_timestamp_epoch_configured(
                        timestamp, &state->timestamp_config, &epoch)) {
                    if (populate) {
                        size_t bin = stats_timeline_bin(
                            epoch, state->histogram_earliest,
                            state->histogram_latest);
                        if (state->histogram_counts[bin] == 0) {
                            state->histogram_first_sequence[bin] = sequence;
                            state->histogram_first_line[bin] = number;
                        }
                        if (state->histogram_counts[bin] != UINT64_MAX) {
                            state->histogram_counts[bin]++;
                        }
                        if (classify_entry(&entry) == TUI_LEVEL_ERROR &&
                            state->histogram_errors[bin] != UINT64_MAX) {
                            state->histogram_errors[bin]++;
                        } else if (classify_entry(&entry) == TUI_LEVEL_WARNING &&
                                   state->histogram_warnings[bin] != UINT64_MAX) {
                            state->histogram_warnings[bin]++;
                        }
                        state->histogram_timestamped++;
                    } else {
                        if (!state->histogram_has_range ||
                            epoch < state->histogram_earliest) {
                            state->histogram_earliest = epoch;
                        }
                        if (!state->histogram_has_range ||
                            epoch > state->histogram_latest) {
                            state->histogram_latest = epoch;
                        }
                        state->histogram_has_range = 1;
                    }
                } else if (!populate && state->histogram_unknown != UINT64_MAX) {
                    state->histogram_unknown++;
                }
                sequence++;
            }
        }
        (*scanned)++;
        progress = analysis_progress(state,
                                     populate ? "Building histogram" :
                                                "Finding timestamp range",
                                     *scanned, 0);
        if (progress <= 0) return progress;
    }
}

static int build_histogram(TuiState *state)
{
    uint64_t scanned = 0;
    size_t index;
    int result;
    if (state->scanning || state->follow_enabled || state->result_limit != 0) {
        snprintf(state->status, sizeof(state->status),
                 "Histogram requires a completed, unlimited file scan");
        return 0;
    }
    if (state->input_path == NULL || strcmp(state->input_path, "-") == 0) {
        snprintf(state->status, sizeof(state->status),
                 "Histogram requires a seekable input file");
        return 0;
    }
    state->histogram_visible = 0;
    state->values_visible = 0;
    state->histogram_has_range = 0;
    state->histogram_unknown = 0;
    state->histogram_timestamped = 0;
    memset(state->histogram_counts, 0, sizeof(state->histogram_counts));
    memset(state->histogram_errors, 0, sizeof(state->histogram_errors));
    memset(state->histogram_warnings, 0, sizeof(state->histogram_warnings));
    for (index = 0; index < LOGVIEW_TIMELINE_BINS; index++) {
        state->histogram_first_sequence[index] = UINT64_MAX;
        state->histogram_first_line[index] = UINT64_MAX;
    }
    result = analysis_progress(state, "Finding timestamp range", 0, 1);
    if (result <= 0) {
        snprintf(state->status, sizeof(state->status),
                 result < 0 ? "Histogram scan cancelled; quitting" :
                              "Histogram scan cancelled");
        return result < 0 ? 1 : 0;
    }
    result = collect_histogram_pass(state, 0, &scanned);
    if (result <= 0) {
        snprintf(state->status, sizeof(state->status),
                 result < 0 ? "Histogram scan cancelled; quitting" :
                              "Histogram scan failed or cancelled");
        return result < 0 ? 1 : 0;
    }
    if (state->histogram_has_range) {
        scanned = 0;
        analysis_progress(state, "Building histogram", 0, 1);
        result = analysis_progress(state, "Building histogram", 0, 1);
        if (result <= 0) {
            snprintf(state->status, sizeof(state->status),
                     result < 0 ? "Histogram scan cancelled; quitting" :
                                  "Histogram scan cancelled");
            return result < 0 ? 1 : 0;
        }
        result = collect_histogram_pass(state, 1, &scanned);
        if (result <= 0) {
            snprintf(state->status, sizeof(state->status),
                     result < 0 ? "Histogram scan cancelled; quitting" :
                                  "Histogram scan failed or cancelled");
            return result < 0 ? 1 : 0;
        }
        for (index = 0; index < LOGVIEW_TIMELINE_BINS; index++) {
            if (state->histogram_counts[index] != 0) {
                state->histogram_cursor = index;
                break;
            }
        }
    }
    reader_reposition(state->reader, 0, 0);
    state->histogram_visible = 1;
    state->context_visible = 0;
    snprintf(state->status, sizeof(state->status),
             "Left/Right: bucket  Enter: jump  Esc or h: records");
    return 0;
}

static int terminal_size_changed(TuiState *state)
{
    int columns;
    int rows;
    terminal_get_size(state->terminal, &columns, &rows);
    if (columns < 24) columns = 24;
    if (rows < 5) rows = 5;
    return columns != state->terminal_columns || rows != state->terminal_rows;
}

static void render_field_summary(TuiState *state, int columns, int rows)
{
    size_t indexes[10];
    size_t count = stats_top_candidates(&state->summary_stats, indexes,
                                        sizeof(indexes) / sizeof(indexes[0]));
    size_t index;
    int visible = rows - 6;
    if (visible < 0) visible = 0;
    printf("Field values | %s | selected records: %llu\n\n",
           state->summary_field,
           (unsigned long long)state->summary_stats.selected);
    if (count == 0) {
        fputs("  No values found for this field in the selected records.\n", stdout);
    }
    if (count > (size_t)visible) count = (size_t)visible;
    for (index = 0; index < count; index++) {
        const HeavyCandidate *candidate =
            &state->summary_stats.candidates[indexes[index]];
        int value_width = columns - 34;
        if (value_width < 8) value_width = 8;
        printf("  %2u. ", (unsigned)(index + 1));
        print_safe(candidate->value, candidate->value_length,
                   (size_t)value_width,
                   candidate->value_length > sizeof(candidate->value), 1);
        printf("  %llu (%s",
               (unsigned long long)candidate->count,
               candidate->error == 0 ? "exact" : "estimated");
        if (candidate->error != 0) {
            printf("; error <= %llu", (unsigned long long)candidate->error);
        }
        fputs(")\n", stdout);
    }
    for (index = count + (count == 0 ? 1u : 0u); index < (size_t)(rows - 4); index++) {
        fputc('\n', stdout);
    }
    printf("\033[%d;1H\033[2KEsc or v: close  q: quit", rows - 2);
}

static uint64_t histogram_group_count(const TuiState *state, size_t first,
                                      size_t end, int component)
{
    size_t index;
    uint64_t total = 0;
    for (index = first; index < end; index++) {
        uint64_t value = component == TUI_LEVEL_ERROR ?
                         state->histogram_errors[index] :
                         component == TUI_LEVEL_WARNING ?
                         state->histogram_warnings[index] :
                         state->histogram_counts[index];
        if (UINT64_MAX - total < value) return UINT64_MAX;
        total += value;
    }
    return total;
}

static void render_histogram(TuiState *state, int columns, int rows)
{
    size_t chart_width = columns > 8 ? (size_t)(columns - 4) : 1;
    int chart_height = rows - 12;
    uint64_t maximum = 0;
    size_t column;
    int level;
    char earliest[40];
    char latest[40];
    if (chart_width > 128) chart_width = 128;
    if (chart_height < 1) chart_height = 1;
    if (chart_height > 12) chart_height = 12;
    for (column = 0; column < chart_width; column++) {
        size_t first = column * LOGVIEW_TIMELINE_BINS / chart_width;
        size_t end = (column + 1) * LOGVIEW_TIMELINE_BINS / chart_width;
        uint64_t count;
        if (end <= first) end = first + 1;
        count = histogram_group_count(state, first, end, 0);
        if (count > maximum) maximum = count;
    }
    if (!state->histogram_has_range) {
        fputs("Time histogram | no selected records have supported timestamps\n\n",
              stdout);
        printf("Unknown timestamps: %llu\n",
               (unsigned long long)state->histogram_unknown);
    } else {
        format_epoch_label(state->histogram_earliest, earliest, sizeof(earliest));
        format_epoch_label(state->histogram_latest, latest, sizeof(latest));
        printf("Time histogram | %llu timestamped | %llu unknown\n",
               (unsigned long long)state->histogram_timestamped,
               (unsigned long long)state->histogram_unknown);
        printf("UTC time: oldest %s  ->  newest %s\n", earliest, latest);
        printf("Relative volume (%d levels; tallest column = %llu records)\n",
               chart_height, (unsigned long long)maximum);
        fputs("Legend: # normal  + warning  ! error; each column groups time buckets\n",
              stdout);
        for (level = chart_height; level > 0; level--) {
            uint64_t threshold = maximum / (uint64_t)chart_height *
                                 (uint64_t)level;
            uint64_t remainder = maximum % (uint64_t)chart_height;
            if (remainder != 0) {
                uint64_t adjustment =
                    (remainder * (uint64_t)level +
                     (uint64_t)chart_height - 1) / (uint64_t)chart_height;
                if (UINT64_MAX - threshold < adjustment) threshold = UINT64_MAX;
                else threshold += adjustment;
            }
            printf("%2d |", level);
            for (column = 0; column < chart_width; column++) {
                size_t first = column * LOGVIEW_TIMELINE_BINS / chart_width;
                size_t end = (column + 1) * LOGVIEW_TIMELINE_BINS / chart_width;
                uint64_t count;
                char symbol = ' ';
                int color = 0;
                int selected;
                if (end <= first) end = first + 1;
                count = histogram_group_count(state, first, end, 0);
                if (count >= threshold && count != 0) {
                    if (histogram_group_count(state, first, end,
                                              TUI_LEVEL_ERROR) != 0) {
                        symbol = '!';
                        color = 31;
                    } else if (histogram_group_count(
                                   state, first, end, TUI_LEVEL_WARNING) != 0) {
                        symbol = '+';
                        color = 33;
                    } else symbol = '#';
                }
                selected = state->histogram_cursor >= first &&
                           state->histogram_cursor < end;
                if (selected) fputs("\033[7m", stdout);
                if (color != 0) printf("\033[%dm", color);
                fputc(symbol, stdout);
                if (color != 0 || selected) fputs("\033[0m", stdout);
            }
            fputs("|\n", stdout);
        }
        {
            size_t first = state->histogram_cursor;
            uint64_t count = state->histogram_counts[first];
            uint64_t range = (uint64_t)(state->histogram_latest -
                                        state->histogram_earliest);
            int64_t bucket_start = state->histogram_earliest +
                (int64_t)((range * first) / LOGVIEW_TIMELINE_BINS);
            int64_t bucket_end = first + 1 == LOGVIEW_TIMELINE_BINS ?
                state->histogram_latest : state->histogram_earliest +
                (int64_t)((range * (first + 1)) / LOGVIEW_TIMELINE_BINS);
            char bucket_start_text[40];
            char bucket_end_text[40];
            format_epoch_label(bucket_start, bucket_start_text,
                               sizeof(bucket_start_text));
            format_epoch_label(bucket_end, bucket_end_text,
                               sizeof(bucket_end_text));
            printf("Bucket %03u | %llu records | errors %llu | warnings %llu | source line %llu\n",
                   (unsigned)first, (unsigned long long)count,
                   (unsigned long long)state->histogram_errors[first],
                   (unsigned long long)state->histogram_warnings[first],
                   (unsigned long long)(state->histogram_first_line[first] == UINT64_MAX ?
                                        0 : state->histogram_first_line[first]));
            printf("Bucket time: %s .. %s\n",
                   bucket_start_text, bucket_end_text);
        }
    }
}

static void move_histogram_cursor(TuiState *state, int direction)
{
    size_t attempt;
    size_t index = state->histogram_cursor;
    for (attempt = 0; attempt < LOGVIEW_TIMELINE_BINS; attempt++) {
        index = direction > 0 ? (index + 1) % LOGVIEW_TIMELINE_BINS :
                (index == 0 ? LOGVIEW_TIMELINE_BINS - 1 : index - 1);
        if (state->histogram_counts[index] != 0) {
            state->histogram_cursor = index;
            return;
        }
    }
}

static void jump_to_histogram_bucket(TuiState *state)
{
    uint64_t sequence =
        state->histogram_first_sequence[state->histogram_cursor];
    if (sequence == UINT64_MAX || sequence >= state->total_rows ||
        !ensure_sequence(state, sequence)) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot load the first record in this time bucket");
        return;
    }
    state->cursor = sequence;
    state->histogram_visible = 0;
    snprintf(state->status, sizeof(state->status),
             "Jumped to first record in bucket %03u",
             (unsigned)state->histogram_cursor);
}

static void render(TuiState *state)
{
    int columns;
    int rows;
    int visible;
    int index;
    int context_rows_visible;
    size_t context_start;
    size_t context_anchor_index;
    uint64_t oldest = oldest_sequence(state);
    uint64_t first;
    terminal_get_size(state->terminal, &columns, &rows);
    if (columns < 24) columns = 24;
    if (rows < 5) rows = 5;
    visible = rows - 3;
    first = state->cursor > (uint64_t)(visible / 2) ?
            state->cursor - (uint64_t)(visible / 2) : 0;
    if (first < oldest) first = oldest;
    if (first + (uint64_t)visible > state->total_rows && state->total_rows > (uint64_t)visible) {
        first = state->total_rows - (uint64_t)visible;
    }
    if (first < oldest) first = oldest;
    if (!state->scanning && state->total_rows != 0) {
        int missing = 0;
        for (index = 0; index < visible && first + (uint64_t)index < state->total_rows; index++) {
            if (row_at(state, first + (uint64_t)index) == NULL) {
                missing = 1;
                break;
            }
        }
        if (missing && !load_window(state, first, (uint64_t)visible)) {
            snprintf(state->status, sizeof(state->status), "Cannot load indexed records");
        }
    }
    if (state->context_visible && !state->scanning && !load_context(state)) {
        snprintf(state->status, sizeof(state->status), "Cannot load record context");
    }
    fputs("\033[H\033[2J\033[?25l", stdout);
    printf("GLogViewer | %s | records: %llu | selected: %llu\n",
           state->scanning ? "scanning" :
           (state->follow_enabled ? "following" : "complete"),
           (unsigned long long)state->physical_records,
           (unsigned long long)state->total_rows);
    if (state->values_visible) {
        render_field_summary(state, columns, rows);
    } else if (state->histogram_visible) {
        render_histogram(state, columns, rows);
    } else if (state->scanning) {
        static const char spinner[] = "|/-\\";
        printf("  Scanning input %c\n",
               spinner[state->progress_frame++ % (sizeof(spinner) - 1)]);
        printf("  Physical records scanned: %llu\n"
               "  Selected records indexed: %llu\n"
               "\n"
               "  Results will appear when the scan completes.\n"
               "  Press q to stop the scan and exit.\n",
               (unsigned long long)state->physical_records,
               (unsigned long long)state->total_rows);
        for (index = 0; index < visible - 8; index++) fputc('\n', stdout);
    } else if (state->help_visible) {
        fputs("\n  Up/Down: move one row     PgUp/PgDn: move one page\n"
              "  Home/End: oldest/newest  /: search  Enter/Shift+Enter: next/previous match\n"
              "  J: jump to a source line (next visible result if filtered)\n"
              "  z: zoom the selected record and scroll through its full contents\n"
              "  v: summarize values for a structured field  h: time histogram\n"
              "  e/E: next/previous error  w/W: next/previous warning\n"
              "  b: toggle source-line bookmark  B: jump to next bookmark\n"
              "  c: show/hide context  +/-: change records before and after\n"
              "  Context defaults to 4 physical records on each side; bookmarks are session-only.\n"
              "  f: edit field filter and rescan  r: edit raw regex and rescan\n"
              "  C: clear search and all filters\n"
              "  x: export filtered/search results (use .csv for CSV)\n"
              "  ?: close help  q: quit\n\n"
              "Search and filtering use the built-in regular expression syntax.\n", stdout);
        for (index = 0; index < rows - 10; index++) fputc('\n', stdout);
    } else if (state->context_visible) {
        printf("\n  Context around source line %llu\n\n",
               (unsigned long long)state->context_anchor_line);
        context_rows_visible = rows - 6;
        if (context_rows_visible < 0) context_rows_visible = 0;
        if ((size_t)context_rows_visible > state->context_count) {
            context_rows_visible = (int)state->context_count;
        }
        context_anchor_index = 0;
        for (index = 0; (size_t)index < state->context_count; index++) {
            if (state->context_rows[index].line_number == state->context_anchor_line) {
                context_anchor_index = (size_t)index;
                break;
            }
        }
        context_start = context_anchor_index > (size_t)(context_rows_visible / 2) ?
                        context_anchor_index - (size_t)(context_rows_visible / 2) : 0;
        if (context_start + (size_t)context_rows_visible > state->context_count) {
            context_start = state->context_count - (size_t)context_rows_visible;
        }
        for (index = 0; index < context_rows_visible; index++) {
            TuiContextRow *row = &state->context_rows[context_start + (size_t)index];
            if (row->line_number == state->context_anchor_line) {
                fputs("\033[7m", stdout);
            }
            print_record(row->line_number, row->text, row->length,
                         row->length == TUI_PREVIEW_BYTES, columns);
            fputs("\033[0m\n", stdout);
        }
        for (index = context_rows_visible; index < rows - 6; index++) fputc('\n', stdout);
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
            else if (row->severity == TUI_LEVEL_ERROR) fputs("\033[31;1m", stdout);
            else if (row->severity == TUI_LEVEL_WARNING) fputs("\033[33m", stdout);
            print_record(row->line_number, row->text, row->length,
                         row->length == TUI_PREVIEW_BYTES, columns);
            fputs("\033[0m\n", stdout);
        }
    }
    printf("\033[%d;1H\033[2K%s%s", rows - 1,
           state->search_text[0] ? "Search: " : "Search: (none) | ",
           state->search_text[0] ? state->search_text :
           "? help  / search  J jump  f filter  r regex  v values  h histogram  x export  q quit");
    printf("\033[%d;1H\033[2K%s", rows, state->status);
    fflush(stdout);
    state->terminal_columns = columns;
    state->terminal_rows = rows;
}

static void navigate_match(TuiState *state, int direction)
{
    start_search_navigation(state, direction, 0);
}

static void navigate_level(TuiState *state, unsigned char severity, int direction)
{
    uint64_t oldest = oldest_sequence(state);
    uint64_t sequence = state->cursor;
    if (state->total_rows == 0) return;
    for (;;) {
        if (direction > 0) {
            if (++sequence >= state->total_rows) sequence = oldest;
        } else {
            if (sequence <= oldest) sequence = state->total_rows - 1;
            else sequence--;
        }
        if (!ensure_sequence(state, sequence)) {
            snprintf(state->status, sizeof(state->status), "Cannot load indexed records");
            return;
        }
        if (row_at(state, sequence)->severity == severity) {
            state->cursor = sequence;
            state->status[0] = '\0';
            return;
        }
        if (sequence == state->cursor) {
            snprintf(state->status, sizeof(state->status), "No matching severity in selected records");
            return;
        }
    }
}

static int reset_indexes(TuiState *state)
{
    FILE *new_index = tmpfile();
    FILE *new_physical_index = tmpfile();
    if (new_index == NULL || new_physical_index == NULL) {
        if (new_index != NULL) fclose(new_index);
        if (new_physical_index != NULL) fclose(new_physical_index);
        return 0;
    }
    fclose(state->index_file);
    fclose(state->physical_index_file);
    state->index_file = new_index;
    state->physical_index_file = new_physical_index;
    state->total_rows = 0;
    state->limited_count = 0;
    state->limited_next = 0;
    state->cursor = 0;
    state->physical_records = 0;
    state->physical_checkpoint_count = 0;
    state->context_count = 0;
    state->context_visible = 0;
    state->search_navigation_active = 0;
    return 1;
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
    if (replacement != NULL) start_search_navigation(state, 1, 1);
    else if (state->total_rows != 0) state->cursor = oldest_sequence(state);
    return 0;
}

static int parse_jump_line(const char *text, uint64_t *line_number)
{
    uint64_t value = 0;
    const unsigned char *cursor = (const unsigned char *)text;
    if (*cursor == '\0') return 0;
    while (*cursor != '\0') {
        unsigned int digit;
        if (*cursor < '0' || *cursor > '9') return 0;
        digit = (unsigned int)(*cursor - '0');
        if (value > (UINT64_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        cursor++;
    }
    if (value == 0) return 0;
    *line_number = value;
    return 1;
}

static int jump_to_line(TuiState *state)
{
    char text[TUI_QUERY_BYTES];
    uint64_t target;
    uint64_t sequence;
    uint64_t last_sequence;
    TuiCheckpoint checkpoint;

    state->search_navigation_active = 0;
    if (state->result_limit != 0) {
        snprintf(state->status, sizeof(state->status),
                 "Source-line jump is unavailable with a limited result view");
        return 0;
    }
    if (state->scanning) {
        snprintf(state->status, sizeof(state->status),
                 "Jump is available after the scan completes");
        return 0;
    }
    if (!terminal_read_line(state->terminal, "Jump to source line: ",
                            text, sizeof(text))) return 0;
    if (!parse_jump_line(text, &target)) {
        snprintf(state->status, sizeof(state->status),
                 "Enter a positive source line number");
        return 0;
    }
    if (target > state->physical_records) {
        snprintf(state->status, sizeof(state->status),
                 "Line %llu is beyond the end of the file (last line %llu)",
                 (unsigned long long)target,
                 (unsigned long long)state->physical_records);
        return 0;
    }
    if (state->total_rows == 0) {
        snprintf(state->status, sizeof(state->status),
                 "No selected records to jump to");
        return 0;
    }

    if (!read_physical_checkpoint(state, target, &checkpoint)) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot read the source-line index");
        return 0;
    }
    if (!reader_reposition(state->reader, checkpoint.byte_offset,
                           checkpoint.line_number - 1)) {
        snprintf(state->status, sizeof(state->status),
                 "Cannot seek to the requested source line");
        return 0;
    }

    sequence = checkpoint.sequence;
    last_sequence = state->total_rows - 1;
    for (;;) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = state->format == FORMAT_JSONL ?
            reader_next_json(state->reader, &line, &length, &number, &offset,
                             NULL, NULL) :
            reader_next(state->reader, &line, &length, &number, &offset);
        if (result == READER_EOF) break;
        if (result == READER_ERROR) {
            snprintf(state->status, sizeof(state->status),
                     "Error reading records at the requested line");
            return 0;
        }
        if (result != READER_LINE) continue;
        {
            LogEntry entry;
            int selected;
            if (!parse_record(state->format, state->custom_pattern, state->scratch,
                              line, length, number, offset, &entry)) continue;
            selected = record_selected(&entry, line, length, *state->field_filter,
                                       *state->raw_regex, *state->field_regex,
                                       state->regex_field_name);
            if (selected < 0) {
                snprintf(state->status, sizeof(state->status),
                         "Regex work limit exceeded at line %llu",
                         (unsigned long long)number);
                return 0;
            }
            if (!selected) continue;
            if (number >= target) {
                if (sequence >= state->total_rows) {
                    snprintf(state->status, sizeof(state->status),
                             "Input changed while jumping to the requested line");
                    return 0;
                }
                cache_row(state, sequence, number, offset,
                          classify_entry(&entry), line, length);
                state->cursor = sequence;
                state->context_visible = 0;
                if (number == target) {
                    snprintf(state->status, sizeof(state->status),
                             "Jumped to source line %llu",
                             (unsigned long long)number);
                } else {
                    snprintf(state->status, sizeof(state->status),
                             "No selected record on line %llu; jumped to line %llu",
                             (unsigned long long)target,
                             (unsigned long long)number);
                }
                return 0;
            }
            sequence++;
        }
    }

    if (ensure_sequence(state, last_sequence)) {
        TuiRow *row = row_at(state, last_sequence);
        if (row != NULL) {
            state->cursor = last_sequence;
            state->context_visible = 0;
            snprintf(state->status, sizeof(state->status),
                     "No selected record at or after line %llu; showing line %llu",
                     (unsigned long long)target,
                     (unsigned long long)row->line_number);
            return 0;
        }
    }
    snprintf(state->status, sizeof(state->status),
             "Cannot locate a selected record for that line");
    return 0;
}

static int has_csv_extension(const char *path)
{
    size_t length = strlen(path);
    const char *extension;
    if (length < 4) return 0;
    extension = path + length - 4;
    return extension[0] == '.' &&
           tolower((unsigned char)extension[1]) == 'c' &&
           tolower((unsigned char)extension[2]) == 's' &&
           tolower((unsigned char)extension[3]) == 'v';
}

static int initialize_export_sink(OutputSink *sink, const char *requested_path,
                                   const char *input_path, OutputFormat format,
                                   char *actual_path, size_t actual_capacity,
                                   char *error, size_t error_capacity)
{
    if (requested_path[0] != '\0') {
        int length = snprintf(actual_path, actual_capacity, "%s", requested_path);
        if (length < 0 || (size_t)length >= actual_capacity) {
            snprintf(error, error_capacity, "export path is too long");
            return 0;
        }
        return output_init(sink, actual_path, input_path, format, NULL, 0,
                           error, error_capacity);
    }
    else {
        const char *name = input_path;
        const char *separator;
        const char *extension;
        size_t stem_length;
        size_t extension_length;
        uint64_t sequence;
        separator = strrchr(name, '/');
        if (strrchr(name, '\\') != NULL &&
            (separator == NULL || strrchr(name, '\\') > separator)) {
            separator = strrchr(name, '\\');
        }
        if (separator != NULL) name = separator + 1;
        extension = strrchr(name, '.');
        if (extension == name) extension = NULL;
        stem_length = extension == NULL ? strlen(name) : (size_t)(extension - name);
        extension_length = extension == NULL ? 0 : strlen(extension);
        if (name[0] == '\0' || stem_length + extension_length >= actual_capacity) {
            snprintf(error, error_capacity, "cannot derive an export filename from the input path");
            return 0;
        }
        for (sequence = 1; sequence != 0; sequence++) {
            int length = snprintf(actual_path, actual_capacity, "%.*s_%llu%s",
                                 (int)stem_length, name,
                                 (unsigned long long)sequence,
                                 extension == NULL ? "" : extension);
            if (length < 0 || (size_t)length >= actual_capacity) {
                snprintf(error, error_capacity, "generated export filename is too long");
                return 0;
            }
            errno = 0;
            if (output_init(sink, actual_path, input_path, format, NULL, 0,
                            error, error_capacity)) return 1;
            output_finish(sink);
            if (errno != EEXIST) return 0;
        }
    }
    snprintf(error, error_capacity, "no available numbered export filename remains");
    return 0;
}

static int export_view(TuiState *state)
{
    char requested_path[TUI_QUERY_BYTES];
    char path[TUI_QUERY_BYTES];
    char error[160] = "";
    OutputSink sink;
    OutputFormat format;
    uint64_t saved_offset = state->reader->next_offset;
    uint64_t saved_line_number = state->reader->next_line_number;
    uint64_t exported = 0;
    int success = 1;
    if (state->result_limit != 0) {
        snprintf(state->status, sizeof(state->status),
                 "Export is unavailable with a limited result view");
        return 0;
    }
    if (state->scanning) {
        snprintf(state->status, sizeof(state->status), "Export is available after the scan completes");
        return 0;
    }
    if (!terminal_read_line(state->terminal, "Export path (Enter for numbered default): ",
                            requested_path, sizeof(requested_path))) return 0;
    format = has_csv_extension(requested_path[0] == '\0' ? state->input_path :
                               requested_path) ? OUTPUT_CSV : OUTPUT_TEXT;
    if (!initialize_export_sink(&sink, requested_path, state->input_path, format,
                                path, sizeof(path), error, sizeof(error))) {
        output_finish(&sink);
        snprintf(state->status, sizeof(state->status), "Export failed: %.125s", error);
        return 0;
    }
    if (!reader_reposition(state->reader, 0, 0)) {
        snprintf(error, sizeof(error), "cannot restart the input scan");
        success = 0;
    }
    while (success) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = state->format == FORMAT_JSONL ?
            reader_next_json(state->reader, &line, &length, &number, &offset,
                             NULL, NULL) :
            reader_next(state->reader, &line, &length, &number, &offset);
        if (result == READER_EOF) break;
        if (result == READER_ERROR) {
            snprintf(error, sizeof(error), "input read error");
            success = 0;
            break;
        }
        if (result != READER_LINE) continue;
        {
            LogEntry entry;
            int selected;
            if (!parse_record(state->format, state->custom_pattern, state->scratch,
                              line, length, number, offset, &entry)) continue;
            selected = record_selected(&entry, line, length,
                                       *state->field_filter, *state->raw_regex,
                                       *state->field_regex, state->regex_field_name);
            if (selected < 0) {
                snprintf(error, sizeof(error), "regex work limit exceeded at line %llu",
                         (unsigned long long)number);
                success = 0;
                break;
            }
            if (selected && state->search_regex != NULL) {
                size_t search_length = length < TUI_PREVIEW_BYTES ?
                                       length : TUI_PREVIEW_BYTES;
                RegexResult search_result = regex_search(state->search_regex,
                                                         line, search_length);
                if (search_result == REGEX_WORK_LIMIT) {
                    snprintf(error, sizeof(error), "search work limit exceeded at line %llu",
                             (unsigned long long)number);
                    success = 0;
                    break;
                }
                selected = search_result == REGEX_MATCH;
            }
            if (selected) {
                if (!output_write(&sink, &entry) || exported == UINT64_MAX) {
                    snprintf(error, sizeof(error), "cannot write export file");
                    success = 0;
                    break;
                }
                exported++;
            }
        }
    }
    if (!reader_reposition(state->reader, saved_offset, saved_line_number)) {
        snprintf(error, sizeof(error), "cannot restore input position");
        success = 0;
    }
    if (!output_finish(&sink)) {
        snprintf(error, sizeof(error), "cannot finish writing export file");
        success = 0;
    }
    if (success) {
        snprintf(state->status, sizeof(state->status),
                 "Exported %llu record%s to %.90s",
                 (unsigned long long)exported, exported == 1 ? "" : "s", path);
    } else {
        snprintf(state->status, sizeof(state->status), "Export failed: %.125s", error);
    }
    return 0;
}

static int handle_key(TuiState *state, TerminalKey key, Filter **filter,
                      Regex **raw_regex, Regex **field_regex,
                      const char *regex_field_name)
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
    if (state->values_visible) {
        if (key.type == TERMINAL_KEY_CHARACTER && key.character == 'q') return 1;
        if (key.type == TERMINAL_KEY_ESCAPE ||
            (key.type == TERMINAL_KEY_CHARACTER && key.character == 'v')) {
            state->values_visible = 0;
            state->status[0] = '\0';
        }
        return 0;
    }
    if (state->histogram_visible) {
        if (key.type == TERMINAL_KEY_CHARACTER && key.character == 'q') return 1;
        if (key.type == TERMINAL_KEY_ESCAPE ||
            (key.type == TERMINAL_KEY_CHARACTER && key.character == 'h')) {
            state->histogram_visible = 0;
            state->status[0] = '\0';
        } else if (key.type == TERMINAL_KEY_LEFT || key.type == TERMINAL_KEY_UP) {
            move_histogram_cursor(state, -1);
        } else if (key.type == TERMINAL_KEY_RIGHT || key.type == TERMINAL_KEY_DOWN) {
            move_histogram_cursor(state, 1);
        } else if (key.type == TERMINAL_KEY_HOME || key.type == TERMINAL_KEY_END) {
            size_t index = key.type == TERMINAL_KEY_HOME ? 0 :
                           LOGVIEW_TIMELINE_BINS - 1;
            for (;;) {
                if (state->histogram_counts[index] != 0) {
                    state->histogram_cursor = index;
                    break;
                }
                if (key.type == TERMINAL_KEY_HOME) {
                    if (++index >= LOGVIEW_TIMELINE_BINS) break;
                } else {
                    if (index == 0) break;
                    index--;
                }
            }
        } else if (key.type == TERMINAL_KEY_ENTER) {
            jump_to_histogram_bucket(state);
        }
        return 0;
    }
    if (state->help_visible) {
        state->help_visible = 0;
        return 0;
    }
    if (key.type == TERMINAL_KEY_ENTER) {
        navigate_match(state, 1);
    } else if (key.type == TERMINAL_KEY_SHIFT_ENTER) {
        navigate_match(state, -1);
    }
    if (key.type == TERMINAL_KEY_ESCAPE && state->search_navigation_active) {
        state->search_navigation_active = 0;
        snprintf(state->status, sizeof(state->status), "Search cancelled");
    }
    if (key.type == TERMINAL_KEY_CHARACTER) {
        if (key.character == 'q') return 1;
        if (key.character == '?') state->help_visible = 1;
        else if (key.character == 'J') jump_to_line(state);
        else if (key.character == 'z') return zoom_selected_record(state);
        else if (key.character == 'v') {
            int action = collect_field_summary(state);
            if (action == 1) return 1;
        }
        else if (key.character == 'h') {
            int action = build_histogram(state);
            if (action == 1) return 1;
        }
        else if (key.character == 'C') {
            int changed = state->search_regex != NULL || *filter != NULL ||
                          *raw_regex != NULL || *field_regex != NULL;
            state->search_navigation_active = 0;
            regex_free(state->search_regex);
            state->search_regex = NULL;
            state->search_text[0] = '\0';
            filter_free(*filter);
            *filter = NULL;
            regex_free(*raw_regex);
            *raw_regex = NULL;
            regex_free(*field_regex);
            *field_regex = NULL;
            if (changed) {
                if (!restart_scan(state)) return 0;
                return 2;
            }
            state->cursor = oldest_sequence(state);
            state->context_visible = 0;
            snprintf(state->status, sizeof(state->status), "Search and filters cleared");
        }
        else if (key.character == 'c') {
            if (state->scanning) {
                snprintf(state->status, sizeof(state->status), "Context is available after the scan completes");
            } else {
                state->context_visible = !state->context_visible;
                state->status[0] = '\0';
            }
        }
        else if (key.character == 'b') toggle_bookmark(state);
        else if (key.character == 'B') jump_to_next_bookmark(state);
        else if (key.character == '+' || key.character == '=') {
            if (state->context_before < TUI_CONTEXT_MAX) state->context_before++;
            if (state->context_after < TUI_CONTEXT_MAX) state->context_after++;
            if (state->context_visible) state->context_count = 0;
            snprintf(state->status, sizeof(state->status),
                     "Context size: %u before, %u after",
                     (unsigned)state->context_before,
                     (unsigned)state->context_after);
        }
        else if (key.character == '-') {
            if (state->context_before != 0) state->context_before--;
            if (state->context_after != 0) state->context_after--;
            if (state->context_visible) state->context_count = 0;
            snprintf(state->status, sizeof(state->status),
                     "Context size: %u before, %u after",
                     (unsigned)state->context_before,
                     (unsigned)state->context_after);
        }
        else if (key.character == '/') prompt_search(state);
        else if (key.character == 'x') export_view(state);
        else if (key.character == 'n') navigate_match(state, 1);
        else if (key.character == 'N') navigate_match(state, -1);
        else if (key.character == 'e') navigate_level(state, TUI_LEVEL_ERROR, 1);
        else if (key.character == 'E') navigate_level(state, TUI_LEVEL_ERROR, -1);
        else if (key.character == 'w') navigate_level(state, TUI_LEVEL_WARNING, 1);
        else if (key.character == 'W') navigate_level(state, TUI_LEVEL_WARNING, -1);
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
            return restart_scan(state) ? 2 : 0;
        }
    } else if (key.type == TERMINAL_KEY_UP && state->cursor > oldest) state->cursor--;
    else if (key.type == TERMINAL_KEY_DOWN && state->cursor + 1 < state->total_rows) state->cursor++;
    else if (key.type == TERMINAL_KEY_PAGE_UP) state->cursor = state->cursor > oldest + page ? state->cursor - page : oldest;
    else if (key.type == TERMINAL_KEY_PAGE_DOWN) state->cursor = state->cursor + page < state->total_rows ? state->cursor + page : state->total_rows - (state->total_rows != 0);
    else if (key.type == TERMINAL_KEY_HOME) state->cursor = oldest;
    else if (key.type == TERMINAL_KEY_END && state->total_rows != 0) state->cursor = state->total_rows - 1;
    (void)regex_field_name;
    return 0;
}

static int wait_for_follow(TuiState *state, int *should_quit)
{
    snprintf(state->status, sizeof(state->status), "Following file; press q to stop");
    render(state);
    while (!*should_quit && !logview_stop_requested) {
        TerminalKey key = terminal_read_key(state->terminal,
                                            state->search_navigation_active ? 0 : 250);
        int action = handle_key(state, key, state->field_filter,
                                state->raw_regex, state->field_regex,
                                state->regex_field_name);
        int search_redraw;
        FollowResult follow_result;
        if (action == 1) {
            *should_quit = 1;
            return 0;
        }
        if (action == 2) {
            state->follow_waiting = 0;
            return 1;
        }
        search_redraw = process_search_navigation(state);
        if (key.type != TERMINAL_KEY_NONE || search_redraw ||
            terminal_size_changed(state)) render(state);
        if (!reader_reposition(state->reader, state->follow_resume_offset,
                               state->follow_line_number_base)) return -1;
        follow_result = follow_poll(&state->input, state->follow_path,
                                    state->reader, state->follow_observed_end,
                                    state->follow_pending_partial,
                                    &state->follow_line_number_base);
        if (follow_result == FOLLOW_STOPPED) {
            *should_quit = 1;
            return 0;
        }
        if (follow_result == FOLLOW_ERROR) return -1;
        if (follow_result == FOLLOW_IDLE) continue;
        if (follow_result == FOLLOW_RESTART) {
            int had_bookmarks = state->bookmark_count != 0;
            if (!reset_indexes(state)) return -1;
            state->bookmark_count = 0;
            state->scanning = 1;
            state->follow_observed_end = 0;
            state->follow_resume_offset = 0;
            state->follow_line_number_base = 0;
            state->follow_pending_partial = 0;
            snprintf(state->status, sizeof(state->status), "%s",
                     had_bookmarks ?
                     "Log rotated or truncated; bookmarks cleared and rescanning" :
                     "Log rotated or truncated; rescanning");
            render(state);
            state->follow_waiting = 0;
            return 1;
        }
        state->follow_resume_offset = state->reader->next_offset;
        state->follow_line_number_base = state->reader->next_line_number;
        state->follow_pending_partial = 0;
        state->follow_waiting = 0;
        snprintf(state->status, sizeof(state->status), "Following file; press q to stop");
        return 1;
    }
    *should_quit = 1;
    return 0;
}

int tui_run(FILE **input_file, LineReader *reader, InputFormat format,
            const char *custom_pattern, JsonScratch *scratch,
            Filter **field_filter, Regex **raw_regex, Regex **field_regex,
            const char *regex_field_name, int follow_enabled,
            size_t result_limit, int bottom_limit,
            size_t context_before, size_t context_after,
            const StatsTimestampConfig *timestamp_config,
            const char *input_path,
            const char *follow_path)
{
    TuiState state;
    int should_quit = 0;
    int result_code = 0;
    memset(&state, 0, sizeof(state));
    state.index_file = tmpfile();
    state.physical_index_file = tmpfile();
    state.result_limit = result_limit;
    state.bottom_limit = bottom_limit;
    state.context_before = context_before > TUI_CONTEXT_MAX ?
                           TUI_CONTEXT_MAX : context_before;
    state.context_after = context_after > TUI_CONTEXT_MAX ?
                          TUI_CONTEXT_MAX : context_after;
    if (result_limit != 0) {
        state.limited_rows = (TuiCheckpoint *)calloc(result_limit,
                                                     sizeof(*state.limited_rows));
    }
    if (state.index_file == NULL || state.physical_index_file == NULL) {
        fputs("logview: cannot create a temporary TUI navigation index.\n", stderr);
        if (state.index_file != NULL) fclose(state.index_file);
        if (state.physical_index_file != NULL) fclose(state.physical_index_file);
        free(state.limited_rows);
        return 2;
    }
    if (result_limit != 0 && state.limited_rows == NULL) {
        fputs("logview: cannot allocate the limited TUI result index.\n", stderr);
        fclose(state.index_file);
        fclose(state.physical_index_file);
        free(state.limited_rows);
        return 2;
    }
    state.input = *input_file;
    state.reader = reader;
    state.format = format;
    state.custom_pattern = custom_pattern;
    state.scratch = scratch;
    state.field_filter = field_filter;
    state.raw_regex = raw_regex;
    state.field_regex = field_regex;
    state.regex_field_name = regex_field_name;
    state.follow_enabled = follow_enabled;
    state.input_path = input_path;
    state.follow_path = follow_path;
    if (timestamp_config != NULL) state.timestamp_config = *timestamp_config;
    if (!terminal_enter(&state.terminal)) {
        fputs("logview: --tui requires an interactive input and output terminal.\n", stderr);
        fclose(state.index_file);
        fclose(state.physical_index_file);
        free(state.limited_rows);
        return 2;
    }
    if (!reader_reposition(reader, 0, 0)) {
        terminal_leave(state.terminal);
        fclose(state.index_file);
        fclose(state.physical_index_file);
        free(state.limited_rows);
        return 2;
    }
    state.scanning = 1;
    snprintf(state.status, sizeof(state.status), "Scanning file; progress updates every 65,536 records");
    render(&state);
    while (!should_quit && !logview_stop_requested) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        if (state.follow_waiting) {
            int wait_result = wait_for_follow(&state, &should_quit);
            if (wait_result < 0) {
                result_code = 2;
                snprintf(state.status, sizeof(state.status), "Follow read error");
                render(&state);
                break;
            }
            if (should_quit) break;
            continue;
        }
        ReaderResult result = format == FORMAT_JSONL ?
            reader_next_json(reader, &line, &length, &number, &offset,
                             index_physical_line, &state) :
            reader_next(reader, &line, &length, &number, &offset);
        if (result == READER_ERROR) {
            result_code = 2;
            snprintf(state.status, sizeof(state.status), "Input read error");
            state.scanning = 0;
            render(&state);
            break;
        }
        if (result == READER_EOF) {
            if (!finalize_bottom_rows(&state)) {
                result_code = 2;
                snprintf(state.status, sizeof(state.status), "Cannot finalize the last records index");
                state.scanning = 0;
                render(&state);
                break;
            }
            if (state.follow_enabled) {
                state.scanning = 0;
                state.follow_observed_end = reader->next_offset;
                state.follow_resume_offset = reader->next_offset;
                state.follow_line_number_base = reader->next_line_number;
                state.follow_pending_partial = 0;
                state.follow_waiting = 1;
                continue;
            }
            state.scanning = 0;
            snprintf(state.status, sizeof(state.status), "Scan complete");
            if (state.search_regex != NULL) {
                start_search_navigation(&state, 1, 1);
            }
            render(&state);
            while (!should_quit && !logview_stop_requested) {
                TerminalKey key = terminal_read_key(
                    state.terminal, state.search_navigation_active ? 0 : 250);
                int action = handle_key(&state, key,
                                        field_filter, raw_regex, field_regex,
                                        regex_field_name);
                int search_redraw;
                if (action == 1) should_quit = 1;
                else if (action == 2) break;
                else {
                    search_redraw = process_search_navigation(&state);
                    if (key.type != TERMINAL_KEY_NONE || search_redraw ||
                        terminal_size_changed(&state)) render(&state);
                }
            }
            continue;
        }
        if (state.follow_enabled && result == READER_LINE && !reader->line_terminated) {
            state.follow_observed_end = reader->next_offset;
            state.follow_resume_offset = offset;
            state.follow_line_number_base = number - 1;
            state.follow_pending_partial = 1;
            if (!reader_reposition(reader, offset, number - 1)) {
                result_code = 2;
                snprintf(state.status, sizeof(state.status), "Cannot retain incomplete log record");
                break;
            }
            state.follow_waiting = 1;
            continue;
        }
        state.physical_records = format == FORMAT_JSONL ? reader->next_line_number : number;
        if (format != FORMAT_JSONL &&
            !add_physical_checkpoint(&state, number, offset)) {
            snprintf(state.status, sizeof(state.status), "Cannot write temporary context index");
            state.scanning = 0;
            render(&state);
            terminal_leave(state.terminal);
            regex_free(state.search_regex);
            fclose(state.index_file);
            fclose(state.physical_index_file);
            free(state.limited_rows);
            *input_file = state.input;
            return 2;
        }
        if (result == READER_LINE) {
            LogEntry entry;
            int selected;
            if (parse_record(format, custom_pattern, scratch, line, length,
                             number, offset, &entry)) {
                selected = record_selected(&entry, line, length, *field_filter,
                                           *raw_regex, *field_regex,
                                           regex_field_name);
            if (selected < 0) {
                    snprintf(state.status, sizeof(state.status), "Regex work limit exceeded at line %llu",
                             (unsigned long long)number);
                    state.scanning = 0;
                    render(&state);
                    terminal_leave(state.terminal);
                    regex_free(state.search_regex);
                    fclose(state.index_file);
                    fclose(state.physical_index_file);
                    free(state.limited_rows);
                    *input_file = state.input;
                    return 2;
                }
                if (selected && !add_row(&state, number, offset,
                                         classify_entry(&entry), line, length)) {
                    snprintf(state.status, sizeof(state.status), "Cannot write temporary navigation index");
                    state.scanning = 0;
                    render(&state);
                    terminal_leave(state.terminal);
                    regex_free(state.search_regex);
                    fclose(state.index_file);
                    fclose(state.physical_index_file);
                    free(state.limited_rows);
                    *input_file = state.input;
                    return 2;
                }
            }
        }
        if ((number & 127u) == 0) {
            TerminalKey key = terminal_read_key(state.terminal, 0);
            int action = handle_key(&state, key,
                                    field_filter, raw_regex, field_regex,
                                    regex_field_name);
            int search_redraw;
            if (action == 1) should_quit = 1;
            else if (action == 2) render(&state);
            else {
                search_redraw = process_search_navigation(&state);
                if (key.type != TERMINAL_KEY_NONE || search_redraw ||
                    terminal_size_changed(&state)) render(&state);
            }
        }
        if (state.scanning && (number & 65535u) == 0) {
            render_scan_progress(&state);
        }
    }
    terminal_leave(state.terminal);
    regex_free(state.search_regex);
    fclose(state.index_file);
    fclose(state.physical_index_file);
    free(state.limited_rows);
    *input_file = state.input;
    return result_code;
}
