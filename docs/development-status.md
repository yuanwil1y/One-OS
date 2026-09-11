# 开发状态与清理审计

审计日期：2026-09-11。代码基准：`de1afc42b73cb1695ff24bf25c1c77b92fe7958b`。

> 2026-09-11 续作更新见本文末「B0 进展」。B0 已完成并可验证；B1—B3 未开始。
> 第 8 节「分支处置」记录的 20 个候选分支**已实际删除**，见该节末尾。

## 当前结论

目前是“硬件基础 + 已汇总的协议组件 + 无 GUI 运行骨架”。不能用组件数量推算产品完成百分比。

| 部分 | 已有代码 | 尚未完成/验证 |
|---|---|---|
| 板级基础 | 共享 SPI、LCD、触摸、SD 驱动及 LVGL 绑定 | 当前 main 不挂载 SD；本次未连接实板 |
| HA | RAM 内 Device/Entity/State 模型、mDNS/SSDP | 应用设备映射、持久化、UI、控制派发 |
| Kismet / Wireshark | Wi-Fi/BLE 扫描会话和跟踪、有限协议解析 | 应用调度、原生无线初始化与整机联合验证 |
| Nmap | 有边界的 LAN 主机/端口/服务发现 | 已联网前提及产品调用链 |
| Theengs | Ruuvi RAWv2、部分 BTHome v2 被动解码 | 完整设备识别库；加密 BTHome 不支持 |
| ESPHome | 明文 Native API 子集、BLE GATT 与 NimBLE 后端 | Noise 未实现；Native API command 明确返回不支持，不能宣称可控制 |
| ZHA / zigpy | quirk/能力转换、后端回调驱动的 interview/ZCL 事务逻辑 | 仓库没有具体原生 Zigbee backend；预留分区不等于协议栈已接通。interview 超时与重试边界已修，见下 |
| OpenThread | 网络发现、状态/拓扑、dataset attach、Joiner | 原生栈生命周期由应用负责；不能等同 Matter 控制器 |
| Matter | 独立研究分支有未合并代码 | 最新构建失败；保留分支单独修复 |
| 产品应用 | 三份 application 规范 + B0 无 GUI 运行骨架 | 配网、Web 管理、SD DB、统一 Device/Entity 页面、控制回执闭环尚未实现 |

这些名称表示有限范围的适配/独立实现，不能理解为完整移植了同名上游产品。

## 验证证据

