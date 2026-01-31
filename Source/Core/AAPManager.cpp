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

#include "AAPManager.h"
#include "../Logger.h"

#if defined APD_OS_WIN

#include <WinSock2.h>
#include <ws2bth.h>
#include <BluetoothAPIs.h>
#include <initguid.h>
#include "MagicAAPWinRT.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

// AAP Service UUID: 74ec2172-0bad-4d01-8f77-997b2be0722a
// {74EC2172-0BAD-4D01-8F77-997B2BE0722A}
DEFINE_GUID(AAP_SERVICE_UUID, 
    0x74ec2172, 0x0bad, 0x4d01, 0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0, 0x72, 0x2a);

namespace Core::AAP {

Manager::Manager()
{
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        LOG(Error, "WSAStartup failed: {}", result);
    }
}

Manager::~Manager()
{
    Disconnect();
    WSACleanup();
}

bool Manager::Connect(uint64_t deviceAddress)
{
    // First disconnect if already connected (without holding lock to avoid deadlock)
    if (_connected) {
        LOG(Warn, "AAP: Already connected, disconnecting first");
        Disconnect();
    }

    std::lock_guard<std::mutex> lock{_mutex};

    SOCKET sock = INVALID_SOCKET;
    bool connected = false;

    // Method 1: Try L2CAP with SOCK_SEQPACKET (datagram-oriented, more native for L2CAP)
    LOG(Info, "AAP: Attempting L2CAP SEQPACKET connection to {:016X} on PSM {}", deviceAddress, kPSM);
    sock = socket(AF_BTH, SOCK_SEQPACKET, BTHPROTO_L2CAP);
    if (sock != INVALID_SOCKET) {
        SOCKADDR_BTH addr{};
        addr.addressFamily = AF_BTH;
        addr.btAddr = deviceAddress;
        addr.port = kPSM;

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
            connected = true;
            LOG(Info, "AAP: L2CAP SEQPACKET connection successful");
        } else {
            int error = WSAGetLastError();
            LOG(Warn, "AAP: L2CAP SEQPACKET failed: {}", error);
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    } else {
        LOG(Warn, "AAP: Failed to create L2CAP SEQPACKET socket: {}", WSAGetLastError());
    }

    // Method 2: Try L2CAP with SOCK_STREAM
    if (!connected) {
        LOG(Info, "AAP: Attempting L2CAP STREAM connection");
        sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP);
        if (sock != INVALID_SOCKET) {
            SOCKADDR_BTH addr{};
            addr.addressFamily = AF_BTH;
            addr.btAddr = deviceAddress;
            addr.port = kPSM;

            if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
                LOG(Info, "AAP: L2CAP STREAM connection successful");
            } else {
                int error = WSAGetLastError();
                LOG(Warn, "AAP: L2CAP STREAM failed: {}", error);
                closesocket(sock);
                sock = INVALID_SOCKET;
            }
        }
    }

    // Method 3: Try RFCOMM with service UUID
    if (!connected) {
        LOG(Info, "AAP: Attempting RFCOMM with AAP UUID");
        sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (sock != INVALID_SOCKET) {
            SOCKADDR_BTH addr{};
            addr.addressFamily = AF_BTH;
            addr.btAddr = deviceAddress;
            addr.serviceClassId = AAP_SERVICE_UUID;
            addr.port = BT_PORT_ANY;

            if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
                connected = true;
                LOG(Info, "AAP: RFCOMM connection successful");
            } else {
                int error = WSAGetLastError();
                LOG(Warn, "AAP: RFCOMM failed: {}", error);
                closesocket(sock);
                sock = INVALID_SOCKET;
            }
        }
    }

    if (!connected || sock == INVALID_SOCKET) {
        LOG(Info, "AAP: Traditional socket methods failed, trying MagicAAP WinRT...");
        
        // Method 4: Try MagicAAP WinRT (requires MagicAAP driver)
        if (ConnectViaMagicAAP(deviceAddress)) {
            // MagicAAP connection successful - don't need socket-based logic
            return true;
        }
        
        LOG(Error, "AAP: All connection methods failed");
        return false;
    }

    _socket = reinterpret_cast<void*>(sock);
    _connected = true;

    LOG(Info, "AAP: Connected successfully");

    // Initialize the connection (send handshake, enable features, request notifications)
    if (!InitializeConnection()) {
        LOG(Error, "AAP: Failed to initialize connection");
        // Clean up without calling Disconnect to avoid lock issues
        _connected = false;
        closesocket(sock);
        _socket = nullptr;
        return false;
    }

    // Start reader thread
    _stopReader = false;
    _readerThread = std::thread(&Manager::ReaderLoop, this);

    if (_callbacks.onConnected) {
        _callbacks.onConnected();
    }

    return true;
}

