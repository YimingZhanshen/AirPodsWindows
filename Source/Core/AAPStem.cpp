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

#include "AAPStem.h"

#include <algorithm>
#include <array>

namespace Core::AAP {

namespace {

constexpr std::array<uint8_t, 6> kStemRecordPrefix{0x04, 0x00, 0x04, 0x00, 0x19, 0x00};

struct StemWireValue {
    uint8_t wire;
    StemEvent event;
};

constexpr std::array kStemPressValues{
    StemWireValue{0x05, {StemSide::Left, StemPress::Single}},
    StemWireValue{0x06, {StemSide::Left, StemPress::Double}},
    StemWireValue{0x07, {StemSide::Left, StemPress::Triple}},
    StemWireValue{0x08, {StemSide::Left, StemPress::Long}},
};

constexpr std::array kStemSideValues{
    StemWireValue{0x01, {StemSide::Left, StemPress::Single}},
    StemWireValue{0x02, {StemSide::Right, StemPress::Single}},
};

constexpr size_t SideIndex(StemSide side) noexcept
{
    return side == StemSide::Right ? 1U : 0U;
}

int ClampVolumeStep(int volumeStepPercent) noexcept
{
    // The direction is carried by the operation, so only the magnitude matters here. Widening
    // first keeps the negation well defined for the whole int range.
    const auto magnitude = volumeStepPercent < 0 ? -static_cast<long long>(volumeStepPercent)
                                                 : static_cast<long long>(volumeStepPercent);
    return static_cast<int>(std::min<long long>(magnitude, kMaxStemVolumeStepPercent));
}

} // namespace

std::optional<StemEvent> ParseStemEvent(std::span<const uint8_t> packet) noexcept
{
    // Exact length, exact header, documented field values only: a truncated, over-long or
    // unknown-field record is not a stem gesture and must not trigger anything.
    if (packet.size() != kStemRecordSize) {
        return std::nullopt;
    }
    if (!std::equal(kStemRecordPrefix.begin(), kStemRecordPrefix.end(), packet.begin())) {
        return std::nullopt;
    }

    const auto press = std::ranges::find(kStemPressValues, packet[6], &StemWireValue::wire);
    const auto side = std::ranges::find(kStemSideValues, packet[7], &StemWireValue::wire);
    if (press == kStemPressValues.end() || side == kStemSideValues.end()) {
        return std::nullopt;
    }

    return StemEvent{side->event.side, press->event.press};
}

StemAction SelectStemAction(const StemActionTable &table, const StemEvent &event) noexcept
{
    const auto index = StemActionIndex(event.side, event.press);
    if (!index.has_value() || *index >= table.size()) {
        return StemAction::None;
    }
    const auto action = table[*index];
    return PlanStemDispatch(action).operation == StemDispatchPlan::Operation::None
               ? StemAction::None
               : action;
}

StemDispatchPlan PlanStemDispatch(StemAction action, int volumeStepPercent) noexcept
{
    StemDispatchPlan plan;

    switch (action) {
    case StemAction::PlayPause:
        plan.operation = StemDispatchPlan::Operation::TogglePlayPause;
        break;
    case StemAction::NextTrack:
        plan.operation = StemDispatchPlan::Operation::NextTrack;
        break;
    case StemAction::PreviousTrack:
        plan.operation = StemDispatchPlan::Operation::PreviousTrack;
        break;
    case StemAction::VolumeUp:
        plan.operation = StemDispatchPlan::Operation::VolumeUp;
        plan.volumeStepPercent = ClampVolumeStep(volumeStepPercent);
        break;
    case StemAction::VolumeDown:
        plan.operation = StemDispatchPlan::Operation::VolumeDown;
        plan.volumeStepPercent = ClampVolumeStep(volumeStepPercent);
        break;
    case StemAction::ToggleNoiseControl:
        plan.operation = StemDispatchPlan::Operation::ToggleNoiseControl;
        break;
    case StemAction::None:
        plan.operation = StemDispatchPlan::Operation::None;
        break;
    default:
        // A value that is not part of the enum (a corrupt settings blob, for instance) must
        // resolve to no work rather than to an arbitrary action.
        plan.operation = StemDispatchPlan::Operation::None;
        break;
    }

    return plan;
}

std::string_view StemSideName(StemSide side) noexcept
{
    switch (side) {
    case StemSide::Left:
        return "left";
    case StemSide::Right:
        return "right";
    }
    return "unknown";
}

std::string_view StemPressName(StemPress press) noexcept
{
    switch (press) {
    case StemPress::Single:
        return "single";
    case StemPress::Double:
        return "double";
    case StemPress::Triple:
        return "triple";
    case StemPress::Long:
        return "long";
    }
    return "unknown";
}

std::string_view StemActionName(StemAction action) noexcept
{
    switch (action) {
    case StemAction::None:
        return "none";
    case StemAction::PlayPause:
        return "play-pause";
    case StemAction::NextTrack:
        return "next-track";
    case StemAction::PreviousTrack:
        return "previous-track";
    case StemAction::VolumeUp:
        return "volume-up";
    case StemAction::VolumeDown:
        return "volume-down";
    case StemAction::ToggleNoiseControl:
        return "noise-control";
    }
    return "unknown";
}

bool StemPressDeduper::Accept(const StemEvent &event) noexcept
{
    const auto now = _clock ? _clock() : TimePointT{};
    if (!StemPressIndex(event.press).has_value() ||
        (event.side != StemSide::Left && event.side != StemSide::Right))
    {
        return false;
    }
    auto &state = _states[SideIndex(event.side)];

    // The release tail of one continuous long press: only the single press overlaps it, because
    // double and triple presses are distinct gestures the firmware reports on their own.
    const bool longPressTail = event.press == StemPress::Single &&
                               state.lastLongPress.has_value() && now >= *state.lastLongPress &&
                               now - *state.lastLongPress <= _window.longPressTail;

    // The identical record delivered twice (transport replay or a reconnect re-delivery).
    const bool duplicate = state.lastEvent.has_value() && state.lastPress.has_value() &&
                           *state.lastPress == event.press && now >= *state.lastEvent &&
                           now - *state.lastEvent <= _window.repeat;

    if (duplicate) {
        // Keep a replayed long-press burst suppressed until there is a real gap, rather than
        // accepting every Nth duplicate after the first 150 ms window.
        state.lastEvent = now;
        if (event.press == StemPress::Long) {
            state.lastLongPress = now;
        }
        ++_suppressedCount;
        return false;
    }

    if (longPressTail) {
        state.lastLongPress = now;
        ++_suppressedCount;
        return false;
    }
    if (event.press == StemPress::Long) {
        state.lastLongPress = now;
    }
    state.lastEvent = now;
    state.lastPress = event.press;
    return true;
}

void StemPressDeduper::Reset() noexcept
{
    for (auto &state : _states) {
        state = {};
    }
    _suppressedCount = 0;
}

} // namespace Core::AAP