- main 的 [CI 34150437719](https://github.com/yuanwil1y/One-OS/actions/runs/34150437719) 成功，对应上述审计 SHA。
- Matter 的 [CI 34336821940](https://github.com/yuanwil1y/One-OS/actions/runs/34336821940) 失败，对应 `d562569ee9ea53f50bba371b1076c139d84adac9`。
- 本次复跑八组现有 host tests。ESPHome、Theengs、ZHA/zigpy、OpenThread、Kismet、Nmap 原有命令通过。
- HA、Wireshark 默认 LeakSanitizer 在当前容器无法读取 `/proc/.../task`，属于运行环境限制。保留 ASan/UBSan、关闭 leak 检查后两组通过；没有修改仓库测试以掩盖限制。
- 当前环境没有 `idf.py`，没有重新本地编译 ESP32-C6，没有进行烧录、射频互操作、内存峰值或实板稳定性验证。历史 CI 构建成功也不证明未被启动调用的组件已在实板工作。

## 为什么显得混乱

1. README 仍描述 L2 合并前状态，与 PR #12 后代码不一致，本次修正。
2. 多个研究分支通过集中整合进入 main，并非逐分支 merge；仅看 Git 的 merged 标志会误认为全都未完成。
3. 研究设计、组件 host 测试通过、整机产品可用是三种不同状态，之前缺少统一入口说明。
4. 测试分布在 tests、tools、组件目录，但仍被 CI 使用，不是可直接删除的垃圾。
5. 部分研究分支含独有 AGENTS.md 任务说明。它们不代表未合并固件，但删除前应备份历史。

## 分支处置

审计时共 22 个分支。保留 `main` 与 `research/matter-chip-tool-l2-api`。
其余 20 个候选名称与完整 SHA 见 [清理清单](../tools/branch-cleanup-candidates.tsv)。

| 分支组 | 判定依据 |
|---|---|
| beta/smoke-v0.1.0-beta.2 | PR #2 已合并；firmware 与 v0.1.0-beta.2 标签一致 |
| cleanup/foundation-clean、cleanup/remove-smoke-beta2 | PR #1/#3 已合并；后续 main 已继续演进 |
| integration/l2-runtime、tmp/test | 提交已是 main 祖先 |
| integration/l2-runtime-work | 剩余独有改动仅 integration placeholder |
| 非 Matter research 分支及 HA checkpoint | 对比各分支相对共同祖先的改动，组件/测试/研究文档已包含在 main；剩余差异为分支任务说明或已被集成配置取代的 CMake/CI |
| tmp-do-not-use 及 -2/-3/-4 | 四者同 SHA；剩余分支任务说明，无独有固件实现 |
| tmp/openthread-main-sync | 剩余任务说明与旧 sdkconfig；main 已补入 DTLS/EC-JPAKE 配置 |

PR #5 的 ZHA/zigpy 内容已通过 PR #12 纳入，旧 draft 已于 2026-09-11 关闭，不能再次整体合并研究分支。
保留 beta.1/beta.2 标签和 Releases。

### 执行结果（2026-09-11 续作）

删除前重新审计，20 个候选 SHA 与清单完全一致，未出现新增独有成果：

- 3 个 smoke/cleanup 分支的树与 `main`/`v0.1.0-beta.2` 标签中已有提交**逐字节相同**
  （`beta/smoke-v0.1.0-beta.2` 与标签提交树哈希同为 `d3aabc9d`，且父提交相同）。
- 其余分支剩余差异只有分支任务说明 `AGENTS.md`、已被集成配置取代的 CMake/CI 子集，
  以及删除 smoke 应用的提交。

已完成全量镜像备份后，用逐分支 `--force-with-lease` 加 `--atomic` 一次性删除 20 个 ref。
当前远端仅剩 `main` 与 `research/matter-chip-tool-l2-api`，两个 beta 标签与 Releases 完好。
脚本 `tools/cleanup_remote_branches.py` 保留，重复执行只会报告候选已不存在。

## 当前开发顺序（用户已更新）

先完成无 GUI 的底层和应用闭环，最后接 GUI。详细任务、依赖、验收与新增边界缺口见 [GUI 之前的开发任务书](pre-ui-development.md)。之前“先接屏幕设备列表”的建议已被此顺序替代。

继续遵守三份 application 文档的最终产品行为：生产识别库只放 SD、未知设备不丢弃、最终使用统一 HA Device/Entity UI。每一步记录提交 SHA、host/CI 证据及单列的实板验收结果。

## B0 进展（2026-09-11）

分支 `feat/b0-diag-entry`，基于 main `c175db5`。

### 已完成并验证

| 交付 | 提交 |
|---|---|
| 统一 host 测试入口 + 无平台依赖诊断协议层 | `4a6bb5a` |
| 修正 dispatcher（清单文件替代 bash 数组）+ 操作门 `app_ops` | `d9cf5a1` |
| Zigbee interview 超时与重试边界修复 | `549562e` |
| 无 GUI 运行骨架：worker、串口入口、资源报告 | `42beb6c` |
| 修正诊断错误名、补声明、修 interview 首次 poll 误刷新 deadline | `7c36791` |

**CI 证据**（run `34606978491`，两者均 success）：

- host tests：10 组全部通过，`failed groups: 0`。
  新增组 `app_diag_protocol`（109 checks, 0 failures）与 `app_ops`（109 checks, 0 failures）。
- build：ESP-IDF v6.1 / esp32c6 固件构建成功（约 4 分 20 秒）。

### B0 能力现状（修正后的准确表述）

B0 的"完成"只覆盖**构建与诊断骨架**，不代表任何真实后端被调用。此前把 B0 描述为
"已完成"过于笼统，这里按三类分开说明：

| 类别 | 状态 |
|---|---|
| 诊断骨架（入口、协议、操作门、worker、串口收发、资源报告字段） | 已实现并 host 测试通过 |
| 真实后端调用（扫描、解析、识别、控制） | **B0 阶段全部为空实现**；B1—B3 已补齐 RF/LAN 与设备状态，见下节 |
| 资源测量（真实 heap/栈余量数值） | **未测量**。字段已实现，但没有任何实板数值，不得引用构建日志推断 |

- `tests/run_all_host_tests.sh` 是唯一入口，读取 `tests/host-test-groups.txt` 清单并调用
  各组原有 runner，不复制测试。CI 先 `--list` 再执行同一入口。
- 串口入口支持 `ping/version/status/resources/scan/cancel/devices/entities/control`，
  响应含 request id、阶段、错误、partial/truncated 标志。
- 资源报告含 free heap、min free heap、largest free block、worker/console 栈余量、
  队列丢弃数、generation 与阶段进度；不含任何凭据。**字段存在 ≠ 已测量**。
- `control` 返回 `not_implemented`，不伪装成控制成功。
- Zigbee 修复已加回归测试：无回调 interview 超时收尾、`retries=255` 不回绕、
  失败 re-interview 保留 last-known-good。

## B1—B3 进展（2026-09-11）

分支 `feat/b1-b3-headless-scan`。目标是无 GUI 的"真实 Wi-Fi/BLE 扫描→解析→应用设备状态→
串口枚举"链路。

### 关键资源事实（读代码确认，不是推测）

- `kismet_wifi_session_start()` **自己调用 `esp_wifi_init()`**，且在
  `esp_wifi_get_mode()` 成功（驱动已初始化）时返回 `ESP_ERR_INVALID_STATE`；
  结束时执行 `esp_wifi_stop()` + `esp_wifi_deinit()`。
  因此 STA 联网与被动扫描在**驱动层**互斥，不只是调度层互斥。
- `kismet_ble_session_start()` **自己执行 `nvs_flash_init()` 与
  `nimble_port_init()`..`nimble_port_deinit()`**，应用不得重复初始化 NimBLE。

### 已实现

| 交付 | 说明 |
|---|---|
| `app_wifi.{c,h}` | 唯一的 STA 生命周期拥有者；显式交接 release → 扫描 → restore；恢复失败如实报告，未配网是正常状态 |
| `app_scan.{c,h}` | 无平台依赖的阶段策略与有界证据表（Wi-Fi/BLE+解析结果/LAN） |
| `app_scan_native.c` | Kismet Wi-Fi 会话 + Wireshark 管理帧解析；Kismet BLE 会话 + Wireshark AD 解析；HA mDNS/SSDP；Nmap |
| `app_device.{c,h}` | 应用绑定表；协议内命名空间身份、代次、离线/清除、只读约束 |
| `kismet_wifi_tracker_get_device_ssid()` | 设备与 SSID 分表存放，应用无法仅凭设备表给 AP 命名 |
| `ha_core_device_remove()` | 清除设备时一并移除其 Entity/State，避免孤立残留 |

回调数据在**回调内部**复制进证据表，不保留会话指针；地址、地址类型、RSSI、信道、
时间、截断与丢弃计数均保留。

### 阶段实现状态（必须逐项如实报告）

| 阶段 | 状态 |
|---|---|
| `wifi_rf` | **已接入真实后端** |
| `ble_rf` | **已接入真实后端** |
| `mdns` / `ssdp` | **已接入真实后端**，仅在获得 IPv4 时执行 |
| `lan_hosts` | **已接入真实后端**（Nmap 主机发现） |
| `lan_services` | **仍为空实现**，返回 `not_implemented`，记为 `skipped` |
| `thread` / `zigbee` | **仍为空实现**，记为 `skipped` 并带明确原因 |
| `enrichment` | **仍为空实现**（无 SD Device DB，无 profile 可依据） |
| `materialize` | **已接入** `ha_core` |

局部失败不会伪装成全协议完成：未接入的阶段一律 `skipped` + 原因，`partial=1`。

### CI 证据（run `34612225371`，两者均 success）

- host tests：**12 组全部通过，`failed groups: 0`**。
  新增 `app_scan`（114 checks, 0 failures）与 `app_device`（118 checks, 0 failures）。
- build：ESP-IDF v6.1 / esp32c6 构建成功。
  `one_os.bin` = 0x1834F0（约 1.52 MB），app 分区 0x300000，剩余 0x17CB10（50%）。
  **该数字来自 CI 构建日志，不是实板资源测量。**

host 测试断言的是规则而非 mock 行为：未接入协议被判为 skipped 而不是 done；
LAN 阶段需要 IP；同一串字节在 Wi-Fi 与 BLE 中保持为两个不同设备；未知设备保留且只读；
未观测到的值不生成 Entity；重复物化不增长；十个代次的不同 AP 不累积；
容量溢出被报告；恶意 SSID 无法注入控制字符；ha_core 拒绝插入时不留下孤儿绑定。

### 仍然阻塞 / 未完成

- **B4—B11 未开始**（Device DB、SD 读取与匹配、配网与 Web 管理、BLE GATT/ESPHome、
  Zigbee 原生后端、OpenThread/Matter、统一控制与确认、整机验收）。
- `lan_services`、`thread`、`zigbee`、`enrichment` 四个阶段仍为空实现。
- **实板验证全部待办**：本轮无实板，未烧录、未做射频互操作、未测量内存峰值、
  未验证串口真实输出与恢复行为。CI 通过只证明编译与 host 规则测试。
- Matter 独立构建仍未修复（在 `research/matter-chip-tool-l2-api`）。

