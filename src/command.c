/* command.c -- command metadata shared by keyboard, menus, and settings. */

#include "command.h"

static const struct commandDescriptor command_descriptors[] = {
    { CMD_INFO, "Info", "F3", NULL },
    { CMD_SETTINGS, "Settings", "F2", NULL },
    { CMD_QUIT, "Quit", "^Q", NULL },
    { CMD_NEW, "New", "^N", NULL },
    { CMD_OPEN, "Open...", "^O", NULL },
    { CMD_SAVE, "Save", "^S", NULL },
    { CMD_SAVE_AS, "Save as...", "F4", NULL },
    { CMD_CLOSE, "Close", "^W", NULL },
    { CMD_UNDO, "Undo", "^Z", NULL },
    { CMD_REDO, "Redo", "^Y", NULL },
    { CMD_CUT, "Cut", "^X", NULL },
    { CMD_COPY, "Copy", "^C", NULL },
    { CMD_PASTE, "Paste", "^V", NULL },
    { CMD_SELECT_ALL, "Select all", "^A", NULL },
    { CMD_FIND, "Find...", "^F", NULL },
    { CMD_HELP, "Help", "F1", NULL },
    { CMD_TOGGLE_LINE_NUMBERS, "Line numbers", NULL, "show_line_numbers" },
    { CMD_TOGGLE_TOP_BAR, "Top bar", NULL, "show_top_bar" },
    { CMD_TOGGLE_MENU, "F10 menu", NULL, "show_menu" },
    { CMD_TOGGLE_INVISIBLES, "Show invisibles", NULL, "show_invisibles" },
    { CMD_TOGGLE_SYNTAX_HIGHLIGHT, "Syntax highlighting", NULL, "syntax_highlight" },
    { CMD_TOGGLE_TREE, "Show/hide file tree", "^E", NULL },
    { CMD_TOGGLE_AUTO_INDENT, "Auto-indent new lines", NULL, "auto_indent" }
};

/**
 * @brief Look up the label, shortcut and optional setting for a command.
 *
 * @return a borrowed immutable descriptor, or NULL for an unknown command; no
 * action is executed.
 */
const struct commandDescriptor *commandGetDescriptor(enum editorCommand command) {
    for (size_t i = 0; i < sizeof(command_descriptors) / sizeof(command_descriptors[0]); i++) {
        if (command_descriptors[i].id == command)
            return &command_descriptors[i];
    }
    return NULL;
}

/**
 * @brief Check whether a command toggles a boolean setting.
 *
 * @return 1 for a descriptor with a setting key, otherwise 0, including
 * unknown commands.
 */
uint8_t commandIsSetting(enum editorCommand command) {
    const struct commandDescriptor *descriptor = commandGetDescriptor(command);
    return descriptor && descriptor->setting_key;
}

/**
 * @brief Read the checkmark state of a setting command.
 *
 * @details settings supplies current or draft values.
 * @return the boolean state, or 0 for commands without a setting.
 */
uint8_t commandIsChecked(enum editorCommand command,
    const struct editorSettings *settings) {
    const struct commandDescriptor *descriptor = commandGetDescriptor(command);
    return descriptor && descriptor->setting_key
        ? settingsGetBool(settings, descriptor->setting_key) : 0;
}

/**
 * @brief Toggle the boolean linked to a menu command.
 *
 * @details Mutates only the supplied settings copy.
 * @return the settings helper's success flag; the caller applies and persists
 * the change separately.
 */
uint8_t commandToggleSetting(enum editorCommand command,
    struct editorSettings *settings) {
    const struct commandDescriptor *descriptor = commandGetDescriptor(command);
    return descriptor && descriptor->setting_key
        ? settingsToggleBool(settings, descriptor->setting_key) : 0;
}
