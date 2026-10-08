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
#include <stdint.h>

#define FILETYPE_KEY_PREFIX "filetype."

enum settingRedoKey {
    REDO_KEY_CTRL_Y = 0,
    REDO_KEY_CTRL_SHIFT_Z
};

/* A small fixed palette of ANSI foreground colors, enough to
 * distinguish editor UI elements without pulling in 256-color/truecolor
 * handling. See ansiColorCode() for the escape sequence each maps to.
 * Each of the 8 base hues has three variants, all standard ANSI (not
 * 256-color/truecolor) so this doesn't relax the "no 256-color/
 * truecolor" constraint above:
 *   - "light": bright, \x1b[9Xm
 *   - "dark": normal-intensity, \x1b[3Xm
 *   - "dim": faint/dim, \x1b[2;3Xm -- a step darker/muted than "dark".
 *     Support is less universal than bright/normal (some terminals
 *     render it identically to "dark" instead of actually dimming),
 *     but it's still base ANSI, not an extended palette.
 * COLOR_GRAY_LIGHT keeps the same code point/escape (\x1b[90m) the
 * old single COLOR_GRAY used, so existing ~/.tinyeditrc files with
 * "gray" still resolve to the same visible color (see
 * settingColorFromLegacyName() in settings.c). */
enum settingColor {
    COLOR_GRAY_LIGHT = 0,
    COLOR_GRAY_DARK,
    COLOR_GRAY_DIM,
    COLOR_BLUE_LIGHT,
    COLOR_BLUE_DARK,
    COLOR_BLUE_DIM,
    COLOR_GREEN_LIGHT,
    COLOR_GREEN_DARK,
    COLOR_GREEN_DIM,
    COLOR_YELLOW_LIGHT,
    COLOR_YELLOW_DARK,
    COLOR_YELLOW_DIM,
    COLOR_CYAN_LIGHT,
    COLOR_CYAN_DARK,
    COLOR_CYAN_DIM,
    COLOR_MAGENTA_LIGHT,
    COLOR_MAGENTA_DARK,
    COLOR_MAGENTA_DIM,
    COLOR_RED_LIGHT,
    COLOR_RED_DARK,
    COLOR_RED_DIM,
    COLOR_WHITE_LIGHT,
    COLOR_WHITE_DARK,
    COLOR_WHITE_DIM,
    /* Not a real color: "don't emit any color escape, leave the
     * terminal's own default foreground". Appended after the 24 real
     * hues (not interleaved into the light/dark/dim scheme) so it
     * doesn't shift any existing hue's index -- ansiColorCode()'s
     * legacy-migration math (SETTING_COLOR_COUNT / 8) and every
     * ~/.tinyeditrc already on disk stay valid unchanged. Exists for
     * settings where "no color" is a meaningful, distinct choice from
     * any of the 24 hues, not merely their fallback on lookup failure
     * (see color_syntax_normal in struct editorSettings). */
    COLOR_TERMINAL_DEFAULT,
    SETTING_COLOR_COUNT,
    SETTING_HUE_COLOR_COUNT = SETTING_COLOR_COUNT - 1
};

/* DECSCUSR cursor shapes. */
enum cursorStyle {
    CURSOR_BLOCK = 0,
    CURSOR_BAR
};

enum lineEndingMode {
    LINE_ENDING_AUTO = 0,
    LINE_ENDING_LF,
    LINE_ENDING_CRLF
};

struct filetypeEntry {
    const char *ext;
    const char *name;
};

/* Every field here is int32_t -- including enums and what are
 * logically booleans -- because settingSlot()/settingsScreenSlot()
 * access fields generically through a byte offset cast to int32_t*
 * (see struct settingDescriptor below): the descriptor-driven
 * parser/F2-screen only works if every field is the same size. Using
 * e.g. uint8_t for the bools here would make that generic accessor
 * read/write the wrong width. */
