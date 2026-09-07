# Theengs portable Level-2 API — Phase 1 research

Status: **research only; no production implementation approved**  
Target: Waveshare ESP32-C6-Touch-LCD-1.9 / ESP-IDF / NimBLE  
Research date: 2026-09-07  
One-OS branch: `research/theengs-l2-api`

## 1. Scope and decision summary

Theengs has clear Level-2 value for One-OS, but the upstream runtime should **not** be imported as-is.

The reusable capability is the semantic layer above BLE advertisements:

1. identify a model from service data, manufacturer data, advertised name, UUID, and when required the device address;
2. decode bounded typed measurements/state from the matched advertisement;
3. expose model metadata such as device class, active/continuous-scan hints, tracking/random-address hints, controllability, battery/voltage-primary status, and encryption scheme;
4. optionally perform narrowly scoped, authorized device-control workflows proven by upstream Theengs App and primary vendor protocol documentation.

The recommended One-OS implementation model is a **clean-room, bounded C decoder engine backed by generated immutable tables**, not the upstream JSON/ArduinoJson runtime. The device rules/data must remain a separately governed artifact with explicit provenance and deterministic generation.

The first implementation approval, if granted, should cover passive identification/decoding and metadata only. Encrypted-payload decryption and active control should remain separate later approvals.

## 2. One-OS constraints and prior work

The root `README.md` establishes the relevant architecture constraints:

- ESP-IDF, NimBLE, FreeRTOS, and other native APIs remain directly usable;
- project Level-2 APIs must represent reusable semantic capabilities rather than native-API renaming;
- Level-2 families are peers and may not expose or depend on another project's public API/types;
- scanner/session/coordinator frameworks and old `nearby_*` compatibility APIs are intentionally absent from the clean foundation;
- the board baseline has 8 MB flash and no PSRAM assumption.

The branch `sdkconfig.defaults` already enables NimBLE, BLE 5.0 features, and extended advertising. Therefore a Theengs decoder does not need to own Bluetooth-host lifecycle or provide a scan abstraction.

The archived `v0.1.0-beta.2` smoke application is useful only as L1 validation. It initializes native NimBLE, runs a passive discovery, and counts legacy/extended advertising reports. It does not parse advertisement content or contain a recognition/decoder database. There is therefore no previous One-OS Level-2 decoder contract that needs to be preserved.

Disposition of prior One-OS work: **REFERENCE-ONLY / TEST/TOOL** for proving native NimBLE scan capability; no production API or decoder code to migrate.

## 3. Upstream baselines inspected

### 3.1 Theengs Decoder

Repository: <https://github.com/theengs/decoder>

Research baselines:

- latest stable release at research time: `v2.4.0`, published 2026-09-02;
- inspected development commit: `280c6fa0040f1b76ba60c713173d85ff0a16b0c8` (2026-09-04).

Relevant upstream files/modules:

- `src/decoder.cpp` and `src/decoder.h` — decoder runtime and matching/evaluation logic;
- `src/decoder_c.cpp` and `include/shared/theengs.h` — JSON-string C bridge;
- `src/devices.h` and `src/devices/*_json.h` — compiled-in device rule dataset;
- `docs/participate/adding-decoders.md` — declarative condition/property grammar;
- `docs/use/include.md` — integration and encrypted-payload behavior;
- `tests/BLE/test_ble.cpp` and `tests/BLE_fail/` — regression and failure-vector structure.

The project describes itself as a portable/lightweight BLE IoT decoder and currently documents support for more than 140 Bluetooth devices. The dataset covers environmental sensors, plant sensors, probes, scales, TPMS, battery/energy devices, trackers/beacons, SwitchBot/Shelly devices, Apple advertisements, Victron devices, and others.

### 3.2 Theengs App

Repository: <https://github.com/theengs/app>

Inspected development commit: `27c6cd3aabe60a58a2ff23ea396f3393b607c493`.

The App consumes Theengs Decoder but also contains substantial non-portable application infrastructure: Qt/QML UI, persistence, filtering, charts, MQTT/gateway behavior, and platform Bluetooth adapters. None of that belongs in a reusable One-OS Theengs Level-2 family.

