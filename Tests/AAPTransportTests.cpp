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

#include <chrono>
#include <condition_variable>
#include <future>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(APD_OS_WIN)
    #include <WinSock2.h>
    #include <ws2tcpip.h>
#endif

#include <QtTest>

#include "AAPTestSupport.h"
#include "Source/Core/AAPMessage.h"
#include "Source/Core/AAPTransport.h"
#include "Source/Core/ConnectionDiagnostics.h"

#if defined(APD_OS_WIN)
    #include "Source/Core/AAPManager.h"
    #pragma comment(lib, "Ws2_32.lib")
#endif

using namespace std::chrono_literals;

#if defined(APD_OS_WIN)
// Connected UDP loopback sockets exercise the production select/recv/send path
// without Bluetooth hardware. Datagrams retain the same whole-record boundaries
// required from SOCK_SEQPACKET.
class LoopbackDatagramPair
{
public:
    LoopbackDatagramPair()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return;
        }
        _initialized = true;
        _transportSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        _peerSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_transportSocket == INVALID_SOCKET || _peerSocket == INVALID_SOCKET) {
            CleanupSockets();
            return;
        }

        sockaddr_in loopback{};
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(_transportSocket, reinterpret_cast<sockaddr *>(&loopback), sizeof(loopback)) ==
                SOCKET_ERROR ||
            bind(_peerSocket, reinterpret_cast<sockaddr *>(&loopback), sizeof(loopback)) ==
                SOCKET_ERROR)
        {
            CleanupSockets();
            return;
        }

        sockaddr_in transportAddress{};
        sockaddr_in peerAddress{};
        int transportSize = sizeof(transportAddress);
        int peerSize = sizeof(peerAddress);
        if (getsockname(
                _transportSocket, reinterpret_cast<sockaddr *>(&transportAddress),
                &transportSize) == SOCKET_ERROR ||
            getsockname(_peerSocket, reinterpret_cast<sockaddr *>(&peerAddress), &peerSize) ==
                SOCKET_ERROR ||
            connect(_transportSocket, reinterpret_cast<sockaddr *>(&peerAddress), peerSize) ==
                SOCKET_ERROR ||
            connect(_peerSocket, reinterpret_cast<sockaddr *>(&transportAddress), transportSize) ==
                SOCKET_ERROR)
        {
            CleanupSockets();
            return;
        }

        DWORD timeout = 1000;
        setsockopt(
            _peerSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
            sizeof(timeout));
    }

    ~LoopbackDatagramPair()
    {
        CleanupSockets();
        if (_initialized) {
            WSACleanup();
        }
    }

    LoopbackDatagramPair(const LoopbackDatagramPair &) = delete;
    LoopbackDatagramPair &operator=(const LoopbackDatagramPair &) = delete;

    bool IsValid() const
    {
        return _transportSocket != INVALID_SOCKET && _peerSocket != INVALID_SOCKET;
    }

    SOCKET ReleaseTransportSocket()
    {
        const SOCKET result = _transportSocket;
        _transportSocket = INVALID_SOCKET;
        return result;
    }

    bool SendFromPeer(const std::vector<uint8_t> &record)
    {
        return send(
                   _peerSocket, reinterpret_cast<const char *>(record.data()),
                   static_cast<int>(record.size()), 0) == static_cast<int>(record.size());
    }

    std::optional<std::vector<uint8_t>> ReceiveOnPeer()
    {
        std::vector<uint8_t> record(4097);
        const int received = recv(
            _peerSocket, reinterpret_cast<char *>(record.data()), static_cast<int>(record.size()),
            0);
        if (received == SOCKET_ERROR) {
            return std::nullopt;
        }
        record.resize(static_cast<size_t>(received));
        return record;
    }

private:
    void CleanupSockets()
    {
        if (_transportSocket != INVALID_SOCKET) {
            closesocket(_transportSocket);
            _transportSocket = INVALID_SOCKET;
        }
        if (_peerSocket != INVALID_SOCKET) {
            closesocket(_peerSocket);
            _peerSocket = INVALID_SOCKET;
        }
    }

    SOCKET _transportSocket{INVALID_SOCKET};
    SOCKET _peerSocket{INVALID_SOCKET};
    bool _initialized{false};
};
#endif

#if defined(APD_OS_WIN)
class UnavailableSeqPacketTransport final : public AAPTestSupport::FakeTransport
{
public:
    bool Connect(uint64_t, std::stop_token) override
    {
        Close();
        return false;
    }
};

class BindStatusTransport final : public AAPTestSupport::FakeTransport
{
public:
    explicit BindStatusTransport(Core::MagicAAPWinRT::MagicAAPBindStatus status) : _status{status}
    {
    }

    bool Connect(uint64_t address, std::stop_token stop) override
    {
        return FakeTransport::Connect(address, stop) &&
               _status == Core::MagicAAPWinRT::MagicAAPBindStatus::Connected;
    }

    Core::AAP::AAPTransportKind Kind() const override
    {
        return Core::AAP::AAPTransportKind::MagicAAP;
    }

    Core::MagicAAPWinRT::MagicAAPBindStatus LastBindStatus() const override
    {
        return _status;
    }

private:
    Core::MagicAAPWinRT::MagicAAPBindStatus _status;
};

