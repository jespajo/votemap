#ifndef SYSTEM_H_INCLUDED
#define SYSTEM_H_INCLUDED

#include <errno.h> // So you have constants like EAGAIN etc.

#include "array.h"

typedef struct System_error System_error;

struct System_error {
    int  code;
    char string[256];
};

System_error get_error();
s64 get_monotonic_time();
void set_blocking(int file_no, bool blocking);

#endif // SYSTEM_H_INCLUDED
