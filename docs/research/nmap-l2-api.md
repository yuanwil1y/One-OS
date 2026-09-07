# Nmap portable Level-2 API — Phase 1 research

Date: 2026-09-07  
Branch: `research/nmap-l2-api`  
Status: **research only; no production implementation in this phase**

## 1. Executive conclusion

A useful Nmap-derived Level-2 API is feasible on One-OS, but it should **not** attempt to embed Nmap itself or reproduce the desktop scanner feature set.

The recommended embedded shape is a small, deterministic scanner family that composes native ESP-IDF/lwIP networking into reusable semantics:

1. bounded host discovery using ICMP Echo first and TCP `connect()` evidence as a fallback;
2. bounded TCP connect port discovery with normalized port states and reasons;
3. a compact service-identification engine using passive banners and a very small set of independently authored protocol probes/matchers;
4. Nmap-specific scan timing, retry, cancellation, progress and partial-result semantics;
5. later, only after hardware validation, narrowly scoped UDP service probes and an experimental ARP-assisted local discovery backend.

The first implementation should deliberately exclude raw SYN/ACK/FIN scans, Nmap OS fingerprinting, NSE, broad Internet scanning, stealth/evasion behavior, and Nmap's shipped databases (`nmap-service-probes`, `nmap-services`, `nmap-os-db`).

This split preserves the main embedded value of Nmap—**probe orchestration, evidence interpretation, service recognition and bounded timing**—without importing host-OS assumptions, a scripting runtime, large databases, or another project API family.

## 2. Repository constraints applied

The root `README.md` establishes that native ESP-IDF, FreeRTOS, lwIP and other platform APIs remain directly usable; wrappers that merely rename native calls are not acceptable. It also requires project API families to be peers with no cross-family calls or public type coupling. See [`../../README.md`](../../README.md).

The branch-specific `AGENTS.md` narrows this further for Nmap:

- L1 remains lwIP/ESP-IDF sockets/network APIs plus FreeRTOS/BSP;
- L2 must add probe orchestration, matching, normalization, timing or result semantics;
- public names use `nmap_*`;
- no NSE/general scripting runtime;
- Phase 1 is research only;
- production work must wait for explicit approval.

See [`../../AGENTS.md`](../../AGENTS.md).

The branch CI currently builds ESP32-C6 with **ESP-IDF v6.1**. The board baseline assumes **8 MB flash, no PSRAM**, while the factory application partition is 3 MB. These bounds argue strongly against importing desktop-scale signature databases or retaining a result matrix for every target × port in RAM.

## 3. Upstream Nmap behavior inspected

Research was based on the current stable Nmap release **7.991** (published 2026-08-06) plus current upstream documentation/source inspected on 2026-09-07.

Primary upstream references:

- Nmap download/release baseline: <https://nmap.org/download>
- Host discovery: <https://nmap.org/book/man-host-discovery.html>
- Port scanning techniques: <https://nmap.org/book/man-port-scanning-techniques.html>
- Port-state model: <https://nmap.org/book/man-port-scanning-basics.html>
- Scan algorithms/timing: <https://nmap.org/book/port-scanning-algorithms.html>
- Performance/timing controls: <https://nmap.org/book/man-performance.html>
- Service/version detection: <https://nmap.org/book/man-version-detection.html>
- Version-detection technique: <https://nmap.org/book/vscan-technique.html>
- `nmap-service-probes` format: <https://nmap.org/book/vscan-fileformat.html>
- NPSL: <https://nmap.org/npsl/>
- Nmap source mirror: <https://github.com/nmap/nmap>
- Scan engine: <https://github.com/nmap/nmap/blob/master/scan_engine.cc>
- Target/ARP discovery: <https://github.com/nmap/nmap/blob/master/targets.cc>
- Service scanner: <https://github.com/nmap/nmap/blob/master/service_scan.cc>

### 3.1 Host discovery semantics worth preserving

Nmap's host discovery does not equate “no ICMP reply” with “host down”. It combines evidence from multiple probe types. On local Ethernet it normally prefers ARP; privileged hosts can use raw TCP/ICMP techniques, while unprivileged operation can fall back to TCP `connect()` probes.

The important portable semantic is **evidence aggregation**, not the exact Nmap default probe set.

For an embedded LAN device, a successful TCP connection proves the host is alive. Equally important, a fast `ECONNREFUSED` also proves that a responding TCP/IP stack exists even though that particular port is closed. A timeout does not prove that the target is absent because filtering and packet loss are possible.

