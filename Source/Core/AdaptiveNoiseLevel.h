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

// D3: Adaptive noise filtering strength - the value carried by AAP control command 0x2E
// (AutoAncStrength, Docs/AAP-Protocol-Spec.md §7). Note the blueprint reserves 0x39 for
// StemConfig (D4); the adaptive-strength wire in AAPManager is and stays 0x2E. The domain
// range is 0-100 with default 50; the setting key is adaptive_noise_level. This header is
// intentionally Qt-free so the pure clamp/mapping/gating logic stays reachable from the settings
// layer, the widgets, and the MSVC standalone harness without pulling in the wire code.
//
// UI↔wire contract: the UI is *filtering strength*, not "more noise". Vendored Android sends
// 100-slider while the wire uses 0=more noise and 100=more filtering, therefore filtering strength
// maps identically to the wire. Do not invert this mapping: UI 0/50/100 -> wire 0/50/100.
//
namespace Core::AAP {

constexpr uint8_t kAdaptiveNoiseLevelMin = 0;
constexpr uint8_t kAdaptiveNoiseLevelMax = 100;
constexpr uint8_t kAdaptiveNoiseLevelDefault = 50;

// Wire value of NoiseControlMode::Adaptive (AAP.h). Kept as a plain constant here so this
// header does not depend on the Qt-backed protocol enums.
constexpr uint8_t kNoiseControlModeAdaptiveWireValue = 0x04;

// Clamp an incoming level into the documented 0-100 range. uint8_t input leaves only the
// upper bound open; hand-edited settings above 100 are capped instead of being sent raw.
constexpr uint8_t NormalizeAdaptiveNoiseLevel(uint8_t level)
{
    return level > kAdaptiveNoiseLevelMax ? kAdaptiveNoiseLevelMax : level;
}

// Explicit UI↔wire mapping for 0x2E. Both ends use filtering strength, so the transform is
// intentionally identity after normalization (the Android UI's 100-slider inversion is not needed).
constexpr uint8_t FilteringStrengthToWire(uint8_t filteringStrength)
{
    return NormalizeAdaptiveNoiseLevel(filteringStrength);
}

constexpr uint8_t FilteringStrengthFromWire(uint8_t wireLevel)
{
    return NormalizeAdaptiveNoiseLevel(wireLevel);
}

// The strength control is adjustable only while the device reports Adaptive mode. An unknown
// mode (nullopt: never reported since connect, or cleared on disconnect) keeps the control
// disarmed so the UI never claims a state the earbuds have not shown.
constexpr bool IsAdaptiveStrengthAdjustable(std::optional<uint8_t> noiseControlModeWireValue)
{
    return noiseControlModeWireValue == kNoiseControlModeAdaptiveWireValue;
}

// Commit policy: a strength change reaches the device only while AAP is connected and the
// reported mode is Adaptive. Switching the noise control mode must never speculatively send
// this command; replaying remembered values after a reconnect is the C7 command broker's job
// (its wire-in replaces the single Submit seam in Core::AirPods::Manager only).
constexpr bool
ShouldCommitAdaptiveNoiseLevel(std::optional<uint8_t> noiseControlModeWireValue, bool aapConnected)
{
    return aapConnected && IsAdaptiveStrengthAdjustable(noiseControlModeWireValue);
}

} // namespace Core::AAP
