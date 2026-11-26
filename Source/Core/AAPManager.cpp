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
#include <exception>
#include <thread>

#if defined APD_OS_WIN

    #include "MagicAAPWinRT.h"

namespace {

// One transport attempt observes caller cancellation, Manager cancellation, and
// its retired generation through a single stop_token. Generation cancellation
// is polled at attempt boundaries; Manager::CancelConnect also requests the
// shared Manager source, which wakes a driver already blocked in Connect.
class TransportAttemptCancellation
{
public:
    TransportAttemptCancellation(
        std::stop_token callerStop, std::stop_source managerStop, uint64_t generation,
        const std::atomic<uint64_t> &cancelledGeneration)
        : _stop{}, _callerCallback{callerStop, [this] noexcept { _stop.request_stop(); }},
          _managerCallback{managerStop.get_token(), [this] noexcept { _stop.request_stop(); }},
          _generation{generation}, _cancelledGeneration{cancelledGeneration}
    {
        IsRequested();
    }

    std::stop_token Token()
    {
        IsRequested();
        return _stop.get_token();
    }

    bool IsRequested()
    {
        if (_cancelledGeneration.load() == _generation) {
            _stop.request_stop();
        }
        return _stop.stop_requested();
    }

private:
    std::stop_source _stop;
    std::stop_callback<std::function<void()>> _callerCallback;
    std::stop_callback<std::function<void()>> _managerCallback;
    uint64_t _generation;
    const std::atomic<uint64_t> &_cancelledGeneration;
};

} // namespace

namespace Core::AAP {

Manager::Manager()
    : _driverInstalledProbe([] { return MagicAAPWinRT::MagicAAPWinRTClient::IsDriverInstalled(); }),
      _driverRunningProbe([] { return MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning(); })
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

    Diagnostics::Recorder::Instance().Reset();
    Diagnostics::Recorder::Instance().Record(
        Diagnostics::Step::TransportConnect, true, "connect attempt started");

    ConnectFailure failure = ConnectFailure::None;
    auto transport = ConnectTransport(attempt, stopToken, failure);
    _lastConnectFailure.store(failure);

    if (cancelled()) {
        if (transport) {
            transport->Close();
        }
        std::lock_guard lock{_mutex};
        if (_connecting) {
            _connecting = false;
        }
        _lastConnectFailure.store(ConnectFailure::TransportError);
        return false;
    }
    if (!transport) {
        std::lock_guard lock{_mutex};
        if (_connecting) {
            _connecting = false;
        }
        LOG(Error, "AAP: All supported transport methods failed");
        return false;
    }

    {
        std::lock_guard lock{_mutex};
        _transport.store(transport);
        _usingMagicAAP = transport->Kind() == AAPTransportKind::MagicAAP;
    }
    LOG(Info, "AAP: Transport connected; starting handshake");

    const bool initialized = InitializeConnection(transport, stopToken);
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

    TransportAttemptCancellation attemptCancellation{
        stopToken, _connectStop, attempt.generation, _cancelledGeneration};
    const auto cancelled = [&] { return attemptCancellation.IsRequested(); };

    LOG(Info, "AAP: Attempting L2CAP SEQPACKET connection to {} on PSM {}",
        Diagnostics::Recorder::MaskAddress(attempt.address), kPSM);
    auto seqPacket = _transportFactory
                         ? _transportFactory(AAPTransportKind::SeqPacket)
                         : std::shared_ptr<AAPTransport>(CreateSeqPacketTransport().release());
    setPending(seqPacket);
    const bool seqConnected = seqPacket->Connect(attempt.address, attemptCancellation.Token());
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

