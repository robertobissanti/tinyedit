/* settings.c -- see settings.h */

#include "settings.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const redoKeyNames[] = { "ctrl-y", "ctrl-shift-z", NULL };
static const char *const colorNames[] = {
    "gray", "blue", "green", "yellow", "cyan", "magenta", "red", "white", NULL
};

/* Built-in extension -> filetype name table, shown in the status bar.
 * Not exhaustive (see github/linguist for a much larger reference) --
 * just the languages/formats a typical user is likely to hit. Extended
 * or overridden per-user via "filetype.<ext> = <Name>" lines in
 * ~/.tinyeditrc (see filetypeOverrides below). */
struct filetypeEntry {
    const char *ext;
    const char *name;
};

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
static const int builtinFiletypeCount =
    (int)(sizeof(builtinFiletypes) / sizeof(builtinFiletypes[0]));

/* User-defined overrides/additions from ~/.tinyeditrc's "filetype.*"
 * keys, loaded once by settingsLoad() and preserved verbatim by
 * settingsSave() (which doesn't otherwise know about them -- the F2
 * screen doesn't edit this table yet). Owned by this module: freed
 * only by being reset at the next settingsLoad() call, per-process
 * this is a small, one-time allocation. */
static struct filetypeEntry *filetypeOverrides = NULL;
static int filetypeOverrideCount = 0;

/* Descriptor table: the single source of truth for every setting's key,
 * label, type, storage location, and valid range/values. Both the
 * ~/.tinyeditrc parser and the F2 settings screen walk this table
 * instead of hardcoding each option. */
const struct settingDescriptor settingDescriptors[] = {
    { "show_line_numbers", "Show line numbers", SETTING_BOOL,
      offsetof(struct editorSettings, show_line_numbers), 0, 0, NULL, 0 },
    { "tab_stop", "Tab width", SETTING_INT,
      offsetof(struct editorSettings, tab_stop), 1, 16, NULL, 0 },
    { "redo_key", "Redo key", SETTING_ENUM,
      offsetof(struct editorSettings, redo_key), 0, 0, redoKeyNames, 2 },
    { "undo_max_depth", "Undo history depth", SETTING_INT,
      offsetof(struct editorSettings, undo_max_depth), 10, 2000, NULL, 0 },
    { "color_gutter", "Gutter color", SETTING_ENUM,
      offsetof(struct editorSettings, color_gutter), 0, 0, colorNames, 8 },
    { "color_selection", "Selection color", SETTING_ENUM,
      offsetof(struct editorSettings, color_selection), 0, 0, colorNames, 8 },
    { "color_statusbar", "Status bar color", SETTING_ENUM,
      offsetof(struct editorSettings, color_statusbar), 0, 0, colorNames, 8 },
};
const int settingDescriptorCount = (int)(sizeof(settingDescriptors) / sizeof(settingDescriptors[0]));

/* Returns a pointer to the int-sized storage for descriptor `d` inside
 * `s`. Every field in editorSettings (bool/int/enum) is stored as a
 * plain int-compatible type, so this single cast covers all of them. */
static int *settingSlot(struct editorSettings *s, const struct settingDescriptor *d) {
    return (int *)((char *)s + d->offset);
}

static const int *settingSlotConst(const struct editorSettings *s, const struct settingDescriptor *d) {
    return (const int *)((const char *)s + d->offset);
}

void settingsDefaults(struct editorSettings *out) {
    out->show_line_numbers = 1;
    out->tab_stop = 4;
    out->redo_key = REDO_KEY_CTRL_Y;
    out->undo_max_depth = 200;
    out->color_gutter = COLOR_GRAY;
    out->color_selection = COLOR_WHITE;
    out->color_statusbar = COLOR_WHITE;
}

static const char *configPath(char *buf, size_t buflen) {
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    snprintf(buf, buflen, "%s/.tinyeditrc", home);
    return buf;
}

