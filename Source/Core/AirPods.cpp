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

#include "GlobalMedia.h"
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

QString Details::ResolveDisplayName(QString deviceName, Model model)
{
    deviceName.remove(" - Find My");

    const bool isDefaultModelName =
        (model == Model::AirPods_Max_USB_C && deviceName == "AirPods Max") ||
        (model == Model::AirPods_5 && deviceName == "AirPods");
    return deviceName.isEmpty() || isDefaultModelName ? Helper::ToString(model) : deviceName;
}

Manager::Manager(QObject *parent) : QObject{parent}
{
    _stateMgr.SetOnDiscardState([this] {
        QMetaObject::invokeMethod(this, [this] { emit Disconnected(); }, Qt::QueuedConnection);
    });

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

Manager::~Manager()
{
    StopScanner();

    _deviceLookupThread.request_stop();
    if (_deviceLookupThread.joinable()) {
        _deviceLookupThread.join();
    }
}

void Manager::StartScanner()
{
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (!_boundDevice.has_value() || !_deviceConnected) {
            LOG(Info, "Skip Bluetooth AdvWatcher start because the bound device is disconnected.");
            return;
        }
        if (_scannerWanted) {
            return;
        }
        _scannerWanted = true;
    }

    if (!_adWatcher.Start()) {
        std::lock_guard<std::mutex> lock{_mutex};
        _scannerWanted = false;
        LOG(Warn, "Bluetooth AdvWatcher start failed.");
    }
    else {
        LOG(Info, "Bluetooth AdvWatcher start succeeded.");
    }
}

