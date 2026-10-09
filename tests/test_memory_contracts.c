#define _DEFAULT_SOURCE

#include "alloc.h"
#include "clipboard.h"

#include <stdint.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Select the actual internal implementation without touching system clipboard. */
#include "../src/clipboard.c"

static void check(uint8_t condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

static void testRejectedSizes(void) {
    for (int32_t operation = 0; operation < 3; operation++) {
        pid_t child = fork();
        check(child >= 0, "fork overflow test");
        if (!child) {
            if (!freopen("/dev/null", "w", stderr)) _exit(2);
            if (operation == 0) teSizeAdd(SIZE_MAX, 1);
            else if (operation == 1) teArrayBytes(SIZE_MAX, 2);
            else teGrowCapacity(16, 33, 32);
            _exit(0);
        }
        int status;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
            WEXITSTATUS(status) == EXIT_FAILURE, "overflow rejects before allocation");
    }
}

static void tick(int sig) { (void)sig; }

/* A handler without SA_RESTART (like the editor's SIGWINCH handler) interrupts
 * read() while a slow clipboard tool runs; the paste must retry, not fall back
 * to stale internal text. */
static void testPasteSurvivesSignal(void) {
    char dir[] = "/tmp/tinyedit-fakeclip-XXXXXX";
    check(mkdtemp(dir) != NULL, "fake clipboard directory");
    char tool[256];
    snprintf(tool, sizeof(tool), "%s/xclip", dir);
    FILE *script = fopen(tool, "w");
    check(script != NULL, "fake clipboard tool");
    fputs("#!/bin/sh\nsleep 0.4\nprintf 'SYSTEM-CLIPBOARD'\n", script);
    check(fclose(script) == 0 && chmod(tool, 0755) == 0, "fake tool executable");
    char *old_path = getenv("PATH") ? teStrdup(getenv("PATH")) : teStrdup("/usr/bin:/bin");
    char new_path[512];
    snprintf(new_path, sizeof(new_path), "%s:/usr/bin:/bin", dir);
    setenv("PATH", new_path, 1);
    backend = CLIPBOARD_BACKEND_XCLIP;
    internalCopy("STALE", 5);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = tick;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, NULL);
    struct itimerval timer = {{0, 0}, {0, 100000}};
    setitimer(ITIMER_REAL, &timer, NULL);
    size_t len = 0;
    char *text = clipboardPaste(&len);
    check(text && len == 16 && !strcmp(text, "SYSTEM-CLIPBOARD"),
        "interrupted paste retries instead of using stale internal text");
    clipboardFree(text);
    setenv("PATH", old_path, 1);
    free(old_path);
    unlink(tool);
    rmdir(dir);
    backend = CLIPBOARD_BACKEND_INTERNAL;
}

int main(void) {
    check(teSizeAdd(SIZE_MAX - 1, 1) == SIZE_MAX, "sum exact boundary");
    check(teArrayBytes(SIZE_MAX, 1) == SIZE_MAX, "product exact boundary");
    check(teArrayBytes(SIZE_MAX, 0) == 0, "zero element size");
    check(teGrowCapacity(16, 17, 32) == 32, "ordinary growth");
    check(teGrowCapacity(SIZE_MAX / 2 + 1, SIZE_MAX, SIZE_MAX) == SIZE_MAX,
        "growth near size limit");
    check(teGrowCapacity(0, 0, 0) == 0, "empty capacity");
    testRejectedSizes();

    backend = CLIPBOARD_BACKEND_INTERNAL;
    size_t len = 99;
    char *text = clipboardPaste(&len);
    check(text && !len && !text[0], "paste before copy is allocated empty text");
    clipboardFree(text);
    clipboardCopy("", 0);
    len = 99;
    text = clipboardPaste(&len);
    check(text && !len && !text[0] && internal_buf[0] == '\0', "empty copy terminates both buffers");
    clipboardFree(text);
    const char bytes[] = {'a', '\0', (char)0xff, 'b'};
    clipboardCopy(bytes, sizeof(bytes));
    text = clipboardPaste(&len);
    check(len == sizeof(bytes) && !memcmp(text, bytes, len) && text[len] == '\0' &&
        internal_buf[internal_len] == '\0', "clipboard preserves raw bytes and terminator");
    text[0] = 'z';
    clipboardFree(text);
    text = clipboardPaste(NULL);
    check(text[0] == 'a', "paste owns independent allocation");
    clipboardFree(text);
    free(internal_buf);
    internal_buf = NULL; internal_len = 0;
    testPasteSurvivesSignal();
    puts("memory contract tests: ok");
    return 0;
}
