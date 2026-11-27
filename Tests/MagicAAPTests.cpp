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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <QtTest>

#include "Source/Core/MagicAAPDeviceBinding.h"
#include "Source/Core/MagicAAPWinRT.h"

#include <Windows.h>

namespace {

using Core::MagicAAPWinRT::BindToTargetInterface;
using Core::MagicAAPWinRT::ExtractAddress;
using Core::MagicAAPWinRT::MagicAAPBindStatus;
using Core::MagicAAPWinRT::MagicAAPCallbackSession;
using Core::MagicAAPWinRT::MagicAAPDeviceInterface;
using Core::MagicAAPWinRT::NormalizeContainerId;

constexpr uint64_t kTarget = 0x340E22CC08A0ULL;
constexpr uint64_t kOther = 0xD4DA214FC3E5ULL;
constexpr wchar_t kTargetContainer[] = L"{AAACAC36-8F27-50B1-B473-47BF5F0D04F0}";
constexpr wchar_t kOtherContainer[] = L"{BBBCAC36-8F27-50B1-B473-47BF5F0D04F1}";

MagicAAPDeviceInterface MakeDevice(
    const std::wstring &address, const std::wstring &container,
    const std::wstring &interfaceGuid = L"{9eec98bb-3c54-45d4-a843-7900c4635e08}")
{
    const std::wstring tail = L"A&26AF450F&0&" + address + L"_C00000000";
    MagicAAPDeviceInterface device;
    device.interfacePath =
        L"\\\\?\\BTHENUM#{74ec2172-0bad-4d01-8f77-997b2be0722a}_VID&0001004C_PID&2027#" + tail +
        L"#" + interfaceGuid;
    device.deviceInstanceId =
        L"BTHENUM\\{74EC2172-0BAD-4D01-8F77-997B2BE0722A}_VID&0001004C_PID&2027\\" + tail;
    device.parentInstanceId =
        L"BTHENUM\\DEV_" + address + L"\\A&26AF450F&0&BLUETOOTHDEVICE_" + address;
    device.containerId = container;
    return device;
}

MagicAAPDeviceInterface MakeUnprovable(const std::wstring &container)
{
    MagicAAPDeviceInterface device;
    device.interfacePath =
        L"\\\\?\\bthenum#{74ec2172-0bad-4d01-8f77-997b2be0722a}_vid&0001004c_pid&2027"
        L"#a&26af450f&0&0#{9eec98bb-3c54-45d4-a843-7900c4635e08}";
    device.containerId = container;
    return device;
}

struct OpenLog {
    std::vector<std::wstring> paths;
};

Core::MagicAAPWinRT::MagicAAPInterfaceOpener CountingOpener(OpenLog &log, bool succeed)
{
    return [&log, succeed](const MagicAAPDeviceInterface &device) -> void * {
        log.paths.push_back(device.interfacePath);
        return succeed ? reinterpret_cast<void *>(0x1234) : nullptr;
    };
}

class ThrowingCopyCallback
{
public:
    explicit ThrowingCopyCallback(std::shared_ptr<std::atomic<bool>> failOnCopy)
        : _failOnCopy{std::move(failOnCopy)}
    {
    }

    ThrowingCopyCallback(ThrowingCopyCallback &&) noexcept = default;
    ThrowingCopyCallback &operator=(ThrowingCopyCallback &&) noexcept = default;
    ThrowingCopyCallback(const ThrowingCopyCallback &other) : _failOnCopy{other._failOnCopy}
    {
        if (_failOnCopy && _failOnCopy->load()) {
            throw std::runtime_error("injected callback snapshot failure");
        }
    }

    void operator()(const std::vector<uint8_t> &) const {}

private:
    std::shared_ptr<std::atomic<bool>> _failOnCopy;
};

} // namespace

