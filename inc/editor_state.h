/* editor_state.h -- selection normalization and ASCII auto-close policy. */

#ifndef __EDITOR_STATE_H
#define __EDITOR_STATE_H

#include "tinyedit.h"

/**
 * @brief Order the selection anchor and cursor into a half-open range.
 *
 * @details Writes zero-based logical rows and source-byte offsets only for a
 * nonempty active selection.
 * @return 1 on success, otherwise 0.
 */
uint8_t editorSelectionRange(const struct editorSelection *selection,
    const struct editorCursor *cursor, int32_t *start_y, int32_t *start_x,
    int32_t *end_y, int32_t *end_x);
/**
 * @brief Find the ASCII pair associated with an opening byte.
 *
 * @details allow_single_quote controls apostrophe pairing.
 * @return a borrowed table entry or NULL; do not free or modify it.
 */
const struct autoClosePair *editorAutoClosePairFor(int32_t byte,
    uint8_t allow_single_quote);
/**
 * @brief Recognize a closing byte from the shared asymmetric ASCII pairs.
 *
 * @return 1 for a registered distinct closer; symmetric delimiters and
 * unrelated bytes return 0. The caller checks the byte at the cursor.
 */
uint8_t editorIsAsymmetricAutoClose(int32_t close_byte);

#endif /* __EDITOR_STATE_H */
