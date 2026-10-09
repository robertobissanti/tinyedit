/* terminal.h -- POSIX terminal lifecycle and input decoding. */

#ifndef __TE_TERMINAL_H
#define __TE_TERMINAL_H

#include "tinyedit.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

extern volatile sig_atomic_t winsize_changed;
extern int32_t mouseEventButton;
extern int32_t mouseEventCol, mouseEventRow;
extern uint8_t mouseEventPress;
extern int32_t pending_key;

/**
 * @brief Write all output bytes, retrying interruptions and partial writes.
 * @return 1 on success, 0 with errno on failure; safe for exit handlers.
 */
uint8_t terminalWrite(const char *data, size_t len);
/**
 * @brief Report a fatal system error and exit through terminal cleanup.
 *
 * @details s is the operation label passed to perror(). Does not return; exit
 * handlers restore modes already enabled.
 */
void terminalDie(const char *s);
/**
 * @brief Configure byte-oriented input without echo or line buffering.
 *
 * @details Call before reading editor events. Saves the original termios state
 * and registers cleanup; setup failure exits.
 */
void terminalEnableRawMode(void);
/**
 * @brief Restore the termios state saved when raw mode was enabled.
 *
 * @details Use only after terminalEnableRawMode(); registered as its exit
 * handler. A restoration failure is fatal.
 */
void terminalDisableRawMode(void);
/**
 * @brief Switch to a separate editor screen and register its cleanup.
 *
 * @details Safe to call again while active. The main screen and shell
 * scrollback return on normal exit.
 */
void terminalEnterAlternateScreen(void);
/**
 * @brief Leave the alternate screen and restore cursor appearance.
 *
 * @details Does nothing if the alternate screen is inactive. Also resets
 * attributes so the shell inherits a normal display.
 */
void terminalRestoreVisualState(void);
/**
 * @brief Ask the terminal to delimit pasted blocks.
 *
 * @details Registers cleanup. A PASTE_START_KEY event must be followed by
 * terminalReadPastedText(), rather than dispatching pasted bytes as
 * keystrokes.
 */
void terminalEnableBracketedPaste(void);
/**
 * @brief Stop terminal paste markers from leaking into the shell.
 *
 * @details Writes the disable sequence; used as the exit handler after
 * enabling bracketed paste.
 */
void terminalDisableBracketedPaste(void);
#ifdef __APPLE__
/**
 * @brief Push escape-disambiguation mode for Command-key events.
 *
 * @details macOS-only and opt-in. Registers cleanup once and avoids pushing a
 * second mode while already active.
 */
void terminalEnableKittyKeyboard(void);
/**
 * @brief Pop the keyboard-protocol mode enabled by this editor.
 *
 * @details macOS-only; does nothing when inactive and restores the previous
 * terminal keyboard state.
 */
void terminalDisableKittyKeyboard(void);
/**
 * @brief Install or remove the editor-owned Command-key block in Ghostty configuration.
 *
 * @details macOS-only; enabled selects installation. Preserves other
 * configuration text and requests a reload.
 * @return a success flag; configuration or reload errors return 0.
 */
uint8_t terminalConfigureGhosttyCommandBindings(uint8_t enabled);
#endif
/**
 * @brief Check for queued terminal input without blocking.
 *
 * @details Used to drain mouse-event bursts before redrawing.
 * @return 1 when select() reports readable stdin, otherwise 0.
 */
uint8_t terminalInputReady(void);
/**
 * @brief Enable SGR click and drag reports for the editor.
 *
 * @details Call only when mouse support is enabled in settings. Registers
 * cleanup and requests Shift-modified reports from supporting terminals.
 */
void terminalEnableMouseReporting(void);
/**
 * @brief Restore native terminal mouse behavior.
 *
 * @details Disables hover, drag and SGR reporting plus the requested Shift
 * override; used on runtime disable and exit.
 */
void terminalDisableMouseReporting(void);
/**
 * @brief Switch between menu hover and document drag reporting.
 *
 * @details enabled selects all-motion reporting; disabling restores button-
 * motion reporting. Call while editor mouse support is active.
 */
void terminalSetMenuMouseMotion(uint8_t enabled);
/**
 * @brief Install the resize signal handler without restarting blocked reads.
 *
 * @details Call at startup so SIGWINCH wakes input and lets the next frame
 * recompute terminal dimensions.
 */
void terminalEnableResizeHandling(void);
/**
 * @brief Decode one terminal input event into the editor's key vocabulary.
 *
 * @details On macOS mac_command_keys enables Command/Super translation. Resize
 * and pending_key are handled here too.
 * @return a raw byte or editorKey value; mouse reports populate the module's
 * shared mouse fields.
 */
int32_t terminalReadKey(
#ifdef __APPLE__
    uint8_t mac_command_keys
#else
    void
#endif
);
/**
 * @brief Collect a bracketed paste until its closing marker.
 *
 * @details Call only after PASTE_START_KEY. Writes outlen and returns owned
 * bytes to free, without a NUL terminator; a failed read returns the bytes
 * collected so far.
 */
char *terminalReadPastedText(size_t *outlen);
/**
 * @brief Get terminal dimensions, querying cursor position if ioctl is unavailable.
 *
 * @details Writes rows and cols and returns 0 on success, -1 on failure. The
 * fallback moves the cursor toward the bottom-right corner.
 */
int32_t terminalGetWindowSize(int32_t *rows, int32_t *cols);

#endif /* __TE_TERMINAL_H */
