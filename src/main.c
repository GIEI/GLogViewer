#include "jsonl.h"
#include "filter.h"
#include "output.h"
#include "follow.h"
#include "stats.h"
#include "tui.h"
#include "parsers.h"
#include "reader.h"
#include "regex.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#define LOGVIEW_VERSION "1.5.0"
#define FIELD_NAME_CAPACITY 256
#define PROFILE_VALUE_CAPACITY 4096
#define LOGVIEW_MAX_INPUTS 16

typedef struct {
    const char *input_path;
    const char *input_paths[LOGVIEW_MAX_INPUTS];
    size_t input_count;
    const char *filter_argument;
    const char *raw_regex_argument;
    const char *exclude_regex_argument;
    const char *field_regex_argument;
    size_t max_line_bytes;
    InputFormat format;
    int format_set;
    const char *custom_pattern;
    const char *output_path;
    const char *column_list;
    OutputFormat export_format;
    int export_set;
    int force_output;
    int stats_mode;
    int stats_flag_set;
    const char *stats_field_argument;
    unsigned syslog_year;
    int syslog_timezone_offset_seconds;
    int syslog_timezone_set;
    int follow_mode;
    int from_start;
    int flush_partial;
    int tui_mode;
    size_t result_limit;
    int bottom_limit;
    int record_mode;
    size_t context_before;
    size_t context_after;
    int context_before_set;
    int context_after_set;
    const char *profile_path;
    const char *save_profile_path;
    int profile_overwrite;
    char profile_pattern[PROFILE_VALUE_CAPACITY];
    char profile_filter[PROFILE_VALUE_CAPACITY];
    char profile_regex[PROFILE_VALUE_CAPACITY];
    char profile_exclude_regex[PROFILE_VALUE_CAPACITY];
    char profile_field_regex[PROFILE_VALUE_CAPACITY];
} Options;

typedef struct {
    uint64_t offset;
    size_t length;
} BottomRecord;

volatile sig_atomic_t logview_stop_requested = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    logview_stop_requested = 1;
}

static void print_usage(FILE *stream)
{
    fputs("Usage: logview <file> [<file> ...] [--format FORMAT] [--pattern PATTERN]\n"
          "                [--filter EXPRESSION]\n"
          "                [--regex pattern] [--regex-field field=pattern]\n"
          "                [--exclude-regex pattern]\n"
          "                [--export text|csv --output path [--columns list] [--force]]\n"
          "                [--stats]\n"
          "                [--stats-field FIELD]\n"
          "                [--syslog-year YYYY] [--syslog-timezone +/-HH:MM]\n"
          "                [--top count | --bottom count] [--record first|last]\n"
          "                [--context-before count] [--context-after count]\n"
          "                [--profile file] [--save-profile file [--profile-overwrite]]\n"
          "                [--follow [--from-start] [--flush-partial]]\n"
          "                [--tui]\n"
          "                [--max-line-bytes count]\n"
          "\n"
          "Options:\n"
          "  --format FORMAT          raw, jsonl, syslog3164, syslog5424, cri,\n"
          "                           apache-common, apache-combined, nginx-combined,\n"
          "                           custom, or auto (default: raw).\n"
          "                           With multiple files (up to 16), one shared format is used; auto detects each file.\n"
          "  --pattern PATTERN        Define a custom %{TOKEN} parsing pattern.\n"
          "  --filter EXPRESSION      Combine field tests with =, !=, ~, <, <=, >, >=, and/or.\n"
          "  --regex pattern          Match a regular expression against each raw line.\n"
          "  --regex-field f=pattern  Match a regular expression against one field.\n"
          "  --exclude-regex pattern  Exclude records matching a raw-line regex.\n"
          "  --export text|csv        Write selected records to an output file.\n"
          "  --output path            Set the export destination.\n"
          "  --columns list           Set CSV columns (comma-separated field names).\n"
          "  --force                  Replace an existing export destination.\n"
          "  --stats                  Print exact record and level counts.\n"
          "  --stats-field FIELD      Focus frequent-value statistics on one field.\n"
          "  --syslog-year YYYY       Supply the year missing from RFC 3164 timestamps.\n"
          "  --syslog-timezone OFFSET Assume this UTC offset for yearless syslog timestamps (default: +00:00).\n"
          "  --top count             Show the first count matching records.\n"
          "  --bottom count          Show the last count matching records.\n"
          "  --record first|last     Print the complete first or last matching record.\n"
          "  --context-before count  Set TUI context records before selection (max 100).\n"
          "  --context-after count   Set TUI context records after selection (max 100).\n"
          "  --profile file          Load versioned query settings; CLI options override them.\n"
          "  --save-profile file     Save the effective query settings to a profile.\n"
          "  --profile-overwrite     Allow --save-profile to replace an existing file.\n"
          "  --follow                 Follow appended records until interrupted.\n"
          "  --from-start             Read existing records before following.\n"
          "  --flush-partial          Emit a final incomplete record on Ctrl-C.\n"
          "  --tui                    Browse records in an interactive terminal; combine with --follow to monitor.\n"
          "  --max-line-bytes count   Lower the 16 MiB maximum line size.\n"
          "  --help                   Show this help.\n"
          "  --version                Show the version.\n",
          stream);
}

static int parse_unsigned_size(const char *text, size_t maximum, size_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (text == NULL || *text == '\0' || *text == '-') {
        return 0;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > maximum) {
        return 0;
    }
    *value = (size_t)parsed;
    return 1;
}

static int parse_syslog_timezone(const char *text, int *offset_seconds)
{
    unsigned hours;
    unsigned minutes;
    int sign;
    if (text == NULL || strlen(text) != 6 ||
        (text[0] != '+' && text[0] != '-') ||
        text[1] < '0' || text[1] > '9' ||
        text[2] < '0' || text[2] > '9' || text[3] != ':' ||
        text[4] < '0' || text[4] > '9' ||
        text[5] < '0' || text[5] > '9') return 0;
    hours = (unsigned)(text[1] - '0') * 10u + (unsigned)(text[2] - '0');
    minutes = (unsigned)(text[4] - '0') * 10u + (unsigned)(text[5] - '0');
    if (hours > 23 || minutes > 59) return 0;
    sign = text[0] == '+' ? 1 : -1;
    *offset_seconds = sign * (int)(hours * 3600u + minutes * 60u);
    return 1;
}

static int parse_context_size(const char *text, size_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    if (text == NULL || *text == '\0' || *text == '-') return 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > 100) return 0;
    *value = (size_t)parsed;
    return 1;
}

static const char *profile_format_name(InputFormat format)
{
    switch (format) {
    case FORMAT_RAW: return "raw";
    case FORMAT_JSONL: return "jsonl";
    case FORMAT_SYSLOG3164: return "syslog3164";
    case FORMAT_SYSLOG5424: return "syslog5424";
    case FORMAT_CRI: return "cri";
    case FORMAT_APACHE_COMMON: return "apache-common";
    case FORMAT_APACHE_COMBINED: return "apache-combined";
    case FORMAT_NGINX_COMBINED: return "nginx-combined";
    case FORMAT_CUSTOM: return "custom";
    case FORMAT_AUTO: return "auto";
    default: return NULL;
    }
}

static int profile_copy_value(char *destination, size_t capacity,
                              const char *value)
{
    size_t length = strlen(value);
    if (length >= capacity || strchr(value, '\n') != NULL ||
        strchr(value, '\r') != NULL) return 0;
    memcpy(destination, value, length + 1);
    return 1;
}

