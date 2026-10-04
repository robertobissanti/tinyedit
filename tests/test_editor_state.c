#include "editor_state.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    exit(1);
}

static void expectRange(struct editorSelection selection, struct editorCursor cursor,
    int32_t expected_sy, int32_t expected_sx, int32_t expected_ey, int32_t expected_ex) {
    int32_t sy = -1, sx = -1, ey = -1, ex = -1;
    if (!editorSelectionRange(&selection, &cursor, &sy, &sx, &ey, &ex))
        fail("expected active range");
    if (sy != expected_sy || sx != expected_sx || ey != expected_ey || ex != expected_ex)
        fail("normalized range mismatch");
}

static void testAutoClosePolicy(void) {
    const char openers[] = "({[\"'$`";
    const char closers[] = ")}]\"'$`";
    for (int32_t i = 0; i < (int32_t)sizeof(openers) - 1; i++) {
        const struct autoClosePair *pair = editorAutoClosePairFor(openers[i], 1);
        if (!pair || pair->open != openers[i] || pair->close != closers[i])
            fail("shared opener policy");
        if (editorIsAsymmetricAutoClose(closers[i]) != (i < 3))
            fail("shared closer classification");
        if (editorIsAsymmetricAutoClose(openers[i]))
            fail("opener classified as asymmetric closer");
    }
    if (editorAutoClosePairFor('\'', 0) || !editorAutoClosePairFor('\'', 1))
        fail("apostrophe opt-in policy");
    const int32_t unrelated[] = {'a', '>', 0x80, -1, ARROW_LEFT};
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); i++)
        if (editorAutoClosePairFor(unrelated[i], 1) || editorIsAsymmetricAutoClose(unrelated[i]))
            fail("unrelated input classified as a pair");
}

int main(void) {
    testAutoClosePolicy();
    struct editorSelection selection = {0, 2, 3, 0};
    struct editorCursor cursor = {8, 4, 0};
    int32_t sy = 7, sx = 7, ey = 7, ex = 7;

    if (editorSelectionRange(&selection, &cursor, &sy, &sx, &ey, &ex))
        fail("inactive selection exposed a range");
    if (sy != 7 || sx != 7 || ey != 7 || ex != 7)
        fail("inactive selection changed outputs");

    selection.active = 1;
    expectRange(selection, cursor, 3, 2, 4, 8);
    selection.anchor_y = 5;
    selection.anchor_x = 1;
    expectRange(selection, cursor, 4, 8, 5, 1);

    selection.anchor_y = cursor.cy;
    selection.anchor_x = cursor.cx;
    selection.pinned = 1;
    if (editorSelectionRange(&selection, &cursor, &sy, &sx, &ey, &ex))
        fail("zero-width pinned selection exposed a range");
    if (!selection.active || !selection.pinned)
        fail("range query mutated pinned selection lifecycle");

    puts("editor state tests: ok");
    return 0;
}
