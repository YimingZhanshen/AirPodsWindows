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

#include "MagicAAPWinRT.h"

#include "AAPMessage.h"

#include <Windows.h>
#include <combaseapi.h>
#include <SetupAPI.h>
#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <winsvc.h>
#include <locale>
#include <codecvt>
#include <exception>
#include <limits>
#include <utility>

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
static constexpr GUID GUID_AAP_SERVICE = {
    0x74ec2172, 0x0bad, 0x4d01, {0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0, 0x72, 0x2a}};

// MagicAAP Device Interface GUID (from driver analysis)
// The driver registers TWO device interfaces:
// 1. {74EC2172-0BAD-4D01-8F77-997B2BE0722A} - AAP Service UUID (same as Bluetooth service)
// 2. {9EEC98BB-3C54-45D4-A843-7900C4635E08} - Custom MagicAAP interface
DEFINE_GUID(
    GUID_DEVINTERFACE_MAGICAAP, 0x9EEC98BB, 0x3C54, 0x45D4, 0xA8, 0x43, 0x79, 0x00, 0xC4, 0x63,
    0x5E, 0x08);

// Alternative: Use AAP Service UUID as device interface
DEFINE_GUID(
    GUID_DEVINTERFACE_AAP_SERVICE, 0x74ec2172, 0x0bad, 0x4d01, 0x8f, 0x77, 0x99, 0x7b, 0x2b, 0xe0,
    0x72, 0x2a);

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
struct MagicAAPWinRTClientImpl {
};
#endif

MagicAAPWinRTClient::MagicAAPWinRTClient()
{
#if HAS_WINRT_BLUETOOTH
    // Initialize WinRT apartment
    ::winrt::init_apartment();
#endif
}

MagicAAPWinRTClient::~MagicAAPWinRTClient()
{
    Disconnect();
}

bool MagicAAPWinRTClient::IsDriverInstalled()
{
    // Check if MagicAAP service exists
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) {
        return false;
    }

    SC_HANDLE service = OpenServiceW(scm, L"MagicAAP", SERVICE_QUERY_STATUS);
    bool exists = (service != nullptr);

    if (service)
        CloseServiceHandle(service);
    CloseServiceHandle(scm);

    return exists;
}

bool MagicAAPWinRTClient::IsDriverRunning()
{
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
    bool running =
        QueryServiceStatus(service, &status) && (status.dwCurrentState == SERVICE_RUNNING);

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    return running;
}

std::vector<std::pair<std::wstring, uint64_t>> MagicAAPWinRTClient::EnumerateAAPDevices()
{
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

        for (const auto &deviceInfo : deviceInfos) {
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
            }
            catch (const ::winrt::hresult_error &e) {
                std::string errStr = ::winrt::to_string(e.message());
                spdlog::warn("[MagicAAPWinRT] Failed to get device details: {}", errStr);
            }
        }
    }
    catch (const ::winrt::hresult_error &e) {
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error(
            "[MagicAAPWinRT] EnumerateAAPDevices failed: {} (0x{:08x})", errStr,
            static_cast<uint32_t>(e.code()));
    }
#else
    spdlog::warn("[MagicAAPWinRT] WinRT Bluetooth headers not available");
#endif

    return devices;
}

bool MagicAAPWinRTClient::Connect(uint64_t bluetoothAddress)
{
#if HAS_WINRT_BLUETOOTH
    std::lock_guard<std::mutex> lock(_mutex);

    if (_connected.load()) {
        SetError(L"Already connected");
        return false;
    }

    try {
        spdlog::info("[MagicAAPWinRT] Connecting to Bluetooth address: {:012X}", bluetoothAddress);

        // Get Bluetooth device
        auto btDevice =
            WinRTBluetooth::BluetoothDevice::FromBluetoothAddressAsync(bluetoothAddress).get();
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
            SetError(
                L"Failed to get RFCOMM services: error " +
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
        socket
            .ConnectAsync(rfcommService.ConnectionHostName(), rfcommService.ConnectionServiceName())
            .get();

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

        // Copy callbacks and publish the session before the thread can run.
        auto snapshot = StartSession();
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this, snapshot);

        return true;
    }
    catch (const ::winrt::hresult_error &e) {
        SetError(std::wstring(e.message()));
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error(
            "[MagicAAPWinRT] Connect failed: {} (0x{:08x})", errStr,
            static_cast<uint32_t>(e.code()));
        CleanupWinRTObjects();
        return false;
    }
#else
    std::lock_guard<std::mutex> lock(_mutex);
    spdlog::error("[MagicAAPWinRT] WinRT Bluetooth not available");
    SetError(L"WinRT Bluetooth headers not available");
    return false;
#endif
}

