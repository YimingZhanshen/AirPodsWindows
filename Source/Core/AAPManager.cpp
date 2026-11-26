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

#include "AAPManager.h"
#include "AAPMessage.h"
#include "ConnectionDiagnostics.h"
#include "../Logger.h"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined APD_OS_WIN

    #include "MagicAAPWinRT.h"

namespace Core::AAP {

namespace {

// Blueprint 0.3: how long a single ATT read may stay outstanding. The boundary is
// half-open on purpose (Att::PendingReadTracker::WindowElapsed): a frame that arrives at
// t0+499 still answers the request, one at t0+500 or later belongs to no live request.
// The lazy resolution in GetAttPathState() and the reader path share that one rule.
constexpr int64_t kAttProbeWindowMs = 500;

const char *DescribeFailure(ConnectFailure failure)
{
    switch (failure) {
    case ConnectFailure::None:
        return "no failure recorded";
    case ConnectFailure::DriverMissing:
        return "MagicAAP driver unavailable";
    case ConnectFailure::ServiceUnavailable:
        return "device does not expose the AAP service";
    case ConnectFailure::TransportError:
        return "transport could not be established";
    case ConnectFailure::HandshakeFailure:
        return "AAP handshake did not complete";
    }
    return "unknown failure";
}

// Returns the diagnostics recorder to the preconnect phase when a Connect call ends
// without an established session; Disconnect() and the reader's unexpected end are the
// matching end points for a live one. Without it the next advertisement match would be
// attributed to the attempt that already failed, and the report would lose the evidence
// that the device was seen again.
class AttemptPhaseGuard
{
public:
    explicit AttemptPhaseGuard(AAPAttempt attempt) : _attempt(attempt) {}

    void Succeeded() noexcept
    {
        _live = true;
    }

    ~AttemptPhaseGuard()
    {
        if (!_live) {
            Diagnostics::Recorder::Instance().EndAttempt(_attempt.generation);
        }
    }

private:
    AAPAttempt _attempt;
    bool _live{false};
};

} // namespace

Manager::Manager()
    : _steadyMillis{[] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
              .count();
      }}
{
}

Manager::~Manager()
{
    Disconnect();
}

void Manager::CancelConnect()
{
    std::stop_source connectStop;
    {
        std::lock_guard lock{_mutex};
        if (_attempt.generation != 0) {
            _cancelledGeneration.store(_attempt.generation);
        }
        connectStop = _connectStop;
    }
    connectStop.request_stop();

    std::shared_ptr<AAPTransport> pending;
    {
        std::lock_guard lock{_pendingConnectMutex};
        pending = _pendingTransport;
    }
    if (pending) {
        pending->Close();
    }
}

bool Manager::Connect(AAPAttempt attempt, std::stop_token stopToken)
{
    if (attempt.generation == 0 || stopToken.stop_requested() ||
        _cancelledGeneration.load() == attempt.generation)
    {
        return false;
    }

    std::unique_lock connectLock{_connectMutex};
    const auto cancelled = [&] {
        return stopToken.stop_requested() || _connectStop.get_token().stop_requested() ||
               _cancelledGeneration.load() == attempt.generation;
    };
    Callbacks callbacks;
    std::shared_ptr<AAPTransport> previousTransport;
    std::jthread previousReader;

    {
        std::lock_guard lock{_mutex};
        if (_connected) {
            return _attempt.address == attempt.address && _attempt.generation == attempt.generation;
        }
        if (_connecting) {
            return _attempt.address == attempt.address && _attempt.generation == attempt.generation;
        }

        _connecting = true;
        _attempt = attempt;
        _connectStop = std::stop_source{};
        callbacks = _callbacks;

        // Every session starts with its own ATT reservation slot and its own acknowledgement
        // bookkeeping; nothing carried over from a retired generation may survive into it.
        _attPathState = AttPathState::Unknown;
        _attPendingRead.Clear();
        _handshakeAckObserved = false;
        _featureFlagsAckObserved = false;
        _featureFlagsEnabled = false;

        // A natural EOF intentionally leaves its reader joinable. Retire it before creating the
        // replacement so assignment can never destroy a live thread.
        _readerStop.request_stop();
        previousReader = std::move(_readerThread);
        previousTransport = _transport.exchange(nullptr);
        _usingMagicAAP = false;
    }

    if (previousTransport) {
        previousTransport->Close();
    }
    if (previousReader.joinable()) {
        previousReader.request_stop();
        previousReader.join();
    }

    if (cancelled()) {
        std::lock_guard lock{_mutex};
        _connecting = false;
        _lastConnectFailure.store(ConnectFailure::TransportError);
        return false;
    }

    // BeginAttempt resets the recorder before this generation opens. AdvertisementMatched is
    // recorded afterwards so it cannot be erased by that reset or attributed to an old session.
    Diagnostics::Recorder::Instance().BeginAttempt(attempt.generation);
    Diagnostics::Recorder::Instance().RecordForAttempt(
        attempt.generation, Diagnostics::Step::AdvertisementMatched, true,
        "address=" + Diagnostics::Recorder::MaskAddress(attempt.address));
    AttemptPhaseGuard attemptPhase{attempt};

    ConnectFailure failure = ConnectFailure::None;
    auto transport = ConnectTransport(attempt, stopToken, failure);
    _lastConnectFailure.store(failure);

    {
        std::lock_guard lock{_mutex};
        if (cancelled()) {
            _connecting = false;
            _lastConnectFailure.store(ConnectFailure::TransportError);
        }
    }
    if (!transport) {
        {
            std::lock_guard lock{_mutex};
            if (_connecting) {
                _connecting = false;
            }
        }
        // Only a transport that really came up is recorded as connected; the reason is the
        // one ConnectTransport determined, never a generic "started".
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::TransportConnect, false,
            DescribeFailure(failure));
        LOG(Error, "AAP: All supported transport methods failed");
        return false;
    }

    {
        std::lock_guard lock{_mutex};
        _transport.store(transport);
        _usingMagicAAP = transport->Kind() == AAPTransportKind::MagicAAP;
    }
    Diagnostics::Recorder::Instance().RecordForAttempt(
        attempt.generation, Diagnostics::Step::TransportConnect, true,
        transport->Kind() == AAPTransportKind::MagicAAP ? "MagicAAP device interface established"
                                                        : "L2CAP SEQPACKET link established");
    LOG(Info, "AAP: Transport connected; starting handshake");

    const bool initialized = InitializeConnection(attempt, stopToken);
    const bool connectionCancelled = cancelled();
    if (!initialized || connectionCancelled) {
        transport->Close();
        std::lock_guard lock{_mutex};
        _transport.store(std::shared_ptr<AAPTransport>{});
        _usingMagicAAP = false;
        _connecting = false;
        _lastConnectFailure.store(
            connectionCancelled ? ConnectFailure::TransportError
                                : ConnectFailure::HandshakeFailure);
        return false;
    }

    std::stop_source readerStop;
    {
        std::lock_guard lock{_mutex};
        if (cancelled()) {
            _connecting = false;
            _lastConnectFailure.store(ConnectFailure::TransportError);
            transport->Close();
            return false;
        }
        _readerStop = std::move(readerStop);
        _readerThread =
            std::jthread{[attempt, transport, callbacks, token = _readerStop.get_token(), this] {
                ReaderLoop(attempt, transport, token, callbacks);
            }};
        _connected = true;
        _connecting = false;
        _lastConnectFailure.store(ConnectFailure::None);
    }

    if (callbacks.onConnected) {
        callbacks.onConnected(attempt);
    }
    // The session is live: Disconnect() (or the reader noticing EOF) now owns the end of the
    // diagnostic sequence, not this guard.
    attemptPhase.Succeeded();
    return true;
}

