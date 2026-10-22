# AirPods AAP 协议实现级规格提取（AAP-Protocol-Spec）

- 用途：AirPodsWindows 的 AAP（Apple Accessory Communication Protocol）实现参考规格。
- 来源：`_research\librepods` v1.0.4 + `_research\OpenPods` 1.11，逐条已标注 file:line。
- 生成日期：2026-10-22。
- 声明：字节表可直接移植，行为以实测为准（控制命令表提取自 iOS 19.1 Beta）。

---

# AirPods AACP 协议实现级规格提取（LibrePods + OpenPods）

**通用包头**：`04 00 04 00 [opcode: u16 LE] [payload]`（`docs/control_commands.md:7`）。连接：L2CAP PSM 4097(0x1001)、UUID `74ec2172-0bad-4d01-8f77-997b2be0722a`（`docs/AAP Definitions.md:3`、`AirPodsService.kt:2638-2641`）；ATT socket 用 PSM 31(0x1F) 零 UUID（`AirPodsService.kt:2655-2662`）。
**连接序列**：Handshake `00 00 04 00 01 00 02 00 00×8`（`AAP Definitions.md:8-10`、`airpods_packets.h:151`、`AACPManager.kt:640-659`）→ ACK `01 00 04 00`（`airpods_packets.h:247`）→ 发 0x4D feature flags → ACK `04 00 04 00 2B 00`（`airpods_packets.h:248`）→ 发 0x0F 订阅（`linux/main.cpp:666-678`）。

## 1. 电量通知 0x0004

| offset | bytes | 含义 |
|---|---|---|
| 0-3 | `04 00 04 00` | 包头 |
| 4-5 | `04 00` | opcode 0x0004 |
| 6 | N | 组件计数 1–3 |
| 7+5i | 组件 id | `02` Right、`04` Left、`08` Case、`01` Headset/Max（仅 Linux） |
| +1 | `01` | 固定 spacer |
| +2 | 电量 | 0–100（%） |
| +3 | 状态 | `00` Unknown、`01` Charging、`02` Discharging、`04` Disconnected、`05` Optimized Charging（仅 Android） |
| +4 | `01` | 组件结束标记 |

示例（22B）：`04 00 04 00 04 00 03 | 02 01 64 02 01 | 04 01 63 01 01 | 08 01 11 02 01`。

**出处**：`docs/AAP Definitions.md:46-91`（格式 51、组件表 54-58、状态表 60-66、示例 70-91）；Android `data/Packets.kt:35-46`（BatteryComponent/BatteryStatus 含 `OPTIMIZED_CHARGING=5`）、`Packets.kt:188-217`（取 data[7]/[9]/[10]、[12]/[14]/[15]、[17]/[19]/[20]，`Packets.kt:161-173` 强制 22B）；Linux `linux/battery.hpp:45-61,71-155`（count=packet[6]，`size==7+5*count`，两个 `0x01` 校验 97-101），`linux/main.cpp:725-729` 接受 22/12B。

**分歧/风险**：① 文档示例把值 `02` 的第 8 字节标成 "Left"，与其组件表（02=Right）矛盾——按表/实现（02=Right）；② `05` Optimized Charging 仅 Android 有，文档与 Linux 缺（Linux 落 default 显示 "Invalid"，`battery.hpp:245-249`）；③ Android 只认 22B，建议按 Linux 的 count 通用解析；④ Disconnected 组件 Android 覆盖旧值、Linux 丢弃（`battery.hpp:107-110`），建议 Linux 行为；⑤ BLE 广播电量是另一种编码（bit7=charging、低7位 0-127，`BLEManager.kt:246-250`、`battery.hpp:280-285`）。

## 2. 佩戴检测 0x0006

| offset | bytes | 含义 |
|---|---|---|
| 0-3 | `04 00 04 00` | 包头 |
| 4-5 | `06 00` | opcode |
| 6 | 状态1 | 主耳：`00` In Ear、`01` Out of Ear、`02` In Case |
| 7 | 状态2 | 副耳：同上 |

