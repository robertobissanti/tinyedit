#include "syntax.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void initRow(erow *row, const char *text) {
    memset(row, 0, sizeof(*row));
    row->rsize = (int32_t)strlen(text);
    row->render = strdup(text);
}

static void freeRow(erow *row) {
    free(row->render);
    free(row->hl);
}

static void expectClass(const erow *row, int32_t at, enum syntaxHighlight expected,
    const char *label) {
    if (!row->hl || row->hl[at] != (uint8_t)expected) {
        fprintf(stderr, "FAIL %-32s at %d: got %d, expected %d\n", label, at,
            row->hl ? row->hl[at] : -1, expected);
        failures++;
    }
}

/* Row index 1, not 0: these cases are about constructs in the body of
 * a file, and row 0 is where YAML front matter would start. The
 * front-matter tests below call syntaxHighlightRow() directly instead. */
static void highlight(erow *row, const char *filename, uint8_t prev_comment,
    uint8_t prev_math) {
    syntaxHighlightRow(row, filename, 1, prev_comment, prev_math, 0, 1, 0);
}

int main(void) {
    erow row;

    initRow(&row, "   #include <stdint.h>");
    highlight(&row, "demo.c", 0, 0);
    expectClass(&row, 3, HL_PREPROCESSOR, "indented C preprocessor");
    freeRow(&row);

    initRow(&row, "éint int value");
    highlight(&row, "demo.c", 0, 0);
    expectClass(&row, 2, HL_NORMAL, "Unicode identifier suffix");
    expectClass(&row, 6, HL_KEYWORD, "standalone C keyword");
    freeRow(&row);

    initRow(&row, "int main(void) { return 0; }");
    highlight(&row, "test.C", 0, 0);
    expectClass(&row, 0, HL_KEYWORD, "uppercase C extension keyword");
    expectClass(&row, 17, HL_KEYWORD, "C return keyword");
    freeRow(&row);

    initRow(&row, "int main(void){ return 1; }");
    highlight(&row, "testC.c", 0, 0);
    expectClass(&row, 0, HL_KEYWORD, "testC.c int keyword");
    expectClass(&row, 4, HL_FUNCTION, "testC.c identifier before '(' is a function");
    expectClass(&row, 10, HL_KEYWORD, "testC.c void keyword");
    expectClass(&row, 18, HL_KEYWORD, "testC.c return keyword");
    freeRow(&row);

    /* Function names are an identifier immediately followed by '(' --
     * anything else stays unhighlighted, and a keyword before '(' is
     * still a keyword (if/while/sizeof must not read as calls). */
    initRow(&row, "int x = add(a, b);");
    highlight(&row, "demo.c", 0, 0);
    expectClass(&row, 8, HL_FUNCTION, "called function name");
    expectClass(&row, 12, HL_NORMAL, "argument stays normal");
    freeRow(&row);

    initRow(&row, "value = other;");
    highlight(&row, "demo.c", 0, 0);
    expectClass(&row, 0, HL_NORMAL, "identifier with no '(' stays normal");
    freeRow(&row);

    initRow(&row, "if (cond) { }");
    highlight(&row, "demo.c", 0, 0);
    expectClass(&row, 0, HL_KEYWORD, "keyword before '(' stays a keyword");
    freeRow(&row);

    initRow(&row, "spaced  (x);");
    highlight(&row, "demo.c", 0, 0);
    expectClass(&row, 0, HL_FUNCTION, "spaces before '(' still a function");
    freeRow(&row);

    initRow(&row, "/* open");
    highlight(&row, "demo.c", 0, 0);
    if (!row.hl_open_comment) { fprintf(stderr, "FAIL open C comment state\n"); failures++; }
    freeRow(&row);

    initRow(&row, "still comment */ int");
    highlight(&row, "demo.c", 1, 0);
    expectClass(&row, 0, HL_COMMENT, "continued C comment");
    expectClass(&row, 17, HL_KEYWORD, "keyword after C comment");
    freeRow(&row);

    initRow(&row, "--colore-caffè: \"a\\\"b\";");
    highlight(&row, "demo.css", 0, 0);
    expectClass(&row, 2, HL_KEYWORD, "Unicode CSS property");
    expectClass(&row, 22, HL_STRING, "escaped CSS quote");
    freeRow(&row);

    initRow(&row, "plain \"text\" <nodo città=\"Roma\">");
    highlight(&row, "demo.xml", 0, 0);
    expectClass(&row, 6, HL_NORMAL, "quote outside XML tag");
    expectClass(&row, 14, HL_KEYWORD, "XML tag name");
    expectClass(&row, 19, HL_KEYWORD, "Unicode XML attribute");
    freeRow(&row);

    initRow(&row, "# Heading **bold** $x+1$");
    highlight(&row, "demo.md", 0, 0);
    expectClass(&row, 0, HL_PREPROCESSOR, "Markdown heading");
    freeRow(&row);

    initRow(&row, "def saluta(): # comment");
    highlight(&row, "demo.py", 0, 0);
    expectClass(&row, 0, HL_KEYWORD, "Python keyword");
    expectClass(&row, 14, HL_COMMENT, "Python comment");
    freeRow(&row);

    /* Built-in extensions must be reported as having highlighting
     * available, including the three handled by dedicated tokenizers
     * rather than by syntaxLangTable -- editorFiletypeLabel()'s
     * "highlight config missing" warning keys off this, and would fire
     * spuriously on every C/Markdown/CSS file if they were missed. */
    if (!syntaxHasBuiltinExtension("c")) {
        fprintf(stderr, "FAIL builtin extension c\n");
        failures++;
    }
    if (!syntaxHasBuiltinExtension("md")) {
        fprintf(stderr, "FAIL builtin extension md (dedicated tokenizer)\n");
        failures++;
    }
    if (!syntaxHasBuiltinExtension("css")) {
        fprintf(stderr, "FAIL builtin extension css (dedicated tokenizer)\n");
        failures++;
    }
    /* YAML front matter: only when the opening "---" is row 0, with
     * keys and values highlighted separately, and a blank line inside
     * it must not end the block. */
    initRow(&row, "---");
    syntaxHighlightRow(&row, "d.md", 1, 0, 0, 0, 0, 0);
    expectClass(&row, 0, HL_PREPROCESSOR, "front matter opening fence");
    if (!row.hl_open_frontmatter) {
        fprintf(stderr, "FAIL front matter did not open\n");
        failures++;
    }
    freeRow(&row);

    initRow(&row, "title: Il mio post");
    syntaxHighlightRow(&row, "d.md", 1, 0, 0, 1, 1, 0);
    expectClass(&row, 0, HL_KEYWORD, "front matter key");
    expectClass(&row, 5, HL_PREPROCESSOR, "front matter colon");
    expectClass(&row, 8, HL_STRING, "front matter value");
    freeRow(&row);

    /* A "---" anywhere but row 0 is a horizontal rule, not front
     * matter -- highlighting the rest of the document as metadata
     * would be the damaging failure mode here. */
    initRow(&row, "---");
    syntaxHighlightRow(&row, "d.md", 1, 0, 0, 0, 4, 0);
    if (row.hl_open_frontmatter) {
        fprintf(stderr, "FAIL '---' mid-document opened front matter\n");
        failures++;
    }
    freeRow(&row);

    /* HTML embedded in Markdown, inline and with attributes. */
    initRow(&row, "Testo <strong>x</strong> fine.");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 6, HL_KEYWORD, "inline HTML tag in Markdown");
    expectClass(&row, 0, HL_NORMAL, "prose around inline HTML tag");
    freeRow(&row);

    initRow(&row, "<div class=\"box\">");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 1, HL_KEYWORD, "block HTML tag name in Markdown");
    expectClass(&row, 5, HL_KEYWORD, "HTML attribute name in Markdown");
    expectClass(&row, 12, HL_STRING, "HTML attribute value in Markdown");
    freeRow(&row);

    /* Prose comparisons must not be mistaken for tags: this is why the
     * tag scanner requires a letter after '<' and a '>' on the line. */
    initRow(&row, "quando 5 < 7 e a > b");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 9, HL_NORMAL, "less-than in prose is not a tag");
    freeRow(&row);

    /* A tag inside a code span stays a code span. */
    initRow(&row, "Usa `<div>` qui.");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 5, HL_STRING, "HTML tag inside Markdown code span");
    freeRow(&row);

    /* Markdown links and images: label and destination get different
     * classes so a row of badges stays readable. */
    initRow(&row, "Vedi [la doc](https://example.com) qui.");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 5, HL_PREPROCESSOR, "link opening bracket");
    expectClass(&row, 6, HL_KEYWORD, "link label");
    expectClass(&row, 14, HL_STRING, "link destination");
    expectClass(&row, 0, HL_NORMAL, "prose around link");
    freeRow(&row);

    /* Badge row form: an image nested inside a link. The outer label
     * scan must not stop at the inner ']'. */
    initRow(&row, "[![License](https://img.shields.io/x)](LICENSE)");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 0, HL_PREPROCESSOR, "nested link opening bracket");
    expectClass(&row, 1, HL_PREPROCESSOR, "nested image marker");
    expectClass(&row, 3, HL_KEYWORD, "nested image label");
    expectClass(&row, 12, HL_STRING, "nested image destination");
    expectClass(&row, 39, HL_STRING, "outer link destination");
    freeRow(&row);

    /* An unmatched "[link]" is prose, not a link. */
    initRow(&row, "Un [link] non chiuso.");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 3, HL_NORMAL, "unmatched bracket is not a link");
    freeRow(&row);

    /* Underscores inside a URL must not open an emphasis span that
     * would swallow the rest of the line. */
    initRow(&row, "[a](http://x/some_file) e **bold** dopo.");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 27, HL_EMPHASIS_STRONG, "bold still works after a URL with underscores");
    freeRow(&row);

    /* Emphasis spanning several lines: a caption split across two rows
     * is the common case, and used to lose its highlighting entirely. */
    initRow(&row, "*Python source in tinyedit, with line");
    highlight(&row, "d.md", 0, 0);
    expectClass(&row, 1, HL_KEYWORD, "italic opened without closer on same line");
    if (row.hl_open_emphasis != 1) {
        fprintf(stderr, "FAIL italic did not carry to next row\n");
        failures++;
    }
    freeRow(&row);

    initRow(&row, "and configurable syntax colors.*");
    syntaxHighlightRow(&row, "d.md", 1, 0, 0, 0, 2, 1);
    expectClass(&row, 0, HL_KEYWORD, "italic continued from previous row");
    if (row.hl_open_emphasis != 0) {
        fprintf(stderr, "FAIL italic did not close on its marker\n");
        failures++;
    }
    freeRow(&row);

    initRow(&row, "sulla riga dopo.**");
    syntaxHighlightRow(&row, "d.md", 1, 0, 0, 0, 2, 2);
    expectClass(&row, 0, HL_EMPHASIS_STRONG, "bold continued from previous row");
    freeRow(&row);

    /* A marker followed by whitespace ("5 * 3") is arithmetic, not an
     * opener -- otherwise it would recolor the rest of the paragraph. */
    initRow(&row, "Un calcolo 5 * 3 = 15 qui.");
    highlight(&row, "d.md", 0, 0);
    if (row.hl_open_emphasis != 0) {
        fprintf(stderr, "FAIL isolated '*' opened an emphasis span\n");
        failures++;
    }
    expectClass(&row, 13, HL_NORMAL, "isolated asterisk stays prose");
    freeRow(&row);

    /* Template markup languages (njk, jinja, liquid, twig) are user
     * .conf files, not compiled in -- they reach the XML tokenizer via
     * base_tokenizer = xml. An extension no config declares stays
     * unknown. */
    if (syntaxHasBuiltinExtension("zzz")) {
        fprintf(stderr, "FAIL unknown extension reported as built-in\n");
        failures++;
    }
    /* Extension matching is case-insensitive (see
     * syntaxExtensionEquals), so the query side must be too. */
    if (!syntaxHasBuiltinExtension("C")) {
        fprintf(stderr, "FAIL builtin extension lookup is case-sensitive\n");
        failures++;
    }

    if (failures) return 1;
    puts("syntax tests: ok");
    return 0;
}
