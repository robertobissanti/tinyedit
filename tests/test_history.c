#include "buffer.h"
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    exit(1);
}

int main(void) {
    struct editorDocument document;
    memset(&document, 0, sizeof(document));
    bufferInsertRow(&document.buffer, 0, "one", 3);
    document.cursor.cx = 3;

    historyRecordEdit(&document, 10, EDIT_INSERT, 100);
    bufferRowAppend(&document.buffer.rows[0], "X", 1);
    document.cursor.cx++;
    historyRecordEdit(&document, 10, EDIT_INSERT, 100);
    bufferRowAppend(&document.buffer.rows[0], "Y", 1);
    document.cursor.cx++;
    if (document.history.undo_count != 1) fail("insert coalescing");

    undoSnapshot snapshot;
    if (!historyBeginUndo(&document, &snapshot)) fail("undo availability");
    historyRestoreSnapshot(&document, &snapshot);
    historyFreeSnapshot(&snapshot);
    if (strcmp(document.buffer.rows[0].chars, "one") != 0 || document.cursor.cx != 3)
        fail("undo restore");

    if (!historyBeginRedo(&document, &snapshot)) fail("redo availability");
    historyRestoreSnapshot(&document, &snapshot);
    historyFreeSnapshot(&snapshot);
    if (strcmp(document.buffer.rows[0].chars, "oneXY") != 0 || document.cursor.cx != 5)
        fail("redo restore");

    historyRecordEdit(&document, 1, EDIT_OTHER, 200);
    bufferRowAppend(&document.buffer.rows[0], "1", 1);
    historyRecordEdit(&document, 1, EDIT_OTHER, 201);
    if (document.history.undo_count != 1) fail("maximum depth");

    historyClear(&document.history);
    bufferClear(&document.buffer);
    puts("history tests: ok");
    return 0;
}
