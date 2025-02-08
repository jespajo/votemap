#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>

#include "../strings.h"
#include "../system.h"

typedef struct File_node File_node;
typedef Array(File_node) File_node_array;

struct File_node {
    char_array      path;       // We need to construct the full path to stat the file, so we might as well keep it.
    char           *name;       // We don't make another copy for this; it's a pointer to the final segment of the .path array.
    enum {
        UNKNOWN_FILE_TYPE,
        REGULAR_FILE,
        DIRECTORY,
    }               type;
    File_node_array children;
};

static void fill_out_file_node(File_node *file_node, Memory_context *context)
// Given a File_node* that already has a .path and .name, add the other information, running recursively for directories.
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

void append_printed_file_tree(char_array *out, File_node *node, int depth)
{
    int indent = 4*depth;
    for (int i = 0; i < indent; i++)  *Add(out) = ' ';

    append_string(out, "%s", node->name);
    if (node->type == DIRECTORY)  *Add(out) = '/';
    append_string(out, "\n");

    for (s64 i = 0; i < node->children.count; i++) {
        append_printed_file_tree(out, &node->children.data[i], depth+1);
    }
}

int main(int argc, char **argv)
{
    Memory_context *ctx = new_context(NULL);

    char *root_path = ".";
    if (argc > 1)  root_path = argv[1];

    File_node *tree = get_file_tree(root_path, ctx);

    char_array out = {.context = ctx};
    append_printed_file_tree(&out, tree, 0);
    puts(out.data);

    free_context(ctx);
    return 0;
}
