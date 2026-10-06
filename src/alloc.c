/* alloc.c -- checked fatal allocations and explicit recoverable attempts. */

#include "alloc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Report allocation failure and terminate through normal exit cleanup.
 *
 * @details Uses exit() so registered handlers can restore terminal modes and
 * visual state.
 */
static void teOutOfMemory(void) {
    errno = ENOMEM;
    perror("tinyedit: memory allocation failed");
    /* exit(), rather than _exit(), deliberately runs the terminal cleanup
     * handlers registered with atexit() by terminal.c. */
    exit(EXIT_FAILURE);
}

/**
 * @brief Allocate storage or leave the editor cleanly on allocation failure.
 *
 * @details size is a byte count; zero requests still allocate one byte.
 * @return owned storage to free, never NULL.
 */
void *teMalloc(size_t size) {
    void *ptr = malloc(size > 0 ? size : 1);
    if (!ptr) teOutOfMemory();
    return ptr;
}

/**
 * @brief Resize owned storage or leave the editor cleanly on failure.
 *
 * @details ptr may be NULL and size is in bytes; zero requests allocate one
 * byte.
 * @return the new owned pointer, which may differ from ptr, never NULL.
 */
void *teRealloc(void *ptr, size_t size) {
    void *grown = realloc(ptr, size > 0 ? size : 1);
    if (!grown) teOutOfMemory();
    return grown;
}

/**
 * @brief Make an owned copy of a NUL-terminated string.
 *
 * @details s must be non-NULL.
 * @return a copy to free; allocation failure exits through the shared cleanup
 * path.
 */
char *teStrdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = teMalloc(len);
    memcpy(copy, s, len);
    return copy;
}

/* ---- recoverable allocations ---------------------------------------- */

void *teTryMalloc(size_t size) {
    void *ptr = malloc(size > 0 ? size : 1);
    if (!ptr) errno = ENOMEM;
    return ptr;
}

void *teTryRealloc(void *ptr, size_t size) {
    void *grown = realloc(ptr, size > 0 ? size : 1);
    if (!grown) errno = ENOMEM;
    return grown;
}

/* ---- checked size arithmetic ---------------------------------------- */

size_t teSizeAdd(size_t left, size_t right) {
    if (right > SIZE_MAX - left) teOutOfMemory();
    return left + right;
}

size_t teArrayBytes(size_t count, size_t element_size) {
    if (element_size && count > SIZE_MAX / element_size) teOutOfMemory();
    return count * element_size;
}

size_t teGrowCapacity(size_t capacity, size_t needed, size_t limit) {
    if (capacity > limit || needed > limit) teOutOfMemory();
    if (!capacity) capacity = needed;
    while (capacity < needed) {
        if (capacity > limit / 2) return needed;
        capacity *= 2;
    }
    return capacity;
}