class MagicAAPTests : public QObject
{
    Q_OBJECT

private slots:
    void AddressProofRequiresCompleteDelimitedToken()
    {
        using Core::MagicAAPWinRT::AddressFromDevicePrefix;
        using Core::MagicAAPWinRT::AddressFromInstanceSuffix;
        using Core::MagicAAPWinRT::ParseAddressToken;

        QVERIFY(ParseAddressToken(L"340e22cc08a0").has_value());
        QCOMPARE(*ParseAddressToken(L"340e22cc08a0"), kTarget);
        for (const auto *token :
             {L"340E22CC08A", L"340E22CC08A00", L"340E22CC08AZ", L"x340E22CC08A0", L"340E22CC08A0x",
              L""})
        {
            QVERIFY(!ParseAddressToken(token).has_value());
        }

        QVERIFY(AddressFromInstanceSuffix(L"a&340e22cc08a0_c00000000").has_value());
        QVERIFY(!AddressFromInstanceSuffix(L"a&340e22cc08a0c_c00000000").has_value());
        QVERIFY(!AddressFromInstanceSuffix(L"a&340e22cc08a_c00000000").has_value());
        QVERIFY(!AddressFromInstanceSuffix(L"a&340e22cc08a0_C00000000x").has_value());
        QVERIFY(!AddressFromInstanceSuffix(L"a&1340e22cc08a0_c00000000").has_value());

        QCOMPARE(*AddressFromDevicePrefix(L"BTHENUM\\dev_340e22cc08a0\\x"), kTarget);
        QVERIFY(!AddressFromDevicePrefix(L"BTHENUM\\XBLUETOOTHDEVICE_340E22CC08A0\\x").has_value());
        QVERIFY(!AddressFromDevicePrefix(L"BTHENUM\\DEV_340E22CC08A00\\x").has_value());
        QVERIFY(!AddressFromDevicePrefix(L"BTHENUM\\DEV_340E22CC08A0\\DEV_D4DA214FC3E5\\x")
                     .has_value());
    }

    void ContradictoryIdentitySourcesProveNothing()
    {
        auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        QCOMPARE(ExtractAddress(target), std::optional<uint64_t>(kTarget));

        target.parentInstanceId = L"BTHENUM\\DEV_D4DA214FC3E5\\A&26AF450F&0&X";
        QVERIFY(!ExtractAddress(target).has_value());
        QVERIFY(!ExtractAddress(MakeUnprovable(kTargetContainer)).has_value());
    }

    void ContainerIdMustBeACompleteCaseInsensitiveGuid()
    {
        QCOMPARE(
            NormalizeContainerId(L"{aaacac36-8f27-50b1-b473-47bf5f0d04f0}"),
            std::wstring(kTargetContainer));
        for (const auto *container :
             {L"", L"AAACAC36-8F27-50B1-B473-47BF5F0D04F0",
              L"{AAACAC36-8F27-50B1-B473-47BF5F0D04F}", L"{AAACAC36-8F27-50B1-B473-47BF5F0D04F00}",
              L"{not-a-guid}"})
        {
            QVERIFY(NormalizeContainerId(container).empty());
        }
    }

    void AddressMismatchOpensNothing()
    {
        const auto other = MakeDevice(L"D4DA214FC3E5", kOtherContainer);
        const auto unproven = MakeUnprovable(kTargetContainer);
        OpenLog log;
        const auto outcome =
            BindToTargetInterface({other, unproven}, kTarget, CountingOpener(log, true));

        QCOMPARE(outcome.status, MagicAAPBindStatus::NoMatch);
        QCOMPARE(outcome.openAttempts, size_t{0});
        QCOMPARE(outcome.foreignCount, size_t{1});
        QCOMPARE(outcome.unprovenCount, size_t{1});
        QVERIFY(log.paths.empty());
        QVERIFY(outcome.handle == nullptr);
    }

    void InvalidTargetWidthOpensNothing()
    {
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        OpenLog log;
        const auto outcome =
            BindToTargetInterface({target}, 0x1340E22CC08A0ULL, CountingOpener(log, true));
        QCOMPARE(outcome.status, MagicAAPBindStatus::NoMatch);
        QVERIFY(log.paths.empty());
    }

