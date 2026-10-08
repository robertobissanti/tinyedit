#include "search.h"
#include "alloc.h"
#include "tinyedit.h"
#include "utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- query lifetime -------------------------------------------------- */

/**
 * @brief Release cached joined document bytes while retaining the compiled query.
 */
void searchInvalidateText(struct searchQuery *query) {
    free(query->text);
    query->text = NULL;
    query->text_len = 0;
}

/**
 * @brief Release owned pattern, compiled regex and joined text; query must be initialized.
 */
void searchQueryFree(struct searchQuery *query) {
    if (query->compiled) regfree(&query->regex);
    free(query->pattern);
    searchInvalidateText(query);
    memset(query, 0, sizeof(*query));
}

/**
 * @brief Decode search escapes into caller-owned text.
 */
static char *searchDecodePattern(const char *raw) {
    size_t len = strlen(raw);
    char *decoded = teMalloc(len + 1);
    size_t dst = 0;
    for (size_t src = 0; src < len; src++) {
        if (raw[src] == '\\' && src + 1 < len) {
            char next = raw[src + 1];
            /* Preserve \\ exactly. In particular, the regex \\n means
             * a literal backslash followed by n, not an actual newline. */
            if (next == '\\') {
                decoded[dst++] = raw[src++];
                decoded[dst++] = raw[src];
                continue;
            }
            if (next == 't' || next == 'n' || next == 'r') {
                src++;
                /* The document normalizes CRLF and LF to the same logical
                 * row boundary, so both regex spellings denote that boundary. */
                decoded[dst++] = next == 't' ? '\t' : next == 'n' ? '\n' : '\n';
                continue;
            }
        }
        decoded[dst++] = raw[src];
    }
    decoded[dst] = '\0';
    return decoded;
}

/**
 * @brief Cache a copied pattern and optional compiled regex; return zero for invalid expressions.
 */
uint8_t searchQueryPrepare(struct searchQuery *query, const char *pattern, uint8_t regex_mode) {
    if (query->pattern && query->regex_mode == regex_mode && strcmp(query->pattern, pattern) == 0)
        return query->valid;
    char *owned_pattern = teStrdup(pattern);
    searchQueryFree(query);
    query->pattern = owned_pattern;
    query->regex_mode = regex_mode;
    query->valid = 1;
    if (regex_mode && *query->pattern) {
        char *decoded = searchDecodePattern(query->pattern);
        query->cross_row_regex = strchr(decoded, '\n') != NULL;
        query->compiled = regcomp(&query->regex, decoded, REG_EXTENDED | REG_NEWLINE) == 0;
        query->valid = query->compiled;
        free(decoded);
    }
    return query->valid;
}

/* ---- document coordinates ------------------------------------------- */

/**
 * @brief Prepare query-owned joined document bytes for cross-row matching.
 */
static char *searchDocumentText(const struct editorBuffer *buffer, size_t *out_len) {
    size_t total = 0;
    for (int32_t y = 0; y < buffer->row_count; y++) {
        size_t bytes = (size_t)buffer->rows[y].size + (y + 1 < buffer->row_count ? 1 : 0);
        if (bytes > SIZE_MAX - total - 1) {
            errno = ENOMEM;
            perror("tinyedit: search text size");
            exit(EXIT_FAILURE);
        }
        total += bytes;
    }
    char *text = teMalloc(total + 1);
    char *dst = text;
    for (int32_t y = 0; y < buffer->row_count; y++) {
        erow *row = &buffer->rows[y];
        memcpy(dst, row->chars, (size_t)row->size);
        dst += row->size;
        if (y + 1 < buffer->row_count) *dst++ = '\n';
    }
    *dst = '\0';
    *out_len = total;
    return text;
}

/**
 * @brief Convert source row and byte coordinates to a joined-text byte offset.
 */
static size_t searchOffsetForPosition(const struct editorBuffer *buffer, int32_t y, int32_t x) {
    size_t offset = 0;
    for (int32_t row = 0; row < y; row++)
        offset += (size_t)buffer->rows[row].size + 1;
    return offset + (size_t)x;
}

/**
 * @brief Translate a search-text offset into a document position.
 *
 * @details Requires a nonempty document. Writes a logical row and source-byte
 * offset, clamping excess offsets to the document end.
 */
static void searchPositionForOffset(const struct editorBuffer *buffer, size_t offset, int32_t *out_y, int32_t *out_x) {
    for (int32_t y = 0; y < buffer->row_count; y++) {
        int32_t size = buffer->rows[y].size;
        if (offset <= (size_t)size) {
            *out_y = y;
            *out_x = (int32_t)offset;
            return;
        }
        offset -= (size_t)size + 1;
    }
    *out_y = buffer->row_count - 1;
    *out_x = buffer->rows[*out_y].size;
}

/* ---- matching ------------------------------------------------------- */

/**
 * @brief Find the last non-overlapping regex match within byte bounds.
 */
