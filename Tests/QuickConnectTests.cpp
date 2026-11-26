//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
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

#include <QtTest>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>

#include "Source/Core/AirPods.h"
#include "Source/Core/PlaybackMonitor.h"
#include "Source/Core/QuickConnect.h"
#include "Source/Gui/TrayActivation.h"

using namespace std::chrono_literals;

namespace {

using Core::BluetoothLink::ServiceStatus;
using Core::QuickConnect::Controller;
using Core::QuickConnect::Device;
using Core::QuickConnect::Outcome;

// Short enough to keep the suite fast, long enough that a queued completion always lands first.
constexpr auto kTestResolveTimeout = 250ms;

class FakeBackend final : public Core::QuickConnect::Backend
{
public:
    std::vector<Device> devices{{"{A}", "AirPods", false}};
    Controller *controller{};
    std::shared_ptr<Core::BluetoothLink::Adapter> linkAdapter;
    std::atomic<int> listCalls{};
    std::atomic<int> reconnectCalls{};
    std::atomic<int> setControllerCalls{};
    std::atomic<int> stopObservingCalls{};

    void SetController(Controller *value) override
    {
        controller = value;
        ++setControllerCalls;
    }

    std::vector<Device> ListDevices(std::stop_token) override
    {
        ++listCalls;
        return devices;
    }

    bool RequestReconnect(const QString &id, std::stop_token) override
    {
        ++reconnectCalls;
        return id == "{A}";
    }

    // Owned by the backend exactly like the Windows backend owns its real
    // adapter, so a detached link worker can never outlive it.
    Core::BluetoothLink::Adapter *LinkAdapter() override
    {
        return linkAdapter ? linkAdapter.get() : nullptr;
    }

    void StopObserving() override
    {
        ++stopObservingCalls;
    }
};

// Scripted stand-in for the BluetoothLink adapter seam: state before/after a
// request, injectable submit failures, and a query mode that parks the worker
// inside a call so tests can prove the deadline backstop and the generation
// discard instead of trusting wall-clock luck.
class FakeLinkAdapter final : public Core::BluetoothLink::Adapter
{
public:
    using State = Core::BluetoothLink::DeviceLinkState;
    using Plan = Core::BluetoothLink::ActionPlan;
    using Failure = Core::BluetoothLink::Failure;

    State before{};
    State after{};
    bool applyAfter{true};
    Failure submitResult{Failure::None};
    std::atomic<bool> failQuery{};
    std::atomic<int> failQueryAt{};
    std::atomic<bool> parkQuery{};
    std::atomic<int> queries{};
    std::atomic<int> submits{};
    std::atomic<bool> parkedEntered{};
    std::atomic<int> parkedWorkers{};
    Plan lastPlan{};

    std::optional<State> Query(uint64_t, std::stop_token) override
    {
        const int call = queries.fetch_add(1) + 1;
        if (failQuery.load() || (failQueryAt.load() > 0 && call == failQueryAt.load())) {
            return std::nullopt;
        }
        if (parkQuery.load() && call == 1) {
            ++parkedWorkers;
            parkedEntered = true;
            // Model an uninterruptible driver call: cancellation is observed
            // only after the call returns, not while it is parked here.
            while (parkQuery.load()) {
                std::this_thread::sleep_for(5ms);
            }
            --parkedWorkers;
        }
        return before;
    }

    Failure Submit(uint64_t, const Plan &plan, std::stop_token stopToken) override
    {
        ++submits;
        lastPlan = plan;
        if (stopToken.stop_requested()) {
            return Failure::Cancelled;
        }
        if (submitResult == Failure::None && applyAfter) {
            before = after;
        }
        return submitResult;
    }
};

Core::BluetoothLink::DeviceLinkState MakeLinkState(
    bool known, Core::BluetoothLink::ServiceStatus a2dp, Core::BluetoothLink::ServiceStatus hfp,
    bool linkReadable = true, bool linkConnected = false)
{
    Core::BluetoothLink::DeviceLinkState state;
    state.deviceKnown = known;
    state.a2dp = a2dp;
    state.hfp = hfp;
    state.linkReadable = linkReadable;
    state.linkConnected = linkConnected;
    return state;
}

// Blocks inside ListDevices until released, so a test can prove the GUI thread was never in there.
class BlockingBackend final : public Core::QuickConnect::Backend
{
public:
    std::promise<void> entered;
    bool connected{};
    std::shared_ptr<Core::BluetoothLink::Adapter> linkAdapter;

    std::vector<Device> ListDevices(std::stop_token stopToken) override
    {
        std::stop_callback cancel{stopToken, [this] { Unblock(); }};
        entered.set_value();
        _release.wait();
        return {{"{A}", "AirPods", connected}};
    }

    bool RequestReconnect(const QString &, std::stop_token) override
    {
        return false;
    }

    Core::BluetoothLink::Adapter *LinkAdapter() override
    {
        return linkAdapter ? linkAdapter.get() : nullptr;
    }

    void Unblock()
    {
        std::call_once(_releaseOnce, [this] { _releasePromise.set_value(); });
    }

private:
    std::promise<void> _releasePromise;
    std::shared_future<void> _release{_releasePromise.get_future()};
    std::once_flag _releaseOnce;
};

class BackendUnblockGuard final
{
public:
    explicit BackendUnblockGuard(std::shared_ptr<BlockingBackend> backend)
        : _backend{std::move(backend)}
    {
    }
    ~BackendUnblockGuard()
    {
        _backend->Unblock();
    }

private:
    std::shared_ptr<BlockingBackend> _backend;
};

} // namespace

class QuickConnectTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // Playback auto-connect
    void playbackConnectsIndependentlyOfTrayAndDeduplicates();
    void automaticConnectedCheckIsSilent();
    void manualClickJoinsAutomaticConnectedCheck();
    void disabledOrUnconfiguredAutomaticConnectDoesNothing();
    void automaticFailureRequiresCooldownAndNewPlayback();
    void automaticUnavailableRequiresCooldownAndNewPlayback();
    void automaticTimeoutRequiresCooldownAndNewPlayback();
    void playbackMonitorDebouncesSilenceAndStopsWhenDisabled();

    // Controller
    void disabledRequestDoesNotCallBackend();
    void emptyDeviceIdReturnsNoDevice();
    void configuredButMissingDeviceReportsUnavailable();
    void connectedDeviceReturnsAlreadyConnected();
    void failedReconnectReturnsFailed();
    void repeatedClicksAlwaysReportProgress();
    void pendingRequestKeepsOriginalDevice();
    void endpointActivationCompletesRequest();
    void resolveTimerFiresOnItsOwn();
    void completionStopsBackendObservation();
    void controllerDetachesBackendOnDestruction();

    // Threading
    void deviceRefreshDoesNotBlockTheGuiThread();
    void reconnectRequestDoesNotBlockTheGuiThread();
    void controllerDestructionCancelsInFlightRefresh();

    // Explicit link control (blueprint 2.8)
    void linkPlanAndSuppressionArePure();
    void linkConnectHappyPath();
    void linkDisconnectSuppressesAutomaticReconnect();
    void manualQuickConnectLiftsSuppression();
    void linkServiceSwitchFailureIsReported();
    void linkRequestHitsFiveSecondBudgetOnce();
    void parkedLinkWorkerCannotPostAfterTimeout();
    void repeatedLinkClicksJoinSameDirectionAndOppositeRetires();
    void linkUnsupportedServiceIsNotGuessed();
    void linkWithoutBoundDeviceReportsNoDevice();
    void linkUnreadableEnumerationIsNotDisconnected();
    void endpointEvidenceDoesNotUseActiveAlone();
    void unknownHfpCanReconnect();
    void disconnectRequiresPositiveProof();
    void unreadableReadbackIsNotDisconnected();
    void multiRadioEnumerationClosesEveryHandle();
    void cancellableSleepStopCallbackRace();
    void timedOutLinkRetryDoesNotJoinBlockedWorker();
    void explicitDisconnectRetiresQuickConnectAndBlocksStaleConnected();
    void deviceConnectionSignalRefreshesCachedLinkState();
    void rebindingRetiresInFlightLinkRequest();
    void rebindingResetsManagerSuppression();

    // Tray activation
    void singleClickOpensMainWindowWhenQuickConnectIsDisabled();
    void singleClickRequestsQuickConnectImmediately();
    void doubleClickDoesNotOpenMainWindow();
    void contextMenuDoesNotRequestQuickConnect();
    void unknownActivationDoesNothing();
};

void QuickConnectTests::initTestCase()
{
    qRegisterMetaType<Outcome>();
    qRegisterMetaType<Core::BluetoothLink::Outcome>();
}

//////////////////////////////////////////////////
// Playback auto-connect
//

void QuickConnectTests::playbackConnectsIndependentlyOfTrayAndDeduplicates()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    controller.SetDeviceId("{A}");
    controller.SetAudioAutoConnectEnabled(true);
    QVERIFY(!controller.IsEnabled());
    QSignalSpy outcomes{&controller, &Controller::OutcomeChanged};
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE(outcomes.count(), 1);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::RequestStarted);
    controller.OnPlaybackStateChanged(true);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    controller.OnEndpointStateChanged("{A}", true);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::Connected);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->listCalls.load(), 1);
}

void QuickConnectTests::automaticConnectedCheckIsSilent()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->devices.front().connected = true;
    Controller controller{backend, kTestResolveTimeout};
    controller.SetDeviceId("{A}");
    controller.SetAudioAutoConnectEnabled(true);
    QSignalSpy outcomes{&controller, &Controller::OutcomeChanged};
    controller.OnPlaybackStateChanged(true);
    QTRY_VERIFY(!controller.IsBusy());
    QCOMPARE(backend->listCalls.load(), 1);
    QCOMPARE(outcomes.count(), 0);
    controller.SetEnabled(true);
    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QTRY_VERIFY(
        !outcomes.isEmpty() && outcomes.last().at(0).value<Outcome>() == Outcome::AlreadyConnected);
    QCOMPARE(backend->reconnectCalls.load(), 0);
    QCOMPARE(outcomes.count(), 1);
}

void QuickConnectTests::manualClickJoinsAutomaticConnectedCheck()
{
    auto backend = std::make_shared<BlockingBackend>();
    backend->connected = true;
    BackendUnblockGuard guard{backend};
    Controller controller{backend, kTestResolveTimeout};
    controller.SetDeviceId("{A}");
    controller.SetEnabled(true);
    controller.SetAudioAutoConnectEnabled(true);
    QSignalSpy outcomes{&controller, &Controller::OutcomeChanged};
    controller.OnPlaybackStateChanged(true);
    auto entered = backend->entered.get_future();
    QVERIFY(entered.wait_for(1s) == std::future_status::ready);
    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    backend->Unblock();
    QTRY_VERIFY(!controller.IsBusy());
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::AlreadyConnected);
}

