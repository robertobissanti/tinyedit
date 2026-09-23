/* utf8.c -- see utf8.h.
 *
 * Ported from linenoise.c (antirez/linenoise, BSD license), where this
 * block of pure functions implements UTF-8 decoding, grapheme-cluster
 * boundary detection, and terminal display-width calculation for
 * correct line editing on multi-byte and wide characters.
 *
 * Original copyright:
 *   Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
 *   Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 */

#include "utf8.h"

/* UTF-8 byte length from the leading byte. See the four standard
 * patterns: 0xxxxxxx (1), 110xxxxx (2), 1110xxxx (3), 11110xxx (4). */
int32_t utf8ByteLen(uint8_t c) {
    if ((c & 0x80) == 0)    return 1;   /* 0xxxxxxx: ASCII */
    if ((c & 0xE0) == 0xC0) return 2;   /* 110xxxxx: 2-byte seq */
    if ((c & 0xF0) == 0xE0) return 3;   /* 1110xxxx: 3-byte seq */
    if ((c & 0xF8) == 0xF0) return 4;   /* 11110xxx: 4-byte seq */
    return 1; /* Fallback for invalid encoding, treat as single byte. */
}

uint32_t utf8DecodeChar(const char *s, size_t *len) {
    unsigned char *p = (unsigned char *)s;
    uint32_t cp;

    if ((*p & 0x80) == 0) {
        *len = 1;
        return *p;
    } else if ((*p & 0xE0) == 0xC0) {
        *len = 2;
        cp = (uint32_t)(*p & 0x1F) << 6;
        cp |= (p[1] & 0x3F);
        return cp;
    } else if ((*p & 0xF0) == 0xE0) {
        *len = 3;
        cp = (uint32_t)(*p & 0x0F) << 12;
        cp |= (uint32_t)(p[1] & 0x3F) << 6;
        cp |= (p[2] & 0x3F);
        return cp;
    } else if ((*p & 0xF8) == 0xF0) {
        *len = 4;
        cp = (uint32_t)(*p & 0x07) << 18;
        cp |= (uint32_t)(p[1] & 0x3F) << 12;
        cp |= (uint32_t)(p[2] & 0x3F) << 6;
        cp |= (p[3] & 0x3F);
        return cp;
    }
    *len = 1;
    return *p; /* Fallback for invalid sequences. */
}

/* Variation selector (emoji style modifiers). */
static uint8_t isVariationSelector(uint32_t cp) {
    return cp == 0xFE0E || cp == 0xFE0F;  /* Text/emoji style */
}

