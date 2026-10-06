/* command.h -- shared command identifiers and menu-facing metadata. */

#ifndef TE_COMMAND_H
#define TE_COMMAND_H

#include "settings.h"

#include <stdint.h>

enum editorCommand {
    CMD_NONE,
    CMD_INFO,
    CMD_SETTINGS,
    CMD_QUIT,
    CMD_OPEN,
    CMD_SAVE,
    CMD_SAVE_AS,
    CMD_CLOSE,
    CMD_UNDO,
    CMD_REDO,
    CMD_CUT,
    CMD_COPY,
    CMD_PASTE,
    CMD_SELECT_ALL,
    CMD_FIND,
    CMD_HELP,
    CMD_TOGGLE_LINE_NUMBERS,
    CMD_TOGGLE_TOP_BAR,
    CMD_TOGGLE_MENU,
    CMD_TOGGLE_INVISIBLES,
    CMD_TOGGLE_SYNTAX_HIGHLIGHT,
    CMD_TOGGLE_AUTO_INDENT
};

struct commandDescriptor {
    enum editorCommand id;
    const char *label;
    const char *shortcut;
    const char *setting_key;
};

/**
 * @brief Look up the label, shortcut and optional setting for a command.
 *
 * @return a borrowed immutable descriptor, or NULL for an unknown command; no
 * action is executed.
 */
const struct commandDescriptor *commandGetDescriptor(enum editorCommand command);
/**
 * @brief Check whether a command toggles a boolean setting.
 *
 * @return 1 for a descriptor with a setting key, otherwise 0, including
 * unknown commands.
 */
uint8_t commandIsSetting(enum editorCommand command);
/**
 * @brief Read the checkmark state of a setting command.
 *
 * @details settings supplies current or draft values.
 * @return the boolean state, or 0 for commands without a setting.
 */
uint8_t commandIsChecked(enum editorCommand command,
    const struct editorSettings *settings);
/**
 * @brief Toggle the boolean linked to a menu command.
 *
 * @details Mutates only the supplied settings copy.
 * @return the settings helper's success flag; the caller applies and persists
 * the change separately.
 */
uint8_t commandToggleSetting(enum editorCommand command,
    struct editorSettings *settings);

#endif
