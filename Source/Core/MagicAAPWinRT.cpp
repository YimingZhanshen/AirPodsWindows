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

#include "MagicAAPWinRT.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <initguid.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <winsvc.h>
#include <locale>
#include <codecvt>
#include <algorithm>
#include <cwctype>

#include <spdlog/spdlog.h>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Cfgmgr32.lib")

// Check for C++/WinRT support - require all necessary headers
#if __has_include(<winrt/base.h>) && __has_include(<winrt/Windows.Devices.Bluetooth.h>)
#define HAS_WINRT_BLUETOOTH 1

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "windowsapp.lib")

// Use global namespace aliases to avoid conflicts
namespace WinRTFoundation = ::winrt::Windows::Foundation;
namespace WinRTBluetooth = ::winrt::Windows::Devices::Bluetooth;
namespace WinRTRfcomm = ::winrt::Windows::Devices::Bluetooth::Rfcomm;
namespace WinRTEnum = ::winrt::Windows::Devices::Enumeration;
namespace WinRTSockets = ::winrt::Windows::Networking::Sockets;
namespace WinRTStreams = ::winrt::Windows::Storage::Streams;

#else
#define HAS_WINRT_BLUETOOTH 0
#endif

// AAP Service GUID
static constexpr GUID GUID_AAP_SERVICE = 
    { 0x74ec2172, 0x0bad, 0x4d01, { 0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0, 0x72, 0x2a } };

// MagicAAP Device Interface GUID (from driver analysis)
// The driver registers TWO device interfaces:
// 1. {74EC2172-0BAD-4D01-8F77-997B2BE0722A} - AAP Service UUID (same as Bluetooth service)
// 2. {9EEC98BB-3C54-45D4-A843-7900C4635E08} - Custom MagicAAP interface
DEFINE_GUID(GUID_DEVINTERFACE_MAGICAAP,
    0x9EEC98BB, 0x3C54, 0x45D4, 0xA8, 0x43, 0x79, 0x00, 0xC4, 0x63, 0x5E, 0x08);

// Alternative: Use AAP Service UUID as device interface
DEFINE_GUID(GUID_DEVINTERFACE_AAP_SERVICE,
    0x74ec2172, 0x0bad, 0x4d01, 0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0, 0x72, 0x2a);

namespace Core::MagicAAPWinRT {

#if HAS_WINRT_BLUETOOTH
// Internal state (to avoid exposing WinRT types in header)
struct MagicAAPWinRTClientImpl {
    WinRTSockets::StreamSocket socket{nullptr};
    WinRTStreams::DataReader reader{nullptr};
    WinRTStreams::DataWriter writer{nullptr};
    WinRTRfcomm::RfcommDeviceService service{nullptr};
};
#else
struct MagicAAPWinRTClientImpl {};
#endif

MagicAAPWinRTClient::MagicAAPWinRTClient() {
#if HAS_WINRT_BLUETOOTH
    // Initialize WinRT apartment
    ::winrt::init_apartment();
#endif
}

MagicAAPWinRTClient::~MagicAAPWinRTClient() {
    Disconnect();
}

bool MagicAAPWinRTClient::IsDriverInstalled() {
    // Check if MagicAAP service exists
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) {
        return false;
    }
    
    SC_HANDLE service = OpenServiceW(scm, L"MagicAAP", SERVICE_QUERY_STATUS);
    bool exists = (service != nullptr);
    
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(scm);
    
    return exists;
}

bool MagicAAPWinRTClient::IsDriverRunning() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) {
        return false;
    }
    
    SC_HANDLE service = OpenServiceW(scm, L"MagicAAP", SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }
    
    SERVICE_STATUS status{};
    bool running = QueryServiceStatus(service, &status) && 
                   (status.dwCurrentState == SERVICE_RUNNING);
    
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    
    return running;
}

