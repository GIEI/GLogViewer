#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "reader.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#endif

static int reader_fill(LineReader *reader)
{
    if (reader->input_position >= reader->input_length) {
        size_t amount;
        if (reader->input_eof || reader->input_error) {
            return 0;
        }
        amount = fread(reader->input, 1, sizeof(reader->input), reader->file);
        reader->input_position = 0;
        reader->input_length = amount;
        if (amount == 0) {
            if (ferror(reader->file)) {
                reader->input_error = 1;
            } else {
                reader->input_eof = 1;
            }
            return 0;
        }
    }
    return 1;
}

static int reader_grow_line(LineReader *reader)
{
    size_t capacity = reader->line_capacity;
    unsigned char *replacement;

    if (capacity >= reader->max_line_bytes) {
        return 0;
    }
    if (capacity == 0) {
        capacity = 4096;
    } else if (capacity > reader->max_line_bytes / 2) {
        capacity = reader->max_line_bytes;
    } else {
        capacity *= 2;
    }
    if (capacity > reader->max_line_bytes) {
        capacity = reader->max_line_bytes;
    }
    replacement = (unsigned char *)realloc(reader->line, capacity);
    if (replacement == NULL) {
        return 0;
    }
    reader->line = replacement;
    reader->line_capacity = capacity;
    return 1;
}

int reader_init(LineReader *reader, FILE *file, size_t max_line_bytes)
{
    memset(reader, 0, sizeof(*reader));
    if (file == NULL || max_line_bytes == 0 ||
        max_line_bytes > LOGVIEW_MAX_LINE_BYTES) {
        return 0;
    }
    reader->file = file;
    reader->max_line_bytes = max_line_bytes;
    return 1;
}

void reader_destroy(LineReader *reader)
{
    free(reader->line);
    free(reader->json_record);
    memset(reader, 0, sizeof(*reader));
}

static void reader_reset_json(LineReader *reader)
{
    int document_hint = reader->json_document_hint;
    reader->json_record_length = 0;
    reader->json_pending_line = NULL;
    reader->json_pending_length = 0;
    reader->json_pending_position = 0;
    reader->json_pending_number = 0;
    reader->json_pending_offset = 0;
    reader->json_pending_result = READER_EOF;
    reader->json_pending_terminated = 0;
    reader->json_pending = 0;
    reader->json_mode_checked = document_hint;
    reader->json_document_mode = document_hint;
    reader->json_active = 0;
    reader->json_in_string = 0;
    reader->json_escape = 0;
    reader->json_outside_string = 0;
    reader->json_outside_escape = 0;
    reader->json_oversized = 0;
    reader->json_depth = 0;
    reader->json_record_line = 0;
    reader->json_record_offset = 0;
}

int reader_reposition(LineReader *reader, uint64_t offset,
                      uint64_t last_line_number)
{
#ifdef _WIN32
    if (_fseeki64(reader->file, (__int64)offset, SEEK_SET) != 0) return 0;
#else
    if (fseeko(reader->file, (off_t)offset, SEEK_SET) != 0) return 0;
#endif
    clearerr(reader->file);
    reader->input_length = 0;
    reader->input_position = 0;
    reader->next_offset = offset;
    reader->next_line_number = last_line_number;
    reader->input_eof = 0;
    reader->input_error = 0;
    reader->line_length = 0;
    reader->line_terminated = 0;
    reader_reset_json(reader);
    return 1;
}

ReaderResult reader_next(LineReader *reader, const unsigned char **line,
                         size_t *line_length, uint64_t *line_number,
                         uint64_t *byte_offset)
{
    size_t length = 0;
    uint64_t start_offset = reader->next_offset;
    int saw_byte = 0;
    int ended_with_lf = 0;
    int too_long = 0;

    for (;;) {
        const unsigned char *start;
        const unsigned char *newline;
        size_t available;
        size_t amount;
        size_t consumed;

        if (!reader_fill(reader)) {
            if (reader->input_error) {
                return READER_ERROR;
            }
            if (!saw_byte) {
                return READER_EOF;
            }
            break;
        }

        start = reader->input + reader->input_position;
        available = reader->input_length - reader->input_position;
        newline = (const unsigned char *)memchr(start, '\n', available);
        amount = newline != NULL ? (size_t)(newline - start) : available;
        consumed = amount + (newline != NULL ? 1u : 0u);
        saw_byte = 1;
        reader->input_position += consumed;
        reader->next_offset += consumed;

        if (!too_long) {
            if (amount > reader->max_line_bytes - length) {
                too_long = 1;
                length = 0;
            } else if (amount != 0) {
                while (length + amount > reader->line_capacity) {
                    if (!reader_grow_line(reader)) {
                        return READER_ERROR;
                    }
                }
                memcpy(reader->line + length, start, amount);
                length += amount;
            }
        }
        if (newline != NULL) {
            ended_with_lf = 1;
            break;
        }
    }

    reader->next_line_number++;
    if (too_long) {
        reader->line_length = 0;
        reader->line_terminated = ended_with_lf;
        *line = reader->line;
        *line_length = 0;
        *line_number = reader->next_line_number;
        *byte_offset = start_offset;
        return READER_TOO_LONG;
    }
    if (ended_with_lf && length > 0 && reader->line[length - 1] == '\r') {
        length--;
    }

    reader->line_length = length;
    reader->line_terminated = ended_with_lf;
    *line = reader->line;
    *line_length = length;
    *line_number = reader->next_line_number;
    *byte_offset = start_offset;
    return READER_LINE;
}

