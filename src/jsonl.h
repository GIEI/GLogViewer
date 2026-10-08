#ifndef LOGVIEW_JSONL_H
#define LOGVIEW_JSONL_H

#include "logview.h"

typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t length;
} JsonScratch;

int json_scratch_init(JsonScratch *scratch, size_t capacity);
void json_scratch_destroy(JsonScratch *scratch);
int jsonl_parse(LogEntry *entry, JsonScratch *scratch);
int log_entry_find_field(const LogEntry *entry, const char *name,
                         ByteSpan *value);

#endif
