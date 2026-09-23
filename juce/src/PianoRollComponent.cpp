#include "PianoRollComponent.h"
#include "Theme.h"
#include "backend/UtauRenderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace hachi
{
namespace
{
// Two pitch points only have to stay in order.  A millisecond is finer than
// any frame the engine resamples at, so this never stands between the user and
// the shape they are drawing -- including a deliberate near-vertical jump.
constexpr double minimumAnchorSeparation = 0.001;

// The seven numbers of a vibrato, in the order UTAU writes them, so a preset
// copied from there reads the same here.
constexpr std::array<const char*, 7> vibratoFieldKeys {
    "length", "cycle", "depth", "fadein", "fadeout", "phase", "offset"
};

}

juce::StringArray PianoRollComponent::vibratoBuiltInPresets()
{
    // name, length %, cycle ms, depth cents, fade in %, fade out %, phase %,
    // height % -- the order UTAU writes them, so a preset copied from there
    // reads the same here.  The first three are the ones UTAU ships with.
    return { juce::String::fromUTF8("默认,65,180,35,20,20,0,0"),
             juce::String::fromUTF8("强,65,210,55,20,20,0,0"),
             juce::String::fromUTF8("弱,65,165,20,20,20,0,0"),
             juce::String::fromUTF8("自定义预设1,77,153,35,20,7,153,13.7") };
}

juce::StringArray PianoRollComponent::vibratoPresetValues(const juce::String& line)
{
    auto numbers = juce::StringArray::fromTokens(line, ",", "");
    if (numbers.size() < 2) return {};
    numbers.remove(0);   // the name
    for (auto& value : numbers) value = value.trim();
    return numbers;
}

juce::String PianoRollComponent::vibratoPresetsWith(const juce::String& saved,
                                                    const juce::String& name,
                                                    const juce::StringArray& values)
{
    auto lines = juce::StringArray::fromLines(saved);
    lines.removeEmptyStrings();
    for (auto index = lines.size(); --index >= 0;)
        if (lines[index].upToFirstOccurrenceOf(",", false, false).trim() == name)
            lines.remove(index);
    lines.add(name + "," + values.joinIntoString(","));
    return lines.joinIntoString("\n");
}

namespace
{
// Presets for the vibrato dialog: the three UTAU ships with, then whatever the
// user has saved.  An entry is its name and its numbers on one line, which is
// both what is stored and what the list shows -- so a preset can be read at a
// glance without opening it.
class VibratoPresetBar final : public juce::Component
{
public:
    VibratoPresetBar(juce::AlertWindow& owner, juce::String saved,
                     std::function<void(const juce::String&)> persist)
        : dialog(owner), stored(std::move(saved)), save(std::move(persist))
    {
        chooser.setEditableText(true);
        chooser.setTextWhenNothingSelected(
            juce::String::fromUTF8("选择预设，或在此输入名称后保存"));
        chooser.onChange = [this] { applyChosen(); };
        addAndMakeVisible(chooser);
        keep.setButtonText(juce::String::fromUTF8("存为预设"));
        keep.setTooltip(juce::String::fromUTF8(
            "把上面的数值存成预设；名称取自左边的输入框"));
        keep.onClick = [this] { keepCurrent(); };
        addAndMakeVisible(keep);
        remove.setButtonText(juce::String::fromUTF8("删除"));
        remove.setTooltip(juce::String::fromUTF8("删除选中的自定义预设"));
        remove.onClick = [this] { removeChosen(); };
        addAndMakeVisible(remove);
        rebuild();
        setSize(460, 26);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        remove.setBounds(area.removeFromRight(56));
        area.removeFromRight(4);
        keep.setBounds(area.removeFromRight(84));
        area.removeFromRight(6);
        chooser.setBounds(area);
    }

private:
    juce::StringArray customs() const
    {
        auto lines = juce::StringArray::fromLines(stored);
        lines.removeEmptyStrings();
        return lines;
    }

    void rebuild()
    {
        chooser.clear(juce::dontSendNotification);
        auto id = 1;
        for (const auto& entry : PianoRollComponent::vibratoBuiltInPresets())
            chooser.addItem(entry, id++);
        const auto saved = customs();
        if (!saved.isEmpty()) chooser.addSeparator();
        firstCustomId = id;
        for (const auto& entry : saved) chooser.addItem(entry, id++);
    }

    void applyChosen()
    {
        const auto index = chooser.getSelectedItemIndex();
        if (index < 0) return;
        const auto numbers = PianoRollComponent::vibratoPresetValues(
            chooser.getItemText(index));
        if (numbers.isEmpty()) return;
        for (std::size_t field = 0; field < vibratoFieldKeys.size(); ++field)
            if (field < static_cast<std::size_t>(numbers.size()))
                if (auto* editor = dialog.getTextEditor(vibratoFieldKeys[field]))
                    editor->setText(numbers[static_cast<int>(field)].trim(), false);
    }

    void keepCurrent()
    {
        auto name = chooser.getText().trim();
        // A name that is really a preset line, because the box was left showing
        // the one that was chosen: take the part before the first comma.
        if (name.contains(",")) name = name.upToFirstOccurrenceOf(",", false, false).trim();
        if (name.isEmpty())
            name = juce::String::fromUTF8("自定义 ") + juce::String(customs().size() + 1);
        juce::StringArray values;
        for (const auto* key : vibratoFieldKeys)
            values.add(dialog.getTextEditorContents(key).trim());
        stored = PianoRollComponent::vibratoPresetsWith(stored, name, values);
        if (save) save(stored);
        rebuild();
        chooser.setText(name, juce::dontSendNotification);
    }

    void removeChosen()
    {
        const auto index = chooser.getSelectedItemIndex();
        if (index < 0) return;
        const auto id = chooser.getSelectedId();
        if (id < firstCustomId) return;   // the built-in three stay
        auto lines = customs();
        const auto position = id - firstCustomId;
        if (position < 0 || position >= lines.size()) return;
        lines.remove(position);
        stored = lines.joinIntoString("\n");
        if (save) save(stored);
        rebuild();
    }

    juce::AlertWindow& dialog;
    juce::String stored;
    std::function<void(const juce::String&)> save;
    juce::ComboBox chooser;
    juce::TextButton keep, remove;
    int firstCustomId = 1;
};


double automaticPitchTransitionInset(double leftDuration, double rightDuration)
{
    // A 40 ms transition is short enough to read as an ordinary note change,
    // while very short notes receive a proportionally smaller interval so the
    // head and tail handles can never cross.
    return std::max(0.002, std::min({ 0.020,
        std::max(0.01, leftDuration) * 0.20,
        std::max(0.01, rightDuration) * 0.20 }));
}

std::vector<PitchCurveEditPoint> simplifyPitchAnchors(
    const std::vector<PitchCurveEditPoint>& input, float toleranceMidi)
{
    if (input.size() <= 2) return input;
    std::vector<bool> keep(input.size(), false);
    keep.front() = true;
    keep.back() = true;
    std::function<void(std::size_t, std::size_t)> simplify =
        [&](std::size_t first, std::size_t last)
    {
        if (last <= first + 1) return;
        const auto span = input[last].timeSeconds - input[first].timeSeconds;
        auto largestError = 0.0f;
        auto largestIndex = first;
        for (auto index = first + 1; index < last; ++index)
        {
            const auto amount = span > 1.0e-9
                ? static_cast<float>((input[index].timeSeconds - input[first].timeSeconds) / span)
                : 0.0f;
            const auto interpolated = input[first].targetMidi
                + (input[last].targetMidi - input[first].targetMidi) * amount;
            const auto error = std::abs(input[index].targetMidi - interpolated);
            if (error > largestError)
            {
                largestError = error;
                largestIndex = index;
            }
        }
        if (largestError <= toleranceMidi) return;
        keep[largestIndex] = true;
        simplify(first, largestIndex);
        simplify(largestIndex, last);
    };
    simplify(0, input.size() - 1);
    std::vector<PitchCurveEditPoint> result;
    for (std::size_t index = 0; index < input.size(); ++index)
        if (keep[index]) result.push_back(input[index]);
    return result;
}

class BezierParameterEditor final : public juce::Component
{
public:
    BezierParameterEditor(const I18n& strings,
                          std::array<float, 4> initialValues)
    {
        const std::array<juce::String, 4> names {
            strings.text("pitchCurve.bezierX1"), strings.text("pitchCurve.bezierY1"),
            strings.text("pitchCurve.bezierX2"), strings.text("pitchCurve.bezierY2")
        };
        for (std::size_t index = 0; index < sliders.size(); ++index)
        {
            addAndMakeVisible(labels[index]);
            labels[index].setText(names[index], juce::dontSendNotification);
            labels[index].setJustificationType(juce::Justification::centredRight);
            labels[index].setColour(juce::Label::textColourId, Palette::text);
            addAndMakeVisible(sliders[index]);
            sliders[index].setSliderStyle(juce::Slider::LinearHorizontal);
            sliders[index].setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
            sliders[index].setNumDecimalPlacesToDisplay(2);
            sliders[index].setRange(index == 0 || index == 2 ? 0.0 : -2.0,
                                    index == 0 || index == 2 ? 1.0 : 3.0, 0.01);
            sliders[index].setValue(initialValues[index], juce::dontSendNotification);
            sliders[index].onValueChange = [this] { repaint(); };
        }
        setSize(470, 330);
    }

    [[nodiscard]] std::array<float, 4> values() const
    {
        return { static_cast<float>(sliders[0].getValue()),
                 static_cast<float>(sliders[1].getValue()),
                 static_cast<float>(sliders[2].getValue()),
                 static_cast<float>(sliders[3].getValue()) };
    }

    void paint(juce::Graphics& g) override
    {
        auto preview = getLocalBounds().removeFromTop(170).reduced(34, 14).toFloat();
        g.setColour(Palette::graphBackground);
        g.fillRoundedRectangle(preview, 5.0f);
        g.setColour(Palette::border);
        g.drawRoundedRectangle(preview, 5.0f, 1.0f);

        auto plot = preview.reduced(18.0f, 12.0f);
        const auto current = values();
        auto minimumY = std::min({ 0.0f, 1.0f, current[1], current[3] });
        auto maximumY = std::max({ 0.0f, 1.0f, current[1], current[3] });
        const auto padding = std::max(0.12f, (maximumY - minimumY) * 0.1f);
        minimumY -= padding;
        maximumY += padding;
        const auto ySpan = std::max(0.01f, maximumY - minimumY);
        const auto pointFor = [plot, minimumY, ySpan](float x, float y)
        {
            return juce::Point<float>(plot.getX() + x * plot.getWidth(),
                plot.getBottom() - ((y - minimumY) / ySpan) * plot.getHeight());
        };
        const auto start = pointFor(0.0f, 0.0f);
        const auto first = pointFor(current[0], current[1]);
        const auto second = pointFor(current[2], current[3]);
        const auto end = pointFor(1.0f, 1.0f);

        g.setColour(Palette::grid.withAlpha(0.8f));
        g.drawLine(start.x, start.y, end.x, end.y, 1.0f);
        g.setColour(Palette::textMuted.withAlpha(0.8f));
        g.drawLine(start.x, start.y, first.x, first.y, 1.0f);
        g.drawLine(second.x, second.y, end.x, end.y, 1.0f);

        juce::Path curve;
        curve.startNewSubPath(start);
        curve.cubicTo(first, second, end);
        g.setColour(Palette::noteLight);
        g.strokePath(curve, juce::PathStrokeType(2.5f));
        g.setColour(Palette::accentLight);
        g.fillEllipse(first.x - 4.0f, first.y - 4.0f, 8.0f, 8.0f);
        g.fillEllipse(second.x - 4.0f, second.y - 4.0f, 8.0f, 8.0f);
        g.setColour(Palette::text);
        g.fillEllipse(start.x - 3.0f, start.y - 3.0f, 6.0f, 6.0f);
        g.fillEllipse(end.x - 3.0f, end.y - 3.0f, 6.0f, 6.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        area.removeFromTop(178);
        for (std::size_t index = 0; index < sliders.size(); ++index)
        {
            auto row = area.removeFromTop(36);
            labels[index].setBounds(row.removeFromLeft(88));
            sliders[index].setBounds(row.reduced(4, 2));
        }
    }

private:
    std::array<juce::Label, 4> labels;
    std::array<juce::Slider, 4> sliders;
};
}

PianoRollComponent::PianoRollComponent(ProjectModel& modelToUse,
                                       const I18n& stringsToUse)
    : model(modelToUse), strings(stringsToUse)
{
    formats.registerBasicFormats();
    model.addChangeListener(this);
    setWantsKeyboardFocus(true);
    addChildComponent(inlineAliasEditor);
    inlineAliasEditor.setMultiLine(false);
    inlineAliasEditor.setReturnKeyStartsNewLine(false);
    inlineAliasEditor.setSelectAllWhenFocused(true);
    inlineAliasEditor.setJustification(juce::Justification::centredLeft);
    inlineAliasEditor.setFont(13.0f);
    inlineAliasEditor.setColour(juce::TextEditor::backgroundColourId,
                                Palette::panelRaised.withAlpha(0.98f));
    inlineAliasEditor.setColour(juce::TextEditor::textColourId, Palette::text);
    inlineAliasEditor.setColour(juce::TextEditor::outlineColourId, Palette::accentLight);
    inlineAliasEditor.setColour(juce::TextEditor::focusedOutlineColourId, Palette::noteLight);
    inlineAliasEditor.onTextChange = [this] { repaint(); };
    inlineAliasEditor.onReturnKey = [this] { finishInlineAliasEdit(true); };
    inlineAliasEditor.onEscapeKey = [this] { finishInlineAliasEdit(false); };
    inlineAliasEditor.onFocusLost = [this] { finishInlineAliasEdit(true); };
    rebuildLayout();
}

PianoRollComponent::~PianoRollComponent()
{
    for (auto& [_, thumbnail] : thumbnails)
        thumbnail->removeChangeListener(this);
    model.removeChangeListener(this);
}

void PianoRollComponent::setPixelsPerSecond(float value)
{
    // The toolbar slider runs to 8000 for millisecond-level work; clamping
    // here to 600 quietly threw away everything above it.
    pixelsPerSecond = juce::jlimit(40.0f, 8000.0f, value);
    rebuildLayout();
}

void PianoRollComponent::setRowHeight(float value)
{
    rowHeight = juce::jlimit(12.0f, 48.0f, value);
    updateCanvasSize();
    repaint();
}

void PianoRollComponent::setSourceEditMode(bool enabled)
{
    sourceEditMode = enabled;
    if (sourceEditMode && focusedClip.isEmpty())
        for (const auto& track : snapshot.tracks)
            if (!track.clips.empty())
            {
                focusedClip = track.clips.front().id;
                break;
            }
    rebuildUtauSoundSpans();
    updateCanvasSize();
    repaint();
}

void PianoRollComponent::setFocusedClip(const juce::String& clipId)
{
    focusedClip = clipId;
    updateCanvasSize();
    repaint();
}

void PianoRollComponent::setFocusedTrack(const juce::String& trackId)
{
    focusedTrack = trackId;
    rebuildNoteHits();
    repaint();
}

void PianoRollComponent::setShowNoteLabels(bool enabled)
{
    if (showNoteLabels == enabled) return;
    showNoteLabels = enabled;
    repaint();
}

std::optional<juce::Range<double>> PianoRollComponent::selectedNotesTimeSpan() const
{
    std::optional<juce::Range<double>> span;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (!selectedNotes.contains(note.id.toStdString())) continue;
                const auto start = clip.startSeconds + note.startSeconds;
                juce::Range<double> range(start, start + note.durationSeconds);
                // A note reaches back before its written start for its
                // consonant, and may stop sounding before its written end.
                // The selection has been heard when all of that has been.
                if (const auto found = utauSoundSpans.find(note.id.toStdString());
                    found != utauSoundSpans.end())
                    range = range.getUnionWith(
                        juce::Range<double>(found->second.first, found->second.second));
                span = span ? span->getUnionWith(range) : range;
            }
    return span;
}

void PianoRollComponent::ensureDefaultEnvelope(const juce::String& noteId)
{
    // Without a lyric a note has no voicebank entry, so nothing knows where it
    // starts or stops sounding and it is left carrying no envelope at all.
    // That is not the same as the shape it is drawn with: an absent envelope
    // is no shaping whatever, which is why such a note came out unlike its
    // neighbours until the soft rise was applied to it by hand.  Giving it a
    // lyric writes exactly the shape already on screen -- read back from the
    // same place the drawing comes from, so the two cannot disagree.
    if (noteId.isEmpty()) return;
    // The span follows from the lyric that was just set, and the layout that
    // knows about it would otherwise only be rebuilt on a later message.
    rebuildLayout();
    if (utauSoundSpans.find(noteId.toStdString()) == utauSoundSpans.end()) return;
    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (note.id != noteId) continue;
                // Already shaped, by hand or by a preset: leave it be.
                if (!note.amplitudeEnvelope.empty()) return;
                if (note.label.trim().isEmpty()) return;
                const auto absoluteStart = clip.startSeconds + note.startSeconds;
                model.setNotesAmplitudeEnvelopes(
                    { { noteId, amplitudeEnvelopeFor(note, absoluteStart) } });
                return;
            }
    }
}

int PianoRollComponent::applyEnvelopePreset(double attackSeconds,
                                            double releaseSeconds,
                                            float plateauEndDb)
{
    const auto selected = selectedNoteIds();
    if (selected.empty()) return 0;
    std::vector<std::pair<juce::String, std::vector<AmplitudeEnvelopePoint>>> edits;
    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (std::find(selected.begin(), selected.end(), note.id) == selected.end())
                    continue;
                const auto absoluteStart = clip.startSeconds + note.startSeconds;
                auto first = 0.0;
                auto last = std::max(0.02, note.durationSeconds);
                if (const auto span = utauSoundSpans.find(note.id.toStdString());
                    span != utauSoundSpans.end())
                {
                    first = std::min(0.0, span->second.first - absoluteStart);
                    last = std::max(first + 0.02, span->second.second - absoluteStart);
                }
                auto attackEnd = first + attackSeconds;
                auto releaseStart = last - releaseSeconds;
                // Same fallback the default shape uses: a note too short to
                // hold both ramps gets thirds rather than crossed points.
                if (releaseStart <= attackEnd + 0.005)
                {
                    const auto span = last - first;
                    attackEnd = first + span / 3.0;
                    releaseStart = first + span * 2.0 / 3.0;
                }
                edits.emplace_back(note.id, std::vector<AmplitudeEnvelopePoint> {
                    { first, -60.0f }, { attackEnd, 0.0f },
                    { releaseStart, plateauEndDb }, { last, -60.0f } });
            }
    }
    const auto count = static_cast<int>(edits.size());
    if (count > 0) model.setNotesAmplitudeEnvelopes(std::move(edits));
    return count;
}

void PianoRollComponent::setShowNoteRange(bool enabled)
{
    if (showNoteRange == enabled) return;
    showNoteRange = enabled;
    repaint();
}

void PianoRollComponent::setShowEnvelope(bool enabled)
{
    if (showEnvelope == enabled) return;
    showEnvelope = enabled;
    repaint();
}

void PianoRollComponent::setShowPitchLine(bool enabled)
{
    if (showPitchLine == enabled) return;
    showPitchLine = enabled;
    repaint();
}

void PianoRollComponent::setShowWaveforms(bool enabled)
{
    if (showWaveforms == enabled) return;
    showWaveforms = enabled;
    repaint();
}

void PianoRollComponent::setSampleRegions(const std::vector<SampleRegionSetting>& regions,
                                          int activeRegion)
{
    sampleRegions = regions;
    activeSampleRegion = juce::jlimit(-1, static_cast<int>(sampleRegions.size()) - 1,
                                      activeRegion);
    repaint();
}

void PianoRollComponent::setPlayheadSeconds(double seconds)
{
    // Thirty times a second, playing or not.  What actually changes is a one
    // pixel line and the mark on the edge that says which way it lies when it
    // is off screen, so repainting the whole roll for it spent a full window
    // repaint on every tick.  The band covers where the line was as well as
    // where it now is: on a scrolling view the old one is blitted along with
    // everything else and has to be painted over.
    if (std::abs(seconds - playheadSeconds) < 1.0e-9) return;
    const auto before = timeToX(playheadSeconds);
    playheadSeconds = seconds;
    const auto after = timeToX(playheadSeconds);
    const auto left = static_cast<int>(std::floor(std::min(before, after))) - 3;
    const auto right = static_cast<int>(std::ceil(std::max(before, after))) + 3;
    playheadBand = { left, 0, std::max(1, right - left), getHeight() };
    repaint(playheadBand);
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        const auto x = viewport->getViewPositionX();
        const auto y = viewport->getViewPositionY();
        repaint(x + 50, y, 24, 40);
        repaint(x + viewport->getViewWidth() - 24, y, 24, 40);
    }
}

void PianoRollComponent::setTool(Tool nextTool)
{
    tool = nextTool;
    setMouseCursor(tool == Tool::draw || tool == Tool::line || tool == Tool::points
                       || tool == Tool::amplitude || tool == Tool::flagCurve
                       ? juce::MouseCursor::CrosshairCursor
                       : juce::MouseCursor::NormalCursor);
    repaint();
}

const NoteData* PianoRollComponent::findNote(const juce::String& noteId, bool* isUtau) const
{
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == noteId)
                {
                    if (isUtau != nullptr)
                        *isUtau = track.pitchAlgorithm == PitchAlgorithm::utau;
                    return &note;
                }
    if (isUtau != nullptr) *isUtau = false;
    return nullptr;
}

std::vector<PianoRollComponent::PositionedUtauNote>
PianoRollComponent::positionedUtauNotesFor(const juce::String& noteId) const
{
    std::vector<PositionedUtauNote> result;
    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        auto containsTarget = false;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                containsTarget = containsTarget || note.id == noteId;
        if (!containsTarget) continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                const auto start = clip.startSeconds + note.startSeconds;
                result.push_back({ note.id, start, start + note.durationSeconds });
            }
        break;
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right)
    {
        if (std::abs(left.startSeconds - right.startSeconds) > 1.0e-9)
            return left.startSeconds < right.startSeconds;
        return left.endSeconds < right.endSeconds;
    });
    return result;
}

std::optional<PianoRollComponent::PositionedUtauNote>
PianoRollComponent::previousUtauNoteFor(const juce::String& noteId) const
{
    const auto notes = positionedUtauNotesFor(noteId);
    const auto found = std::find_if(notes.begin(), notes.end(),
        [&](const auto& note) { return note.id == noteId; });
    if (found == notes.end() || found == notes.begin()) return std::nullopt;
    return *std::prev(found);
}

std::optional<PianoRollComponent::PositionedUtauNote>
PianoRollComponent::nextUtauNoteFor(const juce::String& noteId) const
{
    const auto notes = positionedUtauNotesFor(noteId);
    const auto found = std::find_if(notes.begin(), notes.end(),
        [&](const auto& note) { return note.id == noteId; });
    if (found == notes.end() || std::next(found) == notes.end()) return std::nullopt;
    return *std::next(found);
}

bool PianoRollComponent::formsAdjacentPitchBoundary(const PositionedUtauNote& left,
                                                     const PositionedUtauNote& right)
{
    return std::abs(left.endSeconds - right.startSeconds) <= 0.002;
}

std::optional<PianoRollComponent::IncomingJoinGlide>
PianoRollComponent::incomingJoinGlideFor(const NoteData& note)
{
    // A native connection (Melodyne pitch join or a forced connection) means
    // the renderer glides this note's head out of the previous note's tail
    // pitch.  The displayed pitch line has to do the same, or a connected note
    // shows a step the ear never hears.  UTAU adjacency is handled separately
    // by the crossfade bridge, so this is for the connection-flag path only.
    if (!note.connectedToPrevious) return std::nullopt;
    const NoteData* previousNote = nullptr;
    auto previousEnd = -std::numeric_limits<double>::infinity();
    for (const auto& track : snapshot.tracks)
    {
        auto ownsNote = false;
        for (const auto& clip : track.clips)
            for (const auto& candidate : clip.notes)
                ownsNote = ownsNote || candidate.id == note.id;
        if (!ownsNote) continue;
        // The joined start is this note's absolute position on its own track.
        double joinedStart = 0.0;
        for (const auto& clip : track.clips)
            for (const auto& candidate : clip.notes)
                if (candidate.id == note.id)
                    joinedStart = clip.startSeconds + candidate.startSeconds;
        for (const auto& clip : track.clips)
            for (const auto& candidate : clip.notes)
            {
                if (candidate.id == note.id) continue;
                const auto end = clip.startSeconds + candidate.startSeconds
                    + candidate.durationSeconds;
                if (end <= joinedStart + 0.002 && end > previousEnd)
                {
                    previousEnd = end;
                    previousNote = &candidate;
                }
            }
        break;
    }
    if (previousNote == nullptr) return std::nullopt;
    // The previous note's tail pitch is the last thing its own displayed line
    // reaches, so the two meet exactly.
    const auto& previousAnchors = pitchAnchorsFor(*previousNote);
    const auto leadMidi = previousAnchors.empty()
        ? static_cast<double>(previousNote->midiNote)
        : static_cast<double>(previousAnchors.back().targetMidi);
    // The same join window the renderer uses (AudioEngine::makeRenderRequest).
    const auto joinSeconds = std::min(0.08,
        std::max(0.012, note.durationSeconds * 0.22));
    return IncomingJoinGlide { leadMidi, joinSeconds };
}

float PianoRollComponent::pitchAt(const std::vector<PitchCurveEditPoint>& anchors,
                                  double timeSeconds)
{
    return evaluatePitchCurve(anchors, timeSeconds);
}

std::vector<AmplitudeEnvelopePoint> PianoRollComponent::diagnosticDrawnEnvelope(
    const juce::String& id) const
{
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == id)
                    return amplitudeEnvelopeFor(note,
                        clip.startSeconds + note.startSeconds);
    return {};
}

std::vector<AmplitudeEnvelopePoint> PianoRollComponent::amplitudeEnvelopeFor(
    const NoteData& note, double absoluteStart) const
{
    if (!note.amplitudeEnvelope.empty())
    {
        auto points = note.amplitudeEnvelope;
        // The two outer points mark where the note starts and stops sounding
        // rather than shaping anything, and that span moves on its own: a
        // slower consonant reaches further back for its lead-in, and giving
        // the next note one lets this one sound for longer.  The ends have to
        // follow, or the note would be silenced before it really ends.
        //
        // They move as whole ramps, though.  Pinning only the outermost point
        // stretched the fade it belongs to -- lengthen the lead-in and the
        // rise laid down by a preset grew with it, which is a change of shape
        // and not the lengthening that was asked for.  The opening and
        // closing ramps keep the slope they were given; the plateau between
        // them takes up the difference, and points placed inside stay put.
        if (const auto span = utauSoundSpans.find(note.id.toStdString());
            span != utauSoundSpans.end() && points.size() >= 2)
        {
            const auto first = std::min(0.0, span->second.first - absoluteStart);
            const auto last = std::max(first + 0.02,
                                       span->second.second - absoluteStart);
            const auto startShift = first - points.front().timeSeconds;
            const auto endShift = last - points.back().timeSeconds;
            // With three points the middle one closes the rise and opens the
            // fall at once, so it cannot follow both ends; it is left alone.
            if (points.size() >= 4)
            {
                points[1].timeSeconds += startShift;
                points[points.size() - 2].timeSeconds += endShift;
            }
            points.front().timeSeconds = first;
            points.back().timeSeconds = last;
            // A note can also become shorter than the ramps it was given.
            // Nothing may overtake what comes before it, and nothing may pass
            // the end.
            auto earliest = first;
            for (auto& point : points)
            {
                point.timeSeconds = juce::jlimit(earliest, last, point.timeSeconds);
                earliest = point.timeSeconds;
            }
            points.back().timeSeconds = last;
            // And a spliced boundary crosses here too.  This used to be done
            // only for notes carrying no envelope of their own, so once typing
            // a lyric began writing one, splicing stopped showing at all -- the
            // sound crossfaded while the drawing said otherwise.
            //
            // A crossfade is the one thing that does change a ramp's slope:
            // the rise has to reach the instant the note before stops
            // sounding, and the fall has to start where the next one begins,
            // whatever slope was stored.  Points placed inside the note are
            // left where they are.
            if (points.size() >= 4)
            {
                const auto crossing = spliceCrossingFor(
                    note, absoluteStart, points.front().timeSeconds,
                    points.back().timeSeconds);
                if (crossing.riseEnd) points[1].timeSeconds = *crossing.riseEnd;
                if (crossing.fallStart)
                    points[points.size() - 2].timeSeconds = *crossing.fallStart;
                auto earliest = points.front().timeSeconds;
                for (auto& point : points)
                {
                    point.timeSeconds = juce::jlimit(earliest, last, point.timeSeconds);
                    earliest = point.timeSeconds;
                }
            }
        }
        // The base value scales the whole envelope for display, so the roll
        // shows what will be heard.  The stored shape stays unscaled.
        return scaledAmplitudeEnvelope(points, note.amplitudeEnvelopeBasePercent);
    }
    auto firstTime = 0.0;
    auto lastTime = std::max(0.02, note.durationSeconds);
    if (const auto span = utauSoundSpans.find(note.id.toStdString());
        span != utauSoundSpans.end())
    {
        firstTime = std::min(0.0, span->second.first - absoluteStart);
        // The sounding end, on whichever side of the nominal end it falls: a
        // note handing over to the next one stops early, and putting the
        // release at the nominal end would place the fall in a stretch that
        // never plays, leaving a flat line where the note should die away.
        lastTime = std::max(firstTime + 0.02, span->second.second - absoluteStart);
    }

    // UTAU-style default: silence at the real sample boundaries with a short
    // attack/release and a wide unity plateau.  A negative first point keeps
    // the preutterance visible, so adjacent notes may overlap naturally.
    const auto spanSeconds = std::max(0.02, lastTime - firstTime);
    // A 15 ms attack and a 35 ms release, fixed whatever the note length.
    // Taking a share of the note instead gave a long one an eighty
    // millisecond attack, which fades the consonant down while it is still
    // being pronounced.  The two are deliberately unequal: the attack has to
    // stay short enough to leave the consonant alone, while the release wants
    // length to avoid a click and to hand over cleanly.  UTAU's own default
    // is 5 and 35 -- the median and the most common value across 1097 notes
    // of real USTs -- and the softer 15 ms start is a deliberate choice here,
    // the same shape the toolbar offers as a preset.
    constexpr auto attackSeconds = 0.015;
    constexpr auto releaseSeconds = 0.035;
    auto attackEnd = firstTime + attackSeconds;
    auto releaseStart = lastTime - releaseSeconds;
    // A spliced boundary crossfades the overlap -- the stretch both notes
    // sound through -- and nothing else.  It is bounded by where the later
    // note starts sounding and where the earlier one stops, and both sides
    // read those same two instants, so the rise and the fall are mirror
    // images and the two envelopes really cross.  Neither span moves: an
    // overlap the oto does not leave has nothing to fade across, and the
    // notes keep their ordinary ramps.
    {
        const auto crossing = spliceCrossingFor(note, absoluteStart,
                                                firstTime, lastTime);
        if (crossing.riseEnd) attackEnd = *crossing.riseEnd;
        if (crossing.fallStart) releaseStart = *crossing.fallStart;
    }
    if (releaseStart <= attackEnd + 0.005)
    {
        attackEnd = firstTime + spanSeconds / 3.0;
        releaseStart = firstTime + spanSeconds * 2.0 / 3.0;
    }
    return scaledAmplitudeEnvelope(
        { { firstTime, -60.0f }, { attackEnd, 0.0f },
          { releaseStart, 0.0f }, { lastTime, -60.0f } },
        note.amplitudeEnvelopeBasePercent);
}

float PianoRollComponent::amplitudeDbAt(
    const std::vector<AmplitudeEnvelopePoint>& points, double timeSeconds)
{
    if (points.empty()) return 0.0f;
    const auto right = std::upper_bound(points.begin(), points.end(), timeSeconds,
        [](double value, const AmplitudeEnvelopePoint& point)
        {
            return value < point.timeSeconds;
        });
    if (right == points.begin()) return right->gainDb;
    if (right == points.end()) return points.back().gainDb;
    const auto& left = *std::prev(right);
    const auto span = right->timeSeconds - left.timeSeconds;
    const auto amount = span > 1.0e-9
        ? static_cast<float>(juce::jlimit(0.0, 1.0,
            (timeSeconds - left.timeSeconds) / span)) : 0.0f;
    return left.gainDb + (right->gainDb - left.gainDb) * amount;
}

float PianoRollComponent::amplitudeY(float midi, float gainDb) const
{
    // Unity sits on the top edge of the note block and silence on its bottom,
    // so an ordinary envelope fills the block rather than hovering across its
    // middle.  The scale still runs to 200%, which simply puts anything above
    // unity in the block's height again above it -- louder reads as taller
    // than the note.
    const auto blockHeight = std::max(4.0f, rowHeight - 6.0f);
    const auto top = midiToY(midi) + 3.0f - blockHeight;
    const auto normalized = juce::jlimit(0.0f, 1.0f,
        amplitudePercentFromDb(gainDb) / 200.0f);
    return top + (1.0f - normalized) * blockHeight * 2.0f;
}

float PianoRollComponent::amplitudeDbFromY(float midi, float y) const
{
    // The exact inverse of amplitudeY, so a handle lands where it was dropped.
    const auto blockHeight = std::max(4.0f, rowHeight - 6.0f);
    const auto top = midiToY(midi) + 3.0f - blockHeight;
    const auto normalized = juce::jlimit(0.0f, 1.0f,
        1.0f - (y - top) / (blockHeight * 2.0f));
    return amplitudeDbFromPercent(normalized * 200.0f);
}

PianoRollComponent::SpliceCrossing PianoRollComponent::spliceCrossingFor(
    const NoteData& note, double absoluteStart,
    double firstTime, double lastTime) const
{
    SpliceCrossing crossing;
    const auto neighbours = utauNeighbours.find(note.id.toStdString());
    if (neighbours == utauNeighbours.end()) return crossing;
    if (note.utauSplice && !neighbours->second.previous.empty())
        if (const auto span = utauSoundSpans.find(neighbours->second.previous);
            span != utauSoundSpans.end())
        {
            const auto riseEnd = span->second.second - absoluteStart;
            if (riseEnd > firstTime + 0.003 && riseEnd < lastTime)
                crossing.riseEnd = riseEnd;
        }
    if (neighbours->second.nextSpliced)
        if (const auto span = utauSoundSpans.find(neighbours->second.next);
            span != utauSoundSpans.end())
        {
            const auto fallStart = span->second.first - absoluteStart;
            if (fallStart > firstTime && fallStart < lastTime - 0.003)
                crossing.fallStart = fallStart;
        }
    return crossing;
}

bool PianoRollComponent::keyboardLabelVisible(int midi, float rowHeight)
{
    if (midi % 12 == 0) return true;                 // C always marks the octave
    if (rowHeight >= 11.0f) return true;             // room for all twelve
    if (rowHeight >= 7.5f) return !juce::MidiMessage::isMidiNoteBlack(midi);
    return false;
}

juce::String PianoRollComponent::amplitudeZoomLabel(bool zoomIn)
{
    return juce::String::fromUTF8(zoomIn ? "+" : "−");
}

juce::Rectangle<float> PianoRollComponent::flagLaneBounds() const
{
    // The same strip the loudness envelope uses: one lane, one place, so the
    // two read alike and there is never a question of which is on top.
    return amplitudeLaneBounds();
}

juce::Rectangle<float> PianoRollComponent::flagLanePlotBounds() const
{
    return amplitudeLanePlotBounds();
}

std::pair<float, float> PianoRollComponent::flagLaneWindow() const
{
    const auto& kind = flagCurveKindFor(laneFlag);
    const auto full = std::max(1.0e-3f, kind.maximum - kind.minimum);
    const auto span = full / juce::jlimit(1.0f, 16.0f, laneZoom);
    // At 1x this pins the window to the whole range whatever the centre says,
    // so the unzoomed lane is exactly what it always was.
    const auto low = juce::jlimit(kind.minimum, kind.maximum - span,
                                  laneCentre - span * 0.5f);
    return { low, low + span };
}

float PianoRollComponent::flagLaneY(float value) const
{
    const auto plot = flagLanePlotBounds();
    const auto [low, high] = flagLaneWindow();
    const auto span = std::max(1.0e-3f, high - low);
    const auto normalized = juce::jlimit(0.0f, 1.0f, (value - low) / span);
    return plot.getBottom() - normalized * plot.getHeight();
}

float PianoRollComponent::flagValueFromLaneY(float y) const
{
    const auto plot = flagLanePlotBounds();
    const auto& kind = flagCurveKindFor(laneFlag);
    if (plot.getHeight() <= 1.0f) return kind.minimum;
    const auto [low, high] = flagLaneWindow();
    // Held to what the flag allows, not to what is on show: a handle dragged
    // above the window still reads as the value it is heading for, and the
    // window is moved to follow it rather than pinning it at the edge.
    const auto normalized = (plot.getBottom() - y) / plot.getHeight();
    return juce::jlimit(kind.minimum, kind.maximum,
                        low + normalized * (high - low));
}

void PianoRollComponent::centreFlagLaneOnCurves()
{
    // Where the curves actually sit, so zooming in lands on the work rather
    // than on the middle of a range whose other half is empty.
    auto total = 0.0;
    auto count = 0;
    for (const auto& hit : noteHits)
    {
        bool utau = false;
        const auto* note = findNote(hit.id, &utau);
        if (note == nullptr || !utau) continue;
        for (const auto& point : flagLaneCurveFor(*note))
        {
            total += point.value;
            ++count;
        }
    }
    const auto& kind = flagCurveKindFor(laneFlag);
    laneCentre = count > 0
        ? static_cast<float>(total / count)
        : (kind.minimum > 0.0f ? kind.minimum
           : kind.maximum < 0.0f ? kind.maximum : 0.0f);
}

