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
#include <optional>
#include <vector>

#include <QFrame>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>

#include "../../Core/AAPDeviceInfo.h"

namespace Gui {

// Settings-page card showing the device information pushed by the accessory at
// AAP connect time (blueprint 2.1): name, model, manufacturer, firmware and
// hardware versions, and the case/left/right serial numbers.
//
// Privacy contract (blueprint rule 5): values are display-only. The card never
// copies anything to the clipboard automatically, never logs, and the whole
// card is hidden while the manager cache is empty (disconnected device, wrong
// firmware without 0x001D, or a rejected record). Values stay selectable by
// mouse only, so any copy is an explicit user action.
class DeviceInfoCard : public QFrame
{
    Q_OBJECT

public:
    using InfoProvider = std::function<std::optional<Core::AAP::AAPDeviceInfo>()>;

    struct Row {
        QString label;
        QString value;

        bool operator==(const Row &rhs) const = default;
    };

    explicit DeviceInfoCard(QWidget *parent = nullptr);

    // Provider pulls the manager cache lazily on the GUI thread; an empty
    // provider keeps the card inert (hidden), matching the DriverStatusCard
    // pattern for unit tests without a running manager.
    void SetInfoProvider(InfoProvider provider);
    void Refresh();

    // Translated label/value rows for the given record; empty fields are
    // omitted, and duplicate firmware revisions collapse into one row. Shared
    // with the tests so assertions do not depend on widget internals.
    static std::vector<Row> RowsFor(const std::optional<Core::AAP::AAPDeviceInfo> &info);

private:
    QLabel *_titleLabel = new QLabel{this};
    QVBoxLayout *_rowsLayout = new QVBoxLayout;

    InfoProvider _getInfo;

    void ClearRows();
    void ApplyTheme();
};

} // namespace Gui
