/* backup.c -- see backup.h */

#define _DEFAULT_SOURCE

#include "backup.h"
#include "alloc.h"
#include "fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACKUP_DIR_SUFFIX "/.tinyedit/backup"

/**
 * @brief Turn a path into a stable compact identifier for its backup name.
 *
 * @details s is NUL-terminated.
 * @return a non-cryptographic 64-bit hash; it is an identifier, not a security
 * check.
 *
 * @note FNV-1a, 64-bit -- a small, dependency-free, non-cryptographic hash.
 * Good enough here: we only need to turn an absolute path into a short fixed-
 * width filename with a vanishingly small collision chance for the number of
 * files a single user edits, not to resist a deliberate attacker choosing
 * paths to collide.
 */
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/**
 * @brief Resolve a file's absolute path, including a not-yet-created target.
 *
 * @details If the file does not resolve, resolves its parent and appends the
 * basename.
 * @return owned NUL-terminated text to free, or NULL when resolution fails.
 */
static char *absolutePathOf(const char *filename) {
    char *resolved = realpath(filename, NULL);
    if (resolved != NULL) return resolved;
    /* realpath allocates internally: ENOMEM and other resolution errors
     * are recoverable here, never evidence of a not-yet-created target. */
    if (errno != ENOENT) return NULL;

    const char *base = strrchr(filename, '/');
    char *cwd;
    if (base) {
        size_t dirlen = (size_t)(base - filename);
        if (dirlen == 0) dirlen = 1;
        char *dir = teMalloc(teSizeAdd(dirlen, 1));
        memcpy(dir, filename, dirlen);
        dir[dirlen] = '\0';
        cwd = realpath(dir, NULL);
        int saved_errno = errno;
        free(dir);
        errno = saved_errno;
        if (cwd == NULL) return NULL;
        base++; /* skip '/' */
    } else {
        cwd = realpath(".", NULL);
        if (cwd == NULL) return NULL;
        base = filename;
    }

    size_t len = teSizeAdd(teSizeAdd(strlen(cwd), strlen(base)), 2);
    char *absolute = teMalloc(len);
    snprintf(absolute, len, "%s/%s", cwd, base);
    free(cwd);
    return absolute;
}

/**
 * @brief Build the recovery filename associated with an edited file.
 *
 * @details out has outsize bytes.
 * @return 1 with a NUL-terminated path, or 0 if HOME, path resolution or
 * capacity prevents it; the document need not exist yet.
 */
uint8_t backupPathFor(const char *filename, char *out, size_t outsize) {
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;

    char *abspath = absolutePathOf(filename);
    if (!abspath) return 0;
    uint64_t hash = fnv1a64(abspath);
    free(abspath);
    int32_t n = snprintf(out, outsize, "%s" BACKUP_DIR_SUFFIX "/%016" PRIx64 ".swp",
        home, hash);
    return n > 0 && (size_t)n < outsize;
}

/**
 * @brief Create the private directories used for recovery copies.
 *
 * @details Uses HOME and mode 0700 for newly created directories.
 * @return 1 if both directories exist or are created, otherwise 0.
 */
static uint8_t ensureBackupDir(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;

    char path[1024];
    int32_t n = snprintf(path, sizeof(path), "%s/.tinyedit", home);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return 0;

    n = snprintf(path, sizeof(path), "%s" BACKUP_DIR_SUFFIX, home);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return 0;

    /* Sync the parents too: syncing backup/ alone cannot make newly
     * created .tinyedit/ and backup/ entries durable. Also retry after a
     * previous mkdir succeeded but its parent synchronization failed. */
    if (!fileioSyncDirectory(home)) return 0;
    n = snprintf(path, sizeof(path), "%s/.tinyedit", home);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    return fileioSyncDirectory(path);
}

/**
 * @brief Check whether a recovery file is present for a document.
 *
 * @details filename identifies the original file.
 * @return 1 when its backup path exists, otherwise 0; it does not validate
 * backup contents.
 */