    if (!_driverInstalledProbe()) {
        if (cancelled()) {
            failure = ConnectFailure::TransportError;
            return nullptr;
        }
        LOG(Info, "AAP: MagicAAP driver not installed");
        failure = ConnectFailure::DriverMissing;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver not installed");
        return nullptr;
    }
    if (cancelled()) {
        failure = ConnectFailure::TransportError;
        return nullptr;
    }
    if (!_driverRunningProbe()) {
        if (cancelled()) {
            failure = ConnectFailure::TransportError;
            return nullptr;
        }
        LOG(Warn, "AAP: MagicAAP driver installed but not running");
        failure = ConnectFailure::DriverMissing;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver not running");
        return nullptr;
    }

    std::shared_ptr<AAPTransport> magicAAP;
    bool magicConnected = false;
    try {
        magicAAP = _transportFactory
                       ? _transportFactory(AAPTransportKind::MagicAAP)
                       : std::shared_ptr<AAPTransport>(CreateMagicAAPTransport().release());
        if (magicAAP) {
            setPending(magicAAP);
            magicConnected = magicAAP->Connect(attempt.address, attemptCancellation.Token());
        }
    }
    catch (const std::exception &) {
        // A transport constructor/startup exception is still a failed driver
        // open unless cancellation won the race with startup.
        magicConnected = false;
    }
    clearPending(magicAAP);
    if (cancelled()) {
        if (magicAAP) {
            magicAAP->Close();
        }
        failure = ConnectFailure::TransportError;
        return nullptr;
    }
    if (magicConnected) {
        Diagnostics::Recorder::Instance().Record(Diagnostics::Step::DriverOpen, true);
        failure = ConnectFailure::None;
        return magicAAP;
    }
    if (magicAAP) {
        magicAAP->Close();
    }
    if (cancelled()) {
        failure = ConnectFailure::TransportError;
        return nullptr;
    }
    const auto bindStatus =
        magicAAP ? magicAAP->LastBindStatus() : MagicAAPWinRT::MagicAAPBindStatus::TransportError;
    switch (bindStatus) {
    case MagicAAPWinRT::MagicAAPBindStatus::NoMatch:
        failure = ConnectFailure::NoMatch;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false,
            "no interface proves the bound address and ContainerId");
        break;
    case MagicAAPWinRT::MagicAAPBindStatus::OpenFailed:
        failure = ConnectFailure::OpenFailed;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "exact target interface open failed");
        break;
    case MagicAAPWinRT::MagicAAPBindStatus::DriverMissing:
        failure = ConnectFailure::DriverMissing;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver unavailable during bind");
        break;
    case MagicAAPWinRT::MagicAAPBindStatus::TransportError:
    case MagicAAPWinRT::MagicAAPBindStatus::Cancelled:
    default:
        failure = ConnectFailure::TransportError;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "transport or receiver startup failed");
        break;
    }
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
        _attPathState.store(AttPathState::Unknown);
        _attProbeDeadlineMs.store(0);
    }

    LOG(Info, "AAP: Disconnected");
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::Disconnect, true);
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

    // Optimistic local echo: the CCCD (0x1C) does not work, so the firmware
    // state can only be read back. Follow the write with a read request — when
    // the ATT path is confirmed the response refreshes the cached state; when
    // it is not, this frame is simply ignored by the firmware.
    if (GetAttPathState() == AttPathState::Confirmed) {
        SendPacket(Att::BuildReadRequest(Att::kLoudSoundReductionHandle));
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
// ATT path probe (blueprint 0.3)
//

AttPathState Manager::GetAttPathState() const
{
    auto state = _attPathState.load();
    if (state == AttPathState::Pending) {
        // Lazily resolve the probe outcome once its 500ms window has passed;
        // no probe thread, no timers — whoever asks gets the final answer.
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
        if (nowMs >= _attProbeDeadlineMs.load()) {
            _attPathState.store(AttPathState::Unconfirmed);
            Diagnostics::Recorder::Instance().Record(
                Diagnostics::Step::AttProbe, false, "no ATT answer within 500ms");
            state = AttPathState::Unconfirmed;
        }
    }
    return state;
}

void Manager::StartAttProbe(
    const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken)
{
    // One-shot probe: ask for the loud-sound-reduction value. Any ATT answer
    // (even an error response) proves the firmware accepts ATT on this pipe;
    // no answer within the window marks the path unconfirmed. Never retried.
    auto packet = Att::BuildReadRequest(Att::kLoudSoundReductionHandle);
    if (!SendPacket(transport, packet, stopToken)) {
        _attPathState.store(AttPathState::Unconfirmed);
        Diagnostics::Recorder::Instance().Record(Diagnostics::Step::AttProbe, false, "send failed");
        return;
    }

    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
    _attProbeDeadlineMs.store(nowMs + 500);
    _attPathState.store(AttPathState::Pending);
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::AttProbe, true, "probe sent");
    LOG(Info, "AAP: Sent ATT probe (read handle 0x{:02X}), waiting up to 500ms",
        Att::kLoudSoundReductionHandle);
}