std::shared_ptr<AAPTransport>
Manager::ConnectTransport(AAPAttempt attempt, std::stop_token stopToken, ConnectFailure &failure)
{
    auto setPending = [this](const std::shared_ptr<AAPTransport> &transport) {
        std::lock_guard lock{_pendingConnectMutex};
        _pendingTransport = transport;
    };
    auto clearPending = [this](const std::shared_ptr<AAPTransport> &transport) {
        std::lock_guard lock{_pendingConnectMutex};
        if (_pendingTransport == transport) {
            _pendingTransport.reset();
        }
    };

    const auto cancelled = [&] {
        return stopToken.stop_requested() || _connectStop.get_token().stop_requested() ||
               _cancelledGeneration.load() == attempt.generation;
    };

    LOG(Info, "AAP: Attempting L2CAP SEQPACKET connection to {:016X} on PSM {}", attempt.address,
        kPSM);
    auto seqPacket = _transportFactory
                         ? _transportFactory(AAPTransportKind::SeqPacket)
                         : std::shared_ptr<AAPTransport>(CreateSeqPacketTransport().release());
    setPending(seqPacket);
    const bool seqConnected = seqPacket->Connect(attempt.address, stopToken);
    clearPending(seqPacket);
    if (seqConnected && !cancelled()) {
        failure = ConnectFailure::None;
        return seqPacket;
    }
    seqPacket->Close();
    failure = ConnectFailure::ServiceUnavailable;

    if (cancelled()) {
        failure = ConnectFailure::TransportError;
        return nullptr;
    }

    if (!MagicAAPWinRT::MagicAAPWinRTClient::IsDriverInstalled()) {
        LOG(Info, "AAP: MagicAAP driver not installed");
        failure = ConnectFailure::DriverMissing;
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::DriverOpen, false, "driver not installed");
        return nullptr;
    }
    if (!MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning()) {
        LOG(Warn, "AAP: MagicAAP driver installed but not running");
        failure = ConnectFailure::DriverMissing;
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::DriverOpen, false, "driver not running");
        return nullptr;
    }
    Diagnostics::Recorder::Instance().RecordForAttempt(
        attempt.generation, Diagnostics::Step::DriverOpen, true, "driver installed and running");

    auto magicAAP = _transportFactory
                        ? _transportFactory(AAPTransportKind::MagicAAP)
                        : std::shared_ptr<AAPTransport>(CreateMagicAAPTransport().release());
    setPending(magicAAP);
    const bool magicConnected = magicAAP->Connect(attempt.address, stopToken);
    clearPending(magicAAP);
    if (magicConnected && !cancelled()) {
        failure = ConnectFailure::None;
        return magicAAP;
    }
    // The step is named DriverOpen, so it is only satisfied once the device interface was
    // actually handed to us; a rejected open must not look like a successful one.
    Diagnostics::Recorder::Instance().RecordForAttempt(
        attempt.generation, Diagnostics::Step::DriverOpen, false,
        stopToken.stop_requested() || _connectStop.get_token().stop_requested()
            ? "device interface open cancelled"
            : "device interface open rejected");
    magicAAP->Close();
    failure = ConnectFailure::TransportError;
    return nullptr;
}

