/* settings.c -- see settings.h */

#define _DEFAULT_SOURCE

#include "settings.h"
#include "alloc.h"
#include "fileio.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const colorModeNames[] = { "ansi", "rgb", NULL };
static const char *const rgbOutputNames[] = { "truecolor", "ansi-fallback", NULL };
static const int32_t rgbPalette[] = {
    0x808080, 0x000000, 0x404040, 0x5555ff, 0x0000aa, 0x000055,
    0x55ff55, 0x00aa00, 0x005500, 0xffff55, 0xaaaa00, 0x555500,
    0x55ffff, 0x00aaaa, 0x005555, 0xff55ff, 0xaa00aa, 0x550055,
    0xff5555, 0xaa0000, 0x550000, 0xffffff, 0xaaaaaa, 0x555555
};
static char rgbForeground[8][32], rgbBackground[8][32];
static uint8_t rgbForegroundIndex, rgbBackgroundIndex;

static const char *const redoKeyNames[] = { "ctrl-y", "ctrl-shift-z", NULL };
static const char *const cursorStyleNames[] = { "block", "bar", NULL };
static const char *const lineEndingNames[] = { "auto", "lf", "crlf", NULL };
static const char *const legacyNames[] = {
    "gray", "blue", "green", "yellow", "cyan", "magenta", "red", "white", NULL
};
/* Order matches enum settingColor exactly -- each base hue's light,
 * dark, then dim variant. "gray" (with no suffix, from before the
 * light/dark/dim split) is kept as an alias handled separately in
 * settingColorFromLegacyName() so older ~/.tinyeditrc files still
 * load with the same visible color instead of silently falling back
 * to a default. "terminal-default" is COLOR_TERMINAL_DEFAULT, appended
 * after the 24 real hues (see settings.h) -- not one of the 8 base
 * hues' three variants, so it's listed on its own after them rather
 * than fitting the light/dark/dim pattern above. */
static const char *const colorNames[] = {
    "gray-light", "gray-dark", "gray-dim",
    "blue-light", "blue-dark", "blue-dim",
    "green-light", "green-dark", "green-dim",
    "yellow-light", "yellow-dark", "yellow-dim",
    "cyan-light", "cyan-dark", "cyan-dim",
    "magenta-light", "magenta-dark", "magenta-dim",
    "red-light", "red-dark", "red-dim",
    "white-light", "white-dark", "white-dim",
    "terminal-default",
    NULL
};

/* Built-in extension -> filetype name table, shown in the status bar.
 * Not exhaustive (see github/linguist for a much larger reference) --
 * just the languages/formats a typical user is likely to hit. Extended
 * or overridden per-user via "filetype.<ext> = <Name>" lines in
 * ~/.tinyeditrc (see filetypeOverrides below). */
static const struct filetypeEntry builtinFiletypes[] = {
    { "c", "C" }, { "h", "C" },
    { "cpp", "C++" }, { "cc", "C++" }, { "cxx", "C++" }, { "hpp", "C++" },
    { "py", "Python" },
    { "js", "JavaScript" }, { "jsx", "JavaScript" },
    { "ts", "TypeScript" }, { "tsx", "TypeScript" },
    { "go", "Go" },
    { "rs", "Rust" },
    { "java", "Java" },
    { "rb", "Ruby" },
    { "php", "PHP" },
    { "m", "Matlab/Octave" },
    { "sh", "Shell" }, { "bash", "Shell" }, { "zsh", "Shell" },
    { "md", "Markdown" }, { "markdown", "Markdown" },
    { "html", "HTML" }, { "htm", "HTML" },
    { "css", "CSS" },
    { "json", "JSON" },
    { "yml", "YAML" }, { "yaml", "YAML" },
    { "xml", "XML" },
    { "sql", "SQL" },
    { "lua", "Lua" },
    { "pl", "Perl" },
    { "swift", "Swift" },
    { "kt", "Kotlin" },
    { "toml", "TOML" },
    { "ini", "INI" },
};
static const int32_t builtinFiletypeCount =
    (int32_t)(sizeof(builtinFiletypes) / sizeof(builtinFiletypes[0]));

/* User-defined overrides/additions from ~/.tinyeditrc's "filetype.*"
 * keys, loaded once by settingsLoad() and preserved verbatim by
 * settingsSave() (which doesn't otherwise know about them -- the F2
 * screen doesn't edit this table yet). Owned by this module: freed
 * only by being reset at the next settingsLoad() call, per-process
 * this is a small, one-time allocation. */
static struct filetypeEntry *filetypeOverrides = NULL;
static int32_t filetypeOverrideCount = 0;

/* Descriptor table: the single source of truth for every setting's key,
 * label, type, storage location, and valid range/values. Both the
 * ~/.tinyeditrc parser and the F2 settings screen walk this table
 * instead of hardcoding each option. */
