/* tree.h -- owned, lazily expanded filesystem tree. */
#ifndef TE_TREE_H
#define TE_TREE_H
#include <stdint.h>

struct treeEntry {
    char *path;
    int32_t depth;
    uint8_t directory, expanded, symlink;
};
struct editorTree {
    struct treeEntry *entries;
    int32_t count, capacity, selected, scroll, measured_columns, width;
    uint8_t visible, focused, parent_selected;
};
/* Tree owns paths and entries. Borrowed entry pointers are invalidated by
 * expansion, collapse, root replacement or clear. Symlinks are leaves. */
/** @brief Release owned paths and entries; invalidate all borrowed entry pointers. */
void treeClear(struct editorTree *tree);
/** @brief Replace the tree with a copied root path; return zero preserving the tree on failure. */
uint8_t treeSetRoot(struct editorTree *tree, const char *path);
/** @brief Lazily add directory children; return zero on filesystem failure. Symlinks remain leaves. */
uint8_t treeExpand(struct editorTree *tree, int32_t index);
/** @brief Remove owned descendant entries at a file-entry index. */
void treeCollapse(struct editorTree *tree, int32_t index);
/** @brief Move selection by entries and adjust scrolling for visible row count. */
void treeMove(struct editorTree *tree, int32_t delta, int32_t rows);
/** @brief Compute sidebar width in screen columns, or zero when hidden or too narrow. */
int32_t treeWidth(struct editorTree *tree, int32_t columns);
#endif
