#ifndef LOGVIEW_OUTPUT_H
#define LOGVIEW_OUTPUT_H

#include "logview.h"

#include <stdio.h>

#define LOGVIEW_MAX_COLUMNS 32
#define LOGVIEW_COLUMN_NAME 128

typedef enum {
    OUTPUT_TEXT,
    OUTPUT_CSV
} OutputFormat;

typedef struct {
    FILE *stream;
    unsigned char *text_buffer;
    size_t text_buffer_length;
    size_t text_buffer_capacity;
    int owns_stream;
    OutputFormat format;
    char columns[LOGVIEW_MAX_COLUMNS][LOGVIEW_COLUMN_NAME];
    size_t column_count;
} OutputSink;

int output_init(OutputSink *sink, const char *path, const char *input_path,
                OutputFormat format, const char *columns, int force,
                int include_source_file,
                char *error, size_t error_capacity);
int output_init_utf8(OutputSink *sink, const char *path, const char *input_path,
                     OutputFormat format, const char *columns, int force,
                     int include_source_file,
                     char *error, size_t error_capacity);
int output_write(OutputSink *sink, const LogEntry *entry);
int output_write_source(OutputSink *sink, const LogEntry *entry,
                        const char *source_path);
int output_flush(OutputSink *sink);
int output_finish(OutputSink *sink);
int output_paths_same_file(const char *left, const char *right);

#endif
