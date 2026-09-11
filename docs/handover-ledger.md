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
| **B5 SD 读取/识别/配方** | **完成** | **通过** | **通过** | **未做** | 见 §4 |
| **B6 配网与导入后端** | **进行中** | **通过** | **通过** | **未做** | HTTP 路由与临时 APSTA 未接；见 §4b |
| B7 BLE GATT / ESPHome | 部分（已有组件） | 通过 | 通过 | 未做 | Noise 认证未实现；GATT 交接未接应用 |
| B8 Zigbee 原生后端 | 未开始 | 部分 | 通过 | — | 无原生 coordinator；无应用通路 |
| B9 OpenThread / Matter | 部分 | 通过 | 通过 | 未做 | Matter 构建未修；Thread 生命周期未接应用 |
| B10 统一控制闭环 | 未开始 | — | — | — | 全部 |
| B11 无 GUI 整机验收 | 未开始 | — | — | — | 全部 |

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

1. **HTTP 路由与处理器**（`/api/status`、`/api/wifi/scan`、`/api/wifi/connect`、`/api/db/upload`、
   可选 `/api/portal/finish`）。线层（`app_portal`）与会话逻辑（`app_provision`）已就绪，
   处理器应当是薄传输层。
2. **AP 密码的本地出口**：按产品规则只在设备自身呈现与本地串口输出，不得进入日志或
   `/api/status`。生成逻辑已实现并测试，出口未接。
3. **NVS 凭据持久化**：`app_wifi` 已实现 `wifi_mgr_set_credentials`/`clear`/启动加载，
   但"连接失败后保留凭据以便重试"与"版本标记"未验证。
4. **`/api/status` 尚未接 `app_runtime_db_state()`**（库状态与版本已可取）。
5. **实板全部未做**：无板、无浏览器、无真实 HTTP 服务器。

**关于 `wifi_mgr_ap_*` 的证据等级**：它是本轮唯一完全没有自动化测试覆盖的产物。
host 测试无法编译 ESP-IDF 专属代码，所以它只有"目标构建通过"这一项证据，
AP 实际能否启动、STA 能否在其后恢复，**都未验证**。


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
