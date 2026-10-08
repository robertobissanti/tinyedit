/* utf8.c -- see utf8.h.
 *
 * Grapheme grouping and display-width heuristics were ported from
 * linenoise.c (antirez/linenoise, BSD license). The bounded, strict decoder
 * is original to tinyedit; it preserves malformed input as independent
 * bytes while keeping navigation and display within the supplied bounds.
 *
 * Original copyright:
 *   Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
 *   Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 */

#include "utf8.h"

/**
 * @brief Estimate a UTF-8 sequence length from its leading byte.
 *
 * @details This does not validate successor bytes; use utf8DecodeChar() for
 * that.
 * @return 1-4; invalid leads are treated as one byte.
 */
int32_t utf8ByteLen(uint8_t c) {
    if ((c & 0x80) == 0)    return 1;   /* 0xxxxxxx: ASCII */
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 1; /* Fallback for invalid encoding, treat as single byte. */
}

/**
 * @brief Decode one Unicode scalar without reading beyond the available bytes.
 *
 * @details s supplies available bytes.
 * @param s Input bytes; no NUL terminator is required.
 * @param available Number of readable bytes starting at s.
 * @return valid plus the codepoint and consumed count; an invalid prefix
 * consumes one byte, while empty input consumes zero.
 */
struct utf8DecodeResult utf8DecodeChar(const char *s, size_t available) {
    struct utf8DecodeResult result = {0, 0, 0};
    if (available == 0) return result;

    const uint8_t *p = (const uint8_t *)s;
    result.consumed = 1;
    if (p[0] <= 0x7f) {
        result.codepoint = p[0];
        result.valid = 1;
        return result;
    }

    size_t length;
    uint8_t second_min = 0x80, second_max = 0xbf;
    uint32_t cp;
    if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        length = 2;
        cp = p[0] & 0x1f;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        length = 3;
        cp = p[0] & 0x0f;
        if (p[0] == 0xe0) second_min = 0xa0;
        if (p[0] == 0xed) second_max = 0x9f;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        length = 4;
        cp = p[0] & 0x07;
        if (p[0] == 0xf0) second_min = 0x90;
        if (p[0] == 0xf4) second_max = 0x8f;
    } else {
        return result;
    }
    if (available < length || p[1] < second_min || p[1] > second_max)
        return result;
    for (size_t i = 2; i < length; i++)
        if (p[i] < 0x80 || p[i] > 0xbf) return result;
    cp = (cp << 6) | (p[1] & 0x3f);
    for (size_t i = 2; i < length; i++) cp = (cp << 6) | (p[i] & 0x3f);
    result.codepoint = cp;
    result.consumed = length;
    result.valid = 1;
    return result;
}

/**
 * @brief Recognize the text and emoji presentation selectors used by this module.
 *
 * @details cp is a decoded Unicode code point.
 * @return 1 for U+FE0E or U+FE0F, otherwise 0.
 */
static uint8_t isVariationSelector(uint32_t cp) {
    return cp == 0xFE0E || cp == 0xFE0F;  /* Text/emoji style */
}

/**
 * @brief Recognize an emoji skin-tone modifier.
 *
 * @details cp is a decoded Unicode code point.
 * @return 1 for U+1F3FB through U+1F3FF.
 */
