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

Manager::Manager()
{
    // The broker owns serialization and desired state; this layer only answers it with the wire
    // encoding, the connection gate, and the protocol evidence that arrives on the reader.
    _commandBroker = std::make_unique<AAPCommandBroker>(
        [this](const AAPCommand &command, uint64_t generation, std::stop_token stopToken) {
            return SendCommand(command, generation, stopToken);
        },
        [this](const AAPCommandOutcome &outcome) { HandleCommandSettled(outcome); });
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
        _sessionStop.request_stop();
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
        _sessionStop.request_stop();
        _sessionStop = std::stop_source{};
        _connectStop = std::stop_source{};
        callbacks = _callbacks;
        // A new session starts with no battery data; the fusion falls back to BLE until
        // the device sends its first 0x0004 notification on this generation.
        _batterySnapshot.reset();

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

    {
        std::lock_guard lock{_mutex};
        if (cancelled()) {
            _connecting = false;
            _lastConnectFailure.store(ConnectFailure::TransportError);
        }
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

    const bool initialized = InitializeConnection(transport, attempt, stopToken);
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

    // Publish the transport gate before opening the broker, but start the reader only afterwards.
    // A submission in the intervening window is retained as desired/TransportClosed rather than
    // sent against a closed gate and misreported as Rejected.
    {
        std::lock_guard lock{_mutex};
        if (cancelled()) {
            _connecting = false;
            _lastConnectFailure.store(ConnectFailure::TransportError);
            transport->Close();
            return false;
        }
        _disconnecting = false;
        _connected = true;
        _connecting = false;
        _lastConnectFailure.store(ConnectFailure::None);
    }
    _commandBroker->BeginSession(attempt.generation);
    bool cancelledAfterOpen = false;
    std::stop_source readerStop;
    {
        std::lock_guard lock{_mutex};
        if (cancelled()) {
            _connecting = false;
            _lastConnectFailure.store(ConnectFailure::TransportError);
            cancelledAfterOpen = true;
        }
        else {
            _readerStop = std::move(readerStop);
            _readerThread =
                std::jthread{[attempt, transport, callbacks, token = _readerStop.get_token(),
                              this] { ReaderLoop(attempt, transport, token, callbacks); }};
        }
    }
    if (cancelledAfterOpen) {
        _commandBroker->EndSession();
        std::lock_guard lock{_mutex};
        _disconnecting = true;
        _connected = false;
        transport->Close();
        return false;
    }

    // The session is live: everything the user asked for while disconnected goes out in a fixed
    // order. BeginSession also accepts any evidence buffered before the reader started. D3 is
    // replayed only when Adaptive is reported or desired; otherwise it stays desired silently.
    const bool replayAdaptive = GetNoiseControlMode() == NoiseControlMode::Adaptive ||
                                _commandBroker->Desired(AAPCommandKind::SetNoiseControl) ==
                                    static_cast<uint32_t>(NoiseControlMode::Adaptive);
    _commandBroker->ReplayDesiredState(attempt.generation, [replayAdaptive](AAPCommandKind kind) {
        return kind != AAPCommandKind::SetAdaptiveNoiseLevel || replayAdaptive;
    });

    // A settings change can race the handshake replay. The revision check closes that window:
    // changes made while the gate was closed are sent immediately after it opens. This is also the
    // single bounded recovery attempt when the handshake replay failed; a new session always starts
    // with a fresh generation check, irrespective of an earlier session's success.
    {
        std::lock_guard lock{_stemConfigMutex};
        if (!_stemConfigState.Replay(attempt.generation, [&](uint8_t value) {
                return SendPacket(attempt, Packets::BuildStemConfigPacket(value), stopToken);
            }))
        {
            LOG(Warn, "AAP: Failed to send pending stem config after connection");
        }
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
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver not installed");
        return nullptr;
    }
    if (!MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning()) {
        LOG(Warn, "AAP: MagicAAP driver installed but not running");
        failure = ConnectFailure::DriverMissing;
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver not running");
        return nullptr;
    }
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::DriverOpen, true);

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
    magicAAP->Close();
    failure = ConnectFailure::TransportError;
    return nullptr;
}

