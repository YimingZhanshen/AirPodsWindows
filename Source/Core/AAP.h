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
#include <vector>
#include <optional>
#include <functional>

#include "Base.h"
#include "../Helper.h"
#include "../Logger.h"
#include "PpeCapLevel.h"

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

enum class EarStatus : uint8_t { InEar = 0x00, OutOfEar = 0x01, InCase = 0x02, Unknown = 0xFF };

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

enum class PersonalizedVolumeState : uint8_t { Enabled = 0x01, Disabled = 0x02, Unknown = 0xFF };

//////////////////////////////////////////////////
// Loud Sound Reduction (Headphone Safety)
//

enum class LoudSoundReductionState : uint8_t { Enabled = 0x01, Disabled = 0x00, Unknown = 0xFF };

//////////////////////////////////////////////////
// Volume Swipe Mode (control command 0x25)
//

// Control identifier 0x25 is VolumeSwipeMode (the "swipe on the stem to change volume"
// switch), not loud sound reduction: `04 00 04 00 09 00 25 [01|02] 00 00 00` with the
// usual 0x01 = enabled / 0x02 = disabled convention (Docs/AAP-Protocol-Spec.md §7,
// appendix VolumeSwipe). Loud sound reduction lives on ATT handle 0x1B instead, so this
// value must never be routed into the LSR state. The project does not expose a volume
// swipe control yet; the identifiers exist so an inbound 0x25 echo is recognised for what
// it is and ignored.
enum class VolumeSwipeMode : uint8_t { Enabled = 0x01, Disabled = 0x02, Unknown = 0xFF };

//////////////////////////////////////////////////
// PPE Cap Level (hearing protection, control 0x38; range 0-100)
//

struct PpeCapLevel {
    uint8_t level; // 0-100
};

//////////////////////////////////////////////////
// Low Latency Audio State
//

enum class LowLatencyAudioState : uint8_t { Enabled = 0x01, Disabled = 0x02, Unknown = 0xFF };

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

enum class BatteryComponent : uint8_t { Right = 0x02, Left = 0x04, Case = 0x08, Unknown = 0xFF };

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
inline const std::vector<uint8_t> Handshake = {0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Feature flags packet (opcode 0x004D) - enables conversational awareness while audio is
// playing and adaptive transparency. Byte 6 is a capability mask: both LibrePods platforms send
// `D7` (Android createSetFeatureFlagsPacket, Linux SET_SPECIFIC_FEATURES); a protocol-doc capture
// showed `FF`, i.e. `D7 | 0x28` — the extra bits (0x08/0x20) are undocumented. Send `D7` to match
// the reference implementations; without this packet the earbuds answer a request for Adaptive
// mode (0x0D = 0x04) with a different mode. See Docs/AAP-Protocol-Spec.md §8.
inline const std::vector<uint8_t> EnableFeatures = {0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xD7,
                                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Request notifications packet - Required to receive battery, ear detection, noise control updates
inline const std::vector<uint8_t> RequestNotifications = {0x04, 0x00, 0x04, 0x00, 0x0F,
                                                          0x00, 0xFF, 0xFF, 0xFF, 0xFF};

// Noise Control Mode packet builder
inline std::vector<uint8_t> BuildNoiseControlPacket(NoiseControlMode mode)
{
    return {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, static_cast<uint8_t>(mode), 0x00, 0x00, 0x00};
}

// Conversational Awareness toggle packet builder
inline std::vector<uint8_t> BuildConversationalAwarenessPacket(bool enable)
{
    return {0x04, 0x00, 0x04, 0x00,
            0x09, 0x00, 0x28, enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x02),
            0x00, 0x00, 0x00};
}

// Adaptive Audio Noise level packet builder (0-100)
inline std::vector<uint8_t> BuildAdaptiveNoisePacket(uint8_t level)
{
    return {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, level, 0x00, 0x00, 0x00};
}

// Head tracking start packet
inline const std::vector<uint8_t> StartHeadTracking = {
    0x04, 0x00, 0x04, 0x00, 0x17, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x08, 0xA1,
    0x02, 0x42, 0x0B, 0x08, 0x0E, 0x10, 0x02, 0x1A, 0x05, 0x01, 0x40, 0x9C, 0x00, 0x00};

// Head tracking stop packet
inline const std::vector<uint8_t> StopHeadTracking = {
    0x04, 0x00, 0x04, 0x00, 0x17, 0x00, 0x00, 0x00, 0x10, 0x00, 0x11, 0x00, 0x08, 0x7E, 0x10,
    0x02, 0x42, 0x0B, 0x08, 0x4E, 0x10, 0x02, 0x1A, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00};

// Personalized Volume toggle packet builder
inline std::vector<uint8_t> BuildPersonalizedVolumePacket(bool enable)
{
    return {0x04, 0x00, 0x04, 0x00,
            0x09, 0x00, 0x26, enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x02),
            0x00, 0x00, 0x00};
}

