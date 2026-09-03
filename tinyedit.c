/* tinyedit.c -- a tiny full-screen text editor in the style of antirez's
 * "kilo", built with plain C and raw terminal mode (no external TUI lib).
 *
 * Keys:
 *   Arrows, Home, End, PageUp, PageDown   move the cursor
 *   Enter                                 new line
 *   Backspace / Delete                    delete char
 *   Ctrl-S                                save
 *   Ctrl-Q                                quit (asks twice if unsaved)
 *
 * Build:  make
 * Run:    ./tinyedit [filename]
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "clipboard.h"

/* ---- config -------------------------------------------------------- */

#define TE_VERSION "0.1"
#define TE_TAB_STOP 4
#define TE_QUIT_TIMES 2
#define ABUF_INIT {NULL, 0}

#define CTRL_KEY(k) ((k) & 0x1f)

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
    SHIFT_ARROW_DOWN
};

/* ---- data ------------------------------------------------------------ */

typedef struct erow {
    int size;
    int rsize;   /* size of the rendered line (tabs expanded) */
    char *chars;
    char *render;
} erow;

#define UNDO_MAX_DEPTH 200
#define UNDO_COALESCE_SECS 1 /* time(NULL) is only 1s granular; see editorPushUndo() */

enum undoEditType { EDIT_NONE, EDIT_INSERT, EDIT_DELETE, EDIT_OTHER };

typedef struct undoSnapshot {
    erow *row;
    int numrows;
    int cx, cy;
} undoSnapshot;

struct editorConfig {
    int cx, cy;          /* cursor position in the file (chars) */
    int rx;               /* cursor position in the rendered line */
    int rowoff;            /* row of file we are scrolled to */
    int coloff;            /* column of file we are scrolled to */
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

    undoSnapshot *undo_stack;
    int undo_count;
    undoSnapshot *redo_stack;
    int redo_count;
    enum undoEditType last_edit_type;
    time_t last_edit_time;
};

static struct editorConfig E;

/* ---- terminal ---------------------------------------------------------- */

static void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

static void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

static void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

static int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = (int)read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';

        /* Meta/readline-style word jump: ESC b (backward-word),
         * ESC f (forward-word). Single byte after ESC, no '[' or 'O'. */
        if (seq[0] == 'b') return ALT_ARROW_LEFT;
        if (seq[0] == 'f') return ALT_ARROW_RIGHT;

        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                } else if (seq[2] == ';') {
                    /* Modified arrow/nav key: ESC [ 1 ; <mod> <letter>
                     * e.g. Alt+Up = ESC [ 1 ; 3 A. We only need the
                     * modifier digit and the final letter. */
                    char mod, letter;
                    if (read(STDIN_FILENO, &mod, 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &letter, 1) != 1) return '\x1b';
                    int is_alt = (mod == '3');
                    int is_shift = (mod == '2');
                    switch (letter) {
                        case 'A': return is_shift ? SHIFT_ARROW_UP : ARROW_UP;
                        case 'B': return is_shift ? SHIFT_ARROW_DOWN : ARROW_DOWN;
                        case 'C':
                            if (is_alt) return ALT_ARROW_RIGHT;
                            if (is_shift) return SHIFT_ARROW_RIGHT;
                            return ARROW_RIGHT;
                        case 'D':
                            if (is_alt) return ALT_ARROW_LEFT;
                            if (is_shift) return SHIFT_ARROW_LEFT;
                            return ARROW_LEFT;
                        case 'H': return HOME_KEY;
                        case 'F': return END_KEY;
                    }
                    return '\x1b';
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

static int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

static int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ---- row operations ----------------------------------------------------- */

static int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        /* UTF-8 continuation bytes (10xxxxxx) are part of the previous
         * character and occupy no extra terminal column. */
        if (((unsigned char)row->chars[j] & 0xC0) == 0x80) continue;
        if (row->chars[j] == '\t')
            rx += (TE_TAB_STOP - 1) - (rx % TE_TAB_STOP);
        rx++;
    }
    return rx;
}

static void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc((size_t)(row->size + tabs * (TE_TAB_STOP - 1) + 1));

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TE_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

static void editorInsertRow(int at, const char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (size_t)(E.numrows - at));

    E.row[at].size = (int)len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

static void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

static void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (size_t)(E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

static void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, (size_t)(row->size + 2));
    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;
    editorUpdateRow(row);
    E.dirty++;
}

static void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, (size_t)row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

static void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* ---- undo / redo ------------------------------------------------------- */

