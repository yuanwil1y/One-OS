# ZHA / zigpy 可移植 Level-2 API 第一阶段调研

状态：**历史 Phase 1 调研快照（2026-09-07），非当前执行任务**  
分支：`research/zha-zigpy-l2-api`  
调研日期：2026-09-07  
目标：Waveshare ESP32-C6-Touch-LCD-1.9 / ESP-IDF + FreeRTOS  

> 本文保留调研当时的候选 API、基线描述与来源。之后 PR #12 已将 zha_l2/zigpy_l2 代码和 zb_storage 预留分区纳入 main；原生 Zigbee backend 仍待实现。下文“没有实现/没有分区”、分支 AGENTS.md、候选 matcher 与 flash 数据库等描述均属于历史上下文，不能作为当前实现状态或执行要求。当前任务见 [GUI 前开发任务书](../pre-ui-development.md)，匹配器与生产存储遵循 application 规范：应用 Device DB 唯一匹配，生产语料只放 SD。

## 1. 结论摘要

1. **zigpy 与 ZHA 必须继续分成两个 One-OS API 家族。** zigpy 的可移植核心是协调器/设备模型、join 事件到 interview、ZDO 发现、ZCL 事务、报告状态和失败/重试语义；ZHA 的可移植核心是 quirk/fingerprint 匹配、结构覆盖、能力归一化、配置计划和设备特定语义。ZHA 不应在公开 API 中暴露 `zigpy_*` 类型。
2. **ESP32-C6 硬件可承载这些能力，但当前 One-OS baseline 还没有 Zigbee coordinator stack/runtime。** 当前仅启用 `CONFIG_IEEE802154_ENABLED=y`，这只是 L1 802.15.4 能力。Espressif 当前 ESP Zigbee SDK v2 支持 ESP32-C6、Coordinator/Router/End Device、Zigbee 3.0 / Zigbee Pro R23 / ZCL v8，以及 ZDO/ZCL 请求；Phase 2 若获批，第一前置工作应是把 Zigbee stack 作为 L1 原生依赖接入，而不是在 L2 里重写 Zigbee 协议。
3. **当前分区表没有 Zigbee 持久化分区。** ESP Zigbee SDK v2 使用 NVS 型 `zb_storage`；One-OS 当前 `partitions_8mb.csv` 只有 `nvs`、`phy_init`、`factory`。这意味着 coordinator 网络持久状态尚未具备生产条件。
4. **不能把“附近 Zigbee 设备发现”理解为对任意第三方网络的枚举。** ZHA/zigpy 正常模型是对本机作为协调器/信任中心管理的网络进行授权 commissioning、join、interview 与控制。对陌生加密网络做密钥提取、安全绕过或未授权接入不在范围内。
5. **最值得优先移植的 zigpy 语义是 interview + bounded ZCL transaction engine。** 特别是：join 后自动 interview；Node Descriptor → Active Endpoints → Simple Descriptors → Basic manufacturer/model；部分结果显式化；失败时保留旧设备模型的 shadow re-interview；请求关联、超时、有限重试、按项状态；读取/写入/报告的内存边界。
6. **最值得优先移植的 ZHA 语义是 quirk 匹配 + capability normalization + configuration plan。** 2026 年 upstream 已把 quirks v2 的主实现从 zigpy 迁到 ZHA / `zha-device-handlers`；`zigpy.quirks.v2` 只保留兼容 shim。因此新 quirks 能力必须归 `zha_*`，不能归 `zigpy_*`。
7. **不能直接把 Python 对象图搬进 ESP32-C6。** zha-device-handlers 的动态类替换、任意 Python filters/converters、HA entity 生命周期都不适合作为嵌入式运行时。可移植方案应是紧凑只读 quirk 数据 + 有限 C 解释/handler 表；任意 Python 行为只能手工移植或 `REFERENCE-ONLY`。
8. **许可证要求强制区分来源。** 当前 zigpy `LICENSE` 为 GPL-3.0；ZHA 与 `zha-device-handlers` 为 Apache-2.0。One-OS 当前分支根目录未发现 `LICENSE`。因此本文对 zigpy 候选默认标记 `CLEAN-ROOM REIMPLEMENT`/`REFERENCE-ONLY`，不建议复制其代码；ZHA/zha-device-handlers 可按 Apache-2.0 做有来源记录的 `PORT`，但实际复制前仍需先确定 One-OS 的项目许可证与 NOTICE 策略。

## 2. One-OS 现状与 Phase 2 前置条件

### 2.1 当前 baseline

来自仓库根 `README.md` / `AGENTS.md`：

- L1 继续直接使用 ESP-IDF、FreeRTOS、IEEE 802.15.4 等原生 API；
- L2 只有在提供真实可复用的编排/解析/模型语义时才成立，不能只给原生 API 改名；
- 不同 project API family 是 peers，不能互相依赖或暴露彼此 public types；
- BSP 只拥有板级事实；
- 当前硬件基线不假定 PSRAM；
- Phase 1 禁止生产实现。

当前 `firmware/sdkconfig.defaults` 只明确启用了 `CONFIG_IEEE802154_ENABLED=y`，未见 ESP Zigbee SDK 组件配置。当前 `firmware/partitions_8mb.csv` 也没有 `zb_storage`。

### 2.2 推荐的 L1 Zigbee runtime（若 Phase 2 获批）

优先评估 **ESP Zigbee SDK v2.x**，不是自行从 IEEE 802.15.4 拼 Zigbee NWK/APS/ZDO/ZCL。

当前官方能力：

- ESP32-C6 可构建 Zigbee device；
- Zigbee 3.0、Zigbee Pro R23、ZCL v8；
- Coordinator / Router / (Sleepy) End Device；
- `esp_zigbee_init/start/launch_mainloop`；
- ZDO `ezb_zdo_node_desc_req`、`ezb_zdo_active_ep_req`、`ezb_zdo_simple_desc_req`、bind、permit joining 等；
- ZCL Read/Write Attributes、Configure Reporting、Discover Attributes/Commands 等；
- Trust Center install-code policy；
- Zigbee SDK API 非线程安全，除 Zigbee callback/posted Zigbee task callback 外必须持有 Zigbee lock。

这部分全部属于 **L1 native facility**。L2 的价值来自状态机、事务关联、bounded store、cache、fallback、归一化与可组合的设备模型。

### 2.3 持久化原则

ESP Zigbee SDK v2 的网络安全材料、计数器、网络数据集应继续由原生 stack 的 `zb_storage` 管理。One-OS `zigpy_*` 不应复制或导出 network keys。

