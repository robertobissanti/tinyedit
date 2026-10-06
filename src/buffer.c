#include "buffer.h"

#include "alloc.h"
#include "history.h"
#include "utf8.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Convert a source-byte cursor offset into a display column.
 *
 * @details cx must be a valid boundary within row and tab_stop must be
 * positive. Counts graphemes and expanded tabs; returns the zero-based column.
 */
int32_t bufferRowCxToRx(const erow *row, int32_t cx, int32_t tab_stop) {
    int32_t rx = 0, pos = 0;
    while (pos < cx) {
        if (row->chars[pos] == '\t') {
            rx += tab_stop - (rx % tab_stop);
            pos++;
        } else {
            size_t len = utf8NextCharLen(row->chars, (size_t)pos, (size_t)row->size);
            if (len == 0) len = 1;
            rx += utf8SingleCharWidth(row->chars + pos, len);
            pos += (int32_t)len;
        }
    }
    return rx;
}

/**
 * @brief Find a source-byte cursor position for a display column.
 *
 * @details tab_stop must be positive.
 * @return the start of a glyph containing target_rx, or the row end when the
 * target is beyond it.
 */
int32_t bufferRowRxToCx(const erow *row, int32_t target_rx, int32_t tab_stop) {
    int32_t rx = 0, pos = 0;
    if (target_rx <= 0) return 0;
    while (pos < row->size) {
        int32_t width;
        size_t len;
        if (row->chars[pos] == '\t') {
            width = tab_stop - (rx % tab_stop);
            len = 1;
        } else {
            len = utf8NextCharLen(row->chars, (size_t)pos, (size_t)row->size);
            if (len == 0) len = 1;
            width = utf8SingleCharWidth(row->chars + pos, len);
        }
        if (rx + width > target_rx) break;
        rx += width;
        pos += (int32_t)len;
    }
    return pos;
}

/**
 * @brief Insert a copy of a byte span as a logical row.
 *
 * @details at is in [0, row_count] and text contains len bytes without a line
 * ending. New display caches start empty; existing row pointers may be
 * invalidated.
 */
void bufferInsertRow(struct editorBuffer *buffer, int32_t at, const char *text, size_t len) {
    if (at < 0 || at > buffer->row_count) return;
    if (historyInsertRow(buffer, at, text, len)) return;
    if (len > INT32_MAX || buffer->row_count == INT32_MAX) {
        errno = ENOMEM; perror("tinyedit: buffer size"); exit(EXIT_FAILURE);
    }
    if (buffer->capacity <= buffer->row_count) {
        int32_t capacity = buffer->capacity > 0 ? buffer->capacity : 1;
        capacity = capacity <= INT32_MAX / 2 ? capacity * 2 : INT32_MAX;
        if ((size_t)capacity > SIZE_MAX / sizeof(*buffer->rows)) {
            errno = ENOMEM; perror("tinyedit: buffer size"); exit(EXIT_FAILURE);
        }
        buffer->rows = teRealloc(buffer->rows, sizeof(*buffer->rows) * (size_t)capacity);
        buffer->capacity = capacity;
    }
    memmove(&buffer->rows[at + 1], &buffer->rows[at],
        sizeof(erow) * (size_t)(buffer->row_count - at));
    erow *row = &buffer->rows[at];
    memset(row, 0, sizeof(*row));
    row->owner = buffer;
    row->chars_capacity = len + 1;
    row->size = (int32_t)len;
    row->chars = teMalloc(len + 1);
    memcpy(row->chars, text, len);
    row->chars[len] = '\0';
    row->seg_wrapcols = -1;
    buffer->row_count++;
}

/**
 * @brief Release all text and display allocations owned by a row.
 *
 * @details Does not remove the row from its buffer or reset its pointers. The
 * caller must avoid freeing the same row twice.
 */
void bufferFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
    free(row->seg_start);
    free(row->seg_start_rx);
}

