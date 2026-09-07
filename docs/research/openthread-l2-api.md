# OpenThread 可移植 Level-2 API 第一阶段调研

> 分支：`research/openthread-l2-api`  
> 阶段：Phase 1 / research only  
> 目标板：Waveshare ESP32-C6-Touch-LCD-1.9  
> 固件基线：ESP-IDF v6.1 + FreeRTOS + LVGL，ESP32-C6，8 MB Flash，无 PSRAM 假设  
> 结论状态：**仅调研与 API 设计建议；未实现生产代码。**

## 1. 结论摘要

OpenThread 适合在 One-OS 中形成独立的 `openthread_*` Level-2 API 家族，但首批 API 不应是 `esp_openthread_*` / `ot*` 的改名包装，也不应复活旧扫描/session/radio coordinator 架构。

第一阶段建议把可移植 L2 能力集中在以下工作流：

1. **Thread Network Discovery 聚合**：调用 `otThreadDiscover()`，在异步回调中复制、去重、限量并规范化 Thread Discovery 结果；这是最高优先级。
2. **安全状态快照**：组合 role、attach duration、网络标识、RLOC/partition/leader/parent、计数器、commissioned 状态等多个原生 getter，明确排除 Network Key / PSKc。
3. **本地拓扑快照**：组合 neighbor 表以及 FTD 可用的 router/child 信息，产出有界结果；明确它是“本地视角”，不是完整全网拓扑。
4. **已授权 Operational Dataset 附着**：校验 TLV、应用 dataset、启 IPv6/Thread、等待 attach/超时并报告状态；这是有持久化副作用的控制工作流。
5. **Thread Joiner 入网**：使用 PSKd 执行 MeshCoP Joiner，再启动 Thread 并等待 attach；后续优先级。
6. **一次性授权 Joiner**：已附着设备临时启动 Commissioner，仅授权指定 EUI-64/Discerner + PSKd，超时后清理；后续优先级。
7. **Network Diagnostic Get**：对指定节点发送有限 TLV 查询并解析成有界结构；需要额外 OpenThread 编译特性，后续优先级。

不建议作为生产 L2 的能力：原始 802.15.4 active scan、energy scan、promiscuous/PCAP 被动嗅探、CLI passthrough、强制 Router/Leader、Network Diagnostic Reset、Border Router 一键封装、Matter 设备发现/commissioning、直接 dataset/key getter/setter 包装。

最重要的平台约束是 ESP32-C6 的单 2.4 GHz RF：Wi-Fi/BLE/802.15.4 通过共存仲裁共享射频。ESP-IDF v6.1 明确标注 Thread scan 与 BLE scan **不支持同时进行**，Wi-Fi STA scan/connecting/connected 与 Thread scan 为 **C1（支持但不稳定）**。因此 One-OS 应在应用层安排无线工作流，不让 OpenThread L2 依赖其他 API 家族；L2 自身只负责检测/拒绝自身重入并清理其临时状态。

## 2. One-OS 架构边界

根目录 `README.md` 与本分支 `AGENTS.md` 给出的约束决定了本设计：

- L1 继续直接使用 ESP-IDF/OpenThread/IEEE 802.15.4/FreeRTOS/lwIP；
- L2 只在“多个原生调用 + 状态管理 + 超时 + 结果归一化”形成真实复用能力时成立；
- 不增加只改名的 wrapper；
- `openthread_*` 家族不能调用 Matter、Home Assistant、ZHA、Kismet 或其他 One-OS L2 家族；
- 不引入通用 radio lifecycle/session/coordinator；跨无线能力的编排由 application 完成；
- L2 public type 不应暴露其他 One-OS 家族类型；本建议进一步避免暴露 ESP-IDF/OpenThread 的 public struct，以提高可移植性；
- Phase 1 只写调研文档，不修改生产 firmware。

当前 `firmware/sdkconfig.defaults` 只有 `CONFIG_IEEE802154_ENABLED=y`，**没有**启用 `CONFIG_OPENTHREAD_ENABLED`。因此现有基线证明 ESP32-C6 802.15.4 硬件能力已预留，但不能视为 OpenThread 栈已经集成。

CI 固定使用 **ESP-IDF v6.1**，所以调研以该版本为实现基准，而不是任意最新 upstream API。

## 3. 精确上游基线与来源

### 3.1 ESP-IDF v6.1

One-OS CI：

- `espressif/esp-idf-ci-action@v1`
- `esp_idf_version: v6.1`
- target `esp32c6`

ESP-IDF v6.1 的 `components/openthread` 指向 Espressif OpenThread fork commit：

- `espressif/openthread@b678a4f63b6f9397d1a0fa8f31e5b8e0271a4d00`

相关参考：

- ESP-IDF v6.1 Thread API：<https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/network/esp_openthread.html>
- ESP-IDF v6.1 RF coexistence：<https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/coexist.html>
- IDF OpenThread Kconfig：<https://github.com/espressif/esp-idf/blob/v6.1/components/openthread/Kconfig>
- 精确 OpenThread source commit：<https://github.com/espressif/openthread/tree/b678a4f63b6f9397d1a0fa8f31e5b8e0271a4d00>

### 3.2 ESP-IDF 对 OpenThread 的职责

ESP-IDF 官方文档明确区分：

- **操纵 Thread 网络本身使用 OpenThread API**；
- ESP-IDF 额外提供 stack launch/manage、netif binding、border routing 等平台集成。