如果 L2 需要跨重启保留 interview 结果，只保存**可重建的派生状态**：IEEE/NWK 映射、node descriptor、endpoint/simple descriptor、manufacturer/model、有限 attribute cache、quirk/version fingerprint。该缓存必须版本化、可丢弃、可重新 interview，且与原生 network dataset 分离。

## 3. Upstream 快照与来源归属

调研固定到以下 upstream 快照，避免把之后的行为变化混入本阶段结论：

| Project | Snapshot | License | 关键模块 |
|---|---|---|---|
| `zigpy/zigpy` | `083d14fef7a2240920b67d98df11a55c6e4012ec` (2026-08-20) | GPL-3.0 | `zigpy/application.py`, `device.py`, `endpoint.py`, `zcl/`, `zdo/`, `appdb.py` |
| `zigpy/zha` | `66603431339afe37fa0048b70ff31d77dceb8f95` (2026-09-02) | Apache-2.0 | `zha/quirks.py`, `zha/zigbee/device.py`, `zha/zigbee/cluster_config.py`, `zha/application/platforms/` |
| `zigpy/zha-device-handlers` | `6a3822c1348f9e40ad243eb881084a1c075da277` (2026-08-26) | Apache-2.0 | `zhaquirks/builder/`, `zhaquirks/device.py`, vendor quirk modules |
| ESP Zigbee SDK docs | latest, checked 2026-09-07 | Espressif component licenses; integration to be reviewed in Phase 2 | `esp_zigbee.h`, `ezbee/zdo`, `ezbee/zcl`, `ezbee/secur`, BDB |

关键归属变化：`zigpy/quirks/v2/__init__.py` 明确说明 quirks v2 authoring API 已迁到 `zhaquirks`，`QuirkBuilder` 位于 `zhaquirks.builder`，metadata 位于 `zhaquirks.builder.metadata`，ZHA entity/device-class enums 位于 `zha.application`。因此 2026 年后的 quirk v2 语义归 **ZHA/zha-device-handlers**。

## 4. zigpy 候选能力调研

### ZG-1 — 授权 commissioning session + join-to-interview handoff

- **Exact upstream project/module:** `zigpy.application.ControllerApplication`; join handling / `permit()`；ZDO `Mgmt_Permit_Joining_req`。
- **Behavior:** upstream 对新 join 产生 `device_joined`，随后安排 device initialization；`permit()` 是限时 permit joining 的 controller 行为。
- **Zigbee layer:** BDB/ZDO/NWK + controller orchestration。
- **Product value:** 给手持设备一个安全、限时、自动关闭的“添加 Zigbee 设备”会话；join 成功后自动进入 interview，而不是让 app 自己拼多个原生 callback。
- **Proposed C API/types:** `zigpy_commissioning_session_t`; `zigpy_commissioning_opts_t { duration_s, max_new_devices, interview_on_join }`; `zigpy_commissioning_start()`, `zigpy_commissioning_cancel()`, `zigpy_commissioning_status()`；事件 `ZIGPY_EVENT_DEVICE_JOINED` / `...INTERVIEW_*`。
- **L1 facilities required:** ESP Zigbee BDB open/close network、ZDO permit joining、join/device announce signal、Zigbee task/lock。
- **Why L2:** 如果只调用 `ezb_bdb_open_network()` 就是无价值 wrapper；只有“deadline + auto-close + bounded joined set + join→interview + explicit result”才是 reusable L2。
- **Coordinator/network prerequisites:** 本机已形成/恢复为 Coordinator/Trust Center 的授权 Zigbee 网络。
- **ESP32-C6 feasibility:** 高；原生 SDK 提供 coordinator/BDB/ZDO。
- **RAM/flash/bounds:** 会话必须有限设备数；不允许 `0xFF` indefinite permit 作为默认路径；事件队列固定上限。
- **Persistent-network-state implications:** 不写 network key；permit 状态不应跨重启自动恢复为 open。
- **Authorization/security:** 只对用户拥有/授权的网络开放；可与原生 install-code policy 配合；会话结束强制 close。
- **License/provenance:** `CLEAN-ROOM REIMPLEMENT`（zigpy GPL-3.0；行为参考，底层调用 Espressif native）。
- **Disposition:** `L2 API`，但优先级低于 interview/ZCL engine；避免把 BDB native 包装本身做成 API。
- **Tests/interoperability:** duration 结束自动关闭；0/非法时长；多个 join；设备 rejoin 不重复记为新设备；permit API 失败；install-code-required 网络；join 后 interview 失败仍关闭窗口。

### ZG-2 — Device interview / partial interview / safe re-interview

- **Exact upstream project/module:** `zigpy.device.Device._discover()/initialize()/reinterview()`；`zigpy.endpoint.Endpoint.initialize()/get_model_info()`。
- **Behavior:** 依次获取 Node Descriptor、Active Endpoints、每个 endpoint 的 Simple Descriptor，再从 Basic cluster 读取 manufacturer/model。`reinterview()` 使用 shadow device，只有新 discovery 成功才替换旧模型；失败保留旧状态。Basic identity 读取包含兼容性 fallback：先尝试 manufacturer+model 联合读取，再分开读取，因为部分设备不能处理多属性请求。
- **Zigbee layer:** ZDO descriptor discovery + ZCL Basic cluster。
- **Product value:** 这是“识别设备、枚举 endpoint/cluster/capability”的基础，而且把睡眠设备、超时、部分失败、厂商兼容性从 app 中移走。
- **Proposed C API/types:** `zigpy_device_ref_t { ieee, nwk }`; `zigpy_interview_phase_t`; `zigpy_interview_status_t`; caller/pool-owned `zigpy_interview_session_t`; `zigpy_interview_begin()`, `zigpy_interview_cancel()`, `zigpy_interview_get_snapshot()`, `zigpy_reinterview_begin()`；result 包含 `complete_mask`, `truncated_mask`, per-endpoint status。
- **L1 facilities required:** `ezb_zdo_node_desc_req`, `ezb_zdo_active_ep_req`, `ezb_zdo_simple_desc_req`; ZCL Basic Read Attributes；request callback；timer/FreeRTOS synchronization；Zigbee lock/task。
- **Why L2:** 多阶段状态机、重试/timeout、partial result、identity fallback、shadow swap 都是 native ZDO/ZCL 之上的真实编排。
- **Coordinator/network prerequisites:** 目标必须是本机 Zigbee 网络内可寻址 device；sleepy device 可能需要等待 poll/check-in。
- **ESP32-C6 feasibility:** 高；官方 SDK 已提供所需 ZDO/ZCL primitive。
- **RAM/flash/bounds:** endpoint 数、每 endpoint cluster 数和总 snapshot 必须配置上限；超出时返回 `truncated`，不能无限 realloc。禁止按 Zigbee theoretical maximum 直接常驻对象图。
- **Persistent-network-state implications:** 可保存 versioned derived snapshot；reinterview 失败不得覆盖 last-known-good snapshot。
- **Authorization/security:** 只 interview 已授权加入本机网络的设备。
- **License/provenance:** `CLEAN-ROOM REIMPLEMENT`。
- **Disposition:** **`L2 API` P0**。
- **Tests/interoperability:** single/multi-endpoint；inactive endpoint；Basic cluster absent；combined manufacturer/model read timeout then individual success；sleepy device timeout；partial Simple Descriptor；reinterview failure keeps old snapshot；NWK address change with same IEEE。

