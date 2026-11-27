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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

#include <QtTest>

#include "AAPTestSupport.h"
#include "Source/Core/AAP.h"
#include "Source/Core/AAPAtt.h"
#include "Source/Core/AAPBattery.h"
#include "Source/Core/AAPStem.h"
#include "Source/Core/AdaptiveNoiseLevel.h"
#include "Source/Core/AAPDeviceInfo.h"
#include "Source/Core/ConnectionDiagnostics.h"
#include "Source/Core/GlobalMedia_abstract.h"

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

class OutcomeQueue
{
public:
    void Push(Core::AAP::AAPCommandOutcome outcome)
    {
        {
            std::lock_guard lock{_mutex};
            _outcomes.push_back(std::move(outcome));
        }
        _condition.notify_all();
    }

    std::optional<Core::AAP::AAPCommandOutcome> Wait()
    {
        std::unique_lock lock{_mutex};
        if (!_condition.wait_for(
                lock, std::chrono::seconds{1}, [this] { return !_outcomes.empty(); }))
        {
            return std::nullopt;
        }
        auto outcome = _outcomes.front();
        _outcomes.erase(_outcomes.begin());
        return outcome;
    }

    size_t Size() const
    {
        std::lock_guard lock{_mutex};
        return _outcomes.size();
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    std::vector<Core::AAP::AAPCommandOutcome> _outcomes;
};

class ManualGate
{
public:
    void Open()
    {
        std::lock_guard lock{_mutex};
        _open = true;
        _condition.notify_all();
    }

    void Wait(std::stop_token stop)
    {
        std::unique_lock lock{_mutex};
        _condition.wait(lock, stop, [this] { return _open; });
    }

private:
    std::mutex _mutex;
    std::condition_variable_any _condition;
    bool _open{false};
};

class DeadlineSignal
{
public:
    explicit DeadlineSignal(bool armed = true) : _armed{armed} {}

    std::chrono::steady_clock::time_point Read(AAPTestSupport::ManualClock &clock)
    {
        const auto now = clock.Now();
        if (_armed.load()) {
            std::call_once(_signalOnce, [this] { _promise.set_value(); });
        }
        return now;
    }

    bool Wait(std::chrono::seconds timeout = std::chrono::seconds{1})
    {
        return _future.wait_for(timeout) == std::future_status::ready;
    }

