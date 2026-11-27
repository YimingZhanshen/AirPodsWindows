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

#include "HeadTrackingCard.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QStyle>
#include <QVBoxLayout>

#if defined(APD_OS_WIN)
    #include <Windows.h>
#endif

#include "../Theme.h"
#include "ActivityIndicator.h"

namespace Gui {

namespace {

// `QT_TRANSLATE_NOOP` instead of `tr()` because the strings reach lupdate through an array;
// the context matches the class name lupdate assigns to `tr()` in this widget.
constexpr const char *kFieldNames[] = {
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Orientation 1"),
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Orientation 2"),
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Orientation 3"),
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Acceleration, horizontal"),
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Acceleration, vertical"),
};

constexpr const char *kColumnHeaders[] = {
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Full scale"),
    QT_TRANSLATE_NOOP("Gui::HeadTrackingCard", "Raw"),
};

using TimePoint = HeadTrackingViewModel::Clock::time_point;

TimePoint Now()
{
    return HeadTrackingViewModel::Clock::now();
}

} // namespace

HeadTrackingCard::HeadTrackingCard(QWidget *parent) : QFrame{parent}
{
    setObjectName("cardHeadTrackingData");
    setProperty("cssClass", "settingCard");
    setFrameShape(QFrame::NoFrame);

    _titleLabel->setObjectName("htTitleLabel");
    _titleLabel->setProperty("cssClass", "cardTitle");

    _statusLabel->setWordWrap(true);
    _statusLabel->setObjectName("htStatusLabel");
    _statusLabel->setProperty("cssClass", "cardTitle");

    _detailLabel->setWordWrap(true);
    _detailLabel->setObjectName("htDetailLabel");
    _detailLabel->setProperty("cssClass", "cardDescription");

    _noteLabel->setWordWrap(true);
    _noteLabel->setObjectName("htNoteLabel");
    _noteLabel->setProperty("cssClass", "cardDescription");

    _pulse = new Widget::ActivityIndicator{16, this};

    auto *headerLayout = new QHBoxLayout;
    headerLayout->setSpacing(8);
    headerLayout->addWidget(_titleLabel, 1);
    headerLayout->addWidget(_pulse, 0, Qt::AlignVCenter | Qt::AlignRight);

    auto *gridLayout = new QGridLayout;
    gridLayout->setHorizontalSpacing(12);
    gridLayout->setVerticalSpacing(2);

    for (std::size_t column = 0; column < std::size(kColumnHeaders); ++column) {
        auto *header = new QLabel{
            QCoreApplication::translate("Gui::HeadTrackingCard", kColumnHeaders[column]), this};
        header->setProperty("cssClass", "cardDescription");
        header->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        header->setObjectName(
            QStringLiteral("htColumn%1").arg(static_cast<qulonglong>(column + 1)));
        _columnLabels.push_back(header);
        gridLayout->addWidget(
            header, 0, static_cast<int>(column) + 1, Qt::AlignRight | Qt::AlignVCenter);
    }

    for (std::size_t row = 0; row < std::size(kFieldNames); ++row) {
        auto *name = new QLabel{
            QCoreApplication::translate("Gui::HeadTrackingCard", kFieldNames[row]), this};
        name->setWordWrap(true);
        name->setProperty("cssClass", "cardDescription");
        name->setObjectName(QStringLiteral("htField%1").arg(static_cast<qulonglong>(row + 1)));
        _fieldLabels.push_back(name);

        auto *value = new QLabel{this};
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);

        // Fixed-width digits keep the column steady while the stream runs.
        auto *rawValue = new QLabel{this};
        rawValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rawValue->setProperty("cssClass", "cardDescription");

        // Object names are the handles the rendering tests use.
        value->setObjectName(QStringLiteral("htv%1").arg(static_cast<qulonglong>(row + 1)));
        rawValue->setObjectName(QStringLiteral("htr%1").arg(static_cast<qulonglong>(row + 1)));

        gridLayout->addWidget(name, static_cast<int>(row) + 1, 0);
        gridLayout->addWidget(value, static_cast<int>(row) + 1, 1);
        gridLayout->addWidget(rawValue, static_cast<int>(row) + 1, 2);

        _valueLabels.push_back(value);
        _valueLabels.push_back(rawValue);
    }
    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 0);
    gridLayout->setColumnStretch(2, 0);

    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(6);
    layout->addLayout(headerLayout);
    layout->addWidget(_statusLabel);
    layout->addWidget(_detailLabel);
    layout->addLayout(gridLayout);
    layout->addWidget(_noteLabel);
    setLayout(layout);

    _tickTimer.setInterval(1000);
    connect(&_tickTimer, &QTimer::timeout, this, &HeadTrackingCard::OnTick);

    ApplyTheme();
    // No provider yet: stay hidden until SetManagerProvider() runs, like DriverStatusCard.
    setVisible(false);
    // Paint the empty state directly: `ApplyView` would compare it against the default `_view`
    // and skip the labels, leaving them blank instead of showing the "-" placeholders.
    Retranslate();

    connect(
        &Theme::Manager::Instance(), &Theme::Manager::Changed, this, &HeadTrackingCard::ApplyTheme);
}