### ZG-3 — Bounded device/endpoint/cluster model

- **Exact upstream project/module:** `zigpy.device.Device`, `zigpy.endpoint.Endpoint`, simple descriptor model。
- **Behavior:** Device 以 IEEE 为稳定身份、NWK 为当前短地址；endpoint 记录 profile id、device type、input/server clusters 与 output/client clusters；device 还保留 node descriptor、manufacturer/model、LQI/RSSI/last seen。
- **Zigbee layer:** ZDO/ZCL data model。
- **Product value:** 为 UI、诊断、能力识别和应用层提供统一只读 snapshot，而不要求每个 app 自己缓存 ZDO callback 数据。
- **Proposed C API/types:** `zigpy_device_snapshot_t`, `zigpy_node_desc_t`, `zigpy_endpoint_desc_t`, `zigpy_cluster_ref_t { endpoint_id, cluster_id, role }`; `zigpy_device_get()`, `zigpy_endpoint_count/get()`, `zigpy_cluster_count/get()`；不公开 Espressif `ezb_*` 类型。
- **L1 facilities required:** interview 输出 + join/network signals；可选 LQI/RSSI source。
- **Why L2:** 数据模型是 interview 的持久结果和后续事务寻址基础，不是单个 native descriptor 的 typedef rename。
- **Coordinator/network prerequisites:** 同 ZG-2。
- **ESP32-C6 feasibility:** 高。
- **RAM/flash/bounds:** 固定/配置上限 + caller iteration；字符串长度有明确最大值并保留 truncation/invalid UTF-8 标志；不在 snapshot 内保存任意大小 payload。
- **Persistent-network-state implications:** IEEE 为主键；NWK 可随 rejoin 更新；snapshot cache 可重建。
- **Authorization/security:** 不含 key material。
- **License/provenance:** `CLEAN-ROOM REIMPLEMENT`。
- **Disposition:** **`L2 API` P0**。
- **Tests/interoperability:** duplicate endpoint/cluster；unknown profile/device id；manufacturer-specific cluster；NWK 更新；cache load schema version mismatch。

### ZG-4 — ZCL transaction engine: read / write / cluster command

- **Exact upstream project/module:** `zigpy.zcl.Cluster`; `read_attributes`, `write_attributes`, request/response matching, command dispatch；`zigpy.device.Device` request limiter/retry behavior。
- **Behavior:** 把 endpoint/cluster + TSN + direction 与 request 关联；属性读取返回 success/failure per item；写入更新状态；manufacturer code 被保留；upstream 当前对 Read Attributes 采用每请求最多 5 个 attribute 的保守 chunk，对 Write/Configure Reporting 的 serialized records 采用 50-byte request budget。后两者是 upstream 兼容性策略，不应未经 ESP stack 验证原样硬编码。
- **Zigbee layer:** APS + ZCL Foundation + cluster-specific commands。
- **Product value:** 提供可靠、bounded、可取消的读写/控制能力，并统一处理 timeout、status、manufacturer-specific、batching 和 callback 生命周期。
- **Proposed C API/types:** `zigpy_zcl_target_t { ieee, endpoint, cluster_id, cluster_role, manufacturer_code }`; 独立 `zigpy_zcl_value_t`; `zigpy_attr_read_req_t/result_t`, `zigpy_attr_write_req_t/result_t`, `zigpy_command_req_t/result_t`; `zigpy_attr_read_async()`, `zigpy_attr_write_async()`, `zigpy_command_invoke_async()`, `zigpy_request_cancel()`。
- **L1 facilities required:** ESP Zigbee ZCL read/write/general command APIs；command confirmation callback；timer；Zigbee lock/task。ESP SDK callback 中 attribute value 内存由 stack 管理，若 L2 保留必须在 callback 结束前复制。
- **Why L2:** request correlation、typed decode、chunking、per-record status、有限并发、retry/timeout、safe copy 是 reusable transaction semantics；单独暴露 `ezb_zcl_*` 不属于 L2。
- **Coordinator/network prerequisites:** 已 interview 或调用方提供有效 endpoint/cluster；目标在线/可达。
- **ESP32-C6 feasibility:** 高；但 manufacturer-specific duplicate-ID 行为应纳入兼容测试，不能假定所有 SDK edge case 已解决。
- **RAM/flash/bounds:** 固定最大 in-flight request；每请求 records/payload byte budget；variable-length value 必须复制到 caller/pool 提供的 bounded buffer；返回 `BUFFER_TOO_SMALL` / `TRUNCATED`，禁止隐藏式无限分配。
- **Persistent-network-state implications:** 写操作可更新派生 attribute cache；失败不得把 cache 当成功值。
- **Authorization/security:** 写和 command 只面向已授权网络内设备；不提供 raw security bypass。
- **License/provenance:** `CLEAN-ROOM REIMPLEMENT`。
- **Disposition:** **`L2 API` P0**。
- **Tests/interoperability:** mixed success/failure attributes；unsupported attr；default response；timeout/retry；manufacturer-specific attr/command；large string/octet；batch split；sleepy device；late response after cancel；duplicate TSN protection。

### ZG-5 — Reporting subscription + attribute state events

