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
 * Run:    bin/tinyedit [filename]
 *
 * Module map (keep this list in sync whenever a source module is added,
 * removed, or given a materially different responsibility):
 *   alloc.c        checked size arithmetic and fatal/recoverable allocations
 *   backup.c       crash-recovery snapshots
 *   buffer.c       document rows and text-buffer mutations
 *   clipboard.c    system clipboard integration and local fallback
 *   command.c      shared command metadata and setting-toggle links
 *   editor_state.c selection ranges and shared ASCII auto-close policy
 *   fileio.c       home-path expansion, staged loading and atomic replacement
 *   history.c      bounded row deltas, undo/redo and allocation-failure rollback
 *   linenoise.c    historical line-editor implementation; not built
 *   menu.c         UTF-8 menu layout, viewport clipping and input navigation
 *   render.c       shared layout calculations for rows and wrapped text
 *   search.c       compiled queries, text search and source-coordinate results
 *   settings.c     persistent configuration parsing and serialization
 *   syntax.c       filetype detection and syntax highlighting
 *   terminal.c     raw terminal setup, input decoding, and terminal I/O
 *   tinyedit.c     application flow, editor commands, screens, and drawing
 *   utf8.c         UTF-8 decoding, navigation, and display-width helpers
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "tinyedit.h"
#include "alloc.h"
#include "backup.h"
#include "buffer.h"
#include "editor_state.h"
#include "fileio.h"
#include "history.h"
#include "render.h"
#include "clipboard.h"
#include "command.h"
#include "syntax.h"
#include "menu.h"
#include "terminal.h"
#include "utf8.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
static struct editorMenu M;
static struct editorEditBatch edit_batch = {0, -1, 0};
static struct editorCursor edit_saved_cursor;
static struct editorSelection edit_saved_selection;
static uint8_t edit_saved_dirty, edit_saved_final_newline;
static enum historyError edit_last_error;
static uint8_t replay_preparing;
static uint8_t menu_mouse_motion_enabled;
static uint8_t drawing_heading;
static uint8_t drawing_heading_bold;

static const char *const void_tags[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
    "meta", "param", "source", "track", "wbr"
};

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

/* NULL marks blank spacer lines in the centered splash. */
static const char *splashLines[] = {
    "tinyedit",
    NULL, /* session slogan, assigned at startup */
    NULL,
    "version " TE_VERSION,
    "by Roberto Bissanti",
    "MIT licensed -- free to use and redistribute",
    NULL,
    /* These four lines share one left edge for their two key columns. */
    "Ctrl-O  open a file        Ctrl-S  save    ",
    "Ctrl-F  find               F4      save as ",
    "Ctrl-Z  undo               F2      settings",
    "Ctrl-Q  quit               F1      help    ",
    NULL,
    "Start typing, or press F3 for details",
};
static const int32_t splashLineCount =
    (int32_t)(sizeof(splashLines) / sizeof(splashLines[0]));

static const struct autoCloseMultiByte autoCloseMultiByteTable[] = {
    { "\xc2\xab", 2, "\xc2\xbb", 2 },             /* « » */
    { "\xe2\x80\x9c", 3, "\xe2\x80\x9d", 3 },     /* “ ” */
    { "\xe2\x80\x98", 3, "\xe2\x80\x99", 3 },     /* ‘ ’ */
};
static const int32_t autoCloseMultiByteCount =
    (int32_t)(sizeof(autoCloseMultiByteTable) / sizeof(autoCloseMultiByteTable[0]));

/* One entry per F1 line; NULL marks a section header. */
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
    { "> in XML/HTML", "Insert matching end tag (except HTML void elements)" },
    { "Backspace / Delete", "Delete character (UTF-8 aware)" },
    { "Ctrl-Z / Ctrl-Y", "Undo / redo" },
#ifdef __APPLE__
    { "Cmd-S/F/Z/O/W/C/X/A/Q/G/R (Ghostty, experimental)", "Save/find/undo/open/close/copy/cut/select all/quit/regex/replace; see README" },
#endif
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

/**
 * @brief Choose the modifier name shown in shortcut hints.
 *
 * @details Use for UI text; the return value is a borrowed literal, selected
 * from the live settings.
 */
static const char *editorPrimaryModifier(void) {
#ifdef __APPLE__
    return S.mac_command_keys ? "Cmd" : "Ctrl";
#else
    return "Ctrl";
#endif
}

/**
 * @brief Adapt shortcut text to the active Ctrl or Command mode.
 *
 * @details Write into dst with its byte capacity dstsize; output is NUL-terminated when capacity is nonzero and may be truncated.
 */
static void editorShortcutText(char *dst, size_t dstsize, const char *src) {
    size_t used = 0;
    if (dstsize == 0) return;
    while (*src && used + 1 < dstsize) {
#ifdef __APPLE__
        if (S.mac_command_keys && strncmp(src, "Ctrl", 4) == 0) {
            if (used + 3 >= dstsize) break;
            memcpy(dst + used, "Cmd", 3);
            used += 3;
            src += 4;
        } else
#endif
        {
            dst[used++] = *src++;
        }
    }
    dst[used] = '\0';
}

/* ---- terminal adapter -------------------------------------------------- */
/**
 * @brief Read one input event using the live keyboard settings.
 *
 * @return a raw byte or an editorKey value from terminalReadKey().
 */
static int32_t editorReadKey(void) {
    return terminalReadKey(
#ifdef __APPLE__
        (uint8_t)S.mac_command_keys
#endif
    );
}


/**
 * @brief Translate a source-byte cursor offset into a display column.
 *
 * @details Pass a valid character boundary in row and use the result for
 * layout, with the current tab width.
 */
static int32_t editorRowCxToRx(erow *row, int32_t cx) {
    return bufferRowCxToRx(row, cx, S.tab_stop);
}

/**
 * @brief Find the source-byte position corresponding to a display column.
 *
 * @details A column inside a tab or wide glyph maps to its start; columns past
 * the text map to the row end.
 */
static int32_t editorRowRxToCx(erow *row, int32_t target_rx) {
    return bufferRowRxToCx(row, target_rx, S.tab_stop);
}

static void editorInvalidateDocumentCaches(void) {
    searchInvalidateText(&E.search.query);
    E.document.display_cache.chars_valid = 0;
    E.document.display_cache.pair_valid = 0;
}

/**
 * @brief Locate the bracket at the cursor and its nested matching partner.
 *
 * @details Scans text, including strings and comments, without a syntax
 * parser.
 * @return 1 and fills zero-based row and source-byte outputs only on success.
 *
 * @note Finds the delimiter under the cursor, or the one immediately to its
 * left, and its matching ASCII bracket. Matching deliberately operates on raw
 * text rather than auto-close history: pasted and pre-existing pairs behave
 * the same way. UTF-8 is stepped as complete characters while scanning, so a
 * cursor or match never lands inside a multibyte sequence.
 */
