#include "json_pretty.h"

#include <string.h>

#define JSON_PRETTY_MAX_DEPTH 64u
#define JSON_PRETTY_INDENT 2u

typedef enum {
    FRAME_OBJECT_KEY_OR_END,
    FRAME_OBJECT_KEY,
    FRAME_OBJECT_COLON,
    FRAME_OBJECT_VALUE,
    FRAME_OBJECT_DELIMITER,
    FRAME_ARRAY_VALUE_OR_END,
    FRAME_ARRAY_VALUE,
    FRAME_ARRAY_DELIMITER
} JsonFrameState;

typedef struct {
    JsonFrameState state;
    size_t item_count;
} JsonFrame;

typedef struct {
    const unsigned char *input;
    size_t length;
    size_t position;
    size_t output_length;
    size_t maximum_output_bytes;
    FILE *output;
} JsonPrettyParser;

static int write_byte(JsonPrettyParser *parser, unsigned char byte)
{
    if (parser->output_length >= parser->maximum_output_bytes) return 0;
    parser->output_length++;
    return parser->output == NULL || fputc(byte, parser->output) != EOF;
}

static int write_bytes(JsonPrettyParser *parser, const unsigned char *bytes,
                       size_t length)
{
    if (length > parser->maximum_output_bytes - parser->output_length) return 0;
    parser->output_length += length;
    return parser->output == NULL ||
           fwrite(bytes, 1, length, parser->output) == length;
}

static int write_indent(JsonPrettyParser *parser, size_t depth)
{
    size_t count = depth * JSON_PRETTY_INDENT;
    while (count-- != 0) {
        if (!write_byte(parser, ' ')) return 0;
    }
    return 1;
}

static void skip_whitespace(JsonPrettyParser *parser)
{
    while (parser->position < parser->length) {
        unsigned char byte = parser->input[parser->position];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') break;
        parser->position++;
    }
}

static int valid_utf8_sequence(const unsigned char *data, size_t length,
                               size_t *sequence_length)
{
    unsigned char first = data[0];
    size_t count;
    size_t index;
    if (first < 0x80) {
        *sequence_length = 1;
        return 1;
    }
    if (first >= 0xc2 && first <= 0xdf) count = 2;
    else if (first >= 0xe0 && first <= 0xef) count = 3;
    else if (first >= 0xf0 && first <= 0xf4) count = 4;
    else return 0;
    if (count > length) return 0;
    for (index = 1; index < count; index++) {
        if ((data[index] & 0xc0) != 0x80) return 0;
    }
    if ((first == 0xe0 && data[1] < 0xa0) ||
        (first == 0xed && data[1] >= 0xa0) ||
        (first == 0xf0 && data[1] < 0x90) ||
        (first == 0xf4 && data[1] >= 0x90)) return 0;
    *sequence_length = count;
    return 1;
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static int read_hex_quad(JsonPrettyParser *parser, unsigned int *value)
{
    unsigned int result = 0;
    size_t index;
    if (parser->length - parser->position < 4) return 0;
    for (index = 0; index < 4; index++) {
        int digit = hex_digit(parser->input[parser->position++]);
        if (digit < 0) return 0;
        result = (result << 4) | (unsigned int)digit;
    }
    *value = result;
    return 1;
}

static int parse_string(JsonPrettyParser *parser)
{
    size_t start = parser->position;
    if (parser->position >= parser->length ||
        parser->input[parser->position++] != '"') return 0;
    while (parser->position < parser->length) {
        unsigned char byte = parser->input[parser->position++];
        if (byte == '"') {
            return write_bytes(parser, parser->input + start,
                               parser->position - start);
        }
        if (byte < 0x20) return 0;
        if (byte == '\\') {
            unsigned char escape;
            if (parser->position >= parser->length) return 0;
            escape = parser->input[parser->position++];
            if (escape == '"' || escape == '\\' || escape == '/' ||
                escape == 'b' || escape == 'f' || escape == 'n' ||
                escape == 'r' || escape == 't') continue;
            if (escape == 'u') {
                unsigned int codepoint;
                if (!read_hex_quad(parser, &codepoint)) return 0;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    unsigned int low;
                    if (parser->length - parser->position < 6 ||
                        parser->input[parser->position] != '\\' ||
                        parser->input[parser->position + 1] != 'u') return 0;
                    parser->position += 2;
                    if (!read_hex_quad(parser, &low) ||
                        low < 0xdc00 || low > 0xdfff) return 0;
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return 0;
                }
            } else {
                return 0;
            }
        } else if (byte >= 0x80) {
            size_t sequence_length;
            size_t sequence_start = parser->position - 1;
            if (!valid_utf8_sequence(parser->input + sequence_start,
                                     parser->length - sequence_start,
                                     &sequence_length)) return 0;
            parser->position = sequence_start + sequence_length;
        }
    }
    return 0;
}