bool MagicAAPWinRTClient::ConnectById(const std::wstring &deviceId)
{
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
        socket
            .ConnectAsync(rfcommService.ConnectionHostName(), rfcommService.ConnectionServiceName())
            .get();

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

        // Copy callbacks and publish the session before the thread can run.
        auto snapshot = StartSession();
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this, snapshot);

        return true;
    }
    catch (const ::winrt::hresult_error &e) {
        SetError(std::wstring(e.message()));
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error(
            "[MagicAAPWinRT] ConnectById failed: {} (0x{:08x})", errStr,
            static_cast<uint32_t>(e.code()));
        CleanupWinRTObjects();
        return false;
    }
#else
    std::lock_guard<std::mutex> lock(_mutex);
    SetError(L"WinRT Bluetooth not available");
    return false;
#endif
}

void MagicAAPWinRTClient::Disconnect()
{
    _stopReceiver.store(true);

    if (_receiverThread.joinable()) {
        _receiverThread.join();
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _sessions.Close();
    CleanupWinRTObjects();
    CleanupDeviceHandle();
    _connected.store(false);

    spdlog::info("[MagicAAPWinRT] Disconnected");
}

void MagicAAPWinRTClient::CleanupWinRTObjects()
{
#if HAS_WINRT_BLUETOOTH
    if (_impl) {
        auto impl = static_cast<MagicAAPWinRTClientImpl *>(_impl);
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
        }
        catch (...) {
            // Ignore cleanup errors
        }
        delete impl;
        _impl = nullptr;
    }
#endif
}

bool MagicAAPWinRTClient::SendData(const std::vector<uint8_t> &data)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_connected.load()) {
        SetError(L"Not connected");
        return false;
    }

    // Handle device interface mode
    if (_usingDeviceInterface && _deviceHandle) {
        HANDLE hDevice = static_cast<HANDLE>(_deviceHandle);
        if (data.size() > (std::numeric_limits<DWORD>::max)()) {
            SetError(L"WriteFile record is too large");
            return false;
        }
        DWORD bytesWritten = 0;

        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            SetError(L"WriteFile event creation failed");
            return false;
        }

        BOOL result = WriteFile(
            hDevice, data.data(), static_cast<DWORD>(data.size()), &bytesWritten, &overlapped);

        if (!result && ::GetLastError() == ERROR_IO_PENDING) {
            // Wait for completion. On timeout, cancel and drain the operation before
            // destroying its stack OVERLAPPED and event.
            if (WaitForSingleObject(overlapped.hEvent, 5000) == WAIT_OBJECT_0) {
                result = GetOverlappedResult(hDevice, &overlapped, &bytesWritten, FALSE);
            }
            else {
                CancelIoEx(hDevice, &overlapped);
                result = GetOverlappedResult(hDevice, &overlapped, &bytesWritten, TRUE);
            }
        }

        const DWORD writeError = ::GetLastError();
        CloseHandle(overlapped.hEvent);

        if (result && bytesWritten == data.size()) {
            spdlog::debug("[MagicAAPWinRT] Sent {} bytes via device interface", bytesWritten);
            return true;
        }
        if (result) {
            SetError(
                L"WriteFile completed a partial record: " + std::to_wstring(bytesWritten) +
                L" of " + std::to_wstring(data.size()) + L" bytes");
            spdlog::error(
                "[MagicAAPWinRT] Atomic WriteFile failed: wrote {} of {} bytes", bytesWritten,
                data.size());
        }
        else {
            SetError(L"WriteFile failed: error " + std::to_wstring(writeError));
            spdlog::error("[MagicAAPWinRT] WriteFile failed: {}", writeError);
        }
        return false;
    }

