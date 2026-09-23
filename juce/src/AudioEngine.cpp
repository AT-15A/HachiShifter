#include "AudioEngine.h"
#include "StartupLog.h"
#include "backend/MelodyneProvider.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace hachi
{
namespace
{
double automaticUtauPitchTransitionInset(double leftDuration, double rightDuration)
{
    return std::max(0.002, std::min({ 0.020,
        std::max(0.01, leftDuration) * 0.20,
        std::max(0.01, rightDuration) * 0.20 }));
}

std::optional<std::pair<float, float>> contourAt(const NoteData& note, double localSeconds)
{
    if (note.contour.empty()) return std::pair { 0.0f, 0.0f };
    const auto right = std::lower_bound(note.contour.begin(), note.contour.end(), localSeconds,
        [](const PitchPoint& point, double time) { return point.timeSeconds < time; });
    const auto rightIndex = right == note.contour.end()
        ? note.contour.size() - 1 : static_cast<std::size_t>(std::distance(note.contour.begin(), right));
    const auto leftIndex = rightIndex > 0 && note.contour[rightIndex].timeSeconds > localSeconds
        ? rightIndex - 1 : rightIndex;
    const auto& left = note.contour[leftIndex];
    const auto& next = note.contour[rightIndex];
    if (!left.voiced || !next.voiced) return std::nullopt;
    const auto amount = next.timeSeconds > left.timeSeconds
        ? static_cast<float>(juce::jlimit(0.0, 1.0,
            (localSeconds - left.timeSeconds) / (next.timeSeconds - left.timeSeconds))) : 0.0f;
    const auto source = left.relativeCents
        + (next.relativeCents - left.relativeCents) * amount;
    const auto leftTarget = renderedPitchCents(note, left);
    const auto rightTarget = renderedPitchCents(note, next);
    return std::pair { source, leftTarget + (rightTarget - leftTarget) * amount };
}

float amplitudeGainAt(const std::vector<AmplitudeEnvelopePoint>& envelope,
                      double localSeconds, float basePercent = 100.0f)
{
    // The base value scales the whole envelope, and an empty envelope is an
    // implied flat 0 dB line that the base raises just the same.  Everything
    // is done on the linear gain so the -60 dB floor and +12 dB ceiling match
    // the drawn envelope's own clamps.
    const auto factor = juce::jlimit(0.0f, 200.0f, basePercent) / 100.0f;
    const auto apply = [factor](float linearGain)
    {
        if (factor <= 1.0e-6f) return 0.0f;
        const auto scaled = linearGain * factor;
        if (scaled <= 1.0e-4f) return 0.0f;
        return juce::jlimit(0.0f, 3.981072f, scaled); // +12 dB ceiling.
    };
    if (envelope.empty()) return apply(1.0f);
    const auto right = std::lower_bound(envelope.begin(), envelope.end(), localSeconds,
        [](const auto& point, double time) { return point.timeSeconds < time; });
    if (right == envelope.begin())
        return apply(std::pow(10.0f, right->gainDb / 20.0f));
    if (right == envelope.end())
        return apply(std::pow(10.0f, envelope.back().gainDb / 20.0f));
    const auto& left = *(right - 1);
    const auto span = right->timeSeconds - left.timeSeconds;
    const auto amount = span > 1.0e-9
        ? static_cast<float>(juce::jlimit(0.0, 1.0,
            (localSeconds - left.timeSeconds) / span)) : 0.0f;
    const auto db = left.gainDb + (right->gainDb - left.gainDb) * amount;
    return apply(std::pow(10.0f, db / 20.0f));
}

std::pair<float, float> panGains(float pan, bool mono)
{
    pan = juce::jlimit(-1.0f, 1.0f, pan);
    if (mono)
        return { std::sqrt(0.5f * (1.0f - pan)),
                 std::sqrt(0.5f * (1.0f + pan)) };
    // Stereo tracks use a balance law: centre must preserve both source
    // channels at unity instead of applying an unintended -3 dB attenuation.
    return { pan > 0.0f ? std::sqrt(1.0f - pan) : 1.0f,
             pan < 0.0f ? std::sqrt(1.0f + pan) : 1.0f };
}

// joinedStart / joinedEnd say that the clip meets its neighbour there with a
// Melodyne pitch join; see AudioEngine::clipsJoinAt.
backend::Mld5FileRenderRequest makeRenderRequest(const ClipData& clip, const TrackData& track,
                                                  const juce::File& hifiganModelDirectory,
                                                  const backend::OrtExecutionConfig& inference,
                                                  bool joinedStart = false,
                                                  bool joinedEnd = false)
{
    backend::Mld5FileRenderRequest request;
    request.sourceFile = clip.sourceFile;
    request.sourceOffsetSeconds = clip.sourceOffsetSeconds;
    request.sourceDurationSeconds = clip.sourceDurationSeconds > 1.0e-9
        ? clip.sourceDurationSeconds : clip.durationSeconds;
    request.targetDurationSeconds = clip.durationSeconds;
    request.hifiganModelDirectory = hifiganModelDirectory;
    request.inference = inference;
    // The neural decoder fades each edge over 3 ms so that a clip boundary
    // whose phase is unrelated to the next one does not click.  A joined seam
    // is not one of those -- the mixer crossfades it -- and baking a 3 ms fade
    // into both sides of every join dips the level at each of them.  A bare
    // de-click is enough there.
    if (track.pitchAlgorithm == PitchAlgorithm::nsfHifigan)
    {
        if (joinedStart) request.neuralGuardStartSeconds = 0.0005f;
        if (joinedEnd) request.neuralGuardEndSeconds = 0.0005f;
    }
    switch (track.pitchAlgorithm)
    {
        case PitchAlgorithm::nsfHifigan:
            request.pitchBackend = backend::PitchRenderBackend::nsfHifigan;
            break;
        case PitchAlgorithm::mld3:
            request.pitchBackend = backend::PitchRenderBackend::mld3;
            break;
        case PitchAlgorithm::world:
            request.pitchBackend = backend::PitchRenderBackend::world;
            break;
        case PitchAlgorithm::vocalShifter:
            request.pitchBackend = backend::PitchRenderBackend::vslib;
            break;
        case PitchAlgorithm::llsm2:
            request.pitchBackend = backend::PitchRenderBackend::llsm2;
            break;
        case PitchAlgorithm::utau:
        case PitchAlgorithm::mld5:
        default:
            request.pitchBackend = backend::PitchRenderBackend::mld5;
            break;
    }
    request.stretchAlgorithm = static_cast<int>(track.stretchAlgorithm);
    request.normalizeVolume = track.normalizeVolume;
    // Decoding a clip on its own can leave its level below the source's; the
    // phrase-at-a-time order does not have that problem, because the model sees
    // the whole phrase.  The floor was raised in the per-clip order to
    // compensate -- but a splice-first reference never raises it, so per-clip
    // flooring is exactly what pulled a process-then-splice render away from a
    // splice-first bounce (each of many short clips floored independently).
    // Match the reference's behaviour: leave the neural level untouched here.
    request.matchNsfSourceLevel = false;
    const auto sourceDuration = request.sourceDurationSeconds;
    std::vector<backend::TimeMapPoint> timeAnchors;
    if (!clip.sourceTimeMap.empty())
    {
        timeAnchors.reserve(clip.sourceTimeMap.size());
        for (const auto& point : clip.sourceTimeMap)
            timeAnchors.push_back({ juce::jlimit(0.0, clip.durationSeconds, point.targetSeconds),
                                    juce::jlimit(0.0, sourceDuration, point.sourceSeconds) });
    }
    else
    {
        timeAnchors.push_back({ 0.0, 0.0 });
        for (const auto& note : clip.notes)
        {
            const auto noteTargetStart = juce::jlimit(0.0, clip.durationSeconds, note.startSeconds);
            const auto sourceStart = clip.durationSeconds > 1.0e-9
                ? noteTargetStart / clip.durationSeconds * sourceDuration : 0.0;
            timeAnchors.push_back({ noteTargetStart, sourceStart });
            if (note.consonantSeconds <= 1.0e-6 || note.attackSpeed <= 1.0e-6f) continue;
            const auto targetAttack = juce::jlimit(noteTargetStart, clip.durationSeconds,
                noteTargetStart + note.consonantSeconds);
            const auto sourceAttack = juce::jlimit(sourceStart, sourceDuration,
                sourceStart + note.consonantSeconds * static_cast<double>(note.attackSpeed));
            timeAnchors.push_back({ targetAttack, sourceAttack });
        }
    }
    timeAnchors.push_back({ clip.durationSeconds, sourceDuration });
    std::stable_sort(timeAnchors.begin(), timeAnchors.end(), [](const auto& left, const auto& right)
    {
        if (std::abs(left.targetSeconds - right.targetSeconds) > 1.0e-9)
            return left.targetSeconds < right.targetSeconds;
        return left.sourceSeconds < right.sourceSeconds;
    });
    for (const auto& anchor : timeAnchors)
    {
        if (request.timeMap.empty())
        {
            request.timeMap.push_back(anchor);
            continue;
        }
        auto& previous = request.timeMap.back();
        if (std::abs(anchor.targetSeconds - previous.targetSeconds) <= 1.0e-7)
        {
            previous.sourceSeconds = std::max(previous.sourceSeconds, anchor.sourceSeconds);
            continue;
        }
        if (anchor.sourceSeconds > previous.sourceSeconds + 1.0e-7)
            request.timeMap.push_back(anchor);
    }
    if (request.timeMap.empty() || request.timeMap.back().targetSeconds < clip.durationSeconds - 1.0e-7)
        request.timeMap.push_back({ clip.durationSeconds, sourceDuration });
    constexpr auto framePeriodSeconds = 0.005;
    request.framePeriodMs = framePeriodSeconds * 1000.0;
    const auto frameCount = std::max(2, static_cast<int>(std::ceil(clip.durationSeconds
                                                                   / framePeriodSeconds)) + 1);
    request.sourceMidi.resize(static_cast<std::size_t>(frameCount), 0.0f);
    request.targetMidi.resize(static_cast<std::size_t>(frameCount), 0.0f);
    request.formantSemitones.resize(static_cast<std::size_t>(frameCount), 0.0f);
    request.noteGain.resize(static_cast<std::size_t>(frameCount), 1.0f);
    request.tension.resize(static_cast<std::size_t>(frameCount), 0.0f);
    request.breath.resize(static_cast<std::size_t>(frameCount), 0.0f);
    request.robustPitchCurve.resize(static_cast<std::size_t>(frameCount), 0.0f);
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const auto time = std::min(clip.durationSeconds, static_cast<double>(frame) * framePeriodSeconds);
        for (std::size_t noteIndex = 0; noteIndex < clip.notes.size(); ++noteIndex)
        {
            const auto& note = clip.notes[noteIndex];
            const auto local = time - note.startSeconds;
            if (local < -1.0e-9 || local > note.durationSeconds + 1.0e-9) continue;
            request.formantSemitones[static_cast<std::size_t>(frame)] = note.formantSemitones;
            const auto clampedLocal = juce::jlimit(0.0, note.durationSeconds, local);
            request.noteGain[static_cast<std::size_t>(frame)] = note.gain
                * amplitudeGainAt(note.amplitudeEnvelope, clampedLocal, note.amplitudeEnvelopeBasePercent);
            request.tension[static_cast<std::size_t>(frame)] = note.tension;
            request.breath[static_cast<std::size_t>(frame)] = note.breath;
            // Keep adjacent robust notes as separate detector regions.  A
            // boolean mask alone lets a slope limiter bridge two valid notes
            // at a hard musical interval and mistakes it for an F0 outlier.
            // Zero remains disabled; positive values identify the owning note.
            request.robustPitchCurve[static_cast<std::size_t>(frame)] =
                note.robustPitchCurve ? static_cast<float>(noteIndex + 1) : 0.0f;
            const auto cents = contourAt(note, clampedLocal);
            if (!cents) break; // Preserve the analysed unvoiced mask.
            const auto sourceCenter = note.sourceMidiCenter >= 0.0f ? note.sourceMidiCenter : note.midiNote;
            request.sourceMidi[static_cast<std::size_t>(frame)] = sourceCenter + cents->first / 100.0f;
            // Vibrato is a target-pitch edit, not a display-only decoration.
            // Keep it in the common request so NSF-HiFiGAN and every model-free
            // native backend hear exactly what the piano roll draws.
            request.targetMidi[static_cast<std::size_t>(frame)] = note.midiNote
                + (cents->second + vibratoCentsAt(note, clampedLocal)) / 100.0f;
            break;
        }
    }

    for (const auto& joinedNote : clip.notes)
    {
        if (!joinedNote.connectedToPrevious) continue;
        const NoteData* previousNote = nullptr;
        auto previousEnd = -std::numeric_limits<double>::infinity();
        const auto joinedStart = clip.startSeconds + joinedNote.startSeconds;
        for (const auto& candidateClip : track.clips)
            for (const auto& candidate : candidateClip.notes)
            {
                const auto end = candidateClip.startSeconds + candidate.startSeconds
                    + candidate.durationSeconds;
                if (end <= joinedStart + 0.002
                    && end > previousEnd && candidate.id != joinedNote.id)
                {
                    previousEnd = end;
                    previousNote = &candidate;
                }
            }
        if (previousNote != nullptr)
        {
            const auto previousCents = contourAt(*previousNote, previousNote->durationSeconds);
            const auto previousPitch = previousNote->midiNote + (previousCents
                ? (previousCents->second
                    + vibratoCentsAt(*previousNote, previousNote->durationSeconds)) / 100.0f
                : 0.0f);
            const auto joinSeconds = std::min(0.08,
                std::max(0.012, joinedNote.durationSeconds * 0.22));
            const auto firstFrame = juce::jlimit(0, frameCount - 1,
                static_cast<int>(std::llround(joinedNote.startSeconds / framePeriodSeconds)));
            const auto joinFrames = std::min(frameCount - firstFrame,
                std::max(2, static_cast<int>(std::ceil(joinSeconds / framePeriodSeconds))));
            if (joinFrames < 2) continue;
            for (int frame = 0; frame < joinFrames; ++frame)
            {
                auto& target = request.targetMidi[static_cast<std::size_t>(firstFrame + frame)];
                if (!(target > 0.0f)) continue;
                const auto x = static_cast<float>(frame) / static_cast<float>(joinFrames - 1);
                const auto smooth = x * x * (3.0f - 2.0f * x);
                target = previousPitch + (target - previousPitch) * smooth;
            }
        }
    }

    // Native timeline-pitch continuity: where two notes abut on the timeline
    // (not an explicit glide, just neighbours), the per-frame target otherwise
    // steps from the first note's tail pitch to the next note's head pitch in a
    // single frame -- an audible seam in the model backends.  Lay the same short
    // automatic S-transition the UTAU path uses (smoothstep across a small inset
    // either side of the boundary) directly into the target-MIDI line, so the
    // one native pitch line the models read is continuous across the seam.  Only
    // between two voiced sides, so an analysed unvoiced gap is never bridged.
    {
        std::vector<std::size_t> order(clip.notes.size());
        std::iota(order.begin(), order.end(), std::size_t { 0 });
        std::stable_sort(order.begin(), order.end(), [&](auto left, auto right)
        {
            return clip.notes[left].startSeconds < clip.notes[right].startSeconds;
        });
        const auto targetAt = [&](double seconds) -> float
        {
            const auto frame = static_cast<int>(std::llround(seconds / framePeriodSeconds));
            if (frame < 0 || frame >= frameCount) return 0.0f;
            return request.targetMidi[static_cast<std::size_t>(frame)];
        };
        for (std::size_t position = 1; position < order.size(); ++position)
        {
            const auto& previous = clip.notes[order[position - 1]];
            const auto& next = clip.notes[order[position]];
            const auto previousEnd = previous.startSeconds + previous.durationSeconds;
            if (std::abs(previousEnd - next.startSeconds) > 0.002) continue;
            // An explicit connection already glided this seam above.
            if (next.connectedToPrevious) continue;
            const auto inset = automaticUtauPitchTransitionInset(
                previous.durationSeconds, next.durationSeconds);
            const auto startSeconds = previousEnd - inset;
            const auto endSeconds = next.startSeconds + inset;
            const auto startPitch = targetAt(startSeconds - framePeriodSeconds * 0.0);
            const auto endPitch = targetAt(endSeconds);
            // Both sides must be voiced (a real target); 0 marks unvoiced.
            if (!(startPitch > 0.0f) || !(endPitch > 0.0f)) continue;
            const auto firstFrame = juce::jlimit(0, frameCount - 1,
                static_cast<int>(std::llround(startSeconds / framePeriodSeconds)));
            const auto lastFrame = juce::jlimit(0, frameCount - 1,
                static_cast<int>(std::llround(endSeconds / framePeriodSeconds)));
            if (lastFrame - firstFrame < 2) continue;
            for (int frame = firstFrame; frame <= lastFrame; ++frame)
            {
                auto& target = request.targetMidi[static_cast<std::size_t>(frame)];
                if (!(target > 0.0f)) continue; // never fabricate over unvoiced
                const auto x = static_cast<float>(frame - firstFrame)
                    / static_cast<float>(lastFrame - firstFrame);
                const auto smooth = x * x * (3.0f - 2.0f * x);
                target = startPitch + (endPitch - startPitch) * smooth;
            }
        }
    }
    return request;
}

// One request covering a whole glide chain: the clips' time maps stitched
// end to end into one map, and one pitch line with every seam glided so the
// single decode never meets a hard F0 step inside the phrase.
}

