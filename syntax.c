/* syntax.c -- see syntax.h */

#include "syntax.h"
#include "utf8.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- language tables ---------------------------------------------- */

/* Describes one "C-like" language for the generic tokenizer below:
 * a keyword list, a quote-character set for strings, and a comment
 * style. Adding a language is adding one of these plus its extension
 * list to syntaxLangTable -- no new tokenizer code.
 *
 * The trailing NULL in each initializer below is the `filetype` field:
 * built-in languages take their status-bar name from settings.c's own
 * table instead, so they leave it unset. It's spelled out rather than
 * left off because the test build treats a missing field initializer
 * as a warning. */
static const char *const cExtensions[] = { "c", "h", NULL };
static const char *const cKeywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do",
    "else", "enum", "extern", "for", "goto", "if", "inline", "register",
    "restrict", "return", "sizeof", "static", "struct", "switch",
    "typedef", "union", "volatile", "while", "_Bool", "_Complex",
    "_Imaginary",
    "char", "double", "float", "int", "long", "short", "signed",
    "unsigned", "void", "uint", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", "ssize_t",
    "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "ptrdiff_t",
    "wchar_t",
    NULL
};

static const struct syntaxLang cLang = {
    cExtensions, cKeywords, "\"'", "//", "/*", "*/", 1, NULL, 0, 1, NULL, BASE_TOKENIZER_GENERIC, NULL
};

static const char *const cppExtensions[] = { "cpp", "cc", "cxx", "hpp", "hh", "hxx", NULL };
static const char *const cppKeywords[] = {
    /* C keywords, minus the ones C++ repurposes as something else
     * below (e.g. C's "register"/"restrict" storage-class keywords
     * are deprecated/absent in C++; kept out to avoid ever tagging
     * a C++ identifier that reuses that name). Order/content otherwise
     * matches cKeywords. */
    "break", "case", "const", "continue", "default", "do", "else",
    "enum", "extern", "for", "goto", "if", "inline", "return", "sizeof",
    "static", "struct", "switch", "typedef", "union", "while",
    "char", "double", "float", "int", "long", "short", "signed",
    "unsigned", "void", "uint", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", "ssize_t",
    "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "ptrdiff_t",
    "wchar_t",
    /* C++-specific */
    "auto", "bool", "catch", "class", "const_cast", "constexpr",
    "delete", "dynamic_cast", "explicit", "export", "false", "friend",
    "mutable", "namespace", "new", "noexcept", "nullptr", "operator",
    "override", "private", "protected", "public", "reinterpret_cast",
    "static_assert", "static_cast", "template", "this", "throw", "true",
    "try", "typename", "using", "virtual",
    NULL
};
static const struct syntaxLang cppLang = {
    cppExtensions, cppKeywords, "\"'", "//", "/*", "*/", 1, NULL, 0, 1, NULL, BASE_TOKENIZER_GENERIC, NULL
};

static const char *const pyExtensions[] = { "py", NULL };
static const char *const pyKeywords[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue",
    "def", "del", "elif", "else", "except", "False", "finally", "for",
    "from", "global", "if", "import", "in", "is", "lambda", "None",
    "nonlocal", "not", "or", "pass", "raise", "return", "True", "try",
    "while", "with", "yield",
    "int", "float", "str", "bool", "list", "dict", "set", "tuple", "bytes",
    NULL
};
/* Python has no block comments and triple-quoted strings are out of
 * scope for the generic tokenizer (its string handling is single-quote-
 * char, not a 3-char delimiter) -- single/double quotes still cover
 * the common case correctly, a triple-quoted string just renders as
 * three back-to-back single-char strings rather than one block, close
 * enough for a first pass and not worth a Python-specific tokenizer. */
static const struct syntaxLang pyLang = {
    pyExtensions, pyKeywords, "\"'", "#", NULL, NULL, 0, NULL, 0, 1, NULL, BASE_TOKENIZER_GENERIC, NULL
};

static const char *const shExtensions[] = { "sh", "bash", "zsh", NULL };
static const char *const shKeywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
    "done", "case", "esac", "function", "in", "select", "time",
    "return", "break", "continue", "local", "export", "readonly",
    "declare", "unset", "shift", "exit", "trap", "source", "eval",
    NULL
};
static const struct syntaxLang shLang = {
    shExtensions, shKeywords, "\"'", "#", NULL, NULL, 0, NULL, 0, 1, NULL, BASE_TOKENIZER_GENERIC, NULL
};

static const char *const jsExtensions[] = { "js", "jsx", "ts", "tsx", NULL };
static const char *const jsKeywords[] = {
    "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "export", "extends", "false",
    "finally", "for", "function", "if", "import", "in", "instanceof",
    "let", "new", "null", "return", "super", "switch", "this", "throw",
    "true", "try", "typeof", "undefined", "var", "void", "while", "with",
    "yield", "async", "await", "static", "get", "set",
    "interface", "type", "enum", "implements", "namespace", "as",
    "boolean", "number", "string", "any", "unknown", "never",
    NULL
};
static const struct syntaxLang jsLang = {
    jsExtensions, jsKeywords, "\"'`", "//", "/*", "*/", 0, NULL, 0, 1, NULL, BASE_TOKENIZER_GENERIC, NULL
};

static const struct syntaxLang *const syntaxLangTable[] = {
    &cLang, &cppLang, &pyLang, &shLang, &jsLang,
};
static const int32_t syntaxLangTableCount =
    (int32_t)(sizeof(syntaxLangTable) / sizeof(syntaxLangTable[0]));

/* User-defined languages loaded from ~/.tinyedit/syntax/ (.conf files) (see
 * syntaxLoadUserLangs() below) -- same struct syntaxLang shape as the
 * built-ins above, but heap-allocated (including every string/array
 * inside) since they're parsed at runtime rather than compiled in.
 * Loaded once, lazily, on the first call that needs a language lookup;
 * never freed (lives for the process's lifetime, same as the built-in
 * tables). */
static const struct syntaxLang **userLangTable = NULL;
static int32_t userLangTableCount = 0;
static uint8_t userLangsLoaded = 0;

static void syntaxLoadUserLangs(void);

/* File extensions are conventionally case-insensitive for language
 * detection, even on case-sensitive filesystems (notably .C/.H on Unix).
 * Compare ASCII letters without depending on the user's locale. */
