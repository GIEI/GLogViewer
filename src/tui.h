#ifndef LOGVIEW_TUI_H
#define LOGVIEW_TUI_H

#include "filter.h"
#include "jsonl.h"
#include "parsers.h"
#include "reader.h"
#include "regex.h"

int tui_run(FILE *input, LineReader *reader, InputFormat format,
            const char *custom_pattern, JsonScratch *scratch,
            Filter **field_filter, Regex **raw_regex, Regex *field_regex,
            const char *regex_field_name);

#endif
