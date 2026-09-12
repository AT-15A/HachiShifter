#include "MainComponent.h"
#include "OtoWaveformEditorComponent.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

namespace hachi
{
namespace
{
juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }

class ComposeTrackSelector final : public juce::Component
{
public:
    ComposeTrackSelector(const std::vector<TrackData>& tracks, const I18n& strings)
    {
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setScrollBarThickness(9);
        addAndMakeVisible(viewport);
        for (const auto& track : tracks)
        {
            auto toggle = std::make_unique<juce::ToggleButton>();
            toggle->setButtonText(track.name + utf8("  ·  ")
                                  + strings.text(track.compose ? "track.compose" : "track.audio"));
            toggle->setToggleState(track.compose, juce::dontSendNotification);
            content.addAndMakeVisible(*toggle);
            toggles.push_back(std::move(toggle));
        }
        setSize(430, juce::jlimit(56, 320, static_cast<int>(toggles.size()) * rowHeight + 4));
    }

    bool isCompose(std::size_t index) const
    {
        return index < toggles.size() && toggles[index]->getToggleState();
    }

    void resized() override
    {
        viewport.setBounds(getLocalBounds());
        const auto contentHeight = std::max(getHeight(), static_cast<int>(toggles.size()) * rowHeight + 4);
        content.setSize(std::max(1, getWidth() - viewport.getScrollBarThickness()), contentHeight);
        for (std::size_t index = 0; index < toggles.size(); ++index)
            toggles[index]->setBounds(6, 2 + static_cast<int>(index) * rowHeight,
                                      content.getWidth() - 12, rowHeight);
    }

private:
    static constexpr int rowHeight = 28;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
};
}

juce::Rectangle<int> DropdownButton::textAreaFor(juce::Rectangle<int> bounds)
{
    return bounds.reduced(4, 0).withTrimmedRight(HachiLookAndFeel::dropdownArrowWidth);
}

void DropdownButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    getLookAndFeel().drawButtonBackground(g, *this,
        findColour(juce::TextButton::buttonColourId), highlighted, down);
    g.setColour(findColour(getToggleState() ? juce::TextButton::textColourOnId
                                            : juce::TextButton::textColourOffId)
                    .withAlpha(highlighted || down ? 1.0f : 0.86f));
    g.setFont(getLookAndFeel().getTextButtonFont(*this, getHeight()));
    g.drawText(getButtonText(), textAreaFor(getLocalBounds()),
               juce::Justification::centred, false);
    HachiLookAndFeel::drawDropdownArrow(g, getLocalBounds());
}

void EnvelopePresetButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    getLookAndFeel().drawButtonBackground(g, *this,
        findColour(juce::TextButton::buttonColourId), highlighted, down);

    auto area = getLocalBounds().toFloat().reduced(4.0f, 3.0f);
    const auto captionHeight = caption.isEmpty() ? 0.0f : 11.0f;
    auto plot = area.removeFromTop(std::max(8.0f, area.getHeight() - captionHeight));

    // The ramps keep their true ratio to each other but not to the note: a 5 ms
    // attack against a note of any real length would be a fraction of a pixel.
    // Together they take a fixed share of the width, so the four presets are
    // told apart by the same thing that distinguishes them in use.
    const auto ramps = std::max(1.0e-6, attackSeconds + releaseSeconds);
    const auto rampWidth = plot.getWidth() * 0.62f;
    const auto attackWidth = rampWidth * static_cast<float>(attackSeconds / ramps);
    const auto releaseWidth = rampWidth - attackWidth;
    const auto floorY = plot.getBottom();
    const auto peakY = plot.getY();
    const auto holdEnd = peakY + (floorY - peakY)
        * (1.0f - std::pow(10.0f, plateauEndDb / 20.0f));

    juce::Path shape;
    shape.startNewSubPath(plot.getX(), floorY);
    shape.lineTo(plot.getX() + attackWidth, peakY);
    shape.lineTo(plot.getRight() - releaseWidth, holdEnd);
    shape.lineTo(plot.getRight(), floorY);
    shape.closeSubPath();

    const auto tint = juce::Colour(0xff72d6aa);
    g.setColour(tint.withAlpha(highlighted || down ? 0.42f : 0.26f));
    g.fillPath(shape);
    g.setColour(tint.withAlpha(highlighted || down ? 1.0f : 0.85f));
    g.strokePath(shape, juce::PathStrokeType(1.3f));

    if (caption.isNotEmpty())
    {
        g.setColour(findColour(juce::TextButton::textColourOffId)
                        .withAlpha(highlighted || down ? 1.0f : 0.8f));
        g.setFont(9.5f);
        g.drawText(caption, area, juce::Justification::centred, false);
    }
}

