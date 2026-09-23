#pragma once

#include "ProjectModel.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <memory>
#include <unordered_map>

namespace hachi
{
class TimelineComponent final : public juce::Component,
                                private juce::ChangeListener,
                                private juce::Timer
{
public:
    explicit TimelineComponent(ProjectModel& modelToUse);
    ~TimelineComponent() override;

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void setPixelsPerSecond(float value);
    void setRowHeight(float value);
    void setPlayheadSeconds(double seconds);
    [[nodiscard]] int pixelForSeconds(double seconds) const;
    [[nodiscard]] double secondsForPixel(int pixel) const;
    [[nodiscard]] juce::String trackIdForPixel(int pixel) const;
    // The lane and the moment under the pointer, or nothing when the pointer
    // is not over the lanes.  A copied piece of material lands here, which is
    // the only way to say "that track, there" -- clicking an empty lane
    // reports no clip and does not name the track it belongs to, so the
    // selection alone could never carry a paste onto another track.
    struct Anchor { juce::String trackId; double seconds = 0.0; };
    [[nodiscard]] std::optional<Anchor> pointerAnchor() const;
    // The lane geometry, so a check can aim at a lane without guessing it.
    [[nodiscard]] int diagnosticRulerHeight() const { return rulerHeight; }
    [[nodiscard]] int diagnosticRowHeight() const { return rowHeight; }
    // Takes the model's current state, as a change message would.
    void diagnosticRefresh() { snapshot = model.snapshot(); }
    std::function<void(double)> onSeek;
    // A right-click on empty lane space, in screen coordinates.
    std::function<void(juce::Point<int>)> onEmptyAreaMenu;
    std::function<void(const juce::String&)> onClipSelected;
    std::function<void(const juce::String&)> onClipGainRequested;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuild();
    [[nodiscard]] float timeToX(double seconds) const;
    [[nodiscard]] double gridQuarterNotes() const;
    [[nodiscard]] double snapToGrid(double seconds) const;
    void showTempoMenu(double quarterPosition, juce::Point<int> screenPosition);
    void showTempoDialog(double quarterPosition);

    struct ClipHit
    {
        juce::String id;
        juce::Rectangle<float> bounds;
        double startSeconds = 0.0;
        double durationSeconds = 0.0;
        double fadeInSeconds = 0.0;
        double fadeOutSeconds = 0.0;
        bool muted = false;
    };

    ProjectModel& model;
    ProjectData snapshot;
    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache thumbnailCache { 96 };
    std::unordered_map<std::string, std::unique_ptr<juce::AudioThumbnail>> thumbnails;
    std::vector<ClipHit> clipHits;
    float pixelsPerSecond = 140.0f;
    static constexpr int rulerHeight = 24;
    int rowHeight = 96;
    std::optional<Anchor> hoverAnchor;
    void rememberPointer(const juce::MouseEvent& event);
    double playheadSeconds = 0.0;
    juce::String selectedClip;
    juce::String draggedClip;
    double draggedClipStart = 0.0;
    double draggedClipDuration = 0.0;
    double draggedClipPreviewStart = 0.0;
    double draggedClipPreviewDuration = 0.0;
    double draggedClipFadeIn = 0.0;
    double draggedClipFadeOut = 0.0;
    double draggedClipPreviewFadeIn = 0.0;
    double draggedClipPreviewFadeOut = 0.0;
    float dragAnchorX = 0.0f;
    enum class DragMode { none, move, resizeLeft, resizeRight, fadeIn, fadeOut }
        dragMode = DragMode::none;
};
}
