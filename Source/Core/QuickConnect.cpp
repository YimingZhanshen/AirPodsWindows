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

#include "QuickConnect.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include <QMetaObject>

#include "../Assert.h"

namespace Core::QuickConnect {
namespace {

QString NormalizeId(QString id)
{
    id = id.trimmed();
    if (id.startsWith('{') && id.endsWith('}')) {
        id = id.mid(1, id.size() - 2);
    }
    return id.toLower();
}

// Blocking reconnect and explicit-link mutations share one process-lifetime queue so
// opposite-direction requests are serialized globally. Driver calls have no reliable
// cancellation point, so the single worker is intentionally never destroyed or joined:
// the OS reclaims it at process exit. Controller generations make stale queued jobs
// return without posting, and a currently blocked job can outlive its Controller only
// through shared backend/link state, never through a raw Controller pointer.
struct MutationWorker {
    MutationWorker() : thread{[this] { Run(); }} {}

    ~MutationWorker() = default;

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> jobs;
    std::thread thread;

    void Run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] { return !jobs.empty(); });
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }
};

MutationWorker &GlobalMutationWorker()
{
    // Deliberately leaked: destroying this at static teardown could join a blocked
    // driver call and freeze process shutdown.
    static auto *worker = new MutationWorker{};
    return *worker;
}

void QueueMutation(std::function<void()> job)
{
    auto &worker = GlobalMutationWorker();
    {
        std::lock_guard lock{worker.mutex};
        worker.jobs.push_back(std::move(job));
    }
    worker.condition.notify_one();
}

} // namespace

void Backend::SetController(Controller *controller)
{
    Q_UNUSED(controller)
}

Controller::Controller(
    std::shared_ptr<Backend> backend, std::chrono::milliseconds resolveTimeout,
    std::chrono::milliseconds autoRetryCooldown, std::chrono::milliseconds linkDeadline,
    std::chrono::milliseconds linkPoll)
    : QObject{nullptr}, _backend{std::move(backend)}, _autoRetryCooldown{autoRetryCooldown},
      _linkDeadline{linkDeadline}, _linkPoll{linkPoll}
{
    APD_ASSERT(_backend != nullptr);

    _link->controller = this;
    _backend->SetController(this);

    _resolveTimer.setSingleShot(true);
    _resolveTimer.setInterval(resolveTimeout);
    QObject::connect(&_resolveTimer, &QTimer::timeout, this, &Controller::ResolveTimedOut);

    // Backstop for the explicit link requests: the worker enforces the same
    // deadline, but a request stuck inside a driver call can only be
    // answered to the GUI by this timer.
    _linkDeadlineTimer.setSingleShot(true);
    _linkDeadlineTimer.setInterval(linkDeadline);
    QObject::connect(&_linkDeadlineTimer, &QTimer::timeout, this, &Controller::ResolveLinkTimedOut);
}

Controller::~Controller()
{
    // Detach queued work from this object first, so anything still running posts nowhere.
    {
        std::lock_guard<std::mutex> lock{_link->mutex};
        _link->controller = nullptr;
    }

    _deviceRefreshThread.request_stop();
    _requestStop.request_stop();
    _linkStop.request_stop();

    // The refresh worker cancels promptly - WinRT enumeration takes a stop callback and the
    // endpoint walk checks between items - so waiting for it costs nothing.
    if (_deviceRefreshThread.joinable()) {
        _deviceRefreshThread.join();
    }

    // Reconnect and link mutations are already retired by their generation/stop tokens and run
    // on the single process-lifetime queue. Joining that queue here could freeze the GUI on an
    // uninterruptible driver call, so it is intentionally left to process exit.
    _backend->SetController(nullptr);
}

void Controller::PostToController(
    const std::shared_ptr<Link> &link, std::function<void(Controller &)> work)
{
    std::lock_guard<std::mutex> lock{link->mutex};
    if (link->controller == nullptr) {
        return;
    }

    QMetaObject::invokeMethod(
        link->controller,
        [controller = link->controller, work = std::move(work)]() { work(*controller); },
        Qt::QueuedConnection);
}

void Controller::SetEnabled(bool enabled)
{
    _enabled = enabled;
}

bool Controller::IsEnabled() const
{
    return _enabled;
}

void Controller::SetAudioAutoConnectEnabled(bool enabled)
{
    _audioAutoConnectEnabled = enabled;
    if (!enabled) {
        _playbackActive = false;
    }
}

bool Controller::IsBusy() const
{
    return _pending || _requestRunning;
}

