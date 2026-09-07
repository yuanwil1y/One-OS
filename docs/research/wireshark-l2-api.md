# Wireshark-derived Level-2 API — Phase 1 research

Status: **research complete; no production implementation started**  
Target: Waveshare ESP32-C6-Touch-LCD-1.9 / ESP-IDF + FreeRTOS  
Wireshark reference snapshot: `wireshark/wireshark` GitHub mirror `master` at `cc2f545b2360075c2bf6f19c78e81c77c429399f` (inspected 2026-09-07)

## 1. Constraints and conclusions

This research follows the repository foundation rules and `AGENTS.md` boundary:

- L1 remains ESP-IDF/NimBLE/lwIP/IEEE 802.15.4 capture/network/radio APIs.
- Wireshark-derived L2 APIs only turn bounded raw protocol bytes into reusable protocol semantics.
- No GUI, display-filter VM, plugin runtime, `tvbuff_t`/`proto_tree`, reassembly framework, dissector registry, preference system, conversation tracking, crypto/key database, or full Wireshark dissector universe is portable to this target.
- No public Wireshark-family API may expose ESP-IDF packet structs or another project API family's types.
- No dynamic allocation is needed for the recommended first set. Results should be caller-owned or fixed-capacity and may use byte offsets/spans into the caller's immutable input.
- Wireshark source is GPL-2.0-or-later. The production recommendation is therefore **not to copy or port Wireshark source**. Use it as field-truth/test-oracle reference, then implement the approved parser behavior from public protocol standards and assigned-number registries as a **CLEAN-ROOM REIMPLEMENT**.

The recommended small production set is:

1. `wireshark_ble_ad_parse()` — BLE GAP Advertising Data / EIR-style AD structures.
2. `wireshark_wifi_mgmt_parse()` — selected 802.11 management header/fixed fields and discovery-relevant information elements.
3. `wireshark_ieee802154_mac_parse()` — bounded IEEE 802.15.4 MAC header/address/security-header boundary parsing.
4. `wireshark_dns_message_parse()` — bounded DNS parser with an mDNS/DNS-SD useful subset (PTR/SRV/TXT/A/AAAA).

Everything else below is deferred or reference-only until an application demonstrates demand.

## 2. Repository and capture-path evidence

The current One-OS repository is intentionally a clean foundation. Its Git history begins with `4bf04f1d8a926c1fdc26b1ffa0115c089990a7d7` (`chore: bootstrap clean One-OS foundation`) and does not contain the old scanner/NearBy parser tree. Therefore there is no reliable legacy Wireshark/NearBy parser implementation in this repository to port or compare line-by-line. This is an evidence gap, not permission to reconstruct old architecture from memory.

The archived beta smoke application does confirm native radio paths that matter to this research:

- BLE: NimBLE passive discovery (`BLE_GAP_EVENT_DISC`, and extended discovery when enabled).
- IEEE 802.15.4: `esp_ieee802154` promiscuous receive over channels 11–26 with an `rx_done` raw-frame callback.
- Wi-Fi: native ESP-IDF active AP scan was validated, but the smoke app did **not** validate a raw 802.11 management-frame capture pipeline.

Current `sdkconfig.defaults` enables NimBLE, extended advertising support and IEEE 802.15.4, with no PSRAM assumed. This makes fixed-size, zero-allocation parsers materially preferable to desktop-style dissection.

## 3. Proposed common parse contract

This is an API-shape proposal only, not production code.

```c
typedef enum {
    WIRESHARK_PARSE_OK = 0,
    WIRESHARK_PARSE_TRUNCATED,
    WIRESHARK_PARSE_MALFORMED,
    WIRESHARK_PARSE_UNSUPPORTED,
    WIRESHARK_PARSE_OUTPUT_FULL,
} wireshark_parse_status_t;

typedef struct {
    wireshark_parse_status_t status;
    size_t consumed;
    size_t error_offset;
} wireshark_parse_diag_t;

typedef struct {
    uint16_t offset;
    uint16_t length;
} wireshark_span_t;
```

Rules:

- Parsers never read past `len` and do not mutate input.
- Valid fields decoded before an error remain available in the result; `diag.status` states why parsing stopped.
- `TRUNCATED` means a syntactically plausible field declared bytes that are not present.
- `MALFORMED` means the bytes are present but violate a protocol constraint.
- `UNSUPPORTED` means an otherwise valid construct is deliberately outside the bounded subset.
- `OUTPUT_FULL` means the input is valid but a fixed repeated-field/result capacity was reached; parsing may continue only far enough to validate safely, without writing beyond capacity.
- Offsets/spans are preferred to copied arbitrary payloads. Strings are copied only when wire compression or canonicalization requires it (notably DNS names).
- These small utility types are internal to the `wireshark_*` family and are not a generic packet framework.

