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

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Core::AAP {

// The five sensor counts exactly as the device sends them. Kept in this Qt-free header so the
// transport and presentation layers use one classifier and one parser, including their rejection
// rules. A 0x17 command shares the opcode but is not sensor data.
struct HeadTrackingData {
    std::int16_t orientation1{0};
    std::int16_t orientation2{0};
    std::int16_t orientation3{0};
    std::int16_t horizontalAcceleration{0};
    std::int16_t verticalAcceleration{0};
};

// Documented sensor records are at least 70 bytes. Fields end at byte 55, but accepting a short
// 0x17 prefix would let a truncated command masquerade as a pose.
inline constexpr std::size_t kHeadTrackingRecordSize = 70;
inline constexpr std::size_t kHeadTrackingOrientationOffset = 43;
inline constexpr std::size_t kHeadTrackingHorizontalAccelerationOffset = 51;
inline constexpr std::size_t kHeadTrackingVerticalAccelerationOffset = 53;

inline bool IsHeadTrackingStreamRecord(std::span<const std::uint8_t> packet) noexcept
{
    if (packet.size() < kHeadTrackingRecordSize) {
        return false;
    }

    static constexpr std::array<std::uint8_t, 10> kPrefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                                          0x00, 0x00, 0x00, 0x10, 0x00};
    for (std::size_t index = 0; index < kPrefix.size(); ++index) {
        if (packet[index] != kPrefix[index]) {
            return false;
        }
    }

    // 0x44/0x45 are the documented sensor subcodes. The remaining byte is reserved and must be
    // zero; accepting an arbitrary subcode would turn malformed or future commands into poses.
    return packet[11] == 0x00 && (packet[10] == 0x44 || packet[10] == 0x45);
}

inline std::int16_t
ReadHeadTrackingInt16LE(std::span<const std::uint8_t> packet, std::size_t offset) noexcept
{
    const auto bits = static_cast<std::uint16_t>(packet[offset]) |
                      (static_cast<std::uint16_t>(packet[offset + 1]) << 8);
    return static_cast<std::int16_t>(bits);
}

inline std::optional<HeadTrackingData>
ParseHeadTrackingData(std::span<const std::uint8_t> packet) noexcept
{
    if (!IsHeadTrackingStreamRecord(packet)) {
        return std::nullopt;
    }

    return HeadTrackingData{
        ReadHeadTrackingInt16LE(packet, kHeadTrackingOrientationOffset),
        ReadHeadTrackingInt16LE(packet, kHeadTrackingOrientationOffset + 2),
        ReadHeadTrackingInt16LE(packet, kHeadTrackingOrientationOffset + 4),
        ReadHeadTrackingInt16LE(packet, kHeadTrackingHorizontalAccelerationOffset),
        ReadHeadTrackingInt16LE(packet, kHeadTrackingVerticalAccelerationOffset)};
}

} // namespace Core::AAP
