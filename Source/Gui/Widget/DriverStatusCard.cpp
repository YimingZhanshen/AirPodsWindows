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

#include "DriverStatusCard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QStyle>
#include <QVBoxLayout>

namespace Gui {

namespace {

// Installation command mirrors Docs/Driver.md; the driver is maintained by the
// MagicPods project, this app only depends on the interface.
constexpr const char *kInstallCommand =
    "irm \"https://magicpods.app/utils/magicaap-community-v1.ps1\" | iex";

} // namespace

DriverStatusCard::DriverStatusCard(QWidget *parent) : QFrame{parent}
{
    setObjectName("cardDriverStatus");
    setProperty("cssClass", "settingCard");
    setFrameShape(QFrame::NoFrame);

    _titleLabel->setText(tr("Driver (MagicAAP)"));
    _titleLabel->setProperty("cssClass", "cardTitle");

    _descLabel->setWordWrap(true);
    _descLabel->setProperty("cssClass", "cardDescription");

    _detailLabel->setWordWrap(true);
    _detailLabel->setProperty("cssClass", "cardDescription");

    _copyButton->setAutoDefault(false);
    _docsButton->setAutoDefault(false);
    _exportButton->setAutoDefault(false);

    auto *textLayout = new QVBoxLayout;
    textLayout->setSpacing(2);
    textLayout->addWidget(_titleLabel);
    textLayout->addWidget(_descLabel);
    textLayout->addWidget(_detailLabel);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(8);
    buttonLayout->addWidget(_copyButton);
    buttonLayout->addWidget(_docsButton);
    buttonLayout->addWidget(_exportButton);

    auto *layout = new QHBoxLayout;
    layout->setSpacing(16);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->addLayout(textLayout, 1);
    layout->addLayout(buttonLayout, 0);
    setLayout(layout);

    connect(_copyButton, &QPushButton::clicked, this, [this] {
        if (auto *clipboard = QGuiApplication::clipboard(); clipboard != nullptr) {
            clipboard->setText(QString::fromUtf8(kInstallCommand));
        }
        emit CopyInstallCommandRequested();
    });
    connect(_docsButton, &QPushButton::clicked, this,
            [this] { emit OpenDriverDocsRequested(); });
    connect(_exportButton, &QPushButton::clicked, this,
            [this] { emit ExportDiagnosticsRequested(); });

    ApplyTheme();
    Refresh();
}

void DriverStatusCard::SetManagerProvider(std::function<Core::AirPods::Manager &()> provider)
{
    _getManager = std::move(provider);
    Refresh();
}

QString DriverStatusCard::StatusText(LinkStatus status)
{
    switch (status) {
        case LinkStatus::NotInstalled:
            return QObject::tr("Driver not installed");
        case LinkStatus::InstalledNotConnected:
            return QObject::tr("Driver ready - waiting for the earbuds");
        case LinkStatus::ConnectedViaDriver:
            return QObject::tr("Connected (driver channel)");
        case LinkStatus::ConnectedOtherChannel:
            return QObject::tr("Connected (native channel - driver features limited)");
    }
    return QObject::tr("Unknown");
}

QString DriverStatusCard::StatusDetailText(
    LinkStatus status, Core::AAP::AttPathState attPathState, Core::AAP::ConnectFailure lastFailure)
{
    switch (status) {
        case LinkStatus::NotInstalled:
            return QObject::tr(
                "Install the driver to unlock ANC control, loud sound reduction and other "
                "AAP features. Copy the install command or open the documentation.");
        case LinkStatus::InstalledNotConnected: {
            if (lastFailure == Core::AAP::ConnectFailure::None) {
                return QObject::tr("Open the earbud case near this computer to connect.");
            }
            return FailureHint(lastFailure);
        }
        case LinkStatus::ConnectedViaDriver:
            if (attPathState == Core::AAP::AttPathState::Confirmed) {
                return QObject::tr("All driver features available.");
            }
            if (attPathState == Core::AAP::AttPathState::Unconfirmed) {
                return QObject::tr(
                    "The earbuds do not answer attribute reads on this channel; loud sound "
                    "reduction state cannot be read back.");
            }
            return QObject::tr("All driver features available.");
        case LinkStatus::ConnectedOtherChannel:
            return QObject::tr(
                "Connected without the driver channel; features that require the driver "
                "(loud sound reduction, precise battery) may not work. See the documentation.");
    }
    return {};
}

QString DriverStatusCard::FailureHint(Core::AAP::ConnectFailure failure)
{
    switch (failure) {
        case Core::AAP::ConnectFailure::None:
            return {};
        case Core::AAP::ConnectFailure::DriverMissing:
            return QObject::tr(
                "Last connection failed: the MagicAAP driver is not installed or not running. "
                "Install it (see documentation), then reconnect.");
        case Core::AAP::ConnectFailure::ServiceUnavailable:
            return QObject::tr(
                "Last connection failed: the earbuds did not offer the ANC service. Make sure "
                "they are paired, in range, and the case is open.");
        case Core::AAP::ConnectFailure::TransportError:
            return QObject::tr(
                "Last connection failed: the Bluetooth transport could not be opened. Check "
                "that Bluetooth is on and the earbuds are close to this computer.");
        case Core::AAP::ConnectFailure::HandshakeFailure:
            return QObject::tr(
                "Last connection failed during the protocol handshake. Move the earbuds closer "
                "and reconnect; see the documentation if it persists.");
    }
    return {};
}

void DriverStatusCard::Refresh()
{
    LinkStatus status = LinkStatus::NotInstalled;
    Core::AAP::AttPathState attPathState = Core::AAP::AttPathState::Unknown;
    Core::AAP::ConnectFailure lastFailure = Core::AAP::ConnectFailure::None;

    if (_getManager) {
        auto &manager = _getManager();
        const bool driverAvailable = Core::AAP::Manager::IsMagicAAPDriverAvailable();
        const bool aapConnected = manager.IsAAPConnected();

        if (!driverAvailable) {
            status = LinkStatus::NotInstalled;
        } else if (!aapConnected) {
            status = LinkStatus::InstalledNotConnected;
        } else if (manager.IsAAPConnectedViaMagicAAP()) {
            status = LinkStatus::ConnectedViaDriver;
        } else {
            status = LinkStatus::ConnectedOtherChannel;
        }
        attPathState = manager.GetAAPAttPathState();
        lastFailure = manager.GetLastAAPConnectFailure();
    }

    _status = status;
    _descLabel->setText(StatusText(status));
    _detailLabel->setText(StatusDetailText(status, attPathState, lastFailure));

    // The install command only helps when the driver is missing; the docs stay.
    _copyButton->setVisible(status == LinkStatus::NotInstalled);
    _docsButton->setVisible(status != LinkStatus::ConnectedViaDriver);
}

void DriverStatusCard::ApplyTheme()
{
    // Reuses the settings-card look: the dialog's stylesheet keys off the
    // cssClass property, same as the .ui-defined cards.
    style()->unpolish(this);
    style()->polish(this);
}

} // namespace Gui
