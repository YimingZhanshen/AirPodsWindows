#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include <QtTest>
#include <QFile>
#include <QImage>
#include <QTemporaryFile>
#include <QVector>
#include <QXmlStreamReader>

#include <Config.h>
#include "Source/Core/AirPods.h"
#include "Source/Core/Settings.h"
#include "Source/Core/SettingsRepository.h"
#include "Source/Core/Update.h"
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
    void OnNoiseControlModeChanged(uint32_t mode) override
    {
        noiseControlMode = mode;
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

    bool automaticEarDetection{false};
    bool conversationalAwareness{false};
    uint8_t conversationalAwarenessVolumePercent{};
    bool personalizedVolume{false};
    bool loudSoundReduction{false};
    uint8_t ppeCapLevel{};
    uint32_t noiseControlMode{};
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
    void ParsesAdvertisementState();
    void NormalizesSingleBatteryModels();
    void FiltersDuplicateAndWeakAdvertisements();
    void AcceptsWeakAndUnknownRssiFromTrackedAddress();
    void RemembersSideAddressAcrossStateReset();
    void RejectsConflictingBatterySources();
    void RecoversFromWeakerBatterySource();
    void AcceptsAdjacentBatteryBucketsAfterAddressRotation();
    void MergesAdvertisementsFromBothSides();
    void RejectsAdvertisementsFromDifferentModels();
    void AcceptsKnownModelAfterUnknownAdvertisement();
    void TracksLidAcrossShortGaps();
    void TracksLidWhilePodsAreOut();
    void EarDetectionTrackerSurvivesStateLoss();
    void PackagesCompatibleLowLatencySilence();
    void LoadsSettingsThroughRepository();
    void MigratesLegacySettingsRepository();
    void PreservesExistingSettingsDuringMigration();
    void RollsBackFailedSettingsMigration();
    void PropagatesQuickConnectSettings();
    void PreservesForkSettingsAndAddsPlaybackAutoConnect();
    void KeepsPpeCapIndependentFromLegacyAdaptiveTransparency();
    void PersistsPpeCapLevelRoundTrip();
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
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x2030), Model::AirPods_5);
    QCOMPARE(Core::AppleCP::AirPods::GetModel(0x2036), Model::AirPods_5);
    QCOMPARE(Helper::ToString(Model::AirPods_5), QString{"AirPods 5"});

    for (const auto modelId : {uint16_t{0x2030}, uint16_t{0x2036}}) {
        const Advertisement advertisement{
            MakeAdvertisementData(0x1234, -45, Side::Left, 8, 7, 5, modelId)};
        QCOMPARE(advertisement.GetAdvState().model, Model::AirPods_5);
        QCOMPARE(advertisement.GetAdvState().pods.left.battery.Value(), 80u);
        QCOMPARE(advertisement.GetAdvState().pods.right.battery.Value(), 70u);
        QCOMPARE(advertisement.GetAdvState().caseBox.battery.Value(), 50u);
    }

    // Sanitized captures from an AirPods 5 with Wireless Charging Case. Bytes 11-26 are
    // intentionally zeroed because the protocol's trailing payload may identify the device.
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
    QCOMPARE(tracker.Update(Lid::Unobservable, Lid::Opened, start + 5s), std::optional<bool>{true});
    tracker.Reset();
    QCOMPARE(tracker.Update(std::nullopt, Lid::Closed, start + 6s), std::nullopt);
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
          "popup_on_lid_open"})
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
