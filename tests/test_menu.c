#include "menu.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the renderer to exercise private UTF-8 clipping with arbitrary labels. */
#include "../src/menu.c"

static char output[8192];
static size_t output_len;

static void collect(void *context, const char *text, int32_t len) {
    (void)context;
    if (len < 0 || (size_t)len >= sizeof(output) - output_len) exit(2);
    memcpy(output + output_len, text, (size_t)len);
    output_len += (size_t)len;
    output[output_len] = '\0';
}

static void check(uint8_t condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

int main(void) {
    struct commandDescriptor descriptor = {CMD_FIND, "Funzione 界 é", "⌘Z", NULL};
    menuAppendItem(collect, NULL, "", &descriptor, 20);
    check(utf8StrWidth(output, output_len) == 20, "UTF-8 item fills visual width");
    check(strstr(output, "⌘Z") != NULL, "UTF-8 shortcut preserved");
    output_len = 0;
    check(menuAppendText(collect, NULL, "é界", 2) == 1, "wide grapheme clipped whole");
    check(strcmp(output, "é") == 0, "combining grapheme preserved");
    char long_label[1024];
    memset(long_label, 'x', sizeof(long_label) - 1);
    long_label[sizeof(long_label) - 1] = '\0';
    descriptor.label = long_label;
    output_len = 0;
    menuAppendItem(collect, NULL, "[x] ", &descriptor, 600);
    check(utf8StrWidth(output, output_len) == 600, "label exceeds old fixed buffer");
    output_len = 0;
    menuAppendItem(collect, NULL, "[x] ", &descriptor, 1);
    check(utf8StrWidth(output, output_len) == 1, "narrow shortcut padding");
    struct editorMenu menu = {1, 2, 0};
    int32_t width = menuPopupWidth(menu.selected_menu, 12);
    int32_t start = menuPopupStart(menu.selected_menu, width, 12);
    check(start == 1 && width == 10, "popup fits narrow viewport");
    check(menuHandleMouse(&menu, 3, 2, 1, 12, 0, 0) == CMD_UNDO,
        "mouse uses shifted popup geometry");
    menu.open = 1;
    check(menuHandleMouse(&menu, 1, 13, 1, 12, 1, 0) == CMD_NONE,
        "mouse rejects offscreen title");
    struct editorSettings settings;
    settingsDefaults(&settings);
    check(!commandIsChecked(CMD_TOGGLE_CURSOR_BLINK, &settings), "cursor blink defaults off");
    check(commandToggleSetting(CMD_TOGGLE_CURSOR_BLINK, &settings), "toggle cursor blink");
    check(commandIsChecked(CMD_TOGGLE_CURSOR_BLINK, &settings), "cursor blink checkmark on");
    menu.open = 1; menu.selected_menu = 3; menu.selected_item = 6;
    check(menuHandleKey(&menu, '\r') == CMD_TOGGLE_CURSOR_BLINK, "View accepts cursor blink");
    puts("menu tests: ok");
    return 0;
}
