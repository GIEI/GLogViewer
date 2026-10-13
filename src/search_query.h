#ifndef LOGVIEW_SEARCH_QUERY_H
#define LOGVIEW_SEARCH_QUERY_H

#include "regex.h"

#include <stddef.h>

typedef struct SearchQuery SearchQuery;

SearchQuery *search_query_compile(const char *text, char *error,
                                 size_t error_capacity);
void search_query_free(SearchQuery *query);
RegexResult search_query_match(const SearchQuery *query,
                               const unsigned char *text, size_t text_length);

#endif