backend::Mld5FileRenderRequest AudioEngine::mergedRequestFor(
    const std::vector<const ClipData*>& group, const TrackData& track,
    const juce::File& hifiganModelDirectory,
    const backend::OrtExecutionConfig& inference)
{
    backend::Mld5FileRenderRequest request;
    request.sourceFile = group.front()->sourceFile;
    auto sourceStart = std::numeric_limits<double>::max();
    auto sourceEnd = 0.0;
    std::vector<double> targetOffsets;
    targetOffsets.reserve(group.size());
    auto targetDuration = 0.0;
    for (const auto* clip : group)
    {
        targetOffsets.push_back(targetDuration);
        targetDuration += clip->durationSeconds;
        sourceStart = std::min(sourceStart, clip->sourceOffsetSeconds);
        sourceEnd = std::max(sourceEnd, clip->sourceOffsetSeconds + clip->sourceDurationSeconds);
    }
    request.sourceOffsetSeconds = sourceStart;
    request.sourceDurationSeconds = std::max(1.0e-6, sourceEnd - sourceStart);
    request.targetDurationSeconds = targetDuration;
    request.hifiganModelDirectory = hifiganModelDirectory;
    request.inference = inference;
    request.pitchBackend = backend::PitchRenderBackend::nsfHifigan;
    request.stretchAlgorithm = static_cast<int>(track.stretchAlgorithm);
    request.normalizeVolume = track.normalizeVolume;
    request.isGlideMerged = true;
    // The variable-hop paths read the two sides of a source discontinuity in
    // order, choosing the old source before a seam and the new one after it,
    // so their anchors must not be sorted together across the seam.
    const auto preserveSourceSeams = track.stretchAlgorithm == StretchAlgorithm::variableMelHop
        || track.stretchAlgorithm == StretchAlgorithm::nsfShiftThenSplice;

    for (std::size_t index = 0; index < group.size(); ++index)
    {
        const auto& clip = *group[index];
        const auto targetOffset = targetOffsets[index];
        const auto sourceOffset = clip.sourceOffsetSeconds - sourceStart;
        std::vector<backend::TimeMapPoint> localAnchors;
        if (!clip.sourceTimeMap.empty())
        {
            for (const auto& point : clip.sourceTimeMap)
                localAnchors.push_back({
                    juce::jlimit(0.0, clip.durationSeconds, point.targetSeconds),
                    juce::jlimit(0.0, clip.sourceDurationSeconds, point.sourceSeconds) });
        }
        else
        {
            for (const auto& note : clip.notes)
            {
                const auto noteStart = juce::jlimit(0.0, clip.durationSeconds, note.startSeconds);
                const auto srcStart = clip.durationSeconds > 1.0e-9
                    ? noteStart / clip.durationSeconds * clip.sourceDurationSeconds : 0.0;
                localAnchors.push_back({ noteStart, srcStart });
                if (note.consonantSeconds <= 1.0e-6 || note.attackSpeed <= 1.0e-6f) continue;
                const auto targetAttack = juce::jlimit(noteStart, clip.durationSeconds,
                    noteStart + note.consonantSeconds);
                const auto sourceAttack = juce::jlimit(srcStart, clip.sourceDurationSeconds,
                    srcStart + note.consonantSeconds * static_cast<double>(note.attackSpeed));
                localAnchors.push_back({ targetAttack, sourceAttack });
            }
        }
        localAnchors.push_back({ 0.0, 0.0 });
        localAnchors.push_back({ clip.durationSeconds, clip.sourceDurationSeconds });
        std::stable_sort(localAnchors.begin(), localAnchors.end(),
                         [](const auto& left, const auto& right)
        {
            if (std::abs(left.targetSeconds - right.targetSeconds) > 1.0e-9)
                return left.targetSeconds < right.targetSeconds;
            return left.sourceSeconds < right.sourceSeconds;
        });
        std::vector<backend::TimeMapPoint> localMap;
        for (const auto& anchor : localAnchors)
        {
            if (localMap.empty())
            {
                localMap.push_back(anchor);
                continue;
            }
            auto& previous = localMap.back();
            if (std::abs(anchor.targetSeconds - previous.targetSeconds) <= 1.0e-7)
            {
                previous.sourceSeconds = std::max(previous.sourceSeconds, anchor.sourceSeconds);
                continue;
            }
            if (anchor.sourceSeconds > previous.sourceSeconds + 1.0e-7)
                localMap.push_back(anchor);
        }
        for (const auto& anchor : localMap)
        {
            const backend::TimeMapPoint mapped {
                targetOffset + anchor.targetSeconds,
                sourceOffset + anchor.sourceSeconds
            };
            if (!request.timeMap.empty()
                && std::abs(mapped.targetSeconds - request.timeMap.back().targetSeconds) <= 1.0e-7
                && std::abs(mapped.sourceSeconds - request.timeMap.back().sourceSeconds) <= 1.0e-7)
                continue;
            request.timeMap.push_back(mapped);
        }
    }
    if (!preserveSourceSeams)
    {
        auto anchors = std::move(request.timeMap);
        request.timeMap.clear();
        std::stable_sort(anchors.begin(), anchors.end(), [](const auto& left, const auto& right)
        {
            if (std::abs(left.targetSeconds - right.targetSeconds) > 1.0e-9)
                return left.targetSeconds < right.targetSeconds;
            return left.sourceSeconds < right.sourceSeconds;
        });
        for (const auto& anchor : anchors)
        {
            if (request.timeMap.empty())
            {
                request.timeMap.push_back(anchor);
                continue;
            }
            auto& previous = request.timeMap.back();
            if (std::abs(anchor.targetSeconds - previous.targetSeconds) <= 1.0e-7)
            {
                previous.sourceSeconds = std::max(previous.sourceSeconds, anchor.sourceSeconds);
                continue;
            }
            if (anchor.sourceSeconds > previous.sourceSeconds + 1.0e-7)
                request.timeMap.push_back(anchor);
        }
    }
    if (request.timeMap.empty() || request.timeMap.back().targetSeconds < targetDuration - 1.0e-7)
        request.timeMap.push_back({ targetDuration, sourceEnd - sourceStart });

    constexpr auto framePeriodSeconds = 0.005;
    request.framePeriodMs = framePeriodSeconds * 1000.0;
    const auto frameCount = std::max(2, static_cast<int>(
        std::ceil(targetDuration / framePeriodSeconds)) + 1);
    request.sourceMidi.assign(static_cast<std::size_t>(frameCount), 0.0f);
    request.targetMidi.assign(static_cast<std::size_t>(frameCount), 0.0f);
    request.formantSemitones.assign(static_cast<std::size_t>(frameCount), 0.0f);
    request.noteGain.assign(static_cast<std::size_t>(frameCount), 1.0f);
    request.tension.assign(static_cast<std::size_t>(frameCount), 0.0f);
    request.breath.assign(static_cast<std::size_t>(frameCount), 0.0f);
    request.robustPitchCurve.assign(static_cast<std::size_t>(frameCount), 0.0f);
    auto lastSourceMidi = 0.0f;
    auto lastTargetMidi = 0.0f;
    for (int frame = 0; frame < frameCount; ++frame)
    {
        const auto time = std::min(targetDuration, static_cast<double>(frame) * framePeriodSeconds);
        std::size_t index = 0;
        while (index + 1 < group.size() && targetOffsets[index + 1] <= time) ++index;
        const auto& clip = *group[index];
        const auto local = time - targetOffsets[index];
        for (const auto& note : clip.notes)
        {
            const auto noteLocal = local - note.startSeconds;
            if (noteLocal < -1.0e-9 || noteLocal > note.durationSeconds + 1.0e-9) continue;
            request.formantSemitones[static_cast<std::size_t>(frame)] = note.formantSemitones;
            const auto clampedLocal = juce::jlimit(0.0, note.durationSeconds, noteLocal);
            request.noteGain[static_cast<std::size_t>(frame)] = note.gain
                * amplitudeGainAt(note.amplitudeEnvelope, clampedLocal, note.amplitudeEnvelopeBasePercent);
            request.tension[static_cast<std::size_t>(frame)] = note.tension;
            request.breath[static_cast<std::size_t>(frame)] = note.breath;
            request.robustPitchCurve[static_cast<std::size_t>(frame)] =
                note.robustPitchCurve ? static_cast<float>(index + 1) : 0.0f;
            const auto cents = contourAt(note, clampedLocal);
            if (!cents)
            {
                // Unvoiced: carry the last voiced pitch forward so the model
                // still has a carrier reference under the consonant audio.
                if (lastSourceMidi > 0.0f)
                {
                    request.sourceMidi[static_cast<std::size_t>(frame)] = lastSourceMidi;
                    request.targetMidi[static_cast<std::size_t>(frame)] = lastTargetMidi;
                }
                continue;
            }
            const auto sourceCenter = note.sourceMidiCenter >= 0.0f
                ? note.sourceMidiCenter : note.midiNote;
            lastSourceMidi = sourceCenter + cents->first / 100.0f;
            lastTargetMidi = note.midiNote
                + (cents->second + vibratoCentsAt(note, clampedLocal)) / 100.0f;
            request.sourceMidi[static_cast<std::size_t>(frame)] = lastSourceMidi;
            request.targetMidi[static_cast<std::size_t>(frame)] = lastTargetMidi;
            break;
        }
    }
    // Glide each seam out of the previous clip's tail pitch, so the one decode
    // never sees the step that splicing two independent renders would leave.
    for (std::size_t index = 1; index < group.size(); ++index)
    {
        const auto& previousClip = *group[index - 1];
        const auto& nextClip = *group[index];
        auto previousPitch = previousClip.notes.empty()
            ? 0.0 : static_cast<double>(previousClip.notes.back().midiNote);
        if (!previousClip.notes.empty())
        {
            const auto& note = previousClip.notes.back();
            const auto cents = contourAt(note, note.durationSeconds);
            previousPitch = note.midiNote + (cents
                ? (cents->second + vibratoCentsAt(note, note.durationSeconds)) / 100.0f
                : 0.0f);
        }
        const auto joinSeconds = std::min(0.08, std::max(0.012, nextClip.durationSeconds * 0.22));
        const auto firstFrame = juce::jlimit(0, frameCount - 1,
            static_cast<int>(std::llround(targetOffsets[index] / framePeriodSeconds)));
        const auto joinFrames = std::min(frameCount - firstFrame,
            std::max(2, static_cast<int>(std::ceil(joinSeconds / framePeriodSeconds))));
        if (joinFrames < 2) continue;
        for (int frame = 0; frame < joinFrames; ++frame)
        {
            auto& target = request.targetMidi[static_cast<std::size_t>(firstFrame + frame)];
            if (!(target > 0.0f)) continue;
            const auto x = static_cast<float>(frame) / static_cast<float>(joinFrames - 1);
            const auto smooth = x * x * (3.0f - 2.0f * x);
            target = static_cast<float>(previousPitch + (target - previousPitch) * smooth);
        }
    }
    return request;
}

