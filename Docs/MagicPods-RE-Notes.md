# MagicPodsService.exe 逆向取证笔记（2026-10-22）

对 MagicPods（Microsoft Store 版）`MagicPodsService.exe` 的静态逆向，目的是为 AAP-Protocol-Spec
提供**第三实现**的字节级交叉验证，并回答四个悬置问题：ATT 传输路径、0x004D 标志字节（D7/FF）、
连接初始化顺序、VendorID spoof。全程只用 REA managed 静态分析（`rea inspect-managed-artifact` /
`inspect-managed-members` / `inspect-managed-native-boundaries`）+ 本地元数据解析脚本，未运行目标。

## 0. 目标身份

| 项 | 值 |
|---|---|
| 文件 | `_research\magicpods_pkg\MagicPodsService.exe`（MSIX 提取） |
| SHA-256 | `ee569afe32a49ad509f0903c6f2815fdc90a417ca6d5d3af692235e94d31682d`（8,419,328 B） |
| CLR | IL-only，.NETFramework v4.0.30319（4.7.2 目标），x86_64，无强签名 |
| MVID | `f6f82372-1530-4655-be32-a89105f26b91` |
| 混淆 | ArmDot（方法/类型名打乱、字符串加密、`calli`+`ldftn` 函数指针分发） |
| 元数据 | 6729 TypeDef / 16989 MethodDef / 59 ImplMap / 5816 FieldRVA；`#US` 流**不存在**（0 条 ldstr，字符串全部加密） |

## 1. 逆向路径（可复现）

1. `inspect-managed-native-boundaries` 拿全部 59 个 P/Invoke（含 member token 与声明类型）。
2. `inspect-managed-members --type <精确全名>` 绕开 64 MB 元数据表示预算（全量准入会触发
   `resource-limit`）；`--type` 单类型准入 `coverage: complete`，给出成员、签名、CIL 锚点、调用边。
3. 自写 PE/CLI 元数据解析（`_research\parse_cli_*.ps1`、`scan_call_sites.ps1`）：TypeDef/MethodDef/
   FieldRVA 表偏移用 REA 证据里的 `row_offset` 锚点校准（FieldRVA 行宽 = 6 字节：RVA(4)+Field(2)）。
4. ArmDot 字符串解码（`_research\decode_armdot_strings.ps1`）：`newarr System.Char` + 逐元素
   `K1^K2` 或 `fieldData[i]^K` 的解码桩，99 条字符串全部还原。
5. FieldRVA 静态数据块扫描（`_research\scan_fielddata.ps1`）：定位 AAP 包模板表与 UI 字符串表。

## 2. 结构地图（ArmDot 混淆名 → 职能）

| 混淆全名（TypeDef 行） | 职能 | 证据 |
|---|---|---|
| `GalaxyBudsSDK.Stages.getTagsDeleteKeyset.ContractReferenceAssemblyAttributeUTFByteArray`（517） | **驱动管道 I/O 封装**：CreateFile / ReadFile / ReadFileEx / WriteFile / WriteFileEx / DeviceIoControl / CancelIoEx / CancelSynchronousIo / DuplicateHandle / LocalAlloc / CloseHandle，全部 P/Invoke 在此类 | ImplMap 0x1c000030-0x1c00003b |
| `AirohaSDK.AirReps156X.Stages.getRootIsValueCreated.getIsFamilyAndAssemblygetKeyAvailable`（555，112 方法） | **连接管理层**：唯一调用管道 I/O 类的上层；对上暴露 `callvirt` 接口 | 调用点扫描（`calls_to_transport.txt`） |
| `GalaxyBudsSDK.Struct.VariantHResults.setExceptionMessageByteCapacity`（~250 方法） | **AAP 会话/功能层**：`Set anc to…` 等日志字符串、`aap_*` 参数名均在此 | 解码字符串 + `callvirt` 连接层 |
| `GalaxyBudsSDK.Enums.ThreadInterruptedgetVersion.getHasFlushedFinalBlockHeaderVersionNumber`（381） | bthprops 蓝牙服务管理：BluetoothEnumerateInstalledServices / BluetoothSetServiceState / BluetoothGetDeviceInfo（函数指针表安装器模式） | ImplMap 0x1c00001c-0x1c000022 |
| `AirohaSDK.AirReps156X.Stages.OSVERSIONINFOEXConvertHexDigit.GetFieldMarshalFullDateTimePattern`（700） | **包模板工厂**：10 个 `ldtoken`+`RuntimeHelpers.InitializeArray` 模板初始化器 + 委派分发表（字段 0x04000ee8） | `calls_to_4d_init.txt`、方法 0x06004074-0x0600407f |
| `MagicPods.Setupapi`（313） | SetupAPI 设备接口枚举（SetupDiGetClassDevs/EnumDeviceInfo/GetDeviceProperty） | ImplMap 0x1c000009-0x1c00000d |
| 通知集成层（`RegistrationServicesPathDiscovery` 等） | YourPhone 通知镜像图标缓存；注册表 `…\PushNotifications\Background`、`…\Notifications\Settings` | 解码字符串 |

