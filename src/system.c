// For clock_gettime() we need _POSIX_C_SOURCE >= 199309L.
// For strerror_r()    we need _POSIX_C_SOURCE >= 200112L.
#define _POSIX_C_SOURCE 200112L

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>

#include "strings.h"
#include "system.h"

System_error get_error_info(int code)
{
    System_error error = {.code = code};

    bool ok = !strerror_r(code, error.string, sizeof(error.string));
    if (!ok) {
        snprintf(error.string, sizeof(error.string), "We couldn't get the system's error message");
    }

    return error;
}

System_error get_last_error()
// The idea is to use get_last_error() with classic C functions where they put the error code in errno.
// With stuff like pthreads where they return the error code instead, use get_error_info(code).
{
    return get_error_info(errno);
}

char_array *load_text_file(char *file_name, Memory_context *context)
// Return NULL if the file can't be opened.
{
    char_array *buffer = NewArray(buffer, context);

    FILE *file = fopen(file_name, "r");
    if (!file) {
        return NULL;
    }

    while (true) {
        int c = fgetc(file);
        if (c == EOF)  break;

        // |Memory: This doubles the buffer when needed.
        *Add(buffer) = (char)c;
    }

    *Add(buffer) = '\0';
    buffer->count -= 1;

    fclose(file);

    return buffer;
}

u8_array *load_binary_file(char *file_name, Memory_context *context)
// Return NULL if the file can't be opened.
{
    u8_array *buffer = NewArray(buffer, context);

    FILE *file = fopen(file_name, "rb");
    if (!file) {
        return NULL;
    }

    while (true) {
        int c = fgetc(file);
        if (c == EOF)  break;

        // |Memory: This doubles the buffer when needed.
        *Add(buffer) = (u8)c;
    }

    fclose(file);

    return buffer;
}

void write_array_to_file_(void *data, u64 unit_size, s64 count, char *file_name)
{
    if (!count)  Fatal("You probably don't want to write an empty array to %s.", file_name);

    FILE *file = fopen(file_name, "wb");
    if (!file) {
        Fatal("Couldn't create file %s (%s).", file_name, get_last_error().string);
    }

    u64 num_chars_written = fwrite(data, unit_size, count, file);
    assert(num_chars_written > 0);

    fclose(file);
}

s64 get_monotonic_time()
// In milliseconds.
{
    struct timespec time;
    bool ok = !clock_gettime(CLOCK_MONOTONIC, &time);
    if (!ok) {
        Fatal("clock_gettime failed (%s).", get_last_error().string);
    }

    s64 milliseconds = 1000*time.tv_sec + time.tv_nsec/1.0e6;

    return milliseconds;
}

void set_blocking(int file_no, bool blocking)
// file_no is an open file descriptor.
{
    int flags = fcntl(file_no, F_GETFL, 0);
    if (flags == -1) {
        Fatal("fcntl failed (%s).", get_last_error().string);
    }

    if (blocking)  flags &= ~O_NONBLOCK;
    else           flags |=  O_NONBLOCK;

    bool ok = !fcntl(file_no, F_SETFL, flags);
    if (!ok) {
        Fatal("fcntl failed (%s).", get_last_error().string);
    }
}

void recursively_add_file_names(char *top_path, s64 top_path_length, string_array *files)
// Add the paths of all files we can access under the directory at top_path. Don't follow symlinks.
// top_path can't have a trailing slash.
// top_path_length can be zero, in which case we'll use the current directory, but we won't put "./" at the start of the paths.
{
    if (top_path_length > 0)  assert(top_path[top_path_length-1] != '/');

    if (top_path_length == 0)  top_path = ".";

    DIR *dir = opendir(top_path);
    if (!dir) {
        Fatal("Couldn't open directory %s: %s", top_path, get_last_error().string);
    }

    while (true) {
        errno = 0;
        struct dirent *dirent = readdir(dir);
        if (!dirent && errno != 0) {
            Fatal("Couldn't read directory %s: %s", top_path, get_last_error().string);
        } else if (!dirent) {
            // Since errno == 0, this means there are no more files to read.
            break;
        } else {
            if (!strcmp(dirent->d_name, "."))   continue;
            if (!strcmp(dirent->d_name, ".."))  continue;

            char_array file_path = {.context = files->context};
            if (top_path_length > 0) {
                append_string(&file_path, "%s/%s", top_path, dirent->d_name);
            } else {
                append_string(&file_path, "%s", dirent->d_name);
            }

            *Add(files) = file_path.data;

            struct stat file_info = {0};
            int r = stat(file_path.data, &file_info);
            if (r == -1) {
                Fatal("Couldn't stat file %s: %s", file_path.data, get_last_error().string);
            }

            int S_IFMT  = 0170000; //|Cleanup: #include these octal constants. Feature test macros?
            int S_IFDIR = 0040000;

            bool is_directory = (file_info.st_mode & S_IFMT) == S_IFDIR;
            bool is_readable  = (file_info.st_mode & S_IXOTH); // This is conservative. We're only reading directories if anyone can read them.

            if (is_directory && is_readable) {
                recursively_add_file_names(file_path.data, file_path.count, files);
            }
        }
    }

    int r = closedir(dir);
    if (r == -1) {
        Fatal("Couldn't close directory %s: %s", top_path, get_last_error().string);
    }
}
