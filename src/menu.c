/* menu.c -- rectangular menu overlay and its keyboard/mouse navigation. */

#include "menu.h"
#include "tinyedit.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>



static const enum editorCommand app_items[] = {
    CMD_INFO, CMD_SETTINGS, CMD_NONE, CMD_QUIT
};
static const enum editorCommand file_items[] = {
    CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_NONE, CMD_CLOSE
};
static const enum editorCommand edit_items[] = {
    CMD_UNDO, CMD_REDO, CMD_NONE, CMD_CUT, CMD_COPY, CMD_PASTE,
    CMD_NONE, CMD_SELECT_ALL, CMD_FIND
};
static const enum editorCommand view_items[] = {
    CMD_TOGGLE_LINE_NUMBERS, CMD_TOGGLE_TOP_BAR, CMD_TOGGLE_MENU,
    CMD_TOGGLE_INVISIBLES, CMD_TOGGLE_SYNTAX_HIGHLIGHT,
    CMD_TOGGLE_AUTO_INDENT, CMD_TOGGLE_CURSOR_BLINK, CMD_NONE, CMD_TOGGLE_TREE
};
static const enum editorCommand help_items[] = { CMD_HELP };

static const struct menuDefinition menus[] = {
    { "TinyEdit", app_items, (int32_t)(sizeof(app_items) / sizeof(app_items[0])) },
    { "File", file_items, (int32_t)(sizeof(file_items) / sizeof(file_items[0])) },
    { "Edit", edit_items, (int32_t)(sizeof(edit_items) / sizeof(edit_items[0])) },
    { "View", view_items, (int32_t)(sizeof(view_items) / sizeof(view_items[0])) },
    { "Help", help_items, (int32_t)(sizeof(help_items) / sizeof(help_items[0])) }
};
static const int32_t menu_count = (int32_t)(sizeof(menus) / sizeof(menus[0]));

/**
 * @brief Find the first actionable entry in a menu.
 *
 * @details menu_index must identify a built-in menu.
 * @return an item index, skipping separator entries.
 */
static int32_t menuFirstItem(int32_t menu_index) {
    for (int32_t i = 0; i < menus[menu_index].item_count; i++) {
        if (menus[menu_index].items[i] != CMD_NONE) return i;
    }
    return 0;
}

/**
 * @brief Move between actionable items with wraparound.
 *
 * @details menu_index and item are valid table indices and delta is +1 or -1.
 * @return the next item index, skipping separators.
 */
static int32_t menuNextItem(int32_t menu_index, int32_t item, int32_t delta) {
    int32_t count = menus[menu_index].item_count;
    do {
        item = (item + delta + count) % count;
    } while (menus[menu_index].items[item] == CMD_NONE);
    return item;
}

/**
 * @brief Measure borrowed menu text in display columns.
 */
static int32_t menuTextWidth(const char *text) {
    size_t width = utf8StrWidth(text, strlen(text));
    return width < INT32_MAX - 8 ? (int32_t)width : INT32_MAX - 8;
}

/**
 * @brief Locate a menu title's one-based terminal column.
 * @details Saturates when a title is beyond representable screen coordinates.
 */
static int32_t menuStartColumn(int32_t menu_index) {
    int32_t col = 1;
    for (int32_t i = 0; i < menu_index; i++) {
        int32_t advance = menuTextWidth(menus[i].label) + 2;
        if (advance > INT32_MAX - col) return INT32_MAX;
        col += advance;
    }
    return col;
}

/**
 * @brief Measure a popup's content width including shortcuts and checkmarks.
 *
 * @details definition must refer to valid command entries.
 * @return a column width excluding the border characters.
 */
static int32_t menuWidth(const struct menuDefinition *definition) {
    int32_t width = 0;
    for (int32_t i = 0; i < definition->item_count; i++) {
        enum editorCommand command = definition->items[i];
        if (command == CMD_NONE) continue;
        const struct commandDescriptor *descriptor = commandGetDescriptor(command);
        int32_t item_width = menuTextWidth(descriptor->label) + 2;
        if (commandIsSetting(command)) item_width += 4;
        if (descriptor->shortcut) {
            int32_t shortcut_width = menuTextWidth(descriptor->shortcut) + 1;
            item_width = shortcut_width > INT32_MAX - 2 - item_width
                ? INT32_MAX - 2 : item_width + shortcut_width;
        }
        if (item_width > width) width = item_width;
    }
    return width;
}

/* Clip at grapheme boundaries: byte counts and terminal columns differ. */
/**
 * @brief Append borrowed menu text within a display-column budget.
 */
static int32_t menuAppendText(menuAppendFn append, void *context,
    const char *text, int32_t columns) {
    size_t len = strlen(text), offset = 0;
    int32_t used = 0;
    while (offset < len && used < columns) {
        size_t step = utf8NextCharLen(text, offset, len);
        int32_t width = utf8SingleCharWidth(text + offset, step);
        if (width > columns - used || step > INT32_MAX) break;
        append(context, text + offset, (int32_t)step);
        offset += step;
        used += width;
    }
    return used;
}

