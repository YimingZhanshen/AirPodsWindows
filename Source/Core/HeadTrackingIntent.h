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

namespace Core::AAP {

// The single place that decides how a head-tracking request reaches the transport.
//
// Blueprint 2.5 only needs "on while it should be on", so D5 keeps the *intent* here and leaves
// the send behind this port. Blueprint 2.7 (C7) replaces the implementation with
// `AAPCommandBroker`, whose command list already contains `SetHeadTracking`
// (task-C7-brief.md), and then only `Flush` call sites and this port change: the desired-state
// bookkeeping below, the generation reset and the replay-on-reconnect path stay as they are.
// That is deliberate — D5 must not stand up a second timeout/rollback mechanism next to C7's.
class IHeadTrackingCommandPort
{
public:
    virtual ~IHeadTrackingCommandPort() = default;

    // Returns true when the packet reached an open transport.
    virtual bool RequestHeadTracking(bool enable) = 0;
};

// Pure policy behind the `head_tracking` setting: the device stream must run exactly while the user
// switched the feature on AND the AAP transport is up.
//
// No Qt, no threads, no clock: the caller serializes access and supplies the transport
// generations, so the reconnect replay is a decision that can be tested without hardware.
class HeadTrackingIntent
{
public:
    enum class Action : std::uint8_t {
        None,  // no packet was pushed: already applied, link down, or the send failed
        Start, // a Start packet was pushed through the port
        Stop,  // a Stop packet was pushed through the port
    };

    // Reflects the settings field; the caller follows up with `Flush`.
    void SetEnabled(bool enabled);
    // A fresh transport generation forgets everything the device was doing: a new session starts
    // with head tracking off, so the next `Flush` re-pushes the intent. This is the replay.
    void OnTransportUp(std::uint64_t generation);
    // The link dropped. Nothing can be sent, and whatever was believed applied is void; a later
    // `OnTransportUp` for a newer generation replays it.
    void OnTransportDown();

    // Pushes the desired state if it is not already known to be in effect for this session.
    // A failed send leaves the intent pending, so calling `Flush` again retries it — through the
    // same path, never a second mechanism.
    Action Flush(IHeadTrackingCommandPort &port);

    // True while a send is still owed (head tracking enabled, link up, not yet applied).
    bool IsPending() const;
    bool Enabled() const
    {
        return _enabled;
    }
    bool LinkUp() const
    {
        return _linkUp;
    }
    std::uint64_t Generation() const
    {
        return _generation;
    }
    // What the device is believed to be doing, or nullopt when this session is unknown.
    std::optional<bool> Applied() const
    {
        return _applied;
    }

private:
    bool _enabled{false};
    bool _linkUp{false};
    std::uint64_t _generation{0};
    std::optional<bool> _applied;
};

} // namespace Core::AAP