/**
 * @brief Remove a logical row and release its owned storage.
 *
 * @details at is zero-based; invalid indices are ignored. Later rows shift, so
 * previously saved row pointers may no longer identify the same row.
 */
void bufferDeleteRow(struct editorBuffer *buffer, int32_t at) {
    if (at < 0 || at >= buffer->row_count) return;
    if (historyDeleteRow(buffer, at)) return;
    bufferFreeRow(&buffer->rows[at]);
    memmove(&buffer->rows[at], &buffer->rows[at + 1],
        sizeof(erow) * (size_t)(buffer->row_count - at - 1));
    buffer->row_count--;
}

/**
 * @brief Release every row and return a buffer to its empty state.
 *
 * @details Safe for an initialized empty buffer. Resets rows and row_count;
 * surrounding document metadata is unchanged.
 */
void bufferClear(struct editorBuffer *buffer) {
    for (int32_t i = 0; i < buffer->row_count; i++) bufferFreeRow(&buffer->rows[i]);
    free(buffer->rows);
    buffer->rows = NULL;
    buffer->row_count = 0;
    buffer->capacity = 0;
    buffer->history = NULL;
}

/**
 * @brief Insert a single raw byte into a row.
 *
 * @details at is a source-byte offset; byte is not a Unicode code point. This
 * helper records into an active transaction but does not rebuild rendering
 * or mark dirty.
 */
void bufferRowInsertByte(erow *row, int32_t at, uint8_t byte) {
    char value = (char)byte;
    bufferRowInsert(row, at, &value, 1);
}

/**
 * @brief Insert a byte span into the source text of a row.
 *
 * @details at is a byte offset, with out-of-range values treated as append.
 * text must not alias storage invalidated by resizing; caches and document
 * state remain the caller's responsibility.
 */
void bufferRowInsert(erow *row, int32_t at, const char *text, size_t len) {
    if (at < 0 || at > row->size) at = row->size;
    if (len > (size_t)(INT32_MAX - row->size)) {
        if (row->owner && row->owner->history && row->owner->history->pending) {
            row->owner->history->error = HISTORY_SIZE; return;
        }
        errno = ENOMEM; perror("tinyedit: row size"); exit(EXIT_FAILURE);
    }
    size_t needed = (size_t)row->size + len + 1;
    if (!historyPrepareRow(row, needed)) return;
    if (row->chars_capacity < needed) {
        struct editorHistory *history = row->owner ? row->owner->history : NULL;
        char *grown = history && history->pending ? teTryRealloc(row->chars, needed) :
            teRealloc(row->chars, needed);
        if (!grown) { history->error = HISTORY_MEMORY; return; }
        row->chars = grown; row->chars_capacity = needed;
    }
    memmove(&row->chars[at + (int32_t)len], &row->chars[at],
        (size_t)(row->size - at + 1));
    memcpy(&row->chars[at], text, len);
    row->size += (int32_t)len;
}

/**
 * @brief Append a byte span to a row's source text.
 *
 * @details text contains len bytes and must remain valid across resizing.
 * Preserves NUL termination; active transactions record source changes.
 * Rendering remains the caller's responsibility.
 */
void bufferRowAppend(erow *row, const char *text, size_t len) {
    bufferRowInsert(row, row->size, text, len);
}

/**
 * @brief Remove one raw byte from a row.
 *
 * @details at is a byte offset, not a character index. Character deletion must
 * first determine the complete grapheme range.
 */
void bufferRowDeleteByte(erow *row, int32_t at) {
    bufferRowDeleteRange(row, at, at + 1);
}

/**
 * @brief Remove a half-open range of source bytes.
 *
 * @details start and end are byte offsets; negative starts and ends past the
 * row are clamped. The caller updates display caches and edit state.
 */
