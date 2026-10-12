#define _POSIX_C_SOURCE 200809L

#include "output.h"

#include "jsonl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define LOGVIEW_TEXT_OUTPUT_BUFFER_SIZE (256u * 1024u)

static int flush_text_buffer(OutputSink *sink)
{
    size_t written;
    if (sink->text_buffer_length == 0) return 1;
    written = fwrite(sink->text_buffer, 1, sink->text_buffer_length,
                     sink->stream);
    if (written != sink->text_buffer_length) {
        size_t remaining = sink->text_buffer_length - written;
        if (written != 0) {
            memmove(sink->text_buffer, sink->text_buffer + written, remaining);
        }
        sink->text_buffer_length = remaining;
        return 0;
    }
    sink->text_buffer_length = 0;
    return 1;
}

static int allocate_text_buffer(OutputSink *sink, char *error,
                                size_t error_capacity)
{
    if (sink->format != OUTPUT_TEXT) return 1;
    sink->text_buffer = (unsigned char *)malloc(LOGVIEW_TEXT_OUTPUT_BUFFER_SIZE);
    if (sink->text_buffer == NULL) {
        snprintf(error, error_capacity, "cannot allocate text output buffer");
        return 0;
    }
    sink->text_buffer_capacity = LOGVIEW_TEXT_OUTPUT_BUFFER_SIZE;
    return 1;
}

static void release_text_buffer(OutputSink *sink)
{
    free(sink->text_buffer);
    sink->text_buffer = NULL;
    sink->text_buffer_length = 0;
    sink->text_buffer_capacity = 0;
}

