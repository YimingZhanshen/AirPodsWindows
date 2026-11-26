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

#pragma once

// Explicit Bluetooth audio link control (blueprint 2.8, task D7).
//
// The tray "connect / disconnect" entry needs one narrow capability: toggle
// the A2DP/HFP audio services of the bound classic-Bluetooth device and prove
// the radio actually moved, within five seconds. This header owns the Qt-free,
// hardware-free policy around that operation:
//
//  * `BuildPlan` validates the service state before touching anything. A
//    profile the device does not expose, or that the user disabled, is never
//    guessed at - the plan either names supported services or refuses.
//  * `Evaluate` folds the freshest read-back into a Pending/Satisfied/Failed
//    decision, so "Windows accepted the request" is never reported as
//    "connected" (KS one-shot requests are submission-only).
//  * `RunAttempt` drives query -> submit -> poll with an injected clock and
//    wait function, which is what the standalone harness exercises; the Qt
//    layer only moves it onto a worker thread.
//  * `SuppressionFor` maps a terminal outcome to the manual-disconnect
//    suppression transition consumed by QuickConnect and `Core::AirPods::Manager`.
//
// The OS side (BluetoothLinkControl_win.cpp) implements `Adapter` with
// bthprops paired-device read-back plus KSPropertySETID_BtAudio one-shot
// requests against the device's audio endpoints; no driver install/removal
// (BluetoothSetServiceState) and no radio reset, matching the researched
// selection in the blueprint.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace Core::BluetoothLink {

