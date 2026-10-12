#include "parsers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const unsigned char *value;
    size_t length;
} FieldInput;

static int is_digit(unsigned char value)
{
    return value >= '0' && value <= '9';
}

static int add_field(LogEntry *entry, const char *name,
                     const unsigned char *value, size_t length)
{
    LogField *field;
    if (entry->field_count >= LOGVIEW_MAX_FIELDS) {
        return 0;
    }
    field = &entry->fields[entry->field_count++];
    field->name.data = (const unsigned char *)name;
    field->name.length = strlen(name);
    field->value.data = value;
    field->value.length = length;
    return 1;
}

static int add_slice(LogEntry *entry, const char *name, size_t start,
                     size_t length)
{
    return add_field(entry, name, entry->raw.data + start, length);
}

static void trim_span(const unsigned char *data, size_t *start, size_t *length)
{
    while (*length != 0 && (data[*start] == ' ' || data[*start] == '\t')) {
        (*start)++;
        (*length)--;
    }
    while (*length != 0 &&
           (data[*start + *length - 1] == ' ' ||
            data[*start + *length - 1] == '\t')) {
        (*length)--;
    }
}

static int parse_uint(const unsigned char *data, size_t start, size_t length,
                      unsigned maximum, unsigned *value)
{
    size_t index;
    unsigned result = 0;
    if (length == 0) {
        return 0;
    }
    for (index = 0; index < length; index++) {
        if (!is_digit(data[start + index])) {
            return 0;
        }
        result = result * 10u + (unsigned)(data[start + index] - '0');
        if (result > maximum) {
            return 0;
        }
    }
    *value = result;
    return 1;
}

static int parse_token(const unsigned char *data, size_t length,
                       size_t *position, size_t *start, size_t *token_length)
{
    size_t cursor = *position;
    while (cursor < length && data[cursor] == ' ') {
        cursor++;
    }
    *start = cursor;
    while (cursor < length && data[cursor] != ' ') {
        cursor++;
    }
    if (cursor == *start) {
        return 0;
    }
    *token_length = cursor - *start;
    *position = cursor;
    return 1;
}

static int parse_quoted(const unsigned char *data, size_t length,
                        size_t *position, size_t *start, size_t *value_length)
{
    size_t cursor = *position;
    if (cursor >= length || data[cursor] != '"') {
        return 0;
    }
    *start = ++cursor;
    while (cursor < length) {
        if (data[cursor] == '\\') {
            cursor += cursor + 1 < length ? 2 : 1;
        } else if (data[cursor] == '"') {
            *value_length = cursor - *start;
            *position = cursor + 1;
            return 1;
        } else {
            cursor++;
        }
    }
    return 0;
}

static int parse_http(LogEntry *entry, int combined)
{
    const unsigned char *data = entry->raw.data;
    size_t length = entry->raw.length;
    size_t position = 0;
    size_t start[8];
    size_t token_length[8];
    size_t count = 0;
    size_t date_start;
    size_t date_length;
    size_t request_start;
    size_t request_length;
    unsigned status;

    if (!parse_token(data, length, &position, &start[count], &token_length[count])) return 0;
    count++;
    if (!parse_token(data, length, &position, &start[count], &token_length[count])) return 0;
    count++;
    if (!parse_token(data, length, &position, &start[count], &token_length[count])) return 0;
    count++;
    while (position < length && data[position] == ' ') position++;
    if (position >= length || data[position++] != '[') return 0;
    date_start = position;
    while (position < length && data[position] != ']') position++;
    if (position >= length) return 0;
    date_length = position - date_start;
    position++;
    while (position < length && data[position] == ' ') position++;
    if (!parse_quoted(data, length, &position, &request_start, &request_length)) return 0;
    if (!parse_token(data, length, &position, &start[count], &token_length[count])) return 0;
    count++;
    if (!parse_uint(data, start[3], token_length[3], 999, &status)) return 0;
    if (!parse_token(data, length, &position, &start[count], &token_length[count])) return 0;
    count++;
    if (!(token_length[4] == 1 && data[start[4]] == '-') &&
        !parse_uint(data, start[4], token_length[4], 0xffffffffu, &status)) {
        return 0;
    }

    entry->field_count = 0;
    if (!add_slice(entry, "client_ip", start[0], token_length[0]) ||
        !add_slice(entry, "remote_ident", start[1], token_length[1]) ||
        !add_slice(entry, "user", start[2], token_length[2]) ||
        !add_slice(entry, "timestamp", date_start, date_length) ||
        !add_slice(entry, "request", request_start, request_length) ||
        !add_slice(entry, "status", start[3], token_length[3]) ||
        !add_slice(entry, "bytes", start[4], token_length[4])) return 0;

    if (combined) {
        size_t referrer_start;
        size_t referrer_length;
        size_t agent_start;
        size_t agent_length;
        while (position < length && data[position] == ' ') position++;
        if (!parse_quoted(data, length, &position, &referrer_start, &referrer_length)) return 0;
        while (position < length && data[position] == ' ') position++;
        if (!parse_quoted(data, length, &position, &agent_start, &agent_length)) return 0;
        while (position < length && data[position] == ' ') position++;
        if (position != length ||
            !add_slice(entry, "referrer", referrer_start, referrer_length) ||
            !add_slice(entry, "user_agent", agent_start, agent_length)) return 0;
    } else {
        while (position < length && data[position] == ' ') position++;
        if (position != length) return 0;
    }
    entry->parse_ok = 1;
    return 1;
}