The App is nevertheless useful as a **reference** for proven active BLE control workflows. It currently implements control for:

- SwitchBot Bot S1 / SmartSwitch (`SBS1` / `X1`);
- SwitchBot Curtain 2/3 (`SBCU` / `W070160X`);
- SwitchBot Blind Tilt (`SBBT` / `W270160X`).

For Bot S1, the inspected implementation discovers the SwitchBot service, enables notifications, writes commands with response, requests device info, decodes status responses, and implements action/configuration commands. The source itself links to SwitchBot's public BLE API documentation. If active control is later approved, One-OS should derive packet formats and state-machine behavior from the primary vendor specification and independent tests, using Theengs App only as a secondary reference.

### 3.3 Primary vendor material for active control

Primary protocol reference: <https://github.com/OpenWonderLabs/SwitchBotAPI-BLE>

The vendor documentation confirms a central-to-peripheral BLE GATT request/response model, SwitchBot service/characteristic UUIDs, command framing, response status values, and device-specific command payloads. It also demonstrates that protocol identifiers and advertising formats can change across device/firmware generations, so updateability and per-rule provenance are necessary.

## 4. What the upstream Decoder actually does

### 4.1 Input packet model

`decodeBLEJson()` consumes an ArduinoJson object containing some combination of:

- `servicedata`;
- `manufacturerdata`;
- `name`;
- `servicedatauuid`;
- device address (`id` / `mac` depending on integration).

The web parser documentation describes the same raw fields. This is a good semantic boundary for One-OS, but JSON itself is not required for the firmware API.

### 4.2 Device fingerprinting

Each upstream device definition contains declarative match conditions. The inspected grammar supports, among other operations:

- service-data or manufacturer-data length checks;
- byte/hex-string containment;
- value-at-index matching;
- device-address and reverse-device-address matching at an index;
- advertised-name matching;
- service UUID matching;
- AND/OR combinations and nested conditions;
- property-specific conditions.

The upstream documentation explicitly warns that nested recursive conditions can cause stack overflow. A One-OS runtime should therefore not preserve recursive evaluation as an unbounded capability.

### 4.3 Property decoding

After a model matches, upstream rules can extract and transform values through operations including:

- integer/float extraction from hex data with endianness and signedness;
- bit-field extraction;
- fixed/static values;
- string/ASCII extraction;
- arithmetic and bitwise post-processing;
- enum-style lookup;
- conditional properties;
- unit-derived values such as Celsius/Fahrenheit conversions.

This is a genuine reusable Level-2 semantic capability: it converts raw BLE advertisement bytes into a stable device/model identity and useful typed state.

### 4.4 Device metadata tags

The upstream rule tag encodes device class and behavioral metadata. Useful portable concepts include:

- semantic device class (temperature/humidity, contact/motion, battery, plant, tire, energy, actuator, tracker, etc.);
- active scanning required;
- continuous scanning required;
- tracker discoverability;
- potential random-MAC behavior;
- controllable device;
- battery/voltage as a primary property;
- encryption model.

These are useful as Level-2 metadata, but they should be represented as stable One-OS-owned C enums/flags rather than copied JSON keys or JSON values.

### 4.5 Upstream runtime cost and dispatch model

The upstream C++ class defaults to a `DynamicJsonDocument` capacity of about 11,800 bytes for the device rule being parsed. `decodeBLEJson()` then linearly iterates through `_devices` and deserializes each device JSON definition until a match is found. Model/attribute lookups follow the same parse-and-scan pattern. The C bridge additionally accepts/returns serialized JSON and uses C++ strings/heap allocation.

This design is portable enough for Arduino-class environments, but it is a poor fit for the deterministic One-OS baseline on an ESP32-C6 with no PSRAM assumption:

- unnecessary runtime JSON parsing and heap activity;
- repeated parsing of immutable rules;
- worst-case linear traversal over the whole model set;
- C++/ArduinoJson dependency introduced solely for the decoder;
- recursive condition grammar that must be bounded defensively;
- difficult-to-predict peak stack/heap and decode latency as the corpus grows.

