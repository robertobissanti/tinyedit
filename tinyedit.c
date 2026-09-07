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
#include "backup.h"
#include "clipboard.h"
#include "syntax.h"
#include "utf8.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
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
static uint8_t search_switch_to_replace;

static int32_t search_saved_cx, search_saved_cy, search_saved_rowoff, search_saved_coloff;
static int32_t search_dir = 1; /* 1 = forward, -1 = backward */

/* Toggled by Ctrl-G inside the Ctrl-F search prompt (see
 * editorFindCallback()): when set, editorFindFrom() treats the query
 * as a POSIX extended regular expression (via <regex.h>, no external
 * dependency -- already part of libc) instead of a literal substring.
 * Reset to 0 at the start of every editorFind() call rather than
 * persisted setting-wise -- regex mode is a per-search choice, not a
 * standing preference (mirrors search_dir, which is also reset per
 * search rather than remembered across them). */
static uint8_t search_regex_mode = 0;

/* Populated by editorReadKey() when it decodes an SGR mouse report
 * (see MOUSE_EVENT_KEY in tinyedit.h) -- read immediately by
 * editorProcessKeypress() before the next editorReadKey() call can
 * overwrite them. Cb/button values used here (see SGR mouse protocol,
 * xterm ctlseqs): 0 = left button, 64 = wheel up, 65 = wheel down;
 * mouseEventCol/Row are 1-based terminal columns/rows as reported by
 * the terminal (raw screen coordinates, NOT yet translated into
 * file/row offsets -- editorProcessKeypress() does that translation,
 * since it needs E.rowoff/E.coloff/gutter width which this low-level
 * decoding layer doesn't have). */
static int32_t mouseEventButton;
static int32_t mouseEventCol, mouseEventRow;
static uint8_t mouseEventPress; /* 1 = press/drag ('M' terminator), 0 = release ('m') */
static int32_t pending_key = -1;

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

/* Bracketed paste mode (\x1b[?2004h/l, a widely-supported terminal
 * extension, not a POSIX/termios setting -- toggled via an escape
 * sequence written to the terminal, unlike raw mode above which is a
 * termios attribute): once enabled, the terminal wraps any pasted text
 * in ESC[200~ ... ESC[201~ markers instead of just feeding it to stdin
 * as if it had been typed. editorReadKey() watches for the start
 * marker (see PASTE_START_KEY) and editorProcessKeypress() then reads
 * the whole block in one shot via editorReadPastedText() -- turning an
 * O(paste length) sequence of individual keystroke-processing calls
 * (slow: one undo-snapshot/full redraw per character) each of which
 * ALSO ran through auto-close-pair logic as if the user had typed
 * every character (wrong: spurious closing brackets/quotes left behind
 * for every '(', '\'', '`', etc. in the pasted text) into a single
 * bulk insert. Disabled on exit like raw mode -- leaving it on would
 * change how paste behaves in whatever the user's shell/next program
 * is after tinyedit quits. */
static void disableBracketedPaste(void) {
    write(STDOUT_FILENO, "\x1b[?2004l", 8);
}

static void enableBracketedPaste(void) {
    atexit(disableBracketedPaste);
    write(STDOUT_FILENO, "\x1b[?2004h", 8);
}

/* Non-blocking check for whether another byte is already sitting in
 * the input stream, ready to read without waiting. Used to coalesce
 * bursts of mouse events (see MOUSE_EVENT_KEY in
 * editorProcessKeypress()): a single physical trackpad/wheel scroll
 * gesture generates many individual SGR mouse reports in rapid
 * succession, and without this, the main loop would do one full
 * screen redraw PER report -- by the time redraw N finishes, reports
 * N+1..N+20 are already queued, so the display visibly lags behind
 * the gesture. Checking this after handling one event lets the loop
 * drain and apply the whole burst before redrawing once. */
static uint8_t stdinHasDataReady(void) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

/* SGR mouse reporting (\x1b[?1002h enables click+drag button-motion
 * events, \x1b[?1006h switches their encoding to the SGR variant --
 * unbounded coordinates and unambiguous press/release, vs. the legacy
 * X10 encoding this project doesn't use). Toggled at runtime by the
 * S.mouse_enabled setting (F2), NOT unconditionally at startup like
 * bracketed paste above -- enabling it hands every click/drag to
 * tinyedit instead of the terminal's own text selection (e.g.
 * Cmd+C/Cmd+V on Ghostty), so it must be an explicit opt-in. Still registered
 * with atexit() once turned on, same reasoning as bracketed paste:
 * must not leak into whatever runs in this terminal after tinyedit
 * quits, regardless of how the setting was left. */
static void disableMouseReporting(void) {
    write(STDOUT_FILENO, "\x1b[?1002l\x1b[?1006l", 16);
}

static void enableMouseReporting(void) {
    atexit(disableMouseReporting);
    write(STDOUT_FILENO, "\x1b[?1002h\x1b[?1006h", 16);
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

/* Consumes and discards bytes from stdin up to and including the next
 * CSI terminator (a final byte in 0x40-0x7E, i.e. '@'-'~' -- ANSI
 * X3.64/ECMA-48's definition, covers every letter and '~') or up to
 * `max` bytes, whichever comes first. Used when editorReadKey() has
 * recognized the start of an escape sequence (ESC [ ...) but the
 * specific parameter layout doesn't match any pattern it knows how to
 * interpret -- e.g. a terminal sending Shift+Enter or similar modified
 * keys as "ESC [ 27 ; 2 ; 13 ~" (the modifyOtherKeys CSI-u-family
 * format some terminals use), which has one more ';'-separated field
 * than the nav-key patterns above expect. Without draining the rest of
 * an unrecognized sequence here, its trailing bytes (e.g. "13~") get
 * left in the input stream and are read one at a time by the *next*
 * calls to editorReadKey(), landing in the buffer as literal text --
 * this is the bug reported for Shift+Enter, generalized to any
 * unrecognized CSI sequence rather than special-cased per key. `max`
 * bounds the drain so a malformed/adversarial stream can't block here
 * forever waiting for a terminator that never arrives. */
static void editorDrainUnknownCsiSequence(int32_t max) {
    for (int32_t i = 0; i < max; i++) {
        uint8_t b;
        if (read(STDIN_FILENO, &b, 1) != 1) return;
        if (b >= 0x40 && b <= 0x7E) return;
    }
}

static int32_t editorReadKey(void) {
    if (pending_key >= 0) {
        int32_t key = pending_key;
        pending_key = -1;
        return key;
    }
    ssize_t nread;
    uint8_t c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno == EINTR && winsize_changed)
            return CTRL_KEY('l'); /* no-op key: lets the main loop redraw */
        if (nread == -1 && errno != EAGAIN && errno != EINTR) die("read");
    }

    if (c == '\x1b') {
        uint8_t seq[3];
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
                } else if (seq[1] == '2' && seq[2] == '0') {
                    /* Bracketed paste markers: ESC[200~ (paste start)
                     * and ESC[201~ (paste end) -- both 4 bytes after
                     * '[' ("200~"/"201~"), one byte longer than every
                     * other sequence this function recognizes (which
                     * top out at 3: <digit><digit>'~' or
                     * <digit>';'<mod><term>), so they need their own
                     * branch and their own extra read() rather than
                     * fitting the seq[2]=='~'/';' cases above -- at
                     * this point only "20" of "200"/"201" has been
                     * consumed (seq[1]='2', seq[2]='0'), two bytes
                     * (the third digit and '~') are still pending.
                     * Only paste START is useful to report here: START
                     * is what tells the caller
                     * (editorProcessKeypress()) to switch into
                     * bulk-paste mode via editorReadPastedText(), which
                     * itself reads and consumes bytes up through the
                     * END marker -- so END is never seen from this
                     * side under normal operation. If it somehow is
                     * (e.g. a END with no matching START, or read
                     * outside of paste mode), fall through to the
                     * plain '\x1b' return below like any other
                     * unrecognized sequence, rather than inventing a
                     * meaning for it. */
                    uint8_t third_digit, term;
                    if (read(STDIN_FILENO, &third_digit, 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &term, 1) != 1) return '\x1b';
                    if (term == '~' && third_digit == '0') return PASTE_START_KEY;
                } else if (seq[1] == '1' && seq[2] == '3') {
                    uint8_t term;
                    if (read(STDIN_FILENO, &term, 1) != 1) return '\x1b';
                    if (term == '~') return F3_KEY; /* common CSI F3 form: ESC[13~ */
                } else if (seq[2] >= '0' && seq[2] <= '9') {
                    /* CSI-u (modifyOtherKeys/fixterms) form for a plain
                     * key with modifiers: ESC[<codepoint>;<mod>u, e.g.
                     * Ctrl-Shift-S = ESC[115;6u (115 = lowercase 's';
                     * Shift is folded into the modifier field, not the
                     * codepoint). Reached here because the codepoint's
                     * first two digits ("11" of "115") didn't match any
                     * of the fixed 2-byte-prefix cases above -- covers
                     * any 3-digit-or-more codepoint prefix. Digits/mod
                     * are read as variable-width runs like the SGR
                     * mouse report below, since CSI-u doesn't pad
                     * fields to a fixed width. Only decoded for
                     * codepoint 115 ('s') to recognize Ctrl-Shift-S; any
                     * other codepoint here is a modified key this
                     * project doesn't otherwise bind, so it's dropped
                     * rather than mapped to something surprising. */
                    int32_t fields[2] = {(seq[1] - '0') * 10 + (seq[2] - '0'), 0};
                    int32_t field_idx = 0;
                    uint8_t term = 0;
                    uint8_t ok = 1;
                    for (int32_t guard = 0; guard < 16; guard++) {
                        uint8_t b;
                        if (read(STDIN_FILENO, &b, 1) != 1) { ok = 0; break; }
                        if (b >= '0' && b <= '9') {
                            fields[field_idx] = fields[field_idx] * 10 + (b - '0');
                        } else if (b == ';') {
                            field_idx++;
                            if (field_idx > 1) { ok = 0; break; }
                        } else if (b == 'u') {
                            term = b;
                            break;
                        } else {
                            ok = 0;
                            break;
                        }
                    }
                    if (ok && term && field_idx == 1 && fields[0] == 115) {
                        /* mod is a bitmask + 1 (1 = no modifiers, 2 =
                         * Shift, 5 = Ctrl, 6 = Ctrl+Shift). */
                        if (fields[1] == 6) return SAVE_AS_KEY;
                    } else if (!ok) {
                        editorDrainUnknownCsiSequence(16);
                    }
                    return '\x1b';
                } else if (seq[2] == ';') {
                    /* Modified nav key. Two layouts share this prefix:
                     *   ESC [ 1 ; <mod> <letter>   e.g. Alt+Up = ESC[1;3A
                     *   ESC [ 5 ; <mod> ~          Shift+PageUp = ESC[5;2~
                     *   ESC [ 6 ; <mod> ~          Shift+PageDown = ESC[6;2~
                     * seq[1] tells us which: '1' terminates with a
                     * letter, '5'/'6' terminate with '~'. */
                    uint8_t mod, term;
                    if (read(STDIN_FILENO, &mod, 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &term, 1) != 1) return '\x1b';
                    uint8_t is_alt = (mod == '3');
                    uint8_t is_shift = (mod == '2');

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
                    /* `term` didn't match a known final byte -- either
                     * because it's itself a further ';'-separated
                     * parameter (e.g. modifyOtherKeys' "ESC[27;2;13~"
                     * for Shift+Enter has THREE fields, not two, so
                     * `term` here would be '1' from "13~", not the
                     * actual terminator) or a genuinely unrecognized
                     * layout. Either way, `mod` and `term` are already
                     * consumed but the real terminator (if any) is
                     * still pending in the input stream -- drain it so
                     * its bytes don't get read individually as literal
                     * text on the next editorReadKey() calls (see
                     * editorDrainUnknownCsiSequence()). 16-byte cap:
                     * generously covers every CSI layout terminals
                     * actually send (longest observed forms are under
                     * 10 bytes total), while still bounding the drain. */
                    if (term < 0x40 || term > 0x7e)
                        editorDrainUnknownCsiSequence(16);
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
                    case 'Z': return SHIFT_TAB; /* CSI Z, "backtab" */
                }
                /* seq[1] == '<': SGR mouse report, ESC[<Cb;Cx;Cy(M|m)
                 * -- decode it into the mouseEvent* globals rather than
                 * just draining it (unlike '?' below, which this
                 * project has no use for beyond not misreading it as
                 * literal text). Reads digits/semicolons directly off
                 * stdin since the fields are variable-width (unlike
                 * every fixed-layout CSI sequence handled above). A
                 * malformed/truncated report (terminal disconnect
                 * mid-sequence, or a mouse protocol variant this
                 * project doesn't expect) falls through to the generic
                 * drain instead of returning a half-decoded event. */
                if (seq[1] == '<') {
                    int32_t fields[3] = {0, 0, 0};
                    int32_t field_idx = 0;
                    uint8_t term = 0;
                    uint8_t ok = 1;
                    for (int32_t guard = 0; guard < 32; guard++) {
                        uint8_t b;
                        if (read(STDIN_FILENO, &b, 1) != 1) { ok = 0; break; }
                        if (b >= '0' && b <= '9') {
                            fields[field_idx] = fields[field_idx] * 10 + (b - '0');
                        } else if (b == ';') {
                            field_idx++;
                            if (field_idx > 2) { ok = 0; break; }
                        } else if (b == 'M' || b == 'm') {
                            term = b;
                            break;
                        } else {
                            ok = 0;
                            break;
                        }
                    }
                    if (ok && term && field_idx == 2) {
                        mouseEventButton = fields[0];
                        mouseEventCol = fields[1];
                        mouseEventRow = fields[2];
                        mouseEventPress = (term == 'M');
                        return MOUSE_EVENT_KEY;
                    }
                    editorDrainUnknownCsiSequence(16);
                } else if (seq[1] == '?') {
                    /* DEC private-mode reply -- known multi-byte-
                     * parameter prefix, not a final byte itself (its
                     * sequence has more bytes still pending), so drain
                     * it. Any OTHER unrecognized seq[1] is assumed to
                     * already BE the final byte of a two-byte-total CSI
                     * sequence (matching every other case in this
                     * switch, which are all exactly "ESC [ <letter>")
                     * -- draining there on a guess would risk consuming
                     * the user's next real keystroke while waiting for
                     * a terminator that already passed. */
                    editorDrainUnknownCsiSequence(16);
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                case 'P': return F1_KEY; /* SS3 F1, ESC O P -- verified on Ghostty and Terminal.app */
                case 'Q': return F2_KEY; /* SS3 F2, e.g. ESC O Q on Ghostty */
                case 'R': return F3_KEY; /* SS3 F3, ESC O R */
                case 'S': return F4_KEY; /* SS3 F4, ESC O S */
            }
        }
        return '\x1b';
    }
    return c;
}

