#ifndef SYSTEM_H_INCLUDED
#define SYSTEM_H_INCLUDED

#include <errno.h> // So you have constants like EAGAIN etc.

#include "array.h"

typedef struct System_error System_error;

struct System_error {
    int  code;
    char string[200];
};

System_error get_error_info(int code);
System_error get_last_error();
char_array *load_text_file(char *file_name, Memory_context *context);
u8_array *load_binary_file(char *file_name, Memory_context *context);
void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name);
s64 get_monotonic_time();
void set_blocking(int file_no, bool blocking);
void recursively_add_file_names(char *top_dir_path, s64 top_path_length, string_array *files);

#define write_array_to_file(ARRAY, FILE_NAME)  \
    write_array_to_file_((ARRAY)->data, sizeof((ARRAY)->data[0]), (ARRAY)->count, (FILE_NAME))

#endif // SYSTEM_H_INCLUDED