- **Exact upstream project/module:** `zigpy.zcl.Cluster.configure_reporting()/bind()`；`AttributeReportedEvent` / attribute cache。
- **Behavior:** 配置 reporting，接收 unsolicited Report Attributes，把 value 按 endpoint/cluster/attribute/manufacturer code 更新到 cache 并发出事件；即使值未改变，upstream 也有独立 report event 语义。
- **Zigbee layer:** ZDO bind + ZCL Configure Reporting / Report Attributes。
- **Product value:** 低功耗实时状态，不需要 UI/app 高频轮询；也是传感器、开关、功率数据的核心。
- **Proposed C API/types:** `zigpy_reporting_req_t`; `zigpy_reporting_status_t`; `zigpy_reporting_configure_async()`；`zigpy_attribute_event_t { source, attr_id, type, value, timestamp, is_report }`; controller event callback/queue。是否 bind 由调用请求显式指定；ZHA 的配置计划由 app 转换成这些操作，`zigpy_*` 不依赖 `zha_*`。
- **L1 facilities required:** ZDO bind、ZCL Configure Reporting、Report Attribute callback、Zigbee lock/task。
- **Why L2:** bind/config request 生命周期 + report decode/copy/cache/event 是多 primitive 编排。
- **Coordinator/network prerequisites:** endpoint/cluster 支持 reporting；部分设备需要 bind；sleepy device 配置可能延迟。
- **ESP32-C6 feasibility:** 高。
- **RAM/flash/bounds:** report event queue 有固定深度；value copy 有固定 byte budget；cache 只存订阅/感兴趣属性，不做全属性无限缓存。
- **Persistent-network-state implications:** reporting configuration 远端设备可能保留或丢失；重启后 L2 不能仅相信本地缓存，需支持 idempotent reconfigure。
- **Authorization/security:** 标准授权 Zigbee 流量。
- **License/provenance:** `CLEAN-ROOM REIMPLEMENT`。
- **Disposition:** **`L2 API` P0/P1**。
- **Tests/interoperability:** success-only configure response；per-attribute failure；report before/after config；unchanged value still emits report event；restart/reconfigure；device removed；queue overflow surfaced。

### ZG-6 — Derived device-cache persistence

- **Exact upstream project/module:** `zigpy.appdb.PersistingListener` / schema migrations；controller backup/topology state are separate concerns。
- **Behavior:** upstream host runtime persists devices, endpoints, clusters, attributes and other derived topology state.
- **Zigbee layer:** host-side state, not a Zigbee protocol layer。
- **Product value:** 快速开机显示 last-known state，避免每次重启立即完整 interview 所有设备。
- **Proposed C API/types:** 不建议 Phase 2 首先公开一个 storage abstraction。优先把 `zigpy_*` 内部 cache 直接用 ESP-IDF NVS 的独立 namespace/version；只公开 `zigpy_cache_invalidate_device()` / `zigpy_cache_reinterview_needed()` 之类业务语义（若实际需要）。
- **L1 facilities required:** ESP-IDF NVS；原生 Zigbee `zb_storage` 保持独立。
- **Why L2:** schema/version/rebuild policy 属于 device-model persistence；纯 NVS CRUD 不是 L2。
- **Coordinator/network prerequisites:** stack network dataset 已正确恢复。
- **ESP32-C6 feasibility:** 高，但 flash wear 与容量必须测量。
- **RAM/flash/bounds:** compact binary records；只缓存 bounded snapshot/selected attrs；CRC/version；避免 SQLite 模型。
- **Persistent-network-state implications:** **绝不复制 network keys/security counters**；derived cache 可全部删除并重建。
- **Authorization/security:** cache 可能包含设备标识和状态，应避免无必要存储敏感值。
- **License/provenance:** `REFERENCE-ONLY` for zigpy DB schema; One-OS storage format应自行设计。
- **Disposition:** `L2 internal / DATA`，非首批 public API。
- **Tests/interoperability:** power loss during write；schema upgrade；network factory reset invalidates derived cache；stale NWK update；flash wear soak。

### ZG-7 — topology, groups, OTA, radio plugin/backup framework

- **Exact upstream project/module:** `zigpy.topology`, `zigpy.group`, `zigpy.ota`, `zigpy.backups`, radio entry-point/controller abstractions。
- **Behavior:** host-oriented topology scans、group bookkeeping、OTA provider ecosystem、radio adapter abstraction和备份。
- **Assessment:** 对 One-OS 第一阶段目标不是最小必要能力。Integrated ESP32-C6 不需要 Python radio plugin/NCP abstraction；OTA provider ecosystem 依赖网络/文件/签名策略，范围明显更大；network backup 与原生 stack dataset 的可移植边界需要单独专题。
- **Proposed API:** 当前不提 public API。
- **L1 facilities:** varies。
- **Why not L2 now:** 会显著扩大范围，且存在 native stack ownership 冲突。
- **Coordinator/network prerequisites:** varies。
- **ESP32-C6 feasibility:** groups/topology 可行；OTA/backup 需另调研。
- **RAM/flash/bounds:** host模型直接搬运不合适。
- **Persistent state:** 高风险与 native stack 重叠。
- **Authorization/security:** OTA/backup 涉及更严格完整性/密钥边界。
- **License/provenance:** `REFERENCE-ONLY`。
- **Disposition:** `DROP` from first implementation / later research。
- **Tests:** 不进入首批测试矩阵。

## 5. ZHA / zha-device-handlers 候选能力调研

### ZH-1 — Quirk/fingerprint registry + deterministic match

- **Exact upstream project/module:** `zha.quirks.DeviceMatch`, `QuirkRegistryEntry`, `DeviceRegistry`。
- **Behavior:** 按 `(manufacturer, model)`、manufacturer-only、model-only、wildcard/filter 顺序查找；可增加 firmware min/max 条件；match 后应用 transform；transform 失败时回退 bare device；resolution 幂等，并保留 quirk provenance。
- **Zigbee layer:** interview 后的 device semantics；不是无线协议本身。
- **Product value:** 解决真实市场设备“不完全按标准实现”的识别和兼容问题，是 ZHA 的核心差异化价值。
- **Proposed C API/types:** 独立于 zigpy 的 `zha_device_signature_t`（manufacturer/model/firmware + endpoint/profile/device/cluster summary）；`zha_quirk_db_t`; `zha_quirk_match_t { quirk_id, source_id, confidence/flags }`; `zha_quirk_match()`。
- **L1 facilities required:** 无直接 radio call；输入由 app 从任一来源构建。若 app 使用 `zigpy_*`，它负责把 `zigpy_device_snapshot_t` 字段复制到 `zha_device_signature_t`，两 family headers 不互相 include。
- **Why L2:** deterministic matching + rule precedence + fallback + provenance 是 reusable semantic engine。
- **Coordinator/network prerequisites:** 通常需要完成 Basic identity 和 endpoint/cluster interview；firmware filter 需要相应 attribute 已读取/缓存。
- **ESP32-C6 feasibility:** 高，若规则是 compact data；不能在设备上运行 Python predicate。
- **RAM/flash/bounds:** registry 常驻 flash，按 hash/index 查找；RAM 只保留少量 match state；任意 Python filter 必须编译成有限 predicate opcode 或手工 handler。
- **Persistent-network-state implications:** quirk match 是派生结果；DB 版本变化可重新计算。
- **Authorization/security:** 不涉及绕过 Zigbee 安全。
- **License/provenance:** `PORT` from Apache-2.0 semantics; data/provenance retained。任意直接复制仍需项目 license/NOTICE 决策。
- **Disposition:** **`L2 API` P0**；quirk records 是 `DATA`。
- **Tests/interoperability:** exact > manufacturer-only > model-only > wildcard；firmware boundaries；missing firmware allow/deny；duplicate registrations determinism；bad transform fallback；no-match returns standard device。

