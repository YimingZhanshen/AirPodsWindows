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

#include "BluetoothLinkControl.h"

#if !defined APD_OS_WIN
    #error "This file shouldn't be compiled."
#endif

#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <cwchar>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "OS/Windows.h"
#include "../Logger.h"

namespace Core::BluetoothLink {
namespace {

// Members of the BtAudio KS property set (ksmedia.h). The existing
// QuickConnect path sends KSPROPERTY_ONESHOT_RECONNECT by name; the sibling
// disconnect id is documented in the same set but older SDK headers do not
// always export the constant, so mirror the documented values here and use
// them for both directions to keep the two call sites consistent.
constexpr ULONG kOneShotReconnectId = 0;
constexpr ULONG kOneShotDisconnectId = 1;

// Profile markers observed in BTHENUM filter device instance paths. A device
// instance for a classic audio function ends with the remote BD_ADDR in
// uppercase hex, and the device id embeds the Bluetooth service class GUID:
//   {0000110A|0000110B|0000110C} A2DP source/sink/AV distribution,
//   {0000111E} / BTHHFENUM      hands-free profile.
// Anything that matches neither stays unclassified and is never touched - the
// policy explicitly refuses to guess at unsupported services.
constexpr std::wstring_view kA2dpMarkers[] = {L"0000110A", L"0000110B", L"0000110C"};
constexpr std::wstring_view kHfpMarkers[] = {L"0000111E", L"BTHHFENUM"};

enum class Profile : uint8_t { Unclassified, A2DP, HFP };

class ScopedComApartment final
{
public:
    ScopedComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        _uninitialize = SUCCEEDED(result);
    }