The issue is not whether the upstream library can run on an ESP32; upstream explicitly supports MCU use. The issue is whether its runtime model is the right One-OS Level-2 contract. It is not.

## 5. Recommended portable architecture

### 5.1 Boundary: advertisement acquisition stays L1/application-owned

The Theengs Level-2 family must not initialize NimBLE, start/stop scanning, own scan sessions, or wrap GAP callbacks. The application receives native NimBLE advertising reports and presents a short-lived, project-owned view of relevant data to the decoder.

A provisional research API shape is:

```c
typedef struct {
    const uint8_t *manufacturer_data;
    uint16_t manufacturer_len;
    const uint8_t *service_data;
    uint16_t service_len;

    uint16_t service_uuid16;
    bool has_service_uuid16;

    const char *name;
    uint8_t name_len;

    uint8_t address[6];
    uint8_t address_type;
    bool has_address;

    int8_t rssi;
} theengs_adv_view_t;
```

This type intentionally does not expose a NimBLE struct. It is not another radio abstraction: it is the minimal protocol-neutral input record required by the semantic decoder and can also be populated by host tests or captured vectors.

### 5.2 Candidate A — passive model identification and advertisement decoding

**Proposed API family**

```c
theengs_status_t theengs_decode_adv(
    const theengs_adv_view_t *adv,
    theengs_decoded_t *out);
```

`theengs_decoded_t` should contain:

- a compact internal model key plus a stable model-id string lookup;
- semantic model flags/class;
- an encryption-status/scheme field;
- a fixed-capacity array of typed decoded properties;
- an explicit `value_count` and truncation/unsupported status if a generated rule would exceed the compile-time bound.

Property values should be typed (`signed`, `unsigned`, floating point, boolean, enum/token, short text/bytes as justified by corpus analysis) and identified by generated property IDs. The implementation phase must derive exact fixed bounds from the selected corpus rather than guessing them in the public API.

**Source/module:** Theengs Decoder `decoder.cpp`, rule grammar, device definitions.  
**Packet/workflow:** BLE advertising service/manufacturer/name/UUID/address -> model match -> typed field extraction.  
**Supported family:** selected subset of the upstream BLE device/protocol corpus, expanded through the governed data process below.  
**Product value:** broad passive nearby-device identification plus useful sensor/status/state decoding without connections.  
**L1 APIs composed:** none owned by the decoder; application supplies bytes originating from native NimBLE GAP reports.  
**Why L2:** model fingerprinting, protocol dispatch, and semantic field extraction are reusable logic substantially above BLE transport primitives.  
**ESP32-C6 feasibility:** strong if implemented as immutable generated tables plus fixed scratch/result storage; no runtime JSON parser or heap required.  
**Bounded-data strategy:** pre-index candidate rules, fixed property count, fixed operator grammar, fixed condition depth, validate all offsets at generation time and runtime.  
**Updateability:** versioned source manifest + deterministic generator + pinned provenance; regenerate firmware tables.  
**Authorization:** passive only; no special authorization beyond ordinary lawful observation of advertisements.  
**License/provenance:** `CLEAN-ROOM REIMPLEMENT`; Theengs source/rules are `REFERENCE-ONLY` unless an explicit GPL distribution decision is made.  
**Disposition:** **L2 API**.  
**Tests/vectors:** independent raw advertisement vectors, collision/negative vectors, truncation suite, corpus invariants, and optional offline differential reference checks.

### 5.3 Candidate B — model and property metadata lookup

**Proposed API family**

```c
bool theengs_model_info(uint16_t model_key, theengs_model_info_t *out);
bool theengs_property_info(uint16_t property_key, theengs_property_info_t *out);
```

Metadata should cover stable model ID, human-readable brand/model strings where selected for the build, device class, capability/scan/encryption flags, and compact property metadata such as unit/semantic kind.