class ThrowingMagicAAPTransport final : public AAPTestSupport::FakeTransport
{
public:
    bool Connect(uint64_t, std::stop_token) override
    {
        throw std::runtime_error("injected MagicAAP startup failure");
    }

    Core::AAP::AAPTransportKind Kind() const override
    {
        return Core::AAP::AAPTransportKind::MagicAAP;
    }
};

// Models a driver open that only observes its stop token. Close deliberately does
// not release it, so the test proves Manager cancellation reaches the driver
// rather than merely retiring the pending transport after startup finishes.
class BlockingMagicAAPTransport final : public Core::AAP::AAPTransport
{
public:
    bool Connect(uint64_t address, std::stop_token stop) override
    {
        std::unique_lock<std::mutex> lock{_mutex};
        _address = address;
        ++_connectWaiters;
        _condition.notify_all();
        _condition.wait_for(
            lock, std::chrono::seconds(5), [&] { return stop.stop_requested() || _released; });
        --_connectWaiters;
        // A late driver failure must not overwrite intentional cancellation with
        // OpenFailed diagnostics in the Manager.
        _status = Core::MagicAAPWinRT::MagicAAPBindStatus::OpenFailed;
        return false;
    }

    bool Send(std::span<const uint8_t>, std::stop_token) override
    {
        return false;
    }

    std::optional<std::vector<uint8_t>> Receive(std::stop_token) override
    {
        return std::nullopt;
    }

    void Close() override
    {
        std::lock_guard<std::mutex> lock{_mutex};
        ++_closeCalls;
        _condition.notify_all();
    }

    Core::AAP::AAPTransportKind Kind() const override
    {
        return Core::AAP::AAPTransportKind::MagicAAP;
    }

    Core::MagicAAPWinRT::MagicAAPBindStatus LastBindStatus() const override
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _status;
    }

    bool WaitForConnectWaiter(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock{_mutex};
        return _condition.wait_for(lock, timeout, [this] { return _connectWaiters > 0; });
    }

    void ReleaseStartup()
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _released = true;
        _condition.notify_all();
    }

    uint64_t LastAddress() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _address;
    }

    size_t CloseCalls() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _closeCalls;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    size_t _connectWaiters{0};
    size_t _closeCalls{0};
    uint64_t _address{0};
    bool _released{false};
    Core::MagicAAPWinRT::MagicAAPBindStatus _status{
        Core::MagicAAPWinRT::MagicAAPBindStatus::NoMatch};
};
#endif

class AAPTransportTests : public QObject
{
    Q_OBJECT

private slots:
    void ShortWriteFailsWithoutEmittingASecondRecord()
    {
        AAPTestSupport::FakeTransport transport;
        const auto record = AAPTestSupport::Records::FeatureAck();
        QVERIFY(transport.Connect(1, {}));
        transport.SetSendLimit(record.size() - 1);

        QVERIFY(!transport.Send(record, {}));
        QCOMPARE(transport.SendCalls(), size_t{1});
        QCOMPARE(transport.LastSend(), record);
        // The refused suffix must not have been written as a second record.
        QVERIFY(transport.SentRecords().empty());
    }

    void CompleteWriteEmitsExactlyOneRecord()
    {
        AAPTestSupport::FakeTransport transport;
        const auto record = AAPTestSupport::Records::FeatureAck();

        QVERIFY(transport.Connect(1, {}));
        QVERIFY(transport.Send(record, {}));
        QCOMPARE(transport.SendCalls(), size_t{1});
        QCOMPARE(transport.SentRecords(), (std::vector<std::vector<uint8_t>>{record}));
    }

    // A parked Receive is observed through the transport's own waiter count, then woken by
    // the stop request. No wall-clock wait is used to claim the worker was blocked.
    void StopInterruptsBlockedReceive()
    {
        AAPTestSupport::FakeTransport transport;
        std::stop_source stop;
        std::optional<std::vector<uint8_t>> result;
        AAPTestSupport::Worker worker{
            [&] { result = transport.Receive(stop.get_token()); }, [&] { stop.request_stop(); }};

        QVERIFY(transport.WaitForReceiveWaiter(2s));
        stop.request_stop();
        QVERIFY(worker.Finished(2s));
        QVERIFY(!result.has_value());
    }

    void CloseInterruptsBlockedReceive()
    {
        AAPTestSupport::FakeTransport transport;
        transport.SetBlockReceive(true);
        std::optional<std::vector<uint8_t>> result;
        AAPTestSupport::Worker worker{
            [&] { result = transport.Receive({}); }, [&] { transport.Close(); }};

        QVERIFY(transport.WaitForReceiveWaiter(2s));
        transport.Close();
        QVERIFY(worker.Finished(2s));
        QVERIFY(!result.has_value());
        QVERIFY(transport.IsClosed());
    }

    // Leaving the scope must release the park by itself, which is what stops a failed
    // assertion from turning into a CI hang.
    void ScopeExitReleasesAParkedReceive()
    {
        AAPTestSupport::FakeTransport transport;
        transport.SetBlockReceive(true);
        bool returned = false;
        {
            AAPTestSupport::ScopedRelease unblock{transport};
            AAPTestSupport::Worker worker{
                [&] {
                    transport.Receive({});
                    returned = true;
                },
                [&] { transport.ReleaseAll(); }};
            QVERIFY(transport.WaitForReceiveWaiter(2s));
            transport.ReleaseAll();
            QVERIFY(worker.Finished(2s));
        }
        QVERIFY(returned);
    }

