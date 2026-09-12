#pragma once

#include "SampleSettings.h"
#include "Theme.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <functional>

namespace hachi
{
class OtoWaveformEditorComponent final : public juce::Component,
                                         private juce::ScrollBar::Listener
{
public:
    // jieMode adds the three inner boundaries that turn a classic two-region
    // oto entry into the four-region (Jie) layout.  Its classic timing fields
    // are stored in oto.jie.ini and its extra boundaries in oto4.ini; the
    // voicebank's original oto.ini is never modified in Jie mode.
    // mouMode is jieMode plus the per-region classes: the same four
    // boundaries, written to otomou.ini with a class string in front.
    OtoWaveformEditorComponent(VoicebankOtoEntry entry, bool jieMode,
                               bool mouMode,
                               std::function<void()> savedCallback);

    void paint(juce::Graphics& g) override;
    void resized() override;
    // Read-only views for the offline layout check in --smoke-oto-editor.
    juce::String diagnosticParameterText(int index) const
    {
        return juce::isPositiveAndBelow(index, 8)
            ? parameterEditors[static_cast<std::size_t>(index)].getText() : juce::String{};
    }
    // Test seam: dialogs are unreachable without a mouse, so the count
    // is switched and read back the way a click would.
    void diagnosticSetRegionCount(int count) { setRegionCount(count); }
    [[nodiscard]] int diagnosticRegionCount() const { return regionCount(); }
    [[nodiscard]] juce::String diagnosticClasses() const { return edited.mouClasses; }
    void diagnosticTickConsonant(int index, bool consonant)
    {
        setRegionIsConsonant(index, consonant);
    }
    bool diagnosticConsonantTicked(int index) const
    {
        return juce::isPositiveAndBelow(index, 4)
            && consonantButtons[static_cast<std::size_t>(index)].getToggleState();
    }
    bool diagnosticConsonantEnabled(int index) const
    {
        return juce::isPositiveAndBelow(index, 4)
            && consonantButtons[static_cast<std::size_t>(index)].isEnabled();
    }
    juce::String diagnosticRegionName(int index) const;
    // Visible is not the same as present: a component with no parent still
    // reports the flag setVisible left on it, and paints nowhere.
    bool diagnosticParameterAttached(int index) const
    {
        return juce::isPositiveAndBelow(index, 8)
            && parameterEditors[static_cast<std::size_t>(index)]
                   .getParentComponent() == this
            && parameterLabels[static_cast<std::size_t>(index)]
                   .getParentComponent() == this;
    }
    juce::String diagnosticParameterLabel(int index) const
    {
        return juce::isPositiveAndBelow(index, 8)
            ? parameterLabels[static_cast<std::size_t>(index)].getText()
            : juce::String{};
    }
    bool diagnosticParameterShown(int index) const
    {
        return juce::isPositiveAndBelow(index, 8)
            && parameterEditors[static_cast<std::size_t>(index)].isVisible();
    }
    void diagnosticDragOffsetTo(double ms)
    {
        waveform.diagnosticDragOffsetTo(ms);
        refreshEditors();
    }
    void diagnosticDragBoundary(int index, double ms)
    {
        waveform.diagnosticDragBoundary(index, ms);
        refreshEditors();
    }
    juce::String diagnosticHandleAtX(float x) const
    {
        return waveform.diagnosticHandleAtX(x);
    }
    float diagnosticXForBoundary(int index) const
    {
        return waveform.diagnosticXForBoundary(index);
    }
    double diagnosticBoundaryMs(int index) const
    {
        return index == 0 ? edited.jieOnsetMs
             : index == 1 ? edited.jieGlideMs : edited.jieNucleusMs;
    }
    double diagnosticEntrySpanMs() const { return waveform.entrySpanMilliseconds(); }
    // Type into one parameter box and commit it, the way leaving the field does.
    void diagnosticTypeParameter(int index, const juce::String& text)
    {
        if (!juce::isPositiveAndBelow(index, 8)) return;
        parameterEditors[static_cast<std::size_t>(index)].setText(text, false);
        commitEditors();
    }
    // Whether every zoom caption fits the button it is drawn in, and by how
     // much.  JUCE indents button text from both edges before it fits it, so a
     // button can be wide enough for the glyphs and still show an ellipsis.
    bool diagnosticZoomCaptionsFit(juce::String* report = nullptr) const;
    void diagnosticZoomOut() { waveform.nudgeHorizontalZoom(1.0 / 1.4); }
    double diagnosticTotalMs() const { return waveform.totalMilliseconds(); }
    double diagnosticVisibleMs() const { return waveform.visibleLengthMilliseconds(); }
    juce::Rectangle<int> diagnosticScrollBounds() const { return waveformScroll.getBounds(); }
    bool diagnosticScrollEnabled() const { return waveformScroll.isEnabled(); }

private:
    class WaveformView final : public juce::Component
    {
    public:
        WaveformView(VoicebankOtoEntry& entry, bool jieMode, bool mouMode,
                     std::function<void()> changedCallback);

        void paint(juce::Graphics& g) override;
        void mouseMove(const juce::MouseEvent& event) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;
        void mouseUp(const juce::MouseEvent&) override;
        double durationMilliseconds() const { return durationMs; }
        // Zoom is applied as a visible time window and an amplitude scale.
        // Nudging by a factor keeps the current centre, so repeatedly
        // pressing + closes in on whatever is on screen.
        void nudgeHorizontalZoom(double factor);
        void nudgeVerticalZoom(double factor);
        void frameEntry();          // fit the view to this oto entry
        void showWholeFile();       // the opening view: nothing off screen
        // The visible window, for the scroll bar underneath to mirror.
        double totalMilliseconds() const { return durationMs; }
        double visibleStartMilliseconds() const { return viewStartMs(); }
        double visibleLengthMilliseconds() const { return visibleSpanMs(); }
        void setVisibleStartMilliseconds(double startMs);
        std::function<void()> onViewChanged;
        void diagnosticDragOffsetTo(double ms) { applyHandle(Handle::offset, ms); }
        // Drag one region boundary, 0 for the first, to an absolute time.
        void diagnosticDragBoundary(int index, double ms)
        {
            applyHandle(index == 0 ? Handle::jieOnset
                        : index == 1 ? Handle::jieGlide : Handle::jieNucleus, ms);
        }
        // What a press at this x would take hold of.
        juce::String diagnosticHandleAtX(float x) const;
        float diagnosticXForBoundary(int index) const
        {
            const std::array<double, 3> bounds { entry.jieOnsetMs, entry.jieGlideMs,
                                                 entry.jieNucleusMs };
            return xForMilliseconds(entry.offsetMs
                + bounds[static_cast<std::size_t>(juce::jlimit(0, 2, index))]);
        }
        // Length of the region this oto entry covers, offset to cutoff.
        double entrySpanMilliseconds() const { return endMilliseconds() - entry.offsetMs; }

    private:
        enum class Handle { none, offset, consonant, cutoff, preutterance, overlap,
                            jieOnset, jieGlide, jieNucleus };
        void loadWaveform();
        juce::Rectangle<int> plotBounds() const;
        float xForMilliseconds(double milliseconds) const;
        double millisecondsForX(float x) const;
        double endMilliseconds() const;
        // Moves one boundary to an absolute time in the file.  Split out
        // of the drag so the offline editor check can exercise it.
        void applyHandle(Handle handle, double value);
        // How many boundaries this entry has: one fewer than its regions.
        int boundaryCount() const { return juce::jlimit(2, 4, regions) - 1; }
        // What bounds one boundary from above -- the next boundary that
        // exists, or the end of the entry for the last one.  The boundaries
        // past the count are not walls: they are values the file is keeping
        // for a different split, and a two-region entry's one boundary has to
        // be able to reach the end.
        double boundaryCeiling(int index) const;
        // Ordered, whatever the count.  A boundary the count does not reach
        // follows the last visible one rather than being crossed by it.
        void orderBoundaries();
        Handle handleNear(const juce::Point<float>& position) const;
        void updateCursor(const juce::Point<float>& position);

        VoicebankOtoEntry& entry;
        const bool jie;
        // 谋 names the regions by number rather than by the part of a Chinese
        // syllable they usually are: any of them can be a consonant here, so
        // 韵尾 would be saying something the entry may well contradict.
        const bool mou;
    public:
        // How many regions this entry has, and what each of them is.  Four
        // and empty unless a 谋 class string says otherwise: the view then
        // draws that many bands and one boundary fewer, rather than three
        // lines two of which nothing can move.
        int regions = 4;
        juce::String classes;
    private:
        double viewZoom = 1.0;      // 1 = whole file visible
        double viewCentreMs = 0.0;
        double amplitudeZoom = 1.0;
        double panStartCentreMs = 0.0;
        bool panning = false;
        double visibleSpanMs() const;
        double viewStartMs() const;
        void clampView();
        std::function<void()> changed;
        juce::AudioFormatManager formats;
        std::vector<std::pair<float, float>> waveformPeaks;
        double durationMs = 0.0;
        Handle dragging = Handle::none;
    };

    // Which parameters this mode shows, in the order they are laid out.  Jie
    // mode drops the classic consonant: the end of the onset region is the
    // same boundary, so showing both put two draggable lines on one position.
    std::vector<std::size_t> visibleParameters() const
    {
        if (!jie) return { 0, 1, 2, 3, 4 };
        // One boundary editor fewer per region fewer: three regions have two
        // inner boundaries, two regions have one.
        std::vector<std::size_t> shown { 0, 2, 3, 4 };
        for (int index = 0; index + 1 < regionCount(); ++index)
            shown.push_back(static_cast<std::size_t>(5 + index));
        return shown;
    }
    // The count lives in the class string; without one it is four, which is
    // what an oto4 entry has always had.
    [[nodiscard]] int regionCount() const;
    // Switch this entry to a different count: the letters that survive are
    // kept, and the boundaries that no longer exist collapse onto the end.
    void setRegionCount(int count);
    // Mark one region a consonant, or stop.  Unmarking writes a vowel unless
    // the region is a silence, which the tick boxes have no way to say and so
    // leave alone -- the class field is where an S is typed.
    void setRegionIsConsonant(int index, bool consonant);
    [[nodiscard]] bool regionIsConsonant(int index) const;
    // Push the count and the classes into the view and the editor row.
    void syncRegionCount();
    // Which parameter boxes are on screen, from visibleParameters().
    void applyParameterVisibility();
    void refreshEditors();
    void commitEditors();
    void save();
    void closeWindow();
    static juce::String formatNumber(double value);

    const bool jie;
    const bool mou;
    VoicebankOtoEntry original;
    VoicebankOtoEntry edited;
    std::function<void()> onSaved;
    juce::Label fileLabel;
    juce::Label helpLabel;
    // The class string, shown only in 谋 mode: one letter per region, and its
    // length is the region count.
    juce::Label classesLabel;
    juce::TextEditor classesEditor;
    // Two / three / four regions, down the right of the waveform.  One of
    // them is always on, and picking one rewrites the class string.
    std::array<juce::TextButton, 3> countButtons;
    // One tick per region, ticked where that region is a consonant.  Any
    // number of them may be ticked, none included.
    juce::Label consonantLabel;
    std::array<juce::ToggleButton, 4> consonantButtons;
    // Five classic oto values, then the three Jie boundaries.  The last
    // three are only created and laid out when jie is true.
    std::array<juce::Label, 8> parameterLabels;
    std::array<juce::TextEditor, 8> parameterEditors;
    WaveformView waveform;
    juce::TextButton saveButton;
    juce::TextButton cancelButton;
    // Small +/- pair per axis, down the right-hand side of the waveform.
    juce::TextButton hZoomInButton, hZoomOutButton;
    juce::TextButton vZoomInButton, vZoomOutButton;
    juce::TextButton zoomResetButton;
    // Zooming in can leave the entry off screen; this is how it is reached
    // without having to drag the waveform itself.
    juce::ScrollBar waveformScroll { false };
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;
    void refreshScrollBar();
    bool refreshingEditors = false;
};
}
