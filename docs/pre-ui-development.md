# GUI 之前的开发任务书

日期：2026-09-11。基于 main `bad20fc2c9c2a70e0f9f191976967a751c884aaf`。
用户顺序：先跑通底层与应用逻辑，最后补 GUI。本文件是当前执行顺序；三份 application 规范继续定义最终产品行为，不要求提前做页面。

## 给接手 Agent 的指令

从最新 main 开始，先读本文件、development-status.md、三份 application 规范及对应组件头文件。不要整体合并旧 research 分支，不重写已有协议组件，不把“编译/fixture 通过”写成“实板已完成”。
本轮先按任务推进无 GUI runtime；LVGL 基础保留，但不开发 Device 列表、Settings、弹窗、动画或网页视觉样式。串口测试入口只负责提交应用请求、读状态快照，不能另建一套业务逻辑。
开发前固定实际使用的 ESP-IDF/组件版本，核对目标版本原生 API。以下函数/文件建议为任务设计，不代表仓库已有实现。

每项交付：代码提交 SHA、变更范围、可重复的测试命令/fixture、CI 链接、已知限制。硬件测试缺设备时标“待实板”，不得编造通过结果。
避免多任务共同修改 main.c/CMake/sdkconfig/分区表；由同一集成人统一处理这些共享文件。

## 执行顺序与依赖

| ID | 任务 | 前置 | 交付 / 验收重点 |
|---|---|---|---|
| B0 | 构建与诊断入口 | 无 | 可复现 C6 构建、串口请求/状态出口、真实组件链接与资源报告 |
| B1 | 原生生命周期与操作互斥 | B0 | Wi-Fi/NimBLE/802.15.4/存储所有权，启动失败、取消、切换可恢复 |
| B2 | 无 GUI 扫描链 | B1 | Wi-Fi/BLE → 字节解析 → 有界证据；联网后 mDNS/SSDP/Nmap；部分失败明确 |
| B3 | 无 GUI Device/Entity 状态 | B2 | 未知设备、稳定身份、代次、在线/过期；串口可枚举快照 |
| B4 | Device DB 格式与主机生成器 | B0 | 版本化二进制格式、索引、来源、验证器与确定性 fixture |
| B5 | SD 读取与设备识别/实体配方 | B3+B4 | 有界读取、唯一匹配器、decoder/quirk 选择、无 DB 降级 |
| B6 | Wi-Fi 持久化与 Web 管理后端 | B1；上传需 B4+B5 | NVS、APSTA、HTTP API、分块上传/恢复；页面后做 |
| B7 | BLE GATT 与 ESPHome 真实后端补齐 | B1 | BLE 所有权交接、GATT 互操作、ESPHome Noise 与认证控制 |
| B8 | Zigbee 原生后端与事务补齐 | B1 | 真正 coordinator/ZDO/ZCL、interview 超时、report、持久化 |
| B9 | OpenThread 与 Matter 集成 | B1；Matter 独立修复 | Thread 原生生命周期、Matter 构建/配网/订阅及资源可行性 |
| B10 | 统一控制与确认状态 | B3+B5；逐个依赖 B7/B8/B9 | 一个 Entity 一个后端；pending/confirmed/failed、超时、迟到回调 |
| B11 | 整机无 GUI 验收 | 以上目标能力 | 真实设备闭环、失败恢复、重复运行、Flash/RAM/栈余量 |

先完成 B0→B1→B2→B3，得到“扫描→设备状态→串口输出”的可检验闭环。
B4 可独立开发；之后 B5/B6。B7/B8/B9 分后端推进，不让 Matter 构建问题阻塞其他能力。
GUI 开始前，至少完成目标版本所需后端的 B10/B11；未支持协议明确列入未完清单，不能静默删掉目标能力。

## B0 — 构建与诊断入口

