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

#include "DeviceInfoCard.h"

#include <QHBoxLayout>
#include <QStyle>

#include <utility>

namespace Gui {

namespace {

constexpr const char *kLabelClass = "cardDescription";
constexpr const char *kValueClass = "cardDescription";

// Field order and label wording follow blueprint 2.1 (固件×2、硬件、型号、
// 序列号含左右耳、厂商) and the 0x001D field table in Docs/AAP-Protocol-Spec.md.
void AppendRow(
    std::vector<DeviceInfoCard::Row> &rows, const QString &label, const std::string &value)
{
    if (value.empty()) {
        return;
    }
    rows.push_back({label, QString::fromStdString(value)});
}

} // namespace

DeviceInfoCard::DeviceInfoCard(QWidget *parent) : QFrame{parent}
{
    setObjectName("cardDeviceInfo");
    setProperty("cssClass", "settingCard");
    setFrameShape(QFrame::NoFrame);

    _titleLabel->setText(tr("Device information"));
    _titleLabel->setProperty("cssClass", "cardTitle");
    // Captions carry no interaction: no link handling, no selection, no focus
    // (blueprint rule 5 applies to the whole card, not only the values).
    _titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);

    _rowsLayout->setSpacing(2);

    auto *layout = new QVBoxLayout;
    layout->setSpacing(4);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->addWidget(_titleLabel);
    layout->addLayout(_rowsLayout);
    setLayout(layout);

    ApplyTheme();
    Refresh();
}

void DeviceInfoCard::SetInfoProvider(InfoProvider provider)
{
    _getInfo = std::move(provider);
    Refresh();
}

std::vector<DeviceInfoCard::Row>
DeviceInfoCard::RowsFor(const std::optional<Core::AAP::AAPDeviceInfo> &info)
{
    if (!info.has_value()) {
        return {};
    }

    // tr() here resolves to DeviceInfoCard's static overload (Q_OBJECT), so the
    // rows land in the Gui::DeviceInfoCard translation context like the title.
    std::vector<Row> rows;
    AppendRow(rows, tr("Name"), info->name);
    AppendRow(rows, tr("Model"), info->model);
    AppendRow(rows, tr("Manufacturer"), info->manufacturer);
    AppendRow(rows, tr("Serial number"), info->serial);
    AppendRow(rows, tr("Firmware version"), info->firmware1);
    // The record carries two firmware strings that are usually identical; a
    // second row only appears when the device reports a distinct value.
    if (!info->firmware2.empty() && info->firmware2 != info->firmware1) {
        AppendRow(rows, tr("Firmware version 2"), info->firmware2);
    }
    AppendRow(rows, tr("Hardware revision"), info->hardwareRevision);
    AppendRow(rows, tr("Left earbud serial"), info->leftSerial);
    AppendRow(rows, tr("Right earbud serial"), info->rightSerial);
    return rows;
}

void DeviceInfoCard::Refresh()
{
    // Titles are rebuilt here so a language change retranslates the card
    // (SettingsWindow re-runs Refresh() from its LanguageChange hook).
    _titleLabel->setText(tr("Device information"));

    std::optional<Core::AAP::AAPDeviceInfo> info;
    if (_getInfo) {
        info = _getInfo();
    }
    const auto rows = RowsFor(info);

    ClearRows();
    // No data (no provider, disconnected, rebind, or a rejected record): the
    // card hides itself instead of rendering an empty shell.
    if (rows.empty()) {
        setVisible(false);
        return;
    }

    for (const auto &row : rows) {
        auto *rowLayout = new QHBoxLayout;
        rowLayout->setSpacing(8);

        auto *label = new QLabel{row.label, this};
        label->setProperty("cssClass", kLabelClass);
        label->setTextInteractionFlags(Qt::NoTextInteraction);

        auto *value = new QLabel{this};
        value->setTextFormat(Qt::PlainText);
        value->setText(row.value);
        value->setProperty("cssClass", kValueClass);
        value->setWordWrap(true);
        // Manual mouse selection only: no buttons, no clipboard automation,
        // no focus capture (blueprint rule 5).
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);

        rowLayout->addWidget(label, 0, Qt::AlignTop | Qt::AlignLeft);
        rowLayout->addWidget(value, 1);
        _rowsLayout->addLayout(rowLayout);
    }

    setVisible(true);
    updateGeometry();
}

void DeviceInfoCard::ClearRows()
{
    // Take each row layout out of the parented layout before deleting it, so a
    // refresh can never leave stale widgets in the settings scroll area.
    while (auto *item = _rowsLayout->takeAt(0)) {
        if (auto *subLayout = item->layout()) {
            while (auto *subItem = subLayout->takeAt(0)) {
                if (auto *widget = subItem->widget()) {
                    widget->deleteLater();
                }
                delete subItem;
            }
        }
        delete item;
    }
}

void DeviceInfoCard::ApplyTheme()
{
    // Reuses the settings-card look: the dialog's stylesheet keys off the
    // cssClass property, same as the .ui-defined cards.
    style()->unpolish(this);
    style()->polish(this);
}

} // namespace Gui
