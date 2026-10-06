#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

int tinyeditBenchmarkApplicationMain(int argc, char **argv);
#define main tinyeditBenchmarkApplicationMain
#include "../src/tinyedit.c"
#undef main

static double elapsedMilliseconds(clock_t start, int32_t repetitions) {
    return 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC / repetitions;
}

int main(void) {
    const int32_t sizes[] = {1000, 10000, 50000};
    puts("rows,draw_ms,count_ms,pair_ms,snapshot_ms,snapshot_bytes,delta_edit_ms,delta_200_bytes");
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        settingsDefaults(&S);
        S.show_line_numbers = 0;
        S.soft_wrap = 72;
        E.view.screencols = 80;
        E.view.screenrows = 40;
        E.search.search_match_y = -1;
        E.search.search_match_end_y = -1;
        const char *line = "abcdef é界 👩🏽‍💻 0123456789 abcdefghijklmnopqrstuvwxyz 0123456789";
        for (int32_t i = 0; i < sizes[k]; i++)
            bufferInsertRow(&E.document.buffer, i, line, strlen(line));
        editorUpdateAllRows();
        int32_t wrapcols = editorSoftWrapCols();
        editorTotalVideoRows(wrapcols);
        E.view.rowoff = sizes[k] / 2;
        clock_t start = clock();
        for (int32_t i = 0; i < 20; i++) {
            struct abuf ab = ABUF_INIT;
            editorDrawRows(&ab);
            abFree(&ab);
        }
        double draw_ms = elapsedMilliseconds(start, 20);
        start = clock();
        volatile int32_t count = 0;
        for (int32_t i = 0; i < 20; i++) count = editorCountChars();
        double count_ms = elapsedMilliseconds(start, 20);
        (void)count;
        editorRowInsertString(&E.document.buffer.rows[0], 0, "(", 1);
        editorRowInsertString(&E.document.buffer.rows[sizes[k] - 1],
            E.document.buffer.rows[sizes[k] - 1].size, ")", 1);
        E.document.cursor.cy = 0;
        E.document.cursor.cx = 0;
        start = clock();
        for (int32_t i = 0; i < 20; i++) {
            int32_t ay, ax, my, mx;
            editorMatchingPairAtCursor(&ay, &ax, &my, &mx);
        }
        double pair_ms = elapsedMilliseconds(start, 20);
        size_t snapshot_bytes = sizeof(undoRow) * (size_t)sizes[k];
        for (int32_t y = 0; y < sizes[k]; y++) snapshot_bytes += (size_t)E.document.buffer.rows[y].size + 1;
        start = clock();
        for (int32_t i = 0; i < 5; i++) {
            undoSnapshot snapshot = historyMakeSnapshot(&E.document);
            historyFreeSnapshot(&snapshot);
        }
        double snapshot_ms = elapsedMilliseconds(start, 5);
        historySetBudget(&E.document, HISTORY_DEFAULT_BUDGET);
        start = clock();
        for (int32_t i = 0; i < 200; i++) {
            historyRecordEdit(&E.document, 200, EDIT_OTHER, i);
            bufferRowInsertByte(&E.document.buffer.rows[0], 0, 'x');
            historyFinishEdit(&E.document);
        }
        double delta_ms = elapsedMilliseconds(start, 200);
        printf("%d,%.6f,%.6f,%.6f,%.6f,%zu,%.6f,%zu\n", sizes[k], draw_ms,
            count_ms, pair_ms, snapshot_ms, snapshot_bytes, delta_ms, E.document.history.bytes);

        editorResetDocument();
    }
    return 0;
}
