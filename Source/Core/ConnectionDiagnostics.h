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

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Connection diagnostics step log (blueprint 1.4).
//
// Every stage of the ANC connection pipeline records one line — timestamp,
// step name, ok/failed, and a short detail — so that a user reporting "it does
// not connect" can export a report that pinpoints the failing stage without a
// debug build. The report is redacted before it leaves this class: device
// addresses are masked and no serial numbers are ever stored.
//
// The log is process-global (a singleton): connection attempts originate from
// several threads (BLE watcher, device lookup, AAP reader), and the export
// must capture whichever attempt failed. Recording is best-effort and never
// blocks the connection path.
//
namespace Core::Diagnostics {

enum class Step : uint8_t {
    ScannerStarted,
    AdvertisementMatched,
    DeviceLookup,
    ServiceEnabled,
    DriverOpen,
    TransportConnect,
    Handshake,
    FeatureFlags,
    NotificationSubscribe,
    AttProbe,
    Disconnect,
};

// Human-readable, stable step names (English in the export; the UI translates
// around them). Also used by tests to assert the recording contract.
const char *StepName(Step step);

class Recorder
{
public:
    static Recorder &Instance();

    // Records one step. `detail` must already be redacted by the caller.
    void Record(Step step, bool ok, std::string detail = {});

    // Clears the log (called at the start of a fresh connection attempt).
    void Reset();

    // Renders the report: header (app/OS/driver/connection summary supplied by
    // the caller) followed by one line per recorded step.
    std::string BuildReport(const std::string &header) const;

    // Masks a classic-Bluetooth device address for display: keeps the last
    // byte so two devices remain distinguishable, hides the rest.
    static std::string MaskAddress(uint64_t address);

    size_t StepCount() const;

private:
    struct Entry {
        int64_t millisSinceStart;
        Step step;
        bool ok;
        std::string detail;
    };

    mutable std::mutex _mutex;
    std::vector<Entry> _entries;
    int64_t _startMillis;
};

} // namespace Core::Diagnostics
