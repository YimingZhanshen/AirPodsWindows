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
//
// The states are per session: Unknown -> (Pending -> Confirmed | Unconfirmed).
// An explicit read may retry an Unconfirmed path, but exactly one read may be outstanding
// at a time (see Att::PendingReadTracker). Once Confirmed, the path stays confirmed for the
// session: a later read-back that happens to time out says nothing about whether ATT works.
enum class AttPathState : uint8_t {
    Unknown,    // no probe sent yet (or connection torn down)
    Pending,    // request on the wire, still inside its 500ms window
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
    using FnOnConnectedT = std::function<void(AAPAttempt)>;
    using FnOnDisconnectedT = std::function<void(AAPAttempt)>;
    using FnOnAttProbeResultT = std::function<void(AttPathState, AAPAttempt)>;

    FnOnNoiseControlChangedT onNoiseControlChanged;
    FnOnConversationalAwarenessChangedT onConversationalAwarenessChanged;
    FnOnSpeakingLevelChangedT onSpeakingLevelChanged;
    FnOnEarDetectionChangedT onEarDetectionChanged;
    FnOnHeadTrackingDataT onHeadTrackingData;
    FnOnPersonalizedVolumeChangedT onPersonalizedVolumeChanged;
    FnOnLoudSoundReductionChangedT onLoudSoundReductionChanged;
    FnOnAutomaticEarDetectionChangedT onAutomaticEarDetectionChanged;
    FnOnPpeCapLevelChangedT onPpeCapLevelChanged;
    FnOnConnectedT onConnected;
    FnOnDisconnectedT onDisconnected;
    FnOnAttProbeResultT onAttProbeResult;
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
    // Lazily resolves a pending probe once its window passes, using the same half-open
    // deadline rule as the reader path (Att::PendingReadTracker::WindowElapsed), so the
    // getter and an inbound response cannot disagree about the 499/500/501ms boundary.
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

    // ATT path (blueprint 0.3 / P3). Both fields belong to _mutex: the reservation is
    // installed before the request is written, so a response that overtakes the sender is
    // still attributable, and the observable state cannot be rewritten to Pending after it
    // was confirmed. One tracker means one outstanding read and one deadline per session.
    // mutable: resolving an expired window is a lazy, cache-like update performed by the
    // const getter, exactly as the previous atomic deadline field was.
    mutable AttPathState _attPathState{AttPathState::Unknown};
    mutable Att::PendingReadTracker _attPendingRead;

    // Handshake and feature flags are only acknowledged by the device when its ACK record
    // has actually been read (AAP.h). Both start false for every attempt and are never
    // inferred from a successful send.
    bool _handshakeAckObserved{false};
    bool _featureFlagsAckObserved{false};
    // False after a failed feature-flags write. Capability-dependent setters must degrade
    // rather than emit commands the session never enabled.
    bool _featureFlagsEnabled{false};

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

    // Internal methods
    // Clock injection keeps ATT deadline tests deterministic; production uses steady_clock.
    std::function<int64_t()> _steadyMillis;
    // Test-only barrier between the public command's session snapshot and final revalidation.
    // It lets a test replace the session without timing-dependent sleeps; production leaves it
    // empty. Initialization and ATT reservation sends do not cross this public-command boundary.
    std::function<void()> _sendBarrier;
    // Public control commands pin connected state, attempt, transport, and cancellation as one
    // snapshot, then revalidate after the barrier. On success, sentAttempt identifies the session
    // that actually received the command so follow-up work cannot bind itself to a replacement.
    bool SendControlPacket(
        const std::vector<uint8_t> &packet, bool requireFeatureFlags = false,
        AAPAttempt *sentAttempt = nullptr);
    // Initialization-only raw sender. It deliberately runs before the connected gate opens.
    bool SendPacket(
        const std::shared_ptr<AAPTransport> &transport, const std::vector<uint8_t> &packet,
        std::stop_token stopToken);
    // Attempt-bound initialization/ATT sender; reservation ownership is established by the caller.
    bool SendPacket(
        const std::vector<uint8_t> &packet, AAPAttempt attempt,
        const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken = {},
        bool requireFeatureFlags = false);
    bool IsCurrentAttempt(AAPAttempt attempt) const;
    // Requires _mutex; used by the reader path, which already holds it.
    bool IsCurrentAttemptLocked(AAPAttempt attempt) const;
    bool IsActiveAttemptLocked(AAPAttempt attempt) const;
    void ProcessPacket(const std::vector<uint8_t> &packet);
    void ProcessPacket(
        const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks);
    void ReaderLoop(
        AAPAttempt attempt, std::shared_ptr<AAPTransport> transport, std::stop_token stopToken,
        Callbacks callbacks);
    bool InitializeConnection(AAPAttempt attempt, std::stop_token stopToken);
    bool WaitCancellable(std::chrono::milliseconds delay, std::stop_token callerStop);
    std::shared_ptr<AAPTransport>
    ConnectTransport(AAPAttempt attempt, std::stop_token stopToken, ConnectFailure &failure);

    // ATT path (blueprint 0.3 / P3): probe once after the handshake and route ATT
    // frames received on the AAP pipe.
    void StartAttProbe(AAPAttempt attempt, std::stop_token stopToken = {});
    // Reserves the single outstanding read for `attempt` before writing the request frame.
    // Repeating it for the same session and handle is idempotent: no second frame, no
    // second deadline. The tracker token prevents a delayed failed send from releasing a
    // replacement reservation. Returns false when another read still owns the session.
    bool RequestAttRead(AAPAttempt attempt, uint16_t handle, std::stop_token stopToken = {});
    // Requires _mutex. Moves the probe state to Confirmed and reports whether this is the
    // transition the callback has to observe.
    bool MarkAttConfirmedLocked();
    void ProcessAttFrame(
        const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks);
};

} // namespace Core::AAP