namespace
{
// Everything about one note that decides how it is rendered.  Written by
// the cache key, and hashed by the waveform display to ask whether the note
// under a drawn waveform is still the note that produced it -- one list, so
// the two can never disagree about what counts as an edit.
void writeNoteRenderFields(juce::MemoryOutputStream& stream, const NoteData& note)
{
    const auto label = note.label.toUTF8();
    stream.write(label.getAddress(), label.sizeInBytes());
    const auto flags = note.utauFlags.toUTF8();
    stream.write(flags.getAddress(), flags.sizeInBytes());
    stream.writeInt(note.utauConsonantVelocity);
    stream.writeBool(note.utauPreutteranceOverrideEnabled);
    stream.writeDouble(note.utauPreutteranceSeconds);
    stream.writeBool(note.utauOverlapOverrideEnabled);
    stream.writeDouble(note.utauOverlapSeconds);
    stream.writeDouble(note.utauStpSeconds);
    stream.writeBool(note.vibratoEnabled);
    stream.writeDouble(note.vibratoLengthPercent);
    stream.writeDouble(note.vibratoCycleMs);
    stream.writeDouble(note.vibratoDepthCents);
    stream.writeDouble(note.vibratoFadeInPercent);
    stream.writeDouble(note.vibratoFadeOutPercent);
    stream.writeDouble(note.vibratoPhasePercent);
    stream.writeDouble(note.vibratoOffsetPercent);
    stream.writeBool(note.utauFlagSplit);
    stream.writeBool(note.utauFlagCurveEnabled);
    for (const auto& curve : note.utauFlagCurves)
    {
        stream.writeString(curve.flag);
        for (const auto& point : curve.points)
        {
            stream.writeDouble(point.timeSeconds);
            stream.writeDouble(point.value);
            stream.writeInt(static_cast<int>(point.shape));
            for (const auto handle : { point.bezierX1, point.bezierY1,
                                       point.bezierX2, point.bezierY2 })
                stream.writeFloat(handle);
        }
    }
    stream.writeBool(note.utauSplice);
    for (const auto& text : { note.utauRegionFlags1, note.utauRegionFlags2,
                              note.utauRegionFlags3, note.utauRegionFlags4 })
    {
        const auto raw = text.toUTF8();
        stream.write(raw.getAddress(), raw.sizeInBytes());
    }
    stream.writeBool(note.utauJieSplitSet);
    stream.writeDouble(note.utauJieSplit1);
    stream.writeDouble(note.utauJieSplit2);
    stream.writeDouble(note.utauJieSplit3);
    stream.writeDouble(note.startSeconds);
    stream.writeDouble(note.durationSeconds);
    stream.writeFloat(note.midiNote);
    stream.writeFloat(note.sourceMidiCenter);
    stream.writeDouble(note.consonantSeconds);
    stream.writeFloat(note.attackSpeed);
    stream.writeByte(static_cast<char>(note.robustPitchCurve ? 1 : 0));
    stream.writeByte(static_cast<char>(note.connectedToPrevious ? 1 : 0));
    stream.writeByte(static_cast<char>(note.connectedToNext ? 1 : 0));
    stream.writeFloat(note.modulation);
    stream.writeFloat(note.drift);
    stream.writeFloat(note.tension);
    stream.writeFloat(note.breath);
    stream.writeFloat(note.formantSemitones);
    stream.writeFloat(note.gain);
    stream.writeInt64(static_cast<juce::int64>(note.amplitudeEnvelope.size()));
    for (const auto& point : note.amplitudeEnvelope)
    {
        stream.writeDouble(point.timeSeconds);
        stream.writeFloat(point.gainDb);
    }
    for (const auto& point : note.contour)
    {
        stream.writeDouble(point.timeSeconds);
        stream.writeFloat(point.relativeCents);
        stream.writeFloat(point.withoutVibratoCents);
        stream.writeByte(static_cast<char>(point.voiced ? 1 : 0));
        stream.writeFloat(point.manualTargetCents);
        stream.writeByte(static_cast<char>(point.hasManualTarget ? 1 : 0));
    }
    stream.writeInt64(static_cast<juce::int64>(note.pitchControlPoints.size()));
    for (const auto& point : note.pitchControlPoints)
    {
        stream.writeDouble(point.timeSeconds);
        stream.writeFloat(point.targetMidi);
        stream.writeInt(static_cast<int>(point.shape));
        stream.writeFloat(point.bezierX1);
        stream.writeFloat(point.bezierY1);
        stream.writeFloat(point.bezierX2);
        stream.writeFloat(point.bezierY2);
    }
}

// The note as the cache key sees it, in one number.  Two notes with the same
// value render the same audio, which is what lets a drawn waveform be checked
// against the note still under it.
std::uint64_t noteRenderHash(const NoteData& note)
{
    return AudioEngine::utauNoteRenderHash(note);
}

std::uint64_t noteRenderHashImpl(const NoteData& note)
{
    juce::MemoryOutputStream stream;
    writeNoteRenderFields(stream, note);
    // FNV-1a: no dependency, and collisions here cost a waveform that is drawn
    // when it should not be, not audio that is wrong.
    std::uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const unsigned char*>(stream.getData());
    for (std::size_t index = 0; index < stream.getDataSize(); ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string renderKey(const ClipData& clip, const TrackData& track,
                      const juce::File& hifiganModelDirectory,
                      const backend::OrtExecutionConfig& inference,
                      const juce::File& utauResamplerFile)
{
    juce::MemoryOutputStream stream;
    // The render order reaches the per-clip render through matchNsfSourceLevel,
    // so two orders are two different buffers and must not share a cache entry.
    stream.writeInt(static_cast<int>(track.renderOrder));
    const auto path = clip.sourceFile.getFullPathName().toUTF8();
    stream.write(path.getAddress(), path.sizeInBytes());
    stream.writeInt64(clip.sourceFile.getLastModificationTime().toMilliseconds());
    stream.writeDouble(clip.sourceOffsetSeconds);
    stream.writeDouble(clip.sourceDurationSeconds);
    stream.writeDouble(clip.durationSeconds);
    stream.writeInt64(static_cast<juce::int64>(clip.sourceTimeMap.size()));
    for (const auto& point : clip.sourceTimeMap)
    {
        stream.writeDouble(point.targetSeconds);
        stream.writeDouble(point.sourceSeconds);
    }
    stream.writeInt(static_cast<int>(track.pitchAlgorithm));
    stream.writeInt(static_cast<int>(track.stretchAlgorithm));
    stream.writeBool(track.normalizeVolume);
    stream.writeBool(utauModeUsesRegions(track.utauMode));
    stream.writeInt(track.utauConsonantVelocity);
    const auto globalFlags = track.utauGlobalFlags.toUTF8();
    stream.write(globalFlags.getAddress(), globalFlags.sizeInBytes());
    const auto voicebankPath = track.voicebankDirectory.getFullPathName().toUTF8();
    stream.write(voicebankPath.getAddress(), voicebankPath.sizeInBytes());
    stream.writeInt64(track.voicebankDirectory.getLastModificationTime().toMilliseconds());
    if (track.pitchAlgorithm == PitchAlgorithm::utau
        && track.voicebankDirectory.isDirectory())
    {
        juce::Array<juce::File> otoFiles;
        track.voicebankDirectory.findChildFiles(
            otoFiles, juce::File::findFiles, true, "*");
        otoFiles.removeIf([](const juce::File& file)
        {
            const auto name = file.getFileName();
            return !name.equalsIgnoreCase("oto.ini")
                && !name.equalsIgnoreCase("oto.jie.ini")
                && !name.equalsIgnoreCase("oto4.ini");
        });
        otoFiles.sort();
        for (const auto& file : otoFiles)
        {
            const auto relative = file.getRelativePathFrom(
                track.voicebankDirectory).toUTF8();
            stream.write(relative.getAddress(), relative.sizeInBytes());
            stream.writeInt64(file.getLastModificationTime().toMilliseconds());
            stream.writeInt64(file.getSize());
        }
    }
    const auto resamplerPath = utauResamplerFile.getFullPathName().toUTF8();
    stream.write(resamplerPath.getAddress(), resamplerPath.sizeInBytes());
    stream.writeInt64(utauResamplerFile.getLastModificationTime().toMilliseconds());
    const auto modelPath = hifiganModelDirectory.getFullPathName().toUTF8();
    stream.write(modelPath.getAddress(), modelPath.sizeInBytes());
    const auto modelDirectory = hifiganModelDirectory.existsAsFile()
        ? hifiganModelDirectory.getParentDirectory() : hifiganModelDirectory;
    const auto model = modelDirectory.getChildFile("pc_nsf_hifigan.onnx");
    const auto config = modelDirectory.getChildFile("config.json");
    stream.writeInt64(model.getLastModificationTime().toMilliseconds());
    stream.writeInt64(model.getSize());
    stream.writeInt64(config.getLastModificationTime().toMilliseconds());
    stream.writeInt64(config.getSize());
    stream.writeInt(static_cast<int>(inference.requested));
    stream.writeInt(inference.deviceIndex);
    stream.writeInt(inference.intraOpThreads);
    for (const auto& note : clip.notes)
        writeNoteRenderFields(stream, note);
    return std::string(static_cast<const char*>(stream.getData()), stream.getDataSize());
}

// One cache entry per phrase: the clips it is made of, in order, plus the
// order itself so the two never collide.
std::string mergedRenderKey(const std::vector<const ClipData*>& group, const TrackData& track,
                            const juce::File& hifiganModelDirectory,
                            const backend::OrtExecutionConfig& inference)
{
    std::string key = "merged|" + std::to_string(static_cast<int>(track.renderOrder)) + "|";
    for (const auto* clip : group)
        key += renderKey(*clip, track, hifiganModelDirectory, inference, {}) + ";";
    return key;
}

backend::UtauRenderRequest makeUtauRequest(const ClipData& clip, const TrackData& track,
                                           const juce::File& resampler,
                                           const ProjectData& project)
{
    backend::UtauRenderRequest request;
    request.voicebankDirectory = track.voicebankDirectory;
    request.resamplerExecutable = resampler;
    request.fourRegion = utauModeUsesRegions(track.utauMode);
    request.consonantClasses = track.utauMode == UtauMode::mou;
    request.targetDurationSeconds = clip.durationSeconds;
    request.bpm = project.bpm;
    request.notes.reserve(clip.notes.size());
    for (const auto& note : clip.notes)
    {
        backend::UtauNoteRenderSpec renderedNote;
        renderedNote.alias = note.label;
        // The engine parses flags first-wins, so the note has to come first
        // for its own settings to override the track's rather than the other
        // way round.  This is also the order UTAU itself concatenates in.
        renderedNote.flags = note.utauFlags + track.utauGlobalFlags;
        renderedNote.splice = note.utauSplice;
        renderedNote.flagCurve = note.utauFlagCurveEnabled;
        // The engine reads a curve as straight lines between the points it is
        // given, so a curved segment is sampled into enough of them to follow.
        // Its store holds 64 per curve and drops the rest silently, so the
        // budget is spent here rather than losing the tail of a long curve.
        constexpr int flagCurvePointLimit = 60;
        for (const auto& curve : note.utauFlagCurves)
        {
            const auto& drawn = curve.points;
            if (drawn.empty()) continue;
            auto curved = 0;
            for (std::size_t index = 1; index < drawn.size(); ++index)
                if (drawn[index].shape != PitchCurveShape::linear) ++curved;
            const auto perSegment = curved > 0
                ? juce::jlimit(2, 12,
                    (flagCurvePointLimit - static_cast<int>(drawn.size())) / curved)
                : 0;
            std::vector<std::pair<double, double>> sampled;
            for (std::size_t index = 0; index < drawn.size(); ++index)
            {
                if (index > 0 && drawn[index].shape != PitchCurveShape::linear)
                    for (auto step = 1; step <= perSegment; ++step)
                    {
                        const auto at = drawn[index - 1].timeSeconds
                            + (drawn[index].timeSeconds - drawn[index - 1].timeSeconds)
                                * step / static_cast<double>(perSegment + 1);
                        sampled.emplace_back(at, flagCurveValueAt(drawn, at));
                    }
                sampled.emplace_back(drawn[index].timeSeconds, drawn[index].value);
            }
            renderedNote.flagCurves.emplace_back(curve.flag, std::move(sampled));
        }
        renderedNote.startSeconds = note.startSeconds;
        renderedNote.durationSeconds = note.durationSeconds;
        renderedNote.midiNote = note.midiNote;
        renderedNote.gain = note.gain;
        // The base value scales the whole envelope, and a note with no
        // envelope of its own still has an implied flat 0 dB line the base
        // raises just the same -- write one out so UTAU rendering honours it.
        auto shapedEnvelope = note.amplitudeEnvelope;
        if (shapedEnvelope.empty()
            && std::abs(note.amplitudeEnvelopeBasePercent - 100.0f) > 1.0e-6f)
            shapedEnvelope = { { 0.0, 0.0f },
                               { std::max(0.01, note.durationSeconds), 0.0f } };
        shapedEnvelope = scaledAmplitudeEnvelope(shapedEnvelope,
                                                 note.amplitudeEnvelopeBasePercent);
        renderedNote.amplitudeEnvelope.reserve(shapedEnvelope.size());
        for (const auto& point : shapedEnvelope)
            renderedNote.amplitudeEnvelope.push_back({ point.timeSeconds, point.gainDb });
        // Fitting the envelope to the note it is now is left to the mixer,
        // which is where the note's real lead-in is known.  Carrying only the
        // closing point out to the end here stretched the fall that belongs to
        // it, so a note twice as long faded for twice as long -- a shape
        // nobody chose, and not the one the roll was drawing.
        renderedNote.consonantVelocity =
            note.utauConsonantVelocity != inheritedUtauConsonantVelocity
            ? note.utauConsonantVelocity : track.utauConsonantVelocity;
        renderedNote.preutteranceOverrideEnabled =
            note.utauPreutteranceOverrideEnabled;
        renderedNote.preutteranceSeconds = note.utauPreutteranceSeconds;
        renderedNote.overlapOverrideEnabled = note.utauOverlapOverrideEnabled;
        renderedNote.overlapSeconds = note.utauOverlapSeconds;
        renderedNote.stpSeconds = note.utauStpSeconds;
        renderedNote.jieSplitSet = note.utauJieSplitSet;
        renderedNote.jieSplit = { note.utauJieSplit1, note.utauJieSplit2,
                                  note.utauJieSplit3 };
        renderedNote.flagSplit = note.utauFlagSplit;
        renderedNote.regionFlags = { note.utauRegionFlags1, note.utauRegionFlags2,
                                     note.utauRegionFlags3, note.utauRegionFlags4 };
        renderedNote.bpm = project.tempoAtSeconds(
            clip.startSeconds + note.startSeconds);
        if (!note.pitchControlPoints.empty())
        {
            const auto firstTime = std::min(0.0,
                note.pitchControlPoints.front().timeSeconds);
            renderedNote.pitchCurve.reserve(static_cast<std::size_t>(
                std::ceil((note.durationSeconds - firstTime) / 0.005)) + 2);
            for (auto time = firstTime; time < note.durationSeconds; time += 0.005)
                renderedNote.pitchCurve.push_back({ time,
                    (evaluatePitchCurve(note.pitchControlPoints, time) - note.midiNote)
                        * 100.0f + static_cast<float>(vibratoCentsAt(note, time)) });
            renderedNote.pitchCurve.push_back({ note.durationSeconds,
                (evaluatePitchCurve(note.pitchControlPoints, note.durationSeconds)
                    - note.midiNote) * 100.0f
                    + static_cast<float>(vibratoCentsAt(note, note.durationSeconds)) });
        }
        else
        {
            if (note.vibratoEnabled)
            {
                // A plain UTAU note carries a two-point contour: its start and
                // its end.  Sampling the swing only at those two instants
                // flattens it away entirely, so resample the base pitch densely
                // and add the swing to every point.
                const auto baseCentsAt = [&note](double time)
                {
                    if (note.contour.empty()) return 0.0f;
                    if (time <= note.contour.front().timeSeconds)
                        return renderedPitchCents(note, note.contour.front());
                    if (time >= note.contour.back().timeSeconds)
                        return renderedPitchCents(note, note.contour.back());
                    for (std::size_t index = 1; index < note.contour.size(); ++index)
                    {
                        const auto& left = note.contour[index - 1];
                        const auto& right = note.contour[index];
                        if (time > right.timeSeconds) continue;
                        const auto width = right.timeSeconds - left.timeSeconds;
                        const auto amount = width > 1.0e-9
                            ? static_cast<float>((time - left.timeSeconds) / width) : 0.0f;
                        return renderedPitchCents(note, left)
                            + (renderedPitchCents(note, right)
                               - renderedPitchCents(note, left)) * amount;
                    }
                    return renderedPitchCents(note, note.contour.back());
                };
                renderedNote.pitchCurve.reserve(static_cast<std::size_t>(
                    std::ceil(note.durationSeconds / 0.005)) + 2);
                for (auto time = 0.0; time < note.durationSeconds; time += 0.005)
                    renderedNote.pitchCurve.push_back({ time,
                        baseCentsAt(time)
                            + static_cast<float>(vibratoCentsAt(note, time)) });
                renderedNote.pitchCurve.push_back({ note.durationSeconds,
                    baseCentsAt(note.durationSeconds)
                        + static_cast<float>(vibratoCentsAt(note, note.durationSeconds)) });
            }
            else
            {
                renderedNote.pitchCurve.reserve(note.contour.size());
                for (const auto& point : note.contour)
                    renderedNote.pitchCurve.push_back({ point.timeSeconds,
                                                        renderedPitchCents(note, point) });
            }
        }
        // A continuous, unclamped native pitch evaluator, shared by the UTAU
        // resampler PIT and (via contourAt) the native backends: cents from the
        // note's own MIDI at any local time, so a seam's lead-in and tail read
        // the true contour instead of a clamped flat hold.  Captures a copy of
        // the note so it stays valid for the whole async render.
        renderedNote.timelinePitchCents =
            [note, baseMidi = note.midiNote](double time) -> float
        {
            const auto vib = static_cast<float>(vibratoCentsAt(note,
                juce::jlimit(0.0, note.durationSeconds, time)));
            if (!note.pitchControlPoints.empty())
                return (evaluatePitchCurve(note.pitchControlPoints, time) - baseMidi)
                    * 100.0f + vib;
            if (note.contour.empty()) return vib;
            const auto sampleContour = [&](double t) -> float
            {
                if (t <= note.contour.front().timeSeconds)
                    return renderedPitchCents(note, note.contour.front());
                if (t >= note.contour.back().timeSeconds)
                    return renderedPitchCents(note, note.contour.back());
                for (std::size_t i = 1; i < note.contour.size(); ++i)
                {
                    const auto& l = note.contour[i - 1];
                    const auto& r = note.contour[i];
                    if (t > r.timeSeconds) continue;
                    const auto w = r.timeSeconds - l.timeSeconds;
                    const auto a = w > 1.0e-9
                        ? static_cast<float>((t - l.timeSeconds) / w) : 0.0f;
                    return renderedPitchCents(note, l)
                        + (renderedPitchCents(note, r) - renderedPitchCents(note, l)) * a;
                }
                return renderedPitchCents(note, note.contour.back());
            };
            return sampleContour(time) + vib;
        };
        request.notes.push_back(std::move(renderedNote));
    }

    // Adjacent notes retain independent tail/head pitches.  A short automatic
    // S transition occupies the interval around their nominal boundary:
    // previous tail at -inset -> following head at +inset.  The identical
    // absolute-pitch bridge is written into both resampler requests so their
    // overlap/crossfade cannot produce two contradictory pitch trajectories.
    std::vector<std::size_t> order(request.notes.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](auto left, auto right)
    {
        return request.notes[left].startSeconds < request.notes[right].startSeconds;
    });
    const auto centsAt = [](const backend::UtauNoteRenderSpec& note, double time)
    {
        if (note.pitchCurve.empty()) return 0.0f;
        const auto right = std::lower_bound(note.pitchCurve.begin(), note.pitchCurve.end(), time,
            [](const backend::UtauPitchPoint& point, double value)
            {
                return point.timeSeconds < value;
            });
        if (right == note.pitchCurve.begin()) return right->cents;
        if (right == note.pitchCurve.end()) return note.pitchCurve.back().cents;
        const auto& left = *std::prev(right);
        const auto amount = right->timeSeconds > left.timeSeconds
            ? static_cast<float>((time - left.timeSeconds)
                / (right->timeSeconds - left.timeSeconds)) : 0.0f;
        return left.cents + (right->cents - left.cents) * amount;
    };
    const auto replaceCurveRange = [](backend::UtauNoteRenderSpec& note,
                                      double start, double end,
                                      float startMidi, float endMidi)
    {
        note.pitchCurve.erase(std::remove_if(note.pitchCurve.begin(), note.pitchCurve.end(),
            [&](const auto& point)
            {
                return point.timeSeconds >= start - 1.0e-7
                    && point.timeSeconds <= end + 1.0e-7;
            }), note.pitchCurve.end());
        const auto duration = std::max(1.0e-6, end - start);
        std::vector<backend::UtauPitchPoint> bridge;
        for (auto time = start; time < end; time += 0.005)
        {
            const auto u = static_cast<float>(juce::jlimit(0.0, 1.0,
                (time - start) / duration));
            const auto shaped = u * u * (3.0f - 2.0f * u);
            const auto midi = startMidi + (endMidi - startMidi) * shaped;
            bridge.push_back({ time, (midi - note.midiNote) * 100.0f });
        }
        bridge.push_back({ end, (endMidi - note.midiNote) * 100.0f });
        note.pitchCurve.insert(note.pitchCurve.end(), bridge.begin(), bridge.end());
        std::stable_sort(note.pitchCurve.begin(), note.pitchCurve.end(),
            [](const auto& left, const auto& right)
            {
                return left.timeSeconds < right.timeSeconds;
            });
    };
    for (std::size_t index = 1; index < order.size(); ++index)
    {
        auto& previous = request.notes[order[index - 1]];
        auto& next = request.notes[order[index]];
        const auto previousEnd = previous.startSeconds + previous.durationSeconds;
        if (std::abs(previousEnd - next.startSeconds) > 0.002) continue;
        const auto inset = automaticUtauPitchTransitionInset(
            previous.durationSeconds, next.durationSeconds);
        const auto previousTailTime = previous.durationSeconds - inset;
        const auto nextHeadTime = inset;
        const auto startMidi = previous.midiNote
            + centsAt(previous, previousTailTime) / 100.0f;
        const auto endMidi = next.midiNote
            + centsAt(next, nextHeadTime) / 100.0f;
        replaceCurveRange(previous, previousTailTime,
                          previous.durationSeconds + inset, startMidi, endMidi);
        replaceCurveRange(next, -inset, nextHeadTime, startMidi, endMidi);
    }
    return request;
}
}

AudioEngine::AudioEngine()
{
    startupLog("AudioEngine: construct");
    // Before anyone says otherwise, the engine that travels with the
    // application is the one to use.  Without this a headless caller that
    // never sets a resampler renders through the built-in fallback and sounds
    // plausible while the packaged engine sits unused beside it.
    utauResamplerFile = bundledUtauResampler(juce::File::getSpecialLocation(
        juce::File::currentExecutableFile).getParentDirectory());
    formatManager.registerBasicFormats();
    sourcePlayer.setSource(this);
    startupLog("AudioEngine: opening default devices");
    deviceManager.initialiseWithDefaultDevices(0, 2);
    startupLog("AudioEngine: default devices returned");
    deviceManager.addAudioCallback(&sourcePlayer);
}

AudioEngine::~AudioEngine()
{
    // The device callback may be running on the driver's real-time thread.
    // Detach it before changing/destroying the AudioSourcePlayer; doing these
    // operations in the opposite order leaves a small release-build race in
    // which the driver can enter a player whose source is being torn down.
    playing.store(false, std::memory_order_release);
    deviceManager.removeAudioCallback(&sourcePlayer);
    sourcePlayer.setSource(nullptr);

    // RenderService is declared before the playback/cache members and would
    // therefore normally be destroyed after them.  Stop its jobs explicitly
    // while all callback targets and caches are still alive.
    renderService.cancelAll();
    deviceManager.closeAudioDevice();
}

bool AudioEngine::ensureOutputDevice(juce::String& error)
{
    const auto hasOutput = [this]
    {
        const auto* device = deviceManager.getCurrentAudioDevice();
        return device != nullptr
            && device->getActiveOutputChannels().countNumberOfSetBits() > 0;
    };
    if (hasOutput())
    {
        error.clear();
        return true;
    }
    // A driver may appear after startup (USB interface connected, Bluetooth
    // endpoint enabled, or Windows device service restarted).  Retry here so
    // Play does not enter a false playing state with no callback to advance
    // the transport.
    startupLog("AudioEngine: opening output device");
    error = deviceManager.initialiseWithDefaultDevices(0, 2);
    startupLog("AudioEngine: device initialisation returned: " + error);
    if (hasOutput())
    {
        error.clear();
        return true;
    }
    if (error.isEmpty()) error = "No audio output device is available";
    return false;
}

void AudioEngine::restoreDeviceState(juce::PropertiesFile& properties)
{
    const auto saved = properties.getValue("audio.deviceState");
    if (saved.isEmpty()) return;
    const auto xml = juce::parseXML(saved);
    if (xml == nullptr) return;
    startupLog("AudioEngine: restoring saved device");
    deviceManager.initialise(0, 2, xml.get(), true);
    startupLog("AudioEngine: saved device returned");
}

void AudioEngine::saveDeviceState(juce::PropertiesFile& properties) const
{
    if (const auto state = deviceManager.createStateXml())
    {
        properties.setValue("audio.deviceState", state->toString());
        properties.saveIfNeeded();
    }
}

void AudioEngine::prepareToPlay(int, double sampleRate)
{
    outputSampleRate.store(sampleRate > 0.0 ? sampleRate : 48'000.0);
}

void AudioEngine::releaseResources()
{
}

std::optional<double> AudioEngine::probeDuration(const juce::File& file)
{
    if (auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(file)))
        return static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    return std::nullopt;
}

bool AudioEngine::setAuditionFile(const juce::File& file)
{
    auto reader = std::shared_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(file));
    if (reader == nullptr) return false;
    stop();
    {
        const juce::ScopedWriteLock guard(renderLock);
        auditionReader = std::move(reader);
        auditionScratch.setSize(juce::jlimit(1, 2, static_cast<int>(auditionReader->numChannels)), 2);
        // Only on the way in: swapping one audition file for another must not
        // overwrite the project position with an audition one.
        if (!auditionMode.exchange(true))
            projectTimelineSample.store(timelineSample.load());
    }
    timelineSample.store(0);
    sendChangeMessage();
    return true;
}