MainComponent::MainComponent()
    : tooltipWindow(this, 450), menuBar(this), progressBar(progress),
      trackList(project, strings), timeline(project), pianoRoll(project, strings)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "HachiShifterNext";
    options.filenameSuffix = "settings";
    options.folderName = "HachiShifterNext";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    preferences = std::make_unique<juce::PropertiesFile>(options);
    restoreRecentProjects();
    savedProjectRevision = project.revisionNumber();
    audio.restoreDeviceState(*preferences);
    applyPreferences();
    setLookAndFeel(&lookAndFeel);
    setOpaque(true);
    setWantsKeyboardFocus(true);

    for (auto* button : { &openButton, &saveButton, &audioButton, &melodyneButton,
                          &playButton, &stopButton, &noteEditButton, &wrenchButton,
                          &lineButton, &pointButton, &connectButton, &pitchParamButton,
                          &driftParamButton, &attackParamButton,
                          &breathParamButton, &tensionParamButton, &formantParamButton,
                          &volumeParamButton, &voicebankSettingsButton,
                          &spliceButton, &flagCurveButton, &flagEnvelopeButton })
        addAndMakeVisible(*button);
    addAndMakeVisible(showViewMenuButton);
    addAndMakeVisible(drawButton);
    for (auto* component : { static_cast<juce::Component*>(&menuBar),
                             static_cast<juce::Component*>(&bpmCaption),
                             static_cast<juce::Component*>(&bpmEditor),
                             static_cast<juce::Component*>(&beatsCaption),
                             static_cast<juce::Component*>(&beatsEditor),
                             static_cast<juce::Component*>(&denominatorLabel),
                             static_cast<juce::Component*>(&gridCaption),
                             static_cast<juce::Component*>(&gridSelector),
                             static_cast<juce::Component*>(&stretchCaption),
                             static_cast<juce::Component*>(&stretchSelector),
                             static_cast<juce::Component*>(&scaleCaption),
                             static_cast<juce::Component*>(&scaleSelector),
                             static_cast<juce::Component*>(&pitchAlgorithm),
                             static_cast<juce::Component*>(&stretchAlgorithm),
                             static_cast<juce::Component*>(&renderOrder),
                             static_cast<juce::Component*>(&pitchLabel),
                             static_cast<juce::Component*>(&stretchLabel),
                             static_cast<juce::Component*>(&renderOrderLabel),
                             static_cast<juce::Component*>(&statusLabel),
                             static_cast<juce::Component*>(&sourceEditHint),
                             static_cast<juce::Component*>(&sampleRegionSelector),
                             static_cast<juce::Component*>(&sampleAliasEditor),
                             static_cast<juce::Component*>(&sampleStartEditor),
                             static_cast<juce::Component*>(&sampleEndEditor),
                             static_cast<juce::Component*>(&sampleAlignmentEditor),
                             static_cast<juce::Component*>(&sampleFixedEditor),
                             static_cast<juce::Component*>(&sampleAliasLabel),
                             static_cast<juce::Component*>(&sampleStartLabel),
                             static_cast<juce::Component*>(&sampleEndLabel),
                             static_cast<juce::Component*>(&sampleAlignmentLabel),
                             static_cast<juce::Component*>(&sampleFixedLabel),
                             static_cast<juce::Component*>(&sampleSaveButton),
                             static_cast<juce::Component*>(&otoImportButton),
                             static_cast<juce::Component*>(&otoExportButton),
                             static_cast<juce::Component*>(&utauVoicebankLabel),
                             static_cast<juce::Component*>(&utauVoicebankButton),
                             static_cast<juce::Component*>(&utauVoicebankPath),
                             static_cast<juce::Component*>(&noteAliasLabel),
                             static_cast<juce::Component*>(&noteAliasEditor),
                             static_cast<juce::Component*>(&noteConsonantVelocityLabel),
                             static_cast<juce::Component*>(&noteConsonantVelocityEditor),
                             static_cast<juce::Component*>(&noteFlagsLabel),
                             static_cast<juce::Component*>(&noteFlagsEditor),
                             static_cast<juce::Component*>(&parameterTitle),
                             static_cast<juce::Component*>(&smoothCaption),
                             static_cast<juce::Component*>(&smoothSlider),
                             static_cast<juce::Component*>(&robustPitchCurveButton),
                             static_cast<juce::Component*>(&zoomSlider),
                             static_cast<juce::Component*>(&vZoomSlider),
                             static_cast<juce::Component*>(&progressBar),
                             static_cast<juce::Component*>(&collapseTracksButton),
                             static_cast<juce::Component*>(&trackViewport),
                             static_cast<juce::Component*>(&timelineViewport),
                             static_cast<juce::Component*>(&pianoViewport),
                             static_cast<juce::Component*>(&panelSplitter) })
        addAndMakeVisible(*component);

    for (auto* button : { &horizontalZoomOutButton, &horizontalZoomInButton,
                          &verticalZoomOutButton, &verticalZoomInButton })
        addAndMakeVisible(*button);

    panelSplitter.setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    panelSplitter.addMouseListener(this, false);

    for (auto* editor : { &sampleAliasEditor, &sampleStartEditor, &sampleEndEditor,
                          &sampleAlignmentEditor, &sampleFixedEditor })
        editor->setSelectAllWhenFocused(true);
    for (auto* editor : { &sampleStartEditor, &sampleEndEditor,
                          &sampleAlignmentEditor, &sampleFixedEditor })
        editor->setInputRestrictions(14, "0123456789.-");
    sampleRegionSelector.onChange = [this]
    {
        commitSampleEditors();
        activeSampleSetting = std::max(0, sampleRegionSelector.getSelectedItemIndex());
        refreshSampleEditors();
    };
    const auto commit = [this] { commitSampleEditors(); };
    sampleAliasEditor.onFocusLost = commit;
    sampleStartEditor.onFocusLost = commit;
    sampleEndEditor.onFocusLost = commit;
    sampleAlignmentEditor.onFocusLost = commit;
    sampleFixedEditor.onFocusLost = commit;
    sampleSaveButton.onClick = [this] { saveSampleSettings(); };
    otoImportButton.onClick = [this] { importOto(); };
    otoExportButton.onClick = [this] { exportOto(); };
    utauVoicebankButton.onClick = [this] { chooseUtauVoicebank(); };
    voicebankSettingsButton.onClick = [this]
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        juce::MessageManager::callAsync([safe]
        {
            if (safe != nullptr) safe->showVoicebankSettings();
        });
    };
    noteAliasEditor.setSelectAllWhenFocused(true);
    noteAliasEditor.onTextChange = [this]
    {
        // Typing saves, like the Flags and consonant-velocity fields beside it.
        // Only the value is written here; switching the track to UTAU, binding a
        // default voicebank and refreshing the layout stay on focus loss and
        // Return so they do not run once per keystroke.
        if (selectedNoteId.isEmpty() || !noteAliasEditor.isEnabled()) return;
        project.setNoteLabel(selectedNoteId, noteAliasEditor.getText());
        pianoRoll.ensureDefaultEnvelope(selectedNoteId);
    };
    noteAliasEditor.onFocusLost = [this] { commitNoteAlias(); };
    noteAliasEditor.onReturnKey = [this] { commitNoteAlias(); };
    noteFlagsEditor.setSelectAllWhenFocused(true);
    noteFlagsEditor.onTextChange = [this]
    {
        noteFlagsDirty = true;
        commitNoteFlags();
    };
    noteFlagsEditor.onFocusLost = [this] { commitNoteFlags(); };
    noteFlagsEditor.onReturnKey = [this] { commitNoteFlags(); };
    noteConsonantVelocityEditor.setSelectAllWhenFocused(true);
    noteConsonantVelocityEditor.setInputRestrictions(0, "-0123456789");
    noteConsonantVelocityEditor.setJustification(juce::Justification::centred);
    noteConsonantVelocityEditor.onTextChange = [this]
    {
        noteConsonantVelocityDirty = true;
        commitNoteConsonantVelocity();
    };
    noteConsonantVelocityEditor.onFocusLost = [this]
    {
        commitNoteConsonantVelocity();
        refreshSelectedNoteParameter();
    };
    noteConsonantVelocityEditor.onReturnKey = [this] { commitNoteConsonantVelocity(); };
    pianoRoll.onSampleRegionEdited = [this](int index, const SampleRegionSetting& row, bool)
    {
        if (index < 0 || index >= static_cast<int>(sampleSettingsRows.size())) return;
        activeSampleSetting = index;
        sampleSettingsRows[static_cast<std::size_t>(index)] = row;
        sampleRegionSelector.setSelectedItemIndex(index, juce::dontSendNotification);
        refreshSampleEditors();
    };

    openButton.setComponentID("icon.open");
    saveButton.setComponentID("icon.save");
    audioButton.setComponentID("icon.audio");
    melodyneButton.setComponentID("icon.melodyne");
    refreshCollapseIcon();
    playButton.setComponentID("icon.play");
    stopButton.setComponentID("icon.stop");
    noteEditButton.setComponentID("icon.pointer");
    drawButton.setComponentID("icon.draw");
    lineButton.setComponentID("icon.line");
    pointButton.setComponentID("icon.points");
    wrenchButton.setComponentID("icon.wrench");
    connectButton.setComponentID("icon.connect");

    bpmEditor.setEditable(true, false, false);
    beatsEditor.setEditable(true, false, false);
    for (auto* editor : { &bpmEditor, &beatsEditor })
    {
        editor->setJustificationType(juce::Justification::centred);
        editor->setColour(juce::Label::backgroundColourId, Palette::background);
        editor->setColour(juce::Label::outlineColourId, Palette::grid);
    }
    bpmEditor.onTextChange = [this]
    {
        const auto value = bpmEditor.getText().getDoubleValue();
        if (value >= 20.0 && value <= 400.0)
        {
            const auto data = project.snapshot();
            project.setTempo(value, data.numerator, data.denominator);
        }
    };
    beatsEditor.onTextChange = [this]
    {
        const auto value = beatsEditor.getText().getIntValue();
        if (value >= 1 && value <= 32)
        {
            const auto data = project.snapshot();
            project.setTempo(data.bpm, value, data.denominator);
        }
    };

    for (const auto& value : { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64",
                               "1/4.", "1/8.", "1/16.", "1/4t", "1/8t", "1/16t" })
        gridSelector.addItem(value, gridSelector.getNumItems() + 1);
    for (const auto& value : { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" })
        scaleSelector.addItem(value, scaleSelector.getNumItems() + 1);
    // Item ids are the denominators themselves, so the id is the setting.
    for (const auto value : { 2, 4, 8, 16, 32, 64, 128 })
        stretchSelector.addItem("1/" + juce::String(value), value);
    gridSelector.onChange = [this] { project.setGridDivision(gridSelector.getText()); };
    stretchSelector.onChange = [this]
    {
        project.setNoteEditDivision(stretchSelector.getSelectedId());
    };
    scaleSelector.onChange = [this] { project.setBaseScale(scaleSelector.getText()); };

    trackViewport.setViewedComponent(&trackList, false);
    trackViewport.setScrollBarsShown(true, false);
    trackViewport.setScrollBarThickness(10);
    timelineViewport.setViewedComponent(&timeline, false);
    timelineViewport.setScrollBarsShown(true, true);
    timelineViewport.setScrollBarThickness(10);
    pianoViewport.setViewedComponent(&pianoRoll, false);
    pianoViewport.setScrollBarsShown(true, true);
    pianoViewport.setScrollBarThickness(10);
    // The wheel scrolls and the modifiers zoom, which is the way round every
    // other editor of this kind works:
    //
    //     wheel              scroll up and down
    //     shift + wheel      scroll back and forth
    //     ctrl + wheel       zoom vertically
    //     ctrl + shift       zoom horizontally
    //
    // Plain scrolling is left to the Viewport itself rather than reimplemented,
    // and the sideways one is handed to the horizontal scrollbar with the
    // wheel's delta moved onto the axis that bar reads -- so both directions
    // move by the same amount per notch instead of by two hand-picked ones.
    const auto wheelFor = [this](EditorViewport& viewport)
    {
        return [this, &viewport](const juce::MouseEvent& event,
                                 const juce::MouseWheelDetails& wheel) -> bool
        {
            auto delta = wheel.deltaY;
            if (wheel.isReversed) delta = -delta;
            const auto factor = std::exp(static_cast<double>(delta) * 1.35);
            switch (wheelActionFor(event.mods))
            {
                case WheelAction::zoomHorizontally:
                    zoomSlider.setValue(juce::jlimit(zoomSlider.getMinimum(),
                        zoomSlider.getMaximum(), zoomSlider.getValue() * factor));
                    return true;
                case WheelAction::zoomVertically:
                    vZoomSlider.setValue(juce::jlimit(0.5, 2.0,
                        vZoomSlider.getValue() * factor));
                    return true;
                case WheelAction::scrollHorizontally:
                {
                    auto sideways = wheel;
                    sideways.deltaX = wheel.deltaY;
                    sideways.deltaY = 0.0f;
                    viewport.getHorizontalScrollBar().mouseWheelMove(event, sideways);
                    return true;
                }
                case WheelAction::scrollVertically:
                    break;
            }
            // Unhandled, so the Viewport scrolls the way it always has.
            return false;
        };
    };
    timelineViewport.onWheel = wheelFor(timelineViewport);
    pianoViewport.onWheel = wheelFor(pianoViewport);

    pitchAlgorithm.addItem("mld5", 1);
    pitchAlgorithm.addItem("nsf-hifigan", 2);
    pitchAlgorithm.addItem("WORLD", 3);
    pitchAlgorithm.addItem("vslib", 4);
    pitchAlgorithm.addItem("llsm2", 6);
    pitchAlgorithm.addItem("UTAU", 7);
    // The other UTAU modes.  Same algorithm as item 7 plus TrackData::utauMode,
    // so every UTAU affordance is inherited rather than reimplemented.
    pitchAlgorithm.addItem(utf8("界•UTAU"), 8);
    pitchAlgorithm.addItem(utf8("谋•UTAU"), 9);
    pitchAlgorithm.setSelectedId(1);
    refreshStretchAlgorithmItems(1);
    pitchAlgorithm.onChange = [this]
    {
        const auto id = pitchAlgorithm.getSelectedId();
        const auto chosenUtau = utauModeForPickerItem(id);
        const auto utauItem = chosenUtau.has_value();
        if (!utauItem && utauAmplitudeEnvelopeActive)
        {
            closeEnvelopeLanes();
            pianoRoll.setTool(PianoRollComponent::Tool::note);
            setToolButton(noteEditButton);
        }
        else if (utauItem && volumeParamButton.getToggleState()
                 && !utauAmplitudeEnvelopeActive)
        {
            pianoRoll.setTool(PianoRollComponent::Tool::note);
            setToolButton(noteEditButton);
        }
        const auto previousStretch = stretchAlgorithm.getSelectedId();
        refreshStretchAlgorithmItems(previousStretch);
        const auto algorithm = id == 2 ? PitchAlgorithm::nsfHifigan
            : id == 3 ? PitchAlgorithm::world
            : id == 4 ? PitchAlgorithm::vocalShifter
            : id == 5 ? PitchAlgorithm::mld3
            : id == 6 ? PitchAlgorithm::llsm2
            : utauItem ? PitchAlgorithm::utau : PitchAlgorithm::mld5;
        const auto utauMode = chosenUtau.value_or(UtauMode::classic);
        const auto data = project.snapshot();
        const auto selectedTrack = std::find_if(data.tracks.begin(), data.tracks.end(),
            [this](const auto& track)
            {
                return track.id == selectedTrackId;
            });
        if (selectedTrack != data.tracks.end())
        {
            project.setTrackPitchAlgorithm(selectedTrack->id, algorithm);
            project.setTrackUtauMode(selectedTrack->id, utauMode);
            if (algorithm == PitchAlgorithm::utau
                && !selectedTrack->voicebankDirectory.isDirectory())
                bindDefaultUtauVoicebank(selectedTrack->id);
        }
        else
        {
            project.setPitchAlgorithm(algorithm);
            project.setUtauMode(utauMode);
        }
        if (previousStretch != stretchAlgorithm.getSelectedId())
        {
            if (selectedTrack != data.tracks.end())
                project.setTrackStretchAlgorithm(selectedTrack->id,
                                                  StretchAlgorithm::melodyneHybrid);
            else
                project.setStretchAlgorithm(StretchAlgorithm::melodyneHybrid);
        }
        refreshSelectedNoteParameter();
        resized();
    };
    renderOrder.onChange = [this]
    {
        const auto order = renderOrder.getSelectedId() == 2
            ? RenderOrder::stretchSpliceThenPitch : RenderOrder::processThenSplice;
        const auto data = project.snapshot();
        const auto selectedTrack = std::find_if(data.tracks.begin(), data.tracks.end(),
            [this](const auto& track)
            {
                return track.id == selectedTrackId;
            });
        if (selectedTrack != data.tracks.end())
            project.setTrackRenderOrder(selectedTrack->id, order);
        else
            project.setRenderOrder(order);
    };
    stretchAlgorithm.onChange = [this]
    {
        const auto id = stretchAlgorithm.getSelectedId();
        const auto algorithm = id == 2 ? StretchAlgorithm::variableMelHop
            : id == 3 ? StretchAlgorithm::loop
            : id == 4 ? StretchAlgorithm::soundTouch
            : id == 5 ? StretchAlgorithm::nsfShiftThenSplice
            : StretchAlgorithm::melodyneHybrid;
        const auto data = project.snapshot();
        const auto selectedTrack = std::find_if(data.tracks.begin(), data.tracks.end(),
            [this](const auto& track)
            {
                return track.id == selectedTrackId;
            });
        if (selectedTrack != data.tracks.end())
            project.setTrackStretchAlgorithm(selectedTrack->id, algorithm);
        else
            project.setStretchAlgorithm(algorithm);
    };

    smoothSlider.setRange(0.0, 100.0, 1.0);
    smoothSlider.setValue(0.0);
    smoothSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    smoothSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
    smoothSlider.onValueChange = [this]
    {
        if (updatingSmoothSlider || smoothSliderDragging || selectedNoteId.isEmpty()
            || !smoothSlider.isEnabled()) return;
        applySelectedNoteParameter();
    };
    smoothSlider.onDragStart = [this] { smoothSliderDragging = true; };
    smoothSlider.onDragEnd = [this]
    {
        smoothSliderDragging = false;
        if (selectedNoteId.isNotEmpty() && smoothSlider.isEnabled())
            applySelectedNoteParameter();
    };
    robustPitchCurveButton.onClick = [this]
    {
        if (updatingRobustPitchCurve || selectedNoteId.isEmpty()
            || !robustPitchCurveButton.isEnabled()) return;
        project.setNoteRobustPitchCurve(selectedNoteId,
            robustPitchCurveButton.getToggleState());
    };

    // Far enough in that a millisecond is eight pixels wide, which is the
    // smallest gap two pitch points are allowed to keep -- so the view can now
    // resolve the finest edit the model permits, rather than stopping at a
    // little over half a pixel per millisecond.
    zoomSlider.setRange(40.0, 8000.0, 1.0);
    // Without a skew the first tenth of the slider would hold everything from
    // reading a whole phrase to ordinary note editing.
    zoomSlider.setSkewFactorFromMidPoint(280.0);
    zoomSlider.setValue(140.0);
    zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    zoomSlider.onValueChange = [this]
    {
        const auto zoom = static_cast<float>(zoomSlider.getValue());
        // What the zoom should hold still, decided before the scale changes.
        // The red playhead is the editing focus and stays centred whenever it
        // is actually in view; when it is not, centring on it threw the view
        // across the piece for no reason the user could see, so what is on
        // screen is held instead.  This callback also covers the toolbar
        // slider, mouse-wheel zoom, menu commands and the +/- buttons.
        const auto viewWidth = pianoViewport.getViewWidth();
        const auto viewLeft = pianoViewport.getViewPositionX();
        const auto playheadPixel = pianoRoll.pixelForSeconds(audio.position());
        const auto playheadInView = playheadPixel >= viewLeft
            && playheadPixel < viewLeft + viewWidth;
        const auto anchorSeconds = playheadInView
            ? audio.position()
            : pianoRoll.secondsForPixel(viewLeft + viewWidth / 2);
        timeline.setPixelsPerSecond(zoom);
        pianoRoll.setPixelsPerSecond(zoom);
        if (viewWidth > 0)
        {
            const auto nextViewX = std::max(0,
                pianoRoll.pixelForSeconds(anchorSeconds) - viewWidth / 2);
            pianoViewport.setViewPosition(nextViewX, pianoViewport.getViewPositionY());
            timelineViewport.setViewPosition(nextViewX, timelineViewport.getViewPositionY());
            lastPianoX = pianoViewport.getViewPositionX();
            lastTimelineX = timelineViewport.getViewPositionX();
        }
    };

    vZoomSlider.setRange(0.5, 2.0, 0.05);
    vZoomSlider.setValue(1.0);
    vZoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    vZoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    vZoomSlider.onValueChange = [this]
    {
        const auto factor = static_cast<float>(vZoomSlider.getValue());
        const auto pianoRow = 22.0f * factor;
        pianoRoll.setRowHeight(pianoRow);
        const auto row = 96.0f * factor;
        timeline.setRowHeight(row);
        trackList.setRowHeight(row);
        if (preferences != nullptr)
            preferences->setValue("ui.vZoom", vZoomSlider.getValue());
    };
    horizontalZoomOutButton.onClick = [this] { adjustHorizontalZoom(1.0 / 1.25); };
    horizontalZoomInButton.onClick = [this] { adjustHorizontalZoom(1.25); };
    verticalZoomOutButton.onClick = [this] { adjustVerticalZoom(1.0 / 1.2); };
    verticalZoomInButton.onClick = [this] { adjustVerticalZoom(1.2); };
    timeline.onSeek = [this](double seconds) { audio.setPosition(seconds); };
    // The same menu from either half of the track area: the headers on the
    // left and the lanes on the right are one surface to the person using it.
    const auto trackAreaMenu = [this](juce::Point<int> at) { showTrackAreaMenu(at); };
    timeline.onEmptyAreaMenu = trackAreaMenu;
    trackList.onEmptyAreaMenu = trackAreaMenu;
    timeline.onClipSelected = [this](const juce::String& clipId) { focusClip(clipId); };
    timeline.onClipGainRequested = [this](const juce::String& clipId)
    {
        selectedClipId = clipId;
        showClipGainDialog();
    };
    pianoRoll.onSeek = [this](double seconds) { audio.setPosition(seconds); };
    pianoRoll.onLoadVibratoPresets = [this]
    {
        return preferences != nullptr
            ? preferences->getValue("ui.vibratoPresets") : juce::String();
    };
    pianoRoll.onSaveVibratoPresets = [this](const juce::String& text)
    {
        if (preferences != nullptr) preferences->setValue("ui.vibratoPresets", text);
    };
    pianoRoll.onEditPosition = [this](double seconds)
    {
        // Following the edit must never derail playback: while something is
        // playing the playhead belongs to the transport, not to the cursor.
        if (!audio.isPlaying()) audio.setPosition(seconds);
    };
    pianoRoll.onNoteSelected = [this](const juce::String& noteId)
    {
        focusNote(noteId);
        updateUtauRenderSelection();
        // Splicing needs a boundary, so the button follows the selection.
        refreshSpliceButton();
    };
    pianoRoll.onOpenRegionEditor = [this](const juce::String& noteId)
    {
        showRegionEditorForNote(noteId);
    };
    pianoRoll.onNoteAliasCommitted = [this](const juce::String& noteId)
    {
        prepareUtauTrackForNote(noteId);
    };
    trackList.peakProvider = [this](const juce::String& trackId) { return audio.trackPeak(trackId); };
    trackList.onTrackSelected = [this](const juce::String& trackId)
    {
        selectedTrackId = trackId;
        pianoRoll.setFocusedTrack(trackId);
        refreshProjectControls();
    };

    openButton.onClick = [this] { openProject(); };
    saveButton.onClick = [this] { saveProject(); };
    audioButton.onClick = [this] { importAudio(); };
    melodyneButton.onClick = [this] { importMelodyne(); };
    collapseTracksButton.onClick = [this] { setTracksCollapsed(!tracksCollapsed); };
    playButton.onClick = [this] { togglePlayback(); };
    stopButton.onClick = [this]
    {
        playWhenRenderReady = false;
        audio.stop();
        audio.setPosition(0.0);
    };
    noteEditButton.onClick = [this]
    {
        closeEnvelopeLanes();
        setSourceEditMode(false);
        pianoRoll.setTool(PianoRollComponent::Tool::note);
        setToolButton(noteEditButton);
    };
    wrenchButton.onClick = [this]
    {
        closeEnvelopeLanes();
        setSourceEditMode(true);
        pianoRoll.setTool(PianoRollComponent::Tool::note);
        setToolButton(wrenchButton);
    };
    drawButton.onSecondaryClick = [this](juce::Point<int> at)
    { showDrawSettingsMenu(at); };
    drawButton.onClick = [this]
    {
        closeEnvelopeLanes();
        setSourceEditMode(false);
        pianoRoll.setTool(PianoRollComponent::Tool::draw);
        setToolButton(drawButton);
    };
    lineButton.onClick = [this]
    {
        closeEnvelopeLanes();
        setSourceEditMode(false);
        pianoRoll.setTool(PianoRollComponent::Tool::line);
        setToolButton(lineButton);
    };
    pointButton.onClick = [this]
    {
        closeEnvelopeLanes();
        setSourceEditMode(false);
        pianoRoll.setTool(PianoRollComponent::Tool::points);
        setToolButton(pointButton);
    };
    connectButton.onClick = [this]
    {
        closeEnvelopeLanes();
        setSourceEditMode(false);
        pianoRoll.setTool(PianoRollComponent::Tool::connect);
        setToolButton(connectButton);
    };
    for (auto* button : { &pitchParamButton, &driftParamButton, &attackParamButton,
                          &breathParamButton, &tensionParamButton,
                          &formantParamButton, &volumeParamButton })
        button->onClick = [this, button]
        {
            if (button == &volumeParamButton && isUtauAlgorithmSelected())
            {
                setEnvelopeLane(nextEnvelopeLane(envelopeLane, EnvelopeLane::amplitude));
                return;
            }
            // Any other parameter lane closes both envelope lanes with it.
            closeEnvelopeLanes();
            setToolButton(*button);
            parameterMode = button == &driftParamButton ? ParameterMode::pitchDrift
                : button == &attackParamButton ? ParameterMode::attackSpeed
                : button == &breathParamButton ? ParameterMode::breath
                : button == &tensionParamButton ? ParameterMode::tension
                : button == &formantParamButton ? ParameterMode::formant
                : button == &volumeParamButton ? ParameterMode::volume
                : ParameterMode::pitchSmooth;
            refreshSelectedNoteParameter();
        };
    noteEditButton.setClickingTogglesState(false);
    wrenchButton.setClickingTogglesState(false);

    project.addChangeListener(this);
    audio.addChangeListener(this);
    refreshTexts();
    refreshProjectControls();
    setSourceEditMode(false);
    setToolButton(pitchParamButton);
    refreshSelectedNoteParameter();
    setSize(1280, 760);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    if (preferences != nullptr) audio.saveDeviceState(*preferences);
    panelSplitter.removeMouseListener(this);
    audio.removeChangeListener(this);
    project.removeChangeListener(this);
    menuBar.setModel(nullptr);
    setLookAndFeel(nullptr);
}

void MainComponent::applyPreferences()
{
    if (preferences == nullptr) return;
    strings.setLanguage(static_cast<I18n::Language>(juce::jlimit(1, 5,
        preferences->getIntValue("ui.language", static_cast<int>(strings.getLanguage()) + 1)) - 1));
    const auto parseColour = [](juce::String value, juce::Colour fallback)
    {
        value = value.trim().removeCharacters("#");
        if (value.length() != 6 && value.length() != 8) return fallback;
        if (value.length() == 6) value = "ff" + value;
        return juce::Colour::fromString(value);
    };
    Palette::applyTheme(preferences->getValue("ui.theme", "dark"),
        parseColour(preferences->getValue("ui.accent", "7F69CA"), juce::Colour(0xff7f69ca)),
        parseColour(preferences->getValue("ui.accentLight", "CBCBFA"), juce::Colour(0xffcbcbfa)),
        parseColour(preferences->getValue("ui.noteColour", "F4C000"), juce::Colour(0xfff4c000)));
    audio.setHifiganModelDirectory(juce::File(
        preferences->getValue("algorithm.hifiganPath")));
    // A missing or empty setting resolves to the bundled engine inside
    // AudioEngine, which is also where the headless path picks it up.
    audio.setUtauResamplerFile(juce::File(
        preferences->getValue("algorithm.utauResampler").trim().unquoted()));
    const auto tracks = project.snapshot().tracks;
    for (const auto& track : tracks)
        if (track.pitchAlgorithm == PitchAlgorithm::utau
            && !track.voicebankDirectory.isDirectory())
            bindDefaultUtauVoicebank(track.id);
    const auto analysisConfig = backend::AnalysisService::configFromProperties(preferences.get());
    audio.setInferenceConfiguration(analysisConfig.inference, analysisConfig.deviceIndex);
    pianoRoll.setShowNoteLabels(preferences->getBoolValue("ui.showNoteLabels", false));
    showWaveforms = preferences->getBoolValue("ui.showWaveforms", true);
    pianoRoll.setShowWaveforms(showWaveforms);
    pianoRoll.setDrawLengthDivision(
        preferences->getIntValue("ui.drawLengthDivision", 64));
    viewOptions = viewOptionsFrom(*preferences);
    applyViewOptions();
    setTracksCollapsed(preferences->getBoolValue("ui.tracksCollapsed", false));
    const auto vZoom = juce::jlimit(0.5, 2.0, preferences->getDoubleValue("ui.vZoom", 1.0));
    vZoomSlider.setValue(vZoom, juce::dontSendNotification);
    pianoRoll.setRowHeight(22.0f * static_cast<float>(vZoom));
    const auto row = 96.0f * static_cast<float>(vZoom);
    timeline.setRowHeight(row);
    trackList.setRowHeight(row);
    // Applying a new model path must invalidate and immediately reschedule
    // already imported compose clips; waiting for a later edit made the
    // settings change appear ineffective.
    syncAudio(project.snapshot());
    lookAndFeel.refreshColours();
    applyUiScale();
}

void MainComponent::applyUiScale()
{
    static const float systemScale = juce::Desktop::getInstance().getGlobalScaleFactor();
    const auto uiScale = static_cast<float>(juce::jlimit(0.6, 2.0,
        preferences != nullptr ? preferences->getDoubleValue("ui.uiScale", 1.0) : 1.0));
    juce::Desktop::getInstance().setGlobalScaleFactor(systemScale * uiScale);
}

void MainComponent::refreshTexts()
{
    openButton.setButtonText({});
    openButton.setTooltip(strings.text("file.open"));
    saveButton.setButtonText({});
    saveButton.setTooltip(strings.text("file.save"));
    audioButton.setButtonText({});
    audioButton.setTooltip(strings.text("file.audio"));
    melodyneButton.setButtonText({});
    melodyneButton.setTooltip(strings.text("file.melodyne"));
    collapseTracksButton.setTooltip(utf8(
        "折叠上方的音轨与编排，把整个窗口留给调音；再次点击展开"));
    horizontalZoomOutButton.setButtonText("-");
    horizontalZoomOutButton.setTooltip(strings.text("view.zoomOut"));
    horizontalZoomInButton.setButtonText("+");
    horizontalZoomInButton.setTooltip(strings.text("view.zoomIn"));
    verticalZoomOutButton.setButtonText("-");
    verticalZoomOutButton.setTooltip(strings.text("view.vZoomOut"));
    verticalZoomInButton.setButtonText("+");
    verticalZoomInButton.setTooltip(strings.text("view.vZoomIn"));
    playButton.setButtonText({});
    playButton.setTooltip(strings.text("transport.play"));
    stopButton.setButtonText({});
    stopButton.setTooltip(strings.text("transport.stop"));
    noteEditButton.setButtonText({});
    noteEditButton.setTooltip(strings.text("tool.main"));
    wrenchButton.setButtonText({});
    wrenchButton.setTooltip(strings.text("tool.wrench"));
    drawButton.setButtonText({});
    drawButton.setTooltip(strings.text("tool.draw"));
    lineButton.setButtonText({});
    lineButton.setTooltip(strings.text("tool.line"));
    pointButton.setButtonText({});
    pointButton.setTooltip(strings.text("tool.points"));
    connectButton.setButtonText({});
    connectButton.setTooltip(strings.text("tool.connect"));
    parameterTitle.setText(strings.text("editor.parameters"), juce::dontSendNotification);
    smoothCaption.setText(strings.text("editor.smooth"), juce::dontSendNotification);
    pitchParamButton.setButtonText(strings.text("param.pitch"));
    driftParamButton.setButtonText(strings.text("param.drift"));
    attackParamButton.setButtonText(strings.text("param.attack"));
    breathParamButton.setButtonText(strings.text("param.breath"));
    tensionParamButton.setButtonText(strings.text("param.tension"));
    formantParamButton.setButtonText(strings.text("param.formant"));
    volumeParamButton.setButtonText(strings.text("param.volume"));
    volumeParamButton.setTooltip(utf8("编辑 UTAU 音符在重采样后的振幅包络"));
    robustPitchCurveButton.setButtonText(strings.text("param.robustPitchCurveShort"));
    robustPitchCurveButton.setTooltip(strings.text("param.robustPitchCurve"));
    bpmCaption.setText("BPM", juce::dontSendNotification);
    beatsCaption.setText(strings.text("beats.bar"), juce::dontSendNotification);
    denominatorLabel.setText("/ 4", juce::dontSendNotification);
    gridCaption.setText(strings.text("grid"), juce::dontSendNotification);
    stretchCaption.setText(strings.text("stretch.unit"), juce::dontSendNotification);
    stretchSelector.setTooltip(strings.text("stretch.unitHelp"));
    scaleCaption.setText(strings.text("base.scale"), juce::dontSendNotification);
    pitchLabel.setText(strings.text("algo.pitch"), juce::dontSendNotification);
    stretchLabel.setText(strings.text("algo.stretch"), juce::dontSendNotification);
    pitchAlgorithm.setTooltip(strings.text("algo.pitch"));
    stretchAlgorithm.setTooltip(strings.text("algo.stretch"));
    refreshStretchAlgorithmItems(stretchAlgorithm.getSelectedId());
    renderOrderLabel.setText(strings.text("algo.order"), juce::dontSendNotification);
    renderOrder.setTooltip(strings.text("algo.order"));
    {
        const auto previousOrder = renderOrder.getSelectedId();
        renderOrder.clear(juce::dontSendNotification);
        renderOrder.addItem(strings.text("algo.order.processThenSplice"), 1);
        renderOrder.addItem(strings.text("algo.order.stretchSpliceThenPitch"), 2);
        renderOrder.setSelectedId(previousOrder > 0 ? previousOrder : 1,
                                  juce::dontSendNotification);
    }
    statusLabel.setText(strings.text("status.ready"), juce::dontSendNotification);
    sourceEditHint.setText(strings.text("edit.source"), juce::dontSendNotification);
    sampleAliasLabel.setText(strings.text("sample.alias"), juce::dontSendNotification);
    sampleStartLabel.setText(strings.text("sample.start"), juce::dontSendNotification);
    sampleEndLabel.setText(strings.text("sample.end"), juce::dontSendNotification);
    sampleAlignmentLabel.setText(strings.text("sample.alignment"), juce::dontSendNotification);
    sampleFixedLabel.setText(strings.text("sample.fixed"), juce::dontSendNotification);
    sampleSaveButton.setButtonText(strings.text("sample.save"));
    otoImportButton.setButtonText(strings.text("sample.importOto"));
    otoExportButton.setButtonText(strings.text("sample.exportOto"));
    utauVoicebankLabel.setText(utf8("UTAU 音源库"), juce::dontSendNotification);
    utauVoicebankButton.setButtonText(utf8("选择音源库…"));
    // A dropdown rather than dialog settings: these are reached while tuning,
    // so they stay in the tool row where a click finds them, but folded into
    // one button because the row is the scarcest space in the window.
    showViewMenuButton.setButtonText(utf8("显示"));
    showViewMenuButton.setTooltip(utf8(
        "范围：每个音符实际发声范围的橙色框\n"
        "包络：在音符上画出它自己的振幅包络"));
    // Opens on press rather than release, the way a dropdown does.
    showViewMenuButton.setTriggeredOnMouseDown(true);
    showViewMenuButton.onClick = [this] { showViewMenu(); };
    // Attack, release and the gain the hold ends on.  These are the four
    // commonest Envelope fields across real USTs rather than round numbers:
    // 0,5,35 is UTAU's own default, then a decaying hold, a softer attack and
    // a short tail.
    struct EnvelopePreset { const char* text; const char* tip;
                            double attack; double release; float plateauEnd; };
    static const std::array<EnvelopePreset, 4> presets {{
        { "标准", "起音 5 ms，释放 35 ms（UTAU 默认）", 0.005, 0.035, 0.0f },
        { "渐弱", "起音 5 ms，释放 35 ms，平台末尾降到 90%", 0.005, 0.035, -0.92f },
        { "柔起", "起音 15 ms，释放 35 ms", 0.015, 0.035, 0.0f },
        { "短收", "起音 5 ms，释放 5 ms", 0.005, 0.005, 0.0f }
    }};
    envelopePresetCaption.setText(utf8("包络预设"), juce::dontSendNotification);
    envelopePresetCaption.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(envelopePresetCaption);
    for (std::size_t index = 0; index < presets.size(); ++index)
    {
        const auto& preset = presets[index];
        auto& button = envelopePresetButtons[index];
        button.configure(utf8(preset.text), preset.attack, preset.release,
                         preset.plateauEnd);
        button.setTooltip(utf8(preset.tip));
        button.onClick = [this, preset]
        {
            const auto count = pianoRoll.applyEnvelopePreset(
                preset.attack, preset.release, preset.plateauEnd);
            if (count > 0)
                statusLabel.setText(utf8("包络预设：") + juce::String(count)
                    + utf8(" 个音符"), juce::dontSendNotification);
            else
                showError(utf8("先选中要套用预设的音符。"));
        };
        addAndMakeVisible(button);
    }
    spliceButton.setButtonText(utf8("拼接"));
    spliceButton.setTooltip(utf8("把选中的相邻音符在交界处交叉淡入淡出；"
                                  "需要选中两个以上相邻的音符"));
    spliceButton.onClick = [this] { spliceSelectedNotes(); };
    spliceButton.setEnabled(false);
    flagCurveButton.setButtonText(utf8("线性flag"));
    flagCurveButton.setTooltip(utf8("让选中音符的 g 随时间连续变化，改由 flag 包络里的曲线决定；"
                                     "Flags 里写的 g 在开启期间不再起作用，其它 flag 照常"));
    flagCurveButton.setClickingTogglesState(true);
    flagCurveButton.setEnabled(false);
    flagCurveButton.onClick = [this]
    {
        const auto noteIds = pianoRoll.selectedNoteIds();
        if (noteIds.empty()) return;
        const auto enable = flagCurveButton.getToggleState();
        project.setNotesUtauFlagCurveEnabled(noteIds, enable);
        statusLabel.setText((enable ? utf8("线性 flag：") : utf8("取消线性 flag："))
            + juce::String(static_cast<int>(noteIds.size())) + utf8(" 个音符"),
            juce::dontSendNotification);
        // Switching a note back to a plain flag leaves the lane with nothing
        // to draw, so it steps back to the note tool rather than sitting empty.
        if (!enable && pianoRoll.currentTool() == PianoRollComponent::Tool::flagCurve)
        {
            pianoRoll.setTool(PianoRollComponent::Tool::note);
            setToolButton(noteEditButton);
        }
        refreshSelectedNoteParameter();
    };
    flagEnvelopeButton.setButtonText(utf8("flag 包络"));
    flagEnvelopeButton.setTooltip(utf8("在底部通道里画 g 的渐变曲线；先对该音符开启线性 flag"));
    flagEnvelopeButton.setEnabled(false);
    flagEnvelopeButton.onClick = [this]
    {
        setEnvelopeLane(nextEnvelopeLane(envelopeLane, EnvelopeLane::flagCurve));
    };
    voicebankSettingsButton.setButtonText(utf8("音源库设置"));
    voicebankSettingsButton.setTooltip(utf8("读取并查看当前 UTAU 音源库中的 oto.ini"));
    noteAliasLabel.setText(utf8("音符发音/别名"), juce::dontSendNotification);
    noteConsonantVelocityLabel.setText(utf8("辅音速度"), juce::dontSendNotification);
    const auto noteVelocityHelp = utf8("显示轨道的全局辅音速度；修改后保存为所选音符的单独辅音速度（支持任意正负整数），合成时单音值优先");
    noteConsonantVelocityLabel.setTooltip(noteVelocityHelp);
    noteConsonantVelocityEditor.setTooltip(noteVelocityHelp);
    noteConsonantVelocityEditor.setTextToShowWhenEmpty({}, Palette::textMuted);
    noteFlagsLabel.setText("Flags", juce::dontSendNotification);
    noteFlagsEditor.setTextToShowWhenEmpty({}, Palette::textMuted);
    refreshSelectedNoteParameter();
}

void MainComponent::refreshProjectControls()
{
    // Which track is in hand decides whether a material track is heard, and
    // the engine only re-reads that while syncing.
    if (auditionTrackAtLastSync != selectedTrackId)
    {
        auditionTrackAtLastSync = selectedTrackId;
        syncAudio(project.snapshot());
    }
    const auto data = project.snapshot();
    bpmEditor.setText(juce::String(data.bpm, std::abs(data.bpm - std::floor(data.bpm)) < 1.0e-9 ? 0 : 2),
                      juce::dontSendNotification);
    beatsEditor.setText(juce::String(data.numerator), juce::dontSendNotification);
    gridSelector.setText(data.gridDivision, juce::dontSendNotification);
    stretchSelector.setSelectedId(juce::jlimit(2, 128, data.noteEditDivision),
                                  juce::dontSendNotification);
    scaleSelector.setText(data.baseScale, juce::dontSendNotification);
    auto selected = selectedTrackId.isNotEmpty()
        ? std::find_if(data.tracks.begin(), data.tracks.end(),
            [this](const auto& track) { return track.id == selectedTrackId; })
        : data.tracks.end();
    if (selected == data.tracks.end())
        selected = std::find_if(data.tracks.begin(), data.tracks.end(),
                                [](const auto& track) { return track.compose; });
    if (selected != data.tracks.end())
    {
        const auto pitchId = selected->pitchAlgorithm == PitchAlgorithm::nsfHifigan ? 2
            : selected->pitchAlgorithm == PitchAlgorithm::world ? 3
            : selected->pitchAlgorithm == PitchAlgorithm::vocalShifter ? 4
            : selected->pitchAlgorithm == PitchAlgorithm::mld3 ? 5
            : selected->pitchAlgorithm == PitchAlgorithm::llsm2 ? 6
            : selected->pitchAlgorithm == PitchAlgorithm::utau
                ? utauModePickerItem(selected->utauMode) : 1;
        const auto stretchId = selected->stretchAlgorithm == StretchAlgorithm::variableMelHop ? 2
            : selected->stretchAlgorithm == StretchAlgorithm::loop ? 3
            : selected->stretchAlgorithm == StretchAlgorithm::soundTouch ? 4
            : selected->stretchAlgorithm == StretchAlgorithm::nsfShiftThenSplice ? 5 : 1;
        pitchAlgorithm.setSelectedId(pitchAlgorithm.indexOfItemId(pitchId) >= 0 ? pitchId : 1,
                                     juce::dontSendNotification);
        const auto utauItem = utauModeForPickerItem(pitchId).has_value();
        if (!utauItem && utauAmplitudeEnvelopeActive)
        {
            closeEnvelopeLanes();
            pianoRoll.setTool(PianoRollComponent::Tool::note);
            setToolButton(noteEditButton);
        }
        else if (utauItem && volumeParamButton.getToggleState()
                 && !utauAmplitudeEnvelopeActive)
        {
            pianoRoll.setTool(PianoRollComponent::Tool::note);
            setToolButton(noteEditButton);
        }
        refreshStretchAlgorithmItems(stretchId);
        renderOrder.setSelectedId(
            selected->renderOrder == RenderOrder::stretchSpliceThenPitch ? 2 : 1,
            juce::dontSendNotification);
        utauVoicebankPath.setText(selected->voicebankDirectory.isDirectory()
            ? selected->voicebankDirectory.getFullPathName()
            : utf8("尚未选择音源库"), juce::dontSendNotification);
        utauVoicebankPath.setTooltip(utauVoicebankPath.getText());
    }
    refreshSelectedNoteParameter();
    refreshSpliceButton();
    resized();
}

bool MainComponent::isUtauAlgorithmSelected() const
{
    // Every UTAU mode, not just the two that existed first: the whole UTAU
    // toolbar hangs off this, so a mode left out of it comes up wearing the
    // generic parameter set instead.
    return utauModeForPickerItem(pitchAlgorithm.getSelectedId()).has_value();
}

void MainComponent::refreshSelectedNoteParameter()
{
    NoteData selected;
    auto found = false;
    const auto data = project.snapshot();
    const auto selectedIds = pianoRoll.selectedNoteIds();
    std::vector<NoteData> selectedNotes;
    std::vector<int> selectedNoteConsonantVelocities;
    selectedNotes.reserve(selectedIds.size());
    selectedNoteConsonantVelocities.reserve(selectedIds.size());
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (std::find(selectedIds.begin(), selectedIds.end(), note.id)
                    != selectedIds.end())
                {
                    selectedNotes.push_back(note);
                    selectedNoteConsonantVelocities.push_back(
                        note.utauConsonantVelocity != inheritedUtauConsonantVelocity
                            ? note.utauConsonantVelocity
                            : track.utauConsonantVelocity);
                }
                if (note.id == selectedNoteId)
                {
                    selected = note;
                    found = true;
                }
            }
    if (!found && !selectedNotes.empty())
    {
        selected = selectedNotes.front();
        found = true;
    }
    updatingSmoothSlider = true;
    double value = 0.0;
    if (parameterMode == ParameterMode::pitchSmooth)
    {
        smoothCaption.setText(strings.text("editor.smooth"), juce::dontSendNotification);
        smoothSlider.setRange(0.0, 100.0, 1.0);
        smoothSlider.setTextValueSuffix("%");
        value = (1.0 - static_cast<double>(selected.modulation)) * 100.0;
    }
    else if (parameterMode == ParameterMode::pitchDrift)
    {
        smoothCaption.setText(strings.text("editor.drift"), juce::dontSendNotification);
        // Melodyne stores the remaining drift factor.  Present the familiar
        // correction amount: 100% removes drift, 0% preserves it, while a
        // negative value retains imported emphasis factors above 1.0.
        smoothSlider.setRange(-100.0, 100.0, 1.0);
        smoothSlider.setTextValueSuffix("%");
        value = (1.0 - static_cast<double>(selected.drift)) * 100.0;
    }
    else if (parameterMode == ParameterMode::attackSpeed)
    {
        smoothCaption.setText(strings.text("editor.attackSpeed"), juce::dontSendNotification);
        smoothSlider.setRange(5.0, 2000.0, 1.0);
        smoothSlider.setTextValueSuffix("%");
        value = static_cast<double>(selected.attackSpeed) * 100.0;
    }
    else if (parameterMode == ParameterMode::breath)
    {
        smoothCaption.setText(strings.text("param.breath"), juce::dontSendNotification);
        smoothSlider.setRange(0.0, 100.0, 1.0);
        smoothSlider.setTextValueSuffix("%");
        value = static_cast<double>(selected.breath) * 100.0;
    }
    else if (parameterMode == ParameterMode::tension)
    {
        smoothCaption.setText(strings.text("param.tension"), juce::dontSendNotification);
        smoothSlider.setRange(-100.0, 100.0, 1.0);
        smoothSlider.setTextValueSuffix("%");
        value = static_cast<double>(selected.tension) * 100.0;
    }
    else if (parameterMode == ParameterMode::formant)
    {
        smoothCaption.setText(strings.text("param.formant"), juce::dontSendNotification);
        smoothSlider.setRange(-12.0, 12.0, 0.1);
        smoothSlider.setTextValueSuffix(" st");
        value = selected.formantSemitones;
    }
    else
    {
        smoothCaption.setText(strings.text("param.volume"), juce::dontSendNotification);
        smoothSlider.setRange(-60.0, 12.0, 0.1);
        smoothSlider.setTextValueSuffix(" dB");
        value = selected.gain > 1.0e-6f ? 20.0 * std::log10(selected.gain) : -60.0;
    }
    smoothSlider.setValue(value, juce::dontSendNotification);
    updatingSmoothSlider = false;
    smoothSlider.setEnabled(found);
    const auto multipleNotes = selectedNotes.size() > 1;
    noteAliasEditor.setEnabled(found && !multipleNotes);
    noteConsonantVelocityEditor.setEnabled(!selectedNotes.empty());
    noteFlagsEditor.setEnabled(!selectedNotes.empty());
    // The switch follows the notes, and the lane opens only for notes that
    // have a curve to show.
    const auto anyFlagCurve = std::any_of(selectedNotes.begin(), selectedNotes.end(),
        [](const auto& note) { return note.utauFlagCurveEnabled; });
    const auto allFlagCurve = !selectedNotes.empty()
        && std::all_of(selectedNotes.begin(), selectedNotes.end(),
            [](const auto& note) { return note.utauFlagCurveEnabled; });
    flagCurveButton.setEnabled(!selectedNotes.empty());
    flagCurveButton.setToggleState(allFlagCurve, juce::dontSendNotification);
    flagEnvelopeButton.setEnabled(anyFlagCurve);
    if (!noteAliasEditor.hasKeyboardFocus(false))
        noteAliasEditor.setText(multipleNotes ? juce::String::fromUTF8("×")
            : found ? selected.label : juce::String{}, false);
    if (!noteFlagsEditor.hasKeyboardFocus(false))
    {
        noteFlagsMixed = false;
        juce::String displayedFlags;
        if (!selectedNotes.empty())
        {
            displayedFlags = selectedNotes.front().utauFlags;
            noteFlagsMixed = std::any_of(std::next(selectedNotes.begin()), selectedNotes.end(),
                [&displayedFlags](const auto& note)
                {
                    return note.utauFlags != displayedFlags;
                });
            if (noteFlagsMixed) displayedFlags = "-";
        }
        noteFlagsDirty = false;
        noteFlagsEditor.setText(displayedFlags, false);
    }
    if (!noteConsonantVelocityEditor.hasKeyboardFocus(false))
    {
        noteConsonantVelocityMixed = false;
        juce::String displayedVelocity;
        if (!selectedNoteConsonantVelocities.empty())
        {
            const auto velocity = selectedNoteConsonantVelocities.front();
            noteConsonantVelocityMixed = std::any_of(
                std::next(selectedNoteConsonantVelocities.begin()),
                selectedNoteConsonantVelocities.end(),
                [velocity](const auto value)
                {
                    return value != velocity;
                });
            if (noteConsonantVelocityMixed)
                displayedVelocity = "-";
            else
                displayedVelocity = juce::String(velocity);
        }
        noteConsonantVelocityDirty = false;
        noteConsonantVelocityEditor.setText(displayedVelocity, false);
    }
    updatingRobustPitchCurve = true;
    robustPitchCurveButton.setToggleState(found && selected.robustPitchCurve,
                                           juce::dontSendNotification);
    updatingRobustPitchCurve = false;
    const auto showRobust = pitchAlgorithm.getSelectedId() == 1;
    const auto visibilityChanged = robustPitchCurveButton.isVisible() != showRobust;
    robustPitchCurveButton.setVisible(showRobust);
    robustPitchCurveButton.setEnabled(found && showRobust);
    if (visibilityChanged) resized();
}

