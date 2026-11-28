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

// Manager/worker seam suite: the connection lifecycle that P2 introduced, exercised
// without Bluetooth hardware and without wall-clock inference.
//
// Seam and its constraints: Core::AAP::Manager has one private
// `_transportFactoryForTest` hook that ConnectTransport consults before the platform
// creators. It is reachable only through the friend declaration in AAPManager.h, it is
// never assigned by production code, and it is not exposed in any public signature, so
// the production API surface is unchanged. The hook replaces only transport *creation*:
// the pending-publish, CancelConnect-close, handshake, reader startup and generation
// filtering paths under test are the real production ones. A test seam was necessary
// because neither a SEQPACKET socket nor the MagicAAP device interface can be built or
// parked from a hardware-free CI job.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QtTest>

#include "AAPTestSupport.h"
#include "Source/Core/AAPManager.h"
#include "Source/Core/AAPMessage.h"
#include "Source/Core/AAPTransport.h"
#include "Source/Core/ConnectionDiagnostics.h"

using AAPTestSupport::BlockingTransport;
using AAPTestSupport::CallbackRecorder;
using AAPTestSupport::PromiseGate;
using AAPTestSupport::ScopedRelease;
using AAPTestSupport::ScopedUnblock;
using AAPTestSupport::TransportFactory;
using AAPTestSupport::Worker;
using namespace std::chrono_literals;

namespace {
using HandshakeFactoryHook = TransportFactory<BlockingTransport>::Hook;

TransportFactory<BlockingTransport> MakeHandshakeFactory(HandshakeFactoryHook hook = {})
{
    return TransportFactory<BlockingTransport>{
        [hook = std::move(hook)](BlockingTransport &transport) {
            transport.QueueRecord(AAPTestSupport::Records::HandshakeAck());
            if (hook) {
                hook(transport);
            }
        }};
}

// Disconnect joins a production reader, so exercise it on a test-owned worker too. The
// worker releases every injected transport on all exits and the deadline makes a broken
// Close -> stop -> join sequence a failed assertion instead of an unbounded teardown.
template <typename Factory>
bool DisconnectWithin(Core::AAP::Manager &manager, Factory &factory)
{
    Worker worker{[&] { manager.Disconnect(); }, [&] { factory.ReleaseAllBuilt(); }};
    return worker.Finished(2s);
}

} // namespace

