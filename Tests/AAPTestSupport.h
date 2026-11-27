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
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Source/Core/AAPCommandBroker.h"
#include "Source/Core/AAPConnectionStateMachine.h"
#include "Source/Core/AAPTransport.h"

// Deterministic, hardware-free test support for the AAP transport and worker seams.
//
// Rules this header upholds, because a concurrency test that hangs is worse than a
// missing one:
//   * Every wait is a bounded wait for a condition to *become* true. Nothing sleeps
//     to infer that a worker is parked, so no assertion depends on scheduling speed.
//   * Every parked call has a release path that runs on all exits, including a
//     QVERIFY early return: either the RAII guard, the transport destructor, or the
//     Worker destructor.
//   * Blocking transports park on a shared state object owned by a shared_ptr, so a
//     parked call never touches memory that the test already destroyed.
//   * Time comes from ManualClock, not the wall clock, wherever a test reasons about
//     backoff or deadlines.
// The remaining wall-clock use is a bounded rendezvous budget. A missed condition fails
// its assertion, and Worker's process watchdog catches a broken release before CTest's
// per-test TIMEOUT, which remains the outer safety net.

namespace AAPTestSupport {

// Single budget for every rendezvous. Generous enough that a loaded CI runner cannot
// trip it while a worker is merely slow, small enough that a genuine deadlock fails
// the test well before the 60 s CTest timeout.
inline constexpr std::chrono::milliseconds kRendezvousTimeout{10000};

// A Worker may be destroyed while its body is deadlocked, so its Join cannot be the only
// guard. This process-level backstop exits before CTest's 60 s ceiling. The watchdog is a
// joined jthread on the normal path; nothing is detached and ordinary test cleanup is intact.
inline constexpr std::chrono::milliseconds kWorkerFailFastTimeout{30000};
inline constexpr int kWorkerFailFastExitCode{121};

// A promise-backed blocking stub. Wait and Signal both have bounded deadlines; the RAII
// guard signals on every exit, including a QVERIFY early return. No predicate polling is
// involved, so an unsatisfied gate never depends on a wall-clock sleep to make progress.
class PromiseGate
{
public:
    struct State {
        size_t waiters{0};
        bool open{false};
    };

    PromiseGate() : _future{_promise.get_future().share()} {}

    bool Wait(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        {
            std::lock_guard lock{_mutex};
            ++_waiters;
        }
        _condition.notify_all();
        const bool ready = _future.wait_for(timeout) == std::future_status::ready;
        {
            std::lock_guard lock{_mutex};
            --_waiters;
        }
        _condition.notify_all();
        return ready;
    }

    void Signal()
    {
        std::call_once(_signalOnce, [this] {
            _promise.set_value();
            std::lock_guard lock{_mutex};
            _open = true;
        });
        _condition.notify_all();
    }

    // Observes the actual Wait entry state, not a signal emitted before Wait was called.
    bool WaitForWaiter(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(lock, timeout, [this] { return _waiters > 0; });
    }

    State Snapshot() const
    {
        std::lock_guard lock{_mutex};
        return {_waiters, _open};
    }

private:
    std::promise<void> _promise;
    std::shared_future<void> _future;
    std::once_flag _signalOnce;
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    size_t _waiters{0};
    bool _open{false};
};

// The atomic counterpart for callbacks that cannot own a promise. Wait blocks on a
// condition variable until Open publishes readiness; Reset arms the gate again.
class AtomicGate
{
public:
    struct State {
        size_t waiters{0};
        bool open{false};
    };

    explicit AtomicGate(bool initiallyOpen = false) : _open{initiallyOpen} {}

    bool Wait(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        ++_waiters;
        _condition.notify_all();
        const bool ready = _condition.wait_for(lock, timeout, [this] { return _open.load(); });
        --_waiters;
        _condition.notify_all();
        return ready;
    }

    void Open()
    {
        {
            std::lock_guard lock{_mutex};
            _open.store(true);
        }
        _condition.notify_all();
    }

