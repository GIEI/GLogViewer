#ifndef LOGVIEW_READER_H
#define LOGVIEW_READER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "logview.h"

typedef enum {
    READER_LINE,
    READER_EOF,
    READER_TOO_LONG,
    READER_ERROR
} ReaderResult;

typedef struct {
    FILE *file;
    unsigned char *line;
    size_t line_length;
    size_t line_capacity;
    size_t max_line_bytes;
    unsigned char input[65536];
    size_t input_length;
    size_t input_position;
    uint64_t next_offset;
    uint64_t next_line_number;
    int input_eof;
    int input_error;
    int line_terminated;
} LineReader;

int reader_init(LineReader *reader, FILE *file, size_t max_line_bytes);
void reader_destroy(LineReader *reader);
int reader_reposition(LineReader *reader, uint64_t offset,
                      uint64_t last_line_number);
ReaderResult reader_next(LineReader *reader, const unsigned char **line,
                         size_t *line_length, uint64_t *line_number,
                         uint64_t *byte_offset);

#endif