重要原生接口：

- `esp_openthread_init()` / `esp_openthread_start()`：启动完整 OpenThread 栈；
- `esp_openthread_get_instance()`：取得当前 `otInstance *`；
- `esp_openthread_stop()`：停止栈，但 Thread active 时返回 invalid state；
- `esp_openthread_lock_acquire()` / `esp_openthread_lock_release()`：非 OpenThread callback 上下文调用任何带 `otInstance *` 的 OpenThread API 时必须加锁；
- ESP event 包含 `OPENTHREAD_EVENT_ATTACHED`、`DETACHED`、`ROLE_CHANGED`、`IF_UP/DOWN`、`GOT_IP6/LOST_IP6`、`DATASET_CHANGED` 等；
- `esp_openthread_port_config_t::storage_partition_name` 指定 OpenThread dataset/settings 持久化分区。

因此锁、异步事件等待、超时和 copy-out 都是 L2 工作流实现必须统一处理的内部责任；但“启动/停止 OpenThread 栈”本身仍是 L1 平台能力，不建议单独做成 public L2 改名函数。

## 4. 相关历史 One-OS / NearBy 工作

`v0.1.0-beta.2` 对应的临时 smoke test 中，`run_i154()` 曾经：

- 注册 `esp_ieee802154` RX callback；
- 对 11..26 信道逐个 `esp_ieee802154_enable()`；
- `esp_ieee802154_set_channel()`；
- 开启 promiscuous；
- `esp_ieee802154_receive()` 并驻留约 80 ms；
- 统计原始 802.15.4 frame 数量；
- 每个信道结束恢复 promiscuous/receive/radio 状态。

该代码证明 ESP32-C6 板级原生 802.15.4 收包可行，但它没有：

- MLE Thread Discovery；
- Thread Network Name / Extended PAN ID / joinable 语义解析；
- Operational Dataset；
- Joiner/Commissioner；
- Thread attach role；
- neighbor/router/child topology。

因此它只作为 **REFERENCE-ONLY 的硬件可行性证据**，不应移植回新的 L2 API。

## 5. Thread Discovery：OpenThread 真正提供什么

### 5.1 `otThreadDiscover()`

精确基线：`include/openthread/thread.h`。

```c
otError otThreadDiscover(otInstance              *aInstance,
                         uint32_t                 aScanChannels,
                         uint16_t                 aPanId,
                         bool                     aJoiner,
                         bool                     aEnableEui64Filtering,
                         otHandleActiveScanResult aCallback,
                         void                    *aCallbackContext);
```

行为：

- 发起 **Thread Discovery Scan / MLE Discovery**，不是普通 802.15.4 beacon scan；
- 需要 IPv6 interface 已启用，否则 `OT_ERROR_INVALID_STATE`；
- 同一时间已有 discovery 时返回 `OT_ERROR_BUSY`；
- 成功开始扫描后，OpenThread 在整个扫描过程中临时启用 rx-on-when-idle；
- callback 在每个 MLE Discovery Response 到达时触发，完成时以 `aResult == NULL` 通知；
- 可按 channel mask 和 PAN ID 过滤；
- `aJoiner` 控制 Discovery Request 的 Joiner Flag；
- 可启用 EUI-64 filtering。

### 5.2 Discovery 结果元数据

`otActiveScanResult` 同时用于 link active scan 和 Thread Discovery；当 `mDiscover=true` 时包含 Thread discovery 语义。可用字段包括：

- channel；
- PAN ID；
- Extended PAN ID；
- Network Name；
- Extended Address；
- RSSI；
- LQI；
- Thread version；
- joinable；
- Native Commissioner 标志；
- Joiner UDP port；
- Steering Data（最大 16 bytes）。

这组字段足够支持 One-OS 的“附近 Thread 网络发现”而无需自行解析原始 MAC/MLE frame。

### 5.3 为什么是 L2

直接暴露 `otThreadDiscover()` 不算 L2。真正有价值的 L2 是：

- 检查当前 stack/interface 状态；
- 统一锁；
- 发起 discovery；
- 异步 callback -> 同步有界 snapshot；
- copy-out，不泄露 callback 生命周期中的 upstream pointer；
- 去重（建议 `(ext_pan_id, network_name, pan_id, channel)` 作为候选 key，具体去重规则 Phase 2 以实测确定）；
- 保存 strongest/latest RSSI/LQI；
- caller capacity + `count/truncated`；
- busy/reentrancy；
- timeout；
- 若 L2 临时改变了 interface 状态则只恢复自己改变的状态。

### 5.4 被动发现为何不进入首批 L2

OpenThread 的 `otLinkSetPromiscuous()` / PCAP 属于 link/raw frame 能力。Promiscuous 只能在 Thread interface disabled 时开启；它会把问题变成原始 802.15.4/MLE frame 捕获与解析，并破坏正在工作的 Thread interface。

结论：

- **Thread Discovery**：L2 API；
- raw `otLinkActiveScan()`：单个原生 link scan，`DROP` 作为 L2；
- `otLinkEnergyScan()`：单个原生 energy scan，`DROP` 作为 L2；
- promiscuous/PCAP passive sniff：`TEST/TOOL`，不放生产 L2。

## 6. Candidate A — `openthread_discover_networks()`

### 6.1 建议 public 类型

