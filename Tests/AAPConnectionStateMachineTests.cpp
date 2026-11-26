#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <QtTest>

#include "AAPTestSupport.h"
#include "Source/Core/AAPConnectionStateMachine.h"

using Core::AAP::AAPAttempt;
using Core::AAP::AAPConnectionState;
using Core::AAP::AAPConnectionStateMachine;
using namespace std::chrono_literals;

namespace {
double NoJitter()
{
    return 0.5;
}
double MinimumJitter()
{
    return 0.0;
}
double MaximumJitter()
{
    return 1.0;
}
} // namespace

class AAPConnectionStateMachineTests : public QObject
{
    Q_OBJECT

private slots:
    // The state machine is deliberately caller-serialized, so this exercises the policy
    // under an external lock rather than racing it: a genuinely concurrent Begin would be
    // a data race, not a supported use. Real thread-level single-flight is covered by
    // AAPManagerWorkerTests::ConcurrentConnectRequestsProduceOneSession.
    void ConcurrentConnectStartsExactlyOneAttempt()
    {
        // The state machine intentionally has no internal lock; callers serialize it.
        AAPConnectionStateMachine machine{NoJitter};
        std::mutex mutex;
        std::atomic<int> starts{0};
        std::atomic<int> staleGenerations{0};
        std::vector<AAPAttempt> attempts(1000);
        std::vector<std::jthread> threads;
        for (size_t i = 0; i < attempts.size(); ++i) {
            threads.emplace_back([&, i] {
                std::lock_guard lock{mutex};
                const auto result = machine.Begin(42);
                attempts[i] = result.attempt;
                starts += result.started;
                staleGenerations += result.started && result.attempt.generation != 1;
            });
        }
        threads.clear();
        QCOMPARE(starts.load(), 1);
        QCOMPARE(staleGenerations.load(), 0);
        QCOMPARE(machine.State(), AAPConnectionState::Connecting);
    }

    void BackoffRetryAllocatesFreshGeneration()
    {
        AAPConnectionStateMachine machine{NoJitter};
        const auto first = machine.Begin(42).attempt;
        QVERIFY(machine.Connected(first, AAPConnectionStateMachine::Clock::now()));
        const auto retryDelay = machine.Lost(first, AAPConnectionStateMachine::Clock::now() + 1s);
        QVERIFY(retryDelay.has_value());
        const auto second = machine.Begin(42).attempt;
        QVERIFY(second.generation > first.generation);
        QVERIFY(machine.IsCurrent(second));
        QVERIFY(!machine.IsCurrent(first));
    }

    void BackoffRebindUsesNewAddressAndFreshGeneration()
    {
        AAPConnectionStateMachine machine{NoJitter};
        constexpr uint64_t deviceA = 0x340E22CC08A0ULL;
        constexpr uint64_t deviceB = 0xD4DA214FC3E5ULL;
        const auto first = machine.Begin(deviceA).attempt;
        QVERIFY(machine.Connected(first, AAPConnectionStateMachine::Clock::now()));
        QVERIFY(machine.Lost(first, AAPConnectionStateMachine::Clock::now() + 1s).has_value());
        QCOMPARE(machine.State(), AAPConnectionState::Backoff);

        // Begin owns attempt allocation. A rebind during backoff must not reuse
        // the retired A attempt (the old P2 regression) or retain its address.
        const auto rebound = machine.Begin(deviceB);
        QVERIFY(rebound.started);
        QCOMPARE(rebound.attempt.address, deviceB);
        QVERIFY(rebound.attempt.generation > first.generation);
        QVERIFY(machine.IsCurrent(rebound.attempt));
        QVERIFY(!machine.IsCurrent(first));
        QVERIFY(!machine.Connected(first, AAPConnectionStateMachine::Clock::now()));
        QVERIFY(!machine.Lost(first, AAPConnectionStateMachine::Clock::now()).has_value());
    }

