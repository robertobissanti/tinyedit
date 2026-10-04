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

/**
 * @brief Estimate a UTF-8 sequence length from its leading byte.
 *
 * @details This does not validate successor bytes; use utf8DecodeChar() for
 * that.
 * @return 1-4; invalid leads are treated as one byte.
 */
int32_t utf8ByteLen(uint8_t c);

/* One decoding step. Callers must check valid before using codepoint;
 * consumed is also the safe distance to the next byte on an error. */
struct utf8DecodeResult {
    uint32_t codepoint;
    size_t consumed;
    uint8_t valid;
};

/**
 * @brief Decode one Unicode scalar without reading beyond the available bytes.
 *
 * @details s supplies available bytes.
 * @param s Input bytes; no NUL terminator is required.
 * @param available Number of readable bytes starting at s.
 * @return valid plus the codepoint and consumed count; an invalid prefix
 * consumes one byte, while empty input consumes zero.
 */
struct utf8DecodeResult utf8DecodeChar(const char *s, size_t available);

/**
 * @brief Measure the grapheme immediately before a cursor position.
 *
 * @details pos is a byte boundary within buf.
 * @return its byte length, or zero at the start; malformed bytes remain
 * independent editing units.
 */
size_t utf8PrevCharLen(const char *buf, size_t pos);

/**
 * @brief Measure the grapheme starting at a byte position.
 *
 * @details buf contains len bytes and pos is a character boundary.
 * @return a byte step, zero at or beyond the end, or one for malformed input;
 * never reads past len.
 */
size_t utf8NextCharLen(const char *buf, size_t pos, size_t len);

/**
 * @brief Estimate the screen columns occupied by a Unicode code point.
 *
 * @return 0, 1 or 2 using the supported control, combining and wide-character
 * ranges; terminal font behavior can differ from this estimate.
 */
int32_t utf8CharWidth(uint32_t cp);

/**
 * @brief Measure text in display columns while ignoring ANSI CSI escapes.
 *
 * @details s contains len bytes.
 * @return columns rather than bytes; malformed bytes occupy one cell and
 * supported joined emoji avoid repeated width counts.
 */
size_t utf8StrWidth(const char *s, size_t len);

/**
 * @brief Measure the base width at the start of a grapheme span.
 *
 * @details s contains len bytes, normally supplied by utf8NextCharLen().
 * @return zero for an empty span and one for a malformed first byte.
 */
int32_t utf8SingleCharWidth(const char *s, size_t len);

#endif /* __TE_UTF8_H */