void QuickConnectTests::disabledOrUnconfiguredAutomaticConnectDoesNothing()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");
    controller.OnPlaybackStateChanged(true);
    controller.SetDeviceId({});
    controller.SetAudioAutoConnectEnabled(true);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QTest::qWait(20);
    QCOMPARE(backend->listCalls.load(), 0);
}

void QuickConnectTests::automaticFailureRequiresCooldownAndNewPlayback()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->devices = {{"{B}", "Unavailable headphones", false}};
    Controller controller{backend, kTestResolveTimeout, 80ms};
    controller.SetDeviceId("{B}");
    controller.SetAudioAutoConnectEnabled(true);
    QSignalSpy outcomes{&controller, &Controller::OutcomeChanged};
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE(outcomes.count(), 1);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::Failed);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    QTest::qWait(100);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE(outcomes.count(), 2);
    QCOMPARE(backend->reconnectCalls.load(), 2);
    controller.SetAudioAutoConnectEnabled(false);
    QTest::qWait(100);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->reconnectCalls.load(), 2);
}

void QuickConnectTests::automaticUnavailableRequiresCooldownAndNewPlayback()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout, 80ms};
    controller.SetDeviceId("{Missing}");
    controller.SetAudioAutoConnectEnabled(true);
    QSignalSpy outcomes{&controller, &Controller::OutcomeChanged};
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE(outcomes.count(), 1);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::DeviceUnavailable);
    QCOMPARE(backend->listCalls.load(), 1);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->listCalls.load(), 1);
    QTest::qWait(100);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->listCalls.load(), 1);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE(outcomes.count(), 2);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::DeviceUnavailable);
    QCOMPARE(backend->listCalls.load(), 2);
    QCOMPARE(backend->reconnectCalls.load(), 0);
}

void QuickConnectTests::automaticTimeoutRequiresCooldownAndNewPlayback()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout, 80ms};
    controller.SetDeviceId("{A}");
    controller.SetAudioAutoConnectEnabled(true);
    QSignalSpy outcomes{&controller, &Controller::OutcomeChanged};
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE(outcomes.count(), 1);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::RequestStarted);
    QTRY_COMPARE_WITH_TIMEOUT(outcomes.count(), 2, 2'000);
    QCOMPARE(outcomes.at(1).at(0).value<Outcome>(), Outcome::TimedOut);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    QTest::qWait(100);
    controller.OnPlaybackStateChanged(true);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QTRY_COMPARE_WITH_TIMEOUT(outcomes.count(), 4, 2'000);
    QCOMPARE(outcomes.at(2).at(0).value<Outcome>(), Outcome::RequestStarted);
    QCOMPARE(outcomes.at(3).at(0).value<Outcome>(), Outcome::TimedOut);
    QCOMPARE(backend->reconnectCalls.load(), 2);
    QCOMPARE(controller.OnEndpointStateChanged("{A}", true), Outcome::Connected);
    QCOMPARE(outcomes.count(), 5);
    QCOMPARE(outcomes.last().at(0).value<Outcome>(), Outcome::Connected);
}

void QuickConnectTests::playbackMonitorDebouncesSilenceAndStopsWhenDisabled()
{
    std::atomic<int> sample{1};
    std::atomic<int> polls{};
    Core::GlobalMedia::PlaybackMonitor monitor{
        [&]() -> std::optional<bool> {
            ++polls;
            const int value = sample.load();
            return value < 0 ? std::nullopt : std::optional<bool>{value != 0};
        },
        5ms, 80ms};
    QSignalSpy changed{&monitor, &Core::GlobalMedia::PlaybackMonitor::PlaybackChanged};
    QTest::qWait(10);
    QCOMPARE(polls.load(), 0);
    monitor.SetEnabled(true);
    QTRY_COMPARE(changed.count(), 1);
    QCOMPARE(changed.last().at(0).toBool(), true);
    sample = 0;
    QTest::qWait(20);
    sample = 1;
    QTest::qWait(20);
    QCOMPARE(changed.count(), 1);
    sample = -1;
    QTest::qWait(100);
    QCOMPARE(changed.count(), 1);
    sample = 0;
    QTRY_COMPARE(changed.count(), 2);
    QCOMPARE(changed.last().at(0).toBool(), false);
    monitor.SetEnabled(false);
    const int stoppedPolls = polls.load();
    sample = 1;
    QTest::qWait(30);
    QCOMPARE(polls.load(), stoppedPolls);
    QCOMPARE(changed.count(), 2);
    monitor.SetEnabled(true);
    QTRY_COMPARE(changed.count(), 3);
}

//////////////////////////////////////////////////
// Controller
//

void QuickConnectTests::disabledRequestDoesNotCallBackend()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::Disabled);
    QCOMPARE(backend->reconnectCalls.load(), 0);
}

void QuickConnectTests::emptyDeviceIdReturnsNoDevice()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    controller.SetEnabled(true);

    QCOMPARE(controller.Request(), Outcome::NoDevice);
    QCOMPARE(backend->reconnectCalls.load(), 0);
}

// "Nothing configured" and "the configured device is not around" are different problems and the
// tray says different things about them.
void QuickConnectTests::configuredButMissingDeviceReportsUnavailable()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{Missing}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QVERIFY(spy.wait(2'000));
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::DeviceUnavailable);
    QCOMPARE(backend->reconnectCalls.load(), 0);
}

