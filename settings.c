/* settings.c -- see settings.h */

#define _DEFAULT_SOURCE

#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const redoKeyNames[] = { "ctrl-y", "ctrl-shift-z", NULL };
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
    { "show_line_numbers", "Show line numbers", SETTING_BOOL,
      offsetof(struct editorSettings, show_line_numbers), 0, 0, NULL, 0 },
    { "tab_stop", "Tab width", SETTING_INT,
      offsetof(struct editorSettings, tab_stop), 1, 16, NULL, 0 },
    { "mouse_enabled", "Mouse support (disables native terminal selection)", SETTING_BOOL,
      offsetof(struct editorSettings, mouse_enabled), 0, 0, NULL, 0 },
    { "redo_key", "Redo key", SETTING_ENUM,
      offsetof(struct editorSettings, redo_key), 0, 0, redoKeyNames, 2 },
    { "undo_max_depth", "Undo history depth", SETTING_INT,
      offsetof(struct editorSettings, undo_max_depth), 10, 2000, NULL, 0 },
    { "color_gutter", "Gutter color", SETTING_ENUM,
      offsetof(struct editorSettings, color_gutter), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_selection", "Selection color", SETTING_ENUM,
      offsetof(struct editorSettings, color_selection), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_statusbar", "Status bar color", SETTING_ENUM,
      offsetof(struct editorSettings, color_statusbar), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "show_top_bar", "Show top bar", SETTING_BOOL,
      offsetof(struct editorSettings, show_top_bar), 0, 0, NULL, 0 },
    { "soft_wrap", "Max wrap width (0=window edge)", SETTING_INT,
      offsetof(struct editorSettings, soft_wrap), 0, 500, NULL, 0 },
    { "home_end_visual_line", "Home/End use visual line", SETTING_BOOL,
      offsetof(struct editorSettings, home_end_visual_line), 0, 0, NULL, 0 },
    { "backup_interval", "Backup interval s (0=off, min 5)", SETTING_INT,
      offsetof(struct editorSettings, backup_interval), 0, 3600, NULL, 0 },
    { "auto_indent", "Auto-indent new lines", SETTING_BOOL,
      offsetof(struct editorSettings, auto_indent), 0, 0, NULL, 0 },
    { "auto_close_pairs", "Auto-close brackets/quotes", SETTING_BOOL,
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
    { "color_syntax_comment", "Syntax: comment color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_comment), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_number", "Syntax: number color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_number), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_preprocessor", "Syntax: preprocessor color", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_preprocessor), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_emphasis_strong", "Syntax: bold text color (Markdown)", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_emphasis_strong), 0, 0, colorNames, SETTING_COLOR_COUNT },
    { "color_syntax_math", "Syntax: LaTeX math color (Markdown)", SETTING_ENUM,
      offsetof(struct editorSettings, color_syntax_math), 0, 0, colorNames, SETTING_COLOR_COUNT },
};
const int32_t settingDescriptorCount = (int32_t)(sizeof(settingDescriptors) / sizeof(settingDescriptors[0]));

/* Returns a pointer to the int32_t-sized storage for descriptor `d`
 * inside `s`. Every field in editorSettings (bool/int/enum) is
 * int32_t (see settings.h), so this single cast covers all of them. */
static int32_t *settingSlot(struct editorSettings *s, const struct settingDescriptor *d) {
    return (int32_t *)((char *)s + d->offset);
}

static const int32_t *settingSlotConst(const struct editorSettings *s, const struct settingDescriptor *d) {
    return (const int32_t *)((const char *)s + d->offset);
}

void settingsDefaults(struct editorSettings *out) {
    out->show_line_numbers = 1;
    out->tab_stop = 4;
    out->redo_key = REDO_KEY_CTRL_Y;
    out->undo_max_depth = 200;
    out->color_gutter = COLOR_GRAY_LIGHT;
    out->color_selection = COLOR_WHITE_DARK;
    out->color_statusbar = COLOR_WHITE_DARK;
    out->show_top_bar = 0;
    out->soft_wrap = 0;
    out->home_end_visual_line = 1;
    out->backup_interval = 0;
    out->auto_indent = 1;
    out->auto_close_pairs = 1;
    out->auto_close_single_quote = 0;
    out->insert_spaces_for_tab = 1;
    out->show_invisibles = 0;
    out->color_invisibles = COLOR_GRAY_LIGHT;
    out->syntax_highlight = 1;
    out->color_syntax_normal = COLOR_TERMINAL_DEFAULT;
    out->color_syntax_keyword = COLOR_BLUE_LIGHT;
    out->color_syntax_string = COLOR_GREEN_LIGHT;
    out->color_syntax_comment = COLOR_GRAY_DIM;
    out->color_syntax_number = COLOR_MAGENTA_LIGHT;
    out->color_syntax_preprocessor = COLOR_YELLOW_LIGHT;
    out->color_syntax_emphasis_strong = COLOR_RED_LIGHT;
    out->color_syntax_math = COLOR_CYAN_LIGHT;
    out->mouse_enabled = 0;
}