- 保留现有八组 host tests，提供统一运行入口，复用原测试脚本，不复制测试实现。
- CI 保留 ESP-IDF v6.1 / esp32c6 基准，新增组件需明确依赖版本和兼容性；不要为压绿 CI 随意升级或移除组件。
- 实际调用待测 runtime API，检查 map/size，避免依赖列在 REQUIRES 却因未引用被链接器裁掉，产生虚假的整机体积结论。
- 使用开发配置下的串口入口提交 scan/cancel/status/devices/entities/control 等请求；响应含 request ID、阶段、错误、partial/truncated 标志。控制不是默认启动动作。
- 记录 heap 最低值、最大可分配块、任务栈余量、队列丢弃数和固件版本。不输出 STA 密码或协议密钥。
- 验收：相同提交可重复构建；诊断入口调用未来 GUI 将使用的同一套应用逻辑。无实板时提交构建与 host 证据，串口实测单列待办。

## B1 — 生命周期、所有权和互斥

- 补 NVS、event loop、netif、Wi-Fi STA、各原生栈与存储启动/停止、失败回滚。board 继续只负责板级事实。
- 为 Scan / Control / Commissioning / Web Management 建应用级操作状态机，返回 BUSY、取消和终态，不新增通用 radio_runtime 框架。
- 核对每个现有组件实际拥有的资源：例如 kismet_ble.h 明确写扫描 session 拥有 NimBLE host 生命周期，不能再由应用无条件重复初始化。
- Wi-Fi 监听与 STA 恢复、BLE scan 与 GATT/Matter commissioning、Zigbee 与 OpenThread 的 802.15.4 所有权需要明确交接。串行执行是初始策略，但不能假设“加个互斥锁”就解决协议栈配置/释放兼容性。
- 若 Zigbee/Thread 原生 SDK 不能在同一构建或同次启动安全切换，先给出编译/最小实验依据，明确可支持模式和待解决限制。
- 回调只拷贝有界数据进队列；应用单 owner 修改 HA/绑定表；有限超时、generation/request ID 过滤取消后的迟到事件。
- 验收：重复启动/停止、初始化中途失败、扫描取消、门户退出后能再次扫描；无重复 event handler、重复释放或悬空回调。

## B2 — 扫描与证据链

- 组合 Kismet Wi-Fi/BLE session 与 Wireshark parser；保留原始长度、捕获长度、地址类型、时间、RSSI 与截断标志。
- 现有 BLE report callback 已带有界字节，不必重写扫描器；必须复制后再跨任务使用，不能只用 tracker 名单而丢失识别数据。
- 有 IP 后调用 HA mDNS/SSDP 与 Nmap；无 Wi-Fi 时标记 LAN 阶段 skipped/unavailable，保留 RF 结果。
- 有限阶段状态机：start、progress、cancel、partial、done；队列满/表满、畸形报文、丢包、掉网均有显式结果。
- Thread/Zigbee 阶段等 B8/B9 后接入；扫描不能自动入网、配对或发送控制。
- 验收：fixture 覆盖畸形/截断/重复/超限；实板确认 Wi-Fi/BLE 真实结果和扫描结束后网络恢复。

## B3 — Device / Entity / State，无 GUI

- 使用已有 ha_core，补应用绑定表、typed evidence 转换、代次/TTL、去重与 unavailable。
- 缺 SD、DB 缺失/损坏、NOT_FOUND/AMBIGUOUS 都能生成通用只读 Device。
- 不凭 RSSI、名称、SSID、IP 或随机 BLE 地址跨协议合并设备；只使用有明确依据的稳定身份规则。
- 先提供 generic 只读字段和串口快照；协议对象/回调指针不能进入 HA 状态。
- 设备掉线、重新扫描、相同设备更新不无限增长；已授权节点离线保留身份。
- 验收：没有 LVGL 页面也可列出设备、实体、状态和 stale/unavailable；重复扫描不会无界增长。

## B4 — 主机 Device DB 工具和格式

