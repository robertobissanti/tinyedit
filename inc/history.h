#ifndef __HISTORY_H
#define __HISTORY_H

#include "tinyedit.h"

/**
 * @brief Copy the document's source rows and cursor for undo.
 *
 * @details Rendering, selection and file metadata are not copied.
 * @return an independently owned snapshot to release with
 * historyFreeSnapshot().
 */
undoSnapshot historyMakeSnapshot(const struct editorDocument *document);
/**
 * @brief Release the source rows owned by a snapshot.
 *
 * @details Resets the snapshot's row pointer and row count; cursor fields are
 * left unchanged.
 */
void historyFreeSnapshot(undoSnapshot *snapshot);
/**
 * @brief Record the document before mutation, coalescing nearby edits.
 *
 * @details max_depth must be positive. type and now determine grouping; every
 * call invalidates redo and the oldest snapshot is dropped when necessary.
 */
void historyRecordEdit(struct editorDocument *document, int32_t max_depth,
    enum undoEditType type, time_t now);
/**
 * @brief Move the current state to redo and take the next undo snapshot.
 *
 * @details The caller must restore and then free that snapshot.
 * @return 1 and transfers a snapshot into the output, or 0 if undo is empty.
 */
uint8_t historyBeginUndo(struct editorDocument *document, undoSnapshot *snapshot);
/**
 * @brief Move the current state to undo and take the next redo snapshot.
 *
 * @details The caller must restore and then free that snapshot.
 * @return 1 and transfers a snapshot into the output, or 0 if redo is empty.
 */
uint8_t historyBeginRedo(struct editorDocument *document, undoSnapshot *snapshot);
/**
 * @brief Replace source text and cursor with a saved snapshot.
 *
 * @details Copies the snapshot rather than consuming it and marks the document
 * dirty. The caller must rebuild rendering and syntax caches afterward.
 */
void historyRestoreSnapshot(struct editorDocument *document, const undoSnapshot *snapshot);
/**
 * @brief Release undo and redo storage and reset grouping state.
 *
 * @details Use on an initialized history when switching documents or exiting.
 * Leaves an empty history that can be reused.
 */
void historyClear(struct editorHistory *history);

#endif
