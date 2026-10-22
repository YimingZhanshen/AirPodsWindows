//
// AirPodsWindows - AirPods Desktop User Experience Enhancement Program.
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

#include <QAbstractItemView>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QPainter>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QSysInfo>

#include "Theme.h"
#include <QCheckBox>
#include <QPushButton>
#include <QMessageBox>
#include <QDesktopServices>

#include <Config.h>

#include "../Core/Debug.h"
#include "../Core/ConnectionDiagnostics.h"
#include "Widget/DriverStatusCard.h"

using namespace std::chrono_literals;

namespace Gui {

namespace {

class ComboPopupPaintFilter final : public QObject
{
public:
    explicit ComboPopupPaintFilter(QObject *parent) : QObject{parent} {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            auto *popup = qobject_cast<QWidget *>(watched);
            const auto &colors = Theme::Manager::Instance().Colors();
            QPainter painter{popup};
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(popup->rect(), Qt::transparent);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(colors.popupBorder);
            painter.setBrush(colors.popupSurface);
            painter.drawRoundedRect(QRectF{popup->rect()}.adjusted(3, 3, -3, -3), 8, 8);
            // Consume the private QFrame paint event to avoid a second, native rectangular border.
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
};

void ConfigureComboBoxPopup(QComboBox *comboBox)
{
    auto *view = comboBox->view();
    view->setFrameShape(QFrame::NoFrame);

    // Qt wraps combo views in a private QFrame. Paint one antialiased surface on a translucent
    // window; a QRegion mask would discard the partially transparent pixels along its corners.
    auto *popup = view->window();
    popup->setObjectName("apdComboPopup");
    Theme::ConfigurePopupSurface(popup);
    popup->installEventFilter(new ComboPopupPaintFilter{popup});
    view->viewport()->setAutoFillBackground(false);
}

} // namespace

SettingsWindow::SettingsWindow(
    std::function<int()> getCurrentLocaleIndex, Core::QuickConnect::Controller &quickConnect,
    std::function<Core::AirPods::Manager &()> getAirPodsManager, QWidget *parent)
    : QDialog{parent}, _getCurrentLocaleIndex{std::move(getCurrentLocaleIndex)},
      _quickConnect{quickConnect}
{
    // getAirPodsManager is deliberately NOT stored here: this window is built
    // by the tray icon before the application's AirPods manager exists, so any
    // resolution during construction dereferences a null pointer. The provider
    // is installed later via SetAirPodsManagerProvider().
    static_cast<void>(getAirPodsManager);

    _ui.setupUi(this);

    // Navigation pane + stacked pages replace the former tab widget. The last page is Debug.
    const auto debugPageIndex = _ui.navList->count() - 1;
    APD_ASSERT(debugPageIndex == _ui.pages->count() - 1);
    APD_ASSERT(_ui.navList->item(debugPageIndex)->text() == "Debug");

    connect(
        _ui.navList, &QListWidget::currentRowChanged, _ui.pages, &QStackedWidget::setCurrentIndex);
    _ui.navList->setCurrentRow(0);

    for (const auto &[label, checkBox] : {
             std::pair{_ui.lbLowAudioLatency, _ui.cbLowAudioLatency},
             std::pair{_ui.lbAutoEarDetection, _ui.cbAutoEarDetection},
             std::pair{_ui.lbTrayQuickConnect, _ui.cbTrayQuickConnectEnabled},
         })
    {
        label->setBuddy(checkBox);
        checkBox->setAccessibleName(label->text());
    }

    // The default combo popup delegate ignores stylesheet item padding; the styled one honours it.
    _ui.cbLanguages->setItemDelegate(new QStyledItemDelegate{_ui.cbLanguages});
    _ui.cbAppearanceMode->setItemDelegate(new QStyledItemDelegate{_ui.cbAppearanceMode});
    _ui.cbTrayQuickConnectDevice->setItemDelegate(
        new QStyledItemDelegate{_ui.cbTrayQuickConnectDevice});
    ConfigureComboBoxPopup(_ui.cbLanguages);
    ConfigureComboBoxPopup(_ui.cbAppearanceMode);
    ConfigureComboBoxPopup(_ui.cbTrayQuickConnectDevice);
    _ui.lbAppearance->setBuddy(_ui.cbAppearanceMode);
    _ui.cbAppearanceMode->setAccessibleName(_ui.lbAppearance->text());

    // Neither is the dialog's default action; without this the first one would be drawn as the
    // accent-coloured default button.
    _ui.pbUnbind->setAutoDefault(false);
    _ui.pbOpenLogsDirectory->setAutoDefault(false);
    for (auto *button : _ui.buttonBox->buttons()) {
        if (auto *pushButton = qobject_cast<QPushButton *>(button); pushButton != nullptr) {
            pushButton->setAutoDefault(false);
        }
    }

    _aboutTextTemplate = _ui.label->text();
    UpdateDescriptions();
    UpdateStandardButtonTexts();
    connect(
        &Theme::Manager::Instance(), &Theme::Manager::Changed, this,
        &SettingsWindow::UpdateDescriptions);

#if !defined APD_DEBUG
    _ui.navList->item(debugPageIndex)->setHidden(true);
#else
    connect(
        _ui.cbAdvOverride, &QCheckBox::toggled, this, &SettingsWindow::On_cbAdvOverride_toggled);

    connect(
        _ui.teAdvOverride, &QTextEdit::textChanged, this,
        &SettingsWindow::On_teAdvOverride_textChanged);
#endif

    InitCreditsText();

    auto versionText =
        QString{"<a href=\"%1\">v%2</a>"}.arg(Config::UrlCurrentRelease).arg(CONFIG_VERSION_STRING);
#if defined APD_BUILD_GIT_HASH
    versionText +=
        QString{" (<a href=\"%1\">%2</a>)"}
            .arg(QString{"%1/commit/%2"}.arg(Config::UrlRepository).arg(APD_BUILD_GIT_HASH))
            .arg(QString{APD_BUILD_GIT_HASH}.left(7));
#endif
    _ui.lbVersion->setText(versionText);

    // Setup noise control mode combo box
    _ui.cbNoiseControlMode->addItem(tr("Off"));
    _ui.cbNoiseControlMode->addItem(tr("Noise Cancellation"));
    _ui.cbNoiseControlMode->addItem(tr("Transparency"));
    _ui.cbNoiseControlMode->addItem(tr("Adaptive"));

    _ui.hsMaxReceivingRange->setMinimum(50);
    _ui.hsMaxReceivingRange->setMaximum(100);

    for (const auto &locale : Utils::AvailableLocales()) {
        const auto displayName = locale.language() == QLocale::English
                                     ? QStringLiteral("English")
                                     : locale.nativeLanguageName();
        _ui.cbLanguages->addItem(displayName);
    }
    _ui.cbLanguages->addItem("...");

    connect(&_quickConnect, &Core::QuickConnect::Controller::DevicesChanged, this, [this] {
        UpdateQuickConnectDevices(GetCurrent());
    });

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

    connect(
        _ui.cbAppearanceMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int index) {
            if (_trigger) {
                On_cbAppearanceMode_currentIndexChanged(index);
            }
        });

