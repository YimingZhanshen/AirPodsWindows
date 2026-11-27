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

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Core::AAP {

// Device metadata carried by the AAP device-information record (opcode 0x001D).
// The accessory pushes this record once upon connection; it cannot be requested
// (librepods docs/device-info.md, Docs/AAP-Protocol-Spec.md §3). Field order and
// names follow the documented layout; values are UTF-8 (see ParseDeviceInfo).
struct AAPDeviceInfo {
    std::string name;
    std::string model;
    std::string manufacturer;
    std::string serial;
    std::string firmware1;
    std::string firmware2;
    std::string hardwareRevision;
    std::string leftSerial;
    std::string rightSerial;

    bool operator==(const AAPDeviceInfo &rhs) const = default;
};

// Pure 0x001D record parser, validated against the 222-byte real capture in
// librepods "AAP Definitions.md" (offset table in Docs/AAP-Protocol-Spec.md §3):
//   offset 0-3   header `04 00 04 00`
//   offset 4-5   little-endian opcode `1D 00`
//   offset 6-10  unknown preamble bytes; every observed capture carries the NUL
//                skeleton `?? ?? 00 ?? 00`, and the first string starts at 11
//   offset 11..  NUL-separated UTF-8 text fields — only the first 11 are read:
//     name, model, manufacturer, serial, firmware v1, firmware v2, hardware
//     revision, updater identifier, left-bud serial, right-bud serial, numeric
//     version. Everything after the 11th field is the trailing encrypted blob
//     and is deliberately ignored.
//
// Encoding evidence (UTF-8, not UTF-16): librepods "AAP Definitions.md" line
// "The strings are null-terminated UTF-8 strings", the byte-exact ASCII sample
// packet, `QString::fromUtf8` in linux/main.cpp parseMetadata, and the UTF-8
// rename packets this app sends (0x001A), whose round-tripped names appear here.
//
// Every malformed record returns std::nullopt (wrong header/opcode, broken
// preamble skeleton, fewer than 11 NUL-delimited fields, invalid UTF-8, or
// control bytes in a field) so callers keep their previous cache. Empty fields
// are valid devices sending no value and map to empty strings. The parser never
// logs or copies the raw packet.
std::optional<AAPDeviceInfo> ParseDeviceInfo(std::span<const uint8_t> packet);

} // namespace Core::AAP
