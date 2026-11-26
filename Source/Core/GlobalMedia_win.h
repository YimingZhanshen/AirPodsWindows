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

#if !defined APD_OS_WIN
    #error "This file shouldn't be compiled."
#endif

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "GlobalMedia_abstract.h"
#include "OS/Windows.h"

namespace Core::GlobalMedia {
namespace Details {

enum class ActionId : uint32_t { Play, Pause };

class MediaProgramAbstract
{
public:
    enum class Priority : uint32_t {
        Max = 0,

        SystemSession = 1,
        MusicPlayer = 2,

        Min = std::numeric_limits<uint32_t>::max()
    };

    virtual inline ~MediaProgramAbstract() {};

    virtual bool IsAvailable() = 0;
    virtual bool IsPlaying() const = 0;
    virtual bool Play() = 0;
    virtual bool Pause() = 0;

    virtual std::wstring GetProgramName() const = 0;
    virtual Priority GetPriority() const = 0;
};
} // namespace Details

class Controller final : public Helper::Singleton<Controller>, public Details::ControllerAbstract
{
protected:
    Controller() = default;
    friend Helper::Singleton<Controller>;

public:
    void Play() override;
    void Pause() override;
    // Stem gesture media operations (blueprint 2.4). Returns false when nothing accepted the
    // command - no media session, or the audio endpoint refused - and the reason is logged.
    bool Execute(Details::MediaAction action, int volumeStepPercent) override;
    void SetVolume(int percent);
    std::optional<float> GetVolume() const;
    void ClearVolumeReductionState(); // Explicitly clear saved volume state

private:
    std::mutex _mutex;
    std::vector<std::unique_ptr<Details::MediaProgramAbstract>> _pausedPrograms;
    Details::VolumeReductionPolicy _volumeReduction;

    // Endpoint volume write that skips the conversational-awareness bookkeeping: an explicit
    // user nudge from a stem press ends any old attenuation session before applying the new value.
    bool SetEndpointVolume(float percent);
};
} // namespace Core::GlobalMedia