    void Reset()
    {
        std::lock_guard lock{_mutex};
        _open.store(false);
    }

    // Entry and the open predicate are sampled under the same mutex used by Wait.
    bool WaitForWaiter(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(lock, timeout, [this] { return _waiters > 0; });
    }

    State Snapshot() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return {_waiters, _open.load()};
    }

private:
    std::atomic<bool> _open;
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    size_t _waiters{0};
};

// Opens either kind of gate on scope exit. Release is idempotent, so composing it with a
// Worker's own release callback cannot double-signal or throw.
class ScopedUnblock
{
public:
    explicit ScopedUnblock(PromiseGate &gate) : _release([&gate] { gate.Signal(); }) {}
    explicit ScopedUnblock(AtomicGate &gate) : _release([&gate] { gate.Open(); }) {}

    ScopedUnblock(const ScopedUnblock &) = delete;
    ScopedUnblock &operator=(const ScopedUnblock &) = delete;

    ~ScopedUnblock()
    {
        _release();
    }

private:
    std::function<void()> _release;
};

// Wall-clock-free time source. AAPConnectionStateMachine receives its time point from
// the caller, so tests hand it a counter they advance themselves and backoff arithmetic
// becomes exact and reproducible. The zero epoch is deliberate: no test may compare a
// harness time against a real timestamp, because then only the differences mean anything.
class ManualClock
{
public:
    using Duration = std::chrono::steady_clock::duration;
    using TimePoint = Core::AAP::AAPConnectionStateMachine::TimePoint;

    ManualClock() = default;
    explicit ManualClock(Duration start) : _now(start) {}

    TimePoint Now() const
    {
        std::lock_guard lock{_mutex};
        return _now;
    }
    void Advance(Duration by)
    {
        std::lock_guard lock{_mutex};
        _now += by;
    }
    // Advances and returns the new time, for the common `now = tick(1s)` idiom.
    TimePoint Tick(Duration by)
    {
        std::lock_guard lock{_mutex};
        _now += by;
        return _now;
    }

private:
    mutable std::mutex _mutex;
    TimePoint _now{Duration{0}};
};

// Runs a blocking transport call off the test thread and guarantees "release, then
// join" on every exit path, including a QVERIFY early return. The release callback
// wakes whatever the body parked on; if that release is broken, the process watchdog
// terminates the test instead of leaving the destructor blocked in an unbounded join.
class Worker
{
public:
    using Body = std::function<void()>;
    using Release = std::function<void()>;

    explicit Worker(Body body, Release release = {})
        : _body(std::move(body)), _release(std::move(release))
    {
        _watchdog = std::jthread{[this](std::stop_token stop) {
            std::unique_lock lock{_watchdogMutex};
            const bool disarmed = _watchdogCondition.wait_for(
                lock, stop, kWorkerFailFastTimeout, [this] { return !_watchdogArmed; });
            if (disarmed) {
                return;
            }
            lock.unlock();
            std::fputs("AAP test worker exceeded 30 s; terminating the test process.\n", stderr);
            std::fflush(stderr);
            std::quick_exit(kWorkerFailFastExitCode);
        }};
        _thread = std::thread{[this] {
            _body();
            DisarmWatchdog();
            std::lock_guard lock{_mutex};
            _finished = true;
            _finishedCondition.notify_all();
        }};
    }

    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    ~Worker()
    {
        Join();
    }

    // Whether the body finished within the budget. False means "not yet", never "proved
    // stuck": the caller releases through the guard and asserts the observable outcome.
    bool Finished(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _finishedCondition.wait_for(lock, timeout, [this] { return _finished; });
    }

