# 交接记录（Nearby Devices 无 GUI 开发）

本文件是给下一个窗口的**唯一入口**。目标是让接手者不必重新调查就能继续：当前提交、
每个阶段的准确状态、具体未完成项、可复现的测试命令与结果、以及下一步。

规则：本文件只写**已核实**的内容。每一项区分四类证据，互不替代：

| 标记 | 含义 |
|---|---|
| 代码 | 源码已实现并提交 |
| host | `tests/run_all_host_tests.sh` 中的测试组通过（本机或 CI） |
| 构建 | ESP-IDF v6.1 / esp32c6 目标固件构建通过 |
| 实板 | 在 Waveshare ESP32-C6-Touch-LCD-1.9 上验证过 |

**实板列目前全部为空：从未烧录过任何一块板子。** 任何声称实板通过的说法都缺少证据。

---

## 1. 当前工作状态

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/yuanwil1y/One-OS` |
| 当前分支 | `feat/b6-http-portal`（B6 剩余工作） |
| 已合并 | PR [#20](https://github.com/yuanwil1y/One-OS/pull/20) **已 merged**（merge commit `74facd3`）：B5 与 B6 前半部分进入 main |
| 进行中 | PR [#21](https://github.com/yuanwil1y/One-OS/pull/21)（**draft**）：B6 临时配网 AP，CI green |
| main | `74facd3`，CI green（run [34635862479](https://github.com/yuanwil1y/One-OS/actions/runs/34635862479)） |
| 目标硬件 | Waveshare ESP32-C6-Touch-LCD-1.9，ESP-IDF v6.1，无 PSRAM，8 MB flash |

### 远端分支情况（实测，非文档推断）

`git ls-remote --heads origin` 的实际结果：只有 `main`、`feat/b6-http-portal` 与
`research/matter-chip-tool-l2-api`。
B5 的工作分支 `feat/b5-recognition` 在 PR #20 合并后**已删除**（合并提交
`74facd3` 保留了全部历史，工作树无本地独有提交）。

**这里有一个本轮犯过并已纠正的错误**：本文件的第一版曾写"远端存在
`research/esphome`、`integration/l2-runtime`、`cleanup/*`、`tmp/*` 等多个分支"，
依据是 `git branch -a` 的输出——而那个列表里包含的是**本地缓存的陈旧远端引用**，
不是远端实际状态。用 `git ls-remote` 复核后，远端只有 main、Matter 研究分支和
B5 自己的分支。

所以开发状态文档里"远端仅剩 `main` 与 `research/matter-chip-tool-l2-api`"的记载
一直是**正确**的。之前把它判为"过期文档"是错的，本文件已改正。

教训：判断远端状态用 `git ls-remote`（或 `git fetch --prune` 之后的 `git branch -r`），
不要直接用 `git branch -a`——后者在没有 prune 的情况下会保留已删除分支的引用。

### 本分支上的提交与 CI（`feat/b6-http-portal`，PR #21）

| 提交 | 内容 |
|---|---|
| `05f8de3` … `272c81d` | B6 临时配网 AP（见 §4b） |
| `76514ce` | B7：ESPHome 认证 Noise 传输（`esphome_noise*.c`）与 host 证据 |
| `7a886b8` | 修 CI：一个 gcc 报、clang 不报的未使用变量 |
| `73a5dc3` | B7：BLE GATT 会话（`app_ble_gatt.{c,h}`）与新 host 组 `app_ble_gatt` |
| `4166050` | 文档：§4d 与 B10 状态更正 |
| `c22235f` | 修目标构建：`impl_t` 缺 `noise_psk`；`hardclose` 在 `nwipe` 定义前调用它 |
| `19562b5` | 修 BLE GATT 组件：cancel 竞态不再返回假成功 |
| `385978d` `ccd050c` `6daa2b3` | 修加密会话用例：真正订阅 off 回调；明文会话拒绝命令的错误码 |
| `713e39b` | 未完成的 Windows socket shim 不接入 runner，只作为文档化工具保留 |

**目标构建（`build` job）在 `c22235f`、`19562b5`、`385978d`、`713e39b` 上均已通过。**
host-tests job 在 `6daa2b3` 之前的每一次失败都是一处不同的真实缺陷，逐条记在 §4d 的
"编译期教训"里。

## 2. 本地环境（本机没有任何 C 工具链，默认状态）

本机原始状态：**没有 `cc`/`gcc`/`clang`/CMake/Ninja，没有 `idf.py`，没有可用的 WSL，
也没有带编译器的 POSIX shell。** 因此：

- `tests/run_all_host_tests.sh` **无法直接运行**（需要带编译器的 bash）；
- 目标固件构建**只能在 CI 进行**。

本轮装好了两样东西（均为本机开发便利，不属于仓库依赖）：

1. `C:\Program Files\LLVM`（winget `LLVM.LLVM`），仅提供 `clang-format`/`clangd`；
2. `D:\OS\.toolchain\llvm-mingw-20260908-ucrt-x86_64`（llvm-mingw，clang 23 + MinGW-w64
   sysroot + compiler-rt），**这是实际用来编译 host 测试的编译器**。

### 运行 host 测试（Windows）

```powershell
cd D:\OS\One-OS
.\tools\local\run-host-tests.ps1                 # 全部组
.\tools\local\run-host-tests.ps1 -Group app_device
.\tools\local\run-host-tests.ps1 -List
```

该脚本读取与 CI 相同的 `tests/host-test-groups.txt`，对每组执行与对应 `.sh` runner
**相同的编译与运行命令**（相同 flag、源文件、include 路径）。它是便利工具，清单与
shell runner 仍是权威；某个组没有本地实现时会报错而不是跳过。详见
`tools/local/README.md`。

**两个组无法在本机运行**（需要 `arpa/inet.h`、`sys/socket.h`、`netdb.h`，llvm-mingw 的
sysroot 没有）：`esphome_l2`、`nmap_l2`。它们在**编译期**因系统头缺失失败，不会被误当成
通过，只在 CI 验证。

## 3. 阶段状态表

### 3a. 交付状态：每一阶段还剩哪些**验收项**没做

上面那张表说的是"代码写到哪了"。这一张说的是任务书要求的**验收**还差什么——两者不是一回事，
所以分开列。证据等级：**host** = 本机与 CI 的 host 组；**build** = CI 的 ESP32-C6 目标构建；
**实板** = 从未执行。

| 阶段 | 代码 | host | build | 实板 | **仍未完成的验收项** |
|---|---|---|---|---|---|
| B0 构建与诊断入口 | 完成 | 通过 | 通过 | 未做 | 实板清单 §1（启动日志、`version`/`status`/`ping`/`help` 的真实串口输出）、§6（Flash/RAM/栈实测） |
| B1 板级基础 | 完成 | 通过 | 通过 | 未做 | §1、§6；屏幕与 SD 共用 SPI2 的实测（§2.1） |
| B2 扫描链 | 完成 | 通过 | 通过 | 未做 | §2 全部：真实 Wi-Fi/BLE/LAN 阶段的设备数、取消、超时、容量边界、射频交接 |
| B3 Device/Entity | 完成 | 通过 | 通过 | 未做 | §2 的设备/实体一致性；多轮扫描下的新鲜度实测 |
| B4 DB 格式与工具 | 完成 | 通过 | 不适用 | 不适用 | 无（纯离线工具；与旧 NearBy One NEXT 库的兼容性**无证据**，且未声称） |
| B5 SD 读取/识别/配方 | 完成 | 通过 | 通过 | 未做 | §3 全部：真卡挂载、`/nearby/db/devices.nbdb` 路径、缺卡/坏库/版本不符、读取期间换库、重复 enrichment 不产生重复实体 |
| B6 配网与导入 | 完成 | 通过 | 通过 | 未做 | §4：NVS 重启后凭据恢复、APSTA 实测、浏览器交互、上传中断与断电恢复、替换失败保留旧库；HTTP transport 无自动测试 |
| B7 BLE GATT | 软件完成 | 通过 | 通过 | 未做 | §5b 全 9 项 + §5b-bis 7 项；**运行时不注册后端**（见射频归属决定） |
| B7 ESPHome | 软件完成 | 通过 | 通过 | 未做 | §5c 7 项 + §5c-bis 7 项；**识别身份已定、已实现、并已在识别入口真正调用**（mDNS instance 名，`app_device_db: 182 checks`），语料含 profile 1006；**仍未做**：运行时不注册后端、node 从未被连接过（没有网络任务做 probe/entities/subscribe/poll/command）、`test_api_client` 的加密路径仍失败（§4d） |
| B8 Zigbee | **未开始** | 部分（已有 11 个测试覆盖超时/重试/last-good） | 通过 | — | `esp_zigbee` 依赖不存在；coordinator 生命周期、入网、interview、ZCL 读写/命令、报告、网络持久化**全部未实现**；`APP_STAGE_ZIGBEE` 固定返回 `zigbee_backend_unavailable` |
| B9 OpenThread | 部分 | 通过 | 通过 | 未做 | Thread 生命周期、持久化、与射频交接未接应用 |
| B9 Matter | **构建失败** | 通过 | 失败 | — | §4e：`app/StatusIB.h` 在该 pin 上不存在；需要 pin 决定 |
| B10 控制闭环 | 模块完成、**未注册后端** | 通过 | 通过 | 未做 | 三条控制链（BLE/ESPHome）**运行时均不可达**，因为没有任何后端注册；§5 的 5.1–5.11 因此全部待办 |
| B11 无 GUI 验收 | 软件侧完成 | 通过 | 通过 | **全部待办** | 清单 §0–§7 每一项；无任何资源数字（heap 最低余量、最大可分配块、栈高水位、持续趋势） |

**一句话总结**：软件侧能做的都做了并且有 host 证据；**每一个剩余验收项都需要实板，或需要一个
产品决定**（ESPHome 识别身份、Matter CHIP pin、BLE 射频归属）。

| 阶段 | 代码 | host | 构建 | 实板 | 未完成项 |
|---|---|---|---|---|---|
| B0 构建/诊断入口 | 完成 | 通过 | 通过 | 未做 | 资源字段从未在实板取过值 |
| B1 生命周期/互斥 | 完成 | 通过 | 通过 | 未做 | 会话销毁超时的实际触发未验证 |
| B2 扫描链 | 完成 | 通过 | 通过 | 未做 | 射频与 LAN 服务探测未对真实环境验证 |
| B3 Device/Entity 状态 | 完成 | 通过 | 通过 | 未做 | — |
| B4 DB 格式/工具 | 完成 | 通过 | 通过 | 不适用 | — |
| **B5 SD 读取/识别/配方** | **完成** | **通过** | **通过** | **未做** | 已合并进 main；见 §4 |
| **B6 配网与导入后端** | **完成（软件）** | **通过** | **通过** | **未做** | NVS 重启行为与浏览器交互未实测；见 §4b |
| B7 BLE GATT / ESPHome | 部分（认证传输已通） | 通过（app_ble_gatt / app_ble_addr / app_ble_native / app_ctl_ble / app_ctl_ble_gatt / app_ctl_esphome） | 通过 | 未做 | ESPHome Noise 与认证控制已实现并 host 验证。**两侧控制链路（BLE 与 ESPHome）软件侧全部完成**：`app_ctl_ble` 143、`app_ctl_ble_gatt` 53、`app_ctl_esphome` 109 checks。**仍未做**：`app_runtime.c` 不注册任何 control 后端（→ 运行时答复仍是 `NO_BACKEND`），注册需要先定射频归属（见 §"B7 未决：BLE 射频归属"）；两侧都**从未对真实设备跑过**。见 §4d |
| B8 Zigbee 原生后端 | 未开始 | 部分 | 通过 | — | 无原生 coordinator。**核实结论**：§B8 列出的三个软件缺陷（interview 无回调超时、`retries=255` 回绕、失败 re-interview 清 snapshot）**已经修完并有 host 测试**（`zha_zigpy_l2` 组，11 个测试）；真正缺的是 `esp_zigbee` SDK 依赖与 coordinator/ZDO/ZCL 实机通路——本仓库**完全没有**该依赖（`grep esp_zb_` 无结果），且 802.15.4 验收需要真实设备 |
| B9 OpenThread / Matter | 部分 | 通过 | 通过 | 未做 | Matter 构建未修，**失败点已定位**：`matter_l2_direct_part1.inc:27` 引用 `app/StatusIB.h`，而该分支 pinned 的 connectedhomeip（`539342f`）里**没有这个文件**（见 §4e）；修它需要先定 pin 与头文件位置。Thread 生命周期未接应用 |
| **B10 统一控制闭环** | **模块 + 已接线** | **通过（含 app_cli_session / app_ctl_ble / app_ctl_ble_gatt）** | **通过** | **未做** | `APP_DIAG_CMD_CONTROL` 调用 `app_control_submit()`，控制循环可达。**控制后端已存在两个 host 组**（`app_ctl_ble` 85、`app_ctl_ble_gatt` 53），`BLE_GATT` 已翻为可驱动。**仍未做**：运行时没有注册任何后端（`app_control_register_backend` 无调用者），所以正确答复仍是 `NO_BACKEND`；注册需要先决定 BLE 会话的启动时机与射频归属。`app_runtime.c` 只能由目标构建编译，实板未验 |
| B11 无 GUI 整机验收 | 软件侧完成 | 通过（含 app_cli_session 100） | 通过 | 未做 | 实板清单全部待办，见 `docs/hardware-acceptance.md`；BLE 控制新增 5b.10–5b.16 七项 |

### B5 具体交付（本轮）

代码：

- `firmware/main/app_device_db_sd.{c,h}`：SD 存储适配器。路径
  `/sdcard/nearby/db/devices.nbdb`（由 `BOARD_SD_MOUNT_POINT` +
  `APP_DB_SD_RELATIVE_PATH` 组装），复用 `board_sd_mount()`，**全程持有一个文件句柄**，
  短读报 `ESP_ERR_INVALID_SIZE`，区分"无卡"与"卡上无库"。
- `firmware/main/app_device_db.c`：修复了一个**使识别永远无法工作的缺陷**——
  `db_read()` 曾以 `db->open` 为门，而 `open` 只在全部校验完成后才置位，因此第一次读头部
  就失败，每个库都会落到 `io_error`。现在读以 `opened`（存储已绑定、长度已知）为门。
  另加：每次匹配前重读头部做变更检测、显式全量重校验、域与 decoder/quirk 可用性校验。
- `firmware/main/app_device.{c,h}`：`app_recognition_enrich()` 填充按观测身份索引的识别表，
  `app_recognition_table_recognizer()` 让物化阶段消费该表；表满通过
  `app_recognition_table_truncated()` 报告并使扫描 partial。
- `firmware/main/app_runtime.c`：启动时打开库，每次 enrichment 前重开（不重启即可识别后插入
  的卡），`resources` 输出 `db_state`/`db`/`db_path`。
- `tests/host/test_app_device_db.c` + 新测试组 `app_device_db`（161 checks）。

## 4. B5 已知未完成 / 未验证

1. **实板全部未做**：未插卡、未烧录。SD 挂载、与 LCD 共享 SPI2 的并发、真实读取耗时、
   卡被拔出的行为——都没有观测过。
2. **无生产语料**：只验证过 2 672 字节的 5-profile 测试 fixture。真实设备库需要多少索引桶
   （当前预算 256 桶 = 8 KiB）未知。
3. **`enrichment` 阶段在无卡时记为 `skipped`**：这是刻意的（缺卡是正常部署状态，不是扫描
   失败），但意味着"未识别"与"识别不可用"在阶段表里都表现为 skipped。设备层的
   `recognition=db_unavailable` 才是准确区分，已在 `devices` 输出中体现。
4. **`thread` / `zigbee` 阶段仍是空实现**，记 `skipped` + 原因，归 B8/B9。
5. **Matter 构建未修**，仍在 `research/matter-chip-tool-l2-api`。

## 4b. B6 进度（进行中）

已完成（代码 + host 测试，实板全部未做）：

| 交付 | 说明 | 测试组 |
|---|---|---|
| `app_db_import.{c,h}` | 数据库替换状态机：分块流式写入、双重长度校验、容量与空间预检、`.part` 清理、**全量校验后才替换**、失败回滚；FAT 无原子 rename，因此序列设计为任意中断点都可恢复 | `app_db_import` 150 checks |
| `app_portal.{c,h}` | 请求/响应线层：表单解码（含 `%00` 拒绝）、Content-Length 解析、JSON 构建（SSID 十六进制、无密码字段、缓冲区不足则**什么都不写**）、临时 AP 密码生成 | `app_portal` 95 checks |
| `app_provision.{c,h}` | 会话编排：操作门互斥、射频交接顺序与回滚、会话超时、**替换期间关闭并重开 reader**、令牌恒定时间比较 | `app_provision` 120 checks |
| `wifi_mgr_ap_*`（`app_wifi.c`） | 临时配网 AP：只在 `release_for_scan` 与 `restore_after_scan` 之间运行，所以驱动不会在活着的 STA 上被重新初始化；短于 8 字符的密码被拒绝而不是静默降级成开放 AP；射频被隔离时拒绝启动 | 无 host 测试（ESP-IDF 专属）；**目标构建已通过**（PR #21，run 34636417815） |

### B6 未完成项

1. **NVS 凭据持久化的重启行为未实测**：`wifi_mgr_set_credentials`/`clear`/启动加载
   已实现，但"重启后自动重连"与"连接失败仍保留凭据"只有代码，没有实板证据。
2. **`/api/status` 的 AP 地址**取自 `wifi_mgr_ap_ipv4()`，只在 AP 运行时非空。
3. **HTTP 传输层没有任何自动化测试**：它是 ESP-IDF 专属代码，只有目标构建证明它能编译。
   浏览器交互、上传中断、`/api/portal/finish` 的"先应答后停止"顺序都未验证。
4. **实板全部未做**：无板、无浏览器、无真实 HTTP 服务器。

### 本地入口（已完成）

`request <id> portal <start|stop|status>`：

- `start` 启动会话，并在**串口**打印一次 SSID、生成的 AP 密码、会话令牌与 AP 地址；
- `stop` 结束会话；
- `status` 报告阶段、AP 是否真的在运行、凭据是否已呈现、上传阶段——**永不包含凭据本身**。

两个函数把这个纪律固定下来：`execute_portal()` 启动/停止会话但**从不组装凭据字符串**，
所以密码不会流经所有传输共用的响应结构；`app_runtime_portal_present()` 是唯一产出凭据的地方，
只由串口调用，而且第二次调用会拒绝并说明"已呈现过"，因此之后抓取的 scrollback 里没有密码。

### 编译期问题的教训（本轮共 3 次）

`app_runtime.c`、`app_http_portal.c`、`app_portal_native.c`、`app_device_db_sd.c` 都是
ESP-IDF 专属文件，**没有任何 host 组编译它们**。本轮连续出现三个只有目标构建能发现的错误：

1. `portal_sta_release` 声明为 `void`，而会话的 ops 表要求 `esp_err_t`——会话会检查这个返回值
   并在失败时回滚，所以"释放失败却报告成功"会让会话没有可回滚的东西；
2. `app_db_import_io_t` 用了却没有包含 `app_db_import.h`；
3. 把读取器的 vtable 赋给导入器的 vtable 类型——两者结构不同。

每一次本机 18 组 host 测试都是全绿。**结论：改动 host 不编译的文件后，目标构建是唯一的检查手段。**

## 4c. B10 与 B11 软件侧（本轮完成）

### B10 统一控制闭环

`app_control.{c,h}`（144 checks）。贯穿全模块的一条规则：**发送成功不等于状态改变**。
观察态只在 `confirm()` 或设备自身 `report()` 时移动；失败、超时、取消都会恢复到上一次
已确认的状态。准入按顺序检查：Entity 存在 → 所属 Device 在线 → 可写 → 动作被宣告 →
值存在且在配方范围内 → **恰好一个**后端认领（0 个是 NO_BACKEND，多于 1 个是
AMBIGUOUS_BACKEND，歧义绝不是猜测的许可）。

`app_entity_binding_t` 现在携带已解析的控制绑定（后端 id、写目标、配方范围），
控制循环因此不需要回头读数据库重新推导一次已经做过的决定。

**当前没有任何后端注册**，且 `app_backend_is_drivable()` 仍把所有控制后端报为不可驱动，
所以现有语料里没有任何可写实体——控制循环会正确地拒绝一切。

**但必须更正一句此前的表述。** 本节曾写"控制循环会正确地拒绝一切"，隐含的是"有请求被拒绝"。
实际不是：`app_control.h` 在整个 `firmware/` 下只被 `app_control.c` 自己 include，
`app_control_submit()`/`app_control_tick()`/`app_control_reconcile()` 没有任何调用者，
`firmware/main/app_runtime.c` 的 `APP_DIAG_CMD_CONTROL` 分支仍直接返回 `APP_DIAG_ERR_NOT_IMPLEMENTED`
（注释写的是"未实现的后端报 NOT_IMPLEMENTED"，但代码在到达任何后端之前就返回了）。
因此现状是**根本没有请求进入控制循环**，而不是请求被拒绝。B10 的模块与 host 测试是真的，
接线是缺的。

**本轮已修**（提交 `654863f`）：`APP_DIAG_CMD_CONTROL` 不再直接返回 `NOT_IMPLEMENTED`，
而是调用 `app_control_submit()`，参数取自 diag 请求的 `target`/`action`/`value`/`request_id`/
`timeout_ms`，时钟用运行时的 `now_ms()`。每个拒绝按原因映射到各自的 diag 错误
（`NO_BACKEND`/`BUSY`/`OUT_OF_RANGE` 等不再折叠成一个通用失败），成功时返回的是
"已受理"并带上控制循环给出的状态名，而不是声称状态已改变。

**为什么这件事重要**：在此之前，B10 的 144 项 host 检查与全部准入规则**没有任何生产调用者**，
也就是说根本没有被编译进镜像的代码去执行它们。现在这条链是真实可达的。

**仍未做完**：没有任何后端注册，所以运行时的正确答复是 `NO_BACKEND`——这是设计状态，
不是缺陷；要让 BLE 或 ESPHome 真正可写，还需要 B7 的控制后端（固件适配器本轮已完成）。
另外 `app_backend_is_drivable()` 对所有控制后端返回 false，实体的写目标因此会在识别阶段
被丢掉（`app_device_db.c` 只在 drivable 时保留），所以即使注册了后端，可写实体仍然为空。
`app_runtime.c` 是 ESP-IDF-only，只能由目标构建编译（已通过）与实板验证，不能 host 测试。

### B11 软件验收

`tests/host/test_app_acceptance.c`（581 checks）：100 轮完整扫描，每轮断言所有表未越界，
并在最后做**算术校验**——"物化总数 == 当前绑定数 + 已淘汰数"，任何泄漏或漏计都会在这里
以不匹配的形式暴露；32 轮的孤儿检查（应用表与 ha_core 双向）；超容量环境必须被报告为
truncated；取消扫描不清空设备列表；一次漏报不删除设备；扫描代次不干扰在线设备的控制。

实板部分见 `docs/hardware-acceptance.md`，**每一项都待办**。


## 4d. B7 进度（ESPHome 认证传输已通；BLE 会话已建；适配器未接）

### 已完成并有证据的部分

**Noise 认证传输（`esphome_l2`）。** 此前该组件只实现明文分片，遇到需要加密的 peer 就返回
`NOISE_NOT_SUPPORTED`，`esphome_api_command()` 永远无法发送。现在：

- `esphome_noise_crypto.c`：从规范实现 SHA-256、HMAC-SHA256、HKDF、ChaCha20-Poly1305、X25519。
  可移植 C11，无堆、无全局、无递归、无第三方密码库；同一份代码同时编进固件与 host 测试。
  GF(2^255-19) 用 10 个 32 位 limb、radix 2^25.5（ref10 布局），**不使用 `unsigned __int128`**，
  因此在 32 位 RISC-V 上没有 128 位算术依赖。
- `esphome_noise.c`：Noise 框架 rev 34（§5、§6、§7.5、§9、§12）的 NNpsk0 状态机。
  握手态 240 B，每个传输 cipherstate 48 B，全部在调用者的 session 内。
- `esphome_api.c`：加密分片。prologue 为 `"NoiseAPIInit"`（12 字节，无终止符，与
  `aioesphomeapi` 的 `b"NoiseAPIInit\0\0"` 一致——Noise 的 prologue 长度由调用者给出，
  12 与 14 只差两个 NUL，客户端发的就是 12 字节）；client hello 为
  `00 01 00 00`；之后每帧 `01 <len:2B BE> <AEAD(type:2B BE | data_len:2B BE | payload)>`。
  认证失败的帧丢弃、绝不分发，连续 4 次失败断开连接。
- `esphome_api_command()` 现在会在已建立的加密会话上发送；没有 PSK 时仍以
  `AUTH_REQUIRED` 明确拒绝，而不是明文发送。**发送成功仍不改状态**，状态只由 peer 的报告移动。
- `PROVENANCE.md` 已更新：删掉了"Noise 未实现、控制保持 fail-closed"的旧结论。

证据分三层，都不是自证：

1. `tests/esphome_l2/test_noise_crypto.c`（80 例）：SHA-256 用 FIPS 180-4 / RFC 6234 全向量，
   HMAC 用 RFC 4231 case 1/2/3/4/6/7，HKDF 用 RFC 5869 case 1，AEAD 用 RFC 8439 §2.8.2
   （另含 1232 个单比特翻转的拒绝用例），X25519 用 RFC 7748 §5.2/§6.1 **含 1000 次迭代**。
   另在 32 位 i686 构建下结果与 64 位逐字节一致。
2. `tests/esphome_l2/test_noise.c`：钉住固定 PSK / prologue / 临时密钥下的握手报文与传输帧字节。
   这些 fixture 由 `tools/reference/noise_nnpsk0_reference.py` 生成——那是**另一份用 Python
   `hashlib` + `cryptography` 独立实现**的同一握手；另有一个按规范直写的 responder 驱动完整交换。
3. `tests/esphome_l2/test_api_client.c`：在 loopback 上跑完整加密会话（握手 → probe → entities →
   subscribe → command → 状态上报 → disconnect），并覆盖错误 PSK 与伪造帧两种拒绝路径。
   该用例需要 POSIX socket，**由 CI 编译运行**；本机镜像把它报成响亮失败而不是跳过。

**BLE GATT 会话（`firmware/main/app_ble_gatt.{c,h}`，新 host 组 177 checks）。**
GATT 层本身是同步包装：一个共享完成槽、没有会话身份。三个后果被抬到会话层处理：

1. **取消不能看起来像成功。** 取消若终止链路，未完成的 read 会以 `ESP_OK` + 零长度返回。
   会话为每个操作打上签发时的 generation，取消/关闭/超时之后返回的操作一律报
   `CANCELLED`/`TIMEOUT` 并丢弃载荷，绝不报成功。
2. **迟到的回调不能落进下一个会话。** 订阅记录自己的 generation，不匹配的通知在进入应用
   回调之前就被丢弃并计数。
3. **操作超时是会话失败，不是重试。** 链路被拆掉、会话转 FAILED，重连是调用者的显式决定。

另含：`open()` 内部完成发现（数据库在会话内定界：8 服务 / 24 特征 / 12 描述符）、
按特征索引解析 value handle 与 CCCD（走该特征自己的描述符区间，不假设固定间隔）、
`turn_on`/`turn_off`/`set_value`/`set_text` 加 scale 的编解码，以及无法编码时报 `UNSUPPORTED`
而不是猜一种编码。

### B7 已完成：固件适配器（本轮，commit `0976d2b`）

`firmware/main/app_ble_gatt_native.{c,h}` 把 `app_ble_gatt_session_*` 绑到真实的
`esphome_ble_gatt_*`：install 填 ops 表，`gatt_init` 把平台 radio 对注入 transport config。
测试组 `app_ble_native` 用**真实的** `esphome_ble_gatt.c` + 假 radio 驱动它（只替换
`esphome_ble_gatt_nimble.c`），**96 checks，0 failures**，本机 clang 与 CI 的 gcc 走同一份
`tests/host/run_app_ble_native_tests.sh`。

写这个测试时查出并修掉了适配器自身的两个真缺陷，都属于"只在实机上才会暴露"的类型：

1. **平台 radio 钩子收到两个不同的上下文**。会话的 acquire/release 经适配器以
   `radio_ctx` 调用平台对，而 transport 在 connect 内部自己 suspend 时传的是
   `radio_user`。适配器把 `radio_user` 设成了**适配器自身**，于是同一个平台函数在两条
   路径上拿到不同指针，从错误的结构体里读自己的状态（suspend 计数读出
   1357958449 这样的垃圾值，第二次 suspend 失败）。现在 `radio_user = radio_ctx`。
2. **读失败后调用者的缓冲区里留着 transport 的垃圾**。适配器原先在调用**之前**清零，
   于是最后写缓冲区的是失败路径——调用者只要记下错误继续跑，就会把那几个字节当作读数
   发布出去。现在改为**非 OK 返回之后**清零：成功读不受影响，"读失败"和"值为空"成为同一
   个可观测状态。

另外两处支撑性修正：

3. `esphome_ble_gatt_session_t` 现在显式 `_Alignas(max_align_t)`。transport 内部把
   session 强转成 impl 结构，所以 session 必须按最严格成员对齐；不加这个说明符时对齐
   跟随 `size_t`，而 `app_ble_gatt_session_t` 是 1784 字节，嵌在它后面的 session 就会
   落在 8 字节边界上。UBSan 在 host 上抓到了这一点。
4. `esphome_ble_gatt_init()` 改为安装**有名字的** backend
   （`ESPHOME_BLE_GATT_BACKEND`，默认仍是 NimBLE）。这是 host 测试"真实 transport +
   脚本化 radio"的正式替换点；替换 `esphome_ble_gatt_init()` 本身意味着真实那份不再被
   编译，那样测试就没有意义。

*未验证*：该适配器从未对真实 peer 跑过，也没有目标构建。设备地址字节序（§ 下文第 1 条）
仍未定，适配器目前不转换。

### B7 已完成：设备地址字节序约定（本轮，commit 见 §8b）

`firmware/main/app_ble_addr.{c,h}`：**BLE 地址在本应用的唯一字节序**。

这不是外观问题，是一个真缺陷。BLE 地址是 6 字节，树里两种顺序都存在：

- **控制器序**：射频上报、也必须交回给射频的顺序，`addr[0]` 是最低有效字节。
  NimBLE 的 `ble_addr_t::val` 就是这个顺序（little-endian；
  `kismet_ble_session.c` 把 `d->addr.val` 原样搬进上报，所以这正是到达应用的顺序）。
- **显示序**（= 线上顺序）：所有面向人的地方打印的顺序，`addr[0]` 是最高有效字节，
  读作 `c4:99:4c:1a:2b:3d`。设备标签、nRF Connect、以及识别库里
  `BLE_PUBLIC_ADDRESS` 规则写的都是这个顺序。

在应用里持有控制器序的后果是可见的：`app_device_identity_of_ble()` 生成的 device id
会把地址**倒着打印**（标签为 `c4:99:4c:1a:2b:3d` 的设备得到 `ble_003d2b1a4c99c4`），
既让看串口的操作员困惑，又与数据库的 `BLE_PUBLIC_ADDRESS` 匹配键不一致——因为那是按
人读标签的方式写的。

**规则**：射频边界之上，应用一律持有**显示序**；控制器序的地址在**进入应用时转换一次**
（`app_scan_native.c` 收到上报处），此后任何地方都不得再转。适配器在**交给射频前**转回
控制器序（`app_ble_gatt_native.c::nat_gatt_connect()`）。

**为什么需要两个方向 + 交接测试**：翻转两次与不翻转**逐字节相同**。只测转换函数本身的
测试，在"两处都转"和"两处都不转"两种情况下会同样通过。所以：

- `app_ble_addr` 组（45 checks）：两个方向都钉在一个**真实地址的两种写法**上（不是我自己的
  输出），并测往返；
- `app_ble_native` 组新增 `test_peer_address_reaches_the_radio_once()`（该组 96 → 111 checks）：
  从会话给出的显示序地址，一路验证到**假射频实际收到的字节**必须是它的反转。
  我用**变异测试**验证过这个测试确实会失败：把适配器里的转换去掉后，
  三个断言立刻报错（`the radio was handed the wrong byte order`）；恢复后全绿。

写这个模块时又抓出**我自己代码里的两个错误**：原地翻转循环读到了自己刚写过的位置
（结果是旋转而不是反转，且只对回文地址正确），以及 NULL 时返回值与头文件声明不一致。
两者都由新测试当场抓到。

*未验证*：真机上射频是否接受这个顺序（硬件清单 5b.2 项）；`app_scan_native.c` 的改动
是 target-only，本机无法编译，由 CI 目标构建覆盖。

### B7 已完成：在途操作期间 deinit（本轮，commit 见 §8b）

这是任务书第三节第 1 条（"会话销毁超时 / 资源归属"）在 BLE 侧的具体形态，而且
**先复现、后修**：

我按 `app_ble_gatt` 既有测试的写法，在 `tests/esphome_l2/test_gatt.c` 加了一个
**在途 read 中调用 `deinit()`** 的 mock 分支（模拟 NimBLE 后端在 `deinit` 里删除完成
信号量，而调用者仍阻塞在那个信号量上），然后：

1. **先拿到崩溃**：ASan 报 `access-violation on unknown address 0x48`，栈是
   `end()` ← `esphome_ble_gatt_read()`。原因：`deinit` 的 `memset(s,0,...)` 把
   `i->backend` 清零之后，仍在途的调用者在 `end()` 里解引用它
   （`b->connected(i->backend_ctx)`）。
2. **修掉崩溃**后又暴露出**更糟的第二个问题**：这次 read **返回 0（成功）** 且
   `*len == 0`——正是本文件记录过的 "cancel 造成假成功" 的同一类缺陷，只是路径换成
   deinit。原因：`op_epoch` 虽然先自增，但紧随其后的 `memset` 把 `op_epoch_at_start`
   也清零了，于是 `end()` 里的 `i->op_epoch != i->op_epoch_at_start` 变成 `0 != 0`，
   判定为 "没有被放弃"。

**修法**（`esphome_ble_gatt.c`）：

- `deinit` 先把 backend 指针与 backend ctx **取到局部**（因为 `backend->deinit()` 会清掉
  这块存储），做 disconnect / 清订阅 / 还 radio，然后**在调用 `backend->deinit()` 之前**
  把 `i->backend` 置 NULL；
- `end()` 先取 `b = i->backend` 并判 NULL：为 NULL 说明会话已被拆除，此时不去问后端
  "链路还在吗"（那正是解引用 NULL），直接按被放弃上报；
- `memset` 之后写入 `op_epoch_at_start = UINT32_MAX`，一个**不可能被任何操作持有的代次**，
  让 `end()` 的比较必然失败。理论上要 42 亿次 deinit 才可能误判，而误判的方向是安全的
  （报失败，而它确实失败了）。

新测试断言：read 返回 `ESP_ERR_INVALID_STATE`（不是 0）、后端确实执行了这次 read、
后端确实被 deinit 过（其计数器在调用返回时已被清零，这本身就是拆除发生过的证据）、
以及被 wipe 的 session 上的后续操作被拒绝。

*未验证*：真机上的并发时序。本机用 mock 复现的是同一状态机，不是同一条线程模型。

### B7 已完成：BLE 控制后端（本轮，commit 见 §8b）

`firmware/main/app_ctl_ble.{c,h}`：把 `app_control` 接到 `app_ble_gatt` 会话上，
新测试组 `app_ctl_ble` **85 checks，0 failures**（本机 + CI，ASan+UBSan）。

它保护的是整个控制设计赖以成立的那条规则——**发送成功不是状态变化**。所以断言全是关于
**顺序**：send 之后、write 之后、以及只有设备上报之后。GATT 会话是脚本化的，因此这三个
时刻都是被刻意走到的，而不是靠和射频赛跑。

三个在真机上会静默出错的地方被钉住：

- **没有链路时是 FAILED，不是 SENT**。如果告诉调用者"已经发出去了"而其实没有，这条控制会
  一直 pending 到超时，然后被报成"设备超时"——把固件的问题说成设备的问题。
- **`app_control_report()` 会把 pending 的控制判失败**（按设计："设备自己的上报优先于尚未
  确认的请求"）。所以通知**只缓冲**，由 worker 在 tick 里比较后再 `confirm()`。若在上报路径
  里调 `report()`，就会用这条通知杀掉它本来要确认的那条控制。
- **解码词表与控制词表不同**：`app_ble_gatt_decode_state()` 是值编解码器，把单字节 0x01
  渲染成 `"1"`（对数值寄存器完全正确），而 `app_control` 对布尔发布 `on`/`off`。直接比较
  `"1"` 与 `"on"` 正是这个测试**第一次运行就抓到的问题**：设备明明上报已经执行，控制却一直
  pending。现在有一个显式的映射函数，未识别的值原样透传。

另外：槽位在终止时被 `pending_release()` **清零**（`memset`），所以终止状态对调用者不可见——
包括真实调用者。测试因此改为观察**外部可见的东西**：`app_control_observed_state()`、
`app_control_pending_count()`、以及后端计数器。

**仍未接线（这是 B7 剩下的最后一步）**：`app_backend_is_drivable()` 对
`DEVICE_DB_BACKEND_BLE_GATT` 仍返回 false。在识别阶段写 `write_target_id` 的代码路径由它把门
（`app_device_db.c:608`、`app_device.c:1454`），所以**即使后端已经注册、测试全绿，实体仍然不会
可写**，控制循环的正确答复仍是 `NO_BACKEND`。同一个改动里还必须更新
`test_control_is_not_wired`——它现在断言"没有任何实体可写"，那条断言的前提正是"还没有控制器"，
而控制器刚刚有了。这两处必须一起改，否则两个决定互相矛盾。

（以上两节描述的是提交 `5067c5b`/`8cd5a5d` 的状态；下一节记录的提交已经把它闭合。）

**接线还差的那一个函数**——**本轮已完成**，见下一节。当时写下的契约是：
`app_ctl_ble_t::gatt.resolve`。固件版要做的事，用**已经存在**的字段就够了：

1. `app_device_binding_t` 带 BLE 显示序地址，必须与**当前已打开 peer** 的地址相同，否则这条
   控制是要发给另一台设备的，写下去就是驱动了错误的外设；
2. `entity->write_target_id` 就是配方里的 GATT characteristic 索引
   （`device_db_recipe_t::read_source_id` 是读侧的同一个索引），而
   `app_ble_gatt_resolve()` 能把 characteristic 索引在**已发现的数据库**上换成 value handle——
   所以这是查表，不是第二次发现；
3. characteristic 必须可写，否则在这里拒绝，而不是到了射频才失败。

这三条都在 host 上验证了（见下），不是需要实板的猜测。

### B7 已完成：BLE 控制链路已接线（本轮，commit 见 §8b）

三个新件把 B7 的软件链路接通，并各自有 host 组：

| 件 | 作用 | 组 / checks |
|---|---|---|
| `app_ctl_ble.{c,h}` | 控制器：队列、在 worker 上写、只凭设备上报确认、到期失败 | `app_ctl_ble` / 85 |
| `app_ctl_ble_gatt.{c,h}` | 固件绑定：**哪台设备的哪个 characteristic** | `app_ctl_ble_gatt` / 53 |
| `app_backend_is_drivable()` | `BLE_GATT` 翻为 true，配方的 write target 得以存活 | `app_device` / 196 |

**`app_ctl_ble_gatt` 存在的唯一理由**，是控制链路里错得最贵的那一问：写哪个
characteristic。**拒绝是可见的**（控制失败并记录原因），**写错 handle 不是**——写入会成功、
设备会确认，而错的那个属性已经被改了。所以这个组的重点全在拒绝路径上，每一条都被刻意走到：

- 没有活动会话（含"phase 还是 READY 但链路已断"这种）；
- **不是当前连接的那台设备**；
- 设备 id 不是 BLE 的 / 十六进制格式坏的 / 太短；
- characteristic 索引取不到 value handle，或 handle 为 0；
- **characteristic 不可写**（Read+Notify 这种传感器形状必须被拒）。

写这个组时抓到一个我自己埋的坑，值得记下来：`app_ble_addr_equal()` 是**故意对字节序不敏感**的
（它的用途是"调用者忘了转换或转两次时仍认作同一台设备"，避免产生两条记录）。这个容忍度对去重是
对的，**用在 peer 身份校验上就是错的**——地址反过来的 peer 是一台不同的设备，把它当作同一台就会
拿另一台外设的数据库去解析 handle。测试里"反序地址必须被拒"这一条当场把它抓出来，现在用的是
精确比较并写明了为什么不能用那个 helper。

另外两处判断也被钉住：`write` 一律带 response（控制是设备必须在 ATT 层确认的命令；无响应写
在包入队时就返回，那样的"成功"更没有意义），以及 characteristic 是否可写**在适配器里判**，
这样调用者能知道原因，而不是等射频去拒绝。

**仍未验证**：射频。从未有 peer 真正收到过本固件的一次写入——硬件清单 5b 项。

### B7 修复：带值控制的确认（本轮，commit `4e7c481`）

限界版的后端只为 `turn_on`/`turn_off` 推导期望状态，于是**设备明明执行了的 `set_value` 永远无法
确认**：它跑到 deadline 然后被报成失败。这是最坏方向的假阴性——操作员被告知一台其实工作正常的
设备没有响应——而且在任何基于开关的测试里都看不见，而此前所有测试都是基于开关的。两个缺陷：

1. `set_value`/`set_text` 根本没有期望状态，所以任何上报都不可能匹配。现在期望值就是用户请求的
   值：设备上报了它被给定的值，就是执行了。
2. **按文本比较在配方声明了 scale 时是错的**。`scale` 是语料表达"以十分之一为单位"的寄存器的
   方式：codec 在写上线之前会先乘，所以听话的设备上报的是**乘过的**数。scale=10 的配方请求
   `"12"` 写的是 120、回报也是 120，这与 `"12"` 永不相等——控制超时，而设备其实完全照做了。

现在期望值套用与 codec **相同的** scale，并按 codec 的方式把数字规范化（`strtof` 后 `"%ld"`），
所以写 `"128"` 和写 `"128.0"` 结果一致。**用变异测试验证过**：把 scale 去掉后
`test_a_scaled_recipe_expects_the_scaled_value` 的三条断言立刻失败，恢复后通过。

顺带把"发出去了但永远无法确认"这一类**从构造上关掉**并记录下来而不是假装测过：这个后端能发出的
每个 action，要么是有名字的状态，要么带着设备必须回报的值；`app_ble_gatt_encode_action()` 会拒绝
其余一切，新测试 `test_an_action_the_codec_cannot_encode_is_refused` 钉住 `press` 被拒为
`unsupported` 且不猜任何字节。`ble_confirm_unavailable` 这个理由保留在 `app_ctl_ble_tick()` 里，
作为将来出现"两者都不是"的 action 时的守卫。

**一个值得记下的过程教训**：fixture 原来只有一个开关实体，本轮新加的两个测试因此**在循环的能力
检查处就被拒了**，根本没走到它们要测的代码——第一次运行报的是"93 checks, 0 failures"。现在 fixture
里多了一个声明 `set_value` 的 dimmer 和一个声明 `press` 的 button，测试才真正触达后端。

### B7 未决：BLE 射频归属（B10 注册后端前必须先定）

`app_runtime.c` 里没有任何 `app_control_register_backend()` 调用，所以运行时不会用到上面这条
控制链。**这不是遗漏，是一个还没做的决定**，而它必须由实板回答。核实到的事实：

| 事实 | 位置 |
|---|---|
| `ble_rf` 阶段的会话**自己拥有整个 NimBLE 端口生命周期**：`nimble_port_init()` 起、`nimble_port_run()` 跑、扫描结束后 `nimble_port_stop()` + `nimble_port_deinit()` 拆 | `kismet_ble_session.c:189`、`:106`、`:253`、`:257` |
| GATT 会话用的是**同一个 NimBLE 主机栈**（`ble_gap_connect`、`ble_gattc_*`） | `esphome_ble_gatt_nimble.c` |
| 所以一次扫描结束后，NimBLE 端口**已被 deinit**，此时 `esphome_ble_gatt_init()` 里的 `ble_gap_connect()` 没有已初始化的栈可用 | 由上两条推出 |
| 扫描阶段是**串行**的，射频交接已经有一套既有约定（隔离、`radio_available`、`destroy_checked`） | `app_scan_native.c:171`、`:359` |

**必须做的决定**：BLE 控制会话的 NimBLE 端口由谁持有、什么时候初始化？可选的三种，各自代价：

1. **按需再 init**：控制时若端口未起就 `nimble_port_init()`，用完再 deinit。代价是每次控制一次
   栈初始化（时间与堆抖动）；需要确认 IDF/NimBLE 在这个版本上支持反复 init/deinit，**这一条只有
   实板能证实**。
2. **常驻端口**：让控制会话持有端口，扫描阶段借用。代价是 BLE 栈常驻 RAM，直接和 B11 的
   heap 预算相关；而且 `kismet_ble_session` 目前的 `nimble_port_init/deinit` 必须改成借还模型。
3. **不接线**：保持 `NO_BACKEND`，直到有实板能测出上面两种哪个可行。

**在所有三种里，现在都不应该注册后端**：方案 3 之外，方案 1 和 2 都要改 `kismet_ble_session`
的所有权模型，而那需要实板验证；在没验证前注册，会让每条控制报 `backend_failed`——
**比现在诚实的 `NO_BACKEND` 更糟**。

这条要在有板子时**第一个**回答，因为它同时决定 B7 的收尾和 B10 的注册。

### B7 已完成：ESPHome 控制后端（本轮，commit `2ffc59f`）

`esphome_l2` 在线路层**本来就是完整的**：分帧、Noise NNpsk0 握手、protobuf 编解码、实体发现、
状态订阅、命令编码器。缺的是 `app_control` 要对话的那一层。`app_ctl_esphome.{c,h}` 就是它，
形状与 `app_ctl_ble` 相同，新组 `app_ctl_esphome` **109 checks，0 failures**。

同一条规则，而在 ESPHome 上**更容易搞错**：状态上报走的订阅，对一台本来就有值的实体来说
**总会**有上报，所以"收到上报就确认"会把一条被拒绝的命令标成已确认。三条在真机上静默的用例被钉住：

- **不一致的上报不确认**，到期才是它的终点；
- **`missing` 不是状态**。设备对某个实体没有值时 API 会置 `missing`；把它当值会"从缺席确认"，
  把上一个值当current则是"从陈旧数据确认"；
- 没有控制在途时的上报**被忽略**，不计入。

**一条命令需要两个标识符**，而它们来自不同地方是有原因的：实体 `key` 是语料能portably命名的，
所以走 `entity->write_target_id`；`device_id` 是**设备自己报告**的事实，所以从发现结果里读，
而不是把它复制进语料。测试断言两者都到达了命令。

拒绝路径：没有会话是 FAILED 而不是 SENT；设备不再暴露的 key 被拒而不是照发；客户端拒绝的命令
算失败控制；没有 ESPHome 等价命令的 action 是 UNSUPPORTED 而不是猜一个。

**又一个 fixture 教训**（与上一轮同源、换了形状）：第一版给每种实体都设了 `has_range`，而循环
会把声明的范围应用到**任何带值的 action**——于是 `select_option eco` 在到达后端之前就被判
`out_of_range`。**循环是对的**，范围属于数值实体；现在 `bind_entity()` 只对 number 域设它。

至此 **B7 的软件链路（BLE + ESPHome 两侧）全部完成**，剩的只有射频与运行时注册。

### B7 未决：ESPHome 控制链路的可达性（逐环核实）

控制后端已经完成，但"运行时能否走到它"需要逐环核实。核实结果：

| 环节 | 现状 | 结论 |
|---|---|---|
| 发现节点 | `lan_note_nmap()` 把 ESPHome 的知名端口 **6053** 放进探测端口表（`app_scan_native.c:1153`），开放即计为一个 service；`lan_note_mdns()` 记录 mDNS 服务类型 | **有**：设备会以 LAN 设备形式出现，带 IPv4 |
| 连接地址 | `app_scan_lan_t` 有 `ipv4`，**没有** `port`；mDNS 记录的 `service->port` 没有被带进证据 | **够用**：ESPHome Native API 的端口是协议常量(6053)，仓库里已经如此对待；host 取证据里的 IPv4 |
| 实体绑定 | 需要一条 `ESPHOME_API` 配方（`write_target_id` = ESPHome 实体 key） | **缺**：`tests/fixtures/device_db/devices_fixture.nbdb` 里**没有任何 ESPHOME_API 配方**，所以现在没有任何实体可以被控制 |
| 控制器 | `app_ctl_esphome`（本轮） | **有** |
| 登记后端 | `app_runtime.c` 不注册任何后端 | **缺**，且与 BLE 侧同一个射频归属问题（ESPHome 走 TCP，不需要 NimBLE，但仍需一个不阻塞 worker 的网络任务） |

**因此要让 ESPHome 侧真正可控制，最小缺口是三件**（都不需要改架构）：

1. 语料里加一条 `ESPHOME_API` 可写配方，key 指向真实节点上的实体；
2. 一个网络任务承担 `probe/entities/subscribe/poll/command`——这些都是**阻塞 I/O**，绝不能跑在
   应用 worker 上（`esphome_api_poll` 带 timeout，`esphome_api_entities` 会一直读到 Done）；
3. 在 `app_runtime.c` 注册后端（与 BLE 侧一起做，见上一节）。

**调用顺序有一个不易察觉的陷阱**，值得写下来：`esphome_api_entities()` 会自己读到
`ListEntitiesDone`，在这之间到达的其它帧都会被它丢弃。所以正确的顺序是
**先 `subscribe()` 再 `entities()`**：反过来的话，实体列表请求与 Done 之间到达的状态上报会被丢掉，
而状态上报正是控制确认所依赖的东西。今天这个窗口里通常没有状态流量（节点先发实体再发 Done），
所以它是潜伏的而不是当前的故障——正因为如此才要写进文档。

### B7 未决（**已在本轮解决**，保留证据链）：ESPHome 设备曾**根本无法被识别**

下面是发现问题时的核实记录。**它已经修好了**——见上一节"B7 已决"。保留这一段是因为它记录了
**为什么**当时没有草率动手，以及证据在哪。

上一节按"链路上缺什么"列了三件事。继续往下核实时发现一个更根本的问题：**ESPHome 设备不会匹配到
任何 profile**，因为识别路径里根本没有 ESPHome 这一格。逐条证据：

| 环节 | 事实 | 位置 |
|---|---|---|
| 语料 | **没有任何 ESPHOME 后端配方**：5 个 fixture profile 的后端分别是 `passive_value`×4、`zigbee_*`、`none` | `tools/device_db/profiles/fixture_profiles.json` |
| 格式能力 | `esphome` 是合法的 fingerprint protocol（值 7），`esphome_node_name` 是合法 identity kind（值 4） | `tools/device_db/nbdb.py:59`、`:85` |
| **匹配键** | **`build_key()` 没有 ESPHome 分支**——只有 `BLE`/`WIFI`/`MDNS`/`LAN` 四个 case，其余走 `default: return 0u`。返回 0 表示"这条观察里没有任何可匹配的东西"，于是 profile 永远不会被解析 | `firmware/main/app_device_db.c` 的 `build_key()`，switch 在 `case DEVICE_DB_PROTO_MDNS/LAN` 之后直接 `default` |
| 匹配入口 | `app_device_db_match()` 只被调用三次：BLE、WIFI、以及 **LAN 走 `DEVICE_DB_PROTO_MDNS`**。没有任何调用传入 `DEVICE_DB_PROTO_ESPHOME` | `app_device_db.c:1124`、`:1137`、`:1150` |
| 身份来源 | `esphome_node_name` 是节点自报的名字，只有 API 的 hello 响应里有；应用里**没有任何地方调用** `esphome_api_probe()`/`esphome_api_entities()` | `grep esphome_api_` 在 `firmware/main/*.c` 中无结果 |

**为什么这在计算上是必然的**：ESPHome 设备在扫描里**只能**以 LAN/mDNS 设备的形式出现，而 LAN 设备的
匹配键是 `lan->service`，也就是服务类型字符串（`_esphome._tcp`）。**同一个服务类型下所有 ESPHome
节点都相同**，所以它不可能区分出某一台节点的 profile——就算给 `build_key()` 加上一个
`DEVICE_DB_PROTO_ESPHOME` 分支也解决不了，因为那时手里根本没有节点名。

**所以当时把它记成一次产品取舍**。事后重新核对发现：**它不是取舍**——`device-db-format.md`
已经把 `ESPHOME_NODE_NAME` 定为安全的跨协议身份，而 mDNS instance 就是那个名字，所以按已有约定
实现即可（见上一节）。当时之所以停下，是因为还没有把"格式里已经定义了这个身份"与"扫描能不能
拿到它"这两件事对上。

### B7 已决：ESPHome 设备的识别身份 = mDNS instance 名（本轮完成）

上一节把这件事记成"需要产品决定"。重新核对后**它不是产品取舍，而是工程决定**——因为仓库自己的
约定已经回答了它：`device-db-format.md` 把 `ESPHOME_NODE_NAME` 列为**安全的**跨协议身份
（kind 4），而扫描能给出一台 LAN 设备的唯一节点名，就是 mDNS 的 **instance**
（`example-node-1._esphome._tcp.local` 里的 `example-node-1`）。服务类型
`_esphome._tcp` 对所有 ESPHome 节点都一样，**不可能**用于区分。

所以按已有约定实现了它，改动四处：

| 改动 | 位置 |
|---|---|
| `app_scan_lan_t` 增加 `instance` 字段（既有 struct 里只有 hostname 与 service type，没有这一项） | `include/app_scan.h` |
| mDNS 阶段填入 instance | `app_scan_native.c::lan_note_mdns()` |
| **`build_key()` 增加 `DEVICE_DB_PROTO_ESPHOME` 分支**，键 = instance 名（规范化后） | `app_device_db.c` |
| 合并 LAN 观测时保留 instance（与 hostname/service 同一规则，且不能覆盖已有值） | `app_scan.c::app_scan_ingest_lan()` |

配套：`app_backend_is_drivable()` 的 `ESPHOME_API` 翻为 **true**（控制器已有 109 项 host 检查），
语料里新增 **profile 1006**（instance 键 `examplenode1`、`esphome_node_name` 身份、一条
`ESPHOME_API` 可写配方 + 一条被动 sensor）。

**写这个 fixture 时抓到一个真错误**，值得记下：我最初把指纹键写成 `example-node-1`，
结果**匹配不上**。原因不是代码，是键的形态——指纹哈希是在**规范化之后**的字节上算的
（小写、去掉 `-` 和 `:`，见 `nbdb.py::normalize_key` 与 C 的 `canonicalize()`），
所以语料里必须写 `examplenode1`。我直接解码了生成出来的 `.nbdb` 才看清：存储的
`key_hash` 是 `0xc3ae0728`（对应 `examplenode1`），而我按字面 `example-node-1` 重算得到
`0xb2b7f0d2`。**这条规则没有写在格式文档里**，只在两个实现里，所以现在写进了 fixture 的
`_note`。附带发现：Zigbee 那条 fixture 键（`example|plug-zb-2`）也有同样的字面/规范化差异，
但应用不用 Zigbee 哨兵做匹配，所以它今天无害——不改，但如果 B8 接上 Zigbee 匹配就会变成真问题。

*未验证*：真机。instance 名是否**总是**等于 API 自报的节点名，需要一台真实节点核对；
这正是清单 §5c.11 的判据。

### B7 修复：加了键却**没调用它**（本轮，commit `86b3a30`）

上一轮给 `build_key()` 加了 `DEVICE_DB_PROTO_ESPHOME` 分支、也加了按 instance 命名的 profile，
但**没有在识别入口调用它**。识别入口（`app_device_db.c` 的 recognizer）依次尝试 BLE、Wi-Fi、LAN，
而 LAN 那一次用的是 `DEVICE_DB_PROTO_MDNS`——它按**服务类型**取键。于是 ESPHome 节点仍然只会被拿去
和 `_esphome._tcp` 比对（这个服务类型对所有节点都一样），而存在的 profile 是按 instance 命名的。
**结果是：新键分支的单元测试全绿，节点依然识别不了。**

修法：LAN 分支**先**按 ESPHome 协议尝试，再回落到通用 mDNS。顺序在两个方向上都是要点：
instance 比服务类型更具体，必须先赢；而先试 mDNS 会为**任何** ESPHome 节点解析出按服务类型的 profile，
那正是 instance 键存在的意义所在。SSDP/Nmap 的观测没有 instance，取不到键，回落到"没有可匹配项"——
这是正常结果而不是错误。

**新测试强于上一轮那个**：`test_an_esphome_node_becomes_controllable()` 走的是整条路径
（ingest → recognizer → table → materialise → binding），断言节点匹配到 profile 1006、
instance 名在 ingest 后仍在、并且**至少一个实体是 writable 且 backend 为 `ESPHOME_API` 且带真实
write target**。最后这条才让"可控"有意义：它只有在配方的 write target 熬过识别之后才可能成立，而那
又要求后端可驱动。**上一轮的测试直接调 `app_device_db_match`，所以它能在节点依然无法被识别的情况下
通过。**

**教训（值得记）**：为一个新分支写单元测试，不等于验证了**分发会走到那个分支**。给某个协议/后端
加一条代码路径时，必须同时验证"选择它的那个地方真的选了它"——这就是"18 组全绿但目标构建失败"的同一
类错误，只是换到了识别层。

### B7 未完成项（明确列出）

- ~~**固件适配器**~~：已完成，见 §"B7 已完成：固件适配器"。
- ~~**设备地址字节序**~~：已完成，见 §"B7 已完成：设备地址字节序约定"。
- **control 后端**：`claims`/`send` 实现并注册进 `app_control`；通知 → `app_control_report()`/
  `app_control_confirm()`。注意 `app_control_backend_ops_t::send` 不传 request_id，而确认只认
  request_id，需要扩展 vtable 或让适配器用 `app_control_pending_at()` 反查。
- **GATT 组件自身两个缺陷**（逐行复核当前代码后的结论，与之前的记录不同）：
  (a) ~~cancel 与在途操作竞态时返回假成功~~ **已修**（commit `19562b5`）：
  `op_epoch`/`op_epoch_at_start` 已存在，`end()` 会把被放弃的操作改判为
  `ESP_ERR_INVALID_STATE`。本文件此前的记录是陈旧的。
  (b) ~~操作在途时调用 `deinit()` 会破坏状态~~ **本轮已修**，见 §"B7 已完成：在途操作期间 deinit"。
- **实体可写性**：`app_backend_is_drivable()` 是编译期开关且对所有控制后端返回 false，
  `BLE_GATT` 的写目标因此在识别阶段就被丢掉（`app_device_db.c` 只在 drivable 时保留
  `write_target_id`），`entity_upsert_recipe()` 还会丢掉 `read_source_id`，
  容器里的 `codec_id`/`subscription_id` 生产读取路径根本没读。要支持 BLE 控制必须先补这条链。
- **应用通路**：`firmware/main/` 里没有任何地方 include `esphome_ble_gatt.h`，
  `esphome_ble_gatt_nimble.c` 也不被任何 host 测试编译（`test_gatt.c` 用一个全零 ops 顶替生产符号）。
- **实板互操作**：真实 ESPHome 节点 + API 加密密钥；一块可控 BLE 外设。见 §9 与
  `docs/hardware-acceptance.md`。

### 本轮的编译期教训（第 4、5、6 次）

前三次已在 §"编译期问题的教训"记录。本轮又三次：

4. `tests/esphome_l2/test_noise_crypto.c` 里一个不再被使用的 `ad` 数组被 CI 的 **gcc**
   以 `-Werror=unused-but-set-variable` 拒绝，而本机 clang 不报这个诊断。
5. 同一个提交里 `esphome_api.c` 有两处**只有目标构建能发现**的错误：`impl_t` 从未加上
   `noise_client_hello()` 读取的 `noise_psk` 成员；`hardclose()` 在 `nwipe()` 定义之前调用它。
6. `scb_off`（`test_api_client.c`）定义了但从未被订阅——gcc 报 unused，clang 不报；
   而当时的测试断言 `got_off==0` 因此是**空洞的**。修正为真正订阅该回调。

第 5、6 次的共同点：**本机唯一能编译这些文件的 host 二进制需要 POSIX socket**，
所以本机在 include 阶段就停下，错误只在 CI 暴露。这正是"18 组全绿但目标构建失败"的重演。
为缩小这个缺口，本轮加了 `tests/esphome_l2/stubs/win/`（进程内 socket 回环）与
`tools/local/build-win-shim-test.ps1`，它已经能驱动明文握手；但它尚未完整模拟 TCP 流控，
在加密路径上会死锁，因此**没有接入 runner**。该测试仍只由 CI 在 Linux 上编译运行。

### Noise 客户端在 CI 上被逐个找出的真实缺陷

这些都不是推理出来的，而是靠"打印双方实际交换的字节"逐个定位的，且每个都是真 bug：

1. `impl_t` 没有 `noise_psk` 成员，目标构建直接失败（第 5 条教训）。
2. **PSK 从未被保存**：`esphome_api_init()` 只记下"调用者给了 PSK"这个布尔，从未复制密钥。
   诊断输出 `NX enter max_frame=512 psk=0` 一行定案。现在密钥被复制进 session，
   握手读副本（调用者的缓冲区因此不必活过 init），`deinit` 时擦除。
3. **协议错误码是陈旧的**：加密会话失败时报告的是上一次明文连接留下的 `OVERSIZED`，
   把一轮排查引向了错误的 bug。`noise_client_hello` 现在先清零 `protocol_error`。
4. **客户端从不发送 NOISE_HELLO**：线上头四个字节是 `01 00 30 3e`（握手头 + 消息首字节），
   而 `aioesphomeapi` 会先发 `00 01 00 00`。这四个字节不只是分帧——**仍然接受明文的 peer
   靠它判断这条连接是加密的**。现已由 `esphome_noise_client_hello_marker()` 发出。
5. **测试用的加密 peer 把 3 字节的握手头当 4 字节读**，吃掉了消息的第一个字节，
   之后每一步都在错位的流上断言。已修正为 3 字节。
6. 握手长度检查改为按协议实际携带的字节（`hs[2]`）进行，读与检查因此不可能不一致。

### B7 仍未完成的关键一项

**加密端到端用例（`tests/esphome_l2/test_api_client.c`）仍然失败**，但本机调试回路已经建成，
失败被定位到**第 2 条握手消息的最后一个字节**，且**不是密钥、nonce 或 AD 的问题**。

本机调试回路（本轮提交，见 `tests/local-win/README.md`）：shim 可用，
`D:\OS\.local-build\stubinc\` + `D:\OS\.local-build\local_enc.c` 把两端放进同一进程，
**一次运行几秒出结果**。

**已逐字节排除的（两侧完全一致）**：

| 检查点 | 结果 |
|---|---|
| prologue 后 `h`/`ck` | 一致 |
| PSK 混入后 `ck`/`k`/`h` | 一致 |
| `e.public_key` 混入后 `ck`/`k`/`h` | 一致 |
| 第 1 条消息 AEAD 的 nonce/key/ad | 一致，peer 校验**通过** |
| `tag` 混入后 `h` | 一致 |
| 第 2 条消息的 nonce、32 字节 `k`、32 字节 `h` | 逐字节一致 |
| 第 2 条消息 tag 的前 14 字节 | peer 产生 `2b 34 32 09 52 45 f6 08 6e 60 9b 04 7a 09`，initiator 收到同一串 |

**分歧点**：第 2 条消息共 51 字节（3 字节帧头 + 32 字节 `e` + 16 字节 tag）。
peer 发出、并打印的 tag 末两字节是 `87 b4`；initiator **收到**的末两字节是 `7a 3e`。
即**收发之间最后一个字节不同**（`0xb4` 对 `0x3e`），因此 AEAD 拒绝（`read_message=4`），
整条用例失败。

**下一步唯一要回答的问题**：这一个字节在哪里被改写。两条候选，按顺序：

1. **回环 harness 的问题**。本轮已确认本机 shim **不支持顺序连接**：
   `test_api_client.c` 会开五次连接、每次关掉上一个监听，而 Windows 会把刚释放的描述符号
   重新分配给新监听，此时上一个线程仍以为自己拥有那个号——trace 里明文阶段的
   `fd=0`/`fd=2` 在加密阶段被原样再发一次。这解释了完整测试在本机必然卡住，
   但**还不能解释 reduced harness 里那一个字节的差异**（它只有一条连接）。
2. **`msg2` 的写入范围**：确认 `tests/esphome_l2/noise_test_responder.c` 的
   `ntr_handshake` 里没有任何路径在发送前写到 `msg2[32..47]` 之外，
   以及 `esphome_noise.c` 的 `encrypt_empty_payload` 是否写满 16 字节。

### 本轮新增的关键证据（不需要网络，可重复）

新增 `tools/reference/noise_nnpsk0_interop.py`：对任意给定的一对临时密钥，用**独立 Python 实现**
跑完整 NNpsk0 并打印每一个中间值（`ck`/`k`/`h`/nonce/两条消息/传输密钥）。
用它对本机 harness 的那对密钥跑，结果：

- `msg1` = `07a37cbc…557c73`，与仓库里已有的钉死向量 `MSG1` **逐字节相同** → 脚本本身可信；
- 参考实现给出的 `msg2` 前 **46 字节**与 test peer 实际发出的 `msg2` **完全一致**，
  **只有最后两字节不同**（参考 `…85 b6`，实际 `…87 b4`）；
- 而该 tag 的最后两字节在两次运行之间**取值稳定**（不是随机的栈残留）。

**这条证据把范围缩得很小**：协议层、密钥调度、tag 的前 14 字节在"本仓实现"与"独立实现"之间
完全一致，分歧只在那两字节的去向。因此**问题不在 Noise 协议实现**，
而在 test peer 构造/发送这 48 字节的那段代码，或回环的传输行为。

**可重复的命令**（密钥为 harness 使用的那一对）：

```
python tools/reference/noise_nnpsk0_interop.py \
  0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20 \
  404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f
```

在真实 TCP 上（CI 或一台 Linux 机器）重跑同一条打印，可以直接区分"回环的问题"与
"协议/测试的问题"——这是最省事的一步。

**这条路我试了两次都没跑成**：两次都因为把 `hs[48..50]` 写进了 48 字节数组，
gcc 的 `-Werror=array-bounds` 先让构建失败，比较根本没发生（同一轮里我犯这个错两次）。
诊断代码已撤回。**下一次要比较这两个字节，必须先在
`tests/local-win` 的回环上编译并运行过再提交**，不要再让 CI 承担"这段打印能不能编译"。

**我在记录与提交信息里两次把结论说过头**（先说"最后一个字节"，再说"只有 tag 的第 15 字节不同"），
两次都被下一次运行推翻。上面这张表是当前**有打印支撑**的事实，不要再往上加推论。

### 传输帧序列现已由独立实现钉死（本轮新增，本机已通过）

在此之前**只有一条传输帧**（HelloRequest）被钉死；帧计数器、nonce 构造和 AEAD 只在
"一条消息"的尺度上被外部实现检查过。第二条帧一旦错位，本地测试是看不出来的。

新增 `tools/reference/noise_transport_reference.py`：用参考握手产生的传输密钥，按顺序加密
客户端在握手后会发出的 5 个包（HelloRequest、DeviceInfoRequest、ListEntitiesRequest、
SubscribeStatesRequest、SwitchCommand）与响应端发回的 2 个帧，打印全部 7 条。测试
`tests/esphome_l2/test_noise.c::test_transport_frame_sequence()` 逐条比对，并断言每次调用
**恰好**推进一次 nonce。

写这个测试时暴露了一处**我自己的错误假设**，值得记下，因为它是协议层的硬事实：

- ESPHome 加密帧的明文是 **`[msg_type:2 BE][payload_len:2 BE][payload]`**，
  **associated data 为空**；
- 3 字节的线上前缀（`0x01` + 大端密文长度）由会话层拼上，
  `esphome_noise_encrypt()` 只产生密文；
- 因此空 payload 的单帧长度是 **3 + 4 + 16 = 23 字节**。

我第一版测试误以为 `msg_type` 是 associated data、明文只有 2 字节，于是报出
"packet 0 is 16 bytes, expected 20"。修正后 5 条发送帧与 2 条接收帧与 Python 参考实现
**逐字节一致**；`esphome_api.c` 的 `noise_build()`/`noise_unwrap()` 也正是这个布局
（`pt[0..1]=type`、`pt[2..3]=len`、AD 为 NULL），两者互为佐证。

可重复命令：

```
python tools/reference/noise_transport_reference.py
.\tools\local\run-host-tests.ps1 -Group esphome_l2
```

## 4e. B9：Matter 研究分支的构建失败已定位到具体事实（本轮核实）

`research/matter-chip-tool-l2-api` 比 main 领先 50 个提交、落后 17 个。它的 CI 最近三次
（最近 `d562569`，2026-09-09）都是 **`host-tests` success、`build` failure**。失败点已查到
**具体一行**，并且其中一部分核实到了外部一手来源：

1. **编译错误**：`.../components/matter_l2/matter_l2_direct_part1.inc:27:10: fatal error:
   app/StatusIB.h: No such file or directory`，随后 `FAILED: .../matter_l2.cpp.obj`、构建停止。
   即 ESP-IDF 侧 1801 个目标全部构建成功，**只有 matter_l2 自己**编不过。
2. **一手核实**：该分支把 connectedhomeip 子模块钉在
   `third_party/connectedhomeip` = `539342f32d5f4dc93761c2f9325afe29270068f1`
   （`espressif/connectedhomeip.git`，见该分支 `.gitmodules`）。取
   `src/app/StatusIB.h` 得到 GitHub raw **HTTP 404**——**该文件在这个 revision 上不存在**，
   所以这不是 include 路径配置问题。
3. **CI 如何取 CHIP**：`checkout_submodules.py --platform esp32 --shallow`，然后在
   `esp-idf-ci-action` 里 `source scripts/activate.sh -p esp32` 再 `idf.py build`；另有一个
   `tools/check_matter_l2_scope.py` 约束 matter_l2 不得调用其它 L2 家族或 `esp_matter`。

**结论**：失败**不是**"构建工具链不可用"，而是 matter_l2 引用了一个在该 pinned CHIP 版本中
不存在的头文件。修它需要先确定用哪个 pin 的 connectedhomeip、以及该版本里 `StatusIB` 的真实位置
（本机没有 CHIP 检出、没有 idf.py，**无法**在本轮查证）。因此**没有**盲改研究分支。

## 5. 本轮修掉的四个边界问题

任务书第三节的四项，逐项证据与残留风险：

1. **会话销毁超时**：新增 `kismet_{wifi,ble}_session_task_alive()`（`finished` **且**
   `claimed` 才算退出；`claimed` 是 destroy_checked 取到任务最后一次 give 的信号量）。
   应用侧隔离并回收会话，隔离期间拒绝启动任何射频阶段；Wi-Fi 新增
   `WIFI_MGR_QUARANTINED` 状态，任务退出后自动重连。BLE 的 tracker 不再被无条件释放。
   *未验证*：这条路径从未真实触发过（需要原生调用卡住 15 秒）。
2. **设备新鲜度**：缺失设备在**连续两轮**完整覆盖未见后才移除；首轮标记 `STALE` 并保留身份
   与 Entity；`UNAVAILABLE` 专表示"没看成"（阶段 skipped/failed/cancelled/partial）；
   任一来源再次看到即重置计数；可用性同步到 ha_core Entity（`available=false`，状态保留）。
   *未验证*：真实射频漏报下的行为。
3. **Wi-Fi 事件代次**：审计确认 epoch 在事件产生时（锁内）绑定，`driver_down()` 先 bump 再
   拆、`driver_up()` 后 bump；IP 路径还要求 netif 真有地址。修掉
   `wifi_mgr_quarantine()` 未清 `released_for_scan` 的问题。
4. **库文件变化**：每次匹配前重读头部比对（替换/改写/等长修改都会改变头部字节，因为头部
   含自身内容的校验和）；`app_device_db_verify_unchanged()` 是显式的全量重校验（流式 body
   CRC + 重新询问介质长度），能抓原地改写与增长。两级代价不同，已分别写入文档。

### 本轮另外修掉的一个（B6 侧，host 已验证）

5. **已停止的 AP 仍在 `/api/status` 里报出旧地址**。`app_wifi.c::wifi_mgr_ap_ipv4()` 本身是
   对的：AP 一旦停下就返回 `ESP_ERR_INVALID_STATE` 并清空输出。但 `/api/status` 的文档由
   `app_portal_build_status_json()` 生成，而它**照抄调用者 struct 里的内容**——浏览器和
   按验收清单操作的人读到的正是这份文档，`"active":false` 旁边挂着一个地址会让人去找一个
   根本没在运行的门户。现在构建器只在 `active` 为真时才输出 `ap_ssid`/`ap_ipv4`，其余情况
   一律空串：不变量由**构造保证**，不再依赖每个调用者记得清空（这也正是响应形状放在这个
   文件里的原因）。

新增测试 2 个（`app_portal` 组 95 → **104 checks**）：

- `test_status_ap_address_follows_the_ap()`：AP 停止时地址与 SSID 在文档里为空；
- `test_status_carries_no_generated_secret()`：用**真实生成的** AP 密码和真实解析出的
  station 密码去查文档，断言两者都不出现。既有测试只查字段名 `password`，查不到值——
  万一以后有人加字段时传错缓冲区，只有查值才能发现。

## 6. 测试命令与结果

### 本机（2026-09-12，本轮新增组）

```powershell
cd D:\OS\One-OS
.\tools\local\run-host-tests.ps1                      # 全部组
.\tools\local\run-host-tests.ps1 -Group app_ble_native  # 本轮新增：BLE 固件适配器
.\tools\local\run-host-tests.ps1 -Group app_cli_session # 本轮新增：无 GUI 验收会话
```

本轮新增两组：

| 组 | checks | 内容 |
|---|---|---|
| `app_ble_native` | 96 | 编译**真实的** `esphome_ble_gatt.c`，只把 NimBLE 后端换成脚本化 radio，验证固件侧的 ops 表 |
| `app_cli_session` | 100 | 把 `docs/hardware-acceptance.md` 里操作员真正会敲的命令行走一遍真实的 parse → decide → render 路径 |

`app_cli_session` 用的是真实决策模块（`app_control` 判控制拒绝、`app_scan` 判阶段结论），
不是第二份规则副本；它抓的是**漂移**：命令还能解析但渲染出没人能据以行动的响应、拒绝理由变了、
或者某个阶段的结论不再被报告。它证明不了平台那一半——`app_runtime.c`、`app_scan_native.c`
和射频路径是 ESP-IDF-only，由目标构建覆盖。

### 本机（2026-09-11，上一轮）

```powershell
cd D:\OS\One-OS
.\tools\local\run-host-tests.ps1
```

结果：**19 组通过，2 组因本机缺 POSIX socket 头失败（esphome_l2、nmap_l2），后者只在 CI 验证。**

**本机通过不等于 CI 通过**：本机无法编译 ESP-IDF 专属文件（如 `app_scan_native.c`），
目标构建只在 CI 进行。本轮就出现过本机 15 组全绿、而 CI 目标构建失败的两次
（`stopped` 未初始化、`stopped` 重复定义），原因都是 host 测试覆盖不到那个文件。
**改动任何 host 不编译的文件后，必须等 CI 目标构建通过再认为完成。**

各应用组 checks 数：

| 组 | checks | failures |
|---|---|---|
| `app_diag_protocol` | 109 | 0 |
| `app_ops` | 109 | 0 |
| `app_scan` | 136 | 0 |
| `app_device` | 181 | 0 |
| `app_device_db` | 161 | 0 |
| `app_db_import` | 150 | 0 |
| `app_portal` | 95 | 0 |
| `app_provision` | 120 | 0 |
| `app_ble_gatt` | 177 | 0 |
| `app_ble_native`（本轮新增） | 96 | 0 |
| `app_cli_session`（本轮新增） | 100 | 0 |
| `app_portal`（本轮 +9） | 104 | 0 |
| `app_acceptance` | 581 | 0 |
| `device_db_python` | 36 | 0 |
| `device_db_format` | 200 | 0 |

（23 组在本机通过；`esphome_l2` 与 `nmap_l2` 见上文。）

### CI

- **`55afe25`（本分支 HEAD，含本轮全部改动）：`build` success，`host-tests` 22 组通过、1 组失败**
  —— [run 34688559014](https://github.com/yuanwil1y/One-OS/actions/runs/34688559014)。
  唯一失败组是本文件多处记录的 `esphome_l2` / `test_api_client` 加密路径（见 §4d）。
  本轮新增/加强的三组在 CI 上全部 PASS：`app_ble_native`（96 checks）、
  `app_cli_session`（100 checks）、`app_portal`（104 checks）。CI 用的是 **gcc**，
  比本机 clang 严格，所以这一栏是独立证据而不是本机结果的重复。
- 前一次运行 **`a838205`：`build` success、`host-tests` 21 组通过**
  —— [run 34688060996](https://github.com/yuanwil1y/One-OS/actions/runs/34688060996)；
  该次的 `test_noise` 报 `noise tests: ok`（含新增的多帧传输向量）。
- **main `74facd3`（PR #20 合并提交）：`build` 与 `host-tests` 均 success**
  —— [run 34635862479](https://github.com/yuanwil1y/One-OS/actions/runs/34635862479)。
  这是 B5 完成的构建证据。
- B6 分支 `feat/b6-http-portal` **不触发 CI**（workflow 只在 push 到 main 与 PR 事件时运行），
  所以该分支上的改动目前**只有本机 host 测试证据**。本轮在其中加入的 `wifi_mgr_ap_*`
  是本机完全无法编译的 ESP-IDF 代码，必须等它进入 PR 或 main 才能拿到目标构建证据。
- 历史：`34634665402`（B5 分支最后一轮）success；`34634041141`、`34632937479` 失败，
  原因分别是 `stopped` 重复定义与 `stopped` 未初始化，均已在 `0acda78` 修复。

### 目标构建（本机无法执行）

```bash
cd firmware && idf.py set-target esp32c6 && idf.py build
```

## 7. 串口诊断入口（无 GUI）

烧录后在 115200 波特率串口输入：

```
request 1 help
request 2 version
request 3 resources      # 含 db_state / db / db_path
request 4 scan full
request 5 devices
request 6 entities
request 7 cancel 4
```

DATABASE 部署：把 `devices.nbdb` 放到卡的 `/nearby/db/` 目录（即
`/sdcard/nearby/db/devices.nbdb`）。没有库时扫描照常，设备以 generic 只读形式出现，
`resources` 中的 `db_state` 会给出具体原因。

## 8. 下一步

1. **B6**：NVS 持久化（已有基础）、临时 APSTA、HTTP `/api/status|wifi/scan|wifi/connect|db/upload`、
   流式上传到 `.part` + 校验 + 替换 + 断电恢复（FAT 上不能只靠 rename）。上传路径与 B5 的
   reader 必须协调：替换前关闭 reader。
2. **B7**：ESPHome Noise 认证（当前只有明文切片，`ESP_ERR_NOT_SUPPORTED` 不能简单删掉）；
   BLE GATT 的 control 后端与**设备地址字节序约定**（适配器本轮已完成并 host 验证）。
3. **B8**：Zigbee 原生 coordinator/ZDO/ZCL，复用已有 interview 超时与 last-known-good 修复。
   核实：那三个软件修复**已经完成并测试**（见阶段表），真正缺的是 `esp_zigbee` SDK 依赖
   （仓库中完全没有）与实机通路。
4. **B9**：先修 Matter 构建（独立分支），再做 Thread 生命周期与 Matter 配网/订阅。
5. **B10/B11**：统一控制闭环与整机验收；B11 需要实板才能完成资源测量部分。

## 8b. 本轮（2026-09-12）实际完成的内容

按提交顺序，全部已推送到 `feat/b6-http-portal`：

| 提交 | 内容 | 证据等级 |
|---|---|---|
| `c90f898` | 传输帧序列由独立 Python 实现钉死（7 条帧），暴露并纠正了我对帧布局的错误假设 | host 通过 |
| `c4cbc90` | 上述结论写入交接记录 | 文档 |
| `0976d2b` | **B7 固件适配器** `app_ble_gatt_native.{c,h}` + 新组 `app_ble_native`；修掉适配器两个真缺陷（radio_user 指向错误上下文、读失败后残留 transport 垃圾）与 transport session 对齐 | host 通过（本机 + CI） |
| `a838205` / `0e198e6` | 交接记录与硬件清单更新 | 文档 |
| `df1e6c0` | **B11 无 GUI 验收会话**测试组 `app_cli_session`：把硬件清单里的命令行按真实 parse → decide → render 走一遍 | host 通过（本机 + CI） |
| `55afe25` | **B6 修复**：已停止的 AP 不再在 `/api/status` 报出旧地址；新增按**值**检查密钥不泄漏的测试 | host 通过（本机 + CI） |

## 9. 需要人工提供的事项

按"它能解锁什么"排序，而不是按阶段号：

| 阻塞 | 需要的最小操作 | 解锁 |
|---|---|---|
| **一块 Waveshare ESP32-C6-Touch-LCD-1.9 + 串口端口号** | 连接并告知端口（`COM…`） | B11 全部实板项；**所有** RAM/heap/栈数字；BLE 射频归属决定（B7/B10 注册）；BLE 地址字节序的最终确认 |
| **microSD 卡（FAT32，含 `/nearby/db/`）** | 插入一张卡 | 清单 0.2、3.x；真实库读取与上传 |
| **一块可控 GATT 外设** | 第二块 ESP32 跑 `bleprph`，或 Linux 主机跑 BlueZ `btgatt-server` | B7 BLE 控制端到端（清单 5b.10–5b.16）——**这是最便宜的单点解锁**，也是唯一能证实"写错 handle 会静默成功"这件事的装置 |
| **一台可访问的 ESPHome 节点 + 其 API 加密密钥** | 节点地址 + key | B7 ESPHome 端到端（清单 5c.x）；同时是那条 404/握手修复的最终判据 |
| **一个决定：ESPHome 设备的识别身份用哪一种** | ~~已完成~~：按仓库已有约定选了 mDNS instance 名并实现（§"B7 已决"），**不再需要你决定** | — |
| **一条 `ESPHOME_API` 可写配方** | ~~已在 fixture 里加好~~（profile 1006，键为 mDNS instance）。真机使用时需要把它换成**真实节点**的 instance 名与实体 key——ESPHome 的 key 是节点自行分配的不透明 u32，无法从 object_id 推导 | 让 ESPHome 侧在新板上立刻可控 |
| **一个标准 Zigbee 设备** | 任一 Zigbee 灯具/开关 | B8 的验收；同时是 `esp_zigbee` 依赖是否值得引入的判据 |
| **一个 Thread Border Router 或 dataset** | 可用的 Thread 网络 | B9 Thread 侧 |
| **Matter 的 pin 决定** | 在 CHIP 侧确认：`539342f` 里 `StatusIB` 的真实位置，或换一个包含它的 pin | B9 Matter 构建（见 §4e；本机无法查证，需要一次可访问 CHIP 检出的排查） |
| **生产 `devices.nbdb`** | 真实设备库文件（或授权来源清单） | 索引桶预算的真实性；识别覆盖率 |

