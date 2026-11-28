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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <string>
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
    #include "Source/Core/ConnectionDiagnostics.h"
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
// Byte-exact 222-byte 0x001D record from librepods' "AAP Definitions.md"
// example packet (same capture as Docs/AAP-Protocol-Spec.md §3). Shared with
// the device-information cache tests below; keep in sync with the copy inside
// AAPProtocolTests.cpp.
namespace {

std::vector<uint8_t> DeviceInfoSampleBytes()
{
    static const std::vector<uint8_t> sample = [] {
        const QByteArray hex = QByteArray::fromHex(
            "040004001d0002d5000400416972506f64732050726f004133303438004170706c6520496e632e0051584e"
            "524848595850360036312e313836383034303030323030303030302e323731330036312e31383638303430"
            "3030323030303030302e3237313300312e302e3000636f6d2e6170706c652e6163636573736f72792e7570"
            "64617465722e6170702e3731004859394c5432454632364a59004833504c5748444a32364b300036333537"
            "3533360089312a6567a5400f84a3ca234947efd40b90d78436ae5946748d70273e66066a25893000353339"
            "35303630363400");
        return std::vector<uint8_t>(hex.begin(), hex.end());
    }();
    return sample;
}

// Builds a well-formed 0x001D record whose serial is `serial`.
std::vector<uint8_t> BuildDeviceInfoWithSerial(const char *serial)
{
    std::vector<uint8_t> packet{0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xD5, 0x00, 0x04, 0x00};
    const std::vector<const char *> fields{
        "AirPods Pro", "A3048",   "Apple Inc.", serial,
        "61.18",       "61.18",   "1.0.0",      "com.apple.accessory.updater.app.71",
        "Lserial",     "Rserial", "6357536"};
    for (const char *field : fields) {
        packet.insert(packet.end(), field, field + std::char_traits<char>::length(field));
        packet.push_back(0x00);
    }
    return packet;
}

} // namespace
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
#if defined(APD_OS_WIN)
    static void ResetRecorder()
    {
        auto &recorder = Core::Diagnostics::Recorder::Instance();
        recorder.EndAttempt(recorder.CurrentGeneration());
        recorder.Reset();
        QCOMPARE(recorder.CurrentGeneration(), uint64_t{0});
        QCOMPARE(recorder.StepCount(), size_t{0});
    }

    void init()
    {
        ResetRecorder();
    }

    void cleanup()
    {
        ResetRecorder();
    }
