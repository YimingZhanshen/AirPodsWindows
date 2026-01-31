//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
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
#include <vector>
#include <optional>
#include <functional>

#include "Base.h"
#include "../Helper.h"
#include "../Logger.h"

// AAP = Apple Audio Protocol
// Protocol runs on top of L2CAP with PSM 0x1001 (4097)
//
// Reference: https://github.com/kavishdevar/librepods/blob/main/AAP%20Definitions.md
//
namespace Core::AAP {

constexpr uint16_t kPSM = 0x1001; // L2CAP PSM for AAP

//////////////////////////////////////////////////
// Noise Control Mode
//

enum class NoiseControlMode : uint8_t {
    Off = 0x01,
    NoiseCancellation = 0x02,
    Transparency = 0x03,
    Adaptive = 0x04,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Ear Detection Status
//

enum class EarStatus : uint8_t {
    InEar = 0x00,
    OutOfEar = 0x01,
    InCase = 0x02,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Conversational Awareness State
//

enum class ConversationalAwarenessState : uint8_t {
    Enabled = 0x01,
    Disabled = 0x02,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Personalized Volume State
//

enum class PersonalizedVolumeState : uint8_t {
    Enabled = 0x01,
    Disabled = 0x02,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Loud Sound Reduction (Headphone Safety)
//

enum class LoudSoundReductionState : uint8_t {
    Enabled = 0x01,
    Disabled = 0x00,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Adaptive Transparency Level (only in Adaptive mode)
//

struct AdaptiveTransparencyLevel {
    uint8_t level; // 0-100
};

//////////////////////////////////////////////////
// Low Latency Audio State
//

enum class LowLatencyAudioState : uint8_t {
    Enabled = 0x01,
    Disabled = 0x02,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Speaking Level (for Conversational Awareness)
//

enum class SpeakingLevel : uint8_t {
    StartedSpeaking_GreatlyReduce = 0x01,
    StartedSpeaking_GreatlyReduce2 = 0x02,
    StoppedSpeaking = 0x03,
    // Intermediate values (0x04-0x07) are intermediate volume levels
    NormalVolume = 0x08,
    NormalVolume2 = 0x09,
    Unknown = 0xFF
};

//////////////////////////////////////////////////
// Battery Component
//

enum class BatteryComponent : uint8_t {
    Right = 0x02,
    Left = 0x04,
    Case = 0x08,
    Unknown = 0xFF
};

enum class BatteryStatus : uint8_t {
    Unknown = 0x00,
    Charging = 0x01,
    Discharging = 0x02,
    Disconnected = 0x04
};

//////////////////////////////////////////////////
// AAP Packets
//

namespace Packets {

// Handshake packet - Required to establish connection
// Without this, AirPods will not respond to any packets
inline const std::vector<uint8_t> Handshake = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Enable features packet - Enables Conversational Awareness and Adaptive Transparency
// This is needed for CA to work when audio is playing
inline const std::vector<uint8_t> EnableFeatures = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Request notifications packet - Required to receive battery, ear detection, noise control updates
inline const std::vector<uint8_t> RequestNotifications = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF
};

// Noise Control Mode packet builder
inline std::vector<uint8_t> BuildNoiseControlPacket(NoiseControlMode mode) {
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D,
        static_cast<uint8_t>(mode), 0x00, 0x00, 0x00
    };
}

// Conversational Awareness toggle packet builder
inline std::vector<uint8_t> BuildConversationalAwarenessPacket(bool enable) {
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28,
        enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x02),
        0x00, 0x00, 0x00
    };
}

// Adaptive Audio Noise level packet builder (0-100)
inline std::vector<uint8_t> BuildAdaptiveNoisePacket(uint8_t level) {
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E,
        level, 0x00, 0x00, 0x00
    };
}

// Head tracking start packet
inline const std::vector<uint8_t> StartHeadTracking = {
    0x04, 0x00, 0x04, 0x00, 0x17, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x10, 0x00, 0x08, 0xA1, 0x02, 0x42,
    0x0B, 0x08, 0x0E, 0x10, 0x02, 0x1A, 0x05, 0x01,
    0x40, 0x9C, 0x00, 0x00
};

// Head tracking stop packet
inline const std::vector<uint8_t> StopHeadTracking = {
    0x04, 0x00, 0x04, 0x00, 0x17, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x11, 0x00, 0x08, 0x7E, 0x10, 0x02,
    0x42, 0x0B, 0x08, 0x4E, 0x10, 0x02, 0x1A, 0x05,
    0x01, 0x00, 0x00, 0x00, 0x00
};

// Personalized Volume toggle packet builder
inline std::vector<uint8_t> BuildPersonalizedVolumePacket(bool enable) {
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26,
        enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x02),
        0x00, 0x00, 0x00
    };
}

// Loud Sound Reduction toggle packet builder (Headphone Safety)
inline std::vector<uint8_t> BuildLoudSoundReductionPacket(bool enable) {
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25,
        enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x00),
        0x00, 0x00, 0x00
    };
}

