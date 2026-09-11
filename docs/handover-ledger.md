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

`git ls-remote --heads origin` 的实际结果：只有 `main` 与 `research/matter-chip-tool-l2-api`。
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

| 阶段 | 代码 | host | 构建 | 实板 | 未完成项 |
|---|---|---|---|---|---|
| B0 构建/诊断入口 | 完成 | 通过 | 通过 | 未做 | 资源字段从未在实板取过值 |
| B1 生命周期/互斥 | 完成 | 通过 | 通过 | 未做 | 会话销毁超时的实际触发未验证 |
| B2 扫描链 | 完成 | 通过 | 通过 | 未做 | 射频与 LAN 服务探测未对真实环境验证 |
| B3 Device/Entity 状态 | 完成 | 通过 | 通过 | 未做 | — |
| B4 DB 格式/工具 | 完成 | 通过 | 通过 | 不适用 | — |
| **B5 SD 读取/识别/配方** | **完成** | **通过** | **通过** | **未做** | 已合并进 main；见 §4 |
| **B6 配网与导入后端** | **完成（软件）** | **通过** | **通过** | **未做** | NVS 重启行为与浏览器交互未实测；见 §4b |
| B7 BLE GATT / ESPHome | 部分（认证传输已通） | 通过（新增 app_ble_gatt 组） | 通过 | 未做 | ESPHome Noise 与认证控制已实现并 host 验证；实节点未验证。BLE GATT 会话生命周期已建；**固件适配器、control 后端、GATT 组件自身的 cancel 竞态仍未做**；见 §4d |
| B8 Zigbee 原生后端 | 未开始 | 部分 | 通过 | — | 无原生 coordinator；无应用通路 |
| B9 OpenThread / Matter | 部分 | 通过 | 通过 | 未做 | Matter 构建未修；Thread 生命周期未接应用 |
| **B10 统一控制闭环** | **仅模块（未接线）** | **通过** | **通过** | **未做** | **更正**：`app_control.h` 在固件里没有任何调用者，`APP_DIAG_CMD_CONTROL` 仍直接返回 `NOT_IMPLEMENTED`，所以不是"一切都正确地被拒绝"，而是**根本没有提交**；见 §4c |
| **B11 无 GUI 整机验收** | **软件侧完成** | **通过** | **通过** | **未做** | 实板清单全部待办，见 `docs/hardware-acceptance.md` |

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
接线是缺的；接线列为 B10 未完项，不再是"完成（软件）"。

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

### B7 未完成项（明确列出）

- **固件适配器**：把 `app_ble_gatt_session_*` 绑到 `esphome_ble_gatt_*` 的 ops 实现（含地址字节序
  转换：`esphome_ble_gatt_nimble.c` 会反转 6 字节，而扫描证据是 NimBLE 原始顺序，两侧约定不一致，
  必须先定一个并写成有回归表的函数）。
- **control 后端**：`claims`/`send` 实现并注册进 `app_control`；通知 → `app_control_report()`/
  `app_control_confirm()`。注意 `app_control_backend_ops_t::send` 不传 request_id，而确认只认
  request_id，需要扩展 vtable 或让适配器用 `app_control_pending_at()` 反查。
- **GATT 组件自身两个缺陷**（已复核代码，未修）：
  (a) cancel 与在途操作竞态时**返回假成功**——`wait_kind` 是单一共享字段，cancel 把它改成
  `WAIT_DISCONNECT`，DISCONNECT 事件把 `op_status` 置 0，在途 read 的 `waitdone` 取到信号量后
  返回 `ESP_OK`，于是 `esphome_ble_gatt_read()` 以 `*len == 0` 报告成功；`discover` 在这种竞态下
  甚至可能返回 `ESP_OK` 加一个空数据库。
  (b) 操作在途时调用 `deinit()` 会删除信号量并清零会话，阻塞中的调用者随后解引用
  `i->backend == NULL`，是 use-after-free。
- **实体可写性**：`app_backend_is_drivable()` 是编译期开关且对所有控制后端返回 false，
  `BLE_GATT` 的写目标因此在识别阶段就被丢掉（`app_device_db.c` 只在 drivable 时保留
  `write_target_id`），`entity_upsert_recipe()` 还会丢掉 `read_source_id`，
  容器里的 `codec_id`/`subscription_id` 生产读取路径根本没读。要支持 BLE 控制必须先补这条链。
- **应用通路**：`firmware/main/` 里没有任何地方 include `esphome_ble_gatt.h`，
  `esphome_ble_gatt_nimble.c` 也不被任何 host 测试编译（`test_gatt.c` 用一个全零 ops 顶替生产符号）。
- **实板互操作**：真实 ESPHome 节点 + API 加密密钥；一块可控 BLE 外设。见 §9 与
  `docs/hardware-acceptance.md`。

### 本轮的编译期教训（第 4、5 次）

前三次已在 §"编译期问题的教训"记录。本轮又两次：

4. `tests/esphome_l2/test_noise_crypto.c` 里一个不再被使用的 `ad` 数组被 CI 的 **gcc**
   以 `-Werror=unused-but-set-variable` 拒绝，而本机 clang 不报这个诊断。两台编译器都要过。
5. 同一个提交里 `esphome_api.c` 有两处**只有目标构建能发现**的错误：`impl_t` 从未加上
   `noise_client_hello()` 读取的 `noise_psk` 成员；`hardclose()` 在 `nwipe()` 定义之前调用它。
   本机之所以没发现，是因为唯一编译 `esphome_api.c` 的 host 二进制需要 POSIX socket 头，
   本机构建在 include 阶段就停下了——**这就是"18 组全绿但目标构建失败"的第 5 次重演**。
   本地镜像现在把这次编译报成失败，而不是静默跳过。


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

## 6. 测试命令与结果

### 本机（2026-09-11，本轮）

```powershell
cd D:\OS\One-OS
.\tools\local\run-host-tests.ps1
```

结果：**16 组通过，2 组因本机缺 POSIX socket 头失败（esphome_l2、nmap_l2），后者只在 CI 验证。**

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
| `device_db_python` | 36 | 0 |
| `device_db_format` | 200 | 0 |

（16 组在本机通过；`esphome_l2` 与 `nmap_l2` 见上文。）

### CI

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
   BLE GATT 生命周期交接。
3. **B8**：Zigbee 原生 coordinator/ZDO/ZCL，复用已有 interview 超时与 last-known-good 修复。
4. **B9**：先修 Matter 构建（独立分支），再做 Thread 生命周期与 Matter 配网/订阅。
5. **B10/B11**：统一控制闭环与整机验收；B11 需要实板才能完成资源测量部分。

## 9. 需要人工提供的事项

| 阻塞 | 需要的最小操作 |
|---|---|
| 全部实板验收（射频、SD、串口、资源测量） | 连接一块 Waveshare ESP32-C6-Touch-LCD-1.9，确认串口端口号 |
| 真实 ESPHome 节点互操作（B7） | 一台可访问的 ESPHome 设备 + 其 API 加密密钥 |
| 真实 Zigbee 设备（B8） | 一个标准 Zigbee 灯具/开关 |
| 真实 Thread 网络（B9） | 一个 Thread Border Router 或 dataset |
| 生产 `devices.nbdb` | 真实设备库文件（或授权来源清单），用于验证索引桶预算 |
| Matter 构建 | 若需构建 connectedhomeip，需确认可用的子模块/工具链获取方式与时间预算 |