static uint8_t syntaxExtensionEquals(const char *left, const char *right) {
    while (*left && *right) {
        unsigned char a = (unsigned char)*left;
        unsigned char b = (unsigned char)*right;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

/* Language keywords are ASCII spelling by definition.  Do not delegate
 * this decision to the process locale: libc's isalpha() may classify
 * non-ASCII bytes differently on different systems, while the tokenizer
 * must make the same choice on macOS, Linux and other POSIX targets. */
static uint8_t syntaxIsAsciiLetter(unsigned char byte) {
    return (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
        (byte >= (unsigned char)'a' && byte <= (unsigned char)'z');
}

static const struct syntaxLang *syntaxLangForFilename(const char *filename) {
    if (!filename) return NULL;
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return NULL;
    const char *ext = dot + 1;

    if (!userLangsLoaded) syntaxLoadUserLangs();

    /* User-defined languages take priority over built-ins: a
     * ~/.tinyedit/syntax/ (.conf files) for an extension tinyedit already
     * knows (e.g. redefining "c") is a deliberate override, not a
     * conflict to warn about -- same "last one wins" tolerance the
     * rest of the config-file handling in this project already has
     * (see settings.c's filetype.* overrides). */
    for (int32_t i = 0; i < userLangTableCount; i++) {
        const struct syntaxLang *lang = userLangTable[i];
        for (int32_t j = 0; lang->extensions[j]; j++)
            if (syntaxExtensionEquals(lang->extensions[j], ext)) return lang;
    }

    for (int32_t i = 0; i < syntaxLangTableCount; i++) {
        const struct syntaxLang *lang = syntaxLangTable[i];
        for (int32_t j = 0; lang->extensions[j]; j++)
            if (syntaxExtensionEquals(lang->extensions[j], ext)) return lang;
    }
    return NULL;
}

/* Shared by the two public per-extension queries below: finds the user
 * language claiming `ext`, loading the user table on first use exactly
 * like syntaxLangForFilename() does. Only the user table is searched --
 * both callers are about what an installed .conf provides, which is
 * precisely what the built-in table can't answer. */
static const struct syntaxLang *syntaxUserLangForExtension(const char *ext) {
    if (!ext || ext[0] == '\0') return NULL;
    if (!userLangsLoaded) syntaxLoadUserLangs();

    for (int32_t i = 0; i < userLangTableCount; i++) {
        const struct syntaxLang *lang = userLangTable[i];
        for (int32_t j = 0; lang->extensions[j]; j++)
            if (syntaxExtensionEquals(lang->extensions[j], ext)) return lang;
    }
    return NULL;
}

uint8_t syntaxUserLangHasExtension(const char *ext) {
    return syntaxUserLangForExtension(ext) != NULL;
}

const char *syntaxUserFiletypeForExtension(const char *ext) {
    const struct syntaxLang *lang = syntaxUserLangForExtension(ext);
    return lang ? lang->filetype : NULL;
}

/* ---- user-defined languages (~/.tinyedit/syntax/, .conf files) --------
 *
 * Same INI-style "key = value" format as ~/.tinyeditrc (see settings.c's
 * settingsLoad()) -- deliberately reused instead of a new format/parser,
 * per the project's "zero external dependencies" rule (a YAML parser
 * would be a new dependency to write or vendor just for this). One file
 * per language; the filename itself isn't meaningful (unlike
 * ~/.tinyeditrc's fixed name), only its "extensions" key says which
 * files it applies to. Comma-separated lists for the two list-valued
 * keys (extensions, keywords) since '=' already separates key from
 * value and a second delimiter class keeps the format flat -- no
 * quoting/escaping rules to get wrong.
 *
 * Expected keys (all optional except extensions/keywords, which make
 * an entry meaningless if absent):
 *   extensions          = m,mat        (comma-separated, no leading dot)
 *   keywords             = function,end,if,else,for,while,...
 *   quote_chars           = "'
 *   line_comment          = %
 *   block_comment_start   = %{
 *   block_comment_end     = %}
 *   hash_line_is_preprocessor = true
 *   keyword_prefix_chars  = \        (e.g. LaTeX: keywords spelled "\begin")
 *   math_mode             = true     (recognizes "$...$"/"$$...$$" as HL_MATH)
 *   highlight_function_calls = true  (identifier immediately followed by
 *                                     '(' is tagged HL_FUNCTION)
 *   filetype              = Nunjucks (status-bar language name for this
 *                                     language's extensions)
 */

static char *syntaxDupTrimmed(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char *out = malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* Splits a comma-separated value into a NULL-terminated array of
 * malloc'd strings (each trimmed). Returns NULL for an empty/blank
 * value -- callers treat a NULL extensions/keywords list as "this
 * language entry is unusable", since both are required for the
 * generic tokenizer to do anything. */
static char **syntaxSplitList(const char *value, int32_t *out_count) {
    int32_t count = 1;
    for (const char *p = value; *p; p++) if (*p == ',') count++;

    char **items = malloc(sizeof(char *) * (size_t)(count + 1));
    int32_t n = 0;
    const char *start = value;
    for (;;) {
        const char *comma = strchr(start, ',');
        size_t seglen = comma ? (size_t)(comma - start) : strlen(start);
        char *seg = malloc(seglen + 1);
        memcpy(seg, start, seglen);
        seg[seglen] = '\0';
        char *trimmed = syntaxDupTrimmed(seg);
        free(seg);
        if (trimmed[0] != '\0') items[n++] = trimmed;
        else free(trimmed);
        if (!comma) break;
        start = comma + 1;
    }
    items[n] = NULL;

    if (n == 0) {
        free(items);
        *out_count = 0;
        return NULL;
    }
    *out_count = n;
    return items;
}

/* Parses one ~/.tinyedit/syntax/<name>.conf file into a heap-allocated
 * struct syntaxLang, or NULL if it has no usable "extensions"/
 * "keywords" keys (see header comment above) -- a malformed or
 * incomplete user file is skipped silently rather than surfacing a
 * parse-error dialog, same tolerant handling ~/.tinyeditrc itself
 * already gets in settingsLoad(). */
static const struct syntaxLang *syntaxParseLangFile(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char **extensions = NULL;
    char **keywords = NULL;
    char *quote_chars = NULL;
    char *line_comment = NULL;
    char *block_comment_start = NULL;
    char *block_comment_end = NULL;
    uint8_t hash_line_is_preprocessor = 0;
    char *keyword_prefix_chars = NULL;
    uint8_t math_mode = 0;
    uint8_t highlight_function_calls = 0;
    char *filetype = NULL;
    uint8_t base_tokenizer = 0;
    char **template_delimiters = NULL;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* '#' starts a comment, but it's also a real character in some
         * values (a "{# #}" template delimiter). Honour a backslash
         * escape so those can be written, collapsing "\#" to "#" in
         * place; an unescaped '#' still ends the line as before. */
        for (char *p = line; *p; p++) {
            if (*p == '\\' && p[1] == '#') {
                memmove(p, p + 1, strlen(p));
                continue; /* p now points at the '#', keep it */
            }
            if (*p == '#') { *p = '\0'; break; }
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = syntaxDupTrimmed(line);
        char *value = syntaxDupTrimmed(eq + 1);

        int32_t unused_count;
        if (strcmp(key, "extensions") == 0) {
            extensions = syntaxSplitList(value, &unused_count);
        } else if (strcmp(key, "keywords") == 0) {
            keywords = syntaxSplitList(value, &unused_count);
        } else if (strcmp(key, "quote_chars") == 0) {
            free(quote_chars); quote_chars = syntaxDupTrimmed(value);
        } else if (strcmp(key, "line_comment") == 0) {
            free(line_comment); line_comment = syntaxDupTrimmed(value);
        } else if (strcmp(key, "block_comment_start") == 0) {
            free(block_comment_start); block_comment_start = syntaxDupTrimmed(value);
        } else if (strcmp(key, "block_comment_end") == 0) {
            free(block_comment_end); block_comment_end = syntaxDupTrimmed(value);
        } else if (strcmp(key, "hash_line_is_preprocessor") == 0) {
            hash_line_is_preprocessor = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "keyword_prefix_chars") == 0) {
            free(keyword_prefix_chars); keyword_prefix_chars = syntaxDupTrimmed(value);
        } else if (strcmp(key, "math_mode") == 0) {
            math_mode = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "highlight_function_calls") == 0) {
            highlight_function_calls = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "filetype") == 0) {
            free(filetype); filetype = syntaxDupTrimmed(value);
        } else if (strcmp(key, "base_tokenizer") == 0) {
            base_tokenizer = (strcmp(value, "xml") == 0 || strcmp(value, "html") == 0);
        } else if (strcmp(key, "template_delimiters") == 0) {
            template_delimiters = syntaxSplitList(value, &unused_count);
        }
        free(key);
        free(value);
    }
    fclose(fp);

    /* `keywords` is required only for the generic tokenizer, which has
     * nothing to highlight without it. A base_tokenizer = xml language
     * gets its highlighting from the markup tokenizer and its own
     * template_delimiters, so demanding a keyword list there would
     * force every such file to carry a meaningless one. */
    if (!extensions || (!keywords && !base_tokenizer)) {
        free(extensions); free(keywords); free(quote_chars);
        free(line_comment); free(block_comment_start); free(block_comment_end);
        free(keyword_prefix_chars);
        free(filetype); free(template_delimiters);
        return NULL;
    }

    /* "filetype =" with a blank value is the same as omitting the key:
     * an empty name would otherwise be reported as a valid label and
     * blank out the status bar field. */
    if (filetype && filetype[0] == '\0') {
        free(filetype);
        filetype = NULL;
    }

    struct syntaxLang *lang = malloc(sizeof(struct syntaxLang));
    lang->extensions = (const char *const *)extensions;
    lang->keywords = (const char *const *)keywords;
    lang->quote_chars = quote_chars;
    lang->line_comment = line_comment;
    /* A block comment needs BOTH delimiters -- one without the other
     * is a malformed entry, treated as "no block comments" rather than
     * guessing what the missing side should be. */
    if (block_comment_start && block_comment_end) {
        lang->block_comment_start = block_comment_start;
        lang->block_comment_end = block_comment_end;
    } else {
        free(block_comment_start);
        free(block_comment_end);
        lang->block_comment_start = NULL;
        lang->block_comment_end = NULL;
    }
    lang->hash_line_is_preprocessor = hash_line_is_preprocessor;
    lang->keyword_prefix_chars = keyword_prefix_chars;
    lang->math_mode = math_mode;
    lang->highlight_function_calls = highlight_function_calls;
    lang->filetype = filetype;
    lang->base_tokenizer = base_tokenizer ? BASE_TOKENIZER_XML : BASE_TOKENIZER_GENERIC;
    lang->template_delimiters = (const char *const *)template_delimiters;
    /* An xml-based language may legitimately omit `keywords` (see the
     * validity check above), but matchKeyword() walks this array
     * unconditionally. Give it an empty list rather than NULL so any
     * future path reaching the generic tokenizer can't dereference it. */
    if (!lang->keywords) {
        static const char *const noKeywords[] = { NULL };
        lang->keywords = noKeywords;
    }
    return lang;
}

static void syntaxLoadUserLangs(void) {
    userLangsLoaded = 1; /* set first: a failed/empty scan should not retry every keystroke */

    const char *home = getenv("HOME");
    if (!home || !*home) return;

    char dirpath[1024];
    int32_t dirlen = snprintf(dirpath, sizeof(dirpath), "%s/.tinyedit/syntax", home);
    if (dirlen < 0 || (size_t)dirlen >= sizeof(dirpath)) return;
    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        const char *name = entry->d_name;
        size_t namelen = strlen(name);
        if (namelen < 6 || strcmp(name + namelen - 5, ".conf") != 0) continue;

        char filepath[1280];
        int32_t pathlen = snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, name);
        if (pathlen < 0 || (size_t)pathlen >= sizeof(filepath)) continue;
        const struct syntaxLang *lang = syntaxParseLangFile(filepath);
        if (!lang) continue;

        userLangTable = realloc(userLangTable,
            sizeof(struct syntaxLang *) * (size_t)(userLangTableCount + 1));
        userLangTable[userLangTableCount++] = lang;
    }
    closedir(dir);
}