static uint8_t editorFindMatchingPair(int32_t *anchor_y, int32_t *anchor_x,
    int32_t *match_y, int32_t *match_x) {
    int32_t y = E.document.cursor.cy;
    int32_t x = E.document.cursor.cx;
    if (y < 0 || y >= E.document.buffer.row_count) return 0;

    erow *row = &E.document.buffer.rows[y];
    int32_t at = -1;
    if (x < row->size && strchr("()[]{}", row->chars[x])) at = x;
    else if (x > 0) {
        size_t step = utf8PrevCharLen(row->chars, (size_t)x);
        int32_t prev = x - (int32_t)(step ? step : 1);
        if (strchr("()[]{}", row->chars[prev])) at = prev;
    }
    if (at < 0) return 0;

    char bracket = row->chars[at];
    char open, close;
    uint8_t forward;
    switch (bracket) {
        case '(': open = '('; close = ')'; forward = 1; break;
        case '[': open = '['; close = ']'; forward = 1; break;
        case '{': open = '{'; close = '}'; forward = 1; break;
        case ')': open = '('; close = ')'; forward = 0; break;
        case ']': open = '['; close = ']'; forward = 0; break;
        case '}': open = '{'; close = '}'; forward = 0; break;
        default: return 0;
    }

    int32_t depth = 1;
    if (forward) {
        for (int32_t scan_y = y; scan_y < E.document.buffer.row_count; scan_y++) {
            erow *scan_row = &E.document.buffer.rows[scan_y];
            int32_t scan_x = scan_y == y ? at + 1 : 0;
            while (scan_x < scan_row->size) {
                char byte = scan_row->chars[scan_x];
                if (byte == open) depth++;
                else if (byte == close && --depth == 0) {
                    *anchor_y = y; *anchor_x = at;
                    *match_y = scan_y; *match_x = scan_x;
                    return 1;
                }
                size_t step = utf8NextCharLen(scan_row->chars, (size_t)scan_x,
                    (size_t)scan_row->size);
                scan_x += (int32_t)(step ? step : 1);
            }
        }
    } else {
        for (int32_t scan_y = y; scan_y >= 0; scan_y--) {
            erow *scan_row = &E.document.buffer.rows[scan_y];
            int32_t scan_x = scan_y == y ? at : scan_row->size;
            while (scan_x > 0) {
                size_t step = utf8PrevCharLen(scan_row->chars, (size_t)scan_x);
                scan_x -= (int32_t)(step ? step : 1);
                char byte = scan_row->chars[scan_x];
                if (byte == close) depth++;
                else if (byte == open && --depth == 0) {
                    *anchor_y = y; *anchor_x = at;
                    *match_y = scan_y; *match_x = scan_x;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static uint8_t editorMatchingPairAtCursor(int32_t *anchor_y, int32_t *anchor_x,
    int32_t *match_y, int32_t *match_x) {
    struct editorDisplayCache *cache = &E.document.display_cache;
    if (!cache->pair_valid || cache->cursor_y != E.document.cursor.cy ||
            cache->cursor_x != E.document.cursor.cx) {
        cache->has_pair = editorFindMatchingPair(&cache->anchor_y, &cache->anchor_x,
            &cache->match_y, &cache->match_x);
        cache->cursor_y = E.document.cursor.cy;
        cache->cursor_x = E.document.cursor.cx;
        cache->pair_valid = 1;
    }
    if (!cache->has_pair) return 0;
    *anchor_y = cache->anchor_y;
    *anchor_x = cache->anchor_x;
    *match_y = cache->match_y;
    *match_x = cache->match_x;
    return 1;
}

static void editorBeginEdit(void) {
    if (edit_batch.depth++ == 0) {
        if (!E.document.history.pending) E.document.history.error = HISTORY_OK;
        edit_batch.first_row = -1;
        edit_batch.undo_recorded = 0;
        edit_saved_cursor = E.document.cursor;
        edit_saved_selection = E.document.selection;
        edit_saved_dirty = E.document.file.dirty;
        edit_saved_final_newline = E.document.file.final_newline;
        edit_last_error = HISTORY_OK;
    }
}

static void editorTouchRowsFrom(int32_t from) {
    if (edit_batch.first_row < 0 || from < edit_batch.first_row)
        edit_batch.first_row = from;
}

/**
 * @brief Propagate syntax changes through the rows that depend on them.
 *
 * @details Pass a valid starting row; force retokenizes all following rows.
 * Otherwise stop once comment, math, front matter and emphasis states settle.
 */
static void editorRehighlightFrom(int32_t from, uint8_t force) {
    if (edit_batch.depth) {
        editorTouchRowsFrom(from);
        return;
    }
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

/**
 * @brief Rebuild a changed row's display text and invalidate wrapping.
 *
 * @details Source mutations must be complete. Syntax propagation is separate
 * so compound commands never tokenize intermediate or partial UTF-8 text.
 */
/* Existing nontransactional display refreshes keep the controlled-exit
 * contract; edit and replay preparations propagate failure to their caller. */
static uint8_t editorRenderFailure(enum historyError error) {
    if (!E.document.history.pending && !replay_preparing) {
        errno = ENOMEM;
        terminalDie("render allocation");
    }
    E.document.history.error = error;
    return 0;
}

static uint8_t editorRebuildRow(erow *row) {
    size_t tabs = 0;
    for (size_t pos = 0; pos < (size_t)row->size; ) {
        if (row->chars[pos] == '\t') tabs++;
        pos += row->chars[pos] == '\t' ? 1 :
            utf8NextCharLen(row->chars, pos, (size_t)row->size);
    }

    size_t padding = (size_t)(S.tab_stop - 1);
    if (padding != 0 && tabs > (SIZE_MAX - (size_t)row->size - 1) / padding) {
        return editorRenderFailure(HISTORY_SIZE);
    }
    size_t capacity = (size_t)row->size + tabs * padding + 1;
    if (capacity - 1 > INT32_MAX) {
        return editorRenderFailure(HISTORY_SIZE);
    }
    char *render = teTryMalloc(capacity);
    if (!render) return editorRenderFailure(HISTORY_MEMORY);
    free(row->render);
    free(row->seg_start);
    free(row->seg_start_rx);
    row->seg_start = NULL;
    row->seg_start_rx = NULL;
    row->seg_count = 0;
    row->seg_wrapcols = -1;
    row->render = render;

    int32_t idx = 0, rx = 0;
    row->grapheme_count = 0;
    for (size_t pos = 0; pos < (size_t)row->size; ) {
        size_t len = row->chars[pos] == '\t' ? 1 :
            utf8NextCharLen(row->chars, pos, (size_t)row->size);
        if (row->chars[pos] == '\t') {
            int32_t width = S.tab_stop - rx % S.tab_stop;
            row->render[idx++] = S.show_invisibles ? INVISIBLE_TAB_GLYPH : ' ';
            for (int32_t fill = 1; fill < width; fill++) row->render[idx++] = ' ';
            rx += width;
        } else {
            memcpy(row->render + idx, row->chars + pos, len);
            if (row->chars[pos] == ' ' && S.show_invisibles)
                row->render[idx] = INVISIBLE_SPACE_GLYPH;
            idx += (int32_t)len;
            rx += utf8SingleCharWidth(row->chars + pos, len);
        }
        pos += len;
        row->grapheme_count++;
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    row->render_dirty = 0;
    return 1;
}

static void editorUpdateRow(erow *row) {
    if (historyFailed(&E.document)) return;
    editorInvalidateDocumentCaches();
    int32_t index = (int32_t)(row - E.document.buffer.rows);
    if (edit_batch.depth) {
        row->render_dirty = 1;
        editorTouchRowsFrom(index);
        return;
    }
    if (editorRebuildRow(row)) editorRehighlightFrom(index, 0);
}

static void editorSetStatusMessage(const char *fmt, ...);

static void editorEndEdit(void) {
    if (--edit_batch.depth != 0) return;
    int32_t first = edit_batch.first_row;
    int32_t last = first;
    if (!historyFailed(&E.document) && first >= 0) {
        for (int32_t i = first; i < E.document.buffer.row_count; i++) {
            if (!E.document.buffer.rows[i].render_dirty) continue;
            if (!editorRebuildRow(&E.document.buffer.rows[i])) break;
            last = i;
        }
    }
    uint8_t recorded = E.document.history.pending != NULL;
    edit_last_error = historyFinishEdit(&E.document);
    if (edit_last_error != HISTORY_OK) {
        if (!recorded || !E.document.history.hold) {
            E.document.cursor = edit_saved_cursor;
            E.document.selection = edit_saved_selection;
            E.document.file.dirty = edit_saved_dirty;
            E.document.file.final_newline = edit_saved_final_newline;
        }
        editorInvalidateDocumentCaches();
        editorSetStatusMessage(edit_last_error == HISTORY_LIMIT ?
            "Undo memory limit: edit cancelled" : "Not enough memory: edit cancelled");
        E.document.history.error = HISTORY_OK;
        editorRehighlightFrom(0, 1);
    } else if (first >= 0) {
        for (int32_t i = first; i <= last && i < E.document.buffer.row_count; i++) {
            erow *row = &E.document.buffer.rows[i];
            erow *previous = i > 0 ? &E.document.buffer.rows[i - 1] : NULL;
            syntaxHighlightRow(row, E.document.file.filename, (uint8_t)S.syntax_highlight,
                previous ? previous->hl_open_comment : 0,
                previous ? previous->hl_open_math : 0,
                previous ? previous->hl_open_frontmatter : 0, i,
                previous ? previous->hl_open_emphasis : 0);
        }
        if (last + 1 < E.document.buffer.row_count) editorRehighlightFrom(last + 1, 0);
        if (E.document.history.dropped_count)
            editorSetStatusMessage("Undo limit: %d oldest action(s) removed",
                E.document.history.dropped_count);
    }
    edit_batch.first_row = -1;
}

/**
 * @brief Refresh cached display text after a change to global rendering settings.
 *
 * @details Use when tabs, invisible characters or highlighting change; source
 * text and undo history remain intact.
 *
 * @note Recomputes row->render for every row -- needed whenever a setting that
 * editorUpdateRow() reads (tab_stop, show_invisibles) changes after rows
 * already exist, since editorUpdateRow() is otherwise only called on the
 * specific row(s) an edit touches. Without this, rows untouched since a
 * settings change keep rendering with the old tab_stop/invisibles state until
 * the user happens to edit them -- likely a preexisting gap for tab_stop
 * alone, closed here as a side effect of also needing it for show_invisibles.
 */
static void editorUpdateAllRows(void) {
    editorInvalidateDocumentCaches();
    editorBeginEdit();
    for (int32_t i = 0; i < E.document.buffer.row_count; i++) editorUpdateRow(&E.document.buffer.rows[i]);
    editorEndEdit();
}

/**
 * @brief Insert a logical row and prepare it for display.
 *
 * @details at is a zero-based insertion index and s contains len bytes without
 * a line ending. Marks the document dirty; the caller owns undo recording.
 */
static void editorInsertRow(int32_t at, const char *s, size_t len) {
    if (at < 0 || at > E.document.buffer.row_count) return;
    bufferInsertRow(&E.document.buffer, at, s, len);
    if (historyFailed(&E.document)) return;
    editorUpdateRow(&E.document.buffer.rows[at]);
    E.document.file.dirty = 1;
}

/**
 * @brief Remove a logical row and update dependent highlighting.
 *
 * @details Invalid indices are ignored. Marks the document dirty without
 * creating an undo snapshot.
 */
static void editorDelRow(int32_t at) {
    if (at < 0 || at >= E.document.buffer.row_count) return;
    editorInvalidateDocumentCaches();
    bufferDeleteRow(&E.document.buffer, at);
    if (at < E.document.buffer.row_count) editorRehighlightFrom(at, 0);
    E.document.file.dirty = 1;
}

/**
 * @brief Insert one raw byte and refresh the edited row.
 *
 * @details at is a source-byte offset, c is a byte rather than a Unicode code
 * point. Marks dirty but leaves undo grouping to the caller.
 */
static void editorRowInsertChar(erow *row, int32_t at, uint8_t c) {
    bufferRowInsertByte(row, at, c);
    editorUpdateRow(row);
    E.document.file.dirty = 1;
}

/**
 * @brief Insert a byte span into a row and refresh its display caches.
 *
 * @details text supplies len bytes and must remain valid during insertion. No
 * line splitting or new undo boundary is performed.
 */
static void editorRowInsertString(erow *row, int32_t at, const char *text, size_t len) {
    if (len == 0) return;
    bufferRowInsert(row, at, text, len);
    editorUpdateRow(row);
    E.document.file.dirty = 1;
}

/**
 * @brief Append source bytes to a row and refresh its display caches.
 *
 * @details s supplies len bytes. Marks dirty without recording undo; use to
 * join rows as part of a larger action.
 */
static void editorRowAppendString(erow *row, char *s, size_t len) {
    bufferRowAppend(row, s, len);
    editorUpdateRow(row);
    E.document.file.dirty = 1;
}

/* ---- undo / redo ------------------------------------------------------- */

static void editorSetStatusMessage(const char *fmt, ...);
static uint8_t editorDiffersFromDisk(void);

/**
 * @brief Bind source rows and initialize the active document memory budget.
 *
 * @details A budget already active remains unchanged until document reset.
 */
static void editorBindHistory(void) {
    size_t units = (size_t)S.undo_memory_mb;
    size_t unit = (size_t)1024 * 1024;
    size_t budget = units > SIZE_MAX / unit ? SIZE_MAX : units * unit;
    historySetBudget(&E.document, budget);
}

/** @brief Record one rollback boundary before source mutations. */
static void editorPushUndo(enum undoEditType type) {
    if (edit_batch.depth && edit_batch.undo_recorded) return;
    if (edit_batch.depth) edit_batch.undo_recorded = 1;
    editorBindHistory();
    historyRecordEdit(&E.document, S.undo_max_depth, type, time(NULL));
}

/**
 * @brief Prepare the target display before undo/redo changes source ownership.
 *
 * @details On failure both text and history position remain unchanged.
 */
static uint8_t editorPrepareReplay(struct historyAction *action) {
    if (!action) return 1;
    replay_preparing = 1;
    for (struct historyChange *change = action->first; change; change = change->next) {
        if (change->spare.chars && !change->spare.render &&
            !editorRebuildRow(&change->spare)) {
            historyReleaseDisplay(action);
            replay_preparing = 0;
            E.document.history.error = HISTORY_OK;
            editorSetStatusMessage("Not enough memory: undo/redo cancelled");
            return 0;
        }
    }
    replay_preparing = 0;
    return 1;
}

static void editorReplay(uint8_t reverse) {
    if (historyFinishEdit(&E.document) != HISTORY_OK) {
        E.document.history.error = HISTORY_OK;
        editorSetStatusMessage("Not enough memory: edit cancelled");
        return;
    }
    struct historyAction *action = reverse ? E.document.history.undo_stack :
        E.document.history.redo_stack;
    if (!editorPrepareReplay(action)) return;
    if (!(reverse ? historyUndo(&E.document) : historyRedo(&E.document))) {
        editorSetStatusMessage(reverse ? "Nothing to undo" : "Nothing to redo");
        return;
    }
    E.document.selection.active = 0;
    editorInvalidateDocumentCaches();
    editorRehighlightFrom(0, 1);
    E.document.file.dirty = editorDiffersFromDisk();
    editorSetStatusMessage(reverse ? "Undo" : "Redo");
}

static void editorUndo(void) { editorReplay(1); }
static void editorRedo(void) { editorReplay(0); }

/* ---- editor operations --------------------------------------------------- */

/**
 * @brief Insert one byte at the cursor as part of a larger action.
 *
 * @details Creates a row at EOF if needed and advances cx by one byte. The
 * caller must record the action's undo boundary first.
 */
static void editorInsertCharRaw(int32_t c) {
    if (E.document.cursor.cy == E.document.buffer.row_count) editorInsertRow(E.document.buffer.row_count, "", 0);
    if (historyFailed(&E.document)) return;
    editorRowInsertChar(&E.document.buffer.rows[E.document.cursor.cy], E.document.cursor.cx, (uint8_t)c);
    E.document.cursor.cx++;
}

/**
 * @brief Insert a typed byte with undo tracking.
 *
 * @details Use for ordinary typing; consecutive insertions may coalesce into
 * one undo step.
 */
static void editorInsertChar(int32_t c) {
    editorBeginEdit();
    editorPushUndo(EDIT_INSERT);
    editorInsertCharRaw(c);
    editorEndEdit();
}

/**
 * @brief Split the current line and move to the start of the new row.
 *
 * @details Does not copy indentation or record undo. Use after recording a
 * boundary when composing a multiline edit.
 */
static void editorInsertNewlineRaw(void) {
    if (historyFailed(&E.document)) return;
    if (E.document.cursor.cx == 0) {
        editorInsertRow(E.document.cursor.cy, "", 0);
    } else {
        erow *row = &E.document.buffer.rows[E.document.cursor.cy];
        editorInsertRow(E.document.cursor.cy + 1, &row->chars[E.document.cursor.cx], (size_t)(row->size - E.document.cursor.cx));
        row = &E.document.buffer.rows[E.document.cursor.cy];
        if (historyFailed(&E.document)) return;
        bufferRowDeleteRange(row, E.document.cursor.cx, row->size);
        editorUpdateRow(row);
    }
    if (historyFailed(&E.document)) return;
    E.document.cursor.cy++;
    E.document.cursor.cx = 0;
}

/**
 * @brief Handle Enter with optional inherited indentation.
 *
 * @details Records one undo step for both the split and copied indentation;
 * pasted newlines should use the raw helper instead.
 *
 * @note Enter as typed by the user (as opposed to a newline embedded in
 * pasted/recovered text, which goes through editorInsertNewlineRaw() directly
 * and must NOT be reindented -- the source already has whatever indentation it
 * has). When S.auto_indent is on, copies the leading whitespace (spaces/tabs,
 * nothing else) of the line the cursor was on before the split onto the new
 * line, so continuing to type keeps the same indent level without retyping it
 * by hand.
 */
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

    editorBeginEdit();
    editorPushUndo(EDIT_OTHER);
    editorInsertNewlineRaw();

    if (indent_len > 0) {
        erow *row = &E.document.buffer.rows[src_row];
        editorRowInsertString(&E.document.buffer.rows[E.document.cursor.cy],
            E.document.cursor.cx, row->chars, (size_t)indent_len);
        E.document.cursor.cx += indent_len;
    }
    editorEndEdit();
}

/**
 * @brief Apply Backspace to the preceding grapheme or line boundary.
 *
 * @details Records undo, joins rows at column zero, and moves the cursor.
 * Selection deletion is handled by the dispatcher before calling this helper.
 */
static void editorDelChar(void) {
    if (E.document.cursor.cy == E.document.buffer.row_count) return;
    if (E.document.cursor.cx == 0 && E.document.cursor.cy == 0) return;

    editorBeginEdit();
    editorPushUndo(EDIT_DELETE);

    erow *row = &E.document.buffer.rows[E.document.cursor.cy];
    if (E.document.cursor.cx > 0) {
        /* Delete the whole grapheme cluster before cx (base character
         * plus any joined modifiers/marks), not just one byte or one
         * codepoint, so Backspace removes e.g. an emoji with a
         * skin-tone modifier in a single press. */
        size_t del_count = utf8PrevCharLen(row->chars, (size_t)E.document.cursor.cx);
        if (del_count == 0) del_count = 1;
        bufferRowDeleteRange(row, E.document.cursor.cx - (int32_t)del_count, E.document.cursor.cx);
        editorUpdateRow(row);
        E.document.file.dirty = 1;
        E.document.cursor.cx -= (int32_t)del_count;
    } else {
        E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy - 1].size;
        editorRowAppendString(&E.document.buffer.rows[E.document.cursor.cy - 1], row->chars, (size_t)row->size);
        editorDelRow(E.document.cursor.cy);
        E.document.cursor.cy--;
    }
    editorEndEdit();
}

/* ---- file i/o ------------------------------------------------------------- */

/**
 * @brief Resolve the line ending to use for serialization.
 *
 * @details An explicit LF or CRLF setting wins; auto uses the format detected
 * when opening the file.
 */
static enum lineEndingMode editorEffectiveLineEnding(void) {
    if (S.line_ending == LINE_ENDING_LF || S.line_ending == LINE_ENDING_CRLF)
        return (enum lineEndingMode)S.line_ending;
    return E.document.file.detected_line_ending;
}

/**
 * @brief Serialize the active document for saving or backup.
 *
 * @details Writes its byte count to buflen. The caller must free the returned
 * allocation and use the count; it is not NUL-terminated.
 */
static char *editorRowsToString(size_t *buflen) {
    return bufferSerialize(&E.document.buffer, editorEffectiveLineEnding(),
        E.document.file.final_newline, buflen);
}

static void editorSetStatusMessage(const char *fmt, ...);
static void editorSetStatusMessageSticky(const char *fmt, ...);
static void editorRefreshScreen(void);
static int32_t editorReadKey(void);
static int32_t editorReadMultiByteKey(uint8_t lead, char *out);
static void editorFreeUndoRedo(void);
static void editorResetDocument(void);
static void editorCancelMouseDrag(void);
static void abAppend(struct abuf *ab, const char *s, int32_t len);
static void abFree(struct abuf *ab);
static void editorMenuAppend(void *context, const char *text, int32_t len);

/**
 * @brief Make whitespace visible in a single-line prompt.
 *
 * @details buf contains len bytes.
 * @return a new NUL-terminated display string to free; the original prompt
 * input is untouched.
 *
 * @note Prompt input is kept verbatim, but spaces/tabs are always rendered as
 * compact placeholders so whitespace is unambiguous while searching or
 * replacing. Newlines from a clipboard paste are displayed as "\n" so a one-
 * line message bar remains one line, while the underlying replacement text
 * still retains the real newline.
 */
static char *editorPromptDisplayText(const char *buf, size_t len) {
    size_t extra = 0;
    for (size_t i = 0; i < len; i++)
        if (buf[i] == '\n' || buf[i] == '\r') extra++;
    char *display = teMalloc(teSizeAdd(teSizeAdd(len, extra), 1));
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

/**
 * @brief Append input bytes to an existing prompt allocation.
 *
 * @details Grows owned storage as needed, including the NUL terminator.
 * Updates capacity and used length; text must not alias the prompt buffer.
 * Size overflow exits through the terminal cleanup handlers.
 */
static void editorPromptAppend(char **buf, size_t *bufsize, size_t *buflen,
    const char *text, size_t len) {
    if (*buflen == SIZE_MAX || len > SIZE_MAX - *buflen - 1) {
        errno = ENOMEM;
        terminalDie("prompt size");
    }
    size_t needed = *buflen + len + 1;
    if (needed > *bufsize) {
        size_t capacity = *bufsize > 0 ? *bufsize : 1;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        *buf = teRealloc(*buf, capacity);
        *bufsize = capacity;
    }
    memcpy(*buf + *buflen, text, len);
    *buflen += len;
    (*buf)[*buflen] = '\0';
}

static void editorPathCompletionClear(struct editorPathCompletion *completion) {
    for (size_t i = 0; i < completion->count; i++) free(completion->paths[i]);
    free(completion->paths);
    memset(completion, 0, sizeof(*completion));
}

static int editorPathCompare(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void editorPathComplete(struct editorPathCompletion *completion,
    char **buf, size_t *capacity, size_t *length) {
    if (!strcmp(*buf, "~")) {
        editorPromptAppend(buf, capacity, length, "/", 1);
        return;
    }
    if (!completion->count) {
        const char *slash = strrchr(*buf, '/');
        size_t prefix_len = slash ? (size_t)(slash - *buf) + 1 : 0;
        const char *prefix = *buf + prefix_len;
        char *directory = teMalloc(teSizeAdd(prefix_len, 2));
        if (prefix_len) {
            memcpy(directory, *buf, prefix_len);
            directory[prefix_len] = '\0';
        } else strcpy(directory, ".");
        char *expanded = fileioExpandHomePath(directory);
        free(directory);
        if (!expanded) return;
        DIR *dir = opendir(expanded);
        if (!dir) { free(expanded); return; }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            const char *name = entry->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            if (name[0] == '.' && prefix[0] != '.') continue;
            if (strncmp(name, prefix, strlen(prefix))) continue;
            size_t name_len = strlen(name);
            size_t dir_len = strlen(expanded);
            char *lookup = teMalloc(teSizeAdd(teSizeAdd(dir_len, name_len), 2));
            memcpy(lookup, expanded, dir_len);
            lookup[dir_len] = '/';
            memcpy(lookup + dir_len + 1, name, name_len + 1);
            struct stat st;
            uint8_t is_dir = stat(lookup, &st) == 0 && S_ISDIR(st.st_mode);
            free(lookup);
            char *candidate = teMalloc(teSizeAdd(teSizeAdd(prefix_len, name_len), 2));
            memcpy(candidate, *buf, prefix_len);
            memcpy(candidate + prefix_len, name, name_len);
            size_t end = prefix_len + name_len;
            if (is_dir) candidate[end++] = '/';
            candidate[end] = '\0';
            if (completion->count == completion->capacity) {
                completion->capacity = teGrowCapacity(completion->capacity,
                    teSizeAdd(completion->count, 1), SIZE_MAX / sizeof(*completion->paths));
                completion->paths = teRealloc(completion->paths,
                    teArrayBytes(completion->capacity, sizeof(*completion->paths)));
            }
            completion->paths[completion->count++] = candidate;
        }
        closedir(dir);
        free(expanded);
        if (!completion->count) return;
        qsort(completion->paths, completion->count, sizeof(*completion->paths), editorPathCompare);
    }
    const char *candidate = completion->paths[completion->next];
    *length = 0;
    editorPromptAppend(buf, capacity, length, candidate, strlen(candidate));
    completion->next = (completion->next + 1) % completion->count;
    /* A unique directory can immediately be completed one level deeper. */
    if (completion->count == 1) editorPathCompletionClear(completion);
}

/**
 * @brief Run an editable prompt with optional live search feedback.
 *
 * @details prompt is a printf-style format for the input, plus a mode string
 * when status_fn is supplied; short_prompt may be NULL. callback sees the
 * current input and each handled key.
 * @param prompt Format with one %s for input, or mode followed by input when
 * status_fn is set.
 * @param short_prompt Optional compact format with the same placeholder order.
 * @param status_fn Optional callback returning a borrowed mode label on each
 * redraw.
 * @param callback Optional callback invoked after handled input, including
 * acceptance and cancellation.
 * @param complete_paths Enable filesystem completion with Tab.
 * @return owned text on acceptance or NULL on cancellation.
 *

 */
static char *editorPromptCB(const char *prompt, const char *short_prompt,
    const char *(*status_fn)(void), void (*callback)(char *, int32_t), uint8_t complete_paths) {
    size_t bufsize = 128;
    char *buf = teMalloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    struct editorPathCompletion completion = {0};

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
        else if (complete_paths) {
            size_t display_len = strlen(display), start = 0;
            const size_t available = sizeof(E.ui.statusmsg) - sizeof("Path: ");
            while (display_len - start > available)
                start += utf8NextCharLen(display, start, display_len);
            editorSetStatusMessage(start ? "Path: %s" : active_prompt, display + start);
        } else editorSetStatusMessage(active_prompt, display);
        free(display);
        editorRefreshScreen();

        int32_t c = editorReadKey();
        if (c != '\t') editorPathCompletionClear(&completion);
        if (c == '\t' && complete_paths) {
            editorPathComplete(&completion, &buf, &bufsize, &buflen);
        } else if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) {
                buflen -= utf8PrevCharLen(buf, buflen);
                buf[buflen] = '\0';
            }
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

/**
 * @brief Ask for text using the editor's message bar.
 *
 * @details prompt contains a single %s placeholder.
 * @return a NUL-terminated allocation to free, or NULL when the user cancels.
 */
static char *editorPrompt(const char *prompt) {
    return editorPromptCB(prompt, "Path: %s", NULL, NULL, 1);
}

/**
 * @brief Release the active document's text rows.
 *
 * @details Use during document replacement; this clears the buffer but does
 * not reset filename, cursor or history.
 *
 * @note Discards every row currently in the buffer (but not
 * E.document.file.filename), resetting to a blank document. Used before
 * reloading content from scratch -- e.g. replacing what editorOpen() read from
 * disk with a newer crash-recovery backup, see editorOfferBackupRecovery().
 */
static void editorClearRows(void) {
    editorInvalidateDocumentCaches();
    bufferClear(&E.document.buffer);
    E.document.cursor.cx = 0;
    E.document.cursor.cy = 0;
    editorCancelMouseDrag();
}

/**
 * @brief Load recovered bytes as logical rows and detect line endings.
 *
 * @details Builds a candidate before replacing active rows. data is bounded by
 * len; the final newline is metadata, not an extra editable row.
 */
static void editorLoadLines(const char *data, size_t len) {
    struct editorDocument candidate = {0};
    candidate.file.final_newline = 1;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n') continue;
        if (!fileioAppendLine(&candidate, data + start, i - start + 1))
            terminalDie("recovery size");
        start = i + 1;
    }
    if (start < len && !fileioAppendLine(&candidate, data + start, len - start))
        terminalDie("recovery size");
    editorClearRows();
    E.document.buffer = candidate.buffer;
    editorBindHistory();
    E.document.file.final_newline = candidate.file.final_newline;
    E.document.file.detected_line_ending = candidate.file.detected_line_ending;
    E.document.file.line_endings_mixed = candidate.file.line_endings_mixed;
    editorUpdateAllRows();
}

/**
 * @brief Resolve and remember a language label for the current filename.
 *
 * @details Call after naming a document, outside rendering. A user syntax
 * configuration may supply a new label that is persisted to ~/.tinyeditrc.
 *
 * @note Resolves the status-bar filetype for the just-opened file's extension,
 * in the order the two config sources are authoritative in:
 *
 * 1. ~/.tinyeditrc (or the built-in table) already names it. The name is
 * settled; the only open question is whether the syntax .conf that would
 * highlight it is actually installed. A built-in language carries its own
 * compiled-in tokenizer, so only a name that came from a user override can be
 * missing one -- that's the case worth a warning, since the user sees a
 * language name in the status bar but no colors and would otherwise have no
 * hint why. 2. Nothing names it, but an installed .conf claims the extension
 * and declares a "filetype" key. Record it in ~/.tinyeditrc so the extension
 * is known from now on, and persist immediately.
 *
 * Anything else (no name anywhere, or a .conf without a filetype key) leaves
 * the field absent, exactly as before.
 *
 * Called once per file open rather than from editorFiletypeLabel(), which runs
 * on every status-bar redraw -- resolving and potentially writing
 * ~/.tinyeditrc at that rate would be wasteful and would put a disk write in
 * the render path.
 */
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

/**
 * @brief Explain why a named filetype has no syntax colors.
 *
 * @details Call after startup hints so this message stays visible. Checks user
 * and built-in highlighting before warning.
 *
 * @note Warns when the open file's extension has a filetype name but no
 * highlighting to go with it -- see editorResolveFiletype() above for why only
 * a user-declared name can end up in that state. Split out from that function,
 * and called after the startup hint is set, so the sticky hint doesn't
 * immediately overwrite this message (same ordering constraint the backup-
 * recovery message has).
 */
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

/**
 * @brief Associate a filename and load its rows into the active buffer.
 *
 * @details Reads an independent candidate before replacing the active document.
 * A missing file becomes a named new buffer; I/O errors preserve current state.
 * @return 1 on commit, 0 with errno on failure.
 */
static uint8_t editorOpen(const char *filename) {
    char *path = fileioExpandHomePath(filename);
    if (!path) return 0;
    struct editorDocument candidate;
    uint8_t loaded = fileioLoadDocument(path, &candidate);
    int saved_errno = errno;
    free(path);
    if (!loaded) { errno = saved_errno; return 0; }
    /* No current text, history or backup is released until reading and
     * closing the candidate have both succeeded. */
    editorResetDocument();
    E.document = candidate;
    editorBindHistory();
    editorResolveFiletype();
    editorUpdateAllRows();
    if (E.document.file.line_endings_mixed)
        editorSetStatusMessage("Mixed line endings: auto uses %s",
            E.document.file.detected_line_ending == LINE_ENDING_CRLF ? "CRLF" : "LF");
    return 1;
}

/**
 * @brief Write a recovery copy when the dirty document's backup is due.
 *
 * @details Call from the main loop. Unnamed documents, disabled backups and
 * intervals that have not elapsed are skipped.
 *
 * @note Writes a crash-recovery backup if S.backup_interval seconds have
 * passed since the last one and the buffer has unsaved changes (a clean buffer
 * has nothing to recover that isn't already safely on disk, so writing one
 * would be pure overhead). Called once per main loop iteration -- cheap when
 * not due, since it's just a time(NULL) and comparison until the interval
 * actually elapses. A brand new buffer with no filename yet is skipped:
 * there's nowhere stable to derive a backup path from until the user picks a
 * name (Ctrl-S).
 */
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

/**
 * @brief Append a centered, padded line to the recovery warning.
 *
 * @details ab receives output bytes; bg is an ANSI background or attribute
 * sequence and text is NUL-terminated.
 */
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

/**
 * @brief Display the recovery warning before asking for a decision.
 *
 * @details Writes the frame to the terminal; reading the answer and loading
 * the backup are the caller's responsibility.
 */
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

/**
 * @brief Offer to replace the loaded document with its recovery copy.
 *
 * @details Call after opening the file. Only y/Y accepts; successful recovery
 * marks dirty and keeps the backup until save or clean exit.
 */
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

    editorLoadLines(content, len);
    free(content);
    E.document.file.dirty = 1;
    editorSetStatusMessage("Recovered unsaved changes from backup.");
}

/**
 * @brief Save to a candidate path and commit identity after durable replacement.
 * @details An uncertain replacement keeps the old identity and recovery data;
 * it must not be mistaken for an untouched target. Undo history is preserved.
 */
static enum fileSaveResult editorSaveToPath(const char *filename) {
    char *path = fileioExpandHomePath(filename);
    if (!path) {
        editorSetStatusMessage("Can't save! Path error: %s", strerror(errno));
        return FILE_SAVE_FAILED;
    }
    size_t len;
    char *bytes = editorRowsToString(&len);
    enum fileSaveResult result = fileioAtomicSave(path, bytes, len);
    int saved_errno = errno;
    free(bytes);
    if (result == FILE_SAVE_DURABLE) {
        char *name = teStrdup(path);
        if (E.document.file.filename) backupRemove(E.document.file.filename);
        backupRemove(name);
        free(E.document.file.filename);
        E.document.file.filename = name;
        E.document.file.dirty = 0;
        E.document.file.save_uncertain = 0;
        E.document.file.last_backup_time = 0;
        editorResolveFiletype();
        editorRehighlightFrom(0, 1);
        editorSetStatusMessage("%zu bytes written to disk", len);
    } else if (result == FILE_SAVE_UNCERTAIN) {
        E.document.file.dirty = 1;
        E.document.file.save_uncertain = 1;
        uint8_t renamed = !E.document.file.filename ||
            strcmp(path, E.document.file.filename) != 0;
        editorSetStatusMessageSticky(
            "Target written, durability unconfirmed; name unchanged. Retry %s: %s",
            renamed ? "Save as" : "save", strerror(saved_errno));
    } else {
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(saved_errno));
    }
    free(path);
    errno = saved_errno;
    return result;
}

/**
 * @brief Save the active document, asking for a name when needed.
 *
 * @details force_prompt selects Save as. On success clears dirty and removes
 * the backup; cancellation or I/O failure is reported in the message bar.
 *
 * @note `force_prompt` makes an already-named buffer go through "Save as"
 * again (F4/Ctrl-Shift-S) instead of silently overwriting
 * E.document.file.filename, which is what a plain save (Ctrl-S) does when a
 * name already exists.
 */
static void editorSaveInternal(uint8_t force_prompt) {
    char *name = NULL;
    if (E.document.file.filename == NULL || force_prompt) {
        name = editorPrompt("Save as: (Tab complete, Esc cancel) %s");
        if (!name) {
            editorSetStatusMessage("Save aborted.");
            return;
        }
        if (name[0] == '\0') {
            free(name);
            editorSetStatusMessage("Save aborted: empty filename.");
            return;
        }
    }
    editorSaveToPath(name ? name : E.document.file.filename);
    free(name);
}

/**
 * @brief Save to the current filename, or ask for one if unnamed.
 *
 * @details Delegates to editorSaveInternal() with the normal save behavior.
 */
static void editorSave(void) {
    editorSaveInternal(0);
}

/**
 * @brief Ask for a filename and save under that name.
 *
 * @details Uses the shared save path even if the document already has a
 * filename.
 */
static void editorSaveAs(void) {
    editorSaveInternal(1);
}

/**
 * @brief Check whether the serialized document actually differs from its file.
 *
 * @details Used after undo/redo and before leaving a dirty document.
 * @return 1 for changed text or an unreadable file; unnamed empty buffers
 * are unchanged.
 *
 * Any I/O failure answers "yes, it differs": if the file can't be read the
 * safe assumption is that there is something to lose, so the user still gets
 * the prompt. An unnamed document is changed when it contains text.
 */
static uint8_t editorDiffersFromDisk(void) {
    if (E.document.file.save_uncertain) return 1;
    if (!E.document.file.filename)
        return E.document.buffer.row_count > 1 ||
            (E.document.buffer.row_count == 1 && E.document.buffer.rows[0].size > 0);

    FILE *fp = fopen(E.document.file.filename, "rb");
    if (!fp) return 1;

    uint8_t differs = 0;
    char disk[4096];
    const char *ending = editorEffectiveLineEnding() == LINE_ENDING_CRLF ? "\r\n" : "\n";
    size_t ending_len = strlen(ending);
    for (int32_t y = 0; !differs && y < E.document.buffer.row_count; y++) {
        const erow *row = &E.document.buffer.rows[y];
        for (size_t offset = 0; !differs && offset < (size_t)row->size; ) {
            size_t count = (size_t)row->size - offset;
            if (count > sizeof(disk)) count = sizeof(disk);
            differs = fread(disk, 1, count, fp) != count ||
                memcmp(disk, row->chars + offset, count) != 0;
            offset += count;
        }
        if (!differs && (y + 1 < E.document.buffer.row_count || E.document.file.final_newline))
            differs = fread(disk, 1, ending_len, fp) != ending_len ||
                memcmp(disk, ending, ending_len) != 0;
    }
    if (!differs) differs = fgetc(fp) != EOF || ferror(fp) != 0;

    fclose(fp);
    return differs;
}

/**
 * @brief Resolve unsaved changes before leaving the document.
 *
 * @details action is the readable operation name in the prompt.
 * @return 1 to proceed, 0 on cancellation or an unsuccessful save.
 *
 * @note Shared save/discard/cancel gate for every operation that would leave
 * the current document (quit, close, or open another file). Returns 1 only
 * when it is safe to proceed; a failed/cancelled save leaves the document
 * open.
 */
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

/**
 * @brief Release the current document and reset all document-specific state.
 *
 * @details Use only after resolving unsaved changes. Removes its backup and
 * history while preserving terminal dimensions and live settings.
 *
 * @note Releases every piece of state owned by the current document while
 * keeping terminal dimensions and user settings intact. The next document
 * starts with independent cursor, selection, search, backup, and undo state.
 */
static void editorResetDocument(void) {
    searchQueryFree(&E.search.query);
    E.search.last_cy = -1;
    E.search.last_end_y = -1;
    if (E.document.file.filename) backupRemove(E.document.file.filename);
    editorFreeUndoRedo();
    editorClearRows();
    free(E.document.file.filename);
    E.document.file.filename = NULL;

    E.document.cursor.cx = 0;
    E.document.cursor.cy = 0;
    E.document.cursor.rx = 0;
    E.view.rowoff = 0;
    E.view.coloff = 0;
    E.view.free_scroll = 0;
    E.document.file.detected_line_ending = LINE_ENDING_LF;
    E.document.file.line_endings_mixed = 0;
    E.document.file.final_newline = 1;
    E.document.file.dirty = 0;
    E.document.file.save_uncertain = 0;
    E.document.selection.active = 0;
    E.document.selection.anchor_x = 0;
    E.document.selection.anchor_y = 0;
    E.document.selection.pinned = 0;
    editorCancelMouseDrag();
    E.search.search_match_y = -1;
    E.search.search_match_x = 0;
    E.search.search_match_len = 0;
    E.search.search_match_end_y = -1;
    E.search.search_match_end_x = 0;
    E.document.file.last_backup_time = 0;
    E.document.history.last_edit_type = EDIT_NONE;
    E.document.history.last_edit_time = 0;
    editorBindHistory();
}

/**
 * @brief Close the document after resolving unsaved changes.
 *
 * @details Leaves the editor running with a fresh unnamed buffer; cancellation
 * keeps the current document.
 */
static void editorCloseFile(void) {
    if (!editorConfirmDocumentChange("closing")) return;
    editorResetDocument();
    editorSetStatusMessageSticky("File closed. %s-O open | %s-Q quit | F1 help",
        editorPrimaryModifier(), editorPrimaryModifier());
}

/**
 * @brief Ask for another path and switch the active document.
 *
 * @details Checks unsaved changes and fully reads the target before resetting
 * state. A missing target is a new named buffer; cancellation keeps the
 * current document.
 *
 * @note Ctrl-O switches the single active document. The current document
 * remains in place if save confirmation or path entry is cancelled, or if an
 * existing target cannot be read. ENOENT is intentional: like Vim, this opens
 * a named empty buffer and the file is created on the first successful save.
 */
static void editorOpenFile(void) {
    if (!editorConfirmDocumentChange("opening another file")) return;

    char *name = editorPrompt("Open file: (Tab complete, Esc cancel) %s");
    if (!name) {
        editorSetStatusMessage("Open cancelled.");
        return;
    }

    struct stat st;
    if (!editorOpen(name)) {
        editorSetStatusMessage("Can't open file: %s", strerror(errno));
        free(name);
        return;
    }
    uint8_t exists = stat(E.document.file.filename, &st) == 0;
    free(name);
    editorOfferBackupRecovery();
    if (!E.document.file.dirty) {
        if (exists) editorSetStatusMessage("Opened %s", E.document.file.filename);
        else editorSetStatusMessage("New file: %s", E.document.file.filename);
    }
}

/**
 * @brief Leave the editor after resolving unsaved changes.
 *
 * @details Removes the document's recovery copy and calls exit(), allowing
 * registered terminal cleanup handlers to run.
 */
static void editorQuit(void) {
    if (!editorConfirmDocumentChange("quitting")) return;

    /* A discarded dirty buffer must not leave a recovery file that would
     * look like evidence of a crash the next time this path is opened. */
    if (E.document.file.filename) backupRemove(E.document.file.filename);

    exit(0);
}

/* ---- append buffer -------------------------------------------------------- */

/**
 * @brief Append output bytes to the frame being assembled.
 *
 * @details ab must be initialized with ABUF_INIT; s contains len bytes. The
 * buffer grows as needed and is not NUL-terminated.
 */
static void abAppend(struct abuf *ab, const char *s, int32_t len) {
    if (len <= 0) return;
    if (len > INT32_MAX - ab->len) { errno = EOVERFLOW; terminalDie("frame size"); }
    size_t needed = (size_t)ab->len + (size_t)len;
    if (needed > ab->capacity) {
        size_t capacity = ab->capacity ? ab->capacity : 256;
        while (capacity < needed) capacity *= 2;
        ab->b = teRealloc(ab->b, capacity);
        ab->capacity = capacity;
    }
    memcpy(ab->b + ab->len, s, (size_t)len);
    ab->len += len;
}

/**
 * @brief Adapt the menu output callback to the editor's frame buffer.
 *
 * @details context must point to an initialized abuf; text contains len bytes
 * that are copied immediately.
 */
static void editorMenuAppend(void *context, const char *text, int32_t len) {
    abAppend((struct abuf *)context, text, len);
}

/**
 * @brief Release the storage used by a rendered frame.
 *
 * @details Call once after writing the frame; the abuf fields are not reset
 * and must not be reused without initialization.
 */
static void abFree(struct abuf *ab) { free(ab->b); }

/**
 * @brief Reset text attributes while restoring the editor background.
 *
 * @details Use during document rendering instead of a bare SGR reset so syntax
 * and selection colors do not erase the configured background.
 *
 * @note Resets SGR attributes (colors, reverse-video, bold, ...) the same as a
 * literal "\x1b[m" everywhere else in this file, but immediately re-applies
 * the configured background color (see color_background in settings.h) if one
 * is set -- otherwise every reset scattered through editorDrawRowSegment()
 * (one per highlighted character/glyph, see its per-char
 * syn_color/is_invisible_glyph resets) would also erase the background for the
 * very next character, defeating a whole-editor background the instant any
 * syntax highlighting or selection is active. Callers that don't need to
 * distinguish should always use this over a bare "\x1b[m" for that reason.
 */
static void abAppendReset(struct abuf *ab) {
    abAppend(ab, "\x1b[m", 3);
    const char *bg = ansiBgColorCode(S.color_background);
    if (bg[0]) abAppend(ab, bg, (int32_t)strlen(bg));
    if (drawing_heading) {
        const char *fg = ansiColorCode(S.color_syntax_preprocessor);
        abAppend(ab, fg, (int32_t)strlen(fg));
        abAppend(ab, "\x1b[7m", 4);
    }
    if (drawing_heading_bold) abAppend(ab, "\x1b[1m", 4);
}

/* ---- output ---------------------------------------------------------------- */

/**
 * @brief Get the current line-number gutter width in screen columns.
 *
 * @return zero when line numbers are hidden; includes the padding between
 * numbers and text.
 *
 * @note Width of the left-hand line-number gutter, including one space of
 * padding before the text starts. Zero when gutter is disabled. Grows with
 * E.document.buffer.row_count so files with 1000+ lines still right-align
 * cleanly.
 */
static int32_t editorGutterWidth(void) {
    return renderGutterWidth(&E.document.buffer, (uint8_t)S.show_line_numbers);
}

/**
 * @brief Get the screen columns available to document text.
 *
 * @details Subtracts the gutter from the terminal width and never returns a
 * negative value.
 */
static int32_t editorTextCols(void) {
    return renderTextCols(&E.view, &E.document.buffer, (uint8_t)S.show_line_numbers);
}

/**
 * @brief Choose the effective wrapping width for the current viewport.
 *
 * @return at least one display column, applying the configured cap and the
 * right-hand margin.
 *
 * @note Effective soft-wrap width in render columns. Wrap is always active (no
 * horizontal scrolling in this editor): text always wraps at the window edge
 * (editorTextCols() - 1 column of right-hand margin) at minimum. soft_wrap ==
 * 0 means "no extra limit, just the window edge"; soft_wrap > 0 additionally
 * caps the width to that value when the window is wider than it (e.g. to keep
 * prose readable on a wide terminal), while still following the window edge on
 * a narrower one.
 */
static int32_t editorSoftWrapCols(void) {
    return renderSoftWrapCols(&E.view, &E.document.buffer,
        (uint8_t)S.show_line_numbers, S.soft_wrap);
}

/**
 * @brief Ensure the row's wrap boundaries are available.
 *
 * @details wrapcols is measured in display columns.
 * @return the segment count; row owns separate cached byte offsets and
 * display-column offsets.
 */
static int32_t editorRowSegments(erow *row, int32_t wrapcols) {
    return renderRowSegments(row, wrapcols);
}

/**
 * @brief Find the render-byte end of a segment after trimming spaces.
 *
 * @details i is a valid segment index in seg_start.
 * @return an exclusive byte offset, not a screen column.
 */
static int32_t editorSegVisibleEnd(erow *row, int32_t nseg, const int32_t *seg_start, int32_t i) {
    return renderSegmentVisibleEnd(row, nseg, seg_start, i);
}

/**
 * @brief Find the display-column end of a segment after trimming spaces.
 *
 * @details Use alongside rx values for End navigation. seg_start stores bytes
 * and seg_start_rx stores display columns.
 */
static int32_t editorSegVisibleEndRx(erow *row, int32_t nseg, const int32_t *seg_start,
    const int32_t *seg_start_rx, int32_t i) {
    return renderSegmentVisibleEndRx(row, nseg, seg_start, seg_start_rx, i, S.tab_stop);
}

/**
 * @brief Translate a row-wide display column into a wrap segment and local column.
 *
 * @details Writes zero-based outputs and builds the row's wrap cache if
 * necessary.
 *
 * @note Finds which visual segment of `row` contains render-column `rx`, and
 * the column within that segment. Used to translate the logical cursor
 * position into (segment index, in-segment column) for scrolling/rendering
 * with wrap active.
 */
static void editorRxToSegment(erow *row, int32_t wrapcols, int32_t rx, int32_t *seg_idx, int32_t *seg_col) {
    renderRxToSegment(row, wrapcols, rx, seg_idx, seg_col);
}

/**
 * @brief Convert a logical row and segment into an absolute visual row.
 *
 * @details filerow and seg are zero-based. Uses wrapcols to count all
 * preceding wrapped rows.
 */
static int32_t editorVideoRowOf(int32_t filerow, int32_t seg, int32_t wrapcols) {
    return renderVideoRowOf(&E.document.buffer, filerow, seg, wrapcols);
}

/**
 * @brief Map an absolute visual row back to its logical row and segment.
 *
 * @details Writes zero-based outputs; positions beyond the text fall back to
 * the last logical row and segment zero.
 */
static void editorFileRowAtVideoRow(int32_t vy, int32_t wrapcols, int32_t *out_filerow, int32_t *out_seg) {
    renderFileRowAtVideoRow(&E.document.buffer, vy, wrapcols, out_filerow, out_seg);
}

/**
 * @brief Count the visual rows occupied by the active document.
 *
 * @details Use to bound viewport scrolling; an empty document returns zero.
 */
static int32_t editorTotalVideoRows(int32_t wrapcols) {
    return renderTotalVideoRows(&E.document.buffer, wrapcols);
}

/**
 * @brief Map terminal mouse coordinates to an addressable text position.
 *
 * @details screen_col and screen_row are one-based. Filter non-text UI clicks
 * first; outputs are a zero-based logical row and source-byte offset.
 *
 * @note Converts a 1-based (screen_col, screen_row) terminal coordinate --
 * exactly what an SGR mouse report gives (see MOUSE_EVENT_KEY) -- into a (file
 * row, file column) cursor position, clamped to the nearest valid spot if the
 * click landed outside the text (e.g. past the end of a short line, in the
 * gutter, or below the last line). Inverse of the cursor-positioning math in
 * editorRefreshScreen() (see "cursor_row + 1 + (S.show_top_bar ? 1 : 0) +
 * (S.show_menu ? 1 : 0)" / "cursor_col + editorGutterWidth() + 1" there) --
 * kept as its own function since both need the exact same coordinate transform
 * and must not drift apart from each other. Clicks in the gutter or
 * status/message bars are the caller's responsibility to filter out first
 * (this function assumes a click inside the text area).
 */
static void editorMouseToCursor(int32_t screen_col, int32_t screen_row, int32_t *out_cy, int32_t *out_cx) {
    int32_t gutter = editorGutterWidth();
    int32_t wrapcols = editorSoftWrapCols();

    int32_t cursor_row = screen_row - 1 - (S.show_top_bar ? 1 : 0) -
        (S.show_menu ? 1 : 0);
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

/**
 * @brief Update cursor display coordinates and keep its context in view.
 *
 * @details Call before drawing. Consumes the one-shot free_scroll flag so
 * mouse-wheel scrolling can leave the cursor stationary.
 */
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
    /* A margin wider than half the viewport has no stable position:
     * its top and bottom constraints overlap. Cap it locally so a
     * value such as 20 still behaves sensibly in a short terminal. */
    int32_t scrolloff = S.scrolloff;
    int32_t max_scrolloff = (E.view.screenrows - 1) / 2;
    if (scrolloff > max_scrolloff) scrolloff = max_scrolloff;

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

        if (cursor_vy < E.view.rowoff + scrolloff)
            E.view.rowoff = cursor_vy - scrolloff;
        if (cursor_vy >= E.view.rowoff + E.view.screenrows - scrolloff)
            E.view.rowoff = cursor_vy - E.view.screenrows + scrolloff + 1;
        if (E.view.rowoff < 0) E.view.rowoff = 0;
        int32_t max_rowoff = editorTotalVideoRows(wrapcols) - E.view.screenrows;
        if (max_rowoff < 0) max_rowoff = 0;
        if (E.view.rowoff > max_rowoff) E.view.rowoff = max_rowoff;
        E.view.coloff = 0;
    } else {
        if (E.document.cursor.cy < E.view.rowoff + scrolloff)
            E.view.rowoff = E.document.cursor.cy - scrolloff;
        if (E.document.cursor.cy >= E.view.rowoff + E.view.screenrows - scrolloff)
            E.view.rowoff = E.document.cursor.cy - E.view.screenrows + scrolloff + 1;
        if (E.view.rowoff < 0) E.view.rowoff = 0;
        int32_t max_rowoff = E.document.buffer.row_count - E.view.screenrows;
        if (max_rowoff < 0) max_rowoff = 0;
        if (E.view.rowoff > max_rowoff) E.view.rowoff = max_rowoff;
        if (E.document.cursor.rx < E.view.coloff) E.view.coloff = E.document.cursor.rx;
        int32_t textcols = editorTextCols();
        if (E.document.cursor.rx >= E.view.coloff + textcols) E.view.coloff = E.document.cursor.rx - textcols + 1;
    }
}

/**
 * @brief Append a portion of a row with syntax, search and selection colors.
 *
 * @details seg_from and seg_to delimit a half-open render-byte range.
 * Selection and bracket x coordinates refer to source bytes; y coordinates are
 * logical rows.
 *
 * @note Draws the render-byte range [seg_from, seg_to) of `filerow` into `ab`,
 * applying selection/search-match highlight -- the body shared by both the
 * unwrapped (one call per file row) and wrapped (one call per visual segment)
 * paths in editorDrawRows().
 */
static void editorDrawRowSegment(struct abuf *ab, int32_t filerow, int32_t seg_from, int32_t seg_to,
    uint8_t has_sel, int32_t sel_y0, int32_t sel_x0, int32_t sel_y1, int32_t sel_x1,
    uint8_t has_pair, int32_t pair_y0, int32_t pair_x0, int32_t pair_y1, int32_t pair_x1) {
    erow *row = &E.document.buffer.rows[filerow];
    int32_t len = seg_to - seg_from;
    if (len <= 0) return;
    const char *ext = E.document.file.filename ? strrchr(E.document.file.filename, '.') : NULL;
    uint8_t markdown_styles = S.markdown_text_styles && S.syntax_highlight &&
        ext && syntaxIsMarkdownExtension(ext + 1);

    char *line = &row->render[seg_from];
    int32_t row_sel_start = -1, row_sel_end = -1;
    if (has_sel && filerow >= sel_y0 && filerow <= sel_y1) {
        row_sel_start = (filerow == sel_y0) ? sel_x0 : 0;
        row_sel_end = (filerow == sel_y1) ? sel_x1 : row->size;
    }

    int32_t match_start = -1, match_end = -1;
    if (E.search.search_match_y <= filerow && filerow <= E.search.search_match_end_y) {
        match_start = filerow == E.search.search_match_y ? E.search.search_match_x : 0;
        match_end = filerow == E.search.search_match_end_y ? E.search.search_match_end_x : row->size;
    }

    int32_t source_byte = 0, render_byte = 0, source_rx = 0;
    uint8_t in_sel = 0;
    for (int32_t j = 0; j < len; ) {
        int32_t rendercol = seg_from + j;
        /* Selection and search coordinates refer to bytes in chars[],
         * while this loop walks render[]. A tab occupies several render
         * bytes, all of which represent the same source byte. */
        while (source_byte < row->size && render_byte < rendercol) {
            uint8_t is_tab = row->chars[source_byte] == '\t';
            size_t step = is_tab ? 1 : utf8NextCharLen(row->chars,
                (size_t)source_byte, (size_t)row->size);
            int32_t width = is_tab ? S.tab_stop - source_rx % S.tab_stop :
                utf8SingleCharWidth(row->chars + source_byte, step);
            int32_t render_step = is_tab ? width : (int32_t)step;
            if (render_byte + render_step > rendercol) break;
            render_byte += render_step;
            source_byte += (int32_t)step;
            source_rx += width;
        }
        int32_t filecol = source_byte;
        size_t char_len = utf8NextCharLen(line, (size_t)j, (size_t)len);
        if (char_len == 0 || (size_t)j + char_len > (size_t)len) char_len = 1;
        int32_t emitted_len = (int32_t)char_len;
        struct utf8DecodeResult decoded = utf8DecodeChar(line + j, (size_t)(len - j));
        uint8_t should_sel = (row_sel_start >= 0 &&
            filecol >= row_sel_start && filecol < row_sel_end) ||
            (match_start >= 0 && filecol >= match_start && filecol < match_end);
        uint8_t should_pair = has_pair &&
            ((filerow == pair_y0 && filecol == pair_x0) ||
             (filerow == pair_y1 && filecol == pair_x1));
        uint8_t should_highlight = should_sel || should_pair;
        if (should_highlight && !in_sel) {
            const char *sel_color = ansiColorCode(S.color_selection);
            abAppend(ab, sel_color, (int32_t)strlen(sel_color));
            abAppend(ab, drawing_heading ? "\x1b[27m" : "\x1b[7m",
                drawing_heading ? 5 : 4);
            in_sel = 1;
        } else if (!should_highlight && in_sel) {
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
        uint8_t is_invisible_glyph = !should_highlight && emitted_len == 1 &&
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
         * for this row (see editorUpdateRow()); plain brackets still use
         * their configured color independently of language detection. */
        const char *syn_color = NULL;
        if (!should_highlight && !is_invisible_glyph) {
            uint8_t plain_bracket = decoded.valid &&
                decoded.codepoint > 0 && decoded.codepoint < 128 && strchr("()[]{}", (char)decoded.codepoint) &&
                (!row->hl || (rendercol < row->rsize && row->hl[rendercol] == HL_NORMAL));
            if (plain_bracket) {
                syn_color = ansiColorCode(S.color_syntax_bracket);
            } else if (row->hl && rendercol < row->rsize) {
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
        uint8_t styled = markdown_styles && row->hl && rendercol < row->rsize;
        if (styled) {
            if (row->hl_heading || row->hl[rendercol] == HL_EMPHASIS_STRONG)
                abAppend(ab, "\x1b[1m", 4);
            else if (row->hl[rendercol] == HL_EMPHASIS)
                abAppend(ab, "\x1b[3m", 4);
            else styled = 0;
        }

        /* Emit the complete UTF-8 sequence before resetting the color.
         * ANSI escapes between continuation bytes would split the codepoint
         * and make terminals render replacement diamonds (�), especially
         * visible with accented characters such as é. */
        if (decoded.valid) abAppend(ab, &line[j], emitted_len);
        else abAppend(ab, "\xef\xbf\xbd", 3);

        if (syn_color || styled) abAppendReset(ab);
        if (is_invisible_glyph) abAppendReset(ab);
        j += emitted_len;
    }
    if (in_sel) abAppendReset(ab);
}

/**
 * @brief Decide whether selection or search highlighting crosses a row boundary.
 *
 * @details Used to highlight the end-of-line cell even when invisible markers
 * are off.
 * @return 1 when the newline after filerow lies within a selection or a
 * multiline search match.
 *
 * @note A logical newline lives between two rows, rather than in either row's
 * chars buffer. Give it one visible cell when a selection or regex match
 * crosses it, so blank selected rows and \n matches don't disappear.
 */
static uint8_t editorRowTerminatorHighlighted(int32_t filerow, uint8_t has_sel,
    int32_t sel_y0, int32_t sel_y1) {
    if (has_sel && filerow >= sel_y0 && filerow < sel_y1) return 1;
    return E.search.search_match_y >= 0 &&
        filerow >= E.search.search_match_y && filerow < E.search.search_match_end_y;
}

/**
 * @brief Append a highlighted end-of-line cell.
 *
 * @details Uses the selection color and optional invisible glyph, then
 * restores the normal rendering attributes.
 */
static void editorDrawHighlightedTerminator(struct abuf *ab) {
    const char *sel_color = ansiColorCode(S.color_selection);
    abAppend(ab, sel_color, (int32_t)strlen(sel_color));
    abAppend(ab, drawing_heading ? "\x1b[27m" : "\x1b[7m",
        drawing_heading ? 5 : 4);
    abAppend(ab, S.show_invisibles ? "$" : " ", 1);
    abAppendReset(ab);
}

/**
 * @brief Append the line number or padding for a wrapped continuation.
 *
 * @details gutter is a width in columns and filerow is zero-based.
 * Continuations keep the same gutter width without repeating the number.
 */
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

/**
 * @brief Choose a startup or Info slogan, avoiding the previous choice.
 *
 * @details Updates the UI slogan index. Uses system randomness when available
 * and a pseudorandom fallback otherwise.
 *
 * @note Splash lines shown on the empty buffer, centered as a block. NULL is a
 * blank spacer line. Kept short and few: this is the first thing a new user
 * sees and the only place the basic keys are advertised without knowing to
 * press F1, but it still has to fit a small terminal, so it lists the way out
 * and the way to get help rather than trying to summarize the whole keymap.
 */
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

/**
 * @brief Append one row of the empty editor's splash screen.
 *
 * @details y is a zero-based viewport row and textcols is the available
 * display width.
 *
 * @note Draws the splash line belonging to video row `y`, or a plain "~" when
 * that row isn't part of the block. The block is centered both ways and
 * skipped entirely on a terminal too short to hold it, where "~" everywhere is
 * better than a half-drawn banner.
 */
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

static void editorBeginHeadingRow(struct abuf *ab, int32_t filerow) {
    uint8_t heading = filerow < E.document.buffer.row_count &&
        E.document.buffer.rows[filerow].hl_heading;
    drawing_heading = heading && S.markdown_heading_reverse;
    drawing_heading_bold = heading && S.markdown_text_styles;
    if (drawing_heading || drawing_heading_bold) abAppendReset(ab);
    if (drawing_heading) {
        /* Erase-to-end does not reliably paint reverse-video blanks. Write
         * actual spaces first, then return to render the row over them. */
        for (int32_t col = 0; col < editorTextCols(); col++) abAppend(ab, " ", 1);
        char position[32];
        int32_t len = snprintf(position, sizeof(position), "\x1b[%dG", editorGutterWidth() + 1);
        abAppend(ab, position, len);
    }
}

static void editorEndHeadingRow(struct abuf *ab) {
    if (!drawing_heading) abAppend(ab, "\x1b[K", 3);
    if (drawing_heading || drawing_heading_bold) {
        drawing_heading = 0;
        drawing_heading_bold = 0;
        abAppendReset(ab);
    }
    abAppend(ab, "\r\n", 2);
}

/**
 * @brief Append the visible document rows or the startup splash.
 *
 * @details Uses the current viewport and cached wrapping; selection, search
 * and bracket highlighting are computed for this frame.
 */
static void editorDrawRows(struct abuf *ab) {
    int32_t sel_y0 = 0, sel_x0 = 0, sel_y1 = 0, sel_x1 = 0;
    uint8_t has_sel = editorSelectionRange(&E.document.selection, &E.document.cursor, &sel_y0, &sel_x0, &sel_y1, &sel_x1);
    int32_t pair_y0 = -1, pair_x0 = -1, pair_y1 = -1, pair_x1 = -1;
    uint8_t has_pair = editorMatchingPairAtCursor(&pair_y0, &pair_x0, &pair_y1, &pair_x1);
    int32_t gutter = editorGutterWidth();
    int32_t textcols = editorTextCols();
    int32_t wrapcols = editorSoftWrapCols();

    if (wrapcols == 0) {
        for (int32_t y = 0; y < E.view.screenrows; y++) {
            int32_t filerow = y + E.view.rowoff;
            editorDrawGutter(ab, gutter, filerow, 0);
            editorBeginHeadingRow(ab, filerow);

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
                    has_sel, sel_y0, sel_x0, sel_y1, sel_x1,
                    has_pair, pair_y0, pair_x0, pair_y1, pair_x1);
                if (editorRowTerminatorHighlighted(filerow, has_sel, sel_y0, sel_y1))
                    editorDrawHighlightedTerminator(ab);
            }

            editorEndHeadingRow(ab);
        }
        return;
    }

    /* Locate the first visible segment once, then walk forward. */
    int32_t filerow = 0, seg = 0;
    int32_t remaining = E.view.rowoff;
    while (filerow < E.document.buffer.row_count) {
        int32_t height = editorRowSegments(&E.document.buffer.rows[filerow], wrapcols);
        if (remaining < height) { seg = remaining; break; }
        remaining -= height;
        filerow++;
    }
    for (int32_t y = 0; y < E.view.screenrows; y++) {
        if (filerow >= E.document.buffer.row_count) {
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

        erow *row = &E.document.buffer.rows[filerow];
        int32_t nseg = editorRowSegments(row, wrapcols);
        int32_t seg_from = row->seg_start[seg];
        int32_t seg_to = editorSegVisibleEnd(row, nseg, row->seg_start, seg);

        editorDrawGutter(ab, gutter, filerow, seg > 0);
        editorBeginHeadingRow(ab, filerow);
        editorDrawRowSegment(ab, filerow, seg_from, seg_to, has_sel, sel_y0, sel_x0, sel_y1, sel_x1,
            has_pair, pair_y0, pair_x0, pair_y1, pair_x1);

        /* End-of-line glyph: only after the LAST visual segment of a
         * logical row (seg == nseg - 1), not after every wrapped
         * video row -- a soft-wrap point isn't a real newline in the
         * file, only the row's actual end is. Appended rather than
         * substituted into render (see editorUpdateRow()), so it's
         * exempt from the single-byte-glyph constraint that applies
         * to in-line invisibles. */
        uint8_t terminator_highlighted = seg == nseg - 1 &&
            editorRowTerminatorHighlighted(filerow, has_sel, sel_y0, sel_y1);
        if (terminator_highlighted) {
            editorDrawHighlightedTerminator(ab);
        } else if (S.show_invisibles && seg == nseg - 1) {
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

        editorEndHeadingRow(ab);
        if (++seg == nseg) { filerow++; seg = 0; }
    }
}

/**
 * @brief Count editable graphemes and boundaries between logical rows.
 *
 * @return a character count for the UI, not a byte count; the file's optional
 * final newline is excluded.
 *
 * @note Counts grapheme clusters (not bytes, not raw codepoints) across the
 * whole buffer, using the same utf8NextCharLen() boundary logic as cursor
 * movement -- so an emoji with a skin-tone modifier counts as one character
 * here too, consistent with how it's edited/deleted as one unit elsewhere in
 * the editor. Newlines between rows count as one character each, matching how
 * the file is written to disk (editorRowsToString() joins rows with '\n').
 */
static int32_t editorCountChars(void) {
    struct editorDisplayCache *cache = &E.document.display_cache;
    if (!cache->chars_valid) {
        int32_t count = E.document.buffer.row_count > 0 ? E.document.buffer.row_count - 1 : 0;
        for (int32_t i = 0; i < E.document.buffer.row_count; i++) {
            int32_t addition = E.document.buffer.rows[i].grapheme_count;
            if (addition > INT32_MAX - count) { count = INT32_MAX; break; }
            count += addition;
        }
        cache->chars = count;
        cache->chars_valid = 1;
    }
    return cache->chars;
}

/**
 * @brief Look up the current document's readable language name.
 *
 * @details It performs no configuration writes during redraw.
 * @return a borrowed label or NULL when no extension is known.
 *
 * @note Filetype name for the status bar, derived from
 * E.document.file.filename's extension (e.g. "tinyedit.c" -> "C"), via the
 * built-in table plus any ~/.tinyeditrc "filetype.<ext> = <Name>" overrides.
 * Returns NULL if there's no filename, no extension, or the extension is
 * unknown -- callers should omit the field entirely rather than show a blank.
 */
static const char *editorFiletypeLabel(void) {
    if (!E.document.file.filename) return NULL;
    const char *dot = strrchr(E.document.file.filename, '.');
    /* No dot, or a dot with nothing after it (e.g. "Makefile",
     * "foo."): no extension to look up. A leading dot with no other
     * dot (e.g. ".gitignore") also has no meaningful extension. */
    if (!dot || dot[1] == '\0' || dot == E.document.file.filename) return NULL;
    return filetypeForExtension(dot + 1);
}

/**
 * @brief Append the optional document title bar.
 *
 * @details Shows filename and modified state using the configured bar colors;
 * does nothing when the top bar is disabled.
 *
 * @note Top title bar (optional, show_top_bar): basename + dirty
 * indicator. Kept separate from the bottom status bar, which shows transient
 * position/count info instead -- the top bar acts as a persistent title that
 * stays visible while scrolling.
 *
 * @note The bars reset the editor's background before applying their own
 * background/foreground pair. abAppendReset() restores the editor's background
 * after each bar, so a custom bar never leaks into text rows.
 */
static void editorDrawTopBar(struct abuf *ab) {
    if (!S.show_top_bar) return;

    if (S.color_background != COLOR_TERMINAL_DEFAULT) abAppend(ab, "\x1b[49m", 5);
    const char *bar_bg = ansiBgColorCode(S.color_statusbar);
    const char *bar_fg = ansiColorCode(S.color_statusbar_text);
    if (bar_bg[0]) abAppend(ab, bar_bg, (int32_t)strlen(bar_bg));
    abAppend(ab, bar_fg, (int32_t)strlen(bar_fg));

    const char *name = E.document.file.filename;
    if (name) {
        const char *slash = strrchr(name, '/');
        if (slash) name = slash + 1;
    }
    const char *parts[] = {
        name ? name : "[No Name]",
        E.document.file.dirty ? " (modified)" : ""
    };
    int32_t title_width = 0;
    for (size_t part = 0; part < sizeof(parts) / sizeof(parts[0]); part++) {
        size_t size = strlen(parts[part]), pos = 0;
        while (pos < size) {
            size_t len = utf8NextCharLen(parts[part], pos, size);
            int32_t width = utf8SingleCharWidth(parts[part] + pos, len);
            if (width > E.view.screencols - title_width) break;
            title_width += width;
            pos += len;
        }
        if (pos < size) break;
    }
    int32_t cols = (E.view.screencols - title_width) / 2;
    for (int32_t padding = 0; padding < cols; padding++) abAppend(ab, " ", 1);
    for (size_t part = 0; part < sizeof(parts) / sizeof(parts[0]); part++) {
        size_t size = strlen(parts[part]), pos = 0;
        while (pos < size) {
            size_t len = utf8NextCharLen(parts[part], pos, size);
            int32_t width = utf8SingleCharWidth(parts[part] + pos, len);
            if (width > E.view.screencols - cols) break;
            abAppend(ab, parts[part] + pos, (int32_t)len);
            cols += width;
            pos += len;
        }
        if (pos < size) break;
    }
    while (cols < E.view.screencols) {
        abAppend(ab, " ", 1);
        cols++;
    }
    abAppendReset(ab);
    abAppend(ab, "\r\n", 2);
}

/**
 * @brief Append document statistics and cursor position.
 *
 * @details Uses the current settings for colors and filename placement, then
 * restores the document background.
 */
static void editorDrawStatusBar(struct abuf *ab) {
    if (S.color_background != COLOR_TERMINAL_DEFAULT) abAppend(ab, "\x1b[49m", 5);
    const char *bar_bg = ansiBgColorCode(S.color_statusbar);
    const char *bar_fg = ansiColorCode(S.color_statusbar_text);
    if (bar_bg[0]) abAppend(ab, bar_bg, (int32_t)strlen(bar_bg));
    abAppend(ab, bar_fg, (int32_t)strlen(bar_fg));
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
    if (len < 0) len = 0;
    if ((size_t)len >= sizeof(status)) len = (int32_t)sizeof(status) - 1;
    if (rlen < 0) rlen = 0;
    if ((size_t)rlen >= sizeof(rstatus)) rlen = (int32_t)sizeof(rstatus) - 1;
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

/**
 * @brief Append the current prompt or status message.
 *
 * @details Respects message expiry and sticky messages; long active prompts
 * show the input tail so ongoing typing stays visible.
 */
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
    if (S.show_menu && !M.open && E.view.screencols >= 8) {
        char position[32];
        int32_t row = E.view.screenrows + (S.show_top_bar ? 1 : 0) +
            (S.show_menu ? 1 : 0) + 2;
        int32_t len = snprintf(position, sizeof(position), "\x1b[%d;%dH", row,
            E.view.screencols - 7);
        abAppend(ab, position, len);
        abAppend(ab, "F10 Menu", 8);
    }
}

/**
 * @brief Redraw the editor and position the terminal cursor.
 *
 * @details Handles pending resize, updates scrolling, assembles the frame and
 * writes it to stdout; also draws the menu overlay when open.
 */
static void editorRefreshScreen(void) {
    uint8_t need_full_clear = 0;
    if (winsize_changed) {
        winsize_changed = 0;
        int32_t rows, cols;
        if (terminalGetWindowSize(&rows, &cols) == 0) {
            E.view.screenrows = rows - 2 - (S.show_top_bar ? 1 : 0) -
                (S.show_menu ? 1 : 0); /* status/message bars plus optional top/menu bars */
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
    if (S.show_menu) menuDrawBar(&M, &S, E.view.screencols, editorMenuAppend, &ab);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    if (S.show_menu) menuDrawPopup(&M, &S, S.show_top_bar ? 2 : 1, E.view.screencols,
        editorMenuAppend, &ab);

    if (!M.open) {
        char buf[32];
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
            cursor_row + 1 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0),
            cursor_col + editorGutterWidth() + 1);
        abAppend(&ab, buf, (int32_t)strlen(buf));
        abAppend(&ab, "\x1b[?25h", 6);
    }

    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

/**
 * @brief Show a formatted message that expires after a short interval.
 *
 * @details fmt and its arguments follow printf conventions. Replaces the
 * previous message and disables sticky mode.
 */
static void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.ui.statusmsg, sizeof(E.ui.statusmsg), fmt, ap);
    va_end(ap);
    E.ui.statusmsg_time = time(NULL);
    E.ui.statusmsg_sticky = 0;
}

/**
 * @brief Show a formatted message until another message replaces it.
 *
 * @details fmt and its arguments follow printf conventions. Use for startup
 * guidance that should survive idle time.
 *
 * @note Like editorSetStatusMessage(), but the message stays in the message
 * bar until replaced by another call to either function -- no 5s timeout. Used
 * for the startup shortcut hint, which should remain visible until the user
 * does something that produces a real status update, not vanish on its own
 * after a few seconds.
 */
static void editorSetStatusMessageSticky(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.ui.statusmsg, sizeof(E.ui.statusmsg), fmt, ap);
    va_end(ap);
    E.ui.statusmsg_time = time(NULL);
    E.ui.statusmsg_sticky = 1;
}

/* ---- input ------------------------------------------------------------------ */

/**
 * @brief Find a source-byte cursor position for a local segment column.
 *
 * @details seg_from_rx and target_col are display columns. Stops at the first
 * character boundary reaching the target, or the row end.
 *
 * @note Converts an (rx, in-segment column) target on a given video row back
 * into a cx (char offset) on that row's logical line -- inverse of
 * editorRowCxToRx() restricted to one visual segment. Used by Up/Down in
 * wrapped mode to preserve the cursor's horizontal position when moving
 * between visual segments, same idea as unwrapped Up/Down preserving
 * E.document.cursor.rx via editorRowCxToRx()/clamping below.
 */
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

/**
 * @brief Move Up or Down by one visual row.
 *
 * @details key must be ARROW_UP or ARROW_DOWN and the cursor must address an
 * existing row. Recomputes its display column before each move.
 *
 * @note Up/Down cursor movement when soft-wrap is active: moves by one visual
 * segment instead of one logical row (see TODO.md -- decided to match modern
 * editor behavior instead of jumping whole paragraphs on a wrapped long line).
 * Preserves the target render column across segments/rows, same intent as the
 * unwrapped path preserving E.document.cursor.rx.
 */
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
    if (target_vy < 0) {
        E.document.cursor.cy = 0;
        E.document.cursor.cx = 0;
        return;
    }
    int32_t total = editorTotalVideoRows(wrapcols);
    if (target_vy >= total) {
        /* Match unwrapped Down at EOF: there is no following visual row,
         * so move to the logical line end. This lets Shift+Down select the
         * remainder of the last line instead of collapsing at its anchor. */
        E.document.cursor.cy = E.document.buffer.row_count - 1;
        E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy].size;
        return;
    }

    int32_t target_filerow, target_seg;
    editorFileRowAtVideoRow(target_vy, wrapcols, &target_filerow, &target_seg);

    erow *target_row = &E.document.buffer.rows[target_filerow];
    int32_t t_nseg = editorRowSegments(target_row, wrapcols);
    if (target_seg >= t_nseg) target_seg = t_nseg - 1;

    E.document.cursor.cy = target_filerow;
    E.document.cursor.cx = editorSegColToCx(target_row, target_row->seg_start_rx[target_seg], seg_col);
}

/**
 * @brief Move the cursor by a character or vertical row.
 *
 * @details key is an arrow key. Horizontal motion follows grapheme boundaries;
 * vertical motion uses visual segments when wrapping is active.
 */
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
            if (E.document.cursor.cy != 0) {
                E.document.cursor.cy--;
            } else {
                E.document.cursor.cx = 0;
            }
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

/**
 * @brief Move across whitespace and the adjoining word.
 *
 * @details forward selects the direction. Updates the byte-based cursor and
 * crosses a line boundary when already at its edge.
 */
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
        while (E.document.cursor.cx < row->size && isspace((unsigned char)row->chars[E.document.cursor.cx])) {
            size_t step = utf8NextCharLen(row->chars, (size_t)E.document.cursor.cx, (size_t)row->size);
            E.document.cursor.cx += (int32_t)(step ? step : 1);
        }
        while (E.document.cursor.cx < row->size && !isspace((unsigned char)row->chars[E.document.cursor.cx])) {
            size_t step = utf8NextCharLen(row->chars, (size_t)E.document.cursor.cx, (size_t)row->size);
            E.document.cursor.cx += (int32_t)(step ? step : 1);
        }
    } else {
        if (E.document.cursor.cx == 0) {
            if (E.document.cursor.cy > 0) {
                E.document.cursor.cy--;
                E.document.cursor.cx = E.document.buffer.rows[E.document.cursor.cy].size;
            }
            return;
        }
        erow *row = &E.document.buffer.rows[E.document.cursor.cy];
        int32_t i = E.document.cursor.cx;
        while (i > 0) {
            size_t step = utf8PrevCharLen(row->chars, (size_t)i);
            int32_t prev = i - (int32_t)(step ? step : 1);
            if (!isspace((unsigned char)row->chars[prev])) break;
            i = prev;
        }
        while (i > 0) {
            size_t step = utf8PrevCharLen(row->chars, (size_t)i);
            int32_t prev = i - (int32_t)(step ? step : 1);
            if (isspace((unsigned char)row->chars[prev])) break;
            i = prev;
        }
        E.document.cursor.cx = i;
    }
}

