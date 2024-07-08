#include <stdio.h>

#include "array.h"

typedef Array(s64)  s64_array;

typedef struct Instruction Instruction;
typedef Array(Instruction) Regex;
typedef struct Thread      Thread; // Russ Cox calls these "threads".
typedef Array(Thread)      Thread_array;

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
        s64          paren_num;  // If opcode == SAVE, the ordinal number of the parenthesis being saved.
    };
};

#define MAX_SUBSTRINGS  8

struct Thread {
    Instruction *instruction;
    char        *parens[2*MAX_SUBSTRINGS]; // [first_char, end_char] pairs.
};

Regex *compile_test_regex(Memory_Context *context)
{
    Regex *regex = NewArray(regex, context);

    *Add(regex) = (Instruction){CHAR, .c = 'x'};
    *Add(regex) = (Instruction){CHAR, .c = 'x'};
    *Add(regex) = (Instruction){CHAR, .c = 'x'};
    *Add(regex) = (Instruction){MATCH};

    return regex;
}

#include <string.h>
#define Swap(A, B)                              \
    do{                                         \
        assert(sizeof(*(A)) == sizeof(*(B)));   \
        u8 tmp[sizeof(*(A))];                   \
        memcpy(tmp, (A), sizeof(*(A)));         \
        memcpy((A), (B), sizeof(*(A)));         \
        memcpy((B), tmp, sizeof(*(A)));         \
    }while(0)

void add_thread(Thread_array *threads, Instruction *instruction, char **parens)
{
    Thread thread = {0};

    thread.instruction = instruction;

    if (parens)  memcpy(thread.parens, parens, sizeof(thread.parens));

    *Add(threads) = thread;
}

bool match_regex(char *string, s64 length, Regex *regex, char_array2 *substrings)
{
    bool is_match = false;
    char **parens = NULL;

    // Create a child memory context for temporary data.
    Memory_Context *tmp_ctx = new_context(regex->context);

    Thread_array cur_threads  = {.context = tmp_ctx};
    Thread_array next_threads = {.context = tmp_ctx};

    add_thread(&cur_threads, &regex->data[0], NULL);

    for (s64 string_index = 0; string_index <= length; string_index++) {
        char c = string[string_index];

        for (s64 thread_index = 0; thread_index < cur_threads.count; thread_index++) {
            Thread  *thread = &cur_threads.data[thread_index];
            Instruction *op = thread->instruction;

            switch (op->opcode) {
                case CHAR:
                    if (c == op->c)  add_thread(&next_threads, op+1, thread->parens);
                    break;
                case JUMP:
                    add_thread(&cur_threads, op->arg, thread->parens);
                    break;
                case SPLIT:
                    add_thread(&cur_threads, op->args[0], thread->parens);
                    add_thread(&cur_threads, op->args[1], thread->parens);
                    break;
                case SAVE:
                    if (op->paren_num >= 2*MAX_SUBSTRINGS)  Error("Parenthesis out of range: %ld.", op->paren_num);
                    thread->parens[op->paren_num] = &string[string_index];
                    add_thread(&cur_threads, op+1, thread->parens);
                    break;
                case MATCH:
                    is_match = true;
                    parens = thread->parens;
                    cur_threads.count = 0;
                    break;
                default:
                    assert(!"Unexpected opcode.");
            }
        }

        cur_threads.count = 0;
        Swap(&cur_threads, &next_threads);
    }

    // Copy substrings out.
    if (substrings && parens) {
        // If the caller indicated that they want substring matches, but they haven't initialised
        // the array themselves, we'll initialise it with the same context as the regex.
        if (!substrings->context)  substrings->context = regex->context;

        while (*parens) {
            assert(*(parens+1));

            char_array *substring = Add(substrings);
            *substring = (char_array){.context = substrings->context};
            while (*parens < *(parens+1)) {//|Speed!
                *Add(substring) = **parens;
                *parens += 1;
            }
            *Add(substring) = '\0';
            substring->count -= 1;

            parens += 2;
        }
    }

    free_context(tmp_ctx);

    return is_match;
}

Regex *compile_regex(char *pattern, Memory_Context *context)
{
    Regex *regex = NewArray(regex, context);

    s64 length = strlen(pattern);
    s64 num_parens = 0;

    for (s64 i = 0; i < length; i++) {
        switch (pattern[i]) {
            case '(':
            case ')':
                if ((num_parens % 2) ^ (pattern[i] == ')')) {
                    Error("Unexpected character in regex at index %ld: '%c'.", i, pattern[i]);
                    return NULL;
                }
                *Add(regex) = (Instruction){SAVE, .paren_num = num_parens};
                num_parens += 1;
                break;
            default:
                *Add(regex) = (Instruction){CHAR, .c = pattern[i]};
        }
    }

    *Add(regex) = (Instruction){MATCH};

    return regex;
}

#include "strings.h" // for get_string

int main()
{
    Memory_Context *ctx = new_context(NULL);

    typedef struct Test Test;
    struct Test {
        Regex       *regex;

        char        *string;
        s64          length;

        bool         expect_match;
        char_array2  expect_substrings;
    };

    Array(Test) *tests = NewArray(tests, ctx);
    {
        Regex *regex = compile_regex("xxx", ctx);
        {
            char string[] = "xxx";
            *Add(tests) = (Test){regex, string, lengthof(string), true};
        }
        {
            char string[] = "xxxy";
            *Add(tests) = (Test){regex, string, lengthof(string), true};
        }
        {
            char string[] = "xxyx";
            *Add(tests) = (Test){regex, string, lengthof(string), false};
        }
    }
    {
        Regex *regex = compile_regex("a(bc)", ctx);
        {
            char string[] = "abcd";
            Test test = {regex, string, lengthof(string), true, {.context = ctx}};
            *Add(&test.expect_substrings) = *get_string(ctx, "bc");
            *Add(tests) = test;
        }
    }

    for (s64 i = 0; i < tests->count; i++) {
        Test *test = &tests->data[i];

        char_array2 substrings = {0};

        bool result = match_regex(test->string, test->length, test->regex, test->expect_substrings.count ? &substrings : NULL);

        if (result != test->expect_match) {
            Log("Test %ld failed. Expected %s. Got %s.", i, test->expect_match ? "match" : "no match", result ? "match" : "no match");
        } else if (substrings.count != test->expect_substrings.count) {
            Log("Test %ld failed. Expected %ld substrings. Got %ld.", i, test->expect_substrings.count, substrings.count);
        } else {
            bool fail = false;
            for (s64 j = 0; j < substrings.count; j++) {
                char_array *expect = &test->expect_substrings.data[j];
                char_array *actual = &substrings.data[j];
                if (strcmp(expect->data, actual->data)) {
                    fail = true;
                    break;
                }
            }
            if (fail) {
                Log("Test %ld failed.", i);
                Log("  %16s%16s", "expect", "actual");
                Log("  %16s%16s", "------", "------");
                for (s64 j = 0; j < substrings.count; j++) {
                    char *expect = test->expect_substrings.data[j].data;
                    char *actual = substrings.data[j].data;
                    Log("  %16s%16s", expect, actual);
                }
            }
        }
    }

    free_context(ctx);

    return 0;
}