/* ---- generic C-like tokenizer --------------------------------------- */

/* Defined near the Markdown tokenizer below (shared with it); forward
 * declared here since the generic tokenizer's optional math_mode (see
 * struct syntaxLang) needs it too. */
static int32_t syntaxTryHighlightMath(erow *row, const char *s, int32_t len, int32_t i);

static uint8_t isWordBoundary(char c) {
    unsigned char byte = (unsigned char)c;
    return byte == '\0' || (byte < 0x80 && !isalnum(byte) && byte != '_');
}

static int32_t syntaxCharLenAt(const char *s, int32_t len, int32_t i) {
    if ((unsigned char)s[i] < 0x80) return 1;
    size_t clen = utf8NextCharLen(s, (size_t)i, (size_t)len);
    if (clen == 0 || clen > (size_t)(len - i)) return 1;
    return (int32_t)clen;
}

static uint8_t syntaxIsIdentifierByte(char c, const char *extra, uint8_t first) {
    unsigned char byte = (unsigned char)c;
    if (byte >= 0x80) return 1;
    if (c == '_' || (extra && strchr(extra, c))) return 1;
    if (first) return syntaxIsAsciiLetter(byte);
    return syntaxIsAsciiLetter(byte) || (byte >= (unsigned char)'0' && byte <= (unsigned char)'9');
}

static int32_t syntaxHighlightQuoted(erow *row, const char *s, int32_t len,
    int32_t start, uint8_t backslash_escapes) {
    char quote = s[start];
    int32_t i = start;
    row->hl[i++] = HL_STRING;
    while (i < len) {
        int32_t clen = syntaxCharLenAt(s, len, i);
        for (int32_t k = 0; k < clen; k++) row->hl[i + k] = HL_STRING;
        if (backslash_escapes && s[i] == '\\' && i + 1 < len) {
            i++;
            clen = syntaxCharLenAt(s, len, i);
            for (int32_t k = 0; k < clen; k++) row->hl[i + k] = HL_STRING;
            i += clen;
            continue;
        }
        if (s[i] == quote) return i + 1;
        i += clen;
    }
    return i;
}

static int32_t matchKeyword(const char *const *list, const char *s, int32_t avail) {
    for (int32_t i = 0; list[i]; i++) {
        int32_t klen = (int32_t)strlen(list[i]);
        if (klen > avail) continue;
        if (memcmp(s, list[i], (size_t)klen) != 0) continue;
        if (!isWordBoundary(s[klen])) continue;
        return klen;
    }
    return 0;
}