void MainComponent::applySelectedNoteParameter()
{
    const auto value = smoothSlider.getValue();
    if (parameterMode == ParameterMode::pitchSmooth)
        project.setNoteModulation(selectedNoteId,
            1.0f - static_cast<float>(value / 100.0));
    else if (parameterMode == ParameterMode::pitchDrift)
        project.setNoteDrift(selectedNoteId,
            1.0f - static_cast<float>(value / 100.0));
    else if (parameterMode == ParameterMode::attackSpeed)
        project.setNoteAttackSpeed(selectedNoteId,
            static_cast<float>(value / 100.0));
    else if (parameterMode == ParameterMode::breath)
        project.setNoteBreath(selectedNoteId, static_cast<float>(value / 100.0));
    else if (parameterMode == ParameterMode::tension)
        project.setNoteTension(selectedNoteId, static_cast<float>(value / 100.0));
    else if (parameterMode == ParameterMode::formant)
        project.setNoteFormant(selectedNoteId, static_cast<float>(value));
    else
        project.setNoteGain(selectedNoteId, value <= -59.9 ? 0.0f
            : static_cast<float>(std::pow(10.0, value / 20.0)));
}

void MainComponent::commitNoteAlias()
{
    if (selectedNoteId.isEmpty() || !noteAliasEditor.isEnabled()) return;
    project.setNoteLabel(selectedNoteId, noteAliasEditor.getText());
    prepareUtauTrackForNote(selectedNoteId);
    // After the track has a voicebank, or the note would have no span to take
    // its shape from.
    pianoRoll.ensureDefaultEnvelope(selectedNoteId);
}

void MainComponent::commitNoteFlags()
{
    if (!noteFlagsEditor.isEnabled()) return;
    const auto noteIds = pianoRoll.selectedNoteIds();
    if (noteIds.empty()) return;
    // Merely focusing and leaving a mixed-value field must not replace every
    // note's Flags with the visual "-" marker.
    if (noteFlagsMixed && !noteFlagsDirty) return;
    project.setNotesUtauFlags(noteIds, noteFlagsEditor.getText());
    noteFlagsMixed = false;
    noteFlagsDirty = false;
}

void MainComponent::commitNoteConsonantVelocity()
{
    if (!noteConsonantVelocityEditor.isEnabled()) return;
    const auto noteIds = pianoRoll.selectedNoteIds();
    if (noteIds.empty()) return;
    // Do not treat the mixed-value marker as an edited value merely because
    // the editor received or lost keyboard focus.
    if (noteConsonantVelocityMixed && !noteConsonantVelocityDirty) return;
    const auto text = noteConsonantVelocityEditor.getText().trim();
    // A lone minus sign is an in-progress negative number (or the mixed-value
    // marker), never a request to overwrite the selected notes with zero.
    if (text == "-") return;
    const auto velocity = text.isEmpty()
        ? inheritedUtauConsonantVelocity : text.getIntValue();
    project.setNotesUtauConsonantVelocity(noteIds, velocity);
    if (velocity != inheritedUtauConsonantVelocity
        && text != juce::String(velocity))
        noteConsonantVelocityEditor.setText(juce::String(velocity), false);
    noteConsonantVelocityMixed = false;
    noteConsonantVelocityDirty = false;
}

void MainComponent::chooseUtauVoicebank()
{
    const auto data = project.snapshot();
    const auto selected = std::find_if(data.tracks.begin(), data.tracks.end(),
        [this](const auto& track) { return track.id == selectedTrackId; });
    if (selected == data.tracks.end()) return;
    auto initial = selected->voicebankDirectory.isDirectory()
        ? selected->voicebankDirectory : juce::File{};
    if (!initial.isDirectory() && preferences != nullptr)
    {
        const juce::File defaultDirectory(
            preferences->getValue("algorithm.utauVoicebank"));
        if (defaultDirectory.isDirectory()) initial = defaultDirectory;
    }
    chooser = std::make_unique<juce::FileChooser>(utf8("选择 UTAU 音源库目录"), initial);
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectDirectories,
        [this, trackId = selected->id](const juce::FileChooser& selectedChooser)
        {
            const auto directory = selectedChooser.getResult();
            if (!directory.isDirectory()) return;
            juce::StringArray imported, warnings;
            int sidecars = 0, regions = 0;
            SampleSettings::importVoicebank(directory, imported, sidecars, regions, warnings);
            project.setTrackVoicebankDirectory(trackId, directory);
            if (preferences != nullptr)
            {
                preferences->setValue("algorithm.utauVoicebank", directory.getFullPathName());
                preferences->saveIfNeeded();
            }
            statusLabel.setText(utf8("UTAU 音源库：") + juce::String(imported.size())
                + utf8(" 个采样，") + juce::String(regions) + utf8(" 个别名")
                + (warnings.isEmpty() ? juce::String{} : utf8("（部分条目有警告）")),
                juce::dontSendNotification);
        });
}

void MainComponent::refreshSpliceButton()
{
    // One note has no boundary to splice, so the action stays out of reach
    // until at least two are selected.
    spliceButton.setEnabled(isUtauAlgorithmSelected()
        && pianoRoll.selectedNoteIds().size() >= 2);
}

void MainComponent::spliceSelectedNotes()
{
    const auto selected = pianoRoll.selectedNoteIds();
    if (selected.size() < 2) return;
    const auto data = project.snapshot();

    struct Placed { juce::String id; double start = 0.0; double end = 0.0; bool spliced = false; };
    std::vector<Placed> notes;
    for (const auto& track : data.tracks)
    {
        if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                if (std::find(selected.begin(), selected.end(), note.id) == selected.end())
                    continue;
                const auto start = clip.startSeconds + note.startSeconds;
                notes.push_back({ note.id, start, start + note.durationSeconds,
                                  note.utauSplice });
            }
    }
    std::stable_sort(notes.begin(), notes.end(), [](const auto& left, const auto& right)
    {
        return left.start < right.start;
    });

    // The flag belongs to the later note of each pair: it says "fade into what
    // came before me".  Only a shared boundary can be spliced.
    std::vector<juce::String> boundaries;
    auto allSpliced = true;
    for (std::size_t index = 1; index < notes.size(); ++index)
    {
        if (std::abs(notes[index - 1].end - notes[index].start) > 0.002) continue;
        boundaries.push_back(notes[index].id);
        allSpliced = allSpliced && notes[index].spliced;
    }
    if (boundaries.empty())
    {
        showError(utf8("选中的音符之间没有相邻的交界。"));
        return;
    }
    // Pressing it again on an already spliced run takes the splice off, which
    // is the only way back other than undo.
    const auto enable = !allSpliced;
    project.setNotesUtauSplice(boundaries, enable);
    statusLabel.setText((enable ? utf8("拼接：") : utf8("取消拼接："))
        + juce::String(static_cast<int>(boundaries.size())) + utf8(" 处交界"),
        juce::dontSendNotification);
}