void Manager::Disconnect()
{
    CancelConnect();
    std::lock_guard connectLock{_connectMutex};
    // Close the broker before _connected/_disconnecting make the loss observable. A submission in
    // this window is retained as desired/TransportClosed, and active flights settle without
    // rollback.
    _commandBroker->EndSession();
    Callbacks::FnOnDisconnectedT callback;
    AAPAttempt attempt;
    std::shared_ptr<AAPTransport> transport;
    std::jthread reader;

    {
        std::lock_guard lock{_mutex};
        _sessionStop.request_stop();
        _readerStop.request_stop();
        _disconnecting = true;
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
        ReleaseAttRead();
        _noiseControlMode.reset();
        _conversationalAwarenessState.reset();
        _personalizedVolumeState.reset();
        _automaticEarDetectionState.reset();
        _loudSoundReductionState.reset();
        _batterySnapshot.reset();
        _ppeCapLevel.reset();
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
    return _connected && !_disconnecting;
}

AAPAttempt Manager::GetLastAttempt() const
{
    std::lock_guard lock{_mutex};
    return _attempt;
}

std::optional<AAPBatterySnapshot> Manager::GetBatterySnapshot() const
{
    std::lock_guard lock{_mutex};
    return _batterySnapshot;
}

bool Manager::IsCurrentAttempt(AAPAttempt attempt) const
{
    std::lock_guard lock{_mutex};
    return attempt.generation != 0 && _connected && _attempt.address == attempt.address &&
           _attempt.generation == attempt.generation;
}

bool Manager::SetNoiseControlMode(NoiseControlMode mode)
{
    return SubmitCommand(
               {AAPCommandKind::SetNoiseControl, static_cast<uint32_t>(Helper::ToUnderlying(mode))})
        .accepted;
}

bool Manager::SetNoiseControlMode(
    NoiseControlMode mode, AAPAttempt attempt, std::stop_token stopToken)
{
    if (stopToken.stop_requested() || _cancelledGeneration.load() == attempt.generation) {
        return false;
    }
    return SendPacket(attempt, Packets::BuildNoiseControlPacket(mode), stopToken);
}

std::optional<NoiseControlMode> Manager::GetNoiseControlMode() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _noiseControlMode;
}

bool Manager::SetConversationalAwareness(bool enable)
{
    return SubmitCommand({AAPCommandKind::SetConversationalAwareness,
                          static_cast<uint32_t>(enable ? 0x01 : 0x02)})
        .accepted;
}

std::optional<ConversationalAwarenessState> Manager::GetConversationalAwarenessState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _conversationalAwarenessState;
}

bool Manager::SetPersonalizedVolume(bool enable)
{
    return SubmitCommand(
               {AAPCommandKind::SetPersonalizedVolume, static_cast<uint32_t>(enable ? 0x01 : 0x02)})
        .accepted;
}

std::optional<PersonalizedVolumeState> Manager::GetPersonalizedVolumeState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _personalizedVolumeState;
}

bool Manager::SetAutomaticEarDetection(bool enable)
{
    return SubmitCommand({AAPCommandKind::SetAutomaticEarDetection,
                          static_cast<uint32_t>(enable ? 0x01 : 0x02)})
        .accepted;
}

std::optional<bool> Manager::GetAutomaticEarDetectionState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _automaticEarDetectionState;
}

bool Manager::SetLoudSoundReduction(bool enable)
{
    return SubmitCommand(
               {AAPCommandKind::SetLoudSoundReduction, static_cast<uint32_t>(enable ? 0x01 : 0x00)})
        .accepted;
}

std::optional<LoudSoundReductionState> Manager::GetLoudSoundReductionState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _loudSoundReductionState;
}

bool Manager::SetPpeCapLevel(uint8_t level)
{
    return SubmitCommand({AAPCommandKind::SetPpeCapLevel, NormalizePpeCapLevel(level)}).accepted;
}

std::optional<uint8_t> Manager::GetPpeCapLevel() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _ppeCapLevel;
}

bool Manager::SetAdaptiveNoiseLevel(uint8_t level)
{
    return SubmitCommand({AAPCommandKind::SetAdaptiveNoiseLevel, FilteringStrengthToWire(level)})
        .accepted;
}

bool Manager::SetStemConfig(uint8_t mask)
{
    std::lock_guard lock{_stemConfigMutex};
    _stemConfigState.SetMask(mask);
    if (!_connected) {
        LOG(Info, "AAP: Saved stem config mask 0x{:02X} for the next connection",
            _stemConfigState.Mask());
        return true;
    }

    const auto attempt = GetLastAttempt();
    if (!_stemConfigState.Replay(attempt.generation, [&](uint8_t value) {
            return SendPacket(attempt, Packets::BuildStemConfigPacket(value), {});
        }))
    {
        LOG(Warn, "AAP: Failed to send stem config mask 0x{:02X}", _stemConfigState.Mask());
        return false;
    }
    LOG(Info, "AAP: Set stem config mask to 0x{:02X}", _stemConfigState.Mask());
    return true;
}

bool Manager::IsHeadTrackingActive() const
{
    return _headTrackingActive;
}