const struct settingDescriptor settingDescriptors[] = {
    { "color_mode", "Mode", SETTING_ENUM, offsetof(struct editorSettings, color_mode), 0, 0, colorModeNames, 2 },
    { "rgb_output", "RGB output", SETTING_ENUM, offsetof(struct editorSettings, rgb_output), 0, 0, rgbOutputNames, 2 },
    { "rgb_gutter", "Gutter color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_gutter), 0, 0, NULL, 0 },
    { "rgb_selection", "Selection color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_selection), 0, 0, NULL, 0 },
    { "rgb_statusbar", "Status bar color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_statusbar), 0, 0, NULL, 0 },
    { "rgb_statusbar_text", "Status bar text color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_statusbar_text), 0, 0, NULL, 0 },
    { "rgb_invisibles", "Invisibles color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_invisibles), 0, 0, NULL, 0 },
    { "rgb_syntax_normal", "Syntax: normal text color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_normal), 0, 0, NULL, 0 },
    { "rgb_syntax_keyword", "Syntax: keyword color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_keyword), 0, 0, NULL, 0 },
    { "rgb_syntax_string", "Syntax: string color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_string), 0, 0, NULL, 0 },
    { "rgb_syntax_json_key", "Syntax: JSON key color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_json_key), 0, 0, NULL, 0 },
    { "rgb_syntax_bracket", "Syntax: bracket color (all files)", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_bracket), 0, 0, NULL, 0 },
    { "rgb_syntax_comment", "Syntax: comment color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_comment), 0, 0, NULL, 0 },
    { "rgb_syntax_number", "Syntax: number color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_number), 0, 0, NULL, 0 },
    { "rgb_syntax_preprocessor", "Syntax: preprocessor color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_preprocessor), 0, 0, NULL, 0 },
    { "rgb_syntax_function", "Syntax: function name color (C/C++)", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_function), 0, 0, NULL, 0 },
    { "rgb_syntax_emphasis_strong", "Markdown: bold text color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_emphasis_strong), 0, 0, NULL, 0 },
    { "rgb_syntax_italic", "Markdown: italic text color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_italic), 0, 0, NULL, 0 },
    { "rgb_syntax_math", "Markdown: LaTeX math color", SETTING_RGB, offsetof(struct editorSettings, rgb_color_syntax_math), 0, 0, NULL, 0 },
    { "rgb_markdown_heading_background", "Markdown: heading background color", SETTING_RGB,
      offsetof(struct editorSettings, rgb_markdown_heading_background), 0, 0, NULL, 0 },
    { "rgb_background", "Editor background color (off=terminal default)", SETTING_RGB, offsetof(struct editorSettings, rgb_color_background), 0, 0, NULL, 0 },

    { "show_line_numbers", "Show line numbers", SETTING_BOOL,
      offsetof(struct editorSettings, show_line_numbers), 0, 0, NULL, 0 },
    { "tab_stop", "Tab width", SETTING_INT,
      offsetof(struct editorSettings, tab_stop), 1, 16, NULL, 0 },
    { "mouse_enabled", "Mouse support (disables native terminal selection)", SETTING_BOOL,
      offsetof(struct editorSettings, mouse_enabled), 0, 0, NULL, 0 },
#ifdef __APPLE__
    { "mac_command_keys", "macOS Command keys (Ghostty, experimental)", SETTING_BOOL,
      offsetof(struct editorSettings, mac_command_keys), 0, 0, NULL, 0 },
#endif
    { "cursor_blink", "Cursor blinking", SETTING_BOOL,
      offsetof(struct editorSettings, cursor_blink), 0, 0, NULL, 0 },
    { "cursor_style", "Cursor shape", SETTING_ENUM,
      offsetof(struct editorSettings, cursor_style), 0, 0, cursorStyleNames, 2 },
    { "line_ending", "Line endings on save", SETTING_ENUM,
      offsetof(struct editorSettings, line_ending), 0, 0, lineEndingNames, 3 },
    { "redo_key", "Redo key", SETTING_ENUM,
      offsetof(struct editorSettings, redo_key), 0, 0, redoKeyNames, 2 },
    { "undo_memory_mb", "Undo memory MiB (next document/session)", SETTING_INT,
      offsetof(struct editorSettings, undo_memory_mb), 1, 4096, NULL, 0 },
    { "undo_max_depth", "Undo history depth", SETTING_INT,
      offsetof(struct editorSettings, undo_max_depth), 10, 2000, NULL, 0 },
    { "color_gutter", "Gutter color", SETTING_ENUM,
      offsetof(struct editorSettings, color_gutter), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_selection", "Selection color", SETTING_ENUM,
      offsetof(struct editorSettings, color_selection), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_statusbar", "Status bar color", SETTING_ENUM,
      offsetof(struct editorSettings, color_statusbar), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_statusbar_text", "Status bar text color", SETTING_ENUM,
      offsetof(struct editorSettings, color_statusbar_text), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "show_top_bar", "Show top bar", SETTING_BOOL,
      offsetof(struct editorSettings, show_top_bar), 0, 0, NULL, 0 },
    { "show_menu", "Enable F10 menu", SETTING_BOOL,
      offsetof(struct editorSettings, show_menu), 0, 0, NULL, 0 },
    { "soft_wrap", "Max wrap width (0=window edge)", SETTING_INT,
      offsetof(struct editorSettings, soft_wrap), 0, 500, NULL, 0 },
    { "scrolloff", "Cursor margin (rows)", SETTING_INT,
      offsetof(struct editorSettings, scrolloff), 0, 20, NULL, 0 },
    { "home_end_visual_line", "Home/End use visual line", SETTING_BOOL,
      offsetof(struct editorSettings, home_end_visual_line), 0, 0, NULL, 0 },
    { "backup_interval", "Backup interval s (0=off, min 5)", SETTING_INT,
      offsetof(struct editorSettings, backup_interval), 0, 3600, NULL, 0 },
    { "auto_indent", "Auto-indent new lines", SETTING_BOOL,
      offsetof(struct editorSettings, auto_indent), 0, 0, NULL, 0 },
    { "auto_close_pairs", "Auto-close brackets/quotes/tags", SETTING_BOOL,
      offsetof(struct editorSettings, auto_close_pairs), 0, 0, NULL, 0 },
    { "auto_close_single_quote", "Auto-close single quote '", SETTING_BOOL,
      offsetof(struct editorSettings, auto_close_single_quote), 0, 0, NULL, 0 },
    { "insert_spaces_for_tab", "Tab key inserts spaces", SETTING_BOOL,
      offsetof(struct editorSettings, insert_spaces_for_tab), 0, 0, NULL, 0 },
    { "show_invisibles", "Show invisible characters", SETTING_BOOL,
      offsetof(struct editorSettings, show_invisibles), 0, 0, NULL, 0 },
    { "color_invisibles", "Invisibles color", SETTING_ENUM,
      offsetof(struct editorSettings, color_invisibles), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "syntax_highlight", "Syntax highlighting", SETTING_BOOL,
      offsetof(struct editorSettings, syntax_highlight), 0, 0, NULL, 0 },


    { "color_syntax_normal", "Syntax: normal text color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_normal), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_keyword", "Syntax: keyword color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_keyword), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_string", "Syntax: string color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_string), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_json_key", "Syntax: JSON key color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_json_key), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_bracket", "Syntax: bracket color (all files)", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_bracket), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_comment", "Syntax: comment color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_comment), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_number", "Syntax: number color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_number), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_preprocessor", "Syntax: preprocessor color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_preprocessor), 0, 0, colorNames, SETTING_COLOR_COUNT },


    { "color_syntax_function", "Syntax: function name color (C/C++)", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_function), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_background", "Editor background color (off=terminal default)", SETTING_ENUM,
      offsetof(struct editorSettings, color_background), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "markdown_text_styles", "Markdown bold and italic", SETTING_BOOL,
      offsetof(struct editorSettings, markdown_text_styles), 0, 0, NULL, 0 },
    { "markdown_heading_reverse", "Markdown: reverse heading colors", SETTING_BOOL,
      offsetof(struct editorSettings, markdown_heading_reverse), 0, 0, NULL, 0 },
    { "color_syntax_emphasis_strong", "Markdown: bold text color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_emphasis_strong), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_italic", "Markdown: italic text color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_italic), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_math", "Markdown: LaTeX math color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_math), 0, 0, colorNames, SETTING_COLOR_COUNT },
};
const int32_t settingDescriptorCount = (int32_t)(sizeof(settingDescriptors) / sizeof(settingDescriptors[0]));

/**
 * @brief Access a writable setting through its table descriptor.
 *
 * @details s must be an editorSettings object and d one of its descriptors.
 * @return a borrowed int32_t slot; every settings field must have that size.
 */
static int32_t *settingSlot(struct editorSettings *s, const struct settingDescriptor *d) {
    return (int32_t *)((char *)s + d->offset);
}

/**
 * @brief Access a setting through its table descriptor without modifying it.
 *
 * @details s and d must correspond to the same settings layout.
 * @return a borrowed pointer into s.
 */
static const int32_t *settingSlotConst(const struct editorSettings *s, const struct settingDescriptor *d) {
    return (const int32_t *)((const char *)s + d->offset);
}

/**
 * @brief Look up an option by its configuration key.
 *
 * @details key is NUL-terminated and case-sensitive.
 * @return a borrowed descriptor or NULL when unknown.
 */
const struct settingDescriptor *settingsFind(const char *key) {
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        if (strcmp(settingDescriptors[i].key, key) == 0)
            return &settingDescriptors[i];
    }
    return NULL;
}

/**
 * @brief Read a named boolean option from a settings object.
 *
 * @return its truth value, or 0 when the key is missing or not a boolean
 * setting.
 */
uint8_t settingsGetBool(const struct editorSettings *settings, const char *key) {
    const struct settingDescriptor *descriptor = settingsFind(key);
    return descriptor && descriptor->type == SETTING_BOOL &&
        *settingSlotConst(settings, descriptor) != 0;
}

/**
 * @brief Toggle a named boolean option in a settings object.
 *
 * @details Does not apply terminal changes or save configuration.
 * @return 1 when toggled, 0 for a missing or non-boolean key.
 */
uint8_t settingsToggleBool(struct editorSettings *settings, const char *key) {
    const struct settingDescriptor *descriptor = settingsFind(key);
    if (!descriptor || descriptor->type != SETTING_BOOL) return 0;
    *settingSlot(settings, descriptor) = !*settingSlot(settings, descriptor);
    return 1;
}

/**
 * @brief Fill every setting field with its built-in default.
 *
 * @details out must point to writable settings storage; no file is read or
 * written and filetype overrides are untouched.
 */
void settingsDefaults(struct editorSettings *out) {
    out->color_mode = COLOR_MODE_ANSI;
    out->rgb_output = RGB_OUTPUT_TRUECOLOR;
    out->show_line_numbers = 1;
    out->tab_stop = 4;
    out->redo_key = REDO_KEY_CTRL_Y;
    out->undo_max_depth = 200;
    out->undo_memory_mb = 64;
    out->color_gutter = COLOR_GRAY_LIGHT;
    out->color_selection = COLOR_WHITE_DARK;
    out->color_statusbar = COLOR_WHITE_DARK;
    out->color_statusbar_text = COLOR_WHITE_LIGHT;
    out->show_top_bar = 0;
    out->show_menu = 1;
    out->soft_wrap = 0;
    out->scrolloff = 0;
    out->home_end_visual_line = 1;
    out->backup_interval = 0;
    out->auto_indent = 1;
    out->auto_close_pairs = 1;
    out->auto_close_single_quote = 0;
    out->insert_spaces_for_tab = 1;
    out->show_invisibles = 0;
    out->color_invisibles = COLOR_GRAY_LIGHT;
    out->syntax_highlight = 1;
    out->markdown_heading_reverse = 0;
    out->rgb_markdown_heading_background = RGB_TERMINAL_DEFAULT;
    out->markdown_text_styles = 0;
    out->color_syntax_normal = COLOR_TERMINAL_DEFAULT;
    out->color_syntax_keyword = COLOR_BLUE_LIGHT;
    out->color_syntax_string = COLOR_GREEN_LIGHT;
    out->color_syntax_json_key = COLOR_CYAN_LIGHT;
    out->color_syntax_bracket = COLOR_YELLOW_LIGHT;
    out->color_syntax_comment = COLOR_GRAY_DIM;
    out->color_syntax_number = COLOR_MAGENTA_LIGHT;
    out->color_syntax_preprocessor = COLOR_YELLOW_LIGHT;
    out->color_syntax_emphasis_strong = COLOR_RED_LIGHT;
    out->color_syntax_italic = out->color_syntax_keyword;
    out->color_syntax_math = COLOR_CYAN_LIGHT;
    out->color_syntax_function = COLOR_YELLOW_LIGHT;
    out->color_background = COLOR_TERMINAL_DEFAULT;
    out->rgb_color_gutter = out->color_gutter == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_gutter];
    out->rgb_color_selection = out->color_selection == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_selection];
    out->rgb_color_statusbar = out->color_statusbar == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_statusbar];
    out->rgb_color_statusbar_text = out->color_statusbar_text == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_statusbar_text];
    out->rgb_color_invisibles = out->color_invisibles == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_invisibles];
    out->rgb_color_syntax_normal = out->color_syntax_normal == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_normal];
    out->rgb_color_syntax_keyword = out->color_syntax_keyword == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_keyword];
    out->rgb_color_syntax_string = out->color_syntax_string == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_string];
    out->rgb_color_syntax_json_key = out->color_syntax_json_key == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_json_key];
    out->rgb_color_syntax_bracket = out->color_syntax_bracket == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_bracket];
    out->rgb_color_syntax_comment = out->color_syntax_comment == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_comment];
    out->rgb_color_syntax_number = out->color_syntax_number == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_number];
    out->rgb_color_syntax_preprocessor = out->color_syntax_preprocessor == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_preprocessor];
    out->rgb_color_syntax_emphasis_strong = out->color_syntax_emphasis_strong == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_emphasis_strong];
    out->rgb_color_syntax_italic = out->rgb_color_syntax_keyword;
    out->rgb_color_syntax_math = out->color_syntax_math == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_math];
    out->rgb_color_syntax_function = out->color_syntax_function == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_syntax_function];
    out->rgb_color_background = out->color_background == COLOR_TERMINAL_DEFAULT ? RGB_TERMINAL_DEFAULT : rgbPalette[out->color_background];
    out->mouse_enabled = 0;
