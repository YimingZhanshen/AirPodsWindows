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

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <QObject>
#include <QMetaType>
#include <QString>
#include <QTimer>

#include "BluetoothLinkControl.h"

namespace Core::QuickConnect {

class Controller;

enum class Outcome : uint32_t {
    Disabled,
    NoDevice,          // nothing is configured yet
    DeviceUnavailable, // configured, but it is not reporting an audio endpoint right now
    AlreadyConnected,
    RequestStarted,
    Failed,
    Connected,
    TimedOut
};

struct Device {
    QString id;
    QString name;
    bool connected{};
};

class Backend
{
public:
    virtual ~Backend() = default;

    virtual void SetController(Controller *controller);
    virtual std::vector<Device> ListDevices(std::stop_token stopToken) = 0;
    virtual bool RequestReconnect(const QString &id, std::stop_token stopToken) = 0;

    // Explicit link control for the tray connect/disconnect entry (blueprint
    // 2.8). The backend owns the adapter's lifetime; returning nullptr means
    // "this platform/backend cannot toggle the link" and produces the
    // AdapterUnavailable outcome instead of a guess.
    virtual Core::BluetoothLink::Adapter *LinkAdapter()
    {
        return nullptr;
    }

    // Called once the Controller stops caring about endpoint-state changes for the device it last
    // asked to reconnect. A backend that arms system notifications must disarm them here, otherwise
    // it keeps doing that work for every device on the machine until the process exits.
    virtual void StopObserving() {}
};

class NullBackend final : public Backend
{
public:
    std::vector<Device> ListDevices(std::stop_token) override
    {
        return {};
    }
    bool RequestReconnect(const QString &, std::stop_token) override
    {
        return false;
    }
};

class Controller final : public QObject
{
    Q_OBJECT

public:
    // How long to wait for the endpoint to come up after Windows accepted the reconnect request.
    static constexpr std::chrono::milliseconds kDefaultResolveTimeout{10'000};
    static constexpr std::chrono::milliseconds kAutoRetryCooldown{30'000};

    // The backend is shared rather than borrowed because a reconnect request can still be inside a
    // blocking driver call when the Controller goes away; that worker is detached and keeps the
    // backend alive for as long as it needs it.
    //
    // `resolveTimeout` is a parameter so tests can drive the real timer instead of calling the
    // timeout slot by hand. `linkDeadline` and `linkPoll` do the same for the explicit link
    // requests; production keeps the five-second blueprint budget.
    explicit Controller(
        std::shared_ptr<Backend> backend,
        std::chrono::milliseconds resolveTimeout = kDefaultResolveTimeout,
        std::chrono::milliseconds autoRetryCooldown = kAutoRetryCooldown,
        std::chrono::milliseconds linkDeadline = Core::BluetoothLink::kLinkDeadline,
        std::chrono::milliseconds linkPoll = Core::BluetoothLink::kLinkPollInterval);
    ~Controller() override;

    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    bool IsBusy() const;
    void SetAudioAutoConnectEnabled(bool enabled);
    void OnPlaybackStateChanged(bool playing);
    void SetDeviceId(QString id);
    std::vector<Device> Devices();
    void RefreshDevices();
    Outcome Request();
    Outcome OnEndpointStateChanged(const QString &id, bool connected);
    Outcome ResolveTimedOut();

    // Explicit link control (blueprint 2.8, tray "connect / disconnect").
    //
    // `SetLinkAddress` carries the bound classic-Bluetooth address; without it
    // the toggle reports NoDevice instead of guessing which paired device the
    // user meant. Requests never block the GUI thread: the whole query ->
    // submit -> read-back round trip runs on a cancellable worker under the
    // same deadline the OS budget uses, and a repeated click joins the running
    // request rather than starting a second one. A successful manual
    // disconnect suppresses automatic reconnect (playback-triggered quick
    // connect and the AAP session in `Core::AirPods::Manager`) until an
    // explicit connect lifts it; a device coming back on its own never does.
    void SetLinkAddress(uint64_t address);
    uint64_t LinkAddress() const
    {
        return _linkAddress;
    }
    bool IsLinkBusy() const
    {
        return _linkRunning;
    }
    bool IsLinkSuppressed() const
    {
        return _linkSuppressed;
    }
    std::optional<bool> IsLinkConnected() const
    {
        return _lastLinkConnected;
    }
    Core::BluetoothLink::Outcome RequestLinkConnect();
    Core::BluetoothLink::Outcome RequestLinkDisconnect();

signals:
    void DevicesChanged();
    void OutcomeChanged(Core::QuickConnect::Outcome outcome, QString deviceName);
    void LinkOutcomeChanged(Core::BluetoothLink::Outcome outcome, int failure);

private:
    // Lets a worker post back without ever dereferencing a Controller that has started tearing
    // down. `~Controller` clears the pointer under the mutex before anything else happens, so a
    // worker either posts to a live Controller or finds nothing and gives up.
    struct Link {
        std::mutex mutex;
        Controller *controller{};
    };

    std::shared_ptr<Backend> _backend;
    std::shared_ptr<Link> _link{std::make_shared<Link>()};
    bool _enabled{};
    bool _audioAutoConnectEnabled{};
    bool _playbackActive{};
    bool _automaticRequest{};
    bool _manualRequestJoined{};
    std::chrono::milliseconds _autoRetryCooldown;
    std::chrono::steady_clock::time_point _nextAutoRetry{};
    bool _pending{};
    QString _deviceId;
    QString _pendingDeviceId;
    QString _pendingDeviceName;
    QString _requestingDeviceId;
    bool _connectedDuringRequest{};
    std::vector<Device> _devices;
    std::atomic<bool> _deviceRefreshRunning{};
    std::atomic<bool> _requestRunning{};
    std::jthread _deviceRefreshThread;
    std::stop_source _requestStop;
    std::thread _requestThread;
    QTimer _resolveTimer;

    // Link-request state is only touched on the Controller's own thread; the
    // worker talks to it exclusively through `PostToController`.
    std::chrono::milliseconds _linkDeadline;
    std::chrono::milliseconds _linkPoll;
    uint64_t _linkAddress{};
    uint64_t _linkGeneration{};
    bool _linkRunning{};
    bool _linkSuppressed{};
    std::optional<bool> _lastLinkConnected;
    std::thread _linkThread;
    std::stop_source _linkStop;
    QTimer _linkDeadlineTimer;

    static void
    PostToController(const std::shared_ptr<Link> &link, std::function<void(Controller &)> work);

    QString DeviceNameFor(const QString &id) const;
    Outcome Request(bool automatic);
    Outcome Complete(Outcome outcome);
    void EmitOutcome(Outcome outcome, const QString &deviceName);
    Core::BluetoothLink::Outcome RequestLink(bool connect);
    void FinishLink(uint64_t generation, Core::BluetoothLink::AttemptResult result);
    void ResolveLinkTimedOut();
    void
    EmitLinkOutcome(Core::BluetoothLink::Outcome outcome, Core::BluetoothLink::Failure failure);
};

} // namespace Core::QuickConnect

Q_DECLARE_METATYPE(Core::QuickConnect::Outcome)
Q_DECLARE_METATYPE(Core::BluetoothLink::Outcome)
