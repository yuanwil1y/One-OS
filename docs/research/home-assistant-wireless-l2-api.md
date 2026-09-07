# Home Assistant Wireless Level-2 API Research — Phase 1

Status: **research only; no production firmware implementation**  
Target: Waveshare ESP32-C6-Touch-LCD-1.9 / ESP32-C6 / ESP-IDF + FreeRTOS + LVGL / no PSRAM assumed  
One-OS branch: `research/home-assistant-wireless-l2-api`  
Research date: 2026-09-07  
Home Assistant Core snapshot: `b37f49658ab6447f73fb9be6187b4ca963254983` (`dev` at research time)

## 1. Scope and architecture constraints

This report follows the repository root `README.md` and `AGENTS.md`.

The main architecture consequence is that Home Assistant-derived Level-2 APIs may preserve **Home Assistant discovery semantics**, but must not claim ownership of lower-level mechanisms that Home Assistant merely consumes. ESP-IDF Wi-Fi, NimBLE, lwIP and IEEE 802.15.4 remain Level 1. Other upstream project families such as zigpy/ZHA, OpenThread and Matter remain peers and must not be routed through `ha_*`.

The first-phase question is therefore not “what radios can ESP32-C6 scan?”, but “which reusable discovery workflows are actually defined by Home Assistant, and which of those are worth a bounded embedded C API?”

## 2. Sources and provenance snapshot

Primary Home Assistant sources inspected at the pinned Core commit above:

- `homeassistant/components/bluetooth/match.py` — BLE matcher indexing, match semantics and match-history suppression.
- `homeassistant/loader.py` — discovery manifest matcher schemas.
- `homeassistant/generated/bluetooth.py` — generated BLE integration matcher corpus.
- `homeassistant/components/zeroconf/discovery.py` — Zeroconf service normalization, matcher semantics and HomeKit model routing.
- `homeassistant/generated/zeroconf.py` — generated Zeroconf and HomeKit discovery corpus.
- `homeassistant/components/ssdp/scanner.py` — SSDP listener/search lifecycle, matcher indexing and UPnP description enrichment.
- `homeassistant/generated/ssdp.py` — generated SSDP matcher corpus.
- `homeassistant/components/dhcp/__init__.py` — DHCP/client observation normalization, OUI/hostname matcher indexing and rediscovery.
- `homeassistant/generated/dhcp.py` — generated DHCP matcher corpus.
- `homeassistant/components/thread/discovery.py` — Thread Border Router discovery over `_meshcop._udp.local.` mDNS.
- `homeassistant/components/zha/manifest.json` — evidence that ZHA depends on the separate `zha`, `zha-quirks` and zigpy radio stack ecosystem.
- `homeassistant/components/matter/manifest.json` — evidence that Matter uses separate Matter client/proxy projects and mDNS service discovery.
- Home Assistant Core `LICENSE.md` — Apache License 2.0.

Relevant ESP-IDF source:

- ESP-IDF v6.1 `docs/en/api-guides/coexist.rst` — single 2.4 GHz RF resource and supported/unsupported Wi-Fi/BLE/IEEE 802.15.4 coexistence combinations.

Relevant prior One-OS evidence:

- Commit `d876905d7ca1ebb2dc11c960c5bf5af848a15b7d`, `firmware/main/smoke_tests.c` — temporary beta.2 hardware smoke code used native ESP-IDF active Wi-Fi AP scan, native NimBLE passive BLE scan and native IEEE 802.15.4 channel sweep. This validates Level-1 hardware paths but does **not** define a Home Assistant API and must not be copied back as an `ha_*` abstraction.

Upstream links:

- https://github.com/home-assistant/core/blob/b37f49658ab6447f73fb9be6187b4ca963254983/homeassistant/components/bluetooth/match.py
- https://github.com/home-assistant/core/blob/b37f49658ab6447f73fb9be6187b4ca963254983/homeassistant/components/zeroconf/discovery.py
- https://github.com/home-assistant/core/blob/b37f49658ab6447f73fb9be6187b4ca963254983/homeassistant/components/ssdp/scanner.py
- https://github.com/home-assistant/core/blob/b37f49658ab6447f73fb9be6187b4ca963254983/homeassistant/components/dhcp/__init__.py
- https://github.com/home-assistant/core/blob/b37f49658ab6447f73fb9be6187b4ca963254983/homeassistant/components/thread/discovery.py
- https://github.com/espressif/esp-idf/blob/v6.1/docs/en/api-guides/coexist.rst

## 3. Executive conclusion

### 3.1 What is genuinely Home Assistant-derived

The strongest reusable HA semantics are:

1. **BLE integration matching and rediscovery suppression** based on connectability, local-name glob, service UUID, service-data UUID, manufacturer ID and manufacturer-data prefix, with bounded history of which advertisement fields have already been seen.
2. **Zeroconf/mDNS matching and service normalization**: service type plus optional instance-name glob and TXT-property glob matching, plus normalized service data.
3. **HomeKit discovery classification over mDNS**: model (`md`/`MD`) matching and paired-state (`sf`) handling used by HA to route discoveries.
4. **SSDP matcher + discovery enrichment semantics**: matching headers/device-description fields, listening for advertisements, active search, and optional description lookup.
5. **DHCP matcher semantics**: normalized IP/MAC/hostname followed by hostname glob and MAC/OUI matching. The matcher is HA-defined; the observation sources are largely external/host-specific.
6. **Thread Border Router classification over mDNS**: useful HA-specific parsing of `_meshcop._udp.local.` records, but it is LAN discovery, not IEEE 802.15.4 RF scanning.

