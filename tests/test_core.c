/* Exercise the actual private core so regressions cannot test a parallel
 * implementation instead of the functions reached by the application. */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "alloc.h"
#include "fileio.h"
#include "syntax.h"

static int32_t syntax_calls;
static void coreHighlightRow(erow *row, const char *filename, uint8_t enabled,
    uint8_t comment, uint8_t math, uint8_t frontmatter, int32_t index, uint8_t emphasis);
#define syntaxHighlightRow coreHighlightRow

static enum fileSaveResult coreAtomicSave(const char *filename, const char *bytes, size_t len);
int tinyeditApplicationMain(int argc, char **argv);
#define fileioAtomicSave coreAtomicSave
static uint8_t fail_core_render;
static void *coreTryMalloc(size_t bytes);
#define teTryMalloc coreTryMalloc
#define main tinyeditApplicationMain
#include "../src/tinyedit.c"
#undef main
#undef teTryMalloc

static void *coreTryMalloc(size_t bytes) {
    return fail_core_render ? NULL : teTryMalloc(bytes);
}
#undef fileioAtomicSave
#undef syntaxHighlightRow

static void coreHighlightRow(erow *row, const char *filename, uint8_t enabled,
    uint8_t comment, uint8_t math, uint8_t frontmatter, int32_t index, uint8_t emphasis) {
    syntax_calls++;
    syntaxHighlightRow(row, filename, enabled, comment, math, frontmatter, index, emphasis);
}

static uint8_t force_uncertain_save;

static enum fileSaveResult coreAtomicSave(const char *filename, const char *bytes, size_t len) {
    enum fileSaveResult result = fileioAtomicSave(filename, bytes, len);
    if (result == FILE_SAVE_DURABLE && force_uncertain_save) {
        errno = EIO;
        return FILE_SAVE_UNCERTAIN;
    }
    return result;
}

static void check(uint8_t condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static void testPromptGrowth(void) {
    size_t capacity = 128, length = 0;
    char *text = teMalloc(capacity);
    text[0] = '\0';
    for (size_t i = 0; i < 128; i++)
        editorPromptAppend(&text, &capacity, &length, "a", 1);
    check(length == 128 && capacity >= 129 && text[length] == '\0', "typed prompt boundary");
    const char *paste = "é界😀";
    for (size_t i = 0; i < 1000; i++)
        editorPromptAppend(&text, &capacity, &length, paste, strlen(paste));
    check(length == 128 + 1000 * strlen(paste) && capacity > length,
        "long pasted prompt capacity");
    for (size_t i = 0; i < 1000; i++)
        check(memcmp(text + 128 + i * strlen(paste), paste, strlen(paste)) == 0,
            "prompt retains UTF-8 bytes");
    check(text[length] == '\0', "long prompt termination");
    free(text);
}

static void testPathCompletion(void) {
    char directory[] = "/tmp/tinyedit-completion-XXXXXX";
    check(mkdtemp(directory) != NULL, "completion fixture directory");
    const char *names[] = {"é one.txt", "é two.txt", ".hidden", "folder"};
    char path[256];
    for (size_t i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (i == 3) check(mkdir(path, 0700) == 0, "completion folder");
        else {
            int fd = open(path, O_CREAT | O_WRONLY, 0600);
            check(fd >= 0, "completion file");
            close(fd);
        }
    }
    struct editorPathCompletion completion = {0};
    size_t capacity = 1, length = 0;
    char *buf = teMalloc(capacity);
    snprintf(path, sizeof(path), "%s/é", directory);
    editorPromptAppend(&buf, &capacity, &length, path, strlen(path));
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(strstr(buf, "é one.txt") != NULL && completion.count == 2,
        "UTF-8 prefix completes first sorted match with spaces");
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(strstr(buf, "é two.txt") != NULL, "Tab cycles alternatives");
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(strstr(buf, "é one.txt") != NULL, "Tab wraps alternatives");
    editorPathCompletionClear(&completion);
    length = 0;
    snprintf(path, sizeof(path), "%s/fo", directory);
    editorPromptAppend(&buf, &capacity, &length, path, strlen(path));
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(strstr(buf, "/folder/") != NULL && !completion.count,
        "unique directory adds slash and resets completion");
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(strstr(buf, "/folder/") != NULL, "empty directory leaves input intact");
    length = 0;
    snprintf(path, sizeof(path), "%s/", directory);
    editorPromptAppend(&buf, &capacity, &length, path, strlen(path));
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(completion.count == 3, "hidden files excluded without explicit dot");
    editorPathCompletionClear(&completion);
    length = 0;
    snprintf(path, sizeof(path), "%s/.", directory);
    editorPromptAppend(&buf, &capacity, &length, path, strlen(path));
    editorPathComplete(&completion, &buf, &capacity, &length);
    check(strstr(buf, "/.hidden") != NULL, "explicit dot completes hidden file");
    free(buf);
    for (size_t i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
        if (i == 3) rmdir(path); else unlink(path);
    }
    rmdir(directory);
}

static void testFilesystemTree(void) {
    char root[] = "/tmp/tinyedit-tree-XXXXXX";
    check(mkdtemp(root) != NULL, "tree fixture");
    char folder[256], nested[256], file[256], link[256];
    snprintf(folder, sizeof(folder), "%s/folder", root);
    snprintf(nested, sizeof(nested), "%s/folder/nested", root);
    snprintf(file, sizeof(file), "%s/folder/nested/é long filename.md", root);
    snprintf(link, sizeof(link), "%s/link", root);
    check(mkdir(folder, 0700) == 0 && mkdir(nested, 0700) == 0, "tree nested directories");
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    check(fd >= 0, "tree file");
    close(fd);
    check(symlink(root, link) == 0, "tree symlink cycle fixture");
    struct editorTree tree = {0};
    check(treeSetRoot(&tree, root) && tree.count == 3, "root expands only first level");
    check(tree.entries[1].directory && tree.entries[2].symlink && tree.entries[2].directory,
        "directory symlinks classified as folders");
    check(treeExpand(&tree, 2) && tree.count == 5 && tree.entries[2].expanded,
        "cyclic directory symlink expands only one level on request");
    treeCollapse(&tree, 2);
    check(treeExpand(&tree, 1) && tree.count == 4, "lazy expansion");
    check(treeExpand(&tree, 2) && tree.count == 5 && tree.entries[3].depth == 3,
        "nested depth preserved");
    tree.selected = 3;
    treeMove(&tree, INT32_MAX, 2);
    check(tree.selected == 4 && tree.scroll == 3, "tree selection and scroll bounded");
    treeMove(&tree, INT32_MIN, 2);
    check(tree.selected == 0 && tree.scroll == 0, "tree movement avoids overflow");
    tree.selected = 3;
    treeCollapse(&tree, 1);
    check(tree.count == 3 && tree.selected == 1 && !tree.entries[1].expanded,
        "collapse frees descendants and restores selection");
    check(!treeSetRoot(&tree, file) && tree.count == 3, "invalid root preserves existing tree");
    tree.visible = 1;
    check(treeWidth(&tree, 39) == 0 && treeWidth(&tree, 80) <= 40,
        "sidebar preserves at least half the terminal");
    treeClear(&tree);

    check(treeSetRoot(&T, nested), "file double-click fixture root");
    settingsDefaults(&S);
    E.view.screencols = 100; E.view.screenrows = 20;
    T.visible = 1; T.focused = 0;
    mouseEventRow = 4 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0);
    mouseEventCol = 2; mouseEventPress = 1; mouseEventButton = 0;
    tree_click_pending = 0;
    editorHandleMouseEvent();
    check(T.focused && T.selected == 1 && !E.document.file.filename,
        "single file click selects without opening");
    mouseEventPress = 0; editorHandleMouseEvent();
    mouseEventPress = 1; editorHandleMouseEvent();
    check(E.document.file.filename && strstr(E.document.file.filename, "é long filename.md"),
        "second file click opens selected file");
    editorResetDocument(); treeClear(&T);
    check(treeSetRoot(&T, root), "application tree root");
    E.view.screencols = 100;
    E.view.screenrows = 20;
    settingsDefaults(&S);
    M.open = 0;
    editorTreeKey(CTRL_KEY('e'));
    check(T.visible && T.focused, "Ctrl-E opens and focuses tree");
    editorTreeKey(CTRL_KEY('b'));
    check(T.visible && !T.focused, "Ctrl-B returns to document without closing sidebar");
    int32_t full_width = E.view.screencols - editorGutterWidth();
    check(editorTextCols() == full_width - editorSidebarWidth(), "document layout reserves sidebar columns");
    editorResetDocument();
    editorInsertChar('a');
    editorInsertChar('b');
    int32_t cy, cx;
    editorMouseToCursor(editorSidebarWidth() + editorGutterWidth() + 2,
        1 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0), &cy, &cx);
    check(cy == 0 && cx == 1, "mouse maps document after sidebar offset");
    editorTreeKey(CTRL_KEY('b'));
    editorDispatchKey('x');
    check(E.document.buffer.rows[0].size == 2, "tree typing cannot modify document");
    editorTreeKey('\x1b');
    check(T.visible && !T.focused, "Esc leaves tree visible");
    editorTreeKey(CTRL_KEY('e'));
    check(!T.visible && !T.focused, "Ctrl-E closes sidebar from document");
    editorTreeKey(CTRL_KEY('e'));
    mouseEventRow = 1 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0);
    mouseEventCol = editorSidebarWidth() - 1;
    mouseEventPress = 1;
    mouseEventButton = 0;
    editorHandleMouseEvent();
    check(!T.visible, "header close control hides sidebar");
    struct abuf hints = ABUF_INIT;
    editorDrawMessageBar(&hints);
    abAppend(&hints, "\0", 1);
    check(strstr(hints.b, "Tree:") == NULL && strstr(hints.b, "show tree") != NULL,
        "closing sidebar restores document hints");
    abFree(&hints);
    hints = (struct abuf)ABUF_INIT;
    T.visible = T.focused = 1;
    editorSetStatusMessage("Temporary message");
    E.ui.statusmsg_time = time(NULL) - 6;
    editorDrawMessageBar(&hints);
    abAppend(&hints, "\0", 1);
    check(strstr(hints.b, "Tree:") != NULL && strstr(hints.b, "Temporary message") == NULL,
        "expired notification restores tree hints");
    abFree(&hints);
    hints = (struct abuf)ABUF_INIT;
    T.focused = 0;
    editorDrawMessageBar(&hints);
    abAppend(&hints, "\0", 1);
    check(strstr(hints.b, "tree focus") != NULL && strstr(hints.b, "Tree:") == NULL,
        "default hints follow current focus");
    abFree(&hints);
    hints = (struct abuf)ABUF_INIT;
    struct timespec first = {100, 900000000}, second = {101, 100000000};
    tree_click_pending = 0;
    check(!editorTreeDoubleClick(&first, -1), "parent requires second click");
    check(editorTreeDoubleClick(&second, -1), "double click crosses second boundary");
    check(!editorTreeDoubleClick(&second, -1), "third click starts a new pair");
    second.tv_sec = 102;
    check(!editorTreeDoubleClick(&second, -1), "slow second click does not activate parent");
    tree_click_pending = 0;
    check(treeSetRoot(&T, nested), "set nested root for parent navigation");
    T.visible = 1;
    mouseEventRow = 2 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0);
    mouseEventCol = 2;
    tree_click_pending = 0;
    editorHandleMouseEvent();
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "nested") == 0,
        "single mouse click cannot change root");
    T.parent_selected = 0;
    T.selected = 0;
    editorTreeKey(ARROW_LEFT);
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "nested") == 0,
        "Left on root never changes directory");
    editorTreeKey(ARROW_UP);
    check(T.parent_selected, "Up on root selects parent entry");
    struct abuf parent_frame = ABUF_INIT;
    editorDrawSidebar(&parent_frame);
    abAppend(&parent_frame, "\0", 1);
    check(strstr(parent_frame.b, ansiColorCode(S.color_syntax_keyword)) != NULL &&
        strstr(parent_frame.b, ".. (up a dir)") != NULL,
        "parent entry is visibly selected");
    abFree(&parent_frame);
    editorTreeKey(ARROW_DOWN);
    check(!T.parent_selected && T.selected == 0, "Down from parent selects root");
    editorTreeKey(ARROW_UP);
    editorTreeKey('\r');
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "folder") == 0, "Enter on parent changes directory");
    T.selected = 1;
    editorTreeKey(ARROW_RIGHT);
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "nested") == 0,
        "Right on a directory makes it the tree root");
    check(treeSetRoot(&T, folder), "restore root for directory click");
    T.focused = 1;
    T.selected = 1;
    editorTreeKey(' ');
    check(T.entries[1].expanded && T.count == 3, "Space expands selected branch");
    editorTreeKey(' ');
    check(!T.entries[1].expanded && T.count == 2, "Space collapses selected branch");
    T.parent_selected = 1;
    editorTreeKey(' ');
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "folder") == 0,
        "Space on parent entry does not change root");
    T.parent_selected = 0;
    tree_click_pending = 0;
    mouseEventRow = 4 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0);
    mouseEventCol = 3;
    editorHandleMouseEvent();
    check(T.selected == 1 && T.entries[1].expanded && T.count == 3 &&
        strcmp(strrchr(T.entries[0].path, '/') + 1, "folder") == 0,
        "single triangle click expands directory without changing root");
    tree_click_pending = 0;
    mouseEventCol = 8;
    editorHandleMouseEvent();
    check(!T.entries[1].expanded && T.count == 2,
        "single name click collapses directory");
    check(clock_gettime(CLOCK_MONOTONIC, &tree_click_time) == 0, "double click clock");
    editorHandleMouseEvent();
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "nested") == 0,
        "double directory click changes root");
    tree_click_pending = 0;
    second.tv_sec = 101;
    check(!editorTreeDoubleClick(&first, 1) && !editorTreeDoubleClick(&second, 2),
        "clicking different directories is not a double click");
    check(treeSetRoot(&T, root), "restore root for symlink navigation");
    T.visible = T.focused = 1;
    S.color_syntax_keyword = COLOR_GREEN_LIGHT;
    S.color_syntax_preprocessor = COLOR_RED_LIGHT;
    struct abuf link_frame = ABUF_INIT;
    editorDrawSidebar(&link_frame);
    abAppend(&link_frame, "\0", 1);
    check(strstr(link_frame.b, ansiColorCode(COLOR_RED_LIGHT)) != NULL &&
        strstr(link_frame.b, ansiColorCode(COLOR_GREEN_LIGHT)) != NULL &&
        strstr(link_frame.b, "▸ link") != NULL,
        "sidebar reuses configured highlight colors and directory triangle");
    abFree(&link_frame);
    T.selected = 2;
    editorTreeKey(ARROW_RIGHT);
    check(T.selected == 0 && T.count == 3, "Right enters cyclic directory symlink safely");
    mouseEventRow = 5 + (S.show_top_bar ? 1 : 0) + (S.show_menu ? 1 : 0);
    tree_click_pending = 0;
    editorHandleMouseEvent();
    check(T.selected == 2 && T.entries[2].expanded && T.count == 5,
        "single directory symlink click expands one level");
    check(clock_gettime(CLOCK_MONOTONIC, &tree_click_time) == 0, "symlink double click clock");
    editorHandleMouseEvent();
    check(T.selected == 0 && T.count == 3 && E.document.buffer.rows[0].size == 2,
        "double click enters directory symlink and preserves dirty document");
    check(unlink(link) == 0 && symlink("folder/nested", link) == 0,
        "relative directory symlink fixture");
    check(treeSetRoot(&T, root), "reload relative symlink");
    T.selected = 2;
    editorTreeKey('\r');
    check(T.entries[2].expanded && T.count == 4,
        "Enter expands directory symlink");
    editorTreeKey(ARROW_RIGHT);
    check(strcmp(strrchr(T.entries[0].path, '/') + 1, "nested") == 0 && T.count == 2,
        "relative symlink enters target and lists children");
    check(unlink(link) == 0 && symlink(file, link) == 0, "file symlink fixture");
    check(treeSetRoot(&T, root) && T.entries[2].symlink && !T.entries[2].directory,
        "file symlink remains a file");
    check(unlink(link) == 0 && symlink("missing", link) == 0, "broken symlink fixture");
    check(treeSetRoot(&T, root) && T.entries[2].symlink && !T.entries[2].directory,
        "broken symlink remains visible without failing directory listing");
    treeClear(&T);
    T.count = T.capacity = 100;
    T.entries = teMalloc(teArrayBytes((size_t)T.count, sizeof(*T.entries)));
    for (int32_t i = 0; i < T.count; i++)
        T.entries[i] = (struct treeEntry){teStrdup("entry"), 0, 0, 0, 0};
    T.visible = T.focused = 1;
    editorTreeKey(HOME_KEY);
    check(T.parent_selected && T.scroll == 0, "Home selects first tree entry including parent");
    editorTreeKey(PAGE_DOWN);
    check(!T.parent_selected && T.selected == 17, "Page Down moves by 18 available tree rows");
    editorTreeKey(PAGE_UP);
    check(T.parent_selected && T.scroll == 0, "Page Up returns to parent entry");
    editorTreeKey(END_KEY);
    check(T.selected == 99 && T.scroll == 82, "End reveals last entry in a large tree");
    editorTreeKey(PAGE_DOWN);
    check(T.selected == 99, "Page Down stops at end");
    editorTreeKey(PAGE_UP);
    check(T.selected == 81 && T.scroll == 81, "Page Up reveals preceding page");
    editorTreeKey(HOME_KEY);
    editorTreeKey(PAGE_UP);
    check(T.parent_selected && T.scroll == 0, "Page Up stops at beginning");
    treeClear(&T);
    editorResetDocument();
    unlink(file); unlink(link); rmdir(nested); rmdir(folder); rmdir(root);

    struct abuf ab = ABUF_INIT;
    const char *name = "é界 this filename is deliberately long.tex";
    int32_t columns = editorTreeLabel(&ab, name, 16);
    check(columns <= 16 && utf8StrWidth(ab.b, (size_t)ab.len) == (size_t)columns,
        "tree abbreviation respects display columns");
    abAppend(&ab, "\0", 1);
    check(strstr(ab.b, "…") != NULL && memcmp(ab.b + ab.len - 5, ".tex", 4) == 0,
        "tree abbreviation preserves extension");
    abFree(&ab);
}

static void testUndoModified(void) {
    editorResetDocument();
    settingsDefaults(&S);
    editorInsertChar('x');
    check(E.document.file.dirty, "insertion marks unnamed document modified");
    editorUndo();
    check(!E.document.file.dirty && E.document.history.undo_count == 0,
        "undo to unnamed empty document clears modified");
    editorRedo();
    check(E.document.file.dirty, "redo restores modified");
    editorResetDocument();
    char path[] = "/tmp/tinyedit-undo-modified-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create undo saved-state fixture");
    close(fd);
    check(editorOpen(path), "open undo fixture");
    editorInsertChar('a');
    editorUndo();
    check(!E.document.file.dirty, "undo to disk content clears modified");
    editorRedo();
    check(E.document.file.dirty, "redo away from disk marks modified");
    check(editorSaveToPath(path) == FILE_SAVE_DURABLE, "save undo fixture");
    editorUndo();
    check(E.document.file.dirty && E.document.history.undo_count == 0,
        "zero undo remains modified when earlier text differs from saved file");
    editorRedo();
    check(!E.document.file.dirty, "redo back to saved text clears modified");
    editorResetDocument();
    unlink(path);
}

static void testHistoryMemoryRecovery(void) {
    editorResetDocument();
    settingsDefaults(&S);
    size_t initial_budget = E.document.history.budget;
    S.undo_memory_mb = 1;
    editorInsertChar('a');
    check(E.document.history.budget == initial_budget, "setting deferred even before first edit");
    S.undo_memory_mb = 64;
    editorInsertChar('b');
    editorUndo();
    E.document.selection.active = 1;
    fail_core_render = 1;
    editorHandleCommandKey(CTRL_KEY('y'));
    check(E.document.selection.active, "redo OOM preserves selection through dispatch");
    check(E.document.buffer.row_count == 0 && E.document.history.redo_count == 1,
        "redo render OOM leaves document and history position unchanged");
    fail_core_render = 0;
    editorRedo();
    check(!strcmp(E.document.buffer.rows[0].chars, "ab"), "redo retry succeeds");
    E.document.history.last_edit_type = EDIT_NONE;
    fail_core_render = 1;
    editorInsertChar('c');
    check(!strcmp(E.document.buffer.rows[0].chars, "ab") && E.document.cursor.cx == 2 &&
        E.document.history.undo_count == 1, "render OOM rolls back edit and cursor");
    fail_core_render = 0;
    editorInsertChar('d');
    fail_core_render = 1;
    editorUndo();
    check(!strcmp(E.document.buffer.rows[0].chars, "abd") &&
        E.document.history.undo_count == 2, "undo render OOM leaves source and stack unchanged");
    fail_core_render = 0;
    editorUndo();
    check(!strcmp(E.document.buffer.rows[0].chars, "ab"), "undo retry succeeds");
    E.document.selection.active = 1;
    E.document.selection.anchor_x = 0;
    E.document.cursor.cx = 2;
    size_t old_bytes = E.document.history.bytes;
    E.document.history.budget = old_bytes + 400;
    char text[1024]; memset(text, 'z', sizeof(text));
    editorReplaceSelectionWithText(1, 0, 0, 0, 2, text, sizeof(text));
    check(!strcmp(E.document.buffer.rows[0].chars, "ab") &&
        E.document.selection.active && E.document.cursor.cx == 2 &&
        E.document.history.bytes == old_bytes && E.document.history.redo_count == 1,
        "oversized selection replacement restores text cursor selection and redo");
    E.document.selection.active = 0;
    E.document.cursor.cx = 1;
    E.document.history.budget = sizeof(struct historyAction) + 1;
    editorHandleEditKey(DEL_KEY);
    check(E.document.cursor.cx == 1 && !strcmp(E.document.buffer.rows[0].chars, "ab"),
        "failed forward delete restores cursor before dispatch movement");
    editorResetDocument();
}

