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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace Core::AAP {

//////////////////////////////////////////////////
// Command broker (blueprint 2.7)
//
// Settings writes used to be fire-and-forget: the UI flipped, the packet left, and
// nothing ever checked whether the earbuds applied it. This broker keeps the user's
// *desired* value, puts at most one command on the wire at a time, and only publishes
// a *confirmed* value when the firmware echoes or reads that value back. When the
// evidence does not arrive within the timeout the caller is told to roll the UI back
// to the value it actually knows the device holds.
//
// The class is deliberately payload-agnostic and Qt-free (it belongs to apd_domain):
// it only moves `kind` + `value` pairs and the protocol evidence vocabulary. Encoding
// `value` into AAP bytes and matching incoming records against it is the transport
// owner's job (Core::AAP::Manager, apd_core).
//

// One mutable device setting. The declaration order is also the deterministic order
// used when replaying the desired state after a successful reconnect.
enum class AAPCommandKind : uint8_t {
    SetNoiseControl = 0,
    SetConversationalAwareness,
    SetPersonalizedVolume,
    SetLoudSoundReduction,
    SetPpeCapLevel,
    SetAdaptiveNoiseLevel,
    SetAutomaticEarDetection,
    SetHeadTracking,
};

inline constexpr size_t kAAPCommandKindCount = 8;

// `value` is the protocol data byte for the command, so an outgoing command and an
// incoming piece of evidence share one representation:
//   SetNoiseControl (0x0D)              0x01 Off, 0x02 ANC, 0x03 Transparency, 0x04 Adaptive
//   SetConversationalAwareness (0x28)   0x01 on, 0x02 off
//   SetPersonalizedVolume (0x26)        0x01 on, 0x02 off
//   SetLoudSoundReduction (ATT 0x1B)    0x01 on, 0x00 off (ATT convention, not 0x02)
//   SetPpeCapLevel (0x38)               0-100
//   SetAdaptiveNoiseLevel (0x2E)        0-100
//   SetAutomaticEarDetection (0x0A)     0x01 on, 0x02 off
//   SetHeadTracking (0x17)              0x01 start, 0x00 stop
struct AAPCommand {
    AAPCommandKind kind{AAPCommandKind::SetNoiseControl};
    uint32_t value{0};
    // Unique for each new desired request and preserved when that request is replayed. A
    // duplicate queued, in-flight, or already-confirmed submission retains the original id.
    uint64_t requestId{0};

    bool operator==(const AAPCommand &rhs) const = default;
};

enum class AAPCommandFailure : uint8_t {
    Timeout,         // no matching evidence inside the echo window
    Rejected,        // the send seam refused or failed to write the record
    TransportClosed, // the session ended before the command settled
    Superseded,      // a newer value for the same kind replaced it
};

// What the protocol can prove about a command — the honest capability table from
// Docs/AAP-Protocol-Spec.md §7/§11/§12. A kind without evidence is never reported as
// confirmed, no matter how long the caller waits.
enum class AAPConfirmMode : uint8_t {
    // The device repeats the control identifier with the new value
    // (`04 00 04 00 09 00 <id> <value> …`): 0x0D listening mode, 0x28 conversation
    // detection, 0x26 adaptive volume, 0x0A ear-detection config, 0x2E adaptive filtering
    // strength (when firmware emits it), and 0x38 PPE cap. A missing 0x2E echo is a timeout,
    // never proof of success.
    Echo,
    // The value is only observable by asking again: the ATT write/read answer for
    // handle 0x1B (loud sound reduction), or the 0x0017 sensor stream that proves head
    // tracking actually started.
    Readback,
    // No reply is defined or parsed: stopping head tracking (silence is not evidence).
    // The command is applied and reported as such, but never confirmed and never timed out.
    Unverifiable,
};

struct AAPCommandResult {
    bool accepted{false};
    bool confirmed{false};
    std::optional<AAPCommandFailure> failure;
    uint64_t requestId{0};
};

enum class AAPCommandSettlement : uint8_t {
    Confirmed,          // matching echo/readback arrived
    AppliedUnconfirmed, // the protocol offers no evidence: settled once the write returned
    TimedOut,           // AAPCommandFailure::Timeout
    Rejected,           // AAPCommandFailure::Rejected
    TransportClosed,    // AAPCommandFailure::TransportClosed
    Superseded,         // AAPCommandFailure::Superseded
};

// Published for every command that leaves the in-flight slot.
struct AAPCommandOutcome {
    AAPCommand command;
    uint64_t generation{0};
    uint64_t requestId{0};
    AAPConfirmMode mode{AAPConfirmMode::Unverifiable};
    AAPCommandSettlement settlement{AAPCommandSettlement::Superseded};
    std::optional<AAPCommandFailure> failure;
    // Device state the caller should show instead of the desired value. Set only when the
    // request demonstrably did not take effect; nullopt means "no confirmed baseline exists
    // yet, refresh from the device".
    std::optional<uint32_t> rollbackValue;
    // True when the caller must revert what it displays. False for a superseded command
    // (a newer value owns the UI) and for a session loss (desired stays queued so the next
    // reconnect replays it, and a cancelled connect must not read as a user-visible failure).
    bool rollBackDesired{false};
};

// Serial desired-state tracker and command confirmer.
//
// Threading: Submit is safe from any thread, including the GUI thread, and never blocks on
// the device; one internal worker owns the wire, so commands are strictly serialized and a
// burst of slider moves collapses to the last value. OnEcho runs on the reader thread. The
// observer runs on the worker thread and must not touch widgets — cross-thread UI updates go
// through queued signals.
class AAPCommandBroker
{
public:
    static constexpr std::chrono::milliseconds kDefaultEchoTimeout{2000};

    using Sender =
        std::function<bool(const AAPCommand &command, uint64_t generation, std::stop_token stop)>;
    using Observer = std::function<void(const AAPCommandOutcome &outcome)>;

    struct Config {
        std::chrono::milliseconds echoTimeout{kDefaultEchoTimeout};
        // Replaces the built-in capability table for a kind, e.g. to degrade loud sound
        // reduction to Unverifiable when the ATT path probe answered nothing.
        std::array<std::optional<AAPConfirmMode>, kAAPCommandKindCount> overrides{};
        // Optional deterministic clock. Production leaves this empty and uses steady_clock.
        struct Clock {
            std::function<std::chrono::steady_clock::time_point()> now;
            std::function<void(std::chrono::milliseconds)> advance;
        };
        Clock clock{};
    };

    // Built-in protocol capability table, indexed by AAPCommandKind.
    static const std::array<AAPConfirmMode, kAAPCommandKindCount> &DefaultModes();
    // Kind default, ignoring the value.
    static AAPConfirmMode ModeFor(AAPCommandKind kind);
    // Protocol evidence actually available for one concrete command value.
    static AAPConfirmMode ModeFor(AAPCommandKind kind, uint32_t value);
    // True when a missing reply is a real failure worth rolling back.
    static bool RequiresEvidence(AAPConfirmMode mode);

    explicit AAPCommandBroker(Sender sender, Observer observer = {}, Config config = {});
    ~AAPCommandBroker();

    AAPCommandBroker(const AAPCommandBroker &) = delete;
    AAPCommandBroker &operator=(const AAPCommandBroker &) = delete;

    // Records `command` as desired and schedules the send. An unsent value for the same kind
    // is replaced in place (a fast slider drag only ever puts its last value on the wire) and
    // an in-flight command of the same kind settles as Superseded. Repeating the queued, in-flight,
    // or already-confirmed desired value is a no-op. Without an open session the desired value is
    // still recorded for the next replay and the result says TransportClosed.
    AAPCommandResult Submit(AAPCommand command, std::stop_token stop = {});

    // Records a disconnected caller's latest desire without queueing or sending it. Any older
    // queued value for the same kind is cancelled and eligible in-flight work is superseded, so
    // a reconnect replay cannot emit a stale value. The next ReplayDesiredState applies the new
    // desire. This keeps offline settings writes safe while a transport owner opens a replay.
    AAPCommandResult RecordDesired(AAPCommand command);

    // Evidence that the device holds `command`. Only a kind + value + generation match confirms
    // the in-flight command; a stale generation is dropped outright, and a same-generation
    // value mismatch only refreshes the known device state without confirming a newer command.
    void OnEcho(AAPCommand command, uint64_t generation);
    // ATT responses are correlated with the read owner. A response to the startup probe is
    // baseline evidence only and cannot accidentally confirm an overlapping command.
    void OnObserved(AAPCommand command, uint64_t generation);
    void OnEcho(AAPCommand command, uint64_t generation, uint64_t requestId);

    // Opens `generation` as the current session and re-pushes eligible desired values in
    // declaration order. The optional filter lets mode-gated kinds wait without losing desired
    // state. Idempotent and single-flight: repeated calls for the same generation never duplicate
    // sends; callers explicitly submit a deferred kind when it becomes eligible.
    void ReplayDesiredState(
        uint64_t generation, const std::function<bool(AAPCommandKind)> &includeKind = {});

    // Opens `generation` as the current session. Confirmed baselines belong to this session only;
    // evidence received before the first open is retained in one bounded slot per kind.
    void BeginSession(uint64_t generation);
    // Drops the in-flight and queued work as TransportClosed and clears session-scoped confirmed
    // baselines, but keeps every desired value for the next BeginSession + ReplayDesiredState.
    void EndSession();
    bool IsSessionOpen() const;
    uint64_t Generation() const;

    void SetConfirmMode(AAPCommandKind kind, AAPConfirmMode mode);
    // Effective mode for one concrete value: the override table, else the protocol default.
    AAPConfirmMode Mode(AAPCommandKind kind, uint32_t value) const;

    std::optional<uint32_t> Desired(AAPCommandKind kind) const;
    std::optional<uint32_t> Confirmed(AAPCommandKind kind) const;
    uint64_t LatestRequest(AAPCommandKind kind) const;
    bool IsCurrentRequest(AAPCommandKind kind, uint64_t requestId) const;
    bool HasPendingCommand(AAPCommandKind kind) const;
    // In-flight command, if any (diagnostics and tests).
    std::optional<AAPCommand> InFlight() const;
    // Values recorded but not yet on the wire.
    size_t QueuedCount() const;

    // Blocks until nothing is queued or in flight. Returns false on timeout/shutdown.
    bool WaitForIdle(std::chrono::milliseconds timeout);
    // Deterministic test seam: advances an injected clock and wakes the echo wait without sleeping.
    void AdvanceTimeForTesting(std::chrono::milliseconds delta);
    // Stops the worker and discards pending work without reporting it. The destructor calls
    // this; callers only need it to drain deterministically in a test.
    void Shutdown();

private:
    static constexpr size_t kKindIndex(AAPCommandKind kind)
    {
        return static_cast<size_t>(kind);
    }

    // A failed atomic head-tracking write is retried a small, bounded number of times in this
    // same serialized slot. Other C7 command kinds retain their one-attempt behavior.
    static constexpr unsigned kHeadTrackingSendAttempts = 3;

    struct KindState {
        std::optional<uint32_t> desired;
        uint64_t desiredRequestId{0};
        std::optional<uint32_t> confirmed;
        // Latest value waiting for the wire; collapses every intermediate slider step.
        std::optional<uint32_t> queued;
        uint64_t queuedRequestId{0};
        std::stop_token queuedStop;
    };

    struct FlightControl {
        std::atomic<bool> cancelled{false};
        std::shared_ptr<std::stop_source> stopSource{std::make_shared<std::stop_source>()};
        std::mutex callbacksMutex;
        std::vector<std::unique_ptr<std::stop_callback<std::function<void()>>>> callbacks;
    };

    struct Flight {
        AAPCommand command;
        uint64_t generation{0};
        AAPConfirmMode mode{AAPConfirmMode::Unverifiable};
        bool superseded{false};
        bool sessionLost{false};
        bool evidence{false};
        bool capabilityWithdrawn{false};
        bool cancelled{false};
        std::shared_ptr<FlightControl> control;
        // Supersession wakes a sender already inside a stop-aware transport write.
        std::shared_ptr<std::stop_source> stopSource;
    };

    void WorkerLoop(std::stop_token stopToken);
    // Caller must hold _mutex. Atomically replaces same-kind desired/pending work, optionally
    // queueing the new value for an open session. Disconnected callers never queue wire work.
    uint64_t RecordDesiredLocked(
        AAPCommand command, std::stop_token stop, bool queueWhenOpen,
        std::shared_ptr<FlightControl> *cancellationControl = nullptr);
    // Registers outside _mutex so an already-racing stop cannot invoke its callback reentrantly.
    void RegisterCancellation(std::shared_ptr<FlightControl> control, std::stop_token stop);
    // Caller must hold _mutex. Retires queued/in-flight work but preserves desired values.
    void CloseSessionLocked(std::vector<AAPCommandOutcome> &outcomes);
    // Caller holds _mutex. Moves cancellation callbacks out so they are destroyed after unlock.
    std::shared_ptr<FlightControl> RetireFlightLocked();
    void SettleLocked(Flight &flight, bool sent, std::vector<AAPCommandOutcome> &outcomes);
    void Publish(const std::vector<AAPCommandOutcome> &outcomes);
    std::chrono::steady_clock::time_point Now() const;
    AAPConfirmMode ModeLocked(AAPCommandKind kind, uint32_t value) const;

    Sender _sender;
    Observer _observer;
    std::chrono::milliseconds _echoTimeout;
    Config::Clock _clock;
    mutable std::mutex _mutex;
    std::condition_variable_any _workCondition;
    std::condition_variable_any _settleCondition;
    std::array<KindState, kAAPCommandKindCount> _kinds;
    std::array<std::optional<std::pair<AAPCommand, uint64_t>>, kAAPCommandKindCount> _earlyEvidence;
    std::array<std::optional<AAPConfirmMode>, kAAPCommandKindCount> _overrides;
    std::deque<AAPCommandKind> _queue;
    std::optional<Flight> _flight;
    uint64_t _generation{0};
    uint64_t _nextRequestId{1};
    bool _sessionOpen{false};
    bool _hasOpenedSession{false};
    // Generation whose replay is queued or done; guards single-flight replay.
    uint64_t _replayedGeneration{0};
    bool _stopping{false};

    std::jthread _worker;
};

} // namespace Core::AAP
