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

#include <cassert>

#include "OS/Windows.h"

#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    std::string suffix;
    suffix.reserve(4);
    for (size_t i = 8; i < 12; ++i) {
        suffix.push_back(static_cast<char>(buffer[i]));
    }
    return "***" + suffix;
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
// advertises support on the GET bit before it will honour the request. A failed
// or short query is unreadable evidence, not proof that the profile is absent.
std::optional<bool> SupportsOneShot(IKsControl &ksControl, ULONG propertyId)
{
    KSPROPERTY property{KSPROPSETID_BtAudio, propertyId, KSPROPERTY_TYPE_BASICSUPPORT};
    ULONG supported{};
    ULONG returned{};
    const HRESULT result = ksControl.KsProperty(
        reinterpret_cast<PKSPROPERTY>(&property), sizeof(property), &supported, sizeof(supported),
        &returned);
    if (FAILED(result) || returned < sizeof(supported)) {
        return std::nullopt;
    }
    return (supported & KSPROPERTY_TYPE_GET) != 0;
}

HRESULT SendOneShot(IKsControl &ksControl, ULONG propertyId)
{
    KSPROPERTY property{KSPROPSETID_BtAudio, propertyId, KSPROPERTY_TYPE_GET};
    ULONG returned{};
    return ksControl.KsProperty(
        reinterpret_cast<PKSPROPERTY>(&property), sizeof(property), nullptr, 0, &returned);
}

// One endpoint row of the target device, resolved on the calling worker.
struct AudioEndpointRow {
    Profile profile{Profile::Unclassified};
    DWORD state{};
    std::wstring filterId;
    std::optional<bool> profileConnected;
    std::optional<bool> reconnectSupported;
    std::optional<bool> disconnectSupported;
    bool readable{true};
};

struct EndpointCollection {
    std::vector<AudioEndpointRow> rows;
    bool readable{};
};

