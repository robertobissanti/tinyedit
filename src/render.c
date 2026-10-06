#include "render.h"

#include "alloc.h"
#include "buffer.h"
#include "utf8.h"

#include <stdlib.h>

/**
 * @brief Measure the columns reserved for line numbers.
 *
 * @details Includes one padding column and returns zero when show_line_numbers
 * is false.
 */
int32_t renderGutterWidth(const struct editorBuffer *buffer, uint8_t show_line_numbers) {
    if (!show_line_numbers) return 0;
    int32_t digits = 3, count = buffer->row_count;
    while (count >= 1000) {
        digits++;
        count /= 10;
    }
    if (digits > 14) digits = 14;
    return digits + 1;
}

/**
 * @brief Measure the viewport width available after the gutter.
 *
 * @return a nonnegative column count from view and buffer; does not change
 * either.
 */
int32_t renderTextCols(const struct editorView *view, const struct editorBuffer *buffer,
    uint8_t show_line_numbers) {
    int32_t cols = view->screencols - renderGutterWidth(buffer, show_line_numbers);
    return cols > 0 ? cols : 0;
}

/**
 * @brief Resolve the wrapping width with a right-hand margin.
 *
 * @details soft_wrap <= 0 means no extra cap.
 * @return at least one display column, even in a narrow viewport.
 */
int32_t renderSoftWrapCols(const struct editorView *view, const struct editorBuffer *buffer,
    uint8_t show_line_numbers, int32_t soft_wrap) {
    int32_t margin = renderTextCols(view, buffer, show_line_numbers) - 1;
    if (margin < 1) margin = 1;
    if (soft_wrap <= 0) return margin;
    return margin < soft_wrap ? margin : soft_wrap;
}

/**
 * @brief Build or reuse a row's visual wrap boundaries.
 *
 * @details row must have current render text; wrapcols is in display columns.
 * A grapheme wider than wrapcols occupies a segment on its own, intact.
 * @return at least one segment and caches separate render-byte and display-column starts owned by row.
 */
int32_t renderRowSegments(erow *row, int32_t wrapcols) {
    if (row->seg_start && row->seg_start_rx && row->seg_wrapcols == wrapcols)
        return row->seg_count;
    free(row->seg_start);
    free(row->seg_start_rx);
    row->seg_start = NULL;
    row->seg_start_rx = NULL;
    row->seg_count = 0;
    row->seg_wrapcols = wrapcols;

    int32_t capacity = 16;
    row->seg_start = teMalloc(teArrayBytes((size_t)capacity, sizeof(*row->seg_start)));
    row->seg_start_rx = teMalloc(teArrayBytes((size_t)capacity, sizeof(*row->seg_start_rx)));
    if (wrapcols <= 0 || row->rsize == 0) {
        row->seg_start[0] = 0;
        row->seg_start_rx[0] = 0;
        row->seg_count = 1;
        return 1;
    }

    int32_t count = 0, line_start = 0, line_start_rx = 0;
    while (line_start < row->rsize) {
        if (count == capacity) {
            /* Each segment consumes at least one of rsize <= INT32_MAX bytes. */
            capacity = capacity > INT32_MAX / 2 ? INT32_MAX : capacity * 2;
            row->seg_start = teRealloc(row->seg_start,
                teArrayBytes((size_t)capacity, sizeof(*row->seg_start)));
            row->seg_start_rx = teRealloc(row->seg_start_rx,
                teArrayBytes((size_t)capacity, sizeof(*row->seg_start_rx)));
        }
        row->seg_start[count] = line_start;
        row->seg_start_rx[count++] = line_start_rx;
        int32_t col = 0, pos = line_start, last_space_pos = -1, last_space_col = -1;
        while (pos < row->rsize && col < wrapcols) {
            size_t len = utf8NextCharLen(row->render, (size_t)pos, (size_t)row->rsize);
            if (len == 0) len = 1;
            int32_t width = utf8SingleCharWidth(row->render + pos, len);
            /* A glyph wider than the viewport still owns one segment;
             * leaving it unconsumed would make the outer loop never advance. */
            if (col + width > wrapcols && pos > line_start) break;
            if (row->render[pos] == ' ') {
                last_space_pos = pos;
                last_space_col = col;
            }
            col += width;
            pos += (int32_t)len;
        }
        if (pos >= row->rsize) {
            line_start_rx += col;
            line_start = row->rsize;
        } else if (last_space_pos >= 0 && last_space_pos + 1 > line_start) {
            line_start_rx += last_space_col + 1;
            line_start = last_space_pos + 1;
        } else {
            line_start_rx += col;
            line_start = pos;
        }
    }
    row->seg_count = count;
    return count;
}