void Manager::Disconnect()
{
    CancelConnect();
    std::lock_guard connectLock{_connectMutex};
    Callbacks::FnOnDisconnectedT callback;
    AAPAttempt attempt;
    std::shared_ptr<AAPTransport> transport;
    std::jthread reader;

    {
        std::lock_guard lock{_mutex};
        _readerStop.request_stop();
        attempt = _attempt;
        const bool wasConnected = _connected.exchange(false);
        _connecting = false;
        _headTrackingActive = false;
        _usingMagicAAP = false;
        transport = _transport.exchange(nullptr);
        reader = std::move(_readerThread);
        if (wasConnected) {
            callback = _callbacks.onDisconnected;
        }
    }

    // Fixed shutdown order: Close wakes Receive, request stop is already active, then join.
    if (transport) {
        transport->Close();
    }
    if (reader.joinable()) {
        reader.join();
    }

    {
        std::lock_guard lock{_mutex};
        _noiseControlMode.reset();
        _conversationalAwarenessState.reset();
        _loudSoundReductionState.reset();
        _attPathState = AttPathState::Unknown;
        _attPendingRead.Clear();
        _handshakeAckObserved = false;
        _featureFlagsAckObserved = false;
        _featureFlagsEnabled = false;
    }

    LOG(Info, "AAP: Disconnected");
    Diagnostics::Recorder::Instance().RecordForAttempt(
        attempt.generation, Diagnostics::Step::Disconnect, true);
    // The attempt is over: the next scanner/advertisement events belong to the following
    // generation, and this one stays in the report as the most recent attempt.
    Diagnostics::Recorder::Instance().EndAttempt(attempt.generation);
    if (callback) {
        callback(attempt);
    }
}

bool Manager::IsConnected() const
{
    return _connected;
}

AAPAttempt Manager::GetLastAttempt() const
{
    std::lock_guard lock{_mutex};
    return _attempt;
}

bool Manager::IsCurrentAttempt(AAPAttempt attempt) const
{
    std::lock_guard lock{_mutex};
    return IsCurrentAttemptLocked(attempt);
}

bool Manager::IsCurrentAttemptLocked(AAPAttempt attempt) const
{
    // Both halves of the session identity have to agree: the same generation number reused
    // for another device is not the same session.
    return attempt.generation != 0 && _connected && _attempt.address == attempt.address &&
           _attempt.generation == attempt.generation;
}

bool Manager::SetNoiseControlMode(NoiseControlMode mode)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set noise control mode - not connected");
        return false;
    }

    auto packet = Packets::BuildNoiseControlPacket(mode);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set noise control mode to {}", Helper::ToString(mode).toStdString());
    return true;
}

std::optional<NoiseControlMode> Manager::GetNoiseControlMode() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _noiseControlMode;
}

bool Manager::SetConversationalAwareness(bool enable)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set conversational awareness - not connected");
        return false;
    }
    {
        std::lock_guard lock{_mutex};
        if (!_featureFlagsEnabled) {
            LOG(Warn, "AAP: Conversational awareness unavailable, feature flags were not sent");
            return false;
        }
    }

    auto packet = Packets::BuildConversationalAwarenessPacket(enable);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set conversational awareness to {}", enable ? "enabled" : "disabled");
    return true;
}

std::optional<ConversationalAwarenessState> Manager::GetConversationalAwarenessState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _conversationalAwarenessState;
}

bool Manager::SetPersonalizedVolume(bool enable)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set personalized volume - not connected");
        return false;
    }

    auto packet = Packets::BuildPersonalizedVolumePacket(enable);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set personalized volume to {}", enable ? "enabled" : "disabled");
    return true;
}

std::optional<PersonalizedVolumeState> Manager::GetPersonalizedVolumeState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _personalizedVolumeState;
}

bool Manager::SetAutomaticEarDetection(bool enable)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set automatic ear detection - not connected");
        return false;
    }

    auto packet = Packets::BuildAutomaticEarDetectionPacket(enable);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set automatic ear detection to {}", enable ? "enabled" : "disabled");
    return true;
}

std::optional<bool> Manager::GetAutomaticEarDetectionState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _automaticEarDetectionState;
}

bool Manager::SetLoudSoundReduction(bool enable)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set loud sound reduction - not connected");
        return false;
    }

    auto packet = Packets::BuildLoudSoundReductionPacket(enable);
    if (!SendPacket(packet)) {
        return false;
    }

    // The CCCD (0x1C) does not work, so the firmware state can only be read back. The
    // read-back uses the same single-outstanding reservation as the probe: while a read is
    // still in flight no second frame is put on the wire, and an unconfirmed ATT path never
    // gets speculative traffic.
    if (GetAttPathState() == AttPathState::Confirmed) {
        RequestAttRead(GetLastAttempt(), Att::kLoudSoundReductionHandle);
    }

    LOG(Info, "AAP: Set loud sound reduction to {}", enable ? "enabled" : "disabled");
    return true;
}

std::optional<LoudSoundReductionState> Manager::GetLoudSoundReductionState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _loudSoundReductionState;
}

bool Manager::SetPpeCapLevel(uint8_t level)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set PPE cap level - not connected");
        return false;
    }
    {
        std::lock_guard lock{_mutex};
        if (!_featureFlagsEnabled) {
            LOG(Warn, "AAP: PPE/adaptive controls unavailable, feature flags were not sent");
            return false;
        }
    }

    // Control command 0x38 is PPE (hearing-protection) cap level, range 0-100 per
    // Docs/AAP-Protocol-Spec.md §7. Values outside the documented range (including stale
    // pre-P5 inputs from the old 0-50 adaptive-transparency UI) clamp at the domain edge.
    level = NormalizePpeCapLevel(level);

    auto packet = Packets::BuildPpeCapLevelPacket(level);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set PPE cap level to {}", level);
    return true;
}

std::optional<uint8_t> Manager::GetPpeCapLevel() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _ppeCapLevel;
}

bool Manager::SetAdaptiveNoiseLevel(uint8_t level)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set adaptive noise level - not connected");
        return false;
    }
    {
        std::lock_guard lock{_mutex};
        if (!_featureFlagsEnabled) {
            LOG(Warn, "AAP: Adaptive noise unavailable, feature flags were not sent");
            return false;
        }
    }

    if (level > 100) {
        level = 100;
    }

    auto packet = Packets::BuildAdaptiveNoisePacket(level);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set adaptive noise level to {}", level);
    return true;
}