### 3.2 What is not Home Assistant-derived

- Raw 802.11 AP scanning is not a Home Assistant discovery API. On One-OS it remains native `esp_wifi_*` Level 1.
- Raw IEEE 802.15.4 scanning/sniffing is not defined by Home Assistant Core. Zigbee behavior belongs to ZHA/zigpy and native radio stacks; Thread radio behavior belongs to OpenThread; Matter commissioning/control belongs to Matter/CHIP.
- HA's Bluetooth stack consumes Bleak/habluetooth/adapters for transport. The reusable HA contribution is principally matcher/lifecycle semantics, not a new BLE controller API.
- Generic HomeKit, Matter, Zigbee or UPnP control must not be implemented as a Home Assistant wrapper merely because HA integrates those protocols.

## 4. Passive discovery vs active discovery vs interaction

| Class | Examples in this research | Network/radio side effect | Phase-1 conclusion |
|---|---|---|---|
| Passive discovery | BLE passive advertisements; SSDP NOTIFY reception; DHCP request/client observations | Receive/listen only | Good input to HA matchers where feasible |
| Active discovery, not control | mDNS browse/query; SSDP M-SEARCH; optional UPnP description GET | Queries peers/services but does not change device state | Valid future `ha_mdns_*` / `ha_ssdp_*` workflow if bounded |
| Native RF scan | Wi-Fi AP active/passive scan; IEEE 802.15.4 channel receive sweep | Radio-level discovery | Level 1 or another project, not `ha_*` |
| Active interaction/control | BLE GATT writes, UPnP actions, HomeKit pairing/control, Matter commissioning/control, Zigbee joining/control | Reads/writes device state and often requires credentials/keys | Not part of Phase 1; generic forms should stay in their owning protocol/project family |

## 5. Candidate A — HA BLE integration matcher and discovery history

### 5.1 Exact upstream source and behavior

`homeassistant/components/bluetooth/match.py` defines the matcher engine. Supported matching dimensions include connectability, local name, service UUID, service-data UUID, manufacturer ID and manufacturer-data prefix. All specified matcher fields must match. Local-name matching uses fnmatch-style globbing; HA rejects overly broad local-name patterns whose first three characters contain a pattern construct.

HA indexes each matcher into one likely-selective bucket (local name, then manufacturer ID, then service UUID, then service-data UUID) to avoid comparing every advertisement against every matcher. It also keeps separate bounded LRU match histories for connectable/non-connectable observations. A device is reconsidered when its name changes or newly observed manufacturer/service-data/service-UUID fields appear.

The current generated matcher corpus is in `homeassistant/generated/bluetooth.py`.

### 5.2 Ownership classification

- **HA-defined:** matcher schema, matching semantics, indexing strategy, new-field rediscovery suppression.
- **Consumed by HA:** Bleak, habluetooth, bluetooth-adapters and OS/controller transport.
- **Not HA:** ESP32 NimBLE scanning itself.

### 5.3 Value to One-OS

This directly turns raw BLE advertisements into likely HA integration/device-family candidates without requiring a GATT connection. It improves identification while remaining passive and cheap compared with active probing.

### 5.4 Proposed bounded C API

Proposed public names for a future approved implementation:

```c
typedef struct ha_ble_matcher ha_ble_matcher_t;
typedef struct ha_ble_observation ha_ble_observation_t;
typedef struct ha_ble_discovery ha_ble_discovery_t;
typedef struct ha_ble_match_result ha_ble_match_result_t;

esp_err_t ha_ble_matcher_validate(const ha_ble_matcher_t *matcher);
esp_err_t ha_ble_discovery_init(ha_ble_discovery_t *ctx,
                                const ha_ble_matcher_t *matchers,
                                size_t matcher_count,
                                void *history_storage,
                                size_t history_capacity);
esp_err_t ha_ble_discovery_process(ha_ble_discovery_t *ctx,
                                   const ha_ble_observation_t *observation,
                                   ha_ble_match_result_t *results,
                                   size_t result_capacity,
                                   size_t *result_count);
esp_err_t ha_ble_discovery_forget_address(ha_ble_discovery_t *ctx,
                                          const uint8_t address[6],
                                          uint8_t address_type);
```

Public observations should be project-owned bounded views, not NimBLE public types. UUID arrays, service-data entries and manufacturer-data entries must carry explicit counts and maximum accepted counts. Output must be caller-owned and return a truncation/overflow status instead of allocating.

### 5.5 Level-1 APIs composed

For an optional finite scan adapter later: NimBLE GAP discovery APIs plus FreeRTOS timing/cancellation. The matcher engine itself can also accept an observation produced by application-owned native scanning, which avoids creating a second BLE lifecycle manager.

### 5.6 Meaningful Level-2 behavior