### 3.2 Port-state semantics worth preserving

Nmap models port states as scanner observations, including `open`, `closed`, `filtered`, and ambiguous states such as `open|filtered` for UDP. This is valuable L2 behavior because native socket error codes alone are awkward for applications.

For a portable TCP connect scan on ESP-IDF:

| Native observation | Proposed normalized state | Host evidence |
|---|---|---|
| `connect()` succeeds | `NMAP_PORT_OPEN` | alive |
| `ECONNREFUSED` | `NMAP_PORT_CLOSED` | alive |
| deadline expires | `NMAP_PORT_FILTERED` / no-response reason | unknown |
| `EHOSTUNREACH` / `ENETUNREACH` | `NMAP_PORT_UNREACHABLE` | unreachable evidence |
| other local/socket error | `NMAP_PORT_ERROR` | no inference |

The state must retain a separate `reason` so applications can distinguish “timed out”, “route unavailable”, “locally resource constrained”, and other causes.

### 3.3 Timing semantics worth preserving

Nmap's scan engine (`ultra_scan`) manages concurrency, RTT estimates, retransmissions, packet loss and scan deadlines across targets rather than issuing isolated network calls. That orchestration is genuine Level-2 value.

One-OS should preserve the ideas of:

- explicit per-probe timeout;
- bounded retry count;
- bounded in-flight probes;
- per-host/global deadline;
- progress accounting;
- explicit cancellation;
- streaming partial results;
- optional conservative latency adaptation constrained by configured minimum/maximum values.

It should **not** reproduce Nmap timing templates intended for aggressive, stealthy or evasion-oriented network scanning.

### 3.4 Service/version detection semantics worth preserving

Nmap service detection selects probes, sends protocol bytes, observes replies and matches those replies against signatures. It supports “hard” matches, softer narrowing matches, rarity ordering, fallback and per-probe wait limits.

The portable embedded value is the pipeline:

`candidate service -> low-cost probe -> bounded response capture -> matcher -> normalized service metadata -> confidence/evidence`

The Nmap database and regex corpus are not required to preserve that architecture.

## 4. ESP32-C6 / ESP-IDF feasibility

### 4.1 TCP connect scanning — feasible, P0

ESP-IDF/lwIP provides BSD socket behavior (`socket`, non-blocking `connect`, `select`, `getsockopt(SO_ERROR)`, `close`, `send`, `recv`). A small worker set can therefore scan several endpoints concurrently without raw TCP packet construction.

This is the strongest Phase-2 candidate because it is portable, requires no privileged host feature, maps cleanly to Nmap's connect-scan behavior, and produces meaningful normalized states.

**Native calls composed:** BSD sockets, `fcntl`, `select`, `getsockopt`, monotonic timing, FreeRTOS task/event primitives.

**Why it is L2:** the API would own scheduling, deadlines, socket-error interpretation, retries, result normalization, cancellation and streaming progress; it is not a one-call socket wrapper.

### 4.2 ICMP Echo host discovery — feasible, P0

ESP-IDF has an ICMP Echo/ping session API (`esp_ping_*`) and examples support Wi-Fi targets. It is suitable as the cheapest first probe for a local-LAN discovery strategy.

ICMP alone must not decide “down”. The L2 orchestrator should fall back to one or more configured TCP connect probes when ICMP receives no response.

**Native calls composed:** ESP-IDF ICMP Echo API plus timing/cancellation state.

**Why it is L2:** multiple probe methods are combined into one host observation with evidence flags, RTT and a partial/unknown outcome when appropriate.

### 4.3 ARP local discovery — technically plausible, but experimental on this board

Nmap prefers ARP on directly connected Ethernet because it is efficient and strongly identifies local hosts. The Waveshare target is Wi-Fi based, and ESP-NETIF's documented L2 TAP interface currently states that only Ethernet (IEEE 802.3) is supported. Therefore Nmap's ordinary raw-Ethernet ARP path cannot simply be transplanted to Wi-Fi STA.

esp-lwIP exposes ARP internals such as `etharp_request` / ARP table helpers, and ESP-NETIF can expose the underlying lwIP netif to stack internals. This suggests a possible implementation path, but it has three problems:

1. it is not the same documented application-facing interface as BSD sockets;
2. lwIP non-socket APIs have TCP/IP-thread/core-locking constraints;
3. Wi-Fi behavior and ARP-cache size/eviction must be proven on ESP-IDF v6.1 hardware, not assumed from desktop Nmap or Ethernet examples.

**Disposition in Phase 1:** `TEST/TOOL`, P1 feasibility experiment only. Do not make ARP a required P0 backend or public guarantee until an on-device prototype proves safe public-API usage and deterministic MAC retrieval.

ESP-IDF references:

- lwIP guide: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/lwip.html>
- ICMP Echo example: <https://github.com/espressif/esp-idf/tree/master/examples/protocols/icmp_echo>
- ESP-NETIF L2 TAP example (documents Ethernet-only limitation): <https://github.com/espressif/esp-idf/tree/master/examples/protocols/l2tap>
- ESP-NETIF network-stack interface: <https://github.com/espressif/esp-idf/blob/master/components/esp_netif/include/esp_netif_net_stack.h>
- esp-lwIP ARP declarations: <https://github.com/espressif/esp-lwip/blob/2.2.0-esp/src/include/lwip/etharp.h>

### 4.4 Raw TCP SYN/ACK/FIN scanning — possible in principle, not justified for P0/P1

lwIP has raw capabilities, but a useful Nmap-style raw TCP scanner also needs packet construction/checksums, source/destination tuple correlation, retransmission rules, RST handling, ICMP error handling, concurrency timing, and careful interaction with the host TCP/IP stack. Those costs are disproportionate on the current no-PSRAM embedded baseline.

TCP connect already provides the main legitimate LAN inventory value with much less fragility.

**Disposition:** `DROP` from initial product API; at most future `TEST/TOOL` research if a concrete capability gap appears.

### 4.5 UDP scanning — feasible only as a constrained service-aware feature

Generic UDP scanning is inherently ambiguous: a UDP reply establishes `open`, an ICMP port-unreachable can establish `closed`, while silence can mean either open or filtered. ICMP rate limiting also causes slow/uncertain scans.

One-OS should not send empty packets across a wide UDP port range. A later implementation should pair a **small curated set of harmless protocol requests** with response matchers. If ESP-IDF/lwIP cannot reliably correlate ICMP unreachable errors to a UDP probe on hardware, silence must remain `NMAP_PORT_OPEN_OR_FILTERED` rather than being guessed closed.

**Disposition:** P1 `L2 API`, protocol-aware and bounded only.

## 5. Proposed public API shape (research sketch, not an implementation contract)

The API should use ordinary C data with bounded fields and no public types from another project API family.

```c
typedef struct nmap_scan *nmap_scan_handle_t;

typedef enum {
    NMAP_TRANSPORT_TCP,
    NMAP_TRANSPORT_UDP,
} nmap_transport_t;

typedef enum {
    NMAP_HOST_UP,
    NMAP_HOST_NO_RESPONSE,
    NMAP_HOST_UNREACHABLE,
    NMAP_HOST_ERROR,
} nmap_host_state_t;

typedef enum {
    NMAP_PORT_OPEN,
    NMAP_PORT_CLOSED,
    NMAP_PORT_FILTERED,
    NMAP_PORT_OPEN_OR_FILTERED,
    NMAP_PORT_UNREACHABLE,
    NMAP_PORT_ERROR,
} nmap_port_state_t;

typedef enum {
    NMAP_REASON_NONE,
    NMAP_REASON_REPLY,
    NMAP_REASON_CONNECTION_ACCEPTED,
    NMAP_REASON_CONNECTION_REFUSED,
    NMAP_REASON_TIMEOUT,
    NMAP_REASON_HOST_UNREACHABLE,
    NMAP_REASON_NETWORK_UNREACHABLE,
    NMAP_REASON_CANCELLED,
    NMAP_REASON_LOCAL_RESOURCE,
    NMAP_REASON_IO_ERROR,
} nmap_reason_t;

typedef struct {
    uint16_t max_inflight;
    uint8_t max_retries;
    uint32_t probe_timeout_ms;
    uint32_t host_timeout_ms;
    uint32_t max_probes_per_second;
} nmap_timing_policy_t;

typedef struct {
    uint16_t max_targets;
    uint16_t max_ports_per_target;
    uint16_t max_service_probes_per_port;
    uint16_t max_capture_bytes;
} nmap_limits_t;
```

Candidate lifecycle:

```c
esp_err_t nmap_discovery_start(const nmap_discovery_config_t *config,
                               nmap_result_cb_t callback,
                               void *user_ctx,
                               nmap_scan_handle_t *out_scan);

esp_err_t nmap_port_scan_start(const nmap_port_scan_config_t *config,
                               nmap_result_cb_t callback,
                               void *user_ctx,
                               nmap_scan_handle_t *out_scan);

esp_err_t nmap_service_scan_start(const nmap_service_scan_config_t *config,
                                  nmap_result_cb_t callback,
                                  void *user_ctx,
                                  nmap_scan_handle_t *out_scan);

esp_err_t nmap_scan_cancel(nmap_scan_handle_t scan);
esp_err_t nmap_scan_wait(nmap_scan_handle_t scan, uint32_t timeout_ms);
void nmap_scan_destroy(nmap_scan_handle_t scan);
```

Results should be **streamed** through callbacks/events and optionally copied by the application. The Nmap component should not allocate a full target-by-port result matrix.

A host result should carry at least:

- target address;
- normalized host state;
- evidence flags (ICMP reply, TCP accepted, TCP refused, UDP reply, later ARP reply if validated);
- best/last RTT where meaningful;
- probe counts and completion/partial status.

A port result should carry:

- address, transport and port;
- normalized state and reason;
- elapsed time/retry count;
- whether the observation also proves host reachability.

A service result should carry bounded metadata such as:

- normalized service identifier/name;
- optional product/version/banner text;
- confidence level;
- transport/port;
- matcher/probe identifier for provenance/debugging;
- truncation flags when capture limits are reached.

Host-level product/device classification (“this is a printer/router/brand X”) should remain **APP** logic unless it is directly and reliably declared by a protocol response. The Nmap L2 family should return evidence, not resurrect the old recognition/product-semantics layer.

## 6. Candidate APIs and detailed disposition

### 6.1 Bounded host discovery

**Exact upstream behavior referenced:** Nmap combines ICMP/TCP/UDP/ARP/ND evidence and treats responsive TCP refusal as proof of a live host. Local Ethernet normally gets ARP discovery.

**Network prerequisite:** interface has a configured IP route; intended initial scope is explicit IPv4 targets or the directly connected private LAN.

**Product value:** quickly reduces a LAN target set before heavier service scanning while preserving uncertainty instead of incorrectly hiding filtered hosts.

**Proposed API:** `nmap_discovery_start()`, `nmap_host_result_t`, `nmap_discovery_config_t`.

**Native calls composed:** `esp_ping_*`, BSD TCP sockets, FreeRTOS/timing primitives; later optional validated ARP backend.

**Why L2:** adaptive/fallback probe plan, evidence aggregation, normalized host state, retry/deadline/cancel semantics.

**Privilege/raw feasibility:** P0 path requires no raw TCP. ICMP is provided by ESP-IDF. ARP is not required for P0.

**Timeout/retry/concurrency:** proposed initial default: ICMP first; only no-response targets receive TCP fallbacks; 4 in flight by default, hard initial cap 8; at most one retry; global rate cap. All values remain configurable within compiled safety ceilings.

**RAM/flash/bounds:** stream results; max 256 targets in the initial local-CIDR convenience path; no retained target matrix.

**Network load:** one low-cost ICMP probe per target plus conditional TCP fallbacks. No “probe every Nmap ping type” behavior.

**Authorization:** local/owned/administered networks or targets for which the operator has permission.

**Provenance:** `CLEAN-ROOM REIMPLEMENT` of behavior using native ESP-IDF/lwIP and protocol standards; Nmap source is `REFERENCE-ONLY`.

**Disposition:** `L2 API`, P0.

**Tests / false positives:** ICMP-enabled host; ICMP-blocked host with open TCP; ICMP-blocked host with closed TCP (`ECONNREFUSED` still proves up); nonexistent address timeout; route-unreachable target; packet loss; duplicate target; cancellation; network disconnect mid-scan. Never interpret timeout alone as definitively down.

### 6.2 TCP connect port discovery

**Exact upstream behavior referenced:** Nmap connect scan delegates connection establishment to the operating system rather than crafting raw SYN packets; Nmap then interprets connection results as port states.

**Network prerequisite:** routed IPv4/IPv6 socket reachability to explicit target.

**Product value:** identify reachable LAN services without privileged/raw packet machinery.

**Proposed API:** `nmap_port_scan_start()`, `nmap_port_result_t`, explicit port list/profile.

