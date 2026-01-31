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

// MagicAAP WinRT Client - Uses Windows.Devices.Bluetooth.Rfcomm API
// This requires MagicAAP driver to be installed

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>

namespace Core::MagicAAPWinRT {

// AAP Service UUID: 74ec2172-0bad-4d01-8f77-997b2be0722a
constexpr wchar_t AAP_SERVICE_UUID[] = L"{74EC2172-0BAD-4D01-8F77-997B2BE0722A}";

class MagicAAPWinRTClient {
public:
    MagicAAPWinRTClient();
    ~MagicAAPWinRTClient();

    // Non-copyable
    MagicAAPWinRTClient(const MagicAAPWinRTClient&) = delete;
    MagicAAPWinRTClient& operator=(const MagicAAPWinRTClient&) = delete;

    // Check if MagicAAP driver is installed
    static bool IsDriverInstalled();
    
    // Check if MagicAAP driver is running
    static bool IsDriverRunning();

    // Find devices with AAP service (requires MagicAAP driver)
    static std::vector<std::pair<std::wstring, uint64_t>> EnumerateAAPDevices();

    // Connect to device by Bluetooth address
    bool Connect(uint64_t bluetoothAddress);
    
    // Connect to device by device ID
    bool ConnectById(const std::wstring& deviceId);
    
    // Connect via MagicAAP device interface (direct file I/O)
    bool ConnectViaDeviceInterface(uint64_t bluetoothAddress);
    
    // Enumerate MagicAAP device interfaces
    static std::vector<std::wstring> EnumerateMagicAAPDevices();

    // Disconnect
    void Disconnect();

    // Check connection status
    bool IsConnected() const { return _connected.load(); }

    // Send raw data
    bool SendData(const std::vector<uint8_t>& data);

    // Callbacks
    using OnDataReceivedCallback = std::function<void(const std::vector<uint8_t>&)>;
    using OnDisconnectedCallback = std::function<void()>;
    
    void SetOnDataReceived(OnDataReceivedCallback callback);
    void SetOnDisconnected(OnDisconnectedCallback callback);

    // Get last error message
    std::wstring GetLastError() const { return _lastError; }

private:
    std::atomic<bool> _connected{false};
    std::atomic<bool> _stopReceiver{false};
    std::thread _receiverThread;
    mutable std::mutex _mutex;
    std::wstring _lastError;
    
    OnDataReceivedCallback _onDataReceived;
    OnDisconnectedCallback _onDisconnected;
    
    // Opaque pointer to implementation (to avoid WinRT headers in header)
    void* _impl{nullptr};
    
    // Device handle for direct device interface connection
    void* _deviceHandle{nullptr};
    bool _usingDeviceInterface{false};
    
    void ReceiverLoop();
    void SetError(const std::wstring& error);
    void CleanupWinRTObjects();
    void CleanupDeviceHandle();
};

} // namespace Core::MagicAAPWinRT