std::vector<std::pair<std::wstring, uint64_t>> MagicAAPWinRTClient::EnumerateAAPDevices() {
    std::vector<std::pair<std::wstring, uint64_t>> devices;
    
#if HAS_WINRT_BLUETOOTH
    try {
        // Build selector for RFCOMM devices with AAP UUID
        auto serviceId = WinRTRfcomm::RfcommServiceId::FromUuid(::winrt::guid(GUID_AAP_SERVICE));
        auto selector = WinRTRfcomm::RfcommDeviceService::GetDeviceSelector(serviceId);
        
        std::string selectorStr = ::winrt::to_string(selector);
        spdlog::info("[MagicAAPWinRT] Device selector: {}", selectorStr);
        
        // Find devices
        auto deviceInfos = WinRTEnum::DeviceInformation::FindAllAsync(selector).get();
        
        spdlog::info("[MagicAAPWinRT] Found {} devices with AAP service", deviceInfos.Size());
        
        for (const auto& deviceInfo : deviceInfos) {
            auto id = deviceInfo.Id();
            auto name = deviceInfo.Name();
            
            std::string nameStr = ::winrt::to_string(name);
            std::string idStr = ::winrt::to_string(id);
            spdlog::info("[MagicAAPWinRT] Device: {} - {}", nameStr, idStr);
            
            // Try to get Bluetooth address from device
            try {
                auto rfcommService = WinRTRfcomm::RfcommDeviceService::FromIdAsync(id).get();
                if (rfcommService) {
                    auto btDevice = rfcommService.Device();
                    if (btDevice) {
                        uint64_t address = btDevice.BluetoothAddress();
                        devices.emplace_back(std::wstring(name), address);
                    }
                }
            } catch (const ::winrt::hresult_error& e) {
                std::string errStr = ::winrt::to_string(e.message());
                spdlog::warn("[MagicAAPWinRT] Failed to get device details: {}", errStr);
            }
        }
    } catch (const ::winrt::hresult_error& e) {
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error("[MagicAAPWinRT] EnumerateAAPDevices failed: {} (0x{:08x})",
                     errStr, static_cast<uint32_t>(e.code()));
    }
#else
    spdlog::warn("[MagicAAPWinRT] WinRT Bluetooth headers not available");
#endif
    
    return devices;
}

bool MagicAAPWinRTClient::Connect(uint64_t bluetoothAddress) {
#if HAS_WINRT_BLUETOOTH
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (_connected.load()) {
        SetError(L"Already connected");
        return false;
    }
    
    try {
        spdlog::info("[MagicAAPWinRT] Connecting to Bluetooth address: {:012X}", bluetoothAddress);
        
        // Get Bluetooth device
        auto btDevice = WinRTBluetooth::BluetoothDevice::FromBluetoothAddressAsync(bluetoothAddress).get();
        if (!btDevice) {
            SetError(L"Failed to get Bluetooth device");
            return false;
        }
        
        std::string deviceName = ::winrt::to_string(btDevice.Name());
        spdlog::info("[MagicAAPWinRT] Found device: {}", deviceName);
        
        // Get RFCOMM services with AAP UUID
        auto serviceId = WinRTRfcomm::RfcommServiceId::FromUuid(::winrt::guid(GUID_AAP_SERVICE));
        auto servicesResult = btDevice.GetRfcommServicesForIdAsync(serviceId).get();
        
        if (servicesResult.Error() != WinRTBluetooth::BluetoothError::Success) {
            SetError(L"Failed to get RFCOMM services: error " + 
                    std::to_wstring(static_cast<int>(servicesResult.Error())));
            return false;
        }
        
        auto services = servicesResult.Services();
        if (services.Size() == 0) {
            SetError(L"No AAP service found on device. Is MagicAAP driver installed?");
            return false;
        }
        
        spdlog::info("[MagicAAPWinRT] Found {} AAP services", services.Size());
        
        auto rfcommService = services.GetAt(0);
        
        // Create socket and connect
        WinRTSockets::StreamSocket socket;
        socket.Control().KeepAlive(true);
        
        spdlog::info("[MagicAAPWinRT] Connecting to service...");
        socket.ConnectAsync(
            rfcommService.ConnectionHostName(),
            rfcommService.ConnectionServiceName()
        ).get();
        
        spdlog::info("[MagicAAPWinRT] Connected!");
        
        // Create internal state
        auto impl = new MagicAAPWinRTClientImpl();
        impl->socket = std::move(socket);
        impl->service = rfcommService;
        impl->reader = WinRTStreams::DataReader(impl->socket.InputStream());
        impl->reader.InputStreamOptions(WinRTStreams::InputStreamOptions::Partial);
        impl->writer = WinRTStreams::DataWriter(impl->socket.OutputStream());
        _impl = impl;
        
        _connected.store(true);
        _stopReceiver.store(false);
        
        // Start receiver thread
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this);
        
        return true;
        
    } catch (const ::winrt::hresult_error& e) {
        SetError(std::wstring(e.message()));
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error("[MagicAAPWinRT] Connect failed: {} (0x{:08x})",
                     errStr, static_cast<uint32_t>(e.code()));
        CleanupWinRTObjects();
        return false;
    }
