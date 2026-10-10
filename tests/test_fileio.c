#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "fileio.h"
#include "alloc.h"
#include "backup.h"
#include "settings.h"
#include "buffer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int32_t read_failure_after = -1, read_calls;
static int read_errno = EIO;
static uint8_t fail_stream_close, fail_file_sync, fail_directory_sync;
static uint8_t fail_rename, fail_file_close, fail_directory_close;
static uint8_t short_writes, interrupt_write, interrupt_sync;
static int32_t writes_before_failure = -1;
static uint8_t zero_write, fail_write;

static ssize_t faultGetline(char **line, size_t *capacity, FILE *stream) {
    if (read_failure_after >= 0 && read_calls++ >= read_failure_after) {
        errno = read_errno;
        return -1;
    }
    return getline(line, capacity, stream);
}

static int faultFclose(FILE *stream) {
    int result = fclose(stream);
    if (result == 0 && fail_stream_close) { errno = EIO; return EOF; }
    return result;
}

static int faultFsync(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (interrupt_sync) { interrupt_sync = 0; errno = EINTR; return -1; }
    if ((S_ISDIR(st.st_mode) && fail_directory_sync) ||
        (!S_ISDIR(st.st_mode) && fail_file_sync)) { errno = EIO; return -1; }
    return fsync(fd);
}

static int faultRename(const char *source, const char *target) {
    if (fail_rename) { errno = EACCES; return -1; }
    return rename(source, target);
}

static ssize_t faultWrite(int fd, const void *bytes, size_t len) {
    if (interrupt_write) { interrupt_write = 0; errno = EINTR; return -1; }
    if (fail_write || writes_before_failure == 0) { errno = ENOSPC; return -1; }
    if (writes_before_failure > 0) writes_before_failure--;
    if (zero_write) return 0;
    if (short_writes && len > 2) len = 2;
    return write(fd, bytes, len);
}

static int faultClose(int fd) {
    struct stat st;
    uint8_t valid = fstat(fd, &st) == 0;
    uint8_t regular = valid && S_ISREG(st.st_mode);
    uint8_t directory = valid && S_ISDIR(st.st_mode);
    int result = close(fd);
    if (result == 0 && ((regular && fail_file_close) ||
        (directory && fail_directory_close))) { errno = EIO; return -1; }
    return result;
}

#define getline faultGetline
#define fclose faultFclose
#define fsync faultFsync
#define rename faultRename
#define write faultWrite
#define close faultClose
#include "../src/fileio.c"
#undef getline
#undef fclose
#undef fsync
#undef rename
#undef write
#undef close

static void check(uint8_t condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

static void putFile(const char *path, const char *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    check(fp != NULL, "create fixture");
    check(fwrite(bytes, 1, len, fp) == len && fclose(fp) == 0, "write fixture");
}

static void checkFile(const char *path, const char *expected) {
    char bytes[128];
    FILE *fp = fopen(path, "rb");
    check(fp != NULL, "open saved file");
    size_t len = fread(bytes, 1, sizeof(bytes), fp);
    check(len == strlen(expected) && memcmp(bytes, expected, len) == 0,
        "saved file content");
    check(fclose(fp) == 0, "close saved file");
}

static void testLoading(const char *path, const char *directory) {
    const char *samples[] = {"", "abc\r", "abc\r\r\n", "\r\n", "a\r\nb\r\n", "a\nb", "a\nb\n", "é\t界\r"};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        putFile(path, samples[i], strlen(samples[i]));
        struct editorDocument document;
        check(fileioLoadDocument(path, &document), "load complete document");
        size_t len;
        char *serialized = bufferSerialize(&document.buffer,
            document.file.detected_line_ending, document.file.final_newline, &len);
        check(len == strlen(samples[i]) && memcmp(serialized, samples[i], len) == 0,
            "exact line ending and content CR round trip");
        free(serialized);
        free(document.file.filename);
        bufferClear(&document.buffer);
    }
    const char binary[] = "text row\n\x89PNG\r\n\x1a\n\0binary";
    putFile(path, binary, sizeof(binary) - 1);
    struct editorDocument binary_document;
    check(!fileioLoadDocument(path, &binary_document) && errno == EILSEQ,
        "binary input rejected even after valid text rows");
    check(binary_document.buffer.rows == NULL && binary_document.buffer.row_count == 0 &&
        binary_document.file.filename == NULL, "binary rejection releases candidate");
    const char mixed[] = "a\r\nb\nc\r\r\nlast\r";
    putFile(path, mixed, sizeof(mixed) - 1);
    struct editorDocument mixed_document;
    check(fileioLoadDocument(path, &mixed_document) &&
        mixed_document.file.line_endings_mixed &&
        mixed_document.file.detected_line_ending == LINE_ENDING_CRLF &&
        !mixed_document.file.final_newline && mixed_document.buffer.rows[2].size == 2 &&
        mixed_document.buffer.rows[3].size == 5, "mixed ending detection preserves content CR");
    bufferClear(&mixed_document.buffer);
    free(mixed_document.file.filename);
    putFile(path, "first\nsecond\n", 13);
    const int errors[] = {EIO, ENOMEM, EINTR};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        read_failure_after = 1; read_calls = 0; read_errno = errors[i];
        struct editorDocument candidate;
        check(!fileioLoadDocument(path, &candidate) && errno == errors[i], "partial read failure propagated");
        check(candidate.buffer.row_count == 0 && candidate.buffer.rows == NULL &&
            candidate.file.filename == NULL, "partial candidate released");
    }
    read_failure_after = -1;
    fail_stream_close = 1;
    struct editorDocument candidate;
    check(!fileioLoadDocument(path, &candidate) && errno == EIO, "read close failure propagated");
    check(!candidate.buffer.rows && !candidate.file.filename, "close failure candidate released");
    fail_stream_close = 0;
    check(!fileioLoadDocument(directory, &candidate) && errno == EISDIR, "directory rejected");
    check(unlink(path) == 0 && fileioLoadDocument(path, &candidate), "missing document accepted");
    check(candidate.buffer.row_count == 0 && candidate.file.final_newline, "new document metadata");
    free(candidate.file.filename);
}

