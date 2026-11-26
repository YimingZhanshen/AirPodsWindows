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

// MagicAAP WinRT Client - Uses MagicAAP driver device interfaces, with the
// Windows.Devices.Bluetooth.Rfcomm API kept for the legacy service path.
// This requires the MagicAAP driver to be installed.

#pragma once

#include "MagicAAPDeviceBinding.h"

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <stop_token>
#include <thread>
#include <memory>

class MagicAAPTests;

namespace Core::MagicAAPWinRT {

// AAP Service UUID: 74ec2172-0bad-4d01-8f77-997b2be0722a
constexpr wchar_t AAP_SERVICE_UUID[] = L"{74EC2172-0BAD-4D01-8F77-997B2BE0722A}";

class MagicAAPWinRTClient
{
public:
    MagicAAPWinRTClient();
    ~MagicAAPWinRTClient();

    // Non-copyable
    MagicAAPWinRTClient(const MagicAAPWinRTClient &) = delete;
    MagicAAPWinRTClient &operator=(const MagicAAPWinRTClient &) = delete;

    // Check if MagicAAP driver is installed
    static bool IsDriverInstalled();

    // Check if MagicAAP driver is running
    static bool IsDriverRunning();

    // Find devices with AAP service (requires MagicAAP driver)
    static std::vector<std::pair<std::wstring, uint64_t>> EnumerateAAPDevices();

    // Every device interface the driver publishes, together with the identity
    // Windows can prove for it. Selection and opening live in BindToTargetInterface.
    static std::vector<MagicAAPDeviceInterface> EnumerateInterfaces();

    // Connect to device by Bluetooth address
    bool Connect(uint64_t bluetoothAddress);

    // Connect to device by device ID
    bool ConnectById(const std::wstring &deviceId);

    // Opens only the device interface that proves `bluetoothAddress`. An
    // interface whose identity cannot be proven is reported, never opened, and a
    // failed open on the matching device is not retried against another one.
    MagicAAPBindStatus
    ConnectViaDeviceInterface(uint64_t bluetoothAddress, std::stop_token stop = {});

    // Disconnect
    void Disconnect();

    // Check connection status
    bool IsConnected() const
    {
        return _connected.load();
    }

    // Send one complete record atomically. A partial device-interface write fails;
    // the unwritten suffix is never retried as a second record.
    bool SendData(const std::vector<uint8_t> &data);

    // Callbacks. In device-interface mode each data callback is exactly one
    // successful ReadFile completion; P1 performs no stream reassembly.
    //
    // The receiver thread is handed an immutable snapshot of both callbacks plus
    // the session that started it, so these members are only ever read or written
    // under `_mutex` and a retired session can never signal a newer one.
    using OnDataReceivedCallback = std::function<void(const std::vector<uint8_t> &)>;
    using OnDisconnectedCallback = std::function<void()>;

    void SetOnDataReceived(OnDataReceivedCallback callback);
    void SetOnDisconnected(OnDisconnectedCallback callback);

    // Last failure, already redacted: counts and masked addresses only.
    std::wstring GetLastError() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _lastError;
    }

private:
    friend class ::MagicAAPTests;

    // Immutable state for exactly one receiver thread.
    struct ReceiverSnapshot {
        uint64_t session{0};
        void *deviceHandle{nullptr};
        void *impl{nullptr};
        bool usingDeviceInterface{false};
        OnDataReceivedCallback onDataReceived;
        OnDisconnectedCallback onDisconnected;
    };

    std::atomic<bool> _connected{false};
    std::atomic<bool> _stopReceiver{false};
    std::atomic<uint64_t> _connectionGeneration{0};
    std::thread _receiverThread;
    // Serializes potentially blocking device-interface startups. Committed
    // session state and callbacks use _mutex, which is never held while a driver
    // enumeration/open is in progress.
    std::mutex _connectMutex;
    mutable std::mutex _mutex;
    std::wstring _lastError;
    MagicAAPCallbackSession _sessions;

    OnDataReceivedCallback _onDataReceived;
    OnDisconnectedCallback _onDisconnected;

    // Opaque pointer to implementation (to avoid WinRT headers in header)
    void *_impl{nullptr};

    // Device handle for direct device interface connection
    void *_deviceHandle{nullptr};
    bool _usingDeviceInterface{false};

    // Copies the callbacks and publishes a new session. `_mutex` must be held.
    ReceiverSnapshot StartSession();

    // Owns the complete receiver-start transaction. Any exception retires the
    // session and releases every partially initialized transport resource.
    bool StartReceiverSession();
    void RollbackConnection();
    void ReceiverLoop(ReceiverSnapshot snapshot);
    void SetError(const std::wstring &error);
    void CleanupWinRTObjects();
    void CleanupDeviceHandle();

    // Hardware-free seams. Production uses WinRT/SetupAPI/CreateFile and a real
    // receiver thread; tests can substitute enumeration, opening, and thread start.
    using InterfaceEnumerator = std::function<std::vector<MagicAAPDeviceInterface>()>;
    using ReceiverThreadStarter = std::function<std::thread(const ReceiverSnapshot &)>;
    InterfaceEnumerator _interfaceEnumerator;
    std::function<MagicAAPDeviceHandle(const MagicAAPDeviceInterface &)> _interfaceOpener;
    std::function<bool()> _driverInstalledProbe;
    std::function<bool()> _driverRunningProbe;
    ReceiverThreadStarter _receiverThreadStarter;
};

} // namespace Core::MagicAAPWinRT
