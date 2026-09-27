#include "alloc.h"
#include "buffer.h"
#include "render.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stop at the first failed invariant, with a short label for the case. */
static void check(uint8_t condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", what);
        exit(1);
    }
}

/* Keep the expected scalar explicit: accepting the right byte count alone
 * would miss a decoder that assembles the wrong value. */
static void valid(const char *bytes, size_t length, uint32_t codepoint) {
    struct utf8DecodeResult result = utf8DecodeChar(bytes, length);
    check(result.valid && result.consumed == length &&
        result.codepoint == codepoint, "valid boundary");
}

/* Each rejection must leave the following byte available for a fresh decode. */
static void invalid(const char *bytes, size_t length) {
    struct utf8DecodeResult result = utf8DecodeChar(bytes, length);
    check(!result.valid && result.consumed == 1, "invalid boundary");
}

/* Exercise the decoder first, then the editor's byte and screen coordinates
 * with malformed bytes next to a valid multibyte character. */
int main(void) {
    const char nul[] = {0};
    check(utf8DecodeChar(nul, 0).consumed == 0, "empty buffer");
    valid(nul, 1, 0);
    valid("\x7f", 1, 0x7f);
    valid("\xc2\x80", 2, 0x80);
    valid("\xdf\xbf", 2, 0x7ff);
    valid("\xe0\xa0\x80", 3, 0x800);
    valid("\xe1\x80\x80", 3, 0x1000);
    valid("\xec\xbf\xbf", 3, 0xcfff);
    valid("\xed\x80\x80", 3, 0xd000);
    valid("\xed\x9f\xbf", 3, 0xd7ff);
    valid("\xee\x80\x80", 3, 0xe000);
    valid("\xef\xbf\xbf", 3, 0xffff);
    valid("\xf0\x90\x80\x80", 4, 0x10000);
    valid("\xf1\x80\x80\x80", 4, 0x40000);
    valid("\xf3\xbf\xbf\xbf", 4, 0xfffff);
    valid("\xf4\x80\x80\x80", 4, 0x100000);
    valid("\xf4\x8f\xbf\xbf", 4, 0x10ffff);

    const char *bad[] = {"\x80", "\xbf", "\xc0\x80", "\xc1\xbf",
        "\xf5\x80\x80\x80", "\xff", "\xe0\x9f\xbf",
        "\xed\xa0\x80", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
        "\xc2\x41", "\xe1\x80\x41", "\xf1\x80\x80\x41",
        "\xe0\xc0\x80", "\xed\xa0\xbf", "\xf4\x8f\xbf\xc0"};
    const size_t bad_lengths[] = {1, 1, 2, 2, 4, 1, 3, 3, 4, 4, 2, 3, 4, 3, 3, 4};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        invalid(bad[i], bad_lengths[i]);

    const char *complete[] = {"\xc2\x80", "\xe0\xa0\x80", "\xf0\x90\x80\x80"};
    for (size_t i = 0; i < 3; i++) {
        for (size_t length = 1; length < i + 2; length++) {
            char *prefix = teMalloc(length);
            memcpy(prefix, complete[i], length);
            invalid(prefix, length);
            free(prefix);
        }
    }

    const char text[] = {'A', (char)0xe2, (char)0x82, 'B',
        (char)0xc3, (char)0xa9, (char)0xff, 'C'};
    const size_t steps[] = {1, 1, 1, 1, 2, 1, 1};
    size_t at = 0;
    for (size_t i = 0; i < 7; i++) {
        check(utf8NextCharLen(text, at, sizeof(text)) == steps[i], "forward navigation");
        at += steps[i];
    }
    check(at == sizeof(text), "forward end");
    for (size_t i = 7; i > 0; i--) {
        check(utf8PrevCharLen(text, at) == steps[i - 1], "backward navigation");
        at -= steps[i - 1];
    }
    check(at == 0, "backward end");
    check(utf8StrWidth(text, sizeof(text)) == 7, "malformed width");

    struct editorBuffer buffer = {0, NULL};
    bufferInsertRow(&buffer, 0, text, sizeof(text));
    erow *row = &buffer.rows[0];
    check(bufferRowCxToRx(row, 6, 4) == 5, "file offset to screen column");
    check(bufferRowRxToCx(row, 5, 4) == 6, "screen column to file offset");
    row->render = teMalloc(sizeof(text));
    memcpy(row->render, text, sizeof(text));
    row->rsize = (int32_t)sizeof(text);
    check(renderRowSegments(row, 5) == 2, "malformed wrap count");
    check(row->seg_start[1] == 6 && row->seg_start_rx[1] == 5,
        "render offset and column at wrap");

    size_t serialized_length;
    char *serialized = bufferSerialize(&buffer, LINE_ENDING_LF, 1, &serialized_length);
    check(serialized_length == sizeof(text) + 1 &&
        memcmp(serialized, text, sizeof(text)) == 0, "original bytes serialized");
    free(serialized);
    bufferRowDeleteRange(row, 1, 1 + (int32_t)utf8NextCharLen(row->chars, 1, (size_t)row->size));
    check(row->size == (int32_t)sizeof(text) - 1 &&
        (uint8_t)row->chars[1] == 0x82, "one malformed byte deleted");
    bufferClear(&buffer);
    puts("utf8 tests: ok");
    return 0;
}
