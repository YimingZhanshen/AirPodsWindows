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

#include "AirPods.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <QVector>
#include <QMetaObject>

#include "Bluetooth.h"
#include "GlobalMedia.h"
#include "../Helper.h"
#include "../Logger.h"
#include "../Assert.h"
#include "../Application.h"
#include "../Gui/MainWindow.h"

using namespace Core;
using namespace std::chrono_literals;

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

std::optional<Model> ModelFromModelNumber(std::string_view modelNumber)
{
    static const std::unordered_map<std::string, Model> kModelMap{
        {"A1523", Model::AirPods_1},
        {"A1722", Model::AirPods_1},
        {"A2032", Model::AirPods_2},
        {"A2031", Model::AirPods_2},
        {"A2565", Model::AirPods_3},
        {"A2564", Model::AirPods_3},
        {"A3053", Model::AirPods_4},
        {"A3050", Model::AirPods_4},
        {"A3054", Model::AirPods_4},
        {"A3056", Model::AirPods_4_ANC},
        {"A3055", Model::AirPods_4_ANC},
        {"A3057", Model::AirPods_4_ANC},
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

} // namespace

namespace Details {

//
// Advertisement
//

bool Advertisement::IsDesiredAdv(const Bluetooth::AdvertisementWatcher::ReceivedData &data)
{
    auto iter = data.manufacturerDataMap.find(AppleCP::VendorId);
    if (iter == data.manufacturerDataMap.end()) {
        return false;
    }

    const auto &manufacturerData = (*iter).second;
    if (!AppleCP::AirPods::IsValid(manufacturerData)) {
        return false;
    }

    return true;
}

Advertisement::Advertisement(const Bluetooth::AdvertisementWatcher::ReceivedData &data)
{
    APD_ASSERT(IsDesiredAdv(data));
    _data = data;

    auto protocol = AppleCP::As<AppleCP::AirPods>(GetMfrData());
    APD_ASSERT(protocol.has_value());
    _protocol = std::move(protocol.value());

    // Store state
    //

    _state.model = _protocol.GetModel();
    _state.side = _protocol.GetBroadcastedSide();

    _state.pods.left.battery = _protocol.GetLeftBattery();
    _state.pods.left.isCharging = _protocol.IsLeftCharging();
    _state.pods.left.isInEar = _protocol.IsLeftInEar();

    _state.pods.right.battery = _protocol.GetRightBattery();
    _state.pods.right.isCharging = _protocol.IsRightCharging();
    _state.pods.right.isInEar = _protocol.IsRightInEar();

    _state.caseBox.battery = _protocol.GetCaseBattery();
    _state.caseBox.isCharging = _protocol.IsCaseCharging();

    _state.caseBox.isBothPodsInCase = _protocol.IsBothPodsInCase();
    _state.caseBox.isLidOpened = _protocol.IsLidOpened();

    if (_state.pods.left.battery.Available()) {
        _state.pods.left.battery = _state.pods.left.battery.Value() * 10;
    }
    if (_state.pods.right.battery.Available()) {
        _state.pods.right.battery = _state.pods.right.battery.Value() * 10;
    }
    if (_state.caseBox.battery.Available()) {
        _state.caseBox.battery = _state.caseBox.battery.Value() * 10;
    }
}

int16_t Advertisement::GetRssi() const
{
    return _data.rssi;
}

const auto &Advertisement::GetTimestamp() const
{
    return _data.timestamp;
}

auto Advertisement::GetAddress() const -> AddressType
{
    return _data.address;
}

std::vector<uint8_t> Advertisement::GetDesensitizedData() const
{
    auto desensitizedData = _protocol.Desensitize();

    std::vector<uint8_t> result(sizeof(desensitizedData), 0);
    std::memcpy(result.data(), &desensitizedData, sizeof(desensitizedData));
    return result;
}

auto Advertisement::GetAdvState() const -> const AdvState &
{
    return _state;
}

const std::vector<uint8_t> &Advertisement::GetMfrData() const
{
    auto iter = _data.manufacturerDataMap.find(AppleCP::VendorId);
    APD_ASSERT(iter != _data.manufacturerDataMap.end());

    return (*iter).second;
}

//
// StateManager
//

StateManager::StateManager()
{
    _lostTimer.Start(10s, [this] {
        std::lock_guard<std::mutex> lock{_mutex};
        DoLost();
    });

    _stateResetTimer.left.Start(10s, [this] {
        std::lock_guard<std::mutex> lock{_mutex};
        DoStateReset(Side::Left);
    });

    _stateResetTimer.right.Start(10s, [this] {
        std::lock_guard<std::mutex> lock{_mutex};
        DoStateReset(Side::Right);
    });
}

std::optional<State> StateManager::GetCurrentState() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _cachedState;
}

auto StateManager::OnAdvReceived(Advertisement adv) -> std::optional<UpdateEvent>
{
    std::lock_guard<std::mutex> lock{_mutex};

    if (!IsPossibleDesiredAdv(adv)) {
        LOG(Warn, "This adv may not be broadcast from the device we desire.");
        return std::nullopt;
    }

    UpdateAdv(std::move(adv));
    return UpdateState();
}

void StateManager::Disconnect()
{
    std::lock_guard<std::mutex> lock{_mutex};

    LOG(Info, "StateManager: Disconnect.");
    ResetAll();
}

void StateManager::OnRssiMinChanged(int16_t rssiMin)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _rssiMin = rssiMin;
}