    void Join()
    {
        if (_release) {
            _release();
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        DisarmWatchdog();
    }

private:
    void DisarmWatchdog()
    {
        {
            std::lock_guard lock{_watchdogMutex};
            _watchdogArmed = false;
        }
        _watchdogCondition.notify_all();
    }

    Body _body;
    Release _release;
    // These synchronization members outlive _watchdog during member destruction.
    std::mutex _watchdogMutex;
    std::condition_variable_any _watchdogCondition;
    bool _watchdogArmed{true};
    std::jthread _watchdog;
    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _finishedCondition;
    bool _finished{false};
};

// A scripted AAP transport for the message-boundary contract: whole records in,
// whole records out. Injection is per record, so "fragmented" means the peer handed
// over a record that is itself short - the transport layer never reassembles bytes.
//
// Send limits model a failed atomic write, not a byte stream: a caller must never
// retry the unwritten suffix as a second record.
class FakeTransport : public Core::AAP::AAPTransport
{
public:
    bool Connect(uint64_t address, std::stop_token stop) override
    {
        auto state = _state; // keep the state alive if the test destroys us mid-wait
        std::unique_lock lock{state->mutex};
        // A fresh Connect starts a new session after Close; a Close during this call still wins.
        state->closed = false;
        ++state->connectWaiters;
        state->condition.notify_all();
        state->condition.wait(
            lock, stop, [state] { return !state->blockConnect || state->closed; });
        --state->connectWaiters;
        if (stop.stop_requested() || state->closed || !state->connectResult) {
            return false;
        }
        ++state->connectCalls;
        state->lastConnectAddress = address;
        state->connected = true;
        state->eof = false;
        state->records.clear();
        return true;
    }

    bool Send(std::span<const uint8_t> data, std::stop_token stop) override
    {
        auto state = _state;
        {
            std::unique_lock lock{state->mutex};
            ++state->sendAttempts;
            ++state->sendWaiters;
            state->condition.notify_all();
            state->condition.wait(
                lock, stop, [state] { return !state->blockSend || state->closed; });
            --state->sendWaiters;
            if (stop.stop_requested() || state->closed || !state->connected || state->sendError) {
                return false;
            }

            ++state->sendCalls;
            state->lastSend.assign(data.begin(), data.end());
            if (state->sendFailures > 0) {
                --state->sendFailures;
                return false;
            }
            const size_t written = state->sendLimit.has_value()
                                       ? std::min(data.size(), *state->sendLimit)
                                       : data.size();
            if (written != data.size()) {
                return false;
            }
            state->sentRecords.push_back(std::vector<uint8_t>(data.begin(), data.end()));
            state->condition.notify_all();
        }
        return true;
    }

    std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override
    {
        auto state = _state;
        std::unique_lock lock{state->mutex};
        ++state->receiveWaiters;
        state->condition.notify_all();
        state->condition.wait(lock, stop, [state] {
            if (state->closed) {
                return true;
            }
            if (state->blockReceive) {
                return false;
            }
            return state->eof || !state->records.empty();
        });
        --state->receiveWaiters;
        if (stop.stop_requested() || state->closed || state->records.empty()) {
            return std::nullopt;
        }

        auto record = std::move(state->records.front());
        state->records.pop_front();
        return record;
    }

    void Close() override
    {
        ReleaseAll();
    }

    Core::AAP::AAPTransportKind Kind() const override
    {
        std::lock_guard lock{_state->mutex};
        return _state->kind;
    }

    // Tears the session down and wakes every parked caller. This is what the RAII guard
    // and the destructor call, so a blocked worker always has a release path: a parked
    // Receive/Send/Connect returns as if the transport had just closed. Releasing is
    // idempotent, which is what makes "guard, then join" deadlock-free.
    void ReleaseAll()
    {
        auto state = _state;
        std::lock_guard lock{state->mutex};
        state->closed = true;
        state->connected = false;
        state->blockConnect = false;
        state->blockSend = false;
        state->blockReceive = false;
        state->records.clear();
        state->condition.notify_all();
    }

    ~FakeTransport() override
    {
        ReleaseAll();
    }

