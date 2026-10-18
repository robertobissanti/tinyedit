/* syntax.c -- see syntax.h */

#include "syntax.h"
#include "alloc.h"
#include "fileio.h"
#include "markdown.h"
#include "resources.h"
#include "utf8.h"

#include <dirent.h>
#include <errno.h>
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

static const char *const jsonExtensions[] = { "json", NULL };
static const char *const jsonKeywords[] = { "true", "false", "null", NULL };
static const struct syntaxLang jsonLang = {
    jsonExtensions, jsonKeywords, "\"", NULL, NULL, NULL, 0, NULL, 0, 0, NULL, BASE_TOKENIZER_GENERIC, NULL
};

static const struct syntaxLang *const syntaxLangTable[] = {
    &cLang, &cppLang, &pyLang, &shLang, &jsLang, &jsonLang,
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

/**
 * @brief Compare two extension strings using ASCII case folding.
 *
 * @details left and right are NUL-terminated without leading dots.
 * @return 1 only when the entire extensions match, independent of locale.
 */
uint8_t syntaxExtensionEquals(const char *left, const char *right) {
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

/**
 * @brief Recognize letters in language keywords without locale-dependent results.
 *
 * @details byte is an unsigned input byte.
 * @return 1 only for ASCII A-Z or a-z.
 *
 * @note Language keywords are ASCII spelling by definition. Do not delegate
 * this decision to the process locale: libc's isalpha() may classify non-ASCII
 * bytes differently on different systems, while the tokenizer must make the
 * same choice on macOS, Linux and other POSIX targets.
 */
uint8_t syntaxIsAsciiLetter(unsigned char byte) {
    return (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
        (byte >= (unsigned char)'a' && byte <= (unsigned char)'z');
}

/**
 * @brief Find a table-driven language for a filename.
 *
 * @details Loads user definitions on first use and prefers them to built-in
 * table entries.
 * @return a borrowed language or NULL; dedicated tokenizers are selected
 * elsewhere.
 */
static const struct syntaxLang *syntaxLangForFilename(const char *filename) {
    if (!filename) return NULL;
    const char *basename = strrchr(filename, '/');
    basename = basename ? basename + 1 : filename;
    const char *dot = strrchr(basename, '.');
    if (!dot || !dot[1]) return NULL;
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

/**
 * @brief Find the installed user language claiming an extension.
 *
 * @details ext has no leading dot. Loads user definitions lazily; returns a
 * module-owned language or NULL.
 *
 * @note Shared by the two public per-extension queries below: finds the user
 * language claiming `ext`, loading the user table on first use exactly like
 * syntaxLangForFilename() does. Only the user table is searched -- both
 * callers are about what an installed .conf provides, which is precisely what
 * the built-in table can't answer.
 */
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

/**
 * @brief Check whether an installed user syntax definition claims an extension.
 *
 * @details ext has no leading dot.
 * @return 1 when a user language is found; compiled-in languages are not
 * considered.
 */
uint8_t syntaxUserLangHasExtension(const char *ext) {
    return syntaxUserLangForExtension(ext) != NULL;
}

/**
 * @brief Get the label declared by a user syntax definition.
 *
 * @return a borrowed filetype string or NULL when the definition or its label
 * is absent; do not free it.
 */
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

/**
 * @brief Copy a configuration token without surrounding whitespace.
 *
 * @details s is NUL-terminated.
 * @return an owned NUL-terminated allocation to free.
 */
static char *syntaxDupTrimmed(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char *out = teMalloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/**
 * @brief Split a comma-separated configuration value into trimmed items.
 *
 * @details Writes out_count and returns an owned NULL-terminated array, or
 * NULL for an empty list. Each string and the array must be freed when
 * discarded.
 *
 * @note Splits a comma-separated value into a NULL-terminated array of
 * malloc'd strings (each trimmed). Returns NULL for an empty/blank value --
 * callers treat a NULL extensions/keywords list as "this language entry is
 * unusable", since both are required for the generic tokenizer to do anything.
 */
static char **syntaxSplitList(const char *value, int32_t *out_count) {
    int32_t count = 1;
    for (const char *p = value; *p; p++) {
        if (*p != ',') continue;
        if (count == INT32_MAX) {
            teAllocationFailure("syntax list count exceeds limit");
        }
        count++;
    }

    char **items = teMalloc(teArrayBytes((size_t)count + 1, sizeof(*items)));
    int32_t n = 0;
    const char *start = value;
    for (;;) {
        const char *comma = strchr(start, ',');
        size_t seglen = comma ? (size_t)(comma - start) : strlen(start);
        char *seg = teMalloc(seglen + 1);
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

/**
 * @brief Parse one user syntax file into a language definition.
 *
 * @details path identifies a readable configuration.
 * @return an owned definition for registration, or NULL when unreadable or
 * unusable; generic definitions require keywords, XML-based definitions do
 * not.
 */
static const struct syntaxLang *syntaxParseLangFile(const char *path) {
    FILE *fp = fileioOpenRead(path);
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

    struct syntaxLang *lang = teMalloc(sizeof(*lang));
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

/**
 * @brief Append usable .conf definitions from a resource directory.
 *
 * @details Definitions remain module-owned for the process lifetime.
 */
static void syntaxLoadDirectory(const char *dirpath) {
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

        if (userLangTableCount == INT32_MAX) {
            teAllocationFailure("syntax language count exceeds limit");
        }
        userLangTable = teRealloc(userLangTable,
            teArrayBytes((size_t)userLangTableCount + 1, sizeof(*userLangTable)));
        userLangTable[userLangTableCount++] = lang;
    }
    closedir(dir);
}

/** @brief Load personal definitions first, then shared defaults, once. */
static void syntaxLoadUserLangs(void) {
    userLangsLoaded = 1;
    const char *home = getenv("HOME");
    if (home && *home) {
        char dirpath[1024];
        int32_t n = snprintf(dirpath, sizeof(dirpath), "%s/.tinyedit/syntax", home);
        if (n >= 0 && (size_t)n < sizeof(dirpath)) syntaxLoadDirectory(dirpath);
    }
    syntaxLoadDirectory(TE_DATADIR "/syntax");
}

/* ---- generic C-like tokenizer --------------------------------------- */

/**
 * @brief Decide whether a byte can end an ASCII keyword.
 *
 * @return 1 for NUL or ASCII non-identifier punctuation; non-ASCII bytes
 * remain part of an identifier rather than splitting a UTF-8 word.
 */
static uint8_t isWordBoundary(char c) {
    unsigned char byte = (unsigned char)c;
    return byte == '\0' || (byte < 0x80 && !isalnum(byte) && byte != '_');
}

/**
 * @brief Get a safe byte step for tokenizer text at a character boundary.
 *
 * @details s contains len bytes and i is within that span.
 * @return a grapheme length, using one byte for ASCII or malformed input.
 */
static int32_t syntaxCharLenAt(const char *s, int32_t len, int32_t i) {
    if ((unsigned char)s[i] < 0x80) return 1;
    size_t clen = utf8NextCharLen(s, (size_t)i, (size_t)len);
    if (clen == 0 || clen > (size_t)(len - i)) return 1;
    return (int32_t)clen;
}

/**
 * @brief Classify a byte as an identifier opener or continuation.
 *
 * @details first selects the opener rule; extra may be NULL or list additional
 * prefix characters.
 * @return 1 for eligible bytes, including non-ASCII bytes whose full span is
 * stepped separately.
 */
uint8_t syntaxIsIdentifierByte(char c, const char *extra, uint8_t first) {
    unsigned char byte = (unsigned char)c;
    if (byte >= 0x80) return 1;
    if (c == '_' || (extra && strchr(extra, c))) return 1;
    if (first) return syntaxIsAsciiLetter(byte);
    return syntaxIsAsciiLetter(byte) || (byte >= (unsigned char)'0' && byte <= (unsigned char)'9');
}

/**
 * @brief Color a quoted string and find where tokenization should resume.
 *
 * @details row->hl must be allocated for s, which contains len bytes; start
 * points to the quote. backslash_escapes enables escaped characters.
 * @return the exclusive end byte offset, including an unterminated span to row
 * end.
 */
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

/**
 * @brief Match a keyword at the current tokenizer position.
 *
 * @details list is NULL-terminated, s has avail bytes plus an accessible
 * terminator.
 * @return the matched byte length, or zero; the following byte must be a word
 * boundary.
 */
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

/* Highlighting is optional: an OOM discards this cache, not source text. */
/**
 * @brief Ensure row-owned highlight capacity for render bytes.
 */
uint8_t syntaxReserveHighlight(erow *row) {
    uint8_t *highlight = teTryRealloc(row->hl, (size_t)row->rsize);
    if (!highlight) {
        free(row->hl);
        row->hl = NULL;
        row->hl_heading = 0;
        row->hl_open_comment = row->hl_open_math = 0;
        row->hl_open_frontmatter = row->hl_open_emphasis = 0;
        return 0;
    }
    row->hl = highlight;
    return 1;
}

/**
 * @brief Color a row using a table-driven language definition.
 *
 * @details row->render must be current. Rebuilds its highlight array and
 * writes outgoing comment and math state from the supplied previous-row flags.
 */
static void syntaxHighlightRowGeneric(erow *row, const struct syntaxLang *lang,
    uint8_t prev_open_comment, uint8_t prev_open_math, uint8_t json_keys) {
    if (!syntaxReserveHighlight(row)) return;
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
            int32_t start = i;
            i = syntaxHighlightQuoted(row, s, len, i, 1);
            if (json_keys && s[start] == '"') {
                int32_t next = i;
                while (next < len && (s[next] == ' ' || s[next] == '\t' || s[next] == '\r')) next++;
                if (next < len && s[next] == ':')
                    memset(row->hl + start, HL_JSON_KEY, (size_t)(i - start));
            }
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
            /* A directive owns ordinary text to its right, but not a
             * trailing comment: leave its opener for the comment branch
             * above, which also preserves multi-line comment state. */
            for (; i < len; i++) {
                uint8_t starts_line_comment = line_comment_len &&
                    i + line_comment_len <= len &&
                    memcmp(&s[i], lang->line_comment, (size_t)line_comment_len) == 0;
                uint8_t starts_block_comment = block_start_len &&
                    i + block_start_len <= len &&
                    memcmp(&s[i], lang->block_comment_start, (size_t)block_start_len) == 0;
                if (starts_line_comment || starts_block_comment) break;
                row->hl[i] = HL_PREPROCESSOR;
            }
            if (i < len) continue;
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

/**
 * @brief Color a complete fixed-delimiter math span on one row.
 *
 * @details row->hl must be ready and i is a valid byte offset in s.
 * @return the exclusive end byte offset, or -1 when the opener or closer is
 * missing.
 *
 * @note Looks for `open` starting at s[i] and, if found, scans forward for
 * `close` (both fixed-width, unlike the "$"/"$$" case which shares a character
 * between widths and needs its own logic below). Returns the index just past
 * `close`'s end on success, -1 otherwise. Used for LaTeX's "\[...\]" (display)
 * and "\(...\)" (inline) math delimiters, which -- unlike "$"/"$$" -- don't
 * overload one marker character for two different widths, so a plain fixed-
 * delimiter scan is enough.
 */
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

/**
 * @brief Try a complete inline or display math span at the current byte.
 *
 * @details row->hl must be ready and s contains len bytes.
 * @return the exclusive end offset, or -1 without changing highlights;
 * multiline state belongs to the enclosing tokenizer.
 *
 * @note If `s[i]` starts a LaTeX math span -- inline "$...$"/"\(...\)" or
 * display "$$...$$"/"\[...\]" -- colors it HL_MATH in row->hl and returns the
 * index just past its end. Otherwise returns -1 and touches nothing, leaving
 * the caller to try its own rules for s[i]. Shared between the Markdown
 * tokenizer and the generic C-like tokenizer's optional math_mode (see struct
 * syntaxLang) -- LaTeX math syntax doesn't depend on which language it's
 * embedded in, so both can call the exact same matching logic instead of
 * duplicating it.
 */
int32_t syntaxTryHighlightMath(erow *row, const char *s, int32_t len, int32_t i) {
    if (s[i] == '\\') {
        int32_t end = syntaxTryHighlightDelimited(row, s, len, i, "\\[", "\\]");
        if (end >= 0) return end;
        return syntaxTryHighlightDelimited(row, s, len, i, "\\(", "\\)");
    }

    if (s[i] != '$') return -1;
    /* Inline matching stays on this row. Markdown handles standalone
     * dollar delimiters separately, with a state for each delimiter width. */
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
/**
 * @brief Recognize extensions handled by the markup tokenizer.
 *
 * @details ext is NUL-terminated without a dot.
 * @return 1 for html, htm or xml, ignoring ASCII case.
 */
static uint8_t isXmlExtension(const char *ext) {
    return syntaxExtensionEquals(ext, "html") || syntaxExtensionEquals(ext, "htm") ||
        syntaxExtensionEquals(ext, "xml");
}

/**
 * @brief Color an embedded template expression or comment at a byte position.
 *
 * @details delimiters is NULL or a NULL-terminated list of 'open close' pairs.
 * row->hl must be ready.
 * @return the exclusive end offset, or i unchanged when no opener matches;
 * unterminated blocks reach row end.
 */
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

/**
 * @brief Color markup, attributes, comments and optional template spans.
 *
 * @details row->render must be current; template_delimiters may be NULL.
 * Updates row->hl and hl_open_comment; returns whether the row ends inside a
 * comment.
 */
static uint8_t syntaxHighlightRowXml(erow *row, uint8_t prev_open_comment,
    const char *const *template_delimiters) {
    if (!syntaxReserveHighlight(row)) return 0;
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
/**
 * @brief Recognize the extension handled by the CSS tokenizer.
 *
 * @details ext is NUL-terminated without a dot.
 * @return 1 for css, ignoring ASCII case.
 */
static uint8_t isCssExtension(const char *ext) {
    return syntaxExtensionEquals(ext, "css");
}

/**
 * @brief Color CSS tokens and carry block-comment state to the next row.
 *
 * @details row->render must be current. Rebuilds row->hl and updates
 * hl_open_comment; returns the outgoing comment flag.
 */
static uint8_t syntaxHighlightRowCss(erow *row, uint8_t prev_open_comment) {
    if (!syntaxReserveHighlight(row)) return 0;
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

static uint8_t syntaxCodeNameEquals(const char *name, int32_t length, const char *candidate) {
    if (!candidate || strlen(candidate) != (size_t)length) return 0;
    for (int32_t at = 0; at < length;) {
        size_t step = utf8NextCharLen(name, (size_t)at, (size_t)length);
        if (step == 1) {
            uint8_t a = (uint8_t)name[at], b = (uint8_t)candidate[at];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) return 0;
        } else if (memcmp(name + at, candidate + at, step)) return 0;
        at += (int32_t)step;
    }
    return 1;
}

uint8_t syntaxHighlightCodeRow(erow *row, const char *language, int32_t length,
    uint8_t comment, uint8_t math) {
    if (length <= 0) return 0;
    const char *alias = NULL;
    if (syntaxCodeNameEquals(language, length, "python")) alias = "py";
    else if (syntaxCodeNameEquals(language, length, "javascript")) alias = "js";
    else if (syntaxCodeNameEquals(language, length, "typescript")) alias = "ts";
    else if (syntaxCodeNameEquals(language, length, "bash") || syntaxCodeNameEquals(language, length, "shell")) alias = "sh";
    else if (syntaxCodeNameEquals(language, length, "c++")) alias = "cpp";
    if (alias) { language = alias; length = (int32_t)strlen(alias); }
    if (!userLangsLoaded) syntaxLoadUserLangs();
    const struct syntaxLang *lang = NULL;
    for (int32_t table = 0; table < 2 && !lang; table++) {
        int32_t count = table == 0 ? userLangTableCount : syntaxLangTableCount;
        for (int32_t i = 0; i < count && !lang; i++) {
            const struct syntaxLang *entry = table == 0 ? userLangTable[i] : syntaxLangTable[i];
            if (syntaxCodeNameEquals(language, length, entry->filetype)) lang = entry;
            for (int32_t j = 0; !lang && entry->extensions[j]; j++)
                if (syntaxCodeNameEquals(language, length, entry->extensions[j])) lang = entry;
        }
    }
    if (lang && lang->base_tokenizer == BASE_TOKENIZER_XML) {
        syntaxHighlightRowXml(row, comment, lang->template_delimiters);
        row->hl_open_math = 0;
    } else if (lang)
        syntaxHighlightRowGeneric(row, lang, comment, math, syntaxCodeNameEquals(language, length, "json"));
    else if (syntaxCodeNameEquals(language, length, "html") || syntaxCodeNameEquals(language, length, "xml") ||
        syntaxCodeNameEquals(language, length, "htm") || syntaxCodeNameEquals(language, length, "svg")) {
        syntaxHighlightRowXml(row, comment, NULL);
        row->hl_open_math = 0;
    } else if (syntaxCodeNameEquals(language, length, "css")) {
        syntaxHighlightRowCss(row, comment);
        row->hl_open_math = 0;
    }
    else return 0;
    return 1;
}

/**
 * @brief Rebuild a row's syntax colors using its filename and incoming states.
 *
 * @details Call in row order with current render text. Updates row-owned
 * highlights and outgoing comment, math, front matter and emphasis states;
 * propagate changed states to following rows. Disabled or unsupported
 * highlighting clears the highlight array.
 * @param row Row owning current render text and the highlight array to
 * rebuild.
 * @param filename Filename used for language detection; may be NULL.
 * @param syntax_highlight_enabled Whether syntax coloring is enabled.
 * @param prev_open_comment Previous row's outgoing comment or Markdown fence
 * state.
 * @param prev_open_math Previous row's outgoing multiline math state.
 * @param prev_open_frontmatter Previous row's outgoing YAML front matter
 * state.
 * @param row_index Zero-based logical row number.
 * @param prev_open_emphasis Previous row's encoded emphasis state; this is not
 * just a boolean.
 */
void syntaxHighlightRow(erow *row, const char *filename,
    uint8_t syntax_highlight_enabled, uint8_t prev_open_comment, uint8_t prev_open_math,
    uint8_t prev_open_frontmatter, int32_t row_index, uint8_t prev_open_emphasis) {
    row->hl_heading = 0;
    row->markdown_code = (struct markdownCodeState){0};
    row->markdown_count = 0;
    row->seg_wrapcols = -1;
    if (!syntax_highlight_enabled || !filename || row->rsize <= 0) {
        free(row->hl);
        row->hl = NULL;
        row->hl_open_comment = syntax_highlight_enabled && filename && row->rsize <= 0
            ? prev_open_comment : 0;
        row->hl_open_math = syntax_highlight_enabled && filename && row->rsize <= 0
            ? prev_open_math : 0;
        /* An empty line inside front matter must not end the block:
         * the state carries straight through rather than resetting,
         * as do fenced code and multiline math. */
        row->hl_open_frontmatter = prev_open_frontmatter;
        /* A blank line ends an emphasis span (unlike front matter,
         * which survives one) -- see hl_open_emphasis in tinyedit.h. */
        row->hl_open_emphasis = 0;
        const char *extension = filename ? strrchr(filename, '.') : NULL;
        if (syntax_highlight_enabled && extension && syntaxIsMarkdownExtension(extension + 1) &&
            row->rsize <= 0 && row->owner && row_index > 0 && row_index <= row->owner->row_count)
            row->markdown_code = row->owner->rows[row_index - 1].markdown_code;
        return;
    }

    const char *basename = strrchr(filename, '/');
    basename = basename ? basename + 1 : filename;
    const char *dot = strrchr(basename, '.');
    const char *ext = (dot && dot[1]) ? dot + 1 : NULL;

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

    if (ext && syntaxIsMarkdownExtension(ext)) {
        markdownHighlightRow(row, prev_open_comment, prev_open_math,
            prev_open_frontmatter, row_index, prev_open_emphasis);
        markdownHighlightAnchors(row);
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

    syntaxHighlightRowGeneric(row, lang, prev_open_comment, prev_open_math,
        ext && syntaxExtensionEquals(ext, "json"));
}

/**
 * @brief Get the configured foreground sequence for a syntax class.
 *
 * @details s supplies live or draft settings.
 * @return a borrowed ANSI literal, or NULL when no color should be emitted,
 * including terminal-default normal text.
 */
const char *syntaxColorFor(enum syntaxHighlight hl, const struct editorSettings *s) {
    if (hl >= HL_HEADING_1 && hl <= HL_HEADING_6) {
        int32_t level = (int32_t)hl - HL_HEADING_1;
        int32_t ansi = s->color_markdown_heading[level];
        int32_t rgb = s->rgb_color_markdown_heading[level];
        if (ansi == COLOR_TERMINAL_DEFAULT) ansi = s->color_syntax_preprocessor;
        if (rgb == RGB_TERMINAL_DEFAULT) rgb = s->rgb_color_syntax_preprocessor;
        return ansiColorCode(settingsResolveColor(s, ansi, rgb));
    }
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
            if (settingsColor(s, color_syntax_normal) == COLOR_TERMINAL_DEFAULT) return NULL;
            return ansiColorCode(settingsColor(s, color_syntax_normal));
        case HL_COMMENT:      return ansiColorCode(settingsColor(s, color_syntax_comment));
        case HL_EMPHASIS:     return ansiColorCode(settingsColor(s, color_syntax_italic));
        case HL_KEYWORD:      return ansiColorCode(settingsColor(s, color_syntax_keyword));
        case HL_JSON_KEY:     return ansiColorCode(settingsColor(s, color_syntax_json_key));
        case HL_LINK_LABEL:
        case HL_LINK_DESTINATION:
        case HL_LINK_MARKER:  return ansiColorCode(settingsColor(s, color_syntax_json_key));
        case HL_STRING:       return ansiColorCode(settingsColor(s, color_syntax_string));
        case HL_NUMBER:       return ansiColorCode(settingsColor(s, color_syntax_number));
        case HL_PREPROCESSOR: return ansiColorCode(settingsColor(s, color_syntax_preprocessor));
        case HL_EMPHASIS_STRONG: return ansiColorCode(settingsColor(s, color_syntax_emphasis_strong));
        case HL_MATH:          return ansiColorCode(settingsColor(s, color_syntax_math));
        case HL_FUNCTION:      return ansiColorCode(settingsColor(s, color_syntax_function));
        default:              return NULL;
    }
}

/**
 * @brief Check whether compiled-in highlighting supports an extension.
 *
 * @details ext is NUL-terminated without a dot.
 * @return 1 for a built-in table language or dedicated tokenizer, ignoring
 * ASCII case; user definitions are excluded.
 */
uint8_t syntaxHasBuiltinExtension(const char *ext) {
    if (!ext || ext[0] == '\0') return 0;

    /* The three dedicated tokenizers key off the extension directly
     * rather than appearing in syntaxLangTable, so they have to be
     * asked separately -- same order as the dispatch in
     * syntaxHighlightRow(). */
    if (syntaxIsMarkdownExtension(ext) || isXmlExtension(ext) || isCssExtension(ext)) return 1;

    for (int32_t i = 0; i < syntaxLangTableCount; i++) {
        const struct syntaxLang *lang = syntaxLangTable[i];
        for (int32_t j = 0; lang->extensions[j]; j++)
            if (syntaxExtensionEquals(lang->extensions[j], ext)) return 1;
    }
    return 0;
}

uint8_t syntaxIsMarkdownExtension(const char *ext) {
    return markdownIsExtension(ext);
}
