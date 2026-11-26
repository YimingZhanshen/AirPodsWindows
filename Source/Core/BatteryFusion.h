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

#include <cstdint>
#include <optional>

#include "AAPBattery.h"

namespace Core::AirPods {

// Which feed produced the battery value the UI currently shows for a component.
// Everything defaults to BLE so behaviour stays identical while no AAP session feeds
// battery data (the driver-less path never leaves BLE).
enum class BatterySource : uint8_t {
    BLE,
    AAP,
};

namespace Details {

// Fuses the 1%-precise AAP battery stream (opcode 0x0004) over the quantized BLE
// advertisement batteries. Pure: no clocks, no threads, no Qt. The caller
// serializes access, which lets the harness replay whole notification sequences.
//
// Rules (plan D2):
// - Inside one AAP session the parsed value wins per component; a component that
//   reports Disconnected, or Reset() on session end, falls back to BLE.
// - Level rises (and drops of at most kMaxImmediateDrop) are accepted immediately.
//   A larger drop needs two consecutive snapshots with the same candidate level
//   before it is presented, so one bogus low reading cannot fake a sudden drain.
// - Source switching never depends on time windows: only snapshot content and an
//   explicit Reset change it, so the shown source cannot flicker.
class BatteryFusion
{
public:
    // The AAP-side value a component presents; the caller compares it with the
    // BLE reading it already holds and writes the pair into State.
    struct Reading {
        uint8_t level{0};
        // AAP status is authoritative for a presented component.
        bool charging{false};
        // AAP presents this component and BLE disagrees by more than one bucket.
        bool conflict{false};
    };

    // Advertisement batteries are quantized to 10% buckets (AirPodsAdvertisement
    // multiplies the nibble by ten); only differences beyond one bucket are conflicts.
    static constexpr int kBleBucketPercent = 10;
    static constexpr uint8_t kMaxImmediateDrop = 2;

    // Drops every AAP session state; call before the reader starts and on session end.
    void Reset();

    // Consumes one parsed snapshot of the current session. Returns true when the
    // presented view of at least one component changed (the caller republishes).
    bool OnAapSnapshot(const AAP::AAPBatterySnapshot &snapshot);

    // nullopt when AAP does not present the component (pure BLE path). Unknown
    // components never present. `bleLevel` is the current advertisement value,
    // used only for the conflict flag; the caller keeps its own charging bit
    // only while this component falls back to BLE.
    std::optional<Reading>
    Resolve(AAP::BatteryComponent component, std::optional<uint8_t> bleLevel) const;

    bool Presents(AAP::BatteryComponent component) const;

private:
    struct Slot {
        bool presented{false};
        uint8_t level{0};
        bool charging{false};
        // Large drop awaiting its second identical observation.
        std::optional<uint8_t> pendingDrop;
    };

    // Accepts one reading for a presented slot, applying the drop guard. Returns
    // true when the presented view changed.
    static bool Apply(Slot &slot, const AAP::AAPBatteryReading &reading);

    const Slot *slotFor(AAP::BatteryComponent component) const;

    Slot _left;
    Slot _right;
    Slot _caseBox;
};

} // namespace Details
} // namespace Core::AirPods