static void testTerminalOutput(void) {
    FILE *output = tmpfile();
    check(output != NULL, "terminal output fixture");
    int saved_stdout = dup(STDOUT_FILENO);
    check(saved_stdout >= 0, "save terminal output descriptor");
    check(dup2(fileno(output), STDOUT_FILENO) >= 0, "redirect terminal output");
    uint8_t written = terminalWrite("hello\x1b[m", 8);
    uint8_t empty = terminalWrite("", 0);
    check(dup2(saved_stdout, STDOUT_FILENO) >= 0, "restore terminal output");
    char bytes[8];
    rewind(output);
    check(written && empty && fread(bytes, 1, sizeof(bytes), output) == sizeof(bytes) &&
        !memcmp(bytes, "hello\x1b[m", sizeof(bytes)), "terminal writes complete output");
    fclose(output);
    int readonly = open("/dev/null", O_RDONLY);
    check(readonly >= 0 && dup2(readonly, STDOUT_FILENO) >= 0, "failing output fixture");
    uint8_t failed = !terminalWrite("x", 1);
    int saved_errno = errno;
    check(dup2(saved_stdout, STDOUT_FILENO) >= 0, "restore output after error");
    close(readonly);
    close(saved_stdout);
    check(failed && saved_errno == EBADF, "terminal output reports error without recursive exit");
}

static void testTopBar(void) {
    char filename[1024];
    memset(filename, 'a', sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    S.show_top_bar = 1;
    E.document.file.filename = filename;
    E.view.screencols = 250;
    struct abuf ab = ABUF_INIT;
    editorDrawTopBar(&ab);
    check(utf8StrWidth(ab.b, (size_t)ab.len) == 250, "long top bar bounded width");
    abFree(&ab);

    E.document.file.filename = "界e\xcc\x81😀";
    for (int32_t cols = 1; cols <= 8; cols++) {
        E.view.screencols = cols;
        ab = (struct abuf)ABUF_INIT;
        editorDrawTopBar(&ab);
        check(utf8StrWidth(ab.b, (size_t)ab.len) == (size_t)cols,
            "Unicode top bar width");
        size_t pos = 0;
        while (pos < (size_t)ab.len) {
            struct utf8DecodeResult decoded = utf8DecodeChar(ab.b + pos, (size_t)ab.len - pos);
            check(decoded.valid, "top bar does not split UTF-8");
            pos += decoded.consumed;
        }
        abFree(&ab);
    }
    S.color_statusbar = COLOR_BLUE_DARK;
    S.color_statusbar_text = COLOR_WHITE_DARK;
    for (int32_t menu = 0; menu < 2; menu++) {
        S.show_menu = menu;
        ab = (struct abuf)ABUF_INIT;
        editorDrawTopBar(&ab);
        abAppend(&ab, "", 1);
        check(strstr(ab.b, "\x1b[47m") != NULL && strstr(ab.b, "\x1b[34m") != NULL &&
            strstr(ab.b, "\x1b[107m") == NULL && strstr(ab.b, "\x1b[7m") == NULL,
            "top bar explicitly swaps dark colors with or without menu");
        abFree(&ab);
        ab = (struct abuf)ABUF_INIT;
        editorDrawStatusBar(&ab);
        abAppend(&ab, "", 1);
        check(strstr(ab.b, "\x1b[7m") == NULL,
            "bottom status bar retains its configured color orientation");
        abFree(&ab);
    }
    E.document.file.filename = "src/nested/abc.md";
    E.view.screencols = 20;
    ab = (struct abuf)ABUF_INIT;
    editorDrawTopBar(&ab);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "       abc.md       ") != NULL, "top bar title centered");
    check(strstr(ab.b, "src/") == NULL, "top bar omits relative directory");
    abFree(&ab);
    E.document.file.filename = "/tmp/project/abc.md";
    ab = (struct abuf)ABUF_INIT;
    editorDrawTopBar(&ab);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "       abc.md       ") != NULL && strstr(ab.b, "/tmp/") == NULL,
        "top bar omits absolute directory and centers basename");
    abFree(&ab);
    T.entries = teMalloc(sizeof(*T.entries));
    T.entries[0] = (struct treeEntry){teStrdup("/a/very/long/sidebar/path/selected-folder"), 0, 1, 0, 0};
    T.count = T.capacity = 1;
    T.visible = T.focused = 1;
    E.view.screencols = 80;
    E.document.file.dirty = 1;
    for (uint8_t parent = 0; parent < 2; parent++) {
        T.parent_selected = parent;
        ab = (struct abuf)ABUF_INIT;
        editorDrawTopBar(&ab);
        abAppend(&ab, "", 1);
        check(strstr(ab.b, "abc.md (modified)") != NULL &&
            strstr(ab.b, "sidebar") == NULL && strstr(ab.b, "up a dir") == NULL,
            "sidebar focus preserves document title and modified indicator");
        abFree(&ab);
    }
    E.document.file.filename = NULL;
    ab = (struct abuf)ABUF_INIT;
    editorDrawTopBar(&ab);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "[No Name] (modified)") != NULL,
        "sidebar focus preserves unnamed document title");
    abFree(&ab);
    E.document.file.dirty = 0;
    treeClear(&T);
    E.document.file.filename = NULL;
    S.show_top_bar = 0;
    E.view.screencols = 80;

    char filetype[240];
    memset(filetype, 't', sizeof(filetype) - 1);
    filetype[sizeof(filetype) - 1] = '\0';
    settingsSetFiletype("coretest", filetype);
    E.document.file.filename = "file.coretest";
    E.view.screencols = 300;
    ab = (struct abuf)ABUF_INIT;
    editorDrawStatusBar(&ab);
    abFree(&ab);
    E.document.file.filename = NULL;
}

static void testEmptyPages(void) {
    const int32_t keys[] = {PAGE_UP, PAGE_DOWN, SHIFT_PAGE_UP, SHIFT_PAGE_DOWN};
    E.view.screenrows = 20;
    E.view.screencols = 80;
    S.show_menu = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        E.view.rowoff = 12;
        E.view.coloff = 3;
        E.document.selection.active = 1;
        pending_key = keys[i];
        editorProcessKeypress();
        check(E.document.cursor.cx == 0 && E.document.cursor.cy == 0 &&
            E.document.cursor.rx == 0 && !E.document.selection.active &&
            E.view.rowoff == 0 && E.view.coloff == 0, "empty page navigation");
    }
}

static void testUnicodeTabs(void) {
    const char *prefixes[] = {"é", "界", "😀", "e\xcc\x81", "\xff"};
    const int32_t widths[] = {1, 2, 2, 1, 1};
    S.syntax_highlight = 0;
    S.show_line_numbers = 0;
    S.show_menu = 0;
    S.show_top_bar = 0;
    E.view.screencols = 80;
    for (size_t sample = 0; sample < sizeof(prefixes) / sizeof(prefixes[0]); sample++) {
        for (int32_t tab_stop = 2; tab_stop <= 8; tab_stop += 2) {
            for (int32_t visible = 0; visible <= 1; visible++) {
                S.tab_stop = tab_stop;
                S.show_invisibles = visible;
                const char *prefix = prefixes[sample];
                size_t prefix_len = strlen(prefix);
                char source[32];
                memcpy(source, prefix, prefix_len);
                memcpy(source + prefix_len, "\tX", 3);
                editorInsertRow(0, source, prefix_len + 2);
                erow *row = &E.document.buffer.rows[0];
                int32_t fill = tab_stop - widths[sample] % tab_stop;
                check(row->rsize == (int32_t)prefix_len + fill + 1,
                    "tab padding uses screen columns");
                check(bufferRowCxToRx(row, row->size, tab_stop) == widths[sample] + fill + 1,
                    "tab cursor coordinate");
                check(utf8StrWidth(row->render, (size_t)row->rsize) ==
                    (size_t)(widths[sample] + fill + 1), "render and cursor agree");
                check(memcmp(row->chars, source, prefix_len + 2) == 0,
                    "layout preserves source bytes");

                struct abuf ab = ABUF_INIT;
                editorDrawRowSegment(&ab, 0, 0, row->rsize, 1,
                    0, (int32_t)prefix_len, 0, (int32_t)prefix_len + 1,
                    0, 0, 0, 0, 0);
                char expected[64];
                const char *color = ansiColorCode(S.color_selection);
                size_t used = strlen(color);
                memcpy(expected, color, used);
                memcpy(expected + used, "\x1b[7m", 4); used += 4;
                expected[used++] = visible ? '>' : ' ';
                for (int32_t i = 1; i < fill; i++) expected[used++] = ' ';
                memcpy(expected + used, "\x1b[mX", 4); used += 4;
                uint8_t found = 0;
                for (size_t i = 0; i + used <= (size_t)ab.len; i++)
                    if (memcmp(ab.b + i, expected, used) == 0) found = 1;
                check(found, "Unicode tab selection covers all padding");
                abFree(&ab);

                int32_t cy, cx;
                editorMouseToCursor(widths[sample] + fill + 1, 1, &cy, &cx);
                check(cy == 0 && cx == (int32_t)prefix_len + 1, "click after Unicode tab");
                bufferClear(&E.document.buffer);
            }
        }
    }
    S.tab_stop = 4;
    S.show_invisibles = 0;
    const char source[] = "é\t\xcc\x81X";
    editorInsertRow(0, source, sizeof(source) - 1);
    erow *row = &E.document.buffer.rows[0];
    const char expected[] = "é   \xcc\x81X";
    check(row->rsize == (int32_t)(sizeof(expected) - 1) &&
        memcmp(row->render, expected, sizeof(expected)) == 0,
        "combining mark after tab is preserved");
    check(bufferRowCxToRx(row, row->size, S.tab_stop) ==
        (int32_t)utf8StrWidth(row->render, (size_t)row->rsize),
        "combining mark after tab uses consistent coordinates");
    bufferClear(&E.document.buffer);
}

static void testDocumentTransactions(void) {
    char directory[] = "/tmp/tinyedit-transactions-XXXXXX";
    check(mkdtemp(directory) != NULL, "create transaction fixtures");
    check(setenv("HOME", directory, 1) == 0, "isolated backup home");
    char old_path[1024], new_path[1024], uncertain_path[1024];
    snprintf(old_path, sizeof(old_path), "%s/original.txt", directory);
    snprintf(new_path, sizeof(new_path), "%s/new.txt", directory);
    snprintf(uncertain_path, sizeof(uncertain_path), "%s/uncertain.txt", directory);
    FILE *stream = fopen(old_path, "wb");
    check(stream && fwrite("original\r", 1, 9, stream) == 9 && fclose(stream) == 0,
        "create original document");
    check(editorOpen(old_path), "open original");
    editorPushUndo(EDIT_INSERT);
    editorInsertChar('Z');
    E.document.selection.active = 1;
    E.document.selection.anchor_x = 0;
    E.view.rowoff = 3;
    E.search.search_match_y = 0;
    check(backupWrite(old_path, "recovery", 8), "original recovery copy");
    struct editorDocument before = E.document;
    struct editorView before_view = E.view;
    struct editorSearch before_search = E.search;
    check(!editorOpen(directory) && errno == EISDIR, "open failure returned to caller");
    check(memcmp(&before, &E.document, sizeof(before)) == 0 &&
        memcmp(&before_view, &E.view, sizeof(before_view)) == 0 &&
        memcmp(&before_search, &E.search, sizeof(before_search)) == 0 &&
        backupExists(old_path), "failed open preserves complete active state and backup");
    check(editorSaveToPath(directory) == FILE_SAVE_FAILED, "Save as failure");
    check(memcmp(&before, &E.document, sizeof(before)) == 0 && backupExists(old_path),
        "failed Save as preserves identity, text, history and backup");
    check(backupWrite(new_path, "stale", 5), "new path recovery fixture");
    check(editorSaveToPath(new_path) == FILE_SAVE_DURABLE &&
        strcmp(E.document.file.filename, new_path) == 0 && !E.document.file.dirty &&
        !backupExists(old_path) && !backupExists(new_path), "durable Save as commits name and recovery cleanup");
    check(E.document.history.undo_count == before.history.undo_count,
        "Save as preserves undo history");
    check(backupWrite(new_path, "recovery", 8), "backup before uncertain replacement");
    check(backupWrite(uncertain_path, "recovery", 8), "destination backup before uncertainty");
    force_uncertain_save = 1;
    check(editorSaveToPath(uncertain_path) == FILE_SAVE_UNCERTAIN &&
        strcmp(E.document.file.filename, new_path) == 0 && E.document.file.dirty &&
        E.document.file.save_uncertain && backupExists(new_path) && backupExists(uncertain_path),
        "uncertain Save as keeps original identity and both recovery copies");
    check(editorDiffersFromDisk(), "uncertain durability still requires save confirmation");
    force_uncertain_save = 0;
    check(editorSaveToPath(uncertain_path) == FILE_SAVE_DURABLE &&
        !E.document.file.save_uncertain && !backupExists(new_path) && !backupExists(uncertain_path),
        "retry commits and clears uncertainty");
    editorResetDocument();
    check(editorOpen(uncertain_path), "reload durable output");
    check(E.document.buffer.rows[0].size == 10 &&
        memcmp(E.document.buffer.rows[0].chars, "Zoriginal\r", 10) == 0,
        "content CR preserved through real save and reopen");
    editorLoadLines("abc\r", 4);
    check(E.document.buffer.rows[0].size == 4 &&
        E.document.buffer.rows[0].chars[3] == '\r' && !E.document.file.final_newline,
        "recovery preserves unterminated content CR");
    editorLoadLines("abc\r\r\n", 6);
    check(E.document.buffer.rows[0].size == 4 &&
        E.document.buffer.rows[0].chars[3] == '\r' &&
        E.document.file.detected_line_ending == LINE_ENDING_CRLF,
        "recovery removes only the real CRLF terminator");
    editorResetDocument();
    unlink(old_path); unlink(new_path); unlink(uncertain_path);
    char path[1024];
    snprintf(path, sizeof(path), "%s/.tinyedit/backup", directory); rmdir(path);
    snprintf(path, sizeof(path), "%s/.tinyedit", directory); rmdir(path);
    rmdir(directory);
}

static void runInputBurst(const char *bytes, size_t len, int32_t turns) {
    int saved_stdin = dup(STDIN_FILENO);
    int descriptors[2];
    check(saved_stdin >= 0 && pipe(descriptors) == 0, "create input pipe");
    check(write(descriptors[1], bytes, len) == (ssize_t)len, "queue complete input burst");
    check(dup2(descriptors[0], STDIN_FILENO) >= 0, "attach queued input");
    close(descriptors[0]);
    for (int32_t turn = 0; turn < turns; turn++) {
        check(terminalInputReady() || pending_key >= 0, "expected event remains queued");
        editorProcessKeypress();
    }
    check(!terminalInputReady() && pending_key == -1, "burst consumed once without pending duplicates");
    check(dup2(saved_stdin, STDIN_FILENO) >= 0, "restore input");
    close(saved_stdin);
    close(descriptors[1]);
}

static void testNewDocument(void) {
    char new_home[] = "/tmp/tinyedit-new-home-XXXXXX";
    char *previous_home = getenv("HOME") ? teStrdup(getenv("HOME")) : NULL;
    check(mkdtemp(new_home) && setenv("HOME", new_home, 1) == 0, "New temporary home");
    settingsDefaults(&S);
    editorResetDocument();
    E.view.screencols = 80; E.view.screenrows = 20;
    editorInsertRow(0, "keep", 4);
    E.document.file.filename = teStrdup("/nonexistent-tinyedit-directory/file.txt");
    E.document.file.dirty = 1;
    E.document.selection.active = 1;
    E.document.selection.anchor_x = 1;
    E.document.cursor.cx = 3;
    E.view.rowoff = 2;
    int32_t count = E.document.history.undo_count;
    int saved_output = dup(STDOUT_FILENO), sink = open("/dev/null", O_WRONLY);
    check(saved_output >= 0 && sink >= 0 && dup2(sink, STDOUT_FILENO) >= 0, "New output sink");
    close(sink);
    runInputBurst("\x0e" "x", 2, 1);
    check(E.document.buffer.row_count == 1 && E.document.selection.active &&
        E.document.cursor.cx == 3 && E.view.rowoff == 2 && E.document.history.undo_count == count,
        "cancel New preserves document, selection, cursor, view and history");
    runInputBurst("\x0e" "y", 2, 1);
    check(E.document.file.dirty && E.document.selection.active && E.document.file.filename &&
        memcmp(E.document.buffer.rows[0].chars, "keep", 4) == 0, "failed save aborts New");
    T.visible = 1; T.focused = 1;
    E.search.search_match_y = 0; E.document.file.last_backup_time = 123;
    E.search.regex_mode = 1; E.search.switch_to_replace = 1;
    E.search.saved_cx = 99; E.search.last_len = 10;
    E.search.direction = -1;
    runInputBurst("\x0e" "n", 2, 1);
    check(!E.document.buffer.row_count && !E.document.file.filename && !E.document.file.dirty &&
        !E.document.selection.active && !E.document.history.undo_count && !E.document.history.redo_count &&
        E.search.search_match_y == -1 && !E.search.regex_mode && !E.search.switch_to_replace &&
        !E.search.saved_cx && !E.search.last_len && E.search.direction == 1 &&
        !E.document.file.last_backup_time &&
        !E.document.cursor.cx && !E.document.cursor.cy && !E.view.rowoff && T.visible && !T.focused,
        "discard New resets document state and focuses document while keeping sidebar");
    char saved_path[] = "/tmp/tinyedit-new-save-XXXXXX";
    int file = mkstemp(saved_path);
    check(file >= 0, "New save fixture"); close(file);
    editorInsertRow(0, "saved", 5);
    E.document.file.filename = teStrdup(saved_path); E.document.file.dirty = 1;
    check(backupWrite(saved_path, "old", 3), "New backup fixture");
    runInputBurst("\x0e" "y", 2, 1);
    check(!E.document.file.filename && !E.document.buffer.row_count && !backupExists(saved_path),
        "successful save before New clears document and old backup");
    FILE *saved = fopen(saved_path, "rb");
    char content[8] = {0};
    check(saved && fread(content, 1, sizeof(content), saved) == 6 && memcmp(content, "saved\n", 6) == 0,
        "New saves the old document before replacing it");
    fclose(saved); unlink(saved_path);
#ifdef __APPLE__
    int keys[2], saved_input = dup(STDIN_FILENO);
    check(saved_input >= 0 && pipe(keys) == 0, "Cmd-N input fixture");
    check(write(keys[1], "\x1b[110;9u", 8) == 8 && dup2(keys[0], STDIN_FILENO) >= 0,
        "Cmd-N input bytes"); close(keys[0]);
    check(terminalReadKey(1) == CTRL_KEY('n'), "experimental Cmd-N protocol mapping");
    check(dup2(saved_input, STDIN_FILENO) >= 0, "restore Cmd-N input");
    close(saved_input); close(keys[1]);
#endif
    T.visible = 0;
    check(editorMenuCommandKey(CMD_NEW) == CTRL_KEY('n'), "New menu dispatch");
    check(dup2(saved_output, STDOUT_FILENO) >= 0, "restore New output"); close(saved_output);
    char cleanup[256];
    snprintf(cleanup, sizeof(cleanup), "%s/.tinyedit/backup", new_home); rmdir(cleanup);
    snprintf(cleanup, sizeof(cleanup), "%s/.tinyedit", new_home); rmdir(cleanup);
    rmdir(new_home);
    if (previous_home) { setenv("HOME", previous_home, 1); free(previous_home); }
    else unsetenv("HOME");
}