Yes: multi-field matching, indexed selection, normalization, bounded history, rediscovery suppression and explicit forgetting are substantially more than native call renaming.

A **universal HA BLE scanner/radio manager is not recommended** because the repository explicitly forbids recreating a generic radio lifecycle abstraction. Applications can own long-running NimBLE scanning; this L2 API should own HA-specific semantics.

### 5.7 ESP32-C6 feasibility and radio limits

High feasibility. NimBLE is already enabled by the foundation and the beta.2 smoke build demonstrated passive scanning on the target. ESP-IDF v6.1 documents Wi-Fi STA + BLE scan coexistence as supported, but they still share one 2.4 GHz RF resource. BLE scanning must not be assumed to receive continuously while other radio work is active.

### 5.8 RAM/flash/buffer implications

- Do not copy HA's Python LRU size of 2048 entries; that is inappropriate for a no-PSRAM C6.
- History capacity must be caller-selected/fixed at compile time; suggested first validation range is 16–64 devices.
- Store compact bit/ID sets for “seen service UUID/service-data UUID/manufacturer-data-present” rather than full advertisement payload history.
- Matcher corpus size is a separate flash-budget decision. Importing the full generated HA corpus verbatim should not be bundled with the engine implementation.
- No heap growth per advertisement.

### 5.9 Authentication/authorization

Passive advertisement matching requires none. Any follow-up GATT connection/read/write is device/integration-specific and can require pairing, bonding, PIN/passkey or application credentials; it must not be implied by a BLE match result.

### 5.10 License/provenance

- Matcher engine: **CLEAN-ROOM REIMPLEMENT** from documented/observed semantics.
- Verbatim generated HA matcher corpus: conservatively classify as **COPY** from Apache-2.0 Home Assistant Core, with provenance/notice requirements.
- Current recommendation is to keep corpus import separate until flash budget and attribution policy are approved.

### 5.11 Disposition

**L2 API — highest priority.**

### 5.12 Required tests and malformed/edge cases

- All matcher dimensions independently and in combination.
- Default connectability behavior and explicit non-connectable matchers.
- Manufacturer ID zero and absent manufacturer data.
- Manufacturer prefix equal/shorter/longer than advertisement value.
- UUID normalization and duplicates.
- Local-name exact/wildcard match; reject wildcard in first three characters.
- Missing/empty name, malformed AD fields, truncated observations.
- Random/private addresses and bounded-history eviction.
- Same address with new service-data UUID / service UUID / changed name triggers reevaluation.
- Separate history for connectable vs non-connectable observations.
- Result-capacity exhaustion is reported without writing out of bounds.

## 6. Candidate B — HA Zeroconf/mDNS matcher and service normalization

### 6.1 Exact upstream source and behavior

`homeassistant/components/zeroconf/discovery.py` normalizes Zeroconf service information, chooses a usable non-link-local/non-unspecified address when available, preserves all addresses, port, hostname, type, instance name and decoded TXT properties, and matches integrations by service type plus optional instance-name glob and TXT-property glob constraints. TXT property values are compared after lowercasing.

The generated current matcher/model corpus is `homeassistant/generated/zeroconf.py`.

### 6.2 Ownership classification

- **HA-defined:** integration matcher/routing semantics and HA service normalization behavior.
- **Consumed:** `python-zeroconf` mDNS/DNS-SD transport/cache/parser.
- **Protocol-defined elsewhere:** DNS-SD/mDNS wire format.

### 6.3 Value to One-OS

After the ESP32-C6 joins Wi-Fi, this can identify a large class of local smart-home devices by advertised service type, instance name and TXT properties without guessing ports or actively probing every host.

### 6.4 Proposed bounded C API

```c
typedef struct ha_mdns_matcher ha_mdns_matcher_t;
typedef struct ha_mdns_service_view ha_mdns_service_view_t;
typedef struct ha_mdns_match_result ha_mdns_match_result_t;

esp_err_t ha_mdns_matcher_validate(const ha_mdns_matcher_t *matcher);
esp_err_t ha_mdns_match(const ha_mdns_matcher_t *matchers,
                        size_t matcher_count,
                        const ha_mdns_service_view_t *service,
                        ha_mdns_match_result_t *results,
                        size_t result_capacity,
                        size_t *result_count);
```

A later, separately approved finite discovery workflow could be:

```c
esp_err_t ha_mdns_discover_once(const ha_mdns_discover_options_t *options,
                                ha_mdns_service_t *services,
                                size_t service_capacity,
                                size_t *service_count);
```

The API must use caller-owned fixed buffers for service names/TXT data or bounded views into parser-owned packets. It must expose truncation explicitly.

### 6.5 Level-1 APIs composed

lwIP UDP/socket APIs and an ESP-IDF-compatible mDNS component/parser where available. FreeRTOS timers/events for a finite query window. The HA family must not wrap another One-OS project family.

### 6.6 Meaningful Level-2 behavior

Yes: DNS-SD record correlation, normalization, service-type routing and multi-field matcher semantics are reusable workflows rather than an API rename.

### 6.7 ESP32-C6 feasibility and radio limits