static void editorSetStatusMessage(const char *fmt, ...);

/* Deep-copies the current buffer (rows + cursor) into a snapshot. */
static undoSnapshot editorMakeSnapshot(void) {
    undoSnapshot snap;
    snap.numrows = E.numrows;
    snap.cx = E.cx;
    snap.cy = E.cy;
    snap.row = malloc(sizeof(erow) * (size_t)E.numrows);
    for (int i = 0; i < E.numrows; i++) {
        snap.row[i].size = E.row[i].size;
        snap.row[i].rsize = E.row[i].rsize;
        snap.row[i].chars = malloc((size_t)E.row[i].size + 1);
        memcpy(snap.row[i].chars, E.row[i].chars, (size_t)E.row[i].size + 1);
        snap.row[i].render = malloc((size_t)E.row[i].rsize + 1);
        memcpy(snap.row[i].render, E.row[i].render, (size_t)E.row[i].rsize + 1);
    }
    return snap;
}

static void editorFreeSnapshot(undoSnapshot *snap) {
    for (int i = 0; i < snap->numrows; i++) {
        free(snap->row[i].chars);
        free(snap->row[i].render);
    }
    free(snap->row);
    snap->row = NULL;
    snap->numrows = 0;
}

static void editorClearRedoStack(void) {
    for (int i = 0; i < E.redo_count; i++) editorFreeSnapshot(&E.redo_stack[i]);
    E.redo_count = 0;
}

/* Pushes a snapshot of the buffer as it was BEFORE the edit about to
 * happen, unless this edit can be coalesced with the previous one (same
 * type, within UNDO_COALESCE_SECS -- a rough approximation since time(NULL)
 * only has 1s resolution, but good enough to group "typing a word" into
 * one undo step without pulling in a finer clock). Any new edit clears
 * the redo stack (standard undo/redo semantics). */
static void editorPushUndo(enum undoEditType type) {
    time_t now = time(NULL);
    int coalesce = (type != EDIT_OTHER) &&
        (type == E.last_edit_type) &&
        (now - E.last_edit_time <= UNDO_COALESCE_SECS);

    editorClearRedoStack();
    E.last_edit_type = type;
    E.last_edit_time = now;

    if (coalesce) return;

    if (E.undo_count == UNDO_MAX_DEPTH) {
        editorFreeSnapshot(&E.undo_stack[0]);
        memmove(&E.undo_stack[0], &E.undo_stack[1],
            sizeof(undoSnapshot) * (size_t)(UNDO_MAX_DEPTH - 1));
        E.undo_count--;
    }
    E.undo_stack = realloc(E.undo_stack, sizeof(undoSnapshot) * (size_t)(E.undo_count + 1));
    E.undo_stack[E.undo_count++] = editorMakeSnapshot();
}

/* Replaces the live buffer with the given snapshot's rows/cursor. Does
 * NOT free the snapshot itself -- caller owns that (it's about to be
 * pushed onto the other stack, not discarded). */
static void editorRestoreSnapshot(undoSnapshot *snap) {
    for (int i = 0; i < E.numrows; i++) editorFreeRow(&E.row[i]);
    free(E.row);

    E.numrows = snap->numrows;
    E.row = malloc(sizeof(erow) * (size_t)E.numrows);
    for (int i = 0; i < E.numrows; i++) {
        E.row[i].size = snap->row[i].size;
        E.row[i].rsize = snap->row[i].rsize;
        E.row[i].chars = malloc((size_t)snap->row[i].size + 1);
        memcpy(E.row[i].chars, snap->row[i].chars, (size_t)snap->row[i].size + 1);
        E.row[i].render = malloc((size_t)snap->row[i].rsize + 1);
        memcpy(E.row[i].render, snap->row[i].render, (size_t)snap->row[i].rsize + 1);
    }
    E.cx = snap->cx;
    E.cy = snap->cy;
    if (E.cy > E.numrows) E.cy = E.numrows;
    E.dirty++;
}

static void editorUndo(void) {
    if (E.undo_count == 0) {
        editorSetStatusMessage("Nothing to undo");
        return;
    }
    undoSnapshot current = editorMakeSnapshot();
    E.redo_stack = realloc(E.redo_stack, sizeof(undoSnapshot) * (size_t)(E.redo_count + 1));
    E.redo_stack[E.redo_count++] = current;

    undoSnapshot *top = &E.undo_stack[--E.undo_count];
    editorRestoreSnapshot(top);
    editorFreeSnapshot(top);
    E.undo_stack = realloc(E.undo_stack, sizeof(undoSnapshot) * (size_t)(E.undo_count > 0 ? E.undo_count : 1));
    E.last_edit_type = EDIT_NONE;
    editorSetStatusMessage("Undo");
}