#else
    spdlog::error("[MagicAAPWinRT] WinRT Bluetooth not available");
    SetError(L"WinRT Bluetooth headers not available");
    return false;
#endif
}

bool MagicAAPWinRTClient::ConnectById(const std::wstring& deviceId) {
#if HAS_WINRT_BLUETOOTH
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (_connected.load()) {
        SetError(L"Already connected");
        return false;
    }
    
    try {
        // Convert wstring to string properly using WideCharToMultiByte would be better,
        // but for logging we just use a simple conversion
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::string idStr = converter.to_bytes(deviceId);
        spdlog::info("[MagicAAPWinRT] Connecting to device ID: {}", idStr);
        
        // Get RFCOMM service from device ID
        auto rfcommService = WinRTRfcomm::RfcommDeviceService::FromIdAsync(deviceId).get();
        if (!rfcommService) {
            SetError(L"Failed to get RFCOMM service");
            return false;
        }
        
        // Create socket and connect
        WinRTSockets::StreamSocket socket;
        socket.Control().KeepAlive(true);
        
        spdlog::info("[MagicAAPWinRT] Connecting to service...");
        socket.ConnectAsync(
            rfcommService.ConnectionHostName(),
            rfcommService.ConnectionServiceName()
        ).get();
        
        spdlog::info("[MagicAAPWinRT] Connected!");
        
        // Create internal state
        auto impl = new MagicAAPWinRTClientImpl();
        impl->socket = std::move(socket);
        impl->service = rfcommService;
        impl->reader = WinRTStreams::DataReader(impl->socket.InputStream());
        impl->reader.InputStreamOptions(WinRTStreams::InputStreamOptions::Partial);
        impl->writer = WinRTStreams::DataWriter(impl->socket.OutputStream());
        _impl = impl;
        
        _connected.store(true);
        _stopReceiver.store(false);
        
        // Start receiver thread
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this);
        
        return true;
        
    } catch (const ::winrt::hresult_error& e) {
        SetError(std::wstring(e.message()));
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error("[MagicAAPWinRT] ConnectById failed: {} (0x{:08x})",
                     errStr, static_cast<uint32_t>(e.code()));
        CleanupWinRTObjects();
        return false;
    }
#else
    SetError(L"WinRT Bluetooth not available");
    return false;
#endif
}

void MagicAAPWinRTClient::Disconnect() {
    _stopReceiver.store(true);
    
    if (_receiverThread.joinable()) {
        _receiverThread.join();
    }
    
    std::lock_guard<std::mutex> lock(_mutex);
    CleanupWinRTObjects();
    CleanupDeviceHandle();
    _connected.store(false);
    
    spdlog::info("[MagicAAPWinRT] Disconnected");
}

void MagicAAPWinRTClient::CleanupWinRTObjects() {
#if HAS_WINRT_BLUETOOTH
    if (_impl) {
        auto impl = static_cast<MagicAAPWinRTClientImpl*>(_impl);
        try {
            if (impl->writer) {
                impl->writer.DetachStream();
                impl->writer = nullptr;
            }
            if (impl->reader) {
                impl->reader.DetachStream();
                impl->reader = nullptr;
            }
            if (impl->socket) {
                impl->socket.Close();
                impl->socket = nullptr;
            }
            impl->service = nullptr;
        } catch (...) {
            // Ignore cleanup errors
        }
        delete impl;
        _impl = nullptr;
    }
#endif
}

