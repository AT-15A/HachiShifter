#include "ProjectModel.h"
#include "SampleSettings.h"
#include "backend/UstImporter.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace hachi
{
float renderedPitchCents(const NoteData& note, const PitchPoint& point)
{
    if (point.hasManualTarget) return point.manualTargetCents;

    // Melodyne stores an exact zero for notes flattened with the pitch
    // modulation tool.  Some analysed source points do not contain a separate
    // pitchWithoutVibrato curve (it is byte-for-byte equal to pitchCent), so
    // the generic drift/modulation decomposition would otherwise leave the
    // original contour untouched.  Treat the saved zero as the authoritative
    // flat-note edit for both display and rendering.
    if (note.modulation <= 1.0e-4f) return 0.0f;

    return note.drift * point.withoutVibratoCents
        + note.modulation * (point.relativeCents - point.withoutVibratoCents);
}

float shapedSegmentProgress(PitchCurveShape shape, float u,
                            float bezierX1, float bezierY1,
                            float bezierX2, float bezierY2)
{
    u = juce::jlimit(0.0f, 1.0f, u);
    switch (shape)
    {
        case PitchCurveShape::smooth: return u * u * (3.0f - 2.0f * u);
        case PitchCurveShape::easeIn: return u * u;
        case PitchCurveShape::easeOut: return 1.0f - (1.0f - u) * (1.0f - u);
        case PitchCurveShape::customBezier: break;
        // A contour-wide shape has no per-segment answer: whoever owns a whole
        // contour handles that one, and everything else is a straight line.
        case PitchCurveShape::natural:
        case PitchCurveShape::linear:
        default: return u;
    }
    // Invert the Bezier X component to turn the normalised timeline U into the
    // curve parameter T, then evaluate Y.  Newton iteration is fast for
    // ordinary handles; bisection is the fallback around flat derivatives.
    const auto x1 = juce::jlimit(0.0f, 1.0f, bezierX1);
    const auto x2 = juce::jlimit(0.0f, 1.0f, bezierX2);
    const auto cubic = [](float t, float first, float second)
    {
        const auto inverse = 1.0f - t;
        return 3.0f * inverse * inverse * t * first
            + 3.0f * inverse * t * t * second + t * t * t;
    };
    const auto derivative = [](float t, float first, float second)
    {
        const auto inverse = 1.0f - t;
        return 3.0f * inverse * inverse * first
            + 6.0f * inverse * t * (second - first)
            + 3.0f * t * t * (1.0f - second);
    };
    auto parameter = u;
    for (auto iteration = 0; iteration < 6; ++iteration)
    {
        const auto slope = derivative(parameter, x1, x2);
        if (std::abs(slope) < 1.0e-5f) break;
        const auto candidate = parameter - (cubic(parameter, x1, x2) - u) / slope;
        if (candidate < 0.0f || candidate > 1.0f) break;
        parameter = candidate;
    }
    auto lower = 0.0f;
    auto upper = 1.0f;
    for (auto iteration = 0; iteration < 18; ++iteration)
    {
        if (cubic(parameter, x1, x2) < u) lower = parameter;
        else upper = parameter;
        parameter = (lower + upper) * 0.5f;
    }
    return cubic(parameter, bezierY1, bezierY2);
}

juce::String utauModeLabel(UtauMode mode)
{
    return mode == UtauMode::jie ? juce::String::fromUTF8("界•UTAU")
         : mode == UtauMode::mou ? juce::String::fromUTF8("谋•UTAU")
                                 : juce::String("UTAU");
}

juce::String utauModeKey(UtauMode mode)
{
    // "utau4" is what 界 has always been called in project files and over
    // MCP, so it stays exactly that; only the new mode needs a new word.
    return mode == UtauMode::jie ? juce::String("utau4")
         : mode == UtauMode::mou ? juce::String("utaumou")
                                 : juce::String("utau");
}

int utauModePickerItem(UtauMode mode)
{
    return mode == UtauMode::jie ? 8 : mode == UtauMode::mou ? 9 : 7;
}

std::optional<UtauMode> utauModeForPickerItem(int itemId)
{
    switch (itemId)
    {
        case 7: return UtauMode::classic;
        case 8: return UtauMode::jie;
        case 9: return UtauMode::mou;
        default: return std::nullopt;
    }
}

UtauMode parseUtauMode(const juce::String& text)
{
    const auto value = text.trim().toLowerCase();
    if (value == "utau4" || value == "jie") return UtauMode::jie;
    if (value == "utaumou" || value == "mou") return UtauMode::mou;
    return UtauMode::classic;
}

const std::vector<FlagCurveKind>& flagCurveKinds()
{
    // Ordered as they are offered: the formant shift first, since it is what a
    // curve is usually wanted for, then the rest of the timbre controls.
    static const std::vector<FlagCurveKind> kinds {
        { "g",  "共振峰平移", -50.0f,  50.0f },
        { "Mt", "张力",      -100.0f, 100.0f },
        { "Rd", "声门 Rd",    -100.0f, 100.0f },
        { "Mo", "开口度",     -100.0f, 100.0f },
        { "ME", "共振峰强调", -100.0f, 100.0f },
        { "Mr", "歌手共振峰", -100.0f, 100.0f },
        { "MH", "高频滚降",   -100.0f, 100.0f },
        { "Mq", "高次谐波滚降", 0.0f,  100.0f },
        { "Mf", "共振峰调谐",   0.0f,  100.0f },
        { "Mb", "元音气声",   -100.0f, 100.0f },
        { "Ab", "全帧气声",   -100.0f, 100.0f },
        { "Md", "干燥度",     -100.0f, 100.0f },
        { "Mn", "噪声平滑",   -100.0f, 100.0f },
        { "NA", "鼻音度",     -100.0f, 100.0f },
        { "RG", "自动混声",   -100.0f, 100.0f },
        { "b",  "清辅音噪声",  -20.0f, 100.0f, true },
        { "bh", "辅音区谐波",  -20.0f, 100.0f, true }
    };
    return kinds;
}

const FlagCurveKind& flagCurveKindFor(const juce::String& flag)
{
    for (const auto& kind : flagCurveKinds())
        if (flag == kind.flag) return kind;
    return flagCurveKinds().front();
}

std::vector<FlagCurvePoint> flagCurvePointsFor(const NoteData& note,
                                               const juce::String& flag)
{
    for (const auto& curve : note.utauFlagCurves)
        if (curve.flag == flag) return curve.points;
    return {};
}

float flagCurveValueAt(const std::vector<FlagCurvePoint>& points, double timeSeconds)
{
    if (points.empty()) return 0.0f;
    if (timeSeconds <= points.front().timeSeconds) return points.front().value;
    if (timeSeconds >= points.back().timeSeconds) return points.back().value;
    const auto right = std::upper_bound(points.begin(), points.end(), timeSeconds,
        [](double value, const FlagCurvePoint& point)
        {
            return value < point.timeSeconds;
        });
    if (right == points.begin()) return points.front().value;
    if (right == points.end()) return points.back().value;
    const auto& next = *right;
    const auto& left = *std::prev(right);
    const auto span = next.timeSeconds - left.timeSeconds;
    if (span <= 1.0e-9) return next.value;
    const auto u = static_cast<float>((timeSeconds - left.timeSeconds) / span);
    // The shape belongs to the segment arriving at a point, as on the pitch
    // line, so the first point never has its own shape consulted.
    const auto shaped = shapedSegmentProgress(next.shape, u, next.bezierX1,
                                              next.bezierY1, next.bezierX2,
                                              next.bezierY2);
    return left.value + (next.value - left.value) * shaped;
}

float evaluatePitchCurve(const std::vector<PitchCurveEditPoint>& points,
                         double timeSeconds)
{
    if (points.empty()) return 60.0f;
    if (points.size() == 1) return points.front().targetMidi;
    const auto right = std::upper_bound(points.begin(), points.end(), timeSeconds,
        [](double value, const PitchCurveEditPoint& point)
        {
            return value < point.timeSeconds;
        });
    if (right == points.begin()) return points.front().targetMidi;
    if (right == points.end()) return points.back().targetMidi;
    const auto rightIndex = static_cast<std::size_t>(right - points.begin());
    const auto leftIndex = rightIndex - 1;
    const auto& left = points[leftIndex];
    const auto& next = points[rightIndex];
    const auto span = next.timeSeconds - left.timeSeconds;
    if (span <= 1.0e-9) return next.targetMidi;
    const auto u = static_cast<float>(juce::jlimit(0.0, 1.0,
        (timeSeconds - left.timeSeconds) / span));

    auto shaped = u;
    switch (next.shape)
    {
        case PitchCurveShape::linear:
        case PitchCurveShape::smooth:
        case PitchCurveShape::easeIn:
        case PitchCurveShape::easeOut:
        case PitchCurveShape::customBezier:
            shaped = shapedSegmentProgress(next.shape, u, next.bezierX1,
                                           next.bezierY1, next.bezierX2,
                                           next.bezierY2);
            break;
        case PitchCurveShape::natural:
        {
            // A monotone cubic Hermite curve.  Interior tangents use the
            // Fritsch-Carlson weighted harmonic mean; note endpoints settle
            // horizontally.  This keeps adjacent segments C1-smooth without
            // introducing accidental overshoot.  With only two points it is
            // exactly a smooth S transition.
            std::vector<double> intervals(points.size() - 1);
            std::vector<float> secants(points.size() - 1);
            for (std::size_t index = 0; index + 1 < points.size(); ++index)
            {
                intervals[index] = std::max(1.0e-9,
                    points[index + 1].timeSeconds - points[index].timeSeconds);
                secants[index] = (points[index + 1].targetMidi
                    - points[index].targetMidi) / static_cast<float>(intervals[index]);
            }
            std::vector<float> tangents(points.size(), 0.0f);
            for (std::size_t index = 1; index + 1 < points.size(); ++index)
            {
                const auto before = secants[index - 1];
                const auto after = secants[index];
                if (before == 0.0f || after == 0.0f
                    || std::signbit(before) != std::signbit(after))
                    continue;
                const auto firstWeight = 2.0 * intervals[index] + intervals[index - 1];
                const auto secondWeight = intervals[index] + 2.0 * intervals[index - 1];
                tangents[index] = static_cast<float>((firstWeight + secondWeight)
                    / (firstWeight / before + secondWeight / after));
            }
            const auto h00 = 2.0f * u * u * u - 3.0f * u * u + 1.0f;
            const auto h10 = u * u * u - 2.0f * u * u + u;
            const auto h01 = -2.0f * u * u * u + 3.0f * u * u;
            const auto h11 = u * u * u - u * u;
            return h00 * left.targetMidi
                + h10 * static_cast<float>(span) * tangents[leftIndex]
                + h01 * next.targetMidi
                + h11 * static_cast<float>(span) * tangents[rightIndex];
        }
    }
    return left.targetMidi + (next.targetMidi - left.targetMidi) * shaped;
}

juce::String pitchCurveShapeName(PitchCurveShape value)
{
    switch (value)
    {
        case PitchCurveShape::natural: return "natural";
        case PitchCurveShape::linear: return "linear";
        case PitchCurveShape::smooth: return "smooth";
        case PitchCurveShape::easeIn: return "ease-in";
        case PitchCurveShape::easeOut: return "ease-out";
        case PitchCurveShape::customBezier: return "custom-bezier";
    }
    return "natural";
}

PitchCurveShape parsePitchCurveShape(const juce::String& value)
{
    if (value == "linear") return PitchCurveShape::linear;
    if (value == "smooth") return PitchCurveShape::smooth;
    if (value == "ease-in") return PitchCurveShape::easeIn;
    if (value == "ease-out") return PitchCurveShape::easeOut;
    if (value == "custom-bezier") return PitchCurveShape::customBezier;
    return PitchCurveShape::natural;
}

// The two above are named in the project file and over MCP, so they belong to
// the interface; everything below is this file's own business.
namespace
{
juce::String pitchAlgorithmName(PitchAlgorithm value)
{
    switch (value)
    {
        case PitchAlgorithm::mld5: return "mld5";
        case PitchAlgorithm::mld3: return "mld3";
        case PitchAlgorithm::nsfHifigan: return "nsf-hifigan";
        case PitchAlgorithm::world: return "world";
        case PitchAlgorithm::vocalShifter: return "vslib";
        case PitchAlgorithm::llsm2: return "llsm2";
        case PitchAlgorithm::utau: return "utau";
    }
    return "mld5";
}

PitchAlgorithm parsePitchAlgorithm(const juce::String& value)
{
    if (value == "nsf-hifigan") return PitchAlgorithm::nsfHifigan;
    if (value == "mld3") return PitchAlgorithm::mld3;
    if (value == "world") return PitchAlgorithm::world;
    if (value == "vslib") return PitchAlgorithm::vocalShifter;
    if (value == "llsm2") return PitchAlgorithm::llsm2;
    if (value == "utau") return PitchAlgorithm::utau;
    return PitchAlgorithm::mld5;
}

juce::String stretchAlgorithmName(StretchAlgorithm value)
{
    switch (value)
    {
        case StretchAlgorithm::melodyneHybrid: return "melodyne-hybrid";
        case StretchAlgorithm::variableMelHop: return "variable-mel-hop";
        case StretchAlgorithm::loop: return "loop";
        case StretchAlgorithm::soundTouch: return "soundtouch";
        case StretchAlgorithm::nsfShiftThenSplice: return "nsf-shift-then-splice";
    }
    return "melodyne-hybrid";
}

StretchAlgorithm parseStretchAlgorithm(const juce::String& value)
{
    if (value == "variable-mel-hop") return StretchAlgorithm::variableMelHop;
    if (value == "loop") return StretchAlgorithm::loop;
    if (value == "soundtouch") return StretchAlgorithm::soundTouch;
    if (value == "nsf-shift-then-splice") return StretchAlgorithm::nsfShiftThenSplice;
    return StretchAlgorithm::melodyneHybrid;
}

juce::String renderOrderName(RenderOrder value)
{
    switch (value)
    {
        case RenderOrder::processThenSplice: return "process-then-splice";
        case RenderOrder::stretchSpliceThenPitch: return "stretch-splice-then-pitch";
    }
    return "process-then-splice";
}

RenderOrder parseRenderOrder(const juce::String& value)
{
    if (value == "stretch-splice-then-pitch") return RenderOrder::stretchSpliceThenPitch;
    return RenderOrder::processThenSplice;
}
}

double ProjectData::durationSeconds() const
{
    double duration = 8.0;
    for (const auto& track : tracks)
        for (const auto& clip : track.clips)
            duration = std::max(duration, clip.startSeconds + clip.durationSeconds);
    return duration;
}

double ProjectData::secondsForQuarterPosition(double quarterPosition) const
{
    const auto baseTempo = juce::jlimit(20.0, 400.0, bpm);
    if (quarterPosition <= 0.0)
        return beatOriginSeconds + quarterPosition * 60.0 / baseTempo;

    auto seconds = beatOriginSeconds;
    auto previousQuarter = 0.0;
    auto tempo = baseTempo;
    for (const auto& change : tempoChanges)
    {
        const auto changeQuarter = std::max(0.0, change.quarterPosition);
        if (changeQuarter <= previousQuarter + 1.0e-9)
        {
            tempo = juce::jlimit(20.0, 400.0, change.bpm);
            continue;
        }
        if (quarterPosition <= changeQuarter)
            return seconds + (quarterPosition - previousQuarter) * 60.0 / tempo;
        seconds += (changeQuarter - previousQuarter) * 60.0 / tempo;
        previousQuarter = changeQuarter;
        tempo = juce::jlimit(20.0, 400.0, change.bpm);
    }
    return seconds + (quarterPosition - previousQuarter) * 60.0 / tempo;
}

double ProjectData::quarterPositionForSeconds(double targetSeconds) const
{
    const auto baseTempo = juce::jlimit(20.0, 400.0, bpm);
    if (targetSeconds <= beatOriginSeconds)
        return (targetSeconds - beatOriginSeconds) * baseTempo / 60.0;

    auto seconds = beatOriginSeconds;
    auto previousQuarter = 0.0;
    auto tempo = baseTempo;
    for (const auto& change : tempoChanges)
    {
        const auto changeQuarter = std::max(0.0, change.quarterPosition);
        if (changeQuarter <= previousQuarter + 1.0e-9)
        {
            tempo = juce::jlimit(20.0, 400.0, change.bpm);
            continue;
        }
        const auto changeSeconds = seconds
            + (changeQuarter - previousQuarter) * 60.0 / tempo;
        if (targetSeconds <= changeSeconds)
            return previousQuarter + (targetSeconds - seconds) * tempo / 60.0;
        seconds = changeSeconds;
        previousQuarter = changeQuarter;
        tempo = juce::jlimit(20.0, 400.0, change.bpm);
    }
    return previousQuarter + (targetSeconds - seconds) * tempo / 60.0;
}

double ProjectData::tempoAtQuarterPosition(double quarterPosition) const
{
    auto tempo = juce::jlimit(20.0, 400.0, bpm);
    for (const auto& change : tempoChanges)
    {
        if (change.quarterPosition > quarterPosition + 1.0e-9) break;
        tempo = juce::jlimit(20.0, 400.0, change.bpm);
    }
    return tempo;
}

double ProjectData::tempoAtSeconds(double seconds) const
{
    return tempoAtQuarterPosition(quarterPositionForSeconds(seconds));
}

ProjectModel::ProjectModel()
{
    project.name = "Untitled";
}

ProjectData ProjectModel::snapshot() const
{
    const juce::ScopedLock guard(lock);
    return project;
}

std::uint64_t ProjectModel::revisionNumber() const
{
    const juce::ScopedLock guard(lock);
    return revision;
}

void ProjectModel::pushUndoLocked()
{
    undoHistory.push_back(project);
    if (undoHistory.size() > maxHistory)
        undoHistory.erase(undoHistory.begin());
    redoHistory.clear();
    ++revision;
}

void ProjectModel::replace(ProjectData replacement)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        project = std::move(replacement);
    }
    sendChangeMessage();
}

void ProjectModel::clear()
{
    replace(ProjectData{});
}

bool ProjectModel::undo()
{
    {
        const juce::ScopedLock guard(lock);
        if (undoHistory.empty()) return false;
        redoHistory.push_back(project);
        if (redoHistory.size() > maxHistory) redoHistory.erase(redoHistory.begin());
        project = std::move(undoHistory.back());
        undoHistory.pop_back();
        ++revision;
    }
    sendChangeMessage();
    return true;
}

bool ProjectModel::redo()
{
    {
        const juce::ScopedLock guard(lock);
        if (redoHistory.empty()) return false;
        undoHistory.push_back(project);
        if (undoHistory.size() > maxHistory) undoHistory.erase(undoHistory.begin());
        project = std::move(redoHistory.back());
        redoHistory.pop_back();
        ++revision;
    }
    sendChangeMessage();
    return true;
}

bool ProjectModel::canUndo() const
{
    const juce::ScopedLock guard(lock);
    return !undoHistory.empty();
}

bool ProjectModel::canRedo() const
{
    const juce::ScopedLock guard(lock);
    return !redoHistory.empty();
}

juce::String ProjectModel::makeId(const char* prefix)
{
    return juce::String(prefix) + "_" + juce::Uuid().toString().removeCharacters("-");
}

