// The JSON printer is fine. The parser is slow and bad. We would like to:
// - Split up the parser into `void parse_json()` and `JSON_Value *get_json_parsed()`.
// - Remove the Parsed_JSON type.

#ifndef JSON_H_INCLUDED
#define JSON_H_INCLUDED

#include "array.h"
#include "map.h"

typedef struct JSON_Value   JSON_Value;
typedef Dict(JSON_Value)    JSON_Object;
typedef Array(JSON_Value)   JSON_Array;
typedef struct Parsed_JSON  Parsed_JSON;

enum JSON_Type {
    JSON_NULL,
    JSON_BOOLEAN,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
};

struct JSON_Value {
    enum JSON_Type   type;
    union {
        bool         boolean;
        double       number;
        char_array  *string;
        JSON_Array  *array;
        JSON_Object *object;
    };
};

struct Parsed_JSON {
    bool success;

    // If success, we parsed this many characters. If failure, this is the index of the character
    // that we were at when we failed.
    s64 num_chars;

    JSON_Value json;
};

Parsed_JSON parse_json(char *source, s64 length, Memory_Context *context); // @Speed.
s64 json_value_to_uint(JSON_Value *json);
s64 assert_json_uint(JSON_Value *json);
float assert_json_float(JSON_Value *json);
char_array *assert_json_string(JSON_Value *json);
JSON_Array *json_value_to_array(JSON_Value *json);
JSON_Array *assert_json_array(JSON_Value *json);
JSON_Object *assert_json_object(JSON_Value *json);
void print_json(JSON_Value *json, char_array *out);
char_array *get_json_printed(JSON_Value *json, Memory_Context *context);

#endif // JSON_H_INCLUDED
