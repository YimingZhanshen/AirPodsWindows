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
    // A new session says nothing about the device: the buds stream head tracking only after a
    // Start on this link, so the applied state is unknown and `Flush` below re-pushes it. The
    // reconnect replay is this same path, not a special case.
    _applied.reset();
}

void HeadTrackingIntent::OnTransportDown()
{
    _linkUp = false;
    _applied.reset();
}

HeadTrackingIntent::Action HeadTrackingIntent::Flush(IHeadTrackingCommandPort &port)
{
    if (!_linkUp) {
        // Nothing can be sent. Keep the intent, forget the device: the next `OnTransportUp`
        // makes it pending again.
        _applied.reset();
        return Action::None;
    }

    const bool desired = _enabled;
    // An unknown session is treated as "off", which is what a fresh transport actually is, so
    // connecting with the feature disabled sends a pointless Stop packet on every reconnect.
    if (_applied.value_or(false) == desired) {
        _applied = _applied.value_or(false);
        return Action::None;
    }

    const Action action = desired ? Action::Start : Action::Stop;
    if (port.RequestHeadTracking(desired)) {
        _applied = desired;
        return action;
    }
    // A failed send keeps the previous belief, so `IsPending()` stays true and the next `Flush`
    // retries through the same entry point. It must not be logged or reported as pushed.
    return Action::None;
}

bool HeadTrackingIntent::IsPending() const
{
    if (!_linkUp) {
        return false;
    }
    return _applied.value_or(false) != _enabled;
}

} // namespace Core::AAP
