CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude -Isrc
BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/logview
SOURCES = src/main.c src/reader.c src/jsonl.c src/parsers.c src/filter.c src/regex.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SOURCES) include/logview.h src/reader.h src/jsonl.h src/parsers.h src/filter.h src/regex.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $(TARGET)

test: $(TARGET)
	python3 tests/test_cli.py $(TARGET)

clean:
	rm -f $(TARGET)