AAPCommandResult Manager::SubmitCommand(AAPCommand command, std::stop_token stopToken)
{
    // A disconnected submit is desired-state bookkeeping, never a late write on a retiring
    // session. RecordDesired does not publish outcomes, so callers may hold their intent lock.
    if (!_connected) {
        if (stopToken.stop_requested()) {
            return {false, false, AAPCommandFailure::Rejected};
        }
        CommandMode(command.kind, command.value);
        return _commandBroker->RecordDesired(command);
    }
    CommandMode(command.kind, command.value);
    if (command.kind == AAPCommandKind::SetAdaptiveNoiseLevel && IsConnected()) {
        std::optional<NoiseControlMode> reportedMode;
        {
            std::lock_guard lock{_mutex};
            reportedMode = _noiseControlMode;
        }
        if (_beforeAdaptiveEligibilityForTest) {
            _beforeAdaptiveEligibilityForTest();
        }
        // Mode notifications take this same lock before touching desired/queued state. Keeping
        // the eligibility decision and desired recording in one critical section prevents an
        // Adaptive notification from observing the old desired/cache pair and queueing nothing.
        // Re-read after the test rendezvous: the notification may have changed the mode since the
        // first snapshot. Lock order is always Manager -> Broker; the broker never calls back while
        // holding its mutex, and its wire worker takes the manager lock after releasing it.
        std::lock_guard lock{_mutex};
        if (!_connected) {
            return _commandBroker->Submit(command, stopToken);
        }
        reportedMode = _noiseControlMode;
        const bool pendingAdaptive =
            !reportedMode.has_value() && _commandBroker->Desired(AAPCommandKind::SetNoiseControl) ==
                                             static_cast<uint32_t>(NoiseControlMode::Adaptive);
        if (reportedMode != NoiseControlMode::Adaptive && !pendingAdaptive) {
            // Settings may change while disconnected/Off/unknown. Remember the desired filtering
            // strength without queueing a command that can only produce a false rejection.
            return _commandBroker->RecordDesired(command);
        }
        return _commandBroker->Submit(command, stopToken);
    }
    return _commandBroker->Submit(command, stopToken);
}

std::optional<uint32_t> Manager::DesiredCommand(AAPCommandKind kind) const
{
    return _commandBroker->Desired(kind);
}

std::optional<uint32_t> Manager::ConfirmedCommand(AAPCommandKind kind) const
{
    return _commandBroker->Confirmed(kind);
}

uint64_t Manager::LatestCommandRequest(AAPCommandKind kind) const
{
    return _commandBroker->LatestRequest(kind);
}

bool Manager::IsCurrentCommandRequest(AAPCommandKind kind, uint64_t requestId) const
{
    return _commandBroker->IsCurrentRequest(kind, requestId);
}

bool Manager::HasPendingCommand(AAPCommandKind kind) const
{
    return _commandBroker->HasPendingCommand(kind);
}

void Manager::RefreshCommand(AAPCommandKind kind)
{
    std::shared_ptr<AAPTransport> transport;
    uint64_t generation{0};
    {
        std::lock_guard lock{_mutex};
        if (!_connected || _disconnecting) {
            return;
        }
        transport = _transport.load();
        generation = _attempt.generation;
    }
    if (kind == AAPCommandKind::SetLoudSoundReduction) {
        if (AcquireAttRead(generation, 0, {})) {
            if (!SendPacket(transport, Att::BuildReadRequest(Att::kLoudSoundReductionHandle), {})) {
                ReleaseAttRead();
            }
            // The response releases the owner and is treated as baseline evidence.
        }
        return;
    }
    // Re-subscribing requests current settings notifications without inventing a desired value.
    SendPacket(transport, Packets::RequestNotifications, {});
}

AAPConfirmMode Manager::CommandMode(AAPCommandKind kind, uint32_t value) const
{
    // ATT-on-AAP is unverified on Windows. Without a proven read path, silence cannot distinguish
    // an applied LSR write from an ignored one, so do not manufacture a timeout/rollback.
    if (kind == AAPCommandKind::SetLoudSoundReduction &&
        GetAttPathState() != AttPathState::Confirmed)
    {
        _commandBroker->SetConfirmMode(kind, AAPConfirmMode::Unverifiable);
    }
    return _commandBroker->Mode(kind, value);
}

bool Manager::WaitForCommandsIdle(std::chrono::milliseconds timeout)
{
    return _commandBroker->WaitForIdle(timeout);
}