static int valid_name(const char *name, size_t length)
{
    size_t index;
    if (length == 0 || length >= LOGVIEW_COLUMN_NAME ||
        !((name[0] >= 'A' && name[0] <= 'Z') ||
          (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return 0;
    for (index = 1; index < length; index++) {
        unsigned char byte = (unsigned char)name[index];
        if (byte == '.') {
            if (name[index - 1] == '.' || index + 1 == length) return 0;
        } else if (!((byte >= 'A' && byte <= 'Z') ||
                     (byte >= 'a' && byte <= 'z') ||
                     (byte >= '0' && byte <= '9') || byte == '_')) return 0;
    }
    return 1;
}

static int same_file(const char *left, const char *right)
{
#ifdef _WIN32
    HANDLE left_handle;
    HANDLE right_handle;
    BY_HANDLE_FILE_INFORMATION left_info;
    BY_HANDLE_FILE_INFORMATION right_info;
    int equal;
    left_handle = CreateFileA(left, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (left_handle == INVALID_HANDLE_VALUE) return 0;
    right_handle = CreateFileA(right, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (right_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(left_handle);
        return 0;
    }
    equal = GetFileInformationByHandle(left_handle, &left_info) &&
            GetFileInformationByHandle(right_handle, &right_info) &&
            left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
            left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
            left_info.nFileIndexLow == right_info.nFileIndexLow;
    CloseHandle(left_handle);
    CloseHandle(right_handle);
    return equal;
#else
    struct stat left_stat;
    struct stat right_stat;
    if (stat(left, &left_stat) != 0 || stat(right, &right_stat) != 0) return 0;
    return left_stat.st_dev == right_stat.st_dev && left_stat.st_ino == right_stat.st_ino;
#endif
}

int output_paths_same_file(const char *left, const char *right)
{
    if (left == NULL || right == NULL) return 0;
    return same_file(left, right);
}

static int parse_columns(OutputSink *sink, const char *columns,
                         int include_source_file, char *error,
                         size_t error_capacity)
{
    static const char *defaults[] = {"line_number", "timestamp", "level", "message", "raw"};
    size_t index;
    if (columns == NULL) {
        if (include_source_file) {
            strcpy(sink->columns[0], "source_file");
            for (index = 0; index < sizeof(defaults) / sizeof(defaults[0]); index++)
                strcpy(sink->columns[index + 1], defaults[index]);
            sink->column_count = sizeof(defaults) / sizeof(defaults[0]) + 1;
            return 1;
        }
        for (index = 0; index < sizeof(defaults) / sizeof(defaults[0]); index++) {
            strcpy(sink->columns[index], defaults[index]);
        }
        sink->column_count = sizeof(defaults) / sizeof(defaults[0]);
        return 1;
    }
    while (*columns != '\0') {
        const char *separator = strchr(columns, ',');
        size_t length = separator == NULL ? strlen(columns) : (size_t)(separator - columns);
        size_t prior;
        if (sink->column_count >= LOGVIEW_MAX_COLUMNS ||
            !valid_name(columns, length)) {
            snprintf(error, error_capacity, "invalid or excessive CSV column list");
            return 0;
        }
        for (prior = 0; prior < sink->column_count; prior++) {
            if (strlen(sink->columns[prior]) == length &&
                memcmp(sink->columns[prior], columns, length) == 0) {
                snprintf(error, error_capacity, "duplicate CSV column name");
                return 0;
            }
        }
        memcpy(sink->columns[sink->column_count], columns, length);
        sink->columns[sink->column_count][length] = '\0';
        sink->column_count++;
        if (separator == NULL) break;
        columns = separator + 1;
        if (*columns == '\0') {
            snprintf(error, error_capacity, "CSV column list cannot end with a comma");
            return 0;
        }
    }
    if (sink->column_count == 0) {
        snprintf(error, error_capacity, "CSV column list cannot be empty");
        return 0;
    }
    if (include_source_file) {
        size_t prior;
        int found = 0;
        for (prior = 0; prior < sink->column_count; prior++)
            if (strcmp(sink->columns[prior], "source_file") == 0) found = 1;
        if (!found) {
            if (sink->column_count >= LOGVIEW_MAX_COLUMNS) {
                snprintf(error, error_capacity, "CSV column list leaves no room for source_file");
                return 0;
            }
            strcpy(sink->columns[sink->column_count++], "source_file");
        }
    }
    return 1;
}

static int csv_write_value(FILE *stream, const unsigned char *data, size_t length)
{
    size_t index = 0;
    int quote = 0;
    for (index = 0; index < length; index++) {
        if (data[index] == ',' || data[index] == '"' ||
            data[index] == '\r' || data[index] == '\n') quote = 1;
    }
    if (quote && fputc('"', stream) == EOF) return 0;
    index = 0;
    while (index < length) {
        unsigned char byte = data[index];
        size_t utf8_length = 1;
        if (byte < 0x80) {
            if (byte == '"' && fputc('"', stream) == EOF) return 0;
            if (fputc(byte, stream) == EOF) return 0;
            index++;
            continue;
        }
        if (byte >= 0xc2 && byte <= 0xdf) utf8_length = 2;
        else if (byte >= 0xe0 && byte <= 0xef) utf8_length = 3;
        else if (byte >= 0xf0 && byte <= 0xf4) utf8_length = 4;
        else utf8_length = 0;
        if (utf8_length != 0 && index + utf8_length <= length) {
            size_t offset;
            int valid = 1;
            for (offset = 1; offset < utf8_length; offset++) {
                if ((data[index + offset] & 0xc0) != 0x80) valid = 0;
            }
            if (utf8_length == 3 && ((byte == 0xe0 && data[index + 1] < 0xa0) ||
                                     (byte == 0xed && data[index + 1] >= 0xa0))) valid = 0;
            if (utf8_length == 4 && ((byte == 0xf0 && data[index + 1] < 0x90) ||
                                     (byte == 0xf4 && data[index + 1] >= 0x90))) valid = 0;
            if (valid) {
                if (fwrite(data + index, 1, utf8_length, stream) != utf8_length) return 0;
                index += utf8_length;
                continue;
            }
        }
        if (fwrite("\xef\xbf\xbd", 1, 3, stream) != 3) return 0;
        index++;
    }
    if (quote && fputc('"', stream) == EOF) return 0;
    return 1;
}

static int write_header(OutputSink *sink)
{
    size_t index;
    if (sink->format != OUTPUT_CSV) return 1;
    for (index = 0; index < sink->column_count; index++) {
        if (index && fputc(',', sink->stream) == EOF) return 0;
        if (!csv_write_value(sink->stream,
                             (const unsigned char *)sink->columns[index],
                             strlen(sink->columns[index]))) return 0;
    }
    return fwrite("\r\n", 1, 2, sink->stream) == 2;
}

int output_init(OutputSink *sink, const char *path, const char *input_path,
                OutputFormat format, const char *columns, int force,
                int include_source_file,
                char *error, size_t error_capacity)
{
    memset(sink, 0, sizeof(*sink));
    sink->format = format;
    if (format == OUTPUT_CSV && !parse_columns(sink, columns, include_source_file,
                                               error, error_capacity)) return 0;
    if (path == NULL) {
        if (!allocate_text_buffer(sink, error, error_capacity)) return 0;
        sink->stream = stdout;
        if (!write_header(sink)) {
            release_text_buffer(sink);
            snprintf(error, error_capacity, "cannot write output header");
            return 0;
        }
        return 1;
    }
    if (same_file(path, input_path)) {
        snprintf(error, error_capacity, "output path refers to the input file");
        return 0;
    }
    if (!allocate_text_buffer(sink, error, error_capacity)) return 0;
#ifdef _WIN32
    {
        int flags = _O_WRONLY | _O_CREAT | _O_BINARY | (force ? _O_TRUNC : _O_EXCL);
        int descriptor = _open(path, flags, _S_IREAD | _S_IWRITE);
        if (descriptor >= 0) sink->stream = _fdopen(descriptor, "wb");
        if (descriptor < 0 || sink->stream == NULL) {
            if (descriptor >= 0) _close(descriptor);
            release_text_buffer(sink);
            snprintf(error, error_capacity, "cannot create output file: %s", strerror(errno));
            return 0;
        }
    }
#else
    {
        int flags = O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL);
        int descriptor = open(path, flags, 0666);
        if (descriptor >= 0) sink->stream = fdopen(descriptor, "wb");
        if (descriptor < 0 || sink->stream == NULL) {
            if (descriptor >= 0) close(descriptor);
            release_text_buffer(sink);
            snprintf(error, error_capacity, "cannot create output file: %s", strerror(errno));
            return 0;
        }
    }
#endif
    sink->owns_stream = 1;
    if (!write_header(sink)) {
        snprintf(error, error_capacity, "cannot write CSV header");
        return 0;
    }
    return 1;
}

static int write_entry_csv(OutputSink *sink, const LogEntry *entry,
                           const char *source_path)
{
    size_t index;
    for (index = 0; index < sink->column_count; index++) {
        const char *column = sink->columns[index];
        ByteSpan value = {NULL, 0};
        char number[32];
        int found = 1;
        if (strcmp(column, "line_number") == 0) {
            int count = snprintf(number, sizeof(number), "%llu", (unsigned long long)entry->line_number);
            value.data = (const unsigned char *)number;
            value.length = count < 0 ? 0 : (size_t)count;
        } else if (strcmp(column, "source_file") == 0) {
            if (source_path != NULL) {
                value.data = (const unsigned char *)source_path;
                value.length = strlen(source_path);
            }
        } else if (strcmp(column, "raw") == 0) {
            value = entry->raw;
        } else {
            found = log_entry_find_field(entry, column, &value);
        }
        if (index && fputc(',', sink->stream) == EOF) return 0;
        if (found && !csv_write_value(sink->stream, value.data, value.length)) return 0;
    }
    return fwrite("\r\n", 1, 2, sink->stream) == 2;
}

int output_write_source(OutputSink *sink, const LogEntry *entry,
                        const char *source_path)
{
    size_t required;
    if (sink->stream == NULL) return 0;
    if (sink->format == OUTPUT_CSV) return write_entry_csv(sink, entry, source_path);
    if (sink->text_buffer == NULL || entry->raw.length == (size_t)-1) return 0;
    if (source_path != NULL) {
        const unsigned char *path = (const unsigned char *)source_path;
        size_t path_length = strlen(source_path);
        size_t path_index;
        if (!flush_text_buffer(sink) || fputc('[', sink->stream) == EOF) return 0;
        for (path_index = 0; path_index < path_length; path_index++) {
            unsigned char byte = path[path_index];
            if (byte < 0x20 || byte == 0x7f) {
                if (fprintf(sink->stream, "\\x%02X", (unsigned)byte) < 0) return 0;
            } else if (fputc(byte, sink->stream) == EOF) return 0;
        }
        if (fprintf(sink->stream, ":%llu] ",
                    (unsigned long long)entry->line_number) < 0) return 0;
        if (entry->raw.length != 0 && fwrite(entry->raw.data, 1,
            entry->raw.length, sink->stream) != entry->raw.length) return 0;
        return fputc('\n', sink->stream) != EOF;
    }
    required = entry->raw.length + 1;
    if (required > sink->text_buffer_capacity) {
        if (!flush_text_buffer(sink)) return 0;
        if (entry->raw.length != 0 &&
            fwrite(entry->raw.data, 1, entry->raw.length, sink->stream) != entry->raw.length) return 0;
        return fputc('\n', sink->stream) != EOF;
    }
    if (required > sink->text_buffer_capacity - sink->text_buffer_length &&
        !flush_text_buffer(sink)) return 0;
    if (entry->raw.length != 0) {
        memcpy(sink->text_buffer + sink->text_buffer_length,
               entry->raw.data, entry->raw.length);
    }
    sink->text_buffer[sink->text_buffer_length + entry->raw.length] = '\n';
    sink->text_buffer_length += required;
    return 1;
}

int output_write(OutputSink *sink, const LogEntry *entry)
{
    return output_write_source(sink, entry, NULL);
}

int output_flush(OutputSink *sink)
{
    if (sink->stream == NULL || !flush_text_buffer(sink)) return 0;
    return fflush(sink->stream) == 0;
}

int output_finish(OutputSink *sink)
{
    int result = 1;
    if (sink->stream != NULL && !flush_text_buffer(sink)) result = 0;
    if (sink->stream != NULL && fflush(sink->stream) != 0) result = 0;
    if (sink->owns_stream && sink->stream != NULL && fclose(sink->stream) != 0) result = 0;
    sink->stream = NULL;
    sink->owns_stream = 0;
    release_text_buffer(sink);
    return result;
}