bool Manager::StartHeadTracking()
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot start head tracking - not connected");
        return false;
    }

    if (_headTrackingActive) {
        LOG(Warn, "AAP: Head tracking already active");
        return true;
    }

    if (!SendPacket(Packets::StartHeadTracking)) {
        return false;
    }

    _headTrackingActive = true;
    LOG(Info, "AAP: Started head tracking");
    return true;
}

bool Manager::StopHeadTracking()
{
    if (!_connected) {
        return false;
    }

    if (!_headTrackingActive) {
        return true;
    }

    if (!SendPacket(Packets::StopHeadTracking)) {
        return false;
    }

    _headTrackingActive = false;
    LOG(Info, "AAP: Stopped head tracking");
    return true;
}

bool Manager::IsHeadTrackingActive() const
{
    return _headTrackingActive;
}

void Manager::SetCallbacks(Callbacks callbacks)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _callbacks = std::move(callbacks);
}

//////////////////////////////////////////////////
// ATT path probe (blueprint 0.3 / P3)
//

AttPathState Manager::GetAttPathState() const
{
    bool windowElapsed = false;
    AttPathState state = AttPathState::Unknown;
    AAPAttempt attempt;
    {
        std::lock_guard<std::mutex> lock{_mutex};
        attempt = _attempt;
        state = _attPathState;
        // Lazily resolve a probe whose window has closed: whoever asks first gets the final
        // answer, so no thread or timer is needed. ClearIfElapsed uses the same half-open
        // rule as the reader path, and a confirmed path is never downgraded by a later
        // read-back that happens to time out. A Pending state with no live reservation can
        // only mean the window elapsed, so it cannot get stuck.
        _attPendingRead.ClearIfElapsed(_steadyMillis());
        if (state == AttPathState::Pending && !_attPendingRead.HasPending()) {
            _attPathState = AttPathState::Unconfirmed;
            state = AttPathState::Unconfirmed;
            windowElapsed = true;
        }
    }
    if (windowElapsed) {
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::AttProbe, false, "no ATT answer within 500ms");
    }
    return state;
}

bool Manager::MarkAttConfirmedLocked()
{
    // Only the first answer is an event the UI has to hear; a later read-back refreshes the
    // cached value without re-announcing that the ATT path works.
    const bool firstConfirmation = _attPathState != AttPathState::Confirmed;
    _attPathState = AttPathState::Confirmed;
    return firstConfirmation;
}

bool Manager::RequestAttRead(AAPAttempt attempt, uint16_t handle)
{
    const int64_t nowMs = _steadyMillis();
    Att::PendingReadTracker::StartResult start = Att::PendingReadTracker::StartResult::Rejected;
    bool announcesProbe = false;
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (!IsActiveAttemptLocked(attempt)) {
            return false;
        }
        start = _attPendingRead.Start(attempt, handle, nowMs + kAttProbeWindowMs, nowMs);
        if (start == Att::PendingReadTracker::StartResult::Started) {
            // The reservation and the Pending state are installed in the same critical
            // section, before the request is written. A device that answers immediately is
            // therefore already attributable, and the observable state can never be written
            // back to Pending after a response has confirmed it.
            announcesProbe = _attPathState != AttPathState::Confirmed;
            if (announcesProbe) {
                _attPathState = AttPathState::Pending;
            }
        }
    }
    }

    if (start == Att::PendingReadTracker::StartResult::Rejected) {
        LOG(Warn, "AAP: ATT read of handle 0x{:02X} deferred, another read is outstanding",
            static_cast<int>(handle));
        return false;
    }
    if (start == Att::PendingReadTracker::StartResult::AlreadyPending) {
        // Idempotent repeat: the original request still owns the one window, so no second
        // frame and no second deadline are added to the session.
        LOG(Trace, "AAP: ATT read of handle 0x{:02X} already outstanding",
            static_cast<int>(handle));
        return true;
    }

    if (!SendPacket(Att::BuildReadRequest(handle), attempt)) {
        bool reportSendFailure = false;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            // Release only the reservation this call made. A response that arrived in the
            // meantime already consumed it and confirmed the path, which must survive a
            // failed follow-up read.
            if (_attPendingRead.ReleaseIf(attempt, handle) && announcesProbe) {
                _attPathState = AttPathState::Unconfirmed;
                reportSendFailure = true;
            }
        }
        if (reportSendFailure) {
            Diagnostics::Recorder::Instance().RecordForAttempt(
                attempt.generation, Diagnostics::Step::AttProbe, false, "send failed");
        }
        LOG(Warn, "AAP: Failed to send ATT probe (read handle 0x{:02X})", static_cast<int>(handle));
        return false;
    }

    if (announcesProbe && GetAttPathState() == AttPathState::Pending) {
        // An immediate response may already have confirmed the path inside Send(). In that
        // case no later "waiting" line is emitted after the confirmation.
        Diagnostics::Recorder::Instance().RecordPendingForAttempt(
            attempt.generation, Diagnostics::Step::AttProbe, "probe sent, waiting up to 500ms");
        LOG(Info, "AAP: Sent ATT probe (read handle 0x{:02X}), waiting up to 500ms",
            static_cast<int>(handle));
    }
    else {
        LOG(Debug, "AAP: Read back ATT handle 0x{:02X}", static_cast<int>(handle));
    }
    return true;
}

void Manager::StartAttProbe(AAPAttempt attempt)
{
    // One-shot probe for the session: ask for the loud-sound-reduction value. Any ATT answer
    // (even an error response) proves the firmware accepts ATT on this pipe; no answer inside
    // the window marks the path unconfirmed, and the probe is never retried.
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (!IsActiveAttemptLocked(attempt) || _attPathState != AttPathState::Unknown ||
            _attPendingRead.HasPending())
        {
            return;
        }
    }
    RequestAttRead(attempt, Att::kLoudSoundReductionHandle);
}