High once Wi-Fi is associated. mDNS operates over the IP network rather than raw RF scanning. It therefore shares Wi-Fi airtime with BLE/802.15.4 but does not require a special RF mode.

### 6.8 RAM/flash/buffer implications

- DNS names and TXT payloads are attacker-controlled network inputs; enforce packet, record, TXT-key/value and result-count bounds.
- Prefer one packet scratch buffer plus caller-provided result slots.
- Do not retain an unbounded Zeroconf cache. A one-shot browser can use an expiration-bounded fixed table keyed by service type + instance.
- The generated HA Zeroconf corpus can be large and changes frequently; treat corpus selection as a separate flash/versioning problem.

### 6.9 Authentication/authorization

Discovery requires none. A discovered service may later require credentials or protocol-specific pairing; matcher output conveys identity candidates only.

### 6.10 License/provenance

- Engine: **CLEAN-ROOM REIMPLEMENT**.
- Verbatim generated matcher corpus: **COPY** if imported; preserve Apache-2.0 provenance.

### 6.11 Disposition

**L2 API — second priority.**

### 6.12 Required tests and malformed/edge cases

- Exact service-type match and optional name/TXT constraints.
- Case normalization for TXT values.
- Wildcards and empty/missing properties.
- Multiple A/AAAA records, link-local-only services and unusable addresses.
- PTR/SRV/TXT records arriving out of order.
- Duplicate/updated/removed service records.
- Malformed DNS compression pointers, oversized labels/TXT values and packet truncation.
- Fixed table/result-capacity exhaustion.
- Cancellation/timeout of finite discovery with native network state preserved.

## 7. Candidate C — HA HomeKit discovery classification over mDNS

### 7.1 Exact upstream source and behavior

In `homeassistant/components/zeroconf/discovery.py`, HA treats `_hap._tcp.local.` and `_hap._udp.local.` specially. It reads model from TXT `md` or `MD`, tries exact model, then first token split by space or `-`, then wildcard model matchers from the generated HomeKit corpus. `sf == 0` is interpreted as paired.

### 7.2 Ownership classification

- **HA-defined:** model-to-integration routing behavior and the exact fallback order/paired-state interpretation used for HA discovery.
- **HomeKit-defined:** HAP service/TXT fields themselves.

### 7.3 Value to One-OS

Adds more specific product/integration identification to a generic HomeKit mDNS service while still requiring no pairing or connection.

### 7.4 Proposed bounded C API

```c
esp_err_t ha_mdns_homekit_classify(const ha_mdns_service_view_t *service,
                                   const ha_homekit_model_matcher_t *models,
                                   size_t model_count,
                                   ha_homekit_discovery_result_t *out);
```

The result should contain only bounded integration/model identifiers and flags such as `paired_known` / `paired`; it should not expose a HomeKit transport/session object.

### 7.5 Level-1 APIs composed

Same mDNS/lwIP path as Candidate B; no HomeKit session is required.

### 7.6 Meaningful Level-2 behavior

Yes: ordered model normalization/routing and paired-state classification are more than generic mDNS parsing.

### 7.7 ESP32-C6 feasibility and radio limits

High after Wi-Fi association; same coexistence considerations as mDNS.

### 7.8 RAM/flash/buffer implications

Small runtime state. The model corpus may be sizeable; use immutable compact entries and do not copy arbitrary TXT values into unbounded storage.

### 7.9 Authentication/authorization

Discovery/classification requires none. HomeKit pairing/control requires HAP authentication and belongs to a HomeKit protocol implementation, not this classifier.

### 7.10 License/provenance

Engine: **CLEAN-ROOM REIMPLEMENT**. Verbatim model corpus: **COPY** from Home Assistant Core if imported.

### 7.11 Disposition

**L2 API**, but as a narrow `ha_mdns_*` specialization after the general mDNS matcher, not a generic `ha_homekit_*` control stack.

### 7.12 Required tests and malformed/edge cases

- `md` and `MD` precedence.
- Exact model, space-split, hyphen-split and wildcard fallback order.
- Missing/non-string/empty model.
- `sf` missing, zero, nonzero and malformed numeric value.
- Overlong UTF-8/invalid TXT values and bounded output.

## 8. Candidate D — HA SSDP discovery matching and enrichment

### 8.1 Exact upstream source and behavior

`homeassistant/components/ssdp/scanner.py` uses `async_upnp_client` for transport/protocol support. HA adds a device tracker, listener lifecycle, callback registration, periodic scanning, initial scan, multicast M-SEARCH and an IPv4 broadcast M-SEARCH fallback. It indexes integration matchers by selected common fields and requires all matcher fields to be present/equal. It may fetch/cache the UPnP device description referenced by `LOCATION` so matching can include description metadata such as manufacturer/device type.

### 8.2 Ownership classification

- **HA-defined:** matching/indexing policy, scanner lifecycle choices, callback semantics and description enrichment workflow.
- **Consumed:** `async-upnp-client` protocol implementation.
- **UPnP/SSDP-defined elsewhere:** wire format and M-SEARCH/NOTIFY behavior.

### 8.3 Value to One-OS

Strong LAN discovery for TVs, receivers, media devices, bridges and other UPnP/SSDP products. Description enrichment often yields much better product identity than a bare IP/MAC.

