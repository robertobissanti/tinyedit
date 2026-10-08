/* syntax.h -- syntax highlighting: keywords, strings, comments, numbers,
 * tokenized per row and mapped to colors from the same ANSI palette
 * (enum settingColor, see settings.h) used everywhere else in the
 * editor.
 *
 * One generic tokenizer (syntaxHighlightRowGeneric() in syntax.c)
 * drives every "C-like" language (C, C++, Python, Shell, JS, ...): the
 * shape is the same across all of them (keyword/type list, quote
 * characters, line-comment marker, block-comment delimiters, optional
 * '#'-prefixed line highlighted as a whole e.g. preprocessor/shebang)
 * -- only the table of names/delimiters changes per language. See
 * struct syntaxLang below. Languages with a genuinely different shape
 * (Markdown: headings/emphasis/links, not keywords; XML/HTML: tags/
 * attributes, not statements) get their own dedicated tokenizer
 * function instead of being forced into this table -- see
 * syntaxHighlightRowMarkdown()/syntaxHighlightRowXml().
 *
 * Dispatch: syntaxHighlightRow() looks up the language table entry (or
 * dedicated tokenizer) for the supplied filename's extension and calls the right
 * one; callers in tinyedit.c don't need to know which case applies.
 *
 * Recomputed per row on edit (editorUpdateRow() calls
 * syntaxHighlightRow() once per row it already touches), not for the
 * whole buffer on every redraw -- see IDEAS.md's note on large-file
 * cost for why that matters.
 *
 * Multi-line constructs (block comments spanning several lines) need
 * one bit of state carried from the previous row into the next -- see
 * erow.hl_open_comment in tinyedit.h.
 */

#ifndef __TE_SYNTAX_H
#define __TE_SYNTAX_H

#include "settings.h"
#include "tinyedit.h"

#include <stddef.h>
#include <stdint.h>

/* One value per byte of row->render (same length as row->rsize),
 * telling editorDrawRowSegment() which color to use for that byte span.
 * HL_NORMAL means "no highlight, use the default text color" --
 * deliberately value 0 so a freshly calloc'd hl array (a row with
 * highlighting not yet computed, or a language with nothing to
 * highlight) reads as "nothing highlighted" without an explicit fill.
 * Shared across every language/tokenizer: XML reuses HL_KEYWORD for
 * tag markers rather than inventing a parallel class, since it maps to
 * the same settingColor slot. HL_EMPHASIS_STRONG exists only for
 * Markdown bold text (double asterisk/underscore marker; single-marker
 * italic uses HL_EMPHASIS instead) -- kept as its own class rather than
 * reused from another language's slot because bold and italic are
 * visually distinct concepts a user would reasonably want different
 * colors for, unlike (say) Markdown headings reusing HL_PREPROCESSOR's
 * "structural marker" meaning. HL_MATH (Markdown LaTeX math,
 * "$...$"/"$$...$$") is the same case as HL_EMPHASIS_STRONG: initially
 * shared with HL_STRING (same "literal/non-prose" reasoning as code
 * spans), split into its own class on explicit request so math and
 * inline code can be told apart at a glance in documents mixing both.
 * HL_FUNCTION (C-like languages only, see syntaxHighlightRowGeneric()
 * in syntax.c) marks an identifier immediately followed by '(' --
 * covers both call sites and declarations, since the tokenizer has no
 * real parser to tell them apart; kept as its own class since a
 * function name is visually a different kind of thing from a
 * keyword. */
enum syntaxHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_KEYWORD,
    HL_STRING,
    HL_NUMBER,
    HL_PREPROCESSOR,
    HL_EMPHASIS_STRONG,
    HL_MATH,
    HL_FUNCTION,
    HL_EMPHASIS,
    HL_JSON_KEY
};

