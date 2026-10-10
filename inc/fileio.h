#ifndef __TE_FILEIO_H
#define __TE_FILEIO_H

#include "tinyedit.h"

#include <stdio.h>

/* Replacement is irreversible once rename succeeds, even if directory sync
 * fails. Callers must preserve recovery data for the uncertain outcome. */
enum fileSaveResult {
    FILE_SAVE_FAILED,
    FILE_SAVE_UNCERTAIN,
    FILE_SAVE_DURABLE
};

/** @brief Expand leading ~ or ~/ using HOME; all other paths remain literal.
 * Returns owned text to free, or NULL with errno when HOME is unavailable
 * or the result length would overflow. No shell evaluation is performed. */
char *fileioExpandHomePath(const char *path);

/** @brief Read all rows into an empty candidate; no active editor state is changed.
 * On failure, returns 0 with errno and releases candidate-owned storage.
 * Only regular files are accepted; pipes/devices are rejected without blocking.
 * A missing ordinary path becomes an empty named document; dangling symlinks fail.
 * NUL-containing binary input is rejected with EILSEQ; malformed UTF-8 text is preserved. */
uint8_t fileioLoadDocument(const char *filename, struct editorDocument *candidate);
/** @brief Read a stream into an empty candidate, checking read errors separately
 * from EOF. NUL bytes are rejected with EILSEQ. The caller owns and closes
 * the stream; failure clears the buffer. */
uint8_t fileioLoadStream(FILE *stream, struct editorDocument *candidate);
/** @brief Append one LF-delimited line, removing only its actual LF/CRLF terminator.
 * Tracks ending metadata and preserves content CR bytes. Returns 0 on size limits. */
uint8_t fileioAppendLine(struct editorDocument *document, const char *line, size_t len);
/** @brief Sync directory entries after creating a directory or replacing a file.
 * Returns 1 on success, 0 with errno; retries interrupted synchronization. */
uint8_t fileioSyncDirectory(const char *directory);
/** @brief Replace a flushed temporary file and sync the containing directory.
 * FAILED leaves the target untouched; UNCERTAIN means rename succeeded.
 * The caller removes any remaining temporary file and preserves errno. */
enum fileSaveResult fileioReplace(const char *temporary, const char *target);
/** @brief Write beside the target, sync, replace and sync its directory. Existing
 * symlinks are followed and target permissions preserved; dangling symlinks fail.
 * Returns a precise replacement outcome; failures before replacement leave the target intact. */
enum fileSaveResult fileioAtomicSave(const char *filename, const char *bytes, size_t len);

#endif /* __TE_FILEIO_H */