```c
#define OPENTHREAD_NETWORK_NAME_MAX 16
#define OPENTHREAD_STEERING_DATA_MAX 16

typedef struct {
    uint32_t channel_mask;          /* 0 = platform supported Thread channels */
    uint16_t pan_id;                /* 0xffff = no PAN filter */
    bool joiner_flag;
    bool enable_eui64_filtering;
    uint32_t timeout_ms;
} openthread_discovery_options_t;

typedef struct {
    uint8_t  channel;
    uint16_t pan_id;
    uint8_t  extended_pan_id[8];
    uint8_t  extended_address[8];
    char     network_name[OPENTHREAD_NETWORK_NAME_MAX + 1];
    int8_t   rssi_dbm;
    uint8_t  lqi;
    uint8_t  thread_version;
    bool     joinable;
    bool     native_commissioner;
    uint16_t joiner_udp_port;
    uint8_t  steering_data[OPENTHREAD_STEERING_DATA_MAX];
    uint8_t  steering_data_len;
} openthread_network_t;

typedef struct {
    size_t count;
    bool truncated;
} openthread_discovery_result_t;

openthread_status_t openthread_discover_networks(
    const openthread_discovery_options_t *options,
    openthread_network_t *networks,
    size_t capacity,
    openthread_discovery_result_t *result);
```

以上仅是 Phase 1 API 草案，不是实现承诺。

### 6.2 L1 calls composed

- `esp_openthread_get_instance()`；
- `esp_openthread_lock_acquire/release()`；
- `otIp6IsEnabled()` / 必要时 `otIp6SetEnabled(true)`（是否由 L2临时启用，Phase 2 应采用最小副作用策略）；
- `otThreadDiscover()`；
- `otThreadIsDiscoverInProgress()`；
- FreeRTOS event/semaphore/notification 用于等待 completion；
- optional ESP event/state query 用于生命周期判断。

### 6.3 ESP32-C6 feasibility / coexistence

硬件和 upstream 支持明确可行，但：

- Thread scan + BLE scan：ESP-IDF v6.1 标记 **X（不支持）**；
- Wi-Fi STA scan/connecting/connected + Thread scan：**C1（支持但不稳定）**；
- BLE advertising/connected + Thread scan：支持。

L2 不能调用 BLE/Wi-Fi 家族来“协调”它们；application 应串行安排扫描窗口。L2 只需要保证同一个 OpenThread discovery 不重入。

### 6.4 Bounds / RAM

- 结果必须由 caller 提供固定 capacity；
- 不建立无限 heap vector；
- 推荐 Phase 2 默认 UI 调用 capacity 8~16，但 public API 不硬编码设备数量；
- callback 内只做 bounded copy/merge，不阻塞；
- exact RAM/flash delta 必须在实现后用 `idf.py size-components`、minimum free heap 和 task stack watermark 实测。

### 6.5 Tests / edge cases

- 0 result；
- 同一网络多次 response；
- 同 Network Name 不同 Extended PAN ID；
- capacity=0 / capacity=1；
- result overflow -> `truncated=true`；
- Thread interface down；
- scan already busy；
- lock timeout；
- timeout 先于 native completion：由于 `otThreadDiscover()` 没有通用 cancel API，private op context 必须保持到 completion，期间继续返回 busy，不能 use-after-free；
- scan 时已 attached；
- scan 时 role 为 router/leader 的干扰策略；
- Wi-Fi/BLE 共存压力测试。

### 6.6 Disposition / provenance

- Disposition：**L2 API / P0**
- Provenance：**CLEAN-ROOM REIMPLEMENT**，依据 OpenThread public headers/docs；CLI 只 `REFERENCE-ONLY`。

## 7. Candidate B — `openthread_get_state_snapshot()`

### 7.1 Value

应用 UI/诊断不应重复拼接十几个 native getter，也不应取得 Network Key/PSKc。一个只读、无 secret 的 snapshot 是稳定 L2 语义。

### 7.2 建议数据

```c
typedef enum {
    OPENTHREAD_ROLE_DISABLED,
    OPENTHREAD_ROLE_DETACHED,
    OPENTHREAD_ROLE_CHILD,
    OPENTHREAD_ROLE_ROUTER,
    OPENTHREAD_ROLE_LEADER,
} openthread_role_t;

typedef struct {
    bool commissioned;
    bool ip6_enabled;
    openthread_role_t role;
    uint32_t attach_duration_s;

    uint8_t channel;
    uint16_t pan_id;
    uint8_t extended_pan_id[8];
    char network_name[17];

    uint16_t rloc16;
    uint32_t partition_id;
    bool has_leader_data;
    uint8_t leader_router_id;
    uint8_t leader_weight;
    uint8_t data_version;
    uint8_t stable_data_version;

    bool has_parent;
    uint8_t parent_ext_address[8];
    uint16_t parent_rloc16;
    int8_t parent_avg_rssi_dbm;
    int8_t parent_last_rssi_dbm;

    /* selected non-secret counters */
    uint32_t ip_tx_success;
    uint32_t ip_rx_success;
    uint32_t ip_tx_failure;
    uint32_t ip_rx_failure;
    uint16_t attach_attempts;
    uint16_t parent_changes;
} openthread_state_snapshot_t;

openthread_status_t openthread_get_state_snapshot(
    openthread_state_snapshot_t *snapshot);
```

### 7.3 L1 calls composed

候选包括：

