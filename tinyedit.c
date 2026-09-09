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
#include "alloc.h"
#include "backup.h"
#include "buffer.h"
#include "editor_state.h"
#include "history.h"
#include "clipboard.h"
#include "syntax.h"
#include "terminal.h"
#include "utf8.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- globals ------------------------------------------------------------ */

static struct editorConfig E;
static struct editorSettings S;

/* The Ghostty Command-key bridge is opt-in. Keep the on-screen language in
 * step with it, without changing the underlying Ctrl-based key handling. */
static const char *editorPrimaryModifier(void) {
    return S.mac_command_keys ? "Cmd" : "Ctrl";
}

static void editorShortcutText(char *dst, size_t dstsize, const char *src) {
    size_t used = 0;
    if (dstsize == 0) return;
    while (*src && used + 1 < dstsize) {
        if (S.mac_command_keys && strncmp(src, "Ctrl", 4) == 0) {
            if (used + 3 >= dstsize) break;
            memcpy(dst + used, "Cmd", 3);
            used += 3;
            src += 4;
        } else {
            dst[used++] = *src++;
        }
    }
    dst[used] = '\0';
}

/* Toggled by Ctrl-G inside the Ctrl-F search prompt (see
 * editorFindCallback()): when set, editorFindFrom() treats the query
 * as a POSIX extended regular expression (via <regex.h>, no external
 * dependency -- already part of libc) instead of a literal substring.
 * Reset to 0 at the start of every editorFind() call rather than
 * persisted setting-wise -- regex mode is a per-search choice, not a
 * standing preference (mirrors E.search.direction, which is also reset per
 * search rather than remembered across them). */
/* ---- terminal adapter -------------------------------------------------- */
static int32_t editorReadKey(void) {
    return terminalReadKey((uint8_t)S.mac_command_keys);
}


static int32_t editorRowCxToRx(erow *row, int32_t cx) {
    return bufferRowCxToRx(row, cx, S.tab_stop);
}

/* Map a rendered display column to a source-byte cursor offset. Tabs
 * have visual width but only one source byte, so mouse placement must
 * use this same tab expansion as editorRowCxToRx(). */
static int32_t editorRowRxToCx(erow *row, int32_t target_rx) {
    return bufferRowRxToCx(row, target_rx, S.tab_stop);
}

/* Placeholder glyphs for S.show_invisibles, single ASCII bytes on
 * purpose: row->render is a byte buffer that the rest of the editor
 * (wrap segmentation, cursor/rx mapping, selection) indexes assuming
 * 1 byte in chars maps to the SAME BYTE COUNT it always would in
 * render -- a real middle-dot/arrow glyph is 2-3 UTF-8 bytes and would
 * silently break row->rsize and every byte-offset computed from it.
 * Using single-byte substitutes keeps every existing computation
 * valid; the tradeoff is a less pretty glyph than mainstream editors'
 * Unicode dot/arrow. End-of-line has no substitute character here --
 * it's drawn separately, once per row, right after the visible text
 * in editorDrawRows(), since it isn't replacing an existing byte. */
/* Re-tokenizes every row from `from` onward, stopping as soon as a
 * row's hl_open_comment AND hl_open_math both come out the same as
 * before the recompute -- beyond that point no later row's
 * highlighting can change, since syntaxHighlightRow() only depends on
 * its own text plus those two carried-over bits. Needed because
 * editing a row can open or close a multi-line block comment or LaTeX
 * "\[...\]" math span, which shifts every row after it.
 * syntaxHighlightRow() itself no-ops (clearing row->hl) when
 * S.syntax_highlight is off or E.document.file.filename's extension isn't a
 * recognized language, so this doesn't need to check that first. */
/* `force` skips the early-break below: it exists for callers where every
 * row's hl_open_comment/hl_open_math is still its post-editorInsertRow()
 * default of 0 rather than a value produced by a real tokenize pass (e.g.
 * right after a "Save as" gives an untitled buffer its first filetype), so
 * a row matching that default doesn't mean its highlighting is settled. */
static void editorRehighlightFrom(int32_t from, uint8_t force) {
    uint8_t open_comment = from > 0 ? E.document.buffer.rows[from - 1].hl_open_comment : 0;
    uint8_t open_math = from > 0 ? E.document.buffer.rows[from - 1].hl_open_math : 0;
    uint8_t open_frontmatter = from > 0 ? E.document.buffer.rows[from - 1].hl_open_frontmatter : 0;
    uint8_t open_emphasis = from > 0 ? E.document.buffer.rows[from - 1].hl_open_emphasis : 0;
    for (int32_t i = from; i < E.document.buffer.row_count; i++) {
        uint8_t prev_comment = E.document.buffer.rows[i].hl_open_comment;
        uint8_t prev_math = E.document.buffer.rows[i].hl_open_math;
        uint8_t prev_frontmatter = E.document.buffer.rows[i].hl_open_frontmatter;
        uint8_t prev_emphasis = E.document.buffer.rows[i].hl_open_emphasis;
        syntaxHighlightRow(&E.document.buffer.rows[i], E.document.file.filename, (uint8_t)S.syntax_highlight, open_comment, open_math,
            open_frontmatter, i, open_emphasis);
        open_comment = E.document.buffer.rows[i].hl_open_comment;
        open_math = E.document.buffer.rows[i].hl_open_math;
        open_frontmatter = E.document.buffer.rows[i].hl_open_frontmatter;
        open_emphasis = E.document.buffer.rows[i].hl_open_emphasis;
        if (!force && i > from && open_comment == prev_comment && open_math == prev_math &&
            open_frontmatter == prev_frontmatter && open_emphasis == prev_emphasis) break;
    }
}

static void editorUpdateRow(erow *row) {
    int32_t tabs = 0;
    for (int32_t j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    free(row->seg_start);
    free(row->seg_start_rx);
    row->seg_start = NULL;
    row->seg_start_rx = NULL;
    row->seg_count = 0;
    row->seg_wrapcols = -1;
    row->render = teMalloc((size_t)(row->size + tabs * (S.tab_stop - 1) + 1));

    int32_t idx = 0;
    for (int32_t j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = S.show_invisibles ? INVISIBLE_TAB_GLYPH : ' ';
            while (idx % S.tab_stop != 0) row->render[idx++] = ' ';
        } else if (row->chars[j] == ' ' && S.show_invisibles) {
            row->render[idx++] = INVISIBLE_SPACE_GLYPH;
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    /* Index into E.document.buffer.rows: editorUpdateRow() is also called before a row
     * has been linked into E.document.buffer.rows (e.g. editorInsertRow() calls it on
     * E.document.buffer.rows[at] after already growing/placing it, so this is safe;
     * see call sites below). */
    int32_t idx_in_buffer = (int32_t)(row - E.document.buffer.rows);
    uint8_t prev_open_comment = idx_in_buffer > 0 ? E.document.buffer.rows[idx_in_buffer - 1].hl_open_comment : 0;
    uint8_t prev_open_math = idx_in_buffer > 0 ? E.document.buffer.rows[idx_in_buffer - 1].hl_open_math : 0;
    uint8_t prev_open_frontmatter =
        idx_in_buffer > 0 ? E.document.buffer.rows[idx_in_buffer - 1].hl_open_frontmatter : 0;
    uint8_t prev_open_emphasis =
        idx_in_buffer > 0 ? E.document.buffer.rows[idx_in_buffer - 1].hl_open_emphasis : 0;
    syntaxHighlightRow(row, E.document.file.filename, (uint8_t)S.syntax_highlight, prev_open_comment, prev_open_math,
        prev_open_frontmatter, idx_in_buffer, prev_open_emphasis);
    if (idx_in_buffer >= 0 && idx_in_buffer + 1 < E.document.buffer.row_count)
        editorRehighlightFrom(idx_in_buffer + 1, 0);
}

/* Recomputes row->render for every row -- needed whenever a setting
 * that editorUpdateRow() reads (tab_stop, show_invisibles) changes
 * after rows already exist, since editorUpdateRow() is otherwise only
 * called on the specific row(s) an edit touches. Without this, rows
 * untouched since a settings change keep rendering with the old
 * tab_stop/invisibles state until the user happens to edit them --
 * likely a preexisting gap for tab_stop alone, closed here as a side
 * effect of also needing it for show_invisibles. */
static void editorUpdateAllRows(void) {
    for (int32_t i = 0; i < E.document.buffer.row_count; i++) editorUpdateRow(&E.document.buffer.rows[i]);
}

static void editorInsertRow(int32_t at, const char *s, size_t len) {
    if (at < 0 || at > E.document.buffer.row_count) return;
    bufferInsertRow(&E.document.buffer, at, s, len);
    editorUpdateRow(&E.document.buffer.rows[at]);
    E.document.file.dirty = 1;
}

static void editorDelRow(int32_t at) {
    if (at < 0 || at >= E.document.buffer.row_count) return;
    bufferDeleteRow(&E.document.buffer, at);
    if (at < E.document.buffer.row_count) editorRehighlightFrom(at, 0);
    E.document.file.dirty = 1;
}

static void editorRowInsertChar(erow *row, int32_t at, int32_t c) {
    bufferRowInsertByte(row, at, c);
    editorUpdateRow(row);
    E.document.file.dirty = 1;
}

static void editorRowAppendString(erow *row, char *s, size_t len) {
    bufferRowAppend(row, s, len);
    editorUpdateRow(row);
    E.document.file.dirty = 1;
}

static void editorRowDelChar(erow *row, int32_t at) {
    if (at < 0 || at >= row->size) return;
    bufferRowDeleteByte(row, at);
    editorUpdateRow(row);
    E.document.file.dirty = 1;
}

/* ---- undo / redo ------------------------------------------------------- */

static void editorSetStatusMessage(const char *fmt, ...);

static void editorPushUndo(enum undoEditType type) {
    historyRecordEdit(&E.document, S.undo_max_depth, type, time(NULL));
}

static void editorUndo(void) {
    undoSnapshot snapshot;
    if (!historyBeginUndo(&E.document, &snapshot)) {
        editorSetStatusMessage("Nothing to undo");
        return;
    }
    historyRestoreSnapshot(&E.document, &snapshot);
    historyFreeSnapshot(&snapshot);
    editorUpdateAllRows();
    editorSetStatusMessage("Undo");
}

static void editorRedo(void) {
    undoSnapshot snapshot;
    if (!historyBeginRedo(&E.document, &snapshot)) {
        editorSetStatusMessage("Nothing to redo");
        return;
    }
    historyRestoreSnapshot(&E.document, &snapshot);
    historyFreeSnapshot(&snapshot);
    editorUpdateAllRows();
    editorSetStatusMessage("Redo");
}

/* ---- editor operations --------------------------------------------------- */

/* Inserts one byte without taking an undo snapshot. Callers that group
 * several mutations into one user action use this raw form after pushing
 * exactly one snapshot themselves. */
static void editorInsertCharRaw(int32_t c) {
    if (E.document.cursor.cy == E.document.buffer.row_count) editorInsertRow(E.document.buffer.row_count, "", 0);
    editorRowInsertChar(&E.document.buffer.rows[E.document.cursor.cy], E.document.cursor.cx, c);
    E.document.cursor.cx++;
}

static void editorInsertChar(int32_t c) {
    editorPushUndo(EDIT_INSERT);
    editorInsertCharRaw(c);
}

/* Splits the current row without creating an undo snapshot. Callers that
 * expose this as one user action must push exactly one snapshot themselves. */
static void editorInsertNewlineRaw(void) {
    if (E.document.cursor.cx == 0) {
        editorInsertRow(E.document.cursor.cy, "", 0);
    } else {
        erow *row = &E.document.buffer.rows[E.document.cursor.cy];
        editorInsertRow(E.document.cursor.cy + 1, &row->chars[E.document.cursor.cx], (size_t)(row->size - E.document.cursor.cx));
        row = &E.document.buffer.rows[E.document.cursor.cy];
        row->size = E.document.cursor.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.document.cursor.cy++;
    E.document.cursor.cx = 0;
}

/* Enter as typed by the user (as opposed to a newline embedded in
 * pasted/recovered text, which goes through editorInsertNewlineRaw()
 * directly and must NOT be reindented -- the source already has
 * whatever indentation it has). When S.auto_indent is on, copies the
 * leading whitespace (spaces/tabs, nothing else) of the line the
 * cursor was on before the split onto the new line, so continuing to
 * type keeps the same indent level without retyping it by hand. */
static void editorInsertNewlineAutoIndent(void) {
    int32_t src_row = E.document.cursor.cy;
    int32_t indent_len = 0;
    if (S.auto_indent && src_row < E.document.buffer.row_count) {
        erow *row = &E.document.buffer.rows[src_row];
        while (indent_len < row->size &&
               (row->chars[indent_len] == ' ' || row->chars[indent_len] == '\t'))
            indent_len++;
        /* Splitting mid-indent (cursor sits inside the leading
         * whitespace itself) shouldn't duplicate more of it than the
         * new line already inherits from the split -- cap at cx. */
        if (indent_len > E.document.cursor.cx) indent_len = E.document.cursor.cx;
    }

    editorPushUndo(EDIT_OTHER);
    editorInsertNewlineRaw();

    if (indent_len > 0) {
        erow *row = &E.document.buffer.rows[src_row];
        for (int32_t i = 0; i < indent_len; i++)
            editorInsertCharRaw((unsigned char)row->chars[i]);
    }
}

static void editorDelChar(void) {
    if (E.document.cursor.cy == E.document.buffer.row_count) return;
    if (E.document.cursor.cx == 0 && E.document.cursor.cy == 0) return;

    editorPushUndo(EDIT_DELETE);

    erow *row = &E.document.buffer.rows[E.document.cursor.cy];
    if (E.document.cursor.cx > 0) {
        /* Delete the whole grapheme cluster before cx (base character
         * plus any joined modifiers/marks), not just one byte or one
         * codepoint, so Backspace removes e.g. an emoji with a
         * skin-tone modifier in a single press. */
        size_t del_count = utf8PrevCharLen(row->chars, (size_t)E.document.cursor.cx);
        if (del_count == 0) del_count = 1;
        for (size_t k = 0; k < del_count; k++)
            editorRowDelChar(row, E.document.cursor.cx - 1 - (int32_t)k);
        E.document.cursor.cx -= (int32_t)del_count;
    } else {
        E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy - 1].size;
        editorRowAppendString(&E.document.buffer.rows[E.document.cursor.cy - 1], row->chars, (size_t)row->size);
        editorDelRow(E.document.cursor.cy);
        E.document.cursor.cy--;
    }
}

/* ---- file i/o ------------------------------------------------------------- */

static enum lineEndingMode editorEffectiveLineEnding(void) {
    if (S.line_ending == LINE_ENDING_LF || S.line_ending == LINE_ENDING_CRLF)
        return (enum lineEndingMode)S.line_ending;
    return E.document.file.detected_line_ending;
}

static char *editorRowsToString(size_t *buflen) {
    return bufferSerialize(&E.document.buffer, editorEffectiveLineEnding(), buflen);
}

static void editorSetStatusMessage(const char *fmt, ...);
static void editorSetStatusMessageSticky(const char *fmt, ...);
static void editorRefreshScreen(void);
static int32_t editorReadKey(void);
static int32_t editorReadMultiByteKey(uint8_t lead, char *out);
static void editorFreeUndoRedo(void);
static void abAppend(struct abuf *ab, const char *s, int32_t len);
static void abFree(struct abuf *ab);

/* Prompt input is kept verbatim, but spaces/tabs are always rendered as
 * compact placeholders so whitespace is unambiguous while searching or
 * replacing.
 * Newlines from a clipboard paste are displayed as "\n" so a one-line
 * message bar remains one line, while the underlying replacement text
 * still retains the real newline. */
static char *editorPromptDisplayText(const char *buf, size_t len) {
    size_t extra = 0;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n' || buf[i] == '\r') extra++;
    char *display = teMalloc(len + extra + 1);
    size_t dst = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            display[dst++] = '\\';
            display[dst++] = buf[i] == '\n' ? 'n' : 'r';
        } else if (buf[i] == ' ') {
            display[dst++] = INVISIBLE_SPACE_GLYPH;
        } else if (buf[i] == '\t') {
            display[dst++] = INVISIBLE_TAB_GLYPH;
        } else {
            display[dst++] = buf[i];
        }
    }
    display[dst] = '\0';
    return display;
}

static void editorPromptAppend(char **buf, size_t *bufsize, size_t *buflen,
    const char *text, size_t len) {
    while (*buflen + len + 1 > *bufsize) *bufsize *= 2;
    memcpy(*buf + *buflen, text, len);
    *buflen += len;
    (*buf)[*buflen] = '\0';
}

/* Displays a prompt in the message bar and lets the user type a response
 * with basic line editing (Backspace, Enter, Esc to cancel). Returns a
 * malloc'd string (caller must free), or NULL if the user pressed Esc.
 *
 * If callback is non-NULL, it is invoked after every keystroke (including
 * the initial empty buffer) as callback(buf, key), so callers can drive
 * live side effects such as incremental-search highlighting. The callback
 * is also invoked once more with key == '\r' or '\x1b' right before the
 * prompt returns, so it can do final cleanup/confirmation. */
/* `prompt` takes exactly one "%s" (filled with the buffer being typed)
 * unless `status_fn` is non-NULL, in which case it takes two: the
 * first filled with status_fn()'s return value (re-evaluated every
 * redraw, so it can reflect state the callback toggles mid-prompt,
 * e.g. editorFind()'s regex-mode indicator), the second with the
 * buffer as usual. Kept as a single optional extra field rather than a
 * generic varargs prompt-formatting scheme -- the only caller that
 * needs a live-updating prompt is search, not worth a bigger API for
 * one user.
 *
 * `short_prompt` is optional (NULL for callers that don't need it,
 * e.g. "Save as:"): when the fully-formatted `prompt` wouldn't fit on
 * the message bar alongside the buffer being typed, this switches to
 * `short_prompt` instead (same %s rules as `prompt`) -- e.g. Search's
 * long form spelling out every shortcut shrinks to "Search: " once the
 * query grows too long for both to fit. If even `short_prompt` doesn't
 * fit, editorDrawMessageBar()'s tail-scroll behavior takes over from
 * there (this function doesn't need to know about that layer -- it
 * only picks which of the two full strings to hand to
 * editorSetStatusMessage()). */
static char *editorPromptCB(const char *prompt, const char *short_prompt,
    const char *(*status_fn)(void), void (*callback)(char *, int32_t)) {
    size_t bufsize = 128;
    char *buf = teMalloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        char *display = editorPromptDisplayText(buf, buflen);
        const char *active_prompt = prompt;
        if (short_prompt) {
            /* Two independent size limits, both checked: E.view.screencols
             * (the visible width -- text beyond it never gets a
             * chance to show, see editorDrawMessageBar()'s
             * tail-scroll) AND sizeof(E.ui.statusmsg) (the fixed 80-byte
             * buffer editorSetStatusMessage() formats into --
             * vsnprintf() silently truncates whatever doesn't fit
             * there, which bit *before* the screencols check ever
             * mattered: on a wide terminal the long prompt "fits" on
             * screen but still gets truncated by vsnprintf() into
             * E.ui.statusmsg's 80 bytes, silently dropping the tail of
             * the query the user typed -- this is what the user saw
             * as "search freezes after 5 characters" even though the
             * search itself kept working on the full, untruncated
             * `buf`). Whichever limit is smaller determines whether
             * the long prompt can be shown at all. */
            char probe[sizeof(E.ui.statusmsg)];
            int32_t plen;
            if (status_fn) plen = snprintf(probe, sizeof(probe), prompt, status_fn(), display);
            else plen = snprintf(probe, sizeof(probe), prompt, display);
            int32_t stmsg_limit = (int32_t)sizeof(E.ui.statusmsg) - 1;
            int32_t limit = E.view.screencols < stmsg_limit ? E.view.screencols : stmsg_limit;
            if (plen > limit) active_prompt = short_prompt;
        }
        if (status_fn) editorSetStatusMessage(active_prompt, status_fn(), display);
        else editorSetStatusMessage(active_prompt, display);
        free(display);
        editorRefreshScreen();

        int32_t c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == CTRL_KEY('c')) {
            clipboardCopy(buf, buflen);
            editorSetStatusMessage("Prompt copied");
        } else if (c == CTRL_KEY('v')) {
            size_t len;
            char *text = clipboardPaste(&len);
            if (text) {
                editorPromptAppend(&buf, &bufsize, &buflen, text, len);
                clipboardFree(text);
            }
        } else if (c == PASTE_START_KEY) {
            size_t len;
            char *text = terminalReadPastedText(&len);
            editorPromptAppend(&buf, &bufsize, &buflen, text, len);
            free(text);
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
        } else if (c >= 32 && c < 127) {
            char ch = (char)c;
            editorPromptAppend(&buf, &bufsize, &buflen, &ch, 1);
        } else if (c >= 0x80 && c <= 0xff) {
            char seq[4];
            int32_t seq_len = editorReadMultiByteKey((uint8_t)c, seq);
            editorPromptAppend(&buf, &bufsize, &buflen, seq, (size_t)seq_len);
        }

        if (callback) callback(buf, c);
        if (E.search.switch_to_replace) {
            editorSetStatusMessage("");
            return buf;
        }
    }
}

static char *editorPrompt(const char *prompt) {
    return editorPromptCB(prompt, NULL, NULL, NULL);
}

/* Discards every row currently in the buffer (but not E.document.file.filename),
 * resetting to a blank document. Used before reloading content from
 * scratch -- e.g. replacing what editorOpen() read from disk with a
 * newer crash-recovery backup, see editorOfferBackupRecovery(). */
static void editorClearRows(void) {
    bufferClear(&E.document.buffer);
    E.document.cursor.cx = 0;
    E.document.cursor.cy = 0;
}

/* Splits `data` (length `len`, not necessarily NUL-terminated) into
 * rows on '\n', trimming a trailing '\r' from each (CRLF-tolerant),
 * appending them to the buffer via editorInsertRow(). Shared by
 * editorOpen() (reading a file) and editorOfferBackupRecovery()
 * (reading a backup) so both parse line endings the same way. */
static void editorLoadLines(const char *data, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || data[i] == '\n') {
            size_t linelen = i - start;
            if (linelen > 0 && data[start + linelen - 1] == '\r') linelen--;
            /* A trailing newline at end-of-input produces one final
             * empty "row" here (start == len) that real files/backups
             * never intend -- editorRowsToString() always terminates
             * every row including the last with '\n', so skip it. */
            if (i < len || linelen > 0) editorInsertRow(E.document.buffer.row_count, data + start, linelen);
            start = i + 1;
        }
    }
}