## 4. Candidate A — BLE Advertising Data (recommended P0 L2 API)

### Upstream source and semantics

Primary Wireshark field-truth sources:

- `epan/dissectors/packet-bthci_cmd.c`: Bluetooth common EIR/AD parsing and `btcommon.eir_ad.ad` / `.eir` dissector registration.
- `epan/dissectors/packet-bluetooth.c`: downstream AD-type/company-ID/service-UUID payload dissectors and examples such as iBeacon / Matter advertisements.
- `epan/dissectors/packet-btle.c`: BLE Link Layer context; **not** needed for the proposed AD parser.
- Bluetooth Core Specification, GAP data types, and Bluetooth Assigned Numbers should be the normative clean-room implementation references.

AD data is a sequence of structures: one length octet, one AD type octet, then `length - 1` value octets. Useful generic discovery semantics include:

- Flags (`0x01`).
- 16/32/128-bit service UUID lists (`0x02`–`0x07`).
- Shortened / Complete Local Name (`0x08` / `0x09`).
- Tx Power Level (`0x0A`).
- Appearance (`0x19`).
- Service Data (`0x16`, `0x20`, `0x21`).
- URI (`0x24`) if demand appears.
- Manufacturer Specific Data (`0xFF`): first two value bytes are Bluetooth Company Identifier, followed by opaque vendor payload.

Multi-octet Bluetooth UUID/company values in AD payloads are little-endian on the wire; arbitrary manufacturer/service payloads remain opaque unless a separately approved parser owns their semantics.

### Why One-OS needs it / why it is L2

NimBLE reports give advertising payload bytes, but applications repeatedly need the semantic contents: local name, advertised services, company ID, service-data selectors and Tx power. This is substantial reusable parsing above L1 discovery and is not a native-API renaming wrapper.

### Proposed API/data shape

```c
#define WIRESHARK_BLE_MAX_UUID16       12
#define WIRESHARK_BLE_MAX_UUID32        4
#define WIRESHARK_BLE_MAX_UUID128       4
#define WIRESHARK_BLE_MAX_MFG_DATA      4
#define WIRESHARK_BLE_MAX_SERVICE_DATA  6
#define WIRESHARK_BLE_MAX_UNKNOWN       8

typedef struct {
    uint8_t type;
    wireshark_span_t value;
} wireshark_ble_ad_item_t;

typedef struct {
    wireshark_parse_diag_t diag;
    bool flags_present;
    uint8_t flags;
    bool name_present;
    bool name_complete;
    wireshark_span_t name;
    bool tx_power_present;
    int8_t tx_power_dbm;
    bool appearance_present;
    uint16_t appearance;
    /* fixed arrays/counts for UUIDs, manufacturer/service-data, unknown items */
} wireshark_ble_ad_t;

wireshark_parse_status_t
wireshark_ble_ad_parse(const uint8_t *data, size_t len, wireshark_ble_ad_t *out);
```

The exact capacities are Phase-2 tunables; they must be compile-time constants or caller-supplied capacities, not heap allocations.

### Required input

- Raw GAP AD payload bytes and exact byte length from a NimBLE discovery report.
- No public dependency on `struct ble_gap_event`.
- RSSI, peer address, PHY, SID and legacy/extended-report metadata remain L1/application context; the parser does not invent them from AD bytes.

### Malformed/truncated behavior

- Zero-length structure: stop cleanly as padding/terminator; do not read a type byte.
- Length 1: valid type with zero-length value unless that specific type requires more bytes.
- Declared structure extends past input: return `TRUNCATED` at that structure; retain earlier decoded fields.
- Manufacturer data shorter than 2 value bytes: `MALFORMED` for the typed semantic, while the raw item span may remain observable.
- UUID-list value whose size is not a multiple of UUID width: `MALFORMED` after complete entries.
- Duplicate singleton-ish fields: deterministic policy should preserve the first valid instance and still expose later raw items if capacity permits; do not silently merge contradictory names/flags.

### Bounded cost estimate

Planning estimate, to be measured with `idf.py size-components` in Phase 2:

- Flash/code: ~3–6 KiB for generic AD types above.
- Result RAM: ~250–500 B depending configured repeated-field capacities.
- Parser stack: target <160 B.
- Heap: 0 B.

### Provenance / disposition

- Wireshark source: `REFERENCE-ONLY`.
- Production implementation: `CLEAN-ROOM REIMPLEMENT` from Bluetooth specification/assigned numbers.
- Disposition: **L2 API / P0**.
- Device-specific payload decoders such as iBeacon, Matter, Apple Continuity, vendor telemetry: **APP or other approved family**, not automatically part of this API.

### Vectors/fuzz cases

