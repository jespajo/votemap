//
// Usage:
//
//      Regex *regex = compile_regex("(\\d+)-(\\d+)-(\\d+)", context);
//
//      char *test_string = "123-456-789";
//
//      Match *match = run_regex(regex, test_string, strlen(test_string), context);
//      if (match->success) {
//          string_array groups = copy_capture_groups(match, context);
//          // groups.count == 3
//          // groups.data == {"123", "456", "789"}
//      }
//
//
// Supported regex stuff:
//
//     Character classes
//         any character                       .
//         numeric                             \d
//         whitespace                          \s
//         alphanumeric or underscore          \w
//         any of these characters             [aeiou]
//         character range                     [a-zA-Z]
//         negative variants                   \D   \S   [^\w\-]   etc.
//         hex-encoded raw bytes               \x20
//
//     Quantifiers
//         zero or more                        .*
//         one or more                         .+
//         zero or one                         .?
//         exact count                         .{4}
//         count range                         .{3,5}
//         at least                            .{3,}
//         at most                             .{,5}
//         non-greedy variants                 .*?   .{4}?
//
//     Special
//         alternation                         this|that
//         capture groups                      (.*)
//         named capture groups                (?<name>.*)
//
//
// Differences between this regex and others:
//
// - The default is to fully match. In other words, /abc/ in our program behaves like /^abc$/ in other programs.
//   If you want the behaviour you'd get from /abc/ in other programs, you have to write /.*?abc.*/.
//
// - There's no need to escape forward slashes. Although we sometimes use forward slashes to delimit a regular
//   expression in comments (like in the above point), they have no significance in patterns.
//
// - There's no Unicode support.[1] You can use e.g. /\xff/ to match a single raw byte with the value 0xff.
//
// I made these decisions because I wrote this regex implementation for routing in a HTTP server. So you can
// match "/path/to/file" with the pattern "/path/to/file", which is nicer than "^\/path\/to\/file$".
//
// [1]: https://jeune.us/haiku-slam.html#1410830961
//
#ifndef REGEX_H_INCLUDED
#define REGEX_H_INCLUDED

#include "array.h"
#include "map.h"

typedef Array(struct Regex_instruction) Regex_program;
typedef struct Regex    Regex;
typedef struct Capture  Capture;
typedef Array(Capture)  Capture_array;
typedef struct Match    Match;

struct Regex {
    char           *source;
    Regex_program   program;
    string_array    groups;         // One pointer for each group. For named groups, it's the name, otherwise it's NULL.
};

struct Capture {
    char           *data;           // A pointer into the matched data. To extract copies, use copy_capture_groups() or copy_named_capture_groups().
    s64             length;
};

struct Match {
    bool            success;        // True if the string matched the pattern. This does not report an error; a compiled regex should never fail.
    Capture_array   captures;
};

Regex *compile_regex(char *pattern, Memory_context *context);
Match *run_regex(Regex *regex, char *string, s64 string_length, Memory_context *context);
string_array copy_capture_groups(Match *match, Memory_context *context);
string_dict copy_named_capture_groups(Match *match, Regex *regex, Memory_context *context);

#endif
