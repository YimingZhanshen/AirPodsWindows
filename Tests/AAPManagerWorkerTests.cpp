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

using AAPTestSupport::BlockingTransport;
using AAPTestSupport::CallbackRecorder;
using AAPTestSupport::PromiseGate;
using AAPTestSupport::ScopedRelease;
using AAPTestSupport::TransportFactory;
using AAPTestSupport::Worker;
using namespace std::chrono_literals;

namespace {
// Disconnect joins a production reader, so exercise it on a test-owned worker too. The
// worker releases every injected transport on all exits and the deadline makes a broken
// Close -> stop -> join sequence a failed assertion instead of an unbounded teardown.
template <typename Factory>
bool DisconnectWithin(Core::AAP::Manager &manager, Factory &factory)
{
    Worker worker{[&] { manager.Disconnect(); }, [&] { factory.ReleaseAllBuilt(); }};
    return worker.Finished(2s);
}

// Holds Disconnect inside transport Close after the Manager has withdrawn its connected
// gate. That gives the public send path a deterministic teardown window to refuse writes.
class CloseGatedTransport final : public BlockingTransport
{
public:
    void SetCloseGates(std::shared_ptr<PromiseGate> entered, std::shared_ptr<PromiseGate> release)
    {
        _closeEntered = std::move(entered);
        _closeRelease = std::move(release);
    }

    void Close() override
    {
        _closeEntered->Signal();
        _closeRelease->Wait(AAPTestSupport::kRendezvousTimeout);
        BlockingTransport::Close();
    }

private:
    std::shared_ptr<PromiseGate> _closeEntered{std::make_shared<PromiseGate>()};
    std::shared_ptr<PromiseGate> _closeRelease{std::make_shared<PromiseGate>()};
};
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
        TransportFactory<BlockingTransport> factory{[](BlockingTransport &transport) {
            // Park the reader so the session stays up deterministically instead of
            // racing an immediate EOF; the thread belongs to production, so the release
            // is an RAII guard rather than a Worker the test owns.
            transport.Block(BlockingTransport::Op::Receive);
        }};
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

    // Regression guard: the handshake and the ATT probe are written while the session is
    // being published, before the observable connected flag flips. A gate on the flag
    // rather than on the transport snapshot silently made Connect() unable to send.
    void HandshakeReachesTheInjectedTransportBeforeTheSessionIsConnected()
    {
        TransportFactory<BlockingTransport> factory;
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        const Core::AAP::AAPAttempt attempt{42, 1};
        QVERIFY(manager.Connect(attempt));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);

        const auto sent = transport->SentRecords();
        QCOMPARE(sent.size(), size_t{4});
        QCOMPARE(sent[0], Core::AAP::Packets::Handshake);
        QCOMPARE(sent[1], Core::AAP::Packets::EnableFeatures);
        QCOMPARE(sent[2], Core::AAP::Packets::RequestNotifications);
        QCOMPARE(sent[3], AAPTestSupport::Records::AttReadRequest());

        QVERIFY(DisconnectWithin(manager, factory));
        QVERIFY(!manager.IsConnected());
    }

    // Initialization uses the pinned raw sender, but every later send must still honor
    // the connected gate. Hold teardown inside Close and prove neither path writes.
    void PublicSendIsRefusedDuringTeardown()
    {
        auto closeEntered = std::make_shared<PromiseGate>();
        auto closeRelease = std::make_shared<PromiseGate>();
        TransportFactory<CloseGatedTransport> factory{[&](CloseGatedTransport &transport) {
            transport.SetCloseGates(closeEntered, closeRelease);
            transport.Block(BlockingTransport::Op::Receive);
        }};
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

        QVERIFY(manager.Connect(Core::AAP::AAPAttempt{42, 1}));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        const auto sendsBeforeTeardown = transport->CallCount(BlockingTransport::Op::Send);
        QCOMPARE(sendsBeforeTeardown, size_t{4});

        Worker disconnect{
            [&] { manager.Disconnect(); },
            [&] {
                closeRelease->Signal();
                factory.ReleaseAllBuilt();
            }};
        QVERIFY(closeEntered->Wait(2s));
        QVERIFY(!manager.IsConnected());
        QVERIFY(!manager._transport.load());

        QVERIFY(!manager.SendPacket(AAPTestSupport::Records::NoiseControl(0x01)));
        QVERIFY(!manager.SetNoiseControlMode(Core::AAP::NoiseControlMode::Off));
        QVERIFY(!manager.StartHeadTracking());
        QCOMPARE(transport->CallCount(BlockingTransport::Op::Send), sendsBeforeTeardown);

        closeRelease->Signal();
        QVERIFY(disconnect.Finished(2s));
        QVERIFY(!manager._readerThread.joinable());
    }

    // Single-flight: the lifecycle mutex must serialize whole attempts, so concurrent
    // requests build exactly one transport and yield exactly one session.
    void ConcurrentConnectRequestsProduceOneSession()
    {
        TransportFactory<BlockingTransport> factory{
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); }};
        Core::AAP::Manager manager;
        manager._transportFactoryForTest = [&factory] { return factory.Build(); };

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

        // The first attempt owns the lifecycle mutex and is parked before publication.
        // Only then start the contender, so a sequential implementation cannot pass.
        QVERIFY(factory.WaitForCount(1, 2s));
        auto transport = factory.Last();
        QVERIFY(transport != nullptr);
        QVERIFY(transport->WaitForParked(BlockingTransport::Op::Connect, 2s));
        QVERIFY(!manager._transport.load());

        PromiseGate secondStarted;
        Worker secondConnect{
            [&] {
                secondStarted.Signal();
                connect(2, second);
            },
            [&] {
                manager.CancelConnect();
                factory.ReleaseAllBuilt();
            }};
        QVERIFY(secondStarted.Wait(2s));
        // Positive contention proof: the contender has entered Connect while the first is
        // still parked, yet no second transport has been built or published.
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
        TransportFactory<BlockingTransport> factory{
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Connect); }};
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

    void CallerCancellationWakesConnectAndJoinsTheWorker()
    {
        TransportFactory<BlockingTransport> factory{
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Connect); }};
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
        TransportFactory<BlockingTransport> factory;
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
        QCOMPARE(first->CallCount(BlockingTransport::Op::Receive), size_t{1});
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
        TransportFactory<BlockingTransport> factory{
            [](BlockingTransport &transport) { transport.Block(BlockingTransport::Op::Receive); }};
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
