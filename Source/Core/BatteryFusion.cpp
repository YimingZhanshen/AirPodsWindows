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

#include "BatteryFusion.h"

#include <cmath>

namespace Core::AirPods::Details {

void BatteryFusion::Reset()
{
    _left = {};
    _right = {};
    _caseBox = {};
}

bool BatteryFusion::Apply(Slot &slot, const AAP::AAPBatteryReading &reading)
{
    if (reading.status == AAP::BatteryStatus::Disconnected) {
        // Component-level fallback to BLE; a later connection starts fresh.
        const bool wasPresented = slot.presented;
        slot = {};
        return wasPresented;
    }

    const bool charging = reading.status == AAP::BatteryStatus::Charging;

    if (!slot.presented) {
        // First AAP value of this session for the component: AAP wins right away,
        // the session priority is not something a candidate may revoke.
        slot = {true, reading.level, charging, std::nullopt};
        return true;
    }

    // Rises and small drops are trustworthy without confirmation; large drops must
    // repeat the exact same candidate in the next snapshot before they are shown.
    const bool acceptDirectly = reading.level + kMaxImmediateDrop >= slot.level; // int-safe
    if (!acceptDirectly && slot.pendingDrop != reading.level) {
        slot.pendingDrop = reading.level;
        return false;
    }

    const bool changed = slot.level != reading.level || slot.charging != charging;
    slot.level = reading.level;
    slot.charging = charging;
    slot.pendingDrop.reset();
    return changed;
}

bool BatteryFusion::OnAapSnapshot(const AAP::AAPBatterySnapshot &snapshot)
{
    bool changed = false;
    if (snapshot.left.has_value()) {
        changed |= Apply(_left, *snapshot.left);
    }
    if (snapshot.right.has_value()) {
        changed |= Apply(_right, *snapshot.right);
    }
    if (snapshot.caseBox.has_value()) {
        changed |= Apply(_caseBox, *snapshot.caseBox);
    }
    return changed;
}

std::optional<BatteryFusion::Reading>
BatteryFusion::Resolve(AAP::BatteryComponent component, std::optional<uint8_t> bleLevel) const
{
    const Slot *slot = slotFor(component);
    if (slot == nullptr || !slot->presented) {
        return std::nullopt;
    }

    Reading reading;
    reading.level = slot->level;
    reading.charging = slot->charging;
    // BLE has no value to contradict the precise AAP reading; a difference beyond
    // one advertisement bucket means the two feeds really disagree.
    reading.conflict =
        bleLevel.has_value() &&
        std::abs(static_cast<int>(slot->level) - static_cast<int>(*bleLevel)) > kBleBucketPercent;
    return reading;
}

bool BatteryFusion::Presents(AAP::BatteryComponent component) const
{
    const Slot *slot = slotFor(component);
    return slot != nullptr && slot->presented;
}

const BatteryFusion::Slot *BatteryFusion::slotFor(AAP::BatteryComponent component) const
{
    switch (component) {
    case AAP::BatteryComponent::Left:
        return &_left;
    case AAP::BatteryComponent::Right:
        return &_right;
    case AAP::BatteryComponent::Case:
        return &_caseBox;
    default:
        return nullptr;
    }
}

} // namespace Core::AirPods::Details
