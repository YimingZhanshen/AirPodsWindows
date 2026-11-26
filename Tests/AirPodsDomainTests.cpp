#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string_view>
#include <tuple>
#include <vector>

#include <boost/pfr.hpp>
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMetaObject>
#include <QMap>
#include <QTemporaryFile>
#include <QVector>
#include <QXmlStreamReader>

#include <Config.h>
#include "Source/Core/AirPods.h"
#include "Source/Core/BatteryFusion.h"
#include "Source/Core/AdaptiveNoiseLevel.h"
#include "Source/Core/AAP.h"
#include "Source/Core/HeadTrackingIntent.h"
#include "Source/Core/Settings.h"
#include "Source/Core/SettingsRepository.h"
#include "Source/Core/Update.h"
#include "Source/Gui/HeadTrackingPresentation.h"
#include "Source/Gui/MainWindowPresentation.h"
#include "Source/Gui/TaskbarGeometry.h"

namespace {

using Core::AirPods::Model;
using Core::AirPods::Side;
using Core::AirPods::Details::Advertisement;
using Core::AirPods::Details::EarDetectionTracker;
using Core::AirPods::Details::LidTracker;
using Core::AirPods::Details::StateManager;
using ReceivedData = Core::Bluetooth::AdvertisementWatcher::ReceivedData;
using namespace std::chrono_literals;

class RecordingSettingsObserver final : public Core::Settings::ApplyObserver
{
public:
    void OnLanguageLocaleChanged(const QLocale &) override {}
    void OnAppearanceModeChanged(Core::Settings::AppearanceMode mode) override
    {
        appearanceMode = mode;
        ++appearanceModeChanges;
    }
    void OnAutoRunChanged(bool enable) override
    {
        autoRun = enable;
        ++autoRunChanges;
    }
    void OnLowAudioLatencyChanged(bool) override {}
    void OnAutomaticEarDetectionChanged(bool enable) override
    {
        automaticEarDetection = enable;
    }
    void OnConversationalAwarenessChanged(bool enable) override
    {
        conversationalAwareness = enable;
    }
    void OnConversationalAwarenessVolumePercentChanged(uint8_t percent) override
    {
        conversationalAwarenessVolumePercent = percent;
    }
    void OnPersonalizedVolumeChanged(bool enable) override
    {
        personalizedVolume = enable;
    }
    void OnLoudSoundReductionChanged(bool enable) override
    {
        loudSoundReduction = enable;
    }
    void OnPpeCapLevelChanged(uint8_t level) override
    {
        ppeCapLevel = level;
    }
    void OnAdaptiveNoiseLevelChanged(uint8_t level) override
    {
        adaptiveNoiseLevel = level;
        ++adaptiveNoiseLevelChanges;
    }
    void OnNoiseControlModeChanged(uint32_t mode) override
    {
        noiseControlMode = mode;
    }
    void OnHeadTrackingEnabledChanged(bool enable) override
    {
        headTrackingEnabled = enable;
        ++headTrackingChanges;
    }
    void OnRssiMinChanged(int16_t) override {}
    void OnDeviceAddressChanged(uint64_t) override {}
    void OnTrayIconBatteryChanged(Core::Settings::TrayIconBatteryBehavior) override {}
    void OnTrayQuickConnectEnabledChanged(bool enable) override
    {
        quickConnectEnabled = enable;
        ++quickConnectEnabledChanges;
    }
    void OnAudioAutoConnectEnabledChanged(bool enable) override
    {
        audioAutoConnectEnabled = enable;
        ++audioAutoConnectChanges;
    }
    void OnTrayQuickConnectDeviceChanged(const QString &deviceId) override
    {
        quickConnectDeviceId = deviceId;
        ++quickConnectDeviceChanges;
    }
    void OnTaskbarBatteryChanged(Core::Settings::TaskbarStatusBehavior) override {}
    void OnStemActionsChanged(const Core::AAP::StemActionTable &actions) override
    {
        stemActions = actions;
        ++stemActionChanges;
    }

    bool automaticEarDetection{false};
    bool conversationalAwareness{false};
    uint8_t conversationalAwarenessVolumePercent{};
    bool personalizedVolume{false};
    bool loudSoundReduction{false};
    uint8_t ppeCapLevel{};
    uint8_t adaptiveNoiseLevel{};
    int adaptiveNoiseLevelChanges{0};
    uint32_t noiseControlMode{};
    bool headTrackingEnabled{false};
    int headTrackingChanges{0};
    bool audioAutoConnectEnabled{false};
    int audioAutoConnectChanges{0};
    bool autoRun{false};
    int autoRunChanges{0};
    Core::Settings::AppearanceMode appearanceMode{Core::Settings::AppearanceMode::System};
    int appearanceModeChanges{0};
    bool quickConnectEnabled{false};
    int quickConnectEnabledChanges{0};
    QString quickConnectDeviceId;
    int quickConnectDeviceChanges{0};
    Core::AAP::StemActionTable stemActions{};
    int stemActionChanges{0};
};

class FailingSyncRepository final : public Core::Settings::Repository
{
public:
    bool Contains(const QString &key) const override
    {
        return _values.Contains(key);
    }

    QVariant Read(const QString &key) const override
    {
        return _values.Read(key);
    }

    QStringList Keys() const override
    {
        return _values.Keys();
    }

    void Write(const QString &key, const QVariant &value) override
    {
        _values.Write(key, value);
    }

    void Remove(const QString &key) override
    {
        _values.Remove(key);
    }

    bool Sync() override
    {
        return false;
    }

private:
    Core::Settings::MemoryRepository _values;
};

std::vector<uint8_t> MakePacket(
    uint16_t modelId, Side side, uint8_t leftBattery, uint8_t rightBattery, uint8_t caseBattery,
    bool bothInCase = true, bool lidOpened = true)
{
    std::vector<uint8_t> packet(27, 0);
    packet[0] = static_cast<uint8_t>(Core::AppleCP::PacketType::ProximityPairing);
    packet[1] = 25;
    packet[3] = static_cast<uint8_t>(modelId & 0xff);
    packet[4] = static_cast<uint8_t>(modelId >> 8);

    constexpr uint8_t kCurrentInEar = 1 << 1;
    constexpr uint8_t kBothInCase = 1 << 2;
    constexpr uint8_t kAnotherInEar = 1 << 3;
    constexpr uint8_t kBroadcastFromLeft = 1 << 5;
    packet[5] = kCurrentInEar | kAnotherInEar;
    if (bothInCase) {
        packet[5] |= kBothInCase;
    }
    if (side == Side::Left) {
        packet[5] |= kBroadcastFromLeft;
        packet[6] = static_cast<uint8_t>((rightBattery << 4) | leftBattery);
    }
    else {
        packet[6] = static_cast<uint8_t>((leftBattery << 4) | rightBattery);
    }

    packet[7] = caseBattery;
    packet[8] = lidOpened ? 0 : 1 << 3;
    return packet;
}

ReceivedData MakeAdvertisementData(
    uint64_t address, int16_t rssi, Side side, uint8_t leftBattery = 8, uint8_t rightBattery = 7,
    uint8_t caseBattery = 5, uint16_t modelId = 0x2014)
{
    ReceivedData data;
    data.address = address;
    data.rssi = rssi;
    data.manufacturerDataMap.emplace(
        Core::AppleCP::VendorId, MakePacket(modelId, side, leftBattery, rightBattery, caseBattery));
    return data;
}

ReceivedData MakeTraceAdvertisementData(
    uint64_t address, int16_t rssi, uint8_t statusByte, uint8_t batteryByte = 0x88,
    uint8_t chargingByte = 0x88, uint16_t modelId = 0x2024)
{
    auto data = MakeAdvertisementData(address, rssi, Side::Left, 8, 8, 8, modelId);
    auto &packet = data.manufacturerDataMap.at(Core::AppleCP::VendorId);
    packet[5] = statusByte;
    packet[6] = batteryByte;
    packet[7] = chargingByte;
    return data;
}

Core::AAP::AAPBatteryReading
AapReading(Core::AAP::BatteryComponent component, uint8_t level, Core::AAP::BatteryStatus status)
{
    return {component, level, status};
}

Core::AAP::AAPBatterySnapshot LeftSnapshot(uint8_t level)
{
    Core::AAP::AAPBatterySnapshot snapshot;
    snapshot.left =
        AapReading(Core::AAP::BatteryComponent::Left, level, Core::AAP::BatteryStatus::Discharging);
    return snapshot;
    class RecordingHeadTrackingPort final : public Core::AAP::IHeadTrackingCommandPort
    {
    public:
        bool RequestHeadTracking(bool enable) override
        {
            requests.push_back(enable);
            return succeed;
        }

        bool succeed{true};
        std::vector<bool> requests;
    };

    std::vector<uint8_t> MakeHeadTrackingPacket(uint8_t subCode = 0x44)
    {
        std::vector<uint8_t> packet(70, 0);
        constexpr std::array<uint8_t, 10> prefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                                 0x00, 0x00, 0x00, 0x10, 0x00};
        std::copy(prefix.begin(), prefix.end(), packet.begin());
        packet[10] = subCode;
        const auto writeInt16 = [&packet](std::size_t offset, std::int16_t value) {
            packet[offset] = static_cast<uint8_t>(value & 0xFF);
            packet[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        writeInt16(43, -32768);
        writeInt16(45, -2);
        writeInt16(47, 32767);
        writeInt16(51, 1234);
        writeInt16(53, -42);
        return packet;
    }

} // namespace

class AirPodsDomainTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void RejectsMalformedPackets();
    void RecognizesAirPods5();
    void ResolvesAirPods5DisplayName();
    void RecognizesAirPodsMaxUsbC();
    void ResolvesAirPodsMaxUsbCDisplayName();
    void PreservesForkAndAddsUpstreamModelMappings();
    void MapsModelNumberStringsToUnionOfForkAndUpstreamModels();
    void RefinesKnownModelsOnlyWithinTheirFamily();
    void AirPods5VariantsExposeAapCapabilities();
    void ManagerUsesResolvedModelForStateAndFiltering();
    void RejectsUnknownModelNumberWithoutGuessing();
    void ModelProductIdUnionMatchesExpectedEntries();
    void ParsesAdvertisementState();
    void NormalizesSingleBatteryModels();
    void FiltersDuplicateAndWeakAdvertisements();
    void AcceptsWeakAndUnknownRssiFromTrackedAddress();
    void RemembersSideAddressAcrossStateReset();
    void RejectsConflictingBatterySources();
    void RecoversFromWeakerBatterySource();
    void AcceptsAdjacentBatteryBucketsAfterAddressRotation();
    void FusesAapBatteryOverBleWithinSession();
    void FallsBackWhenAapComponentDisconnects();
    void FallsBackWhenAapSessionResets();
    void ConfirmsLargeBatteryDropOnlyAfterTwoIdenticalReadings();
    void HoldsFirstAapDropAgainstBleBaseline();
    void ClearsPendingDropWhenComponentIsOmitted();
    void HeadsetBatteryUsesLeftPresentationSlot();
    void SerializesBleAndAapStatePublication();
    void SerializesFusedStatePublicationAcrossNestedEventLoop();
    void AcceptsSmallBatteryDropsAndRisesImmediately();
    void MarksBatterySourceConflictBeyondBleBucket();
    void UsesAapStatusForBatteryCharging();
    void BasicStateEqualityCoversBatterySourceFields();
    void PresentationCarriesBatterySourceFields();
    void MergesAdvertisementsFromBothSides();
    void RejectsAdvertisementsFromDifferentModels();
    void AcceptsKnownModelAfterUnknownAdvertisement();
    void TracksLidAcrossShortGaps();
    void TracksLidWhilePodsAreOut();
    void LidPopupCooldownBlocksRapidRepeats();
    void EarDetectionTrackerSurvivesStateLoss();
    void RecognizesHeadTrackingPackets();
    void HeadTrackingPresentationAgreesWithCoreParser();
    void HeadTrackingViewModelThrottlesAndTracksGenerations();
    void HeadTrackingIntentReplaysStartAndStop();
    void HeadTrackingSettingPersistsWithoutAbiBump();
    void AdaptiveNoiseTranslationsAreCompleteInAllCatalogs();
    void PackagesCompatibleLowLatencySilence();
    void LoadsSettingsThroughRepository();
    void MigratesLegacySettingsRepository();
    void PreservesExistingSettingsDuringMigration();
    void RollsBackFailedSettingsMigration();
    void PropagatesQuickConnectSettings();
    void PersistsAndPropagatesStemActions();
    void PreservesForkSettingsAndAddsPlaybackAutoConnect();
    void KeepsPpeCapIndependentFromLegacyAdaptiveTransparency();
    void AppendsPpeCapAfterExistingSettingsFields();
    void PersistsPpeCapLevelRoundTrip();
    void PersistsAdaptiveNoiseLevelDefaultAndReloads();
    void ClampsAndGatesAdaptiveNoiseLevelPolicy();
    void RejectsNullSettingsRepository();
    void ParsesUpdateVersions();
    void ParsesGitHubReleaseMetadata();
    void MatchesInstallerAssetsByArchitecture();
    void RejectsUpdateAssetForAnotherArchitecture();
    void RejectsReleaseMetadataFromAnotherRepository();
    void RejectsUpdateAssetsWithoutDigest();
    void VerifiesUpdateFileDigest();
    void PresentsMainWindowLifecycleStates();
    void PresentsMainWindowDeviceState();
    void PresentsMainWindowCaseBattery();
    void MergesMatchingPodBatteries();
    void PresentsScenesFromPodPlacement();
    void CollapsesAirPodsMaxToSingleBattery();
    void ParsesCapturedSpaceGrayAirPodsMax();
    void PreservesForkAnimationMappingsAndColors();
    void PlacesPopupInsideMultimonitorBounds();
    void TranslationLocalesIncludeUpstreamAndForkLanguages();
    void StemSettingsXmlExposesAllLabels();
    void StemTranslationsAreFinished_data();
    void StemTranslationsAreFinished();
    void AdaptiveNoiseTranslationsAreCompleteInAllCatalogs();
    void HebrewUpdateWindowTranslationsAreFinished_data();
    void HebrewUpdateWindowTranslationsAreFinished();
    void MapsMainWindowAnimationResources();
    void ConvertsTaskbarGeometryAcrossDpiBoundaries();
};

void AirPodsDomainTests::RejectsMalformedPackets()
{
    const std::vector<uint8_t> empty;
    QVERIFY(!Core::AppleCP::AirPods::IsValid(empty));

    auto packet = MakePacket(0x2014, Side::Left, 8, 7, 5);
    QVERIFY(Core::AppleCP::AirPods::IsValid(packet));

    packet[0] = static_cast<uint8_t>(Core::AppleCP::PacketType::AirDrop);
    QVERIFY(!Core::AppleCP::AirPods::IsValid(packet));

    packet = MakePacket(0x2014, Side::Left, 8, 7, 5);
    packet[1] = 24;
    QVERIFY(!Core::AppleCP::AirPods::IsValid(packet));
}

void AirPodsDomainTests::RecognizesAirPods5()
{
    // Neither local product id is correlated with a model number, so both remain coarse.
    // WCC is resolved later only from A3439/A3440/A3441.
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x2030), Model::AirPods_5);
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x2036), Model::AirPods_5);
    QCOMPARE(Helper::ToString(Model::AirPods_5), QString{"AirPods 5"});
    QCOMPARE(Helper::ToString(Model::AirPods_5_WCC), QString{"AirPods 5 (Wireless Charging Case)"});

    for (const auto &[modelId, expected] : {
             std::pair{uint16_t{0x2030}, Model::AirPods_5},
             std::pair{uint16_t{0x2036}, Model::AirPods_5},
         })
    {
        const Advertisement advertisement{
            MakeAdvertisementData(0x1234, -45, Side::Left, 8, 7, 5, modelId)};
        QCOMPARE(advertisement.GetAdvState().model, expected);
        QCOMPARE(advertisement.GetAdvState().pods.left.battery.Value(), 80u);
        QCOMPARE(advertisement.GetAdvState().pods.right.battery.Value(), 70u);
        QCOMPARE(advertisement.GetAdvState().caseBox.battery.Value(), 50u);
    }

    // Sanitized AirPods 5 family captures. Bytes 11-26 are intentionally zeroed because the
    // protocol's trailing payload may identify the device; no WCC model number is present.
    constexpr std::array<uint8_t, 27> capturedLeft{
        0x07, 0x19, 0x01, 0x30, 0x20, 0x35, 0xaa, 0xba, 0x32, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    constexpr std::array<uint8_t, 27> capturedRight{
        0x07, 0x19, 0x01, 0x30, 0x20, 0x55, 0xaa, 0xba, 0x32, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    StateManager manager;
    manager.OnRssiMinChanged(-80);
    for (const auto &[side, address, packet] : {
             std::tuple{Side::Left, uint64_t{0x1111}, capturedLeft},
             std::tuple{Side::Right, uint64_t{0x2222}, capturedRight},
         })
    {
        ReceivedData data;
        data.address = address;
        data.rssi = -60;
        data.manufacturerDataMap.emplace(
            Core::AppleCP::VendorId, std::vector<uint8_t>{packet.cbegin(), packet.cend()});
        const Advertisement captured{data};
        const auto &state = captured.GetAdvState();
        QCOMPARE(state.model, Model::AirPods_5);
        QCOMPARE(state.side, side);
        QCOMPARE(state.pods.left.battery.Value(), 100u);
        QCOMPARE(state.pods.right.battery.Value(), 100u);
        QCOMPARE(state.caseBox.battery.Value(), 100u);
        QVERIFY(state.pods.left.isCharging);
        QVERIFY(state.pods.right.isCharging);
        QVERIFY(!state.caseBox.isCharging);
        QVERIFY(!state.pods.left.isInEar);
        QVERIFY(!state.pods.right.isInEar);
        QVERIFY(state.caseBox.isBothPodsInCase);
        QVERIFY(state.caseBox.isLidOpened);
        const auto update = manager.OnAdvReceived(captured);
        if (side == Side::Left) {
            QVERIFY(update.has_value());
        }
    }
    QCOMPARE(manager.GetCurrentState()->model, Model::AirPods_5);
}

void AirPodsDomainTests::ResolvesAirPods5DisplayName()
{
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName("AirPods", Model::AirPods_5),
        QString{"AirPods 5"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName(
            "Kashionz's AirPods - Find My", Model::AirPods_5),
        QString{"Kashionz's AirPods"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName({}, Model::AirPods_5), QString{"AirPods 5"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName("AirPods", Model::AirPods_5_WCC),
        QString{"AirPods 5 (Wireless Charging Case)"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName(
            "Kashionz's AirPods - Find My", Model::AirPods_5_WCC),
        QString{"Kashionz's AirPods"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName({}, Model::AirPods_5_WCC),
        QString{"AirPods 5 (Wireless Charging Case)"});
}

void AirPodsDomainTests::RecognizesAirPodsMaxUsbC()
{
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x201F), Model::AirPods_Max_USB_C);
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x202D), Model::AirPods_Max_USB_C);
    QCOMPARE(Helper::ToString(Model::AirPods_Max_USB_C), QString{"AirPods Max (USB-C)"});
}

void AirPodsDomainTests::PreservesForkAndAddsUpstreamModelMappings()
{
    const std::vector<std::tuple<uint16_t, Model, QString>> models{
        {0x201D, Model::Powerbeats_Pro_2, "Powerbeats Pro 2"},
        {0x2027, Model::AirPods_Pro_3, "AirPods Pro 3"},
        {0x2030, Model::AirPods_5, "AirPods 5"},
        {0x2036, Model::AirPods_5, "AirPods 5"},
        {0x2003, Model::Powerbeats_3, "Powerbeats 3"},
        {0x2005, Model::Beats_X, "BeatsX"},
        {0x2006, Model::Beats_Solo3, "Beats Solo3"},
    };
    for (const auto &[modelId, model, name] : models) {
        QCOMPARE(Core::AppleCP::AirPods::GetModel(modelId), model);
        QCOMPARE(Helper::ToString(model), name);
        const Advertisement advertisement{
            MakeAdvertisementData(0x1234, -45, Side::Left, 8, 7, 5, modelId)};
        QCOMPARE(advertisement.GetAdvState().model, model);
    }
}

void AirPodsDomainTests::MapsModelNumberStringsToUnionOfForkAndUpstreamModels()
{
    // Every model number here has an entry in Docs/AAP-Protocol-Spec.md §14 (sourced from
    // librepods data/AirPods.kt, vendored under _research/) or is pinned by an upstream
    // commit already in the fork's history.
    const std::vector<std::pair<std::string_view, Model>> cases{
        {"A1523", Model::AirPods_1},           {"A1722", Model::AirPods_1},
        {"A2032", Model::AirPods_2},           {"A2031", Model::AirPods_2},
        {"A2565", Model::AirPods_3},           {"A2564", Model::AirPods_3},
        {"A3053", Model::AirPods_4},           {"A3050", Model::AirPods_4},
        {"A3054", Model::AirPods_4},           {"A3056", Model::AirPods_4_ANC},
        {"A3055", Model::AirPods_4_ANC},       {"A3057", Model::AirPods_4_ANC},
        {"A3531", Model::AirPods_5},           {"A3532", Model::AirPods_5},
        {"A3533", Model::AirPods_5},           {"A3439", Model::AirPods_5_WCC},
        {"A3440", Model::AirPods_5_WCC},       {"A3441", Model::AirPods_5_WCC},
        {"A2084", Model::AirPods_Pro},         {"A2083", Model::AirPods_Pro},
        {"A2931", Model::AirPods_Pro_2},       {"A2699", Model::AirPods_Pro_2},
        {"A2698", Model::AirPods_Pro_2},       {"A3047", Model::AirPods_Pro_2_USB_C},
        {"A3048", Model::AirPods_Pro_2_USB_C}, {"A3049", Model::AirPods_Pro_2_USB_C},
        {"A3063", Model::AirPods_Pro_3},       {"A3064", Model::AirPods_Pro_3},
        {"A3065", Model::AirPods_Pro_3},       {"A2096", Model::AirPods_Max},
        {"A3184", Model::AirPods_Max_USB_C},
    };
    for (const auto &[modelNumber, expected] : cases) {
        const auto actual = Core::AirPods::Details::ModelFromModelNumber(modelNumber);
        QVERIFY2(
            actual.has_value(),
            qPrintable(QString{"model number %1 unmapped"}.arg(
                QString::fromUtf8(modelNumber.data(), int(modelNumber.size())))));
        QCOMPARE(actual.value(), expected);
    }

    // Case and internal whitespace are normalised before lookup so an untrimmed Windows
    // System.Devices.ModelNumber value still resolves.
    QCOMPARE(Core::AirPods::Details::ModelFromModelNumber(" a3440 ").value(), Model::AirPods_5_WCC);
    QCOMPARE(
        Core::AirPods::Details::ModelFromModelNumber("A3440\r\n").value(), Model::AirPods_5_WCC);
}

void AirPodsDomainTests::RefinesKnownModelsOnlyWithinTheirFamily()
{
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::Unknown, Model::AirPods_5_WCC),
        Model::AirPods_5_WCC);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_5, Model::AirPods_5_WCC),
        Model::AirPods_5_WCC);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_5, Model::AirPods_5), Model::AirPods_5);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_5, Model::AirPods_Pro_3),
        Model::AirPods_5);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_4, Model::AirPods_4_ANC),
        Model::AirPods_4_ANC);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_Pro_2, Model::AirPods_Pro_2_USB_C),
        Model::AirPods_Pro_2_USB_C);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_Max, Model::AirPods_Max_USB_C),
        Model::AirPods_Max_USB_C);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_Pro_2_USB_C, Model::AirPods_Pro_2),
        Model::AirPods_Pro_2_USB_C);
    QCOMPARE(
        Core::AirPods::Details::ResolveModel(Model::AirPods_Pro_3, std::nullopt),
        Model::AirPods_Pro_3);
}