**出处**：`docs/AAP Definitions.md:108-121`（格式 112；primary/secondary 语义 115："主耳摘下→麦克风切换、副耳变新主耳、包重发"；状态表 117-121）；Android `Packets.kt:90-108`（8B，`status=listOf(data[6],data[7])`）、`AirPodsService.kt:1233-1247`（`==0x00` 为在耳）；Linux `linux/eardetection.hpp:70-90`（00/01/02/其他=Disconnected）。

**分歧/风险**：Android 只分 in-ear/非 in-ear，不区分 0x02 InCase；建议按 Linux 三分＋断开兜底。primary/secondary 与左右无关（电量包内第一只为 primary，`battery.hpp:122-141`；BLE status bit5=primaryLeft/bit6=thisInCase，`BLEManager.kt:346-353`）。

## 3. 设备信息 0x001D

偏移由 `AAP Definitions.md:184` 的 222 字节示例实测（字符串变长、`00` 分隔）：

| offset | 内容 | 含义 |
|---|---|---|
| 0-3 / 4-5 | `04 00 04 00` / `1D 00` | 包头/opcode |
| 6-10 | `02 D5 00 04 00` | 未知 |
| 11 | `AirPods Pro\0` | ① 名称 |
| 23 | `A3048\0` | ② 型号 |
| 29 | `Apple Inc.\0` | ③ 厂商 |
| 40 | `QXNRHHYXP6\0` | ④ 序列号 |
| 51 | `61.1868040002000000.2713\0` | ⑤ 固件版本1 |
| 76 | （同上）\0 | ⑥ 固件版本2 |
| 101 | `1.0.0\0` | ⑦ 硬件版本 |
| 107 | `com.apple.accessory.updater.app.71\0` | ⑧ updater id |
| 142 | `HY9LT2EF26JY\0` | ⑨ 左耳序列号 |
| 155 | `H3PLWHDJ26K0\0` | ⑩ 右耳序列号 |
| 168 | `6357536\0` | ⑪ 版本3/未知数字 |
| 176 | 35B 二进制 | ⑫ 加密 blob（结构未知） |
| 212 | `539506064\0` | ⑬ 未知数字串 |

**出处**：顺序 `docs/device-info.md:7-26`、`AAP Definitions.md:158-187`；Android `AACPManager.kt:1282-1315`（切 packet[6..]、跳 NUL、丢首个垃圾串、映射 11 字段）；Linux `linux/main.cpp:543-593`（头 5B 后固定跳 6B，仅取 name/model/manufacturer）。

**分歧/风险**：① 文档写 opcode 后直接是字符串，实际有 5-6 个未知字节（建议固定从 offset 11 起或"跳到首 NUL 后丢 1 字节垃圾"）；② 文档字段顺序（⑪数字→⑫加密→⑬附加加密）与示例实测（⑪版本→⑫blob→⑬数字串）不符，以示例为准；③ 加密 blob 文档未给出结构，两实现均不解析。

## 4. 重命名 0x001A

| offset | bytes | 含义 |
|---|---|---|
| 0-3 | `04 00 04 00` | 包头 |
| 4-5 | `1A 00` | opcode **0x001A** |
| 6 | `01` | 固定 |
| 7 | len | 名称 UTF-8 长度 |
| 8 | `00` | 固定 |
| 9.. | name | UTF-8 名称 |

**出处**：`AAP Definitions.md:210-216`；实现 `AACPManager.kt:47,788-800`、Linux `airpods_packets.h:178-190`。**修复提交**：commit `4f2d11a` "android: Fix rename packet using wrong opcode (0x1E → 0x1A) (#511)"，文件 `android/app/src/main/java/me/kavishdevar/librepods/utils/AACPManager.kt`（旧路径；同 diff 把 `[size 00 name]` 改为 `[01 size 00 name]`）。

**分歧/风险**：`docs/opcodes.md:22` 仍写 0x001E（且链接的 docs/rename.md 不存在）——错的，以 0x1A 为准。Android 改名后需重新配对（`android/README.md:67`）。

## 5. 头部追踪 0x0017

