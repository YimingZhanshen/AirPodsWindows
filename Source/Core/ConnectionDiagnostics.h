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
// Every stage of the ANC connection pipeline records one line: timestamp,
// step name, outcome, and a short detail. A successful transport write is
// PENDING until the device answers; only an observed ACK is recorded as ok.
// The report is redacted before it leaves this class: device addresses are
// masked and no serial numbers are ever stored.
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

const char *StepName(Step step);

class Recorder
{
public:
    static Recorder &Instance();

    Recorder();

    // Records a BLE/preconnect event, or an event in the currently open attempt.
    // `detail` must already be redacted by the caller.
    void Record(Step step, bool ok, std::string detail = {});
    void RecordPending(Step step, std::string detail = {});

    // Attempt-tagged events are accepted only while that exact generation is open.
    // A late reader/callback from a retired session therefore cannot pollute a new report.
    void RecordForAttempt(uint64_t generation, Step step, bool ok, std::string detail = {});
    void RecordPendingForAttempt(uint64_t generation, Step step, std::string detail = {});

    // Resets the log as the very first operation, then opens a fresh generation.
    // Repeating the currently open generation is idempotent.
    void BeginAttempt(uint64_t generation);

    // Closes only the named generation. A stale worker cannot end its successor.
    void EndAttempt(uint64_t generation);

    // Clears retained events and restarts the timestamp origin.
    void Reset();

    std::string BuildReport(const std::string &header) const;

    static std::string MaskAddress(uint64_t address);

    size_t StepCount() const;
    size_t ReportedAttemptStepCount() const;
    uint64_t CurrentGeneration() const;
    uint64_t ReportedAttempt() const;

private:
    enum class Outcome : uint8_t { Ok, Failed, Pending };

    struct Entry {
        int64_t millisSinceStart;
        uint64_t generation;
        Step step;
        Outcome outcome;
        std::string detail;
    };

    static constexpr size_t kMaxEntries = 512;

    void ResetLocked(int64_t nowMillis);
    void RecordLocked(
        Step step, Outcome outcome, std::string detail, uint64_t generation, int64_t nowMillis);

    mutable std::mutex _mutex;
    std::vector<Entry> _entries;
    uint64_t _generation{0};
    uint64_t _reportedAttempt{0};
    int64_t _startMillis;
};

} // namespace Core::Diagnostics
