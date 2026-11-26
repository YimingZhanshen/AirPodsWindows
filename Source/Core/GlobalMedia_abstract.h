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