void QuickConnectTests::connectedDeviceReturnsAlreadyConnected()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->devices = {{"{A}", "AirPods", true}};
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QVERIFY(spy.wait(2'000));
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::AlreadyConnected);
    QCOMPARE(backend->reconnectCalls.load(), 0);
}

void QuickConnectTests::failedReconnectReturnsFailed()
{
    auto backend = std::make_shared<FakeBackend>();
    backend->devices = {{"{B}", "AirPods Pro", false}};
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{B}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QVERIFY(spy.wait(2'000));
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::Failed);
    QCOMPARE(backend->reconnectCalls.load(), 1);
}

// An impatient user clicking again must always get an answer, and must never start a second
// reconnect - whichever internal phase the first one happens to be in.
void QuickConnectTests::repeatedClicksAlwaysReportProgress()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    // First click starts the worker; the second lands while it is still running.
    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::RequestStarted);

    // Wait for the worker's completion, then click again while the resolve timer is pending.
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 2'000);
    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QCOMPARE(spy.count(), 3);

    QCOMPARE(backend->listCalls.load(), 1);
    QCOMPARE(backend->reconnectCalls.load(), 1);
}

void QuickConnectTests::pendingRequestKeepsOriginalDevice()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000);

    // Changing the selection mid-flight must not retarget the request already in progress.
    controller.SetDeviceId("{B}");
    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QCOMPARE(backend->listCalls.load(), 1);
    QCOMPARE(backend->reconnectCalls.load(), 1);
    QCOMPARE(controller.OnEndpointStateChanged("{A}", true), Outcome::Connected);
}

void QuickConnectTests::endpointActivationCompletesRequest()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000);
    spy.clear();

    // Windows reports the container id in canonical `{GUID}` form; matching is case-insensitive.
    QCOMPARE(controller.OnEndpointStateChanged("a", true), Outcome::Connected);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::Connected);
    QCOMPARE(spy.at(0).at(1).toString(), QString{"AirPods"});

    // A repeat notification for an already-completed request changes nothing.
    QCOMPARE(controller.OnEndpointStateChanged("{A}", true), Outcome::Connected);
    QCOMPARE(spy.count(), 1);
}

// Drives the real QTimer the constructor wires up, rather than calling the slot by hand.
void QuickConnectTests::resolveTimerFiresOnItsOwn()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000);
    spy.clear();

    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5'000);
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::TimedOut);
    QCOMPARE(spy.at(0).at(1).toString(), QString{"AirPods"});

    // The timer is single-shot; nothing else arrives.
    QCOMPARE(controller.ResolveTimedOut(), Outcome::TimedOut);
    QCOMPARE(spy.count(), 1);
}

// Leaving the backend observing after a request resolves is what made it keep doing expensive work
// for every audio device on the machine.
void QuickConnectTests::completionStopsBackendObservation()
{
    auto backend = std::make_shared<FakeBackend>();
    Controller controller{backend, kTestResolveTimeout};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000);
    QCOMPARE(backend->stopObservingCalls.load(), 0);

    QCOMPARE(controller.OnEndpointStateChanged("{A}", true), Outcome::Connected);
    QCOMPARE(backend->stopObservingCalls.load(), 1);
}

void QuickConnectTests::controllerDetachesBackendOnDestruction()
{
    auto backend = std::make_shared<FakeBackend>();
    {
        Controller controller{backend, kTestResolveTimeout};
        QCOMPARE(backend->controller, &controller);
    }

    QVERIFY(backend->controller == nullptr);
    QCOMPARE(backend->setControllerCalls.load(), 2);
}

//////////////////////////////////////////////////
// Explicit link control (blueprint 2.8)
//
// The five-second budget is the production constant; the tests inject a
// shortened one and drive the real QTimer backstop rather than calling the
// slot by hand, same contract as the resolve timer above.
//

constexpr auto kTestLinkDeadline = 200ms;
constexpr auto kTestLinkPoll = 20ms;

namespace {

Core::BluetoothLink::Outcome LastLinkOutcome(const QSignalSpy &spy)
{
    return spy.last().at(0).value<Core::BluetoothLink::Outcome>();
}

int LastLinkFailure(const QSignalSpy &spy)
{
    return spy.last().at(1).toInt();
}

std::shared_ptr<FakeBackend> MakeLinkBackend(std::shared_ptr<FakeLinkAdapter> adapter)
{
    auto backend = std::make_shared<FakeBackend>();
    backend->linkAdapter = std::move(adapter);
    return backend;
}

} // namespace