#ifdef __APPLE__
    out->mac_command_keys = 0;
#endif
    out->cursor_style = CURSOR_BLOCK;
    out->cursor_blink = 0;
    out->line_ending = LINE_ENDING_AUTO;
}

/**
 * @brief Build the configuration path under the user's HOME.
 *
 * @details buf has buflen bytes.
 * @return buf with NUL-terminated text, or NULL if HOME is unavailable or the
 * path does not fit.
 */
static const char *configPath(char *buf, size_t buflen) {
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    int32_t n = snprintf(buf, buflen, "%s/.tinyeditrc", home);
    if (n < 0 || (size_t)n >= buflen) return NULL;
    return buf;
}

/**
 * @brief Find the stored index for an enum's readable value.
 *
 * @details names is a NULL-terminated string table and name is NUL-terminated.
 * @return the matching index, or -1 when absent.
 */
static int32_t enumIndexOf(const char *const *names, const char *name) {
    for (int32_t i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

/**
 * @brief Translate older color spellings to the current palette.
 *
 * @details name is a NUL-terminated legacy value.
 * @return the palette index, or -1 if it is unknown.
 *
 * @note Maps a pre-split color name ("gray", "cyan", ...) written by an older
 * tinyedit version to its closest equivalent in the current 3-variants-per-hue
 * palette, or -1 if `name` isn't one of the 8 legacy names. Without this, an
 * older ~/.tinyeditrc would silently lose its color customization on load
 * (enumIndexOf() finding no exact match in the new "hue-light"/"hue-
 * dark"/"hue-dim" names, leaving the field at settingsDefaults()'s value
 * instead) -- exactly the migration gap the change in enum settingColor's
 * field comment warns about. Each legacy name maps to its "light" variant:
 * that's what the old single-variant palette actually rendered as (see
 * ansiColorCode() -- e.g. old COLOR_GRAY was already \x1b[90m, the
 * bright/light code, not \x1b[30m). Works unchanged across both the original
 * 2-variant (light/dark) and current 3-variant (light/dark/dim) palette since
 * "light" is always index 0 of each hue's block -- only the block STRIDE
 * changed (2 -> 3), captured here as HUE_COLOR_COUNT (the 24 real hues, i.e.
 * SETTING_COLOR_COUNT minus COLOR_TERMINAL_DEFAULT -- see settings.h) divided
 * by the legacy name count rather than hardcoded, so a future variant addition
 * doesn't need this function touched again.
 */
static int32_t settingColorFromLegacyName(const char *name) {
    int32_t variants_per_hue = SETTING_HUE_COLOR_COUNT / 8;
    for (int32_t i = 0; legacyNames[i]; i++)
        if (strcmp(legacyNames[i], name) == 0) return i * variants_per_hue; /* *_LIGHT is first in each block */
    return -1;
}

/**
 * @brief Remove leading and trailing whitespace from a configuration token.
 *
 * @details s is a writable NUL-terminated string. Trims in place without
 * allocating a replacement.
 */
static void trim(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

/**
 * @brief Add or replace the readable label for a filename extension.
 *
 * @details ext and name are NUL-terminated and copied into module-owned
 * storage. Repeated extensions replace their existing label.
 */
static void addFiletypeOverride(const char *ext, const char *name) {
    /* Same extension re-declared later in the file wins (matches how
     * the descriptor-based settings above already let the last
     * occurrence of a key win, since the loop just keeps overwriting
     * the same slot). */
    for (int32_t i = 0; i < filetypeOverrideCount; i++) {
        if (strcmp(filetypeOverrides[i].ext, ext) == 0) {
            free((void *)filetypeOverrides[i].name);
            filetypeOverrides[i].name = teStrdup(name);
            return;
        }
    }
    if (filetypeOverrideCount == INT32_MAX) {
        errno = ENOMEM; perror("tinyedit: filetype count"); exit(EXIT_FAILURE);
    }
    filetypeOverrides = teRealloc(filetypeOverrides,
        teArrayBytes((size_t)filetypeOverrideCount + 1, sizeof(*filetypeOverrides)));
    filetypeOverrides[filetypeOverrideCount].ext = teStrdup(ext);
    filetypeOverrides[filetypeOverrideCount].name = teStrdup(name);
    filetypeOverrideCount++;
}

/**
 * @brief Register a nonempty extension and its readable label.
 *
 * @details Copies both strings into the override table; invalid empty or NULL
 * inputs are ignored. Persistence is a separate settingsSave() call.
 */
void settingsSetFiletype(const char *ext, const char *name) {
    if (!ext || !*ext || !name || !*name) return;
    addFiletypeOverride(ext, name);
}

/**
 * @brief Release every module-owned extension and label override.
 *
 * @details Resets the override table and count; used before loading a fresh
 * configuration.
 */
static void freeFiletypeOverrides(void) {
    for (int32_t i = 0; i < filetypeOverrideCount; i++) {
        free((void *)filetypeOverrides[i].ext);
        free((void *)filetypeOverrides[i].name);
    }
    free(filetypeOverrides);
    filetypeOverrides = NULL;
    filetypeOverrideCount = 0;
}

/**
 * @brief Load configuration over defaults and rebuild filetype overrides.
 *
 * @details out receives valid settings even when the file is missing or
 * malformed. Missing configuration is created when possible; invalid entries
 * are ignored and integers are clamped.
 */
void settingsLoad(struct editorSettings *out) {
    settingsDefaults(out);
    freeFiletypeOverrides();

    char path[1024];
    if (!configPath(path, sizeof(path))) return;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* No config file yet: defaults stand in memory for this run,
         * and are also written to disk right away so ~/.tinyeditrc
         * exists (and is inspectable/editable by hand) from the very
         * first launch, instead of only appearing after the user
         * happens to open F2 and press Ctrl-S. Failure here (e.g.
         * read-only $HOME) is not fatal -- the in-memory defaults
         * from settingsDefaults() above still work for this session,
         * same as before this existed. */
        settingsSave(out);
        return;
    }

    uint8_t has_italic = 0, has_rgb_italic = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *key_start = line;
        while (isspace((unsigned char)*key_start)) key_start++;
        char *hash = strchr(line, '#');
        char *assignment = strchr(line, '=');
        if (hash && assignment && hash > assignment) {
            char *value_start = assignment + 1;
            while (isspace((unsigned char)*value_start)) value_start++;
            if (value_start == hash && strncmp(key_start, "rgb_", 4) == 0 &&
                strncmp(key_start, "rgb_output", 10) != 0) hash = strchr(hash + 1, '#');
        }
        /* A filetype label may contain '#' ("C#", "F#"): only " #" starts a
         * comment there, otherwise saving would silently truncate the label. */
        if (hash && assignment && hash > assignment &&
            strncmp(key_start, FILETYPE_KEY_PREFIX, strlen(FILETYPE_KEY_PREFIX)) == 0) {
            while (hash && !isspace((unsigned char)hash[-1])) hash = strchr(hash + 1, '#');
        }
        if (hash) *hash = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);
        if (key[0] == '\0') continue;

        if (strncmp(key, FILETYPE_KEY_PREFIX, strlen(FILETYPE_KEY_PREFIX)) == 0) {
            const char *ext = key + strlen(FILETYPE_KEY_PREFIX);
            if (ext[0] != '\0' && value[0] != '\0')
                addFiletypeOverride(ext, value);
            continue;
        }

        for (int32_t i = 0; i < settingDescriptorCount; i++) {
            const struct settingDescriptor *d = &settingDescriptors[i];
            if (strcmp(d->key, key) != 0) continue;

            if (strcmp(key, "color_syntax_italic") == 0) has_italic = 1;
            if (strcmp(key, "rgb_syntax_italic") == 0) has_rgb_italic = 1;
            int32_t *slot = settingSlot(out, d);
            if (d->type == SETTING_RGB) {
                settingsParseRgb(value, slot);
            } else if (d->type == SETTING_BOOL) {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
                    *slot = 1;
                else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
                    *slot = 0;
            } else if (d->type == SETTING_INT) {
                char *end = NULL;
                errno = 0;
                long parsed = strtol(value, &end, 10);
                if (errno == 0 && end != value && *end == '\0') {
                    if (parsed < d->int_min) parsed = d->int_min;
                    if (parsed > d->int_max) parsed = d->int_max;
                    *slot = (int32_t)parsed;
                }
            } else { /* SETTING_ENUM */
                int32_t idx = enumIndexOf(d->enum_names, value);
                if (idx < 0 && d->enum_names == colorNames)
                    idx = settingColorFromLegacyName(value); /* pre-light/dark ~/.tinyeditrc */
                if (idx >= 0) *slot = idx;
            }
            break;
        }
    }
    fclose(fp);
    if (!has_italic) out->color_syntax_italic = out->color_syntax_keyword;
    if (!has_rgb_italic) out->rgb_color_syntax_italic = out->rgb_color_syntax_keyword;
}