/* Finds the enum index of `name` in a NULL-terminated name list, or -1. */
static int enumIndexOf(const char *const *names, const char *name) {
    for (int i = 0; names[i]; i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}

static void trim(char *s) {
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

#define FILETYPE_KEY_PREFIX "filetype."

static void addFiletypeOverride(const char *ext, const char *name) {
    /* Same extension re-declared later in the file wins (matches how
     * the descriptor-based settings above already let the last
     * occurrence of a key win, since the loop just keeps overwriting
     * the same slot). */
    for (int i = 0; i < filetypeOverrideCount; i++) {
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
    for (int i = 0; i < filetypeOverrideCount; i++) {
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
    if (!fp) return; /* no config file yet: defaults stand */

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

        for (int i = 0; i < settingDescriptorCount; i++) {
            const struct settingDescriptor *d = &settingDescriptors[i];
            if (strcmp(d->key, key) != 0) continue;

            int *slot = settingSlot(out, d);
            if (d->type == SETTING_BOOL) {
                *slot = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
            } else if (d->type == SETTING_INT) {
                int v = atoi(value);
                if (v < d->int_min) v = d->int_min;
                if (v > d->int_max) v = d->int_max;
                *slot = v;
            } else { /* SETTING_ENUM */
                int idx = enumIndexOf(d->enum_names, value);
                if (idx >= 0) *slot = idx;
            }
            break;
        }
    }
    fclose(fp);
}

int settingsSave(const struct editorSettings *s) {
    char path[1024];
    if (!configPath(path, sizeof(path))) return 0;

    FILE *fp = fopen(path, "w");
    if (!fp) return 0;

    fprintf(fp, "# tinyedit configuration -- edit with F2 inside the editor,\n");
    fprintf(fp, "# or by hand (key = value, '#' starts a comment).\n\n");

    for (int i = 0; i < settingDescriptorCount; i++) {
        const struct settingDescriptor *d = &settingDescriptors[i];
        const int *slot = settingSlotConst(s, d);

        if (d->type == SETTING_BOOL) {
            fprintf(fp, "%s = %s\n", d->key, *slot ? "true" : "false");
        } else if (d->type == SETTING_INT) {
            fprintf(fp, "%s = %d\n", d->key, *slot);
        } else {
            fprintf(fp, "%s = %s\n", d->key, d->enum_names[*slot]);
        }
    }

    /* Preserve filetype.* overrides even though the F2 screen doesn't
     * edit them yet -- otherwise saving settings from F2 would silently
     * wipe out anything the user added by hand. */
    if (filetypeOverrideCount > 0) {
        fprintf(fp, "\n# filetype overrides (status bar language name)\n");
        for (int i = 0; i < filetypeOverrideCount; i++)
            fprintf(fp, "%s%s = %s\n", FILETYPE_KEY_PREFIX,
                filetypeOverrides[i].ext, filetypeOverrides[i].name);
    }

    fclose(fp);
    return 1;
}

const char *filetypeForExtension(const char *ext) {
    if (!ext || ext[0] == '\0') return NULL;

    for (int i = 0; i < filetypeOverrideCount; i++)
        if (strcmp(filetypeOverrides[i].ext, ext) == 0)
            return filetypeOverrides[i].name;

    for (int i = 0; i < builtinFiletypeCount; i++)
        if (strcmp(builtinFiletypes[i].ext, ext) == 0)
            return builtinFiletypes[i].name;

    return NULL;
}

const char *ansiColorCode(enum settingColor c) {
    switch (c) {
        case COLOR_GRAY:    return "\x1b[90m";
        case COLOR_BLUE:    return "\x1b[34m";
        case COLOR_GREEN:   return "\x1b[32m";
        case COLOR_YELLOW:  return "\x1b[33m";
        case COLOR_CYAN:    return "\x1b[36m";
        case COLOR_MAGENTA: return "\x1b[35m";
        case COLOR_RED:     return "\x1b[31m";
        case COLOR_WHITE:   return "\x1b[37m";
        default:            return "\x1b[39m";
    }
}

const char *settingColorName(enum settingColor c) {
    if (c >= 0 && c < 8) return colorNames[c];
    return "white";
}
