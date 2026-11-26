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

#include "AirPods.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <QVector>
#include <QMetaObject>

#include "ConnectionDiagnostics.h"
#include "GlobalMedia.h"
#include "Settings.h"
#include "../Error.h"
#include "../Helper.h"
#include "../Logger.h"

namespace Core::AirPods {

namespace {

std::string NormalizeModelNumber(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    return result;
}

} // namespace

namespace Details {

// Every entry below is anchored in Docs/AAP-Protocol-Spec.md §14 (which cites librepods'
// data/AirPods.kt) or in an upstream commit that was already in the fork's history. When
// Apple publishes a new identifier without a matching reference we leave the string out so
// callers see nullopt instead of a guessed capability.
std::optional<Model> ModelFromModelNumber(std::string_view modelNumber)
{
    static const std::unordered_map<std::string, Model> kModelMap{
        // AirPods 1/2/3 -- AAP-Protocol-Spec.md row 216-218 (librepods data/AirPods.kt).
        {"A1523", Model::AirPods_1},
        {"A1722", Model::AirPods_1},
        {"A2032", Model::AirPods_2},
        {"A2031", Model::AirPods_2},
        {"A2565", Model::AirPods_3},
        {"A2564", Model::AirPods_3},
        // AirPods 4 / 4 (ANC) -- AAP-Protocol-Spec.md row 219-220.
        {"A3053", Model::AirPods_4},
        {"A3050", Model::AirPods_4},
        {"A3054", Model::AirPods_4},
        {"A3056", Model::AirPods_4_ANC},
        {"A3055", Model::AirPods_4_ANC},
        {"A3057", Model::AirPods_4_ANC},
        // AirPods 5 and AirPods 5 with Wireless Charging Case -- AAP-Protocol-Spec.md row
        // 221-222 (librepods AirPods.kt:114-149). The base variant uses A3531/A3532/A3533;
        // the wireless-charging variant uses A3439/A3440/A3441.
        {"A3531", Model::AirPods_5},
        {"A3532", Model::AirPods_5},
        {"A3533", Model::AirPods_5},
        {"A3439", Model::AirPods_5_WCC},
        {"A3440", Model::AirPods_5_WCC},
        {"A3441", Model::AirPods_5_WCC},
        // AirPods Pro 1/2/3 -- AAP-Protocol-Spec.md row 223-226.
        {"A2084", Model::AirPods_Pro},
        {"A2083", Model::AirPods_Pro},
        {"A2931", Model::AirPods_Pro_2},
        {"A2699", Model::AirPods_Pro_2},
        {"A2698", Model::AirPods_Pro_2},
        {"A3047", Model::AirPods_Pro_2_USB_C},
        {"A3048", Model::AirPods_Pro_2_USB_C},
        {"A3049", Model::AirPods_Pro_2_USB_C},
        {"A3063", Model::AirPods_Pro_3},
        {"A3064", Model::AirPods_Pro_3},
        {"A3065", Model::AirPods_Pro_3},
        // AirPods Max (Lightning) and AirPods Max (USB-C / "Max 2") -- AAP-Protocol-Spec.md
        // row 227 (enums.h:51-52) and the upstream PR #251 mapping that fixed c543222.
        {"A2096", Model::AirPods_Max},
        {"A3184", Model::AirPods_Max_USB_C},
    };

    if (modelNumber.empty()) {
        return std::nullopt;
    }

    const auto normalized = NormalizeModelNumber(modelNumber);
    const auto iter = kModelMap.find(normalized);
    if (iter == kModelMap.end()) {
        return std::nullopt;
    }

    return iter->second;
}

Model ResolveModel(Model productModel, std::optional<Model> modelNumberModel)
{
    if (!modelNumberModel.has_value()) {
        return productModel;
    }

    const auto candidate = modelNumberModel.value();
    if (productModel == Model::Unknown || productModel == candidate) {
        return candidate;
    }

    switch (productModel) {
    case Model::AirPods_4:
        return candidate == Model::AirPods_4_ANC ? candidate : productModel;
    case Model::AirPods_5:
        return candidate == Model::AirPods_5_WCC ? candidate : productModel;
    case Model::AirPods_Pro_2:
        return candidate == Model::AirPods_Pro_2_USB_C ? candidate : productModel;
    case Model::AirPods_Max:
        return candidate == Model::AirPods_Max_USB_C ? candidate : productModel;
    default:
        return productModel;
    }
}

QString ResolveDisplayName(QString deviceName, Model model)
{
    deviceName.remove(" - Find My");

    const bool isDefaultModelName =
        (model == Model::AirPods_Max_USB_C && deviceName == "AirPods Max") ||
        (model == Model::AirPods_5 && deviceName == "AirPods") ||
        (model == Model::AirPods_5_WCC && deviceName == "AirPods");
    return deviceName.isEmpty() || isDefaultModelName ? Helper::ToString(model) : deviceName;
}

} // namespace Details

namespace {

using EarDetectionClock = Details::EarDetectionTracker::Clock;

EarState EarStateFromBle(const PodState &pod)
{
    if (!pod.battery.Available()) {
        return EarState::Unknown;
    }
    if (pod.isInCase) {
        return EarState::InCase;
    }
    return pod.isInEar ? EarState::InEar : EarState::OutOfEar;
}

EarState EarStateFromAap(AAP::EarStatus status)
{
    switch (status) {
    case AAP::EarStatus::InEar:
        return EarState::InEar;
    case AAP::EarStatus::OutOfEar:
        return EarState::OutOfEar;
    case AAP::EarStatus::InCase:
        return EarState::InCase;
    case AAP::EarStatus::Unknown:
    default:
        return EarState::Unknown;
    }
}

EarState CombineEarStates(EarState left, EarState right);

EarState FuseAggregateEarState(EarState aap, EarState ble)
{
    // A complete/known AAP aggregate describes both physical pods regardless of role order.
    // Partial/unknown AAP aggregates fall back to BLE rather than inventing an ear mapping.
    return aap == EarState::Unknown ? ble : aap;
}

EarState CombineEarStates(EarState left, EarState right)
{
    // One worn pod is enough to keep playback active; this also prevents single-ear alternation
    // from looking like a removal. Unknown peers cannot prove a full removal.
    if (left == EarState::InEar || right == EarState::InEar) {
        return EarState::InEar;
    }
    if (left == EarState::Unknown || right == EarState::Unknown) {
        return EarState::Unknown;
    }
    if (left == EarState::InCase && right == EarState::InCase) {
        return EarState::InCase;
    }
    return EarState::OutOfEar;
}

bool SameAapSession(
    const std::optional<AAP::AAPAttempt> &left, const std::optional<AAP::AAPAttempt> &right)
{
    return left.has_value() == right.has_value() &&
           (!left.has_value() ||
            (left->address == right->address && left->generation == right->generation));
}

} // namespace

std::optional<EarState> Details::EarDetectionTracker::UpdateBle(
    const State &state, Timestamp now, std::optional<uint64_t> expectedEpoch)
{
    std::lock_guard<std::mutex> lock{_mutex};
    if (expectedEpoch.has_value() && *expectedEpoch != _epoch) {
        return std::nullopt;
    }
    if (_bleAt.has_value() && now < *_bleAt) {
        return std::nullopt; // An out-of-order reading cannot refresh the source.
    }
    _ble.left = EarStateFromBle(state.pods.left);
    _ble.right = EarStateFromBle(state.pods.right);
    _bleAt = now;
    _bleConfirmed = true;
    return UpdateCandidate(now);
}

std::optional<EarState>
Details::EarDetectionTracker::UpdateAap(EarState primary, EarState secondary, Timestamp now)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _aapPrimary = primary;
    _aapSecondary = secondary;
    _aapPrimarySide.reset();
    _aapSession.reset();
    return UpdateCandidate(now);
}