/** @brief Load a complete color preset atomically, preserving non-color settings. */
uint8_t settingsLoadColorScheme(const char *path, struct editorSettings *draft,
    char *error, size_t error_size) {
    FILE *fp = fopen(path, "r");
    if (!fp) { snprintf(error, error_size, "Cannot open color scheme"); return 0; }
    struct editorSettings candidate = *draft;
    uint8_t *seen = teMalloc((size_t)settingDescriptorCount);
    memset(seen, 0, (size_t)settingDescriptorCount);
    uint8_t valid = 1, has_mode = 0;
    char line[512];
    while (valid && fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) { valid = 0; break; }
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { valid = 0; break; }
        *eq = '\0';
        char *value = eq + 1;
        trim(line); trim(value);
        const struct settingDescriptor *d = settingsFind(line);
        if (!d) { valid = 0; break; }
        uint8_t mode = strcmp(line, "color_mode") == 0;
        uint8_t output = strcmp(line, "rgb_output") == 0;
        uint8_t color = d->type == SETTING_RGB ||
            (strncmp(line, "color_", 6) == 0 && !mode) ||
            strcmp(line, "markdown_heading_reverse") == 0;
        if (!mode && !output && !color) { valid = 0; break; }
        char *comment = strchr(value + (value[0] == '#' ? 1 : 0), '#');
        if (comment) *comment = '\0';
        trim(value);
        int32_t parsed = 0;
        if (d->type == SETTING_RGB) valid = settingsParseRgb(value, &parsed);
        else if (d->type == SETTING_BOOL) {
            if (!strcmp(value, "true") || !strcmp(value, "1")) parsed = 1;
            else if (strcmp(value, "false") && strcmp(value, "0")) valid = 0;
        } else {
            parsed = enumIndexOf(d->enum_names, value);
            if (parsed < 0 && d->enum_names == colorNames) parsed = settingColorFromLegacyName(value);
            if (parsed < 0) valid = 0;
        }
        for (int32_t i = 0; i < settingDescriptorCount; i++) {
            if (&settingDescriptors[i] != d) continue;
            if (seen[i]) valid = 0;
            seen[i] = 1;
        }
        if (valid && !output) *settingSlot(&candidate, d) = parsed;
        if (mode) has_mode = 1;
    }
    if (ferror(fp)) valid = 0;
    if (fclose(fp)) valid = 0;
    if (!has_mode) valid = 0;
    for (int32_t i = 0; valid && i < settingDescriptorCount; i++) {
        const struct settingDescriptor *d = &settingDescriptors[i];
        uint8_t required = candidate.color_mode == COLOR_MODE_RGB ? d->type == SETTING_RGB :
            (d->type == SETTING_ENUM && d->enum_names == colorNames) ||
            strcmp(d->key, "markdown_heading_reverse") == 0;
        if (required && !seen[i] && strcmp(d->key, "rgb_syntax_italic") == 0) {
            candidate.rgb_color_syntax_italic = candidate.rgb_color_syntax_keyword;
        } else if (required && !seen[i] && strcmp(d->key, "color_syntax_italic") == 0) {
            candidate.color_syntax_italic = candidate.color_syntax_keyword;
        } else if (required && !seen[i]) valid = 0;
    }
    free(seen);
    if (!valid) { snprintf(error, error_size, "Invalid or incomplete color scheme"); return 0; }
    *draft = candidate;
    if (error_size) error[0] = '\0';
    return 1;
}

