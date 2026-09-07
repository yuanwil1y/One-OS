# Matter / CHIP Level-2 Controller

This component is the One-OS Matter controller family for ESP32-C6. It uses Espressif `esp-matter` and its pinned connectedhomeip controller implementation directly; it is not a port of the Linux `chip-tool` executable.

## Upstream provenance

- esp-matter: `espressif/esp-matter@d1a7edcf22d4af34e0cb994de81f5f23f55993bc`
- connectedhomeip: the recursive Espressif submodule pinned by that esp-matter commit
- licenses: upstream Apache-2.0; One-OS adapter code follows the repository license policy

`third_party/esp-matter` is a git submodule. The firmware CMake file adds the official ESP32 Matter components exactly through `connectedhomeip/config/esp32/components` and `esp-matter/components`.

## Ownership boundary

Owned here:

- controller/fabric lifecycle and persisted operational state;
- CASE to already-authorized operational nodes;
- bounded Attribute Read and Node Probe;
- Write, Invoke, Subscribe/Unsubscribe;
- explicit user-triggered commissioning over an already-known IP peer or BLE commissioning transport.

Not owned here:

- generic BLE scanning;
- mDNS/DNS-SD environment scanning;
- Matter candidate discovery/matching;
- Thread network management;
- HA Device/Entity state or recognition database logic.

On-network commissioning therefore accepts a concrete peer IP/port instead of calling `DiscoverCommissionableNodes`. BLE APIs use connectedhomeip BLE central transport only after an explicit commissioning request.

## Bounded memory contract

Application-owned result accumulation is fixed capacity:

- 8 endpoints per node probe;
- 4 Device Types per endpoint;
- 24 Server Clusters per endpoint;
- 8 Parts per endpoint;
- 128 bytes per generic scalar/string/bytes read value;
- 4 in-flight one-shot IM operations;
- 2 subscriptions;
- 384 bytes per JSON Write/Invoke payload;
- 4 PAA certificates, 640 bytes each;
- NOC/ICAC/RCAC output buffers are 600 bytes each.

Descriptor lists use the lower-level `ReadClient::Callback` path. One-OS streams `ReplaceAll` array elements and `AppendItem` reports directly into the fixed structures above. It does not use connectedhomeip `BufferedReadCallback` and therefore does not add an application-owned `std::vector<PacketBufferHandle>` whole-list cache. Internal connectedhomeip packet/exchange allocations are still controlled by the upstream stack.

When a probe exceeds a One-OS bound, returned data is partial and `truncated` is set. No dynamic result container grows to match a remote list.

## Credentials and fail-closed behavior

The controller operational keystore, operational certificate store, FabricTable and GroupDataProvider use connectedhomeip persistent storage facilities on ESP32.

One-OS does **not** use `ExampleOperationalCredentialsIssuer`: upstream explicitly documents it as test/tool code that serializes its CA key to clear persistent storage. Instead `chip_controller_config_t` accepts a fixed-buffer C operational-credentials provider. A product can back those callbacks with an appropriate protected CA/signing service without exposing connectedhomeip C++ types through the L2 API.

- creating the first Fabric fails closed unless `generate_controller_noc` is supplied;
- commissioning fails closed unless `generate_device_noc` is supplied;
- the provider's device NOC IPK must exactly match the controller Fabric IPK;
- PAA roots are caller supplied and copied into a fixed trust store;
- zero PAA roots means commissioning returns `CHIP_STATUS_NO_TRUST_ROOTS`;
- Test CD support, test attestation store and test operational issuer are disabled in `sdkconfig.defaults`;
- NVS initialization failure never triggers an automatic erase;
- credentials and IPK bytes are never logged by this component.

The Fabric IPK is persisted through connectedhomeip `GroupDataProvider`, not in a separate One-OS plaintext key.

## Timeout/cancel semantics

One-shot requests have bounded slots and optional timers. Timeout/cancel completes the public operation, but the slot is not reusable until the upstream CASE/IM transaction reaches its terminal callback. This prevents repeated timeouts from accumulating unbounded live `ReadClient`/Write/Invoke transactions.

Subscriptions are explicitly bounded and are stopped with `InteractionModelEngine::ShutdownSubscription`.

## Build and runtime validation

The research branch CI:

1. checks out esp-matter and connectedhomeip recursively;
2. runs host tests for fixed-capacity result logic;
3. runs a forbidden-cross-family/scope check;
4. builds and links the actual One-OS firmware for `esp32c6` with ESP-IDF v6.1;
5. prints `idf.py size` and `idf.py size-components`.

No physical board is required by the branch completion gate. When no board/live Matter node is available, runtime-only values are reported as:

- free heap: `NOT_MEASURED`
- largest free block: `NOT_MEASURED`
- CHIP task stack high-water mark: `NOT_MEASURED`
- runtime peak memory: `NOT_MEASURED`
- live CASE/ACL/interoperability observation: `NOT_MEASURED`