/* Skin tone modifier. */
static uint8_t isSkinToneModifier(uint32_t cp) {
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

/* Zero Width Joiner. */
static uint8_t isZWJ(uint32_t cp) {
    return cp == 0x200D;
}

/* Regional Indicator (for flag emoji). */
static uint8_t isRegionalIndicator(uint32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

/* Combining mark or other zero-width character. */
static uint8_t isCombiningMark(uint32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) ||   /* Combining Diacriticals */
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||   /* Combining Diacriticals Extended */
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||   /* Combining Diacriticals Supplement */
           (cp >= 0x20D0 && cp <= 0x20FF) ||   /* Combining Diacriticals for Symbols */
           (cp >= 0xFE20 && cp <= 0xFE2F);     /* Combining Half Marks */
}

/* Extends the previous character (doesn't start a new grapheme). */
static uint8_t isGraphemeExtend(uint32_t cp) {
    return isVariationSelector(cp) || isSkinToneModifier(cp) ||
           isZWJ(cp) || isCombiningMark(cp);
}

/* Decode the UTF-8 codepoint ending at position 'pos' (exclusive) and
 * return its value. Also sets *cplen to the byte length of the codepoint. */
static uint32_t utf8DecodePrev(const char *buf, size_t pos, size_t *cplen) {
    if (pos == 0) {
        *cplen = 0;
        return 0;
    }
    /* Scan backwards to find the start byte. */
    size_t i = pos;
    do {
        i--;
    } while (i > 0 && (pos - i) < 4 && ((unsigned char)buf[i] & 0xC0) == 0x80);
    *cplen = pos - i;
    size_t dummy;
    return utf8DecodeChar(buf + i, &dummy);
}

size_t utf8PrevCharLen(const char *buf, size_t pos) {
    if (pos == 0) return 0;

    size_t total = 0;
    size_t curpos = pos;

    /* First, get the last codepoint. */
    size_t cplen;
    uint32_t cp = utf8DecodePrev(buf, curpos, &cplen);
    if (cplen == 0) return 0;
    total += cplen;
    curpos -= cplen;

    /* If we're at an extending character, we need to find what it extends.
     * Keep going back through the grapheme cluster. */
    while (curpos > 0) {
        size_t prevlen;
        uint32_t prevcp = utf8DecodePrev(buf, curpos, &prevlen);
        if (prevlen == 0) break;

        if (isZWJ(prevcp)) {
            /* ZWJ joins two emoji. Include the ZWJ and continue to get
             * the preceding character. */
            total += prevlen;
            curpos -= prevlen;
            /* Now get the character before ZWJ. */
            prevcp = utf8DecodePrev(buf, curpos, &prevlen);
            if (prevlen == 0) break;
            total += prevlen;
            curpos -= prevlen;
            cp = prevcp;
            continue;  /* Check if there's more extending before this. */
        } else if (isGraphemeExtend(cp)) {
            /* Current cp is an extending character; include previous. */
            total += prevlen;
            curpos -= prevlen;
            cp = prevcp;
            continue;
        } else if (isRegionalIndicator(cp) && isRegionalIndicator(prevcp)) {
            /* Two regional indicators form a flag. But we need to be careful:
             * flags are always pairs, so only join if we're at an even boundary.
             * For simplicity, just join one pair. */
            total += prevlen;
            break;
        } else {
            /* No more extending; we've found the start of the cluster. */
            break;
        }
    }

    return total;
}

size_t utf8NextCharLen(const char *buf, size_t pos, size_t len) {
    if (pos >= len) return 0;

    size_t total = 0;
    size_t curpos = pos;

    /* Get the first codepoint. */
    size_t cplen;
    uint32_t cp = utf8DecodeChar(buf + curpos, &cplen);
    total += cplen;
    curpos += cplen;

    uint8_t isRI = isRegionalIndicator(cp);

    /* Consume any extending characters that follow. */
    while (curpos < len) {
        size_t nextlen;
        uint32_t nextcp = utf8DecodeChar(buf + curpos, &nextlen);

        if (isZWJ(nextcp) && curpos + nextlen < len) {
            /* ZWJ: include it and the following character. */
            total += nextlen;
            curpos += nextlen;
            /* Get the character after ZWJ. */
            utf8DecodeChar(buf + curpos, &nextlen);
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

/* If s[] points at an ANSI CSI escape sequence (e.g. a color change like
 * ESC [ 1 ; 32 m), return its length in bytes. Otherwise return 0.
 *
 * The caller must have already verified that s[0] == ESC (0x1b). The
 * sequence layout follows ECMA-48: ESC '[', parameter bytes (0x30-0x3f),
 * intermediate bytes (0x20-0x2f), and a final byte (0x40-0x7e). */
static size_t ansiEscapeLen(const char *s, size_t len) {
    size_t i;
    if (len < 2 || s[1] != '[') return 0;
    i = 2;
    while (i < len && (unsigned char)s[i] >= 0x30 && (unsigned char)s[i] <= 0x3f) i++;
    while (i < len && (unsigned char)s[i] >= 0x20 && (unsigned char)s[i] <= 0x2f) i++;
    if (i >= len || (unsigned char)s[i] < 0x40 || (unsigned char)s[i] > 0x7e) return 0;
    return i + 1;
}

size_t utf8StrWidth(const char *s, size_t len) {
    size_t width = 0;
    size_t i = 0;
    uint8_t after_zwj = 0;  /* Track if previous char was ZWJ */

    while (i < len) {
        size_t clen;
        uint32_t cp = utf8DecodeChar(s + i, &clen);

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

        if (after_zwj) {
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

int32_t utf8SingleCharWidth(const char *s, size_t len) {
    if (len == 0) return 0;
    size_t clen;
    uint32_t cp = utf8DecodeChar(s, &clen);
    (void)clen;
    return utf8CharWidth(cp);
}