static void syntaxHighlightRowGeneric(erow *row, const struct syntaxLang *lang,
    uint8_t prev_open_comment, uint8_t prev_open_math) {
    row->hl = realloc(row->hl, (size_t)row->rsize);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    const char *s = row->render;
    int32_t len = row->rsize;
    int32_t line_comment_len = lang->line_comment ? (int32_t)strlen(lang->line_comment) : 0;
    int32_t block_start_len = lang->block_comment_start ? (int32_t)strlen(lang->block_comment_start) : 0;
    int32_t block_end_len = lang->block_comment_end ? (int32_t)strlen(lang->block_comment_end) : 0;

    uint8_t in_comment = lang->block_comment_start ? prev_open_comment : 0;
    /* "\[...\]" is the only math delimiter that can span multiple
     * lines (unlike "$"/"$$"/"\(...\)", which this tokenizer treats as
     * single-line, matching Markdown's math handling -- see
     * syntaxTryHighlightMath()'s doc comment). Carried in exactly like
     * in_comment above. */
    uint8_t in_math = lang->math_mode ? prev_open_math : 0;
    int32_t first_token = 0;
    while (first_token < len && (s[first_token] == ' ' || s[first_token] == '\t')) first_token++;
    int32_t i = 0;
    while (i < len) {
        if (in_math) {
            if (i + 1 < len && s[i] == '\\' && s[i + 1] == ']') {
                row->hl[i] = HL_MATH;
                row->hl[i + 1] = HL_MATH;
                i += 2;
                in_math = 0;
                continue;
            }
            row->hl[i] = HL_MATH;
            i++;
            continue;
        }

        if (in_comment) {
            if (i + block_end_len <= len && memcmp(&s[i], lang->block_comment_end, (size_t)block_end_len) == 0) {
                for (int32_t k = 0; k < block_end_len; k++) row->hl[i + k] = HL_COMMENT;
                i += block_end_len;
                in_comment = 0;
                continue;
            }
            row->hl[i] = HL_COMMENT;
            i++;
            continue;
        }

        if (line_comment_len && i + line_comment_len <= len &&
            memcmp(&s[i], lang->line_comment, (size_t)line_comment_len) == 0) {
            for (; i < len; i++) row->hl[i] = HL_COMMENT;
            break;
        }

        if (block_start_len && i + block_start_len <= len &&
            memcmp(&s[i], lang->block_comment_start, (size_t)block_start_len) == 0) {
            for (int32_t k = 0; k < block_start_len; k++) row->hl[i + k] = HL_COMMENT;
            i += block_start_len;
            in_comment = 1;
            continue;
        }

        if (lang->quote_chars && strchr(lang->quote_chars, s[i])) {
            i = syntaxHighlightQuoted(row, s, len, i, 1);
            continue;
        }

        if (lang->math_mode && s[i] == '\\' && i + 1 < len && s[i + 1] == '[') {
            /* "\[" with no matching "\]" before end of line -- opens a
             * multi-line math span rather than failing silently (which
             * is what syntaxTryHighlightMath() below would do, since
             * it's single-line only and would just fall through to
             * being treated as an unrecognized "\[..." keyword). */
            int32_t end = syntaxTryHighlightMath(row, s, len, i);
            if (end >= 0) { i = end; continue; }
            row->hl[i] = HL_MATH;
            row->hl[i + 1] = HL_MATH;
            i += 2;
            in_math = 1;
            continue;
        }

        if (lang->math_mode && (s[i] == '$' || s[i] == '\\')) {
            int32_t end = syntaxTryHighlightMath(row, s, len, i);
            if (end >= 0) { i = end; continue; }
        }

        if (lang->hash_line_is_preprocessor && i == first_token && s[i] == '#') {
            for (; i < len; i++) row->hl[i] = HL_PREPROCESSOR;
            break;
        }

        if (isdigit((unsigned char)s[i]) ||
            (s[i] == '.' && i + 1 < len && isdigit((unsigned char)s[i + 1]))) {
            uint8_t prev_boundary = i == 0 || isWordBoundary(s[i - 1]);
            if (prev_boundary) {
                int32_t start = i;
                while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '.' ||
                       ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                    i++;
                for (int32_t k = start; k < i; k++) row->hl[k] = HL_NUMBER;
                continue;
            }
        }

        uint8_t ascii_keyword_start = (unsigned char)s[i] < 0x80 &&
            syntaxIsIdentifierByte(s[i], lang->keyword_prefix_chars, 1);
        if (syntaxIsIdentifierByte(s[i], lang->keyword_prefix_chars, 1)) {
            int32_t avail = len - i;
            int32_t klen = ascii_keyword_start ? matchKeyword(lang->keywords, s + i, avail) : 0;
            if (klen) {
                for (int32_t k = 0; k < klen; k++) row->hl[i + k] = HL_KEYWORD;
                i += klen;
                continue;
            }
            /* Not a recognized keyword -- still consume the run of
             * identifier-ish characters (letters/underscore, plus any
             * keyword_prefix_chars byte) as one non-highlighted unit
             * rather than falling through to the catch-all i++ below,
             * so e.g. an unrecognized "\foo" LaTeX command doesn't get
             * its '\' and "foo" evaluated separately against unrelated
             * rules on the next iterations. */
            int32_t ident_start = i;
            i += syntaxCharLenAt(s, len, i);
            while (i < len && syntaxIsIdentifierByte(s[i], lang->keyword_prefix_chars, 0))
                i += syntaxCharLenAt(s, len, i);

            /* Function name heuristic: an identifier immediately
             * followed by '(' (skipping spaces/tabs) is tagged
             * HL_FUNCTION -- covers both call sites ("foo(x)") and
             * declarations ("void foo(int x)"), since this tokenizer
             * has no real parser to distinguish them. Only look ahead
             * within the row: a name whose '(' is on the next line
             * (rare, but legal C) is left unhighlighted rather than
             * peeking past row->render, which this tokenizer never
             * does elsewhere either. */
            if (lang->highlight_function_calls) {
                int32_t j = i;
                while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
                if (j < len && s[j] == '(') {
                    for (int32_t k = ident_start; k < i; k++) row->hl[k] = HL_FUNCTION;
                    continue;
                }
            }
            continue;
        }

        i++;
    }

    row->hl_open_comment = in_comment;
    row->hl_open_math = in_math;
}

/* ---- Markdown (dedicated tokenizer) ----------------------------------
 *
 * Not "keywords + strings + comments" like the C-like languages above:
 * highlights ATX headings (leading '#'s -> HL_PREPROCESSOR, reusing that
 * class for "structural marker" rather than adding a parallel one),
 * inline code spans `...` and fenced code blocks ```...``` (-> HL_STRING,
 * reusing "literal text" semantics), LaTeX math "$...$"/"$$...$$"/
 * "\(...\)"/"\[...\]" (its own HL_MATH class, so it's independently
 * colorable from code spans even though both start from the same
 * "literal/non-prose" idea), and
 * emphasis wrapped in single or double asterisks/underscores (->
 * HL_KEYWORD, reusing "stands out from body text").
 * No nested Markdown-in-code-fence-language highlighting -- a code
 * fence's contents render as HL_STRING regardless of the language tag
 * after the opening ```, matching the "no nested/embedded syntax" scope
 * boundary from the original C-only version. */
static uint8_t isMdExtension(const char *ext) {
    return syntaxExtensionEquals(ext, "md") || syntaxExtensionEquals(ext, "markdown");
}

/* Looks for `open` starting at s[i] and, if found, scans forward for
 * `close` (both fixed-width, unlike the "$"/"$$" case which shares a
 * character between widths and needs its own logic below). Returns the
 * index just past `close`'s end on success, -1 otherwise. Used for
 * LaTeX's "\[...\]" (display) and "\(...\)" (inline) math delimiters,
 * which -- unlike "$"/"$$" -- don't overload one marker character for
 * two different widths, so a plain fixed-delimiter scan is enough. */
static int32_t syntaxTryHighlightDelimited(erow *row, const char *s, int32_t len, int32_t i,
    const char *open, const char *close) {
    int32_t open_len = (int32_t)strlen(open);
    if (i + open_len > len || memcmp(&s[i], open, (size_t)open_len) != 0) return -1;

    int32_t close_len = (int32_t)strlen(close);
    int32_t j = i + open_len;
    while (j + close_len <= len) {
        if (memcmp(&s[j], close, (size_t)close_len) == 0) {
            int32_t end = j + close_len;
            for (int32_t k = i; k < end; k++) row->hl[k] = HL_MATH;
            return end;
        }
        j++;
    }
    return -1;
}

/* If `s[i]` starts a LaTeX math span -- inline "$...$"/"\(...\)" or
 * display "$$...$$"/"\[...\]" -- colors it HL_MATH in row->hl and
 * returns the index just past its end. Otherwise returns -1 and
 * touches nothing, leaving the caller to try its own rules for s[i].
 * Shared between the Markdown tokenizer and the generic C-like
 * tokenizer's optional math_mode (see struct syntaxLang) -- LaTeX math
 * syntax doesn't depend on which language it's embedded in, so both
 * can call the exact same matching logic instead of duplicating it. */
static int32_t syntaxTryHighlightMath(erow *row, const char *s, int32_t len, int32_t i) {
    if (s[i] == '\\') {
        int32_t end = syntaxTryHighlightDelimited(row, s, len, i, "\\[", "\\]");
        if (end >= 0) return end;
        return syntaxTryHighlightDelimited(row, s, len, i, "\\(", "\\)");
    }

    if (s[i] != '$') return -1;
    /* "$$" must close with "$$", not with a lone "$" that happens to
     * appear inside the formula (e.g. as a currency symbol some
     * formulas legitimately contain). Single-line only -- a "$$" that
     * spans multiple lines renders each line independently rather than
     * as one highlighted block (same scope limit as Markdown's fenced
     * code blocks/emphasis: no other multi-line tracking here). Same
     * single-line limit applies to "\[...\]"/"\(...\)" above. */
    int32_t start = i;
    int32_t width = (i + 1 < len && s[i + 1] == '$') ? 2 : 1;
    int32_t j = i + width;
    int32_t close_start = -1;
    while (j < len) {
        if (s[j] == '$') {
            if (width == 1 || (j + 1 < len && s[j + 1] == '$')) {
                close_start = j;
                break;
            }
        }
        j++;
    }
    if (close_start < 0) return -1;

    int32_t end = close_start + width;
    for (int32_t k = start; k < end; k++) row->hl[k] = HL_MATH;
    return end;
}