static void testColorSettings(void) {
    settingsDefaults(&S);
    editorResetDocument();
    E.view.screencols = 80; E.view.screenrows = 20;
    struct editorSettings edited = S;
    settings_page = SETTINGS_MAIN;
    int32_t main_count = editorSettingsVisibleCount(&edited);
    check(editorSettingsDescriptorAt(&edited, main_count - 1) == -1, "main ends with Colors");
    for (int32_t i = 0; i < main_count - 1; i++)
        check(!editorSettingsIsColor(&settingDescriptors[editorSettingsDescriptorAt(&edited, i)]),
            "main hides color settings");
    check(!editorSettingsOnPage(&edited, settingsFind("markdown_heading_reverse")),
        "heading reverse hidden from main Settings");
    settings_page = SETTINGS_COLORS;
    check(editorSettingsOnPage(&edited, settingsFind("markdown_heading_reverse")),
        "heading reverse available in Colors");
    check(editorSettingsVisibleCount(&edited) > 10 && editorSettingsDescriptorAt(&edited, 0) == -4,
        "Colors contains Back, mode and two groups");
    check(strcmp(settingDescriptors[editorSettingsDescriptorAt(&edited, 1)].key, "color_mode") == 0,
        "Mode is first after Back");
    check(editorSettingsDescriptorAt(&edited, 2) == -5, "scheme follows Mode");
    int32_t ansi_count = editorSettingsVisibleCount(&edited);
    for (int32_t row = 0; row < ansi_count; row++) {
        edited.color_mode = COLOR_MODE_ANSI;
        int32_t ansi_idx = editorSettingsDescriptorAt(&edited, row);
        edited.color_mode = COLOR_MODE_RGB;
        /* RGB output is the sole extra row, after the scheme selector. */
        int32_t rgb_idx = editorSettingsDescriptorAt(&edited, row + (row > 2));
        if (ansi_idx < 0) check(rgb_idx == ansi_idx, "same color group positions in both modes");
        else {
            check(rgb_idx >= 0, "matching color setting exists in RGB");
            check((strcmp(settingDescriptors[ansi_idx].key, "markdown_heading_reverse") == 0 &&
                strcmp(settingDescriptors[rgb_idx].key, "rgb_markdown_heading_background") == 0) ||
                strcmp(settingDescriptors[ansi_idx].label, settingDescriptors[rgb_idx].label) == 0,
                "ANSI and RGB have identical color labels and order");
        }
    }
    edited.color_mode = COLOR_MODE_RGB;
    check(editorSettingsVisibleCount(&edited) == ansi_count + 1, "RGB adds output choice");
    settings_page = SETTINGS_COLORS;
    edited.syntax_highlight = 0;
    check(editorSettingsVisibleCount(&edited) > 10, "syntax palette editable while highlighting disabled");
    int32_t headings = 0;
    for (int32_t i = 1; i < editorSettingsVisibleCount(&edited); i++) {
        int32_t idx = editorSettingsDescriptorAt(&edited, i);
        if (idx == -2 || idx == -3) {
            headings++;
            check(editorSettingsMove(&edited, i - 1, 1) == i + 1, "navigation skips heading");
        } else if (idx >= 0 && editorSettingsIsColor(&settingDescriptors[idx]))
            check(settingDescriptors[idx].type == SETTING_RGB, "active RGB palette only");
    }
    check(headings == 2, "both color groups share one page");
    edited.rgb_color_background = 0x010203;
    edited.rgb_color_syntax_keyword = 0x040506;
    struct abuf frame = ABUF_INIT;
    editorSettingsRender(&frame, &edited, 1, 0, "");
    check(memcmp(frame.b, "\x1b[m", 3) == 0, "Settings starts in terminal default colors");
    check(memmem(frame.b, (size_t)frame.len, "\x1b[48;2;1;2;3m\x1b[38;2;4;5;6mprintf\x1b[m",
        strlen("\x1b[48;2;1;2;3m\x1b[38;2;4;5;6mprintf\x1b[m")) != NULL,
        "RGB syntax preview resets immediately");
    abFree(&frame);
    settings_page = SETTINGS_MAIN;
    int saved_output = dup(STDOUT_FILENO), sink = open("/dev/null", O_WRONLY);
    check(saved_output >= 0 && sink >= 0 && dup2(sink, STDOUT_FILENO) >= 0, "Settings sink"); close(sink);
    /* Unsupported Super shortcut decodes as Escape without eating the next event. */
    const char cancel[] = "\x1b[110;9u";
    char keys[512];
    size_t length = 0;
    keys[length++] = '\x1b'; keys[length++] = 'O'; keys[length++] = 'Q';
    for (int32_t i = 0; i < main_count - 1; i++) {
        memcpy(keys + length, "\x1b[B", 3); length += 3;
    }
    keys[length++] = '\r';
    memcpy(keys + length, "\x1b[B\r", 4); length += 4; /* RGB */
    memcpy(keys + length, cancel, sizeof(cancel)-1); length += sizeof(cancel)-1; /* Back */
    memcpy(keys + length, cancel, sizeof(cancel)-1); length += sizeof(cancel)-1; /* exit confirmation */
    memcpy(keys + length, cancel, sizeof(cancel)-1); length += sizeof(cancel)-1; /* cancel exit */
    memcpy(keys + length, cancel, sizeof(cancel)-1); length += sizeof(cancel)-1;
    keys[length++] = 'n';
    struct editorSettings original = S;
    runInputBurst(keys, length, 1);
    check(memcmp(&original, &S, sizeof(S)) == 0, "back, cancel exit and discard preserve live settings");
    settings_page = SETTINGS_COLORS;
    edited = S; edited.color_mode = COLOR_MODE_RGB;
    const struct settingDescriptor *rgb_descriptor = settingsFind("rgb_syntax_keyword");
    const char *rgb_inputs[] = {"\x7f\x7f\x7f\x7f\x7f\x7f\x7f#gggggg\r"
        "\x7f\x7f\x7f\x7f\x7f\x7f\x7f#123456\r",
        "\x7f\x7f\x7f\x7f\x7f\x7f\x7f#010203\x1b[110;9u"};
    for (size_t i = 0; i < 2; i++) {
        int input[2], saved_input = dup(STDIN_FILENO);
        check(pipe(input) == 0 && saved_input >= 0, "RGB editing input pipe");
        size_t bytes = strlen(rgb_inputs[i]);
        check(write(input[1], rgb_inputs[i], bytes) == (ssize_t)bytes &&
            dup2(input[0], STDIN_FILENO) >= 0, "RGB editing input"); close(input[0]);
        editorSettingsEditRgb(&edited, 1, 0, rgb_descriptor);
        check(edited.rgb_color_syntax_keyword == 0x123456, i ? "Escape restores RGB field" :
            "invalid RGB cannot be accepted; corrected RGB accepted");
        check(dup2(saved_input, STDIN_FILENO) >= 0, "restore RGB input");
        close(saved_input); close(input[1]);
    }
    char *home = getenv("HOME") ? teStrdup(getenv("HOME")) : NULL;
    check(setenv("HOME", "/nonexistent-tinyedit-directory", 1) == 0, "save failure home");
    edited = S; edited.color_mode = COLOR_MODE_RGB;
    check(!editorSettingsSave(&edited) && memcmp(&original, &S, sizeof(S)) == 0,
        "failed settings save leaves live settings unchanged");
    if (home) { setenv("HOME", home, 1); free(home); } else unsetenv("HOME");
    check(dup2(saved_output, STDOUT_FILENO) >= 0, "restore Settings output"); close(saved_output);
    settings_page = SETTINGS_MAIN;
    settingsDefaults(&S);
}

static void markdownNavigationRows(const char *const *rows, size_t count) {
    editorResetDocument();
    E.document.file.filename = teStrdup("navigation.md");
    for (size_t i = 0; i < count; i++)
        editorInsertRow(E.document.buffer.row_count, rows[i], strlen(rows[i]));
    E.document.file.dirty = 0;
    editorSetStatusMessage("");
}

static void testMarkdownNavigation(void) {
    settingsDefaults(&S);
    T.focused = T.visible = 0;
    E.view.screenrows = 20; E.view.screencols = 80;
    const char *const targets[] = {
        "## Dati", "## Titolo {#dati}", "[Dati sperimentali]{.misura #span title=\"Dati\"}",
        "![Diagramma](architettura.png){#fig:architettura .diagramma}",
        "::: {.diagramma #architettura title=\"Diagramma\"}", "Testo", ":::",
        "## Héading UTF-8 {.classe #esatto}", "Testo. ^paragrafo", "",
        "| Nome | Valore |", "| --- | --- |", "| A | 10 |", "", "^tabella", "",
        "- Primo", "- Secondo", "", "^lista", "", "::: {#minimo}", "contenuto", ":::",
        "", "Primo paragrafo", "continua qui", "", "^multilinea"
    };
    markdownNavigationRows(targets, sizeof(targets) / sizeof(*targets));
    E.document.selection.active = E.document.selection.pinned = 1;
    E.view.rowoff = E.view.coloff = E.view.free_scroll = 1;
    editorLinkAnchor("dati");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 1 &&
        !E.document.selection.active && !E.document.selection.pinned && !E.view.free_scroll,
        "explicit Pandoc ID wins over heading slug and clears navigation state");
    editorLinkAnchor("span");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 2, "Pandoc span with class and property");
    editorLinkAnchor("fig:architettura");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 3, "Pandoc image ID with colon");
    editorLinkAnchor("architettura");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 4, "Pandoc fenced Div attributes");
    editorLinkAnchor("héading-utf-8");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 7, "UTF-8 header slug omits trailing attributes");
    editorLinkAnchor("^paragrafo");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 8, "Obsidian paragraph marker");
    editorLinkAnchor("^tabella");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 14, "standalone Obsidian marker targets marker after table");
    editorLinkAnchor("^lista");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 19, "standalone Obsidian marker targets marker after list");
    editorLinkAnchor("minimo");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 21, "minimal Pandoc fenced Div ID");
    editorLinkAnchor("^multilinea");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 28, "standalone marker targets its own line after multiline paragraph");
    editorLinkAnchor("Esatto");
    check(E.ui.statusmsg_error, "explicit IDs are case sensitive");
    editorLinkAnchor("^dati");
    check(E.ui.statusmsg_error, "Obsidian lookup never falls back to Pandoc ID");
    editorLinkAnchor("missing");
    check(E.ui.statusmsg_error && strstr(E.ui.statusmsg, "missing"), "missing anchor is explicit red error");

    const char *const duplicates[] = {"[uno]{#duplicato}", "## Due {#duplicato}",
        "Testo ^blocco", "Altro ^blocco"};
    markdownNavigationRows(duplicates, sizeof(duplicates) / sizeof(*duplicates));
    editorLinkAnchor("duplicato");
    check(E.ui.statusmsg_error && E.document.cursor.cy == 0 &&
        (strstr(E.ui.statusmsg, "Duplicate") || strstr(E.ui.statusmsg, "duplicate")), "duplicate Pandoc ID refuses arbitrary navigation");
    editorLinkAnchor("^blocco");
    check(E.ui.statusmsg_error && E.document.cursor.cy == 0 &&
        (strstr(E.ui.statusmsg, "Duplicate") || strstr(E.ui.statusmsg, "duplicate")), "duplicate Obsidian ID refuses arbitrary navigation");

    const char *const code[] = {"```markdown", "## Falso {#hidden}", "Testo ^fake",
        "@real", "```", "`[Falso]{#inline}`", "`Testo ^inline-block`", "[Vero]{#real}"};
    markdownNavigationRows(code, sizeof(code) / sizeof(*code));
    S.syntax_highlight = 0;
    editorLinkAnchor("hidden"); check(E.ui.statusmsg_error, "fenced Pandoc ID excluded with highlighting disabled");
    editorLinkAnchor("^fake"); check(E.ui.statusmsg_error, "fenced block marker excluded");
    editorLinkAnchor("inline"); check(E.ui.statusmsg_error, "inline code Pandoc ID excluded");
    editorLinkAnchor("^inline-block"); check(E.ui.statusmsg_error, "inline code block marker excluded");
    struct textLink link;
    E.document.cursor.cy = 3; E.document.cursor.cx = 2;
    check(!editorCursorLink(&link), "reference inside fence excluded without highlighting");
    S.syntax_highlight = 1;

    const char *const quoted_code[] = {"> ```markdown", "> [Falso]{#quoted}",
        "> @real", "> ```", "[Vero]{#real}"};
    markdownNavigationRows(quoted_code, sizeof(quoted_code) / sizeof(*quoted_code));
    editorLinkAnchor("quoted"); check(E.ui.statusmsg_error, "quoted code fence excludes anchors");
    E.document.cursor.cy = 2; E.document.cursor.cx = 4;
    check(!editorCursorLink(&link), "quoted code fence excludes references");
    editorLinkAnchor("real");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 4, "quoted fence closes before actual target");

    const char *const lexical[] = {
        "\\[literal]{#escaped}", "\\::: {#escaped-div}", "\\@real", "[Vero]{#real}",
        "Paragrafo", "^unseparated", "", "- Primo", "", "- Secondo", "", "^loose"
    };
    markdownNavigationRows(lexical, sizeof(lexical) / sizeof(*lexical));
    editorLinkAnchor("escaped"); check(E.ui.statusmsg_error, "escaped span opener is not an anchor");
    editorLinkAnchor("escaped-div"); check(E.ui.statusmsg_error, "escaped Div opener is not an anchor");
    E.document.cursor.cy = 2; E.document.cursor.cx = 3;
    check(!editorCursorLink(&link), "escaped local reference is not navigable");
    editorLinkAnchor("^unseparated");
    check(E.ui.statusmsg_error, "standalone marker requires separating blank line");
    editorLinkAnchor("^loose");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 11, "standalone block marker targets its own line after loose list");

    const char *const continued_list[] = {"- Primo", "  continuazione", "", "^continued"};
    markdownNavigationRows(continued_list, sizeof(continued_list) / sizeof(*continued_list));
    editorLinkAnchor("^continued");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 3, "standalone marker follows list continuation text");

    const char *const references[] = {"é @fig:architettura, poi [@dati].", "[Figura]{#fig:architettura}",
        "## Dati {#dati}", "a@dati @bibliografia `@dati`", "[vai](#d%61ti)"};
    markdownNavigationRows(references, sizeof(references) / sizeof(*references));
    E.document.cursor.cx = 5;
    check(editorCursorLink(&link), "UTF-8 reference before comma is navigable");
    editorOpenCursorLink();
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 1, "reference punctuation excluded from destination");
    E.document.cursor.cy = 0; E.document.cursor.cx = 29;
    check(editorCursorLink(&link), "bracketed local reference recognized");
    editorOpenCursorLink();
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 2, "bracketed local reference resolves");
    E.document.cursor.cy = 3; E.document.cursor.cx = 3;
    check(!editorCursorLink(&link), "email is not local reference even with matching ID");
    E.document.cursor.cx = 10;
    check(!editorCursorLink(&link), "bibliographic citation without local target is not link");
    E.document.cursor.cx = 25;
    check(!editorCursorLink(&link), "inline code reference excluded");
    E.document.cursor.cy = 4; E.document.cursor.cx = 2;
    editorOpenCursorLink();
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 2, "explicit fragment retains percent decoding");

    const char *const anchor_forms[] = {
        "![Testo][#fig:figura]", "![Testo](#fig:figura)",
        "[nota](#^diag)", "[nota]\\(#^diag)", "(#^diag)",
        "![Figura](image.png){#fig:figura}", "Diagramma. ^diag",
        "`![Testo][#fig:figura]`", "\\[Testo][#fig:figura]"
    };
    markdownNavigationRows(anchor_forms, sizeof(anchor_forms) / sizeof(*anchor_forms));
    for (int32_t y = 0; y < 3; y++) {
        E.document.cursor.cy = y; E.document.cursor.cx = 3;
        check(editorCursorLink(&link), "anchor link form recognized");
        editorOpenCursorLink();
        check(!E.ui.statusmsg_error && E.document.cursor.cy == (y == 2 ? 0 : 5),
            "image anchor and block link navigate internally");
    }
    for (int32_t y = 3; y < 4; y++) {
        E.document.cursor.cy = y; E.document.cursor.cx = 3;
        check(!editorCursorLink(&link), "escaped parentheses remain literal");
    }
    E.document.cursor.cy = 4; E.document.cursor.cx = 3;
    editorOpenCursorLink();
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 0,
        "bare block fragment follows the same target as a labelled link");
    for (int32_t y = 7; y < 9; y++) {
        E.document.cursor.cy = y; E.document.cursor.cx = 4;
        check(!editorCursorLink(&link), "code and escaped reference openers remain literal");
    }

    const char *const spaced_block[] = {
        "## Tabelle", "", "| ciao | prova |", "| --- | --- |", "| uno | due |", "",
        "[ciao](#^ciao)", "", "", "", "", "", "", "", "", "", "", "", "^ciao"
    };
    markdownNavigationRows(spaced_block, sizeof(spaced_block) / sizeof(*spaced_block));
    E.document.cursor.cy = 6; E.document.cursor.cx = 3;
    editorOpenCursorLink();
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 18,
        "link before many blank lines reaches standalone marker instead of itself");
    const char *const lone_marker[] = {"^ciao", "", "(#^ciao)", "", "[ciao](#^ciao)"};
    markdownNavigationRows(lone_marker, sizeof(lone_marker) / sizeof(*lone_marker));
    for (int32_t y = 2; y <= 4; y += 2) {
        E.document.cursor.cy = y; E.document.cursor.cx = 2;
        editorOpenCursorLink();
        check(!E.ui.statusmsg_error && E.document.cursor.cy == 0,
            "labelled and bare links resolve a marker without a preceding block");
    }
    const char *const bare_code[] = {"`(#^ciao)`", "~~~", "(#^ciao)", "~~~", "", "^ciao"};
    markdownNavigationRows(bare_code, sizeof(bare_code) / sizeof(*bare_code));
    E.document.cursor.cy = 0; E.document.cursor.cx = 3;
    check(!editorCursorLink(&link), "bare fragment inside inline code is not navigable");
    E.document.cursor.cy = 2;
    check(!editorCursorLink(&link), "bare fragment inside fenced code is not navigable");

    const char *const extra[] = {
        "~~~markdown", "[hidden]{#tilde}", "@dati", "~~~", "`inizio", "[hidden]{#multicode}",
        "@dati", "fine`", "[ok]{title=\"#fake\" #dati}", "## Literal \\{#escaped-heading}",
        "@dati: @dati.extra", "[first]{#adjacent}[second]{#adjacent}"
    };
    markdownNavigationRows(extra, sizeof(extra) / sizeof(*extra));
    editorLinkAnchor("tilde"); check(E.ui.statusmsg_error, "tilde code fence excludes targets");
    editorLinkAnchor("multicode"); check(E.ui.statusmsg_error, "multiline code span excludes target");
    editorLinkAnchor("fake"); check(E.ui.statusmsg_error, "quoted attribute text is not an ID");
    editorLinkAnchor("escaped-heading"); check(E.ui.statusmsg_error, "escaped header attribute opener is literal");
    editorLinkAnchor("dati");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 8, "ID after quoted hash attribute resolves");
    E.document.cursor.cy = 2; E.document.cursor.cx = 2;
    check(!editorCursorLink(&link), "tilde fence reference excluded");
    E.document.cursor.cy = 6;
    check(!editorCursorLink(&link), "multiline inline code reference excluded");
    E.document.cursor.cy = 10; E.document.cursor.cx = 2;
    check(editorCursorLink(&link), "terminal colon punctuation allows local reference");
    E.document.cursor.cx = 5;
    check(!editorCursorLink(&link), "trimmed colon is outside local reference");
    editorLinkAnchor("adjacent");
    check(E.ui.statusmsg_error && (strstr(E.ui.statusmsg, "Duplicate") || strstr(E.ui.statusmsg, "duplicate")),
        "adjacent explicit IDs on one row still count as duplicates");

    const char *const mouse_refs[] = {"start", "@dati, [@fig:immagine]", "[Dati]{#dati}", "![Fig](a.png){#fig:immagine}"};
    markdownNavigationRows(mouse_refs, sizeof(mouse_refs) / sizeof(*mouse_refs));
    S.markdown_live_preview = S.mouse_enabled = 1;
    S.show_line_numbers = S.show_menu = S.show_top_bar = 0;
    E.view.rowoff = E.view.coloff = 0;
    E.search.search_match_y = E.search.search_match_end_y = -1;
    struct abuf reference_frame = ABUF_INIT;
    document_pointer_valid = 0; hovered_link_row = -1;
    editorDrawRowSegment(&reference_frame, 1, 0, E.document.buffer.rows[1].rsize,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    abAppend(&reference_frame, "\0", 1);
    check(strstr(reference_frame.b, "\x1b[4m") != NULL,
        "local references are underlined without pointer hover");
    abFree(&reference_frame);
    mouseEventButton = 35; mouseEventPress = 1; mouseEventCol = 2; mouseEventRow = 2;
    editorHandleMouseEvent();
    editorUpdateLinkHover();
    check(hovered_link_row == 1 && E.document.cursor.cy == 0, "hover recognizes local reference without moving cursor");
    mouseEventButton = 0;
    editorHandleMouseEvent();
    check(E.document.cursor.cy == 2 && !E.document.mouse.dragging && !E.document.selection.active,
        "single preview click follows local reference");
    E.document.cursor.cy = 0; E.document.cursor.cx = 0;
    mouseEventButton = 35; mouseEventCol = 11;
    editorHandleMouseEvent();
    editorUpdateLinkHover();
    check(hovered_link_row == 1, "hover recognizes bracketed colon reference");
    mouseEventButton = 0; editorHandleMouseEvent();
    check(E.document.cursor.cy == 3, "single preview click follows bracketed colon reference");
    settingsDefaults(&S);

    char root[] = "/tmp/tinyedit-anchors-XXXXXX";
    check(mkdtemp(root) != NULL, "cross-file anchor fixtures");
    char source[256], destination[256];
    snprintf(source, sizeof(source), "%s/source.md", root);
    snprintf(destination, sizeof(destination), "%s/altro.md", root);
    FILE *file = fopen(source, "wb");
    check(file && fputs("[Vai](altro.md#dati)\n", file) >= 0 && fclose(file) == 0, "write explicit link source");
    file = fopen(destination, "wb");
    check(file && fputs("## Titolo {#dati}\n\nBlocco ^blocco\n![Fig](a.png){#fig:immagine}\n", file) >= 0 && fclose(file) == 0,
        "write cross-file anchor targets");
    check(editorOpen(source), "open explicit link source");
    E.document.cursor.cx = 2;
    editorOpenCursorLink();
    check(!E.ui.statusmsg_error && !strcmp(E.document.file.filename, destination) && E.document.cursor.cy == 0,
        "local file fragment resolves explicit Pandoc ID");
    check(editorOpen(source), "restore source for block link");
    editorFollowLink("altro.md#^blocco");
    check(!E.ui.statusmsg_error && !strcmp(E.document.file.filename, destination) && E.document.cursor.cy == 2,
        "local file fragment resolves Obsidian block");
    check(editorOpen(source), "restore source for colon fragment");
    editorFollowLink("altro.md#fig:immagine");
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 3, "colon in cross-file explicit fragment remains supported");
    check(editorOpen(source), "restore source for cancelled explicit link");
    E.document.cursor.cx = 2;
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorPushUndo(EDIT_INSERT); editorInsertChar('X');
    E.document.cursor.cx = 2;
    E.document.selection.active = E.document.selection.pinned = 1;
    E.document.selection.anchor_x = 1;
    int input[2], saved_input = dup(STDIN_FILENO);
    check(saved_input >= 0 && pipe(input) == 0, "cancel explicit link input");
    check(write(input[1], "x", 1) == 1 && dup2(input[0], STDIN_FILENO) >= 0, "cancel explicit link answer");
    close(input[0]);
    editorFollowLink("altro.md#fig:immagine");
    check(!strcmp(E.document.file.filename, source) && E.document.file.dirty && E.document.cursor.cx == 2 &&
        E.document.selection.active && E.document.selection.pinned && E.document.selection.anchor_x == 1,
        "cancel explicit cross-file fragment preserves dirty document and selection");
    check(dup2(saved_input, STDIN_FILENO) >= 0, "restore explicit link input");
    close(saved_input); close(input[1]);
    editorResetDocument();
    unlink(source); unlink(destination); rmdir(root);
    markdownNavigationRows(extra, sizeof(extra) / sizeof(*extra));
    E.document.cursor.cy = 10; E.document.cursor.cx = 9;
    check(!editorCursorLink(&link), "dot inside reference does not activate matching ID prefix");
    editorResetDocument();
    settingsDefaults(&S);
}