// Loud Sound Reduction toggle packet builder (Headphone Safety)
// This is an ATT write request (PDU 0x12) to GATT handle 0x1B — NOT an L2CAP control command:
// `12 1B 00 [01|00]`, where 0x01 = on, 0x00 = off. Note this feature alone uses 0x01/0x00 while
// the control commands use 0x01/0x02, and its CCCD (0x1C) does not work so the current state can
// only be read back, never subscribed to. Previously sent as control command 0x25, which is
// actually VolumeSwipeMode. See Docs/AAP-Protocol-Spec.md §7/§12.
//
// TRANSPORT CAVEAT: LibrePods sends ATT PDUs on a *separate* L2CAP socket (PSM 0x001F), not on
// the AAP channel (PSM 0x1001) that the MagicAAP driver exposes. Whether the earbuds accept this
// ATT frame written straight into the AAP pipe (what Manager::SendPacket does today) is
// UNVERIFIED; if hardware testing shows it is ignored, the fallback is a second L2CAP connection
// to PSM 0x001F (or WinRT GATT over BR/EDR). Note that no third-party Windows implementation
// (MagicPods, PodBridge, librepods-windows) sends ATT at all; all rely on the driver pipe.
inline std::vector<uint8_t> BuildLoudSoundReductionPacket(bool enable)
{
    return {0x12, 0x1B, 0x00, static_cast<uint8_t>(enable ? 0x01 : 0x00)};
}

// Off-Ear Auto Pause toggle packet builder (Automatic Ear Detection)
// Control command 0x0A = EarDetectionConfig: 0x01 = on, 0x02 = off. Previously sent as 0x1B,
// which is OneBudANCMode (single-bud ANC) — a different feature entirely.
inline std::vector<uint8_t> BuildAutomaticEarDetectionPacket(bool enable)
{
    return {0x04, 0x00, 0x04, 0x00,
            0x09, 0x00, 0x0A, enable ? static_cast<uint8_t>(0x01) : static_cast<uint8_t>(0x02),
            0x00, 0x00, 0x00};
}

// Hearing-protection (PPE) cap level packet builder (0-100)
// Control command 0x38 = PPE Cap Level (a hearing-protection cap, not a transparency control;
// the reference capture uses 0x52 = 82, so the range is 0-100). The retired P5 UI once
// labelled this "adaptive transparency (0-50)"; the label, naming, and slider range were
// corrected in P5 while this wire encoding stayed per spec.
inline std::vector<uint8_t> BuildPpeCapLevelPacket(uint8_t level)
{
    return {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, level, 0x00, 0x00, 0x00};
}

} // namespace Packets

//////////////////////////////////////////////////
// Packet Parsing
//

// Parse noise control mode from notification packet
// Packet format: 04 00 04 00 09 00 0D [mode] 00 00 00
inline std::optional<NoiseControlMode>
ParseNoiseControlNotification(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    // Check header bytes for noise control notification
    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x0D)
    {
        return std::nullopt;
    }

    switch (data[7]) {
    case 0x01:
        return NoiseControlMode::Off;
    case 0x02:
        return NoiseControlMode::NoiseCancellation;
    case 0x03:
        return NoiseControlMode::Transparency;
    case 0x04:
        return NoiseControlMode::Adaptive;
    default:
        return NoiseControlMode::Unknown;
    }
}

