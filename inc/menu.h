/* menu.h -- keyboard and mouse state for the terminal menu overlay. */

#ifndef TE_MENU_H
#define TE_MENU_H

#include "command.h"

#include <stdint.h>

struct menuDefinition {
    const char *label;
    const enum editorCommand *items;
    int32_t item_count;
};

typedef void (*menuAppendFn)(void *context, const char *text, int32_t len);

struct editorMenu {
    uint8_t open;
    int32_t selected_menu;
    int32_t selected_item;
};

/**
 * @brief Initialize a closed menu with the first actionable entry selected.
 *
 * @details Call on a new editorMenu before passing it to input or drawing
 * helpers.
 */
void menuInit(struct editorMenu *menu);
/**
 * @brief Open the menu at the first title and actionable entry.
 *
 * @details Resets the previous menu selection; does not draw or change
 * terminal reporting modes.
 */
void menuOpen(struct editorMenu *menu);
/**
 * @brief Update menu navigation and return an accepted command.
 *
 * @details key is a decoded editorKey or raw byte.
 * @return CMD_NONE for navigation, dismissal or an inactive menu; the caller
 * executes any returned command.
 */
enum editorCommand menuHandleKey(struct editorMenu *menu, int32_t key);
/**
 * @brief Update menu navigation from a decoded mouse report.
 *
 * @details row, col and menu_row are one-based terminal coordinates. pressed
 * selects on button-down and motion handles hover; release over an item
 * returns its command and closes the popup.
 * @param row One-based terminal row of the mouse report.
 * @param col One-based terminal column of the mouse report.
 * @param menu_row One-based terminal row occupied by the menu bar.
 * @param screencols Visible terminal width, shared with the drawing helpers.
 * @param pressed Whether the report is a button press.
 * @param motion Whether the report is a motion event.
 */
enum editorCommand menuHandleMouse(struct editorMenu *menu, int32_t row,
    int32_t col, int32_t menu_row, int32_t screencols, uint8_t pressed, uint8_t motion);
/**
 * @brief Append the menu titles using the current UI colors.
 *
 * @details append copies bytes to the caller's output buffer through context.
 * Text is clipped to screencols at grapheme boundaries.
 * Call at the bar's screen position; this helper also appends the line ending.
 */
void menuDrawBar(const struct editorMenu *menu,
    const struct editorSettings *settings, int32_t screencols,
    menuAppendFn append, void *context);
/**
 * @brief Append an open menu's border, items and setting checkmarks.
 *
 * @details menu_row is the one-based bar row. Uses absolute cursor positions
 * through append; clips and positions the popup within screencols.
 * Does nothing when closed. The caller restores final frame
 * attributes.
 */
void menuDrawPopup(const struct editorMenu *menu,
    const struct editorSettings *settings, int32_t menu_row, int32_t screencols,
    menuAppendFn append, void *context);

#endif