void Controller::OnPlaybackStateChanged(bool playing)
{
    const bool started = playing && !_playbackActive;
    _playbackActive = playing;
    if (!started || !_audioAutoConnectEnabled || _deviceId.isEmpty() || _pending ||
        _requestRunning || _linkDisconnectRequested ||
        std::chrono::steady_clock::now() < _nextAutoRetry)
    {
        return;
    }
    Request(true);
}

void Controller::SetDeviceId(QString id)
{
    if (_deviceId != id) {
        _lastLinkConnected.reset();
    }
    _deviceId = std::move(id);
}

void Controller::SetLinkAddress(uint64_t address)
{
    if (_linkAddress == address) {
        return;
    }

    // A rebinding invalidates everything the previous device's link proved.
    // The generation bump retires an in-flight worker's post, and the stop
    // token releases it from its poll sleep.
    const bool wasRunning = _linkRunning;
    _linkAddress = address;
    _lastLinkConnected.reset();
    _linkSuppressed = false;
    _linkDisconnectRequested = false;
    ++_linkGeneration;
    _linkStop.request_stop();
    _linkRunning = false;
    _linkDeadlineTimer.stop();
    if (wasRunning) {
        emit LinkBusyChanged(false);
    }
}

std::vector<Device> Controller::Devices()
{
    return _devices;
}

QString Controller::DeviceNameFor(const QString &id) const
{
    const auto iter = std::find_if(_devices.begin(), _devices.end(), [&](const Device &device) {
        return NormalizeId(device.id) == NormalizeId(id);
    });
    return iter != _devices.end() ? iter->name : QString{};
}

void Controller::RefreshDevices()
{
    if (_deviceRefreshRunning.exchange(true)) {
        return;
    }

    if (_deviceRefreshThread.joinable()) {
        _deviceRefreshThread.join();
    }

    _deviceRefreshThread =
        std::jthread{[backend = _backend, link = _link](std::stop_token stopToken) {
            auto devices = backend->ListDevices(stopToken);
            if (stopToken.stop_requested()) {
                return;
            }

            PostToController(link, [devices = std::move(devices)](Controller &self) mutable {
                self._devices = std::move(devices);
                self._deviceRefreshRunning = false;
                emit self.DevicesChanged();
            });
        }};
}

Outcome Controller::Request()
{
    return Request(false);
}

Outcome Controller::Request(bool automatic)
{
    if (automatic && (_linkSuppressed || _linkDisconnectRequested)) {
        // The user explicitly disconnected this device's link during the
        // session. A playback edge must not silently undo that; only an
        // explicit connect request lifts the suppression.
        return Outcome::Disabled;
    }
    if (!automatic && _linkDisconnectRequested) {
        EmitOutcome(Outcome::Disabled, {});
        return Outcome::Disabled;
    }
    if (!automatic) {
        _linkSuppressed = false;
    }

    if (!automatic && IsBusy()) {
        _manualRequestJoined = true;
    }
    // A click while work is already under way always answers with the same outcome, whether that
    // work is the enumeration/reconnect round trip or the wait for the endpoint to come up. The
    // caller decides how loudly to report it.
    if (_pending) {
        EmitOutcome(Outcome::RequestStarted, _pendingDeviceName);
        return Outcome::RequestStarted;
    }

    if (_requestRunning) {
        EmitOutcome(Outcome::RequestStarted, DeviceNameFor(_requestingDeviceId));
        return Outcome::RequestStarted;
    }

    if (!automatic && !_enabled) {
        EmitOutcome(Outcome::Disabled, {});
        return Outcome::Disabled;
    }

    if (_deviceId.isEmpty()) {
        EmitOutcome(Outcome::NoDevice, {});
        return Outcome::NoDevice;
    }

    _requestRunning = true;
    _automaticRequest = automatic;
    _manualRequestJoined = !automatic;
    _requestingDeviceId = _deviceId;
    _connectedDuringRequest = false;
    const uint64_t generation = ++_requestGeneration;

    _requestStop = std::stop_source{};

    QueueMutation([backend = _backend, link = _link, stopToken = _requestStop.get_token(),
                   requestedDeviceId = _deviceId, generation]() {
        const auto devices = backend->ListDevices(stopToken);
        const auto deviceIter =
            std::find_if(devices.begin(), devices.end(), [&](const Device &device) {
                return device.id == requestedDeviceId;
            });

        auto outcome = Outcome::DeviceUnavailable;
        QString deviceName;
        if (deviceIter != devices.end()) {
            deviceName = deviceIter->name;
            if (deviceIter->connected) {
                outcome = Outcome::AlreadyConnected;
            }
            else {
                outcome = backend->RequestReconnect(requestedDeviceId, stopToken)
                              ? Outcome::RequestStarted
                              : Outcome::Failed;
            }
        }

        if (stopToken.stop_requested()) {
            return;
        }

        PostToController(
            link, [requestedDeviceId, deviceName = std::move(deviceName), outcome,
                   generation](Controller &self) mutable {
                if (generation != self._requestGeneration) {
                    return;
                }
                self._requestRunning = false;
                self._requestingDeviceId.clear();

                if (outcome == Outcome::RequestStarted) {
                    if (self._connectedDuringRequest) {
                        self._connectedDuringRequest = false;
                        self._backend->StopObserving();
                        self.EmitOutcome(Outcome::Connected, deviceName);
                        return;
                    }

                    self._pending = true;
                    self._pendingDeviceId = requestedDeviceId;
                    self._pendingDeviceName = deviceName;
                    self._resolveTimer.start();
                }

                self._connectedDuringRequest = false;
                self.EmitOutcome(outcome, deviceName);
            });
    });

    return Outcome::RequestStarted;
}