void QuickConnectTests::linkPlanAndSuppressionArePure()
{
    namespace BL = Core::BluetoothLink;

    QVERIFY(
        BL::BuildPlan(
            MakeLinkState(false, BL::ServiceStatus::Disconnected, BL::ServiceStatus::Unsupported),
            true)
            .decision == BL::Decision::DeviceNotFound);

    const auto connect =
        MakeLinkState(true, BL::ServiceStatus::Disconnected, BL::ServiceStatus::Connected);
    const auto plan = BL::BuildPlan(connect, true);
    QVERIFY(plan.decision == BL::Decision::Proceed && plan.connectA2DP && !plan.connectHFP);
    QVERIFY(plan.TouchesAny());

    const auto disconnect = BL::BuildPlan(
        MakeLinkState(true, BL::ServiceStatus::Connected, BL::ServiceStatus::Connected, true, true),
        false);
    QVERIFY(
        disconnect.decision == BL::Decision::Proceed && disconnect.disconnectA2DP &&
        disconnect.disconnectHFP);

    QVERIFY(
        BL::BuildPlan(
            MakeLinkState(true, BL::ServiceStatus::Unsupported, BL::ServiceStatus::Unavailable),
            true)
            .decision == BL::Decision::NoSupportedService);
    QVERIFY(
        BL::BuildPlan(
            MakeLinkState(true, BL::ServiceStatus::Disconnected, BL::ServiceStatus::Unsupported),
            false)
            .decision == BL::Decision::AlreadySatisfied);

    const auto progress = BL::Evaluate(
        plan,
        MakeLinkState(true, BL::ServiceStatus::Connected, BL::ServiceStatus::Connected, true, true),
        true);
    QVERIFY(progress.evaluation == BL::Evaluation::Satisfied);

    QVERIFY(BL::SuppressionFor(BL::Outcome::Disconnected) == std::optional<bool>{true});
    QVERIFY(BL::SuppressionFor(BL::Outcome::AlreadyDisconnected) == std::optional<bool>{true});
    QVERIFY(BL::SuppressionFor(BL::Outcome::Connected) == std::optional<bool>{false});
    QVERIFY(!BL::SuppressionFor(BL::Outcome::TimedOut).has_value());
}

void QuickConnectTests::linkConnectHappyPath()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QVERIFY(controller.IsLinkBusy());
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Connected);
    QVERIFY(!controller.IsLinkBusy());
    QVERIFY(controller.IsLinkConnected().value_or(false));
    QVERIFY(!controller.IsLinkSuppressed());
    QCOMPARE(adapter->submits.load(), 1);
}

void QuickConnectTests::linkDisconnectSuppressesAutomaticReconnect()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported, true, false);
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetEnabled(true);
    controller.SetAudioAutoConnectEnabled(true);
    controller.SetDeviceId("{A}");
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Disconnected);
    QVERIFY(controller.IsLinkSuppressed());
    QVERIFY(controller.IsLinkConnected() == std::optional<bool>{false});

    // Playback edges must not undo the manual disconnect...
    backend->reconnectCalls = 0;
    controller.OnPlaybackStateChanged(false);
    controller.OnPlaybackStateChanged(true);
    QTest::qWait(50);
    QCOMPARE(backend->reconnectCalls.load(), 0);

    // ...and neither does a natural endpoint-connect event.
    controller.OnEndpointStateChanged("{A}", true);
    QTest::qWait(20);
    QVERIFY(controller.IsLinkSuppressed());
    QCOMPARE(backend->reconnectCalls.load(), 0);
}

void QuickConnectTests::manualQuickConnectLiftsSuppression()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported, true, false);
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetEnabled(true);
    controller.SetAudioAutoConnectEnabled(true);
    controller.SetDeviceId("{A}");
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(controller.IsLinkSuppressed());

    // A manual quick-connect click is the explicit reconnect gesture.
    QVERIFY(controller.Request() == Core::QuickConnect::Outcome::RequestStarted);
    QVERIFY(!controller.IsLinkSuppressed());
}

void QuickConnectTests::linkServiceSwitchFailureIsReported()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->submitResult = Core::BluetoothLink::Failure::RequestRejected;
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Failed);
    QVERIFY(
        LastLinkFailure(spy) == static_cast<int>(Core::BluetoothLink::Failure::RequestRejected));
    QVERIFY(!controller.IsLinkBusy());
    QVERIFY(!controller.IsLinkSuppressed());
    QVERIFY(!controller.IsLinkConnected().has_value()); // no state was proven
}

void QuickConnectTests::linkRequestHitsFiveSecondBudgetOnce()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->applyAfter = false; // request accepted, link never comes up
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000);
    QVERIFY(LastLinkOutcome(spy) == Outcome::TimedOut);
    QVERIFY(LastLinkFailure(spy) == static_cast<int>(Core::BluetoothLink::Failure::Timeout));

    // Exactly one answer per request: worker deadline and timer backstop race
    // intentionally, the generation guard makes the loser silent.
    QTest::qWait(300);
    QCOMPARE(spy.count(), 1);
    QVERIFY(!controller.IsLinkBusy());
}

void QuickConnectTests::parkedLinkWorkerCannotPostAfterTimeout()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->parkQuery = true;
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, 100ms, kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QTRY_VERIFY(adapter->parkedEntered.load());
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000); // the backstop answers the user
    QVERIFY(LastLinkOutcome(spy) == Outcome::TimedOut);

    adapter->parkQuery = false; // release the worker; its post is stale now
    QTest::qWait(200);
    QCOMPARE(spy.count(), 1);

    // A fresh request owns a fresh generation and completes normally.
    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Connected);
}

void QuickConnectTests::repeatedLinkClicksJoinSameDirectionAndOppositeRetires()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->parkQuery = true;
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, 5'000ms, kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QTRY_VERIFY(adapter->parkedEntered.load());

    // Same-direction clicks join the parked request; the opposite direction
    // retires it and starts a new logical request without joining the driver.
    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QCOMPARE(adapter->submits.load(), 0);
    QVERIFY(controller.IsLinkDisconnectPending());

    // The disconnect observes the still-disconnected link and completes before
    // the retired connect is released. Only the newer request may speak.
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::AlreadyDisconnected);
    QVERIFY(controller.IsLinkSuppressed());
    QVERIFY(!controller.IsLinkBusy());

    adapter->parkQuery = false;
    QTRY_COMPARE_WITH_TIMEOUT(adapter->parkedWorkers.load(), 0, 2'000);
    QTest::qWait(50);
    QCOMPARE(adapter->submits.load(), 0);
    QCOMPARE(spy.count(), 2); // joined-request feedback, then the terminal result
}