- Flags + complete name + UUID16 list + manufacturer data.
- Scan response containing only complete name.
- Extended-advertising payload longer than legacy 31 bytes.
- Zero-length terminator followed by garbage.
- Declared length larger than remaining bytes at every possible offset.
- Manufacturer data lengths 0/1/2.
- UUID16/32/128 lists with valid and off-by-one lengths.
- Duplicate names, flags and manufacturer blocks.
- Repeated unknown types until result capacity is reached.

## 5. Candidate B — 802.11 management frame / selected IEs (recommended P0 L2 API)

### Upstream source and semantics

Primary field-truth source:

- `epan/dissectors/packet-ieee80211.c`.
- IEEE 802.11-2020+ should be the normative clean-room implementation source.

Useful bounded discovery fields:

- Management frame control/subtype and address fields.
- Beacon/probe response fixed fields: timestamp, beacon interval, capability information.
- Information Elements (IEs): `id`, `length`, `value`.
- SSID IE: 0–32 octets; zero length is the broadcast/hidden-SSID form. Treat SSID as raw bytes plus length, not an assumed UTF-8 C string.
- Supported Rates and Extended Supported Rates: each octet contains the rate value plus the basic-rate flag.
- DS Parameter Set: one-byte current channel when present; a non-1-byte value is malformed for this element.
- RSN IE: group cipher suite, pairwise suite list, AKM list and capability bits, all strictly length-bounded.
- Capability-presence summaries for HT/VHT/HE may be useful later, but deep PHY capability trees should not be in the initial parser.
- Vendor-specific IE (`221`): expose OUI/type and bounded raw span only in the initial API; do not port the vendor dissector universe.

802.11 frame control and common fixed fields are little-endian; MAC addresses are six opaque octets. IE length is one octet. RSN count/capability integers are little-endian, while cipher/AKM suite selectors are OUI/type byte sequences.

### Why One-OS needs it / why it is L2

ESP-IDF active scan already returns useful AP summaries and should remain directly usable. A Wireshark-derived L2 parser is justified only when an application captures raw management frames and needs semantics absent from the native scan record: exact IE presence/order, RSN suites, vendor OUI spans, raw SSID representation, and management subtype/fixed fields.

This distinction prevents an unnecessary wrapper around `wifi_ap_record_t`.

### Proposed API/data shape

```c
#define WIRESHARK_WIFI_MAX_RATES       16
#define WIRESHARK_WIFI_MAX_PAIRWISE     8
#define WIRESHARK_WIFI_MAX_AKM          8
#define WIRESHARK_WIFI_MAX_VENDOR_IE    8
#define WIRESHARK_WIFI_MAX_UNKNOWN_IE  12

typedef struct {
    wireshark_parse_diag_t diag;
    uint8_t subtype;
    uint8_t addr1[6], addr2[6], addr3[6];
    bool ssid_present;
    uint8_t ssid_len;
    uint8_t ssid[32];
    bool ds_channel_present;
    uint8_t ds_channel;
    /* supported rates, capability flags, bounded RSN summary, vendor/unknown spans */
} wireshark_wifi_mgmt_t;

wireshark_parse_status_t
wireshark_wifi_mgmt_parse(const uint8_t *frame, size_t len,
                          wireshark_wifi_mgmt_t *out);
```

A second internal/helper entry point may parse an IE block once a validated subtype-specific fixed header yields the IE offset; it should not become a generic TLV framework exposed across API families.

### Required input

- Raw IEEE 802.11 management frame bytes beginning at Frame Control and exact length.
- The L1 capture adapter must strip/identify any ESP-IDF-specific wrapper before calling this parser.
- RSSI, channel and timestamp from the radio driver remain native metadata outside the public `wireshark_*` structure.

### Malformed/truncated behavior

- Less than minimum management header: `TRUNCATED`.
- Subtype whose fixed body is outside the approved subset: validate header and return `UNSUPPORTED` rather than guessing an IE offset.
- One-byte IE tail (ID without length): `TRUNCATED`.
- IE length past remaining input: `TRUNCATED`.
- SSID length >32: `MALFORMED`; do not copy beyond 32.
- DS Parameter length !=1: `MALFORMED` for that element.
- RSN nested count arrays that exceed enclosing IE: `TRUNCATED`/`MALFORMED` at the exact nested field.
- Repeated or unknown IEs: preserve bounded descriptors; `OUTPUT_FULL` if configured capacity is exceeded.

### Bounded cost estimate

- Flash/code: ~7–14 KiB with SSID/rates/DS/RSN and shallow vendor handling.
- Result RAM: ~350–700 B depending RSN/vendor capacities.
- Parser stack: target <256 B.
- Heap: 0 B.

### Provenance / disposition

