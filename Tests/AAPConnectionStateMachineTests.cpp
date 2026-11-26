#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <QtTest>

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
    void SerializedConcurrentRequestsStartOneAttempt()
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

    void OldGenerationCannotCompleteOrDisconnectNewAttempt()
    {
        AAPConnectionStateMachine machine{NoJitter};
        const auto old = machine.Begin(42).attempt;
        QVERIFY(machine.TransportConnected(old));
        QVERIFY(machine.Connected(old, AAPConnectionStateMachine::Clock::now()));
        QVERIFY(machine.Stop());
        QVERIFY(machine.FinishStop());
        const auto current = machine.Begin(42).attempt;
        QVERIFY(current.generation > old.generation);
        QVERIFY(!machine.Connected(old, AAPConnectionStateMachine::Clock::now()));
        QVERIFY(!machine.Lost(old, AAPConnectionStateMachine::Clock::now()).has_value());
        QVERIFY(machine.IsCurrent(current));
    }

    void ExplicitDisconnectSuppressesRetryAndStaleResults()
    {
        AAPConnectionStateMachine machine{NoJitter};
        const auto attempt = machine.Begin(42).attempt;
        QVERIFY(machine.Stop());
        QVERIFY(!machine.Lost(attempt, AAPConnectionStateMachine::Clock::now()).has_value());
        QVERIFY(!machine.Connected(attempt, AAPConnectionStateMachine::Clock::now()));
        QVERIFY(machine.FinishStop());
        QCOMPARE(machine.State(), AAPConnectionState::Idle);
    }

    void UnexpectedDisconnectsUseCappedJitteredBackoff()
    {
        AAPConnectionStateMachine machine{NoJitter};
        auto now = AAPConnectionStateMachine::Clock::now();
        const std::vector<std::chrono::milliseconds> expected{1000ms,  2000ms,  4000ms, 8000ms,
                                                              16000ms, 30000ms, 30000ms};
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto attempt = machine.Begin(42).attempt;
            QVERIFY(machine.Connected(attempt, now));
            QCOMPARE(machine.Lost(attempt, now + 1s), expected[i]);
            QCOMPARE(machine.State(), AAPConnectionState::Backoff);
            now += 1s;
        }

        AAPConnectionStateMachine low{MinimumJitter};
        auto attempt = low.Begin(42).attempt;
        QVERIFY(low.Connected(attempt, now));
        QCOMPARE(low.Lost(attempt, now + 1s), 800ms);
        AAPConnectionStateMachine high{MaximumJitter};
        attempt = high.Begin(42).attempt;
        QVERIFY(high.Connected(attempt, now));
        QCOMPARE(high.Lost(attempt, now + 1s), 1200ms);
    }

    void StableConnectionResetsBackoffAfterSixtySeconds()
    {
        AAPConnectionStateMachine machine{NoJitter};
        auto now = AAPConnectionStateMachine::Clock::now();
        auto attempt = machine.Begin(42).attempt;
        QVERIFY(machine.Connected(attempt, now));
        QCOMPARE(machine.Lost(attempt, now + 1s), 1000ms);
        attempt = machine.Begin(42).attempt;
        QVERIFY(machine.Connected(attempt, now + 2s));
        QCOMPARE(machine.Lost(attempt, now + 62s), 1000ms);
    }
};

QTEST_GUILESS_MAIN(AAPConnectionStateMachineTests)
#include "AAPConnectionStateMachineTests.moc"