void QuickConnectTests::linkUnsupportedServiceIsNotGuessed()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Unsupported, ServiceStatus::Unavailable);
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::NoSupportedService);
    QCOMPARE(adapter->submits.load(), 0);
}

void QuickConnectTests::linkWithoutBoundDeviceReportsNoDevice()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkDisconnect() == Outcome::NoDevice);
    QCOMPARE(spy.count(), 1);
    QVERIFY(LastLinkOutcome(spy) == Outcome::NoDevice);
    QCOMPARE(adapter->queries.load(), 0);
    QCOMPARE(adapter->submits.load(), 0);
}

void QuickConnectTests::linkUnreadableEnumerationIsNotDisconnected()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->failQuery = true;
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Failed);
    QVERIFY(LastLinkFailure(spy) == static_cast<int>(Core::BluetoothLink::Failure::Unreadable));
    QVERIFY(!controller.IsLinkSuppressed());
    QVERIFY(!controller.IsLinkConnected().has_value());
    QCOMPARE(adapter->submits.load(), 0);

    // A successfully enumerated empty endpoint set is different from a failed
    // enumeration: it is valid evidence that no controllable profile exists.
    adapter->failQuery = false;
    adapter->before = MakeLinkState(true, ServiceStatus::Unsupported, ServiceStatus::Unsupported);
    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::AlreadyDisconnected);
    QVERIFY(controller.IsLinkSuppressed());
}

void QuickConnectTests::endpointEvidenceDoesNotUseActiveAlone()
{
    using Core::BluetoothLink::EndpointEvidence;
    using Core::BluetoothLink::ServiceStatus;
    std::vector<EndpointEvidence> evidence{{true, true, std::nullopt}};
    QVERIFY(Core::BluetoothLink::AggregateEndpointEvidence(evidence) == ServiceStatus::Unknown);
    evidence.front().connected = false;
    QVERIFY(
        Core::BluetoothLink::AggregateEndpointEvidence(evidence) == ServiceStatus::Disconnected);
    evidence.front().available = false;
    QVERIFY(Core::BluetoothLink::AggregateEndpointEvidence(evidence) == ServiceStatus::Unavailable);
    evidence.front().available = true;
    evidence.front().connected = true;
    QVERIFY(Core::BluetoothLink::AggregateEndpointEvidence(evidence) == ServiceStatus::Connected);
    evidence.front().controllable = false;
    QVERIFY(Core::BluetoothLink::AggregateEndpointEvidence(evidence) == ServiceStatus::Unsupported);
}

void QuickConnectTests::unknownHfpCanReconnect()
{
    auto state = MakeLinkState(true, ServiceStatus::Unsupported, ServiceStatus::Unknown);
    const auto plan = Core::BluetoothLink::BuildPlan(state, true);
    QVERIFY(plan.decision == Core::BluetoothLink::Decision::Proceed);
    QVERIFY(!plan.connectA2DP && plan.connectHFP);
    QVERIFY(Core::BluetoothLink::BuildPlan(state, false).disconnectHFP);
}

void QuickConnectTests::disconnectRequiresPositiveProof()
{
    namespace BL = Core::BluetoothLink;
    auto state = MakeLinkState(true, BL::ServiceStatus::Connected, BL::ServiceStatus::Unsupported);
    const auto plan = BL::BuildPlan(state, false);
    auto latest =
        MakeLinkState(true, BL::ServiceStatus::Unsupported, BL::ServiceStatus::Unsupported);
    QVERIFY(BL::Evaluate(plan, latest, false).evaluation == BL::Evaluation::Pending);
    latest.a2dp = BL::ServiceStatus::Unknown;
    QVERIFY(BL::Evaluate(plan, latest, false).evaluation == BL::Evaluation::Pending);
    latest.a2dp = BL::ServiceStatus::Unavailable;
    QVERIFY(BL::Evaluate(plan, latest, false).evaluation == BL::Evaluation::Pending);
    latest.a2dp = BL::ServiceStatus::Disconnected;
    QVERIFY(BL::Evaluate(plan, latest, false).evaluation == BL::Evaluation::Satisfied);
}

void QuickConnectTests::unreadableReadbackIsNotDisconnected()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->after = MakeLinkState(true, ServiceStatus::Unsupported, ServiceStatus::Unsupported);
    adapter->failQueryAt = 2; // initial query succeeds, first read-back is unreadable
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, kTestLinkDeadline,
        kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Failed);
    QVERIFY(LastLinkFailure(spy) == static_cast<int>(Core::BluetoothLink::Failure::Unreadable));
    QVERIFY(!controller.IsLinkSuppressed());
    QCOMPARE(adapter->submits.load(), 1);
}

