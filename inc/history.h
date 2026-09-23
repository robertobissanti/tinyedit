#ifndef __HISTORY_H
#define __HISTORY_H

#include "tinyedit.h"

undoSnapshot historyMakeSnapshot(const struct editorDocument *document);
void historyFreeSnapshot(undoSnapshot *snapshot);
void historyRecordEdit(struct editorDocument *document, int32_t max_depth,
    enum undoEditType type, time_t now);
uint8_t historyBeginUndo(struct editorDocument *document, undoSnapshot *snapshot);
uint8_t historyBeginRedo(struct editorDocument *document, undoSnapshot *snapshot);
void historyRestoreSnapshot(struct editorDocument *document, const undoSnapshot *snapshot);
void historyClear(struct editorHistory *history);

#endif
