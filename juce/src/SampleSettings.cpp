#include "SampleSettings.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <memory>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#elif defined(HACHI_HAS_ICONV)
 #include <iconv.h>
#endif

namespace hachi
{
namespace
{
juce::String csvEscape(const juce::String& value)
{
    if (!value.containsAnyOf(",\"\r\n")) return value;
    return "\"" + value.replace("\"", "\"\"") + "\"";
}

juce::StringArray splitCsv(const juce::String& line)
{
    juce::StringArray fields;
    juce::String current;
    auto quoted = false;
    for (int index = 0; index < line.length(); ++index)
    {
        const auto character = line[index];
        if (character == '"')
        {
            if (quoted && index + 1 < line.length() && line[index + 1] == '"')
            {
                current += '"';
                ++index;
            }
            else quoted = !quoted;
        }
        else if (character == ',' && !quoted)
        {
            fields.add(current);
            current.clear();
        }
        else current += character;
    }
    fields.add(current);
    return fields;
}

double number(const juce::StringArray& values, int index, double fallback)
{
    if (index >= values.size() || values[index].trim().isEmpty()) return fallback;
    return values[index].getDoubleValue();
}

juce::String decodeOtoText(const juce::File& file)
{
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() == 0) return {};
    const auto* data = static_cast<const char*>(bytes.getData());
    auto size = bytes.getSize();
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xef
        && static_cast<unsigned char>(data[1]) == 0xbb
        && static_cast<unsigned char>(data[2]) == 0xbf)
    {
        data += 3;
        size -= 3;
    }
    if (juce::CharPointer_UTF8::isValidString(data, static_cast<int>(size)))
        return juce::String::fromUTF8(data, static_cast<int>(size));

#if JUCE_WINDOWS
    const auto wideLength = MultiByteToWideChar(932, 0, data, static_cast<int>(size), nullptr, 0);
    if (wideLength > 0)
    {
        std::vector<wchar_t> wide(static_cast<std::size_t>(wideLength + 1), 0);
        if (MultiByteToWideChar(932, 0, data, static_cast<int>(size), wide.data(), wideLength) > 0)
            return juce::String(wide.data());
    }
#elif defined(HACHI_HAS_ICONV)
    auto decoder = iconv_open("UTF-8", "CP932");
    if (decoder == reinterpret_cast<iconv_t>(-1)) decoder = iconv_open("UTF-8", "SHIFT-JIS");
    if (decoder != reinterpret_cast<iconv_t>(-1))
    {
        std::vector<char> decoded(size * 4 + 4, 0);
        auto* input = const_cast<char*>(data);
        auto inputLeft = size;
        auto* output = decoded.data();
        auto outputLeft = decoded.size() - 1;
        const auto result = iconv(decoder, &input, &inputLeft, &output, &outputLeft);
        iconv_close(decoder);
        if (result != static_cast<std::size_t>(-1))
            return juce::String::fromUTF8(decoded.data(),
                static_cast<int>(decoded.size() - outputLeft - 1));
    }
#endif
    return juce::String::fromUTF8(data, static_cast<int>(size));
}

juce::String otoNumber(double value)
{
    if (std::abs(value) < 0.0005) value = 0.0;
    auto text = juce::String(value, 3);
    while (text.containsChar('.') && text.endsWithChar('0'))
        text = text.dropLastCharacters(1);
    if (text.endsWithChar('.')) text = text.dropLastCharacters(1);
    return text;
}

juce::String otoEntryText(const VoicebankOtoEntry& entry)
{
    return entry.sourceName + "=" + entry.alias + ","
        + otoNumber(entry.offsetMs) + "," + otoNumber(entry.consonantMs) + ","
        + otoNumber(entry.cutoffMs) + "," + otoNumber(entry.preutteranceMs) + ","
        + otoNumber(entry.overlapMs);
}

