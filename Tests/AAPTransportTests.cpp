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
        QCOMPARE(
            transport->LastSend(),
            (std::vector<uint8_t>{
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01, 0x00, 0x00, 0x00}));
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
        QCOMPARE(future.get().settlement, Core::AAP::AAPCommandSettlement::AppliedUnconfirmed);
        QVERIFY(!future.get().rollBackDesired);
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

    void SameGenerationHeadFrameBeforeStartCannotConfirmFlight()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        QVERIFY(transport->Connect(1, {}));
        manager._transport.store(transport);
        manager._attempt = Core::AAP::AAPAttempt{1, 14};
        manager._connected = true;
        manager._commandBroker->BeginSession(14);
        std::promise<Core::AAP::AAPCommandOutcome> settled;
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const Core::AAP::AAPCommandOutcome &outcome,
                                         const Core::AAP::AAPAttempt &) {
            settled.set_value(outcome);
        };
        manager.SetCallbacks(std::move(callbacks));
        std::vector<uint8_t> sensor(70, 0x00);
        sensor[0] = 0x04;
        sensor[2] = 0x04;
        sensor[4] = 0x17;
        sensor[10] = 0x44;
        manager.ProcessPacket(sensor);
        QVERIFY(manager.SubmitCommand({Core::AAP::AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        QVERIFY(transport->WaitForRecordCount(1, std::chrono::seconds{1}));
        // A frame already queued when Start completed is inside the evidence fence.
        manager.ProcessPacket(sensor);
        QVERIFY(!manager.ConfirmedCommand(Core::AAP::AAPCommandKind::SetHeadTracking).has_value());
        QVERIFY(
            settled.get_future().wait_for(std::chrono::milliseconds{20}) ==
            std::future_status::timeout);
        manager._headTrackingStartedAtMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count() -
            100);
        manager.ProcessPacket(sensor);
        auto future = settled.get_future();
        QVERIFY(future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QCOMPARE(future.get().settlement, Core::AAP::AAPCommandSettlement::Confirmed);
        manager._connected = false;
        manager._transport.store(nullptr);
        manager._commandBroker->EndSession();
    }

    void AdaptiveNoiseStrengthRequiresAdaptiveModeAndCurrentSession()
    {
        Core::AAP::Manager manager;
        manager._connected = true;

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

        manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        transport->SetSendLimit(5);
        QVERIFY(!manager.SetAdaptiveNoiseLevel(70));
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        transport->SetSendLimit(std::nullopt);
        QVERIFY(manager.SetAdaptiveNoiseLevel(255));
        QCOMPARE(manager.GetAdaptiveNoiseLevel(), std::optional<uint8_t>{100});
        QCOMPARE(
            transport->LastSend(),
            (std::vector<uint8_t>{
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100, 0x00, 0x00, 0x00}));

        manager._noiseControlMode = Core::AAP::NoiseControlMode::Off;
        const auto sends = transport->SendCalls();
        QVERIFY(!manager.SetAdaptiveNoiseLevel(0));
        QCOMPARE(transport->SendCalls(), sends);

        manager.Disconnect();
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());
        QVERIFY(!manager.GetNoiseControlMode().has_value());
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

        std::future<bool> result = std::async(
            std::launch::async, [&manager] { return manager.SetAdaptiveNoiseLevel(70); });
        QVERIFY(oldTransport->WaitForSendWaiter(std::chrono::seconds{1}));

        // Retire generation 9 while its write is blocked. The attempt-bound cancellation must
        // release the old send before a generation-10 transport is installed.
        manager.Disconnect();
        QVERIFY(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(!result.get());
        QVERIFY(oldTransport->SentRecords().empty());
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        {
            std::lock_guard<std::mutex> lock{manager._mutex};
            manager._attempt = Core::AAP::AAPAttempt{1, 10};
            manager._transport.store(newTransport);
            manager._connected = true;
            manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        }
        QCOMPARE(newTransport->SendCalls(), size_t{0});
        QVERIFY(manager.SetAdaptiveNoiseLevel(40));
        const auto records = newTransport->SentRecords();
        QCOMPARE(records.size(), size_t{1});
        QCOMPARE(records.front().at(7), uint8_t{40});
        QVERIFY(oldTransport->SentRecords().empty());
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
        QVERIFY(manager.SubmitCommand(command).accepted);
        QCOMPARE(manager.DesiredCommand(command.kind), std::optional<uint32_t>{65});
        QCOMPARE(manager._commandBroker->QueuedCount(), size_t{0});
        QCOMPARE(transport->SendCalls(), size_t{0});

        // A 0x2E record outside reported Adaptive mode is stale/misrouted evidence.
        manager.ProcessPacket({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 65, 0x00, 0x00, 0x00});
        QVERIFY(!manager.ConfirmedCommand(command.kind).has_value());
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());

        manager._noiseControlMode = Core::AAP::NoiseControlMode::Adaptive;
        QVERIFY(manager.SubmitCommand(command).accepted);
        QVERIFY(transport->WaitForSendWaiter(std::chrono::seconds{1}));
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
        QCOMPARE(manager.GetAdaptiveNoiseLevel(), std::optional<uint8_t>{60});

        transport->SetEof();
        QTRY_VERIFY(!manager.IsConnected());
        QVERIFY(!manager.GetAdaptiveNoiseLevel().has_value());
        QVERIFY(!manager.GetNoiseControlMode().has_value());
        manager.Disconnect();
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

        // Handshake, raw stem config, feature flags, notification subscriptions, then one ATT read.
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

        // Generation 2 must replay 0x39 even though generation 1 sent the same revision. The first
        // replay write fails; the bounded post-handshake recovery must retry on this transport.
        transport->SetSendFailures(1);
        QVERIFY(manager.Connect({0x1234, 2}));
        QCOMPARE(transport->SendCalls(), size_t{6});
        const auto records = transport->SentRecords();
        QCOMPARE(records.size(), size_t{5});
        QCOMPARE(records.at(0), Core::AAP::Packets::Handshake);
        QCOMPARE(records.at(4).at(4), uint8_t{0x09});
        QCOMPARE(records.at(4).at(5), uint8_t{0x39});
        QCOMPARE(records.at(4).at(7), uint8_t{0x05});
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
        transport->SetSendFailures(2);
        QVERIFY(manager.Connect({0x1234, 1}));
        // Handshake, two failed 0x39 attempts, features, notifications, ATT probe. No third retry.
        QCOMPARE(transport->SendCalls(), size_t{6});
        QCOMPARE(transport->SentRecords().size(), size_t{4});
        manager.Disconnect();

        QVERIFY(manager.Connect({0x1234, 2}));
        QCOMPARE(transport->SendCalls(), size_t{5});
        const auto records = transport->SentRecords();
        QCOMPARE(records.at(1).at(5), uint8_t{0x39});
        QCOMPARE(records.at(1).at(7), uint8_t{0x0A});
        manager.Disconnect();
    }

    void AttemptBoundNoiseControlRevalidatesAttemptAndCancellation()
    {
        Core::AAP::Manager manager;
        auto transport = std::make_shared<AAPTestSupport::FakeTransport>();
        manager._transport.store(transport);
        manager._attempt = {0x1234, 7};
        manager._connected = true;
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
        QCOMPARE(
            transport->LastSend(),
            (std::vector<uint8_t>{
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00}));
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

        // Hold the send gate after the queued request has been validated by its caller. The worker
        // enters SetNoiseControlMode but cannot acquire _mutex until the replacement is installed.
        std::unique_lock barrier{manager._mutex};
        auto result = std::async(std::launch::async, [&] {
            return manager.SetNoiseControlMode(
                Core::AAP::NoiseControlMode::NoiseCancellation, {0x1234, 7}, {});
        });
        const bool blocked =
            result.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout;
        manager._attempt = {0x1234, 8};
        manager._transport.store(replacement);
        barrier.unlock();

        QVERIFY(blocked);
        QVERIFY(!result.get());
        QCOMPARE(original->SendCalls(), size_t{0});
        QCOMPARE(replacement->SendCalls(), size_t{0});
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

        std::unique_lock barrier{manager._mutex};
        auto result = std::async(std::launch::async, [&] {
            return manager.SetNoiseControlMode(
                Core::AAP::NoiseControlMode::NoiseCancellation, {0x1234, 7}, {});
        });
        const bool blocked =
            result.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout;
        manager._cancelledGeneration = 7;
        barrier.unlock();

        QVERIFY(blocked);
        QVERIFY(!result.get());
        QCOMPARE(transport->SendCalls(), size_t{0});
        manager._connected = false;
        manager._transport.store(nullptr);
    }

    void AttemptBoundNoiseControlPinsTransportDuringWrite()
    {
        Core::AAP::Manager manager;
        auto original = std::make_shared<AAPTestSupport::FakeTransport>();
        auto replacement = std::make_shared<AAPTestSupport::FakeTransport>();
        original->SetBlockSend(true);
        manager._transport.store(original);
        manager._attempt = {0x1234, 7};
        manager._connected = true;

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
        QCOMPARE(original->LastSend().at(5), uint8_t{0x0D});
        QCOMPARE(replacement->SendCalls(), size_t{0});
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
#endif
};

QTEST_GUILESS_MAIN(AAPTransportTests)

#include "AAPTransportTests.moc"