- Wireshark source: `REFERENCE-ONLY`.
- Production implementation: `CLEAN-ROOM REIMPLEMENT` from IEEE 802.11 definitions and public registries.
- Disposition: **L2 API / P0**, conditional on an application supplying raw management frames.
- Deep WPS/WFA/vendor-specific dissectors: **REFERENCE-ONLY** initially.

### Vectors/fuzz cases

- Probe Request with zero-length SSID.
- Beacon with SSID, Supported Rates, DS channel and Extended Rates.
- Hidden SSID beacon.
- SSID length 32 and invalid 33.
- One-byte trailing IE header.
- IE length extending one byte / many bytes past frame.
- RSN with zero, one and multiple pairwise/AKM suites; nested count overflow/truncation.
- Repeated SSID/RSN/vendor IEs.
- Unknown IE IDs and maximum repeated descriptors.
- Random bit flips in frame control and IE length bytes.

## 6. Candidate C — IEEE 802.15.4 MAC (recommended P0 L2 API)

### Upstream source and semantics

Primary sources:

- `epan/dissectors/packet-ieee802154.c`.
- `epan/dissectors/packet-ieee802154.h`.
- IEEE 802.15.4 specification should be normative for clean-room implementation.

Wireshark explicitly notes that IEEE 802.15.4 fields are little-endian. The common MAC format contains FCF, optional sequence number, address fields, optional auxiliary security header, payload and optional FCS. The header defines:

- Frame types: Beacon, Data, ACK, MAC Command, Multipurpose, Fragment, Extended.
- FCF flags: security enabled, frame pending, ACK request, PAN ID compression, sequence-number suppression, IE present.
- Destination/source address modes: none, reserved, short, extended.
- Frame versions: 2003, 2006, 2015, reserved.
- Auxiliary Security Control: security level, key-ID mode, frame-counter suppression and ASN-in-nonce flag.
- 127-byte classic maximum MAC frame length and 2-byte FCS baseline, while Wireshark also supports capture variants with 4-byte/no FCS.

Address/PAN-field presence is not a fixed struct: it depends on frame version, address modes, PAN compression and multipurpose-frame rules. That decision table is the semantic core worth reimplementing carefully.

### Why One-OS needs it / why it is L2

The board already receives raw 802.15.4 frames through the native `esp_ieee802154` callback. Converting FCF/address/security-header structure into payload boundaries and canonical identifiers is reusable L2 work required before an application can decide whether payload is Zigbee, Thread or something else.

It must **not** decrypt payloads or dispatch to a Zigbee/Thread family.

### Proposed API/data shape

```c
typedef enum {
    WIRESHARK_I154_ADDR_NONE = 0,
    WIRESHARK_I154_ADDR_SHORT,
    WIRESHARK_I154_ADDR_EXTENDED,
} wireshark_ieee802154_addr_kind_t;

typedef struct {
    wireshark_parse_diag_t diag;
    uint8_t frame_type;
    uint8_t frame_version;
    bool security_enabled;
    bool ack_requested;
    bool pan_id_compression;
    bool sequence_present;
    uint8_t sequence;
    /* destination/source PAN + short/EUI-64 address when present */
    /* auxiliary-security fields when present */
    wireshark_span_t payload;
} wireshark_ieee802154_mac_t;

wireshark_parse_status_t
wireshark_ieee802154_mac_parse(const uint8_t *mac, size_t len,
                               wireshark_ieee802154_mac_t *out);
```

The API should take the MAC bytes and length, not `esp_ieee802154_frame_info_t`. L1 metadata such as RSSI/LQI/channel stays with the capture owner.

### Required input

- MAC frame bytes and exact MAC length after the L1 adapter has handled any ESP-IDF receive-buffer prefix/trailer convention.
- Optional FCS presence should be an explicit parser option/argument if the capture path cannot guarantee one format; never auto-guess from arbitrary trailing bytes.

### Malformed/truncated behavior

- FCF shorter than required: `TRUNCATED`.
- Reserved frame version/address mode: `UNSUPPORTED` or `MALFORMED` according to the specification rule.
- Missing sequence number when FCF says it is present: `TRUNCATED`.
- Address/PAN fields shorter than the presence decision requires: `TRUNCATED`.
- Auxiliary security header shorter than its key-ID mode requires: `TRUNCATED`.
- Header-IE/payload-IE details can initially be `UNSUPPORTED` while still returning a validated payload/header boundary when possible.
- No decryption, MIC validation, key lookup or attack-oriented behavior.

### Bounded cost estimate

- Flash/code: ~4–8 KiB for common/2015 address and auxiliary-security-header parsing.
- Result RAM: <160 B.
- Parser stack: target <160 B.
- Heap: 0 B.

### Provenance / disposition

