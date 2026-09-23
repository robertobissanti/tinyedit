#include "editor_state.h"

static const struct autoClosePair auto_close_pairs[] = {
    { '(', ')' }, { '{', '}' }, { '[', ']' },
    { '"', '"' }, { '\'', '\'' }, { '$', '$' }, { '`', '`' },
};

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

const struct autoClosePair *editorAutoClosePairFor(int32_t byte,
    uint8_t allow_single_quote) {
    if (byte == '\'' && !allow_single_quote) return NULL;
    int32_t count = (int32_t)(sizeof(auto_close_pairs) / sizeof(auto_close_pairs[0]));
    for (int32_t i = 0; i < count; i++)
        if (auto_close_pairs[i].open == byte) return &auto_close_pairs[i];
    return NULL;
}

uint8_t editorIsAsymmetricAutoClose(const struct autoClosePair *pair,
    int32_t close_byte) {
    return pair && pair->close == close_byte && pair->open != pair->close;
}
