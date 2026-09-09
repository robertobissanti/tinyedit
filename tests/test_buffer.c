#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    exit(1);
}

int main(void) {
    struct editorBuffer buffer = {0, NULL};
    bufferInsertRow(&buffer, 0, "A\tB", 3);
    bufferInsertRow(&buffer, 1, "caff\xc3\xa8", 6);
    if (buffer.row_count != 2) fail("row insertion");
    if (bufferRowCxToRx(&buffer.rows[0], 2, 4) != 4) fail("tab cx to rx");
    if (bufferRowRxToCx(&buffer.rows[0], 3, 4) != 1) fail("tab rx to cx");

    bufferRowInsertByte(&buffer.rows[0], 1, '!');
    bufferRowDeleteByte(&buffer.rows[0], 1);
    bufferRowAppend(&buffer.rows[0], "!", 1);
    if (strcmp(buffer.rows[0].chars, "A\tB!") != 0) fail("row byte mutations");

    size_t len = 0;
    char *text = bufferSerialize(&buffer, LINE_ENDING_CRLF, &len);
    if (len != 14 || memcmp(text, "A\tB!\r\ncaff\xc3\xa8\r\n", len) != 0)
        fail("CRLF serialization");
    free(text);

    text = bufferSerializeRange(&buffer, 0, 1, 1, 4, &len);
    if (len != 8 || memcmp(text, "\tB!\ncaff", len) != 0)
        fail("multiline range serialization");
    free(text);

    bufferDeleteRow(&buffer, 0);
    if (buffer.row_count != 1 || strcmp(buffer.rows[0].chars, "caff\xc3\xa8") != 0)
        fail("row deletion");
    bufferClear(&buffer);
    if (buffer.row_count != 0 || buffer.rows != NULL) fail("buffer clear");

    puts("buffer tests: ok");
    return 0;
}