/* Reads raw bytes from stdin up through (but not including) the next
 * bracketed-paste end marker (ESC[201~, see editorReadKey()'s
 * PASTE_START_KEY), returning them as a malloc'd buffer with *outlen
 * set to its length. Called once editorReadKey() has already reported
 * PASTE_START_KEY -- from that point on, every byte until the end
 * marker is pasted content, not individual keystrokes, so this reads
 * raw off STDIN_FILENO directly rather than going back through
 * editorReadKey()'s key-decoding logic (which would try to interpret
 * e.g. a literal Escape byte inside the pasted text as the start of
 * some other sequence). Growable buffer since paste length is
 * unbounded. Caller owns the returned buffer (free() it). */
static char *editorReadPastedText(size_t *outlen) {
    size_t cap = 4096;
    char *buf = malloc(cap);
    size_t len = 0;

    /* Matches the literal bytes "ESC[201~" one at a time; `matched`
     * counts how many of its 6 bytes have been seen consecutively so
     * far, reset to 0 on any mismatch (so a stray partial match, e.g.
     * pasted text that itself contains "\x1b[20" followed by something
     * else, doesn't falsely end the paste early -- those bytes get
     * appended to the output like any other pasted byte once the
     * mismatch is detected). */
    static const char end_marker[] = "\x1b[201~";
    int32_t matched = 0;

    while (1) {
        uint8_t c;
        if (read(STDIN_FILENO, &c, 1) != 1) break; /* stream ended unexpectedly; return what we have */

        if (c == (uint8_t)end_marker[matched]) {
            matched++;
            if (matched == (int32_t)(sizeof(end_marker) - 1)) break; /* full end marker consumed */
            continue;
        }

        /* Mismatch: flush whatever partial match we'd accumulated
         * (those bytes are real pasted content, not part of the end
         * marker after all) before handling `c` itself. */
        if (matched > 0) {
            if (len + (size_t)matched > cap) {
                while (len + (size_t)matched > cap) cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(&buf[len], end_marker, (size_t)matched);
            len += (size_t)matched;
            matched = 0;
        }

        if (c == (uint8_t)end_marker[0]) {
            /* `c` itself starts a fresh potential match (e.g. the
             * mismatch above was itself an ESC starting a different
             * sequence) -- don't append it yet, let the next iteration
             * decide. */
            matched = 1;
            continue;
        }

        if (len == cap) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)c;
    }

    *outlen = len;
    return buf;
}

static int32_t getCursorPosition(int32_t *rows, int32_t *cols) {
    char buf[32];
    uint32_t i = 0;
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

static int32_t getWindowSize(int32_t *rows, int32_t *cols) {
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

static int32_t editorRowCxToRx(erow *row, int32_t cx) {
    int32_t rx = 0;
    int32_t j = 0;
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
        j += (int32_t)clen;
    }
    return rx;
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
 * S.syntax_highlight is off or E.filename's extension isn't a
 * recognized language, so this doesn't need to check that first. */
/* `force` skips the early-break below: it exists for callers where every
 * row's hl_open_comment/hl_open_math is still its post-editorInsertRow()
 * default of 0 rather than a value produced by a real tokenize pass (e.g.
 * right after a "Save as" gives an untitled buffer its first filetype), so
 * a row matching that default doesn't mean its highlighting is settled. */
static void editorRehighlightFrom(int32_t from, uint8_t force) {
    uint8_t open_comment = from > 0 ? E.row[from - 1].hl_open_comment : 0;
    uint8_t open_math = from > 0 ? E.row[from - 1].hl_open_math : 0;
    for (int32_t i = from; i < E.numrows; i++) {
        uint8_t prev_comment = E.row[i].hl_open_comment;
        uint8_t prev_math = E.row[i].hl_open_math;
        syntaxHighlightRow(&E.row[i], E.filename, (uint8_t)S.syntax_highlight, open_comment, open_math);
        open_comment = E.row[i].hl_open_comment;
        open_math = E.row[i].hl_open_math;
        if (!force && i > from && open_comment == prev_comment && open_math == prev_math) break;
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
    row->render = malloc((size_t)(row->size + tabs * (S.tab_stop - 1) + 1));

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

    /* Index into E.row: editorUpdateRow() is also called before a row
     * has been linked into E.row (e.g. editorInsertRow() calls it on
     * E.row[at] after already growing/placing it, so this is safe;
     * see call sites below). */
    int32_t idx_in_buffer = (int32_t)(row - E.row);
    uint8_t prev_open_comment = idx_in_buffer > 0 ? E.row[idx_in_buffer - 1].hl_open_comment : 0;
    uint8_t prev_open_math = idx_in_buffer > 0 ? E.row[idx_in_buffer - 1].hl_open_math : 0;
    syntaxHighlightRow(row, E.filename, (uint8_t)S.syntax_highlight, prev_open_comment, prev_open_math);
    if (idx_in_buffer >= 0 && idx_in_buffer + 1 < E.numrows)
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
    for (int32_t i = 0; i < E.numrows; i++) editorUpdateRow(&E.row[i]);
}

static void editorInsertRow(int32_t at, const char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (size_t)(E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (size_t)(E.numrows - at));

    E.row[at].size = (int32_t)len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    E.row[at].hl_open_math = 0;
    E.row[at].seg_start = NULL;
    E.row[at].seg_start_rx = NULL;
    E.row[at].seg_count = 0;
    E.row[at].seg_wrapcols = -1;
    E.numrows++;
    editorUpdateRow(&E.row[at]);

    E.dirty = 1;
}

static void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
    free(row->seg_start);
    free(row->seg_start_rx);
}

static void editorDelRow(int32_t at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (size_t)(E.numrows - at - 1));
    E.numrows--;
    if (at < E.numrows) editorRehighlightFrom(at, 0);
    E.dirty = 1;
}

static void editorRowInsertChar(erow *row, int32_t at, int32_t c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, (size_t)(row->size + 2));
    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;
    editorUpdateRow(row);
    E.dirty = 1;
}