struct editorSettings {
    int32_t show_line_numbers;
    int32_t tab_stop;
    int32_t redo_key;
    int32_t undo_max_depth;
    int32_t undo_memory_mb; /* Applied at startup or document reset. */
    int32_t color_gutter;
    int32_t color_selection;
    int32_t color_statusbar;
    int32_t color_statusbar_text;
    int32_t show_top_bar;
    /* Enables the F10/mouse menu overlay and its message-bar entry point.
     * F2 remains available so the menu can always be turned back on. */
    int32_t show_menu;
    /* Wrap is always on (no horizontal scrolling): text always wraps
     * at the window edge at minimum. 0 means no extra limit beyond
     * that; a positive value additionally caps the wrap width when the
     * window is wider than it. See editorSoftWrapCols() in tinyedit.c. */
    int32_t soft_wrap;
    /* Minimum number of visual rows kept above and below the cursor
     * while scrolling, when the file is long enough. 0 retains the
     * edge-following behavior. */
    int32_t scrolloff;
    /* When true (default), Home/End move to the start/end of the
     * current VISUAL (wrapped) segment, matching Up/Down which also
     * move by video row -- this is the VS Code/Sublime convention.
     * When false, Home/End always move to the start/end of the whole
     * LOGICAL line regardless of how many video rows it wraps into
     * (the vim convention) -- diverges from Up/Down in that case. */
    int32_t home_end_visual_line;
    /* Seconds between automatic crash-recovery backup writes (see
     * backup.h) while the buffer is dirty. 0 disables the feature
     * entirely; otherwise clamped to >= 5 to avoid hammering disk on
     * a fast-typing session. */
    int32_t backup_interval;
    /* Enter copies the leading whitespace (spaces/tabs) of the
     * current line onto the new one. */
    int32_t auto_indent;
    /* Typing an opening bracket/quote inserts its matching close
     * right after the cursor (or wraps the active selection); typing
     * the close manually while the cursor sits right before an
     * auto-inserted one skips over it instead of duplicating it. See
     * editorAutoClosePairFor() in editor_state.c for the shared pair table. */
    int32_t auto_close_pairs;
    /* Whether auto_close_pairs above also applies to a typed "'"
     * (single quote/apostrophe). Split out and defaulted to false
     * unlike every other pair in editorAutoCloseTable(), because
     * apostrophes inside contractions/possessives ("don't", "user's")
     * are far more common in prose than a matching pair, so
     * auto-closing it is disruptive there in a way "(" or "\"" isn't.
     * When false, typing "'" always inserts a single "'", regardless
     * of auto_close_pairs. */
    int32_t auto_close_single_quote;
    /* When true, the Tab key inserts tab_stop spaces instead of a
     * literal '\t' byte. Only affects the Tab key itself -- auto_indent
     * always copies a line's existing leading whitespace verbatim
     * (tabs stay tabs, spaces stay spaces) regardless of this. */
    int32_t insert_spaces_for_tab;
    /* Renders spaces, tabs, and end-of-line as visible glyphs
     * (middle dot, arrow, pilcrow) instead of blank space. See
     * editorRenderInvisibles() in tinyedit.c. */
    int32_t show_invisibles;
    int32_t color_invisibles;
    /* Tokenizes and colors C source (keywords, strings, comments,
     * numbers, preprocessor directives) -- see syntax.h. Auto-detected
     * per file from its extension; this setting is the master on/off
     * switch. */
    int32_t syntax_highlight;
    /* Swap heading foreground and background using terminal reverse video. */
    int32_t markdown_heading_reverse;
    int32_t markdown_text_styles;
    /* Color for text with no highlight class (HL_NORMAL, see
     * syntax.h) -- e.g. identifiers, punctuation, whitespace. Default
     * COLOR_TERMINAL_DEFAULT (no escape emitted, terminal's own
     * foreground), same as before this setting existed; set to any of
     * the 24 real hues to recolor plain code text explicitly, distinct
     * from the classified categories below. */
    int32_t color_syntax_normal;
    int32_t color_syntax_keyword;
    int32_t color_syntax_string;
    int32_t color_syntax_json_key;
    int32_t color_syntax_bracket;
    int32_t color_syntax_comment;
    int32_t color_syntax_number;
    int32_t color_syntax_preprocessor;
    /* Markdown-only: color for bold text (double asterisk or double
     * underscore marker), distinct from color_syntax_keyword which
     * single-marker italic text uses -- see enum syntaxHighlight's
     * HL_EMPHASIS_STRONG in syntax.h. */
    int32_t color_syntax_emphasis_strong;
    /* Markdown-only: color for LaTeX math ("$...$"/"$$...$$"), distinct
     * from color_syntax_string which inline code spans/fences use --
     * see enum syntaxHighlight's HL_MATH in syntax.h. */
    int32_t color_syntax_math;
    /* C/C++ (and any user-defined C-like language) only: color for an
     * identifier immediately followed by '(' -- see HL_FUNCTION in
     * syntax.h. */
    int32_t color_syntax_function;
    /* Background color for the whole editor (behind every row, the
     * gutter, status/message bars). COLOR_TERMINAL_DEFAULT (the
     * default) emits no background escape at all, leaving the
     * terminal's own background exactly as before this setting
     * existed -- same "off means untouched" convention as
     * color_syntax_normal. See ansiBgColorCode() in settings.h/.c. */
    int32_t color_background;
    /* Enables SGR mouse reporting (click to move the cursor, drag to
     * select, wheel to scroll) -- see enableMouseReporting() in
     * tinyedit.c. Default false: turning this on disables the
     * terminal's own native text selection (e.g. Cmd+C/Cmd+V on
     * Ghostty) while tinyedit is running, since the terminal hands
     * mouse events to the foreground program instead of handling them
     * itself -- therefore this remains an explicit opt-in. */
    int32_t mouse_enabled;
/* Ghostty's macOS Command keys are unavailable on every other platform. */
#ifdef __APPLE__
    int32_t mac_command_keys;
#endif
    /* CURSOR_BLOCK or CURSOR_BAR (I-beam). */
    int32_t cursor_style;
    /* Preserve detected style, or force LF/CRLF when saving. */
    int32_t line_ending;
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
    int32_t int_min, int_max; /* only used for SETTING_INT */
    const char *const *enum_names;  /* only used for SETTING_ENUM, NULL-terminated */
    int32_t enum_count;
};

