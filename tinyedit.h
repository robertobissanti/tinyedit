/* tinyedit.h -- shared macros, enums and data structures for tinyedit.
 *
 * tinyedit is a single translation unit (tinyedit.c); this header exists
 * so the type definitions are separated from the logic that uses them,
 * per project convention (see CLAUDE.md: includes -> defines -> types ->
 * globals -> functions, with types living in the header).
 */

#ifndef __TINYEDIT_H
#define __TINYEDIT_H

#include <stddef.h>
#include <termios.h>
#include <time.h>

#include "settings.h"

/* ---- config -------------------------------------------------------- */

#define TE_VERSION "0.1"
#define TE_QUIT_TIMES 2
#define ABUF_INIT {NULL, 0}

#define CTRL_KEY(k) ((k) & 0x1f)

/* tab_stop and undo_max_depth are now user-configurable (struct
 * editorSettings, see settings.h) instead of fixed macros. */
#define UNDO_COALESCE_SECS 1 /* time(NULL) is only 1s granular; see editorPushUndo() */

/* ---- types ------------------------------------------------------------ */

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    ALT_ARROW_LEFT,
    ALT_ARROW_RIGHT,
    SHIFT_ARROW_LEFT,
    SHIFT_ARROW_RIGHT,
    SHIFT_ARROW_UP,
    SHIFT_ARROW_DOWN,
    SHIFT_PAGE_UP,
    SHIFT_PAGE_DOWN,
    F2_KEY
};

enum undoEditType { EDIT_NONE, EDIT_INSERT, EDIT_DELETE, EDIT_OTHER };

typedef struct erow {
    int size;
    int rsize;   /* size of the rendered line (tabs expanded) */
    char *chars;
    char *render;
} erow;

typedef struct undoSnapshot {
    erow *row;
    int numrows;
    int cx, cy;
} undoSnapshot;

/* Generic growable byte buffer used to batch a full screen redraw into
 * one write(), instead of issuing many small writes per frame. */
struct abuf {
    char *b;
    int len;
};

struct editorConfig {
    int cx, cy;             /* cursor position in the file (chars) */
    int rx;                 /* cursor position in the rendered line */
    int rowoff;              /* row of file we are scrolled to */
    int coloff;              /* column of file we are scrolled to */
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;

    int sel_active;
    int sel_anchor_x, sel_anchor_y;
    /* When set (via Ctrl-T), plain arrow keys extend the selection just
     * like Shift+Arrow does, instead of collapsing it. Universal
     * fallback for terminals that can't report Shift+Arrow as a
     * distinct sequence (e.g. Terminal.app on macOS -- see CLAUDE.md). */
    int sel_pinned;

    undoSnapshot *undo_stack;
    int undo_count;
    undoSnapshot *redo_stack;
    int redo_count;
    enum undoEditType last_edit_type;
    time_t last_edit_time;

    int search_match_y, search_match_x, search_match_len; /* match_y == -1: no match */
};

#endif /* __TINYEDIT_H */
