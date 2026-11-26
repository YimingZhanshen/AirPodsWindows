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

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Core::AAP {

// Transport-record validation only. One SEQPACKET receive or one MagicAAP
// device-interface ReadFile completion is already a complete record; this class
// never reassembles bytes or invents a delimiter.
class AAPMessage
{
public:
    static constexpr size_t kMaxRecordSize = 4096;

    enum class ValidationResult {
        Valid,
        Empty,
        Oversize,
        Truncated,
        InvalidAttOpcode,
        TruncatedAtt,
        TruncatedAap
    };

    static ValidationResult Validate(std::span<const uint8_t> record) noexcept;
    static std::string_view Describe(ValidationResult result) noexcept;
};

} // namespace Core::AAP
