#ifndef LOGVIEW_PARSERS_H
#define LOGVIEW_PARSERS_H

#include "jsonl.h"

typedef enum {
    FORMAT_RAW,
    FORMAT_JSONL,
    FORMAT_SYSLOG3164,
    FORMAT_SYSLOG5424,
    FORMAT_APACHE_COMMON,
    FORMAT_APACHE_COMBINED,
    FORMAT_NGINX_COMBINED,
    FORMAT_CUSTOM,
    FORMAT_AUTO
} InputFormat;

InputFormat parser_format_from_name(const char *name);
int parser_format_line(InputFormat format, LogEntry *entry);
InputFormat parser_detect_format(const unsigned char *const *lines,
                                const size_t *lengths, size_t count);
int parser_custom_validate(const char *pattern, char *error,
                           size_t error_capacity);
int parser_custom_line(const char *pattern, LogEntry *entry);

#endif