传输路径结论：**所有设备 I/O 只有 MagicAAP 驱动管道一条路**（CreateFile/ReadFile/WriteFile 及
Ex/DeviceIoControl 变体）。10 个原生模块（winmm/user32/dwmapi/setupapi/bthprops/kernel32/
User32/Shcore/Gdi32/cfgmgr32）中**没有任何 ws2_32/WinRT Socket/RFCOMM API**。

## 3. AAP 包模板表（FieldRVA 静态数据 @ 0x5CED01–0x5CF1E2，原样字节）

MagicPods 的发送包全部是静态模板 + 运行时补丁值字节。模板全集（`04 00 04 00` 信封 + `09 00`
控制命令 + ident + 4 字节数据占位）：

| 模板字节 | 解读 | 与我们 AAP.h 的关系 |
|---|---|---|
| `04 00 04 00 09 00 01 00 00 00 00` | cmd 0x01 | — |
| `04 00 04 00 09 00 0C 00 00 00 00` | cmd 0x0C | — |
| `04 00 04 00 09 00 0D 00 00 00 00` | cmd 0x0D 降噪/聆听模式 | ✅ BuildNoiseControlPacket(0x0D) 一致 |
| `04 00 04 00 09 00 16 00 00 00 00` | cmd 0x16 | — |
| `04 00 04 00 09 00 17 00 00 00 00` | cmd 0x17 头部追踪 | ✅ Start/StopHeadTracking(0x17) 一致 |
| `04 00 04 00 09 00 18 00 00 00 00` | cmd 0x18 | — |
| `04 00 04 00 09 00 1A 00 00 00 00` | cmd 0x1A 重命名 | ✅ §4 一致 |
| `04 00 04 00 09 00 1B 00 00 00 00` | cmd 0x1B 单耳 ANC（OneBudANCMode） | ✅ 附录 airpods_packets.h 一致（数据占位 00，运行时补 01/02） |
| `04 00 04 00 09 00 1C 00 00 00 00` | cmd 0x1C | — |
| `04 00 04 00 09 00 1F 00 50 00 00 09 44 45 56 49 43 45` | cmd 0x1F，数据含恒定 `50`，尾随长度 09 + "DEVICE" | ✅ 印证 §7 ①「0x1F 第二字节恒 0x50」怪癖 |
| `04 00 04 00 09 00 23 00 00 00 00` | cmd 0x23 | — |
| `04 00 04 00 09 00 24 21 00 00 00` | cmd 0x24，占位数据 `21`=33 | — |
| `04 00 04 00 09 00 25 00 00 00 00 00 00` | cmd 0x25 音量滑动（VolumeSwipe） | ✅ 我方 LEGACY 注释 0x25=VolumeSwipeMode 一致 |
| `04 00 04 00 09 00 26 00 00 00 00` | cmd 0x26 个性化音量 | ✅ BuildPersonalizedVolumePacket(0x26) 一致 |
| `04 00 04 00 09 00 28 00 00 00 00` | cmd 0x28 对话感知 | ✅ BuildConversationalAwarenessPacket(0x28) 一致 |
| `04 00 01 00 02 00 00 00 00 00 00 00 00` | 握手（13B 变体，见 §5） | ⚠ 与我方 16B 握手不同 |
| `04 00 04 00 0F 00 FF FF 00 FF` | 通知订阅 0x000F | ⚠ 我方 `FF FF FF FF`，byte 8 不同 |
| `04 00 04 00 4D 00 0E 00 00 00 00 00 00 00` | Feature flags 0x004D | ⚠ byte 6 = `0E`（第三种取值，见 §5） |

