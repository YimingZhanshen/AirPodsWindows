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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <QObject>

#include "Bluetooth.h"
#include "AppleCP.h"
#include "AAP.h"
#include "AAPManager.h"
#include "BatteryFusion.h"
#include "../Helper.h"

class AirPodsDomainTests;
class UiRenderingTests;

namespace Core::AirPods {

//
// Structures
//

namespace Details {

struct BasicState {
    Battery battery;
    bool isCharging{false};
    // D2 fusion metadata. Defaults describe the pure BLE path, so a driver-less
    // session behaves exactly as before; the Manager overlays AAP readings right
    // before a State is published.
    BatterySource batterySource{BatterySource::BLE};
    // AAP presents this component and BLE disagrees beyond one quantized bucket.
    bool sourceConflict{false};

    bool operator==(const BasicState &rhs) const = default;
};
} // namespace Details

struct PodState : Details::BasicState {
    bool isInEar{false};
    // A pod charging while its peer is not is still in the case, even when the packet's
    // "both in case" bit is clear.
    bool isInCase{false};

    bool operator==(const PodState &rhs) const = default;
};

struct CaseState : Details::BasicState {
    bool isBothPodsInCase{false};
    bool isLidOpened{false};

    bool operator==(const CaseState &rhs) const = default;
};

struct PodsState {
    PodState left, right;

    bool operator==(const PodsState &rhs) const = default;
};

struct State {
    Model model{Model::Unknown};
    AppleCP::Color color{AppleCP::Color::White};
    PodsState pods;
    CaseState caseBox;
    QString displayName;

    // AAP Protocol states (for AirPods Pro and Max with ANC support)
    std::optional<AAP::NoiseControlMode> noiseControlMode;
    std::optional<AAP::ConversationalAwarenessState> conversationalAwareness;
    std::optional<AAP::PersonalizedVolumeState> personalizedVolume;
    std::optional<AAP::LoudSoundReductionState> loudSoundReduction;
    std::optional<bool> automaticEarDetectionEnabled;
    std::optional<uint8_t> ppeCapLevel;

    bool operator==(const State &rhs) const = default;
};

//
// Classes
//

namespace Details {

QString ResolveDisplayName(QString deviceName, Model model);

// Resolve a Model from the Windows-published device model number (e.g. A3439). Returns
// nullopt for an unknown/absent model number so callers must not guess a capability from it.
// Evidence for each entry is documented in the definition inside AirPods.cpp; anything that
// lacks a local source, upstream table, or public reference stays out of the map.
std::optional<Model> ModelFromModelNumber(std::string_view modelNumber);

// Model-number evidence may refine a generic product-id model within the same family, but must
// not replace a conflicting family. Unknown product ids fall back to the evidenced model number.
Model ResolveModel(Model productModel, std::optional<Model> modelNumberModel);

class Advertisement
{
public:
    using AddressType = decltype(Bluetooth::AdvertisementWatcher::ReceivedData::address);
    using TimestampType = decltype(Bluetooth::AdvertisementWatcher::ReceivedData::timestamp);

    struct AdvState : AirPods::State {
        Side side;
    };

    static bool IsDesiredAdv(const Bluetooth::AdvertisementWatcher::ReceivedData &data);

    Advertisement(const Bluetooth::AdvertisementWatcher::ReceivedData &data);

    int16_t GetRssi() const;
    const TimestampType &GetTimestamp() const;
    AddressType GetAddress() const;
    std::vector<uint8_t> GetDesensitizedData() const;
    const AdvState &GetAdvState() const;

private:
    Bluetooth::AdvertisementWatcher::ReceivedData _data;
    AppleCP::AirPods _protocol;
    AdvState _state;

    const std::vector<uint8_t> &GetMfrData() const;
};

// AirPods use Random Non-resolvable device addresses for privacy reasons. This means we
// can't "Remember" the user's AirPods by any device property. Here we track our desired
// devices in some non-elegant ways, but obviously it is sometimes unreliable.
//
class StateManager
{
public:
    struct UpdateEvent {
        std::optional<State> oldState;
        State newState;
    };

    struct Intervals {
        std::chrono::milliseconds lost{std::chrono::seconds{10}};
        std::chrono::milliseconds stateReset{std::chrono::seconds{10}};
    };

    explicit StateManager(Intervals intervals = {});
    ~StateManager();

    void SetOnDiscardState(std::function<void()> callback);
    std::optional<State> GetCurrentState() const;

    std::optional<UpdateEvent> OnAdvReceived(Advertisement adv);
    void Disconnect();

