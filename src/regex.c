#include "regex.h"

#include <stdlib.h>
#include <string.h>

#define REGEX_MAX_PARSE_DEPTH 128
#define REGEX_MAX_COMPILE_DEPTH 256
#define REGEX_MAX_TREE_LEVELS 32

typedef enum {
    AST_EMPTY,
    AST_LITERAL,
    AST_DOT,
    AST_CLASS,
    AST_BOL,
    AST_EOL,
    AST_CONCAT,
    AST_ALTERNATE,
    AST_STAR,
    AST_PLUS,
    AST_QUESTION
} AstType;

typedef enum {
    INST_CHAR,
    INST_ANY,
    INST_CLASS,
    INST_SPLIT,
    INST_JUMP,
    INST_BOL,
    INST_EOL,
    INST_MATCH
} InstType;

typedef struct {
    AstType type;
    int left;
    int right;
    int value;
} AstNode;

typedef struct {
    InstType type;
    int out1;
    int out2;
    int value;
} Instruction;

typedef struct {
    int instruction;
    int slot;
    int next;
} Patch;

typedef struct {
    int start;
    int head;
    int tail;
} Fragment;

typedef struct {
    unsigned char bits[32];
} ByteClass;

struct Regex {
    AstNode ast[LOGVIEW_MAX_REGEX_AST_NODES];
    size_t ast_count;
    ByteClass classes[LOGVIEW_MAX_REGEX_STATES];
    size_t class_count;
    Instruction instructions[LOGVIEW_MAX_REGEX_STATES];
    size_t instruction_count;
    Patch patches[LOGVIEW_MAX_REGEX_STATES * 2];
    size_t patch_count;
    int start;
};

typedef struct {
    Regex *regex;
    const unsigned char *pattern;
    size_t length;
    size_t position;
    unsigned depth;
    char *error;
    size_t error_capacity;
    int failed;
} Parser;

typedef struct {
    const Regex *regex;
    int current[LOGVIEW_MAX_REGEX_STATES];
    int next[LOGVIEW_MAX_REGEX_STATES];
    int stack[LOGVIEW_MAX_REGEX_STATES];
    unsigned int seen[LOGVIEW_MAX_REGEX_STATES];
    unsigned int generation;
    uint64_t work;
} VM;

static void parser_error(Parser *parser, const char *message)
{
    if (!parser->failed && parser->error != NULL &&
        parser->error_capacity > 0) {
        size_t length = strlen(message);
        if (length >= parser->error_capacity) {
            length = parser->error_capacity - 1;
        }
        memcpy(parser->error, message, length);
        parser->error[length] = '\0';
    }
    parser->failed = 1;
}

static void copy_error(char *error, size_t capacity, const char *message)
{
    size_t length;
    if (error == NULL || capacity == 0) {
        return;
    }
    length = strlen(message);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(error, message, length);
    error[length] = '\0';
}

static int new_ast(Parser *parser, AstType type, int left, int right, int value)
{
    AstNode *node;
    if (parser->regex->ast_count >= LOGVIEW_MAX_REGEX_AST_NODES) {
        parser_error(parser, "Regular expression is too complex.");
        return -1;
    }
    node = &parser->regex->ast[parser->regex->ast_count];
    node->type = type;
    node->left = left;
    node->right = right;
    node->value = value;
    return (int)parser->regex->ast_count++;
}

static int new_class(Parser *parser)
{
    int index;
    if (parser->regex->class_count >= LOGVIEW_MAX_REGEX_STATES) {
        parser_error(parser, "Regular expression has too many character classes.");
        return -1;
    }
    index = (int)parser->regex->class_count++;
    memset(&parser->regex->classes[index], 0, sizeof(ByteClass));
    return index;
}

static void class_add(ByteClass *byte_class, unsigned char value)
{
    byte_class->bits[value >> 3] |= (unsigned char)(1u << (value & 7u));
}

static int class_has(const ByteClass *byte_class, unsigned char value)
{
    return (byte_class->bits[value >> 3] & (unsigned char)(1u << (value & 7u))) != 0;
}

static int is_digit(unsigned char value)
{
    return value >= '0' && value <= '9';
}

static int is_word(unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           is_digit(value) || value == '_';
}