void AudioEngine::clearAuditionFile()
{
    stop();
    {
        const juce::ScopedWriteLock guard(renderLock);
        auditionReader.reset();
        auditionScratch.setSize(0, 0);
        auditionMode.store(false);
    }
    // Back on the project's own timeline, standing where it was left.  Zeroing
    // it here sent the playhead to the beginning every time the sample editor
    // was closed, and the next zoom then dragged the whole view after it.
    timelineSample.store(projectTimelineSample.load());
    sendChangeMessage();
}

bool AudioEngine::clipsJoinAt(const ClipData& clip, const ClipData& neighbour,
                              bool asNext)
{
    if (clip.notes.empty()) return false;
    // Melodyne places the two elements of a pitch join exactly back to back,
    // so a seam that is not touching is not one of them however the notes are
    // marked.  Two milliseconds is the same tolerance the fades use.
    const auto overlap = asNext
        ? clip.startSeconds + clip.durationSeconds - neighbour.startSeconds
        : neighbour.startSeconds + neighbour.durationSeconds - clip.startSeconds;
    if (std::abs(overlap) > 0.002) return false;
    return asNext ? clip.notes.back().connectedToNext
                  : clip.notes.front().connectedToPrevious;
}

namespace
{
// One bucket per millisecond of the note, holding the extremes of the samples
// inside it.  A bucket is what a waveform is drawn from, and a millisecond is
// finer than any zoom this roll offers, so the peaks survive zooming in
// without the whole rendered buffer being kept around to be re-read.
UtauNoteWaveform measureNoteWaveform(const juce::AudioBuffer<float>& buffer,
                                     double sampleRate, double startInBuffer,
                                     double durationSeconds)
{
    UtauNoteWaveform waveform;
    waveform.durationSeconds = durationSeconds;
    if (sampleRate <= 0.0 || durationSeconds <= 0.0 || buffer.getNumSamples() <= 0)
        return waveform;
    const auto buckets = std::max(1, static_cast<int>(std::ceil(durationSeconds * 1000.0)));
    waveform.minima.assign(static_cast<std::size_t>(buckets), 0.0f);
    waveform.maxima.assign(static_cast<std::size_t>(buckets), 0.0f);
    const auto first = static_cast<juce::int64>(std::llround(startInBuffer * sampleRate));
    const auto samples = static_cast<juce::int64>(std::llround(durationSeconds * sampleRate));
    for (int bucket = 0; bucket < buckets; ++bucket)
    {
        const auto from = first + samples * bucket / buckets;
        const auto to = first + samples * (bucket + 1) / buckets;
        auto low = 0.0f;
        auto high = 0.0f;
        for (auto index = std::max<juce::int64>(0, from);
             index < std::min<juce::int64>(to, buffer.getNumSamples()); ++index)
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            {
                const auto value = buffer.getSample(channel, static_cast<int>(index));
                low = std::min(low, value);
                high = std::max(high, value);
            }
        waveform.minima[static_cast<std::size_t>(bucket)] = low;
        waveform.maxima[static_cast<std::size_t>(bucket)] = high;
    }
    return waveform;
}
}

std::uint64_t AudioEngine::utauNoteRenderHash(const NoteData& note)
{
    return noteRenderHashImpl(note);
}

std::shared_ptr<const std::vector<UtauNoteWaveform>>
    AudioEngine::utauNoteWaveforms() const
{
    const juce::ScopedLock guard(utauWaveformLock);
    return utauWaveformSnapshot;
}

void AudioEngine::refreshUtauWaveformSnapshot()
{
    auto collected = std::make_shared<std::vector<UtauNoteWaveform>>();
    {
        const juce::ScopedReadLock guard(renderLock);
        for (const auto& loaded : loadedClips)
        {
            // The last render that was ready, which is the one being heard --
            // so an edit that has not finished rendering leaves the notes it
            // did not touch showing what they still sound like.
            const auto& entry = loaded->rendered != nullptr
                                && loaded->rendered->ready.load(std::memory_order_acquire)
                ? loaded->rendered : loaded->fallbackRendered;
            if (entry == nullptr || !entry->ready.load(std::memory_order_acquire)) continue;
            const juce::ScopedLock sliceGuard(entry->sliceLock);
            for (const auto& waveform : entry->utauWaveforms)
                collected->push_back(waveform);
        }
    }
    const juce::ScopedLock guard(utauWaveformLock);
    utauWaveformSnapshot = std::move(collected);
}