static int load_query_profile(Options *options)
{
    FILE *file;
    char line[PROFILE_VALUE_CAPACITY + 32];
    unsigned int seen = 0;
    int line_number = 0;
    unsigned int profile_year = 0;
    int profile_year_set = 0;
    int timezone_set = 0;
    int profile_timezone = 0;
    size_t profile_before = 4;
    size_t profile_after = 4;
    int before_set = 0;
    int after_set = 0;
    file = fopen(options->profile_path, "rb");
    if (file == NULL) {
        fprintf(stderr, "logview: cannot open query profile '%s'.\n",
                options->profile_path);
        return 0;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        fprintf(stderr, "logview: '%s' is not a supported query profile (expected GLOGVIEW_PROFILE 1).\n",
                options->profile_path);
        return 0;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "GLOGVIEW_PROFILE 1") != 0) {
        fclose(file);
        fprintf(stderr, "logview: '%s' is not a supported query profile (expected GLOGVIEW_PROFILE 1).\n",
                options->profile_path);
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *separator;
        char *value;
        unsigned int bit;
        size_t line_length;
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            fclose(file);
            fprintf(stderr, "logview: query profile line %d is too long.\n",
                    line_number + 1);
            return 0;
        }
        line_length = strlen(line);
        if (line_length != 0 && line[line_length - 1] == '\n') {
            line[--line_length] = '\0';
            if (line_length != 0 && line[line_length - 1] == '\r') {
                line[--line_length] = '\0';
            }
        }
        if (strchr(line, '\r') != NULL || strchr(line, '\n') != NULL) {
            goto invalid_profile;
        }
        if (line[0] == '\0') continue;
        separator = strchr(line, '=');
        if (separator == NULL) goto invalid_profile;
        *separator = '\0';
        value = separator + 1;
        if (strcmp(line, "format") == 0) bit = 1u << 0;
        else if (strcmp(line, "pattern") == 0) bit = 1u << 1;
        else if (strcmp(line, "filter") == 0) bit = 1u << 2;
        else if (strcmp(line, "regex") == 0) bit = 1u << 3;
        else if (strcmp(line, "regex_field") == 0) bit = 1u << 4;
        else if (strcmp(line, "syslog_year") == 0) bit = 1u << 5;
        else if (strcmp(line, "syslog_timezone") == 0) bit = 1u << 6;
        else if (strcmp(line, "context_before") == 0) bit = 1u << 7;
        else if (strcmp(line, "context_after") == 0) bit = 1u << 8;
        else if (strcmp(line, "exclude_regex") == 0) bit = 1u << 9;
        else goto invalid_profile;
        if ((seen & bit) != 0) goto invalid_profile;
        seen |= bit;
        if (bit == (1u << 0)) {
            InputFormat parsed = parser_format_from_name(value);
            if ((int)parsed < 0) goto invalid_profile;
            if (!options->format_set) {
                options->format = parsed;
                options->format_set = 1;
            }
        } else if (bit == (1u << 1)) {
            if (!profile_copy_value(options->profile_pattern,
                                    sizeof(options->profile_pattern), value)) goto invalid_profile;
        } else if (bit == (1u << 2)) {
            if (!profile_copy_value(options->profile_filter,
                                    sizeof(options->profile_filter), value)) goto invalid_profile;
        } else if (bit == (1u << 3)) {
            if (!profile_copy_value(options->profile_regex,
                                    sizeof(options->profile_regex), value)) goto invalid_profile;
        } else if (bit == (1u << 4)) {
            if (!profile_copy_value(options->profile_field_regex,
                                    sizeof(options->profile_field_regex), value)) goto invalid_profile;
        } else if (bit == (1u << 5)) {
            char *end = NULL;
            unsigned long year;
            errno = 0;
            year = strtoul(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' || year > 9999) goto invalid_profile;
            profile_year = (unsigned int)year;
            profile_year_set = year != 0;
        } else if (bit == (1u << 6)) {
            if (value[0] != '\0') {
                if (!parse_syslog_timezone(value, &profile_timezone)) goto invalid_profile;
                timezone_set = 1;
            }
        } else if (bit == (1u << 7)) {
            if (!parse_context_size(value, &profile_before)) goto invalid_profile;
            before_set = 1;
        } else if (bit == (1u << 8)) {
            if (!parse_context_size(value, &profile_after)) goto invalid_profile;
            after_set = 1;
        } else if (bit == (1u << 9)) {
            if (!profile_copy_value(options->profile_exclude_regex,
                                    sizeof(options->profile_exclude_regex), value)) goto invalid_profile;
        }
    }
    if (ferror(file)) {
        fclose(file);
        fprintf(stderr, "logview: error reading query profile '%s'.\n",
                options->profile_path);
        return 0;
    }
    fclose(file);
    if ((seen & (1u << 0)) == 0) {
        fprintf(stderr, "logview: query profile '%s' does not define format.\n",
                options->profile_path);
        return 0;
    }
    if (options->custom_pattern == NULL && options->profile_pattern[0] != '\0' &&
        (!options->format_set || options->format == FORMAT_CUSTOM))
        options->custom_pattern = options->profile_pattern;
    if (options->filter_argument == NULL && options->profile_filter[0] != '\0')
        options->filter_argument = options->profile_filter;
    if (options->raw_regex_argument == NULL && options->profile_regex[0] != '\0')
        options->raw_regex_argument = options->profile_regex;
    if (options->exclude_regex_argument == NULL && options->profile_exclude_regex[0] != '\0')
        options->exclude_regex_argument = options->profile_exclude_regex;
    if (options->field_regex_argument == NULL && options->profile_field_regex[0] != '\0')
        options->field_regex_argument = options->profile_field_regex;
    if (profile_year_set && options->syslog_year == 0 &&
        (!options->format_set || options->format == FORMAT_SYSLOG3164 ||
         options->format == FORMAT_AUTO)) options->syslog_year = profile_year;
    if (timezone_set && !options->syslog_timezone_set &&
        (!options->format_set || options->format == FORMAT_SYSLOG3164 ||
         options->format == FORMAT_AUTO)) {
        options->syslog_timezone_offset_seconds = profile_timezone;
        options->syslog_timezone_set = 1;
    }
    if (before_set && !options->context_before_set) options->context_before = profile_before;
    if (after_set && !options->context_after_set) options->context_after = profile_after;
    return 1;

invalid_profile:
    fclose(file);
    fprintf(stderr, "logview: invalid or unsupported setting on line %d of query profile '%s'.\n",
            line_number + 1, options->profile_path);
    return 0;
}

static int save_query_profile(const Options *options)
{
    FILE *existing;
    FILE *file;
    const char *format = profile_format_name(options->format);
    int timezone_offset = options->syslog_timezone_offset_seconds;
    uint64_t absolute_timezone;
    unsigned int timezone_hours;
    unsigned int timezone_minutes;
    char timezone[8];
    size_t input_index;
    for (input_index = 0; input_index < options->input_count; input_index++) {
        if (output_paths_same_file(options->save_profile_path,
                                   options->input_paths[input_index])) {
            fputs("logview: query profile path must not be an input file.\n", stderr);
            return 0;
        }
    }
    existing = fopen(options->save_profile_path, "rb");
    if (existing != NULL) {
        fclose(existing);
        if (!options->profile_overwrite) {
            fprintf(stderr, "logview: query profile '%s' already exists; use --profile-overwrite to replace it.\n",
                    options->save_profile_path);
            return 0;
        }
    }
    file = fopen(options->save_profile_path, "wb");
    if (file == NULL) {
        fprintf(stderr, "logview: cannot write query profile '%s'.\n",
                options->save_profile_path);
        return 0;
    }
    absolute_timezone = timezone_offset < 0 ?
        (uint64_t)(-(int64_t)timezone_offset) : (uint64_t)timezone_offset;
    if (absolute_timezone > 23u * 3600u + 59u * 60u ||
        absolute_timezone % 60u != 0) {
        fclose(file);
        fputs("logview: invalid query-profile time zone offset.\n", stderr);
        return 0;
    }
    timezone_hours = (unsigned int)(absolute_timezone / 3600u);
    timezone_minutes = (unsigned int)((absolute_timezone % 3600u) / 60u);
    timezone[0] = timezone_offset < 0 ? '-' : '+';
    timezone[1] = (char)('0' + timezone_hours / 10u);
    timezone[2] = (char)('0' + timezone_hours % 10u);
    timezone[3] = ':';
    timezone[4] = (char)('0' + timezone_minutes / 10u);
    timezone[5] = (char)('0' + timezone_minutes % 10u);
    timezone[6] = '\0';
    if ((options->custom_pattern != NULL &&
         !profile_copy_value((char[PROFILE_VALUE_CAPACITY]){0},
                             PROFILE_VALUE_CAPACITY, options->custom_pattern)) ||
        (options->filter_argument != NULL &&
         !profile_copy_value((char[PROFILE_VALUE_CAPACITY]){0},
                             PROFILE_VALUE_CAPACITY, options->filter_argument)) ||
        (options->raw_regex_argument != NULL &&
         !profile_copy_value((char[PROFILE_VALUE_CAPACITY]){0},
                             PROFILE_VALUE_CAPACITY, options->raw_regex_argument)) ||
        (options->exclude_regex_argument != NULL &&
         !profile_copy_value((char[PROFILE_VALUE_CAPACITY]){0},
                             PROFILE_VALUE_CAPACITY, options->exclude_regex_argument)) ||
        (options->field_regex_argument != NULL &&
         !profile_copy_value((char[PROFILE_VALUE_CAPACITY]){0},
                             PROFILE_VALUE_CAPACITY, options->field_regex_argument))) {
        fclose(file);
        fputs("logview: profile values exceed the supported size or contain newlines.\n", stderr);
        return 0;
    }
    if (format == NULL ||
        fprintf(file, "GLOGVIEW_PROFILE 1\nformat=%s\npattern=%s\nfilter=%s\nregex=%s\nregex_field=%s\nexclude_regex=%s\nsyslog_year=%u\nsyslog_timezone=%s\ncontext_before=%u\ncontext_after=%u\n",
                format,
                options->custom_pattern == NULL ? "" : options->custom_pattern,
                options->filter_argument == NULL ? "" : options->filter_argument,
                options->raw_regex_argument == NULL ? "" : options->raw_regex_argument,
                options->field_regex_argument == NULL ? "" : options->field_regex_argument,
                options->exclude_regex_argument == NULL ? "" : options->exclude_regex_argument,
                options->syslog_year,
                options->syslog_timezone_set ? timezone : "",
                (unsigned)options->context_before,
                (unsigned)options->context_after) < 0) {
        fclose(file);
        fprintf(stderr, "logview: failed to finish query profile '%s'.\n",
                options->save_profile_path);
        return 0;
    }
    {
        int write_error = fflush(file) != 0;
        if (fclose(file) != 0) write_error = 1;
        if (write_error) {
        fprintf(stderr, "logview: failed to finish query profile '%s'.\n",
                options->save_profile_path);
        return 0;
        }
    }
    fprintf(stderr, "logview: saved query profile '%s'.\n", options->save_profile_path);
    return 1;
}

