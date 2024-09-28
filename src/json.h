// The JSON printer is fine. The parser is slow and bad. We would like to:
// - Split up the parser into `void parse_json()` and `JSON_value *get_json_parsed()`.
// - Remove the Parsed_JSON type.

#ifndef JSON_H_INCLUDED
#define JSON_H_INCLUDED

#include "array.h"
#include "map.h"

typedef struct JSON_value   JSON_value;
typedef Dict(JSON_value)    JSON_object;
typedef Array(JSON_value)   JSON_array;
typedef struct Parsed_JSON  Parsed_JSON;

enum JSON_type {
    JSON_NULL,
    JSON_BOOLEAN,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

struct JSON_value {
    enum JSON_type   type;
    union {
        bool         boolean;
        double       number;
        char_array  *string;
        JSON_array  *array;
        JSON_object *object;
    };
};

struct Parsed_JSON {
    bool success;

    // If success, we parsed this many characters. If failure, this is the index of the character
    // that we were at when we failed.
    s64 num_chars;

    JSON_value json;
};

Parsed_JSON parse_json(char *source, s64 length, Memory_context *context);
s64 json_value_to_uint(JSON_value *json);
s64 assert_json_uint(JSON_value *json);
float assert_json_float(JSON_value *json);
char_array *assert_json_string(JSON_value *json);
JSON_array *json_value_to_array(JSON_value *json);
JSON_array *assert_json_array(JSON_value *json);
JSON_object *assert_json_object(JSON_value *json);
void print_json(JSON_value *json, char_array *out);
char_array *get_json_printed(JSON_value *json, Memory_context *context);

#endif // JSON_H_INCLUDED
