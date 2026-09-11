#pragma once

/*
 * SD storage adapter for the recognition database.
 *
 * This is the only place that connects the Device DB reader to a filesystem. It
 * owns three things and nothing else:
 *
 *   1. WHERE the corpus is. The product path is /nearby/db/devices.nbdb, which is
 *      relative to the card, so on this board it resolves to
 *      /sdcard/nearby/db/devices.nbdb. The mapping is one constant here rather
 *      than a literal scattered through the reader.
 *   2. HOW the card is brought up: board_sd_mount(), so the SPI2 bus, the chip
 *      select and the mount point stay board facts. The adapter never creates or
 *      deletes a bus, and never unmounts a card the application still uses.
 *   3. FILE IDENTITY. The database file is opened once and the handle is held for
 *      the lifetime of the reader. On FATFS a path is not an identity: reopening
 *      per read would let a file replaced or rewritten during a scan yield records
 *      from two different corpora. Holding one handle keeps every read bound to
 *      the object that was validated, and makes the reader's own change detection
 *      meaningful rather than best-effort.
 *
 * A short read is reported as ESP_ERR_INVALID_SIZE, never as success with fewer
 * bytes: "the medium returned less than asked" must not be mistakable for a valid
 * record.
 *
 * Platform dependent by design (ESP-IDF VFS + this board's SD pins). Everything
 * above it - the reader, the matcher, the policy - stays host-testable.
 */

#include <stdbool.h>
#include <stdint.h>

#include "app_device_db.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Path of the corpus relative to the SD card's root, as the product rules state. */
#define APP_DB_SD_RELATIVE_PATH "nearby/db/devices.nbdb"

typedef struct {
    /* Attempt to mount the card on open when it is not mounted yet. */
    bool auto_mount;
    /* Passed to board_sd_mount(). Never true in production: a database reader
     * must not reformat a user's card. Exposed so a factory or recovery build can
     * state its intent explicitly instead of relying on a hidden default. */
    bool format_if_mount_failed;
} app_device_db_sd_config_t;

/*
 * Adapter state. Holds no buffers: the reader owns the header, the index and the
 * record scratch, and the file handle is the adapter's only resource.
 */
typedef struct {
    void *file;             /* FILE *, held open for the reader's lifetime */
    bool mounted_by_us;     /* we mounted the card and may unmount it */
    bool mount_attempted;   /* a mount was tried; do not spin on a missing card */
    bool medium_present;    /* both the driver and FATFS last reported a card */
    uint64_t card_total_bytes; /* capacity, when FATFS reported it */
    char path[96];          /* absolute path of the corpus */
} app_device_db_sd_t;

/*
 * Storage vtable and context for the reader. The context is `sd`, which must
 * outlive the reader.
 *
 * The adapter is stateless until app_device_db_open() calls open(), so this can be
 * prepared at start-up and used later.
 */
app_db_storage_ops_t app_device_db_sd_ops(void);

void app_device_db_sd_init(app_device_db_sd_t *sd,
                           const app_device_db_sd_config_t *config);

/*
 * Absolute path of the corpus as this board mounts it, for diagnostics and for the
 * upload path in a later task. Returns false when the mount point plus the
 * relative path would not fit, rather than truncating silently.
 */
bool app_device_db_sd_path(const app_device_db_sd_t *sd, char *out, size_t out_size);

/*
 * Release the card if this adapter mounted it and nothing else is using it.
 *
 * Called after the reader is closed. Safe to call when nothing was mounted. The
 * adapter deliberately does NOT unmount a card that was already mounted when it
 * started: whoever mounted it owns it.
 */
void app_device_db_sd_release(app_device_db_sd_t *sd);

/*
 * Is a card available right now?
 *
 * Drives the difference between "no card" and "no corpus on the card", which are
 * different instructions to the user. Cheap and side-effect free after the first
 * successful mount; a failed mount is retried by the next open() rather than here.
 */
bool app_device_db_sd_medium_present(app_device_db_sd_t *sd);

#ifdef __cplusplus
}
#endif