static int parse_number(JsonPrettyParser *parser)
{
    size_t start = parser->position;
    if (parser->position < parser->length &&
        parser->input[parser->position] == '-') parser->position++;
    if (parser->position >= parser->length) return 0;
    if (parser->input[parser->position] == '0') {
        parser->position++;
        if (parser->position < parser->length &&
            parser->input[parser->position] >= '0' &&
            parser->input[parser->position] <= '9') return 0;
    } else if (parser->input[parser->position] >= '1' &&
               parser->input[parser->position] <= '9') {
        do {
            parser->position++;
        } while (parser->position < parser->length &&
                 parser->input[parser->position] >= '0' &&
                 parser->input[parser->position] <= '9');
    } else {
        return 0;
    }
    if (parser->position < parser->length &&
        parser->input[parser->position] == '.') {
        parser->position++;
        if (parser->position >= parser->length ||
            parser->input[parser->position] < '0' ||
            parser->input[parser->position] > '9') return 0;
        do {
            parser->position++;
        } while (parser->position < parser->length &&
                 parser->input[parser->position] >= '0' &&
                 parser->input[parser->position] <= '9');
    }
    if (parser->position < parser->length &&
        (parser->input[parser->position] == 'e' ||
         parser->input[parser->position] == 'E')) {
        parser->position++;
        if (parser->position < parser->length &&
            (parser->input[parser->position] == '+' ||
             parser->input[parser->position] == '-')) parser->position++;
        if (parser->position >= parser->length ||
            parser->input[parser->position] < '0' ||
            parser->input[parser->position] > '9') return 0;
        do {
            parser->position++;
        } while (parser->position < parser->length &&
                 parser->input[parser->position] >= '0' &&
                 parser->input[parser->position] <= '9');
    }
    return write_bytes(parser, parser->input + start,
                       parser->position - start);
}

static int parse_literal(JsonPrettyParser *parser, const char *literal,
                         size_t length)
{
    if (length > parser->length - parser->position ||
        memcmp(parser->input + parser->position, literal, length) != 0) return 0;
    if (!write_bytes(parser, parser->input + parser->position, length)) return 0;
    parser->position += length;
    return 1;
}

static int begin_value(JsonPrettyParser *parser, JsonFrame *stack,
                       size_t *depth, int *root_complete)
{
    unsigned char byte;
    skip_whitespace(parser);
    if (parser->position >= parser->length) return 0;
    byte = parser->input[parser->position];
    if (byte == '{' || byte == '[') {
        JsonFrame *frame;
        if (*depth >= JSON_PRETTY_MAX_DEPTH) return 0;
        parser->position++;
        if (!write_byte(parser, byte)) return 0;
        frame = &stack[(*depth)++];
        frame->item_count = 0;
        if (byte == '{') {
            frame->state = FRAME_OBJECT_KEY_OR_END;
        } else {
            frame->state = FRAME_ARRAY_VALUE_OR_END;
        }
        return 1;
    }
    if (byte == '"') {
        if (!parse_string(parser)) return 0;
    } else if (byte == 't') {
        if (!parse_literal(parser, "true", 4)) return 0;
    } else if (byte == 'f') {
        if (!parse_literal(parser, "false", 5)) return 0;
    } else if (byte == 'n') {
        if (!parse_literal(parser, "null", 4)) return 0;
    } else if (byte == '-' || (byte >= '0' && byte <= '9')) {
        if (!parse_number(parser)) return 0;
    } else {
        return 0;
    }
    if (*depth == 0) *root_complete = 1;
    return 1;
}