static void testMarkdownCodeBlocks(void) {
    settingsDefaults(&S);
    const char *const c_block[] = {"```c", "int answer = 42;", "/* commento", "",
        "ancora commento */", "printf(\"città\");", "```", "## Heading"};
    markdownNavigationRows(c_block, sizeof(c_block) / sizeof(*c_block));
    erow *rows = E.document.buffer.rows;
    check(rows[1].hl && rows[1].hl[0] == HL_KEYWORD && rows[1].hl[13] == HL_NUMBER,
        "fenced C uses language keywords and numbers");
    check(rows[2].hl[0] == HL_COMMENT && rows[3].markdown_code.comment && rows[4].hl[0] == HL_COMMENT,
        "embedded multiline comment survives blank code rows");
    check(rows[5].hl[0] == HL_FUNCTION && rows[5].hl[7] == HL_STRING && !rows[5].markdown_count,
        "embedded strings and calls use C highlighting without Markdown spans");
    check(!rows[6].markdown_code.marker && rows[7].hl_heading, "closing code fence restores Markdown");
    E.document.cursor.cy = 0; E.document.cursor.cx = 4;
    editorDelChar();
    check(E.document.buffer.rows[1].hl[0] == HL_STRING, "removing language tag recolors whole code block");
    editorUndo();
    check(E.document.buffer.rows[1].hl[0] == HL_KEYWORD, "undo restores fenced language highlighting");
    const char *const languages[] = {"~~~python", "def f():", "    return 42", "~~~",
        "```json", "{\"key\": 12}", "```", "```html", "<div title=\"hello\">", "```",
        "```css", "a { color: red; }", "```", "```{.javascript #example}", "const x = 3;", "```"};
    markdownNavigationRows(languages, sizeof(languages) / sizeof(*languages));
    rows = E.document.buffer.rows;
    check(rows[1].hl[0] == HL_KEYWORD && rows[2].hl[4] == HL_KEYWORD, "tilde fence supports Python alias");
    check(rows[5].hl[1] == HL_JSON_KEY && rows[5].hl[8] == HL_NUMBER, "JSON fenced keys and numbers");
    check(rows[8].hl[1] == HL_KEYWORD && rows[8].hl[11] == HL_STRING, "HTML code uses markup tokenizer");
    check(rows[11].hl[4] == HL_KEYWORD, "CSS code uses dedicated tokenizer");
    check(rows[14].hl[0] == HL_KEYWORD, "Pandoc language class selects JavaScript alias");
    const char *const boundaries[] = {"````c", "```", "int x = 1;", "~~~", "int y = 2;", "````",
        "```unknown-language", "int z = 3;", "```", "```", "int plain = 4;", "```", "**bold**"};
    markdownNavigationRows(boundaries, sizeof(boundaries) / sizeof(*boundaries));
    rows = E.document.buffer.rows;
    check(rows[2].hl[0] == HL_KEYWORD && rows[4].hl[0] == HL_KEYWORD,
        "shorter or different fence does not close embedded code");
    check(rows[7].hl[0] == HL_STRING && rows[10].hl[0] == HL_STRING && rows[12].markdown_count,
        "unknown or absent code language stays literal and does not leak into prose");
    S.syntax_highlight = 0; editorUpdateAllRows();
    check(!rows[2].hl && !rows[2].markdown_code.marker, "disabling highlighting clears embedded state");
    editorResetDocument(); settingsDefaults(&S);
}

static int saved_external_input;

static void beginExternalAnswer(char answer) {
    int input[2];
    check(pipe(input) == 0, "external confirmation pipe");
    saved_external_input = dup(STDIN_FILENO);
    check(saved_external_input >= 0 && write(input[1], &answer, 1) == 1 &&
        dup2(input[0], STDIN_FILENO) >= 0, "external confirmation input");
    close(input[0]); close(input[1]);
}

static void endExternalAnswer(void) {
    check(dup2(saved_external_input, STDIN_FILENO) >= 0, "restore external input");
    close(saved_external_input);
}

static void testLinks(void) {
    struct textLink link;
    const char *inline_link = "é [guide](<folder/a b.md#title> \"title\") end";
    check(linksFind(inline_link, (int32_t)strlen(inline_link), 5, &link), "UTF-8 Markdown label link");
    char *target = linksTarget(inline_link, &link);
    check(target && !strcmp(target, "folder/a b.md#title"), "angle target and optional title"); free(target);
    const char *nested = "[![image](a.png)](https://example.org/a(b))";
    check(linksFind(nested, (int32_t)strlen(nested), 4, &link), "nested image label");
    target = linksTarget(nested, &link);
    check(target && !strcmp(target, "https://example.org/a(b)"), "outer link and balanced destination"); free(target);
    const char *escaped = "[name](a\\(b\\).md)";
    check(linksFind(escaped, (int32_t)strlen(escaped), 2, &link), "escaped destination");
    target = linksTarget(escaped, &link);
    check(target && !strcmp(target, "a(b).md"), "Markdown destination unescaped"); free(target);
    const char *raw = "See <https://example.org/a(b)>.";
    check(linksFind(raw, (int32_t)strlen(raw), 8, &link), "raw URL");
    target = linksTarget(raw, &link);
    check(target && !strcmp(target, "https://example.org/a(b)"), "raw URL punctuation excluded"); free(target);
    const char *code = "`[skip](a.md)` [yes](b.md)";
    check(!linksFind(code, (int32_t)strlen(code), 4, &link) &&
        linksFind(code, (int32_t)strlen(code), 17, &link), "inline code excluded");
    check(!linksFind("[broken](", 9, 2, &link), "malformed link bounded");
    target = linksDecode("a%20b/%C3%A9.md");
    check(target && !strcmp(target, "a b/é.md"), "percent-encoded local path"); free(target);
    check(!linksDecode("bad%00name") && !linksDecode("bad%1bname"), "encoded controls rejected");
    target = linksHeadingSlug("## UTF-8 text", 13);
    check(!strcmp(target, "utf-8-text"), "heading slug"); free(target);
    check(linksIsWeb("https://example.org") && !linksIsWeb("file:///tmp/a") &&
        !linksIsWeb("https://example.org\n"), "only safe web schemes");

    settingsDefaults(&S);
    S.show_menu = S.show_top_bar = S.show_line_numbers = S.auto_close_pairs = 0;
    T.visible = T.focused = 0;
    E.view.screenrows = 20; E.view.screencols = 80;
    editorResetDocument();
    editorInsertRow(0, "café world", 11);
    const char double_click[] = "\x1b[<0;3;1M\x1b[<0;3;1m\x1b[<0;3;1M\x1b[<0;3;1m";
    runInputBurst(double_click, sizeof(double_click) - 1, 1);
    check(E.document.selection.active && E.document.selection.anchor_x == 0 &&
        E.document.cursor.cx == 5 && !E.document.mouse.dragging,
        "double click selects UTF-8 word at source boundaries");
    editorResetDocument();
    editorInsertRow(0, "[jump](#target)", 15);
    editorInsertRow(1, "## Target", 9);
    editorSetStatusMessage("");
    struct abuf hint = ABUF_INIT;
    editorDrawMessageBar(&hint); abAppend(&hint, "\0", 1);
    check(strstr(hint.b, "Alt+Enter open link") != NULL, "link shortcut hint shown immediately"); abFree(&hint);
    hint = (struct abuf)ABUF_INIT;
    editorSetStatusMessage("Open file: typed path");
    editorDrawMessageBar(&hint); abAppend(&hint, "\0", 1);
    check(strstr(hint.b, "Open file: typed path") && !strstr(hint.b, "Alt+Enter"), "prompt takes priority over link hint"); abFree(&hint);
    const char alt_enter_text[] = "\x1b\rZ";
    int saved_output = dup(STDOUT_FILENO), sink = open("/dev/null", O_WRONLY);
    check(saved_output >= 0 && sink >= 0 && dup2(sink, STDOUT_FILENO) >= 0, "link output sink"); close(sink);
    runInputBurst(alt_enter_text, sizeof(alt_enter_text) - 1, 2);
    check(E.document.cursor.cy == 1 && E.document.cursor.cx == 1 &&
        !strcmp(E.document.buffer.rows[1].chars, "Z## Target"),
        "Alt-Enter follows anchor without consuming next key");
    editorResetDocument();
    editorInsertRow(0, "[jump](#target)", 15);
    editorInsertRow(1, "## Target", 9);
    const char kitty_alt_enter_text[] = "\x1b[13;3uK";
    runInputBurst(kitty_alt_enter_text, sizeof(kitty_alt_enter_text) - 1, 2);
    check(E.document.cursor.cy == 1 && E.document.cursor.cx == 1 &&
        !strcmp(E.document.buffer.rows[1].chars, "K## Target"),
        "Kitty Alt-Enter follows link without consuming next key");

    char root[] = "/tmp/tinyedit-links-XXXXXX";
    check(mkdtemp(root) != NULL, "local link fixtures");
    char old_path[256], new_path[256];
    snprintf(old_path, sizeof(old_path), "%s/old.md", root);
    snprintf(new_path, sizeof(new_path), "%s/é guide.md", root);
    FILE *file = fopen(old_path, "wb");
    check(file && fputs("[guide](<é guide.md#target>)\n", file) >= 0 && fclose(file) == 0, "write linked source");
    file = fopen(new_path, "wb");
    check(file && fputs("text\n## Target\n", file) >= 0 && fclose(file) == 0, "write linked destination");
    check(editorOpen(old_path), "open linked source");
    runInputBurst(double_click, sizeof(double_click) - 1, 1);
    check(strstr(E.document.file.filename, "é guide.md") && E.document.cursor.cy == 1,
        "double click follows a local link instead of selecting its word");
    check(editorOpen(old_path), "restore linked source");
    editorPushUndo(EDIT_INSERT);
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorInsertChar('X'); E.document.cursor.cx = 2;
    E.document.selection.active = 1; E.document.selection.anchor_x = 1;
    int32_t undo = E.document.history.undo_count;
    runInputBurst("\x1b\rx", 3, 1);
    check(!strcmp(E.document.file.filename, old_path) && E.document.file.dirty &&
        E.document.cursor.cx == 2 && E.document.selection.active && E.document.history.undo_count == undo,
        "cancel link preserves current document and selection/history");
    runInputBurst("\x1b\rn", 3, 1);
    check(strstr(E.document.file.filename, "é guide.md") && E.document.cursor.cy == 1 &&
        !E.document.selection.active && !E.document.history.undo_count,
        "relative Unicode link discard opens destination and heading");
    check(editorOpen(old_path), "reopen link source for save failure");
    free(E.document.file.filename);
    E.document.file.filename = teStrdup("/nonexistent-tinyedit-directory/source.md");
    E.document.file.dirty = 1;
    E.document.cursor.cx = 2;
    /* Use an absolute target so save failure is reached before replacement. */
    int input[2], saved_input = dup(STDIN_FILENO);
    check(saved_input >= 0 && pipe(input) == 0, "link save failure input");
    check(write(input[1], "y", 1) == 1 && dup2(input[0], STDIN_FILENO) >= 0, "link save failure answer");
    close(input[0]);
    editorFollowLink(new_path);
    check(E.document.file.dirty && E.document.cursor.cx == 2 &&
        !strcmp(E.document.file.filename, "/nonexistent-tinyedit-directory/source.md"),
        "failed save protects current document from link replacement");
    check(dup2(saved_input, STDIN_FILENO) >= 0, "restore link input");
    close(saved_input); close(input[1]);

    check(pipe(input) == 0, "prompt Alt-Enter input");
    saved_input = dup(STDIN_FILENO);
    const char prompt_keys[] = "\x1b\r\x1b[13;3uabc\r";
    check(saved_input >= 0 && write(input[1], prompt_keys, sizeof(prompt_keys) - 1) == sizeof(prompt_keys) - 1 &&
        dup2(input[0], STDIN_FILENO) >= 0, "prompt Alt-Enter keys"); close(input[0]);
    char *prompt_value = editorPrompt("Open file: %s");
    check(prompt_value && !strcmp(prompt_value, "abc"), "Alt-Enter inside prompt neither accepts nor consumes next key");
    free(prompt_value);
    check(dup2(saved_input, STDIN_FILENO) >= 0, "restore prompt link input"); close(saved_input); close(input[1]);

    char opener[256], log[256];
#ifdef __APPLE__
    snprintf(opener, sizeof(opener), "%s/open", root);
#else
    snprintf(opener, sizeof(opener), "%s/xdg-open", root);
#endif
    snprintf(log, sizeof(log), "%s/url.txt", root);
    file = fopen(opener, "wb");
    check(file && fputs("#!/bin/sh\nprintf '%s' \"$1\" > \"$TINYEDIT_LINK_LOG\"\n", file) >= 0 &&
        fclose(file) == 0 && chmod(opener, 0700) == 0, "fake browser launcher");
    const char *path_env = getenv("PATH");
    char *old_env = path_env ? teStrdup(path_env) : NULL;
    check(setenv("PATH", root, 1) == 0 && setenv("TINYEDIT_LINK_LOG", log, 1) == 0, "isolated browser environment");
    const char *url = "https://example.org/$(echo-surprise);x?y=1&z=2";
    check(!linksIsWeb("https:///missing-host") &&
        !linksIsWeb("https://trusted.example@evil.example/") &&
        !linksIsWeb("https://example.org\\@evil.example/"),
        "web links reject empty and ambiguous authorities");
    check(linksOpenWeb(url), "launch web link as one argument");
    struct timespec pause = {0, 10000000};
    for (int32_t attempt = 0; attempt < 100; attempt++) {
        struct stat log_stat;
        if (stat(log, &log_stat) == 0 && log_stat.st_size == (off_t)strlen(url)) break;
        nanosleep(&pause, NULL);
    }
    file = fopen(log, "rb");
    char logged[256] = {0};
    check(file && fread(logged, 1, sizeof(logged) - 1, file) == strlen(url) &&
        !strcmp(logged, url), "browser gets literal URL without shell expansion");
    fclose(file);
#ifndef __APPLE__
    char mailer[256];
    snprintf(mailer, sizeof(mailer), "%s/xdg-email", root);
    check(symlink("xdg-open", mailer) == 0, "isolated mail launcher");
#endif
    const char *mail = "mailto:roberto@example.org?subject=Hello%20world&body=Line1%0ALine2";
    check(linksIsMailto(mail) && linksIsMailto("MAILTO:roberto@example.org") &&
        !linksIsMailto("mailto:") && !linksIsMailto("mailto:a b@example.org") &&
        !linksIsMailto("mailto:a@example.org\n") && !linksIsMailto("javascript:alert(1)"),
        "mailto transport checks preserve URI parameters and reject unsafe raw bytes");
    check(linksIsImagePath("/a/b.PNG") && linksIsImagePath("x.jpeg") && !linksIsImagePath("/a/run.command") &&
        !linksIsImagePath("/a.png/tool") && !linksIsImagePath("/a/noext") &&
        !linksIsImagePath("/a/active.svg") && !linksOpenFile("/tmp/x.desktop"),
        "image opener accepts only image extensions");
    beginExternalAnswer('y');
    editorFollowLink(mail);
    endExternalAnswer();
    check(!E.ui.statusmsg_error && strstr(E.ui.statusmsg, "mail application"), "mailto uses desktop mail dispatch");
    for (int32_t attempt = 0; attempt < 100; attempt++) {
        struct stat log_stat;
        if (stat(log, &log_stat) == 0 && log_stat.st_size == (off_t)strlen(mail)) break;
        nanosleep(&pause, NULL);
    }
    file = fopen(log, "rb");
    memset(logged, 0, sizeof(logged));
    check(file && fread(logged, 1, sizeof(logged) - 1, file) == strlen(mail) &&
        !strcmp(logged, mail), "mail launcher receives the literal URI without sending mail");
    fclose(file);
    unlink(log);
    beginExternalAnswer('n');
    editorFollowLink(url);
    endExternalAnswer();
    check(strstr(E.ui.statusmsg, "not opened") && access(log, F_OK) != 0,
        "external web link requires consent");
    beginExternalAnswer('y');
    editorFollowLink(url);
    endExternalAnswer();
    check(!E.ui.statusmsg_error && strstr(E.ui.statusmsg, "browser"),
        "confirmed external web link opens");
    for (int32_t attempt = 0; attempt < 100; attempt++) {
        struct stat log_stat;
        if (stat(log, &log_stat) == 0 && log_stat.st_size == (off_t)strlen(url)) break;
        nanosleep(&pause, NULL);
    }
    unlink(log);
    char image_path[256];
    snprintf(image_path, sizeof(image_path), "%s/-é image $(echo-surprise).png", root);
    file = fopen(image_path, "wb");
    static const unsigned char png_header[] = "\x89PNG\r\n\x1a\n";
    check(file && fwrite(png_header, 1, sizeof(png_header) - 1, file) == sizeof(png_header) - 1 &&
        fclose(file) == 0, "image fixture with literal shell punctuation");
    char masquerade[256];
    snprintf(masquerade, sizeof(masquerade), "%s/masquerade.png", root);
    file = fopen(masquerade, "wb");
    check(file && fputs("#!/bin/sh\nexit 1\n", file) >= 0 && fclose(file) == 0 &&
        !linksVerifyImageFile(masquerade) && errno == EINVAL,
        "executable content with image suffix is refused");
    unlink(masquerade);
    static const struct {
        const char *extension;
        const char *header;
        size_t length;
    } signatures[] = {
        {"jpg", "\xff\xd8\xff", 3}, {"gif", "GIF89a", 6},
        {"webp", "RIFF\0\0\0\0WEBP", 12}, {"bmp", "BM", 2},
        {"tiff", "II*\0", 4}, {"ico", "\0\0\1\0", 4},
        {"avif", "\0\0\0\0ftypavif", 12},
        {"heic", "\0\0\0\0ftypheic", 12}
    };
    for (size_t i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++) {
        char checked_path[256];
        snprintf(checked_path, sizeof(checked_path), "%s/signature.%s", root, signatures[i].extension);
        file = fopen(checked_path, "wb");
        check(file && fwrite(signatures[i].header, 1, signatures[i].length, file) == signatures[i].length &&
            fclose(file) == 0 && linksVerifyImageFile(checked_path),
            "supported image signatures match their extensions");
        unlink(checked_path);
    }
    check(chmod(image_path, 0700) == 0 && !linksVerifyImageFile(image_path) &&
        chmod(image_path, 0600) == 0, "executable image file is refused");
    char *image_absolute = realpath(image_path, NULL);
    check(image_absolute != NULL, "canonical image path");
    editorResetDocument();
    settingsDefaults(&S);
    S.show_menu = S.show_top_bar = S.show_line_numbers = 0;
    S.mouse_enabled = S.markdown_live_preview = 1;
    M.open = 0; T.visible = T.focused = 0;
    E.view.screenrows = 20; E.view.screencols = 100;
    E.document.file.filename = teStrdup(old_path);
    const char *image_source = "![Diagramma](<-é image $(echo-surprise).png>){#fig:architettura .diagramma}";
    editorInsertRow(0, image_source, strlen(image_source));
    editorInsertRow(1, "cursor", 6);
    E.document.cursor.cx = 20;
    E.document.selection.active = 1; E.document.selection.anchor_x = 0;
    E.document.file.dirty = 1;
    beginExternalAnswer('n');
    editorOpenCursorLink();
    endExternalAnswer();
    check(strstr(E.ui.statusmsg, "not opened") && access(log, F_OK) != 0,
        "image confirmation declines without launching");
    beginExternalAnswer('y');
    editorOpenCursorLink();
    endExternalAnswer();
    check(!E.ui.statusmsg_error && E.document.file.dirty && E.document.selection.active &&
        E.document.cursor.cx == 20 && !strcmp(E.document.file.filename, old_path),
        "confirmed image dispatch preserves document, dirty state, cursor and selection");
    for (int32_t attempt = 0; attempt < 100; attempt++) {
        struct stat log_stat;
        if (stat(log, &log_stat) == 0 && log_stat.st_size == (off_t)strlen(image_absolute)) break;
        nanosleep(&pause, NULL);
    }
    file = fopen(log, "rb"); memset(logged, 0, sizeof(logged));
    check(file && fread(logged, 1, sizeof(logged) - 1, file) == strlen(image_absolute) &&
        !strcmp(logged, image_absolute), "image launcher receives one absolute Unicode path without shell evaluation");
    fclose(file);
    E.document.selection.active = 0;
    E.document.cursor.cy = 1; E.document.cursor.cx = 0;
    editorSyncMarkdownPreview();
    struct textLink image_link;
    check(linksFind(image_source, (int32_t)strlen(image_source), 20, &image_link) &&
        markdownLinkIsImage(&E.document.buffer.rows[0], &image_link), "recognize Markdown image destination");
    mouseEventRow = 1; mouseEventCol = image_link.target_start + 1;
    mouseEventPress = 1; mouseEventButton = 35;
    editorHandleMouseEvent(); editorUpdateLinkHover();
    check(hovered_link_row == 0 && E.document.cursor.cy == 1, "hover over visible image path without moving cursor");
    mouseEventButton = 0;
    beginExternalAnswer('y');
    editorHandleMouseEvent();
    endExternalAnswer();
    check(!E.ui.statusmsg_error && E.document.cursor.cy == 1 && E.document.file.dirty &&
        !strcmp(E.document.file.filename, old_path), "live-preview single click opens image without replacing document");
    editorCancelMouseDrag();
    const char *encoded_image = "![Encoded](-%C3%A9%20image%20%24%28echo-surprise%29.png)";
    editorResetDocument();
    E.document.file.filename = teStrdup(old_path);
    editorInsertRow(0, encoded_image, strlen(encoded_image));
    E.document.cursor.cx = 15;
    unlink(log);
    beginExternalAnswer('y');
    editorOpenCursorLink();
    endExternalAnswer();
    check(!E.ui.statusmsg_error, "percent-encoded image path resolves before desktop dispatch");
    for (int32_t attempt = 0; attempt < 100; attempt++) {
        struct stat log_stat;
        if (stat(log, &log_stat) == 0 && log_stat.st_size == (off_t)strlen(image_absolute)) break;
        nanosleep(&pause, NULL);
    }
    file = fopen(log, "rb"); memset(logged, 0, sizeof(logged));
    check(file && fread(logged, 1, sizeof(logged) - 1, file) == strlen(image_absolute) &&
        !strcmp(logged, image_absolute), "encoded image reaches the same desktop target");
    fclose(file); free(image_absolute);
    const char *bad_images[] = {"![missing](missing.png)", "![directory](.)",
        "![scheme](mailto:x@example.org)", "![control](bad%00.png)"};
    for (size_t i = 0; i < sizeof(bad_images) / sizeof(*bad_images); i++) {
        editorResetDocument();
        E.document.file.filename = teStrdup(old_path);
        editorInsertRow(0, bad_images[i], strlen(bad_images[i]));
        E.document.cursor.cx = 3;
        editorOpenCursorLink();
        check(E.ui.statusmsg_error && !strcmp(E.document.file.filename, old_path),
            "invalid image target reports red error without replacing document");
    }
    check(!linksOpenFile("-relative.png") && errno == EINVAL &&
        !linksOpenFile("/tmp/bad\n.png") && errno == EINVAL, "desktop file dispatch rejects options and controls");
#ifndef __APPLE__
    unlink(mailer);
#endif
    unlink(opener);
    check(!linksOpenFile(image_path) && errno == ENOENT, "missing image launcher reported");
    unlink(image_path);
    check(!linksOpenMailto(mail) && errno == ENOENT, "missing mail launcher reported");
    check(!linksOpenMailto("mailto:") && errno == EINVAL, "invalid mail URI refused before launch");
    check(!linksOpenWeb(url) && errno == ENOENT, "missing browser launcher reported");
    if (old_env) { setenv("PATH", old_env, 1); free(old_env); } else unsetenv("PATH");
    unsetenv("TINYEDIT_LINK_LOG"); unlink(log);
    const char *home_env = getenv("HOME");
    char *old_home = home_env ? teStrdup(home_env) : NULL;
    char docs_root[256], docs_folder[256], docs_index[256];
    snprintf(docs_root, sizeof(docs_root), "%s/.tinyedit", root);
    snprintf(docs_folder, sizeof(docs_folder), "%s/.tinyedit/docs", root);
    snprintf(docs_index, sizeof(docs_index), "%s/.tinyedit/docs/README.md", root);
    check(mkdir(docs_root, 0700) == 0 && mkdir(docs_folder, 0700) == 0, "documentation fixture folders");
    file = fopen(docs_index, "wb");
    check(file && fputs("# Installed documentation\n", file) >= 0 && fclose(file) == 0, "documentation index fixture");
    check(setenv("HOME", root, 1) == 0, "documentation isolated home");
    E.document.file.dirty = 0;
    editorDocumentation();
    check(E.document.file.filename && !strcmp(E.document.file.filename, docs_index) &&
        !strcmp(E.document.buffer.rows[0].chars, "# Installed documentation"), "Documentation opens installed index");
    check(E.document.file.read_only, "installed index is read-only");
    const int32_t blocked[] = {'X', '\r', '\t', SHIFT_TAB, BACKSPACE, DEL_KEY,
        CTRL_KEY('x'), CTRL_KEY('v'), CTRL_KEY('z'), CTRL_KEY('y'), CTRL_KEY('s'), F4_KEY};
    for (size_t i = 0; i < sizeof(blocked) / sizeof(*blocked); i++) {
        editorDispatchKey(blocked[i]);
        check(E.ui.statusmsg_error && !E.document.file.dirty &&
            !strcmp(E.document.buffer.rows[0].chars, "# Installed documentation"),
            "read-only commands preserve guide and report red error");
    }
    editorFindAndReplace("Installed");
    check(E.ui.statusmsg_error && !E.document.file.dirty, "read-only search cannot replace");
    editorDispatchKey(CTRL_KEY('a'));
    check(E.document.selection.active, "read-only guide allows selection");
    check(editorSaveToPath(new_path) == FILE_SAVE_FAILED, "read-only direct save rejected");
    check(editorOpen(docs_index) && E.document.file.read_only, "direct Open also protects installed guide");
    check(editorOpen(old_path) && !E.document.file.read_only, "ordinary file remains editable");
    check(editorSaveToPath(docs_index) == FILE_SAVE_FAILED && E.ui.statusmsg_error,
        "Save as cannot overwrite an installed guide from another document");
    editorResetDocument();
    check(!E.document.file.read_only, "new document clears read-only state");
    if (old_home) { setenv("HOME", old_home, 1); free(old_home); } else unsetenv("HOME");
    unlink(docs_index); rmdir(docs_folder); rmdir(docs_root);
    check(editorMenuCommandKey(CMD_DOCUMENTATION) == DOCUMENTATION_KEY, "Documentation menu dispatch");
    editorResetDocument(); unlink(old_path); unlink(new_path); rmdir(root);
    check(dup2(saved_output, STDOUT_FILENO) >= 0, "restore link output"); close(saved_output);
}

