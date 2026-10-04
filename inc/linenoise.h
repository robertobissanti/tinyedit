/* linenoise.h -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * See linenoise.c for more information.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __LINENOISE_H
#define __LINENOISE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* For size_t. */

extern char *linenoiseEditMore;

#define LINENOISE_MAX_FOLDS 16

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    int in_completion;  /* The user pressed TAB and we are now in completion
                         * mode, so input is handled by completeLine(). */
    size_t completion_idx; /* Index of next completion to propose. */
    int ifd;            /* Terminal stdin file descriptor. */
    int ofd;            /* Terminal stdout file descriptor. */
    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    size_t buflen_max;  /* Max buffer size, or 0 if fixed. */
    const char *prompt; /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldpos;      /* Previous refresh cursor position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t oldrows;     /* Rows used by last refrehsed line (multiline mode) */
    int oldrpos;        /* Cursor row from last refresh (for multiline clearing). */
    int history_index;  /* The history index we are currently editing. */
    int fold_count;    /* Number of folded ranges. */
    size_t fold_start[LINENOISE_MAX_FOLDS]; /* Folded range start offsets. */
    size_t fold_end[LINENOISE_MAX_FOLDS];   /* Folded range end offsets. */
};

typedef struct linenoiseCompletions {
  size_t len;
  char **cvec;
} linenoiseCompletions;

/* Non blocking API. */
/**
 * @brief Start a nonblocking line-editing session with a caller-provided buffer.
 *
 * @details buf must have at least buflen writable bytes including room for NUL
 * and remain alive until stop; -1 descriptors select standard streams. Pair
 * feed calls with linenoiseEditStop().
 * @return 0 on success, -1 on setup failure.
 */
int linenoiseEditStart(struct linenoiseState *l, int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt);
/**
 * @brief Process input for a live nonblocking line-editing session.
 *
 * @details Do not free the sentinel; finish with linenoiseEditStop().
 * @return linenoiseEditMore while editing continues, an owned completed line
 * to release with linenoiseFree(), or NULL on end or error.
 */
char *linenoiseEditFeed(struct linenoiseState *l);
/**
 * @brief Finish a historical editing session and restore terminal input.
 *
 * @details l must have been started successfully. Restores raw mode and emits
 * a newline for terminal sessions; does not free the caller's edit buffer.
 */
void linenoiseEditStop(struct linenoiseState *l);
/**
 * @brief Temporarily erase the prompt and edited input.
 *
 * @details Use before printing unrelated output during a live nonblocking
 * session; linenoiseShow() redraws the same input afterward.
 */
void linenoiseHide(struct linenoiseState *l);
/**
 * @brief Redisplay an editing session after unrelated terminal output.
 *
 * @details l must be a live session previously hidden with linenoiseHide().
 */
void linenoiseShow(struct linenoiseState *l);

/* Blocking API. */
/**
 * @brief Read a line with terminal editing or a plain-stream fallback.
 *
 * @details prompt is borrowed NUL-terminated text.
 * @return owned input to release with linenoiseFree(), or NULL on EOF,
 * interruption or failure.
 */
char *linenoise(const char *prompt);
/**
 * @brief Release a line returned by the historical editing API.
 *
 * @details Accepts NULL and ignores linenoiseEditMore so an accidentally
 * passed continuation sentinel is not freed.
 */
void linenoiseFree(void *ptr);

/* Completion API. */
typedef void(linenoiseCompletionCallback)(const char *, linenoiseCompletions *);
typedef char*(linenoiseHintsCallback)(const char *, int *color, int *bold);
typedef void(linenoiseFreeHintsCallback)(void *);
/**
 * @brief Register the callback that supplies Tab-completion candidates.
 *
 * @details fn may be NULL to disable completion; the callback adds copied
 * candidates through linenoiseAddCompletion().
 */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *);
/**
 * @brief Register the callback that supplies a display hint for current input.
 *
 * @details fn may be NULL to disable hints. Hint ownership follows the
 * separately registered free-hints callback.
 */
void linenoiseSetHintsCallback(linenoiseHintsCallback *);
/**
 * @brief Register the cleanup function for hints returned by the hint callback.
 *
 * @details fn may be NULL when hints need no release; otherwise it receives
 * the hint after rendering.
 */
void linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *);
/**
 * @brief Append a copied candidate to a completion result.
 *
 * @details lc is the callback's completion accumulator and str is NUL-terminated. The historical allocator may leave the list unchanged on
 * failure.
 */
void linenoiseAddCompletion(linenoiseCompletions *, const char *);

/* History API. */
/**
 * @brief Append a copied line to the process-wide history.
 *
 * @details line is NUL-terminated. Avoids consecutive duplicates and respects
 * the current limit; returns 1 when added, 0 when skipped or allocation fails.
 */
int linenoiseHistoryAdd(const char *line);
/**
 * @brief Change the number of entries retained in historical line editing.
 *
 * @details len must be positive. Drops oldest entries when shrinking; returns
 * 1 on success, 0 for invalid length or allocation failure.
 */
int linenoiseHistorySetMaxLen(int len);
/**
 * @brief Write historical entries as newline-separated text with private permissions.
 *
 * @details filename is NUL-terminated.
 * @return 0 when the file can be opened and entries are written, -1 if opening
 * fails; this legacy routine does not validate every write.
 */
int linenoiseHistorySave(const char *filename);
/**
 * @brief Append entries read from a saved history file.
 *
 * @details filename is NUL-terminated.
 * @return 0 on completion, -1 on open or allocation failure; entries already
 * loaded remain if a later read fails.
 */
int linenoiseHistoryLoad(const char *filename);

/* Other utilities. */
/**
 * @brief Clear the screen and move the terminal cursor to its origin.
 *
 * @details Writes ANSI output to stdout; does not modify the edited input or
 * history.
 */
void linenoiseClearScreen(void);
/**
 * @brief Select single-line or multiline rendering for future refreshes.
 *
 * @details ml is a boolean flag. Multiline mode permits the edited line to
 * occupy several terminal rows.
 */
void linenoiseSetMultiLine(int ml);
/**
 * @brief Run an interactive raw-byte logger for terminal key diagnostics.
 *
 * @details Writes incoming byte values until the logger's quit sequence is
 * received, then restores terminal mode.
 */
void linenoisePrintKeyCodes(void);
/**
 * @brief Hide edited input behind mask characters.
 *
 * @details Changes the global mode used on subsequent redraws; underlying text
 * remains available to the caller.
 */
void linenoiseMaskModeEnable(void);
/**
 * @brief Return to displaying edited input normally.
 *
 * @details Changes the global mask mode; the next refresh shows the real text.
 */
void linenoiseMaskModeDisable(void);

#ifdef __cplusplus
}
#endif

#endif /* __LINENOISE_H */