/**
 * @brief Copy a selected text range with LF separators.
 *
 * @details Coordinates are ordered, valid, zero-based rows and source-byte
 * offsets with an exclusive end.
 * @return owned NUL-terminated text; outlen excludes the NUL.
 */
static char *editorSerializeRange(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x, size_t *outlen) {
    return bufferSerializeRange(&E.document.buffer, start_y, start_x, end_y, end_x, outlen);
}

/**
 * @brief Remove a text range and leave the cursor at its start.
 *
 * @details Coordinates describe an ordered, valid half-open source range.
 * Joins rows and marks dirty; the caller records undo before calling.
 */
static void editorDeleteRangeRaw(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x) {
    if (historyFailed(&E.document)) return;
    editorBeginEdit();
    if (start_y == end_y) {
        erow *row = &E.document.buffer.rows[start_y];
        bufferRowDeleteRange(row, start_x, end_x);
        editorUpdateRow(row);
    } else {
        erow *first = &E.document.buffer.rows[start_y];
        bufferRowDeleteRange(first, start_x, first->size);

        erow *last = &E.document.buffer.rows[end_y];
        bufferRowAppend(first, &last->chars[end_x], (size_t)(last->size - end_x));
        editorUpdateRow(first);

        for (int32_t y = end_y; y > start_y; y--)
            bufferDeleteRow(&E.document.buffer, y);
        if (start_y + 1 < E.document.buffer.row_count)
            editorRehighlightFrom(start_y + 1, 0);
    }
    E.document.cursor.cy = start_y;
    E.document.cursor.cx = start_x;
    E.document.file.dirty = 1;
    editorEndEdit();
}

