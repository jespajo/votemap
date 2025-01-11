#include <float.h>   // FLT_MIN, FLT_MAX
#include <math.h>    // HUGE_VAL

#include "json.h"
#include "strings.h"

static char *find_end_quote(char *source, s64 length)
{
    assert(source[0] == '"');

    bool escaped = false;
    for (s64 i = 1; i < length; i++) {
        if (!escaped) {
            if (source[i] == '"')  return &source[i];

            if (source[i] == '\\') {
                escaped = true;
                continue;
            }
        }
        escaped = false;
    }
    return NULL;
}

static char_array parse_json_string(char *source, s64 length, Memory_context *context)
// |Todo: Support extended unicode characters. https://datatracker.ietf.org/doc/html/rfc8259#section-7
{
    // The string should start and end with quotation marks.
    assert(source[0] == '"' && source[length-1] == '"');

    char_array result = {.context = context};

    // The assumption here is that the resulting raw bytes can be no bigger than the source string
    // (which might have extra backslash characters). Hence we reserve at least as much space as we
    // need to prevent the need to reallocate. We assert this at the bottom of the function.
    array_reserve(&result, length+1);

    s64 i = 1;
    while (i < length-1) {
        if (source[i] == '\\') {
            switch (source[i+1]) {
                case '"':   *Add(&result) = '"';   break;
                case '\\':  *Add(&result) = '\\';  break;
                case 'n':   *Add(&result) = '\n';  break;
                case 't':   *Add(&result) = '\t';  break;
                case 'r':   *Add(&result) = '\r';  break;

                default:  assert(!"Unexpected escaped character");
            }
            i += 2;
            continue;
        }

        *Add(&result) = source[i];
        i += 1;
    }

    // Make sure the string is zero-terminated, but don't include the terminator in the character count.
    *Add(&result) = '\0';
    result.count -= 1;

    assert(result.count <= length); // Connected with array_reserve() above.

    return result;
}

Parsed_JSON parse_json(char *source, s64 length, Memory_context *context)
// |Terrible!
{
    char *remainder = trim_left(source, WHITESPACE);

  #define ParseError(...) \
      (log_error("Parse error: " __VA_ARGS__), \
       (Parsed_JSON){.num_chars = remainder - source})

    Parsed_JSON parsed = {0};

    if (remainder[0] == 'n') {
        parsed.json.type = JSON_NULL;

        if (strncmp(remainder, "null", lengthof("null")))  return ParseError("Expected `null`");

        parsed.success = true;
        parsed.num_chars = lengthof("null");
        return parsed;
    }

    if (remainder[0] == 'f' || remainder[0] == 't') {
        parsed.json.type = JSON_BOOLEAN;

        bool b = parsed.json.boolean = (remainder[0] == 't');
        char *expect = b ? "true" : "false";
        s64   length = b ? lengthof("true") : lengthof("false");

        if (strncmp(remainder, expect, length))  return ParseError("Expected `%s`", expect);


        parsed.success = true;
        parsed.num_chars = length;
        return parsed;
    }

    if (('0' <= remainder[0] && remainder[0] <= '9') || remainder[0] == '-') {
        parsed.json.type = JSON_NUMBER;

        char *end_of_number;
        double d = parsed.json.number = strtod(remainder, &end_of_number);

        if (d == 0 && end_of_number == remainder)  return ParseError("Expected a number");
        if (d > HUGE_VAL || d < -HUGE_VAL)  return ParseError("Number is out of range");
        // At the moment we ignore a potential underflow. We could detect it with (d == DBL_MIN || d == -DBL_MIN).

        parsed.success = true;
        parsed.num_chars = end_of_number - remainder;
        return parsed;
    }

    if (remainder[0] == '"') {
        parsed.json.type = JSON_STRING;

        char *end_quote = find_end_quote(remainder, length-(remainder-source));
        if (!end_quote)  return ParseError("Couldn't find end quote");

        parsed.json.string = New(char_array, context);

        *parsed.json.string = parse_json_string(remainder, end_quote-remainder+1, context);
        if (!parsed.json.string->data)  return ParseError("Couldn't parse string inside JSON data");

        parsed.success = true;
        parsed.num_chars = end_quote - remainder + 1;
        return parsed;
    }

    if (remainder[0] == '[') {
        parsed.json.type        = JSON_ARRAY;
        parsed.json.array = NewArray(parsed.json.array, context);

        remainder = trim_left(remainder+1, WHITESPACE);

        while (remainder[0] != ']') {
            if (parsed.json.array->count) {
                // The array already has at least one member.
                if (remainder[0] != ',')  return ParseError("Expected a comma");

                remainder = trim_left(remainder+1, WHITESPACE);
            }

            Parsed_JSON parsed_member = parse_json(remainder, length-(remainder-source), context);
            if (!parsed_member.success)  return parsed_member;

            *Add(parsed.json.array) = parsed_member.json;

            remainder = trim_left(remainder+parsed_member.num_chars, WHITESPACE);
        }

        remainder += 1;
        parsed.num_chars = remainder - source;
        parsed.success = true;
        return parsed;
    }

    if (remainder[0] == '{') {
        parsed.json.type   = JSON_OBJECT;
        parsed.json.object = NewDict(parsed.json.object, context);

        remainder = trim_left(remainder+1, WHITESPACE);

        while (remainder[0] != '}') {
            if (parsed.json.object->count) {
                // The object already has at least one property.
                if (remainder[0] != ',')  return ParseError("Expected a comma");

                remainder = trim_left(remainder+1, WHITESPACE);
            }

            char *end_quote = find_end_quote(remainder, length-(remainder-source));
            if (!end_quote)  return ParseError("Couldn't find end of key");

            char_array key = parse_json_string(remainder, end_quote-remainder+1, context);
            if (!key.data)  return ParseError("Couldn't parse key");

            remainder = trim_left(end_quote+1, WHITESPACE);
            if (remainder[0] != ':')  return ParseError("Expected a colon");

            remainder = trim_left(remainder+1, WHITESPACE);
            Parsed_JSON parsed_value = parse_json(remainder, length-(remainder-source), context);
            if (!parsed_value.success)  return parsed_value;

            s64 old_count = parsed.json.object->count;
            *Set(parsed.json.object, key.data) = parsed_value.json;
            if (parsed.json.object->count == old_count)  log_error("Duplicate key `%s`\n", key.data);

            remainder = trim_left(remainder+parsed_value.num_chars, WHITESPACE);
        }

        remainder += 1;
        parsed.num_chars = remainder - source;
        parsed.success = true;
        return parsed;
    }

    return ParseError("Unexpected character");
  #undef ParseError
}