/* Resolves the status-bar filetype for the just-opened file's
 * extension, in the order the two config sources are authoritative in:
 *
 *   1. ~/.tinyeditrc (or the built-in table) already names it. The
 *      name is settled; the only open question is whether the syntax
 *      .conf that would highlight it is actually installed. A built-in
 *      language carries its own compiled-in tokenizer, so only a name
 *      that came from a user override can be missing one -- that's the
 *      case worth a warning, since the user sees a language name in
 *      the status bar but no colors and would otherwise have no hint
 *      why.
 *   2. Nothing names it, but an installed .conf claims the extension
 *      and declares a "filetype" key. Record it in ~/.tinyeditrc so
 *      the extension is known from now on, and persist immediately.
 *
 * Anything else (no name anywhere, or a .conf without a filetype key)
 * leaves the field absent, exactly as before.
 *
 * Called once per file open rather than from editorFiletypeLabel(),
 * which runs on every status-bar redraw -- resolving and potentially
 * writing ~/.tinyeditrc at that rate would be wasteful and would put a
 * disk write in the render path. */
static void editorResolveFiletype(void) {
    if (!E.document.file.filename) return;
    const char *dot = strrchr(E.document.file.filename, '.');
    if (!dot || dot[1] == '\0' || dot == E.document.file.filename) return;
    const char *ext = dot + 1;

    if (filetypeForExtension(ext)) return;

    const char *name = syntaxUserFiletypeForExtension(ext);
    if (!name) return;

    settingsSetFiletype(ext, name);
    settingsSave(&S);
}

/* Warns when the open file's extension has a filetype name but no
 * highlighting to go with it -- see editorResolveFiletype() above for
 * why only a user-declared name can end up in that state. Split out
 * from that function, and called after the startup hint is set, so the
 * sticky hint doesn't immediately overwrite this message (same
 * ordering constraint the backup-recovery message has). */
static void editorWarnMissingHighlightConfig(void) {
    if (!E.document.file.filename) return;
    const char *dot = strrchr(E.document.file.filename, '.');
    if (!dot || dot[1] == '\0' || dot == E.document.file.filename) return;
    const char *ext = dot + 1;

    const char *name = filetypeForExtension(ext);
    if (!name) return;
    if (syntaxUserLangHasExtension(ext) || syntaxHasBuiltinExtension(ext)) return;

    editorSetStatusMessage("Filetype '%s': highlight config missing", name);
}

static void editorOpen(const char *filename) {
    free(E.document.file.filename);
    E.document.file.filename = teStrdup(filename);

    /* Before the read, so it runs for a brand-new file too: the
     * extension is known from the name alone, and a new .njk should
     * get its filetype recorded just like an existing one. */
    editorResolveFiletype();

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno == ENOENT) return; /* new file */
        terminalDie("fopen");
    }

    E.document.file.detected_line_ending = LINE_ENDING_LF;
    E.document.file.line_endings_mixed = 0;
    uint8_t saw_line_ending = 0;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        enum lineEndingMode this_ending = LINE_ENDING_LF;
        uint8_t has_ending = linelen > 0 && line[linelen - 1] == '\n';
        if (has_ending && linelen > 1 && line[linelen - 2] == '\r')
            this_ending = LINE_ENDING_CRLF;
        if (has_ending) {
            if (!saw_line_ending) {
                E.document.file.detected_line_ending = this_ending;
                saw_line_ending = 1;
            } else if (this_ending != E.document.file.detected_line_ending) {
                E.document.file.line_endings_mixed = 1;
            }
        }
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.document.buffer.row_count, line, (size_t)linelen);
    }
    free(line);
    fclose(fp);
    E.document.file.dirty = 0;
    if (E.document.file.line_endings_mixed)
        editorSetStatusMessage("Mixed line endings: auto uses %s",
            E.document.file.detected_line_ending == LINE_ENDING_CRLF ? "CRLF" : "LF");
}

/* Writes a crash-recovery backup if S.backup_interval seconds have
 * passed since the last one and the buffer has unsaved changes (a
 * clean buffer has nothing to recover that isn't already safely on
 * disk, so writing one would be pure overhead). Called once per main
 * loop iteration -- cheap when not due, since it's just a time(NULL)
 * and comparison until the interval actually elapses. A brand new
 * buffer with no filename yet is skipped: there's nowhere stable to
 * derive a backup path from until the user picks a name (Ctrl-S). */
static void editorMaybeBackup(void) {
    int32_t interval = S.backup_interval;
    if (interval <= 0 || !E.document.file.dirty || !E.document.file.filename) return;
    if (interval < 5) interval = 5; /* see settings.h: backup_interval */

    time_t now = time(NULL);
    if (E.document.file.last_backup_time != 0 && now - E.document.file.last_backup_time < interval) return;

    size_t len;
    char *buf = editorRowsToString(&len);
    backupWrite(E.document.file.filename, buf, len);
    free(buf);
    E.document.file.last_backup_time = now;
}

/* If a crash-recovery backup exists for E.document.file.filename, asks the user
 * whether to load it in place of what editorOpen() just read from
 * disk. Called once at startup, after editorOpen(). A backup existing
 * at all is exactly the crash signal (see backup.h): a clean exit
 * always removes its own backup, so one surviving to the next
 * startup means the previous session never got to do that. Marks the
 * buffer dirty on recovery (it now differs from what's on disk) and
 * leaves the backup file itself alone -- it gets cleaned up on the
 * next successful save or clean quit like any other session's. */
/* Draws one line of the recovery screen, centered, padded to
 * E.view.screencols with `bg` as background so the whole row reads as a
 * solid colored bar rather than text floating on the normal
 * background -- this is what makes the screen impossible to miss
 * compared to a message-bar prompt buried at the bottom. */
static void editorRecoveryScreenLine(struct abuf *ab, const char *bg, const char *text) {
    int32_t textlen = (int32_t)strlen(text);
    if (textlen > E.view.screencols) textlen = E.view.screencols;
    int32_t padding = (E.view.screencols - textlen) / 2;

    abAppend(ab, bg, (int32_t)strlen(bg));
    for (int32_t i = 0; i < padding; i++) abAppend(ab, " ", 1);
    abAppend(ab, text, textlen);
    for (int32_t i = padding + textlen; i < E.view.screencols; i++) abAppend(ab, " ", 1);
    abAppend(ab, "\x1b[m\r\n", 5);
}

/* Full-screen, high-visibility warning shown at startup when a
 * crash-recovery backup exists for the file being opened (see
 * backup.h) -- deliberately impossible to miss (solid yellow-on-black
 * bar filling the screen, not a message-bar line easily glossed over)
 * because silently losing unsaved work is a much worse outcome than
 * one extra confirmation the user didn't need. Any key other than
 * y/Y declines recovery and opens the file as read from disk. */
static void editorRecoveryScreen(void) {
    /* Bold + reverse-video (swaps the terminal's own foreground/
     * background) instead of a hardcoded color pair -- readable on
     * any terminal color scheme/theme without guessing whether black-
     * on-yellow (or any other fixed pair) renders sanely there. Same
     * escape already used for selection highlight and the F1/F2
     * headers elsewhere in the editor, so it's also visually
     * consistent with the rest of the UI. */
    const char *bg = "\x1b[1;7m";

    struct abuf ab = ABUF_INIT;
    const char *clear_and_home = "\x1b[?25l\x1b[2J\x1b[H";
    abAppend(&ab, clear_and_home, (int32_t)strlen(clear_and_home));

    int32_t mid = E.view.screenrows / 2;
    for (int32_t i = 0; i < mid - 2; i++) editorRecoveryScreenLine(&ab, bg, "");
    editorRecoveryScreenLine(&ab, bg, "");
    editorRecoveryScreenLine(&ab, bg, "!!  UNSAVED CHANGES FOUND  !!");
    editorRecoveryScreenLine(&ab, bg, "");
    char msg[160];
    snprintf(msg, sizeof(msg), "A previous session on \"%.100s\" did not exit cleanly.",
        E.document.file.filename ? E.document.file.filename : "");
    editorRecoveryScreenLine(&ab, bg, msg);
    editorRecoveryScreenLine(&ab, bg, "Restore the recovered changes?  [y] Yes    [n] No");
    editorRecoveryScreenLine(&ab, bg, "");
    for (int32_t i = mid + 3; i < E.view.screenrows; i++) editorRecoveryScreenLine(&ab, bg, "");

    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

static void editorOfferBackupRecovery(void) {
    if (!E.document.file.filename || !backupExists(E.document.file.filename)) return;

    editorRecoveryScreen();
    int32_t c = editorReadKey();
    if (c != 'y' && c != 'Y') {
        editorSetStatusMessage("");
        return;
    }

    size_t len;
    char *content = backupRead(E.document.file.filename, &len);
    if (!content) {
        editorSetStatusMessage("Could not read recovery backup.");
        return;
    }

    editorClearRows();
    editorLoadLines(content, len);
    free(content);
    E.document.file.dirty = 1;
    editorSetStatusMessage("Recovered unsaved changes from backup.");
}

static uint8_t editorWriteAll(int fd, const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n > 0) written += (size_t)n;
        else if (n == -1 && errno == EINTR) continue;
        else return 0;
    }
    return 1;
}

static uint8_t editorAtomicSave(const char *filename, const char *buf, size_t len) {
    char resolved[4096];
    const char *target = realpath(filename, resolved) ? resolved : filename;
    size_t target_len = strlen(target);
    const char suffix[] = ".tinyedit.XXXXXX";
    char *tmppath = teMalloc(target_len + sizeof(suffix));
    if (!tmppath) { errno = ENOMEM; return 0; }
    memcpy(tmppath, target, target_len);
    memcpy(tmppath + target_len, suffix, sizeof(suffix));

    struct stat existing;
    uint8_t existed = stat(target, &existing) == 0;
    int fd = mkstemp(tmppath);
    if (fd == -1) { free(tmppath); return 0; }

    mode_t mode;
    if (existed) {
        mode = existing.st_mode & 07777;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        mode = 0644 & ~mask;
    }

    uint8_t ok = fchmod(fd, mode) == 0 && editorWriteAll(fd, buf, len) && fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (ok && rename(tmppath, target) != 0) ok = 0;

    if (!ok) {
        int saved_errno = errno;
        unlink(tmppath);
        errno = saved_errno;
    }
    free(tmppath);
    return ok;
}

/* `force_prompt` makes an already-named buffer go through "Save as"
 * again (F4/Ctrl-Shift-S) instead of silently overwriting E.document.file.filename,
 * which is what a plain save (Ctrl-S) does when a name already exists. */