bool writeOtoTextPreservingEncoding(const juce::File& file,
                                    const juce::String& output,
                                    juce::String& error)
{
    juce::MemoryBlock originalBytes;
    if (!file.loadFileAsData(originalBytes) || originalBytes.getSize() == 0)
    {
        error = "Could not read " + file.getFullPathName();
        return false;
    }
    const auto* raw = static_cast<const char*>(originalBytes.getData());
    const auto rawSize = originalBytes.getSize();
    const auto hasUtf8Bom = rawSize >= 3
        && static_cast<unsigned char>(raw[0]) == 0xef
        && static_cast<unsigned char>(raw[1]) == 0xbb
        && static_cast<unsigned char>(raw[2]) == 0xbf;
    const auto* utf8Start = raw + (hasUtf8Bom ? 3 : 0);
    const auto utf8Size = rawSize - (hasUtf8Bom ? 3 : 0);
    const auto wasUtf8 = hasUtf8Bom
        || juce::CharPointer_UTF8::isValidString(utf8Start, static_cast<int>(utf8Size));

    juce::MemoryBlock encoded;
    if (wasUtf8)
    {
        if (hasUtf8Bom)
        {
            const unsigned char bom[] { 0xef, 0xbb, 0xbf };
            encoded.append(bom, sizeof(bom));
        }
        encoded.append(output.toRawUTF8(), output.getNumBytesAsUTF8());
    }
#if JUCE_WINDOWS
    else
    {
        const std::wstring wide(output.toWideCharPointer());
        const auto byteCount = WideCharToMultiByte(932, 0, wide.data(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (byteCount <= 0)
        {
            error = "Could not encode Shift-JIS oto file";
            return false;
        }
        std::vector<char> bytes(static_cast<std::size_t>(byteCount));
        if (WideCharToMultiByte(932, 0, wide.data(), static_cast<int>(wide.size()),
                                bytes.data(), byteCount, nullptr, nullptr) <= 0)
        {
            error = "Could not encode Shift-JIS oto file";
            return false;
        }
        encoded.append(bytes.data(), bytes.size());
    }
#else
    else
    {
        encoded.append(output.toRawUTF8(), output.getNumBytesAsUTF8());
    }
#endif
    if (!file.replaceWithData(encoded.getData(), encoded.getSize()))
    {
        error = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}
}

juce::File SampleSettings::sidecarFor(const juce::File& audio)
{
    // Native rewrite uses the requested interoperable name while still
    // accepting the main-branch .hachi.csv file during migration.
    return juce::File(audio.getFullPathName() + ".hjm.csv");
}

std::vector<SampleRegionSetting> SampleSettings::loadOrDerive(const juce::File& audio,
                                                              const ProjectData& project)
{
    auto sidecar = sidecarFor(audio);
    if (!sidecar.existsAsFile())
    {
        const juce::File legacy(audio.getFullPathName() + ".hachi.csv");
        if (legacy.existsAsFile()) sidecar = legacy;
    }
    std::vector<SampleRegionSetting> rows;
    if (sidecar.existsAsFile())
    {
        auto lines = juce::StringArray::fromLines(sidecar.loadFileAsString());
        for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
        {
            const auto line = lines[lineIndex].trim();
            if (line.isEmpty() || line.startsWithIgnoreCase("name,")) continue;
            const auto values = splitCsv(line);
            if (values.size() < 6) continue;
            SampleRegionSetting row;
            row.name = values[0].trim();
            row.regionStartSeconds = number(values, 1, 0.0);
            row.regionEndSeconds = number(values, 2, row.regionStartSeconds + 0.5);
            row.alignmentSeconds = number(values, 3, row.regionStartSeconds);
            row.fixedDurationSeconds = number(values, 4, 0.0);
            row.relativePitchCents = number(values, 5, 0.0);
            row.melodyneData = number(values, 6, 0.0) >= 0.5;
            row.melodynePitchCenterCents = number(values, 7, 0.0);
            row.melodyneOriginalPitchCenterCents = number(values, 8, 0.0);
            row.melodynePitchDrift = number(values, 9, 1.0);
            row.melodynePitchModulation = number(values, 10, 1.0);
            row.melodyneTransitionSeconds = number(values, 11, 0.0);
            row.melodyneFormantCents = number(values, 12, 0.0);
            row.melodyneAmplitude = number(values, 13, 1.0);
            row.melodyneSibilantBalance = number(values, 14, 0.0);
            row.melodyneAttackSeconds = number(values, 15, 0.0);
            row.melodyneDecayElongation = number(values, 16, 0.0);
            row.overlapSeconds = number(values, 17, 0.0);
            if (row.regionEndSeconds > row.regionStartSeconds) rows.push_back(std::move(row));
        }
    }
    if (!rows.empty()) return rows;

    struct Positioned { const ClipData* clip; const NoteData* note; double sourceStart; };
    std::vector<Positioned> notes;
    for (const auto& track : project.tracks)
        for (const auto& clip : track.clips)
            if (clip.sourceFile == audio)
                for (const auto& note : clip.notes)
                {
                    const auto ratio = clip.durationSeconds > 1.0e-9
                        ? clip.sourceDurationSeconds / clip.durationSeconds : 1.0;
                    notes.push_back({ &clip, &note,
                        clip.sourceOffsetSeconds + note.startSeconds * ratio });
                }
    std::stable_sort(notes.begin(), notes.end(), [](const auto& left, const auto& right)
    {
        return left.sourceStart < right.sourceStart;
    });
    // The same source may be placed many times on the timeline.  Wrench mode
    // describes the source file, so equal source regions are one HJM row rather
    // than one row per project placement.
    std::vector<Positioned> uniqueNotes;
    uniqueNotes.reserve(notes.size());
    for (const auto& item : notes)
    {
        const auto ratio = item.clip->durationSeconds > 1.0e-9
            ? item.clip->sourceDurationSeconds / item.clip->durationSeconds : 1.0;
        const auto sourceDuration = item.note->durationSeconds * ratio;
        const auto duplicate = std::any_of(uniqueNotes.begin(), uniqueNotes.end(),
            [&](const auto& existing)
            {
                const auto existingRatio = existing.clip->durationSeconds > 1.0e-9
                    ? existing.clip->sourceDurationSeconds / existing.clip->durationSeconds : 1.0;
                return std::abs(existing.sourceStart - item.sourceStart) < 0.0005
                    && std::abs(existing.note->durationSeconds * existingRatio - sourceDuration)
                        < 0.0005;
            });
        if (!duplicate) uniqueNotes.push_back(item);
    }
    notes = std::move(uniqueNotes);
    for (std::size_t index = 0; index < notes.size(); ++index)
    {
        const auto& item = notes[index];
        const auto ratio = item.clip->durationSeconds > 1.0e-9
            ? item.clip->sourceDurationSeconds / item.clip->durationSeconds : 1.0;
        SampleRegionSetting row;
        row.name = item.note->label.isNotEmpty()
            ? item.note->label : "note " + juce::String(index + 1);
        row.regionStartSeconds = std::max(0.0, item.sourceStart);
        row.regionEndSeconds = std::max(row.regionStartSeconds + 0.001,
            row.regionStartSeconds + item.note->durationSeconds * ratio);
        row.fixedDurationSeconds = std::min(row.regionEndSeconds - row.regionStartSeconds,
                                             item.note->consonantSeconds * ratio);
        row.alignmentSeconds = row.regionStartSeconds + row.fixedDurationSeconds;
        row.relativePitchCents = (item.note->midiNote - item.note->sourceMidiCenter) * 100.0;
        row.melodyneData = true;
        row.melodynePitchCenterCents = item.note->midiNote * 100.0;
        row.melodyneOriginalPitchCenterCents = item.note->sourceMidiCenter * 100.0;
        row.melodynePitchDrift = item.note->drift;
        row.melodynePitchModulation = item.note->modulation;
        row.melodyneFormantCents = item.note->formantSemitones * 100.0;
        // Melodyne amplitude is note-local.  Persisting the clip gain here
        // discarded the vocal's per-note level when the HJM file was loaded.
        row.melodyneAmplitude = item.note->gain;
        row.melodyneSibilantBalance = item.note->breath;
        row.melodyneAttackSeconds = item.note->consonantSeconds;
        rows.push_back(std::move(row));
    }
    if (rows.empty()) rows.push_back({ "region 1", 0.0, 0.5, 0.0, 0.0 });
    return rows;
}

bool SampleSettings::save(const juce::File& audio,
                          const std::vector<SampleRegionSetting>& input,
                          juce::String& error)
{
    auto rows = input;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& left, const auto& right)
    {
        return left.regionStartSeconds < right.regionStartSeconds;
    });
    juce::String csv = "name,region_start_sec,region_end_sec,note_alignment_sec,fixed_duration_sec,relative_pitch_cents,melodyne_project_data,melodyne_pitch_center_cents,melodyne_original_pitch_center_cents,melodyne_pitch_drift_factor,melodyne_pitch_modulation_factor,melodyne_transition_sec,melodyne_formant_offset_cents,melodyne_amplitude_factor,melodyne_sibilant_balance,melodyne_attack_duration_sec,melodyne_decay_elongation,utau_overlap_sec\n";
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        auto row = rows[index];
        row.regionStartSeconds = std::max(0.0, row.regionStartSeconds);
        row.regionEndSeconds = std::max(row.regionStartSeconds + 0.001, row.regionEndSeconds);
        row.alignmentSeconds = juce::jlimit(row.regionStartSeconds, row.regionEndSeconds,
                                            row.alignmentSeconds);
        row.fixedDurationSeconds = juce::jlimit(0.0, row.regionEndSeconds - row.regionStartSeconds,
                                                row.fixedDurationSeconds);
        const std::array<double, 17> values {
            row.regionStartSeconds, row.regionEndSeconds, row.alignmentSeconds,
            row.fixedDurationSeconds, row.relativePitchCents,
            row.melodyneData ? 1.0 : 0.0, row.melodynePitchCenterCents,
            row.melodyneOriginalPitchCenterCents, row.melodynePitchDrift,
            row.melodynePitchModulation, row.melodyneTransitionSeconds,
            row.melodyneFormantCents, row.melodyneAmplitude,
            row.melodyneSibilantBalance, row.melodyneAttackSeconds,
            row.melodyneDecayElongation, row.overlapSeconds
        };
        csv += csvEscape(row.name.isEmpty() ? "region " + juce::String(index + 1) : row.name);
        for (const auto value : values) csv += "," + juce::String(value, 9).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        csv += "\n";
    }
    const auto sidecar = sidecarFor(audio);
    if (!sidecar.replaceWithText(csv, false, false, "\n"))
    {
        error = "Could not write " + sidecar.getFullPathName();
        return false;
    }
    return true;
}

