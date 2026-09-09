/* terminal.c -- POSIX terminal mode, key decoding, mouse and paste input. */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "terminal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;
volatile sig_atomic_t winsize_changed = 0;
int32_t mouseEventButton;
int32_t mouseEventCol, mouseEventRow;
uint8_t mouseEventPress;
int32_t pending_key = -1;


void terminalDie(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void terminalDisableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
        terminalDie("tcsetattr");
}

/* SGR background is terminal state, but an erase fills cells using the
 * CURRENT background. Reset first, then clear, so the shell prompt is drawn
 * on the terminal's real default background rather than on tinyedit's old
 * painted cells. DECSCUSR 0 restores the terminal's cursor default. */
void terminalRestoreVisualState(void) {
    write(STDOUT_FILENO, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\x1b[0 q", 22);
}

void terminalEnableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) terminalDie("tcgetattr");
    atexit(terminalDisableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) terminalDie("tcsetattr");
}

/* Bracketed paste mode (\x1b[?2004h/l, a widely-supported terminal
 * extension, not a POSIX/termios setting -- toggled via an escape
 * sequence written to the terminal, unlike raw mode above which is a
 * termios attribute): once enabled, the terminal wraps any pasted text
 * in ESC[200~ ... ESC[201~ markers instead of just feeding it to stdin
 * as if it had been typed. editorReadKey() watches for the start
 * marker (see PASTE_START_KEY) and editorProcessKeypress() then reads
 * the whole block in one shot via terminalReadPastedText() -- turning an
 * O(paste length) sequence of individual keystroke-processing calls
 * (slow: one undo-snapshot/full redraw per character) each of which
 * ALSO ran through auto-close-pair logic as if the user had typed
 * every character (wrong: spurious closing brackets/quotes left behind
 * for every '(', '\'', '`', etc. in the pasted text) into a single
 * bulk insert. Disabled on exit like raw mode -- leaving it on would
 * change how paste behaves in whatever the user's shell/next program
 * is after tinyedit quits. */
void terminalDisableBracketedPaste(void) {
    write(STDOUT_FILENO, "\x1b[?2004l", 8);
}

void terminalEnableBracketedPaste(void) {
    atexit(terminalDisableBracketedPaste);
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
uint8_t terminalInputReady(void) {
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
void terminalDisableMouseReporting(void) {
    write(STDOUT_FILENO, "\x1b[?1002l\x1b[?1006l", 16);
}

void terminalEnableMouseReporting(void) {
    atexit(terminalDisableMouseReporting);
    write(STDOUT_FILENO, "\x1b[?1002h\x1b[?1006h", 16);
}

static void handleWinch(int sig) {
    (void)sig;
    winsize_changed = 1;
}

void terminalEnableResizeHandling(void) {
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

int32_t terminalReadKey(uint8_t mac_command_keys) {
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
        if (nread == -1 && errno != EAGAIN && errno != EINTR) terminalDie("read");
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
                     * bulk-paste mode via terminalReadPastedText(), which
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
                     * codepoint 115 ('s') to recognize Ctrl-Shift-S.
                     * With mac_command_keys enabled, a separate,
                     * documented Ghostty bridge also accepts modifier 9
                     * (Super/Command) for S/F/Z/O/W. That bridge is
                     * opt-in because Command is normally consumed by
                     * macOS or the terminal emulator. */
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
                    if (ok && term && field_idx == 1 &&
                        fields[1] == 9 && mac_command_keys) {
                        switch (fields[0]) {
                            case 115: return CTRL_KEY('s'); /* Cmd-S */
                            case 102: return CTRL_KEY('f'); /* Cmd-F */
                            case 122: return CTRL_KEY('z'); /* Cmd-Z */
                            case 111: return CTRL_KEY('o'); /* Cmd-O */
                            case 119: return CTRL_KEY('w'); /* Cmd-W */
                            case 99:  return CTRL_KEY('c'); /* Cmd-C */
                            case 120: return CTRL_KEY('x'); /* Cmd-X */
                            case 97:  return CTRL_KEY('a'); /* Cmd-A */
                            case 113: return CTRL_KEY('q'); /* Cmd-Q */
                            case 103: return CTRL_KEY('g'); /* Cmd-G */
                            case 114: return CTRL_KEY('r'); /* Cmd-R */
                            case 116: return CTRL_KEY('t'); /* Cmd-T */
                            case 121: return CTRL_KEY('y'); /* Cmd-Y */
                            case 100: return CTRL_KEY('d'); /* Cmd-D */
                        }
                    } else if (ok && term && field_idx == 1 && fields[0] == 115) {
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
                    /* Modifier codes are a bitmask + 1: 5 = Ctrl,
                     * 6 = Ctrl+Shift. */
                    uint8_t is_ctrl = (mod == '5');
                    uint8_t is_ctrl_shift = (mod == '6');

                    if (seq[1] == '5' && term == '~') {
                        if (is_ctrl_shift) return SHIFT_DOC_HOME;
                        if (is_ctrl) return DOC_HOME;
                        return is_shift ? SHIFT_PAGE_UP : PAGE_UP;
                    }
                    if (seq[1] == '6' && term == '~') {
                        if (is_ctrl_shift) return SHIFT_DOC_END;
                        if (is_ctrl) return DOC_END;
                        return is_shift ? SHIFT_PAGE_DOWN : PAGE_DOWN;
                    }

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
                        case 'H':
                            if (is_ctrl_shift) return SHIFT_DOC_HOME;
                            if (is_ctrl) return DOC_HOME;
                            return is_shift ? SHIFT_HOME : HOME_KEY;
                        case 'F':
                            if (is_ctrl_shift) return SHIFT_DOC_END;
                            if (is_ctrl) return DOC_END;
                            return is_shift ? SHIFT_END : END_KEY;
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
char *terminalReadPastedText(size_t *outlen) {
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

int32_t terminalGetWindowSize(int32_t *rows, int32_t *cols) {
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