/**
 * @brief Compute popup width in screen columns from the selected menu.
 */
static int32_t menuPopupWidth(int32_t index, int32_t screencols) {
    if (screencols < 3) return 0;
    int32_t width = menuWidth(&menus[index]);
    return width < screencols - 2 ? width : screencols - 2;
}

/**
 * @brief Compute the popup starting screen column.
 */
static int32_t menuPopupStart(int32_t index, int32_t width, int32_t screencols) {
    int32_t start = menuStartColumn(index);
    return start < screencols - width ? start : screencols - width - 1;
}

/**
 * @brief Append one menu item with its selection and enabled state.
 */
static void menuAppendItem(menuAppendFn append, void *context,
    const char *mark, const struct commandDescriptor *descriptor, int32_t width) {
    const char *shortcut = descriptor->shortcut ? descriptor->shortcut : "";
    size_t measured = utf8StrWidth(shortcut, strlen(shortcut));
    int32_t shortcut_width = measured < (size_t)width ? (int32_t)measured : width;
    int32_t label_width = width - shortcut_width;
    if (shortcut_width && label_width) label_width--;
    int32_t used = menuAppendText(append, context, mark, label_width);
    used += menuAppendText(append, context, descriptor->label, label_width - used);
    for (; used < width - shortcut_width; used++) append(context, " ", 1);
    used += menuAppendText(append, context, shortcut, shortcut_width);
    for (; used < width; used++) append(context, " ", 1);
}

/**
 * @brief Append text preceded by an absolute terminal cursor position.
 *
 * @details row and col are one-based. append must copy bytes immediately;
 * context is passed through unchanged.
 */
static void menuAppendAt(menuAppendFn append, void *context, int32_t row,
    int32_t col, const char *text) {
    char position[32];
    int32_t len = snprintf(position, sizeof(position), "\x1b[%d;%dH", row, col);
    append(context, position, len);
    append(context, text, (int32_t)strlen(text));
}

/**
 * @brief Append a horizontal popup border or separator.
 *
 * @details row and col are one-based, width is the number of interior rule
 * cells. left and right are the three-byte UTF-8 border glyphs used by this
 * module.
 */
static void menuAppendRule(menuAppendFn append, void *context, int32_t row,
    int32_t col, const char *left, const char *right, int32_t width) {
    menuAppendAt(append, context, row, col, left);
    for (int32_t i = 0; i < width; i++) append(context, "─", 3);
    append(context, right, 3);
}

/**
 * @brief Initialize a closed menu with the first actionable entry selected.
 *
 * @details Call on a new editorMenu before passing it to input or drawing
 * helpers.
 */
void menuInit(struct editorMenu *menu) {
    menu->open = 0;
    menu->selected_menu = 0;
    menu->selected_item = menuFirstItem(0);
}

/**
 * @brief Open the menu at the first title and actionable entry.
 *
 * @details Resets the previous menu selection; does not draw or change
 * terminal reporting modes.
 */
void menuOpen(struct editorMenu *menu) {
    menu->open = 1;
    menu->selected_menu = 0;
    menu->selected_item = menuFirstItem(0);
}

/**
 * @brief Update menu navigation and return an accepted command.
 *
 * @details key is a decoded editorKey or raw byte.
 * @return CMD_NONE for navigation, dismissal or an inactive menu; the caller
 * executes any returned command.
 */
enum editorCommand menuHandleKey(struct editorMenu *menu, int32_t key) {
    if (key == F10_KEY) {
        if (menu->open) menu->open = 0;
        else menuOpen(menu);
        return CMD_NONE;
    }
    if (!menu->open) return CMD_NONE;

    if (key == '\x1b') {
        menu->open = 0;
    } else if (key == ARROW_LEFT || key == ARROW_RIGHT) {
        int32_t delta = key == ARROW_LEFT ? -1 : 1;
        menu->selected_menu = (menu->selected_menu + delta + menu_count) % menu_count;
        menu->selected_item = menuFirstItem(menu->selected_menu);
    } else if (key == ARROW_UP || key == ARROW_DOWN) {
        menu->selected_item = menuNextItem(menu->selected_menu,
            menu->selected_item, key == ARROW_UP ? -1 : 1);
    } else if (key == '\r') {
        enum editorCommand command = menus[menu->selected_menu].items[menu->selected_item];
        menu->open = 0;
        return command;
    }
    return CMD_NONE;
}

/**
 * @brief Update menu navigation from a decoded mouse report.
 *
 * @details row, col and menu_row are one-based terminal coordinates. pressed
 * selects on button-down and motion handles hover; release over an item
 * returns its command and closes the popup.
 * @param row One-based terminal row of the mouse report.
 * @param col One-based terminal column of the mouse report.
 * @param menu_row One-based terminal row occupied by the menu bar.
 * @param pressed Whether the report is a button press.
 * @param motion Whether the report is a motion event.
 */
