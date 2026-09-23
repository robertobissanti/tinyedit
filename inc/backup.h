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

/* Fills `out` (a buffer of at least `outsize` bytes) with the absolute
 * backup path for `filename`, e.g. "/home/user/.tinyedit/backup/<hash>.swp".
 * `filename` need not exist yet (a brand new unsaved file still gets a
 * stable backup path derived from what its name would be). Returns 1 on
 * success, 0 if $HOME is unset/empty or the path would not fit `outsize`. */
uint8_t backupPathFor(const char *filename, char *out, size_t outsize);

/* Returns 1 if a backup file already exists for `filename` (i.e. a
 * previous session crashed or was killed before it could clean up),
 * 0 otherwise. Used at startup before deciding whether to offer
 * recovery. */
uint8_t backupExists(const char *filename);

/* Reads the backup for `filename` into a malloc'd NUL-terminated
 * buffer (the file content that was being edited, NOT including the
 * leading path line -- that's stripped here). *outlen receives its
 * length excluding the NUL terminator. Returns NULL on any failure
 * (missing/unreadable/malformed backup) or if `filename` has no
 * backup at all -- callers should treat NULL as "nothing to
 * recover", not necessarily an error worth reporting. Caller must
 * free() the result. */
char *backupRead(const char *filename, size_t *outlen);

/* Writes `content` (length `len`, may contain embedded newlines --
 * this is the raw joined buffer, same format editorRowsToString()
 * produces) as the crash-recovery backup for `filename`, preceded by
 * a line with `filename`'s absolute path for identification. Creates
 * ~/.tinyedit/backup/ if it doesn't exist yet. Returns 1 on success,
 * 0 on I/O failure (silently ignorable by the caller -- a failed
 * backup write is not worth interrupting editing over, unlike a
 * failed explicit Ctrl-S save). */
uint8_t backupWrite(const char *filename, const char *content, size_t len);

/* Deletes the backup for `filename`, if any. Called after a clean
 * save or on quitting with nothing unsaved, so a leftover .swp is
 * never mistaken for a crash on the next startup. Failure to remove
 * (e.g. already gone) is not an error. */
void backupRemove(const char *filename);

#endif /* __TE_BACKUP_H */