    void MatchedOpenFailedNeverFallsBackToForeignDevice()
    {
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        const auto other = MakeDevice(L"D4DA214FC3E5", kOtherContainer);
        for (const auto &interfaces : {std::vector{target, other}, std::vector{other, target}}) {
            OpenLog log;
            const auto outcome =
                BindToTargetInterface(interfaces, kTarget, CountingOpener(log, false));
            QCOMPARE(outcome.status, MagicAAPBindStatus::OpenFailed);
            QCOMPARE(outcome.openAttempts, size_t{1});
            QCOMPARE(log.paths, std::vector<std::wstring>{target.interfacePath});
            QVERIFY(outcome.handle == nullptr);
        }
    }

    void SameDevicePathsAreDeterministicAndNeverIncludeForeignPaths()
    {
        const auto servicePath = MakeDevice(L"340E22CC08A0", kTargetContainer);
        const auto customPath = MakeDevice(
            L"340E22CC08A0", kTargetContainer, L"{74ec2172-0bad-4d01-8f77-997b2be0722a}");
        const auto other = MakeDevice(L"D4DA214FC3E5", kOtherContainer);

        OpenLog successLog;
        const auto success = BindToTargetInterface(
            {servicePath, other, customPath}, kTarget, CountingOpener(successLog, true));
        QCOMPARE(success.status, MagicAAPBindStatus::Connected);
        QCOMPARE(success.candidateCount, size_t{2});
        QCOMPARE(successLog.paths, std::vector<std::wstring>{customPath.interfacePath});

        OpenLog failureLog;
        const auto failure = BindToTargetInterface(
            {servicePath, other, customPath}, kTarget, CountingOpener(failureLog, false));
        QCOMPARE(failure.status, MagicAAPBindStatus::OpenFailed);
        QCOMPARE(failure.openAttempts, size_t{2});
        QCOMPARE(
            failureLog.paths,
            (std::vector<std::wstring>{customPath.interfacePath, servicePath.interfacePath}));
    }

    void ContainerSiblingRequiresExactProvenAnchor()
    {
        const auto proven = MakeDevice(L"340E22CC08A0", kTargetContainer);
        const auto sibling = MakeUnprovable(L"{aaacac36-8f27-50b1-b473-47bf5f0d04f0}");
        const auto foreign = MakeDevice(L"D4DA214FC3E5", kOtherContainer);

        OpenLog unanchoredLog;
        const auto unanchored =
            BindToTargetInterface({sibling, foreign}, kTarget, CountingOpener(unanchoredLog, true));
        QCOMPARE(unanchored.status, MagicAAPBindStatus::NoMatch);
        QVERIFY(unanchoredLog.paths.empty());

        OpenLog anchoredLog;
        const auto anchored = BindToTargetInterface(
            {proven, sibling, foreign}, kTarget, CountingOpener(anchoredLog, false));
        QCOMPARE(anchored.status, MagicAAPBindStatus::OpenFailed);
        QCOMPARE(anchored.candidateCount, size_t{2});
        QCOMPARE(anchoredLog.paths.size(), size_t{2});
        QVERIFY(!anchoredLog.paths.empty());
        for (const auto &path : anchoredLog.paths) {
            QVERIFY(path != foreign.interfacePath);
        }

        auto wrongContainer = sibling;
        wrongContainer.containerId = kOtherContainer;
        OpenLog wrongLog;
        const auto wrong = BindToTargetInterface(
            {proven, wrongContainer}, kTarget, CountingOpener(wrongLog, false));
        QCOMPARE(wrong.candidateCount, size_t{1});
        QCOMPARE(wrongLog.paths, std::vector<std::wstring>{proven.interfacePath});
    }

    void ContradictoryOrForeignContainerCannotAnchorSibling()
    {
        const auto proven = MakeDevice(L"340E22CC08A0", kTargetContainer);
        const auto sibling = MakeUnprovable(kTargetContainer);
        auto contradictory = MakeDevice(L"340E22CC08A0", kTargetContainer);
        contradictory.parentInstanceId = L"BTHENUM\\DEV_D4DA214FC3E5\\A&26AF450F&0&X";

        OpenLog conflictLog;
        const auto conflict = BindToTargetInterface(
            {contradictory, sibling}, kTarget, CountingOpener(conflictLog, false));
        QCOMPARE(conflict.status, MagicAAPBindStatus::NoMatch);
        QCOMPARE(conflict.candidateCount, size_t{0});
        QVERIFY(conflictLog.paths.empty());

        const auto sharedForeign = MakeDevice(L"D4DA214FC3E5", kTargetContainer);
        OpenLog foreignLog;
        const auto foreign = BindToTargetInterface(
            {proven, sharedForeign, sibling}, kTarget, CountingOpener(foreignLog, false));
        QCOMPARE(foreign.candidateCount, size_t{1});
        QCOMPARE(foreignLog.paths, std::vector<std::wstring>{proven.interfacePath});
    }

