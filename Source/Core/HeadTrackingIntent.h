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

#include "AAPCommandBroker.h"

namespace Core::AAP {

// The only head-tracking submission seam. Implementations submit to AAPCommandBroker; the broker's
// generation-correlated outcomes are fed back through HeadTrackingIntent::OnCommandSettled. A send
// returning true means "accepted for serialized delivery", not "the firmware applied it".
class IHeadTrackingCommandPort
{
public:
    virtual ~IHeadTrackingCommandPort() = default;

    // Generation zero records the desired value only; a nonzero generation must match the live
    // transport before the command can be queued.
    virtual AAPCommandResult SubmitHeadTracking(bool enable, std::uint64_t generation) = 0;
};

// Pure desired/pending policy behind the `head_tracking` setting. The broker owns serialization,
// desired-state replay, retries and confirmation; this class prevents duplicate submissions and
// keeps the UI intent pending until a matching outcome arrives.
//
// No Qt, threads or clock: the caller serializes access and supplies transport generations.
class HeadTrackingIntent
{
public:
    enum class Action : std::uint8_t {
        None,  // already applied, link down, or the broker did not accept a submission
        Start, // a Start command was accepted by the broker
        Stop,  // a Stop command was accepted by the broker
    };

    struct FlushResult {
        Action action{Action::None};
        AAPCommandResult submission{};
    };

    // Reflects the settings field; the caller follows up with `Flush`.
    void SetEnabled(bool enabled);
    // A new generation has unknown device state. Both desired values are forced onto the wire:
    // even Stop is replayed because session-reset semantics are not assumed.
    void OnTransportUp(std::uint64_t generation);
    // The link dropped. A later OnTransportUp for a newer generation replays the desired value.
    void OnTransportDown();

    FlushResult Flush(IHeadTrackingCommandPort &port);
    // Broker result for this kind. Stale generations and superseded values cannot change Applied.
    void OnCommandSettled(const AAPCommandOutcome &outcome);

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
    // Confirmed/applied state for this session, or nullopt while it is unknown.
    std::optional<bool> Applied() const
    {
        return _applied;
    }

private:
    bool _enabled{false};
    bool _linkUp{false};
    std::uint64_t _generation{0};
    std::optional<bool> _applied;
    std::optional<std::uint64_t> _inFlightGeneration;
    std::optional<std::uint32_t> _inFlightValue;
    // A broker worker can settle a replay just before the transport-up callback updates LinkUp.
    std::uint64_t _settledGeneration{0};
    std::optional<bool> _settledValue;
};

} // namespace Core::AAP