void HeadTrackingCard::SetManagerProvider(std::function<Core::AirPods::Manager &()> provider)
{
    _getManager = std::move(provider);

    if (_getManager && !_signalsConnected) {
        _signalsConnected = true;
        auto &manager = _getManager();
        // Auto connection: emitted from AAP threads, delivered queued on the GUI thread, so the
        // reader thread never runs widget code.
        QObject::connect(
            &manager, &Core::AirPods::Manager::HeadTrackingRecordSafely, this,
            &HeadTrackingCard::OnRecord);
        QObject::connect(
            &manager, &Core::AirPods::Manager::HeadTrackingDesiredSafely, this,
            &HeadTrackingCard::OnDesired);
        QObject::connect(
            &manager, &Core::AirPods::Manager::HeadTrackingSessionSafely, this,
            &HeadTrackingCard::OnSession);
    }

    Refresh();
}

void HeadTrackingCard::Refresh()
{
    if (!_getManager) {
        setVisible(false);
        return;
    }

    setVisible(true);
    auto &manager = _getManager();
    const auto now = Now();
    _model.SetEnabled(manager.IsHeadTrackingEnabled(), now);

    // Adopt the session that is already running: the card is usually built after the connection.
    const auto generation = manager.GetHeadTrackingGeneration();
    if (generation != 0) {
        _model.OnTransportUp(generation, now);
    }
    else {
        _model.OnTransportDown(_model.Generation(), now);
    }

    ApplyView(_model.Present(now));
}

void HeadTrackingCard::OnRecord(const Core::AirPods::HeadTrackingEvent &record)
{
    HeadTrackingCounts counts;
    counts.orientation = {record.orientation1, record.orientation2, record.orientation3};
    counts.horizontalAcceleration = record.horizontalAcceleration;
    counts.verticalAcceleration = record.verticalAcceleration;

    // The transport gate in `Core::AAP::Manager` already proved the 0x17 opcode and parsed the
    // five sensor fields, so the queued value event needs no byte-level policy here.
    const auto view = _model.OnCounts(counts, record.generation, HeadTrackingRecord::Stream, Now());
    if (view.has_value()) {
        ApplyView(*view);
    }
}

void HeadTrackingCard::OnDesired(bool desired)
{
    _model.SetEnabled(desired, Now());
    ApplyView(_model.Present(Now()));
}

void HeadTrackingCard::OnSession(bool live, qulonglong generation)
{
    const auto now = Now();
    if (live) {
        _model.OnTransportUp(static_cast<std::uint64_t>(generation), now);
    }
    else {
        _model.OnTransportDown(static_cast<std::uint64_t>(generation), now);
    }
    ApplyView(_model.Present(now));
}

void HeadTrackingCard::OnTick()
{
    const auto view = _model.Tick(Now());
    if (view.has_value()) {
        ApplyView(*view);
    }
}

void HeadTrackingCard::ApplyView(const HeadTrackingView &view)
{
    if (view == _view) {
        return;
    }

    _view = view;
    Publish();
    UpdateIndicator();
}

QString HeadTrackingCard::PhaseText(HeadTrackingPhase phase)
{
    switch (phase) {
    case HeadTrackingPhase::Off:
        return tr("Head tracking is off");
    case HeadTrackingPhase::Armed:
        return tr("Waiting for the earbuds' first reading");
    case HeadTrackingPhase::Streaming:
        return tr("Streaming");
    case HeadTrackingPhase::Stale:
        return tr("No readings for a while - the earbuds paused the stream");
    case HeadTrackingPhase::LinkDown:
        return tr("The AAP connection dropped");
    }
    return {};
}