    void OnRssiMinChanged(int16_t rssiMin);

private:
    using Clock = std::chrono::steady_clock;
    using Timestamp = std::chrono::time_point<Clock>;

    mutable std::mutex _mutex;

    std::function<void()> _onDiscardState;
    Helper::Timer _lostTimer;
    Helper::Sides<Helper::Timer> _stateResetTimer;
    Helper::Sides<std::optional<std::pair<Advertisement, Timestamp>>> _adv;
    // Addresses outlive expired advertisements so weak packets from a known side remain usable.
    Helper::Sides<std::optional<Advertisement::AddressType>> _knownAddress;
    std::optional<State> _cachedState;
    int16_t _rssiMin{std::numeric_limits<int16_t>::max()};

    bool IsPossibleDesiredAdv(const Advertisement &adv) const;
    bool IsKnownAddress(Advertisement::AddressType address) const;
    void UpdateAdv(Advertisement adv);
    std::optional<UpdateEvent> UpdateState();
    std::function<void()> ResetAll();

    std::function<void()> DoLost();
    void DoStateReset(Side side);
};

// Remembers lid transitions across short advertisement gaps and while the pods are out of the case.
class LidTracker
{
public:
    using Clock = std::chrono::steady_clock;
    using Timestamp = std::chrono::time_point<Clock>;
    enum class Lid : uint8_t { Opened, Closed, Unobservable };

    static constexpr std::chrono::seconds kMemory{60};
    // Blueprint 2.6 (D6): an opened lid pops the window at most once per cooldown window, so
    // rapid open/close cycling cannot bombard the desktop. Closes are reported regardless.
    static constexpr std::chrono::seconds kCooldown{60};

    std::optional<bool>
    Update(std::optional<Lid> oldLid, Lid newLid, Timestamp now, bool popupAllowed = true);
    void OnLost(Timestamp now);
    // Drops learned lid state while retaining popup-rate limits. Used when a link briefly drops
    // and the same device may reconnect.
    void ForgetDevice();
    // Drops all lid memory for an explicit unbind or rebind to another device.
    void Reset();

private:
    mutable std::mutex _mutex;
    std::optional<Lid> _lastLid;
    std::optional<Timestamp> _lostAt;
    std::optional<Timestamp> _lastPopupAt;
};

// Keeps an ear-detection baseline even when StateManager drops and recreates its cached state.
class EarDetectionTracker
{
public:
    std::optional<bool> Update(const State &state);
    void Reset();

private:
    std::optional<bool> _lastBothInEar;
};
} // namespace Details

class Manager : public QObject
{
    Q_OBJECT

public:
    explicit Manager(QObject *parent = nullptr);
    ~Manager();

    void StartScanner();
    void StopScanner();

    void OnRssiMinChanged(int16_t rssiMin);
    void OnAutomaticEarDetectionChanged(bool enable);
    void OnBoundDeviceAddressChanged(uint64_t address);
    void OnConversationalAwarenessChanged(bool enable);
    void OnConversationalAwarenessVolumePercentChanged(uint8_t percent);
    void OnPersonalizedVolumeChanged(bool enable);
    void OnLoudSoundReductionChanged(bool enable);
    void OnPpeCapLevelChanged(uint8_t level);
    void OnAdaptiveNoiseLevelChanged(uint8_t level);
    void OnHeadTrackingChanged(bool enabled);
    void OnNoiseControlModeChanged(AAP::NoiseControlMode mode);

    // AAP Protocol features
    bool SetNoiseControlMode(AAP::NoiseControlMode mode);
    std::optional<AAP::NoiseControlMode> GetNoiseControlMode() const;
    bool SetConversationalAwareness(bool enable);
    std::optional<AAP::ConversationalAwarenessState> GetConversationalAwarenessState() const;
    bool SetPersonalizedVolume(bool enable);
    std::optional<AAP::PersonalizedVolumeState> GetPersonalizedVolumeState() const;
    bool SetLoudSoundReduction(bool enable);
    std::optional<AAP::LoudSoundReductionState> GetLoudSoundReductionState() const;
    bool SetPpeCapLevel(uint8_t level);
    std::optional<uint8_t> GetPpeCapLevel() const;

    bool SetAdaptiveNoiseLevel(uint8_t level);
    bool IsAAPConnected() const;
    AAP::AAPAttempt GetLastAAPAttempt() const;
    uint64_t LatestAAPCommandRequest(AAP::AAPCommandKind kind) const;
    bool IsCurrentAAPCommandRequest(AAP::AAPCommandKind kind, uint64_t requestId) const;
    bool HasPendingAAPCommand(AAP::AAPCommandKind kind) const;
    void RefreshAAPCommand(AAP::AAPCommandKind kind);

