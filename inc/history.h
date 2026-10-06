/* history.h -- row deltas, bounded ownership and recoverable recording. */
#ifndef __HISTORY_H
#define __HISTORY_H

#include "tinyedit.h"

#define HISTORY_DEFAULT_BUDGET ((size_t)64 * 1024 * 1024)

/* Snapshot utilities for benchmarks; application undo uses deltas below.
 * One allocation owns all spans. Empty snapshots allocate nothing. */
undoSnapshot historyMakeSnapshot(const struct editorDocument *document);
void historyFreeSnapshot(undoSnapshot *snapshot);
void historyRestoreSnapshot(struct editorDocument *document, const undoSnapshot *snapshot);

/* Start recording before source mutations. Finish commits, or rolls back the
 * entire pending action without allocation if any prepare operation failed.
 * The first budget set remains active until historyClear/document reset. */
void historySetBudget(struct editorDocument *document, size_t budget);
void historyRecordEdit(struct editorDocument *document, int32_t max_depth,
    enum undoEditType type, time_t now);
enum historyError historyFinishEdit(struct editorDocument *document);
uint8_t historyFailed(const struct editorDocument *document);

/* Buffer-only recording hooks. Preparations leave source unchanged on failure.
 * Changes own detached rows; all row pointers are invalidated by structural edits. */
uint8_t historyPrepareRow(erow *row, size_t capacity);
uint8_t historyInsertRow(struct editorBuffer *buffer, int32_t at,
    const char *text, size_t len);
uint8_t historyDeleteRow(struct editorBuffer *buffer, int32_t at);

/* Replay moves owned row spans; no source allocation is needed. 0 is empty,
 * 1 is success. Finishes any pending action first. Caller rebuilds derived data. */
uint8_t historyUndo(struct editorDocument *document);
uint8_t historyRedo(struct editorDocument *document);
void historyReleaseDisplay(struct historyAction *action);
void historyClear(struct editorHistory *history);

#endif