juce::String ProjectModel::addAudioFile(const juce::File& file, double durationSeconds,
                                        double startSeconds,
                                        const juce::String& targetTrackId)
{
    ClipData clip;
    clip.id = makeId("clip");
    clip.sourceFile = file;
    clip.startSeconds = std::max(0.0, startSeconds);
    clip.durationSeconds = std::max(0.01, durationSeconds);
    clip.sourceDurationSeconds = clip.durationSeconds;

    // Voicebank registration writes timing beside the source before the file
    // is dragged into a project.  Consume that sidecar immediately so an OTO
    // sample arrives as editable note objects instead of a silent/empty piano
    // roll that requires drawing every region again.
    const auto sidecar = SampleSettings::sidecarFor(file);
    const juce::File legacy(file.getFullPathName() + ".hachi.csv");
    if (sidecar.existsAsFile() || legacy.existsAsFile())
    {
        const auto rows = SampleSettings::loadOrDerive(file, ProjectData{});
        std::vector<RegionSpan> spans;
        spans.reserve(rows.size());
        for (const auto& row : rows)
        {
            const auto regionStart = juce::jlimit(0.0, clip.durationSeconds,
                                                   row.regionStartSeconds);
            const auto regionEnd = juce::jlimit(regionStart, clip.durationSeconds,
                                                 row.regionEndSeconds);
            spans.push_back({ regionStart,
                              regionEnd - regionStart < 0.001 ? regionStart : regionEnd });
        }
        for (const auto index : regionsToImport(spans))
        {
            const auto& row = rows[index];
            const auto regionStart = spans[index].startSeconds;
            const auto regionEnd = spans[index].endSeconds;
            if (regionEnd - regionStart < 0.001) continue;
            NoteData note;
            note.id = makeId("note");
            note.startSeconds = regionStart;
            note.durationSeconds = regionEnd - regionStart;
            note.consonantSeconds = juce::jlimit(0.0, note.durationSeconds,
                                                  row.fixedDurationSeconds);
            const auto storedSource = row.melodyneOriginalPitchCenterCents > 0.0
                ? static_cast<float>(row.melodyneOriginalPitchCenterCents / 100.0)
                : row.melodynePitchCenterCents > 0.0
                    ? static_cast<float>(row.melodynePitchCenterCents / 100.0)
                    : 60.0f;
            note.sourceMidiCenter = juce::jlimit(0.0f, 127.0f, storedSource);
            note.midiNote = row.melodynePitchCenterCents > 0.0
                ? juce::jlimit(0.0f, 127.0f,
                    static_cast<float>(row.melodynePitchCenterCents / 100.0))
                : juce::jlimit(0.0f, 127.0f, note.sourceMidiCenter
                    + static_cast<float>(row.relativePitchCents / 100.0));
            note.drift = juce::jlimit(0.0f, 2.0f,
                static_cast<float>(row.melodynePitchDrift));
            note.modulation = juce::jlimit(0.0f, 2.0f,
                static_cast<float>(row.melodynePitchModulation));
            note.formantSemitones = juce::jlimit(-12.0f, 12.0f,
                static_cast<float>(row.melodyneFormantCents / 100.0));
            note.breath = juce::jlimit(0.0f, 1.0f,
                static_cast<float>(row.melodyneSibilantBalance));
            note.gain = juce::jlimit(0.0f, 4.0f,
                static_cast<float>(row.melodyneAmplitude));
            note.attackSpeed = juce::jlimit(0.05f, 20.0f,
                static_cast<float>(row.melodyneAttackSeconds > 1.0e-6
                    ? row.fixedDurationSeconds / row.melodyneAttackSeconds : 1.0));
            // A sidecar stores note-level controls rather than a dense F0
            // curve.  A neutral two-point contour keeps the source waveform's
            // own micro-pitch intact while allowing the whole region to move.
            note.contour.push_back({ 0.0, 0.0f, 0.0f, true });
            note.contour.push_back({ note.durationSeconds, 0.0f, 0.0f, true });
            clip.notes.push_back(std::move(note));
        }
    }
    const auto clipId = clip.id;

    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        const auto target = std::find_if(project.tracks.begin(), project.tracks.end(),
            [&targetTrackId](const auto& track)
            {
                return targetTrackId.isNotEmpty() && track.id == targetTrackId;
            });
        if (target != project.tracks.end())
            target->clips.push_back(std::move(clip));
        else
        {
            TrackData track;
            track.id = makeId("track");
            track.name = file.getFileNameWithoutExtension();
            track.clips.push_back(std::move(clip));
            project.tracks.push_back(std::move(track));
        }
        if (project.name == "Untitled")
            project.name = file.getFileNameWithoutExtension();
    }
    sendChangeMessage();
    return clipId;
}

juce::String ProjectModel::addTrack(const juce::String& requestedName, bool compose,
                                   bool referenceOnly)
{
    juce::String id;
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        TrackData track;
        track.id = makeId("track");
        track.name = requestedName.trim().substring(0, 80);
        if (track.name.isEmpty())
            track.name = compose ? "Melodic Track" : "Audio Track";
        track.compose = compose;
        track.referenceOnly = referenceOnly;
        // A new track follows the currently established project workflow,
        // rather than unexpectedly returning to mld5 after the user has
        // selected NSF/WORLD or a different stretch engine.
        if (!project.tracks.empty())
        {
            track.pitchAlgorithm = project.tracks.back().pitchAlgorithm;
            track.stretchAlgorithm = project.tracks.back().stretchAlgorithm;
            track.renderOrder = project.tracks.back().renderOrder;
        }
        id = track.id;
        project.tracks.push_back(std::move(track));
    }
    sendChangeMessage();
    return id;
}

void ProjectModel::setTrackReferenceOnly(const juce::String& trackId,
                                         bool referenceOnly)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.referenceOnly != referenceOnly)
            {
                pushUndoLocked();
                track.referenceOnly = referenceOnly;
                changed = true;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackName(const juce::String& trackId,
                                const juce::String& requestedName)
{
    const auto name = requestedName.trim().substring(0, 80);
    if (name.isEmpty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.name != name)
            {
                pushUndoLocked();
                track.name = name;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

bool ProjectModel::setClipNotesIfEmpty(const juce::String& clipId,
                                       std::vector<NoteData> notes)
{
    if (notes.empty()) return false;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId && clip.notes.empty())
                {
                    // Import analysis is one operation.  More importantly,
                    // do not replace notes the user drew while it was running.
                    clip.notes = std::move(notes);
                    changed = true;
                    break;
                }
    }
    if (changed) sendChangeMessage();
    return changed;
}

bool ProjectModel::addMidiFile(const juce::File& file, juce::String& error)
{
    auto input = file.createInputStream();
    if (input == nullptr)
    {
        error = "Could not open MIDI file: " + file.getFullPathName();
        return false;
    }
    juce::MidiFile midi;
    if (!midi.readFrom(*input))
    {
        error = "Invalid MIDI file: " + file.getFullPathName();
        return false;
    }
    std::optional<double> importedBpm;
    for (int trackIndex = 0; trackIndex < midi.getNumTracks() && !importedBpm; ++trackIndex)
        if (const auto* sequence = midi.getTrack(trackIndex))
            for (int eventIndex = 0; eventIndex < sequence->getNumEvents(); ++eventIndex)
                if (const auto* event = sequence->getEventPointer(eventIndex);
                    event != nullptr && event->message.isTempoMetaEvent())
                {
                    const auto secondsPerQuarter = event->message.getTempoSecondsPerQuarterNote();
                    if (secondsPerQuarter > 1.0e-9)
                        importedBpm = 60.0 / secondsPerQuarter;
                    break;
                }
    midi.convertTimestampTicksToSeconds();
    std::vector<TrackData> importedTracks;
    for (int trackIndex = 0; trackIndex < midi.getNumTracks(); ++trackIndex)
    {
        const auto* sequence = midi.getTrack(trackIndex);
        if (sequence == nullptr) continue;
        juce::MidiMessageSequence matched(*sequence);
        matched.updateMatchedPairs();
        TrackData track;
        track.id = makeId("track");
        track.name = file.getFileNameWithoutExtension()
            + (midi.getNumTracks() > 1 ? " " + juce::String(trackIndex + 1) : juce::String());
        track.compose = true;
        ClipData clip;
        clip.id = makeId("clip");
        clip.sourceFile = file;
        clip.startSeconds = 0.0;
        for (int eventIndex = 0; eventIndex < matched.getNumEvents(); ++eventIndex)
        {
            const auto* event = matched.getEventPointer(eventIndex);
            if (event == nullptr || !event->message.isNoteOn()) continue;
            const auto offIndex = matched.getIndexOfMatchingKeyUp(eventIndex);
            const auto start = std::max(0.0, event->message.getTimeStamp());
            const auto end = offIndex >= 0
                ? std::max(start + 0.01, matched.getEventTime(offIndex)) : start + 0.25;
            NoteData note;
            note.id = makeId("note");
            note.startSeconds = start;
            note.durationSeconds = end - start;
            note.consonantSeconds = 0.0;
            note.midiNote = static_cast<float>(event->message.getNoteNumber());
            note.sourceMidiCenter = note.midiNote;
            note.contour.push_back({ 0.0, 0.0f, 0.0f, true });
            note.contour.push_back({ note.durationSeconds, 0.0f, 0.0f, true });
            clip.durationSeconds = std::max(clip.durationSeconds, end);
            clip.notes.push_back(std::move(note));
        }
        if (clip.notes.empty()) continue;
        clip.sourceDurationSeconds = clip.durationSeconds;
        track.clips.push_back(std::move(clip));
        importedTracks.push_back(std::move(track));
    }
    if (importedTracks.empty())
    {
        error = "MIDI file contains no notes: " + file.getFullPathName();
        return false;
    }
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        if (project.tracks.empty() && importedBpm)
            project.bpm = juce::jlimit(20.0, 400.0, *importedBpm);
        for (auto& track : importedTracks) project.tracks.push_back(std::move(track));
        if (project.name == "Untitled") project.name = file.getFileNameWithoutExtension();
    }
    sendChangeMessage();
    return true;
}

namespace
{
// A UST mode-2 pitch bend as this editor's pitch handles.
//
// Every pitch in a UST bend is in tenths of a semitone and is measured from
// the note's own NoteNum, so a note at 63 whose bend starts at -20 starts two
// semitones down, at 61 -- which is how a portamento out of the preceding
// note is written.  Times are milliseconds: PBS gives the first point's
// offset from the note's start (normally negative, so the bend begins inside
// the note before), and PBW gives the gap to each point after it.
//
// PBY is one short of PBW on purpose: the last point is the note's own pitch.
//
// PBM names the shape of each segment, and is indexed by the segment rather
// than the point -- entry i shapes the run from point i to point i+1, which is
// this editor's "shape of the incoming segment" on point i+1.
// A UST amplitude envelope as this editor's envelope handles.
//
// UTAU measures the shape from the beginning of the rendered output, which
// starts one preutterance before the note; this editor measures from the note
// itself, so time zero here is one preutterance later and the opening ramp
// sits at negative times.  That is the same convention envelopeForNoteAsItIs
// re-anchors at render time, and it is what keeps p1 lined up with the
// crossfade into the previous note.
//
// Volumes are percentages of the note's own level.  Zero is silence, and the
// renderer treats anything at or below -60 dB as silent, so that is the floor
// rather than an infinity.
void applyUstEnvelope(NoteData& note, const backend::UstNote& source,
                      double preutteranceSeconds)
{
    if (!source.hasEnvelope) return;
    const auto gainDb = [](double percent)
    {
        if (percent <= 0.01) return -60.0f;
        return juce::jlimit(-60.0f, 12.0f,
                            static_cast<float>(20.0 * std::log10(percent / 100.0)));
    };
    const auto fromStart = [preutteranceSeconds](double milliseconds)
    {
        return -preutteranceSeconds + milliseconds / 1000.0;
    };
    const auto fromEnd = [&note](double milliseconds)
    {
        return note.durationSeconds - milliseconds / 1000.0;
    };

    std::vector<AmplitudeEnvelopePoint> points;
    points.push_back({ fromStart(0.0), -60.0f });
    points.push_back({ fromStart(source.envelopeP1), gainDb(source.envelopeV1) });
    points.push_back({ fromStart(source.envelopeP1 + source.envelopeP2),
                       gainDb(source.envelopeV2) });
    if (source.hasMiddlePoint)
        points.push_back({ fromStart(source.envelopeP1 + source.envelopeP2
                                     + source.envelopeP5),
                           gainDb(source.envelopeV5) });
    points.push_back({ fromEnd(source.envelopeP3 + source.envelopeP4),
                       gainDb(source.envelopeV3) });
    points.push_back({ fromEnd(source.envelopeP3), gainDb(source.envelopeV4) });
    points.push_back({ fromEnd(0.0), -60.0f });

    // A short note can leave the opening ramp and the closing one overlapping,
    // and an envelope whose times run backwards is not one this editor can
    // draw or the renderer can read.  Push each point up to the one before it
    // rather than dropping any: the shape stays, squeezed.
    for (std::size_t index = 1; index < points.size(); ++index)
        points[index].timeSeconds = std::max(points[index].timeSeconds,
                                             points[index - 1].timeSeconds);
    note.amplitudeEnvelope = std::move(points);
}

void applyUstPitchBend(NoteData& note, const backend::UstNote& source)
{
    if (!source.hasPitchBend) return;
    const auto pitchAt = [&note](double tenths)
    {
        return note.midiNote + static_cast<float>(tenths) * 0.1f;
    };
    const auto shapeFor = [&source](int segment)
    {
        const auto name = segment < source.shapes.size()
            ? source.shapes[segment] : juce::String();
        if (name == "s") return PitchCurveShape::linear;
        // UTAU's R rises fast and flattens; its J waits and then rises.
        if (name == "r") return PitchCurveShape::easeOut;
        if (name == "j") return PitchCurveShape::easeIn;
        // An empty entry is UTAU's default S-curve.
        return PitchCurveShape::smooth;
    };

    std::vector<PitchCurveEditPoint> points;
    points.reserve(source.widthsMs.size() + 1);
    auto time = source.pitchStartMs / 1000.0;
    points.push_back({ time, pitchAt(source.pitchStartTenths) });
    for (std::size_t index = 0; index < source.widthsMs.size(); ++index)
    {
        time += source.widthsMs[index] / 1000.0;
        const auto tenths = index < source.pitchTenths.size()
            ? source.pitchTenths[index] : 0.0;
        PitchCurveEditPoint point { time, pitchAt(tenths) };
        point.shape = shapeFor(static_cast<int>(index));
        points.push_back(point);
    }
    // A bend with one point says nothing the note's own pitch does not.
    if (points.size() < 2) return;
    note.pitchControlPoints = std::move(points);
}
}

bool ProjectModel::addUstFile(const juce::File& file, juce::String& error,
                              juce::StringArray& warnings)
{
    const auto parsed = backend::UstImporter::read(file, error, warnings);
    if (!parsed) return false;

    TrackData track;
    track.id = makeId("track");
    track.name = parsed->name.isNotEmpty() ? parsed->name
                                           : file.getFileNameWithoutExtension();
    track.compose = true;
    // Plain UTAU: the four-region modes are this application's own, and a UST
    // says nothing that would fill them in.
    track.pitchAlgorithm = PitchAlgorithm::utau;
    track.utauMode = UtauMode::classic;
    track.utauGlobalFlags = parsed->globalFlags;

    ClipData clip;
    clip.id = makeId("clip");
    clip.sourceFile = file;
    clip.startSeconds = 0.0;

    // A UST places its notes by accumulating lengths in ticks, so the timeline
    // is built in musical time and turned into seconds through the project's
    // own tempo map afterwards.  Summing seconds as we went would drift the
    // moment a note carried a tempo change.
    std::vector<TempoChange> tempoChanges;
    std::vector<const backend::UstNote*> sources;
    auto quarters = 0.0;
    auto tempo = parsed->tempo;
    for (const auto& source : parsed->notes)
    {
        if (source.tempo && *source.tempo > 0.0 && *source.tempo != tempo)
        {
            tempo = *source.tempo;
            tempoChanges.push_back({ quarters, juce::jlimit(20.0, 400.0, tempo) });
        }
        const auto length = backend::UstImporter::quarterNotes(source.lengthTicks);
        // A rest becomes a gap rather than a silent note, which is the shape
        // the rest of this editor already works in.
        if (source.isRest() || length <= 0.0)
        {
            quarters += length;
            continue;
        }
        NoteData note;
        note.id = makeId("note");
        note.label = source.lyric.trim();
        note.midiNote = static_cast<float>(source.noteNum);
        note.sourceMidiCenter = note.midiNote;
        note.utauFlags = source.flags;
        if (source.velocity) note.utauConsonantVelocity = *source.velocity;
        // UST intensity is a percentage; this editor keeps gain as a
        // multiplier and the renderer turns it back into a percentage.
        if (source.intensity)
            note.gain = juce::jlimit(0.0f, 2.0f,
                                     static_cast<float>(*source.intensity) / 100.0f);
        // An absent PreUtterance means "whatever the oto says", which is not
        // the same as an override of zero, so only a written value overrides.
        if (source.preutteranceMs)
        {
            note.utauPreutteranceOverrideEnabled = true;
            note.utauPreutteranceSeconds = *source.preutteranceMs / 1000.0;
        }
        if (source.overlapMs)
        {
            note.utauOverlapOverrideEnabled = true;
            note.utauOverlapSeconds = *source.overlapMs / 1000.0;
        }
        // Musical position for now; converted below, once the tempo map is in.
        note.startSeconds = quarters;
        note.durationSeconds = length;
        quarters += length;
        clip.notes.push_back(std::move(note));
        sources.push_back(&source);
    }

    if (clip.notes.empty())
    {
        error = "UST contains only rests: " + file.getFullPathName();
        return false;
    }

    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        // The tempo map has to be in place before a musical position can be
        // turned into seconds, and an imported song owns the tempo only when
        // it is the first thing in the project.
        if (project.tracks.empty())
        {
            project.bpm = juce::jlimit(20.0, 400.0, parsed->tempo);
            project.tempoChanges = tempoChanges;
        }
        else if (!tempoChanges.empty())
            warnings.add("tempo changes ignored: the project already has tracks");

        for (std::size_t index = 0; index < clip.notes.size(); ++index)
        {
            auto& note = clip.notes[index];
            const auto startQuarters = note.startSeconds;
            const auto endQuarters = startQuarters + note.durationSeconds;
            note.startSeconds = project.secondsForQuarterPosition(startQuarters);
            note.durationSeconds = std::max(0.01,
                project.secondsForQuarterPosition(endQuarters) - note.startSeconds);
            note.consonantSeconds = 0.0;
            note.contour.push_back({ 0.0, 0.0f, 0.0f, true });
            note.contour.push_back({ note.durationSeconds, 0.0f, 0.0f, true });
            applyUstPitchBend(note, *sources[index]);
            // The envelope is drawn against the preutterance the UST named.
            // Where it named none the oto's own is not known here -- no
            // voicebank has been chosen yet -- so the shape is anchored at the
            // note and envelopeForNoteAsItIs moves it, ramps intact, once the
            // real preutterance is known at render time.
            applyUstEnvelope(note, *sources[index],
                             note.utauPreutteranceOverrideEnabled
                                 ? note.utauPreutteranceSeconds : 0.0);
        }
        clip.durationSeconds = clip.notes.back().startSeconds
            + clip.notes.back().durationSeconds;
        clip.sourceDurationSeconds = clip.durationSeconds;
        track.clips.push_back(std::move(clip));
        project.tracks.push_back(std::move(track));
        if (project.name == "Untitled" && parsed->name.isNotEmpty())
            project.name = parsed->name;
    }
    if (parsed->voiceDirectory.isNotEmpty())
        warnings.add("this UST asks for voicebank \"" + parsed->voiceDirectory
                     + "\" -- choose it under Settings / Algorithm");
    sendChangeMessage();
    return true;
}

void ProjectModel::setTempo(double bpm, int numerator, int denominator)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        project.bpm = juce::jlimit(20.0, 400.0, bpm);
        project.numerator = juce::jlimit(1, 32, numerator);
        project.denominator = denominator == 2 || denominator == 8 || denominator == 16
            ? denominator : 4;
    }
    sendChangeMessage();
}

