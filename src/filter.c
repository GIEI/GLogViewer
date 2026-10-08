#include "filter.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILTER_MAX_NODES 256
#define FILTER_TEXT_CAPACITY 256
#define FILTER_VALUE_CAPACITY 1024

typedef enum { NODE_PREDICATE, NODE_AND, NODE_OR } NodeKind;
typedef enum { OP_EQUAL, OP_NOT_EQUAL, OP_REGEX } PredicateOp;

typedef struct {
    NodeKind kind;
    int left;
    int right;
    PredicateOp op;
    char field[FILTER_TEXT_CAPACITY];
    char value[FILTER_VALUE_CAPACITY];
    Regex *regex;
} FilterNode;

struct Filter {
    FilterNode nodes[FILTER_MAX_NODES];
    size_t count;
    int root;
};

typedef struct {
    const char *text;
    size_t position;
    Filter *filter;
    char *error;
    size_t error_capacity;
} FilterParser;

static void skip_space(FilterParser *parser)
{
    while (isspace((unsigned char)parser->text[parser->position])) parser->position++;
}

static int word_at(FilterParser *parser, const char *word)
{
    size_t length = strlen(word);
    size_t position = parser->position;
    if (strlen(parser->text + position) < length) return 0;
    if (strncmp(parser->text + position, word, length) != 0) return 0;
    if (position != 0 &&
        (isalnum((unsigned char)parser->text[position - 1]) || parser->text[position - 1] == '_')) return 0;
    if (isalnum((unsigned char)parser->text[position + length]) ||
        parser->text[position + length] == '_') return 0;
    parser->position += length;
    return 1;
}

static int new_node(FilterParser *parser, NodeKind kind)
{
    int index;
    if (parser->filter->count >= FILTER_MAX_NODES) {
        snprintf(parser->error, parser->error_capacity, "filter expression has too many terms");
        return -1;
    }
    index = (int)parser->filter->count++;
    memset(&parser->filter->nodes[index], 0, sizeof(parser->filter->nodes[index]));
    parser->filter->nodes[index].kind = kind;
    parser->filter->nodes[index].left = -1;
    parser->filter->nodes[index].right = -1;
    return index;
}

static int parse_or(FilterParser *parser);

static int parse_value(FilterParser *parser, char *value, size_t capacity)
{
    size_t length = 0;
    int quoted;
    skip_space(parser);
    quoted = parser->text[parser->position] == '"';
    if (quoted) parser->position++;
    while (parser->text[parser->position] != '\0') {
        char byte = parser->text[parser->position];
        if (quoted ? byte == '"' : (isspace((unsigned char)byte) || byte == ')')) break;
        if (byte == '\\' && quoted && parser->text[parser->position + 1] != '\0') {
            parser->position++;
            byte = parser->text[parser->position];
        }
        if (length + 1 >= capacity) {
            snprintf(parser->error, parser->error_capacity, "filter value exceeds the supported length");
            return 0;
        }
        value[length++] = byte;
        parser->position++;
    }
    if (quoted) {
        if (parser->text[parser->position] != '"') {
            snprintf(parser->error, parser->error_capacity, "unterminated quoted filter value");
            return 0;
        }
        parser->position++;
    }
    value[length] = '\0';
    if (length == 0) {
        snprintf(parser->error, parser->error_capacity, "filter value cannot be empty");
        return 0;
    }
    return 1;
}

static int parse_predicate(FilterParser *parser)
{
    size_t start;
    size_t length;
    int node_index;
    FilterNode *node;
    skip_space(parser);
    start = parser->position;
    while (isalnum((unsigned char)parser->text[parser->position]) ||
           parser->text[parser->position] == '_') parser->position++;
    length = parser->position - start;
    if (length == 0 || length >= FILTER_TEXT_CAPACITY) {
        snprintf(parser->error, parser->error_capacity, "expected a field name");
        return -1;
    }
    node_index = new_node(parser, NODE_PREDICATE);
    if (node_index < 0) return -1;
    node = &parser->filter->nodes[node_index];
    memcpy(node->field, parser->text + start, length);
    node->field[length] = '\0';
    skip_space(parser);
    if (parser->text[parser->position] == '=' && parser->text[parser->position + 1] != '=') {
        node->op = OP_EQUAL;
        parser->position++;
    } else if (parser->text[parser->position] == '!' && parser->text[parser->position + 1] == '=') {
        node->op = OP_NOT_EQUAL;
        parser->position += 2;
    } else if (parser->text[parser->position] == '~') {
        node->op = OP_REGEX;
        parser->position++;
    } else {
        snprintf(parser->error, parser->error_capacity, "expected '=', '!=', or '~' after field");
        return -1;
    }
    if (!parse_value(parser, node->value, sizeof(node->value))) return -1;
    if (node->op == OP_REGEX) {
        node->regex = regex_compile(node->value, parser->error, parser->error_capacity);
        if (node->regex == NULL) return -1;
    }
    return node_index;
}

