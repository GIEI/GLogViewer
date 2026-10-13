#include "search_query.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SEARCH_QUERY_MAX_TERMS 64u
#define SEARCH_QUERY_MAX_NODES 127u
#define SEARCH_QUERY_MAX_DEPTH 64u

typedef enum {
    SEARCH_NODE_TERM,
    SEARCH_NODE_NOT,
    SEARCH_NODE_AND,
    SEARCH_NODE_OR
} SearchNodeType;

typedef struct {
    SearchNodeType type;
    int left;
    int right;
    size_t term_index;
} SearchNode;

struct SearchQuery {
    Regex *regex;
    SearchNode nodes[SEARCH_QUERY_MAX_NODES];
    size_t node_count;
    unsigned char *terms[SEARCH_QUERY_MAX_TERMS];
    size_t term_lengths[SEARCH_QUERY_MAX_TERMS];
    size_t term_count;
    int root;
};

typedef struct {
    SearchQuery *query;
    const char *text;
    size_t length;
    size_t position;
    size_t depth;
    char *error;
    size_t error_capacity;
    int failed;
} SearchParser;

static void set_error(SearchParser *parser, const char *message)
{
    size_t length;
    if (parser->failed) return;
    parser->failed = 1;
    if (parser->error == NULL || parser->error_capacity == 0) return;
    length = strlen(message);
    if (length >= parser->error_capacity) length = parser->error_capacity - 1;
    memcpy(parser->error, message, length);
    parser->error[length] = '\0';
}

static void skip_space(SearchParser *parser)
{
    while (parser->position < parser->length &&
           (parser->text[parser->position] == ' ' ||
            parser->text[parser->position] == '\t')) {
        parser->position++;
    }
}

