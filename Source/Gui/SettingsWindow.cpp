//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "SettingsWindow.h"

#include <QLabel>
#include <QToolTip>
#include <QCheckBox>
#include <QPushButton>
#include <QMessageBox>
#include <QDesktopServices>

#include <Config.h>

#include "../Application.h"
#include "../Core/Debug.h"

using namespace std::chrono_literals;

namespace Gui {

class TipLabel : public QLabel
{
    Q_OBJECT

private:
    constexpr static auto content = "(?)";

public:
    TipLabel(QString text, QWidget *parent) : QLabel{content, parent}, _text{std::move(text)}
    {
        QPalette palette = this->palette();
        palette.setColor(QPalette::WindowText, Qt::darkGray);
        setPalette(palette);
    }

private:
    QString _text;

    void enterEvent(QEvent *event) override
    {
        QToolTip::showText(QCursor::pos(), _text, this);
    }

    void leaveEvent(QEvent *event) override
    {
        QToolTip::hideText();
    }
};

SettingsWindow::SettingsWindow(QWidget *parent) : QDialog{parent}
{
    const auto &constMetaFields = GetConstMetaFields();

    _ui.setupUi(this);

    auto debugTabIndex = _ui.tabWidget->count() - 1;
    APD_ASSERT(_ui.tabWidget->tabText(debugTabIndex) == "Debug");
#if !defined APD_DEBUG
    _ui.tabWidget->setTabVisible(debugTabIndex, false);
#else
    connect(
        _ui.cbAdvOverride, &QCheckBox::toggled, this, &SettingsWindow::On_cbAdvOverride_toggled);

    connect(
        _ui.teAdvOverride, &QTextEdit::textChanged, this,
        &SettingsWindow::On_teAdvOverride_textChanged);
#endif

    InitCreditsText();

    auto versionText =
        QString{"<a href=\"%1\">v%2</a>"}
            .arg("https://github.com/SpriteOvO/AirPodsDesktop/releases/tag/" CONFIG_VERSION_STRING)
            .arg(CONFIG_VERSION_STRING);
#if defined APD_BUILD_GIT_HASH
    versionText +=
        QString{" (<a href=\"%1\">%2</a>)"}
            .arg("https://github.com/SpriteOvO/AirPodsDesktop/commit/" APD_BUILD_GIT_HASH)
            .arg(QString{APD_BUILD_GIT_HASH}.left(7));
#endif
    _ui.lbVersion->setText(versionText);

    _ui.hlLowAudioLatency->addWidget(
        new TipLabel{constMetaFields.low_audio_latency.Description(), this});

    _ui.hlTipAutoEarDetection->addWidget(
        new TipLabel{constMetaFields.automatic_ear_detection.Description(), this});

    _ui.hlTipConversationalAwareness->addWidget(
        new TipLabel{constMetaFields.conversational_awareness.Description(), this});

    _ui.hlTipPersonalizedVolume->addWidget(
        new TipLabel{constMetaFields.personalized_volume.Description(), this});

    _ui.hlTipLoudSoundReduction->addWidget(
        new TipLabel{constMetaFields.loud_sound_reduction.Description(), this});

    // Setup noise control mode combo box
    _ui.cbNoiseControlMode->addItem(tr("Off"));
    _ui.cbNoiseControlMode->addItem(tr("Noise Cancellation"));
    _ui.cbNoiseControlMode->addItem(tr("Transparency"));
    _ui.cbNoiseControlMode->addItem(tr("Adaptive"));

    _ui.hsMaxReceivingRange->setMinimum(50);
    _ui.hsMaxReceivingRange->setMaximum(100);

    for (const auto &locale : ApdApp->AvailableLocales()) {
        _ui.cbLanguages->addItem(locale.nativeLanguageName());
    }
    _ui.cbLanguages->addItem("...");

    Update(GetCurrent(), false);

    connect(
        _ui.buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
        &SettingsWindow::RestoreDefaults);

    connect(
        _ui.cbLanguages, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (_trigger) {
                On_cbLanguages_currentIndexChanged(index);
            }
        });