void AirPodsDomainTests::AirPods5VariantsExposeAapCapabilities()
{
    QVERIFY(Core::AirPods::Manager::SupportsANC(Model::AirPods_5));
    QVERIFY(Core::AirPods::Manager::SupportsANC(Model::AirPods_5_WCC));
    QVERIFY(!Core::AirPods::Manager::SupportsANC(Model::AirPods_4));
    QVERIFY(!Core::AirPods::Manager::SupportsANC(Model::AirPods_3));
    QVERIFY(!Core::AirPods::Manager::SupportsANC(Model::Powerbeats_Pro_2));
    QVERIFY(!Core::AirPods::Manager::SupportsANC(Model::Unknown));
}

void AirPodsDomainTests::ManagerUsesResolvedModelForStateAndFiltering()
{
    int updates = 0;
    Core::AirPods::State emitted;
    Core::AirPods::Manager manager;
    manager._modelOverride = Model::AirPods_5_WCC;
    manager._boundModel =
        Core::AirPods::Details::ResolveModel(Model::AirPods_5, manager._modelOverride);
    manager._deviceName = QString{"AirPods"};

    QObject::connect(
        &manager, &Core::AirPods::Manager::StateUpdated, [&](const Core::AirPods::State &state) {
            emitted = state;
            ++updates;
        });

    Core::AirPods::State state;
    state.model = Model::AirPods_5;
    manager.OnStateChanged({std::nullopt, state});
    QCOMPARE(updates, 1);
    QCOMPARE(emitted.model, Model::AirPods_5_WCC);
    QCOMPARE(emitted.displayName, QString{"AirPods 5 (Wireless Charging Case)"});

    manager._deviceConnected = true;
    QVERIFY(manager.OnAdvertisementReceived(
        MakeAdvertisementData(0x1234, -45, Side::Left, 8, 7, 5, 0x2030)));
    QCOMPARE(updates, 2);
    QCOMPARE(emitted.model, Model::AirPods_5_WCC);

    QVERIFY(!manager.OnAdvertisementReceived(
        MakeAdvertisementData(0x5678, -45, Side::Left, 8, 7, 5, 0x2027)));
    QCOMPARE(updates, 2);
}

void AirPodsDomainTests::RejectsUnknownModelNumberWithoutGuessing()
{
    // Apple has not published a reference for these IDs in the vendored spec, so the map
    // must not fabricate a Model for them. Callers fall back to the BLE advertisement path.
    QVERIFY(!Core::AirPods::Details::ModelFromModelNumber("").has_value());
    QVERIFY(!Core::AirPods::Details::ModelFromModelNumber("   ").has_value());
    QVERIFY(!Core::AirPods::Details::ModelFromModelNumber("A9999").has_value());
    QVERIFY(!Core::AirPods::Details::ModelFromModelNumber("Powerbeats Pro 2").has_value());
    QVERIFY(!Core::AirPods::Details::ModelFromModelNumber("Beats Solo3").has_value());
    // A2968 shows up in ntpods as an AirPods Pro 2 alias, but the fork's own spec table does
    // not cite it, so we keep it unmapped rather than inherit an unverified claim.
    QVERIFY(!Core::AirPods::Details::ModelFromModelNumber("A2968").has_value());
}

void AirPodsDomainTests::ModelProductIdUnionMatchesExpectedEntries()
{
    // This fixture locks the evidence-backed product-id union. Duplicate switch cases are a
    // compile error; the production table is not duplicated here as a second source of truth.
    const std::vector<std::pair<uint16_t, Model>> unionTable{
        // Upstream 0.7.0 additions.
        {0x2002, Model::AirPods_1},
        {0x200F, Model::AirPods_2},
        {0x2013, Model::AirPods_3},
        {0x2019, Model::AirPods_4},
        {0x201B, Model::AirPods_4_ANC},
        {0x200E, Model::AirPods_Pro},
        {0x2014, Model::AirPods_Pro_2},
        {0x200A, Model::AirPods_Max},
        {0x201F, Model::AirPods_Max_USB_C},
        {0x202D, Model::AirPods_Max_USB_C}, // AirPods Max 2, upstream PR #251 (fix c543222).
        {0x2012, Model::Beats_Fit_Pro},
        {0x2003, Model::Powerbeats_3}, // Upstream fix 0ff4d72.
        {0x2005, Model::Beats_X},
        {0x2006, Model::Beats_Solo3},
        // Fork additions preserved across the upstream merge.
        {0x2024, Model::AirPods_Pro_2_USB_C},
        {0x2027, Model::AirPods_Pro_3},
        {0x201D, Model::Powerbeats_Pro_2}, // winpods apple_cp/mod.rs + Hebrew/PBP commit.
        {0x2030, Model::AirPods_5}, // Family capture; WCC requires a correlated model number.
        {0x2036, Model::AirPods_5}, // Coarse fork mapping, no WCC capture.
    };
    for (const auto &[productId, expected] : unionTable) {
        QCOMPARE(Core::AppleCP::AirPods::GetModel(productId), expected);
    }

    // Unknown ids must degrade to Model::Unknown rather than pick a nearest neighbour.
    for (const auto probe :
         {uint16_t{0x2011}, uint16_t{0x2016}, uint16_t{0x2026}, uint16_t{0x200B}, uint16_t{0x0000},
          uint16_t{0xFFFF}})
    {
        QCOMPARE(Core::AppleCP::AirPods::GetModel(probe), Model::Unknown);
    }
}

void AirPodsDomainTests::ResolvesAirPodsMaxUsbCDisplayName()
{
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName("AirPods Max", Model::AirPods_Max_USB_C),
        QString{"AirPods Max (USB-C)"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName(
            "Studio AirPods Max - Find My", Model::AirPods_Max_USB_C),
        QString{"Studio AirPods Max"});
    QCOMPARE(
        Core::AirPods::Details::ResolveDisplayName("AirPods Max", Model::AirPods_Max),
        QString{"AirPods Max"});
}

void AirPodsDomainTests::RecognizesHeadTrackingPackets()
{
    auto packet = MakeHeadTrackingPacket();
    QCOMPARE(Gui::ClassifyHeadTrackingRecord(packet), Gui::HeadTrackingRecord::Stream);
    const auto counts = Gui::ReadHeadTrackingCounts(packet);
    QVERIFY(counts.has_value());
    QCOMPARE(counts->orientation[0], std::int16_t{-32768});
    QCOMPARE(counts->verticalAcceleration, std::int16_t{-42});

    packet[10] = 0x45;
    QCOMPARE(Gui::ClassifyHeadTrackingRecord(packet), Gui::HeadTrackingRecord::Stream);
    packet[10] = 0x99;
    QCOMPARE(Gui::ClassifyHeadTrackingRecord(packet), Gui::HeadTrackingRecord::Stream);

    Gui::HeadTrackingViewModel viewModel;
    const Gui::HeadTrackingViewModel::TimePoint now{};
    viewModel.SetEnabled(true, now);
    viewModel.OnTransportUp(7, now);
    auto view = viewModel.OnPacket(packet, 7, now);
    QVERIFY(view.has_value());
    QCOMPARE(view->sample.raw.horizontalAcceleration, std::int16_t{1234});

    packet.resize(55);
    QCOMPARE(Gui::ClassifyHeadTrackingRecord(packet), Gui::HeadTrackingRecord::NotHeadTracking);
    QVERIFY(!Gui::ReadHeadTrackingCounts(packet).has_value());
    QVERIFY(!viewModel.OnPacket(packet, 7, now).has_value());

    packet = MakeHeadTrackingPacket();
    packet[4] = 0x16;
    QCOMPARE(Gui::ClassifyHeadTrackingRecord(packet), Gui::HeadTrackingRecord::NotHeadTracking);
    packet = MakeHeadTrackingPacket();
    packet[8] = 0x11;
    QCOMPARE(Gui::ClassifyHeadTrackingRecord(packet), Gui::HeadTrackingRecord::NotHeadTracking);
}