- 新建 tools/device_db 的 builder/validator、来源清单、极小合规 fixture；不是先下载大规模数据库堆进固件。
- 明确 magic、schema/container/reader ABI、文件长度、索引范围、校验、profile ID、identity、decoder/quirk ID、Entity recipe、来源版本。
- 生成结果可复现；主机与固件校验规则一致；范围计算需检查溢出、重叠/越界、错误长度。
- 生产唯一完整语料位于 SD，不在 flash 内藏第二套 fallback matcher；fixture 仅测试。
- 验收：确定性生成、缺来源拒绝、损坏索引/错版本/截断/未知 decoder ID 被拒绝或明确降级，不产生可写控制。

## B5 — SD、匹配、decoder 与 Entity 配方

- 挂载路径明确映射到 /nearby/db/devices.nbdb；复用 board 的共享 SPI，不重复创建/释放 LCD/SD 总线。
- 只保留少量索引和固定缓冲区，按需 seek/read；数据库 I/O 在 worker/storage 路径。
- 一个应用 Device DB 负责所有 fingerprint 匹配。选择 Theengs decoder、ZHA quirk，再由应用复制数据调用各独立家族。
- 设备识别失败仍保留 generic Device；匹配成功后补 Entity/单位/量程/后端绑定。
- 验收：无卡、错版本、坏文件、读错误、拔卡、重开、歧义匹配；不能靠整库载入 RAM 才通过。

## B6 — 配网与导入后端，页面后做

- NVS 保存 STA 凭据和版本标记；重启自动连接；连接失败不阻塞本地扫描。
- 临时 APSTA 会话与扫描/控制互斥；支持启动、取消、结束后恢复；随机临时 AP 密码只经明确的本地开发出口提供，不进入普通日志/HTTP status。
- 实现 /api/status、/api/wifi/scan、/api/wifi/connect、/api/db/upload，必要时 /api/portal/finish；先用 HTTP 客户端验收，不开发 HTML/CSS。
- 固定缓冲区流式写 .part，核对字节数、上限、空间、格式/校验/来源；写完 flush/fsync，再协调关闭 reader、替换并重新打开。
- FAT 上不要把 rename 自动当作断电原子事务；设计 .old/恢复状态并测试各阶段失败，保留上一个有效 DB。
- 不移植旧 /api/db/format、全卡格式化、session-only 配网。旧项目只按 provisioning 文档选择性迁移，不能带回旧扫描框架。
- 验收：错误密码、断网、超大上传、中断、磁盘满、校验失败和替换恢复；不损坏无关 SD 文件。

## B7 — BLE GATT / ESPHome

- 补并实测扫描结束后 NimBLE→GATT 的所有权交接、服务发现、read/write/notify、断连、超时/取消、迟到回调和缓冲区生命期。
- 当前 ESPHome Native API 只有明文 slice；Noise PSK 形状校验不是认证。补可互操作的 Noise 认证传输，再接已有命令编码，不能简单删掉 ESP_ERR_NOT_SUPPORTED 来宣称完成。
- 对认证失败、错误 PSK、重连、订阅恢复、未知 Entity 类型给明确状态；认证资料按应用/原生持久化机制保存，不放识别 DB。
- 验收：fixture/协议向量 + 一台真实 ESPHome 节点及 BLE 外设的结果；无设备就标待互操作验证。

## B8 — Zigbee 真实后端及已发现的边界缺口