bool SampleSettings::importOto(const juce::File& oto, const juce::File& audio,
                               double audioDuration,
                               std::vector<SampleRegionSetting>& rows,
                               juce::String& error)
{
    if (!oto.existsAsFile()) { error = "oto.ini not found"; return false; }
    rows.clear();
    for (const auto& raw : juce::StringArray::fromLines(decodeOtoText(oto)))
    {
        const auto equals = raw.indexOfChar('=');
        if (equals <= 0) continue;
        const auto wav = raw.substring(0, equals).trim();
        // An oto.ini normally describes a complete voicebank.  Wrench mode is
        // scoped to one source file, so only import rows belonging to that WAV
        // instead of accidentally applying every alias in the bank to it.
        if (audio != juce::File{}
            && !juce::File(wav.replaceCharacter('\\', '/')).getFileName()
                    .equalsIgnoreCase(audio.getFileName()))
            continue;
        auto fields = juce::StringArray::fromTokens(raw.substring(equals + 1), ",", "\"");
        if (fields.size() < 6) continue;
        const auto offset = std::max(0.0, fields[1].getDoubleValue() / 1000.0);
        const auto consonant = std::max(0.0, fields[2].getDoubleValue() / 1000.0);
        const auto cutoffMs = fields[3].getDoubleValue();
        const auto preutter = std::max(0.0, fields[4].getDoubleValue() / 1000.0);
        const auto overlap = fields[5].getDoubleValue() / 1000.0;
        SampleRegionSetting row;
        row.name = fields[0].trim().isNotEmpty() ? fields[0].trim()
                                                   : juce::File(wav).getFileNameWithoutExtension();
        row.regionStartSeconds = offset;
        row.fixedDurationSeconds = consonant;
        row.alignmentSeconds = offset + preutter;
        row.overlapSeconds = overlap;
        // UTAU uses a negative cutoff as the region length from offset, while
        // a positive cutoff trims that amount from the physical file end.
        row.regionEndSeconds = cutoffMs < 0.0
            ? offset + (-cutoffMs) / 1000.0
            : audioDuration > 0.0 ? audioDuration - cutoffMs / 1000.0
                                  : offset + std::max({ 0.05, consonant, preutter });
        if (audioDuration > 0.0)
            row.regionEndSeconds = juce::jlimit(offset + 0.001,
                                                std::max(offset + 0.001, audioDuration),
                                                row.regionEndSeconds);
        else row.regionEndSeconds = std::max(offset + 0.001, row.regionEndSeconds);
        row.alignmentSeconds = juce::jlimit(row.regionStartSeconds, row.regionEndSeconds,
                                            row.alignmentSeconds);
        row.fixedDurationSeconds = juce::jlimit(0.0,
            row.regionEndSeconds - row.regionStartSeconds, row.fixedDurationSeconds);
        rows.push_back(std::move(row));
    }
    if (rows.empty())
    {
        error = audio == juce::File{} ? "oto.ini contains no valid entries"
                                      : "oto.ini contains no entries for " + audio.getFileName();
        return false;
    }
    return true;
}

bool SampleSettings::exportOto(const juce::File& oto, const juce::File& audio,
                               const std::vector<SampleRegionSetting>& rows,
                               double audioDuration, juce::String& error)
{
    juce::String output;
    for (const auto& row : rows)
    {
        const auto cutoff = -std::max(0.0, audioDuration - row.regionEndSeconds) * 1000.0;
        output += audio.getFileName() + "=" + row.name + ","
            + juce::String(row.regionStartSeconds * 1000.0, 3) + ","
            + juce::String(row.fixedDurationSeconds * 1000.0, 3) + ","
            + juce::String(cutoff, 3) + ","
            + juce::String((row.alignmentSeconds - row.regionStartSeconds) * 1000.0, 3)
            + "," + juce::String(row.overlapSeconds * 1000.0, 3) + "\n";
    }
    if (!oto.replaceWithText(output, false, false, "\n"))
    {
        error = "Could not write " + oto.getFullPathName();
        return false;
    }
    return true;
}

namespace
{
// A sample dropped into a voicebank has no oto row until someone writes one,
// and UTAU itself will still play it -- offsets simply default to zero.  Left
// out of the listing it cannot even be opened to be described, so the folder
// is scanned and anything unaccounted for is offered as an entry that has yet
// to be written (lineIndex -1).
void appendUnlistedAudio(const juce::File& root,
                         const juce::Array<juce::File>& otoFiles,
                         bool jieMode,
                         std::vector<VoicebankOtoEntry>& entries)
{
    if (!root.isDirectory()) return;
    juce::Array<juce::File> audioFiles;
    root.findChildFiles(audioFiles, juce::File::findFiles, true,
                        "*.wav;*.flac;*.aif;*.aiff");
    if (audioFiles.isEmpty()) return;

    juce::StringArray described;
    for (const auto& entry : entries)
        described.add(entry.audioFile.getFullPathName().toLowerCase());

    for (const auto& file : audioFiles)
    {
        if (described.contains(file.getFullPathName().toLowerCase())) continue;
        // The row belongs in the oto governing this sample's own folder.  Where
        // there is none, name the file it would be written to so saving can
        // create it rather than failing.
        auto governing = file.getParentDirectory().getChildFile("oto.ini");
        auto bestDepth = -1;
        for (const auto& oto : otoFiles)
        {
            const auto folder = oto.getParentDirectory();
            if (folder != file.getParentDirectory()
                && !file.isAChildOf(folder))
                continue;
            // Deepest wins: a sample is described by the nearest oto above it.
            const auto depth = folder.getFullPathName().length();
            if (depth > bestDepth) { bestDepth = depth; governing = oto; }
        }
        VoicebankOtoEntry entry;
        entry.otoFile = jieMode ? SampleSettings::jieClassicOtoFileFor(governing) : governing;
        entry.audioFile = file;
        // Keyed the way the oto file itself keys a sample: relative to the
        // folder that oto lives in.
        entry.sourceName = file.getRelativePathFrom(governing.getParentDirectory());
        entry.lineIndex = -1;
        entries.push_back(std::move(entry));
    }
}
}

