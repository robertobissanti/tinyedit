#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "backup.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int forced_error;
static int32_t resolutions;
static char *faultRealpath(const char *path, char *resolved) {
    resolutions++;
    if (forced_error) { errno = forced_error; return NULL; }
    return realpath(path, resolved);
}
#define realpath faultRealpath
#include "../src/backup.c"
#undef realpath

static void check(uint8_t condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

int main(void) {
    const int errors[] = {ENOMEM, EACCES, ENOTDIR};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        forced_error = errors[i];
        resolutions = 0;
        check(absolutePathOf("missing") == NULL && errno == errors[i] && resolutions == 1,
            "realpath failure cannot be mistaken for missing target");
    }
    forced_error = 0;
    char directory[] = "/tmp/tinyedit-backup-paths-XXXXXX";
    check(mkdtemp(directory) != NULL, "create path fixture");
    char target[256];
    snprintf(target, sizeof(target), "%s/new.md", directory);
    resolutions = 0;
    char *path = absolutePathOf(target);
    check(path != NULL && resolutions == 2 && strstr(path, "/new.md") != NULL,
        "missing file resolves existing parent");
    free(path);
    check(setenv("HOME", directory, 1) == 0, "isolated backup home");
    snprintf(target, sizeof(target), "%s/a\nb.txt", directory);
    check(backupWrite(target, "ORIGINAL", 8), "write newline filename backup");
    size_t recovered_length = 0;
    char *recovered = backupRead(target, &recovered_length);
    check(recovered && recovered_length == 8 && memcmp(recovered, "ORIGINAL", 8) == 0,
        "path header cannot leak into recovered document");
    free(recovered); backupRemove(target);
    snprintf(target, sizeof(target), "%s/.tinyedit/backup", directory); rmdir(target);
    snprintf(target, sizeof(target), "%s/.tinyedit", directory); rmdir(target);
    check(rmdir(directory) == 0, "remove path fixture");
    puts("backup path tests: ok");
    return 0;
}
