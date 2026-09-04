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

static void highlight(erow *row, const char *filename, uint8_t prev_comment,
    uint8_t prev_math) {
    syntaxHighlightRow(row, filename, 1, prev_comment, prev_math);
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

    if (failures) return 1;
    puts("syntax tests: ok");
    return 0;
}