std::optional<EarState> Details::EarDetectionTracker::UpdateAap(
    EarState primary, EarState secondary, std::optional<Side> primarySide, AAP::AAPAttempt session,
    std::optional<uint64_t> expectedEpoch, Timestamp now)
{
    std::lock_guard<std::mutex> lock{_mutex};
    if (expectedEpoch.has_value() && *expectedEpoch != _epoch) {
        return std::nullopt;
    }
    _aapPrimary = primary;
    _aapSecondary = secondary;
    _aapPrimarySide = primarySide;
    _aapSession = session;
    return UpdateCandidate(now);
}

std::optional<EarState> Details::EarDetectionTracker::UpdateCandidate(Timestamp now)
{
    const auto bleLeft = _bleConfirmed ? _ble.left.value_or(EarState::Unknown) : EarState::Unknown;
    const auto bleRight =
        _bleConfirmed ? _ble.right.value_or(EarState::Unknown) : EarState::Unknown;
    const auto aapPrimary = _aapPrimary.value_or(EarState::Unknown);
    const auto aapSecondary = _aapSecondary.value_or(EarState::Unknown);
    EarState candidate = EarState::Unknown;
    if (_aapPrimarySide.has_value()) {
        // Map roles only when BLE supplied the physical primary identity for this report.
        const auto aapLeft = *_aapPrimarySide == Side::Left ? aapPrimary : aapSecondary;
        const auto aapRight = *_aapPrimarySide == Side::Left ? aapSecondary : aapPrimary;
        const auto fuse = [](EarState aap, EarState ble) {
            return aap == EarState::Unknown ? ble : aap;
        };
        candidate = CombineEarStates(fuse(aapLeft, bleLeft), fuse(aapRight, bleRight));
    }
    else {
        // Role order is not left/right. Fuse two physical aggregates conservatively instead.
        candidate = FuseAggregateEarState(
            CombineEarStates(aapPrimary, aapSecondary), CombineEarStates(bleLeft, bleRight));
    }
    const auto state = _debounce.Update(candidate, now);
    if (!_publishedState.has_value()) {
        if (state == EarState::Unknown) {
            return std::nullopt;
        }
        // Establishing or rebuilding a baseline must not resume/pause media by itself.
        _publishedState = state;
        return std::nullopt;
    }
    if (state != *_publishedState) {
        _publishedState = state;
        return state;
    }
    return std::nullopt;
}

std::optional<EarState> Details::EarDetectionTracker::Reevaluate(Timestamp now)
{
    std::lock_guard<std::mutex> lock{_mutex};
    return UpdateCandidate(now);
}

std::optional<EarState> Details::EarDetectionTracker::Reevaluate(
    Timestamp now, uint64_t expectedEpoch, const std::optional<AAP::AAPAttempt> &expectedSession)
{
    std::lock_guard<std::mutex> lock{_mutex};
    if (expectedEpoch != _epoch || !SameAapSession(expectedSession, _aapSession)) {
        return std::nullopt;
    }
    return UpdateCandidate(now);
}

std::optional<Details::EarDetectionTracker::Clock::time_point>
Details::EarDetectionTracker::Deadline() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    const auto now = Clock::now();
    const auto remaining = _debounce.Deadline(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()));
    if (!remaining.has_value()) {
        return std::nullopt;
    }
    return now + *remaining;
}

uint64_t Details::EarDetectionTracker::Epoch() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _epoch;
}

Details::EarDetectionTracker::FusionContext Details::EarDetectionTracker::Snapshot() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return FusionContext{
        .epoch = _epoch,
        .session = _aapSession,
        .primarySide = _aapPrimarySide,
    };
}

void Details::EarDetectionTracker::InvalidateAapReadings()
{
    _aapPrimary.reset();
    _aapSecondary.reset();
    _aapPrimarySide.reset();
    _aapSession.reset();
    // Cached BLE values are retained for display/fusion, but cannot authorize a source-switch
    // transition until a newly timestamped BLE reading confirms them after the disconnect.
    _bleConfirmed = false;
    _debounce.CancelPending();
    ++_epoch;
}

bool Details::EarDetectionTracker::ResetAap(AAP::AAPAttempt session, uint64_t expectedEpoch)
{
    std::lock_guard<std::mutex> lock{_mutex};
    if (expectedEpoch != _epoch || !_aapSession.has_value() ||
        _aapSession->generation != session.generation || _aapSession->address != session.address)
    {
        return false;
    }
    InvalidateAapReadings();
    return true;
}

void Details::EarDetectionTracker::ResetAap()
{
    std::lock_guard<std::mutex> lock{_mutex};
    InvalidateAapReadings();
}

void Details::EarDetectionTracker::Reset()
{
    std::lock_guard<std::mutex> lock{_mutex};
    _ble.left.reset();
    _ble.right.reset();
    _bleAt.reset();
    _bleConfirmed = false;
    InvalidateAapReadings();
    _publishedState.reset();
    _debounce.Reset();
}

void Details::EarDetectionTracker::SetSensitivity(EarDetectSensitivity sensitivity)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _debounce.SetSensitivity(sensitivity);
}

EarState Details::EarDetectionTracker::CurrentState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _publishedState.value_or(EarState::Unknown);
}

Manager::Manager(QObject *parent) : QObject{parent}
{
    StartAAPConnectWorker();
    _earDebounceTimer.setSingleShot(true);
    _earDebounceTimer.setTimerType(Qt::PreciseTimer);
    connect(&_earDebounceTimer, &QTimer::timeout, this, &Manager::ReevaluateEarDetection);

    _stateMgr.SetOnDiscardState([this] {
        _lidTracker.OnLost(Details::LidTracker::Clock::now());
        // The BLE baseline is gone; battery events must not republish a stale state.
        {
            std::lock_guard lock{_batteryFusionMutex};
            _lastBleState.reset();
            _lastPublishedState.reset();
        }
        ResetEarDetection();
        QMetaObject::invokeMethod(this, [this] { emit Disconnected(); }, Qt::QueuedConnection);
    });

    _adWatcher.CbReceived() += [this](auto &&...args) {
        // Sample before taking `_mutex`: reset may win the lock while this callback is waiting,
        // but a report that entered before reset must not adopt the reset's newer epoch.
        const auto resetGeneration = _earResetGeneration.load();
        std::lock_guard<std::mutex> lock{_mutex};
        OnAdvertisementReceived(std::forward<decltype(args)>(args)..., resetGeneration);
    };

    _adWatcher.CbStateChanged() +=
        [this](auto &&...args) { OnAdvWatcherStateChanged(std::forward<decltype(args)>(args)...); };

    SetupAAPCallbacks();
}

