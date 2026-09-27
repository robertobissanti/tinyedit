/* utf8.h -- UTF-8 decoding, grapheme-cluster boundaries, and terminal
 * display-width calculation, extracted from antirez's linenoise
 * (https://github.com/antirez/linenoise), where this logic lives as a
 * self-contained block of pure functions used to make line editing
 * correct on multi-byte and wide characters.
 *
 * Ported here because tinyedit needs the same primitives for cursor
 * movement and rendering, and reimplementing them would just reproduce
 * the same (non-trivial) logic worse.
 */

#ifndef __TE_UTF8_H
#define __TE_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Expected length for a valid leading byte; invalid leads count as one. */
int32_t utf8ByteLen(uint8_t c);

/* One decoding step. Callers must check valid before using codepoint;
 * consumed is also the safe distance to the next byte on an error. */
struct utf8DecodeResult {
    uint32_t codepoint;
    size_t consumed;
    uint8_t valid;
};

/* Decode at most available bytes. Valid input returns its scalar value and
 * length. Empty input consumes zero bytes; malformed or truncated input
 * returns valid=0 and consumes one byte. The input bytes are never changed. */
struct utf8DecodeResult utf8DecodeChar(const char *s, size_t available);

/* Byte length of the grapheme cluster ending at byte offset `pos` in
 * `buf` (i.e. the cluster the cursor would delete/skip moving left).
 * A grapheme cluster is a base character plus any following variation
 * selectors, skin-tone modifiers, ZWJ-joined characters, combining
 * marks, or a paired regional indicator (flag emoji). */
size_t utf8PrevCharLen(const char *buf, size_t pos);

/* Byte length of the grapheme cluster starting at byte offset `pos` in
 * `buf` (bounded by `len`), i.e. the cluster the cursor would skip
 * moving right. */
size_t utf8NextCharLen(const char *buf, size_t pos, size_t len);

/* Terminal display width (in columns) of a single Unicode codepoint:
 * 0 for control/zero-width/combining characters, 2 for wide characters
 * (CJK, fullwidth forms, most emoji), 1 otherwise. Not a full wcwidth()
 * implementation, but a practical heuristic. */
int32_t utf8CharWidth(uint32_t cp);

/* Terminal display width of a UTF-8 string of `len` bytes, honoring
 * grapheme clusters (a character right after a ZWJ contributes 0
 * width, since it's joined to the previous glyph) and skipping ANSI
 * CSI escape sequences (treated as zero width). */
size_t utf8StrWidth(const char *s, size_t len);

/* Display width of a single UTF-8 character at `s` (of byte length
 * `len`, e.g. from utf8NextCharLen). */
int32_t utf8SingleCharWidth(const char *s, size_t len);

#endif /* __TE_UTF8_H */