static void editorSaveInternal(uint8_t force_prompt) {
    if (E.document.file.filename == NULL || force_prompt) {
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
        free(E.document.file.filename);
        E.document.file.filename = name;
        /* Filetype is derived from E.document.file.filename's extension, so a new
         * name can turn on/change highlighting for rows tokenized
         * under a different (or no) filetype -- recompute now instead
         * of waiting for the next edit to trigger it. */
        editorRehighlightFrom(0, 1);
    }

    size_t len;
    char *buf = editorRowsToString(&len);

    if (editorAtomicSave(E.document.file.filename, buf, len)) {
        free(buf);
        E.document.file.dirty = 0;
        /* The buffer is now identical to what's on disk --
         * the crash-recovery backup (if any) would only ever
         * offer to "recover" something we just saved, so
         * drop it instead of leaving it to confuse the next
         * startup's crash check. */
        backupRemove(E.document.file.filename);
        editorSetStatusMessage("%zu bytes written to disk", len);
        return;
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

static void editorSave(void) {
    editorSaveInternal(0);
}

static void editorSaveAs(void) {
    editorSaveInternal(1);
}

/* Whether the buffer differs from what's on disk, compared byte for
 * byte. E.document.file.dirty only ever goes from 0 to 1: undoing every edit, or
 * retyping what was deleted, leaves it set even though nothing actually
 * changed, and the user then gets asked to save a file that is already
 * identical. Checking the real contents catches all of those without
 * having to keep dirty exact after every keystroke -- this runs once,
 * when leaving the document, so reading the file back costs nothing
 * during editing.
 *
 * Any I/O failure answers "yes, it differs": if the file can't be read
 * the safe assumption is that there is something to lose, so the user
 * still gets the prompt. A buffer with no filename is likewise always
 * different -- there is nothing on disk to match. */
static uint8_t editorDiffersFromDisk(void) {
    if (!E.document.file.filename) return 1;

    FILE *fp = fopen(E.document.file.filename, "rb");
    if (!fp) return 1;

    size_t buflen;
    char *buf = editorRowsToString(&buflen);
    uint8_t differs = 1;

    if (fseek(fp, 0, SEEK_END) == 0) {
        long disklen = ftell(fp);
        if (disklen >= 0 && (size_t)disklen == buflen && fseek(fp, 0, SEEK_SET) == 0) {
            char *disk = teMalloc(buflen ? buflen : 1);
            if (fread(disk, 1, buflen, fp) == buflen)
                differs = (buflen > 0) && (memcmp(disk, buf, buflen) != 0);
            free(disk);
        }
    }

    free(buf);
    fclose(fp);
    return differs;
}

/* Shared save/discard/cancel gate for every operation that would leave the
 * current document (quit, close, or open another file). Returns 1 only when
 * it is safe to proceed; a failed/cancelled save leaves the document open. */
static uint8_t editorConfirmDocumentChange(const char *action) {
    if (!E.document.file.dirty) return 1;

    /* Nothing to save after all -- the edits cancelled out (undone,
     * or retyped identically). Clear the flag so the status bar stops
     * claiming the file is modified too. */
    if (!editorDiffersFromDisk()) {
        E.document.file.dirty = 0;
        return 1;
    }

    editorSetStatusMessage("Save changes before %s? (y/n/Esc to cancel)", action);
    editorRefreshScreen();
    int32_t c = editorReadKey();
    if (c == 'y' || c == 'Y') {
        editorSave();
        return !E.document.file.dirty;
    }
    if (c == 'n' || c == 'N') return 1;

    editorSetStatusMessage("");
    return 0;
}

/* Releases every piece of state owned by the current document while keeping
 * terminal dimensions and user settings intact. The next document starts
 * with independent cursor, selection, search, backup, and undo state. */
static void editorResetDocument(void) {
    if (E.document.file.filename) backupRemove(E.document.file.filename);
    editorClearRows();
    free(E.document.file.filename);
    E.document.file.filename = NULL;
    editorFreeUndoRedo();

    E.document.cursor.cx = 0;
    E.document.cursor.cy = 0;
    E.document.cursor.rx = 0;
    E.view.rowoff = 0;
    E.view.coloff = 0;
    E.view.free_scroll = 0;
    E.document.file.detected_line_ending = LINE_ENDING_LF;
    E.document.file.line_endings_mixed = 0;
    E.document.file.dirty = 0;
    E.document.selection.active = 0;
    E.document.selection.anchor_x = 0;
    E.document.selection.anchor_y = 0;
    E.document.selection.pinned = 0;
    E.search.search_match_y = -1;
    E.search.search_match_x = 0;
    E.search.search_match_len = 0;
    E.document.file.last_backup_time = 0;
    E.document.history.last_edit_type = EDIT_NONE;
    E.document.history.last_edit_time = 0;
}

/* Ctrl-W closes only the current document. tinyedit remains alive with a
 * fresh unnamed buffer, ready for typing or Ctrl-O. */
static void editorCloseFile(void) {
    if (!editorConfirmDocumentChange("closing")) return;
    editorResetDocument();
    editorSetStatusMessageSticky("File closed. %s-O open | %s-Q quit | F1 help",
        editorPrimaryModifier(), editorPrimaryModifier());
}

/* Ctrl-O switches the single active document. The current document remains
 * in place if save confirmation or path entry is cancelled, or if an existing
 * target cannot be read. ENOENT is intentional: like Vim, this opens a named
 * empty buffer and the file is created on the first successful save. */
static void editorOpenFile(void) {
    if (!editorConfirmDocumentChange("opening another file")) return;

    char *name = editorPrompt("Open file: %s (Esc to cancel)");
    if (!name) {
        editorSetStatusMessage("Open cancelled.");
        return;
    }

    struct stat st;
    uint8_t exists = stat(name, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        editorSetStatusMessage("Can't open a directory.");
        free(name);
        return;
    }
    if (exists) {
        FILE *probe = fopen(name, "r");
        if (!probe) {
            editorSetStatusMessage("Can't open file: %s", strerror(errno));
            free(name);
            return;
        }
        fclose(probe);
    } else if (errno != ENOENT) {
        editorSetStatusMessage("Can't open path: %s", strerror(errno));
        free(name);
        return;
    }

    editorResetDocument();
    editorOpen(name);
    free(name);
    editorOfferBackupRecovery();
    if (!E.document.file.dirty) {
        if (exists) editorSetStatusMessage("Opened %s", E.document.file.filename);
        else editorSetStatusMessage("New file: %s", E.document.file.filename);
    }
}

static void editorQuit(void) {
    if (!editorConfirmDocumentChange("quitting")) return;

    /* A discarded dirty buffer must not leave a recovery file that would
     * look like evidence of a crash the next time this path is opened. */
    if (E.document.file.filename) backupRemove(E.document.file.filename);

    exit(0);
}

/* ---- append buffer -------------------------------------------------------- */

static void abAppend(struct abuf *ab, const char *s, int32_t len) {
    char *new = teRealloc(ab->b, (size_t)(ab->len + len));
    if (new == NULL) return;
    memcpy(&new[ab->len], s, (size_t)len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab) { free(ab->b); }

/* Resets SGR attributes (colors, reverse-video, bold, ...) the same as
 * a literal "\x1b[m" everywhere else in this file, but immediately
 * re-applies the configured background color (see color_background in
 * settings.h) if one is set -- otherwise every reset scattered through
 * editorDrawRowSegment() (one per highlighted character/glyph, see its
 * per-char syn_color/is_invisible_glyph resets) would also erase the
 * background for the very next character, defeating a whole-editor
 * background the instant any syntax highlighting or selection is
 * active. Callers that don't need to distinguish should always use
 * this over a bare "\x1b[m" for that reason. */
static void abAppendReset(struct abuf *ab) {
    abAppend(ab, "\x1b[m", 3);
    const char *bg = ansiBgColorCode(S.color_background);
    if (bg[0]) abAppend(ab, bg, (int32_t)strlen(bg));
}

/* ---- output ---------------------------------------------------------------- */

/* Width of the left-hand line-number gutter, including one space of
 * padding before the text starts. Zero when gutter is disabled. Grows
 * with E.document.buffer.row_count so files with 1000+ lines still right-align cleanly. */
static int32_t editorGutterWidth(void) {
    if (!S.show_line_numbers) return 0;
    int32_t digits = 3;
    int32_t n = E.document.buffer.row_count;
    while (n >= 1000) {
        digits++;
        n /= 10;
    }
    /* editorDrawGutter() formats into a fixed 16-byte buffer. Keeping the
     * width bounded here preserves both the visual layout and a meaningful
     * snprintf() bound even if a corrupted/constructed buffer reports an
     * absurd number of rows. */
    if (digits > 14) digits = 14;
    return digits + 1;
}

/* Usable text area width: total screen columns minus the gutter. */
static int32_t editorTextCols(void) {
    int32_t cols = E.view.screencols - editorGutterWidth();
    return cols > 0 ? cols : 0;
}

/* Effective soft-wrap width in render columns. Wrap is always active
 * (no horizontal scrolling in this editor): text always wraps at the
 * window edge (editorTextCols() - 1 column of right-hand margin) at
 * minimum. soft_wrap == 0 means "no extra limit, just the window
 * edge"; soft_wrap > 0 additionally caps the width to that value when
 * the window is wider than it (e.g. to keep prose readable on a wide
 * terminal), while still following the window edge on a narrower one. */
static int32_t editorSoftWrapCols(void) {
    int32_t textcols = editorTextCols();
    int32_t margin = textcols - 1;
    if (margin < 1) margin = 1;
    if (S.soft_wrap <= 0) return margin;
    return margin < S.soft_wrap ? margin : S.soft_wrap;
}

/* Splits row->render into visual segments of at most `wrapcols` render
 * columns each, breaking at the last space at or before the limit
 * (word-wrap) or hard-breaking mid-word if no space is found. Fills
 * seg_start[] with the BYTE offset into row->render where each
 * segment begins (needed to index/memcpy render directly) and, in
 * parallel, seg_start_rx[] with the render-COLUMN offset of that same
 * point -- the two diverge as soon as the row contains any multi-byte
 * UTF-8 character or wide glyph before the wrap point (1 byte is not
 * always 1 column). Callers that compare against a column value (e.g.
 * E.document.cursor.rx, itself computed by editorRowCxToRx() which is UTF-8-aware)
 * MUST use seg_start_rx[], not seg_start[] -- mixing the two silently
 * misplaces the cursor/inserted text on any row with non-ASCII text
 * before a wrap point. seg_start_rx may be NULL if the caller only
 * needs byte offsets (e.g. to memcpy/render). Segment i covers
 * [seg_start[i], seg_start[i+1]) in bytes / [seg_start_rx[i],
 * seg_start_rx[i+1]) in columns, and the last segment ends at
 * row->rsize bytes. Always produces at least 1 segment (even for an
 * empty row), and never splits inside a multi-column character
 * (CJK/wide glyphs), since it walks grapheme clusters via
 * utf8NextCharLen/utf8SingleCharWidth same as the rest of the
 * renderer. Results are cached on the row and have no fixed segment
 * limit; a long generated line remains fully reachable. */
static int32_t editorRowSegments(erow *row, int32_t wrapcols) {
    if (row->seg_start && row->seg_start_rx && row->seg_wrapcols == wrapcols)
        return row->seg_count;

    free(row->seg_start);
    free(row->seg_start_rx);
    row->seg_start = NULL;
    row->seg_start_rx = NULL;
    row->seg_count = 0;
    row->seg_wrapcols = wrapcols;

    int32_t capacity = 16;
    row->seg_start = teMalloc(sizeof(int32_t) * (size_t)capacity);
    row->seg_start_rx = teMalloc(sizeof(int32_t) * (size_t)capacity);
    if (!row->seg_start || !row->seg_start_rx) terminalDie("malloc wrap segments");

    if (wrapcols <= 0 || row->rsize == 0) {
        row->seg_start[0] = 0;
        row->seg_start_rx[0] = 0;
        row->seg_count = 1;
        return 1;
    }

    int32_t nseg = 0;
    int32_t line_start = 0;    /* byte offset where the current segment begins */
    int32_t line_start_rx = 0; /* column offset of the same point */

    while (line_start < row->rsize) {
        if (nseg == capacity) {
            capacity *= 2;
            int32_t *new_start = teRealloc(row->seg_start, sizeof(int32_t) * (size_t)capacity);
            if (!new_start) terminalDie("realloc wrap segments");
            row->seg_start = new_start;

            int32_t *new_rx = teRealloc(row->seg_start_rx, sizeof(int32_t) * (size_t)capacity);
            if (!new_rx) terminalDie("realloc wrap segments");
            row->seg_start_rx = new_rx;
        }
        row->seg_start[nseg] = line_start;
        row->seg_start_rx[nseg] = line_start_rx;
        nseg++;

        int32_t col = 0;
        int32_t pos = line_start;
        int32_t last_space_pos = -1, last_space_col = -1;
        while (pos < row->rsize && col < wrapcols) {
            size_t clen = utf8NextCharLen(row->render, (size_t)pos, (size_t)row->rsize);
            if (clen == 0) clen = 1;
            int32_t w = utf8SingleCharWidth(row->render + pos, clen);
            if (col + w > wrapcols) break;
            if (row->render[pos] == ' ') { last_space_pos = pos; last_space_col = col; }
            col += w;
            pos += (int32_t)clen;
        }

        if (pos >= row->rsize) {
            line_start_rx += col;
            line_start = row->rsize;
        } else if (last_space_pos >= 0 && last_space_pos + 1 > line_start) {
            line_start_rx += last_space_col + 1; /* wrap after the space */
            line_start = last_space_pos + 1;
        } else {
            line_start_rx += col; /* no space to break at: hard break */
            line_start = pos;
        }
    }

    if (nseg == 0) { /* row->rsize == 0 already handled above, kept for safety */
        row->seg_start[nseg] = 0;
        row->seg_start_rx[nseg] = 0;
        nseg++;
    }
    row->seg_count = nseg;
    return nseg;
}

/* Render-column just past the last visible character of segment `i`
 * (out of `nseg` segments starting at `seg_start`), i.e. where the
 * cursor should land on End or after typing the last visible
 * character of that segment. This is NOT simply seg_start[i+1]: when
 * a segment wraps after a space (see editorRowSegments()), that space
 * is logically part of segment i but isn't drawn on its video row --
 * seg_start[i+1] already points past it, at the start of the next
 * word. Using seg_start[i+1] directly as "end of segment" would place
 * the cursor (and any character typed there) one position into the
 * next visual line instead of at the end of the current one. */
static int32_t editorSegVisibleEnd(erow *row, int32_t nseg, const int32_t *seg_start, int32_t i) {
    int32_t end = (i + 1 < nseg) ? seg_start[i + 1] : row->rsize;
    while (end > seg_start[i] && row->render[end - 1] == ' ') end--;
    return end;
}

/* Column equivalent of editorSegVisibleEnd() -- the render-COLUMN
 * just past the last visible character of segment `i`, for callers
 * that need to compare/combine it with other column values (E.document.cursor.rx,
 * editorSegColToCx()'s target_col) instead of indexing row->render
 * directly. Each trimmed trailing space is exactly 1 column wide
 * (ASCII ' ', never a wide/multi-byte glyph -- editorRowSegments()
 * only ever records last_space_pos for byte 0x20), so the column
 * count is simply the byte count minus the number of trimmed bytes. */
static int32_t editorSegVisibleEndRx(erow *row, int32_t nseg, const int32_t *seg_start,
    const int32_t *seg_start_rx, int32_t i) {
    int32_t end_byte = (i + 1 < nseg) ? seg_start[i + 1] : row->rsize;
    int32_t end_rx = (i + 1 < nseg) ? seg_start_rx[i + 1] : editorRowCxToRx(row, row->size);
    while (end_byte > seg_start[i] && row->render[end_byte - 1] == ' ') {
        end_byte--;
        end_rx--;
    }
    return end_rx;
}

/* Finds which visual segment of `row` contains render-column `rx`, and
 * the column within that segment. Used to translate the logical
 * cursor position into (segment index, in-segment column) for
 * scrolling/rendering with wrap active. */
static void editorRxToSegment(erow *row, int32_t wrapcols, int32_t rx, int32_t *seg_idx, int32_t *seg_col) {
    int32_t nseg = editorRowSegments(row, wrapcols);
    int32_t i;
    for (i = 0; i < nseg - 1; i++) {
        if (rx < row->seg_start_rx[i + 1]) break;
    }
    *seg_idx = i;
    *seg_col = rx - row->seg_start_rx[i];
}

/* Number of visual (video) rows a logical file row occupies -- 1 when
 * wrap is off or the row is empty, or the wrap segment count. */
static int32_t editorRowVideoHeight(int32_t filerow, int32_t wrapcols) {
    if (wrapcols <= 0) return 1;
    return editorRowSegments(&E.document.buffer.rows[filerow], wrapcols);
}

/* Converts a (filerow, segment index) pair into an absolute video-row
 * number, counting every visual segment of every row from 0 up to
 * (but not including) filerow, plus `seg` segments into filerow
 * itself. This is the wrapped-mode equivalent of "filerow" alone in
 * unwrapped mode -- E.view.rowoff and the viewport's y position are both
 * expressed in this unit when wrap is active. O(numrows) per call;
 * fine at the scale this editor targets (see IDEAS.md on large files),
 * called at most a couple times per keypress/redraw. */
static int32_t editorVideoRowOf(int32_t filerow, int32_t seg, int32_t wrapcols) {
    int32_t vy = 0;
    for (int32_t i = 0; i < filerow; i++)
        vy += editorRowVideoHeight(i, wrapcols);
    return vy + seg;
}

/* Inverse of editorVideoRowOf(): given an absolute video-row number,
 * finds which (filerow, segment) it falls in. Clamps to the last row
 * if `vy` is past the end of the file. */
static void editorFileRowAtVideoRow(int32_t vy, int32_t wrapcols, int32_t *out_filerow, int32_t *out_seg) {
    int32_t vy_left = vy;
    for (int32_t i = 0; i < E.document.buffer.row_count; i++) {
        int32_t h = editorRowVideoHeight(i, wrapcols);
        if (vy_left < h) {
            *out_filerow = i;
            *out_seg = vy_left;
            return;
        }
        vy_left -= h;
    }
    *out_filerow = E.document.buffer.row_count > 0 ? E.document.buffer.row_count - 1 : 0;
    *out_seg = 0;
}

/* Total number of video rows across the whole file (sum of every
 * row's visual height). Used to clamp scrolling past the end. */
static int32_t editorTotalVideoRows(int32_t wrapcols) {
    int32_t total = 0;
    for (int32_t i = 0; i < E.document.buffer.row_count; i++)
        total += editorRowVideoHeight(i, wrapcols);
    return total;
}

/* Converts a 1-based (screen_col, screen_row) terminal coordinate --
 * exactly what an SGR mouse report gives (see MOUSE_EVENT_KEY) -- into
 * a (file row, file column) cursor position, clamped to the nearest
 * valid spot if the click landed outside the text (e.g. past the end
 * of a short line, in the gutter, or below the last line). Inverse of
 * the cursor-positioning math in editorRefreshScreen() (see
 * "cursor_row + 1 + (S.show_top_bar ? 1 : 0)" / "cursor_col +
 * editorGutterWidth() + 1" there) -- kept as its own function since
 * both need the exact same coordinate transform and must not drift
 * apart from each other. Clicks in the gutter or status/message bars
 * are the caller's responsibility to filter out first (this function
 * assumes a click inside the text area). */
static void editorMouseToCursor(int32_t screen_col, int32_t screen_row, int32_t *out_cy, int32_t *out_cx) {
    int32_t gutter = editorGutterWidth();
    int32_t wrapcols = editorSoftWrapCols();

    int32_t cursor_row = screen_row - 1 - (S.show_top_bar ? 1 : 0);
    int32_t cursor_col = screen_col - 1 - gutter;
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_col < 0) cursor_col = 0;

    int32_t filerow, cx;
    if (wrapcols > 0) {
        int32_t vy = E.view.rowoff + cursor_row;
        int32_t seg;
        editorFileRowAtVideoRow(vy, wrapcols, &filerow, &seg);
        if (E.document.buffer.row_count == 0) {
            *out_cy = 0; *out_cx = 0;
            return;
        }
        erow *row = &E.document.buffer.rows[filerow];
        int32_t nseg = editorRowSegments(row, wrapcols);
        if (seg >= nseg) seg = nseg - 1;
        int32_t target_rx = row->seg_start_rx[seg] + cursor_col;
        /* Keep the full segment extent here, including the blank cells
         * of a trailing tab. editorSegVisibleEndRx() intentionally
         * trims them for visual End navigation, but trimming them for a
         * click would make every cell of that tab map before the tab. */
        int32_t seg_end_rx = (seg + 1 < nseg) ? row->seg_start_rx[seg + 1] :
            editorRowCxToRx(row, row->size);
        if (target_rx > seg_end_rx) target_rx = seg_end_rx;
        cx = editorRowRxToCx(row, target_rx);
    } else {
        filerow = E.view.rowoff + cursor_row;
        if (filerow >= E.document.buffer.row_count) filerow = E.document.buffer.row_count > 0 ? E.document.buffer.row_count - 1 : 0;
        if (E.document.buffer.row_count == 0) {
            *out_cy = 0; *out_cx = 0;
            return;
        }
        erow *row = &E.document.buffer.rows[filerow];
        cx = editorRowRxToCx(row, E.view.coloff + cursor_col);
    }

    *out_cy = filerow;
    *out_cx = cx;
}

static void editorScroll(void) {
    E.document.cursor.rx = 0;
    if (E.document.cursor.cy < E.document.buffer.row_count)
        E.document.cursor.rx = editorRowCxToRx(&E.document.buffer.rows[E.document.cursor.cy], E.document.cursor.cx);

    /* See E.view.free_scroll's declaration in tinyedit.h: the mouse wheel
     * sets this to scroll the view without the cursor being dragged
     * along to follow it -- consumed (and cleared) here, right before
     * the normal follow-the-cursor logic would otherwise immediately
     * undo that by re-centering E.view.rowoff around the (stationary)
     * cursor. One-shot: any redraw after this one goes through the
     * normal path again, so real cursor movement still keeps the
     * cursor on screen as usual. */
    if (E.view.free_scroll) {
        E.view.free_scroll = 0;
        return;
    }

    int32_t wrapcols = editorSoftWrapCols();

    if (wrapcols > 0) {
        /* Wrapped mode: vertical scrolling is in video rows, horizontal
         * scrolling is disabled (a wrapped line never exceeds the text
         * width by construction, so E.view.coloff stays 0). E.document.cursor.cy can be one
         * past the last row (e.g. right after deleting the last line,
         * or mid-edit before clamping) -- there's no row to segment
         * there, so cursor_vy is just the video row right after the
         * last line (0 for an empty buffer). */
        int32_t cursor_vy;
        if (E.document.cursor.cy < E.document.buffer.row_count) {
            int32_t seg_idx, seg_col;
            editorRxToSegment(&E.document.buffer.rows[E.document.cursor.cy], wrapcols, E.document.cursor.rx, &seg_idx, &seg_col);
            cursor_vy = editorVideoRowOf(E.document.cursor.cy, seg_idx, wrapcols);
        } else {
            cursor_vy = editorTotalVideoRows(wrapcols);
        }

        if (cursor_vy < E.view.rowoff) E.view.rowoff = cursor_vy;
        if (cursor_vy >= E.view.rowoff + E.view.screenrows) E.view.rowoff = cursor_vy - E.view.screenrows + 1;
        if (E.view.rowoff < 0) E.view.rowoff = 0;
        E.view.coloff = 0;
    } else {
        if (E.document.cursor.cy < E.view.rowoff) E.view.rowoff = E.document.cursor.cy;
        if (E.document.cursor.cy >= E.view.rowoff + E.view.screenrows) E.view.rowoff = E.document.cursor.cy - E.view.screenrows + 1;
        if (E.document.cursor.rx < E.view.coloff) E.view.coloff = E.document.cursor.rx;
        int32_t textcols = editorTextCols();
        if (E.document.cursor.rx >= E.view.coloff + textcols) E.view.coloff = E.document.cursor.rx - textcols + 1;
    }
}

/* Draws the render-byte range [seg_from, seg_to) of `filerow` into
 * `ab`, applying selection/search-match highlight -- the body shared
 * by both the unwrapped (one call per file row) and wrapped (one call
 * per visual segment) paths in editorDrawRows(). */
static void editorDrawRowSegment(struct abuf *ab, int32_t filerow, int32_t seg_from, int32_t seg_to,
    uint8_t has_sel, int32_t sel_y0, int32_t sel_x0, int32_t sel_y1, int32_t sel_x1) {
    erow *row = &E.document.buffer.rows[filerow];
    int32_t len = seg_to - seg_from;
    if (len <= 0) return;

    char *line = &row->render[seg_from];
    int32_t row_sel_start = -1, row_sel_end = -1;
    if (has_sel && filerow >= sel_y0 && filerow <= sel_y1) {
        row_sel_start = (filerow == sel_y0) ? sel_x0 : 0;
        row_sel_end = (filerow == sel_y1) ? sel_x1 : row->size;
    }

    int32_t match_start = -1, match_end = -1;
    if (E.search.search_match_y == filerow) {
        match_start = E.search.search_match_x;
        match_end = E.search.search_match_x + E.search.search_match_len;
    }

    int32_t source_byte = 0, render_byte = 0;
    uint8_t in_sel = 0;
    for (int32_t j = 0; j < len; ) {
        int32_t rendercol = seg_from + j;
        /* Selection and search coordinates refer to bytes in chars[],
         * while this loop walks render[]. A tab occupies several render
         * bytes, all of which represent the same source byte. */
        while (source_byte < row->size && render_byte < rendercol) {
            int32_t width = row->chars[source_byte] == '\t'
                ? S.tab_stop - render_byte % S.tab_stop : 1;
            if (render_byte + width > rendercol) break;
            render_byte += width;
            source_byte++;
        }
        int32_t filecol = source_byte;
        size_t char_len = utf8NextCharLen(line, (size_t)j, (size_t)len);
        if (char_len == 0 || (size_t)j + char_len > (size_t)len) char_len = 1;
        int32_t emitted_len = (int32_t)char_len;
        uint8_t should_sel = (row_sel_start >= 0 &&
            filecol >= row_sel_start && filecol < row_sel_end) ||
            (match_start >= 0 && filecol >= match_start && filecol < match_end);
        if (should_sel && !in_sel) {
            const char *sel_color = ansiColorCode(S.color_selection);
            abAppend(ab, sel_color, (int32_t)strlen(sel_color));
            abAppend(ab, "\x1b[7m", 4);
            in_sel = 1;
        } else if (!should_sel && in_sel) {
            abAppendReset(ab);
            in_sel = 0;
        }

        /* Invisibles glyphs get their own color, but only outside
         * selection/search-match highlight -- those take priority
         * (matches how every other editor dims/recolors placeholder
         * glyphs only on plain text, never fighting a highlight for
         * attention). Reset immediately after since these are lone
         * bytes interleaved with normal text, unlike the selection
         * span above which covers a contiguous range. */
        uint8_t is_invisible_glyph = !should_sel && emitted_len == 1 &&
            (line[j] == INVISIBLE_SPACE_GLYPH || line[j] == INVISIBLE_TAB_GLYPH) &&
            S.show_invisibles;
        if (is_invisible_glyph) {
            is_invisible_glyph = source_byte < row->size && render_byte == rendercol &&
                (row->chars[source_byte] == ' ' || row->chars[source_byte] == '\t');
        }
        if (is_invisible_glyph) {
            const char *inv_color = ansiColorCode(S.color_invisibles);
            abAppend(ab, inv_color, (int32_t)strlen(inv_color));
        }

        /* Syntax color: same priority rule as invisibles above (outside
         * selection/search, and not already an invisible glyph -- a
         * glyph substituted for a space/tab has no syntax meaning of
         * its own). row->hl is NULL whenever highlighting isn't active
         * for this row (see editorUpdateRow()), so this is a no-op in
         * that case without an extra flag check. */
        const char *syn_color = NULL;
        if (!should_sel && !is_invisible_glyph) {
            if (row->hl && rendercol < row->rsize) {
                syn_color = syntaxColorFor((enum syntaxHighlight)row->hl[rendercol], &S);
            } else if (S.color_syntax_normal != COLOR_TERMINAL_DEFAULT) {
                /* An unknown extension (and syntax highlighting turned
                 * off) has no hl array, but its text is still normal
                 * text. Do not make the user's normal-text color depend
                 * on filetype detection. */
                syn_color = ansiColorCode(S.color_syntax_normal);
            }
        }
        if (syn_color) abAppend(ab, syn_color, (int32_t)strlen(syn_color));

        /* Emit the complete UTF-8 sequence before resetting the color.
         * ANSI escapes between continuation bytes would split the codepoint
         * and make terminals render replacement diamonds (�), especially
         * visible with accented characters such as é. */
        abAppend(ab, &line[j], emitted_len);

        if (syn_color) abAppendReset(ab);
        if (is_invisible_glyph) abAppendReset(ab);
        j += emitted_len;
    }
    if (in_sel) abAppendReset(ab);
}

static void editorDrawGutter(struct abuf *ab, int32_t gutter, int32_t filerow, uint8_t is_continuation) {
    if (gutter <= 0) return;
    char numbuf[16];
    int32_t safe_gutter = gutter;
    if (safe_gutter > (int32_t)sizeof(numbuf) - 1) safe_gutter = (int32_t)sizeof(numbuf) - 1;
    if (filerow < E.document.buffer.row_count && !is_continuation) {
        snprintf(numbuf, sizeof(numbuf), "%*d ", safe_gutter - 1, filerow + 1);
    } else {
        snprintf(numbuf, sizeof(numbuf), "%*s ", safe_gutter - 1, "");
    }
    const char *gutter_color = ansiColorCode(S.color_gutter);
    abAppend(ab, gutter_color, (int32_t)strlen(gutter_color));
    abAppend(ab, numbuf, safe_gutter);
    abAppendReset(ab);
}

/* Splash lines shown on the empty buffer, centered as a block. NULL is
 * a blank spacer line. Kept short and few: this is the first thing a
 * new user sees and the only place the basic keys are advertised
 * without knowing to press F1, but it still has to fit a small
 * terminal, so it lists the way out and the way to get help rather
 * than trying to summarize the whole keymap. */
/* Splash uses the startup choice; Info chooses again on each opening. */
static const char *const slogans[] = {
    "The terminal editor your fingers already know.",
    "You already know how to exit.",
    "No modes, no spells. Just edit.",
    "Fast in the shell, familiar to your hands.",
    "Text editing, not piano lessons.",
    "Pure C. Familiar keys. Just edit.",
    "Built for muscle memory.",
    "Ctrl+S saves. Ctrl+Q quits.",
    "The terminal editor that respects your desktop habits.",
    "Desktop shortcuts at the command line.",
    "Open a file. Start typing.",
    "Small editor. Familiar shortcuts.",
    "No external libraries. Familiar muscle memory.",
    "Forget :wq. Save with Ctrl+S, quit with Ctrl+Q."
};
static const char *sessionSlogan;

static const char *splashLines[] = {
    "tinyedit",
    NULL, /* session slogan, assigned at startup */
    NULL,
    "version " TE_VERSION,
    "by Roberto Bissanti",
    "MIT licensed -- free to use and redistribute",
    NULL,
    /* Padded to a common width so the block's own centering can't
     * stagger them: these four must share one left edge for their two
     * key columns to line up. */
    "Ctrl-O  open a file        Ctrl-S  save    ",
    "Ctrl-F  find               F4      save as ",
    "Ctrl-Z  undo               F2      settings",
    "Ctrl-Q  quit               F1      help    ",
    NULL,
    "Start typing, or press F3 for details",
};
static const int32_t splashLineCount =
    (int32_t)(sizeof(splashLines) / sizeof(splashLines[0]));

static void editorChooseSlogan(void) {
    uint32_t count = (uint32_t)(sizeof(slogans) / sizeof(slogans[0]));
    uint32_t previous = count;
    for (uint32_t i = 0; i < count; i++)
        if (sessionSlogan == slogans[i]) previous = i;
    uint32_t choices = previous < count ? count - 1 : count;
    uint32_t sample = 0;
    uint32_t threshold = (uint32_t)(0u - choices) % choices;
    uint8_t sampled = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        /* Reject the short remainder so every slogan is equally likely.
         * Avoid the first rand() output: on macOS, related seeds and
         * this list size gave visibly repetitive selections. */
        do {
            size_t used = 0;
            while (used < sizeof(sample)) {
                ssize_t n = read(fd, (char *)&sample + used, sizeof(sample) - used);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                used += (size_t)n;
            }
            sampled = used == sizeof(sample);
        } while (sampled && sample < threshold);
        close(fd);
    }
    if (!sampled) {
        /* Best-effort fallback for systems without /dev/urandom. */
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
        for (int i = 0; i < 8; i++) sample = (uint32_t)rand();
    }
    uint32_t chosen = sample % choices;
    if (previous < count && chosen >= previous) chosen++;
    sessionSlogan = slogans[chosen];
    if (!splashLines[1]) splashLines[1] = sessionSlogan;
}

/* Draws the splash line belonging to video row `y`, or a plain "~" when
 * that row isn't part of the block. The block is centered both ways and
 * skipped entirely on a terminal too short to hold it, where "~"
 * everywhere is better than a half-drawn banner. */
static void editorDrawSplashRow(struct abuf *ab, int32_t y, int32_t textcols) {
    /* All-or-nothing: a block that doesn't fit is dropped rather than
     * clipped, so a short terminal never shows a banner missing its
     * last lines. Checking the height directly (not just top >= 0)
     * matters because a negative top still leaves rows 0.. inside
     * [top, top + count), which would draw a truncated block. */
    if (splashLineCount > E.view.screenrows) {
        abAppend(ab, "~", 1);
        return;
    }

    int32_t top = (E.view.screenrows - splashLineCount) / 2;
    if (y < top || y >= top + splashLineCount) {
        abAppend(ab, "~", 1);
        return;
    }

    const char *line = splashLines[y - top];
    char display_line[256];
    if (line == NULL) {
        abAppend(ab, "~", 1);
        return;
    }

    /* Center the block as a unit on its widest line, then center each
     * line within that block. Centering every line on the full screen
     * width instead would stagger the key rows, whose two columns only
     * line up if they share one left edge. */
    int32_t widest = 0;
    for (int32_t i = 0; i < splashLineCount; i++) {
        if (!splashLines[i]) continue;
        char display[256];
        editorShortcutText(display, sizeof(display), splashLines[i]);
        int32_t w = (int32_t)strlen(display);
        if (w > widest) widest = w;
    }
    if (widest > textcols) widest = textcols;

    editorShortcutText(display_line, sizeof(display_line), line);
    int32_t len = (int32_t)strlen(display_line);
    if (len > textcols) len = textcols;
    int32_t padding = (textcols - widest) / 2 + (widest - len) / 2;
    if (padding) {
        abAppend(ab, "~", 1);
        padding--;
    }
    while (padding--) abAppend(ab, " ", 1);
    abAppend(ab, display_line, len);
}

static void editorDrawRows(struct abuf *ab) {
    int32_t sel_y0 = 0, sel_x0 = 0, sel_y1 = 0, sel_x1 = 0;
    uint8_t has_sel = editorSelectionRange(&E.document.selection, &E.document.cursor, &sel_y0, &sel_x0, &sel_y1, &sel_x1);
    int32_t gutter = editorGutterWidth();
    int32_t textcols = editorTextCols();
    int32_t wrapcols = editorSoftWrapCols();

    if (wrapcols == 0) {
        for (int32_t y = 0; y < E.view.screenrows; y++) {
            int32_t filerow = y + E.view.rowoff;
            editorDrawGutter(ab, gutter, filerow, 0);

            if (filerow >= E.document.buffer.row_count) {
                if (E.document.buffer.row_count == 0) {
                    editorDrawSplashRow(ab, y, textcols);
                } else {
                    abAppend(ab, "~", 1);
                }
            } else {
                int32_t len = E.document.buffer.rows[filerow].rsize - E.view.coloff;
                if (len < 0) len = 0;
                if (len > textcols) len = textcols;
                editorDrawRowSegment(ab, filerow, E.view.coloff, E.view.coloff + len,
                    has_sel, sel_y0, sel_x0, sel_y1, sel_x1);
            }

            abAppend(ab, "\x1b[K", 3);
            abAppend(ab, "\r\n", 2);
        }
        return;
    }

    /* Wrapped mode: each video row corresponds to one visual segment
     * of a logical row, resolved via editorFileRowAtVideoRow(). */
    for (int32_t y = 0; y < E.view.screenrows; y++) {
        int32_t vy = y + E.view.rowoff;
        int32_t filerow, seg;

        if (vy >= editorTotalVideoRows(wrapcols)) {
            if (E.document.buffer.row_count == 0) {
                editorDrawGutter(ab, gutter, 0, 1);
                editorDrawSplashRow(ab, y, textcols);
            } else {
                editorDrawGutter(ab, gutter, E.document.buffer.row_count, 0);
                abAppend(ab, "~", 1);
            }
            abAppend(ab, "\x1b[K", 3);
            abAppend(ab, "\r\n", 2);
            continue;
        }

        editorFileRowAtVideoRow(vy, wrapcols, &filerow, &seg);

        erow *row = &E.document.buffer.rows[filerow];
        int32_t nseg = editorRowSegments(row, wrapcols);
        int32_t seg_from = row->seg_start[seg];
        int32_t seg_to = editorSegVisibleEnd(row, nseg, row->seg_start, seg);

        editorDrawGutter(ab, gutter, filerow, seg > 0);
        editorDrawRowSegment(ab, filerow, seg_from, seg_to, has_sel, sel_y0, sel_x0, sel_y1, sel_x1);

        /* End-of-line glyph: only after the LAST visual segment of a
         * logical row (seg == nseg - 1), not after every wrapped
         * video row -- a soft-wrap point isn't a real newline in the
         * file, only the row's actual end is. Appended rather than
         * substituted into render (see editorUpdateRow()), so it's
         * exempt from the single-byte-glyph constraint that applies
         * to in-line invisibles. */
        if (S.show_invisibles && seg == nseg - 1) {
            /* editorSegVisibleEnd() hides the space cells that complete
             * a tab stop. They still have visual width, so restore that
             * width before placing the end-of-line marker; otherwise a
             * trailing tab makes '$' appear immediately after '>'. */
            int32_t hidden_tab_fill = row->rsize - seg_to;
            while (hidden_tab_fill-- > 0) abAppend(ab, " ", 1);
            const char *eol_color = ansiColorCode(S.color_invisibles);
            abAppend(ab, eol_color, (int32_t)strlen(eol_color));
            abAppend(ab, "$", 1);
            abAppendReset(ab);
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
static int32_t editorCountChars(void) {
    int32_t count = 0;
    for (int32_t i = 0; i < E.document.buffer.row_count; i++) {
        erow *row = &E.document.buffer.rows[i];
        size_t pos = 0;
        while (pos < (size_t)row->size) {
            size_t clen = utf8NextCharLen(row->chars, pos, (size_t)row->size);
            if (clen == 0) clen = 1;
            pos += clen;
            count++;
        }
        if (i < E.document.buffer.row_count - 1) count++; /* newline joining this row to the next */
    }
    return count;
}

/* Filetype name for the status bar, derived from E.document.file.filename's
 * extension (e.g. "tinyedit.c" -> "C"), via the built-in table plus
 * any ~/.tinyeditrc "filetype.<ext> = <Name>" overrides. Returns NULL
 * if there's no filename, no extension, or the extension is unknown --
 * callers should omit the field entirely rather than show a blank. */
static const char *editorFiletypeLabel(void) {
    if (!E.document.file.filename) return NULL;
    const char *dot = strrchr(E.document.file.filename, '.');
    /* No dot, or a dot with nothing after it (e.g. "Makefile",
     * "foo."): no extension to look up. A leading dot with no other
     * dot (e.g. ".gitignore") also has no meaningful extension. */
    if (!dot || dot[1] == '\0' || dot == E.document.file.filename) return NULL;
    return filetypeForExtension(dot + 1);
}

/* Top title bar (optional, show_top_bar): filename/path + dirty
 * indicator. Kept separate from the bottom status bar, which shows
 * transient position/count info instead -- the top bar acts as a
 * persistent title that stays visible while scrolling. */
/* Reverse-video (\x1b[7m) swaps whatever foreground/background is
 * currently active -- with a configured color_background, that would
 * swap it into the FOREGROUND of the bar text instead of leaving it
 * as an actual background, mixing the two color systems in a way
 * that made bar text unreadable (foreground ending up the same as
 * the editor's background). The bars always reset to no background
 * right before setting bar_color/reverse-video, so their look stays
 * exactly what it was before color_background existed regardless of
 * that setting; abAppendReset() below (after the bar) restores the
 * editor's own background for the rows that follow. Skipped entirely
 * when color_background is COLOR_TERMINAL_DEFAULT (off) so the byte
 * stream is unchanged from before this setting existed in that,
 * still-default, case. */
static void editorDrawTopBar(struct abuf *ab) {
    if (!S.show_top_bar) return;

    if (S.color_background != COLOR_TERMINAL_DEFAULT) abAppend(ab, "\x1b[49m", 5);
    const char *bar_color = ansiColorCode(S.color_statusbar);
    abAppend(ab, bar_color, (int32_t)strlen(bar_color));
    abAppend(ab, "\x1b[7m", 4);

    char status[160];
    int32_t len = snprintf(status, sizeof(status), " %s%s",
        E.document.file.filename ? E.document.file.filename : "[No Name]", E.document.file.dirty ? " (modified)" : "");
    if (len < 0) len = 0;
    if (len > E.view.screencols) len = E.view.screencols;

    abAppend(ab, status, len);
    while (len < E.view.screencols) {
        abAppend(ab, " ", 1);
        len++;
    }
    abAppendReset(ab);
    abAppend(ab, "\r\n", 2);
}

static void editorDrawStatusBar(struct abuf *ab) {
    if (S.color_background != COLOR_TERMINAL_DEFAULT) abAppend(ab, "\x1b[49m", 5);
    const char *bar_color = ansiColorCode(S.color_statusbar);
    abAppend(ab, bar_color, (int32_t)strlen(bar_color));
    abAppend(ab, "\x1b[7m", 4);
    char status[96], rstatus[80];
    /* Filename only shown here when the top bar is off -- otherwise
     * it's already there, showing it in both places is redundant.
     * The dirty indicator always shows here regardless of the top
     * bar, so it stays visible even if the user disables it. */
    int32_t len;
    if (S.show_top_bar) {
        len = snprintf(status, sizeof(status), "%d lines, %d chars %s",
            E.document.buffer.row_count, editorCountChars(), E.document.file.dirty ? "(modified)" : "");
    } else {
        len = snprintf(status, sizeof(status), "%.20s - %d lines, %d chars %s",
            E.document.file.filename ? E.document.file.filename : "[No Name]", E.document.buffer.row_count, editorCountChars(),
            E.document.file.dirty ? "(modified)" : "");
    }

    const char *filetype = editorFiletypeLabel();
    const char *ending = editorEffectiveLineEnding() == LINE_ENDING_CRLF ? "CRLF" : "LF";
    const char *mixed = E.document.file.line_endings_mixed ? "*" : "";
    int32_t rlen;
    if (filetype)
        rlen = snprintf(rstatus, sizeof(rstatus), "%s | %s%s | %d/%d: C %d",
            filetype, ending, mixed, E.document.cursor.cy + 1, E.document.buffer.row_count, E.document.cursor.cx + 1);
    else
        rlen = snprintf(rstatus, sizeof(rstatus), "%s%s | %d/%d: C %d",
            ending, mixed, E.document.cursor.cy + 1, E.document.buffer.row_count, E.document.cursor.cx + 1);
    if (len > E.view.screencols) len = E.view.screencols;
    abAppend(ab, status, len);
    while (len < E.view.screencols) {
        if (E.view.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppendReset(ab);
    abAppend(ab, "\r\n", 2);
}

static void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int32_t msglen = (int32_t)strlen(E.ui.statusmsg);
    const char *msg = E.ui.statusmsg;
    if (msglen > E.view.screencols) {
        /* Show the TAIL, not the head, when the message doesn't fit.
         * Prompts built with editorPromptCB() put the fixed
         * instructions first and the live text being typed last (see
         * editorFind()/editorFindAndReplace()) -- truncating from the
         * end, as this used to do unconditionally, would cut off
         * exactly the part the user is actively looking at (what
         * they're typing, and the cursor position editorRefreshScreen()
         * places at the end of it) on any terminal too narrow for the
         * full prompt, leaving them unable to see what they're
         * searching for. Keeping the tail means the fixed instructions
         * scroll off first instead. */
        msg += msglen - E.view.screencols;
        msglen = E.view.screencols;
    }
    if (msglen && (E.ui.statusmsg_sticky || time(NULL) - E.ui.statusmsg_time < 5))
        abAppend(ab, msg, msglen);
}

static void editorRefreshScreen(void) {
    uint8_t need_full_clear = 0;
    if (winsize_changed) {
        winsize_changed = 0;
        int32_t rows, cols;
        if (terminalGetWindowSize(&rows, &cols) == 0) {
            E.view.screenrows = rows - 2 - (S.show_top_bar ? 1 : 0); /* status bar + message bar (+ top bar) */
            E.view.screencols = cols;
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
    abAppend(&ab, S.cursor_style == CURSOR_BAR ? "\x1b[6 q" : "\x1b[2 q", 5);
    /* Set before the clear (not after) so the cells \x1b[2J erases
     * pick up this background too, not just the rows/gutter text
     * drawn below -- \x1b[2J fills erased cells with whatever SGR
     * background is currently active, same as \x1b[K per line. */
    const char *bg = ansiBgColorCode(S.color_background);
    if (bg[0]) abAppend(&ab, bg, (int32_t)strlen(bg));
    if (need_full_clear) abAppend(&ab, "\x1b[2J", 4);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawTopBar(&ab);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    {
        int32_t wrapcols = editorSoftWrapCols();
        int32_t cursor_row, cursor_col;
        if (wrapcols > 0 && E.document.cursor.cy < E.document.buffer.row_count) {
            int32_t seg_idx, seg_col;
            editorRxToSegment(&E.document.buffer.rows[E.document.cursor.cy], wrapcols, E.document.cursor.rx, &seg_idx, &seg_col);
            cursor_row = editorVideoRowOf(E.document.cursor.cy, seg_idx, wrapcols) - E.view.rowoff;
            cursor_col = seg_col;
        } else if (wrapcols > 0) {
            /* E.document.cursor.cy is one past the last row (empty buffer, or right
             * after deleting the last line): video row right after
             * the last line's segments, column 0. */
            cursor_row = editorTotalVideoRows(wrapcols) - E.view.rowoff;
            cursor_col = 0;
        } else {
            cursor_row = E.document.cursor.cy - E.view.rowoff;
            cursor_col = E.document.cursor.rx - E.view.coloff;
        }
        snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
            cursor_row + 1 + (S.show_top_bar ? 1 : 0),
            cursor_col + editorGutterWidth() + 1);
    }
    abAppend(&ab, buf, (int32_t)strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

static void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.ui.statusmsg, sizeof(E.ui.statusmsg), fmt, ap);
    va_end(ap);
    E.ui.statusmsg_time = time(NULL);
    E.ui.statusmsg_sticky = 0;
}

/* Like editorSetStatusMessage(), but the message stays in the message
 * bar until replaced by another call to either function -- no 5s
 * timeout. Used for the startup shortcut hint, which should remain
 * visible until the user does something that produces a real status
 * update, not vanish on its own after a few seconds. */
static void editorSetStatusMessageSticky(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.ui.statusmsg, sizeof(E.ui.statusmsg), fmt, ap);
    va_end(ap);
    E.ui.statusmsg_time = time(NULL);
    E.ui.statusmsg_sticky = 1;
}

/* ---- input ------------------------------------------------------------------ */

/* Converts an (rx, in-segment column) target on a given video row back
 * into a cx (char offset) on that row's logical line -- inverse of
 * editorRowCxToRx() restricted to one visual segment. Used by Up/Down
 * in wrapped mode to preserve the cursor's horizontal position when
 * moving between visual segments, same idea as unwrapped Up/Down
 * preserving E.document.cursor.rx via editorRowCxToRx()/clamping below. */
static int32_t editorSegColToCx(erow *row, int32_t seg_from_rx, int32_t target_col) {
    int32_t target_rx = seg_from_rx + target_col;
    int32_t rx = 0, j = 0;
    while (j < row->size) {
        if (rx >= target_rx) break;
        if (row->chars[j] == '\t') {
            rx += (S.tab_stop - 1) - (rx % S.tab_stop);
            rx++;
            j++;
            continue;
        }
        size_t clen = utf8NextCharLen(row->chars, (size_t)j, (size_t)row->size);
        if (clen == 0) clen = 1;
        rx += utf8SingleCharWidth(row->chars + j, clen);
        j += (int32_t)clen;
    }
    return j;
}

/* Up/Down cursor movement when soft-wrap is active: moves by one
 * visual segment instead of one logical row (see TODO.md -- decided
 * to match modern editor behavior instead of jumping whole paragraphs
 * on a wrapped long line). Preserves the target render column across
 * segments/rows, same intent as the unwrapped path preserving E.document.cursor.rx. */
static void editorMoveCursorWrapped(int32_t key, int32_t wrapcols) {
    int32_t seg_idx, seg_col;
    /* Derive rx from the current file position instead of trusting the
     * frame cache E.document.cursor.rx. PageUp/PageDown call this function repeatedly
     * before the next redraw; E.document.cursor.cx changes on every iteration while
     * E.document.cursor.rx would otherwise remain stale, making a whole-page jump move
     * only one visual segment. */
    int32_t current_rx = editorRowCxToRx(&E.document.buffer.rows[E.document.cursor.cy], E.document.cursor.cx);
    editorRxToSegment(&E.document.buffer.rows[E.document.cursor.cy], wrapcols, current_rx, &seg_idx, &seg_col);

    int32_t target_vy = editorVideoRowOf(E.document.cursor.cy, seg_idx, wrapcols) + (key == ARROW_UP ? -1 : 1);
    if (target_vy < 0) target_vy = 0;
    int32_t total = editorTotalVideoRows(wrapcols);
    if (target_vy >= total) target_vy = total - 1;

    int32_t target_filerow, target_seg;
    editorFileRowAtVideoRow(target_vy, wrapcols, &target_filerow, &target_seg);

    erow *target_row = &E.document.buffer.rows[target_filerow];
    int32_t t_nseg = editorRowSegments(target_row, wrapcols);
    if (target_seg >= t_nseg) target_seg = t_nseg - 1;

    E.document.cursor.cy = target_filerow;
    E.document.cursor.cx = editorSegColToCx(target_row, target_row->seg_start_rx[target_seg], seg_col);
}

static void editorMoveCursor(int32_t key) {
    int32_t wrapcols = editorSoftWrapCols();
    if (wrapcols > 0 && (key == ARROW_UP || key == ARROW_DOWN) && E.document.cursor.cy < E.document.buffer.row_count) {
        editorMoveCursorWrapped(key, wrapcols);
        return;
    }

    erow *row = (E.document.cursor.cy >= E.document.buffer.row_count) ? NULL : &E.document.buffer.rows[E.document.cursor.cy];

    switch (key) {
        case ARROW_LEFT:
            if (row && E.document.cursor.cx != 0) {
                size_t back = utf8PrevCharLen(row->chars, (size_t)E.document.cursor.cx);
                E.document.cursor.cx -= (back > 0) ? (int32_t)back : 1;
            } else if (E.document.buffer.rows && E.document.cursor.cy > 0) {
                E.document.cursor.cy--;
                E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy].size;
            } else {
                E.document.cursor.cx = 0;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.document.cursor.cx < row->size) {
                size_t fwd = utf8NextCharLen(row->chars, (size_t)E.document.cursor.cx, (size_t)row->size);
                E.document.cursor.cx += (fwd > 0) ? (int32_t)fwd : 1;
            } else if (row && E.document.cursor.cx == row->size && E.document.cursor.cy < E.document.buffer.row_count - 1) {
                /* End of a line, but not the last one: move to the start
                 * of the next line. At the very end of the document,
                 * stay put instead -- no wraparound past the last
                 * character (mirrors Arrow Left already stopping at
                 * the very start of the document). */
                E.document.cursor.cy++;
                E.document.cursor.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.document.cursor.cy != 0) E.document.cursor.cy--;
            break;
        case ARROW_DOWN:
            if (E.document.buffer.row_count > 0 && E.document.cursor.cy >= E.document.buffer.row_count - 1) {
                /* Down at EOF is End: do not enter a phantom final row. */
                E.document.cursor.cy = E.document.buffer.row_count - 1;
                E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy].size;
            } else if (E.document.cursor.cy < E.document.buffer.row_count) {
                E.document.cursor.cy++;
            }
            break;
    }

    row = (E.document.cursor.cy >= E.document.buffer.row_count) ? NULL : &E.document.buffer.rows[E.document.cursor.cy];
    int32_t rowlen = row ? row->size : 0;
    if (E.document.cursor.cx > rowlen) E.document.cursor.cx = rowlen;
}

/* Word-wise cursor movement for Alt+Left / Alt+Right. Skips whitespace
 * then a run of non-whitespace characters, crossing line boundaries
 * when the cursor is already at the start/end of a line. */
static void editorMoveCursorWord(uint8_t forward) {
    if (forward) {
        if (E.document.cursor.cy >= E.document.buffer.row_count) return;
        erow *row = &E.document.buffer.rows[E.document.cursor.cy];
        if (E.document.cursor.cx == row->size) {
            if (E.document.cursor.cy < E.document.buffer.row_count - 1) {
                E.document.cursor.cy++;
                E.document.cursor.cx = 0;
            }
            return;
        }
        while (E.document.cursor.cx < row->size && isspace((unsigned char)row->chars[E.document.cursor.cx])) E.document.cursor.cx++;
        while (E.document.cursor.cx < row->size && !isspace((unsigned char)row->chars[E.document.cursor.cx])) E.document.cursor.cx++;
    } else {
        if (E.document.cursor.cx == 0) {
            if (E.document.cursor.cy > 0) {
                E.document.cursor.cy--;
                E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy].size;
            }
            return;
        }
        erow *row = &E.document.buffer.rows[E.document.cursor.cy];
        int32_t i = E.document.cursor.cx - 1;
        while (i > 0 && isspace((unsigned char)row->chars[i])) i--;
        while (i > 0 && !isspace((unsigned char)row->chars[i - 1])) i--;
        E.document.cursor.cx = i;
    }
}

/* Normalizes the selection anchor vs the current cursor position into an
 * ordered [start, end) range. Returns 0 and leaves outputs untouched if
 * there is no active selection. */
/* Serializes the given [start_y,start_x) .. [end_y,end_x) half-open range
 * into a malloc'd NUL-terminated buffer, joining lines with '\n'.
 * *outlen receives the length excluding the NUL terminator. */
static char *editorSerializeRange(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x, size_t *outlen) {
    return bufferSerializeRange(&E.document.buffer, start_y, start_x, end_y, end_x, outlen);
}

/* Deletes the given [start_y,start_x) .. [end_y,end_x) half-open range from
 * the buffer and leaves the cursor at start_y,start_x. */
static void editorDeleteRangeRaw(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x) {
    if (start_y == end_y) {
        erow *row = &E.document.buffer.rows[start_y];
        for (int32_t i = 0; i < end_x - start_x; i++)
            editorRowDelChar(row, start_x);
    } else {
        erow *first = &E.document.buffer.rows[start_y];
        first->size = start_x;
        first->chars[first->size] = '\0';

        erow *last = &E.document.buffer.rows[end_y];
        editorRowAppendString(first, &last->chars[end_x], (size_t)(last->size - end_x));
        editorUpdateRow(first);

        for (int32_t y = end_y; y > start_y; y--)
            editorDelRow(y);
    }
    E.document.cursor.cy = start_y;
    E.document.cursor.cx = start_x;
    E.document.file.dirty = 1;
}

static void editorDeleteRange(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x) {
    editorPushUndo(EDIT_OTHER);
    editorDeleteRangeRaw(start_y, start_x, end_y, end_x);
}

/* Inserts `text` (which may contain '\n') at the current cursor position,
 * splitting into new rows as needed, without taking an undo snapshot. */
static void editorInsertTextRaw(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n')
            editorInsertNewlineRaw();
        else
            editorInsertCharRaw((unsigned char)text[i]);
    }
}

/* Replaces the selection captured before key dispatch and keeps deletion plus
 * insertion in one undo step. With no selection this is a plain bulk insert. */
static void editorReplaceSelectionWithText(uint8_t had_sel,
    int32_t sy, int32_t sx, int32_t ey, int32_t ex,
    const char *text, size_t len) {
    if (!had_sel && len == 0) return;
    editorPushUndo(EDIT_OTHER);
    if (had_sel) editorDeleteRangeRaw(sy, sx, ey, ex);
    if (len > 0) editorInsertTextRaw(text, len);
    E.document.selection.active = 0;
}

/* Removes up to one indent level's worth of leading whitespace from
 * `row`, returning how many bytes went away. Mirrors what one indent
 * step inserts, but tolerantly: a single leading tab counts as a whole
 * level regardless of tab_stop, and otherwise up to tab_stop spaces are
 * removed, stopping early at the first non-space so a partially
 * indented line loses only what it actually has. Deliberately handles
 * both tabs and spaces whatever insert_spaces_for_tab says -- outdent
 * has to cope with whatever indentation the file already contains, not
 * just the flavor this editor would produce. */
static int32_t editorRowOutdent(erow *row) {
    if (row->size > 0 && row->chars[0] == '\t') {
        editorRowDelChar(row, 0);
        return 1;
    }
    int32_t removed = 0;
    while (removed < S.tab_stop && row->size > 0 && row->chars[0] == ' ') {
        editorRowDelChar(row, 0);
        removed++;
    }
    return removed;
}

/* Shifts every line touched by the selection one indent level right
 * (`outdent` false) or left (true), as one undo step, keeping the
 * selection over the same lines afterward so the shortcut can be
 * repeated. Blank lines are skipped when indenting -- trailing
 * whitespace on an otherwise empty line is noise, and no editor that
 * does block indent adds it. */
static void editorIndentSelection(uint8_t outdent) {
    int32_t sel_y0, sel_x0, sel_y1, sel_x1;
    if (!editorSelectionRange(&E.document.selection, &E.document.cursor, &sel_y0, &sel_x0, &sel_y1, &sel_x1)) return;

    /* Which lines get shifted. A selection ending at column 0 was
     * dragged onto the next line without covering any of it, so that
     * line is left alone (the convention in every editor with block
     * indent). This narrowing applies to the edit ONLY -- the selection
     * itself must still be restored across its original lines below, or
     * the excluded line would silently drop out of it on every press. */
    int32_t first = sel_y0;
    int32_t last = (sel_y1 > sel_y0 && sel_x1 == 0) ? sel_y1 - 1 : sel_y1;

    editorPushUndo(EDIT_OTHER);

    /* Per-line shift, so each selection endpoint can be moved by what
     * happened to ITS line: with outdent the two lines can lose
     * different amounts (or nothing at all). */
    int32_t delta_y0 = 0, delta_y1 = 0;
    for (int32_t y = first; y <= last; y++) {
        erow *row = &E.document.buffer.rows[y];
        int32_t delta = 0;

        if (outdent) {
            delta = -editorRowOutdent(row);
        } else if (row->size > 0) {
            if (S.insert_spaces_for_tab) {
                for (int32_t i = 0; i < S.tab_stop; i++) editorRowInsertChar(row, 0, ' ');
                delta = S.tab_stop;
            } else {
                editorRowInsertChar(row, 0, '\t');
                delta = 1;
            }
        }

        if (y == sel_y0) delta_y0 = delta;
        if (y == sel_y1) delta_y1 = delta;
    }

    /* Restore the selection over the same lines it covered before, with
     * each end nudged by its own line's shift, so the shortcut can be
     * pressed repeatedly. A column of 0 stays 0: it means "the very
     * start of this line", which is still the start after the line
     * moved -- adding the delta there would push the selection into
     * text it never covered (and, on the excluded last line, make it
     * spill onto a line the user never selected). Otherwise clamp into
     * the line, since an endpoint that sat inside removed indentation
     * has nowhere to land but the new start of text. Anchor and cursor
     * keep their original roles rather than being normalized to
     * start/end -- E.sel_anchor_* is where the user began selecting,
     * and swapping it would flip the direction of any further
     * Shift+Arrow. */
    if (sel_x0 > 0) sel_x0 += delta_y0;
    if (sel_x1 > 0) sel_x1 += delta_y1;
    if (sel_x0 < 0) sel_x0 = 0;
    if (sel_x1 < 0) sel_x1 = 0;
    if (sel_x0 > E.document.buffer.rows[sel_y0].size) sel_x0 = E.document.buffer.rows[sel_y0].size;
    if (sel_x1 > E.document.buffer.rows[sel_y1].size) sel_x1 = E.document.buffer.rows[sel_y1].size;

    uint8_t cursor_at_end = (E.document.cursor.cy > E.document.selection.anchor_y) ||
        (E.document.cursor.cy == E.document.selection.anchor_y && E.document.cursor.cx >= E.document.selection.anchor_x);

    E.document.selection.active = 1;
    if (cursor_at_end) {
        E.document.selection.anchor_y = sel_y0; E.document.selection.anchor_x = sel_x0;
        E.document.cursor.cy = sel_y1; E.document.cursor.cx = sel_x1;
    } else {
        E.document.selection.anchor_y = sel_y1; E.document.selection.anchor_x = sel_x1;
        E.document.cursor.cy = sel_y0; E.document.cursor.cx = sel_x0;
    }
    E.document.file.dirty = 1;
}

/* ---- search / replace ------------------------------------------------------- */

static void editorFindAndReplace(const char *query);

/* In regex replacement text, translate the familiar control-character
 * escapes that can be typed into the single-line prompt. Unknown escapes
 * remain untouched, so a path or a future backreference-like sequence is
 * never silently damaged. The decoded form can contain real newlines and
 * is therefore returned with an explicit byte length. */
static char *editorDecodeRegexReplacement(const char *raw, size_t *out_len) {
    size_t raw_len = strlen(raw);
    char *decoded = teMalloc(raw_len + 1);
    size_t dst = 0;

    for (size_t src = 0; src < raw_len; src++) {
        if (raw[src] == '\\' && src + 1 < raw_len) {
            char next = raw[src + 1];
            if (next == 'n' || next == 't' || next == 'r' || next == '\\') {
                src++;
                if (next == 'n') decoded[dst++] = '\n';
                else if (next == 't') decoded[dst++] = '\t';
                else if (next == 'r') decoded[dst++] = '\r';
                else decoded[dst++] = '\\';
                continue;
            }
        }
        decoded[dst++] = raw[src];
    }

    decoded[dst] = '\0';
    *out_len = dst;
    return decoded;
}

/* POSIX ERE deliberately has no portable \t/\n shorthand. Decode the
 * familiar controls ourselves before handing the pattern to regcomp(),
 * matching the replacement prompt's existing escape behaviour. */
static char *editorDecodeRegexPattern(const char *raw) {
    size_t len = strlen(raw);
    char *decoded = teMalloc(len + 1);
    size_t dst = 0;
    for (size_t src = 0; src < len; src++) {
        if (raw[src] == '\\' && src + 1 < len) {
            char next = raw[src + 1];
            /* Preserve \\ exactly. In particular, the regex \\n means
             * a literal backslash followed by n, not an actual newline. */
            if (next == '\\') {
                decoded[dst++] = raw[src++];
                decoded[dst++] = raw[src];
                continue;
            }
            if (next == 't' || next == 'n' || next == 'r') {
                src++;
                decoded[dst++] = next == 't' ? '\t' : next == 'n' ? '\n' : '\r';
                continue;
            }
        }
        decoded[dst++] = raw[src];
    }
    decoded[dst] = '\0';
    return decoded;
}

/* Finds the LAST regex match on `row` that starts at or before column
 * `limit_x` (inclusive), storing its start offset/length in *out_x/
 * *out_len. Returns 1 if any match qualifies, 0 otherwise. There is no
 * POSIX-portable way to search backward with <regex.h> (REG_STARTEND,
 * which would let this restrict the search window directly, is a
 * BSD/macOS extension absent from glibc -- and this project targets
 * both macOS and Linux), so this re-runs regexec()
 * repeatedly from increasing start offsets and keeps the rightmost
 * match that still qualifies, mirroring how the literal-substring
 * backward search below already works (memcmp() at every offset up to
 * `limit_x`). Not the fastest reverse-regex-search algorithm, but rows
 * are typically well under a few hundred columns, and this only runs
 * on Shift/Arrow-Up/Left inside an interactive search, not per
 * keystroke of typing the query. */
static uint8_t editorRegexFindLastOnRow(const regex_t *re, erow *row, int32_t limit_x,
    int32_t *out_x, int32_t *out_len) {
    uint8_t found = 0;
    int32_t search_from = 0;
    while (search_from <= row->size) {
        regmatch_t m;
        if (regexec(re, &row->chars[search_from], 1, &m, search_from > 0 ? REG_NOTBOL : 0) != 0)
            break;
        int32_t mx = search_from + (int32_t)m.rm_so;
        int32_t mlen = (int32_t)(m.rm_eo - m.rm_so);
        if (mx > limit_x) break;
        *out_x = mx;
        *out_len = mlen;
        found = 1;
        /* Advance the search start to just past this match's END (not
         * its start) so the next iteration looks for the FOLLOWING
         * match, not a sub-match nested inside the one just found --
         * advancing to mx + 1 instead (one byte past the START) would
         * resume scanning from INSIDE a multi-byte match, and
         * regexec() would happily find a shorter match entirely
         * contained within it (e.g. "[0-9]+" matching "222" at mx=5,
         * then resuming at mx+1=6 finds "22" at the new mx=6, which
         * overwrites out_x/out_len with a wrong, truncated result --
         * this was the bug: Arrow-Up landed on a partial match instead
         * of the real previous one). Always advances by at least 1
         * (mlen can be 0 for a pattern that matches empty, e.g. "a*"),
         * so an empty match can't loop forever, and overlapping
         * non-nested matches starting after this one's end are still
         * found normally by continuing the scan from there. */
        search_from = mx + (mlen > 0 ? mlen : 1);
    }
    return found;
}

/* Searches for `query` starting at (from_y, from_x), moving in `dir`
 * (1 forward, -1 backward), wrapping around the whole file. When
 * E.search.regex_mode is set, `query` is compiled as a POSIX extended
 * regular expression (<regex.h>, part of libc, so it adds no external
 * dependency) instead of
 * matched as a literal substring; a malformed pattern is treated as
 * "no match" rather than surfacing regcomp()'s error, consistent with
 * how an empty query already means "no match" below rather than an
 * error dialog. On success sets E.document.cursor.cy/E.document.cursor.cx to the match start, updates
 * E.search_match_*, and returns 1. On failure clears E.search.search_match_y
 * to -1 and returns 0. */
/* Finds a match from the requested position. Interactive search passes
 * wrap=1 so repeated arrows cycle through the document; replace passes
 * wrap=0 so a replacement that still matches the query cannot send the
 * traversal back to the beginning forever. */
static uint8_t editorFindFrom(const char *query, int32_t from_y, int32_t from_x,
    int32_t dir, uint8_t wrap) {
    size_t qlen = strlen(query);
    if (qlen == 0 || E.document.buffer.row_count == 0) {
        E.search.search_match_y = -1;
        return 0;
    }

    regex_t re;
    uint8_t have_re = 0;
    if (E.search.regex_mode) {
        char *pattern = editorDecodeRegexPattern(query);
        int32_t compile_failed = regcomp(&re, pattern, REG_EXTENDED) != 0;
        free(pattern);
        if (compile_failed) {
            E.search.search_match_y = -1;
            return 0;
        }
        have_re = 1;
    }

    int32_t y = from_y;
    int32_t x = from_x;
    uint8_t result = 0;

    for (int32_t steps = 0; steps <= E.document.buffer.row_count; steps++) {
        erow *row = &E.document.buffer.rows[y];
        int32_t mx = -1, mlen = 0;

        if (have_re) {
            if (dir == 1) {
                if (x <= row->size) {
                    regmatch_t m;
                    if (regexec(&re, &row->chars[x], 1, &m, x > 0 ? REG_NOTBOL : 0) == 0) {
                        mx = x + (int32_t)m.rm_so;
                        mlen = (int32_t)(m.rm_eo - m.rm_so);
                    }
                }
            } else {
                if (editorRegexFindLastOnRow(&re, row, x, &mx, &mlen)) {
                    /* mx/mlen already set by the helper. */
                }
            }
        } else if (dir == 1) {
            if (x <= row->size) {
                char *match = strstr(&row->chars[x], query);
                if (match) { mx = (int32_t)(match - row->chars); mlen = (int32_t)qlen; }
            }
        } else {
            /* Backward: scan for the last match starting at or before
             * column x on this row. */
            int32_t limit = x;
            if (limit > row->size - (int32_t)qlen) limit = row->size - (int32_t)qlen;
            for (int32_t i = 0; i <= limit; i++) {
                if (memcmp(&row->chars[i], query, qlen) == 0) mx = i;
            }
            if (mx >= 0) mlen = (int32_t)qlen;
        }

        if (mx >= 0) {
            E.document.cursor.cy = y;
            E.document.cursor.cx = mx;
            E.search.search_match_y = y;
            E.search.search_match_x = mx;
            E.search.search_match_len = mlen;
            result = 1;
            break;
        }

        if (dir == 1) {
            if (!wrap && y == E.document.buffer.row_count - 1) break;
            y = (y + 1) % E.document.buffer.row_count;
            x = 0;
        } else {
            if (!wrap && y == 0) break;
            y = (y - 1 + E.document.buffer.row_count) % E.document.buffer.row_count;
            x = E.document.buffer.rows[y].size;
        }
    }

    if (have_re) regfree(&re);
    if (result) return 1;

    E.search.search_match_y = -1;
    return 0;
}

static void editorFindCallback(char *query, int32_t key) {
    static int32_t last_cy = -1, last_cx = -1, last_len = 0;

    if (key == '\r' || key == '\x1b') {
        if (key == '\x1b') {
            E.document.cursor.cx = E.search.saved_cx;
            E.document.cursor.cy = E.search.saved_cy;
            E.view.rowoff = E.search.saved_rowoff;
            E.view.coloff = E.search.saved_coloff;
        }
        E.search.search_match_y = -1;
        last_cy = -1;
        last_cx = -1;
        last_len = 0;
        return;
    }

    if (key == CTRL_KEY('r')) {
        E.search.switch_to_replace = 1;
        return;
    }

    if (key == CTRL_KEY('g')) {
        E.search.regex_mode = !E.search.regex_mode;
        /* Re-run the search from the saved starting position (as if
         * the query had just been retyped) so toggling mode mid-search
         * immediately reflects the new interpretation instead of
         * waiting for the next keystroke -- same reset already done
         * below when a key isn't a recognized navigation/mode key. */
        last_cy = -1;
        last_cx = -1;
        last_len = 0;
        if (strlen(query) > 0) {
            int32_t from_y = E.search.saved_cy, from_x = E.search.saved_cx;
            if (editorFindFrom(query, from_y, from_x, E.search.direction, 1)) {
                last_cy = E.document.cursor.cy;
                last_cx = E.document.cursor.cx;
                last_len = E.search.search_match_len;
            }
        }
        return;
    }

    if (key == ARROW_DOWN || key == ARROW_RIGHT) {
        E.search.direction = 1;
    } else if (key == ARROW_UP || key == ARROW_LEFT) {
        E.search.direction = -1;
    } else {
        E.search.direction = 1;
        last_cy = -1;
        last_cx = -1;
        last_len = 0;
    }

    if (strlen(query) == 0) {
        E.search.search_match_y = -1;
        return;
    }

    int32_t from_y, from_x;
    if (last_cy == -1) {
        from_y = E.search.saved_cy;
        from_x = E.search.saved_cx;
    } else if (E.search.direction == 1) {
        /* Resume just past the END of the previous match, not one byte
         * past its START -- for a literal query the two are the same
         * length-wise (every match is exactly qlen bytes), but a regex
         * match's length varies with what it actually matched (e.g.
         * "[0-9]+" matching "111" is 3 bytes). Starting from
         * last_cx + 1 instead of last_cx + last_len would resume
         * search from INSIDE the previous match whenever it's longer
         * than 1 byte, finding an overlapping sub-match on the same
         * text instead of advancing past it -- this was the bug
         * reported by the user (Arrow-Down on a regex search got stuck
         * re-matching pieces of the same match instead of moving to
         * the next line). */
        from_y = last_cy;
        from_x = last_cx + (last_len > 0 ? last_len : 1);
    } else if (last_cx > 0) {
        /* Backward: resume just before the START of the previous match
         * (searching for the last match that starts at or before this
         * point -- see editorFindFrom()'s dir==-1 handling). Unlike the
         * forward case, the match's length doesn't matter here: moving
         * one byte before the match's own start is what excludes it
         * from being found again, regardless of how long it is. */
        from_y = last_cy;
        from_x = last_cx - 1;
    } else {
        /* Previous match started at column 0 -- there's no valid
         * "one byte before" on this row (clamping to 0 would just
         * re-find the very same match at the very same spot, since
         * editorFindFrom()'s dir==-1 search includes the row's column
         * 0 in its search window). Skip straight to the end of the
         * PREVIOUS row instead, same starting point editorFindFrom()
         * itself uses when it wraps backward past a row with no match
         * -- this was the bug: Arrow-Up got stuck re-finding a
         * column-0 match forever instead of moving to the prior line. */
        from_y = (last_cy - 1 + E.document.buffer.row_count) % E.document.buffer.row_count;
        from_x = E.document.buffer.rows[from_y].size;
    }

    if (editorFindFrom(query, from_y, from_x, E.search.direction, 1)) {
        last_cy = E.document.cursor.cy;
        last_cx = E.document.cursor.cx;
        last_len = E.search.search_match_len;
    }
}

/* Fed to editorPromptCB() as status_fn -- re-evaluated on every prompt
 * redraw, so the "[regex]"/"[literal]" indicator updates the instant
 * Ctrl-G toggles E.search.regex_mode, without editorFind() needing to
 * rebuild the whole prompt string itself. */
static const char *editorFindModeIndicator(void) {
    return E.search.regex_mode ? "[regex]" : "[literal]";
}

static void editorFind(void) {
    E.search.saved_cx = E.document.cursor.cx;
    E.search.saved_cy = E.document.cursor.cy;
    E.search.saved_rowoff = E.view.rowoff;
    E.search.saved_coloff = E.view.coloff;
    E.search.direction = 1;
    E.search.regex_mode = 0;

    /* Long form spells out every shortcut -- shown while there's room
     * for it alongside the query. Once buf grows enough that the two
     * together wouldn't fit the message bar, editorPromptCB() switches
     * to the short form ("Search [mode]: ") instead, and only once
     * THAT doesn't fit either does editorDrawMessageBar()'s
     * scroll-to-keep-tail-visible behavior take over. Three stages,
     * each only kicking in once the previous one runs out of room. */
    E.search.switch_to_replace = 0;
    char long_prompt[128], short_prompt[32];
    snprintf(long_prompt, sizeof(long_prompt),
        "Find %%s (Esc cancel, Arrows jump, %s-R replace, %s-G regex): %%s",
        editorPrimaryModifier(), editorPrimaryModifier());
    snprintf(short_prompt, sizeof(short_prompt), "Find %%s: %%s");
    char *query = editorPromptCB(
        long_prompt, short_prompt,
        editorFindModeIndicator, editorFindCallback);

    if (E.search.switch_to_replace && query) {
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

    /* E.search.regex_mode carries over unchanged from the Ctrl-F search
     * prompt that led here (see editorFindFrom(): it reads the same
     * global, and nothing resets it between Ctrl-R and this function)
     * -- shown here too so the mode isn't invisible during replace,
     * where escape sequences such as "\n" have replacement semantics
     * instead of being inserted literally.
     *
     * Long form echoes the query being replaced (up to 40 chars);
     * short form drops it (still visible highlighted in the buffer
     * behind this prompt, and was just typed in the previous prompt)
     * once there's no room left alongside the replacement text being
     * typed -- same three-stage shrink as editorFind()'s search
     * prompt (long -> short -> editorDrawMessageBar()'s tail-scroll). */
    char replace_prompt_long[112];
    snprintf(replace_prompt_long, sizeof(replace_prompt_long), "Replace %s \"%.40s\" with: %%s",
        E.search.regex_mode ? "[regex]" : "[literal]", query);
    char replace_prompt_short[48];
    snprintf(replace_prompt_short, sizeof(replace_prompt_short), "Replace %s with: %%s",
        E.search.regex_mode ? "[regex]" : "[literal]");
    E.search.switch_to_replace = 0;
    char *replacement_input = editorPromptCB(replace_prompt_long, replace_prompt_short, NULL, NULL);
    if (!replacement_input) return;

    size_t rlen;
    char *replacement;
    if (E.search.regex_mode) {
        replacement = editorDecodeRegexReplacement(replacement_input, &rlen);
        free(replacement_input);
    } else {
        replacement = replacement_input;
        rlen = strlen(replacement);
    }
    uint8_t all = 0;
    int32_t count = 0;

    /* Replace traverses the file once, from start to finish. Reusing the
     * circular navigation search here used to loop forever whenever the
     * replacement still matched `query` (including a no-op replacement). */
    int32_t y = 0, x = 0;
    while (editorFindFrom(query, y, x, 1, 0)) {
        y = E.search.search_match_y;
        x = E.search.search_match_x;
        /* Actual matched length -- NOT strlen(query). In literal mode
         * these are always equal, but in regex mode the match can be
         * shorter or longer than the pattern text itself (e.g.
         * "[0-9]+" matching "42" is length 2, matching "123456" is
         * length 6) -- using strlen(query) here would delete/skip the
         * wrong number of characters as soon as the pattern's length
         * differs from what it actually matched. */
        int32_t mlen = E.search.search_match_len;

        uint8_t do_replace = all;
        if (!all) {
            editorSetStatusMessage(
                "Replace this occurrence? y/n/a(ll)/q(uit)");
            editorRefreshScreen();
            int32_t c = editorReadKey();
            if (c == 'q' || c == '\x1b') break;
            if (c == 'a') { all = 1; do_replace = 1; }
            else if (c == 'y') do_replace = 1;
            else do_replace = 0;
        }

        if (do_replace) {
            if (count == 0) editorPushUndo(EDIT_OTHER);
            erow *row = &E.document.buffer.rows[y];
            for (int32_t k = 0; k < mlen; k++)
                editorRowDelChar(row, x);
            E.document.cursor.cy = y;
            E.document.cursor.cx = x;
            for (size_t k = 0; k < rlen; k++) {
                if (replacement[k] == '\n') {
                    editorInsertNewlineRaw();
                } else {
                    if (E.document.cursor.cy == E.document.buffer.row_count) editorInsertRow(E.document.buffer.row_count, "", 0);
                    editorRowInsertChar(&E.document.buffer.rows[E.document.cursor.cy], E.document.cursor.cx, (unsigned char)replacement[k]);
                    E.document.cursor.cx++;
                }
            }
            count++;
            y = E.document.cursor.cy;
            x = E.document.cursor.cx;
        } else {
            x += mlen;
        }
        /* A zero-length regex match must always consume one original
         * character before the next search. This applies whether the
         * occurrence was replaced, skipped, or replaced with non-empty
         * text; at end-of-row, size+1 makes the non-wrapping finder move
         * to the next row instead of repeatedly growing the same edge. */
        if (mlen == 0) {
            erow *row = &E.document.buffer.rows[y];
            if (x < row->size) {
                size_t advance = utf8NextCharLen(row->chars, (size_t)x, (size_t)row->size);
                x += (int32_t)(advance > 0 ? advance : 1);
            } else {
                x = row->size + 1;
            }
        }
    }

    E.search.search_match_y = -1;
    free(replacement);
    editorSetStatusMessage("Replaced %d occurrence(s).", count);
}

/* ---- settings screen (F2) --------------------------------------------------- */

static int32_t *settingsScreenSlot(struct editorSettings *s, const struct settingDescriptor *d) {
    return (int32_t *)((char *)s + d->offset);
}

static uint8_t editorSettingsIsSyntaxColor(const struct settingDescriptor *d) {
    return strncmp(d->key, "color_syntax_", strlen("color_syntax_")) == 0;
}

/* Any setting whose value is one of enum settingColor -- i.e. every
 * "color_*" key, syntax ones included. Recognized by key prefix rather
 * than by comparing d->enum_names against colorNames, since that array
 * is file-local to settings.c. */
static uint8_t editorSettingsIsColor(const struct settingDescriptor *d) {
    return d->type == SETTING_ENUM &&
        strncmp(d->key, "color_", strlen("color_")) == 0;
}

/* Syntax colors are subordinate to the syntax-highlighting switch: keep
 * them out of both the display and keyboard navigation while disabled. */
static int32_t editorSettingsVisibleCount(const struct editorSettings *edited) {
    int32_t count = 0;
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        if (edited->syntax_highlight || !editorSettingsIsSyntaxColor(&settingDescriptors[i]))
            count++;
    }
    return count;
}

static int32_t editorSettingsDescriptorAt(const struct editorSettings *edited, int32_t visible_idx) {
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        if (!edited->syntax_highlight && editorSettingsIsSyntaxColor(&settingDescriptors[i]))
            continue;
        if (visible_idx-- == 0) return i;
    }
    return -1;
}