static int pretty_run(const unsigned char *input, size_t length, FILE *output,
                      size_t maximum_output_bytes)
{
    JsonPrettyParser parser;
    JsonFrame stack[JSON_PRETTY_MAX_DEPTH];
    size_t depth = 0;
    int root_complete = 0;
    parser.input = input;
    parser.length = length;
    parser.position = 0;
    parser.output_length = 0;
    parser.maximum_output_bytes = maximum_output_bytes;
    parser.output = output;
    skip_whitespace(&parser);
    if (parser.position == parser.length ||
        !begin_value(&parser, stack, &depth, &root_complete)) return 0;
    while (depth != 0) {
        JsonFrame *frame = &stack[depth - 1];
        skip_whitespace(&parser);
        if (frame->state == FRAME_OBJECT_KEY_OR_END ||
            frame->state == FRAME_OBJECT_KEY) {
            if (parser.position < parser.length &&
                parser.input[parser.position] == '}' &&
                frame->state == FRAME_OBJECT_KEY_OR_END) {
                parser.position++;
                if (!write_byte(&parser, '}')) return 0;
                depth--;
                if (depth == 0) root_complete = 1;
                continue;
            }
            if (frame->state == FRAME_OBJECT_KEY &&
                !write_byte(&parser, '\n')) return 0;
            if (frame->state == FRAME_OBJECT_KEY_OR_END &&
                frame->item_count != 0) {
                return 0;
            }
            if (frame->state == FRAME_OBJECT_KEY_OR_END &&
                !write_byte(&parser, '\n')) {
                return 0;
            }
            if (!write_indent(&parser, depth) || !parse_string(&parser)) return 0;
            frame->item_count++;
            frame->state = FRAME_OBJECT_COLON;
            continue;
        }
        if (frame->state == FRAME_OBJECT_COLON) {
            if (parser.position >= parser.length ||
                parser.input[parser.position++] != ':') return 0;
            if (!write_byte(&parser, ':') || !write_byte(&parser, ' ') ||
                !begin_value(&parser, stack, &depth, &root_complete)) return 0;
            frame->state = FRAME_OBJECT_VALUE;
            continue;
        }
        if (frame->state == FRAME_OBJECT_VALUE) {
            frame->state = FRAME_OBJECT_DELIMITER;
            continue;
        }
        if (frame->state == FRAME_OBJECT_DELIMITER) {
            if (parser.position < parser.length &&
                parser.input[parser.position] == '}') {
                parser.position++;
                if (frame->item_count != 0 &&
                    (!write_byte(&parser, '\n') ||
                     !write_indent(&parser, depth - 1))) return 0;
                if (!write_byte(&parser, '}')) return 0;
                depth--;
                if (depth == 0) root_complete = 1;
                continue;
            }
            if (parser.position >= parser.length ||
                parser.input[parser.position] != ',') return 0;
            parser.position++;
            if (!write_byte(&parser, ',')) return 0;
            frame->state = FRAME_OBJECT_KEY;
            continue;
        }
        if (frame->state == FRAME_ARRAY_VALUE_OR_END ||
            frame->state == FRAME_ARRAY_VALUE) {
            if (frame->state == FRAME_ARRAY_VALUE_OR_END &&
                parser.position < parser.length &&
                parser.input[parser.position] == ']') {
                parser.position++;
                if (!write_byte(&parser, ']')) return 0;
                depth--;
                if (depth == 0) root_complete = 1;
                continue;
            }
            if (frame->state == FRAME_ARRAY_VALUE) {
                if (parser.position >= parser.length ||
                    parser.input[parser.position++] != ',') return 0;
                if (!write_byte(&parser, ',') || !write_byte(&parser, '\n')) return 0;
            } else if (frame->item_count != 0) {
                return 0;
            } else if (!write_byte(&parser, '\n')) {
                return 0;
            }
            if (!write_indent(&parser, depth)) return 0;
            frame->item_count++;
            frame->state = FRAME_ARRAY_DELIMITER;
            if (!begin_value(&parser, stack, &depth, &root_complete)) return 0;
            continue;
        }
        if (frame->state == FRAME_ARRAY_DELIMITER) {
            if (parser.position < parser.length &&
                parser.input[parser.position] == ']') {
                parser.position++;
                if (frame->item_count != 0 &&
                    (!write_byte(&parser, '\n') ||
                     !write_indent(&parser, depth - 1))) return 0;
                if (!write_byte(&parser, ']')) return 0;
                depth--;
                if (depth == 0) root_complete = 1;
                continue;
            }
            if (parser.position >= parser.length ||
                parser.input[parser.position] != ',') return 0;
            frame->state = FRAME_ARRAY_VALUE;
            continue;
        }
        return 0;
    }
    skip_whitespace(&parser);
    return root_complete && parser.position == parser.length &&
           (output == NULL || fflush(output) == 0);
}

int json_pretty_write(FILE *output, const unsigned char *input, size_t length,
                      size_t maximum_output_bytes)
{
    if (output == NULL || input == NULL || maximum_output_bytes == 0) return 0;
    if (!pretty_run(input, length, NULL, maximum_output_bytes)) return 0;
    return pretty_run(input, length, output, maximum_output_bytes);
}