static int seek_file(FILE *file, uint64_t offset)
{
#ifdef _WIN32
    if (offset > (uint64_t)INT64_MAX) return 0;
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    if (offset > (uint64_t)LONG_MAX) return 0;
    return fseek(file, (long)offset, SEEK_SET) == 0;
#endif
}

static int parse_options(int argc, char **argv, Options *options)
{
    int index;
    memset(options, 0, sizeof(*options));
    options->max_line_bytes = LOGVIEW_MAX_LINE_BYTES;
    options->format = FORMAT_RAW;
    options->context_before = 4;
    options->context_after = 4;

    for (index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--help") == 0) {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(argument, "--version") == 0) {
            puts("logview " LOGVIEW_VERSION);
            return 1;
        }
        if (strcmp(argument, "--format") == 0) {
            if (++index >= argc || options->format_set) {
                return -1;
            }
            options->format = parser_format_from_name(argv[index]);
            if ((int)options->format < 0) {
                return -1;
            }
            options->format_set = 1;
            continue;
        }
        if (strcmp(argument, "--pattern") == 0) {
            if (++index >= argc || options->custom_pattern != NULL) return -1;
            options->custom_pattern = argv[index];
            continue;
        }
        if (strcmp(argument, "--filter") == 0) {
            if (++index >= argc || options->filter_argument != NULL) {
                return -1;
            }
            options->filter_argument = argv[index];
            continue;
        }
        if (strcmp(argument, "--regex") == 0) {
            if (++index >= argc || options->raw_regex_argument != NULL) {
                return -1;
            }
            options->raw_regex_argument = argv[index];
            continue;
        }
        if (strcmp(argument, "--regex-field") == 0) {
            if (++index >= argc || options->field_regex_argument != NULL) {
                return -1;
            }
            options->field_regex_argument = argv[index];
            continue;
        }
        if (strcmp(argument, "--exclude-regex") == 0) {
            if (++index >= argc || options->exclude_regex_argument != NULL) return -1;
            options->exclude_regex_argument = argv[index];
            continue;
        }
        if (strcmp(argument, "--export") == 0) {
            if (++index >= argc || options->export_set) return -1;
            if (strcmp(argv[index], "text") == 0) options->export_format = OUTPUT_TEXT;
            else if (strcmp(argv[index], "csv") == 0) options->export_format = OUTPUT_CSV;
            else return -1;
            options->export_set = 1;
            continue;
        }
        if (strcmp(argument, "--output") == 0) {
            if (++index >= argc || options->output_path != NULL) return -1;
            options->output_path = argv[index];
            continue;
        }
        if (strcmp(argument, "--columns") == 0) {
            if (++index >= argc || options->column_list != NULL) return -1;
            options->column_list = argv[index];
            continue;
        }
        if (strcmp(argument, "--force") == 0) {
            if (options->force_output) return -1;
            options->force_output = 1;
            continue;
        }
        if (strcmp(argument, "--stats") == 0) {
            if (options->stats_flag_set) return -1;
            options->stats_mode = 1;
            options->stats_flag_set = 1;
            continue;
        }
        if (strcmp(argument, "--stats-field") == 0) {
            if (++index >= argc || options->stats_field_argument != NULL ||
                argv[index][0] == '\0') return -1;
            options->stats_mode = 1;
            options->stats_field_argument = argv[index];
            continue;
        }
        if (strcmp(argument, "--syslog-year") == 0) {
            size_t year;
            if (++index >= argc || options->syslog_year != 0 ||
                strlen(argv[index]) != 4 ||
                !parse_unsigned_size(argv[index], 9999, &year)) return -1;
            options->syslog_year = (unsigned)year;
            continue;
        }
        if (strcmp(argument, "--syslog-timezone") == 0) {
            if (++index >= argc || options->syslog_timezone_set ||
                !parse_syslog_timezone(argv[index],
                                       &options->syslog_timezone_offset_seconds)) return -1;
            options->syslog_timezone_set = 1;
            continue;
        }
        if (strcmp(argument, "--top") == 0 || strcmp(argument, "--bottom") == 0) {
            int bottom = strcmp(argument, "--bottom") == 0;
            if (++index >= argc || options->result_limit != 0 ||
                !parse_unsigned_size(argv[index], 1000000u, &options->result_limit)) return -1;
            options->bottom_limit = bottom;
            continue;
        }
        if (strcmp(argument, "--record") == 0) {
            if (++index >= argc || options->record_mode != 0) return -1;
            if (strcmp(argv[index], "first") == 0) options->record_mode = 1;
            else if (strcmp(argv[index], "last") == 0) options->record_mode = 2;
            else return -1;
            continue;
        }
        if (strcmp(argument, "--context-before") == 0 ||
            strcmp(argument, "--context-after") == 0) {
            int is_before = strcmp(argument, "--context-before") == 0;
            size_t *target = strcmp(argument, "--context-before") == 0 ?
                             &options->context_before : &options->context_after;
            int *was_set = is_before ? &options->context_before_set :
                           &options->context_after_set;
            if (++index >= argc || *was_set ||
                !parse_context_size(argv[index], target)) return -1;
            *was_set = 1;
            continue;
        }
        if (strcmp(argument, "--profile") == 0) {
            if (++index >= argc || options->profile_path != NULL) return -1;
            options->profile_path = argv[index];
            continue;
        }
        if (strcmp(argument, "--save-profile") == 0) {
            if (++index >= argc || options->save_profile_path != NULL) return -1;
            options->save_profile_path = argv[index];
            continue;
        }
        if (strcmp(argument, "--profile-overwrite") == 0) {
            if (options->profile_overwrite) return -1;
            options->profile_overwrite = 1;
            continue;
        }
        if (strcmp(argument, "--follow") == 0) {
            if (options->follow_mode) return -1;
            options->follow_mode = 1;
            continue;
        }
        if (strcmp(argument, "--from-start") == 0) {
            if (options->from_start) return -1;
            options->from_start = 1;
            continue;
        }
        if (strcmp(argument, "--flush-partial") == 0) {
            if (options->flush_partial) return -1;
            options->flush_partial = 1;
            continue;
        }
        if (strcmp(argument, "--tui") == 0) {
            if (options->tui_mode) return -1;
            options->tui_mode = 1;
            continue;
        }
        if (strcmp(argument, "--max-line-bytes") == 0) {
            if (++index >= argc ||
                !parse_unsigned_size(argv[index], LOGVIEW_MAX_LINE_BYTES,
                                     &options->max_line_bytes)) {
                return -1;
            }
            continue;
        }
        if (argument[0] == '-' && argument[1] != '\0') {
            return -1;
        }
        if (options->input_count >= LOGVIEW_MAX_INPUTS) return -1;
        options->input_paths[options->input_count++] = argument;
    }
    if (options->input_count == 0) {
        return -1;
    }
    options->input_path = options->input_paths[0];
    if (options->profile_path != NULL && !load_query_profile(options)) return -1;
    if (options->profile_overwrite && options->save_profile_path == NULL) return -1;
    if ((options->format == FORMAT_CUSTOM) != (options->custom_pattern != NULL)) return -1;
    if ((options->syslog_year == 0 && options->syslog_timezone_set) ||
        ((options->syslog_year != 0 || options->syslog_timezone_set) &&
         (!options->format_set ||
          (options->format != FORMAT_SYSLOG3164 &&
           options->format != FORMAT_AUTO)))) return -1;
    if (options->export_set != (options->output_path != NULL) ||
        (options->column_list != NULL &&
         (!options->export_set || options->export_format != OUTPUT_CSV)) ||
        (options->force_output && !options->export_set)) return -1;
    if (options->stats_mode && (options->export_set || options->output_path != NULL ||
        options->column_list != NULL || options->force_output || options->result_limit != 0 ||
        options->record_mode != 0)) return -1;
    if ((options->from_start || options->flush_partial) && !options->follow_mode) return -1;
    if (options->follow_mode && (options->stats_mode || options->export_set ||
        options->result_limit != 0 || options->record_mode != 0)) return -1;
    if (options->tui_mode && (options->stats_mode || options->export_set ||
                              options->flush_partial || options->record_mode != 0)) return -1;
    if (options->record_mode != 0 && options->result_limit != 0) return -1;
    if (options->record_mode != 0 && options->export_set) return -1;
    if (options->input_count > 1 &&
        (options->tui_mode || options->follow_mode || options->result_limit != 0 ||
         options->record_mode != 0)) return -1;
    if (options->input_count > 1) {
        size_t left;
        size_t right;
        for (left = 0; left < options->input_count; left++) {
            if (strcmp(options->input_paths[left], "-") == 0) return -1;
            for (right = left + 1; right < options->input_count; right++) {
                if (strcmp(options->input_paths[left], options->input_paths[right]) == 0)
                    return -1;
            }
        }
    }
    return 0;
}