/**
 * @brief Delete a text range as a single undo action.
 *
 * @details Pass valid, ordered logical-row and source-byte endpoints; the end
 * is exclusive.
 */
static void editorDeleteRange(int32_t start_y, int32_t start_x, int32_t end_y, int32_t end_x) {
    editorBeginEdit();
    editorPushUndo(EDIT_OTHER);
    editorDeleteRangeRaw(start_y, start_x, end_y, end_x);
    editorEndEdit();
}

/**
 * @brief Replace a captured selection with Enter and inherited indentation.
 *
 * @details sy/sx and ey/ex are valid half-open source endpoints. Deletion,
 * splitting and indentation share one undo step; clears the selection.
 */
static void editorReplaceSelectionWithNewline(int32_t sy, int32_t sx,
    int32_t ey, int32_t ex) {
    editorBeginEdit();
    editorPushUndo(EDIT_OTHER);
    editorDeleteRangeRaw(sy, sx, ey, ex);

    int32_t src_row = E.document.cursor.cy;
    int32_t indent_len = 0;
    if (S.auto_indent && src_row < E.document.buffer.row_count) {
        erow *row = &E.document.buffer.rows[src_row];
        while (indent_len < row->size &&
               (row->chars[indent_len] == ' ' || row->chars[indent_len] == '\t'))
            indent_len++;
        if (indent_len > E.document.cursor.cx) indent_len = E.document.cursor.cx;
    }

    editorInsertNewlineRaw();
    if (indent_len > 0) {
        erow *row = &E.document.buffer.rows[src_row];
        editorRowInsertString(&E.document.buffer.rows[E.document.cursor.cy],
            E.document.cursor.cx, row->chars, (size_t)indent_len);
        E.document.cursor.cx += indent_len;
    }
    E.document.selection.active = 0;
    editorEndEdit();
}

