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

// MagicAAP device binding (P4).
//
// A MagicAAP driver install publishes one device interface per bound accessory,
// and Windows only lets this application open the interface that belongs to the
// device it actually discovered over BLE. All identity decisions therefore live
// here, over plain strings and numbers, so that they are testable without a
// driver, a radio, or earbuds:
//
//   * Windows writes the target's 48-bit address into the BTHENUM instance
//     segment as a complete, delimited token, e.g.
//     `\\?\BTHENUM#{74ec2172-...}_VID&0001004c_PID&2027#a&26af450f&0&340E22CC08A0_C00000000#{...}`
//     and into the parent service device as `BTHENUM\DEV_340E22CC08A0\...`.
//   * A token is only accepted when it is exactly twelve hex digits bounded by
//     the documented delimiters. A twelve-digit run embedded in a longer token is
//     not an address, so partial substrings can never select a device.
//   * A sibling interface without an address token is eligible only when its
//     valid ContainerId exactly matches an address-proven interface. Without
//     that anchor it is unprovable and returns `NoMatch` without being opened.
//   * Every candidate belongs to the target device. Failure on one of its own
//     paths may try another path of that same device, never a foreign device.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Core::MagicAAPWinRT {

// Opaque device handle. The Win32 layer converts `INVALID_HANDLE_VALUE` into
// `nullptr` so the selection logic below needs no Windows headers.
using MagicAAPDeviceHandle = void *;

// One MagicAAP device interface plus every identity Windows can prove for it.
// Empty strings mean "this source is unavailable on this install".
struct MagicAAPDeviceInterface {
    std::wstring interfacePath;
    std::wstring deviceInstanceId;
    std::wstring parentInstanceId;
    std::wstring containerId;
};

// One complete 48-bit Bluetooth address: exactly twelve hex digits.
std::optional<uint64_t> ParseAddressToken(std::wstring_view token);

// Canonical uppercase `340E22CC08A0` rendering used for comparisons and logs.
std::wstring FormatAddressToken(uint64_t address);

// `...&340E22CC08A0_C00000000` — the BTHENUM instance suffix.
std::optional<uint64_t> AddressFromInstanceSuffix(std::wstring_view text);

// `DEV_340E22CC08A0` as a whole PnP field — the parent service device node.
std::optional<uint64_t> AddressFromDevicePrefix(std::wstring_view text);

// Best available proof of this interface's address. Returns `std::nullopt` when
// no source proves one, and also when two sources disagree: a contradictory
// install is not a license to open a device.
std::optional<uint64_t> ExtractAddress(const MagicAAPDeviceInterface &device);

// `{AAACAC36-8F27-50B1-B473-47BF5F0D04F0}` in a stable uppercase form. Returns an
// empty string when the value is not a GUID.
std::wstring NormalizeContainerId(std::wstring_view containerId);

// Why a MagicAAP device-interface connect ended. `BindToTargetInterface` only
// produces Connected, NoMatch, OpenFailed, or Cancelled; the client adds the
// surrounding contract results so a caller can map one stable taxonomy.
enum class MagicAAPBindStatus : uint8_t {
    Connected,      // a proven target interface is open
    NoMatch,        // nothing proves the target identity; no device was opened
    OpenFailed,     // every path of the proven target device failed to open
    Cancelled,      // a stop was requested before or during the bind
    DriverMissing,  // the driver service is absent or stopped
    TransportError, // already connected, or no interface source at all
};

// Counts are diagnostics only. They must never carry an address or a path.
struct MagicAAPBindOutcome {
    MagicAAPBindStatus status{MagicAAPBindStatus::NoMatch};
    MagicAAPDeviceHandle handle{nullptr};
    size_t interfaceCount{0};
    size_t candidateCount{0};
    size_t unprovenCount{0};
    size_t foreignCount{0};
    size_t openAttempts{0};
};

// Opens one candidate interface. `nullptr` means the open failed.
using MagicAAPInterfaceOpener =
    std::function<MagicAAPDeviceHandle(const MagicAAPDeviceInterface &)>;

// Deterministic candidate order, independent of the device-interface enumeration
// order, so a repeated connect attempt opens the same path first.
void SortInterfacesByPath(std::vector<MagicAAPDeviceInterface> &interfaces);

// Removes duplicate interfaces for the same path.
void DeduplicateInterfaces(std::vector<MagicAAPDeviceInterface> &interfaces);

// Binds to the exact target device. Candidates either prove `targetAddress` or
// share the normalized ContainerId of an address-proven sibling. Only paths of
// that target device are tried; an unproven path without an anchor is NoMatch.
MagicAAPBindOutcome BindToTargetInterface(
    const std::vector<MagicAAPDeviceInterface> &interfaces, uint64_t targetAddress,
    const MagicAAPInterfaceOpener &open, std::stop_token stop = {});

// Monotonic session identity for MagicAAP callbacks and disconnect events.
// A receiver thread holds the session it started with and checks it before
// invoking anything, which keeps a retired client from signalling a newer one.
class MagicAAPCallbackSession
{
public:
    // Publishes a new session and returns its id. Ids are never reused, even
    // after Close(), so a callback retired with an earlier session can never
    // collide with a later one.
    uint64_t Start();

    // Leaves no session current while keeping every issued id spent.
    void Close();

    uint64_t Current() const;

    // Session 0 is never handed out, so it never matches.
    bool IsCurrent(uint64_t session) const;

private:
    mutable std::mutex _mutex;
    uint64_t _sequence{0};
    uint64_t _current{0};
};

} // namespace Core::MagicAAPWinRT
