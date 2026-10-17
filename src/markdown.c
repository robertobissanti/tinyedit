#include "markdown.h"

#include "alloc.h"
#include "links.h"
#include "syntax.h"
#include "tinyedit.h"
#include "utf8.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

static uint8_t preview_enabled;
static const erow *preview_row;
static int32_t preview_cursor;
static const erow *preview_edit_row;
static int32_t preview_edit_start = -1;

/* ---- Markdown presentation ---- */

int32_t markdownSourceRenderByte(const erow *row, int32_t cx, int32_t tab_stop) {
    int32_t at = 0, byte = 0, rx = 0;
    while (at < cx && at < row->size) {
        size_t len = utf8NextCharLen(row->chars, (size_t)at, (size_t)row->size);
        uint8_t tab = row->chars[at] == '\t';
        int32_t width = tab ? tab_stop - rx % tab_stop :
            utf8SingleCharWidth(row->chars + at, len);
        byte += tab ? width : (int32_t)len;
        rx += width;
        at += (int32_t)len;
    }
    return byte;
}

void markdownSetPreview(uint8_t enabled, const erow *active_row,
    int32_t cursor, int32_t tab_stop) {
    preview_enabled = enabled;
    preview_row = active_row;
    preview_cursor = enabled && active_row ? markdownSourceRenderByte(active_row, cursor, tab_stop) : -1;
    int32_t edit_start = -1;
    if (enabled && active_row) {
        for (int32_t i = 0; i < active_row->markdown_count; i++) {
            const struct markdownSpan *span = &active_row->markdown_spans[i];
            if (!span->link || preview_cursor < span->start || preview_cursor > span->end) continue;
            if (preview_cursor == span->start || preview_cursor == span->end ||
                (preview_edit_row == active_row && preview_edit_start == span->start)) {
                edit_start = span->start;
                break;
            }
        }
    }
    preview_edit_row = enabled ? active_row : NULL;
    preview_edit_start = edit_start;
}

void markdownKeepLinkEditing(void) {
    if (!preview_enabled || !preview_row) return;
    for (int32_t i = 0; i < preview_row->markdown_count; i++) {
        const struct markdownSpan *span = &preview_row->markdown_spans[i];
        if (span->link && preview_cursor >= span->start && preview_cursor <= span->end) {
            preview_edit_row = preview_row;
            preview_edit_start = span->start;
            return;
        }
    }
}

const struct markdownSpan *markdownSpanAt(const erow *row, int32_t byte) {
    int32_t lo = 0, hi = row->markdown_count;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (row->markdown_spans[mid].start <= byte) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || byte >= row->markdown_spans[lo - 1].end) return NULL;
    return &row->markdown_spans[lo - 1];
}

uint8_t markdownByteHidden(const erow *row, int32_t byte) {
    if (!preview_enabled) return 0;
    const struct markdownSpan *span = markdownSpanAt(row, byte);
    if (!span || (row == preview_row && preview_cursor >= span->start &&
        preview_cursor <= span->end && (!span->link || preview_edit_start == span->start))) return 0;
    return byte < span->content_start || byte >= span->content_end;
}

uint8_t markdownByteUnderlined(const erow *row, int32_t byte) {
    if (!preview_enabled) return 0;
    const struct markdownSpan *span = markdownSpanAt(row, byte);
    return span && span->link && byte >= span->content_start && byte < span->content_end;
}

