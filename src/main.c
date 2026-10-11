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
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#define LOGVIEW_VERSION "0.5.0-alpha.2"
#define FIELD_NAME_CAPACITY 256

typedef struct {
    const char *input_path;
    const char *filter_argument;
    const char *raw_regex_argument;
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
    int follow_mode;
    int from_start;
    int flush_partial;
    int tui_mode;
} Options;

volatile sig_atomic_t logview_stop_requested = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    logview_stop_requested = 1;
}

static void print_usage(FILE *stream)
{
    fputs("Usage: logview <file> [--format FORMAT] [--pattern PATTERN]\n"
          "                [--filter EXPRESSION]\n"
          "                [--regex pattern] [--regex-field field=pattern]\n"
          "                [--export text|csv --output path [--columns list] [--force]]\n"
          "                [--stats]\n"
          "                [--follow [--from-start] [--flush-partial]]\n"
          "                [--tui]\n"
          "                [--max-line-bytes count]\n"
          "\n"
          "Options:\n"
          "  --format FORMAT          raw, jsonl, syslog3164, syslog5424,\n"
          "                           apache-common, apache-combined, nginx-combined,\n"
          "                           custom, or auto (default: raw).\n"
          "  --pattern PATTERN        Define a custom %{TOKEN} parsing pattern.\n"
          "  --filter EXPRESSION      Combine field tests with =, !=, ~, and/or.\n"
          "  --regex pattern          Match a regular expression against each raw line.\n"
          "  --regex-field f=pattern  Match a regular expression against one field.\n"
          "  --export text|csv        Write selected records to an output file.\n"
          "  --output path            Set the export destination.\n"
          "  --columns list           Set CSV columns (comma-separated field names).\n"
          "  --force                  Replace an existing export destination.\n"
          "  --stats                  Print exact record and level counts.\n"
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

static int parse_options(int argc, char **argv, Options *options)
{
    int index;
    memset(options, 0, sizeof(*options));
    options->max_line_bytes = LOGVIEW_MAX_LINE_BYTES;
    options->format = FORMAT_RAW;

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
            if (options->stats_mode) return -1;
            options->stats_mode = 1;
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
        if (options->input_path != NULL) {
            return -1;
        }
        options->input_path = argument;
    }
    if (options->input_path == NULL) {
        return -1;
    }
    if ((options->format == FORMAT_CUSTOM) != (options->custom_pattern != NULL)) return -1;
    if (options->export_set != (options->output_path != NULL) ||
        (options->column_list != NULL &&
         (!options->export_set || options->export_format != OUTPUT_CSV)) ||
        (options->force_output && !options->export_set)) return -1;
    if (options->stats_mode && (options->export_set || options->output_path != NULL ||
        options->column_list != NULL || options->force_output)) return -1;
    if ((options->from_start || options->flush_partial) && !options->follow_mode) return -1;
    if (options->follow_mode && (options->stats_mode || options->export_set)) return -1;
    if (options->tui_mode && (options->stats_mode || options->export_set ||
                              options->flush_partial)) return -1;
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
                               Regex *field_regex, const char *regex_field_name,
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
    Filter *field_filter = NULL;
    LogStats stats;
    char regex_field_name[FIELD_NAME_CAPACITY];
    const char *regex_field_pattern = NULL;
    char regex_error[160] = {0};
    uint64_t matched_count = 0;
    uint64_t rejected_count = 0;
    uint64_t follow_line_base = 0;
    int result_code = 2;
    int flush_on_stop = 0;
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
    if (regex_field_pattern != NULL) {
        field_regex = regex_compile(regex_field_pattern, regex_error,
                                    sizeof(regex_error));
        if (field_regex == NULL) {
            fprintf(stderr, "logview: invalid --regex-field pattern: %s\n",
                    regex_error);
            regex_free(raw_regex);
            filter_free(field_filter);
            return 3;
        }
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
                     options.column_list, options.force_output,
                     regex_error, sizeof(regex_error))) {
        fprintf(stderr, "logview: %s.\n", regex_error);
        output_ready = output.stream != NULL;
        goto cleanup;
    }
    output_ready = 1;
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
                              regex_field_name, options.follow_mode,
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
            print_io_error("cannot read", options.input_path);
            result_code = 2;
            goto cleanup;
        }
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
            if (!options.stats_mode && !output_write(&output, &entry)) {
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
        }
        if (options.follow_mode) follow_line_base = line_number;
        if (flush_on_stop && !reader.line_terminated) break;
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
                             raw_regex, field_regex, regex_field_name, &stats)) {
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
    regex_free(raw_regex);
    regex_free(field_regex);
    filter_free(field_filter);
    return result_code;
}
