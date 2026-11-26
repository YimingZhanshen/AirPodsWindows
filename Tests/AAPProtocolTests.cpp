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

#include <array>
#include <cstdint>
#include <vector>

#include <QtTest>

#include "Source/Core/AAP.h"
#include "Source/Core/AAPAtt.h"
#include "Source/Core/AAPBattery.h"
#include "Source/Core/AAPDeviceInfo.h"
#include "Source/Core/ConnectionDiagnostics.h"

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

std::vector<uint8_t> FromHex(const QByteArray &hex)
{
    const auto bytes = QByteArray::fromHex(hex);
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// Byte-exact 222-byte 0x001D device-information record from librepods'
// "AAP Definitions.md" example packet (the reference capture also documented in
// Docs/AAP-Protocol-Spec.md §3). Values verified against the same table:
// name, model, manufacturer, serial, firmware x2, hardware revision, updater id,
// left/right serials, numeric version, a 35-byte encrypted blob, and a trailing
// numeric string.
const std::vector<uint8_t> &DeviceInfoSample()
{
    static const std::vector<uint8_t> sample = FromHex(QByteArrayLiteral(
        "040004001d0002d5000400416972506f64732050726f004133303438004170706c6520496e632e0051584e52"
        "4848595850360036312e313836383034303030323030303030302e323731330036312e313836383034303030"
        "323030303030302e3237313300312e302e3000636f6d2e6170706c652e6163636573736f72792e7570646174"
        "65722e6170702e3731004859394c5432454632364a59004833504c5748444a32364b3000363335373533360089"
        "312a6567a5400f84a3ca234947efd40b90d78436ae5946748d70273e66066a258930003533393530363036340"
        "0"));
    return sample;
}

// Builds a syntactically shaped 0x001D record carrying the given NUL-separated
// fields behind the standard header and `02 D5 00 04 00` preamble.
std::vector<uint8_t> BuildDeviceInfoPacket(std::initializer_list<QByteArray> fields)
{
    std::vector<uint8_t> packet =
        Hex({0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xD5, 0x00, 0x04, 0x00});
    for (const auto &field : fields) {
        packet.insert(packet.end(), field.constData(), field.constData() + field.size());
        packet.push_back(0x00);
    }
    return packet;
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
            Packets::Handshake, Hex(
                                    {0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    }

    // Blueprint 0.2: the capability mask is D7 (LibrePods Android/Linux), not the
    // undocumented FF seen in one protocol-doc capture.
    void EnableFeaturesSendsD7CapabilityMask()
    {
        QCOMPARE(
            Packets::EnableFeatures, Hex(
                                         {0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xD7, 0x00, 0x00,
                                          0x00, 0x00, 0x00, 0x00, 0x00}));
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
        struct {
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
                packet,
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, item.wire, 0x00, 0x00, 0x00}));
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

    // P5: the domain boundary clamps into 0-100 before the value is cached or put on the
    // wire; stale uint8 values above 100 (old 0-50 UI era included) never leak through.
    void PpeCapNormalizeClampsToZeroThroughHundred()
    {
        QCOMPARE(NormalizePpeCapLevel(0), uint8_t{0});
        QCOMPARE(NormalizePpeCapLevel(25), uint8_t{25});
        QCOMPARE(NormalizePpeCapLevel(50), uint8_t{50});
        QCOMPARE(NormalizePpeCapLevel(100), uint8_t{100});
        QCOMPARE(NormalizePpeCapLevel(101), uint8_t{100});
        QCOMPARE(NormalizePpeCapLevel(255), uint8_t{100});
        QCOMPARE(kPpeCapLevelDefault, uint8_t{25});
        QCOMPARE(kPpeCapLevelMax, uint8_t{100});
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
            ParseNoiseControlNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01})),
            std::optional(NoiseControlMode::Off));
        QCOMPARE(
            ParseNoiseControlNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02})),
            std::optional(NoiseControlMode::NoiseCancellation));
        QCOMPARE(
            ParseNoiseControlNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x03})),
            std::optional(NoiseControlMode::Transparency));
        QCOMPARE(
            ParseNoiseControlNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x04})),
            std::optional(NoiseControlMode::Adaptive));
        QCOMPARE(
            ParseNoiseControlNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x7F})),
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
        QCOMPARE(ParseSpeakingLevel(frame(0x03)), std::optional(SpeakingLevel::StoppedSpeaking));
        for (uint8_t level = 0x04; level <= 0x07; ++level) {
            QCOMPARE(ParseSpeakingLevel(frame(level)), std::optional(SpeakingLevel::Unknown));
        }
        QCOMPARE(ParseSpeakingLevel(frame(0x08)), std::optional(SpeakingLevel::NormalVolume));
        QCOMPARE(ParseSpeakingLevel(frame(0x09)), std::optional(SpeakingLevel::NormalVolume));

        // The classifier folds everything <= 0x02 into the "start speaking" state.
        QCOMPARE(
            ParseSpeakingLevel(frame(0x00)),
            std::optional(SpeakingLevel::StartedSpeaking_GreatlyReduce));
        QVERIFY(!ParseSpeakingLevel(Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01}))
                     .has_value());
    }

    void EarDetectionParserReadsBothSides()
    {
        auto result = ParseEarDetection(Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x02}));
        QVERIFY(result.has_value());
        QCOMPARE(result->first, EarStatus::InEar);
        QCOMPARE(result->second, EarStatus::InCase);

        auto outOfEar = ParseEarDetection(Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x01, 0x01}));
        QVERIFY(outOfEar.has_value());
        QCOMPARE(outOfEar->first, EarStatus::OutOfEar);

        auto unknown = ParseEarDetection(Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x09, 0x00}));
        QVERIFY(unknown.has_value());
        QCOMPARE(unknown->first, EarStatus::Unknown);

        QVERIFY(!ParseEarDetection(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00})).has_value());
    }

    // D2: battery notification parser (opcode 0x0004), 1% precision. The frames follow
    // Docs/AAP-Protocol-Spec.md §1: 04 00 04 00 04 00 <count> then per component
    // [id, 0x01 spacer, level, status, 0x01 end marker].
    void BatteryParserReadsEveryComponentCount()
    {
        struct ParserCase {
            std::vector<uint8_t> frame;
            std::optional<uint8_t> left;
            std::optional<uint8_t> right;
            std::optional<uint8_t> caseBox;
        };
        // Table-driven 12B/17B/22B records, including both component orders.
        const ParserCase cases[] = {
            {Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x08, 0x01, 17, 0x02, 0x01}),
             std::nullopt, std::nullopt, 17},
            {Hex(
                 {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x02, 0x01, 64, 0x01, 0x01, 0x04, 0x01,
                  33, 0x00, 0x01}),
             33, 64, std::nullopt},
            {Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03, 0x02, 0x01, 100,  0x02,
                  0x01, 0x04, 0x01, 99,   0x01, 0x01, 0x08, 0x01, 17,   0x02, 0x01}),
             99, 100, 17},
        };

        for (const auto &item : cases) {
            auto parsed = ParseBatteryNotification(item.frame);
            QVERIFY2(parsed.has_value(), QByteArray::number(item.frame.size()).constData());
            const auto levelOf = [](const auto &reading) {
                return reading.has_value() ? std::optional<uint8_t>(reading->level) : std::nullopt;
            };
            QCOMPARE(levelOf(parsed->left), item.left);
            QCOMPARE(levelOf(parsed->right), item.right);
            QCOMPARE(levelOf(parsed->caseBox), item.caseBox);
        }
        QCOMPARE(cases[0].frame.size(), size_t{12});
        QCOMPARE(cases[1].frame.size(), size_t{17});
        QCOMPARE(cases[2].frame.size(), size_t{22});
    }

    void BatteryParserMapsHeadsetToSingleBatterySlot()
    {
        // Docs/AAP-Protocol-Spec.md §1 maps 0x01 to Headset/Max. The application already
        // presents every single-battery model through pods.left, so AAP uses that same slot.
        auto parsed = ParseBatteryNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x01, 0x01, 73, 0x01, 0x01}));
        QVERIFY(parsed.has_value());
        QVERIFY(parsed->left.has_value());
        QCOMPARE(parsed->left->component, BatteryComponent::Headset);
        QCOMPARE(parsed->left->level, uint8_t(73));
        QCOMPARE(parsed->left->status, BatteryStatus::Charging);
        QVERIFY(!parsed->right.has_value());
        QVERIFY(!parsed->caseBox.has_value());

        // Headset and Left share one presentation slot and cannot both occupy it.
        QVERIFY(!ParseBatteryNotification(Hex(
                                              {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x01, 0x01,
                                               73, 0x01, 0x01, 0x04, 0x01, 70, 0x01, 0x01}))
                     .has_value());
    }

    void BatteryParserKeepsComponentOrderIndependent()
    {
        // Out-of-order components (case, left, right) still land in their own slots.
        auto reversed = ParseBatteryNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03, 0x08, 0x01, 10,   0x02,
                 0x01, 0x04, 0x01, 20,   0x02, 0x01, 0x02, 0x01, 30,   0x02, 0x01}));
        QVERIFY(reversed.has_value());
        QCOMPARE(reversed->left->level, uint8_t(20));
        QCOMPARE(reversed->right->level, uint8_t(30));
        QCOMPARE(reversed->caseBox->level, uint8_t(10));
    }

    void BatteryParserAcceptsEveryOnePercentLevel()
    {
        for (uint8_t level : {uint8_t(0), uint8_t(1), uint8_t(87), uint8_t(99), uint8_t(100)}) {
            auto parsed = ParseBatteryNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, level, 0x02, 0x01}));
            QVERIFY2(parsed.has_value(), QByteArray::number(level).constData());
            QCOMPARE(parsed->left->level, level);
        }
    }

    void BatteryParserMapsDocumentedStatuses()
    {
        auto statusOf = [](uint8_t wire) -> std::optional<BatteryStatus> {
            auto parsed = ParseBatteryNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x08, 0x01, 50, wire, 0x01}));
            return parsed.has_value() ? std::optional<BatteryStatus>(parsed->caseBox->status)
                                      : std::nullopt;
        };
        QCOMPARE(statusOf(0x00), std::optional(BatteryStatus::Unknown));
        QCOMPARE(statusOf(0x01), std::optional(BatteryStatus::Charging));
        QCOMPARE(statusOf(0x02), std::optional(BatteryStatus::Discharging));
        QCOMPARE(statusOf(0x04), std::optional(BatteryStatus::Disconnected));
        // Android's Optimized Charging (0x05) is surfaced as charging.
        QCOMPARE(statusOf(0x05), std::optional(BatteryStatus::Charging));
        // Any other status byte invalidates the whole notification.
        QVERIFY(!statusOf(0x03).has_value());
        QVERIFY(!statusOf(0x06).has_value());
    }

    void BatteryParserRejectsInvalidFrames()
    {
        const auto frame =
            Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 50, 0x02, 0x01});

        // Wrong opcode or header.
        QVERIFY(!ParseBatteryNotification(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x01, 0x04, 0x01, 50, 0x02, 0x01}))
                     .has_value());
        QVERIFY(!ParseBatteryNotification(
                     Hex({0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 50, 0x02, 0x01}))
                     .has_value());
        QVERIFY(!ParseBatteryNotification({}).has_value());
        QVERIFY(!ParseBatteryNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00})).has_value());

        // Illegal component counts: 0 and 4.
        QVERIFY(
            !ParseBatteryNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00})).has_value());
        std::vector<uint8_t> fourCount = Hex(
            {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x04, 0x01, 10, 0x02, 0x01, 0x08, 0x01, 10,
             0x02, 0x01});
        fourCount.resize(27); // count=4 needs 27 bytes; even a longer record must not pass
        QVERIFY(!ParseBatteryNotification(fourCount).has_value());

        // Size mismatch: the record must be exactly 7 + 5 * count.
        std::vector<uint8_t> shortFrame(frame.begin(), frame.begin() + 11);
        QVERIFY(!ParseBatteryNotification(shortFrame).has_value());
        QVERIFY(
            !ParseBatteryNotification(
                 Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 50, 0x02, 0x01, 0x00}))
                 .has_value());
        QVERIFY(!ParseBatteryNotification(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 50,   0x02,
                          0x01, 0x08, 0x01, 10,   0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}))
                     .has_value());

        // Levels above 100 are out of range.
        QVERIFY(!ParseBatteryNotification(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 101, 0x02, 0x01}))
                     .has_value());

        // Unknown component ids and anything unmapped invalidate the whole notification.
        QVERIFY(!ParseBatteryNotification(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x03, 0x01, 50, 0x02, 0x01}))
                     .has_value());

        // A repeated component inside one notification is a violation, not last-wins.
        QVERIFY(!ParseBatteryNotification(Hex(
                                              {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01,
                                               50, 0x02, 0x01, 0x04, 0x01, 60, 0x02, 0x01}))
                     .has_value());
    }

    void BatteryParserRejectsBadSpacerAndEndBytes()
    {
        const auto good = Hex(
            {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 50, 0x02, 0x01, 0x08, 0x01, 10,
             0x02, 0x01});
        QVERIFY(ParseBatteryNotification(good).has_value());

        // Second-position spacer must be 0x01 in every component.
        for (size_t index : {size_t(8), size_t(13)}) {
            auto broken = good;
            broken[index] = 0x00;
            QVERIFY(!ParseBatteryNotification(broken).has_value());
        }
        // Fifth-position end marker must be 0x01 in every component.
        for (size_t index : {size_t(11), size_t(16)}) {
            auto broken = good;
            broken[index] = 0x02;
            QVERIFY(!ParseBatteryNotification(broken).has_value());
        }
    }

    void BatteryParserKeepsDisconnectedComponent()
    {
        // A Disconnected component parses cleanly; falling back to BLE is the fusion's job.
        auto parsed = ParseBatteryNotification(Hex(
            {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0, 0x04, 0x01, 0x08, 0x01, 75,
             0x02, 0x01}));
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->left->status, BatteryStatus::Disconnected);
        QCOMPARE(parsed->caseBox->status, BatteryStatus::Discharging);
    }

    void PersonalizedVolumeParserAcceptsBothStates()
    {
        QCOMPARE(
            ParsePersonalizedVolumeState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01})),
            std::optional(PersonalizedVolumeState::Enabled));
        QCOMPARE(
            ParsePersonalizedVolumeState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x02})),
            std::optional(PersonalizedVolumeState::Disabled));
    }

    // Blueprint 0.1: the parser matches the 0x0A echo (EarDetectionConfig), and the
    // old 0x1B frame must not be mistaken for it.
    void AutomaticEarDetectionParserMatches0x0AOnly()
    {
        QCOMPARE(
            ParseAutomaticEarDetectionState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x01})),
            std::optional(true));
        QCOMPARE(
            ParseAutomaticEarDetectionState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x02})),
            std::optional(false));
        // OneBudANCMode echo (the identifier this feature used to misuse) is rejected.
        QVERIFY(
            !ParseAutomaticEarDetectionState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x1B, 0x01}))
                 .has_value());
    }

    void PpeCapLevelParserReadsLevel()
    {
        QCOMPARE(
            ParsePpeCapLevel(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 82})),
            std::optional<uint8_t>(82));
        QVERIFY(!ParsePpeCapLevel(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 82})).has_value());
    }

    // Kept for the legacy control path: 0x25 is VolumeSwipeMode, and the parser only
    // claims to understand the old loud-sound-reduction echo format.
    void LegacyLoudSoundReductionParserMatches0x25()
    {
        QCOMPARE(
            ParseLoudSoundReductionState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x01})),
            std::optional(LoudSoundReductionState::Enabled));
        QCOMPARE(
            ParseLoudSoundReductionState(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x00})),
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
    // Device information records (opcode 0x001D, blueprint 2.1)
    //

    void DeviceInfoSampleRecordParsesEveryField()
    {
        // Byte-exact 222-byte capture from librepods "AAP Definitions.md"; this
        // is the same packet the offset table in Docs/AAP-Protocol-Spec.md §3 was
        // measured from. The strings are documented as null-terminated UTF-8.
        const auto &sample = DeviceInfoSample();
        QCOMPARE(sample.size(), size_t{222});

        const auto info = ParseDeviceInfo(sample);
        QVERIFY(info.has_value());
        QCOMPARE(info->name, std::string{"AirPods Pro"});
        QCOMPARE(info->model, std::string{"A3048"});
        QCOMPARE(info->manufacturer, std::string{"Apple Inc."});
        QCOMPARE(info->serial, std::string{"QXNRHHYXP6"});
        QCOMPARE(info->firmware1, std::string{"61.1868040002000000.2713"});
        QCOMPARE(info->firmware2, std::string{"61.1868040002000000.2713"});
        QCOMPARE(info->hardwareRevision, std::string{"1.0.0"});
        QCOMPARE(info->leftSerial, std::string{"HY9LT2EF26JY"});
        QCOMPARE(info->rightSerial, std::string{"H3PLWHDJ26K0"});
    }

    void DeviceInfoIgnoresEverythingAfterTheEleventhField()
    {
        // The record continues with a 35-byte opaque blob (0x89 0x31 …) and a
        // trailing numeric string. Random high/low binary bytes behind the 11th
        // separator must not influence the parse.
        const auto &sample = DeviceInfoSample();
        const auto baseline = ParseDeviceInfo(sample);
        QVERIFY(baseline.has_value());

        auto extended = sample;
        extended.insert(extended.end(), {0x00, 0xFF, 0x80, 0x01, 0x7F, 0xC3, 0x28, 'x', 0x00});
        const auto parsed = ParseDeviceInfo(extended);
        QVERIFY(parsed.has_value());
        QCOMPARE(*parsed, *baseline);
    }

    void DeviceInfoRejectsTruncatedRecords()
    {
        const auto &sample = DeviceInfoSample();
        // Every prefix that ends before the eleventh NUL delimiter is truncated,
        // including a packet containing all ten mapped fields but no field 11.
        for (size_t length = 0; length < 176; ++length) {
            const std::vector<uint8_t> prefix(
                sample.begin(), sample.begin() + static_cast<ptrdiff_t>(length));
            QVERIFY2(
                !ParseDeviceInfo(prefix).has_value(),
                qPrintable(QString{"prefix of %1 bytes was accepted"}.arg(length)));
        }

        // The first eleven fields end at byte 175. A record ending here is
        // complete even without the optional trailing encrypted blob.
        const std::vector<uint8_t> fieldsOnly(sample.begin(), sample.begin() + 176);
        const auto info = ParseDeviceInfo(fieldsOnly);
        QVERIFY(info.has_value());
        QCOMPARE(info->rightSerial, std::string{"H3PLWHDJ26K0"});
    }

    void DeviceInfoRejectsWrongHeaderAndOpcode()
    {
        auto packet = DeviceInfoSample();
        packet[4] = 0x1E; // the rename opcode: docs once mislabelled metadata as 0x1E
        QVERIFY(!ParseDeviceInfo(packet).has_value());

        packet = DeviceInfoSample();
        packet[5] = 0x01;
        QVERIFY(!ParseDeviceInfo(packet).has_value());

        packet = DeviceInfoSample();
        packet[0] = 0x12; // ATT-SIGNATURE-shaped frame must never parse as metadata
        QVERIFY(!ParseDeviceInfo(packet).has_value());

        packet = DeviceInfoSample();
        packet[3] = 0x01;
        QVERIFY(!ParseDeviceInfo(packet).has_value());
    }

    void DeviceInfoRejectsBrokenPreambleSkeleton()
    {
        // The five-byte preamble is fixed by the documented 222-byte capture; every
        // offset participates in proving that the string table starts at byte 11.
        static constexpr std::array<uint8_t, 5> preamble{0x02, 0xD5, 0x00, 0x04, 0x00};
        for (size_t i = 0; i < preamble.size(); ++i) {
            auto packet = DeviceInfoSample();
            packet[6 + i] = static_cast<uint8_t>(packet[6 + i] ^ 0x01);
            QVERIFY2(
                !ParseDeviceInfo(packet).has_value(),
                qPrintable(QString{"accepted corrupt preamble byte %1"}.arg(i)));
        }

        // Both NULs are also checked explicitly through the same exact comparison.
        auto packet = DeviceInfoSample();
        packet[8] = 0x01;
        QVERIFY(!ParseDeviceInfo(packet).has_value());
        packet = DeviceInfoSample();
        packet[10] = 0xFF;
        QVERIFY(!ParseDeviceInfo(packet).has_value());
    }

    void DeviceInfoRejectsInvalidUtf8()
    {
        struct {
            const char *what;
            QByteArray field;
        } cases[] = {
            {"truncated two-byte sequence", QByteArray::fromHex("c328")},
            {"stray continuation byte", QByteArray::fromHex("80")},
            {"overlong two-byte form", QByteArray::fromHex("c080")},
            {"overlong three-byte form", QByteArray::fromHex("e08080")},
            {"UTF-16 surrogate half", QByteArray::fromHex("eda080")},
            {"beyond U+10FFFF", QByteArray::fromHex("f4908080")},
            {"C1 control as valid UTF-8", QByteArray::fromHex("c180")},
            {"trailing byte of a cut sequence", QByteArrayLiteral("Pro\xc3")},
        };

        for (const auto &testCase : cases) {
            const auto packet = BuildDeviceInfoPacket(
                {"AirPods Pro" + testCase.field, "A3048", "Apple Inc.", "QXNRHHYXP6", "61.18",
                 "61.18", "1.0.0", "com.apple.accessory.updater.app.71", "HY9LT2EF26JY",
                 "H3PLWHDJ26K0", "6357536"});
            QVERIFY2(
                !ParseDeviceInfo(packet).has_value(),
                qPrintable(QString{"accepted invalid encoding: %1"}.arg(testCase.what)));
        }
    }

    void DeviceInfoRejectsControlCharactersInFields()
    {
        // Control bytes are display-hostile and a strong hint of a misaligned
        // record (the binary blob leaking into a string slot).
        const auto packet = BuildDeviceInfoPacket(
            {QByteArray{"Air\tpods"}, "A3048", "Apple Inc.", "QXNRHHYXP6", "61.18", "61.18",
             "1.0.0", "updater", "HY9LT2EF26JY", "H3PLWHDJ26K0", "6357536"});
        QVERIFY(!ParseDeviceInfo(packet).has_value());

        const auto del = BuildDeviceInfoPacket(
            {QByteArray{"Air\x7Fpods"}, "A3048", "Apple Inc.", "QXNRHHYXP6", "61.18", "61.18",
             "1.0.0", "updater", "HY9LT2EF26JY", "H3PLWHDJ26K0", "6357536"});
        QVERIFY(!ParseDeviceInfo(del).has_value());
    }

    void DeviceInfoRejectsUnicodeDisplayControls()
    {
        // Qt::PlainText blocks HTML, not bidi reordering or zero-width spoofing.
        struct {
            const char *what;
            QByteArray bytes;
        } cases[] = {
            {"right-to-left override", QByteArray::fromHex("e280ae")},
            {"zero-width space", QByteArray::fromHex("e2808b")},
            {"line separator", QByteArray::fromHex("e280a8")},
            {"paragraph separator", QByteArray::fromHex("e280a9")},
            {"soft hyphen", QByteArray::fromHex("c2ad")},
            {"byte-order mark", QByteArray::fromHex("efbbbf")},
        };

        for (const auto &testCase : cases) {
            const auto packet = BuildDeviceInfoPacket(
                {"AirPods Pro" + testCase.bytes, "A3048", "Apple Inc.", "QXNRHHYXP6", "61.18",
                 "61.18", "1.0.0", "updater", "HY9LT2EF26JY", "H3PLWHDJ26K0", "6357536"});
            QVERIFY2(
                !ParseDeviceInfo(packet).has_value(),
                qPrintable(QString{"accepted display control: %1"}.arg(testCase.what)));
        }
    }

    void DeviceInfoAcceptsMultibyteUtf8Names()
    {
        // UTF-8 (not UTF-16) evidence: "AAP Definitions.md" states the strings are
        // null-terminated UTF-8, linux/main.cpp parseMetadata uses
        // QString::fromUtf8, and this app renames through UTF-8 opcode 0x001A.
        // Multi-byte text with embedded 0x00-free UTF-16 would fail here anyway.
        const auto utf8Name = QString::fromUtf8("AirPods Pro 旺来 💤").toUtf8();
        const auto packet = BuildDeviceInfoPacket(
            {utf8Name, "A3048", "Apple Inc.", "QXNRHHYXP6", "61.18", "61.18", "1.0.0", "updater",
             "HY9LT2EF26JY", "H3PLWHDJ26K0", "6357536"});
        const auto info = ParseDeviceInfo(packet);
        QVERIFY(info.has_value());
        QCOMPARE(QString::fromStdString(info->name), QString::fromUtf8("AirPods Pro 旺来 💤"));
    }

    void DeviceInfoAcceptsEmptyFieldValues()
    {
        // An all-NUL record is a device that omits every value: legal, mapped to
        // empty strings; the card then hides those rows.
        const auto packet = BuildDeviceInfoPacket({"", "", "", "", "", "", "", "", "", "", ""});
        const auto info = ParseDeviceInfo(packet);
        QVERIFY(info.has_value());
        QCOMPARE(info->name, std::string{});
        QCOMPARE(info->serial, std::string{});
        QCOMPARE(info->leftSerial, std::string{});
        QCOMPARE(info->rightSerial, std::string{});
    }

    void DeviceInfoMinimumSizeMatchesTransportValidationFloor()
    {
        // AAPMessage::Validate lets any >= 11-byte 0x001D record through; the
        // parser (not the framing layer) is the gate that must reject the rest.
        const auto shortPacket = Hex({0x04, 0x00, 0x04, 0x00, 0x1D, 0x00, 0x02, 0xD5, 0x00, 0x04});
        QCOMPARE(shortPacket.size(), size_t{10});
        QVERIFY(!ParseDeviceInfo(shortPacket).has_value());
    }

    //////////////////////////////////////////////////
    // Notification classifiers
    //

    void ClassifierMatchesDocumentedFrames()
    {
        QVERIFY(IsNoiseControlNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02})));
        QVERIFY(IsConversationalAwarenessNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01})));
        QVERIFY(IsSpeakingLevelNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01, 0x08})));
        QVERIFY(IsEarDetectionNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00})));
        QVERIFY(IsBatteryNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01})));
        QVERIFY(IsPersonalizedVolumeNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01})));
        QVERIFY(IsAutomaticEarDetectionNotification(
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0A, 0x01})));
        QVERIFY(IsPpeCapLevelNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 0x19})));
        QVERIFY(!IsPpeCapLevelNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 0x19})));
        QVERIFY(IsSettingsNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 0x10})));
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
        QVERIFY(!IsAutomaticEarDetectionNotification(Packets::BuildLoudSoundReductionPacket(true)));
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
        QVERIFY(!ParseSpeakingLevel(Hex({0x04, 0x00, 0x04, 0x00, 0x4B, 0x00, 0x02, 0x00, 0x01}))
                     .has_value());
        QVERIFY(!ParsePpeCapLevel(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38})).has_value());
        QVERIFY(!IsPpeCapLevelNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09})));
    }

    //////////////////////////////////////////////////
    // ATT frames (blueprint 0.3)
    //

    void AttWriteCommandUsesLittleEndianHandle()
    {
        QCOMPARE(Att::BuildWriteCommand(0x1B, 0x01), Hex({0x12, 0x1B, 0x00, 0x01}));
        QCOMPARE(Att::BuildWriteCommand(0x0102, 0xAA), Hex({0x12, 0x02, 0x01, 0xAA}));
        QCOMPARE(Att::BuildLoudSoundReductionWrite(true), Hex({0x12, 0x1B, 0x00, 0x01}));
        QCOMPARE(Att::BuildLoudSoundReductionWrite(false), Hex({0x12, 0x1B, 0x00, 0x00}));
    }

    void AttReadRequestCoversHandle()
    {
        QCOMPARE(Att::BuildReadRequest(Att::kLoudSoundReductionHandle), Hex({0x0A, 0x1B, 0x00}));
    }

    void AttReadResponseParserStripsOpcode()
    {
        QVERIFY(Att::IsReadResponse(Hex({0x0B, 0x01})));
        auto value = Att::ParseReadResponse(Hex({0x0B, 0x01}));
        QVERIFY(value.has_value());
        QCOMPARE(*value, Hex({0x01}));

        QVERIFY(!Att::IsReadResponse(Hex({0x0B})));
        QVERIFY(!Att::ParseReadResponse(Hex({0x12, 0x1B, 0x00, 0x01})).has_value());
    }

    void AttNotificationCarriesHandle()
    {
        auto notification = Att::ParseNotification(Hex({0x1B, 0x1B, 0x00, 0x00}));
        QVERIFY(notification.has_value());
        QCOMPARE(notification->first, uint16_t(0x1B));
        QCOMPARE(notification->second, Hex({0x00}));

        QVERIFY(!Att::IsNotification(Hex({0x1B})));
        QVERIFY(!Att::ParseNotification(Hex({0x0A, 0x1B, 0x00})).has_value());
    }

    void AttErrorResponseIsRecognized()
    {
        // opcode 0x01, requested opcode 0x0A, handle 0x001B, error 0x0A (attr not found)
        QVERIFY(Att::IsErrorResponse(Hex({0x01, 0x0A, 0x1B, 0x00, 0x0A})));
        QVERIFY(!Att::IsErrorResponse(Hex({0x01, 0x0A})));
    }

    // L2CAP AAP frames must never be classified as ATT, and vice versa — this
    // is the routing rule that keeps ATT responses out of the AAP parsers.
    void AttFrameRoutingSeparatesL2capAndAtt()
    {
        QVERIFY(!Att::IsAttFrame(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02})));
        QVERIFY(!Att::IsAttFrame(Packets::Handshake));
        QVERIFY(!Att::IsAttFrame(Packets::BuildNoiseControlPacket(NoiseControlMode::Off)));
        QVERIFY(!Att::IsAttFrame(Packets::BuildLoudSoundReductionPacket(true)));

        QVERIFY(Att::IsAttFrame(Hex({0x0B, 0x01})));
        QVERIFY(Att::IsAttFrame(Hex({0x1B, 0x1B, 0x00, 0x00})));
        QVERIFY(Att::IsAttFrame(Hex({0x01, 0x0A, 0x1B, 0x00, 0x0A})));
        QVERIFY(!Att::IsAttFrame({}));
    }

    //////////////////////////////////////////////////
    // Connection diagnostics (blueprint 1.4)
    //

    void DiagnosticsStepNamesAreStable()
    {
        // The exported report is matched against these names in bug reports;
        // renaming one silently breaks every past report.
        using Core::Diagnostics::Step;
        QCOMPARE(
            QString{Core::Diagnostics::StepName(Step::ScannerStarted)},
            QStringLiteral("scanner_started"));
        QCOMPARE(
            QString{Core::Diagnostics::StepName(Step::TransportConnect)},
            QStringLiteral("transport_connect"));
        QCOMPARE(
            QString{Core::Diagnostics::StepName(Step::Handshake)}, QStringLiteral("handshake"));
        QCOMPARE(QString{Core::Diagnostics::StepName(Step::AttProbe)}, QStringLiteral("att_probe"));
        QCOMPARE(
            QString{Core::Diagnostics::StepName(Step::Disconnect)}, QStringLiteral("disconnect"));
    }

    void DiagnosticsMasksDeviceAddress()
    {
        using Core::Diagnostics::Recorder;
        const auto masked = Recorder::MaskAddress(0x1122334455667788ULL);
        // Last byte kept for distinguishability, everything else hidden.
        QVERIFY(masked.find("88") != std::string::npos);
        QVERIFY(masked.find("1122") == std::string::npos);
        QVERIFY(masked.find("77665544332211") == std::string::npos);
    }

    void DiagnosticsRecorderBuildsReport()
    {
        using Core::Diagnostics::Recorder;
        using Core::Diagnostics::Step;

        auto &recorder = Recorder::Instance();
        recorder.Reset();
        QCOMPARE(recorder.StepCount(), size_t{0});

        recorder.Record(Step::Handshake, true);
        recorder.Record(Step::AttProbe, false, "no ATT answer within 500ms");
        QCOMPARE(recorder.StepCount(), size_t{2});

        const auto report = recorder.BuildReport("header-line");
        const auto asString = QString::fromStdString(report);
        QVERIFY(asString.contains("header-line"));
        QVERIFY(asString.contains("handshake"));
        QVERIFY(asString.contains("att_probe"));
        QVERIFY(asString.contains("FAILED"));
        QVERIFY(asString.contains("no ATT answer within 500ms"));

        recorder.Reset();
        QCOMPARE(recorder.StepCount(), size_t{0});
    }
};

QTEST_GUILESS_MAIN(AAPProtocolTests)

#include "AAPProtocolTests.moc"
