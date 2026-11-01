#include "smart_table.h"
#include "alloc.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

/** @brief Advance one source scalar so combining marks after syntax remain content. */
static int32_t tableNext(const erow *row, int32_t at) {
    return at + (int32_t)utf8DecodeChar(row->chars + at, (size_t)(row->size - at)).consumed;
}

/** @brief Parse a bounded outer-pipe row; return zero for ambiguous syntax. */
uint8_t smartTableParse(const erow *row, struct smartTableRow *out) {
    memset(out, 0, sizeof(*out));
    out->separator = out->empty = 1;
    if (row->size < 2 || row->size > SMART_TABLE_BYTES || row->chars[0] != '|') return 0;
    int32_t start = 1, code = 0, last = -1;
    for (int32_t at = 1; at < row->size;) {
        uint8_t c = (uint8_t)row->chars[at];
        int32_t next = tableNext(row, at);
        if (c == '\\' && !code) {
            at = next < row->size ? tableNext(row, next) : next;
            continue;
        }
        if (c == '`') {
            int32_t end = next;
            while (end < row->size && row->chars[end] == '`') end = tableNext(row, end);
            int32_t run = end - at;
            if (!code) code = run;
            else if (run == code) code = 0;
            at = end;
            continue;
        }
        if (c == '|' && !code) {
            if (out->count == SMART_TABLE_COLUMNS) return 0;
            struct smartTableCell *cell = &out->cells[out->count++];
            cell->open = start - 1; cell->close = at;
            cell->start = start; cell->end = at;
            while (cell->start < cell->end && row->chars[cell->start] == ' ') cell->start++;
            while (cell->end > cell->start && row->chars[cell->end - 1] == ' ') cell->end--;
            int32_t p = cell->start, end = cell->end;
            if (p < end && row->chars[p] == ':') { cell->left = 1; p++; }
            if (p < end && row->chars[end - 1] == ':') { cell->right = 1; end--; }
            int32_t dashes = 0;
            while (p < end && row->chars[p] == '-') { dashes++; p++; }
            cell->separator = p == end && dashes >= 1;
            if (!cell->separator) out->separator = 0;
            if (cell->end != cell->start) out->empty = 0;
            start = next; last = at;
        }
        at = next;
    }
    /* Trailing spaces and unmatched code delimiters are deliberately ambiguous. */
    return !code && last == row->size - 1 && out->count > 0;
}

/** @brief Find equal-width contiguous source rows without exceeding processing limits. */
uint8_t smartTableFind(const struct editorBuffer *buffer, int32_t y, struct smartTable *table) {
    struct smartTableRow parsed;
    if (y < 0 || y >= buffer->row_count || !smartTableParse(&buffer->rows[y], &parsed)) return 0;
    *table = (struct smartTable){y, y, parsed.count};
    size_t bytes = (size_t)buffer->rows[y].size;
    for (int32_t direction = -1; direction <= 1; direction += 2) {
        int32_t at = y + direction;
        while (at >= 0 && at < buffer->row_count && smartTableParse(&buffer->rows[at], &parsed)) {
            if (parsed.count != table->columns) return 0;
            bytes += (size_t)buffer->rows[at].size;
            if (bytes > SMART_TABLE_BYTES || table->last - table->first + 1 >= SMART_TABLE_ROWS) return 0;
            if (direction < 0) table->first = at; else table->last = at;
            at += direction;
        }
    }
    return 1;
}

/** @brief Measure cell content at an absolute display column, including literal tabs. */
static int32_t tableWidth(const erow *row, const struct smartTableCell *cell,
    int32_t column, int32_t tab_stop) {
    int32_t initial = column;
    for (int32_t at = cell->start; at < cell->end;) {
        int32_t next = at + (int32_t)utf8NextCharLen(row->chars, (size_t)at, (size_t)cell->end);
        column += row->chars[at] == '\t' ? tab_stop - column % tab_stop :
            utf8SingleCharWidth(row->chars + at, (size_t)(next - at));
        at = next;
    }
    return column - initial;
}