static int split_assignment(const char *argument, char *field_name,
                            size_t field_capacity, const char **value)
{
    const char *separator;
    size_t name_length;
    size_t index;
    if (argument == NULL) {
        return 0;
    }
    separator = strchr(argument, '=');
    if (separator == NULL) {
        return 0;
    }
    name_length = (size_t)(separator - argument);
    if (name_length == 0 || name_length >= field_capacity ||
        !((argument[0] >= 'A' && argument[0] <= 'Z') ||
          (argument[0] >= 'a' && argument[0] <= 'z') ||
          argument[0] == '_')) {
        return 0;
    }
    for (index = 1; index < name_length; index++) {
        unsigned char byte = (unsigned char)argument[index];
        if (byte == '.') {
            if (argument[index - 1] == '.' || index + 1 == name_length) return 0;
        } else if (!((byte >= 'A' && byte <= 'Z') ||
                     (byte >= 'a' && byte <= 'z') ||
                     (byte >= '0' && byte <= '9') || byte == '_')) {
            return 0;
        }
    }
    memcpy(field_name, argument, name_length);
    field_name[name_length] = '\0';
    *value = separator + 1;
    return 1;
}

static int contains_nul(ByteSpan span)
{
    return span.length > 0 && memchr(span.data, 0, span.length) != NULL;
}

static void print_io_error(const char *operation, const char *path)
{
    fprintf(stderr, "logview: %s '%s': %s\n", operation, path,
            strerror(errno));
}

static FILE *open_input_file(const char *path, int sequential)
{
#ifdef _WIN32
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    HANDLE handle;
    int descriptor;
    FILE *stream;

    if (sequential) flags |= FILE_FLAG_SEQUENTIAL_SCAN;
    handle = CreateFileA(path, GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, flags, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            errno = ENOENT;
        } else if (error == ERROR_ACCESS_DENIED ||
                   error == ERROR_SHARING_VIOLATION) {
            errno = EACCES;
        } else if (error == ERROR_TOO_MANY_OPEN_FILES) {
            errno = EMFILE;
        } else if (error == ERROR_NOT_ENOUGH_MEMORY) {
            errno = ENOMEM;
        } else {
            errno = EIO;
        }
        return NULL;
    }
    descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return NULL;
    }
    stream = _fdopen(descriptor, "rb");
    if (stream == NULL) _close(descriptor);
    return stream;
#else
    (void)sequential;
    return fopen(path, "rb");
#endif
}

static int detect_file_format(FILE *input, LineReader *reader,
                              size_t max_line_bytes, InputFormat *format)
{
    const unsigned char *lines[32];
    size_t lengths[32];
    unsigned char *copies[32] = {0};
    size_t count = 0;
    size_t total = 0;
    int success = 0;
    while (count < 32 && total < 256u * 1024u) {
        const unsigned char *line;
        size_t length;
        uint64_t number;
        uint64_t offset;
        ReaderResult result = reader_next(reader, &line, &length, &number, &offset);
        (void)number;
        (void)offset;
        if (result == READER_EOF) break;
        if (result == READER_ERROR) goto done;
        if (result == READER_TOO_LONG) continue;
        if (length == 0) continue;
        if (length > 256u * 1024u - total) break;
        copies[count] = (unsigned char *)malloc(length);
        if (copies[count] == NULL) goto done;
        memcpy(copies[count], line, length);
        lines[count] = copies[count];
        lengths[count] = length;
        total += length;
        count++;
    }
    *format = parser_detect_format(lines, lengths, count);
    success = 1;
done:
    while (count != 0) free(copies[--count]);
    if (!success) return 0;
    if (fseek(input, 0, SEEK_SET) != 0) return 0;
    reader_destroy(reader);
    return reader_init(reader, input, max_line_bytes);
}

static int accumulate_timeline(FILE *input, LineReader *reader,
                               const Options *options, JsonScratch *scratch,
                               Filter *field_filter, Regex *raw_regex,
                               Regex *field_regex, Regex *exclude_regex,
                               const char *regex_field_name,
                               LogStats *stats)
{
    const unsigned char *line;
    size_t line_length;
    uint64_t line_number;
    uint64_t byte_offset;
    ReaderResult read_result;
    if (fseek(input, 0, SEEK_SET) != 0) return 0;
    reader_destroy(reader);
    if (!reader_init(reader, input, options->max_line_bytes)) return 0;
    for (;;) {
        LogEntry entry;
        int selected = 1;
        read_result = options->format == FORMAT_JSONL ?
            reader_next_json(reader, &line, &line_length, &line_number,
                             &byte_offset, NULL, NULL) :
            reader_next(reader, &line, &line_length, &line_number,
                        &byte_offset);
        if (read_result == READER_EOF) return 1;
        if (read_result == READER_ERROR) return 0;
        if (read_result != READER_LINE || memchr(line, 0, line_length) != NULL) continue;
        entry.raw.data = line;
        entry.raw.length = line_length;
        entry.line_number = line_number;
        entry.byte_offset = byte_offset;
        entry.field_count = 0;
        entry.parse_ok = options->format == FORMAT_RAW;
        if (options->format == FORMAT_JSONL) {
            if (!jsonl_parse(&entry, scratch)) continue;
        } else if (options->format == FORMAT_CUSTOM) {
            if (!parser_custom_line(options->custom_pattern, &entry)) continue;
        } else if (options->format != FORMAT_RAW &&
                   !parser_format_line(options->format, &entry)) continue;
        if (field_filter != NULL) {
            RegexResult filter_result = REGEX_NO_MATCH;
            selected = filter_matches(field_filter, &entry, &filter_result);
            if (filter_result == REGEX_WORK_LIMIT) return 0;
        }
        if (selected && raw_regex != NULL) {
            RegexResult regex_result = regex_search(raw_regex, line, line_length);
            if (regex_result == REGEX_WORK_LIMIT) return 0;
            selected = regex_result == REGEX_MATCH;
        }
        if (selected && exclude_regex != NULL) {
            RegexResult regex_result = regex_search(exclude_regex, line, line_length);
            if (regex_result == REGEX_WORK_LIMIT) return 0;
            if (regex_result == REGEX_MATCH) selected = 0;
        }
        if (selected && field_regex != NULL) {
            ByteSpan field_value;
            if (!log_entry_find_field(&entry, regex_field_name, &field_value)) selected = 0;
            else {
                RegexResult regex_result = regex_search(field_regex,
                                                        field_value.data,
                                                        field_value.length);
                if (regex_result == REGEX_WORK_LIMIT) return 0;
                selected = regex_result == REGEX_MATCH;
            }
        }
        if (selected && !stats_timeline_add(stats, &entry)) return 0;
    }
}

typedef struct {
    const char *path;
    FILE *file;
    LineReader reader;
    InputFormat format;
    int reader_ready;
    int has_head;
    int diagnostic_emitted;
    int has_last_timestamp;
    int64_t head_timestamp;
    int64_t last_timestamp;
    uint64_t head_line_number;
    uint64_t head_byte_offset;
    const unsigned char *head_data;
    size_t head_length;
    FILE *unknown_spool;
} MultiInput;

typedef struct {
    uint64_t line_number;
    uint64_t length;
} MultiSpoolHeader;

