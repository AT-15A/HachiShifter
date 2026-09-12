#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace hachi::backend
{
struct UtauPitchPoint
{
    double timeSeconds = 0.0;
    float cents = 0.0f;
};

struct UtauAmplitudePoint
{
    double timeSeconds = 0.0;
    float gainDb = 0.0f;
};

struct UtauNoteRenderSpec
{
    juce::String alias;
    juce::String flags;
    double startSeconds = 0.0;
    double durationSeconds = 0.25;
    float midiNote = 60.0f;
    float gain = 1.0f;
    std::vector<UtauPitchPoint> pitchCurve;
    std::vector<UtauAmplitudePoint> amplitudeEnvelope;
    int consonantVelocity = 100;
    bool preutteranceOverrideEnabled = false;
    double preutteranceSeconds = 0.0;
    bool overlapOverrideEnabled = false;
    double overlapSeconds = 0.0;
    // Local tempo at this note's musical start.  Zero uses request.bpm for
    // compatibility with older callers and smoke fixtures.
    double bpm = 0.0;
    // Manual four-region split for this note, as three cumulative fractions
    // of the sounding span.  Unset lets the regions be allocated by weight.
    // Declared last: several callers build this struct positionally.
    bool jieSplitSet = false;
    std::array<double, 3> jieSplit {};
    // Per-region flags; empty entries mean the region uses the note flags.
    bool flagSplit = false;
    std::array<juce::String, 4> regionFlags {};
    // A per-frame curve for g.  Times are relative to the nominal note start,
    // like the amplitude envelope; the engine measures from the start of the
    // rendered segment, which is a preutterance earlier.  While this is on the
    // g written in the flags text is ignored.
    bool flagCurve = false;
    // Already sampled: a shaped segment is handed over as enough plain points
    // for the engine's straight-line reading to follow it.  One entry per flag
    // that has a curve, named as the engine names it.
    std::vector<std::pair<juce::String, std::vector<std::pair<double, double>>>>
        flagCurves;
    // Crossfade into the note before this one at mix time.  Affects only
    // the mix, never the render, so it is not part of a note's cache key.
    bool splice = false;
    // STP: the whole oto entry is moved this far inside the recording before
    // anything is read from it.  Positive reads later in the file, negative
    // earlier.  Declared last, like the fields above it, because several
    // callers build this struct positionally.
    double stpSeconds = 0.0;
};

struct UtauRenderRequest
{
    juce::File voicebankDirectory;
    juce::File resamplerExecutable;
    bool fourRegion = false;
    // 谋•UTAU: read 谋•OTO and hand the engine the per-region classes.  Off
    // for UTAU and 界•UTAU, which have no such annotation -- with it off the
    // engine is sent exactly what it was sent before.
    bool consonantClasses = false;
    double targetDurationSeconds = 0.0;
    double bpm = 120.0;
    std::vector<UtauNoteRenderSpec> notes;
    std::function<void(double)> progress;
};

struct UtauRenderResult
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
    juce::String backend;
    juce::String warning;
};

// The four-region split of one note, in seconds, onset/glide/nucleus/coda.
struct UtauRegionSplit
{
    bool valid = false;
    std::array<double, 4> seconds {};
};

// A note whose lyric is this is a rest: it keeps its place in the phrase and
// sounds nothing.  Two letters rather than one, because a single "R" is a real
// alias in the Chinese CVVC banks -- they record the release of every vowel as
// "a R", "ai R" and so on -- and a lyric of "R" should go on reaching it.
// Not the same as an empty lyric, which renders the piano preview tone.
[[nodiscard]] bool isRestLyric(const juce::String& lyric);

struct UtauSampleTiming
{
    double preutteranceSeconds = 0.0;
    double consonantSeconds = 0.0;
    double overlapSeconds = 0.0;
    // Source lengths of the four regions, from the voicebank's oto4.ini.
    // Empty unless the sample has been extended to four regions.
    bool hasRegions = false;
    std::array<double, 4> regionSeconds {};
    // One letter per region from 谋-OTO; empty in every other mode.
    juce::String mouClasses;
    // Offset to cutoff: all the audio this entry has, and so the furthest a
    // lead-in can reach back before the beat.
    double sampleSeconds = 0.0;
    // The oto alias the lyric actually resolved to.  With a prefix.map that
    // is the lyric wrapped in the prefix and suffix its pitch calls for, so
    // it says which pitch bank of a multi-bank voicebank was reached.
    juce::String resolvedAlias;
};

class UtauRenderer final
{
public:
    static void invalidateVoicebankCache();
    // Where the crossfade into the next note finishes, which is where the
    // note before it stops sounding.
    //
    // The next note begins sounding a preutterance before its beat and fades
    // in across its overlap; the note before it fades out over the same
    // stretch.  Never past the next note's own end: beyond there the note
    // after it owns the seam, and an overlap typed longer than the note it
    // belongs to would leave the previous syllable droning under the phrase.
    [[nodiscard]] static double crossfadeEnd(double soundStart, double overlap,
                                             double nextNoteEnd);
    // Whether there is a crossfade between them at all.
    //
    // Contiguous counts.  A preutterance of zero puts the next note's sounding
    // start exactly on this note's end, and asking whether it starts *before*
    // that end answered no: the overlap was ignored for every note with no
    // lead-in, which is most of a plain CV bank.  Only a real gap -- the next
    // note beginning to sound after this one has finished -- has no seam.
    [[nodiscard]] static bool crossfadesInto(double soundStart, double noteEnd);
    [[nodiscard]] static std::optional<UtauSampleTiming> sampleTiming(
        const juce::File& voicebankDirectory, const juce::String& alias, float midiNote,
        int consonantVelocity = 100, bool fourRegion = false,
        bool consonantClasses = false);
    static UtauRenderResult render(const UtauRenderRequest& request);
    // How one note's four regions divide its output.  With no manual split
    // this reproduces the engine's own weight allocation, so what the piano
    // roll draws is what the resampler is asked for.
    // leadInSeconds is how far the sample reaches back before the note
    // begins.  The onset is the consonant, and the consonant is what is sung
    // before the beat, so the onset ends where the note starts: that is the
    // whole meaning of a preutterance.  Its own length follows from that.
    [[nodiscard]] static UtauRegionSplit regionSplit(
        const std::array<double, 4>& sourceSeconds, double outputSeconds,
        int consonantVelocity, double leadInSeconds,
        const std::array<double, 3>* manualFractions,
        // 谋-OTO classes, one letter per region.  Null in every other
        // mode, and then this behaves exactly as it always did.
        const juce::String* classes = nullptr);
};
}