void ProjectModel::setTempoChange(double quarterPosition, double bpm)
{
    const auto position = std::max(0.0, quarterPosition);
    const auto tempo = juce::jlimit(20.0, 400.0, bpm);
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        const auto before = project;
        auto existing = std::find_if(project.tempoChanges.begin(),
            project.tempoChanges.end(), [&](const auto& change)
            {
                return std::abs(change.quarterPosition - position) < 1.0e-7;
            });
        if (position <= 1.0e-7)
        {
            if (std::abs(project.bpm - tempo) < 1.0e-7) return;
        }
        else if (existing != project.tempoChanges.end()
                 && std::abs(existing->bpm - tempo) < 1.0e-7)
            return;

        pushUndoLocked();
        if (position <= 1.0e-7)
            project.bpm = tempo;
        else if (existing != project.tempoChanges.end())
            existing->bpm = tempo;
        else
            project.tempoChanges.push_back({ position, tempo });
        std::stable_sort(project.tempoChanges.begin(), project.tempoChanges.end(),
            [](const auto& left, const auto& right)
            {
                return left.quarterPosition < right.quarterPosition;
            });

        const auto remapTime = [&](double oldSeconds)
        {
            return project.secondsForQuarterPosition(
                before.quarterPositionForSeconds(oldSeconds));
        };
        for (std::size_t trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex)
        {
            auto& track = project.tracks[trackIndex];
            if (!track.compose || trackIndex >= before.tracks.size()) continue;
            const auto& oldTrack = before.tracks[trackIndex];
            for (std::size_t clipIndex = 0;
                 clipIndex < track.clips.size() && clipIndex < oldTrack.clips.size();
                 ++clipIndex)
            {
                auto& clip = track.clips[clipIndex];
                const auto& oldClip = oldTrack.clips[clipIndex];
                const auto oldClipStart = oldClip.startSeconds;
                const auto oldClipEnd = oldClipStart + oldClip.durationSeconds;
                const auto newClipStart = remapTime(oldClipStart);
                const auto newClipEnd = remapTime(oldClipEnd);
                clip.startSeconds = newClipStart;
                clip.durationSeconds = std::max(0.01, newClipEnd - newClipStart);

                const auto remapClipOffset = [&](double oldOffset)
                {
                    return remapTime(oldClipStart + oldOffset) - newClipStart;
                };
                for (auto& point : clip.sourceTimeMap)
                    point.targetSeconds = remapClipOffset(point.targetSeconds);
                clip.fadeInSeconds = juce::jlimit(0.0, clip.durationSeconds,
                    remapClipOffset(oldClip.fadeInSeconds));
                const auto newFadeOutStart = remapTime(oldClipEnd - oldClip.fadeOutSeconds);
                clip.fadeOutSeconds = juce::jlimit(0.0, clip.durationSeconds,
                                                   newClipEnd - newFadeOutStart);

                for (std::size_t noteIndex = 0;
                     noteIndex < clip.notes.size() && noteIndex < oldClip.notes.size();
                     ++noteIndex)
                {
                    auto& note = clip.notes[noteIndex];
                    const auto& oldNote = oldClip.notes[noteIndex];
                    const auto oldNoteStart = oldClipStart + oldNote.startSeconds;
                    const auto oldNoteEnd = oldNoteStart + oldNote.durationSeconds;
                    const auto newNoteStart = remapTime(oldNoteStart);
                    const auto newNoteEnd = remapTime(oldNoteEnd);
                    note.startSeconds = newNoteStart - newClipStart;
                    note.durationSeconds = std::max(0.001,
                                                    newNoteEnd - newNoteStart);
                    note.consonantSeconds = juce::jlimit(0.0, note.durationSeconds,
                        remapTime(oldNoteStart + oldNote.consonantSeconds)
                            - newNoteStart);
                    for (std::size_t index = 0;
                         index < note.contour.size() && index < oldNote.contour.size(); ++index)
                        note.contour[index].timeSeconds = remapTime(oldNoteStart
                            + oldNote.contour[index].timeSeconds) - newNoteStart;
                    for (std::size_t index = 0;
                         index < note.pitchControlPoints.size()
                            && index < oldNote.pitchControlPoints.size(); ++index)
                        note.pitchControlPoints[index].timeSeconds = remapTime(oldNoteStart
                            + oldNote.pitchControlPoints[index].timeSeconds) - newNoteStart;
                    for (std::size_t index = 0;
                         index < note.amplitudeEnvelope.size()
                            && index < oldNote.amplitudeEnvelope.size(); ++index)
                        note.amplitudeEnvelope[index].timeSeconds = remapTime(oldNoteStart
                            + oldNote.amplitudeEnvelope[index].timeSeconds) - newNoteStart;
                    for (std::size_t index = 0;
                         index < note.sibilantMarkers.size()
                            && index < oldNote.sibilantMarkers.size(); ++index)
                        note.sibilantMarkers[index] = remapTime(oldNoteStart
                            + oldNote.sibilantMarkers[index]) - newNoteStart;
                    clip.durationSeconds = std::max(clip.durationSeconds,
                        note.startSeconds + note.durationSeconds);
                }
            }
        }
        changed = true;
    }
    if (changed) sendChangeMessage();
}

juce::int64 ProjectModel::contentFingerprint() const
{
    juce::MemoryOutputStream stream;
    toValueTree(juce::File()).writeToStream(stream);
    // Hash the raw bytes.  A serialised ValueTree is binary and full of zero
    // bytes, so reading it back as a string would stop at the first one and
    // leave almost the whole project out of the digest.
    const auto* bytes = static_cast<const juce::uint8*>(stream.getData());
    auto hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < stream.getDataSize(); ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return static_cast<juce::int64>(hash);
}

void ProjectModel::setGridDivision(const juce::String& division)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        project.gridDivision = division;
    }
    sendChangeMessage();
}

void ProjectModel::setNoteEditDivision(int division)
{
    const auto value = juce::jlimit(2, 128, division);
    {
        const juce::ScopedLock guard(lock);
        if (project.noteEditDivision == value) return;
        pushUndoLocked();
        project.noteEditDivision = value;
    }
    sendChangeMessage();
}

void ProjectModel::setBaseScale(const juce::String& scale)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        project.baseScale = scale;
    }
    sendChangeMessage();
}

void ProjectModel::setTrackCompose(const juce::String& trackId, bool enabled)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        for (auto& track : project.tracks)
            if (track.id == trackId)
                track.compose = enabled;
    }
    sendChangeMessage();
}

void ProjectModel::setTrackMuted(const juce::String& trackId, bool muted)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        for (auto& track : project.tracks)
            if (track.id == trackId)
                track.muted = muted;
    }
    sendChangeMessage();
}

void ProjectModel::setTrackSolo(const juce::String& trackId, bool solo)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        for (auto& track : project.tracks)
            if (track.id == trackId)
                track.solo = solo;
    }
    sendChangeMessage();
}

void ProjectModel::setTrackVolume(const juce::String& trackId, float volume)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        for (auto& track : project.tracks)
            if (track.id == trackId)
                track.volume = juce::jlimit(0.0f, 2.0f, volume);
    }
    sendChangeMessage();
}

void ProjectModel::setTrackPan(const juce::String& trackId, float pan)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        for (auto& track : project.tracks)
            if (track.id == trackId)
                track.pan = juce::jlimit(-1.0f, 1.0f, pan);
    }
    sendChangeMessage();
}

void ProjectModel::setTrackSmoothOverlaps(const juce::String& trackId, bool enabled)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.smoothOverlaps != enabled)
            {
                pushUndoLocked();
                track.smoothOverlaps = enabled;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackNormalizeVolume(const juce::String& trackId, bool enabled)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.normalizeVolume != enabled)
            {
                pushUndoLocked();
                track.normalizeVolume = enabled;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackVoicebankDirectory(const juce::String& trackId,
                                               const juce::File& directory)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.voicebankDirectory != directory)
            {
                pushUndoLocked();
                track.voicebankDirectory = directory;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackUtauConsonantVelocity(const juce::String& trackId,
                                                  int velocity)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.utauConsonantVelocity != velocity)
            {
                pushUndoLocked();
                track.utauConsonantVelocity = velocity;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackUtauGlobalFlags(const juce::String& trackId,
                                            const juce::String& flags)
{
    const auto normalized = flags.trim();
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.utauGlobalFlags != normalized)
            {
                pushUndoLocked();
                track.utauGlobalFlags = normalized;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setPitchAlgorithm(PitchAlgorithm algorithm)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.pitchAlgorithm != algorithm)
            {
                if (!changed) pushUndoLocked();
                track.pitchAlgorithm = algorithm;
                changed = true;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setUtauMode(UtauMode mode)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.utauMode != mode)
            {
                if (!changed) pushUndoLocked();
                track.utauMode = mode;
                changed = true;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackUtauMode(const juce::String& trackId, UtauMode mode)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.utauMode != mode)
            {
                pushUndoLocked();
                track.utauMode = mode;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackPitchAlgorithm(const juce::String& trackId,
                                          PitchAlgorithm algorithm)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.pitchAlgorithm != algorithm)
            {
                pushUndoLocked();
                track.pitchAlgorithm = algorithm;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setStretchAlgorithm(StretchAlgorithm algorithm)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.stretchAlgorithm != algorithm)
            {
                if (!changed) pushUndoLocked();
                track.stretchAlgorithm = algorithm;
                changed = true;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackStretchAlgorithm(const juce::String& trackId,
                                            StretchAlgorithm algorithm)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.stretchAlgorithm != algorithm)
            {
                pushUndoLocked();
                track.stretchAlgorithm = algorithm;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setRenderOrder(RenderOrder order)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.renderOrder != order)
            {
                if (!changed) pushUndoLocked();
                track.renderOrder = order;
                changed = true;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setTrackRenderOrder(const juce::String& trackId, RenderOrder order)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.renderOrder != order)
            {
                pushUndoLocked();
                track.renderOrder = order;
                changed = true;
                break;
            }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::moveClip(const juce::String& clipId, double startSeconds)
{
    {
        const juce::ScopedLock guard(lock);
        pushUndoLocked();
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId)
                    clip.startSeconds = std::max(0.0, startSeconds);
    }
    sendChangeMessage();
}

juce::String ProjectModel::duplicateClip(const juce::String& clipId,
                                         double startSeconds,
                                         const juce::String& targetTrackId)
{
    juce::String insertedId;
    {
        const juce::ScopedLock guard(lock);
        auto sourceTrackIndex = project.tracks.size();
        ClipData copy;
        auto found = false;
        for (std::size_t trackIndex = 0; trackIndex < project.tracks.size() && !found;
             ++trackIndex)
            for (const auto& clip : project.tracks[trackIndex].clips)
                if (clip.id == clipId)
                {
                    sourceTrackIndex = trackIndex;
                    copy = clip;
                    found = true;
                    break;
                }
        if (!found || sourceTrackIndex >= project.tracks.size()) return {};

        auto destinationTrackIndex = sourceTrackIndex;
        if (targetTrackId.isNotEmpty())
            for (std::size_t trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex)
                if (project.tracks[trackIndex].id == targetTrackId)
                {
                    destinationTrackIndex = trackIndex;
                    break;
                }

        pushUndoLocked();
        copy.id = makeId("clip");
        copy.startSeconds = startSeconds >= 0.0
            ? startSeconds : copy.startSeconds + copy.durationSeconds;
        copy.startSeconds = std::max(0.0, copy.startSeconds);
        for (auto& note : copy.notes) note.id = makeId("note");

        // Connection flags at the outer edges describe neighbouring notes in
        // the original timeline.  Preserve joins inside the copied clip, but
        // do not accidentally glide into unrelated material at its new place.
        if (!copy.notes.empty())
        {
            const auto first = std::min_element(copy.notes.begin(), copy.notes.end(),
                [](const auto& left, const auto& right)
                {
                    return left.startSeconds < right.startSeconds;
                });
            const auto last = std::max_element(copy.notes.begin(), copy.notes.end(),
                [](const auto& left, const auto& right)
                {
                    return left.startSeconds + left.durationSeconds
                        < right.startSeconds + right.durationSeconds;
                });
            first->connectedToPrevious = false;
            last->connectedToNext = false;
        }
        insertedId = copy.id;
        project.tracks[destinationTrackIndex].clips.push_back(std::move(copy));
    }
    sendChangeMessage();
    return insertedId;
}

void ProjectModel::resizeClip(const juce::String& clipId, double startSeconds,
                              double durationSeconds)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId)
                {
                    const auto oldDuration = std::max(0.01, clip.durationSeconds);
                    const auto nextDuration = std::max(0.01, durationSeconds);
                    const auto nextStart = std::max(0.0, startSeconds);
                    if (std::abs(clip.startSeconds - nextStart) <= 1.0e-9
                        && std::abs(oldDuration - nextDuration) <= 1.0e-9) return;
                    pushUndoLocked();
                    const auto ratio = nextDuration / oldDuration;
                    for (auto& note : clip.notes)
                    {
                        note.startSeconds *= ratio;
                        note.durationSeconds *= ratio;
                        note.consonantSeconds *= ratio;
                        // attackSpeed is the source-time / element-time slope.
                        // Keep the source Attack boundary fixed while its target
                        // position stretches with the rest of the clip.
                        note.attackSpeed = juce::jlimit(0.05f, 20.0f,
                            note.attackSpeed / static_cast<float>(ratio));
                        for (auto& point : note.contour) point.timeSeconds *= ratio;
                        for (auto& point : note.pitchControlPoints) point.timeSeconds *= ratio;
                        for (auto& marker : note.sibilantMarkers) marker *= ratio;
                    }
                    // The selected source range is unchanged by a timeline
                    // stretch.  Move only the target side of the imported warp
                    // so the original Melodyne Attack/vowel source anchors are
                    // retained exactly.
                    for (auto& point : clip.sourceTimeMap)
                        point.targetSeconds *= ratio;
                    clip.fadeInSeconds = std::min(nextDuration, clip.fadeInSeconds * ratio);
                    clip.fadeOutSeconds = std::min(nextDuration, clip.fadeOutSeconds * ratio);
                    clip.crossfadeInSeconds = std::min(nextDuration, clip.crossfadeInSeconds * ratio);
                    clip.crossfadeOutSeconds = std::min(nextDuration, clip.crossfadeOutSeconds * ratio);
                    clip.startSeconds = nextStart;
                    clip.durationSeconds = nextDuration;
                    changed = true;
                    break;
                }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setClipGain(const juce::String& clipId, float gain)
{
    const auto next = juce::jlimit(0.0f, 4.0f, gain);
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId)
                {
                    if (std::abs(clip.gain - next) <= 1.0e-6f) return;
                    pushUndoLocked();
                    clip.gain = next;
                    changed = true;
                    break;
                }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setClipFades(const juce::String& clipId, double fadeInSeconds,
                                double fadeOutSeconds)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId)
                {
                    const auto nextIn = juce::jlimit(0.0, clip.durationSeconds,
                                                     fadeInSeconds);
                    const auto nextOut = juce::jlimit(0.0, clip.durationSeconds,
                                                      fadeOutSeconds);
                    if (std::abs(clip.fadeInSeconds - nextIn) <= 1.0e-9
                        && std::abs(clip.fadeOutSeconds - nextOut) <= 1.0e-9) return;
                    pushUndoLocked();
                    clip.fadeInSeconds = nextIn;
                    clip.fadeOutSeconds = nextOut;
                    changed = true;
                    break;
                }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setClipMuted(const juce::String& clipId, bool muted)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId)
                {
                    if (clip.muted == muted) return;
                    pushUndoLocked();
                    clip.muted = muted;
                    changed = true;
                    break;
                }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::removeClip(const juce::String& clipId)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            const auto found = std::find_if(track.clips.begin(), track.clips.end(),
                [&](const auto& clip) { return clip.id == clipId; });
            if (found == track.clips.end()) continue;
            pushUndoLocked();
            track.clips.erase(found);
            changed = true;
            break;
        }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::removeTrack(const juce::String& trackId)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        const auto found = std::find_if(project.tracks.begin(), project.tracks.end(),
            [&](const auto& track) { return track.id == trackId; });
        if (found != project.tracks.end())
        {
            pushUndoLocked();
            project.tracks.erase(found);
            changed = true;
        }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::transposeNote(const juce::String& noteId, float semitones)
{
    transposeNotes({ noteId }, semitones);
}

void ProjectModel::transposeNotes(const std::vector<juce::String>& noteIds, float semitones)
{
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    changed = changed || includes(note.id);
        if (!changed) return;
        pushUndoLocked();
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (includes(note.id))
                    {
                        const auto previous = note.midiNote;
                        note.midiNote = juce::jlimit(0.0f, 127.0f, note.midiNote + semitones);
                        const auto applied = note.midiNote - previous;
                        for (auto& point : note.pitchControlPoints)
                            point.targetMidi = juce::jlimit(0.0f, 127.0f,
                                point.targetMidi + applied);
                    }
    }
    if (changed) sendChangeMessage();
}

bool ProjectModel::moveUtauNotes(const std::vector<juce::String>& noteIds,
                                 double deltaSeconds, float semitones)
{
    if (noteIds.empty()) return false;
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };

    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        TrackData* ownerTrack = nullptr;
        ClipData* ownerClip = nullptr;
        std::size_t foundCount = 0;
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
            {
                const auto count = static_cast<std::size_t>(std::count_if(
                    clip.notes.begin(), clip.notes.end(),
                    [&](const auto& note) { return includes(note.id); }));
                if (count == 0) continue;
                if (ownerClip != nullptr) return false; // One drag cannot cross clips.
                ownerTrack = &track;
                ownerClip = &clip;
                foundCount = count;
            }
        if (ownerTrack == nullptr || ownerClip == nullptr
            || ownerTrack->pitchAlgorithm != PitchAlgorithm::utau
            || foundCount != noteIds.size())
            return false;

        auto phraseStart = std::numeric_limits<double>::max();
        auto phraseEnd = 0.0;
        auto minimumPitchDelta = -127.0f;
        auto maximumPitchDelta = 127.0f;
        for (const auto& note : ownerClip->notes)
            if (includes(note.id))
            {
                phraseStart = std::min(phraseStart, note.startSeconds);
                phraseEnd = std::max(phraseEnd,
                    note.startSeconds + note.durationSeconds);
                minimumPitchDelta = std::max(minimumPitchDelta, -note.midiNote);
                maximumPitchDelta = std::min(maximumPitchDelta, 127.0f - note.midiNote);
            }
        if (phraseStart == std::numeric_limits<double>::max()) return false;

        const auto phraseDuration = std::max(0.01, phraseEnd - phraseStart);
        auto destination = std::max(0.0, phraseStart + deltaSeconds);
        const auto appliedPitch = juce::jlimit(
            minimumPitchDelta, maximumPitchDelta, semitones);

        // Treat the selected notes as one phrase.  UTAU is monophonic, so a
        // time collision is meaningful regardless of the displayed pitch row.
        auto collisionStart = std::numeric_limits<double>::max();
        const auto destinationEnd = destination + phraseDuration;
        for (const auto& note : ownerClip->notes)
            if (!includes(note.id)
                && note.startSeconds < destinationEnd - 1.0e-9
                && note.startSeconds + note.durationSeconds > destination + 1.0e-9)
                collisionStart = std::min(collisionStart, note.startSeconds);

        const auto insertsAtCollision =
            collisionStart != std::numeric_limits<double>::max();
        if (insertsAtCollision)
        {
            // A move is a cut followed by an insert, not a copy followed by
            // an insert.  When the target lies to the right, removing the
            // phrase first moves that target left by exactly phraseDuration.
            destination = collisionStart >= phraseEnd - 1.0e-9
                ? collisionStart - phraseDuration : collisionStart;
        }
        const auto appliedTime = destination - phraseStart;
        changed = std::abs(appliedTime) > 1.0e-9
            || std::abs(appliedPitch) > 1.0e-6f || insertsAtCollision;
        if (!changed) return false;

        pushUndoLocked();
        if (insertsAtCollision)
        {
            // Close the source slot first, then open an equal-sized slot at
            // the destination.  Notes after both locations therefore keep
            // their original absolute positions and no extra gap accumulates.
            for (auto& note : ownerClip->notes)
                if (!includes(note.id)
                    && note.startSeconds >= phraseEnd - 1.0e-9)
                    note.startSeconds -= phraseDuration;
            for (auto& note : ownerClip->notes)
                if (!includes(note.id)
                    && note.startSeconds >= destination - 1.0e-9)
                    note.startSeconds += phraseDuration;
        }

        for (auto& note : ownerClip->notes)
            if (includes(note.id))
            {
                note.startSeconds += appliedTime;
                note.midiNote = juce::jlimit(0.0f, 127.0f,
                                             note.midiNote + appliedPitch);
                for (auto& point : note.pitchControlPoints)
                    point.targetMidi = juce::jlimit(0.0f, 127.0f,
                        point.targetMidi + appliedPitch);
            }

        std::stable_sort(ownerClip->notes.begin(), ownerClip->notes.end(),
            [](const auto& left, const auto& right)
            {
                return left.startSeconds < right.startSeconds;
            });
        for (const auto& note : ownerClip->notes)
            ownerClip->durationSeconds = std::max(ownerClip->durationSeconds,
                note.startSeconds + note.durationSeconds);
    }
    if (changed) sendChangeMessage();
    return changed;
}

