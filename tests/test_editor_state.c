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

int main(void) {
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