void Manager::StopScanner()
{
    {
        std::lock_guard<std::mutex> lock{_mutex};
        if (!_scannerWanted) {
            return;
        }
        _scannerWanted = false;
    }

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
    _deviceLookupThread.request_stop();
    if (_deviceLookupThread.joinable()) {
        _deviceLookupThread.join();
    }

    std::lock_guard<std::mutex> lock{_mutex};

    _requestedDeviceAddress = address;
    _boundDevice.reset();
    _modelOverride.reset();
    _boundModel = Model::Unknown;
    _deviceConnected = false;
    _stateMgr.Disconnect();

    // Disconnect AAP if connected
    _aapMgr.Disconnect();

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
    _boundModel = AppleCP::AirPods::GetModel(_boundDevice->GetProductId());

    if (auto modelNumber = _boundDevice->GetModelNumber(); modelNumber.has_value()) {
        _modelOverride = ModelFromModelNumber(modelNumber.value());
        if (_modelOverride.has_value()) {
            LOG(Info, "Detected model number '{}', override model: {}", modelNumber.value(),
                Helper::ToString(_modelOverride.value()));
        }
    }

    _deviceName = QString::fromStdString([&] {
        auto name = _boundDevice->GetName();
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
    const bool oldDeviceConnected = _deviceConnected;
    bool newDeviceConnected = state == Bluetooth::DeviceState::Connected;
    bool doDisconnect = oldDeviceConnected && !newDeviceConnected;
    bool doConnect = !oldDeviceConnected && newDeviceConnected;

    LOG(Info, "OnBoundDeviceConnectionStateChanged: state={}, _deviceConnected={}, newDeviceConnected={}, doConnect={}",
        static_cast<int>(state), oldDeviceConnected, newDeviceConnected, doConnect);

    _deviceConnected = newDeviceConnected;
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
        _stateMgr.Disconnect();
        _aapMgr.Disconnect();
    }
    
    if (doConnect && _boundDevice.has_value()) {
        // Try to connect AAP for devices that support it
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

    newState.displayName = Details::ResolveDisplayName(_deviceName, newState.model);

    emit StateUpdated(newState);

    // Try to connect AAP if we have a valid model now and device is connected
    if (_deviceConnected && !_aapMgr.IsConnected() && SupportsANC(newState.model)) {
        LOG(Info, "OnStateChanged: Device supports ANC and AAP not connected, attempting connection");
        ConnectAAP();
    }

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
    emit LidToggled(opened);
}

void Manager::OnBothInEar(bool isBothInEar)
{
    if (!_automaticEarDetection) {
        LOG(Info, "automatic_ear_detection: Do nothing because it is disabled. ({})", isBothInEar);
        return;
    }

    if (isBothInEar) {
        GlobalMedia::Play();
    }
    else {
        GlobalMedia::Pause();
    }
}

bool Manager::OnAdvertisementReceived(const Bluetooth::AdvertisementWatcher::ReceivedData &data)
{
    if (!Details::Advertisement::IsDesiredAdv(data)) {
        return false;
    }

    Details::Advertisement adv{data};

    if (!_deviceConnected) {
        LOG(Info, "AirPods advertisement received, but device disconnected.");
        return false;
    }

    if (_boundModel != Model::Unknown && adv.GetAdvState().model != _boundModel) {
        LOG(Trace, "Ignore advertisement for a model other than the bound device.");
        return false;
    }

    LOG(Trace, "AirPods advertisement received. Data: {}, Address Hash: {}, RSSI: {}",
        Helper::ToString(adv.GetDesensitizedData()), Helper::Hash(data.address), data.rssi);

    auto optUpdateEvent = _stateMgr.OnAdvReceived(std::move(adv));
    if (optUpdateEvent.has_value()) {
        OnStateChanged(std::move(optUpdateEvent.value()));
    }
    return true;
}

void Manager::OnAdvWatcherStateChanged(
    Bluetooth::AdvertisementWatcher::State state, const std::optional<std::string> &optError)
{
    switch (state) {
    case Bluetooth::AdvertisementWatcher::State::Started:
        emit ScannerAvailabilityChanged(true);
        LOG(Info, "Bluetooth AdvWatcher started.");
        break;

    case Bluetooth::AdvertisementWatcher::State::Stopped:
        if (_scannerWanted) {
            emit ScannerAvailabilityChanged(false);
            LOG(Warn, "Bluetooth AdvWatcher stopped. Error: '{}'.", optError.value_or("nullopt"));
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

void Manager::OnConversationalAwarenessVolumePercentChanged(uint8_t percent)
{
    std::lock_guard<std::mutex> lock{_mutex};
    // Clamp the value to valid range (10-100) matching UI slider constraints
    _conversationalAwarenessVolumePercent = std::clamp(percent, uint8_t{10}, uint8_t{100});
    LOG(Info, "Conversational awareness volume percent changed to {}%", _conversationalAwarenessVolumePercent);
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
    
    // Track the current noise control mode
    _currentNoiseControlMode = mode;
    
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
// kFullVolumePercent (100) signals to GlobalMedia::SetVolume to restore the saved pre-speaking volume
// The actual restoration logic is in GlobalMedia_win.cpp which restores to the saved volume, not literally 100%
constexpr int kFullVolumePercent = 100;  // Signal value to restore to original volume

void Manager::OnSpeakingLevelChanged(AAP::SpeakingLevel level)
{
    if (!_conversationalAwarenessEnabled) {
        return;
    }
    
    // Disable conversational awareness in transparency mode to avoid volume restoration bugs
    // The AAP firmware in transparency mode doesn't reliably send restoration events
    if (_currentNoiseControlMode.has_value() && 
        _currentNoiseControlMode.value() == AAP::NoiseControlMode::Transparency) {
        LOG(Info, "Conversational awareness disabled in transparency mode to avoid volume bugs");
        return;
    }
    
    switch (level) {
        case AAP::SpeakingLevel::StartedSpeaking_GreatlyReduce:
        case AAP::SpeakingLevel::StartedSpeaking_GreatlyReduce2:
            LOG(Info, "User started speaking - reducing media volume to {}%", _conversationalAwarenessVolumePercent);
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