void ProjectModel::setNotesMidi(const std::vector<juce::String>& noteIds, float midiNote)
{
    const auto target = juce::jlimit(0.0f, 127.0f, midiNote);
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    changed = changed || (includes(note.id)
                        && std::abs(note.midiNote - target) > 1.0e-6f);
        if (!changed) return;
        pushUndoLocked();
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (includes(note.id))
                    {
                        const auto applied = target - note.midiNote;
                        note.midiNote = target;
                        for (auto& point : note.pitchControlPoints)
                            point.targetMidi = juce::jlimit(0.0f, 127.0f,
                                point.targetMidi + applied);
                    }
    }
    sendChangeMessage();
}

void ProjectModel::averageNotesMidi(const std::vector<juce::String>& noteIds)
{
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };
    auto sum = 0.0;
    auto count = 0;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (includes(note.id))
                    {
                        sum += note.midiNote;
                        ++count;
                    }
    }
    if (count > 0) setNotesMidi(noteIds, static_cast<float>(sum / count));
}

void ProjectModel::quantizeNotesMidi(const std::vector<juce::String>& noteIds,
                                     float stepSemitones)
{
    const auto step = juce::jlimit(0.01f, 12.0f, stepSemitones);
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (includes(note.id))
                    {
                        const auto target = juce::jlimit(0.0f, 127.0f,
                            std::round(note.midiNote / step) * step);
                        changed = changed || std::abs(note.midiNote - target) > 1.0e-6f;
                    }
        if (!changed) return;
        pushUndoLocked();
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (includes(note.id))
                    {
                        const auto target = juce::jlimit(0.0f, 127.0f,
                            std::round(note.midiNote / step) * step);
                        const auto applied = target - note.midiNote;
                        note.midiNote = target;
                        for (auto& point : note.pitchControlPoints)
                            point.targetMidi = juce::jlimit(0.0f, 127.0f,
                                point.targetMidi + applied);
                    }
    }
    sendChangeMessage();
}

void ProjectModel::removeNotesRippling(const std::vector<juce::String>& noteIds)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
            {
                // The stretch the chosen notes take up, from the first start
                // to the last end.  What follows closes up against what came
                // before, whether either of those is a note or a silence.
                auto from = std::numeric_limits<double>::max();
                auto to = -std::numeric_limits<double>::max();
                for (const auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end())
                    {
                        from = std::min(from, note.startSeconds);
                        to = std::max(to, note.startSeconds + note.durationSeconds);
                    }
                if (to <= from) continue;
                const auto span = to - from;
                if (!changed) pushUndoLocked();
                changed = true;
                std::erase_if(clip.notes, [&noteIds](const auto& note)
                {
                    return std::find(noteIds.begin(), noteIds.end(), note.id)
                        != noteIds.end();
                });
                for (auto& note : clip.notes)
                    if (note.startSeconds >= to - 1.0e-9)
                        note.startSeconds = std::max(0.0, note.startSeconds - span);
                auto needed = 0.0;
                for (const auto& note : clip.notes)
                    needed = std::max(needed, note.startSeconds + note.durationSeconds);
                clip.durationSeconds = std::max(0.01,
                    std::max(needed, clip.durationSeconds - span));
            }
        dropEmptyRecordedClipsLocked();
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::insertGapBeforeNote(const juce::String& noteId, double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            // Where in the track the silence opens, in absolute seconds.  Only
            // the track holding the note is touched: a gap is an edit to one
            // part, not to the piece.
            auto at = -1.0;
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (note.id == noteId) at = clip.startSeconds + note.startSeconds;
            if (at < 0.0) continue;
            if (!changed) pushUndoLocked();
            changed = true;
            for (auto& clip : track.clips)
            {
                // A clip that begins after the point moves whole; the one the
                // note is in keeps its start and grows, with the notes from
                // there on carried along.
                if (clip.startSeconds >= at - 1.0e-9)
                {
                    clip.startSeconds += seconds;
                    continue;
                }
                const auto local = at - clip.startSeconds;
                auto moved = false;
                for (auto& note : clip.notes)
                    if (note.startSeconds >= local - 1.0e-9)
                    {
                        note.startSeconds += seconds;
                        moved = true;
                    }
                if (!moved) continue;
                auto needed = 0.0;
                for (const auto& note : clip.notes)
                    needed = std::max(needed, note.startSeconds + note.durationSeconds);
                clip.durationSeconds = std::max(needed, clip.durationSeconds + seconds);
            }
        }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::closeGapBeforeNote(const juce::String& noteId, double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            auto at = -1.0;
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    if (note.id == noteId) at = clip.startSeconds + note.startSeconds;
            if (at < 0.0) continue;
            // Never past the front of the piece: the silence being closed is
            // only as long as there is room to close it into.
            const auto span = std::min(seconds, at);
            if (span <= 0.0) continue;
            if (!changed) pushUndoLocked();
            changed = true;
            for (auto& clip : track.clips)
            {
                if (clip.startSeconds >= at - 1.0e-9)
                {
                    clip.startSeconds = std::max(0.0, clip.startSeconds - span);
                    continue;
                }
                const auto local = at - clip.startSeconds;
                auto moved = false;
                for (auto& note : clip.notes)
                    if (note.startSeconds >= local - 1.0e-9)
                    {
                        note.startSeconds = std::max(0.0, note.startSeconds - span);
                        moved = true;
                    }
                if (!moved) continue;
                auto needed = 0.0;
                for (const auto& note : clip.notes)
                    needed = std::max(needed, note.startSeconds + note.durationSeconds);
                clip.durationSeconds = std::max(0.01,
                    std::max(needed, clip.durationSeconds - span));
            }
        }
    }
    if (changed) sendChangeMessage();
}

std::vector<juce::String> ProjectModel::notesOverlapping(
    const juce::String& clipId, double fromSeconds, double toSeconds) const
{
    std::vector<juce::String> found;
    if (toSeconds <= fromSeconds) return found;
    const juce::ScopedLock guard(lock);
    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
        {
            if (clip.id != clipId) continue;
            for (const auto& note : clip.notes)
            {
                // Touching end to end is not overlapping: a phrase pasted
                // against the one before it replaces nothing.
                const auto start = clip.startSeconds + note.startSeconds;
                if (start < toSeconds - 1.0e-9
                    && start + note.durationSeconds > fromSeconds + 1.0e-9)
                    found.push_back(note.id);
            }
        }
    return found;
}

std::vector<juce::String> ProjectModel::insertNotes(
    const juce::String& clipId, const std::vector<NoteData>& noteTemplates,
    double absoluteStartSeconds)
{
    std::vector<juce::String> inserted;
    if (noteTemplates.empty()) return inserted;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.id == clipId)
                {
                    if (clip.durationSeconds <= 0.0) return inserted;
                    const auto utauMode = track.pitchAlgorithm == PitchAlgorithm::utau;
                    // A UTAU phrase may extend the clip it lands in, and so
                    // may anything landing in a clip with no recording behind
                    // it -- that is a span of the timeline, not a length of
                    // audio, the same rule drawing a note follows.  A
                    // recording ends where its audio does, so there a note is
                    // still cropped to what can actually sound.
                    const auto mayStretch = utauMode || !clipIsRecording(clip);
                    const auto localOrigin = mayStretch
                        ? std::max(0.0, absoluteStartSeconds - clip.startSeconds)
                        : juce::jlimit(0.0, clip.durationSeconds,
                            absoluteStartSeconds - clip.startSeconds);
                    std::vector<NoteData> candidates;
                    candidates.reserve(noteTemplates.size());
                    for (const auto& noteTemplate : noteTemplates)
                    {
                        auto note = noteTemplate;
                        note.startSeconds = localOrigin
                            + std::max(0.0, noteTemplate.startSeconds);
                        if (mayStretch)
                        {
                            // Copy every musical and UTAU-specific field
                            // verbatim.  Only the new object's id changes.
                            candidates.push_back(std::move(note));
                            continue;
                        }
                        const auto remaining = clip.durationSeconds - note.startSeconds;
                        if (remaining < 0.01) continue;
                        note.durationSeconds = std::min(
                            std::max(0.01, noteTemplate.durationSeconds), remaining);
                        note.consonantSeconds = std::min(note.consonantSeconds,
                                                        note.durationSeconds);
                        for (auto& point : note.contour)
                            point.timeSeconds = juce::jlimit(0.0,
                                note.durationSeconds, point.timeSeconds);
                        for (auto& point : note.pitchControlPoints)
                            point.timeSeconds = juce::jlimit(0.0,
                                note.durationSeconds, point.timeSeconds);
                        for (auto& marker : note.sibilantMarkers)
                            marker = juce::jlimit(0.0, note.durationSeconds, marker);
                        candidates.push_back(std::move(note));
                    }
                    if (candidates.empty()) return inserted;
                    pushUndoLocked();
                    if (utauMode)
                    {
                        const auto phraseStart = std::min_element(
                            candidates.begin(), candidates.end(),
                            [](const auto& left, const auto& right)
                            {
                                return left.startSeconds < right.startSeconds;
                            })->startSeconds;
                        auto phraseEnd = phraseStart;
                        for (const auto& note : candidates)
                            phraseEnd = std::max(phraseEnd,
                                note.startSeconds + note.durationSeconds);
                        const auto phraseDuration = std::max(0.01,
                                                            phraseEnd - phraseStart);
                        auto collisionStart = std::numeric_limits<double>::max();
                        for (const auto& existing : clip.notes)
                            if (existing.startSeconds < phraseEnd - 1.0e-9
                                && existing.startSeconds + existing.durationSeconds
                                    > phraseStart + 1.0e-9)
                                collisionStart = std::min(collisionStart,
                                                          existing.startSeconds);
                        if (collisionStart != std::numeric_limits<double>::max())
                        {
                            for (auto& existing : clip.notes)
                                if (existing.startSeconds >= collisionStart - 1.0e-9)
                                    existing.startSeconds += phraseDuration;
                            const auto align = collisionStart - phraseStart;
                            for (auto& note : candidates)
                                note.startSeconds += align;
                        }
                    }
                    else
                    {
                        candidates.front().connectedToPrevious = false;
                        candidates.back().connectedToNext = false;
                    }
                    inserted.reserve(candidates.size());
                    for (auto& note : candidates)
                    {
                        note.id = makeId("note");
                        inserted.push_back(note.id);
                        clip.notes.push_back(std::move(note));
                    }
                    std::stable_sort(clip.notes.begin(), clip.notes.end(),
                        [](const auto& left, const auto& right)
                        {
                            return left.startSeconds < right.startSeconds;
                        });
                    if (utauMode)
                        for (const auto& note : clip.notes)
                            clip.durationSeconds = std::max(clip.durationSeconds,
                                note.startSeconds + note.durationSeconds);
                    break;
                }
    }
    if (!inserted.empty()) sendChangeMessage();
    return inserted;
}

std::vector<juce::String> ProjectModel::duplicateNotes(
    const std::vector<juce::String>& noteIds, const juce::String& targetClipId,
    double absoluteStartSeconds)
{
    const auto data = snapshot();
    std::vector<std::pair<double, NoteData>> found;
    juce::String destination = targetClipId;
    for (const auto& track : data.tracks)
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
                if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end())
                {
                    if (destination.isEmpty()) destination = clip.id;
                    found.emplace_back(clip.startSeconds + note.startSeconds, note);
                }
    if (found.empty() || destination.isEmpty()) return {};
    std::stable_sort(found.begin(), found.end(), [](const auto& left, const auto& right)
    {
        return left.first < right.first;
    });
    const auto origin = found.front().first;
    std::vector<NoteData> templates;
    templates.reserve(found.size());
    for (auto& [absolute, note] : found)
    {
        note.startSeconds = absolute - origin;
        templates.push_back(std::move(note));
    }
    return insertNotes(destination, templates, absoluteStartSeconds);
}

void ProjectModel::resizeNote(const juce::String& noteId, double newStart, double newDuration)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            for (auto& clip : track.clips)
            {
                for (auto& note : clip.notes)
                {
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        const auto oldStart = note.startSeconds;
                        const auto oldDuration = std::max(0.01, note.durationSeconds);
                        const auto oldEnd = oldStart + oldDuration;
                        auto otherEnd = 0.0;
                        for (const auto& candidate : clip.notes)
                            if (candidate.id != noteId)
                                otherEnd = std::max(otherEnd,
                                    candidate.startSeconds + candidate.durationSeconds);
                        const auto wasTail = oldEnd >= otherEnd - 1.0e-6
                            && oldEnd >= clip.durationSeconds - 0.002;
                        const auto targetDuration = std::max(0.01, newDuration);
                        const auto oldAttack = juce::jlimit(0.0, oldDuration,
                            note.consonantSeconds);
                        const auto newAttack = std::min(oldAttack, targetDuration);
                        const auto remapTime = [&](double time)
                        {
                            // A negative UTAU head anchor belongs to the
                            // preutterance before the nominal note.  Resizing
                            // the note body must not collapse it back to zero.
                            if (time < 0.0) return time;
                            const auto clamped = juce::jlimit(0.0, oldDuration, time);
                            if (clamped <= oldAttack || oldDuration <= oldAttack + 1.0e-9)
                                return oldAttack > 1.0e-9
                                    ? clamped * newAttack / oldAttack : 0.0;
                            return newAttack + (clamped - oldAttack)
                                * (targetDuration - newAttack) / (oldDuration - oldAttack);
                        };
                        for (auto& point : note.contour)
                            point.timeSeconds = remapTime(point.timeSeconds);
                        for (auto& point : note.pitchControlPoints)
                            point.timeSeconds = remapTime(point.timeSeconds);
                        for (auto& marker : note.sibilantMarkers)
                            marker = remapTime(marker);
                        note.startSeconds = std::max(0.0, newStart);
                        note.durationSeconds = targetDuration;
                        note.consonantSeconds = newAttack;
                        if (wasTail)
                            clip.durationSeconds = std::max(0.01,
                                std::max(otherEnd, note.startSeconds + note.durationSeconds));
                        changed = true;
                        break;
                    }
                }
                if (changed) break;
            }
            if (changed) break;
        }
    }
    if (changed) sendChangeMessage();
}

juce::String ProjectModel::splitNote(const juce::String& noteId, double localSeconds)
{
    juce::String createdId;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            for (auto& clip : track.clips)
            {
                for (std::size_t noteIndex = 0; noteIndex < clip.notes.size(); ++noteIndex)
                {
                    if (clip.notes[noteIndex].id != noteId) continue;
                    const auto original = clip.notes[noteIndex];
                    const auto split = juce::jlimit(0.01,
                        std::max(0.01, original.durationSeconds - 0.01), localSeconds);
                    if (split <= 0.0099 || split >= original.durationSeconds - 0.0099)
                        return {};

                    const auto evaluate = [&](double time)
                    {
                        PitchPoint result;
                        result.timeSeconds = time;
                        if (original.contour.empty()) return result;
                        const auto right = std::lower_bound(original.contour.begin(),
                            original.contour.end(), time,
                            [](const PitchPoint& point, double value)
                            {
                                return point.timeSeconds < value;
                            });
                        const auto rightIndex = static_cast<std::size_t>(right == original.contour.end()
                            ? original.contour.size() - 1 : right - original.contour.begin());
                        const auto leftIndex = rightIndex > 0
                            && original.contour[rightIndex].timeSeconds > time
                                ? rightIndex - 1 : rightIndex;
                        const auto& left = original.contour[leftIndex];
                        const auto& next = original.contour[rightIndex];
                        const auto amount = next.timeSeconds > left.timeSeconds
                            ? static_cast<float>(juce::jlimit(0.0, 1.0,
                                (time - left.timeSeconds)
                                    / (next.timeSeconds - left.timeSeconds))) : 0.0f;
                        result.relativeCents = left.relativeCents
                            + (next.relativeCents - left.relativeCents) * amount;
                        result.withoutVibratoCents = left.withoutVibratoCents
                            + (next.withoutVibratoCents - left.withoutVibratoCents) * amount;
                        result.voiced = left.voiced && next.voiced;
                        result.manualTargetCents = left.manualTargetCents
                            + (next.manualTargetCents - left.manualTargetCents) * amount;
                        result.hasManualTarget = left.hasManualTarget && next.hasManualTarget;
                        return result;
                    };

                    auto left = original;
                    auto right = original;
                    left.durationSeconds = split;
                    left.consonantSeconds = std::min(original.consonantSeconds, split);
                    left.connectedToNext = false;
                    right.id = makeId("note");
                    createdId = right.id;
                    right.startSeconds = original.startSeconds + split;
                    right.durationSeconds = original.durationSeconds - split;
                    right.consonantSeconds = original.consonantSeconds > split
                        ? original.consonantSeconds - split : 0.0;
                    right.connectedToPrevious = false;

                    left.contour.clear();
                    right.contour.clear();
                    for (const auto& point : original.contour)
                    {
                        if (point.timeSeconds < split - 1.0e-8)
                            left.contour.push_back(point);
                        if (point.timeSeconds > split + 1.0e-8)
                        {
                            auto shifted = point;
                            shifted.timeSeconds -= split;
                            right.contour.push_back(shifted);
                        }
                    }
                    auto boundary = evaluate(split);
                    boundary.timeSeconds = split;
                    left.contour.push_back(boundary);
                    boundary.timeSeconds = 0.0;
                    right.contour.insert(right.contour.begin(), boundary);

                    left.pitchControlPoints.clear();
                    right.pitchControlPoints.clear();
                    if (!original.pitchControlPoints.empty())
                    {
                        const auto controlPitchAt = [&](double time)
                        {
                            const auto found = std::upper_bound(
                                original.pitchControlPoints.begin(),
                                original.pitchControlPoints.end(), time,
                                [](double value, const PitchCurveEditPoint& point)
                                {
                                    return value < point.timeSeconds;
                                });
                            if (found == original.pitchControlPoints.begin())
                                return found->targetMidi;
                            if (found == original.pitchControlPoints.end())
                                return original.pitchControlPoints.back().targetMidi;
                            const auto& before = *(found - 1);
                            const auto amount = found->timeSeconds > before.timeSeconds
                                ? static_cast<float>((time - before.timeSeconds)
                                    / (found->timeSeconds - before.timeSeconds)) : 0.0f;
                            return before.targetMidi
                                + (found->targetMidi - before.targetMidi) * amount;
                        };
                        for (const auto& point : original.pitchControlPoints)
                        {
                            if (point.timeSeconds < split - 1.0e-8)
                                left.pitchControlPoints.push_back(point);
                            if (point.timeSeconds > split + 1.0e-8)
                            {
                                auto shifted = point;
                                shifted.timeSeconds -= split;
                                right.pitchControlPoints.push_back(shifted);
                            }
                        }
                        const auto boundaryPitch = controlPitchAt(split);
                        left.pitchControlPoints.push_back({ split, boundaryPitch });
                        right.pitchControlPoints.insert(right.pitchControlPoints.begin(),
                            { 0.0, boundaryPitch });
                    }

                    left.amplitudeEnvelope.clear();
                    right.amplitudeEnvelope.clear();
                    if (!original.amplitudeEnvelope.empty())
                    {
                        const auto amplitudeAt = [&](double time)
                        {
                            const auto found = std::upper_bound(
                                original.amplitudeEnvelope.begin(),
                                original.amplitudeEnvelope.end(), time,
                                [](double value, const AmplitudeEnvelopePoint& point)
                                {
                                    return value < point.timeSeconds;
                                });
                            if (found == original.amplitudeEnvelope.begin())
                                return found->gainDb;
                            if (found == original.amplitudeEnvelope.end())
                                return original.amplitudeEnvelope.back().gainDb;
                            const auto& before = *(found - 1);
                            const auto span = found->timeSeconds - before.timeSeconds;
                            const auto amount = span > 1.0e-9
                                ? static_cast<float>(juce::jlimit(0.0, 1.0,
                                    (time - before.timeSeconds) / span)) : 0.0f;
                            return before.gainDb
                                + (found->gainDb - before.gainDb) * amount;
                        };
                        for (const auto& point : original.amplitudeEnvelope)
                        {
                            if (point.timeSeconds < split - 1.0e-8)
                                left.amplitudeEnvelope.push_back(point);
                            if (point.timeSeconds > split + 1.0e-8)
                            {
                                auto shifted = point;
                                shifted.timeSeconds -= split;
                                right.amplitudeEnvelope.push_back(shifted);
                            }
                        }
                        const auto boundaryGain = amplitudeAt(split);
                        left.amplitudeEnvelope.push_back({ split, boundaryGain });
                        right.amplitudeEnvelope.insert(right.amplitudeEnvelope.begin(),
                            { 0.0, boundaryGain });
                    }

                    left.sibilantMarkers.clear();
                    right.sibilantMarkers.clear();
                    for (const auto marker : original.sibilantMarkers)
                        if (marker <= split) left.sibilantMarkers.push_back(marker);
                        else right.sibilantMarkers.push_back(marker - split);

                    pushUndoLocked();
                    clip.notes[noteIndex] = std::move(left);
                    clip.notes.insert(clip.notes.begin()
                        + static_cast<std::ptrdiff_t>(noteIndex + 1), std::move(right));
                    break;
                }
                if (createdId.isNotEmpty()) break;
            }
            if (createdId.isNotEmpty()) break;
        }
    }
    if (createdId.isNotEmpty()) sendChangeMessage();
    return createdId;
}

