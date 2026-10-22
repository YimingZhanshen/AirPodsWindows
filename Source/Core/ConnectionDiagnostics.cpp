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

#include <chrono>
#include <cstdio>

namespace Core::Diagnostics {

const char *StepName(Step step)
{
    switch (step) {
        case Step::ScannerStarted: return "scanner_started";
        case Step::AdvertisementMatched: return "advertisement_matched";
        case Step::DeviceLookup: return "device_lookup";
        case Step::ServiceEnabled: return "service_enabled";
        case Step::DriverOpen: return "driver_open";
        case Step::TransportConnect: return "transport_connect";
        case Step::Handshake: return "handshake";
        case Step::FeatureFlags: return "feature_flags";
        case Step::NotificationSubscribe: return "notification_subscribe";
        case Step::AttProbe: return "att_probe";
        case Step::Disconnect: return "disconnect";
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

Recorder::Recorder() : _startMillis{std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count()}
{}

void Recorder::Record(Step step, bool ok, std::string detail)
{
    auto nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

    std::lock_guard<std::mutex> lock{_mutex};
    // Guard against unbounded growth during a reconnect storm; the first and
    // last attempts are the interesting ones.
    if (_entries.size() >= 512) {
        _entries.erase(_entries.begin(), _entries.begin() + 256);
    }
    _entries.push_back(Entry{nowMillis - _startMillis, step, ok, std::move(detail)});
}

void Recorder::Reset()
{
    std::lock_guard<std::mutex> lock{_mutex};
    _entries.clear();
    _startMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
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
    for (const auto &entry : _entries) {
        report += "  ";
        report += std::to_string(entry.millisSinceStart);
        report += "  ";
        report += StepName(entry.step);
        report += entry.ok ? "  ok" : "  FAILED";
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
    std::snprintf(buffer, sizeof(buffer), "xxxx-xx-xx-xx-xx-%02X",
                  static_cast<unsigned>(address & 0xFF));
    return buffer;
}

size_t Recorder::StepCount() const
{
    std::lock_guard<std::mutex> lock{_mutex};
    return _entries.size();
}

} // namespace Core::Diagnostics