static void class_add_escape(ByteClass *byte_class, unsigned char code)
{
    unsigned int value;
    if (code == 'd') {
        for (value = '0'; value <= '9'; value++) {
            class_add(byte_class, (unsigned char)value);
        }
    } else if (code == 'w') {
        for (value = 0; value <= 255; value++) {
            if (is_word((unsigned char)value)) {
                class_add(byte_class, (unsigned char)value);
            }
        }
    } else if (code == 's') {
        static const unsigned char whitespace[] = {
            ' ', '\t', '\n', '\r', '\f', '\v'
        };
        size_t index;
        for (index = 0; index < sizeof(whitespace); index++) {
            class_add(byte_class, whitespace[index]);
        }
    } else {
        class_add(byte_class, code);
    }
}

typedef struct {
    int is_class;
    int class_code;
    unsigned char literal;
} ClassItem;

static int parse_class_item(Parser *parser, ClassItem *item)
{
    unsigned char value;
    if (parser->position >= parser->length) {
        parser_error(parser, "Unterminated character class.");
        return 0;
    }
    value = parser->pattern[parser->position++];
    if (value == '\\') {
        if (parser->position >= parser->length) {
            parser_error(parser, "Dangling escape in character class.");
            return 0;
        }
        value = parser->pattern[parser->position++];
        if (value == 'd' || value == 'w' || value == 's') {
            item->is_class = 1;
            item->class_code = value;
            item->literal = 0;
            return 1;
        }
        if (value == 'n') {
            value = '\n';
        } else if (value == 'r') {
            value = '\r';
        } else if (value == 't') {
            value = '\t';
        }
    }
    item->is_class = 0;
    item->class_code = 0;
    item->literal = value;
    return 1;
}

static int parse_character_class(Parser *parser)
{
    int index = new_class(parser);
    int negate = 0;
    int item_count = 0;
    ByteClass *byte_class;
    if (index < 0) {
        return -1;
    }
    byte_class = &parser->regex->classes[index];
    if (parser->position < parser->length &&
        parser->pattern[parser->position] == '^') {
        negate = 1;
        parser->position++;
    }

    while (parser->position < parser->length &&
           parser->pattern[parser->position] != ']') {
        ClassItem first;
        if (!parse_class_item(parser, &first)) {
            return -1;
        }
        item_count++;
        if (!first.is_class && parser->position + 1 < parser->length &&
            parser->pattern[parser->position] == '-' &&
            parser->pattern[parser->position + 1] != ']') {
            ClassItem last;
            unsigned int value;
            parser->position++;
            if (!parse_class_item(parser, &last) || last.is_class) {
                parser_error(parser, "Character ranges require literal endpoints.");
                return -1;
            }
            if (first.literal > last.literal) {
                parser_error(parser, "Character range endpoints are reversed.");
                return -1;
            }
            for (value = first.literal; value <= last.literal; value++) {
                class_add(byte_class, (unsigned char)value);
            }
        } else if (first.is_class) {
            class_add_escape(byte_class, (unsigned char)first.class_code);
        } else {
            class_add(byte_class, first.literal);
        }
    }

    if (parser->position >= parser->length || item_count == 0) {
        parser_error(parser, "Empty or unterminated character class.");
        return -1;
    }
    parser->position++;
    if (negate) {
        size_t byte;
        for (byte = 0; byte < sizeof(byte_class->bits); byte++) {
            byte_class->bits[byte] = (unsigned char)~byte_class->bits[byte];
        }
    }
    return index;
}

static int parse_alternation(Parser *parser);