static void testSaving(const char *path, const char *directory) {
    const char content[] = "caffè\nnew\r";
    for (int32_t failure = 0; failure < 6; failure++) {
        putFile(path, "original", 8);
        fail_write = failure == 0; zero_write = failure == 1;
        fail_file_sync = failure == 2; fail_file_close = failure == 3;
        fail_rename = failure == 4;
        short_writes = failure == 5;
        writes_before_failure = failure == 5 ? 2 : -1;
        check(fileioAtomicSave(path, content, sizeof(content) - 1) == FILE_SAVE_FAILED,
            "failure before replacement");
        checkFile(path, "original");
        fail_write = zero_write = fail_file_sync = fail_file_close = fail_rename = short_writes = 0;
        writes_before_failure = -1;
        DIR *dir = opendir(directory);
        check(dir != NULL, "inspect temporary files");
        struct dirent *entry;
        while ((entry = readdir(dir)))
            check(strstr(entry->d_name, ".tinyedit.") == NULL, "temporary removed after failure");
        closedir(dir);
    }
    check(chmod(path, 0600) == 0, "set target permissions");
    short_writes = interrupt_write = interrupt_sync = 1;
    check(fileioAtomicSave(path, content, sizeof(content) - 1) == FILE_SAVE_DURABLE,
        "short writes and signals handled");
    short_writes = 0;
    checkFile(path, content);
    struct stat st;
    check(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "target mode preserved");
    fail_directory_sync = 1;
    check(fileioAtomicSave(path, "replaced", 8) == FILE_SAVE_UNCERTAIN,
        "post rename uncertainty distinguished");
    checkFile(path, "replaced");
    fail_directory_sync = 0;
    fail_directory_close = 1;
    check(fileioAtomicSave(path, "close uncertain", 15) == FILE_SAVE_UNCERTAIN,
        "post rename directory close failure distinguished");
    checkFile(path, "close uncertain");
    fail_directory_close = 0;
    char link[1024];
    snprintf(link, sizeof(link), "%s/link", directory);
    check(symlink(path, link) == 0, "create symlink");
    check(fileioAtomicSave(link, content, sizeof(content) - 1) == FILE_SAVE_DURABLE,
        "save through symlink");
    checkFile(path, content);
    check(lstat(link, &st) == 0 && S_ISLNK(st.st_mode), "symlink preserved");
    check(unlink(link) == 0, "remove symlink");
    check(symlink("missing-target", link) == 0, "create dangling relative symlink");
    struct editorDocument candidate;
    check(!fileioLoadDocument(link, &candidate) && errno == ENOENT &&
        !candidate.file.filename && !candidate.buffer.rows,
        "opening a dangling symlink reports an error without a candidate");
    errno = 0;
    check(fileioAtomicSave(link, content, sizeof(content) - 1) == FILE_SAVE_FAILED &&
        errno == ENOENT, "dangling symlink reports missing target");
    check(lstat(link, &st) == 0 && S_ISLNK(st.st_mode), "dangling symlink preserved");
    char destination[1024];
    ssize_t link_length = readlink(link, destination, sizeof(destination));
    check(link_length == 14 && memcmp(destination, "missing-target", 14) == 0,
        "dangling symlink destination unchanged");
    check(unlink(link) == 0, "remove dangling symlink");
    char missing[1024];
    snprintf(missing, sizeof(missing), "%s/missing-absolute-target", directory);
    check(symlink(missing, link) == 0, "create dangling absolute symlink");
    check(fileioAtomicSave(link, content, sizeof(content) - 1) == FILE_SAVE_FAILED &&
        errno == ENOENT, "absolute dangling symlink fails");
    check(lstat(link, &st) == 0 && S_ISLNK(st.st_mode) && access(missing, F_OK) == -1,
        "absolute link preserved without creating target");
    char chain[1024];
    snprintf(chain, sizeof(chain), "%s/chain", directory);
    check(symlink(link, chain) == 0, "create dangling symlink chain");
    check(fileioAtomicSave(chain, content, sizeof(content) - 1) == FILE_SAVE_FAILED &&
        errno == ENOENT && lstat(chain, &st) == 0 && S_ISLNK(st.st_mode),
        "dangling chain fails and remains a link");
    check(unlink(chain) == 0 && unlink(link) == 0, "remove dangling chain");
    check(fileioAtomicSave(link, content, sizeof(content) - 1) == FILE_SAVE_DURABLE,
        "ordinary new file remains supported");
    checkFile(link, content);
    check(unlink(link) == 0, "remove ordinary new file");
}