void AirPodsDomainTests::HeadTrackingPresentationAgreesWithCoreParser()
{
    const auto packet = MakeHeadTrackingPacket();
    const auto counts = Gui::ReadHeadTrackingCounts(packet);
    const auto parsed = Core::AAP::ParseHeadTrackingData(packet);
    QVERIFY(counts.has_value());
    QVERIFY(parsed.has_value());
    QCOMPARE(counts->orientation[0], parsed->orientation1);
    QCOMPARE(counts->orientation[1], parsed->orientation2);
    QCOMPARE(counts->orientation[2], parsed->orientation3);
    QCOMPARE(counts->horizontalAcceleration, parsed->horizontalAcceleration);
    QCOMPARE(counts->verticalAcceleration, parsed->verticalAcceleration);

    const auto sample = Gui::NormalizeHeadTrackingSample(*counts);
    QCOMPARE(sample.orientation[0], -1.0);
    QCOMPARE(sample.orientation[1], -2.0 / 32768.0);
    QVERIFY(sample.orientation[2] > 0.999 && sample.orientation[2] < 1.0);
    QCOMPARE(sample.horizontalAcceleration, 1234.0 / 32768.0);
    QCOMPARE(sample.verticalAcceleration, -42.0 / 32768.0);
}

void AirPodsDomainTests::HeadTrackingViewModelThrottlesAndTracksGenerations()
{
    using ViewModel = Gui::HeadTrackingViewModel;
    const ViewModel::TimePoint start{};
    ViewModel model;
    Gui::HeadTrackingCounts counts{{1, 2, 3}, 4, 5};
    model.SetEnabled(true, start);
    model.OnTransportUp(7, start);

    auto view = model.OnCounts(counts, 7, Gui::HeadTrackingRecord::Stream, start);
    QVERIFY(view.has_value());
    QCOMPARE(view->phase, Gui::HeadTrackingPhase::Streaming);
    QCOMPARE(view->acceptedRecords, uint32_t{1});
    QCOMPARE(view->publishedUpdates, uint32_t{1});

    counts.orientation[0] = 9;
    QVERIFY(!model.OnCounts(counts, 7, Gui::HeadTrackingRecord::Stream, start + 50ms));
    QVERIFY(!model.OnCounts(counts, 7, Gui::HeadTrackingRecord::Stream, start + 99ms));
    view = model.OnCounts(counts, 7, Gui::HeadTrackingRecord::Stream, start + 100ms);
    QVERIFY(view.has_value());
    QCOMPARE(view->sample.orientation[0], 9.0 / 32768.0);
    QCOMPARE(view->acceptedRecords, uint32_t{4});
    QCOMPARE(view->publishedUpdates, uint32_t{2});

    QVERIFY(!model.OnCounts(counts, 8, Gui::HeadTrackingRecord::Stream, start + 101ms));
    QCOMPARE(model.Present(start + 101ms).acceptedRecords, uint32_t{4});

    model.OnTransportUp(8, start + 200ms);
    QVERIFY(!model.Present(start + 200ms).hasSample);
    model.OnTransportDown(7, start + 201ms);
    QCOMPARE(model.Phase(start + 201ms), Gui::HeadTrackingPhase::Armed);
    QVERIFY(!model.OnCounts(counts, 7, Gui::HeadTrackingRecord::Stream, start + 201ms));
    QVERIFY(model.OnCounts(counts, 8, Gui::HeadTrackingRecord::Stream, start + 201ms).has_value());
    QVERIFY(!model.Tick(start + 950ms).has_value());
    const auto stale = model.Tick(start + 951ms);
    QVERIFY(stale.has_value());
    QCOMPARE(stale->phase, Gui::HeadTrackingPhase::Stale);

    model.SetEnabled(false, start + 1000ms);
    QCOMPARE(model.Phase(start + 1000ms), Gui::HeadTrackingPhase::Off);
    QVERIFY(!model.Present(start + 1000ms).hasSample);
    QVERIFY(!model.OnCounts(counts, 8, Gui::HeadTrackingRecord::Stream, start + 1001ms));
    model.SetEnabled(true, start + 1002ms);
    QCOMPARE(model.Phase(start + 1002ms), Gui::HeadTrackingPhase::Armed);
    QVERIFY(model.OnCounts(counts, 8, Gui::HeadTrackingRecord::Stream, start + 1002ms).has_value());
    model.OnTransportDown(8, start + 1003ms);
    QCOMPARE(model.Phase(start + 1003ms), Gui::HeadTrackingPhase::LinkDown);
}

void AirPodsDomainTests::HeadTrackingIntentReplaysStartAndStop()
{
    using Intent = Core::AAP::HeadTrackingIntent;
    RecordingHeadTrackingPort port;
    Intent intent;
    intent.SetEnabled(true);
    QCOMPARE(intent.Flush(port), Intent::Action::None);
    QVERIFY(port.requests.empty());

    intent.OnTransportUp(0);
    QVERIFY(!intent.LinkUp());
    intent.OnTransportUp(7);
    port.succeed = false;
    QCOMPARE(intent.Flush(port), Intent::Action::None);
    QVERIFY(intent.IsPending());
    QCOMPARE(port.requests, (std::vector<bool>{true}));

    port.succeed = true;
    QCOMPARE(intent.Flush(port), Intent::Action::Start);
    QCOMPARE(intent.Applied(), std::optional<bool>{true});
    QVERIFY(!intent.IsPending());
    QCOMPARE(intent.Flush(port), Intent::Action::None);

    intent.SetEnabled(false);
    QCOMPARE(intent.Flush(port), Intent::Action::Stop);
    QCOMPARE(intent.Applied(), std::optional<bool>{false});
    QCOMPARE(intent.Flush(port), Intent::Action::None);

    intent.OnTransportDown();
    QVERIFY(!intent.LinkUp());
    QVERIFY(!intent.Applied().has_value());
    intent.OnTransportUp(8);
    // A fresh device session starts with the stream off, so a disabled intent needs no packet.
    QCOMPARE(intent.Flush(port), Intent::Action::None);
    QCOMPARE(port.requests.size(), size_t{3});

    intent.SetEnabled(true);
    QCOMPARE(intent.Flush(port), Intent::Action::Start);
    intent.OnTransportDown();
    intent.OnTransportUp(9);
    QCOMPARE(intent.Flush(port), Intent::Action::Start);
    QCOMPARE(port.requests, (std::vector<bool>{true, true, false, true, true}));
    intent.OnTransportUp(9);
    QCOMPARE(intent.Flush(port), Intent::Action::None);
    intent.SetEnabled(false);
    QCOMPARE(intent.Flush(port), Intent::Action::Stop);
    QCOMPARE(port.requests, (std::vector<bool>{true, true, false, true, true, false}));
}

void AirPodsDomainTests::HeadTrackingSettingPersistsWithoutAbiBump()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("popup_on_lid_open", true);
    auto *memoryRepository = repository.get();
    Core::Settings::SetRepository(std::move(repository));
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::kFieldsAbiVersion, uint32_t{1});
    QVERIFY(!Core::Settings::GetCurrent().head_tracking);

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    QVERIFY(!observer.headTrackingEnabled);
    QCOMPARE(observer.headTrackingChanges, 1);
    {
        auto settings = Core::Settings::ModifiableAccess();
        settings->head_tracking = true;
    }
    QVERIFY(observer.headTrackingEnabled);
    QCOMPARE(observer.headTrackingChanges, 2);
    QVERIFY(memoryRepository->Read("head_tracking").toBool());
    QVERIFY(memoryRepository->Read("popup_on_lid_open").toBool());
    Core::Settings::SetApplyObserver(nullptr);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::PackagesCompatibleLowLatencySilence()
{
    QFile qrc{QString{APD_SOURCE_DIR} + "/Source/Resource/Resource.qrc"};
    QVERIFY(qrc.open(QIODevice::ReadOnly | QIODevice::Text));

    const auto qrcContents = QString::fromUtf8(qrc.readAll());
    QVERIFY(qrcContents.contains("<file>Audio/Silence.mp3</file>"));
    QVERIFY(QFile::exists(QString{APD_SOURCE_DIR} + "/Source/Resource/Audio/Silence.mp3"));
}

void AirPodsDomainTests::ParsesAdvertisementState()
{
    const Advertisement advertisement{MakeAdvertisementData(0x1234, -45, Side::Left)};
    const auto &state = advertisement.GetAdvState();

    QCOMPARE(state.model, Model::AirPods_Pro_2);
    QCOMPARE(state.side, Side::Left);
    QCOMPARE(state.pods.left.battery.Value(), 80U);
    QCOMPARE(state.pods.right.battery.Value(), 70U);
    QCOMPARE(state.caseBox.battery.Value(), 50U);
    QVERIFY(state.pods.left.isInEar);
    QVERIFY(state.pods.right.isInEar);
    QVERIFY(state.caseBox.isBothPodsInCase);
    QVERIFY(state.caseBox.isLidOpened);
}

void AirPodsDomainTests::NormalizesSingleBatteryModels()
{
    for (const auto model :
         {Model::AirPods_Max, Model::AirPods_Max_USB_C, Model::Powerbeats_3, Model::Beats_X,
          Model::Beats_Solo3})
    {
        QVERIFY(Core::AirPods::HasSingleBattery(model));
        QVERIFY(!Core::AirPods::HasChargingCase(model));
        for (const auto side : {Side::Left, Side::Right}) {
            const uint8_t left = side == Side::Left ? 6 : 15;
            const uint8_t right = side == Side::Left ? 15 : 6;
            const Advertisement advertisement{MakeAdvertisementData(
                0x1234, -45, side, left, right, 9,
                model == Model::AirPods_Max         ? 0x200A
                : model == Model::AirPods_Max_USB_C ? 0x201F
                : model == Model::Powerbeats_3      ? 0x2003
                : model == Model::Beats_X           ? 0x2005
                                                    : 0x2006)};
            const auto &state = advertisement.GetAdvState();
            QCOMPARE(state.pods.left.battery.Value(), 60U);
            QCOMPARE(state.pods.right.battery.Value(), 60U);
            QVERIFY(!state.caseBox.battery.Available());
            QVERIFY(!state.caseBox.isCharging);
            if (Core::AirPods::IsW1Beats(model)) {
                QVERIFY(!Core::AirPods::ReportsLidState(model));
                QVERIFY(!Core::AirPods::HasEarDetection(model));
                QVERIFY(!state.caseBox.isBothPodsInCase);
                QVERIFY(!state.caseBox.isLidOpened);
                QVERIFY(!state.pods.left.isInCase && !state.pods.right.isInCase);
                QVERIFY(!state.pods.left.isInEar && !state.pods.right.isInEar);
            }
            else {
                // Max keeps the fork's historical lid/card behavior despite having no case.
                QVERIFY(Core::AirPods::ReportsLidState(model));
                QVERIFY(state.caseBox.isBothPodsInCase);
                QVERIFY(state.caseBox.isLidOpened);
            }
        }
    }
    QVERIFY(Core::AirPods::HasChargingCase(Model::AirPods_Pro_2));
    QVERIFY(Core::AirPods::HasEarDetection(Model::AirPods_Pro_2));
}

void AirPodsDomainTests::FiltersDuplicateAndWeakAdvertisements()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    auto first =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1234, -45, Side::Left)});
    QVERIFY(first.has_value());
    QVERIFY(!first->oldState.has_value());

    auto duplicate =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1234, -45, Side::Left)});
    QVERIFY(!duplicate.has_value());

    auto weak = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x1234, -90, Side::Left, 7, 7, 5)});
    QVERIFY(!weak.has_value());
    QCOMPARE(manager.GetCurrentState()->pods.left.battery.Value(), 80U);
}

void AirPodsDomainTests::AcceptsWeakAndUnknownRssiFromTrackedAddress()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);
    QVERIFY(manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xAAAA, -76, 0x2B)})
                .has_value());

    auto weak = manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xAAAA, -88, 0x23)});
    QVERIFY(weak.has_value());
    QVERIFY(weak->newState.pods.left.isInEar);
    QVERIFY(!weak->newState.pods.right.isInEar);

    auto unknown =
        manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xAAAA, -127, 0x29)});
    QVERIFY(unknown.has_value());
    QVERIFY(!unknown->newState.pods.left.isInEar);
    QVERIFY(unknown->newState.pods.right.isInEar);

    QVERIFY(!manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xBBBB, -127, 0x2B)})
                 .has_value());
    QVERIFY(!manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xBBBB, -90, 0x2B)})
                 .has_value());

    // An unknown RSSI cannot prove that another source is stronger or farther away.
    auto otherSide = manager.OnAdvReceived(
        Advertisement{MakeTraceAdvertisementData(0xCCCC, -70, 0x0B, 0x68, 0x88)});
    QVERIFY(otherSide.has_value());
    QCOMPARE(manager.GetCurrentState()->pods.left.battery.Value(), 60U);
}

void AirPodsDomainTests::RemembersSideAddressAcrossStateReset()
{
    StateManager manager{{.lost = 10s, .stateReset = 50ms}};
    manager.OnRssiMinChanged(-80);
    QVERIFY(manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xAAAA, -76, 0x2B)})
                .has_value());
    QTest::qSleep(250);

    auto weak = manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xAAAA, -88, 0x21)});
    QVERIFY(weak.has_value());
    QVERIFY(!weak->newState.pods.left.isInEar);
    QVERIFY(!weak->newState.pods.right.isInEar);

    manager.Disconnect();
    QVERIFY(!manager.OnAdvReceived(Advertisement{MakeTraceAdvertisementData(0xAAAA, -88, 0x2B)})
                 .has_value());
}

void AirPodsDomainTests::RejectsConflictingBatterySources()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);
    auto left = MakeAdvertisementData(0x1111, -72, Side::Left, 4, 15, 15, 0x201B);
    left.manufacturerDataMap.at(Core::AppleCP::VendorId)[5] = 0x22;
    auto right = MakeAdvertisementData(0x2222, -55, Side::Right, 9, 9, 15, 0x201B);
    right.manufacturerDataMap.at(Core::AppleCP::VendorId)[5] = 0x0B;
    QVERIFY(manager.OnAdvReceived(Advertisement{right}).has_value());
    for (int i = 0; i < 5; ++i) {
        QVERIFY(!manager.OnAdvReceived(Advertisement{left}).has_value());
        manager.OnAdvReceived(Advertisement{right});
        QCOMPARE(manager.GetCurrentState()->pods.left.battery.Value(), 90U);
        QCOMPARE(manager.GetCurrentState()->pods.right.battery.Value(), 90U);
    }

    // A genuine update from the selected source is still accepted.
    right.manufacturerDataMap.at(Core::AppleCP::VendorId)[6] = 0x89;
    manager.OnAdvReceived(Advertisement{right});
    QCOMPARE(manager.GetCurrentState()->pods.left.battery.Value(), 80U);
}

