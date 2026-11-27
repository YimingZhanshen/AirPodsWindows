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

#pragma once

#include "MagicAAPDeviceBinding.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

class AAPTransportTests;

namespace Core::MagicAAPWinRT {
class MagicAAPWinRTClient;
} // namespace Core::MagicAAPWinRT

namespace Core::AAP {

enum class AAPTransportKind { SeqPacket, MagicAAP };

// Every successful send/receive is one complete AAP or ATT record. P1 supports
// no byte-stream transport, so a partial write fails rather than splitting a record.
class AAPTransport
{
public:
    virtual ~AAPTransport() = default;

    virtual bool Connect(uint64_t address, std::stop_token stop) = 0;
    virtual bool Send(std::span<const uint8_t> data, std::stop_token stop) = 0;
    virtual std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) = 0;
    virtual void Close() = 0;
    virtual AAPTransportKind Kind() const = 0;
    // Only MagicAAP produces a bind result. A default keeps injected and legacy
    // transports safe to query without downcasting.
    virtual Core::MagicAAPWinRT::MagicAAPBindStatus LastBindStatus() const
    {
        return Core::MagicAAPWinRT::MagicAAPBindStatus::TransportError;
    }
};

class SeqPacketTransport final : public AAPTransport
{
public:
    SeqPacketTransport();
    ~SeqPacketTransport() override;

    bool Connect(uint64_t address, std::stop_token stop) override;
    bool Send(std::span<const uint8_t> data, std::stop_token stop) override;
    std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override;
    void Close() override;
    AAPTransportKind Kind() const override
    {
        return AAPTransportKind::SeqPacket;
    }

private:
    // Receive and Send pin the socket state while using its handle. Close shuts
    // it down immediately, but the descriptor is physically closed only after
    // the last pin is released, so Windows cannot reuse its handle underneath
    // an active reader or writer.
    struct SocketState {
        explicit SocketState(void *socketHandle);
        ~SocketState();
        void Shutdown();

        void *handle;
        std::atomic<bool> closed{false};
    };

    std::shared_ptr<SocketState> _socket;
    std::shared_ptr<SocketState> _connectingSocket;
    std::mutex _ioMutex;
    bool _closed{false};
    bool _winsockInitialized{false};

    // Test-only observation at the actual receive poll point. Arrival at Receive() itself is
    // insufficient: cancellation or Close can win before a socket is pinned and polled.
    std::function<void()> _receivePollObserverForTest;

    friend class ::AAPTransportTests;
};

std::unique_ptr<AAPTransport> CreateSeqPacketTransport();

// P1 keeps the existing device-interface receiver thread. Its callback only
// enqueues complete ReadFile records; synchronous Receive owns the queue wait.
class MagicAAPTransport final : public AAPTransport
{
public:
    MagicAAPTransport();
    ~MagicAAPTransport() override;

    bool Connect(uint64_t address, std::stop_token stop) override;
    bool Send(std::span<const uint8_t> data, std::stop_token stop) override;
    std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override;
    void Close() override;
    AAPTransportKind Kind() const override
    {
        return AAPTransportKind::MagicAAP;
    }

    Core::MagicAAPWinRT::MagicAAPBindStatus LastBindStatus() const override
    {
        return _lastBindStatus.load();
    }

private:
    void Enqueue(std::vector<uint8_t> record);
    void SignalEof();

    mutable std::mutex _clientMutex;
    // Startup and Close retain independent snapshots. The driver operation runs
    // without _clientMutex, while the shared owner keeps the client alive until
    // both startup and receiver shutdown have finished.
    std::shared_ptr<Core::MagicAAPWinRT::MagicAAPWinRTClient> _client;
    std::atomic<Core::MagicAAPWinRT::MagicAAPBindStatus> _lastBindStatus{
        Core::MagicAAPWinRT::MagicAAPBindStatus::NoMatch};
    std::mutex _queueMutex;
    std::condition_variable_any _queueCondition;
    std::deque<std::vector<uint8_t>> _records;
    bool _closed{false};
    bool _eof{false};
};

std::unique_ptr<AAPTransport> CreateMagicAAPTransport();

} // namespace Core::AAP