/**
 * @brief Insert multiline bytes at the cursor without changing their indentation.
 *
 * @details text contains len bytes, split at LF. Advances the cursor and marks
 * dirty; the caller records undo for the whole action.
 */
static void editorInsertTextRaw(const char *text, size_t len) {
    editorBeginEdit();

    size_t start = 0;
    for (size_t i = 0; i <= len && !historyFailed(&E.document); i++) {
        if (i == len || text[i] == '\n') {
            size_t chunk_len = i - start;
            if (chunk_len > 0) {
                if (E.document.cursor.cy == E.document.buffer.row_count)
                    editorInsertRow(E.document.buffer.row_count, "", 0);
                if (historyFailed(&E.document)) break;
                editorRowInsertString(&E.document.buffer.rows[E.document.cursor.cy],
                    E.document.cursor.cx, text + start, chunk_len);
                E.document.cursor.cx += (int32_t)chunk_len;
            }
            if (i < len) editorInsertNewlineRaw();
            start = i + 1;
        }
    }
    editorEndEdit();
}

/**
 * @brief Insert bulk text, replacing the selection captured before dispatch.
 *
 * @details had_sel controls whether sy/sx..ey/ex is deleted. Deletion and
 * insertion share one undo step, and the selection is cleared afterward.
 */