void Manager::SetupAAPCallbacks()
{
    AAP::Callbacks callbacks;

    callbacks.onNoiseControlChanged = [this](AAP::NoiseControlMode mode, AAP::AAPAttempt attempt) {
        OnNoiseControlModeNotification(mode, attempt);
    };

    callbacks.onConversationalAwarenessChanged =
        [this](AAP::ConversationalAwarenessState state, AAP::AAPAttempt attempt) {
            OnConversationalAwarenessStateChanged(state, attempt);
        };

    callbacks.onPersonalizedVolumeChanged =
        [this](AAP::PersonalizedVolumeState state, AAP::AAPAttempt attempt) {
            OnPersonalizedVolumeStateChanged(state, attempt);
        };

    callbacks.onLoudSoundReductionChanged =
        [this](AAP::LoudSoundReductionState state, AAP::AAPAttempt attempt) {
            OnLoudSoundReductionStateChanged(state, attempt);
        };

    callbacks.onAutomaticEarDetectionChanged = [this](bool enabled, AAP::AAPAttempt attempt) {
        OnAutomaticEarDetectionNotification(enabled, attempt);
    };

    callbacks.onPpeCapLevelChanged = [this](uint8_t level, AAP::AAPAttempt attempt) {
        OnPpeCapLevelNotification(level, attempt);
    };

    callbacks.onSpeakingLevelChanged = [this](AAP::SpeakingLevel level, AAP::AAPAttempt attempt) {
        OnSpeakingLevelChanged(level, attempt);
    };

    callbacks.onEarDetectionChanged =
        [this](AAP::EarStatus primary, AAP::EarStatus secondary, AAP::AAPAttempt attempt) {
            // This is the callback-entry synchronization point shared with reset. The handler
            // may capture the tracker epoch later, after a reset has completed.
            const auto resetGeneration = _earResetGeneration.load();
            OnEarDetectionChanged(primary, secondary, attempt, resetGeneration);
        };

    callbacks.onBatteryChanged = [this](AAP::AAPBatterySnapshot snapshot, AAP::AAPAttempt attempt) {
        OnAAPBatteryChanged(snapshot, attempt);
    };

    callbacks.onHeadTrackingData = [this](AAP::HeadTrackingData data, AAP::AAPAttempt attempt) {
        OnHeadTrackingData(data, attempt);
    };

    callbacks.onAttProbeResult = [this](AAP::AttPathState state, AAP::AAPAttempt attempt) {
        OnAttProbeResult(state, attempt);
    };

    callbacks.onConnected = [this](AAP::AAPAttempt attempt) { OnAAPConnected(attempt); };

    callbacks.onDisconnected = [this](AAP::AAPAttempt attempt) { OnAAPDisconnected(attempt); };

    _aapMgr.SetCallbacks(std::move(callbacks));
}

Manager::~Manager()
{
    {
        std::lock_guard lock{_aapWorkerMutex};
        _aapShutdown = true;
        _aapDesiredAddress.reset();
        _aapRetryScheduled = false;
    }
    _aapWorkerCv.notify_all();
    _aapMgr.CancelConnect();

    if (_aapConnectWorker.joinable()) {
        _aapConnectWorker.request_stop();
        _aapConnectWorker.join();
    }

    DisconnectAAP();
    StopScanner();

    {
        std::lock_guard lock{_aapWorkerMutex};
        _aapMgr.SetCallbacks({});
    }

    _adWatcher.CbReceived().UnregisterAll();
    _adWatcher.CbStateChanged().UnregisterAll();

    _deviceLookupThread.request_stop();
    if (_deviceLookupThread.joinable()) {
        _deviceLookupThread.join();
    }
}

bool Manager::StartScannerOnce()
{
    if (!_scannerWanted.exchange(true)) {
        bool deviceConnected;
        bool hasDevice;
        {
            std::lock_guard<std::mutex> lock{_mutex};
            deviceConnected = _deviceConnected;
            hasDevice = _boundDevice.has_value();
        }
        if (!hasDevice || !deviceConnected) {
            _scannerWanted = false;
            LOG(Info, "Skip Bluetooth AdvWatcher start because the bound device is disconnected.");
            return false;
        }
    }

    if (_scannerRunning) {
        return true;
    }

    if (!_adWatcher.Start()) {
        LOG(Warn, "Bluetooth AdvWatcher start failed.");
        return false;
    }
    LOG(Info, "Bluetooth AdvWatcher start succeeded.");
    return true;
}

void Manager::StartScanner()
{
    if (!StartScannerOnce() && _scannerWanted) {
        ScheduleScannerRetry();
    }
}

void Manager::StopScanner()
{
    _scannerWanted = false;
    StopScannerRetry();
    _scannerRunning = false;

    if (!_adWatcher.Stop()) {
        LOG(Warn, "AsyncScanner::Stop() failed.");
    }
    else {
        LOG(Info, "AsyncScanner::Stop() succeeded.");
    }
}

void Manager::ScheduleScannerRetry()
{
    std::lock_guard lock{_scannerRetryMutex};
    if (!_scannerWanted || _scannerRunning) {
        return;
    }

    _scannerRetryWorker.Start(std::chrono::milliseconds{3000}, [this] {
        if (!_scannerWanted || _scannerRunning) {
            return false;
        }
        StartScannerOnce();
        return _scannerWanted && !_scannerRunning;
    });
}

void Manager::StopScannerRetry()
{
    std::lock_guard lock{_scannerRetryMutex};
    _scannerRetryWorker.Stop();
}

void Manager::OnRssiMinChanged(int16_t rssiMin)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _stateMgr.OnRssiMinChanged(rssiMin);
}

void Manager::OnAutomaticEarDetectionChanged(bool enable)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _automaticEarDetection = enable;
}

void Manager::OnEarDetectSensitivityChanged(EarDetectSensitivity sensitivity)
{
    QMetaObject::invokeMethod(
        this,
        [this, sensitivity] {
            {
                std::lock_guard<std::mutex> lock{_mutex};
                _earDetectSensitivity = sensitivity;
                _earDetection.SetSensitivity(sensitivity);
            }
            ScheduleEarDeadline();
        },
        Qt::QueuedConnection);
}

void Manager::OnBoundDeviceAddressChanged(uint64_t address)
{
    _deviceLookupThread.request_stop();
    if (_deviceLookupThread.joinable()) {
        _deviceLookupThread.join();
    }

    // Cancel and drain the old AAP worker before holding the AirPods state mutex. A driver
    // call may be slow, and this method can arrive from the GUI or another state callback.
    DisconnectAAP();
    _currentNoiseControlMode = -1;

    {
        std::lock_guard<std::mutex> lock{_mutex};
        _requestedDeviceAddress = address;
        _boundDevice.reset();
        _modelOverride.reset();
        _boundModel = Model::Unknown;
        _deviceConnected = false;
        _lidTracker.Reset();
    }

    // StateManager invokes the discard callback synchronously. Run it outside `_mutex` so the
    // callback can serialize reset with BLE/AAP callbacks without lock inversion.
    if (!_stateMgr.Disconnect()) {
        QMetaObject::invokeMethod(this, [this] { ScheduleEarDeadline(); }, Qt::QueuedConnection);
    }

    emit DeviceConnectionChanged(false);
    QMetaObject::invokeMethod(this, [this] { StopScanner(); }, Qt::QueuedConnection);

    if (address == 0) {
        LOG(Info, "Unbind device.");
        return;
    }

    LOG(Info, "Bind a new device.");

    _deviceLookupThread = std::jthread{[this, address](std::stop_token stopToken) {
        OS::Windows::Winrt::Initialize();
        auto device = Bluetooth::DeviceManager::FindDevice(address);
        if (stopToken.stop_requested()) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this, address, device = std::move(device)]() mutable {
                CompleteBoundDeviceLookup(address, std::move(device));
            },
            Qt::QueuedConnection);
    }};
}

