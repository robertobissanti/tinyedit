#ifndef __BUFFER_H
#define __BUFFER_H

#include "tinyedit.h"

int32_t bufferRowCxToRx(const erow *row, int32_t cx, int32_t tab_stop);
int32_t bufferRowRxToCx(const erow *row, int32_t target_rx, int32_t tab_stop);
void bufferInsertRow(struct editorBuffer *buffer, int32_t at, const char *text, size_t len);
void bufferDeleteRow(struct editorBuffer *buffer, int32_t at);
void bufferFreeRow(erow *row);
void bufferClear(struct editorBuffer *buffer);
void bufferRowInsertByte(erow *row, int32_t at, int32_t byte);
void bufferRowInsert(erow *row, int32_t at, const char *text, size_t len);
void bufferRowAppend(erow *row, const char *text, size_t len);
void bufferRowDeleteByte(erow *row, int32_t at);
void bufferRowDeleteRange(erow *row, int32_t start, int32_t end);
char *bufferSerialize(const struct editorBuffer *buffer, enum lineEndingMode ending,
    size_t *out_len);
char *bufferSerializeRange(const struct editorBuffer *buffer, int32_t start_y,
    int32_t start_x, int32_t end_y, int32_t end_x, size_t *out_len);

#endif
