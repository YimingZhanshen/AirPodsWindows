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

#include "AAPAtt.h"

namespace Core::AAP::Att {

std::vector<uint8_t> BuildWriteCommand(uint16_t handle, uint8_t value)
{
    return {
        kOpcodeWriteCommand,
        static_cast<uint8_t>(handle & 0xFF),
        static_cast<uint8_t>((handle >> 8) & 0xFF),
        value
    };
}

std::vector<uint8_t> BuildReadRequest(uint16_t handle)
{
    return {
        kOpcodeReadRequest,
        static_cast<uint8_t>(handle & 0xFF),
        static_cast<uint8_t>((handle >> 8) & 0xFF)
    };
}

bool IsReadResponse(const std::vector<uint8_t>& data)
{
    return data.size() >= 2 && data[0] == kOpcodeReadResponse;
}

std::optional<std::vector<uint8_t>> ParseReadResponse(const std::vector<uint8_t>& data)
{
    if (!IsReadResponse(data)) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(data.begin() + 1, data.end());
}

bool IsNotification(const std::vector<uint8_t>& data)
{
    return data.size() >= 3 && data[0] == kOpcodeHandleValueNotification;
}

std::optional<std::pair<uint16_t, std::vector<uint8_t>>> ParseNotification(
    const std::vector<uint8_t>& data)
{
    if (!IsNotification(data)) {
        return std::nullopt;
    }
    uint16_t handle = static_cast<uint16_t>(data[1] | (data[2] << 8));
    return std::make_pair(handle, std::vector<uint8_t>(data.begin() + 3, data.end()));
}

bool IsErrorResponse(const std::vector<uint8_t>& data)
{
    return data.size() >= 5 && data[0] == kOpcodeErrorResponse;
}

bool IsAttFrame(const std::vector<uint8_t>& data)
{
    if (data.empty()) {
        return false;
    }
    // Every L2CAP AAP frame starts with `04 00 04 00` (or `00 00 04 00` for the
    // handshake), so those never belong to the ATT path.
    if (data.size() >= 4 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
        data[3] == 0x00) {
        return false;
    }
    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x04 &&
        data[3] == 0x00) {
        return false;
    }
    return IsReadResponse(data) || IsNotification(data) || IsErrorResponse(data);
}

std::vector<uint8_t> BuildLoudSoundReductionWrite(bool enable)
{
    return BuildWriteCommand(
        kLoudSoundReductionHandle, static_cast<uint8_t>(enable ? 0x01 : 0x00));
}

} // namespace Core::AAP::Att
