#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace hachi
{
inline constexpr int inheritedUtauConsonantVelocity =
    std::numeric_limits<int>::min();

struct SampleRegionSetting;

// Native annotation vocabulary shared by every import path.  Input formats
// may retain provenance, but downstream editing does not branch on it.
enum class NativeSegmentRole
{
    unknown,
    consonant,
    vowel,
    transition,
    silence,
    breath,
    noise,
    ending
};
[[nodiscard]] juce::String nativeSegmentRoleName(NativeSegmentRole role);
[[nodiscard]] NativeSegmentRole parseNativeSegmentRole(const juce::String& value);

struct NativeSegment
{
    juce::String id;
    juce::String alias = "-";
    NativeSegmentRole role = NativeSegmentRole::unknown;
    double sourceStartSeconds = 0.0;
    double sourceEndSeconds = 0.0;
    // Native data only: imported/estimated/user.  This is provenance, not an
    // execution mode, and does not select a renderer or an editor layout.
    juce::String provenance = "estimated";
    float confidence = 0.0f;
    double alignmentSeconds = 0.0;
    double overlapSeconds = 0.0;
    bool stretchable = true;
    double stretchWeight = 1.0;
};

struct NativeMaterialAnnotation
{
    int version = 2;
    juce::String materialId;
    juce::File sourceFile;
    juce::File annotationFile;
    std::vector<NativeSegment> segments;
};
// The UTAU synthesis modes.  A track in any of them is PitchAlgorithm::utau,
// so every UTAU affordance is inherited rather than reimplemented; the mode
// only says how a sample is carved up and how a syllable reaches the engine.
//
//   classic  plain UTAU: one sample, the oto's own consonant and stretch
//   jie 界   four regions per sample, each with its own stretch policy
//   mou 谋   the four-region CV of 界, plus a two-region VC -- one written
//            syllable becomes a CV note and a VC note, as UTAU tuning has
//            always done it
enum class UtauMode { classic, jie, mou };

// Four-region data is in play for 界 and 谋 alike: 谋 keeps the four-region
// CV and only adds the VC beside it, so everything that reads oto4 reads it
// for both.  Only the labels tell the two apart.
[[nodiscard]] constexpr bool utauModeUsesRegions(UtauMode mode)
{
    return mode != UtauMode::classic;
}
[[nodiscard]] juce::String utauModeLabel(UtauMode mode);
// The word written into a project file and accepted over MCP.  "utau4" is
// what 界 has always been called there and still reads.
[[nodiscard]] juce::String utauModeKey(UtauMode mode);
[[nodiscard]] UtauMode parseUtauMode(const juce::String& text);
// Which item of the pitch-algorithm picker a mode is, and back.  Kept in one
// place: the id used to be written out by hand wherever it was needed, and
// adding a third mode left one of those spellings behind, so 谋 came up
// wearing the generic parameter set instead of the UTAU toolbar.
// utauModeForPickerItem answers nothing for an item that is not a UTAU mode.
[[nodiscard]] int utauModePickerItem(UtauMode mode);
[[nodiscard]] std::optional<UtauMode> utauModeForPickerItem(int itemId);

enum class PitchAlgorithm
{
    mld5,
    mld3,
    nsfHifigan,
    world,
    vocalShifter,
    llsm2,
    utau
};

// The two NSF-exclusive variable-hop Mel stretch orders.  Melodyne5 applies
// its pitch/frequency-domain mask to each element before the time-domain
// Catmull-Rom stretch/splice (pitch first, then splice), so the shift-first
// path matches the original algorithm; the splice-first path is the HachiShifter
// variable-mel-hop default and keeps the join blended before formant shifting.
[[nodiscard]] PitchAlgorithm defaultPitchAlgorithm(const juce::File& modelDirectory = {});

enum class StretchAlgorithm
{
    melodyneHybrid,
    variableMelHop,
    loop,
    soundTouch,
    nsfShiftThenSplice
};

// Where the splice sits relative to the pitch work, for a track that composes.
//  - processThenSplice: every clip is rendered on its own and the results are
//    laid end to end.  Each render is independent, so it is what the cache,
//    cancellation and the per-clip fades are all shaped around.
//  - stretchSpliceThenPitch (the DAW/Melodyne order): clips joined by a
//    Melodyne pitch join are assembled into one phrase first -- their time maps
//    stitched into one continuous map, their pitch lines glided across each
//    seam -- decoded in a single NSF-HiFiGAN pass, and only then cut back into
//    per-clip buffers.  Nothing is spliced after the model has run, so phase,
//    F0 and mel stay continuous through every internal seam.
enum class RenderOrder
{
    processThenSplice,
    stretchSpliceThenPitch
};

struct PitchPoint
{
    double timeSeconds = 0.0;
    float relativeCents = 0.0f;
    float withoutVibratoCents = 0.0f;
    bool voiced = true;
    float manualTargetCents = 0.0f;
    bool hasManualTarget = false;
};

enum class PitchCurveShape
{
    natural,
    linear,
    smooth,
    easeIn,
    easeOut,
    customBezier
};

struct PitchCurveEditPoint
{
    double timeSeconds = 0.0;
    float targetMidi = 60.0f;
    // The shape of the incoming segment: previous point -> this point.
    // The first point's value is intentionally ignored.
    PitchCurveShape shape = PitchCurveShape::natural;
    // Normalised cubic Bezier handles for the incoming segment. X is clamped
    // to [0, 1] so time remains single-valued; Y may leave that range to make
    // controlled pitch overshoot possible.
    float bezierX1 = 0.33f;
    float bezierY1 = 0.0f;
    float bezierX2 = 0.67f;
    float bezierY2 = 1.0f;
};

// Per-note amplitude automation used after the UTAU resampler and before the
// internal wavtool-style overlap mixer.  Times are relative to the nominal
// note start and may be negative to cover the oto.ini preutterance region.
struct AmplitudeEnvelopePoint
{
    double timeSeconds = 0.0;
    float gainDb = 0.0f;
};

// A handle on a per-frame flag curve.  Times are relative to the nominal note
// start, exactly like an amplitude envelope point, and may be negative to
// reach into the preutterance; the value is in the flag's own units.  One
// point is a legitimate curve -- it holds that value for the whole note.
struct FlagCurvePoint
{
    double timeSeconds = 0.0;
    float value = 0.0f;
    // The shape of the segment arriving at this point, as on the pitch line,
    // so the first point never has its own shape consulted.  A flag curve is
    // straight lines to begin with; "natural" is a contour-wide shape, is not
    // offered here, and reads as linear if it ever turns up.
    PitchCurveShape shape = PitchCurveShape::linear;
    float bezierX1 = 0.33f;
    float bezierY1 = 0.0f;
    float bezierX2 = 0.67f;
    float bezierY2 = 1.0f;
};

// A shape by name, and back.  Used by the project file, the MCP layer and
// anything else that has to say which shape a segment has.
[[nodiscard]] juce::String pitchCurveShapeName(PitchCurveShape value);
[[nodiscard]] PitchCurveShape parsePitchCurveShape(const juce::String& value);
// The eased progress along one segment, 0..1.  Shared by the pitch line and
// the flag curves, so a shape means the same thing in both.
[[nodiscard]] float shapedSegmentProgress(PitchCurveShape shape, float u,
                                          float bezierX1, float bezierY1,
                                          float bezierX2, float bezierY2);
// A flag curve read at one instant, held flat outside its own handles.
[[nodiscard]] float flagCurveValueAt(const std::vector<FlagCurvePoint>& points,
                                     double timeSeconds);

// One flag's curve on one note.
struct FlagCurve
{
    juce::String flag;
    std::vector<FlagCurvePoint> points;
};

// Which flags can be drawn as a curve, and the range each is drawn on.  The
// ranges are the engine's own clamps, so a handle cannot be put somewhere that
// would not be rendered.  Only flags whose effect is decided frame by frame
// are here: a kernel choice or a whole-note normalisation has no per-frame
// meaning, and the engine says so and ignores it.
struct FlagCurveKind
{
    const char* flag;
    const char* label;
    float minimum;
    float maximum;
    // b and bh only ever act on the onset: bh is gated to the onset region by
    // the engine outright, and b acts on unvoiced frames, which a rendered
    // note keeps inside its onset.  Measured on si-/shi-/ban, both leave the
    // vowel within 0.1% (bh) and 4% (b) while moving the onset by 8-99%; the
    // flag that does work across the whole note is Mb, not these two.  So
    // their curves are drawn and edited over the onset alone.
    bool onsetOnly = false;
};
[[nodiscard]] const std::vector<FlagCurveKind>& flagCurveKinds();
[[nodiscard]] const FlagCurveKind& flagCurveKindFor(const juce::String& flag);
struct NoteData;
// The points of one flag's curve on a note, empty when it has none.
[[nodiscard]] std::vector<FlagCurvePoint> flagCurvePointsFor(const NoteData& note,
                                                             const juce::String& flag);

[[nodiscard]] float evaluatePitchCurve(
    const std::vector<PitchCurveEditPoint>& points, double timeSeconds);

// A source-time anchor imported from an external editor.  Both values are
// relative to the beginning of the clip: targetSeconds is the position on the
// HachiShifter timeline and sourceSeconds is the corresponding position in the
// selected source-media range.  Keeping these anchors in the native project is
// essential for preserving nonlinear Attack/vowel timing instead of reducing
// every imported element to one linear duration ratio.
struct SourceTimePoint
{
    double targetSeconds = 0.0;
    double sourceSeconds = 0.0;
};

struct NoteData
{
    juce::String id;
    juce::String label;
    // The native editor consumes these fields for every note, regardless of
    // whether the note came from UST, MPD, MIDI or a hand-created placement.
    // Segment times are note-local after an HJM region is bound to a note.
    NativeSegmentRole nativeRole = NativeSegmentRole::unknown;
    juce::String nativeProvenance = "estimated";
    float nativeConfidence = 0.0f;
    double nativeSourceStartSeconds = -1.0;
    double nativeSourceEndSeconds = -1.0;
    std::vector<NativeSegment> nativeSegments;
    // Passed verbatim as the UTAU resampler's flags argument.  Interpretation
    // belongs to the selected resampler because flag dialects are not uniform.
    juce::String utauFlags;
    // Every signed integer, including negative values, is a valid resampler
    // velocity.  INT_MIN is reserved internally for the track-wide value.
    int utauConsonantVelocity = inheritedUtauConsonantVelocity;
    // Optional per-note UTAU timing.  Preutterance is the final lead-in after
    // consonant velocity has been applied; overlap is signed so a negative
    // value creates a real gap before this note.
    bool utauPreutteranceOverrideEnabled = false;
    double utauPreutteranceSeconds = 0.0;
    bool utauOverlapOverrideEnabled = false;
    double utauOverlapSeconds = 0.0;
    // STP: how far the whole oto entry is moved inside the recording before a
    // note of it is read.  Positive reads later in the file, negative earlier.
    // Every boundary the entry holds -- offset, overlap, preutterance,
    // consonant, cutoff and the four region lengths -- is measured from the
    // offset, so shifting the offset carries all of them along together.  It
    // changes which audio is played, never where the note sits in the piece.
    double utauStpSeconds = 0.0;
    // Manual four-region split, as three cumulative fractions of the note's
    // sounding span (onset|glide, glide|nucleus, nucleus|coda).  Unset means
    // the regions are allocated by weight from the sample's own lengths.
    // Synthetic vibrato, following UTAU's seven VBR parameters.  It is a layer
    // on top of whatever pitch curve the note already has: the curve stays the
    // baseline and the vibrato swings around it.
    bool vibratoEnabled = false;
    double vibratoLengthPercent = 65.0;    // share of the note, measured back from its end
    double vibratoCycleMs = 180.0;         // one full swing
    double vibratoDepthCents = 35.0;
    double vibratoFadeInPercent = 20.0;    // of the vibrato span
    double vibratoFadeOutPercent = 20.0;
    double vibratoPhasePercent = 0.0;      // 100 = one whole cycle
    double vibratoOffsetPercent = 0.0;     // shifts the swing centre, in depths
    // Display only: draw the pitch line with the vibrato already folded in,
    // instead of a flat pitch line plus a separate swing.  Purely how the
    // note is shown; what is rendered is the same either way.
    bool vibratoRealLine = false;
    // Per-region flags for the four-region mode.  Empty entries fall back to
    // the note and track flags, so a split note keeps its ordinary flags
    // wherever it has nothing region-specific to say.
    bool utauFlagSplit = false;
    // Crossfade this note into the one before it at mix time.  Deliberately
    // not a timing override: splicing must not touch preutterance or
    // overlap, which belong to the voicebank and drive the drawn spans.
    bool utauSplice = false;
    juce::String utauRegionFlags1, utauRegionFlags2,
                 utauRegionFlags3, utauRegionFlags4;
    // Per-frame ("linear") flags for this note.  While this is on, g comes
    // from the curve below and the g written in the Flags text is ignored;
    // every other flag in that text goes on working as before.
    bool utauFlagCurveEnabled = false;
    std::vector<FlagCurve> utauFlagCurves;
    bool utauJieSplitSet = false;
    double utauJieSplit1 = 0.0;
    double utauJieSplit2 = 0.0;
    double utauJieSplit3 = 0.0;
    double startSeconds = 0.0;
    double durationSeconds = 0.25;
    double consonantSeconds = 0.04;
    // Melodyne may keep an unpitched onset as a complete preceding element.
    // During UTAU conversion it belongs to the following vowel and contributes
    // to that vowel's preutterance rather than becoming an independent note.
    bool melodyneConsonantCandidate = false;
    juce::String melodyneVowelNoteId;
    float midiNote = 60.0f;
    float sourceMidiCenter = -1.0f;
    float modulation = 1.0f;
    float drift = 1.0f;
    float tension = 0.0f;
    float breath = 0.0f;
    float formantSemitones = 0.0f;
    float gain = 1.0f;
    float attackSpeed = 1.0f;
    // Melodyne's Robust Pitch Curve is stored by the detector/source.  Hachi
    // exposes the same behaviour per note so difficult notes can opt in
    // without changing the rest of the source analysis.
    bool robustPitchCurve = false;
    bool connectedToPrevious = false;
    bool connectedToNext = false;
    std::vector<PitchPoint> contour;
    // Sparse, user-visible pitch handles used by the UTAU point editor.
    // These are stored separately from the dense 5 ms render contour so a
    // deliberately added collinear handle survives simplification and save/load.
    std::vector<PitchCurveEditPoint> pitchControlPoints;
    std::vector<AmplitudeEnvelopePoint> amplitudeEnvelope;
    // Scales the whole amplitude envelope up or down without changing its
    // shape.  100 is the envelope as drawn; 200 is twice as loud; 0 is silence.
    // A note with no envelope of its own still has an implied flat 100% line,
    // which this raises like any other.
    float amplitudeEnvelopeBasePercent = 100.0f;
    std::vector<double> sibilantMarkers;
};

// Scales an amplitude envelope by a base percent (100 = unchanged), in the
// dB domain, clamped to the same [-60, +12] dB range the UST importer uses.
[[nodiscard]] std::vector<AmplitudeEnvelopePoint> scaledAmplitudeEnvelope(
    const std::vector<AmplitudeEnvelopePoint>& points, float basePercent);
// The inverse: recover the drawn envelope from a scaled one.
[[nodiscard]] std::vector<AmplitudeEnvelopePoint> unscaledAmplitudeEnvelope(
    const std::vector<AmplitudeEnvelopePoint>& points, float basePercent);

[[nodiscard]] float renderedPitchCents(const NoteData& note, const PitchPoint& point);

struct ClipData
{
    juce::String id;
    juce::File sourceFile;
    double startSeconds = 0.0;
    double sourceOffsetSeconds = 0.0;
    double sourceDurationSeconds = 0.0;
    double durationSeconds = 1.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    // Melodyne successive-join amplitude transitions.  These are the actual
    // crossfade mechanism in a Melodyne project: MUSuccessiveJoin.amplitudeTransitionDuration
    // with joinsAmplitudes=true, applied as a LINEAR complementary fade at the
    // join boundary (element shape powers are 1.0).  The legacy fadeInTime/
    // fadeOutTime fields are always NaN in saved projects.
    double crossfadeInSeconds = 0.0;
    double crossfadeOutSeconds = 0.0;
    float gain = 1.0f;
    bool muted = false;
    // A Melodyne glide split: two elements joined by followingJoin.joinsPitches.
    // The recording runs continuously through the seam while the two halves are
    // pitched and stretched separately, so under stretchSpliceThenPitch such a
    // chain is rendered as one phrase rather than spliced afterwards.
    bool glideConnectedToNext = false;
    bool glideConnectedFromPrevious = false;
    std::vector<SourceTimePoint> sourceTimeMap;
    std::vector<NoteData> notes;
};

struct TrackData
{
    juce::String id;
    juce::String name;
    bool compose = true;
    bool muted = false;
    bool solo = false;
    // A material track: something to work against rather than part of the
    // piece.  It is heard only while it is the track being worked on, and is
    // silent whenever anything else is playing -- so a reference vocal or a
    // backing take can sit in the project without ever being mixed into it.
    bool referenceOnly = false;
    float volume = 1.0f;
    float pan = 0.0f;
    bool smoothOverlaps = false;
    bool normalizeVolume = false;
    // UTAU tracks select samples from this directory instead of using the
    // MIDI clip's sourceFile as audio.  The path is stored per track so one
    // project can use several independent voicebanks.
    juce::File voicebankDirectory;
    // UTAU resampler positional parameter 4.  100 preserves the voicebank's
    // original consonant duration; larger values make consonants faster.
    int utauConsonantVelocity = 100;
    juce::String utauGlobalFlags;
    // The four-region ("Jie/UTAU") variant of the UTAU mode.  It is a flag on
    // the existing utau algorithm rather than a separate PitchAlgorithm value
    // because "is this a UTAU track?" is asked in ~27 places across the piano
    // roll, track list, engine and project model; a second enum value would
    // have to be added to every one of them, and any missed site would
    // silently drop an affordance from the new mode.
    UtauMode utauMode = UtauMode::classic;
    // MLD5/MLD3 are retained only for loading old projects.  New native
    // projects default to LLSM2; the UI promotes NSF-HiFiGAN when its model
    // pack is actually available.
    PitchAlgorithm pitchAlgorithm = defaultPitchAlgorithm();
    StretchAlgorithm stretchAlgorithm = StretchAlgorithm::melodyneHybrid;
    // Kept at processThenSplice so an existing project, and every track that
    // predates this field, renders exactly as it did before.
    RenderOrder renderOrder = RenderOrder::processThenSplice;
    std::vector<ClipData> clips;
};

// A native connection is explicit and can cross source files or HJM regions.
// The legacy note booleans remain as a compatibility projection for existing
// renderers; new editing code can identify both endpoints without guessing
// from their order on a track.
struct NativeConnection
{
    juce::String id;
    juce::String leftNoteId;
    juce::String rightNoteId;
    juce::String type = "pitch-and-amplitude";
    double boundarySeconds = 0.0;
    std::vector<PitchCurveEditPoint> pitchCurve;
    std::vector<AmplitudeEnvelopePoint> amplitudeCurve;
};

// Vibrato offset in cents at a time inside the note.  Shared by the piano roll
// and the renderer so what is drawn is what is heard.
double vibratoCentsAt(const NoteData& note, double localSeconds);

struct TempoChange
{
    // Musical position measured in quarter notes from beatOriginSeconds.
    double quarterPosition = 0.0;
    double bpm = 120.0;
};

struct ProjectData
{
    juce::String name = "Untitled";
    double bpm = 120.0;
    double beatOriginSeconds = 0.0;
    int numerator = 4;
    int denominator = 4;
    juce::String gridDivision = "1/16";
    // Smallest unit a note can be stretched or moved by, as 1/N of a beat.
    int noteEditDivision = 64;
    juce::String baseScale = "C";
    std::vector<TempoChange> tempoChanges;
    std::vector<NativeConnection> nativeConnections;
    std::vector<TrackData> tracks;

    [[nodiscard]] double durationSeconds() const;
    [[nodiscard]] double secondsForQuarterPosition(double quarterPosition) const;
    [[nodiscard]] double quarterPositionForSeconds(double seconds) const;
    [[nodiscard]] double tempoAtQuarterPosition(double quarterPosition) const;
    [[nodiscard]] double tempoAtSeconds(double seconds) const;
};

class ProjectModel final : public juce::ChangeBroadcaster
{
public:
    ProjectModel();

    [[nodiscard]] ProjectData snapshot() const;
    [[nodiscard]] std::uint64_t revisionNumber() const;
    void replace(ProjectData replacement);
    void clear();
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;

    [[nodiscard]] juce::String addAudioFile(const juce::File& file, double durationSeconds,
                                            double startSeconds = 0.0,
                                            const juce::String& targetTrackId = {});
    [[nodiscard]] juce::String addTrack(const juce::String& name, bool compose = true,
                                       bool referenceOnly = false);
    void setTrackReferenceOnly(const juce::String& trackId, bool referenceOnly);
    void setTrackName(const juce::String& trackId, const juce::String& name);
    bool setClipNotesIfEmpty(const juce::String& clipId, std::vector<NoteData> notes);
    bool addMidiFile(const juce::File& file, juce::String& error);

    // A UTAU project file, opened as a plain UTAU track: the notes, their
    // lyrics and timing, and the per-note UTAU parameters the renderer reads.
    // warnings collects what could not be carried over, which is worth saying
    // out loud rather than leaving it to be noticed missing.
    bool addUstFile(const juce::File& file, juce::String& error,
                    juce::StringArray& warnings);
    void setTempo(double bpm, int numerator, int denominator = 4);
    void setTempoChange(double quarterPosition, double bpm);
    // Digest of everything the project serialiser writes.  Use this to ask
    // "has anything changed?" -- it cannot miss a field the way a
    // hand-maintained projection of the project can.
    [[nodiscard]] juce::int64 contentFingerprint() const;
    void setGridDivision(const juce::String& division);
    void setNoteEditDivision(int division);
    void setBaseScale(const juce::String& scale);
    void setTrackCompose(const juce::String& trackId, bool enabled);
    void setTrackMuted(const juce::String& trackId, bool muted);
    void setTrackSolo(const juce::String& trackId, bool solo);
    void setTrackVolume(const juce::String& trackId, float volume);
    void setTrackPan(const juce::String& trackId, float pan);
    void setTrackSmoothOverlaps(const juce::String& trackId, bool enabled);
    void setTrackNormalizeVolume(const juce::String& trackId, bool enabled);
    void setTrackVoicebankDirectory(const juce::String& trackId,
                                    const juce::File& directory);
    void setTrackUtauConsonantVelocity(const juce::String& trackId, int velocity);
    void setTrackUtauGlobalFlags(const juce::String& trackId, const juce::String& flags);
    void setUtauMode(UtauMode mode);
    void setTrackUtauMode(const juce::String& trackId, UtauMode mode);
    void setNotesVibrato(const std::vector<juce::String>& noteIds,
                         const NoteData& parameters, bool enabled);
    void setNotesVibratoRealLine(const std::vector<juce::String>& noteIds, bool enabled);
    // Writes the swing into the note as ordinary pitch control points and
    // switches the vibrato off.  One undo step; once the resulting curve has
    // been edited by hand there is no way back to the parametric form.
    bool bakeNoteVibratoIntoPitch(const juce::String& noteId);
    void setNotesUtauSplice(const std::vector<juce::String>& noteIds, bool enabled);
    // Turning this on gives a note that has no curve yet a flat one at zero,
    // so there is something to drag rather than an empty lane.
    void setNotesUtauFlagCurveEnabled(const std::vector<juce::String>& noteIds,
                                      bool enabled);
    // Drop every flag curve on the given notes, back to what they looked like
    // the moment the switch went on.  Notes with the switch off are left
    // alone, curves and all: they are not showing a curve to be reset, and a
    // mixed selection should not quietly lose what it is holding for later.
    // Answers whether anything was actually dropped.
    bool resetNotesUtauFlagCurves(const std::vector<juce::String>& noteIds);
    // The same, for one flag: the notes' other curves are left as they are.
    bool resetNotesUtauFlagCurve(const std::vector<juce::String>& noteIds,
                                 const juce::String& flag);
    // An empty list drops that flag's curve; the others are left alone.
    bool setNoteUtauFlagCurve(const juce::String& noteId, const juce::String& flag,
                              std::vector<FlagCurvePoint> points);
    void setNotesRegionFlags(const std::vector<juce::String>& noteIds, bool split,
                             const juce::String& first, const juce::String& second,
                             const juce::String& third, const juce::String& fourth);
    // Hands the four regions back to the oto, forgetting a hand-placed split.
    void setNotesUtauJieSplitCleared(const std::vector<juce::String>& noteIds);
    void setNotesUtauJieSplit(const std::vector<juce::String>& noteIds,
                              double first, double second, double third);
    void clearNotesUtauJieSplit(const std::vector<juce::String>& noteIds);
    void setTrackPitchAlgorithm(const juce::String& trackId, PitchAlgorithm algorithm);
    void setTrackStretchAlgorithm(const juce::String& trackId, StretchAlgorithm algorithm);
    void setPitchAlgorithm(PitchAlgorithm algorithm);
    void setStretchAlgorithm(StretchAlgorithm algorithm);
    void setTrackRenderOrder(const juce::String& trackId, RenderOrder order);
    void setRenderOrder(RenderOrder order);
    void moveClip(const juce::String& clipId, double startSeconds);
    [[nodiscard]] juce::String duplicateClip(const juce::String& clipId,
                                             double startSeconds = -1.0,
                                             const juce::String& targetTrackId = {});
    void resizeClip(const juce::String& clipId, double startSeconds,
                    double durationSeconds);
    void setClipGain(const juce::String& clipId, float gain);
    void setClipFades(const juce::String& clipId, double fadeInSeconds,
                      double fadeOutSeconds);
    void setClipMuted(const juce::String& clipId, bool muted);
    void removeClip(const juce::String& clipId);
    void removeTrack(const juce::String& trackId);
    void transposeNote(const juce::String& noteId, float semitones);
    void transposeNotes(const std::vector<juce::String>& noteIds, float semitones);
    // Move a group of UTAU notes as one phrase.  When the destination phrase
    // overlaps an existing note, the phrase is inserted immediately before
    // the first collision and the following notes are shifted to the right.
    bool moveUtauNotes(const std::vector<juce::String>& noteIds,
                       double deltaSeconds, float semitones);
    void setNotesMidi(const std::vector<juce::String>& noteIds, float midiNote);
    void averageNotesMidi(const std::vector<juce::String>& noteIds);
    void quantizeNotesMidi(const std::vector<juce::String>& noteIds, float stepSemitones = 1.0f);
    // Which notes of a clip a stretch of the timeline lands on.  Pasting asks
    // this before it writes anything, so it can say what it is about to
    // replace.
    [[nodiscard]] std::vector<juce::String> notesOverlapping(
        const juce::String& clipId, double fromSeconds, double toSeconds) const;
    [[nodiscard]] std::vector<juce::String> insertNotes(
        const juce::String& clipId, const std::vector<NoteData>& noteTemplates,
        double absoluteStartSeconds);
    [[nodiscard]] std::vector<juce::String> duplicateNotes(
        const std::vector<juce::String>& noteIds, const juce::String& targetClipId,
        double absoluteStartSeconds);
    void resizeNote(const juce::String& noteId, double newStart, double newDuration);
    [[nodiscard]] juce::String splitNote(const juce::String& noteId,
                                         double localSeconds);
    [[nodiscard]] juce::String mergeNotes(const std::vector<juce::String>& noteIds);
    // Explicitly connect selected notes without destructively merging their
    // editable data. Pairs may cross clips and source-material regions.
    void setNotesConnection(const std::vector<juce::String>& noteIds, bool enabled);
    // Inserts a zero-length lead-in note in front of this one carrying the same
    // lyric, and pins the following note's preutterance to 0 with the given
    // overlap, so a consonant can be sung ahead of the beat.  Returns the new
    // note's id, or empty when the note is already at the clip start.
    [[nodiscard]] juce::String insertPrefixNote(const juce::String& noteId,
                                                double targetOverlapSeconds);
    void setNoteModulation(const juce::String& noteId, float modulation);
    void setNoteDrift(const juce::String& noteId, float drift);
    void setNoteTension(const juce::String& noteId, float tension);
    void setNoteBreath(const juce::String& noteId, float breath);
    void setNoteFormant(const juce::String& noteId, float semitones);
    void setNoteGain(const juce::String& noteId, float gain);
    void setNoteAttack(const juce::String& noteId, double consonantSeconds, float attackSpeed);
    void setNoteAttackSpeed(const juce::String& noteId, float attackSpeed);
    void setNoteLabel(const juce::String& noteId, const juce::String& label);
    // Several at once, as one undoable step.  Typing a line of lyrics is one
    // action to the person doing it, so one Ctrl+Z should take it back rather
    // than walking the phrase backwards a syllable at a time.
    void setNoteLabels(const std::vector<std::pair<juce::String, juce::String>>& labels);
    // Convert Chinese lyrics without tying the command to an import source or
    // renderer; callers decide which track/selection to apply it to.
    int convertTrackLyricsToPinyin(const juce::String& trackId);
    void setNoteUtauFlags(const juce::String& noteId, const juce::String& flags);
    void setNotesUtauFlags(const std::vector<juce::String>& noteIds,
                           const juce::String& flags);
    void setNotesUtauConsonantVelocity(const std::vector<juce::String>& noteIds,
                                       int velocity);
    // Sets the STP of these notes, in seconds.  One undoable step.
    void setNotesUtauStp(const std::vector<juce::String>& noteIds, double seconds);
    void setNoteUtauTimingOverrides(const juce::String& noteId, bool enabled,
                                    double preutteranceSeconds,
                                    double overlapSeconds);
    void setNoteRobustPitchCurve(const juce::String& noteId, bool enabled);
    bool setNotePitchCurve(const juce::String& noteId,
                           std::vector<PitchCurveEditPoint> points,
                           bool storeControlPoints = false);
    bool setNoteAmplitudeEnvelope(const juce::String& noteId,
                                  std::vector<AmplitudeEnvelopePoint> points);
    bool setNotesAmplitudeEnvelopes(
        std::vector<std::pair<juce::String, std::vector<AmplitudeEnvelopePoint>>> envelopes);
    // Scales the whole amplitude envelope of these notes by a base percent
    // (100 = unchanged, 200 = twice as loud, 0 = silence) without reshaping it.
    void setNotesAmplitudeEnvelopeBase(const std::vector<juce::String>& noteIds,
                                       float basePercent);
    // Where a new note goes, and whether there is room for one at all.
    //
    // Notes on one track are a sequence, not a chord: the UTAU modes splice
    // them end to end and every other mode reads them the same way.  Two that
    // share a moment are not a thicker sound, they are a phrase with no
    // defined order.  So a request landing inside an existing note is
    // refused, and one with room is trimmed to stop where the next begins.
    //
    // It lives here rather than in the tool that calls it because only here
    // are the notes certainly current.  A view holds a snapshot that refreshes
    // on a message, so two quick clicks can both read a list that is one note
    // out of date -- which is one of the ways overlapping notes were made.
    struct NoteSpan { double startSeconds = 0.0; double durationSeconds = 0.0; };
    struct PlannedNote
    {
        bool create = false;
        double startSeconds = 0.0;
        double durationSeconds = 0.0;
    };
    // All times clip-local.  Pure, and public so a check reads the same rule
    // the model applies.
    [[nodiscard]] static PlannedNote plannedNoteFor(double startSeconds,
                                                    double requestedSeconds,
                                                    double clipSeconds,
                                                    const std::vector<NoteSpan>& occupied);
    // An empty stretch of timeline on a melodic track, for notes to be drawn
    // into.  A track starts life with no clips and nothing else in the
    // interface makes one, so without this a new track cannot be drawn on at
    // all.  Returns an empty id for an audio track or an unknown one.
    [[nodiscard]] juce::String addClip(const juce::String& trackId,
                                       double startSeconds, double durationSeconds);
    // Which of a recording's sidecar regions become notes.
    //
    // One recording usually serves several oto entries: New Geping's a.wav is
    // 0.41 seconds of one syllable with three rows against it -- "a", "a -"
    // and "a" again -- each a different alias over the same sound.  Taken one
    // per row they arrive as three notes stacked on top of each other, which
    // reads as a syllable mysteriously split and, on a track that can only
    // sing one note at a time, leaves all but one of them silent.
    //
    // So overlapping rows are alternatives, not a sequence: the fullest one
    // wins and the rest are dropped.  Rows that do not overlap are separate
    // sounds in one file and are all kept.  Returns the indices to keep, in
    // the order given.  Pure, and public so a check reads the same rule the
    // importer acts on.
    struct RegionSpan { double startSeconds = 0.0; double endSeconds = 0.0; };
    [[nodiscard]] static std::vector<std::size_t> regionsToImport(
        const std::vector<RegionSpan>& regions);
    // The first moment at or after the one asked for that is not already
    // sounding.  Inside a note there is no room for another, so the answer is
    // where that note ends -- and where notes abut, after the last of them.
    // Pure, and public so a check reads the same rule the tool acts on.
    // Lays the pitch line flat on each note's own pitch: every frame
    // targeted at the note -- the unvoiced ones too, so a stretch where
    // nothing was detected moves with the rest instead of keeping whatever it
    // had -- drift and modulation off, and two anchors left.  One undoable
    // step for the lot.
    //
    // This sets where the note is asked to go.  It does not touch the
    // measured curve it is moved away from, and under mld5 the sound is
    // shifted by the difference between the two -- so an octave error in the
    // measurement still comes out as an octave error in the result.
    void flattenNotePitch(const std::vector<juce::String>& noteIds);
    [[nodiscard]] static double firstFreeStartFrom(double startSeconds,
                                                   const std::vector<NoteSpan>& occupied);
    // Returns an empty id when there is no room, rather than making an overlap.
    [[nodiscard]] juce::String addNote(const juce::String& preferredClipId,
                                       double absoluteStart, double duration, float midiNote);
    // Draws a note starting at the first free moment from here, no longer than
    // asked and no longer than the room before whatever follows.
    //
    // Separate from addNote because the search has to happen under the lock:
    // a view works from a snapshot that refreshes on a message, so it can be a
    // note out of date and would pick a start that is no longer free.
    [[nodiscard]] juce::String addNoteFrom(const juce::String& preferredClipId,
                                           double absoluteFromSeconds,
                                           double maximumDuration, float midiNote);
    void removeNote(const juce::String& noteId);
    void removeNotes(const std::vector<juce::String>& noteIds);
    // Removes the notes and the stretch of time they occupied: whatever
    // followed moves back to meet whatever came before, and the clip loses
    // that much length.  The inverse of pasting a phrase in.
    void removeNotesRippling(const std::vector<juce::String>& noteIds);
    // Open a silence in front of a note: everything from that note on, all
    // through its track, moves later by that much and the track grows by it.
    // The inverse of removeNotesRippling, which closes one up.
    void insertGapBeforeNote(const juce::String& noteId, double seconds);
    // Close a silence in front of a note: that note and everything after it,
    // all through its track, move earlier by that much and the track loses it.
    void closeGapBeforeNote(const juce::String& noteId, double seconds);
    void toggleNoteConnection(const juce::String& noteId);
    void applySourceSettings(const juce::File& source,
                             const std::vector<SampleRegionSetting>& rows);

    bool save(const juce::File& file, juce::String& error) const;
    bool load(const juce::File& file, juce::String& error);

private:
    void pushUndoLocked();
    // Whether a clip's source is a recording -- audio the engine plays
    // straight out when there are no notes left to play it through.
    //
    // Not simply "has a source file": a UST clip keeps the .ust as its
    // source too, and a score file makes no sound of its own.  Named the
    // other way round -- everything that is not one of the score formats is
    // audio -- so that a new audio format the importer learns is treated
    // correctly without anyone remembering to come back here.
    [[nodiscard]] static bool clipIsRecording(const ClipData& clip);
    // Drops any clip that is a recording and has just lost its last note.
    //
    // The notes are what a recording is played through; with none left the
    // clip renders its source straight out, so deleting every note left the
    // material sounding at full volume with its waveform still drawn --
    // measured at 0.1996 RMS against 0 for no clip at all.  Deleting the
    // notes is how the sound is meant to go away, so the recording goes with
    // them.  A composed clip has nothing to sound and is kept: with no notes
    // it is a silent span of timeline, which is what a new track is drawn
    // into.  Call with the lock, inside an operation that has already pushed
    // an undo step, so the notes and the clip come back together.
    void dropEmptyRecordedClipsLocked();
    // The one place a drawn note is built, so the two entry points cannot
    // drift apart over what a new note starts life as.  Call with the lock.
    [[nodiscard]] juce::String addNoteLocked(ClipData& destination,
                                             const PlannedNote& planned, float midiNote);
    juce::ValueTree toValueTree(const juce::File& projectFile) const;
    static ProjectData fromValueTree(const juce::ValueTree& tree,
                                     const juce::File& projectFile);
    static juce::String makeId(const char* prefix);

    mutable juce::CriticalSection lock;
    ProjectData project;
    std::vector<ProjectData> undoHistory;
    std::vector<ProjectData> redoHistory;
    std::uint64_t revision = 0;
    // Keep large Melodyne projects bounded: snapshots are full native project
    // states, so a short history is preferable to unbounded contour copies.
    static constexpr std::size_t maxHistory = 24;
};
}