/** @brief Fill per-column display widths; return zero if output would exceed the limit. */
uint8_t smartTableWidths(const struct editorBuffer *buffer, const struct smartTable *table,
    int32_t tab_stop, int32_t *widths) {
    int32_t column = 2;
    struct smartTableRow parsed;
    for (int32_t c = 0; c < table->columns; c++) {
        widths[c] = 3;
        for (int32_t y = table->first; y <= table->last; y++) {
            smartTableParse(&buffer->rows[y], &parsed);
            struct smartTableCell *cell = &parsed.cells[c];
            int32_t width = y == table->first + 1 && parsed.separator ? 3 :
                tableWidth(&buffer->rows[y], cell, column, tab_stop);
            if (width > widths[c]) widths[c] = width;
        }
        if (widths[c] > SMART_TABLE_BYTES - column - 3) return 0;
        column += widths[c] + 3;
    }
    return 1;
}

/** @brief Translate a source position inside preserved content to its new byte offset. */
static void tableMap(int32_t *point, int32_t old_start, int32_t old_end, int32_t new_start) {
    if (!point || *point < old_start || *point > old_end) return;
    *point = new_start + *point - old_start;
}

/**
 * @brief Allocate a formatted row and map optional cursor/anchor source offsets.
 * @details parsed and widths must come from the validated table. align entries
 * are zero for left, two for right, three for centered. Caller frees the result.
 */
char *smartTableFormat(const erow *row, const struct smartTableRow *parsed,
    const int32_t *widths, const uint8_t *align, int32_t tab_stop, int32_t *cursor, int32_t *anchor) {
    size_t capacity = (size_t)row->size + 2;
    for (int32_t c = 0; c < parsed->count; c++) capacity += (size_t)widths[c] + 3;
    char *text = teMalloc(capacity);
    int32_t at = 0, old_cursor = cursor ? *cursor : -1, old_anchor = anchor ? *anchor : -1;
    text[at++] = '|';
    for (int32_t c = 0; c < parsed->count; c++) {
        const struct smartTableCell *cell = &parsed->cells[c];
        text[at++] = parsed->separator && cell->left ? ':' : ' ';
        int32_t begin = at;
        if (parsed->separator) {
            memset(text + at, '-', (size_t)widths[c]); at += widths[c];
        } else {
            int32_t width = tableWidth(row, cell, at, tab_stop);
            int32_t padding = widths[c] - width, leading = 0;
            /* Tab expansion depends on the absolute column. Pick only padding
             * that still fits, preserving literal tabs and all source bytes. */
            int32_t desired = align[c] == 2 ? padding : align[c] == 3 ? padding / 2 : 0;
            for (int32_t n = desired; n > 0 && n > desired - tab_stop; n--) {
                int32_t candidate = tableWidth(row, cell, at + n, tab_stop);
                if (n + candidate <= widths[c]) { leading = n; width = candidate; break; }
            }
            memset(text + at, ' ', (size_t)leading); at += leading;
            begin = at;
            memcpy(text + at, row->chars + cell->start, (size_t)(cell->end - cell->start));
            at += cell->end - cell->start;
            memset(text + at, ' ', (size_t)(widths[c] - width - leading)); at += widths[c] - width - leading;
        }
        int32_t left = cell->open + 1;
        int32_t right = cell->close;
        if (cursor && old_cursor >= left && old_cursor <= right) {
            *cursor = old_cursor < cell->start ? begin : old_cursor > cell->end ?
                begin + cell->end - cell->start : old_cursor;
            if (old_cursor >= cell->start && old_cursor <= cell->end) tableMap(cursor, cell->start, cell->end, begin);
        }
        if (anchor && old_anchor >= left && old_anchor <= right) {
            *anchor = old_anchor < cell->start ? begin : old_anchor > cell->end ?
                begin + cell->end - cell->start : old_anchor;
            if (old_anchor >= cell->start && old_anchor <= cell->end) tableMap(anchor, cell->start, cell->end, begin);
        }
        if (cursor && old_cursor >= left && old_cursor <= right && *cursor > at) *cursor = at;
        if (anchor && old_anchor >= left && old_anchor <= right && *anchor > at) *anchor = at;
        text[at++] = parsed->separator && cell->right ? ':' : ' '; text[at++] = '|';
    }
    text[at] = '\0';
    if (cursor && old_cursor == row->size) *cursor = at;
    if (anchor && old_anchor == row->size) *anchor = at;
    return text;
}