/* A "---" line, the YAML front matter delimiter. Trailing whitespace
 * is tolerated; anything else on the line means it isn't a delimiter. */
static uint8_t syntaxIsFrontMatterFence(const char *s, int32_t len) {
    if (len < 3 || s[0] != '-' || s[1] != '-' || s[2] != '-') return 0;
    for (int32_t i = 3; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

/* One line of YAML front matter: "key: value" with the key as a
 * keyword and the value as a string, so the metadata block reads as
 * structured data rather than prose. Comments ("# ...") and list items
 * ("- item") get the treatment they'd have in any other language.
 *
 * Deliberately shallow -- no nested mappings, block scalars, anchors or
 * multi-line values. Front matter in practice is a flat list of scalar
 * keys, and a real YAML parser is far outside what this file does. */
static void syntaxHighlightFrontMatterLine(erow *row, const char *s, int32_t len) {
    int32_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;

    if (i < len && s[i] == '#') {
        for (int32_t k = i; k < len; k++) row->hl[k] = HL_COMMENT;
        return;
    }

    /* "- " list item: the dash is a marker, the rest is a value. */
    if (i < len && s[i] == '-' && (i + 1 >= len || s[i + 1] == ' ')) {
        row->hl[i] = HL_PREPROCESSOR;
        i++;
        for (int32_t k = i; k < len; k++) row->hl[k] = HL_STRING;
        return;
    }

    int32_t key_start = i;
    while (i < len && s[i] != ':') i++;
    if (i >= len) {
        /* No colon: a bare continuation/scalar line, all value. */
        for (int32_t k = key_start; k < len; k++) row->hl[k] = HL_STRING;
        return;
    }

    for (int32_t k = key_start; k < i; k++) row->hl[k] = HL_KEYWORD;
    row->hl[i++] = HL_PREPROCESSOR; /* the colon itself */
    for (int32_t k = i; k < len; k++) row->hl[k] = HL_STRING;
}

/* An HTML tag embedded in Markdown, inline (<em>) or block (<div ...>).
 * Highlights the angle brackets and tag name as HL_KEYWORD, attribute
 * names likewise, and quoted attribute values as HL_STRING -- the same
 * classes the XML tokenizer uses, so a tag looks the same whether it
 * sits in an .html file or inside a document.
 *
 * Returns the index just past '>', or `i` unchanged when s[i] doesn't
 * begin a plausible tag. Being conservative matters here: prose is full
 * of '<' used as a less-than sign, and highlighting "a < b" as markup
 * would be worse than leaving real tags plain. So a tag must start with
 * a letter or '/' and actually reach a '>' on the same line. */
static int32_t syntaxTryHighlightMarkdownTag(erow *row, const char *s, int32_t len, int32_t i) {
    if (s[i] != '<') return i;

    int32_t j = i + 1;
    if (j < len && s[j] == '/') j++;
    if (j >= len || !syntaxIsAsciiLetter((unsigned char)s[j])) return i;

    /* Scan ahead for the closing '>', bailing out on anything that
     * says "this isn't a tag" (another '<' first, or end of line). */
    int32_t close = -1;
    uint8_t in_quote = 0;
    char quote = 0;
    for (int32_t k = j; k < len; k++) {
        if (in_quote) {
            if (s[k] == quote) in_quote = 0;
            continue;
        }
        if (s[k] == '"' || s[k] == '\'') { in_quote = 1; quote = s[k]; continue; }
        if (s[k] == '<') return i;
        if (s[k] == '>') { close = k; break; }
    }
    if (close < 0) return i;

    row->hl[i] = HL_KEYWORD;
    int32_t k = i + 1;
    if (s[k] == '/') row->hl[k++] = HL_KEYWORD;
    while (k < close && syntaxIsIdentifierByte(s[k], ":.-", 0)) row->hl[k++] = HL_KEYWORD;

    while (k < close) {
        if (s[k] == '"' || s[k] == '\'') {
            char q = s[k];
            row->hl[k++] = HL_STRING;
            while (k < close && s[k] != q) row->hl[k++] = HL_STRING;
            if (k < close) row->hl[k++] = HL_STRING;
            continue;
        }
        if (syntaxIsIdentifierByte(s[k], ":.-", 1)) {
            while (k < close && syntaxIsIdentifierByte(s[k], ":.-", 0))
                row->hl[k++] = HL_KEYWORD;
            continue;
        }
        k++;
    }

    row->hl[close] = HL_KEYWORD;
    return close + 1;
}

/* A Markdown link "[text](url)" or image "![alt](url)": brackets,
 * parentheses and the leading '!' as HL_PREPROCESSOR markers, the
 * label as HL_KEYWORD, the destination as HL_STRING. Keeping the two
 * apart is what makes a row of badges readable -- the long URL stops
 * competing with the label for attention.
 *
 * Handles the nested image-inside-link form "[![alt](img)](url)" that
 * badge rows use: the label scan tracks bracket depth, so the inner
 * "![alt](img)" is found as a unit and highlighted by recursing into
 * it rather than terminating the outer label at the first ']'.
 *
 * Returns the index just past the closing ')', or `i` unchanged when
 * this isn't a well-formed link -- an unmatched "[link]" or a stray
 * "(text)" in prose is left as plain text, same conservative stance as
 * the HTML tag scanner. Reference-style links ("[text][ref]") and bare
 * autolinks are out of scope for this first pass. */
static int32_t syntaxTryHighlightMarkdownLink(erow *row, const char *s, int32_t len, int32_t i) {
    int32_t start = i;
    int32_t bracket = i;
    if (s[bracket] == '!') bracket++;
    if (bracket >= len || s[bracket] != '[') return start;

    /* Find the ']' matching this '[', allowing nested bracket pairs. */
    int32_t depth = 0;
    int32_t label_end = -1;
    for (int32_t k = bracket; k < len; k++) {
        if (s[k] == '[') depth++;
        else if (s[k] == ']') {
            depth--;
            if (depth == 0) { label_end = k; break; }
        }
    }
    if (label_end < 0) return start;
    if (label_end + 1 >= len || s[label_end + 1] != '(') return start;

    /* Matching ')', allowing nested parens (they appear in real URLs,
     * e.g. Wikipedia article titles). */
    depth = 0;
    int32_t url_end = -1;
    for (int32_t k = label_end + 1; k < len; k++) {
        if (s[k] == '(') depth++;
        else if (s[k] == ')') {
            depth--;
            if (depth == 0) { url_end = k; break; }
        }
    }
    if (url_end < 0) return start;

    if (s[start] == '!') row->hl[start] = HL_PREPROCESSOR;
    row->hl[bracket] = HL_PREPROCESSOR;

    /* Label: recurse so a nested image inside a link keeps its own
     * colors, and plain label text becomes HL_KEYWORD. */
    int32_t k = bracket + 1;
    while (k < label_end) {
        int32_t nested = syntaxTryHighlightMarkdownLink(row, s, label_end, k);
        if (nested != k) { k = nested; continue; }
        row->hl[k] = HL_KEYWORD;
        k++;
    }

    row->hl[label_end] = HL_PREPROCESSOR;
    row->hl[label_end + 1] = HL_PREPROCESSOR;
    for (int32_t u = label_end + 2; u < url_end; u++) row->hl[u] = HL_STRING;
    row->hl[url_end] = HL_PREPROCESSOR;
    return url_end + 1;
}

static void syntaxHighlightRowMarkdown(erow *row, uint8_t prev_in_fence, uint8_t prev_in_math,
    uint8_t prev_in_frontmatter, int32_t row_index, uint8_t prev_in_emphasis) {
    row->hl = realloc(row->hl, (size_t)row->rsize);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    const char *s = row->render;
    int32_t len = row->rsize;

    /* YAML front matter, before every other rule: while inside it the
     * document isn't Markdown at all. Recognized only when the opening
     * "---" is row 0 -- further down, "---" is a horizontal rule. */
    if (row_index == 0 && syntaxIsFrontMatterFence(s, len)) {
        for (int32_t k = 0; k < len; k++) row->hl[k] = HL_PREPROCESSOR;
        row->hl_open_comment = 0;
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 1;
        return;
    }
    if (prev_in_frontmatter) {
        row->hl_open_comment = 0;
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;
        if (syntaxIsFrontMatterFence(s, len)) {
            for (int32_t k = 0; k < len; k++) row->hl[k] = HL_PREPROCESSOR;
            row->hl_open_frontmatter = 0; /* closing delimiter */
            return;
        }
        syntaxHighlightFrontMatterLine(row, s, len);
        row->hl_open_frontmatter = 1;
        return;
    }

    uint8_t is_fence_line = len >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`';
    if (is_fence_line) {
        for (int32_t k = 0; k < len; k++) row->hl[k] = HL_STRING;
        row->hl_open_comment = !prev_in_fence;
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;
        return;
    }
    if (prev_in_fence) {
        for (int32_t k = 0; k < len; k++) row->hl[k] = HL_STRING;
        row->hl_open_comment = 1;
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;
        return;
    }

    /* Multi-line "\[...\]" display math -- same "\]" search as the
     * generic tokenizer's in_math handling, kept separate here since
     * Markdown's fence state above already owns hl_open_comment for
     * this tokenizer and math needs its own bit (hl_open_math). */
    if (prev_in_math) {
        int32_t close_at = -1;
        for (int32_t k = 0; k + 1 < len; k++) {
            if (s[k] == '\\' && s[k + 1] == ']') { close_at = k; break; }
        }
        if (close_at >= 0) {
            for (int32_t k = 0; k < close_at + 2; k++) row->hl[k] = HL_MATH;
            row->hl_open_comment = 0;
            row->hl_open_math = 0;
            row->hl_open_frontmatter = 0;
            /* Falls through to scan the rest of the line normally
             * (text after "\]" is prose again), unlike the fence case
             * above which always consumes the whole line -- math only
             * consumes up through its closing delimiter. */
            int32_t i = close_at + 2;
            while (i < len) {
                if (s[i] == '`') {
                    int32_t start = i++;
                    while (i < len && s[i] != '`') i++;
                    if (i < len) i++;
                    for (int32_t k = start; k < i; k++) row->hl[k] = HL_STRING;
                    continue;
                }
                if (s[i] == '$' || s[i] == '\\') {
                    int32_t end = syntaxTryHighlightMath(row, s, len, i);
                    if (end >= 0) { i = end; continue; }
                }
                i++;
            }
            return;
        }
        for (int32_t k = 0; k < len; k++) row->hl[k] = HL_MATH;
        row->hl_open_comment = 0;
        row->hl_open_math = 1;
        row->hl_open_frontmatter = 0;
        return;
    }

    /* Continuation of an emphasis span opened on an earlier row. A
     * blank line ends it (see hl_open_emphasis in tinyedit.h): the
     * `len <= 0` case is handled by the empty-row path in
     * syntaxHighlightRow(), so reaching here with prev_in_emphasis set
     * means this row has text. */
    if (prev_in_emphasis) {
        char marker_cls = (prev_in_emphasis == 1) ? (char)HL_KEYWORD : (char)HL_EMPHASIS_STRONG;
        int32_t run_needed = (prev_in_emphasis == 1) ? 1 : 2;

        int32_t close_start = -1;
        for (int32_t k = 0; k < len; k++) {
            if (s[k] != '*' && s[k] != '_') continue;
            int32_t crun = 0;
            while (k + crun < len && s[k + crun] == s[k]) crun++;
            if (crun >= run_needed) { close_start = k + run_needed; break; }
            k += crun - 1;
        }

        row->hl_open_comment = 0;
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;

        if (close_start < 0) {
            for (int32_t k = 0; k < len; k++) row->hl[k] = (uint8_t)marker_cls;
            row->hl_open_emphasis = prev_in_emphasis;
            return;
        }
        for (int32_t k = 0; k < close_start; k++) row->hl[k] = (uint8_t)marker_cls;
        row->hl_open_emphasis = 0;
        /* Text after the closer is ordinary prose; leaving it
         * HL_NORMAL is correct, and re-scanning it for further
         * constructs isn't worth a second pass here. */
        return;
    }

    if (len > 0 && s[0] == '#') {
        int32_t k = 0;
        while (k < len && s[k] == '#') k++;
        if (k < len && s[k] == ' ') {
            for (int32_t i = 0; i < len; i++) row->hl[i] = HL_PREPROCESSOR;
            row->hl_open_comment = 0;
            row->hl_open_math = 0;
            row->hl_open_frontmatter = 0;
            return;
        }
    }

    int32_t i = 0;
    while (i < len) {
        if (s[i] == '\\' && i + 1 < len && s[i + 1] == '[') {
            int32_t end = syntaxTryHighlightMath(row, s, len, i);
            if (end >= 0) { i = end; continue; }
            /* "\[" with no "\]" before end of line -- opens multi-line
             * math, same as the generic tokenizer's handling. */
            row->hl[i] = HL_MATH;
            row->hl[i + 1] = HL_MATH;
            i += 2;
            while (i < len) row->hl[i++] = HL_MATH;
            row->hl_open_comment = 0;
            row->hl_open_math = 1;
            row->hl_open_frontmatter = 0;
            return;
        }
        if (s[i] == '`') {
            int32_t start = i++;
            while (i < len && s[i] != '`') i++;
            if (i < len) i++; /* consume closing backtick */
            for (int32_t k = start; k < i; k++) row->hl[k] = HL_STRING;
            continue;
        }
        /* After the code-span rule above, so a tag written inside
         * backticks (`<div>`, common when documenting HTML) stays a
         * code span rather than being highlighted as real markup. */
        if (s[i] == '<') {
            int32_t end = syntaxTryHighlightMarkdownTag(row, s, len, i);
            if (end != i) { i = end; continue; }
        }
        /* Before the emphasis rule below: URLs are full of '_' and '*'
         * ("some_file", "a*b"), which would otherwise open an emphasis
         * span that swallows the rest of the line. */
        if (s[i] == '[' || s[i] == '!') {
            int32_t end = syntaxTryHighlightMarkdownLink(row, s, len, i);
            if (end != i) { i = end; continue; }
        }
        if (s[i] == '$' || s[i] == '\\') {
            int32_t end = syntaxTryHighlightMath(row, s, len, i);
            if (end >= 0) { i = end; continue; }
        }
        if ((s[i] == '*' || s[i] == '_') &&
            !(i > 0 && isalnum((unsigned char)s[i - 1]))) {
            char marker = s[i];
            int32_t run = 1;
            while (i + run < len && s[i + run] == marker) run++;
            /* Find the matching closing run of the same length on this
             * line; if there is none, the span continues onto the next
             * row (see hl_open_emphasis in tinyedit.h) and is closed
             * either by that marker later or by a blank line. */
            int32_t j = i + run;
            int32_t close_start = -1;
            while (j < len) {
                if (s[j] == marker) {
                    int32_t crun = 0;
                    while (j + crun < len && s[j + crun] == marker) crun++;
                    if (crun >= run) { close_start = j; break; }
                    j += crun;
                } else j++;
            }
            if (close_start >= 0) {
                /* run == 1: a single asterisk or underscore marker,
                 * i.e. italic -- mapped to HL_KEYWORD. run >= 2: a
                 * doubled marker, i.e. bold -- its own
                 * HL_EMPHASIS_STRONG class so the two are
                 * independently colorable. */
                enum syntaxHighlight cls = (run == 1) ? HL_KEYWORD : HL_EMPHASIS_STRONG;
                for (int32_t k = i; k < close_start + run; k++) row->hl[k] = (uint8_t)cls;
                i = close_start + run;
                continue;
            }

            /* No closer on this line: highlight to end of line and
             * carry the span into the next row. Only for a marker that
             * looks like an opener -- one followed by something other
             * than whitespace -- so a trailing "*" or a lone "5 * 3"
             * asterisk doesn't start a span. */
            if (i + run < len && s[i + run] != ' ' && s[i + run] != '\t') {
                enum syntaxHighlight cls = (run == 1) ? HL_KEYWORD : HL_EMPHASIS_STRONG;
                for (int32_t k = i; k < len; k++) row->hl[k] = (uint8_t)cls;
                row->hl_open_comment = 0;
                row->hl_open_math = 0;
                row->hl_open_frontmatter = 0;
                row->hl_open_emphasis = (run == 1) ? 1 : 2;
                return;
            }
        }
        i++;
    }

    row->hl_open_comment = 0;
    row->hl_open_math = 0;
    row->hl_open_frontmatter = 0;
    row->hl_open_emphasis = 0;
}