#if HAS_WINRT_BLUETOOTH
    // Handle WinRT mode
    if (!_impl) {
        SetError(L"Not connected");
        return false;
    }

    auto impl = static_cast<MagicAAPWinRTClientImpl *>(_impl);
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
    }
    catch (const ::winrt::hresult_error &e) {
        SetError(std::wstring(e.message()));
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error("[MagicAAPWinRT] SendData failed: {}", errStr);
        return false;
    }
#else
    return false;
#endif
}

MagicAAPWinRTClient::ReceiverSnapshot MagicAAPWinRTClient::StartSession()
{
    ReceiverSnapshot snapshot;
    snapshot.session = _sessions.Start();
    snapshot.deviceHandle = _deviceHandle;
    snapshot.impl = _impl;
    snapshot.usingDeviceInterface = _usingDeviceInterface;
    snapshot.onDataReceived = _onDataReceived;
    snapshot.onDisconnected = _onDisconnected;
    return snapshot;
}

void MagicAAPWinRTClient::ReceiverLoop(ReceiverSnapshot snapshot)
{
    spdlog::info("[MagicAAPWinRT] Receiver thread started");

    // Handle device interface mode
    if (snapshot.usingDeviceInterface && snapshot.deviceHandle) {
        HANDLE hDevice = static_cast<HANDLE>(snapshot.deviceHandle);
        // One extra byte detects an overlong driver record; the transport rejects
        // anything above the 4096-byte AAPMessage limit.
        std::vector<uint8_t> buffer(Core::AAP::AAPMessage::kMaxRecordSize + 1);

        while (!_stopReceiver.load() && _connected.load()) {
            OVERLAPPED overlapped = {};
            overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (overlapped.hEvent == nullptr) {
                spdlog::error(
                    "[MagicAAPWinRT] ReadFile event creation failed: {}", ::GetLastError());
                break;
            }

            DWORD bytesRead = 0;
            BOOL result = ReadFile(
                hDevice, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);

            if (!result && ::GetLastError() == ERROR_IO_PENDING) {
                const DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100);
                if (waitResult == WAIT_OBJECT_0) {
                    result = GetOverlappedResult(hDevice, &overlapped, &bytesRead, FALSE);
                }
                else {
                    // The OVERLAPPED lives on this stack. Drain cancellation before
                    // closing its event or issuing another read, including on Disconnect.
                    CancelIoEx(hDevice, &overlapped);
                    result = GetOverlappedResult(hDevice, &overlapped, &bytesRead, TRUE);
                }
            }

            const DWORD readError = ::GetLastError();
            CloseHandle(overlapped.hEvent);

            if (result && bytesRead > 0) {
                std::vector<uint8_t> data(buffer.begin(), buffer.begin() + bytesRead);
                spdlog::debug("[MagicAAPWinRT] Received {} bytes via device interface", bytesRead);

                if (snapshot.onDataReceived && _connected.load() &&
                    _sessions.IsCurrent(snapshot.session))
                {
                    snapshot.onDataReceived(data);
                }
            }
            else if (result) {
                spdlog::error("[MagicAAPWinRT] Rejected empty device-interface record");
            }
            else if (!_stopReceiver.load() && readError != ERROR_OPERATION_ABORTED) {
                spdlog::error("[MagicAAPWinRT] Device-interface ReadFile failed: {}", readError);
            }
        }

        spdlog::info("[MagicAAPWinRT] Receiver thread stopped (device interface)");
        if (snapshot.onDisconnected && _connected.load() && _sessions.IsCurrent(snapshot.session)) {
            snapshot.onDisconnected();
        }
        return;
    }

