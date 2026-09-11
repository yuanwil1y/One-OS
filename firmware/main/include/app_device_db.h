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
/* Callback budget for a full body-checksum pass: one call per this many bytes.
 * The pass is read-bound (a 1 MiB corpus is 4096 blocks), so checking every 64
 * blocks keeps cancellation responsive without turning the hook into the cost. */
#define APP_DB_PROGRESS_INTERVAL_BYTES (64u * 256u)

/* A recipe's read source is "none" when the format's NO_INDEX sentinel is
 * stored. Named separately from the raw sentinel so a reader can tell "this
 * recipe publishes no protocol value" apart from "index 0xFFFFFFFF". */
#define APP_DB_NO_READ_SOURCE DEVICE_DB_NO_INDEX

/*
 * Storage the database reads through.
 *
 * A read that returns fewer bytes than requested must report
 * ESP_ERR_INVALID_SIZE rather than succeeding, so a truncated or shrinking file
 * cannot be mistaken for a valid record.
 *
 * FILE IDENTITY IS PART OF THE CONTRACT. `open` must return a handle whose reads
 * the adapter keeps bound to the same file object for every later `read` - on
 * FATFS that means a held `FILE *`, not a fresh open per read - so a file
 * replaced, renamed over or rewritten during a session cannot be observed as a
 * silent mixture of two corpora. The reader additionally re-checks the header
 * before each match attempt; see app_device_db.c.
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
 * Optional progress hook, polled during the bounded body-checksum pass.
 *
 * Returns true to abandon the pass, which closes the database with IO_ERROR
 * rather than publishing a partially validated corpus. Platform independent on
 * purpose: the runtime installs a closure over the operation gate's cancel flag,
 * and a host test installs one that fires after N calls.
 */
typedef bool (*app_db_progress_fn_t)(void *ctx);

/*
 * Recognition database handle.
 *
 * No heap allocation: the header, the record scratch and the index all live in
 * the struct, so this can sit in static storage. The index buffer is supplied by
 * the caller and is the only variable-size part.
 */
typedef struct {
    app_db_storage_ops_t storage;
    void *storage_ctx;

    app_db_state_t state;
    bool open;
    /*
     * True from the moment the storage is bound and a size is known until
     * close(). Distinct from `open`, which additionally means "fully validated and
     * safe to match against": the reads that perform validation need the file
     * before validation has finished.
     */
    bool opened;
    uint32_t file_size;

    /* Views built over `header` and `index`. */
    device_db_t db;
    uint8_t header[APP_DB_HEADER_MAX];
    uint32_t header_size;
    uint8_t record[DEVICE_DB_PROFILE_SIZE];

    uint8_t *index;
    uint32_t index_capacity;
    uint32_t index_size;

    /* Progress/cancellation hook for the streaming validation pass. */
    app_db_progress_fn_t progress;
    void *progress_ctx;
    uint32_t progress_interval_bytes;

    /* Diagnostics. */
    uint32_t content_version;
    uint32_t profile_count;
    uint32_t reads;
    uint32_t read_errors;
    uint32_t progress_calls;
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

/*
 * Install a progress/cancellation hook used by the bounded body-checksum pass.
 *
 * The hook is polled once per `interval_bytes` (0 selects
 * APP_DB_PROGRESS_INTERVAL_BYTES). Returning true abandons the pass and leaves
 * the database closed with IO_ERROR, so a cancelled scan does not spend seconds
 * hashing a corpus it will not use. Must be called before app_device_db_open();
 * open() clears it again, because a stale closure would outlive its owner.
 */
void app_device_db_set_progress(app_device_db_t *db, app_db_progress_fn_t progress,
                                void *ctx, uint32_t interval_bytes);

/*
 * Re-validate the corpus on the medium against what was validated at open.
 *
 * Two levels, and the difference matters for cost:
 *
 *   matching internally performs the CHEAP check - the header is re-read and
 *   compared before every match attempt, so a replaced, rewritten or truncated
 *   file cannot be matched against. Because every `.nbdb` header carries the
 *   content checksum of its own body, a different corpus cannot present the same
 *   header, so this catches replacement as well as a same-length rewrite.
 *
 *   THIS function additionally streams the whole body checksum, so it also
 *   catches an in-place edit that leaves the header untouched. That costs a full
 *   read of the corpus, which is why it is an explicit operation rather than
 *   something every match pays for on a card shared with the display.
 *
 * Returns false and closes the database (state IO_ERROR) when the medium no
 * longer matches. Not safe to call concurrently with a match: the application
 * worker owns the database.
 */
bool app_device_db_verify_unchanged(app_device_db_t *db);

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