/* ---- XML/HTML (dedicated tokenizer) -----------------------------------
 *
 * Tags/attributes, not statements -- also not a fit for the C-like
 * table. Highlights "<", "/", tag name and ">" as HL_KEYWORD (a tag is
 * the structural equivalent of a keyword here), attribute values in
 * quotes as HL_STRING, and comments <!-- ... --> as HL_COMMENT
 * (multi-line, using the same hl_open_comment carry-over as C's block
 * comments). No DOCTYPE/CDATA/entity special-casing -- first pass
 * covers the common case (tags, attributes, comments), same "narrow
 * first version" scope as the rest of this file. */
static uint8_t isXmlExtension(const char *ext) {
    return syntaxExtensionEquals(ext, "html") || syntaxExtensionEquals(ext, "htm") ||
        syntaxExtensionEquals(ext, "xml");
}

/* Template-engine extensions embedded in markup: Nunjucks/Jinja/Liquid
 * style "{{ expr }}", "{% tag %}" and "{# comment #}". Only consulted
 * for extensions that are templates rather than plain markup (see
 * isTemplateMarkupExtension()), so a literal "{{" in an ordinary .html
 * file keeps rendering as text.
 *
 * Highlighted as a whole block -- delimiters as HL_PREPROCESSOR (the
 * "structural marker" role it already plays for C's "#" lines and
 * Markdown's headings) and the contents as HL_FUNCTION -- rather than
 * tokenized into keywords/strings/numbers. Two reasons: the expression
 * inside is a different language from the surrounding markup, and the
 * point of highlighting a template is seeing at a glance which parts
 * are dynamic, which a solid block conveys better than picking out
 * "for" and "in" within it. A comment block is HL_COMMENT throughout,
 * matching every other comment in the editor.
 *
 * Returns the index just past the closing delimiter, or `i` unchanged
 * when s[i] doesn't start a template block. Unterminated blocks run to
 * end of line: these are single-line constructs here, since carrying a
 * third multi-line state through the XML tokenizer isn't worth it for
 * a construct that is nearly always closed on the same line. */
