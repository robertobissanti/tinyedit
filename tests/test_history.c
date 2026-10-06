#include "alloc.h"
#include "buffer.h"
#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t fail_after = -1;
static size_t requested_bytes, allocation_count;

static void *testTryMalloc(size_t size) {
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    allocation_count++;
    requested_bytes += size;
    return teTryMalloc(size);
}

static void *testTryRealloc(void *pointer, size_t size) {
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    allocation_count++;
    requested_bytes += size;
    return teTryRealloc(pointer, size);
}

/* Inject failures into the actual journal and buffer implementations. */
#define teTryMalloc testTryMalloc
#define teTryRealloc testTryRealloc
#include "../src/history.c"
#include "../src/buffer.c"
#undef teTryMalloc
#undef teTryRealloc

static void check(uint8_t condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

static void release(struct editorDocument *document) {
    historyClear(&document->history);
    bufferClear(&document->buffer);
}

static void append(struct editorDocument *document, enum undoEditType type, time_t now) {
    historyRecordEdit(document, 20, type, now);
    bufferRowAppend(&document->buffer.rows[0], "X", 1);
    document->cursor.cx++;
    check(historyFinishEdit(document) == HISTORY_OK, "commit append");
}

static void testReplay(void) {
    struct editorDocument document = {0};
    bufferInsertRow(&document.buffer, 0, "é界", strlen("é界"));
    document.cursor.cx = document.buffer.rows[0].size;
    append(&document, EDIT_INSERT, 100);
    append(&document, EDIT_INSERT, 100);
    check(document.history.undo_count == 1, "coalesce completed edits");
    size_t bytes = document.history.bytes;
    fail_after = 0;
    check(historyUndo(&document), "undo needs no source allocation");
    check(!strcmp(document.buffer.rows[0].chars, "é界") && document.cursor.cx == 5,
        "undo coalesced UTF-8");
    check(historyRedo(&document), "redo needs no source allocation");
    check(!strcmp(document.buffer.rows[0].chars, "é界XX") && document.cursor.cx == 7,
        "redo coalesced UTF-8");
    check(document.history.bytes == bytes, "replay preserves budget");
    fail_after = -1;
    historyUndo(&document);
    append(&document, EDIT_OTHER, 200);
    check(document.history.redo_count == 0, "successful branch invalidates redo");
    release(&document);
}

static void testStructuralReplay(void) {
    struct editorDocument document = {0};
    const char malformed[] = {'a', '\0', (char)0xff, 'b'};
    bufferInsertRow(&document.buffer, 0, malformed, sizeof(malformed));
    bufferInsertRow(&document.buffer, 1, "last", 4);
    historyRecordEdit(&document, 10, EDIT_OTHER, 1);
    bufferRowDeleteRange(&document.buffer.rows[0], 1, 3);
    bufferDeleteRow(&document.buffer, 1);
    bufferInsertRow(&document.buffer, 0, "界", strlen("界"));
    bufferRowAppend(&document.buffer.rows[1], "Z", 1);
    check(historyFinishEdit(&document) == HISTORY_OK, "compound commit");
    int32_t capacity = document.buffer.capacity;
    fail_after = 0;
    for (int32_t i = 0; i < 10; i++) {
        check(historyUndo(&document), "compound undo");
        check(document.buffer.row_count == 2 && document.buffer.rows[0].size == 4 &&
            !memcmp(document.buffer.rows[0].chars, malformed, 4) &&
            !strcmp(document.buffer.rows[1].chars, "last"), "exact malformed and NUL restore");
        check(historyRedo(&document), "compound redo");
        check(!strcmp(document.buffer.rows[0].chars, "界") &&
            !strcmp(document.buffer.rows[1].chars, "abZ"), "exact compound redo");
    }
    check(document.buffer.capacity == capacity, "replay retains vector capacity");
    fail_after = -1;
    release(&document);
}

static void testAllocationRollback(void) {
    for (int32_t n = 0; n < 14; n++) {
        struct editorDocument document = {0};
        bufferInsertRow(&document.buffer, 0, "original", 8);
        append(&document, EDIT_OTHER, 1);
        historyUndo(&document);
        size_t bytes = document.history.bytes;
        document.selection.active = 1;
        document.selection.anchor_x = 2;
        fail_after = n;
        historyRecordEdit(&document, 20, EDIT_OTHER, 20);
        bufferRowAppend(&document.buffer.rows[0], "A", 1);
        bufferInsertRow(&document.buffer, 1, "new", 3);
        bufferInsertRow(&document.buffer, 2, "new2", 4);
        bufferDeleteRow(&document.buffer, 0);
        document.cursor.cx = 99;
        enum historyError error = historyFinishEdit(&document);
        fail_after = -1;
        if (error != HISTORY_OK) {
            check(document.buffer.row_count == 1 &&
                !strcmp(document.buffer.rows[0].chars, "original"), "OOM restores all source");
            check(document.history.bytes == bytes && document.history.redo_count == 1 &&
                document.history.undo_count == 0, "failed branch preserves history");
            document.history.error = HISTORY_OK;
            check(historyRedo(&document), "redo works after allocation failure");
            check(!strcmp(document.buffer.rows[0].chars, "originalX"), "redo unchanged after failure");
        }
        release(&document);
    }
}

static void testBudget(void) {
    struct editorDocument document = {0};
    bufferInsertRow(&document.buffer, 0, "x", 1);
    size_t action_bytes = sizeof(struct historyAction) + sizeof(struct historyChange) + 3;
    historySetBudget(&document, action_bytes * 2);
    for (int32_t i = 0; i < 8; i++) {
        historyRecordEdit(&document, 20, EDIT_OTHER, i);
        bufferRowInsertByte(&document.buffer.rows[0], 0, 'x');
        check(historyFinishEdit(&document) == HISTORY_OK, "budget permits individual action");
        check(document.history.bytes <= document.history.budget, "combined history bound");
    }
    check(document.history.dropped_count > 0, "eviction is observable");
    historyUndo(&document);
    size_t bytes = document.history.bytes;
    int32_t old_size = document.buffer.rows[0].size;
    historyRecordEdit(&document, 20, EDIT_OTHER, 100);
    bufferInsertRow(&document.buffer, 0, "small", 5);
    char big[1024]; memset(big, 'z', sizeof(big));
    bufferRowAppend(&document.buffer.rows[1], big, sizeof(big));
    check(historyFinishEdit(&document) == HISTORY_LIMIT, "oversized action refused");
    check(document.buffer.row_count == 1 && document.buffer.rows[0].size == old_size &&
        document.history.bytes == bytes && document.history.redo_count == 1,
        "limit rolls back earlier changes and preserves redo");
    size_t budget = document.history.budget;
    historySetBudget(&document, 1);
    check(document.history.budget == budget, "budget changes deferred until reset");
    release(&document);
    historySetBudget(&document, 1234);
    check(document.history.budget == 1234, "reset applies new budget");
    release(&document);
}

static void testDepthAndHeldAction(void) {
    struct editorDocument document = {0};
    bufferInsertRow(&document.buffer, 0, "x", 1);
    for (int32_t i = 0; i < 8; i++) append(&document, EDIT_OTHER, i);
    historyRecordEdit(&document, 2, EDIT_INSERT, 100);
    bufferRowAppend(&document.buffer.rows[0], "a", 1);
    historyFinishEdit(&document);
    historyRecordEdit(&document, 2, EDIT_INSERT, 100);
    bufferRowAppend(&document.buffer.rows[0], "b", 1);
    historyFinishEdit(&document);
    check(document.history.undo_count == 2, "trim depth including coalescence");
    historyRecordEdit(&document, 10, EDIT_OTHER, 200);
    document.history.hold = 1;
    bufferRowAppend(&document.buffer.rows[0], "c", 1);
    historyFinishEdit(&document);
    bufferRowAppend(&document.buffer.rows[0], "d", 1);
    document.history.hold = 0;
    historyFinishEdit(&document);
    historyUndo(&document);
    check(!strcmp(document.buffer.rows[0].chars, "xXXXXXXXXab"), "held replace session single undo");
    release(&document);
}

static void testMixedChanges(void) {
    struct editorDocument document = {0};
    uint32_t random = 17;
    for (int32_t step = 0; step < 300; step++) {
        size_t before_len;
        int32_t before_rows = document.buffer.row_count;
        char *before = bufferSerialize(&document.buffer, LINE_ENDING_LF, 0, &before_len);
        historyRecordEdit(&document, 32, EDIT_OTHER, step);
        for (int32_t part = 0; part < 7; part++) {
            random = random * 1664525u + 1013904223u;
            int32_t rows = document.buffer.row_count;
            if (!rows || random % 4 == 0) {
                int32_t at = (int32_t)(random % (uint32_t)(rows + 1));
                bufferInsertRow(&document.buffer, at, "é界", strlen("é界"));
            } else {
                int32_t at = (int32_t)(random % (uint32_t)rows);
                if (random % 4 == 1) bufferDeleteRow(&document.buffer, at);
                else if (random % 4 == 2) bufferRowAppend(&document.buffer.rows[at], "x", 1);
                else if (document.buffer.rows[at].size)
                    bufferRowDeleteRange(&document.buffer.rows[at], 0,
                        (int32_t)utf8NextCharLen(document.buffer.rows[at].chars, 0,
                            (size_t)document.buffer.rows[at].size));
            }
        }
        check(historyFinishEdit(&document) == HISTORY_OK, "mixed action commit");
        size_t after_len;
        int32_t after_rows = document.buffer.row_count;
        char *after = bufferSerialize(&document.buffer, LINE_ENDING_LF, 0, &after_len);
        check(historyUndo(&document), "mixed action undo");
        size_t actual_len;
        char *actual = bufferSerialize(&document.buffer, LINE_ENDING_LF, 0, &actual_len);
        check(document.buffer.row_count == before_rows && actual_len == before_len &&
            !memcmp(actual, before, before_len), "mixed shifts undo exact source");
        free(actual);
        check(historyRedo(&document), "mixed action redo");
        actual = bufferSerialize(&document.buffer, LINE_ENDING_LF, 0, &actual_len);
        check(document.buffer.row_count == after_rows && actual_len == after_len &&
            !memcmp(actual, after, after_len), "mixed shifts redo exact source");
        free(actual); free(before); free(after);
    }
    release(&document);
}

static void testLargeDocument(void) {
    struct editorDocument document = {0};
    char line[1024]; memset(line, 'a', sizeof(line));
    for (int32_t i = 0; i < 20000; i++)
        bufferInsertRow(&document.buffer, i, line, sizeof(line));
    requested_bytes = allocation_count = 0;
    historyRecordEdit(&document, 200, EDIT_OTHER, 1);
    bufferRowInsertByte(&document.buffer.rows[10000], 0, 'z');
    historyFinishEdit(&document);
    check(document.history.bytes < 2048 && allocation_count == 3,
        "20 MiB document records one changed row with three allocations");
    printf("20 MiB source: history=%zu bytes, requested=%zu bytes, allocations=%zu\n",
        document.history.bytes, requested_bytes, allocation_count);
    release(&document);
}

int main(void) {
    testReplay();
    testStructuralReplay();
    testAllocationRollback();
    testBudget();
    testDepthAndHeldAction();
    testMixedChanges();
    testLargeDocument();
    puts("history tests: ok");
    return 0;
}
