#pragma once

#include "AudioEngine.h"
#include "I18n.h"
#include "PianoRollComponent.h"
#include "SettingsComponent.h"
#include "SampleSettings.h"
#include "VoicebankSettingsComponent.h"
#include "AssetManagerComponent.h"
#include "Theme.h"
#include "TimelineComponent.h"
#include "TrackListComponent.h"
#include "backend/MelodyneImporter.h"
#include "backend/MelodyneProvider.h"
#include "backend/AnalysisService.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace hachi
{
class EditorViewport final : public juce::Viewport
{
public:
    std::function<bool(const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override
    {
        if (onWheel && onWheel(event, wheel)) return;
        juce::Viewport::mouseWheelMove(event, wheel);
    }
};

// A button that opens a menu, and says so with the same chevron the mode
// switcher shows.  The text is drawn clear of the arrow's column rather than
// centred over the whole button, so a longer name cannot run into it.
class DropdownButton final : public juce::TextButton
{
public:
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
    // Where the text goes, given the whole button.  Pure and static so a check
    // can ask whether a label fits without painting anything.
    [[nodiscard]] static juce::Rectangle<int> textAreaFor(juce::Rectangle<int> bounds);
};

// A tool button that answers the right button too, so a tool can carry its
// own settings without taking a second slot in a row that has none to spare.
class ToolButton final : public juce::TextButton
{
public:
    std::function<void(juce::Point<int>)> onSecondaryClick;
    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu() && onSecondaryClick)
        {
            onSecondaryClick(event.getScreenPosition());
            return;
        }
        juce::TextButton::mouseDown(event);
    }
};

// A preset button that draws the shape it applies.  Four two-character names
// all look alike in a toolbar; the trapezoid says which is which at a glance.
class EnvelopePresetButton final : public juce::Button
{
public:
    EnvelopePresetButton() : juce::Button({}) {}
    void configure(juce::String captionText, double attack, double release,
                   float plateauEnd)
    {
        caption = std::move(captionText);
        attackSeconds = attack;
        releaseSeconds = release;
        plateauEndDb = plateauEnd;
        repaint();
    }
    void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

private:
    juce::String caption;
    double attackSeconds = 0.005;
    double releaseSeconds = 0.035;
    float plateauEndDb = 0.0f;
};

