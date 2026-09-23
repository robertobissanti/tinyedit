/* alloc.h -- checked allocation helpers used by every TinyEdit module. */

#ifndef __TE_ALLOC_H
#define __TE_ALLOC_H

#include <stddef.h>

void *teMalloc(size_t size);
void *teRealloc(void *ptr, size_t size);
char *teStrdup(const char *s);

#endif /* __TE_ALLOC_H */
