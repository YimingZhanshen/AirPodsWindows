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

#include "AAPDeviceInfo.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace Core::AAP {

namespace {

// Header + opcode + the shortest observed preamble that still opens the first
// string (AAPMessage validates the same 11-byte floor for opcode 0x001D).
constexpr size_t kPacketMinimumSize = 11;
constexpr size_t kFieldsOffset = 11;
constexpr size_t kFieldCount = 11;

// Unicode format controls and separators are display-hostile even inside plain text:
// bidi overrides can reorder the displayed serial, zero-width characters can spoof
// adjacent fields, and line/paragraph separators can break the card's single-line rows.
bool IsDisplayControlOrSeparator(uint32_t codePoint)
{
    return codePoint == 0x00AD || (codePoint >= 0x0600 && codePoint <= 0x0605) ||
           codePoint == 0x061C || codePoint == 0x06DD || codePoint == 0x070F ||
           codePoint == 0x08E2 || codePoint == 0x180E ||
           (codePoint >= 0x200B && codePoint <= 0x200F) ||
           (codePoint >= 0x202A && codePoint <= 0x202E) ||
           (codePoint >= 0x2060 && codePoint <= 0x2064) ||
           (codePoint >= 0x2066 && codePoint <= 0x206F) || codePoint == 0x2028 ||
           codePoint == 0x2029 || (codePoint >= 0xFEFF && codePoint <= 0xFFFB) ||
           codePoint == 0x110BD || codePoint == 0x110CD ||
           (codePoint >= 0x13430 && codePoint <= 0x1343F) ||
           (codePoint >= 0x1BCA0 && codePoint <= 0x1BCA3) ||
           (codePoint >= 0x1D173 && codePoint <= 0x1D17A) || codePoint == 0xE0001 ||
           (codePoint >= 0xE0020 && codePoint <= 0xE007F);
}

// Strict UTF-8 with the printable subset the device-information contract uses:
// rejects every C0/C1 control code, DEL, Unicode display controls and separators,
// stray or truncated continuation bytes, overlong forms, surrogates, and code points
// above U+10FFFF. The binary blob behind the eleventh field can therefore never
// masquerade as display text.
bool IsDisplayableUtf8(std::span<const uint8_t> text)
{
    for (size_t i = 0; i < text.size(); ++i) {
        const uint8_t lead = text[i];
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7F) {
                return false;
            }
            continue;
        }

        size_t extra = 0;
        uint32_t codePoint = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            codePoint = lead & 0x1F;
        }
        else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            codePoint = lead & 0x0F;
        }
        else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            codePoint = lead & 0x07;
        }
        else {
            // Stray continuation byte (0x80-0xBF) or a lead byte above U+10FFFF.
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            const uint8_t continuation = text[i + k];
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        i += extra;

        const bool overlong = (extra == 1 && codePoint < 0x80) ||
                              (extra == 2 && codePoint < 0x800) ||
                              (extra == 3 && codePoint < 0x10000);
        const bool surrogate = codePoint >= 0xD800 && codePoint <= 0xDFFF;
        // codePoint < 0xA0 also rejects the C1 block encoded as valid two-byte
        // UTF-8 (the overlong check alone would let U+0080-U+009F through).
        if (overlong || surrogate || codePoint > 0x10FFFF || codePoint < 0xA0 ||
            IsDisplayControlOrSeparator(codePoint))
        {
            return false;
        }
    }
    return true;
}

size_t IndexOfNul(std::span<const uint8_t> data)
{
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == 0x00) {
            return i;
        }
    }
    return std::string_view::npos;
}

} // namespace

std::optional<AAPDeviceInfo> ParseDeviceInfo(std::span<const uint8_t> packet)
{
    if (packet.size() < kPacketMinimumSize) {
        return std::nullopt;
    }
    if (packet[0] != 0x04 || packet[1] != 0x00 || packet[2] != 0x04 || packet[3] != 0x00) {
        return std::nullopt;
    }
    // Opcodes travel little-endian (AAPMessage::Validate uses the same order).
    if (packet[4] != 0x1D || packet[5] != 0x00) {
        return std::nullopt;
    }
    // The five-byte preamble is fixed in the only documented 222-byte capture and
    // in Docs/AAP-Protocol-Spec.md §3; string offsets are meaningless without it.
    constexpr std::array<uint8_t, 5> kPreamble{0x02, 0xD5, 0x00, 0x04, 0x00};
    if (!std::equal(kPreamble.begin(), kPreamble.end(), packet.begin() + 6)) {
        return std::nullopt;
    }

    std::array<std::string, kFieldCount> fields{};
    size_t pos = kFieldsOffset;
    for (size_t i = 0; i < kFieldCount; ++i) {
        const auto rest = packet.subspan(std::min(pos, packet.size()));
        const size_t separator = IndexOfNul(rest);
        // All eleven fields must be delimited. In particular, a record ending
        // inside the eleventh value is truncated, not a valid metadata record.
        if (separator == std::string_view::npos) {
            return std::nullopt;
        }
        const size_t length = separator;
        const auto segment = rest.subspan(0, length);
        if (!IsDisplayableUtf8(segment)) {
            return std::nullopt;
        }
        fields[i].assign(reinterpret_cast<const char *>(segment.data()), segment.size());
        pos += length + 1;
    }

    AAPDeviceInfo info;
    info.name = std::move(fields[0]);
    info.model = std::move(fields[1]);
    info.manufacturer = std::move(fields[2]);
    info.serial = std::move(fields[3]);
    info.firmware1 = std::move(fields[4]);
    info.firmware2 = std::move(fields[5]);
    info.hardwareRevision = std::move(fields[6]);
    // fields[7] is the updater app identifier and fields[10] an unnamed numeric
    // version: validated as text, but not surfaced (blueprint 2.1 card fields).
    info.leftSerial = std::move(fields[8]);
    info.rightSerial = std::move(fields[9]);
    return info;
}

} // namespace Core::AAP