### ZH-2 — Structural quirk overlay, not dynamic Python object replacement

- **Exact upstream project/module:** `zhaquirks.device.BaseCustomDevice`, `CustomZigpyDevice`, `CustomEndpoint`; QuirkBuilder `.adds/.removes/.replaces`。
- **Behavior:** upstream 可 clone/replace device、endpoint、cluster 并保留原状态/cache；quirk 可以修正错误 descriptor、替换 cluster implementation、增加虚拟/自定义 cluster。
- **Zigbee layer:** device/endpoint/ZCL schema overlay。
- **Product value:** 很多厂商设备报告错误 cluster 或把数据放在 manufacturer-specific cluster；没有结构 overlay，后续归一化会失效。
- **Proposed C API/types:** `zha_device_view_t` / `zha_endpoint_view_t` / `zha_cluster_view_t`; compact `zha_quirk_patch_t` opcodes: add/remove/replace endpoint/cluster, attribute alias, manufacturer-code override, constant attribute。`zha_quirk_apply_view()` 只生成 overlay view，不破坏原始 signature。
- **L1 facilities required:** 无直接 radio；ZHA family 自有 input/view 类型。
- **Why L2:** 将原始 descriptor 解释成稳定 corrected view，是 device semantics。
- **Coordinator/network prerequisites:** 原始 signature 已存在。
- **ESP32-C6 feasibility:** 中高；数据驱动 structural patch 可行，任意 Python subclass method 不可直接移植。
- **RAM/flash/bounds:** base signature 保留；overlay 只存 patch/index，避免复制整套 object graph；patch count 上限。
- **Persistent-network-state implications:** raw signature 与 applied quirk id/version 可缓存；DB 升级后重新计算 overlay。
- **Authorization/security:** 无。
- **License/provenance:** declarative semantics `PORT`; arbitrary Python override `REFERENCE-ONLY` / manual clean-room C handler。
- **Disposition:** **`L2 API` P0/P1**；quirk patch table 为 `DATA`。
- **Tests/interoperability:** add/remove/replace；raw signature unchanged；cache value survives compatible overlay；invalid patch rejected；unknown custom behavior falls back/no-match。

### ZH-3 — Capability normalization

- **Exact upstream project/module:** `zhaquirks.builder.metadata` (`EntityMetadata`, `ZCLSensorMetadata`, `BinarySensorMetadata`, `SwitchMetadata`, `NumberMetadata`, `ZCLEnumMetadata`, command/write buttons) + `zha.application` device-class semantics。
- **Behavior:** 把 endpoint/cluster/attribute/command 解释为用户可理解的能力，附带 unit、multiplier/divisor、range/step、enum、on/off values、reporting hints、primary/disabled 等 metadata。
- **Zigbee layer:** ZCL semantic normalization。
- **Product value:** 从“cluster 0x0402 attribute 0x0000”提升到“temperature sensor + °C + scaling + read/report capability”；对手持 UI 和统一控制价值最高。
- **Proposed C API/types:** `zha_capability_kind_t { SENSOR, BINARY_SENSOR, SWITCH, NUMBER, ENUM, ACTION }`; `zha_capability_t`（独立 source tuple + unit/scale/range/access/reporting hints）；`zha_capability_iter_begin/next()` 或 caller-buffer `zha_capability_enumerate()`。
- **L1 facilities required:** 无直接 radio。Application 根据 capability 的 source tuple 自行选择 `zigpy_*` 或其他 native mechanism 读写；`zha_*` 不调用 `zigpy_*`。
- **Why L2:** 这是从 Zigbee schema/quirk 到产品语义的真正归一化，不是 native wrapper。
- **Coordinator/network prerequisites:** signature + matched quirk；标准设备可走内置标准 ZCL capability rules。
- **ESP32-C6 feasibility:** 高，若 metadata 在 flash、迭代输出 bounded。
- **RAM/flash/bounds:** 不复制 HA entity object；capability descriptors 迭代生成；strings 使用 string table/id；浮点比例可优先 rational `(num, den)` 避免不必要 RAM/precision 问题。
- **Persistent-network-state implications:** capability set 可从 signature+quirk DB 重建，不必单独永久存储。
- **Authorization/security:** command/write capability 只描述合法能力，不自行发送操作。
- **License/provenance:** selective `PORT` from Apache-2.0 metadata model；避免复制 Home Assistant UI/registry code。
- **Disposition:** **`L2 API` P0**。
- **Tests/interoperability:** standard temperature/occupancy/on-off/power；scale/unit/range；duplicate capabilities；primary selection；unknown vendor attr；quirk overrides standard metadata。

### ZH-4 — Cluster configuration plan aggregation

- **Exact upstream project/module:** `zha.zigbee.cluster_config.AggregatedAttrConfig`, `aggregate_cluster_configs()`, `configure_cluster_configs()`, `initialize_cluster_configs()`。
- **Behavior:** 多个 entity/capability 对同一 cluster 的 bind/read/reporting 需求先聚合；read-on-startup 取 OR；reporting 默认合并为更严格 interval/change，override 可替代默认；随后每 cluster 执行一次 bind/configure reporting，并记录逐 cluster/attribute outcome。
- **Zigbee layer:** ZCL/ZDO operation planning；实际发送属于 zigpy/native family。
- **Product value:** 避免同一 cluster 因多个 UI capability 重复 bind/report/config；给应用一个确定、可审计的配置清单。
- **Proposed C API/types:** `zha_config_plan_t`; `zha_cluster_plan_t`; `zha_attr_plan_t`; `zha_config_plan_build(signature, capabilities, ...)`；输出 `needs_bind`, `read_on_startup`, reporting tuple。**不提供执行函数调用 `zigpy_*`**；application 读取 plan 后自行执行。
- **L1 facilities required:** 无；执行者需要 ZDO bind/ZCL reporting/read。
- **Why L2:** 将多个高层能力折叠为最小、确定的配置需求是 orchestration/model behavior。
- **Coordinator/network prerequisites:** capability set 已建立。
- **ESP32-C6 feasibility:** 高；小型 bounded aggregation map/pool 即可。
- **RAM/flash/bounds:** cluster/attribute plan 数固定上限；溢出显式返回 partial/truncated。
- **Persistent-network-state implications:** plan 可重建；配置结果可短期缓存但不能假设设备永久保留。
- **Authorization/security:** plan 只描述标准 bind/report/read。
- **License/provenance:** `PORT` from Apache-2.0 ZHA semantics。
- **Disposition:** **`L2 API` P1**。
- **Tests/interoperability:** duplicate capability merge；reporting override precedence；read_on_startup merge；bind failure independent from reporting；partial plan overflow。