void PianoRollComponent::keepFlagValueInView(float value)
{
    const auto [low, high] = flagLaneWindow();
    const auto span = high - low;
    const auto margin = span * 0.08f;
    if (value > high - margin) laneCentre = value + span * 0.5f - margin;
    else if (value < low + margin) laneCentre = value - span * 0.5f + margin;
    else return;
    const auto& kind = flagCurveKindFor(laneFlag);
    laneCentre = juce::jlimit(kind.minimum + span * 0.5f,
                              kind.maximum - span * 0.5f, laneCentre);
}

void PianoRollComponent::nudgeFlagLaneZoom(bool zoomIn)
{
    static constexpr std::array<float, 6> levels { 1.0f, 2.0f, 3.0f, 4.0f,
                                                   6.0f, 8.0f };
    auto level = 0;
    auto distance = std::numeric_limits<float>::max();
    for (int index = 0; index < static_cast<int>(levels.size()); ++index)
        if (const auto next = std::abs(laneZoom - levels[static_cast<std::size_t>(index)]);
            next < distance)
        {
            distance = next;
            level = index;
        }
    const auto wanted = levels[static_cast<std::size_t>(
        juce::jlimit(0, static_cast<int>(levels.size()) - 1,
                     level + (zoomIn ? 1 : -1)))];
    // Leaving the full view, the centre is whatever the range's middle is,
    // which is rarely where anything is drawn; take it to the curves instead.
    if (laneZoom <= 1.0f && wanted > 1.0f) centreFlagLaneOnCurves();
    laneZoom = wanted;
    repaint();
}

void PianoRollComponent::setFlagLaneFlag(const juce::String& flag)
{
    for (const auto& kind : flagCurveKinds())
        if (flag == kind.flag)
        {
            laneFlag = flag;
            // Another flag, another range: the centre is in the old flag's
            // units and means nothing here.  The zoom level is a preference
            // and stays.
            centreFlagLaneOnCurves();
            // A drag belongs to the curve it started on.
            if (dragMode == DragMode::flagPoint) dragMode = DragMode::none;
            repaint();
            return;
        }
}

const TrackData* PianoRollComponent::trackForNote(const juce::String& noteId) const
{
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == noteId) return &track;
    return nullptr;
}

std::pair<double, double> PianoRollComponent::flagLaneSpanFor(
    const NoteData& note) const
{
    const auto whole = std::max(0.02, note.durationSeconds);
    // What the note sounds for, in its own time.  It begins a lead-in ahead of
    // the note -- that lead-in is the consonant -- and ends where the next
    // note's overlap has taken over.  A flag reaches all of it, so all of it
    // can be edited: measuring from the note's own start instead left the
    // consonant drawn but untouchable, since it lies at negative times.
    auto sounds = std::pair<double, double>(0.0, whole);
    for (const auto& hit : noteHits)
        if (hit.id == note.id)
        {
            const auto absoluteStart = hit.startSeconds + hit.clipStartSeconds;
            if (const auto span = utauSoundSpans.find(note.id.toStdString());
                span != utauSoundSpans.end())
                sounds = { span->second.first - absoluteStart,
                           span->second.second - absoluteStart };
            break;
        }
    if (!flagCurveKindFor(laneFlag).onsetOnly) return sounds;
    auto leadIn = -sounds.first;
    const auto* track = trackForNote(note.id);
    if (track == nullptr)
    {
        if (leadIn <= 1.0e-4 && note.utauPreutteranceOverrideEnabled)
            leadIn = note.utauPreutteranceSeconds;
        return { -std::max(0.02, std::min(leadIn, whole)), 0.0 };
    }
    juce::ignoreUnused(whole);
    const auto velocity = note.utauConsonantVelocity != inheritedUtauConsonantVelocity
        ? note.utauConsonantVelocity : track->utauConsonantVelocity;
    const auto timing = backend::UtauRenderer::sampleTiming(
        track->voicebankDirectory, note.label, note.midiNote, velocity,
        utauModeUsesRegions(track->utauMode),
        track->utauMode == UtauMode::mou);
    if (leadIn <= 1.0e-4 && timing) leadIn = timing->preutteranceSeconds;
    if (leadIn <= 1.0e-4 && note.utauPreutteranceOverrideEnabled)
        leadIn = note.utauPreutteranceSeconds;
    leadIn = std::max(0.02, std::min(leadIn, whole));

    // Where the engine's con_out lands, which is the only thing b and bh can
    // reach past.  With a region plan -- four-region on and the sample carrying
    // oto4 rows, exactly what makes the renderer send the region lengths -- it
    // is the onset region's own boundary, and that ends at the note's start.
    // Without one the engine falls back to the oto consonant scaled by
    // velocity, measured from the start of the rendered stretch, which runs
    // past the note's start whenever that is longer than the lead-in: la- has
    // a 129 ms consonant against a 65 ms lead-in, and bh reaches 64 ms into it.
    auto onsetEnd = 0.0;
    if (timing && !(utauModeUsesRegions(track->utauMode) && timing->hasRegions))
        onsetEnd = juce::jlimit(0.0, whole,
            timing->consonantSeconds * std::pow(2.0, 1.0 - velocity / 100.0)
                - leadIn);
    return { -leadIn, onsetEnd };
}

std::vector<juce::String> PianoRollComponent::chosenNoteIds(
    const juce::String& noteId) const
{
    std::vector<juce::String> ids;
    ids.reserve(selectedNotes.size());
    for (const auto& id : selectedNotes)
        ids.push_back(juce::String::fromUTF8(id.c_str()));
    if (ids.empty() && noteId.isNotEmpty()) ids.push_back(noteId);
    return ids;
}

bool PianoRollComponent::flagResetAvailable(
    const std::vector<juce::String>& noteIds) const
{
    for (const auto& id : noteIds)
        if (const auto* note = findNote(id); note != nullptr
            && note->utauFlagCurveEnabled && !note->utauFlagCurves.empty())
            return true;
    return false;
}

std::vector<juce::String> PianoRollComponent::laneNoteIds() const
{
    std::vector<juce::String> ids;
    ids.reserve(noteHits.size());
    for (const auto& hit : noteHits)
    {
        bool utau = false;
        if (const auto* note = findNote(hit.id, &utau); note != nullptr && utau)
            ids.push_back(hit.id);
    }
    return ids;
}

bool PianoRollComponent::flagResetAvailableForLane() const
{
    for (const auto& id : laneNoteIds())
        if (flagResetAvailableForNote(id)) return true;
    return false;
}

void PianoRollComponent::showFlagLaneSwitchContextMenu(juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String::fromUTF8("flag 包络 · ") + laneFlag);
    menu.addItem(1, juce::String::fromUTF8("重置该 flag（整轨）"),
                 flagResetAvailableForLane(), false);
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
        { screenPosition.x, screenPosition.y, 1, 1 }),
        [safe](int chosen)
        {
            if (safe == nullptr || chosen != 1) return;
            // This flag across every note on show, and nothing else: the other
            // sixteen curves on each note stay where they are.
            safe->model.resetNotesUtauFlagCurve(safe->laneNoteIds(), safe->laneFlag);
        });
}

bool PianoRollComponent::flagResetAvailableForNote(const juce::String& noteId) const
{
    const auto* note = findNote(noteId);
    return note != nullptr && note->utauFlagCurveEnabled
        && !flagCurvePointsFor(*note, laneFlag).empty();
}

std::pair<double, double> PianoRollComponent::flagLaneDrawnSpan(
    const NoteData& note, double absoluteStart) const
{
    // The same stretch that can be edited, so what is drawn is what answers a
    // click and there is nowhere on the line that cannot be worked on.
    const auto [from, to] = flagLaneSpanFor(note);
    return { absoluteStart + from, absoluteStart + to };
}

juce::Point<float> PianoRollComponent::diagnosticFlagHandleCentre(
    const juce::String& noteId, int index) const
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return {};
    for (const auto& hit : noteHits)
        if (hit.id == noteId)
        {
            const auto start = hit.startSeconds + hit.clipStartSeconds;
            const auto points = flagLaneCurveFor(*note);
            if (index < 0 || index >= static_cast<int>(points.size())) return {};
            const auto plot = flagLanePlotBounds();
            const auto [drawFrom, drawTo] = flagLaneDrawnSpan(*note, start);
            const auto left = juce::jlimit(plot.getX(), plot.getRight(),
                                           timeToX(drawFrom));
            const auto right = juce::jlimit(plot.getX(), plot.getRight(),
                                            timeToX(drawTo));
            const auto& point = points[static_cast<std::size_t>(index)];
            return { juce::jlimit(left, right, timeToX(start + point.timeSeconds)),
                     flagLaneY(point.value) };
        }
    return {};
}

std::vector<FlagCurvePoint> PianoRollComponent::flagLaneCurveFor(
    const NoteData& note) const
{
    if (!note.utauFlagCurveEnabled) return {};
    auto points = flagCurvePointsFor(note, laneFlag);
    if (!points.empty()) return points;
    // Flat at the flag's own resting value, spanning the note: something to
    // take hold of.  Zero for the ones that run either side of it, the low end
    // for those that do not, since that is where "no effect" sits for them.
    const auto& kind = flagCurveKindFor(laneFlag);
    const auto resting = kind.minimum > 0.0f ? kind.minimum
                       : kind.maximum < 0.0f ? kind.maximum : 0.0f;
    // One handle, at the end.  A curve of a single point reads as that value
    // for the whole note -- both the engine's fc_eval and flagCurveValueAt
    // hold flat either side of a lone point -- so dragging it is setting the
    // flag's level for the note, which is what is wanted before there is any
    // shape to draw.  A second point is a click away, and makes it a curve.
    const auto [from, to] = flagLaneSpanFor(note);
    juce::ignoreUnused(from);
    return { { to, resting } };
}

juce::Rectangle<float> PianoRollComponent::flagLaneZoomButtonBounds(bool zoomIn) const
{
    // Left of the flag switch, laid out like the loudness lane's own pair so
    // the two lanes are worked the same way.
    const auto lane = flagLaneBounds();
    return { lane.getRight() - 96.0f - (zoomIn ? 25.0f : 49.0f),
             lane.getY() + 4.0f, 21.0f, 21.0f };
}

juce::Rectangle<float> PianoRollComponent::flagLaneSwitchBounds() const
{
    // Top-right of the lane, beside the heading that names the flag.
    auto header = flagLaneBounds().removeFromTop(28.0f);
    return header.removeFromRight(96.0f).reduced(6.0f, 4.0f);
}

void PianoRollComponent::showFlagLaneSwitchMenu()
{
    juce::PopupMenu menu;
    auto id = 1;
    for (const auto& kind : flagCurveKinds())
    {
        menu.addItem(id, juce::String(kind.flag) + juce::String::fromUTF8("  ·  ")
                             + juce::String::fromUTF8(kind.label),
                     true, laneFlag == kind.flag);
        ++id;
    }
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    const auto where = flagLaneSwitchBounds().toNearestInt()
                           + getScreenPosition();
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(where),
        [safe](int chosen)
        {
            if (safe == nullptr || chosen <= 0) return;
            const auto& kinds = flagCurveKinds();
            if (chosen <= static_cast<int>(kinds.size()))
                safe->setFlagLaneFlag(kinds[static_cast<std::size_t>(chosen - 1)].flag);
        });
}

PianoRollComponent::FlagPointMenuState PianoRollComponent::flagPointMenuState(
    int pointCount, float value)
{
    return { pointCount > 1, std::abs(value - flagCurveDefault) > 1.0e-4f };
}

void PianoRollComponent::showFlagPointMenu(const juce::String& noteId, int index,
                                           juce::Point<int> screenPosition)
{
    const auto* note = findNote(noteId);
    if (note == nullptr || index < 0) return;
    const auto shown = flagLaneCurveFor(*note);
    if (index >= static_cast<int>(shown.size())) return;
    const auto value = shown[static_cast<std::size_t>(index)].value;
    const auto state = flagPointMenuState(
        static_cast<int>(flagCurvePointsFor(*note, laneFlag).size()), value);
    juce::PopupMenu menu;
    menu.addItem(1, juce::String::fromUTF8("删除标点"), state.canDelete, false);
    menu.addItem(2, juce::String::fromUTF8("重置为默认值（0）"), state.canReset, false);
    menu.addItem(3, juce::String::fromUTF8("输入 flag 值…"), true, false);
    menu.addSeparator();
    // Only the flag on show, and only this note: the lane's other sixteen
    // curves and every other note are left as they are.
    menu.addItem(4, juce::String::fromUTF8("重置该音的 ") + laneFlag,
                 flagResetAvailableForNote(noteId), false);
    // The shape of the segment arriving at this handle, so the first one has
    // no segment to shape.  Same names and same maths as the pitch line.
    menu.addSeparator();
    if (index == 0)
        menu.addItem(20, juce::String::fromUTF8("线条形状（首个标点没有前段）"),
                     false, false);
    else
    {
        const auto shape = flagCurvePointsFor(*note, laneFlag)[static_cast<std::size_t>(index)].shape;
        juce::PopupMenu shapes;
        shapes.addItem(21, juce::String::fromUTF8("直线"), true,
                       shape == PitchCurveShape::linear);
        shapes.addItem(22, juce::String::fromUTF8("平滑"), true,
                       shape == PitchCurveShape::smooth);
        shapes.addItem(23, juce::String::fromUTF8("缓入"), true,
                       shape == PitchCurveShape::easeIn);
        shapes.addItem(24, juce::String::fromUTF8("缓出"), true,
                       shape == PitchCurveShape::easeOut);
        shapes.addSeparator();
        shapes.addItem(25, juce::String::fromUTF8("自定义贝塞尔…"), true,
                       shape == PitchCurveShape::customBezier);
        menu.addSubMenu(juce::String::fromUTF8("线条形状"), shapes);
    }
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    // At the handle, like every other right-click menu here.  Aimed at the
    // component instead, it opened in the corner of the roll, which for a lane
    // at the bottom of a tall canvas is nowhere near the cursor.
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
        { screenPosition.x, screenPosition.y, 1, 1 }),
        [safe, noteId, index](int chosen)
        {
            if (safe == nullptr || chosen <= 0) return;
            if (chosen == 3) { safe->showFlagPointValueDialog(noteId, index); return; }
            if (chosen == 4)
            {
                // Dropping the curve leaves the note showing the starting
                // handle again, which is what the initial state is.
                safe->model.setNoteUtauFlagCurve(noteId, safe->laneFlag, {});
                return;
            }
            if (chosen == 25) { safe->showFlagPointBezierDialog(noteId, index); return; }
            const auto* current = safe->findNote(noteId);
            if (current == nullptr) return;
            auto points = flagCurvePointsFor(*current, safe->laneFlag);
            if (index >= static_cast<int>(points.size())) return;
            auto& point = points[static_cast<std::size_t>(index)];
            if (chosen == 1) points.erase(points.begin() + index);
            else if (chosen == 2) point.value = flagCurveDefault;
            else if (chosen >= 21 && chosen <= 24)
                point.shape = chosen == 21 ? PitchCurveShape::linear
                    : chosen == 22 ? PitchCurveShape::smooth
                    : chosen == 23 ? PitchCurveShape::easeIn
                                   : PitchCurveShape::easeOut;
            else return;
            safe->model.setNoteUtauFlagCurve(noteId, safe->laneFlag, std::move(points));
        });
}

juce::String PianoRollComponent::flagPointValueText(const juce::String& noteId,
                                                    int index) const
{
    const auto* note = findNote(noteId);
    if (note == nullptr || index < 0) return {};
    // What the lane is showing, not what the note has stored.  A note with no
    // curve yet still shows the handle every flag starts from, and the menu
    // offers to type its value -- read from the stored curve there was nothing
    // to read and the box never opened at all.
    const auto points = flagLaneCurveFor(*note);
    if (index >= static_cast<int>(points.size())) return {};
    return juce::String(points[static_cast<std::size_t>(index)].value, 3);
}

juce::String PianoRollComponent::flagPointValueBlurb(const juce::String& noteId,
                                                     int index) const
{
    const auto* note = findNote(noteId);
    if (note == nullptr || index < 0) return {};
    const auto shown = flagLaneCurveFor(*note);
    if (index >= static_cast<int>(shown.size())) return {};
    const auto& point = shown[static_cast<std::size_t>(index)];
    // The flag the lane is on, not g.  This was written for g and said so
    // whatever was being edited: Mt reaches +/-100 and Mq starts at 0, so the
    // range printed was wrong for sixteen of the seventeen flags and the value
    // it showed looked wrong against it.
    const auto& kind = flagCurveKindFor(laneFlag);
    auto blurb = juce::String(kind.flag) + juce::String::fromUTF8("：")
        + juce::String::fromUTF8(kind.label)
        + juce::String::fromUTF8("，范围 ") + juce::String(kind.minimum, 0)
        + juce::String::fromUTF8(" … ") + juce::String(kind.maximum, 0)
        + juce::String::fromUTF8("。\n");
    if (laneFlag == "g")
        blurb += juce::String::fromUTF8(
            "正值下移共振峰（听感偏成熟/厚），负值上移（偏年轻/薄）。\n");
    // Which handle this is.  Curves overlap wherever notes do, so a number on
    // its own does not say which of them was picked up.
    return blurb + juce::String::fromUTF8("当前：第 ") + juce::String(index + 1)
        + juce::String::fromUTF8(" 个标点（共 ") + juce::String(shown.size())
        + juce::String::fromUTF8(" 个），位于音符内 ")
        + juce::String(point.timeSeconds, 3)
        + juce::String::fromUTF8(" 秒，现值 ") + juce::String(point.value, 3)
        + juce::String::fromUTF8("。");
}

void PianoRollComponent::showFlagPointValueDialog(const juce::String& noteId, int index)
{
    const auto* note = findNote(noteId);
    if (note == nullptr || index < 0) return;
    const auto shown = flagLaneCurveFor(*note);
    if (index >= static_cast<int>(shown.size())) return;
    const auto& point = shown[static_cast<std::size_t>(index)];
    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("标点数值"),
        flagPointValueBlurb(noteId, index),
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("value", juce::String(point.value, 3),
                          laneFlag + juce::String::fromUTF8(" 值："));
    if (auto* editor = dialog->getTextEditor("value"))
    {
        editor->setSelectAllWhenFocused(true);
        editor->setInputRestrictions(0, "-0123456789.");
    }
    dialog->addButton(juce::String::fromUTF8("应用"), 1);
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, noteId, index](int result)
            {
                if (safe != nullptr && result == 1)
                    if (const auto* current = safe->findNote(noteId);
                        current != nullptr
                        && index < static_cast<int>(
                               safe->flagLaneCurveFor(*current).size()))
                    {
                        // The shown curve again, so typing a value into the
                        // handle a fresh note starts with writes that handle
                        // rather than falling through to nothing.
                        auto points = safe->flagLaneCurveFor(*current);
                        // The model clamps to the engine's own range, so a
                        // typed 900 lands on 50 rather than being refused.
                        points[static_cast<std::size_t>(index)].value =
                            dialog->getTextEditorContents("value").getFloatValue();
                        safe->model.setNoteUtauFlagCurve(noteId, safe->laneFlag, std::move(points));
                    }
                delete dialog;
            }), false);
}

void PianoRollComponent::showFlagPointBezierDialog(const juce::String& noteId,
                                                   int index)
{
    const auto* note = findNote(noteId);
    if (note == nullptr || index <= 0
        || index >= static_cast<int>(flagCurvePointsFor(*note, laneFlag).size()))
        return;
    const auto& point = flagCurvePointsFor(*note, laneFlag)[static_cast<std::size_t>(index)];
    auto* dialog = new juce::AlertWindow(strings.text("pitchCurve.bezierTitle"),
        juce::String::fromUTF8("归一化控制点。X 是段内的时间位置（0–1），"
                                "Y 是这一段走完的比例；Y 超出 0–1 会先冲过头再回来。"),
        juce::MessageBoxIconType::NoIcon);
    const std::array<std::pair<const char*, float>, 4> fields {{
        { "x1", point.bezierX1 }, { "y1", point.bezierY1 },
        { "x2", point.bezierX2 }, { "y2", point.bezierY2 } }};
    const std::array<const char*, 4> labels { "pitchCurve.bezierX1",
        "pitchCurve.bezierY1", "pitchCurve.bezierX2", "pitchCurve.bezierY2" };
    for (std::size_t at = 0; at < fields.size(); ++at)
    {
        dialog->addTextEditor(fields[at].first, juce::String(fields[at].second, 3),
                              strings.text(labels[at]));
        if (auto* editor = dialog->getTextEditor(fields[at].first))
        {
            editor->setSelectAllWhenFocused(true);
            editor->setInputRestrictions(0, "-0123456789.");
        }
    }
    dialog->addButton(juce::String::fromUTF8("应用"), 1);
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, noteId, index](int result)
            {
                if (safe != nullptr && result == 1)
                    if (const auto* current = safe->findNote(noteId);
                        current != nullptr
                        && index < static_cast<int>(
                               flagCurvePointsFor(*current, safe->laneFlag).size()))
                    {
                        auto points = flagCurvePointsFor(*current, safe->laneFlag);
                        auto& edited = points[static_cast<std::size_t>(index)];
                        edited.shape = PitchCurveShape::customBezier;
                        edited.bezierX1 = dialog->getTextEditorContents("x1").getFloatValue();
                        edited.bezierY1 = dialog->getTextEditorContents("y1").getFloatValue();
                        edited.bezierX2 = dialog->getTextEditorContents("x2").getFloatValue();
                        edited.bezierY2 = dialog->getTextEditorContents("y2").getFloatValue();
                        safe->model.setNoteUtauFlagCurve(noteId, safe->laneFlag, std::move(points));
                    }
                delete dialog;
            }), false);
}

void PianoRollComponent::drawFlagLane(juce::Graphics& g) const
{
    const auto lane = flagLaneBounds();
    const auto plot = flagLanePlotBounds();
    g.setColour(Palette::panel.withAlpha(0.97f));
    g.fillRoundedRectangle(lane, 5.0f);
    g.setColour(Palette::border.brighter(0.18f));
    g.drawRoundedRectangle(lane, 5.0f, 1.2f);

    const auto& kind = flagCurveKindFor(laneFlag);
    const auto [windowLow, windowHigh] = flagLaneWindow();
    // The heading reports the slice on show, not the flag's whole range, so
    // the numbers beside the grid are never a surprise.
    const auto zoomText = laneZoom > 1.0f
        ? juce::String::fromUTF8("  ×") + juce::String(laneZoom, laneZoom < 2.0f ? 1 : 0)
        : juce::String();
    g.setFont(11.0f);
    g.setColour(Palette::text);
    g.drawText(juce::String::fromUTF8("flag 包络 · ") + laneFlag
                   + juce::String::fromUTF8("（") + juce::String::fromUTF8(kind.label)
                   + juce::String::fromUTF8("）  纵向 ")
                   + juce::String(windowLow, 0) + " … " + juce::String(windowHigh, 0)
                   + zoomText
                   + (kind.onsetOnly ? juce::String::fromUTF8("   仅声母区")
                                     : juce::String()),
        lane.toNearestInt().removeFromTop(28).reduced(8, 0).withTrimmedRight(158),
        juce::Justification::centredLeft, false);

    for (const auto zoomIn : { false, true })
    {
        const auto button = flagLaneZoomButtonBounds(zoomIn);
        g.setColour(Palette::panelRaised);
        g.fillRoundedRectangle(button, 3.0f);
        g.setColour(Palette::border.brighter(0.25f));
        g.drawRoundedRectangle(button, 3.0f, 1.0f);
        // Greyed at the ends, so the pair says how far it can still go.
        const auto atEnd = zoomIn ? laneZoom >= 8.0f : laneZoom <= 1.0f;
        g.setColour(atEnd ? Palette::textMuted : Palette::text);
        g.setFont(15.0f);
        g.drawText(amplitudeZoomLabel(zoomIn), button.toNearestInt(),
                   juce::Justification::centred, false);
    }

    // The switch, at the top right of the box.
    const auto switchBounds = flagLaneSwitchBounds();
    g.setColour(Palette::panelRaised);
    g.fillRoundedRectangle(switchBounds, 3.0f);
    g.setColour(Palette::border.brighter(0.25f));
    g.drawRoundedRectangle(switchBounds, 3.0f, 1.0f);
    g.setColour(Palette::text);
    g.setFont(11.0f);
    g.drawText(laneFlag + juce::String::fromUTF8("  ▾"), switchBounds.toNearestInt(),
               juce::Justification::centred, false);

    g.setFont(10.0f);
    const auto windowSpan = windowHigh - windowLow;
    for (auto step = 0; step <= 4; ++step)
    {
        const auto value = windowLow + windowSpan * static_cast<float>(step) / 4.0f;
        const auto y = flagLaneY(value);
        // The line at zero is the one worth finding, when the window holds it.
        g.setColour(std::abs(value) < windowSpan * 0.005f
                        ? juce::Colour(0xff72d6aa).withAlpha(0.36f)
                        : Palette::grid.withAlpha(0.70f));
        g.drawHorizontalLine(static_cast<int>(y), plot.getX(), plot.getRight());
        g.setColour(Palette::textMuted);
        g.drawText(juce::String(value, 0),
                   juce::Rectangle<int>(static_cast<int>(plot.getRight()) + 2,
                                        static_cast<int>(y) - 7, 34, 14),
                   juce::Justification::centredLeft, false);
    }
    // Zero is worth a line of its own when the window has been taken off it.
    if (windowLow < 0.0f && windowHigh > 0.0f)
    {
        g.setColour(juce::Colour(0xff72d6aa).withAlpha(0.36f));
        g.drawHorizontalLine(static_cast<int>(flagLaneY(0.0f)),
                             plot.getX(), plot.getRight());
    }

    auto drawnAny = false;
    for (const auto& hit : noteHits)
    {
        bool utau = false;
        const auto* note = findNote(hit.id, &utau);
        if (note == nullptr || !utau || !note->utauFlagCurveEnabled) continue;
        const auto absoluteStart = hit.startSeconds + hit.clipStartSeconds;
        const auto dragging = dragMode == DragMode::flagPoint && note->id == draggedNote;
        const auto points = dragging ? flagStroke : flagLaneCurveFor(*note);
        if (points.empty()) continue;
        drawnAny = true;
        // A curve reaches exactly as far as the note it belongs to: the engine
        // reads it across that note's rendered stretch and nowhere else.  Held
        // flat before the first handle and after the last, within that stretch,
        // which is what the engine does with the ends.
        const auto [soundStart, soundEnd] = flagLaneDrawnSpan(*note, absoluteStart);
        if (kind.onsetOnly)
        {
            // The onset alone.  What lies past it is shaded, so it is plain
            // that the flat stretch there is out of this flag's hands rather
            // than something that was not drawn yet.
            auto shadeEnd = absoluteStart + note->durationSeconds;
            if (const auto span = utauSoundSpans.find(note->id.toStdString());
                span != utauSoundSpans.end())
                shadeEnd = span->second.second;
            const auto shadeFrom = juce::jlimit(plot.getX(), plot.getRight(),
                                                timeToX(soundEnd));
            const auto shadeTo = juce::jlimit(plot.getX(), plot.getRight(),
                                              timeToX(shadeEnd));
            if (shadeTo > shadeFrom + 0.5f)
            {
                g.setColour(juce::Colours::black.withAlpha(0.20f));
                g.fillRect(juce::Rectangle<float>::leftTopRightBottom(
                    shadeFrom, plot.getY(), shadeTo, plot.getBottom()));
            }
        }
        const auto left = juce::jlimit(plot.getX(), plot.getRight(), timeToX(soundStart));
        const auto right = juce::jlimit(plot.getX(), plot.getRight(), timeToX(soundEnd));
        if (right <= left + 0.5f) continue;
        juce::Path path;
        const auto xAt = [&](double timeSeconds)
        {
            return juce::jlimit(left, right, timeToX(absoluteStart + timeSeconds));
        };
        path.startNewSubPath(left, flagLaneY(points.front().value));
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            // A curved segment is walked rather than jumped, so what is drawn
            // is the shape that will be rendered.  A straight one needs one
            // line, whatever its length.
            if (index > 0 && points[index].shape != PitchCurveShape::linear)
            {
                const auto from = points[index - 1].timeSeconds;
                const auto to = points[index].timeSeconds;
                const auto steps = juce::jlimit(4, 48,
                    static_cast<int>((xAt(to) - xAt(from)) / 3.0f));
                for (auto step = 1; step < steps; ++step)
                {
                    const auto at = from + (to - from) * step / static_cast<double>(steps);
                    path.lineTo(xAt(at), flagLaneY(flagCurveValueAt(points, at)));
                }
            }
            path.lineTo(xAt(points[index].timeSeconds), flagLaneY(points[index].value));
        }
        path.lineTo(right, flagLaneY(points.back().value));
        // Where two notes overlap their curves are two lines of one colour.
        // With a note selected its curve is drawn brighter and heavier and the
        // rest step back, so which one is being edited can be seen before the
        // click rather than after it.  With nothing selected they are all as
        // they were.
        const auto anySelected = !selectedNotes.empty();
        const auto isSelected = selectedNotes.contains(note->id.toStdString());
        g.setColour(juce::Colour(0xffe4b363).withAlpha(
            !anySelected ? 0.92f : (isSelected ? 1.0f : 0.42f)));
        g.strokePath(path, juce::PathStrokeType(
            anySelected && isSelected ? 2.3f : 1.6f));
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            const auto centre = juce::Point<float>(xAt(points[index].timeSeconds),
                                                   flagLaneY(points[index].value));
            if (!plot.expanded(6.0f).contains(centre)) continue;
            const auto held = dragging
                && static_cast<int>(index) == draggedFlagPoint;
            g.setColour(held ? juce::Colours::white : juce::Colour(0xffe4b363));
            g.fillEllipse(centre.x - 3.5f, centre.y - 3.5f, 7.0f, 7.0f);
        }
    }
    if (!drawnAny)
    {
        g.setColour(Palette::textMuted);
        g.setFont(12.0f);
        g.drawText(juce::String::fromUTF8("先选中音符并开启「线性flag」，这里才有曲线可画"
                                           "（开启后每个 flag 都会有一条可拖的水平线）"),
                   plot.toNearestInt(), juce::Justification::centred, false);
    }
}

juce::Rectangle<float> PianoRollComponent::amplitudeLaneBounds() const
{
    auto visible = getLocalBounds().toFloat();
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
        visible = viewport->getViewArea().toFloat();
    const auto height = juce::jlimit(180.0f, 320.0f, visible.getHeight() * 0.46f);
    return juce::Rectangle<float>(visible.getX() + 60.0f,
        visible.getBottom() - height - 4.0f,
        std::max(120.0f, visible.getWidth() - 64.0f), height);
}

juce::Rectangle<float> PianoRollComponent::amplitudeLanePlotBounds() const
{
    auto lane = amplitudeLaneBounds();
    lane.removeFromTop(30.0f);
    lane.removeFromLeft(6.0f);
    lane.removeFromRight(56.0f);
    lane.removeFromBottom(12.0f);
    return lane;
}

float PianoRollComponent::amplitudeLaneY(float gainDb) const
{
    const auto plot = amplitudeLanePlotBounds();
    const auto normalized = juce::jlimit(0.0f, 1.0f,
        amplitudePercentFromDb(gainDb) / amplitudeLaneMaxPercent);
    return plot.getBottom() - normalized * plot.getHeight();
}

float PianoRollComponent::amplitudeDbFromLaneY(float y) const
{
    const auto plot = amplitudeLanePlotBounds();
    const auto normalized = juce::jlimit(0.0f, 1.0f,
        (plot.getBottom() - y) / std::max(1.0f, plot.getHeight()));
    return amplitudeDbFromPercent(normalized * amplitudeLaneMaxPercent);
}

juce::Rectangle<float> PianoRollComponent::amplitudeZoomButtonBounds(bool zoomIn) const
{
    const auto lane = amplitudeLaneBounds();
    return { lane.getRight() - (zoomIn ? 25.0f : 49.0f),
             lane.getY() + 4.0f, 21.0f, 21.0f };
}

float PianoRollComponent::amplitudePercentFromDb(float gainDb)
{
    return gainDb <= -59.9f ? 0.0f : std::pow(10.0f, gainDb / 20.0f) * 100.0f;
}

float PianoRollComponent::amplitudeDbFromPercent(float percent)
{
    if (percent <= 0.05f) return -60.0f;
    return juce::jlimit(-60.0f, 12.0f,
        20.0f * std::log10(percent / 100.0f));
}

std::vector<AmplitudeEnvelopePoint> PianoRollComponent::mapAmplitudeEnvelopeToNote(
    const std::vector<AmplitudeEnvelopePoint>& source,
    const juce::String& sourceNoteId, const juce::String& targetNoteId) const
{
    if (source.empty() || sourceNoteId == targetNoteId) return source;
    bool utau = false;
    const auto* targetNote = findNote(targetNoteId, &utau);
    const auto targetHit = std::find_if(noteHits.begin(), noteHits.end(),
        [&](const auto& hit) { return hit.id == targetNoteId; });
    if (targetNote == nullptr || !utau || targetHit == noteHits.end()) return {};

    const auto targetStart = targetHit->startSeconds + targetHit->clipStartSeconds;
    const auto targetDefault = amplitudeEnvelopeFor(*targetNote, targetStart);
    if (targetDefault.size() < 2) return {};
    const auto sourceFirst = source.front().timeSeconds;
    const auto sourceSpan = std::max(1.0e-6,
        source.back().timeSeconds - sourceFirst);
    const auto targetFirst = targetDefault.front().timeSeconds;
    const auto targetSpan = std::max(1.0e-6,
        targetDefault.back().timeSeconds - targetFirst);

    std::vector<AmplitudeEnvelopePoint> mapped;
    mapped.reserve(source.size());
    for (const auto& point : source)
    {
        const auto fraction = juce::jlimit(0.0, 1.0,
            (point.timeSeconds - sourceFirst) / sourceSpan);
        mapped.push_back({ targetFirst + fraction * targetSpan, point.gainDb });
    }
    return mapped;
}

void PianoRollComponent::commitAmplitudeEnvelopeToSelection(
    const juce::String& sourceNoteId,
    const std::vector<AmplitudeEnvelopePoint>& source)
{
    std::vector<juce::String> ids;
    ids.reserve(selectedNotes.size() + 1);
    for (const auto& id : selectedNotes)
        ids.push_back(juce::String::fromUTF8(id.c_str()));
    if (std::find(ids.begin(), ids.end(), sourceNoteId) == ids.end())
        ids.push_back(sourceNoteId);

    std::vector<std::pair<juce::String, std::vector<AmplitudeEnvelopePoint>>> edits;
    edits.reserve(ids.size());
    for (const auto& id : ids)
    {
        bool utau = false;
        if (findNote(id, &utau) == nullptr || !utau) continue;
        auto mapped = mapAmplitudeEnvelopeToNote(source, sourceNoteId, id);
        // What was dragged is the shape with the base already in it; the note
        // stores the shape without it, or the base would be multiplied twice.
        if (const auto* target = findNote(id); target != nullptr)
            mapped = unscaledAmplitudeEnvelope(mapped,
                                               target->amplitudeEnvelopeBasePercent);
        if (mapped.size() >= 2) edits.emplace_back(id, std::move(mapped));
    }
    model.setNotesAmplitudeEnvelopes(std::move(edits));
}

juce::PopupMenu PianoRollComponent::buildPitchCurveShapeMenu(
    const juce::String& noteId, int anchorIndex)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return {};
    auto& anchors = pitchAnchorsFor(*note);
    const auto validAnchor = anchorIndex >= 0
        && anchorIndex < static_cast<int>(anchors.size());
    const auto hasIncoming = validAnchor && anchorIndex > 0;

    juce::PopupMenu menu;
    menu.addSectionHeader(strings.text("pitchCurve.incomingSegment"));
    if (!hasIncoming)
        menu.addItem(1, strings.text("pitchCurve.noPrevious"), false, false);
    else
    {
        const auto current = anchors[static_cast<std::size_t>(anchorIndex)].shape;
        menu.addItem(1, strings.text("pitchCurve.natural"), true,
                     current == PitchCurveShape::natural);
        menu.addItem(2, strings.text("pitchCurve.linear"), true,
                     current == PitchCurveShape::linear);
        menu.addItem(3, strings.text("pitchCurve.smooth"), true,
                     current == PitchCurveShape::smooth);
        menu.addItem(4, strings.text("pitchCurve.easeIn"), true,
                     current == PitchCurveShape::easeIn);
        menu.addItem(5, strings.text("pitchCurve.easeOut"), true,
                     current == PitchCurveShape::easeOut);
        menu.addSeparator();
        menu.addItem(6, strings.text("pitchCurve.customBezier"), true,
                     current == PitchCurveShape::customBezier);
    }
    // Snapping is about the point itself rather than the segment leading into
    // it, so it stays available on the first point, where there is no segment.
    const auto alreadyOnNote = validAnchor
        && std::abs(anchors[static_cast<std::size_t>(anchorIndex)].targetMidi
                    - note->midiNote) < 1.0e-3f;
    menu.addSeparator();
    menu.addItem(7, strings.text("pitchCurve.snapToNote"),
                 validAnchor && !alreadyOnNote, false);
    // Typing the pitch in, for when the row a point has to land on is known
    // as a number rather than as somewhere to drag to.  Like snapping, this
    // is about the point and not the segment, so the first point has it too.
    menu.addItem(9, strings.text("pitchCurve.setFrequency"), validAnchor, false);
    // Only an interior point can be removed: the line has to be handed to a
    // neighbour on each side, and the two endpoints are what tie the curve to
    // the start and end of the note.
    const auto interior = validAnchor && anchorIndex > 0
        && anchorIndex + 1 < static_cast<int>(anchors.size());
    menu.addItem(8, strings.text("pitchCurve.deletePoint"), interior, false);
    return menu;
}

namespace
{
// The ids in a menu, in the order they are offered.
std::vector<int> menuIds(const juce::PopupMenu& menu, bool enabledOnly)
{
    std::vector<int> ids;
    juce::PopupMenu::MenuItemIterator walk(menu);
    while (walk.next())
    {
        const auto& item = walk.getItem();
        if (item.itemID == 0) continue;
        if (enabledOnly && !item.isEnabled) continue;
        ids.push_back(item.itemID);
    }
    return ids;
}
}

bool PianoRollComponent::anchorMenuChoiceHandled(int choice)
{
    return choice >= 1 && choice <= 9;
}

