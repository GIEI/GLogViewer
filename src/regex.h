#ifndef LOGVIEW_REGEX_H
#define LOGVIEW_REGEX_H

#include "logview.h"

typedef struct Regex Regex;

typedef enum {
    REGEX_NO_MATCH,
    REGEX_MATCH,
    REGEX_WORK_LIMIT
} RegexResult;

Regex *regex_compile(const char *pattern, char *error, size_t error_capacity);
void regex_free(Regex *regex);
RegexResult regex_search(const Regex *regex, const unsigned char *text,
                         size_t text_length);

#endif