    void Push(std::vector<uint8_t> record)
    {
        std::lock_guard lock{_state->mutex};
        _state->records.push_back(std::move(record));
        _state->condition.notify_all();
    }

    // Pushes several records at once so a test can prove one Receive call yields
    // exactly one record even when several are queued back to back.
    void PushAll(std::vector<std::vector<uint8_t>> records)
    {
        std::lock_guard lock{_state->mutex};
        for (auto &record : records) {
            _state->records.push_back(std::move(record));
        }
        _state->condition.notify_all();
    }

    void SetEof()
    {
        std::lock_guard lock{_state->mutex};
        _state->eof = true;
        _state->condition.notify_all();
    }

    void SetSendLimit(std::optional<size_t> limit)
    {
        std::lock_guard lock{_state->mutex};
        _state->sendLimit = limit;
    }

    void SetSendError(bool error)
    {
        std::lock_guard lock{_state->mutex};
        _state->sendError = error;
    }

    void SetSendFailures(size_t failures)
    {
        std::lock_guard lock{_state->mutex};
        _state->sendFailures = failures;
    }

    void SetBlockSend(bool block)
    {
        std::lock_guard lock{_state->mutex};
        _state->blockSend = block;
        _state->condition.notify_all();
    }

    void SetBlockReceive(bool block)
    {
        std::lock_guard lock{_state->mutex};
        _state->blockReceive = block;
        _state->condition.notify_all();
    }

    void SetBlockConnect(bool block)
    {
        std::lock_guard lock{_state->mutex};
        _state->blockConnect = block;
        _state->condition.notify_all();
    }

    void SetConnectResult(bool result)
    {
        std::lock_guard lock{_state->mutex};
        _state->connectResult = result;
    }

    void SetKind(Core::AAP::AAPTransportKind kind)
    {
        std::lock_guard lock{_state->mutex};
        _state->kind = kind;
    }

    size_t SendCalls() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->sendCalls;
    }

    bool WaitForRecordCount(size_t count, std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        auto state = _state;
        std::unique_lock lock{state->mutex};
        return state->condition.wait_for(
            lock, timeout, [state, count] { return state->sentRecords.size() >= count; });
    }

    bool WaitForSendAttempts(size_t count, std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        auto state = _state;
        std::unique_lock lock{state->mutex};
        return state->condition.wait_for(
            lock, timeout, [state, count] { return state->sendAttempts >= count; });
    }

    size_t SendAttempts() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->sendAttempts;
    }

    size_t ConnectCalls() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->connectCalls;
    }

    uint64_t LastConnectAddress() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->lastConnectAddress;
    }

    std::vector<uint8_t> LastSend() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->lastSend;
    }

    std::vector<std::vector<uint8_t>> SentRecords() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->sentRecords;
    }

    size_t RecordCount() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->records.size();
    }

    bool IsClosed() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->closed;
    }

    bool WaitForReceiveWaiter(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        return WaitForArrival(_state->receiveWaiters, timeout);
    }

    bool WaitForSendWaiter(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        return WaitForArrival(_state->sendWaiters, timeout);
    }

    bool WaitForConnectWaiter(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        return WaitForArrival(_state->connectWaiters, timeout);
    }

private:
    // Parked waiters hold this by value, which is what makes the destructor safe.
    struct State {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::deque<std::vector<uint8_t>> records;
        std::vector<std::vector<uint8_t>> sentRecords;
        std::vector<uint8_t> lastSend;
        std::optional<size_t> sendLimit;
        size_t sendCalls{0};
        size_t sendAttempts{0};
        size_t sendFailures{0};
        size_t connectCalls{0};
        size_t receiveWaiters{0};
        size_t sendWaiters{0};
        size_t connectWaiters{0};
        uint64_t lastConnectAddress{0};
        Core::AAP::AAPTransportKind kind{Core::AAP::AAPTransportKind::SeqPacket};
        bool connected{false};
        bool closed{false};
        bool eof{false};
        bool sendError{false};
        bool blockSend{false};
        bool blockReceive{false};
        bool blockConnect{false};
        bool connectResult{true};
    };

    // Bounded wait for a caller to reach one of the transport entry points. The
    // transport's own condition protects the counter, so this observes arrival
    // instead of guessing it from a sleep.
    bool WaitForArrival(const size_t &counter, std::chrono::milliseconds timeout)
    {
        auto state = _state;
        std::unique_lock lock{state->mutex};
        return state->condition.wait_for(lock, timeout, [state, &counter] { return counter > 0; });
    }

    std::shared_ptr<State> _state{std::make_shared<State>()};
};