bool StateManager::IsPossibleDesiredAdv(const Advertisement &adv) const
{
    const auto advRssi = adv.GetRssi();
    if (advRssi < _rssiMin) {
        LOG(Warn,
            "IsPossibleDesiredAdv returns false. Reason: RSSI is less than the limit. "
            "curr: '{}' min: '{}'",
            advRssi, _rssiMin);
        return false;
    }

    const auto &advState = adv.GetAdvState();

    auto &lastAdv = advState.side == Side::Left ? _adv.left : _adv.right;
    auto &lastAnotherAdv = advState.side == Side::Left ? _adv.right : _adv.left;

    // If the Random Non-resolvable Address of our devices is changed
    // or the packet is sent from another device that it isn't ours
    //
    if (lastAdv.has_value() && lastAdv->first.GetAddress() != adv.GetAddress()) {
        const auto &lastAdvState = lastAdv->first.GetAdvState();

        if (advState.model != lastAdvState.model) {
            LOG(Warn, "IsPossibleDesiredAdv returns false. Reason: model new='{}' old='{}'",
                Helper::ToString(advState.model), Helper::ToString(lastAdvState.model));
            return false;
        }

        Battery::ValueType leftBatteryDiff = 0, rightBatteryDiff = 0, caseBatteryDiff = 0;

        using SignedBatteryValueT = std::make_signed_t<Battery::ValueType>;

        if (advState.pods.left.battery.Available() && lastAdvState.pods.left.battery.Available()) {
            leftBatteryDiff = std::abs(
                static_cast<SignedBatteryValueT>(advState.pods.left.battery.Value()) -
                static_cast<SignedBatteryValueT>(lastAdvState.pods.left.battery.Value()));
        }
        if (advState.pods.right.battery.Available() && lastAdvState.pods.right.battery.Available())
        {
            rightBatteryDiff = std::abs(
                static_cast<SignedBatteryValueT>(advState.pods.right.battery.Value()) -
                static_cast<SignedBatteryValueT>(lastAdvState.pods.right.battery.Value()));
        }
        if (advState.caseBox.battery.Available() && lastAdvState.caseBox.battery.Available()) {
            caseBatteryDiff = std::abs(
                static_cast<SignedBatteryValueT>(advState.caseBox.battery.Value()) -
                static_cast<SignedBatteryValueT>(lastAdvState.caseBox.battery.Value()));
        }

        // The battery changes in steps of 1, so the data of two packets in a short time
        // can not exceed 1, otherwise it is not our device
        //
        if (leftBatteryDiff > 1 || rightBatteryDiff > 1 || caseBatteryDiff > 1) {
            LOG(Warn,
                "IsPossibleDesiredAdv returns false. Reason: BatteryDiff l='{}' r='{}' c='{}'",
                leftBatteryDiff, rightBatteryDiff, caseBatteryDiff);
            return false;
        }

        int16_t rssiDiff = std::abs(advRssi - lastAdv->first.GetRssi());
        if (rssiDiff > 50) {
            LOG(Warn, "IsPossibleDesiredAdv returns false. Reason: Current side rssiDiff '{}'",
                rssiDiff);
            return false;
        }

        LOG(Warn, "Address changed, but it might still be the same device.");
    }

    if (lastAnotherAdv.has_value()) {
        int16_t rssiDiff = std::abs(advRssi - lastAnotherAdv->first.GetRssi());
        if (rssiDiff > 50) {
            LOG(Warn, "IsPossibleDesiredAdv returns false. Reason: Another side rssiDiff '{}'",
                rssiDiff);
            return false;
        }
    }

    return true;
}

