#include "alloc.h"
#include "buffer.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    exit(1);
}

static void setRender(erow *row, const char *text) {
    row->rsize = (int32_t)strlen(text);
    row->render = teMalloc((size_t)row->rsize + 1);
    memcpy(row->render, text, (size_t)row->rsize + 1);
}

int main(void) {
    struct editorBuffer buffer = {0, NULL};
    struct editorView view = {0, 0, 0, 0, 20};
    bufferInsertRow(&buffer, 0, "one two three", 13);
    bufferInsertRow(&buffer, 1, "four", 4);
    setRender(&buffer.rows[0], "one two three");
    setRender(&buffer.rows[1], "four");

    if (renderGutterWidth(&buffer, 0) != 0 || renderGutterWidth(&buffer, 1) != 4)
        fail("gutter width");
    if (renderTextCols(&view, &buffer, 1) != 16) fail("text columns");
    if (renderSoftWrapCols(&view, &buffer, 1, 8) != 8) fail("soft wrap cap");

    if (renderRowSegments(&buffer.rows[0], 7) != 3) fail("word wrapping");
    if (buffer.rows[0].seg_start[1] != 4 || buffer.rows[0].seg_start_rx[1] != 4)
        fail("segment starts");
    int32_t segment, column;
    renderRxToSegment(&buffer.rows[0], 7, 9, &segment, &column);
    if (segment != 2 || column != 1) fail("rx to segment");
    if (renderTotalVideoRows(&buffer, 7) != 4) fail("total video rows");
    int32_t file_row;
    renderFileRowAtVideoRow(&buffer, 3, 7, &file_row, &segment);
    if (file_row != 1 || segment != 0) fail("video row inverse");

    bufferClear(&buffer);
    bufferInsertRow(&buffer, 0, "界😀A", strlen("界😀A"));
    setRender(&buffer.rows[0], "界😀A");
    if (renderRowSegments(&buffer.rows[0], 1) != 3) fail("wide glyph wrap count");
    if (buffer.rows[0].seg_start[1] != 3 || buffer.rows[0].seg_start[2] != 7 ||
        buffer.rows[0].seg_start_rx[1] != 2 || buffer.rows[0].seg_start_rx[2] != 4)
        fail("wide glyph segment boundaries");
    if (renderRowSegments(&buffer.rows[0], 2) != 3) fail("wide glyph resize");
    bufferClear(&buffer);

    bufferInsertRow(&buffer, 0, "e\xcc\x81界\xff", strlen("e\xcc\x81界\xff"));
    setRender(&buffer.rows[0], "e\xcc\x81界\xff");
    if (renderRowSegments(&buffer.rows[0], 1) != 3) fail("combining and malformed wrap");
    if (buffer.rows[0].seg_start[1] != 3 || buffer.rows[0].seg_start[2] != 6)
        fail("grapheme not split by narrow wrap");
    bufferClear(&buffer);
    puts("render tests: ok");
    return 0;
}