数据旁证：`02 10 E8 03`（= 1000，音量 100.0% 常量）、ASCII "DEVICE"、CRC-16 查表、IMU/音频
对称滤波系数表、64B 高熵密钥块。同区还有明文 UI 字符串表（`01 0D "Bluetooth off"`、
`01 0D "Connected {0}"`、`01 17 "Headphones disconnected"`、`01 11 "Searching AirPods"`、
`01 04 "Case"/"Left"/"Right"` 等）。

## 4. ArmDot 加密字符串还原（99 条，节选）

功能参数名（`aap_*`，与模板命令一一对应的设置面）：

`aap_personalised_volume` `aap_tune_volume` `aap_one_airpod_anc` `aap_end_call`
`aap_double_tap_left` `aap_double_tap_right` `aap_microphone` `aap_conversation_awareness`
`aap_digital_crown` `aap_press_and_hold_duration` `aap_press_speed` `aap_apply_settings`
`aap_cycles_between_anc` `aap_cycles_between_anc_ext` `anc_mode` `anc_mode_ext`
`adaptive_anc_options`（部分字节经 FieldRVA 佐证修复）

设置日志（格式串证明「模板+补丁值」发送方式）：

`Set anc to {value}({hex})` `Set anc with one AirPods to {value}({hex})`
`Set conversation awareness to {value}({hex})` `Set personalised volume to {value}({hex})`
`Set press speed to {value}({hex})` `Set press and hold duration to {value}({hex})`
`Set double tap to L:{left}({lhex}) R:{right}({rhex})` `Set microphone position to {value}({hex})`
`Set tone volume to {value}({byte})` `Set end call to {value}({hex})`
`Set cycles between to {value}({hex})` `Unable find state {state}, set {option}`
`Setting {name} does not apply, due disabled headphones management` `Not implemented {name}`

枚举词表：`SinglePress` `DoublePress` `Slowest` `Shortest` `Shorter` `Default` `Off` `Anc`
`Transparency` `Adaptive` `Left` `Right` `FrontToBack` `BackToFront` `NextTrack`
`PreviousTrack` `PlayPause` `Siri`。

注册表/系统集成（非蓝牙）：`{1e87508d-89c2-42f0-8a7e-645a0f50ca58}`、
`SOFTWARE\Microsoft\Windows\CurrentVersion\PushNotifications\Background`、
`…\Notifications\Settings`、`Microsoft.YourPhone_8wekyb3d8bbwe!App`、`DisplayName`、
`CacheIcons`。此层只做通知镜像/图标缓存，与 VID spoof 无关。

## 5. 四个悬置问题的结论

1. **ATT 走驱动管道还是独立 socket/PSM 0x001F？—— MagicPods 两者都不用。**
   全程序集无 socket/RFCOMM/PSM API；无任何 ATT PDU 模板（`12 1B 00` / `12 2A 00` / `12 18 00`
   的两处命中均为查表数据非包）；大声压保护等高级功能在 MagicPods 走 **AAP 控制命令**（0x1B=
   单耳 ANC 等），不是 ATT 写。ATT 句柄写是 **LibrePods 专有路径**（其自行开 L2CAP PSM 0x001F）。
   对我方的含义：经 MagicAAP 驱动管道发 ATT PDU（`BuildLoudSoundReductionPacket` 的 0x12 0x1B 00）
   是**未验证路径**——驱动只绑定 AAP PSM 0x1001，且没有任何第三实现这么做过；LibrePods 的
   ATT 是在自己建的 socket 上发的。硬件验证仍是 P0。