void AirPodsDomainTests::RecoversFromWeakerBatterySource()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);
    auto partial = MakeAdvertisementData(0x1111, -72, Side::Left, 4, 15, 15, 0x201B);
    auto complete = MakeAdvertisementData(0x2222, -50, Side::Right, 9, 9, 5, 0x201B);
    complete.manufacturerDataMap.at(Core::AppleCP::VendorId)[7] |= 0x70;
    QVERIFY(manager.OnAdvReceived(Advertisement{partial}).has_value());
    auto update = manager.OnAdvReceived(Advertisement{complete});
    QVERIFY(update.has_value());
    QCOMPARE(update->newState.pods.left.battery.Value(), 90U);
    QCOMPARE(update->newState.pods.right.battery.Value(), 90U);
    QCOMPARE(update->newState.caseBox.battery.Value(), 50U);
    QVERIFY(update->newState.pods.left.isCharging);
    QVERIFY(update->newState.pods.right.isCharging);
    QVERIFY(update->newState.caseBox.isCharging);
    QVERIFY(!manager.OnAdvReceived(Advertisement{partial}).has_value());
}

void AirPodsDomainTests::AcceptsAdjacentBatteryBucketsAfterAddressRotation()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);
    manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1111, -50, Side::Right, 8, 7, 5)});
    auto rotated = MakeAdvertisementData(0x2222, -51, Side::Right, 9, 8, 6);
    rotated.manufacturerDataMap.at(Core::AppleCP::VendorId)[7] |= 0x70;
    auto update = manager.OnAdvReceived(Advertisement{rotated});
    QVERIFY(update.has_value());
    QCOMPARE(update->newState.pods.left.battery.Value(), 90U);
    QCOMPARE(update->newState.pods.right.battery.Value(), 80U);
    QCOMPARE(update->newState.caseBox.battery.Value(), 60U);
}

void AirPodsDomainTests::MergesAdvertisementsFromBothSides()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    QVERIFY(
        manager
            .OnAdvReceived(Advertisement{MakeAdvertisementData(0x1111, -45, Side::Left, 8, 6, 4)})
            .has_value());

    auto update = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x2222, -46, Side::Right, 9, 7, 5)});
    QVERIFY(update.has_value());
    QVERIFY(update->oldState.has_value());
    QCOMPARE(update->newState.pods.left.battery.Value(), 90U);
    QCOMPARE(update->newState.pods.right.battery.Value(), 70U);
    QCOMPARE(update->newState.caseBox.battery.Value(), 50U);
}

// D2: battery fusion. The Manager copies the same Resolve() answers into State;
// these tests pin the pure policy the overlay is built from.
void AirPodsDomainTests::FusesAapBatteryOverBleWithinSession()
{
    Core::AirPods::Details::BatteryFusion fusion;

    // Before any AAP data the fusion presents nothing: the BLE path is untouched.
    QVERIFY(
        !fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90)).has_value());
    QVERIFY(!fusion.Presents(Core::AAP::BatteryComponent::Left));

    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(87)));
    auto left = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90));
    QVERIFY(left.has_value());
    QCOMPARE(left->level, uint8_t(87));
    QVERIFY(!left->conflict); // 3 points apart stays inside one advertisement bucket

    // Components AAP never mentioned keep falling back to BLE. Omitted components keep
    // their presented value, but a pending drop candidate requires adjacent snapshots.
    QVERIFY(
        !fusion.Resolve(Core::AAP::BatteryComponent::Case, std::optional<uint8_t>(50)).has_value());
    Core::AAP::AAPBatterySnapshot caseOnly;
    caseOnly.caseBox =
        AapReading(Core::AAP::BatteryComponent::Case, 42, Core::AAP::BatteryStatus::Discharging);
    QVERIFY(fusion.OnAapSnapshot(caseOnly));
    QVERIFY(fusion.Presents(Core::AAP::BatteryComponent::Left));
    QVERIFY(fusion.Resolve(Core::AAP::BatteryComponent::Right, std::nullopt) == std::nullopt);
    auto stored = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->level, uint8_t(87));
    QVERIFY(!stored->conflict); // no BLE value cannot conflict

    // Feeding the identical snapshot changes nothing (no republish churn).
    QVERIFY(!fusion.OnAapSnapshot(caseOnly));
}

void AirPodsDomainTests::FallsBackWhenAapComponentDisconnects()
{
    Core::AirPods::Details::BatteryFusion fusion;
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(87)));
    QVERIFY(fusion.Presents(Core::AAP::BatteryComponent::Left));

    Core::AAP::AAPBatterySnapshot disconnected;
    disconnected.left =
        AapReading(Core::AAP::BatteryComponent::Left, 0, Core::AAP::BatteryStatus::Disconnected);
    QVERIFY(fusion.OnAapSnapshot(disconnected));
    QVERIFY(!fusion.Presents(Core::AAP::BatteryComponent::Left));
    QVERIFY(
        !fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90)).has_value());

    // Reconnecting starts a fresh presentation (immediate acceptance again).
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(85)));
    auto restored = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90));
    QVERIFY(restored.has_value());
    QCOMPARE(restored->level, uint8_t(85));
}

void AirPodsDomainTests::FallsBackWhenAapSessionResets()
{
    Core::AirPods::Details::BatteryFusion fusion;
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(87)));

    // AAP disconnect (or a new session) drops every overlay without a time window.
    fusion.Reset();
    QVERIFY(!fusion.Presents(Core::AAP::BatteryComponent::Left));
    QVERIFY(
        !fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90)).has_value());

    // A pending large drop must not survive the reset either.
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(90)));
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(40)));
    fusion.Reset();
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(80))); // first value of the new session
    auto value = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt);
    QVERIFY(value.has_value());
    QCOMPARE(value->level, uint8_t(80));
}

void AirPodsDomainTests::ConfirmsLargeBatteryDropOnlyAfterTwoIdenticalReadings()
{
    Core::AirPods::Details::BatteryFusion fusion;
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(50)));

    // One 10-point drop is held: the presented value stays 50.
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(40)));
    auto held = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt);
    QVERIFY(held.has_value());
    QCOMPARE(held->level, uint8_t(50));

    // A different candidate restarts the confirmation count.
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(41)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(50));

    // Two consecutive identical candidates confirm the drop.
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(41)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(41));
    // Re-feeding the confirmed value changes nothing further.
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(41)));
}

void AirPodsDomainTests::HoldsFirstAapDropAgainstBleBaseline()
{
    using Fusion = Core::AirPods::Details::BatteryFusion;
    const Fusion::BleBaseline ble{.left = 90};

    Fusion held;
    QVERIFY(!held.OnAapSnapshot(LeftSnapshot(10), ble));
    QVERIFY(!held.Presents(Core::AAP::BatteryComponent::Left));
    QVERIFY(!held.Resolve(Core::AAP::BatteryComponent::Left, uint8_t{90}).has_value());
    QVERIFY(held.OnAapSnapshot(LeftSnapshot(10), ble));
    QCOMPARE(held.Resolve(Core::AAP::BatteryComponent::Left, uint8_t{90})->level, uint8_t(10));

    // A rise and a drop within the two-point guard still win immediately on an empty slot.
    Fusion rise;
    QVERIFY(rise.OnAapSnapshot(LeftSnapshot(95), ble));
    QCOMPARE(rise.Resolve(Core::AAP::BatteryComponent::Left, uint8_t{90})->level, uint8_t(95));
    Fusion smallDrop;
    QVERIFY(smallDrop.OnAapSnapshot(LeftSnapshot(88), ble));
    QCOMPARE(smallDrop.Resolve(Core::AAP::BatteryComponent::Left, uint8_t{90})->level, uint8_t(88));
}

void AirPodsDomainTests::ClearsPendingDropWhenComponentIsOmitted()
{
    Core::AirPods::Details::BatteryFusion fusion;
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(90)));
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(40)));

    Core::AAP::AAPBatterySnapshot caseOnly;
    caseOnly.caseBox =
        AapReading(Core::AAP::BatteryComponent::Case, 50, Core::AAP::BatteryStatus::Discharging);
    QVERIFY(fusion.OnAapSnapshot(caseOnly));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(90));

    // Consecutive means consecutive snapshots, not consecutive observations of this component.
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(40)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(90));
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(40)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(40));
}

void AirPodsDomainTests::HeadsetBatteryUsesLeftPresentationSlot()
{
    using Fusion = Core::AirPods::Details::BatteryFusion;
    using Component = Core::AAP::BatteryComponent;
    Fusion fusion;

    QVERIFY(!fusion.Presents(Component::Headset));
    QVERIFY(!fusion.Resolve(Component::Headset, std::optional<uint8_t>(90)).has_value());

    Core::AAP::AAPBatterySnapshot headset;
    headset.left = AapReading(Component::Headset, 73, Core::AAP::BatteryStatus::Charging);
    QVERIFY(fusion.OnAapSnapshot(headset));
    QVERIFY(fusion.Presents(Component::Headset));
    QVERIFY(fusion.Presents(Component::Left));
    QVERIFY(!fusion.Presents(Component::Right));
    QVERIFY(!fusion.Presents(Component::Case));

    const auto resolved = fusion.Resolve(Component::Headset, std::optional<uint8_t>(90));
    QVERIFY(resolved.has_value());
    QCOMPARE(resolved->level, uint8_t(73));
    QVERIFY(resolved->charging);
    QVERIFY(resolved->conflict);
    const auto left = fusion.Resolve(Component::Left, std::nullopt);
    QVERIFY(left.has_value());
    QCOMPARE(left->level, resolved->level);
    QCOMPARE(left->charging, resolved->charging);

    headset.left = AapReading(Component::Headset, 0, Core::AAP::BatteryStatus::Disconnected);
    QVERIFY(fusion.OnAapSnapshot(headset));
    QVERIFY(!fusion.Presents(Component::Headset));
    QVERIFY(!fusion.Presents(Component::Left));
    QVERIFY(!fusion.Resolve(Component::Headset, std::optional<uint8_t>(90)).has_value());
}

void AirPodsDomainTests::SerializesBleAndAapStatePublication()
{
    using Manager = Core::AirPods::Manager;
    Manager manager;
    Core::AirPods::State ble;
    ble.model = Model::AirPods_Pro_2;
    ble.displayName = QStringLiteral("AirPods Pro");
    ble.pods.left.battery = Core::AirPods::Battery{90};

    std::vector<uint8_t> published;
    bool pumpedQueuedEvents = false;
    QObject::connect(&manager, &Manager::StateUpdated, &manager, [&](const auto &) {
        // Reproduce a receiver that spins the event loop while the BLE signal is being
        // delivered. A direct BLE emit lets the queued AAP publish overtake it and then
        // leaves the stale BLE value as the last state observed by later receivers.
        if (!pumpedQueuedEvents) {
            pumpedQueuedEvents = true;
            QCoreApplication::processEvents();
        }
    });
    QObject::connect(&manager, &Manager::StateUpdated, &manager, [&](const auto &state) {
        if (state.pods.left.battery.Available()) {
            published.push_back(static_cast<uint8_t>(state.pods.left.battery.Value()));
        }
    });

    const auto aap = LeftSnapshot(88);
    const Core::AirPods::Details::BatteryFusion::BleBaseline baseline{.left = uint8_t{90}};
    QMetaObject::invokeMethod(
        &manager,
        [&] {
            {
                std::lock_guard lock{manager._batteryFusionMutex};
                manager._batteryFusion.OnAapSnapshot(aap, baseline);
            }
            manager.RequestFusedStatePublish();
        },
        Qt::QueuedConnection);

    Core::AirPods::Details::StateManager::UpdateEvent update{std::nullopt, ble};
    manager.OnStateChanged(std::move(update));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    // Both inputs now converge through Manager's owner-thread publisher. The first
    // delivery recomputes from BLE 90 + AAP 88, and the second is suppressed as duplicate.
    QCOMPARE(published.size(), size_t{1});
    QCOMPARE(published.front(), uint8_t{88});
}

void AirPodsDomainTests::SerializesFusedStatePublicationAcrossNestedEventLoop()
{
    using Manager = Core::AirPods::Manager;
    Manager manager;
    Core::AirPods::State older;
    older.pods.left.battery = Core::AirPods::Battery{80};
    Core::AirPods::State newer;
    newer.pods.left.battery = Core::AirPods::Battery{40};
    {
        std::lock_guard lock{manager._batteryFusionMutex};
        manager._lastBleState = older;
    }

    std::vector<uint8_t> firstReceiver;
    std::vector<uint8_t> secondReceiver;
    bool spunEventLoop = false;
    QObject::connect(&manager, &Manager::StateUpdated, &manager, [&](const auto &state) {
        if (state.pods.left.battery.Available()) {
            firstReceiver.push_back(static_cast<uint8_t>(state.pods.left.battery.Value()));
        }
        if (!spunEventLoop) {
            spunEventLoop = true;
            {
                std::lock_guard lock{manager._batteryFusionMutex};
                manager._lastBleState = newer;
            }
            manager.RequestFusedStatePublish();
            QCoreApplication::processEvents();
        }
    });
    QObject::connect(&manager, &Manager::StateUpdated, &manager, [&](const auto &state) {
        if (state.pods.left.battery.Available()) {
            secondReceiver.push_back(static_cast<uint8_t>(state.pods.left.battery.Value()));
        }
    });

    manager.PublishFusedState();
    QCoreApplication::processEvents();

    // The nested request used to publish 40 to the first receiver and then resume the
    // older 80 emission to the second receiver. Both receivers must now observe 80, 40.
    QVERIFY(spunEventLoop);
    QCOMPARE(firstReceiver.size(), size_t{2});
    QCOMPARE(secondReceiver.size(), size_t{2});
    QCOMPARE(firstReceiver[0], uint8_t{80});
    QCOMPARE(firstReceiver[1], uint8_t{40});
    QCOMPARE(secondReceiver[0], uint8_t{80});
    QCOMPARE(secondReceiver[1], uint8_t{40});
}

void AirPodsDomainTests::AcceptsSmallBatteryDropsAndRisesImmediately()
{
    Core::AirPods::Details::BatteryFusion fusion;
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(50)));

    // A 2-point drop is inside the immediate window.
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(48)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(48));

    // Rises never wait for confirmation, not even by more than two points.
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(97)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(97));

    // A rise while a drop candidate is pending clears the pending state.
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(30)));
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(96)));
    auto pendingCleared = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt);
    QCOMPARE(pendingCleared->level, uint8_t(96));
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(30)));
    QCOMPARE(fusion.Resolve(Core::AAP::BatteryComponent::Left, std::nullopt)->level, uint8_t(30));
}

void AirPodsDomainTests::MarksBatterySourceConflictBeyondBleBucket()
{
    Core::AirPods::Details::BatteryFusion fusion;
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(45)));

    // AAP 45 vs BLE 90: more than one 10% bucket apart, the feeds really disagree.
    auto conflicting =
        fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90));
    QVERIFY(conflicting->conflict);

    // AAP 91 vs BLE 90 is quantization noise, not a conflict.
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(91)));
    auto agreed = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90));
    QVERIFY(!agreed->conflict);

    // Exactly one bucket apart still counts as agreement (difference must EXCEED 10).
    QVERIFY(!fusion.OnAapSnapshot(LeftSnapshot(81))); // 10-point drop: still confirming
    QVERIFY(fusion.OnAapSnapshot(LeftSnapshot(81)));
    auto edge = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(90));
    QCOMPARE(edge->level, uint8_t(81));
    QVERIFY(!edge->conflict);
}