juce::String ProjectModel::mergeNotes(const std::vector<juce::String>& noteIds)
{
    if (noteIds.size() < 2) return {};
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };

    juce::String mergedId;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            for (auto& clip : track.clips)
            {
                std::vector<std::size_t> selectedIndices;
                for (std::size_t index = 0; index < clip.notes.size(); ++index)
                    if (includes(clip.notes[index].id)) selectedIndices.push_back(index);
                // A merge is deliberately confined to one clip.  This also
                // verifies that every requested note still exists when the
                // asynchronous context-menu command is finally delivered.
                if (selectedIndices.size() != noteIds.size()) continue;

                std::stable_sort(selectedIndices.begin(), selectedIndices.end(),
                    [&](std::size_t left, std::size_t right)
                    {
                        return clip.notes[left].startSeconds
                            < clip.notes[right].startSeconds;
                    });
                const auto firstIndex = selectedIndices.front();
                const auto lastIndex = selectedIndices.back();
                auto merged = clip.notes[firstIndex];
                auto totalDuration = 0.0;
                for (const auto index : selectedIndices)
                    totalDuration += std::max(0.01, clip.notes[index].durationSeconds);

                merged.startSeconds = clip.notes[firstIndex].startSeconds;
                merged.durationSeconds = std::max(0.01, totalDuration);
                merged.label.clear();
                merged.connectedToPrevious = clip.notes[firstIndex].connectedToPrevious;
                merged.connectedToNext = clip.notes[lastIndex].connectedToNext;
                merged.consonantSeconds = std::min(merged.consonantSeconds,
                                                    merged.durationSeconds);
                // Curves from several independent notes use incompatible
                // local time origins.  Start the merged, lyric-less note as a
                // clean flat note instead of retaining misleading fragments.
                merged.contour.clear();
                merged.contour.push_back({ 0.0, 0.0f, 0.0f, true });
                merged.contour.push_back({ merged.durationSeconds, 0.0f, 0.0f, true });
                merged.pitchControlPoints.clear();
                merged.amplitudeEnvelope.clear();
                merged.sibilantMarkers.clear();
                mergedId = merged.id;

                pushUndoLocked();
                clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(),
                    [&](const auto& note) { return includes(note.id); }), clip.notes.end());
                clip.notes.push_back(std::move(merged));
                std::stable_sort(clip.notes.begin(), clip.notes.end(),
                    [](const auto& left, const auto& right)
                    {
                        return left.startSeconds < right.startSeconds;
                    });
                const auto mergedNote = std::find_if(clip.notes.begin(), clip.notes.end(),
                    [&](const auto& note) { return note.id == mergedId; });
                if (mergedNote != clip.notes.end())
                    clip.durationSeconds = std::max(clip.durationSeconds,
                        mergedNote->startSeconds + mergedNote->durationSeconds);
                break;
            }
            if (mergedId.isNotEmpty()) break;
        }
    }
    if (mergedId.isNotEmpty()) sendChangeMessage();
    return mergedId;
}

void ProjectModel::setNoteModulation(const juce::String& noteId, float modulation)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.modulation = juce::jlimit(0.0f, 2.0f, modulation);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteDrift(const juce::String& noteId, float drift)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.drift = juce::jlimit(0.0f, 2.0f, drift);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteTension(const juce::String& noteId, float tension)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.tension = juce::jlimit(-1.0f, 1.0f, tension);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteBreath(const juce::String& noteId, float breath)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.breath = juce::jlimit(0.0f, 1.0f, breath);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteFormant(const juce::String& noteId, float semitones)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.formantSemitones = juce::jlimit(-12.0f, 12.0f, semitones);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteGain(const juce::String& noteId, float gain)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.gain = juce::jlimit(0.0f, 4.0f, gain);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteAttack(const juce::String& noteId, double consonantSeconds,
                                 float attackSpeed)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        pushUndoLocked();
                        note.consonantSeconds = juce::jlimit(0.0, note.durationSeconds,
                                                            consonantSeconds);
                        note.attackSpeed = juce::jlimit(0.05f, 20.0f, attackSpeed);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteAttackSpeed(const juce::String& noteId, float attackSpeed)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        const auto next = juce::jlimit(0.05f, 20.0f, attackSpeed);
                        if (std::abs(note.attackSpeed - next) <= 1.0e-6f) return;
                        pushUndoLocked();
                        note.attackSpeed = next;
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

namespace
{
// What changing a note's lyric costs it, wherever that happens.
//
// Preutterance and overlap are millimetre marks on one particular recording,
// so they cannot follow the note to another one: kept, they pin the new
// sound's lead-in to a length its own oto never asked for, which also freezes
// the consonant handle, since an override outranks whatever the velocity says.
// Consonant velocity is a ratio and stays meaningful, so it is left alone.
//
// A hand-placed four-region split is where the consonant, the glide and the
// tail sit in one recording.  Another recording divides differently, so the
// note goes back to following its own oto rather than keeping proportions
// read off a waveform it no longer plays.
void relabelNote(NoteData& note, const juce::String& trimmed)
{
    note.label = trimmed;
    note.utauPreutteranceOverrideEnabled = false;
    note.utauPreutteranceSeconds = 0.0;
    note.utauOverlapOverrideEnabled = false;
    note.utauOverlapSeconds = 0.0;
    // An STP is a distance into one particular recording.  Another recording
    // has its sound somewhere else, so it goes back to zero with the rest.
    note.utauStpSeconds = 0.0;
    note.utauJieSplitSet = false;
    note.utauJieSplit1 = 0.0;
    note.utauJieSplit2 = 0.0;
    note.utauJieSplit3 = 0.0;
}
}

void ProjectModel::setNoteLabels(
    const std::vector<std::pair<juce::String, juce::String>>& labels)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& [noteId, label] : labels)
        {
            const auto trimmed = label.trim();
            for (auto& track : project.tracks)
                for (auto& clip : track.clips)
                    for (auto& note : clip.notes)
                        if (note.id == noteId && note.label != trimmed)
                        {
                            // Once for the whole batch, so it undoes as one.
                            if (!changed) pushUndoLocked();
                            relabelNote(note, trimmed);
                            changed = true;
                        }
        }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteLabel(const juce::String& noteId, const juce::String& label)
{
    const auto trimmed = label.trim();
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId && note.label != trimmed)
                    {
                        pushUndoLocked();
                        relabelNote(note, trimmed);
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteUtauFlags(const juce::String& noteId, const juce::String& flags)
{
    const auto trimmed = flags.trim();
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId && note.utauFlags != trimmed)
                    {
                        pushUndoLocked();
                        note.utauFlags = trimmed;
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesUtauFlags(const std::vector<juce::String>& noteIds,
                                     const juce::String& flags)
{
    if (noteIds.empty()) return;
    const auto trimmed = flags.trim();
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.utauFlags != trimmed)
                    {
                        if (!changed) pushUndoLocked();
                        note.utauFlags = trimmed;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesUtauConsonantVelocity(
    const std::vector<juce::String>& noteIds, int velocity)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.utauConsonantVelocity != velocity)
                    {
                        if (!changed) pushUndoLocked();
                        note.utauConsonantVelocity = velocity;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

double vibratoCentsAt(const NoteData& note, double localSeconds)
{
    if (!note.vibratoEnabled) return 0.0;
    const auto duration = note.durationSeconds;
    if (duration <= 1.0e-9 || note.vibratoDepthCents == 0.0) return 0.0;
    const auto share = juce::jlimit(0.0, 100.0, note.vibratoLengthPercent) / 100.0;
    const auto span = duration * share;
    if (span <= 1.0e-9) return 0.0;
    // UTAU measures the vibrato span back from the end of the note.
    const auto start = duration - span;
    if (localSeconds <= start) return 0.0;
    const auto position = juce::jlimit(0.0, 1.0, (localSeconds - start) / span);

    const auto fadeIn = juce::jlimit(0.0, 100.0, note.vibratoFadeInPercent) / 100.0;
    const auto fadeOut = juce::jlimit(0.0, 100.0, note.vibratoFadeOutPercent) / 100.0;
    auto envelope = 1.0;
    if (fadeIn > 1.0e-9) envelope = std::min(envelope, position / fadeIn);
    if (fadeOut > 1.0e-9) envelope = std::min(envelope, (1.0 - position) / fadeOut);
    envelope = juce::jlimit(0.0, 1.0, envelope);

    const auto cycleSeconds = std::max(0.01, note.vibratoCycleMs) / 1000.0;
    const auto phase = (localSeconds - start) / cycleSeconds
        + note.vibratoPhasePercent / 100.0;
    const auto swing = std::sin(phase * 2.0 * juce::MathConstants<double>::pi);
    const auto centre = note.vibratoOffsetPercent / 100.0;
    return note.vibratoDepthCents * envelope * (swing + centre);
}

void ProjectModel::setNotesUtauSplice(const std::vector<juce::String>& noteIds,
                                      bool enabled)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.utauSplice != enabled)
                    {
                        if (!changed) pushUndoLocked();
                        note.utauSplice = enabled;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesUtauFlagCurveEnabled(
    const std::vector<juce::String>& noteIds, bool enabled)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.utauFlagCurveEnabled != enabled)
                    {
                        if (!changed) pushUndoLocked();
                        note.utauFlagCurveEnabled = enabled;
                        // Nothing is written here.  This used to seed a flat
                        // two-point curve on g so the lane had something to
                        // drag; the lane now draws that starting line itself,
                        // for whichever flag is on show and across the stretch
                        // that flag reaches.  Seeding as well left g -- and
                        // only g -- opening with two handles instead of one,
                        // at the note's own start and end rather than the
                        // stretch it sounds for, and made a note count as
                        // carrying a curve before anything had been drawn.
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

bool ProjectModel::resetNotesUtauFlagCurves(
    const std::vector<juce::String>& noteIds)
{
    if (noteIds.empty()) return false;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.utauFlagCurveEnabled
                        && !note.utauFlagCurves.empty()
                        && std::find(noteIds.begin(), noteIds.end(), note.id)
                               != noteIds.end())
                    {
                        if (!changed) pushUndoLocked();
                        note.utauFlagCurves.clear();
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
    return changed;
}

bool ProjectModel::resetNotesUtauFlagCurve(
    const std::vector<juce::String>& noteIds, const juce::String& flag)
{
    if (noteIds.empty() || flag.isEmpty()) return false;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.utauFlagCurveEnabled
                        && std::find(noteIds.begin(), noteIds.end(), note.id)
                               != noteIds.end()
                        && std::any_of(note.utauFlagCurves.begin(),
                                       note.utauFlagCurves.end(),
                            [&flag](const auto& curve) { return curve.flag == flag; }))
                    {
                        if (!changed) pushUndoLocked();
                        std::erase_if(note.utauFlagCurves,
                            [&flag](const auto& curve) { return curve.flag == flag; });
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
    return changed;
}

bool ProjectModel::setNoteUtauFlagCurve(const juce::String& noteId,
                                       const juce::String& flag,
                                       std::vector<FlagCurvePoint> points)
{
    if (noteId.isEmpty() || flag.isEmpty()) return false;
    const auto& kind = flagCurveKindFor(flag);
    std::stable_sort(points.begin(), points.end(),
        [](const auto& left, const auto& right)
        {
            return left.timeSeconds < right.timeSeconds;
        });
    std::vector<FlagCurvePoint> normalized;
    normalized.reserve(points.size());
    for (auto point : points)
    {
        if (!std::isfinite(point.timeSeconds) || !std::isfinite(point.value)) continue;
        // A contour-wide shape means nothing to a single segment; it reads as a
        // straight line, so store it as one rather than leaving a value that
        // says something it cannot do.
        if (point.shape == PitchCurveShape::natural) point.shape = PitchCurveShape::linear;
        point.bezierX1 = juce::jlimit(0.0f, 1.0f, point.bezierX1);
        point.bezierX2 = juce::jlimit(0.0f, 1.0f, point.bezierX2);
        point.bezierY1 = juce::jlimit(-2.0f, 3.0f, point.bezierY1);
        point.bezierY2 = juce::jlimit(-2.0f, 3.0f, point.bezierY2);
        point.timeSeconds = juce::jlimit(-5.0, 60.0, point.timeSeconds);
        // The engine clamps this flag to that range either way; holding the
        // same one here keeps what is drawn and what is heard the same thing.
        point.value = juce::jlimit(kind.minimum, kind.maximum, point.value);
        // Two handles at the same instant would be a step the engine reads as
        // one; the later one wins, as it does there.
        if (!normalized.empty()
            && point.timeSeconds <= normalized.back().timeSeconds + 1.0e-5)
            normalized.back() = point;
        else
            normalized.push_back(point);
    }
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId)
                    {
                        const auto existing = flagCurvePointsFor(note, flag);
                        const auto same = existing.size() == normalized.size()
                            && std::equal(existing.begin(),
                                          existing.end(), normalized.begin(),
                                [](const auto& left, const auto& right)
                                {
                                    return std::abs(left.timeSeconds - right.timeSeconds) < 1.0e-9
                                        && std::abs(left.value - right.value) < 1.0e-6f
                                        && left.shape == right.shape
                                        && std::abs(left.bezierX1 - right.bezierX1) < 1.0e-6f
                                        && std::abs(left.bezierY1 - right.bezierY1) < 1.0e-6f
                                        && std::abs(left.bezierX2 - right.bezierX2) < 1.0e-6f
                                        && std::abs(left.bezierY2 - right.bezierY2) < 1.0e-6f;
                                });
                        if (same) return false;
                        pushUndoLocked();
                        // An empty list means this flag has no curve any
                        // more; the other flags' curves are untouched either
                        // way, which is what makes them independent.
                        std::erase_if(note.utauFlagCurves,
                            [&flag](const auto& curve) { return curve.flag == flag; });
                        if (!normalized.empty())
                            note.utauFlagCurves.push_back({ flag, normalized });
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
    return changed;
}

void ProjectModel::setNotesRegionFlags(const std::vector<juce::String>& noteIds,
                                       bool split, const juce::String& first,
                                       const juce::String& second,
                                       const juce::String& third,
                                       const juce::String& fourth)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                {
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) == noteIds.end())
                        continue;
                    if (!changed) pushUndoLocked();
                    note.utauFlagSplit = split;
                    note.utauRegionFlags1 = first.trim();
                    note.utauRegionFlags2 = second.trim();
                    note.utauRegionFlags3 = third.trim();
                    note.utauRegionFlags4 = fourth.trim();
                    changed = true;
                }
    }
    if (changed) sendChangeMessage();
}

bool ProjectModel::bakeNoteVibratoIntoPitch(const juce::String& noteId)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                {
                    if (note.id != noteId || !note.vibratoEnabled) continue;
                    if (note.durationSeconds <= 1.0e-9) continue;

                    // Base pitch under the swing: the edited curve if there is
                    // one, otherwise the note's own contour.
                    const auto baseAt = [&note](double time)
                    {
                        if (!note.pitchControlPoints.empty())
                            return evaluatePitchCurve(note.pitchControlPoints, time);
                        if (note.contour.empty()) return note.midiNote;
                        const auto cents = [&note](const PitchPoint& point)
                        {
                            return renderedPitchCents(note, point);
                        };
                        if (time <= note.contour.front().timeSeconds)
                            return note.midiNote + cents(note.contour.front()) / 100.0f;
                        if (time >= note.contour.back().timeSeconds)
                            return note.midiNote + cents(note.contour.back()) / 100.0f;
                        for (std::size_t i = 1; i < note.contour.size(); ++i)
                        {
                            const auto& left = note.contour[i - 1];
                            const auto& right = note.contour[i];
                            if (time > right.timeSeconds) continue;
                            const auto width = right.timeSeconds - left.timeSeconds;
                            const auto amount = width > 1.0e-9
                                ? static_cast<float>((time - left.timeSeconds) / width) : 0.0f;
                            return note.midiNote
                                + (cents(left) + (cents(right) - cents(left)) * amount) / 100.0f;
                        }
                        return note.midiNote + cents(note.contour.back()) / 100.0f;
                    };

                    // A vibrato is a sine, and from one crest to the next
                    // trough a sine is a single curve.  Sampling it eight times
                    // a cycle wrote a hundred points where a dozen would do,
                    // and every one of them then has to be dragged by hand.
                    //
                    // One point per crest and trough, joined by the cubic that
                    // fits a half sine, carries the same shape: handles at
                    // 0.36434 leave a worst-case error of 0.00019 of the swing,
                    // against 0.01 for the plain smooth join -- a fiftieth of a
                    // cent on a hundred-cent vibrato.
                    const auto cycle = std::max(0.02, note.vibratoCycleMs / 1000.0);
                    const auto span = note.durationSeconds
                        * juce::jlimit(0.0, 100.0, note.vibratoLengthPercent) / 100.0;
                    const auto swingStart = std::max(0.0, note.durationSeconds - span);
                    const auto phase = note.vibratoPhasePercent / 100.0;

                    std::vector<double> times;
                    times.push_back(0.0);
                    if (swingStart > 1.0e-6) times.push_back(swingStart);
                    // A sine is at an extreme a quarter turn in, and every half
                    // turn after that; it crosses zero halfway between.
                    std::vector<double> extremes;
                    for (auto index = 0; index < 8000; ++index)
                    {
                        const auto time = swingStart
                            + cycle * (0.25 + index * 0.5 - phase);
                        if (time >= note.durationSeconds) break;
                        if (time > swingStart + 1.0e-9)
                        {
                            times.push_back(time);
                            extremes.push_back(time);
                        }
                    }
                    // Between two extremes a half turn is one curve and needs
                    // nothing in between.  The stretch before the first and
                    // after the last is whatever the phase leaves over, so a
                    // crossing goes in it -- one point, two in all -- cutting
                    // it into quarter turns the fitted curves do cover.
                    const auto firstExtreme = extremes.empty()
                        ? note.durationSeconds : extremes.front();
                    const auto lastExtreme = extremes.empty()
                        ? swingStart : extremes.back();
                    std::vector<double> crossings;
                    for (auto index = -4; index < 8000; ++index)
                    {
                        const auto time = swingStart + cycle * (index * 0.5 - phase);
                        if (time >= note.durationSeconds - 1.0e-9) break;
                        if (time < swingStart - 1.0e-9) continue;
                        if (time < firstExtreme - 1.0e-9 || time > lastExtreme + 1.0e-9)
                        {
                            times.push_back(time);
                            crossings.push_back(time);
                        }
                    }
                    times.push_back(note.durationSeconds);
                    std::sort(times.begin(), times.end());
                    times.erase(std::unique(times.begin(), times.end(),
                        [](double left, double right)
                        {
                            return std::abs(left - right) < 1.0e-6;
                        }), times.end());

                    std::vector<PitchCurveEditPoint> points;
                    points.reserve(times.size());
                    for (const auto time : times)
                    {
                        PitchCurveEditPoint point;
                        point.timeSeconds = time;
                        point.targetMidi = baseAt(time)
                            + static_cast<float>(vibratoCentsAt(note, time) / 100.0);
                        // What the sine does between this point and the one
                        // before decides how they are joined.  Handles fitted
                        // to each arc: worst case 0.0002 of the swing, against
                        // 0.01 for a plain smooth join and 0.21 for a straight
                        // line -- a fiftieth of a cent on a hundred cent
                        // vibrato, against ten.
                        const auto kindOf = [&](double when)
                        {
                            const auto close = [when](double value)
                            {
                                return std::abs(value - when) < 1.0e-9;
                            };
                            if (std::any_of(extremes.begin(), extremes.end(), close))
                                return 1;   // crest or trough, where it is flat
                            if (std::any_of(crossings.begin(), crossings.end(), close))
                                return 2;   // zero crossing, where it is steepest
                            return 0;
                        };
                        const auto here = kindOf(time);
                        const auto before = points.empty()
                            ? 0 : kindOf(points.back().timeSeconds);
                        point.shape = PitchCurveShape::smooth;
                        if (before == 1 && here == 1)          // half a turn
                        {
                            point.shape = PitchCurveShape::customBezier;
                            point.bezierX1 = 0.36434f;
                            point.bezierY1 = 0.0f;
                            point.bezierX2 = 0.63566f;
                            point.bezierY2 = 1.0f;
                        }
                        else if (before == 2 && here == 1)     // rising quarter
                        {
                            point.shape = PitchCurveShape::customBezier;
                            point.bezierX1 = 0.33125f;
                            point.bezierY1 = 0.52033f;
                            point.bezierX2 = 0.64000f;
                            point.bezierY2 = 1.0f;
                        }
                        else if (before == 1 && here == 2)     // falling quarter
                        {
                            point.shape = PitchCurveShape::customBezier;
                            point.bezierX1 = 0.36000f;
                            point.bezierY1 = 0.0f;
                            point.bezierX2 = 0.66875f;
                            point.bezierY2 = 0.47967f;
                        }
                        points.push_back(point);
                    }

                    pushUndoLocked();
                    note.pitchControlPoints = std::move(points);
                    note.vibratoEnabled = false;
                    note.vibratoRealLine = false;
                    changed = true;
                }
    }
    if (changed) sendChangeMessage();
    return changed;
}

