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

#include <cstdint>
#include <vector>

#include <QtTest>

#include "Source/Core/AAP.h"

// Table-driven coverage for the AAP protocol layer (blueprint items 0.1 / 0.2 / 0.5):
// every packet builder emits exactly the documented bytes, every parser accepts the
// documented notification frames, and the control identifiers corrected against
// librepods' control_commands.md (0x0A ear detection, 0x38 PPE, ATT handle 0x1B for
// loud sound reduction) stay corrected.

namespace {

using namespace Core::AAP;

std::vector<uint8_t> Hex(std::initializer_list<int> bytes)
{
    std::vector<uint8_t> result;
    result.reserve(bytes.size());
    for (int byte : bytes) {
        result.push_back(static_cast<uint8_t>(byte));
    }
    return result;
}

} // namespace

class AAPProtocolTests : public QObject
{
    Q_OBJECT

private slots:
    //////////////////////////////////////////////////
    // Fixed packets (byte-exact regression locks)
    //

    void HandshakePacketIsSixteenBytes()
    {
        QCOMPARE(
            Packets::Handshake,
            Hex({0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00}));
    }

    // Blueprint 0.2: the capability mask is D7 (LibrePods Android/Linux), not the
    // undocumented FF seen in one protocol-doc capture.
    void EnableFeaturesSendsD7CapabilityMask()
    {
        QCOMPARE(
            Packets::EnableFeatures,
            Hex({0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xD7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }

    void RequestNotificationsSubscribesAllSettings()
    {
        QCOMPARE(
            Packets::RequestNotifications,
            Hex({0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}));
    }

    //////////////////////////////////////////////////
    // Control command builders
    //

    void NoiseControlPacketCoversAllModes()
    {
        struct
        {
            NoiseControlMode mode;
            uint8_t wire;
        } cases[] = {
            {NoiseControlMode::Off, 0x01},
            {NoiseControlMode::NoiseCancellation, 0x02},
            {NoiseControlMode::Transparency, 0x03},
            {NoiseControlMode::Adaptive, 0x04},
        };

        for (const auto &item : cases) {
            auto packet = Packets::BuildNoiseControlPacket(item.mode);
            QCOMPARE(
                packet, Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, item.wire, 0x00, 0x00, 0x00}));
        }
    }

    void ConversationalAwarenessPacketUsesEnableDisableConvention()
    {
        QCOMPARE(
            Packets::BuildConversationalAwarenessPacket(true),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00}));
        QCOMPARE(
            Packets::BuildConversationalAwarenessPacket(false),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x02, 0x00, 0x00, 0x00}));
    }

    void PersonalizedVolumePacketUsesEnableDisableConvention()
    {
        QCOMPARE(
            Packets::BuildPersonalizedVolumePacket(true),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01, 0x00, 0x00, 0x00}));
        QCOMPARE(
            Packets::BuildPersonalizedVolumePacket(false),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x02, 0x00, 0x00, 0x00}));
    }

    void AdaptiveNoisePacketCarriesLevel()
    {
        QCOMPARE(
            Packets::BuildAdaptiveNoisePacket(100),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100, 0x00, 0x00, 0x00}));
    }

    // Blueprint 0.1: automatic ear detection must send control command 0x0A
    // (EarDetectionConfig), not 0x1B (OneBudANCMode).
    void AutomaticEarDetectionPacketUsesControlCommand0x0A()
    {
        QCOMPARE(
            Packets::BuildAutomaticEarDetectionPacket(true),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x01, 0x00, 0x00, 0x00}));
        QCOMPARE(
            Packets::BuildAutomaticEarDetectionPacket(false),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x02, 0x00, 0x00, 0x00}));
    }

    // Blueprint 0.1: control command 0x38 is the PPE (hearing-protection) cap level,
    // range 0-100; the old "adaptive transparency (0-50)" interpretation is gone.
    void PpeCapLevelPacketUsesControlCommand0x38()
    {
        QCOMPARE(
            Packets::BuildPpeCapLevelPacket(82),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 82, 0x00, 0x00, 0x00}));
        QCOMPARE(
            Packets::BuildPpeCapLevelPacket(100),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 100, 0x00, 0x00, 0x00}));
    }

    // Blueprint 0.1/0.3: loud sound reduction is an ATT Write Command to handle 0x1B
    // and alone uses the 0x01/0x00 (not 0x01/0x02) value convention.
    void LoudSoundReductionPacketIsAttWriteToHandle0x1B()
    {
        QCOMPARE(Packets::BuildLoudSoundReductionPacket(true), Hex({0x12, 0x1B, 0x00, 0x01}));
        QCOMPARE(Packets::BuildLoudSoundReductionPacket(false), Hex({0x12, 0x1B, 0x00, 0x00}));
    }

    //////////////////////////////////////////////////
    // Notification parsers
    //

    void NoiseControlParserAcceptsAllModes()
    {
        QCOMPARE(
            ParseNoiseControlNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01})),
            std::optional(NoiseControlMode::Off));
        QCOMPARE(
            ParseNoiseControlNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02})),
            std::optional(NoiseControlMode::NoiseCancellation));
        QCOMPARE(
            ParseNoiseControlNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x03})),
            std::optional(NoiseControlMode::Transparency));
        QCOMPARE(
            ParseNoiseControlNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x04})),
            std::optional(NoiseControlMode::Adaptive));
        QCOMPARE(
            ParseNoiseControlNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x7F})),
            std::optional(NoiseControlMode::Unknown));
        QVERIFY(!ParseNoiseControlNotification({0x00}).has_value());
        QVERIFY(!ParseNoiseControlNotification({}).has_value());
    }

    void ConversationalAwarenessParserAcceptsBothStates()
    {
        QCOMPARE(
            ParseConversationalAwarenessState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01})),
            std::optional(ConversationalAwarenessState::Enabled));
        QCOMPARE(
            ParseConversationalAwarenessState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x02})),
            std::optional(ConversationalAwarenessState::Disabled));
        QVERIFY(!ParseConversationalAwarenessState(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01}))
                     .has_value());
    }

    // Blueprint 0.5: speaking levels 0x01-0x09 all classified; 0x04-0x07 are the
    // intermediate ramp and map to Unknown by design.
    void SpeakingLevelParserCoversEveryDocumentedLevel()
    {
        auto frame = [](uint8_t level) {
            return Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, level});
        };

        QCOMPARE(
            ParseSpeakingLevel(frame(0x01)),
            std::optional(SpeakingLevel::StartedSpeaking_GreatlyReduce));
        QCOMPARE(
            ParseSpeakingLevel(frame(0x02)),
            std::optional(SpeakingLevel::StartedSpeaking_GreatlyReduce));
        QCOMPARE(
            ParseSpeakingLevel(frame(0x03)), std::optional(SpeakingLevel::StoppedSpeaking));
        for (uint8_t level = 0x04; level <= 0x07; ++level) {
            QCOMPARE(ParseSpeakingLevel(frame(level)), std::optional(SpeakingLevel::Unknown));
        }
        QCOMPARE(ParseSpeakingLevel(frame(0x08)), std::optional(SpeakingLevel::NormalVolume));
        QCOMPARE(ParseSpeakingLevel(frame(0x09)), std::optional(SpeakingLevel::NormalVolume));

        // The classifier folds everything <= 0x02 into the "start speaking" state.
        QCOMPARE(
            ParseSpeakingLevel(frame(0x00)),
            std::optional(SpeakingLevel::StartedSpeaking_GreatlyReduce));
        QVERIFY(!ParseSpeakingLevel(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01}))
                     .has_value());
    }

    void EarDetectionParserReadsBothSides()
    {
        auto result = ParseEarDetection(
            Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x02}));
        QVERIFY(result.has_value());
        QCOMPARE(result->first, EarStatus::InEar);
        QCOMPARE(result->second, EarStatus::InCase);

        auto outOfEar = ParseEarDetection(
            Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x01, 0x01}));
        QVERIFY(outOfEar.has_value());
        QCOMPARE(outOfEar->first, EarStatus::OutOfEar);

        auto unknown = ParseEarDetection(Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x09, 0x00}));
        QVERIFY(unknown.has_value());
        QCOMPARE(unknown->first, EarStatus::Unknown);

        QVERIFY(!ParseEarDetection(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00})).has_value());
    }

    void PersonalizedVolumeParserAcceptsBothStates()
    {
        QCOMPARE(
            ParsePersonalizedVolumeState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01})),
            std::optional(PersonalizedVolumeState::Enabled));
        QCOMPARE(
            ParsePersonalizedVolumeState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x02})),
            std::optional(PersonalizedVolumeState::Disabled));
    }

    // Blueprint 0.1: the parser matches the 0x0A echo (EarDetectionConfig), and the
    // old 0x1B frame must not be mistaken for it.
    void AutomaticEarDetectionParserMatches0x0AOnly()
    {
        QCOMPARE(
            ParseAutomaticEarDetectionState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x01})),
            std::optional(true));
        QCOMPARE(
            ParseAutomaticEarDetectionState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x02})),
            std::optional(false));
        // OneBudANCMode echo (the identifier this feature used to misuse) is rejected.
        QVERIFY(!ParseAutomaticEarDetectionState(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1B, 0x01}))
                     .has_value());
    }

    void PpeCapLevelParserReadsLevel()
    {
        QCOMPARE(
            ParseAdaptiveTransparencyLevel(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 82})),
            std::optional<uint8_t>(82));
        QVERIFY(!ParseAdaptiveTransparencyLevel(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 82}))
                     .has_value());
    }

    // Kept for the legacy control path: 0x25 is VolumeSwipeMode, and the parser only
    // claims to understand the old loud-sound-reduction echo format.
    void LegacyLoudSoundReductionParserMatches0x25()
    {
        QCOMPARE(
            ParseLoudSoundReductionState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x01})),
            std::optional(LoudSoundReductionState::Enabled));
        QCOMPARE(
            ParseLoudSoundReductionState(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x00})),
            std::optional(LoudSoundReductionState::Disabled));
    }

    void HeadTrackingParserReadsLittleEndianOffsets()
    {
        std::vector<uint8_t> frame(56, 0x00);
        auto writeInt16 = [&frame](size_t offset, int16_t value) {
            frame[offset] = static_cast<uint8_t>(value & 0xFF);
            frame[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        writeInt16(43, 0x1234);
        writeInt16(45, -2);
        writeInt16(47, 0x7FFF);
        writeInt16(51, -32768);
        writeInt16(53, 42);

        auto result = ParseHeadTrackingData(frame);
        QVERIFY(result.has_value());
        QCOMPARE(result->orientation1, int16_t(0x1234));
        QCOMPARE(result->orientation2, int16_t(-2));
        QCOMPARE(result->orientation3, int16_t(0x7FFF));
        QCOMPARE(result->horizontalAcceleration, int16_t(-32768));
        QCOMPARE(result->verticalAcceleration, int16_t(42));

        frame.resize(55);
        QVERIFY(!ParseHeadTrackingData(frame).has_value());
    }

    //////////////////////////////////////////////////
    // Notification classifiers
    //

    void ClassifierMatchesDocumentedFrames()
    {
        QVERIFY(IsNoiseControlNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02})));
        QVERIFY(IsConversationalAwarenessNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01})));
        QVERIFY(IsSpeakingLevelNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x08})));
        QVERIFY(IsEarDetectionNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00})));
        QVERIFY(IsBatteryNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01})));
        QVERIFY(IsPersonalizedVolumeNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01})));
        QVERIFY(IsAutomaticEarDetectionNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x01})));
        QVERIFY(IsSettingsNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 0x10})));
        QCOMPARE(
            GetSettingType(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 0x10})),
            std::optional<uint8_t>(0x38));
    }

    // The loud-sound-reduction ATT frame is not an L2CAP settings notification; the
    // send path must never be reclassified as an echo of any 0x09 command.
    void AttFrameIsNotClassifiedAsSettingsNotification()
    {
        QVERIFY(!IsSettingsNotification(Packets::BuildLoudSoundReductionPacket(true)));
        QVERIFY(!IsSettingsNotification(Packets::BuildLoudSoundReductionPacket(false)));
        QVERIFY(!IsLoudSoundReductionNotification(Packets::BuildLoudSoundReductionPacket(true)));
        QVERIFY(!IsAutomaticEarDetectionNotification(
                     Packets::BuildLoudSoundReductionPacket(true)));
    }

    // Truncated frames must be rejected everywhere, never read out of bounds.
    void TruncatedFramesAreRejected()
    {
        for (size_t size = 0; size < 6; ++size) {
            std::vector<uint8_t> frame(size, 0x04);
            QVERIFY(!IsNoiseControlNotification(frame));
            QVERIFY(!IsSettingsNotification(frame));
        }
        QVERIFY(!ParseNoiseControlNotification({0x04, 0x00, 0x04, 0x00, 0x09, 0x00}).has_value());
        QVERIFY(!ParseEarDetection({0x04, 0x00, 0x04, 0x00}).has_value());
        QVERIFY(!ParseSpeakingLevel(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01}))
                     .has_value());
    }
};

QTEST_GUILESS_MAIN(AAPProtocolTests)

#include "AAPProtocolTests.moc"