extern const struct settingDescriptor settingDescriptors[];
extern const int32_t settingDescriptorCount;

/**
 * @brief Look up an option by its configuration key.
 *
 * @details key is NUL-terminated and case-sensitive.
 * @return a borrowed descriptor or NULL when unknown.
 */
const struct settingDescriptor *settingsFind(const char *key);
/**
 * @brief Read a named boolean option from a settings object.
 *
 * @return its truth value, or 0 when the key is missing or not a boolean
 * setting.
 */
uint8_t settingsGetBool(const struct editorSettings *settings, const char *key);
/**
 * @brief Toggle a named boolean option in a settings object.
 *
 * @details Does not apply terminal changes or save configuration.
 * @return 1 when toggled, 0 for a missing or non-boolean key.
 */
uint8_t settingsToggleBool(struct editorSettings *settings, const char *key);

/**
 * @brief Look up a readable filetype label, preferring user overrides.
 *
 * @details ext is case-sensitive without a leading dot.
 * @return a borrowed string or NULL when unknown; its lifetime depends on the
 * override table.
 */
const char *filetypeForExtension(const char *ext);

/**
 * @brief Register a nonempty extension and its readable label.
 *
 * @details Copies both strings into the override table; invalid empty or NULL
 * inputs are ignored. Persistence is a separate settingsSave() call.
 */
void settingsSetFiletype(const char *ext, const char *name);

/**
 * @brief Fill every setting field with its built-in default.
 *
 * @details out must point to writable settings storage; no file is read or
 * written and filetype overrides are untouched.
 */
void settingsDefaults(struct editorSettings *out);

/**
 * @brief Load configuration over defaults and rebuild filetype overrides.
 *
 * @details out receives valid settings even when the file is missing or
 * malformed. Missing configuration is created when possible; invalid entries
 * are ignored and integers are clamped.
 */
void settingsLoad(struct editorSettings *out);

/**
 * @brief Persist settings and known filetype overrides through a temporary file.
 *
 * @details s must contain valid descriptor values, including enum indices.
 * @return 1 after file and directory sync, otherwise 0 with the temporary
 * file removed. A post-rename sync failure may have replaced the config.
 */
uint8_t settingsSave(const struct editorSettings *s);

/**
 * @brief Get the foreground escape sequence for a palette entry.
 *
 * @details Default or unsupported values reset the terminal foreground;
 * callers wanting no output must check that case separately.
 * @return a borrowed literal.
 */
const char *ansiColorCode(int32_t c);

/**
 * @brief Get the background escape sequence for a palette entry.
 *
 * @details Default or unsupported values return an empty string to leave the
 * background alone.
 * @return a borrowed literal; dim variants use the corresponding dark
 * background.
 */
const char *ansiBgColorCode(int32_t c);

/**
 * @brief Identify a dim foreground variant in the shared palette.
 *
 * @return 1 only for the dim entries of the eight hue triplets; the terminal-
 * default entry is not dim.
 */
uint8_t settingColorIsDim(int32_t c);

/**
 * @brief Get the configuration spelling of a palette entry.
 *
 * @return a borrowed name, with white-dark as the fallback for an out-of-range
 * index.
 */
const char *settingColorName(int32_t c);

#endif /* __TE_SETTINGS_H */
