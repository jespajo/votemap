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
s64 get_monotonic_time();
void set_blocking(int file_no, bool blocking);
char_array2 *read_directory(char *dir_path, bool with_dir_prefix, Memory_context *context);

#endif // SYSTEM_H_INCLUDED
