#ifndef LOGVIEW_STATS_H
#define LOGVIEW_STATS_H

#include "logview.h"

#include <stdio.h>

#define LOGVIEW_TIMELINE_BINS 256
#define LOGVIEW_HEAVY_HITTERS 512

typedef struct {
    unsigned syslog_year;
    int syslog_timezone_offset_seconds;
} StatsTimestampConfig;

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
    const unsigned char *focus_field;
    size_t focus_field_length;
    StatsTimestampConfig timestamp_config;
} LogStats;

void stats_init(LogStats *stats);
int stats_physical(LogStats *stats);
int stats_parsed(LogStats *stats);
int stats_rejected(LogStats *stats);
int stats_set_focus_field(LogStats *stats, const unsigned char *field,
                          size_t field_length);
int stats_set_timestamp_config(LogStats *stats,
                               const StatsTimestampConfig *config);
int stats_selected(LogStats *stats, const LogEntry *entry);
int stats_timeline_add(LogStats *stats, const LogEntry *entry);
int stats_timeline_add_epoch(LogStats *stats, int64_t epoch);
int stats_timestamp_epoch(ByteSpan timestamp, int64_t *epoch);
int stats_timestamp_epoch_configured(ByteSpan timestamp,
                                     const StatsTimestampConfig *config,
                                     int64_t *epoch);
size_t stats_timeline_bin(int64_t epoch, int64_t earliest, int64_t latest);
size_t stats_top_candidates(const LogStats *stats, size_t *selected,
                            size_t capacity);
int stats_print(const LogStats *stats, FILE *stream);

#endif
