<h1 align="center">
    <a href="https://github.com/YimingZhanshen/AirPodsWindows"><img src="/Source/Resource/Image/Icon.svg" alt="Icon" width="128"></a>
    <br>
    AirPodsDesktop
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

## 🔍 预览
![Preview Image](/Assets/Preview.gif)

## ✨ 特性

### 基础功能
* 🔋 **电池信息显示** - 实时显示左/右耳机和充电盒电量
* 👂 **自动人耳检测** - 取下耳机自动暂停，戴上自动恢复
* 🚀 **低音频延迟模式** - 修复短音频播放问题
* 🌈 **精美动画** - 优雅的用户界面体验

### ANC 高级功能（需要 MagicAAP 驱动）
* 🎧 **降噪控制** - 支持关闭/降噪/通透/自适应四种模式切换
* 🗣️ **对话感知** - 说话时自动降低媒体音量（AirPods Pro/Max）
* 🔊 **个性化音量** - 根据环境和聆听习惯自动调整音量
* 👂 **入耳检测状态** - 实时检测左右耳机佩戴状态
* 🔇 **响亮声音降低** - 保护听力，自动降低过响的声音
* 📊 **自适应通透级别** - 精细控制环境声音透过量（0-50）

## ⚙️ 系统要求

- **操作系统**: Windows 10/11
- **蓝牙**: 需要蓝牙 4.0+ 适配器
- **AirPods**: 支持所有 AirPods 型号
- **ANC 功能**: 需要安装 [MagicAAP 驱动](https://github.com/kavishdevar/librepods)（仅 AirPods Pro/Max/AirPods 4 ANC）

## 📦 安装

1. 从 [Releases](https://github.com/YimingZhanshen/AirPodsWindows/releases) 下载最新版本
2. 运行安装程序或解压便携版
3. （可选）安装 MagicAAP 驱动以启用 ANC 控制功能

## 🛠️ 构建
查看 [构建说明](/Docs/Build.md)。

## 🤝 贡献
*AirPodsDesktop* 是一个开源项目，您可以通过以下方式贡献：
* [打开问题](https://github.com/YimingZhanshen/AirPodsWindows/issues/new/choose) 来报告错误或建议新功能。
* [提交 PR](https://github.com/YimingZhanshen/AirPodsWindows/compare) 来修复已知 BUG 或尝试 TODO 列表中的事项。
* [翻译到其他语言](/CONTRIBUTING.md#-translation-guide) 或 [改进现有的翻译](/CONTRIBUTING.md#-translation-guide)。

## 💎 第三方库
* [Qt 5.15.2](https://www.qt.io/download-qt-installer) ([LGPLv3 License](https://doc.qt.io/qt-5/lgpl.html))
* [spdlog](https://github.com/gabime/spdlog) ([MIT License](https://github.com/gabime/spdlog/blob/v1.x/LICENSE))
* [cxxopts](https://github.com/jarro2783/cxxopts) ([MIT License](https://github.com/jarro2783/cxxopts/blob/master/LICENSE))
* [cpr](https://github.com/whoshuu/cpr) ([MIT License](https://github.com/whoshuu/cpr/blob/master/LICENSE))
* [json](https://github.com/nlohmann/json) ([MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT))
* [SingleApplication](https://github.com/itay-grudev/SingleApplication) ([MIT License](https://github.com/itay-grudev/SingleApplication/blob/master/LICENSE))
* [pfr](https://github.com/boostorg/pfr) ([BSL-1.0 License](https://github.com/boostorg/pfr/blob/develop/LICENSE_1_0.txt))
* [magic_enum](https://github.com/Neargye/magic_enum) ([MIT License](https://github.com/Neargye/magic_enum/blob/master/LICENSE))
* [stacktrace](https://github.com/boostorg/stacktrace) ([BSL-1.0 License](https://www.boost.org/LICENSE_1_0.txt))

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
