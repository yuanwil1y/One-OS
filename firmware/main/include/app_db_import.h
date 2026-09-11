#pragma once

/*
 * Device DB import: the state machine that replaces /nearby/db/devices.nbdb.
 *
 * This file is platform independent. Every filesystem operation arrives through
 * app_db_import_io_t, so the whole algorithm - including power-loss recovery - is
 * exercised on the host against a fake filesystem that can fail at any step.
 *
 * WHY A STATE MACHINE AND NOT A FUNCTION
 *
 * A streaming upload cannot be a single call: bytes arrive in chunks from HTTP,
 * and the interesting question is what state the card is in if power is lost
 * between any two of those chunks. Modelling it as explicit steps makes both
 * properties testable:
 *
 *   - the upload is bounded and resumable-in-the-sense-that-it-can-be-restarted,
 *     with the byte count checked exactly;
 *   - the replacement sequence is captured in one place, so "what if we die here"
 *     has an answer for every step rather than for the happy path only.
 *
 * THE REPLACEMENT SEQUENCE, AND WHY IT IS IN THIS ORDER
 *
 * FAT on this card does not give an atomic rename. `rename(db, bak)` followed by
 * `rename(part, db)` is therefore NOT a transaction, and there is a window in the
 * middle where `db` does not exist. Claiming power-loss safety from that sequence
 * would be wrong, so the sequence is chosen so that the window is recoverable and
 * the old database is never the thing that is lost:
 *
 *   1. write and validate  <db>.part        - the live database is untouched
 *   2. rename  <db>    -> <db>.bak          - the only moment `db` is absent
 *   3. rename  <db>.part -> <db>            - the new database takes the path
 *   4. unlink  <db>.bak                     - the previous copy is retired
 *
 * A failure at 1 or 2 leaves the old database in place: the import reports failure
 * and nothing is lost. A power loss between 2 and 3 leaves the old database in
 * <db>.bak, and app_db_import_recover() puts it back. A power loss between 3 and 4
 * leaves a valid new database at the path and a stale <db>.bak, which recovery
 * deletes after confirming the path is good. Every interruption therefore ends
 * with exactly one valid corpus at the path.
 *
 * The old project's "format the whole SD card" behaviour is deliberately absent:
 * three files are ever touched, all under the database directory.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_device_db.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Receive/write chunk. Fixed, because the body is never buffered whole. */
#define APP_DB_IMPORT_CHUNK 2048u

typedef enum {
    APP_DB_IMPORT_IDLE = 0,
    APP_DB_IMPORT_RECEIVING, /* accepting bytes into <db>.part */
    APP_DB_IMPORT_VALIDATING,
    APP_DB_IMPORT_REPLACING,
    APP_DB_IMPORT_DONE,
    APP_DB_IMPORT_FAILED,
    APP_DB_IMPORT_CANCELED,
} app_db_import_phase_t;

/* Stable reasons, reported to the browser and logged. Never contains secrets. */
typedef enum {
    APP_DB_IMPORT_ERR_NONE = 0,
    APP_DB_IMPORT_ERR_NO_MEDIUM,      /* no card: nothing can be imported */
    APP_DB_IMPORT_ERR_NO_SPACE,       /* the card cannot hold the declared size */
    APP_DB_IMPORT_ERR_TOO_LARGE,      /* above the format's own hard cap */
    APP_DB_IMPORT_ERR_IO,             /* a filesystem operation failed */
    APP_DB_IMPORT_ERR_SHORT_WRITE,    /* fewer bytes written than received */
    APP_DB_IMPORT_ERR_LENGTH_MISMATCH,/* received bytes != declared content length */
    APP_DB_IMPORT_ERR_INVALID_DB,     /* the uploaded bytes are not a usable corpus */
    APP_DB_IMPORT_ERR_BUSY,           /* another import is in flight */
    APP_DB_IMPORT_ERR_NOT_ACTIVE,     /* finish/cancel without a begin */
    APP_DB_IMPORT_ERR_CANCELED,
} app_db_import_error_t;