**Source/module:** Theengs rule metadata and tag model.  
**Packet/workflow:** generated model/property key -> immutable metadata record.  
**Supported family:** exactly the models present in the selected generated pack.  
**Product value:** lets UI/apps present decoded data without embedding vendor/model semantics themselves.  
**L1 APIs composed:** none.  
**Why L2:** metadata is part of the reusable decoder semantic model, not a board/native primitive.  
**ESP32-C6 feasibility:** strong; immutable strings/tables can remain in flash and returned as read-only views.  
**Bounded-data strategy:** generated IDs and length-delimited/static strings; no runtime JSON.  
**Updateability:** generated from the same versioned model manifest as Candidate A.  
**Authorization:** none.  
**License/provenance:** `CLEAN-ROOM REIMPLEMENT` API/data schema; each data record must retain independent provenance.  
**Disposition:** **L2 API + DATA**.  
**Tests/vectors:** lookup completeness, stable-ID checks, missing-key behavior, schema/version checks.

### 5.4 Candidate C — encrypted-advertisement envelope recognition

The upstream Decoder does not generally perform key acquisition/decryption internally. For encrypted formats it can identify the encryption model and expose bounded envelope fields such as cipher text, counter, MIC, and address; integrations may decrypt with a valid bind key and submit plaintext for normal decoding.

One-OS should preserve only the reusable passive portion initially: recognize the encryption scheme and return `THEENGS_NEEDS_KEY` (or equivalent) together with safely parsed, bounded envelope metadata when useful.

**Proposed research shape:** encryption scheme/status embedded in `theengs_decoded_t`, with an optional fixed-capacity `theengs_encrypted_payload_t` if corpus analysis proves it useful.

**Source/module:** Theengs encrypted-device rules and `docs/use/include.md`.  
**Packet/workflow:** encrypted advertisement -> model/scheme recognition -> bounded envelope extraction.  
**Supported family:** only explicitly documented schemes represented in the selected data pack.  
**Product value:** identifies otherwise useful encrypted devices and tells the application why fields are unavailable.  
**L1 APIs composed:** none.  
**Why L2:** encryption-envelope interpretation is protocol semantics, not BLE transport.  
**ESP32-C6 feasibility:** strong for identification/extraction.  
**Bounded-data strategy:** fixed envelope limits per generated scheme; reject malformed/truncated packets.  
**Updateability:** scheme/version metadata in generated pack.  
**Authorization:** actual decryption requires a legitimately supplied user/device key; the decoder must never discover, extract, guess, or bypass credentials.  
**License/provenance:** `CLEAN-ROOM REIMPLEMENT` from protocol facts/primary documentation; Theengs implementation `REFERENCE-ONLY`.  
**Disposition:** **L2 API feature** for envelope/status; actual decryption **DEFER** pending explicit approval and key-storage policy.  
**Tests/vectors:** encrypted-envelope positive/truncated vectors; verify no plaintext is fabricated without a supplied key.

### 5.5 Candidate D — authorized SwitchBot actuator control

This is a separate, connection-oriented capability demonstrated in Theengs App for Bot S1, Curtain 2/3, and Blind Tilt. It should not be mixed into the passive decoder engine.

Possible later API surface:

```c
theengs_status_t theengs_switchbot_get_info(...);
theengs_status_t theengs_switchbot_bot_action(...);
theengs_status_t theengs_switchbot_curtain_move(...);
theengs_status_t theengs_switchbot_blind_tilt(...);
```

The exact signatures must be designed only in an approved implementation phase, after deciding how to pass native NimBLE connection context without inventing a general BLE/session wrapper.

