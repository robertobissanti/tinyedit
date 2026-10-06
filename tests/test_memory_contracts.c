#define _DEFAULT_SOURCE

#include "alloc.h"
#include "clipboard.h"

#include <stdint.h>
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
    puts("memory contract tests: ok");
    return 0;
}