void MainComponent::showVoicebankSettings()
{
    const auto data = project.snapshot();
    const auto selected = std::find_if(data.tracks.begin(), data.tracks.end(),
        [this](const auto& track) { return track.id == selectedTrackId; });
    if (selected == data.tracks.end()
        || selected->pitchAlgorithm != PitchAlgorithm::utau)
        return;
    if (!selected->voicebankDirectory.isDirectory())
    {
        showError(utf8("请先为当前轨道选择 UTAU 音源库。"));
        return;
    }

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = utf8("音源库设置 — ") + selected->voicebankDirectory.getFileName();
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    // Open on the entry the selected note uses, so the alias does not have to
    // be hunted down in a large voicebank.
    juce::String noteAlias;
    if (selectedNoteId.isNotEmpty())
        for (const auto& track : data.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (note.id == selectedNoteId) noteAlias = note.label;
    options.content.setOwned(new VoicebankSettingsComponent(
        selected->voicebankDirectory, utauModeUsesRegions(selected->utauMode),
        selected->utauMode == UtauMode::mou, noteAlias));
    if (auto* window = options.launchAsync())
        window->setResizeLimits(780, 440, 1800, 1200);
}


void MainComponent::showRegionEditorForNote(const juce::String& noteId)
{
    const auto wanted = noteId.isNotEmpty() ? noteId : selectedNoteId;
    if (wanted.isEmpty()) return;
    const auto data = project.snapshot();
    const TrackData* owner = nullptr;
    juce::String alias;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (note.id == wanted) { owner = &track; alias = note.label; }
    if (owner == nullptr) return;
    if (owner->pitchAlgorithm != PitchAlgorithm::utau)
    {
        showError(utf8("区域编辑器仅用于 UTAU 轨道。"));
        return;
    }
    if (!owner->voicebankDirectory.isDirectory())
    {
        showError(utf8("请先为当前轨道选择 UTAU 音源库。"));
        return;
    }

    juce::StringArray warnings;
    const auto mouMode = owner->utauMode == UtauMode::mou;
    const auto entries = SampleSettings::loadVoicebankOto(
        owner->voicebankDirectory, warnings, utauModeUsesRegions(owner->utauMode),
        mouMode);
    const auto index = SampleSettings::findEntryForAlias(entries, alias);
    if (index < 0)
    {
        showError(utf8("音源库里没有可编辑的 oto 条目。"));
        return;
    }
    const auto entry = entries[static_cast<std::size_t>(index)];

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = (mouMode ? utf8("谋•OTO 分区编辑器 — ")
                           : utauModeUsesRegions(owner->utauMode)
                               ? utf8("界•OTO 四区编辑器 — ")
                               : utf8("oto 时序编辑器 — "))
        + entry.sourceName;
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.content.setOwned(new OtoWaveformEditorComponent(
        entry, utauModeUsesRegions(owner->utauMode), mouMode, [] {}));
    if (auto* window = options.launchAsync())
        window->setResizeLimits(720, 400, 1800, 1100);
}

bool MainComponent::bindDefaultUtauVoicebank(const juce::String& trackId)
{
    if (preferences == nullptr || trackId.isEmpty()) return false;
    const juce::File directory(preferences->getValue("algorithm.utauVoicebank"));
    if (!directory.isDirectory()) return false;

    juce::StringArray imported, warnings;
    int sidecars = 0, regions = 0;
    SampleSettings::importVoicebank(directory, imported, sidecars, regions, warnings);
    project.setTrackVoicebankDirectory(trackId, directory);
    statusLabel.setText(utf8("UTAU 默认音源库：") + directory.getFileName()
        + (warnings.isEmpty() ? juce::String{} : utf8("（部分条目有警告）")),
        juce::dontSendNotification);
    return true;
}

void MainComponent::prepareUtauTrackForNote(const juce::String& noteId)
{
    const auto data = project.snapshot();
    const TrackData* owner = nullptr;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (std::any_of(clip.notes.begin(), clip.notes.end(),
                [&noteId](const auto& note) { return note.id == noteId; }))
            {
                owner = &track;
                break;
            }
    if (owner == nullptr) return;

    selectedTrackId = owner->id;
    trackList.setSelectedTrack(owner->id);
    pianoRoll.setFocusedTrack(owner->id);
    if (owner->pitchAlgorithm != PitchAlgorithm::utau)
        project.setTrackPitchAlgorithm(owner->id, PitchAlgorithm::utau);

    const auto hasVoicebank = owner->voicebankDirectory.isDirectory()
        || bindDefaultUtauVoicebank(owner->id);
    if (!hasVoicebank)
        statusLabel.setText(utf8("未找到 UTAU 音源库：请在设置的算法页选择默认音源文件夹"),
                            juce::dontSendNotification);
    else
        statusLabel.setText(utf8("已切换到 UTAU，正在生成音符音频…"),
                            juce::dontSendNotification);
    refreshProjectControls();
}

void MainComponent::closeEnvelopeLanes()
{
    // The state and both buttons, without touching the tool: the callers
    // are choosing a tool of their own and set it themselves.
    envelopeLane = EnvelopeLane::none;
    utauAmplitudeEnvelopeActive = false;
    volumeParamButton.setToggleState(false, juce::dontSendNotification);
    flagEnvelopeButton.setToggleState(false, juce::dontSendNotification);
}

MainComponent::EnvelopeLane MainComponent::nextEnvelopeLane(EnvelopeLane open,
                                                            EnvelopeLane clicked)
{
    if (clicked == EnvelopeLane::none) return EnvelopeLane::none;
    // The button for the lane that is already open closes it; any other
    // button opens its own, which closes whatever was open before.
    return open == clicked ? EnvelopeLane::none : clicked;
}

void MainComponent::setEnvelopeLane(EnvelopeLane lane)
{
    envelopeLane = lane;
    utauAmplitudeEnvelopeActive = lane == EnvelopeLane::amplitude;
    if (lane != EnvelopeLane::none) setSourceEditMode(false);
    switch (lane)
    {
        case EnvelopeLane::amplitude:
            pianoRoll.setTool(PianoRollComponent::Tool::amplitude);
            setToolButton(volumeParamButton);
            parameterMode = ParameterMode::volume;
            break;
        case EnvelopeLane::flagCurve:
            pianoRoll.setTool(PianoRollComponent::Tool::flagCurve);
            setToolButton(flagEnvelopeButton);
            break;
        case EnvelopeLane::none:
            pianoRoll.setTool(PianoRollComponent::Tool::note);
            setToolButton(noteEditButton);
            break;
    }
    // Both buttons, every time: the one that was not clicked is exactly the
    // one that used to be left showing a lane it no longer had.
    volumeParamButton.setToggleState(lane == EnvelopeLane::amplitude,
                                     juce::dontSendNotification);
    flagEnvelopeButton.setToggleState(lane == EnvelopeLane::flagCurve,
                                      juce::dontSendNotification);
    refreshSelectedNoteParameter();
}

MainComponent::WheelAction MainComponent::wheelActionFor(
    const juce::ModifierKeys& modifiers)
{
    if (modifiers.isCommandDown())
        return modifiers.isShiftDown() ? WheelAction::zoomHorizontally
                                       : WheelAction::zoomVertically;
    return modifiers.isShiftDown() ? WheelAction::scrollHorizontally
                                   : WheelAction::scrollVertically;
}

std::vector<int> MainComponent::stretchAlgorithmItemsFor(int pitchAlgorithmItemId)
{
    // NSF-HiFiGAN reads the value as a splice order and names only two of
    // them; every other value decodes at a fixed hop, so offering "loop" and
    // "SoundTouch" there would be two more names for Melodyne Hybrid.
    if (pitchAlgorithmItemId == 2) return { 1, 2, 5 };
    // vslib is the Signalsmith stretcher itself, so all three of its analysis
    // clocks are real -- and the two NSF orders mean nothing to it.
    if (pitchAlgorithmItemId == 4) return { 1, 3, 4 };
    return {};
}

void MainComponent::addTrackFromMenu(bool compose)
{
    selectedTrackId = project.addTrack(
        strings.text(compose ? "track.compose" : "track.audio"), compose);
    trackList.setSelectedTrack(selectedTrackId);
    refreshProjectControls();
    menuItemsChanged();
}

void MainComponent::addReferenceTrackFromMenu()
{
    // An audio track: material is a recording to work against, not a part to
    // be composed.
    selectedTrackId = project.addTrack(strings.text("track.newReference"), false, true);
    trackList.setSelectedTrack(selectedTrackId);
    refreshProjectControls();
    menuItemsChanged();
}

void MainComponent::syncAudio(const ProjectData& data)
{
    audio.setAuditionTrack(selectedTrackId);
    audio.syncProject(data);
}

juce::String MainComponent::selectionAfterRemoving(const std::vector<TrackData>& tracks,
                                                   const juce::String& removedId)
{
    for (std::size_t index = 0; index < tracks.size(); ++index)
        if (tracks[index].id == removedId)
        {
            if (index + 1 < tracks.size()) return tracks[index + 1].id;
            if (index > 0) return tracks[index - 1].id;
            return {};
        }
    return removedId;
}

void MainComponent::deleteSelectedTrack()
{
    const auto trackId = selectedTrackId;
    if (trackId.isEmpty()) return;
    confirmDestructive(strings.text("track.delete"), [this, trackId]
    {
        const auto next = selectionAfterRemoving(project.snapshot().tracks, trackId);
        project.removeTrack(trackId);
        selectedTrackId = next;
        trackList.setSelectedTrack(next);
        pianoRoll.setFocusedTrack(next);
        refreshProjectControls();
        menuItemsChanged();
    });
}

namespace
{
// The dropdown's contents, in order.  A menu id is a position in this list,
// so what is offered and what a click means come from the same place and
// cannot drift apart when an entry is added or moved.
struct ViewMenuEntry
{
    const char* text;
    bool MainComponent::ViewOptions::* flag;
    bool utauOnly;   // there is nothing for it to draw in the other modes
};
const std::array<ViewMenuEntry, 4> viewMenuEntries {{
    { "范围", &MainComponent::ViewOptions::noteRange, false },
    { "包络", &MainComponent::ViewOptions::envelope, false },
    // Named for what it draws rather than kept as the button's 波形显示: the
    // View menu already offers 显示波形 for the clip's own waveform, and two
    // near identical names for two different pictures is a trap.
    { "合成波形", &MainComponent::ViewOptions::utauWaveform, true },
    { "音高线", &MainComponent::ViewOptions::pitchLine, false },
}};
}

MainComponent::ViewOptions MainComponent::viewOptionsFrom(const juce::PropertySet& properties)
{
    ViewOptions options;
    options.noteRange = properties.getBoolValue("ui.showNoteRange", true);
    options.envelope = properties.getBoolValue("ui.showEnvelope", false);
    options.utauWaveform = properties.getBoolValue("ui.showUtauWaveforms", false);
    options.pitchLine = properties.getBoolValue("ui.showPitchLine", true);
    return options;
}

void MainComponent::storeViewOptions(juce::PropertySet& properties,
                                     const ViewOptions& options)
{
    properties.setValue("ui.showNoteRange", options.noteRange);
    properties.setValue("ui.showEnvelope", options.envelope);
    properties.setValue("ui.showUtauWaveforms", options.utauWaveform);
    properties.setValue("ui.showPitchLine", options.pitchLine);
}

bool MainComponent::viewMenuItemEnabled(int chosen, bool utauEditorActive)
{
    const auto index = static_cast<std::size_t>(chosen - 1);
    if (chosen <= 0 || index >= viewMenuEntries.size()) return false;
    return utauEditorActive || !viewMenuEntries[index].utauOnly;
}

MainComponent::ViewOptions MainComponent::afterViewMenuChoice(ViewOptions options,
                                                              int chosen)
{
    // Zero is the menu being dismissed, which changes nothing.
    const auto index = static_cast<std::size_t>(chosen - 1);
    if (chosen > 0 && index < viewMenuEntries.size())
    {
        const auto flag = viewMenuEntries[index].flag;
        options.*flag = !(options.*flag);
    }
    return options;
}

void MainComponent::applyViewOptions()
{
    pianoRoll.setShowNoteRange(viewOptions.noteRange);
    pianoRoll.setShowEnvelope(viewOptions.envelope);
    pianoRoll.setShowUtauWaveforms(viewOptions.utauWaveform);
    pianoRoll.setShowPitchLine(viewOptions.pitchLine);
}

void MainComponent::showViewMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());
    // Ticks rather than radio items: both can be on at once, and now that the
    // buttons are gone the tick is the only place their state shows.
    const auto utauEditorActive = isUtauAlgorithmSelected();
    for (std::size_t index = 0; index < viewMenuEntries.size(); ++index)
    {
        const auto id = static_cast<int>(index) + 1;
        menu.addItem(id, juce::String::fromUTF8(viewMenuEntries[index].text),
                     viewMenuItemEnabled(id, utauEditorActive),
                     viewOptions.*(viewMenuEntries[index].flag));
    }
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&showViewMenuButton),
        [this](int chosen)
        {
            const auto updated = afterViewMenuChoice(viewOptions, chosen);
            if (updated.noteRange == viewOptions.noteRange
                && updated.envelope == viewOptions.envelope
                && updated.utauWaveform == viewOptions.utauWaveform
                && updated.pitchLine == viewOptions.pitchLine) return;
            viewOptions = updated;
            applyViewOptions();
            if (preferences != nullptr) storeViewOptions(*preferences, viewOptions);
        });
}

void MainComponent::setDrawLengthDivision(int division)
{
    pianoRoll.setDrawLengthDivision(division);
    if (preferences != nullptr)
        preferences->setValue("ui.drawLengthDivision", pianoRoll.drawLengthDivision());
}

void MainComponent::showDrawSettingsMenu(juce::Point<int> screenPosition)
{
    juce::PopupMenu units;
    const auto offered = PianoRollComponent::drawLengthDivisions();
    const auto current = pianoRoll.drawLengthDivision();
    for (std::size_t index = 0; index < offered.size(); ++index)
        units.addItem(static_cast<int>(index) + 1,
                      "1/" + juce::String(offered[index]) + utf8(" 小节"),
                      true, offered[index] == current);

    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());
    menu.addSubMenu(utf8("选择最小拉伸倍率"), units);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea(
            juce::Rectangle<int>(screenPosition.x, screenPosition.y, 1, 1)),
        [this, offered](int chosen)
        {
            // The id is the position in the list the menu was built from, so
            // what is shown and what a click means come from one place.
            const auto index = static_cast<std::size_t>(chosen - 1);
            if (chosen > 0 && index < offered.size())
                setDrawLengthDivision(offered[index]);
        });
}

MainComponent::DeleteTarget MainComponent::deleteTargetFor(bool notesSelected,
                                                           bool clipSelected)
{
    if (notesSelected) return DeleteTarget::notes;
    if (clipSelected) return DeleteTarget::clip;
    return DeleteTarget::nothing;
}

void MainComponent::deleteSelectedClip()
{
    const auto clipId = selectedClipId;
    if (clipId.isEmpty()) return;
    confirmDestructive(strings.text("clip.delete"),
        [this, clipId] { project.removeClip(clipId); });
}

void MainComponent::showTrackAreaMenu(juce::Point<int> screenPosition)
{
    juce::PopupMenu menu;
    menu.setLookAndFeel(&getLookAndFeel());
    menu.addItem(1, strings.text("track.newHere"));
    // Somewhere to keep a reference vocal or a backing take: audible while it
    // is the track in hand, silent whenever anything else is playing.
    menu.addItem(3, strings.text("track.newReference"));
    // Named for the selected track rather than for the click, because the
    // click landed on the space between tracks and points at none of them --
    // and a right-click there deliberately leaves the selection alone.
    menu.addItem(2, strings.text("track.delete"), selectedTrackId.isNotEmpty());
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetScreenArea(
            juce::Rectangle<int>(screenPosition.x, screenPosition.y, 1, 1)),
        [this](int chosen)
        {
            // A melodic track, which is what the app makes by default and what
            // ProjectModel::addTrack assumes; the Track menu still offers the
            // audio kind explicitly for the times that is not what is wanted.
            if (chosen == 1) addTrackFromMenu(true);
            else if (chosen == 3) addReferenceTrackFromMenu();
            else if (chosen == 2) deleteSelectedTrack();
        });
}

void MainComponent::refreshStretchAlgorithmItems(int preferredId)
{
    const auto previous = preferredId > 0 ? preferredId : stretchAlgorithm.getSelectedId();
    const auto items = stretchAlgorithmItemsFor(pitchAlgorithm.getSelectedId());
    stretchAlgorithm.clear(juce::dontSendNotification);
    for (const auto item : items)
        stretchAlgorithm.addItem(strings.text(
            item == 2 ? "algo.stretch.nsfVariableMel"
            : item == 3 ? "algo.stretch.loop"
            : item == 4 ? "algo.stretch.soundTouch"
            : item == 5 ? "algo.stretch.nsfShiftThenSplice"
            : "algo.stretch.melodyneHybrid"), item);
    // Keep the current choice only if it survived into the new list.  It often
    // does not: switching backend can take away the very clock that was
    // selected, and the caller resets the track to Melodyne Hybrid when the
    // selection moves.  Melodyne Hybrid is item 1 everywhere, so a picker that
    // is about to be hidden still reports a sane, harmless id.
    const auto kept = previous > 0 && stretchAlgorithm.indexOfItemId(previous) >= 0;
    stretchAlgorithm.setSelectedId(kept ? previous : 1, juce::dontSendNotification);
}

void MainComponent::diagnosticPressTool(PianoRollComponent::Tool wanted)
{
    // Through the button's own handler, so the check follows the path a click
    // follows and not a shortcut around it.
    juce::Button* button = &noteEditButton;
    if (wanted == PianoRollComponent::Tool::points) button = &pointButton;
    else if (wanted == PianoRollComponent::Tool::draw) button = &drawButton;
    else if (wanted == PianoRollComponent::Tool::line) button = &lineButton;
    if (button->onClick) button->onClick();
}

PianoRollComponent::Tool MainComponent::diagnosticTool() const
{
    return pianoRoll.currentTool();
}

void MainComponent::diagnosticRefreshControls()
{
    refreshProjectControls();
}

void MainComponent::setToolButton(juce::Button& selected)
{
    const std::array<juce::Button*, 6> editTools {
        &noteEditButton, &wrenchButton, &drawButton, &lineButton, &pointButton, &connectButton
    };
    for (auto* button : editTools)
        button->setToggleState(button == &selected, juce::dontSendNotification);
    const std::array<juce::Button*, 7> parameterTools {
        &pitchParamButton, &driftParamButton, &attackParamButton,
        &breathParamButton, &tensionParamButton,
        &formantParamButton, &volumeParamButton
    };
    for (auto* button : parameterTools)
        if (&selected == button || &selected == &pitchParamButton || &selected == &driftParamButton
            || &selected == &attackParamButton
            || &selected == &breathParamButton
            || &selected == &tensionParamButton || &selected == &formantParamButton
            || &selected == &volumeParamButton)
            button->setToggleState(button == &selected, juce::dontSendNotification);
}

std::optional<int> MainComponent::followViewPosition(
    int viewLeft, int viewWidth, int playheadX, int leftMargin,
    const std::optional<juce::Range<int>>& run)
{
    if (viewWidth <= 0) return {};
    const auto rightMargin = 56;
    // Repeating a phrase means hearing it again, and again after that.  If it
    // fits the window there is no reason to move at all once it is in view:
    // paging away at the end left the phrase behind and had to be dragged
    // back to before it could be played a second time.
    if (run && run->getLength() > 0
        && run->getLength() <= viewWidth - leftMargin - rightMargin)
    {
        if (run->getStart() >= viewLeft + leftMargin
            && run->getEnd() <= viewLeft + viewWidth - rightMargin)
            return {};
        return std::max(0, run->getStart() - leftMargin - 24);
    }
    if (playheadX < viewLeft + leftMargin
        || playheadX > viewLeft + viewWidth - rightMargin)
        return std::max(0, playheadX - viewWidth / 4);
    return {};
}

void MainComponent::refreshCollapseIcon()
{
    // The chevron points the way the arrangement is about to go.  Only this
    // knows which one that is: applyPreferences settles the state before the
    // constructor has finished naming icons, so a second place naming one
    // would put the wrong chevron back.
    collapseTracksButton.setComponentID(tracksCollapsed ? "icon.expand"
                                                        : "icon.collapse");
    collapseTracksButton.repaint();
}

void MainComponent::setTracksCollapsed(bool collapsed)
{
    tracksCollapsed = collapsed;
    refreshCollapseIcon();
    if (preferences != nullptr)
        preferences->setValue("ui.tracksCollapsed", collapsed);
    resized();
}

void MainComponent::armSelectionPlayback()
{
    // Playing a selection should finish with it.  Left to run, the transport
    // carried on through the rest of the piece and the view followed it, which
    // is not what asking to hear the selected part means.
    audio.setPlayUntil(0.0);
    playbackRun.reset();
    if (sourceEditActive) return;
    const auto span = pianoRoll.selectedNotesTimeSpan();
    // Starting at or past the end -- a marquee drawn right to left leaves the
    // playhead there -- would stop before a sound was made.
    if (span && span->getEnd() > audio.position() + 0.01)
    {
        // This is the marquee "export the last render" means: noted as play
        // begins, so stopping halfway through leaves it whole.  Pressing space
        // with nothing selected plays the render history instead and never
        // reaches here, which is why that does not count as one.
        lastRenderedNoteIds = pianoRoll.selectedNoteIds();
        lastRenderedSpan = *span;
        audio.setPlayUntil(span->getEnd());
        // What the view should try to keep in front of you: the selection, and
        // wherever the playhead was set if that is earlier still.
        playbackRun = juce::Range<double>(
            std::min(audio.position(), span->getStart()), span->getEnd());
    }
}

void MainComponent::togglePlayback()
{
    if (audio.isPlaying())
    {
        playWhenRenderReady = false;
        audio.stop();
        return;
    }
    if (!sourceEditActive && pianoRoll.selectedNoteIds().empty()
        && audio.selectAllRenderedUtauNotes())
        syncAudio(project.snapshot());
    juce::String deviceError;
    if (!audio.ensureOutputDevice(deviceError))
    {
        playWhenRenderReady = false;
        showError(strings.text("settings.noAudioDevice") + "\n" + deviceError);
        return;
    }
    // Never start a newly requested selection from fallback audio belonging to
    // the previous marquee.  Wait for the current render even when an older
    // phrase is still available as a continuity fallback.
    if (!sourceEditActive && audio.renderProgress())
    {
        playWhenRenderReady = true;
        return;
    }
    playWhenRenderReady = false;
    // Respect the transport position chosen by the user.  The marquee handler
    // already moves it to the selection once; subsequent Space presses and
    // explicit red-line seeks must never be overwritten by cached-audio bounds.
    armSelectionPlayback();
    audio.play();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(Palette::background);
    g.setColour(Palette::panel);
    g.fillRect(0, 0, getWidth(), 61);
    g.setColour(Palette::border);
    g.drawHorizontalLine(60, 0.0f, static_cast<float>(getWidth()));
    const auto splitterBounds = panelSplitter.getBounds();
    g.setColour(Palette::background);
    g.fillRect(splitterBounds);
    g.setColour(Palette::border);
    g.drawHorizontalLine(splitterBounds.getY(), static_cast<float>(splitterBounds.getX()),
                         static_cast<float>(splitterBounds.getRight()));
    g.drawHorizontalLine(splitterBounds.getBottom() - 1, static_cast<float>(splitterBounds.getX()),
                         static_cast<float>(splitterBounds.getRight()));
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z')
    {
        if (key.getModifiers().isShiftDown()) project.redo();
        else project.undo();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Y')
    {
        project.redo();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'G')
    {
        showRegionEditorForNote();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'A')
    {
        pianoRoll.selectAllNotes();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::deleteKey
        || key.getKeyCode() == juce::KeyPress::backspaceKey)
    {
        // Only reached when the roll did not take it, which it does whenever
        // notes are selected -- so in practice this is the clip case.
        switch (deleteTargetFor(!pianoRoll.selectedNoteIds().empty(),
                                selectedClipId.isNotEmpty()))
        {
            case DeleteTarget::notes:   pianoRoll.deleteSelectedNotes(); return true;
            case DeleteTarget::clip:    deleteSelectedClip(); return true;
            case DeleteTarget::nothing: return false;
        }
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'C')
    {
        if (!pianoRoll.selectedNoteIds().empty()) copySelectedNotes(false);
        else if (selectedClipId.isNotEmpty()) copySelectedClip();
        else return false;
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'X'
        && !pianoRoll.selectedNoteIds().empty())
    {
        copySelectedNotes(true);
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'V')
    {
        // Plain: the moment it was copied from when it is going to another
        // track, the playhead when it is going back onto its own -- there the
        // original moment would be on top of itself.  With shift: the playhead
        // either way, which is where clicking in the roll has already put it.
        if (!copiedNotes.empty())
            pasteCopiedNotes(key.getModifiers().isShiftDown()
                ? std::optional<double>(audio.position()) : std::nullopt);
        else if (copiedClipId.isNotEmpty()) pasteCopiedClip();
        else return false;
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'D'
        && selectedClipId.isNotEmpty())
    {
        duplicateSelectedClip();
        return true;
    }
    if (key == juce::KeyPress::spaceKey
        && (preferences == nullptr
            || preferences->getBoolValue("operation.spacePlayback", true)))
    {
        togglePlayback();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'N')
    {
        newProject();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'O')
    {
        openProject();
        return true;
    }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'S')
    {
        if (key.getModifiers().isShiftDown()) saveProjectAs();
        else saveProject();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::homeKey)
    {
        audio.stop();
        audio.setPosition(0.0);
        return true;
    }
    return false;
}

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& path : files)
        if (juce::File(path).hasFileExtension("wav;flac;aif;aiff;mp3;ogg;hjpx;hspx;mpd;mid;midi;ust"))
            return true;
    return false;
}