std::vector<int> PianoRollComponent::diagnosticAnchorMenuIds(
    const juce::String& noteId, int anchorIndex)
{
    return menuIds(buildPitchCurveShapeMenu(noteId, anchorIndex), false);
}

std::vector<int> PianoRollComponent::diagnosticEnabledAnchorMenuIds(
    const juce::String& noteId, int anchorIndex)
{
    return menuIds(buildPitchCurveShapeMenu(noteId, anchorIndex), true);
}

void PianoRollComponent::showPitchCurveShapeMenu(const juce::String& noteId,
                                                  int anchorIndex,
                                                  juce::Point<int> screenPosition)
{
    auto menu = buildPitchCurveShapeMenu(noteId, anchorIndex);
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
        { screenPosition.x, screenPosition.y, 1, 1 }),
        [safe, noteId, anchorIndex](int result)
        {
            if (safe == nullptr || !anchorMenuChoiceHandled(result)) return;
            if (result == 6)
            {
                safe->showCustomBezierDialog(noteId, anchorIndex);
                return;
            }
            if (result == 9)
            {
                safe->showAnchorFrequencyDialog(noteId, anchorIndex);
                return;
            }
            const auto* currentNote = safe->findNote(noteId);
            if (currentNote == nullptr) return;
            auto anchorsCopy = safe->pitchAnchorsFor(*currentNote);
            if (anchorIndex < 0
                || anchorIndex >= static_cast<int>(anchorsCopy.size())) return;
            if (result == 7)
            {
                anchorsCopy[static_cast<std::size_t>(anchorIndex)].targetMidi =
                    currentNote->midiNote;
                safe->model.setNotePitchCurve(noteId, std::move(anchorsCopy), true);
                return;
            }
            if (anchorIndex == 0) return;
            if (result == 8)
            {
                // The following point keeps its own segment shape, which now
                // describes the span from the previous point instead.
                if (anchorIndex + 1 >= static_cast<int>(anchorsCopy.size())) return;
                anchorsCopy.erase(anchorsCopy.begin() + anchorIndex);
                safe->model.setNotePitchCurve(noteId, std::move(anchorsCopy), true);
                return;
            }
            anchorsCopy[static_cast<std::size_t>(anchorIndex)].shape =
                result == 2 ? PitchCurveShape::linear
                : result == 3 ? PitchCurveShape::smooth
                : result == 4 ? PitchCurveShape::easeIn
                : result == 5 ? PitchCurveShape::easeOut
                              : PitchCurveShape::natural;
            safe->model.setNotePitchCurve(noteId, std::move(anchorsCopy), true);
        });
}

void PianoRollComponent::flattenPitchLine(const juce::String& noteId)
{
    // The model does the whole thing in one step: setNotePitchCurve alone was
    // not enough, because it leaves the measured curve as it found it and the
    // renderer shifts away from that.
    model.flattenNotePitch(chosenNoteIds(noteId));
    // The cached anchors were derived from the old line.
    pitchAnchorCache.clear();
    repaint();
}

void PianoRollComponent::showTransposeDialog(const juce::String& noteId)
{
    std::vector<juce::String> targets;
    for (const auto& id : selectedNotes) targets.push_back(juce::String::fromUTF8(id.c_str()));
    if (targets.empty()) targets.push_back(noteId);
    if (targets.empty() || targets.front().isEmpty()) return;

    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("音高调整"),
        juce::String::fromUTF8(
            "按半音平移选中的音符（12 平均律）。\n"
            "只改音高，不改时间；音高标点跟着一起移动。\n"
            "两个框都填时按两者之差平移。"),
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("up", "0",
                          juce::String::fromUTF8("上移（半音）："));
    dialog->addTextEditor("down", "0",
                          juce::String::fromUTF8("下移（半音）："));
    for (const auto* key : { "up", "down" })
        if (auto* editor = dialog->getTextEditor(key))
        {
            editor->setInputRestrictions(4, "0123456789");
            editor->setSelectAllWhenFocused(true);
        }
    dialog->addButton(juce::String::fromUTF8("应用"), 1);
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, targets](int result)
            {
                if (safe != nullptr && result == 1)
                {
                    const auto up = dialog->getTextEditorContents("up").getIntValue();
                    const auto down = dialog->getTextEditorContents("down").getIntValue();
                    safe->model.transposeNotes(targets, static_cast<float>(up - down));
                }
                delete dialog;
            }), false);
}

void PianoRollComponent::deleteSelectedNotes()
{
    std::vector<juce::String> removed;
    removed.reserve(selectedNotes.size());
    for (const auto& id : selectedNotes) removed.push_back(juce::String::fromUTF8(id.c_str()));
    if (removed.empty()) removed.push_back(selectedNote);
    if (removed.empty() || removed.front().isEmpty()) return;
    selectedNote.clear();
    selectedNotes.clear();
    model.removeNotes(removed);
    if (onNoteSelected) onNoteSelected({});
}

namespace
{
// The note menu, in the order it is shown.  6 and 7 are the two faces of one
// item, so both carry the same answer.
enum class NoteMenuScope { both, utauOnly, plainOnly };
struct NoteMenuEntry { int id; NoteMenuScope scope; };
using Scope = NoteMenuScope;
const std::array<NoteMenuEntry, 22> noteMenuEntries {{
    {  1, Scope::both      },   // 分割音符
    {  2, Scope::both      },   // 合并音符
    { 20, Scope::both      },   // 强制连接（保留各音符数据）
    { 23, Scope::both      },   // 包络基础值… -- 缩放整条响度包络，任意轨道可用
    { 24, Scope::utauOnly  },   // 添加拼字音符 -- 前置一个只有引导声的音符
    {  3, Scope::utauOnly  },   // 时序…             -- from the voicebank entry
    { 19, Scope::utauOnly  },   // 修改 STP…         -- moves that entry in the wav
    {  4, Scope::utauOnly  },   // 区域编辑器…       -- refuses off UTAU anyway
    {  5, Scope::both      },   // 颤音…             -- target pitch layer for every track
    {  6, Scope::both      },   // 改为真实颤音线
    {  7, Scope::both      },   // 退回为标准颤音模式
    {  8, Scope::both      },   // 修改颤音为音高标点… -- bakes into the anchor line
    {  9, Scope::utauOnly  },   // flag 拆分…        -- needs the four regions
    { 14, Scope::utauOnly  },   // 重置线性flag
    { 12, Scope::utauOnly  },   // 辅音强制重置      -- a pinned preutterance is UTAU's
    { 13, Scope::both      },   // 时序删除          -- removes notes and closes the gap
    { 15, Scope::utauOnly  },   // 添加空格
    { 16, Scope::utauOnly  },   // 添加指定长度空格…
    { 17, Scope::utauOnly  },   // 批量输入歌词…     -- a lyric names a sample
    // Where the anchor line is the pitch line and there is no vibrato or
    // voicebank to fall back on, laying it flat is the way back.  In UTAU the
    // same thing is reached through the anchors and the vibrato items.
    { 18, Scope::plainOnly },   // 初始化音高线
    { 11, Scope::both      },   // 音高调整…
    { 10, Scope::both      },   // 删除音符
}};
}

std::vector<int> PianoRollComponent::noteMenuItemsFor(bool utauTrack)
{
    std::vector<int> items;
    items.reserve(noteMenuEntries.size());
    for (const auto& entry : noteMenuEntries)
        if (entry.scope == NoteMenuScope::both
            || (utauTrack ? entry.scope == NoteMenuScope::utauOnly
                          : entry.scope == NoteMenuScope::plainOnly))
            items.push_back(entry.id);
    return items;
}

PianoRollComponent::NoteMenu PianoRollComponent::buildNoteMenu(
    const juce::String& noteId) const
{
    NoteMenu built;
    const auto* note = findNote(noteId);
    if (note == nullptr) return built;

    std::optional<backend::UtauSampleTiming> noteTiming;
    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        auto belongsToTrack = false;
        for (const auto& clip : track.clips)
            if (std::any_of(clip.notes.begin(), clip.notes.end(),
                [&](const auto& candidate) { return candidate.id == noteId; }))
                belongsToTrack = true;
        if (!belongsToTrack) continue;
        const auto velocity = note->utauConsonantVelocity
                != inheritedUtauConsonantVelocity
            ? note->utauConsonantVelocity : track.utauConsonantVelocity;
        noteTiming = backend::UtauRenderer::sampleTiming(
            track.voicebankDirectory, note->label, note->midiNote, velocity,
            utauModeUsesRegions(track.utauMode),
            track.utauMode == UtauMode::mou);
        if (noteTiming)
        {
            if (note->utauPreutteranceOverrideEnabled)
                noteTiming->preutteranceSeconds = note->utauPreutteranceSeconds;
            if (note->utauOverlapOverrideEnabled)
                noteTiming->overlapSeconds = note->utauOverlapSeconds;
        }
        break;
    }

    // Editing uses the project's smallest stretch unit, which follows tempo
    // and time signature rather than being a fixed musical note value.
    const auto quantum = noteEditQuantumSeconds();
    const auto duration = note->durationSeconds;
    const auto canSplit = duration + 1.0e-8 >= quantum * 2.0;

    auto split = duration * 0.5;
    if (canSplit)
    {
        // Ties round toward a longer first half.  The second half uses the
        // exact remainder, so the original note end can never drift.
        split = std::ceil((duration * 0.5) / quantum - 1.0e-9) * quantum;
        split = juce::jlimit(quantum, duration - quantum, split);
    }

    std::vector<juce::String> mergeIds;
    mergeIds.reserve(selectedNotes.size());
    for (const auto& id : selectedNotes)
        mergeIds.push_back(juce::String::fromUTF8(id.c_str()));
    const auto allInOneClip = [&]
    {
        if (mergeIds.size() < 2) return false;
        for (const auto& track : snapshot.tracks)
            for (const auto& clip : track.clips)
            {
                auto found = std::size_t{};
                for (const auto& candidate : clip.notes)
                    if (std::find(mergeIds.begin(), mergeIds.end(), candidate.id)
                        != mergeIds.end())
                        ++found;
                if (found == mergeIds.size()) return true;
            }
        return false;
    }();

    const auto utauTrack = isUtauNote(noteId);
    const auto offered = noteMenuItemsFor(utauTrack);
    const auto wanted = [&offered](int id)
    {
        return std::find(offered.begin(), offered.end(), id) != offered.end();
    };

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String::fromUTF8("音符"));
    if (wanted(1))
        menu.addItem(1, juce::String::fromUTF8("分割音符"), canSplit, false);
    if (wanted(2))
        menu.addItem(2, juce::String::fromUTF8("合并音符"), allInOneClip, false);
    if (wanted(20))
        menu.addItem(20, juce::String::fromUTF8("强制连接（保留音符数据）"),
                     selectedNotes.size() >= 2, false);
    if (wanted(23))
        menu.addItem(23, juce::String::fromUTF8("包络基础值…"), true, false);
    if (wanted(24))
        menu.addItem(24, juce::String::fromUTF8("添加拼字音符"),
                     selectedNotes.size() == 1, false);
    const auto canEditTiming = noteTiming.has_value() && selectedNotes.size() == 1;
    if (wanted(3))
        menu.addItem(3, juce::String::fromUTF8("时序…"), canEditTiming, false);
    // Offered whether or not the lyric resolves to an entry: a note may be
    // carrying an STP from a lyric it used to have, and refusing to open
    // would be the only thing standing between the user and clearing it.
    if (wanted(19))
        menu.addItem(19, juce::String::fromUTF8("修改 STP…"), true, false);
    if (wanted(4))
        menu.addItem(4, juce::String::fromUTF8("区域编辑器…  Ctrl+G"),
                     onOpenRegionEditor != nullptr && selectedNotes.size() == 1, false);
    const auto* vibratoNote = findNote(noteId);
    const auto hasVibrato = vibratoNote != nullptr && vibratoNote->vibratoEnabled;
    if (wanted(5))
        menu.addItem(5, juce::String::fromUTF8("颤音…"), vibratoNote != nullptr,
                     hasVibrato);
    if (wanted(6) && wanted(7))
    {
        if (hasVibrato && vibratoNote->vibratoRealLine)
            menu.addItem(7, juce::String::fromUTF8("退回为标准颤音模式"), true, false);
        else
            menu.addItem(6, juce::String::fromUTF8("改为真实颤音线"), hasVibrato, false);
    }
    if (wanted(8))
        menu.addItem(8, juce::String::fromUTF8("修改颤音为音高标点…"), hasVibrato, false);
    // Splitting flags only means anything where the note has four regions.
    auto fourRegionTrack = false;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& candidate : clip.notes)
                if (candidate.id == noteId)
                    fourRegionTrack = track.pitchAlgorithm == PitchAlgorithm::utau
                        && utauModeUsesRegions(track.utauMode);
    const auto* flagNote = findNote(noteId);
    if (wanted(9))
        menu.addItem(9, juce::String::fromUTF8("flag 拆分…"), fourRegionTrack,
                     flagNote != nullptr && flagNote->utauFlagSplit);
    if (wanted(14))
        menu.addItem(14, juce::String::fromUTF8("重置线性flag"),
                     flagResetAvailable(chosenNoteIds(noteId)), false);
    if (wanted(12))
        menu.addItem(12, juce::String::fromUTF8("辅音强制重置"), true, false);
    if (wanted(13))
        menu.addItem(13, juce::String::fromUTF8("时序删除"), true, false);
    // A silence opened in front of the note, on UTAU tracks in every mode.
    if (wanted(15))
        menu.addItem(15, juce::String::fromUTF8("添加空格"), true, false);
    if (wanted(16))
        menu.addItem(16, juce::String::fromUTF8("添加指定长度空格…"), true, false);
    if (wanted(17))
        menu.addItem(17, juce::String::fromUTF8("批量输入歌词…"),
                     !batchLyricTargets(noteId).empty(), false);
    if (wanted(18))
        menu.addItem(18, juce::String::fromUTF8("初始化音高线"), true, false);
    if (wanted(11))
        menu.addItem(11, juce::String::fromUTF8("音高调整…"), true, false);
    menu.addSeparator();
    if (wanted(10))
        menu.addItem(10, juce::String::fromUTF8("删除音符  Delete"), true, false);

    built.menu = std::move(menu);
    built.splitSeconds = split;
    built.mergeIds = std::move(mergeIds);
    return built;
}

std::vector<int> PianoRollComponent::diagnosticNoteMenuIds(
    const juce::String& noteId) const
{
    std::vector<int> ids;
    auto built = buildNoteMenu(noteId);
    juce::PopupMenu::MenuItemIterator walk(built.menu);
    while (walk.next())
        if (walk.getItem().itemID != 0) ids.push_back(walk.getItem().itemID);
    return ids;
}

void PianoRollComponent::showNoteContextMenu(const juce::String& noteId,
                                              juce::Point<int> screenPosition)
{
    auto built = buildNoteMenu(noteId);
    const auto split = built.splitSeconds;
    auto mergeIds = std::move(built.mergeIds);
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    built.menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
        { screenPosition.x, screenPosition.y, 1, 1 }),
        [safe, noteId, split, mergeIds = std::move(mergeIds)](int result)
        {
            if (safe == nullptr) return;
            if (result == 3)
            {
                safe->showNoteTimingDialog(noteId);
                return;
            }
            if (result == 19)
            {
                safe->showStpDialog(noteId);
                return;
            }
            if (result == 4)
            {
                if (safe->onOpenRegionEditor) safe->onOpenRegionEditor(noteId);
                return;
            }
            if (result == 5)
            {
                safe->showVibratoDialog(noteId);
                return;
            }
            if (result == 6 || result == 7)
            {
                safe->model.setNotesVibratoRealLine(safe->chosenNoteIds(noteId),
                                                    result == 6);
                return;
            }
            if (result == 8)
            {
                safe->confirmBakeVibrato(noteId);
                return;
            }
            if (result == 14)
            {
                safe->model.resetNotesUtauFlagCurves(safe->chosenNoteIds(noteId));
                return;
            }
            if (result == 17)
            {
                safe->showBatchLyricDialog(noteId);
                return;
            }
            if (result == 12)
            {
                safe->resetConsonant(noteId);
                return;
            }
            if (result == 13)
            {
                safe->deleteSelectedNotesRippling(noteId);
                return;
            }
            if (result == 9)
            {
                safe->showRegionFlagDialog(noteId);
                return;
            }
            if (result == 18)
            {
                safe->flattenPitchLine(noteId);
                return;
            }
            if (result == 11)
            {
                safe->showTransposeDialog(noteId);
                return;
            }
            if (result == 15)
            {
                safe->model.insertGapBeforeNote(noteId,
                    safe->quarterBarSecondsAt(
                        std::max(0.0, safe->absoluteStartOf(noteId))));
                return;
            }
            if (result == 16)
            {
                safe->showGapDialog(noteId);
                return;
            }
            if (result == 10)
            {
                // Right-clicking already put this note in the selection, so
                // this removes exactly what Delete would have.
                safe->deleteSelectedNotes();
                return;
            }
            if (result == 20)
            {
                safe->model.setNotesConnection(safe->chosenNoteIds(noteId), true);
                return;
            }
            if (result == 23)
            {
                safe->showEnvelopeBaseDialog(noteId);
                return;
            }
            if (result == 24)
            {
                const auto prefix = safe->model.insertPrefixNote(noteId,
                    safe->effectiveUtauOverlapFor(noteId));
                if (prefix.isEmpty()) return;
                safe->selectedNote = prefix;
                safe->selectedNotes.clear();
                safe->selectedNotes.insert(prefix.toStdString());
                if (safe->onNoteSelected) safe->onNoteSelected(prefix);
                safe->repaint();
                return;
            }
            if (result != 1 && result != 2) return;
            const auto resultingId = result == 1
                ? safe->model.splitNote(noteId, split)
                : safe->model.mergeNotes(mergeIds);
            if (resultingId.isEmpty()) return;
            safe->selectedNote = resultingId;
            safe->selectedNotes.clear();
            safe->selectedNotes.insert(resultingId.toStdString());
            if (safe->onNoteSelected) safe->onNoteSelected(resultingId);
            safe->repaint();
        });
}

std::vector<PianoRollComponent::RegionFlagField>
PianoRollComponent::regionFlagFieldsFor(const juce::String& noteId) const
{
    std::vector<RegionFlagField> fields;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (note.id != noteId) continue;
                const std::array<juce::String, 4> stored {
                    note.utauRegionFlags1, note.utauRegionFlags2,
                    note.utauRegionFlags3, note.utauRegionFlags4 };
                // 界 is always the four parts of a Chinese syllable.  谋 says
                // per entry how many regions there are and lets any of them be
                // a consonant, so the boxes follow the entry and are named by
                // position -- the same names the wave and the tick boxes use.
                const auto mou = track.utauMode == UtauMode::mou;
                auto count = 4;
                if (mou)
                {
                    const auto velocity =
                        note.utauConsonantVelocity != inheritedUtauConsonantVelocity
                        ? note.utauConsonantVelocity : track.utauConsonantVelocity;
                    const auto timing = backend::UtauRenderer::sampleTiming(
                        track.voicebankDirectory, note.label, note.midiNote,
                        velocity, true, true);
                    if (timing)
                        count = SampleSettings::mouRegionCount(timing->mouClasses);
                }
                static const std::array<const char*, 4> jieNames {
                    "\xe5\xa3\xb0\xe6\xaf\x8d", "\xe4\xbb\x8b\xe9\x9f\xb3",
                    "\xe9\x9f\xb5\xe8\x85\xb9", "\xe9\x9f\xb5\xe5\xb0\xbe" };
                for (int index = 0; index < count; ++index)
                {
                    const auto name = mou
                        ? juce::String::fromUTF8("\xe7\xac\xac")
                              + juce::String(index + 1)
                              + juce::String::fromUTF8("\xe5\x8c\xba")
                        : juce::String::fromUTF8(
                              jieNames[static_cast<std::size_t>(index)]);
                    fields.push_back({ "r" + juce::String(index + 1),
                                       name + juce::String::fromUTF8("\xef\xbc\x9a"),
                                       stored[static_cast<std::size_t>(index)] });
                }
                return fields;
            }
    return fields;
}

juce::StringArray PianoRollComponent::diagnosticRegionFlagFields(
    const juce::String& noteId) const
{
    juce::StringArray shown;
    for (const auto& field : regionFlagFieldsFor(noteId))
        shown.add(field.label + field.value);
    return shown;
}

double PianoRollComponent::absoluteStartOf(const juce::String& noteId) const
{
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == noteId) return clip.startSeconds + note.startSeconds;
    return -1.0;
}

std::optional<PianoRollComponent::GapInfo> PianoRollComponent::gapAt(
    juce::Point<float> position) const
{
    if (sourceEditMode) return std::nullopt;
    const auto seconds = static_cast<double>(position.x - 58.0f) / pixelsPerSecond;
    if (seconds < 0.0) return std::nullopt;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose || track.pitchAlgorithm != PitchAlgorithm::utau
            || (focusedTrack.isNotEmpty() && track.id != focusedTrack))
            continue;
        for (const auto& clip : track.clips)
        {
            // In order, and by where each note really ends: notes can overlap,
            // so the silence begins after the furthest end so far rather than
            // after the previous note's own.
            std::vector<const NoteData*> ordered;
            for (const auto& note : clip.notes) ordered.push_back(&note);
            std::sort(ordered.begin(), ordered.end(),
                      [](const auto* left, const auto* right)
                      {
                          return left->startSeconds < right->startSeconds;
                      });
            auto reach = -1.0;
            const NoteData* previous = nullptr;
            for (const auto* note : ordered)
            {
                const auto start = clip.startSeconds + note->startSeconds;
                if (previous != nullptr && start > reach + 1.0e-6
                    && seconds >= reach && seconds < start)
                    return GapInfo { clip.id, previous->id, note->id, reach, start,
                                     previous->midiNote };
                const auto end = start + note->durationSeconds;
                if (end > reach) { reach = end; previous = note; }
            }
        }
    }
    return std::nullopt;
}

juce::String PianoRollComponent::insertNoteIntoGap(const GapInfo& gap)
{
    const auto length = quarterBarSecondsAt(gap.fromSeconds);
    // The silence opens in front of the note that follows, which carries it
    // and everything after later; the new note goes in the space that leaves,
    // starting where the silence did.
    model.insertGapBeforeNote(gap.nextId, length);
    return model.addNote(gap.clipId, gap.fromSeconds, length, gap.midi);
}

void PianoRollComponent::showGapContextMenu(const GapInfo& gap,
                                            juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    menu.addItem(1, juce::String::fromUTF8("删除空格"), true, false);
    menu.addItem(2, juce::String::fromUTF8("插入四分之一小节音符"), true, false);
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(
        { screenPosition.x, screenPosition.y, 1, 1 }),
        [safe, gap](int result)
        {
            if (safe == nullptr || result == 0) return;
            if (result == 1)
                safe->model.closeGapBeforeNote(gap.nextId,
                                               gap.toSeconds - gap.fromSeconds);
            else if (result == 2)
            {
                const auto id = safe->insertNoteIntoGap(gap);
                if (id.isNotEmpty())
                {
                    safe->selectedNote = id;
                    safe->selectedNotes.clear();
                    safe->selectedNotes.insert(id.toStdString());
                    if (safe->onNoteSelected) safe->onNoteSelected(id);
                }
            }
        });
}

bool PianoRollComponent::isUtauNote(const juce::String& noteId) const
{
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == noteId)
                    return track.pitchAlgorithm == PitchAlgorithm::utau;
    return false;
}

double PianoRollComponent::quarterBarSecondsAt(double seconds) const
{
    const auto beat = 60.0 / juce::jlimit(20.0, 400.0, snapshot.tempoAtSeconds(seconds));
    const auto bar = beat * static_cast<double>(snapshot.numerator) * 4.0
        / static_cast<double>(std::max(1, snapshot.denominator));
    return std::max(1.0e-6, bar / 4.0);
}

double PianoRollComponent::gapUnitSecondsAt(double seconds) const
{
    // A 128th of a quarter note.  The tempo is quarter notes a minute, so a
    // beat here is one of those whatever a bar happens to hold.
    const auto beat = 60.0 / juce::jlimit(20.0, 400.0, snapshot.tempoAtSeconds(seconds));
    return std::max(1.0e-9, beat / 128.0);
}

juce::StringArray PianoRollComponent::splitBatchLyrics(const juce::String& text)
{
    juce::StringArray words;
    juce::String current;
    for (auto character = text.getCharPointer(); !character.isEmpty(); ++character)
    {
        const auto letter = *character;
        // Space, tab and newline, and the full-width space U+3000, which is
        // what an input method set to Chinese or Japanese types.  Reading only
        // the ASCII one would put a whole line onto the first note.
        const auto separates = letter == ' ' || letter == '\t' || letter == '\n'
            || letter == '\r' || letter == juce::juce_wchar(0x3000);
        if (separates)
        {
            if (current.isNotEmpty()) words.add(current);
            current.clear();
            continue;
        }
        current += juce::String::charToString(letter);
    }
    if (current.isNotEmpty()) words.add(current);
    return words;
}

std::vector<juce::String> PianoRollComponent::batchLyricTargets(
    const juce::String& noteId) const
{
    // Several selected: fill exactly those, in the order they are sung.
    if (selectedNotes.size() > 1)
    {
        std::vector<const NoteData*> chosen;
        std::vector<std::pair<double, const NoteData*>> ordered;
        for (const auto& track : snapshot.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (selectedNotes.contains(note.id.toStdString()))
                        ordered.emplace_back(clip.startSeconds + note.startSeconds, &note);
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto& left, const auto& right)
                         { return left.first < right.first; });
        std::vector<juce::String> ids;
        ids.reserve(ordered.size());
        for (const auto& [when, note] : ordered) ids.push_back(note->id);
        return ids;
    }
    // Otherwise this note and everything after it in the same clip, so one
    // click and one line of text fills a phrase.
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
        {
            std::vector<std::pair<double, juce::String>> ordered;
            auto found = false;
            for (const auto& note : clip.notes)
            {
                ordered.emplace_back(note.startSeconds, note.id);
                if (note.id == noteId) found = true;
            }
            if (!found) continue;
            std::stable_sort(ordered.begin(), ordered.end(),
                             [](const auto& left, const auto& right)
                             { return left.first < right.first; });
            std::vector<juce::String> ids;
            auto reached = false;
            for (const auto& [when, id] : ordered)
            {
                if (id == noteId) reached = true;
                if (reached) ids.push_back(id);
            }
            return ids;
        }
    return {};
}

void PianoRollComponent::showBatchLyricDialog(const juce::String& noteId)
{
    const auto targets = batchLyricTargets(noteId);
    if (targets.empty()) return;
    // Title and the box, nothing else: the instructions ran longer than the
    // thing they explained, and the editor is the only place to type anyway.
    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("批量输入歌词"), {},
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("lyrics", {}, juce::String::fromUTF8("歌词"));
    if (auto* editor = dialog->getTextEditor("lyrics"))
    {
        editor->setSelectAllWhenFocused(true);
        editor->setEscapeAndReturnKeysConsumed(false);
    }
    dialog->addButton(juce::String::fromUTF8("填入"), 1,
                      juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, targets](int result)
            {
                if (safe != nullptr && result == 1)
                {
                    const auto words = splitBatchLyrics(
                        dialog->getTextEditorContents("lyrics"));
                    std::vector<std::pair<juce::String, juce::String>> labels;
                    const auto count = std::min(static_cast<std::size_t>(words.size()),
                                                targets.size());
                    labels.reserve(count);
                    for (std::size_t index = 0; index < count; ++index)
                        labels.emplace_back(targets[index],
                                            words[static_cast<int>(index)]);
                    // Fewer words than notes leaves the rest alone; more words
                    // than notes drops the extras.  Neither is an error worth
                    // stopping for -- the person can see what happened.
                    safe->model.setNoteLabels(labels);
                }
                delete dialog;
            }), false);
}

void PianoRollComponent::showGapDialog(const juce::String& noteId)
{
    const auto at = absoluteStartOf(noteId);
    if (at < 0.0) return;
    const auto unit = gapUnitSecondsAt(at);
    // It opens at the one-click amount, so the dialog can also say what that
    // amount is in the units it counts in.
    const auto preset = juce::jmax(1, juce::roundToInt(quarterBarSecondsAt(at) / unit));
    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("添加指定长度空格"),
        juce::String::fromUTF8("在该音符前插入一段空白，该轨道之后的音符整体后移。\n"
                               "单位：1 = 四分音符的 1/128；四分之一小节 = ")
            + juce::String(preset) + juce::String::fromUTF8("。"),
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("units", juce::String(preset),
                          juce::String::fromUTF8("长度（单位）"));
    if (auto* editor = dialog->getTextEditor("units"))
    {
        editor->setInputRestrictions(7, "0123456789");
        editor->setSelectAllWhenFocused(true);
    }
    dialog->addButton(juce::String::fromUTF8("添加"), 1,
                      juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, noteId, unit](int result)
            {
                if (safe != nullptr && result == 1)
                {
                    const auto units = juce::jmax(0,
                        dialog->getTextEditorContents("units").getIntValue());
                    if (units > 0)
                        safe->model.insertGapBeforeNote(noteId, units * unit);
                }
                delete dialog;
            }), false);
}

double PianoRollComponent::effectiveUtauOverlapFor(const juce::String& noteId) const
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return 0.0;
    if (note->utauOverlapOverrideEnabled) return note->utauOverlapSeconds;
    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        for (const auto& clip : track.clips)
            for (const auto& candidate : clip.notes)
            {
                if (candidate.id != noteId) continue;
                const auto velocity = note->utauConsonantVelocity
                        != inheritedUtauConsonantVelocity
                    ? note->utauConsonantVelocity : track.utauConsonantVelocity;
                if (const auto timing = backend::UtauRenderer::sampleTiming(
                        track.voicebankDirectory, note->label, note->midiNote,
                        velocity, utauModeUsesRegions(track.utauMode),
                        track.utauMode == UtauMode::mou))
                    return timing->overlapSeconds;
                return 0.0;
            }
    }
    return 0.0;
}

void PianoRollComponent::showEnvelopeBaseDialog(const juce::String& noteId)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    const auto targets = chosenNoteIds(noteId);
    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("包络基础值"),
        juce::String::fromUTF8(
            "把这个音符的整条响度包络按比例整体升降，不改变它的形状。\n"
            "100 是包络本身，200 是两倍高（更响），110 是 1.1 倍，0 是静音。\n"
            "没有画过包络的音符，隐含的那条 100% 平线一样会被抬起来。"),
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("base",
                          juce::String(note->amplitudeEnvelopeBasePercent, 1),
                          juce::String::fromUTF8("基础值，% （0–200）："));
    if (auto* editor = dialog->getTextEditor("base"))
    {
        editor->setInputRestrictions(0, "0123456789.");
        editor->setSelectAllWhenFocused(true);
    }
    dialog->addButton(juce::String::fromUTF8("应用"), 1,
                      juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, targets](int result)
            {
                if (safe != nullptr && result == 1)
                    safe->model.setNotesAmplitudeEnvelopeBase(targets,
                        static_cast<float>(
                            dialog->getTextEditorContents("base").getDoubleValue()));
                delete dialog;
            }), false);
}

void PianoRollComponent::showStpDialog(const juce::String& noteId)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    const auto targets = chosenNoteIds(noteId);
    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("修改 STP"),
        juce::String::fromUTF8(
            "把这个音符所用 oto 条目中的全部分段信息整体移动：offset、重叠、"
            "先行声音、辅音和 cutoff，以及四区域的划分，一起移动同样的距离。\n"
            "正数为后移（读取录音中更靠后的部分），负数为前移；单位是毫秒。\n"
            "音符在曲子里的位置不变，变的只是它读到录音的哪一段。"),
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("stp", juce::String(note->utauStpSeconds * 1000.0, 3),
                          juce::String::fromUTF8("STP，ms："));
    if (auto* editor = dialog->getTextEditor("stp"))
    {
        editor->setInputRestrictions(0, "-0123456789.");
        editor->setSelectAllWhenFocused(true);
    }
    dialog->addButton(juce::String::fromUTF8("应用"), 1,
                      juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, targets](int result)
            {
                if (safe != nullptr && result == 1)
                    safe->model.setNotesUtauStp(targets,
                        dialog->getTextEditorContents("stp").getDoubleValue()
                            / 1000.0);
                delete dialog;
            }), false);
}

void PianoRollComponent::showRegionFlagDialog(const juce::String& noteId)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    std::vector<juce::String> targets;
    for (const auto& id : selectedNotes) targets.push_back(juce::String::fromUTF8(id.c_str()));
    if (targets.empty()) targets.push_back(noteId);

    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("flag \u62c6\u5206"),
        juce::String::fromUTF8(
            "\u6bcf\u4e2a\u533a\u5355\u72ec\u7684 flag\uff1b\u7559\u7a7a\u5219\u8be5\u533a"
            "\u6cbf\u7528\u97f3\u7b26\u548c\u8f68\u9053\u7684 flag\u3002\n"
            "\u4ec5\u9010\u5e27\u97f3\u8272\u7c7b flag \u53ef\u5206\u533a\uff08g / Mt / Mb / "
            "Mr / Mo / QX / Mu / b / bh / Mf / MH / Mq \u7b49\uff09\uff1b\n"
            "\u5185\u6838\u9009\u62e9\uff08K1/K2/M1/HF\uff09\u3001u\u3001P/p\u3001SK\u3001"
            "JZ/BX/TM \u7b49\u6574\u6bb5\u6027\u8d28\u7684 flag \u4ecd\u4e3a\u5168\u5c40\u3002"),
        juce::MessageBoxIconType::NoIcon);
    const auto fields = regionFlagFieldsFor(noteId);
    for (const auto& field : fields)
    {
        dialog->addTextEditor(field.key, field.value, field.label);
        if (auto* editor = dialog->getTextEditor(field.key))
            editor->setSelectAllWhenFocused(true);
    }
    // What the regions this entry does not have are carrying.  A box that is
    // not on screen was not edited, so its value goes back in as it was rather
    // than being cleared by a dialog that never showed it.
    const std::array<juce::String, 4> stored {
        note->utauRegionFlags1, note->utauRegionFlags2,
        note->utauRegionFlags3, note->utauRegionFlags4 };
    const auto shown = static_cast<int>(fields.size());
    dialog->addButton(juce::String::fromUTF8("\u5e94\u7528"), 1);
    dialog->addButton(juce::String::fromUTF8("\u5173\u95ed\u62c6\u5206"), 2);
    dialog->addButton(juce::String::fromUTF8("\u53d6\u6d88"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, targets, stored, shown](int result)
            {
                if (safe != nullptr && result != 0)
                {
                    auto values = stored;
                    for (int index = 0; index < shown; ++index)
                        values[static_cast<std::size_t>(index)] =
                            dialog->getTextEditorContents(
                                "r" + juce::String(index + 1));
                    safe->model.setNotesRegionFlags(targets, result == 1,
                        values[0], values[1], values[2], values[3]);
                }
                delete dialog;
            }), false);
}

void PianoRollComponent::confirmBakeVibrato(const juce::String& noteId)
{
    const auto* note = findNote(noteId);
    if (note == nullptr || !note->vibratoEnabled) return;
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
        juce::String::fromUTF8("\u4fee\u6539\u98a4\u97f3"),
        juce::String::fromUTF8(
            "\u98a4\u97f3\u5c06\u5199\u6210\u666e\u901a\u7684\u97f3\u9ad8\u6807\u70b9\uff0c"
            "\u53ef\u5728\u6807\u70b9\u6a21\u5f0f\u4e0b\u9010\u70b9\u4fee\u6539\uff0c"
            "\u540c\u65f6\u98a4\u97f3\u53c2\u6570\u5173\u95ed\u3002\n\n"
            "\u8fd9\u4e00\u6b65\u53ef\u4ee5 Ctrl+Z \u64a4\u9500\uff1b"
            "\u4f46\u4e00\u65e6\u5f00\u59cb\u7f16\u8f91\u8fd9\u6761\u66f2\u7ebf\uff0c"
            "\u5c31\u53ea\u80fd\u9760 Ctrl+Z \u9010\u6b65\u9000\u56de\uff0c"
            "\u65e0\u6cd5\u76f4\u63a5\u6062\u590d\u6210\u53c2\u6570\u5f0f\u98a4\u97f3\u3002"),
        juce::String::fromUTF8("\u4fee\u6539"),
        juce::String::fromUTF8("\u53d6\u6d88"),
        this,
        juce::ModalCallbackFunction::create([safe, noteId](int result)
        {
            if (safe == nullptr || result == 0) return;
            safe->model.bakeNoteVibratoIntoPitch(noteId);
        }));
}

void PianoRollComponent::deleteSelectedNotesRippling(const juce::String& noteId)
{
    // Ordinary delete leaves a hole where the notes were, which is right when
    // the timing around them is meant to stay put.  This one takes the time
    // with them: what followed moves back to meet what came before, note or
    // silence either way.  It is what undoes a phrase pasted in.
    std::vector<juce::String> targets;
    for (const auto& id : selectedNotes)
        targets.push_back(juce::String::fromUTF8(id.c_str()));
    if (targets.empty()) targets.push_back(noteId);
    model.removeNotesRippling(targets);
    clearNoteSelection();
}