    void Arm()
    {
        _armed.store(true);
    }

private:
    std::atomic<bool> _armed;
    std::once_flag _signalOnce;
    std::promise<void> _promise;
    std::future<void> _future{_promise.get_future()};
};

Core::AAP::AAPCommandBroker::Config UnverifiableConfig(AAPTestSupport::ManualClock &clock)
{
    Core::AAP::AAPCommandBroker::Config config;
    config.clock.now = [&clock] { return clock.Now(); };
    config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
    for (auto &mode : config.overrides) {
        mode = Core::AAP::AAPConfirmMode::Unverifiable;
    }
    return config;
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
    static const std::vector<uint8_t> sample =
        FromHex(QByteArrayLiteral("040004001d0002d5000400416972506f64732050726f0041333034380041"
                                  "70706c6520496e632e0051584e52"
                                  "4848595850360036312e313836383034303030323030303030302e323731"
                                  "330036312e313836383034303030"
                                  "323030303030302e3237313300312e302e3000636f6d2e6170706c652e61"
                                  "63636573736f72792e7570646174"
                                  "65722e6170702e3731004859394c5432454632364a59004833504c574844"
                                  "4a32364b3000363335373533360089"
                                  "312a6567a5400f84a3ca234947efd40b90d78436ae5946748d70273e6606"
                                  "6a258930003533393530363036340"
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
        // D3 keeps this wire untouched: control command 0x2E (AutoAncStrength). 0x39 is
        // StemConfig (D4 territory) and must never appear as the adaptive-strength id.
        QCOMPARE(
            Packets::BuildAdaptiveNoisePacket(100),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100, 0x00, 0x00, 0x00}));
        QCOMPARE(
            Packets::BuildAdaptiveNoisePacket(0),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 0, 0x00, 0x00, 0x00}));
        QCOMPARE(
            Packets::BuildAdaptiveNoisePacket(50),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 50, 0x00, 0x00, 0x00}));
        // Raw input above 100 is normalized by the shared policy before it reaches the
        // builder, yielding the 100 frame rather than an out-of-range byte.
        QCOMPARE(
            Packets::BuildAdaptiveNoisePacket(Core::AAP::NormalizeAdaptiveNoiseLevel(255)),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100, 0x00, 0x00, 0x00}));
        const auto packet = Packets::BuildAdaptiveNoisePacket(50);
        QVERIFY(packet.size() >= 7);
        QVERIFY(packet[6] != 0x39);

        // The UI is filtering strength, so the explicit mapping is identity at every endpoint
        // and midpoint: UI 0/50/100 -> wire 0/50/100 (0=more ambient noise, 100=more filtering).
        for (const uint8_t level : {uint8_t{0}, uint8_t{50}, uint8_t{100}}) {
            QCOMPARE(Core::AAP::FilteringStrengthToWire(level), level);
            QCOMPARE(Core::AAP::FilteringStrengthFromWire(level), level);
            QCOMPARE(
                Packets::BuildAdaptiveNoisePacket(Core::AAP::FilteringStrengthToWire(level))[7],
                level);
        }
        QCOMPARE(Core::AAP::FilteringStrengthToWire(255), uint8_t{100});
        QCOMPARE(Core::AAP::FilteringStrengthFromWire(255), uint8_t{100});
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

    void StemConfigMaskAndPacketFollowRawGestureBits()
    {
        StemActionTable actions{};
        QCOMPARE(StemConfigMask(actions), uint8_t{0});
        QCOMPARE(
            Packets::BuildStemConfigPacket(0),
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x39, 0x00, 0x00, 0x00, 0x00}));

        constexpr std::array<uint8_t, 4> bits{0x01, 0x02, 0x04, 0x08};
        for (size_t press = 0; press < bits.size(); ++press) {
            actions = {};
            actions[press] = StemAction::PlayPause;
            QCOMPARE(StemConfigMask(actions), bits[press]);
            actions = {};
            actions[4 + press] = StemAction::VolumeUp;
            QCOMPARE(StemConfigMask(actions), bits[press]);
            QCOMPARE(
                Packets::BuildStemConfigPacket(bits[press]),
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x39, bits[press], 0x00, 0x00, 0x00}));
        }
        actions = {};
        actions[0] = static_cast<StemAction>(0xFF);
        QCOMPARE(StemConfigMask(actions), uint8_t{0});
        QCOMPARE(StemConfigMask(DefaultStemActions()), uint8_t{0x0F});
        QCOMPARE(Packets::BuildStemConfigPacket(0xFF).at(7), uint8_t{0x0F});
    }

    void StemParserCoversEverySideAndPress()
    {
        constexpr StemSide sides[]{StemSide::Left, StemSide::Right};
        constexpr StemPress presses[]{
            StemPress::Single, StemPress::Double, StemPress::Triple, StemPress::Long};
        constexpr uint8_t pressBytes[]{0x05, 0x06, 0x07, 0x08};
        constexpr uint8_t sideBytes[]{0x01, 0x02};

        for (size_t sideIndex = 0; sideIndex < std::size(sides); ++sideIndex) {
            for (size_t pressIndex = 0; pressIndex < std::size(presses); ++pressIndex) {
                const auto result = ParseStemEvent(Hex(
                    {0x04, 0x00, 0x04, 0x00, 0x19, 0x00, pressBytes[pressIndex],
                     sideBytes[sideIndex]}));
                QVERIFY(result.has_value());
                QCOMPARE(result->side, sides[sideIndex]);
                QCOMPARE(result->press, presses[pressIndex]);
            }
        }
    }

    void StemParserRejectsMalformedRecords()
    {
        const auto valid = Hex({0x04, 0x00, 0x04, 0x00, 0x19, 0x00, 0x05, 0x01});
        for (size_t length = 0; length < valid.size(); ++length) {
            QVERIFY(!ParseStemEvent(std::span{valid}.first(length)).has_value());
        }
        auto overlong = valid;
        overlong.push_back(0x00);
        QVERIFY(!ParseStemEvent(overlong).has_value());

        for (size_t index = 0; index < 6; ++index) {
            auto badPrefix = valid;
            badPrefix[index] ^= 0xFF;
            QVERIFY(!ParseStemEvent(badPrefix).has_value());
        }
        for (unsigned value = 0; value <= 0xFF; ++value) {
            auto badField = valid;
            badField[6] = static_cast<uint8_t>(value);
            const bool validPress = value >= 0x05 && value <= 0x08;
            QCOMPARE(ParseStemEvent(badField).has_value(), validPress);
            badField = valid;
            badField[7] = static_cast<uint8_t>(value);
            QCOMPARE(ParseStemEvent(badField).has_value(), value == 0x01 || value == 0x02);
        }
    }

    void StemActionTableMapsAllEightSlots()
    {
        constexpr StemActionTable actions{StemAction::PlayPause,     StemAction::NextTrack,
                                          StemAction::PreviousTrack, StemAction::VolumeUp,
                                          StemAction::VolumeDown,    StemAction::ToggleNoiseControl,
                                          StemAction::None,          StemAction::PlayPause};
        constexpr StemSide sides[]{StemSide::Left, StemSide::Right};
        constexpr StemPress presses[]{
            StemPress::Single, StemPress::Double, StemPress::Triple, StemPress::Long};
        for (size_t sideIndex = 0; sideIndex < std::size(sides); ++sideIndex) {
            for (size_t pressIndex = 0; pressIndex < std::size(presses); ++pressIndex) {
                QCOMPARE(
                    SelectStemAction(actions, {sides[sideIndex], presses[pressIndex]}),
                    actions[sideIndex * 4 + pressIndex]);
            }
        }
        QVERIFY(!StemActionIndex(static_cast<StemSide>(0xFF), StemPress::Single).has_value());
        QCOMPARE(
            SelectStemAction(actions, {static_cast<StemSide>(0xFF), StemPress::Single}),
            StemAction::None);
    }

    void QueuedStemActionRejectsStaleAttempts()
    {
        constexpr StemActionRequest request{static_cast<int>(StemAction::PlayPause), 0x1234, 7};
        QVERIFY(IsCurrentStemActionRequest(request, 0x1234, 7, true));
        QVERIFY(!IsCurrentStemActionRequest(request, 0x1234, 8, true));
        QVERIFY(!IsCurrentStemActionRequest(request, 0x5678, 7, true));
        QVERIFY(!IsCurrentStemActionRequest(request, 0x1234, 7, false));
        QVERIFY(!IsCurrentStemActionRequest({}, 0, 0, true));
    }

    void StemVolumeUsesFloatReadsAndEndsOldAttenuationSession()
    {
        using Core::GlobalMedia::Details::CalculateStemVolume;
        using Core::GlobalMedia::Details::VolumeReductionPolicy;
        QCOMPARE(CalculateStemVolume(29.6F, 1, 5), std::optional<float>{35.0F});
        QCOMPARE(CalculateStemVolume(29.6F, -1, 5), std::optional<float>{25.0F});
        QCOMPARE(CalculateStemVolume(99.6F, 1, 5), std::optional<float>{100.0F});
        QVERIFY(!CalculateStemVolume(std::nullopt, 1, 5).has_value());

        VolumeReductionPolicy policy;
        QVERIFY(policy.Begin(63.6F));
        QCOMPARE(policy.ReductionTarget(40), 25.0F);
        QCOMPARE(policy.RestorationTarget(), std::optional<float>{64.0F});
        policy.End();
        QVERIFY(!policy.RestorationTarget().has_value());
        QVERIFY(policy.Begin(35.0F)); // A new session captures the manually adjusted baseline.
        QCOMPARE(policy.ReductionTarget(40), 14.0F);
        QVERIFY(!VolumeReductionPolicy{}.Begin(std::nullopt));
    }

    void StemDeduperCollapsesRepeatedLongPressBurstsAndReleaseTails()
    {
        using namespace std::chrono_literals;
        using TimePointT = std::chrono::steady_clock::time_point;
        auto now = TimePointT{};
        StemPressDeduper deduper([&] { return now; });
        const StemEvent leftLong{StemSide::Left, StemPress::Long};
        const StemEvent leftSingle{StemSide::Left, StemPress::Single};

        QVERIFY(deduper.Accept(leftLong));
        now += 100ms;
        QVERIFY(!deduper.Accept(leftLong));
        now += 100ms;
        QVERIFY(!deduper.Accept(leftLong));
        now += 151ms; // A real gap ends the continuous press.
        QVERIFY(deduper.Accept(leftLong));
        now += 100ms;
        QVERIFY(!deduper.Accept(leftSingle)); // Release tail of the second long press.
        now += 401ms;
        QVERIFY(deduper.Accept(leftSingle));
        QVERIFY(deduper.Accept({StemSide::Right, StemPress::Long}));
        QCOMPARE(deduper.SuppressedCount(), size_t{3});

        deduper.Reset();
        QVERIFY(deduper.Accept(leftLong));
        QCOMPARE(deduper.SuppressedCount(), size_t{0});
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

    void AdaptiveNoiseLevelParserAcceptsOnlyValid0x2EEchoes()
    {
        QCOMPARE(
            ParseAdaptiveNoiseLevelNotification(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 0})),
            std::optional<uint8_t>{0});
        QCOMPARE(
            ParseAdaptiveNoiseLevelNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 50})),
            std::optional<uint8_t>{50});
        QCOMPARE(
            ParseAdaptiveNoiseLevelNotification(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 100})),
            std::optional<uint8_t>{100});
        QVERIFY(!ParseAdaptiveNoiseLevelNotification(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x2E, 101}))
                     .has_value());
        QVERIFY(!ParseAdaptiveNoiseLevelNotification(
                     Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x38, 50}))
                     .has_value());
        QVERIFY(!ParseAdaptiveNoiseLevelNotification({}).has_value());
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

    // P3: control identifier 0x25 is VolumeSwipeMode. It is recognised under that name and
    // can never produce a LoudSoundReductionState — loud sound reduction only exists on ATT
    // handle 0x1B. (The retired loud-sound-reduction control parser read this frame as LSR.)
    void VolumeSwipeIs0x25AndIsNotLoudSoundReduction()
    {
        const auto enabled =
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x01, 0x00, 0x00, 0x00});
        const auto disabled =
            Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x02, 0x00, 0x00, 0x00});

        QVERIFY(IsVolumeSwipeNotification(enabled));
        QCOMPARE(ParseVolumeSwipeMode(enabled), std::optional(VolumeSwipeMode::Enabled));
        QCOMPARE(ParseVolumeSwipeMode(disabled), std::optional(VolumeSwipeMode::Disabled));
        // The 0x00 used by the loud-sound-reduction ATT value is not a volume swipe state.
        QCOMPARE(
            ParseVolumeSwipeMode(
                Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x25, 0x00, 0x00, 0x00, 0x00})),
            std::optional(VolumeSwipeMode::Unknown));
        QVERIFY(!ParseVolumeSwipeMode(Hex({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x26, 0x01}))
                     .has_value());

        // No LSR value can come out of the AAP control path: the ATT write is not a
        // settings notification, and the 0x25 frame reaches no LSR parser.
        QVERIFY(!IsSettingsNotification(Packets::BuildLoudSoundReductionPacket(true)));
        QVERIFY(!IsVolumeSwipeNotification(Packets::BuildLoudSoundReductionPacket(true)));
    }

    void HeadTrackingParserReadsLittleEndianOffsets()
    {
        std::vector<uint8_t> frame(70, 0x00);
        constexpr std::array<uint8_t, 10> prefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                                 0x00, 0x00, 0x00, 0x10, 0x00};
        std::copy(prefix.begin(), prefix.end(), frame.begin());
        frame[10] = 0x44;
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

        frame.resize(69);
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
    // Command broker (blueprint 2.7) — all timing is injected, never slept.
    //

    void NoEchoRollsBackToKnownDeviceValue()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        DeadlineSignal deadline;
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        AAPCommandBroker broker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        broker.OnEcho({AAPCommandKind::SetNoiseControl, 0x01}, 1);
        QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x02}).accepted);
        QVERIFY(deadline.Wait());

        broker.AdvanceTimeForTesting(std::chrono::milliseconds{1999});
        QCOMPARE(outcomes.Size(), size_t{0});
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{1});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::TimedOut);
        QCOMPARE(outcome->failure, std::optional(AAPCommandFailure::Timeout));
        QVERIFY(outcome->rollBackDesired);
        QCOMPARE(outcome->rollbackValue, std::optional<uint32_t>{0x01});
        QCOMPARE(broker.Confirmed(AAPCommandKind::SetNoiseControl), std::optional<uint32_t>{0x01});
    }

    void TimeoutRestoresDesiredAndCarriesRequestIdentity()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        DeadlineSignal deadline;
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        AAPCommandBroker confirmedBroker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        confirmedBroker.BeginSession(1);
        confirmedBroker.OnEcho({AAPCommandKind::SetPpeCapLevel, 25}, 1);
        const auto result = confirmedBroker.Submit({AAPCommandKind::SetPpeCapLevel, 75});
        QVERIFY(result.accepted);
        QVERIFY(result.requestId != 0);
        QCOMPARE(confirmedBroker.LatestRequest(AAPCommandKind::SetPpeCapLevel), result.requestId);
        QVERIFY(deadline.Wait());
        confirmedBroker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->requestId, result.requestId);
        QCOMPARE(outcome->rollbackValue, std::optional<uint32_t>{25});
        QCOMPARE(
            confirmedBroker.Desired(AAPCommandKind::SetPpeCapLevel), std::optional<uint32_t>{25});
        QVERIFY(confirmedBroker.IsCurrentRequest(AAPCommandKind::SetPpeCapLevel, result.requestId));
    }

    void FirstSessionTimeoutHasNoInventedBaseline()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        DeadlineSignal deadline;
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        AAPCommandBroker broker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x02}).accepted);
        QVERIFY(deadline.Wait());
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QVERIFY(outcome->rollBackDesired);
        QVERIFY(!outcome->rollbackValue.has_value());
        QVERIFY(!broker.Desired(AAPCommandKind::SetNoiseControl).has_value());
    }

    void ConfirmedBaselineIsScopedToSessionAndEarlyEvidenceIsAccepted()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker broker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; }, {},
            UnverifiableConfig(clock));
        broker.OnEcho({AAPCommandKind::SetPpeCapLevel, 33}, 7);
        broker.BeginSession(7);
        QCOMPARE(broker.Confirmed(AAPCommandKind::SetPpeCapLevel), std::optional<uint32_t>{33});
        broker.EndSession();
        QVERIFY(!broker.Confirmed(AAPCommandKind::SetPpeCapLevel).has_value());
        broker.OnEcho({AAPCommandKind::SetPpeCapLevel, 99}, 7);
        broker.BeginSession(8);
        QVERIFY(!broker.Confirmed(AAPCommandKind::SetPpeCapLevel).has_value());
    }

    void CapabilityWithdrawalSettlesActiveFlight()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        AAPCommandBroker broker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetLoudSoundReduction, 0x01}).accepted);
        broker.SetConfirmMode(AAPCommandKind::SetLoudSoundReduction, AAPConfirmMode::Unverifiable);
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::AppliedUnconfirmed);
        QVERIFY(!outcome->rollBackDesired);
    }

    void CancellationNotificationCannotBeLost()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        AAPTestSupport::PromiseGate predicateRelease;
        AAPTestSupport::ScopedUnblock releaseGuard{predicateRelease};
        std::promise<void> predicateEntered;
        auto predicateFuture = predicateEntered.get_future();
        int clockCalls = 0;
        AAPCommandBroker::Config config;
        config.clock.now = [&] {
            if (++clockCalls == 2) {
                predicateEntered.set_value();
                predicateRelease.Wait();
            }
            return clock.Now();
        };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        AAPCommandBroker broker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        std::stop_source stop;
        QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x02}, stop.get_token()).accepted);
        QVERIFY(predicateFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        predicateRelease.Signal();
        stop.request_stop();
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::Rejected);
    }

    void ManualClockWakeupCannotBeLost()
    {
        using namespace Core::AAP;
        for (int iteration = 0; iteration < 100; ++iteration) {
            AAPTestSupport::ManualClock clock;
            OutcomeQueue outcomes;
            std::promise<void> deadlineStarted;
            auto deadlineFuture = deadlineStarted.get_future();
            bool deadlineObserved = false;
            AAPCommandBroker::Config config;
            config.clock.now = [&] {
                const auto now = clock.Now();
                if (!deadlineObserved) {
                    deadlineObserved = true;
                    deadlineStarted.set_value();
                }
                return now;
            };
            config.clock.advance = [&clock](std::chrono::milliseconds delta) {
                clock.Advance(delta);
            };
            AAPCommandBroker broker(
                [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
                [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
            broker.BeginSession(1);
            QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x02}).accepted);
            QVERIFY(deadlineFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
            broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
            QVERIFY(outcomes.Wait().has_value());
        }
    }

    void AdaptiveNoiseNoEchoRollsBackAndKeepsDesiredForReplay()
    {
        using namespace Core::AAP;
        QCOMPARE(
            AAPCommandBroker::ModeFor(AAPCommandKind::SetAdaptiveNoiseLevel, 50),
            AAPConfirmMode::Echo);
        AAPTestSupport::ManualClock clock;
        DeadlineSignal deadline;
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        AAPCommandBroker broker(
            [](const AAPCommand &, uint64_t, std::stop_token) { return true; },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        broker.OnEcho({AAPCommandKind::SetAdaptiveNoiseLevel, 25}, 1);
        QVERIFY(broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 80}).accepted);
        QVERIFY(deadline.Wait());
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::TimedOut);
        QVERIFY(outcome->rollBackDesired);
        QCOMPARE(outcome->rollbackValue, std::optional<uint32_t>{25});
        QCOMPARE(
            broker.Desired(AAPCommandKind::SetAdaptiveNoiseLevel), std::optional<uint32_t>{80});
    }

    void AdaptiveNoiseConfirmedBaselineDoesNotSurviveSessionClose()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        DeadlineSignal deadline;
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        int calls = 0;
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token) {
                if (++calls == 1) {
                    liveBroker->OnEcho(command, generation);
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        liveBroker = &broker;
        const AAPCommand confirmed{AAPCommandKind::SetAdaptiveNoiseLevel, 25};
        const AAPCommand replayed{AAPCommandKind::SetAdaptiveNoiseLevel, 70};
        broker.BeginSession(1);
        QVERIFY(broker.Submit(confirmed).accepted);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);
        QCOMPARE(broker.Confirmed(confirmed.kind), std::optional<uint32_t>{25});

        // RecordDesired is bookkeeping only: it never reports an applied value.
        QVERIFY(!broker.RecordDesired(replayed).accepted);
        QCOMPARE(
            broker.Desired(AAPCommandKind::SetAdaptiveNoiseLevel), std::optional<uint32_t>{70});
        broker.EndSession();
        QVERIFY(!broker.Confirmed(confirmed.kind).has_value());
        broker.BeginSession(2);
        broker.ReplayDesiredState(2);
        QVERIFY(deadline.Wait());
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->generation, uint64_t{2});
        QCOMPARE(outcome->settlement, AAPCommandSettlement::TimedOut);
        QVERIFY(outcome->rollBackDesired);
        QVERIFY(!outcome->rollbackValue.has_value());
        QVERIFY(!broker.Confirmed(replayed.kind).has_value());
    }

    void DuplicateInFlightSubmissionDoesNotSupersedeOrWriteAgain()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        ManualGate gate;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::atomic<size_t> sends{0};
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token stop) {
                if (++sends == 1) {
                    started.set_value();
                    gate.Wait(stop);
                }
                liveBroker->OnEcho(command, generation);
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        liveBroker = &broker;
        broker.BeginSession(1);
        const AAPCommand command{AAPCommandKind::SetAdaptiveNoiseLevel, 70};
        const auto first = broker.Submit(command);
        QVERIFY(first.accepted);
        QVERIFY(startedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto duplicate = broker.Submit(command);
        QVERIFY(duplicate.accepted);
        QCOMPARE(duplicate.requestId, first.requestId);
        QCOMPARE(broker.LatestRequest(command.kind), first.requestId);
        QCOMPARE(broker.QueuedCount(), size_t{0});
        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        QCOMPARE(sends.load(), size_t{1});
        QCOMPARE(outcomes.Size(), size_t{1});
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);

        // The manager cache update follows the broker observer. A notification in that narrow
        // window still sees the same desired/confirmed value and must not submit another write.
        const auto confirmedDuplicate = broker.Submit(command);
        QVERIFY(confirmedDuplicate.accepted);
        QCOMPARE(confirmedDuplicate.requestId, first.requestId);
        QCOMPARE(broker.QueuedCount(), size_t{0});
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        QCOMPARE(sends.load(), size_t{1});
    }

    void DuplicateInFlightTimeoutRetainsOriginalRequestIdentity()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        std::promise<void> deadlineStarted;
        auto deadlineFuture = deadlineStarted.get_future();
        bool deadlineObserved = false;
        AAPCommandBroker::Config config;
        config.clock.now = [&] {
            const auto now = clock.Now();
            if (!deadlineObserved) {
                deadlineObserved = true;
                deadlineStarted.set_value();
            }
            return now;
        };
        config.clock.advance = [&](std::chrono::milliseconds delta) { clock.Advance(delta); };
        std::atomic<size_t> sends{0};
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token) {
                ++sends;
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        const AAPCommand command{AAPCommandKind::SetAdaptiveNoiseLevel, 70};
        const auto first = broker.Submit(command);
        QVERIFY(first.accepted);
        const auto duplicate = broker.Submit(command);
        QVERIFY(duplicate.accepted);
        QCOMPARE(duplicate.requestId, first.requestId);
        QCOMPARE(broker.LatestRequest(command.kind), first.requestId);
        QVERIFY(deadlineFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->requestId, first.requestId);
        QCOMPARE(outcome->settlement, AAPCommandSettlement::TimedOut);
        QCOMPARE(broker.Desired(command.kind), std::optional<uint32_t>{70});
        QCOMPARE(sends.load(), size_t{1});
    }

    void DuplicateAfterRollbackRetainsRollbackIdentity()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        std::promise<void> deadlineStarted;
        auto deadlineFuture = deadlineStarted.get_future();
        bool deadlineObserved = false;
        AAPCommandBroker::Config config;
        config.clock.now = [&] {
            const auto now = clock.Now();
            if (!deadlineObserved) {
                deadlineObserved = true;
                deadlineStarted.set_value();
            }
            return now;
        };
        config.clock.advance = [&](std::chrono::milliseconds delta) { clock.Advance(delta); };
        int calls = 0;
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token) {
                if (++calls == 1) {
                    liveBroker->OnEcho(command, generation);
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        liveBroker = &broker;
        broker.BeginSession(1);
        const AAPCommand baseline{AAPCommandKind::SetNoiseControl, 0x01};
        const auto first = broker.Submit(baseline);
        QVERIFY(first.accepted);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);

        const auto failed = broker.Submit({AAPCommandKind::SetNoiseControl, 0x02});
        QVERIFY(failed.accepted);
        QVERIFY(deadlineFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::TimedOut);
        QCOMPARE(broker.Desired(baseline.kind), std::optional<uint32_t>{baseline.value});

        const auto duplicate = broker.Submit(baseline);
        QVERIFY(duplicate.accepted);
        QCOMPARE(duplicate.requestId, failed.requestId);
        QCOMPARE(broker.QueuedCount(), size_t{0});
        QCOMPARE(calls, 2);
    }

    void CancelledAdaptiveRequestRestoresConfirmedReplayIntent()
    {
        using namespace Core::AAP;
        OutcomeQueue outcomes;
        std::promise<void> cancelledStarted;
        auto cancelledFuture = cancelledStarted.get_future();
        int calls = 0;
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token) {
                if (++calls == 1) {
                    liveBroker->OnEcho(command, generation);
                }
                else {
                    cancelledStarted.set_value();
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); });
        liveBroker = &broker;
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 25}).accepted);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);

        std::stop_source stop;
        QVERIFY(
            broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 80}, stop.get_token()).accepted);
        QVERIFY(cancelledFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        stop.request_stop();
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::Rejected);
        QVERIFY(outcome->rollBackDesired);
        QCOMPARE(outcome->rollbackValue, std::optional<uint32_t>{25});
        QCOMPARE(
            broker.Desired(AAPCommandKind::SetAdaptiveNoiseLevel), std::optional<uint32_t>{25});
    }

    void DuplicateInFlightCallerCanCancelSharedFlight()
    {
        using namespace Core::AAP;
        OutcomeQueue outcomes;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::atomic<size_t> sends{0};
        std::stop_source firstStop;
        std::stop_source duplicateStop;
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token) {
                if (++sends == 1) {
                    started.set_value();
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); });
        broker.BeginSession(1);
        const auto first =
            broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 70}, firstStop.get_token());
        QVERIFY(first.accepted);
        QVERIFY(startedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto duplicate =
            broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 70}, duplicateStop.get_token());
        QVERIFY(duplicate.accepted);
        QCOMPARE(duplicate.requestId, first.requestId);
        duplicateStop.request_stop();
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::Rejected);
        QCOMPARE(sends.load(), size_t{1});
    }

    void OfflineSameValueRetiresInFlightCommand()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        ManualGate gate;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token stop) {
                started.set_value();
                gate.Wait(stop);
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); },
            UnverifiableConfig(clock));
        const AAPCommand command{AAPCommandKind::SetHeadTracking, 0x01};
        broker.BeginSession(1);
        QVERIFY(broker.Submit(command).accepted);
        QVERIFY(startedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        const auto offline = broker.RecordDesired(command);
        gate.Open();
        QVERIFY(!offline.accepted);
        QCOMPARE(offline.failure, std::optional(AAPCommandFailure::TransportClosed));
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::Superseded);
        QCOMPARE(broker.Desired(command.kind), std::optional<uint32_t>{command.value});
    }

    void AdaptiveNoiseStaleEchoCannotConfirmReplacementSession()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        DeadlineSignal deadline{false};
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        std::promise<void> firstStarted;
        std::promise<void> secondStarted;
        auto firstFuture = firstStarted.get_future();
        auto secondFuture = secondStarted.get_future();
        int calls = 0;
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token) {
                if (++calls == 1) {
                    firstStarted.set_value();
                }
                else {
                    secondStarted.set_value();
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        const AAPCommand command{AAPCommandKind::SetAdaptiveNoiseLevel, 70};
        broker.BeginSession(1);
        QVERIFY(broker.Submit(command).accepted);
        QVERIFY(firstFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.EndSession();
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::TransportClosed);
        broker.BeginSession(2);
        deadline.Arm();
        QVERIFY(broker.Submit(command).accepted);
        QVERIFY(secondFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.OnEcho(command, 1);
        QVERIFY(!broker.Confirmed(command.kind).has_value());
        QVERIFY(deadline.Wait());
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->generation, uint64_t{2});
        QCOMPARE(outcome->settlement, AAPCommandSettlement::TimedOut);
    }

    void AdaptiveNoiseDesiredStateReplaysAfterReconnect()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        std::mutex sentMutex;
        std::vector<uint64_t> generations;
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token) {
                {
                    std::lock_guard lock{sentMutex};
                    generations.push_back(generation);
                }
                liveBroker->OnEcho(command, generation);
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        liveBroker = &broker;
        const auto result = broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 65});
        QVERIFY(!result.accepted);
        QCOMPARE(result.failure, std::optional(AAPCommandFailure::TransportClosed));
        broker.ReplayDesiredState(3);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        broker.EndSession();
        broker.ReplayDesiredState(4);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        std::lock_guard lock{sentMutex};
        QCOMPARE(generations, (std::vector<uint64_t>{3, 4}));
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);
    }

    void AdaptiveNoiseRecordedWhileIneligibleReplaysOnlyWhenEligible()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        std::mutex sentMutex;
        std::vector<uint32_t> sent;
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token) {
                {
                    std::lock_guard lock{sentMutex};
                    sent.push_back(command.value);
                }
                liveBroker->OnEcho(command, generation);
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        liveBroker = &broker;
        QVERIFY(!broker.RecordDesired({AAPCommandKind::SetAdaptiveNoiseLevel, 60}).accepted);
        QCOMPARE(
            broker.Desired(AAPCommandKind::SetAdaptiveNoiseLevel), std::optional<uint32_t>{60});
        broker.ReplayDesiredState(
            1, [](AAPCommandKind kind) { return kind != AAPCommandKind::SetAdaptiveNoiseLevel; });
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        QCOMPARE(outcomes.Size(), size_t{0});
        {
            std::lock_guard lock{sentMutex};
            QVERIFY(sent.empty());
        }
        broker.EndSession();
        broker.ReplayDesiredState(2);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        std::lock_guard lock{sentMutex};
        QCOMPARE(sent, (std::vector<uint32_t>{60}));
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);
    }

    void AdaptiveNoiseRapidSliderKeepsOnlyLastQueuedValue()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        ManualGate gate;
        std::mutex sentMutex;
        std::vector<uint32_t> sent;
        bool first = true;
        std::promise<void> firstStarted;
        auto firstFuture = firstStarted.get_future();
        AAPCommandBroker *liveBroker = nullptr;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token stop) {
                {
                    std::lock_guard lock{sentMutex};
                    sent.push_back(command.value);
                }
                if (first) {
                    first = false;
                    firstStarted.set_value();
                    gate.Wait(stop);
                }
                else {
                    liveBroker->OnEcho(command, generation);
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        liveBroker = &broker;
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, 10}).accepted);
        QVERIFY(firstFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        for (const uint32_t value : {20U, 30U, 40U, 50U}) {
            QVERIFY(broker.Submit({AAPCommandKind::SetAdaptiveNoiseLevel, value}).accepted);
        }
        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        std::lock_guard lock{sentMutex};
        QCOMPARE(sent, (std::vector<uint32_t>{10, 50}));
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Superseded);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Confirmed);
    }

    void StaleGenerationEchoCannotConfirmNewCommand()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        DeadlineSignal deadline{false};
        AAPCommandBroker::Config config;
        config.clock.now = [&] { return deadline.Read(clock); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        std::promise<void> firstStarted;
        std::promise<void> secondStarted;
        auto firstFuture = firstStarted.get_future();
        auto secondFuture = secondStarted.get_future();
        int calls = 0;
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token) {
                if (++calls == 1) {
                    firstStarted.set_value();
                }
                else {
                    secondStarted.set_value();
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        const AAPCommand command{AAPCommandKind::SetPpeCapLevel, 80};
        broker.BeginSession(1);
        QVERIFY(broker.Submit(command).accepted);
        QVERIFY(firstFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.EndSession();
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::TransportClosed);

        broker.BeginSession(2);
        deadline.Arm();
        QVERIFY(broker.Submit(command).accepted);
        QVERIFY(secondFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.OnEcho(command, 1);
        QVERIFY(!broker.Confirmed(command.kind).has_value());
        QVERIFY(deadline.Wait());
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{2000});
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->generation, uint64_t{2});
        QCOMPARE(outcome->settlement, AAPCommandSettlement::TimedOut);
    }

    void HeadTrackingStartRequiresCurrentGenerationEvidence()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        std::atomic<bool> deadlineRead{false};
        AAPCommandBroker::Config config;
        config.clock.now = [&] {
            deadlineRead.store(true);
            return clock.Now();
        };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        int sends = 0;
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token) {
                if (++sends == 1) {
                    started.set_value();
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(7);
        QVERIFY(broker.Submit({AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        QVERIFY(startedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QTRY_VERIFY(deadlineRead.load());
        broker.OnEcho({AAPCommandKind::SetHeadTracking, 0x01}, 6);
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{1999});
        QCOMPARE(outcomes.Size(), size_t{0});
        broker.AdvanceTimeForTesting(std::chrono::milliseconds{1});
        const auto timeout = outcomes.Wait();
        QVERIFY(timeout.has_value());
        QCOMPARE(timeout->settlement, AAPCommandSettlement::TimedOut);
        QCOMPARE(timeout->generation, uint64_t{7});
        QVERIFY(!broker.Confirmed(AAPCommandKind::SetHeadTracking).has_value());
        QCOMPARE(broker.Desired(AAPCommandKind::SetHeadTracking), std::optional<uint32_t>{1});

        // Stop has no protocol evidence and therefore settles after the write rather than timing
        // out or claiming confirmation.
        QVERIFY(broker.Submit({AAPCommandKind::SetHeadTracking, 0x00}).accepted);
        const auto stop = outcomes.Wait();
        QVERIFY(stop.has_value());
        QCOMPARE(stop->settlement, AAPCommandSettlement::AppliedUnconfirmed);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
    }

    void ReconnectReplaysEveryDesiredValueInStableOrder()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        std::mutex sentMutex;
        std::vector<AAPCommand> sent;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t, std::stop_token) {
                std::lock_guard lock{sentMutex};
                sent.push_back(command);
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); },
            UnverifiableConfig(clock));
        for (size_t index = 0; index < kAAPCommandKindCount; ++index) {
            const auto kind = static_cast<AAPCommandKind>(index);
            const auto result = broker.Submit({kind, static_cast<uint32_t>(index + 1)});
            QVERIFY(!result.accepted);
            QCOMPARE(result.failure, std::optional(AAPCommandFailure::TransportClosed));
        }
        broker.ReplayDesiredState(7);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        {
            std::lock_guard lock{sentMutex};
            QCOMPARE(sent.size(), kAAPCommandKindCount);
            for (size_t index = 0; index < kAAPCommandKindCount; ++index) {
                QCOMPARE(sent[index].kind, static_cast<AAPCommandKind>(index));
                QCOMPARE(sent[index].value, index + 1U);
            }
        }
        broker.ReplayDesiredState(7);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        {
            std::lock_guard lock{sentMutex};
            QCOMPARE(sent.size(), kAAPCommandKindCount);
        }
        broker.EndSession();
        broker.ReplayDesiredState(8);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        {
            std::lock_guard lock{sentMutex};
            QCOMPARE(sent.size(), kAAPCommandKindCount * 2);
        }
    }

    void DisconnectedStopSupersedesReconnectReplay()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        ManualGate gate;
        std::promise<void> blockerStarted;
        auto blockerFuture = blockerStarted.get_future();
        std::atomic<bool> blockerAnnounced{false};
        std::mutex sentMutex;
        std::vector<AAPCommand> sent;
        std::vector<uint64_t> generations;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token stop) {
                {
                    std::lock_guard lock{sentMutex};
                    sent.push_back(command);
                    generations.push_back(generation);
                }
                if (command.kind == AAPCommandKind::SetNoiseControl) {
                    if (!blockerAnnounced.exchange(true)) {
                        blockerStarted.set_value();
                    }
                    gate.Wait(stop);
                }
                return true;
            },
            {}, UnverifiableConfig(clock));
        const AAPCommand start{AAPCommandKind::SetHeadTracking, 0x01};
        const AAPCommand stop{AAPCommandKind::SetHeadTracking, 0x00};
        // Start is desired while offline. A blocker keeps the reconnect's replay queued while
        // the transport owner opens generation 8 but the intent still believes it is offline.
        QVERIFY(!broker.Submit(start).accepted);
        broker.BeginSession(7);
        const auto noise = broker.Submit({AAPCommandKind::SetNoiseControl, 0x01});
        QVERIFY(noise.accepted);
        broker.ReplayDesiredState(7);
        QVERIFY(blockerFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.ReplayDesiredState(8);
        QCOMPARE(broker.Desired(start.kind), std::optional<uint32_t>{0x01});
        QCOMPARE(broker.QueuedCount(), size_t{2});

        const auto disconnectedStop = broker.RecordDesired(stop);
        QVERIFY(!disconnectedStop.accepted);
        QCOMPARE(disconnectedStop.failure, std::optional(AAPCommandFailure::TransportClosed));
        QCOMPARE(broker.Desired(start.kind), std::optional<uint32_t>{0x00});
        QCOMPARE(broker.QueuedCount(), size_t{1});

        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        {
            std::lock_guard lock{sentMutex};
            QCOMPARE(
                sent, (std::vector<AAPCommand>{
                          {AAPCommandKind::SetNoiseControl, 0x01, noise.requestId},
                          {AAPCommandKind::SetNoiseControl, 0x01, noise.requestId}}));
            QCOMPARE(generations, (std::vector<uint64_t>{7, 8}));
        }

        // The cancelled replay Start must not survive into the next reconnect either.
        broker.EndSession();
        broker.ReplayDesiredState(9);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        {
            std::lock_guard lock{sentMutex};
            QCOMPARE(
                sent, (std::vector<AAPCommand>{
                          {AAPCommandKind::SetNoiseControl, 0x01, noise.requestId},
                          {AAPCommandKind::SetNoiseControl, 0x01, noise.requestId},
                          {AAPCommandKind::SetNoiseControl, 0x01, noise.requestId},
                          {AAPCommandKind::SetHeadTracking, 0x00, disconnectedStop.requestId}}));
            QCOMPARE(generations, (std::vector<uint64_t>{7, 8, 9, 9}));
        }
    }

    void OfflineSameDesireCancelsQueuedReplay()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        ManualGate gate;
        std::promise<void> blockerStarted;
        auto blockerFuture = blockerStarted.get_future();
        std::atomic<unsigned> sends{0};
        int blockerCalls = 0;
        std::mutex sentMutex;
        std::vector<uint64_t> generations;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t generation, std::stop_token stop) {
                if (command.kind == AAPCommandKind::SetNoiseControl) {
                    if (++blockerCalls == 1) {
                        blockerStarted.set_value();
                    }
                    gate.Wait(stop);
                    return true;
                }
                std::lock_guard lock{sentMutex};
                ++sends;
                generations.push_back(generation);
                return true;
            },
            {}, UnverifiableConfig(clock));
        const AAPCommand start{AAPCommandKind::SetHeadTracking, 0x01};
        broker.BeginSession(7);
        QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x01}).accepted);
        QVERIFY(blockerFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(broker.Submit(start).accepted);
        QCOMPARE(broker.QueuedCount(), size_t{1});

        // The transport owner has already moved offline while the broker session is still
        // observable. Re-recording the same desired value must cancel its stale queued work.
        const auto result = broker.RecordDesired(start);
        QVERIFY(!result.accepted);
        QCOMPARE(result.failure, std::optional(AAPCommandFailure::TransportClosed));
        QCOMPARE(broker.Desired(start.kind), std::optional<uint32_t>{0x01});
        QCOMPARE(broker.QueuedCount(), size_t{0});

        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        QCOMPARE(sends.load(), 0U);

        broker.EndSession();
        broker.ReplayDesiredState(8);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        QCOMPARE(sends.load(), 1U);
        std::lock_guard lock{sentMutex};
        QCOMPARE(generations, (std::vector<uint64_t>{8}));
    }

    void DisconnectedStopSupersedesInFlightStart()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        ManualGate gate;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::atomic<unsigned> calls{0};
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token stop) {
                if (++calls == 1) {
                    started.set_value();
                    gate.Wait(stop);
                }
                return false;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); },
            UnverifiableConfig(clock));
        broker.BeginSession(7);
        const auto startResult = broker.Submit({AAPCommandKind::SetHeadTracking, 0x01});
        QVERIFY(startResult.accepted);
        QVERIFY(startedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(!broker.RecordDesired({AAPCommandKind::SetHeadTracking, 0x00}).accepted);
        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));

        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(
            outcome->command,
            (AAPCommand{AAPCommandKind::SetHeadTracking, 0x01, startResult.requestId}));
        QCOMPARE(outcome->settlement, AAPCommandSettlement::Superseded);
        QCOMPARE(outcome->failure, std::optional(AAPCommandFailure::Superseded));
        QCOMPARE(calls.load(), 1U);
        QCOMPARE(broker.Desired(AAPCommandKind::SetHeadTracking), std::optional<uint32_t>{0});
        QCOMPARE(broker.QueuedCount(), size_t{0});
    }

    void RapidSliderSubmissionsKeepOnlyLastQueuedValue()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        ManualGate gate;
        std::mutex sentMutex;
        std::vector<uint32_t> sent;
        bool first = true;
        std::promise<void> firstStarted;
        auto firstFuture = firstStarted.get_future();
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t, std::stop_token stop) {
                {
                    std::lock_guard lock{sentMutex};
                    sent.push_back(command.value);
                }
                if (first) {
                    first = false;
                    firstStarted.set_value();
                    gate.Wait(stop);
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); },
            UnverifiableConfig(clock));
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetPpeCapLevel, 10}).accepted);
        QVERIFY(firstFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        for (uint32_t value : {20U, 30U, 40U, 50U}) {
            QVERIFY(broker.Submit({AAPCommandKind::SetPpeCapLevel, value}).accepted);
        }
        QCOMPARE(broker.Desired(AAPCommandKind::SetPpeCapLevel), std::optional<uint32_t>{50});
        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        std::lock_guard lock{sentMutex};
        QCOMPARE(sent, (std::vector<uint32_t>{10, 50}));
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Superseded);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::AppliedUnconfirmed);
    }

    void TransportClosePreservesDesiredAndDropsQueuedWork()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        ManualGate gate;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        std::atomic<size_t> sends{0};
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token stop) {
                ++sends;
                started.set_value();
                gate.Wait(stop);
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x02}).accepted);
        QVERIFY(startedFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(broker.Submit({AAPCommandKind::SetHeadTracking, 0x01}).accepted);
        broker.EndSession();
        gate.Open();
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::TransportClosed);
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::TransportClosed);
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        QCOMPARE(sends.load(), size_t{1});
        QCOMPARE(broker.QueuedCount(), size_t{0});
        QCOMPARE(broker.Desired(AAPCommandKind::SetNoiseControl), std::optional<uint32_t>{0x02});
        QCOMPARE(broker.Desired(AAPCommandKind::SetHeadTracking), std::optional<uint32_t>{0x01});
        QVERIFY(!broker.IsSessionOpen());
    }

    void SupersededFlightCannotConsumeOldValueEcho()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        AAPCommandBroker::Config config;
        config.clock.now = [&clock] { return clock.Now(); };
        config.clock.advance = [&clock](std::chrono::milliseconds delta) { clock.Advance(delta); };
        OutcomeQueue outcomes;
        ManualGate gate;
        std::promise<void> firstStarted;
        std::promise<void> secondStarted;
        auto firstFuture = firstStarted.get_future();
        auto secondFuture = secondStarted.get_future();
        int calls = 0;
        AAPCommandBroker broker(
            [&](const AAPCommand &, uint64_t, std::stop_token stop) {
                if (++calls == 1) {
                    firstStarted.set_value();
                    gate.Wait(stop);
                }
                else {
                    secondStarted.set_value();
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); }, config);
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetPpeCapLevel, 25}).accepted);
        QVERIFY(firstFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        QVERIFY(broker.Submit({AAPCommandKind::SetPpeCapLevel, 75}).accepted);
        gate.Open();
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::Superseded);
        broker.OnEcho({AAPCommandKind::SetPpeCapLevel, 25}, 1);
        QCOMPARE(broker.Confirmed(AAPCommandKind::SetPpeCapLevel), std::optional<uint32_t>{25});
        QVERIFY(secondFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        broker.OnEcho({AAPCommandKind::SetPpeCapLevel, 75}, 1);
        const auto outcome = outcomes.Wait();
        QVERIFY(outcome.has_value());
        QCOMPARE(outcome->settlement, AAPCommandSettlement::Confirmed);
        QCOMPARE(broker.Confirmed(AAPCommandKind::SetPpeCapLevel), std::optional<uint32_t>{75});
    }

    void CancelledQueuedCommandNeverReachesWire()
    {
        using namespace Core::AAP;
        AAPTestSupport::ManualClock clock;
        OutcomeQueue outcomes;
        ManualGate gate;
        std::promise<void> firstStarted;
        auto firstFuture = firstStarted.get_future();
        std::mutex sentMutex;
        std::vector<uint32_t> sent;
        AAPCommandBroker broker(
            [&](const AAPCommand &command, uint64_t, std::stop_token stop) {
                {
                    std::lock_guard lock{sentMutex};
                    sent.push_back(command.value);
                }
                if (sent.size() == 1) {
                    firstStarted.set_value();
                    gate.Wait(stop);
                }
                return true;
            },
            [&](const AAPCommandOutcome &outcome) { outcomes.Push(outcome); },
            UnverifiableConfig(clock));
        broker.BeginSession(1);
        QVERIFY(broker.Submit({AAPCommandKind::SetNoiseControl, 0x01}).accepted);
        QVERIFY(firstFuture.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        std::stop_source stop;
        QVERIFY(broker.Submit({AAPCommandKind::SetPpeCapLevel, 50}, stop.get_token()).accepted);
        stop.request_stop();
        gate.Open();
        QVERIFY(broker.WaitForIdle(std::chrono::seconds{1}));
        std::lock_guard lock{sentMutex};
        QCOMPARE(sent, (std::vector<uint32_t>{0x01}));
        QCOMPARE(outcomes.Wait()->settlement, AAPCommandSettlement::AppliedUnconfirmed);
        const auto cancelled = outcomes.Wait();
        QVERIFY(cancelled.has_value());
        QCOMPARE(cancelled->settlement, AAPCommandSettlement::Rejected);
        QCOMPARE(cancelled->failure, std::optional(AAPCommandFailure::Rejected));
    }

    void AcknowledgementRecordsAreRecognised()
    {
        QVERIFY(IsHandshakeAck(Hex({0x01, 0x00, 0x04, 0x00})));
        // Five bytes with the same prefix is an ATT Error Response instead.
        QVERIFY(!IsHandshakeAck(Hex({0x01, 0x00, 0x04, 0x00, 0x0A})));
        QVERIFY(!IsHandshakeAck(Hex({0x01, 0x00, 0x04})));
        QVERIFY(IsFeatureFlagsAck(Hex({0x04, 0x00, 0x04, 0x00, 0x2B, 0x00})));
        QVERIFY(!IsFeatureFlagsAck(Hex({0x04, 0x00, 0x04, 0x00, 0x4D, 0x00})));
        QVERIFY(!Att::IsAttFrame(Hex({0x01, 0x00, 0x04, 0x00})));
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

    void DiagnosticsRecorderResetsAndIsolatesGenerations()
    {
        using Core::Diagnostics::Recorder;
        using Core::Diagnostics::Step;

        auto &recorder = Recorder::Instance();
        recorder.BeginAttempt(7);
        recorder.RecordForAttempt(7, Step::AdvertisementMatched, true, "model=14");
        recorder.RecordPendingForAttempt(7, Step::Handshake, "send accepted, ACK unconfirmed");
        recorder.RecordForAttempt(7, Step::FeatureFlags, false, "send failed");
        recorder.RecordPendingForAttempt(7, Step::AttProbe, "probe sent, waiting up to 500ms");
        QCOMPARE(recorder.ReportedAttemptStepCount(), size_t{4});

        const auto report = QString::fromStdString(recorder.BuildReport({}));
        QVERIFY(report.contains("attempt 7"));
        QVERIFY(report.contains("handshake  PENDING  send accepted, ACK unconfirmed"));
        QVERIFY(!report.contains("handshake  ok"));
        QVERIFY(report.contains("feature_flags  FAILED  send failed"));
        QVERIFY(report.contains("att_probe  PENDING"));

        // A late event or end notification from generation 6 cannot alter open attempt 7.
        const auto before = recorder.StepCount();
        recorder.RecordForAttempt(6, Step::Handshake, true, "stale ACK");
        recorder.EndAttempt(6);
        QCOMPARE(recorder.StepCount(), before);
        QCOMPARE(recorder.CurrentGeneration(), uint64_t{7});
        QVERIFY(!QString::fromStdString(recorder.BuildReport({})).contains("stale ACK"));
        recorder.EndAttempt(7);
        QCOMPARE(recorder.CurrentGeneration(), uint64_t{0});

        // BeginAttempt resets before opening generation 8, including untagged preconnect
        // lines. No scanner, advertisement or handshake evidence from attempt 7 survives.
        recorder.Record(Step::ScannerStarted, true);
        recorder.Record(Step::AdvertisementMatched, true, "old preconnect");
        recorder.BeginAttempt(8);
        QCOMPARE(recorder.StepCount(), size_t{0});
        QCOMPARE(recorder.ReportedAttemptStepCount(), size_t{0});
        QCOMPARE(recorder.CurrentGeneration(), uint64_t{8});
        QCOMPARE(recorder.ReportedAttempt(), uint64_t{8});

        // Untagged BLE watcher callbacks have no attempt identity. While an attempt is open,
        // accepting them would silently attach a delayed event to the successor generation.
        const auto openCount = recorder.StepCount();
        recorder.Record(Step::ScannerStarted, true, "stale watcher callback");
        recorder.RecordPending(Step::AttProbe, "stale watcher callback");
        QCOMPARE(recorder.StepCount(), openCount);

        recorder.RecordForAttempt(7, Step::Handshake, true, "late old ACK");
        recorder.RecordForAttempt(8, Step::TransportConnect, false, "driver unavailable");
        recorder.BeginAttempt(8); // idempotent while the same attempt is open
        QCOMPARE(recorder.ReportedAttemptStepCount(), size_t{1});
        const auto fresh = QString::fromStdString(recorder.BuildReport({}));
        QVERIFY(fresh.contains("attempt 8"));
        QVERIFY(fresh.contains("transport_connect  FAILED  driver unavailable"));
        QVERIFY(!fresh.contains("attempt 7"));
        QVERIFY(!fresh.contains("scanner_started"));
        QVERIFY(!fresh.contains("advertisement_matched"));
        QVERIFY(!fresh.contains("late old ACK"));
        QVERIFY(!fresh.contains("stale watcher callback"));
        recorder.EndAttempt(8);
    }
};

QTEST_GUILESS_MAIN(AAPProtocolTests)

#include "AAPProtocolTests.moc"
