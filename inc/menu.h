/* menu.h -- keyboard and mouse state for the terminal menu overlay. */

#ifndef TE_MENU_H
#define TE_MENU_H

#include <stdint.h>

#include "command.h"

typedef void (*menuAppendFn)(void *context, const char *text, int32_t len);

struct editorMenu {
    uint8_t open;
    int32_t selected_menu;
    int32_t selected_item;
};

void menuInit(struct editorMenu *menu);
void menuOpen(struct editorMenu *menu);
enum editorCommand menuHandleKey(struct editorMenu *menu, int32_t key);
enum editorCommand menuHandleMouse(struct editorMenu *menu, int32_t row,
    int32_t col, int32_t menu_row, uint8_t pressed, uint8_t motion);
void menuDrawBar(const struct editorMenu *menu,
    const struct editorSettings *settings, menuAppendFn append, void *context);
void menuDrawPopup(const struct editorMenu *menu,
    const struct editorSettings *settings, int32_t menu_row,
    menuAppendFn append, void *context);

#endif