### ZH-5 — Value transform / manufacturer-specific handler subset

- **Exact upstream project/module:** zha-device-handlers custom clusters + QuirkBuilder converters/replacements；vendor quirk modules。
- **Behavior:** 修正厂商编码、属性位置、scale、enum、derived state；某些 quirk 将 manufacturer-specific payload 转为标准意义。
- **Zigbee layer:** manufacturer-specific ZCL/device semantics。
- **Product value:** 决定“识别了设备”之后是否真的能正确显示/控制大量非标准设备。
- **Proposed C API/types:** `zha_transform_id_t`; `zha_transform_decode()` / `zha_transform_encode()`；优先支持有限、可验证 opcode：scale/offset, enum-map, bitfield, invert, alias, const, endian/width conversion；复杂行为注册静态 `zha_vendor_handler_t`。
- **L1 facilities required:** 无 direct radio；输入输出均为 `zha_*` value/source types。
- **Why L2:** vendor payload → normalized value 的 parser/model 行为。
- **Coordinator/network prerequisites:** matched quirk + source attribute/command payload。
- **ESP32-C6 feasibility:** 中等；简单 transform 很可行，任意 Python callback 不可行。
- **RAM/flash/bounds:** opcode program/lookup table 在 flash；固定 recursion/step limit，禁止脚本式无限执行。
- **Persistent-network-state implications:** derived value cache 可重建。
- **Authorization/security:** encode 路径只生成合法 command/attribute semantics，不绕过 network security。
- **License/provenance:** declarative transforms `PORT/DATA`; arbitrary Python code `REFERENCE-ONLY` 或手工 clean-room handler。
- **Disposition:** `L2 API` P1/P2，按真实设备覆盖率分批。
- **Tests/interoperability:** round-trip where applicable；overflow/div-zero；unknown enum；manufacturer code；malformed payload；handler step limit。

### ZH-6 — Device automation trigger / action normalization

- **Exact upstream project/module:** `QuirkDefinition.device_automation_triggers`, ZHA event/platform metadata。
- **Behavior:** 将遥控器 button press、double press、hold 等 cluster command/event 解释为稳定 action trigger。
- **Zigbee layer:** ZCL command/event semantic normalization。
- **Product value:** 对遥控器、场景开关、门铃等设备很高，但不是最先实现 interview/read/report 的阻塞项。
- **Proposed C API/types:** `zha_action_t { action_id, endpoint, source_cluster, args... }`; `zha_action_decode(frame_or_event, matched_quirk, out)`。
- **L1 facilities required:** application 提供收到的 cluster command/event 结构；ZHA 不依赖 zigpy public type。
- **Why L2:** vendor/cluster event → stable action 的归一化。
- **Coordinator/network prerequisites:** matched quirk/capability metadata。
- **ESP32-C6 feasibility:** 高，采用 data-driven table。
- **RAM/flash/bounds:** bounded event args；string-id table。
- **Persistent-network-state implications:** 无需持久化。
- **Authorization/security:** receive-side normal event；任何 resulting control action 由 app 决定。
- **License/provenance:** `PORT/DATA` from Apache-2.0 metadata。
- **Disposition:** `L2 API` P2。
- **Tests/interoperability:** single/double/hold/release；unknown event；duplicate mappings；multi-endpoint remotes。

### ZH-7 — Tuya DP / large vendor-specific families

- **Exact upstream project/module:** `zha-device-handlers` Tuya builders/custom clusters and vendor files。
- **Behavior:** 大量 TS0601 等设备用 manufacturer-specific datapoints，不等价于标准 ZCL attributes；quirks 做 DP id/type/value 与 normalized capability 的映射。
- **Zigbee layer:** vendor-specific application payload。
- **Product value:** 潜在覆盖率很高。
- **Proposed C API/types:** 不在 Phase 1 固化通用 API；优先让 ZH-5 transform/handler extension point 能承载 DP parser，再单独做 Tuya data compiler/handler research。
- **L1 facilities required:** manufacturer cluster payload receive/send。
- **Why L2:** parser/mapper 是真实 vendor semantic；但范围过大，不应绑进 P0 核心。
- **Coordinator/network prerequisites:** joined/interviewed/matched device。
- **ESP32-C6 feasibility:** 中等；协议本身可处理，数据库覆盖和 flash 体积是主要问题。
- **RAM/flash/bounds:** DP database 不应全量展开到 RAM；flash/SD indexed data；payload strict length checks。
- **Persistent-network-state implications:** only derived mappings/state。
- **Authorization/security:** no security bypass。
- **License/provenance:** `DATA/PORT` for Apache-2.0 rules; complex custom code selective/manual。
- **Disposition:** `DATA + later L2 module`, not initial implementation。
- **Tests/interoperability:** representative sensor/switch/TRV/energy devices；malformed DP；unknown DP preserved/ignored safely。

### ZH-8 — Friendly names, alerts, HA registry/UI semantics

- **Exact upstream project/module:** `FriendlyNameMetadata`, `DeviceAlertMetadata`, entity registry/category/translation metadata, Home Assistant integration surface。
- **Behavior:** UI labels、translation、registry defaults、alerts、HA lifecycle。
- **Product value:** 部分字段以后可用于 One-OS UI，但并非 Zigbee L2 核心。
- **Proposed API:** 暂不建立完整 HA entity model；如后续 UI 需要，仅以 optional display metadata 从 quirk DATA 暴露。
- **L1 facilities:** none。
- **Why not core L2:** 与 host/UI lifecycle 强耦合，容易把 Home Assistant 应用层误搬进 firmware。
- **Coordinator/network prerequisites:** none beyond matched device。
- **ESP32-C6 feasibility:** 数据可行，完整 HA 模型不合理。
- **RAM/flash/bounds:** translation strings/large metadata 对 flash 影响大。
- **Persistent state:** none。
- **Authorization/security:** none。
- **License/provenance:** `REFERENCE-ONLY` for HA-specific behavior; selected Apache metadata may be `DATA/PORT` later。
- **Disposition:** `APP/DATA/DROP from core`。
- **Tests:** 不进入首批 L2 conformance。

## 6. 跨家族组合规则

必须维持：

