/* history.c -- bounded row deltas and allocation-free source replay. */

#include "history.h"

#include "alloc.h"
#include "buffer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- allocation sizes ------------------------------------------------ */

/**
 * @brief Reject sizes that cannot be represented before allocating.
 * @details Follows the application's fatal allocation contract and cleanup.
 */
static void historySizeError(void) {
    errno = ENOMEM;
    perror("tinyedit: history size");
    exit(EXIT_FAILURE);
}

/** @brief Calculate an array size without overflowing size_t. */
static size_t historyArrayBytes(size_t count, size_t element_size) {
    if (count > SIZE_MAX / element_size) historySizeError();
    return count * element_size;
}

/* ---- snapshots ------------------------------------------------------- */

/**
 * @brief Copy source rows and cursor into one owned allocation.
 * @details Row descriptors precede their NUL-terminated byte spans. The row
 * strings are borrowed views into that allocation, not separately owned.
 * Empty documents require no allocation; derived caches are not copied.
 */
undoSnapshot historyMakeSnapshot(const struct editorDocument *document) {
    undoSnapshot snapshot = {0};
    snapshot.numrows = document->buffer.row_count;
    snapshot.cx = document->cursor.cx;
    snapshot.cy = document->cursor.cy;
    if (snapshot.numrows == 0) return snapshot;

    size_t descriptors = historyArrayBytes((size_t)snapshot.numrows,
        sizeof(*snapshot.row));
    size_t total = descriptors;
    for (int32_t i = 0; i < snapshot.numrows; i++) {
        size_t bytes = (size_t)document->buffer.rows[i].size + 1;
        if (bytes > SIZE_MAX - total) historySizeError();
        total += bytes;
    }
    snapshot.row = teMalloc(total);
    char *text = (char *)snapshot.row + descriptors;
    for (int32_t i = 0; i < snapshot.numrows; i++) {
        const erow *row = &document->buffer.rows[i];
        snapshot.row[i].size = row->size;
        snapshot.row[i].chars = text;
        size_t bytes = (size_t)row->size + 1;
        memcpy(text, row->chars, bytes);
        text += bytes;
    }
    return snapshot;
}

/**
 * @brief Release the single allocation owned by a snapshot.
 * @details Invalidates all row views and resets storage, retaining the cursor.
 */
void historyFreeSnapshot(undoSnapshot *snapshot) {
    free(snapshot->row);
    snapshot->row = NULL;
    snapshot->numrows = 0;
}

/**
 * @brief Restore source text and cursor without consuming the snapshot.
 * @details Allocates the row vector once, then releases old text before copying
 * replacement spans to avoid holding both document payloads. Allocations remain
 * fatal; this is not a recoverable transaction. The caller rebuilds caches.
 */
void historyRestoreSnapshot(struct editorDocument *document, const undoSnapshot *snapshot) {
    struct editorBuffer restored = {0};
    if (snapshot->numrows > 0) {
        size_t bytes = historyArrayBytes((size_t)snapshot->numrows,
            sizeof(*restored.rows));
        restored.rows = teMalloc(bytes);
        memset(restored.rows, 0, bytes);
        restored.row_count = snapshot->numrows;
        restored.capacity = snapshot->numrows;
    }
    bufferClear(&document->buffer);
    if (restored.row_count > 0) {
        for (int32_t i = 0; i < restored.row_count; i++) {
            erow *row = &restored.rows[i];
            row->size = snapshot->row[i].size;
            size_t text_bytes = (size_t)row->size + 1;
            row->chars = teMalloc(text_bytes);
            row->chars_capacity = text_bytes;
            memcpy(row->chars, snapshot->row[i].chars, text_bytes);
            row->seg_wrapcols = -1;
        }
    }
    document->buffer = restored;
    for (int32_t i = 0; i < restored.row_count; i++)
        document->buffer.rows[i].owner = &document->buffer;
    document->cursor.cx = snapshot->cx;
    document->cursor.cy = snapshot->cy;
    if (document->cursor.cy > document->buffer.row_count)
        document->cursor.cy = document->buffer.row_count;
    document->file.dirty = 1;
}