bool Manager::SendCommand(const AAPCommand &command, uint64_t generation, std::stop_token stopToken)
{
    AAPAttempt attempt;
    {
        std::lock_guard lock{_mutex};
        if (!_connected || _disconnecting || _attempt.generation != generation ||
            stopToken.stop_requested())
        {
            return false;
        }
        attempt = _attempt;
    }

    std::vector<uint8_t> packet;
    switch (command.kind) {
    case AAPCommandKind::SetNoiseControl:
        if (command.value < 0x01 || command.value > 0x04) {
            return false;
        }
        packet = Packets::BuildNoiseControlPacket(static_cast<NoiseControlMode>(command.value));
        break;
    case AAPCommandKind::SetConversationalAwareness:
        if (command.value < 0x01 || command.value > 0x02) {
            return false;
        }
        packet = Packets::BuildConversationalAwarenessPacket(command.value == 0x01);
        break;
    case AAPCommandKind::SetPersonalizedVolume:
        if (command.value < 0x01 || command.value > 0x02) {
            return false;
        }
        packet = Packets::BuildPersonalizedVolumePacket(command.value == 0x01);
        break;
    case AAPCommandKind::SetLoudSoundReduction:
        if (command.value > 0x01) {
            return false;
        }
        packet = Packets::BuildLoudSoundReductionPacket(command.value == 0x01);
        break;
    case AAPCommandKind::SetPpeCapLevel:
        packet =
            Packets::BuildPpeCapLevelPacket(static_cast<uint8_t>(std::min(command.value, 100U)));
        break;
    case AAPCommandKind::SetAdaptiveNoiseLevel:
        packet = Packets::BuildAdaptiveNoisePacket(
            FilteringStrengthToWire(static_cast<uint8_t>(std::min(command.value, 100U))));
        break;
    case AAPCommandKind::SetAutomaticEarDetection:
        if (command.value < 0x01 || command.value > 0x02) {
            return false;
        }
        packet = Packets::BuildAutomaticEarDetectionPacket(command.value == 0x01);
        break;
    case AAPCommandKind::SetHeadTracking:
        if (command.value > 0x01) {
            return false;
        }
        packet = command.value ? Packets::StartHeadTracking : Packets::StopHeadTracking;
        break;
    default:
        return false;
    }

    const bool adaptive = command.kind == AAPCommandKind::SetAdaptiveNoiseLevel;
    if (!SendPacketForAttempt(
            attempt, packet, stopToken,
            adaptive ? std::optional{NoiseControlMode::Adaptive} : std::nullopt, adaptive))
    {
        return false;
    }
    if (command.kind == AAPCommandKind::SetLoudSoundReduction) {
        // The write has no response. A read in the same serial slot supplies the readback; failure
        // to issue that read is not proof that the already-written value was rejected. The startup
        // probe and this read share one pending-ATT owner so responses cannot confirm each other.
        if (AcquireAttRead(generation, command.requestId, stopToken)) {
            if (!SendPacketForAttempt(
                    attempt, Att::BuildReadRequest(Att::kLoudSoundReductionHandle), stopToken))
            {
                ReleaseAttRead();
            }
        }
    }
    // A successful write only starts the confirmation window. IsHeadTrackingActive changes in
    // HandleCommandSettled, never optimistically here.
    return true;
}

void Manager::RouteCommandEcho(AAPCommand command, AAPAttempt attempt)
{
    if (IsCurrentAttempt(attempt)) {
        _commandBroker->OnEcho(command, attempt.generation);
    }
}

