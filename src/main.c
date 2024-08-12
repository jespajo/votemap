//|Todo:
//| Factor out parse_token().
//| Alternation.
//| Use strings instead of chars where possible.
//| Anchors.
//| Named capture groups.
//| Count specifiers, i.e. \d{3}.

#include <stdio.h>
#include <string.h>

#include "strings.h"

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
};

//
// stick with ASCII, son
// none of that UTF-8
// or we'll all be runed
//
// - https://jeune.us/haiku-slam.html#1410830961
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

        // If opcode == SAVE, a number identifying the capture group. Even numbers are the starts of captures, odds the ends.
        // For example,
        //                 /xxx(xx)xx(xxx(xx)xx)xxx/     <-- In this regular expression,
        //                     2  3  4   6  7  5         <-- these are the capture_id's.
        s64                capture_id;
    };
};


#include "strings.h"

static Regex *parse_error(char *pattern, s64 index);

Regex *compile_regex_NEW(char *pattern, Memory_Context *context)
//|Todo Speed: Merge CHAR instructions into a CHARS instruction if there's room.
{
    Regex *regex = NewArray(regex, context);

    // *shift_index is the index of the first instruction to shift right if we encounter a ?, + or *.
    // *(shift_index-1) is the index of the first instruction to shift right if we encounter a |.
    #define MAX_NESTED_CAPTURE_GROUPS 10
    s64 shift_indices[MAX_NESTED_CAPTURE_GROUPS+1] = {0};
    s64 *shift_index = &shift_indices[1];

    char *c = pattern;

    if (*c != '^') {
        // The regex doesn't start with an anchor, so add the implicit `.*?` (non-greedy match-anything).
        *Add(regex) = (Instruction){SPLIT, .rel_next = {3, 1}};
        *Add(regex) = (Instruction){ANY};
        *Add(regex) = (Instruction){JUMP, .rel_next = {-2}};
    } else {
        c += 1;
    }

    while (*c) switch (*c) {
        case '\\':
          {
            if (*(c+1) == 'd' || *(c+1) == 'D') {                   // \d or \D
                Instruction *inst = Add(regex);
                *inst = (Instruction){ASCII_CLASS};
                for (char d = '0'; d <= '9'; d++)  inst->class[d/8] |= (1<<(7-d%8));
                if (*(c+1) == 'D') {
                    for (s64 i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
                }
            } else if (*(c+1) == 's' || *(c+1) == 'S') {            // \s or \S
                Instruction *inst = Add(regex);
                *inst = (Instruction){ASCII_CLASS};
                for (char *s = WHITESPACE; *s; s++)  inst->class[(*s)/8] |= (1<<(7-(*s)%8));
                if (*(c+1) == 'S') {
                    for (s64 i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
                }
            } else if (*(c+1) == 't') {                             // \t
                *Add(regex) = (Instruction){CHAR, .c = '\t'};
            } else if (*(c+1) == 'n') {                             // \n
                *Add(regex) = (Instruction){CHAR, .c = '\n'};
            } else if (Contains("()*?+[.\\", *(c+1))) {             // escaped special character
                *Add(regex) = (Instruction){CHAR, .c = *(c+1)};
            } else {
                return parse_error(pattern, c-pattern);
            }
            *shift_index = regex->count-1;
            c += 2;
            continue;
          }
        case '[':
          {
            Instruction *inst = Add(regex);
            *inst = (Instruction){ASCII_CLASS};

            bool negate = false;
            c += 1;
            if (*c == '^') {
                negate = true;
                c += 1;
            }

            do { //|Cleanup: This is too ugly to live...
                if (*c == '\0')  return parse_error(pattern, c-pattern);

                if (*(c+1) == '-') {
                    char range_start = *c;
                    char range_end   = *(c+2); //|Fixme: What if this is a special character? What if it's negative?
                    if (range_start >= range_end)  return parse_error(pattern, c-pattern);
                    for (s64 i = 0; i < range_end-range_start; i++) {
                        u8 byte_index = ((u8)range_start + i)/8;
                        u8 bit_index  = ((u8)range_start + i)%8;
                        inst->class[byte_index] |= (1 << (7 - bit_index));
                    }
                    c += 2;
                } else {
                    u8 byte_index = ((u8)*c)/8;
                    u8 bit_index  = ((u8)*c)%8;
                    inst->class[byte_index] |= (1 << (7 - bit_index));
                    c += 1;
                }
            } while (*c != ']');

            if (negate) {
                for (s64 i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
            }

            c += 1;
            *shift_index = regex->count-1;
            continue;
          }
        case '(':
          {
            s64 capture_id = 2;
            for (s64 i = regex->count-1; i >= 0; i--) {
                Instruction *inst = &regex->data[i];
                if (inst->opcode == SAVE && inst->capture_id % 2 == 0) {
                    capture_id = inst->capture_id + 2;
                    break;
                }
            }
            *Add(regex) = (Instruction){SAVE, .capture_id = capture_id};
            *shift_index = regex->count-1;
            shift_index += 1;
            if (shift_index-shift_indices >= countof(shift_indices))  return parse_error(pattern, c-pattern); // Max nested capture groups exceeded.
            c += 1;
            continue;
          }
        case ')':
          {
            Instruction *save_start = &regex->data[*(shift_index-1)];
            if (save_start->opcode != SAVE)  return parse_error(pattern, c-pattern); // There is a right-parenthesis without a matching left-parenthesis before it.
            assert(save_start->capture_id % 2 == 0);
            *Add(regex) = (Instruction){SAVE, .capture_id = save_start->capture_id+1};
            *shift_index = 0;
            shift_index -= 1;
            assert(shift_index > shift_indices);
            c += 1;
            continue;
          }
        case '*':
          {
            if (*shift_index == 0)  return parse_error(pattern, c-pattern);
            
            // Shift instructions right.
            s64 shift_count = regex->count - *shift_index;
            Add(regex);
            for (s64 i = regex->count-1; i > *shift_index; i--)  regex->data[i] = regex->data[i-1];

            regex->data[*shift_index] = (Instruction){SPLIT, .rel_next = {1, 2+shift_count}};

            *Add(regex) = (Instruction){JUMP, .rel_next = {-shift_count-1}};

            *shift_index = 0;
            c += 1;
            continue;
          }
        case '?':
          {
            if (*shift_index == 0)  return parse_error(pattern, c-pattern);

            // Shift instructions right.
            s64 shift_count = regex->count - *shift_index;
            Add(regex);
            for (s64 i = regex->count-1; i > *shift_index; i--)  regex->data[i] = regex->data[i-1];

            regex->data[*shift_index] = (Instruction){SPLIT, .rel_next = {1, 1+shift_count}};

            *shift_index = 0;
            c += 1;
            continue;
          }
        case '+':
          {
            if (*shift_index == 0)  return parse_error(pattern, c-pattern);
        
            s64 shift_count = regex->count - *shift_index;
            *Add(regex) = (Instruction){SPLIT, .rel_next = {-shift_count, 1}};

            *shift_index = 0;
            c += 1;
            continue;
          }
        case '.':
          {
            *Add(regex) = (Instruction){ANY};
            c += 1;
            *shift_index = regex->count-1;
            continue;
          }
        default:
          {
            *Add(regex) = (Instruction){CHAR, .c = *c};
            c += 1;
            *shift_index = regex->count-1;
            continue;
          }
    }

    *Add(regex) = (Instruction){MATCH};

    // Turn .rel_next members into actual pointers.
    for (s64 i = 0; i < regex->count; i++) {
        Instruction *inst = &regex->data[i];

        if (inst->opcode == JUMP || inst->opcode == SPLIT) {
            Instruction *next0 = inst + inst->rel_next[0];
            Instruction *next1 = inst + inst->rel_next[1];
            inst->next[0] = next0;
            inst->next[1] = next1;
        }
    }

    return regex;
}

static Regex *parse_error(char *pattern, s64 index)
{
    Log("Unexpected character in regex pattern at index %ld: '%c'.", index, pattern[index]);
    return NULL;
}
Regex *compile_regex(char *pattern, Memory_Context *context)
{
    Regex *regex = NewArray(regex, context);

    s64 num_parens = 0;
    char *c = pattern;

    if (*c != '^') {
        // The regex doesn't start with an anchor, so add the implicit `.*?` (non-greedy match-anything).
        *Add(regex) = (Instruction){SPLIT, .rel_next = {3, 1}};
        *Add(regex) = (Instruction){ANY};
        *Add(regex) = (Instruction){JUMP, .rel_next = {-2}};
    } else {
        c += 1;
    }

    while (*c) {
        switch (*c) {
            case '(':
            case ')':
                if ((num_parens % 2) ^ (*c == ')'))  return parse_error(pattern, c-pattern);

                *Add(regex) = (Instruction){SAVE};
                num_parens += 1;
                break;
            case '*':
                if (!regex->count)  return parse_error(pattern, c-pattern);
              {
                Instruction popped = regex->data[regex->count-1];
                regex->count -= 1;

                *Add(regex) = (Instruction){SPLIT, .rel_next = {1, 3}};
                *Add(regex) = popped;
                *Add(regex) = (Instruction){JUMP, .rel_next = {-2}};
              }
                break;
            case '?':
                if (!regex->count)  return parse_error(pattern, c-pattern);
              {
                Instruction popped = regex->data[regex->count-1];
                regex->count -= 1;

                *Add(regex) = (Instruction){SPLIT, .rel_next = {1, 2}};
                *Add(regex) = popped;
              }
                break;
            case '+':
                *Add(regex) = (Instruction){SPLIT, .rel_next = {-1, 1}};
                break;
            case '[':
              {
                Instruction *inst = Add(regex);
                *inst = (Instruction){ASCII_CLASS};

                bool negate = false;
                c += 1;
                if (*c == '^') {
                    negate = true;
                    c += 1;
                }

                do { //|Cleanup: This is too ugly to live...
                    if (*c == '\0')  return parse_error(pattern, c-pattern);

                    if (*(c+1) == '-') {
                        char range_start = *c;
                        char range_end   = *(c+2); //|Fixme: What if this is a special character? What if it's negative?
                        if (range_start >= range_end)  return parse_error(pattern, c-pattern);
                        for (s64 i = 0; i < range_end-range_start; i++) {
                            u8 byte_index = ((u8)range_start + i)/8;
                            u8 bit_index  = ((u8)range_start + i)%8;
                            inst->class[byte_index] |= (1 << (7 - bit_index));
                        }
                        c += 2;
                    } else {
                        u8 byte_index = ((u8)*c)/8;
                        u8 bit_index  = ((u8)*c)%8;
                        inst->class[byte_index] |= (1 << (7 - bit_index));
                        c += 1;
                    }
                } while (*c != ']');

                if (negate)  for (s64 i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
              }
                break;
            case '.':
                *Add(regex) = (Instruction){ANY};
                break;
            case '\\':
                c += 1;
                if (*c == 'd' || *c == 'D') {
                    Instruction *inst = Add(regex);
                    *inst = (Instruction){ASCII_CLASS};
                    for (char d = '0'; d <= '9'; d++)  inst->class[d/8] |= (1<<(7-d%8));
                    if (*c == 'D')  for (s64 i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
                } else if (*c == 's' || *c == 'S') {
                    Instruction *inst = Add(regex);
                    *inst = (Instruction){ASCII_CLASS};
                    for (char *s = WHITESPACE; *s; s++)  inst->class[(*s)/8] |= (1<<(7-(*s)%8));
                    if (*c == 'S')  for (s64 i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
                } else if (*c == 't') {
                    *Add(regex) = (Instruction){CHAR, .c = '\t'};
                } else if (*c == 'n') {
                    *Add(regex) = (Instruction){CHAR, .c = '\n'};
                } else if (Contains("()*?+[.\\", *c)) {
                    *Add(regex) = (Instruction){CHAR, .c = *c};
                } else {
                    return parse_error(pattern, c-pattern);
                }
                break;
            default:
                *Add(regex) = (Instruction){CHAR, .c = *c};
        }
        c += 1;
    }

    *Add(regex) = (Instruction){MATCH};

    // Turn .rel_next members into actual pointers.
    for (s64 i = 0; i < regex->count; i++) {
        Instruction *inst = &regex->data[i];

        if (inst->opcode == JUMP || inst->opcode == SPLIT) {
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
                print_string(&out, "%-14s", "ASCII_CLASS");
              {
                int j = 0;
                bool prev_is_set = false;
                while (j < 128) {
                    bool is_set = inst->class[j/8] & (1<<(7-(j%8)));
                    if (!prev_is_set) {
                        if (is_set)  *Add(&out) = isprint(j) ? (char)j : '.';
                    } else {
                        if (!is_set)  print_string(&out, "-%c", isprint(j-1) ? (char)(j-1) : '.');
                    }
                    prev_is_set = is_set;
                    j += 1;
                }
              }
                break;
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
                break;
            case MATCH:
                print_string(&out, "%-14s", "MATCH");
                break;
            default:
                assert(!"Unexpected opcode.");
        }
        print_string(&out, "\n");
    }

    Log(out.data);

    free_context(ctx);
}

bool match_regex(char *string, s64 string_length, Regex *regex, s64_array *capture_offsets)
{
    typedef struct Capture  Capture;
    typedef struct Thread   Thread; // Russ Cox calls these "threads", in the sense that the compiled regex executes in a virtual machine, which has multiple parallel instructions being executed for each letter in the string being tested. Where the metaphor fails, though, is that the order of execution of the regex threads is deterministic---and must be for greedy matching. If we think of a better word, we'll use that. E.g. Try?
    typedef Array(Thread)   Thread_array;

    // Inside this function, we store captures as a singly-linked list, but convert them to a s64_array before returning.
    struct Capture {
        Capture *prev;
        s64      offset; // The index of the character in the source string we were up to when we encountered the SAVE instruction.
        s64      id;
    };

    struct Thread {
        Instruction *instruction;
        Capture     *captures;
    };

    bool is_match = false;

    Capture *captures = NULL;
    s64  num_captures = -1;

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
                    if (c == inst->c)  *Add(&next_threads) = (Thread){inst+1, thread.captures};
                    break;
                case ASCII_CLASS:
                  {
                    u8 byte_index = ((u8)c)/8;
                    u8 bit_index  = ((u8)c)%8;
                    bool is_set   = inst->class[byte_index] & (1<<(7-bit_index));
                    if (is_set)  *Add(&next_threads) = (Thread){inst+1, thread.captures};
                  }
                    break;
                case ANY:
                    *Add(&next_threads) = (Thread){inst+1, thread.captures};
                    break;
                case JUMP:
                    *Add(&cur_threads) = (Thread){inst->next[0], thread.captures};
                    break;
                case SPLIT:
                    *Add(&cur_threads) = (Thread){inst->next[1], thread.captures};
                    *Add(&cur_threads) = (Thread){inst->next[0], thread.captures}; // This is the one we want to run straight away, so we push it right to the top of the stack.
                    break;
                case SAVE:
                  {
                    Capture *capture = New(Capture, tmp_ctx);
                    capture->prev   = thread.captures;
                    capture->offset = string_index;
                    capture->id     = inst->capture_id;
                    if (capture->id >= num_captures)  num_captures = capture->id+1;
                    *Add(&cur_threads) = (Thread){inst+1, capture};
                  }
                    break;
                case MATCH:
                    is_match = true;
                    captures = thread.captures;
                    cur_threads.count = 0;
                    break;
                default:
                    log_regex(regex);
                    assert(!"Unexpected opcode.");
            }
        }

        // We consumed the current threads in LIFO order; the lowest-priority threads executed last.
        // That means that the next threads are currently ordered with the lowest-priority threads
        // at the top of the stack. Before we start consuming them, we need to reverse them.
        reverse_array(&next_threads);

        cur_threads.count = 0;
        Swap(&cur_threads, &next_threads);
    }

    if (captures && capture_offsets) {
        // If the caller didn't initialise capture_offsets with a context, we'll use the regex context.
        if (!capture_offsets->context)  capture_offsets->context = regex->context;

        // Initialise them all to -1.
        array_reserve(capture_offsets, num_captures);
        for (s64 i = 0; i < num_captures; i++)  capture_offsets->data[i] = -1;

        for (Capture *c = captures; c != NULL; c = c->prev)  capture_offsets->data[c->id] = c->offset;
        capture_offsets->count = num_captures;

        // Currently we don't use capture id's 0 and 1, so pretend they're not there. |Temporary |Hack
        capture_offsets->count -= 2;
        capture_offsets->data  += 2;
    }

    free_context(tmp_ctx);

    return is_match;
}

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
            regex = compile_regex_NEW(regex_source->data, regex_ctx);
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
