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

#include <functional>
#include <optional>
#include <vector>

#include <QFrame>
#include <QLabel>
#include <QTimer>

#include "../../Core/AirPods.h"
#include "../HeadTrackingPresentation.h"
#include "ActivityIndicator.h"

namespace Gui {

// Settings-page card for blueprint 2.5: the live head-tracking stream.
//
// It is a renderer and nothing else. All policy — which records count, how they are normalized,
// how often the view refreshes, when the readout goes stale — lives in the Qt-free
// `HeadTrackingViewModel`, which the card drives on the GUI thread from the manager's queued
// `*Safely` signals. That split is what keeps the AAP reader thread away from QWidget code.
//
// Field mapping: the protocol notes call the three attitude channels "orientation 1/2/3" and
// publish no axis names and no physical scale, and LibrePods plots the raw counts on an
// auto-scaled axis. So the card labels the rows by protocol name and shows each value as a
// signed full-scale fraction plus the raw count, instead of inventing "yaw = 37 deg".
class HeadTrackingCard : public QFrame
{
    Q_OBJECT

public:
    explicit HeadTrackingCard(QWidget *parent = nullptr);

    // Provider resolves the application's manager lazily; the card stays inert (hidden) without
    // one, the same contract as DriverStatusCard, so GUI tests can build it standalone.
    void SetManagerProvider(std::function<Core::AirPods::Manager &()> provider);

    // Re-reads enable/link state from the manager and repaints.
    void Refresh();
    // Pushes a view into the labels. Public so a test can drive the card without a device.
    void ApplyView(const HeadTrackingView &view);

    // Rendered strings, kept static so the mapping is checkable without a widget.
    static QString PhaseText(HeadTrackingPhase phase);
    // "-0.500" for the normalized column, "-16384" for the raw one, or an em dash when empty.
    static QString FormatNormalized(double value, bool available);
    static QString FormatRaw(std::int16_t value, bool available);
    // The five value rows of `view`, name first. Order: orientation 1/2/3, acceleration H/V.
    static std::vector<QString> ValueRows(const HeadTrackingView &view);

    // Reduced motion (Settings ▸ accessibility): the pulsing live indicator is decorative, so it
    // is dropped and the status text carries "streaming" instead. Tests use the override because
    // a CI runner's OS hint is not something the card should depend on.
    static bool IsReducedMotion();
    void SetReducedMotionOverride(std::optional<bool> reduced);

    // Exposed for the rendering tests.
    HeadTrackingView View() const
    {
        return _view;
    }

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void OnRecord(const Core::AirPods::HeadTrackingEvent &record);
    void OnDesired(bool desired);
    void OnSession(bool live, qulonglong generation);
    void OnTick();
    void Publish();
    void UpdateIndicator();
    void ApplyTheme();

    QLabel *_titleLabel = new QLabel{this};
    QLabel *_statusLabel = new QLabel{this};
    QLabel *_detailLabel = new QLabel{this};
    QLabel *_noteLabel = new QLabel{this};
    std::vector<QLabel *> _valueLabels;
    Widget::ActivityIndicator *_pulse = nullptr;

    HeadTrackingViewModel _model;
    HeadTrackingView _view;
    std::function<Core::AirPods::Manager &()> _getManager;
    std::optional<bool> _reducedMotionOverride;
    bool _signalsConnected{false};
    QTimer _tickTimer{this};
};

} // namespace Gui
