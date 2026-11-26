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

#include "AAPCommandBroker.h"

#include <algorithm>

namespace Core::AAP {

const std::array<AAPConfirmMode, kAAPCommandKindCount> &AAPCommandBroker::DefaultModes()
{
    // Indexed by AAPCommandKind. Echo means a same-kind/value record is required evidence;
    // 0x2E is optional on real firmware, so absence honestly becomes timeout/rollback.
    static constexpr std::array<AAPConfirmMode, kAAPCommandKindCount> kModes{
        AAPConfirmMode::Echo,     // SetNoiseControl            0x0D
        AAPConfirmMode::Echo,     // SetConversationalAwareness 0x28 (spec §11 reply)
        AAPConfirmMode::Echo,     // SetPersonalizedVolume      0x26
        AAPConfirmMode::Readback, // SetLoudSoundReduction      ATT handle 0x1B
        AAPConfirmMode::Echo,     // SetPpeCapLevel             0x38
        AAPConfirmMode::Echo,     // SetAdaptiveNoiseLevel      0x2E optional firmware echo
        AAPConfirmMode::Echo,     // SetAutomaticEarDetection   0x0A
        AAPConfirmMode::Readback, // SetHeadTracking            0x17 sensor stream
    };
    return kModes;
}

AAPConfirmMode AAPCommandBroker::ModeFor(AAPCommandKind kind)
{
    if (static_cast<size_t>(kind) >= kAAPCommandKindCount) {
        return AAPConfirmMode::Unverifiable;
    }
    return DefaultModes()[kKindIndex(kind)];
}

AAPConfirmMode AAPCommandBroker::ModeFor(AAPCommandKind kind, uint32_t value)
{
    const auto mode = ModeFor(kind);
    // Starting head tracking is observable (the 0x0017 sensor records only flow while it is
    // active); stopping it is not, because the absence of records proves nothing. Reporting a
    // 2s timeout for a still head would roll back a setting the device actually accepted.
    if (kind == AAPCommandKind::SetHeadTracking && value == 0) {
        return AAPConfirmMode::Unverifiable;
    }
    return mode;
}

bool AAPCommandBroker::RequiresEvidence(AAPConfirmMode mode)
{
    return mode != AAPConfirmMode::Unverifiable;
}

AAPCommandBroker::AAPCommandBroker(Sender sender, Observer observer, Config config)
    : _sender{std::move(sender)}, _observer{std::move(observer)}, _echoTimeout{config.echoTimeout},
      _clock{std::move(config.clock)}
{
    _overrides = config.overrides;
    _worker = std::jthread{[this](std::stop_token stopToken) { WorkerLoop(stopToken); }};
}

AAPCommandBroker::~AAPCommandBroker()
{
    Shutdown();
}

AAPCommandResult AAPCommandBroker::Submit(AAPCommand command, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return {false, false, AAPCommandFailure::Rejected, 0};
    }
    if (static_cast<size_t>(command.kind) >= kAAPCommandKindCount || command.value > 0xFF) {
        return {false, false, AAPCommandFailure::Rejected, 0};
    }

    std::lock_guard lock{_mutex};
    if (_stopping) {
        return {false, false, AAPCommandFailure::Rejected, 0};
    }
    command.requestId = _nextRequestId++;
    auto &state = _kinds[kKindIndex(command.kind)];
    // The user's intent survives transport failure so a reconnect can replay it.
    state.desired = command.value;
    state.desiredRequestId = command.requestId;
    if (!_sessionOpen || _generation == 0) {
        return {false, false, AAPCommandFailure::TransportClosed, command.requestId};
    }

    if (state.desired == command.value && state.confirmed == command.value) {
        // Confirmation and the manager's cached display update are separate publications. A
        // notification between them still carries the same desired/confirmed value, not new work.
        return {true, false, std::nullopt};
    }
    const bool sameFlight = _flight && _flight->generation == _generation &&
                            _flight->command.kind == command.kind &&
                            _flight->command.value == command.value;
    if (sameFlight) {
        // A repeated notification is evidence of interest, not a new user value. Keep the original
        // flight and its echo window instead of superseding it and submitting a duplicate write.
        return {true, false, std::nullopt};
    }
    if (state.queued == command.value) {
        state.queuedStop = stop;
        return {true, false, std::nullopt};
    }