### 8.4 Proposed bounded C API

Initial engine:

```c
esp_err_t ha_ssdp_matcher_validate(const ha_ssdp_matcher_t *matcher);
esp_err_t ha_ssdp_match(const ha_ssdp_matcher_t *matchers,
                        size_t matcher_count,
                        const ha_ssdp_service_view_t *service,
                        ha_ssdp_match_result_t *results,
                        size_t result_capacity,
                        size_t *result_count);
```

Later finite discovery, only after separate approval:

```c
esp_err_t ha_ssdp_discover_once(const ha_ssdp_discover_options_t *options,
                                ha_ssdp_service_t *services,
                                size_t service_capacity,
                                size_t *service_count);
```

Description fetching should be an option with explicit maximum body size and timeout; no unbounded XML DOM.

### 8.5 Level-1 APIs composed

lwIP UDP multicast/broadcast sockets, FreeRTOS timers/cancellation and `esp_http_client` (or equivalent Level-1 HTTP client) for bounded `LOCATION` GETs.

### 8.6 Meaningful Level-2 behavior

Yes: active/passive discovery orchestration, case-insensitive header normalization, matcher indexing, deduplication/lifecycle and optional bounded description enrichment.

### 8.7 ESP32-C6 feasibility and radio limits

High on an associated Wi-Fi network. Main constraints are sockets/buffers and shared Wi-Fi airtime, not special RF receive modes.

### 8.8 RAM/flash/buffer implications

- Cap UDP datagram size, header count, header name/value length, tracked devices and callbacks/results.
- Description body must have a hard maximum and streaming/selective XML extraction; never allocate based on `Content-Length` without a cap.
- Cache must be fixed-capacity/TTL-bounded or omitted in a one-shot API.
- Generated HA SSDP matcher corpus should be a separate flash-budget decision.

### 8.9 Authentication/authorization

Discovery and public device-description GET generally do not require authentication. UPnP/SOAP control is a separate active-interaction capability and may have device-specific authorization; it is not proposed here.

### 8.10 License/provenance

Engine: **CLEAN-ROOM REIMPLEMENT**. Verbatim generated matcher corpus: **COPY** if imported. `async-upnp-client` code is **REFERENCE-ONLY** unless separately reviewed and approved; do not route One-OS through it.

### 8.11 Disposition

**L2 API — third priority**, preferably matcher/parser first, active scanner second.

### 8.12 Required tests and malformed/edge cases

- Header names case-insensitive; values exact according to HA matcher semantics.
- Presence-only matcher fields if supported by selected corpus representation.
- M-SEARCH responses plus NOTIFY alive/update/byebye.
- Duplicate USN/location records and updates.
- Missing/invalid `LOCATION` and non-HTTP URLs.
- Oversized/malformed headers and datagrams.
- HTTP timeout, redirect policy, oversized body, malformed XML, duplicate tags and entity-expansion style abuse.
- Result/cache capacity exhaustion and deterministic eviction.
- Cancellation and socket cleanup.

## 9. Candidate E — HA DHCP/client matcher semantics

### 9.1 Exact upstream source and behavior

`homeassistant/components/dhcp/__init__.py` normalizes IPv4/MAC/hostname observations, ignores unusable link-local/loopback/unspecified IPs, suppresses unchanged address data, and matches integrations using hostname fnmatch and MAC/OUI patterns. It indexes OUI-backed matchers separately from hostname-only matchers. HA receives observations from external DHCP watching, network host discovery and device-tracker data.

### 9.2 Ownership classification

- **HA-defined:** normalization/matcher/indexing/rediscovery semantics.
- **Consumed/host-specific:** `aiodhcpwatcher`, `aiodiscover`, router/device-tracker sources.

### 9.3 Value to One-OS

If One-OS already has a trustworthy `(IPv4, MAC, hostname)` client observation, HA's corpus can identify devices that do not advertise richer mDNS/SSDP information.

### 9.4 Proposed bounded C API

Only if a valid observation source exists:

```c
esp_err_t ha_dhcp_match(const ha_dhcp_matcher_t *matchers,
                        size_t matcher_count,
                        const ha_dhcp_client_view_t *client,
                        ha_dhcp_match_result_t *results,
                        size_t result_capacity,
                        size_t *result_count);
```

Do **not** propose `ha_dhcp_scan_start()` in the current target architecture.

### 9.5 Level-1 APIs composed

The matcher itself needs no radio API. A future observation source might use lwIP DHCP client/server hooks, ARP/neighbor information or application-provided router data, but none should be invented merely to make this HA API exist.

### 9.6 Meaningful Level-2 behavior

The corpus-driven hostname/OUI matcher is meaningful, but the acquisition side is weak on a normal ESP32-C6 Wi-Fi STA. Therefore it is not a first implementation target.

### 9.7 ESP32-C6 feasibility and radio limits

Matcher: high feasibility. Whole-LAN passive DHCP watching: low/uncertain in ordinary STA operation because Home Assistant's Linux-host observation model does not transfer directly to an ESP32 station.

### 9.8 RAM/flash/buffer implications

Tiny runtime state if one observation is matched at a time. The generated DHCP corpus consumes flash and should be compacted/curated separately. Hostnames and pattern strings need hard lengths.

