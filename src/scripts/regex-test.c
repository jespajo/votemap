#include "../regex.h"
#include "../strings.h"
#include "../system.h"

//|Todo: If we keep the below, it should maybe print to a supplied char_array?
char_array *extract_string(char *data, s64 start_offset, s64 end_offset, Memory_context *context)
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
    Memory_context *top_context = new_context(NULL);
    Memory_context *regex_ctx   = new_context(top_context);

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

        Match *match = run_regex(regex, line->data, line->count, regex_ctx);
        string_array captures = copy_capture_groups(match, regex_ctx);
        string_dict named_captures = copy_named_capture_groups(match, regex, regex_ctx);

        char_array out = {.context = regex_ctx};
        append_string(&out, "Regex:  %s\n", regex_source->data);
        append_string(&out, "String: %s\n", line->data);
        append_string(&out, "Match:  %s\n", match->success ? "yes" : "no");
        for (s64 i = 0; i < captures.count; i++) {
            char *substring = captures.data[i];
            append_string(&out, "  %ld: %s\n", i, substring);
        }
        for (s64 i = 0; i < named_captures.count; i++) {
            char *key = named_captures.keys[i];
            char *val = named_captures.vals[i];
            append_string(&out, "  %s: %s\n", key, val);
        }
        puts(out.data);
    }

    free_context(top_context);

    return 0;
}
