#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "terminal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

extern volatile sig_atomic_t logview_stop_requested;

struct TerminalState {
#ifdef _WIN32
    HANDLE input_handle;
    HANDLE output_handle;
    DWORD original_input_mode;
    DWORD original_output_mode;
#else
    struct termios original_mode;
#endif
    int active;
};

static TerminalKey make_key(TerminalKeyType type, int character)
{
    TerminalKey key;
    key.type = type;
    key.character = character;
    return key;
}

int terminal_enter(TerminalState **state_out)
{
    TerminalState *state;
    if (state_out == NULL) return 0;
    *state_out = NULL;
#ifdef _WIN32
    if (!_isatty(_fileno(stdin)) || !_isatty(_fileno(stdout))) return 0;
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return 0;
#endif
    state = (TerminalState *)calloc(1, sizeof(*state));
    if (state == NULL) return 0;
#ifdef _WIN32
    state->input_handle = GetStdHandle(STD_INPUT_HANDLE);
    state->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (state->input_handle == INVALID_HANDLE_VALUE ||
        state->output_handle == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(state->input_handle, &state->original_input_mode) ||
        !GetConsoleMode(state->output_handle, &state->original_output_mode)) {
        free(state);
        return 0;
    }
    if (!SetConsoleMode(state->input_handle,
                        state->original_input_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) ||
        !SetConsoleMode(state->output_handle,
                        state->original_output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        SetConsoleMode(state->input_handle, state->original_input_mode);
        SetConsoleMode(state->output_handle, state->original_output_mode);
        free(state);
        return 0;
    }
#else
    {
        struct termios raw_mode;
        if (tcgetattr(STDIN_FILENO, &state->original_mode) != 0) {
            free(state);
            return 0;
        }
        raw_mode = state->original_mode;
        raw_mode.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw_mode.c_iflag &= (tcflag_t)~(IXON | ICRNL);
        raw_mode.c_cc[VMIN] = 0;
        raw_mode.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_mode) != 0) {
            free(state);
            return 0;
        }
    }
#endif
    state->active = 1;
    *state_out = state;
    return 1;
}

void terminal_leave(TerminalState *state)
{
    if (state == NULL) return;
    if (state->active) {
#ifdef _WIN32
        SetConsoleMode(state->input_handle, state->original_input_mode);
        SetConsoleMode(state->output_handle, state->original_output_mode);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &state->original_mode);
#endif
        fputs("\033[0m\033[?25h\n", stdout);
        fflush(stdout);
    }
    free(state);
}

#ifdef _WIN32
static TerminalKey windows_key(void)
{
    int value = _getch();
    if (value == 0 || value == 224) {
        value = _getch();
        if (value == 72) return make_key(TERMINAL_KEY_UP, 0);
        if (value == 80) return make_key(TERMINAL_KEY_DOWN, 0);
        if (value == 75) return make_key(TERMINAL_KEY_LEFT, 0);
        if (value == 77) return make_key(TERMINAL_KEY_RIGHT, 0);
        if (value == 73) return make_key(TERMINAL_KEY_PAGE_UP, 0);
        if (value == 81) return make_key(TERMINAL_KEY_PAGE_DOWN, 0);
        if (value == 71) return make_key(TERMINAL_KEY_HOME, 0);
        if (value == 79) return make_key(TERMINAL_KEY_END, 0);
        return make_key(TERMINAL_KEY_NONE, 0);
    }
    if (value == 3) return make_key(TERMINAL_KEY_ESCAPE, 0);
    if (value == 13) return make_key(TERMINAL_KEY_ENTER, 0);
    if (value == 8 || value == 127) return make_key(TERMINAL_KEY_BACKSPACE, 0);
    if (value == 27) return make_key(TERMINAL_KEY_ESCAPE, 0);
    return make_key(TERMINAL_KEY_CHARACTER, value);
}
#else
static int wait_input(int milliseconds)
{
    fd_set input_set;
    struct timeval timeout;
    FD_ZERO(&input_set);
    FD_SET(STDIN_FILENO, &input_set);
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return select(STDIN_FILENO + 1, &input_set, NULL, NULL, &timeout) > 0;
}