void Manager::Disconnect()
{
    std::lock_guard<std::mutex> lock{_mutex};

    if (!_connected) {
        return;
    }

    _stopReader = true;
    _connected = false;
    _headTrackingActive = false;

    // Disconnect MagicAAP client if used
    if (_usingMagicAAP && _magicAAPClient) {
        _magicAAPClient->Disconnect();
        _magicAAPClient.reset();
        _usingMagicAAP = false;
    }

    // Close traditional socket if used
    if (_socket != nullptr) {
        SOCKET sock = reinterpret_cast<SOCKET>(_socket);
        closesocket(sock);
        _socket = nullptr;
    }

    if (_readerThread.joinable()) {
        _readerThread.join();
    }

    _noiseControlMode.reset();
    _conversationalAwarenessState.reset();

    LOG(Info, "AAP: Disconnected");

    if (_callbacks.onDisconnected) {
        _callbacks.onDisconnected();
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

    if (level > 50) {
        level = 50;
    }

    auto packet = Packets::BuildAdaptiveTransparencyLevelPacket(level);
    if (!SendPacket(packet)) {
        return false;
    }

    LOG(Info, "AAP: Set adaptive transparency level to {}", level);
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

bool Manager::SendPacket(const std::vector<uint8_t>& packet)
{
    if (!_connected) {
        return false;
    }
    
    // Use MagicAAP client if available
    if (_usingMagicAAP && _magicAAPClient) {
        bool success = _magicAAPClient->SendData(packet);
        if (success) {
            LOG(Trace, "AAP: Sent {} bytes via MagicAAP", packet.size());
        }
        return success;
    }

    // Use traditional socket
    if (_socket == nullptr) {
        return false;
    }

    SOCKET sock = reinterpret_cast<SOCKET>(_socket);
    int sent = send(sock, reinterpret_cast<const char*>(packet.data()),
                    static_cast<int>(packet.size()), 0);

    if (sent == SOCKET_ERROR) {
        LOG(Error, "AAP: Failed to send packet: {}", WSAGetLastError());
        return false;
    }

    LOG(Trace, "AAP: Sent {} bytes", sent);
    return true;
}

void Manager::ProcessPacket(const std::vector<uint8_t>& packet)
{
    // Noise control notification
    if (IsNoiseControlNotification(packet)) {
        auto mode = ParseNoiseControlNotification(packet);
        if (mode.has_value()) {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _noiseControlMode = mode;
            }
            LOG(Info, "AAP: Noise control mode changed to {}", Helper::ToString(mode.value()).toStdString());
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
            LOG(Info, "AAP: Conversational awareness state: {}", Helper::ToString(state.value()).toStdString());
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

    // Head tracking data
    if (_headTrackingActive && packet.size() >= 56) {
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
    constexpr size_t bufferSize = 1024;
    std::vector<char> buffer(bufferSize);

    while (!_stopReader && _connected) {
        SOCKET sock = reinterpret_cast<SOCKET>(_socket);
        
        // Set socket to non-blocking mode for timeout handling
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
        
        if (selectResult == SOCKET_ERROR) {
            LOG(Error, "AAP: Select error: {}", WSAGetLastError());
            break;
        }

        if (selectResult == 0) {
            // Timeout, continue loop
            continue;
        }

        int received = recv(sock, buffer.data(), static_cast<int>(bufferSize), 0);
        
        if (received == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                LOG(Error, "AAP: Receive error: {}", error);
                break;
            }
            continue;
        }

        if (received == 0) {
            LOG(Info, "AAP: Connection closed by remote");
            break;
        }

        std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + received);
        ProcessPacket(packet);
    }

    if (!_stopReader) {
        // Connection was lost unexpectedly
        AAP::Callbacks::FnOnDisconnectedT callback;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            _connected = false;
            callback = _callbacks.onDisconnected;
        }
        // Invoke callback outside the lock to avoid potential deadlocks
        if (callback) {
            callback();
        }
    }
}

// Protocol timing constants
constexpr auto kPacketProcessingDelay = std::chrono::milliseconds(100);

bool Manager::InitializeConnection()
{
    // Send handshake
    if (!SendPacket(Packets::Handshake)) {
        LOG(Error, "AAP: Failed to send handshake");
        return false;
    }
    LOG(Info, "AAP: Sent handshake");

    // Small delay to allow handshake to be processed
    std::this_thread::sleep_for(kPacketProcessingDelay);

    // Enable features (Conversational Awareness, Adaptive Transparency)
    if (!SendPacket(Packets::EnableFeatures)) {
        LOG(Warn, "AAP: Failed to send enable features packet");
        // Continue anyway - some features may still work
    }
    LOG(Info, "AAP: Sent enable features");

    std::this_thread::sleep_for(kPacketProcessingDelay);

    // Request notifications (battery, ear detection, noise control, etc.)
    if (!SendPacket(Packets::RequestNotifications)) {
        LOG(Error, "AAP: Failed to send request notifications");
        return false;
    }
    LOG(Info, "AAP: Sent request notifications");

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
        return false;
    }
    
    if (!MagicAAPWinRT::MagicAAPWinRTClient::IsDriverRunning()) {
        LOG(Warn, "AAP: MagicAAP driver installed but not running");
        return false;
    }
    
    LOG(Info, "AAP: MagicAAP driver is available, attempting connection...");
    
    // Create MagicAAP client
    _magicAAPClient = std::make_unique<MagicAAPWinRT::MagicAAPWinRTClient>();
    
    // Set callbacks
    _magicAAPClient->SetOnDataReceived([this](const std::vector<uint8_t>& data) {
        ProcessPacket(data);
    });
    
    _magicAAPClient->SetOnDisconnected([this]() {
        LOG(Info, "AAP: MagicAAP connection lost");
        AAP::Callbacks::FnOnDisconnectedT callback;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            _connected = false;
            _usingMagicAAP = false;
            callback = _callbacks.onDisconnected;
        }
        if (callback) {
            callback();
        }
    });
    
    // First, try device interface connection (direct file I/O)
    LOG(Info, "AAP: Trying device interface connection...");
    if (_magicAAPClient->ConnectViaDeviceInterface(deviceAddress)) {
        LOG(Info, "AAP: Connected via MagicAAP device interface!");
        
        _connected = true;
        _usingMagicAAP = true;
        
        // Initialize the AAP protocol
        if (!InitializeConnection()) {
            LOG(Error, "AAP: Failed to initialize MagicAAP connection");
            _magicAAPClient->Disconnect();
            _magicAAPClient.reset();
            _connected = false;
            _usingMagicAAP = false;
            return false;
        }
        
        if (_callbacks.onConnected) {
            _callbacks.onConnected();
        }
        
        return true;
    }
    
    // Fallback: try WinRT RFCOMM connection
    LOG(Info, "AAP: Device interface failed, trying WinRT RFCOMM...");
    if (!_magicAAPClient->Connect(deviceAddress)) {
        std::wstring errorW = _magicAAPClient->GetLastError();
        std::string error(errorW.begin(), errorW.end());
        LOG(Warn, "AAP: MagicAAP WinRT connection failed: {}", error);
        _magicAAPClient.reset();
        return false;
    }
    
    LOG(Info, "AAP: Connected via MagicAAP WinRT!");
    
    _connected = true;
    _usingMagicAAP = true;
    
    // Initialize the AAP protocol
    if (!InitializeConnection()) {
        LOG(Error, "AAP: Failed to initialize MagicAAP connection");
        _magicAAPClient->Disconnect();
        _magicAAPClient.reset();
        _connected = false;
        _usingMagicAAP = false;
        return false;
    }
    
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
Manager::~Manager() { Disconnect(); }
bool Manager::Connect(uint64_t) { return false; }
void Manager::Disconnect() {}
bool Manager::IsConnected() const { return false; }
bool Manager::SetNoiseControlMode(NoiseControlMode) { return false; }
std::optional<NoiseControlMode> Manager::GetNoiseControlMode() const { return std::nullopt; }
bool Manager::SetConversationalAwareness(bool) { return false; }
std::optional<ConversationalAwarenessState> Manager::GetConversationalAwarenessState() const { return std::nullopt; }
bool Manager::SetAdaptiveNoiseLevel(uint8_t) { return false; }
bool Manager::StartHeadTracking() { return false; }
bool Manager::StopHeadTracking() { return false; }
bool Manager::IsHeadTrackingActive() const { return false; }
void Manager::SetCallbacks(Callbacks) {}
bool Manager::IsMagicAAPDriverAvailable() { return false; }
bool Manager::SendPacket(const std::vector<uint8_t>&) { return false; }
void Manager::ProcessPacket(const std::vector<uint8_t>&) {}
void Manager::ReaderLoop() {}
bool Manager::InitializeConnection() { return false; }
bool Manager::ConnectViaMagicAAP(uint64_t) { return false; }

} // namespace Core::AAP

#endif
