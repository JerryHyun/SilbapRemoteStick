#pragma once
/* fsops: the single gate through which the web API and WebDAV change the SD card.
 * Every delete, overwrite, rename and move runs as: lock -> close_catalog_files -> change -> unlock -> changed,
 * so the USB side never reads through a stale open file (catalog ops.c re-stats on the next read and serves
 * zeros + cat_stale_flag when the file changed). Uploads stream into tmp/up-<8 hex> in 16 KiB chunks, each chunk
 * written under the lock (never held across a network receive), then are renamed into place.
 * All rel paths are wc_norm_path output (relative to root, "" = root); they are re-checked here. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fsops_hooks.h"
enum { FS_OK = 0, FS_EEXIST = -1, FS_ENOENT = -2, FS_ENOSPC = -3, FS_EINVAL = -4, FS_EIO = -5, FS_EINTO_SELF = -6 };
#define FS_CHUNK      (16 * 1024)        /* one SD write under the lock */
#define FS_HEADROOM   (1024 * 1024)      /* uploads keep this much free space */
#define FS_MAX_CONFLICT 999              /* "name (999).ext" is the last try */
/* root: mount point ("/sd"); tmp_dir: absolute folder for upload temp files (SILBAP_DIR "/tmp"). Strings are copied. */
void fs_init(const char *root, const char *tmp_dir, const fs_hooks_t *h);
/* Creates rel; its last segment passes wc_sd_safe_name first. FS_EEXIST if it exists, FS_ENOENT without a parent.
 * Does not close catalog files (nothing the disk serves is touched) but does request a refresh. */
int  fs_mkdir(const char *rel);
/* Renames rel inside its folder. new_name passes wc_sd_safe_name first (FS_EINVAL if rejected). FS_EEXIST if the
 * name is taken by another entry. A case-only change ("rose.jef" -> "Rose.jef") is a single rename: FatFs
 * accepts a new name that resolves to the same object (see fsops.c), so no temp name is ever left behind. */
int  fs_rename(const char *rel, const char *new_name);
/* Moves rel into dst_dir_rel under the same name. FS_EINTO_SELF if dst is rel or inside it, FS_EEXIST if the
 * name is taken there, FS_ENOENT if rel or the folder is missing (or dst is not a folder). Moving to the folder
 * rel is already in is a no-op. */
int  fs_move(const char *rel, const char *dst_dir_rel);
/* Deletes a file or a whole folder (bottom-up, one lock section per entry). removed_files (may be NULL) gets the
 * number of files deleted, also on a partial failure. No trash. */
int  fs_delete(const char *rel, uint32_t *removed_files);
/* Upload: begin -> write* -> commit | abort. size_hint from Content-Length (0 = unknown). begin checks the name
 * (FS_EINVAL), the folder (FS_ENOENT) and space: FS_ENOSPC when size_hint > free - 1 MiB (or free < 1 MiB). */
/* The space check calls hooks->free_bytes under the lock. In the firmware that is f_getfree, which counts the
 * whole FAT when FSINFO has no valid count and refresh_prepare_sd's boot warm-up failed: the first upload then
 * holds the lock for that scan (FatFs keeps the count afterwards). */
typedef struct fs_upload fs_upload_t;
int  fs_upload_begin(const char *dir_rel, const char *name, uint64_t size_hint, fs_upload_t **out);
/* Buffers to 16 KiB, writes each chunk under lock. After it returns an error the upload is dead: the caller must
 * call fs_upload_abort (commit would also clean up, but returns that error). */
int  fs_upload_write(fs_upload_t *u, const void *data, size_t len);
/* Finishes the upload: finds the first free name ("rose (2).jef" on a conflict, up to "(999)", else FS_EEXIST)
 * probing at most 16 names per lock hold, then lock -> close_catalog_files -> re-check -> rename -> utime ->
 * unlock -> changed. mtime 0 = hooks->now(). final_name gets the name used. Always consumes u: on failure the
 * temp file is removed. */
int  fs_upload_commit(fs_upload_t *u, int64_t mtime, char *final_name, size_t fn_sz);
/* WebDAV PUT/COPY: finishes the upload under the name given to begin, replacing a file of that name (also one that
 * differs only in case). One lock hold does close_catalog_files -> stat -> move the old file aside into tmp ->
 * rename the temp file into place -> utime -> remove the old copy, so no reader ever sees the name missing and a
 * failed rename puts the old file back. FS_EEXIST when a folder has that name. *replaced (may be NULL) says
 * whether a file was replaced. mtime 0 = hooks->now(). Always consumes u: on failure the temp file is removed. */
int  fs_upload_commit_replace(fs_upload_t *u, int64_t mtime, bool *replaced);
void fs_upload_abort(fs_upload_t *u);    /* closes and removes the temp file; NULL is fine */
/* How many web operations currently hold the volume open: an upload between begin and commit/abort, a delete,
 * rename, move or mkdir between its first and last step, or a WebDAV GET streaming an open FIL (enter/leave).
 * A coherency re-mount drops the FATFS object without flushing it, so it must never run while this is non-zero. */
uint32_t fs_inflight(void);
/* WebDAV GET: brackets the whole stream (open -> close), so a re-mount cannot invalidate the FIL mid-body. */
void fs_inflight_enter(void);
void fs_inflight_leave(void);
extern uint32_t fs_stat_probes;          /* diagnostics/tests: name probes made by fs_upload_commit */
void fs_cleanup_tmp(void);               /* boot: removes tmp/up-* (keeps catalog.new / names.new) */
/* Firmware only (main/fsops_esp.c): fs_init on SD_MOUNT and SILBAP_DIR "/tmp" with sdcard_lock/unlock,
 * refresh_close_data_files, refresh_request(REFRESH_CHANGED), esp_vfs_fat_info and timekeep_now. Call before the
 * SD card is mounted (refresh_prepare_sd runs fs_cleanup_tmp). */
void fs_esp_init(void);