/**
 * @brief Get a segment's exclusive render-byte end without trailing spaces.
 *
 * @details segment indexes segment_starts in a current wrap cache. Use the
 * result to slice render text, not to position a screen cursor.
 */
int32_t renderSegmentVisibleEnd(const erow *row, int32_t segment_count,
    const int32_t *segment_starts, int32_t segment) {
    int32_t end = segment + 1 < segment_count ? segment_starts[segment + 1] : row->rsize;
    while (end > segment_starts[segment] && row->render[end - 1] == ' ') end--;
    return end;
}

/**
 * @brief Get a segment's exclusive display-column end without trailing spaces.
 *
 * @details Byte and column start arrays must describe the same cache. tab_stop
 * must match the row's layout; the return value is a row-wide column.
 */
int32_t renderSegmentVisibleEndRx(const erow *row, int32_t segment_count,
    const int32_t *segment_starts, const int32_t *segment_starts_rx,
    int32_t segment, int32_t tab_stop) {
    int32_t end_byte = segment + 1 < segment_count ? segment_starts[segment + 1] : row->rsize;
    int32_t end_rx = segment + 1 < segment_count ? segment_starts_rx[segment + 1] :
        bufferRowCxToRx(row, row->size, tab_stop);
    while (end_byte > segment_starts[segment] && row->render[end_byte - 1] == ' ') {
        end_byte--;
        end_rx--;
    }
    return end_rx;
}

/**
 * @brief Map a row-wide display column to a visual segment and local column.
 *
 * @details Builds the wrap cache if necessary. Writes zero-based segment and
 * column outputs without changing source text.
 */
void renderRxToSegment(erow *row, int32_t wrapcols, int32_t rx,
    int32_t *segment, int32_t *column) {
    int32_t count = renderRowSegments(row, wrapcols), index;
    for (index = 0; index < count - 1; index++)
        if (rx < row->seg_start_rx[index + 1]) break;
    *segment = index;
    *column = rx - row->seg_start_rx[index];
}

/**
 * @brief Count the visual rows used by a logical row.
 *
 * @details file_row must address an existing row.
 * @return one for nonpositive wrapcols, otherwise the cached or newly computed
 * segment count.
 */
int32_t renderRowVideoHeight(struct editorBuffer *buffer, int32_t file_row,
    int32_t wrapcols) {
    return wrapcols <= 0 ? 1 : renderRowSegments(&buffer->rows[file_row], wrapcols);
}

/**
 * @brief Translate a logical row and segment into an absolute visual row.
 *
 * @details Both indices are zero-based and must describe a valid position.
 * Counts preceding rows using the same wrapcols.
 */
int32_t renderVideoRowOf(struct editorBuffer *buffer, int32_t file_row,
    int32_t segment, int32_t wrapcols) {
    int32_t video_row = 0;
    for (int32_t i = 0; i < file_row; i++)
        video_row += renderRowVideoHeight(buffer, i, wrapcols);
    return video_row + segment;
}

/**
 * @brief Translate an absolute visual row to its logical row and segment.
 *
 * @details video_row must be nonnegative. Writes the last row and segment zero
 * when past EOF, or 0/0 for an empty buffer.
 */
void renderFileRowAtVideoRow(struct editorBuffer *buffer, int32_t video_row,
    int32_t wrapcols, int32_t *file_row, int32_t *segment) {
    int32_t remaining = video_row;
    for (int32_t i = 0; i < buffer->row_count; i++) {
        int32_t height = renderRowVideoHeight(buffer, i, wrapcols);
        if (remaining < height) {
            *file_row = i;
            *segment = remaining;
            return;
        }
        remaining -= height;
    }
    *file_row = buffer->row_count > 0 ? buffer->row_count - 1 : 0;
    *segment = 0;
}

/**
 * @brief Count visual rows across the whole buffer.
 *
 * @details May populate row wrap caches.
 * @return zero for an empty buffer; wrapcols must agree with the viewport used
 * by other conversions.
 */
int32_t renderTotalVideoRows(struct editorBuffer *buffer, int32_t wrapcols) {
    int32_t total = 0;
    for (int32_t i = 0; i < buffer->row_count; i++)
        total += renderRowVideoHeight(buffer, i, wrapcols);
    return total;
}