void PianoRollComponent::resetConsonant(const juce::String& noteId)
{
    // What holds a consonant away from where the voicebank puts it is a pinned
    // preutterance.  It can be left somewhere unusable -- a lead-in so long
    // there is no handle left to grab, most of all -- and then there is no way
    // back through the handle itself.  This is the way back, and it puts the
    // consonant's end on the note's own start.
    //
    // Two things it does not touch.  The consonant velocity is a setting,
    // chosen deliberately and read as such, and it moves where the consonant
    // begins rather than where it ends.  And the two movable lines dividing
    // the vowel are somebody's work on the vowel, nothing to do with the
    // consonant: this used to throw them away along with the pin, which is a
    // great deal more than was asked for.
    //
    // They cannot simply be left alone, though.  They are held as fractions of
    // the sounding span, and restoring the lead-in moves where that span
    // begins -- so the same fractions would land the lines somewhere else.
    // Where they stand now is noted here and restated on the next layout,
    // against the span the reset has just changed.
    std::vector<juce::String> targets;
    for (const auto& id : selectedNotes)
        targets.push_back(juce::String::fromUTF8(id.c_str()));
    if (targets.empty()) targets.push_back(noteId);

    const auto record = [this](const juce::String& id)
    {
        const auto* note = findNote(id);
        if (note == nullptr || !note->utauJieSplitSet) return;
        const auto span = utauSoundSpans.find(id.toStdString());
        if (span == utauSoundSpans.end()) return;
        const auto start = span->second.first;
        const auto length = span->second.second - start;
        if (length <= 1.0e-9) return;
        pendingSplitRestate.push_back({ id, start + note->utauJieSplit1 * length,
                                        start + note->utauJieSplit2 * length,
                                        start + note->utauJieSplit3 * length });
    };
    for (const auto& id : targets)
    {
        record(id);
        // The note before this one ends where this one's lead-in begins, so
        // its span moves too, and its lines with it.
        if (const auto found = utauNeighbours.find(id.toStdString());
            found != utauNeighbours.end() && !found->second.previous.empty())
            record(juce::String::fromUTF8(found->second.previous.c_str()));
    }

    for (const auto& id : targets)
        model.setNoteUtauTimingOverrides(id, false, 0.0, 0.0);
}

void PianoRollComponent::restateStandingSplits()
{
    // Spend the list first: writing to the model brings us back through the
    // layout, and a list still holding entries would go round again.
    const auto standing = std::exchange(pendingSplitRestate,
                                        std::vector<StandingSplit>{});
    for (const auto& entry : standing)
    {
        const auto span = utauSoundSpans.find(entry.id.toStdString());
        if (span == utauSoundSpans.end()) continue;
        const auto start = span->second.first;
        const auto length = span->second.second - start;
        if (length <= 1.0e-9) continue;
        const auto asFraction = [start, length](double absolute)
        {
            return juce::jlimit(0.0, 1.0, (absolute - start) / length);
        };
        model.setNotesUtauJieSplit({ entry.id }, asFraction(entry.first),
                                   asFraction(entry.second), asFraction(entry.third));
    }
}

void PianoRollComponent::showVibratoDialog(const juce::String& noteId)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    // Apply to the whole selection when several notes are selected, so a chorus
    // of notes can be given the same swing in one go.
    std::vector<juce::String> targets;
    for (const auto& id : selectedNotes) targets.push_back(juce::String::fromUTF8(id.c_str()));
    if (targets.empty()) targets.push_back(noteId);

    auto* dialog = new juce::AlertWindow(
        juce::String::fromUTF8("颤音"),
        juce::String::fromUTF8("以该音的音高线为基准上下颤动；长度从音符末尾往前算。"),
        juce::MessageBoxIconType::NoIcon);
    struct Field { const char* key; const char* label; double value; };
    // Same order as vibratoFieldKeys, which is the order a preset lists them.
    const std::array<Field, 7> fields {{
        { "length",  "长度 (% 于音符末尾)：", note->vibratoLengthPercent },
        { "cycle",   "周期 (ms)：",                                    note->vibratoCycleMs },
        { "depth",   "深度 (cents)：",                                 note->vibratoDepthCents },
        { "fadein",  "淡入 (%)：",                                    note->vibratoFadeInPercent },
        { "fadeout", "淡出 (%)：",                                    note->vibratoFadeOutPercent },
        { "phase",   "相位 (%)：",                                    note->vibratoPhasePercent },
        { "offset",  "中心偏移 (% 于深度)：", note->vibratoOffsetPercent }
    }};
    for (const auto& field : fields)
    {
        dialog->addTextEditor(field.key, juce::String(field.value, 3),
                              juce::String::fromUTF8(field.label));
        if (auto* editor = dialog->getTextEditor(field.key))
        {
            editor->setSelectAllWhenFocused(true);
            editor->setInputRestrictions(0, "-0123456789.");
        }
    }
    auto* presets = new VibratoPresetBar(*dialog,
        onLoadVibratoPresets ? onLoadVibratoPresets() : juce::String(),
        [safe = juce::Component::SafePointer<PianoRollComponent>(this)]
        (const juce::String& text)
        {
            if (safe != nullptr && safe->onSaveVibratoPresets)
                safe->onSaveVibratoPresets(text);
        });
    dialog->addCustomComponent(presets);
    dialog->addButton(juce::String::fromUTF8("应用"), 1);
    dialog->addButton(juce::String::fromUTF8("关闭颤音"), 2);
    dialog->addButton(juce::String::fromUTF8("取消"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, presets, targets](int result)
            {
                if (safe != nullptr && result != 0)
                {
                    NoteData parameters;
                    const auto number = [dialog](const char* key, double fallback)
                    {
                        const auto text = dialog->getTextEditorContents(key).trim();
                        return text.isEmpty() ? fallback : text.getDoubleValue();
                    };
                    parameters.vibratoLengthPercent = number("length", 65.0);
                    parameters.vibratoCycleMs = number("cycle", 180.0);
                    parameters.vibratoDepthCents = number("depth", 35.0);
                    parameters.vibratoFadeInPercent = number("fadein", 20.0);
                    parameters.vibratoFadeOutPercent = number("fadeout", 20.0);
                    parameters.vibratoPhasePercent = number("phase", 0.0);
                    parameters.vibratoOffsetPercent = number("offset", 0.0);
                    safe->model.setNotesVibrato(targets, parameters, result == 1);
                }
                dialog->removeCustomComponent(0);
                delete presets;
                delete dialog;
            }), false);
}

void PianoRollComponent::showNoteTimingDialog(const juce::String& noteId)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    std::optional<backend::UtauSampleTiming> timing;
    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        const auto contains = std::any_of(track.clips.begin(), track.clips.end(),
            [&](const auto& clip)
            {
                return std::any_of(clip.notes.begin(), clip.notes.end(),
                    [&](const auto& candidate) { return candidate.id == noteId; });
            });
        if (!contains) continue;
        const auto velocity = note->utauConsonantVelocity
                != inheritedUtauConsonantVelocity
            ? note->utauConsonantVelocity : track.utauConsonantVelocity;
        timing = backend::UtauRenderer::sampleTiming(
            track.voicebankDirectory, note->label, note->midiNote, velocity,
            utauModeUsesRegions(track.utauMode),
            track.utauMode == UtauMode::mou);
        break;
    }
    // No entry for this lyric: the note cannot sound, but it may still be
    // carrying a pin from a lyric it used to have, and refusing to open was
    // the only thing standing between the user and taking that pin out.
    const auto aliasKnown = timing.has_value();
    if (!timing) timing = backend::UtauSampleTiming {};

    const auto preutteranceSeconds = note->utauPreutteranceOverrideEnabled
        ? note->utauPreutteranceSeconds : timing->preutteranceSeconds;
    const auto overlapSeconds = note->utauOverlapOverrideEnabled
        ? note->utauOverlapSeconds : timing->overlapSeconds;
    auto* dialog = new juce::AlertWindow(juce::String::fromUTF8("时序"),
        juce::String::fromUTF8(
            "先行声音在辅音速度计算后应用；重叠为正时交叉衔接，为负时形成静音间隔。\n"
            "填回音源值后直接应用，音符重新跟随音源；手动改过的数值则固定不变。")
        + (aliasKnown ? juce::String()
                      : juce::String::fromUTF8(
                            "\n音源库中没有这个歌词，所以下方的音源值是空的；"
                            "「填回音源值」仍可用来清掉上一个歌词留下的固定值。")),
                                          juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("preMilliseconds",
        juce::String(preutteranceSeconds * 1000.0, 3),
        juce::String::fromUTF8("先行声音 (Pre)，ms："));
    dialog->addTextEditor("overlapMilliseconds",
        juce::String(overlapSeconds * 1000.0, 3),
        juce::String::fromUTF8("重叠 (Overlap)，ms："));
    if (auto* editor = dialog->getTextEditor("preMilliseconds"))
    {
        editor->setSelectAllWhenFocused(true);
        editor->setInputRestrictions(0, "0123456789.");
    }
    if (auto* editor = dialog->getTextEditor("overlapMilliseconds"))
    {
        editor->setSelectAllWhenFocused(true);
        editor->setInputRestrictions(0, "-0123456789.");
    }
    // Restoring is a custom component rather than a dialog button: every
    // AlertWindow button dismisses the window, so as a button it put the
    // voicebank values back and left before they could be seen or adjusted.
    auto* restoreButton = new juce::TextButton(
        juce::String::fromUTF8("填回音源值"));
    restoreButton->setSize(120, 24);
    // Whether the fields still hold exactly what the voicebank says.  Applying
    // in that state hands the note back to the voicebank instead of pinning
    // the numbers, so a later oto edit reaches it.
    auto restored = std::make_shared<bool>(false);
    const auto sourcePre = timing->preutteranceSeconds;
    const auto sourceOverlap = timing->overlapSeconds;
    restoreButton->onClick = [dialog, restored, sourcePre, sourceOverlap]
    {
        // Silent set: notifying would immediately clear the flag below.
        if (auto* editor = dialog->getTextEditor("preMilliseconds"))
            editor->setText(juce::String(sourcePre * 1000.0, 3), false);
        if (auto* editor = dialog->getTextEditor("overlapMilliseconds"))
            editor->setText(juce::String(sourceOverlap * 1000.0, 3), false);
        *restored = true;
    };
    for (const auto* key : { "preMilliseconds", "overlapMilliseconds" })
        if (auto* editor = dialog->getTextEditor(key))
            editor->onTextChange = [restored] { *restored = false; };
    dialog->addCustomComponent(restoreButton);
    dialog->addButton(juce::String::fromUTF8("应用"), 1);
    dialog->addButton(juce::String::fromUTF8("取消"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, restoreButton, restored, noteId](int result)
            {
                if (safe != nullptr && result == 1)
                {
                    const auto enabled = !*restored;
                    const auto preutteranceSeconds = enabled
                        ? std::max(0.0, dialog->getTextEditorContents(
                            "preMilliseconds").getDoubleValue() / 1000.0)
                        : 0.0;
                    const auto overlapSeconds = enabled
                        ? dialog->getTextEditorContents(
                            "overlapMilliseconds").getDoubleValue() / 1000.0
                        : 0.0;
                    safe->model.setNoteUtauTimingOverrides(
                        noteId, enabled, preutteranceSeconds, overlapSeconds);
                }
                dialog->removeCustomComponent(0);
                delete restoreButton;
                delete dialog;
            }), false);
}

void PianoRollComponent::showCustomBezierDialog(const juce::String& noteId,
                                                 int anchorIndex)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    const auto& anchors = pitchAnchorsFor(*note);
    if (anchorIndex <= 0 || anchorIndex >= static_cast<int>(anchors.size())) return;
    const auto& point = anchors[static_cast<std::size_t>(anchorIndex)];
    auto* editor = new BezierParameterEditor(strings,
        { point.bezierX1, point.bezierY1, point.bezierX2, point.bezierY2 });
    auto* dialog = new juce::AlertWindow(strings.text("pitchCurve.bezierTitle"),
        strings.text("pitchCurve.bezierHelp"), juce::MessageBoxIconType::NoIcon);
    dialog->addCustomComponent(editor);
    dialog->addButton(strings.text("dialog.apply"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, editor, noteId, anchorIndex](int result)
            {
                if (safe != nullptr && result == 1)
                {
                    if (const auto* currentNote = safe->findNote(noteId))
                    {
                        auto anchorsCopy = safe->pitchAnchorsFor(*currentNote);
                        if (anchorIndex > 0
                            && anchorIndex < static_cast<int>(anchorsCopy.size()))
                        {
                            const auto values = editor->values();
                            auto& endpoint = anchorsCopy[static_cast<std::size_t>(anchorIndex)];
                            endpoint.shape = PitchCurveShape::customBezier;
                            endpoint.bezierX1 = juce::jlimit(0.0f, 1.0f, values[0]);
                            endpoint.bezierY1 = juce::jlimit(-2.0f, 3.0f, values[1]);
                            endpoint.bezierX2 = juce::jlimit(0.0f, 1.0f, values[2]);
                            endpoint.bezierY2 = juce::jlimit(-2.0f, 3.0f, values[3]);
                            safe->model.setNotePitchCurve(noteId,
                                                         std::move(anchorsCopy), true);
                        }
                    }
                }
                dialog->removeCustomComponent(0);
                delete editor;
                delete dialog;
            }), false);
}

std::optional<float> PianoRollComponent::anchorMidiForFrequency(double hertz)
{
    if (!std::isfinite(hertz) || hertz <= 0.0) return {};
    const auto midi = 69.0 + 12.0 * std::log2(hertz / 440.0);
    if (!std::isfinite(midi) || midi < 0.0 || midi > 127.0) return {};
    return static_cast<float>(midi);
}

double PianoRollComponent::anchorFrequencyForMidi(float midi)
{
    return 440.0 * std::pow(2.0, (static_cast<double>(midi) - 69.0) / 12.0);
}

std::vector<PitchCurveEditPoint> PianoRollComponent::anchorsWithFrequency(
    std::vector<PitchCurveEditPoint> anchors, int anchorIndex, double hertz)
{
    if (anchorIndex < 0 || anchorIndex >= static_cast<int>(anchors.size())) return {};
    const auto midi = anchorMidiForFrequency(hertz);
    if (!midi) return {};
    // Only the pitch of the one point.  Its time, the shape of the segment
    // leading into it and its Bezier handles are all left as they were.
    anchors[static_cast<std::size_t>(anchorIndex)].targetMidi = *midi;
    return anchors;
}

void PianoRollComponent::showAnchorFrequencyDialog(const juce::String& noteId,
                                                    int anchorIndex)
{
    const auto* note = findNote(noteId);
    if (note == nullptr) return;
    const auto& anchors = pitchAnchorsFor(*note);
    if (anchorIndex < 0 || anchorIndex >= static_cast<int>(anchors.size())) return;
    // Opened on the pitch the point already has, so it can be nudged by a few
    // hertz without having to be worked out first.
    const auto current = anchorFrequencyForMidi(
        anchors[static_cast<std::size_t>(anchorIndex)].targetMidi);
    auto* dialog = new juce::AlertWindow(strings.text("pitchCurve.frequencyTitle"),
        strings.text("pitchCurve.frequencyHelp"), juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("hertz", juce::String(current, 3),
                          strings.text("pitchCurve.frequencyField"));
    if (auto* editor = dialog->getTextEditor("hertz"))
    {
        editor->setInputRestrictions(0, "0123456789.");
        editor->setSelectAllWhenFocused(true);
    }
    dialog->addButton(strings.text("dialog.apply"), 1,
                      juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PianoRollComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, noteId, anchorIndex](int result)
            {
                if (safe != nullptr && result == 1)
                    if (const auto* currentNote = safe->findNote(noteId))
                    {
                        auto moved = PianoRollComponent::anchorsWithFrequency(
                            safe->pitchAnchorsFor(*currentNote), anchorIndex,
                            dialog->getTextEditorContents("hertz").getDoubleValue());
                        if (!moved.empty())
                            safe->model.setNotePitchCurve(noteId, std::move(moved), true);
                    }
                delete dialog;
            }), false);
}

std::vector<PitchCurveEditPoint>& PianoRollComponent::pitchAnchorsFor(const NoteData& note)
{
    const auto key = note.id.toStdString();
    if (const auto found = pitchAnchorCache.find(key); found != pitchAnchorCache.end())
        return found->second;

    std::vector<PitchCurveEditPoint> anchors;
    // Whether this curve is the automatic one derived from the contour or a
    // set of points the user placed.  Only the automatic one may be reshaped
    // below; an explicit curve is the user's and is left alone.
    const auto derived = note.pitchControlPoints.empty();
    if (!derived)
        anchors = note.pitchControlPoints;
    else
    {
        std::vector<PitchCurveEditPoint> dense;
        dense.reserve(note.contour.size() + 2);
        for (const auto& point : note.contour)
            if (point.voiced)
                dense.push_back({ juce::jlimit(0.0, note.durationSeconds, point.timeSeconds),
                                  juce::jlimit(0.0f, 127.0f,
                                      note.midiNote + renderedPitchCents(note, point) / 100.0f) });
        if (dense.empty())
            dense = { { 0.0, note.midiNote }, { note.durationSeconds, note.midiNote } };
        std::stable_sort(dense.begin(), dense.end(), [](const auto& left, const auto& right)
        {
            return left.timeSeconds < right.timeSeconds;
        });
        const auto startPitch = pitchAt(dense, 0.0);
        const auto endPitch = pitchAt(dense, note.durationSeconds);
        if (dense.front().timeSeconds > 1.0e-7) dense.insert(dense.begin(), { 0.0, startPitch });
        else dense.front() = { 0.0, startPitch };
        if (dense.back().timeSeconds < note.durationSeconds - 1.0e-7)
            dense.push_back({ note.durationSeconds, endPitch });
        else dense.back() = { note.durationSeconds, endPitch };

        // Short UTAU notes have very little horizontal room.  Limit their handle
        // count by duration (roughly one interior point per 45 ms), and never show
        // more than 16.  A tolerance search keeps the most significant bends.
        const auto maximumAnchors = static_cast<std::size_t>(juce::jlimit(2, 16,
            static_cast<int>(std::floor(note.durationSeconds / 0.045)) + 2));
        anchors = simplifyPitchAnchors(dense, 0.025f); // 2.5 cents initially.
        if (anchors.size() > maximumAnchors)
        {
            auto low = 0.025f;
            auto high = 0.25f;
            while (simplifyPitchAnchors(dense, high).size() > maximumAnchors && high < 64.0f)
                high *= 2.0f;
            for (int iteration = 0; iteration < 16; ++iteration)
            {
                const auto middle = (low + high) * 0.5f;
                auto candidate = simplifyPitchAnchors(dense, middle);
                if (candidate.size() > maximumAnchors) low = middle;
                else
                {
                    high = middle;
                    anchors = std::move(candidate);
                }
            }
        }
    }

    // Where two notes meet, the automatic curve pulls its endpoints inwards to
    // leave room for the transition between them.  This must not be re-applied
    // to a stored curve: the stored endpoints already carry that inset from
    // when they were first derived, and forcing their time again pinned them
    // in place -- the endpoint at a boundary could be dragged but sprang back
    // the moment the anchors were rebuilt.
    const auto positioned = positionedUtauNotesFor(note.id);
    const auto current = std::find_if(positioned.begin(), positioned.end(),
        [&](const auto& value) { return value.id == note.id; });
    if (derived && anchors.size() >= 2 && current != positioned.end())
    {
        if (current != positioned.begin())
        {
            const auto& previous = *std::prev(current);
            if (formsAdjacentPitchBoundary(previous, *current))
            {
                const auto inset = automaticPitchTransitionInset(
                    previous.endSeconds - previous.startSeconds, note.durationSeconds);
                anchors.erase(std::remove_if(std::next(anchors.begin()), anchors.end(),
                    [&](const auto& point) { return point.timeSeconds <= inset; }), anchors.end());
                anchors.front().timeSeconds = inset;
            }
        }
        if (std::next(current) != positioned.end())
        {
            const auto& next = *std::next(current);
            if (formsAdjacentPitchBoundary(*current, next))
            {
                const auto inset = automaticPitchTransitionInset(note.durationSeconds,
                    next.endSeconds - next.startSeconds);
                const auto tailTime = note.durationSeconds - inset;
                anchors.erase(std::remove_if(anchors.begin(), std::prev(anchors.end()),
                    [&](const auto& point) { return point.timeSeconds >= tailTime; }),
                    std::prev(anchors.end()));
                anchors.back().timeSeconds = tailTime;
            }
        }
    }
    return pitchAnchorCache.emplace(key, std::move(anchors)).first->second;
}

void PianoRollComponent::diagnosticRefresh()
{
    pitchAnchorCache.clear();
    rebuildLayout();
}

std::vector<PianoRollComponent::DiagnosticNote>
PianoRollComponent::diagnosticNotes() const
{
    std::vector<DiagnosticNote> result;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                DiagnosticNote entry;
                entry.id = note.id;
                entry.label = note.label;
                entry.start = clip.startSeconds + note.startSeconds;
                entry.end = entry.start + note.durationSeconds;
                entry.utau = track.pitchAlgorithm == PitchAlgorithm::utau;
                entry.soundingStart = entry.start;
                entry.soundingEnd = entry.end;
                if (const auto span = utauSoundSpans.find(note.id.toStdString());
                    span != utauSoundSpans.end())
                {
                    entry.soundingStart = span->second.first;
                    entry.soundingEnd = span->second.second;
                }
                entry.envelope = amplitudeEnvelopeFor(note, entry.start);
                result.push_back(std::move(entry));
            }
    return result;
}

void PianoRollComponent::selectAllNotes()
{
    selectedNotes.clear();
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose && !sourceEditMode) continue;
        if (!sourceEditMode && focusedTrack.isNotEmpty() && track.id != focusedTrack) continue;
        for (const auto& clip : track.clips)
        {
            // Wrench mode edits one source instance.  Drawing every project
            // use of the same WAV stacked duplicate vertical sliders on top
            // of one HJM region.
            if (sourceEditMode && clip.id != focusedClip) continue;
            for (const auto& note : clip.notes) selectedNotes.insert(note.id.toStdString());
        }
    }
    selectedNote = selectedNotes.empty() ? juce::String()
        : juce::String::fromUTF8(selectedNotes.begin()->c_str());
    if (onNoteSelected) onNoteSelected(selectedNote);
    repaint();
}

void PianoRollComponent::clearNoteSelection()
{
    selectedNote.clear();
    selectedNotes.clear();
    if (onNoteSelected) onNoteSelected({});
    repaint();
}

void PianoRollComponent::setSelectedNoteIds(const std::vector<juce::String>& noteIds)
{
    selectedNotes.clear();
    for (const auto& id : noteIds)
        if (id.isNotEmpty()) selectedNotes.insert(id.toStdString());
    selectedNote = noteIds.empty() ? juce::String{} : noteIds.front();
    if (onNoteSelected) onNoteSelected(selectedNote);
    repaint();
}

std::vector<juce::String> PianoRollComponent::selectedNoteIds() const
{
    std::vector<juce::String> result;
    result.reserve(selectedNotes.size() + (selectedNote.isNotEmpty() ? 1u : 0u));
    for (const auto& id : selectedNotes)
        result.push_back(juce::String::fromUTF8(id.c_str()));
    if (result.empty() && selectedNote.isNotEmpty()) result.push_back(selectedNote);
    return result;
}

double PianoRollComponent::gridQuarterNotes() const
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

double PianoRollComponent::gridSecondsAt(double seconds) const
{
    const auto quarter = snapshot.quarterPositionForSeconds(seconds);
    return std::max(0.001,
        snapshot.secondsForQuarterPosition(quarter + gridQuarterNotes())
            - snapshot.secondsForQuarterPosition(quarter));
}

double PianoRollComponent::drawnLengthFor(double draggedSeconds,
                                          double unitSeconds,
                                          double availableSeconds)
{
    const auto unit = std::max(1.0e-6, unitSeconds);
    // The unit the pointer is standing in counts, so a press that never moves
    // still makes one -- floor of the distance, plus the one being pointed at.
    const auto units = std::max(1.0, std::floor(draggedSeconds / unit) + 1.0);
    return std::min(units * unit, availableSeconds);
}

std::vector<int> PianoRollComponent::drawLengthDivisions()
{
    std::vector<int> divisions;
    for (auto division = 128; division >= 4; division /= 2)
        divisions.push_back(division);
    return divisions;
}

void PianoRollComponent::setDrawLengthDivision(int division)
{
    const auto offered = drawLengthDivisions();
    if (std::find(offered.begin(), offered.end(), division) == offered.end()) return;
    drawLengthDivisionValue = division;
}

double PianoRollComponent::drawUnitSecondsAt(double seconds) const
{
    // quarterBarSecondsAt is a quarter of a bar, so a bar is four of them.
    // Said this way rather than with a bar length of its own, so the two
    // cannot disagree when the signature or the tempo changes.
    const auto bar = quarterBarSecondsAt(seconds) * 4.0;
    return std::max(1.0e-6, bar / static_cast<double>(std::max(1, drawLengthDivisionValue)));
}

bool PianoRollComponent::drawingTrackIsUtau() const
{
    for (const auto& track : snapshot.tracks)
        if (track.id == focusedTrack)
            return track.pitchAlgorithm == PitchAlgorithm::utau;
    // No track selected: fall back to whoever owns the focused clip.
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == focusedClip)
                return track.pitchAlgorithm == PitchAlgorithm::utau;
    return false;
}

juce::String PianoRollComponent::clipForDrawingAt(double seconds)
{
    const auto pick = [&](const ProjectData& data) -> juce::String
    {
        const ClipData* containing = nullptr;
        const ClipData* growable = nullptr;
        for (const auto& track : data.tracks)
        {
            if (!track.compose) continue;
            const auto mine = focusedTrack.isEmpty() || track.id == focusedTrack;
            for (const auto& clip : track.clips)
            {
                if (clip.id == focusedClip) return clip.id;
                if (!mine) continue;
                if (seconds >= clip.startSeconds
                    && seconds <= clip.startSeconds + clip.durationSeconds)
                    containing = &clip;
                // A clip with no recording behind it is a span of the
                // timeline and stretches to fit, so one that begins before
                // here can still take the note.  Reaching for the latest such
                // clip rather than making a new one: drawing past the end of
                // the seed clip used to add a second clip every time, and
                // then find neither of them.
                if (clip.sourceFile == juce::File() && clip.startSeconds <= seconds
                    && (growable == nullptr || clip.startSeconds > growable->startSeconds))
                    growable = &clip;
            }
        }
        if (containing != nullptr) return containing->id;
        if (growable != nullptr) return growable->id;
        return {};
    };
    if (const auto found = pick(snapshot); found.isNotEmpty()) return found;

    // Nothing to draw into.  A melodic track starts with no clips at all and
    // no other part of the interface makes one, so the first stroke on a new
    // track makes it -- from the start of the timeline, since a clip that
    // began under the pointer would leave the bars before it unreachable.
    // One bar is only a seed: a composed clip grows to fit what is drawn.
    auto empty = false;
    for (const auto& track : snapshot.tracks)
        if (track.id == focusedTrack && track.compose && track.clips.empty())
            empty = true;
    if (!empty) return {};

    const auto seed = std::max(0.05, quarterBarSecondsAt(0.0) * 4.0);
    if (model.addClip(focusedTrack, 0.0, seed).isEmpty()) return {};
    // Asked of the model, not of this view: the copy here only catches up on
    // a message, and the clip was made a moment ago.
    return pick(model.snapshot());
}

void PianoRollComponent::beginDrawingNote(const juce::MouseEvent& event)
{
    const auto rawSeconds = std::max(0.0,
        static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond);
    const auto cellStart = snapDownToGrid(rawSeconds);

    const auto clipId = clipForDrawingAt(cellStart);
    if (clipId.isEmpty()) return;
    // From the model, so a clip made a moment ago is visible here.
    const auto data = model.snapshot();
    const ClipData* destination = nullptr;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == clipId) destination = &clip;
    if (destination == nullptr) return;

    std::vector<ProjectModel::NoteSpan> occupied;
    occupied.reserve(destination->notes.size());
    for (const auto& note : destination->notes)
        occupied.push_back({ note.startSeconds, note.durationSeconds });

    // The cell clicked, or -- if something is already sounding there -- the
    // moment that note ends.  The model repeats this search under its own lock
    // when the button comes up; this copy is only what the preview draws.
    const auto localStart = ProjectModel::firstFreeStartFrom(
        cellStart - destination->startSeconds, occupied);
    // A clip with no recording behind it stretches to fit, so the only limit
    // is the next note; one backed by audio ends where its audio does.
    auto available = destination->sourceFile == juce::File()
        ? std::numeric_limits<double>::max()
        : destination->durationSeconds - localStart;
    for (const auto& span : occupied)
        if (span.startSeconds > localStart)
            available = std::min(available, span.startSeconds - localStart);
    // The model's own floor for a note.  Below it nothing can be made, so
    // nothing is started and no rectangle appears promising otherwise.
    if (available < 0.01) return;

    drawClipId = destination->id;
    drawStartSeconds = destination->startSeconds + localStart;
    drawAvailableSeconds = available;
    drawMidi = juce::jlimit(0.0f, 127.0f, std::round(yToMidi(event.position.y)));
    drawUnitSeconds = drawUnitSecondsAt(drawStartSeconds);
    drawLengthSeconds = drawnLengthFor(0.0, drawUnitSeconds, drawAvailableSeconds);
    dragMode = DragMode::drawNewNote;
    repaint();
}

double PianoRollComponent::snapDownToGrid(double seconds) const
{
    const auto step = gridQuarterNotes();
    const auto quarter = snapshot.quarterPositionForSeconds(seconds);
    return std::max(0.0, snapshot.secondsForQuarterPosition(
        std::floor(quarter / step) * step));
}

double PianoRollComponent::snapToGrid(double seconds) const
{
    const auto step = gridQuarterNotes();
    const auto quarter = snapshot.quarterPositionForSeconds(seconds);
    return std::max(0.0, snapshot.secondsForQuarterPosition(
        std::round(quarter / step) * step));
}

int PianoRollComponent::pixelForSeconds(double seconds) const
{
    return static_cast<int>(std::round(timeToX(seconds)));
}

double PianoRollComponent::secondsForPixel(int pixel) const
{
    return std::max(0.0, static_cast<double>(pixel - 58) / pixelsPerSecond);
}

float PianoRollComponent::timeToX(double seconds) const
{
    return 58.0f + static_cast<float>(seconds) * pixelsPerSecond;
}

float PianoRollComponent::midiToY(float midi) const
{
    return static_cast<float>(highestMidi) * rowHeight - midi * rowHeight;
}

float PianoRollComponent::yToMidi(float y) const
{
    return static_cast<float>(highestMidi) - y / rowHeight;
}

void PianoRollComponent::rebuildLayout()
{
    snapshot = model.snapshot();
    rebuildUtauSoundSpans();
    if (!pendingSplitRestate.empty())
    {
        // Restate before anything is drawn from these spans, so the lines do
        // not flicker through one frame of their old fractions.
        restateStandingSplits();
        snapshot = model.snapshot();
        rebuildUtauSoundSpans();
    }
    std::unordered_set<std::string> validNotes;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes) validNotes.insert(note.id.toStdString());
    std::erase_if(selectedNotes, [&](const auto& id) { return !validNotes.contains(id); });
    if (selectedNote.isNotEmpty() && !validNotes.contains(selectedNote.toStdString()))
        selectedNote.clear();
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
            thumbnail->addChangeListener(this);
            if (clip.sourceFile.existsAsFile())
                thumbnail->setSource(new juce::FileInputSource(clip.sourceFile));
            next.emplace(key, std::move(thumbnail));
        }
    thumbnails = std::move(next);
    updateCanvasSize();
    repaint();
}

void PianoRollComponent::rebuildUtauSoundSpans()
{
    struct DisplayNote
    {
        juce::String id;
        double start = 0.0;
        double end = 0.0;
        bool spliced = false;
        std::optional<backend::UtauSampleTiming> timing;
    };

    utauSoundSpans.clear();
    utauNeighbours.clear();
    if (sourceEditMode) return;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose || track.pitchAlgorithm != PitchAlgorithm::utau
            || !track.voicebankDirectory.isDirectory())
            continue;
        std::vector<DisplayNote> notes;
        // sampleTiming stats the voicebank folder, builds a cache key and
        // scans every oto entry by name, so asking it once per note made an
        // edit cost thousands of string comparisons.  Notes repeat their
        // lyric and their pitch, so ask once per distinct answer.
        std::map<std::tuple<juce::String, float, int>,
                 std::optional<backend::UtauSampleTiming>> timings;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                const auto start = clip.startSeconds + note.startSeconds;
                DisplayNote display { note.id, start, start + note.durationSeconds,
                                      note.utauSplice };
                const auto consonantVelocity =
                    note.utauConsonantVelocity != inheritedUtauConsonantVelocity
                    ? note.utauConsonantVelocity : track.utauConsonantVelocity;
                const auto timingKey = std::make_tuple(note.label, note.midiNote,
                                                       consonantVelocity);
                const auto cached = timings.find(timingKey);
                display.timing = cached != timings.end()
                    ? cached->second
                    : timings.emplace(timingKey, backend::UtauRenderer::sampleTiming(
                          track.voicebankDirectory, note.label, note.midiNote,
                          consonantVelocity, utauModeUsesRegions(track.utauMode),
                          track.utauMode == UtauMode::mou)).first->second;
                if (display.timing)
                {
                    if (note.utauPreutteranceOverrideEnabled)
                        display.timing->preutteranceSeconds =
                            std::max(0.0, note.utauPreutteranceSeconds);
                    if (note.utauOverlapOverrideEnabled)
                        display.timing->overlapSeconds = note.utauOverlapSeconds;
                }
                notes.push_back(std::move(display));
            }
        std::stable_sort(notes.begin(), notes.end(), [](const auto& left, const auto& right)
        {
            if (std::abs(left.start - right.start) > 1.0e-9) return left.start < right.start;
            return left.end < right.end;
        });
        for (std::size_t index = 0; index < notes.size(); ++index)
        {
            const auto& note = notes[index];
            if (!note.timing) continue;
            const auto leadIn = std::min(note.timing->preutteranceSeconds,
                                         std::max(0.0, note.start));
            auto soundingEnd = note.end;
            if (index + 1 < notes.size())
            {
                const auto& next = notes[index + 1];
                // Read through the renderer's own rules rather than
                // spelled out again here: what is drawn and what is mixed have
                // to say the same thing about where a note stops, and a second
                // copy of a rule is how they came to disagree.
                const auto nextSoundStart = next.timing
                    ? next.start - std::min(next.timing->preutteranceSeconds,
                                            std::max(0.0, next.start))
                    : next.start;
                if (next.timing && backend::UtauRenderer::crossfadesInto(
                        nextSoundStart, note.end))
                {
                    const auto reach = backend::UtauRenderer::crossfadeEnd(
                        nextSoundStart, next.timing->overlapSeconds, next.end);
                    // Splicing is the exception: it crossfades only the
                    // stretch the two notes already share and leaves both
                    // spans where they were, so there the tail does stop at
                    // this note's end.
                    soundingEnd = next.spliced ? std::min(note.end, reach) : reach;
                }
            }
            const auto soundingStart = note.start - leadIn;
            soundingEnd = std::max(soundingStart + 0.001, soundingEnd);
            auto& neighbours = utauNeighbours[note.id.toStdString()];
            if (index > 0) neighbours.previous = notes[index - 1].id.toStdString();
            if (index + 1 < notes.size())
            {
                neighbours.next = notes[index + 1].id.toStdString();
                neighbours.nextSpliced = notes[index + 1].spliced;
            }
            utauSoundSpans[note.id.toStdString()] = { soundingStart, soundingEnd };
        }
    }
}

void PianoRollComponent::updateCanvasSize()
{
    auto duration = snapshot.durationSeconds();
    if (sourceEditMode)
        for (const auto& track : snapshot.tracks)
            for (const auto& clip : track.clips)
                if (clip.id == focusedClip)
                    if (const auto found = thumbnails.find(clip.sourceFile.getFullPathName().toStdString());
                        found != thumbnails.end())
                        duration = std::max(duration, found->second->getTotalLength());
    // Bounded because the width is duration times zoom: a long project at the
    // finest zoom would otherwise ask for a component wider than the drawing
    // coordinates stay exact in.
    const auto width = juce::jlimit(470.0, 3.2e7,
                                    static_cast<double>(timeToX(duration)) + 400.0);
    setSize(static_cast<int>(width),
            (highestMidi - lowestMidi + 1) * static_cast<int>(rowHeight));
    // Hit regions depend on exactly what this function does -- the note list,
    // the sounding spans and the time and pitch scales -- so they are rebuilt
    // here rather than at the top of every paint.
    rebuildNoteHits();
}

void PianoRollComponent::setShowUtauWaveforms(bool enabled)
{
    if (showUtauWaveforms == enabled) return;
    showUtauWaveforms = enabled;
    repaint();
}

void PianoRollComponent::setUtauNoteWaveforms(
    std::shared_ptr<const std::vector<UtauNoteWaveform>> waveforms)
{
    // Same pointer, same peaks: a repaint per timer tick would otherwise cost
    // one for every note in the song, forever.
    if (utauWaveforms == waveforms) return;
    utauWaveforms = std::move(waveforms);
    if (showUtauWaveforms) repaint();
}

void PianoRollComponent::drawUtauNoteWaveforms(juce::Graphics& g)
{
    if (utauWaveforms == nullptr || utauWaveforms->empty()) return;
    // Indexed once rather than searched per note: a song is thousands of
    // notes and this runs inside paint.
    std::unordered_map<std::string, const UtauNoteWaveform*> byId;
    byId.reserve(utauWaveforms->size());
    for (const auto& waveform : *utauWaveforms)
        byId.emplace(waveform.noteId.toStdString(), &waveform);

    for (const auto& track : snapshot.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        if (focusedTrack.isNotEmpty() && track.id != focusedTrack) continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                const auto found = byId.find(note.id.toStdString());
                if (found == byId.end()) continue;
                const auto& waveform = *found->second;
                // The note has to still be the note that was rendered.  Edit
                // it and the hash moves, and the waveform stops being drawn
                // until the new audio arrives to replace it.
                if (waveform.renderHash != AudioEngine::utauNoteRenderHash(note)) continue;
                if (waveform.maxima.empty() || waveform.durationSeconds <= 0.0) continue;

                const auto left = timeToX(clip.startSeconds + note.startSeconds);
                const auto width = static_cast<float>(note.durationSeconds) * pixelsPerSecond;
                if (width < 1.0f) continue;
                const auto height = rowHeight * 6.0f;
                const auto centre = midiToY(note.midiNote) + rowHeight * 0.5f;
                g.setColour(Palette::textMuted.withAlpha(track.muted ? 0.12f : 0.34f));
                const auto buckets = static_cast<int>(waveform.maxima.size());
                const auto columns = juce::jlimit(1, buckets,
                                                  static_cast<int>(std::ceil(width)));
                for (int column = 0; column < columns; ++column)
                {
                    // Every bucket the column covers, so zooming out keeps the
                    // peaks instead of sampling one bucket in twenty.
                    const auto from = buckets * column / columns;
                    const auto to = std::max(from + 1, buckets * (column + 1) / columns);
                    auto low = 0.0f;
                    auto high = 0.0f;
                    for (auto bucket = from; bucket < to && bucket < buckets; ++bucket)
                    {
                        low = std::min(low, waveform.minima[static_cast<std::size_t>(bucket)]);
                        high = std::max(high, waveform.maxima[static_cast<std::size_t>(bucket)]);
                    }
                    const auto x = left + width * static_cast<float>(column)
                                          / static_cast<float>(columns);
                    const auto top = centre - high * height * 0.5f;
                    const auto bottom = centre - low * height * 0.5f;
                    g.fillRect(x, top, std::max(1.0f, width / static_cast<float>(columns)),
                               std::max(1.0f, bottom - top));
                }
            }
    }
}