void Manager::CompleteBoundDeviceLookup(uint64_t address, std::optional<Bluetooth::Device> device)
{
    std::optional<Bluetooth::DeviceState> initialState;
    {
        std::lock_guard<std::mutex> lock{_mutex};

        if (address != _requestedDeviceAddress) {
            LOG(Info, "Ignore a stale bound-device lookup result.");
            return;
        }

        if (!device.has_value()) {
            LOG(Warn, "The bound device address is no longer available.");
            emit BoundDeviceUnavailable();
            return;
        }

        _boundDevice = std::move(device);
        const auto productModel = AppleCP::AirPods::GetModel(_boundDevice->GetProductId());

        if (auto modelNumber = _boundDevice->GetModelNumber(); modelNumber.has_value()) {
            _modelOverride = Details::ModelFromModelNumber(modelNumber.value());
            if (_modelOverride.has_value()) {
                LOG(Info, "Detected model number '{}', override model: {}", modelNumber.value(),
                    Helper::ToString(_modelOverride.value()));
            }
        }
        _boundModel = Details::ResolveModel(productModel, _modelOverride);

        _deviceName = QString::fromStdString([&] {
            auto name = _boundDevice->GetName();
            return name.find("Bluetooth") != std::string::npos ? std::string{} : name;
        }());

        _boundDevice->CbConnectionStatusChanged() += [this](auto &&...args) {
            OnBoundDeviceConnectionStateChanged(std::forward<decltype(args)>(args)...);
        };

        initialState = _boundDevice->GetConnectionState();
    }

    if (initialState.has_value()) {
        OnBoundDeviceConnectionStateChanged(*initialState);
    }
}

void Manager::OnBoundDeviceConnectionStateChanged(Bluetooth::DeviceState state)
{
    bool doDisconnect = false;
    bool doConnect = false;
    bool hasBoundDevice = false;
    bool newDeviceConnected = state == Bluetooth::DeviceState::Connected;
    bool oldDeviceConnected = false;

    {
        std::lock_guard<std::mutex> lock{_mutex};
        oldDeviceConnected = _deviceConnected;
        doDisconnect = oldDeviceConnected && !newDeviceConnected;
        doConnect = !oldDeviceConnected && newDeviceConnected;
        hasBoundDevice = _boundDevice.has_value();
        _deviceConnected = newDeviceConnected;

        if (doDisconnect) {
            // A transient service disconnect retains the popup rate limit; only an unbind/rebind
            // resets it.
            _lidTracker.ForgetDevice();
        }
    }

    LOG(Info,
        "OnBoundDeviceConnectionStateChanged: state={}, _deviceConnected={}, "
        "newDeviceConnected={}, doConnect={}",
        static_cast<int>(state), oldDeviceConnected, newDeviceConnected, doConnect);

    if (doDisconnect) {
        if (!_stateMgr.Disconnect()) {
            QMetaObject::invokeMethod(
                this, [this] { ScheduleEarDeadline(); }, Qt::QueuedConnection);
        }
    }

    emit DeviceConnectionChanged(newDeviceConnected);
    QMetaObject::invokeMethod(
        this,
        [this, newDeviceConnected] {
            if (newDeviceConnected) {
                StartScanner();
            }
            else {
                StopScanner();
            }
        },
        Qt::QueuedConnection);

    if (doDisconnect) {
        DisconnectAAP();
        _currentNoiseControlMode = -1;
    }

    if (doConnect && hasBoundDevice) {
        ConnectAAP();
    }

    LOG(Info, "The device we bound is updated. current: {}, new: {}", oldDeviceConnected,
        newDeviceConnected);
}

void Manager::ConnectAAP()
{
    LOG(Info, "ConnectAAP called");

    if (!_boundDevice.has_value()) {
        LOG(Warn, "ConnectAAP: No bound device");
        return;
    }

    auto state = _stateMgr.GetCurrentState();
    auto modelToCheck =
        state.has_value() ? Details::ResolveModel(state->model, _modelOverride) : _boundModel;
    LOG(Info, "ConnectAAP: state.has_value={}, state.model={}, _modelOverride.has_value={}",
        state.has_value(), state.has_value() ? static_cast<int>(state->model) : -1,
        _modelOverride.has_value());

    LOG(Info, "ConnectAAP: modelToCheck={}, SupportsANC={}", static_cast<int>(modelToCheck),
        SupportsANC(modelToCheck));

    if (SupportsANC(modelToCheck)) {
        const uint64_t address = _boundDevice->GetAddress();
        LOG(Info, "Scheduling AAP connection for ANC-capable device, address={:016X}", address);
        Diagnostics::Recorder::Instance().Record(
            Diagnostics::Step::AdvertisementMatched, true,
            std::string{"model="} + std::to_string(static_cast<int>(modelToCheck)) +
                " address=" + Diagnostics::Recorder::MaskAddress(address));
        ScheduleAAPConnection(address);
    }
    else {
        LOG(Info, "ConnectAAP: Device does not support ANC");
    }
}

void Manager::StartAAPConnectWorker()
{
    _aapConnectWorker =
        std::jthread{[this](std::stop_token stopToken) { AAPConnectLoop(stopToken); }};
}

void Manager::ScheduleAAPConnection(uint64_t address)
{
    {
        std::lock_guard lock{_aapWorkerMutex};
        if (_aapShutdown) {
            return;
        }
        const bool changed = !_aapDesiredAddress || *_aapDesiredAddress != address;
        _aapDesiredAddress = address;
        if (changed) {
            _aapRetryScheduled = false;
            _aapRetryAt = {};
        }
    }
    _aapWorkerCv.notify_all();
}

void Manager::DisconnectAAP()
{
    {
        std::lock_guard lock{_aapWorkerMutex};
        _aapDesiredAddress.reset();
        _aapRetryScheduled = false;
        _aapRetryAt = {};
        _aapActiveAttempt = {};
        _aapConnection.Stop();
    }
    _aapWorkerCv.notify_all();

    // The connect worker may be blocked in the OS/driver connect. Cancel before waiting for it
    // to unwind; this is also the cancellation path used during shutdown.
    _aapMgr.CancelConnect();
    _aapMgr.Disconnect();

    {
        std::lock_guard lock{_aapWorkerMutex};
        _aapConnection.FinishStop();
    }
    _aapWorkerCv.notify_all();
    _currentNoiseControlMode = -1;

    // P2 marks the attempt Stopping before AAP Manager invokes onDisconnected, so
    // that callback cannot identify itself as current. Explicit teardown must
    // therefore drop the overlay here as well; the next attempt starts from BLE.
    {
        std::lock_guard lock{_batteryFusionMutex};
        _batteryFusion.Reset();
    }
    RequestFusedStatePublish();
}