- 引入与所选 IDF 兼容的原生 Zigbee SDK，落实 coordinator 启动/网络恢复、原生任务/锁、有限 permit-join、join 事件。
- 实现 zigpy_backend_ops 的 ZDO/ZCL 回调和请求关联，不以 fake backend 替代生产无线实现；核对 SDK 所需所有持久化分区。
- 补主动 report/attribute 事件到应用的通路、TSN/地址/endpoint/cluster 关联和有限重试/取消。
- 已查明：zigpy_poll 目前只处理 commissioning 和 ZCL transaction deadline，interview 等不到回调时没有同等超时收尾。补每阶段/整体 deadline，测试不返回的 backend 与迟到回调。
- 已查明：transaction attempts 为 uint8_t，retries 可为 255；poll 递增可能回绕并持续重试。限制合法重试次数或用不会回绕的计数，增加边界测试。
- re-interview 开始会清 snapshot：由组件或应用保留 last-known-good，失败不得毁掉现有设备资料。
- 验收：一台标准设备入网→interview→读→控制→report，重启仍恢复原网络；sleepy/不响应设备有限退出，255 等极值不无限重试。

## B9 — OpenThread / Matter

- OpenThread 的 L2 函数依赖已初始化的原生栈：补 port、netif、任务/锁、退出、dataset 保存/恢复及与其他 RF 阶段交接。
- 验证 Thread discovery/authorized attach/Joiner；不要把 Thread 网络发现描述成任意 Thread 应用设备可识别/可控制。
- 保留 research/matter-chip-tool-l2-api：从其最新失败 CI 定位依赖/配置问题，固定 connectedhomeip/submodule/IDF 版本，先拿到可复现 C6 build。
- 再验证 controller 初始化、fabric/密钥持久化、commissioning、IM read/write/invoke/subscribe、掉线重订阅。与扫描/门户协调。
- 记录真实 map、Flash、heap/stack；若超预算或与 Zigbee/Thread 生命周期冲突，提交具体证据和可行取舍，不直接把未完成代码混入 main。
- Matter 单独提交，构建失败不能标记协议完成。

## B10 — 控制与状态确认

- GUI 将只提交 Entity + operation + value；诊断入口先调用完全相同的应用入口。
- 绑定校验、能力掩码、参数范围、唯一后端路由、transaction ID、pending/confirmed/failed/timeout、取消和过期绑定。
- 发送成功不等于设备状态改变；按协议 response/report/readback 更新 observed state，命令确认与状态确认必要时分开。
- 不支持或歧义设备保持只读；非幂等命令不能不加区分重复发送；掉线/重连不重放旧操作。
- 验收：模拟后端测乱序/迟到/重复/超时；随后逐后端做真实设备闭环。

## B11 — GUI 前交付门槛

- 无 GUI 即可通过同一应用入口完成 scan→parse→match/generic→Device/Entity→control→confirmed state。
- 覆盖无 Wi-Fi、无 SD/DB、坏 DB、容量上限、掉网、取消、重复扫描、门户开关、协议切换和重启恢复。
- 记录重复运行（建议至少 100 轮）与资源低水位；GUI 的 LVGL heap/display buffers 保留预算，不因 headless build 暂时省内存就宣布余量足够。
- CI/host 通过与实板验证分列。没有实板时交付“软件验证完成，硬件验收待办”，不能据此开工大规模 GUI。
- 最终再复用原 HA 风格 Device/Entity/Settings 与 Web 页面；界面只消费快照和调用应用命令。

## 清理交接

已修正 README，关闭已被 PR #12 整合的 PR #5。

**已执行（2026-09-11 续作）**：20 个旧分支在重新审计确认 SHA 未变、无新增独有成果后，
经全量镜像备份，用逐分支 lease 加 atomic push 全部删除。当前远端仅剩 `main` 与
`research/matter-chip-tool-l2-api`，beta 标签与 Releases 完好。执行细节见
[development-status.md](development-status.md) 的「分支处置」。

`tools/cleanup_remote_branches.py` 保留用于复核，重复执行只会报告候选已不存在。
若 main 的代码已变化，脚本仍会停止并要求重新审计；不得移除保护强行运行。

保留 main、Matter 研究分支、beta 标签/Releases、现用测试、来源记录。docs/research 是历史调研，不等于当前任务授权或实现状态，按当前代码及本任务书判断。

## 执行记录：B0（2026-09-11）