**Source/module:** Theengs App `device_sbs1.cpp`, `device_sbcu.cpp`, `device_sbbt.cpp`, `device_utils_switchbot.h`; primary source is SwitchBot's public BLE API repository.  
**Packet/workflow:** connect -> discover GATT service/chars -> enable notifications -> write framed command with response -> parse protocol status/info -> disconnect/return according to caller policy.  
**Supported family:** initially only the App-proven `SBS1`, `SBCU`, `SBBT` workflows, and only after primary vendor documentation and owned-hardware tests agree.  
**Product value:** legitimate local control of nearby owned devices without a cloud dependency.  
**L1 APIs composed:** native NimBLE GAP/GATT APIs and FreeRTOS synchronization/timeouts.  
**Why L2:** device-protocol command encoding plus multi-step GATT request/response state is a reusable workflow above GATT primitives.  
**ESP32-C6 feasibility:** strong for a small number of protocol state machines; no large database required.  
**Bounded-data strategy:** fixed command/response buffers, finite state machine, explicit timeouts, one caller-selected connection/target at a time.  
**Updateability:** protocol version/device-model table with primary-source provenance; do not infer support for newly listed vendor models automatically.  
**Authorization:** caller must explicitly select/authorize the target; no bulk/broadcast actuator control, no pairing/authentication bypass, no credential acquisition, fail closed on protocol/busy/low-battery/unsupported errors. Physical-motion actions may require application-level confirmation.  
**License/provenance:** Theengs App is `REFERENCE-ONLY`; implementation should be `CLEAN-ROOM REIMPLEMENT` from the vendor BLE API and independent traces/tests.  
**Disposition:** **L2 API, P2 / separate approval**.  
**Tests/vectors:** pure command codec vectors from vendor docs, mocked GATT state transitions, timeout/disconnect/error/status tests, then owned-device hardware integration.

## 6. Device data must be separate from the runtime engine

The most important architectural split is:

```text
Theengs L2 decoder engine
    + generated immutable model pack
    + generated property/model metadata
    + host-side generator/validator
```

The engine should not contain hand-written device-specific C branches for every product, and the upstream JSON corpus should not be silently transformed into firmware source.

### 6.1 Canonical manifest

If implementation is approved, create a One-OS-owned declarative manifest outside the runtime. Each model entry should record at least:

- stable One-OS model identifier;
- brand/model display metadata if included;
- primary protocol/source URLs and source revision/date;
- capture/test-vector provenance;
- advertisement discriminators and minimum lengths;
- bounded match conditions;
- decoded properties, types, offsets/bit fields, scaling and enum mappings;
- scan/encryption/control metadata;
- explicit precedence when two models can otherwise collide.

The manifest is **DATA**, not production logic. Its license/provenance must be reviewable per entry.

### 6.2 Offline generator

A host-side generator/validator should turn the canonical manifest into deterministic C data. It may use flexible tooling on the developer machine; firmware must not need that parser.

Generator checks should reject:

- recursive/unbounded conditions;
- unsupported operators;
- invalid offsets/lengths;
- property counts above the configured result bound;
- duplicate/stale IDs;
- ambiguous matches without explicit precedence;
- missing provenance;
- unsupported string/enum sizes.

Generator disposition: **TEST/TOOL**. Generated output disposition: **DATA**.

### 6.3 Runtime dispatch

Do not linearly parse every model. Build candidate buckets from cheap stable discriminators such as:

- AD source type (manufacturer/service data);
- Bluetooth Company ID where standards-compliant;
- service UUID;
- minimum/exact data length;
- short prefix/signature;
- advertised-name prefix only where protocol requires it.

Only the small candidate bucket should run detailed conditions. Rules that depend on MAC/address must be explicitly marked because address randomization/platform masking affects portability.

The runtime evaluator should have a fixed operator set and no recursion. All reads must be bounds-checked even when the generator already proved the canonical rule.

### 6.4 Compiled vs external data

**Phase-2 recommendation: compile the selected generated pack into firmware flash.** The target has 8 MB flash, while RAM is more constrained and no PSRAM is assumed. Compile-time data also gives deterministic integrity/versioning and avoids adding an external rule parser.

Do not yet choose a single mandatory full corpus. Measure generated flash size and candidate-index size, then choose build profiles if useful (for example `core` and `extended`).

An SD-card/external/updatable decoder pack is **DEFERRED**. It would require a versioned binary schema, integrity/authenticity policy, strict size/bounds validation, and safe rollback. Arbitrary unsigned runtime rules are not appropriate for the first implementation.

## 7. Memory, flash, and timing feasibility

No exact One-OS flash footprint is claimed in Phase 1 because no implementation or generated corpus has been built.

What is known:

- target flash: 8 MB;
- target baseline: no PSRAM;
- upstream Decoder's rule scratch JSON capacity defaults to about 11.8 KB;
- upstream runtime repeatedly deserializes immutable rule JSON while scanning the model array;
- One-OS already has native NimBLE enabled.