/* ---- differential ownership ----------------------------------------- */

/** @brief Release reconstructible data without changing source bytes. */
static void historyDiscardDisplay(erow *row) {
    free(row->render); free(row->hl); free(row->seg_start); free(row->seg_start_rx);
    row->render = NULL; row->hl = NULL; row->seg_start = NULL; row->seg_start_rx = NULL;
    row->rsize = 0; row->seg_count = 0; row->seg_wrapcols = -1;
    row->render_dirty = 1;
}

/** @brief Release temporary replay caches while retaining all recorded source. */
void historyReleaseDisplay(struct historyAction *action) {
    for (struct historyChange *change = action->first; change; change = change->next)
        historyDiscardDisplay(&change->spare);
}

/** @brief Release an action and all spans it exclusively owns. */
static void historyFreeAction(struct historyAction *action) {
    struct historyChange *change = action->first;
    while (change) {
        struct historyChange *next = change->next;
        bufferFreeRow(&change->spare);
        free(change);
        change = next;
    }
    free(action);
}

/** @brief Release a complete stack and its accounted bytes. */
static void historyDropStack(struct editorHistory *history, struct historyAction **stack,
    int32_t *count) {
    while (*stack) {
        struct historyAction *action = *stack;
        *stack = action->previous;
        history->bytes -= action->bytes;
        historyFreeAction(action);
    }
    *count = 0;
}

/** @brief Remove the oldest complete undo action. */
static void historyDropOldest(struct editorHistory *history) {
    struct historyAction *oldest = history->undo_stack;
    while (oldest->previous) oldest = oldest->previous;
    if (oldest->next) oldest->next->previous = NULL;
    else history->undo_stack = NULL;
    history->bytes -= oldest->bytes;
    history->undo_count--;
    history->dropped_count++;
    historyFreeAction(oldest);
}

/** @brief Check the budget before requesting additional recording storage.
 * @details Pending actions never evict existing history until successful commit.
 * Both representations of a swap are charged at their maximum source capacity,
 * so replay cannot exceed the original reservation. Derived caches are temporary. */
static uint8_t historyCharge(struct editorHistory *history, size_t bytes) {
    if (history->error != HISTORY_OK) return 0;
    if (bytes > SIZE_MAX - history->pending->bytes) {
        history->error = HISTORY_SIZE;
        return 0;
    }
    if (history->pending->bytes + bytes > history->budget) {
        history->error = HISTORY_LIMIT;
        return 0;
    }
    /* Retained history is bounded independently; the in-flight transaction
     * requires additional rollback storage until its commit succeeds. */
    history->pending->bytes += bytes;
    return 1;
}

/** @brief Allocate a linked change before taking ownership of document data. */
static struct historyChange *historyNewChange(struct editorHistory *history,
    enum historyChangeKind kind, int32_t at, size_t charge) {
    if (charge > SIZE_MAX - sizeof(struct historyChange)) {
        history->error = HISTORY_SIZE;
        return NULL;
    }
    if (!historyCharge(history, sizeof(struct historyChange) + charge)) return NULL;
    struct historyChange *change = teTryMalloc(sizeof(*change));
    if (!change) { history->error = HISTORY_MEMORY; return NULL; }
    memset(change, 0, sizeof(*change));
    change->kind = kind; change->at = at; change->charge = charge;
    change->previous = history->pending->last;
    if (change->previous) change->previous->next = change;
    else history->pending->first = change;
    history->pending->last = change;
    return change;
}

/** @brief Bind rows after a document is moved from a staged loader. */
void historySetBudget(struct editorDocument *document, size_t budget) {
    struct editorHistory *history = &document->history;
    if (history->budget == 0) history->budget = budget ? budget : HISTORY_DEFAULT_BUDGET;
    history->document = document;
    if (document->buffer.history != history) {
        document->buffer.history = history;
        for (int32_t i = 0; i < document->buffer.row_count; i++)
            document->buffer.rows[i].owner = &document->buffer;
    }
}