void StateManager::UpdateAdv(Advertisement adv)
{
    _lostTimer.Reset();

    const auto &advState = adv.GetAdvState();

    if (advState.side == Side::Left) {
        _stateResetTimer.left.Reset();
        _adv.left = std::make_pair(std::move(adv), Clock::now());
    }
    else if (advState.side == Side::Right) {
        _stateResetTimer.right.Reset();
        _adv.right = std::make_pair(std::move(adv), Clock::now());
    }
}

auto StateManager::UpdateState() -> std::optional<UpdateEvent>
{
    Helper::Sides<std::pair<Advertisement::AdvState, Timestamp>> cachedAdvState;

    if (_adv.left.has_value()) {
        cachedAdvState.left = std::make_pair(_adv.left->first.GetAdvState(), _adv.left->second);
    }
    if (_adv.right.has_value()) {
        cachedAdvState.right = std::make_pair(_adv.right->first.GetAdvState(), _adv.right->second);
    }

    State newState;

#define PICK_SIDE(available_condition_with_field)                                                  \
    [&]() -> decltype(auto) {                                                                      \
        const Helper::Sides<bool> available = {                                                    \
            .left = cachedAdvState.left.first.available_condition_with_field,                      \
            .right = cachedAdvState.right.first.available_condition_with_field,                    \
        };                                                                                         \
        if (available.left && available.right) {                                                   \
            return cachedAdvState.left.second > cachedAdvState.right.second                        \
                       ? cachedAdvState.left.first                                                 \
                       : cachedAdvState.right.first;                                               \
        }                                                                                          \
        else {                                                                                     \
            return available.left ? cachedAdvState.left.first : cachedAdvState.right.first;        \
        }                                                                                          \
    }()

    newState.model = PICK_SIDE(model != Model::Unknown).model;
    newState.pods.left = std::move(PICK_SIDE(pods.left.battery.Available()).pods.left);
    newState.pods.right = std::move(PICK_SIDE(pods.right.battery.Available()).pods.right);
    newState.caseBox = std::move(PICK_SIDE(caseBox.battery.Available()).caseBox);

#undef PICK_SIDE

    if (newState == _cachedState) {
        return std::nullopt;
    }

    auto oldState = std::move(_cachedState);
    _cachedState = std::move(newState);

    return UpdateEvent{.oldState = std::move(oldState), .newState = _cachedState.value()};
}

void StateManager::ResetAll()
{
    if (_cachedState.has_value()) {
        ApdApp->GetMainWindow()->DisconnectSafely();
    }

    _adv.left.reset();
    _adv.right.reset();
    _cachedState.reset();
}

void StateManager::DoLost()
{
    if (_cachedState.has_value()) {
        LOG(Info, "StateManager: Device is lost.");
    }
    ResetAll();
}

void StateManager::DoStateReset(Side side)
{
    auto &adv = side == Side::Left ? _adv.left : _adv.right;
    if (adv.has_value()) {
        LOG(Info, "StateManager: DoStateReset called. Side: {}", Helper::ToString(side));
        adv.reset();
    }
}
} // namespace Details

//
// Manager
//

Manager::Manager()
{
    _adWatcher.CbReceived() += [this](auto &&...args) {
        std::lock_guard<std::mutex> lock{_mutex};
        OnAdvertisementReceived(std::forward<decltype(args)>(args)...);
    };

    _adWatcher.CbStateChanged() += [this](auto &&...args) {
        std::lock_guard<std::mutex> lock{_mutex};
        OnAdvWatcherStateChanged(std::forward<decltype(args)>(args)...);
    };

    SetupAAPCallbacks();
}

