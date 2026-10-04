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

#endif /* __TE_ALLOC_H */