- `otDatasetIsCommissioned()`；
- `otIp6IsEnabled()`；
- `otThreadGetDeviceRole()`；
- `otThreadGetCurrentAttachDuration()`；
- `otLinkGetChannel()` / `otLinkGetPanId()`；
- `otThreadGetExtendedPanId()`；
- `otThreadGetNetworkName()`；
- `otThreadGetRloc16()`；
- `otThreadGetPartitionId()`；
- `otThreadGetLeaderData()`；
- `otThreadGetParentInfo()` / parent RSS helpers；
- IP/MLE counters getter；
- ESP-IDF OpenThread lock。

### 7.4 Security

**明确禁止** snapshot 暴露：

- Thread Network Key；
- PSKc；
- Joiner PSKd；
- 原始 Active Dataset TLV（因为其中可包含 credentials）。

如应用确实拥有 authorized dataset，它应通过专门的输入 API 使用，不通过通用状态 API 回读 secrets。

### 7.5 Why L2

它不是一个 getter 的别名，而是 role-aware、多 getter、secret-redacted、copy-out 的一致快照。

### 7.6 Tests

- disabled/detached/child/router/leader；
- 未 commissioned；
- leader/parent 信息 unavailable；
- role 在 snapshot 采集期间变化；
- network name 最大长度；
- lock contention；
- 反向安全测试：结构体中不存在 key/PSKc 字段。

### 7.7 Disposition / provenance

- Disposition：**L2 API / P0**
- Provenance：**CLEAN-ROOM REIMPLEMENT**。

## 8. Candidate C — `openthread_get_local_topology()`

### 8.1 Upstream capability

通用 Thread API：

- `otThreadGetNextNeighborInfo()`：迭代当前 neighbor table；
- parent info / link metrics；

FTD 扩展（`thread_ftd.h`）可进一步读取：

- child entries；
- router info；
- router ID / next hop / path cost 等。

### 8.2 语义限制

该 API 名称必须叫 **local topology** 或 **neighbor topology**，不能叫 full mesh topology：

- neighbor table 只代表本机当前可见/关联邻居；
- FTD router table 是本机路由视角；
- MTD 能看到的信息更少；
- 要获取远端节点自己的信息需 Network Diagnostic/Mesh Diagnostics 等网络查询。

### 8.3 建议 API

```c
typedef enum {
    OPENTHREAD_PEER_NEIGHBOR,
    OPENTHREAD_PEER_PARENT,
    OPENTHREAD_PEER_CHILD,
    OPENTHREAD_PEER_ROUTER,
} openthread_peer_kind_t;

typedef struct {
    openthread_peer_kind_t kind;
    uint8_t ext_address[8];
    uint16_t rloc16;
    uint8_t thread_version;
    uint32_t age_s;
    int8_t avg_rssi_dbm;
    int8_t last_rssi_dbm;
    uint8_t link_quality_in;
    uint8_t link_quality_out;
    uint8_t path_cost;
    bool rx_on_when_idle;
    bool full_thread_device;
    bool full_network_data;
} openthread_peer_t;

typedef struct {
    size_t count;
    bool truncated;
    bool ftd_details_available;
} openthread_topology_result_t;

openthread_status_t openthread_get_local_topology(
    openthread_peer_t *peers,
    size_t capacity,
    openthread_topology_result_t *result);
```

Phase 2 应避免为了填充某字段而伪造 `0`；对 upstream 不可用字段应有 validity flag 或定义明确的 sentinel。

### 8.4 Bounds

IDF v6.1 Kconfig 默认：

- `OPENTHREAD_MLE_MAX_CHILDREN=10`；
- `OPENTHREAD_TMF_ADDR_CACHE_ENTRIES=20`。

但 public API 不依赖这些编译期值；仍以 caller capacity 为边界。

### 8.5 Disposition / provenance

- Disposition：**L2 API / P1**
- Provenance：**CLEAN-ROOM REIMPLEMENT**。

## 9. Candidate D — `openthread_attach_dataset()`

### 9.1 Upstream dataset behavior

精确 OpenThread baseline 提供：

- `otDatasetIsCommissioned()`；
- `otDatasetGetActive/Tlvs()`；
- `otDatasetSetActive/Tlvs()`；
- `otDatasetIsValid()`；
- `otDatasetParseTlvs()`。

`otDatasetSetActiveTlvs()` 接受最大 254-byte Operational Dataset TLV；OpenThread 文档说明：

- dataset 可以部分完整；
- 仅 Network Key 就可能用于尝试 attach；
- 若没有 channel，OpenThread 会跨信道发送 MLE Announce 寻找网络；
- attach 成功后会从 Parent 获取完整 Active Dataset；
- non-volatile storage 对 Thread 已是 mandatory；保存失败会 assert。

因此 dataset attach 是**持久化且含 credential 的控制操作**，不能伪装成无副作用 scan。

### 9.2 建议 API

```c
#define OPENTHREAD_DATASET_TLVS_MAX 254

typedef struct {
    uint8_t bytes[OPENTHREAD_DATASET_TLVS_MAX];
    uint8_t length;
} openthread_dataset_tlvs_t;

typedef struct {
    uint32_t timeout_ms;
    bool require_complete_active_dataset;
} openthread_attach_options_t;

typedef struct {
    openthread_role_t final_role;
    uint32_t attach_duration_s;
    bool dataset_persisted;
} openthread_attach_result_t;

openthread_status_t openthread_attach_dataset(
    const openthread_dataset_tlvs_t *dataset,
    const openthread_attach_options_t *options,
    openthread_attach_result_t *result);
```

