//
// |Todo: Finish this documentation.
//
// Supported regex stuff:
//
//     Character classes
//
//         any character                       .
//         numeric                             \d
//         whitespace                          \s
//         any of these characters             [aeiou]
//         character range                     [a-zA-Z]
//         negative variants                   \D   [^aeiou]
//
//     Quantifiers
//
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
//
//         alternation                         this|that
//         capture groups                      (.*)
//         named capture groups                (?<name>.*)
//
//     |Todo
//
//         hex-encoded bytes                   \x20
//         alphanumeric or underscore          \w
//         special characters in ranges        [a-z\s]
//
//
// Differences between our regex and others:
//
// - We only support ASCII in regex patterns.[1]
//
// - The default is to fully match. In other words, /abc/ in our program behaves like /^abc$/ in other programs.
//   If you want the behaviour you'd get from /abc/ in other programs, you have to write /.*?abc.*/.
//
// - There's no need to escape forward slashes. Although we sometimes use forward slashes to delimit a regular
//   expression in comments (like in the above point), they have no significance in patterns.
//
// These decisions were informed by the fact that we wrote this regex implementation for routing in a HTTP server.
// It means that we can match the exact string "/path/to/file" with the pattern "/path/to/file", which is nicer
// than "^\/path\/to\/file$".
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
