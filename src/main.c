//|Todo:
//| Return captures as a Map somehow?
//| Named capture groups.
//| Fix up tests.
//| ?? - non-greedy ? modifier.
//| Remove anchors.
//| Factor into module.
//| Plug into web server!

//|Speed:
//| Merge adjacent chars into strings.
//| Convert NFAs to DFAs.

#include <stdio.h>
#include <string.h>

#include "strings.h"
#include "map.h"

#define Swap(A, B)                              \
    do {                                        \
        assert(sizeof(*(A)) == sizeof(*(B)));   \
        u8 tmp[sizeof(*(A))];                   \
        memcpy(tmp, (A), sizeof(*(A)));         \
        memcpy((A), (B), sizeof(*(A)));         \
        memcpy((B), tmp, sizeof(*(A)));         \
    } while (0)

typedef Array(s64)         s64_array;

typedef struct Instruction Instruction;
typedef Array(Instruction) Regex;

enum Opcode {
    CHAR = 1,
    ASCII_CLASS,
    ANY,
    JUMP,
    SPLIT,
    SAVE,
    MATCH,
    ANCHOR_START,
    ANCHOR_END,
};

//
// stick with ASCII, son
// none of that UTF-8
// or we'll all be runed
//
// - Junyer (https://jeune.us/haiku-slam.html#1410830961)
//

struct Instruction {
    enum Opcode            opcode;
    union {
        // If opcode == CHAR, the character to match.
        char               c;

        // If opcode == SPLIT, pointers to the two instructions to execute next.
        // If opcode == JUMP, the same, but we only use the first one.
        Instruction       *next[2];

        // For use by the regex compiler only. Until a regex is fully compiled, we can't put in absolute pointers to instructions,
        // because the whole array might move. So instead we make a note of the index of the instruction relative to the current
        // instruction's index, and replace them with absolute pointers just before returning.
        s64                rel_next[2];

        // If opcode == ASCII_CLASS, a bitfield describing the characters in the class.
        // There is one bit for each byte in the range 0--127 (i.e. ASCII characters).
        u8                 class[128/8];

        // If opcode == SAVE:
        struct {
            // A number identifying the capture group. Even numbers are the starts of captures, odds the ends.
            // For example,
            //             /...(..)..(...(..)..).../     <-- In this regular expression,
            //                 0  1  2   4  5  3         <-- these are the save_ids.
            s64            save_id;
            // The name of the capture group, if it is named. The name only appears with the starts of captures, not the ends.
            char          *save_name;
        };
    };
};

static Regex *parse_error(char *pattern, s64 index)
{
    Log("Unexpected character in regex pattern at index %ld: '%c'.", index, pattern[index]);
    return NULL;
}

