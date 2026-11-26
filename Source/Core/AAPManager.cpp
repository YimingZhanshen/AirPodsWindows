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

#include <chrono>

#if defined APD_OS_WIN

    #include "MagicAAPWinRT.h"

namespace Core::AAP {

Manager::Manager() = default;

Manager::~Manager()
{
    Disconnect();
}

bool Manager::Connect(uint64_t deviceAddress)
{
    // First disconnect if already connected (without holding lock to avoid deadlock)
    if (_connected) {
        LOG(Warn, "AAP: Already connected, disconnecting first");
        Disconnect();
    }

    std::lock_guard<std::mutex> lock{_mutex};

    _lastConnectFailure.store(ConnectFailure::None);
    // A fresh attempt starts a fresh step log (blueprint 1.4).
    Diagnostics::Recorder::Instance().Reset();
    Diagnostics::Recorder::Instance().Record(
        Diagnostics::Step::TransportConnect, true, "connect attempt started");

    // P1 supports exactly two message transports. SEQPACKET is attempted first;
    // the MagicAAP device interface is the only driver fallback.
    LOG(Info, "AAP: Attempting L2CAP SEQPACKET connection to {:016X} on PSM {}", deviceAddress,
        kPSM);
    auto transport = CreateSeqPacketTransport();

    if (transport->Connect(deviceAddress, {})) {
        _transport.store(transport);
        _usingMagicAAP = false;
        _connected = true;
    }
    else {
        _lastConnectFailure.store(ConnectFailure::ServiceUnavailable);
        if (ConnectViaMagicAAP(deviceAddress)) {
            return true;
        }
        LOG(Error, "AAP: All supported transport methods failed");
        return false;
    }

    LOG(Info, "AAP: Connected successfully");

    // Initialize the connection (send handshake, enable features, request notifications)
    if (!InitializeConnection()) {
        LOG(Error, "AAP: Failed to initialize connection");
        // Clean up without calling Disconnect to avoid lock issues
        _lastConnectFailure.store(ConnectFailure::HandshakeFailure);
        _connected = false;
        if (auto failedTransport = _transport.exchange(nullptr)) {
            failedTransport->Close();
        }
        return false;
    }

    // Start reader thread
    _readerStop = std::stop_source{};
    _readerExited = false;
    _readerThread = std::thread(&Manager::ReaderLoop, this);

    _lastConnectFailure.store(ConnectFailure::None);

    if (_callbacks.onConnected) {
        _callbacks.onConnected();
    }

    return true;
}

void Manager::Disconnect()
{
    // Move resources that may block into local variables while holding the lock,
    // then perform blocking operations outside the lock to avoid deadlocks.
    AAP::Callbacks::FnOnDisconnectedT callback;
    std::shared_ptr<AAPTransport> transport;
    std::thread localReaderThread;

    {
        std::lock_guard<std::mutex> lock{_mutex};

        if (!_connected) {
            return;
        }

        // Wake Receive before closing the transport it is using.
        _readerStop.request_stop();
        _connected = false;
        _headTrackingActive = false;

        transport = _transport.exchange(nullptr);
        _usingMagicAAP = false;

        // Move reader thread out so we can join/detach outside the lock
        if (_readerThread.joinable()) {
            localReaderThread = std::move(_readerThread);
        }

        // Copy callback to invoke later outside lock
        callback = _callbacks.onDisconnected;
    }

    // Perform potentially blocking operations without holding the mutex
    if (transport) {
        transport->Close();
    }

    // Wait for reader thread to exit up to 5 seconds; if it doesn't exit, detach it.
    if (localReaderThread.joinable()) {
        auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(5);
        while (!_readerExited && (std::chrono::steady_clock::now() - start) < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (_readerExited) {
            try {
                localReaderThread.join();
            }
            catch (...) {
            }
        }
        else {
            try {
                localReaderThread.detach();
            }
            catch (...) {
            }
        }
    }

    // Clear cached states under lock
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _noiseControlMode.reset();
        _conversationalAwarenessState.reset();
        _loudSoundReductionState.reset();
        _attPathState.store(AttPathState::Unknown);
        _attProbeDeadlineMs.store(0);
    }

    LOG(Info, "AAP: Disconnected");
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::Disconnect, true);

    // Invoke callback outside lock
    if (callback) {
        callback();
    }
}