Start（28B）：`04 00 04 00 17 00 | 00 00 10 00 10 00 08 A1 02 42 0B 08 0E 10 02 1A 05 01 40 9C 00 00`
Stop（28B）：`04 00 04 00 17 00 | 00 00 10 00 11 00 08 7E 10 02 42 0B 08 4E 10 02 1A 05 01 00 00 00 00`
变体（实现另有 alternate start/stop，差异在 8-13 区段：`… 0F 00 08 73/75 42 0B 08 10 10 …`，`AACPManager.kt:694-720,756-782`）。

传感器包（≥70B；识别：`04 00 04 00 17 00 00 00 10 00`，byte10=0x44/0x45，byte11=0x00）：

| offset | 长度 | 含义 |
|---|---|---|
| 43-44 | 2 | orientation 1（int16 LE） |
| 45-46 | 2 | orientation 2 |
| 47-48 | 2 | orientation 3 |
| 51-52 | 2 | 水平加速度 |
| 53-54 | 2 | 垂直加速度 |

**出处**：`AAP Definitions.md:378-405`；`AACPManager.kt:661-782,486-497`；解析 `utils/HeadOrientation.kt:44-63,91-93`，换算 `HeadOrientation.kt:78-89`（pitch=(o2+o3)/2/32000*180、yaw=(o2-o3)/2/32000*180 ⇒ 原始 ±32000 ≈ ±180°），校准 `HeadOrientation.kt:35-75`（10 样本、偏移 5500、o1Neutral=19000）；识别 `Packets.kt:248-265`。
**分歧/风险**：单位与量程文档未给出，仅能从实现反推角度换算；o1/o2/o3 的物理含义文档未给出（实现仅用 o2/o3 算 pitch/yaw）。

## 6. 按键事件 0x0019

| offset | bytes | 含义 |
|---|---|---|
| 0-3 / 4-5 | `04 00 04 00` / `19 00` | 包头/opcode |
| 6 | 事件 | `05` single、`06` double、`07` triple、`08` long |
| 7 | 侧 | `01` Left、`02` Right |

**出处**：`AACPManager.kt:51,132-146,251-264`（整包 8B，type=data[6]，bud=data[7]）、`AirPodsService.kt:1094-1108`。
**分歧/风险**：文档未给出（opcodes.md 只有条目）；byte5 未知（推测 0x00）；实现异常消息下标写错（`AACPManager.kt:260-262`），以实际取值为准。

## 7. 控制命令 0x0009

信封（固定 11B）：`04 00 04 00 | 09 00 | id | d1 | d2 | d3 d3'`（offset 0-5 头+opcode、6 identifier、7 d1、8 d2、9-10 恒 `00 00`）。出处：`control_commands.md:3-15`、`BasicControlCommand.hpp:6-19`、`AACPManager.kt:292-303,1100-1120`（解析时剥重复头、裁尾 0）。