static int month_number(const unsigned char *data, size_t start)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    size_t index;
    for (index = 0; index < 12; index++) {
        if (memcmp(data + start, months[index], 3) == 0) return (int)index + 1;
    }
    return 0;
}

static int parse_syslog3164(LogEntry *entry)
{
    const unsigned char *data = entry->raw.data;
    size_t length = entry->raw.length;
    size_t position = 0;
    size_t host_start;
    size_t host_length;
    size_t tag_start;
    size_t tag_length;
    size_t message_start;
    if (length < 16 || !month_number(data, 0) || data[3] != ' ') return 0;
    if (!(is_digit(data[4]) || data[4] == ' ') ||
        !is_digit(data[5]) || data[6] != ' ' ||
        !is_digit(data[7]) || !is_digit(data[8]) || data[9] != ':' ||
        !is_digit(data[10]) || !is_digit(data[11]) || data[12] != ':' ||
        !is_digit(data[13]) || !is_digit(data[14]) || data[15] != ' ') return 0;
    position = 16;
    if (!parse_token(data, length, &position, &host_start, &host_length)) return 0;
    while (position < length && data[position] == ' ') position++;
    tag_start = position;
    while (position < length && data[position] != ':' && data[position] != '[') position++;
    if (position == tag_start || position >= length ||
        (data[position] != ':' && data[position] != '[')) return 0;
    tag_length = position - tag_start;
    trim_span(data, &tag_start, &tag_length);
    if (tag_length == 0) return 0;
    entry->field_count = 0;
    if (position < length && data[position] == '[') {
        size_t pid_start = ++position;
        unsigned pid;
        while (position < length && is_digit(data[position])) position++;
        if (position == pid_start || position >= length || data[position++] != ']' ||
            !parse_uint(data, pid_start, position - pid_start - 1, 0x7fffffffu, &pid)) return 0;
        if (!add_slice(entry, "pid", pid_start, position - pid_start - 1)) return 0;
    }
    if (position >= length || data[position++] != ':') return 0;
    if (position < length && data[position] == ' ') position++;
    message_start = position;
    if (!add_slice(entry, "timestamp", 0, 15) ||
        !add_slice(entry, "host", host_start, host_length) ||
        !add_slice(entry, "process", tag_start, tag_length) ||
        !add_slice(entry, "message", message_start, length - message_start)) return 0;
    entry->parse_ok = 1;
    return 1;
}

