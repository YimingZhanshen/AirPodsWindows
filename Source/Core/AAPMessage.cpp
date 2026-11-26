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

#include "AAPMessage.h"

#include <algorithm>
#include <iterator>

#include "AAPAtt.h"

namespace Core::AAP {

namespace {

bool HasAapHeader(std::span<const uint8_t> record)
{
    return record.size() >= 4 && record[0] == 0x04 && record[1] == 0x00 && record[2] == 0x04 &&
           record[3] == 0x00;
}

bool HasHandshakeHeader(std::span<const uint8_t> record)
{
    return record.size() >= 4 && record[0] == 0x00 && record[1] == 0x00 && record[2] == 0x04 &&
           record[3] == 0x00;
}

bool IsHandshakeAck(std::span<const uint8_t> record)
{
    return record.size() == 4 && record[0] == 0x01 && record[1] == 0x00 && record[2] == 0x04 &&
           record[3] == 0x00;
}

bool IsHandshake(std::span<const uint8_t> record)
{
    constexpr uint8_t handshake[] = {0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    return record.size() == std::size(handshake) &&
           std::equal(record.begin(), record.end(), handshake);
}

AAPMessage::ValidationResult ValidateAtt(std::span<const uint8_t> record)
{
    size_t minimumSize = 0;
    switch (record[0]) {
    case Att::kOpcodeErrorResponse:
        minimumSize = 5;
        break;
    case Att::kOpcodeReadRequest:
        minimumSize = 3;
        break;
    case Att::kOpcodeReadResponse:
        minimumSize = 2;
        break;
    case Att::kOpcodeWriteCommand:
        minimumSize = 3;
        break;
    case Att::kOpcodeHandleValueNotification:
        minimumSize = 3;
        break;
    default:
        return AAPMessage::ValidationResult::InvalidAttOpcode;
    }
    return record.size() >= minimumSize ? AAPMessage::ValidationResult::Valid
                                        : AAPMessage::ValidationResult::TruncatedAtt;
}

} // namespace

AAPMessage::ValidationResult AAPMessage::Validate(std::span<const uint8_t> record) noexcept
{
    if (record.empty()) {
        return ValidationResult::Empty;
    }
    if (record.size() > kMaxRecordSize) {
        return ValidationResult::Oversize;
    }
    // The four-byte handshake ACK shares its prefix with the five-byte ATT
    // Error Response, but it is a complete AAP record in its own right.
    if (IsHandshakeAck(record)) {
        return ValidationResult::Valid;
    }
    if (record.size() < 4) {
        if (record[0] == 0x00 || record[0] == 0x04) {
            return ValidationResult::Truncated;
        }
        return ValidateAtt(record);
    }

    if (HasHandshakeHeader(record)) {
        return IsHandshake(record) ? ValidationResult::Valid : ValidationResult::TruncatedAap;
    }
    if (!HasAapHeader(record)) {
        return ValidateAtt(record);
    }

    if (record.size() < 6) {
        return ValidationResult::Truncated;
    }

    const uint16_t opcode = static_cast<uint16_t>(record[4] | (record[5] << 8));
    size_t minimumSize = 6;
    switch (opcode) {
    case 0x0004: // Battery: count byte followed by five bytes per component.
        if (record.size() < 7 || record[6] == 0 || record[6] > 3) {
            return ValidationResult::TruncatedAap;
        }
        minimumSize = 7 + static_cast<size_t>(record[6]) * 5;
        break;
    case 0x0006: // Ear detection.
    case 0x0019: // Stem event.
        minimumSize = 8;
        break;
    case 0x0009: // Settings command/echo envelope: header, id, state, and two trailing bytes.
        minimumSize = 11;
        break;
    case 0x000E: // Audio source.
        minimumSize = 13;
        break;
    case 0x000F: // Notification subscription mask.
        minimumSize = 10;
        break;
    case 0x0017: // Head tracking commands are 28 bytes; identified sensor records are >=70.
        minimumSize = 28;
        if (record.size() >= 12 && record[11] == 0x00 && (record[10] == 0x44 || record[10] == 0x45))
        {
            minimumSize = 70;
        }
        break;
    case 0x001A: // Rename: fixed prefix, then the declared name length.
        if (record.size() < 9) {
            return ValidationResult::TruncatedAap;
        }
        minimumSize = 9 + static_cast<size_t>(record[7]);
        break;
    case 0x001D: // Device information.
        minimumSize = 11;
        break;
    case 0x002E: // Smart routing: count byte followed by eight bytes per entry.
        if (record.size() < 9) {
            return ValidationResult::TruncatedAap;
        }
        minimumSize = 9 + static_cast<size_t>(record[8]) * 8;
        break;
    case 0x0030: // Magic cloud key request.
        minimumSize = 8;
        break;
    case 0x0031: // Magic cloud key response, including both 16-byte key blocks.
        minimumSize = 47;
        break;
    case 0x004B: // Speaking level.
        minimumSize = 10;
        break;
    case 0x004D: // Feature flags.
        minimumSize = 14;
        break;
    default:
        // A valid AAP header with an unknown opcode is forwarded to the
        // protocol layer, which logs it without applying unrelated parsing.
        return ValidationResult::Valid;
    }

    return record.size() >= minimumSize ? ValidationResult::Valid : ValidationResult::TruncatedAap;
}

std::string_view AAPMessage::Describe(ValidationResult result) noexcept
{
    switch (result) {
    case ValidationResult::Valid:
        return "valid record";
    case ValidationResult::Empty:
        return "empty record";
    case ValidationResult::Oversize:
        return "record exceeds 4096 bytes";
    case ValidationResult::Truncated:
        return "truncated AAP header";
    case ValidationResult::InvalidAttOpcode:
        return "unknown ATT opcode";
    case ValidationResult::TruncatedAtt:
        return "truncated ATT record";
    case ValidationResult::TruncatedAap:
        return "truncated AAP record";
    }
    return "invalid record";
}

} // namespace Core::AAP
