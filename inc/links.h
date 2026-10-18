/* links.h -- bounded link recognition and safe desktop URL dispatch. */
#ifndef TE_LINKS_H
#define TE_LINKS_H
#include <stdint.h>

struct textLink {
    int32_t start, end;
    int32_t target_start, target_end;
};

/** @brief Find the inline link or bare web URL under a source-byte cursor offset.
 * @details All offsets are source-byte boundaries with exclusive ends; no
 * allocation or ownership transfer. Returns 1 and fills link, otherwise 0. */
uint8_t linksFind(const char *text, int32_t length, int32_t cursor,
    struct textLink *link);
/** @brief Copy a link destination without Markdown escapes.
 * @return owned text to free, or NULL when it contains control bytes. */
char *linksTarget(const char *text, const struct textLink *link);
/** @brief Decode %XX escapes in a local path or fragment.
 * @return owned text to free, or NULL when a control byte would result. */
char *linksDecode(const char *text);
/** @brief Build a GitHub-style anchor from an ATX heading row of length bytes.
 * @return owned text to free. */
char *linksHeadingSlug(const char *text, int32_t length);
/** @brief Check for a complete HTTP(S) URL with no spaces or control bytes.
 * @return 1 when acceptable, otherwise 0. */
uint8_t linksIsWeb(const char *target);
/** Transport check for a nonempty mailto URI; raw spaces/controls are rejected.
 * URI parameters stay encoded and are interpreted by the desktop mail composer. */
uint8_t linksIsMailto(const char *target);
/** Open the configured mail composer without sending mail. Asynchronous;
 * success confirms launcher execution, not the presence of a mail account. */
uint8_t linksOpenMailto(const char *target);
/** @brief Launch only HTTP(S), with an argument vector and never through a shell.
 * @details Reports launcher failure (0 with errno); browser loading happens
 * asynchronously and is not confirmed. */
uint8_t linksOpenWeb(const char *target);
/** Open an absolute local file with the default desktop application.
 * Success confirms launcher execution only; never invokes a shell. */
uint8_t linksIsImagePath(const char *path);
/** Check regular, non-executable image content against its filename signature.
 * SVG is intentionally excluded because it can contain active content. */
uint8_t linksVerifyImageFile(const char *path);
uint8_t linksOpenFile(const char *path);
#endif