static void testMouseScrollView(void) {
    for (uint8_t mode = 0; mode < 4; mode++) {
        settingsDefaults(&S);
        S.markdown_live_preview = mode & 1;
        S.soft_wrap = mode > 1 ? 40 : 0;
        S.mouse_enabled = 1;
        S.show_menu = S.show_top_bar = S.show_line_numbers = 0;
        T.visible = T.focused = 0;
        editorResetDocument();
        E.view.screenrows = 20; E.view.screencols = 80;
        E.document.file.filename = teStrdup("scroll.md");
        for (int32_t y = 0; y < 160; y++) editorInsertRow(y, "riga", 4);
        E.document.cursor.cy = 137;
        E.view.rowoff = 20;
        editorSyncMarkdownPreview();
        mouseEventButton = 64; mouseEventPress = 1;
        mouseEventRow = mouseEventCol = 1;
        editorHandleMouseEvent();
        editorScroll();
        check(E.view.rowoff == 17 && E.document.cursor.cy == 137,
            "wheel detaches viewport without moving cursor");
        for (int32_t redraw = 0; redraw < 3; redraw++) {
            mouseEventButton = 35; mouseEventCol++;
            editorHandleMouseEvent();
            editorUpdateLinkHover();
            editorScroll();
            check(E.view.rowoff == 17 && E.document.cursor.cy == 137,
                "passive pointer motion and redraw preserve wheel position");
        }
        mouseEventButton = 0; mouseEventCol = 2;
        editorHandleMouseEvent();
        editorScroll();
        check(!E.view.free_scroll && E.document.cursor.cy == 17,
            "text click restores following at clicked source position");
        editorCancelMouseDrag();
        E.document.cursor.cy = 137; E.view.rowoff = 17; E.view.free_scroll = 1;
        editorDispatchKey(ARROW_UP);
        editorScroll();
        check(!E.view.free_scroll && E.document.cursor.cy == 136 && E.view.rowoff > 17,
            "keyboard navigation restores cursor following");
    }
    editorResetDocument();
    settingsDefaults(&S);
    editorSyncMarkdownPreview();
}

static void testMouseDispatch(void) {
    settingsDefaults(&S);
    S.show_menu = S.show_top_bar = S.show_line_numbers = 0;
    S.syntax_highlight = S.auto_close_pairs = S.mouse_enabled = 0;
    E.view.screenrows = 20;
    E.view.screencols = 80;
    editorResetDocument();
    editorInsertRow(0, "abcdef", 6);
    E.document.file.final_newline = 0;
    const char click_and_text[] = "\x1b[<0;2;1M\x1b[<0;2;1mXY";
    runInputBurst(click_and_text, sizeof(click_and_text) - 1, 2);
    check(E.document.buffer.rows[0].size == 8 &&
        memcmp(E.document.buffer.rows[0].chars, "aXYbcdef", 8) == 0,
        "text following mouse reports is neither lost nor duplicated");
    S.auto_close_pairs = 1;
    const char wheel_and_unicode[] = "\x1b[<65;1;1M\x1b[<65;1;1Mé";
    runInputBurst(wheel_and_unicode, sizeof(wheel_and_unicode) - 1, 1);
    check(E.document.buffer.rows[0].size == 10 &&
        memcmp(E.document.buffer.rows[0].chars, "aXYébcdef", 10) == 0 && !E.view.free_scroll,
        "Unicode after a wheel burst is decoded once and restores cursor-following");
    S.auto_close_pairs = 0;
    const char wheel_and_selection[] = "\x1b[<64;1;1M\x1b[1;2C";
    runInputBurst(wheel_and_selection, sizeof(wheel_and_selection) - 1, 1);
    check(E.document.selection.active && E.document.selection.anchor_x == 5 &&
        E.document.cursor.cx == 6, "navigation with selection following mouse");
    const char wheel_and_paste[] = "\x1b[<65;1;1M\x1b[200~界\x1b[201~";
    runInputBurst(wheel_and_paste, sizeof(wheel_and_paste) - 1, 1);
    check(E.document.buffer.rows[0].size == 12 &&
        memcmp(E.document.buffer.rows[0].chars, "aXYé界cdef", 12) == 0,
        "bracketed paste following mouse uses its own payload reader");
    const char drag[] = "\x1b[<0;2;1M\x1b[<32;4;1M";
    runInputBurst(drag, sizeof(drag) - 1, 1);
    check(E.document.mouse.dragging && E.document.selection.active &&
        E.document.mouse.press_anchor_x == 1 && E.document.cursor.cx == 3,
        "coalesced drag selects from the click point");
    const char release[] = "\x1b[<0;80;22m";
    runInputBurst(release, sizeof(release) - 1, 1);
    check(!E.document.mouse.dragging && E.document.selection.active,
        "release outside text ends drag without discarding selection");
    const char press[] = "\x1b[<0;4;1M";
    runInputBurst(press, sizeof(press) - 1, 1);
    check(E.document.mouse.dragging && E.document.mouse.press_anchor_x == 3,
        "new drag starts at its own anchor");
    editorResetDocument();
    check(!E.document.mouse.dragging && !E.document.mouse.press_anchor_x &&
        !E.document.mouse.press_anchor_y, "document reset releases gesture and anchors");
    editorInsertRow(0, "new", 3);
    const char stale_motion[] = "\x1b[<32;3;1M";
    runInputBurst(stale_motion, sizeof(stale_motion) - 1, 1);
    check(!E.document.selection.active && E.document.cursor.cx == 0,
        "old drag motion cannot select in a replacement document");
    runInputBurst(press, sizeof(press) - 1, 1);
    editorLoadLines("restored", 8);
    check(!E.document.mouse.dragging, "recovery replacement cancels old gesture");
    runInputBurst(press, sizeof(press) - 1, 1);
    const char key_and_motion[] = "K\x1b[<32;7;1M";
    runInputBurst(key_and_motion, sizeof(key_and_motion) - 1, 2);
    check(!E.document.selection.active && !E.document.mouse.dragging &&
        E.document.cursor.cx == 4, "keyboard input ends an in-progress drag");
    editorResetDocument();
    for (int32_t i = 0; i < 300; i++) editorInsertRow(i, "row", 3);
    char burst[2048];
    size_t used = 0;
    const char wheel[] = "\x1b[<65;1;1M";
    for (int32_t i = 0; i <= MOUSE_BURST_LIMIT; i++) {
        memcpy(burst + used, wheel, sizeof(wheel) - 1);
        used += sizeof(wheel) - 1;
    }
    memcpy(burst + used, "X", 1); used++;
    runInputBurst(burst, used, 2);
    check(E.document.buffer.rows[0].size == 4 && E.document.buffer.rows[0].chars[0] == 'X',
        "burst limit leaves subsequent reports and text queued in order");
    editorResetDocument();
    editorInsertRow(0, "menu", 4);
    S.show_menu = 1;
    menuInit(&M);
    const char wheel_and_menu[] = "\x1b[<65;1;2M\x1b[<0;2;1M\x1b[<0;2;1m";
    runInputBurst(wheel_and_menu, sizeof(wheel_and_menu) - 1, 1);
    check(M.open && !E.document.mouse.dragging,
        "later mouse reports in a burst still route through menu handling");
    /* A literal escape is queued as a decoded key, avoiding a pipe EOF while
     * the escape decoder probes for an optional continuation. */
    pending_key = '\x1b';
    editorProcessKeypress();
    check(!M.open, "keyboard following a mouse-opened menu reaches menu routing");
    S.show_menu = 0;
    editorResetDocument();
}

static void testSharedAutoClose(void) {
    settingsDefaults(&S);
    S.show_menu = S.show_top_bar = S.show_line_numbers = S.syntax_highlight = 0;
    S.auto_close_pairs = S.auto_close_single_quote = 1;
    const char openers[] = "({[\"'$`";
    const char closers[] = ")}]\"'$`";
    for (int32_t i = 0; i < (int32_t)sizeof(openers) - 1; i++) {
        editorResetDocument();
        pending_key = openers[i];
        editorProcessKeypress();
        erow *row = &E.document.buffer.rows[0];
        check(row->size == 2 && row->chars[0] == openers[i] &&
            row->chars[1] == closers[i] && E.document.cursor.cx == 1,
            "typing uses shared opener policy for every ASCII pair");
        int32_t undo_count = E.document.history.undo_count;
        pending_key = closers[i];
        editorProcessKeypress();
        row = &E.document.buffer.rows[0];
        if (openers[i] == '`') {
            check(row->size == 4 && E.document.cursor.cx == 2,
                "backtick keeps its deliberate fresh-pair behavior");
        } else {
            check(row->size == 2 && E.document.cursor.cx == 2 &&
                E.document.history.undo_count == undo_count,
                "typing a matching closer skips without editing or adding undo");
        }
        editorResetDocument();
        editorInsertRow(0, "abc", 3);
        E.document.selection.active = 1;
        E.document.selection.anchor_x = 0;
        E.document.cursor.cx = 3;
        editorDispatchKey(openers[i]);
        row = &E.document.buffer.rows[0];
        check(row->size == 5 && row->chars[0] == openers[i] &&
            memcmp(row->chars + 1, "abc", 3) == 0 && row->chars[4] == closers[i] &&
            !E.document.selection.active, "shared opener policy wraps selected text");
    }
    editorResetDocument();
    editorInsertRow(0, "ab", 2);
    editorInsertRow(1, "cd", 2);
    E.document.selection.active = 1;
    E.document.selection.anchor_x = 1;
    E.document.cursor.cy = 1;
    E.document.cursor.cx = 1;
    editorDispatchKey('(');
    check(strcmp(E.document.buffer.rows[0].chars, "a(b") == 0 &&
        strcmp(E.document.buffer.rows[1].chars, "c)d") == 0,
        "shared pair wraps a multiline selection without shifting endpoints");
    for (int32_t enabled = 0; enabled <= 1; enabled++) {
        for (int32_t apostrophe = 0; apostrophe <= 1; apostrophe++) {
            S.auto_close_pairs = enabled;
            S.auto_close_single_quote = apostrophe;
            for (int32_t i = 0; i < (int32_t)sizeof(openers) - 1; i++) {
                uint8_t allowed = enabled && (openers[i] != '\'' || apostrophe);
                editorResetDocument();
                editorDispatchKey(openers[i]);
                erow *row = &E.document.buffer.rows[0];
                check(row->size == (allowed ? 2 : 1), "typing honors both auto-close settings");
                editorResetDocument();
                editorInsertRow(0, "abc", 3);
                E.document.selection.active = 1;
                E.document.selection.anchor_x = 0;
                E.document.cursor.cx = 3;
                editorDispatchKey(openers[i]);
                row = &E.document.buffer.rows[0];
                check(row->size == (allowed ? 5 : 1) && row->chars[0] == openers[i],
                    "disabled pair replaces selection rather than bypassing replacement");
                if (!allowed) check(E.document.history.undo_count == 1,
                    "selection replacement records one undo action");
            }
        }
    }
    S.auto_close_pairs = 1;
    editorResetDocument();
    editorDispatchKey('$'); editorDispatchKey('$'); editorDispatchKey('$');
    check(strcmp(E.document.buffer.rows[0].chars, "$$$$") == 0 && E.document.cursor.cx == 2,
        "display-math policy preserved after sharing the pair table");
    editorResetDocument();
    editorInsertRow(0, ")", 1);
    editorDispatchKey(']');
    check(strcmp(E.document.buffer.rows[0].chars, "])") == 0,
        "asymmetric closer skips only the same byte at the cursor");
    editorResetDocument();
}

static void testEditBatches(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.auto_close_pairs = 1;
    editorDispatchKey('a');
    int32_t before = E.document.history.undo_count;
    syntax_calls = 0;
    editorDispatchKey('(');
    check(syntax_calls == 1, "auto-close rebuilds syntax once");
    check(E.document.history.undo_count == before + 1, "pair owns explicit undo snapshot");
    editorUndo();
    check(strcmp(E.document.buffer.rows[0].chars, "a") == 0, "pair undo preserves preceding typing");
    editorRedo();
    check(strcmp(E.document.buffer.rows[0].chars, "a()") == 0 && E.document.cursor.cx == 2,
        "pair redo restores text and cursor");

    editorResetDocument();
    editorInsertRow(0, "👩🏽‍💻", strlen("👩🏽‍💻"));
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    syntax_calls = 0;
    editorDelChar();
    check(E.document.buffer.rows[0].size == 0 && syntax_calls == 1,
        "grapheme deletion updates once");
    editorUndo();
    check(strcmp(E.document.buffer.rows[0].chars, "👩🏽‍💻") == 0, "grapheme undo retains UTF-8");

    editorResetDocument();
    E.document.file.filename = teStrdup("batch.md");
    editorInsertRow(0, "prose", 5);
    editorInsertRow(1, "x*2", 3);
    editorInsertRow(2, "$$", 2);
    editorInsertRow(3, "after", 5);
    syntax_calls = 0;
    editorBeginEdit();
    bufferRowDeleteRange(&E.document.buffer.rows[0], 0, 5);
    editorRowInsertString(&E.document.buffer.rows[0], 0, "$$", 2);
    editorRowInsertString(&E.document.buffer.rows[1], 0, "y+", 2);
    check(syntax_calls == 0, "batch defers syntax until source mutations finish");
    editorEndEdit();
    check(syntax_calls == 4 && E.document.buffer.rows[1].hl[0] == HL_MATH &&
        E.document.buffer.rows[3].hl[0] == HL_NORMAL, "batch propagates final multiline state once");
    editorResetDocument();
    S.auto_indent = 1;
    editorInsertRow(0, "    abc", 7);
    E.document.cursor.cx = 7;
    syntax_calls = 0;
    editorInsertNewlineAutoIndent();
    check(syntax_calls == 2 && strcmp(E.document.buffer.rows[1].chars, "    ") == 0,
        "newline and indentation rebuild each changed row once");
    editorUndo();
    check(E.document.buffer.row_count == 1 && E.document.cursor.cx == 7,
        "newline and indentation share undo");
    editorResetDocument();
    editorReplaceSelectionWithText(0, 0, 0, 0, 0, "a\nb\nc", 5);
    E.document.selection.active = 1;
    E.document.selection.anchor_y = 0;
    E.document.selection.anchor_x = 0;
    E.document.cursor.cy = 2;
    E.document.cursor.cx = 1;
    S.insert_spaces_for_tab = 1;
    syntax_calls = 0;
    editorIndentSelection(0);
    check(syntax_calls == 3 && E.document.buffer.rows[0].chars[0] == ' ',
        "block indentation highlights each changed row once");
    editorUndo();
    check(strcmp(E.document.buffer.rows[0].chars, "a") == 0 &&
        strcmp(E.document.buffer.rows[2].chars, "c") == 0, "block indentation is one undo action");
    syntax_calls = 0;
    editorUpdateAllRows();
    check(syntax_calls == 3, "global cache refresh tokenizes each row once");
    syntax_calls = 0;
    editorReplaceSelectionWithText(1, 0, 0, 2, 1, "x\ny", 3);
    check(syntax_calls == 2 && E.document.buffer.row_count == 2 &&
        strcmp(E.document.buffer.rows[0].render, "x") == 0 &&
        strcmp(E.document.buffer.rows[1].render, "y") == 0,
        "multiline replacement updates final rows once after splicing");
    editorUndo();
    check(E.document.buffer.row_count == 3 && strcmp(E.document.buffer.rows[1].chars, "b") == 0,
        "multiline replacement undoes as one action");
    check(edit_batch.depth == 0, "all editing scopes close");
    editorResetDocument();
}

static void testSearchSession(void) {
    editorResetDocument();
    settingsDefaults(&S);
    editorInsertRow(0, "111 é 22", strlen("111 é 22"));
    editorInsertRow(1, "333", 3);
    E.search.saved_cy = 0;
    E.search.saved_cx = 0;
    E.search.saved_rowoff = 7;
    E.search.saved_coloff = 3;
    E.search.direction = 1;
    E.search.regex_mode = 1;
    editorClearSearchNavigation();
    char pattern[] = "[0-9]+";
    struct searchMatch result;
    searchQueryPrepare(&E.search.query, pattern, 1);
    E.document.cursor.cy = 1;
    E.document.cursor.cx = 2;
    E.view.rowoff = 7;
    E.view.coloff = 3;
    check(searchFind(&E.search.query, &E.document.buffer, 0, 0, 1, 0, &result) &&
        E.document.cursor.cy == 1 && E.document.cursor.cx == 2 && E.view.rowoff == 7,
        "pure search cannot move active cursor or view");
    editorFindCallback(pattern, 'x');
    check(E.document.cursor.cy == 0 && E.document.cursor.cx == 0 && E.search.search_match_len == 3,
        "session applies complete regex match");
    editorFindCallback(pattern, ARROW_DOWN);
    check(E.document.cursor.cx == 7 && E.search.search_match_len == 2, "forward skips entire regex match");
    editorFindCallback(pattern, ARROW_DOWN);
    check(E.document.cursor.cy == 1 && E.document.cursor.cx == 0, "forward advances to next row");
    editorFindCallback(pattern, ARROW_UP);
    check(E.document.cursor.cy == 0 && E.document.cursor.cx == 7, "reverse excludes column-zero match");
    editorFindCallback(pattern, '\x1b');
    check(E.document.cursor.cy == 0 && E.document.cursor.cx == 0 && E.view.rowoff == 7 &&
        E.view.coloff == 3 && E.search.last_cy == -1 && E.search.search_match_y == -1,
        "cancel restores cursor/view and clears navigation state");

    E.search.regex_mode = 1;
    char multiline[] = "22\\n333";
    editorFindCallback(multiline, 'x');
    check(E.search.search_match_y == 0 && E.search.search_match_x == 7 &&
        E.search.search_match_end_y == 1 && E.search.query.text != NULL, "session applies multiline result");
    editorPushUndo(EDIT_OTHER);
    E.document.cursor.cx = 9;
    editorInsertCharRaw('!');
    check(E.search.query.text == NULL && E.search.query.compiled,
        "core source mutation invalidates text cache without freeing regex");
    check(!editorFindFrom(multiline, 0, 0, 1, 0), "changed document cannot reuse stale match text");
    editorUndo();
    check(editorFindFrom(multiline, 0, 0, 1, 0), "undo invalidates search text and restores matches");
    editorRedo();
    check(!editorFindFrom(multiline, 0, 0, 1, 0), "redo invalidates search text again");
    editorResetDocument();
    check(E.search.query.pattern == NULL && E.search.last_cy == -1,
        "new document releases compiled query and previous navigation");
    char empty[] = "";
    editorFindCallback(empty, ARROW_UP);
    check(E.search.search_match_y == -1, "empty document navigation is safe");
    editorResetDocument();
}