    // Same-kind supersede: the in-flight wait ends now instead of burning the whole window,
    // and the queued slot is a single value, so a slider burst collapses to the last one.
    if (_flight && _flight->generation == _generation && _flight->command.kind == command.kind) {
        _flight->superseded = true;
    }
    if (state.queued) {
        *state.queued = command.value;
    }
    else {
        state.queued = command.value;
        _queue.push_back(command.kind);
    }
    state.queuedRequestId = command.requestId;
    state.queuedStop = stop;

    _workCondition.notify_all();
    _settleCondition.notify_all();
    return {true, false, std::nullopt, command.requestId};
}

AAPCommandResult AAPCommandBroker::RecordDesired(AAPCommand command)
{
    if (static_cast<size_t>(command.kind) >= kAAPCommandKindCount || command.value > 0xFF) {
        return {false, false, AAPCommandFailure::Rejected};
    }
    std::lock_guard lock{_mutex};
    if (_stopping) {
        return {false, false, AAPCommandFailure::Rejected};
    }
    auto &state = _kinds[kKindIndex(command.kind)];
    state.desired = command.value;
    if (state.queued) {
        *state.queued = command.value;
    }
    return {true, false, std::nullopt};
}

void AAPCommandBroker::OnEcho(AAPCommand command, uint64_t generation)
{
    OnEcho(command, generation, 0);
}

void AAPCommandBroker::OnEcho(AAPCommand command, uint64_t generation, uint64_t requestId)
{
    std::lock_guard lock{_mutex};
    if (_stopping || generation == 0 || static_cast<size_t>(command.kind) >= kAAPCommandKindCount) {
        return;
    }
    if (!_sessionOpen) {
        if (_hasOpenedSession) {
            return;
        }
        // The reader may start just before the first BeginSession. Keep one bounded piece of
        // evidence per kind instead of dropping it or letting a retired session overwrite the
        // next baseline.
        _earlyEvidence[kKindIndex(command.kind)] = {command, generation};
        return;
    }
    if (generation != _generation) {
        return;
    }

    auto &state = _kinds[kKindIndex(command.kind)];
    const bool matchesFlight = _flight && _flight->generation == generation &&
                               _flight->command.kind == command.kind &&
                               (requestId == 0 || _flight->command.requestId == requestId);
    if (matchesFlight && _flight->command.value == command.value && RequiresEvidence(_flight->mode))
    {
        _flight->evidence = true;
    }
    else {
        // Either an unsolicited change or the device reporting a different value than the one
        // we asked for. Both are the device's truth, and neither confirms an in-flight command.
        state.confirmed = command.value;
        if (!state.desired) {
            // A first-session rollback cleared the desired value because no baseline existed.
            // The refresh re-subscribe makes the device report its current value; that observed
            // truth is now the best known desire and must survive reconnect, exactly like
            // OnObserved does for an ATT read.
            state.desired = command.value;
            state.desiredRequestId = 0;
        }
    }
    // A same-generation mismatch is useful rollback evidence, but it must not end the wait for
    // the requested value: only matching evidence (or the deadline) can settle this flight.
    if (matchesFlight && _flight->command.value == command.value) {
        _settleCondition.notify_all();
    }
}

void AAPCommandBroker::OnObserved(AAPCommand command, uint64_t generation)
{
    std::lock_guard lock{_mutex};
    if (_stopping || generation == 0 || static_cast<size_t>(command.kind) >= kAAPCommandKindCount) {
        return;
    }
    if (!_sessionOpen) {
        if (_hasOpenedSession) {
            return;
        }
        _earlyEvidence[kKindIndex(command.kind)] = {command, generation};
        return;
    }
    if (generation == _generation) {
        auto &state = _kinds[kKindIndex(command.kind)];
        state.confirmed = command.value;
        if (!state.desired) {
            // A first-session rollback had no baseline. The refresh is now the best known desired
            // state and must survive reconnect even though it did not originate as a new request.
            state.desired = command.value;
            state.desiredRequestId = 0;
        }
    }
}