void PianoRollComponent::drawClipWaveforms(juce::Graphics& g)
{
    juce::String focusedSource;
    if (sourceEditMode)
        for (const auto& track : snapshot.tracks)
            for (const auto& clip : track.clips)
                if (clip.id == focusedClip)
                    focusedSource = clip.sourceFile.getFullPathName();
    bool sourceWaveformDrawn = false;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose && !sourceEditMode) continue;
        if (!sourceEditMode && focusedTrack.isNotEmpty() && track.id != focusedTrack) continue;
        for (const auto& clip : track.clips)
        {
            if (sourceEditMode && clip.id != focusedClip)
                continue;
            const auto found = thumbnails.find(clip.sourceFile.getFullPathName().toStdString());
            if (found == thumbnails.end()) continue;
            float centreMidi = 60.0f;
            if (!clip.notes.empty())
            {
                double weighted = 0.0;
                double duration = 0.0;
                for (const auto& note : clip.notes)
                {
                    const auto weight = std::max(0.01, note.durationSeconds);
                    weighted += static_cast<double>(note.midiNote) * weight;
                    duration += weight;
                }
                centreMidi = static_cast<float>(weighted / std::max(0.01, duration));
            }
            if (sourceEditMode && sourceWaveformDrawn) continue;
            const auto start = sourceEditMode ? 0.0 : clip.startSeconds;
            const auto sourceLength = sourceEditMode ? found->second->getTotalLength()
                                                      : (clip.sourceDurationSeconds > 1.0e-9
                                                          ? clip.sourceDurationSeconds : clip.durationSeconds);
            const auto waveformHeight = rowHeight * 10.0f;
            const juce::Rectangle<float> bounds(timeToX(start),
                midiToY(centreMidi) + rowHeight * 0.5f - waveformHeight * 0.5f,
                std::max(4.0f, static_cast<float>(sourceEditMode ? sourceLength : clip.durationSeconds)
                                      * pixelsPerSecond), waveformHeight);
            g.setColour(Palette::textMuted.withAlpha(track.muted ? 0.12f : 0.34f));
            // Stereo material uses one full-height editing waveform instead of
            // JUCE's stacked per-channel lanes.  Audio playback/export remains
            // stereo; this selects channel 1 for display only.
            found->second->drawChannel(
                g, bounds.toNearestInt(),
                sourceEditMode ? 0.0 : clip.sourceOffsetSeconds,
                sourceEditMode ? sourceLength : clip.sourceOffsetSeconds + sourceLength,
                0, 1.0f);
            if (sourceEditMode) sourceWaveformDrawn = true;
        }
    }
}

void PianoRollComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &model)
    {
        // Rebuild point handles from the authoritative contour after undo,
        // freehand/line edits, note resizing, and project loading.  The
        // simplifier prevents these rebuilds from exposing dense 5 ms frames.
        pitchAnchorCache.clear();
        rebuildLayout();
    }
    else
    {
        updateCanvasSize();
        repaint();
    }
}

void PianoRollComponent::diagnosticBeginResizeDrag(const juce::String& noteId,
                                                   double previewStart,
                                                   double previewDuration)
{
    draggedNote = noteId;
    dragMode = DragMode::resizeRight;
    previewStartSeconds = previewStart;
    previewDurationSeconds = previewDuration;
}

void PianoRollComponent::diagnosticBeginConsonantDrag(const juce::String& noteId,
                                                      double previewPreutterance)
{
    draggedNote = noteId;
    dragMode = DragMode::consonantLeadIn;
    previewConsonantPreutterance = previewPreutterance;
}

bool PianoRollComponent::diagnosticGrabConsonantHandle(std::size_t index)
{
    std::size_t seen = 0;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (seen++ != index) continue;
                const auto span = utauSoundSpans.find(note.id.toStdString());
                if (span == utauSoundSpans.end()) return false;
                const auto handle = consonantHandleAt({ timeToX(span->second.first),
                    midiToY(note.midiNote) + 4.0f });
                if (!handle) return false;
                draggedNote = handle->noteId;
                consonantNoteAbsoluteStart = handle->absoluteStart;
                consonantScaledPreutterance = handle->scaledPreutterance;
                consonantUnscaledPreutterance = handle->unscaledPreutterance;
                consonantMinimumPreutterance = handle->minimumPreutterance;
                consonantMaximumPreutterance = handle->maximumPreutterance;
                previewConsonantPreutterance = handle->currentPreutterance;
                previewConsonantVelocity = handle->currentVelocity;
                consonantSetsPin = handle->setsPin;
                consonantOverlapSeconds = handle->overlapSeconds;
                dragMode = DragMode::consonantLeadIn;
                return true;
            }
    return false;
}

void PianoRollComponent::dragConsonantTo(double cursorSeconds)
{
    const auto requested = juce::jlimit(consonantMinimumPreutterance,
        consonantMaximumPreutterance,
        consonantNoteAbsoluteStart - cursorSeconds);
    if (consonantSetsPin)
    {
        // Straight to the lead-in.  谋 writes what the drag asked for rather
        // than the velocity that would produce it, so there is nothing to
        // express it through and the handle follows the cursor exactly.  How
        // far it may go is the handle's business, not this one's: free back to
        // the note in front when the entry calls the first region a vowel, and
        // what the oto can express when it calls it a consonant.
        previewConsonantPreutterance = requested;
        return;
    }
    const auto maximumScale = std::max(1.0e-12,
        (consonantMaximumPreutterance - consonantUnscaledPreutterance)
            / std::max(1.0e-9, consonantScaledPreutterance));
    const auto scale = juce::jlimit(1.0e-12, maximumScale,
        (requested - consonantUnscaledPreutterance)
            / std::max(1.0e-9, consonantScaledPreutterance));
    previewConsonantVelocity = static_cast<int>(std::lround(
        100.0 * (1.0 - std::log2(scale))));
    const auto exactScale = std::pow(2.0,
        1.0 - static_cast<double>(previewConsonantVelocity) / 100.0);
    previewConsonantPreutterance = juce::jlimit(
        consonantMinimumPreutterance, consonantMaximumPreutterance,
        consonantUnscaledPreutterance
            + consonantScaledPreutterance * exactScale);
}

void PianoRollComponent::diagnosticBeginMoveDrag(const juce::String& noteId,
                                                 double deltaSeconds)
{
    draggedNote = noteId;
    dragMode = DragMode::moveUtauNote;
    selectedNotes.clear();
    selectedNotes.insert(noteId.toStdString());
    previewMoveDeltaSeconds = deltaSeconds;
    if (const auto* note = findNote(noteId))
    {
        dragStartMidi = note->midiNote;
        previewMidi = note->midiNote;
    }
}

juce::Rectangle<int> PianoRollComponent::diagnosticDragRepaintArea(float mouseX) const
{
    const auto band = dragRepaintBand(mouseX);
    if (band.isEmpty()) return getLocalBounds();
    const auto left = static_cast<int>(std::floor(band.getStart())) - 8;
    const auto right = static_cast<int>(std::ceil(band.getEnd())) + 8;
    return { left, 0, std::max(1, right - left), getHeight() };
}

juce::Range<float> PianoRollComponent::dragRepaintBand(float mouseX) const
{
    // Region handles are collected while painting, so a partial paint would
    // lose the ones it skipped.  Wrench mode always repaints in full.
    if (sourceEditMode) return {};
    if (dragMode == DragMode::marquee)
        return { std::min(marqueeStart.x, marqueeCurrent.x),
                 std::max(marqueeStart.x, marqueeCurrent.x) };
    juce::Range<float> involved;
    auto found = false;
    for (const auto& hit : noteHits)
    {
        if (hit.id != draggedNote
            && !(dragMode == DragMode::moveUtauNote
                 && selectedNotes.contains(hit.id.toStdString())))
            continue;
        const juce::Range<float> bounds(hit.bounds.getX(), hit.bounds.getRight());
        involved = found ? involved.getUnionWith(bounds) : bounds;
        found = true;
    }
    if (!found) return {};
    // The hits still hold where the drag started, and whatever is being
    // dragged follows the cursor without growing, so it cannot have reached
    // further from the cursor than its own width.  The extra margin covers
    // the pitch line handing over to the note next door.
    const auto reach = involved.getLength() + 96.0f;
    return involved.getUnionWith({ mouseX - reach, mouseX + reach });
}

void PianoRollComponent::repaintDrag(float mouseX)
{
    const auto band = dragRepaintBand(mouseX);
    if (band.isEmpty())
    {
        lastDragBand = {};
        repaint();
        return;
    }
    const auto united = lastDragBand.isEmpty() ? band : band.getUnionWith(lastDragBand);
    lastDragBand = band;
    const auto left = static_cast<int>(std::floor(united.getStart())) - 8;
    const auto right = static_cast<int>(std::ceil(united.getEnd())) + 8;
    // Full height deliberately: the viewport clips it to the rows on screen
    // anyway, and a pitch line may leave its note's own row entirely.
    repaint(left, 0, std::max(1, right - left), getHeight());
}

void PianoRollComponent::rebuildNoteHits()
{
    noteHits.clear();
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose && !sourceEditMode) continue;
        if (!sourceEditMode && focusedTrack.isNotEmpty() && track.id != focusedTrack) continue;
        for (const auto& clip : track.clips)
        {
            if (sourceEditMode && clip.id != focusedClip) continue;
            const auto sourceScale = sourceEditMode && clip.durationSeconds > 1.0e-9
                ? (clip.sourceDurationSeconds > 1.0e-9 ? clip.sourceDurationSeconds
                                                        : clip.durationSeconds)
                    / clip.durationSeconds
                : 1.0;
            for (const auto& note : clip.notes)
            {
                const auto absoluteStart = sourceEditMode
                    ? clip.sourceOffsetSeconds + note.startSeconds * sourceScale
                    : clip.startSeconds + note.startSeconds;
                const auto x = timeToX(absoluteStart);
                const auto y = midiToY(note.midiNote);
                const auto width = std::max(5.0f,
                    static_cast<float>(note.durationSeconds * sourceScale) * pixelsPerSecond);
                const auto blockHeight = std::max(6.0f, rowHeight - 4.0f);
                auto hitBounds = juce::Rectangle<float>(x, y + 2.0f, width, blockHeight);
                // Kept before the sounding span takes over, and worked out
                // here where the source-edit scaling is still to hand.
                const auto nominalBounds = hitBounds;
                if (track.pitchAlgorithm == PitchAlgorithm::utau)
                    if (const auto span = utauSoundSpans.find(note.id.toStdString());
                        span != utauSoundSpans.end())
                    {
                        const auto soundingX = timeToX(span->second.first);
                        hitBounds.setX(soundingX);
                        hitBounds.setWidth(std::max(2.0f,
                            timeToX(span->second.second) - soundingX));
                    }
                noteHits.push_back({ note.id,
                    hitBounds, nominalBounds,
                    note.midiNote, note.startSeconds, note.durationSeconds,
                    clip.startSeconds });
            }
        }
    }
}

const PianoRollComponent::NoteHit* PianoRollComponent::resizableTailAt(
    juce::Point<float> position, double& maximumDuration) const
{
    maximumDuration = 1.0e12;
    constexpr auto boundaryTolerance = 0.001;
    for (auto it = noteHits.rbegin(); it != noteHits.rend(); ++it)
    {
        const auto absoluteStart = it->clipStartSeconds + it->startSeconds;
        const auto nominalBounds = juce::Rectangle<float>(
            timeToX(absoluteStart), midiToY(it->midi) + 2.0f,
            std::max(5.0f, static_cast<float>(it->durationSeconds) * pixelsPerSecond),
            std::max(6.0f, rowHeight - 4.0f));
        const auto tail = juce::Rectangle<float>(nominalBounds.getRight() - 7.0f,
            nominalBounds.getY(), 14.0f, nominalBounds.getHeight());
        if (!tail.contains(position)) continue;

        auto nextStart = 1.0e12;
        for (const auto& candidate : noteHits)
        {
            if (candidate.id == it->id) continue;
            const auto candidateStart = candidate.clipStartSeconds + candidate.startSeconds;
            // The following note is a maximum boundary, not a reason to hide
            // the handle.  An adjacent note therefore still permits shrinking,
            // and the shortened note can later be extended back to this start.
            // If notes already overlap, dragging immediately snaps the tail
            // into the legal non-overlapping range.
            if (candidateStart > absoluteStart + boundaryTolerance)
                nextStart = std::min(nextStart, candidateStart);
        }
        if (nextStart < 1.0e11)
        {
            maximumDuration = nextStart - absoluteStart;
            if (maximumDuration + boundaryTolerance < noteEditQuantumSeconds())
                return nullptr;
        }
        return &*it;
    }
    return nullptr;
}

NoteData PianoRollComponent::effectiveVibrato(const NoteData& note) const
{
    if (dragMode == DragMode::vibrato && note.id == draggedNote)
    {
        auto preview = note;
        preview.vibratoEnabled = true;
        preview.vibratoLengthPercent = previewVibrato.vibratoLengthPercent;
        preview.vibratoCycleMs = previewVibrato.vibratoCycleMs;
        preview.vibratoDepthCents = previewVibrato.vibratoDepthCents;
        preview.vibratoFadeInPercent = previewVibrato.vibratoFadeInPercent;
        preview.vibratoFadeOutPercent = previewVibrato.vibratoFadeOutPercent;
        preview.vibratoPhasePercent = previewVibrato.vibratoPhasePercent;
        preview.vibratoOffsetPercent = previewVibrato.vibratoOffsetPercent;
        return preview;
    }
    return note;
}

PianoRollComponent::VibratoHandlePlace PianoRollComponent::vibratoHandlePlace(
    const NoteData& note, VibratoHandle which)
{
    const auto duration = std::max(1.0e-6, note.durationSeconds);
    const auto span = duration * juce::jlimit(0.0, 100.0, note.vibratoLengthPercent) / 100.0;
    const auto start = duration - span;
    const auto cycle = std::max(0.01, note.vibratoCycleMs) / 1000.0;
    constexpr auto minimumHandleCents = 25.0;
    switch (which)
    {
        case VibratoHandle::length:
            return { start, 0.0, 0.0 };
        case VibratoHandle::offset:
            // Vertically it marks where the swing is centred, so dragging it
            // slides the whole vibrato up or down.
            return { 0.0, note.vibratoDepthCents * note.vibratoOffsetPercent / 100.0,
                     -12.0 };
        case VibratoHandle::fadeIn:
            return { start + span * juce::jlimit(0.0, 100.0,
                        note.vibratoFadeInPercent) / 100.0, 0.0, 0.0 };
        case VibratoHandle::fadeOut:
            return { duration - span * juce::jlimit(0.0, 100.0,
                        note.vibratoFadeOutPercent) / 100.0, 0.0, 0.0 };
        case VibratoHandle::cycle:
            // On the first trough, and the depth handle on the first crest.
            // Both are pushed at least a quarter semitone off the centre line
            // even when the swing is shallower than that, because otherwise
            // they collapse onto the three handles that live on the line and
            // become impossible to tell apart, let alone grab.
            return { std::min(duration, start + cycle * 0.75),
                     -std::max(note.vibratoDepthCents, minimumHandleCents), 0.0 };
        case VibratoHandle::depth:
            return { std::min(duration, start + cycle * 0.25),
                     std::max(note.vibratoDepthCents, minimumHandleCents), 0.0 };
        case VibratoHandle::none: break;
    }
    return { start, 0.0, 0.0 };
}

std::optional<PianoRollComponent::VibratoHandleInfo>
PianoRollComponent::vibratoHandleAt(juce::Point<float> position) const
{
    constexpr auto radius = 5.0f;
    if (sourceEditMode || tool == Tool::amplitude || tool == Tool::flagCurve)
        return std::nullopt;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose || (focusedTrack.isNotEmpty() && track.id != focusedTrack))
            continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (!note.vibratoEnabled) continue;
                // Handles belong to the note being worked on; showing them for
                // every note at once would bury the roll in dots.
                if (!selectedNotes.contains(note.id.toStdString())
                    && note.id != selectedNote)
                    continue;
                const auto shown = effectiveVibrato(note);
                const auto absoluteStart = clip.startSeconds + note.startSeconds;
                const auto baseY = midiToY(note.midiNote) + rowHeight * 0.5f;
                const std::array<VibratoHandle, 6> order {
                    VibratoHandle::offset, VibratoHandle::depth, VibratoHandle::cycle,
                    VibratoHandle::fadeIn, VibratoHandle::fadeOut, VibratoHandle::length
                };
                for (const auto which : order)
                {
                    const auto place = vibratoHandlePlace(shown, which);
                    const juce::Point<float> centre(
                        timeToX(absoluteStart + place.timeSeconds)
                            + static_cast<float>(place.pixelOffsetX),
                        baseY - static_cast<float>(place.cents / 100.0) * rowHeight);
                    if (centre.getDistanceFrom(position) <= radius)
                        return VibratoHandleInfo { note.id, which, absoluteStart };
                }
            }
    }
    return std::nullopt;
}

float PianoRollComponent::diagnosticNoteY(std::size_t index) const
{
    std::size_t seen = 0;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (seen++ == index) return midiToY(note.midiNote) + rowHeight * 0.5f;
    return 0.0f;
}

std::array<double, 3> PianoRollComponent::diagnosticRegionEdges(std::size_t index) const
{
    std::array<double, 3> edges { -1.0, -1.0, -1.0 };
    std::size_t seen = 0;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (seen++ != index) continue;
                const auto span = utauSoundSpans.find(note.id.toStdString());
                if (span == utauSoundSpans.end()) return edges;
                const auto previewing = note.id == draggedNote
                    && dragMode == DragMode::consonantLeadIn;
                const auto resizing = note.id == draggedNote
                    && (dragMode == DragMode::resizeLeft
                        || dragMode == DragMode::resizeRight);
                const auto settledStart = clip.startSeconds + note.startSeconds;
                const auto absoluteStart = resizing
                    ? clip.startSeconds + previewStartSeconds : settledStart;
                const auto leadIn = settledStart - span->second.first;
                const auto settledEnd = settledStart + note.durationSeconds;
                const auto handover = span->second.second < settledEnd - 1.0e-9
                    ? span->second.second : std::numeric_limits<double>::max();
                const auto start = previewing
                    ? absoluteStart - previewConsonantPreutterance
                    : (resizing ? absoluteStart - leadIn : span->second.first);
                const auto finish = resizing
                    ? std::min(absoluteStart + previewDurationSeconds, handover)
                    : span->second.second;
                const auto length = finish - start;
                const auto fractions = jieFractionsFor(track, note, length,
                    previewing ? std::optional<double>(previewConsonantPreutterance)
                               : std::nullopt);
                if (!fractions) return edges;
                for (std::size_t boundary = 0; boundary < 3; ++boundary)
                    edges[boundary] = start + (*fractions)[boundary] * length;
                return edges;
            }
    return edges;
}

std::optional<std::array<double, 3>> PianoRollComponent::jieFractionsFor(
    const TrackData& track, const NoteData& note, double spanSeconds,
    std::optional<double> leadInOverride, int* regionsOut) const
{
    if (regionsOut != nullptr) *regionsOut = 4;
    if (!utauModeUsesRegions(track.utauMode) || spanSeconds <= 1.0e-9) return std::nullopt;
    const auto velocity = note.utauConsonantVelocity != inheritedUtauConsonantVelocity
        ? note.utauConsonantVelocity : track.utauConsonantVelocity;
    // 谋 annotates its entries, and the number of regions is one of the things
    // it says.  Read without that flag the roll never saw the annotation at
    // all and drew four boundaries into a note the entry splits in three.
    const auto mou = track.utauMode == UtauMode::mou;
    const auto timing = backend::UtauRenderer::sampleTiming(
        track.voicebankDirectory, note.label, note.midiNote, velocity, true, mou);
    const auto regions = timing && mou
        ? SampleSettings::mouRegionCount(timing->mouClasses) : 4;
    if (regionsOut != nullptr) *regionsOut = regions;
    // The lead-in this note really has.  A pinned one outranks the oto's, and
    // the block on screen is drawn against it -- read the oto's instead and
    // the first boundary lands wherever the two differ rather than on the note
    // start, which is the one place it is not allowed to leave.
    const auto settledLeadIn = timing
        ? (note.utauPreutteranceOverrideEnabled
               ? std::max(0.0, note.utauPreutteranceSeconds)
               : timing->preutteranceSeconds)
        : 0.0;
    if (note.utauJieSplitSet)
    {
        // The first line is not the split's to decide.  The onset ends where
        // the note starts, hand-placed boundaries or not, and that is exactly
        // what the renderer does with these fractions -- it reads the lead-in
        // and ignores the stored first one.  Drawing the stored value put the
        // consonant line somewhere the rendered consonant never was, and left
        // it there after a reset had moved the lead-in back.
        auto first = juce::jlimit(0.0, 1.0, note.utauJieSplit1);
        if (timing)
            first = juce::jlimit(0.0, 1.0, std::min(
                leadInOverride.value_or(settledLeadIn), spanSeconds) / spanSeconds);
        // Only the boundaries this entry has: the rest sit at the end of the
        // note, where nothing draws them and nothing can take hold of them.
        std::array<double, 3> hand { 1.0, 1.0, 1.0 };
        hand[0] = first;
        auto previous = first;
        const std::array<double, 3> stored { first, note.utauJieSplit2,
                                             note.utauJieSplit3 };
        for (int index = 1; index + 1 < regions; ++index)
        {
            previous = juce::jlimit(previous, 1.0,
                                    stored[static_cast<std::size_t>(index)]);
            hand[static_cast<std::size_t>(index)] = previous;
        }
        return hand;
    }
    if (!timing || !timing->hasRegions) return std::nullopt;
    // Same allocation the renderer will run, so the lines start where the
    // regions actually are rather than at some arbitrary default.
    // The sounding span begins a lead-in before the note, so that lead-in is
    // what the onset gets: it has to end where the note starts.  While one is
    // being dragged the block is already drawn against the lead-in the drag is
    // proposing, and the boundaries have to agree with the block they sit in
    // -- read from the settled timing instead, the consonant slid off the note
    // start for as long as the drag lasted, furthest when the drag was
    // lengthening it.
    const auto leadIn = std::min(leadInOverride.value_or(settledLeadIn), spanSeconds);
    // The same classes the renderer will use, so the lines on the roll are
    // where the regions really end up.
    const auto split = backend::UtauRenderer::regionSplit(
        timing->regionSeconds, spanSeconds, velocity, leadIn, nullptr,
        mou && timing->mouClasses.isNotEmpty() ? &timing->mouClasses : nullptr);
    if (!split.valid) return std::nullopt;
    std::array<double, 3> fractions {};
    auto cumulative = 0.0;
    for (std::size_t index = 0; index < 3; ++index)
    {
        cumulative += split.seconds[index];
        fractions[index] = juce::jlimit(0.0, 1.0, cumulative / spanSeconds);
    }
    return fractions;
}

int PianoRollComponent::diagnosticRegionCount(std::size_t index) const
{
    std::size_t seen = 0;
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (seen++ != index) continue;
                const auto span = utauSoundSpans.find(note.id.toStdString());
                if (span == utauSoundSpans.end()) return 4;
                auto regions = 4;
                jieFractionsFor(track, note, span->second.second - span->second.first,
                                std::nullopt, &regions);
                return regions;
            }
    return 4;
}

std::optional<PianoRollComponent::JieSplitHandleInfo>
PianoRollComponent::jieSplitHandleAt(juce::Point<float> position) const
{
    constexpr auto handleRadius = 5.0f;
    if (tool != Tool::note || sourceEditMode) return std::nullopt;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose || track.pitchAlgorithm != PitchAlgorithm::utau
            || !utauModeUsesRegions(track.utauMode) || !track.voicebankDirectory.isDirectory()
            || (focusedTrack.isNotEmpty() && track.id != focusedTrack))
            continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                const auto span = utauSoundSpans.find(note.id.toStdString());
                if (span == utauSoundSpans.end()) continue;
                const auto spanStart = span->second.first;
                const auto spanEnd = span->second.second;
                const auto spanSeconds = spanEnd - spanStart;
                auto regions = 4;
                const auto fractions = jieFractionsFor(track, note, spanSeconds,
                                                       std::nullopt, &regions);
                if (!fractions) continue;
                const auto top = midiToY(note.midiNote) + 2.0f;
                const auto height = std::max(6.0f, rowHeight - 4.0f);
                // The first line is where the oto says the consonant ends,
                // and it no longer moves with the note: dragging it would
                // only snap back.  Per note, the consonant's length is set by
                // the consonant velocity, and in the voicebank by the region
                // editor.
                for (int boundary = regions - 2; boundary >= 1; --boundary)
                {
                    const auto x = timeToX(spanStart
                        + (*fractions)[static_cast<std::size_t>(boundary)] * spanSeconds);
                    const juce::Rectangle<float> hit(x - handleRadius, top,
                                                     handleRadius * 2.0f, height);
                    if (!hit.contains(position)) continue;
                    return JieSplitHandleInfo { note.id, boundary, spanStart, spanEnd,
                                                *fractions, regions };
                }
            }
    }
    return std::nullopt;
}

std::optional<PianoRollComponent::ConsonantHandleInfo>
PianoRollComponent::consonantHandleAt(juce::Point<float> position) const
{
    constexpr auto handleRadius = 7.0f;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose || track.pitchAlgorithm != PitchAlgorithm::utau
            || !track.voicebankDirectory.isDirectory()
            || (focusedTrack.isNotEmpty() && track.id != focusedTrack))
            continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                const auto span = utauSoundSpans.find(note.id.toStdString());
                if (span == utauSoundSpans.end()) continue;
                const auto absoluteStart = clip.startSeconds + note.startSeconds;
                const auto handle = juce::Rectangle<float>(
                    timeToX(span->second.first) - handleRadius,
                    midiToY(note.midiNote) + 2.0f,
                    handleRadius * 2.0f, std::max(6.0f, rowHeight - 4.0f));
                if (!handle.contains(position)) continue;
                const auto currentVelocity =
                    note.utauConsonantVelocity != inheritedUtauConsonantVelocity
                    ? note.utauConsonantVelocity : track.utauConsonantVelocity;
                const auto base = backend::UtauRenderer::sampleTiming(
                    track.voicebankDirectory, note.label, note.midiNote, 100,
                    utauModeUsesRegions(track.utauMode),
            track.utauMode == UtauMode::mou);
                const auto current = backend::UtauRenderer::sampleTiming(
                    track.voicebankDirectory, note.label, note.midiNote,
                    currentVelocity, utauModeUsesRegions(track.utauMode),
                    track.utauMode == UtauMode::mou);
                const auto slowest = backend::UtauRenderer::sampleTiming(
                    track.voicebankDirectory, note.label, note.midiNote,
                    std::numeric_limits<int>::min() + 1,
                    utauModeUsesRegions(track.utauMode),
                    track.utauMode == UtauMode::mou);
                if (!base || !current || !slowest) continue;
                // A first region 谋 does not call a consonant: the consonant
                // velocity does not reach it, so a drag converted into one
                // moved nothing at all and rewrote a setting the note carries
                // for nothing.  Such a note has its lead-in set directly.  One
                // 谋 calls a consonant is a consonant, and goes on speaking
                // through the velocity as it does in 界.
                const auto mou = track.utauMode == UtauMode::mou;
                const auto firstIsConsonant = base->mouClasses.isEmpty()
                    || base->mouClasses[0] != 'V';
                const auto freeLeadIn = mou && !firstIsConsonant;
                const auto scaledPart = std::min(base->preutteranceSeconds,
                                                  base->consonantSeconds);
                const auto unscaledPart = std::max(0.0,
                    base->preutteranceSeconds - base->consonantSeconds);
                if (!freeLeadIn && scaledPart <= 1.0e-6) continue;
                // Approaching the unscaled part corresponds to increasingly
                // large velocity values.  A tiny positive scale keeps log2
                // finite while removing legacy UTAU's artificial 200 ceiling.
                auto minimum = std::min(absoluteStart,
                    unscaledPart + scaledPart * 1.0e-12);
                // Negative velocities lengthen the consonant.  The latest
                // possible handle is limited by the sample's available audio
                // and by the beginning of the project, not by velocity zero.
                auto maximum = std::min(absoluteStart,
                    slowest->preutteranceSeconds);
                if (freeLeadIn)
                {
                    // Free between the note in front of it and its own start,
                    // and no further back than the entry has audio to give.
                    auto previousStart = 0.0;
                    for (const auto& other : clip.notes)
                    {
                        const auto start = clip.startSeconds + other.startSeconds;
                        if (start < absoluteStart - 1.0e-9)
                            previousStart = std::max(previousStart, start);
                    }
                    minimum = 0.0;
                    maximum = std::min({ absoluteStart, absoluteStart - previousStart,
                                         base->sampleSeconds });
                }
                if (maximum - minimum <= 1.0e-5) continue;
                const auto overlap = note.utauOverlapOverrideEnabled
                    ? note.utauOverlapSeconds : current->overlapSeconds;
                // A pinned lead-in is where the handle is now, not what the
                // oto would have given it.
                const auto settled = note.utauPreutteranceOverrideEnabled
                    ? std::max(0.0, note.utauPreutteranceSeconds)
                    : current->preutteranceSeconds;
                return ConsonantHandleInfo {
                    note.id, absoluteStart, scaledPart, unscaledPart,
                    minimum, maximum,
                    juce::jlimit(minimum, maximum, settled),
                    currentVelocity, freeLeadIn, overlap
                };
            }
    }
    return std::nullopt;
}

int PianoRollComponent::noteEditDivision() const
{
    // 1/N of a beat.  The default 64 is a quarter of the 1/64-bar unit this
    // started out as, which in 4/4 is the 1/256-bar quantum editing has always
    // used -- the arithmetic below keeps that true in every time signature.
    return juce::jlimit(2, 128, snapshot.noteEditDivision);
}

double PianoRollComponent::noteEditQuantumSeconds() const
{
    const auto beatSeconds = 60.0 / juce::jlimit(20.0, 400.0,
                                                snapshot.tempoAtSeconds(playheadSeconds));
    const auto barSeconds = beatSeconds * static_cast<double>(snapshot.numerator)
        * 4.0 / static_cast<double>(std::max(1, snapshot.denominator));
    return std::max(0.001, barSeconds / (4.0 * static_cast<double>(noteEditDivision())));
}

void PianoRollComponent::beginInlineAliasEdit(const NoteHit& hit)
{
    auto currentAlias = juce::String{};
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == hit.id)
                    currentAlias = note.label;

    inlineAliasNoteId = hit.id;
    inlineAliasEditor.setText(currentAlias, false);
    auto bounds = hit.bounds;
    bounds.setWidth(std::max(84.0f, bounds.getWidth()));
    bounds.setHeight(std::max(24.0f, bounds.getHeight()));
    if (bounds.getRight() > static_cast<float>(getWidth()) - 2.0f)
        bounds.setX(std::max(60.0f, static_cast<float>(getWidth()) - bounds.getWidth() - 2.0f));
    inlineAliasEditor.setBounds(bounds.getSmallestIntegerContainer().expanded(1, 1));
    inlineAliasEditor.setVisible(true);
    inlineAliasEditor.toFront(false);
    inlineAliasEditor.grabKeyboardFocus();
    inlineAliasEditor.selectAll();
}

void PianoRollComponent::finishInlineAliasEdit(bool accept)
{
    if (inlineAliasNoteId.isEmpty()) return;
    const auto noteId = inlineAliasNoteId;
    inlineAliasNoteId.clear();
    auto alias = inlineAliasEditor.getText().trim();
    if (alias.endsWithIgnoreCase(".wav"))
        alias = alias.dropLastCharacters(4).trim();
    inlineAliasEditor.setVisible(false);
    if (accept)
    {
        model.setNoteLabel(noteId, alias);
        ensureDefaultEnvelope(noteId);
        if (onNoteAliasCommitted) onNoteAliasCommitted(noteId);
        if (onNoteSelected) onNoteSelected(noteId);
    }
    repaint();
}

void PianoRollComponent::lookAndFeelChanged()
{
    // The inline alias editor caches Palette colours; re-apply them so a theme
    // switch does not leave it on the previous theme's (unreadable) colours.
    inlineAliasEditor.setColour(juce::TextEditor::backgroundColourId,
                                Palette::panelRaised.withAlpha(0.98f));
    inlineAliasEditor.setColour(juce::TextEditor::textColourId, Palette::text);
    inlineAliasEditor.setColour(juce::TextEditor::outlineColourId, Palette::accentLight);
    inlineAliasEditor.setColour(juce::TextEditor::focusedOutlineColourId, Palette::noteLight);
    repaint();
}