/* Steps a SETTING_ENUM value by `delta` (+1/-1), wrapping at both ends.
 * On color_background it skips the 8 "-dim" hues: the dim attribute is
 * foreground-only, so as a background each renders identically to its
 * "-dark" twin (see settingColorIsDim()) and offering both just makes
 * the picker look broken -- you cycle, the swatch doesn't change. A
 * -dim value already in ~/.tinyeditrc still loads and renders fine;
 * stepping away from it lands on a non-dim one and can't come back. */
static void editorSettingsCycleEnum(const struct settingDescriptor *d, int32_t *slot, int32_t delta) {
    uint8_t skip_dim = strcmp(d->key, "color_background") == 0;
    int32_t value = *slot;
    /* Bounded by enum_count: even if every remaining value were dim,
     * this stops after one full lap instead of spinning forever. */
    for (int32_t i = 0; i < d->enum_count; i++) {
        value += delta;
        if (value < 0) value = d->enum_count - 1;
        else if (value >= d->enum_count) value = 0;
        if (!skip_dim || !settingColorIsDim(value)) break;
    }
    *slot = value;
}

/* `scroll_indicator` is '^' when this row is the topmost visible one
 * and there are more settings scrolled off above, 'v' when it's the
 * bottommost visible one and there are more below, or '\0' for no
 * indicator -- drawn in the first column (like the line-number
 * gutter) so it's visible regardless of which row is selected. */