/**
 * @brief Persist settings and known filetype overrides through a temporary file.
 *
 * @details s must contain valid descriptor values, including enum indices. A
 * symlinked ~/.tinyeditrc is followed, so the link itself is preserved.
 * @return 1 after file and directory sync, otherwise 0 with the temporary
 * file removed. A post-rename sync failure may have replaced the config.
 */
uint8_t settingsSave(const struct editorSettings *s) {
    char path[1024];
    if (!configPath(path, sizeof(path))) return 0;

    /* A dotfile manager may keep ~/.tinyeditrc as a symlink: replace the real
     * file it points to, not the link. realpath() allocates internally, so its
     * failure is recoverable here; ENOENT just means the file is not there yet. */
    char *resolved = realpath(path, NULL);
    if (resolved) {
        int32_t resolved_len = snprintf(path, sizeof(path), "%s", resolved);
        free(resolved);
        if (resolved_len < 0 || (size_t)resolved_len >= sizeof(path)) { errno = ENAMETOOLONG; return 0; }
    } else if (errno != ENOENT) {
        return 0;
    }

    char tmppath[1040];
    int32_t pathlen = snprintf(tmppath, sizeof(tmppath), "%s.tmp.XXXXXX", path);
    if (pathlen < 0 || (size_t)pathlen >= sizeof(tmppath)) return 0;
    int fd = mkstemp(tmppath);
    if (fd == -1) return 0;
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmppath);
        return 0;
    }

    fprintf(fp, "# tinyedit configuration -- edit with F2 inside the editor,\n");
    fprintf(fp, "# or by hand (key = value; RGB accepts #RRGGBB or terminal-default).\n\n");

    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        const struct settingDescriptor *d = &settingDescriptors[i];
        const int32_t *slot = settingSlotConst(s, d);

        if (d->type == SETTING_RGB) {
            char value[17];
            settingsFormatRgb(*slot, value, sizeof(value));
            fprintf(fp, "%s = %s\n", d->key, value);
        } else if (d->type == SETTING_BOOL) {
            fprintf(fp, "%s = %s\n", d->key, *slot ? "true" : "false");
        } else if (d->type == SETTING_INT) {
            fprintf(fp, "%s = %" PRId32 "\n", d->key, *slot);
        } else {
            fprintf(fp, "%s = %s\n", d->key, d->enum_names[*slot]);
        }
    }

    /* Preserve filetype.* overrides even though the F2 screen doesn't
     * edit them yet -- otherwise saving settings from F2 would silently
     * wipe out anything the user added by hand. */
    if (filetypeOverrideCount > 0) {
        fprintf(fp, "\n# filetype overrides (status bar language name)\n");
        for (int32_t i = 0; i < filetypeOverrideCount; i++)
            fprintf(fp, "%s%s = %s\n", FILETYPE_KEY_PREFIX,
                filetypeOverrides[i].ext, filetypeOverrides[i].name);
    }

    uint8_t ok = !ferror(fp) && fflush(fp) == 0 && fsync(fd) == 0;
    int close_errno = errno;
    if (fclose(fp) != 0 && ok) {
        ok = 0;
        close_errno = errno;
    }
    errno = close_errno;
    if (ok && fileioReplace(tmppath, path) != FILE_SAVE_DURABLE) ok = 0;
    if (!ok) {
        int saved_errno = errno;
        unlink(tmppath);
        errno = saved_errno;
    }
    return ok;
}