    connect(_ui.cbAutoRun, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbAutoRun_toggled(checked);
        }
    });

    connect(_ui.cbLowAudioLatency, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbLowAudioLatency_toggled(checked);
        }
    });

    connect(_ui.cbAutoEarDetection, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbAutoEarDetection_toggled(checked);
        }
    });

    connect(_ui.cbConversationalAwareness, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbConversationalAwareness_toggled(checked);
        }
    });

    connect(_ui.cbPersonalizedVolume, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbPersonalizedVolume_toggled(checked);
        }
    });

    connect(_ui.cbLoudSoundReduction, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbLoudSoundReduction_toggled(checked);
        }
    });

    connect(_ui.cbNoiseControlMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (_trigger) {
            On_cbNoiseControlMode_currentIndexChanged(index);
        }
    });

    connect(_ui.hsAdaptiveTransparencyLevel, &QSlider::valueChanged, this, [this](int value) {
        if (_trigger) {
            On_hsAdaptiveTransparencyLevel_valueChanged(value);
        }
    });

    connect(_ui.hsMaxReceivingRange, &QSlider::valueChanged, this, [this](int value) {
        if (_trigger) {
            On_hsMaxReceivingRange_valueChanged(value);
        }
    });

    connect(
        _ui.rbDisplayBatteryOnTrayIconDisable, &QRadioButton::toggled, this, [this](bool checked) {
            if (_trigger) {
                On_cbDisplayBatteryOnTrayIcon_toggled(TrayIconBatteryBehavior::Disable);
            }
        });

    connect(
        _ui.rbDisplayBatteryOnTrayIconWhenLowBattery, &QRadioButton::toggled, this,
        [this](bool checked) {
            if (_trigger) {
                On_cbDisplayBatteryOnTrayIcon_toggled(TrayIconBatteryBehavior::WhenLowBattery);
            }
        });

    connect(
        _ui.rbDisplayBatteryOnTrayIconAlways, &QRadioButton::toggled, this, [this](bool checked) {
            if (_trigger) {
                On_cbDisplayBatteryOnTrayIcon_toggled(TrayIconBatteryBehavior::Always);
            }
        });

    connect(
        _ui.rbDisplayBatteryOnTaskbarDisable, &QRadioButton::toggled, this, [this](bool checked) {
            if (_trigger) {
                On_cbDisplayBatteryOnTaskbar_toggled(TaskbarStatusBehavior::Disable);
            }
        });

    connect(_ui.rbDisplayBatteryOnTaskbarText, &QRadioButton::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbDisplayBatteryOnTaskbar_toggled(TaskbarStatusBehavior::Text);
        }
    });

    connect(_ui.rbDisplayBatteryOnTaskbarIcon, &QRadioButton::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbDisplayBatteryOnTaskbar_toggled(TaskbarStatusBehavior::Icon);
        }
    });

    connect(_ui.pbUnbind, &QPushButton::clicked, this, [this]() {
        if (_trigger) {
            On_pbUnbind_clicked();
        }
    });

    connect(_ui.pbOpenLogsDirectory, &QPushButton::clicked, this, [this]() {
        if (_trigger) {
            On_pbOpenLogsDirectory_clicked();
        }
    });
}

int SettingsWindow::GetTabCount() const
{
    return _ui.tabWidget->count();
}

int SettingsWindow::GetTabCurrentIndex() const
{
    return _ui.tabWidget->currentIndex();
}

int SettingsWindow::GetTabLastVisibleIndex() const
{
    return GetTabCount() - 2 /* Skip the Debug tab */;
}

void SettingsWindow::SetTabIndex(int index)
{
    _ui.tabWidget->setCurrentIndex(index);
}

