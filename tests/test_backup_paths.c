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
    check(rmdir(directory) == 0, "remove path fixture");
    puts("backup path tests: ok");
    return 0;
}