static void editorReplaceSelectionWithText(uint8_t had_sel,
    int32_t sy, int32_t sx, int32_t ey, int32_t ex,
    const char *text, size_t len) {
    if (!had_sel && len == 0) return;
    editorBeginEdit();
    editorPushUndo(EDIT_OTHER);
    if (had_sel) editorDeleteRangeRaw(sy, sx, ey, ex);
    if (len > 0) editorInsertTextRaw(text, len);
    E.document.selection.active = 0;
    editorEndEdit();
}

/**
 * @brief Remove one level of leading indentation and refresh the row.
 *
 * @details Accepts a tab or up to the configured number of spaces, independent
 * of the insertion style.
 * @return the number of source bytes removed.
 *
 * @note Removes up to one indent level's worth of leading whitespace from
 * `row`, returning how many bytes went away. Mirrors what one indent step
 * inserts, but tolerantly: a single leading tab counts as a whole level
 * regardless of tab_stop, and otherwise up to tab_stop spaces are removed,
 * stopping early at the first non-space so a partially indented line loses
 * only what it actually has. Deliberately handles both tabs and spaces
 * whatever insert_spaces_for_tab says -- outdent has to cope with whatever
 * indentation the file already contains, not just the flavor this editor would
 * produce.
 */
static int32_t editorRowOutdent(erow *row) {
    int32_t removed = bufferRowOutdent(row, S.tab_stop);
    if (removed > 0) {
        editorUpdateRow(row);
        E.document.file.dirty = 1;
    }
    return removed;
}

/**
 * @brief Shift the selected lines by one indentation level.
 *
 * @details outdent chooses removal instead of insertion. Records one undo
 * action and adjusts both selection endpoints so repeated presses remain
 * useful.
 *
 * @note Shifts every line touched by the selection one indent level right
 * (`outdent` false) or left (true), as one undo step, keeping the selection
 * over the same lines afterward so the shortcut can be repeated. Blank lines
 * are skipped when indenting -- trailing whitespace on an otherwise empty line
 * is noise, and no editor that does block indent adds it.
 */
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

    editorBeginEdit();
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
                char *indent = teMalloc((size_t)S.tab_stop);
                memset(indent, ' ', (size_t)S.tab_stop);
                editorRowInsertString(row, 0, indent, (size_t)S.tab_stop);
                free(indent);
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
    editorEndEdit();
}

/* ---- search / replace ------------------------------------------------------- */

static void editorFindAndReplace(const char *query);

/**
 * @brief Expand supported control escapes in replacement text.
 *
 * @details raw is NUL-terminated.
 * @return owned NUL-terminated bytes with their count in out_len; unknown
 * escapes remain literal.
 *
 * @note In regex replacement text, translate the familiar control-character
 * escapes that can be typed into the single-line prompt. Unknown escapes
 * remain untouched, so a path or a future backreference-like sequence is never
 * silently damaged. The decoded form can contain real newlines and is
 * therefore returned with an explicit byte length.
 */
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

/* The search engine owns query compilation and returns source coordinates;
 * only this adapter applies a result to the active session and cursor. */
static uint8_t editorFindFrom(const char *query, int32_t from_y, int32_t from_x,
    int32_t dir, uint8_t wrap) {
    struct searchMatch match;
    searchQueryPrepare(&E.search.query, query, E.search.regex_mode);
    if (!searchFind(&E.search.query, &E.document.buffer, from_y, from_x, dir, wrap, &match)) {
        E.search.search_match_y = -1;
        E.search.search_match_end_y = -1;
        return 0;
    }
    E.document.cursor.cy = match.start_y;
    E.document.cursor.cx = match.start_x;
    E.search.search_match_y = match.start_y;
    E.search.search_match_x = match.start_x;
    E.search.search_match_len = match.length;
    E.search.search_match_end_y = match.end_y;
    E.search.search_match_end_x = match.end_x;
    return 1;
}

static void editorClearSearchNavigation(void) {
    E.search.last_cy = -1;
    E.search.last_cx = -1;
    E.search.last_end_y = -1;
    E.search.last_end_x = 0;
    E.search.last_len = 0;
}

static void editorRememberSearchMatch(void) {
    E.search.last_cy = E.search.search_match_y;
    E.search.last_cx = E.search.search_match_x;
    E.search.last_end_y = E.search.search_match_end_y;
    E.search.last_end_x = E.search.search_match_end_x;
    E.search.last_len = E.search.search_match_len;
}

/**
 * @brief Handle search-session events and apply engine results to the view.
 * @details Query ownership lives in E.search; Escape restores the saved view.
 */
static void editorFindCallback(char *query, int32_t key) {
    if (key == '\r' || key == '\x1b') {
        if (key == '\x1b') {
            E.document.cursor.cx = E.search.saved_cx;
            E.document.cursor.cy = E.search.saved_cy;
            E.view.rowoff = E.search.saved_rowoff;
            E.view.coloff = E.search.saved_coloff;
        }
        E.search.search_match_y = -1;
        E.search.search_match_end_y = -1;
        editorClearSearchNavigation();
        return;
    }
    if (key == CTRL_KEY('r')) {
        E.search.switch_to_replace = 1;
        return;
    }

    if (key == CTRL_KEY('g')) {
        E.search.regex_mode = !E.search.regex_mode;
        editorClearSearchNavigation();
    } else if (key == ARROW_DOWN || key == ARROW_RIGHT) {
        E.search.direction = 1;
    } else if (key == ARROW_UP || key == ARROW_LEFT) {
        E.search.direction = -1;
    } else {
        E.search.direction = 1;
        editorClearSearchNavigation();
    }
    if (!E.search.query.pattern || strcmp(E.search.query.pattern, query) != 0)
        editorClearSearchNavigation();
    searchQueryPrepare(&E.search.query, query, E.search.regex_mode);
    if (!*query || E.document.buffer.row_count == 0) {
        E.search.search_match_y = -1;
        E.search.search_match_end_y = -1;
        editorClearSearchNavigation();
        return;
    }

    int32_t from_y = E.search.saved_cy, from_x = E.search.saved_cx;
    if (E.search.last_cy >= 0 && E.search.direction == 1) {
        from_y = E.search.last_end_y;
        from_x = E.search.last_end_x;
        if (E.search.last_len == 0) {
            erow *row = &E.document.buffer.rows[from_y];
            if (from_x < row->size)
                from_x += (int32_t)utf8NextCharLen(row->chars, (size_t)from_x, (size_t)row->size);
            else from_x++;
        }
    } else if (E.search.last_cy >= 0 && E.search.last_cx > 0) {
        from_y = E.search.last_cy;
        from_x = E.search.last_cx - 1;
    } else if (E.search.last_cy >= 0) {
        /* A column-zero match must be excluded by moving to the previous row. */
        from_y = (E.search.last_cy - 1 + E.document.buffer.row_count) % E.document.buffer.row_count;
        from_x = E.document.buffer.rows[from_y].size;
    }
    if (editorFindFrom(query, from_y, from_x, E.search.direction, 1))
        editorRememberSearchMatch();
    else editorClearSearchNavigation();
}

/**
 * @brief Get the readable mode label for the search prompt.
 *
 * @return a borrowed literal reflecting the current per-search regex flag.
 *
 * @note Fed to editorPromptCB() as status_fn -- re-evaluated on every prompt
 * redraw, so the "[regex]"/"[literal]" indicator updates the instant Ctrl-G
 * toggles E.search.regex_mode, without editorFind() needing to rebuild the
 * whole prompt string itself.
 */
static const char *editorFindModeIndicator(void) {
    return E.search.regex_mode ? "[regex]" : "[literal]";
}

/**
 * @brief Run incremental search and optionally enter replacement.
 *
 * @details Starts in literal mode, saves the original view for cancellation
 * and uses the prompt callback for live navigation.
 */
static void editorFind(void) {
    E.search.saved_cx = E.document.cursor.cx;
    E.search.saved_cy = E.document.cursor.cy;
    E.search.saved_rowoff = E.view.rowoff;
    E.search.saved_coloff = E.view.coloff;
    E.search.direction = 1;
    E.search.regex_mode = 0;
    E.search.last_cy = -1;
    E.search.last_end_y = -1;

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
        editorFindModeIndicator, editorFindCallback, 0);

    if (E.search.switch_to_replace && query) {
        editorFindAndReplace(query);
    }

    if (query) free(query);
    searchQueryFree(&E.search.query);
}

/**
 * @brief Walk matches once and ask which occurrences to replace.
 *
 * @details query is borrowed from the search prompt and uses its current regex
 * mode. All accepted replacements share one undo step; inserted matches are
 * not revisited.
 */
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
    char *replacement_input = editorPromptCB(replace_prompt_long, replace_prompt_short, NULL, NULL, 0);
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
        int32_t end_y = E.search.search_match_end_y;
        int32_t end_x = E.search.search_match_end_x;
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
            editorBeginEdit();
            if (count == 0) {
                editorPushUndo(EDIT_OTHER);
                E.document.history.hold = 1;
            }
            editorDeleteRangeRaw(y, x, end_y, end_x);
            editorInsertTextRaw(replacement, rlen);
            editorEndEdit();
            if (edit_last_error != HISTORY_OK) { count = 0; break; }
            count++;
            y = E.document.cursor.cy;
            x = E.document.cursor.cx;
        } else {
            y = end_y;
            x = end_x;
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
    E.search.search_match_end_y = -1;
    free(replacement);
    E.document.history.hold = 0;
    historyFinishEdit(&E.document);
    if (edit_last_error == HISTORY_OK) {
        if (E.document.history.dropped_count)
            editorSetStatusMessage("Replaced %d occurrence(s). Undo limit: %d oldest action(s) removed.",
                count, E.document.history.dropped_count);
        else
            editorSetStatusMessage("Replaced %d occurrence(s).", count);
    }
}

/* ---- settings screen (F2) --------------------------------------------------- */

/**
 * @brief Access a setting field through its descriptor offset.
 *
 * @details s is the editable settings copy and d must describe one of its
 * int32_t fields.
 * @return a borrowed writable slot; equal field sizes are essential.
 */
static int32_t *settingsScreenSlot(struct editorSettings *s, const struct settingDescriptor *d) {
    return (int32_t *)((char *)s + d->offset);
}

/**
 * @brief Recognize a syntax-color option for grouping and visibility.
 *
 * @details d is a valid setting descriptor.
 * @return 1 when its key has the color_syntax_ prefix.
 */
static uint8_t editorSettingsIsSyntaxColor(const struct settingDescriptor *d) {
    return strncmp(d->key, "color_syntax_", strlen("color_syntax_")) == 0;
}

/**
 * @brief Choose preview text appropriate to a syntax color.
 *
 * @return a borrowed sample literal for d, with a generic fallback for
 * unfamiliar syntax keys.
 */
static const char *editorSettingsSyntaxColorSample(const struct settingDescriptor *d) {
    if (strcmp(d->key, "color_syntax_normal") == 0) return "text";
    if (strcmp(d->key, "color_syntax_keyword") == 0) return "printf";
    if (strcmp(d->key, "color_syntax_json_key") == 0) return "\"title\":";
    if (strcmp(d->key, "color_syntax_bracket") == 0) return "() [] {}";
    if (strcmp(d->key, "color_syntax_string") == 0) return "\"hello\"";
    if (strcmp(d->key, "color_syntax_comment") == 0) return "// note";
    if (strcmp(d->key, "color_syntax_number") == 0) return "42";
    if (strcmp(d->key, "color_syntax_preprocessor") == 0) return "#include";
    if (strcmp(d->key, "color_syntax_emphasis_strong") == 0) return "**bold**";
    if (strcmp(d->key, "color_syntax_math") == 0) return "$x^2$";
    if (strcmp(d->key, "color_syntax_function") == 0) return "main()";
    return "sample";
}

/**
 * @brief Choose preview text for non-syntax color options.
 *
 * @return a borrowed sample for gutter or invisible-character colors,
 * otherwise NULL.
 */
static const char *editorSettingsPlainColorSample(const struct settingDescriptor *d) {
    if (strcmp(d->key, "color_gutter") == 0) return " 42 ";
    if (strcmp(d->key, "color_invisibles") == 0) return ". > $";
    return NULL;
}

/**
 * @brief Recognize an enum setting that uses the color palette.
 *
 * @return 1 only for enum descriptors whose keys begin with color_.
 *
 * @note Any setting whose value is one of enum settingColor -- i.e. every
 * "color_*" key, syntax ones included. Recognized by key prefix rather than by
 * comparing d->enum_names against colorNames, since that array is file-local
 * to settings.c.
 */
static uint8_t editorSettingsIsColor(const struct settingDescriptor *d) {
    return d->type == SETTING_ENUM &&
        strncmp(d->key, "color_", strlen("color_")) == 0;
}

/**
 * @brief Count options currently available in the settings panel.
 *
 * @details edited is the draft settings copy. Syntax-color options are hidden
 * when its highlighting switch is off.
 */
static int32_t editorSettingsVisibleCount(const struct editorSettings *edited) {
    int32_t count = 0;
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        if (edited->syntax_highlight || !editorSettingsIsSyntaxColor(&settingDescriptors[i]))
            count++;
    }
    return count;
}

/**
 * @brief Resolve a visible panel index to the descriptor table.
 *
 * @details visible_idx is zero-based and uses the current edited settings.
 * @return the descriptor index, or -1 if no visible entry exists.
 */
static int32_t editorSettingsDescriptorAt(const struct editorSettings *edited, int32_t visible_idx) {
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        if (!edited->syntax_highlight && editorSettingsIsSyntaxColor(&settingDescriptors[i]))
            continue;
        if (visible_idx-- == 0) return i;
    }
    return -1;
}

/**
 * @brief Measure the alignment width for visible option labels.
 *
 * @return the width used by the settings panel, including indentation for
 * grouped syntax colors.
 */
static int32_t editorSettingsLabelWidth(const struct editorSettings *edited) {
    int32_t widest = 0;
    for (int32_t i = 0; i < settingDescriptorCount; i++) {
        const struct settingDescriptor *d = &settingDescriptors[i];
        if (!edited->syntax_highlight && editorSettingsIsSyntaxColor(d)) continue;
        uint8_t markdown_child = strncmp(d->label, "Markdown: ", 10) == 0;
        const char *label = editorSettingsIsSyntaxColor(d) ?
            d->label + strlen("Syntax: ") : markdown_child ? d->label + 10 : d->label;
        int32_t width = (int32_t)strlen(label) +
            (editorSettingsIsSyntaxColor(d) || markdown_child ? 3 : 1);
        if (width > widest) widest = width;
    }
    return widest;
}

