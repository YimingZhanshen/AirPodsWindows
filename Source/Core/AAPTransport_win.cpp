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

#include "AAPTransport.h"

#include "AAP.h"
#include "AAPMessage.h"
#include "MagicAAPWinRT.h"

#include <WinSock2.h>
#include <ws2bth.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#pragma comment(lib, "Ws2_32.lib")

namespace Core::AAP {

namespace {

constexpr auto kReceivePollInterval = std::chrono::milliseconds(100);

bool ValidateRecord(std::span<const uint8_t> record, AAPTransportKind kind)
{
    const auto result = AAPMessage::Validate(record);
    if (result == AAPMessage::ValidationResult::Valid) {
        return true;
    }
    const std::string_view kindName =
        kind == AAPTransportKind::SeqPacket ? "SEQPACKET" : "MagicAAP";
    spdlog::error(
        "[AAPTransport] {} rejected {}-byte record: {}", kindName, record.size(),
        AAPMessage::Describe(result));
    return false;
}

} // namespace

SeqPacketTransport::SeqPacketTransport()
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
        _winsockInitialized = true;
    }
    else {
        spdlog::error("[AAPTransport] WSAStartup failed: {}", WSAGetLastError());
    }
}

SeqPacketTransport::~SeqPacketTransport()
{
    Close();
}

bool SeqPacketTransport::Connect(uint64_t address, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return false;
    }

    Close();
    if (!_winsockInitialized) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            spdlog::error("[AAPTransport] WSAStartup failed: {}", WSAGetLastError());
            return false;
        }
        _winsockInitialized = true;
    }

    std::lock_guard<std::mutex> lock{_ioMutex};
    // P1 deliberately provides no L2CAP STREAM or RFCOMM fallback: neither
    // preserves AAP record boundaries.
    SOCKET socketHandle = socket(AF_BTH, SOCK_SEQPACKET, BTHPROTO_L2CAP);
    if (socketHandle == INVALID_SOCKET) {
        spdlog::error("[AAPTransport] SEQPACKET socket creation failed: {}", WSAGetLastError());
        return false;
    }

    SOCKADDR_BTH addressRecord{};
    addressRecord.addressFamily = AF_BTH;
    addressRecord.btAddr = address;
    addressRecord.port = kPSM;
    if (connect(
            socketHandle, reinterpret_cast<SOCKADDR *>(&addressRecord), sizeof(addressRecord)) ==
        SOCKET_ERROR)
    {
        spdlog::error("[AAPTransport] SEQPACKET connect failed: {}", WSAGetLastError());
        closesocket(socketHandle);
        return false;
    }

    _socket = reinterpret_cast<void *>(socketHandle);
    _closed = false;
    spdlog::info("[AAPTransport] Connected with L2CAP SEQPACKET");
    return true;
}

bool SeqPacketTransport::Send(std::span<const uint8_t> data, std::stop_token stop)
{
    if (stop.stop_requested() || !ValidateRecord(data, Kind())) {
        return false;
    }

    std::lock_guard<std::mutex> lock{_ioMutex};
    if (_closed || _socket == nullptr) {
        return false;
    }

    const int sent = send(
        reinterpret_cast<SOCKET>(_socket), reinterpret_cast<const char *>(data.data()),
        static_cast<int>(data.size()), 0);
    if (sent == SOCKET_ERROR) {
        spdlog::error("[AAPTransport] SEQPACKET send failed: {}", WSAGetLastError());
        return false;
    }
    if (static_cast<size_t>(sent) != data.size()) {
        spdlog::error(
            "[AAPTransport] Atomic SEQPACKET send failed: wrote {} of {} bytes", sent, data.size());
        return false;
    }
    return true;
}

std::optional<std::vector<uint8_t>> SeqPacketTransport::Receive(std::stop_token stop)
{
    std::array<uint8_t, AAPMessage::kMaxRecordSize + 1> buffer{};
    std::lock_guard<std::mutex> lock{_ioMutex};

    while (!stop.stop_requested() && !_closed && _socket != nullptr) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(reinterpret_cast<SOCKET>(_socket), &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = static_cast<long>(kReceivePollInterval.count() * 1000);

        const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEINTR) {
                continue;
            }
            spdlog::error("[AAPTransport] SEQPACKET select failed: {}", error);
            return std::nullopt;
        }
        if (selected == 0) {
            continue;
        }

        const int received = recv(
            reinterpret_cast<SOCKET>(_socket), reinterpret_cast<char *>(buffer.data()),
            static_cast<int>(buffer.size()), 0);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK || error == WSAEINTR) {
                continue;
            }
            if (error == WSAEMSGSIZE) {
                spdlog::error("[AAPTransport] SEQPACKET rejected oversize or truncated record");
                continue;
            }
            spdlog::error("[AAPTransport] SEQPACKET receive failed: {}", error);
            return std::nullopt;
        }
        if (received == 0) {
            spdlog::info("[AAPTransport] SEQPACKET peer closed the connection");
            return std::nullopt;
        }

        const std::span<const uint8_t> record(buffer.data(), static_cast<size_t>(received));
        if (received > static_cast<int>(AAPMessage::kMaxRecordSize)) {
            spdlog::error("[AAPTransport] SEQPACKET rejected truncated {}-byte record", received);
            continue;
        }
        if (ValidateRecord(record, Kind())) {
            return std::vector<uint8_t>(record.begin(), record.end());
        }
        // Invalid records are diagnosed and discarded, never passed to the
        // protocol processor. Continue waiting for the next whole record.
    }
    return std::nullopt;
}