**Native calls composed:** non-blocking BSD TCP sockets, `select()`, `getsockopt(SO_ERROR)`, `close()`.

**Why L2:** multi-target/port scheduler, state/reason normalization, timeout/retry policy, cancellation/progress.

**Privilege/raw feasibility:** no raw privileges needed.

**Timeout/retry/concurrency:** default 4 concurrent connections; first-version hard cap 8 until heap/socket measurements justify more; per-probe deadline proposed around a conservative LAN value and always bounded by config; max one retry.

**RAM/flash/bounds:** initial convenience profile should be small (target default <=64 TCP ports; explicit list can have a separately documented hard ceiling). Stream results.

**Network load:** full TCP handshakes may be logged by target services. Rate limiting is mandatory. No stealth semantics.

**Authorization:** explicit/local authorized targets.

**Provenance:** `CLEAN-ROOM REIMPLEMENT`; do not port `ultra_scan` source.

**Disposition:** `L2 API`, P0.

**Tests / false positives:** open listener; immediately refused closed port; firewall-drop timeout; target accepts then immediately resets; route loss; socket exhaustion; cancellation; reused file descriptors; slow service that accepts before app data. A successful connect remains `open` even if the application subsequently closes immediately.

### 6.3 Compact service identification

**Exact upstream behavior referenced:** Nmap selects service probes, captures replies, matches signatures, narrows subsequent probes after partial matches, and can record service/product/version/device metadata.

**Network prerequisite:** port is open or deliberately selected for a service probe.

**Product value:** converts “TCP 22 open” into useful public metadata such as SSH/HTTP service identity and safe banner/version details.

**Proposed API:** `nmap_service_scan_start()`, `nmap_service_result_t`, internally compiled probe descriptors and matchers.

**Native calls composed:** TCP/UDP sockets; optional later TLS via native ESP-IDF/mbedTLS facilities.

**Why L2:** probe selection, response bounding, protocol parsing/matching, confidence and fallback semantics.

**Privilege/raw feasibility:** no raw packet requirement for initial TCP services.

**Timeout/retry/concurrency:** service probes run only on selected/open ports; each probe has a bounded wait; cap probe count per port; reuse the scan-wide cancellation/deadline policy.

**RAM/flash/bounds:** proposed first cap <=512–1024 captured bytes per worker, <=3 service probes per port, <=4 workers. Prefer compiled prefix/token parsers over PCRE or a runtime regex language.

**Network load:** banner-first where possible; otherwise one minimal standards-compliant request. Avoid commands that authenticate, change state, enumerate protected data or create substantial server work.

**Authorization:** only public/authorized service interrogation.

**Provenance:** probe bytes and matchers must be independently authored from RFCs, vendor documentation, controlled test captures and interoperability tests. Nmap's database is `REFERENCE-ONLY`.

**Disposition:** `L2 API`, P0 with a very small service set; grow only per reviewed protocol.

**Initial safe candidates:** passive SSH/FTP/SMTP-style greeting classification; minimal HTTP request/response parsing; later a narrow DNS query probe. TLS certificate/service inspection is a P1 option after RAM/flash measurement.

**Tests / false positives:** protocol on non-standard port; generic/misleading banner; truncated reply; binary reply; server sends a generic HTTP error; immediate close; delayed banner; banner claims a product/version that differs from reality. Results need confidence/evidence rather than assuming every banner is authoritative.

### 6.4 Service-aware UDP discovery

**Exact upstream behavior referenced:** Nmap classifies UDP reply as open, ICMP port-unreachable as closed, and no response as `open|filtered`, with retries constrained by ICMP rate limiting.

**Network prerequisite:** a defined, harmless protocol request and a target/port authorized for interrogation.

**Product value:** LAN services that are predominantly UDP can otherwise be missed.

**Proposed API:** reuse `nmap_service_scan_start()`/`nmap_port_result_t` with `NMAP_TRANSPORT_UDP`; do not expose a generic “spray all UDP ports” API as the default path.

**Native calls composed:** UDP sockets, `select`, receive/error handling; optional ICMP-error correlation only after hardware proof.

**Why L2:** protocol-aware probe selection and ambiguous-state normalization.

**Privilege/raw feasibility:** sending/receiving UDP is straightforward; reliable ICMP-unreachable correlation must be validated on ESP-IDF v6.1.

**Timeout/retry/concurrency:** low rate; normally zero/one retry; explicit `OPEN_OR_FILTERED` on silence.

