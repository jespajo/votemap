//|Speed:
//| Figure out the number of capture groups during compilation and put that on the Regex struct. Then we don't have to figure it out again in match_regex().
//| Experiment with having each Thread being initialised with an array of saves (once the number of capture groups is on the Regex struct, we'll know the size this array needs to be). Test the impact on performance. In other words, do the saves the way Russ Cox does them instead of with your linked list.
//| See if there's a better way to deduplicate the next_threads array. Cox seems to just store the last-added instruction as a global variable and make sure he doesn't add it again. But I don't understand how this is enough---what about duplicates added non-consecutively?
//| Merge adjacent CHAR instructions into a STRING or LITERAL instruction.
//| Convert NFAs to DFAs.

#include <ctype.h> //|Todo: make our own isprint()

#include "regex.h"
#include "strings.h" // For WHITESPACE

//|Todo: Move to basic.h
#define Swap(A, B)                              \
    do {                                        \
        assert(sizeof(*(A)) == sizeof(*(B)));   \
        u8 tmp[sizeof(*(A))];                   \
        memcpy(tmp, (A), sizeof(*(A)));         \
        memcpy((A), (B), sizeof(*(A)));         \
        memcpy((B), tmp, sizeof(*(A)));         \
    } while (0)

//|Todo: Move to array.h
typedef Array(s64)         s64_array;

enum Opcode {
    CHAR = 1,
    ASCII_CLASS,
    ANY,
    JUMP,
    SPLIT,
    SAVE,
    MATCH,
};

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
            // A number identifying the parenthesis. Even numbers are the starts of captures, odds the ends.
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
    log_error("Unexpected character in regex pattern at index %ld: '%c'.", index, pattern[index]);
    return NULL;
}