void QuickConnectTests::multiRadioEnumerationClosesEveryHandle()
{
    struct FakeRadios {
        int search{1};
        int radio{};
        int index{};
        int closedSearches{};
        int closedRadios{};
        int visited{};
        uint32_t error{259};
    } fake;

    const auto result = Core::BluetoothLink::Details::EnumerateRadioHandles<int, int>(
        [&](int *radio) {
            fake.radio = ++fake.index;
            *radio = fake.radio;
            return fake.search;
        },
        [&](int, int *radio) {
            if (fake.index == 3) {
                return false;
            }
            fake.radio = ++fake.index;
            *radio = fake.radio;
            return true;
        },
        [&](int) { ++fake.closedSearches; }, [&](int) { ++fake.closedRadios; },
        [&] { return fake.error; },
        [&](int) {
            ++fake.visited;
            return true;
        });

    QVERIFY(result.started && result.completed);
    QCOMPARE(result.count, size_t{3});
    QCOMPARE(fake.visited, 3);
    QCOMPARE(fake.closedRadios, 3);
    QCOMPARE(fake.closedSearches, 1);

    int failedRadioClosed{};
    const auto failed = Core::BluetoothLink::Details::EnumerateRadioHandles<int, int>(
        [](int *radio) {
            *radio = 7;
            return 0;
        },
        [](int, int *) { return false; }, [](int) { ++failedRadioClosed; },
        [](int) { ++failedRadioClosed; }, [] { return uint32_t{5}; }, [](int) { return true; });
    QVERIFY(!failed.started);
    QCOMPARE(failedRadioClosed, 1);
}

void QuickConnectTests::cancellableSleepStopCallbackRace()
{
    std::vector<std::jthread> workers;
    std::vector<std::stop_source> stops(64);
    for (size_t i = 0; i < stops.size(); ++i) {
        workers.emplace_back(
            [&stops, i] { Core::BluetoothLink::CancellableSleep(5s, stops[i].get_token()); });
    }
    for (auto &stop : stops) {
        stop.request_stop();
    }
    workers.clear();
}