| id | 含义 | data1（d2 见注） | 出处 |
|---|---|---|---|
| 0x0D | ListeningMode | `01` Off、`02` ANC、`03` Transparency、`04` Adaptive | `AAP Definitions.md:93-106,191-208`；`airpods_packets.h:18-22` |
| 0x0A | EarDetectionConfig | `01` 开、`02` 关 | `control_commands.md:81-83`；`AirPodsViewModel.kt:712` |
| 0x1A | ListeningModeConfigs | 位掩码：01=Off、02=ANC、04=Transparency、08=Adaptive（`0F`/`07`/`0B`/`0D`… 为组合） | `control_commands.md:107-109`；`AAP Definitions.md:308-376`；`AirPodsViewModel.kt:747-766`（至少保留 2 模式） |
| 0x1F | Chime Volume | 0–100；Android 发 `[vol, 0x50]` 两字节 | `control_commands.md:126-128`；`AccessibilitySettingsScreen.kt:290-293` |
| 0x26 | Adaptive Volume | `01` 开、`02` 关 | `control_commands.md:145-147`；`airpods_packets.h:79-87` |
| 0x28 | Conversation Detect | `01` 开、`02` 关 | `control_commands.md:152-154`；`AAP Definitions.md:233-246` |
| 0x2E | AutoANC Strength | 0–100（0=放最多噪音，100=滤更多）；UI 发 `100-slider` | `AAP Definitions.md:248-262`；`airpods_packets.h:167-176`；`AdaptiveStrengthScreen.kt:93-110` |
| 0x30 | HRM | 单字节，值域文档未给出（demo `01`） | `control_commands.md:170-171` |
| 0x31 | In Case Tone config | `01` 开、`02` 关 | `control_commands.md:173-175` |
| 0x40 | In Case Tone Volume | 0–100（仅文档，实现未实现） | `control_commands.md:225-227` |
| 0x35 | Sleep Detection | `01` 开、`02` 关 | `control_commands.md:188-190` |
| 0x2C | Hearing Aid | d1=enrolled、d2=enabled：`01 01` 开；任一 `02` 关 | `control_commands.md:159-161`；`airpods_packets.h:110-136`；`HearingAidScreen.kt:119,211-217` |
| 0x33 | Hearing Assist | `01` 开、`02` 关 | `control_commands.md:180-182`；`airpods_packets.h:100-108` |
| 0x37 | PPE Toggle | `01` 开、`02` 关（文档未给值） | `control_commands.md:196-197`；`HearingProtectionScreen.kt:102-115` |
| 0x38 | PPE Cap Level | 单字节，值域文档未给出（demo `52`） | `control_commands.md:199-200` |

**分歧/风险**：① 0x1F 文档 1 字节、Android 实发 2 字节（第二字节恒 `0x50`，含义未知）——照抄两字节；② 0x40 仅文档；另有 ATT 版盒提示音开关 `12 3A 00 01 00 08 [setting]`，`00`=On、`01`=Off（**取值反向**，`AAP Definitions.md:218-231`），与 0x31 的关系文档未给出；③ 0x30/0x38 值域文档未给出。

## 8. Feature flags 0x004D

| offset | bytes | 含义 |
|---|---|---|
| 0-3 / 4-5 | `04 00 04 00` / `4D 00` | 包头/opcode |
| 6 | `D7`（实现）/`FF`（文档抓包）/`0E`（MagicPods 静态模板） | 功能位掩码 |
| 7-13 | `00 ×7` | 保留 |

**出处**：`AAP Definitions.md:21-27`（抓包 `…4d 00 ff 00×7`，称其开启"播放音频时的对话感知"+Adaptive Transparency）；Android `AACPManager.kt:634-638`（D7）；Linux `airpods_packets.h:152`（D7），握手 ACK 后发送（`linux/main.cpp:666-669`）。
**分歧/风险**：文档 FF vs 实现 D7，差异位 0x28（bit3/0x08、bit5/0x20）。**各位语义源材料均未说明（文档未给出）**；只能推断 D7 已覆盖文档所述两项功能，0x08/0x20 为未知（或 Apple 专属）功能。移植用 D7，必要时再试 FF。另注：不发此包时 0x0D=0x04 会被回以别的模式（`AAP Definitions.md:208`）。
**第三实现（MagicPods 逆向）**：模板 `04 00 04 00 4D 00 0E 00 00 00 00 00 00 00`（FieldRVA @ 0x5CF1CB，14B 同长），工厂仅 `newarr 14`+`InitializeArray` 原样拷贝、分发器 `calli` 直返，**全链路无补丁**——byte 6 实为 `0E`，与 D7/FF 均不同（bit 语义仍无源材料，维持 D7；逆向细节见维护者内部笔记，未入库）。
**实现取值并存（2026-10-22 横评补证）**：活跃开源实现中 **D7**（LibrePods Android+Linux+Windows 移植 `AacpPackets.cs:14-15`）与 **FF**（PodBridge `AapProtocol.cs:47-61` `4D 00 FF 00…`、文档抓包）并存且各自可用 → 推断固件对掩码宽容；`0E` 仅见 MagicPods 一家。

## 9. 通知订阅 0x000F