std::vector<VoicebankOtoEntry> SampleSettings::loadVoicebankOto(
    const juce::File& root, juce::StringArray& warnings, bool jieMode,
    bool mouMode)
{
    warnings.clear();
    std::vector<VoicebankOtoEntry> entries;
    if (!root.isDirectory())
    {
        warnings.add("Voicebank directory not found: " + root.getFullPathName());
        return entries;
    }

    juce::Array<juce::File> otoFiles;
    root.findChildFiles(otoFiles, juce::File::findFiles, true, "oto.ini");
    otoFiles.sort();
    for (const auto& originalOto : otoFiles)
    {
        const auto jieOto = jieClassicOtoFileFor(originalOto);
        const auto sourceOto = jieMode && jieOto.existsAsFile() ? jieOto : originalOto;
        // In Jie mode entries always point at their independent destination,
        // even before that destination has been seeded.  Saving can therefore
        // never accidentally fall back to the original oto.ini.
        const auto storageOto = jieMode ? jieOto : originalOto;
        auto malformed = 0;
        const auto otoLines = juce::StringArray::fromLines(decodeOtoText(sourceOto));
        for (int lineIndex = 0; lineIndex < otoLines.size(); ++lineIndex)
        {
            const auto& rawLine = otoLines[lineIndex];
            const auto line = rawLine.trim();
            if (line.isEmpty() || line.startsWithChar(';') || line.startsWithChar('#'))
                continue;
            const auto equals = line.indexOfChar('=');
            if (equals <= 0)
            {
                ++malformed;
                continue;
            }
            const auto wav = line.substring(0, equals).trim();
            const auto fields = splitCsv(line.substring(equals + 1));
            if (wav.isEmpty() || fields.size() < 6)
            {
                ++malformed;
                continue;
            }

            VoicebankOtoEntry entry;
            entry.otoFile = storageOto;
            entry.sourceName = wav;
            entry.audioFile = originalOto.getParentDirectory().getChildFile(
                wav.replaceCharacter('\\', '/'));
            entry.alias = fields[0].trim();
            entry.offsetMs = fields[1].trim().getDoubleValue();
            entry.consonantMs = fields[2].trim().getDoubleValue();
            entry.cutoffMs = fields[3].trim().getDoubleValue();
            entry.preutteranceMs = fields[4].trim().getDoubleValue();
            entry.overlapMs = fields[5].trim().getDoubleValue();
            entry.lineIndex = lineIndex;
            entries.push_back(std::move(entry));
        }
        if (malformed > 0)
            warnings.add(sourceOto.getRelativePathFrom(root) + ": "
                         + juce::String(malformed) + " malformed line(s)");
    }

    std::stable_sort(entries.begin(), entries.end(), [&root](const auto& left, const auto& right)
    {
        const auto leftOto = left.otoFile.getRelativePathFrom(root);
        const auto rightOto = right.otoFile.getRelativePathFrom(root);
        const auto otoOrder = leftOto.compareNatural(rightOto);
        if (otoOrder != 0) return otoOrder < 0;
        const auto fileOrder = left.sourceName.compareNatural(right.sourceName);
        return fileOrder != 0 ? fileOrder < 0 : left.alias.compareNatural(right.alias) < 0;
    });
    if (otoFiles.isEmpty()) warnings.add("No oto.ini found in voicebank directory");
    else if (entries.empty()) warnings.add("No valid oto.ini entries found");
    appendUnlistedAudio(root, otoFiles, jieMode, entries);
    mergeJieOto(entries);
    if (mouMode) mergeMouOto(entries);
    return entries;
}

bool SampleSettings::updateVoicebankOtoEntry(const VoicebankOtoEntry& original,
                                             const VoicebankOtoEntry& updated,
                                             juce::String& error)
{
    if (!original.otoFile.existsAsFile())
    {
        // Writing the first row of a Jie oto that has not been seeded yet: take
        // the classic file beside it as the starting point, the way duplicating
        // an alias does.  Without this a sample with no row at all could be
        // listed in Jie mode but never saved.
        const auto sibling = original.otoFile.getParentDirectory().getChildFile("oto.ini");
        if (original.lineIndex >= 0 || !sibling.existsAsFile()
            || !sibling.copyFileTo(original.otoFile))
        {
            error = "oto.ini not found: " + original.otoFile.getFullPathName();
            return false;
        }
    }

    const auto decoded = decodeOtoText(original.otoFile);
    auto lines = juce::StringArray::fromLines(decoded);
    auto targetLine = original.lineIndex;
    const auto lineMatchesSource = [&original](const juce::String& candidate)
    {
        const auto equals = candidate.indexOfChar('=');
        return equals > 0 && candidate.substring(0, equals).trim() == original.sourceName;
    };
    if (!juce::isPositiveAndBelow(targetLine, lines.size())
        || !lineMatchesSource(lines[targetLine]))
    {
        targetLine = -1;
        for (int index = 0; index < lines.size(); ++index)
            if (lineMatchesSource(lines[index]))
            {
                const auto equals = lines[index].indexOfChar('=');
                const auto fields = splitCsv(lines[index].substring(equals + 1));
                if (!fields.isEmpty() && (fields[0].trim() == original.alias
                    || targetLine < 0))
                    targetLine = index;
                if (!fields.isEmpty() && fields[0].trim() == original.alias) break;
            }
    }
    if (!juce::isPositiveAndBelow(targetLine, lines.size()))
    {
        // A sample that had no row yet gets one appended.  Anything else means
        // the row was there when the list was built and has since gone.
        if (original.lineIndex >= 0)
        {
            error = "The selected oto.ini entry no longer exists";
            return false;
        }
        while (!lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty())
            lines.remove(lines.size() - 1);
        lines.add(otoEntryText(updated));
    }
    else
        lines.set(targetLine, otoEntryText(updated));

    const auto lineEnding = decoded.contains("\r\n") ? juce::String("\r\n")
                                                        : juce::String("\n");
    auto output = lines.joinIntoString(lineEnding);
    if (decoded.endsWithChar('\n')) output += lineEnding;

    if (!writeOtoTextPreservingEncoding(original.otoFile, output, error))
        return false;

    // UTAU rendering consumes the per-sample HJM sidecar generated from oto.ini.
    // Refresh it immediately so editing timing is reflected by the next render.
    if (updated.audioFile.existsAsFile())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto reader = std::unique_ptr<juce::AudioFormatReader>(
            formats.createReaderFor(updated.audioFile));
        if (reader == nullptr || reader->sampleRate <= 0.0)
        {
            error = "oto.ini was saved, but the audio file could not be read";
            return false;
        }
        const auto duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        std::vector<SampleRegionSetting> rows;
        juce::String sidecarError;
        if (!importOto(original.otoFile, updated.audioFile, duration, rows, sidecarError)
            || !save(updated.audioFile, rows, sidecarError))
        {
            error = "oto.ini was saved, but its render cache could not be updated: "
                + sidecarError;
            return false;
        }
    }
    return true;
}

juce::File SampleSettings::jieClassicOtoFileFor(const juce::File& otoFile)
{
    if (otoFile.getFileName().equalsIgnoreCase("oto.jie.ini")) return otoFile;
    return otoFile.getParentDirectory().getChildFile("oto.jie.ini");
}

bool SampleSettings::updateJieVoicebankOtoEntry(const VoicebankOtoEntry& original,
                                                const VoicebankOtoEntry& updated,
                                                juce::String& error)
{
    const auto target = jieClassicOtoFileFor(original.otoFile);
    if (!target.existsAsFile())
    {
        const auto source = original.otoFile.getFileName().equalsIgnoreCase("oto.jie.ini")
            ? original.otoFile.getParentDirectory().getChildFile("oto.ini")
            : original.otoFile;
        if (!source.existsAsFile())
        {
            error = "Original oto.ini not found: " + source.getFullPathName();
            return false;
        }
        if (!source.copyFileTo(target))
        {
            error = "Could not create independent Jie oto: " + target.getFullPathName();
            return false;
        }
    }

    auto targetOriginal = original;
    auto targetUpdated = updated;
    targetOriginal.otoFile = target;
    targetUpdated.otoFile = target;
    // Jie timing must not overwrite the classic mode's shared legacy sidecar.
    // Rendering now reads the selected oto file directly, so no Jie sidecar is
    // required here.
    targetUpdated.audioFile = juce::File();
    return updateVoicebankOtoEntry(targetOriginal, targetUpdated, error);
}

