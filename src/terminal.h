#ifndef LOGVIEW_TERMINAL_H
#define LOGVIEW_TERMINAL_H

#include <stddef.h>

typedef enum {
    TERMINAL_KEY_NONE,
    TERMINAL_KEY_UP,
    TERMINAL_KEY_DOWN,
    TERMINAL_KEY_LEFT,
    TERMINAL_KEY_RIGHT,
    TERMINAL_KEY_PAGE_UP,
    TERMINAL_KEY_PAGE_DOWN,
    TERMINAL_KEY_HOME,
    TERMINAL_KEY_END,
    TERMINAL_KEY_ENTER,
    TERMINAL_KEY_SHIFT_ENTER,
    TERMINAL_KEY_BACKSPACE,
    TERMINAL_KEY_ESCAPE,
    TERMINAL_KEY_CHARACTER
} TerminalKeyType;

typedef struct {
    TerminalKeyType type;
    int character;
} TerminalKey;

typedef struct TerminalState TerminalState;

int terminal_enter(TerminalState **state);
void terminal_leave(TerminalState *state);
TerminalKey terminal_read_key(TerminalState *state, int wait_milliseconds);
int terminal_read_line(TerminalState *state, const char *prompt,
                       char *buffer, size_t capacity);
void terminal_get_size(TerminalState *state, int *columns, int *rows);
void terminal_write(const char *text);

#endif
