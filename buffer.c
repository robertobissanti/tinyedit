#include "buffer.h"

#include "alloc.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

int32_t bufferRowCxToRx(const erow *row, int32_t cx, int32_t tab_stop) {
    int32_t rx = 0, pos = 0;
    while (pos < cx) {
        if (row->chars[pos] == '\t') {
            rx += tab_stop - (rx % tab_stop);
            pos++;
        } else {
            size_t len = utf8NextCharLen(row->chars, (size_t)pos, (size_t)row->size);
            if (len == 0) len = 1;
            rx += utf8SingleCharWidth(row->chars + pos, len);
            pos += (int32_t)len;
        }
    }
    return rx;
}

int32_t bufferRowRxToCx(const erow *row, int32_t target_rx, int32_t tab_stop) {
    int32_t rx = 0, pos = 0;
    if (target_rx <= 0) return 0;
    while (pos < row->size) {
        int32_t width;
        size_t len;
        if (row->chars[pos] == '\t') {
            width = tab_stop - (rx % tab_stop);
            len = 1;
        } else {
            len = utf8NextCharLen(row->chars, (size_t)pos, (size_t)row->size);
            if (len == 0) len = 1;
            width = utf8SingleCharWidth(row->chars + pos, len);
        }
        if (rx + width > target_rx) break;
        rx += width;
        pos += (int32_t)len;
    }
    return pos;
}

void bufferInsertRow(struct editorBuffer *buffer, int32_t at, const char *text, size_t len) {
    if (at < 0 || at > buffer->row_count) return;
    buffer->rows = teRealloc(buffer->rows, sizeof(erow) * (size_t)(buffer->row_count + 1));
    memmove(&buffer->rows[at + 1], &buffer->rows[at],
        sizeof(erow) * (size_t)(buffer->row_count - at));
    erow *row = &buffer->rows[at];
    memset(row, 0, sizeof(*row));
    row->size = (int32_t)len;
    row->chars = teMalloc(len + 1);
    memcpy(row->chars, text, len);
    row->chars[len] = '\0';
    row->seg_wrapcols = -1;
    buffer->row_count++;
}

void bufferFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
    free(row->seg_start);
    free(row->seg_start_rx);
}

void bufferDeleteRow(struct editorBuffer *buffer, int32_t at) {
    if (at < 0 || at >= buffer->row_count) return;
    bufferFreeRow(&buffer->rows[at]);
    memmove(&buffer->rows[at], &buffer->rows[at + 1],
        sizeof(erow) * (size_t)(buffer->row_count - at - 1));
    buffer->row_count--;
}

void bufferClear(struct editorBuffer *buffer) {
    for (int32_t i = 0; i < buffer->row_count; i++) bufferFreeRow(&buffer->rows[i]);
    free(buffer->rows);
    buffer->rows = NULL;
    buffer->row_count = 0;
}

void bufferRowInsertByte(erow *row, int32_t at, int32_t byte) {
    char value = (char)byte;
    bufferRowInsert(row, at, &value, 1);
}

void bufferRowInsert(erow *row, int32_t at, const char *text, size_t len) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = teRealloc(row->chars, (size_t)row->size + len + 1);
    memmove(&row->chars[at + (int32_t)len], &row->chars[at],
        (size_t)(row->size - at + 1));
    memcpy(&row->chars[at], text, len);
    row->size += (int32_t)len;
}

void bufferRowAppend(erow *row, const char *text, size_t len) {
    row->chars = teRealloc(row->chars, (size_t)row->size + len + 1);
    memcpy(&row->chars[row->size], text, len);
    row->size += (int32_t)len;
    row->chars[row->size] = '\0';
}

void bufferRowDeleteByte(erow *row, int32_t at) {
    bufferRowDeleteRange(row, at, at + 1);
}

void bufferRowDeleteRange(erow *row, int32_t start, int32_t end) {
    if (start < 0) start = 0;
    if (end > row->size) end = row->size;
    if (start >= end) return;
    memmove(&row->chars[start], &row->chars[end], (size_t)(row->size - end + 1));
    row->size -= end - start;
}

char *bufferSerialize(const struct editorBuffer *buffer, enum lineEndingMode ending,
    size_t *out_len) {
    size_t ending_len = ending == LINE_ENDING_CRLF ? 2 : 1, total = 0;
    for (int32_t i = 0; i < buffer->row_count; i++)
        total += (size_t)buffer->rows[i].size + ending_len;
    *out_len = total;
    char *result = teMalloc(total > 0 ? total : 1), *dst = result;
    for (int32_t i = 0; i < buffer->row_count; i++) {
        memcpy(dst, buffer->rows[i].chars, (size_t)buffer->rows[i].size);
        dst += buffer->rows[i].size;
        if (ending == LINE_ENDING_CRLF) *dst++ = '\r';
        *dst++ = '\n';
    }
    return result;
}

char *bufferSerializeRange(const struct editorBuffer *buffer, int32_t start_y,
    int32_t start_x, int32_t end_y, int32_t end_x, size_t *out_len) {
    size_t total = 0;
    for (int32_t y = start_y; y <= end_y; y++) {
        int32_t from = y == start_y ? start_x : 0;
        int32_t to = y == end_y ? end_x : buffer->rows[y].size;
        total += (size_t)(to - from) + (y < end_y ? 1u : 0u);
    }
    char *result = teMalloc(total + 1), *dst = result;
    for (int32_t y = start_y; y <= end_y; y++) {
        int32_t from = y == start_y ? start_x : 0;
        int32_t to = y == end_y ? end_x : buffer->rows[y].size;
        size_t len = (size_t)(to - from);
        memcpy(dst, &buffer->rows[y].chars[from], len);
        dst += len;
        if (y < end_y) *dst++ = '\n';
    }
    *dst = '\0';
    *out_len = total;
    return result;
}