const char *app_db_import_phase_name(app_db_import_phase_t phase);
const char *app_db_import_error_name(app_db_import_error_t error);

/*
 * Filesystem seam.
 *
 * Every call returns ESP_OK on success. A short write or a read that returns
 * fewer bytes than asked MUST be reported as an error by the implementation:
 * "the card took less than we gave it" must never look like success.
 */
typedef struct {
    /* Is a writable card present and mounted? */
    bool (*medium_present)(void *ctx);
    /* Free bytes available for a new file, when the filesystem can report it.
     * Returning false means "unknown", which is treated as "do not refuse". */
    bool (*free_bytes)(void *ctx, uint64_t *out_free);
    /* Open `path` for writing, truncating any existing file. */
    esp_err_t (*write_open)(void *ctx, const char *path);
    esp_err_t (*write)(void *ctx, const void *data, size_t length);
    /* Flush and sync so the bytes are on the medium before anything is renamed. */
    esp_err_t (*write_sync)(void *ctx);
    void (*write_close)(void *ctx);
    /* Read the whole of `path` into `dst` (at most `capacity`). Sets *out_size. */
    esp_err_t (*read_all)(void *ctx, const char *path, uint8_t *dst,
                          uint32_t capacity, uint32_t *out_size);
    /*
     * Read exactly `length` bytes at `offset`. Used by the streaming body checksum,
     * so it is the one call that must be able to walk a file larger than any
     * buffer the importer holds.
     *
     * A short read MUST be reported as an error, never as success with fewer bytes.
     */
    esp_err_t (*read_range)(void *ctx, const char *path, uint32_t offset,
                            void *dst, uint32_t length);
    bool (*exists)(void *ctx, const char *path);
    /* Rename within the same filesystem. Must fail rather than silently copy. */
    esp_err_t (*rename)(void *ctx, const char *from, const char *to);
    esp_err_t (*unlink)(void *ctx, const char *path);
} app_db_import_io_t;

/*
 * Import handle.
 *
 * No heap: the paths and the validation staging buffer live here, so this can sit
 * in static storage next to the reader it coordinates with.
 */
#define APP_DB_IMPORT_PATH_MAX 128u

typedef struct {
    app_db_import_io_t io;
    void *io_ctx;

    app_db_import_phase_t phase;
    app_db_import_error_t error;
    esp_err_t last_io_error;

    char db_path[APP_DB_IMPORT_PATH_MAX];
    char part_path[APP_DB_IMPORT_PATH_MAX];
    char backup_path[APP_DB_IMPORT_PATH_MAX];

    uint64_t declared_length;   /* from Content-Length; 0 means unknown */
    uint64_t received;
    uint32_t max_file_bytes;    /* policy cap; 0 selects the format's own cap */

    /* Validation staging: the header only. The body is validated by the reader
     * when it opens the promoted file, so nothing large is ever held. */
    uint8_t header[APP_DB_HEADER_MAX];
    uint32_t content_version;
    uint32_t profile_count;

    /* Diagnostics. */
    bool wrote_any;
    bool backup_taken;
    uint32_t replace_attempts;
} app_db_import_t;

/*
 * Configure the handle.
 *
 * `db_path` is the absolute path of the corpus; the sibling names are derived from
 * it ("<db_path>.part", "<db_path>.bak"), so no other file on the card is touched.
 * Returns ESP_ERR_INVALID_ARG when a path would not fit, rather than truncating.
 */
esp_err_t app_db_import_init(app_db_import_t *imp, const app_db_import_io_t *io,
                             void *io_ctx, const char *db_path,
                             uint32_t max_file_bytes);