// Parse conversational awareness state from notification
// Packet format: 04 00 04 00 09 00 28 [status] 00 00 00
inline std::optional<ConversationalAwarenessState>
ParseConversationalAwarenessState(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x28)
    {
        return std::nullopt;
    }

    switch (data[7]) {
    case 0x01:
        return ConversationalAwarenessState::Enabled;
    case 0x02:
        return ConversationalAwarenessState::Disabled;
    default:
        return ConversationalAwarenessState::Unknown;
    }
}

// Parse conversational awareness speaking level notification
// Packet format: 04 00 04 00 4B 00 02 00 01 [level]
inline std::optional<SpeakingLevel> ParseSpeakingLevel(const std::vector<uint8_t> &data)
{
    if (data.size() < 10) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x4B || data[5] != 0x00 || data[6] != 0x02 || data[7] != 0x00 || data[8] != 0x01)
    {
        return std::nullopt;
    }

    uint8_t level = data[9];
    if (level <= 0x02) {
        return SpeakingLevel::StartedSpeaking_GreatlyReduce;
    }
    else if (level == 0x03) {
        return SpeakingLevel::StoppedSpeaking;
    }
    else if (level >= 0x08) {
        return SpeakingLevel::NormalVolume;
    }
    // Intermediate values
    return SpeakingLevel::Unknown;
}

// Parse ear detection notification
// Packet format: 04 00 04 00 06 00 [primary pod] [secondary pod]
inline std::optional<std::pair<EarStatus, EarStatus>>
ParseEarDetection(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x06 || data[5] != 0x00)
    {
        return std::nullopt;
    }

    auto toEarStatus = [](uint8_t b) -> EarStatus {
        switch (b) {
        case 0x00:
            return EarStatus::InEar;
        case 0x01:
            return EarStatus::OutOfEar;
        case 0x02:
            return EarStatus::InCase;
        default:
            return EarStatus::Unknown;
        }
    };

    return std::make_pair(toEarStatus(data[6]), toEarStatus(data[7]));
}

// Parse personalized volume state from notification
// Packet format: 04 00 04 00 09 00 26 [status] 00 00 00
inline std::optional<PersonalizedVolumeState>
ParsePersonalizedVolumeState(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x26)
    {
        return std::nullopt;
    }

    switch (data[7]) {
    case 0x01:
        return PersonalizedVolumeState::Enabled;
    case 0x02:
        return PersonalizedVolumeState::Disabled;
    default:
        return PersonalizedVolumeState::Unknown;
    }
}

// Parse automatic ear detection (off-ear pause) state from notification
// Control command 0x0A = EarDetectionConfig; the state echo follows the same identifier:
// 04 00 04 00 09 00 0A [status] 00 00 00, 0x01 = enabled, 0x02 = disabled.
// (This used to match 0x1B, which is OneBudANCMode — wrong feature.) See spec §7.
inline std::optional<bool> ParseAutomaticEarDetectionState(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x0A)
    {
        return std::nullopt;
    }

    // 0x01 = Enabled (pause on ear removal), 0x02 = Disabled
    return data[7] == 0x01;
}

// Loud sound reduction has no control-command parser on purpose. Its state only exists on
// ATT handle 0x1B and comes back over ATT (read response 0x0B / notification `1B 1B 00 …`),
// where it is attributed to the pending read of that handle
// (Core::AAP::Att::PendingReadTracker). The 0x25 identifier this parser used to claim is
// VolumeSwipeMode, so reading a control reply can never produce a LoudSoundReductionState.
// See spec §7/§12.

