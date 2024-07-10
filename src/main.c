#include <stdio.h>
#include <string.h>

#include "array.h"

#define Swap(A, B)                              \
    do {                                        \
        assert(sizeof(*(A)) == sizeof(*(B)));   \
        u8 tmp[sizeof(*(A))];                   \
        memcpy(tmp, (A), sizeof(*(A)));         \
        memcpy((A), (B), sizeof(*(A)));         \
        memcpy((B), tmp, sizeof(*(A)));         \
    } while (0)

typedef struct Instruction Instruction;
typedef Array(Instruction) Regex;
typedef Array(s64)         s64_array;

enum Opcode {
    CHAR = 1,
    JUMP,
    SPLIT,
    SAVE,
    MATCH,
};

struct Instruction {
    enum Opcode      opcode;
    union {
        char         c;          // If opcode == CHAR, the character to match.
        Instruction *arg;        // If opcode == JUMP, the instruction to execute next.
        Instruction *args[2];    // If opcode == SPLIT, the two instructions to execute next.
    };
};

bool match_regex(char *string, s64 string_length, Regex *regex, s64_array *capture_offsets)
{
    typedef struct Capture  Capture;
    typedef struct Thread   Thread; // Russ Cox calls these "threads", in the sense that the compiled regex executes in a virtual machine, which has multiple parallel instructions being executed for each letter in the string being tested. Where the metaphor fails, though, is that the order of execution of the regex threads is deterministic---and must be for greedy matching. If we think of a better word, we'll use that. E.g. Try?

    // Inside this function, we store captures as a singly-linked list, but convert them to a s64_array before returning.
    struct Capture {
        Capture *prev;
        s64      offset; // The index of the character in the source string we were up to when we encountered a parenthesis.
    };

    struct Thread {
        Instruction *instruction;
        Capture     *captures;
    };

    bool     is_match = false;
    Capture *captures = NULL;

    // Create a child memory context for temporary data.
    Memory_Context *tmp_ctx = new_context(regex->context);

    Array(Thread) cur_threads  = {.context = tmp_ctx};
    Array(Thread) next_threads = {.context = tmp_ctx};

    *Add(&cur_threads) = (Thread){&regex->data[0]};

    for (s64 string_index = 0; string_index <= string_length; string_index++) {
        char c = string[string_index];

        for (s64 thread_index = 0; thread_index < cur_threads.count; thread_index++) {
            Thread  *thread = &cur_threads.data[thread_index];
            Instruction *op = thread->instruction;

            switch (op->opcode) {
                case CHAR:
                    if (c == op->c)  *Add(&next_threads) = (Thread){op+1, thread->captures};
                    break;
                case JUMP:
                    *Add(&cur_threads) = (Thread){op->arg, thread->captures};
                    break;
                case SPLIT:
                    *Add(&cur_threads) = (Thread){op->args[0], thread->captures};
                    *Add(&cur_threads) = (Thread){op->args[1], thread->captures};
                    break;
                case SAVE:;
                    Capture *capture = New(Capture, tmp_ctx);
                    capture->prev    = thread->captures;
                    capture->offset  = string_index;
                    *Add(&cur_threads) = (Thread){op+1, capture};
                    break;
                case MATCH:
                    is_match = true;
                    captures = thread->captures;
                    cur_threads.count = 0;
                    break;
                default:
                    assert(!"Unexpected opcode.");
            }
        }

        cur_threads.count = 0;
        Swap(&cur_threads, &next_threads);
    }

    if (captures && capture_offsets) {
        // If the caller didn't initialise capture_offsets with a context, we'll use the regex context.
        if (!capture_offsets->context)  capture_offsets->context = regex->context;

        for (Capture *c = captures; c != NULL; c = c->prev)  *Add(capture_offsets) = c->offset;

        reverse_array(capture_offsets);
    }

    free_context(tmp_ctx);

    return is_match;
}

static Regex *parse_error(char *pattern, s64 index)
{
    Log("Unexpected character in regex pattern at index %ld: '%c'.", index, pattern[index]);
    return NULL;
}