static TerminalKey posix_key(int milliseconds)
{
    unsigned char value;
    if (!wait_input(milliseconds) || read(STDIN_FILENO, &value, 1) != 1) {
        return make_key(TERMINAL_KEY_NONE, 0);
    }
    if (value == 3) return make_key(TERMINAL_KEY_ESCAPE, 0);
    if (value == 13 || value == 10) return make_key(TERMINAL_KEY_ENTER, 0);
    if (value == 8 || value == 127) return make_key(TERMINAL_KEY_BACKSPACE, 0);
    if (value == 27) {
        unsigned char sequence[2];
        if (!wait_input(25) || read(STDIN_FILENO, &sequence[0], 1) != 1 || sequence[0] != '[' ||
            !wait_input(25) || read(STDIN_FILENO, &sequence[1], 1) != 1) {
            return make_key(TERMINAL_KEY_ESCAPE, 0);
        }
        if (sequence[1] == 'A') return make_key(TERMINAL_KEY_UP, 0);
        if (sequence[1] == 'B') return make_key(TERMINAL_KEY_DOWN, 0);
        if (sequence[1] == 'C') return make_key(TERMINAL_KEY_RIGHT, 0);
        if (sequence[1] == 'D') return make_key(TERMINAL_KEY_LEFT, 0);
        if (sequence[1] == 'H') return make_key(TERMINAL_KEY_HOME, 0);
        if (sequence[1] == 'F') return make_key(TERMINAL_KEY_END, 0);
        if (sequence[1] == '5' || sequence[1] == '6') {
            unsigned char terminator;
            if (wait_input(25) && read(STDIN_FILENO, &terminator, 1) == 1 && terminator == '~') {
                return make_key(sequence[1] == '5' ? TERMINAL_KEY_PAGE_UP : TERMINAL_KEY_PAGE_DOWN, 0);
            }
        }
        return make_key(TERMINAL_KEY_NONE, 0);
    }
    return make_key(TERMINAL_KEY_CHARACTER, value);
}
#endif

TerminalKey terminal_read_key(TerminalState *state, int wait_milliseconds)
{
    if (state == NULL || !state->active) return make_key(TERMINAL_KEY_NONE, 0);
#ifdef _WIN32
    {
        DWORD started = GetTickCount();
        while (!_kbhit()) {
            if (logview_stop_requested) return make_key(TERMINAL_KEY_NONE, 0);
            if (wait_milliseconds <= 0 || GetTickCount() - started >= (DWORD)wait_milliseconds) {
                return make_key(TERMINAL_KEY_NONE, 0);
            }
            Sleep(10);
        }
        return windows_key();
    }
#else
    if (logview_stop_requested) return make_key(TERMINAL_KEY_NONE, 0);
    return posix_key(wait_milliseconds);
#endif
}

int terminal_read_line(TerminalState *state, const char *prompt,
                       char *buffer, size_t capacity)
{
    size_t length = 0;
    int columns;
    int rows;
    if (state == NULL || buffer == NULL || capacity == 0) return 0;
    buffer[0] = '\0';
    terminal_get_size(state, &columns, &rows);
    (void)columns;
    for (;;) {
        TerminalKey key;
        if (logview_stop_requested) return 0;
        printf("\033[%d;1H\033[2K%s%s", rows, prompt, buffer);
        fflush(stdout);
        key = terminal_read_key(state, 100);
        if (key.type == TERMINAL_KEY_ENTER) return 1;
        if (key.type == TERMINAL_KEY_ESCAPE) return 0;
        if (key.type == TERMINAL_KEY_BACKSPACE && length != 0) {
            buffer[--length] = '\0';
        } else if (key.type == TERMINAL_KEY_CHARACTER && key.character >= 0x20 &&
                   key.character < 0x7f && length + 1 < capacity) {
            buffer[length++] = (char)key.character;
            buffer[length] = '\0';
        }
    }
}

void terminal_get_size(TerminalState *state, int *columns, int *rows)
{
    (void)state;
    *columns = 80;
    *rows = 24;
#ifdef _WIN32
    {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
            *columns = info.srWindow.Right - info.srWindow.Left + 1;
            *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        }
    }
#else
    {
        struct winsize size;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
            if (size.ws_col != 0) *columns = size.ws_col;
            if (size.ws_row != 0) *rows = size.ws_row;
        }
    }
#endif
}

void terminal_write(const char *text)
{
    fputs(text, stdout);
}
