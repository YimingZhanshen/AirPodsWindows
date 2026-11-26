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

#include "HeadTrackingPresentation.h"

#include <cstring>

namespace Gui {

namespace {

// Reads one little-endian int16, the same way `Core::AAP::ParseHeadTrackingData` does.
std::int16_t ReadInt16LE(std::span<const std::uint8_t> packet, std::size_t offset) noexcept
{
    return static_cast<std::int16_t>(
        static_cast<std::uint16_t>(packet[offset]) |
        (static_cast<std::uint16_t>(packet[offset + 1]) << 8));
}

} // namespace

HeadTrackingRecord ClassifyHeadTrackingRecord(std::span<const std::uint8_t> packet) noexcept
{
    // The ten-byte prefix plus the length the fields need. Everything shorter cannot be read
    // safely, so it is not a head-tracking record as far as the view is concerned.
    if (packet.size() < kHeadTrackingRecordSize) {
        return HeadTrackingRecord::NotHeadTracking;
    }

    static constexpr std::array<std::uint8_t, 10> kPrefix{0x04, 0x00, 0x04, 0x00, 0x17,
                                                          0x00, 0x00, 0x00, 0x10, 0x00};
    if (std::memcmp(packet.data(), kPrefix.data(), kPrefix.size()) != 0) {
        return HeadTrackingRecord::NotHeadTracking;
    }

    if (packet[11] != 0x00) {
        return HeadTrackingRecord::NotHeadTracking;
    }

    return HeadTrackingRecord::Stream;
}

std::optional<HeadTrackingCounts>
ReadHeadTrackingCounts(std::span<const std::uint8_t> packet) noexcept
{
    if (packet.size() < kHeadTrackingRecordSize) {
        return std::nullopt;
    }

    HeadTrackingCounts counts;
    counts.orientation[0] = ReadInt16LE(packet, kHeadTrackingOrientationOffset);
    counts.orientation[1] = ReadInt16LE(packet, kHeadTrackingOrientationOffset + 2);
    counts.orientation[2] = ReadInt16LE(packet, kHeadTrackingOrientationOffset + 4);
    counts.horizontalAcceleration = ReadInt16LE(packet, kHeadTrackingHorizontalAccelerationOffset);
    counts.verticalAcceleration = ReadInt16LE(packet, kHeadTrackingVerticalAccelerationOffset);
    return counts;
}

HeadTrackingSample NormalizeHeadTrackingSample(const HeadTrackingCounts &counts) noexcept
{
    HeadTrackingSample sample;
    sample.raw = counts;
    for (std::size_t index = 0; index < counts.orientation.size(); ++index) {
        sample.orientation[index] = NormalizeSensorCount(counts.orientation[index]);
    }
    sample.horizontalAcceleration = NormalizeSensorCount(counts.horizontalAcceleration);
    sample.verticalAcceleration = NormalizeSensorCount(counts.verticalAcceleration);
    return sample;
}

void HeadTrackingViewModel::ResetReadout()
{
    _sample = {};
    _hasSample = false;
    _acceptedRecords = 0;
    _publishedUpdates = 0;
    _lastRecord.reset();
    _lastPublish.reset();
}

void HeadTrackingViewModel::SetEnabled(bool enabled, TimePoint now)
{
    if (_enabled == enabled) {
        return;
    }

    _enabled = enabled;
    // Switching off drops the readout; switching on waits for this session's first record.
    ResetReadout();
    _lastPhase = Phase(now);
}

void HeadTrackingViewModel::OnTransportUp(std::uint64_t generation, TimePoint now)
{
    if (_linkUp && _generation == generation) {
        return;
    }

    _linkUp = true;
    _generation = generation;
    // The device restarted its stream with this session: forget everything from before it, and
    // let the first record through the rate gate so the card fills in without a 100 ms delay.
    ResetReadout();
    _lastPhase = Phase(now);
}

void HeadTrackingViewModel::OnTransportDown(std::uint64_t generation, TimePoint now)
{
    // A loss report for a session this view never adopted must not freeze a live readout.
    if (!_linkUp || _generation != generation) {
        return;
    }

    _linkUp = false;
    _lastPhase = Phase(now);
}

std::optional<HeadTrackingView> HeadTrackingViewModel::OnCounts(
    const HeadTrackingCounts &counts, std::uint64_t generation, HeadTrackingRecord record,
    TimePoint now)
{
    if (record == HeadTrackingRecord::NotHeadTracking || !_enabled || !_linkUp ||
        generation != _generation || generation == 0)
    {
        return std::nullopt;
    }

    _sample = NormalizeHeadTrackingSample(counts);
    _hasSample = true;
    _lastRecord = now;
    ++_acceptedRecords;

    const bool gateOpen = !_lastPublish.has_value() || (now - *_lastPublish) >= kPublishInterval;
    if (!gateOpen) {
        return std::nullopt;
    }

    _lastPublish = now;
    ++_publishedUpdates;
    _lastPhase = Phase(now);
    return Present(now);
}

std::optional<HeadTrackingView> HeadTrackingViewModel::OnPacket(
    std::span<const std::uint8_t> packet, std::uint64_t generation, TimePoint now)
{
    const auto record = ClassifyHeadTrackingRecord(packet);
    if (record == HeadTrackingRecord::NotHeadTracking) {
        return std::nullopt;
    }

    auto counts = ReadHeadTrackingCounts(packet);
    if (!counts.has_value()) {
        return std::nullopt;
    }
    return OnCounts(*counts, generation, record, now);
}

std::optional<HeadTrackingView> HeadTrackingViewModel::Tick(TimePoint now)
{
    const auto phase = Phase(now);
    if (phase == _lastPhase) {
        return std::nullopt;
    }

    _lastPhase = phase;
    return Present(now);
}

HeadTrackingPhase HeadTrackingViewModel::Phase(TimePoint now) const
{
    if (!_enabled) {
        return HeadTrackingPhase::Off;
    }
    if (!_linkUp) {
        return HeadTrackingPhase::LinkDown;
    }
    if (!_lastRecord.has_value()) {
        return HeadTrackingPhase::Armed;
    }
    if ((now - *_lastRecord) >= kStaleAfter) {
        return HeadTrackingPhase::Stale;
    }
    return HeadTrackingPhase::Streaming;
}

HeadTrackingView HeadTrackingViewModel::Present(TimePoint now) const
{
    HeadTrackingView view;
    view.phase = Phase(now);
    view.sample = _sample;
    view.hasSample = _hasSample;
    view.generation = _generation;
    view.acceptedRecords = _acceptedRecords;
    view.publishedUpdates = _publishedUpdates;

    if (_lastRecord.has_value()) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - *_lastRecord);
        // Whole seconds: keeps two consecutive views equal while the card sits idle, so an
        // identical repaint can be skipped.
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(age).count();
        view.sinceLastRecord = std::chrono::seconds{seconds};
    }

    return view;
}

} // namespace Gui