void PianoRollComponent::paint(juce::Graphics& g)
{
    // Only the exposed strip is worth drawing.  At ordinary zoom the canvas
    // runs twenty times the width of the window, so anything that walked the
    // whole piece -- notes, grid lines, bar marks -- spent almost all of its
    // time on pixels the clip then threw away.
    const auto visibleArea = g.getClipBounds().toFloat();

    for (int midi = lowestMidi; midi <= highestMidi; ++midi)
    {
        const auto y = midiToY(static_cast<float>(midi));
        const auto black = juce::MidiMessage::isMidiNoteBlack(midi);
        if (black)
        {
            g.setColour(juce::Colours::black.withAlpha(0.16f));
            g.fillRect(0.0f, y, static_cast<float>(getWidth()), rowHeight);
        }
        g.setColour(Palette::grid);
        g.drawHorizontalLine(static_cast<int>(y), 0.0f, static_cast<float>(getWidth()));
    }

    auto keyboardX = 0;
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
        keyboardX = viewport->getViewPositionX();
    const auto drawKeyboard = [&]
    {
        g.setColour(Palette::panelRaised);
        g.fillRect(keyboardX, 0, 58, getHeight());
        for (int midi = lowestMidi; midi <= highestMidi; ++midi)
        {
            const auto y = midiToY(static_cast<float>(midi));
            if (juce::MidiMessage::isMidiNoteBlack(midi))
            {
                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.fillRect(static_cast<float>(keyboardX), y, 38.0f, rowHeight);
            }
            if (keyboardLabelVisible(midi, rowHeight))
            {
                // A sharp goes on the black key itself, where there is width
                // for three characters; a natural goes on the pale strip to
                // the right of them.  C stays brighter than the rest so an
                // octave can still be found at a glance.
                const auto black = juce::MidiMessage::isMidiNoteBlack(midi);
                g.setColour(midi % 12 == 0 ? Palette::text
                           : black ? Palette::textMuted.brighter(0.40f)
                                   : Palette::textMuted);
                g.setFont(juce::jlimit(7.0f, 10.0f, rowHeight - 2.0f));
                const auto name = juce::MidiMessage::getMidiNoteName(midi, true, true, 4);
                if (black)
                    g.drawText(name, keyboardX + 3, static_cast<int>(y),
                               32, static_cast<int>(rowHeight),
                               juce::Justification::centredRight);
                else
                    g.drawText(name, keyboardX + 39, static_cast<int>(y),
                               18, static_cast<int>(rowHeight),
                               juce::Justification::centred);
            }
        }
    };

    const auto gridStep = gridQuarterNotes();
    const auto firstVisibleSeconds = std::max(0.0,
        static_cast<double>(visibleArea.getX() - 58.0f) / pixelsPerSecond);
    const auto firstQuarter = snapshot.quarterPositionForSeconds(firstVisibleSeconds);
    const auto firstTick = std::floor(firstQuarter / gridStep) - 1.0;
    const auto barQuarters = static_cast<double>(std::max(1, snapshot.numerator))
        * 4.0 / static_cast<double>(std::max(1, snapshot.denominator));
    for (double tick = firstTick;; tick += 1.0)
    {
        const auto quarter = tick * gridStep;
        const auto seconds = snapshot.secondsForQuarterPosition(quarter);
        const auto x = timeToX(seconds);
        if (x > visibleArea.getRight() || x > static_cast<float>(getWidth())) break;
        if (x < 58.0f) continue;
        const auto isBeat = std::abs(quarter - std::round(quarter)) < 1.0e-6;
        const auto isBar = std::abs(quarter / barQuarters
                                    - std::round(quarter / barQuarters)) < 1.0e-6;
        g.setColour(isBar ? Palette::beatGrid.brighter(0.35f)
                   : isBeat ? Palette::beatGrid.withAlpha(0.60f) : Palette::beatGrid.withAlpha(0.30f));
        g.drawVerticalLine(static_cast<int>(x), 0.0f, static_cast<float>(getHeight()));
    }

    for (const auto& change : snapshot.tempoChanges)
    {
        const auto x = timeToX(snapshot.secondsForQuarterPosition(change.quarterPosition));
        if (x < 58.0f || x > getWidth()) continue;
        g.setColour(juce::Colour(0xffffa94d).withAlpha(0.72f));
        g.drawVerticalLine(static_cast<int>(x), 0.0f, static_cast<float>(getHeight()));
    }

    if (showWaveforms) drawClipWaveforms(g);
    if (showUtauWaveforms) drawUtauNoteWaveforms(g);

    // In wrench mode all timing edits are made against the untouched source.
    // The four handles mirror the main-branch HJM editor and intentionally sit
    // above the note hit targets so a boundary can always be grabbed.
    if (sourceEditMode)
    {
        for (int index = 0; index < static_cast<int>(sampleRegions.size()); ++index)
        {
            const auto& region = sampleRegions[static_cast<std::size_t>(index)];
            const auto selected = index == activeSampleRegion;
            const auto startX = timeToX(region.regionStartSeconds);
            const auto endX = timeToX(region.regionEndSeconds);
            const auto fixedX = timeToX(region.regionStartSeconds + region.fixedDurationSeconds);
            const auto alignmentX = timeToX(region.alignmentSeconds);
            const auto band = juce::Rectangle<float>(startX, 4.0f,
                std::max(1.0f, endX - startX), static_cast<float>(getHeight() - 8));
            g.setColour(Palette::accentLight.withAlpha(selected ? 0.105f : 0.035f));
            g.fillRect(band);

            const auto drawHandle = [&](float x, RegionHandle handle, juce::Colour colour,
                                        float thickness, bool dashed)
            {
                juce::Path path;
                path.startNewSubPath(x, 4.0f);
                path.lineTo(x, static_cast<float>(getHeight() - 4));
                g.setColour(colour.withAlpha(selected ? 0.94f : 0.48f));
                if (dashed)
                {
                    const float dashes[] { 5.0f, 4.0f };
                    juce::Path dashedPath;
                    juce::PathStrokeType(thickness).createDashedStroke(dashedPath, path, dashes, 2);
                    g.fillPath(dashedPath);
                }
                else g.strokePath(path, juce::PathStrokeType(thickness));
                regionHandleHits.push_back({ index, handle, x });
            };
            drawHandle(startX, RegionHandle::start, Palette::accentLight, selected ? 2.2f : 1.0f, false);
            if (region.fixedDurationSeconds > 0.0)
                drawHandle(fixedX, RegionHandle::fixedEnd, Palette::noteLight,
                           selected ? 2.0f : 1.0f, true);
            drawHandle(alignmentX, RegionHandle::alignment, Palette::noteFill,
                       selected ? 2.4f : 1.2f, false);
            drawHandle(endX, RegionHandle::end, Palette::accentLight,
                       selected ? 2.2f : 1.0f, false);

            if (selected)
            {
                const auto label = region.name.isEmpty() ? "region " + juce::String(index + 1)
                                                         : region.name;
                const auto labelBounds = juce::Rectangle<float>(startX + 3.0f, 6.0f,
                    std::max(40.0f, endX - startX - 6.0f), 18.0f);
                g.setColour(Palette::panelRaised.withAlpha(0.88f));
                g.fillRoundedRectangle(labelBounds, 3.0f);
                g.setColour(Palette::text);
                g.setFont(11.0f);
                g.drawFittedText(label, labelBounds.toNearestInt().reduced(4, 0),
                                 juce::Justification::centredLeft, 1);
            }
        }
    }

    static const std::array<juce::Colour, 5> contourColours {
        Palette::accentLight, Palette::accent, Palette::noteFill,
        juce::Colour(0xff45b8aa), juce::Colour(0xffad7ad6)
    };
    struct PositionedNote
    {
        juce::String id;
        int midiRow = 0;
        double start = 0.0;
        double end = 0.0;
    };
    std::vector<PositionedNote> visibleNotes;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose && !sourceEditMode) continue;
        if (!sourceEditMode && focusedTrack.isNotEmpty() && track.id != focusedTrack) continue;
        for (const auto& clip : track.clips)
        {
            if (sourceEditMode && clip.id != focusedClip) continue;
            const auto sourceScale = sourceEditMode && clip.durationSeconds > 1.0e-9
                ? (clip.sourceDurationSeconds > 1.0e-9 ? clip.sourceDurationSeconds : clip.durationSeconds)
                    / clip.durationSeconds : 1.0;
            for (const auto& note : clip.notes)
            {
                const auto start = sourceEditMode
                    ? clip.sourceOffsetSeconds + note.startSeconds * sourceScale
                    : clip.startSeconds + note.startSeconds;
                visibleNotes.push_back({ note.id, static_cast<int>(std::lround(note.midiNote)),
                                         start, start + note.durationSeconds * sourceScale });
            }
        }
    }
    std::stable_sort(visibleNotes.begin(), visibleNotes.end(), [](const auto& left, const auto& right)
    {
        if (left.midiRow != right.midiRow) return left.midiRow < right.midiRow;
        if (std::abs(left.start - right.start) > 1.0e-9) return left.start < right.start;
        return left.end < right.end;
    });
    // Note blocks are drawn at a uniform full-row height (z-order handles
    // overlap), so the previous compact-sub-channel lane bookkeeping is no
    // longer needed.
    std::size_t trackIndex = 0;
    for (const auto& track : snapshot.tracks)
    {
        if (!track.compose && !sourceEditMode) { ++trackIndex; continue; }
        if (!sourceEditMode && focusedTrack.isNotEmpty() && track.id != focusedTrack)
        {
            ++trackIndex;
            continue;
        }
        const auto originalColour = contourColours[trackIndex % contourColours.size()];
        for (const auto& clip : track.clips)
        {
            if (sourceEditMode && clip.id != focusedClip)
                continue;
            const auto sourceScale = sourceEditMode && clip.durationSeconds > 1.0e-9
                ? (clip.sourceDurationSeconds > 1.0e-9 ? clip.sourceDurationSeconds : clip.durationSeconds)
                    / clip.durationSeconds
                : 1.0;
            for (const auto& note : clip.notes)
            {
                const auto movingSelected = dragMode == DragMode::moveUtauNote
                    && selectedNotes.contains(note.id.toStdString());
                const auto previewingResize = note.id == draggedNote
                    && (dragMode == DragMode::resizeLeft
                        || dragMode == DragMode::resizeRight);
                const auto effectiveStart = previewingResize
                    ? previewStartSeconds
                    : note.startSeconds + (movingSelected
                        ? previewMoveDeltaSeconds : 0.0);
                const auto effectiveDuration = previewingResize
                    ? previewDurationSeconds : note.durationSeconds;
                const auto effectiveMidi = juce::jlimit(0.0f, 127.0f,
                    note.midiNote + (movingSelected
                        ? previewMidi - dragStartMidi : 0.0f));
                const auto absoluteStart = sourceEditMode
                    ? clip.sourceOffsetSeconds + effectiveStart * sourceScale
                    : clip.startSeconds + effectiveStart;
                const auto x = timeToX(absoluteStart);
                const auto y = midiToY(effectiveMidi);
                const auto width = std::max(5.0f, static_cast<float>(effectiveDuration * sourceScale)
                                                     * pixelsPerSecond);
                const auto midiRow = static_cast<int>(std::lround(note.midiNote));
                // Note blocks keep a uniform full-row height regardless of how
                // many notes overlap on the same MIDI row.  Melodyne's editor
                // draws overlapping notes at the same row height with z-order
                // rather than shrinking each into a compact sub-channel, so
                // the previous `laneHeight = (rowHeight - 4) / laneCount`
                // behaviour is removed: the "amount" of overlap must not
                // change the visible size of a note.
                (void) midiRow;
                const auto blockHeight = std::max(6.0f, rowHeight - 4.0f);
                const auto bounds = juce::Rectangle<float>(x, y + 2.0f, width, blockHeight);
                auto displayBounds = bounds;
                // The visual editor is shared by both workflows.  Melodyne
                // compatible notes use the same UTAU-style block, contour,
                // boundary and baseline drawing; only the data operations
                // below remain gated by the real track algorithm.
                const auto utauMode = true;
                const auto dataIsUtau = track.pitchAlgorithm == PitchAlgorithm::utau;
                const auto previewingConsonant = note.id == draggedNote
                    && dragMode == DragMode::consonantLeadIn;
                auto displaySpanSeconds = 0.0;
                std::optional<double> displayLeadIn;
                if (dataIsUtau)
                {
                    if (const auto span = utauSoundSpans.find(note.id.toStdString());
                        span != utauSoundSpans.end())
                    {
                        // A resize in progress moves the note under the
                        // block, so the block has to move with it: left where
                        // the settled span put it, it stood still while the
                        // note grew and then jumped on release, which reads as
                        // the consonant stretching and shifting.  The lead-in
                        // is carried across unchanged -- resizing a note is
                        // not a statement about its consonant.
                        const auto settledStart = clip.startSeconds + note.startSeconds;
                        const auto leadIn = settledStart - span->second.first;
                        const auto settledEnd = settledStart + note.durationSeconds;
                        // Where the next note cuts this one short, if it does.
                        const auto handover = span->second.second
                            < settledEnd - 1.0e-9
                                ? span->second.second
                                : std::numeric_limits<double>::max();
                        const auto soundingStart = previewingConsonant
                            ? absoluteStart - previewConsonantPreutterance
                            : (previewingResize
                                ? absoluteStart - leadIn
                                : span->second.first + (movingSelected
                                    ? previewMoveDeltaSeconds : 0.0));
                        const auto soundingEnd = previewingResize
                            ? std::min(absoluteStart + effectiveDuration, handover)
                            : span->second.second
                                + (movingSelected ? previewMoveDeltaSeconds : 0.0);
                        displaySpanSeconds = soundingEnd - soundingStart;
                        if (previewingConsonant)
                            displayLeadIn = previewConsonantPreutterance;
                        const auto soundingX = timeToX(soundingStart);
                        const auto soundingWidth = std::max(2.0f,
                            timeToX(soundingEnd) - soundingX);
                        displayBounds = juce::Rectangle<float>(
                            soundingX, bounds.getY() + 1.0f,
                            soundingWidth, bounds.getHeight() - 2.0f);
                    }
                }
                else
                {
                    // Native HJM notes already carry their source-region
                    // timing.  Use the same sounding rectangle for imported
                    // Melodyne/MIDI material instead of waiting for an OTO
                    // lookup, so the native boundary is visible immediately.
                    displaySpanSeconds = effectiveDuration * sourceScale;
                    // The imported onset is already inside the recording.
                    // Alignment is its internal line, not additional lead-in
                    // audio outside the clip's selected source range.
                    displayLeadIn = note.consonantSeconds;
                }
                // The sounding block reaches furthest left, the nominal block
                // furthest right, and together they bound everything drawn
                // below.  Vertical extent is deliberately not tested: a pitch
                // line may leave the note's own row entirely.
                const auto drawnExtent = displayBounds.getUnion(bounds);
                if (cullOffscreenNotes
                    && (drawnExtent.getRight() < visibleArea.getX() - 2.0f
                        || drawnExtent.getX() > visibleArea.getRight() + 2.0f))
                    continue;
                if (utauMode)
                {
                    // A rest holds its stretch of the phrase open and sounds
                    // nothing, so it has no regions to be shaded with and no
                    // sounding block to be drawn as.  It gets an empty one of
                    // its own, which is also what keeps it visible with the
                    // note-range outline turned off.
                    if (dataIsUtau && backend::isRestLyric(note.label))
                    {
                        g.setColour(Palette::panelRaised.withAlpha(0.55f));
                        g.fillRoundedRectangle(bounds, 4.0f);
                        g.setColour(Palette::textMuted.withAlpha(0.75f));
                        g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
                    }
                    if (showNoteRange)
                    {
                        g.setColour(juce::Colour(0xffffbd78).withAlpha(0.82f));
                        g.drawRoundedRectangle(displayBounds, 3.0f, 1.5f);
                    }

                    // Four-region split: three boundaries inside the
                    // sounding block.  Draggable in note-edit mode; in point
                    // mode they are shown faintly and cannot be touched, as
                    // marks to place a pitch point against -- where the
                    // consonant gives way to the vowel is exactly where a
                    // pitch move usually wants to be, and having to leave the
                    // tool to see it was a poor way to find out.
                    const auto guideOnly = tool != Tool::note;
                    if (utauModeUsesRegions(track.utauMode)
                        && dataIsUtau
                        && (tool == Tool::note || tool == Tool::points))
                    {
                        const auto dragging = note.id == draggedNote
                            && dragMode == DragMode::jieSplit;
                        const auto spanSeconds = std::max(1.0e-9,
                            jieSpanEndSeconds - jieSpanStartSeconds);
                        (void) spanSeconds;
                        std::optional<std::array<double, 3>> fractions;
                        auto regions = 4;
                        if (dragging)
                        {
                            fractions = previewJieFractions;
                            regions = previewJieRegions;
                        }
                        else if (displaySpanSeconds > 1.0e-9)
                            fractions = jieFractionsFor(track, note,
                                displaySpanSeconds, displayLeadIn, &regions);
                        if (fractions)
                        {
                            static const std::array<juce::Colour, 4> regionColours {
                                juce::Colour(0xffff7043), juce::Colour(0xffffca28),
                                juce::Colour(0xff4fc3f7), juce::Colour(0xff81c784)
                            };
                            const auto left = displayBounds.getX();
                            const auto width = displayBounds.getWidth();
                            // As many bands as the entry has regions, and one
                            // boundary fewer -- a three-region entry drew a
                            // fourth band of no width and a line on the note's
                            // own end.
                            const auto bands = static_cast<std::size_t>(
                                juce::jlimit(2, 4, regions));
                            std::array<float, 5> edges { left, 0.0f, 0.0f, 0.0f,
                                                         left + width };
                            for (std::size_t index = 0; index + 1 < bands; ++index)
                                edges[index + 1] = left + width
                                    * static_cast<float>((*fractions)[index]);
                            edges[bands] = left + width;
                            for (std::size_t index = 0; index < bands; ++index)
                            {
                                const auto tint = std::max(0.0f,
                                    edges[index + 1] - edges[index]);
                                if (tint <= 0.5f) continue;
                                g.setColour(regionColours[index].withAlpha(
                                    (note.utauJieSplitSet ? 0.30f : 0.17f)
                                        * (guideOnly ? 0.5f : 1.0f)));
                                g.fillRect(juce::Rectangle<float>(edges[index],
                                    displayBounds.getY() + 1.0f, tint,
                                    displayBounds.getHeight() - 2.0f));
                            }
                            for (std::size_t index = 0; index + 1 < bands; ++index)
                            {
                                g.setColour(regionColours[index + 1].withAlpha(
                                    guideOnly ? 0.42f : (dragging ? 1.0f : 0.9f)));
                                g.drawLine(edges[index + 1],
                                    displayBounds.getY() + 1.0f, edges[index + 1],
                                    displayBounds.getBottom() - 1.0f,
                                    guideOnly ? 1.0f : (dragging ? 2.4f : 1.6f));
                            }
                        }
                    }

                    // HJM is more expressive than OTO: draw every native
                    // segment, including arbitrary vowel subdivisions and the
                    // '_' transition placeholders.  Unknown '-' regions stay
                    // visible but subdued until the user supplies an alias.
                    if (!note.nativeSegments.empty())
                    {
                        const auto segmentXAt = [&](double sourceLocal)
                        {
                            const auto sourceAbsolute = note.nativeSourceStartSeconds >= 0.0
                                ? note.nativeSourceStartSeconds + sourceLocal
                                : clip.sourceOffsetSeconds + note.startSeconds + sourceLocal;
                            if (sourceEditMode) return timeToX(sourceAbsolute);
                            const auto source = sourceAbsolute - clip.sourceOffsetSeconds;
                            auto target = clip.sourceDurationSeconds > 1.0e-9
                                ? source * clip.durationSeconds / clip.sourceDurationSeconds : source;
                            const auto& map = clip.sourceTimeMap;
                            if (!map.empty())
                            {
                                target = map.back().targetSeconds;
                                if (source <= map.front().sourceSeconds) target = map.front().targetSeconds;
                                else for (std::size_t i = 1; i < map.size(); ++i)
                                    if (source <= map[i].sourceSeconds)
                                    {
                                        const auto span = map[i].sourceSeconds - map[i-1].sourceSeconds;
                                        const auto u = span > 1.0e-9 ? (source-map[i-1].sourceSeconds)/span : 0.0;
                                        target = map[i-1].targetSeconds + u*(map[i].targetSeconds-map[i-1].targetSeconds);
                                        break;
                                    }
                            }
                            return timeToX(clip.startSeconds + target);
                        };
                        const auto segmentColour = [](const NativeSegment& segment)
                        {
                            switch (segment.role)
                            {
                                case NativeSegmentRole::consonant:
                                    return Palette::noteLight;
                                case NativeSegmentRole::transition:
                                    return juce::Colour(0xff79c9c3);
                                case NativeSegmentRole::unknown:
                                    return Palette::textMuted;
                                case NativeSegmentRole::silence:
                                    return Palette::panelRaised;
                                case NativeSegmentRole::breath:
                                    return juce::Colour(0xff9fc6e8);
                                case NativeSegmentRole::noise:
                                    return juce::Colour(0xffc2a7de);
                                case NativeSegmentRole::ending:
                                    return juce::Colour(0xff6ea6a2);
                                case NativeSegmentRole::vowel:
                                    return segment.stretchable ? juce::Colour(0xff81c784)
                                                               : juce::Colour(0xff4fc3f7);
                            }
                            return Palette::noteFill;
                        };
                        for (std::size_t index = 0; index < note.nativeSegments.size(); ++index)
                        {
                            const auto& segment = note.nativeSegments[index];
                            const auto segmentX = segmentXAt(segment.sourceStartSeconds);
                            const auto segmentRight = segmentXAt(segment.sourceEndSeconds);
                            const auto segmentWidth = std::max(0.0f,
                                segmentRight - segmentX);
                            if (segmentWidth <= 0.5f) continue;
                            const auto alpha = segment.role == NativeSegmentRole::unknown
                                ? 0.12f : segment.role == NativeSegmentRole::transition
                                    ? 0.25f : 0.20f;
                            g.setColour(segmentColour(segment).withAlpha(alpha));
                            g.fillRect(juce::Rectangle<float>(segmentX,
                                displayBounds.getY() + 1.0f, segmentWidth,
                                displayBounds.getHeight() - 2.0f));
                            if (index > 0)
                            {
                                g.setColour(segmentColour(segment).withAlpha(
                                    segment.role == NativeSegmentRole::unknown ? 0.42f : 0.86f));
                                g.drawVerticalLine(static_cast<int>(std::lround(segmentX)),
                                    displayBounds.getY() + 1.0f,
                                    displayBounds.getBottom() - 1.0f);
                            }
                            const auto alias = segment.alias.trim().isEmpty()
                                ? juce::String("-") : segment.alias.trim();
                            if (segmentWidth >= 22.0f && (note.nativeSegments.size() > 1
                                || alias == "-" || alias == "_"))
                            {
                                g.setColour(Palette::text.withAlpha(
                                    segment.role == NativeSegmentRole::unknown ? 0.60f : 0.86f));
                                g.setFont(std::min(10.0f,
                                    std::max(7.0f, displayBounds.getHeight() - 3.0f)));
                                g.drawFittedText(alias,
                                    juce::Rectangle<float>(segmentX + 3.0f,
                                        displayBounds.getY(),
                                        std::max(1.0f, segmentWidth - 6.0f),
                                        displayBounds.getHeight()).toNearestInt(),
                                    juce::Justification::centredLeft, 1);
                            }
                        }
                        const auto alignment = note.nativeSegments.front().alignmentSeconds;
                        if (alignment > 1.0e-9)
                        {
                            const auto alignmentX = segmentXAt(alignment);
                            const float dash[] { 3.0f, 2.0f };
                            juce::Path line;
                            line.startNewSubPath(alignmentX, displayBounds.getY());
                            line.lineTo(alignmentX, displayBounds.getBottom());
                            juce::Path dashed;
                            juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dash, 2);
                            g.setColour(Palette::accentLight.withAlpha(0.92f));
                            g.fillPath(dashed);
                        }
                    }

                    // The body carries its own amplitude envelope: the top
                    // edge follows the gain, so a glance says where the note
                    // fades in, holds and falls away.  Drawn with exactly the
                    // geometry the amplitude tool uses for its curve, so the
                    // two are the same line seen through different tools
                    // rather than two pictures of one envelope; the tool draws
                    // its own, so the shape steps aside there.
                    if (showEnvelope && tool != Tool::amplitude)
                    {
                        const auto envelope = draggedNote == note.id
                                && dragMode == DragMode::amplitudePoint
                                && !amplitudeStroke.empty()
                            ? amplitudeStroke : amplitudeEnvelopeFor(note, absoluteStart);
                        if (envelope.size() >= 2)
                        {
                            const auto floorY = amplitudeY(effectiveMidi, -200.0f);
                            juce::Path shape;
                            juce::Path ridge;
                            for (std::size_t index = 0; index < envelope.size(); ++index)
                            {
                                const auto point = juce::Point<float>(
                                    timeToX(absoluteStart + envelope[index].timeSeconds),
                                    amplitudeY(effectiveMidi, envelope[index].gainDb));
                                if (index == 0)
                                {
                                    shape.startNewSubPath(point.x, floorY);
                                    ridge.startNewSubPath(point);
                                }
                                else ridge.lineTo(point);
                                shape.lineTo(point);
                            }
                            shape.lineTo(timeToX(absoluteStart
                                + envelope.back().timeSeconds), floorY);
                            shape.closeSubPath();
                            const auto lit = selectedNotes.contains(note.id.toStdString())
                                || note.id == selectedNote;
                            // An envelope runs to the note's nominal end while
                            // the note may stop sounding earlier, where the one
                            // after it takes over.  Clipping to the sounding
                            // block keeps the shape off the silence past it.
                            juce::Graphics::ScopedSaveState clipped(g);
                            // Horizontally the sounding block, but open above
                            // it: a gain over unity belongs outside the note,
                            // and cutting it off would read as a flat ceiling.
                            g.reduceClipRegion(displayBounds
                                .withTop(displayBounds.getY()
                                         - displayBounds.getHeight())
                                .getSmallestIntegerContainer());
                            g.setColour(juce::Colour(0xff72d6aa)
                                .withAlpha(lit ? 0.30f : 0.16f));
                            g.fillPath(shape);
                            g.setColour(juce::Colour(0xff72d6aa)
                                .withAlpha(lit ? 0.95f : 0.60f));
                            g.strokePath(ridge, juce::PathStrokeType(lit ? 1.8f : 1.2f));
                        }
                    }

                    // Keep a narrow warm baseline for the nominal note extent;
                    // the note body itself remains the shared blue-green style.
                    const auto nominalBar = juce::Rectangle<float>(
                        bounds.getX(), bounds.getBottom() - 2.0f,
                        bounds.getWidth(), 3.5f);
                    g.setColour(juce::Colour(0xffe7a34b));
                    g.fillRoundedRectangle(nominalBar, 1.75f);
                    if (selectedNotes.contains(note.id.toStdString())
                        && displayBounds.getX() < bounds.getX() - 0.5f)
                    {
                        g.setColour(juce::Colour(0xffffd19a));
                        g.fillRoundedRectangle(juce::Rectangle<float>(
                            displayBounds.getX() - 1.5f, displayBounds.getY(),
                            3.0f, displayBounds.getHeight()), 1.5f);
                    }
                    if (note.id == draggedNote
                        && dragMode == DragMode::consonantLeadIn)
                    {
                        g.setColour(Palette::text);
                        g.setFont(10.5f);
                        g.drawText(juce::String::fromUTF8(dataIsUtau ? "辅音速度 " : "先行 ")
                                + juce::String(previewConsonantVelocity),
                            static_cast<int>(displayBounds.getX() + 6.0f),
                            static_cast<int>(displayBounds.getY() - 19.0f),
                            104, 18, juce::Justification::centredLeft, false);
                    }
                }
                else
                {
                    g.setColour(Palette::noteFill.withAlpha(0.90f));
                    g.fillRoundedRectangle(bounds, 4.0f);
                    const auto consonantWidth = juce::jlimit(0.0f, width,
                        static_cast<float>(note.consonantSeconds * sourceScale) * pixelsPerSecond);
                    g.setColour(Palette::noteLight.withAlpha(0.38f));
                    g.fillRoundedRectangle(bounds.withWidth(consonantWidth), 4.0f);
                    g.setColour(Palette::noteLight.withAlpha(0.82f));
                    g.drawRoundedRectangle(bounds, 3.0f, 1.2f);
                }
                if (selectedNotes.contains(note.id.toStdString()))
                {
                    g.setColour(Palette::text.withAlpha(0.95f));
                    g.drawRoundedRectangle(displayBounds.reduced(1.0f), 3.0f, 1.8f);
                }

                const auto displayLabel = note.id == inlineAliasNoteId
                    ? inlineAliasEditor.getText() : note.label;
                if (displayLabel.isNotEmpty() && displayBounds.getWidth() >= 18.0f)
                {
                    g.setColour(Palette::text.withAlpha(0.86f));
                    g.setFont(std::min(11.0f, std::max(8.0f, displayBounds.getHeight() - 2.0f)));
                    g.drawFittedText(displayLabel, displayBounds.toNearestInt().reduced(4, 0),
                                     juce::Justification::centredLeft, 1);
                }

                if (utauMode && tool == Tool::amplitude)
                {
                    const auto envelope = draggedNote == note.id
                            && dragMode == DragMode::amplitudePoint
                            && !amplitudeStroke.empty()
                        ? amplitudeStroke : amplitudeEnvelopeFor(note, absoluteStart);
                    const auto noteSelected = selectedNotes.contains(note.id.toStdString())
                        || note.id == selectedNote;

                    // A faint 0 dB reference makes the vertical scale readable
                    // without adding a separate automation lane.
                    g.setColour(juce::Colour(0xff72d6aa).withAlpha(0.18f));
                    g.drawHorizontalLine(static_cast<int>(amplitudeY(note.midiNote, 0.0f)),
                        displayBounds.getX(), displayBounds.getRight());

                    juce::Path envelopePath;
                    for (std::size_t index = 0; index < envelope.size(); ++index)
                    {
                        const auto point = juce::Point<float>(
                            timeToX(absoluteStart + envelope[index].timeSeconds),
                            amplitudeY(note.midiNote, envelope[index].gainDb));
                        if (index == 0) envelopePath.startNewSubPath(point);
                        else envelopePath.lineTo(point);
                    }
                    const auto envelopeColour = juce::Colour(0xff72d6aa);
                    g.setColour(envelopeColour.withAlpha(noteSelected ? 0.98f : 0.62f));
                    g.strokePath(envelopePath, juce::PathStrokeType(noteSelected ? 2.2f : 1.5f));
                }

                g.setColour(Palette::textMuted.withAlpha(0.55f));
                const float dash[] { 4.0f, 3.0f };
                juce::Path boundary;
                boundary.startNewSubPath(bounds.getX(), bounds.getY() - rowHeight * 2.0f);
                boundary.lineTo(bounds.getX(), bounds.getBottom() + rowHeight * 2.0f);
                juce::Path dashedBoundary;
                juce::PathStrokeType(1.0f).createDashedStroke(dashedBoundary, boundary, dash, 2);
                g.fillPath(dashedBoundary);

                for (const auto marker : note.sibilantMarkers)
                {
                    g.setColour(Palette::noteLight);
                    const auto markerX = x + static_cast<float>(marker * sourceScale) * pixelsPerSecond;
                    g.drawVerticalLine(static_cast<int>(markerX), bounds.getY(), bounds.getBottom());
                }

                juce::Path originalContour;
                juce::Path contour;
                bool open = false;
                bool originalOpen = false;
                // The consonant sits in the lead-in, before the note start,
                // and it is sung with the head of the pitch curve held: that
                // is what encodePitchbend does when the output time falls
                // ahead of the curve.  Drawing the line only from the note
                // start left that stretch blank and made the pitch look like
                // it jumped at the note boundary.  Start the line where the
                // note actually starts sounding instead.
                auto leadInLocal = 0.0;
                auto tailLocal = note.durationSeconds;
                if (utauMode)
                {
                    if (const auto span = utauSoundSpans.find(note.id.toStdString());
                        span != utauSoundSpans.end())
                    {
                        leadInLocal = std::min(0.0,
                            span->second.first - absoluteStart);
                        // A note stops sounding where the next one starts,
                        // which is earlier than its nominal end whenever the
                        // next note reaches back for its own consonant.
                        tailLocal = juce::jlimit(0.0, note.durationSeconds,
                            span->second.second - absoluteStart);
                    }
                    // Between two adjacent notes the pitch is carried by the S
                    // transition, and the renderer writes that same bridge into
                    // both of them.  Each note therefore hands its line over at
                    // its outermost anchor; drawing on past that point put a
                    // second, differently placed line beside the bridge, which
                    // is what read as a fork after a pitch change.
                    //
                    // Where no neighbour abuts, the line simply starts at the
                    // note.  The consonant reaching back before it is sung at
                    // the held head pitch, but drawing that as a flat run left a
                    // stray horizontal stub hanging off the front of the curve,
                    // so it is not drawn.
                    const PositionedUtauNote here {
                        note.id, absoluteStart, absoluteStart + note.durationSeconds
                    };
                    const auto& handOver = pitchAnchorsFor(note);
                    if (!handOver.empty())
                    {
                        if (const auto previous = previousUtauNoteFor(note.id);
                            previous && formsAdjacentPitchBoundary(*previous, here))
                            leadInLocal = handOver.front().timeSeconds;
                        if (const auto next = nextUtauNoteFor(note.id);
                            next && formsAdjacentPitchBoundary(here, *next))
                            tailLocal = std::min(tailLocal, handOver.back().timeSeconds);
                    }
                }
                const auto foldRealVibratoIntoContour = note.vibratoEnabled
                    && note.vibratoRealLine && tool != Tool::points
                    && note.durationSeconds > 1.0e-9;
                const auto foldedVibrato = foldRealVibratoIntoContour
                    ? effectiveVibrato(note) : note;
                for (const auto& point : note.contour)
                {
                    if (!point.voiced)
                    {
                        open = false;
                        originalOpen = false;
                        continue;
                    }
                    const auto px = x + static_cast<float>(point.timeSeconds * sourceScale) * pixelsPerSecond;
                    const auto sourceCenter = note.sourceMidiCenter >= 0.0f
                        ? note.sourceMidiCenter : note.midiNote;
                    const auto originalPitch = sourceCenter + point.relativeCents / 100.0f;
                    const auto vibratoCents = foldRealVibratoIntoContour
                        ? static_cast<float>(vibratoCentsAt(foldedVibrato,
                            point.timeSeconds)) : 0.0f;
                    const auto pitch = effectiveMidi
                        + (renderedPitchCents(note, point) + vibratoCents) / 100.0f;
                    const auto py = midiToY(pitch) + rowHeight * 0.5f;
                    const auto originalY = midiToY(originalPitch) + rowHeight * 0.5f;
                    if (!originalOpen) originalContour.startNewSubPath(px, originalY);
                    else originalContour.lineTo(px, originalY);
                    originalOpen = true;
                    if (point.timeSeconds < leadInLocal - 1.0e-9) continue;
                    if (point.timeSeconds > tailLocal + 1.0e-9)
                    {
                        if (open)
                        {
                            const auto tailX = x + static_cast<float>(tailLocal
                                * sourceScale) * pixelsPerSecond;
                            contour.lineTo(tailX, py);
                            open = false;
                        }
                        break;
                    }
                    if (!open) contour.startNewSubPath(px, py);
                    else contour.lineTo(px, py);
                    open = true;
                }
                // A note set to the real vibrato line folds the swing into the
                // anchor line above; the separate trace below is then skipped so
                // the swing is not drawn twice.
                const auto realVibratoLine = foldRealVibratoIntoContour;

                // Only while the point tool is in hand, and only as the
                // scaffold the dots sit on.  Not whenever points exist: what
                // the native renderer follows is the contour, which
                // setNotePitchCurve writes the placed curve into -- so outside
                // the tool the contour is the honest line to show.
                const auto pointsOwnTheLine = !utauMode && tool == Tool::points;
                if (!utauMode && !pointsOwnTheLine && showPitchLine)
                {
                    g.setColour(originalColour.withAlpha(0.86f));
                    juce::Path dashed;
                    juce::PathStrokeType(1.35f, juce::PathStrokeType::curved)
                        .createDashedStroke(dashed, originalContour, dash, 2);
                    g.fillPath(dashed);
                    g.setColour(Palette::pitchLine);
                    g.strokePath(contour,
                        juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
                }

                // Vibrato belongs to the note, not to one tool, so it stays
                // visible while notes are being edited rather than only under
                // the point tool.  The amplitude lane is the one exception: it
                // draws its own envelope over the same rows.
                //
                // It is traced against the note's nominal pitch rather than on
                // top of the pitch line it actually swings around.  Riding that
                // line would make the swing unreadable wherever the line moves;
                // against the flat nominal pitch its depth and rate can be read
                // at a glance.  What is rendered adds the two together.
                if (tool != Tool::amplitude
                    && note.vibratoEnabled && note.durationSeconds > 1.0e-9)
                {
                    const auto noteHasFocus = selectedNotes.contains(note.id.toStdString())
                        || note.id == selectedNote;
                    const auto shown = effectiveVibrato(note);
                    const auto baseY = midiToY(note.midiNote) + rowHeight * 0.5f;
                    // With the swing already in the pitch line, a second copy
                    // of it against the nominal pitch is just clutter.  The
                    // handles below are still drawn, so the shape stays
                    // editable in either display mode.
                    const auto drawSeparateTrace = !realVibratoLine;
                    const auto widthPixels = std::max(2.0f,
                        static_cast<float>(note.durationSeconds) * pixelsPerSecond);
                    const auto steps = std::max(8,
                        static_cast<int>(std::ceil(widthPixels / 2.0f)));
                    juce::Path swing;
                    auto open = false;
                    for (int step = 0; step <= steps; ++step)
                    {
                        const auto time = note.durationSeconds * step / steps;
                        const auto cents = vibratoCentsAt(shown, time);
                        const auto swingX = x + static_cast<float>(time) * pixelsPerSecond;
                        const auto swingY = baseY
                            - static_cast<float>(cents / 100.0) * rowHeight;
                        if (!open) { swing.startNewSubPath(swingX, swingY); open = true; }
                        else swing.lineTo(swingX, swingY);
                    }
                    if (drawSeparateTrace)
                    {
                        // The line the swing is centred on, so the depth reads
                        // as a distance rather than an absolute position.
                        g.setColour(juce::Colour(0xff7fd4ff).withAlpha(0.28f));
                        g.drawHorizontalLine(static_cast<int>(baseY), x, x + widthPixels);
                        g.setColour(juce::Colour(0xff7fd4ff)
                            .withAlpha(noteHasFocus ? 0.95f : 0.62f));
                        g.strokePath(swing, juce::PathStrokeType(1.2f));
                    }

                    // Handles on the swing itself, only for the note being
                    // worked on.  Each one drags a single parameter, so the
                    // shape can be dialled in without opening the dialog.
                    if (noteHasFocus)
                    {
                        const std::array<std::pair<VibratoHandle, juce::Colour>, 6> handles {{
                            { VibratoHandle::offset,  juce::Colour(0xffc9a0ff) },
                            { VibratoHandle::length,  juce::Colour(0xffffc857) },
                            { VibratoHandle::fadeIn,  juce::Colour(0xff9ae66e) },
                            { VibratoHandle::fadeOut, juce::Colour(0xff9ae66e) },
                            { VibratoHandle::cycle,   juce::Colour(0xffff8fa3) },
                            { VibratoHandle::depth,   juce::Colour(0xff7fd4ff) }
                        }};
                        for (const auto& handle : handles)
                        {
                            const auto place = vibratoHandlePlace(shown, handle.first);
                            const auto hx = x + static_cast<float>(place.timeSeconds)
                                * pixelsPerSecond + static_cast<float>(place.pixelOffsetX);
                            const auto hy = baseY
                                - static_cast<float>(place.cents / 100.0) * rowHeight;
                            const auto held = dragMode == DragMode::vibrato
                                && note.id == draggedNote
                                && draggedVibratoHandle == handle.first;
                            const auto size = held ? 8.0f : 6.0f;
                            g.setColour(handle.second.withAlpha(held ? 1.0f : 0.85f));
                            g.fillEllipse(hx - size * 0.5f, hy - size * 0.5f, size, size);
                            g.setColour(juce::Colours::black.withAlpha(0.55f));
                            g.drawEllipse(hx - size * 0.5f, hy - size * 0.5f, size, size, 1.0f);
                        }
                    }
                }

                // The S transition between two adjacent notes is part of the
                // pitch line, not a point-tool guide: the renderer writes the
                // same bridge into both notes.  Drawing it only under the point
                // tool left a gap between their lines everywhere else.
                if (utauMode && tool != Tool::amplitude)
                {
                // Anchor handles and the shape menu belong to the point
                // tool; the bridge itself is drawn for every tool, just
                // below, because it is part of the pitch line.
                const PositionedUtauNote positionedNote {
                    note.id, absoluteStart, absoluteStart + note.durationSeconds
                };
                const auto& bridgeAnchors = pitchAnchorsFor(note);
                if (!bridgeAnchors.empty())
                    if (const auto next = nextUtauNoteFor(note.id);
                        next && formsAdjacentPitchBoundary(positionedNote, *next))
                        if (const auto* nextNote = findNote(next->id))
                        {
                            const auto& nextAnchors = draggedNote == nextNote->id
                                    && dragMode == DragMode::pointPitch && !pitchStroke.empty()
                                ? pitchStroke : pitchAnchorsFor(*nextNote);
                            if (!nextAnchors.empty())
                            {
                                const auto bridgeStart = absoluteStart
                                    + bridgeAnchors.back().timeSeconds;
                                const auto bridgeEnd = next->startSeconds
                                    + nextAnchors.front().timeSeconds;
                                const auto steps = std::max(4, static_cast<int>(std::ceil(
                                    std::max(0.001, bridgeEnd - bridgeStart)
                                        * pixelsPerSecond / 3.0)));
                                juce::Path bridge;
                                for (int step = 0; step <= steps; ++step)
                                {
                                    const auto u = static_cast<float>(step) / steps;
                                    const auto shaped = u * u * (3.0f - 2.0f * u);
                                    const auto time = bridgeStart
                                        + (bridgeEnd - bridgeStart) * u;
                                    const auto midi = bridgeAnchors.back().targetMidi
                                        + (nextAnchors.front().targetMidi
                                            - bridgeAnchors.back().targetMidi) * shaped;
                                    const auto point = juce::Point<float>(timeToX(time),
                                        midiToY(midi) + rowHeight * 0.5f);
                                    if (step == 0) bridge.startNewSubPath(point);
                                    else bridge.lineTo(point);
                                }
                                g.setColour(Palette::noteLight.withAlpha(0.88f));
                                g.strokePath(bridge, juce::PathStrokeType(2.0f,
                                    juce::PathStrokeType::curved));
                            }
                        }
                }

                if ((utauMode || pointsOwnTheLine) && showPitchLine
                    && tool != Tool::amplitude)
                {
                    const auto& anchors = draggedNote == note.id
                            && dragMode == DragMode::pointPitch && !pitchStroke.empty()
                        ? pitchStroke : pitchAnchorsFor(note);
                    // Outside the point tool a note set to the real vibrato line
                    // shows the swing folded into this same line, so both views
                    // still come from one place.
                    const auto foldVibrato = note.vibratoEnabled && note.vibratoRealLine
                        && tool != Tool::points && note.durationSeconds > 1.0e-9;
                    const auto shownVibrato = effectiveVibrato(note);
                    // A connected note glides its head out of the previous
                    // note's tail pitch; drawing that here keeps the pitch line
                    // continuous across the seam, matching the render.  UTAU
                    // adjacency draws its own crossfade bridge below instead.
                    const auto joinGlide = dataIsUtau ? std::nullopt
                                                      : incomingJoinGlideFor(note);
                    const auto glideAt = [&](double time, float natural)
                    {
                        if (!joinGlide || joinGlide->joinSeconds <= 1.0e-9
                            || time >= joinGlide->joinSeconds)
                            return natural;
                        const auto u = static_cast<float>(
                            juce::jlimit(0.0, 1.0, time / joinGlide->joinSeconds));
                        const auto shaped = u * u * (3.0f - 2.0f * u);
                        return static_cast<float>(joinGlide->leadMidi)
                            + (natural - static_cast<float>(joinGlide->leadMidi)) * shaped;
                    };
                    juce::Path controlLine;
                    auto controlLineOpen = false;
                    for (std::size_t index = 1; index < anchors.size(); ++index)
                    {
                        const auto startTime = anchors[index - 1].timeSeconds;
                        const auto endTime = anchors[index].timeSeconds;
                        const auto pixelWidth = std::max(1.0f,
                            static_cast<float>(endTime - startTime) * pixelsPerSecond);
                        auto steps = std::max(2, static_cast<int>(std::ceil(pixelWidth / 3.0f)));
                        if (foldVibrato)
                            steps = std::max(steps,
                                static_cast<int>(std::ceil(pixelWidth / 2.0f)));
                        if (joinGlide && startTime < joinGlide->joinSeconds)
                            steps = std::max(steps,
                                static_cast<int>(std::ceil(pixelWidth / 2.0f)));
                        for (int step = 0; step <= steps; ++step)
                        {
                            if (controlLineOpen && step == 0) continue;
                            const auto amount = static_cast<double>(step) / steps;
                            const auto time = startTime + (endTime - startTime) * amount;
                            const auto curveX = x + static_cast<float>(time) * pixelsPerSecond;
                            auto midi = glideAt(time, pitchAt(anchors, time));
                            if (foldVibrato)
                                midi += static_cast<float>(
                                    vibratoCentsAt(shownVibrato, time) / 100.0);
                            const auto curveY = midiToY(midi) + rowHeight * 0.5f;
                            if (!controlLineOpen)
                            {
                                controlLine.startNewSubPath(curveX, curveY);
                                controlLineOpen = true;
                            }
                            else controlLine.lineTo(curveX, curveY);
                        }
                    }
                    const auto noteSelected = selectedNotes.contains(note.id.toStdString())
                        || note.id == selectedNote;
                    g.setColour(Palette::pitchLine.withAlpha(noteSelected ? 1.0f : 0.72f));
                    g.strokePath(controlLine,
                        juce::PathStrokeType(2.0f, juce::PathStrokeType::curved));
                }

                if (tool == Tool::points)
                {
                    const auto& anchors = draggedNote == note.id
                            && dragMode == DragMode::pointPitch && !pitchStroke.empty()
                        ? pitchStroke : pitchAnchorsFor(note);
                    const auto noteSelected = selectedNotes.contains(note.id.toStdString())
                        || note.id == selectedNote;
                    for (std::size_t index = 0; index < anchors.size(); ++index)
                    {
                        const auto anchorX = x + static_cast<float>(anchors[index].timeSeconds)
                            * pixelsPerSecond;
                        const auto anchorY = midiToY(anchors[index].targetMidi)
                            + rowHeight * 0.5f;
                        const auto active = draggedNote == note.id
                            && static_cast<int>(index) == draggedPitchAnchor;
                        const auto radius = index == 0 || index + 1 == anchors.size() ? 4.8f : 4.0f;
                        g.setColour(active ? Palette::noteFill
                                           : Palette::panelRaised.withAlpha(noteSelected ? 1.0f : 0.82f));
                        g.fillEllipse(anchorX - radius, anchorY - radius, radius * 2.0f, radius * 2.0f);
                        g.setColour(Palette::noteLight.withAlpha(noteSelected ? 1.0f : 0.72f));
                        g.drawEllipse(anchorX - radius, anchorY - radius, radius * 2.0f, radius * 2.0f,
                                      active ? 2.0f : 1.4f);
                    }
                }

                if (note.connectedToPrevious)
                {
                    g.setColour(Palette::noteLight);
                    g.fillEllipse(bounds.getX() - 3.0f, bounds.getCentreY() - 3.0f, 6.0f, 6.0f);
                }
            }
        }
        ++trackIndex;
    }

    if (!sourceEditMode && tool == Tool::flagCurve) drawFlagLane(g);
    if (!sourceEditMode && tool == Tool::amplitude)
    {
        const auto lane = amplitudeLaneBounds();
        const auto plot = amplitudeLanePlotBounds();
        g.setColour(Palette::panel.withAlpha(0.97f));
        g.fillRoundedRectangle(lane, 5.0f);
        g.setColour(Palette::border.brighter(0.18f));
        g.drawRoundedRectangle(lane, 5.0f, 1.2f);

        g.setFont(11.0f);
        g.setColour(Palette::text);
        g.drawText(juce::String::fromUTF8("响度包络（UTAU 线性百分比）  纵向 0–")
                + juce::String(static_cast<int>(amplitudeLaneMaxPercent)) + "%",
            lane.toNearestInt().removeFromTop(28).reduced(8, 0).withTrimmedRight(104),
            juce::Justification::centredLeft, false);

        for (const auto zoomIn : { false, true })
        {
            const auto button = amplitudeZoomButtonBounds(zoomIn);
            g.setColour(Palette::panelRaised);
            g.fillRoundedRectangle(button, 3.0f);
            g.setColour(Palette::border.brighter(0.25f));
            g.drawRoundedRectangle(button, 3.0f, 1.0f);
            g.setColour(Palette::text);
            g.setFont(15.0f);
            g.drawText(amplitudeZoomLabel(zoomIn), button.toNearestInt(),
                       juce::Justification::centred, false);
        }

        g.setFont(10.0f);
        for (int guide = 0; guide <= 4; ++guide)
        {
            const auto percent = amplitudeLaneMaxPercent
                * static_cast<float>(guide) / 4.0f;
            const auto guideY = amplitudeLaneY(amplitudeDbFromPercent(percent));
            g.setColour(std::abs(percent - 100.0f) < 0.1f
                    ? juce::Colour(0xff72d6aa).withAlpha(0.36f)
                    : Palette::grid.withAlpha(0.70f));
            g.drawHorizontalLine(static_cast<int>(guideY), plot.getX(), plot.getRight());
            g.setColour(Palette::textMuted);
            const auto label = percent <= 0.05f
                ? juce::String::fromUTF8("0 静音")
                : juce::String(static_cast<int>(std::lround(percent))) + "%";
            g.drawText(label, static_cast<int>(plot.getRight() + 4.0f),
                static_cast<int>(guideY - 8.0f), 49, 16,
                juce::Justification::centredRight, false);
        }

        auto paintedEnvelopeCount = 0;
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(plot.toNearestInt().expanded(7));
        for (const auto& hit : noteHits)
        {
            const auto selected = selectedNotes.contains(hit.id.toStdString())
                || (selectedNotes.empty() && hit.id == selectedNote);
            bool utau = false;
            const auto* envelopeNote = findNote(hit.id, &utau);
            if (envelopeNote == nullptr || !utau) continue;
            const auto absoluteStart = hit.startSeconds + hit.clipStartSeconds;
            auto envelope = amplitudeEnvelopeFor(*envelopeNote, absoluteStart);
            const auto participatesInEdit = hit.id == draggedNote || selected;
            if (dragMode == DragMode::amplitudePoint && amplitudeDragUsesLane
                && !amplitudeStroke.empty() && participatesInEdit)
                envelope = mapAmplitudeEnvelopeToNote(amplitudeStroke, draggedNote, hit.id);
            if (envelope.size() < 2) continue;

            ++paintedEnvelopeCount;
            const auto isMaster = hit.id == draggedNote || hit.id == selectedNote;
            const auto colour = juce::Colour(0xffffa94d).withAlpha(
                isMaster ? 1.0f : selected ? 0.76f : 0.32f);
            juce::Path path;
            for (std::size_t index = 0; index < envelope.size(); ++index)
            {
                const auto point = juce::Point<float>(
                    timeToX(absoluteStart + envelope[index].timeSeconds),
                    amplitudeLaneY(envelope[index].gainDb));
                if (index == 0) path.startNewSubPath(point);
                else path.lineTo(point);
            }
            g.setColour(colour);
            g.strokePath(path, juce::PathStrokeType(
                isMaster ? 2.8f : selected ? 2.1f : 1.35f));
            for (std::size_t index = 0; index < envelope.size(); ++index)
            {
                const auto point = juce::Point<float>(
                    timeToX(absoluteStart + envelope[index].timeSeconds),
                    amplitudeLaneY(envelope[index].gainDb));
                const auto active = envelopeNote->id == draggedNote
                    && amplitudeDragUsesLane
                    && static_cast<int>(index) == draggedAmplitudePoint;
                const auto radius = active ? 6.0f : selected ? 5.0f : 3.8f;
                g.setColour(active ? Palette::noteFill
                                   : Palette::panelRaised.withAlpha(selected ? 1.0f : 0.70f));
                g.fillEllipse(point.x - radius, point.y - radius,
                              radius * 2.0f, radius * 2.0f);
                g.setColour(colour);
                g.drawEllipse(point.x - radius, point.y - radius,
                              radius * 2.0f, radius * 2.0f, active ? 2.4f : 1.8f);
                if (active)
                {
                    const auto percent = amplitudePercentFromDb(envelope[index].gainDb);
                    const auto value = percent <= 0.05f
                        ? juce::String::fromUTF8("响度 0（静音）")
                        : juce::String(percent, 1) + "%  ("
                            + juce::String(envelope[index].gainDb, 1) + " dB)";
                    g.setColour(Palette::text);
                    g.drawText(value, static_cast<int>(point.x + 9.0f),
                        static_cast<int>(point.y - 20.0f), 142, 18,
                        juce::Justification::centredLeft, false);
                }
            }
        }
        if (paintedEnvelopeCount == 0)
        {
            g.setColour(Palette::textMuted);
            g.drawFittedText(juce::String::fromUTF8(
                "当前 UTAU 轨道没有可显示的响度包络"),
                plot.toNearestInt().reduced(12), juce::Justification::centred, 1);
        }
    }

    if (draggedNote.isNotEmpty() && dragMode == DragMode::pitch)
    {
        g.setColour(Palette::noteLight.withAlpha(0.8f));
        g.drawHorizontalLine(static_cast<int>(midiToY(previewMidi) + rowHeight * 0.5f),
                             58.0f, static_cast<float>(getWidth()));
    }

    if (!pitchStroke.empty()
        && (dragMode == DragMode::drawPitch || dragMode == DragMode::linePitch))
    {
        juce::Path preview;
        for (std::size_t index = 0; index < pitchStroke.size(); ++index)
        {
            const auto x = timeToX(pitchEditAbsoluteStart
                                   + pitchStroke[index].timeSeconds);
            const auto y = midiToY(pitchStroke[index].targetMidi) + rowHeight * 0.5f;
            if (index == 0) preview.startNewSubPath(x, y);
            else preview.lineTo(x, y);
        }
        g.setColour(Palette::noteLight.withAlpha(0.96f));
        g.strokePath(preview, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved));
    }

    if (dragMode == DragMode::marquee)
    {
        const auto selection = juce::Rectangle<float>(marqueeStart, marqueeCurrent);
        g.setColour(Palette::accent.withAlpha(0.12f));
        g.fillRect(selection);
        g.setColour(Palette::accentLight.withAlpha(0.92f));
        g.drawRect(selection, 1.0f);
    }

    if (dragMode == DragMode::drawNewNote && drawLengthSeconds > 0.0)
    {
        // Outlined rather than filled, because it is not a note yet: nothing
        // is written until the button comes up.
        const juce::Rectangle<float> pending(
            timeToX(drawStartSeconds), midiToY(drawMidi),
            static_cast<float>(drawLengthSeconds) * pixelsPerSecond, rowHeight);
        g.setColour(Palette::noteFill.withAlpha(0.28f));
        g.fillRoundedRectangle(pending, 3.0f);
        g.setColour(Palette::noteLight.withAlpha(0.95f));
        g.drawRoundedRectangle(pending, 3.0f, 1.6f);
    }

    drawBarNumbers(g);

    g.setColour(juce::Colours::red.withAlpha(0.9f));
    g.drawVerticalLine(static_cast<int>(timeToX(playheadSeconds)), 0.0f, static_cast<float>(getHeight()));
    // Scrolled away from the playhead, there was nothing on screen to say so,
    // and a zoom -- which keeps the playhead centred -- then looked like the
    // view had jumped somewhere at random.  A mark on the edge it lies beyond
    // keeps it present without dragging the view back to it.
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        const auto left = static_cast<float>(viewport->getViewPositionX());
        const auto right = left + static_cast<float>(viewport->getViewWidth());
        const auto top = static_cast<float>(viewport->getViewPositionY());
        const auto playheadX = timeToX(playheadSeconds);
        const auto beyondLeft = playheadX < left + 58.0f;
        if (beyondLeft || playheadX > right)
        {
            const auto edge = beyondLeft ? left + 58.0f : right;
            const auto point = beyondLeft ? edge : edge - 9.0f;
            const auto back = beyondLeft ? edge + 9.0f : edge;
            juce::Path arrow;
            arrow.startNewSubPath(point, top + 21.0f);
            arrow.lineTo(back, top + 16.0f);
            arrow.lineTo(back, top + 26.0f);
            arrow.closeSubPath();
            g.setColour(juce::Colours::red.withAlpha(0.85f));
            g.fillPath(arrow);
        }
    }
    drawKeyboard();
}

