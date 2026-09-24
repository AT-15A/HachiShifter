#include "TimelineComponent.h"
#include "Theme.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace hachi
{
namespace
{
const std::array<juce::Colour, 6> trackColours {
    juce::Colour(0xff7f69ca), juce::Colour(0xffcbcbfa), juce::Colour(0xfff4c000),
    juce::Colour(0xff9b8bdd), juce::Colour(0xffdedcff), juce::Colour(0xffffd94f)
};
}

TimelineComponent::TimelineComponent(ProjectModel& modelToUse) : model(modelToUse)
{
    formats.registerBasicFormats();
    model.addChangeListener(this);
    rebuild();
    startTimerHz(12);
}

TimelineComponent::~TimelineComponent()
{
    stopTimer();
    model.removeChangeListener(this);
}

float TimelineComponent::timeToX(double seconds) const
{
    return static_cast<float>(seconds) * pixelsPerSecond;
}

int TimelineComponent::pixelForSeconds(double seconds) const
{
    return static_cast<int>(std::round(timeToX(seconds)));
}

double TimelineComponent::secondsForPixel(int pixel) const
{
    return std::max(0.0, static_cast<double>(pixel) / pixelsPerSecond);
}

double TimelineComponent::gridQuarterNotes() const
{
    auto text = snapshot.gridDivision.trim().toLowerCase();
    auto dotted = text.endsWithChar('.');
    auto triplet = text.endsWithChar('t');
    if (dotted || triplet) text = text.dropLastCharacters(1);
    const auto slash = text.indexOfChar('/');
    const auto denominator = slash >= 0 ? text.substring(slash + 1).getIntValue() : 16;
    auto quarters = 4.0 / static_cast<double>(std::max(1, denominator));
    if (dotted) quarters *= 1.5;
    if (triplet) quarters *= 2.0 / 3.0;
    return std::max(1.0 / 256.0, quarters);
}

double TimelineComponent::snapToGrid(double seconds) const
{
    const auto step = gridQuarterNotes();
    const auto quarter = snapshot.quarterPositionForSeconds(seconds);
    return std::max(0.0, snapshot.secondsForQuarterPosition(
        std::round(quarter / step) * step));
}

juce::String TimelineComponent::trackIdForPixel(int pixel) const
{
    if (pixel < rulerHeight) return {};
    const auto index = (pixel - rulerHeight) / rowHeight;
    return index >= 0 && index < static_cast<int>(snapshot.tracks.size())
        ? snapshot.tracks[static_cast<std::size_t>(index)].id : juce::String{};
}

void TimelineComponent::setPixelsPerSecond(float value)
{
    // Matches the roll: the two are driven by one slider, and letting them
    // clamp differently put them on different scales past 600, after which
    // nothing that translated between them could be right.
    pixelsPerSecond = juce::jlimit(40.0f, 8000.0f, value);
    rebuild();
}

void TimelineComponent::setRowHeight(float value)
{
    rowHeight = juce::jlimit(40, 220, static_cast<int>(std::round(value)));
    rebuild();
}

void TimelineComponent::setPlayheadSeconds(double seconds)
{
    // Same as the roll: a one pixel line does not need the whole arrangement
    // repainted thirty times a second, and the band has to cover where the
    // line was as well as where it is going.
    if (std::abs(seconds - playheadSeconds) < 1.0e-9) return;
    const auto before = timeToX(playheadSeconds);
    playheadSeconds = seconds;
    const auto after = timeToX(playheadSeconds);
    const auto left = static_cast<int>(std::floor(std::min(before, after))) - 3;
    const auto right = static_cast<int>(std::ceil(std::max(before, after))) + 3;
    repaint(left, 0, std::max(1, right - left), getHeight());
}

void TimelineComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuild();
}

void TimelineComponent::timerCallback()
{
    repaint();
}