static const char *configPath(char *buf, size_t buflen) {
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    int32_t n = snprintf(buf, buflen, "%s/.tinyeditrc", home);
    if (n < 0 || (size_t)n >= buflen) return NULL;
    return buf;
}

/* Finds the enum index of `name` in a NULL-terminated name list, or -1. */
static int32_t enumIndexOf(const char *const *names, const char *name) {
    for (int32_t i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

/* Maps a pre-split color name ("gray", "cyan", ...) written by an
 * older tinyedit version to its closest equivalent in the current
 * 3-variants-per-hue palette, or -1 if `name` isn't one of the 8
 * legacy names. Without this, an older ~/.tinyeditrc would silently
 * lose its color customization on load (enumIndexOf() finding no
 * exact match in the new "hue-light"/"hue-dark"/"hue-dim" names,
 * leaving the field at settingsDefaults()'s value instead) -- exactly
 * the migration gap the change in enum settingColor's field comment
 * warns about. Each legacy name maps to its "light" variant: that's
 * what the old single-variant palette actually rendered as (see
 * ansiColorCode() -- e.g. old COLOR_GRAY was already \x1b[90m, the
 * bright/light code, not \x1b[30m). Works unchanged across both the
 * original 2-variant (light/dark) and current 3-variant
 * (light/dark/dim) palette since "light" is always index 0 of each
 * hue's block -- only the block STRIDE changed (2 -> 3), captured
 * here as HUE_COLOR_COUNT (the 24 real hues, i.e. SETTING_COLOR_COUNT
 * minus COLOR_TERMINAL_DEFAULT -- see settings.h) divided by the
 * legacy name count rather than hardcoded, so a future variant
 * addition doesn't need this function touched again. */
static int32_t settingColorFromLegacyName(const char *name) {
    static const char *const legacyNames[] = {
        "gray", "blue", "green", "yellow", "cyan", "magenta", "red", "white", NULL
    };
    enum { HUE_COLOR_COUNT = SETTING_COLOR_COUNT - 1 };
    int32_t variants_per_hue = HUE_COLOR_COUNT / 8;
    for (int32_t i = 0; legacyNames[i]; i++)
        if (strcmp(legacyNames[i], name) == 0) return i * variants_per_hue; /* *_LIGHT is first in each block */
    return -1;
}

static void trim(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static void addFiletypeOverride(const char *ext, const char *name) {
    /* Same extension re-declared later in the file wins (matches how
     * the descriptor-based settings above already let the last
     * occurrence of a key win, since the loop just keeps overwriting
     * the same slot). */
    for (int32_t i = 0; i < filetypeOverrideCount; i++) {
        if (strcmp(filetypeOverrides[i].ext, ext) == 0) {
            free((void *)filetypeOverrides[i].name);
            filetypeOverrides[i].name = strdup(name);
            return;
        }
    }
    filetypeOverrides = realloc(filetypeOverrides,
        sizeof(struct filetypeEntry) * (size_t)(filetypeOverrideCount + 1));
    filetypeOverrides[filetypeOverrideCount].ext = strdup(ext);
    filetypeOverrides[filetypeOverrideCount].name = strdup(name);
    filetypeOverrideCount++;
}

static void freeFiletypeOverrides(void) {
    for (int32_t i = 0; i < filetypeOverrideCount; i++) {
        free((void *)filetypeOverrides[i].ext);
        free((void *)filetypeOverrides[i].name);
    }
    free(filetypeOverrides);
    filetypeOverrides = NULL;
    filetypeOverrideCount = 0;
}

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

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
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

            int32_t *slot = settingSlot(out, d);
            if (d->type == SETTING_BOOL) {
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
}

uint8_t settingsSave(const struct editorSettings *s) {
    char path[1024];
    if (!configPath(path, sizeof(path))) return 0;

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
    fprintf(fp, "# or by hand (key = value, '#' starts a comment).\n\n");

    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        const struct settingDescriptor *d = &settingDescriptors[i];
        const int32_t *slot = settingSlotConst(s, d);

        if (d->type == SETTING_BOOL) {
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
    if (fclose(fp) != 0) ok = 0;
    if (ok && rename(tmppath, path) != 0) ok = 0;
    if (!ok) unlink(tmppath);
    return ok;
}

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

/* Light variants are the standard ANSI "bright" foreground codes
 * (\x1b[9Xm, 90-97), dark variants the normal-intensity ones
 * (\x1b[3Xm, 30-37) -- both are base ANSI, universally supported by
 * any terminal the 8-color palette already worked on. */
const char *ansiColorCode(int32_t c) {
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

const char *settingColorName(int32_t c) {
    if (c >= 0 && c < SETTING_COLOR_COUNT) return colorNames[c];
    return "white-dark";
}
