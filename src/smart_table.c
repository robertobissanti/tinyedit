#include "smart_table.h"
#include "alloc.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

/* ---- source parsing -------------------------------------------------- */

/** @brief Advance one source scalar so combining marks after syntax remain content. */
static int32_t tableNext(const erow *row, int32_t at) {
    return at + (int32_t)utf8DecodeChar(row->chars + at, (size_t)(row->size - at)).consumed;
}

/** @brief Find the preceding scalar within the previous grapheme boundary. */
static int32_t tablePrev(const erow *row, int32_t at) {
    int32_t next = at - (int32_t)utf8PrevCharLen(row->chars, (size_t)at);
    int32_t previous = next;
    while (next < at) { previous = next; next = tableNext(row, next); }
    return previous;
}

/** @brief Parse a bounded outer-pipe row; return zero for ambiguous syntax. */
uint8_t smartTableParse(const erow *row, struct smartTableRow *out) {
    memset(out, 0, sizeof(*out));
    out->separator = out->empty = 1;
    if (row->size < 2 || row->size > SMART_TABLE_BYTES || row->chars[0] != '|') return 0;
    int32_t start = tableNext(row, 0), open = 0, code = 0, last = -1;
    for (int32_t at = start; at < row->size;) {
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
            cell->open = open; cell->close = at;
            cell->start = start; cell->end = at;
            while (cell->start < cell->end && row->chars[cell->start] == ' ')
                cell->start = tableNext(row, cell->start);
            while (cell->end > cell->start) {
                int32_t previous = tablePrev(row, cell->end);
                if (row->chars[previous] != ' ') break;
                cell->end = previous;
            }
            int32_t p = cell->start, end = cell->end;
            if (p < end && row->chars[p] == ':') { cell->left = 1; p = tableNext(row, p); }
            if (p < end) {
                int32_t previous = tablePrev(row, end);
                if (row->chars[previous] == ':') { cell->right = 1; end = previous; }
            }
            int32_t dashes = 0;
            while (p < end && row->chars[p] == '-') { dashes++; p = tableNext(row, p); }
            cell->separator = p == end && dashes >= 1;
            if (!cell->separator) out->separator = 0;
            if (cell->end != cell->start) out->empty = 0;
            start = next; open = last = at;
        }
        at = next;
    }
    /* Trailing spaces and unmatched code delimiters are deliberately ambiguous. */
    return !code && last == tablePrev(row, row->size) && out->count > 0;
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

/* ---- display widths -------------------------------------------------- */

/** @brief Measure cell content at an absolute display column, including literal tabs. */
static int32_t tableWidth(const erow *row, const struct smartTableSpan *cell,
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
    int32_t count = table->last - table->first + 1;
    if (count < 1 || count > SMART_TABLE_ROWS || table->columns < 1 ||
        table->columns > SMART_TABLE_COLUMNS || tab_stop < 1 || tab_stop > 16) return 0;
    size_t cells = teArrayBytes((size_t)count, (size_t)table->columns);
    struct smartTableSpan *spans = teMalloc(teArrayBytes(cells, sizeof(*spans)));
    struct smartTableRow parsed;
    uint8_t separator = 0, valid = 1;
    for (int32_t r = 0; r < count; r++) {
        if (!smartTableParse(&buffer->rows[table->first + r], &parsed) || parsed.count != table->columns) {
            free(spans); return 0;
        }
        if (r == 1) separator = parsed.separator;
        for (int32_t c = 0; c < table->columns; c++)
            spans[r * table->columns + c] =
                (struct smartTableSpan){parsed.cells[c].start, parsed.cells[c].end};
    }
    int32_t column = 2;
    for (int32_t c = 0; c < table->columns; c++) {
        widths[c] = 3;
        for (int32_t r = 0; r < count; r++) {
            const struct smartTableSpan *cell = &spans[r * table->columns + c];
            int32_t width = r == 1 && separator ? 3 :
                tableWidth(&buffer->rows[table->first + r], cell, column, tab_stop);
            if (width > widths[c]) widths[c] = width;
        }
        if (widths[c] > SMART_TABLE_BYTES - column - 3) { valid = 0; break; }
        column += widths[c] + 3;
    }
    free(spans);
    return valid;
}

/* ---- row formatting -------------------------------------------------- */

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
    if (tab_stop < 1 || tab_stop > 16 || row->size < 0 || row->size > SMART_TABLE_BYTES ||
        parsed->count < 1 || parsed->count > SMART_TABLE_COLUMNS) return NULL;
    size_t capacity = teSizeAdd((size_t)row->size, 2);
    int32_t column = 2;
    for (int32_t c = 0; c < parsed->count; c++) {
        if (widths[c] < 3 || widths[c] > SMART_TABLE_BYTES - column - 3) return NULL;
        capacity = teSizeAdd(capacity, (size_t)widths[c] + 3);
        column += widths[c] + 3;
    }
    column = 2;
    char *text = teMalloc(capacity);
    int32_t at = 0, old_cursor = cursor ? *cursor : -1, old_anchor = anchor ? *anchor : -1;
    int32_t mapped_cursor = old_cursor, mapped_anchor = old_anchor;
    int32_t *cursor_out = cursor, *anchor_out = anchor;
    cursor = cursor ? &mapped_cursor : NULL;
    anchor = anchor ? &mapped_anchor : NULL;
    text[at++] = '|';
    for (int32_t c = 0; c < parsed->count; c++) {
        const struct smartTableCell *cell = &parsed->cells[c];
        text[at++] = parsed->separator && cell->left ? ':' : ' ';
        int32_t begin = at;
        if (parsed->separator) {
            memset(text + at, '-', (size_t)widths[c]); at += widths[c];
        } else {
            struct smartTableSpan span = {cell->start, cell->end};
            int32_t width = tableWidth(row, &span, column, tab_stop);
            if (width > widths[c]) { free(text); return NULL; }
            int32_t padding = widths[c] - width, leading = 0;
            /* Tab expansion depends on the absolute column. Pick only padding
             * that still fits, preserving literal tabs and all source bytes. */
            int32_t desired = align[c] == SMART_TABLE_RIGHT ? padding : align[c] == SMART_TABLE_CENTER ? padding / 2 : 0;
            for (int32_t n = desired; n > 0 && n > desired - tab_stop; n--) {
                int32_t candidate = tableWidth(row, &span, column + n, tab_stop);
                if (n + candidate <= widths[c]) { leading = n; width = candidate; break; }
            }
            if (leading < 0 || widths[c] - width - leading < 0) { free(text); return NULL; }
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
        column += widths[c] + 3;
    }
    text[at] = '\0';
    if (cursor && old_cursor == row->size) *cursor = at;
    if (anchor && old_anchor == row->size) *anchor = at;
    if (cursor_out) *cursor_out = mapped_cursor;
    if (anchor_out) *anchor_out = mapped_anchor;
    return text;
}
