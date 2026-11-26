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
#include <chrono>
#include <mutex>
#include <thread>
#include <functional>
#include <optional>
#include <memory>

#include "AAP.h"
#include "AAPAtt.h"
#include "AAPBattery.h"
#include "AAPCommandBroker.h"
#include "AAPConnectionStateMachine.h"
#include "AAPTransport.h"
#include "Base.h"
#include "../Helper.h"

namespace Core::AAP {

//////////////////////////////////////////////////
// ATT path state (blueprint 0.3)
//

// Whether the firmware answers ATT frames written into the AAP driver pipe.
// LibrePods sends ATT on a separate PSM 0x001F socket; on Windows we only have
// the PSM 0x1001 driver pipe, so the path is probed once per connection with a
// read request to handle 0x1B and the outcome is recorded here.
enum class AttPathState : uint8_t {
    Unknown,    // no probe sent yet (or connection torn down)
    Pending,    // probe sent, waiting within the 500ms window
    Confirmed,  // firmware answered (read response or error response)
    Unconfirmed // no ATT answer within the probe window
};

// Why the last Connect() attempt failed (blueprint 1.3): surfaced to the UI so
// users see an actionable reason instead of a silent feature blackout.
enum class ConnectFailure : uint8_t {
    None,               // no failure recorded (connected, or never attempted)
    DriverMissing,      // MagicAAP driver not installed or not running
    ServiceUnavailable, // device reachable but does not expose the AAP service
    TransportError,     // socket/driver transport could not be created or failed
    HandshakeFailure,   // transport up, but the AAP handshake did not complete
};

//////////////////////////////////////////////////
// Callbacks for AAP events
//

struct Callbacks {
    // Every worker event carries its immutable session identity. Consumers must discard stale
    // generations after queuing, cancellation, or rebinding.
    using FnOnNoiseControlChangedT = std::function<void(NoiseControlMode, AAPAttempt)>;
    using FnOnConversationalAwarenessChangedT =
        std::function<void(ConversationalAwarenessState, AAPAttempt)>;
    using FnOnSpeakingLevelChangedT = std::function<void(SpeakingLevel, AAPAttempt)>;
    using FnOnEarDetectionChangedT = std::function<void(EarStatus, EarStatus, AAPAttempt)>;
    using FnOnHeadTrackingDataT = std::function<void(HeadTrackingData, AAPAttempt)>;
    using FnOnPersonalizedVolumeChangedT = std::function<void(PersonalizedVolumeState, AAPAttempt)>;
    using FnOnLoudSoundReductionChangedT = std::function<void(LoudSoundReductionState, AAPAttempt)>;
    using FnOnAutomaticEarDetectionChangedT = std::function<void(bool, AAPAttempt)>;
    using FnOnPpeCapLevelChangedT = std::function<void(uint8_t, AAPAttempt)>;
    using FnOnBatteryChangedT = std::function<void(AAPBatterySnapshot, AAPAttempt)>;
    using FnOnConnectedT = std::function<void(AAPAttempt)>;
    using FnOnDisconnectedT = std::function<void(AAPAttempt)>;
    using FnOnAttProbeResultT = std::function<void(AttPathState, AAPAttempt)>;
    // Blueprint 2.7: every command the broker tracked reaches a terminal settlement. Fired on
    // the broker worker thread, so receivers must not touch widgets directly.
    using FnOnCommandSettledT = std::function<void(const AAPCommandOutcome &, AAPAttempt)>;

    FnOnNoiseControlChangedT onNoiseControlChanged;
    FnOnConversationalAwarenessChangedT onConversationalAwarenessChanged;
    FnOnSpeakingLevelChangedT onSpeakingLevelChanged;
    FnOnEarDetectionChangedT onEarDetectionChanged;
    FnOnHeadTrackingDataT onHeadTrackingData;
    FnOnPersonalizedVolumeChangedT onPersonalizedVolumeChanged;
    FnOnLoudSoundReductionChangedT onLoudSoundReductionChanged;
    FnOnAutomaticEarDetectionChangedT onAutomaticEarDetectionChanged;
    FnOnPpeCapLevelChangedT onPpeCapLevelChanged;
    FnOnBatteryChangedT onBatteryChanged;
    FnOnConnectedT onConnected;
    FnOnDisconnectedT onDisconnected;
    FnOnAttProbeResultT onAttProbeResult;
    FnOnCommandSettledT onCommandSettled;
};

//////////////////////////////////////////////////
// AAP Manager - Manages L2CAP connection and protocol
//

class Manager
{
public:
    Manager();
    ~Manager();

    // Connection management
    bool Connect(AAPAttempt attempt, std::stop_token stopToken = {});
    void CancelConnect();
    void Disconnect();
    bool IsConnected() const;
    AAPAttempt GetLastAttempt() const;

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

    // PPE cap level (hearing protection, AAP control 0x38, range 0-100)
    bool SetPpeCapLevel(uint8_t level);
    std::optional<uint8_t> GetPpeCapLevel() const;

    // Adaptive noise level (0-100, only effective when noise control is Adaptive)
    bool SetAdaptiveNoiseLevel(uint8_t level);

    // Head tracking
    bool StartHeadTracking();
    bool StopHeadTracking();
    bool IsHeadTrackingActive() const;

    // Cached battery snapshot of the current session (opcode 0x0004). Cleared when
    // a connect starts and when the session ends; consumers fuse it over BLE values.
    std::optional<AAPBatterySnapshot> GetBatterySnapshot() const;

