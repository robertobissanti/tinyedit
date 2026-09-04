/* settings.h -- persistent user settings for tinyedit, loaded from and
 * saved to ~/.tinyeditrc (simple "key = value" INI-style format).
 *
 * The descriptor table in settings.c drives both the file parser and
 * the F2 settings screen in tinyedit.c, so adding a new option is a
 * matter of adding a field here and one row to that table -- not new
 * parsing or rendering code.
 */

#ifndef __TE_SETTINGS_H
#define __TE_SETTINGS_H

#include <stddef.h>

enum settingRedoKey {
    REDO_KEY_CTRL_Y = 0,
    REDO_KEY_CTRL_SHIFT_Z
};

/* A small fixed palette of ANSI foreground colors, enough to
 * distinguish editor UI elements without pulling in 256-color/truecolor
 * handling. See ansiColorCode() for the escape sequence each maps to. */
enum settingColor {
    COLOR_GRAY = 0,
    COLOR_BLUE,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_CYAN,
    COLOR_MAGENTA,
    COLOR_RED,
    COLOR_WHITE
};

struct editorSettings {
    int show_line_numbers;
    int tab_stop;
    enum settingRedoKey redo_key;
    int undo_max_depth;
    enum settingColor color_gutter;
    enum settingColor color_selection;
    enum settingColor color_statusbar;
};

/* Which primitive type a setting's value is, for the generic
 * descriptor-driven parser/TUI. */
enum settingType {
    SETTING_BOOL,
    SETTING_INT,
    SETTING_ENUM
};

/* Describes one configurable setting: its key in the config file, its
 * label in the F2 screen, its type, where its value lives in
 * struct editorSettings (as a byte offset, so the same descriptor
 * drives both load/save and the settings screen), and -- for enums --
 * the list of valid names/values to cycle through. */
struct settingDescriptor {
    const char *key;    /* e.g. "tab_stop", used in ~/.tinyeditrc */
    const char *label;  /* e.g. "Tab width", shown in the F2 screen */
    enum settingType type;
    size_t offset;       /* offsetof(struct editorSettings, field) */
    int int_min, int_max; /* only used for SETTING_INT */
    const char *const *enum_names;  /* only used for SETTING_ENUM, NULL-terminated */
    int enum_count;
};

extern const struct settingDescriptor settingDescriptors[];
extern const int settingDescriptorCount;

/* Maps a file extension to a human-readable language/filetype name for
 * the status bar (e.g. "c" -> "C", "py" -> "Python"). Falls back to a
 * built-in static table (~30 common languages); entries in
 * ~/.tinyeditrc as "filetype.<ext> = <Name>" override or extend it.
 * User overrides are loaded once by settingsLoad() and preserved by
 * settingsSave() even though the F2 screen doesn't edit them directly
 * yet -- this state lives inside settings.c, callers don't need to
 * pass it around. */

/* Returns the filetype name for `ext` (without the leading dot, e.g.
 * "c" not ".c"), or NULL if unknown. Checks user overrides first, then
 * falls back to the built-in table. Returned pointer is either a
 * static string or owned by the module's override table -- do not
 * free, valid until the next settingsLoad() call. */
const char *filetypeForExtension(const char *ext);

/* Fills *out with hardcoded defaults. Always succeeds. */
void settingsDefaults(struct editorSettings *out);

/* Loads settings from ~/.tinyeditrc into *out. Starts from
 * settingsDefaults() and overrides only the keys present in the file,
 * so a partial or missing file still yields a fully valid settings
 * struct. Unknown keys/malformed lines are silently skipped (a config
 * file is not something a user expects to get a parse error dialog
 * from -- this matches the tolerant style of most INI-ish tools). */
void settingsLoad(struct editorSettings *out);

/* Writes *s to ~/.tinyeditrc. Returns 1 on success, 0 on I/O failure. */
int settingsSave(const struct editorSettings *s);

/* ANSI foreground color escape sequence (e.g. "\x1b[90m") for a given
 * settingColor. Returned pointer is a static string, do not free. */
const char *ansiColorCode(enum settingColor c);

/* Human-readable name for a settingColor (e.g. "gray"), used both when
 * writing the config file and in the F2 screen. */
const char *settingColorName(enum settingColor c);

#endif /* __TE_SETTINGS_H */