uint8_t backupExists(const char *filename) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return 0;
    return access(path, F_OK) == 0;
}

/**
 * @brief Read recoverable document bytes without the backup's path header.
 *
 * @details outlen may be NULL; otherwise receives the byte count excluding NUL
 * on success.
 * @return owned NUL-terminated text to free, or NULL on missing, malformed or
 * unreadable backup.
 */
char *backupRead(const char *filename, size_t *outlen) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd == -1) return NULL;

    /* Match the entire known path instead of treating its first newline as
     * a delimiter: POSIX filenames may themselves contain newlines. */
    char *expected = absolutePathOf(filename);
    if (!expected) { close(fd); return NULL; }
    size_t header_length = strlen(expected);
    uint8_t byte;
    ssize_t nread;
    uint8_t header_ok = 1;
    for (size_t i = 0; i <= header_length; i++) {
        do { nread = read(fd, &byte, 1); } while (nread < 0 && errno == EINTR);
        if (nread != 1 || byte != (i == header_length ? '\n' : (uint8_t)expected[i])) {
            header_ok = 0;
            break;
        }
    }
    free(expected);
    if (!header_ok) { close(fd); return NULL; }

    size_t cap = 4096, len = 0;
    char *buf = teMalloc(cap);

    while (1) {
        do { nread = read(fd, buf + len, cap - len); } while (nread < 0 && errno == EINTR);
        if (nread <= 0) break;
        len += (size_t)nread;
        if (len == cap) {
            cap = teGrowCapacity(cap, teSizeAdd(len, 1), SIZE_MAX);
            char *grown = teRealloc(buf, cap);
            buf = grown;
        }
    }
    if (nread == -1) {
        free(buf);
        close(fd);
        return NULL;
    }
    if (close(fd) != 0) { free(buf); return NULL; }

    buf[len] = '\0'; /* cap always left room for at least 1 byte */
    if (outlen) *outlen = len;
    return buf;
}

/**
 * @brief Write document bytes to their recovery file.
 *
 * @details content contains len bytes. Creates the backup directory when
 * needed and prefixes the absolute document path.
 * @return 1 after file and directory sync, 0 on path or I/O failure.
 * A post-rename sync failure may have replaced the backup; it is retained.
 */
uint8_t backupWrite(const char *filename, const char *content, size_t len) {
    if (!ensureBackupDir()) return 0;

    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return 0;

    char *abspath = absolutePathOf(filename);
    if (!abspath) return 0;

    /* Write to a temp file then rename() into place: rename() is
     * atomic on the same filesystem, so a crash mid-write (e.g. power
     * loss) can never leave a half-written .swp that backupRead()
     * would confuse for valid recovered content. */
    char tmppath[1040];
    int32_t n = snprintf(tmppath, sizeof(tmppath), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmppath)) {
        free(abspath);
        return 0;
    }

    int fd = mkstemp(tmppath);
    if (fd == -1) {
        free(abspath);
        return 0;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmppath);
        free(abspath);
        return 0;
    }

    uint8_t ok = fprintf(fp, "%s\n", abspath) >= 0 &&
        fwrite(content, 1, len, fp) == len &&
        fflush(fp) == 0 && fsync(fd) == 0;
    int close_errno = errno;
    if (fclose(fp) != 0 && ok) {
        ok = 0;
        close_errno = errno;
    }
    errno = close_errno;

    if (ok) ok = fileioReplace(tmppath, path) == FILE_SAVE_DURABLE;
    if (!ok) {
        int saved_errno = errno;
        unlink(tmppath);
        free(abspath);
        errno = saved_errno;
        return 0;
    }
    free(abspath);
    return 1;
}

/**
 * @brief Remove a document's recovery file after save or clean departure.
 *
 * @details filename identifies the original document. Missing files and
 * removal failures are deliberately ignored.
 */
void backupRemove(const char *filename) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return;
    unlink(path);
}