void Manager::AAPConnectLoop(std::stop_token stopToken)
{
    while (!stopToken.stop_requested()) {
        std::unique_lock lock{_aapWorkerMutex};
        _aapWorkerCv.wait(lock, [this, &stopToken] {
            return stopToken.stop_requested() || _aapShutdown || _aapDesiredAddress.has_value();
        });
        if (stopToken.stop_requested() || _aapShutdown) {
            break;
        }

        bool ready = false;
        while (!ready) {
            if (!stopToken.stop_requested() && !_aapShutdown && _aapDesiredAddress) {
                const auto state = _aapConnection.State();
                if (state == AAP::AAPConnectionState::Idle) {
                    ready = true;
                }
                else if (state == AAP::AAPConnectionState::Backoff) {
                    ready = !_aapRetryScheduled || AAPClock::now() >= _aapRetryAt;
                    if (!ready) {
                        _aapWorkerCv.wait_until(lock, _aapRetryAt, [this, &stopToken] {
                            return stopToken.stop_requested() || _aapShutdown ||
                                   !_aapDesiredAddress || !_aapRetryScheduled ||
                                   _aapConnection.State() != AAP::AAPConnectionState::Backoff ||
                                   AAPClock::now() >= _aapRetryAt;
                        });
                        continue;
                    }
                }
                else {
                    _aapWorkerCv.wait(lock, [this, &stopToken] {
                        return stopToken.stop_requested() || _aapShutdown || !_aapDesiredAddress ||
                               _aapConnection.State() == AAP::AAPConnectionState::Idle ||
                               _aapConnection.State() == AAP::AAPConnectionState::Backoff;
                    });
                    continue;
                }
            }
            break;
        }

        if (!ready || !_aapDesiredAddress || stopToken.stop_requested() || _aapShutdown) {
            continue;
        }

        const uint64_t address = *_aapDesiredAddress;
        const auto begin = _aapConnection.Begin(address);
        if (!begin.started) {
            _aapWorkerCv.wait(lock, [this, &stopToken] {
                return stopToken.stop_requested() || _aapShutdown || !_aapDesiredAddress ||
                       _aapConnection.State() == AAP::AAPConnectionState::Idle ||
                       _aapConnection.State() == AAP::AAPConnectionState::Backoff;
            });
            continue;
        }

        _aapActiveAttempt = begin.attempt;
        _aapConnectInProgress = true;
        _aapRetryScheduled = false;

        // Reset before AAP Manager starts its reader. Resetting in onConnected
        // would race an immediate first battery notification and could erase it.
        {
            std::lock_guard lock{_batteryFusionMutex};
            _batteryFusion.Reset();
        }
        RequestFusedStatePublish();

        lock.unlock();

        const bool connected = _aapMgr.Connect(begin.attempt, stopToken);
        const bool reportFailure = !connected;
        const auto failure = _aapMgr.GetLastConnectFailure();

        lock.lock();
        _aapConnectInProgress = false;
        _aapActiveAttempt = {};
        if (!stopToken.stop_requested() && !_aapShutdown) {
            if (connected) {
                if (_aapConnection.State() != AAP::AAPConnectionState::Connected) {
                    _aapConnection.TransportConnected(begin.attempt);
                    _aapConnection.Connected(begin.attempt, AAPClock::now());
                }
            }
            else if (_aapDesiredAddress == address) {
                const auto delay = _aapConnection.Lost(begin.attempt, AAPClock::now());
                if (delay.has_value()) {
                    _aapRetryAt = AAPClock::now() + *delay;
                    _aapRetryScheduled = true;
                    _aapWorkerCv.notify_all();
                }
            }
        }

        if (reportFailure && !stopToken.stop_requested() && !_aapShutdown) {
            lock.unlock();
            emit AapConnectionFailedSafely(static_cast<int>(failure));
        }
    }

    std::lock_guard lock{_aapWorkerMutex};
    _aapConnection.Stop();
    _aapMgr.CancelConnect();
    _aapConnection.FinishStop();
}

bool Manager::IsCurrentAAPAttempt(AAP::AAPAttempt attempt) const
{
    if (attempt.generation == 0) {
        return false;
    }
    std::lock_guard lock{_aapWorkerMutex};
    return (_aapConnectInProgress && _aapActiveAttempt.address == attempt.address &&
            _aapActiveAttempt.generation == attempt.generation) ||
           (_aapConnection.State() == AAP::AAPConnectionState::Connected && _aapMgr.IsConnected() &&
            _aapMgr.GetLastAttempt().generation == attempt.generation &&
            _aapMgr.GetLastAttempt().address == attempt.address);
}

bool Manager::SupportsANC(Model model)
{
    switch (model) {
    case Model::AirPods_Pro:
    case Model::AirPods_Pro_2:
    case Model::AirPods_Pro_2_USB_C:
    case Model::AirPods_Pro_3:
    case Model::AirPods_4_ANC:
    case Model::AirPods_5:
    case Model::AirPods_5_WCC:
    case Model::AirPods_Max:
    case Model::AirPods_Max_USB_C:
        return true;
    default:
        return false;
    }
}

void Manager::OnStateChanged(
    Details::StateManager::UpdateEvent updateEvent, uint64_t resetGeneration)
{
    const auto stateGeneration = updateEvent.generation;
    const auto &oldState = updateEvent.oldState;
    auto &newState = updateEvent.newState;

    newState.model = Details::ResolveModel(newState.model, _modelOverride);

    newState.displayName = Details::ResolveDisplayName(_deviceName, newState.model);

    {
        std::lock_guard lock{_batteryFusionMutex};
        _lastBleState = newState;
    }
    // BLE callbacks originate on the WinRT watcher thread. Queue the publish just like
    // AAP battery events so Manager's owner thread is the only StateUpdated producer.
    RequestFusedStatePublish();

    // Try to connect AAP if we have a valid model now and device is connected
    if (_deviceConnected && !_aapMgr.IsConnected() && SupportsANC(newState.model)) {
        LOG(Info,
            "OnStateChanged: Device supports ANC and AAP not connected, attempting connection");
        ConnectAAP();
    }

    using Lid = Details::LidTracker::Lid;
    const auto lidOf = [](const State &state) {
        if (!state.caseBox.isBothPodsInCase) {
            return Lid::Unobservable;
        }
        return state.caseBox.isLidOpened ? Lid::Opened : Lid::Closed;
    };
    std::optional<Lid> oldLid;
    if (oldState.has_value()) {
        oldLid = lidOf(*oldState);
    }
    if (auto lidOpened = _lidTracker.Update(
            oldLid, lidOf(newState), Details::LidTracker::Clock::now(),
            Core::Settings::GetCurrent().popup_on_lid_open);
        lidOpened.has_value())
    {
        OnLidOpened(*lidOpened);
    }

    // BLE and queued AAP reports share this one fusion/debounce path. Revalidate on the owner
    // thread: advertisement callbacks may run on a worker, where starting QTimer is unsafe.
    const auto receivedAt = Details::EarDetectionTracker::Clock::now();
    const auto epoch = _earDetection.Epoch();
    QMetaObject::invokeMethod(
        this,
        [this, newState = std::move(newState), receivedAt, epoch, resetGeneration,
         stateGeneration] {
            std::optional<EarState> changed;
            {
                std::lock_guard<std::mutex> lock{_mutex};
                if (resetGeneration != _earResetGeneration ||
                    !_stateMgr.IsCurrentGeneration(stateGeneration))
                {
                    return;
                }
                changed = _earDetection.UpdateBle(
                    newState,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        receivedAt.time_since_epoch()),
                    epoch);
                if (changed.has_value()) {
                    // Publish under the reset mutex: reset cannot land between validation and
                    // the media action and make a pre-reset transition take effect afterwards.
                    OnEarStateChanged(*changed);
                }
            }
            ScheduleEarDeadline();
        },
        Qt::QueuedConnection);
}