static int multi_parse_entry(const Options *options, InputFormat format,
                             JsonScratch *scratch, const unsigned char *data,
                             size_t length, uint64_t line_number,
                             uint64_t byte_offset, LogEntry *entry)
{
    entry->raw.data = data;
    entry->raw.length = length;
    entry->line_number = line_number;
    entry->byte_offset = byte_offset;
    entry->field_count = 0;
    entry->parse_ok = format == FORMAT_RAW;
    if (format == FORMAT_JSONL) return jsonl_parse(entry, scratch);
    if (format == FORMAT_CUSTOM)
        return parser_custom_line(options->custom_pattern, entry);
    if (format != FORMAT_RAW) return parser_format_line(format, entry);
    return 1;
}

static int multi_spool_unknown(MultiInput *input, const LogEntry *entry)
{
    MultiSpoolHeader header;
    if (input->unknown_spool == NULL) input->unknown_spool = tmpfile();
    if (input->unknown_spool == NULL) return 0;
    header.line_number = entry->line_number;
    header.length = (uint64_t)entry->raw.length;
    return fwrite(&header, sizeof(header), 1, input->unknown_spool) == 1 &&
           (entry->raw.length == 0 ||
            fwrite(entry->raw.data, 1, entry->raw.length,
                   input->unknown_spool) == entry->raw.length);
}

static int multi_load_head(MultiInput *input, const Options *options,
                           JsonScratch *scratch, Filter *field_filter,
                           Regex *raw_regex, Regex *field_regex,
                           Regex *exclude_regex, const char *regex_field_name,
                           LogStats *stats,
                           FILE *timestamp_spool, uint64_t *matched_count,
                           uint64_t *rejected_count)
{
    const unsigned char *line;
    size_t length;
    uint64_t line_number;
    uint64_t byte_offset;
    for (;;) {
        LogEntry entry;
        ReaderResult read_result = input->format == FORMAT_JSONL ?
            reader_next_json(&input->reader, &line, &length, &line_number,
                             &byte_offset, NULL, NULL) :
            reader_next(&input->reader, &line, &length, &line_number,
                        &byte_offset);
        int selected = 1;
        ByteSpan timestamp;
        int64_t epoch = 0;
        int has_timestamp = 0;
        if (read_result == READER_EOF) {
            input->has_head = 0;
            return 1;
        }
        if (read_result == READER_ERROR) return 0;
        if (options->stats_mode &&
            (read_result == READER_LINE || read_result == READER_TOO_LONG) &&
            !stats_physical(stats)) return 0;
        if (read_result == READER_TOO_LONG) {
            if (*rejected_count == UINT64_MAX) return 0;
            (*rejected_count)++;
            if (options->stats_mode && !stats_rejected(stats)) return 0;
            continue;
        }
        if (memchr(line, 0, length) != NULL) {
            if (*rejected_count == UINT64_MAX) return 0;
            (*rejected_count)++;
            if (options->stats_mode && !stats_rejected(stats)) return 0;
            continue;
        }
        if (!multi_parse_entry(options, input->format, scratch, line, length,
                               line_number, byte_offset, &entry)) {
            entry.field_count = 0;
            entry.parse_ok = 0;
            if (*rejected_count == UINT64_MAX) return 0;
            (*rejected_count)++;
            if (options->stats_mode && !stats_rejected(stats)) return 0;
        }
        if (options->stats_mode && entry.parse_ok && !stats_parsed(stats)) return 0;
        if (log_entry_find_field(&entry, "timestamp", &timestamp) &&
            stats_timestamp_epoch_configured(timestamp,
                                              &stats->timestamp_config, &epoch)) {
            has_timestamp = 1;
            if (input->has_last_timestamp && epoch < input->last_timestamp) {
                fprintf(stderr,
                        "logview: '%s' is not ordered by timestamp at source line %llu.\n",
                        input->path, (unsigned long long)line_number);
                input->diagnostic_emitted = 1;
                return 0;
            }
            input->last_timestamp = epoch;
            input->has_last_timestamp = 1;
        }
        if (field_filter != NULL) {
            RegexResult filter_result = REGEX_NO_MATCH;
            selected = filter_matches(field_filter, &entry, &filter_result);
            if (filter_result == REGEX_WORK_LIMIT) return 0;
        }
        if (selected && raw_regex != NULL) {
            RegexResult regex_result = regex_search(raw_regex, line, length);
            if (regex_result == REGEX_WORK_LIMIT) return 0;
            selected = regex_result == REGEX_MATCH;
        }
        if (selected && exclude_regex != NULL) {
            RegexResult regex_result = regex_search(exclude_regex, line, length);
            if (regex_result == REGEX_WORK_LIMIT) return 0;
            if (regex_result == REGEX_MATCH) selected = 0;
        }
        if (selected && field_regex != NULL) {
            ByteSpan value;
            if (!log_entry_find_field(&entry, regex_field_name, &value)) selected = 0;
            else {
                RegexResult regex_result = regex_search(field_regex,
                                                        value.data, value.length);
                if (regex_result == REGEX_WORK_LIMIT) return 0;
                selected = regex_result == REGEX_MATCH;
            }
        }
        if (!selected) continue;
        if (*matched_count == UINT64_MAX) return 0;
        (*matched_count)++;
        if (options->stats_mode && !stats_selected(stats, &entry)) return 0;
        if (has_timestamp) {
            if (timestamp_spool != NULL &&
                fwrite(&epoch, sizeof(epoch), 1, timestamp_spool) != 1) return 0;
            input->head_timestamp = epoch;
            input->head_line_number = line_number;
            input->head_byte_offset = byte_offset;
            input->head_data = line;
            input->head_length = length;
            input->has_head = 1;
            return 1;
        }
        if (!multi_spool_unknown(input, &entry)) return 0;
    }
}

