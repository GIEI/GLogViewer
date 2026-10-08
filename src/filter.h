#ifndef LOGVIEW_FILTER_H
#define LOGVIEW_FILTER_H

#include "jsonl.h"
#include "regex.h"

typedef struct Filter Filter;

Filter *filter_compile(const char *expression, char *error,
                       size_t error_capacity);
int filter_matches(const Filter *filter, const LogEntry *entry,
                   RegexResult *regex_result);
void filter_free(Filter *filter);

#endif