A generated C design should therefore move immutable model data to flash and keep runtime RAM bounded to:

- caller-provided advertisement view;
- small decode scratch state;
- fixed decoded-result storage;
- a small candidate list/index cursor.

Before merging any implementation, measure at minimum:

1. `idf.py size-components` / total firmware delta for each model-pack profile;
2. free heap before/after worst-case decode and verify no decode-path leaks;
3. decoder task/caller stack high-water mark;
4. median and worst-case decode latency for a miss, a first-bucket match, and the largest candidate bucket;
5. maximum generated property count and command/envelope buffer sizes.

Acceptance should be based on measured bounds, not the upstream project's generic “lightweight” label.

## 8. Updateability and provenance model

A decoder database will change much faster than the engine. Keep updates reproducible:

- pin every generated pack to a manifest schema version and generator version;
- include source URLs and revision/date metadata for each model/protocol family;
- produce deterministic generated files and a content checksum;
- require CI to regenerate and fail on uncommitted drift;
- review data changes separately from engine changes when practical;
- add/modify a model only with positive vectors and at least one relevant negative/collision vector;
- keep removed/renamed model IDs mapped deliberately rather than silently reusing numeric IDs.

Theengs upstream releases are a useful coverage/change signal, not an automatic ingestion feed.

## 9. License and provenance conclusion

The inspected Theengs Decoder source headers and repository license are GPLv3-or-later. The inspected Theengs App is also GPLv3-family code and itself lists GPL/LGPL dependencies. The inspected One-OS branch does not contain a repository-root license file.

This document is not legal advice. For engineering provenance, the conservative Phase-1 rule is:

- **Theengs Decoder source code:** `REFERENCE-ONLY`;
- **Theengs App source code:** `REFERENCE-ONLY`;
- **Theengs device-rule corpus:** `REFERENCE-ONLY` unless the project explicitly adopts a compatible distribution/licensing strategy;
- **One-OS decoder/API implementation:** `CLEAN-ROOM REIMPLEMENT` from protocol facts, primary vendor documentation, independently authored manifests, and independent captures/tests;
- **SwitchBot active control:** `CLEAN-ROOM REIMPLEMENT` from the primary OpenWonderLabs BLE API, with Theengs App only as a workflow cross-check.

Do not copy code, tables, JSON definitions, or the regression corpus into production merely because they are technically portable.

If the project later chooses GPL-compatible incorporation, that decision should be explicit and repository-wide before changing any provenance classification above.

## 10. Test strategy and sample vectors

### 10.1 Passive decoder tests

Run the core decoder as host-side pure-C tests independent of NimBLE. Required categories:

- known positive advertisement for every shipped model/rule variant;
- exact expected model identity and typed property values;
- negative/collision vectors differing by one discriminator;
- truncation at every relevant boundary (`0..required_len-1`);
- null/missing optional name, UUID, address, service data, or manufacturer data;
- malformed/unknown advertisements;
- min/max numeric values, signed boundaries, bit fields, enum misses;
- maximum property-count case;
- generated-rule invariants and ambiguity checks;
- randomized/fuzzed byte buffers under host sanitizers where available.

A research/reference vector published by upstream Decoder is:

```text
service data: 712098000163b6658d7cc40d0410024001
reference result: Xiaomi / miflora / HHCCJCY01HHCC, tempc=32, tempf=89.6
```

Treat this as a `REFERENCE-ONLY` research baseline. Production vectors should preferentially come from primary protocol documentation or independently captured owned devices with their provenance recorded.

The large upstream `tests/BLE/test_ble.cpp` and `tests/BLE_fail` suites are valuable as evidence of the kinds of regression/failure coverage needed. Recreate equivalent One-OS-owned tests rather than wholesale-copying the GPL corpus.

### 10.2 Encrypted-envelope tests

- identify encrypted scheme/model without a key;
- exact extraction boundaries for cipher/counter/MIC/address where applicable;
- truncated/malformed envelope rejection;
- prove the API never invents plaintext or attempts credential discovery;
- if decryption is ever separately approved, test only caller-supplied legitimate keys and ensure secrets are not logged.

