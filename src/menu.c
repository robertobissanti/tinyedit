/* menu.c -- rectangular menu overlay and its keyboard/mouse navigation. */

#include "menu.h"
#include "tinyedit.h"

#include <stdio.h>
#include <string.h>

struct menuDefinition {
    const char *label;
    const enum editorCommand *items;
    int32_t item_count;
};

static const enum editorCommand app_items[] = {
    CMD_INFO, CMD_SETTINGS, CMD_NONE, CMD_QUIT
};
static const enum editorCommand file_items[] = {
    CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_NONE, CMD_CLOSE
};
static const enum editorCommand edit_items[] = {
    CMD_UNDO, CMD_REDO, CMD_NONE, CMD_CUT, CMD_COPY, CMD_PASTE,
    CMD_NONE, CMD_SELECT_ALL, CMD_FIND
};
static const enum editorCommand view_items[] = {
    CMD_TOGGLE_LINE_NUMBERS, CMD_TOGGLE_TOP_BAR, CMD_TOGGLE_MENU,
    CMD_TOGGLE_INVISIBLES, CMD_TOGGLE_SYNTAX_HIGHLIGHT,
    CMD_TOGGLE_AUTO_INDENT
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

static int32_t menuFirstItem(int32_t menu_index) {
    for (int32_t i = 0; i < menus[menu_index].item_count; i++) {
        if (menus[menu_index].items[i] != CMD_NONE) return i;
    }
    return 0;
}

static int32_t menuNextItem(int32_t menu_index, int32_t item, int32_t delta) {
    int32_t count = menus[menu_index].item_count;
    do {
        item = (item + delta + count) % count;
    } while (menus[menu_index].items[item] == CMD_NONE);
    return item;
}

static int32_t menuStartColumn(int32_t menu_index) {
    int32_t col = 1;
    for (int32_t i = 0; i < menu_index; i++)
        col += (int32_t)strlen(menus[i].label) + 2;
    return col;
}

static int32_t menuWidth(const struct menuDefinition *definition) {
    int32_t width = 0;
    for (int32_t i = 0; i < definition->item_count; i++) {
        enum editorCommand command = definition->items[i];
        if (command == CMD_NONE) continue;
        const struct commandDescriptor *descriptor = commandGetDescriptor(command);
        int32_t item_width = (int32_t)strlen(descriptor->label) + 2;
        if (commandIsSetting(command)) item_width += 4;
        if (descriptor->shortcut) item_width += (int32_t)strlen(descriptor->shortcut) + 1;
        if (item_width > width) width = item_width;
    }
    return width;
}

static void menuAppendAt(menuAppendFn append, void *context, int32_t row,
    int32_t col, const char *text) {
    char position[32];
    int32_t len = snprintf(position, sizeof(position), "\x1b[%d;%dH", row, col);
    append(context, position, len);
    append(context, text, (int32_t)strlen(text));
}

static void menuAppendRule(menuAppendFn append, void *context, int32_t row,
    int32_t col, const char *left, const char *right, int32_t width) {
    menuAppendAt(append, context, row, col, left);
    for (int32_t i = 0; i < width; i++) append(context, "─", 3);
    append(context, right, 3);
}

void menuInit(struct editorMenu *menu) {
    menu->open = 0;
    menu->selected_menu = 0;
    menu->selected_item = menuFirstItem(0);
}

void menuOpen(struct editorMenu *menu) {
    menu->open = 1;
    menu->selected_menu = 0;
    menu->selected_item = menuFirstItem(0);
}

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

enum editorCommand menuHandleMouse(struct editorMenu *menu, int32_t row,
    int32_t col, int32_t menu_row, uint8_t pressed, uint8_t motion) {
    if (row == menu_row) {
        for (int32_t i = 0; i < menu_count; i++) {
            int32_t start = menuStartColumn(i);
            if ((pressed || motion) && col >= start && col < start +
                (int32_t)strlen(menus[i].label) + 2) {
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
    int32_t start = menuStartColumn(menu->selected_menu);
    int32_t width = menuWidth(&menus[menu->selected_menu]);
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

void menuDrawBar(const struct editorMenu *menu,
    const struct editorSettings *settings, menuAppendFn append, void *context) {
    if (settings->color_background != COLOR_TERMINAL_DEFAULT)
        append(context, "\x1b[49m", 5);
    const char *bg = ansiBgColorCode(settings->color_statusbar);
    const char *fg = ansiColorCode(settings->color_statusbar_text);
    if (bg[0]) append(context, bg, (int32_t)strlen(bg));
    append(context, fg, (int32_t)strlen(fg));
    for (int32_t i = 0; i < menu_count; i++) {
        if (menu->open && i == menu->selected_menu) append(context, "\x1b[7m", 4);
        append(context, " ", 1);
        append(context, menus[i].label, (int32_t)strlen(menus[i].label));
        append(context, " ", 1);
        if (menu->open && i == menu->selected_menu) append(context, "\x1b[27m", 5);
    }
    append(context, "\x1b[K\x1b[m\r\n", 8);
    const char *editor_bg = ansiBgColorCode(settings->color_background);
    if (editor_bg[0]) append(context, editor_bg, (int32_t)strlen(editor_bg));
}

void menuDrawPopup(const struct editorMenu *menu,
    const struct editorSettings *settings, int32_t menu_row,
    menuAppendFn append, void *context) {
    if (!menu->open) return;

    const char *bg = ansiBgColorCode(settings->color_statusbar);
    const char *fg = ansiColorCode(settings->color_statusbar_text);
    if (bg[0]) append(context, bg, (int32_t)strlen(bg));
    append(context, fg, (int32_t)strlen(fg));
    const struct menuDefinition *definition = &menus[menu->selected_menu];
    int32_t width = menuWidth(definition);
    int32_t start = menuStartColumn(menu->selected_menu);
    char line[256];
    char content[256];

    menuAppendRule(append, context, menu_row + 1, start, "┌", "┐", width);
    for (int32_t i = 0; i < definition->item_count; i++) {
        if (definition->items[i] == CMD_NONE) {
            menuAppendRule(append, context, menu_row + i + 2, start, "├", "┤", width);
        } else {
            const struct commandDescriptor *descriptor = commandGetDescriptor(definition->items[i]);
            const char *mark = commandIsSetting(descriptor->id)
                ? (commandIsChecked(descriptor->id, settings) ? "[x] " : "[ ] ") : "";
            int32_t used = snprintf(content, sizeof(content), "%s%s", mark, descriptor->label);
            int32_t shortcut_len = descriptor->shortcut ? (int32_t)strlen(descriptor->shortcut) : 0;
            while (used + shortcut_len < width) content[used++] = ' ';
            if (descriptor->shortcut) {
                memcpy(&content[used], descriptor->shortcut, (size_t)shortcut_len);
                used += shortcut_len;
            }
            content[used] = '\0';
            snprintf(line, sizeof(line), "%-*s", width, content);
            menuAppendAt(append, context, menu_row + i + 2, start, "│");
            if (i == menu->selected_item) append(context, "\x1b[7m", 4);
            append(context, line, width);
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