void bufferRowDeleteRange(erow *row, int32_t start, int32_t end) {
    if (start < 0) start = 0;
    if (end > row->size) end = row->size;
    if (start >= end) return;
    if (!historyPrepareRow(row, (size_t)row->size + 1)) return;
    memmove(&row->chars[start], &row->chars[end], (size_t)(row->size - end + 1));
    row->size -= end - start;
}

/**
 * @brief Remove one leading tab or up to tab_stop spaces.
 *
 * @details Does not refresh caches; works with either indentation style
 * already present in the text.
 * @return the number of source bytes removed.
 */
int32_t bufferRowOutdent(erow *row, int32_t tab_stop) {
    if (row->size > 0 && row->chars[0] == '\t') {
        bufferRowDeleteRange(row, 0, 1);
        return 1;
    }
    int32_t removed = 0;
    while (removed < tab_stop && removed < row->size && row->chars[removed] == ' ')
        removed++;
    bufferRowDeleteRange(row, 0, removed);
    return removed;
}

/**
 * @brief Join rows into the exact byte representation to write to disk.
 *
 * @details ending selects LF or CRLF and final_newline controls the last
 * boundary. Writes out_len and returns owned bytes to free; there is no NUL
 * terminator.
 * @param buffer Logical rows to serialize.
 * @param ending LF or CRLF representation of each emitted boundary.
 * @param final_newline Whether the last row also receives a line ending.
 * @param out_len Required output for the returned allocation's byte count.
 */
char *bufferSerialize(const struct editorBuffer *buffer, enum lineEndingMode ending,
    uint8_t final_newline,
    size_t *out_len) {
    size_t ending_len = ending == LINE_ENDING_CRLF ? 2 : 1, total = 0;
    for (int32_t i = 0; i < buffer->row_count; i++) {
        size_t bytes = teSizeAdd((size_t)buffer->rows[i].size,
            (i + 1 < buffer->row_count || final_newline) ? ending_len : 0);
        total = teSizeAdd(total, bytes);
    }
    *out_len = total;
    char *result = teMalloc(total > 0 ? total : 1), *dst = result;
    for (int32_t i = 0; i < buffer->row_count; i++) {
        memcpy(dst, buffer->rows[i].chars, (size_t)buffer->rows[i].size);
        dst += buffer->rows[i].size;
        if (i + 1 < buffer->row_count || final_newline) {
            if (ending == LINE_ENDING_CRLF) *dst++ = '\r';
            *dst++ = '\n';
        }
    }
    return result;
}

/**
 * @brief Copy a half-open text range using LF between rows.
 *
 * @details Endpoints must be valid, ordered logical rows and source-byte
 * offsets.
 * @param start_y First logical row, zero-based.
 * @param start_x Inclusive source-byte offset in the first row.
 * @param end_y Last logical row, zero-based.
 * @param end_x Exclusive source-byte offset in the last row.
 * @param out_len Required output for text length, excluding NUL.
 * @return owned NUL-terminated text; out_len excludes the terminator.
 */
char *bufferSerializeRange(const struct editorBuffer *buffer, int32_t start_y,
    int32_t start_x, int32_t end_y, int32_t end_x, size_t *out_len) {
    size_t total = 0;
    for (int32_t y = start_y; y <= end_y; y++) {
        int32_t from = y == start_y ? start_x : 0;
        int32_t to = y == end_y ? end_x : buffer->rows[y].size;
        size_t bytes = teSizeAdd((size_t)(to - from), y < end_y ? 1u : 0u);
        total = teSizeAdd(total, bytes);
    }
    char *result = teMalloc(teSizeAdd(total, 1)), *dst = result;
    for (int32_t y = start_y; y <= end_y; y++) {
        int32_t from = y == start_y ? start_x : 0;
        int32_t to = y == end_y ? end_x : buffer->rows[y].size;
        size_t len = (size_t)(to - from);
        memcpy(dst, &buffer->rows[y].chars[from], len);
        dst += len;
        if (y < end_y) *dst++ = '\n';
    }
    *dst = '\0';
    *out_len = total;
    return result;
}