void AirPodsDomainTests::UsesAapStatusForBatteryCharging()
{
    Core::AirPods::Details::BatteryFusion fusion;
    Core::AAP::AAPBatterySnapshot charging;
    charging.left =
        AapReading(Core::AAP::BatteryComponent::Left, 60, Core::AAP::BatteryStatus::Charging);
    charging.right =
        AapReading(Core::AAP::BatteryComponent::Right, 60, Core::AAP::BatteryStatus::Discharging);
    charging.caseBox =
        AapReading(Core::AAP::BatteryComponent::Case, 60, Core::AAP::BatteryStatus::Unknown);
    QVERIFY(fusion.OnAapSnapshot(charging));

    auto left = fusion.Resolve(Core::AAP::BatteryComponent::Left, std::optional<uint8_t>(50));
    QVERIFY(left->charging);

    auto right = fusion.Resolve(Core::AAP::BatteryComponent::Right, std::optional<uint8_t>(50));
    QVERIFY(!right->charging);

    // Unknown AAP status is still authoritative: it does not claim charging.
    auto caseBox = fusion.Resolve(Core::AAP::BatteryComponent::Case, std::optional<uint8_t>(50));
    QVERIFY(!caseBox->charging);
}

void AirPodsDomainTests::BasicStateEqualityCoversBatterySourceFields()
{
    Core::AirPods::PodState first;
    first.battery = Core::AirPods::Battery{87};
    auto second = first;
    QVERIFY(first == second);

    second.batterySource = Core::AirPods::BatterySource::AAP;
    QVERIFY(!(first == second));

    second = first;
    second.sourceConflict = true;
    QVERIFY(!(first == second));

    // PodState/CaseState inherit the comparison through their defaulted operators.
    Core::AirPods::State leftState;
    leftState.pods.left = second;
    Core::AirPods::State rightState;
    QVERIFY(!(leftState == rightState));

    // Defaults describe the pure BLE path (backward compatible with pre-D2 states).
    QVERIFY(Core::AirPods::BatterySource::BLE == Core::AirPods::State{}.pods.left.batterySource);
    QVERIFY(!Core::AirPods::State{}.pods.left.sourceConflict);
    QVERIFY(!Core::AirPods::State{}.caseBox.sourceConflict);
}

void AirPodsDomainTests::PresentationCarriesBatterySourceFields()
{
    Core::AirPods::State state;
    state.model = Model::AirPods_Pro_2;
    state.displayName = QStringLiteral("AirPods Pro");
    state.pods.left.battery = Core::AirPods::Battery{87};
    state.pods.left.batterySource = Core::AirPods::BatterySource::AAP;
    state.pods.left.sourceConflict = true;
    state.pods.right.battery = Core::AirPods::Battery{85};
    state.caseBox.battery = Core::AirPods::Battery{50};

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    const auto presentation = viewModel.Present();

    QVERIFY(presentation.leftBattery.visible);
    QCOMPARE(presentation.leftBattery.source, Core::AirPods::BatterySource::AAP);
    QVERIFY(presentation.leftBattery.sourceConflict);
    QCOMPARE(presentation.rightBattery.source, Core::AirPods::BatterySource::BLE);
    QVERIFY(!presentation.rightBattery.sourceConflict);
    QVERIFY(presentation.leftBattery != presentation.rightBattery);

    // In the merged (both-out, equal-value) layout the shown ring keeps the weaker
    // component's source and reports either side's conflict.
    Core::AirPods::State merged = state;
    merged.pods.left.sourceConflict = false;
    merged.pods.left.battery = Core::AirPods::Battery{90};
    merged.pods.right.battery = Core::AirPods::Battery{90};
    merged.pods.right.batterySource = Core::AirPods::BatterySource::AAP;
    merged.pods.right.isCharging = true;
    merged.pods.left.isCharging = true;
    Gui::MainWindowViewModel mergedView;
    mergedView.UpdateState(merged);
    const auto mergedPresentation = mergedView.Present();
    QVERIFY(!mergedPresentation.leftBattery.visible); // merged into the pods row
    QCOMPARE(mergedPresentation.rightBattery.source, Core::AirPods::BatterySource::AAP);
    QVERIFY(!mergedPresentation.rightBattery.sourceConflict);
}

void AirPodsDomainTests::RejectsAdvertisementsFromDifferentModels()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    const auto first =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x1111, -45, Side::Left)});
    QVERIFY(first.has_value());
    QCOMPARE(first->newState.model, Model::AirPods_Pro_2);

    const auto differentModel = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x2222, -46, Side::Right, 8, 7, 5, 0x2013)});
    QVERIFY(!differentModel.has_value());
    QCOMPARE(manager.GetCurrentState()->model, Model::AirPods_Pro_2);
}

void AirPodsDomainTests::AcceptsKnownModelAfterUnknownAdvertisement()
{
    StateManager manager;
    manager.OnRssiMinChanged(-80);

    const auto unknown = manager.OnAdvReceived(
        Advertisement{MakeAdvertisementData(0x1111, -45, Side::Left, 8, 7, 5, 0xffff)});
    QVERIFY(unknown.has_value());
    QCOMPARE(unknown->newState.model, Model::Unknown);

    const auto known =
        manager.OnAdvReceived(Advertisement{MakeAdvertisementData(0x2222, -46, Side::Right)});
    QVERIFY(known.has_value());
    QCOMPARE(known->newState.model, Model::AirPods_Pro_2);
}

void AirPodsDomainTests::TracksLidAcrossShortGaps()
{
    using Lid = LidTracker::Lid;
    const LidTracker::Timestamp start{};
    LidTracker tracker;

    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start), std::optional<bool>{true});
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Opened, start + 1s), std::nullopt);
    tracker.OnLost(start + 40s);
    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start + 70s), std::nullopt);
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 80s), std::optional<bool>{false});
    tracker.OnLost(start + 90s);
    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start + 95s), std::optional<bool>{true});
    tracker.OnLost(start + 100s);
    QCOMPARE(
        tracker.Update(std::nullopt, Lid::Opened, start + 101s + LidTracker::kMemory),
        std::optional<bool>{true});
    tracker.OnLost(start + 200s);
    tracker.Reset();
    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start + 201s), std::optional<bool>{true});
}

void AirPodsDomainTests::TracksLidWhilePodsAreOut()
{
    using Lid = LidTracker::Lid;
    const LidTracker::Timestamp start{};
    LidTracker tracker;

    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start), std::optional<bool>{true});
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Unobservable, start + 1s), std::nullopt);
    QCOMPARE(tracker.Update(Lid::Unobservable, Lid::Unobservable, start + 2s), std::nullopt);
    QCOMPARE(
        tracker.Update(Lid::Unobservable, Lid::Closed, start + 3s), std::optional<bool>{false});
    QCOMPARE(tracker.Update(Lid::Closed, Lid::Unobservable, start + 4s), std::nullopt);
    // Pods back in with the lid still open is a re-open inside the popup cooldown (blueprint
    // 2.6): no second announcement.
    QCOMPARE(tracker.Update(Lid::Unobservable, Lid::Opened, start + 5s), std::nullopt);
    // A close still releases the popup while opens are suppressed.
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 6s), std::optional<bool>{false});
    // Once the cooldown measured from the announced popup has passed, the same transition
    // announces again.
    QCOMPARE(tracker.Update(Lid::Closed, Lid::Opened, start + 61s), std::optional<bool>{true});
    tracker.Reset();
    QCOMPARE(tracker.Update(std::nullopt, Lid::Closed, start + 62s), std::nullopt);
}

void AirPodsDomainTests::LidPopupCooldownBlocksRapidRepeats()
{
    using Lid = LidTracker::Lid;
    const LidTracker::Timestamp start{};
    LidTracker tracker;

    // The first lid open announces the popup and starts the 60 s cooldown.
    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start), std::optional<bool>{true});

    // Rapid open/close cycles inside the cooldown window announce nothing, while every close
    // still hides the visible popup.
    for (const auto offset : {10s, 20s, 30s, 45s}) {
        QCOMPARE(
            tracker.Update(Lid::Opened, Lid::Closed, start + offset), std::optional<bool>{false});
        QCOMPARE(tracker.Update(Lid::Closed, Lid::Opened, start + offset + 1s), std::nullopt);
    }

    // Suppressed re-opens do not extend the cooldown: from the announced popup at `start`, the
    // next open beyond 60 s pops again.
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 59s), std::optional<bool>{false});
    QCOMPARE(tracker.Update(Lid::Closed, Lid::Opened, start + 61s), std::optional<bool>{true});

    // An explicit unbind/rebind resets the tracker, so a freshly connected device is never
    // muted by the previous session's popup.
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 62s), std::optional<bool>{false});
    tracker.Reset();
    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start + 63s), std::optional<bool>{true});

    // Transient disconnects forget the lid baseline but retain the rate limit.
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 64s), std::optional<bool>{false});
    tracker.ForgetDevice();
    QCOMPARE(tracker.Update(std::nullopt, Lid::Opened, start + 65s), std::nullopt);

    // Disabled popups do not consume the cooldown; the first open after enabling announces.
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 66s), std::optional<bool>{false});
    QCOMPARE(tracker.Update(Lid::Closed, Lid::Opened, start + 67s, false), std::nullopt);
    QCOMPARE(tracker.Update(Lid::Opened, Lid::Closed, start + 68s), std::optional<bool>{false});
    QCOMPARE(tracker.Update(Lid::Closed, Lid::Opened, start + 124s), std::optional<bool>{true});
}

void AirPodsDomainTests::EarDetectionTrackerSurvivesStateLoss()
{
    const auto makeState = [](bool leftInEar, bool rightInEar) {
        Core::AirPods::State state;
        state.pods.left.isInEar = leftInEar;
        state.pods.right.isInEar = rightInEar;
        return state;
    };
    EarDetectionTracker tracker;
    QCOMPARE(tracker.Update(makeState(true, true)), std::optional<bool>{});
    QCOMPARE(tracker.Update(makeState(true, false)), std::optional<bool>{false});
    QCOMPARE(tracker.Update(makeState(true, false)), std::optional<bool>{});
    QCOMPARE(tracker.Update(makeState(false, false)), std::optional<bool>{});
    QCOMPARE(tracker.Update(makeState(true, true)), std::optional<bool>{true});
    tracker.Reset();
    QCOMPARE(tracker.Update(makeState(true, true)), std::optional<bool>{});
}

