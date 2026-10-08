#ifndef LOGVIEW_FOLLOW_H
#define LOGVIEW_FOLLOW_H

#include "reader.h"

typedef enum {
    FOLLOW_ERROR = -1,
    FOLLOW_STOPPED = 0,
    FOLLOW_READY = 1
} FollowResult;

int follow_seek_end(LineReader *reader);
FollowResult follow_wait(FILE **input, const char *path, LineReader *reader,
                         uint64_t observed_end, int pending_partial,
                         uint64_t *line_number_base);

#endif
