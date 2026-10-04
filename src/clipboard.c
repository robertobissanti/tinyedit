/* clipboard.c -- see clipboard.h */

#define _DEFAULT_SOURCE

#include "clipboard.h"
#include "alloc.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- internal fallback buffer ------------------------------------------ */

static char *internal_buf = NULL;
static size_t internal_len = 0;

/**
 * @brief Keep an in-process copy when system clipboard access is unavailable.
 *
 * @details data contains len bytes. Replaces the previous fallback buffer and
 * preserves a trailing NUL.
 */
static void internalCopy(const char *data, size_t len) {
    free(internal_buf);
    internal_buf = teMalloc(len);
    if (internal_buf) memcpy(internal_buf, data, len);
    internal_len = internal_buf ? len : 0;
}

/**
 * @brief Return an independent copy of the fallback clipboard.
 *
 * @details outlen may be NULL; otherwise receives the byte count.
 * @return owned NUL-terminated text, including an allocated empty string when
 * nothing has been copied.
 */
static char *internalPaste(size_t *outlen) {
    if (!internal_buf || internal_len == 0) return NULL;
    char *copy = teMalloc(internal_len + 1);
    memcpy(copy, internal_buf, internal_len);
    copy[internal_len] = '\0';
    if (outlen) *outlen = internal_len;
    return copy;
}

/* ---- backend detection --------------------------------------------------- */

static ClipboardBackend backend = CLIPBOARD_BACKEND_UNKNOWN;

/**
 * @brief Check whether an executable name can be found on PATH.
 *
 * @details cmd is a NUL-terminated command name, not a shell expression.
 * @return 1 for an executable candidate, otherwise 0.
 */
static uint8_t commandExists(const char *cmd) {
    if (strchr(cmd, '/')) return access(cmd, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/bin:/bin";
    const char *segment = path;
    for (;;) {
        const char *colon = strchr(segment, ':');
        size_t dirlen = colon ? (size_t)(colon - segment) : strlen(segment);
        const char *dir = dirlen ? segment : ".";
        size_t actual_dirlen = dirlen ? dirlen : 1;
        size_t needed = actual_dirlen + 1 + strlen(cmd) + 1;
        char *candidate = teMalloc(needed);
        snprintf(candidate, needed, "%.*s/%s", (int)actual_dirlen, dir, cmd);
        uint8_t found = access(candidate, X_OK) == 0;
        free(candidate);
        if (found) return 1;
        if (!colon) break;
        segment = colon + 1;
    }
    return 0;
}

/**
 * @brief Choose an available system clipboard tool or the internal fallback.
 *
 * @details Checks platform and display hints before trying remaining installed
 * tools.
 * @return a backend identifier without invoking a clipboard command.
 */
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

/**
 * @brief Get the selected clipboard backend, probing on first use.
 *
 * @details Runtime command failures use the internal fallback without changing
 * that identifier.
 * @return the cached detection result.
 */
ClipboardBackend clipboardBackend(void) {
    if (backend == CLIPBOARD_BACKEND_UNKNOWN)
        backend = detectBackend();
    return backend;
}

/**
 * @brief Get a readable name for the detected clipboard backend.
 *
 * @return a borrowed literal suitable for status text; do not free it.
 */
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

/**
 * @brief Replace a clipboard child process with the backend's copy tool.
 *
 * @details Call only in the forked child after connecting stdin. Does not
 * return on successful exec; exits with status 127 if dispatch or exec fails.
 */
static void execCopyCommand(ClipboardBackend b) {
    switch (b) {
        case CLIPBOARD_BACKEND_PBCOPY: {
            char *const argv[] = { "pbcopy", NULL };
            execvp(argv[0], argv);
            break;
        }
        case CLIPBOARD_BACKEND_WLCLIPBOARD: {
            char *const argv[] = { "wl-copy", NULL };
            execvp(argv[0], argv);
            break;
        }
        case CLIPBOARD_BACKEND_XCLIP: {
            char *const argv[] = { "xclip", "-selection", "clipboard", "-in", NULL };
            execvp(argv[0], argv);
            break;
        }
        default: break;
    }
    _exit(127);
}

/**
 * @brief Replace a clipboard child process with the backend's paste tool.
 *
 * @details Call only in the forked child after connecting stdout. Does not
 * return on successful exec; exits with status 127 if dispatch or exec fails.
 */
static void execPasteCommand(ClipboardBackend b) {
    switch (b) {
        case CLIPBOARD_BACKEND_PBCOPY: {
            char *const argv[] = { "pbpaste", NULL };
            execvp(argv[0], argv);
            break;
        }
        case CLIPBOARD_BACKEND_WLCLIPBOARD: {
            char *const argv[] = { "wl-paste", "--no-newline", NULL };
            execvp(argv[0], argv);
            break;
        }
        case CLIPBOARD_BACKEND_XCLIP: {
            char *const argv[] = { "xclip", "-selection", "clipboard", "-out", NULL };
            execvp(argv[0], argv);
            break;
        }
        default: break;
    }
    _exit(127);
}

/**
 * @brief Collect a clipboard subprocess status despite interrupted waits.
 *
 * @details pid is the child to reap.
 * @return the raw wait status for WIFEXITED/WEXITSTATUS, or -1 on a non-EINTR
 * failure.
 */
static int32_t waitForChild(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) return -1;
    }
    return status;
}

