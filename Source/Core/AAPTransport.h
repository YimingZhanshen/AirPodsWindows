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

#include <cstdint>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

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
    void *_socket{nullptr};
    std::mutex _ioMutex;
    bool _closed{false};
    bool _winsockInitialized{false};
};

std::unique_ptr<AAPTransport> CreateSeqPacketTransport();

namespace MagicAAPWinRT {
class MagicAAPWinRTClient;
}

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

private:
    void Enqueue(std::vector<uint8_t> record);
    void SignalEof();

    mutable std::mutex _clientMutex;
    std::unique_ptr<MagicAAPWinRT::MagicAAPWinRTClient> _client;
    std::mutex _queueMutex;
    std::condition_variable_any _queueCondition;
    std::deque<std::vector<uint8_t>> _records;
    bool _closed{false};
    bool _eof{false};
};

std::unique_ptr<AAPTransport> CreateMagicAAPTransport();

} // namespace Core::AAP
