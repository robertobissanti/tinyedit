#include "render.h"

#include "alloc.h"
#include "buffer.h"
#include "utf8.h"

#include <stdlib.h>

static uint8_t preview_enabled;
static const erow *preview_row;
static int32_t preview_cursor;

/* ---- Markdown presentation ---- */

static int32_t sourceRenderByte(const erow *row, int32_t cx, int32_t tab_stop) {
    int32_t at = 0, byte = 0, rx = 0;
    while (at < cx && at < row->size) {
        size_t len = utf8NextCharLen(row->chars, (size_t)at, (size_t)row->size);
        uint8_t tab = row->chars[at] == '\t';
        int32_t width = tab ? tab_stop - rx % tab_stop :
            utf8SingleCharWidth(row->chars + at, len);
        byte += tab ? width : (int32_t)len;
        rx += width;
        at += (int32_t)len;
    }
    return byte;
}

void renderSetMarkdownPreview(uint8_t enabled, const erow *active_row,
    int32_t cursor, int32_t tab_stop) {
    preview_enabled = enabled;
    preview_row = active_row;
    preview_cursor = enabled && active_row ? sourceRenderByte(active_row, cursor, tab_stop) : -1;
}

static const struct markdownSpan *renderSpanAt(const erow *row, int32_t byte) {
    int32_t lo = 0, hi = row->markdown_count;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (row->markdown_spans[mid].start <= byte) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || byte >= row->markdown_spans[lo - 1].end) return NULL;
    return &row->markdown_spans[lo - 1];
}

uint8_t renderByteHidden(const erow *row, int32_t byte) {
    if (!preview_enabled) return 0;
    const struct markdownSpan *span = renderSpanAt(row, byte);
    if (!span || (row == preview_row && preview_cursor >= span->start &&
        preview_cursor <= span->end)) return 0;
    return byte < span->content_start || byte >= span->content_end;
}

uint8_t renderByteUnderlined(const erow *row, int32_t byte) {
    if (!preview_enabled) return 0;
    const struct markdownSpan *span = renderSpanAt(row, byte);
    return span && span->link && byte >= span->content_start && byte < span->content_end;
}

int32_t renderByteAtColumn(const erow *row, int32_t column) {
    int32_t byte = 0, rx = 0;
    while (byte < row->rsize && rx < column) {
        size_t len = utf8NextCharLen(row->render, (size_t)byte, (size_t)row->rsize);
        int32_t width = renderByteHidden(row, byte) ? 0 :
            utf8SingleCharWidth(row->render + byte, len);
        if (rx + width > column) break;
        rx += width; byte += (int32_t)len;
    }
    return byte;
}

int32_t renderRowCxToRx(const erow *row, int32_t cx, int32_t tab_stop) {
    if (!preview_enabled || !row->markdown_count) return bufferRowCxToRx(row, cx, tab_stop);
    int32_t end = sourceRenderByte(row, cx, tab_stop), byte = 0, rx = 0;
    while (byte < end) {
        size_t len = utf8NextCharLen(row->render, (size_t)byte, (size_t)row->rsize);
        if (!renderByteHidden(row, byte)) rx += utf8SingleCharWidth(row->render + byte, len);
        byte += (int32_t)len;
    }
    return rx;
}

int32_t renderRowRxToCx(const erow *row, int32_t target_rx, int32_t tab_stop) {
    if (!preview_enabled || !row->markdown_count) return bufferRowRxToCx(row, target_rx, tab_stop);
    int32_t at = 0, byte = 0, raw_rx = 0, rx = 0;
    while (at < row->size && rx < target_rx) {
        size_t len = utf8NextCharLen(row->chars, (size_t)at, (size_t)row->size);
        uint8_t tab = row->chars[at] == '\t';
        int32_t width = tab ? tab_stop - raw_rx % tab_stop :
            utf8SingleCharWidth(row->chars + at, len);
        int32_t visible = renderByteHidden(row, byte) ? 0 : width;
        if (rx + visible > target_rx) break;
        rx += visible; raw_rx += width;
        byte += tab ? width : (int32_t)len; at += (int32_t)len;
    }
    return at;
}

/* ---- layout ---- */

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
    int32_t cursor = row == preview_row ? preview_cursor : -1;
    if (row->seg_start && row->seg_start_rx && row->seg_wrapcols == wrapcols &&
        row->seg_preview_enabled == preview_enabled && row->seg_preview_cursor == cursor)
        return row->seg_count;
    free(row->seg_start);
    free(row->seg_start_rx);
    row->seg_start = NULL;
    row->seg_start_rx = NULL;
    row->seg_count = 0;
    row->seg_wrapcols = wrapcols;
    row->seg_preview_enabled = preview_enabled;
    row->seg_preview_cursor = cursor;

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
        while (pos < row->rsize && (col < wrapcols || renderByteHidden(row, pos))) {
            size_t len = utf8NextCharLen(row->render, (size_t)pos, (size_t)row->rsize);
            if (len == 0) len = 1;
            int32_t width = renderByteHidden(row, pos) ? 0 :
                utf8SingleCharWidth(row->render + pos, len);
            /* A glyph wider than the viewport still owns one segment;
             * leaving it unconsumed would make the outer loop never advance. */
            if (width > 0 && col + width > wrapcols && col > 0) break;
            if (width && row->render[pos] == ' ') {
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
        renderRowCxToRx(row, row->size, tab_stop);
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