void MainComponent::openExternalFile(const juce::File& file)
{
    if (file.hasFileExtension("mpd"))
        loadMelodyneFile(file);
    else if (file.hasFileExtension("hjpx;hspx"))
        performWithUnsavedCheck([this, file] { loadProjectFile(file); });
    else if (file.hasFileExtension("ust")) loadUstFile(file);
    else if (file.hasFileExtension("mid;midi"))
    {
        juce::String error;
        if (!project.addMidiFile(file, error))
            showError(strings.text("error.midi") + "\n" + error);
    }
    else if (const auto duration = audio.probeDuration(file))
        addAnalysedAudioFile(file, *duration);
}

void MainComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    auto dropSeconds = audio.position();
    juce::String targetTrackId;
    if (timelineViewport.getBounds().contains(x, y))
    {
        dropSeconds = timeline.secondsForPixel(x - timelineViewport.getX()
                                               + timelineViewport.getViewPositionX());
        targetTrackId = timeline.trackIdForPixel(y - timelineViewport.getY()
                                                 + timelineViewport.getViewPositionY());
    }
    else if (pianoViewport.getBounds().contains(x, y))
    {
        dropSeconds = pianoRoll.secondsForPixel(x - pianoViewport.getX()
                                                + pianoViewport.getViewPositionX());
        targetTrackId = selectedTrackId;
    }
    auto nextDropSeconds = dropSeconds;
    for (const auto& path : files)
    {
        const juce::File file(path);
        if (file.hasFileExtension("wav;flac;aif;aiff;mp3;ogg"))
        {
            if (const auto duration = audio.probeDuration(file))
            {
                addAnalysedAudioFile(file, *duration, nextDropSeconds, targetTrackId);
                if (targetTrackId.isNotEmpty()) nextDropSeconds += *duration;
            }
        }
        else
            openExternalFile(file);
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    auto menu = area.removeFromTop(27);
    menuBar.setBounds(menu);
    auto toolbar = area.removeFromTop(34).reduced(5, 3);
    auto take = [&toolbar](juce::Component& component, int width)
    {
        component.setBounds(toolbar.removeFromLeft(width));
        toolbar.removeFromLeft(3);
    };
    take(bpmCaption, 30);
    take(bpmEditor, 50);
    take(beatsCaption, 58);
    take(beatsEditor, 32);
    take(denominatorLabel, 27);
    take(gridCaption, 28);
    take(gridSelector, 67);
    take(stretchCaption, 66);
    take(stretchSelector, 70);
    take(scaleCaption, 52);
    take(scaleSelector, 58);
    toolbar.removeFromLeft(5);
    take(stopButton, 28);
    take(playButton, 28);
    toolbar.removeFromLeft(5);
    take(openButton, 28);
    take(saveButton, 28);
    take(audioButton, 32);
    take(melodyneButton, 32);
    toolbar.removeFromLeft(5);
    take(collapseTracksButton, 28);
    zoomSlider.setBounds(toolbar.removeFromRight(100));
    vZoomSlider.setBounds(toolbar.removeFromRight(70));
    progressBar.setBounds(toolbar.removeFromRight(100).reduced(4, 5));

    auto footer = area.removeFromBottom(24);
    statusLabel.setBounds(footer.reduced(8, 0));
    const auto utauEditorActive = isUtauAlgorithmSelected();
    volumeParamButton.setButtonText(utauEditorActive ? utf8("响度包络")
                                                     : strings.text("param.volume"));
    volumeParamButton.setTooltip(utauEditorActive
        ? utf8("显示全部 UTAU 音符的振幅包络；拖动标点编辑，双击曲线加点，右键内部标点删除")
        : strings.text("param.volume"));
    // Both editing modes reserve the same second row.  Its contents change,
    // but the piano-roll origin must not jump when switching between the
    // Melodyne-compatible and UTAU workflows.
    constexpr auto modeEditorHeight = 36;
    const auto sampleEditorHeight = sourceEditActive ? 36 : 0;
    const auto splitAvailable = std::max(1, area.getHeight() - 8 - 36
        - sampleEditorHeight - modeEditorHeight);
    // Folded away, the arrangement takes no room at all and the splitter goes
    // with it -- there is nothing left above to drag against.
    const auto upperHeight = tracksCollapsed ? 0
        : juce::jlimit(150, std::max(150, splitAvailable - 120),
            static_cast<int>(std::round(
                static_cast<float>(splitAvailable) * panelSplitRatio)));
    for (auto* component : { static_cast<juce::Component*>(&trackViewport),
                             static_cast<juce::Component*>(&timelineViewport),
                             static_cast<juce::Component*>(&panelSplitter) })
        component->setVisible(!tracksCollapsed);
    if (!tracksCollapsed)
    {
        auto upper = area.removeFromTop(upperHeight);
        trackViewport.setBounds(upper.removeFromLeft(280));
        timelineViewport.setBounds(upper);
        panelSplitter.setBounds(area.removeFromTop(8));
    }

    auto parameterHeader = area.removeFromTop(36).reduced(4, 3);
    auto takeParameterRight = [&parameterHeader](juce::Component& component, int width)
    {
        component.setBounds(parameterHeader.removeFromRight(width));
        parameterHeader.removeFromRight(3);
    };
    // No stretch algorithm in any UTAU mode: the stretching is the
    // resampler's, and the picker only ever sat there greyed out.  And none on
    // the four backends that stretch inside their own renderers, where every
    // item rendered byte-identical audio and only the diagnostic label moved.
    // Hidden rather than disabled, and its space given back to the row.
    const auto showStretch = !utauEditorActive
        && !stretchAlgorithmItemsFor(pitchAlgorithm.getSelectedId()).empty();
    stretchAlgorithm.setVisible(showStretch);
    stretchLabel.setVisible(showStretch);
    if (showStretch)
    {
        takeParameterRight(stretchAlgorithm, 124);
        takeParameterRight(stretchLabel, 52);
    }
    // Rendering a phrase in one pass is something only the neural decoder
    // does, so the choice is offered only where it means anything.
    const auto showRenderOrder = !utauEditorActive
        && pitchAlgorithm.getSelectedId() == 2;
    renderOrder.setVisible(showRenderOrder);
    renderOrderLabel.setVisible(showRenderOrder);
    if (showRenderOrder)
    {
        takeParameterRight(renderOrder, 108);
        takeParameterRight(renderOrderLabel, 38);
    }
    takeParameterRight(pitchAlgorithm, 106);
    takeParameterRight(pitchLabel, 50);
    voicebankSettingsButton.setVisible(utauEditorActive);
    if (utauEditorActive) takeParameterRight(voicebankSettingsButton, 104);
    spliceButton.setVisible(utauEditorActive);
    if (utauEditorActive) takeParameterRight(spliceButton, 60);
    auto takeParameter = [&parameterHeader](juce::Component& component, int width)
    {
        component.setBounds(parameterHeader.removeFromLeft(width));
        parameterHeader.removeFromLeft(3);
    };
    takeParameter(parameterTitle, 70);
    takeParameter(noteEditButton, 27);
    takeParameter(drawButton, 27);
    takeParameter(lineButton, 27);
    // In every mode: the native renderer reads the pitch points too, so the
    // tool does the same thing on either kind of track.
    pointButton.setVisible(true);
    takeParameter(pointButton, 27);
    takeParameter(wrenchButton, 27);
    takeParameter(connectButton, 27);
    // The common row ends with the view menu and optional robust-pitch toggle.
    // Parameter controls are placed in the mode row below, not mixed into the
    // tool row, so both modes retain the same tool geometry.
    for (auto* component : { static_cast<juce::Component*>(&smoothCaption),
         static_cast<juce::Component*>(&smoothSlider),
         static_cast<juce::Component*>(&pitchParamButton),
         static_cast<juce::Component*>(&driftParamButton),
         static_cast<juce::Component*>(&attackParamButton),
         static_cast<juce::Component*>(&breathParamButton),
         static_cast<juce::Component*>(&tensionParamButton),
         static_cast<juce::Component*>(&formantParamButton),
         static_cast<juce::Component*>(&volumeParamButton),
         static_cast<juce::Component*>(&flagEnvelopeButton),
         static_cast<juce::Component*>(&envelopePresetCaption) })
        component->setVisible(false);
    for (auto& button : envelopePresetButtons) button.setVisible(false);
    // After the envelope presets, at the end of the row, as asked.  That puts
    // it past the controls that come and go with the mode, so unlike before it
    // does not sit at a fixed x.
    takeParameter(showViewMenuButton, 72);
    if (robustPitchCurveButton.isVisible())
        takeParameter(robustPitchCurveButton, 74);
    sourceEditHint.setBounds(parameterHeader.reduced(3, 0));
    auto sampleBar = area.removeFromTop(sampleEditorHeight).reduced(4, 3);
    auto setSampleVisible = [this](bool visible)
    {
        for (auto* component : { static_cast<juce::Component*>(&sampleRegionSelector),
             static_cast<juce::Component*>(&sampleAliasEditor), static_cast<juce::Component*>(&sampleStartEditor),
             static_cast<juce::Component*>(&sampleEndEditor), static_cast<juce::Component*>(&sampleAlignmentEditor),
             static_cast<juce::Component*>(&sampleFixedEditor), static_cast<juce::Component*>(&sampleAliasLabel),
             static_cast<juce::Component*>(&sampleStartLabel), static_cast<juce::Component*>(&sampleEndLabel),
             static_cast<juce::Component*>(&sampleAlignmentLabel), static_cast<juce::Component*>(&sampleFixedLabel),
             static_cast<juce::Component*>(&sampleSaveButton), static_cast<juce::Component*>(&otoImportButton),
             static_cast<juce::Component*>(&otoExportButton) }) component->setVisible(visible);
    };
    setSampleVisible(sourceEditActive);
    if (sourceEditActive)
    {
        const auto takeSample = [&sampleBar](juce::Component& component, int width)
        {
            component.setBounds(sampleBar.removeFromLeft(width));
            sampleBar.removeFromLeft(3);
        };
        takeSample(sampleRegionSelector, 104);
        takeSample(sampleAliasLabel, 34); takeSample(sampleAliasEditor, 100);
        takeSample(sampleStartLabel, 34); takeSample(sampleStartEditor, 64);
        takeSample(sampleEndLabel, 30); takeSample(sampleEndEditor, 64);
        takeSample(sampleAlignmentLabel, 44); takeSample(sampleAlignmentEditor, 64);
        takeSample(sampleFixedLabel, 44); takeSample(sampleFixedEditor, 64);
        takeSample(sampleSaveButton, 64);
        takeSample(otoImportButton, 82);
        takeSample(otoExportButton, 82);
    }
    auto modeBar = area.removeFromTop(modeEditorHeight).reduced(4, 3);
    for (auto* component : { static_cast<juce::Component*>(&utauVoicebankLabel),
         static_cast<juce::Component*>(&utauVoicebankButton),
         static_cast<juce::Component*>(&utauVoicebankPath),
         static_cast<juce::Component*>(&noteAliasLabel),
         static_cast<juce::Component*>(&noteAliasEditor),
         static_cast<juce::Component*>(&noteConsonantVelocityLabel),
         static_cast<juce::Component*>(&noteConsonantVelocityEditor),
         static_cast<juce::Component*>(&noteFlagsLabel),
         static_cast<juce::Component*>(&noteFlagsEditor),
         static_cast<juce::Component*>(&flagCurveButton) })
         component->setVisible(utauEditorActive);
    if (utauEditorActive)
    {
        const auto takeUtau = [&modeBar](juce::Component& component, int width)
        {
            component.setBounds(modeBar.removeFromLeft(width));
            modeBar.removeFromLeft(4);
        };
        takeUtau(utauVoicebankLabel, 76);
        takeUtau(utauVoicebankButton, 108);
        takeUtau(utauVoicebankPath, std::min(330, std::max(100, modeBar.getWidth() - 620)));
        takeUtau(noteAliasLabel, 92);
        takeUtau(noteAliasEditor, 130);
        takeUtau(noteConsonantVelocityLabel, 72);
        takeUtau(noteConsonantVelocityEditor, 52);
        takeUtau(noteFlagsLabel, 42);
        // Leave room for the switch beside it rather than running the box to
        // the end of the bar.
        takeUtau(noteFlagsEditor, std::max(90, modeBar.getWidth() - 96));
        takeUtau(flagCurveButton, std::min(92, std::max(0, modeBar.getWidth())));
    }
    else
    {
        // Melodyne-compatible tracks use the same row for their common
        // expression controls.  The buttons keep their existing handlers and
        // the project's custom icons; only their placement is shared with UTAU.
        const auto takeCommon = [&modeBar](juce::Component& component, int width)
        {
            component.setVisible(true);
            component.setBounds(modeBar.removeFromLeft(width));
            modeBar.removeFromLeft(4);
        };
        takeCommon(smoothCaption, 42);
        takeCommon(smoothSlider, 118);
        takeCommon(pitchParamButton, 50);
        takeCommon(driftParamButton, 50);
        takeCommon(attackParamButton, 50);
        takeCommon(breathParamButton, 50);
        takeCommon(tensionParamButton, 50);
        takeCommon(formantParamButton, 50);
        takeCommon(volumeParamButton, 50);
        robustPitchCurveButton.setVisible(pitchAlgorithm.getSelectedId() == 1);
        if (robustPitchCurveButton.isVisible()) takeCommon(robustPitchCurveButton, 104);
    }
    constexpr auto zoomButtonSize = 24;
    constexpr auto zoomButtonGap = 2;
    constexpr auto zoomControlRail = zoomButtonSize + 4;
    const auto pianoArea = area;
    pianoViewport.setBounds(pianoArea.withTrimmedRight(zoomControlRail)
                                     .withTrimmedBottom(zoomControlRail));

    const auto horizontalY = pianoArea.getBottom() - zoomButtonSize - 2;
    const auto horizontalRight = pianoArea.getRight() - zoomControlRail;
    horizontalZoomInButton.setBounds(horizontalRight - zoomButtonSize,
                                     horizontalY, zoomButtonSize, zoomButtonSize);
    horizontalZoomOutButton.setBounds(horizontalRight - zoomButtonSize * 2 - zoomButtonGap,
                                      horizontalY, zoomButtonSize, zoomButtonSize);

    const auto verticalX = pianoArea.getRight() - zoomButtonSize - 2;
    const auto verticalBottom = pianoArea.getBottom() - zoomControlRail;
    verticalZoomInButton.setBounds(verticalX, verticalBottom - zoomButtonSize,
                                   zoomButtonSize, zoomButtonSize);
    verticalZoomOutButton.setBounds(verticalX,
                                    verticalBottom - zoomButtonSize * 2 - zoomButtonGap,
                                    zoomButtonSize, zoomButtonSize);
    if (!pianoInitialScrollSet && pianoViewport.getHeight() > 0)
    {
        pianoViewport.setViewPosition(0, std::max(0, (pianoRoll.getHeight() - pianoViewport.getHeight()) / 2));
        pianoInitialScrollSet = true;
    }
}

void MainComponent::adjustHorizontalZoom(double factor)
{
    const auto nextZoom = juce::jlimit(zoomSlider.getMinimum(), zoomSlider.getMaximum(),
                                      zoomSlider.getValue() * factor);
    if (std::abs(nextZoom - zoomSlider.getValue()) < 0.001) return;
    zoomSlider.setValue(nextZoom);
}

void MainComponent::adjustVerticalZoom(double factor)
{
    const auto oldZoom = vZoomSlider.getValue();
    const auto nextZoom = juce::jlimit(vZoomSlider.getMinimum(), vZoomSlider.getMaximum(),
                                      oldZoom * factor);
    if (std::abs(nextZoom - oldZoom) < 0.001) return;

    const auto viewHeight = pianoViewport.getViewHeight();
    const auto oldCentreY = pianoViewport.getViewPositionY() + viewHeight / 2.0;
    vZoomSlider.setValue(nextZoom);
    const auto nextViewY = std::max(0, static_cast<int>(std::round(
        oldCentreY * nextZoom / oldZoom - viewHeight / 2.0)));
    pianoViewport.setViewPosition(pianoViewport.getViewPositionX(), nextViewY);
}

void MainComponent::mouseDown(const juce::MouseEvent& event)
{
    if (event.eventComponent != &panelSplitter) return;
    draggingPanelSplitter = true;
    panelSplitterDragScreenY = event.getScreenY();
    panelSplitterDragRatio = panelSplitRatio;
}

void MainComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (!draggingPanelSplitter || event.eventComponent != &panelSplitter) return;
    const auto splitAvailable = std::max(1, getHeight() - 27 - 34 - 24 - 8 - 36);
    panelSplitRatio = juce::jlimit(0.15f, 0.85f,
        panelSplitterDragRatio + static_cast<float>(event.getScreenY() - panelSplitterDragScreenY)
            / static_cast<float>(splitAvailable));
    resized();
    repaint();
}

void MainComponent::mouseUp(const juce::MouseEvent&)
{
    draggingPanelSplitter = false;
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &project)
    {
        const auto data = project.snapshot();
        syncAudio(data);
        const auto trackExists = std::any_of(data.tracks.begin(), data.tracks.end(),
            [this](const auto& track) { return track.id == selectedTrackId; });
        if (!trackExists) selectedTrackId.clear();
        if (selectedTrackId.isEmpty())
        {
            const auto firstCompose = std::find_if(data.tracks.begin(), data.tracks.end(),
                [](const auto& track) { return track.compose; });
            if (firstCompose != data.tracks.end()) selectedTrackId = firstCompose->id;
        }
        trackList.setSelectedTrack(selectedTrackId);
        pianoRoll.setFocusedTrack(selectedTrackId);
        auto clipExists = false;
        for (const auto& track : data.tracks)
            clipExists = clipExists || std::any_of(track.clips.begin(), track.clips.end(),
                [this](const auto& clip) { return clip.id == selectedClipId; });
        if (!clipExists)
        {
            selectedClipId.clear();
            pianoRoll.setFocusedClip({});
        }
        auto noteExists = false;
        for (const auto& track : data.tracks)
            for (const auto& clip : track.clips)
                noteExists = noteExists || std::any_of(clip.notes.begin(), clip.notes.end(),
                    [this](const auto& note) { return note.id == selectedNoteId; });
        if (!noteExists) selectedNoteId.clear();
        refreshProjectControls();
        refreshSelectedNoteParameter();
        menuItemsChanged();
    }
}

void MainComponent::timerCallback()
{
    const auto expectedPlayIcon = audio.isPlaying() ? juce::String("icon.pause")
                                                     : juce::String("icon.play");
    if (playButton.getComponentID() != expectedPlayIcon)
    {
        playButton.setComponentID(expectedPlayIcon);
        playButton.setTooltip(strings.text(audio.isPlaying() ? "transport.pause" : "transport.play"));
        playButton.repaint();
    }
    pianoRoll.setPlayheadSeconds(audio.position());
    timeline.setPlayheadSeconds(audio.position());
    // Which UTAU notes have audio to show.  The roll compares each against the
    // note still under it, so a render finishing here is what makes a waveform
    // appear and an edit is what makes it go.
    audio.refreshUtauWaveforms();
    pianoRoll.setUtauNoteWaveforms(audio.utauNoteWaveforms());
    trackList.repaint();
    // An export asked for the whole song to be rendered and is waiting for it.
    // Writing the files is quick -- it is only mixing what is already
    // rendered -- so it can happen here, where the render has just finished.
    if (exportWaitingForRender && !audio.renderProgress())
    {
        finishExport();
        return;
    }
    if (!importInProgress)
    {
        if (const auto render = audio.renderProgress())
        {
            showingRenderProgress = true;
            progress = *render;
            const auto percent = juce::String(static_cast<int>(
                std::round(*render * 100.0))) + "%";
            statusLabel.setText(activeUtauSelectionCount > 0
                    ? "UTAU selection: " + juce::String(activeUtauSelectionCount)
                        + " notes  |  Rendering " + percent
                    : strings.text("status.rendering") + "  " + percent,
                juce::dontSendNotification);
        }
        else
        {
            if (showingRenderProgress)
            {
                showingRenderProgress = false;
                progress = 0.0;
            }
            if (pendingNativeAnalyses > 0)
                statusLabel.setText(strings.text("status.analyzing") + "  "
                    + nativeAnalysisName + "  "
                    + juce::String(static_cast<int>(std::round(nativeAnalysisProgress * 100.0)))
                    + "%", juce::dontSendNotification);
            else
            {
                const auto backend = audio.activeRenderBackends();
                const auto renderWarning = audio.activeRenderWarnings();
                statusLabel.setText((audio.isPlaying() ? strings.text("transport.play")
                                                        : strings.text("status.ready"))
                                        + "  " + juce::String(audio.position(), 2) + " s"
                                        + (backend.isNotEmpty() ? "  ·  " + backend : juce::String())
                                        + (renderWarning.isNotEmpty()
                                            ? "  ·  UTAU: " + renderWarning : juce::String()),
                                    juce::dontSendNotification);
            }
            if (playWhenRenderReady)
            {
                playWhenRenderReady = false;
                juce::String deviceError;
                if (!audio.hasCurrentRenderedAudio())
                {
                    statusLabel.setText(utf8("UTAU 当前选区没有生成可播放音频"),
                                        juce::dontSendNotification);
                }
                else if (audio.ensureOutputDevice(deviceError))
                {
                    armSelectionPlayback();
                    audio.play();
                }
                else showError(strings.text("settings.noAudioDevice") + "\n" + deviceError);
            }
        }
    }

    if (!syncingScroll)
    {
        // While playing, one decision, made for the roll, with the ruler put
        // on the same instant afterwards.
        //
        // Asking each panel separately looked tidier and was not: they are
        // different widths, so a run that fits one need not fit the other,
        // and then one held still while the other paged.  The sync below --
        // which cannot tell a scroll the user made from one the follow just
        // made -- carried the paging panel's position across and undid the
        // one that had held.  Next tick it held again.  That was the flicker,
        // and a long note brought it on because that is when the two panels
        // disagree about fitting.
        const auto playing = audio.isPlaying();
        if (playing)
        {
            const auto run = playbackRun
                ? std::optional<juce::Range<int>>(juce::Range<int>(
                      pianoRoll.pixelForSeconds(playbackRun->getStart()),
                      pianoRoll.pixelForSeconds(playbackRun->getEnd())))
                : std::nullopt;
            if (const auto next = followViewPosition(
                    pianoViewport.getViewPositionX(), pianoViewport.getViewWidth(),
                    pianoRoll.pixelForSeconds(audio.position()), 58, run))
            {
                pianoViewport.setViewPosition(*next, pianoViewport.getViewPositionY());
                if (!sourceEditActive)
                    timelineViewport.setViewPosition(
                        std::max(0, timeline.pixelForSeconds(
                            pianoRoll.secondsForPixel(*next))),
                        timelineViewport.getViewPositionY());
            }
        }
        const auto timelineX = timelineViewport.getViewPositionX();
        const auto pianoX = pianoViewport.getViewPositionX();
        syncingScroll = true;
        // Scrolling one moves the other to the same instant, not to the same
        // pixel: they measure from different origins and may be at different
        // scales, so a raw pixel is not a place they both understand.  Only
        // for a scroll the user made: while playing the follow owns both, and
        // this cannot tell the two apart.
        if (!playing && !sourceEditActive && timelineX != lastTimelineX)
            pianoViewport.setViewPosition(
                std::max(0, pianoRoll.pixelForSeconds(
                    timeline.secondsForPixel(timelineX))),
                pianoViewport.getViewPositionY());
        else if (!playing && !sourceEditActive && pianoX != lastPianoX)
            timelineViewport.setViewPosition(
                std::max(0, timeline.pixelForSeconds(
                    pianoRoll.secondsForPixel(pianoX))),
                timelineViewport.getViewPositionY());
        lastTimelineX = timelineViewport.getViewPositionX();
        lastPianoX = pianoViewport.getViewPositionX();
        const auto timelineY = timelineViewport.getViewPositionY();
        const auto trackY = trackViewport.getViewPositionY();
        if (timelineY != lastTimelineY)
            trackViewport.setViewPosition(0, timelineY);
        else if (trackY != lastTrackY)
            timelineViewport.setViewPosition(timelineViewport.getViewPositionX(), trackY);
        lastTimelineY = timelineViewport.getViewPositionY();
        lastTrackY = trackViewport.getViewPositionY();
        syncingScroll = false;
    }
}