// Off-Ear Auto Pause toggle packet builder (Automatic Ear Detection)
inline std::vector<uint8_t> BuildAutomaticEarDetectionPacket(bool enable) {
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1B,
        enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x02),
        0x00, 0x00, 0x00
    };
}

// Adaptive Transparency level packet builder (0x00-0x32 = 0-50)
inline std::vector<uint8_t> BuildAdaptiveTransparencyLevelPacket(uint8_t level) {
    // Clamp level to 0-50
    if (level > 50) level = 50;
    return {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38,
        level, 0x00, 0x00, 0x00
    };
}

// Request current settings packet
inline const std::vector<uint8_t> RequestSettings = {
    0x04, 0x00, 0x04, 0x00, 0x0D, 0x00, 0xFF, 0xFF, 0xFF, 0xFF
};

} // namespace Packets

//////////////////////////////////////////////////
// Packet Parsing
//

// Parse noise control mode from notification packet
// Packet format: 04 00 04 00 09 00 0D [mode] 00 00 00
inline std::optional<NoiseControlMode> ParseNoiseControlNotification(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    // Check header bytes for noise control notification
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x0D) {
        return std::nullopt;
    }
    
    switch (data[7]) {
        case 0x01: return NoiseControlMode::Off;
        case 0x02: return NoiseControlMode::NoiseCancellation;
        case 0x03: return NoiseControlMode::Transparency;
        case 0x04: return NoiseControlMode::Adaptive;
        default: return NoiseControlMode::Unknown;
    }
}

// Parse conversational awareness state from notification
// Packet format: 04 00 04 00 09 00 28 [status] 00 00 00
inline std::optional<ConversationalAwarenessState> ParseConversationalAwarenessState(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x28) {
        return std::nullopt;
    }
    
    switch (data[7]) {
        case 0x01: return ConversationalAwarenessState::Enabled;
        case 0x02: return ConversationalAwarenessState::Disabled;
        default: return ConversationalAwarenessState::Unknown;
    }
}

// Parse conversational awareness speaking level notification
// Packet format: 04 00 04 00 4B 00 02 00 01 [level]
inline std::optional<SpeakingLevel> ParseSpeakingLevel(const std::vector<uint8_t>& data) {
    if (data.size() < 10) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x4B || data[5] != 0x00 || data[6] != 0x02 || data[7] != 0x00 ||
        data[8] != 0x01) {
        return std::nullopt;
    }
    
    uint8_t level = data[9];
    if (level <= 0x02) {
        return SpeakingLevel::StartedSpeaking_GreatlyReduce;
    } else if (level == 0x03) {
        return SpeakingLevel::StoppedSpeaking;
    } else if (level >= 0x08) {
        return SpeakingLevel::NormalVolume;
    }
    // Intermediate values
    return SpeakingLevel::Unknown;
}

// Parse ear detection notification
// Packet format: 04 00 04 00 06 00 [primary pod] [secondary pod]
inline std::optional<std::pair<EarStatus, EarStatus>> ParseEarDetection(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x06 || data[5] != 0x00) {
        return std::nullopt;
    }
    
    auto toEarStatus = [](uint8_t b) -> EarStatus {
        switch (b) {
            case 0x00: return EarStatus::InEar;
            case 0x01: return EarStatus::OutOfEar;
            case 0x02: return EarStatus::InCase;
            default: return EarStatus::Unknown;
        }
    };
    
    return std::make_pair(toEarStatus(data[6]), toEarStatus(data[7]));
}

// Parse personalized volume state from notification
// Packet format: 04 00 04 00 09 00 26 [status] 00 00 00
inline std::optional<PersonalizedVolumeState> ParsePersonalizedVolumeState(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x26) {
        return std::nullopt;
    }
    
    switch (data[7]) {
        case 0x01: return PersonalizedVolumeState::Enabled;
        case 0x02: return PersonalizedVolumeState::Disabled;
        default: return PersonalizedVolumeState::Unknown;
    }
}