void Manager::ProcessAttFrame(
    const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks)
{
    const int64_t nowMs = _steadyMillis();
    std::optional<AttPathState> probeEvent;
    std::optional<LoudSoundReductionState> valueEvent;
    const char *confirmReason = nullptr;

    {
        std::lock_guard<std::mutex> lock{_mutex};
        // A frame from a retired session is dropped before it can touch anything: neither the
        // probe state, the cached value, nor the diagnostic log may be written on behalf of
        // an attempt that is no longer current.
        if (!IsActiveAttemptLocked(attempt)) {
            return;
        }
        _attPendingRead.ClearIfElapsed(nowMs);

        if (Att::IsReadResponse(packet)) {
            // The frame carries no handle, so it may only be attributed to the read this
            // session still has outstanding. An unasked-for read response is not proof that
            // ATT works and must never be read as the loud-sound-reduction value.
            if (_attPendingRead.Match(attempt, std::nullopt, nowMs) !=
                Att::PendingReadTracker::MatchResult::Matched)
            {
                LOG(Warn, "AAP: Dropped unattributed ATT read response ({} value bytes)",
                    packet.size() - 1);
                return;
            }
            const std::optional<uint16_t> requested = _attPendingRead.Handle();
            _attPendingRead.Clear();
            if (MarkAttConfirmedLocked()) {
                probeEvent = AttPathState::Confirmed;
                confirmReason = "confirmed by read response";
            }
            LOG(Info, "AAP: ATT path confirmed (read response, {} value bytes)", packet.size() - 1);

            // The handle comes from the reservation, never from the response.
            if (requested.has_value() && *requested == Att::kLoudSoundReductionHandle) {
                auto value = Att::ParseReadResponse(packet);
                if (value.has_value() && !value->empty()) {
                    if ((*value)[0] == 0x01) {
                        valueEvent = LoudSoundReductionState::Enabled;
                    }
                    else if ((*value)[0] == 0x00) {
                        valueEvent = LoudSoundReductionState::Disabled;
                    }
                }
                if (valueEvent.has_value()) {
                    _loudSoundReductionState = valueEvent;
                }
            }
            else {
                LOG(Warn, "AAP: ATT read response answers handle 0x{:02X}, ignored",
                    requested.has_value() ? static_cast<int>(*requested) : 0);
            }
        }
        else if (Att::IsErrorResponse(packet)) {
            const auto error = Att::ParseErrorResponse(packet);
            if (!error || error->requestOpcode != Att::kOpcodeReadRequest) {
                return;
            }
            // The error names the request it answers, so it only counts when that handle is
            // the outstanding read of this session. It proves the ATT stack is reachable but
            // carries no attribute value: loud sound reduction keeps whatever the last
            // successful read reported.
            if (_attPendingRead.Match(attempt, error->handle, nowMs) !=
                Att::PendingReadTracker::MatchResult::Matched)
            {
                LOG(Warn, "AAP: Dropped unattributed ATT error response (handle 0x{:02X})",
                    static_cast<int>(error->handle));
                return;
            }
            _attPendingRead.Clear();
            if (MarkAttConfirmedLocked()) {
                probeEvent = AttPathState::Confirmed;
                confirmReason = "confirmed by error response";
            }
            LOG(Info, "AAP: ATT path confirmed (error response, opcode 0x{:02X}, code 0x{:02X})",
                error->requestOpcode, error->error);
        }
        else if (Att::IsNotification(packet)) {
            // A notification answers no request, so the only attribution available is the
            // handle it carries: anything other than the loud-sound-reduction handle belongs
            // to another attribute and is ignored.
            const auto notification = Att::ParseNotification(packet);
            if (!notification || notification->first != Att::kLoudSoundReductionHandle ||
                notification->second.empty())
            {
                return;
            }
            const uint8_t raw = notification->second[0];
            if (raw == 0x01) {
                valueEvent = LoudSoundReductionState::Enabled;
            }
            else if (raw == 0x00) {
                valueEvent = LoudSoundReductionState::Disabled;
            }
            else {
                return;
            }
            LOG(Info, "AAP: Loud sound reduction (ATT notify): {}", static_cast<int>(*valueEvent));
            _loudSoundReductionState = valueEvent;
        }
    }

    // Diagnostics and callbacks stay outside the state mutex (P2 callback contract).
    if (probeEvent.has_value()) {
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::AttProbe, true, confirmReason);
        if (callbacks.onAttProbeResult) {
            callbacks.onAttProbeResult(*probeEvent, attempt);
        }
    }
    if (valueEvent.has_value() && callbacks.onLoudSoundReductionChanged) {
        callbacks.onLoudSoundReductionChanged(*valueEvent, attempt);
    }
}

bool Manager::IsActiveAttemptLocked(AAPAttempt attempt) const
{
    return attempt.generation != 0 && _attempt.address == attempt.address &&
           _attempt.generation == attempt.generation && (_connected || _connecting);
}

bool Manager::SendPacket(const std::vector<uint8_t> &packet)
{
    AAPAttempt attempt;
    {
        std::lock_guard lock{_mutex};
        attempt = _attempt;
    }
    return SendPacket(packet, attempt);
}

    std::shared_ptr<AAPTransport> transport;
    {
        std::lock_guard lock{_mutex};
        if (!IsActiveAttemptLocked(attempt)) {
            return false;
        }
        transport = _transport.load();
    }
    return SendPacket(packet, attempt, transport);
}

bool Manager::SendPacket(
    const std::shared_ptr<AAPTransport> &transport, const std::vector<uint8_t> &packet,
    std::stop_token stopToken)
{
    if (!transport || stopToken.stop_requested() || _connectStop.get_token().stop_requested()) {
        return false;
    }
    return transport->Send(packet, stopToken);
}

