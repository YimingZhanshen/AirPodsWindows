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
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Stem (force sensor) press notifications, AAP opcode 0x0019.
//
// PROTOCOL EVIDENCE - the layout is copied from a reference implementation, not guessed:
//   librepods `android/.../bluetooth/AACPManager.kt`
//     :51      Opcodes.STEM_PRESS = 0x19, dispatched by receivePacket() on packet[4]
//     :251-264 parseStemPressResponse(): the record is exactly 8 bytes long, the press type is
//              read from index 6 and the bud from index 7 (index 5 is the opcode's high byte)
//     :132-133 SINGLE_PRESS 0x05, DOUBLE_PRESS 0x06, TRIPLE_PRESS 0x07, LONG_PRESS 0x08
//     :140-141 LEFT 0x01, RIGHT 0x02
//   librepods `docs/opcodes.md`: `0x0019 Host Stem press`.
//   `Docs/UpgradeBlueprint.md` items 2.4 and the opcode table repeat the same constants.
//   This repository's transport gate already sizes the record: `AAPMessage.cpp` maps opcode
//   0x0019 to a minimum length of 8 bytes.
//
// So one notification is `04 00 04 00 19 00 [press] [bud]` and nothing else; the header prefix
// comes from the same file's receivePacket() gate (`04000400`). Anything shorter, longer, or
// carrying an unlisted press/bud byte is NOT a stem event and must produce no action.
//
// NOTE ON SUBSCRIPTION: opcode 0x39 (`StemConfig`, raw-gesture bitmask: single 0x01, double 0x02,
// triple 0x04, long 0x08 - librepods `AACPManager.kt:1125-1136`, `control_commands.md:202-204`)
// is how a host tells the firmware it wants the raw events instead of the built-in behaviour.
// This pre-C7 branch sends it through AAP::Manager directly and replays the desired mask whenever
// a transport finishes its handshake.

namespace Core::AAP {

//////////////////////////////////////////////////
// Gesture model
//

enum class StemSide : uint8_t {
    Left = 0x01,  // librepods StemPressBudType.LEFT
    Right = 0x02, // librepods StemPressBudType.RIGHT
};

enum class StemPress : uint8_t {
    Single = 0x05, // librepods StemPressType.SINGLE_PRESS
    Double = 0x06, // DOUBLE_PRESS
    Triple = 0x07, // TRIPLE_PRESS
    Long = 0x08,   // LONG_PRESS
};

struct StemEvent {
    StemSide side;
    StemPress press;

    friend constexpr bool operator==(const StemEvent &, const StemEvent &) = default;
};

// Identity carried across the queued worker-to-owner boundary. The owner must revalidate it before
// doing media or ANC work; action alone cannot distinguish a live gesture from a pre-disconnect
// one.
struct StemActionRequest {
    int action{0};
    uint64_t address{0};
    uint64_t generation{0};
};

constexpr bool IsCurrentStemActionRequest(
    const StemActionRequest &request, uint64_t currentAddress, uint64_t currentGeneration,
    bool connected) noexcept
{
    return connected && request.generation != 0 && request.address == currentAddress &&
           request.generation == currentGeneration;
}

constexpr size_t kStemSideCount = 2;
constexpr size_t kStemPressCount = 4;
constexpr size_t kStemActionCount = kStemSideCount * kStemPressCount;

// The whole notification, header included (librepods `AACPManager.kt:252`: `data.size != 8`).
constexpr size_t kStemRecordSize = 8;

// What the host does for a gesture. `None` disables the pairing entirely.
// The values are persisted by name through magic_enum, so names are the on-disk ABI:
// never rename one, only append.
enum class StemAction : uint32_t {
    None,
    PlayPause,
    NextTrack,
    PreviousTrack,
    VolumeUp,
    VolumeDown,
    ToggleNoiseControl,
};

using StemActionTable = std::array<StemAction, kStemActionCount>;

// A press bit is subscribed when either bud maps that press to a host action. The firmware has one
// raw-gesture mask for both buds; an invalid persisted action is treated as disabled.
constexpr uint8_t kStemSingleMask = 0x01;
constexpr uint8_t kStemDoubleMask = 0x02;
constexpr uint8_t kStemTripleMask = 0x04;
constexpr uint8_t kStemLongMask = 0x08;
constexpr uint8_t kAllStemMasks =
    kStemSingleMask | kStemDoubleMask | kStemTripleMask | kStemLongMask;

// Desired raw-gesture configuration plus the revision acknowledged by one connection generation.
// The owner serializes access. A failed write never advances the acknowledgement, while a new
// generation must replay even when its mask/revision matches a previous session's success.
class StemConfigReplayState
{
public:
    void SetMask(uint8_t mask)
    {
        _mask = mask & kAllStemMasks;
        ++_revision;
    }

    uint8_t Mask() const
    {
        return _mask;
    }

    template <typename Send>
    bool Replay(uint64_t generation, Send &&send)
    {
        if (generation == 0) {
            return false;
        }
        if (_sent.has_value() && _sent->generation == generation && _sent->revision == _revision) {
            return true;
        }
        if (!send(_mask)) {
            return false;
        }
        _sent = SentState{generation, _revision};
        return true;
    }

private:
    struct SentState {
        uint64_t generation;
        uint64_t revision;
    };

