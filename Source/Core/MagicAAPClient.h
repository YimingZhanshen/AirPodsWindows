// MagicAAP Driver Communication Interface
// This module provides communication with the MagicAAP kernel driver
// for L2CAP connection to AirPods AAP service

#pragma once

#include <Windows.h>
#include <SetupAPI.h>
#include <initguid.h>
#include <string>
#include <optional>
#include <vector>
#include <functional>

#pragma comment(lib, "SetupAPI.lib")

namespace Core::MagicAAP {

// MagicAAP Device Interface GUID (same as AAP Service UUID)
// {74EC2172-0BAD-4D01-8F77-997B2BE0722A}
DEFINE_GUID(GUID_DEVINTERFACE_MAGICAAP, 
    0x74ec2172, 0x0bad, 0x4d01, 0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0, 0x72, 0x2a);

// Secondary device interface (for device enumeration)
// {9EEC98BB-3C54-45D4-A843-7900C4635E08}
DEFINE_GUID(GUID_DEVINTERFACE_MAGICAAP_ENUM,
    0x9eec98bb, 0x3c54, 0x45d4, 0xa8, 0x43, 0x79, 0x00, 0xc4, 0x63, 0x5e, 0x08);

class MagicAAPClient {
public:
    MagicAAPClient() = default;
    ~MagicAAPClient();

    // Non-copyable
    MagicAAPClient(const MagicAAPClient&) = delete;
    MagicAAPClient& operator=(const MagicAAPClient&) = delete;

    // Movable
    MagicAAPClient(MagicAAPClient&& other) noexcept;
    MagicAAPClient& operator=(MagicAAPClient&& other) noexcept;

    // Check if MagicAAP driver is installed
    static bool IsDriverInstalled();

    // Find all MagicAAP devices (connected AirPods with AAP support)
    static std::vector<std::wstring> EnumerateDevices();

    // Connect to a specific device
    bool Connect(const std::wstring& devicePath);
    
    // Connect to the first available device
    bool ConnectFirst();

    // Disconnect from the device
    void Disconnect();

    // Check if connected
    bool IsConnected() const { return m_hDevice != INVALID_HANDLE_VALUE; }

    // Send data to the device (AAP protocol packet)
    bool SendData(const std::vector<uint8_t>& data);

    // Receive data from the device
    std::optional<std::vector<uint8_t>> ReceiveData(DWORD timeoutMs = 1000);

    // Set callback for received data
    using DataCallback = std::function<void(const std::vector<uint8_t>&)>;
    void SetDataCallback(DataCallback callback) { m_dataCallback = std::move(callback); }

    // Start async receive loop
    bool StartAsyncReceive();
    void StopAsyncReceive();

    // Get last error message
    std::wstring GetLastErrorMessage() const { return m_lastError; }

private:
    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
    std::wstring m_lastError;
    DataCallback m_dataCallback;
    bool m_asyncReceiveRunning = false;
    HANDLE m_hReceiveThread = nullptr;
    
    static std::wstring GetDevicePath(const GUID& interfaceGuid, DWORD index);
    void SetError(const std::wstring& msg);
};

} // namespace Core::MagicAAP