void SettingsWindow::InitCreditsText()
{
    //: To credit translators, you can leave your name here if you wish.
    //: (Sorted by time added, separated by "|" character, only single "|" for empty)
    auto l10nContributorsStr = tr("|");
    if (!l10nContributorsStr.isEmpty()) {
        auto l10nContributors = l10nContributorsStr.split('|', Qt::SkipEmptyParts);
        if (l10nContributors.isEmpty()) {
            l10nContributorsStr.clear();
        }
        else {
            l10nContributorsStr = tr("Translation Contributors:");
            for (const auto &contributor : l10nContributors) {
                l10nContributorsStr += "<br> - " + contributor.trimmed();
            }
            l10nContributorsStr += "<br><br>";
        }
    }

    static QString libs = [] {
        struct LibInfo {
            const char *name, *url, *license, *licenseUrl;
        };
        static std::vector<LibInfo> libs{
            // clang-format off
            { "Qt 5", "https://www.qt.io/download-qt-installer", "LGPLv3", "https://doc.qt.io/qt-5/lgpl.html" },
            { "spdlog", "https://github.com/gabime/spdlog", "MIT", "https://github.com/gabime/spdlog/blob/v1.x/LICENSE" },
            { "cxxopts", "https://github.com/jarro2783/cxxopts", "MIT", "https://github.com/jarro2783/cxxopts/blob/master/LICENSE" },
            { "cpr", "https://github.com/whoshuu/cpr", "MIT", "https://github.com/whoshuu/cpr/blob/master/LICENSE" },
            { "json", "https://github.com/nlohmann/json", "MIT", "https://github.com/nlohmann/json/blob/develop/LICENSE.MIT" },
            { "SingleApplication", "https://github.com/itay-grudev/SingleApplication", "MIT", "https://github.com/itay-grudev/SingleApplication/blob/master/LICENSE" },
            { "pfr", "https://github.com/boostorg/pfr", "BSL-1.0", "https://github.com/boostorg/pfr/blob/develop/LICENSE_1_0.txt" },
            { "magic_enum", "https://github.com/Neargye/magic_enum", "MIT", "https://github.com/Neargye/magic_enum/blob/master/LICENSE" },
            { "stacktrace", "https://github.com/boostorg/stacktrace", "BSL-1.0", "https://www.boost.org/LICENSE_1_0.txt" }
            // clang-format on
        };

        QString result;
        for (const auto &lib : libs) {
            result += QString{"<br> - <a href=\"%2\">%1</a> (<a href=\"%4\">%3 License</a>)"}
                          .arg(lib.name)
                          .arg(lib.url)
                          .arg(lib.license)
                          .arg(lib.licenseUrl);
        }
        return result;
    }();

    static QString references = [] {
        struct RefInfo {
            const char *name, *url, *description;
        };
        static std::vector<RefInfo> refs{
            // clang-format off
            { "librepods", "https://github.com/kavishdevar/librepods", "AAP protocol & MagicAAP driver" },
            { "OpenPods", "https://github.com/adolfintel/OpenPods", "AirPods BLE protocol" },
            { "AirPodsDesktop", "https://github.com/SpriteOvO/AirPodsDesktop", "Original project" }
            // clang-format on
        };

        QString result;
        for (const auto &ref : refs) {
            result += QString{"<br> - <a href=\"%2\">%1</a> (%3)"}
                          .arg(ref.name)
                          .arg(ref.url)
                          .arg(ref.description);
        }
        return result;
    }();

    auto libsStr = tr("Third-Party Libraries:") + libs;
    auto refsStr = "<br><br>References:" + references;

    _ui.tbCredits->setHtml(l10nContributorsStr + libsStr + refsStr);
}

void SettingsWindow::RestoreDefaults()
{
    Save(GetDefault());
    Update(GetCurrent(), false);
}

void SettingsWindow::Update(const Fields &fields, bool trigger)
{
    _trigger = trigger;

    auto currentLangIndex = ApdApp->GetCurrentLoadedLocaleIndex();
    _lastLanguageIndex = currentLangIndex;
    _ui.cbLanguages->setCurrentIndex(currentLangIndex);

    _ui.cbAutoRun->setChecked(fields.auto_run);

    _ui.cbLowAudioLatency->setChecked(fields.low_audio_latency);

    _ui.cbAutoEarDetection->setChecked(fields.automatic_ear_detection);

    _ui.cbConversationalAwareness->setChecked(fields.conversational_awareness);

    _ui.cbPersonalizedVolume->setChecked(fields.personalized_volume);

    _ui.cbLoudSoundReduction->setChecked(fields.loud_sound_reduction);

    // noise_control_mode values 1-4 map to UI index 0-3
    int noiseControlIndex = static_cast<int>(fields.noise_control_mode) - 1;
    if (noiseControlIndex >= 0 && noiseControlIndex < _ui.cbNoiseControlMode->count()) {
        _ui.cbNoiseControlMode->setCurrentIndex(noiseControlIndex);
    }

    _ui.hsAdaptiveTransparencyLevel->setValue(fields.adaptive_transparency_level);

    _ui.hsMaxReceivingRange->setValue(-fields.rssi_min);

    auto [batteryOnTrayIconDisable, batteryOnTrayIconWhenLowBattery, batteryOnTrayIconAlways] =
        std::make_tuple(
            fields.tray_icon_battery == TrayIconBatteryBehavior::Disable,
            fields.tray_icon_battery == TrayIconBatteryBehavior::WhenLowBattery,
            fields.tray_icon_battery == TrayIconBatteryBehavior::Always);

    _ui.rbDisplayBatteryOnTrayIconDisable->setChecked(batteryOnTrayIconDisable);
    _ui.rbDisplayBatteryOnTrayIconWhenLowBattery->setChecked(batteryOnTrayIconWhenLowBattery);
    _ui.rbDisplayBatteryOnTrayIconAlways->setChecked(batteryOnTrayIconAlways);

    auto [batteryOnTaskbarDisable, batteryOnTaskbarText, batteryOnTaskbarIcon] = std::make_tuple(
        fields.battery_on_taskbar == TaskbarStatusBehavior::Disable,
        fields.battery_on_taskbar == TaskbarStatusBehavior::Text,
        fields.battery_on_taskbar == TaskbarStatusBehavior::Icon);

    _ui.rbDisplayBatteryOnTaskbarDisable->setChecked(batteryOnTaskbarDisable);
    _ui.rbDisplayBatteryOnTaskbarText->setChecked(batteryOnTaskbarText);
    _ui.rbDisplayBatteryOnTaskbarIcon->setChecked(batteryOnTaskbarIcon);

    _ui.pbUnbind->setDisabled(fields.device_address == 0);

    _trigger = true;
}