void Manager::ProcessAttFrame(
    const std::vector<uint8_t> &packet, AAPAttempt attempt, const Callbacks &callbacks)
{
    if (!IsCurrentAttempt(attempt)) {
        return;
    }

    // Read response (0x0B): we only ever issue one kind of read — handle 0x1B —
    // so the value belongs to loud sound reduction.
    if (Att::IsReadResponse(packet)) {
        auto previous = _attPathState.exchange(AttPathState::Confirmed);
        if (previous != AttPathState::Confirmed && callbacks.onAttProbeResult) {
            callbacks.onAttProbeResult(AttPathState::Confirmed, attempt);
        }
        Diagnostics::Recorder::Instance().Record(Diagnostics::Step::AttProbe, true, "confirmed");
        LOG(Info, "AAP: ATT path confirmed (read response, {} value bytes)", packet.size() - 1);

        auto value = Att::ParseReadResponse(packet);
        if (value.has_value() && !value->empty()) {
            std::optional<LoudSoundReductionState> state;
            if ((*value)[0] == 0x01) {
                state = LoudSoundReductionState::Enabled;
            }
            else if ((*value)[0] == 0x00) {
                state = LoudSoundReductionState::Disabled;
            }
            if (state.has_value()) {
                {
                    std::lock_guard<std::mutex> lock{_mutex};
                    _loudSoundReductionState = state;
                }
                if (callbacks.onLoudSoundReductionChanged) {
                    callbacks.onLoudSoundReductionChanged(state.value(), attempt);
                }
            }
        }
        return;
    }

    // Error response (0x01): the ATT stack answered, so the path itself is
    // alive even though the value is unavailable.
    if (Att::IsErrorResponse(packet)) {
        auto previous = _attPathState.exchange(AttPathState::Confirmed);
        if (previous != AttPathState::Confirmed && callbacks.onAttProbeResult) {
            callbacks.onAttProbeResult(AttPathState::Confirmed, attempt);
        }
        LOG(Info, "AAP: ATT path confirmed (error response, opcode 0x{:02X}, code 0x{:02X})",
            packet.size() > 1 ? packet[1] : 0, packet.size() > 4 ? packet[4] : 0);
        return;
    }

    // Handle Value Notification (0x1B) for the loud-sound-reduction handle.
    if (Att::IsNotification(packet)) {
        auto notification = Att::ParseNotification(packet);
        if (notification.has_value() && notification->first == Att::kLoudSoundReductionHandle &&
            !notification->second.empty())
        {
            std::optional<LoudSoundReductionState> state;
            if (notification->second[0] == 0x01) {
                state = LoudSoundReductionState::Enabled;
            }
            else if (notification->second[0] == 0x00) {
                state = LoudSoundReductionState::Disabled;
            }
            if (state.has_value()) {
                {
                    std::lock_guard<std::mutex> lock{_mutex};
                    _loudSoundReductionState = state;
                }
                LOG(Info, "AAP: Loud sound reduction (ATT notify): {}",
                    static_cast<int>(state.value()));
                if (callbacks.onLoudSoundReductionChanged) {
                    callbacks.onLoudSoundReductionChanged(state.value(), attempt);
                }
            }
        }
        return;
    }
}