    void DuplicatePathIsOpenedAtMostOnce()
    {
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        OpenLog log;
        const auto outcome =
            BindToTargetInterface({target, target}, kTarget, CountingOpener(log, false));
        QCOMPARE(outcome.status, MagicAAPBindStatus::OpenFailed);
        QCOMPARE(outcome.candidateCount, size_t{1});
        QCOMPARE(outcome.openAttempts, size_t{1});
        QCOMPARE(log.paths.size(), size_t{1});
    }

    void EnumerationPermutationsKeepTheSameCandidateOrder()
    {
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        const auto sibling = MakeUnprovable(kTargetContainer);
        const auto foreign = MakeDevice(L"D4DA214FC3E5", kOtherContainer);
        std::array<MagicAAPDeviceInterface, 3> metadata{target, sibling, foreign};
        std::array<size_t, 3> order{0, 1, 2};
        std::vector<std::wstring> expected;

        do {
            OpenLog log;
            const std::vector<MagicAAPDeviceInterface> interfaces{
                metadata[order[0]], metadata[order[1]], metadata[order[2]]};
            const auto outcome =
                BindToTargetInterface(interfaces, kTarget, CountingOpener(log, false));
            QCOMPARE(outcome.status, MagicAAPBindStatus::OpenFailed);
            QCOMPARE(outcome.candidateCount, size_t{2});
            QCOMPARE(outcome.openAttempts, size_t{2});
            QVERIFY(
                std::find(log.paths.begin(), log.paths.end(), foreign.interfacePath) ==
                log.paths.end());
            if (expected.empty()) {
                expected = log.paths;
            }
            else {
                QCOMPARE(log.paths, expected);
            }
        } while (std::next_permutation(order.begin(), order.end()));
    }

    void AddressProofSourcesArePermutationInvariant()
    {
        const auto unprovablePath = MakeUnprovable(kTargetContainer).interfacePath;
        for (size_t proofSource = 0; proofSource < 3; ++proofSource) {
            auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
            if (proofSource == 0) {
                target.deviceInstanceId.clear();
                target.parentInstanceId.clear();
            }
            else {
                target.interfacePath = unprovablePath;
                if (proofSource == 1) {
                    target.parentInstanceId.clear();
                }
                else {
                    target.deviceInstanceId.clear();
                }
            }

            OpenLog log;
            const auto other = MakeDevice(L"D4DA214FC3E5", kOtherContainer);
            const auto outcome =
                BindToTargetInterface({other, target}, kTarget, CountingOpener(log, true));
            QCOMPARE(outcome.status, MagicAAPBindStatus::Connected);
            QCOMPARE(outcome.candidateCount, size_t{1});
            QCOMPARE(log.paths, std::vector<std::wstring>{target.interfacePath});
        }
    }

    void CancelledBindOpensNothing()
    {
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        std::stop_source stop;
        stop.request_stop();
        OpenLog log;
        const auto outcome =
            BindToTargetInterface({target}, kTarget, CountingOpener(log, true), stop.get_token());
        QCOMPARE(outcome.status, MagicAAPBindStatus::Cancelled);
        QVERIFY(log.paths.empty());
    }

    void CancellationDuringLastOpenerFailureIsNotOpenFailed()
    {
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        std::promise<void> openerEntered;
        std::promise<void> releaseOpener;
        auto entered = openerEntered.get_future();
        auto release = releaseOpener.get_future();
        std::stop_source stop;
        OpenLog log;
        auto result = std::async(std::launch::async, [&] {
            return BindToTargetInterface(
                {target}, kTarget,
                [&](const MagicAAPDeviceInterface &device) -> void * {
                    log.paths.push_back(device.interfacePath);
                    openerEntered.set_value();
                    release.wait();
                    return nullptr;
                },
                stop.get_token());
        });

        const bool opened = entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        stop.request_stop();
        releaseOpener.set_value();
        QVERIFY(opened);
        const auto outcome = result.get();
        QCOMPARE(outcome.status, MagicAAPBindStatus::Cancelled);
        QCOMPARE(outcome.openAttempts, size_t{1});
    }