Outcome Controller::OnEndpointStateChanged(const QString &id, bool connected)
{
    const bool selected = !_deviceId.isEmpty() && NormalizeId(id) == NormalizeId(_deviceId);
    if (selected && !_linkRunning && !_linkDisconnectRequested &&
        _lastLinkConnected != std::optional<bool>{connected})
    {
        _lastLinkConnected = connected;
        emit LinkConnectionChanged();
    }
    if (_linkDisconnectRequested) {
        return Outcome::RequestStarted;
    }
    if (_requestRunning && connected && NormalizeId(id) == NormalizeId(_requestingDeviceId)) {
        _connectedDuringRequest = true;
        return Outcome::RequestStarted;
    }

    if (!_pending) {
        return connected ? Outcome::Connected : Outcome::RequestStarted;
    }

    if (NormalizeId(id) != NormalizeId(_pendingDeviceId) || !connected) {
        return Outcome::RequestStarted;
    }

    return Complete(Outcome::Connected);
}

void Controller::OnDeviceConnectionChanged(bool connected)
{
    if (!_linkRunning && !_linkDisconnectRequested &&
        _lastLinkConnected != std::optional<bool>{connected})
    {
        _lastLinkConnected = connected;
        emit LinkConnectionChanged();
    }
}

Outcome Controller::ResolveTimedOut()
{
    if (!_pending) {
        return Outcome::TimedOut;
    }

    return Complete(Outcome::TimedOut);
}

Outcome Controller::Complete(Outcome outcome)
{
    _resolveTimer.stop();
    _pending = false;

    // Nothing is waiting on endpoint notifications any more; let the backend disarm whatever it
    // armed, so it stops resolving unrelated device changes for the rest of the process lifetime.
    _backend->StopObserving();

    const auto deviceName = _pendingDeviceName;
    _pendingDeviceId.clear();
    _pendingDeviceName.clear();
    EmitOutcome(outcome, deviceName);

    return outcome;
}

void Controller::EmitOutcome(Outcome outcome, const QString &deviceName)
{
    // An automatic check of an already connected device must not open the battery window.
    const bool silent =
        _automaticRequest && !_manualRequestJoined && outcome == Outcome::AlreadyConnected;
    if (_automaticRequest && (outcome == Outcome::Failed || outcome == Outcome::TimedOut ||
                              outcome == Outcome::DeviceUnavailable))
    {
        _nextAutoRetry = std::chrono::steady_clock::now() + _autoRetryCooldown;
    }
    if (outcome != Outcome::RequestStarted) {
        _automaticRequest = false;
    }
    if (silent) {
        return;
    }
    emit OutcomeChanged(outcome, deviceName);
}

//////////////////////////////////////////////////
// Explicit link control (blueprint 2.8)
//

Core::BluetoothLink::Outcome Controller::RequestLinkConnect()
{
    return RequestLink(true);
}

Core::BluetoothLink::Outcome Controller::RequestLinkDisconnect()
{
    return RequestLink(false);
}

