#include "stats.h"

#include "jsonl.h"

#include <stdio.h>
#include <string.h>

#define HH_HASH_CAPACITY (LOGVIEW_HEAVY_HITTERS * 4)
#define HH_NAME_PREVIEW 63
#define HH_VALUE_PREVIEW 127

static int increment(uint64_t *value)
{
    if (*value == UINT64_MAX) return 0;
    (*value)++;
    return 1;
}

void stats_init(LogStats *stats)
{
    memset(stats, 0, sizeof(*stats));
}

int stats_physical(LogStats *stats) { return increment(&stats->physical); }
int stats_parsed(LogStats *stats) { return increment(&stats->parsed); }
int stats_rejected(LogStats *stats) { return increment(&stats->rejected); }

static int add_level(LogStats *stats, ByteSpan level)
{
    size_t index;
    static const unsigned char unspecified[] = "UNSPECIFIED";
    if (level.data == NULL) {
        level.data = unspecified;
        level.length = sizeof(unspecified) - 1;
    }
    for (index = 0; index < stats->level_count; index++) {
        LevelCount *item = &stats->levels[index];
        if (item->length == level.length &&
            memcmp(item->value, level.data, level.length) == 0) {
            return increment(&item->count);
        }
    }
    if (stats->level_count >= LOGVIEW_MAX_FIELDS || level.length > sizeof(stats->levels[0].value)) return 0;
    stats->levels[stats->level_count].length = level.length;
    memcpy(stats->levels[stats->level_count].value, level.data, level.length);
    stats->levels[stats->level_count].count = 1;
    stats->level_count++;
    return 1;
}