void QuickConnectTests::timedOutLinkRetryDoesNotJoinBlockedWorker()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->parkQuery = true;
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, 100ms, kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QTRY_VERIFY(adapter->parkedEntered.load());
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2'000);
    QVERIFY(LastLinkOutcome(spy) == Outcome::TimedOut);
    QCOMPARE(adapter->parkedWorkers.load(), 1);

    // The first driver call is still parked. A retry must start a fresh logical
    // request immediately rather than joining that worker on the GUI thread.
    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QVERIFY(spy.wait(2'000));
    QVERIFY(LastLinkOutcome(spy) == Outcome::Connected);
    QCOMPARE(adapter->parkedWorkers.load(), 1);
    QCOMPARE(adapter->submits.load(), 1);

    adapter->parkQuery = false;
    QTRY_COMPARE_WITH_TIMEOUT(adapter->parkedWorkers.load(), 0, 2'000);
    QTest::qWait(50);
    QCOMPARE(spy.count(), 2);
}

void QuickConnectTests::explicitDisconnectRetiresQuickConnectAndBlocksStaleConnected()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before =
        MakeLinkState(true, ServiceStatus::Connected, ServiceStatus::Unsupported, true, true);
    adapter->after =
        MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported, true, false);
    auto backend = std::make_shared<BlockingBackend>();
    backend->linkAdapter = adapter;
    BackendUnblockGuard guard{backend};
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, 1'000ms, 5ms};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy linkSpy{&controller, &Controller::LinkOutcomeChanged};
    QSignalSpy quickSpy{&controller, &Controller::OutcomeChanged};

    QVERIFY(controller.Request() == Core::QuickConnect::Outcome::RequestStarted);
    QVERIFY(backend->entered.get_future().wait_for(2s) == std::future_status::ready);
    QVERIFY(controller.RequestLinkDisconnect() == Outcome::Started);
    QVERIFY(!controller.IsBusy());
    QVERIFY(controller.IsLinkDisconnectPending());

    backend->Unblock();
    QVERIFY(linkSpy.wait(2'000));
    QVERIFY(LastLinkOutcome(linkSpy) == Outcome::Disconnected);
    QVERIFY(controller.IsLinkSuppressed());
    QCOMPARE(quickSpy.count(), 0);

    // A late endpoint callback from the retired QuickConnect request cannot
    // complete it or clear the explicit-disconnect suppression.
    controller.OnEndpointStateChanged("{A}", true);
    QCOMPARE(quickSpy.count(), 0);
    QVERIFY(controller.IsLinkSuppressed());
}

void QuickConnectTests::deviceConnectionSignalRefreshesCachedLinkState()
{
    Controller controller{std::make_shared<FakeBackend>(), kTestResolveTimeout};
    QSignalSpy connectionSpy{&controller, &Controller::LinkConnectionChanged};
    controller.SetDeviceId("{A}");
    controller.OnDeviceConnectionChanged(true);
    QVERIFY(controller.IsLinkConnected() == std::optional<bool>{true});
    QCOMPARE(connectionSpy.count(), 1);
    controller.OnDeviceConnectionChanged(false);
    QVERIFY(controller.IsLinkConnected() == std::optional<bool>{false});
    QCOMPARE(connectionSpy.count(), 2);

    // Endpoint notifications use the same tray-refresh path, but duplicate or
    // unrelated events must not churn the action state.
    controller.OnEndpointStateChanged("{A}", true);
    QCOMPARE(connectionSpy.count(), 3);
    controller.OnEndpointStateChanged("{A}", true);
    controller.OnEndpointStateChanged("{other}", false);
    QCOMPARE(connectionSpy.count(), 3);
}

void QuickConnectTests::rebindingResetsManagerSuppression()
{
    Core::AirPods::Details::LinkSuppressionState suppression;
    suppression.BindAddress(0xAABBCCDDEEFFULL);
    suppression.Set(true);
    suppression.BindAddress(0xAABBCCDDEEFFULL);
    QVERIFY(suppression.IsSuppressed());
    suppression.BindAddress(0x112233445566ULL);
    QVERIFY(!suppression.IsSuppressed());
}

void QuickConnectTests::rebindingRetiresInFlightLinkRequest()
{
    using Outcome = Core::BluetoothLink::Outcome;

    auto adapter = std::make_shared<FakeLinkAdapter>();
    adapter->before = MakeLinkState(true, ServiceStatus::Disconnected, ServiceStatus::Unsupported);
    adapter->parkQuery = true;
    auto backend = MakeLinkBackend(adapter);
    Controller controller{
        backend, kTestResolveTimeout, Controller::kAutoRetryCooldown, 5'000ms, kTestLinkPoll};
    controller.SetLinkAddress(0xAABBCCDDEEFFULL);
    QSignalSpy spy{&controller, &Controller::LinkOutcomeChanged};
    QSignalSpy busySpy{&controller, &Controller::LinkBusyChanged};

    QVERIFY(controller.RequestLinkConnect() == Outcome::Started);
    QTRY_VERIFY(adapter->parkedEntered.load());
    QVERIFY(!busySpy.isEmpty() && busySpy.last().at(0).toBool());

    // Binding another device (or unbinding) must not leave a request that can
    // still speak for the previous one, and the tray must be notified.
    controller.SetLinkAddress(0x112233445566ULL);
    QVERIFY(!controller.IsLinkBusy());
    QVERIFY(!busySpy.isEmpty() && !busySpy.last().at(0).toBool());

    adapter->parkQuery = false;
    QTest::qWait(200);
    QCOMPARE(spy.count(), 0);
}

//////////////////////////////////////////////////
// Threading
//

void QuickConnectTests::deviceRefreshDoesNotBlockTheGuiThread()
{
    auto backend = std::make_shared<BlockingBackend>();
    Controller controller{backend, kTestResolveTimeout};
    BackendUnblockGuard unblockGuard{backend};
    QSignalSpy spy{&controller, &Controller::DevicesChanged};

    controller.RefreshDevices();
    QVERIFY(backend->entered.get_future().wait_for(2s) == std::future_status::ready);
    QCOMPARE(spy.count(), 0);

    backend->Unblock();
    QVERIFY(spy.wait(2'000));
    QCOMPARE(controller.Devices().size(), std::size_t{1});
}

void QuickConnectTests::reconnectRequestDoesNotBlockTheGuiThread()
{
    auto backend = std::make_shared<BlockingBackend>();
    Controller controller{backend, kTestResolveTimeout};
    BackendUnblockGuard unblockGuard{backend};
    QSignalSpy spy{&controller, &Controller::OutcomeChanged};
    controller.SetEnabled(true);
    controller.SetDeviceId("{A}");

    QCOMPARE(controller.Request(), Outcome::RequestStarted);
    QVERIFY(backend->entered.get_future().wait_for(2s) == std::future_status::ready);
    QCOMPARE(spy.count(), 0);

    backend->Unblock();
    QVERIFY(spy.wait(2'000));
    QCOMPARE(spy.at(0).at(0).value<Outcome>(), Outcome::Failed);
}

// Quitting while a refresh is in flight must not hang, and must not touch the dead Controller.
void QuickConnectTests::controllerDestructionCancelsInFlightRefresh()
{
    auto backend = std::make_shared<BlockingBackend>();
    {
        Controller controller{backend, kTestResolveTimeout};
        controller.RefreshDevices();
        QVERIFY(backend->entered.get_future().wait_for(2s) == std::future_status::ready);
    }

    QCoreApplication::processEvents();
}

//////////////////////////////////////////////////
// Tray activation
//
// These exercise the same activation decisions that TrayIcon drives.
//

void QuickConnectTests::singleClickOpensMainWindowWhenQuickConnectIsDisabled()
{
    QCOMPARE(
        Gui::GetTrayActivationAction(QSystemTrayIcon::Trigger, false),
        Gui::TrayActivationAction::ShowMainWindow);
}

void QuickConnectTests::singleClickRequestsQuickConnectImmediately()
{
    QCOMPARE(
        Gui::GetTrayActivationAction(QSystemTrayIcon::Trigger, true),
        Gui::TrayActivationAction::QuickConnect);
}

void QuickConnectTests::doubleClickDoesNotOpenMainWindow()
{
    for (const bool enabled : {false, true}) {
        QCOMPARE(
            Gui::GetTrayActivationAction(QSystemTrayIcon::DoubleClick, enabled),
            Gui::TrayActivationAction::None);
    }
}

void QuickConnectTests::contextMenuDoesNotRequestQuickConnect()
{
    QCOMPARE(
        Gui::GetTrayActivationAction(QSystemTrayIcon::Context, true),
        Gui::TrayActivationAction::None);
}

void QuickConnectTests::unknownActivationDoesNothing()
{
    QCOMPARE(
        Gui::GetTrayActivationAction(QSystemTrayIcon::Unknown, true),
        Gui::TrayActivationAction::None);
}

QTEST_GUILESS_MAIN(QuickConnectTests)

#include "QuickConnectTests.moc"
