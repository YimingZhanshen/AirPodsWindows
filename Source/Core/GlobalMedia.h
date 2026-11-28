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

#include "GlobalMedia_abstract.h"

#if defined APD_OS_WIN
    #include "GlobalMedia_win.h"
#endif

namespace Core::GlobalMedia {

inline void Play()
{
    Controller::GetInstance().Play();
}

inline void Pause()
{
    Controller::GetInstance().Pause();
}

#if defined APD_OS_WIN
// One media operation, for example from a stem gesture. False means nothing accepted it and the
// controller already logged why.
inline bool Execute(Details::MediaAction action, int volumeStepPercent)
{
    return Controller::GetInstance().Execute(action, volumeStepPercent);
}
#endif

inline bool SetVolume(int percent)
{
    return Controller::GetInstance().SetVolume(percent);
}

inline std::optional<float> GetVolume()
{
    return Controller::GetInstance().GetVolume();
}

inline void ClearVolumeReductionState()
{
    Controller::GetInstance().ClearVolumeReductionState();
}

} // namespace Core::GlobalMedia
