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
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
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
    #include "Source/Core/ConnectionDiagnostics.h"
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
    void AdaptiveNoiseControlRequiresFeatureFlagsWithoutSendingPacket()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager._featureFlagsEnabled = false;

        QVERIFY(!manager.SetNoiseControlMode(Core::AAP::NoiseControlMode::Adaptive));
        QCOMPARE(transport->SendCalls(), size_t{0});
        QVERIFY(transport->SentRecords().empty());

        // Basic modes remain usable when optional feature flags were rejected.
        const std::vector<Core::AAP::NoiseControlMode> basicModes{
            Core::AAP::NoiseControlMode::Off, Core::AAP::NoiseControlMode::NoiseCancellation,
            Core::AAP::NoiseControlMode::Transparency};
        for (const auto mode : basicModes) {
            QVERIFY(manager.SetNoiseControlMode(mode));
        }
        QCOMPARE(transport->SendCalls(), size_t{3});
        const auto records = transport->SentRecords();
        QCOMPARE(records[0], Core::AAP::Packets::BuildNoiseControlPacket(basicModes[0]));
        QCOMPARE(records[1], Core::AAP::Packets::BuildNoiseControlPacket(basicModes[1]));
        QCOMPARE(records[2], Core::AAP::Packets::BuildNoiseControlPacket(basicModes[2]));

        manager._featureFlagsEnabled = true;
        QVERIFY(manager.SetNoiseControlMode(Core::AAP::NoiseControlMode::Adaptive));
        QCOMPARE(transport->SendCalls(), size_t{4});
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    //////////////////////////////////////////////////
    // ATT single outstanding request per session (blueprint 0.3 / P3)
    //

    // A probe reserves the read for its own attempt before the frame is written, so a
    // device that answers immediately cannot make the confirmation race the state.
    void AttProbeReservesBeforeSendAndConfirmsImmediateResponse()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        constexpr uint64_t kGeneration = 5100;
        manager._attempt = Core::AAP::AAPAttempt{1, kGeneration};
        manager._connected = true;
        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);

        int probeResults = 0;
        int loudSoundReductions = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        callbacks.onLoudSoundReductionChanged = [&loudSoundReductions](
                                                    const Core::AAP::LoudSoundReductionState &,
                                                    const Core::AAP::AAPAttempt &) {
            ++loudSoundReductions;
        };
        manager.SetCallbacks(callbacks);

        bool pendingInsideSend = false;
        transport->SetSendHook([&] {
            // This runs before Send returns: Pending must already be visible and correlated.
            pendingInsideSend = manager._attPathState == Core::AAP::AttPathState::Pending &&
                                manager._attPendingRead.IsLive(manager._steadyMillis());
            manager.ProcessAttFrame(
                std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{1, kGeneration}, callbacks);
        });

        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Unknown);
        manager.StartAttProbe(Core::AAP::AAPAttempt{1, kGeneration});
        transport->SetSendHook({});

        QVERIFY(pendingInsideSend);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Confirmed);
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Confirmed);
        QCOMPARE(probeResults, 1);
        QCOMPARE(loudSoundReductions, 1);
        QCOMPARE(
            manager.GetLoudSoundReductionState(),
            std::optional(Core::AAP::LoudSoundReductionState::Enabled));
        QVERIFY(!manager._attPendingRead.HasPending());
        const auto sent = transport->SentRecords();
        QCOMPARE(sent.size(), size_t{1});
        QCOMPARE(sent.front(), (std::vector<uint8_t>{0x0A, 0x1B, 0x00}));

        const auto report =
            QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains("att_probe  ok  confirmed by read response"));
        QVERIFY(!report.contains("probe sent, waiting up to 500ms"));

        // The one-shot probe is not repeated after confirmation.
        manager.StartAttProbe(Core::AAP::AAPAttempt{1, kGeneration});
        QCOMPARE(transport->SentRecords().size(), size_t{1});
        QCOMPARE(probeResults, 1);

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    //////////////////////////////////////////////////
    // ATT single outstanding request per session (blueprint 0.3 / P3)
    //

    // A frame that belongs to a retired session is dropped before it can confirm anything:
    // neither the probe state nor the cached value may be written by another generation.
    void LateAttFrameFromOldGenerationIsDropped()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        int probeResults = 0;
        int loudSoundReductions = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        callbacks.onLoudSoundReductionChanged = [&loudSoundReductions](
                                                    const Core::AAP::LoudSoundReductionState &,
                                                    const Core::AAP::AAPAttempt &) {
            ++loudSoundReductions;
        };
        manager.SetCallbacks(callbacks);

        manager.StartAttProbe(Core::AAP::AAPAttempt{1, 1});
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);

        // Retire generation 1 and open a real successor with its own reservation.
        manager._attempt = Core::AAP::AAPAttempt{1, 2};
        manager._attPendingRead.Clear();
        manager._attPathState = Core::AAP::AttPathState::Unknown;
        manager.StartAttProbe(Core::AAP::AAPAttempt{1, 2});
        QCOMPARE(manager._attPendingRead.Attempt().generation, uint64_t{2});

        // Same handle, retired generation.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{1, 1}, callbacks);
        // Same generation, other device.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{2, 2}, callbacks);
        // Still unanswered, and no stale frame leaked into the state or the callbacks.
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);
        QVERIFY(manager._attPendingRead.HasPending());
        QCOMPARE(probeResults, 0);
        QCOMPARE(loudSoundReductions, 0);
        QVERIFY(!manager.GetLoudSoundReductionState().has_value());

        // The current generation's answer is accepted.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x0B, 0x00}, Core::AAP::AAPAttempt{1, 2}, callbacks);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Confirmed);
        QCOMPARE(probeResults, 1);
        QCOMPARE(
            manager.GetLoudSoundReductionState(),
            std::optional(Core::AAP::LoudSoundReductionState::Disabled));

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    // An error response proves the ATT stack answers, but it carries no attribute value:
    // the loud sound reduction state must stay unknown until a read actually answers.
    void AttErrorResponseConfirmsPathWithoutSettingValue()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        int probeResults = 0;
        int loudSoundReductions = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        callbacks.onLoudSoundReductionChanged = [&loudSoundReductions](
                                                    const Core::AAP::LoudSoundReductionState &,
                                                    const Core::AAP::AAPAttempt &) {
            ++loudSoundReductions;
        };
        manager.SetCallbacks(callbacks);
        manager.StartAttProbe(Core::AAP::AAPAttempt{1, 1});

        // An error naming another handle answers nothing.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x01, 0x0A, 0x2A, 0x00, 0x01}, Core::AAP::AAPAttempt{1, 1},
            callbacks);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);
        QCOMPARE(probeResults, 0);

        // An error for another request opcode does not answer this read.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x01, 0x12, 0x1B, 0x00, 0x0A}, Core::AAP::AAPAttempt{1, 1},
            callbacks);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);
        QVERIFY(manager._attPendingRead.HasPending());

        // The error that answers the outstanding read confirms the path only.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x01, 0x0A, 0x1B, 0x00, 0x0A}, Core::AAP::AAPAttempt{1, 1},
            callbacks);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Confirmed);
        QCOMPARE(probeResults, 1);
        QCOMPARE(loudSoundReductions, 0);
        QVERIFY(!manager.GetLoudSoundReductionState().has_value());
        QVERIFY(!manager._attPendingRead.HasPending());

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    // ATT notifications carry a handle, and only handle 0x1B belongs to loud sound
    // reduction. An unasked-for read response is not credited to the probe either.
    void AttFramesWithoutMatchingHandleOrRequestAreIgnored()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        int probeResults = 0;
        int loudSoundReductions = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        callbacks.onLoudSoundReductionChanged = [&loudSoundReductions](
                                                    const Core::AAP::LoudSoundReductionState &,
                                                    const Core::AAP::AAPAttempt &) {
            ++loudSoundReductions;
        };
        manager.SetCallbacks(callbacks);

        // No read outstanding: a read response cannot be attributed, so it neither confirms
        // the path nor invents a value.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{1, 1}, callbacks);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Unknown);
        QCOMPARE(probeResults, 0);
        QCOMPARE(loudSoundReductions, 0);

        // A notification for another attribute (0x2A hearing aid) is ignored; the loud
        // sound reduction handle is accepted.
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x1B, 0x2A, 0x00, 0x01}, Core::AAP::AAPAttempt{1, 1}, callbacks);
        QCOMPARE(loudSoundReductions, 0);
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x1B, 0x1B, 0x00, 0x01}, Core::AAP::AAPAttempt{1, 1}, callbacks);
        QCOMPARE(loudSoundReductions, 1);
        QCOMPARE(
            manager.GetLoudSoundReductionState(),
            std::optional(Core::AAP::LoudSoundReductionState::Enabled));

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    // The probe is sent once per window: while a read is outstanding a repeated request is
    // idempotent, so a session never carries two deadlines or two ATT frames.
    void AttProbeNeverIssuesTwoOutstandingReads()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;

        manager.StartAttProbe(Core::AAP::AAPAttempt{1, 1});
        const auto deadline = manager._attPendingRead.DeadlineMs();
        QVERIFY(deadline.has_value());

        for (int repeat = 0; repeat < 5; ++repeat) {
            manager.StartAttProbe(Core::AAP::AAPAttempt{1, 1});
        }
        QCOMPARE(transport->SentRecords().size(), size_t{1});
        QCOMPARE(manager._attPendingRead.DeadlineMs(), deadline);
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);

        // SetLoudSoundReduction reads back through the same reservation: the probe is still
        // open, so the ATT write goes out but no second read does.
        QVERIFY(manager.SetLoudSoundReduction(true));
        const auto records = transport->SentRecords();
        QCOMPARE(records.size(), size_t{2});
        QCOMPARE(records[0], (std::vector<uint8_t>{0x0A, 0x1B, 0x00}));
        QCOMPARE(records[1], (std::vector<uint8_t>{0x12, 0x1B, 0x00, 0x01}));
        QCOMPARE(manager._attPendingRead.DeadlineMs(), deadline);

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void AttProbeDeadlineBoundaryIsDeterministic_data()
    {
        QTest::addColumn<int>("elapsedMs");
        QTest::addColumn<int>("expectedState");
        QTest::newRow("499ms-accepted") << 499 << int(Core::AAP::AttPathState::Confirmed);
        QTest::newRow("500ms-expired") << 500 << int(Core::AAP::AttPathState::Unconfirmed);
        QTest::newRow("501ms-expired") << 501 << int(Core::AAP::AttPathState::Unconfirmed);
    }

    void AttProbeDeadlineBoundaryIsDeterministic()
    {
        QFETCH(int, elapsedMs);
        QFETCH(int, expectedState);
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        int64_t nowMs = 10000;
        manager._steadyMillis = [&] { return nowMs; };
        manager._transport.store(transport);
        constexpr uint64_t kGeneration = 5200;
        manager._attempt = Core::AAP::AAPAttempt{1, kGeneration};
        manager._connected = true;
        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);

        int probeResults = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        manager.StartAttProbe(Core::AAP::AAPAttempt{1, kGeneration});
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);
        nowMs += elapsedMs;
        manager.ProcessAttFrame(
            std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{1, kGeneration}, callbacks);
        QCOMPARE(int(manager.GetAttPathState()), expectedState);
        QCOMPARE(probeResults, elapsedMs < 500 ? 1 : 0);
        QVERIFY(!manager._attPendingRead.HasPending());
        QCOMPARE(transport->SentRecords().size(), size_t{1});

        const auto report =
            QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        if (elapsedMs < 500) {
            QVERIFY(report.contains("confirmed by read response"));
        }
        else {
            QVERIFY(report.contains("att_probe  FAILED  no ATT answer within 500ms"));
        }
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void AttSendFailureReleasesReservationAndRecordsFailure()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->SetSendError(true);
        manager._transport.store(transport);
        constexpr uint64_t kGeneration = 5300;
        manager._attempt = Core::AAP::AAPAttempt{1, kGeneration};
        manager._connected = true;
        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);

        QVERIFY(!manager.RequestAttRead(Core::AAP::AAPAttempt{1, kGeneration}, 0x1B));
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Unconfirmed);
        QVERIFY(!manager._attPendingRead.HasPending());
        QCOMPARE(transport->SentRecords().size(), size_t{0});
        const auto report =
            QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains("att_probe  FAILED  send failed"));

        // The failed write released the only slot, so an explicit later read can reserve it.
        transport->SetSendError(false);
        QVERIFY(manager.RequestAttRead(Core::AAP::AAPAttempt{1, kGeneration}, 0x1B));
        QVERIFY(manager._attPendingRead.HasPending());
        QCOMPARE(transport->SentRecords().size(), size_t{1});
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void ImmediateAttResponseSurvivesFailedSendResult()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->FailNthSend(0);
        manager._transport.store(transport);
        constexpr uint64_t kGeneration = 5400;
        manager._attempt = Core::AAP::AAPAttempt{1, kGeneration};
        manager._connected = true;
        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);
        int probeResults = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        transport->SetSendHook([&] {
            manager.ProcessAttFrame(
                std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{1, kGeneration}, callbacks);
        });

        QVERIFY(!manager.RequestAttRead(Core::AAP::AAPAttempt{1, kGeneration}, 0x1B));
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Confirmed);
        QVERIFY(!manager._attPendingRead.HasPending());
        QCOMPARE(probeResults, 1);
        const auto report =
            QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains("confirmed by read response"));
        QVERIFY(!report.contains("att_probe  FAILED  send failed"));
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void DelayedOldSendFailureCannotReleaseReplacementReservation()
    {
        constexpr uint64_t kGeneration = 5500;
        const Core::AAP::AAPAttempt attempt{1, kGeneration};
        Core::AAP::Manager manager;
        auto oldTransport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(oldTransport->Connect(1, {}));
        oldTransport->SetBlockSend(true);
        std::atomic<int64_t> nowMs{10000};
        manager._steadyMillis = [&] { return nowMs.load(); };
        manager._transport.store(oldTransport);
        manager._attempt = attempt;
        manager._connected = true;
        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);

        auto oldSend = std::async(std::launch::async, [&] {
            return manager.RequestAttRead(attempt, Core::AAP::Att::kLoudSoundReductionHandle);
        });
        QVERIFY(oldTransport->WaitForSendWaiter(std::chrono::seconds(1)));

        // The old write remains blocked while its half-open window closes. The identical
        // session/handle may then reserve a fresh window and send through a replacement link.
        nowMs = 10500;
        auto replacement = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(replacement->Connect(1, {}));
        manager._transport.store(replacement);
        QVERIFY(manager.RequestAttRead(attempt, Core::AAP::Att::kLoudSoundReductionHandle));
        QVERIFY(manager._attPendingRead.HasPending());
        QCOMPARE(manager._attPendingRead.DeadlineMs(), std::optional<int64_t>(11000));

        oldTransport->SetSendError(true);
        oldTransport->SetBlockSend(false);
        QVERIFY(oldSend.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QVERIFY(!oldSend.get());

        // The failed old sender must not clear or downgrade the replacement reservation.
        QVERIFY(manager._attPendingRead.HasPending());
        QCOMPARE(manager._attPendingRead.Attempt().generation, kGeneration);
        QCOMPARE(manager._attPendingRead.DeadlineMs(), std::optional<int64_t>(11000));
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Pending);
        QCOMPARE(replacement->SentRecords().size(), size_t{1});

        manager.ProcessAttFrame(std::vector<uint8_t>{0x0B, 0x01}, attempt, Core::AAP::Callbacks{});
        QCOMPARE(manager.GetAttPathState(), Core::AAP::AttPathState::Confirmed);
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void InitialAttProbeRunsWhileConnecting()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connecting = true;
        manager._connected = false;
        int probeResults = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onAttProbeResult =
            [&probeResults](const Core::AAP::AttPathState &, const Core::AAP::AAPAttempt &) {
                ++probeResults;
            };
        transport->SetSendHook([&] {
            manager.ProcessAttFrame(
                std::vector<uint8_t>{0x0B, 0x01}, Core::AAP::AAPAttempt{1, 1}, callbacks);
        });

        manager.StartAttProbe(Core::AAP::AAPAttempt{1, 1});
        QCOMPARE(manager._attPathState, Core::AAP::AttPathState::Confirmed);
        QCOMPARE(probeResults, 1);
        QVERIFY(!manager._attPendingRead.HasPending());
        QCOMPARE(transport->SentRecords().size(), size_t{1});
        QCOMPARE(transport->SentRecords().front(), (std::vector<uint8_t>{0x0A, 0x1B, 0x00}));
        manager._connecting = false;
        manager._transport.store(nullptr);
    }

    void VolumeSwipeEchoNeverChangesLoudSoundReduction()
    {
        Core::AAP::Manager manager;
        manager._attempt = Core::AAP::AAPAttempt{1, 1};
        manager._connected = true;
        manager._loudSoundReductionState = Core::AAP::LoudSoundReductionState::Enabled;
        int changes = 0;
        Core::AAP::Callbacks callbacks;
        callbacks.onLoudSoundReductionChanged =
            [&changes](const Core::AAP::LoudSoundReductionState &, const Core::AAP::AAPAttempt &) {
                ++changes;
            };
        manager.ProcessPacket(
            std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x02, 0x00, 0x00, 0x00},
            Core::AAP::AAPAttempt{1, 1}, callbacks);
        QCOMPARE(changes, 0);
        QCOMPARE(
            manager.GetLoudSoundReductionState(),
            std::optional(Core::AAP::LoudSoundReductionState::Enabled));
        manager._connected = false;
    }

    //////////////////////////////////////////////////
    // Connection diagnostics on the handshake path (blueprint 1.4 / P3)
    //

    // Returns the most recent line for a step: the Recorder is a process-wide singleton and
    // other tests in this executable have already recorded some of the same steps.
    static QString LineFor(const QString &report, const QString &step)
    {
        const auto lines = report.split(QLatin1Char('\n'));
        QString latest;
        for (const auto &line : lines) {
            if (line.contains(step)) {
                latest = line.trimmed();
            }
        }
        return latest;
    }

    // A rejected feature-flags write must be recorded as a failure, while the sequence
    // continues (the session degrades instead of pretending the packet landed).
    void FeatureFlagsSendFailureIsRecordedAsFailure()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        transport->FailNthSend(1); // handshake, feature flags, notifications, ATT probe
        manager._transport.store(transport);
        constexpr uint64_t kGeneration = 4242;
        manager._attempt = Core::AAP::AAPAttempt{1, kGeneration};
        manager._connected = true;

        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);
        QVERIFY(manager.InitializeConnection(Core::AAP::AAPAttempt{1, kGeneration}, {}));

        const auto report =
            QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains("attempt 4242"));
        const auto flags = LineFor(report, QStringLiteral("feature_flags"));
        QVERIFY(flags.contains("FAILED"));
        QVERIFY(!flags.contains(" ok"));
        QVERIFY(flags.contains("send failed"));
        QVERIFY(!manager._featureFlagsEnabled);
        // Only the writes the transport accepted are on the wire.
        QCOMPARE(transport->SentRecords().size(), size_t{3});
        // A send is never reported as a device acknowledgement.
        const auto handshake = LineFor(report, QStringLiteral("handshake"));
        QVERIFY(handshake.contains("PENDING"));
        QVERIFY(handshake.contains("send accepted, ACK unconfirmed"));
        QVERIFY(!handshake.contains(" ok"));
        QVERIFY(!handshake.contains("ACK observed"));
        QVERIFY(LineFor(report, QStringLiteral("notification_subscribe")).contains("PENDING"));
        QVERIFY(LineFor(report, QStringLiteral("att_probe")).contains("PENDING"));

        // Capability-dependent commands degrade instead of being emitted after the failure.
        QVERIFY(!manager.SetConversationalAwareness(true));
        QVERIFY(!manager.SetAdaptiveTransparencyLevel(50));
        QVERIFY(!manager.SetAdaptiveNoiseLevel(50));
        QVERIFY(!manager.SetNoiseControlMode(Core::AAP::NoiseControlMode::Adaptive));
        QCOMPARE(transport->SendCalls(), size_t{4});

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    // The handshake ACK is a device record (01 00 04 00): the step may claim an
    // acknowledgement only once that record has been read on the live session.
    void HandshakeAckIsRecordedOnlyWhenObserved()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        constexpr uint64_t kGeneration = 4243;
        constexpr uint64_t kSuccessorGeneration = 4244;
        manager._attempt = Core::AAP::AAPAttempt{1, kGeneration};
        manager._connected = true;

        Core::Diagnostics::Recorder::Instance().BeginAttempt(kGeneration);
        QVERIFY(manager.InitializeConnection(Core::AAP::AAPAttempt{1, kGeneration}, {}));
        QVERIFY(!manager._handshakeAckObserved);
        QVERIFY(!manager._featureFlagsAckObserved);
        QVERIFY(manager._featureFlagsEnabled);

        auto report =
            QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        QVERIFY(!report.contains("device ACK observed"));
        QVERIFY(LineFor(report, QStringLiteral("handshake"))
                    .contains("PENDING  send accepted, ACK unconfirmed"));
        QVERIFY(!LineFor(report, QStringLiteral("handshake")).contains(" ok"));

        manager.ProcessPacket(std::vector<uint8_t>{0x01, 0x00, 0x04, 0x00});
        manager.ProcessPacket(std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x2B, 0x00});
        QVERIFY(manager._handshakeAckObserved);
        QVERIFY(manager._featureFlagsAckObserved);

        report = QString::fromStdString(Core::Diagnostics::Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains("handshake  ok  device ACK observed"));
        QVERIFY(report.contains("feature_flags  ok  device ACK observed"));

        // A duplicate ACK does not announce the observation twice.
        const auto before = Core::Diagnostics::Recorder::Instance().ReportedAttemptStepCount();
        manager.ProcessPacket(std::vector<uint8_t>{0x01, 0x00, 0x04, 0x00});
        QCOMPARE(Core::Diagnostics::Recorder::Instance().ReportedAttemptStepCount(), before);

        // An ACK explicitly tagged with the retired generation cannot confirm the successor.
        manager._attempt = Core::AAP::AAPAttempt{1, kSuccessorGeneration};
        manager._handshakeAckObserved = false;
        Core::Diagnostics::Recorder::Instance().BeginAttempt(kSuccessorGeneration);
        const auto freshCount = Core::Diagnostics::Recorder::Instance().ReportedAttemptStepCount();
        manager.ProcessPacket(
            std::vector<uint8_t>{0x01, 0x00, 0x04, 0x00}, Core::AAP::AAPAttempt{1, kGeneration},
            Core::AAP::Callbacks{});
        QVERIFY(!manager._handshakeAckObserved);
        QCOMPARE(Core::Diagnostics::Recorder::Instance().ReportedAttemptStepCount(), freshCount);

        manager._connected = false;
        manager._transport.store(nullptr);
    }

    // Reset happens before the new attempt records AdvertisementMatched, so the report starts
    // with this attempt's evidence and never inherits an older session's lines.
    void BeginAttemptResetsBeforeRecordingAdvertisementMatched()
    {
        auto &recorder = Core::Diagnostics::Recorder::Instance();
        recorder.Record(Core::Diagnostics::Step::ScannerStarted, true);
        recorder.Record(Core::Diagnostics::Step::AdvertisementMatched, true, "old attempt");
        recorder.EndAttempt(4243);

        recorder.BeginAttempt(4244);
        QCOMPARE(recorder.StepCount(), size_t{0});
        const auto masked = Core::Diagnostics::Recorder::MaskAddress(0x1122334455667788ULL);
        recorder.RecordForAttempt(
            4244, Core::Diagnostics::Step::AdvertisementMatched, true, "address=" + masked);
        recorder.RecordForAttempt(
            4244, Core::Diagnostics::Step::TransportConnect, false, "MagicAAP driver unavailable");

        const auto report = QString::fromStdString(recorder.BuildReport({}));
        QVERIFY(!report.contains("scanner_started"));
        QVERIFY(!report.contains("old attempt"));
        QVERIFY(report.contains("attempt 4244"));
        QVERIFY(report.contains("advertisement_matched"));
        QVERIFY(report.indexOf("advertisement_matched") < report.indexOf("transport_connect"));
        QVERIFY(report.contains("transport_connect  FAILED  MagicAAP driver unavailable"));
        QVERIFY(!report.contains("1122334455667788"));
        recorder.EndAttempt(4244);
    }
#endif
};

QTEST_GUILESS_MAIN(AAPTransportTests)

#include "AAPTransportTests.moc"
