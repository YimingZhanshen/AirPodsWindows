<h1 align="center">
    <a href="https://github.com/YimingZhanshen/AirPodsWindows"><img src="/Source/Resource/Image/Icon.svg" alt="Icon" width="128"></a>
    <br>
    AirPodsWindows
</h1>
<p align="center">AirPods 桌面用户体验增强程序 - Windows 平台完整 ANC 控制支持</p>
<p align="center">
    <a href="https://github.com/YimingZhanshen/AirPodsWindows/actions/workflows/windows.yml">
        <img src="https://github.com/YimingZhanshen/AirPodsWindows/actions/workflows/windows.yml/badge.svg"/>
    </a>
    <a href="https://github.com/YimingZhanshen/AirPodsWindows/releases">
        <img src="https://img.shields.io/github/v/release/YimingZhanshen/AirPodsWindows?include_prereleases"/>
    </a>
    <a href="https://github.com/YimingZhanshen/AirPodsWindows/releases">
        <img src="https://img.shields.io/github/downloads/YimingZhanshen/AirPodsWindows/total.svg"/>
    </a>
    <a href="https://github.com/YimingZhanshen/AirPodsWindows/compare">
        <img src="https://img.shields.io/badge/PRs-welcome-brightgreen.svg"/>
    </a>
    <a href="/LICENSE">
        <img src="https://img.shields.io/badge/license-GPLv3-yellow.svg"/>
    </a>
</p>
<p align="center">🌎 <a href="/README.md">English</a> | 🌏 简体中文 | 🌏 <a href="/README-TW.md">繁體中文</a></p>

