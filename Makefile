CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude -Isrc
BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/logview
SOURCES = src/main.c src/reader.c src/follow.c src/terminal.c src/tui.c src/search_query.c src/json_pretty.c src/jsonl.c src/parsers.c src/filter.c src/output.c src/stats.c src/regex.c
GUI_TARGET = $(BUILD_DIR)/logview-gui
GUI_SOURCES = src/gui.c src/gui_nuklear.c src/reader.c src/follow.c src/search_query.c src/json_pretty.c src/jsonl.c src/parsers.c src/filter.c src/output.c src/stats.c src/regex.c
SDL3_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null)
SDL3_LIBS ?= $(shell pkg-config --libs sdl3 2>/dev/null)
GUI_EXTRA_LIBS ?= -lm

.PHONY: all clean test gui

all: $(TARGET)

$(TARGET): $(SOURCES) include/logview.h src/reader.h src/follow.h src/terminal.h src/tui.h src/search_query.h src/json_pretty.h src/jsonl.h src/parsers.h src/filter.h src/output.h src/stats.h src/regex.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $(TARGET)

test: $(TARGET)
	python3 tests/test_cli.py $(TARGET)

gui:
	pkg-config --exists sdl3
	mkdir -p $(BUILD_DIR)
	$(CC) -Iinclude -Isrc -Ithird_party/nuklear $(SDL3_CFLAGS) $(CFLAGS) $(GUI_SOURCES) -o $(GUI_TARGET) $(SDL3_LIBS) $(GUI_EXTRA_LIBS)

clean:
	rm -f $(TARGET) $(GUI_TARGET)
