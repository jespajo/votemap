#include "array.h"
#include "map.h"

typedef struct Instruction Instruction;
typedef Array(Instruction) Regex;
typedef struct Captures    Captures;

//
// stick with ASCII, son
// none of that UTF-8
// or we'll all be runed
//
// - Junyer (https://jeune.us/haiku-slam.html#1410830961)
//

struct Captures {
    Memory_Context *context;

    // For every capture group, a copy of the matching substring, or NULL if there was no match.
    char          **data;
    s64             count; // The count includes the NULLs.

    // For named capture groups only, the matching substrings. We don't make more copies of the
    // strings for this; they're the same pointers as in the above array.
    string_dict    *dict;
};

Regex *compile_regex(char *pattern, Memory_Context *context);
bool match_regex(char *string, s64 string_length, Regex *regex, Captures *captures);