void TimelineComponent::rebuild()
{
    snapshot = model.snapshot();
    const auto selectedStillExists = std::any_of(snapshot.tracks.begin(), snapshot.tracks.end(), [this](const auto& track)
    {
        return std::any_of(track.clips.begin(), track.clips.end(), [this](const auto& clip)
        {
            return clip.id == selectedClip;
        });
    });
    if (!selectedStillExists) selectedClip.clear();
    std::unordered_map<std::string, std::unique_ptr<juce::AudioThumbnail>> next;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
        {
            const auto key = clip.sourceFile.getFullPathName().toStdString();
            if (next.contains(key)) continue;
            if (const auto found = thumbnails.find(key); found != thumbnails.end())
            {
                next.emplace(key, std::move(found->second));
                continue;
            }
            auto thumbnail = std::make_unique<juce::AudioThumbnail>(256, formats, thumbnailCache);
            if (clip.sourceFile.existsAsFile())
                thumbnail->setSource(new juce::FileInputSource(clip.sourceFile));
            next.emplace(key, std::move(thumbnail));
        }
    thumbnails = std::move(next);
    setSize(static_cast<int>(juce::jlimit(470.0, 3.2e7,
        static_cast<double>(timeToX(snapshot.durationSeconds())) + 400.0)),
            rulerHeight + std::max(rowHeight, static_cast<int>(snapshot.tracks.size()) * rowHeight));
    repaint();
}

