#pragma once

#include <chrono>

namespace Core {

enum class EarState { InEar, OutOfEar, InCase, Unknown };
enum class EarDetectSensitivity { Low, Normal, High };

// Pure ear-state debounce policy. The caller serializes access and supplies a monotonic clock.
// A candidate must remain unchanged for the full sensitivity interval before becoming stable.
// Unknown readings are neutral: they neither replace a known state nor restart a pending candidate.
class EarDetectionDebounce
{
public:
    explicit EarDetectionDebounce(EarDetectSensitivity sensitivity = EarDetectSensitivity::Normal);

    EarState Update(EarState candidate, std::chrono::milliseconds now);
    void Reset();

    // Preserves the stable state but discards pending work when the timing policy changes.
    void SetSensitivity(EarDetectSensitivity sensitivity);
    void CancelPending();
    EarState StableState() const
    {
        return _state;
    }

    // Low filters longer, Normal is the standard 300 ms, and High reacts sooner.
    static std::chrono::milliseconds DebounceTime(EarDetectSensitivity sensitivity);

private:
    EarDetectSensitivity _sensitivity;
    EarState _state{EarState::Unknown};
    EarState _candidate{EarState::Unknown};
    std::chrono::milliseconds _candidateSince{0};
    std::chrono::milliseconds _lastNow{0};
    bool _hasCandidate{false};
    bool _hasLastNow{false};
};

} // namespace Core
