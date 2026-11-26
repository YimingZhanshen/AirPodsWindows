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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

#include "Source/Core/AAPTransport.h"

namespace AAPTestSupport {

// Records are injected whole. The send limit models a failed atomic write, not a
// byte stream: callers must never retry the unwritten suffix as another record.
class FakeTransport final : public Core::AAP::AAPTransport
{
public:
    bool Connect(uint64_t, std::stop_token stop) override
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (stop.stop_requested()) {
            return false;
        }
        _closed = false;
        _eof = false;
        _records.clear();
        _sendHook = {};
        _connected = true;
        return true;
    }

    bool Send(std::span<const uint8_t> data, std::stop_token stop) override
    {
        bool accepted = false;
        std::function<void()> hook;
        {
            std::unique_lock<std::mutex> lock{_mutex};
            ++_sendWaiters;
            _sendCondition.notify_all();
            _sendCondition.wait(lock, stop, [this] { return !_blockSend || _closed; });
            --_sendWaiters;
            if (stop.stop_requested() || _closed || !_connected || _sendError) {
                hook = _sendHook;
            }
            else {
                ++_sendCalls;
                _lastSend.assign(data.begin(), data.end());
                if (_sendFailure.has_value() && *_sendFailure + 1 == _sendCalls) {
                    // Exactly one record of the sequence is rejected, which is what a handshake
                    // sequence test needs: the calls around it still succeed.
                    accepted = false;
                }
                else {
                    const size_t written =
                        _sendLimit.has_value() ? std::min(data.size(), *_sendLimit) : data.size();
                    accepted = written == data.size();
                    if (accepted) {
                        _sentRecords.push_back(std::vector<uint8_t>(data.begin(), data.end()));
                    }
                }
                hook = _sendHook;
            }
        }
        // Run outside the transport mutex so an immediate-response hook can call Manager.
        if (hook) {
            hook();
        }
        return accepted;
    }

    std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override
    {
        std::unique_lock<std::mutex> lock{_mutex};
        ++_receiveWaiters;
        _receiveCondition.notify_all();
        _receiveCondition.wait(lock, stop, [this] { return _closed || _eof || !_records.empty(); });
        --_receiveWaiters;
        if (stop.stop_requested() || _closed || _records.empty()) {
            return std::nullopt;
        }

        auto record = std::move(_records.front());
        _records.pop_front();
        return record;
    }

    void Close() override
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _closed = true;
        _connected = false;
        _records.clear();
        _receiveCondition.notify_all();
        _sendCondition.notify_all();
    }

    Core::AAP::AAPTransportKind Kind() const override
    {
        return Core::AAP::AAPTransportKind::SeqPacket;
    }

    void Push(std::vector<uint8_t> record)
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _records.push_back(std::move(record));
        _receiveCondition.notify_all();
    }

    void SetEof()
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _eof = true;
        _receiveCondition.notify_all();
    }

    void SetSendLimit(std::optional<size_t> limit)
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _sendLimit = limit;
    }

    void SetSendError(bool error)
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _sendError = error;
    }

    // Rejects exactly one record of the sequence: the (index+1)-th Send call, 0-based.
    // Letting a test fail a single write is what makes the handshake sequence observable —
    // a global send error would stop the sequence at its first packet.
    void FailNthSend(size_t index)
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _sendFailure = index;
    }

    void SetBlockSend(bool block)
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _blockSend = block;
        _sendCondition.notify_all();
    }

    // Executes after the send result is known but before Send returns, without holding the
    // transport mutex. Tests use this to model a response that overtakes the sender.
    void SetSendHook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _sendHook = std::move(hook);
    }

    size_t SendCalls() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _sendCalls;
    }

    std::vector<std::vector<uint8_t>> SentRecords() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _sentRecords;
    }

    std::vector<uint8_t> LastSend() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _lastSend;
    }

    // Every record the production path managed to put on the wire, in order. Used to
    // prove a session never emits a second ATT read while one is still outstanding.
    std::vector<std::vector<uint8_t>> SentRecords() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _sentRecords;
    }

    size_t RecordCount() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _records.size();
    }

    bool WaitForReceiveWaiter(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock{_mutex};
        return _receiveCondition.wait_for(lock, timeout, [this] { return _receiveWaiters > 0; });
    }

    bool WaitForSendWaiter(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock{_mutex};
        return _sendCondition.wait_for(lock, timeout, [this] { return _sendWaiters > 0; });
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable_any _receiveCondition;
    std::condition_variable_any _sendCondition;
    std::deque<std::vector<uint8_t>> _records;
    std::vector<std::vector<uint8_t>> _sentRecords;
    std::vector<uint8_t> _lastSend;
    std::optional<size_t> _sendLimit;
    std::optional<size_t> _sendFailure;
    size_t _sendCalls{0};
    size_t _receiveWaiters{0};
    size_t _sendWaiters{0};
    bool _connected{false};
    bool _closed{false};
    bool _eof{false};
    bool _sendError{false};
    bool _blockSend{false};
    std::function<void()> _sendHook;
};

} // namespace AAPTestSupport
