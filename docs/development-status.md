# 开发状态与清理审计

审计日期：2026-09-11。代码基准：`de1afc42b73cb1695ff24bf25c1c77b92fe7958b`。

## 当前结论

目前是“硬件基础 + 已汇总的协议组件，等待应用集成”。不能用组件数量推算产品完成百分比。
`firmware/main/main.c` 仅初始化 LCD、LVGL/触摸并运行 LVGL 定时处理；没有创建设备列表页面，也没有启动扫描、配网或 SD 数据库。

| 部分 | 已有代码 | 尚未完成/验证 |
|---|---|---|
| 板级基础 | 共享 SPI、LCD、触摸、SD 驱动及 LVGL 绑定 | 当前 main 不挂载 SD；本次未连接实板 |
| HA | RAM 内 Device/Entity/State 模型、mDNS/SSDP | 应用设备映射、持久化、UI、控制派发 |
| Kismet / Wireshark | Wi-Fi/BLE 扫描会话和跟踪、有限协议解析 | 应用调度、原生无线初始化与整机联合验证 |
| Nmap | 有边界的 LAN 主机/端口/服务发现 | 已联网前提及产品调用链 |
| Theengs | Ruuvi RAWv2、部分 BTHome v2 被动解码 | 完整设备识别库；加密 BTHome 不支持 |
| ESPHome | 明文 Native API 子集、BLE GATT 与 NimBLE 后端 | Noise 未实现；Native API command 明确返回不支持，不能宣称可控制 |
| ZHA / zigpy | quirk/能力转换、后端回调驱动的 interview/ZCL 事务逻辑 | 仓库没有具体原生 Zigbee backend；预留分区不等于协议栈已接通 |
| OpenThread | 网络发现、状态/拓扑、dataset attach、Joiner | 原生栈生命周期由应用负责；不能等同 Matter 控制器 |
| Matter | 独立研究分支有未合并代码 | 最新构建失败；保留分支单独修复 |
| 产品应用 | 三份 application 规范 | 配网、Web 管理、SD DB、统一 Device/Entity 页面、控制回执闭环尚未实现 |

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

本次未删除远端分支：Git HTTPS 写入缺少凭据，可用 GitHub 连接没有删除 ref 操作。
维护者可运行 `python3 tools/cleanup_remote_branches.py` 预览；确认后加 `--apply`。
脚本先镜像备份所有 refs、检查 main 未变化及候选 SHA 未变化，再用逐分支 lease 和 atomic push 删除候选；若远端不支持则停止，不降级为无保护删除。

## 当前开发顺序（用户已更新）

先完成无 GUI 的底层和应用闭环，最后接 GUI。详细任务、依赖、验收与新增边界缺口见 [GUI 之前的开发任务书](pre-ui-development.md)。之前“先接屏幕设备列表”的建议已被此顺序替代。

继续遵守三份 application 文档的最终产品行为：生产识别库只放 SD、未知设备不丢弃、最终使用统一 HA 风格 Device/Entity UI。每一步记录提交 SHA、host/CI 证据及单列的实板验收结果。