2. **0x004D 标志字节 D7 还是 FF？—— 实测有第三种取值 `0E`，且 MagicPods 原样发出。**
   模板 `04 00 04 00 4D 00 0E 00 00 00 00 00 00 00`（14B，与我方同长）；工厂方法
   （0x0600407c `ExistsOpaqueLength`）仅 `newarr 14` + `InitializeArray` 原样拷贝，分发器
   （0x06004078）`calli` 直返，**全链路无补丁**。三方取值：FF（文档抓包）/ D7（LibrePods
   Android+Linux）/ 0E（MagicPods Windows）。bit 含义无源材料可依。维持 D7（两个参考实现
   一致），0E 记录为分歧点。
3. **初始化顺序。** MagicPods 模板集合印证 LibrePods 序列：握手 → 0x4D → 0x0F 订阅 → 通知流；
   且 0x4D 未发时 0x0D=0x04 回错模式的行为与此吻合。分歧：MagicPods 握手为 13B 变体
   `04 00 01 00 02 00 00×7`（= 我方 16B 去掉首 2 字节 `00 00` 与尾 3 字节零）；0x0F 订阅掩码
   `FF FF 00 FF`（我方 `FF FF FF FF`，byte 8 差异）。
4. **VendorID spoof？—— Windows 路径不存在。** 无注册表/驱动层 VID 篡改痕迹；蓝牙侧只用
   bthprops `BluetoothEnumerateInstalledServices`/`BluetoothSetServiceState` 启用远端 AAP 服务。
   VID spoof 是 LibrePods 在 Android（Xposed）/Linux（main.conf DeviceID）上的到达前提，
   Windows + MagicAAP 驱动不需要。

## 6. 与我方实现的交叉验证汇总

✅ 一致（4 个命令 id + 0x1F 的 0x50 怪癖 + 0x25 语义）：0x0D、0x17、0x1A、0x1B、0x25、0x26、0x28。
⚠ 分歧 3 处：0x4D 标志（D7 vs 0E）、0x0F 订阅掩码 byte 8、握手长度（16B vs 13B）。
❓ MagicPods 有而我方未用：cmd 0x01、0x0C、0x16、0x18、0x1C、0x23、0x24（语义未逐一定位，
   0x24 占位数据 33 疑与「Set tone volume / press speed」相关）。
❓ 我方有而 MagicPods 无模板：0x0A（EarDetectionConfig，LibrePods 口径）、0x2E（自适应音量）、
   0x38（PPE cap level）——可能经运行时构造或属 LibrePods 独有命令。

## 7. 限制与未决

- 字符串解码桩启发式（三连续 ldc / ldsflda 四元组）覆盖 99 条，个别多元素方法存在边界错配
  （如 `aap_conversation_awarenessn` 尾部噪声），语义已人工校核。
- 模板 ident ↔ 功能的对应关系，凡标 ✅ 者为三方（MagicPods 模板 + 解密字符串/LibrePods/OpenPods）
  互证；标 —/❓ 者**未**逐一打通「模板工厂→发送方法→日志字符串」的完整调用链。
- 0x4D 的 bit 语义、0x0E 与 D7/FF 的差异原因仍无源材料可依，属**推断项**。
- 0x24/0x1C 尾随常量（`02 10 E8 03` 等）的归属为推断。
- 本笔记为静态证据；「0x4D 原样发出」结论覆盖工厂与分发器链路，最终发送方法未逐指令反汇编。

## 附：工具与产物索引

| 产物 | 路径 |
|---|---|
| 元数据/字符串/模板扫描脚本 | `_research\parse_cli_heaps.ps1` `parse_cli_typedefs.ps1` `scan_call_sites.ps1` `decode_armdot_strings.ps1` `scan_fielddata.ps1` |
| TypeDef 全名表（6729 行） | `_research\magicpods_cli\typedefs.tsv` |
| 解码字符串 | `_research\magicpods_cli\decoded_strings.txt` |
| 包模板命中 | `_research\magicpods_cli\packet_templates.txt` |
| REA 证据（spill JSON） | `inspect-managed-members` × 5 类型选择，MVID `f6f82372-…`，token 身份 build-local |