static void drawWrappedReference(struct abuf *ab) {
    int32_t cols = editorSoftWrapCols();
    int32_t total = editorTotalVideoRows(cols);
    for (int32_t y = 0; y < E.view.screenrows; y++) {
        int32_t vy = E.view.rowoff + y;
        int32_t row_index = E.document.buffer.row_count, segment = 0;
        if (vy < total) editorFileRowAtVideoRow(vy, cols, &row_index, &segment);
        editorDrawGutter(ab, editorGutterWidth(), row_index, segment > 0);
        if (vy >= total) abAppend(ab, "~", 1);
        else {
            erow *row = &E.document.buffer.rows[row_index];
            int32_t segments = editorRowSegments(row, cols);
            editorDrawRowSegment(ab, row_index, row->seg_start[segment],
                editorSegVisibleEnd(row, segments, row->seg_start, segment),
                0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
        }
        abAppend(ab, "\x1b[K\r\n", 5);
    }
}

static void testHeadingReverse(void) {
    editorResetDocument();
    settingsDefaults(&S);
    E.document.file.filename = teStrdup("heading.md");
    E.view.screencols = 12;
    E.view.screenrows = 5;
    E.view.rowoff = 0;
    S.show_line_numbers = 1;
    S.soft_wrap = 8;
    S.markdown_heading_reverse = 1;
    editorInsertRow(0, "# Héading long enough to wrap", strlen("# Héading long enough to wrap"));
    editorInsertRow(1, "plain", 5);
    struct abuf ab = ABUF_INIT;
    editorDrawRows(&ab);
    abAppend(&ab, "", 1);
    const char *background = "\x1b[7m";
    check(strstr(ab.b, background) != NULL, "heading reverse video emitted");
    char ending[64];
    snprintf(ending, sizeof(ending), "%s        \x1b[5G", background);
    check(strstr(ab.b, ending) != NULL, "heading paints text columns and preserves gutter");
    check(!drawing_heading, "heading rendering state restored");
    abFree(&ab);
    S.color_mode = COLOR_MODE_RGB;
    S.rgb_markdown_heading_background = 0x112233;
    S.rgb_color_syntax_preprocessor = 0x445566;
    ab = (struct abuf)ABUF_INIT;
    editorDrawRows(&ab);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[48;2;17;34;51m") != NULL, "RGB heading background emitted");
    check(strstr(ab.b, "\x1b[38;2;68;85;102m") != NULL, "RGB heading foreground preserved");
    check(strstr(ab.b, "\x1b[7m") == NULL, "RGB headings ignore legacy inversion");
    abFree(&ab);
    for (int32_t level = 0; level < 6; level++) {
        S.rgb_color_markdown_heading[level] = 0x102030 + level;
        S.color_markdown_heading[level] = COLOR_RED_LIGHT + level;
        S.color_mode = COLOR_MODE_RGB;
        char expected[64];
        snprintf(expected, sizeof(expected), "\x1b[38;2;16;32;%dm", 48 + level);
        check(strcmp(syntaxColorFor((enum syntaxHighlight)(HL_HEADING_1 + level), &S), expected) == 0,
            "each heading level resolves independent RGB color");
        S.color_mode = COLOR_MODE_ANSI;
        check(strcmp(syntaxColorFor((enum syntaxHighlight)(HL_HEADING_1 + level), &S),
            ansiColorCode(S.color_markdown_heading[level])) == 0, "each heading level resolves ANSI color");
    }
    S.color_mode = COLOR_MODE_RGB;
    S.rgb_color_background = 0x202020;
    S.rgb_color_markdown_heading[0] = 0xE02020;
    ab = (struct abuf)ABUF_INIT;
    editorBeginHeadingRow(&ab, 0);
    abAppend(&ab, "", 1);
    check(drawing_heading_background == 0x372020, "heading background uses twelve percent of foreground hue");
    check(strstr(ab.b, "\x1b[48;2;55;32;32m") != NULL, "tinted heading background emitted");
    editorEndHeadingRow(&ab);
    check(!drawing_heading, "tinted heading background does not leak into plain text");
    abFree(&ab);
    S.rgb_color_markdown_heading[0] = 0x2020E0;
    ab = (struct abuf)ABUF_INIT;
    editorBeginHeadingRow(&ab, 0);
    check(drawing_heading_background == 0x202037, "changing foreground updates heading background hue");
    editorEndHeadingRow(&ab);
    abFree(&ab);
    S.rgb_markdown_heading_background = RGB_TERMINAL_DEFAULT;
    ab = (struct abuf)ABUF_INIT;
    editorBeginHeadingRow(&ab, 0);
    check(!drawing_heading, "terminal default disables heading background override");
    editorEndHeadingRow(&ab);
    abFree(&ab);
    S.color_mode = COLOR_MODE_ANSI;
    S.markdown_text_styles = 1;
    for (uint8_t reverse = 0; reverse < 2; reverse++) {
        S.markdown_heading_reverse = reverse;
        ab = (struct abuf)ABUF_INIT;
        editorBeginHeadingRow(&ab, 0);
        editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
            1, 0, 2, 0, (int32_t)strlen("# Héading"), 0, -1, -1, -1, -1);
        editorDrawHighlightedTerminator(&ab);
        editorEndHeadingRow(&ab);
        abAppend(&ab, "", 1);
        char selected[128];
        snprintf(selected, sizeof(selected), "%s\x1b[7mHéading", ansiColorCode(S.color_selection));
        check(strstr(ab.b, selected) != NULL, "selection retains its color across styled UTF-8 heading");
        snprintf(selected, sizeof(selected), "%s\x1b[7m ", ansiColorCode(S.color_selection));
        check(strstr(ab.b, selected) != NULL, "heading end-of-line selection uses selection background");
        check(!drawing_heading && !drawing_heading_bold, "selection restores heading render state");
        abFree(&ab);
    }
    S.markdown_heading_reverse = 0;
    ab = (struct abuf)ABUF_INIT;
    editorDrawRows(&ab);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, background) == NULL, "heading reverse video disabled");
    abFree(&ab);
    editorResetDocument();
    settingsDefaults(&S);
}

