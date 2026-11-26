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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Gui {

// Blueprint 2.5 — the head-tracking presentation policy.
//
// Everything in this header is free of Qt, of widgets and of any idea about threads, because the
// data has to cross a thread boundary to get here: `Core::AirPods::Manager` forwards every
// accepted 0x17 record from the AAP reader thread through a queued `*Safely` signal and the card
// feeds it to `HeadTrackingViewModel` on the GUI thread. Keeping the policy in a plain
// value-semantic class means a worker can never touch a control, and the whole rate limit,
// normalization and staleness behaviour is checkable in a console harness (no Qt needed).
//
// The caller owns serialization: one instance is used from a single thread (the GUI thread), the
// same contract as `Core::AAP::AAPConnectionStateMachine`.

// The five sensor counts exactly as the device sends them.
struct HeadTrackingCounts {
    // "orientation 1/2/3" — the three attitude channels of the record.
    std::array<std::int16_t, 3> orientation{{0, 0, 0}};
    // "Horizontal Acceleration" and "Vertical Acceleration".
    std::int16_t horizontalAcceleration{0};
    std::int16_t verticalAcceleration{0};

    bool operator==(const HeadTrackingCounts &) const = default;
};

// What one raw AAP message is, as far as the head-tracking view is concerned.
//
// The transport gate in `Core::AAP::Manager` checks the opcode pair 0x17 0x00. This pure reader
// additionally verifies the documented ten-byte sensor prefix before exposing offsets to a view.
enum class HeadTrackingRecord : std::uint8_t {
    NotHeadTracking,
    Stream,
};

// Shortest length whose fields the parser can read: 16 bits at offset 53 ends at 55.
inline constexpr std::size_t kHeadTrackingRecordSize = 56;
// Field offsets from "AAP Definitions.md" §Received Head Tracking Sensor Data. A domain test
// cross-checks them against `Core::AAP::ParseHeadTrackingData`
// (`HeadTrackingPresentationAgreesWithCoreParser`) so the two readers cannot drift apart.
inline constexpr std::size_t kHeadTrackingOrientationOffset = 43;
inline constexpr std::size_t kHeadTrackingHorizontalAccelerationOffset = 51;
inline constexpr std::size_t kHeadTrackingVerticalAccelerationOffset = 53;

HeadTrackingRecord ClassifyHeadTrackingRecord(std::span<const std::uint8_t> packet) noexcept;

// Reads the sensor fields of a record that `ClassifyHeadTrackingRecord` accepted; nullopt for
// anything shorter than `kHeadTrackingRecordSize`.
std::optional<HeadTrackingCounts>
ReadHeadTrackingCounts(std::span<const std::uint8_t> packet) noexcept;

// Signed 16-bit sensor counts, normalized.
//
// No published physical scale exists: the protocol notes list field offsets only, and LibrePods
// plots the raw counts against an axis it auto-scales itself (`Plot`, maxAbs = max(peak*1.2,
// 1000)). Inventing degrees or g would be a guess dressed up as a measurement, so the view model
// divides by 32768 — exact signed full scale: INT16_MIN maps to -1.0 and the result can never
// leave [-1, 1] — and the card labels each value by its protocol name.
constexpr double NormalizeSensorCount(std::int16_t raw) noexcept
{
    return static_cast<double>(raw) / 32768.0;
}

// Normalized mirror of `HeadTrackingCounts`; the raw counts are kept so the card can show both.
struct HeadTrackingSample {
    std::array<double, 3> orientation{{0.0, 0.0, 0.0}};
    double horizontalAcceleration{0.0};
    double verticalAcceleration{0.0};
    HeadTrackingCounts raw;

    bool operator==(const HeadTrackingSample &) const = default;
};

HeadTrackingSample NormalizeHeadTrackingSample(const HeadTrackingCounts &counts) noexcept;

enum class HeadTrackingPhase : std::uint8_t {
    Off,       // the user switched head tracking off
    Armed,     // on and the transport is up, no record of this generation yet
    Streaming, // at least one record accepted in the current generation
    Stale,     // streamed before, but the records stopped
    LinkDown,  // the AAP transport dropped while head tracking was on
};

// One drawable state. The card renders this and adds no policy of its own.
struct HeadTrackingView {
    HeadTrackingPhase phase{HeadTrackingPhase::Off};
    // The newest accepted record; only meaningful once `hasSample` is set.
    HeadTrackingSample sample;
    bool hasSample{false};
    // Transport generation the readout belongs to (blueprint 0.4 generations from P2).
    std::uint64_t generation{0};
    // Diagnostics, both reset with the readout: accepted records versus published updates.
    std::uint32_t acceptedRecords{0};
    std::uint32_t publishedUpdates{0};
    // Whole-second resolution so an idle card can compare views for equality and skip repaints.
    // -1 means nothing has been accepted in this generation yet.
    std::chrono::milliseconds sinceLastRecord{-1};

    bool operator==(const HeadTrackingView &) const = default;

    // A live pulse is only honest while fresh records are arriving.
    bool ShouldPulse() const
    {
        return phase == HeadTrackingPhase::Streaming;
    }
};

// The head-tracking presentation state machine. Time is a parameter of every entry point, which
// is what makes the 10 Hz gate and the stale transition reproducible without a real timer.
class HeadTrackingViewModel
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    // 10 Hz. The device streams roughly 20 records per second, faster than a numeric readout can
    // be read, and every accepted record would otherwise queue another repaint on the GUI thread.
    static constexpr std::chrono::milliseconds kPublishInterval{100};
    // A healthy stream is ~20 Hz. Nothing for this long means the buds paused or the link went
    // quiet, so the card stops presenting the numbers as live.
    static constexpr std::chrono::milliseconds kStaleAfter{750};

    // Mirrors the `head_tracking` setting. Turning it off clears the readout, so the next
    // time it is on the card cannot show the previous session's pose as if it were current.
    void SetEnabled(bool enabled, TimePoint now);
    // A fresh transport generation forgets the device: its stream restarts with each session, so
    // the first record after it is published immediately.
    void OnTransportUp(std::uint64_t generation, TimePoint now);
    // `generation` picks the session that ends; a late loss report cannot freeze a new readout.
    void OnTransportDown(std::uint64_t generation, TimePoint now);

    // Returns the view to publish, or nullopt when the record was ignored (wrong generation, off,
    // dead link, not a head-tracking record) or the 10 Hz gate is still closed. The newest record
    // always wins: throttled records are kept and published when the gate opens, so the readout
    // lags by at most one interval instead of losing the pose.
    std::optional<HeadTrackingView> OnCounts(
        const HeadTrackingCounts &counts, std::uint64_t generation, HeadTrackingRecord record,
        TimePoint now);
    // End-to-end entry point for raw bytes: classify, read, then `OnCounts`.
    std::optional<HeadTrackingView>
    OnPacket(std::span<const std::uint8_t> packet, std::uint64_t generation, TimePoint now);

    // Re-evaluates time-driven state (Streaming -> Stale). Returns a view only when the phase
    // changed, so a card can call it on a one-second timer without repainting.
    std::optional<HeadTrackingView> Tick(TimePoint now);

    HeadTrackingView Present(TimePoint now) const;
    HeadTrackingPhase Phase(TimePoint now) const;
    std::uint64_t Generation() const
    {
        return _generation;
    }
    bool Enabled() const
    {
        return _enabled;
    }

private:
    void ResetReadout();

    bool _enabled{false};
    bool _linkUp{false};
    std::uint64_t _generation{0};
    HeadTrackingSample _sample;
    bool _hasSample{false};
    std::uint32_t _acceptedRecords{0};
    std::uint32_t _publishedUpdates{0};
    std::optional<TimePoint> _lastRecord;
    std::optional<TimePoint> _lastPublish;
    HeadTrackingPhase _lastPhase{HeadTrackingPhase::Off};
};

} // namespace Gui