    // AAP link introspection (blueprint 1.1/1.3)
    bool IsAAPConnectedViaMagicAAP() const;
    AAP::AttPathState GetAAPAttPathState() const;
    AAP::ConnectFailure GetLastAAPConnectFailure() const;

    // Head tracking
    bool StartHeadTracking();
    bool StopHeadTracking();
    bool IsHeadTrackingActive() const;

    // Stem gestures (blueprint 2.4). The settings layer pushes the whole table so a gesture is
    // never resolved against a half-applied configuration.
    void SetStemActions(const AAP::StemActionTable &actions);
    AAP::StemActionTable GetStemActions() const;
    bool ToggleNoiseControl();

    // Check if the model supports ANC features
    static bool SupportsANC(Model model);

private:
    friend class ::AirPodsDomainTests;
    friend class ::UiRenderingTests;

    std::mutex _mutex;
    // Declared before the scanner and workers it feeds so its timer callbacks cannot outlive
    // these trackers. The destructor first drains workers, then member destruction converges.
    Details::LidTracker _lidTracker;
    Details::EarDetectionTracker _earDetection;
    Details::StateManager _stateMgr;
    std::optional<Bluetooth::Device> _boundDevice;
    std::optional<Model> _modelOverride;
    QString _deviceName;
    Model _boundModel{Model::Unknown};
    bool _deviceConnected{false};
    std::atomic<bool> _scannerWanted{false};
    std::atomic<bool> _automaticEarDetection{false};
    std::atomic<bool> _conversationalAwarenessEnabled{false};
    std::atomic<uint8_t> _conversationalAwarenessVolumePercent{40};
    std::atomic<bool> _personalizedVolumeEnabled{false};
    std::atomic<bool> _loudSoundReductionEnabled{false};
    std::atomic<uint8_t> _ppeCapLevel{AAP::kPpeCapLevelDefault};
    std::atomic<uint8_t> _adaptiveNoiseLevel{50};
    std::atomic<bool> _headTrackingEnabled{false};
    std::atomic<int> _currentNoiseControlMode{-1};
    // AAP-over-BLE battery fusion. `_batteryFusionMutex` guards the three fields
    // below and is always a leaf lock: it may be taken while `_mutex` or
    // `_aapWorkerMutex` is held, nothing else is locked while holding it and no
    // signal is emitted under it. BLE and AAP callbacks retain their newest inputs
    // and post RequestFusedStatePublish; StateUpdated has one owner-thread producer.
    mutable std::mutex _batteryFusionMutex;
    Details::BatteryFusion _batteryFusion;
    std::optional<State> _lastBleState;
    std::optional<State> _lastPublishedState;
    // Owner-thread-only guard. A receiver may spin nested event processing during
    // StateUpdated; defer and coalesce those publishes until the current emission has
    // reached every receiver, so an older state can never follow a newer one.
    bool _publishingFusedState{false};
    bool _fusedStatePublishPending{false};
    // Replaced as one snapshot on the settings thread and read on the AAP reader thread.
    std::mutex _stemActionsMutex;
    AAP::StemActionTable _stemActions{AAP::DefaultStemActions()};
    // Long-press/single-press deduplication state; owned by the AAP reader thread, guarded here
    // because the reconnect path resets it from the disconnect callback.
    std::mutex _stemDeduperMutex;
    AAP::StemPressDeduper _stemDeduper;
    uint64_t _requestedDeviceAddress{0};
    std::jthread _deviceLookupThread;

    // AAP Manager for L2CAP protocol communication
    AAP::Manager _aapMgr;

    // Declared last so that it is destroyed first: its destructor stops the watcher and waits
    // for in-flight callbacks, which reach `_stateMgr` and `_mutex` above.
    Bluetooth::AdvertisementWatcher _adWatcher;

    // Destructor body stops and joins these workers before dependent members are destroyed.
    std::atomic<bool> _scannerRunning{false};
    mutable std::mutex _scannerRetryMutex;
    Helper::ConWorker _scannerRetryWorker;

    using AAPClock = std::chrono::steady_clock;
    mutable std::mutex _aapWorkerMutex;
    std::condition_variable _aapWorkerCv;
    AAP::AAPConnectionStateMachine _aapConnection;
    std::optional<uint64_t> _aapDesiredAddress;
    AAP::AAPAttempt _aapActiveAttempt;
    bool _aapConnectInProgress{false};
    bool _aapRetryScheduled{false};
    AAPClock::time_point _aapRetryAt{};
    bool _aapShutdown{false};
    std::jthread _aapConnectWorker;