bool SampleSettings::duplicateVoicebankOtoEntry(const VoicebankOtoEntry& source,
                                                const juce::String& newAlias,
                                                bool jieMode,
                                                juce::String& error)
{
    const auto alias = newAlias.trim();
    if (alias.isEmpty() || alias.containsAnyOf(",\r\n"))
    {
        error = "Alias must not be empty or contain comma/newline characters";
        return false;
    }

    auto target = source.otoFile;
    if (jieMode)
    {
        target = jieClassicOtoFileFor(source.otoFile);
        if (!target.existsAsFile())
        {
            const auto original = source.otoFile.getFileName().equalsIgnoreCase("oto.jie.ini")
                ? source.otoFile.getParentDirectory().getChildFile("oto.ini")
                : source.otoFile;
            if (!original.existsAsFile() || !original.copyFileTo(target))
            {
                error = "Could not create independent Jie oto: " + target.getFullPathName();
                return false;
            }
        }
    }
    if (!target.existsAsFile())
    {
        error = "OTO file not found: " + target.getFullPathName();
        return false;
    }

    const auto decoded = decodeOtoText(target);
    auto lines = juce::StringArray::fromLines(decoded);
    for (const auto& rawLine : lines)
    {
        const auto equals = rawLine.indexOfChar('=');
        if (equals <= 0) continue;
        const auto fields = splitCsv(rawLine.substring(equals + 1));
        if (!fields.isEmpty() && fields[0].trim().equalsIgnoreCase(alias))
        {
            error = "Alias already exists: " + alias;
            return false;
        }
    }

    auto copied = source;
    copied.otoFile = target;
    copied.alias = alias;
    lines.add(otoEntryText(copied));
    const auto lineEnding = decoded.contains("\r\n") ? juce::String("\r\n")
                                                        : juce::String("\n");
    const auto output = lines.joinIntoString(lineEnding) + lineEnding;
    if (!writeOtoTextPreservingEncoding(target, output, error)) return false;

    // A copied Jie alias intentionally starts with the same four-region
    // boundaries as its source.  No audio file is duplicated.
    if (jieMode && source.hasJieOto)
    {
        copied.hasJieOto = true;
        if (!updateJieOtoEntry(copied, copied, error)) return false;
    }
    return true;
}

bool SampleSettings::importVoicebank(const juce::File& root, juce::StringArray& audioFiles,
                                     int& sidecarsWritten, int& regionsWritten,
                                     juce::StringArray& warnings)
{
    audioFiles.clear();
    warnings.clear();
    sidecarsWritten = 0;
    regionsWritten = 0;
    juce::Array<juce::File> otoFiles;
    if (root.existsAsFile() && root.getFileName().equalsIgnoreCase("oto.ini"))
        otoFiles.add(root);
    else if (root.isDirectory())
    {
        juce::Array<juce::File> candidates;
        root.findChildFiles(candidates, juce::File::findFiles, true, "*");
        for (const auto& candidate : candidates)
            if (candidate.getFileName().equalsIgnoreCase("oto.ini")) otoFiles.add(candidate);
    }
    if (otoFiles.isEmpty())
    {
        warnings.add("oto.ini not found under " + root.getFullPathName());
        return false;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (const auto& oto : otoFiles)
    {
        juce::Array<juce::File> samples;
        oto.getParentDirectory().findChildFiles(samples, juce::File::findFiles, false, "*");
        for (const auto& sample : samples)
        {
            if (!sample.hasFileExtension("wav;flac;aif;aiff;mp3;ogg")) continue;
            audioFiles.addIfNotAlreadyThere(sample.getFullPathName(), false);
            auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(sample));
            if (reader == nullptr || reader->sampleRate <= 0.0) continue;
            const auto duration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
            std::vector<SampleRegionSetting> rows;
            juce::String error;
            if (!importOto(oto, sample, duration, rows, error)) continue;
            if (!save(sample, rows, error))
            {
                warnings.add(error);
                continue;
            }
            ++sidecarsWritten;
            regionsWritten += static_cast<int>(rows.size());
        }
    }
    return !audioFiles.isEmpty();
}