void Manager::SetupAAPCallbacks()
{
    AAP::Callbacks callbacks;
    
    callbacks.onNoiseControlChanged = [this](AAP::NoiseControlMode mode) {
        OnNoiseControlModeNotification(mode);
    };
    
    callbacks.onConversationalAwarenessChanged = [this](AAP::ConversationalAwarenessState state) {
        OnConversationalAwarenessStateChanged(state);
    };
    
    callbacks.onPersonalizedVolumeChanged = [this](AAP::PersonalizedVolumeState state) {
        OnPersonalizedVolumeStateChanged(state);
    };
    
    callbacks.onLoudSoundReductionChanged = [this](AAP::LoudSoundReductionState state) {
        OnLoudSoundReductionStateChanged(state);
    };
    
    callbacks.onAdaptiveTransparencyLevelChanged = [this](uint8_t level) {
        OnAdaptiveTransparencyLevelNotification(level);
    };
    
    callbacks.onSpeakingLevelChanged = [this](AAP::SpeakingLevel level) {
        OnSpeakingLevelChanged(level);
    };
    
    callbacks.onEarDetectionChanged = [this](AAP::EarStatus primary, AAP::EarStatus secondary) {
        OnEarDetectionChanged(primary, secondary);
    };
    
    callbacks.onHeadTrackingData = [this](AAP::HeadTrackingData data) {
        OnHeadTrackingData(data);
    };
    
    callbacks.onConnected = [this]() {
        OnAAPConnected();
    };
    
    callbacks.onDisconnected = [this]() {
        OnAAPDisconnected();
    };
    
    _aapMgr.SetCallbacks(std::move(callbacks));
}

void Manager::StartScanner()
{
    if (!_adWatcher.Start()) {
        LOG(Warn, "Bluetooth AdvWatcher start failed.");
    }
    else {
        LOG(Info, "Bluetooth AdvWatcher start succeeded.");
    }
}