> [!IMPORTANT]
> **这是 Fork，不是原项目 AirPodsDesktop。**
> AirPodsWindows 完整继承上游功能，并加上原项目没有的部分：通过 [MagicAAP 驱动](https://magicpods.app/magicaap/)
> 实现的 **Windows 平台完整 ANC 控制** —— 降噪模式切换、对话感知（含说话音量降低滑条）、个性化音量、
> 响亮声音降低、自适应通透级别，以及托盘降噪菜单。
> 想要原版？→ [SpriteOvO/AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop)

## 🆚 AirPodsWindows 与 AirPodsDesktop 的区别

| | [SpriteOvO/AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop)（上游） | **AirPodsWindows（本仓库）** |
| --- | --- | --- |
| 电池 / 入耳检测 / 弹窗动画 / 浅色深色主题 | ✅ | ✅ |
| **Windows 降噪控制** —— 关闭 / 降噪 / 通透 / 自适应 | ❌ | ✅（需 MagicAAP 驱动） |
| 对话感知 **+ 说话音量降低滑条** | ❌ | ✅ |
| 个性化音量 · 响亮声音降低 · 自适应通透级别 | ❌ | ✅ |
| 托盘降噪菜单 | ❌ | ✅ |
| 发布与程序内更新指向 | `SpriteOvO/AirPodsDesktop` | **`YimingZhanshen/AirPodsWindows`** |
| 本构建的 Issue 跟踪 | 上游仓库 | 本仓库 |
| 技术基线 | 0.6.1 起 Qt 6.8.3、仅 x64 | 与上游保持同步 |

* 仅 ANC 相关功能需要 [MagicAAP 驱动](https://magicpods.app/magicaap/)，其余功能无需驱动即可使用。
* 在原项目上也能复现的问题更适合提到上游仓库；本仓库跟踪 Windows ANC 功能与本项目的其它差异。

## 🔍 预览

| 浅色主题 | 深色主题 |
| :---: | :---: |
| <img src="/Assets/Preview-Light.gif" alt="浅色主题弹窗动画" width="360"> | <img src="/Assets/Preview-Dark.gif" alt="深色主题弹窗动画" width="360"> |

## ✨ 特性

* 🔋 通知区域电池信息。
* 👂 自动入耳检测与媒体播放控制。
* 🚀 低延迟音频模式。
* 🌈 设备弹出窗口动画，以及浅色、深色或系统主题。

### ANC 高级功能（需要 MagicAAP 驱动）

* 🎧 降噪控制 - 在关闭/降噪/通透/自适应之间切换。
* 🗣️ 对话感知 - 说话时自动降低媒体音量（AirPods Pro/Max），并可调节说话时的音量降低幅度。
* 🔊 个性化音量 - 根据环境和聆听习惯自动调整音量。
* 👂 入耳检测状态 - 实时检测左右耳机佩戴状态。
* 🔇 响亮声音降低 - 降低过响的声音以保护听力。
* 📊 自适应通透级别 - 精细控制环境声音透过量（0-50）。

## 💻 系统要求

正式版本支持 Windows 10 1809 以上或 Windows 11 x64，不支持 32 位 Windows 与 ARM64。
现有 Qt 5／Win32 用户可直接使用程序内更新器迁移至 Qt 6／x64，现有用户设置会保留。

ANC 控制另外需要 [MagicAAP 驱动](https://magicpods.app/magicaap/)，且仅适用于 AirPods Pro / Max / AirPods 4（ANC）。

## 📦 安装

1. 从 [Releases](https://github.com/YimingZhanshen/AirPodsWindows/releases) 下载最新版本
2. 运行安装程序或解压便携版
3. （可选）安装 MagicAAP 驱动以启用 ANC 控制功能 —— [完整安装步骤见 Docs/Driver.md](/Docs/Driver.md)

## 🛠️ 构建与测试

Windows 环境要求、构建命令与测试方式请参阅[构建说明](/Docs/Build.md)。

## 🤝 贡献

*AirPodsWindows* 是一个开源项目，您可以通过以下方式贡献：

* [打开问题](https://github.com/YimingZhanshen/AirPodsWindows/issues/new/choose) 来报告错误或建议新功能。
* [提交 PR](https://github.com/YimingZhanshen/AirPodsWindows/compare) 来修复错误、改进文档或添加功能。
* [翻译到其他语言](/CONTRIBUTING.md#-translation-guide) 或 [改进现有的翻译](/CONTRIBUTING.md#-translation-guide)。

更新项目文档时，请保持 `README.md`、`README-CN.md` 与 `README-TW.md` 的章节顺序、功能、命令、链接及致谢内容一致。

## 💎 第三方依赖项

* [Qt 6.8.3](https://www.qt.io/download-qt-installer) ([LGPLv3 License](https://doc.qt.io/qt-6/lgpl.html))
* [spdlog](https://github.com/gabime/spdlog) ([MIT License](https://github.com/gabime/spdlog/blob/v1.x/LICENSE))
* [cxxopts](https://github.com/jarro2783/cxxopts) ([MIT License](https://github.com/jarro2783/cxxopts/blob/master/LICENSE))
* [cpr](https://github.com/whoshuu/cpr) ([MIT License](https://github.com/whoshuu/cpr/blob/master/LICENSE))
* [json](https://github.com/nlohmann/json) ([MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT))
* [SingleApplication](https://github.com/itay-grudev/SingleApplication) ([MIT License](https://github.com/itay-grudev/SingleApplication/blob/master/LICENSE))
* [pfr](https://github.com/boostorg/pfr) ([BSL-1.0 License](https://github.com/boostorg/pfr/blob/develop/LICENSE_1_0.txt))
* [magic_enum](https://github.com/Neargye/magic_enum) ([MIT License](https://github.com/Neargye/magic_enum/blob/master/LICENSE))
* [stacktrace](https://github.com/boostorg/stacktrace) ([BSL-1.0 License](https://www.boost.org/LICENSE_1_0.txt))
* [Inter](/Source/Resource/Font/Inter/LICENSE.txt) (SIL Open Font License 1.1)
* [Noto Sans TC](/Source/Resource/Font/NotoSansTC/LICENSE.txt) (SIL Open Font License 1.1)

## 🍺 致谢与参考项目

### 核心参考
* [librepods](https://github.com/kavishdevar/librepods) - AAP 协议逆向工程与文档，MagicAAP 驱动（本项目 ANC 功能实现的核心参考）
* [OpenPods](https://github.com/adolfintel/OpenPods) - AirPods BLE 广播协议解析

### 协议文档
* [AAP Definitions](https://github.com/kavishdevar/librepods/blob/main/AAP%20Definitions.md) - Apple Accessory Protocol 详细定义
* [Discontinued Privacy: Personal Data Leaks in Apple Bluetooth-Low-Energy Continuity Protocols](https://hal.inria.fr/hal-02394619/document) - Apple BLE 协议研究论文

### 相关项目
* [MagicPods](https://magicpods.app/) - 商业 AirPods Windows 应用
* [AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop) - 原始项目（本项目 Fork 自此）

## 📜 许可证
本项目采用 [GPLv3 许可证](/LICENSE)。

## 👤 作者
* **YimingZhanshen** - Fork 维护者，Windows ANC 功能开发
* **SpriteOvO** - 原始项目作者

---
<p align="center">如果这个项目对您有帮助，请考虑给个 ⭐ Star！</p>