static int json_record_reserve(LineReader *reader, size_t required)
{
    size_t capacity = reader->json_record_capacity;
    unsigned char *replacement;
    if (required > reader->max_line_bytes) return 0;
    if (required <= capacity) return 1;
    if (capacity == 0) {
        capacity = reader->max_line_bytes < 4096 ? reader->max_line_bytes : 4096;
    }
    while (capacity < required) {
        if (capacity > reader->max_line_bytes / 2) {
            capacity = reader->max_line_bytes;
        } else {
            capacity *= 2;
        }
    }
    replacement = (unsigned char *)realloc(reader->json_record, capacity);
    if (replacement == NULL) return -1;
    reader->json_record = replacement;
    reader->json_record_capacity = capacity;
    return 1;
}

static int json_record_append(LineReader *reader, unsigned char byte)
{
    int reserve_result;
    if (reader->json_oversized) return 1;
    reserve_result = json_record_reserve(reader, reader->json_record_length + 1);
    if (reserve_result < 0) return -1;
    if (reserve_result == 0) {
        reader->json_oversized = 1;
        return 1;
    }
    reader->json_record[reader->json_record_length++] = byte;
    return 1;
}

static int json_object_is_multiline(const unsigned char *line, size_t length,
                                    size_t start)
{
    size_t depth = 0;
    size_t index;
    int in_string = 0;
    int escape = 0;
    for (index = start; index < length; index++) {
        unsigned char byte = line[index];
        if (in_string) {
            if (escape) escape = 0;
            else if (byte == '\\') escape = 1;
            else if (byte == '"') in_string = 0;
        } else if (byte == '"') {
            in_string = 1;
        } else if (byte == '{' || byte == '[') {
            depth++;
        } else if (byte == '}' || byte == ']') {
            if (depth != 0) depth--;
            if (depth == 0) return 0;
        }
    }
    return depth != 0;
}

static ReaderResult json_fetch_line(LineReader *reader,
                                    ReaderPhysicalLineCallback callback,
                                    void *callback_context)
{
    ReaderResult result = reader_next(reader, &reader->json_pending_line,
                                      &reader->json_pending_length,
                                      &reader->json_pending_number,
                                      &reader->json_pending_offset);
    if (result == READER_EOF || result == READER_ERROR) return result;
    if (callback != NULL &&
        !callback(callback_context, reader->json_pending_number,
                  reader->json_pending_offset)) return READER_ERROR;
    reader->json_pending_position = 0;
    reader->json_pending_result = result;
    reader->json_pending_terminated = reader->line_terminated;
    reader->json_pending = 1;
    return result;
}

static ReaderResult json_return_record(LineReader *reader,
                                      const unsigned char **record,
                                      size_t *record_length,
                                      uint64_t *line_number,
                                      uint64_t *byte_offset)
{
    ReaderResult result = reader->json_oversized ? READER_TOO_LONG : READER_LINE;
    *record = reader->json_record;
    *record_length = reader->json_oversized ? 0 : reader->json_record_length;
    *line_number = reader->json_record_line;
    *byte_offset = reader->json_record_offset;
    reader->json_active = 0;
    reader->json_in_string = 0;
    reader->json_escape = 0;
    reader->json_oversized = 0;
    reader->json_record_length = 0;
    reader->json_depth = 0;
    return result;
}