- Wireshark source: `REFERENCE-ONLY`.
- Production implementation: `CLEAN-ROOM REIMPLEMENT` from IEEE 802.15.4.
- Disposition: **L2 API / P0**.
- Zigbee/Thread payload dispatch: **not this API**.

### Vectors/fuzz cases

- Beacon/data/ACK/MAC-command examples.
- none/short/extended source and destination combinations.
- 2003/2006 versus 2015 PAN-compression cases.
- Sequence-number suppression.
- Auxiliary security levels and key-ID modes 0–3.
- FCF truncated at 0/1 byte.
- Reserved address mode/version.
- Every address/security field truncated one byte early.
- Maximum classic 127-byte frame and minimum legal ACK.
- Explicit no-FCS / 2-byte-FCS options.

## 7. Candidate D — DNS + mDNS/DNS-SD subset (recommended P1 L2 API)

### Upstream source and semantics

Primary source:

- `epan/dissectors/packet-dns.c` (DNS plus multicast-DNS-aware semantics).
- Normative references: RFC 1034/1035, RFC 6762 (mDNS), RFC 6763 (DNS-SD).

Required discovery subset:

- DNS header: ID, flags, QD/AN/NS/AR counts.
- Questions: compressed domain name, QTYPE, QCLASS; preserve the mDNS unicast-response (QU) class bit.
- Resource records: owner name, TYPE, CLASS, TTL, RDLENGTH; preserve the mDNS cache-flush class bit.
- Decode only high-value RDATA initially: A, AAAA, PTR, SRV and TXT.
- Unknown RR types are safely skipped by `RDLENGTH` and retained as a span if capacity permits.
- DNS name compression uses labels and pointer references. A bounded parser must detect loops, out-of-range pointers, excessive pointer indirection, label length >63 and expanded name >255 octets.
- DNS integers are network byte order (big-endian).

### Why One-OS needs it / why it is L2

mDNS/DNS-SD is a high-value joined-LAN device/service discovery source. lwIP gives UDP sockets but not a reusable structured DNS-SD record graph. A bounded parser that safely extracts PTR/SRV/TXT/A/AAAA semantics is meaningful L2 and can be used by multiple applications without building a desktop DNS engine.

### Proposed API/data shape

```c
#define WIRESHARK_DNS_MAX_QUESTIONS  4
#define WIRESHARK_DNS_MAX_RR        16
#define WIRESHARK_DNS_NAME_MAX     256
#define WIRESHARK_DNS_TXT_ITEMS      8

typedef struct {
    uint16_t type;
    uint16_t class_code;
    uint32_t ttl;
    char name[WIRESHARK_DNS_NAME_MAX];
    /* union: A, AAAA, PTR name, SRV priority/weight/port/target, bounded TXT */
} wireshark_dns_rr_t;

typedef struct {
    wireshark_parse_diag_t diag;
    uint16_t id;
    uint16_t flags;
    /* fixed-capacity questions and RRs */
} wireshark_dns_message_t;

wireshark_parse_status_t
wireshark_dns_message_parse(const uint8_t *message, size_t len,
                            bool mdns_semantics,
                            wireshark_dns_message_t *out);
```

Capacities should be reviewed against real mDNS captures; if the struct is too large, Phase 2 should switch the repeated results to caller-provided arrays while retaining zero allocation.

### Required input

- UDP DNS payload bytes only.
- Source/destination IP/port stay with native socket/capture metadata. The caller decides whether UDP/5353 implies mDNS semantics.

### Malformed/truncated behavior

- Header <12 bytes: `TRUNCATED`.
- Any section count that cannot be satisfied by remaining bytes: stop at the first incomplete record.
- Name pointer out of message: `MALFORMED`.
- Name pointer loop or pointer depth above a small fixed limit (for example 16): `MALFORMED`.
- Label >63 or expanded name >255: `MALFORMED`.
- `RDLENGTH` past remaining bytes: `TRUNCATED`.
- SRV shorter than priority+weight+port+target minimum: `MALFORMED`/`TRUNCATED` as appropriate.
- TXT element length beyond RDATA: `MALFORMED`.
- Output record capacity reached: `OUTPUT_FULL`; never recurse/allocate to retain everything.

### Bounded cost estimate

- Flash/code: ~9–18 KiB, dominated by safe compressed-name decoding and RR subset.
- Result RAM: ~0.8–2 KiB depending name/RR capacities; should be caller-provided if measurements are high.
- Parser stack: target <256 B; name decoding should be iterative or strictly depth-bounded.
- Heap: 0 B.

### Provenance / disposition

- Wireshark source: `REFERENCE-ONLY`.
- Production implementation: `CLEAN-ROOM REIMPLEMENT` from RFCs/IANA registries.
- Disposition: **L2 API / P1**, after P0 radio-nearby parsers.

