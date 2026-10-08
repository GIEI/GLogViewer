#ifndef LOGVIEW_STATS_H
#define LOGVIEW_STATS_H

#include "logview.h"

#include <stdio.h>

#define LOGVIEW_TIMELINE_BINS 256
#define LOGVIEW_HEAVY_HITTERS 512

typedef struct {
    unsigned char value[256];
    size_t length;
    uint64_t count;
} LevelCount;

typedef struct {
    uint64_t key_hash;
    uint64_t name_hash;
    uint64_t value_hash;
    size_t name_length;
    size_t value_length;
    unsigned char name[64];
    unsigned char value[128];
    uint64_t count;
    uint64_t error;
    size_t heap_position;
} HeavyCandidate;

typedef struct {
    uint64_t physical;
    uint64_t parsed;
    uint64_t rejected;
    uint64_t selected;
    LevelCount levels[LOGVIEW_MAX_FIELDS];
    size_t level_count;
    int64_t earliest_timestamp;
    int64_t latest_timestamp;
    uint64_t unknown_time;
    uint64_t timeline[LOGVIEW_TIMELINE_BINS];
    uint64_t timestamped;
    int has_timestamp;
    HeavyCandidate candidates[LOGVIEW_HEAVY_HITTERS];
    size_t candidate_count;
    size_t heap[LOGVIEW_HEAVY_HITTERS];
    int hash_slots[LOGVIEW_HEAVY_HITTERS * 4];
} LogStats;

void stats_init(LogStats *stats);
int stats_physical(LogStats *stats);
int stats_parsed(LogStats *stats);
int stats_rejected(LogStats *stats);
int stats_selected(LogStats *stats, const LogEntry *entry);
int stats_timeline_add(LogStats *stats, const LogEntry *entry);
int stats_print(const LogStats *stats, FILE *stream);

#endif