```text
Application
├─ native ESP Zigbee SDK / ESP-IDF / FreeRTOS   (L1)
├─ zigpy_*                                      (L2 family A)
└─ zha_*                                        (L2 family B)
```

推荐组合方式：

1. `zigpy_*` interview 得到 `zigpy_device_snapshot_t`；
2. **Application** 显式复制需要的字段，构造 `zha_device_signature_t`；
3. `zha_quirk_match()` + `zha_capability_enumerate()` 产生 ZHA 自有 capability/config plan；
4. **Application** 根据 `zha_*` plan/source tuple 决定调用 `zigpy_attr_read_async()` / `zigpy_reporting_configure_async()` / `zigpy_command_invoke_async()`；
5. `zigpy_*` report event 到达后，Application 可把 source/value 转成 ZHA 自有类型，再调用 `zha_transform_decode()`；
6. 两个 family 不互相 include header、不保存对方 opaque handle、不返回对方 enum/type。

这会有少量字段复制，但符合 One-OS “peer API families / composition happens in applications” 的根规则，也避免以后任何一个 family 被替换时拖动另一个。

## 7. Memory / flash / bounds 策略

ESP32-C6 baseline 无 PSRAM，因此 Phase 2 设计应从第一天强制 bounded：

- 设备、in-flight requests、interview sessions、endpoint/cluster records、attribute cache、report queue、quirk patch ops、capability results均有 compile-time 或 config-time upper bound；
- API 采用 caller-provided buffer、fixed pool 或 iterator，不能隐藏无限 `malloc/realloc`；
- 超限必须返回 `TRUNCATED` / `BUFFER_TOO_SMALL` / partial mask，而不是静默丢数据；
- 从 ESP Zigbee SDK callback 获取的 ZCL `attr_value` 只在 message processing 生命周期有效，任何跨 callback 保存都必须立即拷贝到 bounded storage；
- upstream zigpy 当前 “Read Attributes 每请求最多 5 条 / write-config records 50 byte budget” 可作为互操作测试起点，不作为未经验证的硬性 One-OS 常量；应在 ESP32-C6 + 多厂商设备测试后确定；
- quirk database 放 flash，尽量 string-id/indexed；不要把全量 zha-device-handlers Python object/data 展开到 RAM；
- 大规模 vendor database 若最终需要，可评估 compact binary + flash/SD reader，但 runtime API 不依赖 SD 存在。

本阶段**不冻结具体 `MAX_DEVICES` 等数值**。这些数值应由 Phase 2 prototype 的 heap/stack watermark、真实多 endpoint device、ZCL payload 与 LVGL 并发占用测量后确定。

## 8. Security / authorization / trust-center 边界

- ESP Zigbee SDK 支持 Trust Center install-code requirement 和 install-code 管理；这应保留为 L1 security policy，不应在 L2 暴露 network-key extraction。
- `zigpy_commissioning_start()` 若实现，只提供明确 duration 的授权窗口；默认不支持永久 open (`0xFF`)。
- device leave/remove、factory reset、network formation 等会改变网络持久状态；第一批 API 不应把这些危险操作混入普通 interview/read API。
- 不实现：key extraction、未授权 network entry、security bypass、jamming、用于绕过安全的 replay、exploit delivery、第三方设备 hijack/persistence。
- 不把 passive 802.15.4 sniffing 当成 ZHA/zigpy “device discovery”。抓包/解码若以后需要，应属于独立 Wireshark/Kismet 等 project family。

## 9. 测试与互操作计划

Phase 2 若获批，最低测试面：

- **Host/unit fixtures:** ZDO Node/ActiveEP/SimpleDesc success/failure/timeout；ZCL read/write/report/default responses；malformed lengths；manufacturer-specific fields；late/duplicate responses；buffer overflow paths。
- **Interview fixtures:** standard light、temperature sensor、battery sleepy sensor、multi-endpoint plug/power strip、device missing Basic cluster、device with odd manufacturer/model read behavior。
- **Reporting:** OnOff、Temperature Measurement、Electrical Measurement；bind required / not required；multiple attrs one cluster；partial configure failure。
- **Quirk matching:** exact/manufacturer/model/wildcard/firmware boundary；raw signature preservation；bad quirk fallback。
- **Capability normalization:** standard + quirk-overridden sensor/switch/number/enum/action；scale/range/unit。
- **Persistence:** power cycle、NWK address change/rejoin、cache schema mismatch、network reset invalidation；确保 native `zb_storage` 与 L2 derived cache ownership 不冲突。
- **Resource:** heap low-watermark、Zigbee task stack high-watermark、LVGL 并发、max configured device/endpoint/cluster/report queue；无 PSRAM 条件。
- **Interoperability capture:** 使用标准 Zigbee coordinator/device test hardware 和公开商用设备；必要时用外部 sniffer 验证帧，但测试工具不进入 production API。

## 10. Sources

Upstream source snapshots:

- `https://github.com/zigpy/zigpy/tree/083d14fef7a2240920b67d98df11a55c6e4012ec`
- `https://github.com/zigpy/zha/tree/66603431339afe37fa0048b70ff31d77dceb8f95`
- `https://github.com/zigpy/zha-device-handlers/tree/6a3822c1348f9e40ad243eb881084a1c075da277`
- zigpy `device.py`: `https://github.com/zigpy/zigpy/blob/083d14fef7a2240920b67d98df11a55c6e4012ec/zigpy/device.py`
- zigpy `endpoint.py`: `https://github.com/zigpy/zigpy/blob/083d14fef7a2240920b67d98df11a55c6e4012ec/zigpy/endpoint.py`
- zigpy ZCL: `https://github.com/zigpy/zigpy/blob/083d14fef7a2240920b67d98df11a55c6e4012ec/zigpy/zcl/__init__.py`
- zigpy quirks-v2 compatibility shim: `https://github.com/zigpy/zigpy/blob/083d14fef7a2240920b67d98df11a55c6e4012ec/zigpy/quirks/v2/__init__.py`
- ZHA quirks registry: `https://github.com/zigpy/zha/blob/66603431339afe37fa0048b70ff31d77dceb8f95/zha/quirks.py`
- ZHA cluster config aggregation: `https://github.com/zigpy/zha/blob/66603431339afe37fa0048b70ff31d77dceb8f95/zha/zigbee/cluster_config.py`
- zha-device-handlers builder metadata: `https://github.com/zigpy/zha-device-handlers/blob/6a3822c1348f9e40ad243eb881084a1c075da277/zhaquirks/builder/metadata.py`
- zha-device-handlers custom device overlay: `https://github.com/zigpy/zha-device-handlers/blob/6a3822c1348f9e40ad243eb881084a1c075da277/zhaquirks/device.py`