// KSPROPERTY_JACK_DESCRIPTION is pin-wise. Query every bridge pin and accept a
// connected jack only when the returned KSMULTIPLE_ITEM/KSJACK_DESCRIPTION
// buffer is structurally valid. Unsupported or malformed driver evidence stays
// unknown; MMDevice ACTIVE is never promoted to profile proof.
std::optional<bool> ReadJackConnection(IKsControl &ksControl, std::stop_token stopToken)
{
    KSP_PIN pinCountProperty{
        KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, KSPROPERTY_TYPE_GET, 0,
        KSPROPERTY_PIN_FLAGS_ATTRIBUTE_RANGE_AWARE};
    ULONG pinCount{};
    ULONG returned{};
    HRESULT result = ksControl.KsProperty(
        reinterpret_cast<PKSPROPERTY>(&pinCountProperty), sizeof(pinCountProperty), &pinCount,
        sizeof(pinCount), &returned);
    if (FAILED(result) || returned < sizeof(pinCount) || pinCount == 0) {
        return std::nullopt;
    }

    bool sawConnected = false;
    bool sawDescription = false;
    for (ULONG pinId = 0; pinId < pinCount; ++pinId) {
        if (stopToken.stop_requested()) {
            return std::nullopt;
        }
        KSP_PIN request{
            KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION, KSPROPERTY_TYPE_GET, pinId,
            KSPROPERTY_PIN_FLAGS_ATTRIBUTE_RANGE_AWARE};
        KSMULTIPLE_ITEM header{};
        returned = 0;
        result = ksControl.KsProperty(
            reinterpret_cast<PKSPROPERTY>(&request), sizeof(request), &header, sizeof(header),
            &returned);
        // Variable-length KS properties report ERROR_MORE_DATA when the first
        // pass supplies only the KSMULTIPLE_ITEM header. The returned byte
        // count, not S_OK alone, validates that sizing pass.
        if (returned < sizeof(header) ||
            !(SUCCEEDED(result) || result == HRESULT_FROM_WIN32(ERROR_MORE_DATA)))
        {
            continue;
        }
        if (header.Count == 0) {
            continue;
        }
        const auto descriptionSize = static_cast<size_t>(header.Count) * sizeof(KSJACK_DESCRIPTION);
        if (header.Size < sizeof(header) || header.Size - sizeof(header) < descriptionSize ||
            descriptionSize > 64 * 1024)
        {
            continue;
        }

        const auto bufferSize = static_cast<size_t>(header.Size);
        std::vector<std::max_align_t> storage(
            (bufferSize + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto *buffer = reinterpret_cast<std::byte *>(storage.data());
        std::memcpy(buffer, &header, sizeof(header));
        returned = 0;
        result = ksControl.KsProperty(
            reinterpret_cast<PKSPROPERTY>(&request), sizeof(request), buffer,
            static_cast<ULONG>(bufferSize), &returned);
        if (FAILED(result) || returned < bufferSize) {
            continue;
        }
        const auto *descriptions =
            reinterpret_cast<const KSJACK_DESCRIPTION *>(buffer + sizeof(KSMULTIPLE_ITEM));
        sawDescription = true;
        for (ULONG i = 0; i < header.Count; ++i) {
            sawConnected = sawConnected || descriptions[i].IsConnected != FALSE;
        }
    }
    return sawDescription ? std::optional<bool>{sawConnected} : std::nullopt;
}

EndpointCollection CollectDeviceEndpoints(uint64_t address, bool probeKs, std::stop_token stopToken)
{
    EndpointCollection collection;
    collection.readable = true;
    const auto addressToken = AddressToken(address);

    auto enumerator = CreateDeviceEnumerator();
    if (!enumerator) {
        collection.readable = false;
        return collection;
    }

    OS::Windows::Com::UniquePtr<IMMDeviceCollection> endpoints;
    HRESULT result =
        enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, endpoints.ReleaseAndAddressOf());
    if (FAILED(result)) {
        LOG(Warn, "EnumAudioEndpoints failed. HRESULT: {:#x}", result);
        collection.readable = false;
        return collection;
    }

    UINT count{};
    if (FAILED(endpoints->GetCount(&count))) {
        collection.readable = false;
        return collection;
    }

    for (UINT i = 0; i < count; ++i) {
        if (stopToken.stop_requested()) {
            collection.readable = false;
            return collection;
        }

        OS::Windows::Com::UniquePtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(i, endpoint.ReleaseAndAddressOf()))) {
            collection.readable = false;
            continue;
        }

        DWORD state{};
        if (FAILED(endpoint->GetState(&state))) {
            collection.readable = false;
            continue;
        }

        auto resolved = ResolveFilter(*enumerator.operator->(), *endpoint.operator->());
        if (!resolved.has_value()) {
            // Without the filter id we cannot know whether this endpoint
            // belongs to the target. Skipping it could turn a failed query
            // into false Absent/Disconnected evidence.
            collection.readable = false;
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
        if (probeKs) {
            auto *ksControl = resolved->ksControl.operator->();
            row.reconnectSupported = SupportsOneShot(*ksControl, kOneShotReconnectId);
            row.disconnectSupported = SupportsOneShot(*ksControl, kOneShotDisconnectId);
            row.readable =
                row.reconnectSupported.has_value() && row.disconnectSupported.has_value();
            if (state == DEVICE_STATE_ACTIVE) {
                row.profileConnected = ReadJackConnection(*ksControl, stopToken);
            }
            else if (state == DEVICE_STATE_UNPLUGGED || state == DEVICE_STATE_NOTPRESENT) {
                row.profileConnected = false;
            }
        }
        collection.rows.push_back(std::move(row));
    }

    return collection;
}

void AggregateProfile(
    const std::vector<AudioEndpointRow> &rows, Profile profile, ServiceStatus &statusOut)
{
    std::vector<EndpointEvidence> evidence;
    for (const auto &row : rows) {
        if (row.profile != profile) {
            continue;
        }
        const bool available = row.state == DEVICE_STATE_ACTIVE ||
                               row.state == DEVICE_STATE_UNPLUGGED ||
                               row.state == DEVICE_STATE_NOTPRESENT;
        const bool controllable = row.reconnectSupported == std::optional<bool>{true} ||
                                  row.disconnectSupported == std::optional<bool>{true};
        evidence.push_back({true, controllable, row.profileConnected, available, row.readable});
    }
    statusOut = AggregateEndpointEvidence(evidence);
}

struct PairedLink {
    bool known{};
    bool readable{};
    bool connected{};
};