    void CloseDropsQueuedRecords()
    {
        AAPTestSupport::FakeTransport transport;
        transport.Push(AAPTestSupport::Records::FeatureAck());
        transport.Close();

        QCOMPARE(transport.RecordCount(), size_t{0});
        QVERIFY(!transport.Receive({}).has_value());
    }

    void EofReturnsNulloptAfterQueuedRecords()
    {
        AAPTestSupport::FakeTransport transport;
        const auto first = AAPTestSupport::Records::FeatureAck();
        const auto second = AAPTestSupport::Records::AttReadRequest();
        transport.PushAll({first, second});
        transport.SetEof();

        QCOMPARE(transport.Receive({}), std::optional(first));
        QCOMPARE(transport.Receive({}), std::optional(second));
        QVERIFY(!transport.Receive({}).has_value());
        // A natural end of stream is not a close: the reader keeps its joinable thread.
        QVERIFY(!transport.IsClosed());
    }

    void HandshakeAckIsAcceptedAsACompleteRecord()
    {
        using Core::AAP::AAPMessage;
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x01, 0x00, 0x04, 0x00}),
            AAPMessage::ValidationResult::Valid);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x01, 0x00, 0x04, 0x00, 0x0A}),
            AAPMessage::ValidationResult::Valid);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x01, 0x00, 0x04}),
            AAPMessage::ValidationResult::TruncatedAtt);
    }

    void HandshakeRejectsIncompleteAndUnknownRecords()
    {
        using Core::AAP::AAPMessage;
        const std::vector<uint8_t> handshake{0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        QCOMPARE(AAPMessage::Validate(handshake), AAPMessage::ValidationResult::Valid);

        auto truncated = handshake;
        truncated.resize(15);
        QCOMPARE(AAPMessage::Validate(truncated), AAPMessage::ValidationResult::TruncatedAap);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x00, 0x00, 0x04, 0x00}),
            AAPMessage::ValidationResult::TruncatedAap);

        auto unknown = handshake;
        unknown[6] = 0x03;
        QCOMPARE(AAPMessage::Validate(unknown), AAPMessage::ValidationResult::TruncatedAap);
    }

    void KnownAapRecordsEnforceDocumentedMinimumLengths_data()
    {
        QTest::addColumn<int>("opcode");
        QTest::addColumn<int>("minimumSize");

        QTest::newRow("ear-detection") << 0x0006 << 8;
        QTest::newRow("settings") << 0x0009 << 11;
        QTest::newRow("audio-source") << 0x000E << 13;
        QTest::newRow("notification-mask") << 0x000F << 10;
        QTest::newRow("head-tracking-command") << 0x0017 << 28;
        QTest::newRow("stem-event") << 0x0019 << 8;
        QTest::newRow("device-information") << 0x001D << 11;
        QTest::newRow("cloud-key-request") << 0x0030 << 8;
        QTest::newRow("cloud-key-response") << 0x0031 << 47;
        QTest::newRow("speaking-level") << 0x004B << 10;
        QTest::newRow("feature-flags") << 0x004D << 14;
    }

    void KnownAapRecordsEnforceDocumentedMinimumLengths()
    {
        QFETCH(int, opcode);
        QFETCH(int, minimumSize);
        using Core::AAP::AAPMessage;

        auto record = std::vector<uint8_t>(minimumSize, 0x00);
        record[0] = 0x04;
        record[2] = 0x04;
        record[4] = static_cast<uint8_t>(opcode & 0xFF);
        record[5] = static_cast<uint8_t>((opcode >> 8) & 0xFF);
        QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::Valid);

        record.resize(minimumSize - 1);
        QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::TruncatedAap);
    }

    void VariableLengthAapRecordsEnforceDeclaredPayloads()
    {
        using Core::AAP::AAPMessage;
        auto battery = std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x04, 0x00,
                                            0x01, 0x02, 0x01, 0x64, 0x02, 0x01};
        QCOMPARE(AAPMessage::Validate(battery), AAPMessage::ValidationResult::Valid);
        battery.pop_back();
        QCOMPARE(AAPMessage::Validate(battery), AAPMessage::ValidationResult::TruncatedAap);
        battery[6] = 0;
        QCOMPARE(AAPMessage::Validate(battery), AAPMessage::ValidationResult::TruncatedAap);

        auto rename = std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x1A, 0x00,
                                           0x01, 0x03, 0x00, 'A',  'i',  'r'};
        QCOMPARE(AAPMessage::Validate(rename), AAPMessage::ValidationResult::Valid);
        rename.pop_back();
        QCOMPARE(AAPMessage::Validate(rename), AAPMessage::ValidationResult::TruncatedAap);
    }

    void SmartRoutingRecordEnforcesCountedEntryLength()
    {
        using Core::AAP::AAPMessage;
        auto record = std::vector<uint8_t>(17, 0x00);
        record[0] = 0x04;
        record[2] = 0x04;
        record[4] = 0x2E;
        record[8] = 0x01;
        QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::Valid);

        record.pop_back();
        QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::TruncatedAap);
        record.resize(8);
        QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::TruncatedAap);
    }

    void HeadTrackingSensorRejectsTruncatedRecords()
    {
        using Core::AAP::AAPMessage;
        auto sensor = std::vector<uint8_t>(70, 0x00);
        sensor[0] = 0x04;
        sensor[2] = 0x04;
        sensor[4] = 0x17;
        sensor[10] = 0x44;
        QCOMPARE(AAPMessage::Validate(sensor), AAPMessage::ValidationResult::Valid);

        sensor.resize(69);
        QCOMPARE(AAPMessage::Validate(sensor), AAPMessage::ValidationResult::TruncatedAap);
    }

    void AttRecordsEnforceKnownOpcodesAndMinimumLengths_data()
    {
        QTest::addColumn<int>("opcode");
        QTest::addColumn<int>("minimumSize");

        QTest::newRow("error-response") << 0x01 << 5;
        QTest::newRow("read-request") << 0x0A << 3;
        QTest::newRow("read-response") << 0x0B << 2;
        QTest::newRow("write-command") << 0x12 << 4;
        QTest::newRow("notification") << 0x1B << 3;
    }

    void AttRecordsEnforceKnownOpcodesAndMinimumLengths()
    {
        QFETCH(int, opcode);
        QFETCH(int, minimumSize);
        using Core::AAP::AAPMessage;

        auto record = std::vector<uint8_t>(minimumSize, static_cast<uint8_t>(opcode));
        QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::Valid);

        if (minimumSize > 1) {
            record.resize(minimumSize - 1);
            QCOMPARE(AAPMessage::Validate(record), AAPMessage::ValidationResult::TruncatedAtt);
        }
    }

    void AttWriteCommandRejectsMissingValueByte()
    {
        using Core::AAP::AAPMessage;
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x12, 0x1B, 0x00}),
            AAPMessage::ValidationResult::TruncatedAtt);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x12, 0x1B, 0x00, 0x01}),
            AAPMessage::ValidationResult::Valid);
    }

    void MessageValidationAcceptsOnlyCompleteSupportedRecords()
    {
        using Core::AAP::AAPMessage;
        const std::vector<uint8_t> featureAck{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
        const std::vector<uint8_t> handshake{0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const std::vector<uint8_t> unknownAap{0x04, 0x00, 0x04, 0x00, 0xFE, 0xFF};
        const std::vector<uint8_t> attRead{0x0A, 0x1B, 0x00};

        QCOMPARE(AAPMessage::Validate(featureAck), AAPMessage::ValidationResult::Valid);
        QCOMPARE(AAPMessage::Validate(handshake), AAPMessage::ValidationResult::Valid);
        QCOMPARE(AAPMessage::Validate(unknownAap), AAPMessage::ValidationResult::Valid);
        QCOMPARE(AAPMessage::Validate(attRead), AAPMessage::ValidationResult::Valid);
        QCOMPARE(AAPMessage::Validate(std::vector<uint8_t>{}), AAPMessage::ValidationResult::Empty);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x04, 0x00}),
            AAPMessage::ValidationResult::Truncated);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00}),
            AAPMessage::ValidationResult::Truncated);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x4B, 0x00}),
            AAPMessage::ValidationResult::TruncatedAap);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x0B}),
            AAPMessage::ValidationResult::TruncatedAtt);
        QCOMPARE(
            AAPMessage::Validate(std::vector<uint8_t>{0x77}),
            AAPMessage::ValidationResult::InvalidAttOpcode);
        auto maximumRecord = std::vector<uint8_t>(AAPMessage::kMaxRecordSize, 0x00);
        maximumRecord[0] = 0x04;
        maximumRecord[2] = 0x04;
        maximumRecord[4] = 0xFE;
        maximumRecord[5] = 0xFF;
        QCOMPARE(AAPMessage::Validate(maximumRecord), AAPMessage::ValidationResult::Valid);
        maximumRecord.push_back(0x00);
        QCOMPARE(AAPMessage::Validate(maximumRecord), AAPMessage::ValidationResult::Oversize);
    }

    void StopInterruptsBlockedSend()
    {
        AAPTestSupport::FakeTransport transport;
        QVERIFY(transport.Connect(1, {}));
        transport.SetBlockSend(true);
        std::stop_source stop;
        bool result = true;
        AAPTestSupport::Worker worker{
            [&] {
                result = transport.Send(AAPTestSupport::Records::FeatureAck(), stop.get_token());
            },
            [&] { stop.request_stop(); }};

        QVERIFY(transport.WaitForSendWaiter(2s));
        stop.request_stop();
        QVERIFY(worker.Finished(2s));
        QVERIFY(!result);
        QCOMPARE(transport.SendCalls(), size_t{0});
    }

    void EachInjectedRecordRetainsItsBoundary()
    {
        AAPTestSupport::FakeTransport transport;
        const auto first = AAPTestSupport::Records::FeatureAck();
        const auto second = AAPTestSupport::Records::HandshakeAck();
        transport.PushAll({first, second});

        QCOMPARE(transport.Receive({}), std::optional(first));
        QCOMPARE(transport.Receive({}), std::optional(second));
    }

    // A peer that hands over a short record stays a short record: the transport contract
    // is one receive equals one record, so reassembly must never be assumed downstream.
    void FragmentedInjectionStaysAFragment()
    {
        AAPTestSupport::FakeTransport transport;
        const auto fragment = AAPTestSupport::Records::Fragment(3);
        const auto complete = AAPTestSupport::Records::HandshakeAck();
        transport.Push(fragment);
        transport.Push(complete);

        QCOMPARE(transport.Receive({}), std::optional(fragment));
        QCOMPARE(transport.Receive({}), std::optional(complete));
    }

    void PromiseGateTimesOutAndReleasesOnEveryExit()
    {
        AAPTestSupport::PromiseGate gate;
        AAPTestSupport::PromiseGate entered;
        bool returned = false;
        {
            AAPTestSupport::ScopedUnblock unblock{gate};
            AAPTestSupport::Worker worker{
                [&] {
                    entered.Signal();
                    returned = gate.Wait(2s);
                },
                [&] { gate.Signal(); }};
            QVERIFY(entered.Wait(2s));
            QVERIFY(!worker.Finished(50ms));
            gate.Signal();
            QVERIFY(worker.Finished(2s));
        }
        QVERIFY(returned);
        QVERIFY(gate.Wait(0ms));
    }

    void AtomicGateTimesOutAndReleasesOnEveryExit()
    {
        AAPTestSupport::AtomicGate gate;
        AAPTestSupport::PromiseGate entered;
        bool returned = false;
        {
            AAPTestSupport::ScopedUnblock unblock{gate};
            AAPTestSupport::Worker worker{
                [&] {
                    entered.Signal();
                    returned = gate.Wait(2s);
                },
                [&] { gate.Open(); }};
            QVERIFY(entered.Wait(2s));
            QVERIFY(!worker.Finished(50ms));
            gate.Open();
            QVERIFY(worker.Finished(2s));
        }
        QVERIFY(returned);
        QVERIFY(gate.Wait(0ms));
    }

