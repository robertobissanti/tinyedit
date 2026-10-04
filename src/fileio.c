#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "fileio.h"
#include "alloc.h"
#include "buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- input paths --------------------------------------------------------- */

/**
 * @brief Expand the current user's home shorthand without invoking a shell.
 * @details Only a complete leading ~ or ~/ is special; embedded tildes,
 * variables, wildcards and ~user retain their literal filename meaning.
 * @return owned text, or NULL with errno if expansion cannot be prepared.
 */
char *fileioExpandHomePath(const char *path) {
    if (path[0] != '~' || (path[1] != '/' && path[1] != '\0')) return teStrdup(path);
    const char *home = getenv("HOME");
    if (!home || !home[0]) { errno = ENOENT; return NULL; }
    size_t home_len = strlen(home), suffix_len = strlen(path + 1);
    if (home_len > SIZE_MAX - suffix_len - 1) { errno = ENAMETOOLONG; return NULL; }
    char *expanded = teMalloc(home_len + suffix_len + 1);
    memcpy(expanded, home, home_len);
    memcpy(expanded + home_len, path + 1, suffix_len + 1);
    return expanded;
}

/* ---- loading ------------------------------------------------------------- */

/**
 * @brief Append one logical row and update line-ending metadata.
 * @details Removes only LF and its immediately preceding CR, preserving all
 * other bytes. Requires a document initialized for sequential loading.
 * @return 0 with EFBIG before insertion if editor index limits would overflow.
 */
uint8_t fileioAppendLine(struct editorDocument *document, const char *line, size_t len) {
    uint8_t ended = len > 0 && line[len - 1] == '\n';
    enum lineEndingMode ending = LINE_ENDING_LF;
    uint8_t had_ending = document->buffer.row_count > 0 && document->file.final_newline;
    if (ended) {
        len--;
        if (len > 0 && line[len - 1] == '\r') {
            len--;
            ending = LINE_ENDING_CRLF;
        }
        if (!had_ending) document->file.detected_line_ending = ending;
        else if (ending != document->file.detected_line_ending)
            document->file.line_endings_mixed = 1;
    }
    if (len > INT32_MAX || document->buffer.row_count == INT32_MAX ||
        (size_t)document->buffer.row_count + 1 > SIZE_MAX / sizeof(erow)) {
        errno = EFBIG;
        return 0;
    }
    document->file.final_newline = ended;
    bufferInsertRow(&document->buffer, document->buffer.row_count, line, len);
    return 1;
}

/**
 * @brief Build candidate rows from an owned-by-caller input stream.
 * @details getline's EOF, I/O and allocation failures are distinguished;
 * failure frees partial rows. The caller still closes the stream.
 */
uint8_t fileioLoadStream(FILE *stream, struct editorDocument *candidate) {
    candidate->file.detected_line_ending = LINE_ENDING_LF;
    candidate->file.final_newline = 1;
    candidate->file.line_endings_mixed = 0;
    char *line = NULL;
    size_t capacity = 0;
    uint8_t ok = 1;
    while (1) {
        errno = 0;
        ssize_t len = getline(&line, &capacity, stream);
        if (len == -1) {
            if (ferror(stream) || !feof(stream)) {
                if (errno == 0) errno = EIO;
                ok = 0;
            }
            break;
        }
        if (!fileioAppendLine(candidate, line, (size_t)len)) {
            ok = 0;
            break;
        }
    }
    int saved_errno = errno;
    free(line);
    if (!ok) bufferClear(&candidate->buffer);
    errno = saved_errno;
    return ok;
}

/**
 * @brief Read a separate named document without touching application state.
 * @details candidate receives owned text and name only after a complete read
 * and successful close; ENOENT creates a named empty document.
 * @return 0 with errno and no owned candidate storage on I/O failure.
 */
uint8_t fileioLoadDocument(const char *filename, struct editorDocument *candidate) {
    memset(candidate, 0, sizeof(*candidate));
    candidate->file.detected_line_ending = LINE_ENDING_LF;
    candidate->file.final_newline = 1;
    if (filename[0] == '\0') { errno = EINVAL; return 0; }
    FILE *stream = fopen(filename, "rb");
    if (!stream) {
        if (errno != ENOENT) return 0;
        candidate->file.filename = teStrdup(filename);
        return 1;
    }
    struct stat st;
    uint8_t ok = fstat(fileno(stream), &st) == 0;
    if (ok && S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        ok = 0;
    }
    if (ok) ok = fileioLoadStream(stream, candidate);
    int saved_errno = errno;
    if (fclose(stream) != 0 && ok) {
        saved_errno = errno;
        ok = 0;
    }
    if (!ok) {
        bufferClear(&candidate->buffer);
        errno = saved_errno;
        return 0;
    }
    candidate->file.filename = teStrdup(filename);
    return 1;
}