### 9.9 Authentication/authorization

None for matching observations. Router APIs that expose DHCP leases may require credentials and are device-specific; they should not be hidden inside this API.

### 9.10 License/provenance

Engine: **CLEAN-ROOM REIMPLEMENT**. Verbatim generated DHCP matcher corpus: **COPY** if imported. External watchers/discovery implementations: **REFERENCE-ONLY**.

### 9.11 Disposition

**APP / DEFER.** Promote to L2 only when One-OS has a legitimate reusable client-observation source; do not create a fake HA “DHCP scanner”.

### 9.12 Required tests and malformed/edge cases

- MAC normalization and OUI/prefix matching.
- Hostname exact/wildcard/case behavior.
- Empty hostname, invalid IP/MAC and registered-device-only entries.
- Same MAC with changed IP/hostname.
- Maximum hostname/pattern lengths and result-capacity exhaustion.

## 10. Candidate F — HA Thread Border Router discovery over mDNS

### 10.1 Exact upstream source and behavior

`homeassistant/components/thread/discovery.py` listens for `_meshcop._udp.local.` and parses Thread Border Router TXT fields such as Border Agent ID, model/network/vendor names, extended address (`xa`), extended PAN ID (`xp`) and Thread version. It maintains known-router state, suppresses identical updates and contains Home Assistant-specific brand/unconfigured heuristics.

### 10.2 Ownership classification

- **HA-defined:** selected normalization, brand mapping, update suppression and Home Assistant-specific unconfigured heuristic.
- **Thread/meshcop-defined elsewhere:** mDNS service and TXT meanings.
- **Consumed:** `python_otbr_api` and Zeroconf.

### 10.3 Value to One-OS

Useful for discovering nearby-LAN Thread Border Routers and identifying vendor/network metadata without entering raw IEEE 802.15.4 mode.

### 10.4 Proposed bounded C API

```c
esp_err_t ha_mdns_thread_router_parse(const ha_mdns_service_view_t *service,
                                      ha_thread_router_info_t *out);
```

No `ha_i154_*` API is justified by this source.

### 10.5 Level-1 APIs composed

Same lwIP/mDNS Level-1 path as Candidate B.

### 10.6 Meaningful Level-2 behavior

Yes as a narrow HA mDNS classifier/normalizer. It should not become a Thread stack abstraction.

### 10.7 ESP32-C6 feasibility and radio limits

High over Wi-Fi IP networking. No raw 802.15.4 receive is required, so it avoids the BLE-scan/802.15.4-scan unsupported coexistence combination.

### 10.8 RAM/flash/buffer implications

Small fixed output structure; all TXT values require fixed maxima/truncation flags. Keep only a caller-chosen number of routers if update tracking is enabled.

### 10.9 Authentication/authorization

Discovery requires none. Reading/changing Thread credentials or commissioning devices is outside this parser and requires protocol-specific authorization.

### 10.10 License/provenance

HA-specific parser/heuristics: **CLEAN-ROOM REIMPLEMENT**. OpenThread/meshcop semantics and `python_otbr_api` implementation: **REFERENCE-ONLY** for this family.

### 10.11 Disposition

**L2 API, low priority**, as a specialization under `ha_mdns_*`. Any actual Thread operational API belongs to the OpenThread project family.

### 10.12 Required tests and malformed/edge cases

- Missing `xa` or `xp` must fail classification cleanly.
- Invalid UTF-8 TXT values.
- Missing/malformed state bitmap.
- Duplicate/update/remove handling.
- Maximum TXT lengths and fixed-capacity router state.

## 11. Candidate G — raw Wi-Fi AP scanning

### 11.1 Exact upstream source and behavior

No Home Assistant Core discovery module inspected defines raw 802.11 RF/AP scanning as a Home Assistant discovery primitive. HA's network discovery work is primarily Zeroconf/mDNS, SSDP and DHCP after IP connectivity. The prior One-OS beta.2 smoke test used `esp_wifi_scan_start()` directly, which is exactly the appropriate Level-1 ownership.

### 11.2 Ownership classification

**Not Home Assistant-defined.** This is ESP-IDF/native Wi-Fi capability.

### 11.3 Value to One-OS

Raw AP data (SSID/BSSID/RSSI/channel/security) is useful to nearby discovery and diagnostics, but it does not become HA-derived simply because HA devices often use Wi-Fi.

### 11.4 Proposed bounded C API

**None in the HA family. Do not create `ha_wifi_scan_*`.**

### 11.5 Level-1 APIs composed

Use native `esp_wifi_scan_start`, `esp_wifi_scan_get_ap_records`/related ESP-IDF APIs directly from applications or a separately justified project family.

### 11.6 Meaningful Level-2 behavior

No HA-specific orchestration was found to justify a wrapper.

### 11.7 ESP32-C6 feasibility and radio limits

Hardware feasibility is already demonstrated. Wi-Fi scan shares the single RF with BLE and IEEE 802.15.4. ESP-IDF v6.1 marks Wi-Fi STA scan + BLE scan supported, while Wi-Fi + IEEE 802.15.4 scan is supported with unstable performance (`C1`).

