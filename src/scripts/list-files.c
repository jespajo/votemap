#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "../strings.h"
#include "../system.h"

void recursively_add_file_names(char *top_dir_path, s64 top_path_length, string_array *files)
{
    // Remove the trailing '/' if there is one. opendir() doesn't like the trailing slash, but we want to
    // keep it in the output, so if we remove it, we'll re-add it (right at the bottom of this function).
    char *trailing_slash = &top_dir_path[top_path_length-1];
    if (*trailing_slash == '/')  *trailing_slash = '\0';
    else                          trailing_slash = NULL;

    DIR *dir = opendir(top_dir_path);
    if (!dir) {
        Fatal("Couldn't open directory %s: %s", top_dir_path, get_last_error().string);
    }

    while (true) {
        errno = 0;
        struct dirent *dirent = readdir(dir);
        if (!dirent && errno != 0) {
            Fatal("Couldn't read directory %s: %s", top_dir_path, get_last_error().string);
        } else if (!dirent) {
            // Since errno == 0, this means there are no more files to read.
            break;
        } else {
            if (!strcmp(dirent->d_name, "."))   continue;
            if (!strcmp(dirent->d_name, ".."))  continue;

            // The underscore is to reserve space for a trailing '/', which we'll add if the file is a directory.
            // |Cleanup: Not sure it's worth the trouble for these trailing slashes!
            char_array *name = get_string(files->context, "%s/%s_", top_dir_path, dirent->d_name);
            name->data[name->count-1] = '\0';

            struct stat info = {0};
            int r = stat(name->data, &info);
            if (r == -1) {
                Fatal("Couldn't stat file %s: %s", name->data, get_last_error().string);
            }

            int S_IFMT  = 0170000; //|Todo: #include these. Feature test macros?
            int S_IFDIR = 0040000;
            bool is_directory = (info.st_mode & S_IFMT) == S_IFDIR;

            *Add(files) = name->data;

            if (!is_directory)  continue;

            name->data[name->count-1] = '/';

            bool readable = (info.st_mode & S_IXOTH); //|Todo: This is conservative. We're only reading directories anyone can read.
            if (readable) {
                recursively_add_file_names(name->data, name->count, files);
            }
        }
    }

    int r = closedir(dir);
    if (r == -1) {
        Fatal("Couldn't close directory %s: %s", top_dir_path, get_last_error().string);
    }

    if (trailing_slash)  *trailing_slash = '/';
}

int main(int argc, char **argv)
{
    Memory_context *ctx = new_context(NULL);

    char *top_dir_name = (argc > 1) ? argv[1] : ".";

    string_array files = {.context = ctx};
    recursively_add_file_names(top_dir_name, strlen(top_dir_name), &files);

    for (s64 i = 0; i < files.count; i++)  puts(files.data[i]);

    free_context(ctx);
    return 0;
}