static uint8_t searchRegexLast(const regex_t *re, const char *text,
    size_t len, size_t limit, size_t *out_start, size_t *out_len) {
    uint8_t found = 0;
    size_t search_from = 0;
    while (search_from <= len) {
        regmatch_t m;
        if (regexec(re, text + search_from, 1, &m,
                search_from > 0 && text[search_from - 1] != '\n' ? REG_NOTBOL : 0) != 0)
            break;
        size_t start = search_from + (size_t)m.rm_so;
        size_t match_len = (size_t)(m.rm_eo - m.rm_so);
        if (start > limit) break;
        *out_start = start;
        *out_len = match_len;
        found = 1;
        if (match_len == 0 && start == len) break;
        size_t advance = match_len ? match_len : utf8NextCharLen(text, start, len);
        search_from = start + (advance ? advance : 1);
    }
    return found;
}

/**
 * @brief Find a match using source-byte coordinates; output changes only on success.
 */
uint8_t searchFind(struct searchQuery *query, const struct editorBuffer *buffer,
    int32_t from_y, int32_t from_x, int32_t dir, uint8_t wrap, struct searchMatch *result) {
    if (!query->valid || !query->pattern || !*query->pattern || buffer->row_count == 0) return 0;
    if (from_y < 0 || from_y >= buffer->row_count) {
        if (!wrap) return 0;
        if (from_y < 0 && dir == 1) {
            from_y = 0;
            from_x = 0;
        } else {
            from_y = buffer->row_count - 1;
            from_x = buffer->rows[from_y].size;
        }
    }
    if (from_x < 0) return 0;
    size_t qlen = strlen(query->pattern);
    if (qlen > INT32_MAX) return 0;
    if (query->cross_row_regex) {
        if (!query->text) query->text = searchDocumentText(buffer, &query->text_len);
        size_t text_len = query->text_len;
        const char *text = query->text;
        size_t from = searchOffsetForPosition(buffer, from_y, from_x);
        if (from > text_len) {
            if (!wrap) return 0;
            from = dir == 1 ? 0 : text_len;
        }
        size_t start = 0, match_len = 0;
        uint8_t found = 0;

        if (dir == 1) {
            regmatch_t m;
            if (regexec(&query->regex, text + from, 1, &m, from > 0 && text[from - 1] != '\n' ? REG_NOTBOL : 0) == 0) {
                start = from + (size_t)m.rm_so;
                match_len = (size_t)(m.rm_eo - m.rm_so);
                found = 1;
            }
            if (!found && wrap && regexec(&query->regex, text, 1, &m, 0) == 0) {
                start = (size_t)m.rm_so;
                match_len = (size_t)(m.rm_eo - m.rm_so);
                found = 1;
            }
        } else {
            found = searchRegexLast(&query->regex, text, text_len, from, &start, &match_len);
            if (!found && wrap)
                found = searchRegexLast(&query->regex, text, text_len, text_len, &start, &match_len);
        }

        if (found && match_len <= INT32_MAX) {
            searchPositionForOffset(buffer, start, &result->start_y, &result->start_x);
            result->length = (int32_t)match_len;
            searchPositionForOffset(buffer, start + match_len, &result->end_y, &result->end_x);
        } else found = 0;
        return found;
    }

    int32_t y = from_y;
    int32_t x = from_x;
    uint8_t found = 0;

    for (int32_t steps = 0; ; steps++) {
        erow *row = &buffer->rows[y];
        int32_t mx = -1, mlen = 0;

        if (query->regex_mode) {
            if (dir == 1) {
                if (x <= row->size) {
                    regmatch_t m;
                    if (regexec(&query->regex, &row->chars[x], 1, &m, x > 0 ? REG_NOTBOL : 0) == 0) {
                        mx = x + (int32_t)m.rm_so;
                        mlen = (int32_t)(m.rm_eo - m.rm_so);
                    }
                }
            } else {
                size_t match_start = 0, match_length = 0;
                if (x >= 0 && searchRegexLast(&query->regex, row->chars,
                        (size_t)row->size, (size_t)x, &match_start, &match_length)) {
                    mx = (int32_t)match_start;
                    mlen = (int32_t)match_length;
                }
            }
        } else if (dir == 1) {
            if (x <= row->size) {
                char *match = strstr(&row->chars[x], query->pattern);
                if (match) { mx = (int32_t)(match - row->chars); mlen = (int32_t)qlen; }
            }
        } else {
            /* Backward: scan for the last match starting at or before
             * column x on this row. */
            int32_t limit = x;
            if (limit > row->size - (int32_t)qlen) limit = row->size - (int32_t)qlen;
            size_t scan = 0;
            while (limit >= 0 && scan <= (size_t)limit) {
                const char *match = strstr(row->chars + scan, query->pattern);
                if (!match || match - row->chars > limit) break;
                mx = (int32_t)(match - row->chars);
                size_t advance = utf8NextCharLen(row->chars, (size_t)mx, (size_t)row->size);
                scan = (size_t)mx + (advance ? advance : 1);
            }
            if (mx >= 0) mlen = (int32_t)qlen;
        }

        if (mx >= 0) {
            result->start_y = y;
            result->start_x = mx;
            result->length = mlen;
            result->end_y = y;
            result->end_x = mx + mlen;
            found = 1;
            break;
        }

        if (steps == buffer->row_count) break;
        if (dir == 1) {
            if (!wrap && y == buffer->row_count - 1) break;
            y = (y + 1) % buffer->row_count;
            x = 0;
        } else {
            if (!wrap && y == 0) break;
            y = (y - 1 + buffer->row_count) % buffer->row_count;
            x = buffer->rows[y].size;
        }
    }

    return found;
}