static int parse_syslog5424(LogEntry *entry)
{
    const unsigned char *data = entry->raw.data;
    size_t length = entry->raw.length;
    size_t position = 0;
    size_t pri_start;
    size_t pri_length;
    size_t starts[7];
    size_t lengths[7];
    unsigned pri;
    unsigned severity;
    static const char *levels[] = {"EMERG", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"};
    size_t sd_start;
    size_t sd_length;
    size_t message_start;
    size_t index;
    if (length < 8 || data[position++] != '<') return 0;
    pri_start = position;
    while (position < length && is_digit(data[position])) position++;
    pri_length = position - pri_start;
    if (position >= length || data[position++] != '>' ||
        !parse_uint(data, pri_start, pri_length, 191, &pri) || position >= length ||
        !is_digit(data[position++]) || position >= length || data[position++] != ' ') return 0;
    for (index = 0; index < 5; index++) {
        if (!parse_token(data, length, &position, &starts[index], &lengths[index])) return 0;
    }
    while (position < length && data[position] == ' ') position++;
    if (position >= length) return 0;
    if (data[position] == '-') {
        sd_start = position++;
        sd_length = 1;
    } else if (data[position] == '[') {
        sd_start = position;
        for (;;) {
            int in_quote = 0;
            int escaped = 0;
            if (position >= length || data[position++] != '[') return 0;
            while (position < length) {
                unsigned char byte = data[position++];
                if (escaped) {
                    escaped = 0;
                    continue;
                }
                if (byte == '\\' && in_quote) {
                    escaped = 1;
                    continue;
                }
                if (byte == '"') {
                    in_quote = !in_quote;
                    continue;
                }
                if (byte == ']' && !in_quote) break;
            }
            if (position > length || data[position - 1] != ']') return 0;
            if (position >= length || data[position] != '[') break;
        }
        sd_length = position - sd_start;
    } else return 0;
    if (position < length && data[position] == ' ') position++;
    message_start = position;
    entry->field_count = 0;
    severity = pri % 8;
    if (!add_slice(entry, "timestamp", starts[0], lengths[0]) ||
        !add_slice(entry, "host", starts[1], lengths[1]) ||
        !add_slice(entry, "process", starts[2], lengths[2]) ||
        !add_slice(entry, "pid", starts[3], lengths[3]) ||
        !add_slice(entry, "message_id", starts[4], lengths[4]) ||
        !add_slice(entry, "structured_data", sd_start, sd_length) ||
        !add_slice(entry, "message", message_start, length - message_start) ||
        !add_field(entry, "level", (const unsigned char *)levels[severity], strlen(levels[severity]))) return 0;
    entry->parse_ok = 1;
    return 1;
}

static int cri_timestamp_shape(const unsigned char *data, size_t length)
{
    static const unsigned char month_days[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    size_t position = 19;
    size_t fraction_digits = 0;
    size_t index;
    unsigned zone_hours;
    unsigned zone_minutes;
    unsigned year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    unsigned maximum_day;
    if (length < 20 || data[4] != '-' || data[7] != '-' ||
        (data[10] != 'T' && data[10] != 't') || data[13] != ':' ||
        data[16] != ':') return 0;
    for (index = 0; index < 19; index++) {
        if (index != 4 && index != 7 && index != 10 && index != 13 &&
            index != 16 && !is_digit(data[index])) return 0;
    }
    if (!parse_uint(data, 0, 4, 9999, &year) ||
        !parse_uint(data, 5, 2, 12, &month) || month == 0 ||
        !parse_uint(data, 8, 2, 31, &day) || day == 0 ||
        !parse_uint(data, 11, 2, 23, &hour) ||
        !parse_uint(data, 14, 2, 59, &minute) ||
        !parse_uint(data, 17, 2, 60, &second)) return 0;
    maximum_day = month_days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        maximum_day++;
    if (day > maximum_day) return 0;
    if (position >= length || (data[position] != '.' && data[position] != 'Z' &&
        data[position] != 'z' && data[position] != '+' && data[position] != '-')) return 0;
    if (data[position] == '.') {
        position++;
        while (position < length && is_digit(data[position])) {
            position++;
            fraction_digits++;
        }
        if (fraction_digits == 0 || fraction_digits > 9) return 0;
    }
    if (position < length && (data[position] == 'Z' || data[position] == 'z'))
        return position + 1 == length;
    if (position + 6 != length || (data[position] != '+' && data[position] != '-') ||
        !is_digit(data[position + 1]) || !is_digit(data[position + 2]) ||
        data[position + 3] != ':' || !is_digit(data[position + 4]) ||
        !is_digit(data[position + 5])) return 0;
    zone_hours = (unsigned)(data[position + 1] - '0') * 10u +
                 (unsigned)(data[position + 2] - '0');
    zone_minutes = (unsigned)(data[position + 4] - '0') * 10u +
                   (unsigned)(data[position + 5] - '0');
    return zone_hours <= 23 && zone_minutes <= 59;
}

static int parse_cri(LogEntry *entry)
{
    const unsigned char *data = entry->raw.data;
    size_t length = entry->raw.length;
    size_t timestamp_length = 0;
    size_t stream_start;
    size_t stream_length;
    size_t tag_start;
    size_t tag_length;
    size_t message_start;
    size_t position = 0;
    while (position < length && data[position] != ' ') position++;
    timestamp_length = position;
    if (!cri_timestamp_shape(data, timestamp_length)) return 0;
    if (position >= length || data[position++] != ' ') return 0;
    stream_start = position;
    while (position < length && data[position] != ' ') position++;
    stream_length = position - stream_start;
    if (!((stream_length == 6 && memcmp(data + stream_start, "stdout", 6) == 0) ||
          (stream_length == 6 && memcmp(data + stream_start, "stderr", 6) == 0))) return 0;
    if (position >= length || data[position++] != ' ') return 0;
    tag_start = position;
    while (position < length && data[position] != ' ') position++;
    tag_length = position - tag_start;
    if (tag_length == 0 || position >= length || data[position++] != ' ') return 0;
    message_start = position;
    entry->field_count = 0;
    if (!add_slice(entry, "timestamp", 0, timestamp_length) ||
        !add_slice(entry, "stream", stream_start, stream_length) ||
        !add_slice(entry, "tag", tag_start, tag_length) ||
        !add_slice(entry, "message", message_start, length - message_start)) return 0;
    entry->parse_ok = 1;
    return 1;
}

InputFormat parser_format_from_name(const char *name)
{
    if (strcmp(name, "raw") == 0) return FORMAT_RAW;
    if (strcmp(name, "jsonl") == 0) return FORMAT_JSONL;
    if (strcmp(name, "syslog3164") == 0) return FORMAT_SYSLOG3164;
    if (strcmp(name, "syslog5424") == 0) return FORMAT_SYSLOG5424;
    if (strcmp(name, "cri") == 0) return FORMAT_CRI;
    if (strcmp(name, "apache-common") == 0) return FORMAT_APACHE_COMMON;
    if (strcmp(name, "apache-combined") == 0) return FORMAT_APACHE_COMBINED;
    if (strcmp(name, "nginx-combined") == 0) return FORMAT_NGINX_COMBINED;
    if (strcmp(name, "custom") == 0) return FORMAT_CUSTOM;
    if (strcmp(name, "auto") == 0) return FORMAT_AUTO;
    return (InputFormat)-1;
}

int parser_format_line(InputFormat format, LogEntry *entry)
{
    entry->field_count = 0;
    entry->parse_ok = 0;
    switch (format) {
    case FORMAT_RAW:
        entry->parse_ok = 1;
        return 1;
    case FORMAT_SYSLOG3164: return parse_syslog3164(entry);
    case FORMAT_SYSLOG5424: return parse_syslog5424(entry);
    case FORMAT_CRI: return parse_cri(entry);
    case FORMAT_APACHE_COMMON: return parse_http(entry, 0);
    case FORMAT_APACHE_COMBINED:
    case FORMAT_NGINX_COMBINED: return parse_http(entry, 1);
    default: return 0;
    }
}

InputFormat parser_detect_format(const unsigned char *const *lines,
                                 const size_t *lengths, size_t count)
{
    static const InputFormat candidates[] = {FORMAT_JSONL, FORMAT_CRI, FORMAT_SYSLOG5424,
        FORMAT_SYSLOG3164, FORMAT_APACHE_COMMON, FORMAT_APACHE_COMBINED};
    unsigned scores[sizeof(candidates) / sizeof(candidates[0])] = {0};
    unsigned best = 0;
    size_t best_index = 0;
    size_t index;
    for (index = 0; index < count; index++) {
        size_t position = 0;
        while (position < lengths[index] &&
               (lines[index][position] == ' ' || lines[index][position] == '\t' ||
                lines[index][position] == '\r')) position++;
        if (position == lengths[index]) continue;
        if (lines[index][position] == '[') return FORMAT_JSONL;
        if (lines[index][position] == '{') {
            size_t end = lengths[index];
            while (end > position &&
                   (lines[index][end - 1] == ' ' || lines[index][end - 1] == '\t' ||
                    lines[index][end - 1] == '\r')) end--;
            if (end == position + 1 || lines[index][end - 1] != '}') {
                return FORMAT_JSONL;
            }
        }
        break;
    }
    for (index = 0; index < count; index++) {
        LogEntry entry;
        size_t candidate;
        entry.raw.data = lines[index];
        entry.raw.length = lengths[index];
        entry.field_count = 0;
        entry.parse_ok = 0;
        for (candidate = 1; candidate < sizeof(candidates) / sizeof(candidates[0]); candidate++) {
            if (parser_format_line(candidates[candidate], &entry)) scores[candidate]++;
        }
        if (lengths[index] > 0 && lines[index][0] == '{') scores[0]++;
    }
    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        if (scores[index] > best) {
            best = scores[index];
            best_index = index;
        } else if (scores[index] == best && best != 0) {
            return FORMAT_RAW;
        }
    }
    if (best == 0 || best * 4 < count * 3) return FORMAT_RAW;
    return candidates[best_index];
}

int parser_custom_validate(const char *pattern, char *error,
                           size_t error_capacity)
{
    const char *cursor = pattern;
    int captures = 0;
    char aliases[LOGVIEW_MAX_FIELDS][128];
    size_t alias_count = 0;
    if (pattern == NULL || *pattern == '\0') {
        snprintf(error, error_capacity, "custom pattern cannot be empty");
        return 0;
    }
    while (*cursor != '\0') {
        if (*cursor == '%') {
            const char *end;
            const char *name_start;
            const char *colon;
            const char *name_end;
            size_t token_length;
            int is_message;
            cursor++;
            if (*cursor++ != '{') goto invalid;
            name_start = cursor;
            end = strchr(cursor, '}');
            if (end == NULL || end == cursor) goto invalid;
            colon = memchr(name_start, ':', (size_t)(end - name_start));
            name_end = colon == NULL ? end : colon;
            token_length = (size_t)(name_end - name_start);
            is_message = token_length == 7 && memcmp(name_start, "MESSAGE", 7) == 0;
            if (!((token_length == 9 && memcmp(name_start, "TIMESTAMP", 9) == 0) ||
                  (token_length == 5 && memcmp(name_start, "LEVEL", 5) == 0) ||
                  is_message ||
                  (token_length == 4 && memcmp(name_start, "HOST", 4) == 0) ||
                  (token_length == 4 && memcmp(name_start, "PROC", 4) == 0) ||
                  (token_length == 3 && memcmp(name_start, "PID", 3) == 0) ||
                  (token_length == 5 && memcmp(name_start, "MONTH", 5) == 0) ||
                  (token_length == 3 && memcmp(name_start, "DAY", 3) == 0) ||
                  (token_length == 4 && memcmp(name_start, "TIME", 4) == 0) ||
                  (token_length == 5 && memcmp(name_start, "FIELD", 5) == 0 && colon != NULL))) goto invalid;
            if (colon != NULL) {
                const char *alias = colon + 1;
                const char *scan;
                size_t alias_length = (size_t)(end - alias);
                size_t alias_index;
                if (alias == end || !((*alias >= 'A' && *alias <= 'Z') ||
                    (*alias >= 'a' && *alias <= 'z') || *alias == '_')) goto invalid;
                for (scan = alias + 1; scan < end; scan++) {
                    if (!( (*scan >= 'A' && *scan <= 'Z') || (*scan >= 'a' && *scan <= 'z') ||
                           (*scan >= '0' && *scan <= '9') || *scan == '_')) goto invalid;
                }
                if (alias_length >= sizeof(aliases[0]) ||
                    alias_count >= LOGVIEW_MAX_FIELDS) goto invalid;
                for (alias_index = 0; alias_index < alias_count; alias_index++) {
                    if (strlen(aliases[alias_index]) == alias_length &&
                        memcmp(aliases[alias_index], alias, alias_length) == 0) {
                        goto invalid;
                    }
                }
                memcpy(aliases[alias_count], alias, alias_length);
                aliases[alias_count][alias_length] = '\0';
                alias_count++;
            }
            if (is_message) {
                if (end[1] != '\0') goto invalid;
            } else if (end[1] == '\0' || end[1] == '%') {
                goto invalid;
            }
            captures++;
            cursor = end + 1;
        } else cursor++;
    }
    if (captures == 0) goto invalid;
    return 1;
invalid:
    snprintf(error, error_capacity, "invalid or unsupported custom placeholder");
    return 0;
}

int parser_custom_line(const char *pattern, LogEntry *entry)
{
    static char stable_names[LOGVIEW_MAX_FIELDS][128];
    size_t pattern_position = 0;
    size_t line_position = 0;
    size_t pattern_length = strlen(pattern);
    const unsigned char *line = entry->raw.data;
    size_t line_length = entry->raw.length;
    entry->field_count = 0;
    while (pattern_position < pattern_length) {
        if (pattern[pattern_position] != '%') {
            if (line_position >= line_length || line[line_position] != (unsigned char)pattern[pattern_position]) return 0;
            pattern_position++;
            line_position++;
        } else {
            const char *name_start;
            const char *name_end;
            const char *next_literal;
            size_t capture_start;
            size_t capture_end;
            char field_name[128];
            size_t field_length;
            pattern_position += 2;
            name_start = pattern + pattern_position;
            name_end = strchr(name_start, '}');
            if (name_end == NULL) return 0;
            next_literal = name_end + 1;
            while (*next_literal != '\0' && *next_literal != '%') next_literal++;
            capture_start = line_position;
            if (next_literal == name_end + 1) {
                capture_end = line_length;
                line_position = line_length;
            } else {
                size_t literal_length = (size_t)(next_literal - (name_end + 1));
                const unsigned char *found = NULL;
                size_t scan;
                for (scan = line_position; scan + literal_length <= line_length; scan++) {
                    if (memcmp(line + scan, name_end + 1, literal_length) == 0) { found = line + scan; break; }
                }
                if (found == NULL) return 0;
                capture_end = (size_t)(found - line);
                line_position = capture_end;
            }
            field_length = (size_t)(name_end - name_start);
            if (field_length >= sizeof(field_name)) return 0;
            memcpy(field_name, name_start, field_length);
            field_name[field_length] = '\0';
            if (strncmp(field_name, "FIELD:", 6) == 0) {
                memmove(field_name, field_name + 6, strlen(field_name + 6) + 1);
            } else {
                char *colon = strchr(field_name, ':');
                if (colon != NULL) *colon = '\0';
                if (strcmp(field_name, "TIMESTAMP") == 0) strcpy(field_name, "timestamp");
                else if (strcmp(field_name, "LEVEL") == 0) strcpy(field_name, "level");
                else if (strcmp(field_name, "MESSAGE") == 0) strcpy(field_name, "message");
                else if (strcmp(field_name, "HOST") == 0) strcpy(field_name, "host");
                else if (strcmp(field_name, "PROC") == 0) strcpy(field_name, "process");
                else if (strcmp(field_name, "PID") == 0) strcpy(field_name, "pid");
                else if (strcmp(field_name, "MONTH") == 0) strcpy(field_name, "month");
                else if (strcmp(field_name, "DAY") == 0) strcpy(field_name, "day");
                else if (strcmp(field_name, "TIME") == 0) strcpy(field_name, "time");
                else return 0;
            }
            if (entry->field_count >= LOGVIEW_MAX_FIELDS) return 0;
            strcpy(stable_names[entry->field_count], field_name);
            if (!add_slice(entry, stable_names[entry->field_count], capture_start,
                           capture_end - capture_start)) return 0;
            pattern_position = (size_t)(name_end + 1 - pattern);
        }
    }
    if (line_position != line_length) return 0;
    entry->parse_ok = 1;
    return 1;
}