std::vector<std::vector<const ClipData*>> AudioEngine::glideChains(
    const std::vector<const ClipData*>& orderedClips)
{
    const auto count = orderedClips.size();
    const auto usable = [](const ClipData& clip)
    {
        return !clip.muted && !clip.notes.empty() && clip.sourceFile.existsAsFile();
    };
    std::vector<std::vector<const ClipData*>> chains;
    std::vector<bool> taken(count, false);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (taken[index]) continue;
        const auto& start = *orderedClips[index];
        if (!start.glideConnectedToNext || !usable(start)) continue;
        std::vector<const ClipData*> chain { &start };
        taken[index] = true;
        const auto* cursor = &start;
        for (;;)
        {
            // The successor need not be the next clip in start order: it is
            // the one whose own preceding join says it is glide-connected and
            // whose source range continues this clip's, on the same recording.
            const ClipData* found = nullptr;
            std::size_t foundAt = 0;
            for (std::size_t other = 0; other < count; ++other)
                if (!taken[other] && usable(*orderedClips[other])
                    && orderedClips[other]->sourceFile == cursor->sourceFile
                    && orderedClips[other]->glideConnectedFromPrevious
                    && std::abs(orderedClips[other]->sourceOffsetSeconds
                        - (cursor->sourceOffsetSeconds + cursor->sourceDurationSeconds)) <= 0.01)
                {
                    found = orderedClips[other];
                    foundAt = other;
                    break;
                }
            if (found == nullptr) break;
            chain.push_back(found);
            taken[foundAt] = true;
            cursor = found;
            if (!cursor->glideConnectedToNext) break;
        }
        // One clip on its own is not a phrase, so it goes down the ordinary
        // per-clip path instead; release it so a later chain may still take it.
        if (chain.size() >= 2)
            chains.push_back(std::move(chain));
        else
            taken[index] = false;
    }
    return chains;
}

int AudioEngine::diagnosticMergedPhraseCount() const
{
    const juce::ScopedReadLock guard(renderLock);
    auto phrases = 0;
    for (const auto& [key, entry] : renderCache)
        if (key.rfind("merged|", 0) == 0) ++phrases;
    return phrases;
}

std::vector<float> AudioEngine::diagnosticNativeTargetMidi(
    const ProjectData& project, int trackIndex, int clipIndex)
{
    if (trackIndex < 0 || trackIndex >= static_cast<int>(project.tracks.size()))
        return {};
    const auto& track = project.tracks[static_cast<std::size_t>(trackIndex)];
    if (clipIndex < 0 || clipIndex >= static_cast<int>(track.clips.size()))
        return {};
    const auto& clip = track.clips[static_cast<std::size_t>(clipIndex)];
    const auto request = makeRenderRequest(clip, track, juce::File{},
                                           backend::OrtExecutionConfig{});
    return request.targetMidi;
}

void AudioEngine::syncProject(const ProjectData& project)
{
    const juce::ScopedWriteLock guard(renderLock);
    rebuildLoadedClips(project);
    auto contentDuration = 0.0;
    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            if (!clip.muted)
                contentDuration = std::max(contentDuration, clip.startSeconds + clip.durationSeconds);
    projectDurationSeconds.store(contentDuration);
}

bool AudioEngine::trackIsAudible(bool muted, bool solo, bool anySolo,
                                 bool referenceOnly, bool beingWorkedOn)
{
    if (muted) return false;
    if (anySolo && !solo) return false;
    // A material track is there to be worked against, not to be part of the
    // piece: it sounds while it is the one in hand and never when anything
    // else is playing.
    if (referenceOnly && !beingWorkedOn) return false;
    return true;
}

void AudioEngine::setAuditionTrack(const juce::String& trackId)
{
    const juce::ScopedWriteLock guard(renderLock);
    auditionTrackId = trackId;
}

void AudioEngine::setUtauRenderNoteSelection(const std::vector<juce::String>& noteIds)
{
    const juce::ScopedWriteLock guard(renderLock);
    // A normal marquee is an exact audition scope.  Keeping old IDs here made
    // every later marquee silently accumulate historical notes and caused the
    // rendered phrase to disagree with the visible selection.  Shift-marquee
    // is already represented by noteIds containing both old and new notes.
    utauRenderNoteSelection.clear();
    for (const auto& id : noteIds)
        if (id.isNotEmpty())
        {
            utauRenderNoteSelection.insert(id.toStdString());
            utauRenderedNoteHistory.insert(id.toStdString());
        }
}

int AudioEngine::selectEveryUtauNote(const ProjectData& project)
{
    std::vector<juce::String> everyNote;
    for (const auto& track : project.tracks)
        if (track.pitchAlgorithm == PitchAlgorithm::utau)
            for (const auto& clip : track.clips)
                for (const auto& note : clip.notes) everyNote.push_back(note.id);
    if (everyNote.empty()) return 0;
    setUtauRenderNoteSelection(everyNote);
    return static_cast<int>(everyNote.size());
}

bool AudioEngine::selectAllRenderedUtauNotes()
{
    const juce::ScopedWriteLock guard(renderLock);
    utauRenderNoteSelection = utauRenderedNoteHistory;
    return !utauRenderNoteSelection.empty();
}

void AudioEngine::setHifiganModelDirectory(const juce::File& directory)
{
    const juce::ScopedWriteLock guard(renderLock);
    if (hifiganModelDirectory == directory) return;
    hifiganModelDirectory = directory;
    renderCache.clear();
}

juce::File AudioEngine::bundledUtauResampler(const juce::File& executableDirectory)
{
    if (executableDirectory == juce::File{}) return {};
    // The engine this application is built to drive, in the two places a
    // portable copy would carry it.  Named outright rather than "any exe
    // here" -- the folder is full of DLLs, and guessing would be worse than
    // nothing.
    for (const auto* relative : { "engines/WCSNDM.exe", "WCSNDM.exe" })
    {
        const auto bundled = executableDirectory.getChildFile(relative);
        if (bundled.existsAsFile()) return bundled;
    }
    return {};
}

juce::File AudioEngine::resolveUtauResampler(const juce::String& configured,
                                             const juce::File& executableDirectory)
{
    // What the caller names wins, so a chosen engine is never quietly swapped.
    // Quotes and padding are tolerated because this often arrives pasted.
    const juce::File chosen(configured.trim().unquoted());
    if (chosen.existsAsFile()) return chosen;
    return bundledUtauResampler(executableDirectory);
}

void AudioEngine::setUtauResamplerFile(const juce::File& executable)
{
    const juce::ScopedWriteLock guard(renderLock);
    // A settings value carried over from another machine names a file that is
    // not there; fall back rather than render through nothing.
    const auto resolved = executable.existsAsFile()
        ? executable
        : bundledUtauResampler(juce::File::getSpecialLocation(
              juce::File::currentExecutableFile).getParentDirectory());
    if (utauResamplerFile == resolved) return;
    utauResamplerFile = resolved;
    renderCache.clear();
}

void AudioEngine::setInferenceConfiguration(backend::InferenceBackend inference, int deviceIndex)
{
    const backend::OrtExecutionConfig next {
        inference, deviceIndex, std::max(1, juce::SystemStats::getNumCpus() - 1)
    };
    const juce::ScopedWriteLock guard(renderLock);
    if (inferenceConfiguration.requested == next.requested
        && inferenceConfiguration.deviceIndex == next.deviceIndex
        && inferenceConfiguration.intraOpThreads == next.intraOpThreads)
        return;
    inferenceConfiguration = next;
    renderCache.clear();
}