void Manager::HandleCommandSettled(const AAPCommandOutcome &outcome)
{
    const bool showValue = outcome.settlement == AAPCommandSettlement::Confirmed ||
                           outcome.settlement == AAPCommandSettlement::AppliedUnconfirmed ||
                           outcome.rollBackDesired;
    const auto value = outcome.rollBackDesired ? outcome.rollbackValue
                                               : std::optional<uint32_t>{outcome.command.value};
    if (!showValue || !value.has_value()) {
        Callbacks callbacks;
        AAPAttempt attempt;
        {
            std::lock_guard lock{_mutex};
            if (!_connected || outcome.generation != _attempt.generation) {
                return;
            }
            callbacks = _callbacks;
            attempt = {_attempt.address, outcome.generation};
            if (outcome.command.kind == AAPCommandKind::SetHeadTracking &&
                (_connected && _attempt.generation == outcome.generation) &&
                (outcome.settlement == AAPCommandSettlement::TimedOut ||
                 outcome.settlement == AAPCommandSettlement::Rejected))
            {
                _headTrackingActive.store(false);
            }
        }
        if (callbacks.onCommandSettled) {
            callbacks.onCommandSettled(outcome, attempt);
        }
        return;
    }

    AAPAttempt attempt{0, outcome.generation};
    Callbacks callbacks;
    std::optional<NoiseControlMode> noise;
    std::optional<ConversationalAwarenessState> conversation;
    std::optional<PersonalizedVolumeState> personalized;
    std::optional<LoudSoundReductionState> loudSound;
    std::optional<uint8_t> ppe;
    std::optional<uint8_t> adaptive;
    std::optional<bool> earDetection;
    {
        std::lock_guard lock{_mutex};
        if (!_connected || outcome.generation != _attempt.generation) {
            return;
        }
        attempt.address = _attempt.address;
        callbacks = _callbacks;
        switch (outcome.command.kind) {
        case AAPCommandKind::SetNoiseControl:
            if (*value >= 0x01 && *value <= 0x04) {
                _noiseControlMode = static_cast<NoiseControlMode>(*value);
                noise = _noiseControlMode;
            }
            break;
        case AAPCommandKind::SetConversationalAwareness:
            if (*value >= 0x01 && *value <= 0x02) {
                _conversationalAwarenessState = *value == 0x01
                                                    ? ConversationalAwarenessState::Enabled
                                                    : ConversationalAwarenessState::Disabled;
                conversation = _conversationalAwarenessState;
            }
            break;
        case AAPCommandKind::SetPersonalizedVolume:
            if (*value >= 0x01 && *value <= 0x02) {
                _personalizedVolumeState = *value == 0x01 ? PersonalizedVolumeState::Enabled
                                                          : PersonalizedVolumeState::Disabled;
                personalized = _personalizedVolumeState;
            }
            break;
        case AAPCommandKind::SetLoudSoundReduction:
            _loudSoundReductionState = *value == 0x01 ? LoudSoundReductionState::Enabled
                                                      : LoudSoundReductionState::Disabled;
            loudSound = _loudSoundReductionState;
            break;
        case AAPCommandKind::SetPpeCapLevel:
            _ppeCapLevel = static_cast<uint8_t>(std::min(*value, 100U));
            ppe = _ppeCapLevel;
            break;
        case AAPCommandKind::SetAdaptiveNoiseLevel:
            _adaptiveNoiseLevel =
                FilteringStrengthFromWire(static_cast<uint8_t>(std::min(*value, 100U)));
            adaptive = _adaptiveNoiseLevel;
            break;
        case AAPCommandKind::SetAutomaticEarDetection:
            if (*value >= 0x01 && *value <= 0x02) {
                _automaticEarDetectionState = *value == 0x01;
                earDetection = _automaticEarDetectionState;
            }
            break;
        case AAPCommandKind::SetHeadTracking:
            if (_connected && _attempt.generation == outcome.generation) {
                _headTrackingActive.store(*value != 0);
            }
            break;
        default:
            break;
        }
    }

    switch (outcome.command.kind) {
    case AAPCommandKind::SetNoiseControl:
        if (callbacks.onNoiseControlChanged && noise) {
            callbacks.onNoiseControlChanged(*noise, attempt);
        }
        break;
    case AAPCommandKind::SetConversationalAwareness:
        if (callbacks.onConversationalAwarenessChanged && conversation) {
            callbacks.onConversationalAwarenessChanged(*conversation, attempt);
        }
        break;
    case AAPCommandKind::SetPersonalizedVolume:
        if (callbacks.onPersonalizedVolumeChanged && personalized) {
            callbacks.onPersonalizedVolumeChanged(*personalized, attempt);
        }
        break;
    case AAPCommandKind::SetLoudSoundReduction:
        if (callbacks.onLoudSoundReductionChanged && loudSound) {
            callbacks.onLoudSoundReductionChanged(*loudSound, attempt);
        }
        break;
    case AAPCommandKind::SetPpeCapLevel:
        if (callbacks.onPpeCapLevelChanged && ppe) {
            callbacks.onPpeCapLevelChanged(*ppe, attempt);
        }
        break;
    case AAPCommandKind::SetAdaptiveNoiseLevel:
        if (callbacks.onAdaptiveNoiseLevelChanged && adaptive) {
            callbacks.onAdaptiveNoiseLevelChanged(*adaptive, attempt);
        }
        break;
    case AAPCommandKind::SetAutomaticEarDetection:
        if (callbacks.onAutomaticEarDetectionChanged && earDetection) {
            callbacks.onAutomaticEarDetectionChanged(*earDetection, attempt);
        }
        break;
    default:
        break;
    }
    if (callbacks.onCommandSettled) {
        callbacks.onCommandSettled(outcome, attempt);
    }
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
    std::unique_lock<std::mutex> lock{_attReadMutex};
    auto state = _attPathState.load();
    if (state == AttPathState::Pending) {
        // Lazily resolve the probe outcome once its 500ms window has passed.
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
        if (nowMs >= _attProbeDeadlineMs.load()) {
            _attPathState.store(AttPathState::Unconfirmed);
            if (_attReadOwner && _attReadOwner->probe) {
                _attReadOwner.reset();
                _attReadCondition.notify_all();
            }
            Diagnostics::Recorder::Instance().Record(
                Diagnostics::Step::AttProbe, false, "no ATT answer within 500ms");
            state = AttPathState::Unconfirmed;
        }
    }
    return state;
}

