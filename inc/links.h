/* links.h -- bounded link recognition and safe desktop URL dispatch. */
#ifndef TE_LINKS_H
#define TE_LINKS_H
#include <stdint.h>

struct textLink {
    int32_t start, end;
    int32_t target_start, target_end;
};

/* All offsets are source byte boundaries. No allocation or ownership transfer. */
uint8_t linksFind(const char *text, int32_t length, int32_t cursor,
    struct textLink *link);
/* Returns an owned, Markdown-unescaped target, or NULL for unsafe controls. */
char *linksTarget(const char *text, const struct textLink *link);
/* Returns an owned decoded local path/fragment, rejecting encoded controls. */
char *linksDecode(const char *text);
/* Returns an owned GitHub-style ATX heading slug. */
char *linksHeadingSlug(const char *text, int32_t length);
uint8_t linksIsWeb(const char *target);
/* Launches only HTTP(S), with an argument vector, never through a shell.
 * Reports launcher failure; browser loading happens asynchronously. */
uint8_t linksOpenWeb(const char *target);
#endif