#if HAS_WINRT_BLUETOOTH
    try {
        while (!_stopReceiver.load() && _connected.load()) {
            if (!snapshot.impl)
                break;

            auto impl = static_cast<MagicAAPWinRTClientImpl *>(snapshot.impl);
            if (!impl->reader)
                break;

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
                        if (snapshot.onDataReceived && _connected.load() &&
                            _sessions.IsCurrent(snapshot.session))
                        {
                            snapshot.onDataReceived(buffer);
                        }
                    }
                }
                else if (status == WinRTFoundation::AsyncStatus::Started) {
                    // Still waiting, cancel and try again
                    loadOp.Cancel();
                }
            }
            catch (const ::winrt::hresult_error &e) {
                if (e.code() == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) {
                    continue; // Normal timeout
                }
                throw;
            }
        }
    }
    catch (const ::winrt::hresult_error &e) {
        std::string errStr = ::winrt::to_string(e.message());
        spdlog::error(
            "[MagicAAPWinRT] Receiver error: {} (0x{:08x})", errStr,
            static_cast<uint32_t>(e.code()));
    }
    catch (const std::exception &e) {
        spdlog::error("[MagicAAPWinRT] Receiver exception: {}", e.what());
    }
#endif

    spdlog::info("[MagicAAPWinRT] Receiver thread stopped");

    // Notify disconnection
    if (snapshot.onDisconnected && _connected.load() && _sessions.IsCurrent(snapshot.session)) {
        snapshot.onDisconnected();
    }
}

void MagicAAPWinRTClient::SetOnDataReceived(OnDataReceivedCallback callback)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _onDataReceived = std::move(callback);
}

void MagicAAPWinRTClient::SetOnDisconnected(OnDisconnectedCallback callback)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _onDisconnected = std::move(callback);
}

void MagicAAPWinRTClient::SetError(const std::wstring &error)
{
    _lastError = error;
}

std::vector<MagicAAPDeviceInterface> MagicAAPWinRTClient::EnumerateInterfaces()
{
    std::vector<MagicAAPDeviceInterface> interfaces;
    const GUID *guidsToTry[] = {&GUID_DEVINTERFACE_MAGICAAP, &GUID_DEVINTERFACE_AAP_SERVICE};

    auto enumerate = [&](const GUID *interfaceGuid, const wchar_t *enumerator) {
        HDEVINFO hDevInfo = SetupDiGetClassDevsW(
            interfaceGuid, enumerator, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) {
            return;
        }

        SP_DEVICE_INTERFACE_DATA interfaceData = {};
        interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        for (DWORD index = 0;
             SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, interfaceGuid, index, &interfaceData);
             ++index)
        {
            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailW(
                hDevInfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);
            if (requiredSize == 0) {
                continue;
            }

            std::vector<uint8_t> buffer(requiredSize);
            auto detailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devInfoData = {};
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
            if (!SetupDiGetDeviceInterfaceDetailW(
                    hDevInfo, &interfaceData, detailData, requiredSize, nullptr, &devInfoData))
            {
                continue;
            }

            MagicAAPDeviceInterface device;
            device.interfacePath = detailData->DevicePath;

            wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
            if (SetupDiGetDeviceInstanceIdW(
                    hDevInfo, &devInfoData, instanceId, MAX_DEVICE_ID_LEN, nullptr))
            {
                device.deviceInstanceId = instanceId;
            }

            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, devInfoData.DevInst, 0) == CR_SUCCESS) {
                wchar_t parentId[MAX_DEVICE_ID_LEN] = {};
                if (CM_Get_Device_IDW(parent, parentId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
                    device.parentInstanceId = parentId;
                }
            }

            DEVPROPTYPE propertyType = DEVPROP_TYPE_EMPTY;
            GUID containerId = {};
            if (SetupDiGetDevicePropertyW(
                    hDevInfo, &devInfoData, &DEVPKEY_Device_ContainerId, &propertyType,
                    reinterpret_cast<PBYTE>(&containerId), sizeof(containerId), nullptr, 0) &&
                propertyType == DEVPROP_TYPE_GUID)
            {
                wchar_t container[39] = {};
                if (StringFromGUID2(containerId, container, ARRAYSIZE(container)) > 0) {
                    device.containerId = container;
                }
            }
            interfaces.push_back(std::move(device));
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    };

    for (const GUID *interfaceGuid : guidsToTry) {
        enumerate(interfaceGuid, L"BTHENUM");
    }
    // Some driver packages omit the enumerator qualifier. This broadens only
    // enumeration; it never broadens the identity gate or opens a device.
    if (interfaces.empty()) {
        for (const GUID *interfaceGuid : guidsToTry) {
            enumerate(interfaceGuid, nullptr);
        }
    }

    SortInterfacesByPath(interfaces);
    DeduplicateInterfaces(interfaces);
    spdlog::info("[MagicAAPWinRT] Enumerated {} identifiable device interfaces", interfaces.size());
    return interfaces;
}