### Vectors/fuzz cases

- mDNS PTR -> SRV + TXT + A/AAAA answer chain.
- Compressed names using one/multiple pointers.
- Self-pointer and two-node pointer loop.
- Pointer outside message.
- Label length 63/64; expanded name 255/256.
- Counts larger than payload can contain.
- Unknown RR safely skipped.
- SRV/TXT exactly minimum size and one byte short.
- mDNS QU/cache-flush bits.
- Record-capacity exhaustion without memory corruption.

## 8. Candidate E — SSDP (defer; app/reference)

### Upstream source and semantics

Wireshark does not use a standalone `packet-ssdp.c`; SSDP is handled inside `epan/dissectors/packet-http.c`, explicitly described there as Simple Service Discovery Protocol implemented atop HTTP over UDP.

Discovery-relevant semantics are textual request/status lines and headers such as `HOST`, `MAN`, `MX`, `ST`, `NT`, `NTS`, `USN`, `LOCATION`, `SERVER` and `CACHE-CONTROL`.

### Assessment

SSDP can identify UPnP devices after joining a LAN, but porting Wireshark's HTTP machinery would violate the bounded design goal. A future One-OS need could justify a tiny independent `wireshark_ssdp_parse()` that scans a bounded datagram for a whitelist of headers; today it does not outrank DNS-SD.

### Required input / boundaries

- UDP payload, typically SSDP multicast/unicast traffic.
- Parse ASCII/HTTP-like lines only up to fixed maximum datagram, line and header counts.
- Reject/flag NULs, overlong lines and missing header separators; tolerate CRLF and only intentionally support any alternate line endings.

### Cost / provenance / disposition

- Estimated flash: ~3–6 KiB; result RAM ~0.5–1 KiB if strings are copied, much less with spans.
- Wireshark source: `REFERENCE-ONLY`.
- If later approved: `CLEAN-ROOM REIMPLEMENT` from UPnP/SSDP specification.
- Disposition: **APP / TEST-REFERENCE; DEFER**, not initial L2 set.

### Vectors/fuzz cases

- `M-SEARCH`, `NOTIFY`, and `HTTP/1.1 200 OK` examples.
- Duplicate headers, unusual capitalization and empty values.
- Missing colon, overlong line/header count, embedded NUL, truncated final line.

## 9. Candidate F — DHCPv4 fingerprint fields (reference-only/defer)

### Upstream source and semantics

Primary source:

- `epan/dissectors/packet-dhcp.c`, which implements DHCP/BOOTP and cites RFC 951, RFC 2131, RFC 2132 and later option RFCs.

Potential discovery fields are DHCP message type, Host Name (option 12), Parameter Request List (55), Vendor Class Identifier (60), Client Identifier (61) and selected vendor/user-class options. DHCPv4 options use pad/end markers and length-delimited options; the fixed BOOTP/DHCP header and addresses use network byte order.

### Assessment

One-OS has no demonstrated raw-DHCP inventory application in the current baseline. On an ordinary joined network, lwIP already owns DHCP client behavior; passively observing arbitrary encrypted Wi-Fi clients would additionally require a separate authorized capture/decryption path. A Wireshark-family DHCP API now would be speculative.

### Required input / behavior if ever implemented

- Complete UDP DHCP payload from an authorized capture/socket path.
- Validate fixed header and DHCP magic cookie before options.
- Honor pad (`0`) and end (`255`); any option length beyond remaining bytes is `TRUNCATED`.
- Option-overload/concatenation rules must be explicitly scoped rather than partially guessed.

### Cost / provenance / disposition

- Estimated useful bounded subset: 5–10 KiB flash, 0.5–1 KiB result if copied; less with spans.
- Wireshark source: `REFERENCE-ONLY`.
- Production, if demanded: `CLEAN-ROOM REIMPLEMENT` from RFC 2131/2132/IANA.
- Disposition: **TEST/REFERENCE / DEFER**.

### Vectors/fuzz cases

- Discover/Offer/Request/Ack packets with common fingerprint options.
- Missing/incorrect magic cookie.
- pad/end placement, duplicate options, zero-length option.
- option length one byte beyond payload.
- truncated fixed header and overloaded-option edge cases.

## 10. Candidate G — BLE Link Layer + ATT/GATT (reference-only)

### Upstream source and semantics

- `epan/dissectors/packet-btle.c`: BLE Link Layer, including advertising/data-channel headers, access address, extended-advertising fields and reassembly-aware state.
- `epan/dissectors/packet-btatt.c`: Attribute Protocol and GATT-level attribute semantics; depends on L2CAP/Bluetooth state, reassembly and a large registered characteristic/service universe.

### Assessment