// Parks an operation for the lifetime of the guard. The destructor always releases,
// so a QVERIFY early return cannot leave a worker blocked.
class ScopedRelease
{
public:
    template <typename Transport>
    explicit ScopedRelease(Transport &transport)
        : _release([&transport] { transport.ReleaseAll(); })
    {
    }

    ScopedRelease(const ScopedRelease &) = delete;
    ScopedRelease &operator=(const ScopedRelease &) = delete;

    ~ScopedRelease()
    {
        _release();
    }

private:
    std::function<void()> _release;
};

// Operation-level stub for the Manager/worker seams: each entry point can be parked
// independently and reports arrival before it waits, so a test can prove a worker is
// inside Connect/Send/Receive without timing it. Close is deliberately non-blocking
// and releases every parked caller, matching what SeqPacketTransport::Close does to a
// socket blocked in select/recv.
class BlockingTransport : public Core::AAP::AAPTransport
{
public:
    enum class Op { Connect, Send, Receive, Any };

    bool Connect(uint64_t address, std::stop_token stop) override
    {
        return Park(Op::Connect, stop, [this, address] {
            ++_state->connectCalls;
            _state->lastConnectAddress = address;
            return _state->connectResult;
        });
    }

    bool Send(std::span<const uint8_t> data, std::stop_token stop) override
    {
        return Park(Op::Send, stop, [this, data] {
            ++_state->sendCalls;
            _state->lastSend.assign(data.begin(), data.end());
            _state->sentRecords.push_back(std::vector<uint8_t>(data.begin(), data.end()));
            return _state->sendResult;
        });
    }

    std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override
    {
        std::optional<std::vector<uint8_t>> result;
        Park(Op::Receive, stop, [this, &result] {
            ++_state->receiveCalls;
            if (_state->records.empty()) {
                result = std::nullopt;
                return true;
            }
            result = std::move(_state->records.front());
            _state->records.pop_front();
            return true;
        });
        return result;
    }

    void Close() override
    {
        std::lock_guard lock{_state->mutex};
        ++_state->closeCalls;
        _state->closed = true;
        _state->connected = false;
        ReleaseLocked();
    }

    Core::AAP::AAPTransportKind Kind() const override
    {
        std::lock_guard lock{_state->mutex};
        return _state->kind;
    }

    void ReleaseAll()
    {
        std::lock_guard lock{_state->mutex};
        ReleaseLocked();
    }

    ~BlockingTransport() override
    {
        ReleaseAll();
    }

    void Block(Op op)
    {
        std::lock_guard lock{_state->mutex};
        SetBlockedLocked(op, true);
    }

    void Unblock(Op op)
    {
        std::lock_guard lock{_state->mutex};
        SetBlockedLocked(op, false);
        _state->condition.notify_all();
    }

