#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Core::AAP {

enum class AAPConnectionState {
    Idle,
    Connecting,
    Handshaking,
    Connected,
    Backoff,
    Stopping,
};

struct AAPAttempt {
    uint64_t address{0};
    uint64_t generation{0};
};

// Pure connection policy. The caller serializes access and supplies both time and jitter,
// making every transition and retry decision reproducible without threads or a Qt event loop.
class AAPConnectionStateMachine
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    // Unit interval, inclusive. 0 produces -20%; 1 produces +20%.
    using Jitter = double (*)();

    struct BeginResult {
        AAPAttempt attempt;
        bool started{false};
    };

    explicit AAPConnectionStateMachine(Jitter jitter = DefaultJitter);

    BeginResult Begin(uint64_t address);
    bool TransportConnected(AAPAttempt attempt);
    bool Connected(AAPAttempt attempt, TimePoint now);
    // Returns the delay when an unexpected disconnect should retry, or nullopt when suppressed.
    std::optional<std::chrono::milliseconds> Lost(AAPAttempt attempt, TimePoint now);
    bool Stop();
    bool FinishStop();
    bool IsCurrent(AAPAttempt attempt) const;
    AAPConnectionState State() const
    {
        return _state;
    }
    std::chrono::milliseconds NextRetryDelay() const
    {
        return _retryDelay;
    }

    static double DefaultJitter();

private:
    static constexpr std::array<uint64_t, 6> kBackoffSeconds{1, 2, 4, 8, 16, 30};

    Jitter _jitter;
    AAPConnectionState _state{AAPConnectionState::Idle};
    AAPAttempt _attempt;
    uint64_t _nextGeneration{1};
    size_t _failureIndex{0};
    TimePoint _connectedSince;
    std::chrono::milliseconds _retryDelay{0};
};

} // namespace Core::AAP