void AudioEngine::rebuildLoadedClips(const ProjectData& project)
{
    std::unordered_set<std::string> projectNoteIds;
    std::unordered_set<std::string> projectClipIds;
    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
        {
            projectClipIds.insert(clip.id.toStdString());
            for (const auto& note : clip.notes)
                projectNoteIds.insert(note.id.toStdString());
        }
    std::erase_if(utauRenderNoteSelection, [&projectNoteIds](const auto& id)
    {
        return !projectNoteIds.contains(id);
    });
    std::erase_if(utauRenderedNoteHistory, [&projectNoteIds](const auto& id)
    {
        return !projectNoteIds.contains(id);
    });
    std::erase_if(playbackFallbackByClip, [&projectClipIds](const auto& item)
    {
        return !projectClipIds.contains(item.first);
    });
    for (const auto& loaded : loadedClips)
    {
        auto ready = loaded->rendered != nullptr
                && loaded->rendered->ready.load(std::memory_order_acquire)
            ? loaded->rendered : loaded->fallbackRendered;
        if (ready != nullptr && ready->ready.load(std::memory_order_acquire))
            playbackFallbackByClip[loaded->clip.id.toStdString()] = std::move(ready);
    }
    loadedClips.clear();
    trackMeters.clear();
    std::unordered_map<std::string, std::shared_ptr<juce::AudioFormatReader>> readers;
    std::unordered_set<std::string> activeRenderKeys;
    // Hand one clip its span of a decoded phrase.  The audible range is found
    // here rather than copied from the phrase, because it is asked per clip.
    const auto sliceInto = [](const RenderedClip& phrase,
                              const RenderedClip::SliceTarget& target)
    {
        if (target.clip == nullptr || phrase.buffer.getNumSamples() <= 0) return;
        const auto channels = phrase.buffer.getNumChannels();
        const auto copied = std::min(target.sampleCount,
            std::max(0, phrase.buffer.getNumSamples() - target.startSample));
        target.clip->buffer.setSize(channels, std::max(1, target.sampleCount));
        target.clip->buffer.clear();
        for (int channel = 0; channel < channels; ++channel)
            target.clip->buffer.copyFrom(channel, 0, phrase.buffer, channel,
                                         target.startSample, copied);
        auto firstAudible = target.clip->buffer.getNumSamples();
        auto lastAudible = -1;
        constexpr auto audibleThreshold = 1.0e-5f;
        for (int channel = 0; channel < channels; ++channel)
            for (int sample = 0; sample < target.clip->buffer.getNumSamples(); ++sample)
                if (std::abs(target.clip->buffer.getSample(channel, sample)) > audibleThreshold)
                {
                    firstAudible = std::min(firstAudible, sample);
                    lastAudible = std::max(lastAudible, sample);
                }
        target.clip->firstAudibleSample = firstAudible;
        target.clip->lastAudibleSample = lastAudible;
        target.clip->sampleRate = phrase.sampleRate;
        target.clip->backend = phrase.backend;
        target.clip->warning = phrase.warning;
        target.clip->progress.store(1.0f, std::memory_order_release);
        target.clip->ready.store(true, std::memory_order_release);
        target.clip->finished.store(true, std::memory_order_release);
    };
    const auto anySolo = std::any_of(project.tracks.begin(), project.tracks.end(),
                                     [](const auto& track) { return track.solo; });
    for (const auto& track : project.tracks)
    {
        auto meter = std::make_shared<std::atomic<float>>(0.0f);
        trackMeters[track.id.toStdString()] = meter;
        if (!trackIsAudible(track.muted, track.solo, anySolo, track.referenceOnly,
                            track.id == auditionTrackId))
            continue;
        std::vector<const ClipData*> orderedClips;
        orderedClips.reserve(track.clips.size());
        for (const auto& clip : track.clips) orderedClips.push_back(&clip);
        std::stable_sort(orderedClips.begin(), orderedClips.end(), [](const auto* left, const auto* right)
        {
            return left->startSeconds < right->startSeconds;
        });
        const auto count = orderedClips.size();
        // stretchSpliceThenPitch: a chain of clips joined by Melodyne pitch
        // joins is decoded in one pass instead of being spliced afterwards.
        // Only the neural decoder is worth doing this for -- it is the one
        // whose phase and mel continuity a splice actually breaks.
        struct PendingGroup
        {
            std::vector<const ClipData*> clips;
            std::vector<std::shared_ptr<RenderedClip>> rendered;
        };
        std::vector<PendingGroup> pendingGroups;
        std::vector<bool> inGlideGroup(count, false);
        const auto mergedMode = track.compose
            && track.renderOrder == RenderOrder::stretchSpliceThenPitch
            && track.pitchAlgorithm == PitchAlgorithm::nsfHifigan;
        if (mergedMode)
            for (auto& chain : glideChains(orderedClips))
            {
                PendingGroup group;
                for (const auto* member : chain)
                {
                    group.clips.push_back(member);
                    for (std::size_t index = 0; index < count; ++index)
                        if (orderedClips[index] == member) { inGlideGroup[index] = true; break; }
                }
                pendingGroups.push_back(std::move(group));
            }
        for (std::size_t clipIndex = 0; clipIndex < count; ++clipIndex)
        {
            const auto& clip = *orderedClips[clipIndex];
            // A voicebank track drives synthesis from note labels + OTO rather
            // than a placed recording.  It may synthesise through the classic
            // resampler (PitchAlgorithm::utau) or natively through the one
            // NSF-HiFiGAN renderer (nsfHifigan + a voicebank directory).  Both
            // need the same note-driven request path; only the final render
            // call differs.
            const auto classicUtau = track.pitchAlgorithm == PitchAlgorithm::utau;
            const auto nsfVoicebank = track.pitchAlgorithm == PitchAlgorithm::nsfHifigan
                && track.voicebankDirectory.isDirectory();
            const auto utauTrack = classicUtau || nsfVoicebank;
            if (clip.muted || (!utauTrack && !clip.sourceFile.existsAsFile())) continue;
            std::shared_ptr<juce::AudioFormatReader> reader;
            if (!utauTrack)
            {
                const auto sourceKey = clip.sourceFile.getFullPathName().toStdString();
                reader = readers[sourceKey];
                if (reader == nullptr)
                {
                    reader.reset(formatManager.createReaderFor(clip.sourceFile));
                    if (reader == nullptr) continue;
                    readers[sourceKey] = reader;
                }
            }
            auto loaded = std::make_unique<LoadedClip>();
            loaded->clip = clip;
            loaded->trackId = track.id.toStdString();
            loaded->smoothOverlaps = track.smoothOverlaps;
            const auto compactDeclick = std::min(0.0025, loaded->clip.durationSeconds * 0.5);
            loaded->clip.fadeInSeconds = std::max(loaded->clip.fadeInSeconds, compactDeclick);
            loaded->clip.fadeOutSeconds = std::max(loaded->clip.fadeOutSeconds, compactDeclick);
            // Worked out once: the mixer fades such a seam, and the neural
            // decoder is told below not to fade its own edge into it as well.
            const auto joinedStart = clipIndex > 0
                && clipsJoinAt(clip, *orderedClips[clipIndex - 1], false);
            const auto joinedEnd = clipIndex + 1 < orderedClips.size()
                && clipsJoinAt(clip, *orderedClips[clipIndex + 1], true);
            // A seam inside a decoded phrase was never cut, so there is nothing
            // there to fade across; the mixer must lay these buffers down flat.
            if (inGlideGroup[clipIndex])
            {
                loaded->smoothOverlaps = false;
                loaded->clip.fadeInSeconds = 0.0;
                loaded->clip.fadeOutSeconds = 0.0;
                loaded->clip.crossfadeInSeconds = 0.0;
                loaded->clip.crossfadeOutSeconds = 0.0;
            }
            if (track.smoothOverlaps && !inGlideGroup[clipIndex] && clipIndex > 0)
            {
                const auto& previous = *orderedClips[clipIndex - 1];
                const auto overlap = previous.startSeconds + previous.durationSeconds - clip.startSeconds;
                if (overlap > 1.0e-6)
                    loaded->clip.fadeInSeconds = std::max(loaded->clip.fadeInSeconds,
                        std::min({ overlap, 0.1, loaded->clip.durationSeconds }));
                else if (joinedStart && clip.crossfadeInSeconds <= 1.0e-6)
                    loaded->clip.fadeInSeconds = std::max(loaded->clip.fadeInSeconds,
                        std::min(0.006, loaded->clip.durationSeconds * 0.5));
            }
            if (track.smoothOverlaps && !inGlideGroup[clipIndex]
                && clipIndex + 1 < orderedClips.size())
            {
                const auto& next = *orderedClips[clipIndex + 1];
                const auto overlap = clip.startSeconds + clip.durationSeconds - next.startSeconds;
                if (overlap > 1.0e-6)
                    loaded->clip.fadeOutSeconds = std::max(loaded->clip.fadeOutSeconds,
                        std::min({ overlap, 0.1, loaded->clip.durationSeconds }));
                else if (joinedEnd && clip.crossfadeOutSeconds <= 1.0e-6)
                    loaded->clip.fadeOutSeconds = std::max(loaded->clip.fadeOutSeconds,
                        std::min(0.006, loaded->clip.durationSeconds * 0.5));
            }
            loaded->trackGain = track.volume;
            loaded->trackPan = juce::jlimit(-1.0f, 1.0f, track.pan);
            loaded->meter = meter;
            loaded->reader = reader;
            if (const auto fallback = playbackFallbackByClip.find(clip.id.toStdString());
                fallback != playbackFallbackByClip.end())
                // Do not audition an earlier backend's cached output while a
                // newly selected backend is pending or unavailable.
                loaded->fallbackRendered.reset();
            auto renderClip = clip;
            const auto hasUtauSelection = utauTrack && !utauRenderNoteSelection.empty();
            // UTAU rendering is explicitly selection-driven.  Rendering every
            // MIDI note while the selection is empty can occupy the worker with
            // a whole song before a subsequently marquee-selected phrase gets
            // a chance to render.  An empty selection therefore schedules no
            // new UTAU work; previously completed audio remains available via
            // fallbackRendered/playbackFallbackByClip.
            if (utauTrack)
                std::erase_if(renderClip.notes, [this](const auto& note)
                {
                    return !utauRenderNoteSelection.contains(note.id.toStdString());
                });
            std::stable_sort(renderClip.notes.begin(), renderClip.notes.end(),
                [](const auto& left, const auto& right)
                {
                    if (left.startSeconds != right.startSeconds)
                        return left.startSeconds < right.startSeconds;
                    if (left.midiNote != right.midiNote) return left.midiNote < right.midiNote;
                    return left.id < right.id;
                });
            // A fallback belongs to an older selection.  It is valid only for
            // a clip that also contains at least one note in the current render
            // scope; otherwise an unrelated old phrase leaks into the mix.
            //
            // A scope of nothing but rests is that same case: there is nothing
            // to sound, so the render comes back silent, a silent render never
            // becomes ready, and playback falls back to the last one that was
            // -- which is the phrase these notes used to be.  Typing RR over a
            // note that had already been played went on playing it.
            const auto anythingSounds = std::any_of(
                renderClip.notes.begin(), renderClip.notes.end(),
                [](const auto& note) { return !backend::isRestLyric(note.label); });
            if (utauTrack && !anythingSounds)
                loaded->fallbackRendered.reset();
            auto requestClip = renderClip;
            auto renderTimelineOffset = 0.0;
            if (hasUtauSelection && !requestClip.notes.empty())
            {
                const auto first = std::min_element(requestClip.notes.begin(), requestClip.notes.end(),
                    [](const auto& left, const auto& right)
                    {
                        return left.startSeconds < right.startSeconds;
                    });
                const auto last = std::max_element(requestClip.notes.begin(), requestClip.notes.end(),
                    [](const auto& left, const auto& right)
                    {
                        return left.startSeconds + left.durationSeconds
                            < right.startSeconds + right.durationSeconds;
                    });
                // Keep enough lead-in for ordinary oto.ini preutterance while
                // avoiding a song-length buffer for a small marquee selection.
                renderTimelineOffset = std::max(0.0, first->startSeconds - 1.0);
                const auto selectedEnd = last->startSeconds + last->durationSeconds + 0.25;
                requestClip.durationSeconds = std::max(0.03,
                    std::min(clip.durationSeconds, selectedEnd) - renderTimelineOffset);
                for (auto& note : requestClip.notes)
                    note.startSeconds -= renderTimelineOffset;
                requestClip.startSeconds += renderTimelineOffset;
            }
            // Every compose path must use a duration-preserving, formant-preserving render.
            // Until a selected external engine is present, the native mld5 renderer is the
            // deterministic model-free fallback rather than device-rate resampling, which
            // shifts both F0 and formants and creates the "old/child voice" failure mode.
            if (track.compose && !renderClip.notes.empty() && inGlideGroup[clipIndex])
            {
                // Filled from the phrase once it is decoded, below.  It is not
                // a renderCache entry: the phrase is what the cache holds, and
                // this buffer is only ever a copy out of it.
                auto slice = std::make_shared<RenderedClip>();
                loaded->rendered = slice;
                for (auto& group : pendingGroups)
                    for (const auto* member : group.clips)
                        if (member == &clip)
                        {
                            group.rendered.push_back(std::move(slice));
                            break;
                        }
            }
            else if (track.compose && !renderClip.notes.empty())
            {
                const auto cacheKey = renderKey(
                    renderClip, track, hifiganModelDirectory, inferenceConfiguration,
                    utauResamplerFile);
                activeRenderKeys.insert(cacheKey);
                auto& state = renderCache[cacheKey];
                if (state == nullptr) state = std::make_shared<RenderedClip>();
                state->timelineOffsetSeconds = renderTimelineOffset;
                loaded->rendered = state;
                // A failed/empty render must not remain as a permanently silent
                // cache entry.  The next selection/project sync is allowed to
                // retry it after paths or voicebank contents have been fixed.
                if (state->finished.load(std::memory_order_acquire)
                    && !state->ready.load(std::memory_order_acquire))
                {
                    state->scheduled.store(false, std::memory_order_release);
                    state->finished.store(false, std::memory_order_release);
                    state->progress.store(0.0f, std::memory_order_release);
                }
                if (!state->scheduled.exchange(true))
                {
                    const auto publish = [state](backend::RenderedAudio result) mutable
                    {
                         if (result.buffer.getNumSamples() <= 0 || result.sampleRate <= 0.0)
                         {
                             state->warning = result.warning.isNotEmpty() ? result.warning
                                 : "Selected backend returned no audio";
                             state->backend = result.backend;
                            state->progress.store(1.0f, std::memory_order_release);
                            state->finished.store(true, std::memory_order_release);
                            return;
                        }
                        auto firstAudible = result.buffer.getNumSamples();
                        auto lastAudible = -1;
                        constexpr auto audibleThreshold = 1.0e-5f;
                        for (int channel = 0; channel < result.buffer.getNumChannels(); ++channel)
                            for (int sample = 0; sample < result.buffer.getNumSamples(); ++sample)
                                if (std::abs(result.buffer.getSample(channel, sample))
                                    > audibleThreshold)
                                {
                                    firstAudible = std::min(firstAudible, sample);
                                    lastAudible = std::max(lastAudible, sample);
                                }
                        if (lastAudible < firstAudible)
                        {
                            state->progress.store(1.0f, std::memory_order_release);
                            state->finished.store(true, std::memory_order_release);
                            return;
                        }
                        state->buffer = std::move(result.buffer);
                        state->sampleRate = result.sampleRate;
                        state->firstAudibleSample = firstAudible;
                        state->lastAudibleSample = lastAudible;
                        state->backend = std::move(result.backend);
                        state->warning = std::move(result.warning);
                        state->ready.store(true, std::memory_order_release);
                        state->progress.store(1.0f, std::memory_order_release);
                        state->finished.store(true, std::memory_order_release);
                    };
                    if (utauTrack)
                    {
                        // What each note will occupy in the buffer that comes
                        // back, and what the note looked like when it was sent.
                        struct PendingNote
                        {
                            juce::String id;
                            std::uint64_t hash;
                            double startInBuffer;
                            double durationSeconds;
                            double timelineStart;
                        };
                        auto pending = std::make_shared<std::vector<PendingNote>>();
                        // The hash has to be of the note as the project holds
                        // it, which is renderClip's copy.  requestClip is the
                        // same notes with their starts shifted back to the
                        // beginning of the trimmed buffer, and hashing those
                        // would never match what the roll asks about -- so
                        // nothing would ever be drawn for a selection that
                        // begins more than a second into the song.
                        for (std::size_t index = 0; index < requestClip.notes.size()
                                                     && index < renderClip.notes.size(); ++index)
                        {
                            const auto& sent = requestClip.notes[index];
                            const auto& asHeld = renderClip.notes[index];
                            if (backend::isRestLyric(sent.label)) continue;
                            pending->push_back({ asHeld.id, noteRenderHash(asHeld),
                                                 sent.startSeconds, sent.durationSeconds,
                                                 clip.startSeconds + asHeld.startSeconds });
                        }
                        const auto measure = [state, pending, engine = this]
                            (backend::RenderedAudio result)
                        {
                            std::vector<UtauNoteWaveform> measured;
                            measured.reserve(pending->size());
                            for (const auto& note : *pending)
                            {
                                auto waveform = measureNoteWaveform(
                                    result.buffer, result.sampleRate,
                                    note.startInBuffer, note.durationSeconds);
                                waveform.noteId = note.id;
                                waveform.renderHash = note.hash;
                                waveform.startSeconds = note.timelineStart;
                                measured.push_back(std::move(waveform));
                            }
                            {
                                const juce::ScopedLock sliceGuard(state->sliceLock);
                                state->utauWaveforms = std::move(measured);
                            }
                            engine->utauWaveformGeneration.fetch_add(
                                1, std::memory_order_release);
                        };
                        auto request = makeUtauRequest(requestClip, track,
                            utauResamplerFile, project);
                        std::weak_ptr<RenderedClip> weakState(state);
                        request.progress = [weakState](double value)
                        {
                            if (const auto current = weakState.lock())
                                current->progress.store(static_cast<float>(
                                    juce::jlimit(0.0, 1.0, value)),
                                    std::memory_order_release);
                        };
                        // publish is declared const, and a copy captured from a
                        // const variable stays const however mutable this is.
                        auto forwardMeasured = [forward = publish, measure]
                            (backend::RenderedAudio result) mutable
                        {
                            // Measured before publishing, so a roll that sees
                            // the audio become ready finds the peaks already
                            // there.
                            if (result.buffer.getNumSamples() > 0
                                && result.sampleRate > 0.0)
                                measure(result);
                            forward(std::move(result));
                        };
                        if (nsfVoicebank)
                            // A voicebank track on NSF-HiFiGAN synthesises
                            // natively through the one NSF-HiFiGAN renderer,
                            // not the classic resampler.
                            renderService.renderNsfUtau(std::move(request),
                                hifiganModelDirectory, inferenceConfiguration,
                                std::move(forwardMeasured));
                        else
                            renderService.renderUtau(std::move(request),
                                std::move(forwardMeasured));
                    }
                    else
                        renderService.renderMld5File(makeRenderRequest(
                            clip, track, hifiganModelDirectory, inferenceConfiguration,
                            joinedStart, joinedEnd), publish);
                }
            }
            // Playback only needs clip timing/gain after the render request is
            // created.  Drop duplicated contours here so large MPD projects do
            // not keep a second full copy of every analysis point per clip.
            loaded->clip.notes.clear();
            loaded->clip.notes.shrink_to_fit();
            loadedClips.push_back(std::move(loaded));
        }
        for (auto& group : pendingGroups)
        {
            // A clip may have been dropped between grouping and loading (an
            // unreadable source), which would leave the slices misaligned.
            if (group.clips.size() < 2 || group.rendered.size() != group.clips.size()) continue;
            const auto mergedKey = mergedRenderKey(group.clips, track,
                hifiganModelDirectory, inferenceConfiguration);
            activeRenderKeys.insert(mergedKey);
            auto& phrase = renderCache[mergedKey];
            if (phrase == nullptr) phrase = std::make_shared<RenderedClip>();
            if (phrase->finished.load(std::memory_order_acquire)
                && !phrase->ready.load(std::memory_order_acquire))
            {
                phrase->scheduled.store(false, std::memory_order_release);
                phrase->finished.store(false, std::memory_order_release);
            }
            const auto sourceKey = group.clips.front()->sourceFile.getFullPathName().toStdString();
            const auto readerIt = readers.find(sourceKey);
            const auto fileRate = readerIt != readers.end() && readerIt->second != nullptr
                ? readerIt->second->sampleRate : outputSampleRate.load();
            std::vector<RenderedClip::SliceTarget> targets;
            targets.reserve(group.clips.size());
            auto targetOffset = 0.0;
            for (std::size_t index = 0; index < group.clips.size(); ++index)
            {
                const auto start = static_cast<int>(std::llround(targetOffset * fileRate));
                targetOffset += group.clips[index]->durationSeconds;
                const auto end = static_cast<int>(std::llround(targetOffset * fileRate));
                targets.push_back({ group.rendered[index], start, std::max(1, end - start) });
            }
            {
                const juce::ScopedLock sliceGuard(phrase->sliceLock);
                if (phrase->ready.load(std::memory_order_acquire))
                    for (const auto& target : targets)
                        sliceInto(*phrase, target);
                else
                    for (auto& target : targets)
                        phrase->pendingSlices.push_back(std::move(target));
            }
            if (!phrase->scheduled.exchange(true))
                renderService.renderMld5File(
                    mergedRequestFor(group.clips, track, hifiganModelDirectory,
                                     inferenceConfiguration),
                    [phrase, sliceInto](backend::RenderedAudio result) mutable
                    {
                        if (result.buffer.getNumSamples() <= 0 || result.sampleRate <= 0.0)
                        {
                            const juce::ScopedLock sliceGuard(phrase->sliceLock);
                            phrase->warning = result.warning.isNotEmpty() ? result.warning
                                : "NSF merged render returned no audio";
                            for (const auto& target : phrase->pendingSlices)
                            {
                                target.clip->warning = phrase->warning;
                                target.clip->progress.store(1.0f, std::memory_order_release);
                                target.clip->finished.store(true, std::memory_order_release);
                            }
                            phrase->pendingSlices.clear();
                            phrase->finished.store(true, std::memory_order_release);
                            return;
                        }
                        phrase->buffer = std::move(result.buffer);
                        phrase->sampleRate = result.sampleRate;
                        phrase->backend = std::move(result.backend);
                        phrase->warning = std::move(result.warning);
                        const juce::ScopedLock sliceGuard(phrase->sliceLock);
                        phrase->ready.store(true, std::memory_order_release);
                        phrase->finished.store(true, std::memory_order_release);
                        for (const auto& target : phrase->pendingSlices)
                            sliceInto(*phrase, target);
                        phrase->pendingSlices.clear();
                    });
        }
    }
    std::erase_if(renderCache, [&](const auto& item)
    {
        return !activeRenderKeys.contains(item.first);
    });
    // Which entries are current has changed, so which notes have peaks has too.
    utauWaveformGeneration.fetch_add(1, std::memory_order_release);
}