static void editorSettingsDrawRow(struct abuf *ab, int32_t idx, uint8_t selected,
    const struct editorSettings *edited, char scroll_indicator) {
    const struct settingDescriptor *d = &settingDescriptors[idx];
    const int32_t *slot = (const int32_t *)((const char *)edited + d->offset);

    char line[96];
    char valuebuf[48];

    if (d->type == SETTING_BOOL) {
        snprintf(valuebuf, sizeof(valuebuf), "%s", *slot ? "on" : "off");
    } else if (d->type == SETTING_INT) {
        snprintf(valuebuf, sizeof(valuebuf), "%d", *slot);
    } else {
        snprintf(valuebuf, sizeof(valuebuf), "%s", d->enum_names[*slot]);
    }

    const char *label = d->label;
    int32_t len;
    if (editorSettingsIsSyntaxColor(d)) {
        label += strlen("Syntax: ");
        len = snprintf(line, sizeof(line), "%c   %-20s %s",
            scroll_indicator ? scroll_indicator : ' ', label, valuebuf);
    } else {
        len = snprintf(line, sizeof(line), "%c %-22s %s",
            scroll_indicator ? scroll_indicator : ' ', label, valuebuf);
    }
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(line)) len = (int32_t)sizeof(line) - 1;

    if (selected) abAppend(ab, "\x1b[7m", 4);
    abAppend(ab, line, len);
    if (selected) abAppend(ab, "\x1b[m", 3);

    /* Live swatch after the value name: the palette has 24 hues whose
     * names differ only by a "-light"/"-dark"/"-dim" suffix, and
     * stepping through them by name alone gives no way to tell what
     * you actually picked (or to notice you skipped past the variant
     * you wanted) until you leave the panel. color_background is shown
     * as an actual background block since that's how it will be used;
     * every other color setting paints the foreground, matching how it
     * renders in the editor. Drawn outside `line` because the escapes
     * around it aren't printable columns and must not count toward the
     * field widths above. */
    if (editorSettingsIsColor(d)) {
        if (strcmp(d->key, "color_background") == 0) {
            const char *bg = ansiBgColorCode(*slot);
            if (bg[0]) {
                abAppend(ab, "  ", 2);
                abAppend(ab, bg, (int32_t)strlen(bg));
                abAppend(ab, "      ", 6);
                abAppend(ab, "\x1b[m", 3);
            }
        } else {
            const char *fg = ansiColorCode(*slot);
            abAppend(ab, "  ", 2);
            abAppend(ab, fg, (int32_t)strlen(fg));
            abAppend(ab, "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88", 9); /* ███ */
            abAppend(ab, "\x1b[m", 3);
        }
    }

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
static const struct helpEntry helpEntries[] = {
    { NULL, "Movement" },
    { "Arrows, Home, End, PageUp/Down", "Move cursor" },
    { "Ctrl-Home/End (or Ctrl-PageUp/Down)", "Jump to start/end of the file" },
    { "Alt+Left/Right (or Esc b / Esc f)", "Jump by word" },
    { "Mouse click (if enabled, see F2)", "Position cursor" },
    { "Mouse wheel (if enabled, see F2)", "Scroll view (cursor/selection unaffected)" },
    { NULL, "Editing" },
    { "Enter", "New line (auto-indents if enabled)" },
    { "Tab", "Indent (spaces or literal tab, see F2)" },
    { "Tab (with selection)", "Indent every selected line one level" },
    { "Shift-Tab", "Outdent selected lines, or the current one" },
    { "( { [ \" ` $", "Auto-close pair / skip over / wrap selection" },
    { "'", "Same, only if auto-close single quote is on (F2, off by default)" },
    { "Backspace / Delete", "Delete character (UTF-8 aware)" },
    { "Ctrl-Z / Ctrl-Y", "Undo / redo" },
    { "Cmd-S/F/Z/O/W/C/X/A/Q/G/R (Ghostty opt-in)", "Save/find/undo/open/close/copy/cut/select all/quit/regex/replace; see README" },
    { "Paste (terminal-native, e.g. Cmd+V)", "Bulk insert, no auto-close on pasted text" },
    { NULL, "Selection & clipboard" },
    { "Shift+Arrows, Shift+PageUp/Down", "Extend selection" },
    { "Shift+Home/End", "Extend selection to start/end of line" },
    { "Shift+Ctrl+Home/End", "Extend selection to start/end of file" },
    { "Mouse drag (if enabled, see F2)", "Extend selection" },
    { "Ctrl-T", "Toggle selection mode (works on every terminal)" },
    { "Ctrl-A", "Select all" },
    { "Ctrl-C / Ctrl-X / Ctrl-V", "Copy / cut / paste (system clipboard)" },
    { NULL, "Find" },
    { "Ctrl-F", "Incremental find" },
    { "Ctrl-G (inside Find)", "Toggle regex mode (POSIX extended)" },
    { "Ctrl-R (inside Find)", "Switch to find & replace" },
    { NULL, "File & editor" },
    { "Ctrl-S", "Save" },
    { "F4 (or Ctrl-Shift-S, terminal permitting)", "Save as (always prompts for a filename)" },
    { "Ctrl-O", "Open another file (offers to save current file first)" },
    { "Ctrl-W", "Close current file without quitting" },
    { "Ctrl-Q", "Quit (offers to save first if unsaved)" },
    { "F2", "Settings panel (Ctrl-D inside it resets to defaults)" },
    { "F1", "This help screen" },
    { "F3", "Info screen: version, author, current file stats" },
    { NULL, "Configuration files (see README.md for details)" },
    { "~/.tinyeditrc", "All settings from F2, plain key=value, hand-editable" },
    { "~/.tinyedit/syntax/*.conf", "Custom syntax-highlighted languages (any filename)" },
    { "~/.tinyedit/backup/", "Crash-recovery backups (never next to your files)" },
};
static const int32_t helpEntryCount = (int32_t)(sizeof(helpEntries) / sizeof(helpEntries[0]));

