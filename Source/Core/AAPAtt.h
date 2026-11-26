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
#include <utility>
#include <vector>

#include "AAPConnectionStateMachine.h"

// ATT (Attribute Protocol) frame helpers for the AAP channel.
//
// The loud-sound-reduction (headphone safety) setting is not an L2CAP control
// command: it is an ATT Write Command to GATT handle 0x1B (`12 1B 00 [01|00]`).
// Control identifier 0x25 in the AAP settings envelope is VolumeSwipeMode, which this
// project neither reads nor writes (Docs/AAP-Protocol-Spec.md §7/§12). LibrePods sends ATT
// PDUs on a separate L2CAP socket (PSM 0x001F); on Windows no third-party implementation
// sends ATT at all, so whether the firmware accepts these frames written into the MagicAAP
// driver pipe (PSM 0x1001) is UNVERIFIED hardware behavior — Manager probes the path once
// per connection with a read request and reports the result (blueprint item 0.3).
//
// Frame layouts (opcode, then little-endian handle, then value):
//   Write Command            12 [handle u16] [value]
//   Read Request             0A [handle u16]
//   Read Response            0B [value]           (no handle — attribution is only valid
//                                                  against a pending request)
//   Handle Value Notification 1B [handle u16] [value]
//   Error Response           01 [request opcode] [handle u16] [error code]
//
namespace Core::AAP::Att {

// The only ATT handle this project touches: headphone safety / loud sound
// reduction. Its CCCD (0x1C) does not work, so the state can only be read back,
// never subscribed to.
inline constexpr uint16_t kLoudSoundReductionHandle = 0x1B;

inline constexpr uint8_t kOpcodeErrorResponse = 0x01;
inline constexpr uint8_t kOpcodeReadRequest = 0x0A;
inline constexpr uint8_t kOpcodeReadResponse = 0x0B;
inline constexpr uint8_t kOpcodeWriteCommand = 0x12;
inline constexpr uint8_t kOpcodeHandleValueNotification = 0x1B;

// ATT Write Command: fire-and-forget write (no response from the peer).
std::vector<uint8_t> BuildWriteCommand(uint16_t handle, uint8_t value);

// ATT Read Request for a single handle.
std::vector<uint8_t> BuildReadRequest(uint16_t handle);

// ATT Read Response (0x0B): returns the value bytes of the last requested
// handle. The frame carries no handle, so it may only be attributed to a request
// the caller still has pending for the same session — PendingReadTracker below is
// what makes that attribution possible. A read response with no matching pending
// request proves nothing about which attribute it answers and must be dropped.
bool IsReadResponse(const std::vector<uint8_t> &data);
std::optional<std::vector<uint8_t>> ParseReadResponse(const std::vector<uint8_t> &data);

// ATT Handle Value Notification (0x1B): carries the handle it belongs to.
bool IsNotification(const std::vector<uint8_t> &data);
std::optional<std::pair<uint16_t, std::vector<uint8_t>>>
ParseNotification(const std::vector<uint8_t> &data);

// ATT Error Response (0x01). A read of handle 0x1B answered with an error
// proves the ATT path is alive even though the value is unavailable.
bool IsErrorResponse(const std::vector<uint8_t> &data);

// Decoded ATT Error Response: which request failed, on which handle, and why.
// The handle is what lets an error be tied back to the pending read instead of
// being credited to whatever the caller happens to be waiting for.
struct ErrorResponse {
    uint8_t requestOpcode{0};
    uint16_t handle{0};
    uint8_t error{0};
};
std::optional<ErrorResponse> ParseErrorResponse(const std::vector<uint8_t> &data);

// True when the frame can be an ATT PDU at all. Every L2CAP AAP frame starts
// with the `04 00 04 00` header (or the `00 00 04 00` handshake), so anything
// matching a known ATT opcode and minimum length is routed to the ATT
// handlers instead of the AAP notification parsers.
bool IsAttFrame(const std::vector<uint8_t> &data);

// Convenience builder: the loud-sound-reduction write frame (identical to
// Packets::BuildLoudSoundReductionPacket; kept here so ATT users have one home).
std::vector<uint8_t> BuildLoudSoundReductionWrite(bool enable);

// One outstanding ATT read per AAP session (blueprint 0.3 / P3).
//
// ATT gives a read response neither a handle nor a request token, so a response is only
// meaningful while exactly one request is outstanding for the session that received it.
// This tracker is that single reservation: it records the AAPAttempt (address +
// generation) and the handle of the outstanding read, plus the steady-clock millisecond at
// which the window closes. Callers reserve before putting the request on the wire, which is
// also what lets an immediate response be correlated instead of racing past a state written
// after the send.
//
// Pure and clock-injected: the caller supplies "now", so every deadline edge is
// reproducible without threads or wall-clock waits. The class is deliberately not
// thread-safe; the owning Manager guards it with its state mutex.
class PendingReadTracker
{
public:
    // Identifies one reservation, not merely its session and handle. An expired reservation
    // may be replaced while the old send is still blocked; only this token lets that send's
    // later failure release the reservation it actually owns.
    using ReservationToken = uint64_t;