| offset | bytes | 含义 |
|---|---|---|
| 0-3 / 4-5 | `04 00 04 00` / `0F 00` | 包头/opcode |
| 6-9 | `FF FF FF FF` | 4 字节掩码（逐字节语义文档未给出；第 3 字节（offset 8）与佩戴检测相关：FF=开、FD=关耳检；文档抓包另有 `FF FF FE FF`） |
| 10 | `FF`（仅 Linux） | 见分歧 |

**出处**：`AAP Definitions.md:29-42`；`AACPManager.kt:622-628`（注释 625-626：第三字节 FD=禁用耳检、可随时发送）；`airpods_packets.h:153`（`040004000f00ffffffffff`）；时机 `linux/main.cpp:670-678`。
**分歧/风险**：Linux 发 5×FF（11B）、文档/Android 4×FF（10B）——建议 4×FF。
**实现变体（2026-10-22 横评）**：PodBridge 实发 `0F 00 FF FF FE FF`（`NoiseControlController.cs:99-104`，与文档抓包 FE 变体一致）；MagicPods 模板 `0F 00 FF FF 00 FF`（FieldRVA @ 0x5CF16D，无补丁痕迹）；librepods-windows 与本项目同为 4×FF（`AacpPackets.cs:17-18`）。四实现三种掩码，固件均接受 → 掩码宽容，维持 4×FF。

## 10. 音频源 0x000D / 0x000E

0x000D（Host→Accessory，请求）：载荷文档未给出，实现不发送。0x000E（Accessory→Host，响应）：

| offset | bytes | 含义 |
|---|---|---|
| 0-3 / 4-5 | `04 00 04 00` / `0E 00` | 包头/opcode |
| 6-11 | MAC 6B | 音频源 MAC（大端，显示反转） |
| 12 | type | `00` NONE、`01` CALL、`02` MEDIA |

**出处**：`docs/opcodes.md:12-13`；`AACPManager.kt:54,148-154,1208-1222`；用途 `AirPodsService.kt:1111-1130`（音频源是他机则让出 owns）。
**分歧/风险**：0x000D 载荷文档未给出/未实现；0x0C、0x14 定义未用（`AACPManager.kt:56-59`）。

## 11. 对话感知电平通知 0x004B

| offset | bytes | 含义 |
|---|---|---|
| 0-3 / 4-5 | `04 00 04 00` / `4B 00` | 包头/opcode |
| 6-8 | `02 00 01` | 固定 |
| 9 | level | `01`/`02`=开始说话（大幅降音量）、`03`=停止（恢复）、中间值=中间音量、`08`/`09`=正常音量 |

整包 10B。**出处**：`AAP Definitions.md:123-136`；`Packets.kt:226-245`（status=data[9]）；`airpods_packets.h:96`；行为 `AirPodsService.kt:907-924`（1/2→startSpeaking、6/8/9→stopSpeaking）。
**分歧/风险**：实现把 `06` 也当恢复、文档只写 08/09——建议 1/2=降、3/6/8/9=恢复。CA 开关状态回包 `04 00 04 00 09 00 28 [status] 00 00 00`（01 开/02 关，仅订阅后发一次，`AAP Definitions.md:137-156`）。

## 12. ATT/GATT（0x18 / 0x1B / 0x2A）

PDU：读 `0A [handle] 00`→`0B [value…]`；写 `12 [handle] 00 [value…]`→`13`；通知 `1B [handle_lo] 00 [value…]`（value 从 offset 3）；CCCD 写 `12 [handle+1] 00 01`（`ATTManager.kt:29-39,88-139,173-192`）。Handles：0x18 透明度、0x1B 大声压保护、0x2A 助听；CCCD=handle+1（0x19、0x2B）；**0x1B 的 CCCD(0x1C) 注释 "doesn't work"**（`ATTManager.kt:37`）。

