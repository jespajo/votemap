#ifndef SYSTEM_H_INCLUDED
#define SYSTEM_H_INCLUDED

#include <errno.h> // So everyone who #includes "system.h" gets errno and constants like EAGAIN.

#include "array.h"

typedef struct System_error System_error;
typedef struct File_node    File_node;
typedef Array(File_node)    File_node_array;

struct System_error {
    int  code;
    char string[200];
};

struct File_node {
    char_array      path;       // The full path to the file, starting with the path passed to get_file_tree().
    char           *name;       // A pointer to the last segment of the path above.
    enum {
        UNKNOWN_FILE_TYPE,
        REGULAR_FILE,
        DIRECTORY,
    }               type;
    File_node_array children;
};

System_error get_error_info(int code);
System_error get_last_error();
char_array *load_text_file(char *file_name, Memory_context *context);
u8_array *load_binary_file(char *file_name, Memory_context *context);
void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name);
s64 get_monotonic_time();
void set_blocking(int file_no, bool blocking);
File_node *get_file_tree(char *path, Memory_context *context);
void print_file_tree(char_array *out, File_node *node, int depth);
File_node *find_file_node(char *path, File_node *root);

#define write_array_to_file(ARRAY, FILE_NAME)  \
    write_array_to_file_((ARRAY)->data, sizeof((ARRAY)->data[0]), (ARRAY)->count, (FILE_NAME))

#endif // SYSTEM_H_INCLUDED
