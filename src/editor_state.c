/* editor_state.c -- pure selection ranges and shared ASCII pair policy. */

#include "editor_state.h"

/* ASCII quote/backtick/math delimiters are symmetric. Apostrophe pairing is
 * opt-in; Unicode quotes and XML tags have separate text-dependent paths. */
static const struct autoClosePair auto_close_pairs[] = {
    { '(', ')' }, { '{', '}' }, { '[', ']' },
    { '"', '"' }, { '\'', '\'' }, { '$', '$' }, { '`', '`' },
};

/**
 * @brief Order the selection anchor and cursor into a half-open range.
 *
 * @details Writes zero-based logical rows and source-byte offsets only for a
 * nonempty active selection.
 * @return 1 on success, otherwise 0.
 */
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

/**
 * @brief Find the ASCII pair associated with an opening byte.
 *
 * @details allow_single_quote controls apostrophe pairing.
 * @return a borrowed table entry or NULL; do not free or modify it.
 */
const struct autoClosePair *editorAutoClosePairFor(int32_t byte,
    uint8_t allow_single_quote) {
    if (byte == '\'' && !allow_single_quote) return NULL;
    int32_t count = (int32_t)(sizeof(auto_close_pairs) / sizeof(auto_close_pairs[0]));
    for (int32_t i = 0; i < count; i++)
        if (auto_close_pairs[i].open == byte) return &auto_close_pairs[i];
    return NULL;
}

/**
 * @brief Recognize a closing byte from the shared asymmetric ASCII pairs.
 * @details Symmetric delimiters are handled as openers; their skip policy and
 * document-dependent behavior remain in the editing command.
 */
uint8_t editorIsAsymmetricAutoClose(int32_t close_byte) {
    int32_t count = (int32_t)(sizeof(auto_close_pairs) / sizeof(auto_close_pairs[0]));
    for (int32_t i = 0; i < count; i++) {
        const struct autoClosePair *pair = &auto_close_pairs[i];
        if (pair->close == close_byte && pair->open != pair->close) return 1;
    }
    return 0;
}