void ProjectModel::setNotesVibratoRealLine(const std::vector<juce::String>& noteIds,
                                           bool enabled)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.vibratoRealLine != enabled)
                    {
                        if (!changed) pushUndoLocked();
                        note.vibratoRealLine = enabled;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesVibrato(const std::vector<juce::String>& noteIds,
                                   const NoteData& parameters, bool enabled)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                {
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) == noteIds.end())
                        continue;
                    if (!changed) pushUndoLocked();
                    note.vibratoEnabled = enabled;
                    note.vibratoLengthPercent = parameters.vibratoLengthPercent;
                    note.vibratoCycleMs = parameters.vibratoCycleMs;
                    note.vibratoDepthCents = parameters.vibratoDepthCents;
                    note.vibratoFadeInPercent = parameters.vibratoFadeInPercent;
                    note.vibratoFadeOutPercent = parameters.vibratoFadeOutPercent;
                    note.vibratoPhasePercent = parameters.vibratoPhasePercent;
                    note.vibratoOffsetPercent = parameters.vibratoOffsetPercent;
                    changed = true;
                }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesUtauJieSplitCleared(const std::vector<juce::String>& noteIds)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.utauJieSplitSet)
                    {
                        if (!changed) pushUndoLocked();
                        note.utauJieSplitSet = false;
                        note.utauJieSplit1 = 0.0;
                        note.utauJieSplit2 = 0.0;
                        note.utauJieSplit3 = 0.0;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesUtauJieSplit(const std::vector<juce::String>& noteIds,
                                        double first, double second, double third)
{
    if (noteIds.empty()) return;
    // Keep the three boundaries ordered inside the note; a drag may collapse a
    // region to nothing but must never invert one.
    const auto third_ = juce::jlimit(0.0, 1.0, third);
    const auto second_ = juce::jlimit(0.0, third_, second);
    const auto first_ = juce::jlimit(0.0, second_, first);
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && (!note.utauJieSplitSet
                            || note.utauJieSplit1 != first_
                            || note.utauJieSplit2 != second_
                            || note.utauJieSplit3 != third_))
                    {
                        if (!changed) pushUndoLocked();
                        note.utauJieSplitSet = true;
                        note.utauJieSplit1 = first_;
                        note.utauJieSplit2 = second_;
                        note.utauJieSplit3 = third_;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::clearNotesUtauJieSplit(const std::vector<juce::String>& noteIds)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (std::find(noteIds.begin(), noteIds.end(), note.id) != noteIds.end()
                        && note.utauJieSplitSet)
                    {
                        if (!changed) pushUndoLocked();
                        note.utauJieSplitSet = false;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNotesUtauStp(const std::vector<juce::String>& noteIds,
                                   double seconds)
{
    if (noteIds.empty() || !std::isfinite(seconds)) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                {
                    if (std::find(noteIds.begin(), noteIds.end(), note.id)
                        == noteIds.end())
                        continue;
                    if (std::abs(note.utauStpSeconds - seconds) <= 1.0e-12) continue;
                    if (!changed) pushUndoLocked();
                    note.utauStpSeconds = seconds;
                    changed = true;
                }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::setNoteUtauTimingOverrides(
    const juce::String& noteId, bool enabled,
    double preutteranceSeconds, double overlapSeconds)
{
    if (!std::isfinite(preutteranceSeconds) || !std::isfinite(overlapSeconds))
        return;
    const auto normalizedPreutterance = std::max(0.0, preutteranceSeconds);
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId
                        && (note.utauPreutteranceOverrideEnabled != enabled
                            || note.utauOverlapOverrideEnabled != enabled
                            || (enabled && (std::abs(note.utauPreutteranceSeconds
                                                    - normalizedPreutterance) > 1.0e-9
                                || std::abs(note.utauOverlapSeconds
                                            - overlapSeconds) > 1.0e-9))))
                    {
                        pushUndoLocked();
                        note.utauPreutteranceOverrideEnabled = enabled;
                        note.utauPreutteranceSeconds = normalizedPreutterance;
                        note.utauOverlapOverrideEnabled = enabled;
                        note.utauOverlapSeconds = overlapSeconds;
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

bool ProjectModel::setNoteAmplitudeEnvelope(
    const juce::String& noteId, std::vector<AmplitudeEnvelopePoint> points)
{
    std::vector<std::pair<juce::String, std::vector<AmplitudeEnvelopePoint>>> envelopes;
    envelopes.emplace_back(noteId, std::move(points));
    return setNotesAmplitudeEnvelopes(std::move(envelopes));
}

bool ProjectModel::setNotesAmplitudeEnvelopes(
    std::vector<std::pair<juce::String, std::vector<AmplitudeEnvelopePoint>>> envelopes)
{
    std::erase_if(envelopes, [](auto& entry)
    {
        auto& points = entry.second;
        std::stable_sort(points.begin(), points.end(), [](const auto& left, const auto& right)
        {
            return left.timeSeconds < right.timeSeconds;
        });
        std::vector<AmplitudeEnvelopePoint> normalized;
        normalized.reserve(points.size());
        for (auto point : points)
        {
            if (!std::isfinite(point.timeSeconds) || !std::isfinite(point.gainDb)) continue;
            point.timeSeconds = juce::jlimit(-5.0, 60.0, point.timeSeconds);
            point.gainDb = juce::jlimit(-60.0f, 12.0f, point.gainDb);
            if (!normalized.empty()
                && point.timeSeconds <= normalized.back().timeSeconds + 1.0e-5)
                continue;
            normalized.push_back(point);
        }
        points = std::move(normalized);
        return entry.first.isEmpty() || points.size() < 2;
    });
    if (envelopes.empty()) return false;

    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (const auto entry = std::find_if(envelopes.begin(), envelopes.end(),
                            [&](const auto& value) { return value.first == note.id; });
                        entry != envelopes.end())
                    {
                        const auto& points = entry->second;
                        const auto same = note.amplitudeEnvelope.size() == points.size()
                            && std::equal(note.amplitudeEnvelope.begin(),
                                          note.amplitudeEnvelope.end(), points.begin(),
                                [](const auto& left, const auto& right)
                                {
                                    return std::abs(left.timeSeconds - right.timeSeconds) <= 1.0e-7
                                        && std::abs(left.gainDb - right.gainDb) <= 1.0e-4f;
                                });
                        if (same) continue;
                        if (!changed) pushUndoLocked();
                        note.amplitudeEnvelope = points;
                        changed = true;
                    }
    }
    if (changed) sendChangeMessage();
    return changed;
}

void ProjectModel::setNoteRobustPitchCurve(const juce::String& noteId, bool enabled)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    if (note.id == noteId && note.robustPitchCurve != enabled)
                    {
                        pushUndoLocked();
                        note.robustPitchCurve = enabled;
                        changed = true;
                        break;
                    }
    }
    if (changed) sendChangeMessage();
}

bool ProjectModel::setNotePitchCurve(const juce::String& noteId,
                                     std::vector<PitchCurveEditPoint> points,
                                     bool storeControlPoints)
{
    if (points.empty()) return false;
    std::stable_sort(points.begin(), points.end(), [](const auto& left, const auto& right)
    {
        return left.timeSeconds < right.timeSeconds;
    });
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                {
                    if (note.id != noteId) continue;
                    for (auto& point : points)
                    {
                        const auto minimumTime = storeControlPoints ? -30.0 : 0.0;
                        point.timeSeconds = juce::jlimit(minimumTime,
                                                         note.durationSeconds,
                                                         point.timeSeconds);
                        point.targetMidi = juce::jlimit(0.0f, 127.0f, point.targetMidi);
                    }
                    pushUndoLocked();
                    changed = true;
                    if (storeControlPoints) note.pitchControlPoints = points;
                    else note.pitchControlPoints.clear();

                    // User-created notes initially contain only two endpoints.
                    // Densify them before drawing so a freehand edit has the
                    // same 5 ms precision as imported Melodyne/FCPE contours.
                    auto needsDensifying = note.contour.size() < 2;
                    for (std::size_t index = 1; index < note.contour.size(); ++index)
                        needsDensifying = needsDensifying
                            || note.contour[index].timeSeconds
                                - note.contour[index - 1].timeSeconds > 0.0075;
                    if (needsDensifying)
                    {
                        const auto original = note.contour;
                        const auto evaluate = [&](double time)
                        {
                            PitchPoint result;
                            result.timeSeconds = time;
                            if (original.empty()) return result;
                            const auto right = std::lower_bound(original.begin(), original.end(), time,
                                [](const PitchPoint& point, double value)
                                {
                                    return point.timeSeconds < value;
                                });
                            const auto rightIndex = static_cast<std::size_t>(right == original.end()
                                ? original.size() - 1 : right - original.begin());
                            const auto leftIndex = rightIndex > 0
                                && original[rightIndex].timeSeconds > time ? rightIndex - 1 : rightIndex;
                            const auto& left = original[leftIndex];
                            const auto& next = original[rightIndex];
                            const auto amount = next.timeSeconds > left.timeSeconds
                                ? static_cast<float>(juce::jlimit(0.0, 1.0,
                                    (time - left.timeSeconds)
                                        / (next.timeSeconds - left.timeSeconds))) : 0.0f;
                            result.relativeCents = left.relativeCents
                                + (next.relativeCents - left.relativeCents) * amount;
                            result.withoutVibratoCents = left.withoutVibratoCents
                                + (next.withoutVibratoCents - left.withoutVibratoCents) * amount;
                            result.voiced = left.voiced && next.voiced;
                            if (left.hasManualTarget && next.hasManualTarget)
                            {
                                result.hasManualTarget = true;
                                result.manualTargetCents = left.manualTargetCents
                                    + (next.manualTargetCents - left.manualTargetCents) * amount;
                            }
                            return result;
                        };
                        note.contour.clear();
                        for (double time = 0.0; time < note.durationSeconds; time += 0.005)
                            note.contour.push_back(evaluate(time));
                        note.contour.push_back(evaluate(note.durationSeconds));
                    }

                    const auto firstTime = points.front().timeSeconds;
                    const auto lastTime = points.back().timeSeconds;
                    const auto targetAt = [&](double time)
                    {
                        return evaluatePitchCurve(points, time);
                    };
                    if (points.size() == 1 && !note.contour.empty())
                    {
                        auto nearest = std::min_element(note.contour.begin(), note.contour.end(),
                            [&](const auto& left, const auto& right)
                            {
                                return std::abs(left.timeSeconds - firstTime)
                                    < std::abs(right.timeSeconds - firstTime);
                            });
                        nearest->manualTargetCents =
                            (points.front().targetMidi - note.midiNote) * 100.0f;
                        nearest->hasManualTarget = true;
                    }
                    else
                    {
                        // Control points describe the whole note, so they apply
                        // to all of it.  Anchors get trimmed away from the ends
                        // where a note abuts its neighbour, and writing only
                        // between the outermost anchors left the contour holding
                        // its old value beyond them - a step down at the first
                        // anchor and back up at the last, which is what showed
                        // up as a break across the consonant even though the
                        // anchors themselves were continuous.  Outside the
                        // anchor span evaluatePitchCurve holds the end value,
                        // matching what encodePitchbend sends.
                        //
                        // A freehand or line stroke is a local edit and stays
                        // confined to the stretch it covers, so that it cannot
                        // wipe measured pitch elsewhere in the note.
                        for (auto& point : note.contour)
                            if (point.voiced
                                && (storeControlPoints
                                    || (point.timeSeconds >= firstTime - 1.0e-7
                                        && point.timeSeconds <= lastTime + 1.0e-7)))
                            {
                                point.manualTargetCents =
                                    (targetAt(point.timeSeconds) - note.midiNote) * 100.0f;
                                point.hasManualTarget = true;
                            }
                    }
                    break;
                }
    }
    if (changed) sendChangeMessage();
    return changed;
}

ProjectModel::PlannedNote ProjectModel::plannedNoteFor(
    double startSeconds, double requestedSeconds, double clipSeconds,
    const std::vector<NoteSpan>& occupied)
{
    // Anything shorter than this is widened to the model's own floor below,
    // which would put the overlap straight back in.
    constexpr auto shortest = 0.01;
    constexpr auto epsilon = 1.0e-9;

    PlannedNote planned;
    if (startSeconds < -epsilon) return planned;
    // Past the end of the clip there is no room.  Clamping instead is what
    // used to stack notes on the final instant.
    if (startSeconds > clipSeconds - shortest + epsilon) return planned;

    auto room = std::min(requestedSeconds, clipSeconds - startSeconds);
    for (const auto& span : occupied)
    {
        const auto end = span.startSeconds + span.durationSeconds;
        // The moment asked for is inside a note that is already there.
        if (span.startSeconds <= startSeconds + epsilon
            && startSeconds < end - epsilon)
            return planned;
        // A note begins further along: the new one stops where it starts.
        if (span.startSeconds > startSeconds)
            room = std::min(room, span.startSeconds - startSeconds);
    }
    if (room < shortest - epsilon) return planned;

    planned.create = true;
    planned.startSeconds = startSeconds;
    planned.durationSeconds = room;
    return planned;
}

namespace
{
// How far into a clip a note may reach.  A clip with a recording behind it is
// as long as its audio and that is the end of it; one composed here is only a
// span of the timeline, so a note may push it out.
double writableEndOf(const ClipData& clip, double startSeconds, double duration)
{
    if (clip.sourceFile != juce::File()) return clip.durationSeconds;
    return std::max(clip.durationSeconds, startSeconds + std::max(0.0, duration));
}
}

juce::String ProjectModel::addClip(const juce::String& trackId,
                                   double startSeconds, double durationSeconds)
{
    juce::String created;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            if (track.id == trackId && track.compose)
            {
                pushUndoLocked();
                ClipData clip;
                clip.id = makeId("clip");
                clip.startSeconds = std::max(0.0, startSeconds);
                clip.durationSeconds = std::max(0.01, durationSeconds);
                clip.sourceDurationSeconds = clip.durationSeconds;
                created = clip.id;
                track.clips.push_back(std::move(clip));
                std::stable_sort(track.clips.begin(), track.clips.end(),
                    [](const auto& left, const auto& right)
                    { return left.startSeconds < right.startSeconds; });
                break;
            }
    }
    if (created.isNotEmpty()) sendChangeMessage();
    return created;
}

juce::String ProjectModel::addNoteLocked(ClipData& destination,
                                         const PlannedNote& planned, float midiNote)
{
    pushUndoLocked();
    NoteData note;
    note.id = makeId("note");
    note.startSeconds = planned.startSeconds;
    note.durationSeconds = planned.durationSeconds;
    note.consonantSeconds = std::min(0.04, note.durationSeconds * 0.3);
    note.midiNote = juce::jlimit(0.0f, 127.0f, midiNote);
    note.sourceMidiCenter = note.midiNote;
    note.contour.push_back({ 0.0, 0.0f, 0.0f, true });
    note.contour.push_back({ note.durationSeconds, 0.0f, 0.0f, true });
    const auto id = note.id;
    const auto reach = note.startSeconds + note.durationSeconds;
    destination.notes.push_back(std::move(note));
    if (destination.sourceFile == juce::File() && destination.durationSeconds < reach)
    {
        destination.durationSeconds = reach;
        destination.sourceDurationSeconds = reach;
    }
    std::stable_sort(destination.notes.begin(), destination.notes.end(),
        [](const auto& left, const auto& right) { return left.startSeconds < right.startSeconds; });
    return id;
}

void ProjectModel::flattenNotePitch(const std::vector<juce::String>& noteIds)
{
    if (noteIds.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                {
                    if (std::find(noteIds.begin(), noteIds.end(), note.id)
                        == noteIds.end())
                        continue;
                    if (!changed) pushUndoLocked();
                    changed = true;

                    for (auto& point : note.contour)
                    {
                        // Every frame, not only the voiced ones.  Where nothing
                        // was detected the note should move with the rest
                        // rather than keep whatever it was holding.
                        point.manualTargetCents = 0.0f;
                        point.hasManualTarget = true;
                    }
                    // Drift and modulation reshape the measured curve, and
                    // there is no longer a curve to reshape.
                    note.drift = 0.0f;
                    note.modulation = 0.0f;
                    note.pitchControlPoints = {
                        { 0.0, note.midiNote },
                        { note.durationSeconds, note.midiNote },
                    };
                }
    }
    if (changed) sendChangeMessage();
}

double ProjectModel::firstFreeStartFrom(double startSeconds,
                                        const std::vector<NoteSpan>& occupied)
{
    constexpr auto epsilon = 1.0e-9;
    auto start = startSeconds;
    // One pass per note at most: each step lands on the end of a note that
    // covered the last position, and a note cannot be crossed twice.  The
    // bound also keeps a zero-length note from spinning here.
    for (std::size_t step = 0; step <= occupied.size(); ++step)
    {
        auto moved = false;
        for (const auto& span : occupied)
        {
            const auto end = span.startSeconds + span.durationSeconds;
            if (span.startSeconds <= start + epsilon && start < end - epsilon)
            {
                start = end;
                moved = true;
            }
        }
        if (!moved) break;
    }
    return start;
}

