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

void terminalDie(const char *s);
void terminalEnableRawMode(void);
void terminalDisableRawMode(void);
void terminalRestoreVisualState(void);
void terminalEnableBracketedPaste(void);
void terminalDisableBracketedPaste(void);
uint8_t terminalInputReady(void);
void terminalEnableMouseReporting(void);
void terminalDisableMouseReporting(void);
void terminalEnableResizeHandling(void);
int32_t terminalReadKey(uint8_t mac_command_keys);
char *terminalReadPastedText(size_t *outlen);
int32_t terminalGetWindowSize(int32_t *rows, int32_t *cols);

#endif /* __TE_TERMINAL_H */