std::optional<double> MainComponent::viewSecondsLeavingSourceEdit(
    bool wasEnabled, bool nowEnabled, double timelineSeconds)
{
    if (nowEnabled || !wasEnabled) return {};
    return std::max(0.0, timelineSeconds);
}

void MainComponent::setSourceEditMode(bool enabled)
{
    playWhenRenderReady = false;
    const auto wasEnabled = sourceEditActive;
    sourceEditActive = enabled;
    if (enabled && selectedClipId.isEmpty())
    {
        const auto data = project.snapshot();
        for (const auto& track : data.tracks)
            if (!track.clips.empty())
            {
                selectedClipId = track.clips.front().id;
                break;
            }
    }
    pianoRoll.setSourceEditMode(enabled);
    pianoRoll.setFocusedClip(selectedClipId);
    if (enabled)
    {
        const auto data = project.snapshot();
        for (const auto& track : data.tracks)
            for (const auto& clip : track.clips)
                if (clip.id == selectedClipId)
                {
                    audio.setAuditionFile(clip.sourceFile);
                    pianoViewport.setViewPosition(std::max(0, pianoRoll.pixelForSeconds(clip.sourceOffsetSeconds)
                                                              - pianoViewport.getViewWidth() / 4),
                                                  pianoViewport.getViewPositionY());
                }
        loadSampleSettings();
    }
    else
    {
        audio.clearAuditionFile();
        // Coming back from source edit, the roll goes to whatever moment the
        // timeline is showing.  This used to copy the timeline's pixel column
        // straight across, and the two run at different pixels per second, so
        // it landed somewhere else -- and it ran even when source edit had not
        // been on, which both the loudness lane and the point tool ask for on
        // every click.  That is the view sliding for no reason.
        if (const auto seconds = viewSecondsLeavingSourceEdit(
                wasEnabled, enabled,
                timeline.secondsForPixel(timelineViewport.getViewPositionX())))
            pianoViewport.setViewPosition(
                std::max(0, pianoRoll.pixelForSeconds(*seconds)),
                pianoViewport.getViewPositionY());
    }
    noteEditButton.setToggleState(!enabled, juce::dontSendNotification);
    wrenchButton.setToggleState(enabled, juce::dontSendNotification);
    sourceEditHint.setVisible(enabled);
    resized();
}

void MainComponent::focusClip(const juce::String& clipId)
{
    selectedClipId = clipId;
    pianoRoll.setFocusedClip(clipId);
    const auto data = project.snapshot();
    const ClipData* selectedClip = nullptr;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == clipId)
            {
                selectedClip = &clip;
                selectedTrackId = track.id;
                trackList.setSelectedTrack(track.id);
                pianoRoll.setFocusedTrack(track.id);
                refreshProjectControls();
                break;
            }

    if (!sourceEditActive) return;
    if (selectedClip == nullptr)
    {
        audio.clearAuditionFile();
        return;
    }
    audio.setAuditionFile(selectedClip->sourceFile);
    pianoViewport.setViewPosition(std::max(0,
        pianoRoll.pixelForSeconds(selectedClip->sourceOffsetSeconds)
            - pianoViewport.getViewWidth() / 4), pianoViewport.getViewPositionY());
    loadSampleSettings();
}

void MainComponent::focusNote(const juce::String& noteId)
{
    selectedNoteId = noteId;
    if (noteId.isNotEmpty())
    {
        const auto data = project.snapshot();
        for (const auto& track : data.tracks)
            for (const auto& clip : track.clips)
                if (std::any_of(clip.notes.begin(), clip.notes.end(),
                    [&noteId](const auto& note) { return note.id == noteId; }))
                {
                    focusClip(clip.id);
                    refreshSelectedNoteParameter();
                    return;
                }
    }
    refreshSelectedNoteParameter();
}

void MainComponent::updateUtauRenderSelection()
{
    const auto noteIds = pianoRoll.selectedNoteIds();
    activeUtauSelectionCount = static_cast<int>(noteIds.size());
    audio.setUtauRenderNoteSelection(noteIds);
    // mouseDown clears the visual selection before a new marquee is finished.
    // Do not rebuild an all-history phrase at that transient point; Space with
    // no selection explicitly requests history playback in togglePlayback().
    if (noteIds.empty()) return;
    const auto data = project.snapshot();
    syncAudio(data);
    if (audio.isPlaying()) return;

    std::unordered_set<std::string> validNoteIds;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                validNoteIds.insert(note.id.toStdString());
    accumulatedUtauNoteIds.clear();
    for (const auto& id : noteIds)
        if (validNoteIds.contains(id.toStdString()))
            accumulatedUtauNoteIds.insert(id.toStdString());
    std::optional<double> firstSelectedSeconds;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (accumulatedUtauNoteIds.contains(note.id.toStdString()))
                {
                    const auto start = clip.startSeconds + note.startSeconds;
                    firstSelectedSeconds = firstSelectedSeconds
                        ? std::min(*firstSelectedSeconds, start) : start;
                }
    if (firstSelectedSeconds)
        audio.setPosition(std::max(0.0, *firstSelectedSeconds - 0.05));
}

void MainComponent::loadSampleSettings()
{
    sampleSettingsFile = juce::File{};
    const auto data = project.snapshot();
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == selectedClipId)
                sampleSettingsFile = clip.sourceFile;
    sampleSettingsRows = sampleSettingsFile.existsAsFile()
        ? SampleSettings::loadOrDerive(sampleSettingsFile, data)
        : std::vector<SampleRegionSetting>{};
    activeSampleSetting = 0;
    sampleRegionSelector.clear(juce::dontSendNotification);
    for (std::size_t index = 0; index < sampleSettingsRows.size(); ++index)
        sampleRegionSelector.addItem(juce::String(index + 1) + utf8(" · ")
                                     + sampleSettingsRows[index].name,
                                     static_cast<int>(index + 1));
    if (!sampleSettingsRows.empty())
        sampleRegionSelector.setSelectedId(1, juce::dontSendNotification);
    pianoRoll.setSampleRegions(sampleSettingsRows, activeSampleSetting);
    refreshSampleEditors();
}

void MainComponent::refreshSampleEditors()
{
    const auto enabled = activeSampleSetting >= 0
        && activeSampleSetting < static_cast<int>(sampleSettingsRows.size());
    for (auto* editor : { &sampleAliasEditor, &sampleStartEditor, &sampleEndEditor,
                          &sampleAlignmentEditor, &sampleFixedEditor }) editor->setEnabled(enabled);
    if (!enabled)
    {
        for (auto* editor : { &sampleAliasEditor, &sampleStartEditor, &sampleEndEditor,
                              &sampleAlignmentEditor, &sampleFixedEditor }) editor->clear();
        return;
    }
    const auto& row = sampleSettingsRows[static_cast<std::size_t>(activeSampleSetting)];
    sampleAliasEditor.setText(row.name, false);
    sampleStartEditor.setText(juce::String(row.regionStartSeconds, 4), false);
    sampleEndEditor.setText(juce::String(row.regionEndSeconds, 4), false);
    sampleAlignmentEditor.setText(juce::String(row.alignmentSeconds, 4), false);
    sampleFixedEditor.setText(juce::String(row.fixedDurationSeconds, 4), false);
    pianoRoll.setSampleRegions(sampleSettingsRows, activeSampleSetting);
}

void MainComponent::commitSampleEditors()
{
    if (activeSampleSetting < 0
        || activeSampleSetting >= static_cast<int>(sampleSettingsRows.size())) return;
    auto& row = sampleSettingsRows[static_cast<std::size_t>(activeSampleSetting)];
    row.name = sampleAliasEditor.getText().trim();
    row.regionStartSeconds = std::max(0.0, sampleStartEditor.getText().getDoubleValue());
    row.regionEndSeconds = std::max(row.regionStartSeconds + 0.001,
                                    sampleEndEditor.getText().getDoubleValue());
    row.alignmentSeconds = juce::jlimit(row.regionStartSeconds, row.regionEndSeconds,
                                        sampleAlignmentEditor.getText().getDoubleValue());
    row.fixedDurationSeconds = juce::jlimit(0.0, row.regionEndSeconds - row.regionStartSeconds,
                                            sampleFixedEditor.getText().getDoubleValue());
    sampleRegionSelector.changeItemText(activeSampleSetting + 1,
        juce::String(activeSampleSetting + 1) + " · " + row.name);
    pianoRoll.setSampleRegions(sampleSettingsRows, activeSampleSetting);
}

void MainComponent::saveSampleSettings()
{
    commitSampleEditors();
    if (!sampleSettingsFile.existsAsFile() || sampleSettingsRows.empty()) return;
    juce::String error;
    if (!SampleSettings::save(sampleSettingsFile, sampleSettingsRows, error))
    {
        showError(error);
        return;
    }
    project.applySourceSettings(sampleSettingsFile, sampleSettingsRows);
    statusLabel.setText(strings.text("sample.saved") + "  "
                        + SampleSettings::sidecarFor(sampleSettingsFile).getFullPathName(),
                        juce::dontSendNotification);
}

void MainComponent::importOto()
{
    chooser = std::make_unique<juce::FileChooser>(strings.text("sample.importOto"),
                                                   sampleSettingsFile.getParentDirectory(),
                                                   "oto.ini;*.ini");
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (file == juce::File{}) return;
            juce::String error;
            const auto duration = audio.probeDuration(sampleSettingsFile).value_or(0.0);
            if (!SampleSettings::importOto(file, sampleSettingsFile, duration,
                                           sampleSettingsRows, error))
            {
                showError(error);
                return;
            }
            activeSampleSetting = 0;
            sampleRegionSelector.clear(juce::dontSendNotification);
            for (std::size_t index = 0; index < sampleSettingsRows.size(); ++index)
                sampleRegionSelector.addItem(juce::String(index + 1) + utf8(" · ")
                    + sampleSettingsRows[index].name, static_cast<int>(index + 1));
            sampleRegionSelector.setSelectedId(1, juce::dontSendNotification);
            pianoRoll.setSampleRegions(sampleSettingsRows, activeSampleSetting);
            refreshSampleEditors();
        });
}

void MainComponent::exportOto()
{
    commitSampleEditors();
    if (!sampleSettingsFile.existsAsFile() || sampleSettingsRows.empty()) return;
    chooser = std::make_unique<juce::FileChooser>(strings.text("sample.exportOto"),
        sampleSettingsFile.getParentDirectory().getChildFile("oto.ini"), "*.ini");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& selected)
        {
            auto file = selected.getResult();
            if (file == juce::File{}) return;
            if (!file.hasFileExtension("ini")) file = file.withFileExtension("ini");
            const auto duration = audio.probeDuration(sampleSettingsFile).value_or(0.0);
            juce::String error;
            if (!SampleSettings::exportOto(file, sampleSettingsFile, sampleSettingsRows,
                                           duration, error)) showError(error);
        });
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { strings.text("menu.file"), strings.text("menu.edit"), strings.text("menu.track"),
             strings.text("menu.view"), strings.text("menu.help") };
}

juce::PopupMenu MainComponent::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu menu;
    if (index == 0)
    {
        menu.addItem(1, strings.text("file.new"));
        menu.addItem(2, strings.text("file.open"));
        menu.addItem(3, strings.text("file.save"));
        menu.addItem(11, strings.text("file.saveAs"));
        juce::PopupMenu recent;
        for (int recentIndex = 0; recentIndex < recentProjectPaths.size(); ++recentIndex)
        {
            const juce::File file(recentProjectPaths[recentIndex]);
            recent.addItem(1'000 + recentIndex,
                file.getFileNameWithoutExtension() + "  —  "
                    + file.getParentDirectory().getFullPathName(),
                file.existsAsFile());
        }
        if (recentProjectPaths.isEmpty())
            recent.addItem(999, strings.text("file.recentEmpty"), false);
        menu.addSubMenu(strings.text("file.recent"), recent);
        menu.addItem(8, strings.text("file.export"));
        menu.addItem(12, strings.text("file.exportLastRender"),
                     !lastRenderedNoteIds.empty());
        menu.addSeparator();
        menu.addItem(4, strings.text("file.audio"));
        menu.addItem(5, strings.text("file.melodyne"));
        menu.addItem(6, strings.text("file.midi"));
        menu.addItem(13, strings.text("file.ust"));
        menu.addSeparator();
        menu.addItem(9, strings.text("file.settings"));
        menu.addItem(10, strings.text("file.assets"));
        menu.addSeparator();
        menu.addItem(7, strings.text("file.exit"));
    }
    else if (index == 1)
    {
        menu.addItem(20, strings.text("edit.undo"), project.canUndo());
        menu.addItem(21, strings.text("edit.redo"), project.canRedo());
        menu.addSeparator();
        menu.addItem(22, strings.text("edit.selectAll"));
        menu.addItem(26, strings.text("edit.deselect"));
        menu.addSeparator();
        const auto hasNotes = !pianoRoll.selectedNoteIds().empty();
        menu.addItem(16, strings.text("edit.copyNotes"), hasNotes);
        menu.addItem(17, strings.text("edit.cutNotes"), hasNotes);
        menu.addItem(51, strings.text("edit.pasteNotesAtOrigin"), !copiedNotes.empty());
        menu.addItem(18, strings.text("edit.pasteNotes"), !copiedNotes.empty());
        menu.addSeparator();
        menu.addItem(27, strings.text("edit.transposeCents"), hasNotes);
        menu.addItem(28, strings.text("edit.setPitch"), hasNotes);
        menu.addItem(29, strings.text("edit.averagePitch"), hasNotes);
        menu.addItem(19, strings.text("edit.quantizePitch"), hasNotes);
        menu.addSeparator();
        menu.addItem(23, strings.text("edit.copyClip"), selectedClipId.isNotEmpty());
        menu.addItem(24, strings.text("edit.pasteClip"), copiedClipId.isNotEmpty());
        menu.addItem(25, strings.text("edit.duplicateClip"), selectedClipId.isNotEmpty());
    }
    else if (index == 2)
    {
        menu.addItem(36, strings.text("track.addCompose"));
        menu.addItem(37, strings.text("track.addAudio"));
        menu.addItem(30, strings.text("file.audio"));
        menu.addSeparator();
        menu.addItem(38, strings.text("track.rename"), selectedTrackId.isNotEmpty());
        menu.addItem(31, strings.text("track.toggleCompose"), selectedTrackId.isNotEmpty());
        menu.addSeparator();
        auto selectedClipMuted = false;
        if (selectedClipId.isNotEmpty())
        {
            const auto data = project.snapshot();
            for (const auto& track : data.tracks)
                for (const auto& clip : track.clips)
                    if (clip.id == selectedClipId) selectedClipMuted = clip.muted;
        }
        menu.addItem(34, strings.text(selectedClipMuted ? "clip.unmute" : "clip.mute"),
                     selectedClipId.isNotEmpty());
        menu.addItem(35, strings.text("clip.gain"), selectedClipId.isNotEmpty());
        menu.addSeparator();
        menu.addItem(32, strings.text("track.delete"), selectedTrackId.isNotEmpty());
        menu.addItem(33, strings.text("clip.delete"), selectedClipId.isNotEmpty());
    }
    else if (index == 3)
    {
        menu.addItem(40, strings.text("view.zoomIn"));
        menu.addItem(41, strings.text("view.zoomOut"));
        menu.addItem(42, strings.text("view.zoomFit"));
        menu.addItem(44, strings.text("view.vZoomIn"));
        menu.addItem(45, strings.text("view.vZoomOut"));
        menu.addSeparator();
        menu.addItem(43, strings.text("view.showWaveforms"), true, showWaveforms);
    }
    else
        menu.addItem(50, strings.text("help.about"));
    return menu;
}

