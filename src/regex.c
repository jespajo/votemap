//|Speed:
//| Merge adjacent CHAR instructions into a STRING instruction.
//| Convert NFAs to DFAs.

#include <ctype.h>

#include "regex.h"
#include "strings.h"

// We can move this to basic.h if we ever want it somewhere else.
#define Swap(A, B)                              \
    do {                                        \
        assert(sizeof(*(A)) == sizeof(*(B)));   \
        u8 tmp[sizeof(*(A))];                   \
        memcpy(tmp, (A), sizeof(*(A)));         \
        memcpy((A), (B), sizeof(*(A)));         \
        memcpy((B), tmp, sizeof(*(A)));         \
    } while (0)

enum Opcode {
    INVALID,
    CHAR,
    BYTE_CLASS,
    ANY,
    JUMP,
    SPLIT,
    SAVE,
    MATCH,
};

typedef struct Regex_instruction Instruction;
struct Regex_instruction {
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

        // If opcode == BYTE_CLASS, a bitfield describing the characters in the class.
        // There is one bit for each byte in the range 0--127 (i.e. ASCII characters).
        u8                 class[128/8];

        // If opcode == SAVE, a number identifying the parenthesis. Even numbers are the
        // starts of captures, odds the ends. For example,
        //             /...(..)..(...(..)..).../     <-- In this regular expression,
        //                 0  1  2   4  5  3         <-- these are the save_ids.
        s64                save_id;
    };
};

static Regex *parse_error(char *pattern, s64 index)
//|Todo: Change to `void report_parse_error(char *pattern, char *fail_char, char *message)`.
{
    char out_data[256] = {0};
    char_array out = {.data = out_data, .limit = sizeof(out_data)};

    if (isprint(pattern[index])) {
        append_string(&out, "Unexpected character in regex pattern at index %ld:\n", index);
        append_string(&out, "    /%s/\n", pattern);
        append_string(&out, "     ");
        for (s64 i = 0; i < index; i++)  *Add(&out) = ' ';
        append_string(&out, "^\n");
    } else {
        append_string(&out, "Unexpected byte in regex pattern at index %ld: '%#02x'\n", index, pattern[index]);
    }

    log_error(out_data);
    return NULL;
}

static void negate_ascii_class(Instruction *inst)
{
    assert(inst->opcode == BYTE_CLASS);
    for (int i = 0; i < countof(inst->class); i++)  inst->class[i] = ~inst->class[i];
}

static Instruction get_token(char *pattern, char **end)
// Read up to three characters from the pattern and return a CHAR or BYTE_CLASS instruction. Assume that special
// characters like '.' and '|' represent themselves literally, regardless of whether they are escaped (useful for
// inside square brackets). Return an INVALID instruction if we read a malformed escape sequence or a weird byte
// (anything other than printable ASCII). Point *end at the character after the last one we parse.
{
    Instruction inst = {INVALID};

    char *p = pattern;

    if (!isprint(*p))  return inst;

    if (*p == '\\') {
        p += 1;
        switch (*p) {
            case 'd':
            case 'D':
                inst.opcode = BYTE_CLASS;
                for (char d = '0'; d <= '9'; d++)  inst.class[d/8] |= (1 << (d%8));
                if (*p == 'D')  negate_ascii_class(&inst);
                p += 1;
                break;
            case 's':
            case 'S':
                inst.opcode = BYTE_CLASS;
                for (char *s = WHITESPACE; *s; s++)  inst.class[*s/8] |= (1 << (*s%8));
                if (*p == 'S')  negate_ascii_class(&inst);
                p += 1;
                break;
            case 'w':
            case 'W':
                inst.opcode = BYTE_CLASS;
                for (char w = 'A'; w <= 'Z'; w++)  inst.class[w/8] |= (1 << (w%8));
                for (char w = 'a'; w <= 'z'; w++)  inst.class[w/8] |= (1 << (w%8));
                for (char w = '0'; w <= '9'; w++)  inst.class[w/8] |= (1 << (w%8));
                inst.class['_'/8] |= (1 << ('_'%8));
                if (*p == 'W')  negate_ascii_class(&inst);
                p += 1;
                break;
            case 't':
                inst.opcode = CHAR;
                inst.c = '\t';
                p += 1;
                break;
            case 'n':
                inst.opcode = CHAR;
                inst.c = '\n';
                p += 1;
                break;
            case 'x':
                p += 1;
                if (!isxdigit(*p) || !isxdigit(*(p+1))) {
                    // Break (and return INVALID) because "\x" must be followed by two hexadecimal digits.
                    break;
                }
                inst.opcode = CHAR;
                inst.c = hex_to_byte(*p, *(p+1));
                p += 2;
                break;
            default:
                if (!Contains("()*?+[]{}.\\-^$", *p)) {
                    // Break (and return INVALID) because it doesn't make sense to escape any other characters.
                    break;
                }
                inst.opcode = CHAR;
                inst.c = *p;
                p += 1;
        }
    } else {
        inst.opcode = CHAR;
        inst.c = *p;
        p += 1;
    }

    if (end)  *end = p;

    return inst;
}

