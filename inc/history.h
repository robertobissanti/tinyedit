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
/** @brief Detach only a changed row, preparing its new source allocation first. */
uint8_t historyPrepareRow(erow *row, size_t capacity);
/** @brief Insert copied source bytes at a row index; return zero without mutation on failure. */
uint8_t historyInsertRow(struct editorBuffer *buffer, int32_t at,
    const char *text, size_t len);
/** @brief Detach the indexed source row into the pending action; return zero on failure. */
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