void TimelineComponent::paint(juce::Graphics& g)
{
    g.fillAll(Palette::base);
    clipHits.clear();
    g.setColour(Palette::background);
    g.fillRect(0, 0, getWidth(), rulerHeight);
    g.setColour(Palette::border);
    g.drawHorizontalLine(rulerHeight - 1, 0.0f, static_cast<float>(getWidth()));
    const auto gridStep = gridQuarterNotes();
    const auto firstQuarter = snapshot.quarterPositionForSeconds(0.0);
    const auto firstTick = static_cast<int>(std::floor(firstQuarter / gridStep)) - 1;
    const auto barQuarters = static_cast<double>(std::max(1, snapshot.numerator))
        * 4.0 / static_cast<double>(std::max(1, snapshot.denominator));
    for (int tick = firstTick;; ++tick)
    {
        const auto quarter = static_cast<double>(tick) * gridStep;
        const auto seconds = snapshot.secondsForQuarterPosition(quarter);
        const auto x = timeToX(seconds);
        if (x > static_cast<float>(getWidth())) break;
        if (x < 0.0f) continue;
        const auto isBeat = std::abs(quarter - std::round(quarter)) < 1.0e-6;
        const auto bar = quarter / barQuarters;
        const auto isBar = std::abs(bar - std::round(bar)) < 1.0e-6;
        g.setColour(isBar ? Palette::grid.brighter(0.28f)
                   : isBeat ? Palette::grid.withAlpha(0.62f) : Palette::grid.withAlpha(0.30f));
        g.drawVerticalLine(static_cast<int>(x), 0.0f, static_cast<float>(getHeight()));
        if (isBar)
        {
            g.setColour(Palette::textMuted);
            g.setFont(10.0f);
            g.drawText(juce::String(static_cast<int>(std::llround(bar)) + 1) + ".1",
                       static_cast<int>(x) + 4, 3, 42, 16, juce::Justification::left);
        }
    }

    for (const auto& change : snapshot.tempoChanges)
    {
        const auto x = timeToX(snapshot.secondsForQuarterPosition(change.quarterPosition));
        if (x < 0.0f || x > getWidth()) continue;
        g.setColour(juce::Colour(0xffffa94d));
        g.drawVerticalLine(static_cast<int>(x), 0.0f, static_cast<float>(getHeight()));
        g.setFont(9.5f);
        g.drawText(juce::String(change.bpm, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".")
                + " BPM", static_cast<int>(x) + 4, 3, 66, 16,
            juce::Justification::centredLeft, false);
    }

    for (std::size_t trackIndex = 0; trackIndex < snapshot.tracks.size(); ++trackIndex)
    {
        const auto& track = snapshot.tracks[trackIndex];
        const auto row = juce::Rectangle<int>(0, rulerHeight + static_cast<int>(trackIndex) * rowHeight,
                                               getWidth(), rowHeight);
        g.setColour(Palette::grid);
        g.drawHorizontalLine(row.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
        const auto colour = trackColours[trackIndex % trackColours.size()];
        for (const auto& clip : track.clips)
        {
            const auto displayStart = clip.id == draggedClip ? draggedClipPreviewStart
                                                              : clip.startSeconds;
            const auto displayDuration = clip.id == draggedClip ? draggedClipPreviewDuration
                                                                 : clip.durationSeconds;
            const auto displayRatio = clip.durationSeconds > 1.0e-9
                ? displayDuration / clip.durationSeconds : 1.0;
            auto displayFadeIn = clip.fadeInSeconds * displayRatio;
            auto displayFadeOut = clip.fadeOutSeconds * displayRatio;
            if (clip.id == draggedClip
                && (dragMode == DragMode::fadeIn || dragMode == DragMode::fadeOut))
            {
                displayFadeIn = draggedClipPreviewFadeIn;
                displayFadeOut = draggedClipPreviewFadeOut;
            }
            auto bounds = juce::Rectangle<float>(timeToX(displayStart),
                                                  static_cast<float>(row.getY() + 19),
                                                  std::max(4.0f, static_cast<float>(displayDuration)
                                                                      * pixelsPerSecond),
                                                  static_cast<float>(rowHeight - 25));
            clipHits.push_back({ clip.id, bounds, clip.startSeconds, clip.durationSeconds,
                                 clip.fadeInSeconds, clip.fadeOutSeconds, clip.muted });
            g.setColour(track.muted || clip.muted ? Palette::clipBackground.withAlpha(0.55f)
                                                  : Palette::clipBackground);
            g.fillRect(bounds);
            g.setColour(colour.withAlpha(0.82f));
            g.drawRect(bounds, 1.0f);
            if (clip.id == selectedClip)
            {
                g.setColour(Palette::text.withAlpha(0.92f));
                g.drawRect(bounds.reduced(1.0f), 2.0f);
            }

            if (const auto found = thumbnails.find(clip.sourceFile.getFullPathName().toStdString()); found != thumbnails.end())
            {
                g.setColour(colour.brighter(0.65f).withAlpha(
                    track.muted || clip.muted ? 0.25f : 0.78f));
                // Keep the editor visually consistent for mono and stereo
                // sources.  Playback still uses every source channel; only the
                // compact waveform lane displays channel 1.
                found->second->drawChannel(
                    g, bounds.withTrimmedTop(17.0f).reduced(1.0f).toNearestInt(),
                    clip.sourceOffsetSeconds,
                    clip.sourceOffsetSeconds
                        + (clip.sourceDurationSeconds > 1.0e-9
                            ? clip.sourceDurationSeconds : clip.durationSeconds),
                    0, 1.0f);
            }
            const auto waveformTop = bounds.getY() + 19.0f;
            const auto fadeInX = bounds.getX() + timeToX(displayFadeIn);
            const auto fadeOutX = bounds.getRight() - timeToX(displayFadeOut);
            if (displayFadeIn > 0.0)
            {
                g.setColour(Palette::text.withAlpha(0.7f));
                g.drawLine(bounds.getX(), bounds.getBottom(),
                           fadeInX, waveformTop, 1.0f);
            }
            if (displayFadeOut > 0.0)
            {
                g.setColour(Palette::text.withAlpha(0.7f));
                g.drawLine(fadeOutX, waveformTop,
                           bounds.getRight(), bounds.getBottom(), 1.0f);
            }
            g.setColour(colour.brighter(0.72f).withAlpha(
                clip.id == selectedClip ? 0.95f : 0.52f));
            g.fillEllipse(fadeInX - 3.5f, waveformTop - 3.5f, 7.0f, 7.0f);
            g.fillEllipse(fadeOutX - 3.5f, waveformTop - 3.5f, 7.0f, 7.0f);
            g.setColour(Palette::text);
            g.setColour(Palette::panel.withAlpha(0.82f));
            g.fillRect(bounds.toNearestInt().withHeight(17));
            g.setColour(clip.muted ? Palette::noteFill : colour.brighter(0.5f));
            g.fillRoundedRectangle(bounds.getX() + 2.0f, bounds.getY() + 2.0f, 14.0f, 13.0f, 2.0f);
            g.setColour(Palette::panel);
            g.setFont(9.0f);
            g.drawText("M", static_cast<int>(bounds.getX() + 2.0f), static_cast<int>(bounds.getY() + 1.0f),
                       14, 14, juce::Justification::centred);
            g.setColour(Palette::text);
            g.setFont(10.0f);
            const auto gainDb = clip.gain <= 0.0001f ? juce::String("-inf")
                : juce::String(20.0f * std::log10(clip.gain), 1);
            g.drawText(clip.sourceFile.getFileNameWithoutExtension() + "  " + gainDb + " dB",
                       bounds.toNearestInt().withTrimmedLeft(19).withHeight(17),
                       juce::Justification::centredLeft, true);
            g.setColour(colour.brighter(0.65f));
            juce::Path leftHandle;
            leftHandle.addTriangle(bounds.getX(), bounds.getY(), bounds.getX() + 7.0f, bounds.getY(),
                                   bounds.getX(), bounds.getY() + 7.0f);
            g.fillPath(leftHandle);
            juce::Path rightHandle;
            rightHandle.addTriangle(bounds.getRight(), bounds.getY(), bounds.getRight() - 7.0f, bounds.getY(),
                                    bounds.getRight(), bounds.getY() + 7.0f);
            g.fillPath(rightHandle);
        }
    }

    g.setColour(Palette::playhead);
    g.drawVerticalLine(static_cast<int>(timeToX(playheadSeconds)), 0.0f, static_cast<float>(getHeight()));
}

std::optional<TimelineComponent::Anchor> TimelineComponent::pointerAnchor() const
{
    return hoverAnchor;
}

void TimelineComponent::rememberPointer(const juce::MouseEvent& event)
{
    const auto trackId = trackIdForPixel(event.y);
    if (trackId.isEmpty())
    {
        hoverAnchor.reset();
        return;
    }
    hoverAnchor = Anchor { trackId, std::max(0.0,
        static_cast<double>(event.position.x) / pixelsPerSecond) };
}

void TimelineComponent::mouseMove(const juce::MouseEvent& event)
{
    rememberPointer(event);
    for (auto it = clipHits.rbegin(); it != clipHits.rend(); ++it)
        if (it->bounds.contains(event.position))
        {
            const auto muteBounds = juce::Rectangle<float>(it->bounds.getX() + 2.0f,
                it->bounds.getY() + 2.0f, 14.0f, 13.0f);
            if (muteBounds.contains(event.position))
            {
                setMouseCursor(juce::MouseCursor::PointingHandCursor);
                return;
            }
            const auto waveformTop = it->bounds.getY() + 19.0f;
            const auto fadeIn = juce::Point<float>(it->bounds.getX()
                + timeToX(it->fadeInSeconds), waveformTop);
            const auto fadeOut = juce::Point<float>(it->bounds.getRight()
                - timeToX(it->fadeOutSeconds), waveformTop);
            if (event.position.getDistanceFrom(fadeIn) <= 8.0f
                || event.position.getDistanceFrom(fadeOut) <= 8.0f)
            {
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
            const auto onHandle = event.position.x <= it->bounds.getX() + 8.0f
                || event.position.x >= it->bounds.getRight() - 8.0f;
            setMouseCursor(onHandle ? juce::MouseCursor::LeftRightResizeCursor
                                    : juce::MouseCursor::NormalCursor);
            return;
        }
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TimelineComponent::mouseExit(const juce::MouseEvent&)
{
    // Off the lanes there is no track to paste onto.
    hoverAnchor.reset();
    if (draggedClip.isEmpty()) setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TimelineComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    for (auto it = clipHits.rbegin(); it != clipHits.rend(); ++it)
        if (it->bounds.contains(event.position))
        {
            const auto muteBounds = juce::Rectangle<float>(it->bounds.getX() + 2.0f,
                it->bounds.getY() + 2.0f, 14.0f, 13.0f);
            if (muteBounds.contains(event.position)) return;
            draggedClip.clear();
            dragMode = DragMode::none;
            selectedClip = it->id;
            if (onClipSelected) onClipSelected(selectedClip);
            if (onClipGainRequested) onClipGainRequested(selectedClip);
            repaint();
            return;
    }
}

void TimelineComponent::showTempoMenu(double quarterPosition,
                                      juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    menu.addItem(1, juce::String::fromUTF8("改变曲速…"));
    juce::Component::SafePointer<TimelineComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
        juce::Rectangle<int>(screenPosition.x, screenPosition.y, 1, 1)),
        [safe, quarterPosition](int result)
        {
            if (safe == nullptr || result != 1) return;
            juce::MessageManager::callAsync([safe, quarterPosition]
            {
                if (safe != nullptr) safe->showTempoDialog(quarterPosition);
            });
        });
}

void TimelineComponent::showTempoDialog(double quarterPosition)
{
    const auto initialTempo = snapshot.tempoAtQuarterPosition(quarterPosition);
    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("改变曲速"),
        juce::String::fromUTF8("从当前四分之一小节开始使用新的曲速。\n"
            "同步修改音符：按曲速调整音符位置和长度。\n"
            "不修改音符：保留音符位置和长度，只修改曲速。"),
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("bpm", juce::String(initialTempo, 2),
                          juce::String::fromUTF8("BPM（20–400）"));
    dialog->addComboBox("noteTiming", { juce::String::fromUTF8("同步修改音符"),
        juce::String::fromUTF8("不修改音符") }, juce::String::fromUTF8("音符处理"));
    dialog->getComboBoxComponent("noteTiming")->setSelectedItemIndex(0,
        juce::dontSendNotification);
    dialog->addButton(juce::String::fromUTF8("确定"), 1);
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TimelineComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, quarterPosition](int result)
            {
                if (safe != nullptr && result == 1)
                {
                    const auto bpm = dialog->getTextEditorContents("bpm").getDoubleValue();
                    if (bpm >= 20.0 && bpm <= 400.0)
                        safe->model.setTempoChange(quarterPosition, bpm,
                            dialog->getComboBoxComponent("noteTiming")->getSelectedItemIndex() == 0);
                }
                delete dialog;
            }), false);
}