    void OldGenerationCannotCompleteOrDisconnectNewAttempt()
    {
        AAPTestSupport::ManualClock clock;
        AAPConnectionStateMachine machine{NoJitter};
        const auto old = machine.Begin(42).attempt;
        QVERIFY(machine.TransportConnected(old));
        QVERIFY(machine.Connected(old, clock.Now()));
        QVERIFY(machine.Stop());
        QVERIFY(machine.FinishStop());
        const auto current = machine.Begin(42).attempt;
        QVERIFY(current.generation > old.generation);
        QVERIFY(!machine.Connected(old, clock.Now()));
        QVERIFY(!machine.Lost(old, clock.Now()).has_value());
        QVERIFY(machine.IsCurrent(current));
    }

    void ExplicitDisconnectSuppressesRetryAndStaleResults()
    {
        AAPConnectionStateMachine machine{NoJitter};
        AAPTestSupport::ManualClock clock;
        const auto attempt = machine.Begin(42).attempt;
        QVERIFY(machine.Stop());
        QVERIFY(!machine.Lost(attempt, clock.Now()).has_value());
        QVERIFY(!machine.Connected(attempt, clock.Now()));
        QVERIFY(machine.FinishStop());
        QCOMPARE(machine.State(), AAPConnectionState::Idle);
    }

    // Backoff is asserted against a hand-advanced clock, so the ladder and the jitter
    // bounds are exact instead of "somewhere near 1 s".
    void UnexpectedDisconnectsUseCappedJitteredBackoff()
    {
        AAPTestSupport::ManualClock clock;
        AAPConnectionStateMachine machine{NoJitter};
        const std::vector<std::chrono::milliseconds> expected{1000ms,  2000ms,  4000ms, 8000ms,
                                                              16000ms, 30000ms, 30000ms};
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto attempt = machine.Begin(42).attempt;
            QVERIFY(machine.Connected(attempt, clock.Now()));
            QCOMPARE(machine.Lost(attempt, clock.Tick(1s)), expected[i]);
            QCOMPARE(machine.State(), AAPConnectionState::Backoff);
        }

        AAPConnectionStateMachine low{MinimumJitter};
        AAPConnectionStateMachine high{MaximumJitter};
        const auto lowAttempt = low.Begin(42).attempt;
        const auto highAttempt = high.Begin(42).attempt;
        QVERIFY(low.Connected(lowAttempt, clock.Now()));
        QVERIFY(high.Connected(highAttempt, clock.Now()));
        clock.Advance(1s);
        QCOMPARE(low.Lost(lowAttempt, clock.Now()), 800ms);
        QCOMPARE(high.Lost(highAttempt, clock.Now()), 1200ms);
    }

    // A reconnect after an unexpected loss must run under a generation the dead session
    // never had; otherwise a late event from the retired reader still looks current and can
    // overwrite the new session.
    void UnexpectedLossReservesAFreshGenerationForTheRetry()
    {
        AAPTestSupport::ManualClock clock;
        AAPConnectionStateMachine machine{NoJitter};
        const auto retired = machine.Begin(42).attempt;
        QVERIFY(machine.Connected(retired, clock.Now()));
        QVERIFY(machine.Lost(retired, clock.Tick(1s)).has_value());

        const auto retry = machine.Begin(42).attempt;
        QVERIFY(machine.State() == AAPConnectionState::Connecting);
        QVERIFY(retry.generation > retired.generation);
        QVERIFY(machine.IsCurrent(retry));
        QVERIFY(!machine.IsCurrent(retired));
        // Events from the retired session are now unambiguously stale.
        QVERIFY(!machine.Connected(retired, clock.Now()));
        QVERIFY(!machine.Lost(retired, clock.Now()).has_value());
        QVERIFY(!machine.TransportConnected(retired));
    }

    void StableConnectionResetsBackoffAfterSixtySeconds()
    {
        AAPTestSupport::ManualClock clock;
        AAPConnectionStateMachine machine{NoJitter};
        auto attempt = machine.Begin(42).attempt;
        QVERIFY(machine.Connected(attempt, clock.Now()));
        QCOMPARE(machine.Lost(attempt, clock.Tick(1s)), 1000ms);
        attempt = machine.Begin(42).attempt;
        QVERIFY(machine.Connected(attempt, clock.Now()));
        QCOMPARE(machine.Lost(attempt, clock.Tick(61s)), 1000ms);
    }
};

QTEST_GUILESS_MAIN(AAPConnectionStateMachineTests)
#include "AAPConnectionStateMachineTests.moc"