void AirPodsDomainTests::LoadsSettingsThroughRepository()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("auto_run", true);
    repository->Write("appearance_mode", QString{"Dark"});
    Core::Settings::SetRepository(std::move(repository));

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QVERIFY(Core::Settings::GetCurrent().auto_run);
    QCOMPARE(Core::Settings::GetCurrent().appearance_mode, Core::Settings::AppearanceMode::Dark);

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    QCOMPARE(observer.autoRunChanges, 1);
    QVERIFY(observer.autoRun);
    QCOMPARE(observer.appearanceModeChanges, 1);
    QCOMPARE(observer.appearanceMode, Core::Settings::AppearanceMode::Dark);

    Core::Settings::SetApplyObserver(nullptr);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::MigratesLegacySettingsRepository()
{
    Core::Settings::MemoryRepository current;
    Core::Settings::MemoryRepository legacy;
    legacy.Write("auto_run", true);
    legacy.Write("device_address", QVariant::fromValue<qulonglong>(0x12345678));
    legacy.Write("abi_version", Core::Settings::kFieldsAbiVersion);

    QVERIFY(Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QCOMPARE(current.Read("auto_run").toBool(), true);
    QCOMPARE(current.Read("device_address").toULongLong(), qulonglong{0x12345678});
    QCOMPARE(current.Read("abi_version").toUInt(), Core::Settings::kFieldsAbiVersion);
}

void AirPodsDomainTests::PreservesExistingSettingsDuringMigration()
{
    Core::Settings::MemoryRepository current;
    Core::Settings::MemoryRepository legacy;
    current.Write("auto_run", false);
    legacy.Write("abi_version", Core::Settings::kFieldsAbiVersion);
    legacy.Write("auto_run", true);
    legacy.Write("language_locale", QStringLiteral("zh_TW"));

    QVERIFY(Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QCOMPARE(current.Read("auto_run").toBool(), false);
    QCOMPARE(current.Read("language_locale").toString(), QStringLiteral("zh_TW"));

    current.Write("auto_run", true);
    QVERIFY(!Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QCOMPARE(current.Read("auto_run").toBool(), true);
}

void AirPodsDomainTests::RollsBackFailedSettingsMigration()
{
    FailingSyncRepository current;
    Core::Settings::MemoryRepository legacy;
    legacy.Write("abi_version", Core::Settings::kFieldsAbiVersion);
    legacy.Write("auto_run", true);

    QVERIFY(!Core::Settings::Details::MigrateLegacySettings(current, legacy));
    QVERIFY(current.Keys().isEmpty());
}

void AirPodsDomainTests::PersistsAndPropagatesStemActions()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    Core::Settings::SetRepository(std::move(repository));
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QVERIFY(
        Core::AAP::StemActionsOf(Core::Settings::GetCurrent()) == Core::AAP::DefaultStemActions());
    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::AAP::StemActionTable custom{};
    for (size_t index = 0; index < custom.size(); ++index) {
        custom[index] = static_cast<Core::AAP::StemAction>(index % 7);
    }
    {
        auto settings = Core::Settings::ModifiableAccess();
        settings->stem_left_single = custom[0];
        settings->stem_left_double = custom[1];
        settings->stem_left_triple = custom[2];
        settings->stem_left_long = custom[3];
        settings->stem_right_single = custom[4];
        settings->stem_right_double = custom[5];
        settings->stem_right_triple = custom[6];
        settings->stem_right_long = custom[7];
    }
    QVERIFY(observer.stemActions == custom);
    QVERIFY(observer.stemActionChanges > 0);
    Core::Settings::SetApplyObserver(nullptr);

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QVERIFY(Core::AAP::StemActionsOf(Core::Settings::GetCurrent()) == custom);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::PropagatesQuickConnectSettings()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("tray_quick_connect_enabled", true);
    repository->Write("tray_quick_connect_device_id", "{A}");
    Core::Settings::SetRepository(std::move(repository));

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    QVERIFY(observer.quickConnectEnabled);
    QCOMPARE(observer.quickConnectEnabledChanges, 1);
    QCOMPARE(observer.quickConnectDeviceId, QString{"{A}"});
    QCOMPARE(observer.quickConnectDeviceChanges, 1);

    Core::Settings::SetApplyObserver(nullptr);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::PreservesForkSettingsAndAddsPlaybackAutoConnect()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("automatic_ear_detection", false);
    repository->Write("conversational_awareness", true);
    repository->Write("conversational_awareness_volume_percent", uint8_t{55});
    repository->Write("personalized_volume", true);
    repository->Write("loud_sound_reduction", true);
    // Retired key: P5 keeps the field in SETTINGS_FIELDS as Impl::Deprecated() so loading an
    // old file stays Successful, but the value must never drive the new PPE cap.
    repository->Write("adaptive_transparency_level", uint8_t{35});
    repository->Write("noise_control_mode", uint32_t{3});
    repository->Write("tray_quick_connect_enabled", true);
    repository->Write("audio_auto_connect_enabled", true);
    auto *memoryRepository = repository.get();
    Core::Settings::SetRepository(std::move(repository));
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    QVERIFY(!observer.automaticEarDetection);
    QVERIFY(observer.conversationalAwareness);
    QCOMPARE(observer.conversationalAwarenessVolumePercent, uint8_t{55});
    QVERIFY(observer.personalizedVolume);
    QVERIFY(observer.loudSoundReduction);
    QCOMPARE(observer.ppeCapLevel, uint8_t{25});
    // D3 appends adaptive_noise_level: not in this legacy repository, so Apply must
    // propagate the compiled-in default without touching the ABI version.
    QCOMPARE(observer.adaptiveNoiseLevel, Core::AAP::kAdaptiveNoiseLevelDefault);
    QCOMPARE(observer.adaptiveNoiseLevelChanges, 1);
    QCOMPARE(observer.noiseControlMode, uint32_t{3});
    QVERIFY(observer.quickConnectEnabled);
    QVERIFY(observer.audioAutoConnectEnabled);
    QCOMPARE(observer.audioAutoConnectChanges, 1);
    QVERIFY(Core::Settings::GetCurrent().popup_on_lid_open);
    Core::Settings::SetApplyObserver(nullptr);

    // Saving must retain every pre-U2 key (except the retired PPE predecessor, which is
    // dropped on the next save), add both upstream keys plus ppe_cap_level, without an ABI bump.
    QCOMPARE(Core::Settings::kFieldsAbiVersion, uint32_t{1});
    Core::Settings::Save(Core::Settings::GetCurrent());
    const auto keys = memoryRepository->Keys();
    for (const auto &key :
         {"language_locale",
          "appearance_mode",
          "auto_run",
          "low_audio_latency",
          "automatic_ear_detection",
          "conversational_awareness",
          "conversational_awareness_volume_percent",
          "personalized_volume",
          "loud_sound_reduction",
          "ppe_cap_level",
          "noise_control_mode",
          "skipped_version",
          "rssi_min",
          "device_address",
          "tray_icon_battery",
          "tray_quick_connect_enabled",
          "tray_quick_connect_device_id",
          "battery_on_taskbar",
          "audio_auto_connect_enabled",
          "popup_on_lid_open",
          "adaptive_noise_level"})
    {
        QVERIFY2(keys.contains(key), key);
    }
    // The deprecated key is removed on save and never rewritten under the old name.
    QVERIFY(!keys.contains("adaptive_transparency_level"));
    QCOMPARE(memoryRepository->Read("ppe_cap_level").toUInt(), uint32_t{25});
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::KeepsPpeCapIndependentFromLegacyAdaptiveTransparency()
{
    // Whatever an old build persisted under adaptive_transparency_level (including the
    // 0-50 values its slider allowed), the new ppe_cap_level must start from its fixed
    // default 25: no migration, no scaling, no silent reuse.
    for (const uint8_t legacy :
         {uint8_t{0}, uint8_t{25}, uint8_t{50}, uint8_t{51}, uint8_t{100}, uint8_t{255}})
    {
        auto repository = std::make_unique<Core::Settings::MemoryRepository>();
        repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
        repository->Write("adaptive_transparency_level", legacy);
        auto *memory = repository.get();
        Core::Settings::SetRepository(std::move(repository));

        // ABI stays 1: the old key is deprecated, not renamed or retyped, so old files load.
        QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
        // Repeated loading is idempotent and never inherits the legacy value.
        QCOMPARE(Core::Settings::GetCurrent().ppe_cap_level, uint8_t{25});
        QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
        QCOMPARE(Core::Settings::GetCurrent().ppe_cap_level, uint8_t{25});

        RecordingSettingsObserver observer;
        Core::Settings::SetApplyObserver(&observer);
        Core::Settings::Apply();
        Core::Settings::SetApplyObserver(nullptr);
        QCOMPARE(observer.ppeCapLevel, uint8_t{25});

        // Saving retires the legacy key instead of reinterpreting it as the new cap.
        Core::Settings::Save(Core::Settings::GetCurrent());
        QVERIFY(!memory->Keys().contains("adaptive_transparency_level"));
        QCOMPARE(memory->Read("ppe_cap_level").toUInt(), uint32_t{25});
    }
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::AppendsPpeCapAfterExistingSettingsFields()
{
    Core::Settings::MetaFields fields;
    std::vector<std::string_view> names;
    boost::pfr::for_each_field(
        fields, [&](const auto &field) { names.push_back(field.GetName()); });

    QVERIFY(names.size() >= 2);
    const auto nameAt = [&](size_t index) {
        return QString::fromUtf8(names[index].data(), int(names[index].size()));
    };
    QCOMPARE(nameAt(names.size() - 2), QString{"popup_on_lid_open"});
    QCOMPARE(nameAt(names.size() - 1), QString{"ppe_cap_level"});
}

void AirPodsDomainTests::PersistsPpeCapLevelRoundTrip()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    auto *memory = repository.get();
    Core::Settings::SetRepository(std::move(repository));

    // Missing key means the fixed default, never a value borrowed from the legacy key.
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetDefault().ppe_cap_level, uint8_t{25});
    QCOMPARE(Core::Settings::GetCurrent().ppe_cap_level, uint8_t{25});

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);
    {
        auto access = Core::Settings::ModifiableAccess();
        access->ppe_cap_level = 80;
    } // destructor persists and applies the changed field only
    Core::Settings::SetApplyObserver(nullptr);
    QCOMPARE(memory->Read("ppe_cap_level").toUInt(), uint32_t{80});
    QCOMPARE(observer.ppeCapLevel, uint8_t{80});

    // Reload round-trip: the persisted value survives, the legacy key stays gone.
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().ppe_cap_level, uint8_t{80});
    QVERIFY(!memory->Keys().contains("adaptive_transparency_level"));

    // And it is forwarded to observers as-is on apply.
    Core::Settings::SetApplyObserver(&observer);
    Core::Settings::Apply();
    Core::Settings::SetApplyObserver(nullptr);
    QCOMPARE(observer.ppeCapLevel, uint8_t{80});

    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

// D3: adaptive_noise_level defaults to 50 when the key is absent, round-trips through the
// repository, notifies the observer once per applied change, and converges under repeated
// loads (idempotent), all while kFieldsAbiVersion stays at 1.
void AirPodsDomainTests::PersistsAdaptiveNoiseLevelDefaultAndReloads()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    auto *memoryRepository = repository.get();
    Core::Settings::SetRepository(std::move(repository));

    RecordingSettingsObserver observer;
    Core::Settings::SetApplyObserver(&observer);

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{50});
    Core::Settings::Apply();
    QCOMPARE(observer.adaptiveNoiseLevel, uint8_t{50});
    QCOMPARE(observer.adaptiveNoiseLevelChanges, 1);

    // A settings-window edit persists through the modified-accessor save path and reaches
    // the observer exactly once (changed-fields-only apply).
    Core::Settings::ModifiableAccess()->adaptive_noise_level = 70;
    QCOMPARE(observer.adaptiveNoiseLevel, uint8_t{70});
    QCOMPARE(observer.adaptiveNoiseLevelChanges, 2);
    QVERIFY(memoryRepository->Keys().contains("adaptive_noise_level"));
    QCOMPARE(memoryRepository->Read("adaptive_noise_level").toUInt(), uint{70});

    // Repeated loads converge on the stored value; nothing re-defaults or duplicates.
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{70});
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{70});
    QCOMPARE(Core::Settings::kFieldsAbiVersion, uint32_t{1});

    // Boundary round-trips: 0 and 100 are the documented range edges and must survive
    // Save/Load unchanged.
    for (const uint8_t level : {uint8_t{0}, uint8_t{100}}) {
        Core::Settings::Fields fields = Core::Settings::GetCurrent();
        fields.adaptive_noise_level = level;
        Core::Settings::Save(fields);
        QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, level);
        QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
        QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, level);
    }

    // Out-of-range disk values are normalized at the apply boundary before any observer
    // (and therefore the device path) sees them.
    memoryRepository->Write("adaptive_noise_level", uint8_t{200});
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    Core::Settings::Apply();
    QCOMPARE(observer.adaptiveNoiseLevel, uint8_t{100});
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{100});

    // Save and the modified-accessor path enforce the same range before persistence.
    auto fields = Core::Settings::GetCurrent();
    fields.adaptive_noise_level = 255;
    Core::Settings::Save(fields);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{100});
    QCOMPARE(memoryRepository->Read("adaptive_noise_level").toUInt(), uint{100});
    Core::Settings::ModifiableAccess()->adaptive_noise_level = 200;
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{100});
    QCOMPARE(memoryRepository->Read("adaptive_noise_level").toUInt(), uint{100});

    // Repeated Load must not retain a previous nondefault when the new repository omits the key
    // or stores a value that cannot be converted to uint8_t.
    memoryRepository->Write("adaptive_noise_level", uint8_t{70});
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{70});
    memoryRepository->Remove("adaptive_noise_level");
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{50});
    Core::Settings::Apply();
    QCOMPARE(observer.adaptiveNoiseLevel, uint8_t{50});

    memoryRepository->Write("adaptive_noise_level", QStringLiteral("not-a-level"));
    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QCOMPARE(Core::Settings::GetCurrent().adaptive_noise_level, uint8_t{50});
    Core::Settings::Apply();
    QCOMPARE(observer.adaptiveNoiseLevel, uint8_t{50});

    Core::Settings::SetApplyObserver(nullptr);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

// D3: the adaptive-strength description must exist in every shipped catalog, so the settings
// page never falls back to an untranslated source string for the strength control.
void AirPodsDomainTests::AdaptiveNoiseTranslationsAreCompleteInAllCatalogs()
{
    using Context = QString;
    struct Entry {
        QString translation;
        bool unfinished{true};
    };
    const std::array<Context, 1> contexts{QStringLiteral("QObject")};
    const std::array<const char *, 1> expected{
        "Sets Adaptive mode filtering strength (0-100): 0 lets more ambient noise through; 100 "
        "filters more. Available only while the connected device reports Adaptive mode."};
    const std::array<QString, 18> locales{"bg_BG", "de_DE", "es_ES", "fa_IR", "fr_FR", "he_IL",
                                          "id_ID", "it_IT", "ja_JP", "ko_KR", "pl_PL", "pt_BR",
                                          "pt_PT", "ru_RU", "tr_TR", "uk_UA", "zh_CN", "zh_TW"};

    const QDir translationDir{QStringLiteral(APD_SOURCE_DIR) "/Source/Resource/Translation"};
    QCOMPARE(
        translationDir.entryList({QStringLiteral("apd_*.ts")}, QDir::Files).size(), qsizetype{18});
    for (const auto &locale : locales) {
        QFile file{translationDir.filePath(QStringLiteral("apd_%1.ts").arg(locale))};
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        QXmlStreamReader xml{&file};
        Context context;
        QString source;
        QString translation;
        bool inSource = false;
        bool inTranslation = false;
        bool unfinished = true;
        QMap<QString, Entry> entries;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                if (xml.name() == QLatin1String("context")) {
                    context.clear();
                }
                else if (xml.name() == QLatin1String("message")) {
                    source.clear();
                    translation.clear();
                    unfinished = true;
                }
                else if (xml.name() == QLatin1String("source")) {
                    inSource = true;
                }
                else if (xml.name() == QLatin1String("translation")) {
                    inTranslation = true;
                    unfinished = xml.attributes().value(QLatin1String("type")) ==
                                 QLatin1String("unfinished");
                }
            }
            else if (xml.isEndElement()) {
                if (xml.name() == QLatin1String("source")) {
                    inSource = false;
                }
                else if (xml.name() == QLatin1String("translation")) {
                    inTranslation = false;
                }
                else if (xml.name() == QLatin1String("message") && contexts.contains(context)) {
                    entries.insert(source, Entry{translation, unfinished});
                }
            }
            else if (xml.isCharacters()) {
                if (inSource) {
                    source += xml.text().toString();
                }
                if (inTranslation) {
                    translation += xml.text().toString();
                }
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        for (const auto *sourceText : expected) {
            const QString key{QString::fromUtf8(sourceText)};
            const auto entry = entries.constFind(key);
            QVERIFY2(entry != entries.constEnd(), qPrintable(locale + ": missing " + key));
            QVERIFY2(!entry->unfinished, qPrintable(locale + ": unfinished " + key));
            QVERIFY2(
                !entry->translation.trimmed().isEmpty(), qPrintable(locale + ": empty " + key));
        }
    }
}

void AirPodsDomainTests::ClampsAndGatesAdaptiveNoiseLevelPolicy()
{
    static_assert(Core::AAP::kAdaptiveNoiseLevelDefault == 50, "");
    static_assert(Core::AAP::NormalizeAdaptiveNoiseLevel(0) == 0, "");
    static_assert(Core::AAP::NormalizeAdaptiveNoiseLevel(50) == 50, "");
    static_assert(Core::AAP::NormalizeAdaptiveNoiseLevel(100) == 100, "");
    static_assert(Core::AAP::NormalizeAdaptiveNoiseLevel(101) == 100, "");
    static_assert(Core::AAP::NormalizeAdaptiveNoiseLevel(255) == 100, "");

    // D3 UI is filtering strength: identity mapping at 0/50/100, never 100-value.
    static_assert(Core::AAP::FilteringStrengthToWire(0) == 0, "");
    static_assert(Core::AAP::FilteringStrengthToWire(50) == 50, "");
    static_assert(Core::AAP::FilteringStrengthToWire(100) == 100, "");
    static_assert(Core::AAP::FilteringStrengthFromWire(0) == 0, "");
    static_assert(Core::AAP::FilteringStrengthFromWire(50) == 50, "");
    static_assert(Core::AAP::FilteringStrengthFromWire(100) == 100, "");

    // Only the Adaptive wire value (0x04) arms the control; unknown modes never do.
    static_assert(!Core::AAP::IsAdaptiveStrengthAdjustable(std::nullopt), "");
    static_assert(!Core::AAP::IsAdaptiveStrengthAdjustable(uint8_t{1}), "");
    static_assert(!Core::AAP::IsAdaptiveStrengthAdjustable(uint8_t{3}), "");
    static_assert(Core::AAP::IsAdaptiveStrengthAdjustable(uint8_t{4}), "");

    // Commits need Adaptive plus a live AAP session; a mode switch alone (or a remembered
    // value on reconnect) must not push 0x2E - that is the C7 broker's replay job.
    static_assert(!Core::AAP::ShouldCommitAdaptiveNoiseLevel(uint8_t{4}, false), "");
    static_assert(!Core::AAP::ShouldCommitAdaptiveNoiseLevel(uint8_t{2}, true), "");
    static_assert(!Core::AAP::ShouldCommitAdaptiveNoiseLevel(std::nullopt, true), "");
    static_assert(Core::AAP::ShouldCommitAdaptiveNoiseLevel(uint8_t{4}, true), "");

    QCOMPARE(Core::AAP::NormalizeAdaptiveNoiseLevel(uint8_t{150}), uint8_t{100});
    QVERIFY(!Core::AAP::ShouldCommitAdaptiveNoiseLevel(std::nullopt, false));
}

void AirPodsDomainTests::RejectsNullSettingsRepository()
{
    auto repository = std::make_unique<Core::Settings::MemoryRepository>();
    repository->Write("abi_version", Core::Settings::kFieldsAbiVersion);
    repository->Write("auto_run", true);
    Core::Settings::SetRepository(std::move(repository));

    Core::Settings::SetRepository(nullptr);

    QCOMPARE(Core::Settings::Load(), Core::Settings::LoadResult::Successful);
    QVERIFY(Core::Settings::GetCurrent().auto_run);
    Core::Settings::SetRepository(Core::Settings::CreatePersistentRepository());
}

void AirPodsDomainTests::ParsesUpdateVersions()
{
    QCOMPARE(Core::Update::ToVersionNumber("v0.5.0"), QVersionNumber(0, 5, 0));
    QCOMPARE(Core::Update::ToVersionNumber("0.5.0"), QVersionNumber(0, 5, 0));
}

