#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "follow.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
typedef struct _stat64 FollowStat;
#else
#include <time.h>
#include <unistd.h>
typedef struct stat FollowStat;
#endif

extern volatile sig_atomic_t logview_stop_requested;

static int file_status(FILE *file, FollowStat *status)
{
#ifdef _WIN32
    return _fstat64(_fileno(file), status) == 0;
#else
    return fstat(fileno(file), status) == 0;
#endif
}

static int path_status(const char *path, FollowStat *status)
{
#ifdef _WIN32
    return _stat64(path, status) == 0;
#else
    return stat(path, status) == 0;
#endif
}

#ifndef _WIN32
static int same_identity(const FollowStat *left, const FollowStat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}
#else
static int path_matches_open_file(FILE *file, const char *path)
{
    HANDLE current_handle = (HANDLE)_get_osfhandle(_fileno(file));
    HANDLE path_handle;
    BY_HANDLE_FILE_INFORMATION current_info;
    BY_HANDLE_FILE_INFORMATION path_info;
    int matches;
    if (current_handle == INVALID_HANDLE_VALUE) return 0;
    path_handle = CreateFileA(path, FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (path_handle == INVALID_HANDLE_VALUE) return 0;
    matches = GetFileInformationByHandle(current_handle, &current_info) &&
              GetFileInformationByHandle(path_handle, &path_info) &&
              current_info.dwVolumeSerialNumber == path_info.dwVolumeSerialNumber &&
              current_info.nFileIndexHigh == path_info.nFileIndexHigh &&
              current_info.nFileIndexLow == path_info.nFileIndexLow;
    CloseHandle(path_handle);
    return matches;
}
#endif

static void pause_poll(void)
{
#ifdef _WIN32
    Sleep(250);
#else
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 250000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR &&
           !logview_stop_requested) {
    }
#endif
}

int follow_seek_end(LineReader *reader)
{
    FollowStat status;
    if (!file_status(reader->file, &status) || status.st_size < 0) return 0;
    return reader_reposition(reader, (uint64_t)status.st_size, 0);
}

FollowResult follow_wait(FILE **input, const char *path, LineReader *reader,
                         uint64_t observed_end, int pending_partial,
                         uint64_t *line_number_base)
{
    int path_was_missing = 0;
    for (;;) {
        FollowStat current;
        FollowStat named;
        int has_current;
        int has_named;
        if (logview_stop_requested) return FOLLOW_STOPPED;
        has_current = file_status(*input, &current);
        has_named = path_status(path, &named);
        if (!has_named) path_was_missing = 1;
        if (has_current && has_named) {
#ifdef _WIN32
            if (path_was_missing || !path_matches_open_file(*input, path)) {
#else
            if (path_was_missing || !same_identity(&current, &named)) {
#endif
                if (!pending_partial &&
                    (!path_was_missing ||
#ifdef _WIN32
                     !path_matches_open_file(*input, path)) &&
#else
                     !same_identity(&current, &named)) &&
#endif
                    current.st_size >= 0 &&
                    (uint64_t)current.st_size > reader->next_offset) {
                    if (!reader_reposition(reader, reader->next_offset,
                                           *line_number_base)) return FOLLOW_ERROR;
                    return FOLLOW_READY;
                }
                FILE *replacement = fopen(path, "rb");
                if (replacement != NULL) {
                    reader->file = replacement;
                    if (!reader_reposition(reader, 0, 0)) {
                        reader->file = *input;
                        fclose(replacement);
                        return FOLLOW_ERROR;
                    }
                    fclose(*input);
                    *input = replacement;
                    *line_number_base = 0;
                    path_was_missing = 0;
                    return FOLLOW_READY;
                }
            } else if (named.st_size < 0 ||
                       (uint64_t)named.st_size < observed_end) {
                if (!reader_reposition(reader, 0, 0)) return FOLLOW_ERROR;
                *line_number_base = 0;
                return FOLLOW_READY;
            } else if ((uint64_t)named.st_size > observed_end) {
                if (!reader_reposition(reader, reader->next_offset,
                                       *line_number_base)) return FOLLOW_ERROR;
                return FOLLOW_READY;
            }
        }
        pause_poll();
    }
}

FollowResult follow_poll(FILE **input, const char *path, LineReader *reader,
                         uint64_t observed_end, int pending_partial,
                         uint64_t *line_number_base)
{
    FollowStat current;
    FollowStat named;
    if (logview_stop_requested) return FOLLOW_STOPPED;
    if (!file_status(*input, &current)) return FOLLOW_ERROR;
    if (!path_status(path, &named)) return FOLLOW_IDLE;
#ifdef _WIN32
    if (!path_matches_open_file(*input, path)) {
#else
    if (!same_identity(&current, &named)) {
#endif
        if (!pending_partial && current.st_size >= 0 &&
            (uint64_t)current.st_size > reader->next_offset) {
            if (!reader_reposition(reader, reader->next_offset,
                                   *line_number_base)) return FOLLOW_ERROR;
            return FOLLOW_READY;
        }
        {
            FILE *replacement = fopen(path, "rb");
            if (replacement == NULL) return FOLLOW_IDLE;
            reader->file = replacement;
            if (!reader_reposition(reader, 0, 0)) {
                reader->file = *input;
                fclose(replacement);
                return FOLLOW_ERROR;
            }
            fclose(*input);
            *input = replacement;
            *line_number_base = 0;
            return FOLLOW_RESTART;
        }
    }
    if (named.st_size < 0) return FOLLOW_ERROR;
    if ((uint64_t)named.st_size < observed_end) {
        if (!reader_reposition(reader, 0, 0)) return FOLLOW_ERROR;
        *line_number_base = 0;
        return FOLLOW_RESTART;
    }
    if ((uint64_t)named.st_size > observed_end) {
        if (!reader_reposition(reader, reader->next_offset,
                               *line_number_base)) return FOLLOW_ERROR;
        return FOLLOW_READY;
    }
    return FOLLOW_IDLE;
}

FollowResult follow_reopen_after_read_error(FILE **input, const char *path,
                                            LineReader *reader,
                                            uint64_t *line_number_base)
{
    unsigned int attempt;
    for (attempt = 0; attempt < 8u; attempt++) {
        FollowStat current;
        FollowStat named;
        FILE *replacement;
        uint64_t resume_offset = 0;
        uint64_t resume_line_number = 0;
        int same_file = 0;
        if (logview_stop_requested) return FOLLOW_STOPPED;
        replacement = fopen(path, "rb");
        if (replacement == NULL) {
            pause_poll();
            continue;
        }
        if (file_status(*input, &current) && path_status(path, &named)) {
#ifdef _WIN32
            same_file = path_matches_open_file(*input, path);
#else
            same_file = same_identity(&current, &named);
#endif
            if (same_file && current.st_size >= 0 && named.st_size >= 0 &&
                current.st_size == named.st_size &&
                (uint64_t)named.st_size >= reader->next_offset) {
                resume_offset = reader->next_offset;
                resume_line_number = *line_number_base;
            }
        }
        reader->file = replacement;
        if (!reader_reposition(reader, resume_offset, resume_line_number)) {
            reader->file = *input;
            fclose(replacement);
            return FOLLOW_ERROR;
        }
        fclose(*input);
        *input = replacement;
        *line_number_base = resume_line_number;
        return FOLLOW_READY;
    }
    if (logview_stop_requested) return FOLLOW_STOPPED;
    return FOLLOW_IDLE;
}