### 9.3 L1 calls composed

- validate TLV length；
- `otDatasetIsValid()`（如果要求完整 Active Dataset）；
- `otDatasetSetActiveTlvs()`；
- `otIp6SetEnabled(true)`；
- `otThreadSetEnabled(true)`；
- state/event callback 或 ESP OpenThread ATTACHED/ROLE_CHANGED event；
- `otThreadGetDeviceRole()`；
- timeout + cleanup；
- ESP OpenThread lock。

不建议仅包装 `esp_openthread_auto_start()`，因为它混合了 IDF/Kconfig dataset 生成语义，且 public API 会失去“输入 dataset、验证、等待 attach、返回结果”的稳定 L2 合约。

### 9.4 Persistence / rollback

这是本候选最重要的设计点：

- `otDatasetSetActiveTlvs()` 会写持久化 settings；
- attach timeout 不代表 dataset 没有被保存；
- L2 **不能静默恢复旧 dataset**，因为这可能覆盖调用者明确授权的新网络配置；
- 结果必须明确指出“attach failed/timed out”和“dataset 已应用/持久化”是两个不同事实；
- 如果未来要提供 transactional rollback，必须是显式 option，并先验证 old dataset 可安全保存/恢复；Phase 1 不建议默认做。

### 9.5 Security

- 仅接受调用者已授权持有的 Operational Dataset；
- 不从附近网络“提取” Network Key；
- 不记录 dataset bytes 到日志；
- 临时 buffer 用后清零（Phase 2）；
- UI/APP 必须明确提示这是网络配置写入和 attach 动作。

### 9.6 Tests

- malformed TLV；
- >254 bytes；
- complete/partial dataset；
- wrong key；
- no network；
- delayed attach；
- dataset persisted but attach timeout；
- reboot 后 commissioned 状态；
- concurrent discovery；
- disable/reenable lifecycle；
- secrets never logged。

### 9.7 Disposition / provenance

- Disposition：**L2 API / P1**
- Provenance：**CLEAN-ROOM REIMPLEMENT**。

## 10. Candidate E — `openthread_joiner_join()`

### 10.1 Exact upstream API

IDF v6.1 所带 OpenThread commit 的 `include/openthread/joiner.h`：

```c
otError otJoinerStart(otInstance      *aInstance,
                      const char      *aPskd,
                      const char      *aProvisioningUrl,
                      const char      *aVendorName,
                      const char      *aVendorModel,
                      const char      *aVendorSwVersion,
                      const char      *aVendorData,
                      otJoinerCallback aCallback,
                      void            *aContext);
```

要求：

- `OPENTHREAD_CONFIG_JOINER_ENABLE=1`；ESP-IDF 对应 `CONFIG_OPENTHREAD_JOINER=y`；
- IPv6 stack 必须 enabled；
- Thread stack 不能已经 fully enabled；
- busy/invalid args/invalid state；
- completion 可返回 success、security、not found、response timeout 等；
- Joiner 完成后仍需要启动 Thread protocol 才进入正常 attach。

OpenThread Joiner state：IDLE -> DISCOVER -> CONNECT -> CONNECTED -> ENTRUST -> JOINED。

### 10.2 L2 workflow

合理的 L2 不是 `otJoinerStart()` 改名，而是：

1. 检查 authorized PSKd；
2. 确认 interface/thread precondition；
3. 启动 Joiner；
4. 等 callback 或 timeout；
5. success 后启动 Thread；
6. 等 role 进入 child/router/leader；
7. 产出 join + attach 两阶段结果；
8. failure 时 `otJoinerStop()` 并恢复 L2 自己临时改变的状态；
9. 清理 PSKd 临时内存。

### 10.3 建议 API

```c
typedef struct {
    const char *pskd;
    const char *provisioning_url;
    const char *vendor_name;
    const char *vendor_model;
    const char *vendor_sw_version;
    uint32_t join_timeout_ms;
    uint32_t attach_timeout_ms;
} openthread_joiner_options_t;

openthread_status_t openthread_joiner_join(
    const openthread_joiner_options_t *options,
    openthread_attach_result_t *result);
```

Public contract 应说明 PSKd 是 Thread MeshCoP credential，不是 Matter setup passcode。

### 10.4 Disposition / provenance

- Disposition：**L2 API / P2**
- Provenance：**CLEAN-ROOM REIMPLEMENT**。

## 11. Candidate F — `openthread_commission_joiner_once()`

### 11.1 Exact upstream API

`include/openthread/commissioner.h`：

- `otCommissionerStart()`：当前设备必须已 attached；
- Commissioner state：DISABLED / PETITION / ACTIVE；
- `otCommissionerAddJoiner()`：指定 EUI-64 或 `NULL` 表示 any joiner；
- `otCommissionerAddJoinerWithDiscerner()`；
- Joiner entry 带 expiration timeout；
- `otCommissionerStop()`；
- ESP-IDF 需要 `CONFIG_OPENTHREAD_COMMISSIONER=y`，默认关闭，默认 max joiner entries=2。

### 11.2 安全设计

生产 L2 不应默认支持“any joiner”：

- 必须显式指定 EUI-64 或 Discerner；
- PSKd + authorization timeout 必填；
- start Commissioner -> wait ACTIVE -> add one joiner -> wait success/expiration -> remove entry -> stop Commissioner；
- 不保留长期 wildcard steering data；
- 不记录 PSKd；
- 如果 Commissioner 原本已由 application/native code active，L2 不能擅自 stop；必须记录 ownership。