/* Full-screen static help overlay (F1). No editable state, so unlike
 * editorSettingsScreen() this doesn't need a local copy or Ctrl-S --
 * any key closes it. Scrolls with Up/Down/PageUp/PageDown if the
 * keybinding list is taller than the terminal. */
static void editorHelpScreen(void) {
    int32_t scroll = 0;

    while (1) {
        struct abuf ab = ABUF_INIT;
        abAppend(&ab, "\x1b[?25l\x1b[H", 9);
        int32_t rows_used = 0;

        {
            const char *header = "\x1b[7m tinyedit -- keybindings (any key to close) \x1b[m\x1b[K\r\n\x1b[K\r\n";
            abAppend(&ab, header, (int32_t)strlen(header));
        }
        rows_used += 2;

        /* Scroll indicator, same gutter-style convention as the F2
         * settings panel (see editorSettingsDrawRow()'s
         * scroll_indicator parameter): '^' on the first visible entry
         * if there are more above, 'v' on the last visible entry if
         * there are more below. Computed up front (last_visible) since
         * the row-drawing loop below needs to know, for EACH row,
         * whether it's the last one that will actually be drawn --
         * that depends on both the screen height and how many entries
         * are left, so it can't be decided until the loop bound is
         * known. */
        int32_t last_visible = scroll;
        {
            int32_t probe_rows = rows_used;
            for (int32_t i = scroll; i < helpEntryCount && probe_rows < E.view.screenrows; i++) {
                last_visible = i;
                probe_rows++;
            }
        }

        for (int32_t i = scroll; i < helpEntryCount && rows_used < E.view.screenrows; i++) {
            char scroll_indicator = ' ';
            if (i == scroll && scroll > 0) scroll_indicator = '^';
            else if (i == last_visible && last_visible < helpEntryCount - 1) scroll_indicator = 'v';

            if (helpEntries[i].key == NULL) {
                abAppend(&ab, "\x1b[1m", 4);
                abAppend(&ab, &scroll_indicator, 1);
                abAppend(&ab, " ", 1);
                abAppend(&ab, helpEntries[i].desc, (int32_t)strlen(helpEntries[i].desc));
                abAppend(&ab, "\x1b[m\x1b[K\r\n", 8);
            } else {
                char key[128], desc[128], line[256];
                editorShortcutText(key, sizeof(key), helpEntries[i].key);
                editorShortcutText(desc, sizeof(desc), helpEntries[i].desc);
                int32_t len = snprintf(line, sizeof(line), "%c   %-38s %s",
                    scroll_indicator, key, desc);
                if (len < 0) len = 0;
                if ((size_t)len >= sizeof(line)) len = (int32_t)sizeof(line) - 1;
                abAppend(&ab, line, len);
                abAppend(&ab, "\x1b[K\r\n", 5);
            }
            rows_used++;
        }

        int32_t total_rows = E.view.screenrows + 2;
        for (; rows_used < total_rows - 1; rows_used++)
            abAppend(&ab, "\x1b[K\r\n", 5);
        if (rows_used < total_rows)
            abAppend(&ab, "\x1b[K", 3);

        abAppend(&ab, "\x1b[H\x1b[?25h", 9);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        int32_t c = editorReadKey();
        int32_t max_scroll = helpEntryCount - (E.view.screenrows - 2);
        if (max_scroll < 0) max_scroll = 0;

        if (c == ARROW_DOWN) {
            if (scroll < max_scroll) scroll++;
        } else if (c == ARROW_UP) {
            if (scroll > 0) scroll--;
        } else if (c == PAGE_DOWN) {
            scroll += E.view.screenrows;
            if (scroll > max_scroll) scroll = max_scroll;
        } else if (c == PAGE_UP) {
            scroll -= E.view.screenrows;
            if (scroll < 0) scroll = 0;
        } else {
            return; /* any other key closes the help screen */
        }
    }
}

static void editorInfoAppendLine(struct abuf *ab, int32_t *rows_used, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int32_t len = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(line)) len = (int32_t)sizeof(line) - 1;
    abAppend(ab, line, len);
    abAppend(ab, "\x1b[K\r\n", 5);
    (*rows_used)++;
}

static void editorInfoAppendSection(struct abuf *ab, int32_t *rows_used, const char *title) {
    const char *section_prefix = "\x1b[1m  ";
    abAppend(ab, section_prefix, (int32_t)strlen(section_prefix));
    abAppend(ab, title, (int32_t)strlen(title));
    abAppend(ab, "\x1b[m\x1b[K\r\n", 8);
    (*rows_used)++;
}

static void editorInfoAppendBlank(struct abuf *ab, int32_t *rows_used) {
    abAppend(ab, "\x1b[K\r\n", 5);
    (*rows_used)++;
}

/* Full-screen static overlay (F3): project identity (version,
 * author, license, homepage) plus live stats about the file currently
 * open -- kept as one screen rather than splitting "about tinyedit"
 * from "about this file" into two separate keys, since both are
 * "information, not action" in the same spirit and a user reaching
 * for one is likely to want the other close by. No editable state, so
 * like editorHelpScreen() any key closes it -- this only reads E/S,
 * never writes them. */
static void editorInfoScreen(void) {
    editorChooseSlogan();
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l\x1b[H", 9);
    int32_t rows_used = 0;

    {
        const char *header = "\x1b[7m tinyedit -- info (any key to close) \x1b[m\x1b[K\r\n\x1b[K\r\n";
        abAppend(&ab, header, (int32_t)strlen(header));
        rows_used += 2;
    }

    editorInfoAppendSection(&ab, &rows_used, "tinyedit");
    editorInfoAppendLine(&ab, &rows_used, "    Version   %s", TE_VERSION);
    {
        char slogan[160];
        editorShortcutText(slogan, sizeof(slogan), sessionSlogan);
        editorInfoAppendLine(&ab, &rows_used, "    %s", slogan);
    }
    editorInfoAppendLine(&ab, &rows_used, "    Author    Roberto Bissanti <roberto.bissanti@gmail.com>");
    editorInfoAppendLine(&ab, &rows_used, "    License   MIT (see LICENSE; utf8.c ported from linenoise, BSD 2-Clause)");
    editorInfoAppendLine(&ab, &rows_used, "    Homepage  https://github.com/robertobissanti/tinyedit");
    editorInfoAppendBlank(&ab, &rows_used);

    editorInfoAppendSection(&ab, &rows_used, "Current file");
    if (E.document.file.filename) {
        editorInfoAppendLine(&ab, &rows_used, "    Path      %s%s", E.document.file.filename, E.document.file.dirty ? " (modified)" : "");
    } else {
        editorInfoAppendLine(&ab, &rows_used, "    Path      [No Name]%s", E.document.file.dirty ? " (modified)" : "");
    }
    const char *filetype = editorFiletypeLabel();
    editorInfoAppendLine(&ab, &rows_used, "    Filetype  %s", filetype ? filetype : "(unknown)");
    editorInfoAppendLine(&ab, &rows_used, "    Lines     %d", E.document.buffer.row_count);
    editorInfoAppendLine(&ab, &rows_used, "    Chars     %d (UTF-8 grapheme clusters, see F1)", editorCountChars());
    editorInfoAppendLine(&ab, &rows_used, "    Cursor    line %d, column %d", E.document.cursor.cy + 1, E.document.cursor.rx + 1);
    editorInfoAppendLine(&ab, &rows_used, "    Encoding  UTF-8");
    if (S.backup_interval > 0) {
        editorInfoAppendLine(&ab, &rows_used, "    Backup    every %ds while unsaved changes exist (see F2)", S.backup_interval);
    } else {
        editorInfoAppendLine(&ab, &rows_used, "    Backup    off (see F2 to enable crash recovery)");
    }
    editorInfoAppendLine(&ab, &rows_used, "    Undo      %d/%d steps used", E.document.history.undo_count, S.undo_max_depth);

    int32_t total_rows = E.view.screenrows + 2;
    for (; rows_used < total_rows - 1; rows_used++)
        abAppend(&ab, "\x1b[K\r\n", 5);
    if (rows_used < total_rows)
        abAppend(&ab, "\x1b[K", 3);

    abAppend(&ab, "\x1b[H\x1b[?25h", 9);
    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);

    editorReadKey(); /* any key closes it */
}

/* Renders one frame of the F2 panel into `ab` -- factored out of
 * editorSettingsScreen()'s main loop so editorSettingsEditInt() can
 * redraw the same panel underneath its own inline numeric prompt,
 * instead of falling through to editorPrompt()/editorRefreshScreen()
 * which draws the main text buffer (the bug this fixes: typing a new
 * value for tab_stop/undo_max_depth/soft_wrap used to flash the
 * editor's own screen, with the file content briefly visible, because
 * editorPrompt() only knows how to redraw the main editor view). */
/* Number of setting rows that fit on screen at once, below the 2-row
 * header and above the blank/note/help rows at the bottom (3 rows
 * reserved for those, matching what editorSettingsRender() always
 * writes after the option list -- the Ctrl-Shift-Z note is the only
 * conditional one and is deliberately not accounted for here, so the
 * reserved space is a safe upper bound rather than something that
 * shifts the visible row count depending on redo_key). Shared between
 * the renderer and the scroll-clamping logic in
 * editorSettingsScreen()/editorSettingsEditInt() so both agree on
 * exactly how many rows are visible. */
static int32_t editorSettingsVisibleRows(void) {
    int32_t visible = E.view.screenrows - 3;
    return visible > 0 ? visible : 1;
}

static void editorSettingsRender(struct abuf *ab, const struct editorSettings *edited,
    int32_t cursor, int32_t scroll, const char *msg) {
    abAppend(ab, "\x1b[?25l\x1b[H", 9);
    int32_t rows_used = 0;

    abAppend(ab, "\x1b[7m Settings \x1b[m\x1b[K\r\n\x1b[K\r\n", 27);
    rows_used += 2;

    int32_t visible = editorSettingsVisibleRows();
    int32_t count = editorSettingsVisibleCount(edited);
    int32_t last_visible = scroll + visible - 1;
    if (last_visible >= count) last_visible = count - 1;
    for (int32_t i = scroll; i < count && i < scroll + visible; i++) {
        char scroll_indicator = '\0';
        if (i == scroll && scroll > 0) scroll_indicator = '^';
        else if (i == last_visible && last_visible < count - 1) scroll_indicator = 'v';
        editorSettingsDrawRow(ab, editorSettingsDescriptorAt(edited, i), i == cursor, edited, scroll_indicator);
        rows_used++;
    }

    abAppend(ab, "\x1b[K\r\n", 5);
    rows_used++;
    if (edited->redo_key == REDO_KEY_CTRL_SHIFT_Z) {
        char note[160];
        editorShortcutText(note, sizeof(note),
            "  Note: Ctrl-Shift-Z may not reach the editor on every "
            "terminal; Ctrl-Y always works as a fallback.\x1b[K\r\n");
        abAppend(ab, note, (int32_t)strlen(note));
        rows_used++;
    }

    char help[128];
    if (msg && msg[0]) {
        editorShortcutText(help, sizeof(help), msg);
    } else {
        snprintf(help, sizeof(help),
            "  Up/Down select, Enter/Space/Left/Right edit, %s-D reset defaults, %s-S/F2 save, Esc cancel",
            editorPrimaryModifier(), editorPrimaryModifier());
    }
    int32_t hlen = (int32_t)strlen(help);
    abAppend(ab, help, hlen);
    abAppend(ab, "\x1b[K\r\n", 5);
    rows_used++;

    /* Clear every remaining screen row so stale buffer content from
     * the previous editorRefreshScreen() frame doesn't show through
     * underneath the panel. Total rows written (2 header + options +
     * blank/note/help + this padding) must equal the terminal height
     * exactly -- one \r\n too many scrolls the screen and desyncs
     * \x1b[H from the top of the visible viewport on every frame. */
    int32_t total_rows = E.view.screenrows + 2;
    for (; rows_used < total_rows - 1; rows_used++)
        abAppend(ab, "\x1b[K\r\n", 5);
    if (rows_used < total_rows)
        abAppend(ab, "\x1b[K", 3); /* last row: no trailing newline */

    abAppend(ab, "\x1b[H\x1b[?25h", 9);
}

