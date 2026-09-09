#include "history.h"

#include "alloc.h"
#include "buffer.h"

#include <stdlib.h>
#include <string.h>

undoSnapshot historyMakeSnapshot(const struct editorDocument *document) {
    undoSnapshot snapshot;
    snapshot.numrows = document->buffer.row_count;
    snapshot.cx = document->cursor.cx;
    snapshot.cy = document->cursor.cy;
    snapshot.row = teMalloc(sizeof(undoRow) * (size_t)document->buffer.row_count);
    for (int32_t i = 0; i < document->buffer.row_count; i++) {
        snapshot.row[i].size = document->buffer.rows[i].size;
        snapshot.row[i].chars = teMalloc((size_t)document->buffer.rows[i].size + 1);
        memcpy(snapshot.row[i].chars, document->buffer.rows[i].chars,
            (size_t)document->buffer.rows[i].size + 1);
    }
    return snapshot;
}

void historyFreeSnapshot(undoSnapshot *snapshot) {
    for (int32_t i = 0; i < snapshot->numrows; i++) free(snapshot->row[i].chars);
    free(snapshot->row);
    snapshot->row = NULL;
    snapshot->numrows = 0;
}

static void clearStack(undoSnapshot *stack, int32_t count) {
    for (int32_t i = 0; i < count; i++) historyFreeSnapshot(&stack[i]);
}

static void clearRedo(struct editorHistory *history) {
    clearStack(history->redo_stack, history->redo_count);
    history->redo_count = 0;
}

void historyRecordEdit(struct editorDocument *document, int32_t max_depth,
    enum undoEditType type, time_t now) {
    struct editorHistory *history = &document->history;
    uint8_t coalesce = type != EDIT_OTHER && type == history->last_edit_type &&
        now - history->last_edit_time <= UNDO_COALESCE_SECS;
    clearRedo(history);
    history->last_edit_type = type;
    history->last_edit_time = now;
    if (coalesce) return;

    if (history->undo_count >= max_depth) {
        historyFreeSnapshot(&history->undo_stack[0]);
        memmove(&history->undo_stack[0], &history->undo_stack[1],
            sizeof(undoSnapshot) * (size_t)(history->undo_count - 1));
        history->undo_count--;
    }
    history->undo_stack = teRealloc(history->undo_stack,
        sizeof(undoSnapshot) * (size_t)(history->undo_count + 1));
    history->undo_stack[history->undo_count++] = historyMakeSnapshot(document);
}

static undoSnapshot popSnapshot(undoSnapshot **stack, int32_t *count) {
    undoSnapshot snapshot = (*stack)[--*count];
    *stack = teRealloc(*stack, sizeof(undoSnapshot) * (size_t)(*count > 0 ? *count : 1));
    return snapshot;
}

uint8_t historyBeginUndo(struct editorDocument *document, undoSnapshot *snapshot) {
    struct editorHistory *history = &document->history;
    if (history->undo_count == 0) return 0;
    history->redo_stack = teRealloc(history->redo_stack,
        sizeof(undoSnapshot) * (size_t)(history->redo_count + 1));
    history->redo_stack[history->redo_count++] = historyMakeSnapshot(document);
    *snapshot = popSnapshot(&history->undo_stack, &history->undo_count);
    history->last_edit_type = EDIT_NONE;
    return 1;
}

uint8_t historyBeginRedo(struct editorDocument *document, undoSnapshot *snapshot) {
    struct editorHistory *history = &document->history;
    if (history->redo_count == 0) return 0;
    history->undo_stack = teRealloc(history->undo_stack,
        sizeof(undoSnapshot) * (size_t)(history->undo_count + 1));
    history->undo_stack[history->undo_count++] = historyMakeSnapshot(document);
    *snapshot = popSnapshot(&history->redo_stack, &history->redo_count);
    history->last_edit_type = EDIT_NONE;
    return 1;
}

void historyRestoreSnapshot(struct editorDocument *document, const undoSnapshot *snapshot) {
    bufferClear(&document->buffer);
    for (int32_t i = 0; i < snapshot->numrows; i++)
        bufferInsertRow(&document->buffer, document->buffer.row_count,
            snapshot->row[i].chars, (size_t)snapshot->row[i].size);
    document->cursor.cx = snapshot->cx;
    document->cursor.cy = snapshot->cy;
    if (document->cursor.cy > document->buffer.row_count)
        document->cursor.cy = document->buffer.row_count;
    document->file.dirty = 1;
}

void historyClear(struct editorHistory *history) {
    clearStack(history->undo_stack, history->undo_count);
    free(history->undo_stack);
    clearStack(history->redo_stack, history->redo_count);
    free(history->redo_stack);
    memset(history, 0, sizeof(*history));
    history->last_edit_type = EDIT_NONE;
}