void Manager::OnLidOpened(bool opened)
{
    emit LidToggled(opened);
}

void Manager::ScheduleEarDeadline()
{
    std::optional<EarDetectionClock::time_point> deadline;
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _earDeadlineResetGeneration = _earResetGeneration;
        _earDeadlineStateGeneration = _stateMgr.Generation();
        const auto context = _earDetection.Snapshot();
        _earDeadlineEpoch = context.epoch;
        _earDeadlineSession = context.session;
        deadline = _earDetection.Deadline();
    }
    if (!deadline.has_value()) {
        _earDebounceTimer.stop();
        return;
    }
    const auto delay =
        std::chrono::ceil<std::chrono::milliseconds>(*deadline - EarDetectionClock::now());
    _earDebounceTimer.start(delay.count() > 0 ? delay.count() : 1);
}

void Manager::ReevaluateEarDetection()
{
    std::optional<EarState> changed;
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (_earDeadlineResetGeneration != _earResetGeneration ||
            !_stateMgr.IsCurrentGeneration(_earDeadlineStateGeneration))
        {
            return;
        }
        changed = _earDetection.Reevaluate(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                EarDetectionClock::now().time_since_epoch()),
            _earDeadlineEpoch, _earDeadlineSession);
        if (changed.has_value()) {
            // Keep validation and publication in one reset-critical section.
            OnEarStateChanged(*changed);
        }
    }
    ScheduleEarDeadline();
}

void Manager::ResetEarDetection()
{
    {
        std::lock_guard<std::mutex> lock{_mutex};
        ResetEarDetectionLocked();
    }
    // Invalidation above is synchronous; only QTimer rearm crosses threads.
    QMetaObject::invokeMethod(this, [this] { ScheduleEarDeadline(); }, Qt::QueuedConnection);
}

void Manager::ResetEarDetectionLocked()
{
    _earResetGeneration.fetch_add(1);
    _earDetection.Reset();
}

void Manager::OnEarStateChanged(EarState state)
{
    if (!_automaticEarDetection) {
        LOG(Info, "automatic_ear_detection: Do nothing because it is disabled. ({})",
            static_cast<int>(state));
        return;
    }

    if (state == EarState::InEar) {
        GlobalMedia::Play();
    }
    else if (state == EarState::OutOfEar || state == EarState::InCase) {
        GlobalMedia::Pause();
    }
}

bool Manager::OnAdvertisementReceived(
    const Bluetooth::AdvertisementWatcher::ReceivedData &data, uint64_t resetGeneration)
{
    if (resetGeneration != _earResetGeneration) {
        return false;
    }
    if (!Details::Advertisement::IsDesiredAdv(data)) {
        return false;
    }

    Details::Advertisement adv{data};

    if (!_deviceConnected) {
        LOG(Info, "AirPods advertisement received, but device disconnected.");
        return false;
    }

    if (_boundModel != Model::Unknown &&
        Details::ResolveModel(adv.GetAdvState().model, _modelOverride) != _boundModel)
    {
        LOG(Trace, "Ignore advertisement for a model other than the bound device.");
        return false;
    }

    LOG(Trace, "AirPods advertisement received. Data: {}, Address Hash: {}, RSSI: {}",
        Helper::ToString(adv.GetDesensitizedData()), Helper::Hash(data.address), data.rssi);

    auto optUpdateEvent = _stateMgr.OnAdvReceived(std::move(adv));
    if (optUpdateEvent.has_value()) {
        OnStateChanged(std::move(optUpdateEvent.value()), resetGeneration);
    }
    return true;
}

void Manager::OnAdvWatcherStateChanged(
    Bluetooth::AdvertisementWatcher::State state, const std::optional<std::string> &optError)
{
    switch (state) {
    case Bluetooth::AdvertisementWatcher::State::Started:
        _scannerRunning = true;
        emit ScannerAvailabilityChanged(true);
        LOG(Info, "Bluetooth AdvWatcher started.");
        Diagnostics::Recorder::Instance().Record(Diagnostics::Step::ScannerStarted, true);
        break;

    case Bluetooth::AdvertisementWatcher::State::Stopped:
        _scannerRunning = false;
        if (_scannerWanted) {
            emit ScannerAvailabilityChanged(false);
            LOG(Warn, "Bluetooth AdvWatcher stopped. Error: '{}'.", optError.value_or("nullopt"));
            Diagnostics::Recorder::Instance().Record(
                Diagnostics::Step::ScannerStarted, false,
                optError.value_or("stopped unexpectedly"));
            ScheduleScannerRetry();
        }
        else {
            LOG(Info, "Bluetooth AdvWatcher stopped intentionally.");
        }
        break;

    default:
        FatalError("Unhandled adv watcher state: '{}'", Helper::ToUnderlying(state));
    }
}

// AAP Protocol public methods

bool Manager::SetNoiseControlMode(AAP::NoiseControlMode mode)
{
    return _aapMgr.SetNoiseControlMode(mode);
}

std::optional<AAP::NoiseControlMode> Manager::GetNoiseControlMode() const
{
    return _aapMgr.GetNoiseControlMode();
}

bool Manager::SetConversationalAwareness(bool enable)
{
    return _aapMgr.SetConversationalAwareness(enable);
}

std::optional<AAP::ConversationalAwarenessState> Manager::GetConversationalAwarenessState() const
{
    return _aapMgr.GetConversationalAwarenessState();
}

bool Manager::SetPersonalizedVolume(bool enable)
{
    return _aapMgr.SetPersonalizedVolume(enable);
}

std::optional<AAP::PersonalizedVolumeState> Manager::GetPersonalizedVolumeState() const
{
    return _aapMgr.GetPersonalizedVolumeState();
}

bool Manager::SetLoudSoundReduction(bool enable)
{
    return _aapMgr.SetLoudSoundReduction(enable);
}

std::optional<AAP::LoudSoundReductionState> Manager::GetLoudSoundReductionState() const
{
    return _aapMgr.GetLoudSoundReductionState();
}

bool Manager::SetPpeCapLevel(uint8_t level)
{
    return _aapMgr.SetPpeCapLevel(level);
}

std::optional<uint8_t> Manager::GetPpeCapLevel() const
{
    return _aapMgr.GetPpeCapLevel();
}

bool Manager::SetAdaptiveNoiseLevel(uint8_t level)
{
    return _aapMgr.SetAdaptiveNoiseLevel(level);
}

bool Manager::IsAAPConnected() const
{
    return _aapMgr.IsConnected();
}

bool Manager::IsAAPConnectedViaMagicAAP() const
{
    return _aapMgr.IsConnectedViaMagicAAP();
}

AAP::AttPathState Manager::GetAAPAttPathState() const
{
    return _aapMgr.GetAttPathState();
}

AAP::ConnectFailure Manager::GetLastAAPConnectFailure() const
{
    return _aapMgr.GetLastConnectFailure();
}

bool Manager::StartHeadTracking()
{
    return _aapMgr.StartHeadTracking();
}

bool Manager::StopHeadTracking()
{
    return _aapMgr.StopHeadTracking();
}

bool Manager::IsHeadTrackingActive() const
{
    return _aapMgr.IsHeadTrackingActive();
}

void Manager::OnConversationalAwarenessChanged(bool enable)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _conversationalAwarenessEnabled = enable;

    if (_aapMgr.IsConnected()) {
        _aapMgr.SetConversationalAwareness(enable);
    }
}