    void CompleteBoundDeviceLookup(uint64_t address, std::optional<Bluetooth::Device> device);
    void OnBoundDeviceConnectionStateChanged(Bluetooth::DeviceState state);
    void OnStateChanged(Details::StateManager::UpdateEvent updateEvent);
    void OnLidOpened(bool opened);
    void OnBothInEar(bool isBothInEar);
    bool OnAdvertisementReceived(const Bluetooth::AdvertisementWatcher::ReceivedData &data);
    void OnAdvWatcherStateChanged(
        Bluetooth::AdvertisementWatcher::State state, const std::optional<std::string> &optError);

    // AAP callbacks
    void OnNoiseControlModeNotification(AAP::NoiseControlMode mode, AAP::AAPAttempt attempt);
    void OnConversationalAwarenessStateChanged(
        AAP::ConversationalAwarenessState state, AAP::AAPAttempt attempt);
    void
    OnPersonalizedVolumeStateChanged(AAP::PersonalizedVolumeState state, AAP::AAPAttempt attempt);
    void
    OnLoudSoundReductionStateChanged(AAP::LoudSoundReductionState state, AAP::AAPAttempt attempt);
    void OnAutomaticEarDetectionNotification(bool enabled, AAP::AAPAttempt attempt);
    void OnPpeCapLevelNotification(uint8_t level, AAP::AAPAttempt attempt);
    void OnAAPCommandSettled(const AAP::AAPCommandOutcome &outcome, AAP::AAPAttempt attempt);
    void OnSpeakingLevelChanged(AAP::SpeakingLevel level, AAP::AAPAttempt attempt);
    void OnEarDetectionChanged(
        AAP::EarStatus primary, AAP::EarStatus secondary, AAP::AAPAttempt attempt);
    void OnAAPBatteryChanged(const AAP::AAPBatterySnapshot &snapshot, AAP::AAPAttempt attempt);
    void OnHeadTrackingData(AAP::HeadTrackingData data, AAP::AAPAttempt attempt);
    void OnStemPress(AAP::StemEvent event, AAP::AAPAttempt attempt);
    void OnAttProbeResult(AAP::AttPathState state, AAP::AAPAttempt attempt);
    void OnAAPConnected(AAP::AAPAttempt attempt);
    void OnAAPDisconnected(AAP::AAPAttempt attempt);
    void SetupAAPCallbacks();
    void ConnectAAP();
    void StartAAPConnectWorker();
    void ScheduleAAPConnection(uint64_t address);
    void DisconnectAAP();
    void AAPConnectLoop(std::stop_token stopToken);
    bool IsCurrentAAPAttempt(AAP::AAPAttempt attempt) const;
    // Applies the fused AAP battery view to a state right before it is published.
    // The caller must hold `_batteryFusionMutex`.
    void OverlayBattery(State &state) const;
    // Posts a fused republish onto the thread that owns Manager; safe from any worker thread.
    void RequestFusedStatePublish();
    // Re-publishes the newest retained BLE state with the current fusion overlay applied.
    // Runs on Manager's thread for both BLE and AAP battery inputs. Nested publish requests
    // are coalesced and drained after the current StateUpdated emission completes.
    void PublishFusedState();
    void ScheduleScannerRetry();
    void StopScannerRetry();
    bool StartScannerOnce();

Q_SIGNALS:
    void StateUpdated(const Core::AirPods::State &newState);
    void BoundDeviceUnavailable();
    void Disconnected();
    void LidToggled(bool opened);
    void ScannerAvailabilityChanged(bool available);
    void DeviceConnectionChanged(bool connected);
    // Emitted from AAP callback threads; cross-thread receivers get the queued
    // delivery Qt applies automatically.
    void AapConnectionStateChangedSafely(bool connected);
    // Payload is Core::AAP::ConnectFailure cast to int (registered metatypes
    // stay free of AAP enums).
    void AapConnectionFailedSafely(int failure);
    void AapCommandFailedSafely(
        uint64_t generation, uint64_t requestId, int kind, uint32_t value, uint32_t rollbackValue,
        bool hasRollbackValue);
    void AapCommandStateSafely(uint64_t generation, int kind, uint32_t value);
    // Emitted from the AAP reader thread for a validated stem gesture. Payload is
    // Core::AAP::StemAction cast to int; the receiver performs the host work on the GUI thread,
    // so no widget and no media API is ever touched from the worker.
    void StemActionRequestedSafely(int action);
};

std::vector<Core::Bluetooth::Device> GetDevices();

} // namespace Core::AirPods