bool Manager::SendPacket(
    const std::vector<uint8_t> &packet, AAPAttempt attempt,
    const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken)
{
    {
        std::lock_guard lock{_mutex};
        if (!IsActiveAttemptLocked(attempt) || _transport.load() != transport) {
            return false;
        }
    }
    if (!SendPacket(transport, packet, stopToken)) {
        return false;
    }

    LOG(Trace, "AAP: Sent {} bytes via {}", packet.size(),
        transport->Kind() == AAPTransportKind::MagicAAP ? "MagicAAP" : "SEQPACKET");
    return true;
}

void Manager::ProcessPacket(const std::vector<uint8_t> &packet)
{
    AAPAttempt attempt;
    Callbacks callbacks;
    {
        std::lock_guard lock{_mutex};
        attempt = _attempt;
        callbacks = _callbacks;
    }
    ProcessPacket(packet, attempt, callbacks);
}

void Manager::ProcessPacket(
    const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks)
{
    if (!IsCurrentAttempt(attempt)) {
        return;
    }

    const auto validation = AAPMessage::Validate(packet);
    if (validation != AAPMessage::ValidationResult::Valid) {
        LOG(Warn, "AAP: Rejected invalid {}-byte record before processing: {}", packet.size(),
            AAPMessage::Describe(validation));
        return;
    }

    // ATT frames on the AAP pipe (blueprint 0.3): anything that is not an
    // L2CAP AAP frame (`04 00 04 00` header) and matches a known ATT opcode is
    // routed to the ATT handlers first, so ATT responses can never be
    // misparsed as settings notifications.
    if (Att::IsAttFrame(packet)) {
        ProcessAttFrame(packet, attempt, callbacks);
        return;
    }

    // Handshake ACK (`01 00 04 00`, Docs/AAP-Protocol-Spec.md connection sequence). This
    // record is the only device-side proof that the handshake was accepted: the line written
    // on the connect path says "send accepted, ACK unconfirmed" until it arrives on the
    // current session. Devices that never answer keep the honest wording instead of being
    // claimed as acknowledged.
    if (IsHandshakeAck(packet)) {
        bool firstObservation = false;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            if (!IsCurrentAttemptLocked(attempt)) {
                return;
            }
            firstObservation = !_handshakeAckObserved;
            _handshakeAckObserved = true;
        }
        if (firstObservation) {
            LOG(Info, "AAP: Handshake acknowledged by the device");
            Diagnostics::Recorder::Instance().RecordForAttempt(
                attempt.generation, Diagnostics::Step::Handshake, true, "device ACK observed");
        }
        return;
    }

    // Feature flags ACK (`04 00 04 00 2B 00`): the same rule for the 0x4D packet. Without it
    // the connection still works, but capabilities such as Adaptive may be answered with
    // another mode, which the report then shows as unconfirmed rather than assumed.
    if (IsFeatureFlagsAck(packet)) {
        bool firstObservation = false;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            if (!IsCurrentAttemptLocked(attempt)) {
                return;
            }
            firstObservation = !_featureFlagsAckObserved;
            _featureFlagsAckObserved = true;
        }
        if (firstObservation) {
            LOG(Info, "AAP: Feature flags acknowledged by the device");
            Diagnostics::Recorder::Instance().RecordForAttempt(
                attempt.generation, Diagnostics::Step::FeatureFlags, true, "device ACK observed");
        }
        return;
    }

    // Noise control notification
    if (IsNoiseControlNotification(packet)) {
        auto mode = ParseNoiseControlNotification(packet);
        if (mode.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _noiseControlMode = mode;
            }
            LOG(Info, "AAP: Noise control mode changed to {}",
                Helper::ToString(mode.value()).toStdString());
            if (callbacks.onNoiseControlChanged) {
                callbacks.onNoiseControlChanged(mode.value(), attempt);
            }
        }
        return;
    }

    // Conversational awareness notification
    if (IsConversationalAwarenessNotification(packet)) {
        auto state = ParseConversationalAwarenessState(packet);
        if (state.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _conversationalAwarenessState = state;
            }
            LOG(Info, "AAP: Conversational awareness state: {}",
                Helper::ToString(state.value()).toStdString());
            if (callbacks.onConversationalAwarenessChanged) {
                callbacks.onConversationalAwarenessChanged(state.value(), attempt);
            }
        }
        return;
    }

    // Personalized volume notification
    if (IsPersonalizedVolumeNotification(packet)) {
        auto state = ParsePersonalizedVolumeState(packet);
        if (state.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _personalizedVolumeState = state;
            }
            LOG(Info, "AAP: Personalized volume state: {}", static_cast<int>(state.value()));
            if (callbacks.onPersonalizedVolumeChanged) {
                callbacks.onPersonalizedVolumeChanged(state.value(), attempt);
            }
        }
        return;
    }

    // Automatic ear detection notification
    if (IsAutomaticEarDetectionNotification(packet)) {
        auto state = ParseAutomaticEarDetectionState(packet);
        if (state.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _automaticEarDetectionState = state;
            }
            LOG(Info, "AAP: Automatic ear detection: {}", state.value() ? "enabled" : "disabled");
            if (callbacks.onAutomaticEarDetectionChanged) {
                callbacks.onAutomaticEarDetectionChanged(state.value(), attempt);
            }
        }
        return;
    }

    // Volume swipe echo (control identifier 0x25). P3: this is not loud sound reduction, so
    // the frame is recognised and dropped instead of being pushed into the LSR state. The
    // project exposes no volume swipe control yet; loud sound reduction only ever changes
    // through an ATT answer to the outstanding read of handle 0x1B.
    if (IsVolumeSwipeNotification(packet)) {
        const auto mode = ParseVolumeSwipeMode(packet);
        LOG(Trace, "AAP: Volume swipe echo (0x25) mode={}, ignored",
            mode.has_value() ? static_cast<int>(*mode) : -1);
        return;
    }

    // PPE cap level notification (control 0x38)
    if (IsPpeCapLevelNotification(packet)) {
        auto level = ParsePpeCapLevel(packet);
        if (level.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _ppeCapLevel = level;
            }
            LOG(Info, "AAP: PPE cap level: {}", level.value());
            if (callbacks.onPpeCapLevelChanged) {
                callbacks.onPpeCapLevelChanged(level.value(), attempt);
            }
        }
        return;
    }

    // Speaking level notification (conversational awareness active)
    if (IsSpeakingLevelNotification(packet)) {
        auto level = ParseSpeakingLevel(packet);
        if (level.has_value() && callbacks.onSpeakingLevelChanged) {
            callbacks.onSpeakingLevelChanged(level.value(), attempt);
        }
        return;
    }

    // Ear detection notification
    if (IsEarDetectionNotification(packet)) {
        auto earStatus = ParseEarDetection(packet);
        if (earStatus.has_value() && callbacks.onEarDetectionChanged) {
            callbacks.onEarDetectionChanged(earStatus->first, earStatus->second, attempt);
        }
        return;
    }

    // Only opcode 0x0017 carries head-tracking data. Unknown AAP records are
    // valid for diagnostics but must never fall through into this parser.
    if (_headTrackingActive && packet.size() >= 56 && packet[0] == 0x04 && packet[1] == 0x00 &&
        packet[2] == 0x04 && packet[3] == 0x00 && packet[4] == 0x17 && packet[5] == 0x00)
    {
        auto trackingData = ParseHeadTrackingData(packet);
        if (trackingData.has_value() && callbacks.onHeadTrackingData) {
            callbacks.onHeadTrackingData(trackingData.value(), attempt);
        }
        return;
    }

    // Log unknown packets for debugging
    LOG(Trace, "AAP: Received unknown packet ({} bytes)", packet.size());
}