/* Inline numeric input for a SETTING_INT field, redrawing the F2 panel
 * (via editorSettingsRender()) on every keystroke instead of handing
 * off to editorPrompt(), which only knows how to redraw the main
 * editor screen underneath. Returns 1 and writes *out on Enter with a
 * non-empty value, 0 on Esc (value unchanged). */
static uint8_t editorSettingsEditInt(struct editorSettings *edited, int32_t cursor,
    int32_t scroll, const struct settingDescriptor *d, int32_t *out) {
    char buf[16];
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s (%d-%d): %s", d->label, d->int_min, d->int_max, buf);

        struct abuf ab = ABUF_INIT;
        editorSettingsRender(&ab, edited, cursor, scroll, msg);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        int32_t c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            return 0;
        } else if (c == '\r') {
            if (buflen == 0) continue;
            int32_t v = atoi(buf);
            if (v < d->int_min) v = d->int_min;
            if (v > d->int_max) v = d->int_max;
            *out = v;
            return 1;
        } else if ((c == '-' || isdigit(c)) && buflen < sizeof(buf) - 1) {
            buf[buflen++] = (char)c;
            buf[buflen] = '\0';
        }
    }
}

/* Ctrl-S, F2 and Esc -> Yes must apply the same live updates before
 * saving. Changing only S leaves cached row rendering out of date. */
static void editorSettingsSave(const struct editorSettings *edited) {
    struct editorSettings previous = S;
    S = *edited;
    if (previous.color_background != COLOR_TERMINAL_DEFAULT &&
        S.color_background == COLOR_TERMINAL_DEFAULT)
        write(STDOUT_FILENO, "\x1b[49m", 5);
    if (S.show_top_bar != previous.show_top_bar)
        winsize_changed = 1;
    if (S.tab_stop != previous.tab_stop ||
        S.show_invisibles != previous.show_invisibles ||
        S.syntax_highlight != previous.syntax_highlight)
        editorUpdateAllRows();
    if (S.mouse_enabled != previous.mouse_enabled) {
        if (S.mouse_enabled) terminalEnableMouseReporting();
        else terminalDisableMouseReporting();
    }
    if (settingsSave(&S)) {
        editorSetStatusMessage("Settings saved to ~/.tinyeditrc");
    } else {
        editorSetStatusMessage("Could not write ~/.tinyeditrc");
    }
}

static void editorSettingsScreen(void) {
    struct editorSettings edited = S;
    int32_t cursor = 0;
    int32_t scroll = 0;
    char msg[80] = "";

    while (1) {
        /* Keep cursor inside the visible window, same idea as
         * editorScroll() for the main buffer -- clamped here (once
         * per frame) rather than inside the ARROW_UP/DOWN cases so it
         * also self-corrects if settingDescriptorCount ever changes
         * or the terminal is resized while the panel is open. */
        int32_t visible = editorSettingsVisibleRows();
        int32_t count = editorSettingsVisibleCount(&edited);
        if (cursor >= count) cursor = count - 1;
        if (cursor < scroll) scroll = cursor;
        if (cursor >= scroll + visible) scroll = cursor - visible + 1;

        struct abuf ab = ABUF_INIT;
        editorSettingsRender(&ab, &edited, cursor, scroll, msg);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        msg[0] = '\0';

        int32_t c = editorReadKey();
        const struct settingDescriptor *d = &settingDescriptors[editorSettingsDescriptorAt(&edited, cursor)];
        int32_t *slot = settingsScreenSlot(&edited, d);

        switch (c) {
            case ARROW_UP:
                cursor = (cursor > 0) ? cursor - 1 : count - 1;
                break;
            case ARROW_DOWN:
                cursor = (cursor + 1) % count;
                break;

            /* Left/Right cycle an enum value backward/forward -- only
             * meaningful for SETTING_ENUM (color pickers in
             * particular grew to 24 entries with the light/dark/dim
             * palette, and Enter/Space alone only steps forward, so
             * overshooting meant stepping through the entire list to
             * get back). Arrows are otherwise unused while the cursor
             * sits on a row (Up/Down already own row navigation), so
             * this doesn't take anything away from BOOL/INT rows --
             * it's simply a no-op there. */
            case ARROW_LEFT:
                if (d->type == SETTING_ENUM)
                    editorSettingsCycleEnum(d, slot, -1);
                break;
            case ARROW_RIGHT:
                if (d->type == SETTING_ENUM)
                    editorSettingsCycleEnum(d, slot, +1);
                break;

            case '\r':
            case ' ':
                if (d->type == SETTING_BOOL) {
                    *slot = !*slot;
                } else if (d->type == SETTING_ENUM) {
                    editorSettingsCycleEnum(d, slot, +1);
                } else { /* SETTING_INT: inline numeric input, panel stays on screen */
                    int32_t v;
                    if (editorSettingsEditInt(&edited, cursor, scroll, d, &v))
                        *slot = v;
                }
                break;

            case CTRL_KEY('s'):
            case F2_KEY:
                editorSettingsSave(&edited);
                return;

            case CTRL_KEY('d'):
                /* Resets only the local edited copy, same as any
                 * other in-panel edit -- Ctrl-S or F2 is still required to
                 * make it live/persist, Esc still discards it (and
                 * will now prompt, since edited != S). Doesn't touch
                 * filetype.* overrides: those aren't part of struct
                 * editorSettings / not edited here at all. */
                settingsDefaults(&edited);
                msg[0] = '\0';
                snprintf(msg, sizeof(msg), "Reset to defaults (not saved yet -- Ctrl-S/F2 to keep, Esc to discard)");
                break;

            case '\x1b': {
                if (memcmp(&edited, &S, sizeof(edited)) == 0) return; /* no changes: exit right away */

                struct abuf ab2 = ABUF_INIT;
                editorSettingsRender(&ab2, &edited, cursor, scroll, "Save changes before leaving? (y/n/Esc to cancel)");
                write(STDOUT_FILENO, ab2.b, (size_t)ab2.len);
                abFree(&ab2);

                int32_t confirm = editorReadKey();
                if (confirm == 'y' || confirm == 'Y') {
                    editorSettingsSave(&edited);
                    return;
                } else if (confirm == 'n' || confirm == 'N') {
                    return; /* discard edited, live settings (S) untouched */
                }
                /* Esc or anything else: stay in the panel, edits kept. */
                break;
            }

            default:
                break;
        }
    }
}

/* Auto-close pair table for characters the user can actually type
 * from a keyboard (all single-byte ASCII): asymmetric pairs have a
 * distinct open/close character; symmetric ones (quotes, "$" for
 * inline LaTeX math, "`" for inline code) use the same character for
 * both, matching how every mainstream editor treats quote/backtick
 * auto-closing. Triple-backtick Markdown code fences are
 * deliberately NOT special-cased the way "$$" is below -- VS Code
 * tried exactly that, users found it more disruptive than helpful
 * (auto-inserting a closing fence gets in the way when typing
 * multi-line code blocks), and it was walked back. Curly quotes
 * («» "" '') aren't in this table -- see autoCloseMultiByteTable
 * below for why they're handled separately. */
static const struct autoClosePair autoCloseTable[] = {
    { '(', ')' }, { '{', '}' }, { '[', ']' },
    { '"', '"' }, { '\'', '\'' }, { '$', '$' }, { '`', '`' },
};
static const int32_t autoCloseTableCount =
    (int32_t)(sizeof(autoCloseTable) / sizeof(autoCloseTable[0]));

static const struct autoClosePair *editorAutoCloseFor(int32_t c) {
    /* Single quote has its own opt-in (default off, see
     * auto_close_single_quote in settings.h) on top of the general
     * auto_close_pairs switch -- skip it here so callers see a NULL
     * pair and fall back to plain insertion/skip-over-nothing, exactly
     * as if it weren't in autoCloseTable at all. */
    if (c == '\'' && !S.auto_close_single_quote) return NULL;
    for (int32_t i = 0; i < autoCloseTableCount; i++)
        if (autoCloseTable[i].open == c) return &autoCloseTable[i];
    return NULL;
}

/* Curly-quote pairs: full auto-close (open inserts its match, wraps
 * the selection) same as the ASCII pairs, PLUS skip-over on the close
 * character -- even though none of these can be typed from a
 * physical keyboard directly (not on any standard layout, only
 * reachable via OS-level compose sequences or paste), once the OPEN
 * character has been composed/pasted, treating it exactly like a
 * regular open-bracket keypress from that point on is both correct
 * and simplest: there's no reason to special-case "how the character
 * arrived" once editorReadMultiByteKey() has assembled it. Each
 * open/close is the raw UTF-8 bytes (not a codepoint) since that's
 * what's compared against/written into row->chars. */
static const struct autoCloseMultiByte autoCloseMultiByteTable[] = {
    { "\xc2\xab", 2, "\xc2\xbb", 2 },             /* « » */
    { "\xe2\x80\x9c", 3, "\xe2\x80\x9d", 3 },     /* “ ” */
    { "\xe2\x80\x98", 3, "\xe2\x80\x99", 3 },     /* ‘ ’ */
};
static const int32_t autoCloseMultiByteCount =
    (int32_t)(sizeof(autoCloseMultiByteTable) / sizeof(autoCloseMultiByteTable[0]));

/* Reads the remaining bytes of a UTF-8 sequence whose lead byte
 * (`lead`, already consumed from the input) was passed in, via
 * editorReadKey() -- correct because editorReadKey() returns
 * continuation bytes (0x80-0xBF) verbatim, the same as any other
 * non-ASCII, non-ESC byte (see its switch: only '\x1b' triggers
 * special handling). `out` receives the full sequence (lead byte
 * included), up to 4 bytes; returns the sequence length. This is the
 * ONLY place that needs to know the difference between "one byte" and
 * "one character" -- everywhere else in the codebase
 * deliberately treats input as a raw byte stream and lets bytes land
 * in row->chars in order, which is simpler and correct for insertion
 * but can't tell whole characters apart for the comparison this
 * function exists to make possible. */
static int32_t editorReadMultiByteKey(uint8_t lead, char *out) {
    int32_t expected_len = utf8ByteLen(lead);
    if (expected_len < 1) expected_len = 1;
    if (expected_len > 4) expected_len = 4;
    out[0] = (char)lead;
    int32_t actual_len = 1;
    for (int32_t i = 1; i < expected_len; i++) {
        int32_t next = editorReadKey();
        /* A malformed/interrupted sequence (e.g. terminal disconnect
         * mid-byte) stops early rather than blocking on further
         * continuation bytes that may never come -- editorReadKey()
         * itself doesn't distinguish this from EOF, so treat any
         * value outside the continuation-byte range (0x80-0xBF) as
         * "sequence ended early" and just stop collecting. */
        if (next < 0x80 || next > 0xBF) {
            pending_key = next;
            break;
        }
        out[actual_len++] = (char)next;
    }
    return actual_len;
}

/* If the bytes right after the cursor equal one of
 * autoCloseMultiByteTable's close sequences AND that's exactly what
 * was just typed (`typed`/`typed_len`), moves the cursor past it and
 * returns 1 without touching the buffer. Returns 0 otherwise, leaving
 * the caller to insert `typed` normally -- this is what makes it safe
 * to call unconditionally instead of matching on buffer content alone
 * (which would skip over existing text regardless of what key was
 * actually pressed). */
static uint8_t editorTrySkipMultiByteClose(const char *typed, int32_t typed_len) {
    uint8_t is_known_close = 0;
    for (int32_t i = 0; i < autoCloseMultiByteCount; i++) {
        if (autoCloseMultiByteTable[i].close_len == typed_len &&
            memcmp(autoCloseMultiByteTable[i].close, typed, (size_t)typed_len) == 0) {
            is_known_close = 1;
            break;
        }
    }
    if (!is_known_close) return 0;

    if (E.document.cursor.cy >= E.document.buffer.row_count) return 0;
    erow *row = &E.document.buffer.rows[E.document.cursor.cy];
    if (row->size - E.document.cursor.cx < typed_len) return 0;
    if (memcmp(&row->chars[E.document.cursor.cx], typed, (size_t)typed_len) != 0) return 0;

    E.document.cursor.cx += typed_len;
    return 1;
}

/* Replaces editorInsertChar(c) for characters typed through the
 * default: case of editorProcessKeypress() -- handles auto-close
 * (open bracket/quote inserts its match right after the cursor, or
 * wraps the active selection), skip-over (typing a close character
 * that's already sitting right after the cursor moves past it instead
 * of duplicating it), and the same two for multi-byte curly quotes
 * («» "" '') -- reads any remaining bytes of a non-ASCII character up
 * front via editorReadMultiByteKey() before deciding, so the
 * comparison/insertion is against the whole character the user
 * actually typed (or composed/pasted), not a lone byte of it. Falls
 * back to plain insertion when S.auto_close_pairs is off or none of
 * the above applies. had_sel and the sel_* range:
 * the selection as it was BEFORE this keypress cleared E.document.selection.active (see
 * editorProcessKeypress()), needed for the wrap case since by the
 * time this runs the selection is already gone. */
static void editorInsertCharAutoClose(int32_t c, uint8_t had_sel,
    int32_t sel_y0, int32_t sel_x0, int32_t sel_y1, int32_t sel_x1) {
    if (!S.auto_close_pairs) {
        editorInsertChar(c);
        return;
    }

    if (c >= 0x80 && c <= 0xff) {
        /* Non-ASCII: assemble the whole character before deciding --
         * a lone byte can never be compared meaningfully against a
         * multi-byte open/close sequence. */
        char seq[4];
        int32_t seq_len = editorReadMultiByteKey((uint8_t)c, seq);
        if (editorTrySkipMultiByteClose(seq, seq_len)) return;

        for (int32_t i = 0; i < autoCloseMultiByteCount; i++) {
            const struct autoCloseMultiByte *mb = &autoCloseMultiByteTable[i];
            if (mb->open_len != seq_len || memcmp(mb->open, seq, (size_t)seq_len) != 0) continue;

            if (had_sel) {
                /* Wrap the selection, same ordering rationale as the
                 * ASCII case: close end-first so it doesn't shift the
                 * still-unused start coordinates on a same-row
                 * selection. */
                E.document.cursor.cy = sel_y1; E.document.cursor.cx = sel_x1;
                for (int32_t k = 0; k < mb->close_len; k++) editorInsertChar((unsigned char)mb->close[k]);
                E.document.cursor.cy = sel_y0; E.document.cursor.cx = sel_x0;
                for (int32_t k = 0; k < mb->open_len; k++) editorInsertChar((unsigned char)mb->open[k]);
                E.document.cursor.cy = sel_y1;
                E.document.cursor.cx = sel_x1 + (sel_y1 == sel_y0 ? mb->open_len + mb->close_len : mb->open_len);
                return;
            }

            for (int32_t k = 0; k < seq_len; k++) editorInsertChar((unsigned char)seq[k]);
            for (int32_t k = 0; k < mb->close_len; k++) editorInsertChar((unsigned char)mb->close[k]);
            E.document.cursor.cx -= mb->close_len;
            return;
        }

        for (int32_t i = 0; i < seq_len; i++) editorInsertChar((unsigned char)seq[i]);
        return;
    }

    const struct autoClosePair *pair = editorAutoCloseFor(c);

    if (pair && had_sel) {
        /* Wrap the selection: close at the end first so inserting it
         * doesn't shift the still-to-be-used start coordinates when
         * start and end are on the same row. */
        E.document.cursor.cy = sel_y1; E.document.cursor.cx = sel_x1;
        editorInsertChar((unsigned char)pair->close);
        E.document.cursor.cy = sel_y0; E.document.cursor.cx = sel_x0;
        editorInsertChar((unsigned char)pair->open);
        E.document.cursor.cy = sel_y1; E.document.cursor.cx = sel_x1 + (sel_y1 == sel_y0 ? 2 : 1);
        return;
    }

    if (pair && pair->open == pair->close) {
        /* $$ (LaTeX display math) special case, checked before the
         * general symmetric-pair skip-over below. Typing "$" four
         * times in a row goes through these states:
         *   1st "$": auto-close opens a pair  -> "$|$"      (cx=1)
         *   2nd "$": skip-over (chars[cx]=='$') -> "$$|"    (cx=2)
         *   3rd "$": nothing to skip (cx==row->size) -- THIS is
         *            where the old code fell through to "open a new
         *            pair", giving "$$$|$" instead of the intended
         *            "$$|$$". Recognized here by the TWO characters
         *            immediately left of the cursor both being "$"
         *            (chars[cx-2] and chars[cx-1]) with nothing to
         *            skip to the right -- that combination can only
         *            happen right after the 1st+2nd "$" of this exact
         *            sequence, not from unrelated separate "$...$"
         *            pairs elsewhere in the line (those never leave
         *            two bare "$" adjacent with the cursor past both).
         *            Turns "$$|" into "$$|$$".
         *   4th "$": now chars[cx]=='$' again (the "$" just inserted
         *            above) -- ordinary skip-over handles it, but
         *            only skips ONE level: cursor ends up "$$$|$",
         *            still nested one "$" deep, not fully past both
         *            pairs. Accepted tradeoff (see TODO.md): a true
         *            double-skip here would need to detect "both
         *            remaining close characters are adjacent with no
         *            content typed between them", which starts
         *            stacking edge cases on an already-narrow special
         *            case for diminishing benefit. One extra Right
         *            arrow exits the last level cleanly -- far better
         *            than the original bug (duplicated/misplaced "$"
         *            characters), just not fully seamless.
         * Deliberately narrow to "$" only (not generalized to
         * quotes): "$$" is a real, meaningful LaTeX construct; "\"\""
         * or "''" doubled have no equivalent convention worth
         * special-casing. */
        if (c == '$' && E.document.cursor.cx >= 2 && E.document.cursor.cy < E.document.buffer.row_count) {
            erow *row = &E.document.buffer.rows[E.document.cursor.cy];
            uint8_t nothing_to_skip = E.document.cursor.cx >= row->size || row->chars[E.document.cursor.cx] != '$';
            if (nothing_to_skip && row->chars[E.document.cursor.cx - 1] == '$' && row->chars[E.document.cursor.cx - 2] == '$') {
                editorInsertChar('$');
                editorInsertChar('$');
                E.document.cursor.cx -= 2;
                return;
            }
        }

        /* Symmetric (quotes, $): typing it while sitting right before
         * an identical character skips over instead of inserting a
         * second one -- covers both "just closed this pair" and
         * "typed the close of a pair someone else opened", since the
         * byte itself can't distinguish the two. Backtick is excluded:
         * unlike quotes/$, a lone "`" is also valid Markdown inline-code
         * syntax typed repeatedly on its own (not just as this pair's
         * close), and skip-over there does more harm than good -- so it
         * always inserts a fresh pair instead. */
        if (c != '`' && E.document.cursor.cy < E.document.buffer.row_count) {
            erow *row = &E.document.buffer.rows[E.document.cursor.cy];
            if (E.document.cursor.cx < row->size && row->chars[E.document.cursor.cx] == pair->close) {
                E.document.cursor.cx++;
                return;
            }
        }
        editorInsertChar(c);
        editorInsertChar((unsigned char)pair->close);
        E.document.cursor.cx--;
        return;
    }

    if (pair) {
        /* Asymmetric open (open != close): always inserts both and
         * places the cursor in between -- typing the OPEN character
         * never skips, only typing the matching CLOSE character
         * (handled by the branch below) does. */
        editorInsertChar(c);
        editorInsertChar((unsigned char)pair->close);
        E.document.cursor.cx--;
        return;
    }

    /* Not an opener -- check whether it's the closer of an asymmetric
     * pair, for skip-over (e.g. typing ')' right before an
     * auto-inserted ')'). */
    for (int32_t i = 0; i < autoCloseTableCount; i++) {
        if (autoCloseTable[i].close == c && autoCloseTable[i].open != autoCloseTable[i].close) {
            if (E.document.cursor.cy < E.document.buffer.row_count) {
                erow *row = &E.document.buffer.rows[E.document.cursor.cy];
                if (E.document.cursor.cx < row->size && row->chars[E.document.cursor.cx] == c) {
                    E.document.cursor.cx++;
                    return;
                }
            }
            break;
        }
    }

    editorInsertChar(c);
}

