/* alloc.c -- fail-fast allocation with terminal-safe process cleanup. */

#include "alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void teOutOfMemory(void) {
    errno = ENOMEM;
    perror("tinyedit: memory allocation failed");
    /* exit(), rather than _exit(), deliberately runs the terminal cleanup
     * handlers registered with atexit() by terminal.c. */
    exit(EXIT_FAILURE);
}

void *teMalloc(size_t size) {
    void *ptr = malloc(size > 0 ? size : 1);
    if (!ptr) teOutOfMemory();
    return ptr;
}

void *teRealloc(void *ptr, size_t size) {
    void *grown = realloc(ptr, size > 0 ? size : 1);
    if (!grown) teOutOfMemory();
    return grown;
}

char *teStrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = teMalloc(len);
    memcpy(copy, s, len);
    return copy;
}