透明度写 `12 18 00 [floats]`（全 LE float）：enabled(0/1)@0、左 EQ×8(0-100)@4-35、左 Amp(0-2)@36、左 Tone(0-2)@40、左 ConvBoost(0/1)@44、左 Ambient(0-1)@48、右同构@52-99、ownVoice（可选，104B）@100。文档一致（`AAP Definitions.md:279-305`），并要求随后发 0x53 Headphone Accommodation（140B、offset6=0x84、offset10/11=Phone/Media、EQ 4×8 LE float 从 12 起，`AACPManager.kt:561-596`）。
助听写 `12 2A 00 [104B]`：头 `02 02 60 00` + 左 EQ×8@4-35 + 左 Amp/Tone/Conv/Ambient@36/40/44/48 + 右 EQ×8@52-83 + 右四项@84/88/92/96 + ownVoice@100；写前把 offset2 改 `64`（`HearingAid.kt:88-187`）。
大声压写 `12 1B 00 [01/00]`（01=开、00=关，**非** 01/02 惯例；`AccessibilitySettingsScreen.kt:264-268`、`HearingProtectionScreen.kt:91-95`）。
**到达前提（VendorID spoof）**：本机 DID VendorID=Apple `0x004C`、source=0x0001。Linux `/etc/bluetooth/main.conf` `DeviceID = bluetooth:004C:0000:0000`（`README.md:107-111`）；Android Xposed `fake_BTA_DmSetLocalDiRecord`（`l2c_fcr_hook.cpp:58-79`），ATT socket 仅在 hook 开启时建（`AirPodsService.kt:2655-2670`）。README ⚪ 项=需 spoof（`README.md:42-59`）。
**MagicPods 逆向补充（Windows 第三实现）**：全程序集无任何 ATT PDU 模板、无独立 socket/RFCOMM API，仅经 MagicAAP 驱动管道收发 AAP 帧；VendorID spoof 在 Windows 路径不存在（bthprops 仅用于启用远端 AAP 服务）。即 ATT 是 LibrePods 自建 PSM 0x001F socket 的专有路径，**本项目经驱动管道发 `12 1B 00` 属未验证路径**（驱动仅绑 AAP PSM 0x1001）；逆向细节见维护者内部笔记（未入库）。
本仓库曾有第二驱动客户端实现 `MagicAAPClient.{h,cpp}`（CreateFile/ReadFile 直连设备接口，355 行，从未进 CMake 源列表、从未编译），v1.3.1 已删除；现行实现为 `MagicAAPWinRT.{h,cpp}`。
**分歧/风险**：0x1B CCCD 坏（只能写后读）；文档透明度表 "Ambient Noise Reduction" 列了两遍（`AAP Definitions.md:294-303`），第二行疑为 ownVoice/笔误（文档未给出）；助听 0x60→0x64 原因未解释（照抄）；0x1B 用 00/01 易错。

## 13. 多设备/智能路由

0x0010（发）：`04 00 04 00 10 00` + 6B 目标 MAC（**反序**）+ 2B 长度（`6C/62/82/7E/59/52 00`）+ `01 E5 …`（ShowUI 为 `01 E6 5B`）+ ASCII key + 单字节类型标记（`42/46/47/48/49/4A/50/51/52/56/58/59/5F`）。5 个变体（`AACPManager.kt:824-1076`）：MediaInformation/NewDevice（`playingApp`、`hostStreamingState`、`btAddress`、`btName`、`otherDevice`+`AudioCategory`）；**HijackRequest（106B，`localscore`、`reason="Hijackv2"`、`audioRoutingScore`、`audioRoutingSetOwnershipToFalse`、`remotescore`）——`sendHijackRequest(selfMac)` 对每个非本机已连接设备发一包，请求把音频/控制权抢回来**；ShowUI（`SmartRoutingKeyShowNearbyUI`）；HijackReversed（`audioRoutingShowReverseUI`、`reason="ReverseBannerTapped"`）；AddTiPiDevice（`newTipi`、`nearbyAudioScore`）。
0x0011（收）：offset6-11 发起方 MAC（反序）+ 文本载荷（含 `btName`、`SetOwnershipToFalse`、`ShowNearbyUI`、`ReverseBannerTapped`），用于识别设备类型与让权请求（`AACPManager.kt:523-559`）。
0x002D 请求字节文档未给出、实现不发送；0x002E（收）：count@8、条目@9+8i（6B MAC **不反转** + info1/info2，语义未知，`AACPManager.kt:1224-1257`）。抢权流程：0x06=01 → MediaInformation → ShowUI → HijackRequest（`AirPodsService.kt:2493-2507`），且要求 vendor_id_hook（`AirPodsService.kt:2474-2478`）。
**分歧/风险**：需 VendorID spoof；MAC 正/反序在 0x10 发（反）、0x2E 收（正）、0x11 收（反）不一致；info1/info2 与 0x2D 均文档未给出；README 称最多 2 台（`README.md:48,113-115`）。