void MainComponent::menuItemSelected(int id, int)
{
    if (id == 1) newProject();
    else if (id == 2) openProject();
    else if (id == 3) saveProject();
    else if (id == 11) saveProjectAs();
    else if (id == 8) exportMixdown();
    else if (id == 12) exportLastRender();
    else if (id == 4 || id == 30) importAudio();
    else if (id == 5) importMelodyne();
    else if (id == 6) importMidi();
    else if (id == 13) importUst();
    else if (id == 9) showSettings();
    else if (id == 10) showAssetManager();
    else if (id == 7) requestClose([] { juce::JUCEApplication::getInstance()->quit(); });
    else if (id >= 1'000 && id < 1'000 + recentProjectPaths.size())
    {
        const juce::File file(recentProjectPaths[id - 1'000]);
        performWithUnsavedCheck([this, file] { loadProjectFile(file); });
    }
    else if (id == 20) project.undo();
    else if (id == 21) project.redo();
    else if (id == 22) pianoRoll.selectAllNotes();
    else if (id == 26) pianoRoll.clearNoteSelection();
    else if (id == 16) copySelectedNotes(false);
    else if (id == 17) copySelectedNotes(true);
    else if (id == 18) pasteCopiedNotes();
    else if (id == 27) showTransposeNotesDialog();
    else if (id == 28) showSetNotesPitchDialog();
    else if (id == 29) project.averageNotesMidi(pianoRoll.selectedNoteIds());
    else if (id == 19) project.quantizeNotesMidi(pianoRoll.selectedNoteIds());
    else if (id == 51) pasteCopiedNotes(copiedOriginSeconds);
    else if (id == 23) copySelectedClip();
    else if (id == 24) pasteCopiedClip();
    else if (id == 25) duplicateSelectedClip();
    else if (id == 36 || id == 37) addTrackFromMenu(id == 36);
    else if (id == 38) showRenameTrackDialog();
    else if (id == 31)
    {
        const auto data = project.snapshot();
        const auto found = std::find_if(data.tracks.begin(), data.tracks.end(),
            [this](const auto& track) { return track.id == selectedTrackId; });
        if (found != data.tracks.end()) project.setTrackCompose(found->id, !found->compose);
    }
    else if (id == 32) deleteSelectedTrack();
    else if (id == 33) deleteSelectedClip();
    else if (id == 34)
    {
        const auto data = project.snapshot();
        for (const auto& track : data.tracks)
            for (const auto& clip : track.clips)
                if (clip.id == selectedClipId)
                {
                    project.setClipMuted(clip.id, !clip.muted);
                    return;
                }
    }
    else if (id == 35) showClipGainDialog();
    else if (id == 40) zoomSlider.setValue(zoomSlider.getValue() * 1.25);
    else if (id == 41) zoomSlider.setValue(zoomSlider.getValue() / 1.25);
    else if (id == 42)
    {
        const auto available = std::max(200, timelineViewport.getWidth());
        zoomSlider.setValue(static_cast<double>(available) / std::max(1.0, project.snapshot().durationSeconds()));
    }
    else if (id == 43)
    {
        showWaveforms = !showWaveforms;
        pianoRoll.setShowWaveforms(showWaveforms);
        if (preferences != nullptr)
            preferences->setValue("ui.showWaveforms", showWaveforms);
        menuItemsChanged();
    }
    else if (id == 44) vZoomSlider.setValue(vZoomSlider.getValue() * 1.2);
    else if (id == 45) vZoomSlider.setValue(vZoomSlider.getValue() / 1.2);
    else if (id == 50)
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
            strings.text("app.title"), strings.text("help.aboutText"));
}

void MainComponent::showSettings()
{
    if (preferences == nullptr) return;
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = strings.text("settings.title");
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    juce::Component::SafePointer<MainComponent> safe(this);
    options.content.setOwned(new SettingsComponent(strings, audio.devices(), *preferences,
        [safe]
        {
            if (safe == nullptr) return;
            if (safe->preferences != nullptr)
                safe->audio.saveDeviceState(*safe->preferences);
            safe->applyPreferences();
            safe->refreshTexts();
            safe->menuItemsChanged();
            safe->repaint();
            safe->trackList.repaint();
            safe->timeline.repaint();
            safe->pianoRoll.repaint();
        }));
    options.launchAsync();
}

void MainComponent::showAssetManager()
{
    if (preferences == nullptr) return;
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = strings.text("asset.title");
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.content.setOwned(new AssetManagerComponent(strings, *preferences));
    options.launchAsync();
}

void MainComponent::showClipGainDialog()
{
    if (selectedClipId.isEmpty()) return;
    auto gain = 1.0f;
    auto found = false;
    const auto data = project.snapshot();
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == selectedClipId)
            {
                gain = clip.gain;
                found = true;
            }
    if (!found) return;

    const auto gainDb = gain > 1.0e-6f ? 20.0 * std::log10(gain) : -60.0;
    auto* dialog = new juce::AlertWindow(strings.text("clip.gain"), juce::String{},
                                          juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("gain", juce::String(gainDb, 1), strings.text("clip.gainDb"));
    dialog->addButton(strings.text("dialog.apply"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    const auto clipId = selectedClipId;
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create([safe, dialog, clipId](int result)
        {
            if (safe != nullptr && result == 1)
            {
                const auto db = juce::jlimit(-60.0, 12.0,
                    dialog->getTextEditorContents("gain").getDoubleValue());
                safe->project.setClipGain(clipId, db <= -59.9 ? 0.0f
                    : static_cast<float>(std::pow(10.0, db / 20.0)));
            }
            delete dialog;
        }), false);
}

void MainComponent::showRenameTrackDialog()
{
    if (selectedTrackId.isEmpty()) return;
    auto currentName = juce::String{};
    for (const auto& track : project.snapshot().tracks)
        if (track.id == selectedTrackId)
        {
            currentName = track.name;
            break;
        }
    if (currentName.isEmpty()) return;
    auto* dialog = new juce::AlertWindow(strings.text("track.rename"), juce::String{},
                                          juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", currentName, strings.text("track.name"));
    dialog->addButton(strings.text("dialog.apply"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    const auto trackId = selectedTrackId;
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create([safe, dialog, trackId](int result)
        {
            if (safe != nullptr && result == 1)
                safe->project.setTrackName(trackId,
                    dialog->getTextEditorContents("name"));
            delete dialog;
        }), false);
}

void MainComponent::showTransposeNotesDialog()
{
    const auto noteIds = pianoRoll.selectedNoteIds();
    if (noteIds.empty()) return;
    auto* dialog = new juce::AlertWindow(strings.text("edit.transposeCents"), juce::String{},
                                          juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("cents", "0", strings.text("edit.cents"));
    dialog->addButton(strings.text("dialog.apply"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create([safe, dialog, noteIds](int result)
        {
            if (safe != nullptr && result == 1)
            {
                const auto cents = juce::jlimit(-4'800.0, 4'800.0,
                    dialog->getTextEditorContents("cents").getDoubleValue());
                safe->project.transposeNotes(noteIds, static_cast<float>(cents / 100.0));
            }
            delete dialog;
        }), false);
}

void MainComponent::showSetNotesPitchDialog()
{
    const auto noteIds = pianoRoll.selectedNoteIds();
    if (noteIds.empty()) return;
    auto initial = 60.0f;
    auto found = false;
    for (const auto& track : project.snapshot().tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end())
                {
                    initial = note.midiNote;
                    found = true;
                    break;
                }
    if (!found) return;
    auto* dialog = new juce::AlertWindow(strings.text("edit.setPitch"), juce::String{},
                                          juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("midi", juce::String(initial, 2), strings.text("edit.midiNote"));
    dialog->addButton(strings.text("dialog.apply"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create([safe, dialog, noteIds](int result)
        {
            if (safe != nullptr && result == 1)
                safe->project.setNotesMidi(noteIds, static_cast<float>(
                    dialog->getTextEditorContents("midi").getDoubleValue()));
            delete dialog;
        }), false);
}

void MainComponent::copySelectedNotes(bool cut)
{
    const auto ids = pianoRoll.selectedNoteIds();
    if (ids.empty()) return;
    std::vector<std::pair<double, NoteData>> notes;
    for (const auto& track : project.snapshot().tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (std::find(ids.begin(), ids.end(), note.id) != ids.end())
                    notes.emplace_back(clip.startSeconds + note.startSeconds, note);
    if (notes.empty()) return;
    std::stable_sort(notes.begin(), notes.end(), [](const auto& left, const auto& right)
    {
        return left.first < right.first;
    });
    const auto origin = notes.front().first;
    copiedOriginSeconds = origin;
    copiedTrackId.clear();
    for (const auto& track : project.snapshot().tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (std::find(ids.begin(), ids.end(), note.id) != ids.end())
                    copiedTrackId = track.id;
    copiedClipId.clear();
    copiedNotes.clear();
    copiedNotes.reserve(notes.size());
    for (auto& [absolute, note] : notes)
    {
        note.startSeconds = absolute - origin;
        copiedNotes.push_back(std::move(note));
    }
    if (cut)
    {
        project.removeNotes(ids);
        pianoRoll.clearNoteSelection();
    }
    statusLabel.setText(strings.text(cut ? "status.notesCut" : "status.notesCopied"),
                        juce::dontSendNotification);
    menuItemsChanged();
}

double MainComponent::pasteTargetSeconds(bool sameTrack, double playheadSeconds,
                                         double originSeconds,
                                         std::optional<double> selectionStartSeconds,
                                         std::optional<double> askedFor,
                                         std::optional<double> pointerSeconds)
{
    if (askedFor) return std::max(0.0, *askedFor);
    // Wherever the pointer is, on either track: pointing at a place is the
    // plainest way of naming one, and it is what a paste is aimed at.
    if (pointerSeconds) return std::max(0.0, *pointerSeconds);
    if (!sameTrack) return std::max(0.0, originSeconds);
    // On its own track a paste goes in front of what is selected, which is
    // what having selected it means; with nothing selected, the playhead.
    return std::max(0.0, selectionStartSeconds.value_or(playheadSeconds));
}

juce::String MainComponent::pasteTargetTrack() const
{
    // The track in front, since that is the one being pasted onto.
    const auto data = project.snapshot();
    for (const auto& track : data.tracks)
        if (track.id == selectedTrackId) return track.id;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == selectedClipId) return track.id;
    return data.tracks.empty() ? juce::String() : data.tracks.front().id;
}

MainComponent::PasteClipPlan MainComponent::pasteClipPlanFor(
    const ProjectData& data, const juce::String& trackId, double atSeconds)
{
    PasteClipPlan plan;
    plan.clipId = pasteTargetClipIn(data, trackId, atSeconds);
    if (plan.clipId.isNotEmpty()) return plan;

    // Only a melodic track can be given one: an audio track has nothing for
    // notes to sound through.
    for (const auto& track : data.tracks)
        if (track.id == trackId && track.compose && track.clips.empty())
        {
            const auto beat = 60.0 / juce::jlimit(20.0, 400.0, data.bpm);
            const auto bar = beat * static_cast<double>(data.numerator) * 4.0
                / static_cast<double>(std::max(1, data.denominator));
            plan.makeOne = true;
            plan.startSeconds = 0.0;
            plan.seedSeconds = std::max(0.05, bar);
        }
    return plan;
}

juce::String MainComponent::pasteTargetClipIn(const ProjectData& data,
                                              const juce::String& trackId,
                                              double atSeconds)
{
    for (const auto& track : data.tracks)
    {
        if (track.id != trackId || track.clips.empty()) continue;
        for (const auto& clip : track.clips)
            if (atSeconds >= clip.startSeconds - 1.0e-9
                && atSeconds < clip.startSeconds + clip.durationSeconds)
                return clip.id;
        return track.clips.front().id;
    }
    return {};
}

void MainComponent::pasteCopiedNotesInto(const juce::String& clipId, double atSeconds,
                                         const std::vector<juce::String>& replacing)
{
    // Replaced notes go first, then the pasted ones land at exactly the times
    // they were given.  Nothing else in the clip is touched: overwriting is
    // meant to swap a stretch out, not to renumber the piece around it.
    if (!replacing.empty()) project.removeNotes(replacing);
    const auto inserted = project.insertNotes(clipId, copiedNotes, atSeconds);
    if (inserted.empty())
    {
        // Silence here read as a broken paste.  It happens where the clip is a
        // recording and the target is past the end of it: there is no audio
        // out there for a note to sound through.
        statusLabel.setText(utf8("这里放不下：录音素材到此为止，音符只能落在它的范围内。"),
                            juce::dontSendNotification);
        return;
    }
    pianoRoll.setSelectedNoteIds(inserted);
    focusNote(inserted.front());
    statusLabel.setText(strings.text("status.notesPasted"),
                        juce::dontSendNotification);
    menuItemsChanged();
}

void MainComponent::pasteCopiedNotes(std::optional<double> atSeconds)
{
    if (copiedNotes.empty()) return;
    const auto data = project.snapshot();
    const auto trackId = pasteTargetTrack();
    const auto sameTrack = trackId.isNotEmpty() && trackId == copiedTrackId;
    // Where the selection begins, if anything is selected.
    std::optional<double> selectionStart;
    {
        const auto ids = pianoRoll.selectedNoteIds();
        for (const auto& track : data.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (std::find(ids.begin(), ids.end(), note.id) != ids.end())
                    {
                        const auto start = clip.startSeconds + note.startSeconds;
                        selectionStart = selectionStart
                            ? std::min(*selectionStart, start) : start;
                    }
    }
    const auto target = pasteTargetSeconds(sameTrack, audio.position(),
                                           copiedOriginSeconds, selectionStart,
                                           atSeconds, pianoRoll.pasteAnchorSeconds());
    const auto plan = pasteClipPlanFor(data, trackId, target);
    auto clipId = plan.clipId;
    if (plan.makeOne)
        clipId = project.addClip(trackId, plan.startSeconds, plan.seedSeconds);
    if (clipId.isEmpty())
    {
        // Nowhere to put them: an audio track has nothing for notes to sound
        // through, and going quiet read as a broken paste.
        statusLabel.setText(utf8("这条轨道放不下音符：请选择一条旋律轨道。"),
                            juce::dontSendNotification);
        return;
    }

    // On its own track a paste goes in, not over: what it lands on and
    // everything after it move along by the length of what was pasted, and the
    // piece gets that much longer.  Inserting already knows how to do that;
    // clearing the way first is what stopped it, since there was then nothing
    // left for it to push.  Carrying a phrase to another track is the case
    // where replacing is meant, and that still asks first.
    if (sameTrack)
    {
        pasteCopiedNotesInto(clipId, target, {});
        return;
    }

    // How far the pasted block reaches, so what it lands on can be found.
    auto extent = 0.0;
    for (const auto& note : copiedNotes)
        extent = std::max(extent, note.startSeconds + note.durationSeconds);
    const auto replacing = project.notesOverlapping(clipId, target, target + extent);
    if (replacing.empty())
    {
        pasteCopiedNotesInto(clipId, target, {});
        return;
    }
    juce::Component::SafePointer<MainComponent> safe(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon,
        utf8("粘贴音符"),
        utf8("这一段和轨道上已有的 ") + juce::String(static_cast<int>(replacing.size()))
            + utf8(" 个音符重叠。\n\n继续将删除这些音符并粘贴覆盖；"
                   "其余音符的位置不受影响。"),
        utf8("覆盖粘贴"), utf8("取消"), this,
        juce::ModalCallbackFunction::create(
            [safe, clipId, target, replacing](int result)
            {
                if (safe == nullptr || result == 0) return;
                safe->pasteCopiedNotesInto(clipId, target, replacing);
            }));
}

void MainComponent::copySelectedClip()
{
    if (selectedClipId.isEmpty()) return;
    const auto data = project.snapshot();
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == selectedClipId)
            {
                copiedNotes.clear();
                copiedClipId = clip.id;
                selectedTrackId = track.id;
                statusLabel.setText(strings.text("status.clipCopied"),
                                    juce::dontSendNotification);
                menuItemsChanged();
                return;
            }
}

MainComponent::ClipPasteTarget MainComponent::clipPasteTargetFor(
    const juce::String& pointerTrackId, std::optional<double> pointerSeconds,
    const juce::String& trackInHand, double playheadSeconds,
    double sourceStartSeconds, double sourceDurationSeconds)
{
    if (pointerTrackId.isNotEmpty() && pointerSeconds)
        return { pointerTrackId, std::max(0.0, *pointerSeconds) };
    auto seconds = std::max(0.0, playheadSeconds);
    // Not past the original yet: land right after it, so pressing paste again
    // and again lays copies end to end instead of on top of one another.
    if (seconds <= sourceStartSeconds + 1.0e-6)
        seconds = sourceStartSeconds + sourceDurationSeconds;
    return { trackInHand, std::max(0.0, seconds) };
}

void MainComponent::pasteCopiedClip()
{
    if (copiedClipId.isEmpty()) return;
    const auto data = project.snapshot();
    auto sourceStart = 0.0;
    auto sourceDuration = 0.0;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == copiedClipId)
            {
                sourceStart = clip.startSeconds;
                sourceDuration = clip.durationSeconds;
            }
    const auto anchor = timeline.pointerAnchor();
    const auto target = clipPasteTargetFor(
        anchor ? anchor->trackId : juce::String{},
        anchor ? std::optional<double>(anchor->seconds) : std::nullopt,
        selectedTrackId, audio.position(), sourceStart, sourceDuration);
    const auto inserted = project.duplicateClip(copiedClipId, target.seconds,
                                                target.trackId);
    if (inserted.isNotEmpty())
    {
        focusClip(inserted);
        statusLabel.setText(strings.text("status.clipPasted"),
                            juce::dontSendNotification);
    }
    else
    {
        copiedClipId.clear();
        menuItemsChanged();
    }
}

void MainComponent::duplicateSelectedClip()
{
    if (selectedClipId.isEmpty()) return;
    const auto inserted = project.duplicateClip(selectedClipId, -1.0,
                                                selectedTrackId);
    if (inserted.isNotEmpty())
    {
        copiedClipId = selectedClipId;
        focusClip(inserted);
        statusLabel.setText(strings.text("status.clipPasted"),
                            juce::dontSendNotification);
        menuItemsChanged();
    }
}

void MainComponent::confirmDestructive(const juce::String& title,
                                       std::function<void()> action)
{
    if (preferences == nullptr
        || !preferences->getBoolValue("operation.confirmDestructive", true))
    {
        if (action) action();
        return;
    }
    auto* dialog = new juce::AlertWindow(title, strings.text("dialog.destructiveMessage"),
                                          juce::MessageBoxIconType::WarningIcon);
    dialog->addButton(strings.text("dialog.delete"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, action = std::move(action)](int result) mutable
            {
                if (safe != nullptr && result == 1 && action) action();
                delete dialog;
            }), false);
}

void MainComponent::newProject()
{
    performWithUnsavedCheck([this]
    {
        audio.stop();
        audio.setPosition(0.0);
        project.clear();
        currentProjectFile = juce::File{};
        savedProjectRevision = project.revisionNumber();
        selectedTrackId.clear();
        selectedClipId.clear();
        selectedNoteId.clear();
        copiedClipId.clear();
        copiedNotes.clear();
        pianoRoll.clearNoteSelection();
        pianoRoll.setFocusedClip({});
        statusLabel.setText(strings.text("status.ready"), juce::dontSendNotification);
    });
}

void MainComponent::openProject()
{
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.open"), juce::File{}, "*.hjpx;*.hspx");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (file == juce::File{}) return;
            performWithUnsavedCheck([this, file] { loadProjectFile(file); });
        });
}

void MainComponent::loadProjectFile(const juce::File& file)
{
    juce::String error;
    // A project replacement invalidates every selection and any phrase render
    // derived from the previous document.  Stop transport before parsing and
    // clear those UI references immediately after a successful replacement,
    // before the asynchronous ChangeBroadcaster callbacks rebuild the views.
    audio.stop();
    if (!project.load(file, error))
    {
        showError(error);
        return;
    }
    audio.setPosition(0.0);
    audio.setUtauRenderNoteSelection({});
    activeUtauSelectionCount = 0;
    selectedTrackId.clear();
    selectedClipId.clear();
    selectedNoteId.clear();
    pianoRoll.clearNoteSelection();
    pianoRoll.setFocusedTrack({});
    pianoRoll.setFocusedClip({});
    currentProjectFile = file;
    savedProjectRevision = project.revisionNumber();
    addRecentProject(file);
    statusLabel.setText(strings.text("status.projectOpened") + "  " + file.getFileName(),
                        juce::dontSendNotification);
    if (error.isNotEmpty())
        showError(strings.text("warning.missingMedia") + "\n" + error);
}

void MainComponent::saveProject(std::function<void(bool)> completion)
{
    if (currentProjectFile != juce::File{})
    {
        const auto saved = saveProjectTo(currentProjectFile);
        if (completion) completion(saved);
        return;
    }
    saveProjectAs(std::move(completion));
}

void MainComponent::saveProjectAs(std::function<void(bool)> completion)
{
    const auto initial = currentProjectFile != juce::File{} ? currentProjectFile
        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(project.snapshot().name + ".hjpx");
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.saveAs"),
                                                   initial,
                                                   "*.hjpx");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, completion = std::move(completion)](const juce::FileChooser& selected) mutable
        {
            auto file = selected.getResult();
            if (file == juce::File{})
            {
                if (completion) completion(false);
                return;
            }
            if (!file.hasFileExtension("hjpx")) file = file.withFileExtension("hjpx");
            const auto saved = saveProjectTo(file);
            if (completion) completion(saved);
        });
}

bool MainComponent::saveProjectTo(const juce::File& file)
{
    juce::String error;
    if (!project.save(file, error))
    {
        showError(error);
        return false;
    }
    currentProjectFile = file;
    savedProjectRevision = project.revisionNumber();
    addRecentProject(file);
    statusLabel.setText(strings.text("status.projectSaved") + "  " + file.getFileName(),
                        juce::dontSendNotification);
    return true;
}

void MainComponent::performWithUnsavedCheck(std::function<void()> action)
{
    if (project.revisionNumber() == savedProjectRevision)
    {
        if (action) action();
        return;
    }
    auto* dialog = new juce::AlertWindow(strings.text("dialog.unsavedTitle"),
        strings.text("dialog.unsavedMessage"), juce::MessageBoxIconType::WarningIcon);
    dialog->addButton(strings.text("dialog.save"), 1);
    dialog->addButton(strings.text("dialog.discard"), 2);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create(
            [safe, dialog, action = std::move(action)](int result) mutable
            {
                if (safe != nullptr && result == 1)
                    safe->saveProject([safe, action = std::move(action)](bool saved) mutable
                    {
                        if (safe != nullptr && saved && action) action();
                    });
                else if (safe != nullptr && result == 2 && action)
                    action();
                delete dialog;
            }), false);
}

void MainComponent::requestClose(std::function<void()> approved)
{
    performWithUnsavedCheck(std::move(approved));
}

void MainComponent::rememberExportDirectory(const juce::File& directory)
{
    if (!directory.isDirectory() || directory == lastExportDirectory) return;
    lastExportDirectory = directory;
    if (preferences == nullptr) return;
    preferences->setValue("files.lastExportDirectory", directory.getFullPathName());
    preferences->saveIfNeeded();
}

void MainComponent::restoreRecentProjects()
{
    recentProjectPaths.clear();
    if (preferences == nullptr) return;
    // Read here rather than in applyPreferences(): this runs once, at startup,
    // which is when the folder an export should open in is wanted.  A folder
    // that has since been moved or removed is dropped, and the chooser falls
    // back to Documents.
    const juce::File exported(preferences->getValue("files.lastExportDirectory"));
    if (exported.isDirectory()) lastExportDirectory = exported;
    juce::StringArray stored;
    stored.addLines(preferences->getValue("files.recentProjects"));
    for (const auto& path : stored)
    {
        const juce::File file(path);
        if (file.existsAsFile() && !recentProjectPaths.contains(file.getFullPathName()))
            recentProjectPaths.add(file.getFullPathName());
        if (recentProjectPaths.size() >= 8) break;
    }
}

void MainComponent::addRecentProject(const juce::File& file)
{
    if (file == juce::File{}) return;
    const auto path = file.getFullPathName();
    recentProjectPaths.removeString(path, true);
    recentProjectPaths.insert(0, path);
    while (recentProjectPaths.size() > 8) recentProjectPaths.remove(8);
    if (preferences != nullptr)
    {
        preferences->setValue("files.recentProjects", recentProjectPaths.joinIntoString("\n"));
        preferences->saveIfNeeded();
    }
    menuItemsChanged();
}

juce::File MainComponent::exportStartFile(const juce::File& remembered,
                                          const juce::String& suggestedName)
{
    const auto folder = remembered.isDirectory()
        ? remembered
        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    return folder.getChildFile(juce::File::createLegalFileName(suggestedName) + ".wav");
}

std::vector<MainComponent::ExportTarget> MainComponent::exportTargets(
    const ProjectData& project, const juce::File& destination,
    const juce::String& onlyTrackId, const juce::String& untitledName)
{
    std::vector<ExportTarget> targets;
    const auto anySolo = std::any_of(project.tracks.begin(), project.tracks.end(),
                                     [](const auto& track) { return track.solo; });
    const auto stem = destination.getFileNameWithoutExtension();
    const auto folder = destination.getParentDirectory();
    for (const auto& track : project.tracks)
    {
        if (onlyTrackId.isNotEmpty() && track.id != onlyTrackId) continue;
        if (track.muted || (anySolo && !track.solo)) continue;
        if (track.clips.empty()) continue;
        auto name = track.name.trim();
        if (name.isEmpty()) name = untitledName;
        // One track was asked for by name, so it goes to the file that was
        // named.  A whole song needs one file each, and they are told apart by
        // the track they came from.
        const auto file = onlyTrackId.isNotEmpty()
            ? destination
            : folder.getChildFile(juce::File::createLegalFileName(
                  stem + " - " + name) + ".wav");
        targets.push_back({ track.id, name, file });
    }
    return targets;
}

void MainComponent::exportMixdown()
{
    const auto data = project.snapshot();
    juce::PopupMenu menu;
    menu.addItem(1, strings.text("export.allTracks"), !data.tracks.empty());
    juce::PopupMenu tracks;
    auto id = 2;
    for (const auto& track : data.tracks)
    {
        auto name = track.name.trim();
        if (name.isEmpty()) name = strings.text("export.untitledTrack");
        tracks.addItem(id++, name, !track.clips.empty());
    }
    menu.addSubMenu(strings.text("export.oneTrack"), tracks, !data.tracks.empty());
    // Where the File menu item was just clicked, rather than the corner of the
    // window.  The menu bar has closed by the time this runs, so the pointer is
    // the only record of where the choice was made; from a keyboard shortcut it
    // may be anywhere, and then the menu bar itself is the sensible anchor.
    const auto pointer = juce::Desktop::getInstance().getMainMouseSource()
                             .getScreenPosition().roundToInt();
    const auto overWindow = getScreenBounds().contains(pointer);
    const auto anchor = overWindow
        ? juce::Rectangle<int>(pointer.x, pointer.y, 1, 1)
        : menuBar.getScreenBounds().removeFromBottom(1);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(anchor),
        [this, data](int chosen)
        {
            if (chosen <= 0) return;
            if (chosen == 1) { chooseExportDestination({}); return; }
            const auto index = static_cast<std::size_t>(chosen - 2);
            if (index >= data.tracks.size()) return;
            chooseExportDestination(data.tracks[index].id);
        });
}