Do **not** create `wireshark_btatt_*` or a raw BTLE framework now:

- Normal NimBLE scan callbacks already provide the AD payload needed for nearby discovery, making the generic AD parser sufficient.
- Live authorized ATT/GATT procedures are already native NimBLE APIs. Wrapping them would violate the foundation rule against renaming native APIs.
- Wireshark's passive ATT/GATT dissection assumes connection/L2CAP state and reassembly not present in a normal scanner callback, and the full GATT semantic database is far too large for this ESP32-C6 baseline.

### Cost / provenance / disposition

- Full port cost: unacceptable/unbounded for this target; dependencies include state, reassembly and broad tables.
- Wireshark source: **REFERENCE-ONLY**.
- Disposition: **TEST/REFERENCE; no production API**.

### Vectors/fuzz cases

If a future approved bounded ATT-value parser exists, vectors must include MTU-edge PDUs, opcode-specific truncation, zero/invalid handles, error responses and long/reassembled attribute values. For Phase 1 these remain desktop oracle tests only.

## 11. Candidate H — Zigbee NWK/APS/ZDP/security (reference-only in Wireshark family)

### Upstream source and semantics

Primary Wireshark sources include:

- `epan/dissectors/packet-zbee-nwk.c`.
- `epan/dissectors/packet-zbee-aps.c`.
- `epan/dissectors/packet-zbee-zdp.c`.
- `epan/dissectors/packet-zbee-security.c`.
- supporting Zigbee TLV files.

The NWK dissector itself depends on 802.15.4, APS, ZDP and security code, demonstrating that this is a protocol stack rather than one bounded field parser.

### Assessment

The reusable Wireshark-family contribution should stop at IEEE 802.15.4 MAC boundaries. Zigbee network/application/security semantics are large, stateful and overlap the separate ZHA/zigpy research family. Creating another Zigbee API here would duplicate project families and invite cross-family architecture.

### Cost / provenance / disposition

- Full useful stack cost: large/unbounded relative to this MCU baseline; includes security and state.
- Wireshark source: **REFERENCE-ONLY** as a field truth/pcap oracle.
- Disposition: **TEST/REFERENCE; DROP as Wireshark production API**.

### Vectors/fuzz cases

Retain public Zigbee pcaps and malformed NWK/APS/ZDP frames as regression or differential test inputs for whichever approved Zigbee family eventually owns parsing; do not add them to a Wireshark runtime component.

## 12. Candidate I — Thread / 6LoWPAN / MLE (reference-only in Wireshark family)

### Upstream source and semantics

- `epan/dissectors/packet-thread.c` handles Thread over CoAP/beacon semantics and depends on 802.15.4, MLE, CoAP and cryptographic helpers.
- Related Wireshark dissectors include MLE/6LoWPAN/IPv6/CoAP layers.

### Assessment

Thread is a layered, stateful stack with security and IPv6 adaptation/reassembly. One-OS already has an independent OpenThread research family, which is the appropriate place to study portable Thread-native semantics. Wireshark remains valuable as a reference decoder and test oracle, not a second Thread runtime API.

### Cost / provenance / disposition

- Full stack: clearly outside the bounded Wireshark L2 scope.
- Wireshark source: **REFERENCE-ONLY**.
- Disposition: **TEST/REFERENCE; DROP as Wireshark production API**.

### Vectors/fuzz cases

Use public Thread captures for oracle comparison of beacons/MLE/CoAP when the owning family is implemented. Include truncated TLVs, invalid lengths and fragmentation edge cases there, not in this component.

## 13. Candidate J — WPS and deep 802.11 vendor IEs (reference-only initially)

Wireshark can deeply decode WPS/WFA and many vendor-specific IEs. Some public identity attributes (manufacturer/model/device name) would help nearby-device identification, but the surface area expands quickly and some WPS content is credential/security-adjacent.

Initial `wireshark_wifi_mgmt_parse()` should expose a bounded vendor IE descriptor (OUI/type + payload span), leaving semantic vendor decoding to explicitly approved, legitimate discovery parsers. Do not implement PIN/credential extraction, bypass or attack workflow support.

- Provenance: `REFERENCE-ONLY`; any future public-identity subset should be `CLEAN-ROOM REIMPLEMENT` from public WFA/vendor documentation.
- Disposition: **REFERENCE-ONLY / DEFER**.
- Fuzz focus if later approved: nested TLV length, duplicate attributes, oversized strings, unknown vendor attributes.

## 14. License/provenance matrix