    // Confirmed command path (blueprint 2.7). The setters above stay the raw wire primitive;
    // settings callers should use SubmitCommand so a value is only reported as applied once the
    // firmware echoes or reads it back, and so a reconnect re-applies what the user asked for.
    AAPCommandResult SubmitCommand(AAPCommand command, std::stop_token stopToken = {});
    std::optional<uint32_t> DesiredCommand(AAPCommandKind kind) const;
    std::optional<uint32_t> ConfirmedCommand(AAPCommandKind kind) const;
    // Protocol evidence currently available for one concrete command value (degrades with the
    // ATT probe, so an unreachable readback never turns into a false timeout).
    AAPConfirmMode CommandMode(AAPCommandKind kind, uint32_t value) const;
    // Blocks until the broker has nothing queued or in flight. Only for tests and shutdown
    // drain; never call it from a thread that owns the wire.
    bool WaitForCommandsIdle(std::chrono::milliseconds timeout);

    // Callbacks
    void SetCallbacks(Callbacks callbacks);

    // Check if connected via MagicAAP driver
    bool IsConnectedViaMagicAAP() const
    {
        return _usingMagicAAP.load();
    }

    // Static method to check if MagicAAP driver is available
    static bool IsMagicAAPDriverAvailable();

    // ATT path probe result for the current connection (blueprint 0.3).
    // Lazily flips a pending probe to Unconfirmed once its 500ms window passes.
    AttPathState GetAttPathState() const;

    // Reason the most recent Connect() attempt failed (blueprint 1.3).
    ConnectFailure GetLastConnectFailure() const
    {
        return _lastConnectFailure.load();
    }

private:
    // Hardware-free tests need to inject a transport and observe the validation
    // gate; neither behavior is reachable through the public connection API.
    friend class ::AAPTransportTests;

    // Serializes whole Connect/Disconnect lifecycles; _mutex protects observable state and is
    // never held while joining a reader or invoking a callback.
    std::mutex _connectMutex;
    mutable std::mutex _mutex;
    std::atomic<bool> _connected{false};
    bool _connecting{false};
    AAPAttempt _attempt;
    std::stop_source _connectStop;
    std::atomic<bool> _headTrackingActive{false};
    std::atomic<bool> _usingMagicAAP{false};

    // Cached states
    std::optional<NoiseControlMode> _noiseControlMode;
    std::optional<ConversationalAwarenessState> _conversationalAwarenessState;
    std::optional<PersonalizedVolumeState> _personalizedVolumeState;
    std::optional<bool> _automaticEarDetectionState;
    std::optional<LoudSoundReductionState> _loudSoundReductionState;
    std::optional<uint8_t> _ppeCapLevel;
    std::optional<AAPBatterySnapshot> _batterySnapshot;

    // ATT path probe (blueprint 0.3): deadline in steady_clock milliseconds,
    // 0 when no probe is outstanding. GetAttPathState flips Pending to
    // Unconfirmed lazily once the deadline passes — no probe thread needed.
    // mutable: the lazy flip happens inside the const getter.
    mutable std::atomic<AttPathState> _attPathState{AttPathState::Unknown};
    mutable std::atomic<int64_t> _attProbeDeadlineMs{0};

    // Why the last Connect() attempt failed; reset to None on success.
    std::atomic<ConnectFailure> _lastConnectFailure{ConnectFailure::None};
    std::atomic<uint64_t> _cancelledGeneration{0};
    std::function<std::shared_ptr<AAPTransport>(AAPTransportKind)> _transportFactory;

    // Callbacks
    Callbacks _callbacks;

    // Atomic session snapshot: senders and the reader retain the transport even
    // while Disconnect exchanges it. P2 still owns worker/generation lifecycle.
    std::atomic<std::shared_ptr<AAPTransport>> _transport{std::shared_ptr<AAPTransport>{}};

    // Retained after natural EOF so the next Connect (or Disconnect) joins it before reuse.
    std::jthread _readerThread;
    std::stop_source _readerStop;
    std::mutex _pendingConnectMutex;
    std::shared_ptr<AAPTransport> _pendingTransport;

    // Blueprint 2.7: desired state, serialization and echo confirmation. Declared last so its
    // destructor joins the command worker before any member the sender can reach is destroyed.
    std::unique_ptr<AAPCommandBroker> _commandBroker;

    // Internal methods
    bool SendPacket(const std::vector<uint8_t> &packet);
    bool SendPacket(
        const std::shared_ptr<AAPTransport> &transport, const std::vector<uint8_t> &packet,
        std::stop_token stopToken);
    bool IsCurrentAttempt(AAPAttempt attempt) const;
    // Command broker seams (blueprint 2.7): the broker owns serialization and the desired
    // state, this layer supplies the wire encoding and the protocol evidence.
    bool SendCommand(const AAPCommand &command, uint64_t generation, std::stop_token stopToken);
    void HandleCommandSettled(const AAPCommandOutcome &outcome);
    void RouteCommandEcho(AAPCommand command, AAPAttempt attempt);
    void ProcessPacket(const std::vector<uint8_t> &packet);
    void ProcessPacket(
        const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks);
    void ReaderLoop(
        AAPAttempt attempt, std::shared_ptr<AAPTransport> transport, std::stop_token stopToken,
        Callbacks callbacks);
    bool
    InitializeConnection(const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken);
    bool WaitCancellable(std::chrono::milliseconds delay, std::stop_token callerStop);
    std::shared_ptr<AAPTransport>
    ConnectTransport(AAPAttempt attempt, std::stop_token stopToken, ConnectFailure &failure);

    // ATT path (blueprint 0.3): probe once after the handshake and route ATT
    // frames received on the AAP pipe.
    void StartAttProbe(const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken);
    void ProcessAttFrame(
        const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks);
};

} // namespace Core::AAP