Core::BluetoothLink::Outcome Controller::RequestLink(bool connect)
{
    // Repeated clicks in the same direction join the logical request. An
    // explicit opposite-direction request retires it by generation; the old
    // driver call may still be blocked, so it is reaped asynchronously.
    if (_linkRunning) {
        if (_linkDisconnectRequested == !connect) {
            EmitLinkOutcome(
                Core::BluetoothLink::Outcome::Started, Core::BluetoothLink::Failure::None);
            return Core::BluetoothLink::Outcome::Started;
        }
        ++_linkGeneration;
        _linkStop.request_stop();
        _linkRunning = false;
        _linkDisconnectRequested = false;
        _linkDeadlineTimer.stop();
        emit LinkBusyChanged(false);
    }

    if (_linkAddress == 0) {
        EmitLinkOutcome(Core::BluetoothLink::Outcome::NoDevice, Core::BluetoothLink::Failure::None);
        return Core::BluetoothLink::Outcome::NoDevice;
    }

    Core::BluetoothLink::Adapter *adapter = _backend->LinkAdapter();
    if (adapter == nullptr) {
        EmitLinkOutcome(
            Core::BluetoothLink::Outcome::AdapterUnavailable, Core::BluetoothLink::Failure::None);
        return Core::BluetoothLink::Outcome::AdapterUnavailable;
    }

    if (!connect) {
        CancelQuickConnectRequests();
    }

    ++_linkGeneration;
    const uint64_t generation = _linkGeneration;
    _linkRunning = true;
    _linkDisconnectRequested = !connect;
    _linkStop = std::stop_source{};

    // The queue serializes this mutation against every other explicit link or reconnect
    // operation process-wide. The job keeps shared backend/link state only; its generation
    // and stop token make completion after retirement a no-op.
    QueueMutation([backend = _backend, link = _link, adapter, stopToken = _linkStop.get_token(),
                   address = _linkAddress, connect, deadline = _linkDeadline, poll = _linkPoll,
                   generation]() {
        const auto result =
            Core::BluetoothLink::RunAttempt(*adapter, address, connect, stopToken, deadline, poll);
        if (stopToken.stop_requested()) {
            return;
        }
        PostToController(
            link, [generation, result](Controller &self) { self.FinishLink(generation, result); });
    });

    _linkDeadlineTimer.start();
    emit LinkBusyChanged(true);
    return Core::BluetoothLink::Outcome::Started;
}

void Controller::CancelQuickConnectRequests()
{
    ++_requestGeneration;
    _requestStop.request_stop();
    _resolveTimer.stop();
    _pending = false;
    _pendingDeviceId.clear();
    _pendingDeviceName.clear();
    _requestRunning = false;
    _requestingDeviceId.clear();
    _connectedDuringRequest = false;
    _automaticRequest = false;
    _manualRequestJoined = false;
    _backend->StopObserving();
}

void Controller::FinishLink(uint64_t generation, Core::BluetoothLink::AttemptResult result)
{
    if (generation != _linkGeneration) {
        // The deadline backstop already answered the user, or a newer request
        // owns the slot. Late events from a retired attempt never re-apply
        // stale state (same contract as the P2 connection generations).
        return;
    }

    _linkRunning = false;
    _linkDisconnectRequested = false;
    _linkDeadlineTimer.stop();

    if (const auto suppression = Core::BluetoothLink::SuppressionFor(result.outcome)) {
        _linkSuppressed = suppression.value();
    }
    switch (result.outcome) {
    case Core::BluetoothLink::Outcome::Connected:
    case Core::BluetoothLink::Outcome::AlreadyConnected:
        _lastLinkConnected = true;
        break;
    case Core::BluetoothLink::Outcome::Disconnected:
    case Core::BluetoothLink::Outcome::AlreadyDisconnected:
        _lastLinkConnected = false;
        break;
    case Core::BluetoothLink::Outcome::DeviceNotFound:
        _lastLinkConnected.reset();
        break;
    default:
        break;
    }

    EmitLinkOutcome(result.outcome, result.failure);
    emit LinkBusyChanged(false);
}

void Controller::ResolveLinkTimedOut()
{
    if (!_linkRunning) {
        return;
    }

    // The UI budget is five seconds even when the worker is stuck inside an
    // uninterruptible driver call. Retire the generation first so the worker
    // can post nothing for this attempt, then release its poll sleep.
    ++_linkGeneration;
    _linkRunning = false;
    _linkDisconnectRequested = false;
    _linkStop.request_stop();
    EmitLinkOutcome(Core::BluetoothLink::Outcome::TimedOut, Core::BluetoothLink::Failure::Timeout);
    emit LinkBusyChanged(false);
}

void Controller::EmitLinkOutcome(
    Core::BluetoothLink::Outcome outcome, Core::BluetoothLink::Failure failure)
{
    emit LinkOutcomeChanged(outcome, static_cast<int>(failure));
}

} // namespace Core::QuickConnect