/**
 * @brief Report whether the pending action has a recoverable preparation error.
 */
uint8_t historyFailed(const struct editorDocument *document) {
    return document->history.error != HISTORY_OK;
}

/** @brief Start an independent rollback boundary even for coalesced edits. */
void historyRecordEdit(struct editorDocument *document, int32_t max_depth,
    enum undoEditType type, time_t now) {
    struct editorHistory *history = &document->history;
    historyFinishEdit(document);
    historySetBudget(document, HISTORY_DEFAULT_BUDGET);
    history->error = HISTORY_OK;
    history->dropped_count = 0;
    history->max_depth = max_depth > 0 ? max_depth : 1;
    if (sizeof(struct historyAction) > history->budget) {
        history->error = HISTORY_LIMIT; return;
    }
    struct historyAction *action = teTryMalloc(sizeof(*action));
    if (!action) { history->error = HISTORY_MEMORY; return; }
    memset(action, 0, sizeof(*action));
    action->before = document->cursor;
    action->selection = document->selection;
    action->dirty = document->file.dirty;
    action->final_newline = document->file.final_newline;
    action->bytes = sizeof(*action);
    action->coalesce = history->undo_stack &&
        (type == EDIT_INSERT || type == EDIT_DELETE) && type == history->last_edit_type &&
        now >= history->last_edit_time && difftime(now, history->last_edit_time) <= UNDO_COALESCE_SECS;
    history->last_edit_type = type;
    history->last_edit_time = now;
    history->pending = action;
}

/** @brief Prepare a row for mutation; see history.h for the 1/0 contract. */
uint8_t historyPrepareRow(erow *row, size_t capacity) {
    struct editorBuffer *buffer = row->owner;
    struct editorHistory *history = buffer ? buffer->history : NULL;
    if (!history) return 1;
    if (history->error != HISTORY_OK) return 0;
    if (!history->pending) return 1;
    int32_t at = (int32_t)(row - buffer->rows);
    struct historyChange *last = history->pending->last;
    if (last && last->kind == HISTORY_ROW_SWAP && last->at == at) {
        if (capacity > last->charge) {
            if (!historyCharge(history, capacity - last->charge)) return 0;
            last->charge = capacity;
        }
        return 1;
    }
    size_t original = row->chars_capacity ? row->chars_capacity : (size_t)row->size + 1;
    size_t charge = capacity > original ? capacity : original;
    struct historyChange *change = historyNewChange(history, HISTORY_ROW_SWAP, at, charge);
    if (!change) return 0;
    char *chars = teTryMalloc(capacity);
    if (!chars) {
        /* No detached data: remove the uninitialized record before rollback. */
        if (change->previous) change->previous->next = NULL;
        else history->pending->first = NULL;
        history->pending->last = change->previous;
        free(change);
        history->error = HISTORY_MEMORY;
        return 0;
    }
    memcpy(chars, row->chars, (size_t)row->size + 1);
    change->spare = *row;
    memset(row, 0, sizeof(*row));
    row->owner = buffer; row->chars = chars; row->size = change->spare.size;
    row->chars_capacity = capacity; row->seg_wrapcols = -1;
    return 1;
}

/** @brief Reserve the vector without losing it or shrinking rollback capacity. */
static uint8_t historyReserveRows(struct editorBuffer *buffer) {
    if (buffer->row_count == INT32_MAX) return 0;
    if (buffer->capacity > buffer->row_count) return 1;
    int32_t capacity = buffer->capacity > 0 ? buffer->capacity : 1;
    if (capacity <= INT32_MAX / 2) capacity *= 2;
    else capacity = INT32_MAX;
    if ((size_t)capacity > SIZE_MAX / sizeof(*buffer->rows)) return 0;
    erow *grown = teTryRealloc(buffer->rows, sizeof(*buffer->rows) * (size_t)capacity);
    if (!grown) return 0;
    buffer->rows = grown; buffer->capacity = capacity;
    return 1;
}