void PianoRollComponent::drawBarNumbers(juce::Graphics& g)
{
    // The arrangement ruler is out of sight while tuning, so the roll has to
    // say which bar is under the cursor by itself.  Numbering matches the
    // ruler's: bar 1 is the start of the song.
    const auto barQuarters = static_cast<double>(std::max(1, snapshot.numerator))
        * 4.0 / static_cast<double>(std::max(1, snapshot.denominator));
    if (barQuarters <= 1.0e-6) return;
    // The roll is taller and wider than its viewport, so the marks are pinned
    // to the visible corner the way the keyboard strip is.  Anchored to the
    // component instead they would scroll off with the top octave.
    auto viewX = 0.0f;
    auto viewY = 0.0f;
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        viewX = static_cast<float>(viewport->getViewPositionX());
        viewY = static_cast<float>(viewport->getViewPositionY());
    }
    // Laying out the text is the expensive part, and it was being done for
    // every bar in the piece even though the clip kept only the visible ones.
    const auto visible = g.getClipBounds().toFloat();
    const auto leftEdge = std::max(viewX + 58.0f, visible.getX());
    const auto firstSeconds = std::max(0.0,
        static_cast<double>(leftEdge - 58.0f) / pixelsPerSecond);
    const auto firstBar = std::max(0, static_cast<int>(std::floor(
        snapshot.quarterPositionForSeconds(firstSeconds) / barQuarters)));
    for (auto bar = firstBar;; ++bar)
    {
        const auto x = timeToX(snapshot.secondsForQuarterPosition(bar * barQuarters));
        if (x > visible.getRight() || x > static_cast<float>(getWidth())) break;
        // Behind the keyboard strip the mark would only be half readable.
        if (x < leftEdge) continue;
        const auto label = juce::String(bar + 1);
        const auto chip = juce::Rectangle<float>(
            x + 2.0f, viewY + 2.0f,
            9.0f + 6.0f * static_cast<float>(label.length()), 14.0f);
        // Over the notes rather than under them: behind a note the number
        // would disappear exactly where the editing is happening.  The chip
        // keeps it legible without hiding much of the note.
        g.setColour(Palette::background.withAlpha(0.72f));
        g.fillRoundedRectangle(chip, 3.0f);
        g.setColour(Palette::textMuted);
        g.setFont(10.0f);
        g.drawText(label, chip, juce::Justification::centred);
    }
}

std::optional<double> PianoRollComponent::pasteAnchorSeconds() const
{
    if (!hoverSeconds) return std::nullopt;
    return snapToGrid(*hoverSeconds);
}