void TimelineComponent::mouseDown(const juce::MouseEvent& event)
{
    rememberPointer(event);
    if (event.y < rulerHeight && event.mods.isPopupMenu())
    {
        const auto seconds = std::max(0.0,
            static_cast<double>(event.position.x) / pixelsPerSecond);
        const auto barQuarters = static_cast<double>(std::max(1, snapshot.numerator))
            * 4.0 / static_cast<double>(std::max(1, snapshot.denominator));
        const auto quarterBar = std::max(1.0 / 256.0, barQuarters / 4.0);
        const auto clickedQuarter = snapshot.quarterPositionForSeconds(seconds);
        const auto snappedQuarter = std::max(0.0,
            std::floor((clickedQuarter + 1.0e-9) / quarterBar) * quarterBar);
        showTempoMenu(snappedQuarter, event.getScreenPosition());
        return;
    }
    for (auto it = clipHits.rbegin(); it != clipHits.rend(); ++it)
        if (it->bounds.contains(event.position))
        {
            selectedClip = it->id;
            if (onClipSelected) onClipSelected(selectedClip);
            const auto muteBounds = juce::Rectangle<float>(it->bounds.getX() + 2.0f,
                it->bounds.getY() + 2.0f, 14.0f, 13.0f);
            if (muteBounds.contains(event.position))
            {
                model.setClipMuted(it->id, !it->muted);
                return;
            }
            draggedClip = it->id;
            draggedClipStart = it->startSeconds;
            draggedClipDuration = it->durationSeconds;
            draggedClipPreviewStart = draggedClipStart;
            draggedClipPreviewDuration = draggedClipDuration;
            draggedClipFadeIn = it->fadeInSeconds;
            draggedClipFadeOut = it->fadeOutSeconds;
            draggedClipPreviewFadeIn = draggedClipFadeIn;
            draggedClipPreviewFadeOut = draggedClipFadeOut;
            dragAnchorX = event.position.x;
            const auto waveformTop = it->bounds.getY() + 19.0f;
            const auto fadeIn = juce::Point<float>(it->bounds.getX()
                + timeToX(it->fadeInSeconds), waveformTop);
            const auto fadeOut = juce::Point<float>(it->bounds.getRight()
                - timeToX(it->fadeOutSeconds), waveformTop);
            if (event.position.getDistanceFrom(fadeIn) <= 8.0f)
            {
                dragMode = DragMode::fadeIn;
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            }
            else if (event.position.getDistanceFrom(fadeOut) <= 8.0f)
            {
                dragMode = DragMode::fadeOut;
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            }
            else if (event.position.x <= it->bounds.getX() + 8.0f)
            {
                dragMode = DragMode::resizeLeft;
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            }
            else if (event.position.x >= it->bounds.getRight() - 8.0f)
            {
                dragMode = DragMode::resizeRight;
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            }
            else
            {
                dragMode = DragMode::move;
                setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            }
            repaint();
            return;
        }
    selectedClip.clear();
    if (onClipSelected) onClipSelected({});
    repaint();
    if (event.mods.isPopupMenu())
    {
        // Below the ruler and on no clip: the space around the tracks.  It
        // used to move the playhead, which no other right-click in the app
        // does, and which left no way to act on the lane itself.
        if (onEmptyAreaMenu) onEmptyAreaMenu(event.getScreenPosition());
        return;
    }
    if (onSeek) onSeek(std::max(0.0, static_cast<double>(event.position.x) / pixelsPerSecond));
}

void TimelineComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (draggedClip.isEmpty()) return;
    const auto snap = [&](double seconds) { return snapToGrid(seconds); };
    const auto delta = static_cast<double>(event.position.x - dragAnchorX) / pixelsPerSecond;
    if (dragMode == DragMode::fadeIn)
        draggedClipPreviewFadeIn = juce::jlimit(0.0, draggedClipDuration,
            static_cast<double>(event.position.x) / pixelsPerSecond - draggedClipStart);
    else if (dragMode == DragMode::fadeOut)
        draggedClipPreviewFadeOut = juce::jlimit(0.0, draggedClipDuration,
            draggedClipStart + draggedClipDuration
                - static_cast<double>(event.position.x) / pixelsPerSecond);
    else if (dragMode == DragMode::resizeLeft)
    {
        const auto end = draggedClipStart + draggedClipDuration;
        draggedClipPreviewStart = juce::jlimit(0.0, end - 0.01,
                                                snap(draggedClipStart + delta));
        draggedClipPreviewDuration = end - draggedClipPreviewStart;
    }
    else if (dragMode == DragMode::resizeRight)
    {
        const auto end = std::max(draggedClipStart + 0.01,
                                  snap(draggedClipStart + draggedClipDuration + delta));
        draggedClipPreviewDuration = end - draggedClipStart;
    }
    else
        draggedClipPreviewStart = snap(draggedClipStart + delta);
    repaint();
}

void TimelineComponent::mouseUp(const juce::MouseEvent&)
{
    if (draggedClip.isNotEmpty())
    {
        if (dragMode == DragMode::fadeIn || dragMode == DragMode::fadeOut)
            model.setClipFades(draggedClip, draggedClipPreviewFadeIn,
                               draggedClipPreviewFadeOut);
        else if (dragMode == DragMode::resizeLeft || dragMode == DragMode::resizeRight)
            model.resizeClip(draggedClip, draggedClipPreviewStart,
                             draggedClipPreviewDuration);
        else
            model.moveClip(draggedClip, draggedClipPreviewStart);
    }
    draggedClip.clear();
    dragMode = DragMode::none;
    setMouseCursor(juce::MouseCursor::NormalCursor);
}
}
