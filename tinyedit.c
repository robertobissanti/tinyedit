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

#include "tinyedit.h"
#include "clipboard.h"
#include "utf8.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---- globals ------------------------------------------------------------ */

static struct editorConfig E;
static struct editorSettings S;

/* Set by the SIGWINCH handler when the terminal window is resized.
 * sig_atomic_t is the only type C guarantees is safe to write from a
 * signal handler and read from the main loop without a data race; the
 * actual resize handling (re-reading dimensions, redrawing) happens
 * in the main loop, not in the handler itself. */
static volatile sig_atomic_t winsize_changed = 0;

/* Set by editorFindCallback() when Ctrl-R is pressed inside the Ctrl-F
 * search prompt, telling editorPromptCB()'s loop to return immediately
 * so editorFind() can hand off to editorFindAndReplace(). */
static int search_switch_to_replace;

static int search_saved_cx, search_saved_cy, search_saved_rowoff, search_saved_coloff;
static int search_dir = 1; /* 1 = forward, -1 = backward */

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

static void handleWinch(int sig) {
    (void)sig;
    winsize_changed = 1;
}

static void enableResizeHandling(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleWinch;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART: we want read() in editorReadKey() to return EINTR
     * on resize so the main loop can react immediately instead of
     * blocking until the next keypress. */
    sigaction(SIGWINCH, &sa, NULL);
}

static int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = (int)read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno == EINTR && winsize_changed)
            return CTRL_KEY('l'); /* no-op key: lets the main loop redraw */
        if (nread == -1 && errno != EAGAIN && errno != EINTR) die("read");
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
                    /* Modified nav key. Two layouts share this prefix:
                     *   ESC [ 1 ; <mod> <letter>   e.g. Alt+Up = ESC[1;3A
                     *   ESC [ 5 ; <mod> ~          Shift+PageUp = ESC[5;2~
                     *   ESC [ 6 ; <mod> ~          Shift+PageDown = ESC[6;2~
                     * seq[1] tells us which: '1' terminates with a
                     * letter, '5'/'6' terminate with '~'. */
                    char mod, term;
                    if (read(STDIN_FILENO, &mod, 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &term, 1) != 1) return '\x1b';
                    int is_alt = (mod == '3');
                    int is_shift = (mod == '2');

                    if (seq[1] == '5' && term == '~')
                        return is_shift ? SHIFT_PAGE_UP : PAGE_UP;
                    if (seq[1] == '6' && term == '~')
                        return is_shift ? SHIFT_PAGE_DOWN : PAGE_DOWN;

                    switch (term) {
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
                case 'P': return F1_KEY; /* SS3 F1, ESC O P -- verified on Ghostty and Terminal.app */
                case 'Q': return F2_KEY; /* SS3 F2, e.g. ESC O Q on Ghostty */
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
    int j = 0;
    while (j < cx) {
        if (row->chars[j] == '\t') {
            rx += (S.tab_stop - 1) - (rx % S.tab_stop);
            rx++;
            j++;
            continue;
        }
        size_t clen = utf8NextCharLen(row->chars, (size_t)j, (size_t)row->size);
        if (clen == 0) clen = 1;
        rx += utf8SingleCharWidth(row->chars + j, clen);
        j += (int)clen;
    }
    return rx;
}

static void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc((size_t)(row->size + tabs * (S.tab_stop - 1) + 1));

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % S.tab_stop != 0) row->render[idx++] = ' ';
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

    if (E.undo_count >= S.undo_max_depth) {
        editorFreeSnapshot(&E.undo_stack[0]);
        memmove(&E.undo_stack[0], &E.undo_stack[1],
            sizeof(undoSnapshot) * (size_t)(E.undo_count - 1));
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
        /* Delete the whole grapheme cluster before cx (base character
         * plus any joined modifiers/marks), not just one byte or one
         * codepoint, so Backspace removes e.g. an emoji with a
         * skin-tone modifier in a single press. */
        size_t del_count = utf8PrevCharLen(row->chars, (size_t)E.cx);
        if (del_count == 0) del_count = 1;
        for (size_t k = 0; k < del_count; k++)
            editorRowDelChar(row, E.cx - 1 - (int)k);
        E.cx -= (int)del_count;
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
 * malloc'd string (caller must free), or NULL if the user pressed Esc.
 *
 * If callback is non-NULL, it is invoked after every keystroke (including
 * the initial empty buffer) as callback(buf, key), so callers can drive
 * live side effects such as incremental-search highlighting. The callback
 * is also invoked once more with key == '\r' or '\x1b' right before the
 * prompt returns, so it can do final cleanup/confirmation. */
static char *editorPromptCB(const char *prompt, void (*callback)(char *, int)) {
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
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
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

        if (callback) callback(buf, c);
        if (search_switch_to_replace) {
            editorSetStatusMessage("");
            return buf;
        }
    }
}