// bthprops read-back of the classic link: fConnected only reports true while
// the ACL link to this exact address is up. Every local radio is inspected; a
// device paired to a later radio must not look absent merely because the first
// radio did not contain it.
PairedLink ReadPairedLink(uint64_t address, std::stop_token stopToken)
{
    PairedLink link;
    if (stopToken.stop_requested()) {
        return link;
    }

    BLUETOOTH_FIND_RADIO_PARAMS radioSearch{};
    radioSearch.dwSize = sizeof(radioSearch);
    bool sawRadio{};
    bool missingOnly = true;
    const auto enumeration = Details::EnumerateRadioHandles<HBLUETOOTH_RADIO_FIND, HANDLE>(
        [&](HANDLE *radio) { return BluetoothFindFirstRadio(&radioSearch, radio); },
        [](HBLUETOOTH_RADIO_FIND search, HANDLE *radio) {
            return BluetoothFindNextRadio(search, radio);
        },
        [](HBLUETOOTH_RADIO_FIND search) { BluetoothFindRadioClose(search); },
        [](HANDLE radio) { CloseHandle(radio); }, [] { return GetLastError(); },
        [&](HANDLE radio) {
            sawRadio = true;
            if (stopToken.stop_requested()) {
                return false;
            }

            BLUETOOTH_DEVICE_INFO info{};
            info.dwSize = sizeof(info);
            info.Address.ullLong = address;
            const auto result = BluetoothGetDeviceInfo(radio, &info);
            if (result == ERROR_SUCCESS) {
                link.readable = true;
                link.known = info.fRemembered != FALSE || info.fAuthenticated != FALSE;
                link.connected = link.connected || info.fConnected != FALSE;
            }
            else if (result == ERROR_NOT_FOUND) {
                link.readable = true;
            }
            else {
                missingOnly = false;
            }
            return true;
        });

    if (!enumeration.started || !enumeration.completed || stopToken.stop_requested()) {
        return {};
    }
    if (sawRadio && missingOnly) {
        link.readable = true;
    }
    if (!link.known && !missingOnly) {
        link.readable = false;
    }
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
        if (!link.readable) {
            return std::nullopt;
        }
        if (!link.known) {
            DeviceLinkState state;
            state.deviceKnown = false;
            state.linkReadable = link.readable;
            state.linkConnected = link.connected;
            return state;
        }

        const auto endpoints = CollectDeviceEndpoints(address, true, stopToken);
        if (stopToken.stop_requested()) {
            return std::nullopt;
        }
        if (!endpoints.readable) {
            return std::nullopt;
        }

        DeviceLinkState state;
        state.deviceKnown = link.known;
        state.linkReadable = link.readable;
        state.linkConnected = link.connected;
        AggregateProfile(endpoints.rows, Profile::A2DP, state.a2dp);
        AggregateProfile(endpoints.rows, Profile::HFP, state.hfp);
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
        const auto endpoints = CollectDeviceEndpoints(address, true, stopToken);
        if (stopToken.stop_requested()) {
            return Failure::Cancelled;
        }
        if (!endpoints.readable) {
            return Failure::Unreadable;
        }
        auto enumerator = CreateDeviceEnumerator();
        if (!enumerator) {
            return Failure::Unreadable;
        }

        for (const auto &row : endpoints.rows) {
            const bool targeted =
                (row.profile == Profile::A2DP && (plan.connectA2DP || plan.disconnectA2DP)) ||
                (row.profile == Profile::HFP && (plan.connectHFP || plan.disconnectHFP));
            if (targeted && !row.readable) {
                return Failure::Unreadable;
            }
        }

        int sent{};
        int rejected{};
        std::set<std::wstring> done;
        for (const auto &row : endpoints.rows) {
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
            // Never enable user-disabled endpoints. UNPLUGGED and NOTPRESENT
            // are valid reconnect targets (notably HFP), while ACTIVE is only
            // skipped when driver evidence proves the profile already up.
            if (row.state != DEVICE_STATE_ACTIVE && row.state != DEVICE_STATE_UNPLUGGED &&
                row.state != DEVICE_STATE_NOTPRESENT)
            {
                continue;
            }
            if (wantConnect && row.profileConnected == std::optional<bool>{true}) {
                continue;
            }
            if (wantDisconnect && row.profileConnected == std::optional<bool>{false}) {
                continue;
            }
            const ULONG propertyId = wantConnect ? kOneShotReconnectId : kOneShotDisconnectId;
            const auto supported = wantConnect ? row.reconnectSupported : row.disconnectSupported;
            if (supported != std::optional<bool>{true}) {
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
            auto *rawKsControl = ksControl.operator->();
            if (!SupportsOneShot(*rawKsControl, propertyId)) {
                ++rejected;
                continue;
            }

            const HRESULT result = SendOneShot(*rawKsControl, propertyId);
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