#if defined(APD_OS_WIN)
    // The reader and the writer must not share a lock, and a Close has to wake a parked
    // Receive. Neither is timed: no datagram is queued before the release step, so the
    // only way Receive returns is Close or a stop request, and every worker is waited on
    // with a deadline and owns a release path.
    void SeqPacketSendDoesNotWaitForReceivePolling()
    {
        using Core::AAP::SeqPacketTransport;
        LoopbackDatagramPair pair;
        QVERIFY(pair.IsValid());
        SeqPacketTransport transport;
        transport._socket = std::make_shared<SeqPacketTransport::SocketState>(
            reinterpret_cast<void *>(pair.ReleaseTransportSocket()));
        transport._closed = false;

        const auto record = AAPTestSupport::Records::FeatureAck();
        std::stop_source stop;
        AAPTestSupport::EntryRendezvous entered;
        std::optional<std::vector<uint8_t>> received;
        AAPTestSupport::Worker reader{
            [&] {
                entered.Arrive();
                received = transport.Receive(stop.get_token());
            },
            [&] {
                stop.request_stop();
                transport.Close();
            }};
        QVERIFY(entered.Wait(2s));

        bool sent = false;
        AAPTestSupport::EntryRendezvous sendEntered;
        AAPTestSupport::Worker writer{
            [&] {
                sendEntered.Arrive();
                sent = transport.Send(record, {});
            },
            [&] { transport.Close(); }};
        QVERIFY(sendEntered.Wait(2s));
        QVERIFY(writer.Finished(2s));
        QVERIFY(sent);
        QCOMPARE(pair.ReceiveOnPeer(), std::optional(record));

        stop.request_stop();
        QVERIFY(reader.Finished(2s));
        transport.Close();
        QVERIFY(!received.has_value());
    }

    void SeqPacketCloseInterruptsReceivePolling()
    {
        using Core::AAP::SeqPacketTransport;
        LoopbackDatagramPair pair;
        QVERIFY(pair.IsValid());
        SeqPacketTransport transport;
        transport._socket = std::make_shared<SeqPacketTransport::SocketState>(
            reinterpret_cast<void *>(pair.ReleaseTransportSocket()));
        transport._closed = false;

        std::stop_source stop;
        AAPTestSupport::EntryRendezvous entered;
        std::optional<std::vector<uint8_t>> received;
        AAPTestSupport::Worker reader{
            [&] {
                entered.Arrive();
                received = transport.Receive(stop.get_token());
            },
            [&] {
                stop.request_stop();
                transport.Close();
            }};
        QVERIFY(entered.Wait(2s));

        // Close alone has to end the poll; the stop token is only the fallback here, so a
        // regression that drops the shutdown shows up as a missed deadline, not a hang.
        transport.Close();
        QVERIFY(reader.Finished(2s));
        QVERIFY(!received.has_value());
        transport.Close();
    }

    void SeqPacketCloseKeepsPinnedDescriptorOpen()
    {
        using Core::AAP::SeqPacketTransport;
        SeqPacketTransport transport;
        const SOCKET pinned = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        QVERIFY(pinned != INVALID_SOCKET);
        sockaddr_in loopback{};
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        QVERIFY(bind(pinned, reinterpret_cast<sockaddr *>(&loopback), sizeof(loopback)) == 0);
        auto state =
            std::make_shared<SeqPacketTransport::SocketState>(reinterpret_cast<void *>(pinned));
        transport._socket = state;
        transport._closed = false;

        transport.Close();
        sockaddr_in address{};
        int addressSize = sizeof(address);
        QVERIFY(getsockname(pinned, reinterpret_cast<sockaddr *>(&address), &addressSize) == 0);
        const SOCKET replacement = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        QVERIFY(replacement != INVALID_SOCKET);
        QVERIFY(replacement != pinned);
        closesocket(replacement);
        state.reset();
    }

    void SeqPacketReceiveHonorsStopToken()
    {
        using Core::AAP::SeqPacketTransport;
        LoopbackDatagramPair pair;
        QVERIFY(pair.IsValid());
        SeqPacketTransport transport;
        transport._socket = std::make_shared<SeqPacketTransport::SocketState>(
            reinterpret_cast<void *>(pair.ReleaseTransportSocket()));
        transport._closed = false;

        std::stop_source stop;
        AAPTestSupport::EntryRendezvous entered;
        std::optional<std::vector<uint8_t>> received;
        AAPTestSupport::Worker reader{
            [&] {
                entered.Arrive();
                received = transport.Receive(stop.get_token());
            },
            [&] {
                stop.request_stop();
                transport.Close();
            }};
        QVERIFY(entered.Wait(2s));
        stop.request_stop();
        QVERIFY(reader.Finished(2s));
        QVERIFY(!received.has_value());
        // A cancelled token must short-circuit later calls without parking again.
        QVERIFY(!transport.Receive(stop.get_token()).has_value());
        transport.Close();
    }

    void SeqPacketReceivePreservesRecordBoundaries()
    {
        using Core::AAP::SeqPacketTransport;
        LoopbackDatagramPair pair;
        QVERIFY(pair.IsValid());
        SeqPacketTransport transport;
        transport._socket = std::make_shared<SeqPacketTransport::SocketState>(
            reinterpret_cast<void *>(pair.ReleaseTransportSocket()));
        transport._closed = false;

        const auto first = AAPTestSupport::Records::FeatureAck();
        const auto second = AAPTestSupport::Records::AttReadRequest();
        QVERIFY(pair.SendFromPeer(first));
        QVERIFY(pair.SendFromPeer(second));
        QCOMPARE(transport.Receive({}), std::optional(first));
        QCOMPARE(transport.Receive({}), std::optional(second));
        transport.Close();
    }

    void MalformedHeadTrackingNeverReachesManagerCallback()
    {
        Core::AAP::Manager manager;
        int trackingCallbacks = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onHeadTrackingData = [&trackingCallbacks](
                                           const Core::AAP::HeadTrackingData &,
                                           const Core::AAP::AAPAttempt &) { ++trackingCallbacks; };
        manager.SetCallbacks(std::move(callbacks));
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager._headTrackingActive = true;

        // The parser only needs 56 bytes, but the documented sensor record is at
        // least 70 bytes. The callback witnesses rejection before parsing.
        std::vector<uint8_t> truncatedSensor(69, 0x00);
        truncatedSensor[0] = 0x04;
        truncatedSensor[2] = 0x04;
        truncatedSensor[4] = 0x17;
        truncatedSensor[10] = 0x44;
        manager.ProcessPacket(truncatedSensor);
        QCOMPARE(trackingCallbacks, 0);

        std::vector<uint8_t> sensor = truncatedSensor;
        sensor.resize(70);
        manager.ProcessPacket(sensor);
        QCOMPARE(trackingCallbacks, 1);
    }

    void UnknownAapIsNotMisparsedAsHeadTracking()
    {
        Core::AAP::Manager manager;
        int trackingCallbacks = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onHeadTrackingData = [&trackingCallbacks](
                                           const Core::AAP::HeadTrackingData &,
                                           const Core::AAP::AAPAttempt &) { ++trackingCallbacks; };
        manager.SetCallbacks(std::move(callbacks));
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager._headTrackingActive = true;

        std::vector<uint8_t> unknownAap(70, 0x00);
        unknownAap[0] = 0x04;
        unknownAap[2] = 0x04;
        unknownAap[4] = 0xFE;
        unknownAap[5] = 0xFF;
        manager.ProcessPacket(unknownAap);
        QCOMPARE(trackingCallbacks, 0);
    }

    void AttWriteIsNotMisparsedAsHeadTracking()
    {
        Core::AAP::Manager manager;
        int trackingCallbacks = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onHeadTrackingData = [&trackingCallbacks](
                                           const Core::AAP::HeadTrackingData &,
                                           const Core::AAP::AAPAttempt &) { ++trackingCallbacks; };
        manager.SetCallbacks(std::move(callbacks));
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager._headTrackingActive = true;

        std::vector<uint8_t> attWrite(70, 0x00);
        attWrite[0] = 0x12;
        attWrite[4] = 0x17;
        manager.ProcessPacket(attWrite);
        QCOMPARE(trackingCallbacks, 0);
    }

    void ManagerUsesTransportAtomicSendResult()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->SetSendLimit(5);
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        QVERIFY(!manager.SetNoiseControlMode(Core::AAP::NoiseControlMode::Off));
        QCOMPARE(transport->SendCalls(), size_t{1});
        QCOMPARE(transport->LastSend(), AAPTestSupport::Records::NoiseControl(0x01));
        // A refused write leaves no partial record behind for the peer to misread.
        QVERIFY(transport->SentRecords().empty());
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void ManagerMapsBindStatusAndRecordsFinalDriverOpen_data()
    {
        using Core::AAP::ConnectFailure;
        using Core::MagicAAPWinRT::MagicAAPBindStatus;
        QTest::addColumn<int>("bindStatus");
        QTest::addColumn<int>("expectedFailure");
        QTest::newRow("connected") << static_cast<int>(MagicAAPBindStatus::Connected)
                                   << static_cast<int>(ConnectFailure::None);
        QTest::newRow("no-match") << static_cast<int>(MagicAAPBindStatus::NoMatch)
                                  << static_cast<int>(ConnectFailure::NoMatch);
        QTest::newRow("open-failed") << static_cast<int>(MagicAAPBindStatus::OpenFailed)
                                     << static_cast<int>(ConnectFailure::OpenFailed);
        QTest::newRow("driver-missing") << static_cast<int>(MagicAAPBindStatus::DriverMissing)
                                        << static_cast<int>(ConnectFailure::DriverMissing);
        QTest::newRow("transport-error") << static_cast<int>(MagicAAPBindStatus::TransportError)
                                         << static_cast<int>(ConnectFailure::TransportError);
        QTest::newRow("cancelled") << static_cast<int>(MagicAAPBindStatus::Cancelled)
                                   << static_cast<int>(ConnectFailure::TransportError);
    }

    void ManagerMapsBindStatusAndRecordsFinalDriverOpen()
    {
        using Core::AAP::ConnectFailure;
        using Core::MagicAAPWinRT::MagicAAPBindStatus;
        QFETCH(int, bindStatus);
        QFETCH(int, expectedFailure);
        const auto status = static_cast<MagicAAPBindStatus>(bindStatus);

        Core::AAP::Manager manager;
        auto seqPacket = std::make_shared<UnavailableSeqPacketTransport>();
        auto magicAAP = std::make_shared<BindStatusTransport>(status);
        manager._driverInstalledProbe = [] { return true; };
        manager._driverRunningProbe = [] { return true; };
        manager._transportFactory = [seqPacket, magicAAP](Core::AAP::AAPTransportKind kind) {
            if (kind == Core::AAP::AAPTransportKind::SeqPacket) {
                return std::shared_ptr<Core::AAP::AAPTransport>{seqPacket};
            }
            return std::shared_ptr<Core::AAP::AAPTransport>{magicAAP};
        };

        Core::Diagnostics::Recorder::Instance().Reset();
        ConnectFailure failure = ConnectFailure::None;
        const auto transport = manager.ConnectTransport({1, 1}, {}, failure);
        const auto report = Core::Diagnostics::Recorder::Instance().BuildReport({});
        if (status == MagicAAPBindStatus::Connected) {
            QVERIFY(transport != nullptr);
            QVERIFY(report.find("driver_open  ok") != std::string::npos);
            QVERIFY(report.find("driver_open  FAILED") == std::string::npos);
            transport->Close();
        }
        else {
            QVERIFY(transport == nullptr);
            QVERIFY(report.find("driver_open  FAILED") != std::string::npos);
            QVERIFY(report.find("driver_open  ok") == std::string::npos);
        }
        QCOMPARE(failure, static_cast<ConnectFailure>(expectedFailure));
    }

    void ManagerRecordsDriverOpenFailureWhenMagicConnectThrows()
    {
        Core::AAP::Manager manager;
        auto seqPacket = std::make_shared<UnavailableSeqPacketTransport>();
        auto magicAAP = std::make_shared<ThrowingMagicAAPTransport>();
        manager._driverInstalledProbe = [] { return true; };
        manager._driverRunningProbe = [] { return true; };
        manager._transportFactory = [seqPacket, magicAAP](Core::AAP::AAPTransportKind kind) {
            if (kind == Core::AAP::AAPTransportKind::SeqPacket) {
                return std::shared_ptr<Core::AAP::AAPTransport>{seqPacket};
            }
            return std::shared_ptr<Core::AAP::AAPTransport>{magicAAP};
        };

        Core::Diagnostics::Recorder::Instance().Reset();
        Core::AAP::ConnectFailure failure = Core::AAP::ConnectFailure::None;
        const auto transport = manager.ConnectTransport({1, 1}, {}, failure);
        const auto report = Core::Diagnostics::Recorder::Instance().BuildReport({});
        QVERIFY(transport == nullptr);
        QCOMPARE(failure, Core::AAP::ConnectFailure::TransportError);
        QVERIFY(report.find("driver_open  FAILED") != std::string::npos);
        QVERIFY(report.find("driver_open  ok") == std::string::npos);
    }

    void ManagerConnectInitializesBeforeConnectionGateOpens()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [transport](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return transport;
        };

        const Core::AAP::AAPAttempt attempt{1, 1};
        QVERIFY(manager.Connect(attempt));
        QVERIFY(manager.IsConnected());
        QCOMPARE(manager.GetLastAttempt().generation, attempt.generation);

        // Handshake, feature flags, notification subscriptions, then one ATT read.
        QCOMPARE(transport->SendCalls(), size_t{4});
        QCOMPARE(transport->LastSend(), (std::vector<uint8_t>{0x0A, 0x1B, 0x00}));
        QVERIFY(manager._readerThread.joinable());

        manager.Disconnect();
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._readerThread.joinable());
    }

    void ManagerMagicAAPStartupCancellationAllowsImmediateRebind_data()
    {
        QTest::addColumn<bool>("disconnect");
        QTest::newRow("cancel-connect") << false;
        QTest::newRow("disconnect") << true;
    }

    void ManagerMagicAAPStartupCancellationAllowsImmediateRebind()
    {
        using Core::AAP::AAPAttempt;
        using Core::AAP::AAPTransportKind;
        using Core::AAP::ConnectFailure;
        QFETCH(bool, disconnect);

        Core::AAP::Manager manager;
        auto seqPacket = std::make_shared<UnavailableSeqPacketTransport>();
        auto magicAAP = std::make_shared<BlockingMagicAAPTransport>();
        manager._driverInstalledProbe = [] { return true; };
        manager._driverRunningProbe = [] { return true; };
        manager._transportFactory = [seqPacket, magicAAP](AAPTransportKind kind) {
            if (kind == AAPTransportKind::SeqPacket) {
                return std::shared_ptr<Core::AAP::AAPTransport>{seqPacket};
            }
            return std::shared_ptr<Core::AAP::AAPTransport>{magicAAP};
        };
        int connectedGenerations = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onConnected = [&](const AAPAttempt &attempt) {
            connectedGenerations = static_cast<int>(attempt.generation);
        };
        manager.SetCallbacks(std::move(callbacks));

        constexpr uint64_t targetAddress = 0x340E22CC08A0ULL;
        const AAPAttempt firstAttempt{targetAddress, 7};
        Core::Diagnostics::Recorder::Instance().Reset();
        auto connect =
            std::async(std::launch::async, [&] { return manager.Connect(firstAttempt); });
        const bool startupEntered = magicAAP->WaitForConnectWaiter(std::chrono::seconds(1));
        if (!startupEntered) {
            magicAAP->ReleaseStartup();
        }
        QVERIFY(startupEntered);

        auto cancel = std::async(std::launch::async, [&] {
            if (disconnect) {
                manager.Disconnect();
            }
            else {
                manager.CancelConnect();
            }
        });
        const bool cancellationReturned =
            cancel.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        const bool startupReturned =
            connect.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

        // Release the fake driver before joining even when the regression is
        // present, so a failed assertion cannot strand a blocked future.
        magicAAP->ReleaseStartup();
        cancel.get();
        const bool connected = connect.get();

        QVERIFY(cancellationReturned);
        QVERIFY(startupReturned);
        QVERIFY(!connected);
        QCOMPARE(manager.GetLastConnectFailure(), ConnectFailure::TransportError);
        QCOMPARE(magicAAP->LastAddress(), targetAddress);
        QVERIFY(magicAAP->CloseCalls() > 0);
        QCOMPARE(connectedGenerations, 0);
        const auto report = Core::Diagnostics::Recorder::Instance().BuildReport({});
        QVERIFY(report.find("driver_open  FAILED") == std::string::npos);
        QVERIFY(report.find("exact target interface open failed") == std::string::npos);

        auto rebound = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [rebound](AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return rebound;
        };
        const AAPAttempt secondAttempt{targetAddress, 8};
        QVERIFY(manager.Connect(secondAttempt));
        QVERIFY(manager.IsConnected());
        QCOMPARE(connectedGenerations, 8);
        QCOMPARE(rebound->SendCalls(), size_t{4});

        auto disconnectRebound = std::async(std::launch::async, [&] { manager.Disconnect(); });
        QVERIFY(disconnectRebound.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        disconnectRebound.get();
        QVERIFY(!manager.IsConnected());
    }

    void CancelConnectBeforeAttemptStartsSuppressesThatGeneration()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [transport](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return transport;
        };
        manager._attempt = Core::AAP::AAPAttempt{1, 7};
        manager.CancelConnect();
        manager._attempt = {};

        const Core::AAP::AAPAttempt attempt{1, 7};
        QVERIFY(!manager.Connect(attempt));
        QCOMPARE(transport->SendCalls(), size_t{0});
    }

    // Every harness deadline expires with a result instead of blocking forever, which is
    // what makes the CI timeout a backstop rather than the primary guard.
    void RendezvousDeadlinesExpireInsteadOfBlocking()
    {
        AAPTestSupport::FakeTransport idle;
        QVERIFY(!idle.WaitForReceiveWaiter(50ms));
        QVERIFY(!idle.WaitForSendWaiter(50ms));
        QVERIFY(!idle.WaitForConnectWaiter(50ms));

        AAPTestSupport::BlockingTransport parked;
        parked.Block(AAPTestSupport::BlockingTransport::Op::Send);
        QVERIFY(!parked.WaitForParked(AAPTestSupport::BlockingTransport::Op::Send, 50ms));

        AAPTestSupport::CallbackRecorder recorder;
        QVERIFY(!recorder.WaitForCount("never", 1, 50ms));
    }
#endif
};

QTEST_GUILESS_MAIN(AAPTransportTests)

#include "AAPTransportTests.moc"