static int is_word_character(unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

static int keyword_at(const SearchParser *parser, const char *keyword)
{
    size_t length = strlen(keyword);
    size_t index;
    if (parser->position + length > parser->length ||
        (parser->position != 0 &&
         is_word_character((unsigned char)parser->text[parser->position - 1]))) {
        return 0;
    }
    for (index = 0; index < length; index++) {
        if (toupper((unsigned char)parser->text[parser->position + index]) !=
            keyword[index]) return 0;
    }
    return parser->position + length == parser->length ||
           !is_word_character((unsigned char)parser->text[parser->position + length]);
}

static int consume_keyword(SearchParser *parser, const char *keyword)
{
    skip_space(parser);
    if (!keyword_at(parser, keyword)) return 0;
    parser->position += strlen(keyword);
    return 1;
}

static int contains_logical_operator(const char *text)
{
    size_t position = 0;
    size_t length = strlen(text);
    int in_quote = 0;
    while (position < length) {
        unsigned char value = (unsigned char)text[position];
        if (in_quote) {
            if (value == '\\' && position + 1 < length &&
                (text[position + 1] == '\\' || text[position + 1] == '"')) {
                position += 2;
                continue;
            }
            if (value == '"') in_quote = 0;
            position++;
            continue;
        }
        if (value == '"') {
            in_quote = 1;
            position++;
            continue;
        }
        if (is_word_character(value) &&
            (position == 0 ||
             !is_word_character((unsigned char)text[position - 1]))) {
            size_t start = position;
            size_t token_length;
            while (position < length &&
                   is_word_character((unsigned char)text[position])) position++;
            token_length = position - start;
            if ((token_length == 3 &&
                 ((toupper((unsigned char)text[start]) == 'A' &&
                   toupper((unsigned char)text[start + 1]) == 'N' &&
                   toupper((unsigned char)text[start + 2]) == 'D') ||
                  (toupper((unsigned char)text[start]) == 'N' &&
                   toupper((unsigned char)text[start + 1]) == 'O' &&
                   toupper((unsigned char)text[start + 2]) == 'T'))) ||
                (token_length == 2 &&
                 toupper((unsigned char)text[start]) == 'O' &&
                 toupper((unsigned char)text[start + 1]) == 'R')) return 1;
            continue;
        }
        position++;
    }
    return 0;
}

static int add_node(SearchParser *parser, SearchNodeType type,
                    int left, int right, size_t term_index)
{
    SearchNode *node;
    if (parser->query->node_count >= SEARCH_QUERY_MAX_NODES) {
        set_error(parser, "Search expression is too complex.");
        return -1;
    }
    node = &parser->query->nodes[parser->query->node_count];
    node->type = type;
    node->left = left;
    node->right = right;
    node->term_index = term_index;
    return (int)parser->query->node_count++;
}

static int parse_or(SearchParser *parser);

static int parse_term(SearchParser *parser)
{
    unsigned char *term;
    size_t capacity;
    size_t term_length = 0;
    size_t term_index;
    int closed = 0;
    if (parser->position >= parser->length ||
        parser->text[parser->position] != '"') {
        set_error(parser, "Expected a quoted literal search term.");
        return -1;
    }
    parser->position++;
    capacity = parser->length - parser->position + 1;
    term = (unsigned char *)malloc(capacity);
    if (term == NULL) {
        set_error(parser, "Out of memory while parsing search expression.");
        return -1;
    }
    while (parser->position < parser->length) {
        unsigned char value = (unsigned char)parser->text[parser->position++];
        if (value == '"') {
            closed = 1;
            break;
        }
        if (value == '\\' && parser->position < parser->length &&
            (parser->text[parser->position] == '"' ||
             parser->text[parser->position] == '\\')) {
            value = (unsigned char)parser->text[parser->position++];
        }
        term[term_length++] = value;
    }
    if (!closed || term_length == 0) {
        free(term);
        set_error(parser, closed ? "Quoted search terms cannot be empty." :
                                  "Unterminated quoted search term.");
        return -1;
    }
    if (parser->query->term_count >= SEARCH_QUERY_MAX_TERMS) {
        free(term);
        set_error(parser, "Search expression has too many terms.");
        return -1;
    }
    term_index = parser->query->term_count++;
    parser->query->terms[term_index] = term;
    parser->query->term_lengths[term_index] = term_length;
    return add_node(parser, SEARCH_NODE_TERM, -1, -1, term_index);
}

static int parse_primary(SearchParser *parser)
{
    int expression;
    skip_space(parser);
    if (parser->position >= parser->length) {
        set_error(parser, "Expected a quoted term or parenthesized expression.");
        return -1;
    }
    if (parser->text[parser->position] == '"') return parse_term(parser);
    if (parser->text[parser->position] != '(') {
        set_error(parser, "Expected a quoted term or parenthesized expression.");
        return -1;
    }
    parser->position++;
    if (++parser->depth > SEARCH_QUERY_MAX_DEPTH) {
        parser->depth--;
        set_error(parser, "Search expression nesting is too deep.");
        return -1;
    }
    expression = parse_or(parser);
    parser->depth--;
    if (parser->failed) return -1;
    skip_space(parser);
    if (parser->position >= parser->length ||
        parser->text[parser->position] != ')') {
        set_error(parser, "Unmatched opening parenthesis in search expression.");
        return -1;
    }
    parser->position++;
    return expression;
}

static int parse_unary(SearchParser *parser)
{
    int operand;
    skip_space(parser);
    if (consume_keyword(parser, "NOT")) {
        operand = parse_unary(parser);
        if (operand < 0) return -1;
        return add_node(parser, SEARCH_NODE_NOT, operand, -1, 0);
    }
    return parse_primary(parser);
}

static int parse_and(SearchParser *parser)
{
    int left = parse_unary(parser);
    if (left < 0) return -1;
    while (!parser->failed && consume_keyword(parser, "AND")) {
        int right = parse_unary(parser);
        if (right < 0) return -1;
        left = add_node(parser, SEARCH_NODE_AND, left, right, 0);
        if (left < 0) return -1;
    }
    return left;
}

static int parse_or(SearchParser *parser)
{
    int left = parse_and(parser);
    if (left < 0) return -1;
    while (!parser->failed && consume_keyword(parser, "OR")) {
        int right = parse_and(parser);
        if (right < 0) return -1;
        left = add_node(parser, SEARCH_NODE_OR, left, right, 0);
        if (left < 0) return -1;
    }
    return left;
}

SearchQuery *search_query_compile(const char *text, char *error,
                                 size_t error_capacity)
{
    SearchQuery *query;
    SearchParser parser;
    if (error != NULL && error_capacity > 0) error[0] = '\0';
    if (text == NULL) {
        if (error != NULL && error_capacity > 0) {
            strncpy(error, "Search query is missing.", error_capacity - 1);
            error[error_capacity - 1] = '\0';
        }
        return NULL;
    }
    query = (SearchQuery *)calloc(1, sizeof(*query));
    if (query == NULL) {
        if (error != NULL && error_capacity > 0) {
            strncpy(error, "Out of memory.", error_capacity - 1);
            error[error_capacity - 1] = '\0';
        }
        return NULL;
    }
    query->root = -1;
    if (!contains_logical_operator(text)) {
        query->regex = regex_compile(text, error, error_capacity);
        if (query->regex == NULL) {
            search_query_free(query);
            return NULL;
        }
        return query;
    }
    memset(&parser, 0, sizeof(parser));
    parser.query = query;
    parser.text = text;
    parser.length = strlen(text);
    parser.error = error;
    parser.error_capacity = error_capacity;
    query->root = parse_or(&parser);
    skip_space(&parser);
    if (parser.failed || query->root < 0 || parser.position != parser.length) {
        if (!parser.failed) set_error(&parser, "Unexpected token in search expression.");
        search_query_free(query);
        return NULL;
    }
    return query;
}

void search_query_free(SearchQuery *query)
{
    size_t index;
    if (query == NULL) return;
    regex_free(query->regex);
    for (index = 0; index < query->term_count; index++) free(query->terms[index]);
    free(query);
}

static int literal_match(const unsigned char *haystack, size_t haystack_length,
                         const unsigned char *needle, size_t needle_length)
{
    const unsigned char *candidate = haystack;
    size_t remaining = haystack_length;
    if (needle_length > haystack_length) return 0;
    while (remaining >= needle_length) {
        const unsigned char *found = (const unsigned char *)memchr(
            candidate, needle[0], remaining - needle_length + 1);
        if (found == NULL) return 0;
        if (memcmp(found, needle, needle_length) == 0) return 1;
        remaining -= (size_t)(found - candidate) + 1;
        candidate = found + 1;
    }
    return 0;
}

static int evaluate_node(const SearchQuery *query, int node_index,
                         const unsigned char *text, size_t text_length)
{
    const SearchNode *node = &query->nodes[node_index];
    switch (node->type) {
    case SEARCH_NODE_TERM:
        return literal_match(text, text_length,
                             query->terms[node->term_index],
                             query->term_lengths[node->term_index]);
    case SEARCH_NODE_NOT:
        return !evaluate_node(query, node->left, text, text_length);
    case SEARCH_NODE_AND:
        return evaluate_node(query, node->left, text, text_length) &&
               evaluate_node(query, node->right, text, text_length);
    case SEARCH_NODE_OR:
        return evaluate_node(query, node->left, text, text_length) ||
               evaluate_node(query, node->right, text, text_length);
    }
    return 0;
}

RegexResult search_query_match(const SearchQuery *query,
                               const unsigned char *text, size_t text_length)
{
    if (query == NULL || (text == NULL && text_length != 0)) return REGEX_NO_MATCH;
    if (query->regex != NULL) return regex_search(query->regex, text, text_length);
    return evaluate_node(query, query->root, text, text_length) ?
           REGEX_MATCH : REGEX_NO_MATCH;
}