static int run_multi_input(const Options *options, Filter *field_filter,
                           Regex *raw_regex, Regex *field_regex,
                           Regex *exclude_regex, const char *regex_field_name,
                           LogStats *stats,
                           uint64_t *matched_count, uint64_t *rejected_count)
{
    MultiInput inputs[LOGVIEW_MAX_INPUTS];
    JsonScratch scratch;
    int scratch_ready = 0;
    OutputSink output;
    int output_ready = 0;
    FILE *timestamp_spool = NULL;
    size_t index;
    int result = 2;
    char error[160] = {0};
    memset(inputs, 0, sizeof(inputs));
    if (options->format == FORMAT_JSONL || options->format == FORMAT_AUTO) {
        if (!json_scratch_init(&scratch, options->max_line_bytes)) {
            fputs("logview: cannot allocate JSON parsing memory.\n", stderr);
            goto done;
        }
        scratch_ready = 1;
    }
    for (index = 0; index < options->input_count; index++) {
        size_t prior;
        inputs[index].path = options->input_paths[index];
        for (prior = 0; prior < index; prior++) {
            if (output_paths_same_file(inputs[prior].path, inputs[index].path)) {
                fprintf(stderr, "logview: input '%s' was specified more than once.\n",
                        inputs[index].path);
                result = 3;
                goto done;
            }
        }
        inputs[index].file = open_input_file(inputs[index].path, 1);
        if (inputs[index].file == NULL) {
            print_io_error("cannot open", inputs[index].path);
            goto done;
        }
        if (!reader_init(&inputs[index].reader, inputs[index].file,
                         options->max_line_bytes)) {
            fputs("logview: cannot initialize a line reader.\n", stderr);
            goto done;
        }
        inputs[index].reader_ready = 1;
        inputs[index].format = options->format;
        if (inputs[index].format == FORMAT_AUTO &&
            !detect_file_format(inputs[index].file, &inputs[index].reader,
                                options->max_line_bytes, &inputs[index].format)) {
            fprintf(stderr, "logview: cannot detect format of '%s'.\n",
                    inputs[index].path);
            goto done;
        }
    }
    if (options->output_path != NULL) {
        for (index = 0; index < options->input_count; index++) {
            if (output_paths_same_file(options->output_path, inputs[index].path)) {
                fputs("logview: output path refers to an input file.\n", stderr);
                result = 3;
                goto done;
            }
        }
    }
    if (!output_init(&output, options->output_path, options->input_path,
                     options->export_set ? options->export_format : OUTPUT_TEXT,
                     options->column_list, options->force_output,
                     options->input_count > 1, error, sizeof(error))) {
        fprintf(stderr, "logview: %s.\n", error);
        output_ready = output.stream != NULL;
        goto done;
    }
    output_ready = 1;
    if (options->stats_mode) {
        timestamp_spool = tmpfile();
        if (timestamp_spool == NULL) {
            fputs("logview: cannot create temporary timestamp storage.\n", stderr);
            goto done;
        }
    }
    for (index = 0; index < options->input_count; index++) {
        if (!multi_load_head(&inputs[index], options, &scratch, field_filter,
                             raw_regex, field_regex, exclude_regex,
                             regex_field_name, stats,
                             timestamp_spool, matched_count, rejected_count)) {
            if (!inputs[index].diagnostic_emitted) {
                if (inputs[index].reader.input_error)
                    print_io_error("cannot read", inputs[index].path);
                else
                    fprintf(stderr, "logview: cannot process '%s'.\n", inputs[index].path);
            }
            goto done;
        }
    }
    result = 0;
    for (;;) {
        size_t best = options->input_count;
        LogEntry entry;
        for (index = 0; index < options->input_count; index++) {
            if (inputs[index].has_head &&
                (best == options->input_count ||
                 inputs[index].head_timestamp < inputs[best].head_timestamp ||
                 (inputs[index].head_timestamp == inputs[best].head_timestamp &&
                  index < best))) best = index;
        }
        if (best == options->input_count) break;
        if (!options->stats_mode) {
            if (!multi_parse_entry(options, inputs[best].format, &scratch,
                                   inputs[best].head_data, inputs[best].head_length,
                                   inputs[best].head_line_number,
                                   inputs[best].head_byte_offset, &entry) ||
                !output_write_source(&output, &entry, inputs[best].path)) {
                print_io_error("cannot write", options->output_path != NULL ?
                               options->output_path : "standard output");
                result = 2;
                goto done;
            }
        }
        if (!multi_load_head(&inputs[best], options, &scratch, field_filter,
                             raw_regex, field_regex, exclude_regex,
                             regex_field_name, stats,
                             timestamp_spool, matched_count, rejected_count)) {
            if (!inputs[best].diagnostic_emitted) {
                if (inputs[best].reader.input_error)
                    print_io_error("cannot read", inputs[best].path);
                else
                    fprintf(stderr, "logview: cannot process '%s'.\n", inputs[best].path);
            }
            result = 2;
            goto done;
        }
    }
    for (index = 0; index < options->input_count; index++) {
        MultiSpoolHeader header;
        unsigned char *data;
        LogEntry entry;
        if (inputs[index].unknown_spool == NULL) continue;
        rewind(inputs[index].unknown_spool);
        while (fread(&header, sizeof(header), 1, inputs[index].unknown_spool) == 1) {
            if (header.length > options->max_line_bytes) {
                fputs("logview: invalid temporary record size.\n", stderr);
                result = 2;
                goto done;
            }
            data = (unsigned char *)malloc(header.length == 0 ? 1 : (size_t)header.length);
            if (data == NULL || (header.length != 0 &&
                fread(data, 1, (size_t)header.length, inputs[index].unknown_spool) != header.length)) {
                free(data);
                fputs("logview: cannot read temporary records without timestamps.\n", stderr);
                result = 2;
                goto done;
            }
            entry.raw.data = data;
            entry.raw.length = (size_t)header.length;
            entry.line_number = header.line_number;
            entry.byte_offset = 0;
            entry.field_count = 0;
            entry.parse_ok = 0;
            if (inputs[index].format == FORMAT_JSONL &&
                !jsonl_parse(&entry, &scratch)) entry.field_count = 0;
            else if (inputs[index].format == FORMAT_CUSTOM &&
                     !parser_custom_line(options->custom_pattern, &entry)) entry.field_count = 0;
            else if (inputs[index].format != FORMAT_RAW &&
                     inputs[index].format != FORMAT_JSONL &&
                     !parser_format_line(inputs[index].format, &entry)) entry.field_count = 0;
            if (!options->stats_mode &&
                !output_write_source(&output, &entry, inputs[index].path)) {
                free(data);
                print_io_error("cannot write", options->output_path != NULL ?
                               options->output_path : "standard output");
                result = 2;
                goto done;
            }
            free(data);
        }
        if (ferror(inputs[index].unknown_spool)) {
            fputs("logview: cannot read temporary records without timestamps.\n", stderr);
            result = 2;
            goto done;
        }
    }
    if (output_ready && !output_finish(&output)) {
        print_io_error("cannot finish output", options->output_path != NULL ?
                       options->output_path : "standard output");
        output_ready = 0;
        result = 2;
        goto done;
    }
    output_ready = 0;
    if (timestamp_spool != NULL) {
        int64_t epoch;
        rewind(timestamp_spool);
        while (fread(&epoch, sizeof(epoch), 1, timestamp_spool) == 1) {
            if (!stats_timeline_add_epoch(stats, epoch)) {
                fputs("logview: cannot update statistics timeline.\n", stderr);
                result = 2;
                goto done;
            }
        }
        if (ferror(timestamp_spool)) {
            fputs("logview: cannot read temporary timestamp storage.\n", stderr);
            result = 2;
            goto done;
        }
    }
    if (options->stats_mode && !stats_print(stats, stdout)) {
        print_io_error("cannot write statistics", "standard output");
        result = 2;
        goto done;
    }
    if (*rejected_count != 0)
        fprintf(stderr, "logview: skipped %llu invalid or oversized record(s).\n",
                (unsigned long long)*rejected_count);
    result = *matched_count == 0 ? 1 : 0;
done:
    if (output_ready) output_finish(&output);
    if (timestamp_spool != NULL) fclose(timestamp_spool);
    for (index = 0; index < options->input_count; index++) {
        if (inputs[index].unknown_spool != NULL) fclose(inputs[index].unknown_spool);
        if (inputs[index].reader_ready) reader_destroy(&inputs[index].reader);
        if (inputs[index].file != NULL) fclose(inputs[index].file);
    }
    if (scratch_ready) json_scratch_destroy(&scratch);
    return result;
}