static uint8_t isSkinToneModifier(uint32_t cp) {
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

/**
 * @brief Recognize the joiner used to combine emoji components.
 *
 * @details cp is a decoded Unicode code point.
 * @return 1 for U+200D.
 */
static uint8_t isZWJ(uint32_t cp) {
    return cp == 0x200D;
}

/**
 * @brief Recognize a regional indicator used in flag emoji.
 *
 * @details cp is a decoded Unicode code point.
 * @return 1 for U+1F1E6 through U+1F1FF.
 */
static uint8_t isRegionalIndicator(uint32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/**
 * @brief Recognize combining marks in the ranges supported by the editor.
 *
 * @details cp is a decoded Unicode code point.
 * @return 1 for the listed combining blocks; this is not a complete Unicode
 * property database.
 */
static uint8_t isCombiningMark(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||   /* Combining Diacriticals */
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||   /* Combining Diacriticals Extended */
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||   /* Combining Diacriticals Supplement */
           (cp >= 0x20D0 && cp <= 0x20FF) ||   /* Combining Diacriticals for Symbols */
           (cp >= 0xFE20 && cp <= 0xFE2F);     /* Combining Half Marks */
}

/**
 * @brief Decide whether a supported code point extends an adjacent grapheme.
 *
 * @return 1 for the selectors, modifiers, joiner and combining marks
 * recognized by this module.
 */
static uint8_t isGraphemeExtend(uint32_t cp) {
    return isVariationSelector(cp) || isSkinToneModifier(cp) ||
           isZWJ(cp) || isCombiningMark(cp);
}

/**
 * @brief Decode the scalar ending immediately before a byte position.
 *
 * @details buf contains at least pos bytes.
 * @return a valid suffix of up to four bytes when possible, otherwise decodes
 * the last byte independently; pos zero returns an empty result.
 */
static struct utf8DecodeResult utf8DecodePrev(const char *buf, size_t pos) {
    if (pos == 0) {
        return utf8DecodeChar(buf, 0);
    }
    size_t first = pos > 4 ? pos - 4 : 0;
    for (size_t i = first; i < pos; i++) {
        struct utf8DecodeResult decoded = utf8DecodeChar(buf + i, pos - i);
        if (decoded.valid && decoded.consumed == pos - i) return decoded;
    }
    return utf8DecodeChar(buf + pos - 1, 1);
}

/**
 * @brief Measure the grapheme immediately before a cursor position.
 *
 * @details pos is a byte boundary within buf.
 * @return its byte length, or zero at the start; malformed bytes remain
 * independent editing units.
 */
size_t utf8PrevCharLen(const char *buf, size_t pos) {
    if (pos == 0) return 0;

    size_t total = 0;
    size_t curpos = pos;

    /* First, get the last codepoint. */
    struct utf8DecodeResult decoded = utf8DecodePrev(buf, curpos);
    size_t cplen = decoded.consumed;
    uint32_t cp = decoded.codepoint;
    if (cplen == 0) return 0;
    total += cplen;
    curpos -= cplen;

    /* If we're at an extending character, we need to find what it extends.
     * Keep going back through the grapheme cluster. */
    while (curpos > 0) {
        struct utf8DecodeResult previous = utf8DecodePrev(buf, curpos);
        size_t prevlen = previous.consumed;
        uint32_t prevcp = previous.codepoint;
        if (prevlen == 0) break;

        if (!decoded.valid || !previous.valid) break;

        if (isZWJ(prevcp)) {
            /* ZWJ joins two emoji. Include the ZWJ and continue to get
             * the preceding character. */
            /* Now get the character before ZWJ. */
            struct utf8DecodeResult before_joiner = utf8DecodePrev(buf, curpos - prevlen);
            if (!before_joiner.valid) break;
            total += prevlen;
            curpos -= prevlen;
            previous = before_joiner;
            prevlen = previous.consumed;
            prevcp = previous.codepoint;
            total += prevlen;
            curpos -= prevlen;
            cp = prevcp;
            decoded = previous;
            continue;  /* Check if there's more extending before this. */
        } else if (isGraphemeExtend(cp)) {
            /* Current cp is an extending character; include previous. */
            total += prevlen;
            curpos -= prevlen;
            cp = prevcp;
            decoded = previous;
            continue;
        } else if (isRegionalIndicator(cp) && isRegionalIndicator(prevcp)) {
            /* Pair from the start of the run, consistently with forward
             * navigation; an odd final indicator is a separate grapheme. */
            size_t run = 1, scan = curpos;
            while (scan > 0) {
                struct utf8DecodeResult indicator = utf8DecodePrev(buf, scan);
                if (!indicator.valid || !isRegionalIndicator(indicator.codepoint)) break;
                run++;
                scan -= indicator.consumed;
            }
            if (run % 2 == 0) total += prevlen;
            break;
        } else {
            /* No more extending; we've found the start of the cluster. */
            break;
        }
    }

    return total;
}

/**
 * @brief Measure the grapheme starting at a byte position.
 *
 * @details buf contains len bytes and pos is a character boundary.
 * @return a byte step, zero at or beyond the end, or one for malformed input;
 * never reads past len.
 */
size_t utf8NextCharLen(const char *buf, size_t pos, size_t len) {
    if (pos >= len) return 0;

    size_t total = 0;
    size_t curpos = pos;

    /* Get the first codepoint. */
    struct utf8DecodeResult decoded = utf8DecodeChar(buf + curpos, len - curpos);
    size_t cplen = decoded.consumed;
    uint32_t cp = decoded.codepoint;
    if (!decoded.valid) return 1;
    total += cplen;
    curpos += cplen;

    uint8_t isRI = isRegionalIndicator(cp);

    /* Consume any extending characters that follow. */
    while (curpos < len) {
        struct utf8DecodeResult next = utf8DecodeChar(buf + curpos, len - curpos);
        size_t nextlen = next.consumed;
        uint32_t nextcp = next.codepoint;
        if (!next.valid) break;

        if (isZWJ(nextcp) && curpos + nextlen < len) {
            /* ZWJ: include it and the following character. */
            total += nextlen;
            curpos += nextlen;
            /* Get the character after ZWJ. */
            next = utf8DecodeChar(buf + curpos, len - curpos);
            if (!next.valid) break;
            nextlen = next.consumed;
            total += nextlen;
            curpos += nextlen;
            continue;  /* Check for more extending after the joined char. */
        } else if (isGraphemeExtend(nextcp)) {
            /* Variation selector, skin tone, combining mark, etc. */
            total += nextlen;
            curpos += nextlen;
            continue;
        } else if (isRI && isRegionalIndicator(nextcp)) {
            /* Second regional indicator for a flag pair. */
            total += nextlen;
            curpos += nextlen;
            isRI = 0;  /* Only pair once. */
            continue;
        } else {
            break;
        }
    }

    return total;
}

/**
 * @brief Estimate the screen columns occupied by a Unicode code point.
 *
 * @return 0, 1 or 2 using the supported control, combining and wide-character
 * ranges; terminal font behavior can differ from this estimate.
 */
int32_t utf8CharWidth(uint32_t cp) {
    /* Control characters and combining marks: zero width. */
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if (isCombiningMark(cp)) return 0;

    /* Grapheme-extending characters: zero width.
     * These modify the preceding character rather than taking space. */
    if (isVariationSelector(cp)) return 0;
    if (isSkinToneModifier(cp)) return 0;
    if (isZWJ(cp)) return 0;

    /* Wide character ranges - these display as 2 columns:
     * - CJK Unified Ideographs and Extensions
     * - Fullwidth forms
     * - Various emoji ranges */
    if (cp >= 0x1100 &&
        (cp <= 0x115F ||                      /* Hangul Jamo */
         cp == 0x2329 || cp == 0x232A ||      /* Angle brackets */
         (cp >= 0x231A && cp <= 0x231B) ||    /* Watch, Hourglass */
         (cp >= 0x23E9 && cp <= 0x23F3) ||    /* Various symbols */
         (cp >= 0x23F8 && cp <= 0x23FA) ||    /* Various symbols */
         (cp >= 0x25AA && cp <= 0x25AB) ||    /* Small squares */
         (cp >= 0x25B6 && cp <= 0x25C0) ||    /* Play/reverse buttons */
         (cp >= 0x25FB && cp <= 0x25FE) ||    /* Squares */
         (cp >= 0x2600 && cp <= 0x26FF) ||    /* Misc Symbols (sun, cloud, etc) */
         (cp >= 0x2700 && cp <= 0x27BF) ||    /* Dingbats */
         (cp >= 0x2934 && cp <= 0x2935) ||    /* Arrows */
         (cp >= 0x2B05 && cp <= 0x2B07) ||    /* Arrows */
         (cp >= 0x2B1B && cp <= 0x2B1C) ||    /* Squares */
         cp == 0x2B50 || cp == 0x2B55 ||      /* Star, circle */
         (cp >= 0x2E80 && cp <= 0xA4CF &&
          cp != 0x303F) ||                    /* CJK ... Yi */
         (cp >= 0xAC00 && cp <= 0xD7A3) ||    /* Hangul Syllables */
         (cp >= 0xF900 && cp <= 0xFAFF) ||    /* CJK Compatibility Ideographs */
         (cp >= 0xFE10 && cp <= 0xFE1F) ||    /* Vertical forms */
         (cp >= 0xFE30 && cp <= 0xFE6F) ||    /* CJK Compatibility Forms */
         (cp >= 0xFF00 && cp <= 0xFF60) ||    /* Fullwidth Forms */
         (cp >= 0xFFE0 && cp <= 0xFFE6) ||    /* Fullwidth Signs */
         (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||  /* Regional Indicators (flags) */
         (cp >= 0x1F300 && cp <= 0x1F64F) ||  /* Misc Symbols and Emoticons */
         (cp >= 0x1F680 && cp <= 0x1F6FF) ||  /* Transport and Map Symbols */
         (cp >= 0x1F900 && cp <= 0x1F9FF) ||  /* Supplemental Symbols */
         (cp >= 0x1FA00 && cp <= 0x1FAFF) ||  /* Chess, Extended-A */
         (cp >= 0x20000 && cp <= 0x2FFFF)))   /* CJK Extension B and beyond */
        return 2;

    return 1; /* Default: single width */
}

/**
 * @brief Measure a complete ANSI CSI sequence without exceeding the span.
 *
 * @details s must start with ESC and contain len available bytes.
 * @return its byte length, or zero for incomplete or unrecognized syntax.
 *
 * @note If s[] points at an ANSI CSI escape sequence (e.g. a color change like
 * ESC [ 1 ; 32 m), return its length in bytes. Otherwise return 0.
 *
 * The caller must have already verified that s[0] == ESC (0x1b). The sequence
 * layout follows ECMA-48: ESC '[', parameter bytes (0x30-0x3f), intermediate
 * bytes (0x20-0x2f), and a final byte (0x40-0x7e).
 */
static size_t ansiEscapeLen(const char *s, size_t len) {
    size_t i;
    if (len < 2 || s[1] != '[') return 0;
    i = 2;
    while (i < len && (unsigned char)s[i] >= 0x30 && (unsigned char)s[i] <= 0x3f) i++;
    while (i < len && (unsigned char)s[i] >= 0x20 && (unsigned char)s[i] <= 0x2f) i++;
    if (i >= len || (unsigned char)s[i] < 0x40 || (unsigned char)s[i] > 0x7e) return 0;
    return i + 1;
}

/**
 * @brief Measure text in display columns while ignoring ANSI CSI escapes.
 *
 * @details s contains len bytes.
 * @return columns rather than bytes; malformed bytes occupy one cell and
 * supported joined emoji avoid repeated width counts.
 */
size_t utf8StrWidth(const char *s, size_t len) {
    size_t width = 0;
    size_t i = 0;
    uint8_t after_zwj = 0;  /* Track if previous char was ZWJ */

    while (i < len) {
        struct utf8DecodeResult decoded = utf8DecodeChar(s + i, len - i);
        size_t clen = decoded.consumed;
        uint32_t cp = decoded.codepoint;

        /* Skip ANSI CSI escape sequences entirely: they produce no
         * glyph, so they must not contribute to the display width.
         * Checked before the ZWJ state so a stray ZWJ immediately
         * followed by ESC cannot swallow the ESC byte. */
        if (cp == 0x1b) {
            size_t skip = ansiEscapeLen(s + i, len - i);
            if (skip > 0) {
                i += skip;
                continue;
            }
        }

        if (!decoded.valid) {
            after_zwj = 0;
            width++;
        } else if (after_zwj) {
            /* Character after ZWJ: don't add width, it's joined.
             * But do check for extending chars after it. */
            after_zwj = 0;
        } else {
            width += (size_t)utf8CharWidth(cp);
        }

        /* Check if this is a ZWJ - next char will be joined. */
        if (isZWJ(cp)) {
            after_zwj = 1;
        }

        i += clen;
    }
    return width;
}

/**
 * @brief Measure the base width at the start of a grapheme span.
 *
 * @details s contains len bytes, normally supplied by utf8NextCharLen().
 * @return zero for an empty span and one for a malformed first byte.
 */
int32_t utf8SingleCharWidth(const char *s, size_t len) {
    if (len == 0) return 0;
    struct utf8DecodeResult decoded = utf8DecodeChar(s, len);
    return decoded.valid ? utf8CharWidth(decoded.codepoint) : 1;
}