**RAM/flash/bounds:** same bounded capture/matcher scheme as TCP service scan.

**Network load:** only explicit small probe sets; no all-ports UDP scan.

**Authorization:** required.

**Provenance:** `CLEAN-ROOM REIMPLEMENT`; protocol request bytes from standards, not Nmap database.

**Disposition:** `L2 API`, P1.

**Tests / false positives:** actual UDP response; ICMP-unreachable behavior; silent open service; filtering; delayed response; ICMP rate limiting; duplicate/out-of-order datagrams.

### 6.5 ARP-assisted local discovery

**Exact upstream behavior referenced:** Nmap prefers ARP on directly connected Ethernet and correlates ARP replies/MAC addresses with local hosts.

**Network prerequisite:** target is on the same IPv4 L2 segment.

**Product value:** potentially fastest/reliable local-host discovery plus MAC evidence.

**Proposed API:** no separate public API initially; optional backend/evidence flag under `nmap_discovery_start()` only if validated.

**Native calls composed:** likely esp-lwIP ARP/netif APIs, not ESP-NETIF L2 TAP on Wi-Fi.

**Why L2:** only if it becomes part of bounded discovery orchestration; raw ARP-table access by itself is L1/platform plumbing.

**Privilege/raw feasibility:** ESP-NETIF L2 TAP is documented Ethernet-only. lwIP ARP internals are technically available but have thread/core and API-stability concerns.

**Timeout/retry/concurrency:** one/few requests at a time; consume replies/table entries immediately; do not assume the ARP cache can retain an entire /24.

**RAM/flash/bounds:** tiny, but ARP-table capacity and eviction are functional constraints.

**Network load:** one ARP request per target is modest on a local /24, but must remain rate-limited.

**Authorization:** same local-LAN authorization requirements.

**Provenance:** Nmap behavior `REFERENCE-ONLY`; any eventual implementation uses native lwIP interfaces and is `CLEAN-ROOM REIMPLEMENT`.

**Disposition:** `TEST/TOOL` P1 feasibility gate; promote to `L2 API` only after on-device validation.

**Tests / false positives:** Wi-Fi AP client isolation; ARP cache eviction; target changes MAC/IP; gateway/proxy ARP; duplicate IP; roam/reconnect; core-locking assertions; repeated /24 sweep.

## 7. License and provenance decision

This is a hard architecture constraint, not a documentation footnote.

Nmap is distributed under the **Nmap Public Source License (NPSL)**. The annotated license explicitly states that software which reads or includes Nmap data files such as `nmap-os-db` or `nmap-service-probes` is considered derivative work for the purposes of that license, and the project also offers a commercial OEM license for embedding Nmap technology into products.

Therefore the safe Phase-1 provenance policy is:

| Material | Provenance | Decision |
|---|---|---|
| Nmap documentation / observed behavior | `REFERENCE-ONLY` | allowed for research/design |
| Nmap C/C++ implementation (`scan_engine`, `service_scan`, etc.) | `REFERENCE-ONLY` | do not copy/port |
| `nmap-service-probes` | `REFERENCE-ONLY` | do not ship/read/copy |
| `nmap-services` frequency data | `REFERENCE-ONLY` | do not ship/read/copy |
| `nmap-os-db` / OS fingerprints | `REFERENCE-ONLY` | do not ship/read/copy |
| One-OS orchestration/state model | `CLEAN-ROOM REIMPLEMENT` | candidate production path after approval |
| Protocol probes authored from standards/vendor docs | `CLEAN-ROOM REIMPLEMENT` | candidate production data after review |
| lwIP/ESP-IDF native APIs | native platform use | L1 dependency, not Nmap-derived code |

No candidate in this research recommends `COPY` or direct `PORT` of Nmap code/data.

If a future requirement genuinely needs Nmap's signature databases or implementation, that should trigger a separate legal/OEM licensing decision before engineering work.

## 8. Proposed timing, memory and load policy

These are starting bounds for Phase-2 validation, not final constants.

### 8.1 Initial safety ceilings

- local convenience target generation: at most 256 addresses (typically one /24); broader ranges require explicit application-supplied target lists and a future policy decision;
- default worker concurrency: 4;
- first implementation hard concurrency ceiling: 8 until heap/socket measurements prove more is safe;
- retry ceiling: 1;
- default TCP port profile: <=64 entries;
- service probes per port: <=3;
- captured service response per worker: 512–1024 bytes maximum;
- default global probe rate: conservative and explicitly capped (initial engineering target around tens of probes/second, not hundreds/thousands);
- every scan has a cancellation path and finite deadline.