static char *editorPrompt(const char *prompt) {
    return editorPromptCB(prompt, NULL);
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

/* Width of the left-hand line-number gutter, including one space of
 * padding before the text starts. Zero when gutter is disabled. Grows
 * with E.numrows so files with 1000+ lines still right-align cleanly. */
static int editorGutterWidth(void) {
    if (!S.show_line_numbers) return 0;
    int digits = 3;
    int n = E.numrows;
    while (n >= 1000) {
        digits++;
        n /= 10;
    }
    return digits + 1;
}

/* Usable text area width: total screen columns minus the gutter. */
static int editorTextCols(void) {
    int cols = E.screencols - editorGutterWidth();
    return cols > 0 ? cols : 0;
}

static void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    int textcols = editorTextCols();
    if (E.rx >= E.coloff + textcols) E.coloff = E.rx - textcols + 1;
}

static void editorDrawRows(struct abuf *ab) {
    int sel_y0 = 0, sel_x0 = 0, sel_y1 = 0, sel_x1 = 0;
    int has_sel = editorGetSelection(&sel_y0, &sel_x0, &sel_y1, &sel_x1);
    int gutter = editorGutterWidth();
    int textcols = editorTextCols();

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;

        if (gutter > 0) {
            char numbuf[16];
            if (filerow < E.numrows) {
                snprintf(numbuf, sizeof(numbuf), "%*d ", gutter - 1, filerow + 1);
            } else {
                snprintf(numbuf, sizeof(numbuf), "%*s ", gutter - 1, "");
            }
            const char *gutter_color = ansiColorCode(S.color_gutter);
            abAppend(ab, gutter_color, (int)strlen(gutter_color));
            abAppend(ab, numbuf, gutter);
            abAppend(ab, "\x1b[m", 3);
        }

        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "tinyedit -- version %s", TE_VERSION);
                if (welcomelen > textcols) welcomelen = textcols;
                int padding = (textcols - welcomelen) / 2;
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
            if (len > textcols) len = textcols;

            if (len > 0) {
                char *line = &E.row[filerow].render[E.coloff];
                int row_sel_start = -1, row_sel_end = -1;
                if (has_sel && filerow >= sel_y0 && filerow <= sel_y1) {
                    row_sel_start = (filerow == sel_y0) ? sel_x0 : 0;
                    row_sel_end = (filerow == sel_y1) ? sel_x1 : E.row[filerow].size;
                }

                int match_start = -1, match_end = -1;
                if (E.search_match_y == filerow) {
                    match_start = E.search_match_x;
                    match_end = E.search_match_x + E.search_match_len;
                }

                int in_sel = 0;
                for (int j = 0; j < len; j++) {
                    int filecol = E.coloff + j;
                    int should_sel = (row_sel_start >= 0 &&
                        filecol >= row_sel_start && filecol < row_sel_end) ||
                        (match_start >= 0 && filecol >= match_start && filecol < match_end);
                    if (should_sel && !in_sel) {
                        const char *sel_color = ansiColorCode(S.color_selection);
                        abAppend(ab, sel_color, (int)strlen(sel_color));
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

/* Counts grapheme clusters (not bytes, not raw codepoints) across the
 * whole buffer, using the same utf8NextCharLen() boundary logic as
 * cursor movement -- so an emoji with a skin-tone modifier counts as
 * one character here too, consistent with how it's edited/deleted as
 * one unit elsewhere in the editor. Newlines between rows count as one
 * character each, matching how the file is written to disk
 * (editorRowsToString() joins rows with '\n'). */
static int editorCountChars(void) {
    int count = 0;
    for (int i = 0; i < E.numrows; i++) {
        erow *row = &E.row[i];
        size_t pos = 0;
        while (pos < (size_t)row->size) {
            size_t clen = utf8NextCharLen(row->chars, pos, (size_t)row->size);
            if (clen == 0) clen = 1;
            pos += clen;
            count++;
        }
        if (i < E.numrows - 1) count++; /* newline joining this row to the next */
    }
    return count;
}

/* Filetype name for the status bar, derived from E.filename's
 * extension (e.g. "tinyedit.c" -> "C"), via the built-in table plus
 * any ~/.tinyeditrc "filetype.<ext> = <Name>" overrides. Returns NULL
 * if there's no filename, no extension, or the extension is unknown --
 * callers should omit the field entirely rather than show a blank. */
static const char *editorFiletypeLabel(void) {
    if (!E.filename) return NULL;
    const char *dot = strrchr(E.filename, '.');
    /* No dot, or a dot with nothing after it (e.g. "Makefile",
     * "foo."): no extension to look up. A leading dot with no other
     * dot (e.g. ".gitignore") also has no meaningful extension. */
    if (!dot || dot[1] == '\0' || dot == E.filename) return NULL;
    return filetypeForExtension(dot + 1);
}

static void editorDrawStatusBar(struct abuf *ab) {
    const char *bar_color = ansiColorCode(S.color_statusbar);
    abAppend(ab, bar_color, (int)strlen(bar_color));
    abAppend(ab, "\x1b[7m", 4);
    char status[96], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines, %d chars %s",
        E.filename ? E.filename : "[No Name]", E.numrows, editorCountChars(),
        E.dirty ? "(modified)" : "");

    const char *filetype = editorFiletypeLabel();
    int rlen;
    if (filetype)
        rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
            filetype, E.cy + 1, E.numrows);
    else
        rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
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
    if (msglen && (E.statusmsg_sticky || time(NULL) - E.statusmsg_time < 5))
        abAppend(ab, E.statusmsg, msglen);
}

static void editorRefreshScreen(void) {
    int need_full_clear = 0;
    if (winsize_changed) {
        winsize_changed = 0;
        int rows, cols;
        if (getWindowSize(&rows, &cols) == 0) {
            E.screenrows = rows - 2; /* status bar + message bar */
            E.screencols = cols;
        }
        /* Shrinking the terminal can leave old rows visible past the
         * new, smaller screenrows -- per-line \x1b[K only clears up to
         * the end of each line we redraw, not rows outside the new
         * viewport entirely, so a full clear is needed here. */
        need_full_clear = 1;
    }

    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    if (need_full_clear) abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
        (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + editorGutterWidth() + 1);
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
    E.statusmsg_sticky = 0;
}

/* Like editorSetStatusMessage(), but the message stays in the message
 * bar until replaced by another call to either function -- no 5s
 * timeout. Used for the startup shortcut hint, which should remain
 * visible until the user does something that produces a real status
 * update, not vanish on its own after a few seconds. */
static void editorSetStatusMessageSticky(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
    E.statusmsg_sticky = 1;
}

/* ---- input ------------------------------------------------------------------ */

static void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                size_t back = utf8PrevCharLen(row->chars, (size_t)E.cx);
                E.cx -= (back > 0) ? (int)back : 1;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                size_t fwd = utf8NextCharLen(row->chars, (size_t)E.cx, (size_t)row->size);
                E.cx += (fwd > 0) ? (int)fwd : 1;
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

/* ---- search / replace ------------------------------------------------------- */

static void editorFindAndReplace(const char *query);

/* Searches for `query` starting at (from_y, from_x), moving in `dir`
 * (1 forward, -1 backward), wrapping around the whole file. On success
 * sets E.cy/E.cx to the match start, updates E.search_match_*, and returns
 * 1. On failure clears E.search_match_y to -1 and returns 0. */
static int editorFindFrom(const char *query, int from_y, int from_x, int dir) {
    size_t qlen = strlen(query);
    if (qlen == 0 || E.numrows == 0) {
        E.search_match_y = -1;
        return 0;
    }

    int y = from_y;
    int x = from_x;

    for (int steps = 0; steps <= E.numrows; steps++) {
        erow *row = &E.row[y];
        char *match = NULL;

        if (dir == 1) {
            if (x <= row->size) match = strstr(&row->chars[x], query);
        } else {
            /* Backward: scan for the last match starting at or before
             * column x on this row. */
            int limit = x;
            if (limit > row->size - (int)qlen) limit = row->size - (int)qlen;
            for (int i = 0; i <= limit; i++) {
                if (memcmp(&row->chars[i], query, qlen) == 0)
                    match = &row->chars[i];
            }
        }

        if (match) {
            int mx = (int)(match - row->chars);
            E.cy = y;
            E.cx = mx;
            E.search_match_y = y;
            E.search_match_x = mx;
            E.search_match_len = (int)qlen;
            return 1;
        }

        if (dir == 1) {
            y = (y + 1) % E.numrows;
            x = 0;
        } else {
            y = (y - 1 + E.numrows) % E.numrows;
            x = E.row[y].size;
        }
    }

    E.search_match_y = -1;
    return 0;
}

static void editorFindCallback(char *query, int key) {
    static int last_cy = -1, last_cx = -1;

    if (key == '\r' || key == '\x1b') {
        if (key == '\x1b') {
            E.cx = search_saved_cx;
            E.cy = search_saved_cy;
            E.rowoff = search_saved_rowoff;
            E.coloff = search_saved_coloff;
        }
        E.search_match_y = -1;
        last_cy = -1;
        last_cx = -1;
        return;
    }

    if (key == CTRL_KEY('r')) {
        search_switch_to_replace = 1;
        return;
    }

    if (key == ARROW_DOWN || key == ARROW_RIGHT) {
        search_dir = 1;
    } else if (key == ARROW_UP || key == ARROW_LEFT) {
        search_dir = -1;
    } else {
        search_dir = 1;
        last_cy = -1;
        last_cx = -1;
    }

    if (strlen(query) == 0) {
        E.search_match_y = -1;
        return;
    }

    int from_y, from_x;
    if (last_cy == -1) {
        from_y = search_saved_cy;
        from_x = search_saved_cx;
    } else if (search_dir == 1) {
        from_y = last_cy;
        from_x = last_cx + 1;
    } else {
        from_y = last_cy;
        from_x = last_cx - 1;
        if (from_x < 0) from_x = 0;
    }

    if (editorFindFrom(query, from_y, from_x, search_dir)) {
        last_cy = E.cy;
        last_cx = E.cx;
    }
}

static void editorFind(void) {
    search_saved_cx = E.cx;
    search_saved_cy = E.cy;
    search_saved_rowoff = E.rowoff;
    search_saved_coloff = E.coloff;
    search_dir = 1;

    search_switch_to_replace = 0;
    char *query = editorPromptCB(
        "Search (Esc to cancel, Arrows to jump, Ctrl-R to replace): %s",
        editorFindCallback);

    if (search_switch_to_replace && query) {
        editorFindAndReplace(query);
    }

    if (query) free(query);
}

/* Search+replace, bound to Ctrl-R while inside the Ctrl-F search prompt
 * (rather than Ctrl-Shift-F, whose byte sequence is indistinguishable from
 * plain Ctrl-F on most raw ttys). Prompts for a search term via the normal
 * incremental-search callback, then for a replacement string, then walks
 * matches one at a time offering y/n/a (yes/no/all). */
static void editorFindAndReplace(const char *query) {
    if (!query || query[0] == '\0') return;

    char replace_prompt[96];
    snprintf(replace_prompt, sizeof(replace_prompt), "Replace \"%.40s\" with: %%s", query);
    search_switch_to_replace = 0;
    char *replacement = editorPrompt(replace_prompt);
    if (!replacement) return;

    size_t qlen = strlen(query);
    size_t rlen = strlen(replacement);
    int all = 0;
    int count = 0;

    int y = search_saved_cy, x = search_saved_cx;
    while (editorFindFrom(query, y, x, 1)) {
        y = E.search_match_y;
        x = E.search_match_x;

        int do_replace = all;
        if (!all) {
            editorSetStatusMessage(
                "Replace this occurrence? y/n/a(ll)/q(uit)");
            editorRefreshScreen();
            int c = editorReadKey();
            if (c == 'q' || c == '\x1b') break;
            if (c == 'a') { all = 1; do_replace = 1; }
            else if (c == 'y') do_replace = 1;
            else do_replace = 0;
        }

        if (do_replace) {
            erow *row = &E.row[y];
            for (size_t k = 0; k < qlen; k++)
                editorRowDelChar(row, x);
            if (rlen > 0) {
                char *tmp = malloc(rlen + 1);
                memcpy(tmp, replacement, rlen);
                tmp[rlen] = '\0';
                for (size_t k = 0; k < rlen; k++)
                    editorRowInsertChar(row, x + (int)k, tmp[k]);
                free(tmp);
            }
            count++;
            x += (int)rlen;
        } else {
            x += (int)qlen;
        }
    }

    E.search_match_y = -1;
    free(replacement);
    editorSetStatusMessage("Replaced %d occurrence(s).", count);
}

/* ---- settings screen (F2) --------------------------------------------------- */

static int *settingsScreenSlot(struct editorSettings *s, const struct settingDescriptor *d) {
    return (int *)((char *)s + d->offset);
}

static void editorSettingsDrawRow(struct abuf *ab, int idx, int selected,
    const struct editorSettings *edited) {
    const struct settingDescriptor *d = &settingDescriptors[idx];
    const int *slot = (const int *)((const char *)edited + d->offset);

    char line[96];
    char valuebuf[48];

    if (d->type == SETTING_BOOL) {
        snprintf(valuebuf, sizeof(valuebuf), "%s", *slot ? "on" : "off");
    } else if (d->type == SETTING_INT) {
        snprintf(valuebuf, sizeof(valuebuf), "%d", *slot);
    } else {
        snprintf(valuebuf, sizeof(valuebuf), "%s", d->enum_names[*slot]);
    }

    int len = snprintf(line, sizeof(line), "  %-22s %s", d->label, valuebuf);
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(line)) len = (int)sizeof(line) - 1;

    if (selected) abAppend(ab, "\x1b[7m", 4);
    abAppend(ab, line, len);
    if (selected) abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\x1b[K\r\n", 5);
}

/* Full-screen settings overlay (F2). Edits a local copy of the live
 * settings so Esc can discard changes cleanly; Ctrl-S writes the copy
 * to ~/.tinyeditrc and makes it live. Reuses the same raw-mode input
 * loop style as the rest of the editor (editorReadKey + a per-frame
 * abuf redraw) rather than pulling in any new input machinery. */
/* Static keybinding reference shown by F1. One entry per line; NULL
 * marks a section header (rendered bold/inverse instead of key+desc).
 * Kept as a flat array rather than scattered doc-comments so this is
 * the one place to update when a keybinding changes -- easy to miss
 * a case in editorProcessKeypress() otherwise. */
struct helpEntry { const char *key; const char *desc; };
static const struct helpEntry helpEntries[] = {
    { NULL, "Movement" },
    { "Arrows, Home, End, PageUp/Down", "Move cursor" },
    { "Alt+Left/Right (or Esc b / Esc f)", "Jump by word" },
    { NULL, "Editing" },
    { "Enter", "New line" },
    { "Backspace / Delete", "Delete character (UTF-8 aware)" },
    { "Ctrl-Z / Ctrl-Y", "Undo / redo" },
    { NULL, "Selection & clipboard" },
    { "Shift+Arrows, Shift+PageUp/Down", "Extend selection" },
    { "Ctrl-T", "Toggle selection mode (works on every terminal)" },
    { "Ctrl-A", "Select all" },
    { "Ctrl-C / Ctrl-X / Ctrl-V", "Copy / cut / paste (system clipboard)" },
    { NULL, "Search" },
    { "Ctrl-F", "Incremental search" },
    { "Ctrl-R (inside search)", "Switch to search & replace" },
    { NULL, "File & editor" },
    { "Ctrl-S", "Save" },
    { "Ctrl-Q", "Quit (asks twice if unsaved)" },
    { "F2", "Settings panel" },
    { "F1", "This help screen" },
};
static const int helpEntryCount = (int)(sizeof(helpEntries) / sizeof(helpEntries[0]));

/* Full-screen static help overlay (F1). No editable state, so unlike
 * editorSettingsScreen() this doesn't need a local copy or Ctrl-S --
 * any key closes it. Scrolls with Up/Down/PageUp/PageDown if the
 * keybinding list is taller than the terminal. */
static void editorHelpScreen(void) {
    int scroll = 0;

    while (1) {
        struct abuf ab = ABUF_INIT;
        abAppend(&ab, "\x1b[?25l\x1b[H", 9);
        int rows_used = 0;

        abAppend(&ab, "\x1b[7m tinyedit -- keybindings (any key to close) \x1b[m\x1b[K\r\n\x1b[K\r\n", 47);
        rows_used += 2;

        for (int i = scroll; i < helpEntryCount && rows_used < E.screenrows; i++) {
            if (helpEntries[i].key == NULL) {
                abAppend(&ab, "\x1b[1m  ", 5);
                abAppend(&ab, helpEntries[i].desc, (int)strlen(helpEntries[i].desc));
                abAppend(&ab, "\x1b[m\x1b[K\r\n", 8);
            } else {
                char line[128];
                int len = snprintf(line, sizeof(line), "    %-38s %s",
                    helpEntries[i].key, helpEntries[i].desc);
                if (len < 0) len = 0;
                if ((size_t)len >= sizeof(line)) len = (int)sizeof(line) - 1;
                abAppend(&ab, line, len);
                abAppend(&ab, "\x1b[K\r\n", 5);
            }
            rows_used++;
        }

        int total_rows = E.screenrows + 2;
        for (; rows_used < total_rows - 1; rows_used++)
            abAppend(&ab, "\x1b[K\r\n", 5);
        if (rows_used < total_rows)
            abAppend(&ab, "\x1b[K", 3);

        abAppend(&ab, "\x1b[H\x1b[?25h", 9);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        int c = editorReadKey();
        int max_scroll = helpEntryCount - (E.screenrows - 2);
        if (max_scroll < 0) max_scroll = 0;

        if (c == ARROW_DOWN) {
            if (scroll < max_scroll) scroll++;
        } else if (c == ARROW_UP) {
            if (scroll > 0) scroll--;
        } else if (c == PAGE_DOWN) {
            scroll += E.screenrows;
            if (scroll > max_scroll) scroll = max_scroll;
        } else if (c == PAGE_UP) {
            scroll -= E.screenrows;
            if (scroll < 0) scroll = 0;
        } else {
            return; /* any other key closes the help screen */
        }
    }
}

static void editorSettingsScreen(void) {
    struct editorSettings edited = S;
    int cursor = 0;
    char msg[80] = "";

    while (1) {
        struct abuf ab = ABUF_INIT;
        abAppend(&ab, "\x1b[?25l\x1b[H", 9);
        int rows_used = 0;

        abAppend(&ab, "\x1b[7m Settings \x1b[m\x1b[K\r\n\x1b[K\r\n", 27);
        rows_used += 2;

        for (int i = 0; i < settingDescriptorCount; i++) {
            editorSettingsDrawRow(&ab, i, i == cursor, &edited);
            rows_used++;
        }

        abAppend(&ab, "\x1b[K\r\n", 5);
        rows_used++;
        if (edited.redo_key == REDO_KEY_CTRL_SHIFT_Z) {
            const char *note =
                "  Note: Ctrl-Shift-Z may not reach the editor on every "
                "terminal; Ctrl-Y always works as a fallback.\x1b[K\r\n";
            abAppend(&ab, note, (int)strlen(note));
            rows_used++;
        }

        char help[96];
        int hlen = snprintf(help, sizeof(help),
            "  %s", msg[0] ? msg :
            "Up/Down select, Enter/Space edit, Ctrl-S save, Esc cancel");
        abAppend(&ab, help, hlen);
        abAppend(&ab, "\x1b[K\r\n", 5);
        rows_used++;

        /* Clear every remaining screen row so stale buffer content from
         * the previous editorRefreshScreen() frame doesn't show through
         * underneath the panel. Total rows written (2 header + options +
         * blank/note/help + this padding) must equal the terminal height
         * exactly -- one \r\n too many scrolls the screen and desyncs
         * \x1b[H from the top of the visible viewport on every frame. */
        int total_rows = E.screenrows + 2;
        for (; rows_used < total_rows - 1; rows_used++)
            abAppend(&ab, "\x1b[K\r\n", 5);
        if (rows_used < total_rows)
            abAppend(&ab, "\x1b[K", 3); /* last row: no trailing newline */

        abAppend(&ab, "\x1b[H\x1b[?25h", 9);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        msg[0] = '\0';

        int c = editorReadKey();
        const struct settingDescriptor *d = &settingDescriptors[cursor];
        int *slot = settingsScreenSlot(&edited, d);

        switch (c) {
            case ARROW_UP:
                cursor = (cursor > 0) ? cursor - 1 : settingDescriptorCount - 1;
                break;
            case ARROW_DOWN:
                cursor = (cursor + 1) % settingDescriptorCount;
                break;

            case '\r':
            case ' ':
                if (d->type == SETTING_BOOL) {
                    *slot = !*slot;
                } else if (d->type == SETTING_ENUM) {
                    *slot = (*slot + 1) % d->enum_count;
                } else { /* SETTING_INT: prompt for a new value */
                    char prompt[64];
                    snprintf(prompt, sizeof(prompt), "%s (%d-%d): %%s",
                        d->label, d->int_min, d->int_max);
                    char *input = editorPrompt(prompt);
                    if (input) {
                        int v = atoi(input);
                        if (v < d->int_min) v = d->int_min;
                        if (v > d->int_max) v = d->int_max;
                        *slot = v;
                        free(input);
                    }
                }
                break;

            case CTRL_KEY('s'):
                S = edited;
                if (settingsSave(&S)) {
                    editorSetStatusMessage("Settings saved to ~/.tinyeditrc");
                } else {
                    editorSetStatusMessage("Could not write ~/.tinyeditrc");
                }
                return;

            case '\x1b':
                return;

            default:
                break;
        }
    }
}

static void editorProcessKeypress(void) {
    static int quit_times = TE_QUIT_TIMES;

    int c = editorReadKey();

    int is_plain_arrow = (c == ARROW_UP || c == ARROW_DOWN ||
        c == ARROW_LEFT || c == ARROW_RIGHT || c == PAGE_UP || c == PAGE_DOWN);

    if (c != SHIFT_ARROW_UP && c != SHIFT_ARROW_DOWN &&
        c != SHIFT_ARROW_LEFT && c != SHIFT_ARROW_RIGHT &&
        c != SHIFT_PAGE_UP && c != SHIFT_PAGE_DOWN &&
        c != CTRL_KEY('a') && c != CTRL_KEY('c') &&
        c != CTRL_KEY('x') && c != CTRL_KEY('v') &&
        c != CTRL_KEY('t') &&
        !(E.sel_pinned && is_plain_arrow))
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
            /* Ctrl-Y always works as redo regardless of the configured
             * redo_key: Ctrl-Shift-Z is frequently indistinguishable
             * from plain Ctrl-Z on a raw tty (see TODO.md), so Ctrl-Y
             * remains a reliable fallback even when the user picked
             * ctrl-shift-z in settings. */
            editorRedo();
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case F1_KEY:
            editorHelpScreen();
            break;

        case F2_KEY:
            editorSettingsScreen();
            break;

        case CTRL_KEY('t'):
            /* Universal selection toggle: works on every terminal, even
             * ones (e.g. Terminal.app on macOS) that can't report
             * Shift+Arrow as a distinct sequence from a plain arrow. */
            E.sel_pinned = !E.sel_pinned;
            if (E.sel_pinned) {
                E.sel_active = 1;
                E.sel_anchor_x = E.cx;
                E.sel_anchor_y = E.cy;
                editorSetStatusMessage("Selection mode ON (arrows extend, Ctrl-T to stop)");
            } else {
                E.sel_active = 0;
                editorSetStatusMessage("Selection mode off");
            }
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
        case PAGE_DOWN:
        case SHIFT_PAGE_UP:
        case SHIFT_PAGE_DOWN: {
            int is_up = (c == PAGE_UP || c == SHIFT_PAGE_UP);
            int extending = (c == SHIFT_PAGE_UP || c == SHIFT_PAGE_DOWN || E.sel_pinned);

            if (extending && !E.sel_active) {
                E.sel_active = 1;
                E.sel_anchor_x = E.cx;
                E.sel_anchor_y = E.cy;
            }

            if (is_up) {
                E.cy = E.rowoff;
            } else {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }
            int times = E.screenrows;
            while (times--)
                editorMoveCursor(is_up ? ARROW_UP : ARROW_DOWN);

            if (extending && E.sel_anchor_x == E.cx && E.sel_anchor_y == E.cy)
                E.sel_active = 0;
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            if (!E.sel_pinned) E.sel_active = 0;
            /* fall through: when sel_pinned is set, a plain arrow
             * extends the selection exactly like Shift+Arrow does. */
        case SHIFT_ARROW_UP:
        case SHIFT_ARROW_DOWN:
        case SHIFT_ARROW_LEFT:
        case SHIFT_ARROW_RIGHT: {
            int extending = (c == SHIFT_ARROW_UP || c == SHIFT_ARROW_DOWN ||
                c == SHIFT_ARROW_LEFT || c == SHIFT_ARROW_RIGHT || E.sel_pinned);

            if (extending && !E.sel_active) {
                E.sel_active = 1;
                E.sel_anchor_x = E.cx;
                E.sel_anchor_y = E.cy;
            }

            int plain = (c == SHIFT_ARROW_UP || c == ARROW_UP) ? ARROW_UP :
                        (c == SHIFT_ARROW_DOWN || c == ARROW_DOWN) ? ARROW_DOWN :
                        (c == SHIFT_ARROW_LEFT || c == ARROW_LEFT) ? ARROW_LEFT : ARROW_RIGHT;
            editorMoveCursor(plain);

            if (extending && E.sel_anchor_x == E.cx && E.sel_anchor_y == E.cy)
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
            break;

        case '\x1b':
            /* Esc also turns off pinned selection mode (Ctrl-T), not
             * just the current selection -- otherwise the next arrow
             * press would silently start a new selection again, since
             * sel_pinned would still be set. */
            E.sel_pinned = 0;
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
    E.statusmsg_sticky = 0;
    E.sel_active = 0;
    E.sel_anchor_x = 0;
    E.sel_anchor_y = 0;
    E.sel_pinned = 0;
    E.search_match_y = -1;
    E.search_match_x = 0;
    E.search_match_len = 0;

    settingsLoad(&S);

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
    enableResizeHandling();
    initEditor();
    atexit(editorFreeUndoRedo);
    if (argc >= 2) editorOpen(argv[1]);

    /* Terminal.app on macOS sends the same byte sequence for a plain
     * arrow and Shift+Arrow, so text selection via Shift+Arrow silently
     * does nothing there -- not a bug, a limitation of that terminal
     * (see CLAUDE.md). Point users at the universal Ctrl-T fallback
     * instead of leaving them to wonder why Shift+Arrow is unresponsive. */
    const char *term_program = getenv("TERM_PROGRAM");
    if (term_program && strcmp(term_program, "Apple_Terminal") == 0) {
        editorSetStatusMessageSticky(
            "Terminal.app: use Ctrl-T to select (Shift+Arrow unsupported here) | F1 help");
    } else {
        editorSetStatusMessageSticky("Ctrl-S save | Ctrl-Q quit | F1 help");
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