static int32_t syntaxTryHighlightTemplateBlock(erow *row, const char *s, int32_t len,
    int32_t i, const char *const *delimiters) {
    if (!delimiters) return i;

    for (int32_t d = 0; delimiters[d]; d++) {
        const char *pair = delimiters[d];
        const char *sep = strchr(pair, ' ');
        if (!sep) continue; /* malformed pair, no "open close" split */

        int32_t open_len = (int32_t)(sep - pair);
        const char *close = sep + 1;
        int32_t close_len = (int32_t)strlen(close);
        if (open_len <= 0 || close_len <= 0) continue;
        if (i + open_len > len || memcmp(s + i, pair, (size_t)open_len) != 0) continue;

        /* A "{#"-style opener is the language's comment form -- give it
         * the editor's comment color rather than the expression one,
         * so it reads like every other comment. */
        uint8_t is_comment = (open_len >= 2 && pair[1] == '#');
        enum syntaxHighlight body = is_comment ? HL_COMMENT : HL_FUNCTION;
        enum syntaxHighlight marker = is_comment ? HL_COMMENT : HL_PREPROCESSOR;

        for (int32_t k = 0; k < open_len; k++) row->hl[i + k] = (uint8_t)marker;
        int32_t j = i + open_len;
        while (j < len) {
            if (j + close_len <= len && memcmp(s + j, close, (size_t)close_len) == 0) {
                for (int32_t k = 0; k < close_len; k++) row->hl[j + k] = (uint8_t)marker;
                return j + close_len;
            }
            int32_t clen = syntaxCharLenAt(s, len, j);
            for (int32_t k = 0; k < clen && j + k < len; k++) row->hl[j + k] = (uint8_t)body;
            j += clen;
        }
        return j; /* unterminated: runs to end of line */
    }
    return i;
}

static uint8_t syntaxHighlightRowXml(erow *row, uint8_t prev_open_comment,
    const char *const *template_delimiters) {
    row->hl = realloc(row->hl, (size_t)row->rsize);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    const char *s = row->render;
    int32_t len = row->rsize;
    uint8_t in_comment = prev_open_comment;
    uint8_t in_tag = 0;

    int32_t i = 0;
    while (i < len) {
        if (in_comment) {
            if (i + 2 < len && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
                row->hl[i] = HL_COMMENT; row->hl[i + 1] = HL_COMMENT; row->hl[i + 2] = HL_COMMENT;
                i += 3;
                in_comment = 0;
                continue;
            }
            row->hl[i] = HL_COMMENT;
            i++;
            continue;
        }

        /* Before the tag/attribute/quote rules below, and deliberately
         * regardless of in_tag: a template block is just as likely
         * inside a tag (href="{{ url }}") as in text content, and in
         * the attribute case it must win over the quoted-string rule
         * so the expression doesn't read as a plain string. */
        {
            int32_t after = syntaxTryHighlightTemplateBlock(row, s, len, i, template_delimiters);
            if (after != i) { i = after; continue; }
        }

        if (i + 3 < len && s[i] == '<' && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
            row->hl[i] = HL_COMMENT; row->hl[i + 1] = HL_COMMENT; row->hl[i + 2] = HL_COMMENT; row->hl[i + 3] = HL_COMMENT;
            i += 4;
            in_comment = 1;
            continue;
        }

        if (in_tag && (s[i] == '"' || s[i] == '\'')) {
            /* XML/HTML quote characters are escaped with entities such
             * as &quot;, not with a C-style backslash.
             *
             * Scanned here rather than via syntaxHighlightQuoted() so a
             * template block inside the value (href="{{ url }}") stays
             * highlighted as an expression: that helper consumes to the
             * closing quote in one go, which would paint the whole
             * attribute as a plain string and hide the dynamic part --
             * exactly the thing a template author needs to see. */
            char quote = s[i];
            row->hl[i++] = HL_STRING;
            while (i < len) {
                int32_t after = syntaxTryHighlightTemplateBlock(row, s, len, i,
                    template_delimiters);
                if (after != i) { i = after; continue; }
                if (s[i] == quote) { row->hl[i++] = HL_STRING; break; }
                int32_t clen = syntaxCharLenAt(s, len, i);
                for (int32_t k = 0; k < clen && i + k < len; k++) row->hl[i + k] = HL_STRING;
                i += clen;
            }
            continue;
        }

        if (s[i] == '<') {
            row->hl[i] = HL_KEYWORD;
            i++;
            in_tag = 1;
            if (i < len && (s[i] == '/' || s[i] == '?' || s[i] == '!'))
                row->hl[i++] = HL_KEYWORD;
            while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
            int32_t name_start = i;
            while (i < len && (syntaxIsIdentifierByte(s[i], ":.-", i == name_start)))
                i += syntaxCharLenAt(s, len, i);
            for (int32_t k = name_start; k < i; k++) row->hl[k] = HL_KEYWORD;
            continue;
        }

        if (in_tag && (s[i] == '>' || (s[i] == '/' && i + 1 < len && s[i + 1] == '>'))) {
            row->hl[i++] = HL_KEYWORD;
            if (i < len && s[i - 1] == '/' && s[i] == '>') row->hl[i++] = HL_KEYWORD;
            in_tag = 0;
            continue;
        }

        if (in_tag && syntaxIsIdentifierByte(s[i], ":.-", 1)) {
            int32_t attr_start = i;
            while (i < len && syntaxIsIdentifierByte(s[i], ":.-", i == attr_start))
                i += syntaxCharLenAt(s, len, i);
            for (int32_t k = attr_start; k < i; k++) row->hl[k] = HL_KEYWORD;
            continue;
        }

        i += syntaxCharLenAt(s, len, i);
    }

    row->hl_open_comment = in_comment;
    return in_comment;
}