## 14. 型号能力表

来源 `data/AirPods.kt`；**BLE model id 不在 AirPods.kt（文档未给出）**，取自 `BLEManager.kt:88-98` / `linux/ble/blemanager.cpp:11-22` / OP `PodsStatus.java:76-104`。

| 名称 | 硬件型号 | BLE id | 能力 |
|---|---|---|---|
| AirPods 1 | A1523, A1722 | 0x0220 | 无 |
| AirPods 2 | A2032, A2031 | 0x0F20 | 无 |
| AirPods 3 | A2565, A2564 | 0x1320 | HEAD_GESTURES |
| AirPods 4 | A3053/A3050/A3054 | 0x1920 | HEAD_GESTURES, SLEEP_DETECTION, ADAPTIVE_VOLUME |
| AirPods 4 (ANC) | A3056/A3055/A3057 | 0x1B20 | +LISTENING_MODE, CONVERSATION_AWARENESS, ADAPTIVE_AUDIO, STEM_CONFIG |
| AirPods 5 | A3531/A3532/A3533 | 文档/表未给出 | 同 4ANC |
| AirPods 5 WCC | A3439/A3440/A3441 | 文档/表未给出 | 同上 +SWIPE_FOR_VOLUME |
| AirPods Pro 1 | A2084, A2083 | 0x0E20 | LISTENING_MODE |
| AirPods Pro 2 Lightning | A2931/A2699/A2698 | 0x1420 | +CONVERSATION_AWARENESS, STEM_CONFIG, LOUD_SOUND_REDUCTION, SLEEP_DETECTION, HEARING_AID, ADAPTIVE_AUDIO, ADAPTIVE_VOLUME, SWIPE_FOR_VOLUME, HEAD_GESTURES |
| AirPods Pro 2 USB-C | A3047/A3048/A3049 | 0x2420 | 同 Pro2 Lightning |
| AirPods Pro 3 | A3063/A3064/A3065 | 0x2720（仅 OP） | Pro2 全部 +PPE +HRM |
| AirPods Max（enums.h:51-52） | A2096 / A3184 | 0x0A20 / 0x1F20 | — |

BLE id 位置：manufacturer data(0x004C) 的 `data[3]<<8|data[4]`（`BLEManager.kt:337`）。能力枚举：`AirPods.kt:35-48`。

## 15. 固件版本相关行为

| 行为 | 出处 |
|---|---|
| 协议定义基于 APP2 USB-C 固件 7A305 | `AAP Definitions.md:1` |
| 0x2E 自适应音频噪音是 7A305 新功能 | `AAP Definitions.md:252` |
| 唤起数字助理、睡眠检测暂停媒体"需要较新固件" | `README.md:50` |
| 新固件后必须先启用 "Off listening mode"(0x34) 才能切到 Off | `android/README.md:65` |
| Adaptive Transparency(0x0D=0x04) 须先发 0x4D，否则回别的模式 | `AAP Definitions.md:27,208` |
| 0x001D 无法请求，仅连接时推送 | `device-info.md:4` |
| 0x004F 信息请求即使用 Apple DID 也不工作 | `opcodes.md:32` |
| 长按配置包按状态组合发送、会被新 Apple 设备覆盖，每次连接需重写 | `AAP Definitions.md:308-312` |

---

# 附录：linux/airpods_packets.h 全部包常量（可直接移植）

前置（`BasicControlCommand.hpp`）：`ControlCommand::HEADER = 04 00 04 00 09 00`；`createCommand(id,d1..d4)=HEADER+id+d1 d2 d3 d4`（11B）。

