/* command.c -- command metadata shared by keyboard, menus, and settings. */

#include "command.h"

static const struct commandDescriptor command_descriptors[] = {
    { CMD_INFO, "Info", "F3", NULL },
    { CMD_SETTINGS, "Settings", "F2", NULL },
    { CMD_QUIT, "Quit", "^Q", NULL },
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
    { CMD_TOGGLE_INVISIBLES, "Show invisibles", NULL, "show_invisibles" }
};

const struct commandDescriptor *commandGetDescriptor(enum editorCommand command) {
    for (size_t i = 0; i < sizeof(command_descriptors) / sizeof(command_descriptors[0]); i++) {
        if (command_descriptors[i].id == command)
            return &command_descriptors[i];
    }
    return NULL;
}

uint8_t commandIsSetting(enum editorCommand command) {
    const struct commandDescriptor *descriptor = commandGetDescriptor(command);
    return descriptor && descriptor->setting_key;
}

uint8_t commandIsChecked(enum editorCommand command,
    const struct editorSettings *settings) {
    const struct commandDescriptor *descriptor = commandGetDescriptor(command);
    return descriptor && descriptor->setting_key
        ? settingsGetBool(settings, descriptor->setting_key) : 0;
}

uint8_t commandToggleSetting(enum editorCommand command,
    struct editorSettings *settings) {
    const struct commandDescriptor *descriptor = commandGetDescriptor(command);
    return descriptor && descriptor->setting_key
        ? settingsToggleBool(settings, descriptor->setting_key) : 0;
}
