/* tinyedit.h -- shared macros, enums and data structures for tinyedit.
 *
 * Shared editor types live here so responsibility-specific modules can
 * operate on the narrowest state object they need.
 *
 * Type convention (applies project-wide, not just this file): every
 * row/column index, buffer length, and screen dimension is int32_t --
 * one consistent signed type for the whole "position in the file/
 * screen" family, so indices and sizes compare and subtract directly
 * without signed/unsigned casts (the code relies on -1 sentinels, e.g.
 * search_match_y, and on mixed index/size comparisons like
 * E.document.cursor.cx > row->size, both of which need a signed type throughout).
 * Pure on/off flags are uint8_t. Raw bytes off the wire (terminal
 * input, UTF-8 code units) are uint8_t; decoded Unicode codepoints are
 * uint32_t (see utf8.h).
 */

#ifndef __TINYEDIT_H
#define __TINYEDIT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "settings.h"

/* ---- config -------------------------------------------------------- */

#define TE_VERSION "0.3.2"
#define ABUF_INIT {NULL, 0}
#define INVISIBLE_SPACE_GLYPH '.'
#define INVISIBLE_TAB_GLYPH '>'

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
    SHIFT_HOME,
    SHIFT_END,
    /* Start/end of the whole buffer, bound to both Ctrl+Home/Ctrl+End
     * (the usual binding, which plenty of terminals never send) and
     * Ctrl+PageUp/Ctrl+PageDown as a fallback that gets through more
     * reliably. */
    DOC_HOME,
    DOC_END,
    SHIFT_DOC_HOME,
    SHIFT_DOC_END,
    /* Shift+Tab, sent as CSI Z ("backtab") by every terminal this
     * project targets. Only meaningful with a selection, where it
     * outdents the selected lines (see editorIndentSelection()). */
    SHIFT_TAB,
    F1_KEY,
    F2_KEY,
    F3_KEY,
    F4_KEY,
    /* Save As, bound to F4 (see editorReadKey()'s SS3 handling) and,
     * where the terminal actually sends it as a distinct sequence
     * rather than the same byte as plain Ctrl-S, to Ctrl-Shift-S via
     * CSI-u (ESC[115;6u -- 115 is 's', lowercase: the shift is
     * reported in the modifier field, not by shifting the codepoint). */
    SAVE_AS_KEY,
    /* Bracketed paste start (ESC[200~, see editorReadKey()) -- signals
     * the caller to switch to editorReadPastedText() instead of
     * treating subsequent bytes as individual keystrokes. See
     * tinyedit.c's paste handling for why this exists: without
     * bracketed paste, pasted text arrives byte-by-byte through the
     * normal keystroke path, which is slow (one undo-snapshot/full
     * redraw per character) AND wrong (auto-close-pair logic reacts to
     * every '(', '\'', '`' etc. in the pasted text as if the user had
     * typed it, leaving spurious closing characters behind). */
    PASTE_START_KEY,
    /* SGR mouse report (ESC[<Cb;Cx;Cy(M|m), see editorReadKey()) --
     * signals the caller that a mouse event was decoded into the
     * mouseEventButton/mouseEventCol/mouseEventRow/mouseEventPress
     * globals (see tinyedit.c), to be read immediately (before the
     * next editorReadKey() call, which may overwrite them). */
    MOUSE_EVENT_KEY
};

enum undoEditType { EDIT_NONE, EDIT_INSERT, EDIT_DELETE, EDIT_OTHER };

typedef struct erow {
    int32_t size;
    int32_t rsize;  /* size of the rendered line (tabs expanded) */
    char *chars;
    char *render;
    /* One enum syntaxHighlight byte per render[] column, recomputed by
     * syntaxHighlightRow() (see syntax.h) whenever the row's text
     * changes. NULL/rsize-0 rows (or when S.syntax_highlight is off)
     * leave this NULL -- editorDrawRowSegment() falls back to the
     * default text color in that case, it does not require hl to be
     * populated. */
    uint8_t *hl;
    /* Whether this row ends inside an unclosed block comment, carried
     * into the next row's syntaxHighlightRow() call as its
     * prev_open_comment argument -- see syntax.h. */
    uint8_t hl_open_comment;
    /* Whether this row ends inside an unclosed multi-line LaTeX
     * "\[...\]" display-math span (see syntaxTryHighlightMathMultiline()
     * in syntax.c) -- a separate bit from hl_open_comment rather than
     * reusing it, since a language could in principle have both a block
     * comment AND math_mode active at once (LaTeX itself doesn't, but
     * nothing enforces that the two states can't coexist for some
     * future language), and conflating them would silently misrender
     * whichever one lost the race. */
    uint8_t hl_open_math;
    /* Whether this row ends inside a YAML front matter block -- the
     * "---" delimited metadata header some Markdown dialects (Jekyll,
     * Eleventy, Hugo) put at the top of a file. Its own bit for the
     * same reason hl_open_math is: the Markdown tokenizer already uses
     * hl_open_comment for fenced code blocks, and a document can
     * contain both. Only ever set on rows near the top of the file,
     * since front matter is recognized only when it opens on row 0. */
    uint8_t hl_open_frontmatter;
    /* Whether this row ends inside an unclosed Markdown emphasis span:
     * 0 = none, 1 = italic (single marker), 2 = bold (double marker).
     * One field with three states rather than two flags, since the two
     * can't be open at once -- the tokenizer tracks a single innermost
     * span, and nesting bold inside italic across lines is past what
     * this tokenizer attempts.
     *
     * Unlike the three states above, an emphasis span is closed by a
     * blank line as well as by its matching marker: a lone '*' in
     * prose ("filetype.*", "5 * 3") is indistinguishable from an
     * opener until the closer shows up, so bounding it at the
     * paragraph keeps such a stray marker from recoloring the rest of
     * the document. That also matches how Markdown itself scopes
     * emphasis. */
    uint8_t hl_open_emphasis;
    /* Cached soft-wrap segmentation. Byte and display-column offsets
     * are kept separately because UTF-8 makes them diverge. Rebuilt
     * when render changes or the effective wrap width changes. */
    int32_t *seg_start;
    int32_t *seg_start_rx;
    int32_t seg_count;
    int32_t seg_wrapcols;
} erow;