void MainComponent::chooseExportDestination(const juce::String& trackId)
{
    const auto data = project.snapshot();
    auto suggested = data.name;
    if (trackId.isNotEmpty())
        for (const auto& track : data.tracks)
            if (track.id == trackId && track.name.trim().isNotEmpty())
                suggested = data.name + " - " + track.name.trim();
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.export"),
        exportStartFile(lastExportDirectory, suggested), "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, trackId](const juce::FileChooser& selected)
        {
            auto file = selected.getResult();
            if (file == juce::File{}) return;
            if (!file.hasFileExtension("wav")) file = file.withFileExtension("wav");
            // Remember where it went, not whether it worked: coming back to the
            // same folder is what saves the walk down from the drive root, and
            // a failed write is the likeliest reason to come straight back.
            rememberExportDirectory(file.getParentDirectory());
            auto targets = exportTargets(project.snapshot(), file, trackId,
                                         strings.text("export.untitledTrack"));
            if (targets.empty())
            {
                showError(strings.text(trackId.isNotEmpty() ? "error.exportSilent"
                                                            : "error.exportEmpty"));
                return;
            }
            beginExport(std::move(targets), {}, {});
        });
}

void MainComponent::exportLastRender()
{
    if (lastRenderedNoteIds.empty() || lastRenderedSpan.getLength() <= 1.0e-6)
    {
        showError(strings.text("error.exportNoRender"));
        return;
    }
    const auto suggested = project.snapshot().name + " - "
        + strings.text("export.lastRenderSuffix");
    chooser = std::make_unique<juce::FileChooser>(
        strings.text("file.exportLastRender"),
        exportStartFile(lastExportDirectory, suggested), "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& selected)
        {
            auto file = selected.getResult();
            if (file == juce::File{}) return;
            if (!file.hasFileExtension("wav")) file = file.withFileExtension("wav");
            rememberExportDirectory(file.getParentDirectory());
            // One file, the whole mix, over the marquee's own stretch of the
            // timeline: what was heard, not one track of it.
            beginExport({ ExportTarget { {}, {}, file } }, lastRenderedNoteIds,
                        lastRenderedSpan);
        });
}

void MainComponent::beginExport(std::vector<ExportTarget> targets,
                                const std::vector<juce::String>& scopeNoteIds,
                                juce::Range<double> range)
{
    if (targets.empty()) return;
    const auto data = project.snapshot();
    pendingExport = std::move(targets);
    pendingExportRange = range;
    audio.stop();
    if (scopeNoteIds.empty())
    {
        // Everything in the song has to be rendered, not just whatever phrase
        // was last selected: UTAU rendering is selection-driven, so an export
        // that did not ask for the whole song would write out the last marquee
        // and silence everywhere else.
        if (const auto inScope = audio.selectEveryUtauNote(data); inScope > 0)
            activeUtauSelectionCount = inScope;
    }
    else
    {
        // The last render is exactly one marquee's worth, so the scope goes
        // back to that marquee and nothing else renders into the file.
        audio.setUtauRenderNoteSelection(scopeNoteIds);
        activeUtauSelectionCount = static_cast<int>(scopeNoteIds.size());
    }
    syncAudio(data);
    exportWaitingForRender = true;
    statusLabel.setText(strings.text("status.exportRendering"), juce::dontSendNotification);
    repaint();
}

void MainComponent::finishExport()
{
    exportWaitingForRender = false;
    const auto targets = std::exchange(pendingExport, {});
    const auto range = std::exchange(pendingExportRange, juce::Range<double>());
    statusLabel.setText(strings.text("status.exporting"), juce::dontSendNotification);
    repaint();
    auto written = 0;
    for (const auto& target : targets)
    {
        juce::String error;
        if (!audio.exportWav(target.file, error, target.trackId,
                             range.getStart(), range.getEnd()))
        {
            showError(strings.text("error.export") + "\n" + target.trackName
                      + "\n" + error);
            break;
        }
        ++written;
    }
    statusLabel.setText(written > 0
            ? strings.text("status.exportDone") + " " + juce::String(written) + " "
                  + strings.text("status.exportFiles")
            : strings.text("status.ready"),
        juce::dontSendNotification);
    // The audition scope was widened to the whole song to render it; hand it
    // back to whatever is selected in the roll.
    updateUtauRenderSelection();
}

void MainComponent::importAudio()
{
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.audio"), juce::File{},
                                                   "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::canSelectMultipleItems,
        [this](const juce::FileChooser& selected)
        {
            const auto files = selected.getResults();
            const auto startSeconds = audio.position();
            for (const auto& file : files)
                if (const auto duration = audio.probeDuration(file))
                    addAnalysedAudioFile(file, *duration, startSeconds);
                else
                    showError(strings.text("error.audio") + "\n" + file.getFullPathName());
        });
}

void MainComponent::addAnalysedAudioFile(const juce::File& file, double durationSeconds,
                                         double startSeconds,
                                         const juce::String& targetTrackId)
{
    const auto clipId = project.addAudioFile(file, durationSeconds, startSeconds, targetTrackId);
    const auto nativeSidecar = SampleSettings::sidecarFor(file);
    const juce::File legacySidecar(file.getFullPathName() + ".hachi.csv");
    // HJM/OTO data is authoritative.  Acoustic analysis must not overwrite
    // explicitly authored sample regions.
    if (!nativeSidecar.existsAsFile() && !legacySidecar.existsAsFile())
        scheduleAnalysis(file, clipId);
}

void MainComponent::scheduleAnalysis(const juce::File& file,
                                     const juce::String& clipId)
{
    juce::Component::SafePointer<MainComponent> safe(this);
    const auto analysisConfig = backend::AnalysisService::configFromProperties(preferences.get());
    ++pendingNativeAnalyses;
    nativeAnalysisProgress = 0.0;
    nativeAnalysisName = file.getFileName();
    statusLabel.setText(strings.text("status.analyzing") + "  " + file.getFileName(),
                        juce::dontSendNotification);
    std::thread([safe, file, clipId, analysisConfig]
    {
        juce::String error;
        auto result = backend::AnalysisService::analyse(file, analysisConfig, error,
            [safe, name = file.getFileName()](double value)
            {
                juce::MessageManager::callAsync([safe, name, value]
                {
                    if (safe == nullptr || safe->importInProgress) return;
                    safe->nativeAnalysisName = name;
                    safe->nativeAnalysisProgress = value;
                });
            });
        juce::MessageManager::callAsync(
            [safe, clipId, result = std::move(result), error]() mutable
            {
                if (safe == nullptr) return;
                const auto backendName = backend::AnalysisService::backendText(result.status);
                const auto inserted = safe->project.setClipNotesIfEmpty(
                    clipId, std::move(result.notes));
                safe->pendingNativeAnalyses = std::max(0, safe->pendingNativeAnalyses - 1);
                safe->nativeAnalysisProgress = inserted ? 1.0 : 0.0;
                if (inserted)
                    safe->statusLabel.setText(safe->strings.text("status.analysisComplete")
                                                + " · " + backendName,
                                              juce::dontSendNotification);
                else if (error.isNotEmpty())
                    safe->statusLabel.setText(safe->strings.text("status.analysisSkipped"),
                                              juce::dontSendNotification);
            });
    }).detach();
}

void MainComponent::importMelodyne()
{
    if (!backend::MelodyneProvider::nativeImportAvailable()
        && !backend::MelodyneProvider::experimentalSelfImportEnabled())
    {
        showError(backend::MelodyneProvider::statusText()
            + "\n当前 Melodyne 导入路径尚未启用。请使用受支持的 Melodyne 宿主接口。 ");
        return;
    }
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.melodyne"), juce::File{}, "*.mpd");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (file == juce::File{}) return;
            loadMelodyneFile(file);
        });
}

void MainComponent::importMidi()
{
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.midi"), juce::File{}, "*.mid;*.midi");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (file == juce::File{}) return;
            juce::String error;
            if (!project.addMidiFile(file, error))
                showError(strings.text("error.midi") + "\n" + error);
        });
}

void MainComponent::importUst()
{
    chooser = std::make_unique<juce::FileChooser>(strings.text("file.ust"),
                                                  juce::File{}, "*.ust");
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& selected)
        {
            const auto file = selected.getResult();
            if (file == juce::File{}) return;
            loadUstFile(file);
        });
}

void MainComponent::loadUstFile(const juce::File& file)
{
    juce::String error;
    juce::StringArray warnings;
    if (!project.addUstFile(file, error, warnings))
    {
        showError(strings.text("error.ust") + "\n" + error);
        return;
    }
    refreshProjectControls();
    menuItemsChanged();
    // A UST names a voicebank this application cannot resolve, and carries an
    // amplitude envelope it does not read.  Both are worth saying once, in the
    // status line, rather than leaving them to be discovered as a wrong sound.
    statusLabel.setText(warnings.isEmpty()
        ? strings.text("status.ustLoaded")
        : strings.text("status.ustLoaded") + "  |  " + warnings.joinIntoString("  |  "),
        juce::dontSendNotification);
}

void MainComponent::loadMelodyneFile(const juce::File& file)
{
    importInProgress = true;
    progress = 0.01;
    statusLabel.setText(strings.text("status.loading"), juce::dontSendNotification);
    juce::Component::SafePointer<MainComponent> safe(this);
    const auto recursiveMediaSearch = preferences == nullptr
        || preferences->getBoolValue("import.recursiveMedia", true);
    const auto preserveProjectEdits = preferences == nullptr
        || preferences->getBoolValue("import.preserveEdits", true);
    const auto reanalyseSourcePitch = preferences != nullptr
        && preferences->getIntValue("import.melodynePitchSource", 1) == 2;
    const auto analysisConfig = backend::AnalysisService::configFromProperties(preferences.get());
    juce::Thread::launch([safe, file, recursiveMediaSearch, preserveProjectEdits,
                          reanalyseSourcePitch, analysisConfig]
    {
        juce::String error;
        backend::MelodyneImportOptions options;
        options.recursiveMediaSearch = recursiveMediaSearch;
        options.preserveProjectEdits = preserveProjectEdits;
        auto imported = backend::MelodyneImporter::importProject(file, error,
            [safe, reanalyseSourcePitch](double value, const juce::String& stage)
            {
                const auto scaled = reanalyseSourcePitch ? value * 0.75 : value;
                juce::MessageManager::callAsync([safe, scaled, stage]
                {
                    if (safe == nullptr) return;
                    safe->progress = scaled;
                    safe->statusLabel.setText(safe->strings.text("status.loading") + "  "
                                                + safe->strings.text(juce::String("mpd.stage.") + stage),
                                              juce::dontSendNotification);
                });
            }, options);
        auto pitchReanalysed = false;
        backend::AnalysisStatus analysisStatus;
        if (imported && reanalyseSourcePitch)
        {
            juce::String pitchError;
            pitchReanalysed = backend::AnalysisService::reanalyseProjectSourcePitch(
                imported->project, analysisConfig, pitchError, [safe](double value)
                {
                    juce::MessageManager::callAsync([safe, value]
                    {
                        if (safe == nullptr) return;
                        safe->progress = 0.75 + value * 0.25;
                        safe->statusLabel.setText(
                            safe->strings.text("status.analyzing") + "  "
                                + safe->strings.text("mpd.stage.reanalyse_pitch"),
                            juce::dontSendNotification);
                    });
                }, &analysisStatus);
        }
        juce::MessageManager::callAsync([safe, imported = std::move(imported), error,
                                         reanalyseSourcePitch, pitchReanalysed,
                                         analysisStatus]() mutable
        {
            if (safe == nullptr) return;
            safe->importInProgress = false;
            safe->progress = 0.0;
            if (!imported)
            {
                safe->showError(safe->strings.text("error.mpd") + "\n" + error);
                return;
            }
            if (reanalyseSourcePitch)
            {
                safe->statusLabel.setText(safe->strings.text(
                    pitchReanalysed ? "status.analysisComplete" : "status.analysisSkipped"),
                    juce::dontSendNotification);
                safe->statusLabel.setText(safe->statusLabel.getText() + utf8(" · ")
                    + backend::AnalysisService::backendText(analysisStatus),
                    juce::dontSendNotification);
            }
            safe->presentMelodyneComposeSelection(std::move(*imported));
        });
    });
}

void MainComponent::presentMelodyneComposeSelection(backend::MelodyneImportResult imported)
{
    // Material annotations belong beside the audio in the project's existing
    // HJM sidecar format.  Seed them during the first MPD import so later
    // audio/UTAU workflows read the same timing data through SampleSettings.
    for (const auto& track : imported.project.tracks)
        for (const auto& clip : track.clips)
        {
            if (!clip.sourceFile.existsAsFile() || clip.notes.empty()) continue;
            std::vector<SampleRegionSetting> rows;
            rows.reserve(clip.notes.size());
            const auto sourceAtTarget = [&clip](double target) {
                if (clip.sourceTimeMap.empty())
                    return clip.sourceOffsetSeconds + target;
                const auto& map = clip.sourceTimeMap;
                if (target <= map.front().targetSeconds) return map.front().sourceSeconds;
                for (std::size_t index = 1; index < map.size(); ++index)
                {
                    if (target > map[index].targetSeconds) continue;
                    const auto width = map[index].targetSeconds - map[index - 1].targetSeconds;
                    const auto u = width > 1.0e-9
                        ? (target - map[index - 1].targetSeconds) / width : 0.0;
                    return map[index - 1].sourceSeconds
                        + (map[index].sourceSeconds - map[index - 1].sourceSeconds) * u;
                }
                return map.back().sourceSeconds;
            };
            for (const auto& note : clip.notes)
            {
                SampleRegionSetting row;
                row.name = note.label;
                row.regionStartSeconds = std::max(0.0,
                    sourceAtTarget(note.startSeconds));
                row.regionEndSeconds = std::min(clip.sourceDurationSeconds,
                    sourceAtTarget(note.startSeconds + note.durationSeconds));
                row.alignmentSeconds = std::clamp(
                    sourceAtTarget(note.startSeconds + note.consonantSeconds),
                    row.regionStartSeconds, row.regionEndSeconds);
                row.fixedDurationSeconds = row.alignmentSeconds - row.regionStartSeconds;
                row.melodyneData = true;
                row.melodynePitchCenterCents = note.midiNote * 100.0;
                row.melodyneOriginalPitchCenterCents = note.sourceMidiCenter * 100.0;
                row.melodynePitchDrift = note.drift;
                row.melodynePitchModulation = note.modulation;
                row.melodyneFormantCents = note.formantSemitones * 100.0;
                row.melodyneAmplitude = note.gain;
                row.melodyneSibilantBalance = note.breath;
                row.melodyneAttackSeconds = note.consonantSeconds;
                rows.push_back(std::move(row));
            }
            // A pitchless leading element is an onset of the following vowel.
            // Collapse only adjacent source rows and preserve the vowel row's
            // identity, so reopening the project creates one UTAU-compatible
            // sample region with a longer preutterance.
            for (std::size_t index = 0; index + 1 < clip.notes.size(); ++index)
            {
                const auto& consonant = clip.notes[index];
                const auto& vowel = clip.notes[index + 1];
                const auto pitchless = std::none_of(consonant.contour.begin(),
                    consonant.contour.end(), [](const auto& point) { return point.voiced; });
                const auto vowelHasPitch = std::any_of(vowel.contour.begin(),
                    vowel.contour.end(), [](const auto& point) { return point.voiced; });
                const auto adjacent = std::abs(consonant.startSeconds + consonant.durationSeconds
                    - vowel.startSeconds) <= 0.002;
                if (!pitchless || !vowelHasPitch || !adjacent || index >= rows.size() - 1) continue;
                auto& onset = rows[index];
                auto& nucleus = rows[index + 1];
                nucleus.regionStartSeconds = onset.regionStartSeconds;
                nucleus.fixedDurationSeconds = std::max(0.0,
                    onset.fixedDurationSeconds + (nucleus.alignmentSeconds - nucleus.regionStartSeconds));
                nucleus.alignmentSeconds = std::clamp(
                    onset.regionStartSeconds + nucleus.fixedDurationSeconds,
                    nucleus.regionStartSeconds, nucleus.regionEndSeconds);
                nucleus.overlapSeconds = std::max(nucleus.overlapSeconds,
                    onset.regionEndSeconds - nucleus.regionStartSeconds);
                onset.regionEndSeconds = onset.regionStartSeconds;
            }
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                [](const auto& row) { return row.regionEndSeconds <= row.regionStartSeconds + 0.001; }),
                rows.end());
            const auto sidecar = SampleSettings::sidecarFor(clip.sourceFile);
            const auto existing = SampleSettings::loadOrDerive(clip.sourceFile, ProjectData{});
            const auto sameRow = [](const auto& left, const auto& right)
            {
                return left.name == right.name
                    && std::abs(left.regionStartSeconds - right.regionStartSeconds) < 1.0e-6
                    && std::abs(left.regionEndSeconds - right.regionEndSeconds) < 1.0e-6
                    && std::abs(left.alignmentSeconds - right.alignmentSeconds) < 1.0e-6
                    && std::abs(left.fixedDurationSeconds - right.fixedDurationSeconds) < 1.0e-6
                    && std::abs(left.overlapSeconds - right.overlapSeconds) < 1.0e-6
                    && std::abs(left.melodynePitchCenterCents - right.melodynePitchCenterCents) < 1.0e-6
                    && std::abs(left.melodyneOriginalPitchCenterCents - right.melodyneOriginalPitchCenterCents) < 1.0e-6;
            };
            const auto differs = sidecar.existsAsFile()
                && (existing.size() != rows.size()
                    || !std::equal(existing.begin(), existing.end(), rows.begin(), sameRow));
            if (differs)
            {
                const auto sourceAudio = clip.sourceFile;
                const auto candidate = std::make_shared<std::vector<SampleRegionSetting>>(std::move(rows));
                juce::AlertWindow::showYesNoCancelBox(
                    juce::MessageBoxIconType::WarningIcon,
                    "素材标注不一致",
                    "已存在 .hjm.csv 标注文件。选择“是”使用 Melodyne 新标注，选择“否”保留原标注。",
                    "使用新标注", "保留原标注", "取消", this,
                    juce::ModalCallbackFunction::create([sourceAudio, candidate](int result)
                    {
                        if (result != 1) return;
                        juce::String error;
                        if (!SampleSettings::save(sourceAudio, *candidate, error))
                            DBG("Could not update Melodyne sidecar: " + error);
                    }));
            }
            else if (!sidecar.existsAsFile())
            {
                juce::String annotationError;
                if (!SampleSettings::save(clip.sourceFile, rows, annotationError))
                    DBG("Could not seed Melodyne sidecar: " + annotationError);
            }
        }
    const auto algorithmId = preferences != nullptr
        ? preferences->getIntValue("import.algorithm", 1) : 1;
    const auto importedPitch = algorithmId == 2 ? PitchAlgorithm::nsfHifigan
        : algorithmId == 3 ? PitchAlgorithm::world
        : algorithmId == 4 ? PitchAlgorithm::vocalShifter
        : algorithmId == 6 ? PitchAlgorithm::llsm2 : PitchAlgorithm::mld5;
    const auto stretchAlgorithmId = preferences != nullptr
        ? preferences->getIntValue("import.stretchAlgorithm", 1) : 1;
    auto importedStretch = stretchAlgorithmId == 2 ? StretchAlgorithm::variableMelHop
        : stretchAlgorithmId == 3 ? StretchAlgorithm::loop
        : stretchAlgorithmId == 4 ? StretchAlgorithm::soundTouch
        : stretchAlgorithmId == 5 ? StretchAlgorithm::nsfShiftThenSplice
        : StretchAlgorithm::melodyneHybrid;
    // The two NSF variable-mel-hop orders are the NSF-HiFiGAN-specific
    // duration paths.  Keep an imported project immediately renderable when
    // another pitch backend is selected in Settings, matching the toolbar's
    // available choices.
    if (importedPitch != PitchAlgorithm::nsfHifigan
        && (importedStretch == StretchAlgorithm::variableMelHop
            || importedStretch == StretchAlgorithm::nsfShiftThenSplice))
        importedStretch = StretchAlgorithm::melodyneHybrid;
    for (auto& track : imported.project.tracks)
    {
        track.pitchAlgorithm = importedPitch;
        track.stretchAlgorithm = importedStretch;
    }

    const auto composeMode = preferences != nullptr
        ? preferences->getIntValue("import.melodyneCompose", 1) : 1;
    if (composeMode != 1)
    {
        for (auto& track : imported.project.tracks)
        {
            if (composeMode == 3) track.compose = true;
            else if (composeMode == 4) track.compose = false;
            // Mode 2 retains the melodic classification stored by Melodyne.
        }
        project.replace(std::move(imported.project));
        if (!imported.missingFiles.isEmpty())
            showError(strings.text("warning.missingMedia") + "\n"
                      + imported.missingFiles.joinIntoString("\n"));
        return;
    }
    auto state = std::make_shared<backend::MelodyneImportResult>(std::move(imported));
    auto* selector = new ComposeTrackSelector(state->project.tracks, strings);
    auto* dialog = new juce::AlertWindow(strings.text("mpd.compose.title"),
                                          strings.text("mpd.compose.description"),
                                          juce::MessageBoxIconType::QuestionIcon);
    dialog->addCustomComponent(selector);
    dialog->addButton(strings.text("dialog.import"), 1);
    dialog->addButton(strings.text("dialog.cancel"), 0,
                      juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    dialog->enterModalState(true,
        juce::ModalCallbackFunction::create([safe, dialog, selector, state](int result)
        {
            if (safe != nullptr && result == 1)
            {
                for (std::size_t index = 0; index < state->project.tracks.size(); ++index)
                    state->project.tracks[index].compose = selector->isCompose(index);
                safe->project.replace(std::move(state->project));
                if (!state->missingFiles.isEmpty())
                    safe->showError(safe->strings.text("warning.missingMedia") + "\n"
                                    + state->missingFiles.joinIntoString("\n"));
            }
            dialog->removeCustomComponent(0);
            delete selector;
            delete dialog;
        }), false);
}

void MainComponent::showError(const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                            strings.text("app.title"), message);
}
}