Regex *compile_regex(char *pattern, Memory_Context *context)
{
    Regex *regex = NewArray(regex, context);

    s64 length = strlen(pattern);
    s64 num_parens = 0;

    for (s64 pattern_index = 0; pattern_index < length; pattern_index++) {
        char c = pattern[pattern_index];

        switch (c) {
            case '(':
            case ')':
                if ((num_parens % 2) ^ (c == ')'))  return parse_error(pattern, pattern_index);

                *Add(regex) = (Instruction){SAVE};
                num_parens += 1;
                break;
            case '*':
                if (!regex->count)  return parse_error(pattern, pattern_index);

                Instruction popped = regex->data[regex->count-1];
                regex->count -= 1;
                Instruction *ops[3];
                for (s64 i = 0; i < 3; i++)  ops[i] = Add(regex);
                *ops[0] = (Instruction){SPLIT, .args = {ops[1], ops[2]+1}};
                *ops[1] = popped;
                *ops[2] = (Instruction){JUMP, .arg = ops[0]};
                break;
            default:
                *Add(regex) = (Instruction){CHAR, .c = c};
        }
    }

    *Add(regex) = (Instruction){MATCH};

    return regex;
}

#include "strings.h"

//|Todo: If we keep the below, it should maybe print to a supplied char_array.
char_array *extract_string(char *data, s64 first_char_offset, s64 last_char_offset, Memory_Context *context)
{
    char_array *result = NewArray(result, context);

    for (s64 i = first_char_offset; i < last_char_offset; i++)  *Add(result) = data[i];

    *Add(result) = '\0';
    result->count -= 1;

    return result;
}

int main()
{
    Memory_Context *top_context = new_context(NULL);
    Memory_Context *regex_ctx   = new_context(top_context);

    char_array *file = load_text_file("regex_tests.txt", top_context);
    assert(file);

    Regex *regex = NULL;
    char_array *regex_source = NULL;

    s64 line_number = 0;
    char *c = file->data;

    while (*c) {
        // We should be at the start of the file or at the end of a line.
        if (c != file->data) {
            assert(*c == '\n');
            c += 1; // If we were at the end of a line advance one character.
        }
        if (*c == '\0')  break;

        // Now we're at the start of a line.
        line_number += 1;

        // Skip comments.
        if (*c == '#') {
            do c++;  while (*c && *c != '\n');
            continue;
        }

        // Two newline characters mean there's a new regex coming.
        if (*c == '\n') {
            reset_context(regex_ctx);
            regex_source = NULL;
            continue;
        }

        // Consume a line.
        char_array *line; {
            s64 first_char_offset = c - file->data;
            do c++;  while (*c && *c != '\n');
            s64 last_char_offset  = c - file->data;

            line = extract_string(file->data, first_char_offset, last_char_offset, regex_ctx);
        }

        if (!regex_source) {
            if (line->data[0] != '/' || line->data[line->count-1] != '/') {
                Fatal("Expected regex on line %ld to be sandwiched by '/' chars.", line_number);
            }
            line->data  += 1;
            line->count -= 2;
            line->data[line->count] = '\0';

            regex_source = line;
            regex = compile_regex(regex_source->data, regex_ctx);
            //|Todo: Do something if compilation failed.

            continue;
        }

        // If we get here, we have a regex and a string to match.

        s64_array offsets = {.context = regex_ctx};

        bool result = match_regex(line->data, line->count, regex, &offsets);

        assert(offsets.count % 2 == 0 || !"There should be an even number of offsets.");

        char_array out = {.context = regex_ctx};

        print_string(&out, "Regex:  %s\n", regex_source->data);
        print_string(&out, "String: %s\n", line->data);
        print_string(&out, "Match:  %s\n", result ? "yes" : "no");
        for (s64 i = 0; i < offsets.count; i += 2) {
            s64 start = offsets.data[i];
            s64 end   = offsets.data[i+1];
            char_array *substring = extract_string(line->data, start, end, regex_ctx);

            print_string(&out, "  %s\n", substring->data);
        }

        Log(out.data);
    }

    free_context(top_context);

    return 0;
}