void AirPodsDomainTests::ParsesGitHubReleaseMetadata()
{
    const auto metadata = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "## Change log\n- Add automatic updates\n\nInstallation notes",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": [{
            "name": "AirPodsDesktop-0.5.0-win64.exe",
            "size": 123456,
            "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "browser_download_url": "%1/download/v0.5.0/AirPodsDesktop-0.5.0-win64.exe"
        }]
    })json"}
                              .arg(Config::UrlReleases);
    const auto release = Core::Update::Details::ParseSingleReleaseResponse(metadata.toStdString());

    QVERIFY(release.has_value());
    QCOMPARE(release->version, QVersionNumber(0, 5, 0));
    QCOMPARE(release->fileName, QString{"AirPodsDesktop-0.5.0-win64.exe"});
    QCOMPARE(release->fileSize, size_t{123456});
    QCOMPARE(
        release->sha256,
        QString{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"});
    QCOMPARE(release->changeLog, QString{"- Add automatic updates"});
    QVERIFY(release->CanAutoUpdate());
    QVERIFY(!release->isPreRelease);

    const auto generatedNotes = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "## What's Changed\n* Improve update handling\n\n**Full Changelog**: https://example.invalid/compare",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": []
    })json"}
                                    .arg(Config::UrlReleases);
    const auto generatedRelease =
        Core::Update::Details::ParseSingleReleaseResponse(generatedNotes.toStdString());

    QVERIFY(generatedRelease.has_value());
    QCOMPARE(generatedRelease->changeLog, QString{"* Improve update handling"});

    // Shaped after the real 0.4.2 release body: CRLF line endings and an emoji shortcode
    // between the hashes and the words.
    const auto decorated = QString{R"json({
        "tag_name": "0.4.2",
        "body": "Beta notice\r\n\r\n## :scroll: Change log\r\n1. Supported AirPods 4\r\n\r\nSorry",
        "html_url": "%1/tag/0.4.2",
        "prerelease": false,
        "assets": []
    })json"}
                               .arg(Config::UrlReleases);
    const auto decoratedRelease =
        Core::Update::Details::ParseSingleReleaseResponse(decorated.toStdString());

    QVERIFY(decoratedRelease.has_value());
    QCOMPARE(decoratedRelease->changeLog, QString{"1. Supported AirPods 4"});
}

void AirPodsDomainTests::RejectsUpdateAssetForAnotherArchitecture()
{
    const auto metadata = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "Change log\nLegacy bridge",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": [{
            "name": "AirPodsDesktop-0.5.0-win32-bridge.exe",
            "size": 123456,
            "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "browser_download_url": "%1/download/v0.5.0/AirPodsDesktop-0.5.0-win32-bridge.exe"
        }]
    })json"}
                              .arg(Config::UrlReleases);

    const auto release = Core::Update::Details::ParseSingleReleaseResponse(metadata.toStdString());
    QVERIFY(release.has_value());
    QVERIFY(release->fileName.isEmpty());
    QVERIFY(!release->CanAutoUpdate());
}

void AirPodsDomainTests::MatchesInstallerAssetsByArchitecture()
{
    using Core::Update::Details::IsCompatibleInstallerAsset;

    const auto installer = QStringLiteral("AirPodsDesktop-0.5.0-win64.exe");
    const auto portable = QStringLiteral("AirPodsDesktop-0.5.0-win64-portable.zip");
    const auto bridge = QStringLiteral("AirPodsDesktop-0.5.0-win32-bridge.exe");

    QVERIFY(IsCompatibleInstallerAsset(installer, QStringLiteral("win64")));
    QVERIFY(!IsCompatibleInstallerAsset(bridge, QStringLiteral("win64")));
    QVERIFY(!IsCompatibleInstallerAsset(portable, QStringLiteral("win64")));
    QVERIFY(IsCompatibleInstallerAsset(bridge, QStringLiteral("win32")));
    QVERIFY(!IsCompatibleInstallerAsset(installer, QStringLiteral("win32")));
}

void AirPodsDomainTests::RejectsReleaseMetadataFromAnotherRepository()
{
    const auto release = Core::Update::Details::ParseSingleReleaseResponse(R"json({
        "tag_name": "v9.9.9",
        "body": "Change log\nUntrusted release",
        "html_url": "https://github.com/AnotherOwner/AirPodsDesktop/releases/tag/v9.9.9",
        "prerelease": false,
        "assets": []
    })json");

    QVERIFY(!release.has_value());
}

void AirPodsDomainTests::RejectsUpdateAssetsWithoutDigest()
{
    const auto metadata = QString{R"json({
        "tag_name": "v0.5.0",
        "body": "Change log\nUnsigned asset",
        "html_url": "%1/tag/v0.5.0",
        "prerelease": false,
        "assets": [{
            "name": "AirPodsDesktop-0.5.0-win64.exe",
            "size": 123456,
            "browser_download_url": "%1/download/v0.5.0/AirPodsDesktop-0.5.0-win64.exe"
        }]
    })json"}
                              .arg(Config::UrlReleases);
    const auto release = Core::Update::Details::ParseSingleReleaseResponse(metadata.toStdString());

    QVERIFY(release.has_value());
    QVERIFY(!release->CanAutoUpdate());
}

void AirPodsDomainTests::VerifiesUpdateFileDigest()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QCOMPARE(file.write("test"), qint64{4});
    file.close();

    const QString expected = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";
    QVERIFY(Core::Update::Details::VerifyFileSha256(file.fileName(), expected));
    QVERIFY(!Core::Update::Details::VerifyFileSha256(
        file.fileName(), "0000000000000000000000000000000000000000000000000000000000000000"));
}

void AirPodsDomainTests::PresentsMainWindowLifecycleStates()
{
    Gui::MainWindowViewModel viewModel;

    auto presentation = viewModel.Present();
    QCOMPARE(presentation.title, QString{"Unavailable"});
    QCOMPARE(presentation.buttonAction, Gui::ButtonAction::NoButton);
    QVERIFY(!presentation.animationModel.has_value());

    viewModel.Available();
    QCOMPARE(viewModel.Present().title, QString{"Disconnected"});

    viewModel.Unbind();
    presentation = viewModel.Present();
    QCOMPARE(presentation.title, QString{"Waiting for Binding"});
    QCOMPARE(presentation.buttonAction, Gui::ButtonAction::Bind);

    viewModel.Disconnect();
    QCOMPARE(viewModel.Present(), presentation);

    viewModel.Bind();
    QCOMPARE(viewModel.Present().title, QString{"Disconnected"});
}

void AirPodsDomainTests::PresentsMainWindowDeviceState()
{
    Core::AirPods::State state;
    state.displayName = "Office AirPods";
    state.model = Core::AirPods::Model::AirPods_Pro_2;
    state.pods.left.battery = 80;
    state.pods.left.isCharging = true;
    state.pods.right.battery = 60;

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    const auto presentation = viewModel.Present();

    QCOMPARE(presentation.title, state.displayName);
    QCOMPARE(presentation.animationModel, std::optional{state.model});
    QVERIFY(presentation.leftBattery.visible);
    QVERIFY(presentation.leftBattery.charging);
    QCOMPARE(presentation.leftBattery.value, 80U);
    QVERIFY(presentation.rightBattery.visible);
    QCOMPARE(presentation.rightBattery.value, 60U);
    QVERIFY(!presentation.caseBattery.visible);
}

void AirPodsDomainTests::PresentsMainWindowCaseBattery()
{
    // Mirrors `PresentsMainWindowDeviceState`, which reports the pods but not the case.
    // Reporting only the case pins each slot to its own source.
    Core::AirPods::State state;
    state.displayName = "Office AirPods";
    state.model = Core::AirPods::Model::AirPods_4_ANC;
    state.caseBox.battery = 40;
    state.caseBox.isCharging = true;

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    auto presentation = viewModel.Present();

    QVERIFY(presentation.caseBattery.visible);
    QVERIFY(presentation.caseBattery.charging);
    QCOMPARE(presentation.caseBattery.value, 40U);
    QVERIFY(!presentation.leftBattery.visible);
    QVERIFY(!presentation.rightBattery.visible);

    state.caseBox.isCharging = false;
    viewModel.UpdateState(state);
    presentation = viewModel.Present();

    QVERIFY(presentation.caseBattery.visible);
    QVERIFY(!presentation.caseBattery.charging);
}

void AirPodsDomainTests::MergesMatchingPodBatteries()
{
    Core::AirPods::State state;
    state.model = Core::AirPods::Model::AirPods_Pro_2;
    state.pods.left.battery = 95;
    state.pods.right.battery = 95;
    state.caseBox.battery = 99;

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    auto presentation = viewModel.Present();

    QVERIFY(!presentation.leftBattery.visible);
    QVERIFY(presentation.rightBattery.visible);
    QCOMPARE(presentation.rightBattery.value, 95U);
    QCOMPARE(presentation.rightBattery.badge, Gui::BatteryBadge::None);
    QCOMPARE(presentation.caseBattery.badge, Gui::BatteryBadge::Case);

    state.caseBox.isBothPodsInCase = true;
    state.pods.left.isInCase = state.pods.right.isInCase = true;
    viewModel.UpdateState(state);
    QCOMPARE(viewModel.Present().caseBattery.badge, Gui::BatteryBadge::None);
    QCOMPARE(viewModel.Present().scene, Gui::Scene::PodsInCase);
    state.caseBox.isBothPodsInCase = false;
    state.pods.left.isInCase = state.pods.right.isInCase = false;

    state.pods.right.isCharging = true;
    viewModel.UpdateState(state);
    presentation = viewModel.Present();
    QVERIFY(presentation.leftBattery.visible);
    QCOMPARE(presentation.leftBattery.badge, Gui::BatteryBadge::Left);
    QCOMPARE(presentation.rightBattery.badge, Gui::BatteryBadge::Right);
}

void AirPodsDomainTests::PresentsScenesFromPodPlacement()
{
    Core::AirPods::State state;
    state.model = Core::AirPods::Model::AirPods_Pro_2;
    state.pods.left.battery = 80;
    state.pods.right.battery = 60;
    state.pods.right.isCharging = true;
    state.caseBox.battery = 99;

    Gui::MainWindowViewModel viewModel;

    state.pods.left.isInCase = state.pods.right.isInCase = true;
    state.caseBox.isBothPodsInCase = true;
    viewModel.UpdateState(state);
    auto presentation = viewModel.Present();
    QCOMPARE(presentation.scene, Gui::Scene::PodsInCase);
    QVERIFY(!presentation.leftBattery.visible);
    QCOMPARE(presentation.rightBattery.value, 60U);
    QCOMPARE(presentation.rightBattery.badge, Gui::BatteryBadge::None);
    QCOMPARE(presentation.caseBattery.badge, Gui::BatteryBadge::None);

    state.pods.right.isInCase = false;
    state.caseBox.isBothPodsInCase = false;
    viewModel.UpdateState(state);
    presentation = viewModel.Present();
    QCOMPARE(presentation.scene, Gui::Scene::OnePodOut);
    QVERIFY(presentation.leftBattery.visible && presentation.rightBattery.visible);
    QCOMPARE(presentation.caseBattery.badge, Gui::BatteryBadge::Case);

    state.pods.left.isInCase = false;
    viewModel.UpdateState(state);
    presentation = viewModel.Present();
    QCOMPARE(presentation.scene, Gui::Scene::BothPodsOut);
    QVERIFY(presentation.leftBattery.visible && presentation.rightBattery.visible);
    QCOMPARE(presentation.leftBattery.badge, Gui::BatteryBadge::Left);
    QCOMPARE(presentation.rightBattery.badge, Gui::BatteryBadge::Right);
    QVERIFY(presentation.caseBattery.visible);
    QCOMPARE(presentation.caseBattery.value, 99U);

    state.pods.left.isInEar = state.pods.right.isInEar = true;
    viewModel.UpdateState(state);
    QCOMPARE(viewModel.Present(), presentation);
}

void AirPodsDomainTests::CollapsesAirPodsMaxToSingleBattery()
{
    for (const auto model : {Model::AirPods_Max, Model::AirPods_Max_USB_C}) {
        QVERIFY(Core::AirPods::HasSingleBattery(model));
        QVERIFY(!Core::AirPods::HasChargingCase(model));
        Core::AirPods::State state;
        state.model = model;
        state.pods.left.battery = Core::AirPods::Battery{80};
        state.pods.right.battery = Core::AirPods::Battery{20};
        state.caseBox.battery = Core::AirPods::Battery{90};

        Gui::MainWindowViewModel viewModel;
        viewModel.UpdateState(state);
        const auto presentation = viewModel.Present();
        QCOMPARE(presentation.scene, Gui::Scene::Headset);
        QVERIFY(!presentation.leftBattery.visible);
        QVERIFY(presentation.rightBattery.visible);
        QCOMPARE(presentation.rightBattery.value, 80U);
        QCOMPARE(presentation.rightBattery.badge, Gui::BatteryBadge::None);
        QVERIFY(!presentation.caseBattery.visible);
    }
}

void AirPodsDomainTests::ParsesCapturedSpaceGrayAirPodsMax()
{
    constexpr std::array<uint8_t, 27> captured{
        0x07, 0x19, 0x01, 0x0a, 0x20, 0x22, 0x04, 0x80, 0x01, 0x0f, 0x45, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ReceivedData data;
    data.address = 0x1234;
    data.rssi = -60;
    data.manufacturerDataMap.emplace(
        Core::AppleCP::VendorId, std::vector<uint8_t>{captured.cbegin(), captured.cend()});
    const Advertisement advertisement{data};
    const auto &state = advertisement.GetAdvState();
    QCOMPARE(state.model, Model::AirPods_Max);
    QCOMPARE(state.color, Core::AppleCP::Color::MaxSpaceGray);
    QCOMPARE(state.pods.left.battery.Value(), 40u);
    QVERIFY(!state.caseBox.battery.Available());

    Gui::MainWindowViewModel viewModel;
    viewModel.UpdateState(state);
    const auto presentation = viewModel.Present();
    QCOMPARE(presentation.animationColor, Core::AppleCP::Color::MaxSpaceGray);
    QCOMPARE(
        Gui::GetAnimationPresentation(Model::AirPods_Max, presentation.animationColor).resource,
        QString{"qrc:/Resource/Video/AirPods_Max/15.avi"});

    // The parsed color must survive aggregation before the presentation layer can select a render.
    StateManager manager;
    manager.OnRssiMinChanged(-80);
    const auto update = manager.OnAdvReceived(advertisement);
    QVERIFY(update.has_value());
    QCOMPARE(update->newState.color, Core::AppleCP::Color::MaxSpaceGray);
}

void AirPodsDomainTests::PreservesForkAnimationMappingsAndColors()
{
    const auto airPods5 = Gui::GetAnimationPresentation(Model::AirPods_5);
    QCOMPARE(airPods5.resource, QString{"qrc:/Resource/Video/AirPods_4_ANC.avi"});

    const auto airPods5Wcc = Gui::GetAnimationPresentation(Model::AirPods_5_WCC);
    QCOMPARE(airPods5Wcc.resource, QString{"qrc:/Resource/Video/AirPods_4_ANC.avi"});

    const auto pro3 = Gui::GetAnimationPresentation(Model::AirPods_Pro_3);
    QCOMPARE(pro3.resource, QString{"qrc:/Resource/Video/AirPods_Pro_3.avi"});

    const auto powerbeatsPro2 = Gui::GetAnimationPresentation(Model::Powerbeats_Pro_2);
    QCOMPARE(powerbeatsPro2.resource, QString{"qrc:/Resource/Video/Beats_Fit_Pro.avi"});

    const auto max = Gui::GetAnimationPresentation(Model::AirPods_Max);
    QCOMPARE(max.resource, QString{"qrc:/Resource/Video/AirPods_Max/16.avi"});
    QCOMPARE(
        Gui::GetAnimationPresentation(Model::AirPods_Max, Core::AppleCP::Color::MaxSpaceGray)
            .resource,
        QString{"qrc:/Resource/Video/AirPods_Max/15.avi"});
    const auto maxUsbC = Gui::GetAnimationPresentation(Model::AirPods_Max_USB_C);
    QCOMPARE(maxUsbC.resource, QString{"qrc:/Resource/Video/AirPods_Max_USB_C/19.avi"});
    QCOMPARE(
        Gui::GetAnimationPresentation(
            Model::AirPods_Max_USB_C, static_cast<Core::AppleCP::Color>(22))
            .resource,
        QString{"qrc:/Resource/Video/AirPods_Max_USB_C/22.avi"});

    const std::vector<std::pair<Model, size_t>> colorCounts{
        {Model::AirPods_Max, 5},  {Model::AirPods_Max_USB_C, 5}, {Model::Beats_Fit_Pro, 14},
        {Model::Beats_Solo3, 28}, {Model::Powerbeats_3, 15},     {Model::Beats_X, 9},
    };
    for (const auto &[model, count] : colorCounts) {
        QCOMPARE(Gui::GetAnimationColors(model).size(), count);
    }
}

void AirPodsDomainTests::PlacesPopupInsideMultimonitorBounds()
{
    const QSize popup{320, 330};
    const QSize margin{24, 24};
    const QVector<QRect> screens{
        QRect{0, 0, 1920, 1040},       // primary monitor
        QRect{-1920, -200, 1280, 720}, // left and above the primary origin
        QRect{1920, 400, 2560, 1440},  // right and below the primary origin
        QRect{100, 100, 240, 180},     // smaller than the popup plus margins
    };
    const auto formatRect = [](const QRect &rect) {
        return QString{"x=%1, y=%2, width=%3, height=%4"}
            .arg(rect.x())
            .arg(rect.y())
            .arg(rect.width())
            .arg(rect.height());
    };
    for (const auto &screen : screens) {
        const auto position = Gui::PopupPosition(screen, popup, margin);
        const QRect placed{position, popup};
        QVERIFY2(placed.intersects(screen), qPrintable(formatRect(screen)));
        QVERIFY2(placed.left() >= screen.left(), qPrintable(formatRect(screen)));
        QVERIFY2(placed.top() >= screen.top(), qPrintable(formatRect(screen)));
        if (screen.width() >= popup.width() + margin.width()) {
            QCOMPARE(placed.right() + 1, screen.right() + 1 - margin.width());
        }
        if (screen.height() >= popup.height() + margin.height()) {
            QCOMPARE(placed.bottom() + 1, screen.bottom() + 1 - margin.height());
        }
    }
}

void AirPodsDomainTests::TranslationLocalesIncludeUpstreamAndForkLanguages()
{
    const auto locales =
        QString{Config::TranslationLocales}.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const auto *locale : {"es_ES", "fa_IR", "id_ID", "pl_PL", "he_IL"}) {
        QVERIFY2(
            locales.contains(locale),
            qPrintable(QString{"Missing translation locale: %1"}.arg(locale)));
    }
}

