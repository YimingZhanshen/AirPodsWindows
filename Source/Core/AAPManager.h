//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
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

#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <optional>
#include <memory>

#include "AAP.h"
#include "Base.h"
#include "../Helper.h"

// Forward declaration for MagicAAP WinRT client
namespace Core::MagicAAPWinRT {
    class MagicAAPWinRTClient;
}

namespace Core::AAP {

//////////////////////////////////////////////////
// Callbacks for AAP events
//

struct Callbacks {
    using FnOnNoiseControlChangedT = std::function<void(NoiseControlMode)>;
    using FnOnConversationalAwarenessChangedT = std::function<void(ConversationalAwarenessState)>;
    using FnOnSpeakingLevelChangedT = std::function<void(SpeakingLevel)>;
    using FnOnEarDetectionChangedT = std::function<void(EarStatus, EarStatus)>;
    using FnOnHeadTrackingDataT = std::function<void(HeadTrackingData)>;
    using FnOnPersonalizedVolumeChangedT = std::function<void(PersonalizedVolumeState)>;
    using FnOnLoudSoundReductionChangedT = std::function<void(LoudSoundReductionState)>;
    using FnOnAutomaticEarDetectionChangedT = std::function<void(bool)>;
    using FnOnAdaptiveTransparencyLevelChangedT = std::function<void(uint8_t)>;
    using FnOnConnectedT = std::function<void()>;
    using FnOnDisconnectedT = std::function<void()>;
    
    FnOnNoiseControlChangedT onNoiseControlChanged;
    FnOnConversationalAwarenessChangedT onConversationalAwarenessChanged;
    FnOnSpeakingLevelChangedT onSpeakingLevelChanged;
    FnOnEarDetectionChangedT onEarDetectionChanged;
    FnOnHeadTrackingDataT onHeadTrackingData;
    FnOnPersonalizedVolumeChangedT onPersonalizedVolumeChanged;
    FnOnLoudSoundReductionChangedT onLoudSoundReductionChanged;
    FnOnAutomaticEarDetectionChangedT onAutomaticEarDetectionChanged;
    FnOnAdaptiveTransparencyLevelChangedT onAdaptiveTransparencyLevelChanged;
    FnOnConnectedT onConnected;
    FnOnDisconnectedT onDisconnected;
};

//////////////////////////////////////////////////
// AAP Manager - Manages L2CAP connection and protocol
//

class Manager {
public:
    Manager();
    ~Manager();

    // Connection management
    bool Connect(uint64_t deviceAddress);
    void Disconnect();
    bool IsConnected() const;

    // Noise control
    bool SetNoiseControlMode(NoiseControlMode mode);
    std::optional<NoiseControlMode> GetNoiseControlMode() const;

    // Conversational awareness
    bool SetConversationalAwareness(bool enable);
    std::optional<ConversationalAwarenessState> GetConversationalAwarenessState() const;

    // Personalized volume
    bool SetPersonalizedVolume(bool enable);
    std::optional<PersonalizedVolumeState> GetPersonalizedVolumeState() const;

    // Automatic ear detection (off-ear auto pause)
    bool SetAutomaticEarDetection(bool enable);
    std::optional<bool> GetAutomaticEarDetectionState() const;

    // Loud sound reduction (headphone safety)
    bool SetLoudSoundReduction(bool enable);
    std::optional<LoudSoundReductionState> GetLoudSoundReductionState() const;

    // Adaptive transparency level (0-50, only effective when noise control is Adaptive)
    bool SetAdaptiveTransparencyLevel(uint8_t level);
    std::optional<uint8_t> GetAdaptiveTransparencyLevel() const;

    // Adaptive noise level (0-100, only effective when noise control is Adaptive)
    bool SetAdaptiveNoiseLevel(uint8_t level);

    // Head tracking
    bool StartHeadTracking();
    bool StopHeadTracking();
    bool IsHeadTrackingActive() const;

    // Callbacks
    void SetCallbacks(Callbacks callbacks);

    // Check if connected via MagicAAP driver
    bool IsConnectedViaMagicAAP() const { return _usingMagicAAP.load(); }
    
    // Static method to check if MagicAAP driver is available
    static bool IsMagicAAPDriverAvailable();

private:
    mutable std::mutex _mutex;
    std::atomic<bool> _connected{false};
    std::atomic<bool> _headTrackingActive{false};
    std::atomic<bool> _usingMagicAAP{false};
    
    // Cached states
    std::optional<NoiseControlMode> _noiseControlMode;
    std::optional<ConversationalAwarenessState> _conversationalAwarenessState;
    std::optional<PersonalizedVolumeState> _personalizedVolumeState;
    std::optional<bool> _automaticEarDetectionState;
    std::optional<LoudSoundReductionState> _loudSoundReductionState;
    std::optional<uint8_t> _adaptiveTransparencyLevel;
    
    // Callbacks
    Callbacks _callbacks;
    
    // Platform-specific socket handle (implemented in platform-specific file)
    void* _socket{nullptr};
    
    // MagicAAP WinRT client (used when driver is available)
    std::unique_ptr<MagicAAPWinRT::MagicAAPWinRTClient> _magicAAPClient;
    
    // Reader thread
    std::thread _readerThread;
    std::atomic<bool> _stopReader{false};
    
    // Internal methods
    bool SendPacket(const std::vector<uint8_t>& packet);
    void ProcessPacket(const std::vector<uint8_t>& packet);
    void ReaderLoop();
    bool InitializeConnection();
    
    // MagicAAP connection method
    bool ConnectViaMagicAAP(uint64_t deviceAddress);
};

} // namespace Core::AAP