static void testBracketColors(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.color_syntax_bracket = COLOR_RED_DARK;
    const char *names[] = {"plain.txt", "code.c", NULL};
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        editorResetDocument();
        if (names[k]) E.document.file.filename = teStrdup(names[k]);
        editorInsertRow(0, "é()[]{}", strlen("é()[]{}"));
        struct abuf ab = ABUF_INIT;
        editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
            0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
        abAppend(&ab, "", 1);
        check(strstr(ab.b, "\x1b[31m(") != NULL && strstr(ab.b, "\x1b[31m}") != NULL,
            "configured brackets in known, unknown and unnamed files");
        abFree(&ab);
    }
    editorResetDocument();
    E.document.file.filename = teStrdup("code.c");
    editorInsertRow(0, "\"()\" /* [] */ {}", strlen("\"()\" /* [] */ {}"));
    struct abuf ab = ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[31m(") == NULL && strstr(ab.b, "\x1b[31m[") == NULL &&
        strstr(ab.b, "\x1b[31m{") != NULL, "strings and comments preserve their colors");
    abFree(&ab);
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        1, 0, 14, 0, 16, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[31m{") == NULL, "selection overrides bracket color");
    abFree(&ab);
    S.syntax_highlight = 0;
    editorRehighlightFrom(0, 1);
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[31m(") != NULL, "bracket color independent of syntax toggle");
    abFree(&ab);
    editorResetDocument();
    S.syntax_highlight = 1;
    E.document.file.filename = teStrdup("brackets.md");
    S.markdown_text_styles = 1;
    S.color_syntax_json_key = COLOR_BLUE_DARK;
    S.color_syntax_preprocessor = COLOR_GREEN_DARK;
    const char *markdown = "[anchor](#id) ![image](img.png){#fig:id} [span]{#id} () [] {}";
    editorInsertRow(0, markdown, strlen(markdown));
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    int32_t plain = (int32_t)(strstr(markdown, " () [] {}") - markdown) + 1;
    erow *row = &E.document.buffer.rows[0];
    for (int32_t at = 0; at < row->size; at += (int32_t)utf8NextCharLen(row->chars, (size_t)at, (size_t)row->size)) {
        if (strchr("()[]{}", row->chars[at]))
            check(row->hl[at] == (at < plain ? HL_LINK_MARKER : HL_NORMAL),
                "link/image/anchor delimiters are distinct from ordinary brackets");
    }
    check(strstr(ab.b, "\x1b[34m[") && strstr(ab.b, "\x1b[34m{") && strstr(ab.b, "\x1b[31m("),
        "ANSI link and Pandoc markers differ from ordinary bracket color");
    check(strstr(ab.b, "\x1b[4ma") != NULL, "source link label is underlined");
    abFree(&ab);
    S.color_mode = COLOR_MODE_RGB;
    S.rgb_color_syntax_bracket = 0x123456;
    S.rgb_color_syntax_json_key = 0x654321;
    S.rgb_color_syntax_preprocessor = 0xABCDEF;
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[38;2;101;67;33m[") && strstr(ab.b, "\x1b[38;2;18;52;86m("),
        "RGB link/anchor markers and ordinary brackets use distinct roles");
    abFree(&ab);
    editorResetDocument();
    E.document.file.filename = teStrdup("literal.md");
    const char *literal = "`[literal]{#fake}` \\[escaped]{#escaped} [plain] {ordinary} ![missing]{#bad}";
    editorInsertRow(0, literal, strlen(literal));
    row = &E.document.buffer.rows[0];
    for (int32_t at = 0; at < row->rsize; at++)
        check(row->hl[at] != HL_LINK_MARKER, "literal, escaped and incomplete anchors do not receive link markers");
    editorResetDocument();
    settingsDefaults(&S);
}

static void testMarkdownStyles(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.markdown_text_styles = 1;
    E.document.file.filename = teStrdup("styles.MD");
    editorInsertRow(0, "**bold** and *italic* é", strlen("**bold** and *italic* é"));
    editorInsertRow(1, "# Heading", 9);
    struct abuf ab = ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[1m") != NULL, "Markdown strong uses bold");
    check(strstr(ab.b, "\x1b[3m") != NULL, "Markdown emphasis uses italic");
    abFree(&ab);
    const char *selected_text = "**TODO.md si trova in `local/TODO.md`**, relativo é";
    editorInsertRow(2, selected_text, strlen(selected_text));
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 2, 0, E.document.buffer.rows[2].rsize,
        1, 2, 0, 2, E.document.buffer.rows[2].size, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    char selected[256];
    snprintf(selected, sizeof(selected), "%s\x1b[7m%s", ansiColorCode(S.color_selection), selected_text);
    check(strstr(ab.b, selected) != NULL,
        "selection remains continuous across Markdown bold, inline code and UTF-8");
    abFree(&ab);
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 1, 0, E.document.buffer.rows[1].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[1m") != NULL, "Markdown heading uses bold without reverse");
    abFree(&ab);
    editorInsertRow(2, "[label](url) <tag>", strlen("[label](url) <tag>"));
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 2, 0, E.document.buffer.rows[2].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[3m") == NULL, "Markdown links and tags are not italic");
    abFree(&ab);
    S.markdown_text_styles = 0;
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, E.document.buffer.rows[0].rsize,
        0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[1m") == NULL && strstr(ab.b, "\x1b[3m") == NULL,
        "Markdown styles opt-in");
    abFree(&ab);
    editorResetDocument();
    settingsDefaults(&S);
}

static void testMarkdownPreview(void) {
    editorResetDocument();
    settingsDefaults(&S);
    check(!S.markdown_live_preview, "live preview defaults off");
    check(commandToggleSetting(CMD_TOGGLE_MARKDOWN_PREVIEW, &S) &&
        commandIsChecked(CMD_TOGGLE_MARKDOWN_PREVIEW, &S), "View toggles live preview");
    E.search.search_match_y = E.search.search_match_end_y = -1;
    E.document.file.filename = teStrdup("preview.MD");
    const char *source = "**caffè界** e *é* [sito](https://example.org/a(b))";
    editorInsertRow(0, source, strlen(source));
    editorInsertRow(1, "plain", 5);
    E.document.cursor.cy = 1;
    E.document.cursor.cx = 0;
    erow *row = &E.document.buffer.rows[0];
    editorSyncMarkdownPreview();
    check(row->markdown_count == 3, "paired emphasis and link presentation spans");
    check(editorRowCxToRx(row, row->size) == 16,
        "preview measures Unicode columns without markup or URL");
    check(editorRowRxToCx(row, 1) == 3 && editorRowRxToCx(row, 6) == 8,
        "preview maps columns back to source bytes including wide glyph");
    struct abuf ab = ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, row->rsize, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(strchr(ab.b, '*') == NULL && strstr(ab.b, "https") == NULL && strstr(ab.b, "\x1b[1m") &&
        strstr(ab.b, "\x1b[3m") && strstr(ab.b, "\x1b[4m"),
        "preview hides markup and emits bold italic underline independently of styles setting");
    abFree(&ab);
    for (int32_t i = 0; i < row->markdown_count; i++) {
        struct markdownSpan span = row->markdown_spans[i];
        E.document.cursor.cy = 0;
        for (int32_t x = span.start; x <= span.end; ) {
            E.document.cursor.cx = x;
            editorSyncMarkdownPreview();
            check(!renderByteHidden(row, span.start) && !renderByteHidden(row, span.end - 1),
                "cursor on either boundary or inside expands complete construct");
            if (x == span.end) break;
            x += (int32_t)utf8NextCharLen(row->chars, (size_t)x, (size_t)row->size);
        }
    }
    E.document.cursor.cy = 1;
    E.document.cursor.cx = 0;
    editorSyncMarkdownPreview();
    check(renderByteHidden(row, 0), "leaving construct collapses again");
    S.show_line_numbers = S.show_top_bar = S.show_menu = 0;
    E.view.screencols = 80; E.view.screenrows = 10; E.view.rowoff = E.view.coloff = 0;
    int32_t y, x;
    editorMouseToCursor(2, 1, &y, &x);
    check(y == 0 && x == 3, "mouse click maps compact Unicode text to source");
    E.document.selection.active = 1;
    check(editorRowCxToRx(row, row->size) == bufferRowCxToRx(row, row->size, S.tab_stop),
        "selection reveals original text");
    E.document.selection.active = 0;
    E.search.search_match_y = 0;
    editorSyncMarkdownPreview();
    check(!renderByteHidden(row, 0), "search reveals delimiters and destination");
    E.search.search_match_y = -1;
    editorInsertRow(2, "**ab**", 6);
    row = &E.document.buffer.rows[2];
    check(editorRowSegments(row, 2) == 1, "hidden closer at wrap boundary adds no phantom row");
    E.document.cursor.cy = 2; E.document.cursor.cx = 2;
    check(editorRowSegments(row, 2) == 3, "expansion invalidates wrap cache");
    E.document.cursor.cy = 1; E.document.cursor.cx = 0;
    check(editorRowSegments(row, 2) == 1, "collapse invalidates wrap cache");
    editorInsertRow(3, "**界**", strlen("**界**"));
    row = &E.document.buffer.rows[3];
    check(editorRowSegments(row, 1) == 1, "hidden opener before wide glyph adds no phantom segment");
    editorInsertRow(4, "`**code**` \\*escaped*", strlen("`**code**` \\*escaped*"));
    row = &E.document.buffer.rows[4];
    check(row->markdown_count == 0, "code and escaped emphasis stay literal");
    check(!strcmp(E.document.buffer.rows[0].chars, source), "presentation preserves source bytes");
    editorInsertRow(5, "**a**\t界", strlen("**a**\t界"));
    row = &E.document.buffer.rows[5];
    check(editorRowCxToRx(row, row->size) == 6 && editorRowRxToCx(row, 4) == 6,
        "tab expansion retains original tab stops and source-byte mapping");
    editorInsertRow(6, "**ab** **cd** **ef** tail", 25);
    E.document.cursor.cy = 6; E.document.cursor.cx = 25;
    S.soft_wrap = 8; S.home_end_visual_line = 1;
    E.document.cursor.rx = editorRowCxToRx(&E.document.buffer.rows[6], 25);
    editorHandleNavigationKey(SHIFT_HOME);
    check(E.document.cursor.cx == 21 && E.document.selection.active,
        "Shift-Home uses expanded coordinates when selection reveals markup");
    E.document.selection.active = 0;
    E.document.cursor.cy = 0; E.document.cursor.cx = 3;
    editorInsertChar('z');
    editorUndo();
    check(!strcmp(E.document.buffer.rows[0].chars, source) &&
        E.document.buffer.rows[0].markdown_count == 3, "undo rebuilds presentation from original source");
    editorRedo();
    check(E.document.buffer.rows[0].markdown_count == 3 &&
        E.document.buffer.rows[0].chars[3] == 'z', "redo rebuilds presentation spans");
    S.syntax_highlight = 0;
    editorRehighlightFrom(0, 1);
    check(E.document.buffer.rows[0].markdown_count == 3, "syntax toggle preserves live preview spans");
    E.document.cursor.cy = 1; E.document.cursor.cx = 0;
    E.document.cursor.rx = 0;
    row = &E.document.buffer.rows[0];
    check(editorRowCxToRx(row, row->size) < bufferRowCxToRx(row, row->size, S.tab_stop),
        "preview still compacts text without syntax colors");
    S.color_syntax_keyword = S.color_syntax_json_key = COLOR_MAGENTA_LIGHT;
    S.color_syntax_emphasis_strong = COLOR_RED_LIGHT;
    ab = (struct abuf)ABUF_INIT;
    editorDrawRowSegment(&ab, 0, 0, row->rsize, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1);
    abAppend(&ab, "", 1);
    check(!strchr(ab.b, '*') && !strstr(ab.b, "https") && strstr(ab.b, "\x1b[1m") &&
        strstr(ab.b, "\x1b[3m") && strstr(ab.b, "\x1b[4m"), "preview styles survive disabled highlighting");
    check(!strstr(ab.b, "\x1b[95m") && !strstr(ab.b, "\x1b[91m"), "preview parsing does not enable syntax colors");
    abFree(&ab);
    E.document.cursor.cy = 0; E.document.cursor.cx = 0;
    editorSyncMarkdownPreview();
    check(!renderByteHidden(row, 0), "cursor still expands preview without syntax colors");
    editorInsertChar('x');
    editorUndo();
    row = &E.document.buffer.rows[0];
    check(row->markdown_count == 3, "editing and undo retain uncolored preview metadata");
    S.markdown_live_preview = 0;
    editorRehighlightFrom(0, 1);
    check(!row->markdown_count && !row->hl, "both features off release parsing metadata");
    editorResetDocument();
    settingsDefaults(&S);
    editorSyncMarkdownPreview();
}

static void testTypingMarkdownLink(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.markdown_live_preview = 1;
    T.visible = T.focused = 0;
    E.document.file.filename = teStrdup("typing.md");
    const char *label = "[Roberto](";
    for (size_t i = 0; label[i]; i++) {
        editorDispatchKey((uint8_t)label[i]);
        editorSyncMarkdownPreview();
    }
    erow *row = &E.document.buffer.rows[0];
    check(!strcmp(row->chars, "[Roberto]()") && E.document.cursor.cx == 10 &&
        row->markdown_count == 0 && !renderByteHidden(row, 0),
        "auto-closed empty destination remains literal and editable");
    const char *destination = "mailto:roberto@example.org";
    for (size_t i = 0; destination[i]; i++) {
        editorDispatchKey((uint8_t)destination[i]);
        editorSyncMarkdownPreview();
        row = &E.document.buffer.rows[0];
        check(!renderByteHidden(row, 0) && !renderByteHidden(row, row->rsize - 1),
            "destination stays in source editing after every typed byte");
    }
    editorDispatchKey(')');
    check(!strcmp(row->chars, "[Roberto](mailto:roberto@example.org)") &&
        E.document.cursor.cx == row->size, "typing closes destination without losing characters");
    editorDispatchKey(' ');
    editorSyncMarkdownPreview();
    check(renderByteHidden(row, 0), "leaving the completed typed link compacts it");
    editorUndo();
    editorRedo();
    check(!strcmp(E.document.buffer.rows[0].chars, "[Roberto](mailto:roberto@example.org) "),
        "undo and redo retain the typed link bytes");
    editorResetDocument();
    settingsDefaults(&S);
    editorSyncMarkdownPreview();
}

static void testMarkdownLinkMouse(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.markdown_live_preview = S.mouse_enabled = 1;
    S.show_line_numbers = S.show_menu = S.show_top_bar = 0;
    S.color_link_hover = COLOR_RED_DARK;
    T.visible = T.focused = 0;
    E.view.screenrows = 12; E.view.screencols = 80;
    E.view.rowoff = E.view.coloff = 0;
    E.document.file.filename = teStrdup("hover.md");
    editorInsertRow(0, "ok", 2);
    const char *source = "[é界](#target) tail";
    editorInsertRow(1, source, strlen(source));
    editorInsertRow(2, "## Target", 9);
    struct textLink link;
    struct markdownSpan span;
    int32_t cy;
    check(editorPreviewLinkAtMouse(1, 2, &cy, &link, &span) && cy == 1,
        "hover recognizes first visible label cell behind hidden opener");
    check(editorPreviewLinkAtMouse(3, 2, &cy, &link, &span), "hover recognizes second cell of wide label glyph");
    check(!editorPreviewLinkAtMouse(INT32_MAX, INT32_MAX, &cy, &link, &span),
        "out-of-screen mouse coordinates do not overflow hit testing");
    check(!editorPreviewLinkAtMouse(4, 2, &cy, &link, &span) &&
        !editorPreviewLinkAtMouse(1, 8, &cy, &link, &span), "hover ignores whitespace and EOF padding");
    mouseEventButton = 35; mouseEventPress = 1; mouseEventCol = 2; mouseEventRow = 2;
    editorHandleMouseEvent();
    struct abuf ab = ABUF_INIT;
    editorDrawRows(&ab); abAppend(&ab, "", 1);
    check(hovered_link_row == 1 && E.document.cursor.cy == 0 &&
        strstr(ab.b, "\x1b[31m\x1b[4mé") && !strstr(ab.b, "#target"),
        "hover colors underlined label without moving cursor or exposing markup");
    abFree(&ab);
    S.color_mode = COLOR_MODE_RGB; S.rgb_color_link_hover = 0x123456;
    ab = (struct abuf)ABUF_INIT;
    editorDrawRows(&ab); abAppend(&ab, "", 1);
    check(strstr(ab.b, "\x1b[38;2;18;52;86m\x1b[4mé") != NULL, "hover honors RGB palette");
    abFree(&ab);
    S.color_mode = COLOR_MODE_ANSI;
    mouseEventRow = 8;
    editorHandleMouseEvent();
    editorUpdateLinkHover();
    check(hovered_link_row == -1, "moving pointer away clears hover");
    E.document.cursor.cy = 1; E.document.cursor.cx = 3;
    editorSyncMarkdownPreview();
    erow *row = &E.document.buffer.rows[1];
    check(renderByteHidden(row, 0), "entering link label vertically leaves it compact");
    E.document.cursor.cx = 0;
    editorSyncMarkdownPreview();
    check(!renderByteHidden(row, 0), "left border opens link for editing");
    E.document.cursor.cx = 3;
    editorSyncMarkdownPreview();
    check(!renderByteHidden(row, 0) && !editorPreviewLinkAtMouse(3, 2, &cy, &link, &span),
        "link remains editable inside after entering from border and clicks edit its source");
    E.document.cursor.cy = 0; E.document.cursor.cx = 0;
    editorSyncMarkdownPreview();
    E.document.cursor.cy = 1; E.document.cursor.cx = row->markdown_spans[0].end;
    editorSyncMarkdownPreview();
    check(!renderByteHidden(row, 0), "right border opens link for editing");
    E.document.cursor.cy = 0; E.document.cursor.cx = 0;
    editorSyncMarkdownPreview();
    check(editorRowRxToCx(row, 3) == row->markdown_spans[0].end,
        "right label insertion boundary maps after hidden destination");
    S.soft_wrap = 2;
    check(editorPreviewLinkAtMouse(2, 3, &cy, &link, &span) && cy == 1,
        "wrapped wide link label remains clickable");
    mouseEventButton = 0; mouseEventPress = 1; mouseEventCol = 2; mouseEventRow = 3;
    editorHandleMouseEvent();
    check(E.document.cursor.cy == 2 && E.document.cursor.cx == 0 &&
        !E.document.mouse.dragging && !E.document.selection.active &&
        !strcmp(E.document.buffer.rows[1].chars, source), "single click follows anchor and preserves source");
    editorSyncMarkdownPreview();
    check(renderByteHidden(&E.document.buffer.rows[1], 0), "following link leaves its markup compact");
    S.soft_wrap = 0;
    S.markdown_live_preview = 0;
    check(!editorPreviewLinkAtMouse(3, 2, &cy, &link, &span), "ordinary editing has no preview activation");
    E.document.cursor.cy = 0;
    mouseEventButton = 0; mouseEventPress = 1; mouseEventCol = 3; mouseEventRow = 2;
    editorHandleMouseEvent();
    check(E.document.cursor.cy == 1 && E.document.cursor.cx == 3,
        "single click without preview retains normal cursor placement");
    editorResetDocument();
    settingsDefaults(&S);
    editorSyncMarkdownPreview();
}

static void testRedrawCaches(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.show_line_numbers = 1;
    E.view.screencols = 30;
    E.view.screenrows = 7;
    const char *lines[] = {"aé界👩🏽‍💻 abcdef", "", "abc\tdef", "end"};
    for (int32_t i = 0; i < 4; i++) editorInsertRow(i, lines[i], strlen(lines[i]));
    const int32_t widths[] = {1, 4, 11};
    for (size_t k = 0; k < sizeof(widths) / sizeof(widths[0]); k++) {
        S.soft_wrap = widths[k];
        int32_t total = editorTotalVideoRows(editorSoftWrapCols());
        for (int32_t offset = 0; offset <= total + 1; offset++) {
            E.view.rowoff = offset;
            struct abuf actual = ABUF_INIT, expected = ABUF_INIT;
            editorDrawRows(&actual);
            drawWrappedReference(&expected);
            check(actual.len == expected.len && memcmp(actual.b, expected.b, (size_t)actual.len) == 0,
                "sequential wrapped drawing matches reference at every scroll offset");
            abFree(&actual); abFree(&expected);
        }
    }
    int32_t expected_count = 3;
    for (int32_t i = 0; i < 4; i++) {
        size_t position = 0;
        while (position < strlen(lines[i])) {
            position += utf8NextCharLen(lines[i], position, strlen(lines[i]));
            expected_count++;
        }
    }
    check(editorCountChars() == expected_count && E.document.display_cache.chars_valid,
        "cached count includes graphemes and logical boundaries");
    editorPushUndo(EDIT_OTHER);
    editorRowInsertString(&E.document.buffer.rows[0], 0, "é", strlen("é"));
    check(!E.document.display_cache.chars_valid && editorCountChars() == expected_count + 1,
        "source edit invalidates character count");
    editorUndo();
    check(editorCountChars() == expected_count, "undo rebuilds character count");
    editorResetDocument();
    check(editorCountChars() == 0, "empty document count cache");
    editorInsertRow(0, "(abc)", 5);
    int32_t ay, ax, my, mx;
    check(editorMatchingPairAtCursor(&ay, &ax, &my, &mx) && mx == 4,
        "matching-pair cache stores source coordinates");
    editorRowInsertString(&E.document.buffer.rows[0], 1, "é", strlen("é"));
    check(editorMatchingPairAtCursor(&ay, &ax, &my, &mx) && mx == 6,
        "pair cache invalidates after text mutation");
    E.document.cursor.cx = 4;
    check(!editorMatchingPairAtCursor(&ay, &ax, &my, &mx), "moving cursor invalidates pair result");
    editorResetDocument();

    struct abuf frame = ABUF_INIT;
    abAppend(&frame, "a", 1);
    char *storage = frame.b;
    for (int32_t i = 0; i < 100; i++) abAppend(&frame, "é", 2);
    check(frame.b == storage && frame.len == 201 && frame.capacity >= 201,
        "small frame appends reuse allocation");
    abFree(&frame);
}

static void testReplaceAllHistoryNotice(void) {
    editorResetDocument();
    settingsDefaults(&S);
    S.undo_max_depth = 1;
    E.view.screenrows = 20;
    E.view.screencols = 80;
    editorInsertChar('a');
    int input[2];
    check(pipe(input) == 0, "replacement input pipe");
    check(write(input[1], "b\ra", 3) == 3, "replacement input bytes");
    close(input[1]);
    int saved_input = dup(STDIN_FILENO), saved_output = dup(STDOUT_FILENO);
    int sink = open("/dev/null", O_WRONLY);
    check(saved_input >= 0 && saved_output >= 0 && sink >= 0, "replacement descriptors");
    check(dup2(input[0], STDIN_FILENO) >= 0 && dup2(sink, STDOUT_FILENO) >= 0,
        "redirect replacement UI");
    close(input[0]);
    close(sink);
    editorFindAndReplace("a");
    check(dup2(saved_input, STDIN_FILENO) >= 0 && dup2(saved_output, STDOUT_FILENO) >= 0,
        "restore replacement descriptors");
    close(saved_input);
    close(saved_output);
    check(strcmp(E.document.buffer.rows[0].chars, "b") == 0,
        "replace all changes source");
    check(strstr(E.ui.statusmsg, "1 oldest action(s) removed") != NULL,
        "replace all preserves undo eviction notice");
    editorUndo();
    check(strcmp(E.document.buffer.rows[0].chars, "a") == 0,
        "replacement remains undoable after eviction");
    editorResetDocument();
}

static void testAuditRegressions(void) {
    editorResetDocument();
    settingsDefaults(&S);
    E.view.screencols = 100;
    E.view.screenrows = 20;
    int input[2];
    check(pipe(input) == 0, "audit input pipe");
    check(write(input[1], "\x1b", 1) == 1, "cancel replacement");
    close(input[1]);
    int saved_input = dup(STDIN_FILENO), saved_output = dup(STDOUT_FILENO);
    int sink = open("/dev/null", O_WRONLY);
    check(dup2(input[0], STDIN_FILENO) >= 0 && dup2(sink, STDOUT_FILENO) >= 0,
        "redirect audit UI");
    close(input[0]); close(sink);
    editorFindAndReplace("%n%s%%é");
    check(dup2(saved_input, STDIN_FILENO) >= 0 && dup2(saved_output, STDOUT_FILENO) >= 0,
        "restore audit UI");
    close(saved_input); close(saved_output);
    E.document.file.filename = teStrdup("name\x1b[2J.txt");
    struct abuf frame = ABUF_INIT;
    editorDrawTopBar(&frame);
    check(memmem(frame.b, (size_t)frame.len, "name\x1b[2J", 8) == NULL,
        "filename controls cannot clear screen");
    abFree(&frame);
    S.show_menu = 0; E.view.screencols = 1;
    strcpy(E.ui.statusmsg, "é"); E.ui.statusmsg_sticky = 1;
    editorDrawMessageBar(&frame);
    check(frame.len == 5 && memcmp(frame.b + 3, "é", 2) == 0,
        "message clipping uses complete UTF-8 grapheme");
    abFree(&frame);
    const char *sequences[] = {"\x1b[<999999999999;1;1M", "\x1b[999999999999;6u", "\x1b[<0;0;1M"};
    for (size_t i = 0; i < sizeof(sequences) / sizeof(sequences[0]); i++) {
        check(pipe(input) == 0, "decoder pipe");
        size_t bytes = strlen(sequences[i]);
        check(write(input[1], sequences[i], bytes) == (ssize_t)bytes, "queue invalid CSI");
        close(input[1]); saved_input = dup(STDIN_FILENO);
        check(dup2(input[0], STDIN_FILENO) >= 0, "redirect decoder"); close(input[0]);
        check(terminalReadKey(0) != MOUSE_EVENT_KEY, "reject overflow or zero mouse coordinate");
        check(dup2(saved_input, STDIN_FILENO) >= 0, "restore decoder"); close(saved_input);
    }
    editorResetDocument();
}

static void testControlBytes(void) {
    settingsDefaults(&S);
    S.show_menu = S.show_top_bar = S.show_line_numbers = S.syntax_highlight = 0;
    S.auto_close_pairs = 1;
    E.view.screenrows = 10; E.view.screencols = 60;
    editorResetDocument();
    editorInsertRow(0, "ab", 2);
    E.document.file.dirty = 0;
    /* Unbound controls and key events above the byte range are not text:
     * NUL/LF/EOT would corrupt the saved file, F10 (menu off) was inserted
     * as (uint8_t)1028. */
    const int32_t ignored[] = {0, CTRL_KEY('d'), CTRL_KEY('j'), CTRL_KEY('k'), 0x1c, F10_KEY};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        pending_key = ignored[i] == 0 ? 0 : ignored[i];
        editorDispatchKey(ignored[i]);
        check(E.document.buffer.row_count == 1 && E.document.buffer.rows[0].size == 2 &&
            !memcmp(E.document.buffer.rows[0].chars, "ab", 2) && !E.document.file.dirty,
            "unbound control keys never enter the document");
    }
    pending_key = -1;
    editorDispatchKey('x');
    check(E.document.buffer.rows[0].size == 3, "printable keys are still inserted");

    /* Controls stored in the document (e.g. a CRLF file with a doubled CR)
     * keep their bytes but are never written to the terminal. */
    editorResetDocument();
    editorInsertRow(0, "vis\r\x1b[2J\x07\x7f", 11);
    erow *row = &E.document.buffer.rows[0];
    check(row->size == 11, "controls remain in the row");
    struct abuf frame = ABUF_INIT;
    editorDrawRowSegment(&frame, 0, 0, row->rsize, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint8_t leaked = 0;
    for (int32_t i = 0; i < frame.len; i++)
        if ((uint8_t)frame.b[i] < 32 && frame.b[i] != '\x1b') leaked = 1;
    check(!leaked && !memmem(frame.b, (size_t)frame.len, "\x1b[2J", 4),
        "document controls cannot reach the terminal");
    check(memmem(frame.b, (size_t)frame.len, "vis", 3) != NULL, "visible text still drawn");
    abFree(&frame);

    /* The recovery screen prints the filename: controls are replaced and the
     * UTF-8 name is centered/clipped by columns. */
    editorResetDocument();
    E.document.file.filename = teStrdup("x\x1b[2Jy\xc3\xa8.txt");
    E.view.screencols = 40;
    struct abuf line = ABUF_INIT;
    editorRecoveryScreenLine(&line, "", E.document.file.filename);
    check(!memmem(line.b, (size_t)line.len, "\x1b[2J", 4) && memmem(line.b, (size_t)line.len, "x?[2Jy\xc3\xa8", 8),
        "recovery filename is sanitized");
    int32_t columns = 0;
    for (int32_t i = 0; i < line.len; ) {
        if (line.b[i] == '\x1b') { while (i < line.len && line.b[i] != 'm') i++; i++; continue; }
        if (line.b[i] == '\r' || line.b[i] == '\n') { i++; continue; }
        size_t step = utf8NextCharLen(line.b, (size_t)i, (size_t)line.len);
        columns += utf8SingleCharWidth(line.b + i, step);
        i += (int32_t)step;
    }
    check(columns == 40, "recovery line fills exactly the screen columns");
    abFree(&line);
    editorResetDocument();
}

static void testSettingsEditIntClamp(void) {
    settingsDefaults(&S);
    E.view.screenrows = 20; E.view.screencols = 80;
    const struct settingDescriptor *tab = settingsFind("tab_stop");
    check(tab != NULL, "tab_stop descriptor");
    struct editorSettings draft = S;
    const char *inputs[] = {"4294967297\r", "4294967300\r", "99999999999999\r", "-5\r"};
    const int32_t expected[] = {16, 16, 16, 1};
    for (size_t i = 0; i < 4; i++) {
        int pipefd[2];
        check(pipe(pipefd) == 0, "int prompt pipe");
        check(write(pipefd[1], inputs[i], strlen(inputs[i])) == (ssize_t)strlen(inputs[i]), "queue digits");
        close(pipefd[1]);
        int saved_in = dup(STDIN_FILENO), saved_out = dup(STDOUT_FILENO);
        int sink = open("/dev/null", O_WRONLY);
        check(dup2(pipefd[0], STDIN_FILENO) >= 0 && dup2(sink, STDOUT_FILENO) >= 0, "redirect int prompt");
        close(pipefd[0]); close(sink);
        int32_t value = 0;
        uint8_t accepted = editorSettingsEditInt(&draft, 0, 0, tab, &value);
        check(dup2(saved_in, STDIN_FILENO) >= 0 && dup2(saved_out, STDOUT_FILENO) >= 0, "restore int prompt");
        close(saved_in); close(saved_out);
        check(accepted && value == expected[i], "oversized numeric input clamps instead of wrapping");
    }
}

static void testSecondReviewRegressions(void) {
    settingsDefaults(&S);
    S.show_menu = S.show_top_bar = S.show_line_numbers = S.syntax_highlight = 0;
    E.view.screenrows = 10; E.view.screencols = 60;

    /* A ZWJ joins the next code point into the same grapheme, so a control
     * after it must still be filtered (ESC c would reset the terminal). */
    editorResetDocument();
    editorInsertRow(0, "A\xe2\x80\x8d\x1b" "c\r\xe2\x80\x8d\x07", 11);
    erow *row = &E.document.buffer.rows[0];
    struct abuf frame = ABUF_INIT;
    editorDrawRowSegment(&frame, 0, 0, row->rsize, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint8_t leaked = 0;
    for (int32_t i = 0; i < frame.len; i++) {
        uint8_t byte = (uint8_t)frame.b[i];
        if (byte < 32 && byte != 0x1b) leaked = 1;
        if (byte == 0x1b && i + 1 < frame.len && frame.b[i + 1] == 'c') leaked = 1;
    }
    check(!leaked && row->size == 11, "controls hidden behind a joiner never reach the terminal");
    abFree(&frame);

    /* A pathname replaced by a FIFO must not block the disk comparison. */
    char directory[] = "/tmp/tinyedit-fifo-XXXXXX";
    check(mkdtemp(directory) != NULL, "fifo fixture directory");
    char path[256];
    snprintf(path, sizeof(path), "%s/doc.txt", directory);
    FILE *file = fopen(path, "w");
    check(file && fputs("x\n", file) >= 0 && fclose(file) == 0, "fifo fixture file");
    editorResetDocument();
    check(editorOpen(path), "open fixture before replacing it");
    check(unlink(path) == 0 && mkfifo(path, 0600) == 0, "replace file with fifo");
    pid_t child = fork();
    check(child >= 0, "fork fifo probe");
    if (!child) { alarm(2); _exit(editorDiffersFromDisk() ? 0 : 1); }
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "FIFO in place of the file counts as different without blocking");
    unlink(path);

    /* Save as asks before replacing a different existing file. */
    char other[256];
    snprintf(other, sizeof(other), "%s/other.txt", directory);
    file = fopen(other, "w");
    check(file && fputs("precious\n", file) >= 0 && fclose(file) == 0, "existing destination");
    editorResetDocument();
    editorInsertRow(0, "new text", 8);
    const char *answers[] = {"n", "\x1b", "y"};
    for (size_t i = 0; i < 3; i++) {
        char keys[512];
        snprintf(keys, sizeof(keys), "%s\r%s", other, answers[i]);
        int input[2];
        check(pipe(input) == 0 && write(input[1], keys, strlen(keys)) == (ssize_t)strlen(keys), "queue save as");
        close(input[1]);
        int saved_in = dup(STDIN_FILENO), saved_out = dup(STDOUT_FILENO);
        int sink = open("/dev/null", O_WRONLY);
        check(dup2(input[0], STDIN_FILENO) >= 0 && dup2(sink, STDOUT_FILENO) >= 0, "redirect save as");
        close(input[0]); close(sink);
        editorSaveAs();
        check(dup2(saved_in, STDIN_FILENO) >= 0 && dup2(saved_out, STDOUT_FILENO) >= 0, "restore save as");
        close(saved_in); close(saved_out);
        char contents[32] = {0};
        file = fopen(other, "r");
        check(file != NULL && fgets(contents, sizeof(contents), file) != NULL, "read destination");
        fclose(file);
        if (i < 2) check(!strcmp(contents, "precious\n") && !E.document.file.filename,
            "declined overwrite leaves the file and the document name untouched");
        else check(!strcmp(contents, "new text\n") && E.document.file.filename,
            "confirmed overwrite replaces the file");
    }
    /* Saving over the already-open file never asks. */
    E.document.file.dirty = 1;
    check(editorConfirmOverwrite(other) == 1, "same file is not an overwrite");
    editorResetDocument();
    unlink(other);
    rmdir(directory);
}

static void testErrorMessages(void) {
    editorResetDocument();
    E.view.screencols = 80;
    S.show_menu = 0;
    char directory[] = "/tmp/tinyedit-link-error-XXXXXX";
    check(mkdtemp(directory) != NULL, "isolated dangling save directory");
    char link[256];
    snprintf(link, sizeof(link), "%s/link", directory);
    check(symlink("missing", link) == 0, "dangling editor save link");
    editorInsertRow(0, "unsaved", 7);
    E.document.file.dirty = 1;
    check(editorSaveToPath(link) == FILE_SAVE_FAILED && E.ui.statusmsg_error &&
        E.document.file.dirty && !E.document.file.filename &&
        !strcmp(E.document.buffer.rows[0].chars, "unsaved"),
        "failed dangling save reports an error and preserves document state");
    check(unlink(link) == 0 && rmdir(directory) == 0, "remove dangling save fixture");
    editorSetErrorMessage("Can't save: %s", "missing target");
    struct abuf frame = ABUF_INIT;
    editorDrawMessageBar(&frame);
    abAppend(&frame, "", 1);
    check(strstr(frame.b, "\x1b[31mCan't save: missing target\x1b[39m") != NULL,
        "error text rendered in red and foreground restored");
    abFree(&frame);
    editorSetStatusMessage("Saved");
    check(!E.ui.statusmsg_error, "normal message clears error severity");
    editorSetErrorMessage("failed");
    editorSetStatusMessageSticky("hint");
    check(!E.ui.statusmsg_error, "sticky hint clears error severity");
}

static void tableFixture(const char *name, const char *const *rows, int32_t count) {
    editorResetDocument(); settingsDefaults(&S);
    editorSetStatusMessage("");
    E.document.file.filename = teStrdup(name);
    for (int32_t y = 0; y < count; y++) editorInsertRow(y, rows[y], strlen(rows[y]));
    E.document.cursor.cy = 0; E.document.cursor.cx = 0;
}

static void testSmartTable(void) {
    const char *rows[] = {"|a|long|", "|:---|---:|", "|界😀é|x|"};
    tableFixture("table.md", rows, 3);
    check(!S.smart_table, "Smart Table defaults off");
    editorHandleEditKey('\t');
    check(E.document.buffer.rows[0].chars[0] != '|', "disabled Tab retains indentation");
    tableFixture("table.md", rows, 3); S.smart_table = 1;
    E.document.cursor.cx = 2;
    E.document.selection.active = 1;
    E.document.selection.anchor_y = 2; E.document.selection.anchor_x = 11;
    editorBeginEdit(); editorPushUndo(EDIT_OTHER);
    check(editorTableAlign(1, 0), "Smart Table explicit alignment"); editorEndEdit();
    check(!strcmp(E.document.buffer.rows[0].chars, "| a     | long |"), "display widths and right alignment");
    check(strstr(E.document.buffer.rows[2].chars, "界😀é") != NULL, "Unicode bytes preserved");
    check(E.document.cursor.cx == 3 && E.document.selection.active,
        "format preserves cursor offset in cell and selection");
    char *aligned = teStrdup(E.document.buffer.rows[0].chars);
    editorUndo(); check(!strcmp(E.document.buffer.rows[0].chars, rows[0]) &&
        E.document.selection.active && E.document.cursor.cx == 2, "format undo exact source and selection");
    editorRedo(); check(!strcmp(E.document.buffer.rows[0].chars, aligned), "format redo exact source"); free(aligned);
    E.document.selection.active = 0; E.document.cursor.cx = 2;
    editorHandleEditKey('\t'); check(E.document.cursor.cx == 10, "Tab next cell");
    editorHandleEditKey(SHIFT_TAB); check(E.document.cursor.cx == 2, "decoded Shift Tab previous cell");
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('\r');
    check(E.document.buffer.row_count == 4 && E.document.cursor.cy == 2, "Enter creates data row after separator");
    editorHandleEditKey('\r');
    check(!E.document.buffer.rows[2].size && E.document.cursor.cy == 2, "empty row exits table");
    editorUndo(); check(E.document.buffer.rows[2].chars[0] == '|', "exit undo atomic");
    editorUndo(); check(E.document.buffer.row_count == 3, "continuation undo atomic");
    const char *literal[] = {"|a\\|b|`c|d`|[é](path)|", "|x|``a`|b``|\xff|"};
    tableFixture("table.md", literal, 2); S.smart_table = 1;
    struct smartTableRow parsed;
    check(smartTableParse(&E.document.buffer.rows[0], &parsed) && parsed.count == 3,
        "escaped pipes and inline code do not split cells");
    editorBeginEdit(); editorPushUndo(EDIT_OTHER); check(editorTableAlign(1, 0), "literal cells align"); editorEndEdit();
    check(strstr(E.document.buffer.rows[0].chars, "a\\|b") && strstr(E.document.buffer.rows[0].chars, "[é](path)") &&
        strstr(E.document.buffer.rows[2].chars, "\xff"), "links and malformed bytes preserved");
    const char *fence[] = {"```", "|a|b|", "```"};
    tableFixture("table.md", fence, 3); S.smart_table = 1; S.syntax_highlight = 0;
    E.document.cursor.cy = 1;
    struct smartTable table;
    check(!editorTableContext(&table), "fenced code excluded with highlighting off");
    tableFixture("table.c", rows, 3); S.smart_table = 1;
    check(!editorTableContext(&table), "non Markdown excluded");
    const char *ambiguous[] = {"|a|b|", "|c|"};
    tableFixture("table.md", ambiguous, 2); S.smart_table = 1;
    check(!editorTableContext(&table), "inconsistent table left intact");
    const char *partial[] = {"|a|b"};
    tableFixture("table.md", partial, 1); S.smart_table = 1;
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('|');
    check(!strcmp(E.document.buffer.rows[0].chars, "| a   | b   |"), "typed pipe aligns completed row");
    editorUndo(); check(!strcmp(E.document.buffer.rows[0].chars, partial[0]), "pipe and alignment undo together");
    editorRedo(); check(!strcmp(E.document.buffer.rows[0].chars, "| a   | b   |"), "pipe and alignment redo together");
    tableFixture("table.md", partial, 1); S.smart_table = 1;
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('c'); editorHandleEditKey('|'); editorUndo();
    check(!strcmp(E.document.buffer.rows[0].chars, "|a|bc"), "formatting pipe does not coalesce with prior typing");
    editorRedo();
    E.document.file.read_only = 1;
    char *before = teStrdup(E.document.buffer.rows[0].chars);
    editorHandleEditKey('|'); editorHandleEditKey('\r');
    check(!strcmp(before, E.document.buffer.rows[0].chars) && E.ui.statusmsg_error,
        "read only rejects Smart Table edits in red"); free(before);
    E.document.file.read_only = 0;
    const char *combining[] = {"|a|́b|"};
    tableFixture("table.md", combining, 1);
    editorBeginEdit(); editorPushUndo(EDIT_OTHER); check(editorTableAlign(1, 0), "leading combining cell aligns"); editorEndEdit();
    check(strstr(E.document.buffer.rows[0].chars, "́b") != NULL,
        "combining marks immediately after a pipe are retained");
    const char *tabs[] = {"|a\tb|界|", "|:---:|---:|", "|x|longer|"};
    tableFixture("table.md", tabs, 3); S.smart_table = 1;
    editorBeginEdit(); editorPushUndo(EDIT_OTHER); check(editorTableAlign(1, 0), "tab table aligns"); editorEndEdit();
    int32_t columns[3];
    for (int32_t y = 0; y < 3; y++) {
        smartTableParse(&E.document.buffer.rows[y], &parsed);
        columns[y] = bufferRowCxToRx(&E.document.buffer.rows[y], parsed.cells[0].close, S.tab_stop);
    }
    check(columns[0] == columns[1] && columns[1] == columns[2] &&
        strchr(E.document.buffer.rows[0].chars, '\t'), "tabs retain bytes and align visual separators");
    const char *raw[] = {"|a|b"};
    tableFixture("table.md", raw, 1);
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('|');
    check(!strcmp(E.document.buffer.rows[0].chars, "|a|b|"), "disabled pipe leaves spacing intact");
    tableFixture("table.txt", raw, 1); S.smart_table = 1;
    E.document.cursor.cx = E.document.buffer.rows[0].size; editorHandleEditKey('|');
    check(!strcmp(E.document.buffer.rows[0].chars, "|a|b|"), "non Markdown pipe unchanged");
    const char *escaped[] = {"|a\\b|c|"};
    tableFixture("table.md", escaped, 1); S.smart_table = 1;
    E.document.cursor.cx = 3; editorHandleEditKey('|');
    check(!strcmp(E.document.buffer.rows[0].chars, "|a\\|b|c|"), "typing escaped pipe does not trigger formatting");
    const char *unbalanced[] = {"|a|`b|"};
    tableFixture("table.md", unbalanced, 1); S.smart_table = 1;
    check(!editorTableContext(&table), "unmatched inline code left intact");
    const char *wide[] = {"|abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz|b|"};
    tableFixture("table.md", wide, 1);
    editorBeginEdit(); editorPushUndo(EDIT_OTHER); check(editorTableAlign(1, 0), "wide table aligns"); editorEndEdit();
    check(strstr(E.document.buffer.rows[0].chars, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz") != NULL,
        "more than eighty display columns never truncates content");
    char many[SMART_TABLE_COLUMNS * 2 + 4]; int32_t n = 0;
    many[n++] = '|';
    for (int32_t c = 0; c <= SMART_TABLE_COLUMNS; c++) { many[n++] = 'x'; many[n++] = '|'; }
    const char *over[] = {many}; many[n] = '\0';
    tableFixture("table.md", over, 1);
    check(!editorTableContext(&table), "column processing limit leaves source intact");
    const char *one[] = {"|x|"}; tableFixture("table.md", one, 1);
    for (int32_t y = 1; y <= SMART_TABLE_ROWS; y++) editorInsertRow(y, "|x|", 3);
    check(!editorTableContext(&table), "row processing limit rejects whole table");
    tableFixture("table.md", rows, 3); S.smart_table = 0;
    E.document.cursor.cx = 2;
    M.open = 1; M.selected_menu = 2; M.selected_item = 9;
    editorDispatchKey('\r');
    check(!strcmp(E.document.buffer.rows[0].chars, "| a     | long |"), "realign menu works with setting off");
    E.document.file.read_only = 1;
    M.open = 1; M.selected_menu = 2; M.selected_item = 9; editorDispatchKey('\r');
    check(E.ui.statusmsg_error, "realign menu respects read only");
    E.document.file.read_only = 0;
    tableFixture("table.md", partial, 1); S.smart_table = 1;
    E.document.history.budget = 1;
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('|');
    check(!strcmp(E.document.buffer.rows[0].chars, partial[0]) && E.ui.statusmsg_error &&
        E.document.cursor.cx == (int32_t)strlen(partial[0]), "Smart Table history limit cancels edit and cursor atomically");
    editorResetDocument(); settingsDefaults(&S);
}

static void testSmartTableSeparator(void) {
    const char *rows[] = {"|aa|bb|cc|dd|", "|---------|:-----------|-------------:|:----------:|", "|x|y|longer|z|"};
    tableFixture("separator.md", rows, 3);
    E.document.cursor.cy = 1; E.document.cursor.cx = 45;
    E.document.selection.active = 1; E.document.selection.anchor_y = 1;
    E.document.selection.anchor_x = 49;
    editorHandleCommandKey(SMART_TABLE_KEY);
    check(!strcmp(E.document.buffer.rows[1].chars, "| --- |:--- | ------:|:---:|"),
        "second row normalizes all four alignments and shrinks surplus dashes");
    check(E.document.cursor.cx <= E.document.buffer.rows[1].size &&
        E.document.selection.anchor_x <= E.document.buffer.rows[1].size,
        "shrinking delimiter keeps cursor and selection in bounds");
    char *formatted = teStrdup(E.document.buffer.rows[1].chars);
    editorUndo(); check(!strcmp(E.document.buffer.rows[1].chars, rows[1]), "delimiter undo preserves original markers");
    editorRedo(); check(!strcmp(E.document.buffer.rows[1].chars, formatted), "delimiter redo exact");
    editorHandleCommandKey(SMART_TABLE_KEY);
    check(!strcmp(E.document.buffer.rows[1].chars, formatted), "delimiter formatting is idempotent"); free(formatted);
    const char *short_rows[] = {"|h|h|h|h|", "|-|:-|-:|:-:|"};
    tableFixture("separator.md", short_rows, 2);
    editorHandleCommandKey(SMART_TABLE_KEY);
    check(!strcmp(E.document.buffer.rows[1].chars, "| --- |:--- | ---:|:---:|"),
        "short GFM delimiters expand to at least three dashes");
    const char *missing[] = {"|head|value|", "|界|bytes\xff|"};
    tableFixture("separator.md", missing, 2);
    E.document.cursor.cy = 1; E.document.cursor.cx = 4;
    E.document.selection.active = 1; E.document.selection.anchor_y = 1; E.document.selection.anchor_x = 1;
    editorHandleCommandKey(SMART_TABLE_KEY);
    check(E.document.buffer.row_count == 3 && E.document.cursor.cy == 2 &&
        E.document.selection.anchor_y == 2 && strstr(E.document.buffer.rows[2].chars, "bytes\xff"),
        "missing delimiter is inserted without overwriting data or losing logical selection");
    editorUndo(); check(E.document.buffer.row_count == 2 && E.document.cursor.cy == 1 &&
        !strcmp(E.document.buffer.rows[1].chars, missing[1]), "insertion and alignment undo atomically");
    const char *header[] = {"|head|value|"};
    tableFixture("separator.md", header, 1); S.smart_table = 1;
    E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('\r');
    check(E.document.buffer.row_count == 3 && E.document.cursor.cy == 2 &&
        !strcmp(E.document.buffer.rows[1].chars, "| ---- | ----- |"),
        "Enter confirms header with mandatory delimiter and prepares first data row");
    editorUndo(); check(E.document.buffer.row_count == 1 &&
        !strcmp(E.document.buffer.rows[0].chars, header[0]), "header completion undo atomic");
    editorRedo(); check(E.document.buffer.row_count == 3 && E.document.cursor.cy == 2,
        "header completion redo atomic");
    editorResetDocument(); settingsDefaults(&S);
}

static void testTableHighlightPropagation(void) {
    const char *rows[] = {"|h|h|", "|---|---|", "|---|---|", "|é\t界|`a|b`|"};
    tableFixture("table.md", rows, 4);
    editorUpdateAllRows();
    check(E.document.buffer.rows[1].hl[2] == HL_TABLE_DELIMITER &&
        E.document.buffer.rows[2].hl[2] == HL_NORMAL, "only second-row dashes are table markers");
    erow *last = &E.document.buffer.rows[3];
    int32_t code_pipe = (int32_t)(strstr(last->render, "a|b") - last->render) + 1;
    check(last->hl[code_pipe] == HL_STRING && last->hl[last->rsize - 1] == HL_TABLE_DELIMITER,
        "expanded tabs and Unicode keep render delimiter offsets correct");
    E.document.cursor.cx = 1; editorHandleEditKey(BACKSPACE);
    check(E.document.buffer.rows[1].hl[2] == HL_NORMAL &&
        E.document.buffer.rows[2].hl[2] == HL_TABLE_DELIMITER,
        "header editing refreshes both dependent rows");
    editorUndo();
    check(E.document.buffer.rows[1].hl[2] == HL_TABLE_DELIMITER &&
        E.document.buffer.rows[2].hl[2] == HL_NORMAL, "undo restores delimiter highlighting");
    S.syntax_highlight = 0; editorUpdateAllRows();
    check(!E.document.buffer.rows[1].hl, "disabled syntax leaves table markers uncolored");
    editorResetDocument(); settingsDefaults(&S);
}

static char *tableAuditText(size_t *length) {
    return bufferSerialize(&E.document.buffer, LINE_ENDING_LF, 1, length);
}

static void testTableAuditFixes(void) {
    const char *first[] = {"界aaaa", "éaaaaa", "éaaaaa", "😀aaaa", "👩‍💻aaaa", ("\xff" "aaaaa")};
    const char *separators[] = {"|---|---|", "|:---|:---|", "|---:|---:|", "|:---:|:---:|"};
    for (size_t variant = 0; variant < sizeof(first) / sizeof(first[0]); variant++) {
        for (size_t alignment = 0; alignment < sizeof(separators) / sizeof(separators[0]); alignment++) {
            char header[128]; snprintf(header, sizeof(header), "|%s|\t|", first[variant]);
            const char *rows[] = {header, separators[alignment], "|x|y|"};
            tableFixture("audit.md", rows, 3);
            E.document.cursor.cx = E.document.buffer.rows[0].size - 1;
            E.document.selection.active = 1; E.document.selection.anchor_y = 2; E.document.selection.anchor_x = 1;
            size_t original_length; char *original = tableAuditText(&original_length);
            editorHandleCommandKey(SMART_TABLE_KEY);
            check(!E.ui.statusmsg_error, "mixed UTF-8/tab columns format safely for each alignment");
            struct smartTableRow parsed;
            int32_t columns[2] = {0};
            for (int32_t y = 0; y < 3; y++) {
                erow *row = &E.document.buffer.rows[y]; smartTableParse(row, &parsed);
                for (int32_t c = 0; c < 2; c++) {
                    int32_t column = bufferRowCxToRx(row, parsed.cells[c].close, S.tab_stop);
                    if (!y) columns[c] = column;
                    check(column == columns[c], "pipe columns use visual widths after multibyte cells and tabs");
                }
            }
            check(strstr(E.document.buffer.rows[0].chars, first[variant]) &&
                strchr(E.document.buffer.rows[0].chars, '\t') && E.document.selection.active,
                "format retains Unicode/malformed bytes, literal tab and selection");
            size_t formatted_length; char *formatted = tableAuditText(&formatted_length);
            editorUndo(); size_t restored_length; char *restored = tableAuditText(&restored_length);
            check(restored_length == original_length && !memcmp(restored, original, original_length),
                "mixed column formatting undo restores exact source"); free(restored);
            editorRedo(); restored = tableAuditText(&restored_length);
            check(restored_length == formatted_length && !memcmp(restored, formatted, formatted_length),
                "mixed column formatting redo restores exact output");
            free(restored); free(formatted); free(original);
        }
    }
    erow invalid = {0}; invalid.chars = "|a|xxxx|"; invalid.size = (int32_t)strlen(invalid.chars);
    struct smartTableRow parsed; smartTableParse(&invalid, &parsed);
    int32_t widths[] = {3, 3}, cursor = 1, anchor = 1; uint8_t align[] = {0, 0};
    char *result = smartTableFormat(&invalid, &parsed, widths, align, 4, &cursor, &anchor);
    check(!result && cursor == 1 && anchor == 1, "invalid padding rejects without mutating position outputs");
    free(result);
    erow joined = {0}; joined.chars = "|x‍ |"; joined.size = (int32_t)strlen(joined.chars);
    check(smartTableParse(&joined, &parsed) && parsed.cells[0].end == 5,
        "backward scalar trimming retains a joiner before ASCII padding");

    const char *seed[] = {"|x|"}; tableFixture("audit.md", seed, 1);
    char header[2003]; header[0] = '|'; memset(header + 1, 'a', 2000); header[2001] = '|'; header[2002] = 0;
    bufferRowDeleteRange(&E.document.buffer.rows[0], 0, E.document.buffer.rows[0].size);
    bufferRowInsert(&E.document.buffer.rows[0], 0, header, 2002); editorUpdateRow(&E.document.buffer.rows[0]);
    editorInsertRow(1, "|---|", 5);
    for (int32_t y = 2; y < SMART_TABLE_ROWS; y++) editorInsertRow(y, "|x|", 3);
    E.document.cursor.cy = 2; E.document.cursor.cx = 2;
    E.document.selection.active = 1; E.document.selection.anchor_y = 1000; E.document.selection.anchor_x = 1;
    size_t original_length; char *original = tableAuditText(&original_length);
    int32_t history_count = E.document.history.undo_count;
    editorHandleCommandKey(SMART_TABLE_KEY);
    size_t restored_length; char *restored = tableAuditText(&restored_length);
    check(E.ui.statusmsg_error && restored_length == original_length && !memcmp(restored, original, original_length) &&
        E.document.cursor.cy == 2 && E.document.cursor.cx == 2 && E.document.selection.anchor_y == 1000 &&
        E.document.history.undo_count == history_count, "aggregate output limit rejects text, position and history changes atomically");
    struct smartTable table; check(editorTableContext(&table), "rejected table remains recognizable");
    free(restored); free(original);
    S.smart_table = 1; E.document.selection.active = 0;
    bufferRowDeleteRange(&E.document.buffer.rows[2], 2, 3); editorUpdateRow(&E.document.buffer.rows[2]);
    E.document.cursor.cy = 2; E.document.cursor.cx = 2;
    editorHandleEditKey('|');
    check(E.ui.statusmsg_error && !strcmp(E.document.buffer.rows[2].chars, "|x|") &&
        !strcmp(E.document.buffer.rows[0].chars, header), "automatic budget rejection keeps typed pipe without partial formatting");
    editorUndo(); check(!strcmp(E.document.buffer.rows[2].chars, "|x"), "rejected alignment does not break typed pipe undo");

    tableFixture("audit.md", seed, 1);
    size_t large = SMART_TABLE_BYTES / 2 - 8;
    char *large_header = teMalloc(large + 3); large_header[0] = '|';
    memset(large_header + 1, 'a', large); large_header[large + 1] = '|'; large_header[large + 2] = 0;
    bufferRowDeleteRange(&E.document.buffer.rows[0], 0, 3);
    bufferRowInsert(&E.document.buffer.rows[0], 0, large_header, large + 2);
    editorUpdateRow(&E.document.buffer.rows[0]); editorInsertRow(1, "|---|", 5); free(large_header);
    S.smart_table = 1; E.document.cursor.cx = E.document.buffer.rows[0].size;
    original = tableAuditText(&original_length); editorHandleEditKey('\r'); restored = tableAuditText(&restored_length);
    check(E.ui.statusmsg_error && original_length == restored_length && !memcmp(original, restored, original_length) &&
        E.document.buffer.row_count == 2 && !E.document.cursor.cy,
        "Enter includes the prospective data row in output preflight");
    free(restored); free(original);
    bufferRowDeleteRange(&E.document.buffer.rows[1], 0, E.document.buffer.rows[1].size);
    bufferRowInsert(&E.document.buffer.rows[1], 0, "|x|", 3); editorUpdateRow(&E.document.buffer.rows[1]);
    original = tableAuditText(&original_length);
    editorHandleCommandKey(SMART_TABLE_KEY); restored = tableAuditText(&restored_length);
    check(E.ui.statusmsg_error && E.document.buffer.row_count == 2 && original_length == restored_length &&
        !memcmp(original, restored, original_length), "mandatory separator is included before any insertion is applied");
    free(restored); free(original);

    tableFixture("audit.md", seed, 1);
    size_t prefix_size = SMART_TABLE_BYTES - 500;
    char *prefix = teMalloc(prefix_size + 1); memset(prefix, 'a', prefix_size); prefix[prefix_size] = 0;
    editorInsertRow(0, prefix, prefix_size); free(prefix);
    char prefix_header[303]; prefix_header[0] = '|'; memset(prefix_header + 1, 'b', 300);
    prefix_header[301] = '|'; prefix_header[302] = 0;
    bufferRowDeleteRange(&E.document.buffer.rows[1], 0, 3);
    bufferRowInsert(&E.document.buffer.rows[1], 0, prefix_header, 302); editorUpdateRow(&E.document.buffer.rows[1]);
    editorInsertRow(2, "|---|", 5); E.document.cursor.cy = 1;
    original = tableAuditText(&original_length);
    editorHandleCommandKey(SMART_TABLE_KEY); restored = tableAuditText(&restored_length);
    check(E.ui.statusmsg_error && original_length == restored_length && !memcmp(original, restored, original_length),
        "output preflight includes source preceding the table");
    free(restored); free(original);

    tableFixture("audit.md", seed, 1);
    char boundary[1022]; boundary[0] = '|'; memset(boundary + 1, 'a', 1019); boundary[1020] = '|'; boundary[1021] = 0;
    bufferRowDeleteRange(&E.document.buffer.rows[0], 0, 3);
    bufferRowInsert(&E.document.buffer.rows[0], 0, boundary, 1021); editorUpdateRow(&E.document.buffer.rows[0]);
    editorInsertRow(1, "|---|", 5);
    for (int32_t y = 2; y < SMART_TABLE_ROWS - 1; y++) editorInsertRow(y, "|x|", 3);
    S.smart_table = 1; E.document.cursor.cx = E.document.buffer.rows[0].size;
    editorHandleEditKey('\r'); restored = tableAuditText(&restored_length);
    check(!E.ui.statusmsg_error && restored_length == SMART_TABLE_BYTES &&
        E.document.buffer.row_count == SMART_TABLE_ROWS && editorTableContext(&table),
        "exact aggregate byte and row boundaries remain supported after Enter"); free(restored);
    editorResetDocument(); settingsDefaults(&S);
}

int main(void) {
    settingsDefaults(&S);
    E.search.search_match_y = -1;
    E.search.search_match_end_y = -1;
    testSmartTable();
    testSmartTableSeparator();
    testTableHighlightPropagation();
    testTableAuditFixes();
    testErrorMessages();
    testAuditRegressions();
    testControlBytes();
    testSecondReviewRegressions();
    testSettingsEditIntClamp();
    testPromptGrowth();
    testPathCompletion();
    testFilesystemTree();
    testReplaceAllHistoryNotice();
    testUndoModified();
    testHistoryMemoryRecovery();
    testTerminalOutput();
    testTopBar();
    erow heading = {0};
    heading.render = "   ## Héading";
    heading.rsize = (int32_t)strlen(heading.render);
    syntaxHighlightRow(&heading, "test.md", 1, 0, 0, 0, 1, 0);
    check(heading.hl_heading, "indented Markdown heading recognized");
    syntaxHighlightRow(&heading, "test.md", 1, 1, 0, 0, 1, 0);
    check(!heading.hl_heading, "heading inside code fence excluded");
    syntaxHighlightRow(&heading, "test.c", 1, 0, 0, 0, 1, 0);
    check(!heading.hl_heading, "heading state cleared on filetype change");
    free(heading.hl);
    testEmptyPages();
    testUnicodeTabs();
    testDocumentTransactions();
    testNewDocument();
    testColorSettings();
    testMouseDispatch();
    testMouseScrollView();
    testLinks();
    testMarkdownNavigation();
    testMarkdownCodeBlocks();
    testSharedAutoClose();
    testEditBatches();
    testSearchSession();
    testBracketColors();
    testMarkdownStyles();
    testMarkdownPreview();
    testMarkdownLinkMouse();
    testTypingMarkdownLink();
    testHeadingReverse();
    testRedrawCaches();
    puts("core tests: ok");
    return 0;
}
