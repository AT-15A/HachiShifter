#pragma once

#include "ProjectModel.h"
#include "backend/RenderService.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace hachi
{
// One rendered UTAU note, summarised for drawing.
//
// A UTAU note has no source file to take a thumbnail of: what there is to
// show is what came back from the resampler.  So the peaks are measured once,
// when the render lands, and the note carries the hash it had at that moment
// -- edit the note and the hash no longer matches, which is how a waveform
// stops being shown for a note that is no longer the one that produced it.
struct UtauNoteWaveform
{
    juce::String noteId;
    std::uint64_t renderHash = 0;
    double startSeconds = 0.0;      // on the timeline
    double durationSeconds = 0.0;
    // One bucket per millisecond: the extremes of the samples inside it.
    std::vector<float> minima;
    std::vector<float> maxima;
};

class AudioEngine final : public juce::AudioSource, public juce::ChangeBroadcaster
{
public:
    AudioEngine();
    ~AudioEngine() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    void syncProject(const ProjectData& project);
    // Stop at this point in the piece rather than at its end.  Zero clears it.
    // Re-armed on every start, so it cannot outlive the run it was set for.
    void setPlayUntil(double seconds);
    void setUtauRenderNoteSelection(const std::vector<juce::String>& noteIds);
    // The track being worked on.  A material track is heard only while it is
    // this one; stored here and read when the mix is built, so it takes
    // effect at the next syncProject like every other change.
    void setAuditionTrack(const juce::String& trackId);
    // Whether a track is heard at all.  Pure, and public so a check reads the
    // same rule the mixer applies.
    [[nodiscard]] static bool trackIsAudible(bool muted, bool solo, bool anySolo,
                                             bool referenceOnly, bool beingWorkedOn);
    // Widen the audition scope to every UTAU note in the song, which is what
    // an export needs: rendering is selection-driven, so exporting whatever
    // was last selected would write one phrase and silence elsewhere.
    // Returns how many notes are now in scope.
    int selectEveryUtauNote(const ProjectData& project);
    bool selectAllRenderedUtauNotes();
    void setHifiganModelDirectory(const juce::File& directory);
    // Naming no resampler, or one that is not there, falls back to an engine
    // shipped beside the application.  This lives here rather than in the
    // window because the window is not the only thing that renders: the
    // headless MCP server drives the same engine and had no way to find it.
    void setUtauResamplerFile(const juce::File& executable);

    // What that falls back to: engines\WCSNDM.exe or WCSNDM.exe beside the
    // running executable, and nothing if neither is there.  Pure, and public
    // so a check reads the same rule the engine resolves by.
    [[nodiscard]] static juce::File bundledUtauResampler(
        const juce::File& executableDirectory);
    // Which of the two a caller's choice resolves to.
    [[nodiscard]] static juce::File resolveUtauResampler(
        const juce::String& configured, const juce::File& executableDirectory);

    // Which engine this instance would actually drive.  The rule above is
    // pure and testable on its own, but a rule nobody calls is worth nothing:
    // this reports what the real constructor and the real setter settled on,
    // for the running executable's own location.
    [[nodiscard]] juce::File diagnosticUtauResamplerFile() const
    {
        return utauResamplerFile;
    }
    void setInferenceConfiguration(backend::InferenceBackend inference, int deviceIndex);
    [[nodiscard]] std::optional<double> probeDuration(const juce::File& file);
    bool setAuditionFile(const juce::File& file);
    void clearAuditionFile();

    void play();
    void stop();
    void setPosition(double seconds);
    [[nodiscard]] double position() const;
    [[nodiscard]] float trackPeak(const juce::String& trackId) const;
    [[nodiscard]] std::optional<double> renderProgress() const;
    [[nodiscard]] bool hasPlayableRenderedAudio() const;
    [[nodiscard]] bool hasCurrentRenderedAudio() const;
    bool rewindToFirstPlayableRenderedAudio(double leadInSeconds = 0.03);
    [[nodiscard]] juce::String activeRenderBackends() const;
    [[nodiscard]] juce::String activeRenderWarnings() const;
    // Which edits the tracks carry that their selected render backend cannot
    // honour.  Editing is unified across UTAU/Melodyne/native, so a feature is
    // never hidden while editing; this is where the difference finally shows,
    // as a warning naming the unrenderable edit rather than a silent drop.
    // Pure and static so the offline check and the UI read the same rule.
    [[nodiscard]] static juce::StringArray renderCapabilityWarnings(
        const ProjectData& project);
    // trackId exports that track alone, for one file per track; empty
    // exports the mix.  Whether a track sounds at all is still decided by
    // mute and solo, exactly as in playback.
    // fromSeconds/toSeconds write only that stretch of the timeline; a range
    // that is not longer than nothing writes the whole song.
    // Whether a clip meets that neighbour at a Melodyne pitch join: the two
    // butt up in time and the notes on either side say they are joined.  The
    // mixer crossfades such a seam, and the neural decoder is told not to fade
    // its own edge into it as well.  Pure, and public so the offline check
    // reads the same rule the mixer and the renderer do.
    [[nodiscard]] static bool clipsJoinAt(const ClipData& clip,
                                          const ClipData& neighbour, bool asNext);

    // The chains stretchSpliceThenPitch decodes in one pass each: runs of two
    // or more clips joined by Melodyne pitch joins over one continuous stretch
    // of one recording.  Clips are given in start order; each chain comes back
    // in playing order.  Pure, and public so a check reads the same rule the
    // engine groups by.
    [[nodiscard]] static std::vector<std::vector<const ClipData*>> glideChains(
        const std::vector<const ClipData*>& orderedClips);

    // The single request covering one such chain: the clips' time maps laid
    // end to end, and one pitch line whose every seam is glided out of the
    // previous clip's tail.  Public for the same reason.
    [[nodiscard]] static backend::Mld5FileRenderRequest mergedRequestFor(
        const std::vector<const ClipData*>& group, const TrackData& track,
        const juce::File& hifiganModelDirectory,
        const backend::OrtExecutionConfig& inference);

    // How many phrases the last sync decided to decode in one pass.
    [[nodiscard]] int diagnosticMergedPhraseCount() const;

    // The per-frame target MIDI the native render request would carry for one
    // clip, so the automatic pitch-seam S-transition can be tested without the
    // ONNX model.  Empty when the indices are out of range.
    [[nodiscard]] static std::vector<float> diagnosticNativeTargetMidi(
        const ProjectData& project, int trackIndex, int clipIndex);

    // The rendered UTAU notes, as peaks to draw.  Shared and immutable, so the
    // roll can hold one while a render replaces it.  Only notes whose audio is
    // ready are in it; whether the note has since been edited is answered by
    // comparing renderHash against the note as it now stands.
    [[nodiscard]] std::shared_ptr<const std::vector<UtauNoteWaveform>>
        utauNoteWaveforms() const;

    // Re-measure which rendered notes are available.  Cheap -- it copies
    // peaks already computed -- and called from the window's timer, which
    // is where a render finishing is noticed.
    void refreshUtauWaveforms();

    // The note as the render cache sees it, in one number: two notes with the
    // same value render the same audio.  Public because the roll asks it of a
    // note under a drawn waveform, to find out whether it is still the note
    // that produced it.
    [[nodiscard]] static std::uint64_t utauNoteRenderHash(const NoteData& note);

    bool exportWav(const juce::File& file, juce::String& error,
                   const juce::String& trackId = {},
                   double fromSeconds = 0.0, double toSeconds = 0.0);
    [[nodiscard]] bool isPlaying() const { return playing.load(); }
    [[nodiscard]] juce::AudioDeviceManager& devices() { return deviceManager; }
    bool ensureOutputDevice(juce::String& error);
    void restoreDeviceState(juce::PropertiesFile& properties);
    void saveDeviceState(juce::PropertiesFile& properties) const;

private:
    struct RenderedClip
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;
        double timelineOffsetSeconds = 0.0;
        int firstAudibleSample = 0;
        int lastAudibleSample = -1;
        juce::String backend;
        juce::String warning;
        std::atomic<bool> scheduled { false };
        std::atomic<bool> ready { false };
        std::atomic<bool> finished { false };
        std::atomic<float> progress { 0.0f };
        // Under stretchSpliceThenPitch a whole glide chain is decoded in one
        // pass; this entry then holds the phrase, and each target is the span
        // belonging to one clip.  A target is filled the moment the phrase
        // arrives, or straight away if the cached phrase is already here.
        struct SliceTarget
        {
            std::shared_ptr<RenderedClip> clip;
            int startSample = 0;
            int sampleCount = 0;
        };
        std::vector<SliceTarget> pendingSlices;
        juce::CriticalSection sliceLock;
        // Filled when a UTAU render lands, and read by the roll through the
        // snapshot below.  Guarded by sliceLock, which is already the lock
        // this entry uses for things a render thread writes and the message
        // thread reads.
        std::vector<UtauNoteWaveform> utauWaveforms;
    };

    struct LoadedClip
    {
        ClipData clip;
        std::string trackId;
        bool smoothOverlaps = false;
        float trackGain = 1.0f;
        float trackPan = 0.0f;
        std::shared_ptr<std::atomic<float>> meter;
        std::shared_ptr<RenderedClip> rendered;
        std::shared_ptr<RenderedClip> fallbackRendered;
        std::shared_ptr<juce::AudioFormatReader> reader;
        juce::AudioBuffer<float> scratch;
    };

    static float fadeEnvelope(const ClipData& clip, double localSeconds);
    void rebuildLoadedClips(const ProjectData& project);

    juce::AudioFormatManager formatManager;
    backend::RenderService renderService;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer sourcePlayer;
    mutable juce::ReadWriteLock renderLock;
    std::vector<std::unique_ptr<LoadedClip>> loadedClips;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<float>>> trackMeters;
    std::unordered_map<std::string, std::shared_ptr<RenderedClip>> renderCache;
    std::unordered_map<std::string, std::shared_ptr<RenderedClip>> playbackFallbackByClip;
    std::string exportTrackFilter;
    std::unordered_set<std::string> utauRenderNoteSelection;
    // The track being worked on, for the material-track rule above.
    juce::String auditionTrackId;
    std::unordered_set<std::string> utauRenderedNoteHistory;
    juce::File hifiganModelDirectory;
    juce::File utauResamplerFile;
    // Rebuilt whenever a UTAU render lands or the project is synced, and
    // handed out by pointer so a paint never walks a vector being rewritten.
    std::shared_ptr<const std::vector<UtauNoteWaveform>> utauWaveformSnapshot;
    mutable juce::CriticalSection utauWaveformLock;
    // The window asks thirty times a second whether the peaks have changed,
    // and rebuilding the snapshot copies every note's buckets.  Bumped when a
    // render lands or the clips are rebuilt; compared before doing the work.
    std::atomic<std::uint64_t> utauWaveformGeneration { 0 };
    std::uint64_t utauWaveformSnapshotGeneration = 0;
    void refreshUtauWaveformSnapshot();
    backend::OrtExecutionConfig inferenceConfiguration;
    std::shared_ptr<juce::AudioFormatReader> auditionReader;
    juce::AudioBuffer<float> auditionScratch;
    std::atomic<bool> auditionMode { false };
    std::atomic<bool> playing { false };
    std::atomic<juce::int64> timelineSample { 0 };
    // Where the project was left standing while a sample is being auditioned.
    // Auditioning plays a file on its own timeline from zero, and without
    // somewhere to put it the project's own position was simply lost.
    std::atomic<juce::int64> projectTimelineSample { 0 };
    // Where this run of playback ends, or zero for the end of the piece.
    // Playing a selection should finish with the selection.
    std::atomic<double> playUntilSeconds { 0.0 };
    std::atomic<double> outputSampleRate { 48'000.0 };
    std::atomic<double> projectDurationSeconds { 0.0 };
    std::atomic<float> masterLimiterGain { 1.0f };
    std::atomic<bool> offlineRendering { false };
};
}
