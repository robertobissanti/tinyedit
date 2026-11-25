#ifndef __TE_MARKDOWN_H
#define __TE_MARKDOWN_H

#include <stdint.h>

struct erow;
struct editorBuffer;

struct markdownSpan {
    int32_t start, content_start, content_end, end; /* Render-byte offsets. */
    uint8_t link;
};

struct markdownCodeState {
    int32_t start, width;
    uint8_t marker, comment, math;
};

struct textLink;

enum markdownAnchorResult { MARKDOWN_ANCHOR_MISSING, MARKDOWN_ANCHOR_FOUND, MARKDOWN_ANCHOR_DUPLICATE };
struct markdownScanState { uint8_t fence; int32_t fence_width, inline_width; };
struct markdownAttributes { int32_t id_start, id_end, end; };
struct markdownAnchorSearch { int32_t count, row, column; };

/* Source navigation never depends on render/highlight caches. Outputs change
 * only on FOUND; duplicates are reported rather than selecting a destination. */
enum markdownAnchorResult markdownResolveAnchor(const struct editorBuffer *buffer,
    const char *anchor, int32_t *row, int32_t *column);
uint8_t markdownSourceVisible(const struct editorBuffer *buffer, int32_t row, int32_t byte);
uint8_t markdownFindReference(const struct editorBuffer *buffer, int32_t row,
    int32_t cursor, struct textLink *link);
char *markdownHeadingSlug(const char *text, int32_t length);
uint8_t markdownLinkIsImage(const struct erow *row, const struct textLink *link);

/* Rebuild row-owned Markdown highlights and spans, with incoming block state.
 * render must be current; spans borrow no source memory. */
void markdownHighlightRow(struct erow *row, uint8_t prev_in_fence, uint8_t prev_in_math,
    uint8_t prev_in_frontmatter, int32_t row_index, uint8_t prev_in_emphasis);
void markdownHighlightAnchors(struct erow *row);
void markdownHighlightTable(struct erow *row, int32_t row_index);
uint8_t markdownIsExtension(const char *ext);

/* Borrow active_row until the next configuration. cursor is a source-byte
 * offset. Configure after mutation and before layout or drawing. */
void markdownSetPreview(uint8_t enabled, const struct erow *active_row,
    int32_t cursor, int32_t tab_stop);
void markdownKeepLinkEditing(void);
const struct markdownSpan *markdownSpanAt(const struct erow *row, int32_t byte);
uint8_t markdownByteHidden(const struct erow *row, int32_t byte);
uint8_t markdownByteUnderlined(const struct erow *row, int32_t byte);
uint8_t markdownPreviewEnabled(void);
int32_t markdownPreviewCursor(const struct erow *row);
int32_t markdownPreviewEditStart(const struct erow *row);
int32_t markdownSourceRenderByte(const struct erow *row, int32_t cx, int32_t tab_stop);

#endif