// Parse automatic ear detection (off-ear pause) state from notification
// Packet format: 04 00 04 00 09 00 1B [status] 00 00 00
inline std::optional<bool> ParseAutomaticEarDetectionState(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x1B) {
        return std::nullopt;
    }
    
    // 0x01 = Enabled (pause on ear removal), 0x02 = Disabled
    return data[7] == 0x01;
}

// Parse loud sound reduction (headphone safety) state
// Packet format: 04 00 04 00 09 00 25 [status] 00 00 00
inline std::optional<LoudSoundReductionState> ParseLoudSoundReductionState(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x25) {
        return std::nullopt;
    }
    
    switch (data[7]) {
        case 0x01: return LoudSoundReductionState::Enabled;
        case 0x00: return LoudSoundReductionState::Disabled;
        default: return LoudSoundReductionState::Unknown;
    }
}

// Parse adaptive transparency level
// Packet format: 04 00 04 00 09 00 38 [level] 00 00 00
inline std::optional<uint8_t> ParseAdaptiveTransparencyLevel(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return std::nullopt;
    }
    
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x38) {
        return std::nullopt;
    }
    
    return data[7];
}

// Structure to hold head tracking data
struct HeadTrackingData {
    int16_t orientation1;
    int16_t orientation2;
    int16_t orientation3;
    int16_t horizontalAcceleration;
    int16_t verticalAcceleration;
};

// Parse head tracking sensor data
// Offsets: orientation1=43, orientation2=45, orientation3=47, hAccel=51, vAccel=53
inline std::optional<HeadTrackingData> ParseHeadTrackingData(const std::vector<uint8_t>& data) {
    if (data.size() < 56) {
        return std::nullopt;
    }
    
    HeadTrackingData result;
    
    auto readInt16LE = [&data](size_t offset) -> int16_t {
        return static_cast<int16_t>(data[offset] | (data[offset + 1] << 8));
    };
    
    result.orientation1 = readInt16LE(43);
    result.orientation2 = readInt16LE(45);
    result.orientation3 = readInt16LE(47);
    result.horizontalAcceleration = readInt16LE(51);
    result.verticalAcceleration = readInt16LE(53);
    
    return result;
}

//////////////////////////////////////////////////
// Check if packet is a specific type
//

inline bool IsNoiseControlNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x0D;
}

inline bool IsConversationalAwarenessNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x28;
}

inline bool IsSpeakingLevelNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 9 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x4B && data[5] == 0x00 && data[6] == 0x02 && data[7] == 0x00 &&
           data[8] == 0x01;
}

inline bool IsEarDetectionNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 6 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x06 && data[5] == 0x00;
}

inline bool IsBatteryNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x04 && data[5] == 0x00;
}

inline bool IsPersonalizedVolumeNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x26;
}

inline bool IsAutomaticEarDetectionNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x1B;
}

inline bool IsLoudSoundReductionNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x25;
}

inline bool IsAdaptiveTransparencyLevelNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 7 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x38;
}

// Check if packet is a settings notification (type 0x09)
inline bool IsSettingsNotification(const std::vector<uint8_t>& data) {
    return data.size() >= 6 &&
           data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 && data[3] == 0x00 &&
           data[4] == 0x09 && data[5] == 0x00;
}

// Get the setting type from a settings notification
inline std::optional<uint8_t> GetSettingType(const std::vector<uint8_t>& data) {
    if (!IsSettingsNotification(data) || data.size() < 7) {
        return std::nullopt;
    }
    return data[6];
}

} // namespace Core::AAP

//////////////////////////////////////////////////
// Helper ToString specializations
//

template <>
inline QString Helper::ToString<Core::AAP::NoiseControlMode>(const Core::AAP::NoiseControlMode &value)
{
    switch (value) {
        case Core::AAP::NoiseControlMode::Off:
            return QObject::tr("Off");
        case Core::AAP::NoiseControlMode::NoiseCancellation:
            return QObject::tr("Noise Cancellation");
        case Core::AAP::NoiseControlMode::Transparency:
            return QObject::tr("Transparency");
        case Core::AAP::NoiseControlMode::Adaptive:
            return QObject::tr("Adaptive");
        default:
            return QObject::tr("Unknown");
    }
}

template <>
inline QString Helper::ToString<Core::AAP::ConversationalAwarenessState>(const Core::AAP::ConversationalAwarenessState &value)
{
    switch (value) {
        case Core::AAP::ConversationalAwarenessState::Enabled:
            return QObject::tr("Enabled");
        case Core::AAP::ConversationalAwarenessState::Disabled:
            return QObject::tr("Disabled");
        default:
            return QObject::tr("Unknown");
    }
}