/**
 * @brief Offer a row insertion to the pending action; 1 means handled, even on a recorded failure.
 * @details See history.h: 0 only when no recording is active.
 */
uint8_t historyInsertRow(struct editorBuffer *buffer, int32_t at,
    const char *text, size_t len) {
    struct editorHistory *history = buffer->history;
    if (!history || (!history->pending && history->error == HISTORY_OK)) return 0;
    if (history->error != HISTORY_OK) return 1;
    if (len > INT32_MAX) { history->error = HISTORY_SIZE; return 1; }
    if (buffer->row_count == INT32_MAX) { history->error = HISTORY_SIZE; return 1; }
    /* Allocate before vector growth: text may point into an existing row. */
    struct historyChange *change = historyNewChange(history, HISTORY_ROW_INSERT, at, len + 1);
    if (!change) return 1;
    char *chars = teTryMalloc(len + 1);
    if (chars) { memcpy(chars, text, len); chars[len] = '\0'; }
    if (!chars || !historyReserveRows(buffer)) {
        free(chars);
        if (change->previous) change->previous->next = NULL;
        else history->pending->first = NULL;
        history->pending->last = change->previous;
        free(change); history->error = HISTORY_MEMORY;
        return 1;
    }
    memmove(buffer->rows + at + 1, buffer->rows + at,
        sizeof(*buffer->rows) * (size_t)(buffer->row_count - at));
    erow *row = buffer->rows + at;
    memset(row, 0, sizeof(*row));
    row->owner = buffer; row->size = (int32_t)len; row->chars = chars;
    row->chars_capacity = len + 1; row->seg_wrapcols = -1;
    buffer->row_count++;
    return 1;
}

/**
 * @brief Offer a row deletion to the pending action; 1 means handled, even on a recorded failure.
 * @details See history.h: 0 only when no recording is active.
 */
uint8_t historyDeleteRow(struct editorBuffer *buffer, int32_t at) {
    struct editorHistory *history = buffer->history;
    if (!history || (!history->pending && history->error == HISTORY_OK)) return 0;
    if (history->error != HISTORY_OK) return 1;
    erow *row = buffer->rows + at;
    size_t charge = row->chars_capacity ? row->chars_capacity : (size_t)row->size + 1;
    struct historyChange *change = historyNewChange(history, HISTORY_ROW_DELETE, at, charge);
    if (!change) return 1;
    change->spare = *row;
    memmove(row, row + 1, sizeof(*row) * (size_t)(buffer->row_count - at - 1));
    buffer->row_count--;
    return 1;
}

/** @brief Exchange owned source representations without allocating. */
static void historyApplyChange(struct editorBuffer *buffer,
    struct historyChange *change, uint8_t reverse) {
    int32_t at = change->at;
    if (change->kind == HISTORY_ROW_SWAP) {
        erow row = buffer->rows[at];
        buffer->rows[at] = change->spare;
        change->spare = row;
    } else if ((change->kind == HISTORY_ROW_INSERT && reverse) ||
               (change->kind == HISTORY_ROW_DELETE && !reverse)) {
        change->spare = buffer->rows[at];
        memmove(buffer->rows + at, buffer->rows + at + 1,
            sizeof(*buffer->rows) * (size_t)(buffer->row_count - at - 1));
        buffer->row_count--;
    } else {
        memmove(buffer->rows + at + 1, buffer->rows + at,
            sizeof(*buffer->rows) * (size_t)(buffer->row_count - at));
        buffer->rows[at] = change->spare;
        memset(&change->spare, 0, sizeof(change->spare));
        buffer->row_count++;
    }
    if (at < buffer->row_count) buffer->rows[at].owner = buffer;
}

