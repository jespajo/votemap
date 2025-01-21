// For clock_gettime() we need _POSIX_C_SOURCE >= 199309L.
// For strerror_r()    we need _POSIX_C_SOURCE >= 200112L.
#define _POSIX_C_SOURCE 200112L

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
{
    return get_error_info(errno);
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

char_array2 *read_directory(char *dir_path, bool with_dir_prefix, Memory_context *context)
// Return an array of char_arrays with the names of the files in the directory.
// If with_dir_prefix is true, you get the full paths; otherwise you just get the filenames.
// dir_path shouldn't have a trailing '/'.
{
    char_array2 *paths = NewArray(paths, context);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        Fatal("Couldn't open directory %s (%s).", dir_path, get_last_error().string);
    }

    while (true) {
        struct dirent *dirent = readdir(dir);
        if (!dirent)  break;

        if (with_dir_prefix)  *Add(paths) = *get_string(context, "%s/%s", dir_path, dirent->d_name);
        else                  *Add(paths) = *get_string(context, "%s", dirent->d_name);

        // |Todo: Look at dirent->type and record whether it's a directory.
    }

    closedir(dir);

    return paths;
}

