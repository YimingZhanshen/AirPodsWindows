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

#include "ConnectionDiagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>

namespace Core::Diagnostics {

namespace {

int64_t SteadyMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

const char *StepName(Step step)
{
    switch (step) {
    case Step::ScannerStarted:
        return "scanner_started";
    case Step::AdvertisementMatched:
        return "advertisement_matched";
    case Step::DeviceLookup:
        return "device_lookup";
    case Step::ServiceEnabled:
        return "service_enabled";
    case Step::DriverOpen:
        return "driver_open";
    case Step::TransportConnect:
        return "transport_connect";
    case Step::Handshake:
        return "handshake";
    case Step::FeatureFlags:
        return "feature_flags";
    case Step::NotificationSubscribe:
        return "notification_subscribe";
    case Step::AttProbe:
        return "att_probe";
    case Step::Disconnect:
        return "disconnect";
    }
    return "unknown";
}

Recorder &Recorder::Instance()
{
    // Deliberately leaked: connection callbacks may outlive a controlled
    // shutdown window, and a static's destructor order across translation
    // units is not worth the risk for a diagnostic log.
    static Recorder *instance = new Recorder{};
    return *instance;
}

Recorder::Recorder() : _startMillis{SteadyMillis()} {}

void Recorder::ResetLocked(int64_t nowMillis)
{
    _entries.clear();
    _startMillis = nowMillis;
}

void Recorder::RecordLocked(
    Step step, Outcome outcome, std::string detail, uint64_t generation, int64_t nowMillis)
{
    if (_entries.size() >= kMaxEntries) {
        _entries.erase(_entries.begin());
    }
    _entries.push_back(
        Entry{nowMillis - _startMillis, generation, step, outcome, std::move(detail)});
}

void Recorder::Record(Step step, bool ok, std::string detail)
{
    const auto nowMillis = SteadyMillis();
    std::lock_guard<std::mutex> lock{_mutex};
    RecordLocked(
        step, ok ? Outcome::Ok : Outcome::Failed, std::move(detail), _generation, nowMillis);
}

void Recorder::RecordPending(Step step, std::string detail)
{
    const auto nowMillis = SteadyMillis();
    std::lock_guard<std::mutex> lock{_mutex};
    RecordLocked(step, Outcome::Pending, std::move(detail), _generation, nowMillis);
}

void Recorder::RecordForAttempt(uint64_t generation, Step step, bool ok, std::string detail)
{
    const auto nowMillis = SteadyMillis();
    std::lock_guard<std::mutex> lock{_mutex};
    if (generation == 0 || generation != _generation) {
        return;
    }
    RecordLocked(
        step, ok ? Outcome::Ok : Outcome::Failed, std::move(detail), generation, nowMillis);
}

void Recorder::RecordPendingForAttempt(uint64_t generation, Step step, std::string detail)
{
    const auto nowMillis = SteadyMillis();
    std::lock_guard<std::mutex> lock{_mutex};
    if (generation == 0 || generation != _generation) {
        return;
    }
    RecordLocked(step, Outcome::Pending, std::move(detail), generation, nowMillis);
}

void Recorder::BeginAttempt(uint64_t generation)
{
    if (generation == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock{_mutex};
    if (_generation == generation) {
        return;
    }

    // Reset is deliberately the first effective operation of a new attempt. No event from
    // the preceding generation (including untagged preconnect lines) can survive it.
    ResetLocked(SteadyMillis());
    _generation = generation;
    _reportedAttempt = generation;
}

void Recorder::EndAttempt(uint64_t generation)
{
    std::lock_guard<std::mutex> lock{_mutex};
    if (generation != 0 && _generation == generation) {
        _generation = 0;
    }
}

void Recorder::Reset()
{
    const auto nowMillis = SteadyMillis();
    std::lock_guard<std::mutex> lock{_mutex};
    ResetLocked(nowMillis);
}

std::string Recorder::BuildReport(const std::string &header) const
{
    std::lock_guard<std::mutex> lock{_mutex};

    std::string report;
    if (!header.empty()) {
        report += header;
        report += "\n\n";
    }
    report += "steps (ms since start):\n";

    uint64_t group = std::numeric_limits<uint64_t>::max();
    for (const auto &entry : _entries) {
        if (entry.generation != 0 && entry.generation != _reportedAttempt) {
            continue;
        }
        if (entry.generation != group) {
            group = entry.generation;
            report += group == 0 ? "  preconnect:\n" : "  attempt " + std::to_string(group) + ":\n";
        }
        report += "    ";
        report += std::to_string(entry.millisSinceStart);
        report += "  ";
        report += StepName(entry.step);
        switch (entry.outcome) {
        case Outcome::Ok:
            report += "  ok";
            break;
        case Outcome::Failed:
            report += "  FAILED";
            break;
        case Outcome::Pending:
            report += "  PENDING";
            break;
        }
        if (!entry.detail.empty()) {
            report += "  ";
            report += entry.detail;
        }
        report += "\n";
    }
    return report;
}

std::string Recorder::MaskAddress(uint64_t address)
{
    // Keep the last byte (models stay distinguishable), hide the rest.
    char buffer[32];
    std::snprintf(
        buffer, sizeof(buffer), "xxxx-xx-xx-xx-xx-%02X", static_cast<unsigned>(address & 0xFF));
    return buffer;
}

size_t Recorder::StepCount() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _entries.size();
}

size_t Recorder::ReportedAttemptStepCount() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    if (_reportedAttempt == 0) {
        return 0;
    }
    return static_cast<size_t>(
        std::count_if(_entries.begin(), _entries.end(), [this](const Entry &entry) {
            return entry.generation == _reportedAttempt;
        }));
}

uint64_t Recorder::CurrentGeneration() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _generation;
}

uint64_t Recorder::ReportedAttempt() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _reportedAttempt;
}

} // namespace Core::Diagnostics
