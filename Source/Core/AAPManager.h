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

#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <optional>
#include <memory>

#include "AAP.h"
#include "AAPAtt.h"
#include "Base.h"
#include "../Helper.h"

// Forward declaration for MagicAAP WinRT client
namespace Core::MagicAAPWinRT {
    class MagicAAPWinRTClient;
}

namespace Core::AAP {

//////////////////////////////////////////////////
// ATT path state (blueprint 0.3)
//

// Whether the firmware answers ATT frames written into the AAP driver pipe.
// LibrePods sends ATT on a separate PSM 0x001F socket; on Windows we only have
// the PSM 0x1001 driver pipe, so the path is probed once per connection with a
// read request to handle 0x1B and the outcome is recorded here.
enum class AttPathState : uint8_t {
    Unknown,     // no probe sent yet (or connection torn down)
    Pending,     // probe sent, waiting within the 500ms window
    Confirmed,  // firmware answered (read response or error response)
    Unconfirmed // no ATT answer within the probe window
};

// Why the last Connect() attempt failed (blueprint 1.3): surfaced to the UI so
// users see an actionable reason instead of a silent feature blackout.
enum class ConnectFailure : uint8_t {
    None,              // no failure recorded (connected, or never attempted)
    DriverMissing,     // MagicAAP driver not installed or not running
    ServiceUnavailable,// device reachable but does not expose the AAP service
    TransportError,    // socket/driver transport could not be created or failed
    HandshakeFailure,  // transport up, but the AAP handshake did not complete
};

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
    using FnOnAttProbeResultT = std::function<void(AttPathState)>;

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
    FnOnAttProbeResultT onAttProbeResult;
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

    // ATT path probe result for the current connection (blueprint 0.3).
    // Lazily flips a pending probe to Unconfirmed once its 500ms window passes.
    AttPathState GetAttPathState() const;

    // Reason the most recent Connect() attempt failed (blueprint 1.3).
    ConnectFailure GetLastConnectFailure() const { return _lastConnectFailure.load(); }

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

    // ATT path probe (blueprint 0.3): deadline in steady_clock milliseconds,
    // 0 when no probe is outstanding. GetAttPathState flips Pending to
    // Unconfirmed lazily once the deadline passes — no probe thread needed.
    // mutable: the lazy flip happens inside the const getter.
    mutable std::atomic<AttPathState> _attPathState{AttPathState::Unknown};
    mutable std::atomic<int64_t> _attProbeDeadlineMs{0};

    // Why the last Connect() attempt failed; reset to None on success.
    std::atomic<ConnectFailure> _lastConnectFailure{ConnectFailure::None};
    
    // Callbacks
    Callbacks _callbacks;
    
    // Platform-specific socket handle (implemented in platform-specific file)
    void* _socket{nullptr};
    
    // MagicAAP WinRT client (used when driver is available)
    std::unique_ptr<MagicAAPWinRT::MagicAAPWinRTClient> _magicAAPClient;
    
    // Reader thread
    std::thread _readerThread;
    std::atomic<bool> _stopReader{false};
    // Indicates whether reader thread has exited
    std::atomic<bool> _readerExited{true};
    
    // Internal methods
    bool SendPacket(const std::vector<uint8_t>& packet);
    void ProcessPacket(const std::vector<uint8_t>& packet);
    void ReaderLoop();
    bool InitializeConnection();

    // ATT path (blueprint 0.3): probe once after the handshake and route ATT
    // frames received on the AAP pipe.
    void StartAttProbe();
    void ProcessAttFrame(const std::vector<uint8_t>& packet);
    
    // MagicAAP connection method
    bool ConnectViaMagicAAP(uint64_t deviceAddress);
};

} // namespace Core::AAP