static void negate_ascii_class(Instruction *inst)
{
    assert(inst->opcode == ASCII_CLASS);
    for (int i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
}

Regex *compile_regex(char *pattern, Memory_context *context)
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

                    save_name = alloc(name_length+1, sizeof(char), context);
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
                *split = (Instruction){SPLIT, .rel_next = {1, shift_count+2}};

                if (*(p+1) == '?') {
                    Swap(&split->rel_next[0], &split->rel_next[1]); // It's non-greedy. Swap the SPLIT's priority.
                    p += 1;
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
                *split = (Instruction){SPLIT, .rel_next = {-inst_count, 1}};

                if (*(p+1) == '?') {
                    Swap(&split->rel_next[0], &split->rel_next[1]); // It's non-greedy. Swap the SPLIT's priority.
                    p += 1;
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

                Instruction *split = &regex->data[*shift_index];
                *split = (Instruction){SPLIT, .rel_next = {1, 1+shift_count}};

                if (*(p+1) == '?') {
                    Swap(&split->rel_next[0], &split->rel_next[1]); // It's non-greedy. Swap the SPLIT's priority.
                    p += 1;
                }

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
                    for (s64 j = 0; j < inst_count; j++) {
                        Instruction inst = regex->data[*shift_index+j];
                        *Add(regex) = inst;
                    }
                }

                if (*p == ',') {
                    p += 1;

                    if (*p == '}') {
                        // There is no given maximum, e.g. \d{1,} meaning 1 or more.
                        *Add(regex) = (Instruction){SPLIT, .rel_next = {1, inst_count+2}};
                        for (s64 j = 0; j < inst_count; j++) {
                            Instruction inst = regex->data[*shift_index+j];
                            *Add(regex) = inst;
                        }
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
                            for (s64 j = 0; j < inst_count; j++) {
                                Instruction inst = regex->data[*shift_index+j];
                                *Add(regex) = inst;
                            }
                        }
                        assert(expect_count == regex->count);
                    }
                }

                if (*p != '}')  return parse_error(pattern, p-pattern);
                p += 1;

                if (*p == '?') {
                    // It's non-greedy. Swap the priority of all the SPLIT instructions we've added in this case.
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

static void print_address(char_array *out, void *address) //|Debug
// Not for general use. Only prints the last 6 hexadecimal digits of the address.
{
    u64 number = ((u64)address) & 0xffffff;
    print_string(out, "0x%06x", number);
}

static void log_regex(Regex *regex) //|Debug
{
    Memory_context *ctx = new_context(regex->context);
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
            default:
                assert(!"Unexpected opcode.");
        }
        print_string(&out, "\n");
    }

    printf(out.data);

    free_context(ctx);
}

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
    // that the order of execution of the regex threads is controlled, as it must be for greedy matching to work.
    typedef struct Thread Thread;
    typedef Array(Thread) Thread_array;
    struct Thread {
        Instruction *instruction;
        Save        *saves;
    };

    bool is_match = false;

    Save *saves = NULL;

    // Create a child memory context for temporary data.
    Memory_context *tmp_ctx = new_context(regex->context);

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
                    *Add(&cur_threads) = (Thread){inst->next[0], thread.saves}; // This is the one we want to run straight away, so push it right to the top of the stack.
                    break;
                case SAVE:
                  {
                    Save *save = New(Save, tmp_ctx); //|Speed: We could store these in a tmp_ctx array. It would speed up allocation. Though it would also mean the ->prev members would become invalid pointers when the array moves.
                    save->prev   = thread.saves;
                    save->id     = inst->save_id;
                    save->name   = inst->save_name;
                    save->offset = string_index;
                    *Add(&cur_threads) = (Thread){inst+1, save};
                    break;
                  }
                case MATCH:
                    if (string_index != string_length)  break; // Only match if we've come to the end of the source string.
                    is_match = true;
                    saves = thread.saves;
                    cur_threads.count = 0;
                    break;
                default:
                    log_regex(regex);
                    assert(!"Unexpected opcode.");
            }
        }

        if (!next_threads.count)  break;

        // Deduplicate the next threads. This deduplication keeps the first instance of a thread.
        // |Todo: Factor this out. The trouble is threads are identified by their .instruction member only.
        for (s64 i1 = next_threads.count-1; i1 >= 0; i1--) {
            Thread *target = &next_threads.data[i1];
            for (s64 i0 = i1-1; i0 >= 0; i0--) {
                if (next_threads.data[i0].instruction != target->instruction)  continue;
                // The target has a precursor. Delete the target.
                for (s64 i2 = i1+1; i2 < next_threads.count; i2++) {
                    *target = next_threads.data[i2];
                    target += 1;
                }
                next_threads.count -= 1;
                break;
            }
        }

        // We consumed the current threads as a stack, i.e. back to front. The lowest-priority threads executed last.
        // All the while, we were adding to the next_threads array. Thus the next_threads array currently has the
        // highest-priority threads at its front. So we need to reverse the array to consume it as a stack.
        reverse_array(&next_threads);

        cur_threads.count = 0;
        Swap(&cur_threads, &next_threads);
    }

    if (is_match && captures) {
        // Use the regex's memory context for the captures if the caller didn't specify a context.
        Memory_context *ctx = captures->context ? captures->context : regex->context;
        *captures = (Captures){.context = ctx}; // This makes sure captures->dict is set to NULL.

        // Figure out how many capture groups there are. We do this in a separate loop over the instructions, rather than
        // keeping a running total in the loop above, because we can't guarantee that every SAVE actually executed.
        // But we can guarantee that the last start-capture is the last capture group, so iterate in reverse.
        // |Cleanup: This is dumb. Save the number of captures on the Regex struct instead.
        for (s64 i = regex->count-1; i >= 0; i--) {
            if (regex->data[i].opcode != SAVE)  continue;
            if (regex->data[i].save_id % 2)     continue;

            captures->count = regex->data[i].save_id/2 + 1;
            break;
        }

        if (captures->count)  captures->data = New(captures->count, char*, ctx);

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

            char *substring = alloc(length+1, sizeof(char), ctx);
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