Regex *compile_regex(char *pattern, Memory_context *context)
{
    Regex *re = New(Regex, context);

    re->source = copy_string(pattern, context);

    re->program = (Regex_program){.context = context};

    re->groups = (string_array){.context = context};

    //
    // Other regex compilers usually parse patterns into intermediate representations before compiling. We
    // validate, parse and compile in a single loop. For this to work, we sometimes need to shift previously-added
    // instructions to place something before them. For example, when compiling /.?/, we place an ANY instruction
    // for the dot, and then, when we see the question mark, we shift the ANY right to make room for a SPLIT that
    // creates two branches: one that executes the ANY and another that goes straight to the next instruction.
    //
    // Most of these past-modifying characters affect either the previous token---like /.?/---or, if the previous
    // character was a right-parenthesis closing a capture group, then the whole capture group---like /(...)?/.
    // The exception is /|/, which puts a SPLIT at the start of the *current* capture group.
    //
    // Here's how we do it. We keep a stack of indexes into the regex->program array that we're building. On the
    // top of the stack is the index of the last token we placed---that is, the instruction to shift if we get a
    // modifier like /?/. Just under this on the stack is the index of the start of the current capture group. When
    // we enter a new capture group, we push to the stack, and pop when we leave the capture group. Thus, if we
    // just left a capture group, /?/ automatically modifies that whole group instead of a single token.
    //
    // shift_index points to the top of the stack, so *shift_index is the index of the first instruction to shift
    // if we encounter a modifier like /?/ or /*/, whereas *(shift_index-1) is the index of the first instruction
    // to shift if we encounter a /|/.
    //
    #define MAX_NESTED_CAPTURE_GROUPS 10
    s64 shift_stack[MAX_NESTED_CAPTURE_GROUPS+1] = {0};
    s64 *shift_index = &shift_stack[1];
    *shift_index = -1; // -1 means there is not currently an instruction that can be validly quantified by ?, + or *.

    char *p = pattern;

    while (*p) {
        switch (*p) {
            case '(': {
                if (shift_index-shift_stack >= countof(shift_stack)-1)  return parse_error(pattern, p-pattern); // The expression exceeds the maximum of nested capture groups.

                s64   save_id   = 2*re->groups.count;
                char *save_name = NULL;

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

                *Add(&re->program) = (Instruction){SAVE, .save_id = save_id};

                *Add(&re->groups) = save_name;

                *shift_index = re->program.count;
                shift_index += 1;
                p += 1;
                continue;
            }
            case ')': {
                s64 start_index = *(shift_index-1) - 1;
                Instruction save_start = re->program.data[start_index];
                if (save_start.opcode != SAVE || save_start.save_id % 2) {
                    // There's an unmatched right parenthesis.
                    return parse_error(pattern, p-pattern);
                }

                // Going back to the start of the capture, look for any JUMP instructions with .rel_next[0] == 0.
                // These are placeholders for jumping to the end of the current capture, which we can now fill in.
                for (s64 i = start_index+1; i < re->program.count; i++) {
                    if (re->program.data[i].opcode != JUMP)  continue;
                    if (re->program.data[i].rel_next[0])     continue;

                    re->program.data[i].rel_next[0] = re->program.count - i;
                }

                *Add(&re->program) = (Instruction){SAVE, .save_id = save_start.save_id+1};

                *shift_index = -1;
                shift_index -= 1;
                assert(shift_index > shift_stack);
                // shift_index is currently the index of the instruction after the SAVE that opens this capture group.
                // Decrement it to point to the SAVE instruction itself. This way +, ? and * work on the group as a whole.
                *shift_index -= 1;
                p += 1;
                continue;
            }
            case '|': {
                s64 shift_count = re->program.count - *(shift_index-1);
                Add(&re->program);
                for (s64 i = re->program.count-1; i > *(shift_index-1); i--)  re->program.data[i] = re->program.data[i-1];

                re->program.data[*(shift_index-1)] = (Instruction){SPLIT, .rel_next = {1, shift_count+2}};

                // If we get to executing the next instruction, it means the left side of a | operator matched fully.
                // We need to jump to the end of the current capture group or the end of the pattern. We don't yet know
                // how far away that will be, so we'll leave .rel_next[0] zero to signify our intent.
                *Add(&re->program) = (Instruction){JUMP};

                *shift_index = -1;
                p += 1;
                continue;
            }
            case '*': {
                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 shift_count = re->program.count - *shift_index;
                Add(&re->program);
                for (s64 i = re->program.count-1; i > *shift_index; i--)  re->program.data[i] = re->program.data[i-1];

                Instruction *split = &re->program.data[*shift_index];
                *split = (Instruction){SPLIT, .rel_next = {1, shift_count+2}};

                if (*(p+1) == '?') {
                    Swap(&split->rel_next[0], &split->rel_next[1]); // It's non-greedy. Swap the SPLIT's priority.
                    p += 1;
                }

                *Add(&re->program) = (Instruction){JUMP, .rel_next = {-shift_count-1}};

                *shift_index = -1;
                p += 1;
                continue;
            }
            case '+': {
                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 inst_count = re->program.count - *shift_index;

                Instruction *split = Add(&re->program);
                *split = (Instruction){SPLIT, .rel_next = {-inst_count, 1}};

                if (*(p+1) == '?') {
                    Swap(&split->rel_next[0], &split->rel_next[1]); // It's non-greedy. Swap the SPLIT's priority.
                    p += 1;
                }

                *shift_index = -1;
                p += 1;
                continue;
            }
            case '?': {
                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 shift_count = re->program.count - *shift_index;
                Add(&re->program);
                for (s64 i = re->program.count-1; i > *shift_index; i--)  re->program.data[i] = re->program.data[i-1];

                Instruction *split = &re->program.data[*shift_index];
                *split = (Instruction){SPLIT, .rel_next = {1, 1+shift_count}};

                if (*(p+1) == '?') {
                    Swap(&split->rel_next[0], &split->rel_next[1]); // It's non-greedy. Swap the SPLIT's priority.
                    p += 1;
                }

                *shift_index = -1;
                p += 1;
                continue;
            }
            case '{': {
                int const REPEAT_LIMIT = 100; // Meaning we accept e.g. \d{100} but not \d{101}.

                if (*shift_index < 0)  return parse_error(pattern, p-pattern);

                s64 inst_count = re->program.count - *shift_index;

                // There are inst_count instructions at the end of the re->program array that we need to repeat some number of times.
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
                        Instruction inst = re->program.data[*shift_index+j];
                        *Add(&re->program) = inst;
                    }
                }

                if (*p == ',') {
                    p += 1;

                    if (*p == '}') {
                        // There is no given maximum, e.g. \d{1,} meaning 1 or more.
                        *Add(&re->program) = (Instruction){SPLIT, .rel_next = {1, inst_count+2}};
                        for (s64 j = 0; j < inst_count; j++) {
                            Instruction inst = re->program.data[*shift_index+j];
                            *Add(&re->program) = inst;
                        }
                        *Add(&re->program) = (Instruction){JUMP, .rel_next = {-inst_count-1}};
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
                            s64 count_to_end = expect_count - re->program.count;
                            *Add(&re->program) = (Instruction){SPLIT, .rel_next = {1, count_to_end}};
                            for (s64 j = 0; j < inst_count; j++) {
                                Instruction inst = re->program.data[*shift_index+j];
                                *Add(&re->program) = inst;
                            }
                        }
                        assert(expect_count == re->program.count);
                    }
                }

                if (*p != '}')  return parse_error(pattern, p-pattern);
                p += 1;

                if (*p == '?') {
                    // It's non-greedy. Swap the priority of all the SPLIT instructions we've added in this case.
                    for (s64 i = *shift_index; i < re->program.count; i++) {
                        Instruction *inst = &re->program.data[i];
                        if (inst->opcode == SPLIT)  Swap(&inst->rel_next[0], &inst->rel_next[1]);
                    }
                    p += 1;
                }

                // Shift our newly added instructions left, overwriting our reference.
                for (s64 i = *shift_index; i < re->program.count-inst_count; i++)  re->program.data[i] = re->program.data[i+inst_count];
                re->program.count -= inst_count;

                *shift_index = -1;
                continue;
            }
            case '.': {
                *Add(&re->program) = (Instruction){ANY};

                *shift_index = re->program.count-1;
                p += 1;
                continue;
            }
            case '[': {
                Instruction *inst = Add(&re->program);
                *inst = (Instruction){BYTE_CLASS};

                bool negate = false;
                p += 1;
                if (*p == '^') {
                    negate = true;
                    p += 1;
                }

                while (*p != ']') { //|Todo: Do we care about rejecting empty square brackets?
                    Instruction token = get_token(p, &p);
                    if (token.opcode == INVALID)  return parse_error(pattern, p-pattern);

                    if (*p == '-') {
                        if (token.opcode != CHAR)  return parse_error(pattern, p-pattern);

                        Instruction next = get_token(p+1, &p);
                        if (next.opcode != CHAR)  return parse_error(pattern, p-pattern);
                        if (next.c <= token.c)    return parse_error(pattern, p-pattern);

                        for (char c = token.c; c <= next.c; c++)  inst->class[c/8] |= (1 << (c%8));
                    } else if (token.opcode == CHAR) {
                        inst->class[token.c/8] |= (1 << (token.c%8));
                    } else {
                        assert(token.opcode == BYTE_CLASS);
                        for (int i = 0; i < countof(inst->class); i++)  inst->class[i] |= token.class[i];
                    }
                }

                if (negate)  negate_ascii_class(inst);

                p += 1;
                *shift_index = re->program.count-1;
                continue;
            }
            default: {
                Instruction inst = get_token(p, &p);
                if (inst.opcode == INVALID)  return parse_error(pattern, p-pattern);

                *Add(&re->program) = inst;

                *shift_index = re->program.count-1;
                continue;
            }
        }
    }

    // Make sure we exited any capture groups we entered.
    if (shift_index != &shift_stack[1])  return parse_error(pattern, p-pattern);

    *Add(&re->program) = (Instruction){MATCH};

    // Turn .rel_next members into actual .next pointers.
    for (s64 i = 0; i < re->program.count; i++) {
        Instruction *inst = &re->program.data[i];

        if (inst->opcode == JUMP) {
            if (!inst->rel_next[0]) {
                // It's a JUMP without a destination. This is a placeholder for jumping to the end of the pattern.
                inst->next[0] = &re->program.data[re->program.count-1];
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

    return re;
}

static void print_address(char_array *out, void *address) //|Debug
// Not for general use. Only prints the last 6 hexadecimal digits of the address.
{
    u64 number = ((u64)address) & 0xffffff;
    append_string(out, "0x%06x", number);
}

static void log_regex(Regex *regex) //|Debug
{
    Memory_context *ctx = new_context(regex->program.context);
    char_array out = {.context = ctx};

    append_string(&out, "/%s/\n", regex->source);

    for (s64 i = 0; i < regex->program.count; i++) {
        Instruction *inst = &regex->program.data[i];

        print_address(&out, inst);
        append_string(&out, ":  ");

        switch (inst->opcode) {
            case CHAR:
                append_string(&out, "%-14s", "CHAR");
                append_string(&out, "'%c'", inst->c);
                break;
            case BYTE_CLASS:
              {
                append_string(&out, "%-14s", "BYTE_CLASS");
                int j = 0;
                bool prev_is_set = false;
                while (j < 128) {
                    bool is_set = inst->class[j/8] & (1<<((j%8)));
                    if (!prev_is_set) {
                        if (is_set)  *Add(&out) = isprint(j) ? (char)j : '.';
                    } else {
                        if (!is_set)  append_string(&out, "-%c", isprint(j-1) ? (char)(j-1) : '.');
                    }
                    prev_is_set = is_set;
                    j += 1;
                }
                break;
              }
            case ANY:
                append_string(&out, "%-14s", "ANY");
                break;
            case JUMP:
                append_string(&out, "%-14s", "JUMP");
                print_address(&out, inst->next[0]);
                break;
            case SPLIT:
                append_string(&out, "%-14s", "SPLIT");
                print_address(&out, inst->next[0]);
                append_string(&out, ", ");
                print_address(&out, inst->next[1]);
                break;
            case SAVE:
                append_string(&out, "%-14s", "SAVE");
                append_string(&out, "%ld", inst->save_id);
                break;
            case MATCH:
                append_string(&out, "%-14s", "MATCH");
                break;
            default:
                assert(!"Unexpected opcode.");
        }
        append_string(&out, "\n");
    }

    printf("%s", out.data);

    free_context(ctx);
}

Match *run_regex(Regex *regex, char *string, s64 string_length, Memory_context *context)
// This function uses the provided context both for the returned data and for temporary allocations. It doesn't
// clean up its temporary allocations. We could add an optional temp_context argument to fix this if needed.
{
    // When we come across a SAVE instruction, we create a Save struct to record the fact. It works like a linked list:
    // a thread references the Save it saw most recently, which references the Save the thread saw before that and so on.
    // We don't want to allocate every Save individually, so we put them in an array. Hence, since the array might move,
    // we use indexes rather than pointers.
    typedef struct Save Save;
    typedef Array(Save) Save_array;
    struct Save {
        s64 id;                 // The SAVE instruction's save_id.
        s64 prev_save_index;    // The index, in the saves array, of the Save previously encountered by the thread.
        s64 match_offset;       // The index of the character we were up to when we saw the SAVE instruction.
    };

    // Russ Cox calls these "threads", in the sense that the compiled regex executes in a virtual machine, which has multiple
    // parallel instructions being executed for each letter in the string being tested. Where the metaphor fails, though, is
    // that the order of execution of the regex threads is deterministic, as it must be for greedy matching to work.
    typedef struct Thread Thread;
    typedef Array(Thread) Thread_array;
    struct Thread {
        Instruction *instruction;
        s64          last_save_index;
    };

    Match *match = New(Match, context);

    // These are the temporary arrays that we don't clean up. |Leak
    Thread_array cur_threads  = {.context = context};
    Thread_array next_threads = {.context = context};
    Save_array   saves        = {.context = context};

    s64 last_save_index = -1;

    *Add(&cur_threads) = (Thread){regex->program.data, -1};

    for (s64 string_index = 0; string_index <= string_length; string_index++) {
        char c = string[string_index];

        while (cur_threads.count) {
            Thread thread = cur_threads.data[--cur_threads.count]; // Pop.

            switch (thread.instruction->opcode) {
                case CHAR:
                    if (c != thread.instruction->c)  break;
                    thread.instruction += 1;
                    *Add(&next_threads) = thread;
                    break;
                case BYTE_CLASS: {
                    bool is_set = thread.instruction->class[c/8] & (1 << (c%8));
                    if (!is_set)  break;
                    thread.instruction += 1;
                    *Add(&next_threads) = thread;
                    break;
                }
                case ANY:
                    thread.instruction += 1;
                    *Add(&next_threads) = thread;
                    break;
                case JUMP:
                    thread.instruction = thread.instruction->next[0];
                    *Add(&cur_threads) = thread;
                    break;
                case SPLIT: {
                    Thread t0 = {thread.instruction->next[0], thread.last_save_index};
                    Thread t1 = {thread.instruction->next[1], thread.last_save_index};
                    *Add(&cur_threads) = t1;
                    *Add(&cur_threads) = t0; // This is the one we want to run straight away, so push it to the top of the stack.
                    break;
                }
                case SAVE: {
                    Save *save = Add(&saves);
                    save->id              = thread.instruction->save_id;
                    save->prev_save_index = thread.last_save_index;
                    save->match_offset    = string_index;

                    thread.instruction += 1;
                    thread.last_save_index = saves.count-1;
                    *Add(&cur_threads) = thread;
                    break;
                }
                case MATCH:
                    if (string_index != string_length)  break; // Only match if we've come to the end of the source string.
                    match->success = true;
                    last_save_index = thread.last_save_index;
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

        // We consumed the current threads as a stack, back to front. The lowest-priority threads executed last.
        // All the while, we were adding to the next_threads array. Thus the next_threads array currently has the
        // highest-priority threads at its front. So we need to reverse it to consume it on the next loop.
        reverse_array(&next_threads);

        cur_threads.count = 0;
        Swap(&cur_threads, &next_threads);
    }

    if (match->success && regex->groups.count > 0) {
        match->captures = (Capture_array){.context = context};
        array_reserve(&match->captures, regex->groups.count);
        memset(match->captures.data, 0, match->captures.limit*sizeof(Capture));

        while (last_save_index >= 0) {
            Save     save    = saves.data[last_save_index];
            Capture *capture = &match->captures.data[save.id/2];

            if (save.id % 2) {
                // The save.id denotes the end of a capture group.
                if (!capture->data) {
                    // We haven't seen this group before. Record the end of the submatch.
                    capture->data = &string[save.match_offset];
                }
            } else {
                // The save.id denotes the start of a capture group.
                if (capture->data && !capture->length) {
                    // We've seen the end of this group but not the start. Record the capture.
                    // Keeping the first instance we see makes sure that an expression like
                    // /(ab)+/ matches "ababab" but only saves the last "ab".
                    capture->length = (capture->data - string) - save.match_offset;
                    capture->data   = &string[save.match_offset];

                    match->captures.count += 1;
                    if (match->captures.count == regex->groups.count)  break;
                }
            }
            last_save_index = save.prev_save_index;
        }
        match->captures.count = regex->groups.count;
    }

    return match;
}

string_array copy_capture_groups(Match *match, Memory_context *context)
// The returned array has one pointer for every capture. Each one points to a unique copy of the matched substring.
// If a capture group didn't match anything, the pointer is NULL. If the capture group had a zero-length match,
// the string is just "\0". (A zero-length match happens if e.g. /(.*)abc/ runs on "abc".)
{
    string_array copies = (string_array){.context = context};

    for (s64 i = 0; i < match->captures.count; i++) {
        Capture *capture = &match->captures.data[i];
        char *copy = NULL;
        if (capture->data) {
            copy = alloc(capture->length+1, sizeof(char), context);
            memcpy(copy, capture->data, capture->length);
            copy[capture->length] = '\0';
        }
        *Add(&copies) = copy;
    }

    return copies;
}

string_dict copy_named_capture_groups(Match *match, Regex *regex, Memory_context *context)
// Like copy_capture_groups() but in dict form. The returned dict only has named capture groups.
{
    string_dict copies = (string_dict){.context = context};

    for (s64 i = 0; i < match->captures.count; i++) {
        char *name = regex->groups.data[i];
        if (!name)  continue;

        Capture *capture = &match->captures.data[i];
        if (!capture->data)  continue;

        char *copy = alloc(capture->length+1, sizeof(char), context);
        memcpy(copy, capture->data, capture->length);
        copy[capture->length] = '\0';

        *Set(&copies, name) = copy;
    }

    return copies;
}
