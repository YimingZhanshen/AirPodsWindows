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

#include "HeadTrackingIntent.h"

namespace Core::AAP {

void HeadTrackingIntent::SetEnabled(bool enabled)
{
    _enabled = enabled;
}

void HeadTrackingIntent::OnTransportUp(std::uint64_t generation)
{
    if (generation == 0) {
        OnTransportDown();
        return;
    }
    if (_linkUp && _generation == generation) {
        return;
    }

    _linkUp = true;
    _generation = generation;
    _applied.reset();
    _inFlightGeneration.reset();
    _inFlightValue.reset();
    if (_settledGeneration == generation) {
        _applied = _settledValue;
    }
}

void HeadTrackingIntent::OnTransportDown()
{
    _linkUp = false;
    _applied.reset();
    _inFlightGeneration.reset();
    _inFlightValue.reset();
}

HeadTrackingIntent::FlushResult HeadTrackingIntent::Flush(IHeadTrackingCommandPort &port)
{
    if (!_linkUp) {
        // Generation zero records the latest desire without queueing wire work. The broker keeps
        // it for the next session, so a Start/Stop chosen while offline replays that exact value.
        return {{}, port.SubmitHeadTracking(_enabled, 0)};
    }

    const auto value = static_cast<std::uint32_t>(_enabled ? 0x01 : 0x00);
    if (_applied.has_value() && *_applied == _enabled) {
        return {{}, {true, false, std::nullopt}};
    }
    if (_inFlightGeneration == _generation && _inFlightValue == value) {
        return {{}, {true, false, std::nullopt}};
    }

    auto submission = port.SubmitHeadTracking(_enabled, _generation);
    if (!submission.accepted) {
        return {{}, submission};
    }

    _inFlightGeneration = _generation;
    _inFlightValue = value;
    return {_enabled ? Action::Start : Action::Stop, submission};
}

void HeadTrackingIntent::OnCommandSettled(const AAPCommandOutcome &outcome)
{
    if (outcome.command.kind != AAPCommandKind::SetHeadTracking || outcome.generation == 0) {
        return;
    }
    if (_linkUp && outcome.generation != _generation) {
        return;
    }
    if (!_linkUp && outcome.generation < _settledGeneration) {
        return;
    }

    const bool applied = outcome.settlement == AAPCommandSettlement::Confirmed ||
                         outcome.settlement == AAPCommandSettlement::AppliedUnconfirmed;
    if (applied) {
        _settledGeneration = outcome.generation;
        _settledValue = outcome.command.value != 0;
        if (_linkUp && outcome.generation == _generation) {
            _applied = _settledValue;
        }
    }

    if (_inFlightGeneration == outcome.generation && _inFlightValue == outcome.command.value) {
        _inFlightGeneration.reset();
        _inFlightValue.reset();
    }
}

bool HeadTrackingIntent::IsPending() const
{
    if (!_linkUp) {
        return false;
    }
    return !_applied.has_value() || *_applied != _enabled;
}

} // namespace Core::AAP
