#ifndef __BUFFER_H
#define __BUFFER_H

#include "tinyedit.h"

/**
 * @brief Convert a source-byte cursor offset into a display column.
 *
 * @details cx must be a valid boundary within row and tab_stop must be
 * positive. Counts graphemes and expanded tabs; returns the zero-based column.
 */
int32_t bufferRowCxToRx(const erow *row, int32_t cx, int32_t tab_stop);
/**
 * @brief Find a source-byte cursor position for a display column.
 *
 * @details tab_stop must be positive.
 * @return the start of a glyph containing target_rx, or the row end when the
 * target is beyond it.
 */
int32_t bufferRowRxToCx(const erow *row, int32_t target_rx, int32_t tab_stop);
/**
 * @brief Insert a copy of a byte span as a logical row.
 *
 * @details at is in [0, row_count] and text contains len bytes without a line
 * ending. New display caches start empty; existing row pointers may be
 * invalidated.
 */
void bufferInsertRow(struct editorBuffer *buffer, int32_t at, const char *text, size_t len);
/**
 * @brief Remove a logical row and release its owned storage.
 *
 * @details at is zero-based; invalid indices are ignored. Later rows shift, so
 * previously saved row pointers may no longer identify the same row.
 */
void bufferDeleteRow(struct editorBuffer *buffer, int32_t at);
/**
 * @brief Release all text and display allocations owned by a row.
 *
 * @details Does not remove the row from its buffer or reset its pointers. The
 * caller must avoid freeing the same row twice.
 */
void bufferFreeRow(erow *row);
/**
 * @brief Release every row and return a buffer to its empty state.
 *
 * @details Safe for an initialized empty buffer. Resets rows and row_count;
 * surrounding document metadata is unchanged.
 */
void bufferClear(struct editorBuffer *buffer);
/**
 * @brief Insert a single raw byte into a row.
 *
 * @details at is a source-byte offset; byte is not a Unicode code point. This
 * helper does not rebuild rendering, record undo or mark dirty.
 */
void bufferRowInsertByte(erow *row, int32_t at, int32_t byte);
/**
 * @brief Insert a byte span into the source text of a row.
 *
 * @details at is a byte offset, with out-of-range values treated as append.
 * text must not alias storage invalidated by resizing; caches and document
 * state remain the caller's responsibility.
 */
void bufferRowInsert(erow *row, int32_t at, const char *text, size_t len);
/**
 * @brief Append a byte span to a row's source text.
 *
 * @details text contains len bytes and must remain valid across resizing.
 * Preserves NUL termination but does not update rendering or undo.
 */
void bufferRowAppend(erow *row, const char *text, size_t len);
/**
 * @brief Remove one raw byte from a row.
 *
 * @details at is a byte offset, not a character index. Character deletion must
 * first determine the complete grapheme range.
 */
void bufferRowDeleteByte(erow *row, int32_t at);
/**
 * @brief Remove a half-open range of source bytes.
 *
 * @details start and end are byte offsets; negative starts and ends past the
 * row are clamped. The caller updates display caches and edit state.
 */
void bufferRowDeleteRange(erow *row, int32_t start, int32_t end);
/**
 * @brief Remove one leading tab or up to tab_stop spaces.
 *
 * @details Does not refresh caches; works with either indentation style
 * already present in the text.
 * @return the number of source bytes removed.
 */
int32_t bufferRowOutdent(erow *row, int32_t tab_stop);
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
    size_t *out_len);
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
    int32_t start_x, int32_t end_y, int32_t end_x, size_t *out_len);

#endif