### 11.3 建议 API

```c
typedef enum {
    OPENTHREAD_JOINER_ID_EUI64,
    OPENTHREAD_JOINER_ID_DISCERNER,
} openthread_joiner_id_kind_t;

typedef struct {
    openthread_joiner_id_kind_t kind;
    uint8_t eui64[8];
    uint64_t discerner_value;
    uint8_t discerner_length;
    const char *pskd;
    uint32_t authorization_timeout_s;
    uint32_t operation_timeout_ms;
} openthread_commission_joiner_options_t;

openthread_status_t openthread_commission_joiner_once(
    const openthread_commission_joiner_options_t *options);
```

### 11.4 Disposition / provenance

- Disposition：**L2 API / P3**
- Provenance：**CLEAN-ROOM REIMPLEMENT**。

## 12. Candidate G — `openthread_query_network_diagnostics()`

### 12.1 Exact upstream API

精确 baseline 的 `include/openthread/netdiag.h` 有：

- `otThreadSendDiagnosticGet()`；
- `otThreadGetNextDiagnosticTlv()`；
- `otThreadSendDiagnosticReset()`。

Diagnostic Get 可请求 TLV，例如：

- Ext Address / RLOC16；
- Mode；
- Connectivity；
- Route；
- Leader Data；
- IPv6 Address List；
- MAC Counters；
- Child Table；
- Thread Version；
- Vendor Name/Model/SW；
- Router Neighbor；
- MLE Counters 等。

但 client API 需要：

`OPENTHREAD_CONFIG_TMF_NETDIAG_CLIENT_ENABLE=1`

ESP-IDF v6.1 Kconfig 没有单独暴露一个明显的 `CONFIG_OPENTHREAD_*NETDIAG_CLIENT` 开关，因此 Phase 2 若批准该 API，需要通过 OpenThread custom config header 明确启用并验证 build/flash/RAM delta。

### 12.2 建议 L2 限制

- 仅允许明确的、只读 TLV allowlist；
- caller 指定 IPv6 destination；
- bounded response array；
- callback message 立即 parse/copy，不把 `otMessage *` 传给 application；
- explicit timeout；
- multicast 查询需特别限制结果数量；
- `Diagnostic Reset` 不进入首批 L2，因为它改变远端 counters/state，属于 diagnostic tool/explicit app action。

### 12.3 Disposition / provenance

- Diagnostic Get：**L2 API / P3**
- Diagnostic Reset：**TEST/TOOL / DROP from production L2**
- Provenance：**CLEAN-ROOM REIMPLEMENT**。

## 13. OpenThread 与 Matter 的严格边界

Thread 提供：

- IEEE 802.15.4 + IPv6 mesh；
- MLE discovery；
- Thread roles/topology；
- Operational Dataset；
- MeshCoP Joiner/Commissioner；
- Thread Network Diagnostics；
- IPv6 transport connectivity。

Matter 提供的是 Thread 之上的 application/commissioning semantics，例如：

- Matter setup passcode / discriminator；
- commissionable node discovery；
- PASE/CASE；
- Fabric；
- Node ID；
- Device Type；
- Endpoint/Cluster/Attribute/Command；
- Matter commissioning 时给 Thread device 下发 Operational Dataset。

因此 OpenThread L2 **不得**：

- 把 Thread Joiner PSKd 当 Matter passcode；
- 扫描/解析 Matter commissionable node；
- 管理 Matter Fabric；
- 暴露 Matter cluster/device semantics；
- 调用 Matter L2 以“实现” Thread join。

Matter family 可以在 application 层调用自己的 API，并在经授权的流程中把 dataset 作为数据交给 OpenThread family；两者 public type 不直接耦合。

## 14. Public API portability strategy

### 14.1 不暴露 `otInstance *`

Phase 1 建议 One-OS public L2 API 不带：

- `otInstance *`；
- `otError`；
- `otOperationalDataset*`；
- `esp_err_t`；
- `esp_event_*` type。

原因：

- 这些是 L1/upstream/platform type；
- 把它们放进 public L2 会让 API 很难 port 到其他 OpenThread platform binding；
- application 仍然可以绕过 L2 直接使用 native OpenThread API，这是 README 明确允许的。

实现内部可有 private adapter：

- ESP-IDF：`esp_openthread_get_instance()` + lock；
- 其他平台：换成对应 instance/serialization 机制。

### 14.2 不新增 public runtime/session manager

Phase 1 **不建议** `openthread_session_open()` / `radio_acquire()` / generic coordinator。

OpenThread native stack 启停仍留在 L1/application；只有当具体 L2 workflow 必须临时改变某个 state 时，它负责记录 ownership 并恢复自己改变的 state。

这避免重新引入 README 已排除的 scanner/session/radio orchestration 架构。

### 14.3 建议统一错误类型

若 Phase 2 批准，可定义小型 family-local error enum，例如：

```c
typedef enum {
    OPENTHREAD_STATUS_OK = 0,
    OPENTHREAD_STATUS_INVALID_ARGUMENT,
    OPENTHREAD_STATUS_INVALID_STATE,
    OPENTHREAD_STATUS_BUSY,
    OPENTHREAD_STATUS_TIMEOUT,
    OPENTHREAD_STATUS_NO_MEMORY,
    OPENTHREAD_STATUS_NOT_FOUND,
    OPENTHREAD_STATUS_SECURITY,
    OPENTHREAD_STATUS_IO,
    OPENTHREAD_STATUS_INTERNAL,
} openthread_status_t;
```