bool MagicAAPWinRTClient::SendData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (!_connected.load()) {
        SetError(L"Not connected");
        return false;
    }
    
    // Handle device interface mode
    if (_usingDeviceInterface && _deviceHandle) {
        HANDLE hDevice = static_cast<HANDLE>(_deviceHandle);
        DWORD bytesWritten = 0;
        
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        
        BOOL result = WriteFile(hDevice, data.data(), static_cast<DWORD>(data.size()), 
                                &bytesWritten, &overlapped);
        
        if (!result && ::GetLastError() == ERROR_IO_PENDING) {
            // Wait for completion
            if (WaitForSingleObject(overlapped.hEvent, 5000) == WAIT_OBJECT_0) {
                GetOverlappedResult(hDevice, &overlapped, &bytesWritten, FALSE);
                result = TRUE;
            }
        }
        
        CloseHandle(overlapped.hEvent);
        
        if (result) {
            spdlog::debug("[MagicAAPWinRT] Sent {} bytes via device interface", bytesWritten);
            return true;
        } else {
            SetError(L"WriteFile failed: error " + std::to_wstring(::GetLastError()));
            spdlog::error("[MagicAAPWinRT] WriteFile failed: {}", ::GetLastError());
            return false;
        }
    }
    
#if HAS_WINRT_BLUETOOTH
    // Handle WinRT mode
    if (!_impl) {
        SetError(L"Not connected");
        return false;
    }
    
    auto impl = static_cast<MagicAAPWinRTClientImpl*>(_impl);
    if (!impl->writer) {
        SetError(L"Writer not available");
        return false;
    }
    
    try {
        // Write data
        impl->writer.WriteBytes(::winrt::array_view<const uint8_t>(data));
        impl->writer.StoreAsync().get();
        
        spdlog::debug("[MagicAAPWinRT] Sent {} bytes", data.size());
        return true;
        
    } catch (const ::winrt::hresult_error& e) {
        SetError(std::wstring(e.message()));
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error("[MagicAAPWinRT] SendData failed: {}", errStr);
        return false;
    }
#else
    return false;
#endif
}

void MagicAAPWinRTClient::ReceiverLoop() {
    spdlog::info("[MagicAAPWinRT] Receiver thread started");
    
    // Handle device interface mode
    if (_usingDeviceInterface && _deviceHandle) {
        HANDLE hDevice = static_cast<HANDLE>(_deviceHandle);
        std::vector<uint8_t> buffer(4096);
        
        while (!_stopReceiver.load() && _connected.load()) {
            OVERLAPPED overlapped = {};
            overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            
            DWORD bytesRead = 0;
            BOOL result = ReadFile(hDevice, buffer.data(), static_cast<DWORD>(buffer.size()), 
                                   &bytesRead, &overlapped);
            
            if (!result && ::GetLastError() == ERROR_IO_PENDING) {
                // Wait for data with timeout
                DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100);
                if (waitResult == WAIT_OBJECT_0) {
                    GetOverlappedResult(hDevice, &overlapped, &bytesRead, FALSE);
                    result = TRUE;
                } else if (waitResult == WAIT_TIMEOUT) {
                    CancelIo(hDevice);
                    CloseHandle(overlapped.hEvent);
                    continue;
                }
            }
            
            CloseHandle(overlapped.hEvent);
            
            if (result && bytesRead > 0) {
                std::vector<uint8_t> data(buffer.begin(), buffer.begin() + bytesRead);
                spdlog::debug("[MagicAAPWinRT] Received {} bytes via device interface", bytesRead);
                
                if (_onDataReceived) {
                    _onDataReceived(data);
                }
            }
        }
        
        spdlog::info("[MagicAAPWinRT] Receiver thread stopped (device interface)");
        if (_onDisconnected && _connected.load()) {
            _onDisconnected();
        }
        return;
    }
    