void AudioEngine::refreshUtauWaveforms()
{
    // Nothing has landed and no clip has moved: the snapshot in hand is still
    // the answer, and rebuilding it would copy every note's peaks for nothing.
    if (utauWaveformGeneration.load(std::memory_order_acquire)
        == utauWaveformSnapshotGeneration)
        return;
    utauWaveformSnapshotGeneration = utauWaveformGeneration.load(std::memory_order_acquire);
    refreshUtauWaveformSnapshot();
}

float AudioEngine::fadeEnvelope(const ClipData& clip, double localSeconds)
{
    auto gain = 1.0f;
    // Melodyne successive-join amplitude transitions are LINEAR complementary
    // fades (element amplitudeFadeIn/OutShapePow are always 1.0).  Each joined
    // element is back-to-back with its partner, so the fade-in of the following
    // element and the fade-out of the preceding element meet at the boundary
    // and together span the full MUSuccessiveJoin.amplitudeTransitionDuration.
    if (clip.crossfadeInSeconds > 1.0e-6)
    {
        const auto phase = static_cast<float>(juce::jlimit(0.0, 1.0,
            localSeconds / clip.crossfadeInSeconds));
        gain *= phase;
    }
    if (clip.crossfadeOutSeconds > 1.0e-6)
    {
        const auto phase = static_cast<float>(juce::jlimit(0.0, 1.0,
            (clip.durationSeconds - localSeconds) / clip.crossfadeOutSeconds));
        gain *= phase;
    }
    if (clip.fadeInSeconds > 1.0e-6)
    {
        const auto phase = static_cast<float>(juce::jlimit(0.0, 1.0,
            localSeconds / clip.fadeInSeconds));
        gain *= phase * phase * (3.0f - 2.0f * phase);
    }
    if (clip.fadeOutSeconds > 1.0e-6)
    {
        const auto phase = static_cast<float>(juce::jlimit(0.0, 1.0,
            (clip.durationSeconds - localSeconds) / clip.fadeOutSeconds));
        gain *= phase * phase * (3.0f - 2.0f * phase);
    }
    return gain;
}

void AudioEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (!playing.load() || info.buffer == nullptr || info.numSamples <= 0) return;

    const auto sampleRate = outputSampleRate.load();
    const auto blockStartSample = timelineSample.load();
    const auto blockStart = static_cast<double>(blockStartSample) / sampleRate;
    const auto blockEnd = static_cast<double>(blockStartSample + info.numSamples) / sampleRate;
    const juce::ScopedReadLock guard(renderLock);

    for (const auto& [_, meter] : trackMeters)
        meter->store(meter->load(std::memory_order_relaxed) * 0.88f, std::memory_order_relaxed);

    if (auditionMode.load() && auditionReader != nullptr)
    {
        const auto readerRate = auditionReader->sampleRate;
        const auto firstSourcePosition = blockStart * readerRate;
        if (firstSourcePosition >= static_cast<double>(auditionReader->lengthInSamples))
        {
            playing.store(false);
            return;
        }
        const auto availableSeconds = (static_cast<double>(auditionReader->lengthInSamples)
                                       - firstSourcePosition) / readerRate;
        const auto outputCount = juce::jlimit(0, info.numSamples,
            static_cast<int>(std::ceil(availableSeconds * sampleRate)));
        const auto lastSourcePosition = firstSourcePosition
            + static_cast<double>(std::max(0, outputCount - 1)) * readerRate / sampleRate;
        const auto sourceBase = static_cast<juce::int64>(std::floor(firstSourcePosition));
        const auto sourceCount = std::max(2, static_cast<int>(std::ceil(lastSourcePosition))
                                            - static_cast<int>(sourceBase) + 2);
        const auto sourceChannels = juce::jlimit(1, 2, static_cast<int>(auditionReader->numChannels));
        auditionScratch.setSize(sourceChannels, sourceCount, false, false, true);
        auditionScratch.clear();
        auditionReader->read(&auditionScratch, 0, sourceCount, sourceBase, true, sourceChannels > 1);
        for (int outputOffset = 0; outputOffset < outputCount; ++outputOffset)
        {
            const auto sourcePosition = firstSourcePosition
                + static_cast<double>(outputOffset) * readerRate / sampleRate - static_cast<double>(sourceBase);
            const auto leftIndex = juce::jlimit(0, sourceCount - 1, static_cast<int>(std::floor(sourcePosition)));
            const auto rightIndex = juce::jmin(sourceCount - 1, leftIndex + 1);
            const auto fraction = static_cast<float>(sourcePosition - std::floor(sourcePosition));
            const auto interpolate = [&, leftIndex, rightIndex, fraction](int channel)
            {
                const auto* samples = auditionScratch.getReadPointer(channel);
                return samples[leftIndex] + (samples[rightIndex] - samples[leftIndex]) * fraction;
            };
            const auto left = interpolate(0);
            const auto right = sourceChannels > 1 ? interpolate(1) : left;
            info.buffer->setSample(0, info.startSample + outputOffset, left);
            if (info.buffer->getNumChannels() > 1)
                info.buffer->setSample(1, info.startSample + outputOffset, right);
        }
        timelineSample.fetch_add(outputCount);
        if (outputCount < info.numSamples) playing.store(false);
        if (!offlineRendering.load(std::memory_order_relaxed)) sendChangeMessage();
        return;
    }

    std::unordered_map<std::string, std::vector<float>> overlapEnvelopeSums;
    std::unordered_map<std::string, std::vector<unsigned short>> overlapCounts;
    for (const auto& loaded : loadedClips)
    {
        if (!exportTrackFilter.empty() && loaded->trackId != exportTrackFilter) continue;
        if (!loaded->smoothOverlaps) continue;
        const auto& clip = loaded->clip;
        const auto clipEnd = clip.startSeconds + clip.durationSeconds;
        const auto overlapStart = std::max(blockStart, clip.startSeconds);
        const auto overlapEnd = std::min(blockEnd, clipEnd);
        if (overlapEnd <= overlapStart) continue;
        auto& sums = overlapEnvelopeSums[loaded->trackId];
        auto& counts = overlapCounts[loaded->trackId];
        if (sums.empty()) sums.assign(static_cast<std::size_t>(info.numSamples), 0.0f);
        if (counts.empty()) counts.assign(static_cast<std::size_t>(info.numSamples), 0);
        const auto begin = juce::jlimit(0, info.numSamples,
            static_cast<int>(std::floor((overlapStart - blockStart) * sampleRate)));
        const auto end = juce::jlimit(begin, info.numSamples,
            static_cast<int>(std::ceil((overlapEnd - blockStart) * sampleRate)));
        for (auto output = begin; output < end; ++output)
        {
            const auto absoluteSeconds = blockStart + static_cast<double>(output) / sampleRate;
            sums[static_cast<std::size_t>(output)] += fadeEnvelope(
                clip, absoluteSeconds - clip.startSeconds);
            ++counts[static_cast<std::size_t>(output)];
        }
    }

    const auto smoothedGain = [&](const LoadedClip& loaded, double localSeconds,
                                  int blockOffset)
    {
        auto envelope = fadeEnvelope(loaded.clip, localSeconds);
        if (loaded.smoothOverlaps)
        {
            const auto sums = overlapEnvelopeSums.find(loaded.trackId);
            const auto counts = overlapCounts.find(loaded.trackId);
            if (sums != overlapEnvelopeSums.end() && counts != overlapCounts.end()
                && counts->second[static_cast<std::size_t>(blockOffset)] > 1)
            {
                const auto total = sums->second[static_cast<std::size_t>(blockOffset)];
                if (total > 1.0e-6f) envelope /= std::max(0.5f, total);
            }
        }
        return loaded.clip.gain * envelope;
    };

    for (auto& loaded : loadedClips)
    {
        // One file per track: everything else is passed over rather than
        // silenced, so the overlap sums above stay this track's own.
        if (!exportTrackFilter.empty() && loaded->trackId != exportTrackFilter) continue;
        const auto& clip = loaded->clip;
        const auto clipEnd = clip.startSeconds + clip.durationSeconds;
        const auto overlapStart = std::max(blockStart, clip.startSeconds);
        const auto overlapEnd = std::min(blockEnd, clipEnd);
        if (overlapEnd <= overlapStart) continue;

        const auto outputBegin = juce::jlimit(0, info.numSamples,
            static_cast<int>(std::floor((overlapStart - blockStart) * sampleRate)));
        const auto outputEnd = juce::jlimit(outputBegin, info.numSamples,
            static_cast<int>(std::ceil((overlapEnd - blockStart) * sampleRate)));
        const auto outputCount = outputEnd - outputBegin;
        if (outputCount <= 0) continue;

        auto renderedState = loaded->rendered != nullptr
                && loaded->rendered->ready.load(std::memory_order_acquire)
            ? loaded->rendered : loaded->fallbackRendered;
        if (renderedState != nullptr
            && renderedState->ready.load(std::memory_order_acquire)
            && renderedState->buffer.getNumSamples() > 0)
        {
            const auto& rendered = renderedState->buffer;
            const auto renderedRate = renderedState->sampleRate;
            const auto renderedChannels = juce::jlimit(1, 2, rendered.getNumChannels());
            const auto firstPosition = (blockStart + static_cast<double>(outputBegin) / sampleRate
                                        - clip.startSeconds
                                        - renderedState->timelineOffsetSeconds) * renderedRate;
            const auto [leftPan, rightPan] = panGains(loaded->trackPan, renderedChannels == 1);
            for (int outputOffset = 0; outputOffset < outputCount; ++outputOffset)
            {
                const auto position = firstPosition
                    + static_cast<double>(outputOffset) * renderedRate / sampleRate;
                if (position < 0.0 || position >= static_cast<double>(rendered.getNumSamples()))
                    continue;
                const auto leftIndex = juce::jlimit(0, rendered.getNumSamples() - 1,
                                                     static_cast<int>(std::floor(position)));
                const auto rightIndex = std::min(rendered.getNumSamples() - 1, leftIndex + 1);
                const auto fraction = static_cast<float>(position - std::floor(position));
                const auto interpolate = [&](int channel)
                {
                    const auto* samples = rendered.getReadPointer(channel);
                    return samples[leftIndex] + (samples[rightIndex] - samples[leftIndex]) * fraction;
                };
                const auto sourceLeft = interpolate(0);
                const auto sourceRight = renderedChannels > 1 ? interpolate(1) : sourceLeft;
                const auto absoluteSeconds = blockStart
                    + static_cast<double>(outputBegin + outputOffset) / sampleRate;
                const auto gain = smoothedGain(*loaded,
                    absoluteSeconds - clip.startSeconds, outputBegin + outputOffset)
                    * loaded->trackGain;
                const auto destination = info.startSample + outputBegin + outputOffset;
                const auto renderedLeft = sourceLeft * gain * leftPan;
                const auto renderedRight = sourceRight * gain * rightPan;
                info.buffer->addSample(0, destination, renderedLeft);
                if (info.buffer->getNumChannels() > 1)
                    info.buffer->addSample(1, destination, renderedRight);
                if (loaded->meter != nullptr)
                {
                    const auto peak = std::max(std::abs(renderedLeft), std::abs(renderedRight));
                    loaded->meter->store(std::max(loaded->meter->load(std::memory_order_relaxed), peak),
                                         std::memory_order_relaxed);
                }
            }
            continue;
        }

        // MIDI-backed UTAU clips have no AudioFormatReader.  Until their
        // asynchronous phrase render is ready they are intentionally silent.
        if (loaded->reader == nullptr) continue;

        const auto readerRate = loaded->reader->sampleRate;
        const auto sourceDuration = clip.sourceDurationSeconds > 1.0e-9
            ? clip.sourceDurationSeconds : clip.durationSeconds;
        const auto playbackRate = sourceDuration / std::max(0.001, clip.durationSeconds);
        const auto firstSourcePosition = clip.sourceOffsetSeconds * readerRate
            + (blockStart + static_cast<double>(outputBegin) / sampleRate - clip.startSeconds)
                * playbackRate * readerRate;
        const auto lastSourcePosition = firstSourcePosition
            + static_cast<double>(outputCount - 1) * playbackRate * readerRate / sampleRate;
        const auto sourceBase = static_cast<juce::int64>(std::floor(firstSourcePosition));
        const auto sourceCount = static_cast<int>(std::ceil(lastSourcePosition))
            - static_cast<int>(sourceBase) + 2;
        if (sourceBase < 0 || sourceCount <= 1) continue;

        const auto sourceChannels = juce::jlimit(1, 2, static_cast<int>(loaded->reader->numChannels));
        loaded->scratch.setSize(sourceChannels, sourceCount, false, false, true);
        loaded->scratch.clear();
        loaded->reader->read(&loaded->scratch, 0, sourceCount, sourceBase, true, sourceChannels > 1);

        const auto [leftPan, rightPan] = panGains(loaded->trackPan, sourceChannels == 1);
        for (int outputOffset = 0; outputOffset < outputCount; ++outputOffset)
        {
            const auto sourcePosition = firstSourcePosition
                + static_cast<double>(outputOffset) * playbackRate * readerRate / sampleRate
                - static_cast<double>(sourceBase);
            const auto leftIndex = juce::jlimit(0, sourceCount - 1, static_cast<int>(std::floor(sourcePosition)));
            const auto rightIndex = juce::jmin(sourceCount - 1, leftIndex + 1);
            const auto fraction = static_cast<float>(sourcePosition - std::floor(sourcePosition));
            const auto interpolate = [&, leftIndex, rightIndex, fraction](int channel)
            {
                const auto* samples = loaded->scratch.getReadPointer(channel);
                return samples[leftIndex] + (samples[rightIndex] - samples[leftIndex]) * fraction;
            };
            const auto sourceLeft = interpolate(0);
            const auto sourceRight = sourceChannels > 1 ? interpolate(1) : sourceLeft;
            const auto absoluteSeconds = blockStart + static_cast<double>(outputBegin + outputOffset) / sampleRate;
            const auto gain = smoothedGain(*loaded,
                absoluteSeconds - clip.startSeconds, outputBegin + outputOffset)
                * loaded->trackGain;
            const auto destination = info.startSample + outputBegin + outputOffset;
            const auto renderedLeft = sourceLeft * gain * leftPan;
            const auto renderedRight = sourceRight * gain * rightPan;
            info.buffer->addSample(0, destination, renderedLeft);
            if (info.buffer->getNumChannels() > 1)
                info.buffer->addSample(1, destination, renderedRight);
            if (loaded->meter != nullptr)
            {
                const auto peak = std::max(std::abs(renderedLeft), std::abs(renderedRight));
                loaded->meter->store(std::max(loaded->meter->load(std::memory_order_relaxed), peak),
                                     std::memory_order_relaxed);
            }
        }
    }

    // A Melodyne project may contain several overlapping elements whose
    // individual gains are valid but whose sum exceeds full scale.  Apply one
    // linked sample envelope (fast attack, slow release) after mixing so block
    // boundaries cannot become gain steps or digital crack/burst artefacts.
    auto limiterGain = masterLimiterGain.load(std::memory_order_relaxed);
    const auto attackMemory = std::exp(-1.0f / static_cast<float>(
        std::max(1.0, sampleRate) * 0.0005));
    const auto releaseMemory = std::exp(-1.0f / static_cast<float>(
        std::max(1.0, sampleRate) * 0.18));
    for (int index = 0; index < info.numSamples; ++index)
    {
        auto linkedPeak = 0.0f;
        for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
            linkedPeak = std::max(linkedPeak, std::abs(info.buffer->getSample(
                channel, info.startSample + index)));
        const auto target = linkedPeak > 0.98f ? 0.98f / linkedPeak : 1.0f;
        const auto memory = target < limiterGain ? attackMemory : releaseMemory;
        limiterGain = target + (limiterGain - target) * memory;
        for (int channel = 0; channel < info.buffer->getNumChannels(); ++channel)
        {
            auto value = info.buffer->getSample(channel, info.startSample + index) * limiterGain;
            const auto magnitude = std::abs(value);
            if (magnitude > 0.98f)
                value = std::copysign(0.98f + 0.02f
                    * std::tanh((magnitude - 0.98f) / 0.02f), value);
            info.buffer->setSample(channel, info.startSample + index, value);
        }
    }
    masterLimiterGain.store(limiterGain, std::memory_order_relaxed);

    const auto nextSample = timelineSample.fetch_add(info.numSamples) + info.numSamples;
    const auto reached = static_cast<double>(nextSample) / sampleRate;
    const auto until = playUntilSeconds.load();
    if (reached >= projectDurationSeconds.load() || (until > 0.0 && reached >= until))
        playing.store(false);
    if (!offlineRendering.load(std::memory_order_relaxed)) sendChangeMessage();
}