int main(int argc, char **argv)
{
    Options options;
    int option_result;
    FILE *input = NULL;
    OutputSink output;
    int output_ready = 0;
    LineReader reader;
    JsonScratch scratch;
    int scratch_ready = 0;
    int reader_ready = 0;
    Regex *raw_regex = NULL;
    Regex *field_regex = NULL;
    Regex *exclude_regex = NULL;
    Filter *field_filter = NULL;
    LogStats stats;
    StatsTimestampConfig timestamp_config;
    char regex_field_name[FIELD_NAME_CAPACITY];
    const char *regex_field_pattern = NULL;
    char regex_error[160] = {0};
    uint64_t matched_count = 0;
    uint64_t rejected_count = 0;
    uint64_t follow_line_base = 0;
    int result_code = 2;
    int flush_on_stop = 0;
    unsigned int follow_read_error_retries = 0;
    FILE *bottom_spool = NULL;
    BottomRecord *bottom_records = NULL;
    size_t bottom_count = 0;
    size_t bottom_next = 0;
    uint64_t bottom_spool_offset = 0;
    unsigned char *last_record = NULL;
    size_t last_record_length = 0;
    const unsigned char *line;
    size_t line_length;
    uint64_t line_number;
    uint64_t byte_offset;
    ReaderResult read_result;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    option_result = parse_options(argc, argv, &options);
    if (option_result > 0) {
        return 0;
    }
    if (option_result < 0) {
        print_usage(stderr);
        return 3;
    }
    if (options.follow_mode || options.tui_mode) {
        logview_stop_requested = 0;
        signal(SIGINT, request_stop);
    }
    stats_init(&stats);
    timestamp_config.syslog_year = options.syslog_year;
    timestamp_config.syslog_timezone_offset_seconds =
        options.syslog_timezone_offset_seconds;
    if (!stats_set_timestamp_config(&stats, &timestamp_config)) {
        fputs("logview: invalid syslog timestamp configuration.\n", stderr);
        return 3;
    }
    if (options.stats_field_argument != NULL &&
        !stats_set_focus_field(&stats,
                               (const unsigned char *)options.stats_field_argument,
                               strlen(options.stats_field_argument))) {
        fputs("logview: invalid --stats-field value.\n", stderr);
        return 3;
    }

    if (options.filter_argument != NULL) {
        field_filter = filter_compile(options.filter_argument, regex_error,
                                      sizeof(regex_error));
        if (field_filter == NULL) {
            fprintf(stderr, "logview: invalid --filter expression: %s\n", regex_error);
            return 3;
        }
    }
    if (options.field_regex_argument != NULL &&
        !split_assignment(options.field_regex_argument, regex_field_name,
                          sizeof(regex_field_name), &regex_field_pattern)) {
        fputs("logview: --regex-field expects field=pattern.\n", stderr);
        return 3;
    }
    if (options.format == FORMAT_CUSTOM) {
        char pattern_error[160];
        if (!parser_custom_validate(options.custom_pattern, pattern_error,
                                    sizeof(pattern_error))) {
            fprintf(stderr, "logview: invalid custom pattern: %s\n", pattern_error);
            return 3;
        }
    }

    if (options.raw_regex_argument != NULL) {
        raw_regex = regex_compile(options.raw_regex_argument, regex_error,
                                  sizeof(regex_error));
        if (raw_regex == NULL) {
            fprintf(stderr, "logview: invalid --regex: %s\n", regex_error);
            filter_free(field_filter);
            return 3;
        }
    }
    if (options.exclude_regex_argument != NULL) {
        exclude_regex = regex_compile(options.exclude_regex_argument, regex_error,
                                     sizeof(regex_error));
        if (exclude_regex == NULL) {
            fprintf(stderr, "logview: invalid --exclude-regex: %s\n", regex_error);
            regex_free(raw_regex);
            filter_free(field_filter);
            return 3;
        }
    }
    if (regex_field_pattern != NULL) {
        field_regex = regex_compile(regex_field_pattern, regex_error,
                                    sizeof(regex_error));
        if (field_regex == NULL) {
            fprintf(stderr, "logview: invalid --regex-field pattern: %s\n",
                    regex_error);
            regex_free(raw_regex);
            regex_free(exclude_regex);
            filter_free(field_filter);
            return 3;
        }
    }

    if (options.save_profile_path != NULL && !save_query_profile(&options)) {
        regex_free(raw_regex);
        regex_free(field_regex);
        regex_free(exclude_regex);
        filter_free(field_filter);
        return 2;
    }

    if (options.input_count > 1) {
        result_code = run_multi_input(&options, field_filter, raw_regex,
                                      field_regex, exclude_regex,
                                      regex_field_name, &stats,
                                      &matched_count, &rejected_count);
        goto cleanup;
    }

    input = open_input_file(options.input_path, !options.tui_mode);
    if (input == NULL) {
        print_io_error("cannot open", options.input_path);
        goto cleanup;
    }
    if (!reader_init(&reader, input, options.max_line_bytes)) {
        fputs("logview: cannot initialize the line reader.\n", stderr);
        goto cleanup;
    }
    reader_ready = 1;
    if (!output_init(&output, options.output_path, options.input_path,
                     options.export_set ? options.export_format : OUTPUT_TEXT,
                     options.column_list, options.force_output, 0,
                     regex_error, sizeof(regex_error))) {
        fprintf(stderr, "logview: %s.\n", regex_error);
        output_ready = output.stream != NULL;
        goto cleanup;
    }
    output_ready = 1;
    if (options.result_limit != 0 && options.bottom_limit && !options.tui_mode) {
        bottom_spool = tmpfile();
        bottom_records = (BottomRecord *)calloc(options.result_limit,
                                                sizeof(*bottom_records));
        if (bottom_spool == NULL || bottom_records == NULL) {
            fputs("logview: cannot allocate temporary storage for --bottom.\n", stderr);
            goto cleanup;
        }
    }
    if (options.format == FORMAT_JSONL) {
        if (!json_scratch_init(&scratch, options.max_line_bytes)) {
            fputs("logview: cannot allocate JSON parsing memory.\n", stderr);
            goto cleanup;
        }
        scratch_ready = 1;
    }
    if (options.format == FORMAT_AUTO) {
        InputFormat detected;
        if (!detect_file_format(input, &reader, options.max_line_bytes, &detected)) {
            fputs("logview: cannot sample input for format detection.\n", stderr);
            goto cleanup;
        }
        options.format = detected;
        if (options.format == FORMAT_JSONL) {
            if (!json_scratch_init(&scratch, options.max_line_bytes)) {
                fputs("logview: cannot allocate JSON parsing memory.\n", stderr);
                goto cleanup;
            }
            scratch_ready = 1;
        }
    }
    if (options.tui_mode) {
        result_code = tui_run(&input, &reader, options.format,
                              options.custom_pattern, &scratch,
                              &field_filter, &raw_regex, &field_regex,
                              &exclude_regex, regex_field_name,
                              options.follow_mode,
                              options.result_limit, options.bottom_limit,
                              options.context_before,
                              options.context_after,
                              &timestamp_config,
                              options.input_path,
                              options.input_path);
        goto cleanup;
    }
    if (options.follow_mode && !options.from_start && !follow_seek_end(&reader)) {
        fputs("logview: cannot seek to the end of the input for follow mode.\n", stderr);
        goto cleanup;
    }

    result_code = 0;
    for (;;) {
        LogEntry entry;
        int selected = 1;

        read_result = options.format == FORMAT_JSONL ?
            reader_next_json(&reader, &line, &line_length, &line_number,
                             &byte_offset, NULL, NULL) :
            reader_next(&reader, &line, &line_length,
                        &line_number, &byte_offset);
        if (read_result == READER_EOF) {
            if (!options.follow_mode) break;
            if (logview_stop_requested) {
                if (!options.flush_partial) break;
                if (flush_on_stop) break;
                flush_on_stop = 1;
                if (!reader_reposition(&reader, reader.next_offset,
                                       follow_line_base)) {
                    fputs("logview: cannot prepare the final partial record.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
                continue;
            }
            {
                FollowResult follow_result = follow_wait(&input, options.input_path,
                                                         &reader, reader.next_offset,
                                                         0,
                                                         &follow_line_base);
                if (follow_result == FOLLOW_ERROR) {
                    print_io_error("cannot follow", options.input_path);
                    result_code = 2;
                    goto cleanup;
                }
                if (follow_result == FOLLOW_STOPPED) {
                    if (!options.flush_partial) break;
                    flush_on_stop = 1;
                    if (!reader_reposition(&reader, reader.next_offset,
                                           follow_line_base)) {
                        fputs("logview: cannot prepare the final partial record.\n", stderr);
                        result_code = 2;
                        goto cleanup;
                    }
                }
            }
            continue;
        }
        if (options.follow_mode &&
            (read_result == READER_LINE || read_result == READER_TOO_LONG) &&
            !reader.line_terminated) {
            if (logview_stop_requested) {
                if (!options.flush_partial) break;
                flush_on_stop = 1;
            } else {
                uint64_t observed_end = reader.next_offset;
                if (!reader_reposition(&reader, byte_offset, follow_line_base)) {
                    fputs("logview: cannot preserve an incomplete record while following.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
                {
                    FollowResult follow_result = follow_wait(&input, options.input_path,
                                                             &reader, observed_end,
                                                             1,
                                                             &follow_line_base);
                    if (follow_result == FOLLOW_ERROR) {
                        print_io_error("cannot follow", options.input_path);
                        result_code = 2;
                        goto cleanup;
                    }
                    if (follow_result == FOLLOW_STOPPED) {
                        if (!options.flush_partial) break;
                        flush_on_stop = 1;
                        if (!reader_reposition(&reader, reader.next_offset,
                                               follow_line_base)) {
                            fputs("logview: cannot prepare the final partial record.\n", stderr);
                            result_code = 2;
                            goto cleanup;
                        }
                    }
                }
                continue;
            }
        }
        if (options.stats_mode &&
            (read_result == READER_LINE || read_result == READER_TOO_LONG)) {
            if (!stats_physical(&stats)) {
                fputs("logview: record counter overflow.\n", stderr);
                result_code = 2;
                goto cleanup;
            }
        }
        if (read_result == READER_ERROR) {
#ifdef ENODATA
            if (options.follow_mode && errno == ENODATA &&
                follow_read_error_retries < 3u) {
                FollowResult recovery = follow_reopen_after_read_error(
                    &input, options.input_path, &reader, &follow_line_base);
                follow_read_error_retries++;
                if (recovery == FOLLOW_READY) continue;
                if (recovery == FOLLOW_STOPPED) {
                    result_code = 0;
                    goto cleanup;
                }
            }
#endif
            print_io_error("cannot read", options.input_path);
            result_code = 2;
            goto cleanup;
        }
        follow_read_error_retries = 0;
        if (read_result == READER_TOO_LONG) {
            rejected_count++;
            if (options.stats_mode && !stats_rejected(&stats)) {
                fputs("logview: rejection counter overflow.\n", stderr);
                result_code = 2;
                goto cleanup;
            }
            if (options.follow_mode) follow_line_base = line_number;
            continue;
        }

        entry.raw.data = line;
        entry.raw.length = line_length;
        entry.line_number = line_number;
        entry.byte_offset = byte_offset;
        entry.field_count = 0;
        entry.parse_ok = options.format == FORMAT_RAW;

        if (contains_nul(entry.raw)) {
            rejected_count++;
            if (options.stats_mode && !stats_rejected(&stats)) {
                fputs("logview: rejection counter overflow.\n", stderr);
                result_code = 2;
                goto cleanup;
            }
            if (options.follow_mode) follow_line_base = line_number;
            continue;
        }
        if (options.format == FORMAT_JSONL) {
            if (!jsonl_parse(&entry, &scratch)) {
                entry.field_count = 0;
                rejected_count++;
                if (options.stats_mode && !stats_rejected(&stats)) {
                    fputs("logview: rejection counter overflow.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
            }
        } else if (options.format == FORMAT_CUSTOM) {
            if (!parser_custom_line(options.custom_pattern, &entry)) {
                entry.field_count = 0;
                rejected_count++;
                if (options.stats_mode && !stats_rejected(&stats)) {
                    fputs("logview: rejection counter overflow.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
            }
        } else if (options.format != FORMAT_RAW) {
            if (!parser_format_line(options.format, &entry)) {
                entry.field_count = 0;
                rejected_count++;
                if (options.stats_mode && !stats_rejected(&stats)) {
                    fputs("logview: rejection counter overflow.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
            }
        }
        if (options.stats_mode && entry.parse_ok && !stats_parsed(&stats)) {
            fputs("logview: parsed-record counter overflow.\n", stderr);
            result_code = 2;
            goto cleanup;
        }

        if (field_filter != NULL) {
            RegexResult filter_result = REGEX_NO_MATCH;
            selected = filter_matches(field_filter, &entry, &filter_result);
            if (filter_result == REGEX_WORK_LIMIT) {
                fprintf(stderr, "logview: filter regex work limit exceeded at line %llu.\n",
                        (unsigned long long)line_number);
                result_code = 2;
                goto cleanup;
            }
        }
        if (selected && raw_regex != NULL) {
            RegexResult regex_result = regex_search(raw_regex, line, line_length);
            if (regex_result == REGEX_WORK_LIMIT) {
                fprintf(stderr, "logview: regex work limit exceeded at line %llu.\n",
                        (unsigned long long)line_number);
                result_code = 2;
                goto cleanup;
            }
            selected = regex_result == REGEX_MATCH;
        }
        if (selected && exclude_regex != NULL) {
            RegexResult regex_result = regex_search(exclude_regex, line, line_length);
            if (regex_result == REGEX_WORK_LIMIT) {
                fprintf(stderr, "logview: exclude regex work limit exceeded at line %llu.\n",
                        (unsigned long long)line_number);
                result_code = 2;
                goto cleanup;
            }
            if (regex_result == REGEX_MATCH) selected = 0;
        }
        if (selected && field_regex != NULL) {
            ByteSpan field_value;
            if (!log_entry_find_field(&entry, regex_field_name, &field_value)) {
                selected = 0;
            } else {
                RegexResult regex_result = regex_search(field_regex,
                                                        field_value.data,
                                                        field_value.length);
                if (regex_result == REGEX_WORK_LIMIT) {
                    fprintf(stderr,
                            "logview: field regex work limit exceeded at line %llu.\n",
                            (unsigned long long)line_number);
                    result_code = 2;
                    goto cleanup;
                }
                selected = regex_result == REGEX_MATCH;
            }
        }

        if (selected) {
            if (options.stats_mode && !stats_selected(&stats, &entry)) {
                fputs("logview: selected-record or level counter overflow.\n", stderr);
                result_code = 2;
                goto cleanup;
            }
            if (options.record_mode == 1) {
                if (!output_write(&output, &entry)) {
                    print_io_error("cannot write", "standard output");
                    result_code = 2;
                    goto cleanup;
                }
            } else if (options.record_mode == 2) {
                unsigned char *copy = (unsigned char *)malloc(line_length == 0 ? 1 : line_length);
                if (copy == NULL) {
                    fputs("logview: cannot allocate memory for the last record.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
                if (line_length != 0) memcpy(copy, line, line_length);
                free(last_record);
                last_record = copy;
                last_record_length = line_length;
            } else if (options.result_limit != 0 && options.bottom_limit && !options.tui_mode) {
                BottomRecord *slot = &bottom_records[bottom_next];
                slot->offset = bottom_spool_offset;
                slot->length = line_length;
                if ((line_length != 0 && fwrite(line, 1, line_length, bottom_spool) != line_length) ||
                    fputc('\n', bottom_spool) == EOF ||
                    UINT64_MAX - bottom_spool_offset < (uint64_t)line_length + 1u) {
                    fputs("logview: cannot write temporary --bottom storage.\n", stderr);
                    result_code = 2;
                    goto cleanup;
                }
                bottom_spool_offset += (uint64_t)line_length + 1u;
                bottom_next = (bottom_next + 1) % options.result_limit;
                if (bottom_count < options.result_limit) bottom_count++;
            } else if (!options.stats_mode && !output_write(&output, &entry)) {
                print_io_error("cannot write", options.output_path != NULL ? options.output_path : "standard output");
                result_code = 2;
                goto cleanup;
            }
            if (options.follow_mode && !output_flush(&output)) {
                print_io_error("cannot flush", "standard output");
                result_code = 2;
                goto cleanup;
            }
            matched_count++;
            if ((options.record_mode == 1 && matched_count != 0) ||
                (options.result_limit != 0 && !options.bottom_limit &&
                 !options.tui_mode && matched_count >= options.result_limit)) break;
        }
        if (options.follow_mode) follow_line_base = line_number;
        if (flush_on_stop && !reader.line_terminated) break;
    }

    if (options.record_mode == 2 && matched_count != 0) {
        LogEntry entry;
        entry.raw.data = last_record;
        entry.raw.length = last_record_length;
        entry.line_number = line_number;
        entry.byte_offset = byte_offset;
        entry.field_count = 0;
        entry.parse_ok = 0;
        if (!output_write(&output, &entry)) {
            print_io_error("cannot write", "standard output");
            result_code = 2;
            goto cleanup;
        }
    }
    if (options.result_limit != 0 && options.bottom_limit && !options.tui_mode && matched_count != 0) {
        size_t index;
        size_t first = bottom_count == options.result_limit ? bottom_next : 0;
        for (index = 0; index < bottom_count; index++) {
            BottomRecord *record = &bottom_records[(first + index) % options.result_limit];
            unsigned char *copy = (unsigned char *)malloc(record->length == 0 ? 1 : record->length);
            LogEntry entry;
            if (copy == NULL || !seek_file(bottom_spool, record->offset) ||
                (record->length != 0 && fread(copy, 1, record->length, bottom_spool) != record->length)) {
                free(copy);
                fputs("logview: cannot read temporary --bottom storage.\n", stderr);
                result_code = 2;
                goto cleanup;
            }
            entry.raw.data = copy;
            entry.raw.length = record->length;
            entry.line_number = 0;
            entry.byte_offset = 0;
            entry.field_count = 0;
            entry.parse_ok = 0;
            if (!output_write(&output, &entry)) {
                free(copy);
                print_io_error("cannot write", "standard output");
                result_code = 2;
                goto cleanup;
            }
            free(copy);
        }
    }
    if (output_ready && !output_finish(&output)) {
        print_io_error("cannot write", options.output_path != NULL ? options.output_path : "standard output");
        output_ready = 0;
        result_code = 2;
        goto cleanup;
    }
    output_ready = 0;
    if (options.stats_mode && stats.has_timestamp &&
        !accumulate_timeline(input, &reader, &options, &scratch, field_filter,
                             raw_regex, field_regex, exclude_regex,
                             regex_field_name, &stats)) {
        fputs("logview: cannot complete the statistics timeline pass.\n", stderr);
        result_code = 2;
        goto cleanup;
    }
    if (options.stats_mode && !stats_print(&stats, stdout)) {
        print_io_error("cannot write statistics", "standard output");
        result_code = 2;
        goto cleanup;
    }
    if (rejected_count != 0) {
        fprintf(stderr, "logview: skipped %llu invalid or oversized record(s).\n",
                (unsigned long long)rejected_count);
    }
    result_code = options.follow_mode ? 0 : (matched_count == 0 ? 1 : 0);

cleanup:
    if (output_ready && !output_finish(&output) && result_code != 2) {
        print_io_error("cannot finish output", options.output_path != NULL ? options.output_path : "standard output");
        result_code = 2;
    }
    if (scratch_ready) {
        json_scratch_destroy(&scratch);
    }
    if (reader_ready) {
        reader_destroy(&reader);
    }
    if (input != NULL && fclose(input) != 0 && result_code != 2) {
        print_io_error("cannot close", options.input_path);
        result_code = 2;
    }
    if (bottom_spool != NULL) fclose(bottom_spool);
    free(bottom_records);
    free(last_record);
    regex_free(raw_regex);
    regex_free(field_regex);
    regex_free(exclude_regex);
    filter_free(field_filter);
    return result_code;
}