static void print_bytes_escaped_for_json(char *bytes, s64 num_bytes, char_array *out)
// Turn a string of bytes into a representation that could be used inside quotation marks in a JSON file.
{
    for (s64 i = 0; i < num_bytes; i++) {
        switch (bytes[i]) {
            case '\n':
                *Add(out) = '\\';
                *Add(out) = 'n';
                break;
            case '\t':
                *Add(out) = '\\';
                *Add(out) = 't';
                break;
            case '\r':
                *Add(out) = '\\';
                *Add(out) = 'r';
                break;
            case '"':
            case '\\':
                *Add(out) = '\\'; // fall through
            default:
                *Add(out) = bytes[i];
        }
    }

    *Add(out) = '\0';
    out->count -= 1;
}

s64 json_value_to_uint(JSON_value *json)
// Return the number if it's a non-negative number in the range of an s64. Otherwise return -1.
{
    if (json->type != JSON_NUMBER)  return -1;
    if (json->number < 0)     return -1;

    // |Fixme: Safely cast with frexp. https://stackoverflow.com/a/26584177
    if (json->number >= (double)INT64_MAX)  return -1;

    return (s64)json->number;
}

s64 assert_json_uint(JSON_value *json)
{
    assert(json_value_to_uint(json) >= 0);
    return (s64)json->number;
}

float assert_json_float(JSON_value *json)
{
    assert(json->type == JSON_NUMBER);
    assert(-FLT_MAX <= json->number && json->number <= FLT_MAX); // This might be pointless. Remove if it causes problems.
    return (float)json->number;
}

char_array *assert_json_string(JSON_value *json)
{
    assert(json->type == JSON_STRING);
    return json->string;
}

JSON_array *json_value_to_array(JSON_value *json)
{
    if (json->type != JSON_ARRAY)  return NULL;
    return json->array;
}

JSON_array *assert_json_array(JSON_value *json)
{
    assert(json->type == JSON_ARRAY);
    return json->array;
}

JSON_object *assert_json_object(JSON_value *json)
{
    assert(json->type == JSON_OBJECT);
    return json->object;
}

void print_json(JSON_value *json, char_array *out)
{
    JSON_value *j = json;

    switch (j->type) {
        case JSON_NULL:
            print_string(out, "null");
            break;
        case JSON_BOOLEAN:
            print_string(out, "%s", j->boolean ? "true" : "false");
            break;
        case JSON_NUMBER:
            print_double(j->number, out);
            break;
        case JSON_STRING:;
            *Add(out) = '"';
            print_bytes_escaped_for_json(j->string->data, j->string->count, out);
            *Add(out) = '"';
            break;
        case JSON_ARRAY:
            *Add(out) = '[';
            for (s64 i = 0; i < j->array->count; i++) {
                if (i)  print_string(out, ", ");
                print_json(&j->array->data[i], out);
            }
            *Add(out) = ']';
            break;
        case JSON_OBJECT:
            *Add(out) = '{';
            for (s64 i = 0; i < j->object->count; i++) {
                if (i)  print_string(out, ", ");

                char *key = j->object->keys[i];
                *Add(out) = '"';
                print_bytes_escaped_for_json(key, strlen(key), out);
                *Add(out) = '"';
                print_string(out, ": ");

                JSON_value *value = &j->object->vals[i];
                print_json(value, out);
            }
            *Add(out) = '}';
            break;
        default:
            assert(!"Bad json type!");
    }

    *Add(out) = '\0';
    out->count -= 1;
}

char_array *get_json_printed(JSON_value *json, Memory_context *context)
{
    char_array *result = NewArray(result, context);

    print_json(json, result);

    return result;
}
