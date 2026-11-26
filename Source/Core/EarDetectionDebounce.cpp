#include "EarDetectionDebounce.h"

#include <algorithm>

namespace Core {

EarDetectionDebounce::EarDetectionDebounce(EarDetectSensitivity sensitivity)
    : _sensitivity{sensitivity}
{
}

std::chrono::milliseconds EarDetectionDebounce::DebounceTime(EarDetectSensitivity sensitivity)
{
    switch (sensitivity) {
    case EarDetectSensitivity::Low:
        return std::chrono::milliseconds{600};
    case EarDetectSensitivity::High:
        return std::chrono::milliseconds{150};
    case EarDetectSensitivity::Normal:
    default:
        return std::chrono::milliseconds{300};
    }
}

EarState EarDetectionDebounce::Update(EarState candidate, std::chrono::milliseconds now)
{
    // Clamp before comparing so a backward clock cannot manufacture a new candidate.
    if (!_hasLastNow || now >= _lastNow) {
        _lastNow = now;
        _hasLastNow = true;
    }
    else {
        now = _lastNow;
    }
    if (candidate == EarState::Unknown) {
        return _state;
    }
    if (candidate == _state) {
        CancelPending();
        return _state;
    }
    if (!_hasCandidate || candidate != _candidate) {
        _candidate = candidate;
        _candidateSince = now;
        _hasCandidate = true;
    }
    return Advance(now);
}

EarState EarDetectionDebounce::Advance(std::chrono::milliseconds now)
{
    if (!_hasLastNow || now >= _lastNow) {
        _lastNow = now;
        _hasLastNow = true;
    }
    else {
        now = _lastNow;
    }
    if (_hasCandidate && now - _candidateSince >= DebounceTime(_sensitivity)) {
        _state = _candidate;
        CancelPending();
    }
    return _state;
}

std::optional<EarState> EarDetectionDebounce::PendingCandidate() const
{
    return _hasCandidate ? std::optional<EarState>{_candidate} : std::nullopt;
}

std::optional<std::chrono::milliseconds>
EarDetectionDebounce::Deadline(std::chrono::milliseconds now) const
{
    if (!_hasCandidate) {
        return std::nullopt;
    }
    if (!_hasLastNow || now < _lastNow) {
        now = _hasLastNow ? _lastNow : now;
    }
    return std::max<std::chrono::milliseconds>(
        std::chrono::milliseconds{}, _candidateSince + DebounceTime(_sensitivity) - now);
}

void EarDetectionDebounce::Reset()
{
    _state = EarState::Unknown;
    CancelPending();
    _hasLastNow = false;
    _lastNow = {};
}

void EarDetectionDebounce::SetSensitivity(EarDetectSensitivity sensitivity)
{
    _sensitivity = sensitivity;
    CancelPending();
}

void EarDetectionDebounce::CancelPending()
{
    _candidate = EarState::Unknown;
    _candidateSince = {};
    _hasCandidate = false;
}

} // namespace Core
