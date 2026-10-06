#include "search.h"
#include "buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t compilations;
static int testCompile(regex_t *regex, const char *pattern, int flags);
#define regcomp testCompile
#include "../src/search.c"
#undef regcomp

static int testCompile(regex_t *regex, const char *pattern, int flags) {
    compilations++;
    return regcomp(regex, pattern, flags);
}

static void check(uint8_t condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static void expectMatch(struct searchQuery *query, struct editorBuffer *buffer,
    int32_t y, int32_t x, int32_t dir, uint8_t wrap,
    int32_t sy, int32_t sx, int32_t ey, int32_t ex) {
    struct searchMatch match = {-9, -9, -9, -9, -9};
    check(searchFind(query, buffer, y, x, dir, wrap, &match), "expected match exists");
    if (match.start_y != sy || match.start_x != sx || match.end_y != ey || match.end_x != ex)
        fprintf(stderr, "query %s from %d:%d dir %d: got %d:%d..%d:%d expected %d:%d..%d:%d\n",
            query->pattern, y, x, dir, match.start_y, match.start_x, match.end_y, match.end_x, sy, sx, ey, ex);
    check(match.start_y == sy && match.start_x == sx && match.end_y == ey && match.end_x == ex,
        "match source coordinates");
}

int main(void) {
    struct searchQuery query = {0};
    struct editorBuffer buffer = {0};
    struct searchMatch match = {-9, -9, -9, -9, -9};
    searchQueryPrepare(&query, "x", 0);
    check(!searchFind(&query, &buffer, 0, 0, 1, 1, &match) && match.start_y == -9,
        "empty buffer leaves result untouched");
    bufferInsertRow(&buffer, 0, "111 é 22", strlen("111 é 22"));
    bufferInsertRow(&buffer, 1, "333", 3);
    check(searchQueryPrepare(&query, "[0-9]+", 1) && compilations == 1, "compile initial regex");
    expectMatch(&query, &buffer, 0, 0, 1, 0, 0, 0, 0, 3);
    expectMatch(&query, &buffer, 0, 3, 1, 0, 0, 7, 0, 9);
    expectMatch(&query, &buffer, 0, 9, -1, 0, 0, 7, 0, 9);
    expectMatch(&query, &buffer, 0, 6, -1, 0, 0, 0, 0, 3);
    expectMatch(&query, &buffer, 0, 0, -1, 1, 0, 0, 0, 3);
    expectMatch(&query, &buffer, 1, 4, 1, 1, 0, 0, 0, 3);
    check(!searchFind(&query, &buffer, 1, 4, 1, 0, &match), "nonwrapping search stops at EOF");
    for (int32_t i = 0; i < 10; i++) {
        searchQueryPrepare(&query, "[0-9]+", 1);
        expectMatch(&query, &buffer, 1, 0, 1, 0, 1, 0, 1, 3);
    }
    check(compilations == 1, "arrows and replacements reuse compiled regex");
    check(!searchQueryPrepare(&query, "[", 1), "invalid regex rejected");
    int32_t before = compilations;
    check(!searchQueryPrepare(&query, "[", 1) && compilations == before,
        "invalid regex is cached without retries");
    check(!searchFind(&query, &buffer, 0, 0, 1, 1, &match), "invalid regex has no match");
    searchQueryPrepare(&query, "é", 0);
    expectMatch(&query, &buffer, 0, 0, 1, 0, 0, 4, 0, 6);
    expectMatch(&query, &buffer, 1, 3, -1, 0, 0, 4, 0, 6);
    searchQueryPrepare(&query, "22\\n333", 1);
    expectMatch(&query, &buffer, 0, 0, 1, 0, 0, 7, 1, 3);
    const char *cached_text = query.text;
    before = compilations;
    expectMatch(&query, &buffer, 1, 3, -1, 0, 0, 7, 1, 3);
    searchQueryPrepare(&query, "22\\n333", 1);
    check(query.text == cached_text && compilations == before, "unchanged document/query retain caches");
    bufferRowDeleteRange(&buffer.rows[1], 0, 3);
    bufferRowInsert(&buffer.rows[1], 0, "444", 3);
    searchInvalidateText(&query);
    check(!searchFind(&query, &buffer, 0, 0, 1, 1, &match) && compilations == before,
        "text mutation invalidates text but retains compiled regex");
    searchQueryPrepare(&query, "22\\r444", 1);
    expectMatch(&query, &buffer, 1, 3, -1, 0, 0, 7, 1, 3);
    searchQueryPrepare(&query, "^|\\n", 1);
    expectMatch(&query, &buffer, 1, 3, -1, 0, 1, 0, 1, 0);
    check(!searchFind(&query, &buffer, 1, 4, 1, 0, &match), "zero-width multiline match does not repeat at EOF");
    searchQueryPrepare(&query, "a*", 1);
    expectMatch(&query, &buffer, 0, 6, -1, 0, 0, 6, 0, 6);
    check(strcmp(buffer.rows[0].chars, "111 é 22") == 0 && strcmp(buffer.rows[1].chars, "444") == 0,
        "engine leaves document source untouched");
    searchQueryPrepare(&query, "", 0);
    check(!searchFind(&query, &buffer, 0, 0, 1, 1, &match), "empty query has no match");
    searchQueryFree(&query);
    searchQueryFree(&query);
    bufferClear(&buffer);
    puts("search tests: ok");
    return 0;
}