分支 `feat/b0-diag-entry`。CI run `34606978491` 全绿：ESP-IDF v6.1 / esp32c6 构建成功，
10 组 host tests 全通过（`failed groups: 0`）。

已交付：

- `tests/run_all_host_tests.sh` 唯一 host 测试入口，读取 `tests/host-test-groups.txt`
  清单调用各组原有 runner，不复制测试。`LEAK_SANITIZER=0` 只关 LeakSanitizer 的 leak 检查，
  ASan/UBSan 仍生效，未改动任何仓库测试。
- `firmware/main/app_diag_protocol.{c,h}`：无平台依赖的请求/响应层，含 request id、阶段、
  阶段状态、错误与**相互独立**的 partial/truncated 标志。
- `firmware/main/include/app_ops.h` + `app_ops.c`：应用级操作门与扫描代次/阶段生命周期，
  同样无平台依赖，固件与 host 测试编译同一份源码。
- `firmware/main/app_runtime.c`：单 worker 任务拥有全部状态变更；调用者提交请求并阻塞到
  自己的响应写回，响应缓冲区不经队列复制，协议回调不得触碰 HA/LVGL。
- `firmware/main/app_diag_console.c`：仅传输层，把请求交给 runtime 并打印返回报告。
- Zigbee 边界修复（`549562e`）：interview 无回调时的超时收尾、`retries=255` 计数回绕、
  失败 re-interview 保留 last-known-good。

**未完成**：每个扫描阶段体仍是空实现，记为 `skipped` 并返回 `not_implemented`；
`control` 同样返回 `not_implemented`。因此 B0 只完成了“构建与诊断入口”，
B1—B3 的“真实扫描→设备状态→串口输出”尚未跑通，不得宣称已完成。
本轮无实板，所有实板验收仍待办。

**后续修正**：B0 的“完成”一度被笼统表述为已跑通，实际只覆盖诊断骨架。
准确区分见 [development-status.md](development-status.md) 的「B0 能力现状（修正后的准确表述）」：
诊断骨架已实现且 host 测试通过；真实后端调用在 B0 阶段全部为空实现；
资源报告字段已实现但**从未测量**，不得用构建日志推断资源余量。

## 执行记录：B1—B3（2026-09-11）

分支 `feat/b1-b3-headless-scan`。CI run `34612225371` 全绿：ESP-IDF v6.1 / esp32c6 构建成功，
**12 组 host tests 全通过**（`failed groups: 0`），新增 `app_scan` 114 checks 与
`app_device` 118 checks。

已交付：`app_wifi`（STA 生命周期与扫描交接）、`app_scan`（阶段策略 + 有界证据）、
`app_scan_native`（Kismet/Wireshark/mDNS/SSDP/Nmap 真实调用）、`app_device`
（应用绑定表 + 代次 + 只读约束），以及 `kismet_wifi_tracker_get_device_ssid()` 与
`ha_core_device_remove()` 两个必要补充。

**阶段实现状态（逐项）**：`wifi_rf`、`ble_rf`、`mdns`、`ssdp`、`lan_hosts`、`materialize`
已接入真实后端；`thread`、`zigbee`、`enrichment` **仍为空实现**，记为 `skipped` 并携带明确原因，
因此局部扫描不会被呈现为全协议完成。`lan_services` 随后在同一轮补完，见下。

**实板验证仍全部待办**：未烧录、未做射频互操作、**未测量任何 RAM/栈余量**、未验证串口真实输出与
有线恢复行为。CI 通过只证明编译与 host 规则测试，不能替代实板结论。

## 执行记录：lan_services（B2 收尾，2026-09-11）

`lan_services` 原以 `not_implemented` 记为 `skipped`，现接入真实 Nmap 服务探测。
范围刻意收窄，因为它是唯一会向其他设备发起 TCP 连接的阶段：目标只取已判定 up 的主机
（上限 16），固定 8 个知名端口（HTTP/HTTPS/SSH/Telnet/ESPHome 6053/MQTT/RTSP/9100），
仅 PASSIVE 探测档且捕获上限 256 字节，不写入、不尝试凭据、不发协议专用载荷，
端口扫描与服务扫描共用一个 deadline 并在两半之间检查取消。
没有开放端口不算失败；无目标可探测返回成功而非错误。