/*
 * Start an import.
 *
 * `declared_length` is the browser's Content-Length, or 0 when it did not send
 * one. When it is known it is enforced in both directions: a body that exceeds it
 * is refused, and finishing with fewer bytes than declared is a failure. When it
 * is unknown the size policy still bounds the transfer.
 *
 * Refuses with APP_DB_IMPORT_ERR_NO_MEDIUM when there is no card, and with
 * _NO_SPACE when the declared size cannot fit. Any pre-existing .part is removed
 * first: a leftover from an interrupted upload is not resumable state, it is
 * garbage, and keeping it would let a partial file be validated later.
 */
app_db_import_error_t app_db_import_begin(app_db_import_t *imp,
                                          uint64_t declared_length);

/*
 * Feed received bytes.
 *
 * Returns an error when the chunk would exceed the declared length or the policy
 * cap; the caller then calls app_db_import_cancel(), which removes the .part file.
 * The bytes are written before the call returns, so no ownership passes to this
 * function.
 */
app_db_import_error_t app_db_import_write(app_db_import_t *imp, const void *data,
                                          size_t length);

/*
 * Finish: validate, replace, and report.
 *
 * Validation is done on the .part file through the same header reader the runtime
 * uses (app_device_db_open over the import's own storage vtable), so an upload
 * cannot be accepted by a weaker check than the one that gates recognition.
 *
 * On success the phase is DONE and `*out_version` holds the corpus content
 * version. On any failure the phase is FAILED, `*out_error` says why, the old
 * database is still at the path, and the .part file has been removed.
 */
app_db_import_error_t app_db_import_finish(app_db_import_t *imp,
                                           uint32_t *out_version);

/* Abandon the import and remove the .part file. Never touches the live corpus. */
void app_db_import_cancel(app_db_import_t *imp);

/*
 * Bring the card back to exactly one valid corpus.
 *
 * Called at start-up and before an import. Handles every interruption point of
 * the sequence above:
 *
 *   - a valid corpus at the path: delete a stale .part and a stale .bak, then done;
 *   - no corpus at the path but a valid one in .bak: the process died between the
 *     two renames, so put it back;
 *   - no corpus at the path and no valid .bak, but a valid .part: the process died
 *     after validating but before the first rename, so promote it;
 *   - nothing valid anywhere: remove what is left and report NO_MEDIUM/FILE state,
 *     which the caller reports as "no corpus" rather than as a failure to recover.
 *
 * `*out_action` receives a short stable word describing what was done, for the log
 * and for diagnostics. Returns the phase the corpus is in afterwards.
 */
typedef enum {
    APP_DB_RECOVERY_NONE = 0,       /* nothing to do: the path was already valid */
    APP_DB_RECOVERY_RESTORED_OLD,   /* .bak put back after an interrupted replace */
    APP_DB_RECOVERY_PROMOTED_PART,  /* a validated .part was promoted */
    APP_DB_RECOVERY_DISCARDED,      /* stale/damaged files removed, no corpus */
    APP_DB_RECOVERY_NO_MEDIUM,      /* no card: nothing was examined */
} app_db_recovery_action_t;

const char *app_db_recovery_action_name(app_db_recovery_action_t action);

app_db_recovery_action_t app_db_import_recover(app_db_import_t *imp,
                                               app_db_state_t *out_state);

/* Does `path` hold a corpus this firmware can use? Sets the version when it does. */
bool app_db_import_probe(app_db_import_t *imp, const char *path,
                         uint32_t *out_version, uint32_t *out_profiles);

/*
 * Full validation: header gates AND the streamed body checksum.
 *
 * app_db_import_probe() reads only the header, which is what makes it cheap enough
 * for a recovery scan. This form additionally streams the body, so a file whose
 * payload was altered after its header was written is rejected - the case a
 * header-only check cannot see. It costs one full read of the file, so it runs at
 * the two points where correctness outweighs the time: before an upload is
 * promoted, and before an interrupted upload is promoted by recovery.
 */
bool app_db_import_verify(app_db_import_t *imp, const char *path,
                          uint32_t *out_version, uint32_t *out_profiles);

#ifdef __cplusplus
}
#endif
