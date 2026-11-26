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

#include "AAPBattery.h"

namespace Core::AAP {

namespace {

// 6-byte header plus the count byte at index 6.
constexpr size_t kPreambleSize = 7;
constexpr size_t kComponentSize = 5;
constexpr uint8_t kMaxComponents = 3;
constexpr uint8_t kSpacer = 0x01;
constexpr uint8_t kEndMarker = 0x01;
// Android-only status value (librepods data/Packets.kt): a charge-limit state, still plugged in.
constexpr uint8_t kStatusOptimizedCharging = 0x05;

std::optional<AAPBatteryReading> ParseComponent(std::span<const uint8_t> component)
{
    const uint8_t id = component[0];
    if (component[1] != kSpacer || component[4] != kEndMarker) {
        return std::nullopt;
    }

    BatteryComponent parsedComponent;
    switch (id) {
    case static_cast<uint8_t>(BatteryComponent::Right):
        parsedComponent = BatteryComponent::Right;
        break;
    case static_cast<uint8_t>(BatteryComponent::Left):
        parsedComponent = BatteryComponent::Left;
        break;
    case static_cast<uint8_t>(BatteryComponent::Case):
        parsedComponent = BatteryComponent::Case;
        break;
    default:
        // Headset (0x01) and anything else are not presented by this parser.
        return std::nullopt;
    }

    const uint8_t level = component[2];
    if (level > 100) {
        return std::nullopt;
    }

    BatteryStatus status;
    switch (component[3]) {
    case static_cast<uint8_t>(BatteryStatus::Unknown):
        status = BatteryStatus::Unknown;
        break;
    case static_cast<uint8_t>(BatteryStatus::Charging):
        status = BatteryStatus::Charging;
        break;
    case static_cast<uint8_t>(BatteryStatus::Discharging):
        status = BatteryStatus::Discharging;
        break;
    case static_cast<uint8_t>(BatteryStatus::Disconnected):
        status = BatteryStatus::Disconnected;
        break;
    case kStatusOptimizedCharging:
        status = BatteryStatus::Charging;
        break;
    default:
        return std::nullopt;
    }

    return AAPBatteryReading{parsedComponent, level, status};
}

} // namespace

std::optional<AAPBatterySnapshot> ParseBatteryNotification(std::span<const uint8_t> packet)
{
    if (packet.size() < kPreambleSize) {
        return std::nullopt;
    }
    // Same header rule as IsBatteryNotification, without the vector round-trip.
    if (packet[0] != 0x04 || packet[1] != 0x00 || packet[2] != 0x04 || packet[3] != 0x00 ||
        packet[4] != 0x04 || packet[5] != 0x00)
    {
        return std::nullopt;
    }

    const size_t count = packet[6];
    if (count < 1 || count > kMaxComponents) {
        return std::nullopt;
    }
    // The transport may keep longer records alive for other opcodes; a battery
    // notification must be exactly one header plus five bytes per component.
    if (packet.size() != kPreambleSize + count * kComponentSize) {
        return std::nullopt;
    }

    AAPBatterySnapshot snapshot;
    for (size_t index = 0; index < count; ++index) {
        const auto component =
            ParseComponent(packet.subspan(kPreambleSize + index * kComponentSize, kComponentSize));
        if (!component.has_value()) {
            return std::nullopt;
        }

        auto &slot = component->component == BatteryComponent::Left
                         ? snapshot.left
                         : (component->component == BatteryComponent::Right ? snapshot.right
                                                                            : snapshot.caseBox);
        // A repeated component in one notification is a protocol violation; do not guess.
        if (slot.has_value()) {
            return std::nullopt;
        }
        slot = component;
    }

    return snapshot;
}

} // namespace Core::AAP