    void CancellationDuringSuccessfulOpenClosesTheHandle()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QVERIFY(handle != nullptr);
        client._driverInstalledProbe = [] { return true; };
        client._driverRunningProbe = [] { return true; };
        client._interfaceEnumerator = [target] { return std::vector{target}; };
        std::promise<void> openerEntered;
        std::promise<void> releaseOpener;
        auto entered = openerEntered.get_future();
        auto release = releaseOpener.get_future();
        std::atomic<int> receiverStarts{0};
        client._interfaceOpener = [&](const MagicAAPDeviceInterface &) -> void * {
            openerEntered.set_value();
            release.wait();
            return handle;
        };
        client._receiverThreadStarter = [&](const auto &) {
            ++receiverStarts;
            return std::thread{};
        };

        std::stop_source stop;
        auto result = std::async(std::launch::async, [&] {
            return client.ConnectViaDeviceInterface(kTarget, stop.get_token());
        });
        const bool opened = entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        stop.request_stop();
        releaseOpener.set_value();
        QVERIFY(opened);
        QCOMPARE(result.get(), MagicAAPBindStatus::Cancelled);
        QCOMPARE(receiverStarts.load(), 0);
        QVERIFY(!client.IsConnected());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(handle, &flags));
    }

    void DisconnectDuringBlockingOpenPreventsConnectionCommit()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QVERIFY(handle != nullptr);
        client._driverInstalledProbe = [] { return true; };
        client._driverRunningProbe = [] { return true; };
        client._interfaceEnumerator = [target] { return std::vector{target}; };
        std::promise<void> openerEntered;
        std::promise<void> releaseOpener;
        auto entered = openerEntered.get_future();
        auto release = releaseOpener.get_future();
        std::atomic<int> receiverStarts{0};
        client._interfaceOpener = [&](const MagicAAPDeviceInterface &) -> void * {
            openerEntered.set_value();
            release.wait();
            return handle;
        };
        client._receiverThreadStarter = [&](const auto &) {
            ++receiverStarts;
            return std::thread{};
        };

        auto connect = std::async(
            std::launch::async, [&] { return client.ConnectViaDeviceInterface(kTarget); });
        const bool openerBlocked =
            entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        auto disconnect = std::async(std::launch::async, [&] { client.Disconnect(); });
        const bool disconnectReturned =
            disconnect.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

        // Always release the driver seam before joining, including on the broken
        // implementation where Disconnect waits for the startup mutex.
        releaseOpener.set_value();
        const auto status = connect.get();
        disconnect.get();

        QVERIFY(openerBlocked);
        QVERIFY(disconnectReturned);
        QCOMPARE(status, MagicAAPBindStatus::Cancelled);
        QCOMPARE(receiverStarts.load(), 0);
        QVERIFY(!client.IsConnected());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(handle, &flags));
    }

    void DisconnectDuringReceiverStartRollsBackPublishedSession()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QVERIFY(handle != nullptr);
        client._driverInstalledProbe = [] { return true; };
        client._driverRunningProbe = [] { return true; };
        client._interfaceEnumerator = [target] { return std::vector{target}; };
        client._interfaceOpener = [&](const MagicAAPDeviceInterface &) -> void * { return handle; };
        std::promise<void> receiverEntered;
        std::promise<void> releaseReceiver;
        auto entered = receiverEntered.get_future();
        auto release = releaseReceiver.get_future();
        std::atomic<int> receiverStarts{0};
        client._receiverThreadStarter = [&](const auto &) {
            ++receiverStarts;
            receiverEntered.set_value();
            release.wait();
            return std::thread{};
        };

        auto connect = std::async(
            std::launch::async, [&] { return client.ConnectViaDeviceInterface(kTarget); });
        const bool receiverBlocked =
            entered.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        auto disconnect = std::async(std::launch::async, [&] { client.Disconnect(); });
        const bool disconnectReturned =
            disconnect.wait_for(std::chrono::seconds(1)) == std::future_status::ready;

        releaseReceiver.set_value();
        const auto status = connect.get();
        disconnect.get();

        QVERIFY(receiverBlocked);
        QVERIFY(disconnectReturned);
        QCOMPARE(status, MagicAAPBindStatus::Cancelled);
        QCOMPARE(receiverStarts.load(), 1);
        QVERIFY(!client.IsConnected());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(handle, &flags));
    }

    void EnumerationExceptionIsAContainedTransportFailure()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        std::atomic<int> openCalls{0};
        client._driverInstalledProbe = [] { return true; };
        client._driverRunningProbe = [] { return true; };
        client._interfaceEnumerator = []() -> std::vector<MagicAAPDeviceInterface> {
            throw std::runtime_error("injected enumeration failure");
        };
        client._interfaceOpener = [&](const MagicAAPDeviceInterface &) -> void * {
            ++openCalls;
            return nullptr;
        };

        QCOMPARE(client.ConnectViaDeviceInterface(kTarget), MagicAAPBindStatus::TransportError);
        QCOMPARE(openCalls.load(), 0);
        QVERIFY(!client.IsConnected());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
    }

    void ReceiverCallbacksAreSnapshottedBeforeThreadStart()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        int oldDataCalls = 0;
        int oldDisconnectCalls = 0;
        int newDataCalls = 0;
        client.SetOnDataReceived([&](const std::vector<uint8_t> &) { ++oldDataCalls; });
        client.SetOnDisconnected([&] { ++oldDisconnectCalls; });

        Core::MagicAAPWinRT::MagicAAPWinRTClient::ReceiverSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(client._mutex);
            snapshot = client.StartSession();
        }
        client.SetOnDataReceived([&](const std::vector<uint8_t> &) { ++newDataCalls; });
        client.SetOnDisconnected({});

        snapshot.onDataReceived({0x01});
        snapshot.onDisconnected();
        QCOMPARE(oldDataCalls, 1);
        QCOMPARE(oldDisconnectCalls, 1);
        QCOMPARE(newDataCalls, 0);
        QVERIFY(client._sessions.IsCurrent(snapshot.session));
        client._sessions.Close();
        QVERIFY(!client._sessions.IsCurrent(snapshot.session));
    }

    void CallbackSessionIdsAreNeverReused()
    {
        MagicAAPCallbackSession sessions;
        QVERIFY(!sessions.IsCurrent(0));
        const auto first = sessions.Start();
        const auto second = sessions.Start();
        QVERIFY(second > first);
        QVERIFY(sessions.IsCurrent(second));
        QVERIFY(!sessions.IsCurrent(first));
        sessions.Close();
        QVERIFY(!sessions.IsCurrent(second));
        QVERIFY(sessions.Start() > second);
    }

    void CallbackSnapshotFailureRollsBackSessionAndHandle()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QVERIFY(handle != nullptr);
        auto failOnCopy = std::make_shared<std::atomic<bool>>(false);
        client.SetOnDataReceived(ThrowingCopyCallback{failOnCopy});
        failOnCopy->store(true);
        client._deviceHandle = handle;
        client._usingDeviceInterface = true;
        client._connected = true;
        client._stopReceiver = false;

        bool started = true;
        {
            std::lock_guard<std::mutex> lock(client._mutex);
            started = client.StartReceiverSession();
        }

        QVERIFY(!started);
        QVERIFY(!client.IsConnected());
        QVERIFY(client._stopReceiver.load());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
        QVERIFY(client._deviceHandle == nullptr);
        QVERIFY(!client._usingDeviceInterface);
        QVERIFY(!client._receiverThread.joinable());
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(handle, &flags));
    }

    void ReceiverThreadFailureRollsBackSessionAndHandle()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QVERIFY(handle != nullptr);
        client._deviceHandle = handle;
        client._usingDeviceInterface = true;
        client._connected = true;
        client._stopReceiver = false;
        client._receiverThreadStarter =
            [](const Core::MagicAAPWinRT::MagicAAPWinRTClient::ReceiverSnapshot &) -> std::thread {
            throw std::runtime_error("injected thread creation failure");
        };

        bool started = true;
        {
            std::lock_guard<std::mutex> lock(client._mutex);
            started = client.StartReceiverSession();
        }

        QVERIFY(!started);
        QVERIFY(!client.IsConnected());
        QVERIFY(client._stopReceiver.load());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
        QVERIFY(client._deviceHandle == nullptr);
        QVERIFY(!client._usingDeviceInterface);
        QVERIFY(!client._receiverThread.joinable());
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(handle, &flags));
    }

    void ConcurrentCallbackSessionsIssueUniqueMonotonicIds()
    {
        MagicAAPCallbackSession sessions;
        constexpr size_t kThreads = 8;
        constexpr size_t kStartsPerThread = 250;
        std::vector<std::vector<uint64_t>> issued(kThreads);
        std::vector<std::jthread> threads;
        for (size_t thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&, thread] {
                for (size_t start = 0; start < kStartsPerThread; ++start) {
                    issued[thread].push_back(sessions.Start());
                }
            });
        }
        threads.clear();

        std::vector<uint64_t> all;
        for (const auto &threadIds : issued) {
            all.insert(all.end(), threadIds.begin(), threadIds.end());
        }
        std::sort(all.begin(), all.end());
        QCOMPARE(all.size(), kThreads * kStartsPerThread);
        for (size_t i = 0; i < all.size(); ++i) {
            QCOMPARE(all[i], static_cast<uint64_t>(i + 1));
        }
        QCOMPARE(sessions.Current(), all.back());
        size_t currentCount = 0;
        for (const auto session : all) {
            currentCount += sessions.IsCurrent(session) ? 1 : 0;
        }
        QCOMPARE(currentCount, size_t{1});
    }

    void ConcurrentConnectsStartExactlyOneReceiverSession()
    {
        Core::MagicAAPWinRT::MagicAAPWinRTClient client;
        const auto target = MakeDevice(L"340E22CC08A0", kTargetContainer);
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        QVERIFY(handle != nullptr);
        std::atomic<int> enumerations{0};
        std::atomic<int> opens{0};
        std::atomic<int> receiverStarts{0};
        client._driverInstalledProbe = [] { return true; };
        client._driverRunningProbe = [] { return true; };
        client._interfaceEnumerator = [&] {
            ++enumerations;
            return std::vector{target};
        };
        client._interfaceOpener = [&](const MagicAAPDeviceInterface &) -> void * {
            ++opens;
            return handle;
        };
        client._receiverThreadStarter =
            [&](const Core::MagicAAPWinRT::MagicAAPWinRTClient::ReceiverSnapshot &) {
                ++receiverStarts;
                return std::thread{[&client] {
                    while (!client._stopReceiver.load()) {
                        std::this_thread::yield();
                    }
                }};
            };

        auto first = std::async(
            std::launch::async, [&] { return client.ConnectViaDeviceInterface(kTarget); });
        auto second = std::async(
            std::launch::async, [&] { return client.ConnectViaDeviceInterface(kTarget); });
        const std::array<MagicAAPBindStatus, 2> results{first.get(), second.get()};
        QCOMPARE(std::count(results.begin(), results.end(), MagicAAPBindStatus::Connected), 1);
        QCOMPARE(std::count(results.begin(), results.end(), MagicAAPBindStatus::TransportError), 1);
        QCOMPARE(enumerations.load(), 1);
        QCOMPARE(opens.load(), 1);
        QCOMPARE(receiverStarts.load(), 1);
        QVERIFY(client.IsConnected());
        QVERIFY(client._receiverThread.joinable());
        QVERIFY(client._sessions.Current() != 0);

        client.Disconnect();
        QVERIFY(!client.IsConnected());
        QVERIFY(!client._receiverThread.joinable());
        QCOMPARE(client._sessions.Current(), uint64_t{0});
        DWORD flags = 0;
        QVERIFY(!GetHandleInformation(handle, &flags));
    }
};

QTEST_GUILESS_MAIN(MagicAAPTests)

#include "MagicAAPTests.moc"
