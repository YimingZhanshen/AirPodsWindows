<h1 align="center">
    <a href="https://github.com/YimingZhanshen/AirPodsWindows"><img src="/Source/Resource/Image/Icon.svg" alt="Icon" width="128"></a>
    <br>
    AirPodsDesktop
</h1>
<p align="center">AirPods 桌面使用者體驗增進軟體 - Windows 完整 ANC 控制</p>
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
<p align="center">🌎 <a href="/README.md">English</a> | 🌏 <a href="/README-CN.md">简体中文</a> | 🌏 繁體中文</p>

## 🔍 預覽
![Preview Image](/Assets/Preview.gif)

## ✨ 功能

### 基礎功能
* 🔋 **電池資訊顯示** - 左右耳機及充電盒電量即時顯示
* 👂 **入耳自動偵測** - 取下自動暫停，戴上自動恢復播放
* 🚀 **低延遲音訊模式** - 修復短音訊播放問題
* 🌈 **精美動畫** - 優雅的使用者介面體驗

### ANC 進階功能（需要 MagicAAP 驅動程式）
* 🎧 **降噪控制** - 切換關閉/降噪/通透/自適應模式
* 🗣️ **對話感知** - 說話時自動降低媒體音量（AirPods Pro/Max）
* 🔊 **個人化音量** - 根據環境和聆聽習慣調整音量
* 👂 **入耳偵測狀態** - 即時偵測左右耳機佩戴狀態
* 🔇 **大聲降低** - 降低高音量以保護聽力
* 📊 **自適應通透等級** - 精細控制環境聲音透過程度（0-50）

## ⚙️ 系統需求

- **作業系統**：Windows 10/11
- **藍牙**：需要藍牙 4.0+ 介面卡
- **AirPods**：支援所有 AirPods 型號
- **ANC 功能**：需要 [MagicAAP 驅動程式](https://github.com/kavishdevar/librepods)（僅 AirPods Pro/Max/AirPods 4 ANC）

## 📦 安裝

1. 從 [Releases](https://github.com/YimingZhanshen/AirPodsWindows/releases) 下載最新版本
2. 執行安裝程式或解壓縮可攜式版本
3. （選用）安裝 MagicAAP 驅動程式以啟用 ANC 控制功能

## 🛠️ 構建
檢視 [構建說明](/Docs/Build.md)。

## 🤝 貢獻
*AirPodsDesktop* 是一個開源專案，您可以透過以下方式貢獻：
* [開立問題](https://github.com/YimingZhanshen/AirPodsWindows/issues/new/choose) 以回報錯誤或建議新功能。
* [提交 PR](https://github.com/YimingZhanshen/AirPodsWindows/compare) 以修正已知 BUG 或嘗試 TODO 清單中的項目。
* [翻譯成其它語言](/CONTRIBUTING.md#-translation-guide) 或 [改進現有的翻譯](/CONTRIBUTING.md#-translation-guide)。

## 💎 第三方函式庫
* [Qt 5.15.2](https://www.qt.io/download-qt-installer) ([LGPLv3 License](https://doc.qt.io/qt-5/lgpl.html))
* [spdlog](https://github.com/gabime/spdlog) ([MIT License](https://github.com/gabime/spdlog/blob/v1.x/LICENSE))
* [cxxopts](https://github.com/jarro2783/cxxopts) ([MIT License](https://github.com/jarro2783/cxxopts/blob/master/LICENSE))
* [cpr](https://github.com/whoshuu/cpr) ([MIT License](https://github.com/whoshuu/cpr/blob/master/LICENSE))
* [json](https://github.com/nlohmann/json) ([MIT License](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT))
* [SingleApplication](https://github.com/itay-grudev/SingleApplication) ([MIT License](https://github.com/itay-grudev/SingleApplication/blob/master/LICENSE))
* [pfr](https://github.com/boostorg/pfr) ([BSL-1.0 License](https://github.com/boostorg/pfr/blob/develop/LICENSE_1_0.txt))
* [magic_enum](https://github.com/Neargye/magic_enum) ([MIT License](https://github.com/Neargye/magic_enum/blob/master/LICENSE))
* [stacktrace](https://github.com/boostorg/stacktrace) ([BSL-1.0 License](https://www.boost.org/LICENSE_1_0.txt))

## 🍺 銘謝與參考

### 核心參考
* [librepods](https://github.com/kavishdevar/librepods) - AAP 協議逆向工程與文件，MagicAAP 驅動程式（ANC 實作核心參考）
* [OpenPods](https://github.com/adolfintel/OpenPods) - AirPods BLE 廣播協議解析

### 協議文件
* [AAP Definitions](https://github.com/kavishdevar/librepods/blob/main/AAP%20Definitions.md) - Apple Accessory Protocol 詳細定義
* [Discontinued Privacy: Personal Data Leaks in Apple Bluetooth-Low-Energy Continuity Protocols](https://hal.inria.fr/hal-02394619/document) - Apple BLE 協議研究論文

### 相關專案
* [MagicPods](https://magicpods.app/) - 商業 AirPods Windows 應用程式
* [AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop) - 原始專案（本專案從此分叉）

## 📜 授權
本專案採用 [GPLv3 授權條款](/LICENSE)。

## 👤 作者
* **YimingZhanshen** - Fork 維護者，Windows ANC 功能開發
* **SpriteOvO** - 原始專案作者

---
<p align="center">如果此專案對您有幫助，請考慮給一個 ⭐ Star！</p>
