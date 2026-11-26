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

#include "MagicAAPDeviceBinding.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace Core::MagicAAPWinRT {

namespace {

constexpr size_t kAddressHexDigits = 12;
constexpr std::wstring_view kInstanceAddressSuffix = L"_C00000000";
constexpr std::wstring_view kDeviceAddressPrefix = L"DEV_";

constexpr bool IsHexDigit(wchar_t value)
{
    return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
           (value >= L'A' && value <= L'F');
}

wchar_t ToUpper(wchar_t value)
{
    return static_cast<wchar_t>(std::towupper(value));
}

// PnP writes the address as its own instance part: `&340E22CC08A0_C00000000`.
// Requiring one of these delimiters on the left rejects a twelve-digit window
// carved out of a longer token, which is the difference between a token match
// and the substring guess this file exists to remove.
constexpr bool IsAddressLeftDelimiter(wchar_t value)
{
    return value == L'&' || value == L'_';
}

// Field terminators. `\` closes a device instance id, `#` closes a segment of a
// device interface path.
constexpr bool IsAddressRightDelimiter(wchar_t value)
{
    return value == L'\\' || value == L'#';
}

bool EqualsCaseInsensitive(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (ToUpper(left[i]) != ToUpper(right[i])) {
            return false;
        }
    }
    return true;
}

bool PathLess(std::wstring_view left, std::wstring_view right)
{
    const size_t shared = std::min(left.size(), right.size());
    for (size_t i = 0; i < shared; ++i) {
        const auto upperLeft = ToUpper(left[i]);
        const auto upperRight = ToUpper(right[i]);
        if (upperLeft != upperRight) {
            return upperLeft < upperRight;
        }
    }
    return left.size() < right.size();
}

std::optional<uint64_t> ParseHex(std::wstring_view token)
{
    if (token.size() != kAddressHexDigits || !std::all_of(token.begin(), token.end(), IsHexDigit)) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (const wchar_t digit : token) {
        const auto nibble = digit <= L'9' ? digit - L'0' : ToUpper(digit) - L'A' + 10;
        value = (value << 4) | static_cast<uint64_t>(nibble);
    }
    return value;
}

// Last occurrence, because the address token is the closing part of the
// instance segment and earlier look-alikes must not win.
size_t FindLastCaseInsensitive(std::wstring_view haystack, std::wstring_view needle)
{
    if (needle.empty() || needle.size() > haystack.size()) {
        return std::wstring_view::npos;
    }
    for (size_t start = haystack.size() - needle.size() + 1; start-- > 0;) {
        if (EqualsCaseInsensitive(haystack.substr(start, needle.size()), needle)) {
            return start;
        }
    }
    return std::wstring_view::npos;
}

size_t FindCaseInsensitive(std::wstring_view haystack, std::wstring_view needle, size_t from)
{
    if (needle.empty() || from + needle.size() > haystack.size()) {
        return std::wstring_view::npos;
    }
    for (size_t start = from; start + needle.size() <= haystack.size(); ++start) {
        if (EqualsCaseInsensitive(haystack.substr(start, needle.size()), needle)) {
            return start;
        }
    }
    return std::wstring_view::npos;
}

bool IsGuidBody(std::wstring_view body)
{
    static constexpr size_t kGroups[] = {8, 4, 4, 4, 12};
    size_t consumed = 0;
    for (size_t group = 0; group < 5; ++group) {
        if (group > 0) {
            if (consumed >= body.size() || body[consumed] != L'-') {
                return false;
            }
            ++consumed;
        }
        if (consumed + kGroups[group] > body.size()) {
            return false;
        }
        for (size_t i = 0; i < kGroups[group]; ++i, ++consumed) {
            if (!IsHexDigit(body[consumed])) {
                return false;
            }
        }
    }
    return consumed == body.size();
}

} // namespace

std::optional<uint64_t> ParseAddressToken(std::wstring_view token)
{
    return ParseHex(token);
}

std::wstring FormatAddressToken(uint64_t address)
{
    static constexpr wchar_t kDigits[] = L"0123456789ABCDEF";
    std::wstring token(kAddressHexDigits, L'0');
    for (size_t i = 0; i < kAddressHexDigits; ++i) {
        const size_t shift = (kAddressHexDigits - 1 - i) * 4;
        token[i] = kDigits[(address >> shift) & 0xFULL];
    }
    return token;
}