static int parse_atom(Parser *parser)
{
    unsigned char value;
    if (parser->position >= parser->length) {
        parser_error(parser, "Expected a regular expression atom.");
        return -1;
    }
    value = parser->pattern[parser->position++];
    if (value == '(') {
        int expression;
        if (++parser->depth > REGEX_MAX_PARSE_DEPTH) {
            parser_error(parser, "Regular expression nesting is too deep.");
            parser->depth--;
            return -1;
        }
        expression = parse_alternation(parser);
        parser->depth--;
        if (parser->failed) {
            return -1;
        }
        if (parser->position >= parser->length ||
            parser->pattern[parser->position] != ')') {
            parser_error(parser, "Unmatched opening parenthesis.");
            return -1;
        }
        parser->position++;
        return expression;
    }
    if (value == '[') {
        int class_index = parse_character_class(parser);
        if (class_index < 0) {
            return -1;
        }
        return new_ast(parser, AST_CLASS, -1, -1, class_index);
    }
    if (value == '.') {
        return new_ast(parser, AST_DOT, -1, -1, 0);
    }
    if (value == '^') {
        return new_ast(parser, AST_BOL, -1, -1, 0);
    }
    if (value == '$') {
        return new_ast(parser, AST_EOL, -1, -1, 0);
    }
    if (value == ')' || value == '|' || value == '*' ||
        value == '+' || value == '?') {
        parser_error(parser, "Quantifier or delimiter has no operand.");
        return -1;
    }
    if (value == '\\') {
        if (parser->position >= parser->length) {
            parser_error(parser, "Dangling escape in regular expression.");
            return -1;
        }
        value = parser->pattern[parser->position++];
        if (value == 'd' || value == 'w' || value == 's') {
            int class_index = new_class(parser);
            if (class_index < 0) {
                return -1;
            }
            class_add_escape(&parser->regex->classes[class_index], value);
            return new_ast(parser, AST_CLASS, -1, -1, class_index);
        }
        if (value == 'n') {
            value = '\n';
        } else if (value == 'r') {
            value = '\r';
        } else if (value == 't') {
            value = '\t';
        }
    }
    return new_ast(parser, AST_LITERAL, -1, -1, value);
}

static int parse_repetition(Parser *parser)
{
    int atom = parse_atom(parser);
    int repeated = 0;
    if (atom < 0) {
        return -1;
    }
    while (parser->position < parser->length) {
        unsigned char operation = parser->pattern[parser->position];
        AstType type;
        if (operation == '*') {
            type = AST_STAR;
        } else if (operation == '+') {
            type = AST_PLUS;
        } else if (operation == '?') {
            type = AST_QUESTION;
        } else {
            break;
        }
        if (repeated) {
            parser_error(parser, "Repeated quantifiers are not supported.");
            return -1;
        }
        parser->position++;
        atom = new_ast(parser, type, atom, -1, 0);
        if (atom < 0) {
            return -1;
        }
        repeated = 1;
    }
    return atom;
}

static int parse_concatenation(Parser *parser)
{
    int levels[REGEX_MAX_TREE_LEVELS];
    int result = -1;
    size_t level;
    for (level = 0; level < REGEX_MAX_TREE_LEVELS; level++) {
        levels[level] = -1;
    }
    while (parser->position < parser->length &&
           parser->pattern[parser->position] != ')' &&
           parser->pattern[parser->position] != '|') {
        int next = parse_repetition(parser);
        size_t current_level = 0;
        if (next < 0) {
            return -1;
        }
        while (current_level < REGEX_MAX_TREE_LEVELS &&
               levels[current_level] >= 0) {
            next = new_ast(parser, AST_CONCAT, levels[current_level], next, 0);
            if (next < 0) {
                return -1;
            }
            levels[current_level] = -1;
            current_level++;
        }
        if (current_level == REGEX_MAX_TREE_LEVELS) {
            parser_error(parser, "Regular expression is too large.");
            return -1;
        }
        levels[current_level] = next;
    }
    for (level = REGEX_MAX_TREE_LEVELS; level > 0; level--) {
        if (levels[level - 1] >= 0) {
            if (result < 0) {
                result = levels[level - 1];
            } else {
                result = new_ast(parser, AST_CONCAT, result,
                                 levels[level - 1], 0);
                if (result < 0) {
                    return -1;
                }
            }
        }
    }
    if (result < 0) {
        result = new_ast(parser, AST_EMPTY, -1, -1, 0);
    }
    return result;
}