#if HAS_WINRT_BLUETOOTH
    try {
        while (!_stopReceiver.load() && _connected.load()) {
            if (!_impl) break;
            
            auto impl = static_cast<MagicAAPWinRTClientImpl*>(_impl);
            if (!impl->reader) break;
            
            try {
                // Read with timeout using async
                auto loadOp = impl->reader.LoadAsync(1024);
                
                // Wait for data with timeout
                auto status = loadOp.wait_for(std::chrono::milliseconds(100));
                
                if (status == WinRTFoundation::AsyncStatus::Completed) {
                    uint32_t bytesRead = loadOp.get();
                    if (bytesRead > 0) {
                        // Read the data
                        std::vector<uint8_t> buffer(bytesRead);
                        impl->reader.ReadBytes(buffer);
                        
                        spdlog::debug("[MagicAAPWinRT] Received {} bytes", bytesRead);
                        
                        // Call callback
                        if (_onDataReceived) {
                            _onDataReceived(buffer);
                        }
                    }
                } else if (status == WinRTFoundation::AsyncStatus::Started) {
                    // Still waiting, cancel and try again
                    loadOp.Cancel();
                }
                
            } catch (const ::winrt::hresult_error& e) {
                if (e.code() == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) {
                    continue; // Normal timeout
                }
                throw;
            }
        }
    } catch (const ::winrt::hresult_error& e) {
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error("[MagicAAPWinRT] Receiver error: {} (0x{:08x})",
                     errStr, static_cast<uint32_t>(e.code()));
    } catch (const std::exception& e) {
        spdlog::error("[MagicAAPWinRT] Receiver exception: {}", e.what());
    }
#endif
    
    spdlog::info("[MagicAAPWinRT] Receiver thread stopped");
    
    // Notify disconnection
    if (_onDisconnected && _connected.load()) {
        _onDisconnected();
    }
}

void MagicAAPWinRTClient::SetOnDataReceived(OnDataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _onDataReceived = std::move(callback);
}

void MagicAAPWinRTClient::SetOnDisconnected(OnDisconnectedCallback callback) {
    std::lock_guard<std::mutex> lock(_mutex);
    _onDisconnected = std::move(callback);
}

void MagicAAPWinRTClient::SetError(const std::wstring& error) {
    _lastError = error;
}

std::vector<std::wstring> MagicAAPWinRTClient::EnumerateMagicAAPDevices() {
    std::vector<std::wstring> devices;
    
    // Method 1: Search via BTHENUM enumerator (the way Python found the device)
    // This searches for Bluetooth devices that have a specific interface GUID
    {
        const GUID* guidsToTry[] = { &GUID_DEVINTERFACE_MAGICAAP, &GUID_DEVINTERFACE_AAP_SERVICE };
        
        for (const GUID* pGuid : guidsToTry) {
            HDEVINFO hDevInfo = SetupDiGetClassDevsW(
                pGuid,
                L"BTHENUM",  // Bluetooth enumerator
                nullptr,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            
            if (hDevInfo == INVALID_HANDLE_VALUE) {
                continue;
            }
            
            SP_DEVICE_INTERFACE_DATA interfaceData = {};
            interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
            
            for (DWORD index = 0; 
                 SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, pGuid, index, &interfaceData);
                 ++index) {
                
                DWORD requiredSize = 0;
                SetupDiGetDeviceInterfaceDetailW(hDevInfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);
                
                if (requiredSize == 0) continue;
                
                std::vector<uint8_t> buffer(requiredSize);
                auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
                detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                
                SP_DEVINFO_DATA devInfoData = {};
                devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
                
                if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &interfaceData, detailData, requiredSize, nullptr, &devInfoData)) {
                    std::wstring devicePath = detailData->DevicePath;
                    
                    // Avoid duplicates
                    bool found = false;
                    for (const auto& existing : devices) {
                        if (existing == devicePath) {
                            found = true;
                            break;
                        }
                    }
                    
                    if (!found) {
                        devices.push_back(devicePath);
                        
                        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
                        std::string pathStr = converter.to_bytes(devicePath);
                        spdlog::info("[MagicAAPWinRT] Found MagicAAP device via BTHENUM: {}", pathStr);
                    }
                }
            }
            
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }
    }
    
    // Method 2: Search without specifying enumerator (fallback)
    if (devices.empty()) {
        const GUID* guidsToTry[] = { &GUID_DEVINTERFACE_MAGICAAP, &GUID_DEVINTERFACE_AAP_SERVICE };
        
        for (const GUID* pGuid : guidsToTry) {
            HDEVINFO hDevInfo = SetupDiGetClassDevsW(
                pGuid,
                nullptr,
                nullptr,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            
            if (hDevInfo == INVALID_HANDLE_VALUE) {
                continue;
            }
            
            SP_DEVICE_INTERFACE_DATA interfaceData = {};
            interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
            
            for (DWORD index = 0; 
                 SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, pGuid, index, &interfaceData);
                 ++index) {
                
                DWORD requiredSize = 0;
                SetupDiGetDeviceInterfaceDetailW(hDevInfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);
                
                if (requiredSize == 0) continue;
                
                std::vector<uint8_t> buffer(requiredSize);
                auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
                detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                
                SP_DEVINFO_DATA devInfoData = {};
                devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
                
                if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &interfaceData, detailData, requiredSize, nullptr, &devInfoData)) {
                    std::wstring devicePath = detailData->DevicePath;
                    
                    // Avoid duplicates
                    bool found = false;
                    for (const auto& existing : devices) {
                        if (existing == devicePath) {
                            found = true;
                            break;
                        }
                    }
                    
                    if (!found) {
                        devices.push_back(devicePath);
                        
                        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
                        std::string pathStr = converter.to_bytes(devicePath);
                        spdlog::info("[MagicAAPWinRT] Found MagicAAP device: {}", pathStr);
                    }
                }
            }
            
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }
    }
    
    if (devices.empty()) {
        spdlog::warn("[MagicAAPWinRT] No MagicAAP device interfaces found");
    }
    
    return devices;
}