void SeqPacketTransport::Close()
{
    std::lock_guard<std::mutex> lock{_ioMutex};
    _closed = true;
    if (_socket != nullptr) {
        const SOCKET socketHandle = reinterpret_cast<SOCKET>(_socket);
        shutdown(socketHandle, SD_BOTH);
        closesocket(socketHandle);
        _socket = nullptr;
    }
    if (_winsockInitialized) {
        WSACleanup();
        _winsockInitialized = false;
    }
}

std::unique_ptr<AAPTransport> CreateSeqPacketTransport()
{
    return std::make_unique<SeqPacketTransport>();
}

MagicAAPTransport::MagicAAPTransport() = default;

MagicAAPTransport::~MagicAAPTransport()
{
    Close();
}

bool MagicAAPTransport::Connect(uint64_t address, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return false;
    }

    Close();
    std::lock_guard<std::mutex> lock{_clientMutex};
    {
        std::lock_guard<std::mutex> queueLock{_queueMutex};
        _closed = false;
        _eof = false;
        _records.clear();
    }

    _client = std::make_unique<MagicAAPWinRT::MagicAAPWinRTClient>();
    _client->SetOnDataReceived([this](const std::vector<uint8_t> &record) { Enqueue(record); });
    _client->SetOnDisconnected([this] { SignalEof(); });
    if (!_client->ConnectViaDeviceInterface(address)) {
        const std::wstring error = _client->GetLastError();
        spdlog::error(
            "[AAPTransport] MagicAAP device-interface connection failed: {}",
            std::string(error.begin(), error.end()));
        _client.reset();
        std::lock_guard<std::mutex> queueLock{_queueMutex};
        _closed = true;
        _eof = true;
        _queueCondition.notify_all();
        return false;
    }
    spdlog::info("[AAPTransport] Connected with MagicAAP device interface");
    return true;
}

bool MagicAAPTransport::Send(std::span<const uint8_t> data, std::stop_token stop)
{
    if (stop.stop_requested() || !ValidateRecord(data, Kind())) {
        return false;
    }

    std::lock_guard<std::mutex> lock{_clientMutex};
    if (!_client) {
        return false;
    }
    return _client->SendData(std::vector<uint8_t>(data.begin(), data.end()));
}

std::optional<std::vector<uint8_t>> MagicAAPTransport::Receive(std::stop_token stop)
{
    while (true) {
        std::unique_lock<std::mutex> lock{_queueMutex};
        const auto ready = [this] { return _closed || _eof || !_records.empty(); };
        // MSVC's condition_variable_any has no stop_token wait_for overload.
        // Poll in bounded intervals so cancellation never waits indefinitely.
        while (!_queueCondition.wait_for(lock, kReceivePollInterval, ready)) {
            if (stop.stop_requested()) {
                return std::nullopt;
            }
        }
        if (stop.stop_requested() || _closed || (_eof && _records.empty())) {
            return std::nullopt;
        }

        auto record = std::move(_records.front());
        _records.pop_front();
        lock.unlock();
        if (ValidateRecord(record, Kind())) {
            return record;
        }
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        // Discard malformed driver records and keep waiting for a valid one.
    }
}

void MagicAAPTransport::Enqueue(std::vector<uint8_t> record)
{
    std::lock_guard<std::mutex> lock{_queueMutex};
    if (_closed || _eof) {
        return;
    }
    _records.push_back(std::move(record));
    _queueCondition.notify_all();
}

void MagicAAPTransport::SignalEof()
{
    std::lock_guard<std::mutex> lock{_queueMutex};
    _eof = true;
    _queueCondition.notify_all();
}

void MagicAAPTransport::Close()
{
    std::unique_ptr<MagicAAPWinRT::MagicAAPWinRTClient> client;
    {
        std::lock_guard<std::mutex> lock{_clientMutex};
        client = std::move(_client);
        std::lock_guard<std::mutex> queueLock{_queueMutex};
        _closed = true;
        _eof = true;
        _records.clear();
        _queueCondition.notify_all();
    }
    // Join the existing client receiver before destroying this queue owner.
    if (client) {
        client->Disconnect();
    }
}

std::unique_ptr<AAPTransport> CreateMagicAAPTransport()
{
    return std::make_unique<MagicAAPTransport>();
}

} // namespace Core::AAP