static void editorRedo(void) {
    if (E.redo_count == 0) {
        editorSetStatusMessage("Nothing to redo");
        return;
    }
    undoSnapshot current = editorMakeSnapshot();
    E.undo_stack = realloc(E.undo_stack, sizeof(undoSnapshot) * (size_t)(E.undo_count + 1));
    E.undo_stack[E.undo_count++] = current;

    undoSnapshot *top = &E.redo_stack[--E.redo_count];
    editorRestoreSnapshot(top);
    editorFreeSnapshot(top);
    E.redo_stack = realloc(E.redo_stack, sizeof(undoSnapshot) * (size_t)(E.redo_count > 0 ? E.redo_count : 1));
    E.last_edit_type = EDIT_NONE;
    editorSetStatusMessage("Redo");
}

/* ---- editor operations --------------------------------------------------- */

static void editorInsertChar(int c) {
    editorPushUndo(EDIT_INSERT);
    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

static void editorInsertNewline(void) {
    editorPushUndo(EDIT_OTHER);
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], (size_t)(row->size - E.cx));
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

static void editorDelChar(void) {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    editorPushUndo(EDIT_DELETE);

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        int del_count = 1;
        /* Also delete any UTF-8 continuation bytes right before cx, so
         * Backspace removes the whole multi-byte character in one go. */
        while (del_count < E.cx &&
               ((unsigned char)row->chars[E.cx - del_count] & 0xC0) == 0x80)
            del_count++;
        for (int k = 0; k < del_count; k++)
            editorRowDelChar(row, E.cx - 1 - k);
        E.cx -= del_count;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, (size_t)row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/* ---- file i/o ------------------------------------------------------------- */

static char *editorRowsToString(size_t *buflen) {
    size_t totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += (size_t)E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, (size_t)E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

static void editorSetStatusMessage(const char *fmt, ...);
static void editorRefreshScreen(void);
static int editorReadKey(void);

/* Displays a prompt in the message bar and lets the user type a response
 * with basic line editing (Backspace, Enter, Esc to cancel). Returns a
 * malloc'd string (caller must free), or NULL if the user pressed Esc. */
static char *editorPrompt(const char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = (char)c;
            buf[buflen] = '\0';
        }
    }
}

static void editorOpen(const char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno == ENOENT) return; /* new file */
        die("fopen");
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, (size_t)linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

