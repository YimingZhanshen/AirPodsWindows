#include "AAPConnectionStateMachine.h"

#include <algorithm>
#include <random>

namespace Core::AAP {

AAPConnectionStateMachine::AAPConnectionStateMachine(Jitter jitter) : _jitter{jitter} {}

AAPConnectionStateMachine::BeginResult AAPConnectionStateMachine::Begin(uint64_t address)
{
    if (_state == AAPConnectionState::Connecting || _state == AAPConnectionState::Handshaking ||
        _state == AAPConnectionState::Connected)
    {
        return {_attempt, false};
    }
    if (_state == AAPConnectionState::Stopping) {
        return {{address, 0}, false};
    }

    _attempt = {address, _nextGeneration++};
    _retryDelay = {};
    _state = AAPConnectionState::Connecting;
    return {_attempt, true};
}

bool AAPConnectionStateMachine::TransportConnected(AAPAttempt attempt)
{
    if (_state != AAPConnectionState::Connecting || !IsCurrent(attempt)) {
        return false;
    }
    _state = AAPConnectionState::Handshaking;
    return true;
}

bool AAPConnectionStateMachine::Connected(AAPAttempt attempt, TimePoint now)
{
    if ((_state != AAPConnectionState::Connecting && _state != AAPConnectionState::Handshaking) ||
        !IsCurrent(attempt))
    {
        return false;
    }
    _connectedSince = now;
    _retryDelay = {};
    _state = AAPConnectionState::Connected;
    return true;
}

std::optional<std::chrono::milliseconds>
AAPConnectionStateMachine::Lost(AAPAttempt attempt, TimePoint now)
{
    if ((_state != AAPConnectionState::Connected && _state != AAPConnectionState::Connecting &&
         _state != AAPConnectionState::Handshaking) ||
        !IsCurrent(attempt))
    {
        return std::nullopt;
    }

    // A connection that survived for a minute is healthy, even if its first retry later fails.
    if (_state == AAPConnectionState::Connected &&
        now - _connectedSince >= std::chrono::seconds{60})
    {
        _failureIndex = 0;
    }
    const auto seconds = kBackoffSeconds[std::min(_failureIndex, kBackoffSeconds.size() - 1)];
    if (_failureIndex < kBackoffSeconds.size()) {
        ++_failureIndex;
    }

    const auto jitter = std::clamp(_jitter(), 0.0, 1.0);
    const auto multiplier = 800 + static_cast<int64_t>(jitter * 400);
    _retryDelay = std::chrono::milliseconds{static_cast<int64_t>(seconds) * multiplier};
    _state = AAPConnectionState::Backoff;
    return _retryDelay;
}

bool AAPConnectionStateMachine::Stop()
{
    if (_state == AAPConnectionState::Stopping || _state == AAPConnectionState::Idle) {
        return false;
    }
    _attempt = {};
    _retryDelay = {};
    _state = AAPConnectionState::Stopping;
    return true;
}

bool AAPConnectionStateMachine::FinishStop()
{
    if (_state != AAPConnectionState::Stopping) {
        return false;
    }
    _attempt = {};
    _failureIndex = 0;
    _retryDelay = {};
    _state = AAPConnectionState::Idle;
    return true;
}

bool AAPConnectionStateMachine::IsCurrent(AAPAttempt attempt) const
{
    return attempt.generation != 0 && attempt.address == _attempt.address &&
           attempt.generation == _attempt.generation;
}

double AAPConnectionStateMachine::DefaultJitter()
{
    static thread_local std::mt19937 generator{std::random_device{}()};
    static thread_local std::uniform_real_distribution<double> distribution{0.0, 1.0};
    return distribution(generator);
}

} // namespace Core::AAP
