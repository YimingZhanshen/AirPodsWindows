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

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

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

    // Create Bluetooth socket
    SOCKET sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP);
    if (sock == INVALID_SOCKET) {
        LOG(Error, "AAP: Failed to create Bluetooth socket: {}", WSAGetLastError());
        return false;
    }

    // Set up the address structure
    SOCKADDR_BTH addr{};
    addr.addressFamily = AF_BTH;
    addr.btAddr = deviceAddress;
    addr.port = kPSM;

    LOG(Info, "AAP: Connecting to device {:016X} on PSM {}", deviceAddress, kPSM);

    // Connect to the device
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        LOG(Error, "AAP: Failed to connect: {}", error);
        closesocket(sock);
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
    if (_socket == nullptr || !_connected) {
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
bool Manager::SendPacket(const std::vector<uint8_t>&) { return false; }
void Manager::ProcessPacket(const std::vector<uint8_t>&) {}
void Manager::ReaderLoop() {}
bool Manager::InitializeConnection() { return false; }

} // namespace Core::AAP

#endif