bool Manager::SendPacket(const std::vector<uint8_t> &packet)
{
    if (!_connected) {
        return false;
    }

    return SendPacket(_transport.load(), packet, {});
}

bool Manager::SendPacket(
    const std::shared_ptr<AAPTransport> &transport, const std::vector<uint8_t> &packet,
    std::stop_token stopToken)
{
    if (!transport || stopToken.stop_requested() || _connectStop.get_token().stop_requested()) {
        return false;
    }
    if (!transport->Send(packet, stopToken)) {
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

    // Loud sound reduction notification
    if (IsLoudSoundReductionNotification(packet)) {
        auto state = ParseLoudSoundReductionState(packet);
        if (state.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _loudSoundReductionState = state;
            }
            LOG(Info, "AAP: Loud sound reduction: {}", static_cast<int>(state.value()));
            if (callbacks.onLoudSoundReductionChanged) {
                callbacks.onLoudSoundReductionChanged(state.value(), attempt);
            }
        }
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
        active = _attempt;
        callback = _callbacks.onDisconnected;
    }

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

bool Manager::InitializeConnection(
    const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken)
{
    if (stopToken.stop_requested() || _connectStop.get_token().stop_requested()) {
        return false;
    }

    // The connection gate remains closed until the handshake completes. Use the pinned
    // session transport directly; SendPacket would reject these pre-connected packets.
    if (!SendPacket(transport, Packets::Handshake, stopToken)) {
        LOG(Error, "AAP: Failed to send handshake");
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::Handshake, false, "send failed");
        return false;
    }
    LOG(Info, "AAP: Sent handshake");
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::Handshake, true);

    if (!WaitCancellable(kPacketProcessingDelay, stopToken)) {
        return false;
    }

    // Enable features (Conversational Awareness, Adaptive Transparency)
    if (!SendPacket(transport, Packets::EnableFeatures, stopToken)) {
        LOG(Warn, "AAP: Failed to send enable features packet");
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::FeatureFlags, false, "send failed");
    }
    else {
        LOG(Info, "AAP: Sent enable features");
        Diagnostics::Recorder::Instance().Record(Diagnostics::Step::FeatureFlags, true);
    }

    if (!WaitCancellable(kPacketProcessingDelay, stopToken)) {
        return false;
    }

    // Request notifications (battery, ear detection, noise control, etc.)
    if (!SendPacket(transport, Packets::RequestNotifications, stopToken)) {
        LOG(Error, "AAP: Failed to send request notifications");
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::NotificationSubscribe, false, "send failed");
        return false;
    }
    LOG(Info, "AAP: Sent request notifications");
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::NotificationSubscribe, true);

    // Blueprint 0.3: probe whether the firmware answers ATT frames on this
    // pipe. Fire-and-forget; the outcome lands in _attPathState and is
    // surfaced through onAttProbeResult.
    StartAttProbe(transport, stopToken);

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

Manager::Manager() {}
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
void Manager::StartAttProbe(const std::shared_ptr<AAPTransport> &, std::stop_token) {}
bool Manager::IsCurrentAttempt(AAPAttempt) const
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
void Manager::ProcessPacket(const std::vector<uint8_t> &) {}
void Manager::ProcessPacket(const std::vector<uint8_t> &, AAPAttempt, const Callbacks &) {}
void Manager::ReaderLoop(AAPAttempt, std::shared_ptr<AAPTransport>, std::stop_token, Callbacks) {}
bool Manager::WaitCancellable(std::chrono::milliseconds, std::stop_token)
{
    return false;
}
bool Manager::InitializeConnection(const std::shared_ptr<AAPTransport> &, std::stop_token)
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
