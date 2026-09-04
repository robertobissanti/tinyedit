/* clipboard.h -- cross-platform system clipboard access with an internal
 * fallback buffer, for use in tinyedit.
 *
 * Detection order at first use:
 *   macOS            -> pbcopy / pbpaste
 *   Linux + Wayland   -> wl-copy / wl-paste   (if $WAYLAND_DISPLAY is set)
 *   Linux + X11       -> xclip -selection clipboard  (if $DISPLAY is set)
 *   none available    -> in-memory buffer (not shared with the OS)
 *
 * clipboardCopy() takes a buffer + length (not necessarily NUL-terminated).
 * clipboardPaste() returns a malloc'd, NUL-terminated string the caller
 * must free with clipboardFree(); *outlen receives its length excluding
 * the NUL terminator. Returns NULL on total failure (never on "empty").
 */

#ifndef __TE_CLIPBOARD_H
#define __TE_CLIPBOARD_H

#include <stddef.h>
#include <stdint.h>

/* Which external backend (if any) is being used. Exposed mostly for the
 * settings screen / status messages. */
typedef enum {
    CLIPBOARD_BACKEND_UNKNOWN = 0,
    CLIPBOARD_BACKEND_INTERNAL,
    CLIPBOARD_BACKEND_PBCOPY,
    CLIPBOARD_BACKEND_WLCLIPBOARD,
    CLIPBOARD_BACKEND_XCLIP
} ClipboardBackend;

/* Copies `len` bytes from `data` to the clipboard. Returns 1 on success,
 * 0 on failure (in which case the previous clipboard contents, if any,
 * are left untouched). */
uint8_t clipboardCopy(const char *data, size_t len);

/* Returns a malloc'd NUL-terminated copy of the current clipboard
 * contents, or NULL if the clipboard is empty or unreadable. */
char *clipboardPaste(size_t *outlen);

void clipboardFree(char *ptr);

/* Returns which backend is actually in use (probes lazily on first
 * call if not yet determined). */
ClipboardBackend clipboardBackend(void);

/* Human-readable name of the active backend, e.g. "pbcopy/pbpaste",
 * "wl-clipboard", "xclip", "internal buffer (no system clipboard found)". */
const char *clipboardBackendName(void);

#endif /* __TE_CLIPBOARD_H */