/**
 * @brief Step through an enum setting with wraparound.
 *
 * @details delta is +1 or -1 and slot is the editable value. Background
 * pickers skip dim variants that look identical to dark ones.
 *
 * @note Steps a SETTING_ENUM value by `delta` (+1/-1), wrapping at both ends.
 * On color_background it skips the 8 "-dim" hues: the dim attribute is
 * foreground-only, so as a background each renders identically to its "-dark"
 * twin (see settingColorIsDim()) and offering both just makes the picker look
 * broken -- you cycle, the swatch doesn't change. A -dim value already in
 * ~/.tinyeditrc still loads and renders fine; stepping away from it lands on a
 * non-dim one and can't come back.
 */
static void editorSettingsCycleEnum(const struct settingDescriptor *d, int32_t *slot, int32_t delta) {
    uint8_t skip_dim = strcmp(d->key, "color_background") == 0 ||
        strcmp(d->key, "color_statusbar") == 0;
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

/**
 * @brief Append one settings entry with its value and preview.
 *
 * @details idx is a descriptor-table index; selected controls highlighting.
 * scroll_indicator is '^', 'v' or NUL, and label_width aligns values.
 *
 * @note `scroll_indicator` is '^' when this row is the topmost visible one and
 * there are more settings scrolled off above, 'v' when it's the bottommost
 * visible one and there are more below, or '\0' for no indicator -- drawn in
 * the first column (like the line-number gutter) so it's visible regardless of
 * which row is selected.
 */
static void editorSettingsDrawRow(struct abuf *ab, int32_t idx, uint8_t selected,
    const struct editorSettings *edited, char scroll_indicator, int32_t label_width) {
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

    uint8_t syntax_color = editorSettingsIsSyntaxColor(d);
    uint8_t markdown_child = strncmp(d->label, "Markdown: ", 10) == 0;
    const char *label = syntax_color ? d->label + strlen("Syntax: ") :
        markdown_child ? d->label + 10 : d->label;
    int32_t indent = syntax_color || markdown_child ? 3 : 1;
    int32_t len = 0;
    line[len++] = scroll_indicator ? scroll_indicator : ' ';
    while (indent-- > 0 && len < (int32_t)sizeof(line) - 1) line[len++] = ' ';
    for (int32_t i = 0; label[i] && len < (int32_t)sizeof(line) - 1; i++)
        line[len++] = label[i];
    int32_t dots = label_width - ((syntax_color || markdown_child ? 3 : 1) + (int32_t)strlen(label)) + 2;
    while (dots-- > 0 && len < (int32_t)sizeof(line) - 1) line[len++] = '.';
    for (int32_t i = 0; valuebuf[i] && len < (int32_t)sizeof(line) - 1; i++)
        line[len++] = valuebuf[i];

    if (selected) abAppend(ab, "\x1b[7m", 4);
    abAppend(ab, line, len);
    if (selected) abAppend(ab, "\x1b[m", 3);

    /* Live preview after the value name: the palette has 24 hues whose
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
        if (editorSettingsIsSyntaxColor(d)) {
            const char *fg = ansiColorCode(*slot);
            const char *bg = ansiBgColorCode(edited->color_background);
            const char *sample = editorSettingsSyntaxColorSample(d);
            abAppend(ab, "  ", 2);
            if (bg[0]) abAppend(ab, bg, (int32_t)strlen(bg));
            abAppend(ab, fg, (int32_t)strlen(fg));
            abAppend(ab, sample, (int32_t)strlen(sample));
            abAppend(ab, "\x1b[m", 3);
        } else if (strcmp(d->key, "color_selection") == 0) {
            const char *fg = ansiColorCode(*slot);
            const char *bg = ansiBgColorCode(edited->color_background);
            abAppend(ab, "  ", 2);
            if (bg[0]) abAppend(ab, bg, (int32_t)strlen(bg));
            abAppend(ab, fg, (int32_t)strlen(fg));
            abAppend(ab, "\x1b[7mselected\x1b[m", 15);
        } else if (strcmp(d->key, "color_statusbar") == 0 ||
            strcmp(d->key, "color_statusbar_text") == 0) {
            int32_t bg_color = strcmp(d->key, "color_statusbar") == 0 ?
                *slot : edited->color_statusbar;
            int32_t fg_color = strcmp(d->key, "color_statusbar") == 0 ?
                edited->color_statusbar_text : *slot;
            const char *bg = ansiBgColorCode(bg_color);
            const char *fg = ansiColorCode(fg_color);
            abAppend(ab, "  ", 2);
            if (bg[0]) abAppend(ab, bg, (int32_t)strlen(bg));
            abAppend(ab, fg, (int32_t)strlen(fg));
            abAppend(ab, " status ", 8);
            abAppend(ab, "\x1b[m", 3);
        } else if (editorSettingsPlainColorSample(d)) {
            const char *fg = ansiColorCode(*slot);
            const char *bg = ansiBgColorCode(edited->color_background);
            const char *sample = editorSettingsPlainColorSample(d);
            abAppend(ab, "  ", 2);
            if (bg[0]) abAppend(ab, bg, (int32_t)strlen(bg));
            abAppend(ab, fg, (int32_t)strlen(fg));
            abAppend(ab, sample, (int32_t)strlen(sample));
            abAppend(ab, "\x1b[m", 3);
        } else if (strcmp(d->key, "color_background") == 0) {
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

/**
 * @brief Show the scrollable shortcut and configuration reference.
 *
 * @details Runs its own input loop until dismissal and adapts shortcut labels
 * to the current modifier mode.
 *
 * @note Full-screen settings overlay (F2). Edits a local copy of the live
 * settings so Esc can discard changes cleanly; Ctrl-S writes the copy to
 * ~/.tinyeditrc and makes it live. Reuses the same raw-mode input loop style
 * as the rest of the editor (editorReadKey + a per-frame abuf redraw) rather
 * than pulling in any new input machinery.
 *
 * @note Static keybinding reference shown by F1. One entry per line; NULL
 * marks a section header (rendered bold/inverse instead of key+desc). Kept as
 * a flat array rather than scattered doc-comments so this is the one place to
 * update when a keybinding changes -- easy to miss a case in
 * editorProcessKeypress() otherwise.
 *
 * @note Full-screen static help overlay (F1). No editable state, so unlike
 * editorSettingsScreen() this doesn't need a local copy or Ctrl-S -- any key
 * closes it. Scrolls with Up/Down/PageUp/PageDown if the keybinding list is
 * taller than the terminal.
 */
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

/**
 * @brief Append a formatted Info row if screen space remains.
 *
 * @details rows_used tracks occupied rows and is incremented only when output
 * is appended. fmt follows printf conventions.
 */
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

/**
 * @brief Append a highlighted section heading to the Info screen.
 *
 * @details title is NUL-terminated; rows_used is shared with the other Info
 * helpers to avoid overflowing the viewport.
 */
static void editorInfoAppendSection(struct abuf *ab, int32_t *rows_used, const char *title) {
    const char *section_prefix = "\x1b[1m  ";
    abAppend(ab, section_prefix, (int32_t)strlen(section_prefix));
    abAppend(ab, title, (int32_t)strlen(title));
    abAppend(ab, "\x1b[m\x1b[K\r\n", 8);
    (*rows_used)++;
}

/**
 * @brief Append a blank Info row if space remains.
 *
 * @details Updates rows_used only when the row fits in the available screen
 * height.
 */
static void editorInfoAppendBlank(struct abuf *ab, int32_t *rows_used) {
    abAppend(ab, "\x1b[K\r\n", 5);
    (*rows_used)++;
}

/**
 * @brief Show project information and statistics for the active file.
 *
 * @details Chooses a new slogan, draws the overlay and waits for a key before
 * returning to editing.
 *
 * @note Full-screen static overlay (F3): project identity (version, author,
 * license, homepage) plus live stats about the file currently open -- kept as
 * one screen rather than splitting "about tinyedit" from "about this file"
 * into two separate keys, since both are "information, not action" in the same
 * spirit and a user reaching for one is likely to want the other close by. No
 * editable state, so like editorHelpScreen() any key closes it -- this only
 * reads E/S, never writes them.
 */
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

/**
 * @brief Calculate how many option rows fit in the settings panel.
 *
 * @return at least one row after allowing room for the panel header and
 * footer.
 *
 * @note Renders one frame of the F2 panel into `ab` -- factored out of
 * editorSettingsScreen()'s main loop so editorSettingsEditInt() can redraw the
 * same panel underneath its own inline numeric prompt, instead of falling
 * through to editorPrompt()/editorRefreshScreen() which draws the main text
 * buffer (the bug this fixes: typing a new value for
 * tab_stop/undo_max_depth/soft_wrap used to flash the editor's own screen,
 * with the file content briefly visible, because editorPrompt() only knows how
 * to redraw the main editor view).
 *
 * @note Number of setting rows that fit on screen at once, below the 2-row
 * header and above the blank/note/help rows at the bottom (3 rows reserved for
 * those, matching what editorSettingsRender() always writes after the option
 * list -- the Ctrl-Shift-Z note is the only conditional one and is
 * deliberately not accounted for here, so the reserved space is a safe upper
 * bound rather than something that shifts the visible row count depending on
 * redo_key). Shared between the renderer and the scroll-clamping logic in
 * editorSettingsScreen()/editorSettingsEditInt() so both agree on exactly how
 * many rows are visible.
 */
static int32_t editorSettingsVisibleRows(void) {
    int32_t visible = E.view.screenrows - 3;
    return visible > 0 ? visible : 1;
}

/**
 * @brief Append the settings panel using the current draft values.
 *
 * @details cursor and scroll are indices in the visible options, not
 * descriptor indices. msg may be NULL; the caller writes and frees ab.
 */
static void editorSettingsRender(struct abuf *ab, const struct editorSettings *edited,
    int32_t cursor, int32_t scroll, const char *msg) {
    abAppend(ab, "\x1b[?25l\x1b[H", 9);
    int32_t rows_used = 0;

    abAppend(ab, "\x1b[7m Settings \x1b[m\x1b[K\r\n\x1b[K\r\n", 27);
    rows_used += 2;

    int32_t visible = editorSettingsVisibleRows();
    int32_t count = editorSettingsVisibleCount(edited);
    int32_t label_width = editorSettingsLabelWidth(edited);
    int32_t last_visible = scroll + visible - 1;
    if (last_visible >= count) last_visible = count - 1;
    for (int32_t i = scroll; i < count && i < scroll + visible; i++) {
        char scroll_indicator = '\0';
        if (i == scroll && scroll > 0) scroll_indicator = '^';
        else if (i == last_visible && last_visible < count - 1) scroll_indicator = 'v';
        editorSettingsDrawRow(ab, editorSettingsDescriptorAt(edited, i), i == cursor,
            edited, scroll_indicator, label_width);
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

/**
 * @brief Read a numeric setting while keeping the panel visible.
 *
 * @details d supplies the accepted range; cursor and scroll preserve the panel
 * view.
 * @return 1 with a clamped value in out on acceptance, or 0 on cancellation.
 *
 * @note Inline numeric input for a SETTING_INT field, redrawing the F2 panel
 * (via editorSettingsRender()) on every keystroke instead of handing off to
 * editorPrompt(), which only knows how to redraw the main editor screen
 * underneath. Returns 1 and writes *out on Enter with a non-empty value, 0 on
 * Esc (value unchanged).
 */
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

/**
 * @brief Apply draft settings to the live editor and persist them.
 *
 * @details Updates row caches, terminal modes and layout as needed. Live
 * changes are applied even if writing ~/.tinyeditrc fails; the result is
 * reported in the status bar.
 */
static void editorSettingsSave(const struct editorSettings *edited) {
    struct editorSettings previous = S;
#ifdef __APPLE__
    uint8_t ghostty_bindings_ok = 1;
#endif
    S = *edited;
    if (previous.color_background != COLOR_TERMINAL_DEFAULT &&
        S.color_background == COLOR_TERMINAL_DEFAULT)
        write(STDOUT_FILENO, "\x1b[49m", 5);
    if (S.show_top_bar != previous.show_top_bar || S.show_menu != previous.show_menu)
        winsize_changed = 1;
    if (S.tab_stop != previous.tab_stop ||
        S.show_invisibles != previous.show_invisibles ||
        S.syntax_highlight != previous.syntax_highlight)
        editorUpdateAllRows();
    if (S.mouse_enabled != previous.mouse_enabled) {
        if (S.mouse_enabled) terminalEnableMouseReporting();
        else terminalDisableMouseReporting();
    }
#ifdef __APPLE__
    if (S.mac_command_keys != previous.mac_command_keys) {
        if (S.mac_command_keys) {
            terminalEnableKittyKeyboard();
            ghostty_bindings_ok = terminalConfigureGhosttyCommandBindings(1);
        } else {
            terminalDisableKittyKeyboard();
            ghostty_bindings_ok = terminalConfigureGhosttyCommandBindings(0);
        }
    }
#endif
    if (settingsSave(&S)) {
#ifdef __APPLE__
        if (ghostty_bindings_ok) {
            editorSetStatusMessage("Settings saved to ~/.tinyeditrc");
        } else {
            editorSetStatusMessage("Settings saved; Ghostty config/reload failed");
        }
#else
        editorSetStatusMessage("Settings saved to ~/.tinyeditrc");
#endif
    } else {
        editorSetStatusMessage("Could not write ~/.tinyeditrc");
    }
}

/**
 * @brief Edit settings in a local draft with save and discard choices.
 *
 * @details Owns the F2 input loop. Changes become live only through
 * editorSettingsSave(); leaving a changed draft offers save, discard or
 * cancellation.
 */
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

/**
 * @brief Test the current filename's extension without case sensitivity.
 *
 * @details extension is NUL-terminated without a leading dot.
 * @return 0 for unnamed files or absent extensions.
 */
static uint8_t editorFilenameHasExtension(const char *extension) {
    if (!E.document.file.filename) return 0;
    const char *dot = strrchr(E.document.file.filename, '.');
    if (!dot || dot[1] == '\0') return 0;

    const char *actual = dot + 1;
    while (*actual && *extension) {
        if (tolower((unsigned char)*actual) != tolower((unsigned char)*extension)) return 0;
        actual++;
        extension++;
    }
    return *actual == '\0' && *extension == '\0';
}

/**
 * @brief Decide whether the current file supports tag auto-closing.
 *
 * @return 1 for XML, HTML or HTM extensions, with case-insensitive matching.
 */
static uint8_t editorIsXmlTagFile(void) {
    return editorFilenameHasExtension("xml") || editorFilenameHasExtension("html") ||
        editorFilenameHasExtension("htm");
}

/**
 * @brief Decide whether HTML void-element rules apply.
 *
 * @return 1 for HTML or HTM; XML files deliberately do not use those
 * exceptions.
 */
static uint8_t editorIsHtmlFile(void) {
    return editorFilenameHasExtension("html") || editorFilenameHasExtension("htm");
}

/**
 * @brief Recognize a name opener supported by the tag-closing heuristic.
 *
 * @details byte is an unsigned input byte.
 * @return 1 for a letter, underscore or colon; this is not a full XML name
 * validator.
 */
static uint8_t editorIsXmlNameStart(uint8_t byte) {
    return isalpha(byte) || byte == '_' || byte == ':';
}

/**
 * @brief Recognize a continuation byte supported in a tag name.
 *
 * @details Accepts the opener characters plus digits, hyphen and dot; used
 * only by the lightweight tag-closing scan.
 */
static uint8_t editorIsXmlNameByte(uint8_t byte) {
    return editorIsXmlNameStart(byte) || isdigit(byte) || byte == '-' || byte == '.';
}

/**
 * @brief Recognize an HTML element that must not receive an end tag.
 *
 * @details name is a byte span of length len, with no terminator required.
 * @return 1 on a case-insensitive match with the built-in void list.
 */
static uint8_t editorIsHtmlVoidTag(const char *name, int32_t len) {
    const int32_t void_tag_count = (int32_t)(sizeof(void_tags) / sizeof(void_tags[0]));

    for (int32_t i = 0; i < void_tag_count; i++) {
        size_t tag_len = strlen(void_tags[i]);
        if ((size_t)len != tag_len) continue;
        int32_t j;
        for (j = 0; j < len; j++)
            if (tolower((unsigned char)name[j]) != void_tags[i][j]) break;
        if (j == len) return 1;
    }
    return 0;
}

/**
 * @brief Insert an end tag after a just-typed opening tag.
 *
 * @details Call after inserting '>' and recording undo. Leaves the cursor
 * between tags, skipping self-closing tags and HTML void elements.
 */
static void editorTryAutoCloseXmlTag(void) {
    if (!editorIsXmlTagFile() || E.document.cursor.cy >= E.document.buffer.row_count) return;

    erow *row = &E.document.buffer.rows[E.document.cursor.cy];
    int32_t close = E.document.cursor.cx - 1;
    if (close <= 0 || close >= row->size || row->chars[close] != '>') return;

    int32_t before_close = close;
    while (before_close > 0) {
        size_t clen = utf8PrevCharLen(row->chars, (size_t)before_close);
        if (clen == 0) return;
        int32_t prev = before_close - (int32_t)clen;
        if (!isspace((unsigned char)row->chars[prev])) break;
        before_close = prev;
    }
    if (before_close > 0 && row->chars[before_close - 1] == '/') return;

    int32_t open = -1;
    int32_t scan = close;
    while (scan > 0) {
        size_t clen = utf8PrevCharLen(row->chars, (size_t)scan);
        if (clen == 0) return;
        int32_t prev = scan - (int32_t)clen;
        if (row->chars[prev] == '<') {
            open = prev;
            break;
        }
        scan = prev;
    }
    if (open < 0) return;

    int32_t name_start = open + 1;
    if (name_start >= close || !editorIsXmlNameStart((uint8_t)row->chars[name_start])) return;
    int32_t name_end = name_start;
    while (name_end < close && editorIsXmlNameByte((uint8_t)row->chars[name_end])) {
        size_t clen = utf8NextCharLen(row->chars, (size_t)name_end, (size_t)row->size);
        if (clen == 0) return;
        name_end += (int32_t)clen;
    }
    if (editorIsHtmlFile() && editorIsHtmlVoidTag(row->chars + name_start, name_end - name_start)) return;

    int32_t name_len = name_end - name_start;
    int32_t closing_len = name_len + 3;
    char *closing = teMalloc((size_t)closing_len);
    closing[0] = '<';
    closing[1] = '/';
    memcpy(closing + 2, row->chars + name_start, (size_t)name_len);
    closing[closing_len - 1] = '>';
    editorRowInsertString(row, E.document.cursor.cx, closing, (size_t)closing_len);
    free(closing);
}

/**
 * @brief Collect the continuation bytes of a just-read UTF-8 lead.
 *
 * @details out must hold at least four bytes; no NUL is appended.
 * @return the bytes collected and queues a non-continuation event for later
 * dispatch.
 */
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

/**
 * @brief Move past a matching Unicode closer instead of inserting it.
 *
 * @details typed supplies typed_len bytes.
 * @return 1 only for a known closer already after the cursor; otherwise leaves
 * text and cursor untouched.
 *
 * @note If the bytes right after the cursor equal one of
 * autoCloseMultiByteTable's close sequences AND that's exactly what was just
 * typed (`typed`/`typed_len`), moves the cursor past it and returns 1 without
 * touching the buffer. Returns 0 otherwise, leaving the caller to insert
 * `typed` normally -- this is what makes it safe to call unconditionally
 * instead of matching on buffer content alone (which would skip over existing
 * text regardless of what key was actually pressed).
 */
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

/**
 * @brief Handle typed text with pair insertion, wrapping and closer skipping.
 *
 * @details c is the first input byte. had_sel and the source-byte endpoints
 * preserve the selection captured before dispatch; bracketed paste uses bulk
 * insertion instead.
 */
static void editorApplyAutoClose(int32_t c, uint8_t had_sel,
    int32_t sel_y0, int32_t sel_x0, int32_t sel_y1, int32_t sel_x1) {
    if (!S.auto_close_pairs) {
        editorInsertChar(c);
        return;
    }

    if (c == '>' && !had_sel) {
        editorPushUndo(EDIT_OTHER);
        editorInsertChar(c);
        editorTryAutoCloseXmlTag();
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

            editorPushUndo(EDIT_OTHER);
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

    const struct autoClosePair *pair = editorAutoClosePairFor(c, S.auto_close_single_quote != 0);

    if (pair && had_sel) {
        editorPushUndo(EDIT_OTHER);
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
                editorPushUndo(EDIT_OTHER);
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
        editorPushUndo(EDIT_OTHER);
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
        editorPushUndo(EDIT_OTHER);
        editorInsertChar(c);
        editorInsertChar((unsigned char)pair->close);
        E.document.cursor.cx--;
        return;
    }

    /* Not an opener -- check whether it's the closer of an asymmetric
     * pair, for skip-over (e.g. typing ')' right before an
     * auto-inserted ')'). */
    if (editorIsAsymmetricAutoClose(c) && E.document.cursor.cy < E.document.buffer.row_count) {
        erow *row = &E.document.buffer.rows[E.document.cursor.cy];
        if (E.document.cursor.cx < row->size && row->chars[E.document.cursor.cx] == c) {
            E.document.cursor.cx++;
            return;
        }
    }

    editorInsertChar(c);
}

static void editorInsertCharAutoClose(int32_t c, uint8_t had_sel,
    int32_t sel_y0, int32_t sel_x0, int32_t sel_y1, int32_t sel_x1) {
    editorBeginEdit();
    editorApplyAutoClose(c, had_sel, sel_y0, sel_x0, sel_y1, sel_x1);
    editorEndEdit();
}

/**
 * @brief Translate a menu command into the existing keyboard action.
 *
 * @return a dispatch key or zero for a command without a key equivalent;
 * setting toggles use a separate path.
 */
static int32_t editorMenuCommandKey(enum editorCommand command) {
    switch (command) {
        case CMD_INFO: return F3_KEY;
        case CMD_SETTINGS: return F2_KEY;
        case CMD_QUIT: return CTRL_KEY('q');
        case CMD_OPEN: return CTRL_KEY('o');
        case CMD_SAVE: return CTRL_KEY('s');
        case CMD_SAVE_AS: return F4_KEY;
        case CMD_CLOSE: return CTRL_KEY('w');
        case CMD_UNDO: return CTRL_KEY('z');
        case CMD_REDO: return CTRL_KEY('y');
        case CMD_CUT: return CTRL_KEY('x');
        case CMD_COPY: return CTRL_KEY('c');
        case CMD_PASTE: return CTRL_KEY('v');
        case CMD_SELECT_ALL: return CTRL_KEY('a');
        case CMD_FIND: return CTRL_KEY('f');
        case CMD_HELP: return F1_KEY;
        default: return 0;
    }
}

/**
 * @brief Apply and persist a View-menu setting change.
 *
 * @details command must identify a boolean setting. Uses the shared settings
 * update path and closes a menu that has just been disabled.
 */
static void editorToggleMenuSetting(enum editorCommand command) {
    struct editorSettings edited = S;
    if (!commandToggleSetting(command, &edited)) return;
    if (command == CMD_TOGGLE_MENU && !edited.show_menu) M.open = 0;
    editorSettingsSave(&edited);
}

/**
 * @brief Keep terminal hover reporting aligned with the open menu.
 *
 * @details Call after menu state changes. Emits a mode change only when the
 * required reporting state differs from the previous one.
 */
static void editorSyncMenuMouseMotion(void) {
    uint8_t enabled = S.mouse_enabled && S.show_menu && M.open;
    if (enabled == menu_mouse_motion_enabled) return;
    terminalSetMenuMouseMotion(enabled);
    menu_mouse_motion_enabled = enabled;
}

/**
 * @brief Release a document's transient mouse gesture without changing selection.
 */
static void editorCancelMouseDrag(void) {
    E.document.mouse = (struct editorMouseState){0};
}

/**
 * @brief Apply one decoded mouse event or return its accepted menu command.
 * @details Never reads the next event. Every report, including reports within
 * a burst, passes through the same menu/text routing and release handling.
 */
static int32_t editorHandleMouseEvent(void) {
    if (!mouseEventPress) editorCancelMouseDrag();
    int32_t message_row = E.view.screenrows + (S.show_top_bar ? 1 : 0) +
        (S.show_menu ? 1 : 0) + 2;
    if (S.show_menu && !M.open && mouseEventPress && mouseEventRow == message_row &&
        mouseEventCol >= E.view.screencols - 7) {
        editorCancelMouseDrag();
        menuOpen(&M);
        editorSyncMenuMouseMotion();
        return 0;
    }
    if (S.show_menu && M.open && !mouseEventPress &&
        mouseEventRow == message_row && mouseEventCol >= E.view.screencols - 7)
        return 0;
    if (S.show_menu && (M.open ||
        (mouseEventPress && mouseEventRow == (S.show_top_bar ? 2 : 1)))) {
        editorCancelMouseDrag();
        enum editorCommand command = menuHandleMouse(&M,
            mouseEventRow, mouseEventCol, S.show_top_bar ? 2 : 1, E.view.screencols,
            mouseEventPress, (mouseEventButton & 32) != 0);
        editorSyncMenuMouseMotion();
        if (commandIsSetting(command)) editorToggleMenuSetting(command);
        else if (command != CMD_NONE) return editorMenuCommandKey(command);
        return 0;
    }
    uint8_t shift_held = (mouseEventButton & 4) != 0;
    int32_t mouse_button = mouseEventButton & ~28;
    if (mouse_button == 64 || mouse_button == 65) {
        /* Wheel gestures move the view independently of cursor and selection. */
        int32_t wrapcols = editorSoftWrapCols();
        int32_t delta = (mouse_button == 64) ? -3 : 3;
        int32_t limit = wrapcols > 0 ? editorTotalVideoRows(wrapcols) : E.document.buffer.row_count;
        E.view.rowoff += delta;
        if (E.view.rowoff < 0) E.view.rowoff = 0;
        if (E.view.rowoff > limit) E.view.rowoff = limit;
        /* Skip cursor-following for this redraw; a following keyboard event
         * clears this override before dispatch, even within the same burst. */
        E.view.free_scroll = 1;
    } else {
        uint8_t in_text_area = mouseEventRow >= 1 + (S.show_top_bar ? 1 : 0) +
            (S.show_menu ? 1 : 0) && mouseEventRow <= 1 +
            (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0) + E.view.screenrows - 1 &&
            mouseEventCol > editorGutterWidth();

        if (mouseEventPress && mouse_button == 0) editorCancelMouseDrag();
        if (in_text_area && mouse_button == 0 && mouseEventPress) {
            /* A plain click remembers a local drag anchor without
             * arming a collapsed selection; Shift-click instead
             * retains its existing anchor, or starts from the
             * cursor position immediately before the click. */
            int32_t cy, cx;
            editorMouseToCursor(mouseEventCol, mouseEventRow, &cy, &cx);
            if (shift_held) {
                if (!E.document.selection.active) {
                    E.document.selection.active = 1;
                    E.document.selection.anchor_x = E.document.cursor.cx;
                    E.document.selection.anchor_y = E.document.cursor.cy;
                }
            } else {
                E.document.selection.active = 0;
                E.document.mouse.press_anchor_x = cx;
                E.document.mouse.press_anchor_y = cy;
            }
            E.document.cursor.cy = cy;
            E.document.cursor.cx = cx;
            E.document.mouse.dragging = 1;
        } else if (in_text_area && mouse_button == 32 && E.document.mouse.dragging) {
            /* Arm from the press point only when motion creates a selection. */
            if (!E.document.selection.active) {
                E.document.selection.active = 1;
                E.document.selection.anchor_x = E.document.mouse.press_anchor_x;
                E.document.selection.anchor_y = E.document.mouse.press_anchor_y;
            }
            int32_t cy, cx;
            editorMouseToCursor(mouseEventCol, mouseEventRow, &cy, &cx);
            E.document.cursor.cy = cy;
            E.document.cursor.cx = cx;
        }
    }
    return 0;
}

/**
 * @brief Apply navigation and its selection transition without reading input.
 * @return 1 for a navigation key, otherwise 0 without changing state.
 */
static uint8_t editorHandleNavigationKey(int32_t c) {
    switch (c) {
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

        case PAGE_UP:
        case PAGE_DOWN:
        case SHIFT_PAGE_UP:
        case SHIFT_PAGE_DOWN: {
            if (E.document.buffer.row_count == 0) {
                E.document.cursor.cx = 0;
                E.document.cursor.cy = 0;
                E.document.cursor.rx = 0;
                E.document.selection.active = 0;
                E.document.selection.anchor_x = 0;
                E.document.selection.anchor_y = 0;
                E.view.rowoff = 0;
                E.view.coloff = 0;
                break;
            }
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

        default: return 0;
    }
    return 1;
}

/**
 * @brief Execute file, history or modal commands selected by a key.
 * @details Modal commands own their prompts and input; ordinary dispatch does
 * not consume additional events. Returns 0 for a non-command key.
 */
static uint8_t editorHandleCommandKey(int32_t c) {
    switch (c) {
        case CTRL_KEY('q'):
            E.document.selection.active = 0;
            editorQuit();
            return 1;

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

        case CTRL_KEY('l'):
            /* Redraw/resize is not an editing action and preserves selection. */
            break;

        default: return 0;
    }
    return 1;
}

/**
 * @brief Apply text and selection actions to the active document.
 * @details Captures the immutable selection once; editing commands retain
 * their own undo and rendering contracts. Paste and UTF-8 input own payload reads.
 */
static void editorHandleEditKey(int32_t c) {
    editorBeginEdit();

    /* Immutable snapshot for actions that consume or wrap the selection.
     * Each switch branch owns its selection transition; there is no global
     * pre-dispatch whitelist for new keys to accidentally bypass. */
    int32_t had_sel_y0 = 0, had_sel_x0 = 0, had_sel_y1 = 0, had_sel_x1 = 0;
    uint8_t had_sel = editorSelectionRange(&E.document.selection, &E.document.cursor, &had_sel_y0, &had_sel_x0, &had_sel_y1, &had_sel_x1);

    switch (c) {
        case '\r':
            if (had_sel)
                editorReplaceSelectionWithNewline(had_sel_y0, had_sel_x0,
                    had_sel_y1, had_sel_x1);
            else
                editorInsertNewlineAutoIndent();
            E.document.selection.active = 0;
            break;

        case '\t':
            if (had_sel) {
                editorIndentSelection(0);
            } else {
                E.document.selection.active = 0;
                if (S.insert_spaces_for_tab) {
                    editorPushUndo(EDIT_OTHER);
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

        case '\x1b':
            /* Esc also turns off pinned selection mode (Ctrl-T), not
             * just the current selection -- otherwise the next arrow
             * press would silently start a new selection again, since
             * sel_pinned would still be set. */
            E.document.selection.pinned = 0;
            E.document.selection.active = 0;
            break;

        default: {
            /* Pairs retain their useful "wrap selection" behavior. Every
             * other typed character replaces selected text, like paste and
             * delete already do. Keep deletion and insertion in one undo
             * action by inserting the first byte directly after the snapshot. */
            const struct autoClosePair *pair = S.auto_close_pairs ?
                editorAutoClosePairFor(c, S.auto_close_single_quote != 0) : NULL;
            if (had_sel && !pair) {
                editorPushUndo(EDIT_OTHER);
                editorDeleteRangeRaw(had_sel_y0, had_sel_x0, had_sel_y1, had_sel_x1);
                editorInsertCharRaw(c);
            } else {
                editorInsertCharAutoClose(c, had_sel, had_sel_y0, had_sel_x0,
                    had_sel_y1, had_sel_x1);
            }
            E.document.selection.active = 0;
            break;
        }
    }
    editorEndEdit();
}

/**
 * @brief Route a non-mouse key through menus, navigation, commands and editing.
 * @details Does not decode or drain input. Accepted mouse menu commands enter
 * here too, so keyboard and mouse execute the same actions.
 */
static void editorDispatchKey(int32_t c) {
    E.view.free_scroll = 0;
    if (S.show_menu && (M.open || c == F10_KEY)) {
        enum editorCommand command = menuHandleKey(&M, c);
        editorSyncMenuMouseMotion();
        if (command == CMD_NONE) return;
        if (commandIsSetting(command)) {
            editorToggleMenuSetting(command);
            return;
        }
        c = editorMenuCommandKey(command);
        if (c == 0) return;
    }
    if (editorHandleNavigationKey(c)) return;
    if (editorHandleCommandKey(c)) return;
    editorHandleEditKey(c);
}

/**
 * @brief Read and route an event, coalescing a bounded burst of mouse reports.
 * @details Dispatches the first following non-mouse event exactly once, in
 * order. The burst limit bounds redraw latency; unread events remain queued.
 */
static void editorProcessKeypress(void) {
    int32_t c = editorReadKey();
    for (int32_t count = 0; c == MOUSE_EVENT_KEY; count++) {
        int32_t command = editorHandleMouseEvent();
        if (command != 0) {
            editorDispatchKey(command);
            return;
        }
        if (count + 1 == MOUSE_BURST_LIMIT || !terminalInputReady()) return;
        c = editorReadKey();
    }
    editorCancelMouseDrag();
    editorDispatchKey(c);
}

/* ---- init ------------------------------------------------------------------- */

/**
 * @brief Release both history stacks owned by the active document.
 *
 * @details Used when resetting a document and as an exit handler;
 * historyClear() leaves the history ready for reuse.
 */
static void editorFreeUndoRedo(void) {
    historyClear(&E.document.history);
}

/**
 * @brief Initialize the editor state, settings and usable viewport.
 *
 * @details Call once after entering terminal mode. Loads ~/.tinyeditrc and
 * obtains terminal dimensions; failure to query dimensions is fatal.
 */
static void initEditor(void) {
    E.document.cursor.cx = 0;
    E.document.cursor.cy = 0;
    E.document.cursor.rx = 0;
    E.view.rowoff = 0;
    E.view.coloff = 0;
    E.view.free_scroll = 0;
    E.document.file.detected_line_ending = LINE_ENDING_LF;
    E.document.file.line_endings_mixed = 0;
    E.document.file.final_newline = 1;
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
    E.search.search_match_end_y = -1;
    E.search.search_match_end_x = 0;
    E.document.file.last_backup_time = 0;

    settingsLoad(&S);
    editorBindHistory();
    menuInit(&M);

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
    if (S.show_menu) E.view.screenrows -= 1;
}

/**
 * @brief Start the terminal editor and run its redraw and input loop.
 *
 * @details An optional argv[1] names the document to open. Registers terminal
 * cleanup, offers recovery, and continues until the quit action exits.
 */
int main(int argc, char **argv) {
    terminalEnableRawMode();
    terminalEnterAlternateScreen();
    terminalEnableBracketedPaste();
    terminalEnableResizeHandling();
    initEditor();
#ifdef __APPLE__
    terminalConfigureGhosttyCommandBindings((uint8_t)S.mac_command_keys);
    if (S.mac_command_keys) terminalEnableKittyKeyboard();
#endif
    editorChooseSlogan();
    if (S.mouse_enabled) terminalEnableMouseReporting();
    atexit(editorFreeUndoRedo);
    if (argc >= 2 && !editorOpen(argv[1])) terminalDie("open file");

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