class MainComponent final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener,
                            private juce::Timer,
                            private juce::MenuBarModel
{
public:
    void applyRenderingPreference();

    // Where a paste lands when nothing says otherwise.  Carried to another
    // track it goes to the moment it was taken from, which is the point of
    // carrying it; dropped back on its own track that would be on top of
    // itself, so there it goes to the playhead, as it always did.  Pure, so
    // the rule can be checked without a window.
    //
    // The pointer comes before the selection because after a copy the
    // selection is what was copied: pasting there put the copy exactly on top
    // of its own original, and off a UTAU track notes are placed rather than
    // pushed along, so nothing appeared to happen at all.
    // Where a copied piece of material lands.
    //
    // The lane under the pointer when there is one, which is the only way to
    // name another track: clicking an empty lane reports no clip and does not
    // say whose lane it was, so before this a paste could only ever go to the
    // track already in hand.  Failing that, the track in hand and the
    // playhead, and if the playhead is not past the original, immediately
    // after it -- so a plain copy-paste still stacks copies end to end.
    struct ClipPasteTarget { juce::String trackId; double seconds = 0.0; };
    [[nodiscard]] static ClipPasteTarget clipPasteTargetFor(
        const juce::String& pointerTrackId, std::optional<double> pointerSeconds,
        const juce::String& trackInHand, double playheadSeconds,
        double sourceStartSeconds, double sourceDurationSeconds);
    // Which clip on a track a phrase goes into: the one covering the moment
    // asked for, else the track's first.  Notes are held relative to their
    // clip, so which clip is chosen decides what their times mean.  Pure, and
    // public so a check can follow the whole chain without a window.
    // The clip a pasted phrase goes into, and what to do when there is
    // none.  A melodic track starts with no clips at all and every
    // note-making path needs one, so pasting onto a track just made did
    // nothing, silently -- the first paste makes the clip, as the first
    // stroke of the draw tool does, and from the same place: the start of
    // the timeline, so the bars before it stay reachable.  One bar is only a
    // seed; a clip with no recording behind it stretches to fit.
    struct PasteClipPlan
    {
        juce::String clipId;      // empty when one has to be made
        bool makeOne = false;     // false and empty means there is nowhere
        // From the start of the timeline, so the bars before the pasted
        // phrase stay reachable.  Carried here rather than written at the
        // call, so it is part of the rule a check can read.
        double startSeconds = 0.0;
        double seedSeconds = 0.0;
    };
    [[nodiscard]] static PasteClipPlan pasteClipPlanFor(const ProjectData& data,
                                                        const juce::String& trackId,
                                                        double atSeconds);
    [[nodiscard]] static juce::String pasteTargetClipIn(const ProjectData& data,
                                                        const juce::String& trackId,
                                                        double atSeconds);
    [[nodiscard]] static double pasteTargetSeconds(
        bool sameTrack, double playheadSeconds, double originSeconds,
        std::optional<double> selectionStartSeconds,
        std::optional<double> askedFor,
        std::optional<double> pointerSeconds);
    // Where a panel should scroll to while playing, or nothing to leave it be.
    // A run that fits the window is shown whole and then held still; anything
    // longer is paged after, as before.  Pure, so it can be checked directly.
    // Where the roll should look after a request to leave source-edit mode.
    // Leaving it really does move the view back to what the timeline is
    // showing; a request that changes nothing must not move it at all, since
    // both the loudness lane and the point tool ask for it on every click.
    // Seconds, not pixels: the two views run at different scales.  Pure.
    // Which stretch clocks a pitch algorithm actually reaches, as picker item
    // ids in display order.  Only two backends read the choice: the neural
    // decoder, which takes it as a splice order, and vslib, which *is* the
    // Signalsmith stretcher this configures.  mld5, mld3, WORLD and llsm2
    // stretch from the time map inside their own renderers and never receive
    // the value -- it is dropped at the dispatch.  Empty means the picker
    // would do nothing and is hidden instead of sitting there inert.
    // What the wheel does in the editors.  The wheel scrolls and the
    // modifiers zoom, which is the way round every other editor of this kind
    // works.  Pure, and public so a check reads the same mapping the handler
    // dispatches on.
    enum class WheelAction { scrollVertically, scrollHorizontally,
                             zoomVertically, zoomHorizontally };
    [[nodiscard]] static WheelAction wheelActionFor(const juce::ModifierKeys& modifiers);

    // The two envelope lanes at the bottom of the roll.  Only one can be open:
    // they occupy the same strip and drag the same way, so with both believing
    // they were open a drag went to whichever the roll's tool happened to be
    // while the buttons said something else.
    enum class EnvelopeLane { none, amplitude, flagCurve };
    // Clicking the button for a lane opens it, or closes it if it was already
    // the open one.  Pure, and public so a check reads the same rule the
    // buttons act on.
    [[nodiscard]] static EnvelopeLane nextEnvelopeLane(EnvelopeLane open,
                                                       EnvelopeLane clicked);
    void setEnvelopeLane(EnvelopeLane lane);
    void closeEnvelopeLanes();
    [[nodiscard]] EnvelopeLane openEnvelopeLane() const { return envelopeLane; }

    [[nodiscard]] static std::vector<int> stretchAlgorithmItemsFor(int pitchAlgorithmItemId);

    // What Delete removes.  The piano roll deletes notes and knows nothing of
    // clips, so an imported clip -- which arrives with no notes at all until
    // its analysis finishes -- could not be removed by keyboard: the key was
    // simply not handled, and the material stayed with its waveform showing.
    //
    // Notes first, since a selection of them is the more specific thing to
    // have asked for.  The same order Ctrl+C already uses.
    enum class DeleteTarget { nothing, notes, clip };
    [[nodiscard]] static DeleteTarget deleteTargetFor(bool notesSelected, bool clipSelected);
    void deleteSelectedClip();

    // What the roll draws over the notes, behind one dropdown.  Both switches
    // are looked at while tuning but flipped rarely, so a pair of permanent
    // buttons in the tool row cost more room than they earned.
    struct ViewOptions
    {
        bool noteRange = true;   // the orange box around each note's real extent
        bool envelope = false;   // the note's own amplitude shape, drawn on it
        // The peaks of what was actually synthesised, per note.  Off unless
        // asked for: they exist only for notes that have been rendered and
        // not touched since, so most of the time there is nothing to draw.
        bool utauWaveform = false;
        // The line the note is sung along.  On by default: without it the
        // roll shows where notes are but not what they do.
        bool pitchLine = true;
    };
    // The settings keys are the ones the two buttons already used, so a
    // project opened after this change still shows what it showed before.
    // Pure, and public so a check reads the same names the application writes.
    [[nodiscard]] static ViewOptions viewOptionsFrom(const juce::PropertySet& properties);
    static void storeViewOptions(juce::PropertySet& properties, const ViewOptions& options);
    // Menu ids are positions in one list, so the menu that is shown and the
    // answer to "what was clicked" cannot drift apart.
    [[nodiscard]] static ViewOptions afterViewMenuChoice(ViewOptions options, int chosen);
    // Only the UTAU modes render note by note, so only they have per-note
    // audio to draw; elsewhere that item is shown greyed rather than dropped,
    // so the menu keeps its shape and the tick still says what is set.
    [[nodiscard]] static bool viewMenuItemEnabled(int chosen, bool utauEditorActive);



    // The menu the space around the tracks offers, and the one place a track
    // is made from a menu -- the Track menu's own items come through here too.
    // What the draw tool offers on its own button: the step a drawn note
    // grows by, which is otherwise invisible and unreachable.
    void showDrawSettingsMenu(juce::Point<int> screenPosition);
    void setDrawLengthDivision(int division);
    void showTrackAreaMenu(juce::Point<int> screenPosition);
    void addTrackFromMenu(bool compose);
    void addReferenceTrackFromMenu();
    // The one way the engine is told about the project.  It carries the track
    // being worked on with it, because a material track's audibility depends
    // on that and the engine decides it while syncing -- sending one without
    // the other would leave the two disagreeing.
    void syncAudio(const ProjectData& data);
    juce::String auditionTrackAtLastSync;
    void deleteSelectedTrack();

    // Which track the selection moves to once the one at this index is gone:
    // the one that takes its place, or the one before it when it was last,
    // and none at all when it was the only one.  Left where it was, the
    // selection names a track that no longer exists and every item that acts
    // on "the selected track" stays enabled and does nothing.  Pure, and
    // public so a check reads the same rule the deletion uses.
    [[nodiscard]] static juce::String selectionAfterRemoving(
        const std::vector<TrackData>& tracks, const juce::String& removedId);

    [[nodiscard]] static std::optional<double> viewSecondsLeavingSourceEdit(
        bool wasEnabled, bool nowEnabled, double timelineSeconds);
    [[nodiscard]] static std::optional<int> followViewPosition(
        int viewLeft, int viewWidth, int playheadX, int leftMargin,
        const std::optional<juce::Range<int>>& run);
    // The tracks an export writes, in project order, with the file each goes
    // to.  A track that would not sound -- muted, or unsoloed while something
    // else is soloed -- is left out rather than written as silence, and so is
    // one with no clips.  Naming: the chosen file itself for a single track,
    // "<stem> - <track>.wav" beside it when every track is being written.
    // Pure, so the rule can be checked without a window.
    // Where the export chooser opens.  The folder last exported to, if it is
    // still there; Documents the first time, or if that folder has since been
    // moved or removed.  Pure, so the rule can be checked without a window.
    [[nodiscard]] static juce::File exportStartFile(const juce::File& remembered,
                                                    const juce::String& suggestedName);
    struct ExportTarget { juce::String trackId, trackName; juce::File file; };
    [[nodiscard]] static std::vector<ExportTarget> exportTargets(
        const ProjectData& project, const juce::File& destination,
        const juce::String& onlyTrackId, const juce::String& untitledName);
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void openExternalFile(const juce::File& file);
    void requestClose(std::function<void()> approved);
    bool keyPressed(const juce::KeyPress& key) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    // Which tool is in hand, and what a layout pass does to it.  A rule in
    // resized() used to take the point tool away again, so no pure function
    // could have caught it: a check has to press the button on a real window
    // and then let the window lay itself out, as every selection does.
    void diagnosticPressTool(PianoRollComponent::Tool wanted);
    [[nodiscard]] PianoRollComponent::Tool diagnosticTool() const;
    void diagnosticRefreshControls();
    [[nodiscard]] bool diagnosticRenderOrderPicker();

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void timerCallback() override;
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex,
                                    const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;
    void refreshTexts();
    void refreshProjectControls();
    void refreshSelectedNoteParameter();
    void applySelectedNoteParameter();
    void adjustHorizontalZoom(double factor);
    void adjustVerticalZoom(double factor);
    void refreshStretchAlgorithmItems(int preferredId = 0);
    void chooseUtauVoicebank();
    void showVoicebankSettings();
    // Crossfades the shared boundaries of the selected adjacent notes.
    void spliceSelectedNotes();
    void refreshSpliceButton();
    // Open the oto / four-region editor straight on the entry a note uses,
    // without going through the voicebank list.  Empty noteId uses the
    // current selection.
    void showRegionEditorForNote(const juce::String& noteId = {});
    bool bindDefaultUtauVoicebank(const juce::String& trackId);
    void prepareUtauTrackForNote(const juce::String& noteId);
    void commitNoteAlias();
    void commitNoteFlags();
    void commitNoteConsonantVelocity();
    void setToolButton(juce::Button& selected);
    void togglePlayback();
    void armSelectionPlayback();
    void setTracksCollapsed(bool collapsed);
    void refreshCollapseIcon();
    void newProject();
    void openProject();
    void loadProjectFile(const juce::File& file);
    void saveProject(std::function<void(bool)> completion = {});
    void saveProjectAs(std::function<void(bool)> completion = {});
    bool saveProjectTo(const juce::File& file);
    void performWithUnsavedCheck(std::function<void()> action);
    void addRecentProject(const juce::File& file);
    void restoreRecentProjects();
    void exportMixdown();
    void chooseExportDestination(const juce::String& trackId);
    void exportLastRender();
    void rememberExportDirectory(const juce::File& directory);
    juce::File lastExportDirectory;
    void beginExport(std::vector<ExportTarget> targets,
                     const std::vector<juce::String>& scopeNoteIds,
                     juce::Range<double> range);
    void finishExport();
    // The marquee that was last set going.  Kept from the moment play starts,
    // so pausing partway through does not shorten it.
    std::vector<juce::String> lastRenderedNoteIds;
    juce::Range<double> lastRenderedSpan;
    void importAudio();
    void addAnalysedAudioFile(const juce::File& file, double durationSeconds,
                              double startSeconds = 0.0,
                              const juce::String& targetTrackId = {});
    void scheduleAnalysis(const juce::File& file, const juce::String& clipId);
    void importMidi();
    // A UTAU project file, which arrives as a plain UTAU track.
    void importUst();
    void loadUstFile(const juce::File& file);
    void importMelodyne();
    void showSettings();
    void applyPreferences();
    void applyUiScale();
    void loadSampleSettings();
    void refreshSampleEditors();
    void commitSampleEditors();
    void saveSampleSettings();
    void importOto();
    void exportOto();
    void showAssetManager();
    void showClipGainDialog();
    void showRenameTrackDialog();
    void showTransposeNotesDialog();
    void showSetNotesPitchDialog();
    void copySelectedNotes(bool cut);
    // Nothing said: back at the moment it was copied from.  A time given:
    // there instead.
    void pasteCopiedNotes(std::optional<double> atSeconds = {});
    // Which clip a paste at this moment lands in, on the track in front.
    [[nodiscard]] juce::String pasteTargetTrack() const;
    [[nodiscard]] juce::String pasteTargetClipUnused(const juce::String& trackId,
                                               double atSeconds) const;
    void pasteCopiedNotesInto(const juce::String& clipId, double atSeconds,
                              const std::vector<juce::String>& replacing);
    void copySelectedClip();
    void pasteCopiedClip();
    void duplicateSelectedClip();
    void confirmDestructive(const juce::String& title, std::function<void()> action);
    void loadMelodyneFile(const juce::File& file);
    void presentMelodyneComposeSelection(backend::MelodyneImportResult imported);
    // A Melodyne import references source recordings but is not a registered
    // material folder yet.  Offer to register the folder(s) holding that media
    // so the material stays reusable, exactly as a UTAU voicebank import is.
    void offerMaterialFolderForImport(const ProjectData& imported);
    void focusClip(const juce::String& clipId);
    void focusNote(const juce::String& noteId);
    void updateUtauRenderSelection();
    void setSourceEditMode(bool enabled);
    void showError(const juce::String& message);

    HachiLookAndFeel lookAndFeel;
    I18n strings;
    ProjectModel project;
    AudioEngine audio;
    juce::TooltipWindow tooltipWindow;

    juce::MenuBarComponent menuBar;
    juce::Label bpmCaption;
    juce::Label bpmEditor;
    juce::Label beatsCaption;
    juce::Label beatsEditor;
    juce::Label denominatorLabel;
    juce::Label gridCaption;
    juce::ComboBox gridSelector;
    juce::Label stretchCaption;
    juce::ComboBox stretchSelector;
    juce::Label scaleCaption;
    juce::ComboBox scaleSelector;
    juce::TextButton openButton;
    juce::TextButton saveButton;
    juce::TextButton audioButton;
    juce::TextButton melodyneButton;
    juce::TextButton collapseTracksButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton noteEditButton;
    juce::TextButton wrenchButton;
    ToolButton drawButton;
    juce::TextButton lineButton;
    juce::TextButton pointButton;
    juce::TextButton connectButton;
    juce::TextButton pitchParamButton;
    juce::TextButton driftParamButton;
    juce::TextButton attackParamButton;
    juce::TextButton breathParamButton;
    juce::TextButton tensionParamButton;
    juce::TextButton formantParamButton;
    juce::TextButton volumeParamButton;
    // Per-frame flags for the selected notes, and the lane that draws the
    // curve.  The lane is only reachable once the notes are switched over:
    // with flags held as a single number there is no curve to show.
    juce::TextButton flagCurveButton, flagEnvelopeButton;
    DropdownButton showViewMenuButton;
    ViewOptions viewOptions;
    void showViewMenu();
    void applyViewOptions();
    juce::Label envelopePresetCaption;
    // Four shapes taken from the commonest Envelope fields in real USTs.
    std::array<EnvelopePresetButton, 4> envelopePresetButtons;
    juce::ToggleButton robustPitchCurveButton;
    juce::ComboBox pitchAlgorithm;
    juce::ComboBox stretchAlgorithm;
    // Only the neural decoder can render a phrase in one pass, so this picker
    // is shown only when that decoder is the one selected.
    juce::ComboBox renderOrder;
    juce::Label pitchLabel;
    juce::Label stretchLabel;
    juce::Label renderOrderLabel;
    juce::Label statusLabel;
    juce::Label sourceEditHint;
    juce::ComboBox sampleRegionSelector;
    juce::TextEditor sampleAliasEditor;
    juce::TextEditor sampleStartEditor;
    juce::TextEditor sampleEndEditor;
    juce::TextEditor sampleAlignmentEditor;
    juce::TextEditor sampleFixedEditor;
    juce::Label sampleAliasLabel, sampleStartLabel, sampleEndLabel,
                sampleAlignmentLabel, sampleFixedLabel;
    juce::TextButton sampleSaveButton, otoImportButton, otoExportButton;
    juce::Label utauVoicebankLabel, noteAliasLabel, noteConsonantVelocityLabel,
                noteFlagsLabel, utauVoicebankPath;
    juce::TextButton utauVoicebankButton;
    juce::TextButton voicebankSettingsButton;
    juce::TextButton spliceButton;
    juce::TextEditor noteAliasEditor, noteConsonantVelocityEditor, noteFlagsEditor;
    bool noteConsonantVelocityMixed = false;
    bool noteConsonantVelocityDirty = false;
    bool noteFlagsMixed = false;
    bool noteFlagsDirty = false;
    juce::Label parameterTitle;
    juce::Label smoothCaption;
    juce::Slider smoothSlider;
    juce::Slider zoomSlider;
    juce::Slider vZoomSlider;
    juce::TextButton horizontalZoomOutButton;
    juce::TextButton horizontalZoomInButton;
    juce::TextButton verticalZoomOutButton;
    juce::TextButton verticalZoomInButton;
    bool showWaveforms = true;
    double progress = 0.0;
    juce::ProgressBar progressBar;
    bool importInProgress = false;
    // An export waits for every note in the song to render before it writes,
    // and the wait is done by the timer rather than by blocking the message
    // thread: a whole song of UTAU notes can take minutes.
    std::vector<ExportTarget> pendingExport;
    juce::Range<double> pendingExportRange;
    bool exportWaitingForRender = false;
    bool showingRenderProgress = false;
    bool playWhenRenderReady = false;
    int activeUtauSelectionCount = 0;
    int pendingNativeAnalyses = 0;
    double nativeAnalysisProgress = 0.0;
    juce::String nativeAnalysisName;

    TrackListComponent trackList;
    TimelineComponent timeline;
    PianoRollComponent pianoRoll;
    juce::Viewport trackViewport;
    EditorViewport timelineViewport;
    EditorViewport pianoViewport;
    juce::Component panelSplitter;
    // The material manager, docked on the right of the main window so it can be
    // operated while editing.  Hidden until its menu item is chosen; its width
    // is drag-resizable via the edge on its left and remembered.
    std::unique_ptr<AssetManagerComponent> assetManager;
    std::unique_ptr<juce::ResizableEdgeComponent> assetManagerResizer;
    // Records the width the drag handle settles on and re-lays the window, so
    // the panel keeps whatever width the user drags it to.
    struct AssetManagerConstrainer final : juce::ComponentBoundsConstrainer
    {
        std::function<void(int)> onWidth;
        void checkBounds(juce::Rectangle<int>& bounds,
                         const juce::Rectangle<int>& previous,
                         const juce::Rectangle<int>& limits,
                         bool isStretchingTop, bool isStretchingLeft,
                         bool isStretchingBottom, bool isStretchingRight) override
        {
            juce::ComponentBoundsConstrainer::checkBounds(bounds, previous, limits,
                isStretchingTop, isStretchingLeft, isStretchingBottom, isStretchingRight);
            if (onWidth) onWidth(bounds.getWidth());
        }
    };
    AssetManagerConstrainer assetManagerConstrainer;
    bool assetManagerVisible = false;
    int assetManagerWidth = 320;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::PropertiesFile> preferences;
    int lastTimelineX = 0;
    int lastPianoX = 0;
    // Arrangement folded away, leaving the whole window to the tuning editor.
    bool tracksCollapsed = false;
    // What this run of playback covers, when it was started for a selection.
    std::optional<juce::Range<double>> playbackRun;
    int lastTimelineY = 0;
    int lastTrackY = 0;
    bool syncingScroll = false;
    bool pianoInitialScrollSet = false;
    bool sourceEditActive = false;
    juce::File sampleSettingsFile;
    std::vector<SampleRegionSetting> sampleSettingsRows;
    int activeSampleSetting = 0;
    juce::String selectedClipId;
    juce::String selectedTrackId;
    juce::String selectedNoteId;
    std::unordered_set<std::string> accumulatedUtauNoteIds;
    juce::String copiedClipId;
    std::vector<NoteData> copiedNotes;
    // Where the copied block sat on the timeline.  Pasting without saying
    // where puts it back at the same moment, on whichever track is in front.
    double copiedOriginSeconds = 0.0;
    // The track it was copied from.  Pasting back onto that same track means
    // something different from carrying it across to another one.
    juce::String copiedTrackId;
    juce::File currentProjectFile;
    juce::StringArray recentProjectPaths;
    std::uint64_t savedProjectRevision = 0;
    enum class ParameterMode { pitchSmooth, pitchDrift, attackSpeed, breath, tension, formant, volume };
    ParameterMode parameterMode = ParameterMode::pitchSmooth;
    bool updatingSmoothSlider = false;
    bool updatingRobustPitchCurve = false;
    bool smoothSliderDragging = false;
    // Both UTAU selector items -- plain (7) and four-region (8) -- must light
    // up every UTAU affordance.  Testing the id against 7 alone silently
    // hides the voicebank bar, its settings button, the point tool and the
    // amplitude envelope from the four-region mode.
    bool isUtauAlgorithmSelected() const;
    // Kept in step with envelopeLane, which is what opens and closes it.
    bool utauAmplitudeEnvelopeActive = false;
    EnvelopeLane envelopeLane = EnvelopeLane::none;
    bool draggingPanelSplitter = false;
    int panelSplitterDragScreenY = 0;
    float panelSplitterDragRatio = 0.60f;
    float panelSplitRatio = 0.60f;
};
}