class AAPManagerWorkerTests : public QObject
{
    Q_OBJECT

private slots:
    // Cross-platform: the attempt identity is rejected before any transport work, and a
    // zero generation can never become a session.
    void ConnectRejectsAZeroGenerationAttempt()
    {
        Core::AAP::Manager manager;
        QVERIFY(!manager.Connect(Core::AAP::AAPAttempt{42, 0}));
        QVERIFY(!manager.IsConnected());
        QCOMPARE(manager.GetLastAttempt().generation, uint64_t{0});
    }

#if defined(APD_OS_WIN)
    void InjectedTransportBecomesThePublishedSession()
    {
        auto factory = MakeHandshakeFactory([](BlockingTransport &transport) {
            // Park the reader so the session stays up deterministically instead of
            // racing an immediate EOF; the thread belongs to production, so the release
            // is an RAII guard rather than a Worker the test owns.
            transport.Block(BlockingTransport::Op::Receive);
        });
        CallbackRecorder recorder;
        Core::AAP::Callbacks callbacks;
        callbacks.onConnected = [&recorder](const Core::AAP::AAPAttempt &attempt) {
            recorder.Record("connected", attempt);
        };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));

        const Core::AAP::AAPAttempt attempt{42, 7};
        QVERIFY(manager.Connect(attempt));
        QCOMPARE(factory.Count(), size_t{1});
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        QVERIFY(
            manager._transport.load() ==
            std::static_pointer_cast<Core::AAP::AAPTransport>(transport));
        QVERIFY(manager.IsConnected());
        QCOMPARE(manager.GetLastAttempt().generation, attempt.generation);
        QCOMPARE(recorder.Count("connected"), size_t{1});
        QCOMPARE(recorder.LastAttempt("connected")->generation, attempt.generation);

        ScopedRelease unblock{*transport};
        QVERIFY(manager._readerThread.joinable());
        QVERIFY(DisconnectWithin(manager, factory));
        // P2 removed the timed detach branch, so a finished Disconnect must leave nothing
        // joinable and nothing published.
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(!manager._transport.load());
        QVERIFY(!manager.IsConnected());
        QVERIFY(transport->IsClosed());
    }

    // Regression guard: a merge resolution dropped the production BeginAttempt/EndAttempt
    // wiring, so every attempt-tagged diagnostic record was silently discarded and the
    // report could never show which generation a step belonged to. Connect must open the
    // attempt (after its own reset, so AdvertisementMatched survives), Disconnect must
    // close it, and a failed Connect must not leave its generation open either.
    void ConnectOpensAndClosesTheDiagnosticsAttempt()
    {
        using Core::Diagnostics::Recorder;
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        Recorder::Instance().Reset();
        const Core::AAP::AAPAttempt attempt{42, 9};
        QVERIFY(manager.Connect(attempt));
        QCOMPARE(Recorder::Instance().CurrentGeneration(), attempt.generation);
        const auto report = QString::fromStdString(Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains(QStringLiteral("advertisement_matched")));
        QVERIFY(report.contains(QStringLiteral("transport_connect")));

        ScopedRelease unblock{*factory.Last()};
        QVERIFY(DisconnectWithin(manager, factory));
        QCOMPARE(Recorder::Instance().CurrentGeneration(), uint64_t{0});

        // A transport that never comes up must close its attempt through the phase guard.
        Core::AAP::Manager failing;
        failing._transportFactoryForTest = [] {
            return std::shared_ptr<Core::AAP::AAPTransport>{};
        };
        QVERIFY(!failing.Connect(Core::AAP::AAPAttempt{42, 10}));
        QCOMPARE(Recorder::Instance().CurrentGeneration(), uint64_t{0});
    }

    // Regression guard: the handshake and the ATT probe are written while the session is
    // being published, before the observable connected flag flips. A gate on the flag
    // rather than on the transport snapshot silently made Connect() unable to send.
    void HandshakeReachesTheInjectedTransportBeforeTheSessionIsConnected()
    {
        auto factory = MakeHandshakeFactory();
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        const Core::AAP::AAPAttempt attempt{42, 1};
        QVERIFY(manager.Connect(attempt));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);

        const auto sent = transport->SentRecords();
        QCOMPARE(sent.size(), size_t{5});
        QCOMPARE(sent[0], Core::AAP::Packets::Handshake);
        QCOMPARE(sent[1], Core::AAP::Packets::BuildStemConfigPacket(Core::AAP::kAllStemMasks));
        QCOMPARE(sent[2], Core::AAP::Packets::EnableFeatures);
        QCOMPARE(sent[3], Core::AAP::Packets::RequestNotifications);
        QCOMPARE(sent[4], AAPTestSupport::Records::AttReadRequest());

        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager.IsConnected());
    }

    void ConnectWithoutObservedHandshakeAckFailsClosed()
    {
        using Core::Diagnostics::Recorder;
        TransportFactory<BlockingTransport> factory{
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); }};
        CallbackRecorder recorder;
        Core::AAP::Callbacks callbacks;
        callbacks.onConnected = [&recorder](const Core::AAP::AAPAttempt &attempt) {
            recorder.Record("connected", attempt);
        };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));
        Recorder::Instance().Reset();

        const Core::AAP::AAPAttempt attempt{42, 1};
        QVERIFY(!manager.Connect(attempt));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(!manager._transport.load());
        QVERIFY(transport->IsClosed());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::HandshakeFailure);
        QCOMPARE(recorder.Count("connected"), size_t{0});
        const auto report = QString::fromStdString(Recorder::Instance().BuildReport({}));
        QVERIFY(report.contains(QStringLiteral("handshake")));
        QVERIFY(!report.contains(QStringLiteral("device ACK observed")));
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void HandshakeAckReturnedAfterDeadlineIsRejected()
    {
        class AckAfterStopTransport : public BlockingTransport
        {
        public:
            std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override
            {
                if (!_firstReceive) {
                    return BlockingTransport::Receive(stop);
                }
                _firstReceive = false;
                std::stop_callback callback{stop, [this] { _stopped.Open(); }};
                _stopped.Wait(2s);
                return AAPTestSupport::Records::HandshakeAck();
            }

        private:
            AAPTestSupport::AtomicGate _stopped;
            bool _firstReceive{true};
        };

        auto transport = std::make_shared<AckAfterStopTransport>();
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&] { return transport; };

        bool result = true;
        Worker connector{[&] { result = manager.Connect(Core::AAP::AAPAttempt{42, 1}); }};
        QVERIFY(connector.Finished(3s));
        QVERIFY(!result);
        QVERIFY(!manager.IsConnected());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::HandshakeFailure);
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(!manager._transport.load());
        QVERIFY(transport->IsClosed());
    }

    void HandshakeAckAfterWallClockDeadlineIsRejectedBeforeTimerStop()
    {
        class GatedAckTransport : public BlockingTransport
        {
        public:
            explicit GatedAckTransport(PromiseGate &release) : _release(release) {}

            std::optional<std::vector<uint8_t>> Receive(std::stop_token stop) override
            {
                if (!_firstReceive) {
                    return BlockingTransport::Receive(stop);
                }
                _firstReceive = false;
                _release.Wait();
                return AAPTestSupport::Records::HandshakeAck();
            }

        private:
            PromiseGate &_release;
            bool _firstReceive{true};
        };

        AAPTestSupport::AtomicGate deadlineEntered;
        AAPTestSupport::AtomicGate deadlineStopped;
        PromiseGate receiveRelease;
        auto transport = std::make_shared<GatedAckTransport>(receiveRelease);
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&] { return transport; };
        manager._beforeHandshakeDeadlineStopForTest = [&](std::stop_token timerStop) {
            deadlineEntered.Open();
            std::stop_callback callback{timerStop, [&] { deadlineStopped.Open(); }};
            deadlineStopped.Wait(2s);
        };

        bool result = true;
        Worker connector{[&] { result = manager.Connect(Core::AAP::AAPAttempt{42, 1}); }};
        ScopedUnblock unblock{receiveRelease};
        QVERIFY(deadlineEntered.Wait(2s));
        receiveRelease.Signal();
        QVERIFY(connector.Finished(2s));
        QVERIFY(!result);
        QVERIFY(!manager.IsConnected());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::HandshakeFailure);
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(!manager._transport.load());
        QVERIFY(transport->IsClosed());
    }

    void CancelConnectWakesHandshakeAckWait()
    {
        TransportFactory<BlockingTransport> factory{
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); }};
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        bool result = true;
        Worker connector{
            [&] { result = manager.Connect(Core::AAP::AAPAttempt{42, 1}); },
            [&] {
                manager.CancelConnect();
                factory.ReleaseAllBuilt();
            }};
        QVERIFY(factory.WaitForCount(1, 2s));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        QVERIFY(transport->WaitForParked(BlockingTransport::Op::Receive, 2s));

        manager.CancelConnect();
        QVERIFY(connector.Finished(2s));
        QVERIFY(!result);
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(transport->IsClosed());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::TransportError);
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void RecordBeforeHandshakeAckIsProcessedAfterConnection()
    {
        using namespace Core::AAP;
        TransportFactory<BlockingTransport> factory{[](BlockingTransport &transport) {
            transport.QueueRecord(
                AAPTestSupport::Records::NoiseControl(static_cast<uint8_t>(NoiseControlMode::Off)));
            transport.QueueRecord(AAPTestSupport::Records::HandshakeAck());
            transport.Block(BlockingTransport::Op::Receive);
        }};
        CallbackRecorder recorder;
        Core::AAP::Callbacks callbacks;
        callbacks.onNoiseControlChanged = [&recorder](NoiseControlMode, const AAPAttempt &attempt) {
            recorder.Record("noise", attempt);
        };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));

        const AAPAttempt attempt{42, 1};
        QVERIFY(manager.Connect(attempt));
        QVERIFY(recorder.WaitForCount("noise", 1, 2s));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        // Two receives prove the handshake wait consumed both queued records before the reader
        // replayed the noise record.
        QCOMPARE(transport->CallCount(BlockingTransport::Op::Receive), size_t{2});
        QCOMPARE(recorder.LastAttempt("noise")->generation, attempt.generation);
        QCOMPARE(manager.GetNoiseControlMode(), std::optional(NoiseControlMode::Off));
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void PreAckRecordLimitFailsClosed()
    {
        using namespace Core::AAP;
        TransportFactory<BlockingTransport> factory{[](BlockingTransport &transport) {
            for (size_t index = 0; index < 65; ++index) {
                transport.QueueRecord(AAPTestSupport::Records::NoiseControl(
                    static_cast<uint8_t>(NoiseControlMode::Off)));
            }
            transport.QueueRecord(AAPTestSupport::Records::HandshakeAck());
        }};
        CallbackRecorder recorder;
        Core::AAP::Callbacks callbacks;
        callbacks.onNoiseControlChanged = [&recorder](NoiseControlMode, const AAPAttempt &attempt) {
            recorder.Record("noise", attempt);
        };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));

        QVERIFY(!manager.Connect(AAPAttempt{42, 1}));
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._readerThread.joinable());
        QVERIFY(!manager._transport.load());
        QCOMPARE(manager.GetLastConnectFailure(), ConnectFailure::HandshakeFailure);
        QCOMPARE(recorder.Count("noise"), size_t{0});
        QVERIFY(factory.Last()->IsClosed());
    }

    void StaleCommandSettlementCannotOverwriteNewerRequest()
    {
        using namespace Core::AAP;
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        std::atomic<size_t> settlements{0};
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &, AAPAttempt) { ++settlements; };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));
        const Core::AAP::AAPAttempt attempt{42, 1};
        QVERIFY(manager.Connect(attempt));
        {
            std::lock_guard lock{manager._mutex};
            manager._noiseControlMode = NoiseControlMode::Transparency;
        }

        const auto stale = manager._commandBroker->RecordDesired(
            {AAPCommandKind::SetNoiseControl, static_cast<uint32_t>(NoiseControlMode::Off)});
        const auto current = manager._commandBroker->RecordDesired(
            {AAPCommandKind::SetNoiseControl,
             static_cast<uint32_t>(NoiseControlMode::NoiseCancellation)});
        QVERIFY(stale.requestId != 0);
        QVERIFY(current.requestId != 0);
        QVERIFY(stale.requestId != current.requestId);

        AAPCommandOutcome outcome;
        outcome.command = {
            AAPCommandKind::SetNoiseControl, static_cast<uint32_t>(NoiseControlMode::Off),
            stale.requestId};
        outcome.generation = attempt.generation;
        outcome.requestId = stale.requestId;
        outcome.mode = AAPConfirmMode::Echo;
        outcome.settlement = AAPCommandSettlement::Confirmed;
        manager.HandleCommandSettled(outcome);
        QCOMPARE(
            manager.GetNoiseControlMode(),
            std::optional<NoiseControlMode>{NoiseControlMode::Transparency});
        QCOMPARE(settlements.load(), size_t{0});

        outcome.command.value = static_cast<uint32_t>(NoiseControlMode::NoiseCancellation);
        outcome.requestId = current.requestId;
        manager.HandleCommandSettled(outcome);
        QCOMPARE(
            manager.GetNoiseControlMode(),
            std::optional<NoiseControlMode>{NoiseControlMode::NoiseCancellation});
        QCOMPARE(settlements.load(), size_t{1});
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void CommandSubmissionWaitsForSettlementCacheUpdate()
    {
        using namespace Core::AAP;
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        auto identityChecked = std::make_shared<PromiseGate>();
        auto identityRelease = std::make_shared<PromiseGate>();
        ScopedUnblock unblock{*identityRelease};
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        const Core::AAP::AAPAttempt attempt{42, 1};
        QVERIFY(manager.Connect(attempt));
        {
            std::lock_guard lock{manager._mutex};
            manager._noiseControlMode = NoiseControlMode::Transparency;
        }
        const auto current = manager._commandBroker->RecordDesired(
            {AAPCommandKind::SetNoiseControl, static_cast<uint32_t>(NoiseControlMode::Off)});
        QVERIFY(current.requestId != 0);
        manager._afterCommandIdentityCheckForTest = [&] {
            identityChecked->Signal();
            identityRelease->Wait(AAPTestSupport::kRendezvousTimeout);
        };

        AAPCommandOutcome outcome;
        outcome.command = {
            AAPCommandKind::SetNoiseControl, static_cast<uint32_t>(NoiseControlMode::Off),
            current.requestId};
        outcome.generation = attempt.generation;
        outcome.requestId = current.requestId;
        outcome.mode = AAPConfirmMode::Echo;
        outcome.settlement = AAPCommandSettlement::Confirmed;
        Worker settlement{[&] { manager.HandleCommandSettled(outcome); }};
        QVERIFY(identityChecked->Wait(2s));
        Worker submission{
            [&] {
                manager.SubmitCommand(
                    {AAPCommandKind::SetNoiseControl,
                     static_cast<uint32_t>(NoiseControlMode::NoiseCancellation)});
            },
            [&] { factory.ReleaseAllBuilt(); }};
        QVERIFY(!submission.Finished(100ms));
        identityRelease->Signal();
        QVERIFY(settlement.Finished(2s));
        QVERIFY(submission.Finished(2s));
        QCOMPARE(
            manager.GetNoiseControlMode(), std::optional<NoiseControlMode>{NoiseControlMode::Off});
        QCOMPARE(
            manager.DesiredCommand(AAPCommandKind::SetNoiseControl),
            std::optional<uint32_t>{static_cast<uint32_t>(NoiseControlMode::NoiseCancellation)});
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void CancelledCommandLosingConnectionDoesNotRecordOfflineIntent()
    {
        using namespace Core::AAP;
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        std::stop_source stop;
        manager._beforeCommandSubmitLockForTest = [&] {
            {
                std::lock_guard lock{manager._mutex};
                manager._connected = false;
            }
            stop.request_stop();
        };
        const auto result = manager.SubmitCommand(
            {AAPCommandKind::SetNoiseControl,
             static_cast<uint32_t>(NoiseControlMode::NoiseCancellation)},
            stop.get_token());
        QVERIFY(!result.accepted);
        QCOMPARE(result.failure, std::optional(AAPCommandFailure::Rejected));
        QVERIFY(!manager.DesiredCommand(AAPCommandKind::SetNoiseControl).has_value());
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void ShutdownCommandsDrainsPublishedCallbacks()
    {
        using namespace Core::AAP;
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        auto callbackEntered = std::make_shared<PromiseGate>();
        auto callbackRelease = std::make_shared<PromiseGate>();
        ScopedUnblock unblock{*callbackRelease};
        Core::AAP::Callbacks callbacks;
        callbacks.onCommandSettled = [&](const AAPCommandOutcome &, AAPAttempt) {
            callbackEntered->Signal();
            callbackRelease->Wait(AAPTestSupport::kRendezvousTimeout);
        };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));
        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        QVERIFY(manager.SubmitCommand({AAPCommandKind::SetHeadTracking, 0}).accepted);
        QVERIFY(callbackEntered->Wait(2s));

        Worker shutdown{[&] { manager.ShutdownCommands(); }, [&] { factory.ReleaseAllBuilt(); }};
        QVERIFY(!shutdown.Finished(100ms));
        callbackRelease->Signal();
        QVERIFY(shutdown.Finished(2s));
        QVERIFY(DisconnectWithin(manager, factory));
    }

    // Initialization uses the pinned raw sender, but every later send must still honor
    // the connected gate. Pause after the public connected decision, close only the gate,
    // and prove refusal while the original transport is still published.
    void PublicSendRechecksConnectedWithTransportStillPresent()
    {
        auto sendEntered = std::make_shared<PromiseGate>();
        auto sendRelease = std::make_shared<PromiseGate>();
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        const auto sendsBefore = transport->CallCount(BlockingTransport::Op::Send);
        QCOMPARE(sendsBefore, size_t{5});

        manager._publicSendBarrierForTest = [&] {
            sendEntered->Signal();
            sendRelease->Wait(AAPTestSupport::kRendezvousTimeout);
        };
        ScopedUnblock unblock{*sendRelease};
        const auto submission = manager.SubmitCommand(
            {Core::AAP::AAPCommandKind::SetNoiseControl,
             static_cast<uint32_t>(Core::AAP::NoiseControlMode::Off)});
        QVERIFY(submission.accepted);
        QVERIFY(sendEntered->Wait(2s));
        QVERIFY(manager.IsConnected());
        QVERIFY(manager._transport.load() == transport);
        {
            std::lock_guard lock{manager._mutex};
            manager._connected = false;
        }
        QVERIFY(manager._transport.load() == transport);

        sendRelease->Signal();
        QVERIFY(manager.WaitForCommandsIdle(2s));
        QCOMPARE(transport->CallCount(BlockingTransport::Op::Send), sendsBefore);
        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager._readerThread.joinable());
    }

    // A command that observed the old connected session must not load a replacement
    // transport after a reconnect. The barrier makes that exact interleaving deterministic.
    void PublicSendCannotReachAReplacementSession()
    {
        auto sendEntered = std::make_shared<PromiseGate>();
        auto sendRelease = std::make_shared<PromiseGate>();
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        auto first = factory.Last();
        QVERIFY(first != nullptr);
        const auto firstSends = first->CallCount(BlockingTransport::Op::Send);
        manager._publicSendBarrierForTest = [&] {
            sendEntered->Signal();
            sendRelease->Wait(AAPTestSupport::kRendezvousTimeout);
        };
        ScopedUnblock unblock{*sendRelease};
        const auto submission = manager.SubmitCommand(
            {Core::AAP::AAPCommandKind::SetNoiseControl,
             static_cast<uint32_t>(Core::AAP::NoiseControlMode::Off)});
        QVERIFY(submission.accepted);
        QVERIFY(sendEntered->Wait(2s));
        QVERIFY(manager._transport.load() == first);

        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 2}));
        auto replacement = factory.Last();
        QVERIFY(replacement != nullptr);
        QVERIFY(replacement != first);
        const auto replacementSends = replacement->CallCount(BlockingTransport::Op::Send);
        QCOMPARE(replacementSends, size_t{5});

        sendRelease->Signal();
        QVERIFY(manager.WaitForCommandsIdle(4s));
        QCOMPARE(first->CallCount(BlockingTransport::Op::Send), firstSends);
        QCOMPARE(replacement->CallCount(BlockingTransport::Op::Send), replacementSends + 1);
        const auto replacementRecords = replacement->SentRecords();
        QCOMPARE(
            replacementRecords.back(),
            Core::AAP::Packets::BuildNoiseControlPacket(Core::AAP::NoiseControlMode::Off));
        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager._readerThread.joinable());
    }

    // Single-flight: the lifecycle mutex must serialize whole attempts, so concurrent
    // requests build exactly one transport and yield exactly one session.
    void ConcurrentConnectRequestsProduceOneSession()
    {
        auto factory = MakeHandshakeFactory([](BlockingTransport &transport) {
            transport.Block(BlockingTransport::Op::Connect);
            transport.Block(BlockingTransport::Op::Receive);
        });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        PromiseGate firstAtLock;
        PromiseGate secondAtLock;
        manager._beforeConnectLockForTest = [&](const Core::AAP::AAPAttempt &attempt) {
            (attempt.generation == 1 ? firstAtLock : secondAtLock).Signal();
        };

        std::atomic<int> successes{0};
        std::atomic<uint64_t> winner{0};
        auto connect = [&](uint64_t generation, bool &result) {
            const Core::AAP::AAPAttempt attempt{42, generation};
            result = manager.Connect(attempt);
            if (result) {
                winner.store(attempt.generation);
                ++successes;
            }
        };
        bool first = false;
        bool second = false;
        Worker firstConnect{
            [&] { connect(1, first); },
            [&] {
                manager.CancelConnect();
                factory.ReleaseAllBuilt();
            }};

        // The first attempt reaches the real lock-attempt boundary, acquires the lifecycle
        // mutex, and is parked before publication. Only then start the contender.
        QVERIFY(firstAtLock.Wait(2s));
        QVERIFY(factory.WaitForCount(1, 2s));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        QVERIFY(transport->WaitForParked(BlockingTransport::Op::Connect, 2s));
        QVERIFY(!manager._transport.load());

        Worker secondConnect{
            [&] { connect(2, second); },
            [&] {
                manager.CancelConnect();
                factory.ReleaseAllBuilt();
            }};
        // Positive contention proof: the contender has reached Connect's actual lock-attempt
        // seam while the first still owns that lock, yet no second transport has been built.
        QVERIFY(secondAtLock.Wait(2s));
        QCOMPARE(factory.Count(), size_t{1});
        QCOMPARE(manager.GetLastAttempt().generation, uint64_t{1});

        transport->Unblock(BlockingTransport::Op::Connect);
        QVERIFY(firstConnect.Finished(2s));
        QVERIFY(secondConnect.Finished(2s));
        QCOMPARE(successes.load(), 1);
        QCOMPARE(factory.Count(), size_t{1});
        QVERIFY(first != second);
        QVERIFY(manager.IsConnected());
        QCOMPARE(manager.GetLastAttempt().generation, winner.load());
        // Disconnect closes the published session before joining its parked reader, so no
        // extra release step is needed here.
        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager.IsConnected());
    }

    // A Connect blocked inside the OS/driver layer must be cancellable, because it holds
    // the lifecycle mutex: if it woke only on its own timeout, Disconnect would never be
    // reachable and the application could not exit.
    void CancelConnectWakesABlockedConnect()
    {
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Connect); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        bool result = true;
        Worker connector{
            [&] { result = manager.Connect(Core::AAP::AAPAttempt{42, 1}); },
            [&] {
                manager.CancelConnect();
                if (auto transport = factory.Last()) {
                    transport->ReleaseAll();
                }
            }};

        // Arrival is observed, never guessed: the factory records the transport before
        // production calls Connect on it, and the transport reports entering the park.
        QVERIFY(factory.WaitForCount(1, 2s));
        auto transport = factory.Last();
        QVERIFY(transport->WaitForParked(BlockingTransport::Op::Connect, 2s));

        manager.CancelConnect();
        QVERIFY(connector.Finished(2s));
        QVERIFY(!result);
        // The parked call never completed, yet Close woke it and withdrew the session.
        QCOMPARE(transport->CallCount(BlockingTransport::Op::Connect), size_t{0});
        QVERIFY(transport->IsClosed());
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._transport.load());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::TransportError);
        // The lifecycle mutex must be free again. Proving it by completing Disconnect on a
        // bounded worker, rather than peeking at a mutex-guarded flag from another thread,
        // keeps this race-free: a Connect that failed to release it misses the deadline.
        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager.IsConnected());
    }

    // CancelConnect can win after Connect's entry check but before the pending transport is
    // published. Rechecking after publication must prevent the transport call altogether;
    // otherwise the call blocks with only the caller token and Disconnect cannot wake it.
    void CancelConnectBeforePendingPublicationDoesNotEnterTransportConnect()
    {
        auto publishEntered = std::make_shared<PromiseGate>();
        auto publishRelease = std::make_shared<PromiseGate>();
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager._beforePendingPublishForTest = [&] {
            publishEntered->Signal();
            publishRelease->Wait(AAPTestSupport::kRendezvousTimeout);
        };

        bool result = true;
        Worker connector{
            [&] { result = manager.Connect(Core::AAP::AAPAttempt{42, 1}); },
            [&] {
                manager.CancelConnect();
                factory.ReleaseAllBuilt();
            }};
        QVERIFY(publishEntered->Wait(2s));
        {
            std::lock_guard lock{manager._pendingConnectMutex};
            QVERIFY(!manager._pendingTransport);
        }

        manager.CancelConnect();
        QCOMPARE(manager._cancelledGeneration.load(), uint64_t{1});
        publishRelease->Signal();
        QVERIFY(connector.Finished(2s));
        QVERIFY(!result);
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        QCOMPARE(transport->CallCount(BlockingTransport::Op::Connect), size_t{0});
        QVERIFY(transport->IsClosed());
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._transport.load());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::TransportError);
        QVERIFY(DisconnectWithin(manager, factory));
    }

    void CallerCancellationWakesConnectAndJoinsTheWorker()
    {
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Connect); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        std::stop_source callerStop;
        bool result = true;
        Worker connector{
            [&] { result = manager.Connect(Core::AAP::AAPAttempt{42, 9}, callerStop.get_token()); },
            [&] {
                callerStop.request_stop();
                factory.ReleaseAllBuilt();
            }};

        QVERIFY(factory.WaitForCount(1, 2s));
        auto transport = factory.Last();
        QVERIFY(transport->WaitForParked(BlockingTransport::Op::Connect, 2s));
        callerStop.request_stop();
        QVERIFY(connector.Finished(2s));
        QVERIFY(!result);
        QVERIFY(transport->IsClosed());
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._transport.load());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::TransportError);
        QVERIFY(DisconnectWithin(manager, factory));
    }

    // A transport that ends the stream on its own must leave the reader object available
    // for the next attempt to join, which is what keeps thread assignment from ever
    // destroying a joinable thread.
    void NaturalEofKeepsTheReaderJoinableAcrossReconnect()
    {
        auto factory = MakeHandshakeFactory();
        CallbackRecorder recorder;
        Core::AAP::Callbacks callbacks;
        callbacks.onConnected = [&recorder](const Core::AAP::AAPAttempt &attempt) {
            recorder.Record("connected", attempt);
        };
        callbacks.onDisconnected = [&recorder](const Core::AAP::AAPAttempt &attempt) {
            recorder.Record("disconnected", attempt);
        };
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };
        manager.SetCallbacks(std::move(callbacks));

        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        auto first = factory.Last();
        QVERIFY(first != nullptr);
        // An unblocked Receive returns end-of-stream immediately: no Close was involved.
        QVERIFY(recorder.WaitForCount("disconnected", 1, 2s));
        QVERIFY(!manager.IsConnected());
        QVERIFY(!first->IsClosed());
        QVERIFY(manager._readerThread.joinable());

        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 2}));
        QCOMPARE(factory.Count(), size_t{2});
        // Retiring the finished reader is what makes the reassignment safe. Natural EOF
        // withdraws the old transport snapshot; it is not an explicit Disconnect close.
        // One Receive consumed the handshake ACK and the reader's first Receive saw EOF.
        QCOMPARE(first->CallCount(BlockingTransport::Op::Receive), size_t{2});
        QVERIFY(!first->IsClosed());
        QVERIFY(recorder.WaitForCount("connected", 2, 2s));
        QVERIFY(recorder.WaitForCount("disconnected", 2, 2s));
        QVERIFY(manager._readerThread.joinable());

        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager._readerThread.joinable());
    }

    // Every worker event carries its attempt; an event from a retired generation must not
    // reach a callback or mutate cached state.
    void RetiredGenerationEventsAreDiscarded()
    {
        auto factory = MakeHandshakeFactory(
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); });
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        CallbackRecorder recorder;
        Core::AAP::Callbacks callbacks;
        callbacks.onNoiseControlChanged =
            [&recorder](Core::AAP::NoiseControlMode, const Core::AAP::AAPAttempt &attempt) {
                recorder.Record("noise", attempt);
            };
        manager.SetCallbacks(callbacks);

        const Core::AAP::AAPAttempt current{42, 5};
        QVERIFY(manager.Connect(current));
        auto transport = factory.Last();
        QVERIFY(manager.IsConnected());

        // Exercise the real reader with its captured identity. The queued record is
        // consumed, then the second Receive proves the stale ReaderLoop reached EOF.
        auto runStaleReader = [&](const Core::AAP::AAPAttempt &attempt,
                                  const std::vector<uint8_t> &record) {
            auto staleTransport = std::make_shared<BlockingTransport>();
            staleTransport->QueueRecord(record);
            Worker reader{
                [&] { manager.ReaderLoop(attempt, staleTransport, {}, callbacks); },
                [&] { staleTransport->ReleaseAll(); }};
            return staleTransport->WaitForCalls(BlockingTransport::Op::Receive, 2, 2s) &&
                   reader.Finished(2s);
        };

        const auto record = AAPTestSupport::Records::NoiseControl(0x02);
        const Core::AAP::AAPAttempt retired{42, current.generation - 1};
        QVERIFY(runStaleReader(retired, record));
        QCOMPARE(recorder.Count("noise"), size_t{0});
        QVERIFY(!manager.GetNoiseControlMode().has_value());

        // The current session's own reader must still accept and publish the record.
        transport->QueueRecord(record);
        QVERIFY(recorder.WaitForCount("noise", 1, 2s));
        QCOMPARE(recorder.LastAttempt("noise")->generation, current.generation);
        QVERIFY(manager.GetNoiseControlMode().has_value());

        // A record from a different bound device is stale even with a matching generation.
        const Core::AAP::AAPAttempt otherDevice{99, current.generation};
        QVERIFY(runStaleReader(otherDevice, AAPTestSupport::Records::NoiseControl(0x03)));
        QCOMPARE(recorder.Count("noise"), size_t{1});
        QCOMPARE(
            manager.GetNoiseControlMode().value(), Core::AAP::NoiseControlMode::NoiseCancellation);

        ScopedRelease unblock{*transport};
        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager.IsConnected());
    }

    // A seam that yields no transport must be reported as a transport failure rather than
    // silently continuing into the handshake.
    void TransportCreationFailureIsReported()
    {
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [] {
            return std::shared_ptr<Core::AAP::AAPTransport>{};
        };
        QVERIFY(!manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        QVERIFY(!manager.IsConnected());
        QCOMPARE(manager.GetLastConnectFailure(), Core::AAP::ConnectFailure::TransportError);
    }
#endif
};

QTEST_GUILESS_MAIN(AAPManagerWorkerTests)

#include "AAPManagerWorkerTests.moc"