### 11.8 RAM/flash/buffer implications

Native ESP-IDF scan result storage must still be bounded/cleared correctly by callers, but that is Level-1 resource handling rather than an HA API concern.

### 11.9 Authentication/authorization

Scanning nearby AP advertisements requires no network authentication; joining a network requires legitimate credentials.

### 11.10 License/provenance

**REFERENCE-ONLY** for this HA research; use ESP-IDF native API directly.

### 11.11 Disposition

**DROP from Home Assistant L2; keep as Level 1 / application capability.**

### 11.12 Required tests and malformed/edge cases

Covered by native/application tests rather than HA-family tests: hidden SSIDs, capacity limits, cancellation, state restoration and coexistence load.

## 12. Candidate H — raw IEEE 802.15.4 / Zigbee / Matter scanning

### 12.1 Exact upstream source and behavior

Home Assistant Core does not define a raw IEEE 802.15.4 RF scanner in the sources inspected.

- ZHA's manifest depends on separate `zha`, `zha-quirks`, zigpy and radio adapter libraries.
- HA Thread discovery inspected here is `_meshcop._udp.local.` mDNS discovery, not raw 802.15.4 scanning.
- HA Matter depends on `matter-python-client`/`matter-ble-proxy` and advertises Matter mDNS service types; the operational protocol is separate.
- Prior One-OS beta.2 performed a direct `esp_ieee802154_*` promiscuous receive sweep on channels 11–26. That validates hardware only.

### 12.2 Ownership classification

**Not Home Assistant-defined.** Raw Zigbee/802.15.4 belongs to native ESP-IDF and ZHA/zigpy semantics; Thread belongs to OpenThread; Matter belongs to Matter/CHIP.

### 12.3 Value to One-OS

High potential value for nearby-device discovery, but attribution must remain with the correct project/native layer.

### 12.4 Proposed bounded C API

**No `ha_i154_scan_*`, `ha_zigbee_*` or `ha_matter_*` API from this research.**

### 12.5 Level-1 APIs composed

Native `esp_ieee802154_*` can be used directly for tests/tools or by the independently researched project family that actually defines the higher-level behavior.

### 12.6 Meaningful Level-2 behavior

None found that is genuinely Home Assistant-defined at raw RF level.

### 12.7 ESP32-C6 feasibility and radio limits

The target can receive IEEE 802.15.4 and the old smoke test demonstrated a channel sweep. However ESP-IDF v6.1 documents BLE **scan** concurrent with Thread/Zigbee **scan** as unsupported (`X`). Wi-Fi + 802.15.4 scan is `C1` (supported but unstable). Therefore any future application composition should serialize BLE scan and raw 802.15.4 scan and should not assume stable simultaneous Wi-Fi + 802.15.4 scanning.

### 12.8 RAM/flash/buffer implications

Raw frame capture requires strict fixed packet/result/ring capacities and prompt release of driver RX buffers. Those constraints belong to the native/owning project implementation, not an HA wrapper.

### 12.9 Authentication/authorization

Passive frame reception is distinct from joining/commissioning/control. Zigbee network interaction requires network keys/authorized joining; Thread requires network credentials; Matter commissioning creates authorized fabric credentials. No bypass/credential-harvesting behavior is in scope.

### 12.10 License/provenance

For the HA family: **REFERENCE-ONLY**. Follow the separate ZHA/zigpy, OpenThread and Matter research branches for ownership/license decisions.

### 12.11 Disposition

**OTHER PROJECT / TEST/TOOL**, not Home Assistant L2.

### 12.12 Required tests and malformed/edge cases

Keep existing hardware smoke concepts in test/tool scope: all channels 11–26, driver buffer release, cancellation, coexistence serialization and recovery to prior radio state. Higher-level protocol malformed cases belong to their owning project APIs.

## 13. Active interaction boundary

Phase 1 did not find a single generic “Home Assistant control protocol.” HA integrations call many device/protocol libraries. Therefore discovery matches must never imply that a generic `ha_control()` API exists.

Potential later interaction must stay with the true protocol owner and authorization model:

- BLE: GATT reads/writes only for a specifically researched device/protocol integration; pairing/bonding/app credentials as required.
- HomeKit: HAP pair-setup/pair-verify and authenticated sessions — separate protocol work.
- Matter: commissioning/fabric credentials and operational sessions — Matter project.
- Zigbee: network joining/keys and ZCL/ZDO — ZHA/zigpy or another Zigbee project family.
- UPnP/vendor HTTP: device-specific actions and authentication, not generic HA discovery.

Excluded regardless of upstream availability: deauthentication, credential harvesting, authentication bypass, poisoning, session hijacking, exploit delivery or persistence.

## 14. Cross-cutting ESP32-C6 resource model

### 14.1 Radio scheduling

ESP32-C6 has one shared 2.4 GHz RF path. ESP-IDF v6.1 uses coexistence arbitration/time division. The API design should therefore expose cancellation/timeouts for finite active discovery and allow applications to compose workflows, rather than introducing a universal `ha_radio_manager`.

Recommended composition rule for applications:

