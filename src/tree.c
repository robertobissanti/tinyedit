#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include "tree.h"
#include "alloc.h"
#include "fileio.h"
#include "utf8.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- tree ownership ----------------------------------------------------- */
/**
 * @brief Release owned paths and entries; invalidate all borrowed entry pointers.
 */
void treeClear(struct editorTree *tree) {
    for (int32_t i = 0; i < tree->count; i++) free(tree->entries[i].path);
    free(tree->entries);
    memset(tree, 0, sizeof(*tree));
}

/**
 * @brief Reserve entry capacity; structural growth invalidates borrowed entry pointers.
 */
static void treeReserve(struct editorTree *tree, int32_t needed) {
    if (needed <= tree->capacity) return;
    size_t capacity = teGrowCapacity((size_t)tree->capacity, (size_t)needed, INT32_MAX);
    tree->entries = teRealloc(tree->entries, teArrayBytes(capacity, sizeof(*tree->entries)));
    tree->capacity = (int32_t)capacity;
}

/**
 * @brief Sort borrowed entries with directories first and names in lexical order.
 */
static int treeCompare(const void *a, const void *b) {
    const struct treeEntry *left = a, *right = b;
    if (left->directory != right->directory) return left->directory ? -1 : 1;
    return strcmp(left->path, right->path);
}

/* ---- expansion ---------------------------------------------------------- */
/**
 * @brief Lazily add directory children; return zero on filesystem failure. Symlinks remain leaves.
 */
uint8_t treeExpand(struct editorTree *tree, int32_t index) {
    if (index < 0 || index >= tree->count) return 0;
    if (!tree->entries[index].directory || tree->entries[index].expanded) return 1;
    if (tree->entries[index].depth == INT32_MAX) { errno = EFBIG; return 0; }
    DIR *dir = opendir(tree->entries[index].path);
    if (!dir) return 0;
    struct editorTree children = {0};
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (children.count == INT32_MAX - tree->count) { errno = EFBIG; break; }
        const char *parent = tree->entries[index].path;
        size_t len = strlen(parent), name_len = strlen(entry->d_name);
        char *path = teMalloc(teSizeAdd(teSizeAdd(len, name_len), 2));
        memcpy(path, parent, len);
        path[len] = '/';
        memcpy(path + len + 1, entry->d_name, name_len + 1);
        struct stat st;
        if (lstat(path, &st) != 0) { free(path); errno = 0; continue; }
        treeReserve(&children, children.count + 1);
        children.entries[children.count++] = (struct treeEntry){path,
            tree->entries[index].depth + 1, S_ISDIR(st.st_mode), 0, S_ISLNK(st.st_mode)};
        errno = 0;
    }
    int saved = errno;
    closedir(dir);
    if (saved) { treeClear(&children); errno = saved; return 0; }
    if (children.count) {
        qsort(children.entries, (size_t)children.count, sizeof(*children.entries), treeCompare);
        treeReserve(tree, tree->count + children.count);
        memmove(tree->entries + index + 1 + children.count, tree->entries + index + 1,
            teArrayBytes((size_t)(tree->count - index - 1), sizeof(*tree->entries)));
        memcpy(tree->entries + index + 1, children.entries,
            teArrayBytes((size_t)children.count, sizeof(*tree->entries)));
        tree->count += children.count;
        if (tree->selected > index) tree->selected += children.count;
    }
    free(children.entries); /* Child paths transfer to the tree. */
    tree->entries[index].expanded = 1;
    tree->measured_columns = 0;
    return 1;
}

/**
 * @brief Remove owned descendant entries at a file-entry index.
 */
void treeCollapse(struct editorTree *tree, int32_t index) {
    if (index < 0 || index >= tree->count) return;
    int32_t end = index + 1;
    while (end < tree->count && tree->entries[end].depth > tree->entries[index].depth)
        free(tree->entries[end++].path);
    int32_t removed = end - index - 1;
    memmove(tree->entries + index + 1, tree->entries + end,
        teArrayBytes((size_t)(tree->count - end), sizeof(*tree->entries)));
    tree->count -= removed;
    tree->entries[index].expanded = 0;
    if (tree->selected >= end) tree->selected -= removed;
    else if (tree->selected > index) tree->selected = index;
    if (tree->scroll > tree->selected) tree->scroll = tree->selected;
    tree->measured_columns = 0;
}

/**
 * @brief Replace the tree with a copied root path; return zero preserving the tree on failure.
 */
uint8_t treeSetRoot(struct editorTree *tree, const char *path) {
    char *expanded = fileioExpandHomePath(path);
    if (!expanded) return 0;
    /* Like fileio/backup, resolution has a recoverable library allocation. */
    char *resolved = realpath(expanded, NULL);
    int saved = errno;
    free(expanded);
    if (!resolved) { errno = saved; return 0; }
    expanded = resolved;
    struct stat st;
    if (stat(expanded, &st) != 0) { free(expanded); return 0; }
    if (!S_ISDIR(st.st_mode)) { free(expanded); errno = ENOTDIR; return 0; }
    struct editorTree staged = {0};
    treeReserve(&staged, 1);
    staged.entries[0] = (struct treeEntry){expanded, 0, 1, 0, 0};
    staged.count = 1;
    if (!treeExpand(&staged, 0)) { int staged_errno = errno; treeClear(&staged); errno = staged_errno; return 0; }
    staged.visible = tree->visible;
    staged.focused = tree->focused;
    treeClear(tree);
    *tree = staged;
    return 1;
}

/* ---- viewport ----------------------------------------------------------- */
/**
 * @brief Move selection by entries and adjust scrolling for visible row count.
 */
void treeMove(struct editorTree *tree, int32_t delta, int32_t rows) {
    if (!tree->count) return;
    if (rows < 1) rows = 1;
    if (delta < -tree->selected) tree->selected = 0;
    else if (delta > tree->count - 1 - tree->selected) tree->selected = tree->count - 1;
    else tree->selected += delta;
    if (tree->scroll > tree->selected) tree->scroll = tree->selected;
    if (tree->selected - tree->scroll >= rows) tree->scroll = tree->selected - rows + 1;
}

/**
 * @brief Compute sidebar width in screen columns, or zero when hidden or too narrow.
 */
int32_t treeWidth(struct editorTree *tree, int32_t columns) {
    if (!tree->visible || columns < 40) return 0;
    if (tree->measured_columns == columns) return tree->width;
    int32_t limit = columns / 2;
    int32_t width = columns / 3;
    for (int32_t i = 0; i < tree->count; i++) {
        const char *label = strrchr(tree->entries[i].path, '/');
        label = label && label[1] ? label + 1 : tree->entries[i].path;
        size_t name = utf8StrWidth(label, strlen(label));
        int32_t depth = tree->entries[i].depth;
        if (name >= (size_t)limit || depth > limit / 2) { width = limit; break; }
        int32_t needed = (int32_t)name + depth * 2 + 3;
        if (needed > width) width = needed;
    }
    if (width > limit) width = limit;
    tree->measured_columns = columns;
    tree->width = width;
    return width;
}
