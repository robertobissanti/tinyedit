#ifndef __RENDER_H
#define __RENDER_H

#include "tinyedit.h"

int32_t renderGutterWidth(const struct editorBuffer *buffer, uint8_t show_line_numbers);
int32_t renderTextCols(const struct editorView *view, const struct editorBuffer *buffer,
    uint8_t show_line_numbers);
int32_t renderSoftWrapCols(const struct editorView *view, const struct editorBuffer *buffer,
    uint8_t show_line_numbers, int32_t soft_wrap);
int32_t renderRowSegments(erow *row, int32_t wrapcols);
int32_t renderSegmentVisibleEnd(const erow *row, int32_t segment_count,
    const int32_t *segment_starts, int32_t segment);
int32_t renderSegmentVisibleEndRx(const erow *row, int32_t segment_count,
    const int32_t *segment_starts, const int32_t *segment_starts_rx,
    int32_t segment, int32_t tab_stop);
void renderRxToSegment(erow *row, int32_t wrapcols, int32_t rx,
    int32_t *segment, int32_t *column);
int32_t renderRowVideoHeight(struct editorBuffer *buffer, int32_t file_row,
    int32_t wrapcols);
int32_t renderVideoRowOf(struct editorBuffer *buffer, int32_t file_row,
    int32_t segment, int32_t wrapcols);
void renderFileRowAtVideoRow(struct editorBuffer *buffer, int32_t video_row,
    int32_t wrapcols, int32_t *file_row, int32_t *segment);
int32_t renderTotalVideoRows(struct editorBuffer *buffer, int32_t wrapcols);

#endif
