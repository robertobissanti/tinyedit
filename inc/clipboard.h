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
 * the NUL terminator. An empty clipboard produces an allocated empty string; failed external
 * access falls back to the in-process buffer.
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

/**
 * @brief Copy bytes to the system clipboard, with an in-process fallback.
 *
 * @details data need not be NUL-terminated. If the external tool fails, stores
 * len bytes internally and still returns 1; allocation failure exits through
 * the checked allocator.
 */
uint8_t clipboardCopy(const char *data, size_t len);

/**
 * @brief Fetch clipboard text, falling back to the in-process copy.
 *
 * @details Release with clipboardFree(); outlen may be NULL and excludes NUL.
 * @return owned NUL-terminated text, including an empty allocation for empty
 * fallback contents.
 */
char *clipboardPaste(size_t *outlen);

/**
 * @brief Release text returned by clipboardPaste().
 *
 * @details Accepts NULL; does not clear the underlying clipboard contents.
 */
void clipboardFree(char *ptr);

/**
 * @brief Get the selected clipboard backend, probing on first use.
 *
 * @details Runtime command failures use the internal fallback without changing
 * that identifier.
 * @return the cached detection result.
 */
ClipboardBackend clipboardBackend(void);

/**
 * @brief Get a readable name for the detected clipboard backend.
 *
 * @return a borrowed literal suitable for status text; do not free it.
 */
const char *clipboardBackendName(void);

#endif /* __TE_CLIPBOARD_H */
