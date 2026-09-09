#include "editor_state.h"

uint8_t editorSelectionRange(const struct editorSelection *selection,
    const struct editorCursor *cursor, int32_t *start_y, int32_t *start_x,
    int32_t *end_y, int32_t *end_x) {
    if (!selection->active) return 0;

    int32_t ay = selection->anchor_y;
    int32_t ax = selection->anchor_x;
    int32_t cy = cursor->cy;
    int32_t cx = cursor->cx;
    if (ay == cy && ax == cx) return 0;

    if (ay < cy || (ay == cy && ax <= cx)) {
        *start_y = ay;
        *start_x = ax;
        *end_y = cy;
        *end_x = cx;
    } else {
        *start_y = cy;
        *start_x = cx;
        *end_y = ay;
        *end_x = ax;
    }
    return 1;
}
