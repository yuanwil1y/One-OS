#pragma once

/*
 * SD-backed recognizer: reads /nearby/db/devices.nbdb on demand and answers typed
 * match requests against it.
 *
 * Application infrastructure, not a Level-2 family and not a second recognition
 * engine. The SD database is the sole production corpus: there is no compiled-in
 * fallback matcher and no fixture is linked into the firmware.
 *
 * Bounded I/O is the point of this layer. The file is opened once; only the header
 * and the index buckets are kept in RAM. Every profile, fingerprint and recipe is
 * seeked to and read one record at a time into a fixed scratch buffer. The corpus
 * is never resident, which matters because the board has no PSRAM and the corpus
 * is expected to grow.
 *
 * Degradation is explicit: missing card, missing file, corrupt file, unsupported
 * version and I/O error are distinct states, and in all of them recognition is
 * skipped while observed devices stay visible as generic read-only Devices.
 *
 * The filesystem is not touched directly. The caller supplies a small read
 * interface, which is what makes this whole path testable on the host - including
 * short reads, I/O errors and a file that changes underneath the reader.
 *
 * Platform independent: no ESP-IDF/FreeRTOS/LVGL headers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_recognizer.h"
#include "app_scan.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on the header, so a hostile length cannot drive a big allocation. */
#define APP_DB_HEADER_MAX 256u
/* Largest index bucket array this firmware will hold in RAM. 1024 buckets is
 * 32 KiB and covers a corpus far larger than the C6 could otherwise support;
 * anything bigger is reported rather than silently accepted. */
#define APP_DB_MAX_INDEX_BYTES (1024u * DEVICE_DB_INDEX_BUCKET_SIZE)

/*
 * Storage the database reads through.
 *
 * A read that returns fewer bytes than requested must report
 * ESP_ERR_INVALID_SIZE rather than succeeding, so a truncated or shrinking file
 * cannot be mistaken for a valid record.
 */
typedef struct {
    /* Open the database file and report its size. */
    esp_err_t (*open)(void *ctx, uint32_t *out_size);
    /* Read exactly `length` bytes at `offset`. */
    esp_err_t (*read)(void *ctx, uint32_t offset, void *dst, uint32_t length);
    void (*close)(void *ctx);
    /* Does the medium itself exist? Distinguishes "no card" from "no file". */
    bool (*medium_present)(void *ctx);
} app_db_storage_ops_t;

/*
 * Recognition database handle.
 *
 * No heap allocation: the header, the record scratch and the index all live in
 * the struct, so this can sit in static storage.
 */
typedef struct {
    app_db_storage_ops_t storage;
    void *storage_ctx;

    app_db_state_t state;
    bool open;
    uint32_t file_size;

    /* Views built over `header` and `index`. */
    device_db_t db;
    uint8_t header[APP_DB_HEADER_MAX];
    uint32_t header_size;
    uint8_t record[DEVICE_DB_PROFILE_SIZE];

    uint8_t *index;
    uint32_t index_capacity;
    uint32_t index_size;

    /* Diagnostics. */
    uint32_t content_version;
    uint32_t profile_count;
    uint32_t reads;
    uint32_t read_errors;
    bool truncated_result;
    esp_err_t last_error;
} app_device_db_t;

/*
 * Recognizer vtable over an app_device_db_t. Pass the database as the context.
 * Safe to install while closed: recognise() then reports "not matched" and the
 * caller keeps its generic Device.
 */
struct app_recognizer_ops {
    bool (*recognize)(void *ctx, uint32_t sources,
                      const app_scan_wifi_t *wifi,
                      const app_scan_ble_t *ble,
                      const app_scan_lan_t *lan,
                      app_recognition_result_t *out);
    app_db_state_t (*state)(void *ctx);
    void (*describe)(void *ctx, char *out, size_t out_size);
};

const app_recognizer_ops_t *app_device_db_recognizer_ops(void);

/*
 * Open the database over the given storage.
 *
 * Never fails hard: on any problem the handle is left closed with `state` set to
 * the reason, and the caller continues without recognition. An index needing more
 * than `index_capacity` is reported as CORRUPT rather than silently truncated.
 */
void app_device_db_open(app_device_db_t *db,
                        const app_db_storage_ops_t *storage,
                        void *storage_ctx,
                        uint8_t *index_buffer,
                        uint32_t index_capacity);

void app_device_db_close(app_device_db_t *db);

/*
 * Match one observation.
 *
 * The caller supplies the protocol and its canonical key. Exactly one of the
 * evidence pointers is non-NULL. Returns true when a recognition result was
 * produced (matched, ambiguous or not found); false when recognition could not
 * run, in which case `*out` is left unmatched.
 *
 * Every record read is bounds-checked by device_db_open() having validated the
 * whole file first, and re-checked here against the live file size so a file that
 * shrank underneath the reader cannot produce a partial match.
 */
bool app_device_db_match(app_device_db_t *db, uint32_t sources,
                         uint8_t protocol,
                         const uint8_t *key, size_t key_length,
                         const app_scan_wifi_t *wifi,
                         const app_scan_ble_t *ble,
                         const app_scan_lan_t *lan,
                         app_recognition_result_t *out);

#ifdef __cplusplus
}
#endif