| Candidate | Wireshark source license | COPY | PORT | Production provenance | Disposition |
|---|---|---:|---:|---|---|
| BLE AD | GPL-2.0-or-later | No | No | CLEAN-ROOM REIMPLEMENT from Bluetooth specs/Assigned Numbers | L2 API P0 |
| 802.11 mgmt/IE | GPL-2.0-or-later | No | No | CLEAN-ROOM REIMPLEMENT from IEEE/WFA public definitions | L2 API P0 |
| 802.15.4 MAC | GPL-2.0-or-later | No | No | CLEAN-ROOM REIMPLEMENT from IEEE 802.15.4 | L2 API P0 |
| DNS/mDNS/DNS-SD | GPL-2.0-or-later | No | No | CLEAN-ROOM REIMPLEMENT from RFC 1035/6762/6763 | L2 API P1 |
| SSDP | GPL-2.0-or-later | No | No | REFERENCE-ONLY now; clean-room if demanded | APP/DEFER |
| DHCPv4 | GPL-2.0-or-later | No | No | REFERENCE-ONLY now; clean-room if demanded | TEST/REFERENCE |
| BTLE / ATT / GATT | GPL-2.0-or-later | No | No | REFERENCE-ONLY | TEST/REFERENCE |
| Zigbee stack | GPL-2.0-or-later | No | No | REFERENCE-ONLY | DROP as Wireshark API |
| Thread stack | GPL-2.0-or-later | No | No | REFERENCE-ONLY | DROP as Wireshark API |
| WPS/deep vendor IE | GPL-2.0-or-later | No | No | REFERENCE-ONLY initially | DEFER |

## 15. ESP32-C6 bounded-design budget

All numbers below are **planning estimates, not measurements**. Phase 2 must measure actual `.text/.rodata/.bss` deltas and stack high-water marks.

| API | Est. flash | Est. result/working RAM | Heap | Main cost driver |
|---|---:|---:|---:|---|
| BLE AD | 3–6 KiB | 0.25–0.5 KiB | 0 | repeated AD descriptors |
| 802.11 mgmt/IE | 7–14 KiB | 0.35–0.7 KiB | 0 | RSN + repeated IEs |
| 802.15.4 MAC | 4–8 KiB | <0.2 KiB | 0 | address/PAN decision rules |
| DNS/mDNS | 9–18 KiB | 0.8–2 KiB | 0 | compressed names + RR subset |

Target aggregate for the first four should remain comfortably below ~50 KiB flash and avoid permanently allocating multi-KiB globals. DNS repeated-record storage should become caller-provided if measurements show a large fixed result struct.

## 16. Phase-2 test strategy if approved

For each approved parser:

1. Unit vectors derived from standards and small public pcaps, stored as minimal byte arrays or capture fixtures.
2. Differential/oracle checks against the pinned Wireshark snapshot for fields that both systems intentionally support; compare semantics, not internal tree structure.
3. Truncation sweep: test every prefix length from zero through full valid vector.
4. Length-field mutation: set every length/count to boundary, one-short, one-long and extreme values.
5. Fixed-capacity saturation: repeated elements beyond every configured result capacity.
6. Fuzz harness on host (libFuzzer/AFL-compatible wrapper if build infrastructure permits) plus deterministic corpus regression.
7. ESP32-C6 build and size report after each parser; no production parser should be merged without measured code/RAM delta.
8. No parser may require network/radio ownership to run its unit tests; input is bytes.

## 17. Prioritized implementation recommendation

### P0 — approve only these first

1. **BLE AD parser** — highest immediate value and smallest/cleanest boundary. Native NimBLE supplies exactly the payload it needs.
2. **IEEE 802.15.4 MAC parser** — raw receive path already validated; creates protocol-neutral MAC semantics without stealing Zigbee/Thread ownership.
3. **802.11 management/IE parser** — high discovery value, but production work should begin only together with a clearly defined raw-management-frame capture adapter/application requirement so it does not become a wrapper over `wifi_ap_record_t`.

### P1 — after P0 measurements

4. **DNS/mDNS/DNS-SD subset** — valuable joined-LAN discovery, but name compression and larger result storage justify doing it after the smaller radio parsers.

### Explicitly reference-only/deferred

- Wireshark BLE Link Layer engine.
- ATT/GATT dissector and GATT characteristic universe.
- Zigbee NWK/APS/ZDP/security stack.
- Thread/MLE/6LoWPAN/CoAP stack.
- DHCPv4 parser unless an authorized LAN-inventory application asks for it.
- SSDP unless an UPnP discovery application asks for it.
- WPS/deep vendor-specific 802.11 dissector universe.
- GUI, filters, plugin runtime, reassembly/conversation framework, preferences, crypto/key databases, taps/statistics and generic dissector dispatch.

## 18. Phase-1 stop point

Phase 1 is complete with this document. **No production `wireshark_*` parser code, component, header or build integration has been created.**

Implementation must not start until the user explicitly approves Phase 2 and specifies which of the recommended parsers are approved.
