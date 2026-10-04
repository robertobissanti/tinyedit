/* backup.h -- periodic crash-recovery backup for the file being edited,
 * written to ~/.tinyedit/backup/<hash>.swp (never next to the original
 * file, so it never pollutes the project directory or needs a .gitignore
 * entry).
 *
 * <hash> is derived from the edited file's absolute path (see
 * backupHashPath()), so a lookup at startup is a direct path build +
 * open(), never a directory scan -- important once many files have been
 * backed up over time. The first line of the .swp file itself carries
 * the absolute path in clear text, so a backup can be identified just by
 * opening it (e.g. for manual recovery, or debugging) even though the
 * filename alone is opaque.
 *
 * Lifecycle: backupWrite() is called periodically by the main loop
 * (see editorConfig's backup fields in tinyedit.h) while the buffer is
 * dirty; backupRemove() is called on a clean exit (after a successful
 * save, or when quitting with nothing to save) so a stale .swp never
 * outlives the session that wrote it -- its mere presence at the next
 * startup is exactly the crash signal backupCheck() looks for.
 */

#ifndef __TE_BACKUP_H
#define __TE_BACKUP_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Build the recovery filename associated with an edited file.
 *
 * @details out has outsize bytes.
 * @return 1 with a NUL-terminated path, or 0 if HOME, path resolution or
 * capacity prevents it; the document need not exist yet.
 */
uint8_t backupPathFor(const char *filename, char *out, size_t outsize);

/**
 * @brief Check whether a recovery file is present for a document.
 *
 * @details filename identifies the original file.
 * @return 1 when its backup path exists, otherwise 0; it does not validate
 * backup contents.
 */
uint8_t backupExists(const char *filename);

/**
 * @brief Read recoverable document bytes without the backup's path header.
 *
 * @details outlen may be NULL; otherwise receives the byte count excluding NUL
 * on success.
 * @return owned NUL-terminated text to free, or NULL on missing, malformed or
 * unreadable backup.
 */
char *backupRead(const char *filename, size_t *outlen);

/**
 * @brief Write document bytes to their recovery file.
 *
 * @details content contains len bytes. Creates the backup directory when
 * needed and prefixes the absolute document path.
 * @return 1 after file and directory sync, 0 on path or I/O failure.
 * A post-rename sync failure may have replaced the backup; it is retained.
 */
uint8_t backupWrite(const char *filename, const char *content, size_t len);

/**
 * @brief Remove a document's recovery file after save or clean departure.
 *
 * @details filename identifies the original document. Missing files and
 * removal failures are deliberately ignored.
 */
void backupRemove(const char *filename);

#endif /* __TE_BACKUP_H */