| 名称 | 字节 |
|---|---|
| NoiseControl::HEADER | `04 00 04 00 09 00 0D` |
| NoiseControl::OFF / NOISE_CANCELLATION / TRANSPARENCY / ADAPTIVE | `…0D 01 00 00 00` / `…0D 02 00 00 00` / `…0D 03 00 00 00` / `…0D 04 00 00 00` |
| OneBudANCMode(0x1B)::HEADER / ENABLED / DISABLED | `04 00 04 00 09 00 1B` / `…1B 01 00 00 00` / `…1B 02 00 00 00` |
| VolumeSwipe(0x25)::HEADER / ENABLED / DISABLED | `…25` / `…25 01 00 00 00` / `…25 02 00 00 00` |
| VolumeSwipe::getIntervalPacket(i) | `04 00 04 00 09 00 23 [i] 00 00 00` |
| AdaptiveVolume(0x26)::HEADER / ENABLED / DISABLED | `…26` / `…26 01 00 00 00` / `…26 02 00 00 00` |
| ConversationalAwareness(0x28)::HEADER / ENABLED / DISABLED | `…28` / `…28 01 00 00 00` / `…28 02 00 00 00` |
| ConversationalAwareness::DATA_HEADER | `04 00 04 00 4B 00 02 00 01` |
| HearingAssist(0x33)::HEADER / ENABLED / DISABLED | `…33` / `…33 01 00 00 00` / `…33 02 00 00 00` |
| HearingAid(0x2C)::HEADER / ENABLED / DISABLED | `…2C` / `…2C 01 01 00 00` / `…2C 02 02 00 00` |
| AllowOffOption(0x34)::HEADER / ENABLED / DISABLED | `…34` / `…34 01 00 00 00` / `…34 02 00 00 00` |
| Connection::HANDSHAKE | `00 00 04 00 01 00 02 00 00 00 00 00 00 00 00 00`（16B） |
| Connection::SET_SPECIFIC_FEATURES | `04 00 04 00 4D 00 D7 00 00 00 00 00 00 00`（14B） |
| Connection::REQUEST_NOTIFICATIONS | `04 00 04 00 0F 00 FF FF FF FF FF`（11B，5×FF，与 Android/文档 4×FF 不同） |
| Connection::AIRPODS_DISCONNECTED | `00 01 00 00` |
| Phone::NOTIFICATION / CONNECTED / DISCONNECTED | `00 04 00 01` / `00 01 00 01` / `00 01 00 00` |
| Phone::STATUS_REQUEST / DISCONNECT_REQUEST | `00 02 00 03` / `00 02 00 00` |
| AdaptiveNoise::HEADER / getPacket(level) | `04 00 04 00 09 00 2E` / `HEADER+[level]+00 00 00` |
| Rename::getPacket(name) | `04 00 04 00 1A 00 01 [len] 00 [name]` |
| MagicPairing::REQUEST_MAGIC_CLOUD_KEYS | `04 00 04 00 30 00 05 00`（8B） |
| MagicPairing::MAGIC_CLOUD_KEYS_HEADER | `04 00 04 00 31 00 02`（7B）；响应≥47B：`…|01 00 10 xx [16B IRK]|04 00 10 xx [16B EncKey]`（len 2B 大端+1 保留） |
| Parse::EAR_DETECTION / BATTERY_STATUS / METADATA | `04 00 04 00 06 00` / `04 00 04 00 04 00` / `04 00 04 00 1D` |
| Parse::HANDSHAKE_ACK / FEATURES_ACK | `01 00 04 00` / `04 00 04 00 2B 00` |

（行号：NoiseControl 15-51、OneBudANC 53-61、VolumeSwipe 63-77、AdaptiveVolume 79-87、CA 89-98、HearingAssist 100-108、HearingAid 110-136、AllowOffOption 138-146、Connection 148-155、Phone 157-165、AdaptiveNoise 167-176、Rename 178-190、MagicPairing 192-239、Parse 241-249。）
补充 Android 独有：`0x29 00 00 FF×7`（"enables setting EQ"，`AACPManager.kt:1259-1270`）、Custom EQ 0x63（`[len:2 LE][?][state][low][mid][high]`，`AACPManager.kt:1321-1349`）。