bool MagicAAPWinRTClient::ConnectViaDeviceInterface(uint64_t bluetoothAddress) {
    std::lock_guard<std::mutex> lock(_mutex);
    
    if (_connected.load()) {
        SetError(L"Already connected");
        return false;
    }
    
    spdlog::info("[MagicAAPWinRT] Attempting connection via device interface...");
    
    // Convert address to hex string for matching (format: XXXXXXXXXXXXXX)
    wchar_t addrStr[16];
    swprintf_s(addrStr, L"%012llX", bluetoothAddress);
    std::wstring addressHex = addrStr;
    spdlog::info("[MagicAAPWinRT] Looking for device with address: {}", 
        std::string(addressHex.begin(), addressHex.end()));
    
    // First enumerate available MagicAAP devices
    auto devicePaths = EnumerateMagicAAPDevices();
    
    if (devicePaths.empty()) {
        spdlog::warn("[MagicAAPWinRT] No enumerated devices, trying to construct path directly...");
        
        // Try to construct the device path directly based on known format:
        // \\?\BTHENUM#{74ec2172-0bad-4d01-8f77-997b2be0722a}_VID&0001004c_PID&xxxx#...#{9eec98bb-...}
        // We need to search for a pattern that includes the Bluetooth address
        
        // Try with the known interface GUID suffix
        HDEVINFO hDevInfo = SetupDiGetClassDevsW(
            nullptr,
            L"BTHENUM",
            nullptr,
            DIGCF_PRESENT | DIGCF_ALLCLASSES);
        
        if (hDevInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA devInfoData = {};
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
            
            for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); ++i) {
                // Get device instance ID
                wchar_t instanceId[512] = {};
                if (SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfoData, instanceId, 
                    sizeof(instanceId)/sizeof(wchar_t), nullptr)) {
                    
                    std::wstring instanceStr = instanceId;
                    // Check if this instance ID contains our Bluetooth address
                    // Format: BTHENUM\{GUID}_VID&xxxx_PID&xxxx\{InstID}&{Address}_C00000000
                    
                    // Also search without leading zeros in address
                    wchar_t addrStrNoZero[16];
                    swprintf_s(addrStrNoZero, L"%llX", bluetoothAddress);
                    std::wstring addressHexNoZero = addrStrNoZero;
                    
                    if (instanceStr.find(addressHex) != std::wstring::npos ||
                        instanceStr.find(addressHexNoZero) != std::wstring::npos) {
                        
                        spdlog::info("[MagicAAPWinRT] Found matching device instance: {}",
                            std::string(instanceStr.begin(), instanceStr.end()));
                        
                        // Now get the device interface for this device
                        SP_DEVICE_INTERFACE_DATA interfaceData = {};
                        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
                        
                        if (SetupDiEnumDeviceInterfaces(hDevInfo, &devInfoData, 
                            &GUID_DEVINTERFACE_MAGICAAP, 0, &interfaceData)) {
                            
                            DWORD requiredSize = 0;
                            SetupDiGetDeviceInterfaceDetailW(hDevInfo, &interfaceData, 
                                nullptr, 0, &requiredSize, nullptr);
                            
                            if (requiredSize > 0) {
                                std::vector<uint8_t> buffer(requiredSize);
                                auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
                                detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                                
                                if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &interfaceData, 
                                    detailData, requiredSize, nullptr, nullptr)) {
                                    devicePaths.push_back(detailData->DevicePath);
                                    spdlog::info("[MagicAAPWinRT] Found device path: {}",
                                        std::string(detailData->DevicePath, 
                                            detailData->DevicePath + wcslen(detailData->DevicePath)));
                                }
                            }
                        }
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }
    }
    
    if (devicePaths.empty()) {
        SetError(L"No MagicAAP device interfaces found");
        return false;
    }
    
    // Try to connect to devices that match the Bluetooth address
    for (const auto& devicePath : devicePaths) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::string pathStr = converter.to_bytes(devicePath);
        
        // Check if this device path contains our Bluetooth address
        std::wstring pathUpper = devicePath;
        std::transform(pathUpper.begin(), pathUpper.end(), pathUpper.begin(), ::towupper);
        
        std::wstring addrUpper = addressHex;
        std::transform(addrUpper.begin(), addrUpper.end(), addrUpper.begin(), ::towupper);
        
        // Also try without leading zeros
        wchar_t addrStrNoZero[16];
        swprintf_s(addrStrNoZero, L"%llX", bluetoothAddress);
        std::wstring addrNoZeroUpper = addrStrNoZero;
        std::transform(addrNoZeroUpper.begin(), addrNoZeroUpper.end(), addrNoZeroUpper.begin(), ::towupper);
        
        if (pathUpper.find(addrUpper) == std::wstring::npos &&
            pathUpper.find(addrNoZeroUpper) == std::wstring::npos) {
            spdlog::info("[MagicAAPWinRT] Skipping device (address mismatch): {}", pathStr);
            continue;
        }
        
        spdlog::info("[MagicAAPWinRT] Trying to open matching device: {}", pathStr);
        
        // Open the device
        HANDLE hDevice = CreateFileW(
            devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        
        if (hDevice == INVALID_HANDLE_VALUE) {
            DWORD err = ::GetLastError();
            spdlog::warn("[MagicAAPWinRT] CreateFile failed: {} (error {})", pathStr, err);
            continue;
        }
        
        spdlog::info("[MagicAAPWinRT] Successfully opened device!");
        
        _deviceHandle = hDevice;
        _usingDeviceInterface = true;
        _connected.store(true);
        _stopReceiver.store(false);
        
        // Start receiver thread for device interface
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this);
        
        return true;
    }
    
    // If we had devices but none matched the address, try them all as fallback
    spdlog::info("[MagicAAPWinRT] No address-matched device found, trying all devices...");
    for (const auto& devicePath : devicePaths) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        std::string pathStr = converter.to_bytes(devicePath);
        spdlog::info("[MagicAAPWinRT] Trying to open device (fallback): {}", pathStr);
        
        HANDLE hDevice = CreateFileW(
            devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        
        if (hDevice == INVALID_HANDLE_VALUE) {
            DWORD err = ::GetLastError();
            spdlog::warn("[MagicAAPWinRT] CreateFile failed: {} (error {})", pathStr, err);
            continue;
        }
        
        spdlog::info("[MagicAAPWinRT] Successfully opened device!");
        
        _deviceHandle = hDevice;
        _usingDeviceInterface = true;
        _connected.store(true);
        _stopReceiver.store(false);
        
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this);
        
        return true;
    }
    
    SetError(L"Failed to open any MagicAAP device interface");
    return false;
}

void MagicAAPWinRTClient::CleanupDeviceHandle() {
    if (_deviceHandle && _deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(_deviceHandle));
        _deviceHandle = nullptr;
    }
    _usingDeviceInterface = false;
}

} // namespace Core::MagicAAPWinRT