juce::String ProjectModel::addNoteFrom(const juce::String& preferredClipId,
                                       double absoluteFromSeconds,
                                       double maximumDuration, float midiNote)
{
    juce::String created;
    {
        const juce::ScopedLock guard(lock);
        ClipData* destination = nullptr;
        for (auto& track : project.tracks)
            if (track.compose)
                for (auto& clip : track.clips)
                {
                    if (clip.id == preferredClipId) destination = &clip;
                    if (destination == nullptr
                        && absoluteFromSeconds >= clip.startSeconds
                        && absoluteFromSeconds <= clip.startSeconds + clip.durationSeconds)
                        destination = &clip;
                }
        if (destination != nullptr)
        {
            std::vector<NoteSpan> occupied;
            occupied.reserve(destination->notes.size());
            for (const auto& existing : destination->notes)
                occupied.push_back({ existing.startSeconds, existing.durationSeconds });
            const auto from = firstFreeStartFrom(
                absoluteFromSeconds - destination->startSeconds, occupied);
            const auto planned = plannedNoteFor(from, maximumDuration,
                                                writableEndOf(*destination, from,
                                                              maximumDuration),
                                                occupied);
            if (planned.create)
                created = addNoteLocked(*destination, planned, midiNote);
        }
    }
    if (created.isNotEmpty()) sendChangeMessage();
    return created;
}

juce::String ProjectModel::addNote(const juce::String& preferredClipId,
                                   double absoluteStart, double duration, float midiNote)
{
    juce::String created;
    {
        const juce::ScopedLock guard(lock);
        ClipData* destination = nullptr;
        for (auto& track : project.tracks)
            if (track.compose)
                for (auto& clip : track.clips)
                {
                    if (clip.id == preferredClipId) destination = &clip;
                    if (destination == nullptr
                        && absoluteStart >= clip.startSeconds
                        && absoluteStart <= clip.startSeconds + clip.durationSeconds)
                        destination = &clip;
                }
        if (destination != nullptr)
        {
            std::vector<NoteSpan> occupied;
            occupied.reserve(destination->notes.size());
            for (const auto& existing : destination->notes)
                occupied.push_back({ existing.startSeconds, existing.durationSeconds });
            const auto local = absoluteStart - destination->startSeconds;
            const auto planned = plannedNoteFor(local, duration,
                                                writableEndOf(*destination, local, duration),
                                                occupied);
            if (!planned.create) return {};
            created = addNoteLocked(*destination, planned, midiNote);
        }
    }
    if (created.isNotEmpty()) sendChangeMessage();
    return created;
}

void ProjectModel::removeNote(const juce::String& noteId)
{
    removeNotes({ noteId });
}

std::vector<std::size_t> ProjectModel::regionsToImport(
    const std::vector<RegionSpan>& regions)
{
    constexpr auto epsilon = 1.0e-9;
    // Longest first, so the row that covers the most of the syllable is the
    // one kept; ties go to whichever came first in the file, which keeps the
    // result stable rather than dependent on the sort.
    std::vector<std::size_t> order(regions.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(),
        [&regions](std::size_t left, std::size_t right)
        {
            const auto leftLength = regions[left].endSeconds - regions[left].startSeconds;
            const auto rightLength = regions[right].endSeconds - regions[right].startSeconds;
            // Two rows of the same span at different offsets do not subtract
            // to bitwise equal lengths -- 0.3-0.1 and 0.4-0.2 differ in the
            // last bits -- and which alias you get must not hinge on that.
            // Equal within a nanosecond counts as equal, and stable_sort then
            // leaves the earlier row first.
            if (std::abs(leftLength - rightLength) <= epsilon) return false;
            return leftLength > rightLength;
        });

    std::vector<std::size_t> kept;
    for (const auto candidate : order)
    {
        const auto& region = regions[candidate];
        if (region.endSeconds - region.startSeconds < epsilon) continue;
        const auto clashes = std::any_of(kept.begin(), kept.end(),
            [&](std::size_t taken)
            {
                return region.startSeconds < regions[taken].endSeconds - epsilon
                    && regions[taken].startSeconds < region.endSeconds - epsilon;
            });
        if (!clashes) kept.push_back(candidate);
    }
    // Back into the order the file gave them, so the notes read left to right.
    std::sort(kept.begin(), kept.end());
    return kept;
}

bool ProjectModel::clipIsRecording(const ClipData& clip)
{
    if (clip.sourceFile == juce::File()) return false;
    return !clip.sourceFile.hasFileExtension("ust;mid;midi;mpd;hjpx;hspx");
}

void ProjectModel::dropEmptyRecordedClipsLocked()
{
    for (auto& track : project.tracks)
        std::erase_if(track.clips, [](const ClipData& clip)
        {
            return clip.notes.empty() && clipIsRecording(clip);
        });
}

void ProjectModel::removeNotes(const std::vector<juce::String>& noteIds)
{
    const auto includes = [&](const juce::String& id)
    {
        return std::find(noteIds.begin(), noteIds.end(), id) != noteIds.end();
    };
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& track : project.tracks)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes)
                    changed = changed || includes(note.id);
        if (!changed) return;
        pushUndoLocked();
        for (auto& track : project.tracks)
        {
            struct Positioned { NoteData* note; double start; };
            std::vector<Positioned> ordered;
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    ordered.push_back({ &note, clip.startSeconds + note.startSeconds });
            std::stable_sort(ordered.begin(), ordered.end(),
                [](const auto& left, const auto& right) { return left.start < right.start; });
            for (std::size_t index = 0; index < ordered.size(); ++index)
                if (includes(ordered[index].note->id))
                {
                    if (index > 0 && !includes(ordered[index - 1].note->id))
                        ordered[index - 1].note->connectedToNext = false;
                    if (index + 1 < ordered.size() && !includes(ordered[index + 1].note->id))
                        ordered[index + 1].note->connectedToPrevious = false;
                }
            for (auto& clip : track.clips)
                clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(),
                    [&](const auto& note) { return includes(note.id); }), clip.notes.end());
        }
        dropEmptyRecordedClipsLocked();
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::toggleNoteConnection(const juce::String& noteId)
{
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        for (auto& track : project.tracks)
        {
            struct Positioned { NoteData* note; double start; };
            std::vector<Positioned> ordered;
            for (auto& clip : track.clips)
                for (auto& note : clip.notes)
                    ordered.push_back({ &note, clip.startSeconds + note.startSeconds });
            std::stable_sort(ordered.begin(), ordered.end(),
                [](const auto& left, const auto& right) { return left.start < right.start; });
            for (std::size_t index = 1; index < ordered.size(); ++index)
                if (ordered[index].note->id == noteId)
                {
                    pushUndoLocked();
                    const auto connected = ordered[index].note->connectedToPrevious
                        && ordered[index - 1].note->connectedToNext;
                    ordered[index].note->connectedToPrevious = !connected;
                    ordered[index - 1].note->connectedToNext = !connected;
                    changed = true;
                    break;
                }
            if (changed) break;
        }
    }
    if (changed) sendChangeMessage();
}

void ProjectModel::applySourceSettings(const juce::File& source,
                                       const std::vector<SampleRegionSetting>& rows)
{
    if (rows.empty()) return;
    auto changed = false;
    {
        const juce::ScopedLock guard(lock);
        struct Entry { ClipData* clip; NoteData* note; };
        std::vector<Entry> entries;
        for (auto& track : project.tracks)
            for (auto& clip : track.clips)
                if (clip.sourceFile == source)
                    for (auto& note : clip.notes) entries.push_back({ &clip, &note });
        std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
        {
            return left.clip->sourceOffsetSeconds < right.clip->sourceOffsetSeconds;
        });
        if (entries.empty()) return;
        pushUndoLocked();
        for (std::size_t index = 0; index < std::min(entries.size(), rows.size()); ++index)
        {
            auto& clip = *entries[index].clip;
            auto& note = *entries[index].note;
            const auto& row = rows[index];
            note.label = row.name.trim();
            clip.sourceOffsetSeconds = std::max(0.0, row.regionStartSeconds);
            clip.sourceDurationSeconds = std::max(0.001,
                row.regionEndSeconds - row.regionStartSeconds);
            // Source-region editing establishes a new linear mapping.  An old
            // MPD warp refers to the previous source range and must not be
            // silently applied to the newly selected samples.
            clip.sourceTimeMap.clear();
            note.gain = juce::jlimit(0.0f, 4.0f,
                static_cast<float>(row.melodyneAmplitude));
            const auto targetPerSource = clip.durationSeconds / clip.sourceDurationSeconds;
            note.consonantSeconds = juce::jlimit(0.0, note.durationSeconds,
                row.fixedDurationSeconds * targetPerSource);
            if (note.sourceMidiCenter >= 0.0f)
                note.midiNote = juce::jlimit(0.0f, 127.0f,
                    note.sourceMidiCenter + static_cast<float>(row.relativePitchCents / 100.0));
            if (row.melodyneData)
            {
                note.drift = juce::jlimit(0.0f, 2.0f,
                    static_cast<float>(row.melodynePitchDrift));
                note.modulation = juce::jlimit(0.0f, 2.0f,
                    static_cast<float>(row.melodynePitchModulation));
                note.formantSemitones = juce::jlimit(-12.0f, 12.0f,
                    static_cast<float>(row.melodyneFormantCents / 100.0));
                note.breath = juce::jlimit(0.0f, 1.0f,
                    static_cast<float>(row.melodyneSibilantBalance));
                note.attackSpeed = juce::jlimit(0.05f, 20.0f,
                    static_cast<float>(row.melodyneAttackSeconds > 1.0e-6
                        ? row.fixedDurationSeconds / row.melodyneAttackSeconds : 1.0));
            }
            changed = true;
        }
    }
    if (changed) sendChangeMessage();
}

juce::ValueTree ProjectModel::toValueTree(const juce::File& projectFile) const
{
    const auto data = snapshot();
    juce::ValueTree root("HachiShifterProject");
    root.setProperty("version", 13, nullptr);
    root.setProperty("name", data.name, nullptr);
    root.setProperty("bpm", data.bpm, nullptr);
    root.setProperty("beatOriginSeconds", data.beatOriginSeconds, nullptr);
    root.setProperty("numerator", data.numerator, nullptr);
    root.setProperty("denominator", data.denominator, nullptr);
    root.setProperty("gridDivision", data.gridDivision, nullptr);
    root.setProperty("noteEditDivision", data.noteEditDivision, nullptr);
    root.setProperty("baseScale", data.baseScale, nullptr);

    for (const auto& change : data.tempoChanges)
    {
        juce::ValueTree tempoTree("TempoChange");
        tempoTree.setProperty("quarterPosition", change.quarterPosition, nullptr);
        tempoTree.setProperty("bpm", change.bpm, nullptr);
        root.addChild(tempoTree, -1, nullptr);
    }

    for (const auto& track : data.tracks)
    {
        juce::ValueTree trackTree("Track");
        trackTree.setProperty("id", track.id, nullptr);
        trackTree.setProperty("name", track.name, nullptr);
        trackTree.setProperty("compose", track.compose, nullptr);
        trackTree.setProperty("muted", track.muted, nullptr);
        trackTree.setProperty("solo", track.solo, nullptr);
        trackTree.setProperty("referenceOnly", track.referenceOnly, nullptr);
        trackTree.setProperty("volume", track.volume, nullptr);
        trackTree.setProperty("pan", track.pan, nullptr);
        trackTree.setProperty("smoothOverlaps", track.smoothOverlaps, nullptr);
        trackTree.setProperty("normalizeVolume", track.normalizeVolume, nullptr);
        trackTree.setProperty("voicebankDirectory",
                              track.voicebankDirectory.getFullPathName(), nullptr);
        trackTree.setProperty("utauConsonantVelocity", track.utauConsonantVelocity, nullptr);
        trackTree.setProperty("utauGlobalFlags", track.utauGlobalFlags, nullptr);
        trackTree.setProperty("utauMode", utauModeKey(track.utauMode), nullptr);
        // Still written so a build from before 谋 opens the project in the
        // nearest mode it has rather than dropping to plain UTAU.
        trackTree.setProperty("utauFourRegion",
                              utauModeUsesRegions(track.utauMode), nullptr);
        if (projectFile != juce::File{} && track.voicebankDirectory != juce::File{})
        {
            const auto relative = track.voicebankDirectory.getRelativePathFrom(
                projectFile.getParentDirectory());
            if (relative.isNotEmpty() && !juce::File::isAbsolutePath(relative))
                trackTree.setProperty("voicebankDirectoryRelative", relative, nullptr);
        }
        trackTree.setProperty("pitchAlgorithm", pitchAlgorithmName(track.pitchAlgorithm), nullptr);
        trackTree.setProperty("stretchAlgorithm", stretchAlgorithmName(track.stretchAlgorithm), nullptr);
        trackTree.setProperty("renderOrder", renderOrderName(track.renderOrder), nullptr);

        for (const auto& clip : track.clips)
        {
            juce::ValueTree clipTree("Clip");
            clipTree.setProperty("id", clip.id, nullptr);
            clipTree.setProperty("sourceFile", clip.sourceFile.getFullPathName(), nullptr);
            if (projectFile != juce::File{} && clip.sourceFile != juce::File{})
            {
                const auto relative = clip.sourceFile.getRelativePathFrom(
                    projectFile.getParentDirectory());
                if (relative.isNotEmpty() && !juce::File::isAbsolutePath(relative))
                    clipTree.setProperty("sourceFileRelative", relative, nullptr);
            }
            clipTree.setProperty("startSeconds", clip.startSeconds, nullptr);
            clipTree.setProperty("sourceOffsetSeconds", clip.sourceOffsetSeconds, nullptr);
            clipTree.setProperty("sourceDurationSeconds", clip.sourceDurationSeconds, nullptr);
            clipTree.setProperty("durationSeconds", clip.durationSeconds, nullptr);
            clipTree.setProperty("fadeInSeconds", clip.fadeInSeconds, nullptr);
            clipTree.setProperty("fadeOutSeconds", clip.fadeOutSeconds, nullptr);
            clipTree.setProperty("crossfadeInSeconds", clip.crossfadeInSeconds, nullptr);
            clipTree.setProperty("crossfadeOutSeconds", clip.crossfadeOutSeconds, nullptr);
            clipTree.setProperty("gain", clip.gain, nullptr);
            clipTree.setProperty("muted", clip.muted, nullptr);
            clipTree.setProperty("glideConnectedToNext", clip.glideConnectedToNext, nullptr);
            clipTree.setProperty("glideConnectedFromPrevious", clip.glideConnectedFromPrevious, nullptr);

            for (const auto& point : clip.sourceTimeMap)
            {
                juce::ValueTree pointTree("SourceTimePoint");
                pointTree.setProperty("targetSeconds", point.targetSeconds, nullptr);
                pointTree.setProperty("sourceSeconds", point.sourceSeconds, nullptr);
                clipTree.addChild(pointTree, -1, nullptr);
            }

            for (const auto& note : clip.notes)
            {
                juce::ValueTree noteTree("Note");
                noteTree.setProperty("id", note.id, nullptr);
                noteTree.setProperty("label", note.label, nullptr);
                noteTree.setProperty("utauFlags", note.utauFlags, nullptr);
                noteTree.setProperty("vibratoEnabled", note.vibratoEnabled, nullptr);
                noteTree.setProperty("vibratoLengthPercent", note.vibratoLengthPercent, nullptr);
                noteTree.setProperty("vibratoCycleMs", note.vibratoCycleMs, nullptr);
                noteTree.setProperty("vibratoDepthCents", note.vibratoDepthCents, nullptr);
                noteTree.setProperty("vibratoFadeInPercent", note.vibratoFadeInPercent, nullptr);
                noteTree.setProperty("vibratoFadeOutPercent", note.vibratoFadeOutPercent, nullptr);
                noteTree.setProperty("vibratoPhasePercent", note.vibratoPhasePercent, nullptr);
                noteTree.setProperty("vibratoOffsetPercent", note.vibratoOffsetPercent, nullptr);
                noteTree.setProperty("vibratoRealLine", note.vibratoRealLine, nullptr);
                noteTree.setProperty("utauFlagSplit", note.utauFlagSplit, nullptr);
                noteTree.setProperty("utauFlagCurveEnabled",
                                     note.utauFlagCurveEnabled, nullptr);
                noteTree.setProperty("utauSplice", note.utauSplice, nullptr);
                noteTree.setProperty("utauRegionFlags1", note.utauRegionFlags1, nullptr);
                noteTree.setProperty("utauRegionFlags2", note.utauRegionFlags2, nullptr);
                noteTree.setProperty("utauRegionFlags3", note.utauRegionFlags3, nullptr);
                noteTree.setProperty("utauRegionFlags4", note.utauRegionFlags4, nullptr);
                noteTree.setProperty("utauJieSplitSet", note.utauJieSplitSet, nullptr);
                noteTree.setProperty("utauJieSplit1", note.utauJieSplit1, nullptr);
                noteTree.setProperty("utauJieSplit2", note.utauJieSplit2, nullptr);
                noteTree.setProperty("utauJieSplit3", note.utauJieSplit3, nullptr);
                noteTree.setProperty("utauConsonantVelocity",
                                     note.utauConsonantVelocity, nullptr);
                noteTree.setProperty("utauConsonantVelocityInherited",
                    note.utauConsonantVelocity == inheritedUtauConsonantVelocity,
                    nullptr);
                noteTree.setProperty("utauPreutteranceOverrideEnabled",
                    note.utauPreutteranceOverrideEnabled, nullptr);
                noteTree.setProperty("utauPreutteranceSeconds",
                    note.utauPreutteranceSeconds, nullptr);
                noteTree.setProperty("utauOverlapOverrideEnabled",
                    note.utauOverlapOverrideEnabled, nullptr);
                noteTree.setProperty("utauOverlapSeconds",
                    note.utauOverlapSeconds, nullptr);
                noteTree.setProperty("utauStpSeconds", note.utauStpSeconds, nullptr);
                noteTree.setProperty("startSeconds", note.startSeconds, nullptr);
                noteTree.setProperty("durationSeconds", note.durationSeconds, nullptr);
                noteTree.setProperty("consonantSeconds", note.consonantSeconds, nullptr);
                noteTree.setProperty("midiNote", note.midiNote, nullptr);
                noteTree.setProperty("sourceMidiCenter", note.sourceMidiCenter, nullptr);
                noteTree.setProperty("modulation", note.modulation, nullptr);
                noteTree.setProperty("drift", note.drift, nullptr);
                noteTree.setProperty("tension", note.tension, nullptr);
                noteTree.setProperty("breath", note.breath, nullptr);
                noteTree.setProperty("formantSemitones", note.formantSemitones, nullptr);
                noteTree.setProperty("gain", note.gain, nullptr);
                noteTree.setProperty("attackSpeed", note.attackSpeed, nullptr);
                noteTree.setProperty("robustPitchCurve", note.robustPitchCurve, nullptr);
                noteTree.setProperty("connectedToPrevious", note.connectedToPrevious, nullptr);
                noteTree.setProperty("connectedToNext", note.connectedToNext, nullptr);
                for (const auto& point : note.contour)
                {
                    juce::ValueTree pointTree("PitchPoint");
                    pointTree.setProperty("timeSeconds", point.timeSeconds, nullptr);
                    pointTree.setProperty("relativeCents", point.relativeCents, nullptr);
                    pointTree.setProperty("withoutVibratoCents", point.withoutVibratoCents, nullptr);
                    pointTree.setProperty("voiced", point.voiced, nullptr);
                    pointTree.setProperty("manualTargetCents", point.manualTargetCents, nullptr);
                    pointTree.setProperty("hasManualTarget", point.hasManualTarget, nullptr);
                    noteTree.addChild(pointTree, -1, nullptr);
                }
                for (const auto& point : note.pitchControlPoints)
                {
                    juce::ValueTree pointTree("PitchControlPoint");
                    pointTree.setProperty("timeSeconds", point.timeSeconds, nullptr);
                    pointTree.setProperty("targetMidi", point.targetMidi, nullptr);
                    pointTree.setProperty("shape", pitchCurveShapeName(point.shape), nullptr);
                    pointTree.setProperty("bezierX1", point.bezierX1, nullptr);
                    pointTree.setProperty("bezierY1", point.bezierY1, nullptr);
                    pointTree.setProperty("bezierX2", point.bezierX2, nullptr);
                    pointTree.setProperty("bezierY2", point.bezierY2, nullptr);
                    noteTree.addChild(pointTree, -1, nullptr);
                }
                for (const auto& point : note.amplitudeEnvelope)
                {
                    juce::ValueTree pointTree("AmplitudeEnvelopePoint");
                    pointTree.setProperty("timeSeconds", point.timeSeconds, nullptr);
                    pointTree.setProperty("gainDb", point.gainDb, nullptr);
                    noteTree.addChild(pointTree, -1, nullptr);
                }
                for (const auto& curve : note.utauFlagCurves)
                for (const auto& point : curve.points)
                {
                    juce::ValueTree pointTree("FlagCurvePoint");
                    pointTree.setProperty("flag", curve.flag, nullptr);
                    pointTree.setProperty("timeSeconds", point.timeSeconds, nullptr);
                    pointTree.setProperty("value", point.value, nullptr);
                    pointTree.setProperty("shape", pitchCurveShapeName(point.shape),
                                          nullptr);
                    pointTree.setProperty("bezierX1", point.bezierX1, nullptr);
                    pointTree.setProperty("bezierY1", point.bezierY1, nullptr);
                    pointTree.setProperty("bezierX2", point.bezierX2, nullptr);
                    pointTree.setProperty("bezierY2", point.bezierY2, nullptr);
                    noteTree.addChild(pointTree, -1, nullptr);
                }
                for (const auto marker : note.sibilantMarkers)
                {
                    juce::ValueTree markerTree("Sibilant");
                    markerTree.setProperty("timeSeconds", marker, nullptr);
                    noteTree.addChild(markerTree, -1, nullptr);
                }
                clipTree.addChild(noteTree, -1, nullptr);
            }
            trackTree.addChild(clipTree, -1, nullptr);
        }
        root.addChild(trackTree, -1, nullptr);
    }
    return root;
}

