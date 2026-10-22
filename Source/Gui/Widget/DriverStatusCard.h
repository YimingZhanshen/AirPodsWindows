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

#pragma once

#include <functional>

#include <QFrame>
#include <QLabel>
#include <QPushButton>

#include "../Core/AirPods.h"

namespace Gui {

// Settings-page card showing the MagicAAP driver link status (blueprint 1.1).
// The driver is a hard requirement for every AAP/ANC feature yet was previously
// invisible: users had no way to tell "driver missing" from "device away".
// Four states, each with an actionable next step:
//   1. driver not installed  -> copy the install command / open the docs
//   2. driver installed, AAP not connected -> bring the buds close / open the lid
//   3. AAP connected via the driver (full feature set)
//   4. AAP connected via another channel (native socket fallback; driver-only
//      features such as loud sound reduction may not work)
class DriverStatusCard : public QFrame
{
    Q_OBJECT

public:
    enum class LinkStatus : int {
        NotInstalled,
        InstalledNotConnected,
        ConnectedViaDriver,
        ConnectedOtherChannel,
    };

    explicit DriverStatusCard(QWidget *parent = nullptr);

    // Provider resolves the application's AirPods manager lazily; the card is
    // inert (hidden) when the provider is empty — e.g. in GUI unit tests.
    void SetManagerProvider(std::function<Core::AirPods::Manager &()> provider);
    void Refresh();

    // Rendered status text for the given state; also used by tests.
    static QString StatusText(LinkStatus status);
    static QString StatusDetailText(
        LinkStatus status, Core::AAP::AttPathState attPathState,
        Core::AAP::ConnectFailure lastFailure);

    // Maps a ConnectFailure value to a user-facing hint ("what to do next").
    static QString FailureHint(Core::AAP::ConnectFailure failure);

Q_SIGNALS:
    // Emitted when the user asks for the install command or the docs; the
    // settings window owns the clipboard/system integration.
    void CopyInstallCommandRequested();
    void OpenDriverDocsRequested();

private:
    QLabel *_titleLabel = new QLabel{this};
    QLabel *_descLabel = new QLabel{this};
    QLabel *_detailLabel = new QLabel{this};
    QPushButton *_copyButton = new QPushButton{tr("Copy install command"), this};
    QPushButton *_docsButton = new QPushButton{tr("Open documentation"), this};

    std::function<Core::AirPods::Manager &()> _getManager;
    LinkStatus _status{LinkStatus::NotInstalled};

    void ApplyTheme();
};
} // namespace Gui