static void negate_ascii_class(Instruction *inst)
{
    assert(inst->opcode == ASCII_CLASS);
    for (int i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
}

Regex *compile_regex(char *pattern, Memory_Context *context)
{
    Regex *regex = NewArray(regex, context);

    //
    // Other regex compilers usually parse patterns into intermediate representations before compiling. We validate, parse and compile in a
    // single loop. For this to work, we sometimes need to shift previously-added instructions to place something before them. For example,
    // when compiling `.?`, we place an ANY instruction for the dot, and then, when we see the question mark, we shift the ANY right to make
    // room for a SPLIT that creates two branches: one that executes the ANY and another that goes straight to the next instruction.
    //
    // Most of these past-modifying characters affect either the previous token---like `.?`---or, if the previous character was a right-
    // parenthesis closing a capture group, then the whole capture group---like `(...)?`. The exception is `|`, which requires that we
    // put a SPLIT before all previous instructions going back to the start of the current capture group.
    //
    // Here's how we do it. We keep a stack of indexes into the regex->data array that we're building. On the top of the stack is the index
    // of the last token we placed---that is, the instruction to shift if we get a modifier like `?`. Just under this on the stack is the
    // index of the start of the current capture group. When we enter a new capture group, we push to the stack, and pop when we leave the
    // capture group. Thus, if we just left a capture group, `?` automatically modifies that whole group instead of a single token.
    //
    // shift_index points to the top of the stack, so *shift_index is the index of the first instruction to shift if we encounter a modifier
    // like `?` or `*`, whereas *(shift_index-1) is the index of the first instruction to shift if we encounter a `|`.
    //
    #define MAX_NESTED_CAPTURE_GROUPS 10
    s64 shift_stack[MAX_NESTED_CAPTURE_GROUPS+1] = {0};
    s64 *shift_index = &shift_stack[1];
    *shift_index = -1; // -1 means there is not currently an instruction that can be validly quantified by ?, + or *.

    char *p = pattern;

    if (*p != '^') {
        // The regex doesn't start with an anchor, so add an implicit .*? (non-greedy match-anything).
        *Add(regex) = (Instruction){SPLIT, .rel_next = {3, 1}};
        *Add(regex) = (Instruction){ANY};
        *Add(regex) = (Instruction){JUMP, .rel_next = {-2}};
    } else {
        p += 1;
    }

    while (*p) {
        switch (*p) {
            case '(':
              {
                if (shift_index-shift_stack >= countof(shift_stack)-1)  return parse_error(pattern, p-pattern); // The expression exceeds the maximum of nested capture groups.

                s64   save_id   = 0;
                char *save_name = NULL;

                // Look for the previous start-capture.
                for (s64 i = regex->count-1; i >= 0; i--) {
                    if (regex->data[i].opcode != SAVE)  continue;
                    if (regex->data[i].save_id % 2)  continue;

                    save_id = regex->data[i].save_id + 2;
                    break;
                }

                // If it's a named group, grab the name as well.
                if (*(p+1) == '?') {
                    if (*(p+2) != '<')  return parse_error(pattern, p-pattern);
                    p += 3; // Point to the first character in the name.

                    s64 name_length = 0;
                    while (*(p + name_length) != '>') {
                        if (*(p + name_length) == '\0')  return parse_error(pattern, p-pattern);
                        name_length += 1;
                    }
                    // We could disallow a zero-length name, but we can have zero-length dict keys, so it's fine.

                    save_name = alloc(context, name_length+1, sizeof(char));
                    memcpy(save_name, p, name_length);
                    save_name[name_length] = '\0';

                    p += name_length;
                }

                *Add(regex) = (Instruction){SAVE, .save_id=save_id, .save_name=save_name};

                *shift_index = regex->count;
                shift_index += 1;
                p += 1;
                continue;
              }
            case ')':
              {
                Instruction *save_start = &regex->data[*(shift_index-1)-1];
                if (save_start->opcode != SAVE || save_start->save_id % 2)  return parse_error(pattern, p-pattern); // There's an unmatched right parenthesis.

                // Going back to the start of the capture, look for any JUMP instructions with .rel_next[0] == 0.
                // These are placeholders for jumping to the end of the current capture, which we can now fill in.
                s64 start_index = save_start - regex->data;
                for (s64 i = start_index+1; i < regex->count; i++) {
                    if (regex->data[i].opcode != JUMP)  continue;
                    if (regex->data[i].rel_next[0])     continue;

                    regex->data[i].rel_next[0] = regex->count - i;
                }

                *Add(regex) = (Instruction){SAVE, .save_id = save_start->save_id+1};

                *shift_index = -1;
                shift_index -= 1;
                assert(shift_index > shift_stack);
                // shift_index is currently the index of the instruction after the SAVE that opens this capture group.
                // Decrement it to point to the SAVE instruction itself. This way +, ? and * work on the group as a whole.
                *shift_index -= 1;
                p += 1;
                continue;
              }
            case '|':
              {
                s64 shift_count = regex->count - *(shift_index-1);
                Add(regex);
                for (s64 i = regex->count-1; i > *(shift_index-1); i--)  regex->data[i] = regex->data[i-1];

                regex->data[*(shift_index-1)] = (Instruction){SPLIT, .rel_next = {1, shift_count+2}};

                // If we get to executing the next instruction, it means the left side of a | operator matched fully.
                // We need to jump to the end of the current capture group or the end of the pattern. We don't yet know
                // how far away that will be, so we'll leave .rel_next[0] zero to signify our intent.
                *Add(regex) = (Instruction){JUMP};

                *shift_index = -1;
                p += 1;
                continue;
              }
            case '*':
              {
                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 shift_count = regex->count - *shift_index;
                Add(regex);
                for (s64 i = regex->count-1; i > *shift_index; i--)  regex->data[i] = regex->data[i-1];

                Instruction *split = &regex->data[*shift_index];
                if (*(p+1) == '?') {
                    *split = (Instruction){SPLIT, .rel_next = {shift_count+2, 1}}; // Non-greedy match.
                    p += 1;
                } else {
                    *split = (Instruction){SPLIT, .rel_next = {1, shift_count+2}}; // Greedy match.
                }

                *Add(regex) = (Instruction){JUMP, .rel_next = {-shift_count-1}};

                *shift_index = -1;
                p += 1;
                continue;
              }
            case '+':
              {
                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 inst_count = regex->count - *shift_index;

                Instruction *split = Add(regex);
                if (*(p+1) == '?') {
                    *split = (Instruction){SPLIT, .rel_next = {1, -inst_count}}; // Non-greedy match.
                    p += 1;
                } else {
                    *split = (Instruction){SPLIT, .rel_next = {-inst_count, 1}}; // Greedy match.
                }

                *shift_index = -1;
                p += 1;
                continue;
              }
            case '?':
              {
                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 shift_count = regex->count - *shift_index;
                Add(regex);
                for (s64 i = regex->count-1; i > *shift_index; i--)  regex->data[i] = regex->data[i-1];

                regex->data[*shift_index] = (Instruction){SPLIT, .rel_next = {1, 1+shift_count}};

                *shift_index = -1;
                p += 1;
                continue;
              }
            case '{':
              {
                int const REPEAT_LIMIT = 100; // Meaning we accept e.g. \d{100} but not \d{101}.

                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 inst_count = regex->count - *shift_index;

                // There are inst_count instructions at the end of the regex->data array that we need to repeat some number of times.
                // We want to overwrite these instructions, but we need to keep them around as a reference. Instead of making a copy,
                // which we'd need to allocate and deallocate, we'll leave them where they are and build out what we want after them.
                // Then, at the end of this case, we'll shift everything we've added left, overwriting our reference.

                int min_repeats; {
                    char *after = NULL;
                    long number = strtol(p+1, &after, 10);

                    if (after == p+1)           return parse_error(pattern, p-pattern);
                    if (number < 0)             return parse_error(pattern, p-pattern);
                    if (number > REPEAT_LIMIT)  return parse_error(pattern, p-pattern);

                    min_repeats = number;
                    p = after;
                }

                // First just append the reference instructions min_repeats times.
                for (int i = 0; i < min_repeats; i++) {
                    for (s64 j = 0; j < inst_count; j++)  *Add(regex) = regex->data[*shift_index+j];
                }

                if (*p == ',') {
                    p += 1;

                    if (*p == '}') {
                        // There is no given maximum, e.g. \d{1,} meaning 1 or more.
                        *Add(regex) = (Instruction){SPLIT, .rel_next = {1, inst_count+2}};
                        for (s64 j = 0; j < inst_count; j++)  *Add(regex) = regex->data[*shift_index+j];
                        *Add(regex) = (Instruction){JUMP, .rel_next = {-inst_count-1}};
                    } else {
                        int max_repeats; {
                            char *after = NULL;
                            long number = strtol(p, &after, 10);

                            if (after == p)             return parse_error(pattern, p-pattern);
                            if (number < min_repeats)   return parse_error(pattern, p-pattern);
                            if (number > REPEAT_LIMIT)  return parse_error(pattern, p-pattern);

                            max_repeats = number;
                            p = after;
                        }

                        // For each value from min_repeats to max_repeats, insert a split to the end followed by the reference instructions.
                        s64 expect_count = *shift_index + inst_count + inst_count*min_repeats + (max_repeats - min_repeats)*(inst_count+1);
                        for (int i = min_repeats; i < max_repeats; i++) {
                            s64 count_to_end = expect_count - regex->count;
                            *Add(regex) = (Instruction){SPLIT, .rel_next = {1, count_to_end}};
                            for (s64 j = 0; j < inst_count; j++)  *Add(regex) = regex->data[*shift_index+j];
                        }
                        assert(expect_count == regex->count);
                    }
                }

                if (*p != '}')  return parse_error(pattern, p-pattern);
                p += 1;

                if (*p == '?') {
                    // It's non-greedy. Swap the priority of the SPLIT instructions we've added in this case.
                    for (s64 i = *shift_index; i < regex->count; i++) {
                        Instruction *inst = &regex->data[i];
                        if (inst->opcode == SPLIT)  Swap(&inst->rel_next[0], &inst->rel_next[1]);
                    }
                    p += 1;
                }

                // Shift our newly added instructions left, overwriting our reference.
                for (s64 i = *shift_index; i < regex->count-inst_count; i++)  regex->data[i] = regex->data[i+inst_count];
                regex->count -= inst_count;

                *shift_index = -1;
                continue;
              }
            case '.':
              {
                *Add(regex) = (Instruction){ANY};

                *shift_index = regex->count-1;
                p += 1;
                continue;
              }
            case '[':
              {
                Instruction *inst = Add(regex);
                *inst = (Instruction){ASCII_CLASS};

                bool negate = false;
                p += 1;
                if (*p == '^') {
                    negate = true;
                    p += 1;
                }

                do { //|Cleanup: This is too ugly to live...
                    if (*p == '\0')  return parse_error(pattern, p-pattern);

                    if (*(p+1) == '-') {
                        char range_start = *p;
                        char range_end   = *(p+2); //|Fixme: What if this is a special character? What if it's negative?
                        if (range_start >= range_end)  return parse_error(pattern, p-pattern);
                        for (s64 i = 0; i < range_end-range_start; i++) {
                            u8 byte_index = ((u8)range_start + i)/8;
                            u8 bit_index  = ((u8)range_start + i)%8;
                            inst->class[byte_index] |= (1 << bit_index);
                        }
                        p += 2;
                    } else {
                        u8 byte_index = ((u8)*p)/8;
                        u8 bit_index  = ((u8)*p)%8;
                        inst->class[byte_index] |= (1 << bit_index);
                        p += 1;
                    }
                } while (*p != ']');

                if (negate)  negate_ascii_class(inst);

                p += 1;
                *shift_index = regex->count-1;
                continue;
              }
            case '\\':
              {
                if (*(p+1) == 'd' || *(p+1) == 'D') {                   // \d or \D
                    Instruction *inst = Add(regex);
                    *inst = (Instruction){ASCII_CLASS};
                    for (char d = '0'; d <= '9'; d++)  inst->class[d/8] |= (1<<(d%8));
                    if (*(p+1) == 'D')  negate_ascii_class(inst);
                } else if (*(p+1) == 's' || *(p+1) == 'S') {            // \s or \S
                    Instruction *inst = Add(regex);
                    *inst = (Instruction){ASCII_CLASS};
                    for (char *s = WHITESPACE; *s; s++)  inst->class[(*s)/8] |= (1<<((*s)%8));
                    if (*(p+1) == 'S')  negate_ascii_class(inst);
                } else if (*(p+1) == 't') {                             // \t
                    *Add(regex) = (Instruction){CHAR, .c = '\t'};
                } else if (*(p+1) == 'n') {                             // \n
                    *Add(regex) = (Instruction){CHAR, .c = '\n'};
                } else if (Contains("()*?+[]{}.\\^$", *(p+1))) {        // escaped special character
                    *Add(regex) = (Instruction){CHAR, .c = *(p+1)};
                } else {
                    return parse_error(pattern, p-pattern);
                }

                *shift_index = regex->count-1;
                p += 2;
                continue;
              }
            case '^':
              {
                *Add(regex) = (Instruction){ANCHOR_START};

                *shift_index = -1;
                p += 1;
                continue;
              }
            case '$':
              {
                *Add(regex) = (Instruction){ANCHOR_END};

                *shift_index = -1;
                p += 1;
                continue;
              }
            default:
              {
                *Add(regex) = (Instruction){CHAR, .c = *p};

                *shift_index = regex->count-1;
                p += 1;
                continue;
              }
        }
    }

    // Make sure we exited any capture groups we entered.
    if (shift_index != &shift_stack[1])  return parse_error(pattern, p-pattern);

    *Add(regex) = (Instruction){MATCH};

    // Turn .rel_next members into actual .next pointers.
    for (s64 i = 0; i < regex->count; i++) {
        Instruction *inst = &regex->data[i];

        if (inst->opcode == JUMP) {
            if (!inst->rel_next[0]) {
                // It's a JUMP without a destination. This is a placeholder for jumping to the end of the pattern.
                inst->next[0] = &regex->data[regex->count-1];
            } else {
                Instruction *next = inst + inst->rel_next[0];
                inst->next[0] = next;
            }
        } else if (inst->opcode == SPLIT) {
            Instruction *next0 = inst + inst->rel_next[0];
            Instruction *next1 = inst + inst->rel_next[1];
            inst->next[0] = next0;
            inst->next[1] = next1;
        }
    }

    return regex;
}

#include <ctype.h> //|Todo: make our own isprint

static void print_address(char_array *out, void *address) //|Debug
// Not for general use. Only prints the last 6 hexadecimal digits of the address.
{
    u64 number = ((u64)address) & 0xffffff;
    print_string(out, "0x%06x", number);
}
static void log_regex(Regex *regex) //|Debug
{
    Memory_Context *ctx = new_context(regex->context);
    char_array out = {.context = ctx};

    for (s64 i = 0; i < regex->count; i++) {
        Instruction *inst = &regex->data[i];

        print_address(&out, inst);
        print_string(&out, ":  ");

        switch (inst->opcode) {
            case CHAR:
                print_string(&out, "%-14s", "CHAR");
                print_string(&out, "'%c'", inst->c);
                break;
            case ASCII_CLASS:
              {
                print_string(&out, "%-14s", "ASCII_CLASS");
                int j = 0;
                bool prev_is_set = false;
                while (j < 128) {
                    bool is_set = inst->class[j/8] & (1<<((j%8)));
                    if (!prev_is_set) {
                        if (is_set)  *Add(&out) = isprint(j) ? (char)j : '.';
                    } else {
                        if (!is_set)  print_string(&out, "-%c", isprint(j-1) ? (char)(j-1) : '.');
                    }
                    prev_is_set = is_set;
                    j += 1;
                }
                break;
              }
            case ANY:
                print_string(&out, "%-14s", "ANY");
                break;
            case JUMP:
                print_string(&out, "%-14s", "JUMP");
                print_address(&out, inst->next[0]);
                break;
            case SPLIT:
                print_string(&out, "%-14s", "SPLIT");
                print_address(&out, inst->next[0]);
                print_string(&out, ", ");
                print_address(&out, inst->next[1]);
                break;
            case SAVE:
                print_string(&out, "%-14s", "SAVE");
                print_string(&out, "%ld", inst->save_id);
                break;
            case MATCH:
                print_string(&out, "%-14s", "MATCH");
                break;
            case ANCHOR_START:
                print_string(&out, "%-14s", "ANCHOR_START");
                break;
            case ANCHOR_END:
                print_string(&out, "%-14s", "ANCHOR_END");
                break;
            default:
                assert(!"Unexpected opcode.");
        }
        print_string(&out, "\n");
    }

    Log(out.data);

    free_context(ctx);
}

typedef struct Captures Captures;
struct Captures {
    Memory_Context *context;

    // For every capture group, a copy of the matching substring, or NULL if there was no match.
    char          **data;
    s64             count; // Including the NULLs.

    // For named capture groups only, the matching substrings. We don't make more copies of the
    // strings for this; they're the same pointers as in the above array.
    string_dict    *dict;
};

bool match_regex(char *string, s64 string_length, Regex *regex, Captures *captures)
{
    // Inside this function, we store saves as a singly-linked list.
    typedef struct Save Save;
    struct Save {
        Save *prev;
        s64   id;
        char *name;
        s64   offset; // The index of the character in the source string we were up to when we encountered the SAVE instruction.
    };

    // Russ Cox calls these "threads", in the sense that the compiled regex executes in a virtual machine, which has multiple
    // parallel instructions being executed for each letter in the string being tested. Where the metaphor fails, though, is
    // that the order of execution of the regex threads is controlled---and must be for greedy matching to work.
    typedef struct Thread Thread;
    typedef Array(Thread) Thread_array;
    struct Thread {
        Instruction *instruction;
        Save        *saves;
    };

    bool is_match = false;

    Save *saves = NULL;
    s64 num_saves = 0;

    // Create a child memory context for temporary data.
    Memory_Context *tmp_ctx = new_context(regex->context);

    Thread_array cur_threads  = {.context = tmp_ctx};
    Thread_array next_threads = {.context = tmp_ctx};

    *Add(&cur_threads) = (Thread){regex->data};

    for (s64 string_index = 0; string_index <= string_length; string_index++) {
        char c = string[string_index];

        while (cur_threads.count) {
            Thread thread = cur_threads.data[--cur_threads.count]; // Pop.
            Instruction *inst = thread.instruction;

            switch (inst->opcode) {
                case CHAR:
                    if (c == inst->c)  *Add(&next_threads) = (Thread){inst+1, thread.saves};
                    break;
                case ASCII_CLASS:
                  {
                    u8 byte_index = ((u8)c)/8;
                    u8 bit_index  = ((u8)c)%8;
                    bool is_set   = inst->class[byte_index] & (1<<(bit_index));
                    if (is_set)  *Add(&next_threads) = (Thread){inst+1, thread.saves};
                    break;
                  }
                case ANY:
                    *Add(&next_threads) = (Thread){inst+1, thread.saves};
                    break;
                case JUMP:
                    *Add(&cur_threads) = (Thread){inst->next[0], thread.saves};
                    break;
                case SPLIT:
                    *Add(&cur_threads) = (Thread){inst->next[1], thread.saves};
                    *Add(&cur_threads) = (Thread){inst->next[0], thread.saves}; // This is the one we want to run straight away, so we push it right to the top of the stack.
                    break;
                case SAVE:
                  {
                    Save *save = New(Save, tmp_ctx); //|Speed: We could store these in a tmp_ctx array. It would speed up allocation. Though it would also mean the ->prev members would become invalid pointers when the array moves.
                    save->prev   = thread.saves;
                    save->id     = inst->save_id;
                    save->name   = inst->save_name;
                    save->offset = string_index;
                    if (save->id%2 && save->id >= num_saves)  num_saves = save->id+1;
                    *Add(&cur_threads) = (Thread){inst+1, save};
                    break;
                  }
                case MATCH:
                    is_match = true;
                    saves = thread.saves;
                    cur_threads.count = 0;
                    break;
                case ANCHOR_START:
                    if (string_index == 0)              *Add(&cur_threads) = (Thread){inst+1, thread.saves};
                    break;
                case ANCHOR_END:
                    if (string_index == string_length)  *Add(&cur_threads) = (Thread){inst+1, thread.saves};
                    break;
                default:
                    log_regex(regex);
                    assert(!"Unexpected opcode.");
            }
        }

        if (!next_threads.count)  break;

        // We consumed the current threads in LIFO order; the lowest-priority threads executed last.
        // That means that the next threads are currently ordered with the lowest-priority threads
        // at the top of the stack. Before we start consuming them, we need to reverse them.
        reverse_array(&next_threads);

        cur_threads.count = 0;
        Swap(&cur_threads, &next_threads);
    }

    if (captures && saves) {
        // If the caller didn't specify a context, use the regex context.
        Memory_Context *ctx = captures->context ? captures->context : regex->context;
        *captures = (Captures){.context = ctx}; // This makes sure captures->dict is initialised to zero.

        assert(num_saves % 2 == 0); //|Temporary

        captures->count = num_saves/2;
        captures->data  = New(captures->count, char*, ctx);

        for (Save *save = saves; save != NULL; save = save->prev) {
            if (save->id % 2 == 0)  continue; // Skip the start-captures.

            s64 capture_index = save->id/2;
            // Only overwrite each NULL once. Because we are iterating over the saves backwards, this makes sure
            // a regular expression like /(ab)+/ matches "ababab" but only saves the last "ab".
            if (captures->data[capture_index])  continue;

            // Look for the corresponding start-capture. |Speed: Are we missing the cache a million times?
            Save *end   = save;
            Save *start = end->prev;
            while (start->id != end->id-1)  start = start->prev;

            s64 length = end->offset - start->offset;

            char *substring = alloc(ctx, length+1, sizeof(char));
            memcpy(substring, &string[start->offset], length);
            substring[length] = '\0';

            captures->data[capture_index] = substring;

            if (start->name) {
                if (!captures->dict)  captures->dict = NewDict(captures->dict, ctx);

                *Set(captures->dict, start->name) = substring;
            }
        }
    }

    free_context(tmp_ctx);

    return is_match;
}

//|Todo: If we keep the below, it should maybe print to a supplied char_array?
char_array *extract_string(char *data, s64 start_offset, s64 end_offset, Memory_Context *context)
// start_offset is the index of the first character in the desired substring.
// end_offset is the index of the character after the last character.
// If start_ and end_offset are the same, the result will be the empty string.
{
    char_array *result = NewArray(result, context);

    for (s64 i = start_offset; i < end_offset; i++)  *Add(result) = data[i];

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
            assert(regex);

            continue;
        }

        // If we get here, we have a regex and a string to match.

        Captures captures = {0};
        bool result = match_regex(line->data, line->count, regex, &captures);

        char_array out = {.context = regex_ctx};
        print_string(&out, "Regex:  %s\n", regex_source->data);
        print_string(&out, "String: %s\n", line->data);
        print_string(&out, "Match:  %s\n", result ? "yes" : "no");
        for (s64 i = 0; i < captures.count; i++) {
            char *substring = captures.data[i];
            print_string(&out, "  %s\n", substring);
        }
        Log(out.data);
    }

    free_context(top_context);

    return 0;
}
