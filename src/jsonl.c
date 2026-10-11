#include "jsonl.h"

#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64
#define JSON_PATH_CAPACITY 512
#define JSON_SCRATCH_EXTRA 65536u

typedef struct {
    const unsigned char *data;
    size_t length;
    size_t position;
    JsonScratch *scratch;
} JsonParser;

static int field_name_equal(ByteSpan name, const char *text)
{
    size_t length = strlen(text);
    return name.length == length &&
           memcmp(name.data, text, length) == 0;
}

static int utf8_valid(const unsigned char *data, size_t length)
{
    size_t index = 0;
    while (index < length) {
        unsigned char first = data[index++];
        unsigned char second;
        if (first < 0x80) {
            if (first == 0) {
                return 0;
            }
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            if (index >= length || (data[index] & 0xc0) != 0x80) {
                return 0;
            }
            index++;
            continue;
        }
        if (first >= 0xe0 && first <= 0xef) {
            if (index + 1 >= length) {
                return 0;
            }
            second = data[index];
            if ((second & 0xc0) != 0x80 ||
                (data[index + 1] & 0xc0) != 0x80 ||
                (first == 0xe0 && second < 0xa0) ||
                (first == 0xed && second >= 0xa0)) {
                return 0;
            }
            index += 2;
            continue;
        }
        if (first >= 0xf0 && first <= 0xf4) {
            if (index + 2 >= length) {
                return 0;
            }
            second = data[index];
            if ((second & 0xc0) != 0x80 ||
                (data[index + 1] & 0xc0) != 0x80 ||
                (data[index + 2] & 0xc0) != 0x80 ||
                (first == 0xf0 && second < 0x90) ||
                (first == 0xf4 && second >= 0x90)) {
                return 0;
            }
            index += 3;
            continue;
        }
        return 0;
    }
    return 1;
}

int json_scratch_init(JsonScratch *scratch, size_t capacity)
{
    size_t allocation_capacity;
    memset(scratch, 0, sizeof(*scratch));
    if (capacity > SIZE_MAX - JSON_SCRATCH_EXTRA) return 0;
    allocation_capacity = capacity + JSON_SCRATCH_EXTRA;
    scratch->data = (unsigned char *)malloc(allocation_capacity);
    if (scratch->data == NULL) {
        return 0;
    }
    scratch->capacity = allocation_capacity;
    return 1;
}

void json_scratch_destroy(JsonScratch *scratch)
{
    free(scratch->data);
    memset(scratch, 0, sizeof(*scratch));
}

static void skip_whitespace(JsonParser *parser)
{
    while (parser->position < parser->length) {
        unsigned char byte = parser->data[parser->position];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
            break;
        }
        parser->position++;
    }
}

static int scratch_byte(JsonParser *parser, unsigned char byte)
{
    if (parser->scratch->length >= parser->scratch->capacity) {
        return 0;
    }
    parser->scratch->data[parser->scratch->length++] = byte;
    return 1;
}