ProjectData ProjectModel::fromValueTree(const juce::ValueTree& root,
                                        const juce::File& projectFile)
{
    ProjectData data;
    const auto projectDirectory = projectFile.getParentDirectory();
    std::map<juce::String, juce::File> recursiveMedia;
    auto indexedMedia = false;
    const auto resolveSource = [&](const juce::ValueTree& clipTree)
    {
        const auto storedPath = clipTree.getProperty("sourceFile").toString();
        juce::File source(storedPath);
        if (source.existsAsFile()) return source;
        const auto relative = clipTree.getProperty("sourceFileRelative").toString();
        if (relative.isNotEmpty())
        {
            const auto candidate = projectDirectory.getChildFile(relative);
            if (candidate.existsAsFile()) return candidate;
        }
        const auto fileName = source.getFileName();
        if (fileName.isNotEmpty())
        {
            const auto besideProject = projectDirectory.getChildFile(fileName);
            if (besideProject.existsAsFile()) return besideProject;
            if (!indexedMedia && projectDirectory.isDirectory())
            {
                indexedMedia = true;
                juce::Array<juce::File> files;
                projectDirectory.findChildFiles(files, juce::File::findFiles, true);
                for (const auto& file : files)
                    recursiveMedia.try_emplace(file.getFileName().toLowerCase(), file);
            }
            if (const auto found = recursiveMedia.find(fileName.toLowerCase());
                found != recursiveMedia.end()) return found->second;
        }
        return source;
    };
    data.name = root.getProperty("name", "Untitled").toString();
    data.bpm = static_cast<double>(root.getProperty("bpm", 120.0));
    data.beatOriginSeconds = static_cast<double>(root.getProperty("beatOriginSeconds", 0.0));
    data.numerator = static_cast<int>(root.getProperty("numerator", 4));
    data.denominator = static_cast<int>(root.getProperty("denominator", 4));
    data.gridDivision = root.getProperty("gridDivision", "1/16").toString();
    data.noteEditDivision = juce::jlimit(2, 128,
        static_cast<int>(root.getProperty("noteEditDivision", 64)));
    data.baseScale = root.getProperty("baseScale", "C").toString();

    for (const auto child : root)
        if (child.hasType("TempoChange"))
        {
            const auto position = std::max(0.0,
                static_cast<double>(child.getProperty("quarterPosition", 0.0)));
            const auto tempo = juce::jlimit(20.0, 400.0,
                static_cast<double>(child.getProperty("bpm", data.bpm)));
            if (position > 1.0e-7) data.tempoChanges.push_back({ position, tempo });
        }
    std::stable_sort(data.tempoChanges.begin(), data.tempoChanges.end(),
        [](const auto& left, const auto& right)
        {
            return left.quarterPosition < right.quarterPosition;
        });

    for (const auto trackTree : root)
    {
        if (!trackTree.hasType("Track")) continue;
        TrackData track;
        track.id = trackTree.getProperty("id").toString();
        track.name = trackTree.getProperty("name").toString();
        track.compose = static_cast<bool>(trackTree.getProperty("compose", true));
        track.muted = static_cast<bool>(trackTree.getProperty("muted", false));
        track.solo = static_cast<bool>(trackTree.getProperty("solo", false));
        track.referenceOnly =
            static_cast<bool>(trackTree.getProperty("referenceOnly", false));
        track.volume = static_cast<float>(trackTree.getProperty("volume", 1.0));
        track.pan = static_cast<float>(trackTree.getProperty("pan", 0.0));
        track.smoothOverlaps = static_cast<bool>(trackTree.getProperty("smoothOverlaps", false));
        track.normalizeVolume = static_cast<bool>(trackTree.getProperty("normalizeVolume", false));
        track.voicebankDirectory = juce::File(
            trackTree.getProperty("voicebankDirectory").toString());
        track.utauConsonantVelocity = static_cast<int>(
            trackTree.getProperty("utauConsonantVelocity", 100));
        track.utauGlobalFlags = trackTree.getProperty("utauGlobalFlags").toString();
        // A project written before 谋 carries only the flag.
        track.utauMode = trackTree.hasProperty("utauMode")
            ? parseUtauMode(trackTree.getProperty("utauMode", "").toString())
            : (static_cast<bool>(trackTree.getProperty("utauFourRegion", false))
                   ? UtauMode::jie : UtauMode::classic);
        if (!track.voicebankDirectory.isDirectory())
        {
            const auto relative = trackTree.getProperty("voicebankDirectoryRelative").toString();
            if (relative.isNotEmpty())
            {
                const auto candidate = projectDirectory.getChildFile(relative);
                if (candidate.isDirectory()) track.voicebankDirectory = candidate;
            }
        }
        track.pitchAlgorithm = parsePitchAlgorithm(trackTree.getProperty("pitchAlgorithm", "mld5").toString());
        track.stretchAlgorithm = parseStretchAlgorithm(trackTree.getProperty("stretchAlgorithm", "melodyne-hybrid").toString());
        track.renderOrder = parseRenderOrder(trackTree.getProperty("renderOrder", "process-then-splice").toString());

        for (const auto clipTree : trackTree)
        {
            if (!clipTree.hasType("Clip")) continue;
            ClipData clip;
            clip.id = clipTree.getProperty("id").toString();
            clip.sourceFile = resolveSource(clipTree);
            clip.startSeconds = static_cast<double>(clipTree.getProperty("startSeconds", 0.0));
            clip.sourceOffsetSeconds = static_cast<double>(clipTree.getProperty("sourceOffsetSeconds", 0.0));
            clip.sourceDurationSeconds = static_cast<double>(clipTree.getProperty("sourceDurationSeconds", 0.0));
            clip.durationSeconds = static_cast<double>(clipTree.getProperty("durationSeconds", 1.0));
            clip.fadeInSeconds = static_cast<double>(clipTree.getProperty("fadeInSeconds", 0.0));
            clip.fadeOutSeconds = static_cast<double>(clipTree.getProperty("fadeOutSeconds", 0.0));
            clip.crossfadeInSeconds = static_cast<double>(clipTree.getProperty("crossfadeInSeconds", 0.0));
            clip.crossfadeOutSeconds = static_cast<double>(clipTree.getProperty("crossfadeOutSeconds", 0.0));
            clip.gain = static_cast<float>(clipTree.getProperty("gain", 1.0));
            clip.muted = static_cast<bool>(clipTree.getProperty("muted", false));
            clip.glideConnectedToNext = static_cast<bool>(
                clipTree.getProperty("glideConnectedToNext", false));
            clip.glideConnectedFromPrevious = static_cast<bool>(
                clipTree.getProperty("glideConnectedFromPrevious", false));

            for (const auto noteTree : clipTree)
            {
                if (noteTree.hasType("SourceTimePoint"))
                {
                    clip.sourceTimeMap.push_back({
                        static_cast<double>(noteTree.getProperty("targetSeconds", 0.0)),
                        static_cast<double>(noteTree.getProperty("sourceSeconds", 0.0)) });
                    continue;
                }
                if (!noteTree.hasType("Note")) continue;
                NoteData note;
                note.id = noteTree.getProperty("id").toString();
                note.label = noteTree.getProperty("label").toString();
                note.utauFlags = noteTree.getProperty("utauFlags").toString();
                const auto storedVelocity = static_cast<int>(
                    noteTree.getProperty("utauConsonantVelocity", -1));
                note.utauJieSplitSet = static_cast<bool>(
                    noteTree.getProperty("utauJieSplitSet", false));
                note.vibratoEnabled = static_cast<bool>(
                    noteTree.getProperty("vibratoEnabled", false));
                note.vibratoLengthPercent = noteTree.getProperty("vibratoLengthPercent", 65.0);
                note.vibratoCycleMs = noteTree.getProperty("vibratoCycleMs", 180.0);
                note.vibratoDepthCents = noteTree.getProperty("vibratoDepthCents", 35.0);
                note.vibratoFadeInPercent = noteTree.getProperty("vibratoFadeInPercent", 20.0);
                note.vibratoFadeOutPercent = noteTree.getProperty("vibratoFadeOutPercent", 20.0);
                note.vibratoPhasePercent = noteTree.getProperty("vibratoPhasePercent", 0.0);
                note.vibratoOffsetPercent = noteTree.getProperty("vibratoOffsetPercent", 0.0);
                note.vibratoRealLine = static_cast<bool>(
                    noteTree.getProperty("vibratoRealLine", false));
                note.utauFlagSplit = static_cast<bool>(
                    noteTree.getProperty("utauFlagSplit", false));
                note.utauFlagCurveEnabled = static_cast<bool>(
                    noteTree.getProperty("utauFlagCurveEnabled", false));
                note.utauSplice = static_cast<bool>(
                    noteTree.getProperty("utauSplice", false));
                note.utauRegionFlags1 = noteTree.getProperty("utauRegionFlags1").toString();
                note.utauRegionFlags2 = noteTree.getProperty("utauRegionFlags2").toString();
                note.utauRegionFlags3 = noteTree.getProperty("utauRegionFlags3").toString();
                note.utauRegionFlags4 = noteTree.getProperty("utauRegionFlags4").toString();
                note.utauJieSplit1 = noteTree.getProperty("utauJieSplit1", 0.0);
                note.utauJieSplit2 = noteTree.getProperty("utauJieSplit2", 0.0);
                note.utauJieSplit3 = noteTree.getProperty("utauJieSplit3", 0.0);
                if (noteTree.hasProperty("utauConsonantVelocityInherited"))
                    note.utauConsonantVelocity = static_cast<bool>(noteTree.getProperty(
                        "utauConsonantVelocityInherited", false))
                            ? inheritedUtauConsonantVelocity : storedVelocity;
                else
                    // Before signed velocity support, every negative value was
                    // normalised to -1 and meant "use the track value".
                    note.utauConsonantVelocity = storedVelocity < 0
                        ? inheritedUtauConsonantVelocity : storedVelocity;
                note.utauPreutteranceOverrideEnabled = static_cast<bool>(
                    noteTree.getProperty("utauPreutteranceOverrideEnabled", false));
                note.utauPreutteranceSeconds = std::max(0.0, static_cast<double>(
                    noteTree.getProperty("utauPreutteranceSeconds", 0.0)));
                note.utauOverlapOverrideEnabled = static_cast<bool>(
                    noteTree.getProperty("utauOverlapOverrideEnabled", false));
                note.utauOverlapSeconds = static_cast<double>(
                    noteTree.getProperty("utauOverlapSeconds", 0.0));
                note.utauStpSeconds = static_cast<double>(
                    noteTree.getProperty("utauStpSeconds", 0.0));
                note.startSeconds = static_cast<double>(noteTree.getProperty("startSeconds", 0.0));
                note.durationSeconds = static_cast<double>(noteTree.getProperty("durationSeconds", 0.25));
                note.consonantSeconds = static_cast<double>(noteTree.getProperty("consonantSeconds", 0.04));
                note.midiNote = static_cast<float>(noteTree.getProperty("midiNote", 60.0));
                note.sourceMidiCenter = static_cast<float>(noteTree.getProperty("sourceMidiCenter", -1.0));
                note.modulation = static_cast<float>(noteTree.getProperty("modulation", 1.0));
                note.drift = static_cast<float>(noteTree.getProperty("drift", 1.0));
                note.tension = static_cast<float>(noteTree.getProperty("tension", 0.0));
                note.breath = static_cast<float>(noteTree.getProperty("breath", 0.0));
                note.formantSemitones = static_cast<float>(noteTree.getProperty("formantSemitones", 0.0));
                note.gain = static_cast<float>(noteTree.getProperty("gain", 1.0));
                note.attackSpeed = static_cast<float>(noteTree.getProperty("attackSpeed", 1.0));
                note.robustPitchCurve = static_cast<bool>(
                    noteTree.getProperty("robustPitchCurve", false));
                note.connectedToPrevious = static_cast<bool>(noteTree.getProperty("connectedToPrevious", false));
                note.connectedToNext = static_cast<bool>(noteTree.getProperty("connectedToNext", false));
                for (const auto child : noteTree)
                {
                    if (child.hasType("PitchPoint"))
                    {
                        const auto relative = static_cast<float>(child.getProperty("relativeCents", 0.0));
                        note.contour.push_back({ static_cast<double>(child.getProperty("timeSeconds", 0.0)),
                                                 relative,
                                                 static_cast<float>(child.getProperty("withoutVibratoCents", relative)),
                                                 static_cast<bool>(child.getProperty("voiced", true)),
                                                 static_cast<float>(child.getProperty("manualTargetCents", 0.0)),
                                                 static_cast<bool>(child.getProperty("hasManualTarget", false)) });
                    }
                    else if (child.hasType("PitchControlPoint"))
                        note.pitchControlPoints.push_back({
                            static_cast<double>(child.getProperty("timeSeconds", 0.0)),
                            static_cast<float>(child.getProperty("targetMidi", note.midiNote)),
                            parsePitchCurveShape(child.getProperty("shape", "natural").toString()),
                            static_cast<float>(child.getProperty("bezierX1", 0.33)),
                            static_cast<float>(child.getProperty("bezierY1", 0.0)),
                            static_cast<float>(child.getProperty("bezierX2", 0.67)),
                            static_cast<float>(child.getProperty("bezierY2", 1.0)) });
                    else if (child.hasType("AmplitudeEnvelopePoint"))
                        note.amplitudeEnvelope.push_back({
                            static_cast<double>(child.getProperty("timeSeconds", 0.0)),
                            juce::jlimit(-60.0f, 12.0f,
                                static_cast<float>(child.getProperty("gainDb", 0.0))) });
                    // "FlagCurveG" is how the g curve was written before any
                    // other flag could have one; it reads as the g curve.
                    else if (child.hasType("FlagCurvePoint")
                             || child.hasType("FlagCurveG"))
                    {
                        const auto flag = child.hasType("FlagCurveG")
                            ? juce::String("g")
                            : child.getProperty("flag", "g").toString();
                        const auto& kind = flagCurveKindFor(flag);
                        auto found = std::find_if(note.utauFlagCurves.begin(),
                            note.utauFlagCurves.end(),
                            [&flag](const auto& curve) { return curve.flag == flag; });
                        if (found == note.utauFlagCurves.end())
                        {
                            note.utauFlagCurves.push_back({ flag, {} });
                            found = std::prev(note.utauFlagCurves.end());
                        }
                        found->points.push_back({
                            static_cast<double>(child.getProperty("timeSeconds", 0.0)),
                            juce::jlimit(kind.minimum, kind.maximum,
                                static_cast<float>(child.getProperty("value", 0.0))),
                            parsePitchCurveShape(
                                child.getProperty("shape", "linear").toString()),
                            static_cast<float>(child.getProperty("bezierX1", 0.33)),
                            static_cast<float>(child.getProperty("bezierY1", 0.0)),
                            static_cast<float>(child.getProperty("bezierX2", 0.67)),
                            static_cast<float>(child.getProperty("bezierY2", 1.0)) });
                    }
                    else if (child.hasType("Sibilant"))
                        note.sibilantMarkers.push_back(static_cast<double>(child.getProperty("timeSeconds", 0.0)));
                }
                for (auto& curve : note.utauFlagCurves)
                    std::stable_sort(curve.points.begin(), curve.points.end(),
                        [](const auto& left, const auto& right)
                        {
                            return left.timeSeconds < right.timeSeconds;
                        });
                std::stable_sort(note.amplitudeEnvelope.begin(),
                                 note.amplitudeEnvelope.end(),
                    [](const auto& left, const auto& right)
                    {
                        return left.timeSeconds < right.timeSeconds;
                    });
                clip.notes.push_back(std::move(note));
            }
            std::stable_sort(clip.sourceTimeMap.begin(), clip.sourceTimeMap.end(),
                [](const auto& left, const auto& right)
                {
                    return left.targetSeconds < right.targetSeconds;
                });
            auto previousTarget = -1.0;
            auto previousSource = -1.0;
            std::erase_if(clip.sourceTimeMap, [&](auto& point)
            {
                point.targetSeconds = juce::jlimit(0.0, clip.durationSeconds,
                                                   point.targetSeconds);
                point.sourceSeconds = juce::jlimit(0.0,
                    clip.sourceDurationSeconds > 0.0 ? clip.sourceDurationSeconds
                                                     : clip.durationSeconds,
                    point.sourceSeconds);
                const auto invalid = !std::isfinite(point.targetSeconds)
                    || !std::isfinite(point.sourceSeconds)
                    || point.targetSeconds <= previousTarget + 1.0e-9
                    || point.sourceSeconds < previousSource - 1.0e-9;
                if (!invalid)
                {
                    previousTarget = point.targetSeconds;
                    previousSource = point.sourceSeconds;
                }
                return invalid;
            });
            if (clip.sourceTimeMap.size() < 2) clip.sourceTimeMap.clear();
            track.clips.push_back(std::move(clip));
        }
        data.tracks.push_back(std::move(track));
    }
    return data;
}

bool ProjectModel::save(const juce::File& file, juce::String& error) const
{
    error.clear();
    if (auto stream = file.createOutputStream())
    {
        stream->setPosition(0);
        stream->truncate();
        toValueTree(file).writeToStream(*stream);
        stream->flush();
        return true;
    }
    error = "Could not write " + file.getFullPathName();
    return false;
}

bool ProjectModel::load(const juce::File& file, juce::String& error)
{
    error.clear();
    // Read the file whole, then parse it out of memory.  ValueTree reads a
    // stream a field at a time and a FileInputStream turns every one of those
    // into its own read of the disk, which is why opening a project took
    // seconds and got worse the bigger it was: 970 KB was 2.8 s off the file
    // against 30 ms from a block the same file loaded into in under a
    // millisecond.  Writing never had this -- 26 ms for the same project --
    // so only this side needed it.
    juce::MemoryBlock bytes;
    if (file.loadFileAsData(bytes))
    {
        juce::MemoryInputStream stream(bytes, false);
        auto tree = juce::ValueTree::readFromStream(stream);
        if (tree.hasType("HachiShifterProject"))
        {
            auto loaded = fromValueTree(tree, file);
            juce::StringArray missing;
            for (const auto& track : loaded.tracks)
                for (const auto& clip : track.clips)
                    if (!clip.sourceFile.existsAsFile())
                        missing.addIfNotAlreadyThere(clip.sourceFile.getFullPathName());
            replace(std::move(loaded));
            if (!missing.isEmpty())
                error = missing.joinIntoString("\n");
            return true;
        }
    }
    error = "Invalid HachiShifter Next project: " + file.getFullPathName();
    return false;
}
}