/** @brief Commit a complete action, or restore all of its previous owned rows. */
enum historyError historyFinishEdit(struct editorDocument *document) {
    struct editorHistory *history = &document->history;
    struct historyAction *action = history->pending;
    if (!action) return history->error;
    if (history->hold && history->error == HISTORY_OK) return HISTORY_OK;
    history->pending = NULL;
    if (history->error != HISTORY_OK) {
        for (struct historyChange *change = action->last; change; change = change->previous)
            historyApplyChange(&document->buffer, change, 1);
        document->cursor = action->before;
        document->selection = action->selection;
        document->file.dirty = action->dirty;
        document->file.final_newline = action->final_newline;
        historyFreeAction(action);
        history->last_edit_type = EDIT_NONE;
        return history->error;
    }
    if (!action->first) { historyFreeAction(action); return HISTORY_OK; }
    action->after = document->cursor;
    historyReleaseDisplay(action);
    historyDropStack(history, &history->redo_stack, &history->redo_count);
    uint8_t coalesce = action->coalesce && history->undo_stack &&
        history->undo_stack->bytes <= history->budget - action->bytes;
    while (history->undo_stack && (history->bytes > history->budget - action->bytes ||
        (coalesce ? history->undo_count > history->max_depth :
            history->undo_count >= history->max_depth))) {
        if (history->undo_count == 1) coalesce = 0;
        historyDropOldest(history);
    }
    if (coalesce && history->undo_stack) {
        struct historyAction *previous = history->undo_stack;
        previous->last->next = action->first;
        action->first->previous = previous->last;
        previous->last = action->last;
        previous->after = action->after;
        previous->bytes += action->bytes - sizeof(*action);
        history->bytes += action->bytes - sizeof(*action);
        free(action);
    } else {
        action->previous = history->undo_stack;
        if (action->previous) action->previous->next = action;
        history->undo_stack = action;
        history->undo_count++;
        history->bytes += action->bytes;
    }
    return HISTORY_OK;
}

/** @brief Move one complete action between stacks by swapping source ownership. */
static uint8_t historyReplay(struct editorDocument *document, uint8_t reverse) {
    struct editorHistory *history = &document->history;
    if (historyFinishEdit(document) != HISTORY_OK) return 0;
    struct historyAction **source = reverse ? &history->undo_stack : &history->redo_stack;
    struct historyAction **target = reverse ? &history->redo_stack : &history->undo_stack;
    struct historyAction *action = *source;
    if (!action) return 0;
    for (struct historyChange *change = reverse ? action->last : action->first;
         change; change = reverse ? change->previous : change->next)
        historyApplyChange(&document->buffer, change, reverse);
    *source = action->previous;
    if (*source) (*source)->next = NULL;
    action->previous = *target; action->next = NULL;
    if (*target) (*target)->next = action;
    *target = action;
    if (reverse) { history->undo_count--; history->redo_count++; }
    else { history->redo_count--; history->undo_count++; }
    historyReleaseDisplay(action);
    document->cursor = reverse ? action->before : action->after;
    document->file.dirty = 1;
    history->last_edit_type = EDIT_NONE;
    return 1;
}

/**
 * @brief Restore the previous action; return zero if absent. Caller rebuilds display data.
 */
uint8_t historyUndo(struct editorDocument *document) { return historyReplay(document, 1); }
/**
 * @brief Replay the next action; return zero if absent. Caller rebuilds display data.
 */
uint8_t historyRedo(struct editorDocument *document) { return historyReplay(document, 0); }

/**
 * @brief Release owned pending, undo and redo actions and reset history.
 */
void historyClear(struct editorHistory *history) {
    history->hold = 0;
    if (history->pending && history->document) historyFinishEdit(history->document);
    historyDropStack(history, &history->undo_stack, &history->undo_count);
    historyDropStack(history, &history->redo_stack, &history->redo_count);
    memset(history, 0, sizeof(*history));
}