ReaderResult reader_next_json(LineReader *reader, const unsigned char **record,
                              size_t *record_length, uint64_t *line_number,
                              uint64_t *byte_offset,
                              ReaderPhysicalLineCallback callback,
                              void *callback_context)
{
    if (!reader->json_mode_checked) {
        for (;;) {
            ReaderResult result = json_fetch_line(reader, callback,
                                                  callback_context);
            size_t position = 0;
            if (result == READER_EOF || result == READER_ERROR) return result;
            if (result == READER_TOO_LONG) {
                reader->json_mode_checked = 1;
                reader->json_document_mode = 0;
                reader->json_pending = 0;
                *record = reader->line;
                *record_length = 0;
                *line_number = reader->json_pending_number;
                *byte_offset = reader->json_pending_offset;
                return READER_TOO_LONG;
            }
            while (position < reader->json_pending_length &&
                   (reader->json_pending_line[position] == ' ' ||
                    reader->json_pending_line[position] == '\t' ||
                    reader->json_pending_line[position] == '\r')) position++;
            if (position == reader->json_pending_length) {
                reader->json_pending = 0;
                continue;
            }
            reader->json_mode_checked = 1;
            if (reader->json_pending_line[position] == '[') {
                reader->json_document_mode = 1;
            } else if (reader->json_pending_line[position] == '{' &&
                       json_object_is_multiline(reader->json_pending_line,
                                                reader->json_pending_length,
                                                position)) {
                reader->json_document_mode = 1;
            }
            if (!reader->json_document_mode) {
                reader->json_document_hint = 0;
                *record = reader->json_pending_line;
                *record_length = reader->json_pending_length;
                *line_number = reader->json_pending_number;
                *byte_offset = reader->json_pending_offset;
                reader->json_pending = 0;
                return READER_LINE;
            }
            reader->json_document_hint = 1;
            break;
        }
    }
    if (!reader->json_document_mode) {
        ReaderResult result;
        if (reader->json_pending) {
            result = reader->json_pending_result;
            *record = reader->json_pending_line;
            *record_length = result == READER_LINE ? reader->json_pending_length : 0;
            *line_number = reader->json_pending_number;
            *byte_offset = reader->json_pending_offset;
            reader->json_pending = 0;
            return result;
        }
        result = json_fetch_line(reader, callback, callback_context);
        if (result == READER_EOF || result == READER_ERROR) return result;
        *record = reader->json_pending_line;
        *record_length = result == READER_LINE ? reader->json_pending_length : 0;
        *line_number = reader->json_pending_number;
        *byte_offset = reader->json_pending_offset;
        reader->json_pending = 0;
        return result;
    }
    for (;;) {
        unsigned char byte;
        if (!reader->json_pending) {
            ReaderResult result = json_fetch_line(reader, callback,
                                                  callback_context);
            if (result == READER_ERROR) return result;
            if (result == READER_EOF) {
                if (reader->json_active) {
                    return json_return_record(reader, record, record_length,
                                              line_number, byte_offset);
                }
                return READER_EOF;
            }
            if (result == READER_TOO_LONG) {
                if (reader->json_active) return READER_ERROR;
                *record = reader->line;
                *record_length = 0;
                *line_number = reader->json_pending_number;
                *byte_offset = reader->json_pending_offset;
                reader->json_pending = 0;
                return READER_TOO_LONG;
            }
        }
        if (reader->json_pending_position >= reader->json_pending_length) {
            reader->json_pending = 0;
            if (reader->json_active && reader->json_pending_terminated) {
                if (json_record_append(reader, '\n') < 0) return READER_ERROR;
            }
            continue;
        }
        byte = reader->json_pending_line[reader->json_pending_position++];
        if (!reader->json_active) {
            if (reader->json_outside_string) {
                if (reader->json_outside_escape) reader->json_outside_escape = 0;
                else if (byte == '\\') reader->json_outside_escape = 1;
                else if (byte == '"') reader->json_outside_string = 0;
            } else if (byte == '"') {
                reader->json_outside_string = 1;
            } else if (byte == '{') {
                reader->json_active = 1;
                reader->json_depth = 1;
                reader->json_record_length = 0;
                reader->json_oversized = 0;
                reader->json_in_string = 0;
                reader->json_escape = 0;
                reader->json_record_line = reader->json_pending_number;
                reader->json_record_offset = reader->json_pending_offset +
                                            reader->json_pending_position - 1;
                if (json_record_append(reader, byte) < 0) return READER_ERROR;
            }
            continue;
        }
        if (json_record_append(reader, byte) < 0) return READER_ERROR;
        if (reader->json_in_string) {
            if (reader->json_escape) reader->json_escape = 0;
            else if (byte == '\\') reader->json_escape = 1;
            else if (byte == '"') reader->json_in_string = 0;
        } else if (byte == '"') {
            reader->json_in_string = 1;
        } else if (byte == '{' || byte == '[') {
            reader->json_depth++;
        } else if (byte == '}' || byte == ']') {
            if (reader->json_depth != 0) reader->json_depth--;
            if (reader->json_depth == 0) {
                return json_return_record(reader, record, record_length,
                                          line_number, byte_offset);
            }
        }
    }
}
