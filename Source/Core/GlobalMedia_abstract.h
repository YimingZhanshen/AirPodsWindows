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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Core::GlobalMedia::Details {

// Platform-neutral media vocabulary. The Windows controller maps these onto the System Media
// Transport Controls and the audio endpoint; nothing here depends on Qt or WinRT so the stem
// gesture layer can name an operation without pulling in platform headers.
enum class MediaAction : uint32_t {
    TogglePlayPause,
    NextTrack,
    PreviousTrack,
    VolumeUp,
    VolumeDown,
};

// Float endpoint reads avoid cumulative integer truncation. A failed read yields no target, so a
// stem press can never turn an unknown volume into an accidental write.
inline std::optional<float>
CalculateStemVolume(std::optional<float> currentPercent, int direction, int stepPercent) noexcept
{
    if (!currentPercent.has_value() || !std::isfinite(*currentPercent)) {
        return std::nullopt;
    }
    const auto target = std::round(std::clamp(
        *currentPercent + static_cast<float>(direction) * static_cast<float>(stepPercent), 0.0f,
        100.0f));
    return target;
}

// Conversational-awareness policy: attenuation is relative to the volume at the start of a session.
// A manual stem adjustment ends that session before writing. Any late restoration signal is then a
// no-op, and the next speaking session captures the user's adjusted volume as its new baseline.
class VolumeReductionPolicy
{
public:
    bool Begin(std::optional<float> currentPercent) noexcept
    {
        if (!currentPercent.has_value() || !std::isfinite(*currentPercent)) {
            return false;
        }
        _baseline = std::clamp(*currentPercent, 0.0f, 100.0f);
        return true;
    }

    float ReductionTarget(int reductionPercent) const noexcept
    {
        if (!_baseline.has_value()) {
            return 0.0f;
        }
        const auto factor = static_cast<float>(std::clamp(reductionPercent, 0, 100)) / 100.0f;
        return std::round(*_baseline * factor);
    }

    std::optional<float> RestorationTarget() const noexcept
    {
        return _baseline.has_value() ? std::optional<float>{std::round(*_baseline)} : std::nullopt;
    }

    void End() noexcept
    {
        _baseline.reset();
    }

    bool IsActive() const noexcept
    {
        return _baseline.has_value();
    }

private:
    std::optional<float> _baseline;
};

class ControllerAbstract
{
public:
    ControllerAbstract() = default;
    virtual ~ControllerAbstract() = default;

    virtual void Play() = 0;
    virtual void Pause() = 0;
    virtual bool Execute(MediaAction, int volumeStepPercent)
    {
        return false;
    }
};
} // namespace Core::GlobalMedia::Details