enum editorCommand menuHandleMouse(struct editorMenu *menu, int32_t row,
    int32_t col, int32_t menu_row, int32_t screencols, uint8_t pressed, uint8_t motion) {
    if (col < 1 || col > screencols) return CMD_NONE;
    if (row == menu_row) {
        for (int32_t i = 0; i < menu_count; i++) {
            int32_t start = menuStartColumn(i);
            if ((pressed || motion) && col >= start && col - start <
                menuTextWidth(menus[i].label) + 2) {
                menu->open = 1;
                menu->selected_menu = i;
                menu->selected_item = menuFirstItem(i);
                return CMD_NONE;
            }
        }
        return CMD_NONE;
    }
    if (!menu->open) return CMD_NONE;

    int32_t item = row - (menu_row + 2);
    int32_t width = menuPopupWidth(menu->selected_menu, screencols);
    if (!width) return CMD_NONE;
    int32_t start = menuPopupStart(menu->selected_menu, width, screencols);
    if (col > start && col <= start + width && item >= 0 &&
        item < menus[menu->selected_menu].item_count &&
        menus[menu->selected_menu].items[item] != CMD_NONE) {
        menu->selected_item = item;
        if (pressed || motion) return CMD_NONE;
        enum editorCommand command = menus[menu->selected_menu].items[item];
        menu->open = 0;
        return command;
    }
    if (pressed || motion) return CMD_NONE;
    menu->open = 0;
    return CMD_NONE;
}

/**
 * @brief Append the menu titles using the current UI colors.
 *
 * @details append copies bytes to the caller's output buffer through context.
 * Call at the bar's screen position; this helper also appends the line ending.
 */
void menuDrawBar(const struct editorMenu *menu,
    const struct editorSettings *settings, int32_t screencols,
    menuAppendFn append, void *context) {
    if (settingsColor(settings, color_background) != COLOR_TERMINAL_DEFAULT)
        append(context, "\x1b[49m", 5);
    const char *bg = ansiBgColorCode(settingsColor(settings, color_statusbar));
    const char *fg = ansiColorCode(settingsColor(settings, color_statusbar_text));
    append(context, bg[0] ? bg : "\x1b[49m", bg[0] ? (int32_t)strlen(bg) : 5);
    append(context, fg, (int32_t)strlen(fg));
    int32_t remaining = screencols;
    for (int32_t i = 0; i < menu_count && remaining > 0; i++) {
        if (menu->open && i == menu->selected_menu) append(context, "\x1b[7m", 4);
        remaining -= menuAppendText(append, context, " ", remaining);
        remaining -= menuAppendText(append, context, menus[i].label, remaining);
        remaining -= menuAppendText(append, context, " ", remaining);
        if (menu->open && i == menu->selected_menu) append(context, "\x1b[27m", 5);
    }
    append(context, "\x1b[K\x1b[m\r\n", 8);
    const char *editor_bg = ansiBgColorCode(settingsColor(settings, color_background));
    if (editor_bg[0]) append(context, editor_bg, (int32_t)strlen(editor_bg));
}

/**
 * @brief Append an open menu's border, items and setting checkmarks.
 *
 * @details menu_row is the one-based bar row. Uses absolute cursor positions
 * through append; does nothing when closed. The caller restores final frame
 * attributes.
 */
void menuDrawPopup(const struct editorMenu *menu,
    const struct editorSettings *settings, int32_t menu_row, int32_t screencols,
    menuAppendFn append, void *context) {
    if (!menu->open) return;
    int32_t width = menuPopupWidth(menu->selected_menu, screencols);
    if (!width) return;

    const char *bg = ansiBgColorCode(settingsColor(settings, color_statusbar));
    const char *fg = ansiColorCode(settingsColor(settings, color_statusbar_text));
    append(context, bg[0] ? bg : "\x1b[49m", bg[0] ? (int32_t)strlen(bg) : 5);
    append(context, fg, (int32_t)strlen(fg));
    const struct menuDefinition *definition = &menus[menu->selected_menu];
    int32_t start = menuPopupStart(menu->selected_menu, width, screencols);

    menuAppendRule(append, context, menu_row + 1, start, "┌", "┐", width);
    for (int32_t i = 0; i < definition->item_count; i++) {
        if (definition->items[i] == CMD_NONE) {
            menuAppendRule(append, context, menu_row + i + 2, start, "├", "┤", width);
        } else {
            const struct commandDescriptor *descriptor = commandGetDescriptor(definition->items[i]);
            const char *mark = commandIsSetting(descriptor->id)
                ? (commandIsChecked(descriptor->id, settings) ? "[x] " : "[ ] ") : "";
            menuAppendAt(append, context, menu_row + i + 2, start, "│");
            if (i == menu->selected_item) append(context, "\x1b[7m", 4);
            menuAppendItem(append, context, mark, descriptor, width);
            if (i == menu->selected_item) append(context, "\x1b[27m", 5);
            append(context, "│", 3);
            continue;
        }
        continue;
    }
    menuAppendRule(append, context, menu_row + definition->item_count + 2,
        start, "└", "┘", width);
    append(context, "\x1b[m", 3);
}