static void editorProcessKeypress(void) {
    int32_t c = editorReadKey();

    /* Immutable snapshot for actions that consume or wrap the selection.
     * Each switch branch owns its selection transition; there is no global
     * pre-dispatch whitelist for new keys to accidentally bypass. */
    int32_t had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1;
    uint8_t had_sel = editorSelectionRange(&E.document.selection, &E.document.cursor, &had_sel_y0, &had_sel_x0, &had_sel_y1, &had_sel_x1);

    switch (c) {
        case '\r':
            E.document.selection.active = 0;
            editorInsertNewlineAutoIndent();
            break;

        case '\t':
            if (had_sel) {
                editorIndentSelection(0);
            } else {
                E.document.selection.active = 0;
                if (S.insert_spaces_for_tab) {
                    for (int32_t i = 0; i < S.tab_stop; i++) editorInsertChar(' ');
                } else {
                    editorInsertChar('\t');
                }
            }
            break;

        /* With no selection this outdents the current line, which is
         * what Shift+Tab does everywhere else -- the cursor doesn't
         * have to be in the indentation for "this line is indented one
         * level too far" to be the obvious intent. */
        case SHIFT_TAB:
            if (had_sel) {
                editorIndentSelection(1);
            } else if (E.document.cursor.cy < E.document.buffer.row_count) {
                E.document.selection.active = 0;
                editorPushUndo(EDIT_OTHER);
                int32_t removed = editorRowOutdent(&E.document.buffer.rows[E.document.cursor.cy]);
                E.document.cursor.cx -= removed;
                if (E.document.cursor.cx < 0) E.document.cursor.cx = 0;
                if (removed) E.document.file.dirty = 1;
            }
            break;

        case CTRL_KEY('q'):
            E.document.selection.active = 0;
            editorQuit();
            return;

        case CTRL_KEY('w'):
            E.document.selection.active = 0;
            editorCloseFile();
            break;

        case CTRL_KEY('o'):
            E.document.selection.active = 0;
            editorOpenFile();
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case F4_KEY:
        case SAVE_AS_KEY:
            E.document.selection.active = 0;
            editorSaveAs();
            break;

        case CTRL_KEY('a'):
            if (E.document.buffer.row_count > 0) {
                E.document.selection.active = 1;
                E.document.selection.anchor_y = 0;
                E.document.selection.anchor_x = 0;
                E.document.cursor.cy = E.document.buffer.row_count - 1;
                E.document.cursor.cx = E.document.buffer.rows[E.document.buffer.row_count - 1].size;
            }
            break;

        case CTRL_KEY('c'):
        case CTRL_KEY('x'): {
            int32_t sy, sx, ey, ex;
            if (editorSelectionRange(&E.document.selection, &E.document.cursor, &sy, &sx, &ey, &ex)) {
                size_t len;
                char *text = editorSerializeRange(sy, sx, ey, ex, &len);
                clipboardCopy(text, len);
                if (c == CTRL_KEY('x')) {
                    editorDeleteRange(sy, sx, ey, ex);
                    editorSetStatusMessage("%zu bytes cut", len);
                    E.document.selection.active = 0;
                } else {
                    editorSetStatusMessage("%zu bytes copied", len);
                }
                free(text);
            }
            break;
        }

        case CTRL_KEY('v'): {
            size_t len;
            char *text = clipboardPaste(&len);
            if (text) {
                editorReplaceSelectionWithText(had_sel,
                    had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1, text, len);
                clipboardFree(text);
                editorSetStatusMessage("pasted");
            }
            break;
        }

        /* Bracketed paste (terminal-native paste, e.g. Cmd+V into the
         * terminal window rather than through this editor's own
         * Ctrl-V/system-clipboard path above): editorReadKey() reports
         * PASTE_START_KEY the instant it sees the ESC[200~ marker, then
         * this reads the entire pasted block in one go and sends it through
         * editorReplaceSelectionWithText() -- the same bulk path Ctrl-V uses,
         * so a paste that
         * arrives this way is both fast (one undo-snapshot/no
         * per-character redraw, vs. an editorProcessKeypress() call per
         * byte the old byte-by-byte path required) and correct
         * (bypasses editorInsertCharAutoClose() entirely, so pasted
         * '(', '\'', '`', etc. don't each trigger auto-close as if
         * freshly typed -- see TODO.md for the bug this fixes: spurious
         * closing characters left behind after a paste). */
        case PASTE_START_KEY: {
            size_t len;
            char *text = terminalReadPastedText(&len);
            editorReplaceSelectionWithText(had_sel,
                had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1, text, len);
            free(text);
            editorSetStatusMessage("pasted");
            break;
        }

        /* Mouse: click positions the cursor, drag (motion while the
         * button is held) extends a selection from the click point,
         * release just stops extending, wheel scrolls without moving
         * the cursor. SGR button codes (see mouseEventButton, xterm
         * ctlseqs): 0 = left button, 32 = left button + motion flag
         * (a drag report, not a fresh press), 64/65 = wheel up/down.
         * Clicks in the gutter or the status/message bars are ignored
         * (editorMouseToCursor() assumes a text-area click; the row
         * bounds check below is what actually filters those out,
         * since gutter clicks still report a row inside the text
         * area's row range -- just filtering out the two bottom rows,
         * which this screen_row/S.show_top_bar math already keeps
         * outside the video-row space editorMouseToCursor() maps). */
        case MOUSE_EVENT_KEY: {
            static uint8_t dragging = 0;
            static int32_t press_anchor_x = 0, press_anchor_y = 0;

            /* Coalescing loop: apply this event, then check whether
             * another one is already queued (see terminalInputReady())
             * and if so read+apply it too, WITHOUT returning to the
             * main loop's editorRefreshScreen() in between. A single
             * wheel gesture or a fast drag generates many SGR reports
             * back-to-back; redrawing after every one makes the
             * display visibly lag behind the gesture by the time it
             * catches up (each redraw takes long enough that several
             * more events queue up while it runs). Only the FINAL
             * state after the whole burst needs to be drawn. */
            uint8_t more = 1;
            while (more) {
                if (mouseEventButton == 64 || mouseEventButton == 65) {
                    /* Wheel: scroll the VIEW only. Never touches
                     * E.document.cursor.cy/E.document.cursor.cx or the selection -- the cursor and
                     * whatever text is selected are conceptually
                     * independent of what's currently visible on
                     * screen (explicit user expectation), so the wheel
                     * must not move either, no matter how far the view
                     * scrolls away from them. Same granularity as a
                     * few Arrow-Up/Down presses -- deliberately not a
                     * full PageUp/PageDown, which would be too coarse
                     * for incremental wheel ticks. */
                    int32_t wrapcols = editorSoftWrapCols();
                    int32_t delta = (mouseEventButton == 64) ? -3 : 3;
                    int32_t limit = wrapcols > 0 ? editorTotalVideoRows(wrapcols) : E.document.buffer.row_count;
                    E.view.rowoff += delta;
                    if (E.view.rowoff < 0) E.view.rowoff = 0;
                    if (E.view.rowoff > limit) E.view.rowoff = limit;
                    /* editorScroll() (called every redraw) normally
                     * forces E.view.rowoff back into the window
                     * [cursor_vy - screenrows + 1, cursor_vy] so the
                     * cursor stays on screen -- correct for actual
                     * cursor movement, but since the wheel never moves
                     * the cursor, that same logic would snap the view
                     * right back to hug the (stationary) cursor on the
                     * very next redraw, capping wheel scroll to roughly
                     * one screenful before/after it (the bug originally
                     * reported: "scrolla solo 2 pagine"). This flag
                     * tells editorScroll() to skip that re-centering
                     * just once; it's cleared automatically the moment
                     * the cursor moves for a real reason (see
                     * E.view.free_scroll's declaration in tinyedit.h), so
                     * scrolling with the wheel and then, say, pressing
                     * an arrow key immediately goes back to normal
                     * "view follows cursor" behavior. */
                    E.view.free_scroll = 1;
                } else {
                    uint8_t in_text_area = mouseEventRow >= 1 + (S.show_top_bar ? 1 : 0) &&
                        mouseEventRow <= 1 + (S.show_top_bar ? 1 : 0) + E.view.screenrows - 1 &&
                        mouseEventCol > editorGutterWidth();

                    if (in_text_area && mouseEventButton == 0 && mouseEventPress) {
                        /* Fresh press: position the cursor there.
                         * E.document.selection.active is deliberately left OFF here
                         * (not set to 1 with anchor==cursor) -- the
                         * anchor is only remembered locally
                         * (press_anchor_y/x below) and E.document.selection.active is
                         * turned on only once an actual drag moves the
                         * cursor away from it (see the drag branch).
                         *
                         * A collapsed anchor==cursor selection LOOKS
                         * invisible right after the click (start==end,
                         * nothing to highlight), but editorSelectionRange()
                         * re-evaluates the anchor against the CURRENT
                         * cursor position on every call, not a
                         * snapshot -- so if E.document.selection.active stayed 1 here
                         * and E.document.cursor.cy/E.document.cursor.cx moved for any OTHER reason
                         * before the next click or Esc (e.g. the wheel
                         * dragging the cursor along to stay on-screen,
                         * see the wheel branch above), a real selection
                         * would suddenly appear out of a plain click
                         * that never dragged -- this was the bug
                         * reported by the user ("ho fatto solo click...
                         * poi scrollando si è magicamente selezionato
                         * il testo"). Not arming sel_active until a
                         * real drag happens closes this off entirely. */
                        int32_t cy, cx;
                        editorMouseToCursor(mouseEventCol, mouseEventRow, &cy, &cx);
                        E.document.cursor.cy = cy;
                        E.document.cursor.cx = cx;
                        E.document.selection.active = 0;
                        press_anchor_x = cx;
                        press_anchor_y = cy;
                        dragging = 1;
                    } else if (in_text_area && mouseEventButton == 32 && dragging) {
                        /* Drag: the cursor has now moved away from the
                         * press point -- arm the selection (if not
                         * already active) with the REMEMBERED press
                         * point as anchor, then move the cursor (and
                         * hence the live selection endpoint) to follow
                         * the mouse. Once armed, the anchor is
                         * E.document.selection.anchor_x/y like any other selection
                         * (Shift+Arrow, Ctrl-A, ...) -- press_anchor_*
                         * only matters for this initial arming. */
                        if (!E.document.selection.active) {
                            E.document.selection.active = 1;
                            E.document.selection.anchor_x = press_anchor_x;
                            E.document.selection.anchor_y = press_anchor_y;
                        }
                        int32_t cy, cx;
                        editorMouseToCursor(mouseEventCol, mouseEventRow, &cy, &cx);
                        E.document.cursor.cy = cy;
                        E.document.cursor.cx = cx;
                    } else if (!mouseEventPress) {
                        /* Release: stop tracking drag motion. A click
                         * with no drag in between leaves
                         * sel_anchor_x/y == E.document.cursor.cx/E.document.cursor.cy, which
                         * editorSelectionRange() already treats as "no
                         * selection" -- no special-casing needed here
                         * for "was this a click or a drag". */
                        dragging = 0;
                    }
                }

                more = 0;
                if (terminalInputReady()) {
                    int32_t next = editorReadKey();
                    if (next == MOUSE_EVENT_KEY) more = 1;
                    /* A non-mouse key arrived instead (e.g. the user
                     * started typing right after scrolling) -- it's
                     * already been consumed by editorReadKey() above,
                     * but this switch has no path left to dispatch it
                     * through, so it's dropped. Rare in practice (would
                     * require keystrokes interleaved within the same
                     * burst of already-buffered input), and dropping
                     * one keystroke is a far smaller issue than the
                     * lag this coalescing exists to fix -- not worth
                     * the complexity of a pushback/replay mechanism
                     * for it. */
                }
            }
            break;
        }

        case CTRL_KEY('z'):
            E.document.selection.active = 0;
            editorUndo();
            break;
        case CTRL_KEY('y'):
            /* Ctrl-Y always works as redo regardless of the configured
             * redo_key: Ctrl-Shift-Z is frequently indistinguishable
             * from plain Ctrl-Z on a raw tty (see TODO.md), so Ctrl-Y
             * remains a reliable fallback even when the user picked
             * ctrl-shift-z in settings. */
            E.document.selection.active = 0;
            editorRedo();
            break;

        case CTRL_KEY('f'):
            E.document.selection.active = 0;
            editorFind();
            break;

        case F1_KEY:
            E.document.selection.active = 0;
            editorHelpScreen();
            break;

        case F3_KEY:
            E.document.selection.active = 0;
            editorInfoScreen();
            break;

        case F2_KEY:
            E.document.selection.active = 0;
            editorSettingsScreen();
            break;

        case CTRL_KEY('t'):
            /* Universal selection toggle: works on every terminal, even
             * ones (e.g. Terminal.app on macOS) that can't report
             * Shift+Arrow as a distinct sequence from a plain arrow. */
            E.document.selection.pinned = !E.document.selection.pinned;
            if (E.document.selection.pinned) {
                E.document.selection.active = 1;
                E.document.selection.anchor_x = E.document.cursor.cx;
                E.document.selection.anchor_y = E.document.cursor.cy;
                editorSetStatusMessage("Selection mode ON (arrows extend, %s-T to stop)",
                    editorPrimaryModifier());
            } else {
                E.document.selection.active = 0;
                editorSetStatusMessage("Selection mode off");
            }
            break;

        case DOC_HOME:
        case DOC_END:
            if (!E.document.selection.pinned) E.document.selection.active = 0;
            /* fall through, same as Home/End below */
        case SHIFT_DOC_HOME:
        case SHIFT_DOC_END: {
            uint8_t to_start = (c == DOC_HOME || c == SHIFT_DOC_HOME);
            uint8_t extending = (c == SHIFT_DOC_HOME || c == SHIFT_DOC_END || E.document.selection.pinned);

            if (extending && !E.document.selection.active) {
                E.document.selection.active = 1;
                E.document.selection.anchor_x = E.document.cursor.cx;
                E.document.selection.anchor_y = E.document.cursor.cy;
            }

            if (to_start) {
                E.document.cursor.cy = 0;
                E.document.cursor.cx = 0;
            } else if (E.document.buffer.row_count > 0) {
                E.document.cursor.cy = E.document.buffer.row_count - 1;
                E.document.cursor.cx = E.document.buffer.rows[E.document.buffer.row_count - 1].size;
            }

            if (extending && E.document.selection.anchor_x == E.document.cursor.cx && E.document.selection.anchor_y == E.document.cursor.cy)
                E.document.selection.active = 0;
            break;
        }

        case HOME_KEY:
        case END_KEY:
            if (!E.document.selection.pinned) E.document.selection.active = 0;
            /* fall through: like the arrows below, a plain Home/End
             * extends the selection while sel_pinned (Ctrl-T) is set. */
        case SHIFT_HOME:
        case SHIFT_END: {
            uint8_t to_home = (c == HOME_KEY || c == SHIFT_HOME);
            uint8_t extending = (c == SHIFT_HOME || c == SHIFT_END || E.document.selection.pinned);

            if (extending && !E.document.selection.active) {
                E.document.selection.active = 1;
                E.document.selection.anchor_x = E.document.cursor.cx;
                E.document.selection.anchor_y = E.document.cursor.cy;
            }

            int32_t hw_wrapcols = editorSoftWrapCols();
            if (hw_wrapcols > 0 && S.home_end_visual_line && E.document.cursor.cy < E.document.buffer.row_count) {
                int32_t seg_idx, seg_col;
                editorRxToSegment(&E.document.buffer.rows[E.document.cursor.cy], hw_wrapcols, E.document.cursor.rx, &seg_idx, &seg_col);
                erow *row = &E.document.buffer.rows[E.document.cursor.cy];
                int32_t nseg = editorRowSegments(row, hw_wrapcols);
                if (to_home) {
                    E.document.cursor.cx = editorSegColToCx(row, row->seg_start_rx[seg_idx], 0);
                } else {
                    int32_t seg_to_rx = editorSegVisibleEndRx(row, nseg, row->seg_start, row->seg_start_rx, seg_idx);
                    E.document.cursor.cx = editorSegColToCx(row, 0, seg_to_rx);
                }
            } else if (to_home) {
                E.document.cursor.cx = 0;
            } else if (E.document.cursor.cy < E.document.buffer.row_count) {
                E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy].size;
            }

            /* Collapsing back onto the anchor means nothing is selected
             * any more -- same rule the arrow keys use. */
            if (extending && E.document.selection.anchor_x == E.document.cursor.cx && E.document.selection.anchor_y == E.document.cursor.cy)
                E.document.selection.active = 0;
            break;
        }

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            /* With a selection, both keys delete the whole range rather
             * than one character, matching Ctrl-V/paste (which already
             * replaced the selection) and every other editor. had_sel is
             * the immutable copy captured before dispatch, so the handler
             * doesn't depend on live selection state while mutating rows. */
            if (had_sel) {
                editorDeleteRange(had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1);
            } else {
                if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
                editorDelChar();
            }
            E.document.selection.active = 0;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        case SHIFT_PAGE_UP:
        case SHIFT_PAGE_DOWN: {
            uint8_t is_up = (c == PAGE_UP || c == SHIFT_PAGE_UP);
            uint8_t extending = (c == SHIFT_PAGE_UP || c == SHIFT_PAGE_DOWN || E.document.selection.pinned);

            if (!extending) E.document.selection.active = 0;

            if (extending && !E.document.selection.active) {
                E.document.selection.active = 1;
                E.document.selection.anchor_x = E.document.cursor.cx;
                E.document.selection.anchor_y = E.document.cursor.cy;
            }

            int32_t pu_wrapcols = editorSoftWrapCols();
            if (pu_wrapcols > 0) {
                /* E.view.rowoff is a video-row index in wrapped mode (see
                 * editorScroll()), not a file-row index -- resolve it
                 * back to a (filerow, segment) before jumping there. */
                int32_t top_filerow, top_seg;
                int32_t target_vy = is_up ? E.view.rowoff : E.view.rowoff + E.view.screenrows - 1;
                int32_t total = editorTotalVideoRows(pu_wrapcols);
                if (target_vy >= total) target_vy = total > 0 ? total - 1 : 0;
                editorFileRowAtVideoRow(target_vy, pu_wrapcols, &top_filerow, &top_seg);
                E.document.cursor.cy = top_filerow;
                erow *row = &E.document.buffer.rows[E.document.cursor.cy];
                int32_t nseg = editorRowSegments(row, pu_wrapcols);
                if (top_seg >= nseg) top_seg = nseg - 1;
                E.document.cursor.cx = editorSegColToCx(row, row->seg_start_rx[top_seg], 0);
            } else if (is_up) {
                E.document.cursor.cy = E.view.rowoff;
            } else {
                E.document.cursor.cy = E.view.rowoff + E.view.screenrows - 1;
                if (E.document.cursor.cy > E.document.buffer.row_count) E.document.cursor.cy = E.document.buffer.row_count;
            }
            int32_t times = E.view.screenrows;
            while (times--)
                editorMoveCursor(is_up ? ARROW_UP : ARROW_DOWN);

            if (extending && E.document.selection.anchor_x == E.document.cursor.cx && E.document.selection.anchor_y == E.document.cursor.cy)
                E.document.selection.active = 0;
            break;
        }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            if (!E.document.selection.pinned) E.document.selection.active = 0;
            /* fall through: when sel_pinned is set, a plain arrow
             * extends the selection exactly like Shift+Arrow does. */
        case SHIFT_ARROW_UP:
        case SHIFT_ARROW_DOWN:
        case SHIFT_ARROW_LEFT:
        case SHIFT_ARROW_RIGHT: {
            uint8_t extending = (c == SHIFT_ARROW_UP || c == SHIFT_ARROW_DOWN ||
                c == SHIFT_ARROW_LEFT || c == SHIFT_ARROW_RIGHT || E.document.selection.pinned);

            if (extending && !E.document.selection.active) {
                E.document.selection.active = 1;
                E.document.selection.anchor_x = E.document.cursor.cx;
                E.document.selection.anchor_y = E.document.cursor.cy;
            }

            int32_t plain = (c == SHIFT_ARROW_UP || c == ARROW_UP) ? ARROW_UP :
                        (c == SHIFT_ARROW_DOWN || c == ARROW_DOWN) ? ARROW_DOWN :
                        (c == SHIFT_ARROW_LEFT || c == ARROW_LEFT) ? ARROW_LEFT : ARROW_RIGHT;
            editorMoveCursor(plain);

            if (extending && E.document.selection.anchor_x == E.document.cursor.cx && E.document.selection.anchor_y == E.document.cursor.cy)
                E.document.selection.active = 0;
            break;
        }

        case ALT_ARROW_LEFT:
            E.document.selection.active = 0;
            editorMoveCursorWord(0);
            break;
        case ALT_ARROW_RIGHT:
            E.document.selection.active = 0;
            editorMoveCursorWord(1);
            break;

        case CTRL_KEY('l'):
            /* Redraw/resize is not an editing action and preserves selection. */
            break;

        case '\x1b':
            /* Esc also turns off pinned selection mode (Ctrl-T), not
             * just the current selection -- otherwise the next arrow
             * press would silently start a new selection again, since
             * sel_pinned would still be set. */
            E.document.selection.pinned = 0;
            E.document.selection.active = 0;
            break;

        default:
            editorInsertCharAutoClose(c, had_sel, had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1);
            E.document.selection.active = 0;
            break;
    }
}

/* ---- init ------------------------------------------------------------------- */

static void editorFreeUndoRedo(void) {
    historyClear(&E.document.history);
}

static void initEditor(void) {
    E.document.cursor.cx = 0;
    E.document.cursor.cy = 0;
    E.document.cursor.rx = 0;
    E.view.rowoff = 0;
    E.view.coloff = 0;
    E.view.free_scroll = 0;
    E.document.file.detected_line_ending = LINE_ENDING_LF;
    E.document.file.line_endings_mixed = 0;
    E.document.buffer.row_count = 0;
    E.document.buffer.rows = NULL;
    E.document.file.dirty = 0;
    E.document.file.filename = NULL;
    E.ui.statusmsg[0] = '\0';
    E.ui.statusmsg_time = 0;
    E.ui.statusmsg_sticky = 0;
    E.document.selection.active = 0;
    E.document.selection.anchor_x = 0;
    E.document.selection.anchor_y = 0;
    E.document.selection.pinned = 0;
    E.search.search_match_y = -1;
    E.search.search_match_x = 0;
    E.search.search_match_len = 0;
    E.document.file.last_backup_time = 0;

    settingsLoad(&S);

    E.document.history.undo_stack = NULL;
    E.document.history.undo_count = 0;
    E.document.history.redo_stack = NULL;
    E.document.history.redo_count = 0;
    E.document.history.last_edit_type = EDIT_NONE;
    E.document.history.last_edit_time = 0;

    if (terminalGetWindowSize(&E.view.screenrows, &E.view.screencols) == -1)
        terminalDie("getWindowSize");
    E.view.screenrows -= 2; /* status bar + message bar */
    if (S.show_top_bar) E.view.screenrows -= 1;
}

int main(int argc, char **argv) {
    terminalEnableRawMode();
    terminalEnableBracketedPaste();
    terminalEnableResizeHandling();
    initEditor();
    editorChooseSlogan();
    atexit(terminalRestoreVisualState);
    if (S.mouse_enabled) terminalEnableMouseReporting();
    atexit(editorFreeUndoRedo);
    if (argc >= 2) editorOpen(argv[1]);

    /* Terminal.app on macOS sends the same byte sequence for a plain
     * arrow and Shift+Arrow, so text selection via Shift+Arrow silently
     * does nothing there -- not a bug, a limitation of that terminal
     * limitation. Point users at the universal Ctrl-T fallback
     * instead of leaving them to wonder why Shift+Arrow is unresponsive. */
    const char *term_program = getenv("TERM_PROGRAM");
    if (term_program && strcmp(term_program, "Apple_Terminal") == 0) {
        editorSetStatusMessageSticky(
            "Terminal.app: %s-T select | %s-O open | %s-Q quit | F1 help",
            editorPrimaryModifier(), editorPrimaryModifier(), editorPrimaryModifier());
    } else {
        editorSetStatusMessageSticky("%s-S save | %s-O open | %s-Q quit | F1 help",
            editorPrimaryModifier(), editorPrimaryModifier(), editorPrimaryModifier());
    }

    editorWarnMissingHighlightConfig();

    /* After the startup hint so a successful recovery's own sticky
     * message (see editorOfferBackupRecovery()) is what's left on
     * screen, not immediately overwritten by the hint above. */
    editorOfferBackupRecovery();

    while (1) {
        editorRefreshScreen();
        editorMaybeBackup();
        editorProcessKeypress();
    }

    return 0;
}