static int parse_alternation(Parser *parser)
{
    int levels[REGEX_MAX_TREE_LEVELS];
    int result = -1;
    size_t level;
    for (level = 0; level < REGEX_MAX_TREE_LEVELS; level++) {
        levels[level] = -1;
    }
    for (;;) {
        int branch = parse_concatenation(parser);
        size_t current_level = 0;
        if (branch < 0) {
            return -1;
        }
        while (current_level < REGEX_MAX_TREE_LEVELS &&
               levels[current_level] >= 0) {
            branch = new_ast(parser, AST_ALTERNATE,
                             levels[current_level], branch, 0);
            if (branch < 0) {
                return -1;
            }
            levels[current_level] = -1;
            current_level++;
        }
        if (current_level == REGEX_MAX_TREE_LEVELS) {
            parser_error(parser, "Regular expression is too large.");
            return -1;
        }
        levels[current_level] = branch;
        if (parser->position >= parser->length ||
            parser->pattern[parser->position] != '|') {
            break;
        }
        parser->position++;
    }
    for (level = REGEX_MAX_TREE_LEVELS; level > 0; level--) {
        if (levels[level - 1] >= 0) {
            if (result < 0) {
                result = levels[level - 1];
            } else {
                result = new_ast(parser, AST_ALTERNATE, result,
                                 levels[level - 1], 0);
                if (result < 0) {
                    return -1;
                }
            }
        }
    }
    return result;
}

static int emit(Regex *regex, InstType type, int value)
{
    Instruction *instruction;
    if (regex->instruction_count >= LOGVIEW_MAX_REGEX_STATES) {
        return -1;
    }
    instruction = &regex->instructions[regex->instruction_count];
    instruction->type = type;
    instruction->out1 = -1;
    instruction->out2 = -1;
    instruction->value = value;
    return (int)regex->instruction_count++;
}

static int new_patch(Regex *regex, int instruction, int slot)
{
    int index;
    if (regex->patch_count >= LOGVIEW_MAX_REGEX_STATES * 2) {
        return -1;
    }
    index = (int)regex->patch_count++;
    regex->patches[index].instruction = instruction;
    regex->patches[index].slot = slot;
    regex->patches[index].next = -1;
    return index;
}

static Fragment fragment_for_instruction(Regex *regex, int instruction)
{
    Fragment fragment = {-1, -1, -1};
    if (instruction < 0) {
        return fragment;
    }
    fragment.start = instruction;
    fragment.head = new_patch(regex, instruction, 1);
    fragment.tail = fragment.head;
    if (fragment.head < 0) {
        fragment.start = -1;
    }
    return fragment;
}

static Fragment empty_fragment(Regex *regex)
{
    return fragment_for_instruction(regex, emit(regex, INST_JUMP, 0));
}

static int patch_out(Regex *regex, int head, int target)
{
    int current = head;
    while (current >= 0) {
        Patch *patch = &regex->patches[current];
        Instruction *instruction = &regex->instructions[patch->instruction];
        if (patch->slot == 1) {
            instruction->out1 = target;
        } else {
            instruction->out2 = target;
        }
        current = patch->next;
    }
    return 1;
}

static int merge_outs(Regex *regex, Fragment *left, Fragment *right)
{
    if (left->head < 0) {
        left->head = right->head;
        left->tail = right->tail;
    } else if (right->head >= 0) {
        regex->patches[left->tail].next = right->head;
        left->tail = right->tail;
    }
    return left->head >= 0;
}