void Manager::OnConversationalAwarenessVolumePercentChanged(uint8_t percent)
{
    std::lock_guard<std::mutex> lock{_mutex};
    // Clamp the value to valid range (10-100) matching UI slider constraints
    _conversationalAwarenessVolumePercent = std::clamp(percent, uint8_t{10}, uint8_t{100});
    LOG(Info, "Conversational awareness volume percent changed to {}%",
        _conversationalAwarenessVolumePercent);
}

void Manager::OnPersonalizedVolumeChanged(bool enable)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _personalizedVolumeEnabled = enable;

    if (_aapMgr.IsConnected()) {
        _aapMgr.SetPersonalizedVolume(enable);
    }
}

void Manager::OnLoudSoundReductionChanged(bool enable)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _loudSoundReductionEnabled = enable;

    if (_aapMgr.IsConnected()) {
        _aapMgr.SetLoudSoundReduction(enable);
    }
}

void Manager::OnPpeCapLevelChanged(uint8_t level)
{
    std::lock_guard<std::mutex> lock{_mutex};
    // Clamp at the domain boundary so a non-UI caller cannot bypass the 0-100 range.
    const auto normalized = AAP::NormalizePpeCapLevel(level);
    _ppeCapLevel = normalized;

    if (_aapMgr.IsConnected()) {
        _aapMgr.SetPpeCapLevel(normalized);
    }
}

void Manager::OnNoiseControlModeChanged(AAP::NoiseControlMode mode)
{
    std::lock_guard<std::mutex> lock{_mutex};

    if (_aapMgr.IsConnected()) {
        _aapMgr.SetNoiseControlMode(mode);
    }
}

// AAP callbacks

void Manager::OnNoiseControlModeNotification(AAP::NoiseControlMode mode, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "Noise control mode changed to: {}", Helper::ToString(mode).toStdString());

    // Track the current noise control mode
    _currentNoiseControlMode = static_cast<int>(mode);
}

void Manager::OnConversationalAwarenessStateChanged(
    AAP::ConversationalAwarenessState state, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "Conversational awareness state changed to: {}",
        Helper::ToString(state).toStdString());
}

void Manager::OnPersonalizedVolumeStateChanged(
    AAP::PersonalizedVolumeState state, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "Personalized volume state changed to: {}", static_cast<int>(state));
}

void Manager::OnLoudSoundReductionStateChanged(
    AAP::LoudSoundReductionState state, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "Loud sound reduction state changed to: {}", static_cast<int>(state));
}

void Manager::OnAutomaticEarDetectionNotification(bool enabled, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "Automatic ear detection notification: {}", enabled ? "enabled" : "disabled");
}

void Manager::OnPpeCapLevelNotification(uint8_t level, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "PPE cap level notification: {}", level);
}

void Manager::OnAttProbeResult(AAP::AttPathState state, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "AAP ATT path state: {}", static_cast<int>(state));
}

void Manager::OnEarDetectionChanged(
    AAP::EarStatus primary, AAP::EarStatus secondary, AAP::AAPAttempt attempt,
    uint64_t resetGeneration)
{
    if (resetGeneration != _earResetGeneration || !IsCurrentAAPAttempt(attempt)) {
        return;
    }
    LOG(Info, "Ear detection changed - Primary: {}, Secondary: {}", static_cast<int>(primary),
        static_cast<int>(secondary));

    // AAP callbacks run on its reader/connect worker. Capture epoch, StateManager generation,
    // and physical primary identity together before queueing; dequeue must not reinterpret the
    // report using a newer BLE role or a post-reset epoch.
    uint64_t epoch;
    uint64_t stateGeneration;
    std::optional<Side> primarySide;
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (resetGeneration != _earResetGeneration) {
            return;
        }
        const auto ble = _stateMgr.GetCurrentState();
        if (!ble.has_value()) {
            return;
        }
        epoch = _earDetection.Epoch();
        stateGeneration = _stateMgr.Generation();
        primarySide = ble->primarySide;
    }
    QMetaObject::invokeMethod(
        this,
        [this, primary, secondary, attempt, epoch, stateGeneration, primarySide, resetGeneration] {
            if (!IsCurrentAAPAttempt(attempt)) {
                return;
            }
            std::optional<EarState> changed;
            {
                std::lock_guard<std::mutex> lock{_mutex};
                if (resetGeneration != _earResetGeneration ||
                    !_stateMgr.IsCurrentGeneration(stateGeneration))
                {
                    return;
                }
                changed = _earDetection.UpdateAap(
                    EarStateFromAap(primary), EarStateFromAap(secondary), primarySide, attempt,
                    epoch,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        EarDetectionClock::now().time_since_epoch()));
                if (changed.has_value()) {
                    OnEarStateChanged(*changed);
                }
            }
            ScheduleEarDeadline();
        },
        Qt::QueuedConnection);
}

void Manager::OnAAPBatteryChanged(const AAP::AAPBatterySnapshot &snapshot, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        LOG(Trace, "AAP: Discard battery event from a stale session.");
        return;
    }

    bool changed = false;
    {
        std::lock_guard lock{_batteryFusionMutex};
        const auto bleLevel = [](const Details::BasicState &slot) -> std::optional<uint8_t> {
            if (!slot.battery.Available()) {
                return std::nullopt;
            }
            return static_cast<uint8_t>(std::min<uint32_t>(slot.battery.Value(), 100u));
        };
        Details::BatteryFusion::BleBaseline baseline;
        if (_lastBleState.has_value()) {
            baseline.left = bleLevel(_lastBleState->pods.left);
            baseline.right = bleLevel(_lastBleState->pods.right);
            baseline.caseBox = bleLevel(_lastBleState->caseBox);
        }
        changed = _batteryFusion.OnAapSnapshot(snapshot, baseline);
    }
    if (!changed) {
        return;
    }
    RequestFusedStatePublish();
}

void Manager::OverlayBattery(State &state) const
{
    // Caller holds `_batteryFusionMutex`.
    const auto overlay = [](const Details::BatteryFusion &fusion, AAP::BatteryComponent component,
                            Details::BasicState &slot) {
        std::optional<uint8_t> bleLevel;
        if (slot.battery.Available()) {
            bleLevel = static_cast<uint8_t>(std::min<uint32_t>(slot.battery.Value(), 100u));
        }
        const auto fused = fusion.Resolve(component, bleLevel);
        if (!fused.has_value()) {
            slot.batterySource = BatterySource::BLE;
            slot.sourceConflict = false;
            return;
        }

        slot.battery = Battery{fused->level};
        slot.isCharging = fused->charging;
        slot.batterySource = BatterySource::AAP;
        slot.sourceConflict = fused->conflict;
    };

    overlay(_batteryFusion, AAP::BatteryComponent::Left, state.pods.left);
    overlay(_batteryFusion, AAP::BatteryComponent::Right, state.pods.right);
    overlay(_batteryFusion, AAP::BatteryComponent::Case, state.caseBox);
}

void Manager::RequestFusedStatePublish()
{
    // BLE and AAP battery events arrive on different worker threads. Hopping to the thread
    // that owns Manager makes every StateUpdated delivery serialized there. The publish
    // recomputes from the newest BLE baseline and fusion at delivery time, so an older queued
    // request cannot overwrite a newer advertisement-driven state. A pending hop is dropped
    // with the object.
    QMetaObject::invokeMethod(this, [this] { PublishFusedState(); }, Qt::QueuedConnection);
}

