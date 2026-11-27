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
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#if defined(APD_OS_WIN)
    #include <WinSock2.h>
    #include <ws2tcpip.h>
#endif

#include <QtTest>

#include "AAPTestSupport.h"
#include "Source/Core/AAPMessage.h"
#include "Source/Core/AAPTransport.h"

#if defined(APD_OS_WIN)
    #include "Source/Core/AAPManager.h"
    #pragma comment(lib, "Ws2_32.lib")
#endif

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
#endif

class AAPTransportTests : public QObject
{
    Q_OBJECT

private slots:
    void ShortWriteFailsWithoutEmittingASecondRecord()
    {
        AAPTestSupport::FakeTransport transport;
        const std::vector<uint8_t> record{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
        QVERIFY(transport.Connect(1, {}));
        transport.SetSendLimit(record.size() - 1);

        QVERIFY(!transport.Send(record, {}));
        QCOMPARE(transport.SendCalls(), size_t{1});
        QCOMPARE(transport.LastSend(), record);
    }

    void CompleteWriteEmitsExactlyOneRecord()
    {
        AAPTestSupport::FakeTransport transport;
        const std::vector<uint8_t> record{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};

        QVERIFY(transport.Connect(1, {}));
        QVERIFY(transport.Send(record, {}));
        QCOMPARE(transport.SendCalls(), size_t{1});
        QCOMPARE(transport.LastSend(), record);
    }

    void StopInterruptsBlockedReceive()
    {
        AAPTestSupport::FakeTransport transport;
        std::stop_source stop;
        auto result =
            std::async(std::launch::async, [&] { return transport.Receive(stop.get_token()); });

        QVERIFY(transport.WaitForReceiveWaiter(std::chrono::seconds(1)));
        stop.request_stop();
        QVERIFY(result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QVERIFY(!result.get().has_value());
    }

    void CloseInterruptsBlockedReceive()
    {
        AAPTestSupport::FakeTransport transport;
        auto result = std::async(std::launch::async, [&] { return transport.Receive({}); });

        QVERIFY(transport.WaitForReceiveWaiter(std::chrono::seconds(1)));
        transport.Close();
        QVERIFY(result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QVERIFY(!result.get().has_value());
    }

    void CloseDropsQueuedRecords()
    {
        AAPTestSupport::FakeTransport transport;
        transport.Push({0x04, 0x00, 0x04, 0x00, 0x2B, 0x00});
        transport.Close();

        QCOMPARE(transport.RecordCount(), size_t{0});
        QVERIFY(!transport.Receive({}).has_value());
    }

    void EofReturnsNulloptAfterQueuedRecords()
    {
        AAPTestSupport::FakeTransport transport;
        const std::vector<uint8_t> first{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
        const std::vector<uint8_t> second{0x0A, 0x1B, 0x00};
        transport.Push(first);
        transport.Push(second);
        transport.SetEof();

        QCOMPARE(transport.Receive({}), std::optional(first));
        QCOMPARE(transport.Receive({}), std::optional(second));
        QVERIFY(!transport.Receive({}).has_value());
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
        auto result = std::async(std::launch::async, [&] {
            return transport.Send(
                std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00}, stop.get_token());
        });

        QVERIFY(transport.WaitForSendWaiter(std::chrono::seconds(1)));
        stop.request_stop();
        QVERIFY(result.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QVERIFY(!result.get());
        QCOMPARE(transport.SendCalls(), size_t{0});
    }

    void EachInjectedRecordRetainsItsBoundary()
    {
        AAPTestSupport::FakeTransport transport;
        const std::vector<uint8_t> first{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
        const std::vector<uint8_t> second{0x0B, 0x01};
        transport.Push(first);
        transport.Push(second);

        QCOMPARE(transport.Receive({}), std::optional(first));
        QCOMPARE(transport.Receive({}), std::optional(second));
    }

#if defined(APD_OS_WIN)
    void SeqPacketSendDoesNotWaitForReceivePolling()
    {
        using Core::AAP::SeqPacketTransport;
        LoopbackDatagramPair pair;
        QVERIFY(pair.IsValid());
        SeqPacketTransport transport;
        transport._socket = std::make_shared<SeqPacketTransport::SocketState>(
            reinterpret_cast<void *>(pair.ReleaseTransportSocket()));
        transport._closed = false;

        const std::vector<uint8_t> record{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
        std::stop_source stop;
        auto receive =
            std::async(std::launch::async, [&] { return transport.Receive(stop.get_token()); });
        const bool receiveBlocked =
            receive.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
        auto send = std::async(std::launch::async, [&] { return transport.Send(record, {}); });
        const bool sendReady = send.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

        stop.request_stop();
        const bool receiveReady =
            receive.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        QVERIFY(receiveReady);
        QVERIFY(!receive.get().has_value());
        if (!sendReady) {
            QVERIFY(send.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        }
        QVERIFY(receiveBlocked);
        QVERIFY(sendReady);
        QVERIFY(send.get());
        QCOMPARE(pair.ReceiveOnPeer(), std::optional(record));
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
        auto receive =
            std::async(std::launch::async, [&] { return transport.Receive(stop.get_token()); });
        const bool receiveBlocked =
            receive.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
        auto close = std::async(std::launch::async, [&] { transport.Close(); });
        const bool closeReady =
            close.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

        const bool receiveClosed =
            receive.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        if (!receiveClosed) {
            stop.request_stop();
        }
        const bool receiveReady =
            receive.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        QVERIFY(receiveReady);
        QVERIFY(!receive.get().has_value());
        close.wait();
        transport.Close();
        QVERIFY(receiveBlocked);
        QVERIFY(closeReady);
        QVERIFY(receiveClosed);
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
        auto receive =
            std::async(std::launch::async, [&] { return transport.Receive(stop.get_token()); });
        const bool receiveBlocked =
            receive.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
        stop.request_stop();
        QVERIFY(receive.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QVERIFY(!receive.get().has_value());
        QVERIFY(receiveBlocked);
        QVERIFY(!transport.Receive(stop.get_token()).has_value());
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

        const std::vector<uint8_t> first{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00};
        const std::vector<uint8_t> second{0x0B, 0x01};
        QVERIFY(pair.SendFromPeer(first));
        QVERIFY(pair.SendFromPeer(second));
        QCOMPARE(transport.Receive({}), std::optional(first));
        QCOMPARE(transport.Receive({}), std::optional(second));
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
        QCOMPARE(
            transport->LastSend(),
            (std::vector<uint8_t>{
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01, 0x00, 0x00, 0x00}));
        manager._connected = false;
        manager._transport.store(nullptr);
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
#endif
};

QTEST_GUILESS_MAIN(AAPTransportTests)

#include "AAPTransportTests.moc"