### 10.3 Authorized control tests

Before radio integration, test command codecs and response parsers with primary vendor vectors. Then test a mocked finite GATT workflow for:

- service/characteristic discovery failure;
- notification setup failure;
- write timeout;
- disconnect at each state;
- protocol OK/error/busy/unsupported/low-battery responses;
- parameter boundaries for position, mode, duration, and speed.

Hardware tests must use explicitly owned/authorized devices and one selected target at a time.

## 11. Prioritized API table

| Priority | Candidate | Proposed One-OS surface | Disposition | L1 composed | Provenance | Phase-1 recommendation |
|---|---|---|---|---|---|---|
| P0 | Passive advertisement identify/decode | `theengs_decode_adv()` + owned input/result/value types | **L2 API** | Application-fed native NimBLE advertisement bytes | `CLEAN-ROOM REIMPLEMENT`; upstream `REFERENCE-ONLY` | Approve first; bounded no-heap generated-table design |
| P0 | Model/property semantic metadata | `theengs_model_info()`, `theengs_property_info()` | **L2 API + DATA** | none | `CLEAN-ROOM REIMPLEMENT` + per-record provenance | Ship with decoder; same generated pack/schema |
| P0 | Model-pack generator/validator | host tool producing immutable C/index tables | **TEST/TOOL + DATA** | none | independently authored manifest; source provenance per model | Required before broad corpus growth |
| P1 | Encrypted advertisement recognition/envelope | decode status/scheme + optional bounded envelope | **L2 API feature** | none | clean-room from protocol facts/primary docs | Recognition only first; decryption deferred |
| P2 | SwitchBot Bot S1 local control | later `theengs_switchbot_*` command/workflow API | **L2 API** | native NimBLE GATT + FreeRTOS timeout/sync | vendor docs `CLEAN-ROOM`; App `REFERENCE-ONLY` | Separate explicit approval + owned-hardware tests |
| P2 | SwitchBot Curtain 2/3 local control | later `theengs_switchbot_curtain_*` | **L2 API** | native NimBLE GATT + FreeRTOS | same | Separate explicit approval |
| P2 | SwitchBot Blind Tilt local control | later `theengs_switchbot_blind_*` | **L2 API** | native NimBLE GATT + FreeRTOS | same | Separate explicit approval |

## 12. Exclusions

- **DROP — upstream JSON-in/JSON-out C bridge.** It retains JSON serialization, heap/string behavior, and does not define the desired embedded C semantic boundary.
- **DROP — runtime ArduinoJson device-rule interpreter.** Immutable rules should be generated into bounded C tables/indexes rather than parsed repeatedly on-device.
- **DROP — generic NimBLE scan/session/radio lifecycle wrapper.** One-OS L1 stays native; the Theengs decoder consumes advertisement data and does not own discovery.
- **APP — scan policy implied by `acts`/`cont`.** L2 may expose the metadata, but applications decide whether/how to scan.
- **APP — tracker/presence policy.** L2 may identify tracker/random-address hints; it must not create a tracking service or identity policy.
- **APP/DROP — Theengs App Qt/QML UI, SQLite/database manager, charts, filters, MQTT/TLS gateway, mobile platform plumbing.** These are not reusable device-protocol L2 capabilities.
- **DROP — automatic import/copy of the full Theengs GPL device database into firmware.** Data must have an explicit compatible licensing decision or be independently curated with provenance.
- **DEFER — arbitrary SD/external decoder packs.** Requires a bounded versioned binary format plus integrity/authenticity and rollback policy before it is safe to load runtime rules.
- **DEFER — encrypted-payload decryption/key storage.** Needs separate authorization/security design; no key discovery, credential extraction, guessing, or auth bypass is in scope.
- **DROP — bulk/broadcast actuator control, authentication bypass, credential theft, jamming/deauthentication, hostile MITM, exploit delivery, or persistence.** These are outside the project safety boundary.
- **DROP — cross-family adapters or `nearby_*` compatibility shims.** The public family remains independent and uses `theengs_*` names only.