void PianoRollComponent::mouseMove(const juce::MouseEvent& event)
{
    // Before the early returns below: a paste wants to know where the pointer
    // is whatever tool is in hand.
    hoverSeconds = std::max(0.0,
        static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond);
    if (sourceEditMode || tool != Tool::note || event.mods.isAnyMouseButtonDown()) return;
    if (const auto vibratoHandle = vibratoHandleAt(event.position))
    {
        setMouseCursor(vibratoHandle->which == VibratoHandle::depth
                || vibratoHandle->which == VibratoHandle::offset
            ? juce::MouseCursor::UpDownResizeCursor
            : juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    if (jieSplitHandleAt(event.position) || consonantHandleAt(event.position))
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    auto maximumDuration = 0.0;
    setMouseCursor(resizableTailAt(event.position, maximumDuration) != nullptr
        ? juce::MouseCursor::LeftRightResizeCursor
        : juce::MouseCursor::NormalCursor);
}

void PianoRollComponent::mouseExit(const juce::MouseEvent&)
{
    // Off the roll there is no pointer to paste at, and the last place it was
    // is not where anyone means.
    hoverSeconds.reset();
    if (!sourceEditMode && tool == Tool::note && dragMode == DragMode::none)
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

void PianoRollComponent::mouseDown(const juce::MouseEvent& event)
{
    hoverSeconds = std::max(0.0,
        static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond);
    noteDragPassedThreshold = false;
    grabKeyboardFocus();
    if (const auto* viewport = findParentComponentOfClass<juce::Viewport>())
        if (event.x < viewport->getViewPositionX() + 58)
            return;
    if (sourceEditMode)
    {
        const RegionHandleHit* closest = nullptr;
        auto closestDistance = 7.0f;
        // Reverse order gives the active/later region priority at shared edges.
        for (auto it = regionHandleHits.rbegin(); it != regionHandleHits.rend(); ++it)
        {
            const auto distance = std::abs(event.position.x - it->x);
            if (distance <= closestDistance)
            {
                closest = &*it;
                closestDistance = distance;
            }
        }
        if (closest != nullptr)
        {
            draggedSampleRegion = closest->region;
            draggedRegionHandle = closest->handle;
            activeSampleRegion = closest->region;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            if (onSampleRegionEdited)
                onSampleRegionEdited(activeSampleRegion,
                    sampleRegions[static_cast<std::size_t>(activeSampleRegion)], false);
            repaint();
            return;
        }
    }
    // Hit regions must reflect the current model even when the first click
    // arrives before a pending repaint (common immediately after MIDI import,
    // track changes, zooming, or scrolling).
    rebuildNoteHits();
    // Wherever an edit begins is where the focus now is, so the playhead
    // follows it there.  That keeps the red line worth looking at, and gives a
    // zoom something real to hold still instead of a marker left at the start
    // of the piece.  Not for a right click, which asks a question rather than
    // editing, and not for the lane's own zoom buttons.
    const auto onLaneZoomButton = !sourceEditMode
        && ((tool == Tool::amplitude
             && (amplitudeZoomButtonBounds(true).contains(event.position)
                 || amplitudeZoomButtonBounds(false).contains(event.position)))
            || (tool == Tool::flagCurve
                && (flagLaneZoomButtonBounds(true).contains(event.position)
                    || flagLaneZoomButtonBounds(false).contains(event.position))));
    playheadBeforeGesture = playheadSeconds;
    if (onEditPosition != nullptr && !event.mods.isPopupMenu() && !onLaneZoomButton)
        onEditPosition(std::max(0.0,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond));
    if (!sourceEditMode && tool == Tool::amplitude)
    {
        const auto lane = amplitudeLaneBounds();
        if (lane.contains(event.position))
        {
            if (amplitudeZoomButtonBounds(true).contains(event.position)
                || amplitudeZoomButtonBounds(false).contains(event.position))
            {
                constexpr std::array<float, 5> zoomLevels { 100.0f, 150.0f, 200.0f,
                                                            300.0f, 400.0f };
                auto level = 0;
                auto distance = std::numeric_limits<float>::max();
                for (int index = 0; index < static_cast<int>(zoomLevels.size()); ++index)
                    if (const auto next = std::abs(amplitudeLaneMaxPercent
                                                   - zoomLevels[static_cast<std::size_t>(index)]);
                        next < distance)
                    {
                        distance = next;
                        level = index;
                    }
                level += amplitudeZoomButtonBounds(true).contains(event.position) ? -1 : 1;
                amplitudeLaneMaxPercent = zoomLevels[static_cast<std::size_t>(
                    juce::jlimit(0, static_cast<int>(zoomLevels.size()) - 1, level))];
                repaint();
                return;
            }

            const NoteData* note = nullptr;
            const NoteHit* hit = nullptr;
            std::vector<AmplitudeEnvelopePoint> envelope;
            auto closestPoint = -1;
            auto closestDistance = 11.0f;
            for (const auto& candidateHit : noteHits)
            {
                bool utau = false;
                const auto* candidateNote = findNote(candidateHit.id, &utau);
                if (candidateNote == nullptr || !utau) continue;
                const auto candidateStart = candidateHit.startSeconds
                    + candidateHit.clipStartSeconds;
                const auto candidateEnvelope = amplitudeEnvelopeFor(
                    *candidateNote, candidateStart);
                for (std::size_t index = 0; index < candidateEnvelope.size(); ++index)
                {
                    const auto point = juce::Point<float>(
                        timeToX(candidateStart + candidateEnvelope[index].timeSeconds),
                        amplitudeLaneY(candidateEnvelope[index].gainDb));
                    const auto pointDistance = point.getDistanceFrom(event.position);
                    if (pointDistance <= closestDistance)
                    {
                        closestDistance = pointDistance;
                        closestPoint = static_cast<int>(index);
                        note = candidateNote;
                        hit = &candidateHit;
                        envelope = candidateEnvelope;
                    }
                }
            }
            if (closestPoint < 0 || note == nullptr || hit == nullptr) return;
            const auto absoluteStart = hit->startSeconds + hit->clipStartSeconds;
            const auto wasSelected = selectedNotes.contains(note->id.toStdString())
                || (selectedNotes.empty() && selectedNote == note->id);
            if (!wasSelected)
            {
                selectedNotes.clear();
                selectedNotes.insert(note->id.toStdString());
            }
            selectedNote = note->id;
            if (onNoteSelected) onNoteSelected(selectedNote);
            // Right-click removes an interior automation point.  The two
            // endpoints are retained so the envelope remains well-defined.
            if (event.mods.isPopupMenu())
            {
                if (envelope.size() > 2 && closestPoint > 0
                    && closestPoint + 1 < static_cast<int>(envelope.size()))
                {
                    envelope.erase(envelope.begin() + closestPoint);
                    commitAmplitudeEnvelopeToSelection(note->id, envelope);
                }
                repaint();
                return;
            }

            draggedNote = note->id;
            draggedAmplitudePoint = closestPoint;
            amplitudeStroke = std::move(envelope);
            amplitudeEditAbsoluteStart = absoluteStart;
            amplitudeEditMidi = note->midiNote;
            amplitudeDragUsesLane = true;
            amplitudePointMinimumTime = amplitudeStroke.front().timeSeconds;
            amplitudePointMaximumTime = amplitudeStroke.back().timeSeconds;
            if (closestPoint > 0)
                amplitudePointMinimumTime = amplitudeStroke[
                    static_cast<std::size_t>(closestPoint - 1)].timeSeconds + 0.005;
            if (closestPoint + 1 < static_cast<int>(amplitudeStroke.size()))
                amplitudePointMaximumTime = amplitudeStroke[
                    static_cast<std::size_t>(closestPoint + 1)].timeSeconds - 0.005;
            if (closestPoint == 0)
            {
                amplitudePointMinimumTime = std::min(0.0,
                    amplitudeStroke.front().timeSeconds);
                if (const auto span = utauSoundSpans.find(note->id.toStdString());
                    span != utauSoundSpans.end())
                    amplitudePointMinimumTime = std::min(0.0,
                        span->second.first - absoluteStart);
            }
            if (closestPoint + 1 == static_cast<int>(amplitudeStroke.size()))
                amplitudePointMaximumTime = note->durationSeconds;
            dragMode = DragMode::amplitudePoint;
            repaint();
            return;
        }
    }
    if (!sourceEditMode && tool == Tool::flagCurve)
    {
        const auto plot = flagLanePlotBounds();
        if (flagLaneSwitchBounds().contains(event.position))
        {
            if (event.mods.isPopupMenu())
                showFlagLaneSwitchContextMenu(event.getScreenPosition());
            else
                showFlagLaneSwitchMenu();
            return;
        }
        for (const auto zoomIn : { true, false })
            if (flagLaneZoomButtonBounds(zoomIn).contains(event.position))
            {
                nudgeFlagLaneZoom(zoomIn);
                return;
            }
        if (flagLaneBounds().contains(event.position))
        {
            const NoteData* note = nullptr;
            const NoteHit* hit = nullptr;
            auto closestPoint = -1;
            auto closestDistance = 11.0f;
            for (const auto& candidateHit : noteHits)
            {
                bool utau = false;
                const auto* candidate = findNote(candidateHit.id, &utau);
                if (candidate == nullptr || !utau || !candidate->utauFlagCurveEnabled)
                    continue;
                const auto start = candidateHit.startSeconds + candidateHit.clipStartSeconds;
                const auto candidatePoints = flagLaneCurveFor(*candidate);
                // Clamped to the drawn stretch, exactly as the paint clamps it.
                // Testing the unclamped time looked for the handle where none
                // was drawn: a note in a phrase stops sounding well before its
                // written end, so its last handle -- drawn at that edge -- was
                // hunted for far to the right of itself and could not be picked
                // up at all.
                const auto [drawFrom, drawTo] =
                    flagLaneDrawnSpan(*candidate, start);
                const auto handleLeft = juce::jlimit(plot.getX(), plot.getRight(),
                                                     timeToX(drawFrom));
                const auto handleRight = juce::jlimit(plot.getX(), plot.getRight(),
                                                      timeToX(drawTo));
                for (std::size_t index = 0; index < candidatePoints.size(); ++index)
                {
                    const auto centre = juce::Point<float>(
                        juce::jlimit(handleLeft, handleRight,
                            timeToX(start + candidatePoints[index].timeSeconds)),
                        flagLaneY(candidatePoints[index].value));
                    if (const auto distance = centre.getDistanceFrom(event.position);
                        distance <= closestDistance)
                    {
                        closestDistance = distance;
                        closestPoint = static_cast<int>(index);
                        note = candidate;
                        hit = &candidateHit;
                    }
                }
            }
            if (note != nullptr && hit != nullptr)
            {
                if (event.mods.isPopupMenu())
                {
                    showFlagPointMenu(note->id, closestPoint,
                                      event.getScreenPosition());
                    return;
                }
                draggedNote = note->id;
                draggedFlagPoint = closestPoint;
                flagStroke = flagLaneCurveFor(*note);
                flagEditAbsoluteStart = hit->startSeconds + hit->clipStartSeconds;
                const auto [spanFrom, spanTo] = flagLaneSpanFor(*note);
                flagPointMinimumTime = closestPoint > 0
                    ? flagStroke[static_cast<std::size_t>(closestPoint) - 1].timeSeconds + 0.005
                    : spanFrom;
                flagPointMaximumTime = closestPoint + 1 < static_cast<int>(flagStroke.size())
                    ? flagStroke[static_cast<std::size_t>(closestPoint) + 1].timeSeconds - 0.005
                    : spanTo;
                // On its own a point is the note's flag level, not a corner of
                // a shape: it reads as that value everywhere, so moving it
                // along the note would change nothing while walking it off the
                // end it sits on.  It moves up and down only.
                if (flagStroke.size() == 1)
                    flagPointMinimumTime = flagPointMaximumTime =
                        flagStroke.front().timeSeconds;
                // b and bh do nothing past the onset, so their handles do not
                // go there; the rest of the flags have the whole note.
                flagPointMinimumTime = std::max(flagPointMinimumTime, spanFrom);
                flagPointMaximumTime = std::min(flagPointMaximumTime, spanTo);
                dragMode = DragMode::flagPoint;
                repaint();
                return;
            }
            // Nothing under the cursor: put a handle on whichever curve-bearing
            // note this column belongs to.  Notes overlap wherever a lead-in
            // reaches back into the note before it, so a column often belongs
            // to two of them -- taking the first left the note behind always
            // winning, and the one in front could not be edited there at all.
            // The one whose curve passes nearest the click is the one that was
            // being aimed at, which is also the one drawn under the cursor.
            if (!event.mods.isPopupMenu() && plot.contains(event.position))
            {
                const auto seconds = static_cast<double>(event.position.x - 58.0f)
                    / pixelsPerSecond;
                const NoteData* chosen = nullptr;
                auto chosenStart = 0.0;
                auto chosenDistance = std::numeric_limits<float>::max();
                for (const auto& candidateHit : noteHits)
                {
                    bool utau = false;
                    const auto* candidate = findNote(candidateHit.id, &utau);
                    if (candidate == nullptr || !utau || !candidate->utauFlagCurveEnabled)
                        continue;
                    const auto start = candidateHit.startSeconds + candidateHit.clipStartSeconds;
                    const auto [spanFrom, spanTo] = flagLaneSpanFor(*candidate);
                    if (seconds < start + spanFrom - 0.001
                        || seconds > start + spanTo + 0.001)
                        continue;
                    const auto points = flagLaneCurveFor(*candidate);
                    if (points.empty()) continue;
                    // Read where the curve is drawn at that column: held flat
                    // past either end, exactly as the paint holds it.
                    const auto at = juce::jlimit(spanFrom, spanTo, seconds - start);
                    const auto y = flagLaneY(flagCurveValueAt(points, at));
                    if (const auto distance = std::abs(y - event.position.y);
                        distance < chosenDistance)
                    {
                        chosenDistance = distance;
                        chosen = candidate;
                        chosenStart = start;
                    }
                }
                if (chosen != nullptr)
                {
                    auto points = flagLaneCurveFor(*chosen);
                    points.push_back({ seconds - chosenStart,
                                       flagValueFromLaneY(event.position.y) });
                    model.setNoteUtauFlagCurve(chosen->id, laneFlag, std::move(points));
                    return;
                }
            }
            return;
        }
    }
    if (!sourceEditMode && tool == Tool::points)
    {
        juce::String closestNoteId;
        auto closestAnchor = -1;
        auto closestDistance = 9.0f;
        double closestAbsoluteStart = 0.0;
        // Every visible note exposes its anchors in point mode -- the native
        // renderer reads the points too, so they mean the same thing there.
        // Tested before note rectangles so endpoints outside the hollow UTAU
        // block remain directly draggable.
        for (const auto& hit : noteHits)
        {
            if (const auto* note = findNote(hit.id); note != nullptr)
            {
                auto& anchors = pitchAnchorsFor(*note);
                for (std::size_t index = 0; index < anchors.size(); ++index)
                {
                    const auto anchor = juce::Point<float>(
                        timeToX(hit.startSeconds + hit.clipStartSeconds
                                + anchors[index].timeSeconds),
                        midiToY(anchors[index].targetMidi) + rowHeight * 0.5f);
                    const auto distance = anchor.getDistanceFrom(event.position);
                    if (distance <= closestDistance)
                    {
                        closestDistance = distance;
                        closestNoteId = note->id;
                        closestAnchor = static_cast<int>(index);
                        closestAbsoluteStart = hit.startSeconds + hit.clipStartSeconds;
                    }
                }
            }
        }
        if (closestNoteId.isNotEmpty())
        {
            if (const auto* note = findNote(closestNoteId))
            {
                selectedNote = closestNoteId;
                selectedNotes.clear();
                selectedNotes.insert(closestNoteId.toStdString());
                if (onNoteSelected) onNoteSelected(closestNoteId);
                if (event.mods.isPopupMenu())
                {
                    draggedNote.clear();
                    draggedPitchAnchor = -1;
                    pitchStroke.clear();
                    dragMode = DragMode::none;
                    repaint();
                    showPitchCurveShapeMenu(closestNoteId, closestAnchor,
                                            event.getScreenPosition());
                    return;
                }
                draggedNote = closestNoteId;
                draggedPitchAnchor = closestAnchor;
                pitchEditAbsoluteStart = closestAbsoluteStart;
                pitchStroke = pitchAnchorsFor(*note);
                // Every anchor can move horizontally, the endpoints included.
                // What differs is how far: an endpoint is bounded by its one
                // neighbour on the inside and by the neighbouring note on the
                // outside, where an interior anchor is bounded on both sides.
                pointDragCanMoveHorizontally = pitchStroke.size() > 1;
                pointDragMinimumTime = 0.0;
                pointDragMaximumTime = note->durationSeconds;

                const PositionedUtauNote positionedNote {
                    note->id, closestAbsoluteStart,
                    closestAbsoluteStart + note->durationSeconds
                };
                if (closestAnchor == 0)
                {
                    auto minimumAbsolute = positionedNote.startSeconds;
                    if (const auto span = utauSoundSpans.find(note->id.toStdString());
                        span != utauSoundSpans.end())
                        minimumAbsolute = std::min(minimumAbsolute, span->second.first);
                    // A head reaches back as far as the last point of the note
                    // before it.  That is the whole constraint: the two must not
                    // cross.  Stopping it at this note's own start instead made
                    // the transition between abutting notes unshapeable, since
                    // the head could then only ever move later.
                    if (const auto previous = previousUtauNoteFor(note->id))
                    {
                        auto previousTail = previous->startSeconds;
                        if (const auto* previousNote = findNote(previous->id))
                        {
                            const auto& previousAnchors = pitchAnchorsFor(*previousNote);
                            if (!previousAnchors.empty())
                                previousTail = previous->startSeconds
                                    + previousAnchors.back().timeSeconds;
                        }
                        minimumAbsolute = previousTail + minimumAnchorSeparation;
                    }
                    pointDragMinimumTime = minimumAbsolute - positionedNote.startSeconds;
                }
                if (closestAnchor + 1 == static_cast<int>(pitchStroke.size())
                    && closestAnchor > 0)
                {
                    // The mirror of the head: a tail runs forward as far as the
                    // first point of the note after it, through a gap or across
                    // the boundary alike.  Both ends are bounded by each other
                    // and by nothing else, so the pair can be brought as close
                    // together as the user wants without ever crossing.
                    auto maximumAbsolute = positionedNote.endSeconds;
                    if (const auto next = nextUtauNoteFor(note->id))
                    {
                        auto nextHead = next->startSeconds;
                        if (const auto* nextNote = findNote(next->id))
                        {
                            const auto& nextAnchors = pitchAnchorsFor(*nextNote);
                            if (!nextAnchors.empty())
                                nextHead = next->startSeconds
                                    + nextAnchors.front().timeSeconds;
                        }
                        maximumAbsolute = nextHead - minimumAnchorSeparation;
                    }
                    pointDragMaximumTime = maximumAbsolute - positionedNote.startSeconds;
                }
                dragMode = DragMode::pointPitch;
                repaint();
                return;
            }
        }
    }
    if (!sourceEditMode && tool == Tool::note && event.mods.isLeftButtonDown())
    {
        if (const auto vibratoHandle = vibratoHandleAt(event.position))
        {
            selectedNote = vibratoHandle->noteId;
            const auto id = selectedNote.toStdString();
            if (!selectedNotes.contains(id))
            {
                selectedNotes.clear();
                selectedNotes.insert(id);
            }
            if (onNoteSelected) onNoteSelected(selectedNote);
            draggedNote = vibratoHandle->noteId;
            draggedVibratoHandle = vibratoHandle->which;
            vibratoDragAbsoluteStart = vibratoHandle->absoluteStart;
            if (const auto* note = findNote(vibratoHandle->noteId))
                previewVibrato = *note;
            dragMode = DragMode::vibrato;
            setMouseCursor(draggedVibratoHandle == VibratoHandle::depth
                    || draggedVibratoHandle == VibratoHandle::offset
                ? juce::MouseCursor::UpDownResizeCursor
                : juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
        if (const auto jieHandle = jieSplitHandleAt(event.position))
        {
            selectedNote = jieHandle->noteId;
            const auto id = selectedNote.toStdString();
            if (!selectedNotes.contains(id))
            {
                selectedNotes.clear();
                selectedNotes.insert(id);
            }
            if (onNoteSelected) onNoteSelected(selectedNote);
            draggedNote = jieHandle->noteId;
            draggedJieBoundary = jieHandle->boundary;
            jieSpanStartSeconds = jieHandle->spanStart;
            jieSpanEndSeconds = jieHandle->spanEnd;
            previewJieFractions = jieHandle->fractions;
            previewJieRegions = jieHandle->regions;
            dragMode = DragMode::jieSplit;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
        if (const auto consonant = consonantHandleAt(event.position))
        {
            selectedNote = consonant->noteId;
            const auto id = selectedNote.toStdString();
            if (!selectedNotes.contains(id))
            {
                selectedNotes.clear();
                selectedNotes.insert(id);
            }
            if (onNoteSelected) onNoteSelected(selectedNote);
            draggedNote = consonant->noteId;
            consonantNoteAbsoluteStart = consonant->absoluteStart;
            consonantScaledPreutterance = consonant->scaledPreutterance;
            consonantUnscaledPreutterance = consonant->unscaledPreutterance;
            consonantMinimumPreutterance = consonant->minimumPreutterance;
            consonantMaximumPreutterance = consonant->maximumPreutterance;
            previewConsonantPreutterance = consonant->currentPreutterance;
            previewConsonantVelocity = consonant->currentVelocity;
            consonantSetsPin = consonant->setsPin;
            consonantOverlapSeconds = consonant->overlapSeconds;
            dragMode = DragMode::consonantLeadIn;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
        auto maximumDuration = 0.0;
        if (const auto* tail = resizableTailAt(event.position, maximumDuration))
        {
            selectedNote = tail->id;
            const auto id = selectedNote.toStdString();
            if (!selectedNotes.contains(id))
            {
                selectedNotes.clear();
                selectedNotes.insert(id);
            }
            if (onNoteSelected) onNoteSelected(selectedNote);
            draggedNote = tail->id;
            dragStartMidi = tail->midi;
            previewMidi = dragStartMidi;
            dragStartY = event.position.y;
            dragStartSeconds = tail->startSeconds;
            dragDurationSeconds = tail->durationSeconds;
            dragClipStartSeconds = tail->clipStartSeconds;
            previewStartSeconds = dragStartSeconds;
            previewDurationSeconds = dragDurationSeconds;
            resizeMaximumDurationSeconds = maximumDuration;
            dragMode = DragMode::resizeRight;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            repaint();
            return;
        }
    }
    if (!sourceEditMode && tool == Tool::draw && !event.mods.isPopupMenu()
        && drawingTrackIsUtau())
    {
        // Drawing makes notes here, not pitch curves, so a note under the
        // pointer is not a reason to stop: clicking one is how the next note
        // is put after it.
        beginDrawingNote(event);
        return;
    }
    for (auto it = noteHits.rbegin(); it != noteHits.rend(); ++it)
    {
        // UTAU noteHits deliberately use the real sounding span, which can
        // extend before the nominal note and end early.  The note popup must
        // instead be restricted to the standard grid rectangle shown by the
        // orange duration line.
        const auto nominalBounds = juce::Rectangle<float>(
            timeToX(it->startSeconds + it->clipStartSeconds),
            midiToY(it->midi) + 2.0f,
            std::max(5.0f, static_cast<float>(it->durationSeconds) * pixelsPerSecond),
            std::max(6.0f, rowHeight - 4.0f));
        if (!sourceEditMode && event.mods.isPopupMenu()
            && nominalBounds.contains(event.position))
        {
            // Pitch-anchor popup handling above has priority in point mode.
            selectedNote = it->id;
            const auto id = selectedNote.toStdString();
            if (!selectedNotes.contains(id))
            {
                selectedNotes.clear();
                selectedNotes.insert(id);
            }
            draggedNote.clear();
            draggedPitchAnchor = -1;
            draggedAmplitudePoint = -1;
            pitchStroke.clear();
            amplitudeStroke.clear();
            dragMode = DragMode::none;
            if (onNoteSelected) onNoteSelected(selectedNote);
            repaint();
            showNoteContextMenu(selectedNote, event.getScreenPosition());
            return;
        }
        if (it->bounds.contains(event.position))
        {
            selectedNote = it->id;
            const auto id = selectedNote.toStdString();
            if (event.mods.isCommandDown())
            {
                if (selectedNotes.contains(id)) selectedNotes.erase(id);
                else selectedNotes.insert(id);
                if (!selectedNotes.contains(id))
                    selectedNote = selectedNotes.empty() ? juce::String()
                        : juce::String::fromUTF8(selectedNotes.begin()->c_str());
            }
            else if (event.mods.isShiftDown())
            {
                selectedNotes.insert(id);
            }
            else
            {
                if (!selectedNotes.contains(id))
                {
                    selectedNotes.clear();
                    selectedNotes.insert(id);
                }
            }
            if (onNoteSelected) onNoteSelected(selectedNote);
            if (selectedNote.isEmpty()) { repaint(); return; }
            if (tool == Tool::connect && !sourceEditMode)
            {
                model.toggleNoteConnection(selectedNote);
                return;
            }
            if (!sourceEditMode && tool == Tool::points)
            {
                bool utau = false;
                if (const auto* note = findNote(it->id, &utau); note != nullptr && utau)
                    (void) pitchAnchorsFor(*note);
                draggedNote.clear();
                draggedPitchAnchor = -1;
                dragMode = DragMode::none;
                repaint();
                return;
            }
            if (!sourceEditMode && tool == Tool::amplitude)
            {
                draggedNote.clear();
                draggedAmplitudePoint = -1;
                amplitudeStroke.clear();
                dragMode = DragMode::none;
                repaint();
                return;
            }
            if (!sourceEditMode && tool == Tool::flagCurve)
            {
                // Selecting a note while the flag lane is open is ordinary --
                // that is how a curve is put on one -- but dragging it around
                // is not what the click was for.
                draggedNote.clear();
                draggedFlagPoint = -1;
                flagStroke.clear();
                dragMode = DragMode::none;
                repaint();
                return;
            }
            draggedNote = it->id;
            if (!sourceEditMode && (tool == Tool::draw || tool == Tool::line))
            {
                dragMode = tool == Tool::draw ? DragMode::drawPitch : DragMode::linePitch;
                pitchEditAbsoluteStart = static_cast<double>(it->bounds.getX() - 58.0f)
                    / pixelsPerSecond;
                const auto local = juce::jlimit(0.0, it->durationSeconds,
                    static_cast<double>(event.position.x - it->bounds.getX()) / pixelsPerSecond);
                pitchStroke = { { local,
                    juce::jlimit(0.0f, 127.0f, pitchMidiFromY(event.position.y)) } };
                repaint();
                return;
            }
            dragStartMidi = it->midi;
            previewMidi = dragStartMidi;
            dragStartY = event.position.y;
            finePitchDrag = event.mods.isAltDown();
            dragStartSeconds = it->startSeconds;
            dragDurationSeconds = it->durationSeconds;
            dragClipStartSeconds = it->clipStartSeconds;
            previewStartSeconds = dragStartSeconds;
            previewDurationSeconds = dragDurationSeconds;
            previewMoveDeltaSeconds = 0.0;
            minimumMoveDeltaSeconds = -dragStartSeconds;
            auto draggedNoteIsUtau = false;
            (void) findNote(it->id, &draggedNoteIsUtau);
            if (draggedNoteIsUtau && !sourceEditMode)
            {
                // A phrase drag is confined to the clip containing the note
                // under the pointer.  This also gives an exact left boundary
                // for multi-note moves.
                for (const auto& track : snapshot.tracks)
                    for (const auto& clip : track.clips)
                        if (std::any_of(clip.notes.begin(), clip.notes.end(),
                            [&](const auto& note) { return note.id == draggedNote; }))
                        {
                            std::unordered_set<std::string> clipSelection;
                            auto earliest = dragStartSeconds;
                            for (const auto& note : clip.notes)
                                if (selectedNotes.contains(note.id.toStdString()))
                                {
                                    clipSelection.insert(note.id.toStdString());
                                    earliest = std::min(earliest, note.startSeconds);
                                }
                            selectedNotes = std::move(clipSelection);
                            minimumMoveDeltaSeconds = -earliest;
                        }
            }
            if (!sourceEditMode && event.position.x <= it->bounds.getX() + 6.0f)
                dragMode = DragMode::resizeLeft;
            else if (draggedNoteIsUtau && !sourceEditMode)
                dragMode = DragMode::moveUtauNote;
            else
                dragMode = DragMode::pitch;
            repaint();
            return;
        }
    }
    // Nothing under the pointer, so this may be the silence between two
    // notes.  Before the marquee, which is what an empty right-click meant.
    if (event.mods.isPopupMenu() && !sourceEditMode)
        if (const auto gap = gapAt(event.position))
        {
            showGapContextMenu(*gap, event.getScreenPosition());
            return;
        }
    if ((tool == Tool::draw || tool == Tool::line) && !sourceEditMode)
    {
        const auto rawSeconds = std::max(0.0,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond);
        const auto cellStart = snapDownToGrid(rawSeconds);
        const auto clipId = clipForDrawingAt(cellStart);
        if (clipId.isEmpty()) return;
        // The model decides whether there is room and how long the note may
        // be; asking it is the only way to be sure the notes weighed are the
        // current ones, since this view's copy refreshes on a message.
        const auto id = model.addNote(clipId, cellStart, gridSecondsAt(cellStart),
            juce::jlimit(0.0f, 127.0f, std::round(yToMidi(event.position.y))));
        if (id.isNotEmpty())
        {
            selectedNote = id;
            selectedNotes.clear();
            selectedNotes.insert(id.toStdString());
            if (onNoteSelected) onNoteSelected(id);
        }
        return;
    }
    marqueeAddsToSelection = event.mods.isShiftDown() || event.mods.isCommandDown();
    if (!marqueeAddsToSelection)
    {
        selectedNote.clear();
        selectedNotes.clear();
        if (onNoteSelected) onNoteSelected({});
    }
    if (!sourceEditMode && (tool == Tool::note || tool == Tool::points
                            || tool == Tool::amplitude || tool == Tool::flagCurve))
    {
        marqueeStart = event.position;
        marqueeCurrent = event.position;
        dragMode = DragMode::marquee;
    }
    repaint();
    if (onSeek)
        onSeek(std::max(0.0, static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond));
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (sourceEditMode || tool == Tool::draw || tool == Tool::line) return;
    rebuildNoteHits();
    if (tool == Tool::amplitude)
    {
        if (dragMode == DragMode::marquee) dragMode = DragMode::none;
        if (!amplitudeLaneBounds().contains(event.position)) return;
        const NoteData* note = nullptr;
        const NoteHit* hit = nullptr;
        std::vector<AmplitudeEnvelopePoint> envelope;
        auto local = 0.0;
        auto closestLine = 14.0f;
        for (const auto& candidateHit : noteHits)
        {
            bool utau = false;
            const auto* candidateNote = findNote(candidateHit.id, &utau);
            if (candidateNote == nullptr || !utau) continue;
            const auto candidateStart = candidateHit.startSeconds
                + candidateHit.clipStartSeconds;
            auto candidateEnvelope = amplitudeEnvelopeFor(*candidateNote, candidateStart);
            const auto candidateLocal = static_cast<double>(event.position.x
                - timeToX(candidateStart)) / pixelsPerSecond;
            if (candidateEnvelope.empty()
                || candidateLocal < candidateEnvelope.front().timeSeconds
                || candidateLocal > candidateEnvelope.back().timeSeconds)
                continue;
            const auto distance = std::abs(event.position.y
                - amplitudeLaneY(amplitudeDbAt(candidateEnvelope, candidateLocal)));
            if (distance <= closestLine)
            {
                closestLine = distance;
                note = candidateNote;
                hit = &candidateHit;
                local = candidateLocal;
                envelope = std::move(candidateEnvelope);
            }
        }
        if (note == nullptr || hit == nullptr) return;
        for (const auto& point : envelope)
            if (std::abs(point.timeSeconds - local) * pixelsPerSecond < 9.0f)
                return;
        envelope.push_back({ local, amplitudeDbAt(envelope, local) });
        std::stable_sort(envelope.begin(), envelope.end(),
            [](const auto& left, const auto& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
        const auto wasSelected = selectedNotes.contains(note->id.toStdString())
            || (selectedNotes.empty() && selectedNote == note->id);
        if (!wasSelected)
        {
            selectedNotes.clear();
            selectedNotes.insert(note->id.toStdString());
        }
        selectedNote = note->id;
        if (onNoteSelected) onNoteSelected(selectedNote);
        commitAmplitudeEnvelopeToSelection(note->id, envelope);
        repaint();
        return;
    }
    if (tool == Tool::points)
    {
        // A double-click in point mode begins with the same mouse-down used by
        // marquee selection.  Cancel that pending marquee before inserting a
        // control point so its mouse-up cannot replace the selected note.
        if (dragMode == DragMode::marquee)
            dragMode = DragMode::none;
        const NoteHit* closestHit = nullptr;
        const NoteData* closestNote = nullptr;
        auto closestLocal = 0.0;
        auto closestDistance = std::numeric_limits<float>::max();
        // Double-clicking the visible pitch line chooses the nearest UTAU
        // curve directly; no prior note selection is required.
        for (const auto& hit : noteHits)
        {
            bool utau = false;
            const auto* note = findNote(hit.id, &utau);
            if (note == nullptr || !utau) continue;
            const auto absoluteStart = hit.startSeconds + hit.clipStartSeconds;
            const auto local = static_cast<double>(event.position.x - timeToX(absoluteStart))
                / pixelsPerSecond;
            if (local < 0.0 || local > note->durationSeconds) continue;
            auto& anchors = pitchAnchorsFor(*note);
            const auto lineY = midiToY(pitchAt(anchors, local)) + rowHeight * 0.5f;
            const auto distance = std::abs(event.position.y - lineY);
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closestHit = &hit;
                closestNote = note;
                closestLocal = local;
            }
        }
        if (closestHit != nullptr && closestNote != nullptr)
        {
            auto& anchors = pitchAnchorsFor(*closestNote);
            for (const auto& anchor : anchors)
                if (std::abs(anchor.timeSeconds - closestLocal) * pixelsPerSecond < 9.0f)
                    return;
            const auto targetPitch = pitchAt(anchors, closestLocal);
            anchors.push_back({ closestLocal, targetPitch });
            std::stable_sort(anchors.begin(), anchors.end(), [](const auto& left, const auto& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
            selectedNote = closestNote->id;
            selectedNotes.clear();
            selectedNotes.insert(selectedNote.toStdString());
            if (onNoteSelected) onNoteSelected(selectedNote);
            model.setNotePitchCurve(closestNote->id, anchors, true);
            repaint();
            return;
        }
        return;
    }
    for (auto it = noteHits.rbegin(); it != noteHits.rend(); ++it)
    {
        if (!it->bounds.contains(event.position)) continue;
        if (tool == Tool::note)
        {
            draggedNote.clear();
            dragMode = DragMode::none;
            pitchStroke.clear();
            selectedNote = it->id;
            selectedNotes.clear();
            selectedNotes.insert(it->id.toStdString());
            if (onNoteSelected) onNoteSelected(it->id);
            // Only where a lyric means anything.  Naming a note is a UTAU
            // idea -- the label chooses the sample to sing -- and committing
            // one converts the whole track to UTAU.  On a track being pitch
            // shifted from its own recording there is nothing to choose, and
            // a double-click here quietly changed what the track was.
            if (isUtauNote(it->id)) beginInlineAliasEdit(*it);
            repaint();
            return;
        }
        for (const auto& track : snapshot.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (note.id == it->id)
                    {
                        draggedNote.clear();
                        dragMode = DragMode::none;
                        const auto local = juce::jlimit(0.0, note.durationSeconds,
                            static_cast<double>(event.position.x - it->bounds.getX())
                                / pixelsPerSecond);
                        if (tool == Tool::connect)
                        {
                            const auto created = model.splitNote(note.id, local);
                            if (created.isNotEmpty())
                            {
                                selectedNote = created;
                                selectedNotes.clear();
                                selectedNotes.insert(created.toStdString());
                                if (onNoteSelected) onNoteSelected(created);
                            }
                            return;
                        }
                        const auto noteStart = clip.startSeconds + note.startSeconds;
                        const auto boundary = noteStart + note.consonantSeconds;
                        const auto boundaryX = timeToX(boundary);
                        if (std::abs(event.position.x - boundaryX) <= 7.0f)
                        {
                            const auto quantized = snapToGrid(boundary);
                            model.setNoteAttack(note.id,
                                juce::jlimit(0.0, note.durationSeconds, quantized - noteStart),
                                note.attackSpeed);
                        }
                        else
                            model.transposeNotes({ note.id }, std::round(note.midiNote) - note.midiNote);
                        return;
                    }
        return;
    }
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (dragMode == DragMode::drawNewNote)
    {
        const auto pointer = std::max(0.0,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond);
        drawLengthSeconds = drawnLengthFor(pointer - drawStartSeconds,
                                           drawUnitSeconds, drawAvailableSeconds);
        repaint();
        return;
    }
    if (draggedSampleRegion >= 0
        && draggedSampleRegion < static_cast<int>(sampleRegions.size()))
    {
        auto& region = sampleRegions[static_cast<std::size_t>(draggedSampleRegion)];
        const auto seconds = std::max(0.0,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond);
        constexpr double minimum = 0.001;
        switch (draggedRegionHandle)
        {
            case RegionHandle::start:
            {
                const auto next = std::min(region.regionEndSeconds - minimum, seconds);
                const auto fixedEnd = region.regionStartSeconds + region.fixedDurationSeconds;
                region.regionStartSeconds = next;
                region.fixedDurationSeconds = juce::jlimit(0.0,
                    region.regionEndSeconds - region.regionStartSeconds,
                    fixedEnd - region.regionStartSeconds);
                region.alignmentSeconds = juce::jlimit(region.regionStartSeconds,
                    region.regionEndSeconds, region.alignmentSeconds);
                break;
            }
            case RegionHandle::fixedEnd:
                region.fixedDurationSeconds = juce::jlimit(0.0,
                    region.regionEndSeconds - region.regionStartSeconds,
                    seconds - region.regionStartSeconds);
                break;
            case RegionHandle::alignment:
                region.alignmentSeconds = juce::jlimit(region.regionStartSeconds,
                    region.regionEndSeconds, seconds);
                break;
            case RegionHandle::end:
                region.regionEndSeconds = std::max(region.regionStartSeconds + minimum, seconds);
                region.fixedDurationSeconds = std::min(region.fixedDurationSeconds,
                    region.regionEndSeconds - region.regionStartSeconds);
                region.alignmentSeconds = std::min(region.alignmentSeconds, region.regionEndSeconds);
                break;
            case RegionHandle::none: break;
        }
        if (onSampleRegionEdited) onSampleRegionEdited(draggedSampleRegion, region, false);
        repaintDrag(event.position.x);
        return;
    }
    if (dragMode == DragMode::marquee)
    {
        marqueeCurrent = event.position;
        updateMarqueeAutoScroll(marqueeCurrent);
        repaintDrag(event.position.x);
        return;
    }
    if (draggedNote.isEmpty()) return;
    if (tool == Tool::connect) return;
    if (dragMode == DragMode::vibrato)
    {
        const auto* note = findNote(draggedNote);
        if (note == nullptr) return;
        const auto duration = std::max(1.0e-6, note->durationSeconds);
        const auto local = juce::jlimit(0.0, duration,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond
                - vibratoDragAbsoluteStart);
        const auto span = duration
            * juce::jlimit(0.0, 100.0, previewVibrato.vibratoLengthPercent) / 100.0;
        const auto start = duration - span;
        switch (draggedVibratoHandle)
        {
            case VibratoHandle::length:
                // Dragging the start earlier makes the vibrato cover more of
                // the note; the span is always measured back from its end.
                previewVibrato.vibratoLengthPercent =
                    juce::jlimit(1.0, 100.0, (duration - local) / duration * 100.0);
                break;
            case VibratoHandle::fadeIn:
                if (span > 1.0e-9)
                    previewVibrato.vibratoFadeInPercent =
                        juce::jlimit(0.0, 100.0, (local - start) / span * 100.0);
                break;
            case VibratoHandle::fadeOut:
                if (span > 1.0e-9)
                    previewVibrato.vibratoFadeOutPercent =
                        juce::jlimit(0.0, 100.0, (duration - local) / span * 100.0);
                break;
            case VibratoHandle::cycle:
                // The handle rides three quarters of the way through the first
                // cycle, so that is what the cursor distance measures.
                previewVibrato.vibratoCycleMs =
                    juce::jlimit(20.0, 2000.0, (local - start) / 0.75 * 1000.0);
                break;
            case VibratoHandle::depth:
            {
                const auto baseY = midiToY(note->midiNote) + rowHeight * 0.5f;
                const auto cents = (baseY - event.position.y) / rowHeight * 100.0f;
                previewVibrato.vibratoDepthCents =
                    juce::jlimit(1.0, 600.0, std::abs(static_cast<double>(cents)));
                break;
            }
            case VibratoHandle::offset:
            {
                // Held as a share of the depth, so the swing keeps its shape
                // and only its centre moves.
                const auto baseY = midiToY(note->midiNote) + rowHeight * 0.5f;
                const auto cents = (baseY - event.position.y) / rowHeight * 100.0f;
                const auto depth = std::max(1.0, previewVibrato.vibratoDepthCents);
                previewVibrato.vibratoOffsetPercent = juce::jlimit(-200.0, 200.0,
                    static_cast<double>(cents) / depth * 100.0);
                break;
            }
            case VibratoHandle::none: break;
        }
        repaintDrag(event.position.x);
        return;
    }
    if (dragMode == DragMode::jieSplit)
    {
        const auto span = std::max(1.0e-9, jieSpanEndSeconds - jieSpanStartSeconds);
        const auto cursorSeconds = static_cast<double>(event.position.x - 58.0f)
            / pixelsPerSecond;
        const auto index = static_cast<std::size_t>(draggedJieBoundary);
        const auto lower = index == 0 ? 0.0 : previewJieFractions[index - 1];
        const auto upper = static_cast<int>(index) + 2 >= previewJieRegions
            ? 1.0 : previewJieFractions[index + 1];
        previewJieFractions[index] = juce::jlimit(lower, upper,
            (cursorSeconds - jieSpanStartSeconds) / span);
        repaintDrag(event.position.x);
        return;
    }
    if (dragMode == DragMode::consonantLeadIn)
    {
        dragConsonantTo(std::max(0.0,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond));
        repaintDrag(event.position.x);
        return;
    }
    if (dragMode == DragMode::amplitudePoint && draggedAmplitudePoint >= 0
        && draggedAmplitudePoint < static_cast<int>(amplitudeStroke.size()))
    {
        auto& point = amplitudeStroke[static_cast<std::size_t>(draggedAmplitudePoint)];
        point.gainDb = amplitudeDragUsesLane
            ? amplitudeDbFromLaneY(event.position.y)
            : amplitudeDbFromY(amplitudeEditMidi, event.position.y);
        const auto local = static_cast<double>(event.position.x - 58.0f)
            / pixelsPerSecond - amplitudeEditAbsoluteStart;
        point.timeSeconds = juce::jlimit(amplitudePointMinimumTime,
                                         amplitudePointMaximumTime, local);
        repaintDrag(event.position.x);
        return;
    }
    if (dragMode == DragMode::flagPoint && draggedFlagPoint >= 0
        && draggedFlagPoint < static_cast<int>(flagStroke.size()))
    {
        auto& point = flagStroke[static_cast<std::size_t>(draggedFlagPoint)];
        point.value = flagValueFromLaneY(event.position.y);
        // Zoomed in, the reachable values are a slice of the flag's range;
        // rather than pinning the handle at the edge, the slice follows it.
        keepFlagValueInView(point.value);
        const auto local = static_cast<double>(event.position.x - 58.0f)
            / pixelsPerSecond - flagEditAbsoluteStart;
        point.timeSeconds = juce::jlimit(flagPointMinimumTime,
                                         flagPointMaximumTime, local);
        repaint();
        return;
    }
    if (dragMode == DragMode::pointPitch && draggedPitchAnchor >= 0
        && draggedPitchAnchor < static_cast<int>(pitchStroke.size()))
    {
        auto& anchor = pitchStroke[static_cast<std::size_t>(draggedPitchAnchor)];
        anchor.targetMidi = juce::jlimit(0.0f, 127.0f, pitchMidiFromY(event.position.y));
        if (pointDragCanMoveHorizontally)
        {
            const auto local = static_cast<double>(event.position.x - 58.0f)
                    / pixelsPerSecond - pitchEditAbsoluteStart;
            if (draggedPitchAnchor == 0 && pitchStroke.size() > 1)
            {
                // Order first: a range computed from neighbours can invert on a
                // degenerate note, and jlimit would be undefined there.
                const auto highest = pitchStroke[1].timeSeconds - minimumAnchorSeparation;
                anchor.timeSeconds = juce::jlimit(
                    std::min(pointDragMinimumTime, highest), highest, local);
            }
            else if (draggedPitchAnchor > 0
                     && draggedPitchAnchor + 1 < static_cast<int>(pitchStroke.size()))
                anchor.timeSeconds = juce::jlimit(
                    pitchStroke[static_cast<std::size_t>(draggedPitchAnchor - 1)].timeSeconds
                        + minimumAnchorSeparation,
                    pitchStroke[static_cast<std::size_t>(draggedPitchAnchor + 1)].timeSeconds
                        - minimumAnchorSeparation,
                    local);
            else if (draggedPitchAnchor > 0
                     && draggedPitchAnchor + 1 == static_cast<int>(pitchStroke.size()))
            {
                const auto lowest =
                    pitchStroke[static_cast<std::size_t>(draggedPitchAnchor - 1)].timeSeconds
                        + minimumAnchorSeparation;
                anchor.timeSeconds = juce::jlimit(
                    lowest, std::max(lowest, pointDragMaximumTime), local);
            }
        }
        repaintDrag(event.position.x);
        return;
    }
    if (dragMode == DragMode::drawPitch || dragMode == DragMode::linePitch)
    {
        const auto local = std::max(0.0,
            static_cast<double>(event.position.x - 58.0f) / pixelsPerSecond
                - pitchEditAbsoluteStart);
        PitchCurveEditPoint point { local,
            juce::jlimit(0.0f, 127.0f, pitchMidiFromY(event.position.y)) };
        if (dragMode == DragMode::linePitch)
        {
            if (pitchStroke.size() == 1) pitchStroke.push_back(point);
            else pitchStroke.back() = point;
        }
        else if (pitchStroke.empty()
                 || std::abs(pitchStroke.back().timeSeconds - point.timeSeconds) >= 0.001
                 || std::abs(pitchStroke.back().targetMidi - point.targetMidi) >= 0.02f)
            pitchStroke.push_back(point);
        repaintDrag(event.position.x);
        return;
    }
    // Moving and resizing a note wait for the pointer to travel first.
    // Placing a lyric means double clicking the note, and two presses with a
    // pixel of hand shake between them used to move it -- vertically as well,
    // since a press near a row boundary needs only one pixel to land on the
    // next pitch.  Point, envelope and boundary handles are deliberately not
    // gated: those are grabbed deliberately and want to answer at once.
    constexpr auto dragThresholdPixels = 6;
    if (dragMode == DragMode::pitch || dragMode == DragMode::moveUtauNote
        || dragMode == DragMode::resizeLeft || dragMode == DragMode::resizeRight)
    {
        if (!noteDragPassedThreshold
            && event.getDistanceFromDragStart() < dragThresholdPixels)
            return;
        noteDragPassedThreshold = true;
    }
    if (dragMode == DragMode::pitch || dragMode == DragMode::moveUtauNote)
    {
        finePitchDrag = finePitchDrag || event.mods.isAltDown();
        previewMidi = finePitchDrag
            ? juce::jlimit(0.0f, 127.0f,
                dragStartMidi - (event.position.y - dragStartY) / rowHeight)
            : juce::jlimit(0.0f, 127.0f, std::round(yToMidi(event.position.y)));
        if (dragMode == DragMode::moveUtauNote)
        {
            const auto travelX = event.getDistanceFromDragStartX();
            auto delta = static_cast<double>(travelX) / pixelsPerSecond;
            // A drag straight up or down asks for a different pitch and
            // nothing else.  Snapping used to run regardless of whether the
            // pointer had moved sideways at all, so a note not sitting on the
            // grid -- and plenty do not -- was pulled onto it the moment its
            // pitch was touched, moving in time when nothing had asked it to.
            if (std::abs(travelX) < dragThresholdPixels)
                delta = 0.0;
            // Alt keeps the established fine/free-edit convention.  Normal
            // drags snap to the same unit note resizing uses.
            else if (!event.mods.isAltDown())
            {
                const auto originalAbsolute = dragClipStartSeconds + dragStartSeconds;
                const auto targetAbsolute = originalAbsolute + delta;
                const auto barQuarters = static_cast<double>(std::max(1, snapshot.numerator))
                    * 4.0 / static_cast<double>(std::max(1, snapshot.denominator));
                const auto quantum = std::max(1.0 / 1024.0,
                    barQuarters / (4.0 * static_cast<double>(noteEditDivision())));
                const auto targetQuarter = snapshot.quarterPositionForSeconds(targetAbsolute);
                const auto snapped = snapshot.secondsForQuarterPosition(
                    std::round(targetQuarter / quantum) * quantum);
                delta = snapped - originalAbsolute;
            }
            previewMoveDeltaSeconds = std::max(minimumMoveDeltaSeconds, delta);
        }
    }
    else if (dragMode == DragMode::resizeLeft)
    {
        const auto end = dragStartSeconds + dragDurationSeconds;
        auto next = dragStartSeconds
            + static_cast<double>(event.getDistanceFromDragStartX()) / pixelsPerSecond;
        if (!event.mods.isAltDown())
        {
            const auto absolute = dragClipStartSeconds + next;
            next = snapToGrid(absolute) - dragClipStartSeconds;
        }
        previewStartSeconds = juce::jlimit(0.0, end - 0.01, next);
        previewDurationSeconds = end - previewStartSeconds;
    }
    else if (dragMode == DragMode::resizeRight)
    {
        const auto quantum = noteEditQuantumSeconds();
        const auto rawDuration = dragDurationSeconds
            + static_cast<double>(event.getDistanceFromDragStartX()) / pixelsPerSecond;
        const auto snappedDuration = std::round(rawDuration / quantum) * quantum;
        previewDurationSeconds = juce::jlimit(quantum,
            std::max(quantum, resizeMaximumDurationSeconds), snappedDuration);
    }
    repaintDrag(event.position.x);
}

void PianoRollComponent::updateMarqueeAutoScroll(juce::Point<float> position)
{
    marqueeScrollStep = 0;
    if (auto* viewport = findParentComponentOfClass<juce::Viewport>())
    {
        const auto view = viewport->getViewArea();
        // The keyboard strip sits over the left edge of the visible area, so
        // the band that triggers a scroll starts where the roll really begins.
        constexpr auto band = 28.0f;
        const auto left = static_cast<float>(view.getX()) + 58.0f + band;
        const auto right = static_cast<float>(view.getRight()) - band;
        // Steady to begin with, faster the further past the edge the pointer
        // goes, so a long sweep does not take all day and a small overshoot
        // does not bolt.
        const auto speed = [](float overshoot)
        {
            return juce::jlimit(6, 30, 6 + juce::roundToInt(overshoot / 4.0f));
        };
        if (position.x < left) marqueeScrollStep = -speed(left - position.x);
        else if (position.x > right) marqueeScrollStep = speed(position.x - right);
    }
    if (marqueeScrollStep != 0) { if (!isTimerRunning()) startTimerHz(30); }
    else stopTimer();
}

void PianoRollComponent::timerCallback()
{
    auto* viewport = findParentComponentOfClass<juce::Viewport>();
    if (dragMode != DragMode::marquee || marqueeScrollStep == 0 || viewport == nullptr)
    {
        stopTimer();
        return;
    }
    const auto before = viewport->getViewPositionX();
    const auto furthest = std::max(0, getWidth() - viewport->getViewWidth());
    viewport->setViewPosition(juce::jlimit(0, furthest, before + marqueeScrollStep),
                              viewport->getViewPositionY());
    if (viewport->getViewPositionX() == before)
    {
        // Already against the end of the roll: nothing further to reveal, and
        // repainting thirty times a second for it would be waste.
        stopTimer();
        return;
    }
    // The roll slid underneath a pointer that has not moved, so the corner of
    // the marquee follows it to its new place in the component.
    marqueeCurrent = getMouseXYRelative().toFloat();
    updateMarqueeAutoScroll(marqueeCurrent);
    repaintDrag(marqueeCurrent.x);
}

void PianoRollComponent::mouseUp(const juce::MouseEvent&)
{
    finishDrag();
}

void PianoRollComponent::finishDrag()
{
    lastDragBand = {};
    if (dragMode == DragMode::drawNewNote)
    {
        dragMode = DragMode::none;
        // The model searches for the free start again under its own lock: the
        // snapshot this preview was drawn from can be a note out of date.
        const auto id = model.addNoteFrom(drawClipId, drawStartSeconds,
                                          drawLengthSeconds, drawMidi);
        drawClipId.clear();
        drawLengthSeconds = 0.0;
        if (id.isNotEmpty())
        {
            selectedNote = id;
            selectedNotes.clear();
            selectedNotes.insert(id.toStdString());
            if (onNoteSelected) onNoteSelected(id);
        }
        repaint();
        return;
    }
    if (draggedSampleRegion >= 0
        && draggedSampleRegion < static_cast<int>(sampleRegions.size()))
    {
        if (onSampleRegionEdited)
            onSampleRegionEdited(draggedSampleRegion,
                sampleRegions[static_cast<std::size_t>(draggedSampleRegion)], true);
        draggedSampleRegion = -1;
        draggedRegionHandle = RegionHandle::none;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    if (dragMode == DragMode::marquee)
    {
        stopTimer();
        marqueeScrollStep = 0;
        const auto selection = juce::Rectangle<float>(marqueeStart, marqueeCurrent);
        // Drawing a box round some notes is choosing them, not choosing a
        // place, so the playhead goes back where it was.  Left at the corner
        // the box was started from, the next paste landed there -- just before
        // the note that had been selected, which is nobody's idea of where a
        // paste should go.  A press that never became a box is still someone
        // putting the playhead down, and keeps its new place.
        if (onEditPosition != nullptr && selection.getWidth() > 3.0f)
            onEditPosition(playheadBeforeGesture);
        if (!marqueeAddsToSelection) selectedNotes.clear();
        // By the block that is drawn, not by the sounding stretch.  A note's
        // lead-in lies over the note before it, so a band drawn wholly inside
        // one note's own block was picking up the next one as well -- and the
        // popup menu and the resize tail already go by the block for exactly
        // that reason.
        for (const auto& hit : noteHits)
            if (selection.intersects(hit.nominalBounds))
                selectedNotes.insert(hit.id.toStdString());
        selectedNote = selectedNotes.empty() ? juce::String()
            : juce::String::fromUTF8(selectedNotes.begin()->c_str());
        dragMode = DragMode::none;
        if (onNoteSelected) onNoteSelected(selectedNote);
        repaint();
        return;
    }
    if (draggedNote.isEmpty()) return;
    const auto geometryDrag = dragMode == DragMode::pitch
        || dragMode == DragMode::moveUtauNote
        || dragMode == DragMode::resizeLeft
        || dragMode == DragMode::resizeRight;
    if (geometryDrag && !noteDragPassedThreshold)
    {
        // Left exactly as it was.  Committing a zero move still writes an undo
        // step and asks for a re-render, which is what made a double click
        // feel like an edit even when nothing had changed.
    }
    else if (dragMode == DragMode::drawPitch || dragMode == DragMode::linePitch
        || dragMode == DragMode::pointPitch)
        model.setNotePitchCurve(draggedNote, std::move(pitchStroke),
                                dragMode == DragMode::pointPitch);
    else if (dragMode == DragMode::amplitudePoint)
        commitAmplitudeEnvelopeToSelection(draggedNote, amplitudeStroke);
    else if (dragMode == DragMode::flagPoint)
        model.setNoteUtauFlagCurve(draggedNote, laneFlag, flagStroke);
    else if (dragMode == DragMode::vibrato)
        model.setNotesVibrato({ draggedNote }, previewVibrato, true);
    else if (dragMode == DragMode::jieSplit)
        model.setNotesUtauJieSplit({ draggedNote }, previewJieFractions[0],
                                   previewJieFractions[1], previewJieFractions[2]);
    else if (dragMode == DragMode::consonantLeadIn)
    {
        if (consonantSetsPin)
            // 谋 pins the lead-in the drag asked for.  Backing into it through
            // the consonant velocity rewrites a setting the note carries --
            // and for a first region the entry calls a vowel the velocity does
            // not reach the lead-in at all, so the handle moved nothing.  The
            // overlap goes back in as it was: the two share one pin.
            model.setNoteUtauTimingOverrides(draggedNote, true,
                                             previewConsonantPreutterance,
                                             consonantOverlapSeconds);
        else
        {
            // A pinned preutterance outranks the velocity, so with one in
            // place this handle could be dragged the width of the screen and
            // the lead-in would not move.  Reaching for it is a request for
            // the lead-in the velocity gives, so the pin is released.
            if (const auto* dragged = findNote(draggedNote);
                dragged != nullptr && dragged->utauPreutteranceOverrideEnabled)
                model.setNoteUtauTimingOverrides(draggedNote, false, 0.0, 0.0);
            model.setNotesUtauConsonantVelocity({ draggedNote },
                                                previewConsonantVelocity);
        }
    }
    else if (dragMode == DragMode::pitch
             || dragMode == DragMode::moveUtauNote)
    {
        std::vector<juce::String> ids;
        ids.reserve(selectedNotes.size());
        for (const auto& id : selectedNotes) ids.push_back(juce::String::fromUTF8(id.c_str()));
        if (ids.empty()) ids.push_back(draggedNote);
        if (dragMode == DragMode::moveUtauNote)
            model.moveUtauNotes(ids, previewMoveDeltaSeconds,
                                previewMidi - dragStartMidi);
        else
            model.transposeNotes(ids, previewMidi - dragStartMidi);
    }
    else
        model.resizeNote(draggedNote, previewStartSeconds, previewDurationSeconds);
    draggedNote.clear();
    finePitchDrag = false;
    draggedPitchAnchor = -1;
    draggedAmplitudePoint = -1;
    pitchStroke.clear();
    amplitudeStroke.clear();
    amplitudeDragUsesLane = false;
    previewMoveDeltaSeconds = 0.0;
    minimumMoveDeltaSeconds = 0.0;
    pointDragCanMoveHorizontally = false;
    pointDragMaximumTime = 0.0;
    pointDragMinimumTime = 0.0;
    dragMode = DragMode::none;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'A')
    {
        selectAllNotes();
        return true;
    }
    if ((key.getKeyCode() == juce::KeyPress::deleteKey
         || key.getKeyCode() == juce::KeyPress::backspaceKey)
        && (selectedNote.isNotEmpty() || !selectedNotes.empty()))
    {
        deleteSelectedNotes();
        return true;
    }
    return false;
}
}