static void editorSave(void) {
    if (E.filename == NULL) {
        char *name = editorPrompt("Save as: %s (Esc to cancel)");
        if (name == NULL) {
            editorSetStatusMessage("Save aborted.");
            return;
        }
        if (name[0] == '\0') {
            free(name);
            editorSetStatusMessage("Save aborted: empty filename.");
            return;
        }
        E.filename = name;
    }

    size_t len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, (off_t)len) == 0) {
            if (write(fd, buf, len) == (ssize_t)len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%zu bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* ---- append buffer -------------------------------------------------------- */

struct abuf {
    char *b;
    int len;
};

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, (size_t)(ab->len + len));
    if (new == NULL) return;
    memcpy(&new[ab->len], s, (size_t)len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab) { free(ab->b); }

/* ---- output ---------------------------------------------------------------- */

static int editorGetSelection(int *start_y, int *start_x, int *end_y, int *end_x);

static void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

static void editorDrawRows(struct abuf *ab) {
    int sel_y0 = 0, sel_x0 = 0, sel_y1 = 0, sel_x1 = 0;
    int has_sel = editorGetSelection(&sel_y0, &sel_x0, &sel_y1, &sel_x1);

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "tinyedit -- version %s", TE_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;

            if (len > 0) {
                char *line = &E.row[filerow].render[E.coloff];
                int row_sel_start = -1, row_sel_end = -1;
                if (has_sel && filerow >= sel_y0 && filerow <= sel_y1) {
                    row_sel_start = (filerow == sel_y0) ? sel_x0 : 0;
                    row_sel_end = (filerow == sel_y1) ? sel_x1 : E.row[filerow].size;
                }

                int in_sel = 0;
                for (int j = 0; j < len; j++) {
                    int filecol = E.coloff + j;
                    int should_sel = row_sel_start >= 0 &&
                        filecol >= row_sel_start && filecol < row_sel_end;
                    if (should_sel && !in_sel) {
                        abAppend(ab, "\x1b[7m", 4);
                        in_sel = 1;
                    } else if (!should_sel && in_sel) {
                        abAppend(ab, "\x1b[m", 3);
                        in_sel = 0;
                    }
                    abAppend(ab, &line[j], 1);
                }
                if (in_sel) abAppend(ab, "\x1b[m", 3);
            }
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

static void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

static void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

static void editorRefreshScreen(void) {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
        (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, (int)strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

static void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* ---- input ------------------------------------------------------------------ */

static void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) E.cy++;
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/* Word-wise cursor movement for Alt+Left / Alt+Right. Skips whitespace
 * then a run of non-whitespace characters, crossing line boundaries
 * when the cursor is already at the start/end of a line. */
static void editorMoveCursorWord(int forward) {
    if (forward) {
        if (E.cy >= E.numrows) return;
        erow *row = &E.row[E.cy];
        if (E.cx == row->size) {
            if (E.cy < E.numrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            return;
        }
        while (E.cx < row->size && isspace((unsigned char)row->chars[E.cx])) E.cx++;
        while (E.cx < row->size && !isspace((unsigned char)row->chars[E.cx])) E.cx++;
    } else {
        if (E.cx == 0) {
            if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            return;
        }
        erow *row = &E.row[E.cy];
        int i = E.cx - 1;
        while (i > 0 && isspace((unsigned char)row->chars[i])) i--;
        while (i > 0 && !isspace((unsigned char)row->chars[i - 1])) i--;
        E.cx = i;
    }
}

/* Normalizes the selection anchor vs the current cursor position into an
 * ordered [start, end) range. Returns 0 and leaves outputs untouched if
 * there is no active selection. */
static int editorGetSelection(int *start_y, int *start_x, int *end_y, int *end_x) {
    if (!E.sel_active) return 0;

    int ay = E.sel_anchor_y, ax = E.sel_anchor_x;
    int cy = E.cy, cx = E.cx;

    if (ay < cy || (ay == cy && ax <= cx)) {
        *start_y = ay; *start_x = ax;
        *end_y = cy; *end_x = cx;
    } else {
        *start_y = cy; *start_x = cx;
        *end_y = ay; *end_x = ax;
    }
    return 1;
}

/* Serializes the given [start_y,start_x) .. [end_y,end_x) half-open range
 * into a malloc'd NUL-terminated buffer, joining lines with '\n'.
 * *outlen receives the length excluding the NUL terminator. */
static char *editorSerializeRange(int start_y, int start_x, int end_y, int end_x, size_t *outlen) {
    size_t totlen = 0;
    for (int y = start_y; y <= end_y; y++) {
        int from = (y == start_y) ? start_x : 0;
        int to = (y == end_y) ? end_x : E.row[y].size;
        if (to > from) totlen += (size_t)(to - from);
        if (y != end_y) totlen += 1;
    }

    char *buf = malloc(totlen + 1);
    char *p = buf;
    for (int y = start_y; y <= end_y; y++) {
        int from = (y == start_y) ? start_x : 0;
        int to = (y == end_y) ? end_x : E.row[y].size;
        if (to > from) {
            memcpy(p, &E.row[y].chars[from], (size_t)(to - from));
            p += to - from;
        }
        if (y != end_y) {
            *p = '\n';
            p++;
        }
    }
    *p = '\0';
    *outlen = totlen;
    return buf;
}

/* Deletes the given [start_y,start_x) .. [end_y,end_x) half-open range from
 * the buffer and leaves the cursor at start_y,start_x. */
static void editorDeleteRange(int start_y, int start_x, int end_y, int end_x) {
    editorPushUndo(EDIT_OTHER);
    if (start_y == end_y) {
        erow *row = &E.row[start_y];
        for (int i = 0; i < end_x - start_x; i++)
            editorRowDelChar(row, start_x);
    } else {
        erow *first = &E.row[start_y];
        first->size = start_x;
        first->chars[first->size] = '\0';

        erow *last = &E.row[end_y];
        editorRowAppendString(first, &last->chars[end_x], (size_t)(last->size - end_x));
        editorUpdateRow(first);

        for (int y = end_y; y > start_y; y--)
            editorDelRow(y);
    }
    E.cy = start_y;
    E.cx = start_x;
    E.dirty++;
}

/* Inserts `text` (which may contain '\n') at the current cursor position,
 * splitting into new rows as needed. Leaves the cursor at the end of the
 * inserted text. */
static void editorInsertText(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n')
            editorInsertNewline();
        else
            editorInsertChar((unsigned char)text[i]);
    }
}

static void editorProcessKeypress(void) {
    static int quit_times = TE_QUIT_TIMES;

    int c = editorReadKey();

    if (c != SHIFT_ARROW_UP && c != SHIFT_ARROW_DOWN &&
        c != SHIFT_ARROW_LEFT && c != SHIFT_ARROW_RIGHT &&
        c != CTRL_KEY('a') && c != CTRL_KEY('c') &&
        c != CTRL_KEY('x') && c != CTRL_KEY('v'))
        E.sel_active = 0;

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage(
                    "WARNING! Unsaved changes. Press Ctrl-Q %d more time(s) to quit without saving.",
                    quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case CTRL_KEY('a'):
            if (E.numrows > 0) {
                E.sel_active = 1;
                E.sel_anchor_y = 0;
                E.sel_anchor_x = 0;
                E.cy = E.numrows - 1;
                E.cx = E.row[E.numrows - 1].size;
            }
            break;

        case CTRL_KEY('c'):
        case CTRL_KEY('x'): {
            int sy, sx, ey, ex;
            if (editorGetSelection(&sy, &sx, &ey, &ex)) {
                size_t len;
                char *text = editorSerializeRange(sy, sx, ey, ex, &len);
                clipboardCopy(text, len);
                if (c == CTRL_KEY('x')) {
                    editorDeleteRange(sy, sx, ey, ex);
                    editorSetStatusMessage("%zu bytes cut", len);
                } else {
                    editorSetStatusMessage("%zu bytes copied", len);
                }
                free(text);
                E.sel_active = 0;
            }
            break;
        }

        case CTRL_KEY('v'): {
            int sy, sx, ey, ex;
            if (editorGetSelection(&sy, &sx, &ey, &ex)) {
                editorDeleteRange(sy, sx, ey, ex);
                E.sel_active = 0;
            }
            size_t len;
            char *text = clipboardPaste(&len);
            if (text) {
                editorInsertText(text, len);
                clipboardFree(text);
                editorSetStatusMessage("pasted");
            }
            break;
        }

        case CTRL_KEY('z'):
            editorUndo();
            break;
        case CTRL_KEY('y'):
            editorRedo();
            break;

        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }
            int times = E.screenrows;
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            E.sel_active = 0;
            editorMoveCursor(c);
            break;

        case SHIFT_ARROW_UP:
        case SHIFT_ARROW_DOWN:
        case SHIFT_ARROW_LEFT:
        case SHIFT_ARROW_RIGHT: {
            if (!E.sel_active) {
                E.sel_active = 1;
                E.sel_anchor_x = E.cx;
                E.sel_anchor_y = E.cy;
            }
            int plain = (c == SHIFT_ARROW_UP) ? ARROW_UP :
                        (c == SHIFT_ARROW_DOWN) ? ARROW_DOWN :
                        (c == SHIFT_ARROW_LEFT) ? ARROW_LEFT : ARROW_RIGHT;
            editorMoveCursor(plain);
            if (E.sel_anchor_x == E.cx && E.sel_anchor_y == E.cy)
                E.sel_active = 0;
            break;
        }

        case ALT_ARROW_LEFT:
            editorMoveCursorWord(0);
            break;
        case ALT_ARROW_RIGHT:
            editorMoveCursorWord(1);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = TE_QUIT_TIMES;
}

/* ---- init ------------------------------------------------------------------- */

static void editorFreeUndoRedo(void) {
    for (int i = 0; i < E.undo_count; i++) editorFreeSnapshot(&E.undo_stack[i]);
    free(E.undo_stack);
    E.undo_stack = NULL;
    E.undo_count = 0;
    editorClearRedoStack();
    free(E.redo_stack);
    E.redo_stack = NULL;
}

static void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.sel_active = 0;
    E.sel_anchor_x = 0;
    E.sel_anchor_y = 0;

    E.undo_stack = NULL;
    E.undo_count = 0;
    E.redo_stack = NULL;
    E.redo_count = 0;
    E.last_edit_type = EDIT_NONE;
    E.last_edit_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2; /* status bar + message bar */
}

int main(int argc, char **argv) {
    enableRawMode();
    initEditor();
    atexit(editorFreeUndoRedo);
    if (argc >= 2) editorOpen(argv[1]);

    editorSetStatusMessage("Ctrl-S save | Ctrl-Q quit | Ctrl-Z undo | Ctrl-Y redo");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