    ~ScopedComApartment()
    {
        if (_uninitialize) {
            CoUninitialize();
        }
    }

private:
    bool _uninitialize{};
};

std::wstring ToUpper(std::wstring value)
{
    for (auto &ch : value) {
        if (ch >= L'a' && ch <= L'z') {
            ch = ch - L'a' + L'A';
        }
    }
    return value;
}

std::wstring AddressToken(uint64_t address)
{
    wchar_t buffer[17]{};
    swprintf(buffer, 17, L"%012llX", static_cast<unsigned long long>(address));
    return buffer;
}

// Device addresses identify the user's hardware; log just enough to correlate
// lines inside one session (same policy as the diagnostics recorder).
std::string MaskAddressShort(uint64_t address)
{
    wchar_t buffer[17]{};
    swprintf(buffer, 17, L"%012llX", static_cast<unsigned long long>(address));
    return "***" + std::string{buffer + 8, 4};
}

Profile ClassifyFilter(const std::wstring &filterIdUpper)
{
    for (const auto &marker : kHfpMarkers) {
        if (filterIdUpper.find(marker) != std::wstring::npos) {
            return Profile::HFP;
        }
    }
    for (const auto &marker : kA2dpMarkers) {
        if (filterIdUpper.find(marker) != std::wstring::npos) {
            return Profile::A2DP;
        }
    }
    return Profile::Unclassified;
}

bool FilterBelongsToAddress(const std::wstring &filterIdUpper, const std::wstring &addressToken)
{
    return !addressToken.empty() && filterIdUpper.find(addressToken) != std::wstring::npos;
}

OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> CreateDeviceEnumerator()
{
    OS::Windows::Com::UniquePtr<IMMDeviceEnumerator> enumerator;
    const HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, enumerator.GetIID(),
        reinterpret_cast<void **>(enumerator.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        LOG(Warn, "Create MMDeviceEnumerator failed. HRESULT: {:#x}", result);
        return {};
    }
    return enumerator;
}

// Walks the endpoint topology to the connected adapter (the BTHENUM filter
// device) and activates IKsControl on it, mirroring the reconnect path in
// QuickConnect_win. Returns the filter device id so callers can classify the
// profile and deduplicate requests per driver instance.
struct ResolvedFilter {
    OS::Windows::Com::UniquePtr<IKsControl> ksControl;
    std::wstring filterId;
};

std::optional<ResolvedFilter> ResolveFilter(IMMDeviceEnumerator &enumerator, IMMDevice &endpoint)
{
    OS::Windows::Com::UniquePtr<IDeviceTopology> topology;
    HRESULT result = endpoint.Activate(
        topology.GetIID(), CLSCTX_ALL, nullptr,
        reinterpret_cast<void **>(topology.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IConnector> connector;
    result = topology->GetConnector(0, connector.ReleaseAndAddressOf());
    if (FAILED(result)) {
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IConnector> connectedConnector;
    result = connector->GetConnectedTo(connectedConnector.ReleaseAndAddressOf());
    if (FAILED(result)) {
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IPart> part;
    result = connectedConnector->QueryInterface(
        part.GetIID(), reinterpret_cast<void **>(part.ReleaseAndAddressOf()));
    if (FAILED(result)) {
        return std::nullopt;
    }

    OS::Windows::Com::UniquePtr<IDeviceTopology> connectedTopology;
    result = part->GetTopologyObject(connectedTopology.ReleaseAndAddressOf());
    if (FAILED(result)) {
        return std::nullopt;
    }

    LPWSTR rawFilterId{};
    result = connectedTopology->GetDeviceId(&rawFilterId);
    if (FAILED(result) || rawFilterId == nullptr) {
        return std::nullopt;
    }
    ResolvedFilter resolved;
    resolved.filterId = rawFilterId;
    CoTaskMemFree(rawFilterId);

    OS::Windows::Com::UniquePtr<IMMDevice> adapter;
    result = enumerator.GetDevice(resolved.filterId.c_str(), adapter.ReleaseAndAddressOf());
    if (FAILED(result)) {
        return std::nullopt;
    }

    result = adapter->Activate(
        resolved.ksControl.GetIID(), CLSCTX_ALL, nullptr,
        reinterpret_cast<void **>(resolved.ksControl.ReleaseAndAddressOf()));
    if (FAILED(result) || !resolved.ksControl) {
        return std::nullopt;
    }
    return resolved;
}

// KSPROPERTY_TYPE_BASICSUPPORT probe for one one-shot id; the BtAudio driver
// advertises support on the GET bit before it will honour the request.
bool SupportsOneShot(IKsControl &ksControl, ULONG propertyId)
{
    KSPROPERTY property{KSPROPERTYSETID_BtAudio, propertyId, KSPROPERTY_TYPE_BASICSUPPORT};
    ULONG supported{};
    ULONG returned{};
    const HRESULT result = ksControl.KsProperty(
        reinterpret_cast<PKSPROPERTY>(&property), sizeof(property), &supported, sizeof(supported),
        &returned);
    return SUCCEEDED(result) && returned >= sizeof(supported) &&
           (supported & KSPROPERTY_TYPE_GET) != 0;
}

HRESULT SendOneShot(IKsControl &ksControl, ULONG propertyId)
{
    KSPROPERTY property{KSPROPERTYSETID_BtAudio, propertyId, KSPROPERTY_TYPE_GET};
    ULONG returned{};
    return ksControl.KsProperty(
        reinterpret_cast<PKSPROPERTY>(&property), sizeof(property), nullptr, 0, &returned);
}

// One endpoint row of the target device, resolved on the calling worker.
struct AudioEndpointRow {
    Profile profile{Profile::Unclassified};
    DWORD state{};
    std::wstring filterId;
    bool reconnectSupported{};
    bool disconnectSupported{};
};

std::vector<AudioEndpointRow>
CollectDeviceEndpoints(uint64_t address, bool probeKs, std::stop_token stopToken)
{
    std::vector<AudioEndpointRow> rows;
    const auto addressToken = AddressToken(address);

    auto enumerator = CreateDeviceEnumerator();
    if (!enumerator) {
        return rows;
    }

    OS::Windows::Com::UniquePtr<IMMDeviceCollection> collection;
    HRESULT result = enumerator->EnumAudioEndpoints(
        eAll, DEVICE_STATEMASK_ALL, collection.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "EnumAudioEndpoints failed. HRESULT: {:#x}", result);
        return rows;
    }

    UINT count{};
    if (FAILED(collection->GetCount(&count))) {
        return rows;
    }

    std::set<std::wstring> seenFilters;
    for (UINT i = 0; i < count; ++i) {
        if (stopToken.stop_requested()) {
            break;
        }

        OS::Windows::Com::UniquePtr<IMMDevice> endpoint;
        if (FAILED(collection->Item(i, endpoint.ReleaseAndAddressOf()))) {
            continue;
        }

        DWORD state{};
        if (FAILED(endpoint->GetState(&state))) {
            continue;
        }

        auto resolved = ResolveFilter(*enumerator.operator->(), *endpoint.operator->());
        if (!resolved.has_value()) {
            continue;
        }
        const auto filterIdUpper = ToUpper(resolved->filterId);
        if (!FilterBelongsToAddress(filterIdUpper, addressToken)) {
            continue;
        }

        AudioEndpointRow row;
        row.profile = ClassifyFilter(filterIdUpper);
        row.state = state;
        row.filterId = std::move(resolved->filterId);
        if (row.profile == Profile::Unclassified) {
            continue; // Never touch endpoints whose profile we cannot name.
        }
        if (seenFilters.insert(row.filterId).second && probeKs &&
            (state == DEVICE_STATE_ACTIVE || state == DEVICE_STATE_NOTPRESENT))
        {
            row.reconnectSupported = SupportsOneShot(*resolved->ksControl, kOneShotReconnectId);
            row.disconnectSupported = SupportsOneShot(*resolved->ksControl, kOneShotDisconnectId);
        }
        rows.push_back(std::move(row));
    }

    return rows;
}

void AggregateProfile(
    const std::vector<AudioEndpointRow> &rows, Profile profile, ServiceStatus &statusOut)
{
    bool anyConnected{};
    bool anyDisconnectedUsable{};
    bool anySupported{};
    bool anyPresent{};
    for (const auto &row : rows) {
        if (row.profile != profile) {
            continue;
        }
        anyPresent = true;
        // One service status per profile: "controllable at all". Asymmetric
        // drivers (disconnect but no reconnect) are gated per operation inside
        // Submit, which reports the refusal instead of guessing.
        if (row.reconnectSupported || row.disconnectSupported) {
            anySupported = true;
        }
        if (row.state == DEVICE_STATE_ACTIVE) {
            anyConnected = true;
        }
        else if (row.state == DEVICE_STATE_NOTPRESENT) {
            anyDisconnectedUsable = true;
        }
    }

    if (!anyPresent || !anySupported) {
        statusOut = ServiceStatus::Unsupported;
    }
    else if (anyConnected) {
        statusOut = ServiceStatus::Connected;
    }
    else if (anyDisconnectedUsable) {
        statusOut = ServiceStatus::Disconnected;
    }
    else {
        // Present but user-disabled or otherwise inert: refused by the plan.
        statusOut = ServiceStatus::Unavailable;
    }
}

struct PairedLink {
    bool known{};
    bool readable{};
    bool connected{};
};

// bthprops read-back of the classic link: fConnected only reports true while
// the ACL link to this exact address is up. Without a local radio nothing is
// readable, which the policy treats as "no proof", never as "connected".
PairedLink ReadPairedLink(uint64_t address, std::stop_token stopToken)
{
    PairedLink link;
    if (stopToken.stop_requested()) {
        return link;
    }

    BLUETOOTH_FIND_RADIO_PARAMS radioSearch{};
    radioSearch.dwSize = sizeof(radioSearch);
    HANDLE radio = nullptr;
    if (!BluetoothFindFirstRadio(&radioSearch, &radio)) {
        return link;
    }

    BLUETOOTH_DEVICE_INFO info{};
    info.dwSize = sizeof(info);
    info.Address.ullLong = address;
    if (BluetoothGetDeviceInfo(radio, &info) == ERROR_SUCCESS) {
        link.readable = true;
        link.known = info.fRemembered != FALSE || info.fAuthenticated != FALSE;
        link.connected = info.fConnected != FALSE;
    }

    CloseHandle(radio);
    return link;
}

class WindowsAdapter final : public Adapter
{
public:
    std::optional<DeviceLinkState> Query(uint64_t address, std::stop_token stopToken) override
    {
        if (stopToken.stop_requested() || address == 0) {
            return std::nullopt;
        }

        ScopedComApartment apartment;

        const auto link = ReadPairedLink(address, stopToken);
        if (stopToken.stop_requested()) {
            return std::nullopt;
        }
        if (!link.known) {
            DeviceLinkState state;
            state.deviceKnown = false;
            state.linkReadable = link.readable;
            state.linkConnected = link.connected;
            return state;
        }

        const auto rows = CollectDeviceEndpoints(address, true, stopToken);
        if (stopToken.stop_requested()) {
            return std::nullopt;
        }

        DeviceLinkState state;
        state.deviceKnown = link.known;
        state.linkReadable = link.readable;
        state.linkConnected = link.connected;
        AggregateProfile(rows, Profile::A2DP, state.a2dp);
        AggregateProfile(rows, Profile::HFP, state.hfp);
        return state;
    }

    Failure Submit(uint64_t address, const ActionPlan &plan, std::stop_token stopToken) override
    {
        if (stopToken.stop_requested() || address == 0) {
            return Failure::Cancelled;
        }
        if (!plan.connectA2DP && !plan.connectHFP && !plan.disconnectA2DP && !plan.disconnectHFP) {
            return Failure::NoSupportedService;
        }

        ScopedComApartment apartment;

        // Resolve everything before the first state-changing request, then
        // send one request per driver filter. A rejection on some endpoints
        // still counts as submitted when another filter accepted: the polling
        // read-back, not this return code, decides the final outcome.
        const auto rows = CollectDeviceEndpoints(address, true, stopToken);
        if (stopToken.stop_requested()) {
            return Failure::Cancelled;
        }

        auto enumerator = CreateDeviceEnumerator();
        if (!enumerator) {
            return Failure::Unreadable;
        }

        int sent{};
        int rejected{};
        std::set<std::wstring> done;
        for (const auto &row : rows) {
            if (stopToken.stop_requested()) {
                return Failure::Cancelled;
            }

            const bool wantConnect = (row.profile == Profile::A2DP && plan.connectA2DP) ||
                                     (row.profile == Profile::HFP && plan.connectHFP);
            const bool wantDisconnect = (row.profile == Profile::A2DP && plan.disconnectA2DP) ||
                                        (row.profile == Profile::HFP && plan.disconnectHFP);
            if (!wantConnect && !wantDisconnect) {
                continue;
            }
            // Never enable user-disabled or absent endpoints, and never send
            // a request to an endpoint that already sits in the wanted state.
            if (row.state != DEVICE_STATE_ACTIVE && row.state != DEVICE_STATE_NOTPRESENT) {
                continue;
            }
            if (wantConnect && row.state == DEVICE_STATE_ACTIVE) {
                continue;
            }
            if (wantDisconnect && row.state == DEVICE_STATE_NOTPRESENT) {
                continue;
            }
            const ULONG propertyId = wantConnect ? kOneShotReconnectId : kOneShotDisconnectId;
            const bool supported = wantConnect ? row.reconnectSupported : row.disconnectSupported;
            if (!supported) {
                ++rejected;
                continue;
            }
            if (!done.insert(row.filterId).second) {
                continue;
            }

            // The reconnect path in QuickConnect_win activates IKsControl on
            // exactly this filter device; reuse that shape so a request can
            // only ever reach the target device's own driver instance.
            OS::Windows::Com::UniquePtr<IMMDevice> adapter;
            if (FAILED(enumerator->GetDevice(row.filterId.c_str(), adapter.ReleaseAndAddressOf())))
            {
                ++rejected;
                continue;
            }
            OS::Windows::Com::UniquePtr<IKsControl> ksControl;
            if (FAILED(adapter->Activate(
                    ksControl.GetIID(), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void **>(ksControl.ReleaseAndAddressOf()))) ||
                !ksControl)
            {
                ++rejected;
                continue;
            }
            if (!SupportsOneShot(*ksControl, propertyId)) {
                ++rejected;
                continue;
            }

            const HRESULT result = SendOneShot(*ksControl, propertyId);
            if (FAILED(result)) {
                LOG(Warn, "BtAudio one-shot {} request failed. Device: {}, HRESULT: {:#x}",
                    wantConnect ? "reconnect" : "disconnect", MaskAddressShort(address), result);
                ++rejected;
            }
            else {
                ++sent;
            }
        }

        if (sent == 0) {
            return rejected == 0 ? Failure::NoSupportedService : Failure::RequestRejected;
        }
        return Failure::None;
    }
};

} // namespace

std::unique_ptr<Adapter> CreateWindowsAdapter()
{
    return std::make_unique<WindowsAdapter>();
}

} // namespace Core::BluetoothLink