// ---------------------------------------------------------------------------
// Jie-oto (oto4.ini): the four-region companion to oto.ini
//
// One row per oto entry, keyed by wav name plus that entry's offset so a wav
// carrying several aliases stays unambiguous:
//
//     <wav>=<oto_offset>,<b1>,<b2>,<b3>[,w1..w4][,mn1..mn4][,mx1..mx4]
//
// b1..b3 are milliseconds relative to the offset and split the sample into
// onset / glide / nucleus / coda.  Fields past b3 are optional per-entry
// stretch policy that the WCSNDM engine reads; this editor never writes them
// but preserves whatever is already on the row.
//
// The file is called oto4.ini on disk because that is the name the engine
// looks for; the interface calls it Jie-oto.
// ---------------------------------------------------------------------------
namespace
{
constexpr double jieOffsetTolerance = 0.5;   // ms

struct JieRow
{
    juce::String wav;
    double offsetMs = 0.0;
    juce::StringArray fields;   // oto offset, b1, b2, b3, then any extra policy
    int lineIndex = -1;
};

std::vector<JieRow> readJieRows(const juce::File& file)
{
    std::vector<JieRow> rows;
    if (!file.existsAsFile()) return rows;
    const auto lines = juce::StringArray::fromLines(decodeOtoText(file));
    for (int index = 0; index < lines.size(); ++index)
    {
        auto line = lines[index].trim();
        if (line.isEmpty() || line.startsWithChar(';') || line.startsWithChar('#')) continue;
        const auto equals = line.indexOfChar('=');
        if (equals <= 0) continue;
        auto fields = splitCsv(line.substring(equals + 1));
        if (fields.size() < 4) continue;
        JieRow row;
        row.wav = line.substring(0, equals).trim();
        row.offsetMs = fields[0].trim().getDoubleValue();
        row.fields = std::move(fields);
        row.lineIndex = index;
        rows.push_back(std::move(row));
    }
    return rows;
}

const JieRow* findJieRow(const std::vector<JieRow>& rows, const juce::String& wav,
                         double offsetMs)
{
    const JieRow* best = nullptr;
    auto bestDistance = jieOffsetTolerance;
    for (const auto& row : rows)
    {
        if (!row.wav.equalsIgnoreCase(wav)) continue;
        const auto distance = std::abs(row.offsetMs - offsetMs);
        if (distance <= bestDistance) { bestDistance = distance; best = &row; }
    }
    return best;
}

juce::String jieNumber(double value)
{
    if (std::abs(value) < 0.0005) value = 0.0;
    auto text = juce::String(value, 3);
    while (text.containsChar('.') && text.endsWithChar('0')) text = text.dropLastCharacters(1);
    if (text.endsWithChar('.')) text = text.dropLastCharacters(1);
    return text;
}

// 谋-oto (otomou.ini): the Jie row with a class string in front.
// ASCII on purpose.  An oto file whose rows carry CP932 aliases is decoded as
// CP932, so a non-ASCII comment written here comes back re-encoded and is
// written out corrupted a little further each time.  Nothing but a human reads
// these lines, so they say it in ASCII and the question never arises.
juce::StringArray mouHeaderLines()
{
    juce::StringArray header;
    header.add("; otomou.ini - per-region phoneme classes for the Mou UTAU mode");
    header.add("; <wav>=<classes>,<oto_offset>,<b1>,<b2>,<b3>[,policy...]");
    header.add("; classes: one letter per region, C consonant / V vowel / S silence.");
    header.add(";          its length is the region count, two to four, and the");
    header.add(";          letters say which part is the vowel.");
    header.add("; All three boundaries are always written, whatever the count: the");
    header.add("; class string alone decides how many regions there are, and the");
    header.add("; ones it does not reach are kept so a different split can use them.");
    return header;
}

// The row for one entry.  All three boundaries, whatever the count: the
// class string alone says how many regions there are, so the ones the count
// does not reach are carried rather than dropped -- otherwise switching an
// entry down to two regions and back up to four reads them back as zero.  It
// also keeps field 4 meaning "policy" for every row, short ones included.
juce::String mouRowText(const VoicebankOtoEntry& entry, const juce::String& classes,
                        const juce::StringArray& extras)
{
    juce::StringArray fields;
    fields.add(classes);
    fields.add(jieNumber(entry.offsetMs));
    fields.add(jieNumber(entry.jieOnsetMs));
    fields.add(jieNumber(entry.jieGlideMs));
    fields.add(jieNumber(entry.jieNucleusMs));
    for (const auto& extra : extras) fields.add(extra);
    return entry.sourceName + "=" + fields.joinIntoString(",");
}

juce::String jieRowText(const VoicebankOtoEntry& entry, const juce::StringArray& extras)
{
    juce::StringArray fields;
    fields.add(jieNumber(entry.offsetMs));
    fields.add(jieNumber(entry.jieOnsetMs));
    fields.add(jieNumber(entry.jieGlideMs));
    fields.add(jieNumber(entry.jieNucleusMs));
    for (const auto& extra : extras) fields.add(extra);
    return entry.sourceName + "=" + fields.joinIntoString(",");
}

double jieEntryEndMs(const VoicebankOtoEntry& entry, double durationMs)
{
    return entry.cutoffMs < 0.0 ? entry.offsetMs - entry.cutoffMs
                                : durationMs - entry.cutoffMs;
}

juce::StringArray jieHeaderLines()
{
    juce::StringArray header;
    header.add("; oto4.ini - four-region (Jie) oto for the WCSNDM engine");
    header.add("; <wav>=<oto_offset>,<onset|glide>,<glide|nucleus>,<nucleus|coda>");
    header.add("; the three boundaries are milliseconds relative to the oto offset");
    return header;
}
}

juce::File SampleSettings::jieOtoFileFor(const juce::File& otoFile)
{
    return otoFile.getParentDirectory().getChildFile("oto4.ini");
}

juce::File SampleSettings::mouOtoFileFor(const juce::File& otoFile)
{
    // 谋•OTO, read only in 谋•UTAU mode.  Its rows are oto4 rows with an
    // optional class string in front, so the same parser reads both and a
    // voicebank can start from a copy of its oto4.ini.
    return otoFile.getParentDirectory().getChildFile("otomou.ini");
}

namespace
{
// A leading token made only of class letters, exactly one per region.  Only
// class letters and the right length count: half a class string ("CV,") would
// otherwise read as a wildcard row whose boundaries are right and whose
// annotation quietly went missing, which is the same rule the engine applies.
juce::String takeClassString(juce::StringArray& fields)
{
    // The length is the region count: two, three or four.  A 谋 entry says
    // how many parts it has by how many letters it writes, and which of them
    // is the vowel by where it puts the V.
    if (fields.isEmpty()) return {};
    const auto head = fields[0].trim().toUpperCase();
    if (head.length() < 2 || head.length() > 4) return {};
    for (auto c : head)
        if (c != 'C' && c != 'V' && c != 'S') return {};
    fields.remove(0);
    return head;
}
}  // namespace

int SampleSettings::mouRegionCount(const juce::String& classes)
{
    const auto length = classes.trim().length();
    return length >= 2 && length <= 4 ? length : 4;
}

juce::String SampleSettings::mouClassesForCount(const juce::String& classes, int count)
{
    count = juce::jlimit(2, 4, count);
    auto from = classes.trim().toUpperCase();
    if (from.length() < 2) from = "CVVV";
    juce::String next;
    for (int index = 0; index < count; ++index)
        next += index < from.length() ? from[index] : 'V';
    return next;
}

void SampleSettings::mergeMouOto(std::vector<VoicebankOtoEntry>& entries)
{
    std::map<juce::String, std::vector<JieRow>> cache;
    for (auto& entry : entries)
    {
        const auto mouFile = mouOtoFileFor(entry.otoFile);
        const auto key = mouFile.getFullPathName();
        if (cache.find(key) == cache.end())
        {
            // The class string sits where the oto offset does in an oto4 row,
            // so it has to come off before the row can be matched by offset --
            // read as it stands, "CVVC" is an offset of zero and the row is
            // never found.  Stripped here, once, and the rest of the row is
            // an ordinary oto4 row from then on.
            auto rows = readJieRows(mouFile);
            for (auto& raw : rows)
            {
                auto stripped = raw.fields;
                const auto marked = takeClassString(stripped);
                if (marked.isEmpty()) continue;
                raw.fields = stripped;
                raw.fields.insert(0, marked);          // keep it for the reader
                raw.offsetMs = stripped[0].trim().getDoubleValue();
            }
            cache.emplace(key, std::move(rows));
        }
        const auto* row = findJieRow(cache[key], entry.sourceName, entry.offsetMs);
        if (row == nullptr) continue;
        auto fields = row->fields;
        const auto classes = takeClassString(fields);
        // One boundary fewer than there are regions, after the oto offset.  A
        // row written here carries all three, but a hand-written one may stop
        // at the count, so this is the least it can have.
        if (classes.isEmpty() || fields.size() < classes.length()) continue;
        // A 谋 row carries its own boundaries too, so a voicebank can annotate
        // without having to keep oto4.ini in step.  A boundary the row does
        // not reach reads as zero, which the renderer clamps up to the one
        // before it -- the count decides how many are regions anyway.
        const auto boundary = [&fields](int index)
        {
            return index < fields.size() ? fields[index].trim().getDoubleValue()
                                         : 0.0;
        };
        entry.hasJieOto = true;
        entry.jieOnsetMs = boundary(1);
        entry.jieGlideMs = boundary(2);
        entry.jieNucleusMs = boundary(3);
        entry.mouClasses = classes;
    }
}