    uint8_t _mask{kAllStemMasks};
    uint64_t _revision{0};
    std::optional<SentState> _sent;
};

uint8_t StemConfigMask(const StemActionTable &actions) noexcept;

namespace Packets {
// Control command 0x0039, Raw Gestures config. The four payload bytes are mask, zero, zero, zero.
std::vector<uint8_t> BuildStemConfigPacket(uint8_t mask);
} // namespace Packets

// Used before the first settings apply as well as by SETTINGS_FIELDS, so a newly constructed
// manager has the same media-control behaviour as a fresh installation.
constexpr StemActionTable DefaultStemActions()
{
    return {StemAction::PlayPause,     StemAction::NextTrack,
            StemAction::PreviousTrack, StemAction::ToggleNoiseControl,
            StemAction::PlayPause,     StemAction::NextTrack,
            StemAction::PreviousTrack, StemAction::ToggleNoiseControl};
}

// Stable, Qt-free names for logs and diagnostics.
std::string_view StemSideName(StemSide side) noexcept;
std::string_view StemPressName(StemPress press) noexcept;
std::string_view StemActionName(StemAction action) noexcept;

// The single index used by every table that is keyed by (side, press). An unknown enum value
// yields nullopt rather than reading past a table.
constexpr std::optional<size_t> StemPressIndex(StemPress press) noexcept
{
    switch (press) {
    case StemPress::Single:
        return 0;
    case StemPress::Double:
        return 1;
    case StemPress::Triple:
        return 2;
    case StemPress::Long:
        return 3;
    }
    return std::nullopt;
}

constexpr std::optional<size_t> StemActionIndex(StemSide side, StemPress press) noexcept
{
    const auto pressIndex = StemPressIndex(press);
    if (!pressIndex.has_value() || (side != StemSide::Left && side != StemSide::Right)) {
        return std::nullopt;
    }
    const size_t sideIndex = side == StemSide::Right ? 1U : 0U;
    return sideIndex * kStemPressCount + *pressIndex;
}

// Parsed from the raw transport record. Returns nullopt for every record that is not an
// exactly-eight-byte `04 00 04 00 19 00` notification with a documented press and bud byte:
// truncated, over-long, mis-prefixed, or unknown-field records never become an event.
std::optional<StemEvent> ParseStemEvent(std::span<const uint8_t> packet) noexcept;

// Gesture-to-action lookup. A table slot holding an unknown value resolves to None.
StemAction SelectStemAction(const StemActionTable &table, const StemEvent &event) noexcept;

//////////////////////////////////////////////////
// Host dispatch plan
//

// The concrete host work behind a configured action, decided as data rather than as an inline
// switch so the mapping is testable without a Windows media session or a paired device.
struct StemDispatchPlan {
    enum class Operation : uint32_t {
        None,
        TogglePlayPause,
        NextTrack,
        PreviousTrack,
        VolumeUp,
        VolumeDown,
        ToggleNoiseControl,
    };

    Operation operation{Operation::None};
    // Only used by the volume operations; always clamped into range.
    int volumeStepPercent{0};

    friend constexpr bool operator==(const StemDispatchPlan &, const StemDispatchPlan &) = default;
};

// One stem press of a volume gesture moves the endpoint by this many percent.
constexpr int kDefaultStemVolumeStepPercent = 5;
// Keeps a mis-set step from slamming the endpoint to 0% or 100% in one press.
constexpr int kMaxStemVolumeStepPercent = 50;

StemDispatchPlan
PlanStemDispatch(StemAction action, int volumeStepPercent = kDefaultStemVolumeStepPercent) noexcept;

//////////////////////////////////////////////////
// Long-press / single-press deduplication
//

// A long press is one continuous gesture: on release the firmware also reports the brief
// contact as a single press, so the tail of a long press must not fire the single-press action a
// second time. Re-delivery of an identical record (transport or reconnect replay) is collapsed
// the same way.
//
// The class is deliberately pure: the clock is injected, so boundaries are exercised with a fake
// timeline instead of wall-clock sleeps. Nothing here touches Qt or a timer.
class StemPressDeduper
{
public:
    using ClockT = std::chrono::steady_clock;
    using TimePointT = ClockT::time_point;
    using ClockFnT = std::function<TimePointT()>;

    struct Window {
        // A single press for the same bud this soon after a long press is the release tail.
        std::chrono::milliseconds longPressTail{std::chrono::milliseconds{400}};
        // The same (side, press) pair this soon after itself is a duplicate record.
        std::chrono::milliseconds repeat{std::chrono::milliseconds{150}};
    };

    explicit StemPressDeduper(ClockFnT clock = &ClockT::now, Window window = {})
        : _clock{std::move(clock)}, _window{window}
    {
    }

    // True when the event should be delivered, false when it was suppressed.
    bool Accept(const StemEvent &event) noexcept;

    // Forget every tracked timestamp: called when a session goes away so a stale suppression
    // window cannot swallow the first press after a reconnect.
    void Reset() noexcept;

    // Number of events suppressed since construction or the last Reset().
    size_t SuppressedCount() const noexcept
    {
        return _suppressedCount;
    }

private:
    struct SideState {
        std::optional<TimePointT> lastLongPress;
        std::optional<TimePointT> lastEvent;
        std::optional<StemPress> lastPress;
    };

    static TimePointT Now() noexcept
    {
        return ClockT::now();
    }

    ClockFnT _clock;
    Window _window;
    std::array<SideState, kStemSideCount> _states;
    size_t _suppressedCount{0};
};

} // namespace Core::AAP
