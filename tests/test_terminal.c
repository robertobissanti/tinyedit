#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "terminal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
static pid_t terminalTestFork(void);
#define fork terminalTestFork
#endif
#include "../src/terminal.c"
#ifdef __APPLE__
#undef fork
static pid_t terminalTestFork(void) { errno = EAGAIN; return -1; }
#endif

static void check(uint8_t condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

int main(void) {
    int32_t rows = -1, cols = -1;
    check(terminalParseCursorPosition("\x1b[23;80R", &rows, &cols) && rows == 23 && cols == 80,
        "valid terminal cursor report");
    const char *invalid[] = {"\x1b[0;80R", "\x1b[23;0R", "\x1b[65536;80R",
        "\x1b[23;999999999999999999R", "\x1b[23;80Rgarbage", "\x1b[23;80", "\x1b[;80R"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        check(!terminalParseCursorPosition(invalid[i], &rows, &cols) && rows == 23 && cols == 80,
            "invalid report leaves dimensions intact");
#ifdef __APPLE__
    char home[] = "/tmp/tinyedit-terminal-XXXXXX";
    check(mkdtemp(home) != NULL, "terminal fixture directory");
    check(setenv("HOME", home, 1) == 0 && setenv("XDG_CONFIG_HOME", home, 1) == 0,
        "isolated Ghostty configuration");
    char folder[256], path[256], target[256];
    snprintf(folder, sizeof(folder), "%s/ghostty", home);
    snprintf(path, sizeof(path), "%s/ghostty/config", home);
    snprintf(target, sizeof(target), "%s/real-config", home);
    check(mkdir(folder, 0700) == 0 && symlink(target, path) == 0, "Ghostty config symlink");
    FILE *fp = fopen(target, "wb");
    const char binary[] = "\0keep this configuration\n";
    check(fp && fwrite(binary, 1, sizeof(binary), fp) == sizeof(binary) && fclose(fp) == 0,
        "binary Ghostty fixture");
    check(!terminalConfigureGhosttyCommandBindings(1), "binary Ghostty config rejected");
    char bytes[sizeof(binary)];
    fp = fopen(target, "rb");
    check(fp && fread(bytes, 1, sizeof(bytes), fp) == sizeof(bytes) &&
        !memcmp(bytes, binary, sizeof(bytes)) && fclose(fp) == 0, "binary Ghostty config unchanged");
    fp = fopen(target, "wb");
    check(fp != NULL, "long Ghostty config fixture");
    for (int32_t i = 0; i < 4095; i++) check(fputc('x', fp) != EOF, "long config prefix");
    check(fputs(managed_lines[0], fp) >= 0 && fclose(fp) == 0, "managed-looking line suffix");
    (void)terminalConfigureGhosttyCommandBindings(1);
    struct stat st;
    check(lstat(path, &st) == 0 && S_ISLNK(st.st_mode), "Ghostty symlink preserved");
    fp = fopen(target, "rb");
    check(fp != NULL, "rewritten Ghostty target");
    for (int32_t i = 0; i < 4095; i++) check(fgetc(fp) == 'x', "long config prefix preserved");
    char suffix[128];
    check(fgets(suffix, sizeof(suffix), fp) && !strcmp(suffix, managed_lines[0]),
        "long config suffix is not mistaken for a managed line");
    fclose(fp);
    unlink(target);
    check(!terminalConfigureGhosttyCommandBindings(1) && lstat(path, &st) == 0 &&
        S_ISLNK(st.st_mode), "dangling Ghostty link preserved");
    unlink(path); rmdir(folder); rmdir(home);
#endif
    puts("terminal tests: ok");
    return 0;
}