需要保存原始 `otError`/`esp_err_t` 时可只放在 internal log/test diagnostics，不作为跨平台 public ABI。

## 15. ESP32-C6 资源、共存与 build 约束

### 15.1 已知静态配置基线

IDF v6.1 OpenThread Kconfig 默认值中：

- OpenThread task stack：8192 bytes；
- task priority：5；
- FTD 为默认 device type；
- native 15.4 radio 为 ESP32-C6 默认 radio path；
- `OPENTHREAD_NUM_MESSAGE_BUFFERS=65`（无 PSRAM message-pool management 时）；
- FTD max children=10；
- Commissioner default off；
- Joiner default off；
- Border Router default off；
- Link Metrics default off；
- CLI/console 默认开；
- platform netif FTD/MTD 默认开。

One-OS 板级基线明确“不假设 PSRAM”，因此不能依赖 OpenThread PSRAM allocation 选项。

### 15.2 Flash/RAM 结论

Phase 1 不伪造一个“OpenThread 占用 X KB”的数字，因为实际 delta 取决于：

- FTD vs MTD；
- Joiner/Commissioner；
- netdiag；
- CLI/console；
- SRP/DNS；
- logging；
- link metrics 等。

Phase 2 的硬性验收应包括：

- `idf.py size-components` 前后 diff；
- minimum free heap；
- largest free block；
- OpenThread task stack high-water mark；
- discovery max-capacity 时 heap；
- join/attach 时 heap；
- Wi-Fi/NimBLE 同时启用时 heap；
- 8 MB partition layout 不溢出。

如果 One-OS 不需要 CLI，应优先评估关闭 OpenThread console/CLI 以减少重复功能与 flash，但这是 Phase 2 配置决策。

### 15.3 RF coexistence

ESP32-C6 只有一条 2.4 GHz RF path。v6.1 coexistence matrix 的关键项：

- Thread scan + BLE scan = X；
- Thread Router + BLE scan = X；
- Thread End Device + BLE scan = C1；
- Wi-Fi STA scan/connecting/connected + Thread scan = C1；
- Wi-Fi STA + Thread End Device = Y；
- Thread Router 在高 Wi-Fi/BLE traffic 下更易丢包；
- Espressif 对 Wi-Fi Thread Border Router 产品建议 dual-SoC。

因此：

- One-OS handheld 不应默认承担 Border Router；
- discovery UI/app 应串行调度 BLE scan 与 Thread scan；
- 若使用 FTD，为避免设备因网络条件升为 Router 而破坏多无线体验，应在 Phase 2 明确“MTD vs FTD + router eligibility”的产品策略；
- 该策略不能通过调用其他 One-OS L2 family 实现。

## 16. Dataset / persistence 设计原则

OpenThread settings 非易失存储是 Thread 正常运行的基础。ESP-IDF port config 也显式含 dataset storage partition。

L2 规则：

- discovery/state/topology：只读，不改 dataset；
- attach/joiner：明确可能写 Active Dataset；
- 不把 credential 写日志；
- 不提供“dump all dataset including secrets”的通用 API；
- 不在失败时静默 erase dataset；
- factory reset/erase persistent dataset 属于显式 APP/maintenance action，不混入 attach workflow；
- 所有改变都记录 ownership，cleanup 仅撤销本次 workflow 的临时状态，不撤销调用者明确提交的 credential state。

## 17. Authorization / safety boundary

允许：

- 扫描附近 Thread network 的公开 discovery response；
- 查看本机已授权加入网络的本地 topology/state；
- 使用用户/应用已提供的 authorized dataset 加入；
- 使用合法 PSKd 执行 Joiner；
- 在本机已授权网络中临时 commissioner 指定 joiner；
- 正常 Thread Network Diagnostic Get。

不进入 API：

- key extraction；
- 从流量推导/破解 Network Key/PSKd；
- 未授权网络进入；
- security bypass；
- jamming；
- hostile replay；
- session hijacking；
- exploit delivery；
- persistence beyond normal authorized Thread settings。

## 18. License / provenance

### 18.1 OpenThread

`espressif/openthread@b678...` 的 `LICENSE` 为 BSD-3-Clause。

本 API family 没有必要复制 OpenThread implementation code。建议：

- public API/type：**CLEAN-ROOM REIMPLEMENT**；
- native API 调用：正常依赖 OpenThread library；
- OpenThread CLI 输出/命令语义：**REFERENCE-ONLY**；
- 旧 One-OS beta.2 raw i154 smoke test：**REFERENCE-ONLY**；
- 不采用 `COPY`；
- 不采用源代码级 `PORT`，除非 Phase 2 出现确切且经许可审查的必要实现（当前没有）。

### 18.2 ESP-IDF

ESP-IDF 平台 integration 是依赖项；One-OS 只调用其 public API，不复制其实现。

## 19. 测试策略总表

### Host/unit test 可覆盖

- result copy/merge/dedup；
- capacity/truncation；
- error mapping；
- dataset TLV length/validation wrapper；
- secret redaction；
- state snapshot role-aware fields；
- lifecycle ownership state machine；
- timeout vs late callback；
- concurrent call -> busy；
- commissioner specified joiner validation；
- netdiag TLV allowlist/parser bounds。