    bool WaitForParked(Op op, std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_state->mutex};
        return _state->condition.wait_for(
            lock, timeout, [this, op] { return ParkedCountLocked(op) > 0; });
    }

    bool
    WaitForCalls(Op op, size_t expected, std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_state->mutex};
        return _state->condition.wait_for(
            lock, timeout, [this, op, expected] { return CallCountLocked(op) >= expected; });
    }

    size_t CallCount(Op op) const
    {
        std::lock_guard lock{_state->mutex};
        return CallCountLocked(op);
    }

    size_t CloseCalls() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->closeCalls;
    }

    bool IsClosed() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->closed;
    }

    uint64_t LastConnectAddress() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->lastConnectAddress;
    }

    std::vector<uint8_t> LastSend() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->lastSend;
    }

    // The whole send sequence, so a suite can assert the handshake order the Manager is
    // supposed to emit rather than only the last packet it wrote.
    std::vector<std::vector<uint8_t>> SentRecords() const
    {
        std::lock_guard lock{_state->mutex};
        return _state->sentRecords;
    }

    void SetConnectResult(bool result)
    {
        std::lock_guard lock{_state->mutex};
        _state->connectResult = result;
    }

    void SetSendResult(bool result)
    {
        std::lock_guard lock{_state->mutex};
        _state->sendResult = result;
    }

    void SetKind(Core::AAP::AAPTransportKind kind)
    {
        std::lock_guard lock{_state->mutex};
        _state->kind = kind;
    }

    void QueueRecord(std::vector<uint8_t> record)
    {
        std::lock_guard lock{_state->mutex};
        _state->records.push_back(std::move(record));
        _state->condition.notify_all();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable_any condition;
        std::deque<std::vector<uint8_t>> records;
        std::vector<std::vector<uint8_t>> sentRecords;
        std::vector<uint8_t> lastSend;
        size_t connectCalls{0};
        size_t sendCalls{0};
        size_t receiveCalls{0};
        size_t closeCalls{0};
        size_t parkedConnect{0};
        size_t parkedSend{0};
        size_t parkedReceive{0};
        uint64_t lastConnectAddress{0};
        Core::AAP::AAPTransportKind kind{Core::AAP::AAPTransportKind::SeqPacket};
        bool connected{false};
        bool closed{false};
        bool blockConnect{false};
        bool blockSend{false};
        bool blockReceive{false};
        bool connectResult{true};
        bool sendResult{true};
    };

    // Returns the value `onReleased` produced, or false when the call was cancelled
    // or the transport closed while parked.
    template <typename OnReleased>
    bool Park(Op op, std::stop_token stop, OnReleased &&onReleased)
    {
        auto state = _state;
        std::unique_lock lock{state->mutex};
        ++ParkedCounterLocked(op);
        state->condition.notify_all();
        state->condition.wait(lock, stop, [state, op] {
            if (state->closed) {
                return true;
            }
            switch (op) {
            case Op::Connect:
                return !state->blockConnect;
            case Op::Send:
                return !state->blockSend;
            case Op::Receive:
                // An idle blocked reader stays parked, but already queued records remain
                // deliverable so a test need not race Unblock against the next poll.
                return !state->blockReceive || !state->records.empty();
            case Op::Any:
                return false;
            }
            return false;
        });
        --ParkedCounterLocked(op);
        if (stop.stop_requested() || state->closed) {
            return false;
        }

        // `onReleased` runs with the mutex held so a test can observe the call count
        // and the parked count without a second rendezvous.
        const bool value = onReleased();
        if (op == Op::Connect) {
            state->connected = value;
        }
        state->condition.notify_all();
        return value;
    }

    bool IsBlockedLocked(Op op) const
    {
        switch (op) {
        case Op::Connect:
            return _state->blockConnect;
        case Op::Send:
            return _state->blockSend;
        case Op::Receive:
            return _state->blockReceive;
        case Op::Any:
            return false;
        }
        return false;
    }

    void SetBlockedLocked(Op op, bool blocked)
    {
        switch (op) {
        case Op::Connect:
            _state->blockConnect = blocked;
            break;
        case Op::Send:
            _state->blockSend = blocked;
            break;
        case Op::Receive:
            _state->blockReceive = blocked;
            break;
        case Op::Any:
            _state->blockConnect = blocked;
            _state->blockSend = blocked;
            _state->blockReceive = blocked;
            break;
        }
    }

    void ReleaseLocked()
    {
        _state->blockConnect = false;
        _state->blockSend = false;
        _state->blockReceive = false;
        _state->condition.notify_all();
    }

    size_t ParkedCountLocked(Op op) const
    {
        switch (op) {
        case Op::Connect:
            return _state->parkedConnect;
        case Op::Send:
            return _state->parkedSend;
        case Op::Receive:
            return _state->parkedReceive;
        case Op::Any:
            return _state->parkedConnect + _state->parkedSend + _state->parkedReceive;
        }
        return 0;
    }

    size_t CallCountLocked(Op op) const
    {
        switch (op) {
        case Op::Connect:
            return _state->connectCalls;
        case Op::Send:
            return _state->sendCalls;
        case Op::Receive:
            return _state->receiveCalls;
        case Op::Any:
            return _state->connectCalls + _state->sendCalls + _state->receiveCalls;
        }
        return 0;
    }

    size_t &ParkedCounterLocked(Op op)
    {
        switch (op) {
        case Op::Connect:
            return _state->parkedConnect;
        case Op::Send:
            return _state->parkedSend;
        case Op::Receive:
            return _state->parkedReceive;
        case Op::Any:
            return _state->parkedReceive;
        }
        return _state->parkedReceive;
    }

    std::shared_ptr<State> _state{std::make_shared<State>()};
};

