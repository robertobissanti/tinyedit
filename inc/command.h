/* command.h -- shared command identifiers and menu-facing metadata. */

#ifndef TE_COMMAND_H
#define TE_COMMAND_H

#include <stdint.h>

#include "settings.h"

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
    CMD_TOGGLE_INVISIBLES
};

struct commandDescriptor {
    enum editorCommand id;
    const char *label;
    const char *shortcut;
    const char *setting_key;
};

const struct commandDescriptor *commandGetDescriptor(enum editorCommand command);
uint8_t commandIsSetting(enum editorCommand command);
uint8_t commandIsChecked(enum editorCommand command,
    const struct editorSettings *settings);
uint8_t commandToggleSetting(enum editorCommand command,
    struct editorSettings *settings);

#endif
