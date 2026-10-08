#include "reader.h"

#include <stdlib.h>
#include <string.h>

static int reader_byte(LineReader *reader)
{
    if (reader->input_position >= reader->input_length) {
        size_t amount;
        if (reader->input_eof || reader->input_error) {
            return -1;
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
            return -1;
        }
    }
    return reader->input[reader->input_position++];
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
    memset(reader, 0, sizeof(*reader));
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
        int byte = reader_byte(reader);
        if (byte < 0) {
            if (reader->input_error) {
                return READER_ERROR;
            }
            if (!saw_byte) {
                return READER_EOF;
            }
            break;
        }

        saw_byte = 1;
        reader->next_offset++;
        if (byte == '\n') {
            ended_with_lf = 1;
            break;
        }

        if (!too_long) {
            if (length == reader->max_line_bytes) {
                too_long = 1;
                length = 0;
            } else {
                if (length == reader->line_capacity &&
                    !reader_grow_line(reader)) {
                    return READER_ERROR;
                }
                reader->line[length++] = (unsigned char)byte;
            }
        }
    }

    reader->next_line_number++;
    if (too_long) {
        return READER_TOO_LONG;
    }
    if (ended_with_lf && length > 0 && reader->line[length - 1] == '\r') {
        length--;
    }

    reader->line_length = length;
    *line = reader->line;
    *line_length = length;
    *line_number = reader->next_line_number;
    *byte_offset = start_offset;
    return READER_LINE;
}