static uint64_t hash_bytes(uint64_t hash, const unsigned char *data, size_t length)
{
    size_t index;
    for (index = 0; index < length; index++) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t candidate_key_hash(const unsigned char *name, size_t name_length,
                                  const unsigned char *value, size_t value_length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = hash_bytes(hash, name, name_length);
    hash ^= 0xff;
    hash *= UINT64_C(1099511628211);
    return hash_bytes(hash, value, value_length);
}

static int candidate_matches(const HeavyCandidate *candidate,
                             const unsigned char *name, size_t name_length,
                             const unsigned char *value, size_t value_length,
                             uint64_t name_hash, uint64_t value_hash,
                             uint64_t key_hash)
{
    size_t name_preview = name_length < HH_NAME_PREVIEW ? name_length : HH_NAME_PREVIEW;
    size_t value_preview = value_length < HH_VALUE_PREVIEW ? value_length : HH_VALUE_PREVIEW;
    return candidate->key_hash == key_hash && candidate->name_hash == name_hash &&
           candidate->value_hash == value_hash && candidate->name_length == name_length &&
           candidate->value_length == value_length &&
           memcmp(candidate->name, name, name_preview) == 0 &&
           memcmp(candidate->value, value, value_preview) == 0;
}

static size_t hash_find(const LogStats *stats, const unsigned char *name,
                        size_t name_length, const unsigned char *value,
                        size_t value_length, uint64_t name_hash,
                        uint64_t value_hash, uint64_t key_hash,
                        size_t *empty_slot)
{
    size_t slot = (size_t)(key_hash % HH_HASH_CAPACITY);
    size_t first_tombstone = HH_HASH_CAPACITY;
    size_t probes;
    for (probes = 0; probes < HH_HASH_CAPACITY; probes++) {
        int encoded = stats->hash_slots[slot];
        if (encoded == 0) {
            *empty_slot = first_tombstone != HH_HASH_CAPACITY ? first_tombstone : slot;
            return LOGVIEW_HEAVY_HITTERS;
        }
        if (encoded == -1) {
            if (first_tombstone == HH_HASH_CAPACITY) first_tombstone = slot;
        } else {
            size_t index = (size_t)(encoded - 1);
            if (candidate_matches(&stats->candidates[index], name, name_length,
                                  value, value_length, name_hash, value_hash,
                                  key_hash)) {
                *empty_slot = slot;
                return index;
            }
        }
        slot = (slot + 1) % HH_HASH_CAPACITY;
    }
    *empty_slot = first_tombstone;
    return LOGVIEW_HEAVY_HITTERS;
}

static void hash_remove(LogStats *stats, const HeavyCandidate *candidate,
                        size_t candidate_index)
{
    size_t slot = (size_t)(candidate->key_hash % HH_HASH_CAPACITY);
    size_t probes;
    for (probes = 0; probes < HH_HASH_CAPACITY; probes++) {
        int encoded = stats->hash_slots[slot];
        if (encoded == 0) return;
        if (encoded == (int)candidate_index + 1) {
            stats->hash_slots[slot] = -1;
            return;
        }
        slot = (slot + 1) % HH_HASH_CAPACITY;
    }
}

static int heap_less(const LogStats *stats, size_t left_position,
                     size_t right_position)
{
    const HeavyCandidate *left = &stats->candidates[stats->heap[left_position]];
    const HeavyCandidate *right = &stats->candidates[stats->heap[right_position]];
    return left->count < right->count ||
           (left->count == right->count && left->key_hash < right->key_hash);
}

static void heap_swap(LogStats *stats, size_t left, size_t right)
{
    size_t temporary = stats->heap[left];
    stats->heap[left] = stats->heap[right];
    stats->heap[right] = temporary;
    stats->candidates[stats->heap[left]].heap_position = left;
    stats->candidates[stats->heap[right]].heap_position = right;
}

static void heap_down(LogStats *stats, size_t position)
{
    for (;;) {
        size_t left = position * 2 + 1;
        size_t right = left + 1;
        size_t smallest = position;
        if (left < stats->candidate_count && heap_less(stats, left, smallest)) smallest = left;
        if (right < stats->candidate_count && heap_less(stats, right, smallest)) smallest = right;
        if (smallest == position) return;
        heap_swap(stats, position, smallest);
        position = smallest;
    }
}

static void heap_up(LogStats *stats, size_t position)
{
    while (position != 0) {
        size_t parent = (position - 1) / 2;
        if (!heap_less(stats, position, parent)) return;
        heap_swap(stats, position, parent);
        position = parent;
    }
}

static void candidate_set(HeavyCandidate *candidate,
                          const unsigned char *name, size_t name_length,
                          const unsigned char *value, size_t value_length,
                          uint64_t name_hash, uint64_t value_hash,
                          uint64_t key_hash, uint64_t count, uint64_t error)
{
    size_t name_preview = name_length < HH_NAME_PREVIEW ? name_length : HH_NAME_PREVIEW;
    size_t value_preview = value_length < HH_VALUE_PREVIEW ? value_length : HH_VALUE_PREVIEW;
    candidate->key_hash = key_hash;
    candidate->name_hash = name_hash;
    candidate->value_hash = value_hash;
    candidate->name_length = name_length;
    candidate->value_length = value_length;
    memset(candidate->name, 0, sizeof(candidate->name));
    memset(candidate->value, 0, sizeof(candidate->value));
    memcpy(candidate->name, name, name_preview);
    memcpy(candidate->value, value, value_preview);
    candidate->count = count;
    candidate->error = error;
}

static int add_candidate(LogStats *stats, const unsigned char *name,
                         size_t name_length, const unsigned char *value,
                         size_t value_length)
{
    uint64_t name_hash = hash_bytes(UINT64_C(14695981039346656037), name, name_length);
    uint64_t value_hash = hash_bytes(UINT64_C(14695981039346656037), value, value_length);
    uint64_t key_hash = candidate_key_hash(name, name_length, value, value_length);
    size_t hash_slot;
    size_t index = hash_find(stats, name, name_length, value, value_length,
                             name_hash, value_hash, key_hash, &hash_slot);
    HeavyCandidate *candidate;
    if (index != LOGVIEW_HEAVY_HITTERS) {
        candidate = &stats->candidates[index];
        if (!increment(&candidate->count)) return 0;
        heap_down(stats, candidate->heap_position);
        return 1;
    }
    if (stats->candidate_count < LOGVIEW_HEAVY_HITTERS) {
        index = stats->candidate_count++;
        candidate = &stats->candidates[index];
        candidate_set(candidate, name, name_length, value, value_length,
                      name_hash, value_hash, key_hash, 1, 0);
        candidate->heap_position = index;
        stats->heap[index] = index;
        stats->hash_slots[hash_slot] = (int)index + 1;
        heap_up(stats, index);
        return 1;
    }
    index = stats->heap[0];
    candidate = &stats->candidates[index];
    if (candidate->count == UINT64_MAX) return 0;
    hash_remove(stats, candidate, index);
    {
        uint64_t previous_minimum = candidate->count;
        candidate_set(candidate, name, name_length, value, value_length,
                      name_hash, value_hash, key_hash, previous_minimum + 1,
                      previous_minimum);
    }
    stats->hash_slots[hash_slot] = (int)index + 1;
    heap_down(stats, 0);
    return 1;
}

static int decimal(const unsigned char *data, size_t length, size_t *position,
                   unsigned minimum_digits, unsigned maximum_digits,
                   unsigned *value)
{
    size_t start = *position;
    unsigned result = 0;
    while (*position < length && *position - start < maximum_digits &&
           data[*position] >= '0' && data[*position] <= '9') {
        result = result * 10u + data[*position] - '0';
        (*position)++;
    }
    if (*position - start < minimum_digits) return 0;
    *value = result;
    return 1;
}

static int parse_month(const unsigned char *data, size_t start)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    size_t index;
    for (index = 0; index < 12; index++) {
        if (memcmp(data + start, months[index], 3) == 0) return (int)index + 1;
    }
    return 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    int adjusted_year = year - (month <= 2);
    int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    unsigned year_of_era = (unsigned)(adjusted_year - era * 400);
    unsigned day_of_year;
    unsigned day_of_era;
    int64_t result;
    month = month > 2 ? month - 3 : month + 9;
    day_of_year = (153 * month + 2) / 5 + day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    result = (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
    return result;
}

static int parse_zone(const unsigned char *data, size_t length, size_t *position,
                      int *offset_seconds)
{
    int sign;
    unsigned hours;
    unsigned minutes;
    size_t cursor = *position;
    if (cursor >= length) return 0;
    if (data[cursor] == 'Z' || data[cursor] == 'z') {
        *offset_seconds = 0;
        *position = cursor + 1;
        return 1;
    }
    if (data[cursor] != '+' && data[cursor] != '-') return 0;
    sign = data[cursor++] == '+' ? 1 : -1;
    if (!decimal(data, length, &cursor, 2, 2, &hours)) return 0;
    if (cursor < length && data[cursor] == ':') cursor++;
    if (!decimal(data, length, &cursor, 2, 2, &minutes) || hours > 23 || minutes > 59) return 0;
    *offset_seconds = sign * (int)(hours * 3600 + minutes * 60);
    *position = cursor;
    return 1;
}

static int valid_date_time(unsigned year, unsigned month, unsigned day,
                           unsigned hour, unsigned minute, unsigned second)
{
    static const unsigned char month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned maximum_day;
    int leap;
    if (year > 9999 || month == 0 || month > 12 || hour > 23 || minute > 59 || second > 60) return 0;
    leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    maximum_day = month_days[month - 1] + (month == 2 && leap ? 1u : 0u);
    return day >= 1 && day <= maximum_day;
}

int stats_timestamp_epoch_configured(ByteSpan timestamp,
                                     const StatsTimestampConfig *config,
                                     int64_t *epoch)
{
    const unsigned char *data = timestamp.data;
    size_t length = timestamp.length;
    size_t position = 0;
    unsigned year, month, day, hour, minute, second;
    int offset_seconds = 0;
    int apache = 0;

    if (data == NULL || epoch == NULL) return 0;

    if (length >= 20 && data[4] == '-' && data[7] == '-') {
        if (!decimal(data, length, &position, 4, 4, &year) || position >= length || data[position++] != '-' ||
            !decimal(data, length, &position, 2, 2, &month) || position >= length || data[position++] != '-' ||
            !decimal(data, length, &position, 2, 2, &day) || position >= length ||
            (data[position] != 'T' && data[position] != 't' && data[position] != ' ')) return 0;
        position++;
    } else if (length >= 26 && data[2] == '/' && data[6] == '/') {
        apache = 1;
        if (!decimal(data, length, &position, 2, 2, &day) || data[position++] != '/') return 0;
        month = (unsigned)parse_month(data, position);
        if (month == 0) return 0;
        position += 3;
        if (position >= length || data[position++] != '/' ||
            !decimal(data, length, &position, 4, 4, &year) || position >= length || data[position++] != ':' ) return 0;
    } else if (config != NULL && config->syslog_year != 0 &&
               length == 15 && data[3] == ' ' && data[6] == ' ' &&
               data[9] == ':' && data[12] == ':') {
        int parsed_month = parse_month(data, 0);
        if (parsed_month == 0) return 0;
        if (data[4] == ' ' && data[5] >= '0' && data[5] <= '9') {
            day = data[5] - '0';
        } else if (data[4] >= '0' && data[4] <= '9' &&
                   data[5] >= '0' && data[5] <= '9') {
            day = (unsigned)(data[4] - '0') * 10u +
                  (unsigned)(data[5] - '0');
        } else return 0;
        position = 7;
        if (!decimal(data, length, &position, 2, 2, &hour) ||
            position >= length || data[position++] != ':' ||
            !decimal(data, length, &position, 2, 2, &minute) ||
            position >= length || data[position++] != ':' ||
            !decimal(data, length, &position, 2, 2, &second) ||
            position != length) return 0;
        year = config->syslog_year;
        month = (unsigned)parsed_month;
        if (!valid_date_time(year, month, day, hour, minute, second)) return 0;
        *epoch = days_from_civil((int)year, month, day) * INT64_C(86400) +
                 (int64_t)hour * 3600 + (int64_t)minute * 60 +
                 (int64_t)second - config->syslog_timezone_offset_seconds;
        return 1;
    } else return 0;

    if (!decimal(data, length, &position, 2, 2, &hour) || position >= length || data[position++] != ':' ||
        !decimal(data, length, &position, 2, 2, &minute) || position >= length || data[position++] != ':' ||
        !decimal(data, length, &position, 2, 2, &second)) return 0;
    if (!valid_date_time(year, month, day, hour, minute, second)) return 0;
    if (!apache && position < length && data[position] == '.') {
        size_t fraction_digits = 0;
        position++;
        while (position < length && data[position] >= '0' && data[position] <= '9') {
            position++;
            fraction_digits++;
        }
        if (fraction_digits == 0) return 0;
    }
    if (apache && position < length && data[position] == ' ') position++;
    if (!parse_zone(data, length, &position, &offset_seconds) || position != length) return 0;
    *epoch = days_from_civil((int)year, month, day) * INT64_C(86400) +
             (int64_t)hour * 3600 + (int64_t)minute * 60 + (int64_t)second - offset_seconds;
    return 1;
}

int stats_timestamp_epoch(ByteSpan timestamp, int64_t *epoch)
{
    return stats_timestamp_epoch_configured(timestamp, NULL, epoch);
}

static int update_time_range(LogStats *stats, const LogEntry *entry)
{
    ByteSpan timestamp;
    int64_t epoch;
    if (!log_entry_find_field(entry, "timestamp", &timestamp) ||
        !stats_timestamp_epoch_configured(timestamp, &stats->timestamp_config,
                                          &epoch)) return increment(&stats->unknown_time);
    if (!stats->has_timestamp || epoch < stats->earliest_timestamp) stats->earliest_timestamp = epoch;
    if (!stats->has_timestamp || epoch > stats->latest_timestamp) stats->latest_timestamp = epoch;
    stats->has_timestamp = 1;
    return 1;
}

int stats_set_focus_field(LogStats *stats, const unsigned char *field,
                          size_t field_length)
{
    if (stats == NULL || field == NULL || field_length == 0) return 0;
    stats->focus_field = field;
    stats->focus_field_length = field_length;
    return 1;
}

int stats_set_timestamp_config(LogStats *stats,
                               const StatsTimestampConfig *config)
{
    if (stats == NULL || config == NULL || config->syslog_year > 9999 ||
        config->syslog_timezone_offset_seconds < -86340 ||
        config->syslog_timezone_offset_seconds > 86340) return 0;
    stats->timestamp_config = *config;
    return 1;
}

int stats_selected(LogStats *stats, const LogEntry *entry)
{
    ByteSpan level;
    size_t index;
    if (!increment(&stats->selected)) return 0;
    if (!log_entry_find_field(entry, "level", &level)) {
        level.data = NULL;
        level.length = 0;
    }
    if (!add_level(stats, level) || !update_time_range(stats, entry)) return 0;
    for (index = 0; index < entry->field_count; index++) {
        if (stats->focus_field != NULL &&
            (entry->fields[index].name.length != stats->focus_field_length ||
             memcmp(entry->fields[index].name.data, stats->focus_field,
                    stats->focus_field_length) != 0)) continue;
        if (!add_candidate(stats, entry->fields[index].name.data,
                           entry->fields[index].name.length,
                           entry->fields[index].value.data,
                           entry->fields[index].value.length)) return 0;
    }
    return 1;
}

int stats_timeline_add(LogStats *stats, const LogEntry *entry)
{
    ByteSpan timestamp;
    int64_t epoch;
    if (!log_entry_find_field(entry, "timestamp", &timestamp) ||
        !stats_timestamp_epoch_configured(timestamp, &stats->timestamp_config,
                                          &epoch)) return 1;
    return stats_timeline_add_epoch(stats, epoch);
}

int stats_timeline_add_epoch(LogStats *stats, int64_t epoch)
{
    size_t bin;
    if (stats == NULL || !stats->has_timestamp ||
        epoch < stats->earliest_timestamp || epoch > stats->latest_timestamp) return 0;
    bin = stats_timeline_bin(epoch, stats->earliest_timestamp,
                             stats->latest_timestamp);
    if (!increment(&stats->timeline[bin]) || !increment(&stats->timestamped)) return 0;
    return 1;
}

size_t stats_timeline_bin(int64_t epoch, int64_t earliest, int64_t latest)
{
    uint64_t range;
    size_t bin;
    if (epoch <= earliest || earliest == latest) return 0;
    if (epoch >= latest) return LOGVIEW_TIMELINE_BINS - 1;
    range = (uint64_t)(latest - earliest);
    bin = (size_t)(((uint64_t)(epoch - earliest) * LOGVIEW_TIMELINE_BINS) /
                   (range + 1));
    return bin < LOGVIEW_TIMELINE_BINS ? bin : LOGVIEW_TIMELINE_BINS - 1;
}

static int print_escaped(FILE *stream, const unsigned char *data, size_t length)
{
    size_t index;
    for (index = 0; index < length; index++) {
        unsigned char byte = data[index];
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\') {
            if (fputc(byte, stream) == EOF) return 0;
        } else if (fprintf(stream, "\\x%02X", (unsigned)byte) < 0) {
            return 0;
        }
    }
    return 1;
}