void AudioEngine::play()
{
    auto duration = projectDurationSeconds.load();
    {
        const juce::ScopedReadLock guard(renderLock);
        if (auditionMode.load() && auditionReader != nullptr)
            duration = static_cast<double>(auditionReader->lengthInSamples) / auditionReader->sampleRate;
    }
    if (duration > 0.0 && position() >= duration)
        setPosition(0.0);
    playing.store(true);
    sendChangeMessage();
}

void AudioEngine::setPlayUntil(double seconds)
{
    playUntilSeconds.store(std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0);
}

void AudioEngine::stop()
{
    playing.store(false);
    masterLimiterGain.store(1.0f, std::memory_order_relaxed);
    {
        const juce::ScopedReadLock guard(renderLock);
        for (const auto& [_, meter] : trackMeters)
            meter->store(0.0f, std::memory_order_relaxed);
    }
    sendChangeMessage();
}

void AudioEngine::setPosition(double seconds)
{
    timelineSample.store(static_cast<juce::int64>(std::max(0.0, seconds) * outputSampleRate.load()));
    sendChangeMessage();
}

double AudioEngine::position() const
{
    return static_cast<double>(timelineSample.load()) / outputSampleRate.load();
}

float AudioEngine::trackPeak(const juce::String& trackId) const
{
    const juce::ScopedReadLock guard(renderLock);
    if (const auto found = trackMeters.find(trackId.toStdString()); found != trackMeters.end())
        return found->second->load(std::memory_order_relaxed);
    return 0.0f;
}

std::optional<double> AudioEngine::renderProgress() const
{
    const juce::ScopedReadLock guard(renderLock);
    int total = 0;
    auto completed = 0.0;
    auto anyUnfinished = false;
    for (const auto& loaded : loadedClips)
        if (loaded->rendered != nullptr)
        {
            ++total;
            const auto done = loaded->rendered->finished.load(std::memory_order_acquire);
            if (!done) anyUnfinished = true;
            completed += done ? 1.0 : static_cast<double>(loaded->rendered->progress.load(
                std::memory_order_acquire));
        }
    // Reporting no progress means finished, so a clip whose progress reached
    // 1.0 before its result was published must not count: callers that wait
    // for this and then export were told to go ahead too early and failed
    // with "pre-render is still running".
    if (total == 0 || (!anyUnfinished && completed >= static_cast<double>(total)))
        return std::nullopt;
    return juce::jlimit(0.0, 0.999, completed / static_cast<double>(total));
}

bool AudioEngine::hasPlayableRenderedAudio() const
{
    const juce::ScopedReadLock guard(renderLock);
    for (const auto& loaded : loadedClips)
        for (const auto& rendered : { loaded->rendered, loaded->fallbackRendered })
            if (rendered != nullptr
                && rendered->ready.load(std::memory_order_acquire)
                && rendered->buffer.getNumSamples() > 0)
                return true;
    return false;
}

bool AudioEngine::hasCurrentRenderedAudio() const
{
    const juce::ScopedReadLock guard(renderLock);
    for (const auto& loaded : loadedClips)
        if (loaded->rendered != nullptr
            && loaded->rendered->ready.load(std::memory_order_acquire)
            && loaded->rendered->buffer.getNumSamples() > 0
            && loaded->rendered->lastAudibleSample >= loaded->rendered->firstAudibleSample)
            return true;
    return false;
}

bool AudioEngine::rewindToFirstPlayableRenderedAudio(double leadInSeconds)
{
    std::optional<double> firstAudibleSeconds;
    {
        const juce::ScopedReadLock guard(renderLock);
        for (const auto& loaded : loadedClips)
        {
            const auto rendered = loaded->rendered != nullptr
                    && loaded->rendered->ready.load(std::memory_order_acquire)
                ? loaded->rendered : loaded->fallbackRendered;
            if (rendered == nullptr
                || !rendered->ready.load(std::memory_order_acquire)
                || rendered->sampleRate <= 0.0
                || rendered->lastAudibleSample < rendered->firstAudibleSample)
                continue;
            const auto absolute = loaded->clip.startSeconds
                + rendered->timelineOffsetSeconds
                + static_cast<double>(rendered->firstAudibleSample) / rendered->sampleRate;
            firstAudibleSeconds = firstAudibleSeconds
                ? std::min(*firstAudibleSeconds, absolute) : absolute;
        }
    }
    if (!firstAudibleSeconds) return false;
    setPosition(std::max(0.0, *firstAudibleSeconds - std::max(0.0, leadInSeconds)));
    return true;
}

juce::String AudioEngine::activeRenderBackends() const
{
    const juce::ScopedReadLock guard(renderLock);
    juce::StringArray names;
    for (const auto& loaded : loadedClips)
        if (loaded->rendered != nullptr
            && loaded->rendered->ready.load(std::memory_order_acquire)
            && loaded->rendered->backend.isNotEmpty())
            names.addIfNotAlreadyThere(loaded->rendered->backend);
    names.sort(true);
    return names.joinIntoString(" + ");
}

juce::String AudioEngine::activeRenderWarnings() const
{
    const juce::ScopedReadLock guard(renderLock);
    juce::StringArray warnings;
    for (const auto& loaded : loadedClips)
        if (loaded->rendered != nullptr
            && loaded->rendered->finished.load(std::memory_order_acquire)
            && loaded->rendered->warning.isNotEmpty())
            warnings.addIfNotAlreadyThere(loaded->rendered->warning);
    return warnings.joinIntoString("; ");
}

juce::StringArray AudioEngine::renderCapabilityWarnings(const ProjectData& project)
{
    // The UTAU resampler path (makeUtauRequest) is the only backend that reads
    // oto flags, consonant velocity, preutterance/overlap overrides, STP and
    // the four-region flag split.  Every other backend renders from analysed
    // source audio through the common request, which already carries pitch,
    // vibrato, amplitude, formant, tension, breath, gain, drift and modulation
    // -- so those edits are honoured everywhere and are never warned about.
    juce::StringArray warnings;
    for (const auto& track : project.tracks)
    {
        if (track.pitchAlgorithm == PitchAlgorithm::utau) continue;
        auto usesFlags = track.utauGlobalFlags.trim().isNotEmpty();
        auto usesFlagCurve = false;
        auto usesConsonantVelocity = false;
        auto usesTimingOverride = false;
        auto usesStp = false;
        auto usesRegionFlags = false;
        for (const auto& clip : track.clips)
            for (const auto& note : clip.notes)
            {
                usesFlags = usesFlags || note.utauFlags.trim().isNotEmpty();
                usesFlagCurve = usesFlagCurve || note.utauFlagCurveEnabled;
                usesConsonantVelocity = usesConsonantVelocity
                    || (note.utauConsonantVelocity != inheritedUtauConsonantVelocity
                        && note.utauConsonantVelocity != 100);
                usesTimingOverride = usesTimingOverride
                    || note.utauPreutteranceOverrideEnabled
                    || note.utauOverlapOverrideEnabled;
                usesStp = usesStp || std::abs(note.utauStpSeconds) > 1.0e-6;
                usesRegionFlags = usesRegionFlags || note.utauFlagSplit
                    || note.utauRegionFlags1.isNotEmpty()
                    || note.utauRegionFlags2.isNotEmpty()
                    || note.utauRegionFlags3.isNotEmpty()
                    || note.utauRegionFlags4.isNotEmpty();
            }
        const auto note = [&](const juce::String& feature)
        {
            warnings.addIfNotAlreadyThere("[" + track.name + "] " + feature);
        };
        if (usesFlags || usesFlagCurve)
            note(juce::String::fromUTF8("UTAU flags 仅在 UTAU 渲染后端生效，当前后端将忽略"));
        if (usesConsonantVelocity)
            note(juce::String::fromUTF8("辅音速度仅 UTAU 后端生效；其他后端用起音时间映射近似"));
        if (usesTimingOverride)
            note(juce::String::fromUTF8("先行/交叠覆盖仅 UTAU 后端生效"));
        if (usesStp)
            note(juce::String::fromUTF8("STP 仅 UTAU 后端生效"));
        if (usesRegionFlags)
            note(juce::String::fromUTF8("分区 flag 仅 UTAU 后端生效"));
    }
    return warnings;
}

bool AudioEngine::exportWav(const juce::File& file, juce::String& error,
                            const juce::String& trackId,
                            double fromSeconds, double toSeconds)
{
    const auto failure = activeRenderWarnings();
    if (failure.isNotEmpty())
    {
        error = failure;
        return false;
    }
    {
        const juce::ScopedReadLock guard(renderLock);
        for (const auto& loaded : loadedClips)
            if (loaded->rendered != nullptr
                && !loaded->rendered->finished.load(std::memory_order_acquire))
            {
                error = "Pre-render is still running";
                return false;
            }
    }

    file.deleteFile();
    auto stream = file.createOutputStream();
    if (stream == nullptr)
    {
        error = "Could not create " + file.getFullPathName();
        return false;
    }
    const auto sampleRate = juce::jlimit(8'000.0, 192'000.0, outputSampleRate.load());
    juce::WavAudioFormat format;
    // Keep project exports stereo/24-bit.  Panning and track balance are part
    // of the mix, and downstream DAWs expect the full-width stem rather than a
    // forced mono fold-down.
    auto writer = std::unique_ptr<juce::AudioFormatWriter>(format.createWriterFor(
        stream.release(), sampleRate, 2, 24, {}, 0));
    if (writer == nullptr)
    {
        error = "Could not create WAV writer";
        return false;
    }

    stop();
    {
        const juce::ScopedWriteLock guard(renderLock);
        exportTrackFilter = trackId.toStdString();
    }
    deviceManager.removeAudioCallback(&sourcePlayer);
    const auto previousPosition = timelineSample.load();
    const auto previousAudition = auditionMode.exchange(false);
    // Playing a selection leaves a stop-here mark behind, and the offline pass
    // runs through the same block callback that honours it: past that moment
    // the transport switched itself off and every remaining block came out
    // empty, so the file was full length with only its opening filled in.
    // An export is not playback and has no business stopping early.
    const auto previousPlayUntil = playUntilSeconds.exchange(0.0);
    offlineRendering.store(true, std::memory_order_release);
    const auto songSeconds = projectDurationSeconds.load();
    const auto fromClamped = juce::jlimit(0.0, std::max(0.0, songSeconds), fromSeconds);
    const auto toClamped = toSeconds > fromClamped
        ? std::min(toSeconds, songSeconds) : songSeconds;
    timelineSample.store(static_cast<juce::int64>(std::llround(fromClamped * sampleRate)));
    masterLimiterGain.store(1.0f, std::memory_order_relaxed);
    playing.store(true);

    constexpr int blockSize = 2048;
    // Rendered and written in stereo so pan automation and track placement are
    // preserved in the exported file.
    juce::AudioBuffer<float> block(2, blockSize);
    const auto totalSamples = static_cast<juce::int64>(std::ceil(
        std::max(0.0, toClamped - fromClamped) * sampleRate));
    auto written = juce::int64(0);
    auto ok = true;
    while (written < totalSamples)
    {
        const auto count = static_cast<int>(std::min<juce::int64>(blockSize, totalSamples - written));
        block.clear();
        juce::AudioSourceChannelInfo info(&block, 0, count);
        getNextAudioBlock(info);
        if (!writer->writeFromAudioSampleBuffer(block, 0, count))
        {
            ok = false;
            error = "WAV write failed";
            break;
        }
        written += count;
    }

    writer.reset();
    playing.store(false);
    {
        const juce::ScopedWriteLock guard(renderLock);
        exportTrackFilter.clear();
    }
    timelineSample.store(previousPosition);
    playUntilSeconds.store(previousPlayUntil);
    auditionMode.store(previousAudition);
    offlineRendering.store(false, std::memory_order_release);
    deviceManager.addAudioCallback(&sourcePlayer);
    sendChangeMessage();
    return ok;
}
}