void SettingsWindow::UpdateAdvOverride()
{
    auto advsStr = _ui.teAdvOverride->toPlainText();
    auto vAdvsStr = advsStr.split('\n', QString::SkipEmptyParts);

    std::vector<std::vector<uint8_t>> advs;

    for (const auto &advStr : vAdvsStr) {

        auto advBytesStr = advStr.split(' ', QString::SkipEmptyParts);

        std::vector<uint8_t> bytes;
        for (const auto advByteStr : advBytesStr) {
            bool success{false};
            auto byte = advByteStr.toUInt(&success, 16);
            APD_ASSERT(success);
            bytes.emplace_back(byte);
        }

        advs.emplace_back(std::move(bytes));
    }

    Core::Debug::DebugConfig::GetInstance().UpdateAdvOverride(
        _ui.cbAdvOverride->isChecked(), std::move(advs));
}

void SettingsWindow::showEvent(QShowEvent *event)
{
    Update(GetCurrent(), false);
}

void SettingsWindow::On_cbLanguages_currentIndexChanged(int index)
{
    if (_ui.cbLanguages->count() != index + 1) {
        _lastLanguageIndex = index;

        const auto &availableLocales = ApdApp->AvailableLocales();
        const auto &locale = availableLocales.at(index);

        ModifiableAccess()->language_locale = locale.name();
    }
    else {
        _ui.cbLanguages->setCurrentIndex(_lastLanguageIndex);
        // clang-format off
        QDesktopServices::openUrl(QUrl{
            "https://github.com/SpriteOvO/AirPodsDesktop/blob/main/CONTRIBUTING.md#-translation-guide"
        });
        // clang-format on
    }
}

void SettingsWindow::On_cbAutoRun_toggled(bool checked)
{
    ModifiableAccess()->auto_run = checked;
}

void SettingsWindow::On_pbUnbind_clicked()
{
    _ui.pbUnbind->setDisabled(true);
    ModifiableAccess()->device_address = 0;
}

void SettingsWindow::On_cbDisplayBatteryOnTrayIcon_toggled(TrayIconBatteryBehavior behavior)
{
    ModifiableAccess()->tray_icon_battery = behavior;
}

void SettingsWindow::On_cbDisplayBatteryOnTaskbar_toggled(TaskbarStatusBehavior behavior)
{
    ModifiableAccess()->battery_on_taskbar = behavior;
}

void SettingsWindow::On_cbLowAudioLatency_toggled(bool checked)
{
    ModifiableAccess()->low_audio_latency = checked;
}

void SettingsWindow::On_cbAutoEarDetection_toggled(bool checked)
{
    ModifiableAccess()->automatic_ear_detection = checked;
}

void SettingsWindow::On_cbConversationalAwareness_toggled(bool checked)
{
    ModifiableAccess()->conversational_awareness = checked;
}

void SettingsWindow::On_cbPersonalizedVolume_toggled(bool checked)
{
    ModifiableAccess()->personalized_volume = checked;
}

void SettingsWindow::On_cbLoudSoundReduction_toggled(bool checked)
{
    ModifiableAccess()->loud_sound_reduction = checked;
}

void SettingsWindow::On_cbNoiseControlMode_currentIndexChanged(int index)
{
    // UI index 0-3 maps to NoiseControlMode values 1-4 (Off=1, ANC=2, Transparency=3, Adaptive=4)
    ModifiableAccess()->noise_control_mode = static_cast<uint32_t>(index + 1);
}

void SettingsWindow::On_hsAdaptiveTransparencyLevel_valueChanged(int value)
{
    ModifiableAccess()->adaptive_transparency_level = value;
}

void SettingsWindow::On_hsMaxReceivingRange_valueChanged(int value)
{
    ModifiableAccess()->rssi_min = -value;
}

void SettingsWindow::On_pbOpenLogsDirectory_clicked()
{
    Utils::File::OpenFileLocation(Logger::GetLogFilePath());
}

void SettingsWindow::On_cbAdvOverride_toggled(bool checked)
{
    UpdateAdvOverride();
}

void SettingsWindow::On_teAdvOverride_textChanged()
{
    UpdateAdvOverride();
}

} // namespace Gui

#include "SettingsWindow.moc"