**未验证**：该阶段只能在 ESP-IDF 下编译，host 无法执行；端口探测与服务识别
**尚未对真实局域网验证**，标为待实板验证。

CI run `34622080671` 全绿：**14 组 host tests、`failed groups: 0`**，esp32c6 构建成功。

## 执行记录：B4（2026-09-11）

已合并。CI run `34620547055` 全绿：14 组 host tests（新增 `device_db_python` 36 checks、
`device_db_format` 200 checks），esp32c6 构建成功。

交付 `.nbdb` 容器规范（[device-db-format.md](device-db-format.md)）、固件校验式读取器、
确定性主机生成器、独立验证器，以及 29 个单一变异的损坏样本。
fixture **仅用于测试**，不被固件构建引用；生产语料只放 SD。

**关于旧项目格式**：旧 NearBy One NEXT 源码在本环境中**不可用**，仓库内只有 provisioning
文档第 9 节的复用矩阵，其中没有文件布局。因此本容器从零定义，**不假设与旧 `.nbdb` 线格式
兼容**，并以 `format_version`/`schema_version`/`reader_abi` 三个版本字段保证无法识别的文件
被判为 `INCOMPATIBLE` 而非误读。若日后取得旧格式，可写一次性转换器，无需改动读取器。

下一步为 B5（SD 按需读取、匹配、decoder/quirk 选择、Entity 配方，并实现 `enrichment` 阶段），
随后 B6。

## 执行记录：B5（2026-09-11 续作）

分支 `feat/b5-recognition`（PR #20）。逐项状态、测试命令与未完成清单见
[交接记录](handover-ledger.md)，内存预算见 [识别内存预算](recognition-budget.md)。

**本轮修掉一个致命缺陷**：`app_device_db.c` 的 `db_read()` 以 `db->open` 为门，而
`open` 只在全部校验完成后置位，因此第一次读头部就失败，读取器在真实硬件上
**永远不可能打开任何库**。当时没有任何测试覆盖该路径。现已改为以 `opened` 为门，
并新增 `app_device_db` 测试组（161 checks）端到端覆盖。

已交付：SD 存储适配器（复用 `board_sd_mount`，全程持有一个文件句柄）、
库变更检测与显式全量重校验、配方与 decoder/quirk 校验、真实 `enrichment` 阶段
（一次匹配全部证据后物化）、`resources` 中的库状态与路径、内存预算文档。

同时处理任务书第三节的四个边界问题：会话销毁超时后的无线资源隔离与回收、
两轮设备新鲜度（STALE 与 UNAVAILABLE 语义分开）、Wi-Fi 事件代次审计与修正、
库文件变化的两级检测。

**验收条件核对（B5）**：

| 验收项 | 状态 |
|---|---|
| 无卡、错版本、坏文件、读错误、拔卡、重开、歧义匹配 | host 覆盖（stub 存储注入） |
| 不能靠整库载入 RAM 才通过 | 读取器约 9 KiB 且与语料规模无关，见预算文档 |
| 挂载路径映射到 `/nearby/db/devices.nbdb` | 已实现为 `/sdcard/nearby/db/devices.nbdb` |
| 复用 board 共享 SPI，不重复创建总线 | 走 `board_sd_mount()` |
| 识别失败仍保留 generic Device | host 覆盖 |
| **实板验证** | **未做**（未插卡、未烧录） |

**未完成**：真实 SD/SPI 并发与耗时、拔卡行为、任何 RAM/栈测量、生产语料规模。
`thread`/`zigbee` 阶段仍为空实现（归 B8/B9）。

下一步为 B6（配网与数据库导入后端）。


