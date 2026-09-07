# OpenThread Level-2 API implementation notes

Branch: `research/openthread-l2-api`  
Target: Waveshare ESP32-C6-Touch-LCD-1.9  
ESP-IDF baseline: v6.1  
Scope: approved Thread network layer only; no Matter application-layer behavior.

## Implemented API surface

The `firmware/components/openthread_l2` component implements:

- `openthread_discover_networks()`
- `openthread_get_state_snapshot()`
- `openthread_get_local_topology()`
- `openthread_attach_dataset()`
- `openthread_joiner_join()`

The public header contains only family-local plain C types. It does not expose `otInstance *`, OpenThread structs, ESP-IDF error/event types, Matter types, or other One-OS L2 family types.

## Lifecycle boundary

The component deliberately does not wrap `esp_openthread_start()`, `esp_openthread_stop()`, `esp_openthread_init()`, or create a generic runtime/session manager. Native ESP-IDF OpenThread lifecycle remains L1/application-owned.

All public APIs therefore require an already initialized ESP-IDF OpenThread instance. Calls that access OpenThread from an application task acquire the ESP-IDF OpenThread API lock internally.

## Discovery behavior

`openthread_discover_networks()` uses MLE Thread Discovery (`otThreadDiscover()`), not raw IEEE 802.15.4 active scan or promiscuous capture.

Implementation properties:

- fixed internal storage for at most 16 unique discovered Thread networks;
- no callback writes into caller memory;
- network-level deduplication by channel + PAN ID + Extended PAN ID + Network Name;
- strongest responder retained for duplicate networks;
- caller capacity is honored and `truncated` is explicit;
- finite timeout;
- because OpenThread has no general discovery cancel operation, timeout returns while the fixed internal context stays alive until native completion; another discovery/control request is `BUSY` until then;
- IPv6 temporarily enabled only for discovery is restored on native completion when Thread is still disabled.

The application remains responsible for serializing Thread discovery against BLE scan, Zigbee ownership and other cross-family RF operations.

## State snapshot

`openthread_get_state_snapshot()` copies a read-only, secret-redacted view of commissioned/IP6 state, role, attach duration, network identity, RLOC/partition/leader information, parent identity/RSSI and selected IPv6/MLE counters.

It intentionally has no Network Key, PSKc, PSKd or raw Operational Dataset field.

## Local topology

`openthread_get_local_topology()` iterates the local neighbor table. In FTD builds it also enriches/adds local router-table entries. It intentionally does not claim to be a complete remote mesh inventory. Optional metrics use explicit validity flags instead of invented zero values.

## Authorized dataset attach

`openthread_attach_dataset()` accepts caller-supplied authorized Operational Dataset TLVs, validates/parses them, persists them, enables IPv6/Thread and waits for an attached role with a finite timeout.

Rules:

- Thread must be disabled on entry;
- malformed/empty/>254-byte input is rejected;
- complete Active Dataset validation is the default; callers may explicitly allow a partial Active Dataset;
- credential-bearing temporary OpenThread dataset structures are zeroed before return;
- dataset bytes are never logged;
- a successfully written dataset remains persisted even if attach later times out;
- on attach failure/timeout, Thread is disabled again and IPv6 is restored only if this operation enabled it;
- a last-moment successful attachment wins over timeout cleanup.

## Thread Joiner

`openthread_joiner_join()` implements Thread MeshCoP Joiner commissioning with a caller-provided PSKd and then waits for normal Thread attach.

This is not Matter commissioning. The API implements no Matter setup passcode, PASE/CASE, Fabric, Node, Endpoint or Cluster semantics.

The Joiner PSKd is copied into a bounded temporary buffer, never logged and zeroed before return. Join/attach timeouts are finite. Failed operations restore temporary IPv6/Thread state without silently erasing successfully provisioned Thread settings.

## Build configuration

`sdkconfig.defaults` enables the native ESP-IDF OpenThread component, native ESP32-C6 802.15.4 radio, FTD support, platform netif and Joiner support. Border Router and Commissioner remain disabled. OpenThread CLI/console, SRP, DNS client and low-level diag are disabled because they are outside this approved slice.

FTD is retained so the approved local router/topology view is available. The application must continue conservative RF serialization on the single ESP32-C6 2.4 GHz radio.

## Tests

`tests/openthread_l2_host_test.c` and `tests/run_openthread_l2_host_test.sh` exercise:

- dataset length bounds;
- discovery deduplication key;
- strongest-result replacement;
- explicit truncation when fixed storage is full.

ESP32-C6 build validation is performed with the repository ESP-IDF v6.1 GitHub Actions build after the implementation commit.

## Deferred / excluded

Not implemented in this slice:

- Matter application semantics or controller operations;
- raw IEEE 802.15.4 scanner/parser/promiscuous capture;
- BLE/Wi-Fi scanning;
- Border Router product architecture;
- Commissioner/one-shot joiner authorization;
- remote Network Diagnostic Get;
- generic `nearby_*`, radio session or coordinator abstractions;
- HA Device/Entity materialization or recognition database logic.