void Manager::ReaderLoop(
    AAPAttempt attempt, std::shared_ptr<AAPTransport> transport, std::stop_token stopToken,
    Callbacks callbacks)
{
    while (transport && !stopToken.stop_requested()) {
        auto packet = transport->Receive(stopToken);
        if (!packet.has_value()) {
            if (!stopToken.stop_requested()) {
                LOG(Info, "AAP: Transport receive ended");
            }
            break;
        }
        ProcessPacket(*packet, attempt, callbacks);
    }

    if (stopToken.stop_requested()) {
        return;
    }

    AAPAttempt active;
    Callbacks::FnOnDisconnectedT callback;
    {
        std::lock_guard lock{_mutex};
        const bool current =
            _attempt.address == attempt.address && _attempt.generation == attempt.generation;
        if (!current || !_connected) {
            return;
        }
        _connected = false;
        _usingMagicAAP = false;
        _transport.store(std::shared_ptr<AAPTransport>{});
        // The session is over, so its ATT reservation and its unacknowledged handshake
        // bookkeeping go with it; a later probe belongs to a new generation.
        _attPathState = AttPathState::Unknown;
        _attPendingRead.Clear();
        _handshakeAckObserved = false;
        _featureFlagsAckObserved = false;
        _featureFlagsEnabled = false;
        active = _attempt;
        callback = _callbacks.onDisconnected;
    }

    // Unexpected loss closes the diagnostic sequence too, so the next advertisement match is
    // recorded for the attempt it leads to rather than for the session that just ended.
    Diagnostics::Recorder::Instance().RecordForAttempt(
        active.generation, Diagnostics::Step::Disconnect, true, "transport closed by device");
    Diagnostics::Recorder::Instance().EndAttempt(active.generation);

    if (callback) {
        callback(active);
    }
}

bool Manager::WaitCancellable(std::chrono::milliseconds delay, std::stop_token callerStop)
{
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (std::chrono::steady_clock::now() < deadline) {
        if (callerStop.stop_requested() || _connectStop.get_token().stop_requested()) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds{10}));
    }
    return !callerStop.stop_requested() && !_connectStop.get_token().stop_requested();
}

// Protocol timing constants
constexpr auto kPacketProcessingDelay = std::chrono::milliseconds(100);

bool Manager::InitializeConnection(AAPAttempt attempt, std::stop_token stopToken)
{
    if (stopToken.stop_requested() || _connectStop.get_token().stop_requested()) {
        return false;
    }

    std::shared_ptr<AAPTransport> transport;
    {
        std::lock_guard lock{_mutex};
        if (!IsActiveAttemptLocked(attempt)) {
            return false;
        }
        transport = _transport.load();
    }
    // The connection gate remains closed until the handshake completes. Pin this attempt's
    // transport for every initialization write and retain the caller's cancellation token.
    if (!SendPacket(Packets::Handshake, attempt, transport, stopToken)) {
        LOG(Error, "AAP: Failed to send handshake");
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::Handshake, false, "send failed");
        return false;
    }
    LOG(Info, "AAP: Sent handshake");
    Diagnostics::Recorder::Instance().RecordPendingForAttempt(
        attempt.generation, Diagnostics::Step::Handshake, "send accepted, ACK unconfirmed");

    if (!WaitCancellable(kPacketProcessingDelay, stopToken)) {
        return false;
    }

    // Enable features (Conversational Awareness, Adaptive Transparency). A rejected write
    // disables capability-dependent commands for this session but leaves basic AAP usable.
    const bool featuresSent = SendPacket(Packets::EnableFeatures, attempt, transport, stopToken);
    {
        std::lock_guard lock{_mutex};
        _featureFlagsEnabled = featuresSent && IsActiveAttemptLocked(attempt);
    }
    if (!featuresSent) {
        LOG(Warn, "AAP: Failed to send enable features packet");
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::FeatureFlags, false,
            "send failed, features degraded (Adaptive may be answered with another mode)");
    }
    else {
        LOG(Info, "AAP: Sent enable features");
        Diagnostics::Recorder::Instance().RecordPendingForAttempt(
            attempt.generation, Diagnostics::Step::FeatureFlags, "send accepted, ACK unconfirmed");
    }
    }

    if (!WaitCancellable(kPacketProcessingDelay, stopToken)) {
        return false;
    }

    // Request notifications (battery, ear detection, noise control, etc.)
    if (!SendPacket(Packets::RequestNotifications, attempt, transport, stopToken)) {
        LOG(Error, "AAP: Failed to send request notifications");
        Diagnostics::Recorder::Instance().RecordForAttempt(
            attempt.generation, Diagnostics::Step::NotificationSubscribe, false, "send failed");
        return false;
    }
    LOG(Info, "AAP: Sent request notifications");
    Diagnostics::Recorder::Instance().RecordPendingForAttempt(
        attempt.generation, Diagnostics::Step::NotificationSubscribe,
        "send accepted, ACK unconfirmed");

    // Probe whether the firmware answers ATT frames on this pipe. The reservation is installed
    // before the write, and initialization is an active attempt even before _connected flips.
    StartAttProbe(attempt);

    return true;
}