    connect(_ui.cbTrayQuickConnectEnabled, &QCheckBox::toggled, this, [this](bool checked) {
        if (_trigger) {
            On_cbTrayQuickConnectEnabled_toggled(checked);
        }
    });

    connect(
        _ui.cbTrayQuickConnectDevice, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int index) {
            if (_trigger) {
                On_cbTrayQuickConnectDevice_currentIndexChanged(index);
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

    connect(_ui.sliderConversationalAwarenessVolume, &QSlider::valueChanged, this, [this](int value) {
        if (_trigger) {
            On_sliderConversationalAwarenessVolume_valueChanged(value);
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

    SetupDriverStatusCard();
}

int SettingsWindow::GetTabCount() const
{
    return _ui.pages->count();
}

void SettingsWindow::SetLowAudioLatencyChecked(bool checked)
{
    const auto oldTrigger = _trigger;
    _trigger = false;
    _ui.cbLowAudioLatency->setChecked(checked);
    _trigger = oldTrigger;
}

int SettingsWindow::GetTabCurrentIndex() const
{
    return _ui.pages->currentIndex();
}

int SettingsWindow::GetTabLastVisibleIndex() const
{
    return GetTabCount() - 2 /* Skip the Debug tab */;
}

void SettingsWindow::SetTabIndex(int index)
{
    _ui.navList->setCurrentRow(index);
}

void SettingsWindow::UpdateDescriptions()
{
    // The setting descriptions live in `Core::Settings` (as `Impl::Desc`) and are translated
    // there, so they are re-read on every language change.
    const auto &constMetaFields = GetConstMetaFields();

    _ui.lbDescLowAudioLatency->setText(constMetaFields.low_audio_latency.Description());
    _ui.lbDescAutoEarDetection->setText(constMetaFields.automatic_ear_detection.Description());
    _ui.lbDescTrayQuickConnect->setText(constMetaFields.tray_quick_connect_enabled.Description());

    _ui.lbDescConversationalAwareness->setText(
        constMetaFields.conversational_awareness.Description());
    _ui.lbDescPersonalizedVolume->setText(constMetaFields.personalized_volume.Description());
    _ui.lbDescLoudSoundReduction->setText(constMetaFields.loud_sound_reduction.Description());
    _ui.lbDescAdaptiveTransparencyLevel->setText(
        constMetaFields.adaptive_transparency_level.Description());

    // The About text carries a hard-coded link colour inside the (translated) HTML; swap it
    // for the theme accent so it stays readable in dark mode.
    const auto accent = Theme::Manager::Instance().Accent().name(QColor::HexRgb);
    _ui.label->setText(QString{_aboutTextTemplate}.replace("#0000ff", accent, Qt::CaseInsensitive));
}

void SettingsWindow::UpdateStandardButtonTexts()
{
    _ui.buttonBox->button(QDialogButtonBox::RestoreDefaults)->setText(tr("Restore Defaults"));
    _ui.buttonBox->button(QDialogButtonBox::Close)->setText(tr("Close"));
}

// Inserts the driver status card after the device card on the General page and
// wires its refresh to the AAP connection state (blueprint 1.1).
void SettingsWindow::SetupDriverStatusCard()
{
    _driverStatusCard = new DriverStatusCard{_ui.general};
    const auto deviceCardIndex = _ui.generalLayout->indexOf(_ui.cardDevice);
    _ui.generalLayout->insertWidget(deviceCardIndex + 1, _driverStatusCard);

    // No manager yet: the card stays hidden until SetAirPodsManagerProvider()
    // runs. Resolving the manager here would dereference the not-yet-created
    // application manager (the tray icon builds this window before it exists).
    _driverStatusCard->setVisible(false);

    connect(_driverStatusCard, &DriverStatusCard::OpenDriverDocsRequested, this, [] {
        QDesktopServices::openUrl(QUrl{
            QStringLiteral("https://github.com/YimingZhanshen/AirPodsWindows/blob/main/Docs/Driver.md")});
    });
    // Blueprint 1.4: one-click export of the connection step log. The report is
    // built from the recorder's redacted entries and written wherever the user
    // picks — it never leaves the machine by itself (data sovereignty).
    connect(_driverStatusCard, &DriverStatusCard::ExportDiagnosticsRequested, this, [this] {
        const auto defaultName = QString{"AirPodsWindows-diagnostics-%1.txt"}.arg(
            QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        const auto path = QFileDialog::getSaveFileName(
            this, tr("Export diagnostics"), defaultName, tr("Text files (*.txt)"));
        if (path.isEmpty()) {
            return;
        }

        QFile file{path};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, tr("Export diagnostics"),
                                 tr("Could not write the diagnostics file."));
            return;
        }

        const auto aapConnected =
            _getAirPodsManager && _getAirPodsManager().IsAAPConnected();
        const auto viaDriver =
            _getAirPodsManager && _getAirPodsManager().IsAAPConnectedViaMagicAAP();
        const auto header =
            QString{"AirPodsWindows %1\nOS: %2\ndriver: %3\naap connected: %4 (via driver: %5)"}
                .arg(
                    QStringLiteral(CONFIG_VERSION_STRING),
                    QSysInfo::prettyProductName(),
                    Core::AAP::Manager::IsMagicAAPDriverAvailable() ? tr("installed")
                                                                    : tr("not installed"),
                    aapConnected ? QStringLiteral("yes") : QStringLiteral("no"),
                    viaDriver ? QStringLiteral("yes") : QStringLiteral("no"))
                .toStdString();
        const auto report = Core::Diagnostics::Recorder::Instance().BuildReport(header);
        file.write(report.data(), static_cast<qint64>(report.size()));
    });
}

// Called by the application layer once the AirPods manager exists; see the
// header for why this cannot happen during construction.
void SettingsWindow::SetAirPodsManagerProvider(
    std::function<Core::AirPods::Manager &()> getAirPodsManager)
{
    _getAirPodsManager = std::move(getAirPodsManager);
    if (_driverStatusCard == nullptr || !_getAirPodsManager) {
        return;
    }

    _driverStatusCard->setVisible(true);
    _driverStatusCard->SetManagerProvider(_getAirPodsManager);

    if (!_driverCardSignalConnected) {
        auto &manager = _getAirPodsManager();
        connect(
            &manager, &Core::AirPods::Manager::AapConnectionStateChangedSafely, _driverStatusCard,
            [this](bool) { _driverStatusCard->Refresh(); });
        connect(
            &manager, &Core::AirPods::Manager::AapConnectionFailedSafely, _driverStatusCard,
            [this](int) { _driverStatusCard->Refresh(); });
        _driverCardSignalConnected = true;
    }
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
            { "Qt 6", "https://www.qt.io/download-qt-installer", "LGPLv3", "https://doc.qt.io/qt-6/lgpl.html" },
            { "spdlog", "https://github.com/gabime/spdlog", "MIT", "https://github.com/gabime/spdlog/blob/v1.x/LICENSE" },
            { "cxxopts", "https://github.com/jarro2783/cxxopts", "MIT", "https://github.com/jarro2783/cxxopts/blob/master/LICENSE" },
            { "cpr", "https://github.com/whoshuu/cpr", "MIT", "https://github.com/whoshuu/cpr/blob/master/LICENSE" },
            { "json", "https://github.com/nlohmann/json", "MIT", "https://github.com/nlohmann/json/blob/develop/LICENSE.MIT" },
            { "SingleApplication", "https://github.com/itay-grudev/SingleApplication", "MIT", "https://github.com/itay-grudev/SingleApplication/blob/master/LICENSE" },
            { "pfr", "https://github.com/boostorg/pfr", "BSL-1.0", "https://github.com/boostorg/pfr/blob/develop/LICENSE_1_0.txt" },
            { "magic_enum", "https://github.com/Neargye/magic_enum", "MIT", "https://github.com/Neargye/magic_enum/blob/master/LICENSE" },
            { "stacktrace", "https://github.com/boostorg/stacktrace", "BSL-1.0", "https://www.boost.org/LICENSE_1_0.txt" } // clang-format on
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
            { "librepods", "https://github.com/kavishdevar/librepods", "AAP protocol reverse engineering" },
            { "MagicAAP", "https://magicpods.app/magicaap/", "MagicAAP driver for ANC features" },
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

    auto currentLangIndex = _getCurrentLocaleIndex();
    _lastLanguageIndex = currentLangIndex;
    _ui.cbLanguages->setCurrentIndex(currentLangIndex);

    _ui.cbAutoRun->setChecked(fields.auto_run);

    _ui.cbAppearanceMode->setCurrentIndex(static_cast<int>(fields.appearance_mode));

    _ui.cbLowAudioLatency->setChecked(fields.low_audio_latency);

    _ui.cbAutoEarDetection->setChecked(fields.automatic_ear_detection);

    _ui.cbConversationalAwareness->setChecked(fields.conversational_awareness);

    _ui.sliderConversationalAwarenessVolume->setValue(fields.conversational_awareness_volume_percent);
    _ui.lblConversationalAwarenessVolumeValue->setText(QString::number(fields.conversational_awareness_volume_percent) + "%");

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

    UpdateQuickConnectDevices(fields);

    _trigger = true;
}

void SettingsWindow::UpdateQuickConnectDevices(const Fields &fields)
{
    const auto previousTrigger = _trigger;
    _trigger = false;

    const auto &selectedId = fields.tray_quick_connect_device_id;

    _ui.cbTrayQuickConnectDevice->clear();
    for (const auto &device : _quickConnect.Devices()) {
        _ui.cbTrayQuickConnectDevice->addItem(device.name, device.id);
    }

    auto selectedIndex = _ui.cbTrayQuickConnectDevice->findData(selectedId);
    if (selectedIndex == -1 && !selectedId.isEmpty()) {
        // The saved device is paired but not reporting an audio endpoint right now - switched off,
        // out of range, or the enumeration has not finished. Show it as chosen but unavailable
        // rather than an empty box that reads like nothing was ever configured.
        _ui.cbTrayQuickConnectDevice->addItem(tr("Saved device (not available)"), selectedId);
        selectedIndex = _ui.cbTrayQuickConnectDevice->count() - 1;
    }
    _ui.cbTrayQuickConnectDevice->setCurrentIndex(selectedIndex);

    _ui.cbTrayQuickConnectEnabled->setChecked(fields.tray_quick_connect_enabled);
    // Always leave the checkbox usable: a transient empty enumeration must never trap the user in
    // a state they cannot turn off.
    _ui.cbTrayQuickConnectEnabled->setEnabled(true);
    _ui.cbTrayQuickConnectDevice->setEnabled(_ui.cbTrayQuickConnectDevice->count() > 0);

    _trigger = previousTrigger;
}

void SettingsWindow::UpdateAdvOverride()
{
    auto advsStr = _ui.teAdvOverride->toPlainText();
    auto vAdvsStr = advsStr.split('\n', Qt::SkipEmptyParts);

    std::vector<std::vector<uint8_t>> advs;

    for (const auto &advStr : vAdvsStr) {

        auto advBytesStr = advStr.split(' ', Qt::SkipEmptyParts);

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

    // Enumerating paired Bluetooth devices and every audio endpoint is not free, so it happens
    // when the dialog is opened rather than on every application start.
    _quickConnect.RefreshDevices();

    // The driver can be installed (or broken by a Windows update) while the app runs;
    // re-probe the link state each time the dialog opens.
    if (_driverStatusCard != nullptr) {
        _driverStatusCard->Refresh();
    }
}

void SettingsWindow::On_cbLanguages_currentIndexChanged(int index)
{
    if (_ui.cbLanguages->count() != index + 1) {
        _lastLanguageIndex = index;

        const auto &availableLocales = Utils::AvailableLocales();
        const auto &locale = availableLocales.at(index);

        ModifiableAccess()->language_locale = locale.name();
    }
    else {
        _ui.cbLanguages->setCurrentIndex(_lastLanguageIndex);
        // clang-format off
        QDesktopServices::openUrl(QUrl{
            "https://github.com/YimingZhanshen/AirPodsWindows/blob/main/CONTRIBUTING.md#-translation-guide"
        });
        // clang-format on
    }
}

void SettingsWindow::On_cbAutoRun_toggled(bool checked)
{
    ModifiableAccess()->auto_run = checked;
}

void SettingsWindow::On_cbTrayQuickConnectEnabled_toggled(bool checked)
{
    ModifiableAccess()->tray_quick_connect_enabled = checked;
}

void SettingsWindow::On_cbTrayQuickConnectDevice_currentIndexChanged(int index)
{
    const auto id = _ui.cbTrayQuickConnectDevice->itemData(index).toString();
    ModifiableAccess()->tray_quick_connect_device_id = id;
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

void SettingsWindow::On_cbAppearanceMode_currentIndexChanged(int index)
{
    if (index < static_cast<int>(AppearanceMode::System) ||
        index > static_cast<int>(AppearanceMode::Dark))
    {
        return;
    }
    ModifiableAccess()->appearance_mode = static_cast<AppearanceMode>(index);
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

void SettingsWindow::On_sliderConversationalAwarenessVolume_valueChanged(int value)
{
    _ui.lblConversationalAwarenessVolumeValue->setText(QString::number(value) + "%");
    ModifiableAccess()->conversational_awareness_volume_percent = static_cast<uint8_t>(value);
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