typedef struct undoRow {
    int32_t size;
    char *chars;
} undoRow;

typedef struct undoSnapshot {
    undoRow *row;
    int32_t numrows;
    int32_t cx, cy;
} undoSnapshot;

struct helpEntry {
    const char *key;
    const char *desc;
};

struct autoClosePair {
    char open;
    char close;
};

struct autoCloseMultiByte {
    const char open[4];
    int32_t open_len;
    const char close[4];
    int32_t close_len;
};

/* Generic growable byte buffer used to batch a full screen redraw into
 * one write(), instead of issuing many small writes per frame. */
struct abuf {
    char *b;
    int32_t len;
};

struct editorBuffer {
    int32_t row_count;
    erow *rows;
};

struct editorCursor {
    int32_t cx, cy;           /* cursor position in the file (chars) */
    int32_t rx;                /* cursor position in the rendered line */
};

struct editorSelection {
    uint8_t active;
    int32_t anchor_x, anchor_y;
    uint8_t pinned;
};

struct editorHistory {
    undoSnapshot *undo_stack;
    int32_t undo_count;
    undoSnapshot *redo_stack;
    int32_t redo_count;
    enum undoEditType last_edit_type;
    time_t last_edit_time;
};

struct editorFileState {
    enum lineEndingMode detected_line_ending;
    uint8_t line_endings_mixed;
    uint8_t dirty;
    char *filename;
    time_t last_backup_time;
};

struct editorDocument {
    struct editorBuffer buffer;
    struct editorCursor cursor;
    struct editorSelection selection;
    struct editorHistory history;
    struct editorFileState file;
};

struct editorView {
    int32_t rowoff;             /* row of file we are scrolled to */
    int32_t coloff;             /* column of file we are scrolled to */
    /* Set by the mouse wheel (see MOUSE_EVENT_KEY handling in
     * tinyedit.c) to tell editorScroll() to skip its usual "keep the
     * cursor on screen" re-centering for exactly one redraw -- the
     * wheel scrolls the view without moving the cursor, and without
     * this flag editorScroll() would otherwise immediately snap
     * E.view.rowoff back to hug the (stationary) cursor. Cleared inside
     * editorScroll() itself right after being consulted, so any
     * subsequent real cursor movement (arrow keys, click, typing, ...)
     * goes through the normal follow-the-cursor path on its very next
     * redraw -- this is a one-shot override, not a persistent mode. */
    uint8_t free_scroll;
    int32_t screenrows;
    int32_t screencols;
};

struct editorUi {
    /* 512, not a smaller round number like 80: this has to hold a
     * fully-formatted prompt (fixed instructions + live query text
     * being typed, see editorPromptCB() in tinyedit.c) without
     * vsnprintf() in editorSetStatusMessage() silently truncating the
     * user's own input before editorDrawMessageBar()'s tail-scroll
     * ever gets a chance to show it -- a search query longer than the
     * buffer would otherwise appear to "stop accepting input" while
     * still being tracked correctly underneath, since only the
     * DISPLAY was truncated, not the actual search buffer. 512 is
     * comfortably larger than any realistic terminal width plus any
     * realistic query length. */
    char statusmsg[512];
    time_t statusmsg_time;
    /* When set, the message bar shows statusmsg indefinitely instead
     * of clearing it after the usual 5s timeout -- used for the
     * startup shortcut hint, which should stay until the user does
     * something that produces a real status update (e.g. saving). */
    uint8_t statusmsg_sticky;
};

struct editorSearch {
    int32_t search_match_y, search_match_x, search_match_len; /* match_y == -1: no match */
    uint8_t switch_to_replace;
    int32_t saved_cx, saved_cy, saved_rowoff, saved_coloff;
    int32_t direction;
    uint8_t regex_mode;
};

struct editorConfig {
    struct editorDocument document;
    struct editorView view;
    struct editorUi ui;
    struct editorSearch search;
};

#endif /* __TINYEDIT_H */
