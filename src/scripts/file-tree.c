#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>

#include "../strings.h"
#include "../system.h"

typedef struct File_node File_node;
typedef Array(File_node) File_node_array;

struct File_node {
    char_array      path;       // The full path to the file, starting with whatever path was passed to get_file_tree().
    char           *name;       // Not a unique copy but a pointer to the last segment of the path member above.
    enum {
        UNKNOWN_FILE_TYPE,
        REGULAR_FILE,
        DIRECTORY,
    }               type;
    File_node_array children;
};

int compare_file_names(void const *ptr1, void const *ptr2)
// This is to qsort() or bsearch() File_nodes. It puts them in lexicographic order based on their .name members.
// It's not quite alphabetical order: 'B' comes before 'a'.
{
    File_node *node1 = (File_node*)ptr1;
    File_node *node2 = (File_node*)ptr2;

    return strcmp(node1->name, node2->name);
}

// |Todo: Move to array.h.
#define qsort_array(ARRAY, COMPARE_FUNCTION) \
    qsort((ARRAY)->data, (ARRAY)->count, sizeof((ARRAY)->data[0]), (COMPARE_FUNCTION))
// |Todo: Move to array.h, document usage gotchas.
#define bsearch_array(KEY, ARRAY, COMPARE_FUNCTION) \
    bsearch((KEY), (ARRAY)->data, (ARRAY)->count, sizeof((ARRAY)->data[0]), (COMPARE_FUNCTION))

static void fill_out_file_node(File_node *file_node, Memory_context *context)
// Given a File_node* that already has a .path and .name, add the other information, running recursively for directories.
// Child file nodes are sorted by name.
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

    qsort_array(&file_node->children, compare_file_names);

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

        // Temporarily modify the path to get a zero-terminated string we can use for searching.
        // We'll make sure to put it back the way it was.
        char tmp = path[j];
        path[j] = '\0';
        node = bsearch_array(&(File_node){.name=&path[i]}, &node->children, compare_file_names);
        path[j] = tmp;

        i = j;
    }

    return node;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  Print a directory tree:\n");
        printf("    %s <directory>\n", argv[0]);
        printf("  Look for a file in a directory:\n");
        printf("    %s <directory> <file path>\n", argv[0]);
        return 0;
    }

    Memory_context *ctx = new_context(NULL);

    char *root_path = argv[1];
    File_node *tree = get_file_tree(root_path, ctx);

    if (argc == 2) {
        char_array out = {.context = ctx};
        append_printed_file_tree(&out, tree, 0);
        puts(out.data);
        return 0;
    }

    char *file_path = argv[2];
    File_node *node = find_file_node(file_path, tree);

    if (!node) {
        puts("We couldn't find that file.");
    } else {
        printf("We found that file!\n");
        printf("  path: %s\n", node->path.data);
        printf("  type: ");
        if (node->type == REGULAR_FILE)    printf("regular file\n");
        else if (node->type == DIRECTORY)  printf("directory\n");
        else                               printf("unknown\n");
    }

    free_context(ctx);
    return 0;
}
