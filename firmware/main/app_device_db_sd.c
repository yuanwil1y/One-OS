/*
 * SD storage adapter for the recognition database.
 *
 * See app_device_db_sd.h for the contract. The three things this file is
 * responsible for:
 *
 *   - the path mapping. The product path is /nearby/db/devices.nbdb relative to
 *     the card, and this board mounts the card at /sdcard, so the absolute path is
 *     built once here from board facts rather than written as a literal anywhere
 *     else.
 *   - bringing the card up through board_sd_mount(), so the SPI2 bus, the chip
 *     select and the mount point stay board responsibilities. This adapter never
 *     creates, reconfigures or deletes a bus, and never unmounts a card somebody
 *     else mounted.
 *   - holding ONE file handle for the reader's lifetime. On FATFS a path is not an
 *     identity: opening per read would let a file replaced mid-scan deliver
 *     records from two different corpora, which is exactly the failure the
 *     reader's change detection exists to prevent. A held handle keeps every read
 *     bound to the object that was validated.
 *
 * Short reads are turned into ESP_ERR_INVALID_SIZE rather than reported as
 * success, because "the medium returned less than asked" must never look like a
 * valid record.
 *
 * Everything here is a POSIX/VFS file operation; nothing in this file makes a
 * product decision.
 */

#include "app_device_db_sd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "app_db_sd";

/* Build "<mount point>/<relative path>". Returns false when it would not fit, so
 * a truncated path can never be handed to fopen(). */