ESP32-C6 native stack references checked against current ESP Zigbee SDK documentation:

- Introduction / supported features: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/introduction.html`
- Development / Zigbee task lock / coordinator example: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/developing.html`
- SDK API: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/api-reference/esp_zigbee_sdk.html`
- ZDO: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/api-reference/esp_zigbee_core/zdo.html`
- ZCL General Commands: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/api-reference/esp_zigbee_core/zcl/zcl_general_command.html`
- Security: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c5/api-reference/esp_zigbee_core/security.html` (API family is shared; Phase 2 should re-check ESP32-C6-target docs at implementation pin)
- v2 compatibility/migration notes, including `zb_storage` NVS: `https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32/migration-guide/v2.x/compat.html`

## 11. Prioritized `zigpy_*` API table

| Priority | Candidate | Proposed public surface | Upstream provenance | L1 prerequisite | Disposition |
|---|---|---|---|---|---|
| **P0** | Device interview + safe reinterview | `zigpy_interview_begin/cancel/get_snapshot`, `zigpy_reinterview_begin`, explicit phase/partial/truncated status | `zigpy.device`, `zigpy.endpoint` | ESP Zigbee ZDO + Basic ZCL + timers/lock | **L2 API / CLEAN-ROOM REIMPLEMENT** |
| **P0** | Bounded device model/enumeration | `zigpy_device_get`, endpoint/cluster iterators, independent C snapshot types | `zigpy.device`, `zigpy.endpoint` | interview events | **L2 API / CLEAN-ROOM REIMPLEMENT** |
| **P0** | ZCL transaction engine | `zigpy_attr_read_async`, `zigpy_attr_write_async`, `zigpy_command_invoke_async`, cancel/result | `zigpy.zcl`, request limiter/model | ESP Zigbee ZCL general/cluster commands | **L2 API / CLEAN-ROOM REIMPLEMENT** |
| **P0/P1** | Reporting + state events | `zigpy_reporting_configure_async`, attribute/report event stream/cache | `zigpy.zcl.Cluster`, attribute events | ZDO bind + ZCL configure/report | **L2 API / CLEAN-ROOM REIMPLEMENT** |
| **P1** | Authorized commissioning session | `zigpy_commissioning_start/cancel/status`, auto-close + join→interview | `ControllerApplication.permit`, join handling | BDB/ZDO permit + coordinator/TC | **L2 API only if orchestration is included** |
| **internal** | Derived device cache persistence | versioned bounded cache; minimal invalidate/reinterview semantics if public API needed | `zigpy.appdb` behavior only | ESP-IDF NVS, separate from `zb_storage` | **L2 internal / DATA; schema REFERENCE-ONLY** |
| later | topology/groups/OTA/backup/radio abstraction | none in first approval set | multiple zigpy modules | varies | **DROP from initial scope / separate research** |

## 12. Prioritized `zha_*` API table

| Priority | Candidate | Proposed public surface | Upstream provenance | Runtime form | Disposition |
|---|---|---|---|---|---|
| **P0** | Quirk/fingerprint matching | `zha_device_signature_t`, `zha_quirk_match()` | `zha.quirks` | compact registry + deterministic matcher | **L2 API / PORT** |
| **P0** | Capability normalization | `zha_capability_t`, bounded iterator/enumeration | `zhaquirks.builder.metadata`, ZHA platform semantics | flash metadata + rule engine | **L2 API / PORT** |
| **P0/P1** | Structural quirk overlay | `zha_quirk_apply_view()`, patch/view types | `zhaquirks.device`, QuirkBuilder structural ops | compact patches; raw signature preserved | **L2 API + DATA / PORT** |
| **P1** | Cluster config plan aggregation | `zha_config_plan_build()`; plan only, no `zigpy_*` execution | `zha.zigbee.cluster_config` | bounded aggregation | **L2 API / PORT** |
| **P1/P2** | Vendor value transforms | `zha_transform_decode/encode`, static vendor handler extension | zha-device-handlers custom clusters/builders | finite opcodes + manual handlers | **L2 API + DATA; complex Python REFERENCE-ONLY** |
| **P2** | Remote/action trigger normalization | `zha_action_decode()` | `QuirkDefinition.device_automation_triggers` | compact lookup table | **L2 API + DATA / PORT** |
| later | Tuya DP broad coverage | extension of transform/handler + generated vendor data | zha-device-handlers Tuya modules | indexed flash/SD data | **DATA + separate later L2 research** |
| app/data | friendly names, alerts, translation, HA registry lifecycle | no core API now | ZHA/quirk metadata / HA host behavior | optional display metadata only | **APP/DATA; HA-specific behavior DROP** |

## 13. Exclusions

| Excluded item | Reason / disposition |
|---|---|
| `zigpy.quirks.v2` as a new `zigpy_*` source | 2026 upstream explicitly moved quirks v2 to ZHA / zha-device-handlers; shim only。**Do not misattribute.** |
| Python `asyncio`, dynamic class/object replacement, plugin entry points | Host runtime implementation detail；ESP32-C6 用 FreeRTOS + fixed pools/state machines。**REFERENCE-ONLY/DROP** |
| Home Assistant entity registry, services, websocket, config entries, translation/UI lifecycle | Application/host integration，不是 Zigbee L2。**APP/DROP** |
| Zigbee2MQTT converters/vendor database | 不是 ZHA/zigpy provenance。若以后研究必须单独 project/source。**OUT OF FAMILY** |
| OpenThread / Matter / HA Bluetooth | 不属于 ZHA/zigpy。**OUT OF FAMILY** |
| Raw IEEE 802.15.4 passive sniffing as “ZHA discovery” | 与 coordinator/interview model 不同；应属于独立抓包/分析 family。**OUT OF FAMILY** |
| 对陌生第三方 Zigbee 网络做 key extraction、未授权 join、安全绕过、jamming、恶意 replay/hijack | 超出授权/安全边界。**DROP / PROHIBITED** |
| 把 `ezb_*` / `esp_zigbee_*` 原生函数逐一改名成 `zigpy_*` | 违反 One-OS “no rename-only wrappers”。**DROP** |
| `zha_*` 调用或公开 `zigpy_*` types，或反向依赖 | 违反 peer-family boundary；composition 必须在 application。**DROP** |
| 在 L2 复制 ESP Zigbee native network keys/datasets/counters | 原生 stack owns network state；会造成安全和一致性风险。**DROP** |
| 首批实现 topology/OTA/network backup 全套 host 功能 | 范围过大且 ownership 未厘清。**Separate research before any implementation** |