### 8.2 RAM model

Use streaming callbacks/events. Keep only:

- the bounded target iterator/list supplied by the caller;
- a small fixed worker/socket set;
- one small capture buffer per service worker;
- compact pending-job and timer state;
- no full target × port × service matrix;
- no runtime regex database.

Because the production baseline assumes no PSRAM and LVGL/radios may be active concurrently, Phase 2 must measure free heap and high-water marks with UI + Wi-Fi active rather than trusting theoretical socket limits.

### 8.3 Flash model

Do not import Nmap's large data files. Service matchers should be small compiled protocol modules/data. Every new matcher/probe family should have a size delta recorded during review so the 3 MB application partition remains an explicit constraint.

### 8.4 Conservative traffic behavior

The implementation should prefer a staged plan:

1. cheap host evidence;
2. only discovered/explicit hosts receive port probes;
3. only selected/open ports receive service probes;
4. protocol probes stop after a strong match;
5. cancellation/deadline stops pending work immediately.

This is both safer for the LAN and more suitable for ESP32-C6 resources than cloning Nmap's desktop breadth.

## 9. Previous One-OS / NearBy material inspected

The clean One-OS foundation intentionally removed the old scanner/discovery architecture, `radio_runtime`, scan session/coordinator layers, compatibility APIs and product-recognition semantics before introducing new Level-2 families. That is an architectural instruction not to revive those layers through Nmap.

The archived beta.2 smoke source was also inspected. Its Wi-Fi test initializes Wi-Fi STA mode and performs an **802.11 AP scan**, along with BLE/802.15.4/SD smoke tests. It does not implement LAN host discovery, TCP port discovery or Nmap-like service matching. It is useful only as evidence that the native radio/platform baseline has been exercised.

Conclusion: there is **no current reusable LAN/Nmap implementation in the clean history that should be ported**. Phase 2, if approved, should start as an independent `nmap_*` component against native ESP-IDF/lwIP.

## 10. Exclusions and non-goals

The following should remain outside the first Nmap L2 implementation:

- **NSE/general scripting runtime — DROP.** Explicitly excluded by `AGENTS.md`; too large and broad for the embedded boundary.
- **Raw SYN/ACK/FIN/Xmas/NULL TCP scans — DROP initially.** Low incremental product value versus connect scanning, much higher packet/control complexity.
- **OS fingerprinting / `nmap-os-db` — DROP.** Requires crafted raw probes, a large/licensed fingerprint database, and has substantial false-positive/maintenance cost.
- **Nmap service/signature databases — DROP as shippable DATA.** NPSL provenance risk and size cost.
- **Nmap top-port frequency database — DROP.** Build independent small port profiles from standards/product requirements instead.
- **Stealth, firewall evasion, decoys, spoofing, timing-evasion templates — DROP.** Outside product/safety boundary.
- **Brute force, credential guessing, auth bypass, exploit/vulnerability delivery — DROP.** Explicit safety boundary.
- **Broad arbitrary Internet/CIDR scanning — APP/policy exclusion.** Initial API is designed for local/explicit authorized targets.
- **Host device/product recognition database — APP/DATA, not Nmap L2.** The L2 API returns network/service evidence; product semantics belong above it.
- **Generic DNS/name wrappers — DROP as L2.** Native resolver APIs remain directly usable unless name resolution becomes a meaningful part of a future Nmap-specific orchestration.

## 11. Validation plan for an approved Phase 2

No production code is written in Phase 1. If approved later, the first implementation should be gated by the following tests.

### 11.1 Host-side deterministic tests

Use controlled local listeners/firewall fixtures to test:

- TCP open / refused / timeout / unreachable mapping;
- partial results and event ordering;
- cancellation while sockets are pending;
- worker concurrency never exceeds configured limit;
- retry ceiling and overall deadline;
- misleading/truncated service banners;
- parser bounds and malformed/binary inputs.

### 11.2 ESP32-C6 hardware tests

On the Waveshare board with ESP-IDF v6.1:

- Wi-Fi STA connected to a controlled LAN;
- ICMP success, loss and blocked-ICMP fallback;
- non-blocking TCP connect and `SO_ERROR` mapping;
- repeated bounded scans without fd/heap leaks;
- heap high-water mark with LVGL/UI active;
- network disconnect/reconnect during scan;
- cancellation latency;
- UDP response/error behavior before enabling UDP state claims;
- dedicated ARP experiment verifying whether public/supported interfaces are sufficient on Wi-Fi, with core-lock checks enabled where available.

### 11.3 False-positive discipline

Every result state must distinguish observation from inference:

- no reply != host down;
- TCP refused != host down;
- UDP silence != closed;
- a banner string != verified software identity;
- proxy ARP != proof of the end host's MAC;
- a service on a conventional port != that conventional service without protocol evidence.

## 12. Prioritized API table

| Priority | Candidate | Proposed surface | Provenance | Disposition | Main feasibility note |
|---|---|---|---|---|---|
| **P0** | Bounded host discovery | `nmap_discovery_start`, `nmap_host_result_t` | `CLEAN-ROOM REIMPLEMENT` | **L2 API** | ICMP + TCP-connect evidence is portable; timeout remains uncertain |
| **P0** | TCP connect port scan | `nmap_port_scan_start`, normalized state/reason | `CLEAN-ROOM REIMPLEMENT` | **L2 API** | Strongest ESP-IDF fit; no raw privilege required |
| **P0** | Scan timing/cancel/progress | `nmap_timing_policy_t`, `nmap_scan_cancel/wait` | `CLEAN-ROOM REIMPLEMENT` | **L2 API** | Core reusable value; hard bounds required |
| **P0** | Small TCP service fingerprint set | `nmap_service_scan_start`, `nmap_service_result_t` | `CLEAN-ROOM REIMPLEMENT` | **L2 API** | Independent probes/matchers only; bounded capture, no regex runtime |
| **P1** | Service-aware UDP probes | same service/port result family | `CLEAN-ROOM REIMPLEMENT` | **L2 API** | Preserve `open|filtered` ambiguity; ICMP error correlation must be tested |
| **P1** | ARP-assisted local discovery | optional discovery backend/evidence | `REFERENCE-ONLY` + native lwIP experiment | **TEST/TOOL** first | ESP-NETIF L2 TAP is Ethernet-only; Wi-Fi path needs hardware proof |
| **P1** | TLS metadata/service continuation | optional service probe | `CLEAN-ROOM REIMPLEMENT` | **L2 API** after sizing | mbedTLS/heap/flash impact must be measured |
| — | Small independent port profile | internal/static profile | independent standards/product data | **DATA** | Do not copy Nmap top-port frequency data |
| — | Host/device classification | app inference over L2 evidence | independent | **APP** | Avoid restoring product-recognition database into Nmap component |
| — | Nmap databases | none | `REFERENCE-ONLY` | **DROP** | NPSL/size/maintenance constraints |
| — | OS fingerprinting | none | `REFERENCE-ONLY` | **DROP** | Raw probes + fingerprint DB + false positives |
| — | Raw SYN/ACK/FIN/etc. scans | none initially | `REFERENCE-ONLY` | **DROP** / future test only | Connect scan covers initial legitimate LAN value |
| — | NSE / scripting | none | `REFERENCE-ONLY` | **DROP** | Explicitly excluded |

## 13. Feasibility limits and final recommendation

### Feasible now, with normal ESP-IDF networking

- ICMP-Echo-based host evidence;
- TCP-connect host fallback and port scanning;
- bounded TCP service banner/protocol interrogation;
- normalized Nmap-like host/port/service results;
- deterministic timing/retry/cancel/progress orchestration.

### Feasible only after a hardware/API gate

- UDP closed-state inference based on ICMP errors;
- ARP request/reply/MAC discovery through lwIP on Wi-Fi STA;
- TLS service/certificate metadata within acceptable RAM/flash budget.

### Not recommended for this platform/product boundary

- a full Nmap port of any kind;
- Nmap database import;
- OS fingerprint emulation;
- raw stealth scan families;
- NSE;
- scan-evasion or offensive extensions.

**Recommendation for approval decision:** if Phase 2 is authorized, approve only the P0 slice first: `nmap_discovery_*` (ICMP + TCP evidence), `nmap_port_scan_*` (TCP connect), shared bounded scan lifecycle/timing, and a tiny independently authored TCP service-probe set. Keep UDP, ARP and TLS behind separate validation gates. This yields meaningful reusable Level-2 capability while staying faithful to One-OS's native-first architecture and ESP32-C6 resource limits.