uint8_t markdownPreviewEnabled(void) { return preview_enabled; }
int32_t markdownPreviewCursor(const erow *row) { return row == preview_row ? preview_cursor : -1; }
int32_t markdownPreviewEditStart(const erow *row) {
    return row == preview_edit_row ? preview_edit_start : -1;
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
/**
 * @brief Recognize extensions handled by the Markdown tokenizer.
 *
 * @details ext is NUL-terminated without a dot.
 * @return 1 for md or markdown, ignoring ASCII case.
 */
uint8_t markdownIsExtension(const char *ext) {
    return syntaxExtensionEquals(ext, "md") || syntaxExtensionEquals(ext, "markdown");
}

/**
 * @brief Recognize the YAML front matter boundary used by Markdown.
 *
 * @details s contains len bytes.
 * @return 1 for exactly three hyphens followed only by spaces or tabs;
 * deciding whether it opens front matter depends on row position.
 */
static uint8_t syntaxIsFrontMatterFence(const char *s, int32_t len) {
    if (len < 3 || s[0] != '-' || s[1] != '-' || s[2] != '-') return 0;
    for (int32_t i = 3; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t') return 0;
    return 1;
}

/**
 * @brief Color a shallow YAML metadata line inside front matter.
 *
 * @details row->hl must already be allocated for len bytes. Highlights simple
 * keys, values, comments and list markers; does not parse full YAML or update
 * block state.
 *
 * @note One line of YAML front matter: "key: value" with the key as a keyword
 * and the value as a string, so the metadata block reads as structured data
 * rather than prose. Comments ("# ...") and list items ("- item") get the
 * treatment they'd have in any other language.
 *
 * Deliberately shallow -- no nested mappings, block scalars, anchors or multi-
 * line values. Front matter in practice is a flat list of scalar keys, and a
 * real YAML parser is far outside what this file does.
 */
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

/**
 * @brief Try to color an HTML tag embedded in Markdown prose.
 *
 * @details row->hl must be ready and i is a valid byte offset in s.
 * @return the exclusive end offset on a recognized same-line tag, or i unchanged
 * without consuming text.
 *
 * @note An HTML tag embedded in Markdown, inline (<em>) or block (<div ...>).
 * Highlights the angle brackets and tag name as HL_KEYWORD, attribute names
 * likewise, and quoted attribute values as HL_STRING -- the same classes the
 * XML tokenizer uses, so a tag looks the same whether it sits in an .html file
 * or inside a document.
 *
 * Returns the index just past '>', or `i` unchanged when s[i] doesn't begin a
 * plausible tag. Being conservative matters here: prose is full of '<' used as
 * a less-than sign, and highlighting "a < b" as markup would be worse than
 * leaving real tags plain. So a tag must start with a letter or '/' and
 * actually reach a '>' on the same line.
 */
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

/* Presentation is conservative where the tokenizer accepts escaped markers. */
static uint8_t syntaxMarkdownEscaped(const erow *row, int32_t at) {
    uint8_t escaped = 0;
    while (at > 0) {
        size_t len = utf8PrevCharLen(row->render, (size_t)at);
        if (len != 1 || row->render[at - 1] != '\\') break;
        escaped = !escaped;
        at--;
    }
    return escaped;
}

static void syntaxMarkdownSpan(erow *row, int32_t start, int32_t content_start,
    int32_t content_end, int32_t end, uint8_t link) {
    if (syntaxMarkdownEscaped(row, start) || (!link && syntaxMarkdownEscaped(row, content_end)))
        return;
    if (row->markdown_count == row->markdown_capacity) {
        int32_t capacity = row->markdown_capacity;
        capacity = capacity > INT32_MAX / 2 ? INT32_MAX : capacity ? capacity * 2 : 4;
        if (capacity == row->markdown_count) return;
        row->markdown_spans = teRealloc(row->markdown_spans,
            teArrayBytes((size_t)capacity, sizeof(*row->markdown_spans)));
        row->markdown_capacity = capacity;
    }
    row->markdown_spans[row->markdown_count++] =
        (struct markdownSpan){start, content_start, content_end, end, link};
}

/**
 * @brief Try to color a Markdown link or image with distinct label and URL colors.
 *
 * @details row->hl must be ready and i is a valid byte offset in s.
 * @return the exclusive end offset on success, or i unchanged when no complete link is
 * recognized.
 *
 * @note A Markdown link "[text](url)" or image "![alt](url)": brackets,
 * parentheses and the leading '!' as HL_PREPROCESSOR markers, the label as
 * HL_KEYWORD, the destination as HL_STRING. Keeping the two apart is what
 * makes a row of badges readable -- the long URL stops competing with the
 * label for attention.
 *
 * Handles the nested image-inside-link form "[![alt](img)](url)" that badge
 * rows use: the label scan tracks bracket depth, so the inner "![alt](img)" is
 * found as a unit and highlighted by recursing into it rather than terminating
 * the outer label at the first ']'.
 *
 * Returns the index just past the closing ')', or `i` unchanged when this
 * isn't a well-formed link -- an unmatched "[link]" or a stray "(text)" in
 * prose is left as plain text, same conservative stance as the HTML tag
 * scanner. Reference-style links ("[text][ref]") and bare autolinks are out of
 * scope for this first pass.
 */
static int32_t syntaxTryHighlightMarkdownLink(erow *row, const char *s, int32_t len, int32_t i) {
    int32_t start = i;
    int32_t span_count = row->markdown_count;
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
    row->markdown_count = span_count;
    int32_t destination = label_end + 2;
    while (destination < url_end && (s[destination] == ' ' || s[destination] == '\t'))
        destination += (int32_t)utf8NextCharLen(s, (size_t)destination, (size_t)url_end);
    if (start == bracket && destination < url_end)
        syntaxMarkdownSpan(row, start, bracket + 1, label_end, url_end + 1, 1);
    return url_end + 1;
}

/* Only standalone dollar delimiters open a block: an unmatched currency
 * symbol in prose must not turn the following document into mathematics. */
/**
 * @brief Recognize a Markdown math fence at the supplied byte position.
 */
static uint8_t syntaxMarkdownMathFence(const char *s, int32_t len) {
    int32_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    int32_t start = i;
    while (i < len && s[i] == '$') i++;
    int32_t width = i - start;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != len) return 0;
    return width == 1 ? 2 : (width == 2 ? 3 : 0);
}

/**
 * @brief Color Markdown text while carrying multiline constructs between rows.
 *
 * @details row->render must be current; previous flags describe fences, math,
 * front matter and emphasis. row_index is zero-based so front matter opens
 * only at the document start.
 */
void markdownHighlightRow(erow *row, uint8_t prev_in_fence, uint8_t prev_in_math,
    uint8_t prev_in_frontmatter, int32_t row_index, uint8_t prev_in_emphasis) {
    if (!syntaxReserveHighlight(row)) return;
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    const char *s = row->render;
    int32_t len = row->rsize;
    const char *source = row->chars ? row->chars : s;
    int32_t source_len = row->chars ? row->size : len;

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

    uint8_t dollar_fence = syntaxMarkdownMathFence(source, source_len);
    if (prev_in_math >= 2 || (!prev_in_math && dollar_fence && !prev_in_fence)) {
        memset(row->hl, HL_MATH, (size_t)len);
        row->hl_open_math = prev_in_math
            ? (dollar_fence == prev_in_math ? 0 : prev_in_math) : dollar_fence;
        row->hl_open_comment = 0;
        row->hl_open_frontmatter = 0;
        row->hl_open_emphasis = 0;
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
        char marker_cls = (prev_in_emphasis == 1) ? (char)HL_EMPHASIS : (char)HL_EMPHASIS_STRONG;
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

    int32_t heading_start = 0;
    while (heading_start < source_len && heading_start < 3 && source[heading_start] == ' ')
        heading_start++;
    if (heading_start < source_len && source[heading_start] == '#') {
        int32_t k = heading_start;
        while (k < source_len && source[k] == '#') k++;
        if (k - heading_start <= 6 &&
            (k == source_len || source[k] == ' ' || source[k] == '\t')) {
            row->hl_heading = 1;
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
                 * i.e. italic -- mapped to HL_EMPHASIS. run >= 2: a
                 * doubled marker, i.e. bold -- its own
                 * HL_EMPHASIS_STRONG class so the two are
                 * independently colorable. */
                enum syntaxHighlight cls = (run == 1) ? HL_EMPHASIS : HL_EMPHASIS_STRONG;
                for (int32_t k = i; k < close_start + run; k++) row->hl[k] = (uint8_t)cls;
                syntaxMarkdownSpan(row, i, i + run, close_start, close_start + run, 0);
                i = close_start + run;
                continue;
            }

            /* No closer on this line: highlight to end of line and
             * carry the span into the next row. Only for a marker that
             * looks like an opener -- one followed by something other
             * than whitespace -- so a trailing "*" or a lone "5 * 3"
             * asterisk doesn't start a span. */
            if (i + run < len && s[i + run] != ' ' && s[i + run] != '\t') {
                enum syntaxHighlight cls = (run == 1) ? HL_EMPHASIS : HL_EMPHASIS_STRONG;
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


/* ---- source navigation (independent of highlighting) ---- */

static int32_t markdownNext(const char *s, int32_t at, int32_t len) {
    return at + (int32_t)utf8NextCharLen(s, (size_t)at, (size_t)len);
}

static uint8_t markdownSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static int32_t markdownTrimStart(const erow *row) {
    int32_t at = 0;
    while (at < row->size && markdownSpace(row->chars[at])) at = markdownNext(row->chars, at, row->size);
    return at;
}

static int32_t markdownTrimEnd(const erow *row) {
    int32_t end = row->size;
    while (end > 0) {
        size_t step = utf8PrevCharLen(row->chars, (size_t)end);
        if (step != 1 || !markdownSpace(row->chars[end - 1])) break;
        end -= (int32_t)step;
    }
    return end;
}

static uint8_t markdownFence(const erow *row, uint8_t *marker, int32_t *width, int32_t *after) {
    int32_t at = markdownTrimStart(row);
    if (at > 3) return 0;
    while (at < row->size && row->chars[at] == '>') {
        at = markdownNext(row->chars, at, row->size);
        int32_t spaces = 0;
        while (at < row->size && row->chars[at] == ' ' && spaces < 3) {
            at = markdownNext(row->chars, at, row->size);
            spaces++;
        }
    }
    int32_t start = at;
    if (at >= row->size || (row->chars[at] != '`' && row->chars[at] != '~')) return 0;
    *marker = (uint8_t)row->chars[at];
    while (at < row->size && (uint8_t)row->chars[at] == *marker) at = markdownNext(row->chars, at, row->size);
    *width = at - start; *after = at;
    return *width >= 3;
}

static uint8_t markdownHasCodeClose(const struct editorBuffer *buffer, int32_t y, int32_t at, int32_t width) {
    for (int32_t line = y; line < buffer->row_count; line++) {
        const erow *row = &buffer->rows[line];
        if (line != y) {
            uint8_t marker; int32_t run, after;
            if (markdownTrimStart(row) == row->size || markdownFence(row, &marker, &run, &after)) return 0;
        }
        while (at < row->size) {
            if (row->chars[at] == '`') {
                int32_t start = at;
                while (at < row->size && row->chars[at] == '`') at = markdownNext(row->chars, at, row->size);
                if (at - start == width) return 1;
            } else at = markdownNext(row->chars, at, row->size);
        }
        at = 0;
    }
    return 0;
}

/* The scratch mask is owned by the caller and reused between rows. A zero
 * byte denotes code or an escaped grapheme, never an anchor/ref delimiter. */
static uint8_t markdownMaskRow(const struct editorBuffer *buffer, int32_t y,
    struct markdownScanState *state, uint8_t **mask, size_t *capacity) {
    const erow *row = &buffer->rows[y];
    if ((size_t)row->size > *capacity) {
        *capacity = teGrowCapacity(*capacity, (size_t)row->size, INT32_MAX);
        *mask = teRealloc(*mask, *capacity);
    }
    if (row->size) memset(*mask, 0, (size_t)row->size);
    uint8_t marker; int32_t width, after;
    if (markdownFence(row, &marker, &width, &after)) {
        if (!state->fence) { state->fence = marker; state->fence_width = width; state->inline_width = 0; }
        else if (marker == state->fence && width >= state->fence_width && after == markdownTrimEnd(row)) state->fence = 0;
        return 0;
    }
    if (state->fence) return 0;
    if (markdownTrimStart(row) == row->size) state->inline_width = 0;
    if (markdownTrimStart(row) >= 4) return 0;
    int32_t at = 0;
    while (at < row->size) {
        if (row->chars[at] == '`') {
            int32_t start = at;
            while (at < row->size && row->chars[at] == '`') at = markdownNext(row->chars, at, row->size);
            int32_t run = at - start;
            if (state->inline_width) {
                if (run == state->inline_width) state->inline_width = 0;
            } else if (markdownHasCodeClose(buffer, y, at, run)) state->inline_width = run;
            else memset(*mask + start, 1, (size_t)run);
            continue;
        }
        int32_t next = markdownNext(row->chars, at, row->size);
        if (!state->inline_width && row->chars[at] == '\\') {
            at = next;
            if (at < row->size) at = markdownNext(row->chars, at, row->size);
            continue;
        }
        if (!state->inline_width) memset(*mask + at, 1, (size_t)(next - at));
        at = next;
    }
    return 1;
}

uint8_t markdownSourceVisible(const struct editorBuffer *buffer, int32_t y, int32_t x) {
    if (y < 0 || y >= buffer->row_count || x < 0 || x >= buffer->rows[y].size) return 0;
    struct markdownScanState state = {0};
    uint8_t *mask = NULL; size_t capacity = 0;
    for (int32_t line = 0; line <= y; line++) markdownMaskRow(buffer, line, &state, &mask, &capacity);
    uint8_t visible = mask[x];
    free(mask);
    return visible;
}

/* Attributes are a bounded, quote-aware token list, not a Pandoc parser.
 * Only a standalone #token denotes an ID; # inside key="value" is ignored. */
static uint8_t markdownAttributes(const erow *row, const uint8_t *mask, int32_t start,
    struct markdownAttributes *attr) {
    if (start >= row->size || row->chars[start] != '{' || (mask && !mask[start])) return 0;
    *attr = (struct markdownAttributes){-1, -1, -1};
    int32_t at = start + 1;
    while (at < row->size) {
        while (at < row->size && markdownSpace(row->chars[at])) at = markdownNext(row->chars, at, row->size);
        if (at >= row->size) return 0;
        if (row->chars[at] == '}') { attr->end = at + 1; return 1; }
        int32_t token = at; uint8_t quote = 0, property = 0;
        while (at < row->size) {
            uint8_t c = (uint8_t)row->chars[at];
            if (!quote && (markdownSpace((char)c) || c == '}')) break;
            if (c == '\\') { at = markdownNext(row->chars, at, row->size); if (at < row->size) at = markdownNext(row->chars, at, row->size); continue; }
            if (!quote && c == '=' && at > token) property = 1;
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            at = markdownNext(row->chars, at, row->size);
        }
        if (quote || at == token) return 0;
        if (row->chars[token] == '#') {
            if (attr->id_start >= 0 || token + 1 == at) return 0;
            for (int32_t i = token + 1; i < at; i = markdownNext(row->chars, i, at))
                if (strchr("{}#\"'", row->chars[i])) return 0;
            attr->id_start = token + 1; attr->id_end = at;
        } else if (!property && !(row->chars[token] == '.' && at > token + 1) &&
            !(row->chars[token] == '-' && at == token + 1)) return 0;
    }
    return 0;
}

static int32_t markdownHeadingStart(const erow *row) {
    int32_t at = markdownTrimStart(row), start = at;
    if (at > 3) return -1;
    while (at < row->size && row->chars[at] == '#') at = markdownNext(row->chars, at, row->size);
    if (at == start || at - start > 6 || (at < row->size && !markdownSpace(row->chars[at]))) return -1;
    return at;
}

static int32_t markdownHeaderAttributes(const erow *row, const uint8_t *mask, struct markdownAttributes *attr) {
    int32_t end = markdownTrimEnd(row);
    for (int32_t at = 0; at < end; at = markdownNext(row->chars, at, end)) {
        if (row->chars[at] != '{' || at == 0 || !markdownSpace(row->chars[at - 1])) continue;
        if (markdownAttributes(row, mask, at, attr) && attr->end == end) return at;
    }
    return -1;
}

char *markdownHeadingSlug(const char *text, int32_t length) {
    erow row = {0}; row.chars = (char *)text; row.size = length;
    struct markdownAttributes attr;
    int32_t suffix = markdownHeaderAttributes(&row, NULL, &attr);
    if (suffix >= 0) length = suffix;
    char *slug = teMalloc(teSizeAdd((size_t)length, 1));
    int32_t at = 0; size_t out = 0;
    while (at < length && text[at] == ' ') at = markdownNext(text, at, length);
    while (at < length && text[at] == '#') at = markdownNext(text, at, length);
    while (at < length && text[at] == ' ') at = markdownNext(text, at, length);
    while (length > at && (text[length - 1] == ' ' || text[length - 1] == '#')) length--;
    while (at < length) {
        int32_t next = markdownNext(text, at, length);
        uint8_t c = (uint8_t)text[at];
        if (c >= 128) { memcpy(slug + out, text + at, (size_t)(next - at)); out += (size_t)(next - at); }
        else if (isalnum(c) || c == '_' || c == '-') slug[out++] = (char)tolower(c);
        else if (c == ' ' || c == '\t') slug[out++] = '-';
        at = next;
    }
    slug[out] = '\0'; return slug;
}

static int32_t markdownPairEnd(const erow *row, const uint8_t *mask, int32_t start, char open, char close) {
    int32_t depth = 0;
    for (int32_t at = start; at < row->size; at = markdownNext(row->chars, at, row->size)) {
        if (!mask[at]) continue;
        if (row->chars[at] == open) depth++;
        else if (row->chars[at] == close && --depth == 0) return at + 1;
    }
    return -1;
}

static uint8_t markdownIdEquals(const erow *row, int32_t from, int32_t to, const char *id) {
    return from >= 0 && strlen(id) == (size_t)(to - from) && !memcmp(row->chars + from, id, (size_t)(to - from));
}

static void markdownRecord(struct markdownAnchorSearch *search, int32_t y, int32_t x) {
    if (search->count < 2) search->count++;
    search->row = y; search->column = x;
}

static uint8_t markdownListItem(const erow *row, int32_t start) {
    if (start >= row->size) return 0;
    const char *s = row->chars;
    if (strchr("-*+", s[start]) && start + 1 < row->size && markdownSpace(s[start + 1])) return 1;
    int32_t at = start;
    while (at < row->size && s[at] >= '0' && s[at] <= '9') at = markdownNext(s, at, row->size);
    return at > start && at + 1 < row->size && (s[at] == '.' || s[at] == ')') && markdownSpace(s[at + 1]);
}

static uint8_t markdownObsidianId(const erow *row, int32_t from, int32_t end) {
    if (from == end) return 0;
    for (int32_t at = from; at < end; at = markdownNext(row->chars, at, end)) {
        uint8_t c = (uint8_t)row->chars[at];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

static enum markdownAnchorResult markdownResolve(const struct editorBuffer *buffer, const char *anchor,
    uint8_t explicit_only, int32_t *target_y, int32_t *target_x) {
    struct markdownScanState state = {0};
    struct markdownAnchorSearch found = {0};
    uint8_t *mask = NULL; size_t capacity = 0;
    uint8_t obsidian = anchor[0] == '^'; const char *id = anchor + (obsidian ? 1 : 0);
    int32_t last_start = -1, last_line = -2, item_start = -1, slug_y = -1;
    uint8_t last_kind = 0;
    if (!*id) return MARKDOWN_ANCHOR_MISSING;
    for (int32_t y = 0; y < buffer->row_count; y++) {
        const erow *row = &buffer->rows[y];
        uint8_t usable = markdownMaskRow(buffer, y, &state, &mask, &capacity);
        int32_t start = markdownTrimStart(row), end = markdownTrimEnd(row);
        if (start == end) continue;
        if (!usable) { last_start = -1; last_kind = 0; continue; }
        int32_t heading = markdownHeadingStart(row);
        uint8_t item = markdownListItem(row, start);
        uint8_t kind = item ? 2 : row->chars[start] == '|' ? 3 : row->chars[start] == '>' ? 4 : 1;
        if (last_line == y - 1 && last_kind == 2 && heading < 0 && kind == 1) kind = 2;
        int32_t block_start = y;
        if (last_start >= 0 && kind == last_kind && ((last_line == y - 1 && heading < 0) || (kind == 2 && item)))
            block_start = last_start;
        if (item) item_start = y;
        if (obsidian) {
            int32_t marker = end;
            while (marker > start) {
                int32_t prev = marker - (int32_t)utf8PrevCharLen(row->chars, (size_t)marker);
                if (markdownSpace(row->chars[prev])) break;
                marker = prev;
            }
            if (marker < end && row->chars[marker] == '^' && mask[marker] &&
                markdownObsidianId(row, marker + 1, end) && markdownIdEquals(row, marker + 1, end, id)) {
                if (marker == start) {
                    uint8_t before = y > 0 && markdownTrimStart(&buffer->rows[y - 1]) == buffer->rows[y - 1].size;
                    uint8_t after = y + 1 == buffer->row_count || markdownTrimStart(&buffer->rows[y + 1]) == buffer->rows[y + 1].size;
                    if (before && after && last_start >= 0) markdownRecord(&found, last_start, markdownTrimStart(&buffer->rows[last_start]));
                } else if (kind != 3 && kind != 4) markdownRecord(&found, kind == 2 ? item_start : block_start, 0);
            }
        } else {
            struct markdownAttributes attr;
            int32_t suffix = heading >= 0 ? markdownHeaderAttributes(row, mask, &attr) : -1;
            if (suffix >= 0 && markdownIdEquals(row, attr.id_start, attr.id_end, id)) markdownRecord(&found, y, 0);
            if (heading >= 0 && mask[start] && slug_y < 0 && !explicit_only) {
                char *slug = markdownHeadingSlug(row->chars, row->size);
                if (!strcmp(slug, anchor)) slug_y = y;
                free(slug);
            }
            int32_t div = start;
            while (div < end && row->chars[div] == ':') div = markdownNext(row->chars, div, end);
            if (div - start >= 3 && start <= 3 && mask[start]) {
                while (div < end && markdownSpace(row->chars[div])) div = markdownNext(row->chars, div, end);
                if (markdownAttributes(row, mask, div, &attr) && attr.end == end &&
                    markdownIdEquals(row, attr.id_start, attr.id_end, id)) markdownRecord(&found, y, start);
            } else {
                int32_t limit = suffix >= 0 ? suffix : end;
                for (int32_t at = 0; at < limit;) {
                    if (!mask[at]) { at = markdownNext(row->chars, at, limit); continue; }
                    uint8_t image = row->chars[at] == '!' && at + 1 < limit && row->chars[at + 1] == '[' && mask[at + 1];
                    int32_t bracket = image ? at + 1 : at;
                    if (row->chars[bracket] != '[') { at = markdownNext(row->chars, at, limit); continue; }
                    int32_t close = markdownPairEnd(row, mask, bracket, '[', ']');
                    if (close < 0) { at = markdownNext(row->chars, at, limit); continue; }
                    if (close < limit && row->chars[close] == '(') {
                        int32_t destination = markdownPairEnd(row, mask, close, '(', ')');
                        if (destination < 0) { at = close; continue; }
                        close = destination;
                        if (!image) { at = close; continue; }
                    } else if (image) { at = close; continue; }
                    if (markdownAttributes(row, mask, close, &attr)) {
                        if (markdownIdEquals(row, attr.id_start, attr.id_end, id)) markdownRecord(&found, y, image ? at : bracket + 1);
                        at = attr.end;
                    } else at = close;
                }
            }
        }
        if (row->chars[start] != '^') { last_start = heading >= 0 ? y : block_start; last_kind = heading >= 0 ? 5 : kind; last_line = y; }
    }
    free(mask);
    if (found.count > 1) return MARKDOWN_ANCHOR_DUPLICATE;
    if (found.count) { *target_y = found.row; *target_x = found.column; return MARKDOWN_ANCHOR_FOUND; }
    if (!obsidian && !explicit_only && slug_y >= 0) { *target_y = slug_y; *target_x = 0; return MARKDOWN_ANCHOR_FOUND; }
    return MARKDOWN_ANCHOR_MISSING;
}

enum markdownAnchorResult markdownResolveAnchor(const struct editorBuffer *buffer, const char *anchor,
    int32_t *target_y, int32_t *target_x) {
    return markdownResolve(buffer, anchor, 0, target_y, target_x);
}

static uint8_t markdownReferenceChar(const char *s, int32_t at, int32_t len) {
    struct utf8DecodeResult decoded = utf8DecodeChar(s + at, (size_t)(len - at));
    uint32_t c = decoded.codepoint;
    return decoded.valid && !((c >= 0x2000 && c <= 0x206f) || (c >= 0x3000 && c <= 0x303f) || c == 0xa0) && (c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':');
}

uint8_t markdownFindReference(const struct editorBuffer *buffer, int32_t y, int32_t cursor,
    struct textLink *link) {
    if (y < 0 || y >= buffer->row_count || cursor < 0 || cursor >= buffer->rows[y].size) return 0;
    struct markdownScanState state = {0}; uint8_t *mask = NULL; size_t capacity = 0;
    for (int32_t line = 0; line <= y; line++) markdownMaskRow(buffer, line, &state, &mask, &capacity);
    const erow *row = &buffer->rows[y]; const char *s = row->chars;
    uint8_t result = 0;
    for (int32_t at = 0; at < row->size;) {
        if (mask[at] && s[at] == '{') {
            struct markdownAttributes attr;
            if (markdownAttributes(row, mask, at, &attr)) { at = attr.end; continue; }
        }
        if (!mask[at] || s[at] != '@') { at = markdownNext(s, at, row->size); continue; }
        int32_t start = at, end = at + 1;
        if (at > 0) {
            int32_t prev = at - (int32_t)utf8PrevCharLen(s, (size_t)at);
            if (markdownReferenceChar(s, prev, at) || s[prev] == '.' || s[prev] == '@' ||
                (s[prev] == '[' && prev > 0 && s[prev - 1] == '[')) { at++; continue; }
        }
        while (end < row->size && mask[end] && markdownReferenceChar(s, end, row->size)) end = markdownNext(s, end, row->size);
        if (start > 0 && s[start - 1] == '[' && end < row->size && s[end] == ']') start--;
        int32_t visible_end = end < row->size && s[end] == ']' && start < at ? end + 1 : end;
        if (end + 1 < row->size && s[end] == '.' && markdownReferenceChar(s, end + 1, row->size)) {
            at = end + 1;
            continue;
        }
        if (cursor >= start && cursor < visible_end && end > at + 1) {
            int32_t id_end = end;
            while (id_end > at + 1 && (s[id_end - 1] == ':' || s[id_end - 1] == '-')) id_end--;
            char *id = teMalloc((size_t)(end - at));
            memcpy(id, s + at + 1, (size_t)(end - at - 1)); id[end - at - 1] = '\0';
            int32_t target_y, target_x;
            enum markdownAnchorResult found = markdownResolve(buffer, id, 1, &target_y, &target_x);
            if (found == MARKDOWN_ANCHOR_MISSING && id_end != end) {
                id[id_end - at - 1] = '\0';
                found = markdownResolve(buffer, id, 1, &target_y, &target_x);
                if (found != MARKDOWN_ANCHOR_MISSING) { end = id_end; visible_end = end; }
            }
            free(id);
            if (found != MARKDOWN_ANCHOR_MISSING && cursor < visible_end) {
                *link = (struct textLink){start, visible_end, at + 1, end}; result = 1; break;
            }
        }
        at = end > at ? end : at + 1;
    }
    free(mask); return result;
}
