#ifndef __RENDER_H
#define __RENDER_H

#include "tinyedit.h"

/**
 * @brief Measure the columns reserved for line numbers.
 *
 * @details Includes one padding column and returns zero when show_line_numbers
 * is false.
 */
int32_t renderGutterWidth(const struct editorBuffer *buffer, uint8_t show_line_numbers);
/**
 * @brief Measure the viewport width available after the gutter.
 *
 * @return a nonnegative column count from view and buffer; does not change
 * either.
 */
int32_t renderTextCols(const struct editorView *view, const struct editorBuffer *buffer,
    uint8_t show_line_numbers);
/**
 * @brief Resolve the wrapping width with a right-hand margin.
 *
 * @details soft_wrap <= 0 means no extra cap.
 * @return at least one display column, even in a narrow viewport.
 */
int32_t renderSoftWrapCols(const struct editorView *view, const struct editorBuffer *buffer,
    uint8_t show_line_numbers, int32_t soft_wrap);
/**
 * @brief Build or reuse a row's visual wrap boundaries.
 *
 * @details row must have current render text; wrapcols is in display columns.
 * A grapheme wider than wrapcols occupies a segment on its own, intact.
 * @return at least one segment and caches separate render-byte and display-column starts owned by row.
 */
int32_t renderRowSegments(erow *row, int32_t wrapcols);
/**
 * @brief Get a segment's exclusive render-byte end without trailing spaces.
 *
 * @details segment indexes segment_starts in a current wrap cache. Use the
 * result to slice render text, not to position a screen cursor.
 */
int32_t renderSegmentVisibleEnd(const erow *row, int32_t segment_count,
    const int32_t *segment_starts, int32_t segment);
/**
 * @brief Get a segment's exclusive display-column end without trailing spaces.
 *
 * @details Byte and column start arrays must describe the same cache. tab_stop
 * must match the row's layout; the return value is a row-wide column.
 */
int32_t renderSegmentVisibleEndRx(const erow *row, int32_t segment_count,
    const int32_t *segment_starts, const int32_t *segment_starts_rx,
    int32_t segment, int32_t tab_stop);
/**
 * @brief Map a row-wide display column to a visual segment and local column.
 *
 * @details Builds the wrap cache if necessary. Writes zero-based segment and
 * column outputs without changing source text.
 */
void renderRxToSegment(erow *row, int32_t wrapcols, int32_t rx,
    int32_t *segment, int32_t *column);
/**
 * @brief Count the visual rows used by a logical row.
 *
 * @details file_row must address an existing row.
 * @return one for nonpositive wrapcols, otherwise the cached or newly computed
 * segment count.
 */
int32_t renderRowVideoHeight(struct editorBuffer *buffer, int32_t file_row,
    int32_t wrapcols);
/**
 * @brief Translate a logical row and segment into an absolute visual row.
 *
 * @details Both indices are zero-based and must describe a valid position.
 * Counts preceding rows using the same wrapcols.
 */
int32_t renderVideoRowOf(struct editorBuffer *buffer, int32_t file_row,
    int32_t segment, int32_t wrapcols);
/**
 * @brief Translate an absolute visual row to its logical row and segment.
 *
 * @details video_row must be nonnegative. Writes the last row and segment zero
 * when past EOF, or 0/0 for an empty buffer.
 */
void renderFileRowAtVideoRow(struct editorBuffer *buffer, int32_t video_row,
    int32_t wrapcols, int32_t *file_row, int32_t *segment);
/**
 * @brief Count visual rows across the whole buffer.
 *
 * @details May populate row wrap caches.
 * @return zero for an empty buffer; wrapcols must agree with the viewport used
 * by other conversions.
 */
int32_t renderTotalVideoRows(struct editorBuffer *buffer, int32_t wrapcols);

#endif