static int parse_primary(FilterParser *parser)
{
    int result;
    skip_space(parser);
    if (parser->text[parser->position] == '(') {
        parser->position++;
        result = parse_or(parser);
        skip_space(parser);
        if (result < 0 || parser->text[parser->position] != ')') {
            snprintf(parser->error, parser->error_capacity, "expected ')' in filter expression");
            return -1;
        }
        parser->position++;
        return result;
    }
    return parse_predicate(parser);
}

static int parse_and(FilterParser *parser)
{
    int left = parse_primary(parser);
    if (left < 0) return -1;
    for (;;) {
        int right;
        int parent;
        skip_space(parser);
        if (!word_at(parser, "and")) break;
        right = parse_primary(parser);
        if (right < 0) return -1;
        parent = new_node(parser, NODE_AND);
        if (parent < 0) return -1;
        parser->filter->nodes[parent].left = left;
        parser->filter->nodes[parent].right = right;
        left = parent;
    }
    return left;
}

static int parse_or(FilterParser *parser)
{
    int left = parse_and(parser);
    if (left < 0) return -1;
    for (;;) {
        int right;
        int parent;
        skip_space(parser);
        if (!word_at(parser, "or")) break;
        right = parse_and(parser);
        if (right < 0) return -1;
        parent = new_node(parser, NODE_OR);
        if (parent < 0) return -1;
        parser->filter->nodes[parent].left = left;
        parser->filter->nodes[parent].right = right;
        left = parent;
    }
    return left;
}

Filter *filter_compile(const char *expression, char *error,
                       size_t error_capacity)
{
    FilterParser parser;
    Filter *filter;
    if (expression == NULL || *expression == '\0') return NULL;
    filter = (Filter *)calloc(1, sizeof(*filter));
    if (filter == NULL) {
        snprintf(error, error_capacity, "out of memory while compiling filter");
        return NULL;
    }
    parser.text = expression;
    parser.position = 0;
    parser.filter = filter;
    parser.error = error;
    parser.error_capacity = error_capacity;
    filter->root = parse_or(&parser);
    skip_space(&parser);
    if (filter->root < 0 || parser.text[parser.position] != '\0') {
        if (error[0] == '\0') snprintf(error, error_capacity, "unexpected token in filter expression");
        filter_free(filter);
        return NULL;
    }
    return filter;
}

static int span_equals(const ByteSpan span, const char *text)
{
    size_t length = strlen(text);
    return span.length == length && memcmp(span.data, text, length) == 0;
}

static int evaluate(const Filter *filter, int index, const LogEntry *entry,
                    RegexResult *regex_result)
{
    const FilterNode *node = &filter->nodes[index];
    ByteSpan value;
    int found;
    if (node->kind == NODE_AND)
        return evaluate(filter, node->left, entry, regex_result) &&
               evaluate(filter, node->right, entry, regex_result);
    if (node->kind == NODE_OR)
        return evaluate(filter, node->left, entry, regex_result) ||
               evaluate(filter, node->right, entry, regex_result);
    found = log_entry_find_field(entry, node->field, &value);
    if (!found) return 0;
    if (node->op == OP_EQUAL) return span_equals(value, node->value);
    if (node->op == OP_NOT_EQUAL) return !span_equals(value, node->value);
    *regex_result = regex_search(node->regex, value.data, value.length);
    return *regex_result == REGEX_MATCH;
}

int filter_matches(const Filter *filter, const LogEntry *entry,
                   RegexResult *regex_result)
{
    if (filter == NULL) return 1;
    *regex_result = REGEX_NO_MATCH;
    return evaluate(filter, filter->root, entry, regex_result);
}

void filter_free(Filter *filter)
{
    size_t index;
    if (filter == NULL) return;
    for (index = 0; index < filter->count; index++) regex_free(filter->nodes[index].regex);
    free(filter);
}