bool Manager::IsConnected() const
{
    return _connected;
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

bool Manager::SetAdaptiveTransparencyLevel(uint8_t level)
{
    if (!_connected) {
        LOG(Warn, "AAP: Cannot set adaptive transparency level - not connected");
        return false;
    }

    // Control command 0x38 is PPE (hearing-protection) cap level, range 0-100 per
    // Docs/AAP-Protocol-Spec.md §7. The name and the UI label ("adaptive transparency") are
    // corrected in the terminology sweep; the wire format here is already per spec.
    if (level > 100) {
        level = 100;
    }

    auto packet = Packets::BuildPpeCapLevelPacket(level);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set PPE cap level (UI: adaptive transparency) to {}", level);
    return true;
}

std::optional<uint8_t> Manager::GetAdaptiveTransparencyLevel() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _adaptiveTransparencyLevel;
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

void Manager::StartAttProbe()
{
    // One-shot probe: ask for the loud-sound-reduction value. Any ATT answer
    // (even an error response) proves the firmware accepts ATT on this pipe;
    // no answer within the window marks the path unconfirmed. Never retried.
    auto packet = Att::BuildReadRequest(Att::kLoudSoundReductionHandle);
    if (!SendPacket(packet)) {
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

void Manager::ProcessAttFrame(const std::vector<uint8_t> &packet)
{
    // Read response (0x0B): we only ever issue one kind of read — handle 0x1B —
    // so the value belongs to loud sound reduction.
    if (Att::IsReadResponse(packet)) {
        auto previous = _attPathState.exchange(AttPathState::Confirmed);
        if (previous != AttPathState::Confirmed && _callbacks.onAttProbeResult) {
            _callbacks.onAttProbeResult(AttPathState::Confirmed);
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
                if (_callbacks.onLoudSoundReductionChanged) {
                    _callbacks.onLoudSoundReductionChanged(state.value());
                }
            }
        }
        return;
    }

    // Error response (0x01): the ATT stack answered, so the path itself is
    // alive even though the value is unavailable.
    if (Att::IsErrorResponse(packet)) {
        auto previous = _attPathState.exchange(AttPathState::Confirmed);
        if (previous != AttPathState::Confirmed && _callbacks.onAttProbeResult) {
            _callbacks.onAttProbeResult(AttPathState::Confirmed);
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
                if (_callbacks.onLoudSoundReductionChanged) {
                    _callbacks.onLoudSoundReductionChanged(state.value());
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

    auto transport = _transport.load();
    if (!transport || !transport->Send(packet, {})) {
        return false;
    }

    LOG(Trace, "AAP: Sent {} bytes via {}", packet.size(),
        transport->Kind() == AAPTransportKind::MagicAAP ? "MagicAAP" : "SEQPACKET");
    return true;
}

void Manager::ProcessPacket(const std::vector<uint8_t> &packet)
{
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
        ProcessAttFrame(packet);
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
            if (_callbacks.onNoiseControlChanged) {
                _callbacks.onNoiseControlChanged(mode.value());
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
            if (_callbacks.onConversationalAwarenessChanged) {
                _callbacks.onConversationalAwarenessChanged(state.value());
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
            if (_callbacks.onPersonalizedVolumeChanged) {
                _callbacks.onPersonalizedVolumeChanged(state.value());
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
            if (_callbacks.onAutomaticEarDetectionChanged) {
                _callbacks.onAutomaticEarDetectionChanged(state.value());
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
            if (_callbacks.onLoudSoundReductionChanged) {
                _callbacks.onLoudSoundReductionChanged(state.value());
            }
        }
        return;
    }

    // Adaptive transparency level notification
    if (IsAdaptiveTransparencyLevelNotification(packet)) {
        auto level = ParseAdaptiveTransparencyLevel(packet);
        if (level.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _adaptiveTransparencyLevel = level;
            }
            LOG(Info, "AAP: Adaptive transparency level: {}", level.value());
            if (_callbacks.onAdaptiveTransparencyLevelChanged) {
                _callbacks.onAdaptiveTransparencyLevelChanged(level.value());
            }
        }
        return;
    }

    // Speaking level notification (conversational awareness active)
    if (IsSpeakingLevelNotification(packet)) {
        auto level = ParseSpeakingLevel(packet);
        if (level.has_value() && _callbacks.onSpeakingLevelChanged) {
            _callbacks.onSpeakingLevelChanged(level.value());
        }
        return;
    }

    // Ear detection notification
    if (IsEarDetectionNotification(packet)) {
        auto earStatus = ParseEarDetection(packet);
        if (earStatus.has_value() && _callbacks.onEarDetectionChanged) {
            _callbacks.onEarDetectionChanged(earStatus->first, earStatus->second);
        }
        return;
    }

    // Only opcode 0x0017 carries head-tracking data. Unknown AAP records are
    // valid for diagnostics but must never fall through into this parser.
    if (_headTrackingActive && packet.size() >= 56 && packet[0] == 0x04 && packet[1] == 0x00 &&
        packet[2] == 0x04 && packet[3] == 0x00 && packet[4] == 0x17 && packet[5] == 0x00)
    {
        auto trackingData = ParseHeadTrackingData(packet);
        if (trackingData.has_value() && _callbacks.onHeadTrackingData) {
            _callbacks.onHeadTrackingData(trackingData.value());
        }
        return;
    }

    // Log unknown packets for debugging
    LOG(Trace, "AAP: Received unknown packet ({} bytes)", packet.size());
}

void Manager::ReaderLoop()
{
    auto transport = _transport.load();
    const auto stop = _readerStop.get_token();

    while (transport && !stop.stop_requested() && _connected) {
        auto packet = transport->Receive(stop);
        if (!packet.has_value()) {
            if (!stop.stop_requested()) {
                LOG(Info, "AAP: Transport receive ended");
            }
            break;
        }
        ProcessPacket(*packet);
    }

    if (!stop.stop_requested() && _connected) {
        // Connection was lost unexpectedly
        AAP::Callbacks::FnOnDisconnectedT callback;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            _connected = false;
            _usingMagicAAP = false;
            callback = _callbacks.onDisconnected;
        }
        // Invoke callback outside the lock to avoid potential deadlocks
        if (callback) {
            callback();
        }
    }

    // Mark reader exited so Disconnect can join safely
    _readerExited = true;
}

// Protocol timing constants
constexpr auto kPacketProcessingDelay = std::chrono::milliseconds(100);

bool Manager::InitializeConnection()
{
    // Send handshake
    if (!SendPacket(Packets::Handshake)) {
        LOG(Error, "AAP: Failed to send handshake");
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::Handshake, false, "send failed");
        return false;
    }
    LOG(Info, "AAP: Sent handshake");
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::Handshake, true);

    // Small delay to allow handshake to be processed
    std::this_thread::sleep_for(kPacketProcessingDelay);

    // Enable features (Conversational Awareness, Adaptive Transparency)
    if (!SendPacket(Packets::EnableFeatures)) {
        LOG(Warn, "AAP: Failed to send enable features packet");
        // Continue anyway - some features may still work
    }
    LOG(Info, "AAP: Sent enable features");
    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::FeatureFlags, true);

    std::this_thread::sleep_for(kPacketProcessingDelay);

    // Request notifications (battery, ear detection, noise control, etc.)
    if (!SendPacket(Packets::RequestNotifications)) {
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
    StartAttProbe();

    return true;
}

bool Manager::IsMagicAAPDriverAvailable()
{
    return MagicAAPWinRT::MagicAAPWinRTClient::IsDriverInstalled() &&
           MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning();
}

bool Manager::ConnectViaMagicAAP(uint64_t deviceAddress)
{
    // Check if MagicAAP driver is available
    if (!MagicAAPWinRT::MagicAAPWinRTClient::IsDriverInstalled()) {
        LOG(Info, "AAP: MagicAAP driver not installed");
        _lastConnectFailure.store(ConnectFailure::DriverMissing);
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver not installed");
        return false;
    }

    if (!MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning()) {
        LOG(Warn, "AAP: MagicAAP driver installed but not running");
        _lastConnectFailure.store(ConnectFailure::DriverMissing);
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::DriverOpen, false, "driver not running");
        return false;
    }

    Diagnostics::Recorder::Instance().Record(Diagnostics::Step::DriverOpen, true);

    LOG(Info, "AAP: MagicAAP driver is available, attempting connection...");

    // Only the MagicAAP device interface is supported. The existing WinRT
    // RFCOMM methods remain in the client for other callers but are not a fallback.
    auto transport = CreateMagicAAPTransport();
    LOG(Info, "AAP: Trying MagicAAP device interface connection...");
    if (!transport->Connect(deviceAddress, {})) {
        _lastConnectFailure.store(ConnectFailure::TransportError);
        return false;
    }

    _transport.store(transport);
    _connected = true;
    _usingMagicAAP = true;
    LOG(Info, "AAP: Connected via MagicAAP device interface!");

    if (!InitializeConnection()) {
        LOG(Error, "AAP: Failed to initialize MagicAAP connection");
        _lastConnectFailure.store(ConnectFailure::HandshakeFailure);
        _connected = false;
        _usingMagicAAP = false;
        if (auto failedTransport = _transport.exchange(nullptr)) {
            failedTransport->Close();
        }
        return false;
    }

    _readerStop = std::stop_source{};
    _readerExited = false;
    _readerThread = std::thread(&Manager::ReaderLoop, this);

    _lastConnectFailure.store(ConnectFailure::None);
    if (_callbacks.onConnected) {
        _callbacks.onConnected();
    }
    return true;
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
bool Manager::Connect(uint64_t)
{
    return false;
}
void Manager::Disconnect() {}
bool Manager::IsConnected() const
{
    return false;
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
void Manager::StartAttProbe() {}
void Manager::ProcessAttFrame(const std::vector<uint8_t> &) {}
bool Manager::SendPacket(const std::vector<uint8_t> &)
{
    return false;
}
void Manager::ProcessPacket(const std::vector<uint8_t> &) {}
void Manager::ReaderLoop() {}
bool Manager::InitializeConnection()
{
    return false;
}
bool Manager::ConnectViaMagicAAP(uint64_t)
{
    return false;
}

} // namespace Core::AAP

#endif
