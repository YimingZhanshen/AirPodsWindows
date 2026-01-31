// MagicAAP Driver Communication Implementation
// Provides L2CAP-like communication to AirPods via MagicAAP kernel driver

#include "MagicAAPClient.h"
#include "../Logger.h"

#include <devguid.h>
#include <regstr.h>

namespace Core::MagicAAP {

MagicAAPClient::~MagicAAPClient()
{
    StopAsyncReceive();
    Disconnect();
}

MagicAAPClient::MagicAAPClient(MagicAAPClient&& other) noexcept
    : m_hDevice(other.m_hDevice)
    , m_lastError(std::move(other.m_lastError))
    , m_dataCallback(std::move(other.m_dataCallback))
    , m_asyncReceiveRunning(other.m_asyncReceiveRunning)
    , m_hReceiveThread(other.m_hReceiveThread)
{
    other.m_hDevice = INVALID_HANDLE_VALUE;
    other.m_asyncReceiveRunning = false;
    other.m_hReceiveThread = nullptr;
}

MagicAAPClient& MagicAAPClient::operator=(MagicAAPClient&& other) noexcept
{
    if (this != &other) {
        StopAsyncReceive();
        Disconnect();
        
        m_hDevice = other.m_hDevice;
        m_lastError = std::move(other.m_lastError);
        m_dataCallback = std::move(other.m_dataCallback);
        m_asyncReceiveRunning = other.m_asyncReceiveRunning;
        m_hReceiveThread = other.m_hReceiveThread;
        
        other.m_hDevice = INVALID_HANDLE_VALUE;
        other.m_asyncReceiveRunning = false;
        other.m_hReceiveThread = nullptr;
    }
    return *this;
}

bool MagicAAPClient::IsDriverInstalled()
{
    // Check if MagicAAP service exists
    SC_HANDLE hScManager = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScManager) {
        return false;
    }

    SC_HANDLE hService = OpenServiceW(hScManager, L"MagicAAP", SERVICE_QUERY_STATUS);
    bool installed = (hService != nullptr);
    
    if (hService) {
        CloseServiceHandle(hService);
    }
    CloseServiceHandle(hScManager);
    
    return installed;
}

std::wstring MagicAAPClient::GetDevicePath(const GUID& interfaceGuid, DWORD index)
{
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &interfaceGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return L"";
    }

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData = {};
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (!SetupDiEnumDeviceInterfaces(
            hDevInfo,
            nullptr,
            &interfaceGuid,
            index,
            &deviceInterfaceData)) {
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return L"";
    }

    // Get required buffer size
    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetailW(
        hDevInfo,
        &deviceInterfaceData,
        nullptr,
        0,
        &requiredSize,
        nullptr
    );

    if (requiredSize == 0) {
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return L"";
    }

    // Allocate buffer
    std::vector<BYTE> buffer(requiredSize);
    auto* pDetailData = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
    pDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

    if (!SetupDiGetDeviceInterfaceDetailW(
            hDevInfo,
            &deviceInterfaceData,
            pDetailData,
            requiredSize,
            nullptr,
            nullptr)) {
        SetupDiDestroyDeviceInfoList(hDevInfo);
        return L"";
    }

    std::wstring devicePath = pDetailData->DevicePath;
    SetupDiDestroyDeviceInfoList(hDevInfo);
    
    return devicePath;
}

std::vector<std::wstring> MagicAAPClient::EnumerateDevices()
{
    std::vector<std::wstring> devices;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_MAGICAAP,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return devices;
    }

    for (DWORD index = 0; ; ++index) {
        std::wstring path = GetDevicePath(GUID_DEVINTERFACE_MAGICAAP, index);
        if (path.empty()) {
            break;
        }
        devices.push_back(path);
        Log("MagicAAP: Found device: {}", WideStringToString(path));
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return devices;
}

bool MagicAAPClient::Connect(const std::wstring& devicePath)
{
    if (IsConnected()) {
        Disconnect();
    }

    Log("MagicAAP: Connecting to device: {}", WideStringToString(devicePath));

    // Try to open the device with CreateFile
    m_hDevice = CreateFileW(
        devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (m_hDevice == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        SetError(L"Failed to open device: error " + std::to_wstring(error));
        Log("MagicAAP: Failed to open device, error: {}", error);
        return false;
    }

    Log("MagicAAP: Successfully connected to device");
    return true;
}

bool MagicAAPClient::ConnectFirst()
{
    auto devices = EnumerateDevices();
    if (devices.empty()) {
        SetError(L"No MagicAAP devices found");
        Log("MagicAAP: No devices found");
        return false;
    }

    return Connect(devices[0]);
}

void MagicAAPClient::Disconnect()
{
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
        Log("MagicAAP: Disconnected");
    }
}

bool MagicAAPClient::SendData(const std::vector<uint8_t>& data)
{
    if (!IsConnected()) {
        SetError(L"Not connected");
        return false;
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    
    if (!overlapped.hEvent) {
        SetError(L"Failed to create event");
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL result = WriteFile(
        m_hDevice,
        data.data(),
        static_cast<DWORD>(data.size()),
        &bytesWritten,
        &overlapped
    );

    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // Wait for completion
            WaitForSingleObject(overlapped.hEvent, 5000);
            GetOverlappedResult(m_hDevice, &overlapped, &bytesWritten, FALSE);
        } else {
            CloseHandle(overlapped.hEvent);
            SetError(L"WriteFile failed: error " + std::to_wstring(error));
            return false;
        }
    }

    CloseHandle(overlapped.hEvent);
    
    Log("MagicAAP: Sent {} bytes", bytesWritten);
    return bytesWritten == data.size();
}

std::optional<std::vector<uint8_t>> MagicAAPClient::ReceiveData(DWORD timeoutMs)
{
    if (!IsConnected()) {
        SetError(L"Not connected");
        return std::nullopt;
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    
    if (!overlapped.hEvent) {
        SetError(L"Failed to create event");
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(1024);
    DWORD bytesRead = 0;
    
    BOOL result = ReadFile(
        m_hDevice,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytesRead,
        &overlapped
    );

    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // Wait for completion with timeout
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
            if (waitResult == WAIT_TIMEOUT) {
                CancelIo(m_hDevice);
                CloseHandle(overlapped.hEvent);
                return std::nullopt;
            }
            GetOverlappedResult(m_hDevice, &overlapped, &bytesRead, FALSE);
        } else {
            CloseHandle(overlapped.hEvent);
            SetError(L"ReadFile failed: error " + std::to_wstring(error));
            return std::nullopt;
        }
    }

    CloseHandle(overlapped.hEvent);

    if (bytesRead > 0) {
        buffer.resize(bytesRead);
        Log("MagicAAP: Received {} bytes", bytesRead);
        return buffer;
    }

    return std::nullopt;
}

bool MagicAAPClient::StartAsyncReceive()
{
    // TODO: Implement async receive thread
    return false;
}

void MagicAAPClient::StopAsyncReceive()
{
    // TODO: Implement async receive stop
}

void MagicAAPClient::SetError(const std::wstring& msg)
{
    m_lastError = msg;
}

// Helper function to convert wide string to narrow string
static std::string WideStringToString(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
    return str;
}

} // namespace Core::MagicAAP
