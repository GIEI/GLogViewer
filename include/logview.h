#ifndef LOGVIEW_H
#define LOGVIEW_H

#include <stddef.h>
#include <stdint.h>

#define LOGVIEW_MAX_FIELDS 64
#define LOGVIEW_MAX_REGEX_STATES 4096
#define LOGVIEW_MAX_REGEX_AST_NODES (LOGVIEW_MAX_REGEX_STATES * 2)
#define LOGVIEW_MAX_LINE_BYTES (16u * 1024u * 1024u)
#define LOGVIEW_MAX_REGEX_WORK 100000000u

typedef struct {
    const unsigned char *data;
    size_t length;
} ByteSpan;

typedef struct {
    ByteSpan name;
    ByteSpan value;
} LogField;

typedef struct {
    ByteSpan raw;
    uint64_t line_number;
    uint64_t byte_offset;
    LogField fields[LOGVIEW_MAX_FIELDS];
    size_t field_count;
    int parse_ok;
} LogEntry;

#endif