1. IP-network discovery (mDNS/SSDP) may run while Wi-Fi STA is connected.
2. BLE scanning may coexist with Wi-Fi but should tolerate reduced receive opportunity.
3. BLE scan and raw IEEE 802.15.4 scan should be serialized because ESP-IDF marks that combination unsupported.
4. Wi-Fi + raw IEEE 802.15.4 scanning should be treated as best-effort/unstable, not a deterministic simultaneous capture mode.

### 14.2 Bounded-memory policy

All future HA APIs should follow these rules:

- caller-owned result arrays;
- fixed/caller-selected history/cache capacities;
- explicit count + capacity parameters;
- explicit truncation/overflow status;
- no unbounded linked lists, maps or per-packet heap growth;
- no full unbounded mDNS/SSDP cache;
- bounded packet/header/TXT/XML parsing;
- immutable matcher corpus in flash when compiled in;
- corpus import/versioning separated from matcher-engine implementation.

### 14.3 Matcher corpus strategy

Home Assistant's generated Bluetooth/Zeroconf/SSDP/DHCP tables are attractive because they encode broad real-world identification knowledge, but blindly copying them into firmware would combine three risks: flash growth, stale generated data and provenance obligations.

Recommended sequence if implementation is later approved:

1. Implement and host-test the HA-compatible matcher engines using a small hand-written fixture corpus.
2. Measure engine RAM/flash on ESP32-C6.
3. Separately build a host-side **TEST/TOOL** that transforms a pinned HA Core generated corpus into a compact C table.
4. Quantify flash cost and matcher coverage before deciding whether to ship full or curated tables.
5. If generated tables are copied, record the pinned HA commit and Apache-2.0 provenance in the generated artifact.

This avoids making a corpus generator part of the runtime API and avoids coupling the HA family to another project family.

## 15. Recommended implementation boundary if Phase 2 is later approved

The smallest high-value first slice is **BLE matcher + bounded match history only**. It should accept HA-family observations and remain independent of a universal scanner manager. A tiny NimBLE-to-observation adapter can be internal only if the approved scope includes a finite scan workflow.

The next slice should be **mDNS matcher/normalizer**, followed by the HomeKit classifier. SSDP should follow after the bounded UDP/header/description-fetch strategy is settled. DHCP should remain deferred until a legitimate observation source exists on this device class.

No production implementation has been started in Phase 1.

## 16. Prioritized proposed API table

| Priority | Proposed API / capability | HA-derived value | Radio/network ownership | Provenance | Disposition |
|---|---|---|---|---|---|
| P0 | `ha_ble_matcher_validate`, `ha_ble_discovery_init`, `ha_ble_discovery_process`, `ha_ble_discovery_forget_address` | Exact HA BLE multi-field matching + bounded rediscovery history | Application/native NimBLE may supply observations; no universal scanner manager | CLEAN-ROOM; matcher corpus separately COPY if imported | **L2 API** |
| P1 | `ha_mdns_matcher_validate`, `ha_mdns_match` | HA Zeroconf service-type/name/TXT matching | lwIP/mDNS Level 1 | CLEAN-ROOM; corpus separately COPY | **L2 API** |
| P1 | `ha_mdns_homekit_classify` | HA HomeKit model routing + paired-state classification | Reuses HA mDNS input, no HAP session | CLEAN-ROOM; model corpus separately COPY | **L2 API** |
| P2 | `ha_ssdp_matcher_validate`, `ha_ssdp_match` | HA SSDP/UPnP integration matcher semantics | lwIP UDP input | CLEAN-ROOM; corpus separately COPY | **L2 API** |
| P3 | `ha_ssdp_discover_once` | Bounded HA-style active/passive search + optional description enrichment | lwIP + bounded HTTP + FreeRTOS | CLEAN-ROOM; external client REFERENCE-ONLY | **L2 API after matcher** |
| P4 | `ha_mdns_thread_router_parse` | HA-specific Thread Border Router normalization/heuristics | mDNS over Wi-Fi, not raw 802.15.4 | CLEAN-ROOM | **L2 API, low priority** |
| Deferred | `ha_dhcp_match` | HA hostname/OUI matcher corpus | Needs a legitimate client observation source | CLEAN-ROOM; corpus separately COPY | **APP / DEFER** |

### Capabilities that should **not** become Home Assistant APIs

- Raw Wi-Fi AP scanning/sniffing: use native ESP-IDF Level 1; do not add `ha_wifi_scan_*`.
- Raw IEEE 802.15.4 channel scanning/sniffing: native/test/tool or the owning Zigbee/Thread project; do not add `ha_i154_scan_*`.
- Zigbee discovery/join/control: ZHA/zigpy or another Zigbee project family, not HA Core semantics.
- Thread operational scanning/joining/network management: OpenThread project family; HA only contributes useful mDNS-side classification here.
- Matter commissioning/control: Matter/CHIP project family; HA's mDNS entries can be consumed by the general mDNS matcher.
- Generic HomeKit pairing/control: HomeKit/HAP protocol implementation, not an HA wrapper.
- Generic UPnP/vendor device control: protocol/device-specific project or application, not `ha_ssdp_*` discovery.
- A universal `ha_radio_manager`, generic observation framework, or compatibility `nearby_*` layer.