/**
 * @brief Send bytes to a system clipboard subprocess without a shell.
 *
 * @details data contains len bytes.
 * @return 1 only if all bytes were sent and the child exited successfully,
 * otherwise 0.
 */
static uint8_t runCopyCommand(ClipboardBackend b, const char *data, size_t len) {
    int fds[2];
    if (pipe(fds) == -1) return 0;
    pid_t pid = fork();
    if (pid == -1) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        close(fds[1]);
        if (dup2(fds[0], STDIN_FILENO) == -1) _exit(127);
        close(fds[0]);
        execCopyCommand(b);
    }

    close(fds[0]);
    void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fds[1], data + written, len - written);
        if (n > 0) written += (size_t)n;
        else if (n == -1 && errno == EINTR) continue;
        else break;
    }
    close(fds[1]);
    signal(SIGPIPE, old_sigpipe);
    int32_t status = waitForChild(pid);
    return written == len && status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * @brief Collect bytes from a system clipboard subprocess.
 *
 * @details outlen may be NULL and excludes the terminator when supplied.
 * @return owned NUL-terminated text or NULL on pipe, read or child failure.
 */
static char *runPasteCommand(ClipboardBackend b, size_t *outlen) {
    int fds[2];
    if (pipe(fds) == -1) return NULL;
    pid_t pid = fork();
    if (pid == -1) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) == -1) _exit(127);
        close(fds[1]);
        execPasteCommand(b);
    }
    close(fds[1]);

    size_t cap = 4096, len = 0;
    char *buf = teMalloc(cap);

    ssize_t n;
    while ((n = read(fds[0], buf + len, cap - len)) > 0) {
        len += (size_t)n;
        if (len == cap) {
            cap *= 2;
            char *grown = teRealloc(buf, cap);
            buf = grown;
        }
    }
    close(fds[0]);
    int32_t status = waitForChild(pid);
    if (n == -1 || status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0'; /* cap was always left with room for at least 1 byte */
    if (outlen) *outlen = len;
    return buf;
}

/* ---- public API -------------------------------------------------------------- */

/**
 * @brief Copy bytes to the system clipboard, with an in-process fallback.
 *
 * @details data need not be NUL-terminated. If the external tool fails, stores
 * len bytes internally and still returns 1; allocation failure exits through
 * the checked allocator.
 */
uint8_t clipboardCopy(const char *data, size_t len) {
    ClipboardBackend b = clipboardBackend();
    if (b == CLIPBOARD_BACKEND_INTERNAL) {
        internalCopy(data, len);
        return 1;
    }

    if (runCopyCommand(b, data, len)) return 1;

    /* External backend failed at runtime (e.g. no display connection even
     * though the binary exists) -- fall back to the internal buffer so
     * copy/paste still works within the session. */
    internalCopy(data, len);
    return 1;
}

/**
 * @brief Fetch clipboard text, falling back to the in-process copy.
 *
 * @details Release with clipboardFree(); outlen may be NULL and excludes NUL.
 * @return owned NUL-terminated text, including an empty allocation for empty
 * fallback contents.
 */
char *clipboardPaste(size_t *outlen) {
    ClipboardBackend b = clipboardBackend();
    if (b == CLIPBOARD_BACKEND_INTERNAL) return internalPaste(outlen);

    char *result = runPasteCommand(b, outlen);
    if (result) return result;

    return internalPaste(outlen);
}

/**
 * @brief Release text returned by clipboardPaste().
 *
 * @details Accepts NULL; does not clear the underlying clipboard contents.
 */
void clipboardFree(char *ptr) {
    free(ptr);
}
