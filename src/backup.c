/* backup.c -- see backup.h */

#define _DEFAULT_SOURCE

#include "backup.h"
#include "alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BACKUP_DIR_SUFFIX "/.tinyedit/backup"

/* FNV-1a, 64-bit -- a small, dependency-free, non-cryptographic hash.
 * Good enough here: we only need to turn an absolute path into a
 * short fixed-width filename with a vanishingly small collision
 * chance for the number of files a single user edits, not to resist
 * a deliberate attacker choosing paths to collide. */
static uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* `realpath(path, NULL)` allocates exactly the space required by the resolved
 * path. A new unsaved buffer's target does not exist yet, so resolve its
 * parent directory and append the basename instead. */
static char *absolutePathOf(const char *filename) {
    char *resolved = realpath(filename, NULL);
    if (resolved != NULL) return resolved;

    const char *base = strrchr(filename, '/');
    char *cwd;
    if (base) {
        size_t dirlen = (size_t)(base - filename);
        if (dirlen == 0) dirlen = 1;
        char *dir = teMalloc(dirlen + 1);
        if (!dir) return NULL;
        memcpy(dir, filename, dirlen);
        dir[dirlen] = '\0';
        cwd = realpath(dir, NULL);
        free(dir);
        if (cwd == NULL) return NULL;
        base++; /* skip '/' */
    } else {
        cwd = realpath(".", NULL);
        if (cwd == NULL) return NULL;
        base = filename;
    }

    size_t len = strlen(cwd) + 1 + strlen(base) + 1;
    char *absolute = teMalloc(len);
    if (absolute != NULL) snprintf(absolute, len, "%s/%s", cwd, base);
    free(cwd);
    return absolute;
}

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

/* Ensures ~/.tinyedit/backup/ exists, creating parent directories as
 * needed. Returns 1 if the directory exists (or was just created), 0
 * on failure. */
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

    return 1;
}

uint8_t backupExists(const char *filename) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return 0;
    return access(path, F_OK) == 0;
}

char *backupRead(const char *filename, size_t *outlen) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd == -1) return NULL;

    /* First line is the original path, kept for identification but not
     * part of the recovered content. Avoid a fixed-size line buffer:
     * the path is metadata and may be longer than our UI buffers. */
    char byte;
    ssize_t nread;
    do {
        nread = read(fd, &byte, 1);
    } while (nread == 1 && byte != '\n');
    if (nread != 1) { close(fd); return NULL; }

    size_t cap = 4096, len = 0;
    char *buf = teMalloc(cap);
    if (!buf) { close(fd); return NULL; }

    while ((nread = read(fd, buf + len, cap - len)) > 0) {
        len += (size_t)nread;
        if (len == cap) {
            cap *= 2;
            char *grown = teRealloc(buf, cap);
            if (!grown) { free(buf); close(fd); return NULL; }
            buf = grown;
        }
    }
    if (nread == -1) {
        free(buf);
        close(fd);
        return NULL;
    }
    close(fd);

    buf[len] = '\0'; /* cap always left room for at least 1 byte */
    if (outlen) *outlen = len;
    return buf;
}

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
    if (fclose(fp) != 0) ok = 0;

    if (!ok) {
        unlink(tmppath);
        free(abspath);
        return 0;
    }
    if (rename(tmppath, path) != 0) {
        unlink(tmppath);
        free(abspath);
        return 0;
    }
    free(abspath);
    return 1;
}

void backupRemove(const char *filename) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return;
    unlink(path);
}
