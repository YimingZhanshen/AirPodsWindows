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
#include <utility>
#include <vector>

// ATT (Attribute Protocol) frame helpers for the AAP channel.
//
// The loud-sound-reduction (headphone safety) setting is not an L2CAP control
// command: it is an ATT Write Command to GATT handle 0x1B (`12 1B 00 [01|00]`).
// LibrePods sends ATT PDUs on a separate L2CAP socket (PSM 0x001F); on Windows
// no third-party implementation sends ATT at all, so whether the firmware
// accepts these frames written into the MagicAAP driver pipe (PSM 0x1001) is
// UNVERIFIED hardware behavior — Manager probes the path once per connection
// with a read request and reports the result (blueprint item 0.3).
//
// Frame layouts (opcode, then little-endian handle, then value):
//   Write Command            12 [handle u16] [value]
//   Read Request             0A [handle u16]
//   Read Response            0B [value]           (no handle — attribution is
//                                                  by the pending request)
//   Handle Value Notification 1B [handle u16] [value]
//   Error Response           01 [request opcode] [handle u16] [error code]
//
namespace Core::AAP::Att {

// The only ATT handle this project touches: headphone safety / loud sound
// reduction. Its CCCD (0x1C) does not work, so the state can only be read back,
// never subscribed to.
inline constexpr uint16_t kLoudSoundReductionHandle = 0x1B;

inline constexpr uint8_t kOpcodeWriteCommand = 0x12;
inline constexpr uint8_t kOpcodeReadRequest = 0x0A;
inline constexpr uint8_t kOpcodeReadResponse = 0x0B;
inline constexpr uint8_t kOpcodeHandleValueNotification = 0x1B;
inline constexpr uint8_t kOpcodeErrorResponse = 0x01;

// ATT Write Command: fire-and-forget write (no response from the peer).
std::vector<uint8_t> BuildWriteCommand(uint16_t handle, uint8_t value);

// ATT Read Request for a single handle.
std::vector<uint8_t> BuildReadRequest(uint16_t handle);

// ATT Read Response (0x0B): returns the value bytes of the last requested
// handle. The frame carries no handle, so the caller attributes it to whatever
// read it issued.
bool IsReadResponse(const std::vector<uint8_t>& data);
std::optional<std::vector<uint8_t>> ParseReadResponse(const std::vector<uint8_t>& data);

// ATT Handle Value Notification (0x1B): carries the handle it belongs to.
bool IsNotification(const std::vector<uint8_t>& data);
std::optional<std::pair<uint16_t, std::vector<uint8_t>>> ParseNotification(
    const std::vector<uint8_t>& data);

// ATT Error Response (0x01). A read of handle 0x1B answered with an error
// proves the ATT path is alive even though the value is unavailable.
bool IsErrorResponse(const std::vector<uint8_t>& data);

// True when the frame can be an ATT PDU at all. Every L2CAP AAP frame starts
// with the `04 00 04 00` header (or the `00 00 04 00` handshake), so anything
// matching a known ATT opcode and minimum length is routed to the ATT
// handlers instead of the AAP notification parsers.
bool IsAttFrame(const std::vector<uint8_t>& data);

// Convenience builder: the loud-sound-reduction write frame (identical to
// Packets::BuildLoudSoundReductionPacket; kept here so ATT users have one home).
std::vector<uint8_t> BuildLoudSoundReductionWrite(bool enable);

} // namespace Core::AAP::Att