bool Manager::AcquireAttRead(uint64_t generation, uint64_t requestId, std::stop_token stopToken)
{
    std::unique_lock<std::mutex> lock{_attReadMutex};
    while (_attReadOwner && !stopToken.stop_requested()) {
        if (_attReadOwner->generation != generation || _disconnecting) {
            return false;
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (_attReadOwner->probe || std::chrono::steady_clock::now() >= _attReadOwner->expiresAt) {
            if (_attReadOwner->probe && nowMs < _attProbeDeadlineMs.load()) {
                _attReadCondition.wait_for(lock, std::chrono::milliseconds{10});
                continue;
            }
            if (!_attReadOwner->probe) {
                _commandBroker->SetConfirmMode(
                    AAPCommandKind::SetLoudSoundReduction, AAPConfirmMode::Unverifiable);
                _attReadOwner.reset();
                _attReadCondition.notify_all();
                return false;
            }
            _attPathState.store(AttPathState::Unconfirmed);
            _attReadOwner.reset();
            _attReadCondition.notify_all();
            break;
        }
        _attReadCondition.wait_for(lock, std::chrono::milliseconds{10});
    }
    if (stopToken.stop_requested() || _disconnecting || _attReadOwner) {
        return false;
    }
    _attReadOwner = AttReadOwner{
        generation, requestId, false, std::chrono::steady_clock::now() + std::chrono::seconds{2}};
    return true;
}

void Manager::ReleaseAttRead()
{
    std::lock_guard lock{_attReadMutex};
    _attReadOwner.reset();
    _attReadCondition.notify_all();
}

void Manager::StartAttProbe(
    const std::shared_ptr<AAPTransport> &transport, std::stop_token stopToken)
{
    // One-shot probe: ask for the loud-sound-reduction value. Any ATT answer
    // (even an error response) proves the firmware accepts ATT on this pipe;
    // no answer within the window marks the path unconfirmed. Never retried.
    auto packet = Att::BuildReadRequest(Att::kLoudSoundReductionHandle);
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    _attProbeDeadlineMs.store(nowMs + 500);
    {
        std::lock_guard lock{_attReadMutex};
        _attReadOwner = AttReadOwner{_attempt.generation, 0, true};
    }
    if (!SendPacket(transport, packet, stopToken)) {
        ReleaseAttRead();
        _attPathState.store(AttPathState::Unconfirmed);
        Diagnostics::Recorder::Instance().Record(Diagnostics::Step::AttProbe, false, "send failed");
        return;
    }

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
        AttReadOwner owner;
        {
            std::lock_guard lock{_attReadMutex};
            owner = _attReadOwner.value_or(AttReadOwner{});
            _attReadOwner.reset();
            _attReadCondition.notify_all();
        }
        auto previous = _attPathState.exchange(AttPathState::Confirmed);
        _commandBroker->SetConfirmMode(
            AAPCommandKind::SetLoudSoundReduction, AAPConfirmMode::Readback);
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
                const AAPCommand command{
                    AAPCommandKind::SetLoudSoundReduction,
                    static_cast<uint32_t>(Helper::ToUnderlying(*state))};
                if (owner.probe || owner.requestId == 0 || owner.generation != attempt.generation) {
                    _commandBroker->OnObserved(command, attempt.generation);
                }
                else {
                    _commandBroker->OnEcho(command, attempt.generation, owner.requestId);
                }
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
        ReleaseAttRead();
        auto previous = _attPathState.exchange(AttPathState::Confirmed);
        _commandBroker->SetConfirmMode(
            AAPCommandKind::SetLoudSoundReduction, AAPConfirmMode::Unverifiable);
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
                RouteCommandEcho(
                    {AAPCommandKind::SetLoudSoundReduction,
                     static_cast<uint32_t>(Helper::ToUnderlying(*state))},
                    attempt);
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
    AAPAttempt attempt, const std::vector<uint8_t> &packet, std::stop_token stopToken)
{
    std::shared_ptr<AAPTransport> transport;
    {
        std::lock_guard lock{_mutex};
        if (attempt.generation == 0 || !_connected || _attempt.address != attempt.address ||
            _attempt.generation != attempt.generation ||
            _cancelledGeneration.load() == attempt.generation || stopToken.stop_requested())
        {
            return false;
        }
        // Pin the validated session's pipe. A later reconnect may replace _transport, but this
        // dispatch can only reach the transport that passed the identity gate above.
        transport = _transport.load();
    }

    // Cancellation may have arrived while the snapshot was being taken. This is the latest check
    // before the transport write; a write already handed to the OS cannot be recalled.
    if (stopToken.stop_requested() || _cancelledGeneration.load() == attempt.generation) {
        return false;
    }
    return SendPacket(transport, packet, stopToken);
}

bool Manager::SendPacketForAttempt(
    const AAPAttempt &attempt, const std::vector<uint8_t> &packet, std::stop_token stopToken,
    std::optional<NoiseControlMode> requiredMode, bool allowPendingAdaptive)
{
    const bool pendingAdaptive = allowPendingAdaptive &&
                                 requiredMode == NoiseControlMode::Adaptive &&
                                 _commandBroker->Desired(AAPCommandKind::SetNoiseControl) ==
                                     static_cast<uint32_t>(NoiseControlMode::Adaptive);
    const auto modeMatches = [&] {
        return !requiredMode || _noiseControlMode == *requiredMode ||
               (pendingAdaptive && !_noiseControlMode.has_value());
    };

    std::shared_ptr<AAPTransport> transport;
    std::stop_source attemptStop;
    {
        std::lock_guard lock{_mutex};
        if (!_connected || attempt.generation == 0 || _attempt.address != attempt.address ||
            _attempt.generation != attempt.generation || !modeMatches())
        {
            return false;
        }
        transport = _transport.load();
        attemptStop = _sessionStop;
        if (!transport) {
            return false;
        }
    }

    // The transport and cancellation source are pinned together. Retiring the attempt stops this
    // send even if a replacement transport is installed before a blocked driver write returns.
    std::stop_source sendStop;
    std::stop_callback attemptCallback{
        attemptStop.get_token(), [&sendStop] { sendStop.request_stop(); }};
    std::stop_callback callerCallback{stopToken, [&sendStop] { sendStop.request_stop(); }};
    if (!SendPacket(transport, packet, sendStop.get_token())) {
        return false;
    }

    std::lock_guard lock{_mutex};
    return _connected && _attempt.address == attempt.address &&
           _attempt.generation == attempt.generation && modeMatches();
}

bool Manager::SendPacket(
    const std::shared_ptr<AAPTransport> &transport, const std::vector<uint8_t> &packet,
    std::stop_token stopToken)
{
    if (!transport || stopToken.stop_requested() || _connectStop.get_token().stop_requested()) {
        return false;
    }
    std::lock_guard sendLock{_sendMutex};
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
                if (*mode != NoiseControlMode::Unknown) {
                    _commandBroker->OnEcho(
                        {AAPCommandKind::SetNoiseControl,
                         static_cast<uint32_t>(Helper::ToUnderlying(*mode))},
                        attempt.generation);
                }
                if (*mode == NoiseControlMode::Adaptive) {
                    if (const auto desired =
                            _commandBroker->Desired(AAPCommandKind::SetAdaptiveNoiseLevel);
                        desired && _adaptiveNoiseLevel != desired)
                    {
                        _commandBroker->Submit({AAPCommandKind::SetAdaptiveNoiseLevel, *desired});
                    }
                }
            }
            LOG(Info, "AAP: Noise control mode changed to {}",
                Helper::ToString(mode.value()).toStdString());
            if (callbacks.onNoiseControlChanged) {
                callbacks.onNoiseControlChanged(mode.value(), attempt);
            }
        }
        return;
    }

    // Optional adaptive filtering-strength echo (0x2E). It is distinct from PPE cap 0x38;
    // silence is not success, so a missing echo remains a broker timeout/rollback.
    if (IsAdaptiveNoiseLevelNotification(packet)) {
        auto level = ParseAdaptiveNoiseLevelNotification(packet);
        if (level.has_value()) {
            {
                std::lock_guard lock{_mutex};
                if (!_connected || _attempt.address != attempt.address ||
                    _attempt.generation != attempt.generation ||
                    _noiseControlMode != NoiseControlMode::Adaptive)
                {
                    return;
                }
                _adaptiveNoiseLevel = level;
            }
            RouteCommandEcho({AAPCommandKind::SetAdaptiveNoiseLevel, *level}, attempt);
            LOG(Info, "AAP: Adaptive filtering strength: {}", level.value());
            if (callbacks.onAdaptiveNoiseLevelChanged) {
                callbacks.onAdaptiveNoiseLevelChanged(*level, attempt);
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
            if (*state != ConversationalAwarenessState::Unknown) {
                RouteCommandEcho(
                    {AAPCommandKind::SetConversationalAwareness,
                     static_cast<uint32_t>(Helper::ToUnderlying(*state))},
                    attempt);
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
            if (*state != PersonalizedVolumeState::Unknown) {
                RouteCommandEcho(
                    {AAPCommandKind::SetPersonalizedVolume,
                     static_cast<uint32_t>(Helper::ToUnderlying(*state))},
                    attempt);
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
            RouteCommandEcho(
                {AAPCommandKind::SetAutomaticEarDetection,
                 static_cast<uint32_t>(state ? 0x01 : 0x02)},
                attempt);
            LOG(Info, "AAP: Automatic ear detection: {}", state.value() ? "enabled" : "disabled");
            if (callbacks.onAutomaticEarDetectionChanged) {
                callbacks.onAutomaticEarDetectionChanged(state.value(), attempt);
            }
        }
        return;
    }

    // Loud sound reduction uses ATT readback/notification, never control id 0x25.
    if (IsLoudSoundReductionNotification(packet)) {
        LOG(Trace, "AAP: Ignoring VolumeSwipe record 0x25 (not loud sound reduction)");
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
            RouteCommandEcho({AAPCommandKind::SetPpeCapLevel, *level}, attempt);
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

    // Battery notification (opcode 0x0004, 1% precision). Malformed records never
    // touch the cache or reach the fusion consumer.
    if (IsBatteryNotification(packet)) {
        auto snapshot = ParseBatteryNotification(packet);
        if (snapshot.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _batterySnapshot = snapshot;
            }
            LOG(Trace, "AAP: Battery notification ({} bytes)", packet.size());
            if (callbacks.onBatteryChanged) {
                callbacks.onBatteryChanged(snapshot.value(), attempt);
            }
        }
        return;
    }

    // Stem press notification (opcode 0x0019). A transient gesture: nothing is cached and no
    // state is echoed. ParseStemEvent only accepts the exact eight-byte record documented in
    // AAPStem.h, so a truncated or unknown-field frame reaches no consumer at all.
    if (auto stemEvent = ParseStemEvent(packet); stemEvent.has_value()) {
        LOG(Info, "AAP: Stem press - bud: {}, gesture: {}", StemSideName(stemEvent->side),
            StemPressName(stemEvent->press));
        if (callbacks.onStemPress) {
            callbacks.onStemPress(*stemEvent, attempt);
        }
        return;
    }

    // Only the shared 0x17 classifier can turn a record into head-tracking data. In particular,
    // a Start/Stop command with the same opcode and a malformed prefix never reach the parser.
    if (IsHeadTrackingStreamRecord(packet)) {
        auto trackingData = ParseHeadTrackingData(packet);
        if (trackingData.has_value()) {
            // A valid current-generation record is proof of a live stream even if it arrived just
            // after the broker's confirmation deadline.
            _headTrackingActive.store(true);
            RouteCommandEcho({AAPCommandKind::SetHeadTracking, 0x01}, attempt);
            if (callbacks.onHeadTrackingData) {
                callbacks.onHeadTrackingData(trackingData.value(), attempt);
            }
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
        _sessionStop.request_stop();
        const bool current =
            _attempt.address == attempt.address && _attempt.generation == attempt.generation;
        if (!current || !_connected) {
            return;
        }
        _batterySnapshot.reset();
        active = _attempt;
        callback = _callbacks.onDisconnected;
    }

    // Close the broker before publishing disconnection to senders.
    _commandBroker->EndSession();
    {
        std::lock_guard lock{_mutex};
        _disconnecting = true;
        _connected = false;
        _usingMagicAAP = false;
        _transport.store(std::shared_ptr<AAPTransport>{});
        ReleaseAttRead();
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
    const std::shared_ptr<AAPTransport> &transport, AAPAttempt attempt, std::stop_token stopToken)
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

    // Subscribe to raw stem gestures only after the handshake. Failure is non-fatal: the desired
    // mask remains pending for one bounded recovery after the gate opens and for the next session.
    {
        std::lock_guard stemConfigLock{_stemConfigMutex};
        const auto mask = _stemConfigState.Mask();
        if (!_stemConfigState.Replay(attempt.generation, [&](uint8_t value) {
                return SendPacket(transport, Packets::BuildStemConfigPacket(value), stopToken);
            }))
        {
            LOG(Warn, "AAP: Failed to replay stem config mask 0x{:02X}", mask);
        }
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

Manager::Manager()
{
    _commandBroker = std::make_unique<AAPCommandBroker>(
        [](const AAPCommand &, uint64_t, std::stop_token) { return false; });
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
std::optional<AAPBatterySnapshot> Manager::GetBatterySnapshot() const
{
    return std::nullopt;
}
bool Manager::SetNoiseControlMode(NoiseControlMode)
{
    return false;
}
bool Manager::SetNoiseControlMode(NoiseControlMode, AAPAttempt, std::stop_token)
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
bool Manager::SetStemConfig(uint8_t)
{
    return false;
}
bool Manager::IsHeadTrackingActive() const
{
    return false;
}
void Manager::SetCallbacks(Callbacks) {}
AAPCommandResult Manager::SubmitCommand(AAPCommand command, std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return {false, false, AAPCommandFailure::Rejected};
    }
    return _commandBroker->RecordDesired(command);
}
uint64_t Manager::LatestCommandRequest(AAPCommandKind kind) const
{
    return _commandBroker->LatestRequest(kind);
}
bool Manager::IsCurrentCommandRequest(AAPCommandKind kind, uint64_t requestId) const
{
    return _commandBroker->IsCurrentRequest(kind, requestId);
}
bool Manager::HasPendingCommand(AAPCommandKind kind) const
{
    return _commandBroker->HasPendingCommand(kind);
}
void Manager::RefreshCommand(AAPCommandKind) {}
std::optional<uint32_t> Manager::DesiredCommand(AAPCommandKind kind) const
{
    return _commandBroker->Desired(kind);
}
std::optional<uint32_t> Manager::ConfirmedCommand(AAPCommandKind kind) const
{
    return _commandBroker->Confirmed(kind);
}
AAPConfirmMode Manager::CommandMode(AAPCommandKind kind, uint32_t value) const
{
    return _commandBroker->Mode(kind, value);
}
bool Manager::WaitForCommandsIdle(std::chrono::milliseconds timeout)
{
    return _commandBroker->WaitForIdle(timeout);
}
bool Manager::IsMagicAAPDriverAvailable()
{
    return false;
}
AttPathState Manager::GetAttPathState() const
{
    return AttPathState::Unconfirmed;
}
void Manager::StartAttProbe(const std::shared_ptr<AAPTransport> &, std::stop_token) {}
bool Manager::AcquireAttRead(uint64_t, uint64_t, std::stop_token)
{
    return false;
}
void Manager::ReleaseAttRead() {}
bool Manager::IsCurrentAttempt(AAPAttempt) const
{
    return false;
}
void Manager::ProcessAttFrame(const std::vector<uint8_t> &, AAPAttempt, const Callbacks &) {}
bool Manager::SendPacket(const std::vector<uint8_t> &)
{
    return false;
}
bool Manager::SendPacket(AAPAttempt, const std::vector<uint8_t> &, std::stop_token)
{
    return false;
}
bool Manager::SendPacketForAttempt(
    const AAPAttempt &, const std::vector<uint8_t> &, std::stop_token,
    std::optional<NoiseControlMode>, bool)
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
bool Manager::InitializeConnection(
    const std::shared_ptr<AAPTransport> &, AAPAttempt, std::stop_token)
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