QString HeadTrackingCard::FormatNormalized(double value, bool available)
{
    if (!available) {
        return tr("-");
    }
    return QString::number(value, 'f', 3);
}

QString HeadTrackingCard::FormatRaw(std::int16_t value, bool available)
{
    if (!available) {
        return tr("-");
    }
    return QString::number(static_cast<int>(value));
}

std::vector<QString> HeadTrackingCard::ValueRows(const HeadTrackingView &view)
{
    const auto available = view.hasSample;
    std::vector<QString> rows;
    rows.reserve(std::size(kFieldNames) * 2);

    for (std::size_t index = 0; index < view.sample.orientation.size(); ++index) {
        rows.push_back(FormatNormalized(view.sample.orientation[index], available));
        rows.push_back(FormatRaw(view.sample.raw.orientation[index], available));
    }
    rows.push_back(FormatNormalized(view.sample.horizontalAcceleration, available));
    rows.push_back(FormatRaw(view.sample.raw.horizontalAcceleration, available));
    rows.push_back(FormatNormalized(view.sample.verticalAcceleration, available));
    rows.push_back(FormatRaw(view.sample.raw.verticalAcceleration, available));
    return rows;
}

void HeadTrackingCard::Retranslate()
{
    _titleLabel->setText(tr("Head tracking"));
    _noteLabel->setText(
        tr("Values are the earbuds' raw sensor counts scaled to signed full range: the protocol "
           "publishes no axis names and no physical unit, so none is claimed here."));
    for (std::size_t index = 0; index < _fieldLabels.size(); ++index) {
        _fieldLabels[index]->setText(
            QCoreApplication::translate("Gui::HeadTrackingCard", kFieldNames[index]));
    }
    for (std::size_t index = 0; index < _columnLabels.size(); ++index) {
        _columnLabels[index]->setText(
            QCoreApplication::translate("Gui::HeadTrackingCard", kColumnHeaders[index]));
    }
    // LanguageChange must republish dynamic strings even when the numeric view is unchanged.
    Publish();
    UpdateIndicator();
}

void HeadTrackingCard::Publish()
{
    _statusLabel->setText(PhaseText(_view.phase));

    QString detail;
    if (_view.generation != 0) {
        detail = tr("Session %1").arg(_view.generation);
    }
    if (_view.sinceLastRecord.count() >= 0) {
        detail =
            detail.isEmpty()
                ? tr("Last reading %1 s ago").arg(_view.sinceLastRecord.count())
                : tr("%1 - last reading %2 s ago").arg(detail).arg(_view.sinceLastRecord.count());
    }
    _detailLabel->setText(detail);

    const auto rows = ValueRows(_view);
    for (std::size_t index = 0; index < _valueLabels.size() && index < rows.size(); ++index) {
        _valueLabels[index]->setText(rows[index]);
    }
}

bool HeadTrackingCard::IsReducedMotion()
{
#if defined(APD_OS_WIN)
    // Qt 6 has no reduced-motion hint on QStyleHints; ask Windows whether window animations
    // are enabled, which is the system switch users flip for "show animations".
    BOOL animationsEnabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animationsEnabled, 0)) {
        return animationsEnabled == FALSE;
    }
#endif
    return false;
}

void HeadTrackingCard::SetReducedMotionOverride(std::optional<bool> reduced)
{
    _reducedMotionOverride = reduced;
    UpdateIndicator();
}

void HeadTrackingCard::UpdateIndicator()
{
    const bool reduced = _reducedMotionOverride.value_or(IsReducedMotion());
    // The pulse is decoration for "live": drop it under reduced motion, and whenever the stream
    // is not actually flowing. The status text says the same thing in words either way.
    _pulse->setVisible(_view.ShouldPulse() && !reduced);
}

void HeadTrackingCard::ApplyTheme()
{
    // The indicator paints itself, so it needs the accent explicitly; the rest of the card is
    // styled by the dialog's stylesheet through the `cssClass` property.
    _pulse->SetColor(Theme::Manager::Instance().Colors().accent);
    style()->unpolish(this);
    style()->polish(this);
}

void HeadTrackingCard::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        Retranslate();
    }
    QFrame::changeEvent(event);
}

void HeadTrackingCard::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);
    // Only poll for the stale transition while the card is actually on screen.
    _tickTimer.start();
}

void HeadTrackingCard::hideEvent(QHideEvent *event)
{
    _tickTimer.stop();
    QFrame::hideEvent(event);
}

} // namespace Gui