void Manager::StopScanner()
{
    if (!_adWatcher.Stop()) {
        LOG(Warn, "AsyncScanner::Stop() failed.");
    }
    else {
        LOG(Info, "AsyncScanner::Stop() succeeded.");
    }
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

void Manager::OnBoundDeviceAddressChanged(uint64_t address)
{
    std::unique_lock<std::mutex> lock{_mutex};

    _boundDevice.reset();
    _modelOverride.reset();
    _deviceConnected = false;
    _stateMgr.Disconnect();
    
    // Disconnect AAP if connected
    _aapMgr.Disconnect();

    // Unbind device
    //
    if (address == 0) {
        LOG(Info, "Unbind device.");
        return;
    }

    // Bind to a new device
    //
    LOG(Info, "Bind a new device.");

    auto optDevice = Bluetooth::DeviceManager::FindDevice(address);
    if (!optDevice.has_value()) {
        LOG(Error, "Find device by address failed.");
        return;
    }

    _boundDevice = std::move(optDevice);

    if (auto modelNumber = _boundDevice->GetModelNumber(); modelNumber.has_value()) {
        _modelOverride = ModelFromModelNumber(modelNumber.value());
        if (_modelOverride.has_value()) {
            LOG(Info, "Detected model number '{}', override model: {}", modelNumber.value(),
                Helper::ToString(_modelOverride.value()));
        }
    }

    _deviceName = QString::fromStdString([&] {
        auto name = _boundDevice->GetName();
        // See https://github.com/SpriteOvO/AirPodsDesktop/issues/15
        return name.find("Bluetooth") != std::string::npos ? std::string{} : name;
    }());

    _boundDevice->CbConnectionStatusChanged() += [this](auto &&...args) {
        std::lock_guard<std::mutex> lock{_mutex};
        OnBoundDeviceConnectionStateChanged(std::forward<decltype(args)>(args)...);
    };

    OnBoundDeviceConnectionStateChanged(_boundDevice->GetConnectionState());
}

void Manager::OnBoundDeviceConnectionStateChanged(Bluetooth::DeviceState state)
{
    bool newDeviceConnected = state == Bluetooth::DeviceState::Connected;
    bool doDisconnect = _deviceConnected && !newDeviceConnected;
    bool doConnect = !_deviceConnected && newDeviceConnected;
    
    LOG(Info, "OnBoundDeviceConnectionStateChanged: state={}, _deviceConnected={}, newDeviceConnected={}, doConnect={}",
        static_cast<int>(state), _deviceConnected, newDeviceConnected, doConnect);
    
    _deviceConnected = newDeviceConnected;

    if (doDisconnect) {
        _stateMgr.Disconnect();
        _aapMgr.Disconnect();
    }
    
    if (doConnect && _boundDevice.has_value()) {
        // Try to connect AAP for devices that support it
        ConnectAAP();
    }

    LOG(Info, "The device we bound is updated. current: {}, new: {}", _deviceConnected,
        newDeviceConnected);
}

void Manager::ConnectAAP()
{
    LOG(Info, "ConnectAAP called");
    
    if (!_boundDevice.has_value()) {
        LOG(Warn, "ConnectAAP: No bound device");
        return;
    }
    
    // Check if already connected or connecting
    if (_aapMgr.IsConnected()) {
        LOG(Info, "ConnectAAP: Already connected");
        return;
    }
    
    auto state = _stateMgr.GetCurrentState();
    auto modelToCheck = state.has_value() ? state->model : Model::Unknown;
    LOG(Info, "ConnectAAP: state.has_value={}, state.model={}, _modelOverride.has_value={}",
        state.has_value(), 
        state.has_value() ? static_cast<int>(state->model) : -1,
        _modelOverride.has_value());
    
    if (modelToCheck == Model::Unknown && _modelOverride.has_value()) {
        modelToCheck = _modelOverride.value();
    }
    
    LOG(Info, "ConnectAAP: modelToCheck={}, SupportsANC={}", 
        static_cast<int>(modelToCheck), SupportsANC(modelToCheck));

    if (SupportsANC(modelToCheck)) {
        uint64_t address = _boundDevice->GetAddress();
        LOG(Info, "Attempting AAP connection for ANC-capable device, address={:016X}", address);
        
        // Connect in a separate thread to avoid blocking the main thread
        // Note: AAPManager::Connect() is thread-safe and handles its own locking
        std::thread([this, address]() {
            if (_aapMgr.Connect(address)) {
                LOG(Info, "AAP connection established successfully");
            } else {
                LOG(Warn, "AAP connection failed - ANC features will not be available");
            }
        }).detach();
    } else {
        LOG(Info, "ConnectAAP: Device does not support ANC");
    }
}

bool Manager::SupportsANC(Model model)
{
    switch (model) {
        case Model::AirPods_Pro:
        case Model::AirPods_Pro_2:
        case Model::AirPods_Pro_2_USB_C:
        case Model::AirPods_Pro_3:
        case Model::AirPods_4_ANC:
        case Model::AirPods_Max:
        case Model::AirPods_Max_USB_C:
            return true;
        default:
            return false;
    }
}

void Manager::OnStateChanged(Details::StateManager::UpdateEvent updateEvent)
{
    const auto &oldState = updateEvent.oldState;
    auto &newState = updateEvent.newState;

    if (newState.model == Model::Unknown && _modelOverride.has_value()) {
        newState.model = _modelOverride.value();
    }

    newState.displayName =
        _deviceName.isEmpty() ? Helper::ToString(newState.model) : _deviceName.remove(" - Find My");

    ApdApp->GetMainWindow()->UpdateStateSafely(newState);

    // Try to connect AAP if we have a valid model now and device is connected
    if (_deviceConnected && !_aapMgr.IsConnected() && SupportsANC(newState.model)) {
        LOG(Info, "OnStateChanged: Device supports ANC and AAP not connected, attempting connection");
        ConnectAAP();
    }

    // Lid opened
    //
    bool newLidOpened = newState.caseBox.isLidOpened && newState.caseBox.isBothPodsInCase;
    bool lidStateSwitched;
    if (!oldState.has_value()) {
        lidStateSwitched = newLidOpened;
    }
    else {
        bool oldLidOpened = oldState->caseBox.isLidOpened && oldState->caseBox.isBothPodsInCase;
        lidStateSwitched = oldLidOpened != newLidOpened;
    }
    if (lidStateSwitched) {
        OnLidOpened(newLidOpened);
    }

    // Both in ear
    //
    if (oldState.has_value()) {
        bool oldBothInEar = oldState->pods.left.isInEar && oldState->pods.right.isInEar;
        bool newBothInEar = newState.pods.left.isInEar && newState.pods.right.isInEar;
        if (oldBothInEar != newBothInEar) {
            OnBothInEar(newBothInEar);
        }
    }
}

void Manager::OnLidOpened(bool opened)
{
    auto &mainWindow = ApdApp->GetMainWindow();
    if (opened) {
        mainWindow->ShowSafely();
    }
    else {
        mainWindow->HideSafely();
    }
}

void Manager::OnBothInEar(bool isBothInEar)
{
    if (!_automaticEarDetection) {
        LOG(Info, "automatic_ear_detection: Do nothing because it is disabled. ({})", isBothInEar);
        return;
    }

    if (isBothInEar) {
        Core::GlobalMedia::Play();
    }
    else {
        Core::GlobalMedia::Pause();
    }
}

bool Manager::OnAdvertisementReceived(const Bluetooth::AdvertisementWatcher::ReceivedData &data)
{
    if (!Details::Advertisement::IsDesiredAdv(data)) {
        return false;
    }

    Details::Advertisement adv{data};

    LOG(Trace, "AirPods advertisement received. Data: {}, Address Hash: {}, RSSI: {}",
        Helper::ToString(adv.GetDesensitizedData()), Helper::Hash(data.address), data.rssi);

    if (!_deviceConnected) {
        LOG(Info, "AirPods advertisement received, but device disconnected.");
        return false;
    }

    auto optUpdateEvent = _stateMgr.OnAdvReceived(Details::Advertisement{data});
    if (optUpdateEvent.has_value()) {
        OnStateChanged(std::move(optUpdateEvent.value()));
    }
    return true;
}

void Manager::OnAdvWatcherStateChanged(
    Bluetooth::AdvertisementWatcher::State state, const std::optional<std::string> &optError)
{
    switch (state) {
    case Core::Bluetooth::AdvertisementWatcher::State::Started:
        ApdApp->GetMainWindow()->AvailableSafely();
        LOG(Info, "Bluetooth AdvWatcher started.");
        break;

    case Core::Bluetooth::AdvertisementWatcher::State::Stopped:
        ApdApp->GetMainWindow()->UnavailableSafely();
        LOG(Warn, "Bluetooth AdvWatcher stopped. Error: '{}'.", optError.value_or("nullopt"));
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

bool Manager::SetAdaptiveTransparencyLevel(uint8_t level)
{
    return _aapMgr.SetAdaptiveTransparencyLevel(level);
}

std::optional<uint8_t> Manager::GetAdaptiveTransparencyLevel() const
{
    return _aapMgr.GetAdaptiveTransparencyLevel();
}

bool Manager::SetAdaptiveNoiseLevel(uint8_t level)
{
    return _aapMgr.SetAdaptiveNoiseLevel(level);
}

bool Manager::IsAAPConnected() const
{
    return _aapMgr.IsConnected();
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

void Manager::OnAdaptiveTransparencyLevelChanged(uint8_t level)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _adaptiveTransparencyLevel = level;
    
    if (_aapMgr.IsConnected()) {
        _aapMgr.SetAdaptiveTransparencyLevel(level);
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

void Manager::OnNoiseControlModeNotification(AAP::NoiseControlMode mode)
{
    LOG(Info, "Noise control mode changed to: {}", Helper::ToString(mode).toStdString());
    
    // Update the cached state in the state manager if we have a current state
    auto state = _stateMgr.GetCurrentState();
    if (state.has_value()) {
        // Notify UI about noise control change
        // The state will be updated when we next receive an advertisement
    }
}

void Manager::OnConversationalAwarenessStateChanged(AAP::ConversationalAwarenessState state)
{
    LOG(Info, "Conversational awareness state changed to: {}", Helper::ToString(state).toStdString());
}

void Manager::OnPersonalizedVolumeStateChanged(AAP::PersonalizedVolumeState state)
{
    LOG(Info, "Personalized volume state changed to: {}", static_cast<int>(state));
}

void Manager::OnLoudSoundReductionStateChanged(AAP::LoudSoundReductionState state)
{
    LOG(Info, "Loud sound reduction state changed to: {}", static_cast<int>(state));
}

void Manager::OnAdaptiveTransparencyLevelNotification(uint8_t level)
{
    LOG(Info, "Adaptive transparency level changed to: {}", level);
}

void Manager::OnEarDetectionChanged(AAP::EarStatus primary, AAP::EarStatus secondary)
{
    LOG(Info, "Ear detection changed - Primary: {}, Secondary: {}", 
        static_cast<int>(primary), static_cast<int>(secondary));
    
    // Handle automatic pause/resume based on ear detection
    if (!_automaticEarDetection) {
        return;
    }
    
    bool bothInEar = (primary == AAP::EarStatus::InEar && secondary == AAP::EarStatus::InEar);
    bool bothOutOfEar = (primary != AAP::EarStatus::InEar && secondary != AAP::EarStatus::InEar);
    
    if (bothOutOfEar) {
        LOG(Info, "Both AirPods out of ear - pausing media");
        Core::GlobalMedia::Pause();
    }
    // Note: We don't auto-resume when put back in ear to avoid unexpected playback
}

void Manager::OnHeadTrackingData(AAP::HeadTrackingData data)
{
    // This callback receives real-time head tracking sensor data
    // Can be used for spatial audio or other applications
    LOG(Trace, "Head tracking: o1={}, o2={}, o3={}, hAccel={}, vAccel={}", 
        data.orientation1, data.orientation2, data.orientation3,
        data.horizontalAcceleration, data.verticalAcceleration);
}

// Volume levels for conversational awareness
constexpr int kConversationalAwarenessVolumePercent = 40;  // Volume when user is speaking
constexpr int kFullVolumePercent = 100;  // Normal volume when not speaking

void Manager::OnSpeakingLevelChanged(AAP::SpeakingLevel level)
{
    if (!_conversationalAwarenessEnabled) {
        return;
    }
    
    switch (level) {
        case AAP::SpeakingLevel::StartedSpeaking_GreatlyReduce:
        case AAP::SpeakingLevel::StartedSpeaking_GreatlyReduce2:
            LOG(Info, "User started speaking - reducing media volume to {}%", kConversationalAwarenessVolumePercent);
            Core::GlobalMedia::SetVolume(kConversationalAwarenessVolumePercent);
            break;
            
        case AAP::SpeakingLevel::StoppedSpeaking:
        case AAP::SpeakingLevel::NormalVolume:
        case AAP::SpeakingLevel::NormalVolume2:
            LOG(Info, "User stopped speaking - restoring media volume");
            Core::GlobalMedia::SetVolume(kFullVolumePercent);
            break;
            
        default:
            // Intermediate levels - could implement gradual volume adjustment
            break;
    }
}

void Manager::OnAAPConnected()
{
    LOG(Info, "AAP connection established - ANC features available");
    
    // Apply user's conversational awareness preference
    if (_conversationalAwarenessEnabled) {
        _aapMgr.SetConversationalAwareness(true);
    }
}

void Manager::OnAAPDisconnected()
{
    LOG(Info, "AAP connection lost - ANC features unavailable");
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
                const auto modelFromNumber = modelNumber.has_value()
                    ? ModelFromModelNumber(modelNumber.value())
                    : std::nullopt;

                const auto doErase =
                    vendorId != AppleCP::VendorId ||
                    (modelFromProductId == AirPods::Model::Unknown && !modelFromNumber.has_value());

                LOG(Trace,
                    "Device VendorId: '{}', ProductId: '{}', modelId: '{}', modelNumber: '{}', doErase: {}",
                    vendorId, productId, Helper::ToString(modelFromProductId),
                    modelNumber.value_or(""), doErase);

                return doErase;
            }),
        devices.end());

    LOG(Info, "AirPods devices count: {} (filtered)", devices.size());
    return devices;
}

} // namespace Core::AirPods
