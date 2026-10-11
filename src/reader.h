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

typedef int (*ReaderPhysicalLineCallback)(void *context,
                                          uint64_t line_number,
                                          uint64_t byte_offset);

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
    unsigned char *json_record;
    size_t json_record_capacity;
    size_t json_record_length;
    const unsigned char *json_pending_line;
    size_t json_pending_length;
    size_t json_pending_position;
    uint64_t json_pending_number;
    uint64_t json_pending_offset;
    ReaderResult json_pending_result;
    int json_pending_terminated;
    int json_pending;
    int json_mode_checked;
    int json_document_mode;
    int json_document_hint;
    int json_active;
    int json_in_string;
    int json_escape;
    int json_outside_string;
    int json_outside_escape;
    int json_oversized;
    size_t json_depth;
    uint64_t json_record_line;
    uint64_t json_record_offset;
} LineReader;

int reader_init(LineReader *reader, FILE *file, size_t max_line_bytes);
void reader_destroy(LineReader *reader);
int reader_reposition(LineReader *reader, uint64_t offset,
                      uint64_t last_line_number);
ReaderResult reader_next(LineReader *reader, const unsigned char **line,
                         size_t *line_length, uint64_t *line_number,
                         uint64_t *byte_offset);
ReaderResult reader_next_json(LineReader *reader, const unsigned char **record,
                              size_t *record_length, uint64_t *line_number,
                              uint64_t *byte_offset,
                              ReaderPhysicalLineCallback callback,
                              void *callback_context);

#endif