    enum class StartResult : uint8_t {
        Started,        // reserved: the caller may write the request frame
        AlreadyPending, // the identical read is still outstanding: stay idempotent, do not
                        // put a second frame or a second deadline on the session
        Rejected        // another read still owns the session, or the request is unusable
    };

    enum class MatchResult : uint8_t {
        NoPendingRead, // nothing outstanding, or the identity/handle does not match
        Expired,       // the window has closed; the answer belongs to no live request
        Matched        // this frame answers the outstanding read
    };

    // The window is half-open: a request is live while nowMs < deadlineMs, so reaching the
    // deadline expires it. With a 500ms window issued at t0 that means t0+499 is still
    // answerable while t0+500 and t0+501 are not. Both the reader path and the lazy
    // GetAttPathState() resolution use this single rule, so a probe can never be flipped to
    // Unconfirmed by the getter and then confirmed by a frame the window had excluded.
    static constexpr bool WindowElapsed(int64_t nowMs, int64_t deadlineMs) noexcept
    {
        return nowMs >= deadlineMs;
    }

    // Reserves `handle` for `attempt` until `deadlineMs` and returns its unique token.
    // An elapsed reservation is replaced. Within one live session, an identical read is
    // idempotent and returns the existing token; a different handle is rejected. A successor
    // generation replaces the old identity, but another address reusing the same generation
    // cannot take over its reservation. Rejected calls set `reservationToken` to zero.
    StartResult Start(
        AAPAttempt attempt, uint16_t handle, int64_t deadlineMs, int64_t nowMs,
        ReservationToken &reservationToken);

    // Classifies an inbound frame. `handle` is nullopt for a read response (the frame carries
    // none) and the parsed handle for an error response, which then has to agree with the
    // reservation. A frame from another address or generation never matches, so a late reply
    // from a retired session cannot confirm or update the current one.
    MatchResult Match(AAPAttempt attempt, std::optional<uint16_t> handle, int64_t nowMs) const;

    // Retires the reservation once its window has elapsed. The caller keeps whatever path
    // state it already learned; only the outstanding request goes away.
    void ClearIfElapsed(int64_t nowMs);

    // Releases the reservation, e.g. once it has been answered or the send failed.
    void Clear();

    // Releases only the reservation identified by `reservationToken`. A request that has
    // already been answered has nothing left to undo, and an old blocked send cannot release
    // a newer same-session replacement.
    bool ReleaseIf(ReservationToken reservationToken) noexcept;

    // True while a reservation exists, whether or not its window is still open.
    bool HasPending() const noexcept
    {
        return _pending.has_value();
    }

    // True while a reservation exists and its window is still open.
    bool IsLive(int64_t nowMs) const noexcept;

    // The handle of the outstanding read; the value of a handle-less read response is
    // interpreted against this, never against whichever feature the caller cares about.
    std::optional<uint16_t> Handle() const noexcept;

    // The session the outstanding read was issued for (identity zero when none).
    AAPAttempt Attempt() const noexcept
    {
        return _pending ? _pending->attempt : AAPAttempt{};
    }

    std::optional<int64_t> DeadlineMs() const noexcept
    {
        return _pending ? std::optional<int64_t>{_pending->deadlineMs} : std::nullopt;
    }

private:
    struct Pending {
        AAPAttempt attempt;
        uint16_t handle{0};
        int64_t deadlineMs{0};
        ReservationToken token{0};
    };

    ReservationToken NextReservationToken() noexcept;

    std::optional<Pending> _pending;
    ReservationToken _nextReservationToken{0};
};

} // namespace Core::AAP::Att