void SampleSettings::mergeJieOto(std::vector<VoicebankOtoEntry>& entries)
{
    std::map<juce::String, std::vector<JieRow>> cache;
    for (auto& entry : entries)
    {
        const auto jieFile = jieOtoFileFor(entry.otoFile);
        const auto key = jieFile.getFullPathName();
        if (cache.find(key) == cache.end()) cache.emplace(key, readJieRows(jieFile));
        const auto* row = findJieRow(cache[key], entry.sourceName, entry.offsetMs);
        if (row == nullptr) continue;
        entry.hasJieOto = true;
        entry.jieOnsetMs = row->fields[1].trim().getDoubleValue();
        entry.jieGlideMs = row->fields[2].trim().getDoubleValue();
        entry.jieNucleusMs = row->fields[3].trim().getDoubleValue();
    }
}

bool SampleSettings::createJieOto(const juce::File& root, int& written, int& kept,
                                  juce::String& error)
{
    written = 0;
    kept = 0;
    juce::StringArray warnings;
    // Seed from the Jie view: rendering in Jie mode sends oto.jie.ini's
    // offsets to the engine, and the rows are keyed by that offset.
    const auto entries = loadVoicebankOto(root, warnings, true);
    if (entries.empty())
    {
        error = "No oto.ini entries found under " + root.getFullPathName();
        return false;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::map<juce::String, std::vector<const VoicebankOtoEntry*>> byOto;
    for (const auto& entry : entries)
        byOto[entry.otoFile.getFullPathName()].push_back(&entry);

    for (const auto& group : byOto)
    {
        const juce::File otoFile(group.first);
        const auto independentOto = jieClassicOtoFileFor(otoFile);
        if (!independentOto.existsAsFile() && !otoFile.copyFileTo(independentOto))
        {
            error = "Could not create independent Jie oto: "
                + independentOto.getFullPathName();
            return false;
        }
        const auto jieFile = jieOtoFileFor(otoFile);
        const auto existing = readJieRows(jieFile);
        auto lines = jieHeaderLines();
        for (const auto* entry : group.second)
        {
            if (const auto* row = findJieRow(existing, entry->sourceName, entry->offsetMs))
            {
                // Never overwrite boundaries the user has already dragged.
                lines.add(entry->sourceName + "=" + row->fields.joinIntoString(","));
                ++kept;
                continue;
            }
            auto seeded = *entry;
            auto durationMs = 0.0;
            if (auto reader = std::unique_ptr<juce::AudioFormatReader>(
                    formats.createReaderFor(entry->audioFile)))
                if (reader->sampleRate > 0.0)
                    durationMs = 1000.0 * static_cast<double>(reader->lengthInSamples)
                        / reader->sampleRate;
            const auto span = std::max(0.0, jieEntryEndMs(*entry, durationMs) - entry->offsetMs);
            // Seed with the classic two-region layout written as four regions:
            // onset = the oto consonant, empty glide, empty coda.  That already
            // renders, and the user pulls the two collapsed boundaries apart to
            // describe the syllable.
            seeded.jieOnsetMs = juce::jlimit(0.0, span, entry->consonantMs);
            seeded.jieGlideMs = seeded.jieOnsetMs;
            seeded.jieNucleusMs = span;
            lines.add(jieRowText(seeded, {}));
            ++written;
        }
        if (!jieFile.replaceWithText(lines.joinIntoString("\n") + "\n", false, false, "\n"))
        {
            error = "Could not write " + jieFile.getFullPathName();
            return false;
        }
    }
    return true;
}

namespace
{
// A 谋 row keyed by wav and offset, with the class string taken off first --
// it sits where the offset does, so read as it stands "CVVC" is an offset of
// zero and nothing is ever found.
struct MouRows
{
    std::vector<JieRow> rows;
    std::vector<juce::String> classes;
};

MouRows readMouRows(const juce::File& file)
{
    MouRows out;
    out.rows = readJieRows(file);
    out.classes.resize(out.rows.size());
    for (std::size_t index = 0; index < out.rows.size(); ++index)
    {
        auto stripped = out.rows[index].fields;
        out.classes[index] = takeClassString(stripped);
        if (out.classes[index].isEmpty() || stripped.isEmpty()) continue;
        out.rows[index].fields = stripped;
        out.rows[index].offsetMs = stripped[0].trim().getDoubleValue();
    }
    return out;
}

// The same row the reader will take, by construction.  Picking it here by a
// rule of its own -- the first within tolerance, where the reader takes the
// nearest and, among equals, the last -- meant that on a voicebank with more
// than one row for a sample the annotation was written to one row and read
// from another, so saving a three-region entry appeared to do nothing at all.
// Several oto aliases can share one wav and offset, and 谋 keys its rows the
// way 界 does, so those rows are not rare.
int findMouRow(const MouRows& rows, const juce::String& wav, double offsetMs)
{
    const auto* row = findJieRow(rows.rows, wav, offsetMs);
    return row == nullptr ? -1 : static_cast<int>(row - rows.rows.data());
}
}  // namespace

bool SampleSettings::updateMouOtoEntry(const VoicebankOtoEntry& original,
                                      const VoicebankOtoEntry& updated,
                                      juce::String& error)
{
    // otomou.ini is 谋's own file: it sits beside the entry's oto and owes
    // nothing to it.  In 界 mode an entry points at oto.jie.ini whether or not
    // that file has been seeded yet, so requiring it here made every save from
    // the voicebank panel fail on a bank nobody had opened the editor on.
    const auto mouFile = mouOtoFileFor(updated.otoFile);
    if (!mouFile.getParentDirectory().isDirectory())
    {
        error = "Voicebank folder not found: "
            + mouFile.getParentDirectory().getFullPathName();
        return false;
    }
    auto lines = mouFile.existsAsFile()
        ? juce::StringArray::fromLines(decodeOtoText(mouFile))
        : mouHeaderLines();
    while (lines.size() > 0 && lines[lines.size() - 1].trim().isEmpty())
        lines.remove(lines.size() - 1);

    // Located by the offset it had before the edit, or the row is orphaned
    // and a duplicate appended -- the engine then finds two for one sample.
    const auto rows = readMouRows(mouFile);
    auto found = findMouRow(rows, original.sourceName, original.offsetMs);
    if (found < 0) found = findMouRow(rows, updated.sourceName, updated.offsetMs);

    juce::StringArray extras;
    auto classes = updated.mouClasses.trim().toUpperCase();
    if (found >= 0)
    {
        const auto& row = rows.rows[static_cast<std::size_t>(found)];
        for (int index = 4; index < row.fields.size(); ++index)
            extras.add(row.fields[index]);
        // Moving a boundary must not throw the annotation away.
        if (classes.isEmpty()) classes = rows.classes[static_cast<std::size_t>(found)];
    }
    if (classes.isEmpty()) classes = "CVVV";

    const auto text = mouRowText(updated, classes, extras);
    if (found >= 0
        && juce::isPositiveAndBelow(rows.rows[static_cast<std::size_t>(found)].lineIndex,
                                    lines.size()))
        lines.set(rows.rows[static_cast<std::size_t>(found)].lineIndex, text);
    else
        lines.add(text);

    if (!mouFile.replaceWithText(lines.joinIntoString("\n") + "\n", false, false, "\n"))
    {
        error = "Could not write " + mouFile.getFullPathName();
        return false;
    }
    return true;
}

bool SampleSettings::createMouOto(const juce::File& root, int& written, int& kept,
                                  int& merged, juce::String& error)
{
    written = 0;
    kept = 0;
    merged = 0;
    juce::StringArray warnings;
    // The Jie view: a 谋 row is a Jie row with letters in front, keyed by the
    // same offset, so the boundaries come across as they are.
    const auto entries = loadVoicebankOto(root, warnings, true);
    if (entries.empty())
    {
        error = "No oto.ini entries found under " + root.getFullPathName();
        return false;
    }
    std::map<juce::String, std::vector<const VoicebankOtoEntry*>> byOto;
    for (const auto& entry : entries)
        byOto[entry.otoFile.getFullPathName()].push_back(&entry);

    for (const auto& group : byOto)
    {
        const juce::File otoFile(group.first);
        const auto mouFile = mouOtoFileFor(otoFile);
        auto lines = mouFile.existsAsFile()
            ? juce::StringArray::fromLines(decodeOtoText(mouFile))
            : mouHeaderLines();
        while (lines.size() > 0 && lines[lines.size() - 1].trim().isEmpty())
            lines.remove(lines.size() - 1);
        // More than one row for a wav and an offset is malformed: that pair is
        // the key.  Collapse them first, keeping whichever carries an
        // annotation, so a file written before seeding knew better is repaired
        // rather than left for the reader and the writer to disagree over.
        {
            const auto before = readMouRows(mouFile);
            std::map<juce::String, std::vector<std::size_t>> byKey;
            for (std::size_t index = 0; index < before.rows.size(); ++index)
                byKey[before.rows[index].wav.toLowerCase() + "|"
                      + juce::String(juce::roundToInt(
                            before.rows[index].offsetMs * 2.0))].push_back(index);
            std::vector<int> drop;
            for (const auto& sameKey : byKey)
            {
                if (sameKey.second.size() < 2) continue;
                auto keep = sameKey.second.front();
                for (const auto index : sameKey.second)
                    if (before.classes[index] != "CVVV") { keep = index; break; }
                for (const auto index : sameKey.second)
                    if (index != keep) drop.push_back(before.rows[index].lineIndex);
            }
            std::sort(drop.begin(), drop.end(), std::greater<int>());
            for (const auto line : drop)
                if (juce::isPositiveAndBelow(line, lines.size()))
                {
                    lines.remove(line);
                    ++merged;
                }
        }
        const auto existing = readMouRows(mouFile);
        // A 谋 row belongs to a wav and an offset, not to an alias, so the
        // several aliases a voicebank often points at one sample region share
        // one row.  Without remembering what this pass has already added, each
        // of them appended its own copy -- and duplicates are exactly what the
        // reader and the writer used to disagree about.
        std::set<juce::String> addedHere;
        for (const auto* entry : group.second)
        {
            const auto key = entry->sourceName.toLowerCase() + "|"
                + juce::String(juce::roundToInt(entry->offsetMs * 2.0));
            if (findMouRow(existing, entry->sourceName, entry->offsetMs) >= 0
                || addedHere.count(key) > 0)
            {
                ++kept;                       // never overwrite an annotation
                continue;
            }
            lines.add(mouRowText(*entry, "CVVV", {}));
            addedHere.insert(key);
            ++written;
        }
        if (!mouFile.replaceWithText(lines.joinIntoString("\n") + "\n", false, false, "\n"))
        {
            error = "Could not write " + mouFile.getFullPathName();
            return false;
        }
    }
    return true;
}

bool SampleSettings::updateJieOtoEntry(const VoicebankOtoEntry& original,
                                       const VoicebankOtoEntry& updated,
                                       juce::String& error)
{
    if (!updated.otoFile.existsAsFile())
    {
        error = "oto.ini not found: " + updated.otoFile.getFullPathName();
        return false;
    }
    const auto jieFile = jieOtoFileFor(updated.otoFile);
    auto lines = jieFile.existsAsFile()
        ? juce::StringArray::fromLines(decodeOtoText(jieFile))
        : jieHeaderLines();
    while (lines.size() > 0 && lines[lines.size() - 1].trim().isEmpty())
        lines.remove(lines.size() - 1);

    // Rows are keyed by the entry's offset, so an edit that moves the offset
    // has to be located by the offset it had before.  Looking it up by the new
    // one leaves the old row orphaned and appends a duplicate, and the engine
    // then finds two rows for the same sample.
    const auto rows = readJieRows(jieFile);
    const auto* existing = findJieRow(rows, original.sourceName, original.offsetMs);
    if (existing == nullptr)
        existing = findJieRow(rows, updated.sourceName, updated.offsetMs);
    juce::StringArray extras;
    if (existing != nullptr)
        for (int index = 4; index < existing->fields.size(); ++index)
            extras.add(existing->fields[index]);
    const auto text = jieRowText(updated, extras);
    if (existing != nullptr && juce::isPositiveAndBelow(existing->lineIndex, lines.size()))
        lines.set(existing->lineIndex, text);
    else
        lines.add(text);

    if (!jieFile.replaceWithText(lines.joinIntoString("\n") + "\n", false, false, "\n"))
    {
        error = "Could not write " + jieFile.getFullPathName();
        return false;
    }
    return true;
}

juce::String SampleSettings::entryLookupName(const VoicebankOtoEntry& entry)
{
    // An oto line may leave the alias empty, in which case UTAU addresses the
    // sample by its file stem.
    return entry.alias.trim().isNotEmpty()
        ? entry.alias.trim() : entry.sourceName.upToLastOccurrenceOf(".", false, true);
}

int SampleSettings::findEntryForAlias(const std::vector<VoicebankOtoEntry>& entries,
                                      const juce::String& alias)
{
    if (entries.empty()) return -1;
    const auto wanted = alias.trim();
    if (wanted.isEmpty()) return 0;

    std::vector<int> order(entries.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = static_cast<int>(index);
    const auto nameOf = [&entries](int index)
    {
        return entryLookupName(entries[static_cast<std::size_t>(index)]);
    };
    for (const auto index : order)
        if (nameOf(index).equalsIgnoreCase(wanted)) return index;

    // No such alias: fall back to the alphabetically nearest one.  Sort by
    // name, find where the wanted alias would go, and pick whichever of the two
    // neighbours shares the longer prefix with it.
    std::stable_sort(order.begin(), order.end(), [&](int left, int right)
    {
        return nameOf(left).compareIgnoreCase(nameOf(right)) < 0;
    });
    std::size_t position = 0;
    while (position < order.size()
           && nameOf(order[position]).compareIgnoreCase(wanted) < 0)
        ++position;
    if (position == 0) return order.front();
    if (position >= order.size()) return order.back();

    const auto shared = [](const juce::String& a, const juce::String& b)
    {
        const auto lowerA = a.toLowerCase(), lowerB = b.toLowerCase();
        int count = 0;
        while (count < lowerA.length() && count < lowerB.length()
               && lowerA[count] == lowerB[count])
            ++count;
        return count;
    };
    const auto after = order[position];
    const auto before = order[position - 1];
    return shared(nameOf(before), wanted) > shared(nameOf(after), wanted) ? before : after;
}
}