// Parse the volume swipe (0x25) state echo.
// Packet format: 04 00 04 00 09 00 25 [status] 00 00 00
inline std::optional<VolumeSwipeMode> ParseVolumeSwipeMode(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x25)
    {
        return std::nullopt;
    }

    switch (data[7]) {
    case 0x01:
        return VolumeSwipeMode::Enabled;
    case 0x02:
        return VolumeSwipeMode::Disabled;
    default:
        return VolumeSwipeMode::Unknown;
    }
}

// Parse PPE cap level (the retired "adaptive transparency" naming was a mislabel: control
// 0x38 caps output loudness for hearing protection)
// Packet format: 04 00 04 00 09 00 38 [level] 00 00 00
inline std::optional<uint8_t> ParsePpeCapLevel(const std::vector<uint8_t> &data)
{
    if (data.size() < 8) {
        return std::nullopt;
    }

    if (data[0] != 0x04 || data[1] != 0x00 || data[2] != 0x04 || data[3] != 0x00 ||
        data[4] != 0x09 || data[5] != 0x00 || data[6] != 0x38)
    {
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
inline std::optional<HeadTrackingData> ParseHeadTrackingData(const std::vector<uint8_t> &data)
{
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

inline bool IsNoiseControlNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x0D;
}

inline bool IsConversationalAwarenessNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x28;
}

inline bool IsSpeakingLevelNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 9 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x4B && data[5] == 0x00 && data[6] == 0x02 &&
           data[7] == 0x00 && data[8] == 0x01;
}

inline bool IsEarDetectionNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 6 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x06 && data[5] == 0x00;
}

inline bool IsBatteryNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x04 && data[5] == 0x00;
}

inline bool IsPersonalizedVolumeNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x26;
}

inline bool IsAutomaticEarDetectionNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x0A;
}

// Volume swipe echo (0x25). Named for what the identifier actually is: an earlier version
// of this project read 0x25 as loud sound reduction and pushed the result into the LSR
// state, which is the misclassification P3 removes.
inline bool IsVolumeSwipeNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x25;
}

inline bool IsPpeCapLevelNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 7 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00 && data[6] == 0x38;
}

// Check if packet is a settings notification (type 0x09)
inline bool IsSettingsNotification(const std::vector<uint8_t> &data)
{
    return data.size() >= 6 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x09 && data[5] == 0x00;
}

// Get the setting type from a settings notification
inline std::optional<uint8_t> GetSettingType(const std::vector<uint8_t> &data)
{
    if (!IsSettingsNotification(data) || data.size() < 7) {
        return std::nullopt;
    }
    return data[6];
}

//////////////////////////////////////////////////
// Device acknowledgements (blueprint 0.3 / P3)
//

// The device answers the handshake with the four-byte record `01 00 04 00` and the feature
// flags packet (0x4D) with `04 00 04 00 2B 00` (Docs/AAP-Protocol-Spec.md, connection
// sequence and appendix Parse::HANDSHAKE_ACK / FEATURES_ACK). AAPMessage already accepts both
// as complete transport records; these classifiers give them a meaning, so a connection step
// can be recorded as "sent, waiting for the device" and only later as acknowledged. Not every
// firmware revision answers, therefore observing an ACK is evidence when it arrives and never
// a precondition for continuing the handshake.
inline bool IsHandshakeAck(const std::vector<uint8_t> &data)
{
    return data.size() == 4 && data[0] == 0x01 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00;
}

inline bool IsFeatureFlagsAck(const std::vector<uint8_t> &data)
{
    return data.size() >= 6 && data[0] == 0x04 && data[1] == 0x00 && data[2] == 0x04 &&
           data[3] == 0x00 && data[4] == 0x2B && data[5] == 0x00;
}

} // namespace Core::AAP

//////////////////////////////////////////////////
// Helper ToString specializations
//

template <>
inline QString
Helper::ToString<Core::AAP::NoiseControlMode>(const Core::AAP::NoiseControlMode &value)
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
inline QString Helper::ToString<Core::AAP::ConversationalAwarenessState>(
    const Core::AAP::ConversationalAwarenessState &value)
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
