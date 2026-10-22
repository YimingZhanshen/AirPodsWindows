<h1 align="center">
    <a href="https://github.com/YimingZhanshen/AirPodsWindows"><img src="/Source/Resource/Image/Icon.svg" alt="Icon" width="128"></a>
    <br>
    AirPodsWindows
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

> [!IMPORTANT]
> **這是 Fork，不是原專案 AirPodsDesktop。**
> AirPodsWindows 完整繼承上游功能，並加上原專案沒有的部分：透過 [MagicAAP 驅動程式](https://magicpods.app/magicaap/)
> 實作的 **Windows 平台完整 ANC 控制** —— 降噪模式切換、對話感知（含說話音量降低滑桿）、個人化音量、
> 大聲降低、自適應通透等級，以及託降噪選單。
> 想要原版？→ [SpriteOvO/AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop)

## 🆚 AirPodsWindows 與 AirPodsDesktop 的差異

| | [SpriteOvO/AirPodsDesktop](https://github.com/SpriteOvO/AirPodsDesktop)（上游） | **AirPodsWindows（本倉庫）** |
| --- | --- | --- |
| 電池 / 入耳偵測 / 彈窗動畫 / 淺色深色主題 | ✅ | ✅ |
| **Windows 降噪控制** —— 關閉 / 降噪 / 通透 / 自適應 | ❌ | ✅（需 MagicAAP 驅動程式） |
| 對話感知 **+ 說話音量降低滑桿** | ❌ | ✅ |
| 個人化音量 · 大聲降低 · 自適應通透等級 | ❌ | ✅ |
| 託降噪選單 | ❌ | ✅ |
| 發布與程式內更新指向 | `SpriteOvO/AirPodsDesktop` | **`YimingZhanshen/AirPodsWindows`** |
| 本建置的 Issue 追蹤 | 上游倉庫 | 本倉庫 |
| 技術基線 | 0.6.1 起 Qt 6.8.3、僅 x64 | 與上游保持同步 |

* 僅 ANC 相關功能需要 [MagicAAP 驅動程式](https://magicpods.app/magicaap/)，其餘功能無需驅動即可使用。
* 在原專案上也能重現的問題更適合回報到上游倉庫；本倉庫追蹤 Windows ANC 功能與本專案的其它差異。

## 🔍 預覽

| 淺色主題 | 深色主題 |
| :---: | :---: |
| <img src="/Assets/Preview-Light.gif" alt="淺色主題彈窗動畫" width="360"> | <img src="/Assets/Preview-Dark.gif" alt="深色主題彈窗動畫" width="360"> |

## ✨ 功能

* 🔋 通知區域電池資訊。
* 👂 自動入耳偵測與媒體播放控制。
* 🚀 低延遲音訊模式。
* 🌈 裝置彈出視窗動畫，以及淺色、深色或系統主題。

### ANC 進階功能（需要 MagicAAP 驅動程式）

* 🎧 降噪控制 - 切換關閉/降噪/通透/自適應模式。
* 🗣️ 對話感知 - 說話時自動降低媒體音量（AirPods Pro/Max），並可調整說話時的音量降低幅度。
* 🔊 個人化音量 - 根據環境和聆聽習慣自動調整音量。
* 👂 入耳偵測狀態 - 即時偵測左右耳機佩戴狀態。
* 🔇 大聲降低 - 降低過大的聲音以保護聽力。
* 📊 自適應通透等級 - 精細控制環境聲音透過程度（0-50）。

## 💻 系統需求

正式版本支援 Windows 10 1809 以上或 Windows 11 x64，不支援 32 位元 Windows 與 ARM64。
現有 Qt 5／Win32 用戶可直接使用程式內更新器遷移至 Qt 6／x64，既有使用者設定會保留。

ANC 控制另外需要 [MagicAAP 驅動程式](https://magicpods.app/magicaap/)，且僅適用於 AirPods Pro / Max / AirPods 4（ANC）。

## 📦 安裝

1. 從 [Releases](https://github.com/YimingZhanshen/AirPodsWindows/releases) 下載最新版本
2. 執行安裝程式或解壓縮可攜式版本
3. （選用）安裝 MagicAAP 驅動程式以啟用 ANC 控制功能 —— [完整安裝步驟見 Docs/Driver.md](/Docs/Driver.md)

## 🛠️ 建置與測試

Windows 環境需求、建置命令與測試方式請參閱[建置說明](/Docs/Build.md)。

## 🤝 貢獻

*AirPodsWindows* 是一個開源項目，您可以透過以下方式貢獻：

* [開立問題](https://github.com/YimingZhanshen/AirPodsWindows/issues/new/choose) 以回報錯誤或建議新功能。
* [提交 PR](https://github.com/YimingZhanshen/AirPodsWindows/compare) 以修正錯誤、改善文件或新增功能。
* [翻譯成其它語言](/CONTRIBUTING.md#-translation-guide) 或 [改進現有的翻譯](/CONTRIBUTING.md#-translation-guide)。

更新專案文件時，請保持 `README.md`、`README-CN.md` 與 `README-TW.md` 的章節順序、功能、命令、連結及致謝內容一致。

## 💎 第三方相依項目

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
