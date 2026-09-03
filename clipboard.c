/* clipboard.c -- see clipboard.h */

#define _DEFAULT_SOURCE

#include "clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- internal fallback buffer ------------------------------------------ */

static char *internal_buf = NULL;
static size_t internal_len = 0;

static void internalCopy(const char *data, size_t len) {
    free(internal_buf);
    internal_buf = malloc(len);
    if (internal_buf) memcpy(internal_buf, data, len);
    internal_len = internal_buf ? len : 0;
}

static char *internalPaste(size_t *outlen) {
    if (!internal_buf || internal_len == 0) return NULL;
    char *copy = malloc(internal_len + 1);
    if (!copy) return NULL;
    memcpy(copy, internal_buf, internal_len);
    copy[internal_len] = '\0';
    if (outlen) *outlen = internal_len;
    return copy;
}

/* ---- backend detection --------------------------------------------------- */

static ClipboardBackend backend = CLIPBOARD_BACKEND_UNKNOWN;

/* Returns 1 if `cmd` is found on $PATH (checked via `command -v`, POSIX
 * shell builtin, so this works even for shell built-ins/aliases-free
 * lookups without depending on a specific `which` binary being present). */
static int commandExists(const char *cmd) {
    char probe[256];
    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", cmd);
    int rc = system(probe);
    return rc == 0;
}

static ClipboardBackend detectBackend(void) {
#ifdef __APPLE__
    if (commandExists("pbcopy") && commandExists("pbpaste"))
        return CLIPBOARD_BACKEND_PBCOPY;
#endif

    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && *wayland &&
        commandExists("wl-copy") && commandExists("wl-paste"))
        return CLIPBOARD_BACKEND_WLCLIPBOARD;

    const char *x11 = getenv("DISPLAY");
    if (x11 && *x11 && commandExists("xclip"))
        return CLIPBOARD_BACKEND_XCLIP;

    /* Last resort: try pbcopy even without __APPLE__ defined (e.g. cross
     * compiled builds) and the two Linux tools even without their usual
     * env var hints, in case detection above was too strict. */
    if (commandExists("pbcopy") && commandExists("pbpaste"))
        return CLIPBOARD_BACKEND_PBCOPY;
    if (commandExists("wl-copy") && commandExists("wl-paste"))
        return CLIPBOARD_BACKEND_WLCLIPBOARD;
    if (commandExists("xclip"))
        return CLIPBOARD_BACKEND_XCLIP;

    return CLIPBOARD_BACKEND_INTERNAL;
}

ClipboardBackend clipboardBackend(void) {
    if (backend == CLIPBOARD_BACKEND_UNKNOWN)
        backend = detectBackend();
    return backend;
}

const char *clipboardBackendName(void) {
    switch (clipboardBackend()) {
        case CLIPBOARD_BACKEND_PBCOPY:       return "pbcopy/pbpaste";
        case CLIPBOARD_BACKEND_WLCLIPBOARD:  return "wl-clipboard";
        case CLIPBOARD_BACKEND_XCLIP:        return "xclip";
        case CLIPBOARD_BACKEND_INTERNAL:
        default:
            return "internal buffer (no system clipboard found)";
    }
}

/* ---- subprocess helpers ---------------------------------------------------- */

static const char *copyCommandFor(ClipboardBackend b) {
    switch (b) {
        case CLIPBOARD_BACKEND_PBCOPY:      return "pbcopy";
        case CLIPBOARD_BACKEND_WLCLIPBOARD: return "wl-copy";
        case CLIPBOARD_BACKEND_XCLIP:       return "xclip -selection clipboard -in";
        default: return NULL;
    }
}

static const char *pasteCommandFor(ClipboardBackend b) {
    switch (b) {
        case CLIPBOARD_BACKEND_PBCOPY:      return "pbpaste";
        case CLIPBOARD_BACKEND_WLCLIPBOARD: return "wl-paste --no-newline";
        case CLIPBOARD_BACKEND_XCLIP:       return "xclip -selection clipboard -out";
        default: return NULL;
    }
}

static int runCopyCommand(const char *cmd, const char *data, size_t len) {
    FILE *pipe = popen(cmd, "w");
    if (!pipe) return 0;
    size_t written = fwrite(data, 1, len, pipe);
    int rc = pclose(pipe);
    return written == len && rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static char *runPasteCommand(const char *cmd, size_t *outlen) {
    FILE *pipe = popen(cmd, "r");
    if (!pipe) return NULL;

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(pipe); return NULL; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, pipe)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); pclose(pipe); return NULL; }
            buf = grown;
        }
    }
    int rc = pclose(pipe);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0'; /* cap was always left with room for at least 1 byte */
    if (outlen) *outlen = len;
    return buf;
}

/* ---- public API -------------------------------------------------------------- */

int clipboardCopy(const char *data, size_t len) {
    ClipboardBackend b = clipboardBackend();
    const char *cmd = copyCommandFor(b);
    if (!cmd) {
        internalCopy(data, len);
        return 1;
    }

    if (runCopyCommand(cmd, data, len)) return 1;

    /* External backend failed at runtime (e.g. no display connection even
     * though the binary exists) -- fall back to the internal buffer so
     * copy/paste still works within the session. */
    internalCopy(data, len);
    return 1;
}

char *clipboardPaste(size_t *outlen) {
    ClipboardBackend b = clipboardBackend();
    const char *cmd = pasteCommandFor(b);
    if (!cmd) return internalPaste(outlen);

    char *result = runPasteCommand(cmd, outlen);
    if (result) return result;

    return internalPaste(outlen);
}

void clipboardFree(char *ptr) {
    free(ptr);
}
