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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>

namespace Core::BluetoothLink {

// Hard cap for one explicit connect/disconnect round trip. The tray reports
// TimedOut past it; the worker thread observes the same budget.
inline constexpr std::chrono::milliseconds kLinkDeadline{5'000};
inline constexpr std::chrono::milliseconds kLinkPollInterval{200};

enum class Service : uint8_t { A2DP, HFP };

// Availability and link state of one audio profile on the target device.
// Absent is a readable zero-row result; Unreadable and Uncontrollable are never
// accepted as proof that a link is down.
enum class ServiceStatus : uint8_t {
    Absent,
    Unreadable,
    Uncontrollable,
    Unavailable,  // present but user-disabled / inactive - never toggled
    Unknown,      // controllable, but the driver did not provide profile connection evidence
    Disconnected, // driver evidence proves the profile link is down
    Connected,    // validated driver evidence proves the profile link is up
};

// One endpoint's contribution to profile state. MMDevice ACTIVE alone is not
// connection proof: `connected` comes from jack/pin driver evidence, or from an
// explicitly unplugged/absent endpoint. `controllable` means the filter exposed
// at least one BtAudio one-shot operation.
struct EndpointEvidence {
    bool present{};
    bool controllable{};
    std::optional<bool> connected;
    bool available{true}; // disabled/unknown MMDevice states are never reconnect targets
    bool readable{true};  // false when filter/state/one-shot support evidence could not be read
};

inline ServiceStatus AggregateEndpointEvidence(std::span<const EndpointEvidence> evidence)
{
    if (evidence.empty()) {
        return ServiceStatus::Absent;
    }

    bool anyPresent{};
    bool anyControllable{};
    bool anyConnected{};
    bool anyDisconnected{};
    bool anyUnknown{};
    for (const auto &item : evidence) {
        if (item.present && !item.readable) {
            return ServiceStatus::Unreadable;
        }
        anyPresent = anyPresent || item.present;
        anyControllable = anyControllable || item.controllable;
        anyConnected =
            anyConnected || (item.available && item.connected == std::optional<bool>{true});
        anyDisconnected =
            anyDisconnected || (item.available && item.connected == std::optional<bool>{false});
        anyUnknown = anyUnknown || (item.available && !item.connected.has_value());
    }

    if (!anyPresent) {
        return ServiceStatus::Absent;
    }
    if (!anyControllable) {
        return ServiceStatus::Uncontrollable;
    }
    if (anyConnected) {
        return ServiceStatus::Connected;
    }
    if (anyUnknown) {
        return ServiceStatus::Unknown;
    }
    if (anyDisconnected) {
        return ServiceStatus::Disconnected;
    }
    return ServiceStatus::Unavailable;
}

struct DeviceLinkState {
    bool deviceKnown{false};   // paired/remembered on a local radio (bthprops)
    bool linkReadable{false};  // radio ACL read-back available
    bool linkConnected{false}; // radio-level ACL state, only meaningful when readable
    ServiceStatus a2dp{ServiceStatus::Absent};
    ServiceStatus hfp{ServiceStatus::Absent};
};

struct RadioEnumerationResult {
    bool started{};
    bool completed{};
    std::size_t count{};
    uint32_t lastError{};
};

namespace Details {

// Generic seam for the Win32 radio walk. Keeping ownership here makes the
// cleanup rules testable with fake handles: every opened radio is closed, and
// the search handle is closed on normal completion, errors, and early stop.
template <
    typename SearchHandle, typename RadioHandle, typename FindFirst, typename FindNext,
    typename CloseSearch, typename CloseRadio, typename GetLastError, typename Visit>
RadioEnumerationResult EnumerateRadioHandles(
    FindFirst findFirst, FindNext findNext, CloseSearch closeSearch, CloseRadio closeRadio,
    GetLastError getLastError, Visit visit)
{
    struct SearchGuard {
        SearchHandle handle;
        CloseSearch close;
        ~SearchGuard()
        {
            if (handle) {
                close(handle);
            }
        }
    };
    struct RadioGuard {
        RadioHandle handle;
        CloseRadio close;
        ~RadioGuard()
        {
            if (handle) {
                close(handle);
            }
        }
    };

    RadioHandle radio{};
    SearchHandle search = findFirst(&radio);
    if (!search) {
        const auto error = getLastError();
        if (radio) {
            closeRadio(radio);
        }
        return {false, false, 0, error};
    }
    SearchGuard searchGuard{search, closeSearch};

    std::size_t count{};
    for (;;) {
        RadioGuard radioGuard{radio, closeRadio};
        ++count;
        if (!visit(radio)) {
            return {true, true, count, 0};
        }
        RadioHandle next{};
        if (!findNext(search, &next)) {
            const auto error = getLastError();
            if (next) {
                closeRadio(next);
            }
            return {true, error == 259 /* ERROR_NO_MORE_ITEMS */, count, error};
        }
        radio = next;
    }
}

} // namespace Details

enum class Decision : uint8_t {
    DeviceNotFound,     // no such paired device - refuse
    Unreadable,         // evidence cannot safely classify the requested state
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
    Busy,               // the bounded worker queue could not accept another request
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
    case Failure::Busy:
        return "busy";
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
    if (!state.linkReadable || state.a2dp == ServiceStatus::Unreadable ||
        state.hfp == ServiceStatus::Unreadable)
    {
        plan.decision = Decision::Unreadable;
        return plan;
    }

    const auto usable = [](ServiceStatus status) {
        return status == ServiceStatus::Unknown || status == ServiceStatus::Disconnected ||
               status == ServiceStatus::Connected;
    };
    const auto provenConnected = [&](ServiceStatus status) {
        return status == ServiceStatus::Connected && state.linkReadable && state.linkConnected;
    };

    if (connect) {
        if (provenConnected(state.a2dp)) {
            plan.decision = Decision::AlreadySatisfied;
            return plan;
        }
        if (usable(state.a2dp)) {
            plan.decision = Decision::Proceed;
            plan.connectA2DP = true;
            return plan;
        }
        if (provenConnected(state.hfp)) {
            plan.decision = Decision::AlreadySatisfied;
            return plan;
        }
        if (usable(state.hfp)) {
            plan.decision = Decision::Proceed;
            plan.connectHFP = true;
            return plan;
        }
        plan.decision = Decision::NoSupportedService;
        return plan;
    }

    if (state.a2dp == ServiceStatus::Connected || state.a2dp == ServiceStatus::Unknown) {
        plan.disconnectA2DP = true;
    }
    if (state.hfp == ServiceStatus::Connected || state.hfp == ServiceStatus::Unknown) {
        plan.disconnectHFP = true;
    }
    if (!plan.TouchesAny()) {
        // Absent is a readable zero-row result and Disconnected is explicit
        // driver proof. Uncontrollable, unavailable, and unknown are not proof.
        const auto provenDown = [](ServiceStatus status) {
            return status == ServiceStatus::Absent || status == ServiceStatus::Disconnected;
        };
        const bool positiveProof = provenDown(state.a2dp) && provenDown(state.hfp);
        plan.decision = positiveProof ? Decision::AlreadySatisfied : Decision::NoSupportedService;
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
// endpoints around), so a connect is satisfied only with both validated jack
// evidence and the ACL link up. A disconnect is satisfied only when every
// touched profile reports explicit driver evidence that its link is down.
inline AttemptProgress Evaluate(const ActionPlan &plan, const DeviceLinkState &latest, bool connect)
{
    AttemptProgress progress;
    if (!latest.deviceKnown) {
        progress.evaluation = Evaluation::Failed;
        progress.failure = Failure::DeviceNotFound;
        return progress;
    }
    if (!latest.linkReadable || latest.a2dp == ServiceStatus::Unreadable ||
        latest.hfp == ServiceStatus::Unreadable)
    {
        progress.evaluation = Evaluation::Failed;
        progress.failure = Failure::Unreadable;
        return progress;
    }

    if (connect) {
        const bool touchedUp = (plan.connectA2DP && latest.a2dp == ServiceStatus::Connected) ||
                               (plan.connectHFP && latest.hfp == ServiceStatus::Connected);
        if (touchedUp && latest.linkReadable && latest.linkConnected) {
            progress.evaluation = Evaluation::Satisfied;
        }
        return progress;
    }

    // A disappeared profile is not proof that a disconnect request worked.
    // Every touched profile must report explicit driver evidence that its link
    // is down; untouched profiles must at least be absent or positively down so
    // an uncontrollable endpoint cannot be hidden by a successful sibling.
    const auto provenDown = [](ServiceStatus status) {
        return status == ServiceStatus::Absent || status == ServiceStatus::Disconnected;
    };
    const bool touchedDown = (!plan.disconnectA2DP || latest.a2dp == ServiceStatus::Disconnected) &&
                             (!plan.disconnectHFP || latest.hfp == ServiceStatus::Disconnected);
    const bool untouchedDown = (plan.disconnectA2DP || provenDown(latest.a2dp)) &&
                               (plan.disconnectHFP || provenDown(latest.hfp));
    if (plan.TouchesAny() && touchedDown && untouchedDown) {
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
                                    std::lock_guard lock{mutex};
                                    cancelled = true;
                                    cv.notify_all();
                                }};
    std::unique_lock lock{mutex};
    if (cancelled) {
        return;
    }
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
    if (stopToken.stop_requested()) {
        return {Outcome::Cancelled, Failure::Cancelled};
    }
    auto state = adapter.Query(address, stopToken);
    if (stopToken.stop_requested()) {
        return {Outcome::Cancelled, Failure::Cancelled};
    }
    if (!state.has_value()) {
        return {Outcome::Failed, Failure::Unreadable};
    }

    const auto terminalFor = [&](const DeviceLinkState &value) -> std::optional<AttemptResult> {
        const auto candidate = BuildPlan(value, connect);
        switch (candidate.decision) {
        case Decision::DeviceNotFound:
            return AttemptResult{Outcome::DeviceNotFound, Failure::DeviceNotFound};
        case Decision::Unreadable:
            return AttemptResult{Outcome::Failed, Failure::Unreadable};
        case Decision::NoSupportedService:
            return AttemptResult{Outcome::NoSupportedService, Failure::NoSupportedService};
        case Decision::AlreadySatisfied:
            return AttemptResult{
                connect ? Outcome::AlreadyConnected : Outcome::AlreadyDisconnected, Failure::None};
        case Decision::Proceed:
            return std::nullopt;
        }
        return std::nullopt;
    };

    ActionPlan plan;
    for (int submission = 0;; ++submission) {
        plan = BuildPlan(*state, connect);
        if (auto terminal = terminalFor(*state)) {
            return *terminal;
        }

        const auto submitted = adapter.Submit(address, plan, stopToken);
        if (stopToken.stop_requested()) {
            return {Outcome::Cancelled, Failure::Cancelled};
        }
        if (submitted != Failure::NoSupportedService) {
            if (submitted != Failure::None) {
                return {Outcome::Failed, submitted};
            }
            break;
        }

        // Submit performs its own endpoint preflight. Another process may have
        // satisfied the request between our query and that preflight, so never
        // classify a nothing-to-send result until a fresh read-back agrees.
        state = adapter.Query(address, stopToken);
        if (stopToken.stop_requested()) {
            return {Outcome::Cancelled, Failure::Cancelled};
        }
        if (!state.has_value()) {
            return {Outcome::Failed, Failure::Unreadable};
        }
        if (auto terminal = terminalFor(*state)) {
            return *terminal;
        }
        if (submission >= 1) {
            return {Outcome::Failed, Failure::NoSupportedService};
        }
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
        if (!fresh.has_value()) {
            return {Outcome::Failed, Failure::Unreadable};
        }
        const auto progress = Evaluate(plan, *fresh, connect);
        if (progress.evaluation == Evaluation::Satisfied) {
            return {connect ? Outcome::Connected : Outcome::Disconnected, Failure::None};
        }
        if (progress.evaluation == Evaluation::Failed) {
            return {Outcome::Failed, progress.failure};
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