MagicAAPBindStatus
MagicAAPWinRTClient::ConnectViaDeviceInterface(uint64_t bluetoothAddress, std::stop_token stop)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_connected.load()) {
        SetError(L"Already connected");
        return MagicAAPBindStatus::TransportError;
    }
    if (stop.stop_requested()) {
        SetError(L"Connection cancelled");
        return MagicAAPBindStatus::Cancelled;
    }
    if (!IsDriverInstalled() || !IsDriverRunning()) {
        SetError(L"MagicAAP driver is not installed or not running");
        return MagicAAPBindStatus::DriverMissing;
    }

    auto interfaces = EnumerateInterfaces();
    if (interfaces.empty()) {
        SetError(L"MagicAAP published no device interfaces; reinstall or restart the driver");
        return MagicAAPBindStatus::TransportError;
    }

    DWORD openError = 0;
    auto outcome = BindToTargetInterface(
        interfaces, bluetoothAddress,
        [&](const MagicAAPDeviceInterface &device) -> MagicAAPDeviceHandle {
            if (stop.stop_requested()) {
                return nullptr;
            }
            HANDLE handle = CreateFileW(
                device.interfacePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                openError = ::GetLastError();
                spdlog::warn("[MagicAAPWinRT] Exact target interface open failed: {}", openError);
                return nullptr;
            }
            return handle;
        },
        stop);

    switch (outcome.status) {
    case MagicAAPBindStatus::NoMatch:
        SetError(L"No MagicAAP interface proves the bound Bluetooth address and ContainerId; "
                 L"update the MagicAAP driver and reconnect");
        break;
    case MagicAAPBindStatus::OpenFailed:
        SetError(
            L"The exact MagicAAP target interface could not be opened (Win32 error " +
            std::to_wstring(openError) + L"); no other device was selected");
        break;
    case MagicAAPBindStatus::Cancelled:
        SetError(L"Connection cancelled");
        break;
    case MagicAAPBindStatus::Connected:
        break;
    default:
        SetError(L"MagicAAP transport error");
        break;
    }

    if (outcome.status != MagicAAPBindStatus::Connected) {
        return outcome.status;
    }
    if (stop.stop_requested()) {
        CloseHandle(static_cast<HANDLE>(outcome.handle));
        SetError(L"Connection cancelled");
        return MagicAAPBindStatus::Cancelled;
    }

    _deviceHandle = outcome.handle;
    _usingDeviceInterface = true;
    _connected.store(true);
    _stopReceiver.store(false);
    auto snapshot = StartSession();
    try {
        _receiverThread = std::thread(&MagicAAPWinRTClient::ReceiverLoop, this, snapshot);
    }
    catch (const std::exception &error) {
        spdlog::error("[MagicAAPWinRT] Receiver thread creation failed: {}", error.what());
        _sessions.Close();
        CleanupDeviceHandle();
        _connected.store(false);
        SetError(L"MagicAAP receiver thread could not be started");
        return MagicAAPBindStatus::TransportError;
    }

    spdlog::info(
        "[MagicAAPWinRT] Bound exact target device ({} candidate path(s), {} open attempt(s))",
        outcome.candidateCount, outcome.openAttempts);
    return MagicAAPBindStatus::Connected;
}

void MagicAAPWinRTClient::CleanupDeviceHandle()
{
    if (_deviceHandle && _deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(_deviceHandle));
        _deviceHandle = nullptr;
    }
    _usingDeviceInterface = false;
}

} // namespace Core::MagicAAPWinRT