// Publishes "the worker has reached the transport call" before that call happens, so a
// suite can observe a hand-off instead of inferring one from elapsed time. Only needed for
// the production transports, which expose no waiter counter; the transports above publish
// arrival themselves.
class EntryRendezvous
{
public:
    struct State {
        bool entered{false};
        bool completed{false};
    };

    void Arrive()
    {
        std::lock_guard lock{_mutex};
        _entered = true;
        _condition.notify_all();
    }

    void Complete()
    {
        std::lock_guard lock{_mutex};
        _completed = true;
        _condition.notify_all();
    }

    bool Wait(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(lock, timeout, [this] { return _entered; });
    }

    bool WaitForCompletion(std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(lock, timeout, [this] { return _entered && _completed; });
    }

    // Entry and completion belong to the same observation; never combine two separately
    // locked getters to infer whether a call is still parked.
    State Snapshot() const
    {
        std::lock_guard lock{_mutex};
        return {_entered, _completed};
    }

    void Reset()
    {
        std::lock_guard lock{_mutex};
        _entered = false;
        _completed = false;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    bool _entered{false};
    bool _completed{false};
};

// Builds and records the transports a Manager seam hands out, so a suite can inspect a
// session after Connect() returned. Ownership is shared because the Manager takes the
// transport into its reader; the factory keeps a reference for assertions. An optional
// per-build hook arms the next attempt, which is how a test parks the *next* Connect or
// the *next* reader without racing the thread that consumes it.
template <typename Transport>
class TransportFactory
{
public:
    using Hook = std::function<void(Transport &)>;

    explicit TransportFactory(Hook hook = {}) : _hook(std::move(hook)) {}

    // Matches the Manager's private seam signature.
    std::shared_ptr<Core::AAP::AAPTransport> operator()()
    {
        return Build();
    }

    std::shared_ptr<Transport> Build()
    {
        auto transport = std::make_shared<Transport>();
        std::lock_guard lock{_mutex};
        if (_hook) {
            _hook(*transport);
        }
        _built.push_back(transport);
        _condition.notify_all();
        return transport;
    }

    bool WaitForCount(size_t expected, std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(
            lock, timeout, [this, expected] { return _built.size() >= expected; });
    }

    std::shared_ptr<Transport> Last() const
    {
        std::lock_guard lock{_mutex};
        return _built.empty() ? nullptr : _built.back();
    }

    std::vector<std::shared_ptr<Transport>> All() const
    {
        std::lock_guard lock{_mutex};
        return _built;
    }

    size_t Count() const
    {
        std::lock_guard lock{_mutex};
        return _built.size();
    }