void AAPCommandBroker::ReplayDesiredState(
    uint64_t generation, const std::function<bool(AAPCommandKind)> &includeKind)
{
    std::vector<AAPCommandOutcome> outcomes;
    {
        std::lock_guard lock{_mutex};
        if (_stopping || generation == 0 || _replayedGeneration == generation) {
            return;
        }

        if (_sessionOpen && _generation != generation) {
            CloseSessionLocked(outcomes);
        }
        if (_generation != generation) {
            for (auto &state : _kinds) {
                state.confirmed.reset();
            }
        }
        _replayedGeneration = generation;
        _generation = generation;
        _sessionOpen = true;
        _hasOpenedSession = true;

        for (size_t index = 0; index < kAAPCommandKindCount; ++index) {
            auto &evidence = _earlyEvidence[index];
            if (evidence && evidence->second == generation) {
                _kinds[index].confirmed = evidence->first.value;
            }
            evidence.reset();
        }

        // Declaration order, one entry per kind: a reconnect always re-applies the whole desired
        // state the same way, and re-running the replay for a session adds nothing.
        for (size_t index = 0; index < kAAPCommandKindCount; ++index) {
            const auto kind = static_cast<AAPCommandKind>(index);
            auto &state = _kinds[index];
            if (!state.desired || state.queued || state.confirmed == state.desired ||
                (includeKind && !includeKind(kind)))
            {
                continue;
            }
            state.queued = state.desired;
            state.queuedRequestId = state.desiredRequestId;
            _queue.push_back(kind);
        }
        _workCondition.notify_all();
    }
    Publish(outcomes);
}

void AAPCommandBroker::BeginSession(uint64_t generation)
{
    std::vector<AAPCommandOutcome> outcomes;
    {
        std::lock_guard lock{_mutex};
        if (_sessionOpen && _generation != generation) {
            CloseSessionLocked(outcomes);
        }
        if (_generation != generation) {
            for (auto &state : _kinds) {
                state.confirmed.reset();
            }
        }
        _generation = generation;
        _sessionOpen = generation != 0 && !_stopping;
        _hasOpenedSession = _hasOpenedSession || generation != 0;
        for (auto &evidence : _earlyEvidence) {
            if (evidence && evidence->second == generation) {
                _kinds[static_cast<size_t>(evidence->first.kind)].confirmed = evidence->first.value;
            }
            evidence.reset();
        }
        _settleCondition.notify_all();
    }
    Publish(outcomes);
}

void AAPCommandBroker::EndSession()
{
    std::vector<AAPCommandOutcome> outcomes;
    {
        std::lock_guard lock{_mutex};
        CloseSessionLocked(outcomes);
        _settleCondition.notify_all();
    }
    Publish(outcomes);
}

void AAPCommandBroker::CloseSessionLocked(std::vector<AAPCommandOutcome> &outcomes)
{
    _sessionOpen = false;
    for (auto &state : _kinds) {
        state.confirmed.reset();
    }
    _earlyEvidence.fill(std::nullopt);

    // Confirmed values describe the device behind this generation, not the user's intent. They
    // must not become a rollback baseline (or be republished) after a reconnect/device change.
    for (auto &state : _kinds) {
        state.confirmed.reset();
    }

    // Values that never reached the wire simply go back to being desired.
    for (const auto kind : _queue) {
        auto &state = _kinds[kKindIndex(kind)];
        if (!state.queued) {
            continue;
        }
        AAPCommandOutcome outcome;
        outcome.command = {kind, *state.queued, state.queuedRequestId};
        outcome.generation = _generation;
        outcome.requestId = state.queuedRequestId;
        outcome.mode = ModeLocked(kind, *state.queued);
        outcome.settlement = AAPCommandSettlement::TransportClosed;
        outcome.failure = AAPCommandFailure::TransportClosed;
        outcome.rollBackDesired = false;
        state.queued.reset();
        state.queuedRequestId = 0;
        state.queuedStop = {};
        outcomes.push_back(outcome);
    }
    _queue.clear();

    if (_flight) {
        _flight->sessionLost = true;
    }
}

bool AAPCommandBroker::IsSessionOpen() const
{
    std::lock_guard lock{_mutex};
    return _sessionOpen;
}

uint64_t AAPCommandBroker::Generation() const
{
    std::lock_guard lock{_mutex};
    return _generation;
}

void AAPCommandBroker::SetConfirmMode(AAPCommandKind kind, AAPConfirmMode mode)
{
    std::lock_guard lock{_mutex};
    if (static_cast<size_t>(kind) < kAAPCommandKindCount) {
        _overrides[kKindIndex(kind)] = mode;
        if (_flight && _flight->command.kind == kind && _flight->generation == _generation) {
            _flight->mode = mode;
            _flight->capabilityWithdrawn = !RequiresEvidence(mode);
            _settleCondition.notify_all();
        }
    }
}