/* ---- CSS (dedicated tokenizer) -----------------------------------------
 *
 * Selectors/properties, not statements -- "#fff" hex colors and
 * ".class"/"#id" selectors would misparse under the C-like table's
 * number/preprocessor rules, so CSS gets its own pass rather than
 * joining syntaxLangTable. Highlights slash-star block comments (CSS
 * has no line comments), string values, and property names (word
 * immediately followed by ':') as HL_KEYWORD. */
static uint8_t isCssExtension(const char *ext) {
    return syntaxExtensionEquals(ext, "css");
}

static uint8_t syntaxHighlightRowCss(erow *row, uint8_t prev_open_comment) {
    row->hl = realloc(row->hl, (size_t)row->rsize);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    const char *s = row->render;
    int32_t len = row->rsize;
    uint8_t in_comment = prev_open_comment;

    int32_t i = 0;
    while (i < len) {
        if (in_comment) {
            if (i + 1 < len && s[i] == '*' && s[i + 1] == '/') {
                row->hl[i] = HL_COMMENT; row->hl[i + 1] = HL_COMMENT;
                i += 2;
                in_comment = 0;
                continue;
            }
            row->hl[i] = HL_COMMENT;
            i++;
            continue;
        }

        if (i + 1 < len && s[i] == '/' && s[i + 1] == '*') {
            row->hl[i] = HL_COMMENT; row->hl[i + 1] = HL_COMMENT;
            i += 2;
            in_comment = 1;
            continue;
        }

        if (s[i] == '"' || s[i] == '\'') {
            i = syntaxHighlightQuoted(row, s, len, i, 1);
            continue;
        }

        if (syntaxIsIdentifierByte(s[i], "-", 1)) {
            int32_t start = i;
            while (i < len && syntaxIsIdentifierByte(s[i], "-", i == start))
                i += syntaxCharLenAt(s, len, i);
            int32_t j = i;
            while (j < len && s[j] == ' ') j++;
            if (j < len && s[j] == ':') {
                for (int32_t k = start; k < i; k++) row->hl[k] = HL_KEYWORD;
            }
            continue;
        }

        i += syntaxCharLenAt(s, len, i);
    }

    row->hl_open_comment = in_comment;
    return in_comment;
}

/* ---- dispatch -------------------------------------------------------- */

void syntaxHighlightRow(erow *row, const char *filename,
    uint8_t syntax_highlight_enabled, uint8_t prev_open_comment, uint8_t prev_open_math,
    uint8_t prev_open_frontmatter, int32_t row_index, uint8_t prev_open_emphasis) {
    if (!syntax_highlight_enabled || !filename || row->rsize <= 0) {
        free(row->hl);
        row->hl = NULL;
        row->hl_open_comment = 0;
        row->hl_open_math = 0;
        /* An empty line inside front matter must not end the block:
         * the state carries straight through rather than resetting,
         * unlike the two flags above which no-op cleanly on a blank
         * row. */
        row->hl_open_frontmatter = prev_open_frontmatter;
        /* A blank line ends an emphasis span (unlike front matter,
         * which survives one) -- see hl_open_emphasis in tinyedit.h. */
        row->hl_open_emphasis = 0;
        return;
    }

    const char *dot = strrchr(filename, '.');
    const char *ext = (dot && dot != filename) ? dot + 1 : NULL;

    /* Resolved before the dedicated tokenizers below so a user .conf
     * can claim an extension they'd otherwise take (and so a language
     * declaring base_tokenizer = xml reaches the markup tokenizer with
     * its own delimiters). Matches the "user languages win" precedence
     * syntaxLangForFilename() already applies over the built-in table. */
    const struct syntaxLang *lang = syntaxLangForFilename(filename);

    if (lang && lang->base_tokenizer == BASE_TOKENIZER_XML) {
        syntaxHighlightRowXml(row, prev_open_comment, lang->template_delimiters);
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;
        row->hl_open_emphasis = 0;
        return;
    }

    if (ext && isMdExtension(ext)) {
        syntaxHighlightRowMarkdown(row, prev_open_comment, prev_open_math,
            prev_open_frontmatter, row_index, prev_open_emphasis);
        return;
    }
    if (ext && isXmlExtension(ext)) {
        syntaxHighlightRowXml(row, prev_open_comment, NULL);
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;
        row->hl_open_emphasis = 0;
        return;
    }
    if (ext && isCssExtension(ext)) {
        syntaxHighlightRowCss(row, prev_open_comment);
        row->hl_open_math = 0;
        row->hl_open_frontmatter = 0;
        row->hl_open_emphasis = 0;
        return;
    }

    if (!lang) {
        free(row->hl);
        row->hl = NULL;
        row->hl_open_comment = 0;
        row->hl_open_math = 0;
        return;
    }

    syntaxHighlightRowGeneric(row, lang, prev_open_comment, prev_open_math);
}

const char *syntaxColorFor(enum syntaxHighlight hl, const struct editorSettings *s) {
    switch (hl) {
        /* HL_NORMAL defaults to COLOR_TERMINAL_DEFAULT, which returns
         * NULL here rather than an escape sequence -- editorDrawRowSegment()
         * treats a NULL syntaxColorFor() result as "don't touch the
         * color", so with the default setting unclassified text is
         * exactly as before this setting existed (terminal's own
         * foreground, no escape emitted at all). Set
         * color_syntax_normal to any of the 24 real hues to recolor it
         * explicitly. */
        case HL_NORMAL:
            if (s->color_syntax_normal == COLOR_TERMINAL_DEFAULT) return NULL;
            return ansiColorCode(s->color_syntax_normal);
        case HL_COMMENT:      return ansiColorCode(s->color_syntax_comment);
        case HL_KEYWORD:      return ansiColorCode(s->color_syntax_keyword);
        case HL_STRING:       return ansiColorCode(s->color_syntax_string);
        case HL_NUMBER:       return ansiColorCode(s->color_syntax_number);
        case HL_PREPROCESSOR: return ansiColorCode(s->color_syntax_preprocessor);
        case HL_EMPHASIS_STRONG: return ansiColorCode(s->color_syntax_emphasis_strong);
        case HL_MATH:          return ansiColorCode(s->color_syntax_math);
        case HL_FUNCTION:      return ansiColorCode(s->color_syntax_function);
        default:              return NULL;
    }
}

uint8_t syntaxHasBuiltinExtension(const char *ext) {
    if (!ext || ext[0] == '\0') return 0;

    /* The three dedicated tokenizers key off the extension directly
     * rather than appearing in syntaxLangTable, so they have to be
     * asked separately -- same order as the dispatch in
     * syntaxHighlightRow(). */
    if (isMdExtension(ext) || isXmlExtension(ext) || isCssExtension(ext)) return 1;

    for (int32_t i = 0; i < syntaxLangTableCount; i++) {
        const struct syntaxLang *lang = syntaxLangTable[i];
        for (int32_t j = 0; lang->extensions[j]; j++)
            if (syntaxExtensionEquals(lang->extensions[j], ext)) return 1;
    }
    return 0;
}