// Hard cap for one explicit connect/disconnect round trip. The tray reports
// TimedOut past it; the worker thread observes the same budget.
inline constexpr std::chrono::milliseconds kLinkDeadline{5'000};
inline constexpr std::chrono::milliseconds kLinkPollInterval{200};

enum class Service : uint8_t { A2DP, HFP };

// Availability and link state of one audio profile on the target device.
enum class ServiceStatus : uint8_t {
    Unsupported,  // device does not expose it, or its driver failed the KS basicsupport probe
    Unavailable,  // present but user-disabled / inactive - never toggled
    Disconnected, // usable endpoint whose link is down
    Connected,    // endpoint active right now
};

struct DeviceLinkState {
    bool deviceKnown{false};   // paired/remembered on a local radio (bthprops)
    bool linkReadable{false};  // radio ACL read-back available
    bool linkConnected{false}; // radio-level ACL state, only meaningful when readable
    ServiceStatus a2dp{ServiceStatus::Unsupported};
    ServiceStatus hfp{ServiceStatus::Unsupported};
};

enum class Decision : uint8_t {
    DeviceNotFound,     // no such paired device - refuse
    NoSupportedService, // nothing controllable - refuse instead of guessing
    AlreadySatisfied,   // the requested state already holds, no OS call needed
    Proceed,
};

struct ActionPlan {
    Decision decision{Decision::Proceed};
    bool connectA2DP{false};
    bool connectHFP{false};
    bool disconnectA2DP{false};
    bool disconnectHFP{false};

    bool TouchesAny() const
    {
        return connectA2DP || connectHFP || disconnectA2DP || disconnectHFP;
    }
};

// Stable error taxonomy for tray messaging and the diagnostics export
// (same role blueprint 1.3 defined for AAP::ConnectFailure).
enum class Failure : uint8_t {
    None,
    DeviceNotFound,     // vanished from the paired list
    NoSupportedService, // nothing to operate on
    RequestRejected,    // the audio driver refused the one-shot request
    Unreadable,         // state could not be queried at all
    Timeout,            // read-back never reached the requested state
    Cancelled,          // stop was requested (shutdown / superseded)
};

inline const char *FailureName(Failure failure)
{
    switch (failure) {
    case Failure::None:
        return "none";
    case Failure::DeviceNotFound:
        return "device_not_found";
    case Failure::NoSupportedService:
        return "no_supported_service";
    case Failure::RequestRejected:
        return "request_rejected";
    case Failure::Unreadable:
        return "unreadable";
    case Failure::Timeout:
        return "timeout";
    case Failure::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

enum class Outcome : uint8_t {
    Started,            // request accepted (or joined); a terminal outcome follows
    NoDevice,           // no bound device address configured
    AdapterUnavailable, // platform/backend without link control
    DeviceNotFound,
    NoSupportedService,
    AlreadyConnected,
    AlreadyDisconnected,
    Connected,
    Disconnected,
    Failed, // classification travels in Failure
    TimedOut,
    Cancelled,
};

inline const char *OutcomeName(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Started:
        return "started";
    case Outcome::NoDevice:
        return "no_device";
    case Outcome::AdapterUnavailable:
        return "adapter_unavailable";
    case Outcome::DeviceNotFound:
        return "device_not_found";
    case Outcome::NoSupportedService:
        return "no_supported_service";
    case Outcome::AlreadyConnected:
        return "already_connected";
    case Outcome::AlreadyDisconnected:
        return "already_disconnected";
    case Outcome::Connected:
        return "connected";
    case Outcome::Disconnected:
        return "disconnected";
    case Outcome::Failed:
        return "failed";
    case Outcome::TimedOut:
        return "timed_out";
    case Outcome::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

// Whether an explicit link outcome suppresses automatic reconnects.
// nullopt = leave the current suppression untouched. A manual disconnect (or a
// disconnect request that found the device already down) suppresses; an
// explicit connect, including one that finds the device already up, lifts it.
// Failures keep the current policy: a request that did not move the link must
// not silently change what the user asked for.
inline std::optional<bool> SuppressionFor(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Disconnected:
    case Outcome::AlreadyDisconnected:
        return true;
    case Outcome::Connected:
    case Outcome::AlreadyConnected:
        return false;
    default:
        return std::nullopt;
    }
}

// Pure: validate the state and name the services the action may toggle.
inline ActionPlan BuildPlan(const DeviceLinkState &state, bool connect)
{
    ActionPlan plan;
    if (!state.deviceKnown) {
        plan.decision = Decision::DeviceNotFound;
        return plan;
    }

    const bool a2dpUsable =
        state.a2dp == ServiceStatus::Disconnected || state.a2dp == ServiceStatus::Connected;
    const bool hfpUsable =
        state.hfp == ServiceStatus::Disconnected || state.hfp == ServiceStatus::Connected;

    if (connect) {
        if (state.a2dp == ServiceStatus::Connected) {
            plan.decision = Decision::AlreadySatisfied;
            return plan;
        }
        if (a2dpUsable) {
            plan.decision = Decision::Proceed;
            plan.connectA2DP = true;
            return plan;
        }
        if (state.hfp == ServiceStatus::Connected) {
            plan.decision = Decision::AlreadySatisfied;
            return plan;
        }
        if (hfpUsable) {
            plan.decision = Decision::Proceed;
            plan.connectHFP = true;
            return plan;
        }
        plan.decision = Decision::NoSupportedService;
        return plan;
    }

    if (state.a2dp == ServiceStatus::Connected) {
        plan.disconnectA2DP = true;
    }
    if (state.hfp == ServiceStatus::Connected) {
        plan.disconnectHFP = true;
    }
    if (!plan.TouchesAny()) {
        plan.decision = Decision::AlreadySatisfied;
    }
    return plan;
}

enum class Evaluation : uint8_t { Pending, Satisfied, Failed };

struct AttemptProgress {
    Evaluation evaluation{Evaluation::Pending};
    Failure failure{Failure::None};
};

// Pure: fold the freshest read-back into progress against the plan. An ACTIVE
// endpoint alone is not evidence of a Bluetooth link (the driver keeps stale
// endpoints around), so when the radio read-back is available a connect is
// only satisfied with the ACL link up; a disconnect is satisfied when no
// touched profile still reports Connected, even if the device keeps the ACL
// alive for other profiles.
inline AttemptProgress Evaluate(const ActionPlan &plan, const DeviceLinkState &latest, bool connect)
{
    AttemptProgress progress;
    if (!latest.deviceKnown) {
        progress.evaluation = Evaluation::Failed;
        progress.failure = Failure::DeviceNotFound;
        return progress;
    }

    if (connect) {
        const bool touchedUp = (plan.connectA2DP && latest.a2dp == ServiceStatus::Connected) ||
                               (plan.connectHFP && latest.hfp == ServiceStatus::Connected);
        const bool linkProven = !latest.linkReadable || latest.linkConnected;
        if (touchedUp && linkProven) {
            progress.evaluation = Evaluation::Satisfied;
        }
        return progress;
    }

    const bool stillConnected = (plan.disconnectA2DP && latest.a2dp == ServiceStatus::Connected) ||
                                (plan.disconnectHFP && latest.hfp == ServiceStatus::Connected);
    if (!stillConnected) {
        progress.evaluation = Evaluation::Satisfied;
    }
    return progress;
}

// Narrow OS seam. Everything platform-specific hides behind these two calls;
// the hardware-free tests inject a fake instead. Windows keeps this narrow on
// purpose: Query is a read-only walk of the paired list and the device's
// audio endpoints, Submit only issues KS one-shot requests (never a driver
// install, never a radio reset), and both observe the caller's stop token.
class Adapter
{
public:
    virtual ~Adapter() = default;

    virtual std::optional<DeviceLinkState> Query(uint64_t address, std::stop_token stopToken) = 0;
    virtual Failure Submit(uint64_t address, const ActionPlan &plan, std::stop_token stopToken) = 0;
};

struct AttemptResult {
    Outcome outcome{Outcome::Failed};
    Failure failure{Failure::Unreadable};
};

inline void CancellableSleep(std::chrono::milliseconds interval, std::stop_token stopToken)
{
    std::mutex mutex;
    std::condition_variable_any cv;
    bool cancelled = false;
    std::stop_callback callback{stopToken, [&] {
                                    cancelled = true;
                                    cv.notify_all();
                                }};
    if (cancelled) {
        return;
    }
    std::unique_lock lock{mutex};
    cv.wait_for(lock, interval, [&] { return cancelled; });
}

// One explicit round trip: query, plan, submit, poll until satisfied or the
// deadline passes. Time and waiting are injected so the harness runs it
// without threads, Qt, or a radio.
inline AttemptResult RunAttempt(
    Adapter &adapter, uint64_t address, bool connect, std::stop_token stopToken,
    std::chrono::milliseconds deadline, std::chrono::milliseconds pollInterval,
    const std::function<std::chrono::steady_clock::time_point()> &now =
        [] { return std::chrono::steady_clock::now(); },
    const std::function<void(std::chrono::milliseconds, std::stop_token)> &wait = CancellableSleep)
{
    auto state = adapter.Query(address, stopToken);
    if (!state.has_value()) {
        return {Outcome::Failed, Failure::Unreadable};
    }

    const auto plan = BuildPlan(*state, connect);
    switch (plan.decision) {
    case Decision::DeviceNotFound:
        return {Outcome::DeviceNotFound, Failure::DeviceNotFound};
    case Decision::NoSupportedService:
        return {Outcome::NoSupportedService, Failure::NoSupportedService};
    case Decision::AlreadySatisfied:
        return {connect ? Outcome::AlreadyConnected : Outcome::AlreadyDisconnected, Failure::None};
    case Decision::Proceed:
        break;
    }

    const auto submitted = adapter.Submit(address, plan, stopToken);
    if (stopToken.stop_requested()) {
        return {Outcome::Cancelled, Failure::Cancelled};
    }
    if (submitted != Failure::None) {
        return {Outcome::Failed, submitted};
    }

    // Submission is not connection: keep reading the state back until it
    // proves out, the device vanishes, or the five-second budget is spent.
    const auto end = now() + deadline;
    for (;;) {
        wait(pollInterval, stopToken);
        if (stopToken.stop_requested()) {
            return {Outcome::Cancelled, Failure::Cancelled};
        }

        const auto fresh = adapter.Query(address, stopToken);
        if (stopToken.stop_requested()) {
            return {Outcome::Cancelled, Failure::Cancelled};
        }
        if (fresh.has_value()) {
            const auto progress = Evaluate(plan, *fresh, connect);
            if (progress.evaluation == Evaluation::Satisfied) {
                return {connect ? Outcome::Connected : Outcome::Disconnected, Failure::None};
            }
            if (progress.evaluation == Evaluation::Failed) {
                return {Outcome::Failed, progress.failure};
            }
        }

        if (now() >= end) {
            return {Outcome::TimedOut, Failure::Timeout};
        }
    }
}

#if defined(APD_OS_WIN)
// Implemented in BluetoothLinkControl_win.cpp.
std::unique_ptr<Adapter> CreateWindowsAdapter();
#endif

} // namespace Core::BluetoothLink