static void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, (size_t)row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int32_t)len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty = 1;
}

static void editorRowDelChar(erow *row, int32_t at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    editorUpdateRow(row);
    E.dirty = 1;
}

/* ---- undo / redo ------------------------------------------------------- */

static void editorSetStatusMessage(const char *fmt, ...);

/* Deep-copies text and cursor only. Rendering depends on the live
 * settings and must be rebuilt when a snapshot is restored. */
static undoSnapshot editorMakeSnapshot(void) {
    undoSnapshot snap;
    snap.numrows = E.numrows;
    snap.cx = E.cx;
    snap.cy = E.cy;
    snap.row = malloc(sizeof(undoRow) * (size_t)E.numrows);
    for (int32_t i = 0; i < E.numrows; i++) {
        snap.row[i].size = E.row[i].size;
        snap.row[i].chars = malloc((size_t)E.row[i].size + 1);
        memcpy(snap.row[i].chars, E.row[i].chars, (size_t)E.row[i].size + 1);
    }
    return snap;
}

static void editorFreeSnapshot(undoSnapshot *snap) {
    for (int32_t i = 0; i < snap->numrows; i++) {
        free(snap->row[i].chars);
    }
    free(snap->row);
    snap->row = NULL;
    snap->numrows = 0;
}

static void editorClearRedoStack(void) {
    for (int32_t i = 0; i < E.redo_count; i++) editorFreeSnapshot(&E.redo_stack[i]);
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
    uint8_t coalesce = (type != EDIT_OTHER) &&
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
    for (int32_t i = 0; i < E.numrows; i++) editorFreeRow(&E.row[i]);
    free(E.row);

    E.numrows = snap->numrows;
    E.row = malloc(sizeof(erow) * (size_t)E.numrows);
    for (int32_t i = 0; i < E.numrows; i++) {
        E.row[i].size = snap->row[i].size;
        E.row[i].rsize = 0;
        E.row[i].chars = malloc((size_t)snap->row[i].size + 1);
        memcpy(E.row[i].chars, snap->row[i].chars, (size_t)snap->row[i].size + 1);
        E.row[i].render = NULL;
        E.row[i].hl = NULL;
        E.row[i].hl_open_comment = 0;
        E.row[i].hl_open_math = 0;
        E.row[i].seg_start = NULL;
        E.row[i].seg_start_rx = NULL;
        E.row[i].seg_count = 0;
        E.row[i].seg_wrapcols = -1;
    }
    /* Rebuild every row with the current invisibles/tab settings and
     * syntax highlighting, including rows beyond unchanged comment state. */
    editorUpdateAllRows();
    E.cx = snap->cx;
    E.cy = snap->cy;
    if (E.cy > E.numrows) E.cy = E.numrows;
    E.dirty = 1;
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

static void editorInsertChar(int32_t c) {
    editorPushUndo(EDIT_INSERT);
    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

/* Splits the current row without creating an undo snapshot. Callers that
 * expose this as one user action must push exactly one snapshot themselves. */
static void editorInsertNewlineRaw(void) {
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

static void editorInsertNewline(void) {
    editorPushUndo(EDIT_OTHER);
    editorInsertNewlineRaw();
}

/* Enter as typed by the user (as opposed to a newline embedded in
 * pasted/recovered text, which goes through editorInsertNewline()
 * directly and must NOT be reindented -- the source already has
 * whatever indentation it has). When S.auto_indent is on, copies the
 * leading whitespace (spaces/tabs, nothing else) of the line the
 * cursor was on before the split onto the new line, so continuing to
 * type keeps the same indent level without retyping it by hand. */
static void editorInsertNewlineAutoIndent(void) {
    int32_t src_row = E.cy;
    int32_t indent_len = 0;
    if (S.auto_indent && src_row < E.numrows) {
        erow *row = &E.row[src_row];
        while (indent_len < row->size &&
               (row->chars[indent_len] == ' ' || row->chars[indent_len] == '\t'))
            indent_len++;
        /* Splitting mid-indent (cursor sits inside the leading
         * whitespace itself) shouldn't duplicate more of it than the
         * new line already inherits from the split -- cap at cx. */
        if (indent_len > E.cx) indent_len = E.cx;
    }

    editorInsertNewline();

    if (indent_len > 0) {
        erow *row = &E.row[src_row];
        for (int32_t i = 0; i < indent_len; i++)
            editorInsertChar((unsigned char)row->chars[i]);
    }
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
            editorRowDelChar(row, E.cx - 1 - (int32_t)k);
        E.cx -= (int32_t)del_count;
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
    for (int32_t j = 0; j < E.numrows; j++)
        totlen += (size_t)E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int32_t j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, (size_t)E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

static void editorSetStatusMessage(const char *fmt, ...);
static void editorSetStatusMessageSticky(const char *fmt, ...);
static void editorRefreshScreen(void);
static int32_t editorReadKey(void);
static int32_t editorReadMultiByteKey(uint8_t lead, char *out);
static void editorFreeUndoRedo(void);
static void abAppend(struct abuf *ab, const char *s, int32_t len);
static void abFree(struct abuf *ab);

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
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        const char *active_prompt = prompt;
        if (short_prompt) {
            /* Two independent size limits, both checked: E.screencols
             * (the visible width -- text beyond it never gets a
             * chance to show, see editorDrawMessageBar()'s
             * tail-scroll) AND sizeof(E.statusmsg) (the fixed 80-byte
             * buffer editorSetStatusMessage() formats into --
             * vsnprintf() silently truncates whatever doesn't fit
             * there, which bit *before* the screencols check ever
             * mattered: on a wide terminal the long prompt "fits" on
             * screen but still gets truncated by vsnprintf() into
             * E.statusmsg's 80 bytes, silently dropping the tail of
             * the query the user typed -- this is what the user saw
             * as "search freezes after 5 characters" even though the
             * search itself kept working on the full, untruncated
             * `buf`). Whichever limit is smaller determines whether
             * the long prompt can be shown at all. */
            char probe[sizeof(E.statusmsg)];
            int32_t plen;
            if (status_fn) plen = snprintf(probe, sizeof(probe), prompt, status_fn(), buf);
            else plen = snprintf(probe, sizeof(probe), prompt, buf);
            int32_t stmsg_limit = (int32_t)sizeof(E.statusmsg) - 1;
            int32_t limit = E.screencols < stmsg_limit ? E.screencols : stmsg_limit;
            if (plen > limit) active_prompt = short_prompt;
        }
        if (status_fn) editorSetStatusMessage(active_prompt, status_fn(), buf);
        else editorSetStatusMessage(active_prompt, buf);
        editorRefreshScreen();

        int32_t c = editorReadKey();
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
        } else if (c >= 32 && c < 127) {
            if (buflen + 2 > bufsize) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = (char)c;
            buf[buflen] = '\0';
        } else if (c >= 0x80 && c <= 0xff) {
            char seq[4];
            int32_t seq_len = editorReadMultiByteKey((uint8_t)c, seq);
            while (buflen + (size_t)seq_len + 1 > bufsize) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            memcpy(buf + buflen, seq, (size_t)seq_len);
            buflen += (size_t)seq_len;
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
    return editorPromptCB(prompt, NULL, NULL, NULL);
}

/* Discards every row currently in the buffer (but not E.filename),
 * resetting to a blank document. Used before reloading content from
 * scratch -- e.g. replacing what editorOpen() read from disk with a
 * newer crash-recovery backup, see editorOfferBackupRecovery(). */
static void editorClearRows(void) {
    for (int32_t i = 0; i < E.numrows; i++) editorFreeRow(&E.row[i]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
    E.cx = 0;
    E.cy = 0;
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
            if (i < len || linelen > 0) editorInsertRow(E.numrows, data + start, linelen);
            start = i + 1;
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
    if (interval <= 0 || !E.dirty || !E.filename) return;
    if (interval < 5) interval = 5; /* see settings.h: backup_interval */

    time_t now = time(NULL);
    if (E.last_backup_time != 0 && now - E.last_backup_time < interval) return;

    size_t len;
    char *buf = editorRowsToString(&len);
    backupWrite(E.filename, buf, len);
    free(buf);
    E.last_backup_time = now;
}

/* If a crash-recovery backup exists for E.filename, asks the user
 * whether to load it in place of what editorOpen() just read from
 * disk. Called once at startup, after editorOpen(). A backup existing
 * at all is exactly the crash signal (see backup.h): a clean exit
 * always removes its own backup, so one surviving to the next
 * startup means the previous session never got to do that. Marks the
 * buffer dirty on recovery (it now differs from what's on disk) and
 * leaves the backup file itself alone -- it gets cleaned up on the
 * next successful save or clean quit like any other session's. */
/* Draws one line of the recovery screen, centered, padded to
 * E.screencols with `bg` as background so the whole row reads as a
 * solid colored bar rather than text floating on the normal
 * background -- this is what makes the screen impossible to miss
 * compared to a message-bar prompt buried at the bottom. */
static void editorRecoveryScreenLine(struct abuf *ab, const char *bg, const char *text) {
    int32_t textlen = (int32_t)strlen(text);
    if (textlen > E.screencols) textlen = E.screencols;
    int32_t padding = (E.screencols - textlen) / 2;

    abAppend(ab, bg, (int32_t)strlen(bg));
    for (int32_t i = 0; i < padding; i++) abAppend(ab, " ", 1);
    abAppend(ab, text, textlen);
    for (int32_t i = padding + textlen; i < E.screencols; i++) abAppend(ab, " ", 1);
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

    int32_t mid = E.screenrows / 2;
    for (int32_t i = 0; i < mid - 2; i++) editorRecoveryScreenLine(&ab, bg, "");
    editorRecoveryScreenLine(&ab, bg, "");
    editorRecoveryScreenLine(&ab, bg, "!!  UNSAVED CHANGES FOUND  !!");
    editorRecoveryScreenLine(&ab, bg, "");
    char msg[160];
    snprintf(msg, sizeof(msg), "A previous session on \"%.100s\" did not exit cleanly.",
        E.filename ? E.filename : "");
    editorRecoveryScreenLine(&ab, bg, msg);
    editorRecoveryScreenLine(&ab, bg, "Restore the recovered changes?  [y] Yes    [n] No");
    editorRecoveryScreenLine(&ab, bg, "");
    for (int32_t i = mid + 3; i < E.screenrows; i++) editorRecoveryScreenLine(&ab, bg, "");

    abAppend(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

static void editorOfferBackupRecovery(void) {
    if (!E.filename || !backupExists(E.filename)) return;

    editorRecoveryScreen();
    int32_t c = editorReadKey();
    if (c != 'y' && c != 'Y') {
        editorSetStatusMessage("");
        return;
    }

    size_t len;
    char *content = backupRead(E.filename, &len);
    if (!content) {
        editorSetStatusMessage("Could not read recovery backup.");
        return;
    }

    editorClearRows();
    editorLoadLines(content, len);
    free(content);
    E.dirty = 1;
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
    char *tmppath = malloc(target_len + sizeof(suffix));
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
 * again (F4/Ctrl-Shift-S) instead of silently overwriting E.filename,
 * which is what a plain save (Ctrl-S) does when a name already exists. */
static void editorSaveInternal(uint8_t force_prompt) {
    if (E.filename == NULL || force_prompt) {
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
        free(E.filename);
        E.filename = name;
        /* Filetype is derived from E.filename's extension, so a new
         * name can turn on/change highlighting for rows tokenized
         * under a different (or no) filetype -- recompute now instead
         * of waiting for the next edit to trigger it. */
        editorRehighlightFrom(0, 1);
    }

    size_t len;
    char *buf = editorRowsToString(&len);

    if (editorAtomicSave(E.filename, buf, len)) {
        free(buf);
        E.dirty = 0;
        /* The buffer is now identical to what's on disk --
         * the crash-recovery backup (if any) would only ever
         * offer to "recover" something we just saved, so
         * drop it instead of leaving it to confuse the next
         * startup's crash check. */
        backupRemove(E.filename);
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

/* Shared save/discard/cancel gate for every operation that would leave the
 * current document (quit, close, or open another file). Returns 1 only when
 * it is safe to proceed; a failed/cancelled save leaves the document open. */
static uint8_t editorConfirmDocumentChange(const char *action) {
    if (!E.dirty) return 1;

    editorSetStatusMessage("Save changes before %s? (y/n/Esc to cancel)", action);
    editorRefreshScreen();
    int32_t c = editorReadKey();
    if (c == 'y' || c == 'Y') {
        editorSave();
        return !E.dirty;
    }
    if (c == 'n' || c == 'N') return 1;

    editorSetStatusMessage("");
    return 0;
}

/* Releases every piece of state owned by the current document while keeping
 * terminal dimensions and user settings intact. The next document starts
 * with independent cursor, selection, search, backup, and undo state. */
static void editorResetDocument(void) {
    if (E.filename) backupRemove(E.filename);
    editorClearRows();
    free(E.filename);
    E.filename = NULL;
    editorFreeUndoRedo();

    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.free_scroll = 0;
    E.dirty = 0;
    E.sel_active = 0;
    E.sel_anchor_x = 0;
    E.sel_anchor_y = 0;
    E.sel_pinned = 0;
    E.search_match_y = -1;
    E.search_match_x = 0;
    E.search_match_len = 0;
    E.last_backup_time = 0;
    E.last_edit_type = EDIT_NONE;
    E.last_edit_time = 0;
}

/* Ctrl-W closes only the current document. tinyedit remains alive with a
 * fresh unnamed buffer, ready for typing or Ctrl-O. */
static void editorCloseFile(void) {
    if (!editorConfirmDocumentChange("closing")) return;
    editorResetDocument();
    editorSetStatusMessageSticky("File closed. Ctrl-O open | Ctrl-Q quit | F1 help");
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
    if (!E.dirty) {
        if (exists) editorSetStatusMessage("Opened %s", E.filename);
        else editorSetStatusMessage("New file: %s", E.filename);
    }
}

static void editorQuit(void) {
    if (!editorConfirmDocumentChange("quitting")) return;

    /* A discarded dirty buffer must not leave a recovery file that would
     * look like evidence of a crash the next time this path is opened. */
    if (E.filename) backupRemove(E.filename);

    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
}

/* ---- append buffer -------------------------------------------------------- */

static void abAppend(struct abuf *ab, const char *s, int32_t len) {
    char *new = realloc(ab->b, (size_t)(ab->len + len));
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

static uint8_t editorGetSelection(int32_t *start_y, int32_t *start_x, int32_t *end_y, int32_t *end_x);

/* Width of the left-hand line-number gutter, including one space of
 * padding before the text starts. Zero when gutter is disabled. Grows
 * with E.numrows so files with 1000+ lines still right-align cleanly. */
static int32_t editorGutterWidth(void) {
    if (!S.show_line_numbers) return 0;
    int32_t digits = 3;
    int32_t n = E.numrows;
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
    int32_t cols = E.screencols - editorGutterWidth();
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
 * E.rx, itself computed by editorRowCxToRx() which is UTF-8-aware)
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
    row->seg_start = malloc(sizeof(int32_t) * (size_t)capacity);
    row->seg_start_rx = malloc(sizeof(int32_t) * (size_t)capacity);
    if (!row->seg_start || !row->seg_start_rx) die("malloc wrap segments");

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
            int32_t *new_start = realloc(row->seg_start, sizeof(int32_t) * (size_t)capacity);
            if (!new_start) die("realloc wrap segments");
            row->seg_start = new_start;

            int32_t *new_rx = realloc(row->seg_start_rx, sizeof(int32_t) * (size_t)capacity);
            if (!new_rx) die("realloc wrap segments");
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
 * that need to compare/combine it with other column values (E.rx,
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
    return editorRowSegments(&E.row[filerow], wrapcols);
}

/* Converts a (filerow, segment index) pair into an absolute video-row
 * number, counting every visual segment of every row from 0 up to
 * (but not including) filerow, plus `seg` segments into filerow
 * itself. This is the wrapped-mode equivalent of "filerow" alone in
 * unwrapped mode -- E.rowoff and the viewport's y position are both
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
    for (int32_t i = 0; i < E.numrows; i++) {
        int32_t h = editorRowVideoHeight(i, wrapcols);
        if (vy_left < h) {
            *out_filerow = i;
            *out_seg = vy_left;
            return;
        }
        vy_left -= h;
    }
    *out_filerow = E.numrows > 0 ? E.numrows - 1 : 0;
    *out_seg = 0;
}

/* Total number of video rows across the whole file (sum of every
 * row's visual height). Used to clamp scrolling past the end. */
static int32_t editorTotalVideoRows(int32_t wrapcols) {
    int32_t total = 0;
    for (int32_t i = 0; i < E.numrows; i++)
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
        int32_t vy = E.rowoff + cursor_row;
        int32_t seg;
        editorFileRowAtVideoRow(vy, wrapcols, &filerow, &seg);
        if (E.numrows == 0) {
            *out_cy = 0; *out_cx = 0;
            return;
        }
        erow *row = &E.row[filerow];
        int32_t nseg = editorRowSegments(row, wrapcols);
        if (seg >= nseg) seg = nseg - 1;
        int32_t target_rx = row->seg_start_rx[seg] + cursor_col;
        /* Walk the segment's characters to find the byte offset whose
         * rendered column is closest to target_rx -- same "walk with
         * utf8NextCharLen, never assume 1 byte == 1 column" rule as
         * everywhere else in this codebase, since a
         * clicked column can land in the middle of a wide/multi-byte
         * glyph. */
        int32_t seg_end = (seg + 1 < nseg) ? row->seg_start[seg + 1] : row->size;
        int32_t pos = row->seg_start[seg];
        int32_t rx = row->seg_start_rx[seg];
        while (pos < seg_end) {
            size_t clen = utf8NextCharLen(row->chars, (size_t)pos, (size_t)row->size);
            if (clen == 0) clen = 1;
            int32_t w = utf8SingleCharWidth(row->chars + pos, clen);
            if (rx + w > target_rx) break;
            rx += w;
            pos += (int32_t)clen;
        }
        cx = pos;
    } else {
        filerow = E.rowoff + cursor_row;
        if (filerow >= E.numrows) filerow = E.numrows > 0 ? E.numrows - 1 : 0;
        if (E.numrows == 0) {
            *out_cy = 0; *out_cx = 0;
            return;
        }
        erow *row = &E.row[filerow];
        int32_t target_rx = E.coloff + cursor_col;
        int32_t pos = 0, rx = 0;
        while (pos < row->size) {
            size_t clen = utf8NextCharLen(row->chars, (size_t)pos, (size_t)row->size);
            if (clen == 0) clen = 1;
            int32_t w = utf8SingleCharWidth(row->chars + pos, clen);
            if (rx + w > target_rx) break;
            rx += w;
            pos += (int32_t)clen;
        }
        cx = pos;
    }

    *out_cy = filerow;
    *out_cx = cx;
}

static void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    /* See E.free_scroll's declaration in tinyedit.h: the mouse wheel
     * sets this to scroll the view without the cursor being dragged
     * along to follow it -- consumed (and cleared) here, right before
     * the normal follow-the-cursor logic would otherwise immediately
     * undo that by re-centering E.rowoff around the (stationary)
     * cursor. One-shot: any redraw after this one goes through the
     * normal path again, so real cursor movement still keeps the
     * cursor on screen as usual. */
    if (E.free_scroll) {
        E.free_scroll = 0;
        return;
    }

    int32_t wrapcols = editorSoftWrapCols();

    if (wrapcols > 0) {
        /* Wrapped mode: vertical scrolling is in video rows, horizontal
         * scrolling is disabled (a wrapped line never exceeds the text
         * width by construction, so E.coloff stays 0). E.cy can be one
         * past the last row (e.g. right after deleting the last line,
         * or mid-edit before clamping) -- there's no row to segment
         * there, so cursor_vy is just the video row right after the
         * last line (0 for an empty buffer). */
        int32_t cursor_vy;
        if (E.cy < E.numrows) {
            int32_t seg_idx, seg_col;
            editorRxToSegment(&E.row[E.cy], wrapcols, E.rx, &seg_idx, &seg_col);
            cursor_vy = editorVideoRowOf(E.cy, seg_idx, wrapcols);
        } else {
            cursor_vy = editorTotalVideoRows(wrapcols);
        }

        if (cursor_vy < E.rowoff) E.rowoff = cursor_vy;
        if (cursor_vy >= E.rowoff + E.screenrows) E.rowoff = cursor_vy - E.screenrows + 1;
        if (E.rowoff < 0) E.rowoff = 0;
        E.coloff = 0;
    } else {
        if (E.cy < E.rowoff) E.rowoff = E.cy;
        if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
        if (E.rx < E.coloff) E.coloff = E.rx;
        int32_t textcols = editorTextCols();
        if (E.rx >= E.coloff + textcols) E.coloff = E.rx - textcols + 1;
    }
}

/* Draws the render-byte range [seg_from, seg_to) of `filerow` into
 * `ab`, applying selection/search-match highlight -- the body shared
 * by both the unwrapped (one call per file row) and wrapped (one call
 * per visual segment) paths in editorDrawRows(). */
static void editorDrawRowSegment(struct abuf *ab, int32_t filerow, int32_t seg_from, int32_t seg_to,
    uint8_t has_sel, int32_t sel_y0, int32_t sel_x0, int32_t sel_y1, int32_t sel_x1) {
    erow *row = &E.row[filerow];
    int32_t len = seg_to - seg_from;
    if (len <= 0) return;

    char *line = &row->render[seg_from];
    int32_t row_sel_start = -1, row_sel_end = -1;
    if (has_sel && filerow >= sel_y0 && filerow <= sel_y1) {
        row_sel_start = (filerow == sel_y0) ? sel_x0 : 0;
        row_sel_end = (filerow == sel_y1) ? sel_x1 : row->size;
    }

    int32_t match_start = -1, match_end = -1;
    if (E.search_match_y == filerow) {
        match_start = E.search_match_x;
        match_end = E.search_match_x + E.search_match_len;
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
        if (!should_sel && !is_invisible_glyph && row->hl && rendercol < row->rsize)
            syn_color = syntaxColorFor((enum syntaxHighlight)row->hl[rendercol], &S);
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
    if (filerow < E.numrows && !is_continuation) {
        snprintf(numbuf, sizeof(numbuf), "%*d ", safe_gutter - 1, filerow + 1);
    } else {
        snprintf(numbuf, sizeof(numbuf), "%*s ", safe_gutter - 1, "");
    }
    const char *gutter_color = ansiColorCode(S.color_gutter);
    abAppend(ab, gutter_color, (int32_t)strlen(gutter_color));
    abAppend(ab, numbuf, safe_gutter);
    abAppendReset(ab);
}

static void editorDrawRows(struct abuf *ab) {
    int32_t sel_y0 = 0, sel_x0 = 0, sel_y1 = 0, sel_x1 = 0;
    uint8_t has_sel = editorGetSelection(&sel_y0, &sel_x0, &sel_y1, &sel_x1);
    int32_t gutter = editorGutterWidth();
    int32_t textcols = editorTextCols();
    int32_t wrapcols = editorSoftWrapCols();

    if (wrapcols == 0) {
        for (int32_t y = 0; y < E.screenrows; y++) {
            int32_t filerow = y + E.rowoff;
            editorDrawGutter(ab, gutter, filerow, 0);

            if (filerow >= E.numrows) {
                if (E.numrows == 0 && y == E.screenrows / 3) {
                    char welcome[80];
                    int32_t welcomelen = snprintf(welcome, sizeof(welcome),
                        "tinyedit -- version %s", TE_VERSION);
                    if (welcomelen > textcols) welcomelen = textcols;
                    int32_t padding = (textcols - welcomelen) / 2;
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
                int32_t len = E.row[filerow].rsize - E.coloff;
                if (len < 0) len = 0;
                if (len > textcols) len = textcols;
                editorDrawRowSegment(ab, filerow, E.coloff, E.coloff + len,
                    has_sel, sel_y0, sel_x0, sel_y1, sel_x1);
            }

            abAppend(ab, "\x1b[K", 3);
            abAppend(ab, "\r\n", 2);
        }
        return;
    }

    /* Wrapped mode: each video row corresponds to one visual segment
     * of a logical row, resolved via editorFileRowAtVideoRow(). */
    for (int32_t y = 0; y < E.screenrows; y++) {
        int32_t vy = y + E.rowoff;
        int32_t filerow, seg;

        if (vy >= editorTotalVideoRows(wrapcols)) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                editorDrawGutter(ab, gutter, 0, 1);
                char welcome[80];
                int32_t welcomelen = snprintf(welcome, sizeof(welcome),
                    "tinyedit -- version %s", TE_VERSION);
                if (welcomelen > textcols) welcomelen = textcols;
                int32_t padding = (textcols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                editorDrawGutter(ab, gutter, E.numrows, 0);
                abAppend(ab, "~", 1);
            }
            abAppend(ab, "\x1b[K", 3);
            abAppend(ab, "\r\n", 2);
            continue;
        }

        editorFileRowAtVideoRow(vy, wrapcols, &filerow, &seg);

        erow *row = &E.row[filerow];
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
    for (int32_t i = 0; i < E.numrows; i++) {
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
        E.filename ? E.filename : "[No Name]", E.dirty ? " (modified)" : "");
    if (len < 0) len = 0;
    if (len > E.screencols) len = E.screencols;

    abAppend(ab, status, len);
    while (len < E.screencols) {
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
            E.numrows, editorCountChars(), E.dirty ? "(modified)" : "");
    } else {
        len = snprintf(status, sizeof(status), "%.20s - %d lines, %d chars %s",
            E.filename ? E.filename : "[No Name]", E.numrows, editorCountChars(),
            E.dirty ? "(modified)" : "");
    }

    const char *filetype = editorFiletypeLabel();
    int32_t rlen;
    if (filetype)
        rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d: C %d",
            filetype, E.cy + 1, E.numrows, E.cx + 1);
    else
        rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d: C %d",
            E.cy + 1, E.numrows, E.cx + 1);
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
    abAppendReset(ab);
    abAppend(ab, "\r\n", 2);
}

static void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int32_t msglen = (int32_t)strlen(E.statusmsg);
    const char *msg = E.statusmsg;
    if (msglen > E.screencols) {
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
        msg += msglen - E.screencols;
        msglen = E.screencols;
    }
    if (msglen && (E.statusmsg_sticky || time(NULL) - E.statusmsg_time < 5))
        abAppend(ab, msg, msglen);
}

static void editorRefreshScreen(void) {
    uint8_t need_full_clear = 0;
    if (winsize_changed) {
        winsize_changed = 0;
        int32_t rows, cols;
        if (getWindowSize(&rows, &cols) == 0) {
            E.screenrows = rows - 2 - (S.show_top_bar ? 1 : 0); /* status bar + message bar (+ top bar) */
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
        if (wrapcols > 0 && E.cy < E.numrows) {
            int32_t seg_idx, seg_col;
            editorRxToSegment(&E.row[E.cy], wrapcols, E.rx, &seg_idx, &seg_col);
            cursor_row = editorVideoRowOf(E.cy, seg_idx, wrapcols) - E.rowoff;
            cursor_col = seg_col;
        } else if (wrapcols > 0) {
            /* E.cy is one past the last row (empty buffer, or right
             * after deleting the last line): video row right after
             * the last line's segments, column 0. */
            cursor_row = editorTotalVideoRows(wrapcols) - E.rowoff;
            cursor_col = 0;
        } else {
            cursor_row = E.cy - E.rowoff;
            cursor_col = E.rx - E.coloff;
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

/* Converts an (rx, in-segment column) target on a given video row back
 * into a cx (char offset) on that row's logical line -- inverse of
 * editorRowCxToRx() restricted to one visual segment. Used by Up/Down
 * in wrapped mode to preserve the cursor's horizontal position when
 * moving between visual segments, same idea as unwrapped Up/Down
 * preserving E.rx via editorRowCxToRx()/clamping below. */
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
 * segments/rows, same intent as the unwrapped path preserving E.rx. */
static void editorMoveCursorWrapped(int32_t key, int32_t wrapcols) {
    int32_t seg_idx, seg_col;
    /* Derive rx from the current file position instead of trusting the
     * frame cache E.rx. PageUp/PageDown call this function repeatedly
     * before the next redraw; E.cx changes on every iteration while
     * E.rx would otherwise remain stale, making a whole-page jump move
     * only one visual segment. */
    int32_t current_rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    editorRxToSegment(&E.row[E.cy], wrapcols, current_rx, &seg_idx, &seg_col);

    int32_t target_vy = editorVideoRowOf(E.cy, seg_idx, wrapcols) + (key == ARROW_UP ? -1 : 1);
    if (target_vy < 0) target_vy = 0;
    int32_t total = editorTotalVideoRows(wrapcols);
    if (target_vy >= total) target_vy = total - 1;

    int32_t target_filerow, target_seg;
    editorFileRowAtVideoRow(target_vy, wrapcols, &target_filerow, &target_seg);

    erow *target_row = &E.row[target_filerow];
    int32_t t_nseg = editorRowSegments(target_row, wrapcols);
    if (target_seg >= t_nseg) target_seg = t_nseg - 1;

    E.cy = target_filerow;
    E.cx = editorSegColToCx(target_row, target_row->seg_start_rx[target_seg], seg_col);
}

static void editorMoveCursor(int32_t key) {
    int32_t wrapcols = editorSoftWrapCols();
    if (wrapcols > 0 && (key == ARROW_UP || key == ARROW_DOWN) && E.cy < E.numrows) {
        editorMoveCursorWrapped(key, wrapcols);
        return;
    }

    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (row && E.cx != 0) {
                size_t back = utf8PrevCharLen(row->chars, (size_t)E.cx);
                E.cx -= (back > 0) ? (int32_t)back : 1;
            } else if (E.row && E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            } else {
                E.cx = 0;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                size_t fwd = utf8NextCharLen(row->chars, (size_t)E.cx, (size_t)row->size);
                E.cx += (fwd > 0) ? (int32_t)fwd : 1;
            } else if (row && E.cx == row->size && E.cy < E.numrows - 1) {
                /* End of a line, but not the last one: move to the start
                 * of the next line. At the very end of the document,
                 * stay put instead -- no wraparound past the last
                 * character (mirrors Arrow Left already stopping at
                 * the very start of the document). */
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
    int32_t rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/* Word-wise cursor movement for Alt+Left / Alt+Right. Skips whitespace
 * then a run of non-whitespace characters, crossing line boundaries
 * when the cursor is already at the start/end of a line. */
static void editorMoveCursorWord(uint8_t forward) {
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
        int32_t i = E.cx - 1;
        while (i > 0 && isspace((unsigned char)row->chars[i])) i--;
        while (i > 0 && !isspace((unsigned char)row->chars[i - 1])) i--;
        E.cx = i;
    }
}

/* Normalizes the selection anchor vs the current cursor position into an
 * ordered [start, end) range. Returns 0 and leaves outputs untouched if
 * there is no active selection. */
static uint8_t editorGetSelection(int32_t *start_y, int32_t *start_x, int32_t *end_y, int32_t *end_x) {
    if (!E.sel_active) return 0;

    int32_t ay = E.sel_anchor_y, ax = E.sel_anchor_x;
    int32_t cy = E.cy, cx = E.cx;

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
static char *editorSerializeRange(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x, size_t *outlen) {
    size_t totlen = 0;
    for (int32_t y = start_y; y <= end_y; y++) {
        int32_t from = (y == start_y) ? start_x : 0;
        int32_t to = (y == end_y) ? end_x : E.row[y].size;
        if (to > from) totlen += (size_t)(to - from);
        if (y != end_y) totlen += 1;
    }

    char *buf = malloc(totlen + 1);
    char *p = buf;
    for (int32_t y = start_y; y <= end_y; y++) {
        int32_t from = (y == start_y) ? start_x : 0;
        int32_t to = (y == end_y) ? end_x : E.row[y].size;
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
static void editorDeleteRange(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x) {
    editorPushUndo(EDIT_OTHER);
    if (start_y == end_y) {
        erow *row = &E.row[start_y];
        for (int32_t i = 0; i < end_x - start_x; i++)
            editorRowDelChar(row, start_x);
    } else {
        erow *first = &E.row[start_y];
        first->size = start_x;
        first->chars[first->size] = '\0';

        erow *last = &E.row[end_y];
        editorRowAppendString(first, &last->chars[end_x], (size_t)(last->size - end_x));
        editorUpdateRow(first);

        for (int32_t y = end_y; y > start_y; y--)
            editorDelRow(y);
    }
    E.cy = start_y;
    E.cx = start_x;
    E.dirty = 1;
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
    if (!editorGetSelection(&sel_y0, &sel_x0, &sel_y1, &sel_x1)) return;

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
        erow *row = &E.row[y];
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
    if (sel_x0 > E.row[sel_y0].size) sel_x0 = E.row[sel_y0].size;
    if (sel_x1 > E.row[sel_y1].size) sel_x1 = E.row[sel_y1].size;

    uint8_t cursor_at_end = (E.cy > E.sel_anchor_y) ||
        (E.cy == E.sel_anchor_y && E.cx >= E.sel_anchor_x);

    E.sel_active = 1;
    if (cursor_at_end) {
        E.sel_anchor_y = sel_y0; E.sel_anchor_x = sel_x0;
        E.cy = sel_y1; E.cx = sel_x1;
    } else {
        E.sel_anchor_y = sel_y1; E.sel_anchor_x = sel_x1;
        E.cy = sel_y0; E.cx = sel_x0;
    }
    E.dirty = 1;
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
    char *decoded = malloc(raw_len + 1);
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
 * search_regex_mode is set, `query` is compiled as a POSIX extended
 * regular expression (<regex.h>, part of libc, so it adds no external
 * dependency) instead of
 * matched as a literal substring; a malformed pattern is treated as
 * "no match" rather than surfacing regcomp()'s error, consistent with
 * how an empty query already means "no match" below rather than an
 * error dialog. On success sets E.cy/E.cx to the match start, updates
 * E.search_match_*, and returns 1. On failure clears E.search_match_y
 * to -1 and returns 0. */
/* Finds a match from the requested position. Interactive search passes
 * wrap=1 so repeated arrows cycle through the document; replace passes
 * wrap=0 so a replacement that still matches the query cannot send the
 * traversal back to the beginning forever. */
static uint8_t editorFindFrom(const char *query, int32_t from_y, int32_t from_x,
    int32_t dir, uint8_t wrap) {
    size_t qlen = strlen(query);
    if (qlen == 0 || E.numrows == 0) {
        E.search_match_y = -1;
        return 0;
    }

    regex_t re;
    uint8_t have_re = 0;
    if (search_regex_mode) {
        if (regcomp(&re, query, REG_EXTENDED) != 0) {
            E.search_match_y = -1;
            return 0;
        }
        have_re = 1;
    }

    int32_t y = from_y;
    int32_t x = from_x;
    uint8_t result = 0;

    for (int32_t steps = 0; steps <= E.numrows; steps++) {
        erow *row = &E.row[y];
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
            E.cy = y;
            E.cx = mx;
            E.search_match_y = y;
            E.search_match_x = mx;
            E.search_match_len = mlen;
            result = 1;
            break;
        }

        if (dir == 1) {
            if (!wrap && y == E.numrows - 1) break;
            y = (y + 1) % E.numrows;
            x = 0;
        } else {
            if (!wrap && y == 0) break;
            y = (y - 1 + E.numrows) % E.numrows;
            x = E.row[y].size;
        }
    }

    if (have_re) regfree(&re);
    if (result) return 1;

    E.search_match_y = -1;
    return 0;
}

static void editorFindCallback(char *query, int32_t key) {
    static int32_t last_cy = -1, last_cx = -1, last_len = 0;

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
        last_len = 0;
        return;
    }

    if (key == CTRL_KEY('r')) {
        search_switch_to_replace = 1;
        return;
    }

    if (key == CTRL_KEY('g')) {
        search_regex_mode = !search_regex_mode;
        /* Re-run the search from the saved starting position (as if
         * the query had just been retyped) so toggling mode mid-search
         * immediately reflects the new interpretation instead of
         * waiting for the next keystroke -- same reset already done
         * below when a key isn't a recognized navigation/mode key. */
        last_cy = -1;
        last_cx = -1;
        last_len = 0;
        if (strlen(query) > 0) {
            int32_t from_y = search_saved_cy, from_x = search_saved_cx;
            if (editorFindFrom(query, from_y, from_x, search_dir, 1)) {
                last_cy = E.cy;
                last_cx = E.cx;
                last_len = E.search_match_len;
            }
        }
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
        last_len = 0;
    }

    if (strlen(query) == 0) {
        E.search_match_y = -1;
        return;
    }

    int32_t from_y, from_x;
    if (last_cy == -1) {
        from_y = search_saved_cy;
        from_x = search_saved_cx;
    } else if (search_dir == 1) {
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
        from_y = (last_cy - 1 + E.numrows) % E.numrows;
        from_x = E.row[from_y].size;
    }

    if (editorFindFrom(query, from_y, from_x, search_dir, 1)) {
        last_cy = E.cy;
        last_cx = E.cx;
        last_len = E.search_match_len;
    }
}

/* Fed to editorPromptCB() as status_fn -- re-evaluated on every prompt
 * redraw, so the "[regex]"/"[literal]" indicator updates the instant
 * Ctrl-G toggles search_regex_mode, without editorFind() needing to
 * rebuild the whole prompt string itself. */
static const char *editorFindModeIndicator(void) {
    return search_regex_mode ? "[regex]" : "[literal]";
}

static void editorFind(void) {
    search_saved_cx = E.cx;
    search_saved_cy = E.cy;
    search_saved_rowoff = E.rowoff;
    search_saved_coloff = E.coloff;
    search_dir = 1;
    search_regex_mode = 0;

    /* Long form spells out every shortcut -- shown while there's room
     * for it alongside the query. Once buf grows enough that the two
     * together wouldn't fit the message bar, editorPromptCB() switches
     * to the short form ("Search [mode]: ") instead, and only once
     * THAT doesn't fit either does editorDrawMessageBar()'s
     * scroll-to-keep-tail-visible behavior take over. Three stages,
     * each only kicking in once the previous one runs out of room. */
    search_switch_to_replace = 0;
    char *query = editorPromptCB(
        "Search %s (Esc cancel, Arrows jump, Ctrl-R replace, Ctrl-G regex): %s",
        "Search %s: %s",
        editorFindModeIndicator, editorFindCallback);

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

    /* search_regex_mode carries over unchanged from the Ctrl-F search
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
        search_regex_mode ? "[regex]" : "[literal]", query);
    char replace_prompt_short[48];
    snprintf(replace_prompt_short, sizeof(replace_prompt_short), "Replace %s with: %%s",
        search_regex_mode ? "[regex]" : "[literal]");
    search_switch_to_replace = 0;
    char *replacement_input = editorPromptCB(replace_prompt_long, replace_prompt_short, NULL, NULL);
    if (!replacement_input) return;

    size_t rlen;
    char *replacement;
    if (search_regex_mode) {
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
        y = E.search_match_y;
        x = E.search_match_x;
        /* Actual matched length -- NOT strlen(query). In literal mode
         * these are always equal, but in regex mode the match can be
         * shorter or longer than the pattern text itself (e.g.
         * "[0-9]+" matching "42" is length 2, matching "123456" is
         * length 6) -- using strlen(query) here would delete/skip the
         * wrong number of characters as soon as the pattern's length
         * differs from what it actually matched. */
        int32_t mlen = E.search_match_len;

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
            erow *row = &E.row[y];
            for (int32_t k = 0; k < mlen; k++)
                editorRowDelChar(row, x);
            E.cy = y;
            E.cx = x;
            for (size_t k = 0; k < rlen; k++) {
                if (replacement[k] == '\n') {
                    editorInsertNewlineRaw();
                } else {
                    if (E.cy == E.numrows) editorInsertRow(E.numrows, "", 0);
                    editorRowInsertChar(&E.row[E.cy], E.cx, (unsigned char)replacement[k]);
                    E.cx++;
                }
            }
            count++;
            y = E.cy;
            x = E.cx;
        } else {
            x += mlen;
        }
        /* A zero-length regex match must always consume one original
         * character before the next search. This applies whether the
         * occurrence was replaced, skipped, or replaced with non-empty
         * text; at end-of-row, size+1 makes the non-wrapping finder move
         * to the next row instead of repeatedly growing the same edge. */
        if (mlen == 0) {
            erow *row = &E.row[y];
            if (x < row->size) {
                size_t advance = utf8NextCharLen(row->chars, (size_t)x, (size_t)row->size);
                x += (int32_t)(advance > 0 ? advance : 1);
            } else {
                x = row->size + 1;
            }
        }
    }

    E.search_match_y = -1;
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
    { "Paste (terminal-native, e.g. Cmd+V)", "Bulk insert, no auto-close on pasted text" },
    { NULL, "Selection & clipboard" },
    { "Shift+Arrows, Shift+PageUp/Down", "Extend selection" },
    { "Mouse drag (if enabled, see F2)", "Extend selection" },
    { "Ctrl-T", "Toggle selection mode (works on every terminal)" },
    { "Ctrl-A", "Select all" },
    { "Ctrl-C / Ctrl-X / Ctrl-V", "Copy / cut / paste (system clipboard)" },
    { NULL, "Search" },
    { "Ctrl-F", "Incremental search" },
    { "Ctrl-G (inside search)", "Toggle regex mode (POSIX extended)" },
    { "Ctrl-R (inside search)", "Switch to search & replace" },
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
            for (int32_t i = scroll; i < helpEntryCount && probe_rows < E.screenrows; i++) {
                last_visible = i;
                probe_rows++;
            }
        }

        for (int32_t i = scroll; i < helpEntryCount && rows_used < E.screenrows; i++) {
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
                char line[128];
                int32_t len = snprintf(line, sizeof(line), "%c   %-38s %s",
                    scroll_indicator, helpEntries[i].key, helpEntries[i].desc);
                if (len < 0) len = 0;
                if ((size_t)len >= sizeof(line)) len = (int32_t)sizeof(line) - 1;
                abAppend(&ab, line, len);
                abAppend(&ab, "\x1b[K\r\n", 5);
            }
            rows_used++;
        }

        int32_t total_rows = E.screenrows + 2;
        for (; rows_used < total_rows - 1; rows_used++)
            abAppend(&ab, "\x1b[K\r\n", 5);
        if (rows_used < total_rows)
            abAppend(&ab, "\x1b[K", 3);

        abAppend(&ab, "\x1b[H\x1b[?25h", 9);
        write(STDOUT_FILENO, ab.b, (size_t)ab.len);
        abFree(&ab);

        int32_t c = editorReadKey();
        int32_t max_scroll = helpEntryCount - (E.screenrows - 2);
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
    editorInfoAppendLine(&ab, &rows_used, "    Author    Roberto Bissanti <roberto.bissanti@gmail.com>");
    editorInfoAppendLine(&ab, &rows_used, "    License   MIT (see LICENSE; utf8.c ported from linenoise, BSD 2-Clause)");
    editorInfoAppendLine(&ab, &rows_used, "    Homepage  https://github.com/robertobissanti/tinyedit");
    editorInfoAppendBlank(&ab, &rows_used);

    editorInfoAppendSection(&ab, &rows_used, "Current file");
    if (E.filename) {
        editorInfoAppendLine(&ab, &rows_used, "    Path      %s%s", E.filename, E.dirty ? " (modified)" : "");
    } else {
        editorInfoAppendLine(&ab, &rows_used, "    Path      [No Name]%s", E.dirty ? " (modified)" : "");
    }
    const char *filetype = editorFiletypeLabel();
    editorInfoAppendLine(&ab, &rows_used, "    Filetype  %s", filetype ? filetype : "(unknown)");
    editorInfoAppendLine(&ab, &rows_used, "    Lines     %d", E.numrows);
    editorInfoAppendLine(&ab, &rows_used, "    Chars     %d (UTF-8 grapheme clusters, see F1)", editorCountChars());
    editorInfoAppendLine(&ab, &rows_used, "    Cursor    line %d, column %d", E.cy + 1, E.rx + 1);
    editorInfoAppendLine(&ab, &rows_used, "    Encoding  UTF-8");
    if (S.backup_interval > 0) {
        editorInfoAppendLine(&ab, &rows_used, "    Backup    every %ds while unsaved changes exist (see F2)", S.backup_interval);
    } else {
        editorInfoAppendLine(&ab, &rows_used, "    Backup    off (see F2 to enable crash recovery)");
    }
    editorInfoAppendLine(&ab, &rows_used, "    Undo      %d/%d steps used", E.undo_count, S.undo_max_depth);

    int32_t total_rows = E.screenrows + 2;
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
    int32_t visible = E.screenrows - 3;
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
        const char *note =
            "  Note: Ctrl-Shift-Z may not reach the editor on every "
            "terminal; Ctrl-Y always works as a fallback.\x1b[K\r\n";
        abAppend(ab, note, (int32_t)strlen(note));
        rows_used++;
    }

    char help[96];
    int32_t hlen = snprintf(help, sizeof(help),
        "  %s", (msg && msg[0]) ? msg :
        "Up/Down select, Enter/Space/Left/Right edit, Ctrl-D reset defaults, Ctrl-S/F2 save, Esc cancel");
    abAppend(ab, help, hlen);
    abAppend(ab, "\x1b[K\r\n", 5);
    rows_used++;

    /* Clear every remaining screen row so stale buffer content from
     * the previous editorRefreshScreen() frame doesn't show through
     * underneath the panel. Total rows written (2 header + options +
     * blank/note/help + this padding) must equal the terminal height
     * exactly -- one \r\n too many scrolls the screen and desyncs
     * \x1b[H from the top of the visible viewport on every frame. */
    int32_t total_rows = E.screenrows + 2;
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
    if (S.show_top_bar != previous.show_top_bar)
        winsize_changed = 1;
    if (S.tab_stop != previous.tab_stop ||
        S.show_invisibles != previous.show_invisibles ||
        S.syntax_highlight != previous.syntax_highlight)
        editorUpdateAllRows();
    if (S.mouse_enabled != previous.mouse_enabled) {
        if (S.mouse_enabled) enableMouseReporting();
        else disableMouseReporting();
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

    if (E.cy >= E.numrows) return 0;
    erow *row = &E.row[E.cy];
    if (row->size - E.cx < typed_len) return 0;
    if (memcmp(&row->chars[E.cx], typed, (size_t)typed_len) != 0) return 0;

    E.cx += typed_len;
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
 * the selection as it was BEFORE this keypress cleared E.sel_active (see
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
                E.cy = sel_y1; E.cx = sel_x1;
                for (int32_t k = 0; k < mb->close_len; k++) editorInsertChar((unsigned char)mb->close[k]);
                E.cy = sel_y0; E.cx = sel_x0;
                for (int32_t k = 0; k < mb->open_len; k++) editorInsertChar((unsigned char)mb->open[k]);
                E.cy = sel_y1;
                E.cx = sel_x1 + (sel_y1 == sel_y0 ? mb->open_len + mb->close_len : mb->open_len);
                return;
            }

            for (int32_t k = 0; k < seq_len; k++) editorInsertChar((unsigned char)seq[k]);
            for (int32_t k = 0; k < mb->close_len; k++) editorInsertChar((unsigned char)mb->close[k]);
            E.cx -= mb->close_len;
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
        E.cy = sel_y1; E.cx = sel_x1;
        editorInsertChar((unsigned char)pair->close);
        E.cy = sel_y0; E.cx = sel_x0;
        editorInsertChar((unsigned char)pair->open);
        E.cy = sel_y1; E.cx = sel_x1 + (sel_y1 == sel_y0 ? 2 : 1);
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
        if (c == '$' && E.cx >= 2 && E.cy < E.numrows) {
            erow *row = &E.row[E.cy];
            uint8_t nothing_to_skip = E.cx >= row->size || row->chars[E.cx] != '$';
            if (nothing_to_skip && row->chars[E.cx - 1] == '$' && row->chars[E.cx - 2] == '$') {
                editorInsertChar('$');
                editorInsertChar('$');
                E.cx -= 2;
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
        if (c != '`' && E.cy < E.numrows) {
            erow *row = &E.row[E.cy];
            if (E.cx < row->size && row->chars[E.cx] == pair->close) {
                E.cx++;
                return;
            }
        }
        editorInsertChar(c);
        editorInsertChar((unsigned char)pair->close);
        E.cx--;
        return;
    }

    if (pair) {
        /* Asymmetric open (open != close): always inserts both and
         * places the cursor in between -- typing the OPEN character
         * never skips, only typing the matching CLOSE character
         * (handled by the branch below) does. */
        editorInsertChar(c);
        editorInsertChar((unsigned char)pair->close);
        E.cx--;
        return;
    }

    /* Not an opener -- check whether it's the closer of an asymmetric
     * pair, for skip-over (e.g. typing ')' right before an
     * auto-inserted ')'). */
    for (int32_t i = 0; i < autoCloseTableCount; i++) {
        if (autoCloseTable[i].close == c && autoCloseTable[i].open != autoCloseTable[i].close) {
            if (E.cy < E.numrows) {
                erow *row = &E.row[E.cy];
                if (E.cx < row->size && row->chars[E.cx] == c) {
                    E.cx++;
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

    /* Captured before the selection-clearing block below runs, so the
     * auto-close wrap-selection path (see editorInsertCharAutoClose())
     * still knows what was selected for a plain printable keypress,
     * which clears E.sel_active like any other non-whitelisted key. */
    int32_t had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1;
    uint8_t had_sel = editorGetSelection(&had_sel_y0, &had_sel_x0, &had_sel_y1, &had_sel_x1);

    uint8_t is_plain_arrow = (c == ARROW_UP || c == ARROW_DOWN ||
        c == ARROW_LEFT || c == ARROW_RIGHT || c == PAGE_UP || c == PAGE_DOWN);

    /* Tab/Shift+Tab keep the selection because with one active they
     * mean "indent/outdent these lines" (see editorIndentSelection())
     * and are meant to be repeatable -- without a selection Tab falls
     * through to inserting one indent at the cursor as usual. */
    uint8_t is_indent_key = (c == '\t' || c == SHIFT_TAB);

    if (c != SHIFT_ARROW_UP && c != SHIFT_ARROW_DOWN &&
        c != SHIFT_ARROW_LEFT && c != SHIFT_ARROW_RIGHT &&
        c != SHIFT_PAGE_UP && c != SHIFT_PAGE_DOWN &&
        c != CTRL_KEY('a') && c != CTRL_KEY('c') &&
        c != CTRL_KEY('x') && c != CTRL_KEY('v') &&
        c != CTRL_KEY('t') && c != MOUSE_EVENT_KEY &&
        c != CTRL_KEY('s') &&
        !(had_sel && is_indent_key) &&
        !(E.sel_pinned && is_plain_arrow))
        E.sel_active = 0;

    switch (c) {
        case '\r':
            editorInsertNewlineAutoIndent();
            break;

        case '\t':
            if (had_sel) {
                editorIndentSelection(0);
            } else if (S.insert_spaces_for_tab) {
                for (int32_t i = 0; i < S.tab_stop; i++) editorInsertChar(' ');
            } else {
                editorInsertChar('\t');
            }
            break;

        /* With no selection this outdents the current line, which is
         * what Shift+Tab does everywhere else -- the cursor doesn't
         * have to be in the indentation for "this line is indented one
         * level too far" to be the obvious intent. */
        case SHIFT_TAB:
            if (had_sel) {
                editorIndentSelection(1);
            } else if (E.cy < E.numrows) {
                editorPushUndo(EDIT_OTHER);
                int32_t removed = editorRowOutdent(&E.row[E.cy]);
                E.cx -= removed;
                if (E.cx < 0) E.cx = 0;
                if (removed) E.dirty = 1;
            }
            break;

        case CTRL_KEY('q'):
            editorQuit();
            return;

        case CTRL_KEY('w'):
            editorCloseFile();
            break;

        case CTRL_KEY('o'):
            editorOpenFile();
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case F4_KEY:
        case SAVE_AS_KEY:
            editorSaveAs();
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
            int32_t sy, sx, ey, ex;
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
            int32_t sy, sx, ey, ex;
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

        /* Bracketed paste (terminal-native paste, e.g. Cmd+V into the
         * terminal window rather than through this editor's own
         * Ctrl-V/system-clipboard path above): editorReadKey() reports
         * PASTE_START_KEY the instant it sees the ESC[200~ marker, then
         * this reads the entire pasted block in one go via
         * editorReadPastedText() and inserts it with editorInsertText()
         * -- same bulk insert path Ctrl-V already uses, so a paste that
         * arrives this way is both fast (one undo-snapshot/no
         * per-character redraw, vs. an editorProcessKeypress() call per
         * byte the old byte-by-byte path required) and correct
         * (bypasses editorInsertCharAutoClose() entirely, so pasted
         * '(', '\'', '`', etc. don't each trigger auto-close as if
         * freshly typed -- see TODO.md for the bug this fixes: spurious
         * closing characters left behind after a paste). */
        case PASTE_START_KEY: {
            int32_t sy, sx, ey, ex;
            if (editorGetSelection(&sy, &sx, &ey, &ex)) {
                editorDeleteRange(sy, sx, ey, ex);
                E.sel_active = 0;
            }
            size_t len;
            char *text = editorReadPastedText(&len);
            editorInsertText(text, len);
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
             * another one is already queued (see stdinHasDataReady())
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
                     * E.cy/E.cx or the selection -- the cursor and
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
                    int32_t limit = wrapcols > 0 ? editorTotalVideoRows(wrapcols) : E.numrows;
                    E.rowoff += delta;
                    if (E.rowoff < 0) E.rowoff = 0;
                    if (E.rowoff > limit) E.rowoff = limit;
                    /* editorScroll() (called every redraw) normally
                     * forces E.rowoff back into the window
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
                     * E.free_scroll's declaration in tinyedit.h), so
                     * scrolling with the wheel and then, say, pressing
                     * an arrow key immediately goes back to normal
                     * "view follows cursor" behavior. */
                    E.free_scroll = 1;
                } else {
                    uint8_t in_text_area = mouseEventRow >= 1 + (S.show_top_bar ? 1 : 0) &&
                        mouseEventRow <= 1 + (S.show_top_bar ? 1 : 0) + E.screenrows - 1 &&
                        mouseEventCol > editorGutterWidth();

                    if (in_text_area && mouseEventButton == 0 && mouseEventPress) {
                        /* Fresh press: position the cursor there.
                         * E.sel_active is deliberately left OFF here
                         * (not set to 1 with anchor==cursor) -- the
                         * anchor is only remembered locally
                         * (press_anchor_y/x below) and E.sel_active is
                         * turned on only once an actual drag moves the
                         * cursor away from it (see the drag branch).
                         *
                         * A collapsed anchor==cursor selection LOOKS
                         * invisible right after the click (start==end,
                         * nothing to highlight), but editorGetSelection()
                         * re-evaluates the anchor against the CURRENT
                         * cursor position on every call, not a
                         * snapshot -- so if E.sel_active stayed 1 here
                         * and E.cy/E.cx moved for any OTHER reason
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
                        E.cy = cy;
                        E.cx = cx;
                        E.sel_active = 0;
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
                         * E.sel_anchor_x/y like any other selection
                         * (Shift+Arrow, Ctrl-A, ...) -- press_anchor_*
                         * only matters for this initial arming. */
                        if (!E.sel_active) {
                            E.sel_active = 1;
                            E.sel_anchor_x = press_anchor_x;
                            E.sel_anchor_y = press_anchor_y;
                        }
                        int32_t cy, cx;
                        editorMouseToCursor(mouseEventCol, mouseEventRow, &cy, &cx);
                        E.cy = cy;
                        E.cx = cx;
                    } else if (!mouseEventPress) {
                        /* Release: stop tracking drag motion. A click
                         * with no drag in between leaves
                         * sel_anchor_x/y == E.cx/E.cy, which
                         * editorGetSelection() already treats as "no
                         * selection" -- no special-casing needed here
                         * for "was this a click or a drag". */
                        dragging = 0;
                    }
                }

                more = 0;
                if (stdinHasDataReady()) {
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

        case F3_KEY:
            editorInfoScreen();
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
        case END_KEY: {
            int32_t hw_wrapcols = editorSoftWrapCols();
            if (hw_wrapcols > 0 && S.home_end_visual_line && E.cy < E.numrows) {
                int32_t seg_idx, seg_col;
                editorRxToSegment(&E.row[E.cy], hw_wrapcols, E.rx, &seg_idx, &seg_col);
                erow *row = &E.row[E.cy];
                int32_t nseg = editorRowSegments(row, hw_wrapcols);
                if (c == HOME_KEY) {
                    E.cx = editorSegColToCx(row, row->seg_start_rx[seg_idx], 0);
                } else {
                    int32_t seg_to_rx = editorSegVisibleEndRx(row, nseg, row->seg_start, row->seg_start_rx, seg_idx);
                    E.cx = editorSegColToCx(row, 0, seg_to_rx);
                }
            } else if (c == HOME_KEY) {
                E.cx = 0;
            } else if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;
        }

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            /* With a selection, both keys delete the whole range rather
             * than one character, matching Ctrl-V/paste (which already
             * replaced the selection) and every other editor. had_sel is
             * the copy captured before the selection-clearing block above,
             * since neither key is whitelisted there. */
            if (had_sel) {
                editorDeleteRange(had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1);
            } else {
                if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
                editorDelChar();
            }
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        case SHIFT_PAGE_UP:
        case SHIFT_PAGE_DOWN: {
            uint8_t is_up = (c == PAGE_UP || c == SHIFT_PAGE_UP);
            uint8_t extending = (c == SHIFT_PAGE_UP || c == SHIFT_PAGE_DOWN || E.sel_pinned);

            if (extending && !E.sel_active) {
                E.sel_active = 1;
                E.sel_anchor_x = E.cx;
                E.sel_anchor_y = E.cy;
            }

            int32_t pu_wrapcols = editorSoftWrapCols();
            if (pu_wrapcols > 0) {
                /* E.rowoff is a video-row index in wrapped mode (see
                 * editorScroll()), not a file-row index -- resolve it
                 * back to a (filerow, segment) before jumping there. */
                int32_t top_filerow, top_seg;
                int32_t target_vy = is_up ? E.rowoff : E.rowoff + E.screenrows - 1;
                int32_t total = editorTotalVideoRows(pu_wrapcols);
                if (target_vy >= total) target_vy = total > 0 ? total - 1 : 0;
                editorFileRowAtVideoRow(target_vy, pu_wrapcols, &top_filerow, &top_seg);
                E.cy = top_filerow;
                erow *row = &E.row[E.cy];
                int32_t nseg = editorRowSegments(row, pu_wrapcols);
                if (top_seg >= nseg) top_seg = nseg - 1;
                E.cx = editorSegColToCx(row, row->seg_start_rx[top_seg], 0);
            } else if (is_up) {
                E.cy = E.rowoff;
            } else {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }
            int32_t times = E.screenrows;
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
            uint8_t extending = (c == SHIFT_ARROW_UP || c == SHIFT_ARROW_DOWN ||
                c == SHIFT_ARROW_LEFT || c == SHIFT_ARROW_RIGHT || E.sel_pinned);

            if (extending && !E.sel_active) {
                E.sel_active = 1;
                E.sel_anchor_x = E.cx;
                E.sel_anchor_y = E.cy;
            }

            int32_t plain = (c == SHIFT_ARROW_UP || c == ARROW_UP) ? ARROW_UP :
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
            editorInsertCharAutoClose(c, had_sel, had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1);
            break;
    }
}

/* ---- init ------------------------------------------------------------------- */

static void editorFreeUndoRedo(void) {
    for (int32_t i = 0; i < E.undo_count; i++) editorFreeSnapshot(&E.undo_stack[i]);
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
    E.free_scroll = 0;
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
    E.last_backup_time = 0;

    settingsLoad(&S);

    E.undo_stack = NULL;
    E.undo_count = 0;
    E.redo_stack = NULL;
    E.redo_count = 0;
    E.last_edit_type = EDIT_NONE;
    E.last_edit_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2; /* status bar + message bar */
    if (S.show_top_bar) E.screenrows -= 1;
}

int main(int argc, char **argv) {
    enableRawMode();
    enableBracketedPaste();
    enableResizeHandling();
    initEditor();
    if (S.mouse_enabled) enableMouseReporting();
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
            "Terminal.app: Ctrl-T select | Ctrl-O open | Ctrl-Q quit | F1 help");
    } else {
        editorSetStatusMessageSticky("Ctrl-S save | Ctrl-O open | Ctrl-Q quit | F1 help");
    }

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