size_t stats_top_candidates(const LogStats *stats, size_t *selected,
                            size_t capacity)
{
    size_t index;
    size_t count = 0;
    for (index = 0; index < stats->candidate_count; index++) {
        size_t position = 0;
        while (position < count &&
               (stats->candidates[selected[position]].count > stats->candidates[index].count ||
                (stats->candidates[selected[position]].count == stats->candidates[index].count &&
                 stats->candidates[selected[position]].key_hash <= stats->candidates[index].key_hash))) position++;
        if (position >= capacity) continue;
        if (count < capacity) count++;
        {
            size_t move;
            for (move = count - 1; move > position; move--) selected[move] = selected[move - 1];
            selected[position] = index;
        }
    }
    return count;
}

int stats_print(const LogStats *stats, FILE *stream)
{
    size_t index;
    size_t top[10];
    size_t top_count;
    if (fprintf(stream, "Physical records: %llu\nParsed records: %llu\nRejected records: %llu\nSelected records: %llu\nLevel counts (selected):\n",
                 (unsigned long long)stats->physical,
                 (unsigned long long)stats->parsed,
                 (unsigned long long)stats->rejected,
                 (unsigned long long)stats->selected) < 0) return 0;
    for (index = 0; index < stats->level_count; index++) {
        const LevelCount *level = &stats->levels[index];
        if (fputs("  ", stream) == EOF || !print_escaped(stream, level->value, level->length) ||
            fprintf(stream, ": %llu\n", (unsigned long long)level->count) < 0) return 0;
    }
    if (stats->has_timestamp) {
        int64_t span = stats->latest_timestamp - stats->earliest_timestamp;
        fprintf(stream, "Timeline range (UTC epoch seconds): %lld..%lld; bins: %d; width: %lld seconds\n",
                (long long)stats->earliest_timestamp, (long long)stats->latest_timestamp,
                LOGVIEW_TIMELINE_BINS, (long long)(span / LOGVIEW_TIMELINE_BINS + 1));
    } else {
        fputs("Timeline: no unambiguous timestamps\n", stream);
    }
    fprintf(stream, "Timestamped records: %llu\nUnknown timestamps: %llu\nTimeline counts:\n",
            (unsigned long long)stats->timestamped,
            (unsigned long long)stats->unknown_time);
    for (index = 0; index < LOGVIEW_TIMELINE_BINS; index++) {
        if (fprintf(stream, "  %03u: %llu\n", (unsigned)index,
                    (unsigned long long)stats->timeline[index]) < 0) return 0;
    }
    top_count = stats_top_candidates(stats, top, sizeof(top) / sizeof(top[0]));
    if (stats->focus_field != NULL) {
        fputs("Frequent values for ", stream);
        if (!print_escaped(stream, stats->focus_field,
                           stats->focus_field_length)) return 0;
        fputs(":\n", stream);
    } else {
        fputs("Frequent field values:\n", stream);
    }
    for (index = 0; index < top_count; index++) {
        const HeavyCandidate *candidate = &stats->candidates[top[index]];
        if (fputs("  ", stream) == EOF ||
            !print_escaped(stream, candidate->name,
                           candidate->name_length < HH_NAME_PREVIEW ? candidate->name_length : HH_NAME_PREVIEW) ||
            fputc('=', stream) == EOF ||
            !print_escaped(stream, candidate->value,
                           candidate->value_length < HH_VALUE_PREVIEW ? candidate->value_length : HH_VALUE_PREVIEW) ||
            fprintf(stream, ": %llu (%s",
                    (unsigned long long)candidate->count,
                    candidate->error == 0 ? "exact" : "estimated") < 0) return 0;
        if (candidate->error != 0 &&
            fprintf(stream, "; error <= %llu",
                    (unsigned long long)candidate->error) < 0) return 0;
        if (fprintf(stream, ")%s\n",
                    candidate->value_length > HH_VALUE_PREVIEW ? "..." : "") < 0) return 0;
    }
    return fflush(stream) == 0;
}