static Fragment compile_ast(Regex *regex, int node_index, unsigned depth)
{
    Fragment invalid = {-1, -1, -1};
    Fragment left;
    Fragment right;
    int instruction;
    AstNode *node;
    if (node_index < 0 || depth > REGEX_MAX_COMPILE_DEPTH) {
        return invalid;
    }
    node = &regex->ast[node_index];
    switch (node->type) {
    case AST_EMPTY:
        return empty_fragment(regex);
    case AST_LITERAL:
        return fragment_for_instruction(regex, emit(regex, INST_CHAR, node->value));
    case AST_DOT:
        return fragment_for_instruction(regex, emit(regex, INST_ANY, 0));
    case AST_CLASS:
        return fragment_for_instruction(regex, emit(regex, INST_CLASS, node->value));
    case AST_BOL:
        return fragment_for_instruction(regex, emit(regex, INST_BOL, 0));
    case AST_EOL:
        return fragment_for_instruction(regex, emit(regex, INST_EOL, 0));
    case AST_CONCAT:
        left = compile_ast(regex, node->left, depth + 1);
        right = compile_ast(regex, node->right, depth + 1);
        if (left.start < 0 || right.start < 0) {
            return invalid;
        }
        patch_out(regex, left.head, right.start);
        left.head = right.head;
        left.tail = right.tail;
        return left;
    case AST_ALTERNATE:
        left = compile_ast(regex, node->left, depth + 1);
        right = compile_ast(regex, node->right, depth + 1);
        if (left.start < 0 || right.start < 0) {
            return invalid;
        }
        instruction = emit(regex, INST_SPLIT, 0);
        if (instruction < 0) {
            return invalid;
        }
        regex->instructions[instruction].out1 = left.start;
        regex->instructions[instruction].out2 = right.start;
        if (!merge_outs(regex, &left, &right)) {
            return invalid;
        }
        left.start = instruction;
        return left;
    case AST_STAR:
        left = compile_ast(regex, node->left, depth + 1);
        if (left.start < 0) {
            return invalid;
        }
        instruction = emit(regex, INST_SPLIT, 0);
        if (instruction < 0) {
            return invalid;
        }
        regex->instructions[instruction].out1 = left.start;
        patch_out(regex, left.head, instruction);
        left.start = instruction;
        left.head = new_patch(regex, instruction, 2);
        left.tail = left.head;
        if (left.head < 0) {
            return invalid;
        }
        return left;
    case AST_PLUS:
        left = compile_ast(regex, node->left, depth + 1);
        if (left.start < 0) {
            return invalid;
        }
        instruction = emit(regex, INST_SPLIT, 0);
        if (instruction < 0) {
            return invalid;
        }
        regex->instructions[instruction].out1 = left.start;
        patch_out(regex, left.head, instruction);
        left.head = new_patch(regex, instruction, 2);
        left.tail = left.head;
        if (left.head < 0) {
            return invalid;
        }
        return left;
    case AST_QUESTION:
        left = compile_ast(regex, node->left, depth + 1);
        if (left.start < 0) {
            return invalid;
        }
        instruction = emit(regex, INST_SPLIT, 0);
        if (instruction < 0) {
            return invalid;
        }
        regex->instructions[instruction].out1 = left.start;
        {
            int optional_patch = new_patch(regex, instruction, 2);
            Fragment optional_out;
            if (optional_patch < 0) {
                return invalid;
            }
            optional_out.start = -1;
            optional_out.head = optional_patch;
            optional_out.tail = optional_patch;
            if (!merge_outs(regex, &left, &optional_out)) {
                return invalid;
            }
        }
        left.start = instruction;
        return left;
    }
    return invalid;
}

Regex *regex_compile(const char *pattern, char *error, size_t error_capacity)
{
    Regex *regex;
    Parser parser;
    int root;
    Fragment fragment;
    int match_instruction;

    if (error != NULL && error_capacity > 0) {
        error[0] = '\0';
    }
    if (pattern == NULL) {
        copy_error(error, error_capacity, "Regular expression is missing.");
        return NULL;
    }
    regex = (Regex *)calloc(1, sizeof(*regex));
    if (regex == NULL) {
        copy_error(error, error_capacity, "Out of memory.");
        return NULL;
    }
    memset(&parser, 0, sizeof(parser));
    parser.regex = regex;
    parser.pattern = (const unsigned char *)pattern;
    parser.length = strlen(pattern);
    parser.error = error;
    parser.error_capacity = error_capacity;

    root = parse_alternation(&parser);
    if (!parser.failed && parser.position != parser.length) {
        parser_error(&parser, "Unmatched closing parenthesis.");
    }
    if (parser.failed || root < 0) {
        regex_free(regex);
        return NULL;
    }

    fragment = compile_ast(regex, root, 0);
    match_instruction = emit(regex, INST_MATCH, 0);
    if (fragment.start < 0 || match_instruction < 0) {
        copy_error(error, error_capacity, "Compiled regular expression is too large.");
        regex_free(regex);
        return NULL;
    }
    patch_out(regex, fragment.head, match_instruction);
    regex->start = fragment.start;
    return regex;
}

void regex_free(Regex *regex)
{
    free(regex);
}

