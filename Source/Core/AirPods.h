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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <QObject>

#include "Bluetooth.h"
#include "AppleCP.h"
#include "AAP.h"
#include "AAPManager.h"
#include "../Helper.h"

namespace Core::AirPods {

//
// Structures
//

namespace Details {

struct BasicState {
    Battery battery;
    bool isCharging{false};

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
    std::optional<uint8_t> adaptiveTransparencyLevel;

    bool operator==(const State &rhs) const = default;
};

//
// Classes
//

namespace Details {

QString ResolveDisplayName(QString deviceName, Model model);

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

    std::optional<bool> Update(std::optional<Lid> oldLid, Lid newLid, Timestamp now);
    void OnLost(Timestamp now);
    void Reset();

private:
    mutable std::mutex _mutex;
    std::optional<Lid> _lastLid;
    std::optional<Timestamp> _lostAt;
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

// Session-local manual-disconnect policy. A rebind starts a new policy scope,
// so suppression for one address can never leak into another device.
class LinkSuppressionState
{
public:
    void BindAddress(uint64_t address)
    {
        if (_address != address) {
            _address = address;
            _suppressed = false;
        }
    }
    void Set(bool suppressed)
    {
        _suppressed = suppressed;
    }
    bool IsSuppressed() const
    {
        return _suppressed;
    }

private:
    uint64_t _address{};
    bool _suppressed{};
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
    void OnAdaptiveTransparencyLevelChanged(uint8_t level);
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
    bool SetAdaptiveTransparencyLevel(uint8_t level);
    std::optional<uint8_t> GetAdaptiveTransparencyLevel() const;
    bool SetAdaptiveNoiseLevel(uint8_t level);
    bool IsAAPConnected() const;

    // AAP link introspection (blueprint 1.1/1.3)
    bool IsAAPConnectedViaMagicAAP() const;
    AAP::AttPathState GetAAPAttPathState() const;
    AAP::ConnectFailure GetLastAAPConnectFailure() const;

    // Head tracking
    bool StartHeadTracking();
    bool StopHeadTracking();
    bool IsHeadTrackingActive() const;

    // Explicit link control (blueprint 2.8, tray connect/disconnect).
    //
    // A manual link disconnect tears the AAP session down through the same
    // desired-address stop path as P2 (state-machine Stop/FinishStop, never a
    // generation bypass) and keeps it down: natural device-connected events
    // must not re-establish it. An explicit reconnect lifts the suppression
    // and connects again when the bound model supports it. The flag is
    // session-lifetime on purpose - a process restart re-reads the OS link
    // state instead of inheriting a stale suppression (see task-D7 report).
    void SetLinkSuppressed(bool suppressed);
    bool IsLinkSuppressed() const;

    // Check if the model supports ANC features
    static bool SupportsANC(Model model);

private:
    std::mutex _mutex;
    // Declared before StateManager so its timer callbacks cannot outlive these trackers.
    Details::LidTracker _lidTracker;
    Details::EarDetectionTracker _earDetection;
    Details::StateManager _stateMgr;
    std::optional<Bluetooth::Device> _boundDevice;
    std::optional<Model> _modelOverride;
    QString _deviceName;
    Model _boundModel{Model::Unknown};
    bool _deviceConnected{false};
    Details::LinkSuppressionState _manualLinkSuppressed;
    std::atomic<bool> _scannerWanted{false};
    std::atomic<bool> _automaticEarDetection{false};
    std::atomic<bool> _conversationalAwarenessEnabled{false};
    std::atomic<uint8_t> _conversationalAwarenessVolumePercent{40};
    std::atomic<bool> _personalizedVolumeEnabled{false};
    std::atomic<bool> _loudSoundReductionEnabled{false};
    std::atomic<uint8_t> _adaptiveTransparencyLevel{25};
    std::atomic<int> _currentNoiseControlMode{-1};
    uint64_t _requestedDeviceAddress{0};
    std::jthread _deviceLookupThread;

    // AAP Manager for L2CAP protocol communication
    AAP::Manager _aapMgr;

    // Declared last so that it is destroyed first: its destructor stops the watcher and waits
    // for in-flight callbacks, which reach `_stateMgr` and `_mutex` above.
    Bluetooth::AdvertisementWatcher _adWatcher;

    // Owned workers are declared last so member destruction cannot release the objects their
    // callbacks touch before both threads have been stopped and joined.
    std::atomic<bool> _scannerRunning{false};
    mutable std::mutex _scannerRetryMutex;
    Helper::ConWorker _scannerRetryWorker;

    using AAPClock = std::chrono::steady_clock;
    std::mutex _aapWorkerMutex;
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
    void OnAdaptiveTransparencyLevelNotification(uint8_t level, AAP::AAPAttempt attempt);
    void OnSpeakingLevelChanged(AAP::SpeakingLevel level, AAP::AAPAttempt attempt);
    void OnEarDetectionChanged(
        AAP::EarStatus primary, AAP::EarStatus secondary, AAP::AAPAttempt attempt);
    void OnHeadTrackingData(AAP::HeadTrackingData data, AAP::AAPAttempt attempt);
    void OnAttProbeResult(AAP::AttPathState state, AAP::AAPAttempt attempt);
    void OnAAPConnected(AAP::AAPAttempt attempt);
    void OnAAPDisconnected(AAP::AAPAttempt attempt);
    void SetupAAPCallbacks();
    void ConnectAAP();
    void StartAAPConnectWorker();
    void ScheduleAAPConnection(uint64_t address);
    void ScheduleAAPConnectionLocked(uint64_t address);
    void DisconnectAAP();
    void AAPConnectLoop(std::stop_token stopToken);
    bool IsCurrentAAPAttempt(AAP::AAPAttempt attempt) const;
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
};

std::vector<Core::Bluetooth::Device> GetDevices();

} // namespace Core::AirPods
