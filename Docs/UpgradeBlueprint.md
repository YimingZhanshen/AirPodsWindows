# AirPodsWindows 升级蓝图 / Upgrade Blueprint

> **文档定位**：面向维护者与贡献者的中长期升级规划。以 v1.3.0（2026-10-21 发布）为基线，
> 依据 2026-10 对 [librepods v1.0.4](https://github.com/librepods-org/librepods)、[OpenPods 1.11](https://github.com/adolfintel/OpenPods)
> 与 MagicAAP 驱动生态的调研写成。条目均带证据出处与验收标准，可直接拆成 issue 实施。
>
> **一句话目标**：把「能用的 Windows ANC 工具」升级为「可信赖的 AirPods Windows 伴侣」——
> 分四个阶段推进：**正确性 → 驱动与可用性 → 信息与交互 → 扩展能力**。

---

## 0. 摘要 / TL;DR

| 维度 | 现状（v1.3.0） | 蓝图目标 |
|---|---|---|
| 上游同步 | AirPodsDesktop 0.6.1 全量并入（Qt 6.8.3 / x64 / 分层架构） | 保持同步，且**新功能尽量放进新文件**降低合并成本 |
| ANC 能力 | 降噪模式、对话感知（含说话音量滑条）、个性化音量、响亮声音降低、自适应通透（0–50） | 核对协议语义 → 补设备信息/电量/按键/自适应强度 → 远期 EQ/多设备 |
| 驱动链路 | 静默依赖 MagicAAP，装不上/失效都无提示 | **驱动状态可见 + 引导安装**，README 补齐两条安装路径与时效注意事项 |
| 质量 | CI 全绿（build/test/package/sign/release），但 **AAP/ANC 零测试覆盖** | 协议层纯函数 → 表驱动单元测试（用真实抓包样本） |
| 协议覆盖 | 仅用到 librepods 文档约 15%（6 条裸包 + 7 个控制 id） | 按用户价值逐项启用，文档化的 34 opcode / 41 控制 id 是矿藏 |

**三条核心判断**（详细论证见 §2、§6）：

1. **协议语义有 4 处分歧需修正**——我们对 `0x1B / 0x25 / 0x38` 的解释与 librepods 从 iOS 19.1 Beta 提取的 `control_commands.md` 不一致，若我们的解释有误，对应的 UI 功能实际在改别的设置（潜在 bug）。**已决定：按 librepods 文档直接修正**（§7 决策 1）。
2. **驱动是最大可用性短板**——官方版要关 Secure Boot + testsigning，社区签名版在 2026-04 Windows 更新后需手动移除驱动策略才能加载；而本仓库对这些**零记载**，应用内也无检测。
3. **fork 代码 2702 行、零测试**——且 38 个文件与上游改动面交集，未来合并成本高；新功能必须按「新文件优先」布局。

---

## 1. 现状基线（Baseline）

### 1.1 版本与发布

- **v1.3.0**（2026-10-21 发布）：并入上游 0.4.1→0.6.1（154 提交），Qt 5/Win32+x64 → **Qt 6.8.3 仅 x64**；
  安装器含 win32-bridge 兼容别名（老 1.x 客户端自动迁移）。
- 4 个 release 标题/资产命名/描述已规范化（`AirPodsWindows vX.Y.Z` / `AirPodsWindows-<版本>-<架构>-<类型>`）。
- 发布机制：tag → CI（Qt6 编译 + 4 测试套 + NSIS 打包 + win32-bridge + 可选签名）→ **draft** → 人工验收 → 发布。

### 1.2 架构与代码规模

```
apd_support ──▶ apd_domain ──┬──▶ apd_core ─────────┐
                             │                      ├──▶ apd_gui ──▶ AirPodsWindows (exe)
                             └──▶ apd_presentation ─┘
```

| 模块 | 内容 | fork 专属行数 |
|---|---|---|
| `Source/Core/AAP.h` | AAP 协议定义：6 条裸包、7 个控制命令、通知解析纯函数 | 554 |
| `Source/Core/AAPManager.cpp/.h` | L2CAP/RFCOMM/驱动四路连接、状态缓存、回调分发 | 868+7 |
| `Source/Core/MagicAAPWinRT.cpp/.h` | MagicAAP 驱动 WinRT 通道 | 907 |
| `Source/Core/MagicAAPClient.cpp/.h` | **死代码**（未进 CMake 源列表，从未编译） | 335+88 |
| 其余（AirPods/Settings/Tray/SettingsWindow/翻译） | ANC 设置、托盘降噪菜单、设置页卡片、14 语种词条 | ≈400 |
| **合计** | | **≈2702** |

### 1.3 能力现状

- **BLE 层**：电池/入耳/盒盖解析（比 OpenPods 全）、弹窗动画（10 机型动画 + PNG 回退）、快速连接、任务栏电量。
- **AAP 层**：降噪模式（Off/ANC/通透/自适应）、对话感知（含说话音量百分比）、个性化音量、自动入耳检测开关、响亮声音降低、自适应通透 0–50、自适应噪声 0–100、头部追踪启停。
- **通知解析**：说话等级、入耳状态、头部追踪数据、各状态位。
- **未暴露**（Core 已实现、UI 未接）：`SetAdaptiveNoiseLevel`（Core=6 引用 / **Gui=0**）、头部追踪（**只打日志**）、`SetAutomaticEarDetection`（Core=2 / Gui=0）。

### 1.4 质量基线

- CI：Qt 6.8.3 编译、4 测试套（`AirPodsDomainTests` / `QuickConnectTests` / `AnimationViewTests` / `UiRenderingTests`，共约 2136 行）、打包、win32-bridge、可选签名、draft release。
- **AAP/ANC 测试覆盖 = 0**（grep `AAP|NoiseControl|MagicAAP` 在 Tests/ 中全部是 `AppearanceMode`/`Cancel`/`Advance` 等误匹配）。
- 翻译：14 语种（bg/de/fr/he/it/ja/ko/pt-BR/pt-PT/ru/tr/uk/zh-CN/zh-TW），he_IL 的上游新词条待回填。

---

## 2. 调研结论（Evidence）

### 2.1 librepods —— 协议字典（不是竞品）

- **定位**：Android（Kotlin/Compose）+ Linux（Qt6/C++）；**主分支无 Windows 代码、无驱动源码**。全仓库检索 `windows|driver|*.sys|*.inf` 零命中。
- **许可证 GPLv3**（2025-11-16 起）→ 与本项目同许可证，常量表/解析逻辑可直接移植并注明出处。
- **版本**：v1.0.4（2026-10-16，`android/version.properties`）；另有 `android/rewrite` v2.0.0-alpha.1、`linux/rust` 分支。
- **协议面**：包格式 `04 00 04 00 [opcode LE16] [data]`，L2CAP **PSM 0x1001**；
  `docs/opcodes.md` 34 个 opcode、`docs/control_commands.md` 41 个控制命令 id（注明提取自 **iOS 19.1 Beta 23B5044l**）、3 个 ATT handle。
- **近一年演进（与我们相关的）**：
  - `4f2d11a`（2026-04-15）**重命名 opcode 0x1E → 0x1A 修正**——将来做改名功能直接用 0x1A。
  - `30d16e9`（2026-04-28）文档重组：`AAP Definitions.md` 迁入 `docs/`，新增 `opcodes.md` / `device-info.md`。
  - `99d6d73`（2026-10-06）**AirPods 5 / AirPods 5 WCC**（A3531–33、A3439–41）。
  - `7341e41`（2026-06-13）自定义 EQ（iOS 27，opcode `0x63`）。
  - 术语改名：noise control → **listening mode**、conversational → **conversation awareness**（跟 iOS 口径）。
- **型号覆盖**：AirPods 1/2/3/4/4(ANC)/5/5(WCC)/Pro1/Pro2(L)/Pro2(USB-C)/Pro3 共 11 型，带能力位
  `LISTENING_MODE / CONVERSATION_AWARENESS / STEM_CONFIG / HEAD_GESTURES / LOUD_SOUND_REDUCTION / PPE / SLEEP_DETECTION / HEARING_AID / ADAPTIVE_AUDIO / ADAPTIVE_VOLUME / SWIPE_FOR_VOLUME / HRM`；**无 Beats/Powerbeats**（MagicPods 驱动页则覆盖全系 Beats）。
- **可信度折扣**（影响实施方式）：`opcodes.md` 链接的 12 篇子文档**从未提交**、CHANGELOG 只有两行、控制命令表来自 Beta 固件 →
  **字节表可参考，行为必须真机验证**。

### 2.2 OpenPods —— BLE 解析参考

- v1.11（2025-09-26），GPLv3，Android。
- 解析 27 字节 `07 19` 厂商数据：型号 id（4 位）、入耳位、左右电量（0–10→0–100%）、充电状态、盒电量、左右翻转位。
- **不如我们**：不解盒盖状态、无双侧合并、不支持 AirPods 4/5；**可借鉴**：RSSI > -60 dBm +「最近 10 秒最强信标」的设备锁定启发式；4 位型号 id 可与我们的 `0x20xx` 表交叉核对。

### 2.3 MagicAAP 驱动 —— 唯一依赖，且脆弱

| 方案 | 步骤 | 时效/风险 |
|---|---|---|
| Official（未签名） | 建还原点 + 备份 BitLocker 密钥 + 关 Secure Boot + `bcdedit /set testsigning ON` + 右键装 `MagicAAP.inf` | 长期有效，但代价高；驱动版本 `0.0.0.1`（2024-06-06）至今未变 |
| Community（社区签名） | 一键 `irm "https://magicpods.app/utils/magicaap-community-v1.ps1" \| iex`；需临时关 Defender + DriverStore 排除 | **2026-04 Windows 安全更新后无法加载**，须先 `CiTool.exe --remove-policy "{8F9CB695-5D48-48D6-A329-7202B44607E3}"` 并保持禁用 |

- 都要求 MagicPods ≥ 2.0.72.0；驱动解锁：降噪控制、精确电量、按键定制、增强耳检测、对话感知、可访问性设置；支持全系 AirPods + Beats。
- **本仓库现状**：README 仅「可选安装 MagicAAP 驱动」一句 + 链接；`Secure Boot` / `testsigning` 在仓库内出现 **0 次**；
  `IsMagicAAPDriverAvailable()` 从未接 UI（仅日志）→ **装不上、被更新搞坏，用户都无从判断**。
- 本机 `magicaap.zip` SHA256 `E16A8F1C…B66C2` 与官网公布的官方未签名包一致。

### 2.4 上游 AirPodsDesktop —— 同步状态与冲突热点

- 已同步至 **v0.6.1**（`56b0400`），核对无漂移。
- 上游近半年改动最热文件（未来冲突热点）：`AirPods.h/cpp`、`Settings.h/cpp`、`MainWindow.*`、`TrayIcon.*`、`SettingsWindow.*`、`Application.*`、`Update.cpp`、7 个 `.ts`。
- **我们与上游改动面交集 38 个文件** → 每次合并都是手工活；新功能必须尽量**新增文件**（上游自己也这么做：`QuickConnect_*`、`SettingsRepository`、`AirPodsAdvertisement` 都是新文件）。

---

### 2.5 magicaap_driver 驱动能力评估（决定蓝图可行性上限）

对本机 `magicaap_driver\`（官方未签名包解压产物）与 `test_magicaap.cpp`、`MagicAAPClient.cpp` 的实证分析：

**驱动是什么**：微软 **BthEcho 蓝牙示例驱动的改造品**——`.sys` 内字符串
`BTHECHO_CONNECTION` / `BTHECHOSAMPLE_CLIENT_CONTEXT` / `BTHECHOSAMPLE_CLIENT_FILE_CONTEXT`，
PDB 路径 `C:\Users\steam\Local Storage\Repos\steam3d\MagicAAP\MagicAAP\x64\Release\MagicAAP.pdb`（作者 steam3d）。
INF：`Class=Bluetooth`、绑定 `BTHENUM\{74ec2172-0bad-4d01-8f77-997b2be0722a}`（AAP 服务 UUID）、
KMDF 1.15、demand-start、**仅 x64**（`NTamd64.10.0.1..16299`）、23.9 KB。

**I/O 模型（关键）**：设备接口 `GUID_DEVINTERFACE_MAGICAAP` → `CreateFile` → **`ReadFile`/`WriteFile` 直读直写**，
`.sys` 与客户端代码中**均无 DeviceIoControl / IOCTL 码** → 它就是一条 **AAP 通道的原始双向字节管道**
（线上对应 L2CAP PSM 0x1001）。驱动不做协议解析、不提供附加能力（无 RSSI/HCI/配对接口），
只负责把字节送进/送出 AAP 通道。

**逐项可行性判定**（蓝图 §4 各功能是否被驱动挡住）：

| 功能类别 | 字节可达性 | 判定 |
|---|---|---|
| AAP 全部 opcode（`0x04` 电量 / `0x06` 耳检测 / `0x19` 按键 / `0x1D` 设备信息 / `0x17` 头追踪 / `0x4B` 对话感知级别 / `0x4D` 特性标志） | ✅ 纯字节 | **驱动完全可支撑** |
| 全部 41 个控制命令（`0x09` + id，含聆听模式/自适应强度/提示音/睡眠检测/HRM 开关…） | ✅ 纯字节 | **驱动完全可支撑** |
| EQ / 耳机适配（`0x53`）、自定义 EQ（`0x63`） | ✅ 纯字节 | **驱动完全可支撑** |
| ATT 帧写（透明度定制 `12 18 00…`、响亮声音抑制 handle `0x1B`、助听器 handle `0x2A`） | ✅ 同样只是字节（`12` = ATT Write Command + 2 字节 handle） | **字节可达**；固件是否在无宿主伪装时接受待实测。MagicPods 官方页宣称驱动解锁「iPhone 同款可访问性设置」→ 乐观 |
| 多设备 smart routing（`0x10/0x11/0x2D/0x2E`） | ✅ 纯字节 | **驱动可支撑**；librepods 的宿主伪装是为 Android 栈服务，Windows 侧待实测 |
| 近场配对密钥（`0x30/0x31`） | ✅ 纯字节 | 驱动可支撑；**数据主权红线**，需本地加密存储设计 |
| VendorID/宿主伪装（"act as Apple device"） | ❌ 驱动无此能力 | 需额外过滤驱动或栈层方案（L，且与最小权限冲突）→ 仅当实测证明 ATT 写必须伪装时才考虑 |
| 多设备接口枚举 | ✅ SetupAPI 按接口枚举可列多设备 | 为 §4.1 多设备预留了通道基础 |

**结论**：**驱动不是蓝图中任何协议功能的瓶颈**——它提供了理论上限的传输能力（任意字节双向）。
真正决定功能上限的是：① 固件对各类写入的接受度（尤其 ATT 写），② 我们的实现与 UI 投入。
唯一的硬缺口是「宿主伪装」；按 MagicPods 的宣传口径，主要可访问性功能应可在现有管道内实现。

**顺带发现**：`MagicAAPWinRT.cpp` 另有一条 **WinRT RFCOMM 路径**（`RfcommServiceId::FromUuid(GUID_AAP_SERVICE)`
+ `StreamSocket`），可在驱动缺失时兜底；RFCOMM 与 L2CAP PSM 0x1001 是否同一服务面，需在阶段 0 一并核实。

---

## 3. 设计原则（Design Principles）

1. **正确性优先于功能**：协议语义未验证前，不新增依赖该语义的功能。
2. **新逻辑进新文件**：协议扩展放 `AAP` 平级新文件（如 `AAPDevice.h/.cpp`、`AAPStem.cpp`），
   UI 能力放新 Widget / 新卡片，避免加宽与上游的 38 文件交集。
3. **协议层可测试**：解析/构造必须是纯函数（现有 `AAP.h` 的 `Parse*` 已是），配真实抓包样本做表驱动测试。
4. **驱动状态可见**：任何依赖驱动的能力，失败时必须给出可行动的提示（不是静默降级）。
5. **尊重数据主权**：设备信息/序列号等只留在本地与用户可见处，不上传、不进日志明文（沿用 `LogSensitiveData` 习惯）。
6. **上游优先回流**：对上游也有价值的修复（如 BLE 匹配启发式、解析 bug）应整理成 PR 上游，减少长期分歧。

---

## 4. 分阶段蓝图（Roadmap）

> 记号：**成本** S≤0.5 天 / M≤2 天 / L>2 天；**冲突风险** 低（新文件）/ 中（新设置项+新 UI）/ 高（改动 AirPods.*、Settings.* 等热点文件）。

### 阶段 0 · 正确性核查（P0）— 目标版本 v1.3.1

| # | 条目 | 目标/价值 | 证据 | 落点 | 成本 | 冲突 | 验收标准 |
|---|---|---|---|---|---|---|---|
| 0.1 | **控制命令语义修正** | 按 `control_commands.md` 修正 `0x1B / 0x25 / 0x38`：`0x1B`=OneBudANC（现被当"摘下暂停"）、`0x25`=VolumeSwipe（现被当"响亮声音降低"）、`0x38`=PPE Cap（现被当"自适应通透级别"） | `AAP.h:220/229/240` vs `librepods/docs/control_commands.md` | `Source/Core/AAP.h` + 对应设置项语义 | M | 中 | 修正后的构造/解析配表驱动测试；UI 描述与发行说明同步更正语义 |
| 0.2 | **`0x4D` 特性标志取值** | 采信 librepods 实现值 `D7`（我们现用 `FF`） | `AAP.h` `EnableFeatures`；`AACPManager.kt`、`linux/airpods_packets.h` | `AAP.h` | S | 低 | 改为 `D7` 并注释依据；CI 绿 |
| 0.3 | **响亮声音降低通路** | 语义修正后其实现是 **ATT Write Command 到 handle `0x1B`**（`12 1B 00 …`），与 L2CAP 控制命令无关 | `ATTManager.kt` `ATTHandles`；§2.5 | 新 `Source/Core/AAPAtt.h/.cpp`（ATT 帧构造，新文件） | M | 低 | 同一管道发 ATT 写；实测固件接受度；不可用则 UI 明示 |
| 0.4 | **`RequestSettings` 死常量清理** | 命名误导（0x0D 实为音频源请求）、且从未发送 | `AAP.h:246`；`AAPManager.cpp` 零引用 | `AAP.h` | S | 低 | 删除或改名；CI 绿 |
| 0.5 | **说话等级语义回归** | 已核对一致（≤0x02 降音/0x03 恢复/≥0x08 正常），补测试固化 | `AAP.h:299` `ParseSpeakingLevel` | `Tests/`（新增 `AAPProtocolTests`） | S | 低 | 表驱动测试覆盖 0x01–0x09 全部等级 |

### 阶段 1 · 驱动与可用性（P1）— 目标版本 v1.3.1

| # | 条目 | 目标/价值 | 证据 | 落点 | 成本 | 冲突 | 验收标准 |
|---|---|---|---|---|---|---|---|
| 1.1 | **驱动状态卡片** | 设置页显示驱动状态（未装/已装/已连接/连接方式=驱动 or 原生 socket），装不上时给可行动指引 | `AAPManager.h:119/122` 从未接 UI；`SettingsWindow.ui` 现有 `cardDevice` 可扩展 | 新 `Source/Gui/Widget/DriverStatusCard.*`（新文件）+ 设置页挂载 | M | 低 | 四种状态正确显示；未装时一键打开帮助页/复制安装命令 |
| 1.2 | **README/Docs 驱动章节** | 补两条安装路径、2026-04 Driver Policy 注意事项、版本要求 | §2.3 | `README*.md`、`Docs/Build.md` 或新 `Docs/Driver.md` | S | 低 | 中英双语、含 `CiTool.exe --remove-policy` 与 SHA256 校验值 |
| 1.3 | **连接失败可诊断** | AAP 连接失败时给用户可读原因（驱动缺失/设备不支持/被占用），而非只写日志 | `AAPManager.cpp:69-140` 四路连接、`AirPods.cpp:401` 仅 warn | `AAPManager` 新增 `LastError` 暴露 + 托盘/设置页提示 | M | 中 | 断开后托盘气泡或设置页显示原因与建议动作 |

### 阶段 2 · 信息与交互（P1/P2）— 目标版本 v1.4.0

| # | 条目 | 目标/价值 | 证据 | 落点 | 成本 | 冲突 | 验收标准 |
|---|---|---|---|---|---|---|---|
| 2.1 | **「关于耳机」信息卡** | 显示固件版本×2、硬件版本、型号、序列号（含左右耳）、厂商 | opcode `0x1D` 连接即推；`docs/device-info.md` + `AACPManager.kt parseInformationPacket`（见附录 A） | 新 `Source/Core/AAPDeviceInfo.h/.cpp`（解析纯函数）+ `SettingsWindow` cardDevice 扩展 | M | 中 | 真机显示正确；序列号不写日志明文（`LogSensitiveData`） |
| 2.2 | **AAP 电量**（含优化充电状态） | 与 BLE 广播互补（BLE 只有 0–10 档、易丢包；AAP 电量更精确） | opcode `0x0004`；librepods `Packets.kt` | `AAPDeviceInfo` 或 `AAPBattery` 新文件 + 状态合并策略 | M | 中 | 双路电量一致时显示、不一致时标注来源 |
| 2.3 | **自适应强度滑条** | 纯 UI 补全：`SetAdaptiveNoiseLevel` 已实现但未暴露 | `AirPods.h` `SetAdaptiveNoiseLevel`（Core=6/Gui=0） | `SettingsWindow` 新卡片 + `SETTINGS_FIELDS` 新增 `adaptive_noise_level` | S | 中 | 滑条 0–100 生效且持久化；仅自适应模式可见/可用 |
| 2.4 | **杆按键事件（媒体控制）** | 单/双/三击、长按 → Windows 媒体控制或自定义动作 | opcode `0x19`（`SINGLE 05/DOUBLE 06/TRIPLE 07/LONG 08`，`L 01/R 02`） | 新 `Source/Core/AAPStem.*` + `SETTINGS_FIELDS` 表 + `GlobalMedia` 扩展 | L | 低 | 默认媒体控制生效；可配置为「无/播放暂停/下一首/上一首/ANC 切换」 |
| 2.5 | **头追踪可视化/空间音频开关** | 现有回调只打日志；可做姿态可视化或「头部追踪开关」进设置 | `0x17` 数据偏移（附录 A）；`AirPods.cpp:761` | `apd_presentation` 新 ViewModel + 可选调试面板 | M-L | 低 | 开关生效；调试面板显示姿态/加速度实时值 |

### 阶段 3 · 工程质量（P2）— 跨版本持续

| # | 条目 | 目标 | 证据 | 落点 | 成本 | 冲突 | 验收标准 |
|---|---|---|---|---|---|---|---|
| 3.1 | **AAP 协议单元测试** | 表驱动覆盖 `Parse*`/构造函数，样本取真实抓包 | Tests/ 现有 4 套、AAP 覆盖 0 | 新 `Tests/AAPProtocolTests.cpp`（链 `apd_core`） | M | 低 | 覆盖全部 `Parse*` 与裸包构造；CI 跑通 |
| 3.2 | **连接状态机 + 退避** | 消除并发连接尝试、失败后指数退避重试 | `AirPods.cpp:362` `ConnectAAP` 仅 `IsConnected()` 守卫 | `AAPManager` 新增 `ConnectState` 状态机 | M | 中 | 断连/驱动拔插反复 30 分钟无重复连接、无资源泄漏 |
| 3.3 | **握手响应驱动** | 用响应/超时替代固定 `kPacketProcessingDelay` sleep | `AAPManager.cpp:233/718` | `AAPManager` | M | 中 | 握手超时可诊断；时延不劣化 |
| 3.4 | **死代码清理** | `MagicAAPClient.cpp/.h` 355 行未编译、`RequestSettings` 死常量 | CMakeLists 无引用 | 删除或归档 | S | 低 | CI 绿；`Docs/` 记录曾有该实现 |
| 3.5 | **翻译回填与术语对齐** | he_IL 上游新词条回填；评估 listening mode/conversation awareness 术语是否跟 iOS | 14 语种清单；librepods `e243d20/a383341` | `APD_CREATE_UPDATE_TS` + 7 语种词条 | S-M | 低 | `lupdate` 后无未翻译漏项（或标注未翻译）；术语决策记录在案 |
| 3.6 | **型号表补全** | AirPods 5 WCC（A3439–41 / 0x2030? 待核）、Powerbeats 3/Beats X/Solo3（现被注释） | `AppleCP.cpp`、`Base.h`；librepods `AirPods.kt` | `AppleCP.cpp` + `Base.h` + `GetAnimationPresentation` + 测试 | S | 中 | 新型号识别 + 动画映射 + `MapsMainWindowAnimationResources` 通过 |

### 阶段 4 · 扩展能力（P3）— 目标版本 v2.0.0

| # | 条目 | 说明 | 依赖/风险 |
|---|---|---|---|
| 4.1 | **多设备（smart routing / TiPi）** | `0x10/0x11/0x2D/0x2E` 设备列表与接管、弹窗；librepods `sendHijackRequest` 可参考 | 现为单设备绑定，需 UI 大改（设备列表页）；L |
| 4.2 | **EQ / 耳机适配** | `0x53`（8 段 EQ×3、Phone/Media 开关）、`0x63` 自定义 EQ（iOS27） | 需读写状态管理；UI 需曲线控件；M-L |
| 4.3 | **透明度定制** | ATT `12 18 00 …`（放大/音调/对话增强/环境降噪 + EQ8），写后须补发 `0x53` | **需 ATT/VendorID spoof**（Windows 侧等价手段待调研）；L，可能长期搁置 |
| 4.4 | **助听器 / HRM / 睡眠检测** | `0x2C/0x33`+ATT `0x2A`、`0x30` HRM、`0x35` 睡眠 | 能力位与机型强相关；HRM 在 `android/rewrite` 分支尚未稳定；L |
| 4.5 | **近场配对密钥 / QR 分享** | `0x30/0x31` IRK/ENC_KEY | 涉及密钥保管（数据主权红线）；需设计本地加密存储；L |
| 4.6 | **Find My** | librepods 亦仅计划未实现 | 等上游社区突破；观望 |

---

## 5. 版本与发布规划

| 版本 | 范围 | 关键门槛 |
|---|---|---|
| **v1.3.1**（patch） | 阶段 0 全部 + 阶段 1 全部 + 3.4/3.5 | P0 真机验证结论；驱动状态卡片实机验收；win32-bridge 升级路径回归 |
| **v1.4.0**（minor） | 阶段 2 全部 + 3.1/3.2/3.6 | 设备信息/电量显示正确；AAP 测试入 CI；多语种词条齐全 |
| **v2.0.0**（major） | 阶段 4 择项 + 术语对齐 + 多设备 | 依赖驱动能力上限与 ATT 路线结论 |

发布流程沿用 `Docs/Release.md`：版本号同步 → 发行说明（双语，`Docs/ReleaseNotes/`）→ 打 tag → CI → draft →
下载资产校验 SHA-256（win64 与 bridge 必须相同）→ 人工验收（含**从上一正式版直接升级**）→ 发布。
可选增强：配置 `WINDOWS_SIGNING_CERTIFICATE_*` 免 SmartScreen 提示；接入 winget/scoop 分发。

---

## 6. 风险与依赖（Risks）

1. **驱动时效风险（最高）**：社区签名版受 Windows 2026-04 Driver Policy 影响；官方版要求关 Secure Boot。
   → 缓解：驱动状态卡片 + 文档 + 与 MagicPods 社区保持同步；长期看只能等驱动签名方案成熟。
2. **上游合并成本**：38 文件交集，`AirPods*/Settings*/TrayIcon*/SettingsWindow*` 是双热区。
   → 缓解：新文件优先（§3.2）；对上游有价值的修复回流 PR；每次合并预留整块时间（本次 0.6.1 合并含 31 冲突）。
3. **协议文档可信度**：控制命令表来自 iOS 19.1 Beta、12 篇子文档未提交、opcodes.md 与实现存在自相矛盾（0x1E/0x1A）。
   → 缓解：**字节表可参考、行为必须实测**；结论回写 `AAP.h` 注释与测试样本。
4. **ATT/VendorID spoof 不确定**：响亮声音降低/透明度定制/助听器可能需要 Windows 侧等价的 VendorID 伪装，目前无已知方案。
   → 缓解：阶段 0.3 先确认现状；若确实不可用，UI 明示限制，不写进宣传。
5. **法律与商标**：GPLv3（含上游与 librepods）；README 已声明 fork 关系与致谢；MagicAAP/MagicPods 为第三方商标，文档仅引用链接。
6. **本机环境限制**：开发机无 Qt 6.8.3（CI 负责编译验证）；AAP 真机验证需要 AirPods + MagicAAP 驱动 + 抓包手段。

---

## 7. 决策记录（Decisions Taken）

| # | 议题 | 决定 | 执行口径 |
|---|---|---|---|
| 1 | P0 协议语义分歧的处理 | **采信 librepods 文档直接改**（维护者 2026-10-22 拍板） | 按 `control_commands.md`（iOS 19.1 Beta 提取）修正 `0x1B/0x25/0x38` 语义与解析/构造代码；`0x4D` 采信实现值 `D7`；改动附表驱动回归测试，行为差异记入发行说明 |
| 2 | UI 术语 | **跟随 iOS 口径** | noise control → **聆听模式**（Listening Mode）、conversational awareness → **对话感知**（Conversation Awareness）；涉及 UI 字符串与 14 语种词条，用 `APD_CREATE_UPDATE_TS` 统一重生成 |
| 3 | 签名证书 | **暂不配置**（维护者授权自行决定） | 沿用未签名 + 发行说明 SmartScreen 提示；保留 `WINDOWS_SIGNING_CERTIFICATE_*` 接口，等发布节奏稳定后再买证书 |
| 4 | ATT/VendorID spoof 路线 | **先在现有字节管道上直接尝试 ATT 写**，不为伪装新建驱动 | 依据 §2.5：驱动是字节管道、MagicPods 宣称可访问性可用 → 成本低；若实测固件拒绝，再评估过滤驱动（L）或明示限制 |
| 5 | 多设备 | **不列为 v2.0.0 主打**，降为 P3 实验项 | UI 改动大、收益存疑；§2.5 已确认驱动侧无阻碍，等待用户需求信号 |
| 6 | 蓝板存放与语言 | **提交进仓库** `Docs/UpgradeBlueprint.md`，暂不做英文全译 | 公开面（README）已三语；蓝图以中文为主、术语带英文；实施到各阶段时按需补英文摘要 |
| 7 | 驱动评估 | **已完成（§2.5）** | 结论：驱动不构成任何协议功能的瓶颈；唯二硬缺口＝宿主伪装（仅特定 ATT 场景可能需要）与「固件接受度需实测」 |

---

## 8. 附录

### 附录 A · 协议速查（字节级）

> 待补：来源 `librepods`（`docs/AAP Definitions.md`、`docs/device-info.md`、`docs/control_commands.md`、
> `android/.../AACPManager.kt`、`linux/airpods_packets.h`），逐条标注出处与分歧。

### 附录 B · 参考资料

| 资料 | 路径/链接 | 用途 |
|---|---|---|
| librepods v1.0.4 | `_research\librepods`（HEAD `89356c0`） | 协议字典、解析实现、型号能力表 |
| OpenPods 1.11 | `_research\OpenPods`（HEAD `9cd8008`） | BLE 广播解析交叉核对 |
| MagicAAP 驱动 | https://magicpods.app/magicaap/ 、`help.magicpods.app` | 驱动安装与限制 |
| 上游 AirPodsDesktop | https://github.com/SpriteOvO/AirPodsDesktop | 同步基线（v0.6.1） |
| 本项目 v1.3.0 发布 | https://github.com/YimingZhanshen/AirPodsWindows/releases/tag/v1.3.0 | 现状基线 |

### 附录 C · 关键证据索引（本仓库）

| 事实 | 出处 |
|---|---|
| PSM 0x1001 | `Source/Core/AAP.h:37` |
| 4 路连接（L2CAP×2 / RFCOMM / MagicAAP） | `Source/Core/AAPManager.cpp:69-140` |
| 控制命令 id 使用 | `Source/Core/AAP.h:170/178/187/211/220/229/240` |
| `RequestSettings` 死常量 | `Source/Core/AAP.h:246`（`AAPManager.cpp` 零引用） |
| 说话等级解析 | `Source/Core/AAP.h:299` |
| 驱动检测未接 UI | `Source/Core/AAPManager.h:119/122`（仅 3 处引用，全在 AAPManager） |
| 自适应噪声未暴露 | `SetAdaptiveNoiseLevel` Core=6 / Gui=0 |
| 头部追踪仅打日志 | `Source/Core/AirPods.cpp:729` `OnHeadTrackingData` |
| AAP 测试覆盖 0 | `Tests/` grep 全为误匹配 |
| 与上游交集 38 文件 | `git diff --name-only 254c079..upstream/main` ∩ `git diff --name-only upstream/main..HEAD` |
| 设置页设备卡片落点 | `Source/Gui/SettingsWindow.ui:350-404`（`cardDevice`） |
| 托盘降噪菜单随 ANC 状态显隐 | `Source/Gui/TrayIcon.cpp:80/183/212` |