    void SetHook(Hook hook)
    {
        std::lock_guard lock{_mutex};
        _hook = std::move(hook);
    }

    // Safety net for an early test return: every transport this seam handed out is
    // released, so nothing left parked can outlive the test. Releasing an already closed
    // transport is a no-op, and Manager::Disconnect closes the published session itself.
    void ReleaseAllBuilt() const
    {
        std::lock_guard lock{_mutex};
        for (const auto &transport : _built) {
            transport->ReleaseAll();
        }
    }

    ~TransportFactory()
    {
        ReleaseAllBuilt();
    }

private:
    TransportFactory(const TransportFactory &) = delete;
    TransportFactory &operator=(const TransportFactory &) = delete;

    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::vector<std::shared_ptr<Transport>> _built;
    Hook _hook;
};

// Captures worker events together with the generation that produced them, and lets a
// test wait for an arrival with a deadline. Recording the attempt, not just a counter,
// is what makes "old generation callbacks are discarded" assertable: a stale event that
// leaks through has a generation the test can point at.
class CallbackRecorder
{
public:
    void Record(std::string_view event, Core::AAP::AAPAttempt attempt)
    {
        std::lock_guard lock{_mutex};
        _events.emplace_back(std::string{event}, attempt);
        _condition.notify_all();
    }

    size_t Count(std::string_view event) const
    {
        std::lock_guard lock{_mutex};
        return CountLocked(event);
    }

    bool WaitForCount(
        std::string_view event, size_t expected,
        std::chrono::milliseconds timeout = kRendezvousTimeout)
    {
        std::unique_lock lock{_mutex};
        return _condition.wait_for(
            lock, timeout, [this, event, expected] { return CountLocked(event) >= expected; });
    }

    std::optional<Core::AAP::AAPAttempt> LastAttempt(std::string_view event) const
    {
        std::lock_guard lock{_mutex};
        std::optional<Core::AAP::AAPAttempt> result;
        for (const auto &[name, attempt] : _events) {
            if (name == event) {
                result = attempt;
            }
        }
        return result;
    }

    std::vector<std::pair<std::string, Core::AAP::AAPAttempt>> Events() const
    {
        std::lock_guard lock{_mutex};
        return _events;
    }

    void Clear()
    {
        std::lock_guard lock{_mutex};
        _events.clear();
    }

private:
    size_t CountLocked(std::string_view event) const
    {
        size_t count = 0;
        for (const auto &[name, attempt] : _events) {
            (void)attempt;
            count += name == event ? 1u : 0u;
        }
        return count;
    }

    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::vector<std::pair<std::string, Core::AAP::AAPAttempt>> _events;
};

// Well-formed records reused across the transport and Manager suites. Keeping them here
// stops a formatting change in one test from silently weakening another.
namespace Records {

inline std::vector<uint8_t> HandshakeAck()
{
    return {0x01, 0x00, 0x04, 0x00};
}

inline std::vector<uint8_t> FeatureAck()
{
    return {0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
}

inline std::vector<uint8_t> AttReadRequest()
{
    return {0x0A, 0x1B, 0x00};
}

inline std::vector<uint8_t> AttReadResponse(std::size_t valueBytes)
{
    std::vector<uint8_t> record(2 + valueBytes, 0x00);
    record[0] = 0x0B;
    return record;
}

inline std::vector<uint8_t> AttTruncatedWriteCommand()
{
    return {0x12, 0x1B, 0x00};
}

// Noise-control notification (opcode 0x0009, sub-opcode 0x0D): the shortest record
// the validator accepts and the parser answers for.
inline std::vector<uint8_t> NoiseControl(std::uint8_t value)
{
    return {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, value, 0x00, 0x00, 0x00};
}

inline std::vector<uint8_t> Fragment(std::size_t bytes)
{
    std::vector<uint8_t> record(bytes, 0x00);
    if (!record.empty()) {
        record[0] = 0x04;
    }
    return record;
}

} // namespace Records

} // namespace AAPTestSupport