### On-device integration test

至少 2 个 Thread capable node / 一个已知 Thread test network：

- discover 0/1/multiple networks；
- RSSI/LQI/channel/PAN/ExtPAN/Name correctness；
- attach authorized dataset；
- wrong key timeout；
- reboot persistence；
- joiner success/failure；
- local neighbor/topology；
- commissioner specified joiner；
- network diagnostic unicast；
- Wi-Fi STA active 时 Thread scan；
- NimBLE scan 与 Thread scan 必须由 app 串行，验证 concurrent policy；
- repeated start/timeout/stop leak check。

### Phase 2 必测资源

- build remains green；
- binary size delta；
- heap delta；
- stack watermark；
- repeated 100x discovery；
- repeated failed attach/joiner；
- no dangling callback/use-after-free；
- no secret in normal logs。

## 20. 优先级 API 表

| Priority | Candidate | Proposed public API | Upstream composition | Why L2 | ESP32-C6 | Provenance | Disposition |
|---|---|---|---|---|---|---|---|
| P0 | Thread Discovery | `openthread_discover_networks()` | OT lock + IP6 state + `otThreadDiscover` + async callback + bounded merge | 异步发现、去重、超时、copy-out、bounds | 可行；需避免 BLE scan 并发 | CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P0 | Safe state snapshot | `openthread_get_state_snapshot()` | role/dataset/network/leader/parent/counters 多 getter + lock | 多源状态统一、role-aware、secret redaction | 可行 | CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P1 | Local topology | `openthread_get_local_topology()` | neighbor iterator + optional FTD router/child data + lock | bounded local topology snapshot | 可行；FTD fields 条件化 | CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P1 | Authorized dataset attach | `openthread_attach_dataset()` | dataset validate/set + IP6 up + Thread enable + wait attach | credential/persistence/attach lifecycle + timeout | 可行；必须明确持久化副作用 | CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P2 | Thread Joiner | `openthread_joiner_join()` | `otJoinerStart` + callback + Thread enable + attach wait | 两阶段 commissioning/attach workflow | 可行；需开启 Joiner feature | CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P3 | One-shot Commissioner | `openthread_commission_joiner_once()` | commissioner start/active + specified joiner + wait/cleanup/stop | scoped authorization + ownership cleanup | 可行；需开启 Commissioner | CLEAN-ROOM REIMPLEMENT | **L2 API** |
| P3 | Network Diagnostic Get | `openthread_query_network_diagnostics()` | send get + callback + TLV parser + bounds | remote diagnostic transaction + parsing | 可行；需启 NETDIAG client 并测 size | CLEAN-ROOM REIMPLEMENT | **L2 API** |

## 21. Exclusions

| Capability | Upstream source | Exclusion reason | Disposition |
|---|---|---|---|
| `esp_openthread_start/stop/init` 改名包装 | ESP-IDF | 只是 L1 lifecycle API 改名；README 禁止无意义 wrapper | **DROP as L2 / keep native L1** |
| raw IEEE 802.15.4 active scan | `otLinkActiveScan` / `esp_ieee802154_*` | 单一 link-layer primitive，不是 Thread network workflow | **DROP as L2** |
| energy scan | `otLinkEnergyScan` | 单一 primitive；可由 test/app 直接用 native | **DROP as L2** |
| passive promiscuous/PCAP sniff | `otLinkSetPromiscuous`, PCAP | 需 Thread interface disabled；原始 frame 工具语义；易扰动正常网络 | **TEST/TOOL** |
| OpenThread CLI passthrough | OT CLI / ESP console | 字符串 shell，不是稳定 typed API；重复 native CLI | **DROP** |
| force Router/Leader/Child | `otThreadBecome*` | 部分 API 明确 test/demo/non-compliant；不应产品化 | **TEST/TOOL / DROP** |
| Network Diagnostic Reset | `otThreadSendDiagnosticReset` | 改变远端 diagnostic counters/state；不属于只读诊断首批能力 | **TEST/TOOL / APP explicit action** |
| unrestricted “any joiner” commissioning | `otCommissionerAddJoiner(NULL,...)` | 安全范围过宽，不应成为默认 L2 workflow | **DROP from production L2** |
| raw dataset/key getter/setter wrappers | Dataset/Thread getter/setter | 会直接暴露 credential 或只是 one-call wrapper | **DROP as L2 / keep native L1** |
| Border Router wrapper | `esp_openthread_border_router_*` | 平台/产品级角色；单 C6 RF 共存不理想，Espressif 推荐 dual-SoC Wi-Fi BR | **APP / platform feature, not this L2** |
| SRP/DNS service browser | OpenThread SRP/DNS | 可独立形成后续 capability，但不是本阶段 discovery/topology/join 核心 | **DEFER / APP or future L2** |
| Matter commissionable discovery / PASE / Fabric / clusters | Matter/CHIP | 属于 Matter application layer，违反 family boundary | **EXCLUDE: Matter family** |
| key extraction / bypass / replay / jamming / hijack | none acceptable | 超出授权与安全边界 | **DROP / prohibited** |

**Phase 1 recommendation:** 如果进入 Phase 2，先只批准 P0（`openthread_discover_networks()`、`openthread_get_state_snapshot()`），验证 ESP-IDF v6.1 OpenThread build、RF coexistence、RAM/flash 和 lifecycle；随后再按批准顺序增加 P1/P2/P3。未经明确批准，不开始生产代码实现。