AAPConfirmMode AAPCommandBroker::Mode(AAPCommandKind kind, uint32_t value) const
{
    std::lock_guard lock{_mutex};
    return static_cast<size_t>(kind) < kAAPCommandKindCount ? ModeLocked(kind, value)
                                                            : AAPConfirmMode::Unverifiable;
}

std::optional<uint32_t> AAPCommandBroker::Desired(AAPCommandKind kind) const
{
    std::lock_guard lock{_mutex};
    return _kinds[kKindIndex(kind)].desired;
}

std::optional<uint32_t> AAPCommandBroker::Confirmed(AAPCommandKind kind) const
{
    std::lock_guard lock{_mutex};
    return _kinds[kKindIndex(kind)].confirmed;
}

uint64_t AAPCommandBroker::LatestRequest(AAPCommandKind kind) const
{
    std::lock_guard lock{_mutex};
    return _kinds[kKindIndex(kind)].desiredRequestId;
}

bool AAPCommandBroker::IsCurrentRequest(AAPCommandKind kind, uint64_t requestId) const
{
    std::lock_guard lock{_mutex};
    return requestId != 0 && _kinds[kKindIndex(kind)].desiredRequestId == requestId;
}

bool AAPCommandBroker::HasPendingCommand(AAPCommandKind kind) const
{
    std::lock_guard lock{_mutex};
    return (_flight && _flight->command.kind == kind) ||
           std::find(_queue.begin(), _queue.end(), kind) != _queue.end();
}

std::optional<AAPCommand> AAPCommandBroker::InFlight() const
{
    std::lock_guard lock{_mutex};
    if (!_flight) {
        return std::nullopt;
    }
    return _flight->command;
}

size_t AAPCommandBroker::QueuedCount() const
{
    std::lock_guard lock{_mutex};
    return _queue.size();
}

bool AAPCommandBroker::WaitForIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock lock{_mutex};
    return _settleCondition.wait_for(
        lock, timeout, [this] { return !_stopping && !_flight && _queue.empty(); });
}

void AAPCommandBroker::AdvanceTimeForTesting(std::chrono::milliseconds delta)
{
    std::lock_guard lock{_mutex};
    if (!_clock.advance || _stopping) {
        return;
    }
    _clock.advance(delta);
    _settleCondition.notify_all();
}

void AAPCommandBroker::Shutdown()
{
    {
        std::lock_guard lock{_mutex};
        _stopping = true;
        _sessionOpen = false;
        for (auto &state : _kinds) {
            state.queued.reset();
            state.queuedRequestId = 0;
        }
        _queue.clear();
        _workCondition.notify_all();
        _settleCondition.notify_all();
    }

    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
    }
}

void AAPCommandBroker::WorkerLoop(std::stop_token stopToken)
{
    std::vector<AAPCommandOutcome> outcomes;
    while (true) {
        outcomes.clear();
        std::unique_lock lock{_mutex};
        _workCondition.wait(lock, stopToken, [this] { return !_queue.empty() || _stopping; });
        if (stopToken.stop_requested() || _stopping) {
            return;
        }

        const auto kind = _queue.front();
        _queue.pop_front();
        auto &state = _kinds[kKindIndex(kind)];
        const uint32_t value = state.queued.value_or(0);
        const auto queuedRequestId = std::exchange(state.queuedRequestId, 0);
        const auto queuedStop = std::exchange(state.queuedStop, {});

        Flight flight;
        flight.command = {kind, value, queuedRequestId};
        flight.generation = _generation;
        flight.mode = ModeLocked(kind, value);
        flight.cancelled = queuedStop.stop_requested();
        _flight = flight;
        // The reference stays valid because only this thread clears the slot; EndSession only
        // flags it, which keeps the wait predicate below from dangling.
        Flight *live = &*_flight;

        state.queued.reset();
        const bool send = !live->cancelled && _sessionOpen && live->generation == _generation;
        lock.unlock();
        // Forward both the caller's cancellation and broker shutdown into the one token passed to
        // the wire. The callbacks live through the echo window, so cancellation also wakes a wait.
        std::stop_source flightStop;
        std::stop_callback workerStop{stopToken, [&flightStop] { flightStop.request_stop(); }};
        std::stop_callback callerStop{queuedStop, [this, &flightStop] {
                                          std::lock_guard lock{_mutex};
                                          flightStop.request_stop();
                                          _settleCondition.notify_all();
                                      }};
        const bool sent = send && _sender
                              ? _sender(flight.command, flight.generation, flightStop.get_token())
                              : false;
        lock.lock();
        live->cancelled = live->cancelled || queuedStop.stop_requested();

        if (stopToken.stop_requested() || _stopping) {
            _flight.reset();
            return;
        }

        // A write with a defined reply gets the echo window; an unverifiable one is final as
        // soon as the record left us, and is never reported as confirmed.
        if (sent && RequiresEvidence(live->mode) && !live->superseded && !live->sessionLost &&
            !live->evidence && !live->cancelled)
        {
            const auto deadline = Now() + _echoTimeout;
            const auto expired = [this, deadline] { return Now() >= deadline; };
            const auto settled = [live, this, &stopToken, &queuedStop, &expired] {
                return live->evidence || live->superseded || live->sessionLost || live->cancelled ||
                       live->capabilityWithdrawn || _stopping || stopToken.stop_requested() ||
                       queuedStop.stop_requested() || expired();
            };
            if (_clock.now) {
                // An injected clock has no timer thread. Its Advance() notifies _settleCondition,
                // so the predicate evaluates time without any wall-clock sleep.
                _settleCondition.wait(lock, settled);
            }
            else {
                _settleCondition.wait_until(lock, deadline, settled);
            }
        }

        if (_flight) {
            SettleLocked(*_flight, sent, outcomes);
            _flight.reset();
        }
        _settleCondition.notify_all();
        lock.unlock();

        Publish(outcomes);
    }
}

