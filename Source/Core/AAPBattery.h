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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Core::AAP {

enum class BatteryComponent : uint8_t { Right = 0x02, Left = 0x04, Case = 0x08, Unknown = 0xFF };

enum class BatteryStatus : uint8_t {
    Unknown = 0x00,
    Charging = 0x01,
    Discharging = 0x02,
    Disconnected = 0x04
};

// One battery component inside an AAP battery notification (opcode 0x0004).
// `level` is reported in 1% steps, unlike the 10%-quantized BLE advertisements.
// See Docs/AAP-Protocol-Spec.md §1.
struct AAPBatteryReading {
    BatteryComponent component{BatteryComponent::Unknown};
    uint8_t level{0}; // 0-100 (percent)
    BatteryStatus status{BatteryStatus::Unknown};
};

// A parsed battery notification. Every component the packet carried is set; a
// component the device did not report stays nullopt. The case member is named
// `caseBox` to mirror `State::caseBox`, because `case` is a keyword.
struct AAPBatterySnapshot {
    std::optional<AAPBatteryReading> left;
    std::optional<AAPBatteryReading> right;
    std::optional<AAPBatteryReading> caseBox;
};

// Strictly parses a battery notification record:
// `04 00 04 00 04 00 <count> count * [id, 0x01 spacer, level, status, 0x01 end]`.
// - count must be 1-3 and the record length must be exactly 7 + 5 * count;
// - component ids must be distinct values from {Right 0x02, Left 0x04, Case 0x08}
//   (the Linux-only Headset id 0x01 is not covered; those models keep BLE);
// - spacer and end bytes must both be 0x01;
// - level must be <= 100 and status one of 0x00/0x01/0x02/0x04, or 0x05
//   (Android's Optimized Charging, surfaced as Charging).
// Any violation returns nullopt so callers keep their previous readings.
std::optional<AAPBatterySnapshot> ParseBatteryNotification(std::span<const uint8_t> packet);

} // namespace Core::AAP