/**
 * @brief Look up a readable filetype label, preferring user overrides.
 *
 * @details ext is case-sensitive without a leading dot.
 * @return a borrowed string or NULL when unknown; its lifetime depends on the
 * override table.
 */
const char *filetypeForExtension(const char *ext) {
    if (!ext || ext[0] == '\0') return NULL;

    for (int32_t i = 0; i < filetypeOverrideCount; i++)
        if (strcmp(filetypeOverrides[i].ext, ext) == 0)
            return filetypeOverrides[i].name;

    for (int32_t i = 0; i < builtinFiletypeCount; i++)
        if (strcmp(builtinFiletypes[i].ext, ext) == 0)
            return builtinFiletypes[i].name;

    return NULL;
}

/** @brief Parse only complete six-digit RGB values or the terminal default sentinel. */
uint8_t settingsParseRgb(const char *text, int32_t *out) {
    if (strcmp(text, "terminal-default") == 0) { *out = RGB_TERMINAL_DEFAULT; return 1; }
    if (strlen(text) != 7 || text[0] != '#') return 0;
    int32_t value = 0;
    for (size_t i = 1; i < 7; i++) {
        uint8_t c = (uint8_t)text[i];
        int32_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        value = value * 16 + digit;
    }
    *out = value;
    return 1;
}

