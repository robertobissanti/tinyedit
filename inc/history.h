/* history.h -- row deltas, bounded ownership and recoverable recording. */
#ifndef __HISTORY_H
#define __HISTORY_H

#include "tinyedit.h"

#define HISTORY_DEFAULT_BUDGET ((size_t)64 * 1024 * 1024)

/* Snapshot utilities for benchmarks; application undo uses deltas below.
 * One allocation owns all spans. Empty snapshots allocate nothing. */
/**
 * @brief Copy source rows and cursor into one owned allocation.
 * @details Row descriptors precede their NUL-terminated byte spans. The row
 * strings are borrowed views into that allocation, not separately owned.
 * Empty documents require no allocation; derived caches are not copied.
 */
undoSnapshot historyMakeSnapshot(const struct editorDocument *document);
/**
 * @brief Release the single allocation owned by a snapshot.
 * @details Invalidates all row views and resets storage, retaining the cursor.
 */
void historyFreeSnapshot(undoSnapshot *snapshot);
/**
 * @brief Restore source text and cursor without consuming the snapshot.
 * @details Allocates the row vector once, then releases old text before copying
 * replacement spans to avoid holding both document payloads. Allocations remain
 * fatal; this is not a recoverable transaction. The caller rebuilds caches.
 */
void historyRestoreSnapshot(struct editorDocument *document, const undoSnapshot *snapshot);

/* Start recording before source mutations. Finish commits, or rolls back the
 * entire pending action without allocation if any prepare operation failed.
 * The first budget set remains active until historyClear/document reset. */
/** @brief Bind rows after a document is moved from a staged loader. */
void historySetBudget(struct editorDocument *document, size_t budget);
/** @brief Start an independent rollback boundary even for coalesced edits. */
void historyRecordEdit(struct editorDocument *document, int32_t max_depth,
    enum undoEditType type, time_t now);
/** @brief Commit a complete action, or restore all of its previous owned rows. */
enum historyError historyFinishEdit(struct editorDocument *document);
/** @brief Report whether the pending action has a recoverable preparation error. */
uint8_t historyFailed(const struct editorDocument *document);

/* Buffer-only recording hooks. Preparations leave source unchanged on failure.
 * Changes own detached rows; all row pointers are invalidated by structural edits. */
/** @brief Prepare a row for in-place mutation, detaching its old source into the pending action.
 * @details capacity is the byte size (with NUL) the row will need. Returns 1 when
 * the caller may mutate the row: no history, no pending action, or the old source
 * was detached and a fresh allocation installed. Returns 0, with the cause in
 * history->error, when the mutation must not happen; the action rolls back at
 * historyFinishEdit(). Row pointers stay valid; the row's derived caches are dropped. */
uint8_t historyPrepareRow(erow *row, size_t capacity);
/** @brief Offer a row insertion to the pending action.
 * @details Return value means "handled", not "succeeded". 0: no recording is
 * active, nothing was done and the caller inserts the row directly. 1: the hook
 * took over; check historyFailed() afterwards, because a budget, size or memory
 * failure is also reported as 1, leaves the buffer unchanged and sets
 * history->error. On success the buffer owns a copy of text (len bytes, no
 * terminator needed) and all row pointers are invalidated. */
uint8_t historyInsertRow(struct editorBuffer *buffer, int32_t at,
    const char *text, size_t len);
/** @brief Offer a row deletion to the pending action.
 * @details Same protocol as historyInsertRow(): 0 means no recording is active and
 * the caller frees and removes the row; 1 means handled, including a recorded
 * failure (check historyFailed()), in which case the row stays in the buffer.
 * On success the pending action owns the detached row (its text and caches) and
 * all row pointers are invalidated. at must be a valid row index. */
uint8_t historyDeleteRow(struct editorBuffer *buffer, int32_t at);

/* Replay moves owned row spans; no source allocation is needed. 0 is empty,
 * 1 is success. Finishes any pending action first. Caller rebuilds derived data. */
/** @brief Restore the previous action; return zero if absent. Caller rebuilds display data. */
uint8_t historyUndo(struct editorDocument *document);
/** @brief Replay the next action; return zero if absent. Caller rebuilds display data. */
uint8_t historyRedo(struct editorDocument *document);
/** @brief Release temporary replay caches while retaining all recorded source. */
void historyReleaseDisplay(struct historyAction *action);
/** @brief Release owned pending, undo and redo actions and reset history. */
void historyClear(struct editorHistory *history);

#endif
