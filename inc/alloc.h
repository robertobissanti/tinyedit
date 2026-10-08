/* alloc.h -- checked allocation helpers used by every TinyEdit module. */

#ifndef __TE_ALLOC_H
#define __TE_ALLOC_H

#include <stddef.h>

/**
 * @brief Allocate storage or leave the editor cleanly on allocation failure.
 *
 * @details size is a byte count; zero requests still allocate one byte.
 * @return owned storage to free, never NULL.
 */
void *teMalloc(size_t size);
/**
 * @brief Resize owned storage or leave the editor cleanly on failure.
 *
 * @details ptr may be NULL and size is in bytes; zero requests allocate one
 * byte.
 * @return the new owned pointer, which may differ from ptr, never NULL.
 */
void *teRealloc(void *ptr, size_t size);
/**
 * @brief Make an owned copy of a NUL-terminated string.
 *
 * @details s must be non-NULL.
 * @return a copy to free; allocation failure exits through the shared cleanup
 * path.
 */
char *teStrdup(const char *s);

/** @brief Attempt allocation, returning NULL with ENOMEM on failure.
 * @details Recoverable exception to the fatal helpers; caller owns cleanup. */
void *teTryMalloc(size_t size);
/** @brief Attempt resizing without losing the original pointer on failure.
 * @details Zero requests allocate one byte. The caller uses a temporary pointer. */
void *teTryRealloc(void *ptr, size_t size);

/* Size arithmetic follows the fatal helpers' terminal-cleanup contract. */
/** @brief Add byte counts, exiting with cleanup on overflow. */
size_t teSizeAdd(size_t left, size_t right);
/** @brief Compute array allocation bytes, exiting with cleanup on overflow. */
size_t teArrayBytes(size_t count, size_t element_size);
/* Grow to at least needed without exceeding limit or overflowing. */
/** @brief Grow byte capacity to needed within limit, exiting if impossible. */
size_t teGrowCapacity(size_t capacity, size_t needed, size_t limit);

#endif /* __TE_ALLOC_H */