/** @brief Write the canonical uppercase RGB spelling or terminal-default. */
void settingsFormatRgb(int32_t value, char *out, size_t size) {
    if (value == RGB_TERMINAL_DEFAULT) snprintf(out, size, "terminal-default");
    else snprintf(out, size, "#%06" PRIX32, (uint32_t)value);
}

/** @brief Select true color or the closest of sixteen fixed ANSI reference colors. */
int32_t settingsResolveColor(const struct editorSettings *s, int32_t ansi, int32_t rgb) {
    if (s->color_mode != COLOR_MODE_RGB) return ansi;
    if (rgb < 0 || rgb > 0xffffff) return COLOR_TERMINAL_DEFAULT;
    if (s->rgb_output == RGB_OUTPUT_TRUECOLOR) return RGB_COLOR_BASE + rgb;
    int32_t best = 0, distance = INT32_MAX;
    for (int32_t i = 0; i < SETTING_HUE_COLOR_COUNT; i++) {
        if (settingColorIsDim(i)) continue;
        int32_t r = (rgb >> 16) - (rgbPalette[i] >> 16);
        int32_t g = ((rgb >> 8) & 255) - ((rgbPalette[i] >> 8) & 255);
        int32_t b = (rgb & 255) - (rgbPalette[i] & 255);
        int32_t candidate = r*r + g*g + b*b;
        if (candidate < distance) { best = i; distance = candidate; }
    }
    return best;
}

/**
 * @brief Format RGB escapes in a rotating borrowed buffer.
 * @details Each foreground/background result survives seven subsequent calls
 * of the same kind. Rendering is single-threaded; no heap storage is needed.
 */
static const char *rgbColorCode(int32_t c, uint8_t background) {
    uint8_t *index = background ? &rgbBackgroundIndex : &rgbForegroundIndex;
    char (*buffers)[32] = background ? rgbBackground : rgbForeground;
    char *buffer = buffers[*index];
    *index = (uint8_t)((*index + 1) % 8);
    int32_t rgb = c - RGB_COLOR_BASE;
    snprintf(buffer, 32, "\x1b[%d;2;%d;%d;%dm", background ? 48 : 38,
        rgb >> 16, (rgb >> 8) & 255, rgb & 255);
    return buffer;
}

/**
 * @brief Get the foreground escape sequence for a palette entry.
 *
 * @details Default or unsupported values reset the terminal foreground;
 * callers wanting no output must check that case separately.
 * @return a borrowed literal.
 *
 * @note Light variants are the standard ANSI "bright" foreground codes
 * (\x1b[9Xm, 90-97), dark variants the normal-intensity ones (\x1b[3Xm, 30-37)
 * -- both are base ANSI, universally supported by any terminal the 8-color
 * palette already worked on.
 */