static void testRecoveryAndSettings(const char *directory, const char *path) {
    check(setenv("HOME", directory, 1) == 0, "isolated settings and backup home");
    struct editorSettings settings;
    settingsDefaults(&settings);
    check(settingsSave(&settings), "durable settings write");
    check(backupWrite(path, "old recovery", 12), "durable backup write");
    fail_directory_sync = 1;
    check(!settingsSave(&settings), "settings directory sync failure reported");
    check(!backupWrite(path, "new recovery", 12) && backupExists(path),
        "backup sync failure reported without deleting recovery data");
    fail_directory_sync = 0;
    size_t len;
    char *bytes = backupRead(path, &len);
    check(bytes && len == 12 && memcmp(bytes, "old recovery", len) == 0,
        "failed backup parent sync preserves existing recovery");
    free(bytes);
    check(backupWrite(path, "new recovery", 12), "backup retry succeeds");
    backupRemove(path);
    char cleanup[1024];
    snprintf(cleanup, sizeof(cleanup), "%s/.tinyeditrc", directory); unlink(cleanup);
    snprintf(cleanup, sizeof(cleanup), "%s/.tinyedit/backup", directory); rmdir(cleanup);
    snprintf(cleanup, sizeof(cleanup), "%s/.tinyedit", directory); rmdir(cleanup);
}

static void testHomePaths(const char *directory) {
    check(setenv("HOME", directory, 1) == 0, "set home for expansion");
    char expected[1024];
    snprintf(expected, sizeof(expected), "%s/tmp/caffè.md", directory);
    char *path = fileioExpandHomePath("~/tmp/caffè.md");
    check(path && strcmp(path, expected) == 0, "expand home and preserve UTF-8");
    free(path);
    path = fileioExpandHomePath("~");
    check(path && strcmp(path, directory) == 0, "expand bare home"); free(path);
    const char *literal[] = {"relative.md", "/tmp/file.md", "x/~/file", "~user/file", "$HOME/file"};
    for (size_t i = 0; i < sizeof(literal) / sizeof(literal[0]); i++) {
        path = fileioExpandHomePath(literal[i]);
        check(path && strcmp(path, literal[i]) == 0, "other paths stay literal"); free(path);
    }
    check(unsetenv("HOME") == 0, "unset isolated home");
    check(!fileioExpandHomePath("~/file") && errno == ENOENT, "missing home reported");
    check(setenv("HOME", "", 1) == 0, "empty home");
    check(!fileioExpandHomePath("~/file") && errno == ENOENT, "empty home reported");
    check(setenv("HOME", directory, 1) == 0, "restore isolated home");
}

int main(void) {
    char fifo_dir[] = "/tmp/tinyedit-fifo-XXXXXX";
    check(mkdtemp(fifo_dir) != NULL, "create FIFO directory");
    char fifo_path[256]; snprintf(fifo_path, sizeof(fifo_path), "%s/pipe", fifo_dir);
    check(mkfifo(fifo_path, 0600) == 0, "create FIFO");
    struct editorDocument fifo_candidate;
    check(!fileioLoadDocument(fifo_path, &fifo_candidate) && errno == EINVAL,
        "opening FIFO returns immediately without reader");
    unlink(fifo_path); rmdir(fifo_dir);
    char directory[] = "/tmp/tinyedit-fileio-XXXXXX";
    check(mkdtemp(directory) != NULL, "create test directory");
    char path[1024];
    snprintf(path, sizeof(path), "%s/document", directory);
    testHomePaths(directory);
    testLoading(path, directory);
    testSaving(path, directory);
    testRecoveryAndSettings(directory, path);
    check(unlink(path) == 0 && rmdir(directory) == 0, "remove fixtures");
    puts("fileio tests: ok");
    return 0;
}
