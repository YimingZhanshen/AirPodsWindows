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

// P5: PPE (Personalized Hearing Protection) cap level - the value carried by AAP control
// command 0x38. The domain range is 0-100 (Docs/AAP-Protocol-Spec.md §7); the retired
// adaptive_transparency_level setting never seeds this value. This header is intentionally
// Qt-free so the pure boundary logic stays reachable from the MSVC standalone harness.
//
namespace Core::AAP {

constexpr uint8_t kPpeCapLevelMin = 0;
constexpr uint8_t kPpeCapLevelMax = 100;
constexpr uint8_t kPpeCapLevelDefault = 25;

// Clamp an incoming level into the documented 0-100 wire range. uint8_t input leaves only
// the upper bound open; stale values above 100 (or leftovers of the old 0-50 era that were
// scaled by hand) are capped instead of being sent raw.
constexpr uint8_t NormalizePpeCapLevel(uint8_t level)
{
    return level > kPpeCapLevelMax ? kPpeCapLevelMax : level;
}

} // namespace Core::AAP