void AirPodsDomainTests::StemSettingsXmlExposesAllLabels()
{
    QFile ui{QString{APD_SOURCE_DIR} + "/Source/Gui/SettingsWindow.ui"};
    QVERIFY(ui.open(QIODevice::ReadOnly | QIODevice::Text));
    QXmlStreamReader xml{&ui};
    QStringList labels;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("string")) {
            labels << xml.readElementText();
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    for (const auto *source :
         {"Stem press actions",
          "Choose the Windows action for each press gesture. Changes are saved automatically.",
          "Single", "Double", "Triple", "Long", "Left stem", "Right stem"})
    {
        QVERIFY2(
            labels.contains(source), qPrintable(QString{"Missing stem UI label: %1"}.arg(source)));
    }
}

void AirPodsDomainTests::StemTranslationsAreFinished_data()
{
    QTest::addColumn<QString>("locale");
    const auto locales =
        QString{Config::TranslationLocales}.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    QCOMPARE(locales.size(), 18);
    for (const auto &locale : locales) {
        QTest::newRow(qPrintable(locale)) << locale;
    }
}

void AirPodsDomainTests::StemTranslationsAreFinished()
{
    QFETCH(QString, locale);
    const std::array<QString, 23> expectedSources{
        QStringLiteral("Action for a single press of the left stem."),
        QStringLiteral("Action for a double press of the left stem."),
        QStringLiteral("Action for a triple press of the left stem."),
        QStringLiteral("Action for a long press of the left stem."),
        QStringLiteral("Action for a single press of the right stem."),
        QStringLiteral("Action for a double press of the right stem."),
        QStringLiteral("Action for a triple press of the right stem."),
        QStringLiteral("Action for a long press of the right stem."),
        QStringLiteral("Stem press actions"),
        QStringLiteral(
            "Choose the Windows action for each press gesture. Changes are saved automatically."),
        QStringLiteral("Single"),
        QStringLiteral("Double"),
        QStringLiteral("Triple"),
        QStringLiteral("Long"),
        QStringLiteral("Left stem"),
        QStringLiteral("Right stem"),
        QStringLiteral("None"),
        QStringLiteral("Play / Pause"),
        QStringLiteral("Next track"),
        QStringLiteral("Previous track"),
        QStringLiteral("Volume up"),
        QStringLiteral("Volume down"),
        QStringLiteral("Toggle noise control"),
    };

    QFile catalog{QString{APD_SOURCE_DIR} + "/Source/Resource/Translation/apd_" + locale + ".ts"};
    QVERIFY2(catalog.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(catalog.fileName()));
    QXmlStreamReader xml{&catalog};
    QString context;
    QString source;
    QMap<QString, QString> translations;
    QMap<QString, int> counts;
    QMap<QString, bool> unfinished;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == QStringLiteral("context")) {
            context.clear();
            source.clear();
            continue;
        }
        if (!xml.isStartElement()) {
            continue;
        }
        if (xml.name() == QStringLiteral("context")) {
            context.clear();
            source.clear();
        }
        else if (xml.name() == QStringLiteral("name")) {
            context = xml.readElementText();
        }
        else if (xml.name() == QStringLiteral("source")) {
            source = xml.readElementText();
        }
        else if (xml.name() == QStringLiteral("translation")) {
            const auto key = context + QLatin1Char{'\\'} + source;
            ++counts[key];
            unfinished[key] = xml.attributes().value(QStringLiteral("type")).toString() ==
                              QStringLiteral("unfinished");
            translations[key] = xml.readElementText().trimmed();
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));

    for (const auto &expected : expectedSources) {
        const bool isDescription = expected.startsWith(QStringLiteral("Action for "));
        const auto key =
            (isDescription ? QStringLiteral("QObject") : QStringLiteral("SettingsWindow")) +
            QLatin1Char{'\\'} + expected;
        QVERIFY2(counts.value(key) == 1, qPrintable(QString{"%1: missing or duplicate %2/%3"})
                                                   .arg(locale, key.section(QLatin1Char{'\\'}, 0, 0),
                                                        expected)));
        QVERIFY2(!unfinished.value(key, true),
            qPrintable(QString{"%1: unfinished translation for %2"}).arg(locale, expected)));
        const auto translation = translations.value(key);
        QVERIFY2(!translation.isEmpty(), qPrintable(QString{"%1: empty translation for %2"})
                                                  .arg(locale, expected)));
        // Spanish "Triple" and French "Double"/"Triple"/"Long" are correctly identical to the
        // source; every other entry must be a real translation rather than an untranslated
        // fallback.
        QVERIFY2((locale == QStringLiteral("es_ES") && expected == QStringLiteral("Triple")) ||
                     (locale == QStringLiteral("fr_FR") &&
                      (expected == QStringLiteral("Double") || expected == QStringLiteral("Triple") ||
                       expected == QStringLiteral("Long"))) ||
                     translation != expected,
            qPrintable(QString{"%1: untranslated fallback for %2"}).arg(locale, expected)));
    }
}

void AirPodsDomainTests::HebrewUpdateWindowTranslationsAreFinished_data()
{
    QTest::addColumn<QString>("expectedSource");
    QTest::addColumn<QString>("expectedTranslation");

    QTest::newRow("view release") << qUtf8Printable(QStringLiteral("View release"))
                                  << qUtf8Printable(QStringLiteral("צפה בפרטי הגרסה"));
    QTest::newRow("skip this version") << qUtf8Printable(QStringLiteral("Skip this version"))
                                       << qUtf8Printable(QStringLiteral("דלג על גרסה זו"));
    QTest::newRow("remind me later") << qUtf8Printable(QStringLiteral("Remind me later"))
                                     << qUtf8Printable(QStringLiteral("הזכר לי מאוחר יותר"));
    QTest::newRow("update now") << qUtf8Printable(QStringLiteral("Update now"))
                                << qUtf8Printable(QStringLiteral("עדכן עכשיו"));
    QTest::newRow("download manually") << qUtf8Printable(QStringLiteral("Download Manually"))
                                       << qUtf8Printable(QStringLiteral("הורדת ידנית"));
}

void AirPodsDomainTests::HebrewUpdateWindowTranslationsAreFinished()
{
    QFETCH(QString, expectedSource);
    QFETCH(QString, expectedTranslation);

    QFile catalog{QString{APD_SOURCE_DIR} + "/Source/Resource/Translation/apd_he_IL.ts"};
    QVERIFY(catalog.open(QIODevice::ReadOnly | QIODevice::Text));

    QXmlStreamReader xml{&catalog};
    QString contextName;
    QString source;
    int matches = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == QStringLiteral("context")) {
            contextName.clear();
            source.clear();
            continue;
        }
        if (!xml.isStartElement()) {
            continue;
        }
        if (xml.name() == QStringLiteral("context")) {
            contextName.clear();
            source.clear();
        }
        else if (xml.name() == QStringLiteral("name")) {
            contextName = xml.readElementText();
        }
        else if (
            contextName == QStringLiteral("UpdateWindow") && xml.name() == QStringLiteral("source"))
        {
            source = xml.readElementText();
        }
        else if (
            contextName == QStringLiteral("UpdateWindow") && source == expectedSource &&
            xml.name() == QStringLiteral("translation"))
        {
            ++matches;
            QVERIFY2(
                xml.attributes().value(QStringLiteral("type")).toString() !=
                    QStringLiteral("unfinished"),
                qPrintable(QString{"UpdateWindow/%1 is marked unfinished"}.arg(expectedSource)));
            const auto translation = xml.readElementText().trimmed();
            QVERIFY2(
                !translation.isEmpty(),
                qPrintable(QString{"UpdateWindow/%1 has an empty Hebrew translation"}.arg(
                    expectedSource)));
            QCOMPARE(translation, expectedTranslation);
        }
    }
    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
    QCOMPARE(matches, 1);
}

void AirPodsDomainTests::MapsMainWindowAnimationResources()
{
    const auto pro = Gui::GetAnimationPresentation(Core::AirPods::Model::AirPods_Pro_2_USB_C);
    QCOMPARE(pro.resource, QString{"qrc:/Resource/Video/AirPods_Pro_2.avi"});
    QCOMPARE(pro.sourceSize, QSize(900, 450));

    QCOMPARE(
        Gui::GetAnimationPresentation(
            Core::AirPods::Model::Beats_Fit_Pro, static_cast<Core::AppleCP::Color>(4))
            .resource,
        QString{"qrc:/Resource/Video/Beats_Fit_Pro/4.avi"});
    QCOMPARE(
        Gui::GetAnimationPresentation(
            Core::AirPods::Model::Beats_Fit_Pro, static_cast<Core::AppleCP::Color>(200))
            .resource,
        QString{"qrc:/Resource/Video/Beats_Fit_Pro/1.avi"});

    const auto fallback = Gui::GetAnimationPresentation(Core::AirPods::Model::Unknown);
    QCOMPARE(fallback.resource, QString{"qrc:/Resource/Video/AirPods_1.avi"});
    QCOMPARE(fallback.sourceSize, QSize(800, 400));

    QFile qrc{QString{APD_SOURCE_DIR} + "/Source/Resource/Animation.qrc"};
    QVERIFY(qrc.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto qrcContents = QString::fromUtf8(qrc.readAll());

    for (uint32_t i = 0; i < static_cast<uint32_t>(Core::AirPods::Model::_Max); ++i) {
        const auto model = static_cast<Core::AirPods::Model>(i);
        for (const auto color : Gui::GetAnimationColors(model)) {
            const auto presentation = Gui::GetAnimationPresentation(model, color);
            QStringList resources{
                QString{presentation.resource}.replace("qrc:/Resource/", ""),
                QString{presentation.FallbackResource()}.replace(":/Resource/", ""),
            };
            const auto stills = QString{presentation.StillDirectory()}.replace(":/Resource/", "");
            QCOMPARE(!stills.isEmpty(), Core::AirPods::HasChargingCase(model));
            if (!stills.isEmpty()) {
                resources << stills + "/L.png" << stills + "/R.png" << stills + "/Case.png";
            }
            for (const auto &resource : resources) {
                QVERIFY2(
                    qrcContents.contains("<file>" + resource + "</file>"),
                    qPrintable(QString{"resource not listed in Animation.qrc: %1"}.arg(resource)));
                QVERIFY2(
                    QFile::exists(QString{APD_SOURCE_DIR} + "/Source/Resource/" + resource),
                    qPrintable(QString{"resource file missing: %1"}.arg(resource)));
            }

            const QImage still{
                QString{APD_SOURCE_DIR} + "/Source/Resource/" +
                QString{presentation.FallbackResource()}.replace(":/Resource/", "")};
            QVERIFY2(!still.isNull(), qPrintable(presentation.resource));
            QVERIFY2(
                std::abs(
                    static_cast<qreal>(presentation.sourceSize.width()) /
                        presentation.sourceSize.height() -
                    static_cast<qreal>(still.width()) / still.height()) < 0.01,
                qPrintable(presentation.resource));
        }
    }

    QFile mainResources{QString{APD_SOURCE_DIR} + "/Source/Resource/Resource.qrc"};
    QVERIFY(mainResources.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto mainContents = QString::fromUtf8(mainResources.readAll());
    QVERIFY(!mainContents.contains("<file>Video/"));
    QVERIFY(!mainContents.contains("<file>Image/Animation/"));
    QVERIFY(!mainContents.contains("<file>Image/Still/"));
}

void AirPodsDomainTests::ConvertsTaskbarGeometryAcrossDpiBoundaries()
{
    // Reproduces issue #217: one 2560x1440 monitor at 125% display scaling. Win32 reports
    // physical pixels, while QWidget geometry is expressed in device-independent pixels.
    const auto layout =
        Gui::TaskbarGeometry::CalculateLayout(QSize{2560, 50}, QRect{160, 0, 2325, 50}, 120, true);

    QCOMPARE(layout.statusLogical, QRect(1988, 0, 60, 40));
    QCOMPARE(layout.taskButtonsNative, QRect(160, 0, 2325, 50));

    QCOMPARE(Gui::TaskbarGeometry::LogicalToNative(layout.statusLogical.right() + 1, 120), 2560);

    const auto restored =
        Gui::TaskbarGeometry::RestoreTaskButtons(QSize{2560, 50}, layout.taskButtonsNative, true);
    QCOMPARE(restored, QRect(160, 0, 2400, 50));

    const auto vertical =
        Gui::TaskbarGeometry::CalculateLayout(QSize{50, 1440}, QRect{0, 120, 50, 1270}, 120, false);
    QCOMPARE(vertical.statusLogical, QRect(0, 1112, 40, 40));
    QCOMPARE(vertical.taskButtonsNative, QRect(0, 120, 50, 1270));
}

QTEST_GUILESS_MAIN(AirPodsDomainTests)

#include "AirPodsDomainTests.moc"
