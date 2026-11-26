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
        kOpcodeWriteCommand, static_cast<uint8_t>(handle & 0xFF),
        static_cast<uint8_t>((handle >> 8) & 0xFF), value};
}

std::vector<uint8_t> BuildReadRequest(uint16_t handle)
{
    return {
        kOpcodeReadRequest, static_cast<uint8_t>(handle & 0xFF),
        static_cast<uint8_t>((handle >> 8) & 0xFF)};
}

bool IsReadResponse(const std::vector<uint8_t> &data)
{
    return data.size() >= 2 && data[0] == kOpcodeReadResponse;
}

std::optional<std::vector<uint8_t>> ParseReadResponse(const std::vector<uint8_t> &data)
{
    if (!IsReadResponse(data)) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(data.begin() + 1, data.end());
}

bool IsNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 3 && data[0] == kOpcodeHandleValueNotification;
}

std::optional<std::pair<uint16_t, std::vector<uint8_t>>>
ParseNotification(const std::vector<uint8_t> &data)
{
    if (!IsNotification(data)) {
        return std::nullopt;
    }
    uint16_t handle = static_cast<uint16_t>(data[1] | (data[2] << 8));
    return std::make_pair(handle, std::vector<uint8_t>(data.begin() + 3, data.end()));
}

bool IsErrorResponse(const std::vector<uint8_t> &data)
{
    return data.size() >= 5 && data[0] == kOpcodeErrorResponse;
}

std::optional<ErrorResponse> ParseErrorResponse(const std::vector<uint8_t> &data)
{
    if (!IsErrorResponse(data)) {
        return std::nullopt;
    }
    ErrorResponse parsed;
    parsed.requestOpcode = data[1];
    parsed.handle = static_cast<uint16_t>(data[2] | (data[3] << 8));
    parsed.error = data[4];
    return parsed;
}

bool IsAttFrame(const std::vector<uint8_t> &data)
{
    if (data.empty()) {
        return false;
    }
    // Every L2CAP AAP frame starts with `04 00 04 00` (or `00 00 04 00` for the
    // handshake), so those never belong to the ATT path.
    if (data.size() >= 4 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
        data[3] == 0x00)
    {
        return false;
    }
    if (data.size() >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x04 &&
        data[3] == 0x00)
    {
        return false;
    }
    return IsReadResponse(data) || IsNotification(data) || IsErrorResponse(data);
}

std::vector<uint8_t> BuildLoudSoundReductionWrite(bool enable)
{
    return BuildWriteCommand(kLoudSoundReductionHandle, static_cast<uint8_t>(enable ? 0x01 : 0x00));
}

PendingReadTracker::StartResult PendingReadTracker::Start(
    AAPAttempt attempt, uint16_t handle, int64_t deadlineMs, int64_t nowMs,
    ReservationToken &reservationToken)
{
    reservationToken = 0;
    // A reservation without a session identity could never be matched to a response,
    // and a deadline that is already in the past could never be answered.
    if (attempt.generation == 0 || WindowElapsed(nowMs, deadlineMs)) {
        return StartResult::Rejected;
    }

    if (_pending) {
        const bool sameGeneration = _pending->attempt.generation == attempt.generation;
        const bool sameSession = _pending->attempt.address == attempt.address && sameGeneration;
        // Generations identify sessions globally. Reusing one for another device must not
        // inherit or displace the existing reservation, even after its deadline elapsed.
        if (sameGeneration && !sameSession) {
            return StartResult::Rejected;
        }
        if (sameSession && !WindowElapsed(nowMs, _pending->deadlineMs)) {
            if (_pending->handle == handle) {
                reservationToken = _pending->token;
                return StartResult::AlreadyPending;
            }
            return StartResult::Rejected;
        }
        // A successor generation or an elapsed same-session window owns a new reservation.
        // Retiring the old identity here prevents its late response from matching the new one.
    }

    reservationToken = NextReservationToken();
    _pending = Pending{attempt, handle, deadlineMs, reservationToken};
    return StartResult::Started;
}

PendingReadTracker::ReservationToken PendingReadTracker::NextReservationToken() noexcept
{
    // Zero means "no reservation". Skip the current token as well, so even wraparound cannot
    // let a replacement alias the reservation an older sender can still release.
    do {
        ++_nextReservationToken;
    } while (_nextReservationToken == 0 || (_pending && _nextReservationToken == _pending->token));
    return _nextReservationToken;
}

PendingReadTracker::MatchResult
PendingReadTracker::Match(AAPAttempt attempt, std::optional<uint16_t> handle, int64_t nowMs) const
{
    if (!_pending) {
        return MatchResult::NoPendingRead;
    }
    if (WindowElapsed(nowMs, _pending->deadlineMs)) {
        return MatchResult::Expired;
    }
    // Both halves of the session identity have to agree: a reply that reaches the current
    // reader but belongs to another address or generation is a foreign frame, and a
    // notification or error naming another handle answers nothing.
    if (_pending->attempt.address != attempt.address ||
        _pending->attempt.generation != attempt.generation)
    {
        return MatchResult::NoPendingRead;
    }
    if (handle.has_value() && *handle != _pending->handle) {
        return MatchResult::NoPendingRead;
    }
    return MatchResult::Matched;
}

void PendingReadTracker::ClearIfElapsed(int64_t nowMs)
{
    if (_pending && WindowElapsed(nowMs, _pending->deadlineMs)) {
        _pending.reset();
    }
}

void PendingReadTracker::Clear()
{
    _pending.reset();
}

bool PendingReadTracker::ReleaseIf(ReservationToken reservationToken) noexcept
{
    if (!_pending || _pending->token != reservationToken) {
        return false;
    }
    _pending.reset();
    return true;
}

bool PendingReadTracker::IsLive(int64_t nowMs) const noexcept
{
    return _pending && !WindowElapsed(nowMs, _pending->deadlineMs);
}

std::optional<uint16_t> PendingReadTracker::Handle() const noexcept
{
    return _pending ? std::optional<uint16_t>{_pending->handle} : std::nullopt;
}

} // namespace Core::AAP::Att