void Manager::PublishFusedState()
{
    if (_publishingFusedState) {
        // A receiver may process queued events during StateUpdated. Do not let the nested
        // publish overtake the rest of the current emission; drain the newest state below.
        _fusedStatePublishPending = true;
        return;
    }

    _publishingFusedState = true;
    for (;;) {
        _fusedStatePublishPending = false;
        std::optional<State> fused;
        {
            std::lock_guard lock{_batteryFusionMutex};
            if (_lastBleState.has_value()) {
                fused = _lastBleState;
                OverlayBattery(*fused);
                if (_lastPublishedState != fused) {
                    _lastPublishedState = fused;
                }
                else {
                    fused.reset();
                }
            }
        }
        if (fused.has_value()) {
            emit StateUpdated(*fused);
        }
        if (!_fusedStatePublishPending) {
            break;
        }
    }
    _publishingFusedState = false;
}

void Manager::OnHeadTrackingData(AAP::HeadTrackingData data, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    // This callback receives real-time head tracking sensor data
    // Can be used for spatial audio or other applications
    LOG(Trace, "Head tracking: o1={}, o2={}, o3={}, hAccel={}, vAccel={}", data.orientation1,
        data.orientation2, data.orientation3, data.horizontalAcceleration,
        data.verticalAcceleration);
}

// Volume levels for conversational awareness
// kFullVolumePercent (100) signals to GlobalMedia::SetVolume to restore the saved pre-speaking
// volume The actual restoration logic is in GlobalMedia_win.cpp which restores to the saved volume,
// not literally 100%
constexpr int kFullVolumePercent = 100; // Signal value to restore to original volume

void Manager::OnSpeakingLevelChanged(AAP::SpeakingLevel level, AAP::AAPAttempt attempt)
{
    if (!IsCurrentAAPAttempt(attempt)) {
        return;
    }
    if (!_conversationalAwarenessEnabled) {
        return;
    }

    // Disable conversational awareness in transparency mode to avoid volume restoration bugs
    // The AAP firmware in transparency mode doesn't reliably send restoration events
    if (auto mode = _currentNoiseControlMode.load();
        mode >= 0 &&
        static_cast<AAP::NoiseControlMode>(mode) == AAP::NoiseControlMode::Transparency)
    {
        LOG(Info, "Conversational awareness disabled in transparency mode to avoid volume bugs");
        return;
    }

    switch (level) {
    case AAP::SpeakingLevel::StartedSpeaking_GreatlyReduce:
    case AAP::SpeakingLevel::StartedSpeaking_GreatlyReduce2:
        LOG(Info, "User started speaking - reducing media volume to {}%",
            _conversationalAwarenessVolumePercent);
        Core::GlobalMedia::SetVolume(_conversationalAwarenessVolumePercent);
        break;

    case AAP::SpeakingLevel::StoppedSpeaking:
    case AAP::SpeakingLevel::NormalVolume:
    case AAP::SpeakingLevel::NormalVolume2:
        LOG(Info, "User stopped speaking - restoring media volume");
        Core::GlobalMedia::SetVolume(kFullVolumePercent);
        break;

    default:
        // Intermediate levels (0x04-0x07) - restore volume to be safe
        // This ensures volume is restored even if final event is missed
        if (static_cast<uint8_t>(level) >= 0x04 && static_cast<uint8_t>(level) <= 0x07) {
            LOG(Info, "Intermediate speaking level detected - restoring media volume");
            Core::GlobalMedia::SetVolume(kFullVolumePercent);
        }
        break;
    }
}

void Manager::OnAAPConnected(AAP::AAPAttempt attempt)
{
    {
        std::lock_guard lock{_aapWorkerMutex};
        if (!_aapConnection.IsCurrent(attempt) || _aapShutdown) {
            return;
        }
        _aapConnection.TransportConnected(attempt);
        _aapConnection.Connected(attempt, AAPClock::now());
        _aapRetryScheduled = false;
    }

    LOG(Info, "AAP connection established - ANC features available");
    emit AapConnectionStateChangedSafely(true);

    // Apply user's conversational awareness preference. The preference fields are written
    // from settings callbacks on this same worker thread, so no queued reordering is needed.
    if (_conversationalAwarenessEnabled) {
        _aapMgr.SetConversationalAwareness(true);
    }
}

void Manager::OnAAPDisconnected(AAP::AAPAttempt attempt)
{
    std::optional<std::chrono::milliseconds> retryDelay;
    bool currentAttempt = false;
    {
        std::lock_guard lock{_aapWorkerMutex};
        currentAttempt = _aapConnection.IsCurrent(attempt);
        if (currentAttempt) {
            retryDelay = _aapConnection.Lost(attempt, AAPClock::now());
            if (retryDelay.has_value()) {
                _aapRetryAt = AAPClock::now() + *retryDelay;
                _aapRetryScheduled = true;
            }
            else {
                _aapRetryScheduled = false;
            }
        }
    }
    _aapWorkerCv.notify_all();
    _currentNoiseControlMode = -1;

    if (currentAttempt) {
        // The session is over: drop the overlay and republish the BLE-only state.
        {
            std::lock_guard lock{_batteryFusionMutex};
            _batteryFusion.Reset();
        }
        RequestFusedStatePublish();
    }

    LOG(Info, "AAP connection lost - ANC features unavailable");
    emit AapConnectionStateChangedSafely(false);

    // Invalidate synchronously under the same Manager mutex used by callback context capture.
    // Only QTimer rearm is queued, so a due timer cannot publish before disconnect cleanup.
    bool invalidated = false;
    {
        std::lock_guard<std::mutex> lock{_mutex};
        const auto epoch = _earDetection.Epoch();
        invalidated = _earDetection.ResetAap(attempt, epoch);
        if (invalidated) {
            _earResetGeneration.fetch_add(1);
        }
    }
    if (invalidated) {
        QMetaObject::invokeMethod(this, [this] { ScheduleEarDeadline(); }, Qt::QueuedConnection);
    }
}

std::vector<Bluetooth::Device> GetDevices()
{
    std::vector<Bluetooth::Device> devices =
        Bluetooth::DeviceManager::GetDevicesByState(Bluetooth::DeviceState::Paired);

    LOG(Info, "Paired devices count: {}", devices.size());

    devices.erase(
        std::remove_if(
            devices.begin(), devices.end(),
            [](const auto &device) {
                const auto vendorId = device.GetVendorId();
                const auto productId = device.GetProductId();
                const auto modelFromProductId = AppleCP::AirPods::GetModel(productId);
                const auto modelNumber = device.GetModelNumber();
                const auto modelFromNumber =
                    modelNumber.has_value() ? Details::ModelFromModelNumber(modelNumber.value())
                                            : std::nullopt;

                const auto doErase =
                    vendorId != AppleCP::VendorId ||
                    (modelFromProductId == AirPods::Model::Unknown && !modelFromNumber.has_value());

                LOG(Trace,
                    "Device VendorId: '{}', ProductId: '{}', modelId: '{}', modelNumber: '{}', "
                    "doErase: {}",
                    vendorId, productId, Helper::ToString(modelFromProductId),
                    modelNumber.value_or(""), doErase);

                return doErase;
            }),
        devices.end());

    LOG(Info, "AirPods devices count: {} (filtered)", devices.size());
    return devices;
}

} // namespace Core::AirPods