const char *ansiColorCode(int32_t c) {
    if (c >= RGB_COLOR_BASE && c <= RGB_COLOR_BASE + 0xffffff) return rgbColorCode(c, 0);
    switch (c) {
        case COLOR_GRAY_LIGHT:    return "\x1b[90m";
        case COLOR_GRAY_DARK:     return "\x1b[30m";
        case COLOR_GRAY_DIM:      return "\x1b[2;30m";
        case COLOR_BLUE_LIGHT:    return "\x1b[94m";
        case COLOR_BLUE_DARK:     return "\x1b[34m";
        case COLOR_BLUE_DIM:      return "\x1b[2;34m";
        case COLOR_GREEN_LIGHT:   return "\x1b[92m";
        case COLOR_GREEN_DARK:    return "\x1b[32m";
        case COLOR_GREEN_DIM:     return "\x1b[2;32m";
        case COLOR_YELLOW_LIGHT:  return "\x1b[93m";
        case COLOR_YELLOW_DARK:   return "\x1b[33m";
        case COLOR_YELLOW_DIM:    return "\x1b[2;33m";
        case COLOR_CYAN_LIGHT:    return "\x1b[96m";
        case COLOR_CYAN_DARK:     return "\x1b[36m";
        case COLOR_CYAN_DIM:      return "\x1b[2;36m";
        case COLOR_MAGENTA_LIGHT: return "\x1b[95m";
        case COLOR_MAGENTA_DARK:  return "\x1b[35m";
        case COLOR_MAGENTA_DIM:   return "\x1b[2;35m";
        case COLOR_RED_LIGHT:     return "\x1b[91m";
        case COLOR_RED_DARK:      return "\x1b[31m";
        case COLOR_RED_DIM:       return "\x1b[2;31m";
        case COLOR_WHITE_LIGHT:   return "\x1b[97m";
        case COLOR_WHITE_DARK:    return "\x1b[37m";
        case COLOR_WHITE_DIM:     return "\x1b[2;37m";
        /* COLOR_TERMINAL_DEFAULT: same "reset to default foreground"
         * escape as the fallback below -- callers that always need an
         * escape (gutter, selection, ...) get a sensible one; callers
         * for whom "no color at all" is meaningfully different (see
         * syntaxColorFor() in syntax.c) check for
         * COLOR_TERMINAL_DEFAULT themselves before calling this. */
        default:                  return "\x1b[39m";
    }
}

/**
 * @brief Get the background escape sequence for a palette entry.
 *
 * @details Default or unsupported values return an empty string to leave the
 * background alone.
 * @return a borrowed literal; dim variants use the corresponding dark
 * background.
 *
 * @note Background SGR codes are the foreground codes' family +10 (30-37 ->
 * 40-47, 90-97 -> 100-107) -- standard ANSI, same universally-supported base
 * palette as ansiColorCode(). "dim" (\x1b[2;3Xm) has no background equivalent
 * in the base ANSI spec (the intensity attribute only affects foreground on
 * any terminal this project targets), so dim variants reuse their base hue's
 * normal (dark) background -- still a distinct, correct color, just without a
 * separate "dim background" concept that doesn't exist to reuse.
 *
 * gray-dark/gray-dim -> \x1b[40m is BLACK, not an actual gray -- same as
 * ansiColorCode()'s COLOR_GRAY_DARK/_DIM, which are already plain black
 * foreground (see settings.h's comment on enum settingColor: "gray" here names
 * a position in the 8-hue ANSI palette, not a real gray). On a terminal whose
 * own background is already black/dark (common), color_background = gray-
 * dark/gray-dim will look like "no effect" for that reason -- intentional,
 * kept consistent with the existing foreground scheme rather than special-
 * cased into a real gray just for this one setting. Pick blue-dark/white-
 * dark/etc. for a visibly distinct editor background instead.
 */
const char *ansiBgColorCode(int32_t c) {
    if (c >= RGB_COLOR_BASE && c <= RGB_COLOR_BASE + 0xffffff) return rgbColorCode(c, 1);
    switch (c) {
        case COLOR_GRAY_LIGHT:                        return "\x1b[100m";
        case COLOR_GRAY_DARK: case COLOR_GRAY_DIM:     return "\x1b[40m";
        case COLOR_BLUE_LIGHT:                         return "\x1b[104m";
        case COLOR_BLUE_DARK: case COLOR_BLUE_DIM:      return "\x1b[44m";
        case COLOR_GREEN_LIGHT:                        return "\x1b[102m";
        case COLOR_GREEN_DARK: case COLOR_GREEN_DIM:    return "\x1b[42m";
        case COLOR_YELLOW_LIGHT:                       return "\x1b[103m";
        case COLOR_YELLOW_DARK: case COLOR_YELLOW_DIM:  return "\x1b[43m";
        case COLOR_CYAN_LIGHT:                         return "\x1b[106m";
        case COLOR_CYAN_DARK: case COLOR_CYAN_DIM:      return "\x1b[46m";
        case COLOR_MAGENTA_LIGHT:                      return "\x1b[105m";
        case COLOR_MAGENTA_DARK: case COLOR_MAGENTA_DIM: return "\x1b[45m";
        case COLOR_RED_LIGHT:                          return "\x1b[101m";
        case COLOR_RED_DARK: case COLOR_RED_DIM:        return "\x1b[41m";
        case COLOR_WHITE_LIGHT:                        return "\x1b[107m";
        case COLOR_WHITE_DARK: case COLOR_WHITE_DIM:    return "\x1b[47m";
        /* COLOR_TERMINAL_DEFAULT: "" rather than "\x1b[49m" (reset-to-
         * default-background) -- unlike ansiColorCode()'s
         * COLOR_TERMINAL_DEFAULT fallback, this setting's "off" state
         * (see color_background in settings.h) must never touch the
         * background at all, including implicitly resetting it, since
         * the caller may be re-asserting the active background after
         * an unrelated \x1b[m elsewhere on the same row. */
        default: return "";
    }
}

/**
 * @brief Identify a dim foreground variant in the shared palette.
 *
 * @return 1 only for the dim entries of the eight hue triplets; the terminal-
 * default entry is not dim.
 *
 * @note The palette is 8 hues x 3 variants in light/dark/dim order (see enum
 * settingColor), so the dim ones are exactly the indices below 24 whose
 * position within their hue triplet is 2. COLOR_TERMINAL_DEFAULT (24) and
 * anything out of range are not dim.
 */
uint8_t settingColorIsDim(int32_t c) {
    return c >= 0 && c < 24 && c % 3 == 2;
}

/**
 * @brief Get the configuration spelling of a palette entry.
 *
 * @return a borrowed name, with white-dark as the fallback for an out-of-range
 * index.
 */
const char *settingColorName(int32_t c) {
    if (c >= 0 && c < SETTING_COLOR_COUNT) return colorNames[c];
    return "white-dark";
}