static int scratch_codepoint(JsonParser *parser, unsigned int codepoint)
{
    if (codepoint <= 0x7f) {
        return scratch_byte(parser, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7ff) {
        return scratch_byte(parser, (unsigned char)(0xc0 | (codepoint >> 6))) &&
               scratch_byte(parser, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    if (codepoint <= 0xffff) {
        return scratch_byte(parser, (unsigned char)(0xe0 | (codepoint >> 12))) &&
               scratch_byte(parser, (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
               scratch_byte(parser, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    return scratch_byte(parser, (unsigned char)(0xf0 | (codepoint >> 18))) &&
           scratch_byte(parser, (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f))) &&
           scratch_byte(parser, (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
           scratch_byte(parser, (unsigned char)(0x80 | (codepoint & 0x3f)));
}

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int parse_hex_quad(JsonParser *parser, unsigned int *value)
{
    unsigned int result = 0;
    unsigned int digit;
    size_t count;
    if (parser->length - parser->position < 4) {
        return 0;
    }
    for (count = 0; count < 4; count++) {
        int hex = hex_digit(parser->data[parser->position++]);
        if (hex < 0) {
            return 0;
        }
        digit = (unsigned int)hex;
        result = (result << 4) | digit;
    }
    *value = result;
    return 1;
}

static int parse_json_string(JsonParser *parser, ByteSpan *value)
{
    size_t start;
    if (parser->position >= parser->length ||
        parser->data[parser->position] != '"') {
        return 0;
    }
    parser->position++;
    start = parser->scratch->length;
    while (parser->position < parser->length) {
        unsigned char byte = parser->data[parser->position++];
        if (byte == '"') {
            value->data = parser->scratch->data + start;
            value->length = parser->scratch->length - start;
            return 1;
        }
        if (byte < 0x20) {
            return 0;
        }
        if (byte != '\\') {
            if (!scratch_byte(parser, byte)) {
                return 0;
            }
            continue;
        }
        if (parser->position >= parser->length) {
            return 0;
        }
        byte = parser->data[parser->position++];
        switch (byte) {
        case '"':
        case '\\':
        case '/':
            if (!scratch_byte(parser, byte)) {
                return 0;
            }
            break;
        case 'b':
            if (!scratch_byte(parser, '\b')) {
                return 0;
            }
            break;
        case 'f':
            if (!scratch_byte(parser, '\f')) {
                return 0;
            }
            break;
        case 'n':
            if (!scratch_byte(parser, '\n')) {
                return 0;
            }
            break;
        case 'r':
            if (!scratch_byte(parser, '\r')) {
                return 0;
            }
            break;
        case 't':
            if (!scratch_byte(parser, '\t')) {
                return 0;
            }
            break;
        case 'u':
            {
                unsigned int codepoint;
                if (!parse_hex_quad(parser, &codepoint)) {
                    return 0;
                }
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    unsigned int low;
                    if (parser->length - parser->position < 6 ||
                        parser->data[parser->position] != '\\' ||
                        parser->data[parser->position + 1] != 'u') {
                        return 0;
                    }
                    parser->position += 2;
                    if (!parse_hex_quad(parser, &low) ||
                        low < 0xdc00 || low > 0xdfff) {
                        return 0;
                    }
                    codepoint = 0x10000 +
                        (((codepoint - 0xd800) << 10) | (low - 0xdc00));
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return 0;
                }
                if (!scratch_codepoint(parser, codepoint)) {
                    return 0;
                }
            }
            break;
        default:
            return 0;
        }
    }
    return 0;
}

static int field_name_valid(ByteSpan name)
{
    size_t index;
    if (name.length == 0 ||
        !((name.data[0] >= 'A' && name.data[0] <= 'Z') ||
          (name.data[0] >= 'a' && name.data[0] <= 'z') ||
          name.data[0] == '_')) {
        return 0;
    }
    for (index = 1; index < name.length; index++) {
        unsigned char byte = name.data[index];
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') ||
              byte == '_')) {
            return 0;
        }
    }
    return 1;
}

static int parse_json_number(JsonParser *parser, ByteSpan *value)
{
    size_t start = parser->position;
    if (parser->position < parser->length &&
        parser->data[parser->position] == '-') {
        parser->position++;
    }
    if (parser->position >= parser->length) {
        return 0;
    }
    if (parser->data[parser->position] == '0') {
        parser->position++;
        if (parser->position < parser->length &&
            parser->data[parser->position] >= '0' &&
            parser->data[parser->position] <= '9') {
            return 0;
        }
    } else if (parser->data[parser->position] >= '1' &&
               parser->data[parser->position] <= '9') {
        do {
            parser->position++;
        } while (parser->position < parser->length &&
                 parser->data[parser->position] >= '0' &&
                 parser->data[parser->position] <= '9');
    } else {
        return 0;
    }
    if (parser->position < parser->length &&
        parser->data[parser->position] == '.') {
        parser->position++;
        if (parser->position >= parser->length ||
            parser->data[parser->position] < '0' ||
            parser->data[parser->position] > '9') {
            return 0;
        }
        while (parser->position < parser->length &&
               parser->data[parser->position] >= '0' &&
               parser->data[parser->position] <= '9') {
            parser->position++;
        }
    }
    if (parser->position < parser->length &&
        (parser->data[parser->position] == 'e' ||
         parser->data[parser->position] == 'E')) {
        parser->position++;
        if (parser->position < parser->length &&
            (parser->data[parser->position] == '+' ||
             parser->data[parser->position] == '-')) {
            parser->position++;
        }
        if (parser->position >= parser->length ||
            parser->data[parser->position] < '0' ||
            parser->data[parser->position] > '9') {
            return 0;
        }
        while (parser->position < parser->length &&
               parser->data[parser->position] >= '0' &&
               parser->data[parser->position] <= '9') {
            parser->position++;
        }
    }
    value->data = parser->data + start;
    value->length = parser->position - start;
    return 1;
}

static int parse_value(JsonParser *parser, ByteSpan *value, int *is_null)
{
    size_t start;
    *is_null = 0;
    skip_whitespace(parser);
    if (parser->position >= parser->length) {
        return 0;
    }
    if (parser->data[parser->position] == '"') {
        return parse_json_string(parser, value);
    }
    if (parser->data[parser->position] == '-' ||
        (parser->data[parser->position] >= '0' &&
         parser->data[parser->position] <= '9')) {
        return parse_json_number(parser, value);
    }
    if (parser->data[parser->position] == 't' &&
        parser->length - parser->position >= 4 &&
        memcmp(parser->data + parser->position, "true", 4) == 0) {
        value->data = parser->data + parser->position;
        value->length = 4;
        parser->position += 4;
        return 1;
    }
    if (parser->data[parser->position] == 'f' &&
        parser->length - parser->position >= 5 &&
        memcmp(parser->data + parser->position, "false", 5) == 0) {
        value->data = parser->data + parser->position;
        value->length = 5;
        parser->position += 5;
        return 1;
    }
    if (parser->data[parser->position] == 'n' &&
        parser->length - parser->position >= 4 &&
        memcmp(parser->data + parser->position, "null", 4) == 0) {
        start = parser->position;
        value->data = parser->data + start;
        value->length = 4;
        parser->position += 4;
        *is_null = 1;
        return 1;
    }
    return 0;
}

static int path_name_equal(const unsigned char *left, size_t left_length,
                           const unsigned char *right, size_t right_length)
{
    return left_length == right_length &&
           memcmp(left, right, left_length) == 0;
}

static int entry_has_field_name(const LogEntry *entry, const unsigned char *name,
                                size_t name_length)
{
    size_t index;
    for (index = 0; index < entry->field_count; index++) {
        if (path_name_equal(entry->fields[index].name.data,
                            entry->fields[index].name.length,
                            name, name_length)) return 1;
    }
    return 0;
}

static int save_field(JsonParser *parser, LogEntry *entry,
                      const unsigned char *path, size_t path_length,
                      int path_valid, ByteSpan leaf, ByteSpan value,
                      int is_null)
{
    ByteSpan stored_path;
    size_t index;
    if (is_null) return 1;
    if (path_valid && path_length != 0 &&
        !entry_has_field_name(entry, path, path_length) &&
        entry->field_count < LOGVIEW_MAX_FIELDS) {
        size_t path_start = parser->scratch->length;
        if (path_length > parser->scratch->capacity - path_start) return 0;
        memcpy(parser->scratch->data + path_start, path, path_length);
        parser->scratch->length += path_length;
        stored_path.data = parser->scratch->data + path_start;
        stored_path.length = path_length;
        entry->fields[entry->field_count].name = stored_path;
        entry->fields[entry->field_count].value = value;
        entry->field_count++;
    }
    if (!field_name_valid(leaf) ||
        entry_has_field_name(entry, leaf.data, leaf.length) ||
        entry->field_count >= LOGVIEW_MAX_FIELDS) return 1;
    for (index = 0; index < leaf.length; index++) {
        if (!scratch_byte(parser, leaf.data[index])) return 0;
    }
    stored_path.data = parser->scratch->data + parser->scratch->length - leaf.length;
    stored_path.length = leaf.length;
    entry->fields[entry->field_count].name = stored_path;
    entry->fields[entry->field_count].value = value;
    entry->field_count++;
    return 1;
}

static int build_child_path(const unsigned char *parent, size_t parent_length,
                            int parent_valid, ByteSpan key,
                            unsigned char *child, size_t *child_length)
{
    size_t required;
    if (!parent_valid || !field_name_valid(key)) return 0;
    required = parent_length + (parent_length != 0 ? 1u : 0u) + key.length;
    if (required >= JSON_PATH_CAPACITY) return 0;
    if (parent_length != 0) {
        memcpy(child, parent, parent_length);
        child[parent_length] = '.';
    }
    memcpy(child + parent_length + (parent_length != 0 ? 1u : 0u),
           key.data, key.length);
    *child_length = required;
    return 1;
}

static int parse_nested_value(JsonParser *parser, LogEntry *entry,
                              const unsigned char *path, size_t path_length,
                              int path_valid, ByteSpan leaf, unsigned int depth);

static int parse_json_object(JsonParser *parser, LogEntry *entry,
                             const unsigned char *path, size_t path_length,
                             int path_valid, unsigned int depth)
{
    ByteSpan seen[LOGVIEW_MAX_FIELDS];
    size_t seen_count = 0;
    if (depth > JSON_MAX_DEPTH || parser->position >= parser->length ||
        parser->data[parser->position++] != '{') return 0;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == '}') {
        parser->position++;
        return 1;
    }
    for (;;) {
        ByteSpan key;
        unsigned char child_path[JSON_PATH_CAPACITY];
        size_t child_path_length = 0;
        int child_path_valid;
        size_t index;
        if (seen_count >= LOGVIEW_MAX_FIELDS ||
            !parse_json_string(parser, &key)) return 0;
        for (index = 0; index < seen_count; index++) {
            if (seen[index].length == key.length &&
                memcmp(seen[index].data, key.data, key.length) == 0) return 0;
        }
        seen[seen_count++] = key;
        skip_whitespace(parser);
        if (parser->position >= parser->length || parser->data[parser->position++] != ':') {
            return 0;
        }
        child_path_valid = build_child_path(path, path_length, path_valid,
                                            key, child_path,
                                            &child_path_length);
        if (!parse_nested_value(parser, entry, child_path, child_path_length,
                                child_path_valid, key, depth + 1)) return 0;
        skip_whitespace(parser);
        if (parser->position >= parser->length) return 0;
        if (parser->data[parser->position] == '}') {
            parser->position++;
            return 1;
        }
        if (parser->data[parser->position++] != ',') return 0;
        skip_whitespace(parser);
    }
}

static int parse_json_array(JsonParser *parser, LogEntry *entry,
                            const unsigned char *path, size_t path_length,
                            int path_valid, unsigned int depth)
{
    if (depth > JSON_MAX_DEPTH || parser->position >= parser->length ||
        parser->data[parser->position++] != '[') return 0;
    skip_whitespace(parser);
    if (parser->position < parser->length && parser->data[parser->position] == ']') {
        parser->position++;
        return 1;
    }
    for (;;) {
        if (!parse_nested_value(parser, entry, path, path_length, path_valid,
                                (ByteSpan){NULL, 0}, depth + 1)) return 0;
        skip_whitespace(parser);
        if (parser->position >= parser->length) return 0;
        if (parser->data[parser->position] == ']') {
            parser->position++;
            return 1;
        }
        if (parser->data[parser->position++] != ',') return 0;
        skip_whitespace(parser);
    }
}

static int parse_nested_value(JsonParser *parser, LogEntry *entry,
                              const unsigned char *path, size_t path_length,
                              int path_valid, ByteSpan leaf, unsigned int depth)
{
    ByteSpan value;
    int is_null = 0;
    skip_whitespace(parser);
    if (parser->position >= parser->length || depth > JSON_MAX_DEPTH) return 0;
    if (parser->data[parser->position] == '{') {
        return parse_json_object(parser, entry, path, path_length,
                                 path_valid, depth);
    }
    if (parser->data[parser->position] == '[') {
        return parse_json_array(parser, entry, path, path_length,
                                path_valid, depth);
    }
    if (!parse_value(parser, &value, &is_null)) return 0;
    if (leaf.data == NULL) return 1;
    return save_field(parser, entry, path, path_length, path_valid,
                      leaf, value, is_null);
}

int jsonl_parse(LogEntry *entry, JsonScratch *scratch)
{
    JsonParser parser;
    unsigned char root_path[JSON_PATH_CAPACITY];
    entry->field_count = 0;
    entry->parse_ok = 0;
    scratch->length = 0;
    root_path[0] = '\0';
    if (entry->raw.length == 0 ||
        !utf8_valid(entry->raw.data, entry->raw.length)) return 0;
    memset(&parser, 0, sizeof(parser));
    parser.data = entry->raw.data;
    parser.length = entry->raw.length;
    parser.scratch = scratch;
    skip_whitespace(&parser);
    if (parser.position >= parser.length || parser.data[parser.position] != '{' ||
        !parse_json_object(&parser, entry, root_path, 0, 1, 0)) return 0;
    skip_whitespace(&parser);
    entry->parse_ok = parser.position == parser.length;
    return entry->parse_ok;
}

static int find_exact_field(const LogEntry *entry, const char *name,
                            ByteSpan *value)
{
    size_t index;
    for (index = 0; index < entry->field_count; index++) {
        if (field_name_equal(entry->fields[index].name, name)) {
            *value = entry->fields[index].value;
            return 1;
        }
    }
    return 0;
}

int log_entry_find_field(const LogEntry *entry, const char *name,
                         ByteSpan *value)
{
    if (find_exact_field(entry, name, value)) {
        return 1;
    }
    if (strcmp(name, "timestamp") == 0) {
        return find_exact_field(entry, "time", value) ||
               find_exact_field(entry, "ts", value);
    }
    if (strcmp(name, "level") == 0) {
        return find_exact_field(entry, "severity", value);
    }
    if (strcmp(name, "message") == 0) {
        return find_exact_field(entry, "msg", value) ||
               find_exact_field(entry, "log", value);
    }
    return 0;
}
