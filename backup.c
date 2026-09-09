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

/* Absolute path of `filename` into `out` (size `outsize`). realpath()
 * requires the file to exist, which a brand new unsaved buffer's
 * target usually doesn't yet -- so for a non-existent path we fall
 * back to resolving just the parent directory and appending the
 * given basename, which still yields a stable absolute path (the
 * same file edited from different relative paths/cwds hashes the
 * same either way, matching realpath()'s own guarantee). */
static uint8_t absolutePathOf(const char *filename, char *out, size_t outsize) {
    if (realpath(filename, out) != NULL) return 1;

    char cwd[1024];
    const char *base = strrchr(filename, '/');
    if (base) {
        char dir[1024];
        size_t dirlen = (size_t)(base - filename);
        if (dirlen == 0) dirlen = 1; /* "/" */
        if (dirlen >= sizeof(dir)) return 0;
        memcpy(dir, filename, dirlen);
        dir[dirlen] = '\0';
        if (realpath(dir, cwd) == NULL) return 0;
        base++; /* skip '/' */
    } else {
        if (getcwd(cwd, sizeof(cwd)) == NULL) return 0;
        base = filename;
    }
    return (size_t)snprintf(out, outsize, "%s/%s", cwd, base) < outsize;
}

uint8_t backupPathFor(const char *filename, char *out, size_t outsize) {
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;

    char abspath[1024];
    if (!absolutePathOf(filename, abspath, sizeof(abspath))) return 0;

    uint64_t hash = fnv1a64(abspath);
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

    char abspath[1024];
    if (!absolutePathOf(filename, abspath, sizeof(abspath))) return 0;

    /* Write to a temp file then rename() into place: rename() is
     * atomic on the same filesystem, so a crash mid-write (e.g. power
     * loss) can never leave a half-written .swp that backupRead()
     * would confuse for valid recovered content. */
    char tmppath[1040];
    int32_t n = snprintf(tmppath, sizeof(tmppath), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(tmppath)) return 0;

    int fd = mkstemp(tmppath);
    if (fd == -1) return 0;
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmppath);
        return 0;
    }

    uint8_t ok = fprintf(fp, "%s\n", abspath) >= 0 &&
        fwrite(content, 1, len, fp) == len &&
        fflush(fp) == 0 && fsync(fd) == 0;
    if (fclose(fp) != 0) ok = 0;

    if (!ok) {
        unlink(tmppath);
        return 0;
    }
    if (rename(tmppath, path) != 0) {
        unlink(tmppath);
        return 0;
    }
    return 1;
}

void backupRemove(const char *filename) {
    char path[1024];
    if (!backupPathFor(filename, path, sizeof(path))) return;
    unlink(path);
}