bool Manager::IsMagicAAPDriverAvailable()
{
    return MagicAAPWinRT::MagicAAPWinRTClient::IsDriverInstalled() &&
           MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning();
}

} // namespace Core::AAP

#else
// Stub implementation for non-Windows platforms
namespace Core::AAP {

Manager::Manager()
    : _steadyMillis{[] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
              .count();
      }}
{
}
Manager::~Manager()
{
    Disconnect();
}
bool Manager::Connect(AAPAttempt, std::stop_token)
{
    return false;
}
void Manager::CancelConnect() {}
void Manager::Disconnect() {}
bool Manager::IsConnected() const
{
    return false;
}
AAPAttempt Manager::GetLastAttempt() const
{
    return {};
}
bool Manager::SetNoiseControlMode(NoiseControlMode)
{
    return false;
}
std::optional<NoiseControlMode> Manager::GetNoiseControlMode() const
{
    return std::nullopt;
}
bool Manager::SetConversationalAwareness(bool)
{
    return false;
}
std::optional<ConversationalAwarenessState> Manager::GetConversationalAwarenessState() const
{
    return std::nullopt;
}
bool Manager::SetPersonalizedVolume(bool)
{
    return false;
}
std::optional<PersonalizedVolumeState> Manager::GetPersonalizedVolumeState() const
{
    return std::nullopt;
}
bool Manager::SetAutomaticEarDetection(bool)
{
    return false;
}
std::optional<bool> Manager::GetAutomaticEarDetectionState() const
{
    return std::nullopt;
}
bool Manager::SetLoudSoundReduction(bool)
{
    return false;
}
std::optional<LoudSoundReductionState> Manager::GetLoudSoundReductionState() const
{
    return std::nullopt;
}
bool Manager::SetPpeCapLevel(uint8_t)
{
    return false;
}
std::optional<uint8_t> Manager::GetPpeCapLevel() const
{
    return std::nullopt;
}
bool Manager::SetAdaptiveNoiseLevel(uint8_t)
{
    return false;
}
bool Manager::StartHeadTracking()
{
    return false;
}
bool Manager::StopHeadTracking()
{
    return false;
}
bool Manager::IsHeadTrackingActive() const
{
    return false;
}
void Manager::SetCallbacks(Callbacks) {}
bool Manager::IsMagicAAPDriverAvailable()
{
    return false;
}
AttPathState Manager::GetAttPathState() const
{
    return AttPathState::Unconfirmed;
}
void Manager::StartAttProbe(AAPAttempt) {}
bool Manager::RequestAttRead(AAPAttempt, uint16_t)
{
    return false;
}
bool Manager::MarkAttConfirmedLocked()
{
    return false;
}
bool Manager::IsCurrentAttempt(AAPAttempt) const
{
    return false;
}
bool Manager::IsCurrentAttemptLocked(AAPAttempt) const
{
    return false;
}
bool Manager::IsActiveAttemptLocked(AAPAttempt) const
{
    return false;
}
void Manager::ProcessAttFrame(const std::vector<uint8_t> &, AAPAttempt, const Callbacks &) {}
bool Manager::SendPacket(const std::vector<uint8_t> &)
{
    return false;
}
bool Manager::SendPacket(
    const std::shared_ptr<AAPTransport> &, const std::vector<uint8_t> &, std::stop_token)
{
    return false;
}
bool Manager::SendPacket(const std::vector<uint8_t> &, AAPAttempt)
{
    return false;
}
bool Manager::SendPacket(
    const std::vector<uint8_t> &, AAPAttempt, const std::shared_ptr<AAPTransport> &,
    std::stop_token)
{
    return false;
}
void Manager::ProcessPacket(const std::vector<uint8_t> &) {}
void Manager::ProcessPacket(const std::vector<uint8_t> &, AAPAttempt, const Callbacks &) {}
void Manager::ReaderLoop(AAPAttempt, std::shared_ptr<AAPTransport>, std::stop_token, Callbacks) {}
bool Manager::WaitCancellable(std::chrono::milliseconds, std::stop_token)
{
    return false;
}
bool Manager::InitializeConnection(AAPAttempt, std::stop_token)
{
    return false;
}
std::shared_ptr<AAPTransport>
Manager::ConnectTransport(AAPAttempt, std::stop_token, ConnectFailure &)
{
    return nullptr;
}

} // namespace Core::AAP

#endif