struct syntaxLang {
    const char *const *extensions;
    const char *const *keywords;
    const char *quote_chars;
    const char *line_comment;
    const char *block_comment_start;
    const char *block_comment_end;
    uint8_t hash_line_is_preprocessor;
    const char *keyword_prefix_chars;
    uint8_t math_mode;
    /* An identifier immediately followed by '(' (skipping spaces/tabs)
     * is tagged HL_FUNCTION instead of left unhighlighted -- covers
     * both call sites and declarations, see syntaxHighlightRowGeneric()
     * in syntax.c. Off by default for user-defined ~/.tinyedit/syntax/
     * languages (syntaxParseLangFile() only sets it from an explicit
     * "highlight_function_calls = true" key) since a language where
     * "name(" doesn't mean a function -- e.g. a math/DSL notation using
     * parens for something else -- would get misleading highlighting
     * otherwise; the built-in C-like languages all opt in explicitly. */
    uint8_t highlight_function_calls;
    /* Human-readable language name for the status bar ("Nunjucks"),
     * from the optional "filetype = <Name>" key. Lives here rather
     * than only in ~/.tinyeditrc's filetype.* overrides so a shipped
     * .conf is self-contained: one file carries both how to highlight
     * a language and what to call it. NULL when the key is absent --
     * the language still highlights, it just contributes no name. */
    const char *filetype;
    /* Which tokenizer handles this language. The generic C-like one
     * (BASE_TOKENIZER_GENERIC) is the default and what every keyword/
     * quote/comment field above describes. BASE_TOKENIZER_XML instead
     * routes the language through the markup tokenizer -- for template
     * formats that are HTML with an expression language embedded
     * (Nunjucks, Jinja, Liquid, Twig), where the surrounding markup
     * needs real tag/attribute highlighting the generic tokenizer
     * can't provide. With the XML base, the keyword/comment fields are
     * unused and `template_delimiters` takes over. */
    enum {
        BASE_TOKENIZER_GENERIC = 0,
        BASE_TOKENIZER_XML
    } base_tokenizer;
    /* Delimiter pairs marking embedded template expressions, as a
     * NULL-terminated array of "open close" strings ("{{ }}", "{% %}",
     * "{# %}"...). Parsed from the `template_delimiters` key. A pair
     * whose opener is "{#" is treated as a comment (highlighted
     * HL_COMMENT); the rest highlight as an expression block. NULL
     * when the language declares none. */
    const char *const *template_delimiters;
};

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
    uint8_t prev_open_frontmatter, int32_t row_index, uint8_t prev_open_emphasis);

/* ext is a NUL-terminated extension without a dot, compared ignoring case. */
/**
 * @brief Recognize extensions handled by the Markdown tokenizer.
 *
 * @details ext is NUL-terminated without a dot.
 * @return 1 for md or markdown, ignoring ASCII case.
 */
uint8_t syntaxIsMarkdownExtension(const char *ext);

/**
 * @brief Get the configured foreground sequence for a syntax class.
 *
 * @details s supplies live or draft settings.
 * @return a borrowed ANSI literal, or NULL when no color should be emitted,
 * including terminal-default normal text.
 */
const char *syntaxColorFor(enum syntaxHighlight hl, const struct editorSettings *s);

/**
 * @brief Check whether an installed user syntax definition claims an extension.
 *
 * @details ext has no leading dot.
 * @return 1 when a user language is found; compiled-in languages are not
 * considered.
 */
uint8_t syntaxUserLangHasExtension(const char *ext);

/**
 * @brief Check whether compiled-in highlighting supports an extension.
 *
 * @details ext is NUL-terminated without a dot.
 * @return 1 for a built-in table language or dedicated tokenizer, ignoring
 * ASCII case; user definitions are excluded.
 */
uint8_t syntaxHasBuiltinExtension(const char *ext);

/**
 * @brief Get the label declared by a user syntax definition.
 *
 * @return a borrowed filetype string or NULL when the definition or its label
 * is absent; do not free it.
 */
const char *syntaxUserFiletypeForExtension(const char *ext);

#endif /* __TE_SYNTAX_H */