#endif

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

    void FakeTransportReconnectsAfterClose()
    {
        AAPTestSupport::FakeTransport transport;
        QVERIFY(transport.Connect(1, {}));
        transport.Close();
        QVERIFY(transport.IsClosed());

        QVERIFY(transport.Connect(2, {}));
        QVERIFY(!transport.IsClosed());
        QCOMPARE(transport.ConnectCalls(), size_t{2});
        QCOMPARE(transport.LastConnectAddress(), uint64_t{2});
    }

    void CloseInterruptsBlockedConnect()
    {
        AAPTestSupport::FakeTransport transport;
        transport.SetBlockConnect(true);
        bool result = true;
        AAPTestSupport::Worker worker{
            [&] { result = transport.Connect(1, {}); }, [&] { transport.Close(); }};

        QVERIFY(transport.WaitForConnectWaiter(2s));
        transport.Close();
        QVERIFY(worker.Finished(2s));
        QVERIFY(!result);
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

    void PromiseGateObservesWaitEntryAndReleasesOnEveryExit()
    {
        AAPTestSupport::PromiseGate gate;
        bool returned = false;
        {
            AAPTestSupport::ScopedUnblock unblock{gate};
            AAPTestSupport::Worker worker{
                [&] { returned = gate.Wait(2s); }, [&] { gate.Signal(); }};
            QVERIFY(gate.WaitForWaiter(2s));
            const auto parked = gate.Snapshot();
            QCOMPARE(parked.waiters, size_t{1});
            QVERIFY(!parked.open);
            gate.Signal();
            QVERIFY(worker.Finished(2s));
        }
        QVERIFY(returned);
        const auto released = gate.Snapshot();
        QCOMPARE(released.waiters, size_t{0});
        QVERIFY(released.open);
        QVERIFY(gate.Wait(0ms));
    }

    void AtomicGateObservesPredicateEntryAndReleasesOnEveryExit()
    {
        AAPTestSupport::AtomicGate gate;
        bool returned = false;
        {
            AAPTestSupport::ScopedUnblock unblock{gate};
            AAPTestSupport::Worker worker{[&] { returned = gate.Wait(2s); }, [&] { gate.Open(); }};
            QVERIFY(gate.WaitForWaiter(2s));
            const auto parked = gate.Snapshot();
            QCOMPARE(parked.waiters, size_t{1});
            QVERIFY(!parked.open);
            gate.Open();
            QVERIFY(worker.Finished(2s));
        }
        QVERIFY(returned);
        const auto released = gate.Snapshot();
        QCOMPARE(released.waiters, size_t{0});
        QVERIFY(released.open);
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
        transport._receivePollObserverForTest = [&] { entered.Arrive(); };
        std::optional<std::vector<uint8_t>> received;
        AAPTestSupport::Worker reader{
            [&] {
                received = transport.Receive(stop.get_token());
                entered.Complete();
            },
            [&] {
                stop.request_stop();
                transport.Close();
            }};
        QVERIFY(entered.Wait(2s));
        const auto polling = entered.Snapshot();
        QVERIFY(polling.entered);
        QVERIFY(!polling.completed);

        bool sent = false;
        AAPTestSupport::Worker writer{
            [&] { sent = transport.Send(record, {}); }, [&] { transport.Close(); }};
        QVERIFY(writer.Finished(2s));
        QVERIFY(sent);
        QCOMPARE(pair.ReceiveOnPeer(), std::optional(record));
        const auto stillPolling = entered.Snapshot();
        QVERIFY(stillPolling.entered);
        QVERIFY(!stillPolling.completed);

        stop.request_stop();
        QVERIFY(entered.WaitForCompletion(2s));
        QVERIFY(reader.Finished(2s));
        transport.Close();
        QVERIFY(!received.has_value());
        const auto completed = entered.Snapshot();
        QVERIFY(completed.entered);
        QVERIFY(completed.completed);
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
        transport._receivePollObserverForTest = [&] { entered.Arrive(); };
        std::optional<std::vector<uint8_t>> received;
        AAPTestSupport::Worker reader{
            [&] {
                received = transport.Receive(stop.get_token());
                entered.Complete();
            },
            [&] {
                stop.request_stop();
                transport.Close();
            }};
        QVERIFY(entered.Wait(2s));
        const auto polling = entered.Snapshot();
        QVERIFY(polling.entered);
        QVERIFY(!polling.completed);

        // Close alone has to end the poll; the stop token is only the fallback here, so a
        // regression that drops the shutdown shows up as a missed deadline, not a hang.
        transport.Close();
        QVERIFY(entered.WaitForCompletion(2s));
        QVERIFY(reader.Finished(2s));
        QVERIFY(!received.has_value());
        transport.Close();
        const auto completed = entered.Snapshot();
        QVERIFY(completed.entered);
        QVERIFY(completed.completed);
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
        transport._receivePollObserverForTest = [&] { entered.Arrive(); };
        std::optional<std::vector<uint8_t>> received;
        AAPTestSupport::Worker reader{
            [&] {
                received = transport.Receive(stop.get_token());
                entered.Complete();
            },
            [&] {
                stop.request_stop();
                transport.Close();
            }};
        QVERIFY(entered.Wait(2s));
        const auto polling = entered.Snapshot();
        QVERIFY(polling.entered);
        QVERIFY(!polling.completed);
        stop.request_stop();
        QVERIFY(entered.WaitForCompletion(2s));
        QVERIFY(reader.Finished(2s));
        QVERIFY(!received.has_value());
        // A cancelled token must short-circuit later calls without parking again.
        QVERIFY(!transport.Receive(stop.get_token()).has_value());
        transport.Close();
        const auto completed = entered.Snapshot();
        QVERIFY(completed.entered);
        QVERIFY(completed.completed);
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

        // Use the shared classifier's documented prefix. The parser only needs 56 bytes, but
        // the documented sensor record is at least 70 bytes; the callback witnesses rejection
        // before parsing. A bad prefix must remain rejected even at the full documented length.
        std::vector<uint8_t> sensor(70, 0x00);
        sensor[0] = 0x04;
        sensor[2] = 0x04;
        sensor[4] = 0x17;
        sensor[8] = 0x10;
        sensor[10] = 0x44;

        auto malformedPrefix = sensor;
        malformedPrefix[8] = 0x00;
        manager.ProcessPacket(malformedPrefix);
        QCOMPARE(trackingCallbacks, 0);

        auto truncatedSensor = sensor;
        truncatedSensor.resize(69);
        manager.ProcessPacket(truncatedSensor);
        QCOMPARE(trackingCallbacks, 0);

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

        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager._commandBroker->BeginSession(1);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));

        QVERIFY(manager.SetNoiseControlMode(Core::AAP::NoiseControlMode::Off));
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(future.get().settlement, Core::AAP::AAPCommandSettlement::Rejected);
        QCOMPARE(transport->SendCalls(), size_t{1});
        QCOMPARE(transport->LastSend(), AAPTestSupport::Records::NoiseControl(0x01));
        // A refused write leaves no partial record behind for the peer to misread.
        QVERIFY(transport->SentRecords().empty());
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void ManagerCommandIsConfirmedBySettingsEcho()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 9};
        manager._connected = true;
        manager._commandBroker->BeginSession(9);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        const Core::AAP::AAPCommand command{Core::AAP::AAPCommandKind::SetNoiseControl, 0x02};
        manager._commandBroker->BeginSession(manager._attempt.generation);
        QVERIFY(manager.SubmitCommand(command).accepted);
        QVERIFY(transport->WaitForRecordCount(1, std::chrono::seconds{1}));
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00});
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(future.get().settlement, Core::AAP::AAPCommandSettlement::Confirmed);
        QCOMPARE(manager.ConfirmedCommand(command.kind), std::optional<uint32_t>{0x02});
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void ManagerLoudSoundReductionUsesAttReadback()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 10};
        manager._connected = true;
        manager._commandBroker->BeginSession(10);
        manager._attPathState.store(Core::AAP::AttPathState::Confirmed);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        const Core::AAP::AAPCommand command{Core::AAP::AAPCommandKind::SetLoudSoundReduction, 0x01};
        manager._commandBroker->BeginSession(manager._attempt.generation);
        QVERIFY(manager.SubmitCommand(command).accepted);
        QCOMPARE(
            manager.CommandMode(command.kind, command.value), Core::AAP::AAPConfirmMode::Readback);
        QVERIFY(transport->WaitForRecordCount(2, std::chrono::seconds{1}));
        manager.ProcessAttFrame({0x0B, 0x01}, manager._attempt, callbacks);
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(future.get().settlement, Core::AAP::AAPCommandSettlement::Confirmed);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        const auto records = transport->SentRecords();
        QCOMPARE(records.size(), size_t{2});
        QCOMPARE(records[0], (std::vector<uint8_t>{0x12, 0x1B, 0x00, 0x01}));
        QCOMPARE(records[1], (std::vector<uint8_t>{0x0A, 0x1B, 0x00}));
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void StartupAttProbeCannotConfirmOverlappingLoudSoundCommand()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 12};
        manager._connected = true;
        manager._commandBroker->BeginSession(12);
        manager.StartAttProbe(transport, {});
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        QVERIFY(manager.SubmitCommand({Core::AAP::AAPCommandKind::SetLoudSoundReduction, 0x01})
                    .accepted);
        QVERIFY(transport->WaitForRecordCount(2, std::chrono::seconds{1}));
        // This is the probe's response. It establishes the baseline but must not confirm the
        // command whose read has not been issued yet.
        manager.ProcessAttFrame({0x0B, 0x00}, manager._attempt, callbacks);
        QVERIFY(transport->WaitForRecordCount(3, std::chrono::seconds{1}));
        QCOMPARE(
            manager.ConfirmedCommand(Core::AAP::AAPCommandKind::SetLoudSoundReduction),
            std::optional<uint32_t>{0x00});
        manager.ProcessAttFrame({0x0B, 0x01}, manager._attempt, callbacks);
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(future.get().settlement, Core::AAP::AAPCommandSettlement::Confirmed);
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void ImmediateAttProbeResponseStaysConfirmed()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 21};
        manager._connected = true;
        manager._commandBroker->BeginSession(21);
        manager.StartAttProbe(transport, {});
        // Pending was published before the write, so an answer that lands while the probe
        // window is open confirms the path and is never downgraded back to Pending.
        manager.ProcessAttFrame({0x0B, 0x00}, manager._attempt, Core::AAP::Callbacks{});
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Confirmed);
        manager.ProcessAttFrame({0x0B, 0x01}, manager._attempt, Core::AAP::Callbacks{});
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Confirmed);
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void AttProbeDeadlineBoundaryIsExact()
    {
        Core::AAP::Manager manager;
        AAPTestSupport::ManualClock clock;
        manager._attNowForTest = [&clock] { return clock.Now(); };
        manager._attPathState.store(Core::AAP::AttPathState::Pending);
        manager._attProbeDeadlineMs.store(500);
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Pending);
        clock.Advance(499ms);
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Pending);
        clock.Advance(1ms);
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Unconfirmed);
    }

    void LateAttResponseFromRetiredSessionIsDropped()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 22};
        manager._connected = true;
        manager._commandBroker->BeginSession(22);
        manager.StartAttProbe(transport, {});
        // The session dies before the answer arrives; a fresh generation starts afterwards.
        manager._connected = false;
        manager._commandBroker->EndSession();
        manager._attempt = Core::AAP::AAPAttempt{1, 23};
        manager._connected = true;
        manager._commandBroker->BeginSession(23);
        // The retired generation's answer must not confirm the new session's ATT path.
        manager.ProcessAttFrame({0x0B, 0x00}, Core::AAP::AAPAttempt{1, 22}, Core::AAP::Callbacks{});
        QVERIFY(manager.GetAttPathState() != Core::AAP::AttPathState::Confirmed);
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void AttErrorWithdrawsCapabilityAndSettlesActiveFlight()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 13};
        manager._connected = true;
        manager._attPathState.store(Core::AAP::AttPathState::Confirmed);
        manager._commandBroker->BeginSession(13);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        QVERIFY(manager.SubmitCommand({Core::AAP::AAPCommandKind::SetLoudSoundReduction, 0x01})
                    .accepted);
        QVERIFY(transport->WaitForRecordCount(2, std::chrono::seconds{1}));
        manager.ProcessAttFrame({0x01, 0x0A, 0x1B, 0x00, 0x0A}, manager._attempt, callbacks);
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto outcome = future.get();
        QCOMPARE(outcome.settlement, Core::AAP::AAPCommandSettlement::AppliedUnconfirmed);
        QVERIFY(!outcome.rollBackDesired);
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void DisconnectDuringSendSettlesAsTransportClosed()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->SetBlockSend(true);
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 11};
        manager._connected = true;
        manager._commandBroker->BeginSession(11);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        QVERIFY(manager.SubmitCommand({Core::AAP::AAPCommandKind::SetNoiseControl, 0x02}).accepted);
        QVERIFY(transport->WaitForSendWaiter(std::chrono::seconds{1}));
        manager.Disconnect();
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto outcome = future.get();
        QCOMPARE(outcome.settlement, Core::AAP::AAPCommandSettlement::TransportClosed);
        QVERIFY(!outcome.rollBackDesired);
        QCOMPARE(
            manager.DesiredCommand(Core::AAP::AAPCommandKind::SetNoiseControl),
            std::optional<uint32_t>{0x02});
    }

    void AdaptiveNoiseStrengthRequiresAdaptiveModeAndCurrentSession()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 12};
        manager._connected = true;
        manager._commandBroker->BeginSession(manager._attempt.generation);

        // Unknown and every non-Adaptive mode reject the write without touching the wire.
        for (const auto mode :
             {Core::AAP::NoiseControlMode::Off, Core::AAP::NoiseControlMode::NoiseCancellation,
              Core::AAP::NoiseControlMode::Transparency})
        {
            manager._noiseControlMode = mode;
            QVERIFY(!manager.SetAdaptiveNoiseLevel(70));
        }
        QVERIFY(!manager.SetAdaptiveNoiseLevel(70));
        QCOMPARE(transport->SendCalls(), size_t{0});
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        // Adaptive accepts the submission; the broker queues it and the wire write can still
        // fail. A failed write publishes no echo, so the cached value stays unknown.
        manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        transport->SetSendLimit(5);
        QVERIFY(manager.SetAdaptiveNoiseLevel(70));
        QTRY_VERIFY(transport->WaitForSendAttempts(1, std::chrono::seconds{1}));
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        transport->SetSendLimit(std::nullopt);
        QVERIFY(manager.SetAdaptiveNoiseLevel(255));
        QTRY_COMPARE(transport->SendCalls(), size_t{2});
        QCOMPARE(
            transport->LastSend(),
            (std::vector<uint8_t>{
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100, 0x00, 0x00, 0x00}));
        // Only the device's echo publishes the cached value.
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100, 0x00, 0x00, 0x00});
        QCOMPARE(manager.GetAdaptiveNoiseLevel(), std::optional<uint8_t>{100});

        manager._noiseControlMode = Core::AAP::NoiseControlMode::Off;
        const auto sends = transport->SendCalls();
        QVERIFY(!manager.SetAdaptiveNoiseLevel(0));
        QCOMPARE(transport->SendCalls(), sends);

        manager.Disconnect();
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());
        QVERIFY(!manager.GetNoiseControlMode().has_value());
    }

    void AdaptiveNoiseStrengthEligibilityAndDesiredRecordingAreAtomic()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 13};
        manager._connected = true;
        manager._noiseControlMode = NoiseControlMode::Off;
        manager._adaptiveNoiseLevel = 60;
        manager._commandBroker->BeginSession(manager._attempt.generation);
        const AAPCommand initial{AAPCommandKind::SetAdaptiveNoiseLevel, 60};
        QVERIFY(!manager._commandBroker->RecordDesired(initial).accepted);
        QCOMPARE(
            manager._commandBroker->Desired(AAPCommandKind::SetAdaptiveNoiseLevel),
            std::optional<uint32_t>{60});
        manager._commandBroker->OnEcho(initial, manager._attempt.generation);
        transport->SetBlockSend(true);

        std::promise<void> eligibilityEntered;
        auto enteredFuture = eligibilityEntered.get_future();
        std::promise<void> resumeEligibility;
        auto resumeFuture = resumeEligibility.get_future();
        manager._beforeAdaptiveEligibilityForTest = [&] {
            eligibilityEntered.set_value();
            resumeFuture.wait();
        };

        std::future<AAPCommandResult> submission = std::async(std::launch::async, [&] {
            return manager.SubmitCommand({AAPCommandKind::SetAdaptiveNoiseLevel, 70});
        });
        QVERIFY(enteredFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

        // Exact former interleaving: Adaptive is reported after eligibility was read as Off, but
        // before 70 is recorded. The notification sees desired/cache 60 and queues nothing. An
        // atomic implementation rechecks Adaptive under the state lock and submits the new value.
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x04, 0x00, 0x00, 0x00});
        resumeEligibility.set_value();
        QVERIFY(submission.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(submission.get().accepted);
        QVERIFY(transport->WaitForSendWaiter(std::chrono::seconds{1}));
        QCOMPARE(
            manager.DesiredCommand(AAPCommandKind::SetAdaptiveNoiseLevel),
            std::optional<uint32_t>{70});

        transport->SetBlockSend(false);
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 70, 0x00, 0x00, 0x00});
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QCOMPARE(transport->SendCalls(), size_t{1});
        QCOMPARE(manager.GetAdaptiveNoiseLevel(), std::optional<uint8_t>{70});
        manager.Disconnect();
    }

    void RepeatedAdaptiveNotificationsDoNotResubmitInFlightStrength()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->SetBlockSend(true);
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 14};
        manager._connected = true;
        manager._noiseControlMode = NoiseControlMode::Adaptive;
        manager._commandBroker->BeginSession(manager._attempt.generation);
        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetAdaptiveNoiseLevel, 65}).accepted);

        std::promise<AAPCommandOutcome> settled;
        auto settledFuture = settled.get_future();
        std::atomic<bool> reported{false};
        Callbacks callbacks;
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &outcome, AAPAttempt) {
            if (!reported.exchange(true)) {
                settled.set_value(outcome);
            }
        };
        manager.SetCallbacks(std::move(callbacks));
        const std::vector<uint8_t> adaptive{0x04, 0x00, 0x04, 0x00, 0x09, 0x00,
                                            0x0D, 0x04, 0x00, 0x00, 0x00};
        manager.ProcessPacket(adaptive);
        QVERIFY(transport->WaitForSendWaiter(std::chrono::seconds{1}));
        manager.ProcessPacket(adaptive);
        manager.ProcessPacket(adaptive);
        QCOMPARE(manager._commandBroker->QueuedCount(), size_t{0});

        transport->SetBlockSend(false);
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 65, 0x00, 0x00, 0x00});
        QVERIFY(settledFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(settledFuture.get().settlement, AAPCommandSettlement::Confirmed);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QCOMPARE(transport->SendCalls(), size_t{1});
        manager.Disconnect();
    }

    void AdaptiveNoiseStrengthAttemptBarrierNeverReachesReplacementTransport()
    {
        Core::AAP::Manager manager;
        auto oldTransport = std::make_shared<AAPTestSupport::FakeTransport>();
        auto newTransport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(oldTransport->Connect(1, {}));
        QVERIFY(newTransport->Connect(1, {}));
        oldTransport->SetBlockSend(true);
        manager._transport.store(oldTransport);
        manager._attempt = Core::AAP::AAPAttempt{1, 9};
        manager._connected = true;
        manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        manager._commandBroker->BeginSession(9);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        auto settledFuture = settled.get_future();
        // Two settlements are expected here: the generation-9 TransportClosed and the
        // generation-10 echo confirmation. A second set_value would throw out of the
        // callback and fail-fast the whole suite.
        std::atomic<bool> reported{false};
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            if (!reported.exchange(true)) {
                settled.set_value(outcome);
            }
        };
        manager.SetCallbacks(std::move(callbacks));

        auto result = std::async(std::launch::async, [&manager] {
            return manager.SubmitCommand({Core::AAP::AAPCommandKind::SetAdaptiveNoiseLevel, 70});
        });
        QVERIFY(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(result.get().accepted);
        QVERIFY(oldTransport->WaitForSendWaiter(std::chrono::seconds{1}));

        // Retire generation 9 while its write is blocked. The attempt-bound cancellation must
        // release the old send before a generation-10 transport is installed.
        manager.Disconnect();
        QVERIFY(settledFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(settledFuture.get().settlement, Core::AAP::AAPCommandSettlement::TransportClosed);
        QVERIFY(oldTransport->SentRecords().empty());
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        {
            std::lock_guard<std::mutex> lock{manager._mutex};
            manager._attempt = Core::AAP::AAPAttempt{1, 10};
            manager._transport.store(newTransport);
            manager._connected = true;
            manager._disconnecting = false;
            manager._sessionStop = std::stop_source{};
            manager._connectStop = std::stop_source{};
            manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        }
        manager._commandBroker->BeginSession(10);
        QCOMPARE(newTransport->SendCalls(), size_t{0});
        QVERIFY(
            manager.SubmitCommand({Core::AAP::AAPCommandKind::SetAdaptiveNoiseLevel, 40}).accepted);
        QVERIFY(newTransport->WaitForRecordCount(1, std::chrono::seconds{1}));
        const auto records = newTransport->SentRecords();
        QCOMPARE(records.size(), size_t{1});
        QCOMPARE(records.front().at(7), uint8_t{40});
        QVERIFY(oldTransport->SentRecords().empty());
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 40, 0x00, 0x00, 0x00});
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager.Disconnect();
    }

    void AdaptiveNoiseStrengthBrokerEchoConfirmsCurrentSessionOnly()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 11};
        manager._connected = true;
        manager._noiseControlMode = Core::AAP::NoiseControlMode::Off;
        manager._commandBroker->BeginSession(manager._attempt.generation);

        std::promise<Core::AAP::AAPCommandOutcome> settled;
        auto settledFuture = settled.get_future();
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        const Core::AAP::AAPCommand command{Core::AAP::AAPCommandKind::SetAdaptiveNoiseLevel, 65};

        // Settings still record desired state while Off, but do not queue a write or manufacture
        // a rejection. The later Adaptive notification/submission owns the wire command.
        const auto deferred = manager.SubmitCommand(command);
        QVERIFY(!deferred.accepted);
        QVERIFY(!deferred.failure.has_value());
        QCOMPARE(manager.DesiredCommand(command.kind), std::optional<uint32_t>{65});
        QCOMPARE(manager._commandBroker->QueuedCount(), size_t{0});
        QCOMPARE(transport->SendCalls(), size_t{0});

        // A 0x2E record outside reported Adaptive mode is stale/misrouted evidence.
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 65, 0x00, 0x00, 0x00});
        QVERIFY(!manager.ConfirmedCommand(command.kind).has_value());
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        QVERIFY(manager.SubmitCommand(command).accepted);
        // WaitForSendWaiter observes an instantaneous gauge that FakeTransport only keeps
        // non-zero while a send is parked; without SetBlockSend the write is long gone by
        // the time this thread waits. Waiting for the record proves the write landed.
        QVERIFY(transport->WaitForRecordCount(1, std::chrono::seconds{1}));
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 65, 0x00, 0x00, 0x00});
        QVERIFY(settledFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto outcome = settledFuture.get();
        QCOMPARE(outcome.settlement, Core::AAP::AAPCommandSettlement::Confirmed);
        QCOMPARE(manager.ConfirmedCommand(command.kind), std::optional<uint32_t>{65});
        QCOMPARE(manager.GetAdaptiveNoiseLevel(), std::optional<uint8_t>{65});
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager.Disconnect();
    }

    void AdaptiveNoiseStrengthCacheClearsOnNaturalSessionEnd()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory = [transport](Core::AAP::AAPTransportKind) {
            return std::static_pointer_cast<Core::AAP::AAPTransport>(transport);
        };
        const Core::AAP::AAPAttempt attempt{1, 8};
        QVERIFY(manager.Connect(attempt));
        QVERIFY(transport->WaitForReceiveWaiter(std::chrono::seconds{1}));
        manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        QVERIFY(manager.SetAdaptiveNoiseLevel(60));
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());
        QVERIFY(transport->WaitForRecordCount(6, std::chrono::seconds{1}));
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 60, 0x00, 0x00, 0x00});
        QCOMPARE(manager.GetAdaptiveNoiseLevel(), std::optional<uint8_t>{60});

        transport->SetEof();
        QTRY_VERIFY(!manager.IsConnected());
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());
        QVERIFY(!manager.GetNoiseControlMode().has_value());
        manager.Disconnect();
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

        // DriverOpen records are attempt-tagged since the production BeginAttempt wiring was
        // restored; this test drives ConnectTransport directly, so it opens generation 1
        // itself and closes it right after the report copy is taken.
        Core::Diagnostics::Recorder::Instance().Reset();
        Core::Diagnostics::Recorder::Instance().BeginAttempt(1);
        ConnectFailure failure = ConnectFailure::None;
        const auto transport = manager.ConnectTransport({1, 1}, {}, failure);
        const auto report = Core::Diagnostics::Recorder::Instance().BuildReport({});
        Core::Diagnostics::Recorder::Instance().EndAttempt(1);
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

        // Same attempt-tagged DriverOpen contract as the bind-status table above.
        Core::Diagnostics::Recorder::Instance().Reset();
        Core::Diagnostics::Recorder::Instance().BeginAttempt(1);
        Core::AAP::ConnectFailure failure = Core::AAP::ConnectFailure::None;
        const auto transport = manager.ConnectTransport({1, 1}, {}, failure);
        const auto report = Core::Diagnostics::Recorder::Instance().BuildReport({});
        Core::Diagnostics::Recorder::Instance().EndAttempt(1);
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

        // Handshake, raw stem config, feature flags, notification subscriptions, then one ATT
        // read.
        QCOMPARE(transport->SendCalls(), size_t{5});
        const auto records = transport->SentRecords();
        QCOMPARE(records.at(0), Core::AAP::Packets::Handshake);
        QCOMPARE(
            records.at(1), (std::vector<uint8_t>{
                               0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x39, 0x0F, 0x00, 0x00, 0x00}));
        QCOMPARE(records.at(2), Core::AAP::Packets::EnableFeatures);
        QCOMPARE(records.at(3), Core::AAP::Packets::RequestNotifications);
        QCOMPARE(transport->LastSend(), (std::vector<uint8_t>{0x0A, 0x1B, 0x00}));
        QVERIFY(manager._readerThread.joinable());

        manager.Disconnect();
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._readerThread.joinable());
    }

    void StemConfigSendsOnChangeAndReplaysOnReconnect()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [transport](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return transport;
        };

        QVERIFY(manager.SetStemConfig(0x05)); // Saved before the transport exists.
        QVERIFY(manager.Connect({0x1234, 1}));
        QCOMPARE(transport->SentRecords().at(1).at(7), uint8_t{0x05});
        QVERIFY(manager.SetStemConfig(0x0A));
        QCOMPARE(transport->LastSend().at(7), uint8_t{0x0A});
        QCOMPARE(transport->SendCalls(), size_t{6});
        manager.Disconnect();

        QVERIFY(manager.Connect({0x1234, 2}));
        const auto records = transport->SentRecords();
        QCOMPARE(records.size(), size_t{11});
        QCOMPARE(records.at(6), Core::AAP::Packets::Handshake);
        QCOMPARE(records.at(7).at(7), uint8_t{0x0A});
        manager.Disconnect();
    }

    void FailedStemConfigWriteRemainsPendingForReconnect()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [transport](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return transport;
        };
        QVERIFY(manager.Connect({0x1234, 1}));
        transport->SetSendError(true);
        QVERIFY(!manager.SetStemConfig(0x03));
        transport->SetSendError(false);
        manager.Disconnect();

        QVERIFY(manager.Connect({0x1234, 2}));
        const auto records = transport->SentRecords();
        QCOMPARE(records.at(6).at(7), uint8_t{0x03});
        manager.Disconnect();
    }

    void FailedStemReplayRecoversInNewSessionAfterPriorSuccess()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [transport](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return transport;
        };
        QVERIFY(manager.SetStemConfig(0x05));
        QVERIFY(manager.Connect({0x1234, 1}));
        manager.Disconnect();

        // Generation 2 must replay 0x39 even though generation 1 sent the same revision. The
        // first replay write fails; the bounded post-handshake recovery must retry on this
        // transport.
        manager._beforeStemReplayForTest = [&] { transport->SetSendFailures(1); };
        QVERIFY(manager.Connect({0x1234, 2}));
        QCOMPARE(transport->SendCalls(), size_t{11});
        const auto records = transport->SentRecords();
        QCOMPARE(records.size(), size_t{10});
        QCOMPARE(records.at(5), Core::AAP::Packets::Handshake);
        QCOMPARE(records.at(9).at(4), uint8_t{0x09});
        QCOMPARE(records.at(9).at(6), uint8_t{0x39});
        QCOMPARE(records.at(9).at(7), uint8_t{0x05});
        manager.Disconnect();
    }

    void FailedStemReplayIsBoundedAndRetriedByNextSession()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transportFactory =
            [transport](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            return transport;
        };
        QVERIFY(manager.SetStemConfig(0x0A));
        int replayCalls = 0;
        manager._beforeStemReplayForTest = [&] {
            if (++replayCalls == 1) {
                transport->SetSendFailures(2);
            }
        };
        QVERIFY(manager.Connect({0x1234, 1}));
        // Handshake, two failed 0x39 attempts, features, notifications, ATT probe. No third
        // retry.
        QCOMPARE(transport->SendCalls(), size_t{6});
        QCOMPARE(transport->SentRecords().size(), size_t{4});
        manager.Disconnect();

        QVERIFY(manager.Connect({0x1234, 2}));
        QCOMPARE(transport->SendCalls(), size_t{11});
        const auto records = transport->SentRecords();
        QCOMPARE(records.at(5).at(6), uint8_t{0x39});
        QCOMPARE(records.at(5).at(7), uint8_t{0x0A});
        manager.Disconnect();
    }

    void AttemptBoundNoiseControlRevalidatesAttemptAndCancellation()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = {0x1234, 7};
        manager._connected = true;
        manager._commandBroker->BeginSession(7);
        manager._commandBroker->SetConfirmMode(
            Core::AAP::AAPCommandKind::SetNoiseControl, Core::AAP::AAPConfirmMode::Unverifiable);
        constexpr Core::AAP::NoiseControlMode mode = Core::AAP::NoiseControlMode::NoiseCancellation;

        QVERIFY(!manager.SetNoiseControlMode(mode, {0x1234, 8}, {}));
        QVERIFY(!manager.SetNoiseControlMode(mode, {0x5678, 7}, {}));
        manager._connected = false;
        QVERIFY(!manager.SetNoiseControlMode(mode, {0x1234, 7}, {}));
        manager._connected = true;
        manager._cancelledGeneration = 7;
        QVERIFY(!manager.SetNoiseControlMode(mode, {0x1234, 7}, {}));
        manager._cancelledGeneration = 0;
        std::stop_source stop;
        stop.request_stop();
        QVERIFY(!manager.SetNoiseControlMode(mode, {0x1234, 7}, stop.get_token()));
        QCOMPARE(transport->SendCalls(), size_t{0});

        QVERIFY(manager.SetNoiseControlMode(mode, {0x1234, 7}, {}));
        QVERIFY(transport->WaitForRecordCount(1, std::chrono::seconds{1}));
        QCOMPARE(
            transport->LastSend(),
            (std::vector<uint8_t>{
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00}));
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager._commandBroker->EndSession();
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void AttemptBoundNoiseControlRechecksGenerationAcrossReconnectBarrier()
    {
        Core::AAP::Manager manager;
        auto original = std::make_shared<AAPTestSupport::FakeTransport>();
        auto replacement = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transport.store(original);
        manager._attempt = {0x1234, 7};
        manager._connected = true;
        manager._commandBroker->BeginSession(7);
        manager._commandBroker->SetConfirmMode(
            Core::AAP::AAPCommandKind::SetNoiseControl, Core::AAP::AAPConfirmMode::Unverifiable);

        AAPTestSupport::PromiseGate entered;
        AAPTestSupport::PromiseGate release;
        AAPTestSupport::ScopedUnblock releaseGuard{release};
        manager._beforeAttemptNoiseControlLockForTest = [&] {
            entered.Signal();
            release.Wait(AAPTestSupport::kRendezvousTimeout);
        };

        // Hold the send gate after the queued request has been validated by its caller. The
        // worker reaches the lock barrier before the replacement is installed.
        std::unique_lock barrier{manager._mutex};
        auto result = std::async(std::launch::async, [&] {
            return manager.SetNoiseControlMode(
                Core::AAP::NoiseControlMode::NoiseCancellation, {0x1234, 7}, {});
        });
        QVERIFY(entered.Wait(std::chrono::seconds{2}));
        manager._attempt = {0x1234, 8};
        manager._transport.store(replacement);
        barrier.unlock();
        release.Signal();

        QVERIFY(!result.get());
        QCOMPARE(original->SendCalls(), size_t{0});
        QCOMPARE(replacement->SendCalls(), size_t{0});
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager._commandBroker->EndSession();
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void AttemptBoundNoiseControlRechecksCancellationAtSendBarrier()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transport.store(transport);
        manager._attempt = {0x1234, 7};
        manager._connected = true;
        manager._commandBroker->BeginSession(7);
        manager._commandBroker->SetConfirmMode(
            Core::AAP::AAPCommandKind::SetNoiseControl, Core::AAP::AAPConfirmMode::Unverifiable);

        AAPTestSupport::PromiseGate entered;
        AAPTestSupport::PromiseGate release;
        AAPTestSupport::ScopedUnblock releaseGuard{release};
        manager._beforeAttemptNoiseControlLockForTest = [&] {
            entered.Signal();
            release.Wait(AAPTestSupport::kRendezvousTimeout);
        };

        std::unique_lock barrier{manager._mutex};
        auto result = std::async(std::launch::async, [&] {
            return manager.SetNoiseControlMode(
                Core::AAP::NoiseControlMode::NoiseCancellation, {0x1234, 7}, {});
        });
        QVERIFY(entered.Wait(std::chrono::seconds{2}));
        manager._cancelledGeneration = 7;
        barrier.unlock();
        release.Signal();

        QVERIFY(!result.get());
        QCOMPARE(transport->SendCalls(), size_t{0});
        manager._commandBroker->EndSession();
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void AttemptBoundNoiseControlPinsTransportDuringWrite()
    {
        Core::AAP::Manager manager;
        auto original = std::make_shared<AAPTestSupport::FakeTransport>();
        auto replacement = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(original->Connect(1, {}));
        QVERIFY(replacement->Connect(1, {}));
        original->SetBlockSend(true);
        manager._transport.store(original);
        manager._attempt = {0x1234, 7};
        manager._connected = true;
        manager._commandBroker->BeginSession(7);
        manager._commandBroker->SetConfirmMode(
            Core::AAP::AAPCommandKind::SetNoiseControl, Core::AAP::AAPConfirmMode::Unverifiable);

        auto result = std::async(std::launch::async, [&] {
            return manager.SetNoiseControlMode(
                Core::AAP::NoiseControlMode::NoiseCancellation, {0x1234, 7}, {});
        });
        const bool reachedSendBarrier = original->WaitForSendWaiter(std::chrono::seconds{1});
        if (reachedSendBarrier) {
            // Replacement after the identity gate must not redirect an in-flight write.
            manager._attempt = {0x1234, 8};
            manager._transport.store(replacement);
        }
        original->SetBlockSend(false);
        QVERIFY(reachedSendBarrier);
        QVERIFY(result.get());
        QCOMPARE(original->SendCalls(), size_t{1});
        QCOMPARE(original->LastSend().at(6), uint8_t{0x0D});
        QCOMPARE(replacement->SendCalls(), size_t{0});
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager._commandBroker->EndSession();
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void ManagerRejectsStaleStemAttemptIdentity()
    {
        Core::AAP::Manager manager;
        manager._attempt = {0x1234, 7};
        manager._connected = true;
        QVERIFY(manager.IsCurrentAttempt({0x1234, 7}));
        QVERIFY(!manager.IsCurrentAttempt({0x1234, 8}));
        QVERIFY(!manager.IsCurrentAttempt({0x5678, 7}));
        manager._connected = false;
        QVERIFY(!manager.IsCurrentAttempt({0x1234, 7}));
    }

    void ManagerRetriesFailedHeadTrackingStartToConfirmation()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 11};
        manager._connected = true;
        manager._commandBroker->BeginSession(11);
        transport->SetSendFailures(2);

        std::promise<AAPCommandOutcome> settled;
        auto settledFuture = settled.get_future();
        Callbacks callbacks;
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &outcome, const AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        QVERIFY(transport->WaitForRecordCount(1, std::chrono::seconds{1}));
        QCOMPARE(transport->SendAttempts(), size_t{3});
        QCOMPARE(transport->SendCalls(), size_t{3});
        QVERIFY(!manager.IsHeadTrackingActive());

        std::vector<uint8_t> sensor(70, 0);
        const std::array<uint8_t, 10> prefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                             0x00, 0x00, 0x00, 0x10, 0x00};
        std::copy(prefix.begin(), prefix.end(), sensor.begin());
        sensor[10] = 0x44;
        manager.ProcessPacket(sensor);
        QVERIFY(settledFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto outcome = settledFuture.get();
        QCOMPARE(outcome.generation, uint64_t{11});
        QCOMPARE(outcome.settlement, AAPCommandSettlement::Confirmed);
        QVERIFY(manager.IsHeadTrackingActive());
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void HeadTrackingStreamRecordConfirmsDesiredStartOnlyOnce()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 41};
        manager._connected = true;
        manager._commandBroker->BeginSession(41);

        std::atomic<int> dataCallbacks{0};
        std::atomic<int> settlements{0};
        Callbacks callbacks;
        callbacks.onHeadTrackingData = [&](const HeadTrackingData &, const AAPAttempt &) {
            ++dataCallbacks;
        };
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &, const AAPAttempt &) {
            ++settlements;
        };
        manager.SetCallbacks(std::move(callbacks));

        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        QVERIFY(transport->WaitForRecordCount(1, std::chrono::seconds{1}));
        std::vector<uint8_t> sensor(70, 0);
        const std::array<uint8_t, 10> prefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                             0x00, 0x00, 0x00, 0x10, 0x00};
        std::copy(prefix.begin(), prefix.end(), sensor.begin());
        sensor[10] = 0x44;
        manager.ProcessPacket(sensor);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QTRY_VERIFY(manager.IsHeadTrackingActive());
        QTRY_COMPARE(settlements.load(), 1);
        QCOMPARE(dataCallbacks.load(), 1);

        manager.ProcessPacket(sensor);
        QVERIFY(manager.IsHeadTrackingActive());
        QCOMPARE(dataCallbacks.load(), 2);
        QCOMPARE(settlements.load(), 1);

        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void HeadTrackingStreamConfirmsStartQueuedBehindAnotherFlight()
    {
        using namespace Core::AAP;
        std::promise<AAPCommandSettlement> headSettlement;
        auto headFuture = headSettlement.get_future();
        std::once_flag headOnce;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->SetBlockSend(true);
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 42};
        manager._connected = true;
        manager._commandBroker->BeginSession(42);
        manager._commandBroker->SetConfirmMode(
            AAPCommandKind::SetNoiseControl, AAPConfirmMode::Unverifiable);

        Callbacks callbacks;
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &outcome, const AAPAttempt &) {
            if (outcome.command.kind == AAPCommandKind::SetHeadTracking) {
                std::call_once(headOnce, [&] { headSettlement.set_value(outcome.settlement); });
            }
        };
        manager.SetCallbacks(std::move(callbacks));

        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetNoiseControl, 0x01}).accepted);
        QVERIFY(transport->WaitForSendWaiter(std::chrono::seconds{1}));
        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        QCOMPARE(manager._commandBroker->QueuedCount(), size_t{1});

        std::vector<uint8_t> sensor(70, 0);
        const std::array<uint8_t, 10> prefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                             0x00, 0x00, 0x00, 0x10, 0x00};
        std::copy(prefix.begin(), prefix.end(), sensor.begin());
        sensor[10] = 0x44;
        manager.ProcessPacket(sensor);
        QVERIFY(manager.IsHeadTrackingActive());

        transport->SetBlockSend(false);
        QVERIFY(transport->WaitForRecordCount(2, std::chrono::seconds{1}));
        manager.ProcessPacket(sensor);
        QVERIFY(headFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(headFuture.get(), AAPCommandSettlement::Confirmed);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));

        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void LateHeadTrackingRecordDoesNotReviveStoppedStream()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 41};
        manager._connected = true;
        manager._commandBroker->BeginSession(41);
        manager._commandBroker->SetConfirmMode(
            AAPCommandKind::SetHeadTracking, AAPConfirmMode::Unverifiable);

        std::atomic<int> dataCallbacks{0};
        std::atomic<int> settlements{0};
        Callbacks callbacks;
        callbacks.onHeadTrackingData = [&](const HeadTrackingData &, const AAPAttempt &) {
            ++dataCallbacks;
        };
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &, const AAPAttempt &) {
            ++settlements;
        };
        manager.SetCallbacks(std::move(callbacks));

        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QTRY_COMPARE(settlements.load(), 1);
        QVERIFY(manager.IsHeadTrackingActive());
        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x00}).accepted);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QTRY_COMPARE(settlements.load(), 2);
        QVERIFY(!manager.IsHeadTrackingActive());
        dataCallbacks = 0;
        settlements = 0;

        std::vector<uint8_t> sensor(70, 0);
        const std::array<uint8_t, 10> prefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                             0x00, 0x00, 0x00, 0x10, 0x00};
        std::copy(prefix.begin(), prefix.end(), sensor.begin());
        sensor[10] = 0x44;
        manager.ProcessPacket(sensor);

        QVERIFY(!manager.IsHeadTrackingActive());
        QCOMPARE(dataCallbacks.load(), 0);
        QCOMPARE(settlements.load(), 0);
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void OfflineStopCancelsStartQueuedByReconnectReplay()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->SetBlockSend(true);
        manager._transport.store(transport);
        manager._commandBroker->SetConfirmMode(
            AAPCommandKind::SetHeadTracking, AAPConfirmMode::Unverifiable);

        const auto offlineStart = manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x01});
        QVERIFY(!offlineStart.accepted);
        manager._attempt = AAPAttempt{1, 21};
        manager._connected = true;
        manager._commandBroker->BeginSession(21);
        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetNoiseControl, 0x01}).accepted);
        manager._commandBroker->ReplayDesiredState(21);
        QVERIFY(transport->WaitForSendWaiter(std::chrono::seconds{1}));

        // Replay opens generation 22 before the transport owner has retired the old attempt.
        // During this window the head-tracking intent still takes its offline submission path.
        {
            std::lock_guard lock{manager._mutex};
            manager._attempt = AAPAttempt{1, 22};
        }
        manager._commandBroker->ReplayDesiredState(22);
        QCOMPARE(manager._commandBroker->QueuedCount(), size_t{2});
        {
            std::lock_guard lock{manager._mutex};
            manager._connected = false;
        }
        const auto offlineStop = manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x00});
        QVERIFY(!offlineStop.accepted);
        QCOMPARE(offlineStop.failure, std::optional(AAPCommandFailure::TransportClosed));
        QCOMPARE(manager._commandBroker->QueuedCount(), size_t{1});

        transport->SetBlockSend(false);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 23};
        manager._connected = true;
        manager._commandBroker->BeginSession(23);
        manager._commandBroker->ReplayDesiredState(23);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));

        size_t headTrackingRecords = 0;
        for (const auto &record : transport->SentRecords()) {
            if (record == Packets::StartHeadTracking || record == Packets::StopHeadTracking) {
                ++headTrackingRecords;
                QCOMPARE(record, Packets::StopHeadTracking);
            }
        }
        QCOMPARE(headTrackingRecords, size_t{1});
        QCOMPARE(
            manager.DesiredCommand(AAPCommandKind::SetHeadTracking), std::optional<uint32_t>{0});
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void ManagerReconnectReplaysDesiredStop()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._commandBroker->SetConfirmMode(
            AAPCommandKind::SetHeadTracking, AAPConfirmMode::Unverifiable);
        const auto offline = manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0x00});
        QVERIFY(!offline.accepted);
        QCOMPARE(offline.failure, std::optional(AAPCommandFailure::TransportClosed));
        QCOMPARE(
            manager.DesiredCommand(AAPCommandKind::SetHeadTracking), std::optional<uint32_t>{0});

        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 21};
        manager._connected = true;
        manager._commandBroker->BeginSession(21);
        manager._commandBroker->ReplayDesiredState(21);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));

        manager._commandBroker->EndSession();
        manager._attempt = AAPAttempt{1, 22};
        manager._commandBroker->BeginSession(22);
        manager._commandBroker->ReplayDesiredState(22);
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        const auto records = transport->SentRecords();
        QCOMPARE(records.size(), size_t{2});
        QCOMPARE(records[0], Packets::StopHeadTracking);
        QCOMPARE(records[1], Packets::StopHeadTracking);
        QCOMPARE(
            manager.DesiredCommand(AAPCommandKind::SetHeadTracking), std::optional<uint32_t>{0});
        QVERIFY(!manager.IsHeadTrackingActive());
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void ConcurrentHeadTrackingToggleAndReplayUseOneBroker()
    {
        using namespace Core::AAP;
        Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = AAPAttempt{1, 31};
        manager._connected = true;
        manager._commandBroker->SetConfirmMode(
            AAPCommandKind::SetHeadTracking, AAPConfirmMode::Unverifiable);
        manager._commandBroker->BeginSession(31);

        std::atomic<uint32_t> lastDesired{0};
        std::jthread toggles{[&] {
            for (uint32_t index = 0; index < 200; ++index) {
                const uint32_t value = index % 2;
                lastDesired.store(value);
                manager.SubmitCommand({AAPCommandKind::SetHeadTracking, value});
            }
        }};
        std::jthread reconnects{[&] {
            for (uint64_t generation = 32; generation <= 40; ++generation) {
                {
                    std::lock_guard lock{manager._mutex};
                    manager._attempt = AAPAttempt{1, generation};
                }
                manager._commandBroker->EndSession();
                manager._commandBroker->BeginSession(generation);
                manager._commandBroker->ReplayDesiredState(generation);
            }
        }};
        toggles.join();
        reconnects.join();
        QVERIFY(manager.WaitForCommandsIdle(std::chrono::seconds{1}));
        QCOMPARE(
            manager.DesiredCommand(AAPCommandKind::SetHeadTracking),
            std::optional<uint32_t>{lastDesired.load()});
        const auto records = transport->SentRecords();
        QVERIFY(!records.empty());
        for (const auto &record : records) {
            QVERIFY(record == Packets::StartHeadTracking || record == Packets::StopHeadTracking);
        }
        QCOMPARE(transport->SendAttempts(), records.size());
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
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
        QCOMPARE(rebound->SendCalls(), size_t{5});

        auto disconnectRebound = std::async(std::launch::async, [&] { manager.Disconnect(); });
        QVERIFY(disconnectRebound.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        disconnectRebound.get();
        QVERIFY(!manager.IsConnected());
    }

    void CancelConnectBeforeAttemptStartsSuppressesThatGeneration()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        std::atomic<int> factoryCalls{0};
        manager._transportFactory =
            [&](Core::AAP::AAPTransportKind) -> std::shared_ptr<Core::AAP::AAPTransport> {
            ++factoryCalls;
            return transport;
        };
        manager._attempt = Core::AAP::AAPAttempt{1, 7};
        manager.CancelConnect();
        QCOMPARE(manager._cancelledGeneration.load(), uint64_t{7});
        manager._attempt = {};

        // The pre-entry generation gate must win before the lifecycle mutex, factory,
        // pending-publication path, or any transport operation is reached.
        const Core::AAP::AAPAttempt attempt{1, 7};
        QVERIFY(!manager.Connect(attempt));
        QCOMPARE(factoryCalls.load(), 0);
        QCOMPARE(transport->ConnectCalls(), size_t{0});
        QCOMPARE(transport->SendCalls(), size_t{0});
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(!manager.IsConnected());
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

    // D2: routing of the battery notification through the production packet path.
    void BatteryNotificationReachesCallbackAndCache()
    {
        Core::AAP::Manager manager;
        int callbacks = 0;
        std::optional<Core::AAP::AAPBatterySnapshot> lastSnapshot;
        Core::AAP::Callbacks cb;
        cb.onBatteryChanged = [&](const Core::AAP::AAPBatterySnapshot &snapshot,
                                  const Core::AAP::AAPAttempt &attempt) {
            QCOMPARE(attempt.address, uint64_t{1});
            QCOMPARE(attempt.generation, uint64_t{1});
            lastSnapshot = snapshot;
            ++callbacks;
        };
        manager.SetCallbacks(cb);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        const std::vector<uint8_t> frame{0x04, 0x00, 0x04, 0x00, 0x04, 0x00,
                                         0x01, 0x04, 0x01, 87,   0x02, 0x01};
        manager.ProcessPacket(frame);
        QCOMPARE(callbacks, 1);
        QVERIFY(lastSnapshot.has_value());
        QCOMPARE(lastSnapshot->left->level, uint8_t(87));
        auto cached = manager.GetBatterySnapshot();
        QVERIFY(cached.has_value());
        QCOMPARE(cached->left->level, uint8_t(87));

        // A malformed record (dead spacer) passes the transport minimum but fails the
        // strict parser: neither the cache nor the consumer may move.
        std::vector<uint8_t> badSpacer = frame;
        badSpacer[8] = 0x00;
        manager.ProcessPacket(badSpacer);
        QCOMPARE(callbacks, 1);
        QCOMPARE(manager.GetBatterySnapshot()->left->level, uint8_t(87));

        // Count/size mismatches are rejected the same way (count=2 with 12 bytes).
        const std::vector<uint8_t> mismatched{0x04, 0x00, 0x04, 0x00, 0x04, 0x00,
                                              0x02, 0x04, 0x01, 50,   0x02, 0x01};
        manager.ProcessPacket(mismatched);
        QCOMPARE(callbacks, 1);
    }

    void StaleGenerationBatteryEventsAreDiscarded()
    {
        Core::AAP::Manager manager;
        int callbacks = 0;
        Core::AAP::Callbacks cb;
        cb.onBatteryChanged = [&](const Core::AAP::AAPBatterySnapshot &,
                                  const Core::AAP::AAPAttempt &attempt) {
            QCOMPARE(attempt.generation, uint64_t{2});
            ++callbacks;
        };
        manager.SetCallbacks(cb);
        manager._attempt = Core::AAP::AAPAttempt{1, 2};
        manager._connected = true;

        const std::vector<uint8_t> frame{0x04, 0x00, 0x04, 0x00, 0x04, 0x00,
                                         0x01, 0x08, 0x01, 50,   0x02, 0x01};
        // The reader of the retired generation must not publish through the new session.
        manager.ProcessPacket(frame, Core::AAP::AAPAttempt{1, 1}, cb);
        QCOMPARE(callbacks, 0);
        QVERIFY(!manager.GetBatterySnapshot().has_value());

        manager.ProcessPacket(frame, Core::AAP::AAPAttempt{1, 2}, cb);
        QCOMPARE(callbacks, 1);
        QVERIFY(manager.GetBatterySnapshot().has_value());
    }

    void BatteryCacheClearedOnDisconnect()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        const std::vector<uint8_t> frame{0x04, 0x00, 0x04, 0x00, 0x04, 0x00,
                                         0x01, 0x04, 0x01, 66,   0x01, 0x01};
        manager.ProcessPacket(frame);
        QVERIFY(manager.GetBatterySnapshot().has_value());

        // AAP disconnect: the cached session snapshot is gone, so no stale overlay
        // can outlive the connection that produced it.
        manager.Disconnect();
        QVERIFY(!manager.GetBatterySnapshot().has_value());
    }

    // Blueprint 2.1: the 0x001D notification feeds a generation-scoped cache and
    // a payload-free callback tagged only with the current session identity.
    void DeviceInfoRecordCachesAndNotifiesWithGeneration()
    {
        Core::AAP::Manager manager;
        int calls = 0;
        Core::AAP::AAPAttempt lastAttempt{};
        Core::AAP::Callbacks callbacks;
        callbacks.onDeviceInfoChanged = [&](Core::AAP::AAPAttempt attempt) {
            ++calls;
            lastAttempt = attempt;
        };
        manager.SetCallbacks(std::move(callbacks));
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        const auto sample = DeviceInfoSampleBytes();
        QCOMPARE(sample.size(), size_t{222});
        manager.ProcessPacket(sample);

        QCOMPARE(calls, 1);
        const auto cached = manager.GetDeviceInfo();
        QVERIFY(cached.has_value());
        QCOMPARE(cached->serial, std::string{"QXNRHHYXP6"});
        QCOMPARE(lastAttempt.address, uint64_t{1});
        QCOMPARE(lastAttempt.generation, uint64_t{1});
    }

    // A rejected record must keep the previous cache intact (the UI keeps showing
    // the last trusted read instead of flickering to empty).
    void MalformedDeviceInfoKeepsPreviousCache()
    {
        Core::AAP::Manager manager;
        int calls = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onDeviceInfoChanged = [&calls](Core::AAP::AAPAttempt) { ++calls; };
        manager.SetCallbacks(std::move(callbacks));
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        const auto valid = BuildDeviceInfoWithSerial("GOODSERIAL");
        manager.ProcessPacket(valid);
        QCOMPARE(calls, 1);
        QVERIFY(manager.GetDeviceInfo().has_value());
        QCOMPARE(manager.GetDeviceInfo()->serial, std::string{"GOODSERIAL"});

        // Passes the transport gate (correct header/opcode, >= 11 bytes) but the
        // serial carries invalid UTF-8: "BAD" + 0xC3 + 0x28 + "SERIAL".
        auto corrupted = BuildDeviceInfoWithSerial("BAD"
                                                   "\xc3"
                                                   "\x28"
                                                   "SERIAL");
        manager.ProcessPacket(corrupted);
        QCOMPARE(calls, 1);
        QCOMPARE(manager.GetDeviceInfo()->serial, std::string{"GOODSERIAL"});

        // Removing the eleventh field's terminator is truncation, even though
        // all nine fields surfaced by the card are still present.
        auto missingEleventh = BuildDeviceInfoWithSerial("OTHER");
        missingEleventh.pop_back();
        manager.ProcessPacket(missingEleventh);
        QCOMPARE(calls, 1);
        QCOMPARE(manager.GetDeviceInfo()->serial, std::string{"GOODSERIAL"});

        // Wrong opcode behind an otherwise valid record: rejected before the
        // cache is written, previous value survives.
        auto wrongOpcode = BuildDeviceInfoWithSerial("OTHER");
        wrongOpcode[4] = 0x1E;
        manager.ProcessPacket(wrongOpcode);
        QCOMPARE(calls, 1);
        QCOMPARE(manager.GetDeviceInfo()->serial, std::string{"GOODSERIAL"});
    }

    // Packets attributed to a superseded attempt never touch the cache nor the
    // callback (reader-thread races against reconnect are dropped by generation).
    void StaleAttemptDeviceInfoIsIgnored()
    {
        Core::AAP::Manager manager;
        int calls = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onDeviceInfoChanged = [&calls](Core::AAP::AAPAttempt) { ++calls; };
        manager.SetCallbacks(std::move(callbacks));
        manager._attempt = Core::AAP::AAPAttempt{1, 5};
        manager._connected = true;

        manager.ProcessPacket(DeviceInfoSampleBytes(), Core::AAP::AAPAttempt{1, 4}, callbacks);
        QCOMPARE(calls, 0);
        QVERIFY(!manager.GetDeviceInfo().has_value());
    }

    // Disconnect is the fixed teardown used by rebind as well as an explicit
    // disconnect. It must drop cached device information between sessions.
    void DisconnectClearsDeviceInfoBetweenSessions()
    {
        Core::AAP::Manager manager;
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager.ProcessPacket(BuildDeviceInfoWithSerial("SESSIONA"));
        QVERIFY(manager.GetDeviceInfo().has_value());

        manager.Disconnect();
        QVERIFY(!manager.GetDeviceInfo().has_value());

        // A later session can cache its own record without inheriting stale
        // values. Rebinding uses this same teardown before changing address.
        manager._attempt = Core::AAP::AAPAttempt{2, 2};
        manager._connected = true;
        manager.ProcessPacket(BuildDeviceInfoWithSerial("SESSIONB"));
        QVERIFY(manager.GetDeviceInfo().has_value());
        QCOMPARE(manager.GetDeviceInfo()->serial, std::string{"SESSIONB"});

        manager.Disconnect();
        QVERIFY(!manager.GetDeviceInfo().has_value());

        manager._attempt = Core::AAP::AAPAttempt{2, 3};
        manager._connected = true;
        manager.ProcessPacket(BuildDeviceInfoWithSerial("SESSIONC"));
        QVERIFY(manager.GetDeviceInfo().has_value());
        QCOMPARE(manager.GetDeviceInfo()->serial, std::string{"SESSIONC"});
        manager.Disconnect();
    }
    // Reset happens before the new attempt records AdvertisementMatched, so the report starts
    // with this attempt's evidence and never inherits an older session's lines. Restored from
    // the P3 batch after a merge resolution dropped it together with the production wiring.
    void BeginAttemptResetsBeforeRecordingAdvertisementMatched()
    {
        constexpr uint64_t kOldGeneration = 4304;
        constexpr uint64_t kFreshGeneration = 4305;
        auto &recorder = Core::Diagnostics::Recorder::Instance();
        recorder.BeginAttempt(kOldGeneration);
        recorder.RecordForAttempt(
            kOldGeneration, Core::Diagnostics::Step::AdvertisementMatched, true, "old attempt");
        recorder.EndAttempt(kOldGeneration);

        // These untagged events must really be accepted before the new attempt clears them.
        recorder.Record(Core::Diagnostics::Step::ScannerStarted, true);
        recorder.Record(
            Core::Diagnostics::Step::AdvertisementMatched, true, "old preconnect event");
        QCOMPARE(recorder.StepCount(), size_t{3});

        recorder.BeginAttempt(kFreshGeneration);
        QCOMPARE(recorder.StepCount(), size_t{0});
        const auto masked = Core::Diagnostics::Recorder::MaskAddress(0x1122334455667788ULL);
        recorder.RecordForAttempt(
            kFreshGeneration, Core::Diagnostics::Step::AdvertisementMatched, true,
            "address=" + masked);
        recorder.RecordForAttempt(
            kFreshGeneration, Core::Diagnostics::Step::TransportConnect, false,
            "MagicAAP driver unavailable");

        const auto report = QString::fromStdString(recorder.BuildReport({}));
        QVERIFY(!report.contains("scanner_started"));
        QVERIFY(!report.contains("old attempt"));
        QVERIFY(!report.contains("old preconnect event"));
        QVERIFY(report.contains("attempt 4305"));
        QVERIFY(report.contains("advertisement_matched"));
        QVERIFY(report.indexOf("advertisement_matched") < report.indexOf("transport_connect"));
        QVERIFY(report.contains("transport_connect  FAILED  MagicAAP driver unavailable"));
        QVERIFY(!report.contains("1122334455667788"));
        recorder.EndAttempt(kFreshGeneration);
    }
#endif
};

QTEST_GUILESS_MAIN(AAPTransportTests)

#include "AAPTransportTests.moc"