std::optional<uint64_t> AddressFromInstanceSuffix(std::wstring_view text)
{
    const size_t suffix = FindLastCaseInsensitive(text, kInstanceAddressSuffix);
    if (suffix == std::wstring_view::npos) {
        return std::nullopt;
    }
    const size_t after = suffix + kInstanceAddressSuffix.size();
    if (after != text.size() && !IsAddressRightDelimiter(text[after])) {
        return std::nullopt;
    }
    if (suffix < kAddressHexDigits) {
        return std::nullopt;
    }
    const size_t start = suffix - kAddressHexDigits;
    if (start != 0 && !IsAddressLeftDelimiter(text[start - 1])) {
        return std::nullopt;
    }
    return ParseHex(text.substr(start, kAddressHexDigits));
}

namespace {

std::optional<uint64_t> ParseDevicePrefixes(std::wstring_view text, bool &conflict)
{
    // Windows mixes cases inside one identifier (`_VID&0001004c_PID&2027`), so
    // the field name is matched without regard to case.
    std::optional<uint64_t> resolved;
    for (size_t at = FindCaseInsensitive(text, kDeviceAddressPrefix, 0);
         at != std::wstring_view::npos;
         at = FindCaseInsensitive(text, kDeviceAddressPrefix, at + kDeviceAddressPrefix.size()))
    {
        // `DEV_` must open a PnP field. `BLUETOOTHDEVICE_...` shares the letters
        // but is not an address field, and must never be read as one.
        if (at == 0 || (text[at - 1] != L'\\' && text[at - 1] != L'#')) {
            continue;
        }
        const size_t start = at + kDeviceAddressPrefix.size();
        if (start + kAddressHexDigits > text.size()) {
            continue;
        }
        const size_t after = start + kAddressHexDigits;
        if (after != text.size() && !IsAddressRightDelimiter(text[after])) {
            continue;
        }
        const auto parsed = ParseHex(text.substr(start, kAddressHexDigits));
        if (!parsed.has_value()) {
            continue;
        }
        if (resolved.has_value() && resolved != parsed) {
            conflict = true;
            return std::nullopt;
        }
        resolved = parsed;
    }
    return resolved;
}

std::optional<uint64_t>
ResolveAddress(const MagicAAPDeviceInterface &targetInterface, bool &conflict)
{
    const std::wstring_view sources[] = {
        targetInterface.interfacePath,
        targetInterface.deviceInstanceId,
        targetInterface.parentInstanceId,
    };

    std::optional<uint64_t> resolved;
    for (const auto source : sources) {
        bool sourceConflict = false;
        for (const auto &candidate :
             {AddressFromInstanceSuffix(source), ParseDevicePrefixes(source, sourceConflict)})
        {
            if (!candidate.has_value()) {
                continue;
            }
            if (resolved.has_value() && resolved != candidate) {
                conflict = true;
                return std::nullopt;
            }
            resolved = candidate;
        }
        if (sourceConflict) {
            conflict = true;
            return std::nullopt;
        }
    }
    return resolved;
}

} // namespace

std::optional<uint64_t> AddressFromDevicePrefix(std::wstring_view text)
{
    bool conflict = false;
    return ParseDevicePrefixes(text, conflict);
}

std::optional<uint64_t> ExtractAddress(const MagicAAPDeviceInterface &targetInterface)
{
    bool conflict = false;
    return ResolveAddress(targetInterface, conflict);
}

std::wstring NormalizeContainerId(std::wstring_view containerId)
{
    if (containerId.size() != 38 || containerId.front() != L'{' || containerId.back() != L'}') {
        return {};
    }
    const auto body = containerId.substr(1, containerId.size() - 2);
    if (!IsGuidBody(body)) {
        return {};
    }
    std::wstring normalized;
    normalized.reserve(containerId.size());
    for (const wchar_t value : containerId) {
        normalized.push_back(ToUpper(value));
    }
    return normalized;
}

void SortInterfacesByPath(std::vector<MagicAAPDeviceInterface> &interfaces)
{
    std::stable_sort(interfaces.begin(), interfaces.end(), [](const auto &left, const auto &right) {
        return PathLess(left.interfacePath, right.interfacePath);
    });
}

void DeduplicateInterfaces(std::vector<MagicAAPDeviceInterface> &interfaces)
{
    std::vector<MagicAAPDeviceInterface> unique;
    unique.reserve(interfaces.size());
    for (auto &candidate : interfaces) {
        const bool seen =
            std::any_of(unique.begin(), unique.end(), [&candidate](const auto &existing) {
                return EqualsCaseInsensitive(existing.interfacePath, candidate.interfacePath);
            });
        if (!seen) {
            unique.push_back(std::move(candidate));
        }
    }
    interfaces = std::move(unique);
}

