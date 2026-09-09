#ifndef __EDITOR_STATE_H
#define __EDITOR_STATE_H

#include "tinyedit.h"

uint8_t editorSelectionRange(const struct editorSelection *selection,
    const struct editorCursor *cursor, int32_t *start_y, int32_t *start_x,
    int32_t *end_y, int32_t *end_x);

#endif /* __EDITOR_STATE_H */
