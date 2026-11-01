#ifndef TE_SMART_TABLE_H
#define TE_SMART_TABLE_H

#include "tinyedit.h"

#define SMART_TABLE_COLUMNS 128
#define SMART_TABLE_ROWS 1024
#define SMART_TABLE_BYTES (1024 * 1024)

struct smartTableCell { int32_t start, end, open, close; uint8_t left, right, separator; };
struct smartTableRow {
    int32_t count;
    struct smartTableCell cells[SMART_TABLE_COLUMNS];
    uint8_t separator, empty;
};
struct smartTable { int32_t first, last, columns; };

/* Conservative source parser: outer pipes, balanced code spans, no indentation.
 * Offsets are source bytes; returned formatted strings belong to the caller.
 * Format requires validated parse/width results and tab_stop in [1,16]. */
uint8_t smartTableParse(const erow *row, struct smartTableRow *parsed);
uint8_t smartTableFind(const struct editorBuffer *buffer, int32_t y, struct smartTable *table);
char *smartTableFormat(const erow *row, const struct smartTableRow *parsed,
    const int32_t *widths, const uint8_t *align, int32_t tab_stop, int32_t *cursor, int32_t *anchor);
uint8_t smartTableWidths(const struct editorBuffer *buffer, const struct smartTable *table,
    int32_t tab_stop, int32_t *widths);

#endif
