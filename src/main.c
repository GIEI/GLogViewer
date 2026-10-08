#include "jsonl.h"
#include "filter.h"
#include "parsers.h"
#include "reader.h"
#include "regex.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define LOGVIEW_VERSION "0.2.0-dev"
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
} Options;

static void print_usage(FILE *stream)
{
    fputs("Usage: logview <file> [--format FORMAT] [--pattern PATTERN]\n"
          "                [--filter EXPRESSION]\n"
          "                [--regex pattern] [--regex-field field=pattern]\n"
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
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') ||
              byte == '_')) {
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

int main(int argc, char **argv)
{
    Options options;
    int option_result;
    FILE *input = NULL;
    LineReader reader;
    JsonScratch scratch;
    int scratch_ready = 0;
    int reader_ready = 0;
    Regex *raw_regex = NULL;
    Regex *field_regex = NULL;
    Filter *field_filter = NULL;
    char regex_field_name[FIELD_NAME_CAPACITY];
    const char *regex_field_pattern = NULL;
    char regex_error[160] = {0};
    uint64_t matched_count = 0;
    uint64_t rejected_count = 0;
    int result_code = 2;
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

    input = fopen(options.input_path, "rb");
    if (input == NULL) {
        print_io_error("cannot open", options.input_path);
        goto cleanup;
    }
    if (!reader_init(&reader, input, options.max_line_bytes)) {
        fputs("logview: cannot initialize the line reader.\n", stderr);
        goto cleanup;
    }
    reader_ready = 1;
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

    result_code = 0;
    for (;;) {
        LogEntry entry;
        int selected = 1;

        read_result = reader_next(&reader, &line, &line_length,
                                  &line_number, &byte_offset);
        if (read_result == READER_EOF) {
            break;
        }
        if (read_result == READER_ERROR) {
            print_io_error("cannot read", options.input_path);
            result_code = 2;
            goto cleanup;
        }
        if (read_result == READER_TOO_LONG) {
            rejected_count++;
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
            continue;
        }
        if (options.format == FORMAT_JSONL) {
            if (!jsonl_parse(&entry, &scratch)) {
                entry.field_count = 0;
                rejected_count++;
            }
        } else if (options.format == FORMAT_CUSTOM) {
            if (!parser_custom_line(options.custom_pattern, &entry)) {
                entry.field_count = 0;
                rejected_count++;
            }
        } else if (options.format != FORMAT_RAW) {
            if (!parser_format_line(options.format, &entry)) {
                entry.field_count = 0;
                rejected_count++;
            }
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
            if (line_length > 0 && fwrite(line, 1, line_length, stdout) != line_length) {
                print_io_error("cannot write", "standard output");
                result_code = 2;
                goto cleanup;
            }
            if (fputc('\n', stdout) == EOF) {
                print_io_error("cannot write", "standard output");
                result_code = 2;
                goto cleanup;
            }
            matched_count++;
        }
    }

    if (fflush(stdout) != 0) {
        print_io_error("cannot write", "standard output");
        result_code = 2;
        goto cleanup;
    }
    if (rejected_count != 0) {
        fprintf(stderr, "logview: skipped %llu invalid or oversized record(s).\n",
                (unsigned long long)rejected_count);
    }
    result_code = matched_count == 0 ? 1 : 0;

cleanup:
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