static void vm_next_generation(VM *vm)
{
    vm->generation++;
    if (vm->generation == 0) {
        memset(vm->seen, 0, sizeof(vm->seen));
        vm->generation = 1;
    }
}

static int vm_push(VM *vm, size_t *stack_count, int instruction)
{
    if (instruction < 0 || (size_t)instruction >= vm->regex->instruction_count) {
        return 1;
    }
    if (vm->seen[instruction] == vm->generation) {
        return 1;
    }
    vm->seen[instruction] = vm->generation;
    if (*stack_count >= LOGVIEW_MAX_REGEX_STATES) {
        return 0;
    }
    vm->stack[(*stack_count)++] = instruction;
    return 1;
}

static int vm_add_state(VM *vm, int *list, size_t *list_count,
                        int instruction_index, size_t position,
                        size_t text_length)
{
    size_t stack_count = 0;
    if (!vm_push(vm, &stack_count, instruction_index)) {
        return 0;
    }
    while (stack_count > 0) {
        const Instruction *instruction;
        int index = vm->stack[--stack_count];
        if (++vm->work > LOGVIEW_MAX_REGEX_WORK) {
            return -1;
        }
        instruction = &vm->regex->instructions[index];
        switch (instruction->type) {
        case INST_SPLIT:
            if (!vm_push(vm, &stack_count, instruction->out2) ||
                !vm_push(vm, &stack_count, instruction->out1)) {
                return 0;
            }
            break;
        case INST_JUMP:
            if (!vm_push(vm, &stack_count, instruction->out1)) {
                return 0;
            }
            break;
        case INST_BOL:
            if (position == 0 && !vm_push(vm, &stack_count, instruction->out1)) {
                return 0;
            }
            break;
        case INST_EOL:
            if (position == text_length &&
                !vm_push(vm, &stack_count, instruction->out1)) {
                return 0;
            }
            break;
        default:
            if (*list_count >= LOGVIEW_MAX_REGEX_STATES) {
                return 0;
            }
            list[(*list_count)++] = index;
            break;
        }
    }
    return 1;
}

RegexResult regex_search(const Regex *regex, const unsigned char *text,
                         size_t text_length)
{
    VM vm;
    int *current = vm.current;
    int *next = vm.next;
    size_t current_count = 0;
    size_t position;

    if (regex == NULL || (text == NULL && text_length != 0)) {
        return REGEX_NO_MATCH;
    }
    memset(&vm, 0, sizeof(vm));
    vm.regex = regex;
    vm_next_generation(&vm);

    for (position = 0; position <= text_length; position++) {
        size_t state_index;
        int added = vm_add_state(&vm, current, &current_count, regex->start,
                                 position, text_length);
        if (added < 0) {
            return REGEX_WORK_LIMIT;
        }
        if (added == 0) {
            return REGEX_WORK_LIMIT;
        }
        for (state_index = 0; state_index < current_count; state_index++) {
            if (regex->instructions[current[state_index]].type == INST_MATCH) {
                return REGEX_MATCH;
            }
        }
        if (position == text_length) {
            break;
        }

        vm_next_generation(&vm);
        {
            size_t next_count = 0;
            size_t index;
            unsigned char byte = text[position];
            for (index = 0; index < current_count; index++) {
                const Instruction *instruction =
                    &regex->instructions[current[index]];
                int matched = 0;
                if (++vm.work > LOGVIEW_MAX_REGEX_WORK) {
                    return REGEX_WORK_LIMIT;
                }
                if (instruction->type == INST_CHAR) {
                    matched = byte == (unsigned char)instruction->value;
                } else if (instruction->type == INST_ANY) {
                    matched = byte != '\n' && byte != '\r';
                } else if (instruction->type == INST_CLASS) {
                    matched = class_has(&regex->classes[instruction->value], byte);
                }
                if (matched) {
                    added = vm_add_state(&vm, next, &next_count, instruction->out1,
                                         position + 1, text_length);
                    if (added < 0) {
                        return REGEX_WORK_LIMIT;
                    }
                    if (added == 0) {
                        return REGEX_WORK_LIMIT;
                    }
                }
            }
            {
                int *swap = current;
                current = next;
                next = swap;
                current_count = next_count;
            }
        }
    }
    return REGEX_NO_MATCH;
}
