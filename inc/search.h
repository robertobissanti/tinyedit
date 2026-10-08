/* search.h -- query ownership and text-only search results. */
#ifndef TE_SEARCH_H
#define TE_SEARCH_H

#include <regex.h>
#include <stddef.h>
#include <stdint.h>

struct editorBuffer;

struct searchMatch {
    int32_t start_y, start_x, end_y, end_x;
    int32_t length;
};

struct searchQuery {
    char *pattern;
    regex_t regex;
    uint8_t regex_mode, compiled, valid, cross_row_regex;
    char *text;
    size_t text_len;
};

/* Zero-initialize before use; free releases compiled regex and cached text. */
/** @brief Release owned pattern, compiled regex and joined text; query must be initialized. */
void searchQueryFree(struct searchQuery *query);
/* Reuses the compiled expression when pattern and mode are unchanged.
 * Invalid expressions are cached too and return 0 until the query changes. */
/** @brief Cache a copied pattern and optional compiled regex; return zero for invalid expressions. */
uint8_t searchQueryPrepare(struct searchQuery *query, const char *pattern, uint8_t regex_mode);
/* Call after source mutations, history restoration or replacing the buffer.
 * The compiled query remains reusable; only joined document text is discarded. */
/** @brief Release cached joined document bytes while retaining the compiled query. */
void searchInvalidateText(struct searchQuery *query);
/* Pure with respect to document/cursor/view. Outputs change only on success.
 * Coordinates are source bytes; dir is +/-1, wrap enables circular navigation.
 * Regex matches are non-overlapping, reverse bounds include the starting byte. */
/** @brief Find a match using source-byte coordinates; output changes only on success. */
uint8_t searchFind(struct searchQuery *query, const struct editorBuffer *buffer,
    int32_t from_y, int32_t from_x, int32_t dir, uint8_t wrap, struct searchMatch *result);

#endif