static bool build_path(const char *mount_point, const char *relative, char *out,
                       size_t out_size)
{
    int written;

    if (out == NULL || out_size == 0u) {
        return false;
    }
    out[0] = '\0';
    if (mount_point == NULL || relative == NULL) {
        return false;
    }

    written = snprintf(out, out_size, "%s/%s", mount_point, relative);
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool app_device_db_sd_path(const app_device_db_sd_t *sd, char *out, size_t out_size)
{
    (void)sd;
    return build_path(BOARD_SD_MOUNT_POINT, APP_DB_SD_RELATIVE_PATH, out, out_size);
}

/*
 * Is a card mounted and is FATFS actually serving it?
 *
 * Two independent facts, and both are needed:
 *
 *   - the driver's card pointer says a card was initialised and mounted;
 *   - esp_vfs_fat_info() on the mount point says the FATFS volume is registered
 *     and answering. It returns ESP_ERR_INVALID_STATE when the partition is not
 *     mounted, which is what a card that was never inserted - or was pulled -
 *     looks like.
 *
 * Neither alone is enough. A pulled card leaves the driver pointer set, and a
 * mount point registered in VFS is not evidence that a card is in the slot. Using
 * both is what makes "no card" distinguishable from "no corpus on the card", and
 * those are different instructions to the user.
 */
static bool fatfs_is_live(app_device_db_sd_t *sd)
{
    uint64_t total = 0u;
    uint64_t free_bytes = 0u;

    if (sd != NULL && sd->path[0] == '\0') {
        return false;
    }
    if (esp_vfs_fat_info(BOARD_SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return false;
    }
    if (sd != NULL) {
        sd->card_total_bytes = total;
    }
    return true;
}

bool app_device_db_sd_medium_present(app_device_db_sd_t *sd)
{
    if (sd == NULL) {
        return false;
    }
    sd->medium_present = (board_sd_card() != NULL) && fatfs_is_live(sd);
    return sd->medium_present;
}

void app_device_db_sd_init(app_device_db_sd_t *sd,
                           const app_device_db_sd_config_t *config)
{
    if (sd == NULL) {
        return;
    }
    memset(sd, 0, sizeof(*sd));
    if (!app_device_db_sd_path(sd, sd->path, sizeof(sd->path))) {
        /* Cannot even name the corpus: every open will report file_missing, which
         * is honest, and this log line says why. */
        ESP_LOGE(TAG, "database path does not fit in %u bytes",
                 (unsigned)sizeof(sd->path));
    }
    /*
     * Mounting is lazy: it happens in sd_open(). Bringing the card up at
     * initialisation would make a missing card look like a start-up failure, and
     * the product rule is that boot must not depend on the database.
     *
     * format_if_mount_failed is accepted but deliberately NOT honoured: a database
     * reader must never reformat a user's card, so no configuration can turn it
     * on by accident. The field exists so a caller has to state the intent and be
     * told it is refused.
     */
    if (config != NULL && config->format_if_mount_failed) {
        ESP_LOGW(TAG, "format_if_mount_failed is not honoured by the database "
                      "reader; the card is never formatted");
    }
}

/* ---------------- vtable ---------------- */

static esp_err_t sd_open(void *ctx, uint32_t *out_size)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;
    FILE *file;
    long size;

    if (sd == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* A previous handle must not leak if open() is called again. */
    if (sd->file != NULL) {
        fclose((FILE *)sd->file);
        sd->file = NULL;
    }

    if (board_sd_card() == NULL) {
        /* Not mounted. Mounting brings up the shared SPI2 bus through board code,
         * which is idempotent: the LCD may already be using it.
         *
         * format_if_mount_failed is always false. A database reader must never
         * reformat a user's card, so this is a property of the call site rather
         * than something a configuration could switch on by accident.
         */
        esp_err_t err = board_sd_mount(false);

        sd->mount_attempted = true;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
            return err;
        }
        sd->mounted_by_us = true;
    }

    if (sd->path[0] == '\0') {
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(sd->path, "rb");
    if (file == NULL) {
        /* ENOENT means the corpus is absent, which is a normal deployment state.
         * Anything else is a medium problem and is reported as such. */
        int saved = errno;

        ESP_LOGI(TAG, "database not opened: %s (errno=%d)", sd->path, saved);
        return (saved == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size <= 0) {
        fclose(file);
        /* An empty file is not a database. Reported as a size problem rather than
         * as a missing file, because the user's problem is the content. */
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    sd->file = file;
    *out_size = (uint32_t)size;
    return ESP_OK;
}

static esp_err_t sd_read(void *ctx, uint32_t offset, void *dst, uint32_t length)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;

    if (sd == NULL || sd->file == NULL || dst == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (length == 0u) {
        return ESP_OK;
    }
    /* fseek takes a long; a 32-bit offset cannot overflow it on this target, but
     * the cast is explicit so the intent is not left to the reader. */
    if (fseek((FILE *)sd->file, (long)offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(dst, 1u, length, (FILE *)sd->file) != (size_t)length) {
        /*
         * Short read. This is the case that matters: the card answered, but with
         * fewer bytes than the record needs. Reporting success here would let the
         * reader interpret stale buffer contents as record data, so it is a size
         * error and the reader fails the operation.
         */
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void sd_close(void *ctx)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;

    if (sd == NULL) {
        return;
    }
    if (sd->file != NULL) {
        fclose((FILE *)sd->file);
        sd->file = NULL;
    }
}

static bool sd_medium_present(void *ctx)
{
    return app_device_db_sd_medium_present((app_device_db_sd_t *)ctx);
}

app_db_storage_ops_t app_device_db_sd_ops(void)
{
    app_db_storage_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.open = sd_open;
    ops.read = sd_read;
    ops.close = sd_close;
    ops.medium_present = sd_medium_present;
    return ops;
}

void app_device_db_sd_release(app_device_db_sd_t *sd)
{
    if (sd == NULL) {
        return;
    }
    /* The reader owns the file handle and closes it through the vtable; this is
     * only a safety net for a handle left behind by a failure path. */
    if (sd->file != NULL) {
        fclose((FILE *)sd->file);
        sd->file = NULL;
    }

    /*
     * Unmount only a card this adapter mounted.
     *
     * A card that was already mounted belongs to whoever mounted it - today the
     * only candidate is this same adapter, but the rule keeps the adapter from
     * taking a resource it did not acquire, which is what makes it safe to call
     * this from anywhere.
     */
    if (sd->mounted_by_us) {
        esp_err_t err = board_sd_unmount();

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
        }
        sd->mounted_by_us = false;
    }
    sd->mount_attempted = false;
    sd->medium_present = false;
}

/* ---------------- import adapter ---------------- */

/*
 * The database import's filesystem seam, over the same card and the same rules.
 *
 * It is a separate vtable because the import's contract has one call the reader's does
 * not - a ranged read, for streaming a body checksum - and its own write side for the
 * ".part" file. It is NOT a second adapter: the corpus path, the mount and the "no card vs
 * no corpus" distinction all come from the same functions above, so there is one idea of
 * where the database lives.
 *
 * The write side keeps its OWN handle, distinct from the reader's. That is deliberate: the
 * reader's handle is closed before the renames, and the upload must be able to finish
 * writing and syncing its .part file regardless of what the reader is doing.
 */
static bool sd_import_medium_present(void *ctx)
{
    return app_device_db_sd_medium_present((app_device_db_sd_t *)ctx);
}

static esp_err_t sd_import_read_all(void *ctx, const char *path, uint8_t *dst,
                                    uint32_t capacity, uint32_t *out_size)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;
    FILE *file;
    long size;

    (void)path; /* the adapter's path is the corpus path; see the header */
    if (sd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sd->path[0] == '\0') {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Its own handle for the validation read, because the reader may be closed at this
     * point - and the probe and the verify must both work while it is.
     */
    file = fopen(sd->path, "rb");
    if (file == NULL) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if ((uint32_t)size < capacity) {
        /* Shorter than what the caller asked for. Reported as a size problem rather than
         * as a successful short read, so a truncated file cannot look usable. */
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fread(dst, 1u, capacity, file) != (size_t)capacity) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(file);
    *out_size = (uint32_t)size;
    return ESP_OK;
}

static esp_err_t sd_import_read_range(void *ctx, const char *path, uint32_t offset,
                                      void *dst, uint32_t length)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;
    uint8_t *bytes = (uint8_t *)dst;

    (void)path;
    if (sd == NULL || bytes == NULL || sd->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Opened once per ranged read sequence would be wasteful, so the import's own read
     * handle is kept open for the whole pass. That is safe precisely because the reader's
     * handle - the one the replace sequence closes - is a different object.
     */
    if (sd->import_read == NULL) {
        sd->import_read = fopen(sd->path, "rb");
        if (sd->import_read == NULL) {
            return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        }
    }
    if (fseek((FILE *)sd->import_read, (long)offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if (fread(bytes, 1u, length, (FILE *)sd->import_read) != (size_t)length) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t sd_import_write_open(void *ctx, const char *path)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;

    if (sd == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sd->import_write != NULL) {
        fclose((FILE *)sd->import_write);
        sd->import_write = NULL;
    }
    sd->import_write = fopen(path, "wb");
    if (sd->import_write == NULL) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t sd_import_write(void *ctx, const void *data, size_t length)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;

    if (sd == NULL || sd->import_write == NULL || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (length == 0u) {
        return ESP_OK;
    }
    /*
     * A short write is an error, never a success with fewer bytes: the caller counts
     * bytes to decide whether the upload is complete, and a silent short write would make
     * a truncated file look finished.
     */
    if (fwrite(data, 1u, length, (FILE *)sd->import_write) != length) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t sd_import_write_sync(void *ctx)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;

    if (sd == NULL || sd->import_write == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fflush((FILE *)sd->import_write) != 0) {
        return ESP_FAIL;
    }
    /*
     * fsync, not just fflush. The rename that follows must see the bytes on the medium:
     * a flush only moves them out of the C library's buffer, and a power loss between the
     * flush and the rename would leave a ".part" whose declared checksum describes bytes
     * that never landed.
     */
    if (fsync(fileno((FILE *)sd->import_write)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void sd_import_write_close(void *ctx)
{
    app_device_db_sd_t *sd = (app_device_db_sd_t *)ctx;

    if (sd == NULL) {
        return;
    }
    if (sd->import_write != NULL) {
        fclose((FILE *)sd->import_write);
        sd->import_write = NULL;
    }
    if (sd->import_read != NULL) {
        fclose((FILE *)sd->import_read);
        sd->import_read = NULL;
    }
}

static bool sd_import_exists(void *ctx, const char *path)
{
    FILE *file;

    (void)ctx;
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

static esp_err_t sd_import_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    if (from == NULL || to == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * One rename within one filesystem. It is NOT atomic across a power loss on FAT, which
     * is why app_db_import.c's sequence is built to be recoverable rather than to claim
     * atomicity - see app_db_import.h.
     */
    return rename(from, to) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t sd_import_unlink(void *ctx, const char *path)
{
    (void)ctx;
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
}

app_db_import_io_t app_device_db_sd_import_ops(void)
{
    app_db_import_io_t io;

    memset(&io, 0, sizeof(io));
    io.medium_present = sd_import_medium_present;
    io.write_open = sd_import_write_open;
    io.write = sd_import_write;
    io.write_sync = sd_import_write_sync;
    io.write_close = sd_import_write_close;
    io.read_all = sd_import_read_all;
    io.read_range = sd_import_read_range;
    io.exists = sd_import_exists;
    io.rename = sd_import_rename;
    io.unlink = sd_import_unlink;
    return io;
}