/* ---- replacement and durability ------------------------------------------ */

/**
 * @brief Synchronize an open descriptor, retrying interrupted calls.
 */
static uint8_t fileioSync(int fd) {
    int result;
    do result = fsync(fd); while (result == -1 && errno == EINTR);
    return result == 0;
}

/**
 * @brief Synchronize directory entries after creation or replacement.
 * @return 0 with errno on open, sync or close failure.
 */
uint8_t fileioSyncDirectory(const char *directory) {
    int fd = open(directory, O_RDONLY | O_DIRECTORY);
    if (fd == -1) return 0;
    uint8_t ok = fileioSync(fd);
    int saved_errno = errno;
    if (close(fd) != 0 && ok) {
        ok = 0;
        saved_errno = errno;
    }
    errno = saved_errno;
    return ok;
}

/**
 * @brief Replace a target and synchronize its parent directory.
 * @details Opens the parent before rename so an open failure leaves the
 * target untouched. A later failure cannot undo an already committed rename.
 */
enum fileSaveResult fileioReplace(const char *temporary, const char *target) {
    char *parent = teStrdup(target);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        free(parent);
        parent = teStrdup(".");
    } else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int dirfd = open(parent, O_RDONLY | O_DIRECTORY);
    int saved_errno = errno;
    free(parent);
    if (dirfd == -1) {
        errno = saved_errno;
        return FILE_SAVE_FAILED;
    }
    enum fileSaveResult result = FILE_SAVE_FAILED;
    if (rename(temporary, target) == 0)
        result = fileioSync(dirfd) ? FILE_SAVE_DURABLE : FILE_SAVE_UNCERTAIN;
    saved_errno = errno;
    if (close(dirfd) != 0 && result == FILE_SAVE_DURABLE) {
        result = FILE_SAVE_UNCERTAIN;
        saved_errno = errno;
    }
    errno = saved_errno;
    return result;
}

/**
 * @brief Write a byte span despite short writes and interrupted calls.
 * @return 0 with errno on failure, including a zero-byte write.
 */
static uint8_t fileioWriteAll(int fd, const char *bytes, size_t len) {
    size_t written = 0;
    while (written < len) {
        size_t chunk = len - written;
        if (chunk > (size_t)SSIZE_MAX) chunk = (size_t)SSIZE_MAX;
        ssize_t count = write(fd, bytes + written, chunk);
        if (count > 0) written += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else {
            if (count == 0) errno = EIO;
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Save through a synchronized temporary file beside the target.
 * @details Preserves existing target permissions and follows existing symlinks.
 * Removes owned temporary storage on failure and preserves the failure errno.
 */
enum fileSaveResult fileioAtomicSave(const char *filename, const char *bytes, size_t len) {
    char *resolved = realpath(filename, NULL);
    if (!resolved && errno != ENOENT) return FILE_SAVE_FAILED;
    const char *target = resolved ? resolved : filename;
    const char suffix[] = ".tinyedit.XXXXXX";
    size_t target_len = strlen(target);
    if (target_len > SIZE_MAX - sizeof(suffix)) {
        free(resolved);
        errno = ENAMETOOLONG;
        return FILE_SAVE_FAILED;
    }
    char *temporary = teMalloc(target_len + sizeof(suffix));
    memcpy(temporary, target, target_len);
    memcpy(temporary + target_len, suffix, sizeof(suffix));
    struct stat st;
    uint8_t exists = stat(target, &st) == 0;
    int fd = mkstemp(temporary);
    enum fileSaveResult result = FILE_SAVE_FAILED;
    if (fd != -1) {
        mode_t mode;
        if (exists) mode = st.st_mode & 07777;
        else {
            mode_t mask = umask(0);
            umask(mask);
            mode = 0644 & ~mask;
        }
        uint8_t ok = fchmod(fd, mode) == 0 &&
            fileioWriteAll(fd, bytes, len) && fileioSync(fd);
        int saved_errno = errno;
        if (close(fd) != 0 && ok) {
            saved_errno = errno;
            ok = 0;
        }
        errno = saved_errno;
        if (ok) result = fileioReplace(temporary, target);
    }
    int saved_errno = errno;
    if (result == FILE_SAVE_FAILED && fd != -1) unlink(temporary);
    free(temporary);
    free(resolved);
    errno = saved_errno;
    return result;
}
