<h1 align="center">
    <a href="https://github.com/YimingZhanshen/AirPodsWindows"><img src="/Source/Resource/Image/Icon.svg" alt="Icon" width="128"></a>
    <br>
    AirPodsDesktop
</h1>
<p align="center">AirPods desktop user experience enhancement program - Full ANC control on Windows</p>
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
<p align="center">🌎 English | 🌏 <a href="/README-CN.md">简体中文</a> | 🌏 <a href="/README-TW.md">繁體中文</a></p>

## 🔍 Preview
![Preview Image](/Assets/Preview.gif)

## ✨ Features

### Basic Features
* 🔋 **Battery information display** - Real-time battery level for left/right earbuds and case
* 👂 **Automatic ear detection** - Auto pause when removed, auto resume when put back
* 🚀 **Low audio latency mode** - Fixes short audio playback issues
* 🌈 **Beautiful animation** - Elegant user interface experience

### Advanced ANC Features (requires MagicAAP driver)
* 🎧 **Noise Control** - Switch between Off/Noise Cancellation/Transparency/Adaptive modes
* 🗣️ **Conversational Awareness** - Automatically lower media volume when speaking (AirPods Pro/Max)
* 🔊 **Personalized Volume** - Adjust volume based on environment and listening habits
* 👂 **Ear Detection Status** - Real-time detection of left/right earbud wearing status
* 🔇 **Loud Sound Reduction** - Protect hearing by reducing loud sounds
* 📊 **Adaptive Transparency Level** - Fine control of environmental sound pass-through (0-50)

## ⚙️ System Requirements

- **OS**: Windows 10/11
- **Bluetooth**: Requires Bluetooth 4.0+ adapter
- **AirPods**: All AirPods models supported
- **ANC Features**: Requires [MagicAAP driver](https://github.com/kavishdevar/librepods) (AirPods Pro/Max/AirPods 4 ANC only)

## 📦 Installation

1. Download the latest version from [Releases](https://github.com/YimingZhanshen/AirPodsWindows/releases)
2. Run the installer or extract the portable version
3. (Optional) Install MagicAAP driver to enable ANC control features

## 🛠️ Build
See the [Build Instructions](/Docs/Build.md).

## 🤝 Contribute
*AirPodsDesktop* is an open source project, here are some ways you can contribute:
* [Open an issue](https://github.com/YimingZhanshen/AirPodsWindows/issues/new/choose) to report bugs or suggest new features.
* [Submit a PR](https://github.com/YimingZhanshen/AirPodsWindows/compare) to fix a known bug or try something from the TODO list.
* [Translate to other languages](/CONTRIBUTING.md#-translation-guide) or [improve existing translations](/CONTRIBUTING.md#-translation-guide).

## 💎 Third-Party Libraries
* [Qt 5.15.2](https://www.qt.io/download-qt-installer) ([LGPLv3 License](https://doc.qt.io/qt-5/lgpl.html))
* [spdlog](https://github.com/gabime/spdlog) ([MIT License](https://github.com/gabime/spdlog/blob/v1.x/LICENSE))
* [cxxopts](https://github.com/jarro2783/cxxopts) ([MIT License](https://github.com/jarro2783/cxxopts/blob/master/LICENSE))
* [cpr](https://github.com/whoshuu/cpr) ([MIT License](https://github.com/whoshuu/cpr/blob/master/LICENSE))
* [json](https://github.com/nlohmann/json) ([MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT))
* [SingleApplication](https://github.com/itay-grudev/SingleApplication) ([MIT License](https://github.com/itay-grudev/SingleApplication/blob/master/LICENSE))
* [pfr](https://github.com/boostorg/pfr) ([BSL-1.0 License](https://github.com/boostorg/pfr/blob/develop/LICENSE_1_0.txt))
* [magic_enum](https://github.com/Neargye/magic_enum) ([MIT License](https://github.com/Neargye/magic_enum/blob/master/LICENSE))
* [stacktrace](https://github.com/boostorg/stacktrace) ([BSL-1.0 License](https://www.boost.org/LICENSE_1_0.txt))

## 🍺 Credits & References

### Core References
* [librepods](https://github.com/kavishdevar/librepods) - AAP protocol reverse engineering & documentation, MagicAAP driver (core reference for ANC implementation)
* [OpenPods](https://github.com/adolfintel/OpenPods) - AirPods BLE broadcast protocol parsing

### Protocol Documentation
* [AAP Definitions](https://github.com/kavishdevar/librepods/blob/main/AAP%20Definitions.md) - Apple Accessory Protocol detailed definitions
* [Discontinued Privacy: Personal Data Leaks in Apple Bluetooth-Low-Energy Continuity Protocols](https://hal.inria.fr/hal-02394619/document) - Apple BLE protocol research paper

### Related Projects
* [MagicPods](https://magicpods.app/) - Commercial AirPods Windows application
* [AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop) - Original project (this project is forked from)

## 📜 License
This project is licensed under the [GPLv3 License](/LICENSE).

## 👤 Authors
* **YimingZhanshen** - Fork maintainer, Windows ANC feature development
* **SpriteOvO** - Original project author

---
<p align="center">If this project helps you, please consider giving a ⭐ Star!</p>