void AAPCommandBroker::SettleLocked(
    Flight &flight, bool sent, std::vector<AAPCommandOutcome> &outcomes)
{
    AAPCommandOutcome outcome;
    outcome.command = flight.command;
    outcome.generation = flight.generation;
    outcome.requestId = flight.command.requestId;
    outcome.mode = flight.mode;
    auto &state = _kinds[kKindIndex(flight.command.kind)];

    const auto rollback =
        [&] { // A value the device demonstrably does not hold: hand the caller the last value it is
            // known to hold. Without a baseline the caller must refresh from the device instead of
            // inventing one.
            outcome.rollBackDesired = true;
            outcome.rollbackValue = state.confirmed;
            // Revert the broker's replay state only if no newer user request owns this kind.
            // Keeping the request identity lets the queued UI consumer distinguish this settlement
            // from a superseded one without racing a newer slider move.
            if (state.desiredRequestId == flight.command.requestId) {
                state.desired = state.confirmed;
            }
        };

    if (flight.sessionLost) {
        // Never a user-visible failure: a cancelled or dropped connect leaves the desired value
        // queued, and the next successful connect replays it. Evidence from the retired session
        // must not repopulate the baseline that CloseSessionLocked just cleared.
        outcome.settlement = AAPCommandSettlement::TransportClosed;
        outcome.failure = AAPCommandFailure::TransportClosed;
    }
    else if (flight.evidence) {
        outcome.settlement = AAPCommandSettlement::Confirmed;
        state.confirmed = flight.command.value;
    }
    else if (flight.superseded) {
        outcome.settlement = AAPCommandSettlement::Superseded;
        outcome.failure = AAPCommandFailure::Superseded;
    }
    else if (flight.cancelled) {
        outcome.settlement = AAPCommandSettlement::Rejected;
        outcome.failure = AAPCommandFailure::Rejected;
        rollback();
    }
    else if (!sent) {
        outcome.settlement = AAPCommandSettlement::Rejected;
        outcome.failure = AAPCommandFailure::Rejected;
        rollback();
    }
    else if (!RequiresEvidence(flight.mode)) {
        outcome.settlement = AAPCommandSettlement::AppliedUnconfirmed;
    }
    else {
        outcome.settlement = AAPCommandSettlement::TimedOut;
        outcome.failure = AAPCommandFailure::Timeout;
        rollback();
    }

    outcomes.push_back(outcome);
}

void AAPCommandBroker::Publish(const std::vector<AAPCommandOutcome> &outcomes)
{
    if (!_observer) {
        return;
    }
    for (const auto &outcome : outcomes) {
        _observer(outcome);
    }
}

AAPConfirmMode AAPCommandBroker::ModeLocked(AAPCommandKind kind, uint32_t value) const
{
    const auto &override = _overrides[kKindIndex(kind)];
    return override.value_or(ModeFor(kind, value));
}

std::chrono::steady_clock::time_point AAPCommandBroker::Now() const
{
    return _clock.now ? _clock.now() : std::chrono::steady_clock::now();
}

} // namespace Core::AAP
