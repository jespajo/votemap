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

static int compare_file_nodes(void const *ptr1, void const *ptr2)
// This is to qsort() or bsearch() File_nodes. It puts them in lexicographic order based on their .name members.
// (Not quite alphabetical order: 'B' comes before 'a'.)
{
    char *name1 = ((File_node*)ptr1)->name;
    char *name2 = ((File_node*)ptr2)->name;

    //
    // The rest of this function is like returning strcmp(name1, name2), except if one string reaches a null byte when the
    // other one is at a '/' character, we'll treat them as identical. This is cooked up to work with find_file_node().
    // The name that ends in a null byte belongs to a real node and the one ending in '/' is a fake node that we are using
    // as a key for bsearch_array(). The fake node's name is really a pointer to a segment of the path string passed to
    // find_file_node(). Doing it this way means we don't have to make a copy of each segment of the path just to have it
    // end with a null byte. We can't temporarily modify the path string because it might be a string literal. So this is
    // hacky for sure but hopefully fine since real file names can't have '/' characters.
    //
    while (*name1 && *name2) {
        if (*name1 != *name2)  break;

        name1 += 1;
        name2 += 1;
    }

    if (*name1 == *name2)  return 0;
    if (*name1 == '/')     return 0;
    if (*name2 == '/')     return 0;

    return *(u8*)name1 - *(u8*)name2;
}

static void fill_out_file_node(File_node *file_node, Memory_context *context)
// Given a File_node* that already has a path and name, add the file type and, if it's a directory,
// run recursively on any files in the directory. Sort child nodes by file name.
{
    assert(file_node->path.data[file_node->path.count-1] != '/');

    struct stat file_info = {0};
    int r = stat(file_node->path.data, &file_info);
    if (r == -1) {
        Fatal("Couldn't stat file %s: %s", file_node->path.data, get_last_error().string);
    }

    int S_IFMT  = 0170000; //|Cleanup: #include these octal constants. Feature test macros?
    int S_IFDIR = 0040000;
    int S_IFREG = 0100000;

    bool is_regular_file = (file_info.st_mode & S_IFMT) == S_IFREG;
    bool is_directory    = (file_info.st_mode & S_IFMT) == S_IFDIR;

    if (is_regular_file)    file_node->type = REGULAR_FILE;
    else if (is_directory)  file_node->type = DIRECTORY;
    else                    file_node->type = UNKNOWN_FILE_TYPE;

    if (!is_directory)  return;

    // This is conservative. We'll only read directories if anyone can read them.
    bool is_readable = (file_info.st_mode & S_IXOTH);
    if (!is_readable)  return;

    file_node->children = (File_node_array){.context = context};

    DIR *dir = opendir(file_node->path.data);
    if (!dir) {
        Fatal("Couldn't open directory %s: %s", file_node->path.data, get_last_error().string);
    }

    while (true) {
        errno = 0;
        struct dirent *dirent = readdir(dir);
        if (!dirent && errno != 0) {
            Fatal("Couldn't read directory %s: %s", file_node->path.data, get_last_error().string);
        } else if (!dirent) {
            // Since errno == 0, this means there are no more files to read.
            break;
        } else {
            if (!strcmp(dirent->d_name, "."))   continue;
            if (!strcmp(dirent->d_name, ".."))  continue;

            File_node child = {0};

            child.path = get_string(context, "%s/%s", file_node->path.data, dirent->d_name);
            child.name = &child.path.data[file_node->path.count+1];
            assert(!strcmp(child.name, dirent->d_name)); //|Temporary

            *Add(&file_node->children) = child;
        }
    }

    r = closedir(dir);
    if (r == -1) {
        Fatal("Couldn't close directory %s: %s", file_node->path.data, get_last_error().string);
    }

    qsort_array(&file_node->children, compare_file_nodes);

    for (s64 i = 0; i < file_node->children.count; i++) {
        fill_out_file_node(&file_node->children.data[i], context);
    }
}

File_node *get_file_tree(char *path, Memory_context *context)
{
    File_node *root = New(File_node, context);

    // If the path has a trailing slash, don't include it in the copy on the struct.
    int length = strlen(path);
    if (path[length-1] == '/')  length -= 1;

    root->path = copy_string(path, length, context);
    root->name = &root->path.data[0];

    fill_out_file_node(root, context);

    return root;
}

void print_file_tree(char_array *out, File_node *node, int depth)
{
    int indent = 4*depth;
    for (int i = 0; i < indent; i++)  *Add(out) = ' ';

    append_string(out, "%s", node->name);
    if (node->type == DIRECTORY)  *Add(out) = '/';
    append_string(out, "\n");

    for (s64 i = 0; i < node->children.count; i++) {
        print_file_tree(out, &node->children.data[i], depth+1);
    }
}

File_node *find_file_node(char *path, File_node *root)
// Find a file node in a tree. The path is relative to the root---so if the root is /tmp/ and you're
// looking for /tmp/abc/def, the path should be "abc/def". You can put a slash at the end ("abc/def/")
// only if the target is a directory. We don't allow double slashes. If the path is the empty string,
// we'll return the root node.
{
    File_node *node = root;

    s64 i = 0;

    while (node) {
        if (path[i] == '/') {
            if (node->type == DIRECTORY)  i += 1;
        }
        if (path[i] == '\0')  break;

        s64 j = i;
        while (path[j] != '\0' && path[j] != '/')  j += 1;

        // See the comment in compare_file_nodes() to understand what is happening here:
        node = bsearch_array(&(File_node){.name=&path[i]}, &node->children, compare_file_nodes);

        i = j;
    }

    return node;
}