MagicAAPBindOutcome BindToTargetInterface(
    const std::vector<MagicAAPDeviceInterface> &interfaces, uint64_t targetAddress,
    const MagicAAPInterfaceOpener &open, std::stop_token stop)
{
    MagicAAPBindOutcome outcome;
    outcome.interfaceCount = interfaces.size();
    if (stop.stop_requested()) {
        outcome.status = MagicAAPBindStatus::Cancelled;
        return outcome;
    }
    constexpr uint64_t kMaxBluetoothAddress = 0xFFFFFFFFFFFFULL;
    if (targetAddress > kMaxBluetoothAddress) {
        outcome.status = MagicAAPBindStatus::NoMatch;
        return outcome;
    }

    std::vector<const MagicAAPDeviceInterface *> candidates;
    std::vector<std::wstring> targetContainers;
    std::vector<std::wstring> foreignContainers;

    for (const auto &candidate : interfaces) {
        if (candidate.interfacePath.empty()) {
            ++outcome.unprovenCount;
            continue;
        }
        bool conflict = false;
        const auto proven = ResolveAddress(candidate, conflict);
        if (conflict || !proven.has_value()) {
            ++outcome.unprovenCount;
            continue;
        }
        if (*proven != targetAddress) {
            ++outcome.foreignCount;
            if (auto container = NormalizeContainerId(candidate.containerId); !container.empty()) {
                foreignContainers.push_back(std::move(container));
            }
            continue;
        }
        candidates.push_back(&candidate);
        if (auto container = NormalizeContainerId(candidate.containerId); !container.empty()) {
            targetContainers.push_back(std::move(container));
        }
    }

    // A container shared by proven foreign and target interfaces is contradictory,
    // so it cannot anchor an otherwise unprovable sibling path.
    targetContainers.erase(
        std::remove_if(
            targetContainers.begin(), targetContainers.end(),
            [&](const auto &container) {
                return std::find(foreignContainers.begin(), foreignContainers.end(), container) !=
                       foreignContainers.end();
            }),
        targetContainers.end());

    // An interface of the same container that exposes no address token still
    // belongs to the proven device, so it stays a valid second path to open.
    // Without a proven device to anchor on, an unprovable interface is a `NoMatch`
    // and is never opened.
    if (!candidates.empty()) {
        for (const auto &candidate : interfaces) {
            bool conflict = false;
            if (candidate.interfacePath.empty() ||
                ResolveAddress(candidate, conflict).has_value() || conflict)
            {
                continue;
            }
            const auto container = NormalizeContainerId(candidate.containerId);
            if (container.empty()) {
                continue;
            }
            if (std::find(targetContainers.begin(), targetContainers.end(), container) !=
                targetContainers.end())
            {
                candidates.push_back(&candidate);
            }
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const auto *left, const auto *right) {
        return PathLess(left->interfacePath, right->interfacePath);
    });
    candidates.erase(
        std::unique(
            candidates.begin(), candidates.end(),
            [](const auto *left, const auto *right) {
                return EqualsCaseInsensitive(left->interfacePath, right->interfacePath);
            }),
        candidates.end());

    outcome.candidateCount = candidates.size();
    if (candidates.empty()) {
        outcome.status = MagicAAPBindStatus::NoMatch;
        return outcome;
    }

    for (const auto *candidate : candidates) {
        if (stop.stop_requested()) {
            outcome.status = MagicAAPBindStatus::Cancelled;
            return outcome;
        }
        ++outcome.openAttempts;
        if (const auto handle = open(*candidate); handle != nullptr) {
            outcome.handle = handle;
            outcome.status = MagicAAPBindStatus::Connected;
            return outcome;
        }
    }

    // Every path of the proven device failed. Reaching for another device would
    // be exactly the blind fallback this bind removes.
    outcome.status = MagicAAPBindStatus::OpenFailed;
    return outcome;
}

uint64_t MagicAAPCallbackSession::Start()
{
    std::lock_guard lock{_mutex};
    // The issued id is never reused, so a callback retired with an earlier
    // session cannot match a later one even after the current slot is emptied.
    _current = ++_sequence;
    return _current;
}

void MagicAAPCallbackSession::Close()
{
    std::lock_guard lock{_mutex};
    _current = 0;
}

uint64_t MagicAAPCallbackSession::Current() const
{
    std::lock_guard lock{_mutex};
    return _current;
}

bool MagicAAPCallbackSession::IsCurrent(uint64_t session) const
{
    std::lock_guard lock{_mutex};
    return session != 0 && _current == session;
}

} // namespace Core::MagicAAPWinRT
