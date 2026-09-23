#include "MainComponent.h"
#include "StartupLog.h"
#include "backend/McpServer.h"
#include "backend/AnalysisService.h"
#include "backend/NativeAnalyzer.h"
#include "backend/FcpeAnalyzer.h"
#include "backend/UtauRenderer.h"
#include "backend/UstImporter.h"
#include "backend/Llsm2Renderer.h"
#include "backend/NsfHifiganRenderer.h"
#include "AudioEngine.h"
#include "OtoWaveformEditorComponent.h"
#include "VoicebankSettingsComponent.h"
#include "PianoRollComponent.h"
#include "TimelineComponent.h"
#include "TrackListComponent.h"
#include "AssetManagerComponent.h"
#include "SampleSettings.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <limits>

namespace hachi
{
class HachiShifterApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "HachiShifter Next"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        startupLog("Application: initialise " + getApplicationVersion());
        auto arguments = juce::StringArray::fromTokens(commandLine, true);
        if (!arguments.isEmpty() && arguments[0] == "--mcp")
        {
            backend::McpServer server;
            setApplicationReturnValue(server.run());
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-oto-editor")
        {
            // Builds the editor off screen for one sample and reports what it
            // laid out.  Dialogs are otherwise unreachable without a mouse,
            // which left their behaviour unverifiable.
            const juce::File bank(arguments[1].unquoted());
            const auto wanted = arguments[2].unquoted();
            const auto jieMode = arguments.size() >= 4
                && (arguments[3] == "jie" || arguments[3] == "mou");
            const auto mouMode = arguments.size() >= 4 && arguments[3] == "mou";
            juce::StringArray otoWarnings;
            const auto rows = SampleSettings::loadVoicebankOto(bank, otoWarnings, jieMode,
                                                              mouMode);
            const VoicebankOtoEntry* found = nullptr;
            for (const auto& row : rows)
                if (row.alias == wanted || row.sourceName == wanted
                    || row.audioFile.getFileNameWithoutExtension() == wanted)
                { found = &row; break; }
            if (found == nullptr)
            {
                std::cout << "found=0" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            OtoWaveformEditorComponent editor(*found, jieMode, mouMode, [] {});
            editor.setBounds(0, 0, 1460, 800);
            editor.resized();
            // What the entry actually opened with, before anything is forced.
            std::cout << "opened_alias=" << editor.diagnosticClasses()
                      << "|entry_classes=" << found->mouClasses
                      << "|offset=" << found->offsetMs
                      << "|alias=" << found->alias << std::endl;
            // The zoom captions have to fit the buttons they are drawn in.
            juce::String zoomReport;
            const auto zoomFits = editor.diagnosticZoomCaptionsFit(&zoomReport);
            if (mouMode)
            {
                // Two, three and four regions each draw their own number of
                // bands and offer their own number of boundary editors --
                // a two-region entry used to be shown with three lines, two
                // of which pointed past the end of it.
                auto countsHold = true;
                juce::String shot;
                for (const auto count : { 4, 3, 2 })
                {
                    editor.diagnosticSetRegionCount(count);
                    editor.resized();
                    countsHold = countsHold
                        && editor.diagnosticRegionCount() == count
                        && editor.diagnosticClasses().length() == count;
                    // One boundary editor fewer per region fewer.
                    for (int index = 0; index < 3; ++index)
                        countsHold = countsHold
                            && editor.diagnosticParameterShown(5 + index)
                                   == (index + 1 < count);
                    shot += " " + juce::String(count) + ":"
                        + editor.diagnosticClasses();
                    const auto picture = editor.createComponentSnapshot(
                        editor.getLocalBounds(), true, 1.0f);
                    juce::File file(juce::File(arguments[2].unquoted() + ".png")
                        .getSiblingFile("oto-" + juce::String(count) + ".png"));
                    if (arguments.size() >= 5)
                    {
                        file = juce::File(arguments[4].unquoted())
                            .getSiblingFile(juce::File(arguments[4].unquoted())
                                .getFileNameWithoutExtension()
                                + "-" + juce::String(count) + ".png");
                        file.deleteFile();
                        juce::PNGImageFormat png;
                        std::unique_ptr<juce::FileOutputStream> out(
                            file.createOutputStream());
                        if (out != nullptr) png.writeImageToStream(picture, *out);
                    }
                }
                std::cout << "zoom_captions_fit=" << (zoomFits ? 1 : 0)
                      << "|zoom" << zoomReport << "|"
                      << "counts_hold=" << (countsHold ? 1 : 0)
                          << "|classes" << shot << std::endl;
                setApplicationReturnValue(countsHold && zoomFits ? 0 : 4);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            static const char* names[] { "offset", "consonant", "cutoff",
                                          "preutterance", "overlap",
                                          "jie_onset", "jie_glide", "jie_nucleus" };
            std::cout << "found=1\n"
                      << "in_oto=" << (found->lineIndex >= 0 ? 1 : 0) << '\n';
            for (int index = 0; index < 8; ++index)
                std::cout << names[index] << "="
                          << (editor.diagnosticParameterShown(index)
                              ? editor.diagnosticParameterText(index)
                              : juce::String("hidden"))
                          << '\n';
            // Optional: move the offset handle and print the entry again, so
            // the effect of a drag on every other boundary is measurable.
            // A negative offset means "do not drag", so the zoom argument
            // after it can be used on its own.
            if (arguments.size() >= 5 && arguments[4].getDoubleValue() >= 0.0)
            {
                editor.diagnosticDragOffsetTo(arguments[4].getDoubleValue());
                for (int index = 0; index < 8; ++index)
                    std::cout << "after_" << names[index] << "="
                              << (editor.diagnosticParameterShown(index)
                                  ? editor.diagnosticParameterText(index)
                                  : juce::String("hidden"))
                              << '\n';
            }
            // Optional: click the zoom-out button this many times first, to
            // measure how far past the file the view can be opened.
            if (arguments.size() >= 6)
                for (auto click = arguments[5].getIntValue(); click > 0; --click)
                    editor.diagnosticZoomOut();
            const auto scroll = editor.diagnosticScrollBounds();
            std::cout << "total_ms=" << editor.diagnosticTotalMs() << '\n'
                      << "visible_ms=" << editor.diagnosticVisibleMs() << '\n';
            std::cout << "scrollbar=" << scroll.getX() << ',' << scroll.getY() << ','
                      << scroll.getWidth() << ',' << scroll.getHeight() << '\n'
                      << "scroll_enabled=" << (editor.diagnosticScrollEnabled() ? 1 : 0)
                      << std::endl;
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-splice-envelope")
        {
            // Splicing crossfades the overlap, and the roll has to draw that:
            // the later note's rise has to reach the instant the earlier one
            // stops sounding, and the earlier one's fall has to start where the
            // later one begins.  A note carrying a stored envelope -- which is
            // every note that has had a lyric typed into it -- must be shaped
            // the same way, or the splice is invisible and looks broken.
            I18n spliceStrings;
            ProjectModel spliceProject;
            juce::String spliceError;
            if (!spliceProject.load(juce::File(arguments[1].unquoted()), spliceError))
            {
                std::cout << "loaded=0|error=" << spliceError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent spliceRoll(spliceProject, spliceStrings);
            spliceRoll.setBounds(0, 0, 1600, 900);
            spliceRoll.resized();
            spliceRoll.diagnosticRefresh();
            // Find a touching pair, which is all a splice can join.
            const auto notesOf = [&] { return spliceRoll.diagnosticNotes(); };
            auto pair = std::size_t(0);
            {
                const auto drawn = notesOf();
                for (std::size_t index = 1; index < drawn.size(); ++index)
                    if (std::abs(drawn[index - 1].end - drawn[index].start) < 0.002
                        && drawn[index].soundingStart < drawn[index - 1].soundingEnd)
                    { pair = index; break; }
            }
            if (pair == 0)
            {
                std::cout << "no_touching_pair_with_overlap" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto laterId = notesOf()[pair].id;

            // The rise of the later note, and the fall of the earlier one.
            // Keep the snapshot alive: it comes back by value, and a reference
            // reaching into the temporary dangles.
            const auto riseEnd = [&](std::size_t index)
            {
                const auto drawn = notesOf();
                const auto& points = drawn[index].envelope;
                return points.size() >= 2 ? points[1].timeSeconds : -1.0;
            };
            const auto fallStart = [&](std::size_t index)
            {
                const auto drawn = notesOf();
                const auto& points = drawn[index].envelope;
                return points.size() >= 2
                    ? points[points.size() - 2].timeSeconds : -1.0;
            };
            const auto measure = [&](bool stored)
            {
                // Either shape the notes carry their own envelope or they do
                // not; both have to answer to a splice.
                for (std::size_t index = pair - 1; index <= pair; ++index)
                {
                    const auto id = notesOf()[index].id;
                    if (stored)
                        spliceProject.setNoteAmplitudeEnvelope(id,
                            { { 0.0, -60.0f }, { 0.015, 0.0f },
                              { 0.30, 0.0f }, { 0.35, -60.0f } });
                    else
                        spliceProject.setNoteAmplitudeEnvelope(id, {});
                }
                spliceProject.setNotesUtauSplice({ laterId }, false);
                spliceRoll.diagnosticRefresh();
                const auto beforeRise = riseEnd(pair);
                const auto beforeFall = fallStart(pair - 1);
                spliceProject.setNotesUtauSplice({ laterId }, true);
                spliceRoll.diagnosticRefresh();
                const auto afterRise = riseEnd(pair);
                const auto afterFall = fallStart(pair - 1);
                // Where they have to land: the two instants the overlap runs
                // between, each read against its own note's start.
                const auto drawn = notesOf();
                const auto wantRise = drawn[pair - 1].soundingEnd - drawn[pair].start;
                const auto wantFall = drawn[pair].soundingStart - drawn[pair - 1].start;
                const auto risen = std::abs(afterRise - wantRise) < 1.0e-6;
                const auto fell = std::abs(afterFall - wantFall) < 1.0e-6;
                std::cout << (stored ? "  stored " : "  default")
                          << " rise " << juce::String(beforeRise, 4)
                          << " -> " << juce::String(afterRise, 4)
                          << " (want " << juce::String(wantRise, 4) << ")"
                          << "   fall " << juce::String(beforeFall, 4)
                          << " -> " << juce::String(afterFall, 4)
                          << " (want " << juce::String(wantFall, 4) << ")"
                          << std::endl;
                return risen && fell
                    && std::abs(afterRise - beforeRise) > 1.0e-6
                    && std::abs(afterFall - beforeFall) > 1.0e-6;
            };

            const auto defaultShaped = measure(false);
            const auto storedShaped = measure(true);
            std::cout << "pair=" << pair
                      << "|default_crossfades=" << (defaultShaped ? 1 : 0)
                      << "|stored_crossfades=" << (storedShaped ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(defaultShaped && storedShaped ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (!arguments.isEmpty() && arguments[0] == "--smoke-dialog-enter")
        {
            // Return must not stand in for pressing Apply.  The dialogs stop it
            // by not registering the key against any button, and this is the
            // behaviour that makes that enough: JUCE fires a button for a key
            // it is registered against, and otherwise answers Return only when
            // a window has a single button.  Every dialog here has two or
            // three, so nothing else has to be done -- but if that ever
            // changed, Return would quietly start applying things again.
            // keyPressed is protected, so ask through a window of our own.
            struct Probe : juce::AlertWindow
            {
                using juce::AlertWindow::AlertWindow;
                bool press(const juce::KeyPress& key) { return keyPressed(key); }
            };
            const auto pressReturn = [](Probe& window)
            {
                return window.press(juce::KeyPress(juce::KeyPress::returnKey));
            };
            Probe twoButtons("t", "m", juce::MessageBoxIconType::NoIcon);
            twoButtons.addButton("Apply", 1);
            twoButtons.addButton("Cancel", 0);
            const auto ignoresReturn = !pressReturn(twoButtons);

            // The same window with the shortcut back on: Return fires it.  If
            // this did not hold, the check above would pass for the wrong
            // reason and would never catch the binding coming back.
            Probe bound("t", "m", juce::MessageBoxIconType::NoIcon);
            bound.addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
            bound.addButton("Cancel", 0);
            const auto shortcutStillWorks = pressReturn(bound);

            // A lone button still answers Return, which is why the button count
            // matters; stated here so the reason is on the record.
            Probe onlyOne("t", "m", juce::MessageBoxIconType::NoIcon);
            onlyOne.addButton("OK", 1);
            const auto singleButtonAnswers = pressReturn(onlyOne);

            // Escape still cancels: nothing is applied by it, and it is the
            // one key nobody presses by accident while typing a number.
            Probe escapable("t", "m", juce::MessageBoxIconType::NoIcon);
            escapable.addButton("Apply", 1);
            escapable.addButton("Cancel", 0);
            const auto escapeCancels =
                escapable.press(juce::KeyPress(juce::KeyPress::escapeKey));

            std::cout << "two_buttons_ignore_return=" << (ignoresReturn ? 1 : 0)
                      << "|a_bound_shortcut_still_fires=" << (shortcutStillWorks ? 1 : 0)
                      << "|single_button_answers_return=" << (singleButtonAnswers ? 1 : 0)
                      << "|escape_still_cancels=" << (escapeCancels ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                ignoresReturn && shortcutStillWorks && singleButtonAnswers
                    && escapeCancels ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (arguments.size() >= 2 && arguments[0] == "--smoke-source-edit-view")
        {
            // The loudness lane and the point tool both ask to leave
            // source-edit mode on every click, whether or not it was on.  That
            // request must not move the view unless it really is leaving; and
            // when it is, it moves to a moment, not to a pixel column the
            // other view happens to be at.
            const auto staysPut = !MainComponent::viewSecondsLeavingSourceEdit(
                                       false, false, 12.5).has_value();
            const auto stillNothingWhenEntering =
                !MainComponent::viewSecondsLeavingSourceEdit(false, true, 12.5).has_value()
                && !MainComponent::viewSecondsLeavingSourceEdit(true, true, 12.5).has_value();
            const auto leaving = MainComponent::viewSecondsLeavingSourceEdit(
                true, false, 12.5);
            const auto goesBack = leaving && std::abs(*leaving - 12.5) < 1.0e-9;
            const auto neverNegative = [&]
            {
                const auto below = MainComponent::viewSecondsLeavingSourceEdit(
                    true, false, -4.0);
                return below && *below == 0.0;
            }();

            // And the conversion itself: a raw pixel copy is wrong whenever
            // the two views are at different scales.  Same arithmetic the
            // scroll sync uses, stated here so the two cannot drift apart.
            I18n viewStrings;
            ProjectModel viewProject;
            juce::String viewError;
            if (!viewProject.load(juce::File(arguments[1].unquoted()), viewError))
            {
                std::cout << "loaded=0|error=" << viewError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent viewRoll(viewProject, viewStrings);
            viewRoll.setBounds(0, 0, 1600, 900);
            viewRoll.setPixelsPerSecond(120.0f);
            TimelineComponent viewTimeline(viewProject);
            viewTimeline.setBounds(0, 0, 1600, 120);
            viewTimeline.setPixelsPerSecond(30.0f);
            constexpr auto timelineColumn = 600;
            const auto atSeconds = viewTimeline.secondsForPixel(timelineColumn);
            const auto converted = viewRoll.pixelForSeconds(atSeconds);
            const auto rawCopyWrong = std::abs(converted - timelineColumn) > 40;

            std::cout << "timeline_px=" << timelineColumn
                      << "|seconds=" << juce::String(atSeconds, 3)
                      << "|roll_px=" << converted
                      << "|no_move_when_already_off=" << (staysPut ? 1 : 0)
                      << "|no_move_when_entering=" << (stillNothingWhenEntering ? 1 : 0)
                      << "|goes_back_when_leaving=" << (goesBack ? 1 : 0)
                      << "|never_negative=" << (neverNegative ? 1 : 0)
                      << "|raw_pixel_copy_would_be_wrong=" << (rawCopyWrong ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                staysPut && stillNothingWhenEntering && goesBack && neverNegative
                    && rawCopyWrong ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (arguments.size() >= 1 && arguments[0] == "--smoke-consonant-hold")
        {
            // A region marked a consonant does not stretch with the note.  It
            // is treated the way the onset always has been: no share of the
            // note's length, its own natural rate, answering to the consonant
            // velocity and to nothing else.  Chinese codas -n / -ng are the
            // reason -- they sit in region 3 and used to stretch like vowels.
            using backend::UtauRenderer;
            const std::array<double, 4> source { 0.06, 0.09, 0.17, 0.12 };
            const auto leadIn = 0.06;
            const juce::String vowelCoda("CVVV");
            const juce::String consonantCoda("CVVC");

            const auto lengths = [&](double seconds, const juce::String* classes,
                                     int velocity = 100)
            {
                return UtauRenderer::regionSplit(source, seconds, velocity, leadIn,
                                                 nullptr, classes);
            };

            // Writing the default down changes nothing at all.
            auto defaultIsInert = true;
            for (const auto seconds : { 0.4, 0.8, 1.6, 3.0 })
            {
                const auto bare = lengths(seconds, nullptr);
                const auto spelt = lengths(seconds, &vowelCoda);
                defaultIsInert = defaultIsInert && bare.valid && spelt.valid;
                for (std::size_t k = 0; k < 4; ++k)
                    defaultIsInert = defaultIsInert
                        && std::abs(bare.seconds[k] - spelt.seconds[k]) < 1.0e-12;
            }

            // A coda marked a consonant holds its length while the note grows;
            // an unmarked one grows with it.
            auto codaHolds = true;
            auto vowelStillStretches = true;
            juce::String report;
            const auto shortest = lengths(0.4, &consonantCoda);
            for (const auto seconds : { 0.4, 0.8, 1.6, 3.0 })
            {
                const auto held = lengths(seconds, &consonantCoda);
                const auto loose = lengths(seconds, &vowelCoda);
                codaHolds = codaHolds && held.valid
                    && std::abs(held.seconds[3] - shortest.seconds[3]) < 1.0e-9
                    // and the onset is still the lead-in, as it always was
                    && std::abs(held.seconds[0] - leadIn) < 1.0e-9;
                // the vowels still absorb everything
                {
                    auto total = 0.0;
                    for (const auto value : held.seconds) total += value;
                    codaHolds = codaHolds && std::abs(total - seconds) < 1.0e-9;
                }
                vowelStillStretches = vowelStillStretches
                    && loose.seconds[3] > shortest.seconds[3] * (seconds > 0.5 ? 1.5 : 0.5);
                report += " " + juce::String(seconds, 1) + "s:"
                    + juce::String(held.seconds[3] * 1000.0, 1) + "/"
                    + juce::String(loose.seconds[3] * 1000.0, 1);
            }

            // Consonant velocity still reaches it -- that is the one thing
            // that is meant to move it.
            const auto slow = lengths(1.6, &consonantCoda, 50);
            const auto fast = lengths(1.6, &consonantCoda, 200);
            const auto velocityReaches = slow.seconds[3] > fast.seconds[3] * 1.5;

            // And it reaches nothing else.  With every region called a vowel
            // the velocity has no consonant to act on, so the split does not
            // move at all; it used to scale region 1 whatever the entry said,
            // which is a 界 rule -- the onset and the glide -- that 谋 does not
            // inherit now the entry names its consonants itself.
            const juce::String allVowel("VVVV");
            const auto slowVowels = lengths(1.6, &allVowel, 50);
            const auto fastVowels = lengths(1.6, &allVowel, 200);
            auto velocityIsTheConsonants = slowVowels.valid && fastVowels.valid;
            for (std::size_t k = 0; k < 4; ++k)
                velocityIsTheConsonants = velocityIsTheConsonants
                    && std::abs(slowVowels.seconds[k] - fastVowels.seconds[k]) < 1.0e-9;
            // Name region 1 a consonant and it answers again.
            const juce::String glideConsonant("CCVV");
            const auto slowGlide = lengths(1.6, &glideConsonant, 50);
            const auto fastGlide = lengths(1.6, &glideConsonant, 200);
            velocityIsTheConsonants = velocityIsTheConsonants
                && slowGlide.seconds[1] > fastGlide.seconds[1] * 1.5;

            // Two and three regions, with the user saying which is the vowel.
            // The rule is the same at every count: region 0 ends where the
            // note starts, whatever it is -- the lead-in and the first
            // boundary are one instant seen from two sides.  After it, what is
            // marked V carries the note and what is marked C keeps its own
            // length; when nothing after region 0 may stretch, those regions
            // fill the note between them, because the note still has to be its
            // own length.
            auto everyCountHolds = true;
            juce::String countReport;
            for (const auto* spelling : { "VC", "CVC", "CVV" })
            {
                const juce::String classes(spelling);
                const auto count = classes.length();
                const auto shortNote = lengths(0.5, &classes);
                const auto longNote = lengths(2.5, &classes);
                everyCountHolds = everyCountHolds && shortNote.valid && longNote.valid
                    // The first boundary is the note start in both, and a
                    // first region called a vowel does not change that.
                    && std::abs(shortNote.seconds[0] - leadIn) < 1.0e-9
                    && std::abs(longNote.seconds[0] - leadIn) < 1.0e-9;
                auto anythingStretches = false;
                for (int k = 1; k < count; ++k)
                    anythingStretches = anythingStretches || classes[k] == 'V';
                auto total = 0.0, stretched = 0.0;
                for (int k = 0; k < count; ++k)
                {
                    const auto index = static_cast<std::size_t>(k);
                    total += longNote.seconds[index];
                    if (k == 0) continue;              // checked above
                    if (classes[k] == 'V' || !anythingStretches)
                        stretched += longNote.seconds[index] - shortNote.seconds[index];
                    else
                        // A consonant is the same length in both notes.
                        everyCountHolds = everyCountHolds
                            && std::abs(longNote.seconds[index]
                                        - shortNote.seconds[index]) < 1.0e-9;
                }
                // Nothing spills into the regions this entry does not have.
                for (int k = count; k < 4; ++k)
                    everyCountHolds = everyCountHolds
                        && longNote.seconds[static_cast<std::size_t>(k)] < 1.0e-12;
                everyCountHolds = everyCountHolds
                    && std::abs(total - 2.5) < 1.0e-9
                    && stretched > 1.9;
                countReport += " " + classes + ":"
                    + juce::String(stretched * 1000.0, 0) + "ms[";
                for (int k = 0; k < 4; ++k)
                    countReport += (k ? "/" : "")
                        + juce::String(longNote.seconds[
                            static_cast<std::size_t>(k)] * 1000.0, 0);
                countReport += "]";
            }

            // Hand-placed boundaries divide only the regions the entry has.
            // A note whose entry is split three ways used to hand the fourth
            // region a share of the note as soon as anyone dragged a boundary.
            auto handRespectsTheCount = true;
            juce::String handReport;
            for (const auto* spelling : { "CV", "CVC", "CVVC" })
            {
                const juce::String classes(spelling);
                const auto count = classes.length();
                const std::array<double, 3> byHand { 0.1, 0.45, 0.7 };
                const auto split = UtauRenderer::regionSplit(source, 2.0, 100,
                                                             leadIn, &byHand, &classes);
                auto total = 0.0;
                for (const auto value : split.seconds) total += value;
                handRespectsTheCount = handRespectsTheCount && split.valid
                    && std::abs(total - 2.0) < 1.0e-9;
                // Nothing at all in the regions past the count.
                for (int k = count; k < 4; ++k)
                    handRespectsTheCount = handRespectsTheCount
                        && split.seconds[static_cast<std::size_t>(k)] < 1.0e-12;
                handReport += " " + classes + ":[";
                for (int k = 0; k < 4; ++k)
                    handReport += (k ? "/" : "")
                        + juce::String(split.seconds[
                            static_cast<std::size_t>(k)] * 1000.0, 0);
                handReport += "]";
            }

            // Both extremes the tick boxes reach: nothing marked, and
            // everything marked.  With no vowel there is nothing that may
            // stretch, so the regions keep their proportions -- but the note
            // still has to come out the length it was asked for.
            auto extremesHold = true;
            juce::String extremeReport;
            for (const auto* spelling : { "VVVV", "CCCC" })
            {
                const juce::String classes(spelling);
                for (const auto seconds : { 0.4, 1.6, 3.0 })
                {
                    const auto split = lengths(seconds, &classes);
                    auto total = 0.0;
                    for (const auto value : split.seconds) total += value;
                    extremesHold = extremesHold && split.valid
                        && std::abs(total - seconds) < 1.0e-9
                        // Even with nothing that may stretch, the first
                        // boundary stays on the note start.
                        && std::abs(split.seconds[0] - leadIn) < 1.0e-9;
                }
                const auto shown = lengths(1.6, &classes);
                extremeReport += " " + classes + ":[";
                for (int k = 0; k < 4; ++k)
                    extremeReport += (k ? "/" : "")
                        + juce::String(shown.seconds[
                            static_cast<std::size_t>(k)] * 1000.0, 0);
                extremeReport += "]";
            }

            std::cout << "default_is_inert=" << (defaultIsInert ? 1 : 0)
                      << "|by_hand_respects_the_count="
                      << (handRespectsTheCount ? 1 : 0) << "|by_hand" << handReport
                      << "|all_vowel_and_all_consonant_hold=" << (extremesHold ? 1 : 0)
                      << "|extremes" << extremeReport
                      << "|every_count_holds=" << (everyCountHolds ? 1 : 0)
                      << "|vowel_took" << countReport
                      << "|coda_holds=" << (codaHolds ? 1 : 0)
                      << "|vowel_still_stretches=" << (vowelStillStretches ? 1 : 0)
                      << "|velocity_still_reaches_it=" << (velocityReaches ? 1 : 0)
                      << "|velocity_is_the_consonants_only="
                      << (velocityIsTheConsonants ? 1 : 0)
                      << "|vowels_50_vs_200=" << juce::String(slowVowels.seconds[1] * 1000.0, 1)
                      << "/" << juce::String(fastVowels.seconds[1] * 1000.0, 1)
                      << "|coda_ms_held/loose" << report
                      << "|velocity_50_vs_200=" << juce::String(slow.seconds[3] * 1000.0, 1)
                      << "/" << juce::String(fast.seconds[3] * 1000.0, 1)
                      << std::endl;
            setApplicationReturnValue(defaultIsInert && extremesHold
                                      && handRespectsTheCount && velocityIsTheConsonants
                                      && codaHolds && everyCountHolds
                                      && vowelStillStretches && velocityReaches ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-mou-panel-counts")
        {
            // The same 2/3/4 choice the waveform editor offers, in the panel
            // beside the entry list.  There is no save button here, so each
            // click has to reach otomou.ini by itself, and the buttons have to
            // show what the selected entry actually is.
            const juce::File bank(arguments[1].unquoted());
            const auto mouFile = bank.getChildFile("otomou.ini");
            const auto jieFile = bank.getChildFile("oto4.ini");
            mouFile.deleteFile();
            auto written = 0, kept = 0;
            juce::String error;
            auto merged = 0;
            SampleSettings::createMouOto(bank, written, kept, merged, error);
            const auto jieBefore = jieFile.loadFileAsString();

            VoicebankSettingsComponent panel(bank, true, true);
            panel.setBounds(0, 0, 1040, 560);
            panel.resized();
            panel.diagnosticSelectRow(0);
            const auto hasRow = panel.diagnosticRowCount() > 0;

            // A seeded entry starts at the four regions an oto4 row has, and
            // each click lands on disk rather than waiting for a save.
            const auto readBack = [&bank]
            {
                juce::StringArray warnings;
                const auto rows = SampleSettings::loadVoicebankOto(bank, warnings,
                                                                   true, true);
                return rows.empty() ? VoicebankOtoEntry() : rows.front();
            };
            const auto startsAtFour = panel.diagnosticToggledCount() == 4;
            const auto four = readBack();
            auto followsClicks = true;
            juce::String trace;
            for (const auto count : { 2, 3, 4, 2 })
            {
                panel.diagnosticSetRegionCount(count);
                const auto onDisk = readBack();
                followsClicks = followsClicks
                    && panel.diagnosticToggledCount() == count
                    && panel.diagnosticClasses().length() == count
                    && onDisk.mouClasses.length() == count;
                trace += " " + juce::String(count) + ":btn"
                    + juce::String(panel.diagnosticToggledCount())
                    + ",mem" + panel.diagnosticClasses()
                    + ",disk" + onDisk.mouClasses;
            }
            // Down to two regions and back up to four: the boundaries the
            // smaller count does not reach have to come back, or an entry
            // loses them the moment anyone tries a different split.
            panel.diagnosticSetRegionCount(4);
            const auto again = readBack();
            const auto boundariesSurvive =
                std::abs(again.jieOnsetMs - four.jieOnsetMs) < 0.001
                && std::abs(again.jieGlideMs - four.jieGlideMs) < 0.001
                && std::abs(again.jieNucleusMs - four.jieNucleusMs) < 0.001;
            const auto jieUntouched = jieFile.loadFileAsString() == jieBefore;

            if (arguments.size() >= 3)
            {
                const juce::File shot(arguments[2].unquoted());
                shot.deleteFile();
                juce::PNGImageFormat png;
                juce::FileOutputStream stream(shot);
                png.writeImageToStream(panel.createComponentSnapshot(
                    panel.getLocalBounds(), true, 1.0f), stream);
            }

            // 界 has no such choice: the buttons are not built at all there.
            VoicebankSettingsComponent jiePanel(bank, true, false);
            jiePanel.setBounds(0, 0, 1040, 560);
            jiePanel.resized();
            jiePanel.diagnosticSelectRow(0);
            const auto jieHasNoCounts = jiePanel.diagnosticToggledCount() == 0;

            mouFile.deleteFile();
            std::cout << "row=" << (hasRow ? 1 : 0)
                      << "|starts_at_four=" << (startsAtFour ? 1 : 0)
                      << "|follows_clicks=" << (followsClicks ? 1 : 0)
                      << "|trace" << trace
                      << "|boundaries_survive=" << (boundariesSurvive ? 1 : 0)
                      << "|jie_file_untouched=" << (jieUntouched ? 1 : 0)
                      << "|jie_has_no_counts=" << (jieHasNoCounts ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(hasRow && startsAtFour && followsClicks
                                      && boundariesSurvive && jieUntouched
                                      && jieHasNoCounts ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-mou-consonant-ticks")
        {
            // Four tick boxes, one per region, any number of them ticked.  A
            // ticked region is a consonant; the rest are vowels, except a
            // silence, which the boxes cannot say and so must not destroy.
            const juce::File bank(arguments[1].unquoted());
            const auto wanted = arguments[2].unquoted();
            juce::StringArray warnings;
            const auto rows = SampleSettings::loadVoicebankOto(bank, warnings, true, true);
            const VoicebankOtoEntry* found = nullptr;
            for (const auto& row : rows)
                if (row.alias == wanted || row.sourceName == wanted
                    || row.audioFile.getFileNameWithoutExtension() == wanted)
                { found = &row; break; }
            if (found == nullptr)
            {
                std::cout << "found=0" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto build = [&found](const juce::String& classes)
            {
                auto entry = *found;
                entry.mouClasses = classes;
                return entry;
            };

            juce::String report;
            // Seeded CVVV: the onset ticked, nothing else.
            OtoWaveformEditorComponent editor(build("CVVV"), true, true, [] {});
            editor.setBounds(0, 0, 1460, 800);
            editor.resized();
            auto opensTicked = editor.diagnosticConsonantTicked(0)
                && !editor.diagnosticConsonantTicked(1)
                && !editor.diagnosticConsonantTicked(2)
                && !editor.diagnosticConsonantTicked(3);

            // Any number, none included.
            editor.diagnosticTickConsonant(3, true);
            const auto coda = editor.diagnosticClasses();
            editor.diagnosticTickConsonant(2, true);
            const auto three = editor.diagnosticClasses();
            editor.diagnosticTickConsonant(0, false);
            editor.diagnosticTickConsonant(2, false);
            editor.diagnosticTickConsonant(3, false);
            const auto none = editor.diagnosticClasses();
            const auto marksAny = coda == "CVVC" && three == "CVCC" && none == "VVVV";
            report += " coda=" + coda + " three=" + three + " none=" + none;

            // 谋 numbers its regions.  Any of them may be a consonant here, so
            // 声母/介音/韵腹/韵尾 would be asserting a structure the entry is
            // free to contradict -- and the number is what the tick boxes are
            // labelled with, so the wave and the boxes can be read together.
            OtoWaveformEditorComponent named(build("CVVC"), true, true, [] {});
            named.setBounds(0, 0, 1460, 800);
            named.resized();
            auto namesFollow = true;
            for (int index = 0; index < 4; ++index)
                namesFollow = namesFollow
                    && named.diagnosticRegionName(index)
                        == juce::String::fromUTF8("第") + juce::String(index + 1)
                           + juce::String::fromUTF8("区")
                    && named.diagnosticParameterLabel(5 + index >= 8 ? 7 : 5 + index)
                        .isNotEmpty();
            for (int index = 0; index < 3; ++index)
                namesFollow = namesFollow
                    && named.diagnosticParameterLabel(5 + index)
                        == juce::String::fromUTF8("第") + juce::String(index + 1)
                           + juce::String::fromUTF8("区末");
            report += " names=" + named.diagnosticRegionName(0) + ","
                + named.diagnosticRegionName(3) + ","
                + named.diagnosticParameterLabel(7);

            // 界 keeps the names it always had.  Its four regions really are
            // the four parts of a Chinese syllable, always in that order, and
            // it has no annotation that could say otherwise.
            OtoWaveformEditorComponent jieView(build({}), true, false, [] {});
            jieView.setBounds(0, 0, 1460, 800);
            jieView.resized();
            const std::array<const char*, 4> classicNames {
                "声母", "介音", "韵腹", "韵尾" };
            for (int index = 0; index < 4; ++index)
                namesFollow = namesFollow
                    && jieView.diagnosticRegionName(index)
                        == juce::String::fromUTF8(classicNames[
                            static_cast<std::size_t>(index)]);
            namesFollow = namesFollow
                && jieView.diagnosticParameterLabel(5)
                    == juce::String::fromUTF8("声母末");
            report += " jie=" + jieView.diagnosticRegionName(3) + ","
                + jieView.diagnosticParameterLabel(5);

            // An entry that opens at two regions and is then grown: the boxes
            // it did not open with have to arrive named and filled in, not as
            // blanks or as nothing at all.
            OtoWaveformEditorComponent grown(build("CV"), true, true, [] {});
            grown.setBounds(0, 0, 1460, 800);
            grown.resized();
            grown.diagnosticSetRegionCount(4);
            grown.resized();
            auto grewComplete = true;
            for (int index = 5; index < 8; ++index)
                grewComplete = grewComplete
                    && grown.diagnosticParameterAttached(index)
                    && grown.diagnosticParameterShown(index)
                    && grown.diagnosticParameterLabel(index).isNotEmpty()
                    && grown.diagnosticParameterText(index).isNotEmpty();
            namesFollow = namesFollow && grewComplete;
            report += " grown=" + grown.diagnosticParameterLabel(7) + ":"
                + grown.diagnosticParameterText(7);

            // A region the count does not reach cannot be marked anything.
            OtoWaveformEditorComponent small(build("CV"), true, true, [] {});
            small.setBounds(0, 0, 1460, 800);
            small.resized();
            auto pastTheCount = small.diagnosticConsonantEnabled(0)
                && small.diagnosticConsonantEnabled(1)
                && !small.diagnosticConsonantEnabled(2)
                && !small.diagnosticConsonantEnabled(3);
            small.diagnosticTickConsonant(3, true);
            pastTheCount = pastTheCount && small.diagnosticClasses() == "CV";
            // Growing the entry re-opens the ones it now has.
            small.diagnosticSetRegionCount(3);
            pastTheCount = pastTheCount && small.diagnosticConsonantEnabled(2)
                && !small.diagnosticConsonantEnabled(3);
            report += " small=" + small.diagnosticClasses();

            // Unticking never turns a silence into a vowel.
            OtoWaveformEditorComponent silent(build("CVSV"), true, true, [] {});
            silent.setBounds(0, 0, 1460, 800);
            silent.resized();
            auto silenceHeld = !silent.diagnosticConsonantTicked(2);
            silent.diagnosticTickConsonant(3, true);
            silenceHeld = silenceHeld && silent.diagnosticClasses() == "CVSC";
            silent.diagnosticTickConsonant(2, true);
            silent.diagnosticTickConsonant(2, false);
            // Ticked and unticked again it becomes a vowel: the box said
            // consonant, and there is nothing left saying it was a silence.
            silenceHeld = silenceHeld && silent.diagnosticClasses() == "CVVC";
            report += " silence=" + silent.diagnosticClasses();

            std::cout << "opens_from_the_classes=" << (opensTicked ? 1 : 0)
                      << "|marks_any_number=" << (marksAny ? 1 : 0)
                      << "|regions_are_numbered=" << (namesFollow ? 1 : 0)
                      << "|nothing_past_the_count=" << (pastTheCount ? 1 : 0)
                      << "|silence_survives_a_mark=" << (silenceHeld ? 1 : 0)
                      << "|" << report.trim() << std::endl;
            setApplicationReturnValue(opensTicked && marksAny && namesFollow
                                      && pastTheCount && silenceHeld ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-mou-boundary-drag")
        {
            // Two and three regions leave the boundaries past the count sitting
            // where they were -- switching the count from the voicebank panel
            // only rewrites the class string, so those values are real times in
            // the middle of the wave.  They are not drawn, and they must not act
            // on the drag either: the last boundary a count has runs to the end
            // of the entry, and a press must never take hold of a line that is
            // not there.
            const juce::File bank(arguments[1].unquoted());
            const auto wanted = arguments[2].unquoted();
            juce::StringArray warnings;
            const auto rows = SampleSettings::loadVoicebankOto(bank, warnings, true, true);
            const VoicebankOtoEntry* found = nullptr;
            for (const auto& row : rows)
                if (row.alias == wanted || row.sourceName == wanted
                    || row.audioFile.getFileNameWithoutExtension() == wanted)
                { found = &row; break; }
            if (found == nullptr)
            {
                std::cout << "found=0" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }

            juce::String report;
            auto reaches = true, ghostFree = true;
            for (const auto count : { 2, 3, 4 })
            {
                // Built fresh each time, with the boundaries the file holds:
                // diagnosticSetRegionCount collapses the ones it drops, which
                // is exactly the state that hides the bug.
                auto entry = *found;
                entry.mouClasses = SampleSettings::mouClassesForCount("CVVV", count);
                OtoWaveformEditorComponent editor(entry, true, true, [] {});
                editor.setBounds(0, 0, 1460, 800);
                editor.resized();
                const auto span = editor.diagnosticEntrySpanMs();

                // The last boundary this count has must reach the end of the
                // entry.  Dragged well past it, it should land on the end.
                const auto last = count - 2;
                editor.diagnosticDragBoundary(last, span * 4.0);
                const auto landed = editor.diagnosticBoundaryMs(last);
                const auto reached = std::abs(landed - span) < 0.5;
                reaches = reaches && reached;
                report += " " + juce::String(count) + ":b"
                    + juce::String(last + 1) + "->"
                    + juce::String(landed, 1) + "/" + juce::String(span, 1);

                // Typing the same value into the box has to reach the same
                // place: a hidden box still holds its text, and reading it
                // walled the boundary the entry does have.
                auto typedEntry = *found;
                typedEntry.mouClasses = SampleSettings::mouClassesForCount("CVVV", count);
                OtoWaveformEditorComponent typed(typedEntry, true, true, [] {});
                typed.setBounds(0, 0, 1460, 800);
                typed.resized();
                typed.diagnosticTypeParameter(5 + last, juce::String(span * 4.0, 3));
                const auto typedLanded = typed.diagnosticBoundaryMs(last);
                reaches = reaches && std::abs(typedLanded - span) < 0.5;
                report += "/typed" + juce::String(typedLanded, 1);

                // And nothing beyond the count may answer a press.  Put the
                // dropped boundaries back where the file had them first.
                auto probe = *found;
                probe.mouClasses = SampleSettings::mouClassesForCount("CVVV", count);
                OtoWaveformEditorComponent fresh(probe, true, true, [] {});
                fresh.setBounds(0, 0, 1460, 800);
                fresh.resized();
                for (int index = count - 1; index < 3; ++index)
                {
                    const auto grabbed =
                        fresh.diagnosticHandleAtX(fresh.diagnosticXForBoundary(index));
                    const auto ghost = grabbed == "b" + juce::String(index + 1);
                    ghostFree = ghostFree && !ghost;
                    report += " " + juce::String(count) + ":x(b"
                        + juce::String(index + 1) + ")=" + grabbed;
                }
            }

            std::cout << "last_boundary_reaches_the_end=" << (reaches ? 1 : 0)
                      << "|no_ghost_handles=" << (ghostFree ? 1 : 0)
                      << "|" << report.trim() << std::endl;
            setApplicationReturnValue(reaches && ghostFree ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-prefix-map")
        {
            // A voicebank recorded at several pitches keeps each one in its own
            // folder and names the pitch in the alias; prefix.map says which
            // pitch takes which.  Its two columns are literal -- a Chinese CVVC
            // bank writes the suffix as " H", with the space that separates it
            // from the lyric -- so reading them trimmed asked for "aH" and
            // resolved nothing at any pitch.
            const juce::File bank(arguments[1].unquoted());
            const auto lyric = arguments[2].unquoted();
            juce::String report;
            auto resolved = true, followsThePitch = true;
            juce::StringArray seen;
            // One pitch from each band the map names, low to high.
            for (const auto note : { 36.0f, 48.0f, 60.0f, 69.0f, 71.0f, 84.0f })
            {
                const auto timing = backend::UtauRenderer::sampleTiming(
                    bank, lyric, note, 100, false, false);
                const auto alias = timing ? timing->resolvedAlias : juce::String();
                resolved = resolved && alias.isNotEmpty();
                // Whatever it resolved to has to start with the lyric: the map
                // wraps it, it does not replace it.
                followsThePitch = followsThePitch
                    && (alias.isEmpty() || alias.startsWith(lyric));
                seen.addIfNotAlreadyThere(alias);
                report += " " + juce::String(juce::roundToInt(note)) + ":"
                    + (alias.isEmpty() ? juce::String("-") : alias);
            }
            // And the pitch really chooses: a map that reaches more than one
            // band must not answer with the same sample everywhere.
            const auto picksByPitch = seen.size() > 1;

            std::cout << "every_pitch_resolves=" << (resolved ? 1 : 0)
                      << "|alias_keeps_the_lyric=" << (followsThePitch ? 1 : 0)
                      << "|the_pitch_picks_the_bank=" << (picksByPitch ? 1 : 0)
                      << "|" << report.trim() << std::endl;
            setApplicationReturnValue(resolved && followsThePitch && picksByPitch
                                      ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--repair-mou-oto")
        {
            // The 生成谋•OTO button, without the window: seed otomou.ini for
            // this voicebank and collapse any row that shares a wav and an
            // offset with another.  Useful on a bank annotated before seeding
            // stopped writing one row per alias -- those duplicates are why an
            // annotation could be saved and then read back as if it were not
            // there.
            const juce::File bank(arguments[1].unquoted());
            auto written = 0, kept = 0, merged = 0;
            juce::String error;
            const auto ok = SampleSettings::createMouOto(bank, written, kept,
                                                         merged, error);
            std::cout << "written=" << written << "|kept=" << kept
                      << "|merged=" << merged
                      << "|error=" << (error.isEmpty() ? juce::String("-") : error)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-mou-oto-file")
        {
            // 谋 keeps its own oto.  Seeding it copies what 界 already has and
            // marks every entry CVVV, which is what an oto4 row has always
            // meant, so a freshly seeded voicebank renders exactly as it did.
            // Saving an entry from the editor writes otomou.ini and never
            // oto4.ini -- annotating a bank cannot disturb what 界 renders --
            // and moving a boundary must not throw the annotation away.
            const juce::File bank(arguments[1].unquoted());
            juce::StringArray warnings;
            const auto jieFile = bank.getChildFile("oto4.ini");
            const auto mouFile = bank.getChildFile("otomou.ini");
            mouFile.deleteFile();
            const auto jieBefore = jieFile.loadFileAsString();
            const auto otoBefore = bank.getChildFile("oto.ini").loadFileAsString();

            auto written = 0, kept = 0, merged = 0;
            juce::String error;
            const auto seeded = SampleSettings::createMouOto(bank, written, kept,
                                                             merged, error);
            // Seeding again keeps what is there rather than doubling it.
            auto again = 0, keptAgain = 0, mergedAgain = 0;
            const auto seededTwice = SampleSettings::createMouOto(
                bank, again, keptAgain, mergedAgain, error);
            // Nothing new the second time, and every entry accounted for: the
            // rows written the first time plus the aliases that shared one.
            const auto seedingIsIdempotent = seeded && seededTwice
                && written > 0 && again == 0 && keptAgain == written + kept;

            // Every seeded row reads back as CVVV.
            auto entries = SampleSettings::loadVoicebankOto(bank, warnings, true, true);
            auto allDefault = !entries.empty();
            for (const auto& entry : entries)
                allDefault = allDefault && entry.mouClasses == "CVVV";

            // Mark one entry's coda a consonant and move a boundary at once.
            auto marked = false, keptClasses = false, jieUntouched = false;
            if (!entries.empty())
            {
                auto edited = entries.front();
                const auto original = edited;
                edited.mouClasses = "CVVC";
                edited.jieGlideMs = edited.jieGlideMs + 7.0;
                // The order the editor saves in: the independent classic
                // timing first, which is what makes oto.jie.ini exist, then
                // the region file.  oto.ini itself is never written.
                marked = SampleSettings::updateJieVoicebankOtoEntry(original, edited,
                                                                    error)
                    && SampleSettings::updateMouOtoEntry(original, edited, error);
                // Now move a boundary again without saying anything about the
                // classes: the annotation has to survive.
                auto reread = SampleSettings::loadVoicebankOto(bank, warnings, true, true);
                auto second = reread.front();
                const auto before = second;
                second.mouClasses = {};
                second.jieGlideMs = second.jieGlideMs + 3.0;
                marked = marked
                    && SampleSettings::updateJieVoicebankOtoEntry(before, second, error)
                    && SampleSettings::updateMouOtoEntry(before, second, error);
                const auto after = SampleSettings::loadVoicebankOto(bank, warnings,
                                                                   true, true);
                keptClasses = !after.empty() && after.front().mouClasses == "CVVC";
                jieUntouched = jieFile.loadFileAsString() == jieBefore
                    && bank.getChildFile("oto.ini").loadFileAsString() == otoBefore;
            }

            // Several aliases can point at one wav and offset, and a 谋 row
            // belongs to the wav and the offset -- so they share one row, and
            // seeding must not append one per alias.  Duplicates are what the
            // writer and the reader used to disagree about: the annotation
            // went into the first of them and was read back out of the last,
            // so saving a three-region entry looked like it did nothing.
            auto oneRowPerRegion = true, readsWhatItWrote = true;
            {
                const auto rows = juce::StringArray::fromLines(
                    mouFile.loadFileAsString());
                auto forSample = 0;
                for (const auto& line : rows)
                    if (line.startsWithIgnoreCase(entries.front().sourceName + "="))
                        ++forSample;
                oneRowPerRegion = forSample == 1;

                // And with a duplicate deliberately in the file, an annotation
                // still comes back.
                auto lines = juce::StringArray::fromLines(mouFile.loadFileAsString());
                for (int index = 0; index < lines.size(); ++index)
                    if (lines[index].startsWithIgnoreCase(
                            entries.front().sourceName + "="))
                    {
                        lines.insert(index + 1, lines[index]);
                        break;
                    }
                mouFile.replaceWithText(lines.joinIntoString("\n") + "\n",
                                        false, false, "\n");
                auto twice = SampleSettings::loadVoicebankOto(bank, warnings,
                                                              true, true);
                auto three = twice.front();
                const auto was = three;
                three.mouClasses = "CVV";
                readsWhatItWrote =
                    SampleSettings::updateMouOtoEntry(was, three, error);
                const auto back = SampleSettings::loadVoicebankOto(bank, warnings,
                                                                   true, true);
                readsWhatItWrote = readsWhatItWrote && !back.empty()
                    && back.front().mouClasses == "CVV";
            }

            // A file that already has duplicates is repaired by seeding, and
            // the row carrying the annotation is the one that survives -- the
            // others say nothing, so keeping one of those instead would throw
            // the author's work away.
            // A file that already has duplicates is repaired by seeding, and
            // the row carrying the annotation is the one that survives -- the
            // others say nothing, so keeping one of those instead would throw
            // the author's work away.  Set up deliberately rather than on top
            // of whatever the checks above left behind.
            // A file that already has duplicates is repaired by seeding, and
            // the row carrying the annotation is the one that survives -- the
            // others say nothing, so keeping one of those instead would throw
            // the author's work away.  From a fresh file, so this stands on
            // its own rather than on whatever the checks above left behind.
            auto repairs = false;
            juce::String report;
            {
                mouFile.deleteFile();
                auto more = 0, held = 0, joined = 0;
                repairs = SampleSettings::createMouOto(bank, more, held, joined, error);
                auto only = SampleSettings::loadVoicebankOto(bank, warnings,
                                                             true, true).front();
                const auto was = only;
                only.mouClasses = "CVC";
                repairs = repairs
                    && SampleSettings::updateMouOtoEntry(was, only, error);

                const auto lines = juce::StringArray::fromLines(
                    mouFile.loadFileAsString());
                juce::StringArray rebuilt;
                for (const auto& line : lines)
                {
                    if (!line.startsWithIgnoreCase(only.sourceName + "="))
                    {
                        rebuilt.add(line);
                        continue;
                    }
                    // The annotated row between two that say nothing.
                    const auto plain = line.replace("CVC,", "CVVV,");
                    rebuilt.add(plain);
                    rebuilt.add(line);
                    rebuilt.add(plain);
                }
                mouFile.replaceWithText(rebuilt.joinIntoString("\n") + "\n",
                                        false, false, "\n");

                repairs = repairs
                    && SampleSettings::createMouOto(bank, more, held, joined, error)
                    && joined == 2;
                const auto after = juce::StringArray::fromLines(
                    mouFile.loadFileAsString());
                auto forSample = 0;
                for (const auto& line : after)
                    if (line.startsWithIgnoreCase(only.sourceName + "="))
                        ++forSample;
                const auto healed = SampleSettings::loadVoicebankOto(bank, warnings,
                                                                     true, true);
                repairs = repairs && forSample == 1 && !healed.empty()
                    && healed.front().mouClasses == "CVC";
                report += " repair[joined=" + juce::String(joined)
                    + " rows=" + juce::String(forSample)
                    + " class=" + (healed.empty() ? juce::String("-")
                                                  : healed.front().mouClasses) + "]";
            }

            // And 界 still reads what it always did: no classes, its own file.
            const auto jieView = SampleSettings::loadVoicebankOto(bank, warnings, true);
            auto jieSeesNothing = !jieView.empty();
            for (const auto& entry : jieView)
                jieSeesNothing = jieSeesNothing && entry.mouClasses.isEmpty();

            mouFile.deleteFile();
            std::cout << "seeded=" << written << "+" << kept
                      << "|seeding_is_idempotent=" << (seedingIsIdempotent ? 1 : 0)
                      << "|seeds_as_cvvv=" << (allDefault ? 1 : 0)
                      << "|entry_saved=" << (marked ? 1 : 0)
                      << "|classes_survive_a_boundary_edit=" << (keptClasses ? 1 : 0)
                      << "|jie_file_untouched=" << (jieUntouched ? 1 : 0)
                      << "|one_row_per_sample_region=" << (oneRowPerRegion ? 1 : 0)
                      << "|reads_the_row_it_wrote=" << (readsWhatItWrote ? 1 : 0)
                      << "|seeding_repairs_duplicates=" << (repairs ? 1 : 0)
                      << report
                      << "|jie_never_sees_classes=" << (jieSeesNothing ? 1 : 0)
                      << "|error=" << (error.isEmpty() ? juce::String("-") : error)
                      << std::endl;
            setApplicationReturnValue(seedingIsIdempotent && allDefault && marked
                                      && keptClasses && jieUntouched
                                      && oneRowPerRegion && readsWhatItWrote && repairs
                                      && jieSeesNothing ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-utau-mode")
        {
            // The UTAU mode used to be a bool, which cannot hold three states.
            // It is a word now, and it has to survive a save, come back right
            // from a project written before the third mode existed, and still
            // leave that older build something it understands.
            ProjectModel modeProject;
            juce::String modeError;
            if (!modeProject.load(juce::File(arguments[1].unquoted()), modeError))
            {
                std::cout << "loaded=0|error=" << modeError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = modeProject.snapshot().tracks.front().id;
            juce::TemporaryFile scratch(".hjpx");

            // The word each mode is written as, and back again.
            auto wordsRoundTrip = true;
            for (const auto mode : { UtauMode::classic, UtauMode::jie, UtauMode::mou })
                wordsRoundTrip = wordsRoundTrip
                    && parseUtauMode(utauModeKey(mode)) == mode
                    && utauModeLabel(mode).isNotEmpty();
            // Anything unknown is plain UTAU, never a guess at the new mode.
            wordsRoundTrip = wordsRoundTrip
                && parseUtauMode("") == UtauMode::classic
                && parseUtauMode("something else") == UtauMode::classic
                // and the word 界 has always been written as still reads
                && parseUtauMode("utau4") == UtauMode::jie;

            // Every mode is an item of the pitch-algorithm picker, and every
            // one of those items is a mode.  The whole UTAU toolbar hangs off
            // that answer, so a mode the picker does not own comes up wearing
            // the generic parameter set -- which is what 谋 did at first,
            // because the id was spelled out by hand in four places and one
            // of them was left behind.
            auto pickerRoundTrips = true;
            for (const auto mode : { UtauMode::classic, UtauMode::jie, UtauMode::mou })
            {
                const auto item = utauModePickerItem(mode);
                const auto back = utauModeForPickerItem(item);
                pickerRoundTrips = pickerRoundTrips && back.has_value()
                    && *back == mode;
            }
            // The three ids are distinct, and nothing else is a UTAU item.
            {
                std::set<int> items;
                for (const auto mode : { UtauMode::classic, UtauMode::jie, UtauMode::mou })
                    items.insert(utauModePickerItem(mode));
                pickerRoundTrips = pickerRoundTrips && items.size() == 3;
                for (int item = -2; item <= 20; ++item)
                    if (items.find(item) == items.end())
                        pickerRoundTrips = pickerRoundTrips
                            && !utauModeForPickerItem(item).has_value();
            }

            // Regions are in play for 界 and 谋 alike -- 谋 keeps the
            // four-region CV -- and for neither in plain UTAU.
            const auto regionsRule = !utauModeUsesRegions(UtauMode::classic)
                && utauModeUsesRegions(UtauMode::jie)
                && utauModeUsesRegions(UtauMode::mou);

            const auto modeOf = [&](ProjectModel& from)
            {
                const auto snapshot = from.snapshot();
                for (const auto& track : snapshot.tracks)
                    if (track.id == trackId) return track.utauMode;
                return UtauMode::classic;
            };

            // Each mode survives a save and a load.
            auto survivesSave = true;
            juce::String saved;
            for (const auto mode : { UtauMode::mou, UtauMode::jie, UtauMode::classic })
            {
                modeProject.setTrackUtauMode(trackId, mode);
                juce::String writeError;
                survivesSave = survivesSave
                    && modeProject.save(scratch.getFile(), writeError);
                ProjectModel reopened;
                juce::String readError;
                survivesSave = survivesSave
                    && reopened.load(scratch.getFile(), readError)
                    && modeOf(reopened) == mode;
                saved += " " + utauModeKey(mode);
            }

            // A project written before 谋 carries only the old flag.  Saved
            // in 界, then the new word stripped out of the file: it has to
            // come back as 界 rather than falling to plain UTAU.
            auto legacyReads = false;
            auto oldBuildCanRead = false;
            {
                modeProject.setTrackUtauMode(trackId, UtauMode::jie);
                juce::String writeError;
                modeProject.save(scratch.getFile(), writeError);
                juce::ValueTree tree;
                if (auto in = scratch.getFile().createInputStream())
                    tree = juce::ValueTree::readFromStream(*in);
                std::function<void(juce::ValueTree&)> strip =
                    [&](juce::ValueTree& node)
                    {
                        if (node.hasType("Track"))
                        {
                            // What an older build would look for.
                            oldBuildCanRead = oldBuildCanRead
                                || static_cast<bool>(
                                       node.getProperty("utauFourRegion", false));
                            node.removeProperty("utauMode", nullptr);
                        }
                        for (auto child : node) strip(child);
                    };
                strip(tree);
                if (auto out = scratch.getFile().createOutputStream())
                {
                    out->setPosition(0);
                    out->truncate();
                    tree.writeToStream(*out);
                }
                ProjectModel legacy;
                juce::String readError;
                legacyReads = legacy.load(scratch.getFile(), readError)
                    && modeOf(legacy) == UtauMode::jie;
            }

            std::cout << "words_round_trip=" << (wordsRoundTrip ? 1 : 0)
                      << "|picker_owns_every_mode=" << (pickerRoundTrips ? 1 : 0)
                      << "|regions_for_jie_and_mou=" << (regionsRule ? 1 : 0)
                      << "|survives_save=" << (survivesSave ? 1 : 0)
                      << "|saved" << saved
                      << "|old_flag_reads_as_jie=" << (legacyReads ? 1 : 0)
                      << "|older_build_still_told=" << (oldBuildCanRead ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(wordsRoundTrip && pickerRoundTrips
                                      && regionsRule && survivesSave
                                      && legacyReads && oldBuildCanRead ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-lane-marquee")
        {
            // A lane along the bottom still leaves the roll above it, and
            // notes are selected there the ordinary way.  The flag lane did
            // not start a marquee at all, so notes could not be picked while
            // it was open -- which is how a curve gets put on one.
            I18n marqueeStrings;
            ProjectModel marqueeProject;
            juce::String marqueeError;
            if (!marqueeProject.load(juce::File(arguments[1].unquoted()), marqueeError))
            {
                std::cout << "loaded=0|error=" << marqueeError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent marqueeRoll(marqueeProject, marqueeStrings);
            marqueeRoll.setBounds(0, 0, 1600, 900);
            marqueeRoll.resized();
            marqueeRoll.diagnosticRefresh();

            const auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto press = [&](juce::Point<float> at, juce::Point<float> from,
                                   bool dragged)
            {
                return juce::MouseEvent(source, at, juce::ModifierKeys::leftButtonModifier,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &marqueeRoll, &marqueeRoll, juce::Time::getCurrentTime(), from,
                    juce::Time::getCurrentTime(), 1, dragged);
            };
            // A box over the first few notes, drawn well above the lane so it
            // is the roll being swept and not the lane being edited.
            const auto drawn = marqueeRoll.diagnosticNotes();
            const auto lane = marqueeRoll.flagLaneBounds();
            const auto sweep = [&](PianoRollComponent::Tool tool)
            {
                marqueeRoll.setTool(tool);
                marqueeRoll.clearNoteSelection();
                const juce::Point<float> from(60.0f, 8.0f);
                const juce::Point<float> to(
                    marqueeRoll.diagnosticEdgeX(drawn[2].end) + 12.0f,
                    std::max(20.0f, lane.getY() - 20.0f));
                marqueeRoll.mouseDown(press(from, from, false));
                marqueeRoll.mouseDrag(press(to, from, true));
                marqueeRoll.mouseUp(press(to, from, true));
                return static_cast<int>(marqueeRoll.selectedNoteIds().size());
            };

            // A band drawn wholly inside one note's own block picks that note
            // and nothing else.  noteHits carry the sounding stretch, which
            // reaches back over the note before, so going by that caught the
            // next note too -- every single-note band grabbed two.
            auto bandTakesOne = true;
            auto worstCaught = 0;
            auto worstAt = -1;
            {
                marqueeRoll.setTool(PianoRollComponent::Tool::note);
                for (int k = 0; k + 1 < static_cast<int>(drawn.size()) && k < 40; ++k)
                {
                    marqueeRoll.clearNoteSelection();
                    const auto left = marqueeRoll.diagnosticEdgeX(drawn[k].start) + 1.0f;
                    // A whisker inside k, nowhere near k+1's own block.
                    const auto right = std::max(left + 2.0f,
                        marqueeRoll.diagnosticEdgeX(drawn[k].end) - 1.0f);
                    const juce::Point<float> from(left, 8.0f);
                    const juce::Point<float> to(right,
                        std::max(20.0f, lane.getY() - 20.0f));
                    marqueeRoll.mouseDown(press(from, from, false));
                    marqueeRoll.mouseDrag(press(to, from, true));
                    marqueeRoll.mouseUp(press(to, from, true));
                    const auto here =
                        static_cast<int>(marqueeRoll.selectedNoteIds().size());
                    if (here > worstCaught) { worstCaught = here; worstAt = k; }
                    bandTakesOne = bandTakesOne && here == 1;
                }
            }
            const auto withNote = sweep(PianoRollComponent::Tool::note);
            const auto withPoints = sweep(PianoRollComponent::Tool::points);
            const auto withAmplitude = sweep(PianoRollComponent::Tool::amplitude);
            const auto withFlag = sweep(PianoRollComponent::Tool::flagCurve);
            // The note tool is the reference: whatever it catches, the lane
            // tools have to catch as well.
            const auto caught = withNote > 0;
            const auto lanesAgree = withAmplitude == withNote && withFlag == withNote
                && withPoints == withNote;
            std::cout << "one_note_band_catches=" << worstCaught
                      << "@note" << worstAt
                      << "|a_band_inside_one_note_takes_one=" << (bandTakesOne ? 1 : 0)
                      << "|notes=" << drawn.size()
                      << "|note_tool=" << withNote
                      << "|points=" << withPoints
                      << "|amplitude=" << withAmplitude
                      << "|flag_curve=" << withFlag
                      << "|marquee_catches_notes=" << (caught ? 1 : 0)
                      << "|every_lane_tool_agrees=" << (lanesAgree ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(caught && lanesAgree && bandTakesOne ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (arguments.size() >= 3 && arguments[0] == "--smoke-flag-lane")
        {
            // The lane maps g to a fixed -50..+50 scale, and a handle has to
            // land back on the value it was dropped at.  Then a picture, since
            // a round-trip says nothing about whether anything was drawn.
            I18n flagStrings;
            ProjectModel flagProject;
            juce::String flagError;
            if (!flagProject.load(juce::File(arguments[1].unquoted()), flagError))
            {
                std::cout << "loaded=0|error=" << flagError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            HachiLookAndFeel flagLookAndFeel;
            juce::LookAndFeel::setDefaultLookAndFeel(&flagLookAndFeel);
            PianoRollComponent flagRoll(flagProject, flagStrings);
            flagRoll.setBounds(0, 0, 1100, 620);
            flagRoll.resized();
            flagRoll.setTool(PianoRollComponent::Tool::flagCurve);

            // Round trip through the lane's own mapping.
            auto worstRoundTrip = 0.0f;
            for (const auto value : { -50.0f, -37.5f, -12.0f, 0.0f, 8.0f, 25.0f, 50.0f })
                worstRoundTrip = std::max(worstRoundTrip, std::abs(
                    flagRoll.flagValueFromLaneY(flagRoll.flagLaneY(value)) - value));
            // Out of range is pinned, not wrapped.
            const auto pinnedHigh = flagRoll.flagValueFromLaneY(
                flagRoll.flagLaneY(400.0f));
            const auto pinnedLow = flagRoll.flagValueFromLaneY(
                flagRoll.flagLaneY(-400.0f));
            // Higher g sits higher in the lane.
            const auto risesUpward = flagRoll.flagLaneY(40.0f) < flagRoll.flagLaneY(-40.0f);

            // Give a note a curve and photograph the strip.
            const auto trackId = flagProject.snapshot().tracks.front().id;
            flagProject.setTrackUtauMode(trackId, UtauMode::jie);
            const auto notes = flagProject.snapshot().tracks.front().clips.front().notes;
            std::vector<juce::String> ids;
            for (std::size_t index = 0; index < std::min<std::size_t>(3, notes.size()); ++index)
                ids.push_back(notes[index].id);
            flagProject.setNotesUtauFlagCurveEnabled(ids, true);
            // The stretch-algorithm picker belongs to the non-UTAU algorithms:
            // in any UTAU mode the stretching is the resampler's own, and the
            // control has nothing to say.  Every UTAU picker item hides it,
            // every other one shows it -- checked over the whole list, so a
            // mode added later cannot quietly leave it on screen.
            auto stretchHiddenInUtau = true;
            for (int item = 1; item <= 9; ++item)
            {
                const auto isUtau = utauModeForPickerItem(item).has_value();
                stretchHiddenInUtau = stretchHiddenInUtau
                    && (isUtau == (item >= 7 && item <= 9));
            }
            // Turning the switch on writes nothing: the lane draws the
            // starting line itself, for whichever flag is on show.  Seeding a
            // curve here as well gave g -- alone among the seventeen -- two
            // handles at the note's own start and end, and made a note count
            // as carrying a curve before anything had been drawn.
            auto switchWritesNothing = !ids.empty();
            // Held in a local: snapshot() hands back a value, and .front() is a
            // call, so a range-for over the chain reads a temporary that has
            // already gone.
            const auto afterSwitch = flagProject.snapshot();
            for (const auto& note : afterSwitch.tracks.front().clips.front().notes)
                if (std::find(ids.begin(), ids.end(), note.id) != ids.end())
                    switchWritesNothing = switchWritesNothing
                        && note.utauFlagCurveEnabled
                        && note.utauFlagCurves.empty();
            {
                // And every flag, g included, opens with the one handle.
                flagRoll.diagnosticRefresh();
                const auto note = flagProject.snapshot().tracks.front()
                                      .clips.front().notes.front();
                for (const auto& kind : flagCurveKinds())
                {
                    flagRoll.setFlagLaneFlag(kind.flag);
                    switchWritesNothing = switchWritesNothing
                        && flagRoll.flagLaneCurveFor(note).size() == 1;
                }
                flagRoll.setFlagLaneFlag("g");
                // What a note looks like the moment the switch goes on.
                const auto freshShot = flagRoll.createComponentSnapshot(
                    flagRoll.flagLaneBounds().toNearestInt().withWidth(760),
                    true, 2.0f);
                juce::File freshFile(juce::File(arguments[2].unquoted())
                    .getSiblingFile(juce::File(arguments[2].unquoted())
                        .getFileNameWithoutExtension() + "-fresh.png"));
                freshFile.deleteFile();
                juce::PNGImageFormat freshPng;
                std::unique_ptr<juce::FileOutputStream> freshStream(
                    freshFile.createOutputStream());
                if (freshStream != nullptr)
                    freshPng.writeImageToStream(freshShot, *freshStream);
            }
            if (!ids.empty())
                flagProject.setNoteUtauFlagCurve(ids.front(), "g",
                    { { 0.0, -30.0f, PitchCurveShape::linear },
                      { 0.12, -30.0f, PitchCurveShape::linear },
                      { 0.30, 42.0f, PitchCurveShape::easeIn },
                      { 0.45, 10.0f, PitchCurveShape::smooth } });
            flagRoll.diagnosticRefresh();
            // The switch sits at the top right of the lane, inside it, clear of
            // the heading that names the flag.  In the window the lane is the
            // visible area, so that is the right of the screen; here, with no
            // viewport, it is the right of the whole canvas.
            const auto laneBox = flagRoll.flagLaneBounds();
            const auto switchBox = flagRoll.flagLaneSwitchBounds();
            const auto switchPlaced = laneBox.contains(switchBox)
                && switchBox.getRight() > laneBox.getRight() - 12.0f
                && switchBox.getY() < laneBox.getY() + 28.0f
                && switchBox.getWidth() > 40.0f;
            // Two pictures: the curve at the left end, the switch at the right.
            const auto lane = laneBox.toNearestInt().withWidth(760);
            const auto shot = flagRoll.createComponentSnapshot(lane, true, 2.0f);
            {
                auto corner = laneBox.toNearestInt();
                corner = corner.withTrimmedLeft(corner.getWidth() - 420)
                               .withHeight(60);
                const auto cornerShot = flagRoll.createComponentSnapshot(
                    corner, true, 2.0f);
                juce::File cornerFile(juce::File(arguments[2].unquoted())
                    .getSiblingFile(juce::File(arguments[2].unquoted())
                        .getFileNameWithoutExtension() + "-switch.png"));
                cornerFile.deleteFile();
                juce::PNGImageFormat cornerPng;
                std::unique_ptr<juce::FileOutputStream> cornerStream(
                    cornerFile.createOutputStream());
                if (cornerStream != nullptr)
                    cornerPng.writeImageToStream(cornerShot, *cornerStream);
            }
            juce::File out(arguments[2].unquoted());
            out.deleteFile();
            juce::PNGImageFormat png;
            std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
            const auto written = stream != nullptr && png.writeImageToStream(shot, *stream);
            stream.reset();
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);

            // What the handle's right-click menu offers, and what each item
            // does to the curve.  The menu itself needs a window; these are
            // the decisions and the results, which do not.
            const auto onlyOne = PianoRollComponent::flagPointMenuState(1, 12.0f);
            const auto several = PianoRollComponent::flagPointMenuState(4, 12.0f);
            const auto atDefault = PianoRollComponent::flagPointMenuState(4, 0.0f);
            const auto menuRule = !onlyOne.canDelete && several.canDelete
                && several.canReset && !atDefault.canReset && onlyOne.canReset;

            const auto curveOf = [&]
            {
                return flagCurvePointsFor(flagProject.snapshot().tracks.front()
                                              .clips.front().notes.front(), "g");
            };
            const auto noteId = flagProject.snapshot().tracks.front()
                .clips.front().notes.front().id;
            const auto edit = [&](std::function<void(std::vector<FlagCurvePoint>&)> change)
            {
                auto points = curveOf();
                change(points);
                flagProject.setNoteUtauFlagCurve(noteId, "g", std::move(points));
                return curveOf();
            };
            // Typed straight in, including a value past what the engine takes.
            const auto typed = edit([](auto& points) { points[1].value = 900.0f; });
            const auto clamped = typed.size() == 4
                && std::abs(typed[1].value - 50.0f) < 1.0e-3f;
            const auto reset = edit([](auto& points)
                { points[1].value = PianoRollComponent::flagCurveDefault; });
            const auto wentBack = reset.size() == 4 && std::abs(reset[1].value) < 1.0e-4f;
            const auto removed = edit([](auto& points)
                { points.erase(points.begin() + 1); });
            const auto deleted = removed.size() == 3
                && std::abs(removed[1].timeSeconds - 0.30) < 1.0e-6;


            const auto stored = flagCurvePointsFor(
                flagProject.snapshot().tracks.front().clips.front().notes.front(), "g");
            // stored is what the edits above left behind, not the drawn curve.
            const auto kept = stored.size() == 3;
            // Every flag the lane offers, its range, and the scale it draws
            // on.  The ranges are the engine's own clamps: if one drifts, a
            // handle can be dropped somewhere that will not be rendered.
            auto rangesHold = true;
            juce::StringArray offered;
            for (const auto& kind : flagCurveKinds())
            {
                offered.add(kind.flag);
                flagRoll.setFlagLaneFlag(kind.flag);
                if (flagRoll.flagLaneFlag() != juce::String(kind.flag))
                { rangesHold = false; continue; }
                // The scale has to map that flag's own ends, and pin beyond.
                const auto low = flagRoll.flagValueFromLaneY(
                    flagRoll.flagLaneY(kind.minimum));
                const auto high = flagRoll.flagValueFromLaneY(
                    flagRoll.flagLaneY(kind.maximum));
                const auto past = flagRoll.flagValueFromLaneY(
                    flagRoll.flagLaneY(kind.maximum + 500.0f));
                rangesHold = rangesHold && std::abs(low - kind.minimum) < 0.05f
                    && std::abs(high - kind.maximum) < 0.05f
                    && std::abs(past - kind.maximum) < 0.05f
                    && flagRoll.flagLaneY(kind.maximum) < flagRoll.flagLaneY(kind.minimum);
                // And the model must refuse to store a value outside it.
                if (!ids.empty())
                {
                    flagProject.setNoteUtauFlagCurve(ids.front(), kind.flag,
                        { { 0.0, kind.maximum + 900.0f }, { 0.2, kind.minimum - 900.0f } });
                    const auto back = flagCurvePointsFor(
                        flagProject.snapshot().tracks.front().clips.front().notes.front(),
                        kind.flag);
                    rangesHold = rangesHold && back.size() == 2
                        && std::abs(back[0].value - kind.maximum) < 1.0e-3f
                        && std::abs(back[1].value - kind.minimum) < 1.0e-3f;
                    flagProject.setNoteUtauFlagCurve(ids.front(), kind.flag, {});
                }
            }
            // One flag's curve does not disturb another's.
            auto independent = false;
            if (!ids.empty())
            {
                flagProject.setNoteUtauFlagCurve(ids.front(), "g",
                    { { 0.0, 10.0f }, { 0.3, -10.0f } });
                flagProject.setNoteUtauFlagCurve(ids.front(), "Mt",
                    { { 0.0, 60.0f }, { 0.4, -60.0f }, { 0.5, 0.0f } });
                const auto note = flagProject.snapshot().tracks.front()
                                      .clips.front().notes.front();
                const auto gPoints = flagCurvePointsFor(note, "g");
                const auto tension = flagCurvePointsFor(note, "Mt");
                flagProject.setNoteUtauFlagCurve(ids.front(), "Mt", {});
                const auto after = flagProject.snapshot().tracks.front()
                                       .clips.front().notes.front();
                independent = gPoints.size() == 2 && tension.size() == 3
                    && flagCurvePointsFor(after, "Mt").empty()
                    && flagCurvePointsFor(after, "g").size() == 2;
            }
            // Switching to a flag that has no curve stored still gives every
            // enabled note a flat line to take hold of -- with only the seeded
            // one drawable, the lane looked empty for sixteen of seventeen
            // flags and said the switch was off when it was on.
            auto everyFlagDrawable = true;
            if (!ids.empty())
            {
                // A note with the switch on and nothing drawn yet: this is
                // what every flag but the seeded one looked like.
                auto note = flagProject.snapshot().tracks.front()
                                .clips.front().notes.front();
                note.utauFlagCurves.clear();
                for (const auto& kind : flagCurveKinds())
                {
                    flagRoll.setFlagLaneFlag(kind.flag);
                    const auto shown = flagRoll.flagLaneCurveFor(note);
                    const auto resting = kind.minimum > 0.0f ? kind.minimum
                                       : kind.maximum < 0.0f ? kind.maximum : 0.0f;
                    // One handle, at the end of what the flag reaches, resting
                    // where that flag does nothing.
                    const auto [spanFrom, spanTo] = flagRoll.flagLaneSpanFor(note);
                    juce::ignoreUnused(spanFrom);
                    everyFlagDrawable = everyFlagDrawable && shown.size() == 1
                        && std::abs(shown.front().value - resting) < 1.0e-3f
                        && std::abs(shown.front().timeSeconds - spanTo) < 1.0e-9;
                }
                // And a note without the switch has nothing, whatever the flag.
                auto bare = note;
                bare.utauFlagCurveEnabled = false;
                everyFlagDrawable = everyFlagDrawable
                    && flagRoll.flagLaneCurveFor(bare).empty();
            }
            // Every handle answers a click where it is drawn -- the last one
            // included.  A note in a phrase stops sounding a preutterance
            // minus an overlap before its written end, so its last handle is
            // drawn at that edge; the hit test used to look for it at the
            // written end instead, tens of pixels to the right, and it could
            // not be picked up or moved at all.
            auto lastHandleMoves = true;
            juce::String handleReport;
            if (ids.size() >= 2 && arguments.size() >= 5)
            {
                flagProject.setTrackVoicebankDirectory(trackId,
                    juce::File(arguments[3].unquoted()));
                for (int index = 0; index < 2; ++index)
                    flagProject.setNoteLabel(ids[static_cast<std::size_t>(index)],
                                             arguments[4].unquoted());
                // The gap is a span of time; it takes a working zoom to be a
                // number of pixels worth missing by.
                flagRoll.setPixelsPerSecond(600.0f);
                flagRoll.resized();
                flagRoll.setFlagLaneFlag("g");
                flagProject.setNoteUtauFlagCurve(ids.front(), "g", {});
                flagRoll.diagnosticRefresh();
                const auto source = juce::Desktop::getInstance().getMainMouseSource();
                const auto press = [&](juce::Point<float> at, juce::Point<float> from,
                                       bool dragged)
                {
                    return juce::MouseEvent(source, at,
                        juce::ModifierKeys::leftButtonModifier,
                        juce::MouseInputSource::defaultPressure,
                        0.0f, 0.0f, 0.0f, 0.0f, &flagRoll, &flagRoll,
                        juce::Time::getCurrentTime(), from,
                        juce::Time::getCurrentTime(), 1, dragged);
                };
                const auto drawnAtSeconds = flagRoll.flagLaneCurveFor(
                    flagProject.snapshot().tracks.front().clips.front()
                        .notes.front()).front().timeSeconds;
                const auto stored = [&](int index)
                {
                    const auto points = flagCurvePointsFor(
                        flagProject.snapshot().tracks.front().clips.front()
                            .notes.front(), "g");
                    return index < static_cast<int>(points.size())
                        ? points[static_cast<std::size_t>(index)].value : 0.0f;
                };
                // How far the drawn place is from where the hit test used to
                // look: the gap that made this unreachable.
                const auto drawnAt = flagRoll.diagnosticFlagHandleCentre(ids.front(), 0);
                auto writtenEndX = drawnAt.x;
                for (const auto& entry : flagRoll.diagnosticNotes())
                    if (entry.id == ids.front())
                        writtenEndX = flagRoll.diagnosticEdgeX(entry.end);
                // Where the old hit test looked, against where it is drawn.
                const auto handleGap = writtenEndX - drawnAt.x;
                // A note starts with the one handle, at its end.
                lastHandleMoves = lastHandleMoves
                    && flagRoll.flagLaneCurveFor(
                           flagProject.snapshot().tracks.front().clips.front()
                               .notes.front()).size() == 1;
                for (const auto wanted : { 30.0f, -20.0f })
                {
                    const auto from = flagRoll.diagnosticFlagHandleCentre(
                        ids.front(), 0);
                    // Sideways as well as up, to show the lone handle keeps its
                    // place: on its own it is the note's level, and sliding it
                    // along would say nothing while taking it off the end.
                    const juce::Point<float> to(from.x - 60.0f,
                                                flagRoll.flagLaneY(wanted));
                    flagRoll.mouseDown(press(from, from, false));
                    flagRoll.mouseDrag(press(to, from, true));
                    flagRoll.mouseUp(press(to, from, true));
                    const auto after = flagCurvePointsFor(
                        flagProject.snapshot().tracks.front().clips.front()
                            .notes.front(), "g");
                    lastHandleMoves = lastHandleMoves && after.size() == 1
                        && std::abs(after.front().value - wanted) < 1.5f
                        && std::abs(after.front().timeSeconds
                                    - drawnAtSeconds) < 1.0e-9
                        // One point reads as that value the whole way along.
                        && std::abs(flagCurveValueAt(after, 0.0) - wanted) < 1.5f
                        && std::abs(flagCurveValueAt(after, 900.0) - wanted) < 1.5f;
                    handleReport += " level=" + juce::String(stored(0), 1);
                }
                handleReport += " drawn_x=" + juce::String(drawnAt.x, 1)
                    + " written_end_x=" + juce::String(writtenEndX, 1)
                    + " gap_px=" + juce::String(handleGap, 1);

                // A handle beyond the sounding end -- a curve drawn before the
                // note's neighbour moved, say -- is drawn at the edge, and has
                // to answer a click there rather than at its own time.
                {
                    const auto note = flagProject.snapshot().tracks.front()
                                          .clips.front().notes.front();
                    flagProject.setNoteUtauFlagCurve(ids.front(), "g",
                        { { note.durationSeconds + 0.20, 12.0f } });
                    flagRoll.diagnosticRefresh();
                    const auto strandedAt =
                        flagRoll.diagnosticFlagHandleCentre(ids.front(), 0);
                    const auto ownTimeX = flagRoll.diagnosticEdgeX(
                        note.startSeconds + note.durationSeconds + 0.20);
                    const juce::Point<float> to(strandedAt.x,
                                                flagRoll.flagLaneY(-40.0f));
                    flagRoll.mouseDown(press(strandedAt, strandedAt, false));
                    flagRoll.mouseDrag(press(to, strandedAt, true));
                    flagRoll.mouseUp(press(to, strandedAt, true));
                    const auto after = flagCurvePointsFor(
                        flagProject.snapshot().tracks.front().clips.front()
                            .notes.front(), "g");
                    lastHandleMoves = lastHandleMoves
                        // and it really was drawn somewhere else, or this
                        // would pass without the clamp
                        && ownTimeX - strandedAt.x > 12.0f
                        && after.size() == 1
                        && std::abs(after.front().value + 40.0f) < 1.5f;
                    handleReport += " stranded_gap_px="
                        + juce::String(ownTimeX - strandedAt.x, 1)
                        + " stranded=" + juce::String(
                            after.empty() ? 0.0f : after.front().value, 1);
                }

                // The consonant is drawn ahead of the note, at negative times.
                // A normal flag reaches it, so a click there has to land on it
                // -- measuring the editable stretch from the note's own start
                // left the whole consonant untouchable.
                {
                    // The first note starts at zero, so nothing can sound
                    // ahead of it and it has no consonant of its own.
                    flagProject.setNoteUtauFlagCurve(ids.front(), "g", {});
                    flagRoll.diagnosticRefresh();
                    const auto second = flagProject.snapshot().tracks.front()
                                            .clips.front().notes[1];
                    const auto [from, to] = flagRoll.flagLaneSpanFor(second);
                    juce::ignoreUnused(to);
                    // A quarter of the way in, well clear of the stretch the
                    // note before still has a claim on.
                    const auto at = from * 0.25;
                    const auto plot = flagRoll.flagLanePlotBounds();
                    const juce::Point<float> click(
                        flagRoll.diagnosticEdgeX(second.startSeconds + at),
                        plot.getCentreY());
                    flagRoll.mouseDown(press(click, click, false));
                    flagRoll.mouseUp(press(click, click, false));
                    auto landed = false;
                    for (const auto& point : flagCurvePointsFor(
                             flagProject.snapshot().tracks.front().clips.front()
                                 .notes[1], "g"))
                        if (std::abs(point.timeSeconds - at) < 0.005) landed = true;
                    lastHandleMoves = lastHandleMoves && from < -0.05 && landed;
                    handleReport += " consonant_from=" + juce::String(from, 4)
                        + " added_at=" + juce::String(at, 4)
                        + " landed=" + juce::String(landed ? 1 : 0);
                    flagProject.setNoteUtauFlagCurve(
                        flagProject.snapshot().tracks.front().clips.front()
                            .notes[1].id, "g", {});
                }
                flagProject.setNoteUtauFlagCurve(ids.front(), "g", {});
                flagRoll.setPixelsPerSecond(140.0f);
                flagRoll.resized();
                flagRoll.diagnosticRefresh();
            }
            // "Reset linear flags": offered when any chosen note has the
            // switch on and something drawn, and it drops the curves on those
            // notes only.  A note with the switch off keeps what it is holding
            // -- it is not showing a curve to reset, and turning the switch
            // back on has to bring it back.
            auto resetRule = true;
            juce::String resetReport;
            juce::String pointResetReport;
            auto laneResetRule = true;
            juce::String laneResetReport;
            if (ids.size() >= 3)
            {
                const auto curvesOn = [&](const juce::String& id)
                {
                    // Same reason: keep the snapshot alive while it is read.
                    const auto snap = flagProject.snapshot();
                    for (const auto& note : snap.tracks.front().clips.front().notes)
                        if (note.id == id) return note.utauFlagCurves.size();
                    return std::size_t{};
                };
                const auto lay = [&](const juce::String& id)
                {
                    flagProject.setNoteUtauFlagCurve(id, "g",
                        { { 0.0, 12.0f }, { 0.2, -8.0f } });
                    flagProject.setNoteUtauFlagCurve(id, "Mt",
                        { { 0.0, 40.0f } });
                };
                for (int index = 0; index < 3; ++index)
                    flagProject.setNotesUtauFlagCurveEnabled(
                        { ids[static_cast<std::size_t>(index)] }, true);
                lay(ids[0]);
                lay(ids[1]);
                // ids[2] has the switch on and nothing drawn.
                flagProject.setNoteUtauFlagCurve(ids[2], "g", {});
                // The roll answers from its own copy of the project, which the
                // app refreshes on every change and a test has to ask for.
                flagRoll.diagnosticRefresh();

                // Nothing drawn anywhere -> nothing to reset.
                resetRule = resetRule && !flagRoll.flagResetAvailable({ ids[2] });
                // Something drawn -> offered.
                resetRule = resetRule && flagRoll.flagResetAvailable({ ids[0] });

                // Switch off on ids[1], curves kept: it is no longer showing
                // one, so on its own there is nothing to reset...
                flagProject.setNotesUtauFlagCurveEnabled({ ids[1] }, false);
                flagRoll.diagnosticRefresh();
                resetRule = resetRule && curvesOn(ids[1]) == 2
                    && !flagRoll.flagResetAvailable({ ids[1] })
                    // ...but mixed with a switched-on note it is still offered.
                    && flagRoll.flagResetAvailable({ ids[0], ids[1], ids[2] });

                resetReport = " pre0=" + juce::String((int)curvesOn(ids[0]))
                    + " pre1=" + juce::String((int)curvesOn(ids[1]))
                    + " avail0=" + juce::String(
                        flagRoll.flagResetAvailable({ ids[0] }) ? 1 : 0)
                    + " avail1=" + juce::String(
                        flagRoll.flagResetAvailable({ ids[1] }) ? 1 : 0)
                    + " avail2=" + juce::String(
                        flagRoll.flagResetAvailable({ ids[2] }) ? 1 : 0);
                flagProject.resetNotesUtauFlagCurves({ ids[0], ids[1], ids[2] });
                flagRoll.diagnosticRefresh();
                resetReport += " on=" + juce::String((int)curvesOn(ids[0]))
                    + " off_kept=" + juce::String((int)curvesOn(ids[1]))
                    + " bare=" + juce::String((int)curvesOn(ids[2]));
                resetRule = resetRule
                    // the switched-on note is back to nothing drawn
                    && curvesOn(ids[0]) == 0
                    // the switched-off note kept every curve it held
                    && curvesOn(ids[1]) == 2
                    && curvesOn(ids[2]) == 0
                    // and with nothing left drawn it stops being offered
                    && !flagRoll.flagResetAvailable({ ids[0], ids[2] });
                // One undoable step: the curves come back whole.
                flagProject.undo();
                resetRule = resetRule && curvesOn(ids[0]) == 2;
                flagProject.redo();
                resetRule = resetRule && curvesOn(ids[0]) == 0;

                flagProject.setNotesUtauFlagCurveEnabled({ ids[1] }, true);
                flagProject.resetNotesUtauFlagCurves({ ids[1] });
                flagRoll.diagnosticRefresh();
                // "Reset this note's <flag>" from the handle menu: only the
                // flag on show, only this note.  The lane's other curves and
                // the other notes keep what they have.
                {
                    lay(ids[0]);
                    lay(ids[1]);
                    flagRoll.setFlagLaneFlag("g");
                    flagRoll.diagnosticRefresh();
                    const auto countFor = [&](const juce::String& id,
                                              const juce::String& flag)
                    {
                        const auto snap = flagProject.snapshot();
                        for (const auto& note : snap.tracks.front().clips.front().notes)
                            if (note.id == id)
                                return flagCurvePointsFor(note, flag).size();
                        return std::size_t{};
                    };
                    resetRule = resetRule
                        && flagRoll.flagResetAvailableForNote(ids[0])
                        // nothing drawn on this flag -> nothing to reset
                        && !flagRoll.flagResetAvailableForNote(ids[2]);
                    // What the menu item does.
                    flagProject.setNoteUtauFlagCurve(ids[0], "g", {});
                    flagRoll.diagnosticRefresh();
                    resetRule = resetRule
                        // this note's g is back to the starting handle
                        && countFor(ids[0], "g") == 0
                        && flagRoll.flagLaneCurveFor(
                               flagProject.snapshot().tracks.front().clips.front()
                                   .notes.front()).size() == 1
                        // its other flag is untouched
                        && countFor(ids[0], "Mt") == 1
                        // and so is the same flag on the note beside it
                        && countFor(ids[1], "g") == 2
                        // with g gone the item stops being offered for it
                        && !flagRoll.flagResetAvailableForNote(ids[0]);
                    pointResetReport = " g=" + juce::String((int)countFor(ids[0], "g"))
                        + " Mt=" + juce::String((int)countFor(ids[0], "Mt"))
                        + " neighbour_g=" + juce::String((int)countFor(ids[1], "g"));
                    flagProject.setNoteUtauFlagCurve(ids[0], "Mt", {});
                    flagProject.setNoteUtauFlagCurve(ids[1], "g", {});
                    flagProject.setNoteUtauFlagCurve(ids[1], "Mt", {});
                    flagRoll.diagnosticRefresh();
                }
                // The switch button: left picks the flag, right opens the
                // flag's own menu, whose one item resets that flag across
                // every note on show.  Notes with the switch off are not
                // touched, and the other flags are not touched.
                {
                    lay(ids[0]);
                    lay(ids[1]);
                    lay(ids[2]);
                    flagProject.setNotesUtauFlagCurveEnabled({ ids[2] }, false);
                    flagRoll.setFlagLaneFlag("g");
                    flagRoll.diagnosticRefresh();
                    const auto countFor = [&](const juce::String& id,
                                              const juce::String& flag)
                    {
                        const auto snap = flagProject.snapshot();
                        for (const auto& note : snap.tracks.front().clips.front().notes)
                            if (note.id == id)
                                return flagCurvePointsFor(note, flag).size();
                        return std::size_t{};
                    };
                    // Both buttons used to land on the flag picker; only the
                    // left one does now.
                    const auto source = juce::Desktop::getInstance().getMainMouseSource();
                    const auto at = flagRoll.flagLaneSwitchBounds().getCentre();
                    const auto rightClick = juce::MouseEvent(source, at,
                        juce::ModifierKeys::rightButtonModifier,
                        juce::MouseInputSource::defaultPressure,
                        0.0f, 0.0f, 0.0f, 0.0f, &flagRoll, &flagRoll,
                        juce::Time::getCurrentTime(), at,
                        juce::Time::getCurrentTime(), 1, false);
                    laneResetRule = flagRoll.flagResetAvailableForLane()
                        // the lane knows about more than the handful given curves
                        && flagRoll.laneNoteIds().size() >= 3;
                    // A right click on the switch must not change the flag.
                    flagRoll.mouseDown(rightClick);
                    laneResetRule = laneResetRule
                        && flagRoll.flagLaneFlag() == juce::String("g");
                    juce::PopupMenu::dismissAllActiveMenus();

                    // What that menu's item does.
                    flagProject.resetNotesUtauFlagCurve(flagRoll.laneNoteIds(), "g");
                    flagRoll.diagnosticRefresh();
                    laneResetRule = laneResetRule
                        // g is gone from every switched-on note on show
                        && countFor(ids[0], "g") == 0 && countFor(ids[1], "g") == 0
                        // their other flag is untouched
                        && countFor(ids[0], "Mt") == 1 && countFor(ids[1], "Mt") == 1
                        // and the switched-off note keeps everything
                        && countFor(ids[2], "g") == 2 && countFor(ids[2], "Mt") == 1
                        // with nothing left on g the item stops being offered
                        && !flagRoll.flagResetAvailableForLane();
                    laneResetReport = " g=" + juce::String((int)countFor(ids[0], "g"))
                        + "," + juce::String((int)countFor(ids[1], "g"))
                        + " Mt=" + juce::String((int)countFor(ids[0], "Mt"))
                        + " off_kept=" + juce::String((int)countFor(ids[2], "g"))
                        + " lane_notes=" + juce::String((int)flagRoll.laneNoteIds().size());
                    // Another flag still has its curves, so the lane offers it.
                    flagRoll.setFlagLaneFlag("Mt");
                    laneResetRule = laneResetRule && flagRoll.flagResetAvailableForLane();
                    flagRoll.setFlagLaneFlag("g");

                    flagProject.setNotesUtauFlagCurveEnabled({ ids[2] }, true);
                    for (const auto& id : { ids[0], ids[1], ids[2] })
                        for (const auto* flag : { "g", "Mt" })
                            flagProject.setNoteUtauFlagCurve(id, flag, {});
                    flagRoll.diagnosticRefresh();
                }


            }
            // b and bh act on the onset alone -- measured, not assumed: on
            // si-/shi-/ban they move the onset by 8-99% and leave the vowel
            // within 4% and 0.1%, while Mb moves the vowel by 10-18%.  So
            // their curves live between the lead-in and the note's start,
            // and every other flag still has the whole note.
            auto onsetOnlyHolds = true;
            juce::String firstBad;
            if (!ids.empty())
            {
                // A real lead-in, so the onset is a stretch and not the sliver
                // that stands in when nothing has been rendered yet.
                constexpr auto leadIn = 0.14;
                for (const auto& id : ids)
                    flagProject.setNoteUtauTimingOverrides(id, true, leadIn, 0.04);
                flagRoll.diagnosticRefresh();
                auto note = flagProject.snapshot().tracks.front()
                                .clips.front().notes.front();
                onsetOnlyHolds = std::abs(note.utauPreutteranceSeconds - leadIn) < 1.0e-9;
                note.utauFlagCurves.clear();
                for (const auto& kind : flagCurveKinds())
                {
                    flagRoll.setFlagLaneFlag(kind.flag);
                    const auto [from, to] = flagRoll.flagLaneSpanFor(note);
                    const auto drawn = flagRoll.flagLaneCurveFor(note);
                    const auto before = onsetOnlyHolds;
                    onsetOnlyHolds = onsetOnlyHolds && to > from
                        && drawn.size() == 1
                        && std::abs(drawn.front().timeSeconds - to) < 1.0e-9
                        && (kind.onsetOnly
                                // starts before the note and stops well short
                                // of its end -- where con_out falls inside that
                                // is the next check's business
                                ? from < 0.0 && to < std::max(0.02, note.durationSeconds)
                                // the note itself, from its start onwards
                                // the sounding stretch, consonant included
                                : from <= 0.0 && to > 0.0);
                    if (before && !onsetOnlyHolds && firstBad.isEmpty())
                        firstBad = juce::String(kind.flag) + "["
                            + juce::String(from, 4) + ".." + juce::String(to, 4)
                            + " dur " + juce::String(note.durationSeconds, 4)
                            + " pts " + juce::String((int)drawn.size()) + "]";
                }
                // A normal flag starts where b does -- the sounding start,
                // a lead-in ahead of the note -- and carries on past where b
                // stops.  Same consonant, more of the note.
                const auto withLeadIn = flagProject.snapshot().tracks.front()
                                            .clips.front().notes[1];
                flagRoll.setFlagLaneFlag("b");
                const auto onsetSpan = flagRoll.flagLaneSpanFor(withLeadIn);
                flagRoll.setFlagLaneFlag("g");
                const auto wholeSpan = flagRoll.flagLaneSpanFor(withLeadIn);
                onsetOnlyHolds = onsetOnlyHolds
                    && std::abs(wholeSpan.first - onsetSpan.first) < 1.0e-9
                    && wholeSpan.first < 0.0
                    && wholeSpan.second > onsetSpan.second;
                // Only these two, and both of them.
                juce::StringArray restricted;
                for (const auto& kind : flagCurveKinds())
                    if (kind.onsetOnly) restricted.add(kind.flag);
                restricted.sort(false);
                onsetOnlyHolds = onsetOnlyHolds
                    && restricted.joinIntoString(",") == "b,bh";
            }
            // With a real voicebank and no region plan, the onset ends where
            // the engine's con_out does -- the oto consonant scaled by
            // velocity, from the start of the rendered stretch -- which for
            // la- (129 ms consonant, 65 ms lead-in) is 64 ms past the note's
            // start, not at it.  Getting this wrong hides a stretch bh really
            // does reach.
            auto classicOnsetHolds = true;
            juce::String classicReport("skipped");
            if (arguments.size() >= 5 && !ids.empty())
            {
                const auto bank = juce::File(arguments[3].unquoted());
                flagProject.setTrackVoicebankDirectory(trackId, bank);
                flagProject.setNoteLabel(ids.front(), arguments[4].unquoted());
                for (const auto planned : { false, true })
                {
                    flagProject.setTrackUtauMode(trackId, planned ? UtauMode::jie : UtauMode::classic);
                    flagRoll.diagnosticRefresh();
                    auto probe = flagProject.snapshot().tracks.front()
                                     .clips.front().notes.front();
                    probe.utauFlagCurves.clear();
                    flagRoll.setFlagLaneFlag("bh");
                    const auto [from, to] = flagRoll.flagLaneSpanFor(probe);
                    classicReport += juce::String(planned ? " planned=" : " classic=")
                        + juce::String(from, 4) + ".." + juce::String(to, 4);
                    classicOnsetHolds = classicOnsetHolds && from < -0.001
                        && (planned ? std::abs(to) < 1.0e-9 : to > 0.02);
                }
                classicReport = classicReport.replace("skipped", "");
                flagProject.setTrackUtauMode(trackId, UtauMode::jie);
                flagRoll.diagnosticRefresh();
            }
            // Vertical zoom: b runs -20..100, so a unit is a fraction of a
            // pixel at the full view.  Zooming in has to narrow the window,
            // sharpen the scale by that much, and still let a handle reach
            // any value the flag allows.
            flagRoll.setFlagLaneFlag("b");
            auto zoomSharpens = true;
            auto zoomReaches = true;
            const auto& bKind = flagCurveKindFor("b");
            const auto fullWindow = flagRoll.flagLaneWindow();
            const auto pixelsPerUnitAt = [&flagRoll]
            {
                return std::abs(flagRoll.flagLaneY(1.0f) - flagRoll.flagLaneY(0.0f));
            };
            const auto atFullView = pixelsPerUnitAt();
            zoomSharpens = zoomSharpens
                && std::abs(fullWindow.first - bKind.minimum) < 1.0e-3f
                && std::abs(fullWindow.second - bKind.maximum) < 1.0e-3f;
            // Zooming out at the full view stays there rather than wrapping.
            flagRoll.nudgeFlagLaneZoom(false);
            zoomSharpens = zoomSharpens && std::abs(flagRoll.flagLaneZoom() - 1.0f) < 1.0e-3f;
            auto previous = 1.0f;
            for (auto step = 0; step < 5; ++step)
            {
                flagRoll.nudgeFlagLaneZoom(true);
                const auto window = flagRoll.flagLaneWindow();
                const auto span = window.second - window.first;
                zoomSharpens = zoomSharpens
                    && flagRoll.flagLaneZoom() > previous
                    && span < (bKind.maximum - bKind.minimum) - 1.0e-3f
                    && std::abs(span * flagRoll.flagLaneZoom()
                                - (bKind.maximum - bKind.minimum)) < 0.05f
                    // The window never leaves the flag's own range.
                    && window.first >= bKind.minimum - 1.0e-3f
                    && window.second <= bKind.maximum + 1.0e-3f;
                previous = flagRoll.flagLaneZoom();
            }
            zoomSharpens = zoomSharpens && pixelsPerUnitAt() > atFullView * 3.0f;
            // Zoomed in, dragging a handle above the window carries the window
            // with it, so the top of the flag's range is still reachable.
            {
                const auto plot = flagRoll.flagLanePlotBounds();
                const auto reached = flagRoll.flagValueFromLaneY(plot.getY() - 4000.0f);
                zoomReaches = std::abs(reached - bKind.maximum) < 1.0e-3f
                    && std::abs(flagRoll.flagValueFromLaneY(plot.getBottom() + 4000.0f)
                                - bKind.minimum) < 1.0e-3f;
            }
            // The buttons sit beside the switch, inside the lane, not on it.
            const auto zoomIn = flagRoll.flagLaneZoomButtonBounds(true);
            const auto zoomOut = flagRoll.flagLaneZoomButtonBounds(false);
            const auto zoomPlaced = flagRoll.flagLaneBounds().contains(zoomIn)
                && flagRoll.flagLaneBounds().contains(zoomOut)
                && !zoomIn.intersects(flagRoll.flagLaneSwitchBounds())
                && !zoomOut.intersects(zoomIn)
                && zoomIn.getRight() < flagRoll.flagLaneSwitchBounds().getX() + 1.0f;
            {
                // A fourth picture: b zoomed in, where the flat lines at zero
                // sat in the bottom eighth of the lane at the full view.
                flagRoll.diagnosticRefresh();
                const auto zoomShot = flagRoll.createComponentSnapshot(
                    flagRoll.flagLaneBounds().toNearestInt().withWidth(760),
                    true, 2.0f);
                juce::File zoomFile(juce::File(arguments[2].unquoted())
                    .getSiblingFile(juce::File(arguments[2].unquoted())
                        .getFileNameWithoutExtension() + "-zoom.png"));
                zoomFile.deleteFile();
                juce::PNGImageFormat zoomPng;
                std::unique_ptr<juce::FileOutputStream> zoomStream(
                    zoomFile.createOutputStream());
                if (zoomStream != nullptr)
                    zoomPng.writeImageToStream(zoomShot, *zoomStream);
            }
            for (auto step = 0; step < 6; ++step) flagRoll.nudgeFlagLaneZoom(false);
            zoomSharpens = zoomSharpens
                && std::abs(flagRoll.flagLaneZoom() - 1.0f) < 1.0e-3f
                && std::abs(pixelsPerUnitAt() - atFullView) < 1.0e-4f;
            flagRoll.setFlagLaneFlag("Mt");
            flagRoll.diagnosticRefresh();
            {
                // A third picture, on a flag nothing was ever drawn for: the
                // lane has to show its own range and a line per enabled note.
                const auto altShot = flagRoll.createComponentSnapshot(
                    flagRoll.flagLaneBounds().toNearestInt().withWidth(760),
                    true, 2.0f);
                juce::File altFile(juce::File(arguments[2].unquoted())
                    .getSiblingFile(juce::File(arguments[2].unquoted())
                        .getFileNameWithoutExtension() + "-other.png"));
                altFile.deleteFile();
                juce::PNGImageFormat altPng;
                std::unique_ptr<juce::FileOutputStream> altStream(
                    altFile.createOutputStream());
                if (altStream != nullptr)
                    altPng.writeImageToStream(altShot, *altStream);
            }
            std::cout << "utau_items_are_7_to_9=" << (stretchHiddenInUtau ? 1 : 0)
                      << "|switch_writes_nothing=" << (switchWritesNothing ? 1 : 0)
                      << "|flag_reset_rule=" << (resetRule ? 1 : 0)
                      << "|reset" << resetReport
                      << "|point_reset" << pointResetReport
                      << "|lane_reset_rule=" << (laneResetRule ? 1 : 0)
                      << "|lane_reset" << laneResetReport
                      << "|round_trip_worst=" << juce::String(worstRoundTrip, 6)
                      << "|pinned=" << juce::String(pinnedLow, 1)
                      << ".." << juce::String(pinnedHigh, 1)
                      << "|points_kept=" << stored.size()
                      << "|maps_back=" << (worstRoundTrip < 0.05f ? 1 : 0)
                      << "|pins_out_of_range=" << ((std::abs(pinnedHigh - 50.0f) < 0.05f
                            && std::abs(pinnedLow + 50.0f) < 0.05f) ? 1 : 0)
                      << "|higher_is_higher=" << (risesUpward ? 1 : 0)
                      << "|curve_stored=" << (kept ? 1 : 0)
                      << "|menu_offers_the_right_items=" << (menuRule ? 1 : 0)
                      << "|typed_value_clamped=" << (clamped ? 1 : 0)
                      << "|reset_to_default=" << (wentBack ? 1 : 0)
                      << "|point_deleted=" << (deleted ? 1 : 0)
                      << "|flags_offered=" << offered.size()
                      << "|ranges_match_the_engine=" << (rangesHold ? 1 : 0)
                      << "|curves_are_independent=" << (independent ? 1 : 0)
                      << "|switch_at_the_top_right=" << (switchPlaced ? 1 : 0)
                      << "|every_flag_starts_drawable=" << (everyFlagDrawable ? 1 : 0)
                      << "|last_handle_moves=" << (lastHandleMoves ? 1 : 0)
                      << "|handles" << handleReport
                      << "|b_and_bh_are_onset_only=" << (onsetOnlyHolds ? 1 : 0)
                      << "|first_bad=" << (firstBad.isEmpty() ? juce::String("-") : firstBad)
                      << "|onset_follows_con_out=" << (classicOnsetHolds ? 1 : 0)
                      << "|spans" << classicReport
                      << "|zoom_sharpens_the_scale=" << (zoomSharpens ? 1 : 0)
                      << "|zoom_still_reaches_the_ends=" << (zoomReaches ? 1 : 0)
                      << "|zoom_buttons_beside_the_switch=" << (zoomPlaced ? 1 : 0)
                      << "|written=" << (written ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                worstRoundTrip < 0.05f && risesUpward && kept && written
                    && menuRule && clamped && wentBack && deleted
                    && rangesHold && independent && offered.size() >= 12 && switchPlaced
                    && everyFlagDrawable && onsetOnlyHolds && classicOnsetHolds
                    && switchWritesNothing && resetRule && laneResetRule
                    && stretchHiddenInUtau
                    && lastHandleMoves
                    && zoomSharpens && zoomReaches && zoomPlaced
                    && std::abs(pinnedHigh - 50.0f) < 0.05f
                    && std::abs(pinnedLow + 50.0f) < 0.05f ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (arguments.size() >= 2 && arguments[0] == "--smoke-keyboard-labels")
        {
            // Every key carries its name when the rows are tall enough, and
            // the rule steps down rather than letting names collide.  An empty
            // project keeps the canvas small enough to photograph the strip.
            const auto shown = [](int midi, float rowHeight)
            {
                return PianoRollComponent::keyboardLabelVisible(midi, rowHeight);
            };
            const auto countAt = [&](float rowHeight)
            {
                auto total = 0;
                for (int midi = 60; midi < 72; ++midi)
                    if (shown(midi, rowHeight)) ++total;
                return total;
            };
            const auto allTwelve = countAt(22.0f) == 12 && countAt(11.0f) == 12;
            const auto naturalsOnly = countAt(9.0f) == 7 && countAt(7.5f) == 7;
            const auto cOnly = countAt(6.0f) == 1 && countAt(2.0f) == 1;
            // C never drops out, whatever the height.
            auto cAlwaysThere = true;
            for (const auto height : { 40.0f, 22.0f, 11.0f, 9.0f, 6.0f, 1.0f })
                cAlwaysThere = cAlwaysThere && shown(60, height) && shown(48, height);

            I18n keyStrings;
            ProjectModel keyProject;
            PianoRollComponent keyRoll(keyProject, keyStrings);
            keyRoll.setRowHeight(22.0f);
            keyRoll.setBounds(0, 0, 260, 460);
            keyRoll.resized();
            const auto shot = keyRoll.createComponentSnapshot(
                juce::Rectangle<int>(0, 0, 60, 460), true, 3.0f);
            juce::File out(arguments[1].unquoted());
            out.deleteFile();
            juce::PNGImageFormat png;
            std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
            const auto written = stream != nullptr && png.writeImageToStream(shot, *stream);
            stream.reset();

            std::cout << "at_22px=" << countAt(22.0f)
                      << "|at_9px=" << countAt(9.0f)
                      << "|at_6px=" << countAt(6.0f)
                      << "|all_twelve_when_tall=" << (allTwelve ? 1 : 0)
                      << "|naturals_when_short=" << (naturalsOnly ? 1 : 0)
                      << "|c_only_when_tiny=" << (cOnly ? 1 : 0)
                      << "|c_never_drops=" << (cAlwaysThere ? 1 : 0)
                      << "|written=" << (written ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                allTwelve && naturalsOnly && cOnly && cAlwaysThere && written ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (!arguments.isEmpty() && arguments[0] == "--smoke-lane-labels")
        {
            // The loudness lane's zoom-out button is a real minus sign, U+2212.
            // Handed to juce::String as bytes it was decoded as the local code
            // page and drew as a box; decoded as UTF-8 it is one character.
            const auto plus = PianoRollComponent::amplitudeZoomLabel(true);
            const auto minus = PianoRollComponent::amplitudeZoomLabel(false);
            const auto asBytes = juce::String("−");   // the old way: raw bytes
            const auto plusIsPlus = plus == "+";
            const auto oneCharacter = minus.length() == 1;
            const auto isMinusSign = oneCharacter && *minus.getCharPointer() == 0x2212;
            // And the bug is real: read as bytes it is not one character.
            const auto bytesWereWrong = asBytes.length() != 1;
            std::cout << "plus=" << plus
                      << "|minus_length=" << minus.length()
                      << "|minus_codepoint=" << (oneCharacter
                            ? static_cast<int>(*minus.getCharPointer()) : -1)
                      << "|as_bytes_length=" << asBytes.length()
                      << "|plus_is_plus=" << (plusIsPlus ? 1 : 0)
                      << "|is_the_minus_sign=" << (isMinusSign ? 1 : 0)
                      << "|raw_bytes_would_be_wrong=" << (bytesWereWrong ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                plusIsPlus && isMinusSign && bytesWereWrong ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (!arguments.isEmpty() && arguments[0] == "--smoke-export-targets")
        {
            // Which tracks an export writes, and where each one goes.  A track
            // that would not sound is left out rather than written as silence,
            // and every file has to be told apart from the others.
            const auto makeTrack = [](const char* id, const char* name, bool muted,
                                      bool solo, bool hasClips)
            {
                TrackData track;
                track.id = id;
                track.name = name;
                track.muted = muted;
                track.solo = solo;
                if (hasClips) track.clips.push_back(ClipData {});
                return track;
            };
            ProjectData song;
            song.name = "song";
            song.tracks.push_back(makeTrack("t1", "Lead", false, false, true));
            song.tracks.push_back(makeTrack("t2", "Harmony", false, false, true));
            song.tracks.push_back(makeTrack("t3", "Scratch", true, false, true));
            song.tracks.push_back(makeTrack("t4", "", false, false, true));
            song.tracks.push_back(makeTrack("t5", "Empty", false, false, false));
            const juce::File destination =
                juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("song.wav");

            // Where the chooser opens: last time's folder, or Documents.
            const auto documents =
                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
            const auto elsewhere =
                juce::File::getSpecialLocation(juce::File::tempDirectory);
            const auto firstTime = MainComponent::exportStartFile({}, "song");
            const auto comingBack = MainComponent::exportStartFile(elsewhere, "song");
            const auto goneAway = MainComponent::exportStartFile(
                elsewhere.getChildFile("no such folder here"), "song");
            const auto opensInDocuments =
                firstTime.getParentDirectory() == documents
                && firstTime.getFileName() == "song.wav";
            const auto opensWhereItLeftOff =
                comingBack.getParentDirectory() == elsewhere
                && comingBack.getFileName() == "song.wav";
            const auto fallsBack = goneAway.getParentDirectory() == documents;
            // A name the filesystem would refuse is made legal, not passed on.
            const auto awkward = MainComponent::exportStartFile(elsewhere, "a/b:c*d");
            const auto nameMadeLegal = !awkward.getFileName().containsAnyOf("/:*")
                && awkward.hasFileExtension("wav");

            const auto all = MainComponent::exportTargets(song, destination, {}, "Untitled");
            juce::StringArray names;
            for (const auto& target : all) names.add(target.file.getFileName());
            // Muted and clipless tracks are not written at all.
            const auto skipsSilent = all.size() == 3
                && std::none_of(all.begin(), all.end(), [](const auto& target)
                   { return target.trackId == "t3" || target.trackId == "t5"; });
            // One file each, all different, all beside the chosen one.
            auto distinct = names;
            distinct.removeDuplicates(false);
            const auto oneEach = distinct.size() == names.size();
            const auto besideIt = std::all_of(all.begin(), all.end(),
                [&destination](const auto& target)
                { return target.file.getParentDirectory() == destination.getParentDirectory(); });
            const auto namesTheTrack = names.contains("song - Lead.wav")
                && names.contains("song - Harmony.wav");
            const auto namesTheUntitled = names.contains("song - Untitled.wav");

            // One track by name goes to the file that was named, unchanged.
            const auto single = MainComponent::exportTargets(song, destination, "t2", "Untitled");
            const auto singleFile = single.size() == 1 && single.front().file == destination
                && single.front().trackId == "t2";
            // Asking for a muted one yields nothing to write, rather than a
            // silent file.
            const auto mutedAsked = MainComponent::exportTargets(song, destination, "t3", "Untitled");

            // With something soloed, only that track is written.
            song.tracks[1].solo = true;
            const auto soloed = MainComponent::exportTargets(song, destination, {}, "Untitled");
            const auto soloWins = soloed.size() == 1 && soloed.front().trackId == "t2";

            std::cout << "opens_at=" << comingBack.getParentDirectory().getFullPathName()
                      << "|opens_in_documents_first=" << (opensInDocuments ? 1 : 0)
                      << "|opens_where_it_left_off=" << (opensWhereItLeftOff ? 1 : 0)
                      << "|falls_back_if_gone=" << (fallsBack ? 1 : 0)
                      << "|name_made_legal=" << (nameMadeLegal ? 1 : 0)
                      << std::endl;
            std::cout << "tracks=" << song.tracks.size()
                      << "|written=" << all.size()
                      << "|files=" << names.joinIntoString(" , ")
                      << "|skips_silent_and_empty=" << (skipsSilent ? 1 : 0)
                      << "|one_file_each=" << (oneEach ? 1 : 0)
                      << "|beside_the_chosen_one=" << (besideIt ? 1 : 0)
                      << "|named_after_the_track=" << (namesTheTrack ? 1 : 0)
                      << "|untitled_still_named=" << (namesTheUntitled ? 1 : 0)
                      << "|single_goes_where_asked=" << (singleFile ? 1 : 0)
                      << "|muted_single_writes_nothing=" << (mutedAsked.empty() ? 1 : 0)
                      << "|solo_wins=" << (soloWins ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                skipsSilent && oneEach && besideIt && namesTheTrack && namesTheUntitled
                    && singleFile && mutedAsked.empty() && soloWins
                    && opensInDocuments && opensWhereItLeftOff && fallsBack
                    && nameMadeLegal ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }

        if (arguments.size() >= 2 && arguments[0] == "--smoke-audition-position")
        {
            // Auditioning a sample plays a file on its own timeline, starting
            // at zero.  The project's own position has to survive that: it did
            // not, so closing the sample editor sent the playhead to the very
            // beginning, and the next zoom -- which keeps the playhead centred
            // -- dragged the whole view after it.
            AudioEngine engine;
            const juce::File sample(arguments[1].unquoted());
            if (!sample.existsAsFile())
            {
                std::cout << "no_sample" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            constexpr auto parked = 12.5;
            engine.setPosition(parked);
            const auto before = engine.position();
            const auto opened = engine.setAuditionFile(sample);
            const auto whileAuditioning = engine.position();
            // Swapping one sample for another must not overwrite what was
            // saved on the way in.
            engine.setPosition(3.0);
            engine.setAuditionFile(sample);
            engine.clearAuditionFile();
            const auto afterwards = engine.position();
            const auto parkedCorrectly = std::abs(before - parked) < 1.0e-6;
            const auto auditionStartsAtZero = std::abs(whileAuditioning) < 1.0e-9;
            const auto restored = std::abs(afterwards - parked) < 1.0e-6;
            std::cout << "opened=" << (opened ? 1 : 0)
                      << "|before=" << before
                      << "|while_auditioning=" << whileAuditioning
                      << "|after_closing=" << afterwards
                      << "|parked=" << (parkedCorrectly ? 1 : 0)
                      << "|audition_from_zero=" << (auditionStartsAtZero ? 1 : 0)
                      << "|position_restored=" << (restored ? 1 : 0) << std::endl;
            setApplicationReturnValue(
                opened && parkedCorrectly && auditionStartsAtZero && restored ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 4 && arguments[0] == "--smoke-lyric-envelope")
        {
            // An imported note has no lyric, so no voicebank entry, so no
            // envelope at all -- and no envelope is not the shape it is drawn
            // with, it is no shaping whatever.  Typing a lyric should hand it
            // that shape without anyone reaching for the preset button.
            I18n shapeStrings;
            ProjectModel shapeProject;
            juce::String shapeError;
            if (!shapeProject.load(juce::File(arguments[1].unquoted()), shapeError))
            {
                std::cout << "loaded=0|error=" << shapeError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto alias = arguments[3].unquoted();
            PianoRollComponent shapeRoll(shapeProject, shapeStrings);
            shapeRoll.setBounds(0, 0, 1600, 900);
            shapeRoll.resized();
            constexpr std::size_t probe = 2;
            const auto noteId = shapeProject.snapshot()
                .tracks.front().clips.front().notes[probe].id;
            const auto storedPoints = [&]
            {
                return shapeProject.snapshot()
                    .tracks.front().clips.front().notes[probe].amplitudeEnvelope;
            };
            const auto ramps = [&]
            {
                const auto points = storedPoints();
                if (points.size() < 4) return std::pair<double, double> { -1.0, -1.0 };
                return std::pair<double, double> {
                    points[1].timeSeconds - points[0].timeSeconds,
                    points.back().timeSeconds - points[points.size() - 2].timeSeconds };
            };

            // An imported note: no lyric, nothing shaped.
            shapeProject.setNoteLabel(noteId, {});
            shapeRoll.ensureDefaultEnvelope(noteId);
            const auto whileEmpty = storedPoints().size();

            shapeProject.setNoteLabel(noteId, alias);
            shapeRoll.ensureDefaultEnvelope(noteId);
            const auto afterLyric = storedPoints().size();
            const auto written = ramps();

            // Anything already shaped by hand must survive a later lyric.
            const std::vector<AmplitudeEnvelopePoint> byHand {
                { 0.0f, -60.0f }, { 0.2f, 0.0f }, { 0.3f, -6.0f }, { 0.4f, -60.0f } };
            shapeProject.setNotesAmplitudeEnvelopes({ { noteId, byHand } });
            shapeProject.setNoteLabel(noteId, alias + " ");
            shapeRoll.ensureDefaultEnvelope(noteId);
            const auto handRamps = ramps();

            const auto startedBare = whileEmpty == 0;
            const auto shaped = afterLyric == 4
                && std::abs(written.first - 0.015) < 1.0e-6
                && std::abs(written.second - 0.035) < 1.0e-6;
            const auto handKept = std::abs(handRamps.first - 0.2) < 1.0e-6;
            std::cout << "points_while_empty=" << whileEmpty
                      << "|points_after_lyric=" << afterLyric
                      << "|written_ramps=" << written.first << "," << written.second
                      << "|hand_ramps=" << handRamps.first << "," << handRamps.second
                      << "|bare_until_lyric=" << (startedBare ? 1 : 0)
                      << "|soft_rise_written=" << (shaped ? 1 : 0)
                      << "|hand_shape_kept=" << (handKept ? 1 : 0) << std::endl;
            setApplicationReturnValue(
                startedBare && shaped && handKept ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 5 && arguments[0] == "--smoke-lyric-timing")
        {
            // Pin a note's timing by hand, then give it a different lyric.
            // Preutterance and overlap are marks on one recording, so carrying
            // them across pinned the new sound's lead-in to a length its own
            // oto never asked for -- and a pin outranks the velocity, so the
            // consonant handle stopped answering as well.
            I18n lyricStrings;
            ProjectModel lyricProject;
            juce::String lyricError;
            if (!lyricProject.load(juce::File(arguments[1].unquoted()), lyricError))
            {
                std::cout << "loaded=0|error=" << lyricError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const juce::File voicebank(arguments[2].unquoted());
            const auto first = arguments[3].unquoted();
            const auto second = arguments[4].unquoted();
            PianoRollComponent lyricRoll(lyricProject, lyricStrings);
            lyricRoll.setBounds(0, 0, 1600, 900);
            lyricRoll.resized();
            // Not the first note: a lead-in cannot reach back past the start
            // of the piece, so on that one every preutterance looks like zero.
            constexpr std::size_t probe = 2;
            const auto noteId = lyricProject.snapshot()
                .tracks.front().clips.front().notes[probe].id;
            const auto leadIn = [&]
            {
                lyricRoll.diagnosticRefresh();
                const auto& note = lyricRoll.diagnosticNotes()[probe];
                return note.start - note.soundingStart;
            };
            const auto pinned = [&]
            {
                const auto& note = lyricProject.snapshot()
                    .tracks.front().clips.front().notes[probe];
                return note.utauPreutteranceOverrideEnabled
                    || note.utauOverlapOverrideEnabled;
            };
            const auto splitPinned = [&]
            {
                const auto& note = lyricProject.snapshot()
                    .tracks.front().clips.front().notes[probe];
                return note.utauJieSplitSet || note.utauJieSplit1 != 0.0
                    || note.utauJieSplit2 != 0.0 || note.utauJieSplit3 != 0.0;
            };
            const auto otoLeadIn = [&](const juce::String& alias, int velocity)
            {
                const auto timing = backend::UtauRenderer::sampleTiming(
                    voicebank, alias, 62.0f, velocity, false);
                return timing ? timing->preutteranceSeconds : -1.0;
            };

            lyricProject.setNoteLabel(noteId, first);
            const auto firstOwn = otoLeadIn(first, 100);
            const auto secondOwn = otoLeadIn(second, 100);
            // Pin something neither lyric would ever ask for.
            constexpr auto pin = 0.25;
            lyricProject.setNoteUtauTimingOverrides(noteId, true, pin, 0.05);
            // Where the consonant, glide and tail sit in this one recording.
            lyricProject.setNotesUtauJieSplit({ noteId }, 0.1, 0.2, 0.3);
            const auto splitTook = splitPinned();
            const auto whilePinned = leadIn();
            lyricProject.setNoteLabel(noteId, second);
            const auto afterSwap = leadIn();
            const auto stillPinned = pinned();
            const auto splitSurvived = splitPinned();
            // With the pin gone the consonant velocity has to reach the
            // lead-in again; while it was in place this did nothing at all.
            lyricProject.setNotesUtauConsonantVelocity({ noteId }, 200);
            const auto afterVelocity = leadIn();
            lyricProject.setNotesUtauConsonantVelocity({ noteId }, 100);

            const auto differs = std::abs(firstOwn - secondOwn) > 0.005;
            const auto tookThePin = std::abs(whilePinned - pin) < 1.0e-6;
            const auto released = !stillPinned
                && std::abs(afterSwap - secondOwn) < 1.0e-6;
            const auto splitReleased = splitTook && !splitSurvived;
            const auto velocityWorks = std::abs(afterVelocity - afterSwap) > 1.0e-4;
            std::cout << "oto_lead_in=" << firstOwn << "," << secondOwn
                      << "|while_pinned=" << whilePinned
                      << "|after_swap=" << afterSwap
                      << "|still_pinned=" << (stillPinned ? 1 : 0)
                      << "|split_pinned_then_released="
                      << (splitTook ? 1 : 0) << (splitSurvived ? 1 : 0)
                      << "|after_velocity=" << afterVelocity
                      << "|aliases_differ=" << (differs ? 1 : 0)
                      << "|pin_took_effect=" << (tookThePin ? 1 : 0)
                      << "|released_on_swap=" << (released ? 1 : 0)
                      << "|velocity_reaches_lead_in=" << (velocityWorks ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                differs && tookThePin && released && velocityWorks
                    && splitReleased ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-vibrato-bake")
        {
            // Turning a vibrato into pitch points wrote eight of them a cycle,
            // which is a hundred points to drag by hand on an ordinary note.
            // A sine between one extreme and the next is a single curve, so
            // two a cycle carry the same shape -- and it has to be the same
            // shape, or fewer points would only mean a different vibrato.
            I18n vibStrings;
            ProjectModel vibProject;
            juce::String vibError;
            if (!vibProject.load(juce::File(arguments[1].unquoted()), vibError))
            {
                std::cout << "loaded=0|error=" << vibError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            // A note apiece: baking writes the curve, and a second vibrato
            // on the same note would be laid on top of the first.  Passing an
            // empty curve does not clear it -- that call ignores an empty
            // list -- which is how the first attempt at this measured two
            // vibratos and blamed the arcs.
            // Held by value: the snapshot is returned by value, and front()
            // is a call, so a reference reaching through it into the temporary
            // is left dangling the moment the statement ends.
            const auto clipNotes = vibProject.snapshot()
                .tracks.front().clips.front().notes;
            const auto duration = clipNotes.front().durationSeconds;
            constexpr auto cycleMs = 180.0;
            constexpr auto depthCents = 50.0;
            const auto cycles = duration / (cycleMs / 1000.0);

            auto worst = 0.0;
            std::size_t mostPoints = 0;
            auto worstStraight = 0.0;
            const std::array<double, 2> phases { 0.0, 30.0 };
            for (std::size_t round = 0; round < phases.size(); ++round)
            {
                const auto phasePercent = phases[round];
                const auto noteId = clipNotes[round].id;
                const auto midi = clipNotes[round].midiNote;
                NoteData vibrato;
                vibrato.vibratoLengthPercent = 100.0;
                vibrato.vibratoCycleMs = cycleMs;
                vibrato.vibratoDepthCents = depthCents;
                vibrato.vibratoFadeInPercent = 0.0;
                vibrato.vibratoFadeOutPercent = 0.0;
                vibrato.vibratoPhasePercent = phasePercent;
                vibrato.vibratoOffsetPercent = 0.0;
                vibProject.setNotesVibrato({ noteId }, vibrato, true);
                if (!vibProject.bakeNoteVibratoIntoPitch(noteId))
                {
                    std::cout << "bake_refused" << std::endl;
                    setApplicationReturnValue(3);
                    juce::MessageManager::callAsync([this] { quit(); });
                    return;
                }
                const auto baked = vibProject.snapshot()
                    .tracks.front().clips.front().notes[round].pitchControlPoints;

                // What the vibrato says it is: the points have to reproduce
                // this however few of them there are.
                const auto wanted = [&](double time)
                {
                    const auto turns = time / (cycleMs / 1000.0) + phasePercent / 100.0;
                    return static_cast<double>(midi) + depthCents
                        * std::sin(turns * 2.0 * juce::MathConstants<double>::pi) / 100.0;
                };
                // Measured over the swing itself, leaving out three quarters
                // of a turn at each end: with a phase shift the vibrato steps
                // at the moment the swing begins -- it is zero before and the
                // sine from there -- and no arrangement of points reproduces a
                // step.  What is being measured is the arc between extremes.
                const auto from = 0.75 * cycleMs / 1000.0;
                const auto to = duration - 0.75 * cycleMs / 1000.0;
                const auto against = [&](const std::vector<PitchCurveEditPoint>& points)
                {
                    auto found = 0.0;
                    for (auto index = 0; index <= 4000; ++index)
                    {
                        const auto time = from + (to - from) * index / 4000.0;
                        found = std::max(found, std::abs(
                            static_cast<double>(evaluatePitchCurve(points, time))
                                - wanted(time)) * 100.0);
                    }
                    return found;
                };
                // The same points joined by straight lines.  If that were no
                // worse, the curves between them would be doing nothing.
                auto straightened = baked;
                for (auto& point : straightened) point.shape = PitchCurveShape::linear;

                worst = std::max(worst, against(baked));
                worstStraight = std::max(worstStraight, against(straightened));
                mostPoints = std::max(mostPoints, baked.size());
            }

            const auto sparse = static_cast<double>(mostPoints) <= cycles * 2.0 + 5.0;
            const auto faithful = worst < 1.0;
            const auto curvesEarnTheirPlace = worstStraight > worst * 10.0;
            std::cout << "cycles=" << juce::String(cycles, 2)
                      << "|most_points=" << mostPoints
                      << "|worst_cents=" << juce::String(worst, 3)
                      << "|worst_cents_if_straight=" << juce::String(worstStraight, 3)
                      << "|two_a_cycle=" << (sparse ? 1 : 0)
                      << "|faithful=" << (faithful ? 1 : 0)
                      << "|curves_earn_their_place=" << (curvesEarnTheirPlace ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                sparse && faithful && curvesEarnTheirPlace ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 4 && arguments[0] == "--smoke-mou-note-regions")
        {
            // A note is split the way its own oto entry says.  Annotate the
            // entry two, three or four ways and the roll has to draw that many
            // bands, offer that many boundaries to the mouse, and hand the
            // renderer lengths for that many regions -- it used to draw four
            // whatever the entry said, because it read the voicebank without
            // asking for the annotation at all.
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const juce::File bank(arguments[2].unquoted());
            const auto alias = arguments[3].unquoted();
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            project.setTrackVoicebankDirectory(trackId, bank);
            project.setTrackUtauMode(trackId, UtauMode::mou);
            // Not the first note: a lead-in cannot reach back past the start
            // of the piece, so the first note has none to speak of.
            constexpr std::size_t probe = 2;
            const auto noteId = project.snapshot()
                .tracks.front().clips.front().notes[probe].id;
            project.setNoteLabel(noteId, alias);

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();

            // Annotate the entry, then ask the roll what it is drawing.
            const auto annotate = [&](const juce::String& classes)
            {
                juce::StringArray warnings;
                auto rows = SampleSettings::loadVoicebankOto(bank, warnings, true, true);
                for (const auto& row : rows)
                    if (row.alias == alias || row.sourceName == alias
                        || row.audioFile.getFileNameWithoutExtension() == alias)
                    {
                        auto edited = row;
                        edited.mouClasses = classes;
                        juce::String error;
                        SampleSettings::updateMouOtoEntry(row, edited, error);
                        break;
                    }
                backend::UtauRenderer::invalidateVoicebankCache();
                roll.diagnosticRefresh();
            };

            auto follows = true, grabbable = true;
            juce::String report;
            for (const auto* spelling : { "CVVV", "CVV", "CV", "CVVC" })
            {
                const juce::String classes(spelling);
                annotate(classes);
                const auto drawn = roll.diagnosticRegionCount(probe);
                follows = follows && drawn == classes.length();

                // One boundary fewer than there are regions, and the first is
                // not among them: it is where the oto says the consonant ends,
                // as it is in 界, and it lives in the lead-in before the note.
                const auto edges = roll.diagnosticRegionEdges(probe);
                const auto y = roll.diagnosticNoteY(probe);
                auto offered = 0;
                for (int boundary = 0; boundary < 3; ++boundary)
                {
                    const auto x = roll.diagnosticEdgeX(
                        edges[static_cast<std::size_t>(boundary)]);
                    if (roll.diagnosticJieHandleAt({ x, y })) ++offered;
                }
                grabbable = grabbable && offered == juce::jmax(0, drawn - 2);
                report += " " + classes + ":" + juce::String(drawn)
                    + "/grab" + juce::String(offered);
            }

            // A two-region entry whose last region is a consonant does not
            // answer to the consonant velocity.  That velocity is the front
            // consonant's, and a join between two syllables has none -- so the
            // lead-in stays put, and with it the note's sounding start.
            const auto leadInAt = [&](const juce::String& classes, int velocity)
            {
                annotate(classes);
                const auto timing = backend::UtauRenderer::sampleTiming(
                    bank, alias, 62.0f, velocity, true, true);
                return timing ? timing->preutteranceSeconds : -1.0;
            };
            const auto moves = [&](const juce::String& classes)
            {
                const auto slow = leadInAt(classes, 50);
                const auto fast = leadInAt(classes, 200);
                report += " " + classes + ":" + juce::String(slow * 1000.0, 1)
                    + "/" + juce::String(fast * 1000.0, 1);
                return std::abs(slow - fast) > 1.0e-9;
            };
            report += " lead-in";
            // The lead-in is region 0's length, so it follows region 0's
            // class: the velocity stretches a consonant, and a first region
            // the entry calls a vowel is not one.  It used to move whatever
            // the annotation said, which is what made an unmarked first
            // segment answer to a consonant velocity.
            const auto joinIgnoresVelocity =
                !moves("VC") && !moves("VVVV") && !moves("VCVV")  // a vowel head
                && moves("CV") && moves("CVVC") && moves("CVVV")  // a consonant one
                // Every region a consonant is still a consonant at the front:
                // the velocity is that one's, and it reaches it.
                && moves("CC") && moves("CCCC");

            // The note-front handle.  With a first region the entry calls a
            // vowel the consonant velocity does not reach the lead-in, so
            // dragging it as a velocity moved nothing at all -- and quietly
            // rewrote a setting the note carries.  谋 pins the lead-in the
            // drag asked for instead, and a vowel first region is free between
            // the note in front of it and its own start.
            const auto noteOf = [&](std::size_t index)
            {
                return project.snapshot().tracks.front().clips.front().notes[index];
            };
            const auto previousStart = noteOf(probe - 1).startSeconds
                + project.snapshot().tracks.front().clips.front().startSeconds;
            const auto ownStart = noteOf(probe).startSeconds
                + project.snapshot().tracks.front().clips.front().startSeconds;
            const auto grab = [&](const juce::String& classes)
            {
                annotate(classes);
                project.setNoteUtauTimingOverrides(noteId, false, 0.0, 0.0);
                project.setNotesUtauConsonantVelocity({ noteId }, 100);
                roll.diagnosticRefresh();
                return roll.diagnosticGrabConsonantHandle(probe);
            };
            const auto pinned = [&]
            {
                const auto note = noteOf(probe);
                return note.utauPreutteranceOverrideEnabled
                    ? note.utauPreutteranceSeconds : -1.0;
            };
            const auto velocityOf = [&] { return noteOf(probe).utauConsonantVelocity; };

            // A vowel first region: offered, free back to the note in front.
            auto freeDrag = grab("VVVV");
            const auto freeMax = roll.diagnosticConsonantRangeMax();
            const auto freeMin = roll.diagnosticConsonantRangeMin();
            freeDrag = freeDrag && roll.diagnosticConsonantSetsPin()
                && freeMin <= 1.0e-9
                && freeMax >= std::min(ownStart - previousStart, 0.2) - 1.0e-6;
            roll.diagnosticDragConsonantTo(0.12);
            roll.diagnosticReleaseDrag();
            const auto vowelPin = pinned();
            const auto vowelVelocity = velocityOf();
            freeDrag = freeDrag && std::abs(vowelPin - 0.12) < 1.0e-6
                && vowelVelocity == 100;

            // A consonant first region is a consonant: the velocity reaches it,
            // so the handle goes on speaking through the velocity there, and
            // pins nothing.
            auto consonantDrag = grab("CVVV");
            consonantDrag = consonantDrag && !roll.diagnosticConsonantSetsPin();
            roll.diagnosticDragConsonantTo(0.09);
            roll.diagnosticReleaseDrag();
            const auto consonantPin = pinned();
            const auto consonantVelocity = velocityOf();
            consonantDrag = consonantDrag && consonantPin < 0.0
                && consonantVelocity != 100;

            // Two regions both consonants: a consonant at the front, so the
            // handle speaks through the velocity and the lead-in follows it.
            const auto pairOffered = grab("CC");
            roll.diagnosticDragConsonantTo(0.09);
            roll.diagnosticReleaseDrag();
            const auto pairPin = pinned();
            const auto pairVelocity = velocityOf();

            // 界 is untouched: the same handle still speaks through the
            // velocity there, as it always has.
            project.setNoteUtauTimingOverrides(noteId, false, 0.0, 0.0);
            project.setNotesUtauConsonantVelocity({ noteId }, 100);
            project.setTrackUtauMode(trackId, UtauMode::jie);
            roll.diagnosticRefresh();
            auto jieUnchanged = roll.diagnosticGrabConsonantHandle(probe)
                && !roll.diagnosticConsonantSetsPin();
            roll.diagnosticDragConsonantTo(0.09);
            roll.diagnosticReleaseDrag();
            jieUnchanged = jieUnchanged && pinned() < 0.0 && velocityOf() != 100;
            const auto jieVelocity = velocityOf();
            project.setTrackUtauMode(trackId, UtauMode::mou);
            project.setNotesUtauConsonantVelocity({ noteId }, 100);
            roll.diagnosticRefresh();

            report += " lead-in free[" + juce::String(freeMin, 3) + ".."
                + juce::String(freeMax, 3) + "] gap="
                + juce::String(ownStart - previousStart, 3)
                + " V:pin " + juce::String(vowelPin, 3) + " vel "
                + juce::String(vowelVelocity)
                + " C:pin " + juce::String(consonantPin, 3) + " vel "
                + juce::String(consonantVelocity)
                + " CC:offered " + juce::String(pairOffered ? 1 : 0)
                + " pin " + juce::String(pairPin, 3)
                + " vel " + juce::String(pairVelocity)
                + " jie:vel " + juce::String(jieVelocity);

            // The per-region flag boxes follow the entry too: one per region it
            // really has, named by position in 谋.  They used to be four,
            // always, labelled with 界's parts of a syllable -- so a
            // three-region entry was offered a 韵尾 box for a region that does
            // not exist, and whatever was typed in it went to the engine to be
            // dropped there instead.
            juce::String flagReport;
            auto flagBoxesFollow = true;
            for (const auto* spelling : { "CVVV", "CVV", "CV" })
            {
                const juce::String classes(spelling);
                annotate(classes);
                const auto shown = roll.diagnosticRegionFlagFields(noteId);
                flagBoxesFollow = flagBoxesFollow && shown.size() == classes.length();
                for (int index = 0; index < shown.size(); ++index)
                    flagBoxesFollow = flagBoxesFollow
                        && shown[index].startsWith(juce::String::fromUTF8("第")
                            + juce::String(index + 1) + juce::String::fromUTF8("区"));
                flagReport += " " + classes + ":" + juce::String(shown.size());
            }
            // 界 keeps the four it has always had, under their own names.
            project.setTrackUtauMode(trackId, UtauMode::jie);
            roll.diagnosticRefresh();
            const auto jieBoxes = roll.diagnosticRegionFlagFields(noteId);
            flagBoxesFollow = flagBoxesFollow && jieBoxes.size() == 4
                && jieBoxes[0].startsWith(juce::String::fromUTF8("声母"))
                && jieBoxes[3].startsWith(juce::String::fromUTF8("韵尾"));
            project.setTrackUtauMode(trackId, UtauMode::mou);
            roll.diagnosticRefresh();
            report += " flagboxes" + flagReport + " jie:"
                + juce::String(jieBoxes.size());

            // After a pin the first line still has to land on the note start:
            // the block is drawn against the pinned lead-in, so the boundary
            // inside it has to be measured against the same one.
            grab("VVVV");
            roll.diagnosticDragConsonantTo(0.12);
            roll.diagnosticReleaseDrag();
            roll.diagnosticRefresh();
            const auto pinnedEdges = roll.diagnosticRegionEdges(probe);
            const auto pinnedOnsetHolds = std::abs(pinnedEdges[0] - ownStart) < 1.0e-6;
            report += " pinned_edge0=" + juce::String(pinnedEdges[0], 4);

            // And the first line really is in the lead-in, before the note.
            annotate("CVV");
            const auto edges = roll.diagnosticRegionEdges(probe);
            const auto noteStart = project.snapshot()
                .tracks.front().clips.front().notes[probe].startSeconds
                + project.snapshot().tracks.front().clips.front().startSeconds;
            const auto onsetBeforeNote = edges[0] <= noteStart + 1.0e-6;
            report += " onset=" + juce::String(edges[0], 4)
                + " note=" + juce::String(noteStart, 4);

            std::cout << "count_follows_the_oto=" << (follows ? 1 : 0)
                      << "|flag_boxes_follow_the_entry="
                      << (flagBoxesFollow ? 1 : 0)
                      << "|a_vowel_head_drags_free=" << (freeDrag ? 1 : 0)
                      << "|a_pinned_onset_stays_on_the_note="
                      << (pinnedOnsetHolds ? 1 : 0)
                      << "|a_consonant_head_still_writes_velocity="
                      << (consonantDrag ? 1 : 0)
                      << "|jie_still_speaks_through_velocity=" << (jieUnchanged ? 1 : 0)
                      << "|velocity_only_where_a_consonant_is="
                      << (joinIgnoresVelocity ? 1 : 0)
                      << "|only_real_boundaries_grab=" << (grabbable ? 1 : 0)
                      << "|onset_is_in_the_lead_in=" << (onsetBeforeNote ? 1 : 0)
                      << "|" << report.trim() << std::endl;
            setApplicationReturnValue(follows && grabbable && onsetBeforeNote
                                      && joinIgnoresVelocity && freeDrag
                                      && consonantDrag && jieUnchanged
                                      && pinnedOnsetHolds
                                      && flagBoxesFollow ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-region-guides")
        {
            // The four regions are worth seeing while placing pitch points --
            // where the consonant gives way to the vowel is where a pitch move
            // usually wants to be -- but they must not be editable there, and
            // must read as marks rather than as something to grab.
            I18n guideStrings;
            ProjectModel guideProject;
            juce::String guideError;
            if (!guideProject.load(juce::File(arguments[1].unquoted()), guideError))
            {
                std::cout << "loaded=0|error=" << guideError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = guideProject.snapshot().tracks.front().id;
            PianoRollComponent guideRoll(guideProject, guideStrings);
            guideRoll.setBounds(0, 0, 1600, 900);
            guideRoll.resized();
            const auto shot = [&](bool fourRegion, PianoRollComponent::Tool tool)
            {
                guideProject.setTrackUtauMode(trackId, fourRegion ? UtauMode::jie : UtauMode::classic);
                guideRoll.setTool(tool);
                guideRoll.diagnosticRefresh();
                juce::Image image(juce::Image::ARGB, 1600, 900, true);
                juce::Graphics g(image);
                guideRoll.paintEntireComponent(g, true);
                return image;
            };
            const auto compare = [](const juce::Image& left, const juce::Image& right)
            {
                auto differing = 0;
                for (int y = 0; y < 900; ++y)
                    for (int x = 0; x < 1600; ++x)
                        if (left.getPixelAt(x, y) != right.getPixelAt(x, y)) ++differing;
                return differing;
            };
            const auto editing = shot(true, PianoRollComponent::Tool::note);
            const auto guiding = shot(true, PianoRollComponent::Tool::points);
            const auto without = shot(false, PianoRollComponent::Tool::points);
            const auto controlNote = shot(false, PianoRollComponent::Tool::note);

            // Drawn in point mode: turning the regions off changes the picture.
            const auto shownWhilePointing = compare(guiding, without) > 200;
            // And drawn in note mode too, as before.
            const auto shownWhileEditing = compare(editing, controlNote) > 200;
            // Fainter there than under the tool that can move them.
            const auto quieter = compare(editing, guiding) > 200;
            // Not grabbable: the handles answer only to the note tool.
            guideProject.setTrackUtauMode(trackId, UtauMode::jie);
            guideRoll.setTool(PianoRollComponent::Tool::points);
            guideRoll.diagnosticRefresh();
            const auto edges = guideRoll.diagnosticRegionEdges(2);
            const auto grabbable = guideRoll.diagnosticJieHandleAt(
                juce::Point<float>(guideRoll.diagnosticEdgeX(edges[0]),
                                   guideRoll.diagnosticNoteY(2)));
            guideRoll.setTool(PianoRollComponent::Tool::note);
            const auto grabbableEditing = guideRoll.diagnosticJieHandleAt(
                juce::Point<float>(guideRoll.diagnosticEdgeX(edges[0]),
                                   guideRoll.diagnosticNoteY(2)));

            std::cout << "shown_while_pointing=" << (shownWhilePointing ? 1 : 0)
                      << "|shown_while_editing=" << (shownWhileEditing ? 1 : 0)
                      << "|fainter_while_pointing=" << (quieter ? 1 : 0)
                      << "|grabbable_pointing=" << (grabbable ? 1 : 0)
                      << "|grabbable_editing=" << (grabbableEditing ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                shownWhilePointing && shownWhileEditing && quieter
                    && !grabbable && grabbableEditing ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 4 && arguments[0] == "--smoke-rest-playback")
        {
            // Typing RR over a note that has already been played has to
            // silence it.  Playback falls back to the last render that was
            // ready whenever the current one is not, and a render with nothing
            // to sound in it never becomes ready -- so the note went on
            // playing what it used to be.
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const juce::File bank(arguments[2].unquoted());
            const juce::File resampler(arguments[3].unquoted());
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            project.setTrackVoicebankDirectory(trackId, bank);
            project.setTrackCompose(trackId, true);
            const auto notesOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            constexpr std::size_t probe = 2;
            const auto noteId = notesOf()[probe].id;
            const auto from = notesOf()[probe].startSeconds;
            const auto to = from + notesOf()[probe].durationSeconds;
            project.setNoteLabel(noteId, arguments.size() >= 5
                                             ? arguments[4].unquoted() : "a");

            AudioEngine engine;
            engine.setUtauResamplerFile(resampler);
            // One note at a time, which is how auditioning a note works -- and
            // the case the fallback was standing in for.
            engine.setUtauRenderNoteSelection({ noteId });
            constexpr auto rate = 48'000.0;
            constexpr auto block = 512;
            engine.prepareToPlay(block, rate);
            juce::AudioBuffer<float> scratch(2, block);

            // Play the note and report the loudest sample heard over it.
            const auto peakOver = [&]
            {
                engine.setPosition(std::max(0.0, from - 0.05));
                engine.setPlayUntil(to);
                engine.play();
                auto peak = 0.0f;
                for (int index = 0; index < 900 && engine.isPlaying(); ++index)
                {
                    scratch.clear();
                    juce::AudioSourceChannelInfo info(&scratch, 0, block);
                    engine.getNextAudioBlock(info);
                    peak = std::max(peak, scratch.getMagnitude(0, block));
                }
                engine.stop();
                return peak;
            };
            // Renders run on a pool, so wait for the one just asked for.
            const auto settle = [&]
            {
                for (int spin = 0; spin < 600; ++spin)
                {
                    if (engine.hasCurrentRenderedAudio()) return true;
                    if (!engine.renderProgress().has_value() && spin > 20) return false;
                    juce::Thread::sleep(50);
                }
                return false;
            };

            engine.syncProject(project.snapshot());
            const auto sungReady = settle();
            const auto sung = peakOver();

            project.setNoteLabel(noteId, "RR");
            engine.syncProject(project.snapshot());
            // Nothing sounds now, so waiting for "current rendered audio" would
            // wait forever; give the render the same chance and move on.
            for (int spin = 0; spin < 40 && engine.renderProgress().has_value(); ++spin)
                juce::Thread::sleep(50);
            juce::Thread::sleep(300);
            const auto rest = peakOver();

            const auto sangFirst = sungReady && sung > 0.005f;
            const auto restIsSilent = rest < sung * 0.02f + 1.0e-5f;
            std::cout << "it_sang_first=" << (sangFirst ? 1 : 0)
                      << "|then_the_rest_is_silent=" << (restIsSilent ? 1 : 0)
                      << "|sung=" << juce::String(sung, 5)
                      << " rest=" << juce::String(rest, 5) << std::endl;
            setApplicationReturnValue(sangFirst && restIsSilent ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-rest-lyric")
        {
            // A lyric of RR is a rest: the note holds its stretch of the phrase
            // open and sounds nothing.  Not an empty lyric, which renders the
            // piano preview tone, and not an alias the voicebank happens not to
            // have, which is reported as missing.
            using backend::isRestLyric;
            auto readsTheWord = isRestLyric("RR") && isRestLyric("rr")
                && isRestLyric("  Rr  ")
                // "R" on its own is a real alias in the Chinese CVVC banks --
                // every vowel's release is recorded as "a R" -- so it has to
                // go on reaching the voicebank.
                && !isRestLyric("R") && !isRestLyric("RRR")
                && !isRestLyric("a R") && !isRestLyric("");

            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const juce::File bank(arguments[2].unquoted());
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            project.setTrackVoicebankDirectory(trackId, bank);
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();

            const auto notesOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            // Three in a row: a sung note, the rest, and a sung note after it.
            constexpr std::size_t before = 2, rest = 3, after = 4;
            const auto sung = juce::String("a");
            project.setNoteLabel(notesOf()[before].id, sung);
            project.setNoteLabel(notesOf()[rest].id, sung);
            project.setNoteLabel(notesOf()[after].id, sung);
            roll.diagnosticRefresh();
            const auto sungSpans = roll.diagnosticNotes();
            const auto beforeEndSung = sungSpans[before].soundingEnd;

            project.setNoteLabel(notesOf()[rest].id, "RR");
            roll.diagnosticRefresh();
            const auto spans = roll.diagnosticNotes();
            // The rest sounds nothing at all, so the roll gives it no
            // sounding stretch -- and with none, every UTAU decoration that
            // hangs off one (the lead-in block, the region bands, the handles)
            // has nothing to draw itself against.  The sung notes around it
            // still have theirs.
            const auto restIsSilent =
                !roll.diagnosticHasSoundingSpan(notesOf()[rest].id)
                && roll.diagnosticHasSoundingSpan(notesOf()[before].id)
                && roll.diagnosticHasSoundingSpan(notesOf()[after].id);
            // And it takes nothing from the note in front of it: a rest has no
            // lead-in to reach back with, so what came before runs to its own
            // end rather than being cut short the way a sung note cuts it.
            const auto beforeRunsOn = spans[before].soundingEnd
                >= beforeEndSung - 1.0e-9
                && spans[before].soundingEnd
                    <= notesOf()[before].startSeconds
                       + notesOf()[before].durationSeconds + 1.0e-9;
            // The note after it is unaffected: it still reaches back for its
            // own lead-in.
            const auto afterKeepsItsLeadIn =
                spans[after].start - spans[after].soundingStart > 1.0e-9;

            // And it is drawn: a rest looks different from the sung note it
            // replaced, so it can be seen and picked up rather than being a
            // hole in the phrase.  Compared as pixels, which also says the
            // roll painted anything at all.
            const auto shotOf = [&]
            {
                juce::Image image(juce::Image::ARGB, 1600, 900, true);
                juce::Graphics g(image);
                roll.paintEntireComponent(g, true);
                return image;
            };
            const auto asRest = shotOf();
            // Against a lyric this voicebank has no sample for.  That is silent
            // too and has no sounding span either, so it looks like whatever a
            // note with nothing behind it looks like -- a rest has to be told
            // apart from one of those, not merely from a note that sings.
            project.setNoteLabel(notesOf()[rest].id, "ZZZZ");
            roll.diagnosticRefresh();
            const auto asUnknown = shotOf();
            project.setNoteLabel(notesOf()[rest].id, "RR");
            roll.diagnosticRefresh();
            auto differing = 0;
            for (int y = 0; y < 900; ++y)
                for (int x = 0; x < 1600; ++x)
                    if (asRest.getPixelAt(x, y) != asUnknown.getPixelAt(x, y))
                        ++differing;
            const auto looksDifferent = differing > 200;

            if (arguments.size() >= 4)
            {
                const juce::File shot(arguments[3].unquoted());
                shot.deleteFile();
                juce::PNGImageFormat png;
                juce::FileOutputStream stream(shot);
                png.writeImageToStream(asRest, stream);
            }

            // A rest is silent on purpose.  A lyric the voicebank has no
            // sample for is silent too, but it is a mistake and is reported as
            // one -- that report is the whole difference between the two, and
            // without it "RR" was only ever a typo that happened to be quiet.
            const auto warningFor = [&bank](const juce::String& second)
            {
                backend::UtauRenderRequest request;
                request.voicebankDirectory = bank;
                request.targetDurationSeconds = 2.5;
                for (const auto& pair : { std::make_pair(juce::String("a"), 0.5),
                                          std::make_pair(second, 1.5) })
                {
                    backend::UtauNoteRenderSpec spec;
                    spec.alias = pair.first;
                    spec.startSeconds = pair.second;
                    spec.durationSeconds = 0.5;
                    spec.midiNote = 62.0f;
                    request.notes.push_back(std::move(spec));
                }
                return backend::UtauRenderer::render(request).warning;
            };
            const auto missing = juce::String("were not found");
            const auto restWarning = warningFor("RR");
            const auto unknownWarning = warningFor("ZZZZ");
            const auto notReportedMissing = !restWarning.contains(missing)
                // and a real typo still is, or the check would pass on a
                // build that had stopped reporting anything at all
                && unknownWarning.contains(missing);

            std::cout << "reads_the_word=" << (readsTheWord ? 1 : 0)
                      << "|a_rest_is_not_a_missing_alias="
                      << (notReportedMissing ? 1 : 0)
                      << "|warning_rr=[" << restWarning
                      << "] warning_zzzz=[" << unknownWarning << "]"
                      << "|the_rest_is_silent=" << (restIsSilent ? 1 : 0)
                      << "|it_takes_nothing_from_the_note_before="
                      << (beforeRunsOn ? 1 : 0)
                      << "|the_note_after_keeps_its_lead_in="
                      << (afterKeepsItsLeadIn ? 1 : 0)
                      << "|it_is_drawn_as_a_rest=" << (looksDifferent ? 1 : 0)
                      << "|pixels=" << differing
                      << "|before " << juce::String(beforeEndSung, 4) << "->"
                      << juce::String(spans[before].soundingEnd, 4)
                      << " own_end " << juce::String(notesOf()[before].startSeconds
                          + notesOf()[before].durationSeconds, 4)
                      << " rest " << juce::String(spans[rest].soundingStart, 4)
                      << ".." << juce::String(spans[rest].soundingEnd, 4)
                      << std::endl;
            setApplicationReturnValue(readsTheWord && restIsSilent && beforeRunsOn
                                      && afterKeepsItsLeadIn && looksDifferent
                                      && notReportedMissing ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-flag-point-value")
        {
            // Right-clicking a handle offers to type its value, and what the
            // box opens with has to be the number that handle is carrying --
            // the one drawn in the lane.
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();
            roll.setTool(PianoRollComponent::Tool::flagCurve);

            const auto notesOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            constexpr std::size_t probe = 3;
            const auto noteId = notesOf()[probe].id;
            project.setNotesUtauFlagCurveEnabled({ noteId }, true);
            roll.diagnosticRefresh();

            // A note with nothing stored still shows a handle -- the one the
            // lane starts every flag from -- and the menu offers to type its
            // value.  The box has to open on that handle's number.
            const auto shownFresh = roll.flagLaneCurveFor(notesOf()[probe]);
            const auto freshText = roll.diagnosticFlagPointValueText(noteId, 0);
            const auto freshOpens = !shownFresh.empty() && freshText.isNotEmpty()
                && std::abs(freshText.getFloatValue() - shownFresh.front().value) < 0.001f;

            // And with a curve stored, every handle reads back its own value
            // rather than a neighbour's.
            project.setNoteUtauFlagCurve(noteId, "g",
                { { 0.05, -12.5 }, { 0.20, 31.25 }, { 0.40, 4.0 } });
            roll.diagnosticRefresh();
            const auto shown = roll.flagLaneCurveFor(notesOf()[probe]);
            auto everyHandleReadsItself = shown.size() == 3;
            juce::String report;
            for (int index = 0; index < static_cast<int>(shown.size()); ++index)
            {
                const auto text = roll.diagnosticFlagPointValueText(noteId, index);
                const auto want = shown[static_cast<std::size_t>(index)].value;
                everyHandleReadsItself = everyHandleReadsItself
                    && text.isNotEmpty()
                    && std::abs(text.getFloatValue() - want) < 0.001f;
                report += " " + juce::String(index) + ":[" + text + "]/"
                    + juce::String(want, 3);
            }

            // And it describes the flag the lane is on, not g.  Mt reaches
            // +/-100 where g reaches +/-50, so a value read against the wrong
            // range is exactly what looks wrong.
            roll.setFlagLaneFlag("Mt");
            roll.diagnosticRefresh();
            const auto blurb = roll.diagnosticFlagPointValueBlurb(noteId, 0);
            const auto namesTheFlag = blurb.startsWith("Mt")
                && blurb.contains("100") && !blurb.contains("50 ")
                && !blurb.contains(juce::String::fromUTF8("共振峰平移"));
            roll.setFlagLaneFlag("g");
            roll.diagnosticRefresh();
            const auto gBlurb = roll.diagnosticFlagPointValueBlurb(noteId, 0);
            const auto stillRightForG = gBlurb.startsWith("g")
                && gBlurb.contains(juce::String::fromUTF8("共振峰平移"));

            std::cout << "a_fresh_handle_opens_its_value=" << (freshOpens ? 1 : 0)
                      << "|it_describes_the_flag_on_show="
                      << (namesTheFlag && stillRightForG ? 1 : 0)
                      << "|mt_blurb=[" << blurb.removeCharacters(juce::newLine) << "]"
                      << "|every_handle_reads_itself="
                      << (everyHandleReadsItself ? 1 : 0)
                      << "|fresh=[" << freshText << "]/"
                      << (shownFresh.empty() ? juce::String("-")
                                             : juce::String(shownFresh.front().value, 3))
                      << report << std::endl;
            setApplicationReturnValue(freshOpens && everyHandleReadsItself
                                      && namesTheFlag && stillRightForG ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-flag-overlap")
        {
            // Two notes overlap wherever one's lead-in reaches back into the
            // one before it, and in that stretch the lane draws two curves.
            // A click there used to go to whichever note came first, so the
            // one in front could not be given a handle in the overlap at all.
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            // A real voicebank, so the lead-in is a real one: the stretch two
            // notes share is as wide as the oto says, rather than the sliver a
            // bankless project leaves -- and a sliver is narrower than the
            // radius a handle answers a click in, so there would be nowhere in
            // it to click that was not already grabbing something.
            project.setTrackVoicebankDirectory(trackId,
                                               juce::File(arguments[2].unquoted()));
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();
            roll.setTool(PianoRollComponent::Tool::flagCurve);

            const auto notesOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            // Two neighbours, each with its own level so the two curves sit at
            // different heights -- which is what tells them apart.
            constexpr std::size_t left = 2, right = 3;
            const auto leftId = notesOf()[left].id;
            const auto rightId = notesOf()[right].id;
            project.setNoteLabel(leftId, "a");
            project.setNoteLabel(rightId, "a");
            // The stretch two notes share is as wide as the following note's
            // overlap, and this bank's is four milliseconds -- narrower than
            // the radius a handle answers a click in, so there would be
            // nowhere in it to click that was not already grabbing one.  Pin a
            // wide one, which is what a note in a real phrase tends to have.
            project.setNoteUtauTimingOverrides(rightId, true, 0.30, 0.25);
            project.setNotesUtauFlagCurveEnabled({ leftId, rightId }, true);
            project.setNoteUtauFlagCurve(leftId, "g", { { 0.0, -20.0 } });
            project.setNoteUtauFlagCurve(rightId, "g", { { 0.0, 30.0 } });
            roll.diagnosticRefresh();

            // Where the two spans really overlap, measured rather than assumed.
            const auto leftNote = notesOf()[left];
            const auto rightNote = notesOf()[right];
            const auto leftSpan = roll.flagLaneSpanFor(leftNote);
            const auto rightSpan = roll.flagLaneSpanFor(rightNote);
            const auto overlapFrom = std::max(leftNote.startSeconds + leftSpan.first,
                                              rightNote.startSeconds + rightSpan.first);
            const auto overlapTo = std::min(leftNote.startSeconds + leftSpan.second,
                                            rightNote.startSeconds + rightSpan.second);
            // Wide enough that its middle is clear of both notes' handles,
            // which answer a click within eleven pixels of themselves.
            const auto reallyOverlaps = overlapTo > overlapFrom + 0.12;
            const auto at = (overlapFrom + overlapTo) / 2.0;

            const auto countOf = [&](const juce::String& id)
            {
                for (const auto& note : notesOf())
                    if (note.id == id)
                        for (const auto& [flag, points] : note.utauFlagCurves)
                            if (flag == "g") return static_cast<int>(points.size());
                return 0;
            };
            const auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto press = [&](juce::Point<float> where)
            {
                return juce::MouseEvent(source, where,
                    juce::ModifierKeys::leftButtonModifier,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false);
            };
            const auto clickAt = [&](double value)
            {
                const juce::Point<float> where(roll.diagnosticEdgeX(at),
                                               roll.flagLaneY(static_cast<float>(value)));
                roll.mouseDown(press(where));
                roll.mouseUp(press(where));
                roll.diagnosticRefresh();
            };

            const auto leftBefore = countOf(leftId), rightBefore = countOf(rightId);
            clickAt(30.0);                       // aimed at the note in front
            const auto rightGrew = countOf(rightId) == rightBefore + 1
                && countOf(leftId) == leftBefore;
            const auto afterFirst = countOf(rightId);
            clickAt(-20.0);                      // aimed at the one behind
            const auto leftGrew = countOf(leftId) == leftBefore + 1
                && countOf(rightId) == afterFirst;

            std::cout << "the_spans_really_overlap=" << (reallyOverlaps ? 1 : 0)
                      << "|aiming_high_edits_the_note_in_front=" << (rightGrew ? 1 : 0)
                      << "|aiming_low_edits_the_one_behind=" << (leftGrew ? 1 : 0)
                      << "|overlap " << juce::String(overlapFrom, 4) << ".."
                      << juce::String(overlapTo, 4) << " at " << juce::String(at, 4)
                      << " counts " << countOf(leftId) << "/" << countOf(rightId)
                      << std::endl;
            setApplicationReturnValue(reallyOverlaps && rightGrew && leftGrew
                                      ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-load-timing")
        {
            // Where the time goes when a project is opened, and a guard on
            // the one phase that was pathological.  read_bytes_ms is the same
            // parse read straight off the file, kept as the control: it is
            // measured on this machine in this run, so the check is a ratio
            // and does not care how fast the machine is.
            const auto ms = [](auto&& work)
            {
                const auto started = juce::Time::getMillisecondCounterHiRes();
                work();
                return juce::Time::getMillisecondCounterHiRes() - started;
            };
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            auto loaded = false;
            // The two halves of the load, separately: reading the bytes, then
            // turning them into a project.
            const juce::File projectFile(arguments[1].unquoted());
            auto readMs = 0.0;
            {
                juce::ValueTree tree;
                if (auto stream = projectFile.createInputStream())
                    readMs = ms([&] { tree = juce::ValueTree::readFromStream(*stream); });
            }
            // The same parse, from memory instead of straight off the file.
            auto memoryMs = 0.0;
            auto slurpMs = 0.0;
            {
                juce::MemoryBlock bytes;
                slurpMs = ms([&] { projectFile.loadFileAsData(bytes); });
                juce::ValueTree tree;
                memoryMs = ms([&]
                {
                    juce::MemoryInputStream stream(bytes, false);
                    tree = juce::ValueTree::readFromStream(stream);
                });
            }
            const auto loadMs = ms([&]
            {
                loaded = project.load(projectFile, loadError);
            });
            if (!loaded)
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            auto notes = 0, tracks = 0;
            juce::StringArray banks;
            for (const auto& track : project.snapshot().tracks)
            {
                ++tracks;
                if (track.voicebankDirectory.isDirectory())
                    banks.addIfNotAlreadyThere(
                        track.voicebankDirectory.getFullPathName());
                for (const auto& clip : track.clips) notes += clip.notes.size();
            }

            std::unique_ptr<PianoRollComponent> roll;
            const auto buildMs = ms([&]
            {
                roll = std::make_unique<PianoRollComponent>(project, strings);
                roll->setBounds(0, 0, 1600, 900);
                roll->resized();
            });
            // The first refresh pays for reading every voicebank the project
            // uses; the second is what an ordinary edit costs afterwards.
            const auto firstRefreshMs = ms([&] { roll->diagnosticRefresh(); });
            const auto secondRefreshMs = ms([&] { roll->diagnosticRefresh(); });
            const auto paintMs = ms([&]
            {
                juce::Image image(juce::Image::ARGB, 1600, 900, true);
                juce::Graphics g(image);
                roll->paintEntireComponent(g, true);
            });
            const auto scratch = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("hachi-load-timing.hjpx");
            scratch.deleteFile();
            juce::String saveError;
            const auto saveMs = ms([&] { project.save(scratch, saveError); });
            scratch.deleteFile();
            AudioEngine engine;
            const auto syncMs = ms([&] { engine.syncProject(project.snapshot()); });
            const auto resyncMs = ms([&] { engine.syncProject(project.snapshot()); });

            // Opening a project must not cost what reading it a field at a
            // time costs.  A quarter of that is a wide margin -- it was above
            // it, at 2610 ms against 2833 -- and well clear of the 44 ms the
            // whole load takes once the bytes are read in one go.
            const auto loadIsNotByField = loadMs < readMs / 4.0;
            std::cout << "notes=" << notes << "|tracks=" << tracks
                      << "|voicebanks=" << banks.size()
                      << "|load_is_not_read_field_by_field="
                      << (loadIsNotByField ? 1 : 0)
                      << "|read_bytes_ms=" << juce::String(readMs, 1)
                      << "|slurp_ms=" << juce::String(slurpMs, 1)
                      << "|parse_from_memory_ms=" << juce::String(memoryMs, 1)
                      << "|load_ms=" << juce::String(loadMs, 1)
                      << "|save_ms=" << juce::String(saveMs, 1)
                      << "|build_roll_ms=" << juce::String(buildMs, 1)
                      << "|first_refresh_ms=" << juce::String(firstRefreshMs, 1)
                      << "|second_refresh_ms=" << juce::String(secondRefreshMs, 1)
                      << "|paint_ms=" << juce::String(paintMs, 1)
                      << "|sync_ms=" << juce::String(syncMs, 1)
                      << "|resync_ms=" << juce::String(resyncMs, 1)
                      << std::endl;
            setApplicationReturnValue(loadIsNotByField ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-flatten-pitch-line")
        {
            // 初始化音高线: the pitch line laid flat on the note's own pitch,
            // with nothing left but an anchor at each end.  Off a UTAU track
            // that line is what sounds -- setNotePitchCurve writes the curve
            // into the contour as manual targets in cents away from the
            // note's pitch -- so flat has to mean zero there too, not merely
            // a straight line on screen.
            I18n strings;

            // It belongs to the plain menu and only that one.  Vibrato is also
            // shared there now: it is a target-pitch layer, not a voicebank-only
            // command.
            const auto utau = PianoRollComponent::noteMenuItemsFor(true);
            const auto plain = PianoRollComponent::noteMenuItemsFor(false);
            const auto has = [](const std::vector<int>& items, int id)
            {
                return std::find(items.begin(), items.end(), id) != items.end();
            };
            const auto inThePlainMenu = has(plain, 18);
            const auto notInTheUtauMenu = !has(utau, 18);
            // And the split of everything else is undisturbed.  The shared
            // forced-connection (20) and envelope-base (23) items are on both,
            // so each menu is two longer than the original UTAU-only split.
            // Pinyin lead-in (24) is UTAU-only, so it lifts the UTAU count too.
            const auto plainStillShort = plain.size() == 12;
            const auto utauStillWhole = utau.size() == 21;

            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-flat-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=f\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            const auto clipId = project.snapshot().tracks.front().clips.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);
            const auto noteId = project.snapshot().tracks.front()
                .clips.front().notes.front().id;

            // A line that goes somewhere, so flattening it is unmistakable.
            std::vector<PitchCurveEditPoint> bent;
            bent.push_back({ 0.0, 60.0f });
            bent.push_back({ 0.4, 67.0f });
            bent.push_back({ 0.8, 55.0f });
            bent.push_back({ 1.0, 60.0f });
            const auto bentSet = project.setNotePitchCurve(noteId, bent, true);
            const auto noteNow = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes.front();
            };
            const auto before = noteNow();
            const auto startedBent = bentSet && before.pitchControlPoints.size() == 4;
            // The contour carries it too, which is what actually sounds.
            auto worstBefore = 0.0f;
            for (const auto& point : before.contour)
                if (point.hasManualTarget)
                    worstBefore = std::max(worstBefore, std::abs(point.manualTargetCents));
            const auto contourWasBent = worstBefore > 100.0f;

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            roll.setFocusedClip(clipId);
            roll.diagnosticRefresh();
            roll.flattenPitchLine(noteId);

            const auto after = noteNow();
            const auto twoAnchorsLeft = after.pitchControlPoints.size() == 2;
            const auto bothOnTheNote = twoAnchorsLeft
                && std::abs(after.pitchControlPoints.front().targetMidi - after.midiNote) < 1.0e-4f
                && std::abs(after.pitchControlPoints.back().targetMidi - after.midiNote) < 1.0e-4f;
            const auto spansTheNote = twoAnchorsLeft
                && std::abs(after.pitchControlPoints.front().timeSeconds) < 1.0e-9
                && std::abs(after.pitchControlPoints.back().timeSeconds
                            - after.durationSeconds) < 1.0e-9;
            // And the line that sounds is flat: zero cents from the note's own
            // pitch at every frame that carries a target.
            auto worstAfter = 0.0f;
            auto targeted = 0;
            for (const auto& point : after.contour)
                if (point.hasManualTarget)
                {
                    ++targeted;
                    worstAfter = std::max(worstAfter, std::abs(point.manualTargetCents));
                }
            const auto contourIsFlat = targeted > 0 && worstAfter < 1.0e-3f;
            // The note itself did not move.
            const auto noteKeptItsPitch = std::abs(after.midiNote - before.midiNote) < 1.0e-6f
                && std::abs(after.durationSeconds - before.durationSeconds) < 1.0e-9;
            // One step back, since it is one action.
            project.undo();
            const auto undone = noteNow();
            const auto undoesInOneStep = undone.pitchControlPoints.size() == 4;

            const auto ok = inThePlainMenu && notInTheUtauMenu && plainStillShort
                && utauStillWhole && startedBent && contourWasBent && twoAnchorsLeft
                && bothOnTheNote && spansTheNote && contourIsFlat && noteKeptItsPitch
                && undoesInOneStep;
            std::cout << "in_the_plain_menu=" << (inThePlainMenu ? 1 : 0)
                      << "|not_in_the_utau_menu=" << (notInTheUtauMenu ? 1 : 0)
                      << "|plain_menu_still_short=" << (plainStillShort ? 1 : 0)
                      << "|utau_menu_still_whole=" << (utauStillWhole ? 1 : 0)
                      << "|started_bent=" << (startedBent ? 1 : 0)
                      << "|contour_was_bent=" << (contourWasBent ? 1 : 0)
                      << "|two_anchors_left=" << (twoAnchorsLeft ? 1 : 0)
                      << "|both_on_the_notes_pitch=" << (bothOnTheNote ? 1 : 0)
                      << "|spans_the_note=" << (spansTheNote ? 1 : 0)
                      << "|contour_is_flat=" << (contourIsFlat ? 1 : 0)
                      << "|note_kept_its_pitch=" << (noteKeptItsPitch ? 1 : 0)
                      << "|undoes_in_one_step=" << (undoesInOneStep ? 1 : 0)
                      << "|cents=" << worstBefore << "->" << worstAfter
                      << "|targeted=" << targeted
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-note-across-tracks")
        {
            // Copy a note, switch to another track, point at a spot in the
            // roll, paste.  Every piece of that chain, since the failure was
            // at the end of it: a track made a moment ago has no clip, every
            // note-making path needs one, and the paste returned silently.
            I18n strings;
            juce::String error;
            juce::StringArray warnings;
            const auto phraseFile = []
            {
                const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("hachi-na-" + juce::Uuid().toDashedString() + ".ust");
                ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                    "Tempo=120.00\r\nTracks=1\r\nProjectName=n\r\n"
                                    "[#0000]\r\nLength=480\r\nLyric=a\r\nNoteNum=60\r\n"
                                    "[#0001]\r\nLength=480\r\nLyric=i\r\nNoteNum=62\r\n"
                                    "[#TRACKEND]\r\n");
                return ust;
            };

            // Two tracks that both carry material.
            ProjectModel two;
            const auto firstUst = phraseFile();
            two.addUstFile(firstUst, error, warnings);
            firstUst.deleteFile();
            const auto secondUst = phraseFile();
            two.addUstFile(secondUst, error, warnings);
            secondUst.deleteFile();
            const auto twoData = two.snapshot();
            const auto fixtureReaches = twoData.tracks.size() == 2
                && !twoData.tracks[0].clips.empty() && !twoData.tracks[1].clips.empty();
            const auto trackB = fixtureReaches ? twoData.tracks[1].id : juce::String{};

            // Copied from track A, so sameTrack is false: the pointer decides
            // where it goes, on either track.
            const auto pointed = MainComponent::pasteTargetSeconds(
                false, 0.0, 0.0, std::nullopt, std::nullopt, 0.7);
            const auto followsThePointer = std::abs(pointed - 0.7) < 1.0e-9;
            const auto clipUnderIt =
                MainComponent::pasteTargetClipIn(twoData, trackB, 0.7);
            const auto findsTheClip = clipUnderIt.isNotEmpty();
            // Past the end of that track's material it still finds the clip to
            // grow, rather than giving up.
            const auto clipBeyond =
                MainComponent::pasteTargetClipIn(twoData, trackB, 3.0);
            const auto findsItBeyond = clipBeyond.isNotEmpty();

            // And the notes actually land on the other track.
            //
            // Two cases, because a paste onto a UTAU track goes in rather than
            // over: in free space it lands exactly where the pointer said, and
            // where it would land inside a note it enters at that note's
            // start and pushes it along.  Asserting the pointer position in
            // both was wrong -- the fixture's track is solid from 0 to 1, so
            // 0.7 correctly came out at 0.5.
            const auto startOfInserted = [](ProjectModel& model,
                                            const juce::String& trackId,
                                            const juce::String& noteId)
            {
                for (const auto& track : model.snapshot().tracks)
                    if (track.id == trackId)
                        for (const auto& clip : track.clips)
                            for (const auto& note : clip.notes)
                                if (note.id == noteId) return note.startSeconds;
                return -1.0;
            };
            auto landed = false;
            auto goesInNotOver = false;
            juce::String landingDetail;
            if (findsTheClip)
            {
                auto phrase = twoData.tracks[0].clips.front().notes;
                for (auto& note : phrase) note.startSeconds = 0.0;

                // Free space past the end of that track's material.
                const auto free = two.insertNotes(clipUnderIt, { phrase.front() }, 1.5);
                const auto freeStart = free.size() == 1
                    ? startOfInserted(two, trackB, free.front()) : -1.0;
                landed = free.size() == 1 && std::abs(freeStart - 1.5) < 1.0e-6;

                // Inside the note running from 0.5: it goes in there.
                const auto inside = two.insertNotes(clipUnderIt, { phrase.front() }, 0.7);
                const auto insideStart = inside.size() == 1
                    ? startOfInserted(two, trackB, inside.front()) : -1.0;
                goesInNotOver = inside.size() == 1
                    && std::abs(insideStart - 0.5) < 1.0e-6;
                landingDetail = "free=" + juce::String(freeStart, 4)
                    + " inside=" + juce::String(insideStart, 4);
            }

            // The track made a moment ago: no clip, and before the fix the
            // paste stopped here without a word.
            ProjectModel fresh;
            const auto freshUst = phraseFile();
            fresh.addUstFile(freshUst, error, warnings);
            freshUst.deleteFile();
            const auto freshTrack = fresh.addTrack("empty", true);
            const auto freshData = fresh.snapshot();
            const auto nothingToFind =
                MainComponent::pasteTargetClipIn(freshData, freshTrack, 1.0).isEmpty();
            // What the window decides about it, asked of the window's own rule.
            const auto plan = MainComponent::pasteClipPlanFor(freshData, freshTrack, 1.0);
            const auto planSaysMakeOne = plan.clipId.isEmpty() && plan.makeOne
                && plan.seedSeconds > 0.05
                && std::abs(plan.startSeconds) < 1.0e-9;
            const auto madeClip = plan.makeOne
                ? fresh.addClip(freshTrack, plan.startSeconds, plan.seedSeconds)
                : juce::String{};
            auto landedOnTheFreshTrack = false;
            if (madeClip.isNotEmpty())
            {
                auto phrase = freshData.tracks[0].clips.front().notes;
                for (auto& note : phrase) note.startSeconds = 0.0;
                landedOnTheFreshTrack =
                    fresh.insertNotes(madeClip, { phrase.front() }, 1.0).size() == 1;
            }
            const auto clipStartsAtZero = madeClip.isNotEmpty()
                && [&]
                {
                    for (const auto& track : fresh.snapshot().tracks)
                        if (track.id == freshTrack && !track.clips.empty())
                            return std::abs(track.clips.front().startSeconds) < 1.0e-9;
                    return false;
                }();

            // An audio track has nothing to sing notes through, so no clip is
            // made there and the window says so instead of going quiet.
            ProjectModel audio;
            const auto audioTrack = audio.addTrack("audio", false);
            const auto audioPlan = MainComponent::pasteClipPlanFor(
                audio.snapshot(), audioTrack, 1.0);
            const auto audioRefused = audioPlan.clipId.isEmpty() && !audioPlan.makeOne
                && audio.addClip(audioTrack, 0.0, 1.0).isEmpty();

            const auto ok = fixtureReaches && followsThePointer && findsTheClip
                && findsItBeyond && landed && goesInNotOver && nothingToFind
                && planSaysMakeOne && landedOnTheFreshTrack && clipStartsAtZero
                && audioRefused;
            std::cout << "fixture_reaches=" << (fixtureReaches ? 1 : 0)
                      << "|follows_the_pointer=" << (followsThePointer ? 1 : 0)
                      << "|finds_the_clip_there=" << (findsTheClip ? 1 : 0)
                      << "|finds_it_past_the_end=" << (findsItBeyond ? 1 : 0)
                      << "|notes_land_on_the_other_track=" << (landed ? 1 : 0)
                      << "|goes_in_not_over=" << (goesInNotOver ? 1 : 0)
                      << "|fresh_track_has_no_clip=" << (nothingToFind ? 1 : 0)
                      << "|plan_says_make_one=" << (planSaysMakeOne ? 1 : 0)
                      << "|paste_makes_one=" << (landedOnTheFreshTrack ? 1 : 0)
                      << "|and_it_starts_at_zero=" << (clipStartsAtZero ? 1 : 0)
                      << "|where=" << landingDetail
                      << "|audio_track_refused=" << (audioRefused ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-clip-across-tracks")
        {
            // Copying a piece of material onto another track.  The model could
            // always do it; what was missing was any way to say which track.
            // Clicking an empty lane reports "no clip" and does not name the
            // lane, so a paste could only ever go to the track already in
            // hand -- and copying sets that to the track the material came
            // from, so it landed back beside the original.
            I18n strings;

            // The rule.  The pointer names a track and a moment; without it,
            // the track in hand and the playhead, and not before the original.
            using Target = MainComponent::ClipPasteTarget;
            const auto pointed = MainComponent::clipPasteTargetFor(
                "track-b", 4.5, "track-a", 0.0, 1.0, 2.0);
            const auto followsThePointer = pointed.trackId == "track-b"
                && std::abs(pointed.seconds - 4.5) < 1.0e-9;
            // Half a pointer is not a pointer: a lane with no moment falls
            // back, or a paste would land at zero on the right track.
            const auto halfPointer = MainComponent::clipPasteTargetFor(
                "track-b", std::nullopt, "track-a", 9.0, 1.0, 2.0);
            const auto needsBoth = halfPointer.trackId == "track-a"
                && std::abs(halfPointer.seconds - 9.0) < 1.0e-9;
            // No pointer, playhead past the original: it goes to the playhead.
            const auto ahead = MainComponent::clipPasteTargetFor(
                {}, std::nullopt, "track-a", 9.0, 1.0, 2.0);
            const auto usesThePlayhead = ahead.trackId == "track-a"
                && std::abs(ahead.seconds - 9.0) < 1.0e-9;
            // No pointer, playhead not past it: right after the original, so
            // repeated pastes lay copies end to end.
            const auto behind = MainComponent::clipPasteTargetFor(
                {}, std::nullopt, "track-a", 0.5, 1.0, 2.0);
            const auto stacksEndToEnd = std::abs(behind.seconds - 3.0) < 1.0e-9;

            // The timeline has to report the lane, or the rule has nothing to
            // act on.  Its lanes are stacked below the ruler, one row each.
            const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-across-" + juce::Uuid().toDashedString());
            folder.createDirectory();
            const auto wav = folder.getChildFile("tone.wav");
            {
                juce::AudioBuffer<float> buffer(1, 44100);
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    buffer.setSample(0, index,
                        0.3f * std::sin(static_cast<float>(index) * 0.05f));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), 44100.0, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }

            ProjectModel project;
            const auto sourceClip = project.addAudioFile(wav, 1.0, 0.0, {});
            const auto sourceTrack = project.snapshot().tracks.front().id;
            const auto melodic = project.addTrack("melodic", true);
            const auto material = project.addTrack("material", false, true);

            TimelineComponent timeline(project);
            timeline.setBounds(0, 0, 1200, 400);
            timeline.setPixelsPerSecond(100.0f);
            timeline.diagnosticRefresh();
            const auto quietAtFirst = !timeline.pointerAnchor().has_value();
            const auto moveTo = [&timeline](int y, double seconds)
            {
                const juce::Point<float> where(
                    static_cast<float>(seconds * 100.0), static_cast<float>(y));
                timeline.mouseMove(juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &timeline, &timeline, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false));
            };
            // Each lane names a different track, or the pointer could not
            // distinguish them.
            std::vector<juce::String> lanes;
            for (int lane = 0; lane < 3; ++lane)
            {
                moveTo(timeline.diagnosticRulerHeight()
                           + lane * timeline.diagnosticRowHeight() + 4, 3.0);
                const auto anchor = timeline.pointerAnchor();
                lanes.push_back(anchor ? anchor->trackId : juce::String{});
            }
            const auto lanesNameTheirTracks = lanes.size() == 3
                && lanes[0] == sourceTrack && lanes[1] == melodic
                && lanes[2] == material;
            const auto anchorCarriesTime = [&]
            {
                moveTo(timeline.diagnosticRulerHeight()
                           + timeline.diagnosticRowHeight() + 4, 3.0);
                const auto anchor = timeline.pointerAnchor();
                return anchor && std::abs(anchor->seconds - 3.0) < 1.0e-6;
            }();

            // And it is forgotten on the way out, or a paste would land on
            // whichever lane the pointer happened to leave from.
            timeline.mouseExit(juce::MouseEvent(
                juce::Desktop::getInstance().getMainMouseSource(), { 0.0f, 0.0f },
                juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                &timeline, &timeline, juce::Time::getCurrentTime(), { 0.0f, 0.0f },
                juce::Time::getCurrentTime(), 1, false));
            const auto forgottenOnLeaving = !timeline.pointerAnchor().has_value();

            // And the model puts it where the rule says, in every direction.
            const auto clipsOn = [&project](const juce::String& trackId)
            {
                for (const auto& track : project.snapshot().tracks)
                    if (track.id == trackId) return track.clips.size();
                return std::size_t{};
            };
            const auto ontoMelodic = project.duplicateClip(sourceClip, 2.0, melodic);
            const auto ontoMaterial = project.duplicateClip(sourceClip, 2.0, material);
            const auto backAgain = ontoMaterial.isNotEmpty()
                ? project.duplicateClip(ontoMaterial, 4.0, sourceTrack) : juce::String{};
            const auto travelsBothWays = ontoMelodic.isNotEmpty()
                && ontoMaterial.isNotEmpty() && backAgain.isNotEmpty()
                && clipsOn(melodic) == 1 && clipsOn(material) == 1
                && clipsOn(sourceTrack) == 2;
            // And it lands where it was told, not beside the original.
            auto landedAtTwo = false;
            for (const auto& track : project.snapshot().tracks)
                if (track.id == melodic)
                    for (const auto& clip : track.clips)
                        if (std::abs(clip.startSeconds - 2.0) < 1.0e-9) landedAtTwo = true;
            folder.deleteRecursively();

            const auto ok = followsThePointer && needsBoth && usesThePlayhead
                && stacksEndToEnd && quietAtFirst && lanesNameTheirTracks
                && anchorCarriesTime && forgottenOnLeaving && travelsBothWays
                && landedAtTwo;
            std::cout << "follows_the_pointer=" << (followsThePointer ? 1 : 0)
                      << "|needs_a_lane_and_a_moment=" << (needsBoth ? 1 : 0)
                      << "|uses_the_playhead=" << (usesThePlayhead ? 1 : 0)
                      << "|stacks_end_to_end=" << (stacksEndToEnd ? 1 : 0)
                      << "|quiet_before_the_pointer=" << (quietAtFirst ? 1 : 0)
                      << "|lanes_name_their_tracks=" << (lanesNameTheirTracks ? 1 : 0)
                      << "|anchor_carries_the_time=" << (anchorCarriesTime ? 1 : 0)
                      << "|forgotten_on_leaving=" << (forgottenOnLeaving ? 1 : 0)
                      << "|travels_both_ways=" << (travelsBothWays ? 1 : 0)
                      << "|landed_where_told=" << (landedAtTwo ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-reference-track")
        {
            // A material track: something to work against rather than part of
            // the piece.  It is heard while it is the track in hand and silent
            // whenever anything else is playing.
            using Engine = AudioEngine;

            // The rule on its own, including what it must leave alone.
            const auto ordinaryUnchanged =
                Engine::trackIsAudible(false, false, false, false, false)
                && !Engine::trackIsAudible(true, false, false, false, false)
                && !Engine::trackIsAudible(false, false, true, false, false)
                && Engine::trackIsAudible(false, true, true, false, false);
            const auto materialSilentElsewhere =
                !Engine::trackIsAudible(false, false, false, true, false);
            const auto materialHeardInHand =
                Engine::trackIsAudible(false, false, false, true, true);
            // Muting still wins: a material track muted by hand stays muted
            // even while it is the one being worked on.
            const auto muteStillWins =
                !Engine::trackIsAudible(true, false, false, true, true);
            // And solo still wins, or soloing something else would let the
            // material back in.
            const auto soloStillWins =
                !Engine::trackIsAudible(false, false, true, true, true);

            // And the mix, rendered.  Two tracks of audio, one of them
            // material: with the other in hand only one should be heard.
            const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-ref-" + juce::Uuid().toDashedString());
            folder.createDirectory();
            constexpr auto rate = 44100.0;
            const auto tone = [&folder](const char* name, double hertz)
            {
                const auto file = folder.getChildFile(name);
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate));
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    buffer.setSample(0, index, static_cast<float>(0.3
                        * std::sin(2.0 * juce::MathConstants<double>::pi * hertz
                                   * static_cast<double>(index) / rate)));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
                return file;
            };
            const auto workWav = tone("work.wav", 220.0);
            const auto materialWav = tone("material.wav", 660.0);

            ProjectModel project;
            const auto workClip = project.addAudioFile(workWav, 1.0, 0.0, {});
            const auto workTrack = project.snapshot().tracks.front().id;
            const auto materialTrack = project.addTrack("material", false, true);
            const auto materialClip =
                project.addAudioFile(materialWav, 1.0, 0.0, materialTrack);
            const auto fixtureReaches = workClip.isNotEmpty()
                && materialClip.isNotEmpty()
                && project.snapshot().tracks.size() == 2
                && project.snapshot().tracks.back().referenceOnly;

            const auto loudnessWith = [](const ProjectData& data,
                                         const juce::String& inHand)
            {
                AudioEngine engine;
                engine.setAuditionTrack(inHand);
                engine.syncProject(data);
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10'000.0;
                while (juce::Time::getMillisecondCounterHiRes() < deadline)
                {
                    if (!engine.renderProgress()) break;
                    juce::Thread::sleep(10);
                }
                auto output = juce::File::createTempFile("hachi-ref-out.wav");
                juce::String error;
                auto rms = -1.0;
                if (engine.exportWav(output, error))
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    if (auto reader = std::unique_ptr<juce::AudioFormatReader>(
                            formats.createReaderFor(output)))
                    {
                        juce::AudioBuffer<float> buffer(
                            static_cast<int>(reader->numChannels),
                            static_cast<int>(reader->lengthInSamples));
                        reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
                        auto squareSum = 0.0;
                        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                            {
                                const auto value = buffer.getSample(channel, sample);
                                squareSum += static_cast<double>(value) * value;
                            }
                        rms = std::sqrt(squareSum / static_cast<double>(
                            std::max(1, buffer.getNumChannels() * buffer.getNumSamples())));
                    }
                }
                output.deleteFile();
                return rms;
            };

            const auto data = project.snapshot();
            const auto workingOnTheWork = loudnessWith(data, workTrack);
            const auto workingOnTheMaterial = loudnessWith(data, materialTrack);

            // For scale: the same project with the material track deleted, so
            // "the material is not in there" is measured rather than assumed.
            ProjectModel alone;
            const auto aloneClip = alone.addAudioFile(workWav, 1.0, 0.0, {});
            juce::ignoreUnused(aloneClip);
            const auto workAlone = loudnessWith(alone.snapshot(), {});
            folder.deleteRecursively();

            const auto everythingSounded = workingOnTheWork > 1.0e-4
                && workingOnTheMaterial > 1.0e-4 && workAlone > 1.0e-4;
            // With the work in hand the mix is the work alone: the material is
            // not being added to it.
            const auto materialKeptOut = everythingSounded
                && std::abs(workingOnTheWork - workAlone) < 1.0e-6;
            // With the material in hand it is heard, so the mix is not that.
            const auto materialAudibleInHand = everythingSounded
                && std::abs(workingOnTheMaterial - workAlone) > 1.0e-4;

            const auto ok = ordinaryUnchanged && materialSilentElsewhere
                && materialHeardInHand && muteStillWins && soloStillWins
                && fixtureReaches && everythingSounded && materialKeptOut
                && materialAudibleInHand;
            std::cout << "ordinary_tracks_unchanged=" << (ordinaryUnchanged ? 1 : 0)
                      << "|material_silent_elsewhere=" << (materialSilentElsewhere ? 1 : 0)
                      << "|material_heard_in_hand=" << (materialHeardInHand ? 1 : 0)
                      << "|mute_still_wins=" << (muteStillWins ? 1 : 0)
                      << "|solo_still_wins=" << (soloStillWins ? 1 : 0)
                      << "|fixture_reaches=" << (fixtureReaches ? 1 : 0)
                      << "|everything_sounded=" << (everythingSounded ? 1 : 0)
                      << "|material_kept_out_of_the_mix=" << (materialKeptOut ? 1 : 0)
                      << "|material_audible_in_hand=" << (materialAudibleInHand ? 1 : 0)
                      << "|rms=" << workingOnTheWork << "/" << workingOnTheMaterial
                      << "/" << workAlone
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-native-pitch-seam")
        {
            // Two adjacent native (mld5) notes an octave apart: the per-frame
            // target MIDI must cross the boundary with a short smoothstep S
            // transition, not a one-frame jump, and must never fabricate pitch
            // over an unvoiced gap.  Tests makeRenderRequest's native timeline
            // pitch continuity without the ONNX model.
            ProjectModel project;
            const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-seam-" + juce::Uuid().toDashedString() + ".wav");
            constexpr auto rate = 44100.0;
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * 1.2));
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    buffer.setSample(0, i, static_cast<float>(0.3
                        * std::sin(2.0 * juce::MathConstants<double>::pi
                                   * 200.0 * i / rate)));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr) { stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()); }
            }
            const auto clipId = project.addAudioFile(wav, 1.0, 0.0, {});
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);
            // Two abutting notes: 0.0..0.5 at C4 (60), 0.5..1.0 at C5 (72).
            project.addNote(clipId, 0.0, 0.5, 60.0f);
            project.addNote(clipId, 0.5, 0.5, 72.0f);
            wav.deleteFile();

            const auto target = AudioEngine::diagnosticNativeTargetMidi(
                project.snapshot(), 0, 0);
            // Boundary frame index at 0.5s, 5ms frames.
            const auto boundary = static_cast<int>(std::lround(0.5 / 0.005));
            auto maxStep = 0.0f;
            auto voicedFrames = 0;
            for (std::size_t i = 1; i < target.size(); ++i)
                if (target[i] > 0.0f && target[i - 1] > 0.0f)
                {
                    maxStep = std::max(maxStep, std::abs(target[i] - target[i - 1]));
                    ++voicedFrames;
                }
            // Pitches present at both ends.
            const auto headVoiced = boundary > 10 && target[static_cast<std::size_t>(10)] > 55.0f
                && target[static_cast<std::size_t>(10)] < 65.0f;
            const auto tailIdx = std::min<int>(static_cast<int>(target.size()) - 5, boundary + 30);
            const auto tailVoiced = tailIdx > 0 && target[static_cast<std::size_t>(tailIdx)] > 67.0f;
            // The seam is smoothed: no single 5ms frame jumps the whole octave.
            // A hard step would be ~12 semitones in one frame; the S transition
            // spreads it over several frames so each step is well under that.
            const auto smoothed = maxStep < 4.0f && voicedFrames > 0;
            // Monotone rise across the boundary (60 -> 72), sampled a few frames
            // either side.
            const auto before = target[static_cast<std::size_t>(std::max(0, boundary - 6))];
            const auto after = target[static_cast<std::size_t>(std::min(
                static_cast<int>(target.size()) - 1, boundary + 6))];
            const auto risesAcross = after > before + 3.0f;

            const auto ok = headVoiced && tailVoiced && smoothed && risesAcross;
            std::cout << "head_voiced=" << (headVoiced ? 1 : 0)
                      << "|tail_voiced=" << (tailVoiced ? 1 : 0)
                      << "|seam_smoothed=" << (smoothed ? 1 : 0)
                      << "|rises_across=" << (risesAcross ? 1 : 0)
                      << "|max_frame_step_st=" << maxStep
                      << "|frames=" << target.size()
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-native-pitch-points")
        {
            // The pitch-point tool was UTAU-only: the button was hidden, the
            // anchors were not grabbable, and -- the part that matters -- the
            // native renderer never read note.pitchControlPoints at all.  Only
            // makeUtauRequest did.  So the points had to start meaning
            // something off a UTAU track before the tool could be offered
            // there, or it would have been one more control that does nothing.
            I18n strings;

            const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-pts-" + juce::Uuid().toDashedString() + ".wav");
            constexpr auto rate = 44100.0;
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * 1.2));
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                {
                    const auto phase = 2.0 * juce::MathConstants<double>::pi
                        * 220.0 * static_cast<double>(index) / rate;
                    buffer.setSample(0, index, static_cast<float>(0.3
                        * (std::sin(phase) + 0.5 * std::sin(2.0 * phase))));
                }
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }

            // The rendered samples, so the two runs can be compared directly.
            const auto renderWith = [&wav](bool withPoints) -> std::vector<float>
            {
                ProjectModel project;
                const auto clipId = project.addAudioFile(wav, 1.2, 0.0, {});
                const auto trackId = project.snapshot().tracks.front().id;
                project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);
                const auto noteId = project.addNote(clipId, 0.0, 1.0, 57.0f);
                if (noteId.isEmpty()) return std::vector<float>{};
                if (withPoints)
                {
                    // A rise of an octave across the note, which no contour of
                    // a steady tone would ever produce on its own.
                    std::vector<PitchCurveEditPoint> points;
                    points.push_back({ 0.0, 57.0f });
                    points.push_back({ 1.0, 69.0f });
                    if (!project.setNotePitchCurve(noteId, points, true))
                        return std::vector<float>{};
                }
                AudioEngine engine;
                engine.syncProject(project.snapshot());
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 20'000.0;
                while (juce::Time::getMillisecondCounterHiRes() < deadline)
                {
                    if (!engine.renderProgress()) break;
                    juce::Thread::sleep(10);
                }
                auto output = juce::File::createTempFile("hachi-pts-out.wav");
                juce::String error;
                std::vector<float> samples;
                if (engine.exportWav(output, error))
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    if (auto reader = std::unique_ptr<juce::AudioFormatReader>(
                            formats.createReaderFor(output)))
                    {
                        juce::AudioBuffer<float> buffer(
                            static_cast<int>(reader->numChannels),
                            static_cast<int>(reader->lengthInSamples));
                        reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
                        samples.reserve(static_cast<std::size_t>(buffer.getNumSamples()));
                        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                            samples.push_back(buffer.getSample(0, sample));
                    }
                }
                output.deleteFile();
                return samples;
            };

            const auto plain = renderWith(false);
            const auto pointed = renderWith(true);
            wav.deleteFile();

            const auto loudness = [](const std::vector<float>& samples)
            {
                auto squareSum = 0.0;
                for (const auto value : samples)
                    squareSum += static_cast<double>(value) * value;
                return std::sqrt(squareSum
                    / static_cast<double>(std::max<std::size_t>(1, samples.size())));
            };
            const auto plainRms = loudness(plain);
            const auto pointedRms = loudness(pointed);
            const auto bothRendered = plainRms > 1.0e-4 && pointedRms > 1.0e-4
                && plain.size() == pointed.size();

            // The heart of it: the points reach the audio.  Identical samples
            // would mean the renderer was handed them and did nothing, which
            // is exactly what it used to do.
            auto moved = 0;
            auto worst = 0.0f;
            if (bothRendered)
                for (std::size_t index = 0; index < plain.size(); ++index)
                {
                    const auto difference = std::abs(plain[index] - pointed[index]);
                    if (difference > 1.0e-4f) ++moved;
                    worst = std::max(worst, difference);
                }
            const auto pointsChangeTheAudio = bothRendered && moved > 0;
            // An octave of it: a handful of samples nudged would be a rounding
            // difference, not a note that was re-pitched.
            const auto changedThroughout = bothRendered
                && moved > static_cast<int>(plain.size() / 4);

            // And the tool can take hold of them, which is the other half:
            // the anchors used to be tested for on UTAU notes only, so on a
            // native one there was nothing to grab even with the button shown.
            ProjectModel project;
            const auto grabWav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-grab-" + juce::Uuid().toDashedString() + ".wav");
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * 1.2));
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    buffer.setSample(0, index, 0.3f
                        * std::sin(static_cast<float>(index) * 0.03f));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(grabWav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }
            const auto grabClip = project.addAudioFile(grabWav, 1.2, 0.0, {});
            const auto grabTrack = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(grabTrack, PitchAlgorithm::mld5);
            const auto grabNote = project.addNote(grabClip, 0.2, 0.6, 60.0f);

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(grabTrack);
            roll.setFocusedClip(grabClip);
            roll.setTool(PianoRollComponent::Tool::points);
            roll.diagnosticRefresh();
            const auto pressAt = [&roll](double seconds, float midi)
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(seconds) * 200.0f,
                    roll.diagnosticYForMidi(midi));
                roll.mouseDown(juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false));
            };
            // The note's first anchor sits at its start, at its own pitch.
            pressAt(0.2, 60.0f);
            const auto tookHold = roll.diagnosticDraggingAnchor();
            roll.mouseUp(juce::MouseEvent(
                juce::Desktop::getInstance().getMainMouseSource(), { 0.0f, 0.0f },
                juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &roll, &roll,
                juce::Time::getCurrentTime(), { 0.0f, 0.0f },
                juce::Time::getCurrentTime(), 1, false));
            // Far from any anchor it takes hold of nothing, or the check above
            // would pass wherever it clicked.
            roll.diagnosticRefresh();
            pressAt(0.2, 84.0f);
            const auto ignoredEmptySpace = !roll.diagnosticDraggingAnchor();
            grabWav.deleteFile();
            const auto grabbable = grabNote.isNotEmpty() && tookHold && ignoredEmptySpace;

            // And the tool stays in hand.  The button is offered in every
            // mode, but the layout pass took it straight back: pressing it
            // chose the point tool, and the next time the window laid itself
            // out -- which is what selecting a note ends up doing -- it was
            // swapped for the draw tool.  So the tool worked until the first
            // click on an anchor and then vanished.
            MainComponent window;
            window.setBounds(0, 0, 1280, 760);
            window.diagnosticPressTool(PianoRollComponent::Tool::points);
            const auto toolTakenUp =
                window.diagnosticTool() == PianoRollComponent::Tool::points;
            // A plain layout pass: a resize, a zoom, a panel folding away.
            window.setBounds(0, 0, 1200, 720);
            const auto survivesLayout =
                window.diagnosticTool() == PianoRollComponent::Tool::points;
            // And the path a click on an anchor actually takes: the selection
            // reaches refreshProjectControls, which lays the window out again.
            window.diagnosticRefreshControls();
            const auto survivesSelection =
                window.diagnosticTool() == PianoRollComponent::Tool::points;
            // The tools that were never disturbed stay undisturbed.
            window.diagnosticPressTool(PianoRollComponent::Tool::draw);
            window.diagnosticRefreshControls();
            const auto othersUnchanged =
                window.diagnosticTool() == PianoRollComponent::Tool::draw;
            const auto staysInHand = toolTakenUp && survivesLayout
                && survivesSelection && othersUnchanged;

            const auto ok = bothRendered && pointsChangeTheAudio && changedThroughout
                && grabbable && staysInHand;
            std::cout << "both_rendered=" << (bothRendered ? 1 : 0)
                      << "|points_change_the_audio=" << (pointsChangeTheAudio ? 1 : 0)
                      << "|changed_throughout=" << (changedThroughout ? 1 : 0)
                      << "|rms=" << plainRms << "/" << pointedRms
                      << "|samples_moved=" << moved << "/" << plain.size()
                      << "|worst=" << worst
                      << "|anchor_grabbable_natively=" << (grabbable ? 1 : 0)
                      << "|tool_taken_up=" << (toolTakenUp ? 1 : 0)
                      << "|survives_layout=" << (survivesLayout ? 1 : 0)
                      << "|survives_selection=" << (survivesSelection ? 1 : 0)
                      << "|other_tools_unchanged=" << (othersUnchanged ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-paste-at-pointer")
        {
            // Copying a note off a UTAU track looked like it did nothing.
            // Paste went to the start of the selection, which after a copy is
            // the very notes copied, and outside UTAU notes are placed rather
            // than pushed along -- so the copy landed exactly on top of its
            // original.  And past the end of the clip nothing was inserted at
            // all, silently: measured before the fix, pasting at 1.0, 1.5 and
            // 3.0 seconds into a one-second clip inserted nothing.
            I18n strings;

            // The roll has to report where the pointer is, or the rule above
            // it has nothing to act on.
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-anchor-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=a\r\n"
                                "[#0000]\r\nLength=480\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#0001]\r\nLength=480\r\nLyric=i\r\nNoteNum=62\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            const auto clipId = project.snapshot().tracks.front().clips.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            roll.setFocusedClip(clipId);
            roll.setTool(PianoRollComponent::Tool::note);

            const auto quietAtFirst = !roll.pasteAnchorSeconds().has_value();
            const auto eventAt = [&roll](double seconds)
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(seconds) * 200.0f,
                    roll.diagnosticYForMidi(64.0f));
                return juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false);
            };
            roll.mouseMove(eventAt(2.6));
            const auto anchor = roll.pasteAnchorSeconds();
            const auto followsThePointer = anchor.has_value();
            // Snapped, like everything else placed on the roll, and near where
            // the pointer actually was.
            const auto snapped = followsThePointer
                && std::abs(*anchor - 2.6) <= 0.13;
            const auto onAGridLine = followsThePointer
                && std::abs(*anchor / 0.125 - std::round(*anchor / 0.125)) < 1.0e-6;
            roll.mouseExit(eventAt(2.6));
            const auto forgottenOnLeaving = !roll.pasteAnchorSeconds().has_value();

            // A clip with no recording behind it stretches to take the paste,
            // wherever it lands.  These were the positions that inserted
            // nothing at all.
            const auto phrase = std::vector<NoteData> {
                project.snapshot().tracks.front().clips.front().notes.front() };
            auto landedEverywhere = true;
            for (const auto at : { 1.0, 1.5, 3.0 })
            {
                ProjectModel scratch;
                const auto copy = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("hachi-anchor-" + juce::Uuid().toDashedString() + ".ust");
                copy.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                     "Tempo=120.00\r\nTracks=1\r\nProjectName=a\r\n"
                                     "[#0000]\r\nLength=480\r\nLyric=a\r\nNoteNum=60\r\n"
                                     "[#0001]\r\nLength=480\r\nLyric=i\r\nNoteNum=62\r\n"
                                     "[#TRACKEND]\r\n");
                juce::String error;
                juce::StringArray warnings;
                scratch.addUstFile(copy, error, warnings);
                copy.deleteFile();
                const auto scratchTrack = scratch.snapshot().tracks.front().id;
                scratch.setTrackPitchAlgorithm(scratchTrack, PitchAlgorithm::mld5);
                const auto scratchClip =
                    scratch.snapshot().tracks.front().clips.front().id;
                auto anchored = phrase;
                anchored.front().startSeconds = 0.0;
                if (scratch.insertNotes(scratchClip, anchored, at).empty())
                    landedEverywhere = false;
            }

            // A recording ends where its audio does, so a paste past it lands
            // nowhere -- and the window says so rather than going quiet.
            ProjectModel recorded;
            const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-anchor-" + juce::Uuid().toDashedString() + ".wav");
            {
                juce::AudioBuffer<float> buffer(1, 44100);
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    buffer.setSample(0, index,
                        0.3f * std::sin(static_cast<float>(index) * 0.05f));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), 44100.0, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }
            const auto recordedClip = recorded.addAudioFile(wav, 1.0, 0.0, {});
            auto anchored = phrase;
            anchored.front().startSeconds = 0.0;
            const auto withinTheAudio =
                !recorded.insertNotes(recordedClip, anchored, 0.2).empty();
            const auto pastTheAudio =
                recorded.insertNotes(recordedClip, anchored, 3.0).empty();
            wav.deleteFile();

            const auto ok = quietAtFirst && followsThePointer && snapped
                && onAGridLine && forgottenOnLeaving && landedEverywhere
                && withinTheAudio && pastTheAudio;
            std::cout << "quiet_before_the_pointer_arrives=" << (quietAtFirst ? 1 : 0)
                      << "|follows_the_pointer=" << (followsThePointer ? 1 : 0)
                      << "|near_where_it_pointed=" << (snapped ? 1 : 0)
                      << "|on_a_grid_line=" << (onAGridLine ? 1 : 0)
                      << "|forgotten_on_leaving=" << (forgottenOnLeaving ? 1 : 0)
                      << "|composed_clip_takes_it_anywhere=" << (landedEverywhere ? 1 : 0)
                      << "|recording_takes_it_within=" << (withinTheAudio ? 1 : 0)
                      << "|recording_refuses_past_its_end=" << (pastTheAudio ? 1 : 0)
                      << "|anchor=" << (anchor ? *anchor : -1.0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-note-menu-split")
        {
            // The note menu offered everything on every track, most of it
            // greyed and none of it able to work: timing and the region editor
            // come from a voicebank entry, flags and the four regions and the
            // forced consonant reset are voicebank ideas, and a lyric names the
            // sample to sing.  Pitch-line edits and vibrato are shared.
            const auto utau = PianoRollComponent::noteMenuItemsFor(true);
            const auto plain = PianoRollComponent::noteMenuItemsFor(false);
            const auto has = [](const std::vector<int>& items, int id)
            {
                return std::find(items.begin(), items.end(), id) != items.end();
            };

            // Everything the handler can be asked to do has to appear in at
            // least one of the menus, or an item exists that nothing offers.
            const std::vector<int> handled { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                             11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 23, 24 };
            auto allReachable = true;
            for (const auto id : handled)
                if (!has(utau, id) && !has(plain, id)) allReachable = false;
            // Everything but the plain-only ones is on the UTAU menu.
            const auto utauKeepsEverything = utau.size() == handled.size() - 1
                && allReachable && !has(utau, 18);

            // The plain menu is the general pitch/note edits, including
            // vibrato, plus the plain-only pitch-line reset.
            const std::vector<int> general { 1, 2, 5, 6, 7, 8, 13, 18, 11, 10, 20, 23 };
            auto plainIsGeneral = plain.size() == general.size();
            for (const auto id : general)
                if (!has(plain, id)) plainIsGeneral = false;

            // Named individually, because "the plain menu is short" would pass
            // even if the wrong items were the ones dropped.
            const auto timingGone = !has(plain, 3);
            const auto regionEditorGone = !has(plain, 4);
            const auto vibratoShared = has(plain, 5) && has(plain, 6)
                && has(plain, 7) && has(plain, 8);
            const auto flagsGone = !has(plain, 9) && !has(plain, 14);
            const auto consonantResetGone = !has(plain, 12);
            const auto gapsGone = !has(plain, 15) && !has(plain, 16);
            const auto lyricsGone = !has(plain, 17);
            // And the ones that do work are still there.
            const auto splitStays = has(plain, 1) && has(plain, 2);
            const auto deleteStays = has(plain, 10);
            const auto transposeStays = has(plain, 11);

            // The shared order: dropping items must not reshuffle the rest.
            // Compared over the ids the two menus have in common, since each
            // now carries some the other does not.
            std::vector<int> sharedInUtau, sharedInPlain;
            for (const auto id : utau) if (has(plain, id)) sharedInUtau.push_back(id);
            for (const auto id : plain) if (has(utau, id)) sharedInPlain.push_back(id);
            const auto sameOrder = !sharedInUtau.empty()
                && sharedInUtau == sharedInPlain;

            // No id twice, in either.
            auto noDuplicates = true;
            for (const auto& items : { utau, plain })
            {
                auto sorted = items;
                std::sort(sorted.begin(), sorted.end());
                if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
                    noDuplicates = false;
            }

            // And the menu has to actually consult that list.  Everything
            // above is the list on its own, which a menu that ignores it
            // passes perfectly -- the same hole that let a disconnected rule
            // through in an earlier check here.  So build the real menu on a
            // real note and walk what it carries.
            I18n strings;
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-menu-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=menu\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#0001]\r\nLength=960\r\nLyric=i\r\nNoteNum=62\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            const auto noteId = project.snapshot().tracks.front()
                .clips.front().notes.front().id;

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            const auto shownOnUtau = roll.diagnosticNoteMenuIds(noteId);
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);
            roll.diagnosticRefresh();
            const auto shownOnPlain = roll.diagnosticNoteMenuIds(noteId);

            // 6 and 7 are the two faces of one item, so only one is ever built.
            const auto builtUtauMatches = shownOnUtau.size() == utau.size() - 1;
            const auto builtPlainMatches = shownOnPlain.size() == plain.size() - 1;
            const auto builtPlainHasTheFlatten = has(shownOnPlain, 18)
                && !has(shownOnUtau, 18);
            auto builtPlainIsGeneral = builtPlainMatches;
            for (const auto id : general)
                if (id != 6 && id != 7 && !has(shownOnPlain, id))
                    builtPlainIsGeneral = false;
            builtPlainIsGeneral = builtPlainIsGeneral
                && (has(shownOnPlain, 6) != has(shownOnPlain, 7));
            const auto builtDropsTheUtauOnes = !has(shownOnPlain, 3)
                && !has(shownOnPlain, 9) && !has(shownOnPlain, 17);

            const auto ok = utauKeepsEverything && plainIsGeneral && timingGone
                && regionEditorGone && vibratoShared && flagsGone
                && consonantResetGone && gapsGone && lyricsGone && splitStays
                && deleteStays && transposeStays && sameOrder && noDuplicates
                && builtUtauMatches && builtPlainIsGeneral && builtDropsTheUtauOnes
                && builtPlainHasTheFlatten;
            std::cout << "utau_keeps_everything=" << (utauKeepsEverything ? 1 : 0)
                      << "|plain_is_the_general_edits=" << (plainIsGeneral ? 1 : 0)
                      << "|timing_gone=" << (timingGone ? 1 : 0)
                      << "|region_editor_gone=" << (regionEditorGone ? 1 : 0)
                      << "|vibrato_shared=" << (vibratoShared ? 1 : 0)
                      << "|flags_gone=" << (flagsGone ? 1 : 0)
                      << "|consonant_reset_gone=" << (consonantResetGone ? 1 : 0)
                      << "|gaps_gone=" << (gapsGone ? 1 : 0)
                      << "|lyrics_gone=" << (lyricsGone ? 1 : 0)
                      << "|split_and_merge_stay=" << (splitStays ? 1 : 0)
                      << "|delete_stays=" << (deleteStays ? 1 : 0)
                      << "|transpose_stays=" << (transposeStays ? 1 : 0)
                      << "|order_kept=" << (sameOrder ? 1 : 0)
                      << "|no_duplicates=" << (noDuplicates ? 1 : 0)
                      << "|built_utau_menu_matches=" << (builtUtauMatches ? 1 : 0)
                      << "|built_plain_menu_is_general=" << (builtPlainIsGeneral ? 1 : 0)
                      << "|built_menu_drops_the_utau_ones=" << (builtDropsTheUtauOnes ? 1 : 0)
                      << "|built_plain_menu_has_the_flatten=" << (builtPlainHasTheFlatten ? 1 : 0)
                      << "|counts=" << utau.size() << "/" << plain.size()
                      << "|built=" << shownOnUtau.size() << "/" << shownOnPlain.size()
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-lyric-gate")
        {
            // Double-clicking a note opened a lyric box whatever the track
            // was.  Committing one calls prepareUtauTrackForNote, which sets
            // the track's algorithm to UTAU -- so on a track being pitch
            // shifted from its own recording, a double-click quietly converted
            // it.  There is nothing for a lyric to choose there: the sound is
            // the recording, not a sample named by the label.
            //
            // Driven through the component's own mouseDoubleClick, since the
            // gate is in that handler and a rule tested on its own would not
            // say whether the handler asks it.
            I18n strings;
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-gate-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=gate\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            const auto clipId = project.snapshot().tracks.front().clips.front().id;
            const auto note = project.snapshot().tracks.front().clips.front().notes.front();

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            roll.setFocusedClip(clipId);
            roll.setTool(PianoRollComponent::Tool::note);
            // The window listens to this and answers by setting the track's
            // algorithm to UTAU.  Counting it here is what says the conversion
            // is out of reach, rather than merely that a box stayed shut.
            auto namings = 0;
            roll.onNoteAliasCommitted = [&namings](const juce::String&) { ++namings; };

            const auto doubleClickNote = [&roll, &note]
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(note.startSeconds
                        + note.durationSeconds * 0.5) * 200.0f,
                    roll.diagnosticYForMidi(note.midiNote));
                const juce::MouseEvent event(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 2, false);
                roll.mouseDoubleClick(event);
            };

            // A UTAU track: the lyric is how the sample is chosen, so the box
            // still opens.  Checked first, because a gate that never opens it
            // would pass the important half for the wrong reason.
            doubleClickNote();
            const auto opensOnUtau = roll.diagnosticAliasEditorOpen();
            roll.diagnosticCommitAliasEdit("ka");
            const auto namesOnUtau = namings == 1;

            // The same note on a track pitch shifting its own recording.
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);
            roll.diagnosticRefresh();
            doubleClickNote();
            const auto silentOnPlainTrack = !roll.diagnosticAliasEditorOpen();
            // Nothing to accept, so the window is never told a note was named
            // and never converts the track.  Trying anyway must change nothing.
            roll.diagnosticCommitAliasEdit("ka");
            const auto nothingToName = namings == 1;
            const auto stillPlain = project.snapshot().tracks.front().pitchAlgorithm
                == PitchAlgorithm::mld5;
            // The double-click still selects, which is all it should do here.
            const auto stillSelects = !roll.selectedNoteIds().empty();

            const auto ok = opensOnUtau && namesOnUtau && silentOnPlainTrack
                && nothingToName && stillPlain && stillSelects;
            std::cout << "opens_on_a_utau_track=" << (opensOnUtau ? 1 : 0)
                      << "|naming_reaches_the_window=" << (namesOnUtau ? 1 : 0)
                      << "|silent_on_a_plain_track=" << (silentOnPlainTrack ? 1 : 0)
                      << "|nothing_to_name_there=" << (nothingToName ? 1 : 0)
                      << "|track_not_converted=" << (stillPlain ? 1 : 0)
                      << "|still_selects_the_note=" << (stillSelects ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-syllable-cuts")
        {
            // A note only ended where the pitch tracker went quiet for more
            // than 80 ms.  Sung legato there is no such gap, so a phrase came
            // in as one note: a 17.8 second vocal gave 23 notes, the longest
            // running 1.795 seconds across five syllables.  A voiced run is
            // now also divided at the energy valleys between syllables.
            //
            // Both directions matter.  Splitting too little was the report;
            // splitting too much would shred a held syllable, and this is the
            // same rule that has to leave 490 single-syllable recordings
            // alone, so the shape of a sustained note is checked too.
            constexpr auto guard = 18;        // 90 ms at the 5 ms hop
            constexpr auto depth = 0.25f;

            // Two syllables joined without a break: full, valley, full.
            std::vector<float> pair;
            for (int frame = 0; frame < 60; ++frame) pair.push_back(0.20f);
            for (int frame = 0; frame < 6; ++frame) pair.push_back(0.02f);
            for (int frame = 0; frame < 60; ++frame) pair.push_back(0.20f);
            const auto pairCuts = backend::NativeAnalyzer::syllableCuts(pair, guard, depth);
            const auto joinFound = pairCuts.size() == 1
                && std::abs(pairCuts.front() - 63) <= 3;

            // One held syllable with vibrato: the level wavers but never
            // falls away, so there is nothing to cut.
            std::vector<float> held;
            for (int frame = 0; frame < 160; ++frame)
                held.push_back(0.20f + 0.05f
                    * std::sin(static_cast<float>(frame) * 0.4f));
            const auto heldCuts = backend::NativeAnalyzer::syllableCuts(held, guard, depth);
            const auto vibratoLeftAlone = heldCuts.empty();

            // A dip that only halves the level is not a join.
            std::vector<float> shallow;
            for (int frame = 0; frame < 60; ++frame) shallow.push_back(0.20f);
            for (int frame = 0; frame < 6; ++frame) shallow.push_back(0.10f);
            for (int frame = 0; frame < 60; ++frame) shallow.push_back(0.20f);
            const auto shallowLeftAlone =
                backend::NativeAnalyzer::syllableCuts(shallow, guard, depth).empty();

            // A valley beside one loud syllable and one soft one is judged by
            // the soft one, or every fade would read as a join.  The pair of
            // cases is the rule: the same 0.03 valley that is far below the
            // loud side is only half the quiet side, and is left alone; drop
            // it to 0.005 and it is a join against both.
            const auto unevenWith = [](float valley)
            {
                std::vector<float> uneven;
                for (int frame = 0; frame < 60; ++frame) uneven.push_back(0.40f);
                for (int frame = 0; frame < 6; ++frame) uneven.push_back(valley);
                for (int frame = 0; frame < 60; ++frame) uneven.push_back(0.06f);
                return backend::NativeAnalyzer::syllableCuts(uneven, guard, depth);
            };
            const auto quietNeighbourWins = unevenWith(0.03f).empty()
                && unevenWith(0.005f).size() == 1;

            // Nothing is cut so close to an edge that a piece could not hold
            // a syllable, and three syllables give two cuts, not a shower.
            std::vector<float> three;
            for (int group = 0; group < 3; ++group)
            {
                if (group > 0)
                    for (int frame = 0; frame < 6; ++frame) three.push_back(0.02f);
                for (int frame = 0; frame < 50; ++frame) three.push_back(0.20f);
            }
            const auto threeCuts = backend::NativeAnalyzer::syllableCuts(three, guard, depth);
            const auto twoJoins = threeCuts.size() == 2;
            auto keptApart = true;
            auto previous = 0;
            for (const auto cut : threeCuts)
            {
                if (cut - previous < guard) keptApart = false;
                previous = cut;
            }
            if (!threeCuts.empty()
                && static_cast<int>(three.size()) - threeCuts.back() < guard)
                keptApart = false;

            // Too short to hold two syllables: nothing is cut at all.
            std::vector<float> brief;
            for (int frame = 0; frame < 20; ++frame) brief.push_back(0.20f);
            brief[10] = 0.01f;
            const auto briefLeftAlone =
                backend::NativeAnalyzer::syllableCuts(brief, guard, depth).empty();

            // Silence has no valleys, only division by nothing.
            const std::vector<float> quiet(120, 0.0f);
            const auto silenceLeftAlone =
                backend::NativeAnalyzer::syllableCuts(quiet, guard, depth).empty();

            // And the rule has to actually reach the analysis.  Everything
            // above is the function on its own, which a version that computes
            // cuts and then ignores them passes perfectly -- disconnecting it
            // was the first break tried here, and it went unnoticed.
            //
            // Two steady tones with a 30 ms trough between them: too short for
            // the 80 ms unvoiced gap to be what divides them, so a second note
            // can only come from the valley.
            const auto tone = [](juce::AudioBuffer<float>& buffer, double rate,
                                 double from, double to, double level)
            {
                const auto first = static_cast<int>(from * rate);
                const auto last = std::min(buffer.getNumSamples(),
                                           static_cast<int>(to * rate));
                for (int index = first; index < last; ++index)
                {
                    const auto phase = 2.0 * juce::MathConstants<double>::pi
                        * 220.0 * static_cast<double>(index) / rate;
                    const auto value = std::sin(phase) + 0.5 * std::sin(2.0 * phase)
                        + 0.25 * std::sin(3.0 * phase);
                    buffer.setSample(0, index, static_cast<float>(level * value));
                }
            };
            const auto writeWav = [](const juce::File& target,
                                     const juce::AudioBuffer<float>& buffer, double rate)
            {
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(target.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            };
            constexpr auto rate = 44100.0;
            const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-cuts-" + juce::Uuid().toDashedString());
            folder.createDirectory();

            const auto joined = folder.getChildFile("joined.wav");
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * 0.73));
                buffer.clear();
                tone(buffer, rate, 0.0, 0.34, 0.35);
                // 60 ms: wider than the 25 ms the level is measured over, so
                // the bottom is actually reached, and still short of the 80 ms
                // that the unvoiced-gap rule needs -- so a second note here
                // can only have come from the valley.
                tone(buffer, rate, 0.34, 0.40, 0.010);
                tone(buffer, rate, 0.40, 0.73, 0.35);
                writeWav(joined, buffer, rate);
            }
            const auto sustained = folder.getChildFile("sustained.wav");
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * 0.73));
                buffer.clear();
                tone(buffer, rate, 0.0, 0.73, 0.35);
                writeWav(sustained, buffer, rate);
            }
            juce::String analysisError;
            const auto config = backend::AnalysisService::configFromEnvironment();
            const auto joinedNotes =
                backend::AnalysisService::analyse(joined, config, analysisError).notes.size();
            const auto sustainedNotes =
                backend::AnalysisService::analyse(sustained, config, analysisError).notes.size();
            folder.deleteRecursively();
            const auto analysisDividesIt = joinedNotes == 2;
            const auto analysisHoldsOne = sustainedNotes == 1;

            const auto ok = joinFound && vibratoLeftAlone && shallowLeftAlone
                && quietNeighbourWins && twoJoins && keptApart && briefLeftAlone
                && silenceLeftAlone && analysisDividesIt && analysisHoldsOne;
            std::cout << "join_found=" << (joinFound ? 1 : 0)
                      << "|vibrato_left_alone=" << (vibratoLeftAlone ? 1 : 0)
                      << "|shallow_dip_left_alone=" << (shallowLeftAlone ? 1 : 0)
                      << "|quiet_neighbour_decides=" << (quietNeighbourWins ? 1 : 0)
                      << "|three_syllables_two_joins=" << (twoJoins ? 1 : 0)
                      << "|pieces_can_hold_a_syllable=" << (keptApart ? 1 : 0)
                      << "|too_short_left_alone=" << (briefLeftAlone ? 1 : 0)
                      << "|silence_left_alone=" << (silenceLeftAlone ? 1 : 0)
                      << "|analysis_divides_a_joined_pair=" << (analysisDividesIt ? 1 : 0)
                      << "|analysis_holds_a_sustained_one=" << (analysisHoldsOne ? 1 : 0)
                      << "|cut_at=" << (pairCuts.empty() ? -1 : pairCuts.front())
                      << "|notes=" << joinedNotes << "/" << sustainedNotes
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-single-syllable")
        {
            // One recording of one syllable must import as one note.
            //
            // A voicebank writes several oto entries against the same file --
            // different aliases over the same sound -- and each was becoming
            // its own note, stacked on the others.  New Geping's a.wav is
            // 0.41 seconds of "a" with three rows against it, and imported as
            // three overlapping notes; across that bank 434 of 490 files came
            // in stacked.  On a track that sings one note at a time the extras
            // cannot sound, which is what "the second half is silent" was.
            using Span = ProjectModel::RegionSpan;

            // The real rows from that file.
            const std::vector<Span> alternatives {
                { 0.041, 0.309237288 }, { 0.041, 0.263139288 }, { 0.074, 0.222553288 }
            };
            const auto kept = ProjectModel::regionsToImport(alternatives);
            const auto oneSyllableOneNote = kept.size() == 1 && kept.front() == 0;

            // Rows that do not overlap are separate sounds in one file, and
            // all of them stay, in the order the file gave them.
            const std::vector<Span> sequence { { 0.0, 0.2 }, { 0.25, 0.4 }, { 0.5, 0.9 } };
            const auto sequenceKept = ProjectModel::regionsToImport(sequence);
            const auto sequenceAllKept = sequenceKept.size() == 3
                && sequenceKept[0] == 0 && sequenceKept[1] == 1 && sequenceKept[2] == 2;

            // Mixed: a long one, a short one inside it, and one clear of both.
            const std::vector<Span> mixed { { 0.0, 0.5 }, { 0.1, 0.2 }, { 0.6, 0.8 } };
            const auto mixedKept = ProjectModel::regionsToImport(mixed);
            const auto mixedRight = mixedKept.size() == 2
                && mixedKept[0] == 0 && mixedKept[1] == 2;

            // Same length, overlapping: the earlier row wins, so the answer
            // does not depend on how the sort happened to order them.
            const std::vector<Span> tied { { 0.1, 0.3 }, { 0.2, 0.4 } };
            const auto tiedKept = ProjectModel::regionsToImport(tied);
            const auto tieGoesToFirst = tiedKept.size() == 1 && tiedKept.front() == 0;

            // An empty row is not a note.
            const std::vector<Span> empty { { 0.1, 0.1 }, { 0.2, 0.6 } };
            const auto emptyKept = ProjectModel::regionsToImport(empty);
            const auto emptyDropped = emptyKept.size() == 1 && emptyKept.front() == 1;

            // And the whole import, on a file with those three rows beside it.
            const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-syl-" + juce::Uuid().toDashedString());
            folder.createDirectory();
            const auto wav = folder.getChildFile("a.wav");
            constexpr auto rate = 44100.0;
            const auto samples = static_cast<int>(rate * 0.406553);
            {
                juce::AudioBuffer<float> buffer(1, samples);
                for (int index = 0; index < samples; ++index)
                    buffer.setSample(0, index, 0.3f
                        * std::sin(static_cast<float>(index) * 0.03f));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, samples);
                }
            }
            const auto sidecar = SampleSettings::sidecarFor(wav);
            sidecar.replaceWithText(
                "name,region_start_sec,region_end_sec,note_alignment_sec,"
                "fixed_duration_sec,relative_pitch_cents,melodyne_project_data,"
                "melodyne_pitch_center_cents,melodyne_original_pitch_center_cents,"
                "melodyne_pitch_drift_factor,melodyne_pitch_modulation_factor,"
                "melodyne_transition_sec,melodyne_formant_offset_cents,"
                "melodyne_amplitude_factor,melodyne_sibilant_balance,"
                "melodyne_attack_duration_sec,melodyne_decay_elongation,"
                "utau_overlap_sec\n"
                "a,0.041,0.309237288,0.049938,0.087688,0,0,0,0,1,1,0,0,1,0,0,0,0.004457\n"
                "a -,0.041,0.263139288,0.049938,0.087688,0,0,0,0,1,1,0,0,1,0,0,0,0.004457\n"
                "a,0.074,0.222553288,0.083,0.054,0,0,0,0,1,1,0,0,1,0,0,0,0.004\n");

            ProjectModel project;
            const auto clipId = project.addAudioFile(wav, 0.406553, 0.0, {});
            const auto clip = project.snapshot().tracks.front().clips.front();
            const auto sidecarWasRead = clipId.isNotEmpty() && !clip.notes.empty();
            const auto importedOne = clip.notes.size() == 1;
            const auto keptTheFullest = importedOne
                && std::abs(clip.notes.front().startSeconds - 0.041) < 1.0e-6
                && std::abs(clip.notes.front().startSeconds
                            + clip.notes.front().durationSeconds - 0.309237288) < 1.0e-6;
            folder.deleteRecursively();

            const auto ok = oneSyllableOneNote && sequenceAllKept && mixedRight
                && tieGoesToFirst && emptyDropped && sidecarWasRead && importedOne
                && keptTheFullest;
            std::cout << "one_syllable_one_note=" << (oneSyllableOneNote ? 1 : 0)
                      << "|separate_sounds_all_kept=" << (sequenceAllKept ? 1 : 0)
                      << "|mixed_case=" << (mixedRight ? 1 : 0)
                      << "|tie_goes_to_the_first=" << (tieGoesToFirst ? 1 : 0)
                      << "|empty_row_dropped=" << (emptyDropped ? 1 : 0)
                      << "|sidecar_was_read=" << (sidecarWasRead ? 1 : 0)
                      << "|import_makes_one_note=" << (importedOne ? 1 : 0)
                      << "|and_it_is_the_fullest=" << (keptTheFullest ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-deleted-notes-silent")
        {
            // Deleting every note of a recording left it sounding.  The notes
            // are what a recording is played through; with none left the clip
            // rendered its source straight out, so the material stayed at full
            // volume with its waveform drawn.  Measured before the fix at
            // 0.1996 RMS against 0 for no clip at all.
            //
            // Rendered for real here rather than reasoned about, because
            // "the clip is gone from the project" and "nothing comes out of
            // the speakers" are not the same claim.
            const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-sil-" + juce::Uuid().toDashedString() + ".wav");
            {
                juce::AudioBuffer<float> buffer(1, 44100);
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    buffer.setSample(0, index,
                        0.4f * std::sin(static_cast<float>(index) * 0.06f));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), 44100.0, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }

            const auto measure = [](ProjectData data)
            {
                AudioEngine engine;
                engine.syncProject(data);
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 8'000.0;
                while (juce::Time::getMillisecondCounterHiRes() < deadline)
                {
                    if (!engine.renderProgress()) break;
                    juce::Thread::sleep(10);
                }
                auto output = juce::File::createTempFile("hachi-sil-out.wav");
                juce::String error;
                auto rms = -1.0;
                if (engine.exportWav(output, error))
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    if (auto reader = std::unique_ptr<juce::AudioFormatReader>(
                            formats.createReaderFor(output)))
                    {
                        juce::AudioBuffer<float> buffer(
                            static_cast<int>(reader->numChannels),
                            static_cast<int>(reader->lengthInSamples));
                        reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
                        auto squareSum = 0.0;
                        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                            {
                                const auto value = buffer.getSample(channel, sample);
                                squareSum += static_cast<double>(value) * value;
                            }
                        rms = std::sqrt(squareSum
                            / static_cast<double>(std::max(1, buffer.getNumChannels()
                                                              * buffer.getNumSamples())));
                    }
                }
                output.deleteFile();
                return rms;
            };
            const auto clipCount = [](const ProjectData& data)
            {
                auto clips = 0;
                for (const auto& track : data.tracks)
                    clips += static_cast<int>(track.clips.size());
                return clips;
            };

            ProjectModel project;
            const auto clipId = project.addAudioFile(wav, 1.0, 0.0, {});
            const auto first = project.addNote(clipId, 0.0, 0.4, 60.0f);
            const auto second = project.addNote(clipId, 0.5, 0.4, 62.0f);
            const auto fixtureReaches = first.isNotEmpty() && second.isNotEmpty()
                && clipCount(project.snapshot()) == 1;
            const auto soundsAtFirst = measure(project.snapshot());

            // One of the two: the recording is still being played through the
            // other, so it stays.
            project.removeNotes({ first });
            const auto keptOnPartial = clipCount(project.snapshot()) == 1;
            const auto stillSounds = measure(project.snapshot());

            // The last one: nothing is left to play it through.
            project.removeNotes({ second });
            const auto clipWentToo = clipCount(project.snapshot()) == 0;
            const auto silent = measure(project.snapshot());

            // And it comes back together, in one step: the notes and the
            // recording went in one operation, so they return in one.
            project.undo();
            const auto undoBringsBoth = clipCount(project.snapshot()) == 1
                && !project.snapshot().tracks.front().clips.front().notes.empty();

            // A composed clip has nothing to sound and is kept -- with no
            // notes it is the silent span a new track is drawn into.
            ProjectModel composed;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-sil-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=s\r\n"
                                "[#0000]\r\nLength=480\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto ustBuilt = composed.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            std::vector<juce::String> everyNote;
            for (const auto& track : composed.snapshot().tracks)
                for (const auto& clip : track.clips)
                    for (const auto& note : clip.notes) everyNote.push_back(note.id);
            composed.removeNotes(everyNote);
            const auto composedKept = ustBuilt && clipCount(composed.snapshot()) == 1;
            const auto composedSilent = measure(composed.snapshot());
            // A UST clip keeps the .ust as its source, so "has a source file"
            // could not tell the two apart -- this is what says the kept one
            // makes no sound of its own.
            const auto keptOneIsSilent = composedSilent >= 0.0
                && composedSilent <= 1.0e-5;

            const auto ok = fixtureReaches && soundsAtFirst > 1.0e-5 && keptOnPartial
                && stillSounds > 1.0e-5 && clipWentToo && silent >= 0.0
                && silent <= 1.0e-5 && undoBringsBoth && composedKept
                && keptOneIsSilent;
            std::cout << "fixture_reaches=" << (fixtureReaches ? 1 : 0)
                      << "|sounds_with_notes=" << (soundsAtFirst > 1.0e-5 ? 1 : 0)
                      << "|partial_delete_keeps_it=" << (keptOnPartial ? 1 : 0)
                      << "|and_it_still_sounds=" << (stillSounds > 1.0e-5 ? 1 : 0)
                      << "|last_note_takes_the_clip=" << (clipWentToo ? 1 : 0)
                      << "|and_it_is_silent=" << (silent >= 0.0 && silent <= 1.0e-5 ? 1 : 0)
                      << "|undo_brings_both_back=" << (undoBringsBoth ? 1 : 0)
                      << "|composed_clip_kept=" << (composedKept ? 1 : 0)
                      << "|and_the_kept_one_is_silent=" << (keptOneIsSilent ? 1 : 0)
                      << "|rms=" << soundsAtFirst << "/" << stillSounds << "/" << silent
                      << std::endl;
            wav.deleteFile();
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-delete-clip")
        {
            // An imported clip could not be deleted from the keyboard.  The
            // piano roll deletes notes and knows nothing of clips, and a clip
            // just imported has no notes at all until its analysis finishes --
            // so Delete was not handled by anything, and the material stayed
            // with its waveform showing.  Reproduced before the fix: the roll
            // reported the key unhandled and the clip count never moved.
            I18n strings;

            // The rule: notes first, since a selection of them is the more
            // specific thing to have asked for; the clip when there are none.
            using Target = MainComponent::DeleteTarget;
            const auto notesWin = MainComponent::deleteTargetFor(true, true) == Target::notes;
            const auto notesAlone = MainComponent::deleteTargetFor(true, false) == Target::notes;
            const auto clipWhenNoNotes =
                MainComponent::deleteTargetFor(false, true) == Target::clip;
            const auto nothingWhenNothing =
                MainComponent::deleteTargetFor(false, false) == Target::nothing;

            // And the model end: removing the clip takes the waveform with it,
            // because the waveform is drawn from the clip's source file.
            ProjectModel project;
            const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-del-" + juce::Uuid().toDashedString() + ".wav");
            {
                juce::AudioBuffer<float> buffer(1, 44100);
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    buffer.setSample(0, index,
                        0.3f * std::sin(static_cast<float>(index) * 0.05f));
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), 44100.0, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }
            const auto clipId = project.addAudioFile(wav, 1.0, 0.0, {});
            const auto countClips = [&project]
            {
                auto clips = 0;
                for (const auto& track : project.snapshot().tracks)
                    clips += static_cast<int>(track.clips.size());
                return clips;
            };
            const auto countNotes = [&project]
            {
                auto notes = 0;
                for (const auto& track : project.snapshot().tracks)
                    for (const auto& clip : track.clips)
                        notes += static_cast<int>(clip.notes.size());
                return notes;
            };
            const auto imported = clipId.isNotEmpty() && countClips() == 1;
            // The heart of the report: freshly imported, it has no notes, so
            // there is nothing a note selection could reach.
            const auto arrivesWithNoNotes = countNotes() == 0;

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            if (!project.snapshot().tracks.empty())
                roll.setFocusedTrack(project.snapshot().tracks.front().id);
            roll.setFocusedClip(clipId);
            roll.selectAllNotes();
            const auto selectsNothing = roll.diagnosticSelectedCount() == 0;
            const auto rollDeclinesTheKey =
                !roll.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
            const auto stillThere = countClips() == 1;

            // Which is why the window has to take it.  Standing in for the
            // window, whose Delete branch is the rule above plus this call.
            project.removeClip(clipId);
            const auto clipGone = countClips() == 0;

            const auto ok = notesWin && notesAlone && clipWhenNoNotes
                && nothingWhenNothing && imported && arrivesWithNoNotes
                && selectsNothing && rollDeclinesTheKey && stillThere && clipGone;
            std::cout << "notes_win_over_clip=" << (notesWin ? 1 : 0)
                      << "|notes_alone=" << (notesAlone ? 1 : 0)
                      << "|clip_when_no_notes=" << (clipWhenNoNotes ? 1 : 0)
                      << "|nothing_when_nothing=" << (nothingWhenNothing ? 1 : 0)
                      << "|imported=" << (imported ? 1 : 0)
                      << "|arrives_with_no_notes=" << (arrivesWithNoNotes ? 1 : 0)
                      << "|select_all_finds_nothing=" << (selectsNothing ? 1 : 0)
                      << "|roll_declines_the_key=" << (rollDeclinesTheKey ? 1 : 0)
                      << "|clip_survives_the_roll=" << (stillThere ? 1 : 0)
                      << "|remove_clip_clears_it=" << (clipGone ? 1 : 0)
                      << std::endl;
            wav.deleteFile();
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-draw-unit")
        {
            // The step a drawn note grows by, now settable from the draw
            // button's own right-click menu.  What matters is that the chosen
            // step is the one the drag actually uses -- a setting that is read
            // back correctly but never reaches the drag would look identical
            // in the menu and do nothing.
            I18n strings;

            const auto offered = PianoRollComponent::drawLengthDivisions();
            // 1/128 of a bar up to 1/4, doubling, coarsest last.
            const auto expected = std::vector<int> { 128, 64, 32, 16, 8, 4 };
            const auto listIsRight = offered == expected;

            ProjectModel project;
            const auto trackId = project.addTrack("unit", true);
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            roll.setTool(PianoRollComponent::Tool::draw);

            // 120 bpm in 4/4: a bar is two seconds.
            constexpr auto bar = 2.0;
            const auto startsAtASixtyFourth = roll.drawLengthDivision() == 64;
            // A settings file can say anything; a step of 1/37 of a bar is not
            // a thing to honour, and the one in force must not change.
            roll.setDrawLengthDivision(37);
            const auto strayIgnored = roll.drawLengthDivision() == 64;
            roll.setDrawLengthDivision(0);
            const auto zeroIgnored = roll.drawLengthDivision() == 64;

            const auto eventAt = [&roll](double seconds, float midi)
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(seconds) * 200.0f,
                    roll.diagnosticYForMidi(midi));
                return juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false);
            };

            // Every step offered, drawn with: a press that never moves is one
            // unit long, and one unit is a bar over the divisor.
            auto everyStepReachesTheDrag = true;
            juce::String measured;
            auto where = 0.0;
            for (const auto division : offered)
            {
                roll.setDrawLengthDivision(division);
                roll.diagnosticRefresh();
                const auto midi = 60.0f + static_cast<float>(division % 12);
                roll.mouseDown(eventAt(where, midi));
                const auto pressed = roll.diagnosticDrawnLength();
                roll.mouseUp(eventAt(where, midi));
                const auto wanted = bar / static_cast<double>(division);
                if (std::abs(pressed - wanted) > 1.0e-9) everyStepReachesTheDrag = false;
                measured += juce::String(division) + ":" + juce::String(pressed, 5) + " ";
                // Well clear of the last one, whatever its length.
                where += 1.0;
            }

            // The coarsest and the finest differ by the factor the list says.
            roll.setDrawLengthDivision(4);
            roll.diagnosticRefresh();
            roll.mouseDown(eventAt(20.0, 72.0f));
            const auto coarse = roll.diagnosticDrawnLength();
            roll.mouseUp(eventAt(20.0, 72.0f));
            roll.setDrawLengthDivision(128);
            roll.diagnosticRefresh();
            roll.mouseDown(eventAt(25.0, 74.0f));
            const auto fine = roll.diagnosticDrawnLength();
            roll.mouseUp(eventAt(25.0, 74.0f));
            const auto thirtyTwoFoldRange = std::abs(coarse - fine * 32.0) < 1.0e-9;

            const auto ok = listIsRight && startsAtASixtyFourth && strayIgnored
                && zeroIgnored && everyStepReachesTheDrag && thirtyTwoFoldRange;
            std::cout << "list_is_right=" << (listIsRight ? 1 : 0)
                      << "|starts_at_a_64th=" << (startsAtASixtyFourth ? 1 : 0)
                      << "|stray_value_ignored=" << (strayIgnored ? 1 : 0)
                      << "|zero_ignored=" << (zeroIgnored ? 1 : 0)
                      << "|every_step_reaches_the_drag=" << (everyStepReachesTheDrag ? 1 : 0)
                      << "|coarsest_is_32x_the_finest=" << (thirtyTwoFoldRange ? 1 : 0)
                      << "|seconds=" << measured.trim()
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-new-track-draw")
        {
            // A new project, a new track, and the draw tool.  This did nothing
            // at all: a track starts with no clips, every note-making path
            // needs one, and nothing in the interface made one.  No error said
            // so -- the click simply had no effect.
            //
            // Both branches are exercised, because a new track is mld5 until
            // it is switched over, and the UTAU one draws by press and release.
            I18n strings;

            // --- the click-to-create tool ---------------------------------
            ProjectModel plain;
            const auto plainTrack = plain.addTrack("plain", true);
            const auto startsWithNoClips = plain.snapshot().tracks.back().clips.empty();
            const auto startsAsMld5 = plain.snapshot().tracks.back().pitchAlgorithm
                == PitchAlgorithm::mld5;

            PianoRollComponent plainRoll(plain, strings);
            plainRoll.setBounds(0, 0, 1400, 700);
            plainRoll.setPixelsPerSecond(200.0f);
            plainRoll.setFocusedTrack(plainTrack);
            plainRoll.setTool(PianoRollComponent::Tool::draw);

            const auto eventOn = [](PianoRollComponent& roll, double seconds, float midi)
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(seconds) * 200.0f,
                    roll.diagnosticYForMidi(midi));
                return juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false);
            };
            const auto notesOf = [](ProjectModel& project)
            {
                std::vector<NoteData> all;
                for (const auto& track : project.snapshot().tracks)
                    for (const auto& clip : track.clips)
                        for (const auto& note : clip.notes)
                            all.push_back(note);
                return all;
            };

            plainRoll.mouseDown(eventOn(plainRoll, 1.0, 64.0f));
            plainRoll.mouseUp(eventOn(plainRoll, 1.0, 64.0f));
            const auto clickMadeANote = notesOf(plain).size() == 1;
            const auto clipAppeared = plain.snapshot().tracks.back().clips.size() == 1;
            // From the start of the timeline, so the bars before the first
            // note are not left unreachable.
            const auto clipStartsAtZero = clipAppeared
                && std::abs(plain.snapshot().tracks.back().clips.front().startSeconds) < 1.0e-9;

            // A second note well past the seed clip's end: a clip with no
            // recording behind it stretches, so this must not hit a wall.
            plainRoll.diagnosticRefresh();
            plainRoll.mouseDown(eventOn(plainRoll, 9.0, 67.0f));
            plainRoll.mouseUp(eventOn(plainRoll, 9.0, 67.0f));
            const auto grewToFit = notesOf(plain).size() == 2;

            // --- the UTAU press-drag-release tool -------------------------
            ProjectModel utau;
            const auto utauTrack = utau.addTrack("utau", true);
            utau.setTrackPitchAlgorithm(utauTrack, PitchAlgorithm::utau);

            PianoRollComponent utauRoll(utau, strings);
            utauRoll.setBounds(0, 0, 1400, 700);
            utauRoll.setPixelsPerSecond(200.0f);
            utauRoll.setFocusedTrack(utauTrack);
            utauRoll.setTool(PianoRollComponent::Tool::draw);

            utauRoll.mouseDown(eventOn(utauRoll, 1.0, 64.0f));
            // The branch is chosen by the track, not by a focused clip -- a
            // new track has no clip to be focused, and gating on one sent the
            // first stroke down the click-to-create path instead.
            const auto utauDrawStarted = utauRoll.diagnosticDrawingNote();
            utauRoll.mouseDrag(eventOn(utauRoll, 1.5, 64.0f));
            utauRoll.mouseUp(eventOn(utauRoll, 1.5, 64.0f));
            const auto utauMadeANote = notesOf(utau).size() == 1;
            const auto utauClipAppeared = utau.snapshot().tracks.back().clips.size() == 1;

            // An audio track is not a place to draw, and never gets a clip
            // this way.
            ProjectModel audio;
            const auto audioTrack = audio.addTrack("audio", false);
            const auto audioRefused = audio.addClip(audioTrack, 0.0, 1.0).isEmpty();

            const auto ok = startsWithNoClips && startsAsMld5 && clickMadeANote
                && clipAppeared && clipStartsAtZero && grewToFit
                && utauDrawStarted && utauMadeANote && utauClipAppeared
                && audioRefused;
            std::cout << "starts_with_no_clips=" << (startsWithNoClips ? 1 : 0)
                      << "|starts_as_mld5=" << (startsAsMld5 ? 1 : 0)
                      << "|click_made_a_note=" << (clickMadeANote ? 1 : 0)
                      << "|clip_appeared=" << (clipAppeared ? 1 : 0)
                      << "|clip_starts_at_zero=" << (clipStartsAtZero ? 1 : 0)
                      << "|clip_grew_to_fit=" << (grewToFit ? 1 : 0)
                      << "|utau_draw_started=" << (utauDrawStarted ? 1 : 0)
                      << "|utau_made_a_note=" << (utauMadeANote ? 1 : 0)
                      << "|utau_clip_appeared=" << (utauClipAppeared ? 1 : 0)
                      << "|audio_track_refused=" << (audioRefused ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-draw-drag")
        {
            // Drawing in the UTAU modes: press puts a start point down, the
            // drag sizes the note in sixty-fourths of a bar, and only letting
            // go makes it.  Pressing where something is already sounding puts
            // the start after it instead.
            //
            // Driven through the component's own mouse handlers, so the
            // snapping, the unit, the preview state and the model call are all
            // in the picture.
            I18n strings;
            ProjectModel project;
            // 120 bpm, 4/4: a bar is two seconds, so a unit is 1/32 second.
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-drag-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=drag\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#0001]\r\nLength=1920\r\nLyric=R\r\nNoteNum=60\r\n"
                                "[#0002]\r\nLength=960\r\nLyric=i\r\nNoteNum=62\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto clipId = project.snapshot().tracks.front().clips.front().id;
            const auto notesNow = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            constexpr auto unit = 2.0 / 64.0;   // a sixty-fourth of a two-second bar

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(project.snapshot().tracks.front().id);
            roll.setFocusedClip(clipId);
            roll.setTool(PianoRollComponent::Tool::draw);

            const auto eventAt = [&roll](double seconds, float midi)
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(seconds) * 200.0f,
                    roll.diagnosticYForMidi(midi));
                return juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false);
            };
            const auto press = [&](double seconds, float midi)
            { roll.mouseDown(eventAt(seconds, midi)); };
            const auto moveTo = [&](double seconds, float midi)
            { roll.mouseDrag(eventAt(seconds, midi)); };
            const auto release = [&](double seconds, float midi)
            { roll.mouseUp(eventAt(seconds, midi)); };
            const auto notePitched = [](const std::vector<NoteData>& notes, float midi)
            {
                for (const auto& note : notes)
                    if (std::abs(note.midiNote - midi) < 0.01f) return note;
                return NoteData{};
            };
            const auto near = [](double left, double right)
            { return std::abs(left - right) < 1.0e-6; };

            const auto before = notesNow().size();

            // 1.  Press in the silence and let go without moving: one unit.
            press(1.5, 64.0f);
            const auto drawingStarted = roll.diagnosticDrawingNote();
            const auto nothingYet = notesNow().size() == before;
            const auto oneUnitWhilePressed = near(roll.diagnosticDrawnLength(), unit);
            release(1.5, 64.0f);
            const auto madeOne = notesNow().size() == before + 1;
            const auto shortest = notePitched(notesNow(), 64.0f);
            const auto pressAloneIsOneUnit = near(shortest.durationSeconds, unit);

            // 2.  Press and drag five and a half units past the start: the
            //     unit under the pointer counts, so six.
            //
            //     Measured from the start the press produced, not from where
            //     the press landed -- those differ, because the start snaps
            //     back to its grid cell, and reading the click position here
            //     instead was wrong the first time.
            press(1.7, 66.0f);
            const auto grew = roll.diagnosticDrawnStart() + 5.5 * unit;
            moveTo(grew, 66.0f);
            const auto sixWhilePressed = near(roll.diagnosticDrawnLength(), 6.0 * unit);
            release(grew, 66.0f);
            const auto dragged = notePitched(notesNow(), 66.0f);
            const auto dragSizesIt = near(dragged.durationSeconds, 6.0 * unit);

            // 3.  Press on the first note, which is sounding from 0 to 1: the
            //     start goes after it, not inside it.
            press(0.5, 67.0f);
            const auto startedAfterIt = near(roll.diagnosticDrawnStart(), 1.0);
            release(0.5, 67.0f);
            const auto afterIt = notePitched(notesNow(), 67.0f);
            const auto occupiedCellStartsAfter = near(afterIt.startSeconds, 1.0);

            // 4.  Drag far past the next note: the length stops where it
            //     begins.  The last note runs from 3 s, so a draw starting at
            //     2.5 s has half a second of room and no more.
            press(2.5, 69.0f);
            moveTo(9.0, 69.0f);
            release(9.0, 69.0f);
            const auto clamped = notePitched(notesNow(), 69.0f);
            const auto stopsAtTheNext = near(clamped.startSeconds + clamped.durationSeconds, 3.0);

            const auto after = notesNow();
            auto overlaps = 0;
            for (std::size_t a = 0; a < after.size(); ++a)
                for (std::size_t b = a + 1; b < after.size(); ++b)
                {
                    const auto aEnd = after[a].startSeconds + after[a].durationSeconds;
                    const auto bEnd = after[b].startSeconds + after[b].durationSeconds;
                    if (after[a].startSeconds < bEnd - 1.0e-9
                        && after[b].startSeconds < aEnd - 1.0e-9) ++overlaps;
                }
            const auto noOverlaps = overlaps == 0;

            // The two rules on their own, at their edges.
            using Span = ProjectModel::NoteSpan;
            const std::vector<Span> abutting { { 0.0, 1.0 }, { 1.0, 1.0 } };
            const auto throughARun = near(
                ProjectModel::firstFreeStartFrom(0.5, abutting), 2.0);
            const auto freeStaysPut = near(
                ProjectModel::firstFreeStartFrom(2.5, abutting), 2.5);
            const auto neverShorterThanAUnit = near(
                PianoRollComponent::drawnLengthFor(-5.0, 0.1, 10.0), 0.1);
            const auto roomWins = near(
                PianoRollComponent::drawnLengthFor(100.0, 0.1, 0.25), 0.25);

            const auto ok = drawingStarted && nothingYet && oneUnitWhilePressed
                && madeOne && pressAloneIsOneUnit && sixWhilePressed
                && dragSizesIt && startedAfterIt && occupiedCellStartsAfter
                && stopsAtTheNext && noOverlaps && throughARun && freeStaysPut
                && neverShorterThanAUnit && roomWins;
            std::cout << "drawing_started=" << (drawingStarted ? 1 : 0)
                      << "|nothing_written_while_pressed=" << (nothingYet ? 1 : 0)
                      << "|one_unit_while_pressed=" << (oneUnitWhilePressed ? 1 : 0)
                      << "|release_makes_one=" << (madeOne ? 1 : 0)
                      << "|press_alone_is_one_unit=" << (pressAloneIsOneUnit ? 1 : 0)
                      << "|six_units_while_pressed=" << (sixWhilePressed ? 1 : 0)
                      << "|drag_sizes_the_note=" << (dragSizesIt ? 1 : 0)
                      << "|occupied_press_starts_after=" << (startedAfterIt ? 1 : 0)
                      << "|and_the_note_lands_there=" << (occupiedCellStartsAfter ? 1 : 0)
                      << "|stops_at_the_next=" << (stopsAtTheNext ? 1 : 0)
                      << "|no_overlaps=" << (noOverlaps ? 1 : 0)
                      << "|start_walks_a_run=" << (throughARun ? 1 : 0)
                      << "|free_start_stays=" << (freeStaysPut ? 1 : 0)
                      << "|never_shorter_than_a_unit=" << (neverShorterThanAUnit ? 1 : 0)
                      << "|room_wins=" << (roomWins ? 1 : 0)
                      << "|unit_seconds=" << unit
                      << std::endl;
            for (const auto& note : after)
                std::cout << "  midi=" << note.midiNote
                          << " start=" << note.startSeconds
                          << " end=" << (note.startSeconds + note.durationSeconds)
                          << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-draw-overlap")
        {
            // Free draw used to make notes that overlapped in time, two ways.
            // Clicking a row where nothing was drawn but where another note
            // was already sounding made a note inside it.  And the snap
            // rounded to the NEAREST grid line, so two clicks either side of
            // one line both landed on it, stacking two notes exactly.
            //
            // This is the non-UTAU tool, which still makes its note on the
            // click.  The UTAU one draws by press, drag and release, and has
            // its own check; the track is moved off UTAU below so that this
            // one keeps testing the branch it was written for.
            //
            // Driven through the component's own mouseDown, so the snapping,
            // the rule, and the wiring between them are all in the picture.
            //
            // The fixture leaves a real hole: a sung second, a silent second,
            // then half a second sung.  Without the hole every click would be
            // refused for the right reason by accident.
            I18n strings;
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-draw-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=draw\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#0001]\r\nLength=1920\r\nLyric=R\r\nNoteNum=60\r\n"
                                "[#0002]\r\nLength=960\r\nLyric=i\r\nNoteNum=62\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto clipId = project.snapshot().tracks.front().clips.front().id;
            project.setTrackPitchAlgorithm(project.snapshot().tracks.front().id,
                                           PitchAlgorithm::mld5);
            const auto notesNow = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            const auto notesBefore = notesNow();

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(project.snapshot().tracks.front().id);
            roll.setFocusedClip(clipId);
            roll.setTool(PianoRollComponent::Tool::draw);

            // A click, as the roll receives one: 58 pixels of keyboard, then
            // time, and the middle of the pitch row.
            const auto clickAt = [&roll](double seconds, float midi)
            {
                const juce::Point<float> where(
                    58.0f + static_cast<float>(seconds) * 200.0f,
                    roll.diagnosticYForMidi(midi));
                const juce::MouseEvent event(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &roll, &roll, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false);
                roll.mouseDown(event);
            };
            const auto overlapsIn = [](const std::vector<NoteData>& notes)
            {
                auto count = 0;
                for (std::size_t a = 0; a < notes.size(); ++a)
                    for (std::size_t b = a + 1; b < notes.size(); ++b)
                    {
                        const auto aEnd = notes[a].startSeconds + notes[a].durationSeconds;
                        const auto bEnd = notes[b].startSeconds + notes[b].durationSeconds;
                        if (notes[a].startSeconds < bEnd - 1.0e-9
                            && notes[b].startSeconds < aEnd - 1.0e-9) ++count;
                    }
                return count;
            };
            // Found by the pitch it was drawn at, not by where it landed:
            // the grid is whatever the roll's own setting says, and hard-coding
            // a cell size here would only test my arithmetic against itself.
            const auto notePitched = [](const std::vector<NoteData>& notes, float midi)
            {
                for (const auto& note : notes)
                    if (std::abs(note.midiNote - midi) < 0.01f) return note;
                return NoteData{};
            };
            const auto covers = [](const NoteData& note, double seconds)
            {
                return note.durationSeconds > 0.0
                    && note.startSeconds <= seconds + 1.0e-9
                    && seconds < note.startSeconds + note.durationSeconds + 1.0e-9;
            };

            // The hole is where it should be, or nothing below means anything.
            const auto fixtureReaches = notesBefore.size() == 2
                && std::abs(notesBefore[0].startSeconds) < 1.0e-6
                && std::abs(notesBefore[0].durationSeconds - 1.0) < 1.0e-6
                && std::abs(notesBefore[1].startSeconds - 3.0) < 1.0e-6;

            // 1. Over the first note, a fifth above it: an empty row, so the
            //    click reaches the note-making branch rather than the note.
            clickAt(0.5, 67.0f);
            const auto refusedOverExisting = notesNow().size() == 2;

            // 2 and 3.  Two clicks in the silence.  Each must make a note
            //    that CONTAINS the moment clicked -- the property the old snap
            //    broke: rounding to the nearest line moved a click in the
            //    right half of a cell into the next one, so the note appeared
            //    somewhere the pointer had never been, and two clicks either
            //    side of a line landed on the same place.
            clickAt(1.4, 64.0f);
            const auto firstMadeOne = notesNow().size() == 3;
            clickAt(1.6, 69.0f);
            const auto madeTwo = notesNow().size() == 4;
            const auto first = notePitched(notesNow(), 64.0f);
            const auto second = notePitched(notesNow(), 69.0f);
            const auto landedUnderThePointer = covers(first, 1.4) && covers(second, 1.6);
            // Which also says the clicks hit the rows they were aimed at: a
            // note at neither pitch would leave both of these empty.
            const auto hitTheRightRows = first.durationSeconds > 0.0
                && second.durationSeconds > 0.0;

            // 4. The very same spot again, with no message loop in between --
            //    so the view's snapshot is a note out of date.  The first
            //    version of this fix asked the view and made an overlap here.
            clickAt(1.4, 71.0f);
            const auto sameCellRefused = notesNow().size() == 4;

            const auto after = notesNow();
            const auto noOverlaps = overlapsIn(after) == 0;

            // And the rule itself, at its edges.
            using Span = ProjectModel::NoteSpan;
            const std::vector<Span> one { { 1.0, 1.0 } };
            const auto beforeIt = ProjectModel::plannedNoteFor(0.5, 1.0, 4.0, one);
            const auto stopsAtTheNext = beforeIt.create
                && std::abs(beforeIt.durationSeconds - 0.5) < 1.0e-9;
            const auto onIt = ProjectModel::plannedNoteFor(1.0, 0.5, 4.0, one);
            const auto startTakenRefused = !onIt.create;
            const auto justAfter = ProjectModel::plannedNoteFor(2.0, 0.5, 4.0, one);
            const auto abuttingAllowed = justAfter.create;
            const auto pastEnd = ProjectModel::plannedNoteFor(4.0, 0.5, 4.0, {});
            const auto pastEndRefused = !pastEnd.create;
            // A hole too small to fill without the model widening it back to
            // its own floor, which would put the overlap straight back.
            const std::vector<Span> tight { { 0.005, 1.0 } };
            const auto sliver = ProjectModel::plannedNoteFor(0.0, 0.5, 4.0, tight);
            const auto sliverRefused = !sliver.create;

            const auto ok = fixtureReaches && refusedOverExisting
                && firstMadeOne && madeTwo
                && landedUnderThePointer && hitTheRightRows && sameCellRefused
                && noOverlaps && stopsAtTheNext && startTakenRefused
                && abuttingAllowed && pastEndRefused && sliverRefused;
            std::cout << "fixture_reaches=" << (fixtureReaches ? 1 : 0)
                      << "|refused_over_existing=" << (refusedOverExisting ? 1 : 0)
                      << "|first_click_makes_one=" << (firstMadeOne ? 1 : 0)
                      << "|two_clicks_two_notes=" << (madeTwo ? 1 : 0)
                      << "|landed_under_the_pointer=" << (landedUnderThePointer ? 1 : 0)
                      << "|hit_the_right_rows=" << (hitTheRightRows ? 1 : 0)
                      << "|same_cell_twice_makes_one=" << (sameCellRefused ? 1 : 0)
                      << "|no_overlaps=" << (noOverlaps ? 1 : 0)
                      << "|stops_at_the_next=" << (stopsAtTheNext ? 1 : 0)
                      << "|taken_start_refused=" << (startTakenRefused ? 1 : 0)
                      << "|abutting_allowed=" << (abuttingAllowed ? 1 : 0)
                      << "|past_clip_end_refused=" << (pastEndRefused ? 1 : 0)
                      << "|sliver_refused=" << (sliverRefused ? 1 : 0)
                      << "|notes=" << after.size()
                      << std::endl;
            for (const auto& note : after)
                std::cout << "  midi=" << note.midiNote
                          << " start=" << note.startSeconds
                          << " end=" << (note.startSeconds + note.durationSeconds)
                          << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-dropdown-arrow")
        {
            // The 显示 button now wears the mode switcher's chevron, and its
            // label has to stay clear of it.  Both are measured off the painted
            // picture rather than worked out from the numbers in the layout
            // code, because it is exactly those numbers that would be wrong.
            Palette::applyTheme("dark", juce::Colour(0xff7f69ca),
                                juce::Colour(0xffcbcbfa), juce::Colour(0xff7f69ca));
            HachiLookAndFeel look;

            const auto paint = [](juce::Button& button)
            {
                juce::Image image(juce::Image::RGB, button.getWidth(), button.getHeight(), true);
                juce::Graphics graphics(image);
                button.paintEntireComponent(graphics, true);
                return image;
            };
            // Every column where the two pictures differ; the ink of whatever
            // one of them draws and the other does not.
            const auto inkedColumns = [](const juce::Image& left, const juce::Image& right)
            {
                std::vector<int> columns;
                for (int x = 0; x < left.getWidth(); ++x)
                    for (int y = 0; y < left.getHeight(); ++y)
                        if (left.getPixelAt(x, y) != right.getPixelAt(x, y))
                        {
                            columns.push_back(x);
                            break;
                        }
                return columns;
            };

            const auto width = 72, height = 26;
            DropdownButton labelled;
            labelled.setLookAndFeel(&look);
            labelled.setButtonText(juce::String::fromUTF8("显示"));
            labelled.setBounds(0, 0, width, height);

            DropdownButton bare;
            bare.setLookAndFeel(&look);
            bare.setBounds(0, 0, width, height);

            juce::TextButton plain;
            plain.setLookAndFeel(&look);
            plain.setBounds(0, 0, width, height);

            const auto labelledImage = paint(labelled);
            const auto bareImage = paint(bare);
            const auto plainImage = paint(plain);

            // The arrow alone: the difference between an empty dropdown button
            // and an empty ordinary one.
            const auto arrowColumns = inkedColumns(bareImage, plainImage);
            const auto arrowDrawn = !arrowColumns.empty();
            const auto arrowLeft = arrowDrawn ? arrowColumns.front() : 0;
            const auto arrowRight = arrowDrawn ? arrowColumns.back() : 0;

            // The text alone: the difference the label makes to the same button.
            const auto textColumns = inkedColumns(labelledImage, bareImage);
            const auto textDrawn = !textColumns.empty();
            const auto textRight = textDrawn ? textColumns.back() : 0;

            const auto clearOfEachOther = arrowDrawn && textDrawn
                && textRight < arrowLeft;
            // And the arrow sits at the right-hand end, where a dropdown's
            // does, rather than anywhere that happens to be clear of the text.
            const auto arrowOnTheRight = arrowDrawn && arrowLeft > width / 2;

            // The rule the painting follows, asked directly: the text area must
            // not reach into the arrow's columns.
            const auto area = DropdownButton::textAreaFor({ 0, 0, width, height });
            const auto areaStopsShort = area.getRight() <= arrowLeft;

            const auto ok = arrowDrawn && textDrawn && clearOfEachOther
                && arrowOnTheRight && areaStopsShort;
            std::cout << "arrow_drawn=" << (arrowDrawn ? 1 : 0)
                      << "|text_drawn=" << (textDrawn ? 1 : 0)
                      << "|clear_of_each_other=" << (clearOfEachOther ? 1 : 0)
                      << "|arrow_on_the_right=" << (arrowOnTheRight ? 1 : 0)
                      << "|text_area_stops_short=" << (areaStopsShort ? 1 : 0)
                      << "|text_right=" << textRight
                      << "|arrow=" << arrowLeft << ".." << arrowRight
                      << "|text_area_right=" << area.getRight()
                      << std::endl;
            labelled.setLookAndFeel(nullptr);
            bare.setLookAndFeel(nullptr);
            plain.setLookAndFeel(nullptr);
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-pitch-line")
        {
            // On pixels, like the waveform switch: the settings and the menu
            // are checked as data elsewhere, and what none of that can see is
            // whether the flag reaches the paint path at all.
            //
            // The second thing this has to catch is gating too much.  A switch
            // wired one block too high would blank the notes as well, and a
            // bare "the picture changed" check would call that a pass.  So the
            // notes are checked to still be there with the line hidden.
            I18n strings;
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-pitch-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=pitch\r\n"
                                "[#0000]\r\nLength=480\r\nLyric=R\r\nNoteNum=60\r\n"
                                "[#0001]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            if (!project.addUstFile(ust, ustError, ustWarnings))
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            ust.deleteFile();
            const auto trackId = project.snapshot().tracks.front().id;
            const auto note = project.snapshot().tracks.front().clips.front().notes.front();

            // A line that visibly goes somewhere, so hiding it is unmissable:
            // a fifth up across the note and back down.
            std::vector<PitchCurveEditPoint> anchors;
            for (const auto [when, midi] : { std::pair<double, float>{ 0.0, 60.0f },
                                             { 0.5, 67.0f },
                                             { 1.0, 60.0f } })
            {
                PitchCurveEditPoint point;
                point.timeSeconds = note.durationSeconds * when;
                point.targetMidi = midi;
                anchors.push_back(point);
            }
            const auto curveSet = project.setNotePitchCurve(note.id, anchors, true);

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 900, 500);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);

            const auto paint = [&roll]
            {
                juce::Image image(juce::Image::RGB, roll.getWidth(), roll.getHeight(), true);
                juce::Graphics graphics(image);
                roll.paintEntireComponent(graphics, true);
                return image;
            };
            const auto differing = [](const juce::Image& left, const juce::Image& right)
            {
                auto count = 0;
                for (int y = 0; y < left.getHeight(); ++y)
                    for (int x = 0; x < left.getWidth(); ++x)
                        if (left.getPixelAt(x, y) != right.getPixelAt(x, y)) ++count;
                return count;
            };
            // How much of the picture is not just the backdrop.  The corner is
            // the backdrop by construction -- no note or line reaches it.
            const auto inked = [](const juce::Image& image)
            {
                const auto background = image.getPixelAt(0, 0);
                auto count = 0;
                for (int y = 0; y < image.getHeight(); ++y)
                    for (int x = 0; x < image.getWidth(); ++x)
                        if (image.getPixelAt(x, y) != background) ++count;
                return count;
            };

            // On is the state it starts in.
            const auto startsOn = roll.showsPitchLine();
            const auto onImage = paint();

            roll.setShowPitchLine(false);
            const auto offImage = paint();
            const auto hidesSomething = differing(onImage, offImage) > 0;

            roll.setShowPitchLine(true);
            const auto onAgain = paint();
            const auto comesBack = differing(onImage, onAgain) == 0;

            // The reference: the same project, same note, no pitch curve on
            // it.  Hiding the line should give back exactly that picture --
            // everything else still drawn, the line gone.
            //
            // Two weaker references were tried and both let a real break
            // through.  "The same roll with nothing focused" paints the notes
            // anyway.  "An empty project" only asks whether two pictures
            // differ, which a blank one satisfies as readily as a correct one:
            // an early return at the top of paint scored a pass against it.
            ProjectModel plainProject;
            const auto plainUst = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-plain-" + juce::Uuid().toDashedString() + ".ust");
            plainUst.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                     "Tempo=120.00\r\nTracks=1\r\nProjectName=pitch\r\n"
                                     "[#0000]\r\nLength=480\r\nLyric=R\r\nNoteNum=60\r\n"
                                     "[#0001]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                     "[#TRACKEND]\r\n");
            juce::String plainError;
            juce::StringArray plainWarnings;
            const auto plainBuilt = plainProject.addUstFile(plainUst, plainError, plainWarnings);
            plainUst.deleteFile();
            PianoRollComponent plain(plainProject, strings);
            plain.setBounds(0, 0, 900, 500);
            plain.setPixelsPerSecond(200.0f);
            plain.setFocusedTrack(plainProject.snapshot().tracks.front().id);
            const auto paintPlain = [&plain]
            {
                juce::Image image(juce::Image::RGB, plain.getWidth(), plain.getHeight(), true);
                juce::Graphics graphics(image);
                plain.paintEntireComponent(graphics, true);
                return image;
            };
            const auto plainImage = paintPlain();
            plain.setShowPitchLine(false);
            const auto plainOffImage = paintPlain();

            const auto lineCost = differing(onImage, offImage);
            // The curve has to be doing something, or the whole fixture is
            // vacuous and every assertion below passes for free.
            const auto curveCost = differing(onImage, plainImage);
            const auto fixtureIsReal = plainBuilt && curveCost > 0;
            // With the line hidden, the swept note and the flat one are the
            // same picture: the curve's only effect is the line itself.
            //
            // The reference has to be the plain roll with the line hidden too,
            // not with it shown.  A note with no stored curve still draws a
            // flat line -- measured, exactly 400 pixels: two pixels thick
            // across the 200 the note is wide -- so comparing against that
            // picture would demand the switch leave a line behind.
            const auto offMatchesNoCurve = differing(offImage, plainOffImage);
            const auto hidesOnlyTheLine = offMatchesNoCurve == 0;
            // And the roll is still a roll.  Everything above compares two
            // pictures for difference, which two blank pictures satisfy
            // perfectly: an early return at the top of paint passed the whole
            // check until this line was added.
            const auto stillPaintsTheRoll = inked(offImage) > 0;

            const auto ok = curveSet && startsOn && hidesSomething && comesBack
                && fixtureIsReal && hidesOnlyTheLine && stillPaintsTheRoll;
            std::cout << "curve_set=" << (curveSet ? 1 : 0)
                      << "|starts_on=" << (startsOn ? 1 : 0)
                      << "|hiding_changes_the_picture=" << (hidesSomething ? 1 : 0)
                      << "|comes_back_identical=" << (comesBack ? 1 : 0)
                      << "|fixture_is_real=" << (fixtureIsReal ? 1 : 0)
                      << "|hides_only_the_line=" << (hidesOnlyTheLine ? 1 : 0)
                      << "|still_paints_the_roll=" << (stillPaintsTheRoll ? 1 : 0)
                      << "|line_pixels=" << lineCost
                      << "|curve_pixels=" << curveCost
                      << "|off_vs_no_curve_off=" << offMatchesNoCurve
                      << "|inked_with_line_off=" << inked(offImage)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-view-menu")
        {
            // The two view switches now live in a dropdown instead of a pair
            // of buttons.  What that move can quietly break is the settings:
            // the keys have to stay the ones the buttons wrote, or everyone
            // who had the range box switched off gets it back and no error
            // says why.  So the check writes the historical names by hand and
            // reads them through the new path.
            juce::PropertySet saved;
            saved.setValue("ui.showNoteRange", false);
            saved.setValue("ui.showEnvelope", true);
            saved.setValue("ui.showUtauWaveforms", true);
            saved.setValue("ui.showPitchLine", false);
            const auto restored = MainComponent::viewOptionsFrom(saved);
            const auto readsOldSettings = !restored.noteRange && restored.envelope
                && restored.utauWaveform && !restored.pitchLine;

            // A fresh install: the box on, the envelope off, as before.
            juce::PropertySet fresh;
            const auto nativeEnvelopeDefault = MainComponent::viewOptionsFrom(fresh).envelope;
            // Test toggling from an explicitly hidden envelope as well as the
            // new first-run default, which shows the imported fade shape.
            fresh.setValue("ui.nativeEnvelope", false);
            const auto defaults = MainComponent::viewOptionsFrom(fresh);
            const auto defaultsKept = nativeEnvelopeDefault && defaults.noteRange && !defaults.envelope
                && !defaults.utauWaveform && defaults.pitchLine;

            // Each item flips its own switch and leaves the other alone.
            const auto first = MainComponent::afterViewMenuChoice(defaults, 1);
            const auto firstIsRange = !first.noteRange && !first.envelope
                && !first.utauWaveform && first.pitchLine;
            const auto second = MainComponent::afterViewMenuChoice(defaults, 2);
            const auto secondIsEnvelope = second.noteRange && second.envelope
                && !second.utauWaveform && second.pitchLine;
            const auto third = MainComponent::afterViewMenuChoice(defaults, 3);
            const auto thirdIsWaveform = third.noteRange && !third.envelope
                && third.utauWaveform && third.pitchLine;
            const auto fourth = MainComponent::afterViewMenuChoice(defaults, 4);
            const auto fourthIsPitchLine = fourth.noteRange && !fourth.envelope
                && !fourth.utauWaveform && !fourth.pitchLine;

            // The waveform item is the only one with nothing to draw outside a
            // UTAU mode, so it is the only one that greys out.  Getting this
            // backwards leaves the switch permanently unreachable, and a menu
            // item that is always grey looks like a feature that is simply
            // missing -- nothing would say otherwise.
            const auto greyRuleHolds =
                MainComponent::viewMenuItemEnabled(1, false)
                && MainComponent::viewMenuItemEnabled(2, false)
                && !MainComponent::viewMenuItemEnabled(3, false)
                && MainComponent::viewMenuItemEnabled(4, false)
                && MainComponent::viewMenuItemEnabled(1, true)
                && MainComponent::viewMenuItemEnabled(2, true)
                && MainComponent::viewMenuItemEnabled(3, true)
                && MainComponent::viewMenuItemEnabled(4, true)
                && !MainComponent::viewMenuItemEnabled(0, true)
                && !MainComponent::viewMenuItemEnabled(5, true);
            // Dismissing the menu, and an id no item owns, change nothing.
            const auto dismissed = MainComponent::afterViewMenuChoice(defaults, 0);
            const auto stray = MainComponent::afterViewMenuChoice(defaults, 5);
            const auto nothingOnMiss =
                dismissed.noteRange == defaults.noteRange
                && dismissed.envelope == defaults.envelope
                && dismissed.utauWaveform == defaults.utauWaveform
                && dismissed.pitchLine == defaults.pitchLine
                && stray.noteRange == defaults.noteRange
                && stray.envelope == defaults.envelope
                && stray.utauWaveform == defaults.utauWaveform
                && stray.pitchLine == defaults.pitchLine;

            juce::PropertySet written;
            MainComponent::storeViewOptions(written, restored);
            const auto back = MainComponent::viewOptionsFrom(written);
            const auto roundTrips = back.noteRange == restored.noteRange
                && back.envelope == restored.envelope
                && back.utauWaveform == restored.utauWaveform
                && back.pitchLine == restored.pitchLine;

            const auto ok = readsOldSettings && defaultsKept && firstIsRange
                && secondIsEnvelope && thirdIsWaveform && fourthIsPitchLine
                && greyRuleHolds && nothingOnMiss && roundTrips;
            std::cout << "reads_old_settings=" << (readsOldSettings ? 1 : 0)
                      << "|defaults_kept=" << (defaultsKept ? 1 : 0)
                      << "|first_item_is_range=" << (firstIsRange ? 1 : 0)
                      << "|second_item_is_envelope=" << (secondIsEnvelope ? 1 : 0)
                      << "|third_item_is_waveform=" << (thirdIsWaveform ? 1 : 0)
                      << "|fourth_item_is_pitch_line=" << (fourthIsPitchLine ? 1 : 0)
                      << "|waveform_greys_outside_utau=" << (greyRuleHolds ? 1 : 0)
                      << "|nothing_on_dismiss_or_stray_id=" << (nothingOnMiss ? 1 : 0)
                      << "|round_trips=" << (roundTrips ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--inspect-melodyne-tracks")
        {
            juce::String error;
            const auto tracks = backend::MelodyneImporter::inspectTracks(
                juce::File(arguments[1]), error);
            std::cout << juce::JSON::toString(tracks) << std::endl;
            if (error.isNotEmpty()) std::cerr << error << std::endl;
            setApplicationReturnValue(error.isEmpty() ? 0 : 2);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-melodyne-provider")
        {
            const auto installation = backend::MelodyneProvider::detect();
            const auto vst3 = backend::MelodyneProvider::probeVst3();
            const auto detected = installation.has_value();
            const auto candidate = detected && installation->isUsableCandidate();
            const auto disabled = !backend::MelodyneProvider::nativeImportAvailable()
                && !backend::MelodyneProvider::nativeRenderAvailable()
                && backend::MelodyneProvider::experimentalSelfImportEnabled()
                && !backend::MelodyneProvider::experimentalMergedRenderEnabled();
            std::cout << "detected=" << (detected ? 1 : 0)
                      << "|candidate=" << (candidate ? 1 : 0)
                      << "|native_import=" << (backend::MelodyneProvider::nativeImportAvailable() ? 1 : 0)
                      << "|native_render=" << (backend::MelodyneProvider::nativeRenderAvailable() ? 1 : 0)
                      << "|ui_test_import_policy=" << (disabled ? 1 : 0)
                      << "|vst3_candidate=" << (vst3.candidateFound ? 1 : 0)
                      << "|vst3_host_supported=" << (vst3.hostPlatformSupported ? 1 : 0)
                      << "|vst3_described=" << (vst3.pluginDescribed ? 1 : 0)
                      << "|vst3_detail=" << vst3.detail
                      << "|status=" << backend::MelodyneProvider::statusText()
                      << std::endl;
            setApplicationReturnValue(candidate && disabled ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--probe-melodyne-vst3-instance")
        {
            backend::MelodyneProvider::createVst3InstanceAsync(44100.0, 512,
                [this](std::unique_ptr<juce::AudioPluginInstance> instance, juce::String error)
                {
                    std::cout << "instance_created=" << (instance ? 1 : 0)
                              << "|error=" << error << std::endl;
                    setApplicationReturnValue(instance ? 0 : 4);
                    juce::MessageManager::callAsync([this] { quit(); });
                });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-native-timing")
        {
            const auto file = juce::File::createTempFile(".wav");
            file.create();
            ProjectData data;
            TrackData track;
            track.id = "timing-track";
            track.pitchAlgorithm = PitchAlgorithm::world;
            ClipData clip;
            clip.id = "timing-clip";
            clip.sourceFile = file;
            clip.sourceOffsetSeconds = 10.0;
            clip.sourceDurationSeconds = 2.0;
            clip.durationSeconds = 1.0;
            clip.sourceTimeMap = { { 0.0, 0.0 }, { 0.2, 0.4 }, { 1.0, 2.0 } };
            NoteData note;
            note.id = "timing-note";
            note.durationSeconds = 1.0;
            note.consonantSeconds = 0.2;
            note.attackSpeed = 1.0f;
            note.contour = { { 0.0, 0.0f, 0.0f, false },
                { 0.4, 10.0f, 10.0f, true }, { 0.6, 20.0f, 20.0f, true } };
            note.amplitudeEnvelope = { { 0.0, -60.0f }, { 0.2, 0.0f }, { 1.0, -60.0f } };
            clip.notes.push_back(note);
            track.clips.push_back(clip);
            data.tracks.push_back(track);
            juce::StringArray warnings;
            const auto converted = SampleSettings::convertMelodyneProject(data, warnings);
            const auto rows = SampleSettings::loadOrDerive(file, {});
            const auto boundaries = rows.size() == 1 && rows[0].segments.size() == 3
                && std::abs(rows[0].alignmentSeconds - 10.4) < 1.0e-6
                && rows[0].fixedDurationSeconds > 0.4
                && rows[0].segments[0].role == NativeSegmentRole::consonant
                && rows[0].segments[1].role == NativeSegmentRole::vowel;
            ProjectModel model;
            model.replace(data);
            model.setNotesUtauConsonantVelocity({ note.id }, 200);
            const auto updated = model.snapshot().tracks.front().clips.front();
            const auto& moved = updated.notes.front();
            const auto timing = std::abs(moved.consonantSeconds - 0.1) < 1.0e-6
                && std::abs(moved.durationSeconds - 1.0) < 1.0e-6
                && std::abs(updated.sourceOffsetSeconds - 10.0) < 1.0e-6
                && std::abs(updated.sourceTimeMap[1].targetSeconds - 0.1) < 1.0e-6
                && std::abs(updated.sourceTimeMap[1].sourceSeconds - 0.4) < 1.0e-6
                && std::abs(moved.contour[1].relativeCents - 10.0f) < 1.0e-6f
                && std::abs(moved.amplitudeEnvelope[1].timeSeconds - 0.1) < 1.0e-6;
            model.undo();
            const auto restored = std::abs(model.snapshot().tracks.front().clips.front()
                .notes.front().consonantSeconds - 0.2) < 1.0e-6;
            file.deleteFile();
            SampleSettings::sidecarFor(file).deleteFile();
            std::cout << "converted=" << converted << "|separate_boundaries=" << boundaries
                      << "|velocity_remaps_time=" << timing << "|undo=" << restored << std::endl;
            setApplicationReturnValue(converted && boundaries && timing && restored ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-native-hjm")
        {
            const auto audio = juce::File::createTempFile("hachi-native-hjm");
            const auto projectFile = juce::File::createTempFile("hachi-native-project")
                .withFileExtension("hjpx");
            SampleRegionSetting row;
            row.name = "-";
            row.role = NativeSegmentRole::unknown;
            row.provenance = "melodyne";
            row.confidence = 0.0f;
            row.regionStartSeconds = 0.0;
            row.regionEndSeconds = 0.6;
            row.alignmentSeconds = 0.1;
            row.overlapSeconds = 0.025;
            row.amplitudeEnvelope = { { 0.0, -60.0f }, { 0.08, -2.0f },
                { 0.5, -2.0f }, { 0.6, -18.0f } };
            row.segments = {
                { "s1", "_", NativeSegmentRole::transition, 0.0, 0.1,
                  "estimated", 0.5f, 0.1, 0.025, false, 0.5 },
                { "s2", "-", NativeSegmentRole::unknown, 0.1, 0.3,
                  "melodyne", 0.0f, 0.1, 0.025, true, 1.0 },
                { "s3", "-", NativeSegmentRole::unknown, 0.3, 0.6,
                  "melodyne", 0.0f, 0.1, 0.025, true, 1.0 }
            };
            juce::String annotationError;
            const auto saved = SampleSettings::save(audio, { row }, annotationError);
            const auto loaded = SampleSettings::loadOrDerive(audio, ProjectData{});
            const auto parsed = loaded.size() == 1 && loaded.front().hjmVersion >= 2
                && loaded.front().segments.size() == 3
                && loaded.front().segments[0].alias == "_"
                && loaded.front().segments[2].role == NativeSegmentRole::unknown
                && loaded.front().amplitudeEnvelope.size() == 4;
            ProjectModel model;
            const auto clipId = model.addAudioFile(audio, 0.6, 0.0, {});
            const auto native = model.snapshot().tracks.front().clips.front().notes.front();
            const auto bound = clipId.isNotEmpty() && native.label == "-"
                && native.nativeSegments.size() == 3
                && native.utauOverlapOverrideEnabled
                && native.amplitudeEnvelope.size() == 4;
            const auto projectSaved = model.save(projectFile, annotationError);
            ProjectModel reopened;
            const auto projectLoaded = reopened.load(projectFile, annotationError);
            auto roundTrip = false;
            if (projectLoaded)
            {
                const auto data = reopened.snapshot();
                roundTrip = !data.tracks.empty() && !data.tracks.front().clips.empty()
                    && !data.tracks.front().clips.front().notes.empty()
                    && data.tracks.front().clips.front().notes.front().nativeSegments.size() == 3;
            }
            audio.deleteFile();
            SampleSettings::sidecarFor(audio).deleteFile();
            projectFile.deleteFile();
            std::cout << "saved=" << (saved ? 1 : 0)
                      << "|parsed_v2=" << (parsed ? 1 : 0)
                      << "|bound_to_native_note=" << (bound ? 1 : 0)
                      << "|native_label=" << native.label
                      << "|native_segments=" << native.nativeSegments.size()
                      << "|native_overlap=" << (native.utauOverlapOverrideEnabled ? 1 : 0)
                      << "|native_amplitude=" << native.amplitudeEnvelope.size()
                      << "|project_saved=" << (projectSaved ? 1 : 0)
                      << "|project_round_trip=" << (roundTrip ? 1 : 0)
                      << "|error=" << annotationError << std::endl;
            setApplicationReturnValue(saved && parsed && bound && projectSaved && roundTrip ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-batch-lyrics")
        {
            // "a ba ca da" fills four notes in the order they are sung.  Two
            // things about that are easy to get wrong: what counts as a space
            // -- an input method set to Chinese types U+3000, not U+0020, so
            // reading only the ASCII one puts the whole line on the first
            // note -- and whether the whole line undoes in one step, since to
            // the person typing it that was one action.
            const auto split = [](const char* utf8)
            {
                return PianoRollComponent::splitBatchLyrics(
                    juce::String::fromUTF8(utf8));
            };
            const auto plain = split("a ba ca da");
            const auto splitsOnSpaces = plain.size() == 4 && plain[0] == "a"
                && plain[1] == "ba" && plain[2] == "ca" && plain[3] == "da";
            // Runs of spaces, and space at either end, are not empty lyrics.
            const auto messy = split("  a   ba  ");
            const auto ignoresRuns = messy.size() == 2 && messy[0] == "a"
                && messy[1] == "ba";
            // The full-width space.
            // The character itself rather than its bytes: \x in C++ eats every
            // hex digit after it, so "\x80ba" is one number, not a byte and a b.
            const auto wide = split("a　ba　ca");
            const auto splitsFullWidth = wide.size() == 3 && wide[0] == "a"
                && wide[1] == "ba" && wide[2] == "ca";
            // Newlines and tabs separate too, so a line pasted from a lyric
            // sheet does not arrive as one word.
            const auto pasted = split("a\nba\tca");
            const auto splitsPasted = pasted.size() == 3;
            const auto nothing = split("   ").isEmpty();

            // And the model side: four notes, filled in order, in one step.
            I18n strings;
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-lyrics-" + juce::Uuid().toDashedString() + ".ust");
            juce::String body = "[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=lyrics\r\n";
            for (int index = 0; index < 4; ++index)
                body += "[#000" + juce::String(index) + "]\r\nLength=480\r\n"
                        "Lyric=zz\r\nNoteNum=60\r\n";
            body += "[#TRACKEND]\r\n";
            ust.replaceWithText(body);
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto notesOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            const auto before = notesOf();
            std::vector<std::pair<juce::String, juce::String>> labels;
            for (int index = 0; index < 4; ++index)
                labels.emplace_back(before[static_cast<std::size_t>(index)].id,
                                    plain[index]);
            project.setNoteLabels(labels);

            const auto after = notesOf();
            auto inOrder = after.size() == 4;
            for (std::size_t index = 0; index < after.size() && inOrder; ++index)
                if (after[index].label != plain[static_cast<int>(index)]) inOrder = false;

            // One Ctrl+Z takes the whole line back, not one syllable.
            project.undo();
            const auto undone = notesOf();
            auto oneStep = undone.size() == 4;
            for (const auto& note : undone)
                if (note.label != "zz") oneStep = false;

            const auto ok = splitsOnSpaces && ignoresRuns && splitsFullWidth
                && splitsPasted && nothing && inOrder && oneStep;
            std::cout << "splits_on_spaces=" << (splitsOnSpaces ? 1 : 0)
                      << "|runs_and_edges_ignored=" << (ignoresRuns ? 1 : 0)
                      << "|full_width_space_splits=" << (splitsFullWidth ? 1 : 0)
                      << "|newline_and_tab_split=" << (splitsPasted ? 1 : 0)
                      << "|blank_gives_nothing=" << (nothing ? 1 : 0)
                      << "|fills_notes_in_order=" << (inOrder ? 1 : 0)
                      << "|undoes_in_one_step=" << (oneStep ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-envelope-lanes")
        {
            // The loudness envelope and the flag envelope share one strip at
            // the bottom of the roll, and used to be able to believe they were
            // both open: nothing cleared the other one's button, so a drag
            // went to whichever the roll's tool happened to be while the
            // buttons showed something else.  Only one may be open at a time.
            using Lane = MainComponent::EnvelopeLane;
            const auto next = [](Lane open, Lane clicked)
            {
                return MainComponent::nextEnvelopeLane(open, clicked);
            };

            // Opening one from nothing.
            const auto opensAmplitude = next(Lane::none, Lane::amplitude) == Lane::amplitude;
            const auto opensFlag = next(Lane::none, Lane::flagCurve) == Lane::flagCurve;

            // Clicking the open one again closes it.
            const auto amplitudeCloses = next(Lane::amplitude, Lane::amplitude) == Lane::none;
            const auto flagCloses = next(Lane::flagCurve, Lane::flagCurve) == Lane::none;

            // The reported bug: opening one while the other is open must leave
            // exactly one open, and it must be the one just clicked.
            const auto flagReplacesAmplitude =
                next(Lane::amplitude, Lane::flagCurve) == Lane::flagCurve;
            const auto amplitudeReplacesFlag =
                next(Lane::flagCurve, Lane::amplitude) == Lane::amplitude;

            // Whatever is open, no click can produce a state that is both.
            // The enum cannot represent that -- which is the point of it being
            // an enum rather than two booleans that could disagree.
            auto onlyEverOne = true;
            for (const auto open : { Lane::none, Lane::amplitude, Lane::flagCurve })
                for (const auto clicked : { Lane::none, Lane::amplitude, Lane::flagCurve })
                {
                    const auto result = next(open, clicked);
                    if (result != Lane::none && result != Lane::amplitude
                        && result != Lane::flagCurve)
                        onlyEverOne = false;
                    // Clicking nothing changes nothing into nothing; every
                    // other click lands on the clicked lane or closes it.
                    if (clicked != Lane::none && result != Lane::none && result != clicked)
                        onlyEverOne = false;
                }

            const auto ok = opensAmplitude && opensFlag && amplitudeCloses && flagCloses
                && flagReplacesAmplitude && amplitudeReplacesFlag && onlyEverOne;
            std::cout << "opens_from_nothing="
                      << (opensAmplitude && opensFlag ? 1 : 0)
                      << "|clicking_the_open_one_closes_it="
                      << (amplitudeCloses && flagCloses ? 1 : 0)
                      << "|flag_replaces_loudness=" << (flagReplacesAmplitude ? 1 : 0)
                      << "|loudness_replaces_flag=" << (amplitudeReplacesFlag ? 1 : 0)
                      << "|never_both=" << (onlyEverOne ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-wheel")
        {
            // Wheel scrolls, modifiers zoom.  Four bindings, and the two axes
            // cross over between them -- shift means horizontal when scrolling
            // and horizontal when zooming, but ctrl alone is the vertical zoom
            // -- so each is named rather than left to be read off an if-chain.
            using Wheel = MainComponent::WheelAction;
            const auto with = [](bool control, bool shift)
            {
                auto modifiers = juce::ModifierKeys();
                if (control) modifiers = modifiers.withFlags(juce::ModifierKeys::commandModifier);
                if (shift) modifiers = modifiers.withFlags(juce::ModifierKeys::shiftModifier);
                return MainComponent::wheelActionFor(modifiers);
            };
            const auto plain = with(false, false) == Wheel::scrollVertically;
            const auto shifted = with(false, true) == Wheel::scrollHorizontally;
            const auto controlled = with(true, false) == Wheel::zoomVertically;
            const auto both = with(true, true) == Wheel::zoomHorizontally;

            // Alt is the fine-edit modifier for dragging and must not be read
            // as one of these; it leaves the wheel alone.
            const auto altIgnored =
                MainComponent::wheelActionFor(
                    juce::ModifierKeys(juce::ModifierKeys::altModifier))
                        == Wheel::scrollVertically
                && MainComponent::wheelActionFor(
                    juce::ModifierKeys(juce::ModifierKeys::altModifier
                                       | juce::ModifierKeys::commandModifier))
                        == Wheel::zoomVertically;

            const auto ok = plain && shifted && controlled && both && altIgnored;
            std::cout << "wheel_scrolls_up_and_down=" << (plain ? 1 : 0)
                      << "|shift_scrolls_sideways=" << (shifted ? 1 : 0)
                      << "|ctrl_zooms_vertically=" << (controlled ? 1 : 0)
                      << "|ctrl_shift_zooms_horizontally=" << (both ? 1 : 0)
                      << "|alt_changes_nothing=" << (altIgnored ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-waveform-toggle")
        {
            // The switch, on pixels.  Everything else about this feature has
            // been checked as data; what nobody has seen is whether anything
            // is actually painted, and whether the switch really stops it.
            // So: paint the roll into an image and compare.
            I18n strings;
            ProjectModel project;
            // A UTAU track with a real note, built the way one actually
            // arrives: the model has no way to hand a clip in directly.
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-toggle-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=toggle\r\n"
                                "[#0000]\r\nLength=480\r\nLyric=R\r\nNoteNum=60\r\n"
                                "[#0001]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            if (!project.addUstFile(ust, ustError, ustWarnings))
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            ust.deleteFile();
            const auto trackId = project.snapshot().tracks.front().id;
            const auto note = project.snapshot().tracks.front().clips.front().notes.front();

            // A waveform for it, loud enough to be unmistakable on screen.
            auto waveforms = std::make_shared<std::vector<UtauNoteWaveform>>();
            UtauNoteWaveform drawn;
            drawn.noteId = note.id;
            drawn.renderHash = AudioEngine::utauNoteRenderHash(note);
            drawn.startSeconds = note.startSeconds;
            drawn.durationSeconds = note.durationSeconds;
            for (int bucket = 0; bucket < 1000; ++bucket)
            {
                const auto value = 0.9f * std::sin(static_cast<float>(bucket) * 0.3f);
                drawn.minima.push_back(-std::abs(value));
                drawn.maxima.push_back(std::abs(value));
            }
            waveforms->push_back(std::move(drawn));

            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 900, 500);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            roll.setUtauNoteWaveforms(waveforms);

            const auto paint = [&roll]
            {
                juce::Image image(juce::Image::RGB, roll.getWidth(), roll.getHeight(), true);
                juce::Graphics graphics(image);
                roll.paintEntireComponent(graphics, true);
                return image;
            };
            const auto same = [](const juce::Image& left, const juce::Image& right)
            {
                for (int y = 0; y < left.getHeight(); ++y)
                    for (int x = 0; x < left.getWidth(); ++x)
                        if (left.getPixelAt(x, y) != right.getPixelAt(x, y)) return false;
                return true;
            };

            // Off is the state it starts in, and nothing may be drawn there.
            const auto startsOff = !roll.showsUtauWaveforms();
            const auto offImage = paint();

            roll.setShowUtauWaveforms(true);
            const auto onImage = paint();
            const auto drawsSomething = !same(offImage, onImage);

            roll.setShowUtauWaveforms(false);
            const auto offAgain = paint();
            const auto switchesBackOff = same(offImage, offAgain);

            // Edited since: the waveform no longer belongs to the note under
            // it, so the switch must make no difference at all.  Compared
            // against the same edit with the switch off, not against the
            // untouched picture -- transposing the note moves the note itself,
            // which would differ for reasons that have nothing to do with this.
            project.transposeNote(note.id, 2.0f);
            // The roll takes its copy of the project from a change broadcast,
            // delivered on the message thread; offline there is nothing to
            // deliver it, so the refresh is asked for directly.
            roll.diagnosticRefresh();
            roll.setShowUtauWaveforms(false);
            const auto editedOff = paint();
            roll.setShowUtauWaveforms(true);
            const auto editedOn = paint();
            const auto editHidesIt = same(editedOff, editedOn);

            const auto ok = startsOff && drawsSomething && switchesBackOff && editHidesIt;
            std::cout << "starts_off=" << (startsOff ? 1 : 0)
                      << "|on_draws_something=" << (drawsSomething ? 1 : 0)
                      << "|off_again_is_blank=" << (switchesBackOff ? 1 : 0)
                      << "|edited_note_draws_nothing=" << (editHidesIt ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-utau-waveform")
        {
            // A UTAU note has no source file to draw a thumbnail of, so what
            // is shown is the audio that came back from the resampler -- and
            // only while the note is still the one that produced it.  Both
            // halves of that matter: audio that never appears is a feature
            // that does nothing, and audio that stays after an edit is a
            // waveform that lies about what will be heard.
            const juce::File bank(arguments[1].unquoted());
            const juce::File resampler(arguments[2].unquoted());
            const auto alias = arguments.size() >= 4 ? arguments[3].unquoted()
                                                     : juce::String("a");
            ProjectModel project;
            const auto trackId = project.addTrack("utau", true);
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            project.setTrackVoicebankDirectory(trackId, bank);

            // Two notes, so "this one changed and that one did not" is a
            // question the answer can distinguish.
            ProjectData built = project.snapshot();
            ClipData clip;
            clip.id = "clip";
            clip.startSeconds = 0.0;
            clip.durationSeconds = 5.0;
            for (int index = 0; index < 2; ++index)
            {
                NoteData note;
                note.id = "note" + juce::String(index);
                note.label = alias;
                // Well into the song on purpose.  A UTAU render trims the
                // buffer to the selection and shifts the notes it sends back
                // to zero, and a fixture that starts in the first second never
                // exercises that shift -- which is where the note the engine
                // hashes stops being the note the roll hashes.
                note.startSeconds = 2.0 + 0.7 * index;
                note.durationSeconds = 0.5;
                note.midiNote = 60.0f + 2.0f * index;
                note.sourceMidiCenter = note.midiNote;
                note.contour.push_back({ 0.0, 0.0f, 0.0f, true });
                note.contour.push_back({ note.durationSeconds, 0.0f, 0.0f, true });
                clip.notes.push_back(std::move(note));
            }
            built.tracks.front().clips.push_back(std::move(clip));

            AudioEngine engine;
            engine.setUtauResamplerFile(resampler);
            engine.prepareToPlay(512, 48'000.0);
            engine.selectEveryUtauNote(built);
            engine.syncProject(built);
            // The render runs on a pool thread; wait for it the way the window
            // does, by asking whether anything is still in flight.
            for (int tries = 0; tries < 600 && engine.renderProgress(); ++tries)
                juce::Thread::sleep(100);
            engine.refreshUtauWaveforms();
            const auto waveforms = engine.utauNoteWaveforms();

            const auto findFor = [&waveforms](const juce::String& id)
                -> const UtauNoteWaveform*
            {
                if (waveforms == nullptr) return nullptr;
                for (const auto& waveform : *waveforms)
                    if (waveform.noteId == id) return &waveform;
                return nullptr;
            };
            const auto& notes = built.tracks.front().clips.front().notes;
            const auto* first = findFor(notes[0].id);
            const auto* second = findFor(notes[1].id);
            const auto bothRendered = first != nullptr && second != nullptr;

            // Peaks, and not a flat line: a note that rendered silence has
            // nothing worth drawing and the point is to show what is there.
            auto audible = 0;
            for (const auto* found : { first, second })
                if (found != nullptr)
                {
                    auto loudest = 0.0f;
                    for (const auto value : found->maxima) loudest = std::max(loudest, value);
                    for (const auto value : found->minima) loudest = std::max(loudest, -value);
                    if (loudest > 0.01f) ++audible;
                }

            // Unedited, each waveform belongs to the note it is drawn under.
            const auto matchesBefore = bothRendered
                && first->renderHash == AudioEngine::utauNoteRenderHash(notes[0])
                && second->renderHash == AudioEngine::utauNoteRenderHash(notes[1]);

            // Edit the first note.  Its waveform must stop matching, and the
            // other note's must not: an edit hides its own note, not the song.
            auto edited = built;
            edited.tracks.front().clips.front().notes[0].midiNote += 3.0f;
            const auto& changed = edited.tracks.front().clips.front().notes;
            const auto hidesTheEdited = bothRendered
                && first->renderHash != AudioEngine::utauNoteRenderHash(changed[0]);
            const auto keepsTheOther = bothRendered
                && second->renderHash == AudioEngine::utauNoteRenderHash(changed[1]);

            // Every field the renderer reads has to count as an edit, or a
            // waveform outlives the sound it stood for.  These are the ones a
            // UTAU note is actually worked on through.
            const auto hashOf = [](NoteData note) { return AudioEngine::utauNoteRenderHash(note); };
            const auto base = hashOf(notes[0]);
            auto everyEditCounts = true;
            {
                auto note = notes[0]; note.label = alias + "x";
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                auto note = notes[0]; note.durationSeconds += 0.05;
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                auto note = notes[0]; note.utauFlags = "g5";
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                auto note = notes[0]; note.utauConsonantVelocity = 150;
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                // STP reads a different part of the recording, so the note
                // sounds different and the waveform drawn for it is stale.
                auto note = notes[0]; note.utauStpSeconds = 0.05;
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                auto note = notes[0]; note.gain = 0.5f;
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                auto note = notes[0]; note.vibratoEnabled = true;
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            {
                auto note = notes[0];
                note.pitchControlPoints.push_back({ 0.1, 62.0f });
                everyEditCounts = everyEditCounts && hashOf(note) != base;
            }
            // And an untouched copy still hashes the same, or nothing would
            // ever be drawn at all.
            const auto stableWhenUntouched = hashOf(notes[0]) == base;

            const auto ok = bothRendered && audible == 2 && matchesBefore
                && hidesTheEdited && keepsTheOther && everyEditCounts
                && stableWhenUntouched;
            std::cout << "notes_with_waveforms=" << (bothRendered ? 2 : 0)
                      << "|audible=" << audible
                      << "|match_before_editing=" << (matchesBefore ? 1 : 0)
                      << "|an_edit_hides_its_note=" << (hidesTheEdited ? 1 : 0)
                      << "|and_leaves_the_others=" << (keepsTheOther ? 1 : 0)
                      << "|every_edit_counts=" << (everyEditCounts ? 1 : 0)
                      << "|untouched_stays_the_same=" << (stableWhenUntouched ? 1 : 0)
                      << "|buckets=" << (first != nullptr ? first->maxima.size() : 0u)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-ust")
        {
            // Opening a UST.  Two things are easy to get wrong and impossible
            // to see afterwards: the tick-to-time conversion, which scales the
            // whole song and reads as a tempo bug, and the pitch bends, whose
            // values are tenths of a semitone measured from each note's own
            // pitch -- read as cents or as absolute pitch, the song is still
            // in tune at every note and wrong everywhere between them.
            const juce::File file(arguments[1].unquoted());
            ProjectModel project;
            juce::String error;
            juce::StringArray warnings;
            if (!project.addUstFile(file, error, warnings))
            {
                std::cout << "opened=0|error=" << error << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto data = project.snapshot();
            const auto& track = data.tracks.front();
            const auto& clip = track.clips.front();

            // A plain UTAU track, which is what a UST describes.
            const auto asUtau = track.pitchAlgorithm == PitchAlgorithm::utau
                && track.utauMode == UtauMode::classic && track.compose;

            // Timing: every note starts where the one before it ended, or
            // later where the UST had a rest, and never earlier.
            auto ordered = true;
            auto sounding = 0.0;
            for (std::size_t index = 0; index < clip.notes.size(); ++index)
            {
                const auto& note = clip.notes[index];
                sounding += note.durationSeconds;
                if (index > 0)
                {
                    const auto& previous = clip.notes[index - 1];
                    if (note.startSeconds < previous.startSeconds
                                            + previous.durationSeconds - 1.0e-6)
                        ordered = false;
                }
            }
            // A quarter note at this project's tempo, against a note whose
            // length the UST wrote in ticks.  480 ticks is one quarter.
            const auto quarter = 60.0 / data.bpm;
            auto lengthsMatch = true;
            for (const auto& note : clip.notes)
            {
                const auto quarters = note.durationSeconds / quarter;
                // Every length in a UST is a whole number of ticks, so every
                // duration must land on a multiple of 1/480 of a quarter.
                const auto ticks = quarters * 480.0;
                if (std::abs(ticks - std::round(ticks)) > 0.05) lengthsMatch = false;
            }

            // The pitch bends.  Their first point sits before the note starts
            // -- that is what a portamento out of the previous note is -- and
            // its pitch should be near the previous note's, because that is
            // where the singer is coming from.
            auto bent = 0, reachesBack = 0, landsOnTheNeighbour = 0;
            for (std::size_t index = 0; index < clip.notes.size(); ++index)
            {
                const auto& note = clip.notes[index];
                if (note.pitchControlPoints.size() < 2) continue;
                ++bent;
                const auto& first = note.pitchControlPoints.front();
                if (first.timeSeconds < 0.0) ++reachesBack;
                if (index > 0 && first.timeSeconds < 0.0)
                {
                    const auto previous = clip.notes[index - 1].midiNote;
                    if (std::abs(first.targetMidi - previous) < 0.51f)
                        ++landsOnTheNeighbour;
                }
            }
            // Not every bend starts on the previous note -- plenty are scoops
            // from nowhere -- but the great majority do, and the scaling is
            // what decides it.  On the reference song 138 of 152 land there
            // when the values are read as tenths of a semitone and 53 of 152
            // when they are read as cents, so two thirds separates the two
            // without being tight enough to fail on a differently sung file.
            const auto scaledRight = reachesBack > 0
                && landsOnTheNeighbour * 3 >= reachesBack * 2;

            // The per-note UTAU parameters.  These reach the resampler as
            // argv, so a value dropped on the way in is silently a different
            // performance rather than an error.
            auto withFlags = 0, withVelocity = 0, withPreutterance = 0, quiet = 0;
            for (const auto& note : clip.notes)
            {
                if (note.utauFlags.isNotEmpty()) ++withFlags;
                if (note.utauConsonantVelocity != inheritedUtauConsonantVelocity)
                    ++withVelocity;
                if (note.utauPreutteranceOverrideEnabled) ++withPreutterance;
                if (std::abs(note.gain - 1.0f) > 1.0e-4f) ++quiet;
            }
            // The envelopes, checked against the values they were read from
            // rather than against themselves.  Three things have to hold, and
            // each fails silently: the shape is anchored one preutterance
            // before the note, its end is the note's end, and the closing ramp
            // starts p3 before that.  Volumes are percentages of the note's
            // own level, so 100 has to arrive as unity and not as +40 dB.
            juce::StringArray parseWarnings;
            const auto sourceProject = backend::UstImporter::read(file, error,
                                                                  parseWarnings);
            std::vector<const backend::UstNote*> sourceNotes;
            if (sourceProject)
                for (const auto& source : sourceProject->notes)
                    if (!source.isRest()
                        && backend::UstImporter::quarterNotes(source.lengthTicks) > 0.0)
                        sourceNotes.push_back(&source);

            auto enveloped = 0, silentEnds = 0, monotonic = 0;
            auto anchoredAtLeadIn = 0, endsAtTheNote = 0, releaseAtP3 = 0, unityIsUnity = 0;
            auto comparable = 0;
            const auto paired = sourceNotes.size() == clip.notes.size();
            for (std::size_t index = 0; index < clip.notes.size() && paired; ++index)
            {
                const auto& note = clip.notes[index];
                const auto& source = *sourceNotes[index];
                const auto& envelope = note.amplitudeEnvelope;
                if (!source.hasEnvelope || envelope.size() < 3) continue;
                ++enveloped;
                if (envelope.front().gainDb <= -59.9f
                    && envelope.back().gainDb <= -59.9f) ++silentEnds;
                auto ordered = true;
                for (std::size_t step = 1; step < envelope.size(); ++step)
                    if (envelope[step].timeSeconds < envelope[step - 1].timeSeconds - 1.0e-9)
                        ordered = false;
                if (ordered) ++monotonic;

                const auto lead = note.utauPreutteranceOverrideEnabled
                    ? note.utauPreutteranceSeconds : 0.0;
                if (std::abs(envelope.front().timeSeconds + lead) < 1.0e-6)
                    ++anchoredAtLeadIn;
                if (std::abs(envelope.back().timeSeconds - note.durationSeconds) < 1.0e-6)
                    ++endsAtTheNote;
                // The closing ramp begins p3 before the end.  On a note too
                // short to hold both ramps the points are squeezed together,
                // so only the ones with room are compared.
                const auto release = note.durationSeconds - source.envelopeP3 / 1000.0;
                if (release > envelope[envelope.size() - 3].timeSeconds)
                {
                    ++comparable;
                    if (std::abs(envelope[envelope.size() - 2].timeSeconds - release) < 1.0e-6)
                        ++releaseAtP3;
                }
                // v2 is the level the note is actually sung at, and it is 100
                // almost everywhere in a UST, which must mean unity gain.
                if (std::abs(source.envelopeV2 - 100.0) < 1.0e-9)
                    if (std::abs(envelope[2].gainDb) < 1.0e-4f) ++unityIsUnity;
            }
            const auto envelopesRead = paired && enveloped > 0
                && silentEnds == enveloped && monotonic == enveloped
                && anchoredAtLeadIn == enveloped && endsAtTheNote == enveloped
                && comparable > 0 && releaseAtP3 == comparable
                && unityIsUnity > 0;

            std::cout << "envelopes=" << enveloped
                      << "|notes_pair_up=" << (paired ? 1 : 0)
                      << "|both_ends_silent=" << silentEnds
                      << "|times_in_order=" << monotonic
                      << "|anchored_one_preutterance_early=" << anchoredAtLeadIn
                      << "|ends_at_the_note_end=" << endsAtTheNote
                      << "|release_starts_at_p3=" << releaseAtP3 << "/" << comparable
                      << "|full_volume_is_unity=" << unityIsUnity
                      << "|envelopes_read=" << (envelopesRead ? 1 : 0)
                      << std::endl;
                        std::cout << "global_flags=" << track.utauGlobalFlags
                      << "|notes_with_flags=" << withFlags
                      << "|notes_with_velocity=" << withVelocity
                      << "|notes_with_preutterance=" << withPreutterance
                      << "|notes_off_unity_gain=" << quiet
                      << std::endl;
            std::cout << "opened=1|notes=" << clip.notes.size()
                      << "|bpm=" << data.bpm
                      << "|plain_utau_track=" << (asUtau ? 1 : 0)
                      << "|notes_in_order=" << (ordered ? 1 : 0)
                      << "|durations_land_on_ticks=" << (lengthsMatch ? 1 : 0)
                      << "|bent=" << bent << "|reach_back=" << reachesBack
                      << "|start_on_the_previous_note=" << landsOnTheNeighbour
                      << "|bend_scaling=" << (scaledRight ? 1 : 0)
                      << "|sounding_seconds=" << sounding
                      << "|first_lyric=" << clip.notes.front().label
                      << "|warnings=" << warnings.size()
                      << std::endl;
            for (const auto& warning : warnings)
                std::cout << "  warning: " << warning << std::endl;
            setApplicationReturnValue(asUtau && ordered && lengthsMatch
                                      && scaledRight && bent > 0
                                      && envelopesRead ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-active-resampler")
        {
            // Which UTAU engine this copy would drive, asked of a real
            // AudioEngine at the real executable's location.  Run it from
            // inside a packaged folder and it answers the question the
            // package exists to answer: does a first launch, with nothing
            // configured, find the engine that travels beside it.
            const auto here = juce::File::getSpecialLocation(
                juce::File::currentExecutableFile).getParentDirectory();
            const auto shipped = AudioEngine::bundledUtauResampler(here);

            // As constructed, before anything is configured -- which is the
            // state a machine with no settings file leaves it in.
            AudioEngine fresh;
            const auto onFirstRun = fresh.diagnosticUtauResamplerFile();

            // An empty setting, which is what an absent settings file yields.
            AudioEngine unset;
            unset.setUtauResamplerFile({});
            const auto withNoSetting = unset.diagnosticUtauResamplerFile();

            // A setting left behind by another machine, naming a file that is
            // not on this one.
            AudioEngine stale;
            stale.setUtauResamplerFile(here.getChildFile("moved-away.exe"));
            const auto withAStaleSetting = stale.diagnosticUtauResamplerFile();

            const auto found = shipped.existsAsFile();
            std::cout << "an_engine_ships_here=" << (found ? 1 : 0)
                      << "|first_run_uses_it="
                      << (found && onFirstRun == shipped ? 1 : 0)
                      << "|empty_setting_uses_it="
                      << (found && withNoSetting == shipped ? 1 : 0)
                      << "|stale_setting_uses_it="
                      << (found && withAStaleSetting == shipped ? 1 : 0)
                      << "|engine=" << (onFirstRun == juce::File{}
                                        ? juce::String("(none)")
                                        : onFirstRun.getFullPathName())
                      << std::endl;
            setApplicationReturnValue(found && onFirstRun == shipped
                                      && withNoSetting == shipped
                                      && withAStaleSetting == shipped ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-bundled-resampler")
        {
            // A portable copy carries its own UTAU engine.  Without a lookup
            // beside the executable it would sit there unused until someone
            // found the settings page, which is not what "unzip and run" means.
            const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-resampler-smoke-" + juce::Uuid().toDashedString());
            const auto engines = root.getChildFile("engines");
            engines.createDirectory();
            const auto beside = root.getChildFile("WCSNDM.exe");
            const auto inFolder = engines.getChildFile("WCSNDM.exe");
            const auto elsewhere = root.getChildFile("chosen.exe");
            const auto write = [](const juce::File& file) { file.replaceWithText("x"); };

            // Nothing anywhere: nothing to drive, and say so rather than
            // handing the renderer a path that is not there.
            const auto emptyHanded =
                AudioEngine::resolveUtauResampler({}, root) == juce::File{};

            // Beside the executable.
            write(beside);
            const auto findsItBeside =
                AudioEngine::resolveUtauResampler({}, root) == beside;

            // The engines folder is looked at first, so a copy put there wins
            // over one loose in a folder already full of DLLs.
            write(inFolder);
            const auto prefersTheFolder =
                AudioEngine::resolveUtauResampler({}, root) == inFolder;

            // A chosen engine is never quietly swapped for the bundled one.
            write(elsewhere);
            const auto settingsWin = AudioEngine::resolveUtauResampler(
                elsewhere.getFullPathName(), root) == elsewhere;
            // Quoted and padded, as a path pasted into a settings field is.
            const auto tolerantOfPasting = AudioEngine::resolveUtauResampler(
                "  \"" + elsewhere.getFullPathName() + "\"  ", root) == elsewhere;

            // A setting left over from another machine names a file that is
            // not there; fall through to the bundled engine instead of failing.
            const auto stale = AudioEngine::resolveUtauResampler(
                root.getChildFile("gone.exe").getFullPathName(), root);
            const auto survivesAStalePath = stale == inFolder;

            root.deleteRecursively();
            const auto ok = emptyHanded && findsItBeside && prefersTheFolder
                && settingsWin && tolerantOfPasting && survivesAStalePath;
            std::cout << "nothing_bundled_resolves_to_nothing=" << (emptyHanded ? 1 : 0)
                      << "|finds_one_beside_the_exe=" << (findsItBeside ? 1 : 0)
                      << "|prefers_the_engines_folder=" << (prefersTheFolder ? 1 : 0)
                      << "|a_chosen_engine_still_wins=" << (settingsWin ? 1 : 0)
                      << "|a_pasted_path_still_works=" << (tolerantOfPasting ? 1 : 0)
                      << "|a_stale_setting_falls_back=" << (survivesAStalePath ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-anchor-frequency")
        {
            // 输入音高: a pitch anchor put where a typed frequency says,
            // rather than dragged to it.  The roll's vertical axis is MIDI, so
            // the number has to be turned into one -- and then reach the
            // contour, which is what actually sounds off a UTAU track.
            I18n strings;
            using Roll = PianoRollComponent;

            const auto midiOf = [](double hertz)
            {
                return Roll::anchorMidiForFrequency(hertz);
            };
            const auto near = [](std::optional<float> got, double wanted)
            {
                return got.has_value() && std::abs(static_cast<double>(*got) - wanted)
                    < 0.01;
            };
            const auto readsTheStandardPitches = near(midiOf(440.0), 69.0)
                && near(midiOf(220.0), 57.0) && near(midiOf(880.0), 81.0)
                && near(midiOf(261.6255653), 60.0) && near(midiOf(27.5), 21.0);
            // The ends of the MIDI range are in.  Outside it there is no row
            // for the point to sit on, and a slip of the keyboard would throw
            // it somewhere the line cannot be seen or dragged back from.
            const auto keepsToTheKeyboard = midiOf(8.18).has_value()
                && midiOf(12543.0).has_value()
                && !midiOf(8.0).has_value() && !midiOf(13000.0).has_value();
            // And nothing that is not a frequency at all.
            const auto refusesNonsense = !midiOf(0.0).has_value()
                && !midiOf(-440.0).has_value()
                && !midiOf(std::numeric_limits<double>::quiet_NaN()).has_value()
                && !midiOf(std::numeric_limits<double>::infinity()).has_value();
            // Back the other way, so the box opens on the pitch the point has.
            auto worstRoundTrip = 0.0;
            for (const auto hertz : { 55.0, 110.0, 261.6255653, 440.0, 1000.0, 3520.0 })
            {
                const auto midi = midiOf(hertz);
                if (!midi) { worstRoundTrip = 1.0e9; break; }
                worstRoundTrip = std::max(worstRoundTrip,
                    std::abs(Roll::anchorFrequencyForMidi(*midi) - hertz) / hertz);
            }
            const auto roundTrips = worstRoundTrip < 1.0e-5;

            // A note with a line that goes somewhere, so a point moving is
            // visible against the ones that do not.
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-hz-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=f\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            const auto clipId = project.snapshot().tracks.front().clips.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::mld5);
            const auto noteId = project.snapshot().tracks.front()
                .clips.front().notes.front().id;
            std::vector<PitchCurveEditPoint> bent;
            bent.push_back({ 0.0, 60.0f });
            bent.push_back({ 0.4, 67.0f });
            bent.push_back({ 0.8, 55.0f });
            bent.push_back({ 1.0, 60.0f });
            const auto bentSet = project.setNotePitchCurve(noteId, bent, true);
            const auto noteNow = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes.front();
            };
            const auto anchors = noteNow().pitchControlPoints;
            const auto startedBent = bentSet && anchors.size() == 4;

            // One point moves and nothing else does -- not its time, not the
            // shape of the segment into it, not its neighbours.
            const auto moved = Roll::anchorsWithFrequency(anchors, 1, 880.0);
            const auto onlyThatPointMoved = startedBent
                && moved.size() == anchors.size()
                && std::abs(moved[1].targetMidi - 81.0f) < 0.01f
                && std::abs(moved[0].targetMidi - anchors[0].targetMidi) < 1.0e-6f
                && std::abs(moved[2].targetMidi - anchors[2].targetMidi) < 1.0e-6f
                && std::abs(moved[3].targetMidi - anchors[3].targetMidi) < 1.0e-6f
                && std::abs(moved[1].timeSeconds - anchors[1].timeSeconds) < 1.0e-12
                && moved[1].shape == anchors[1].shape
                && std::abs(moved[1].bezierX1 - anchors[1].bezierX1) < 1.0e-6f;
            // A number that is not a pitch writes nothing at all, rather than
            // moving the point somewhere it cannot be found.
            const auto writesNothingOnNonsense =
                Roll::anchorsWithFrequency(anchors, 1, 0.0).empty()
                && Roll::anchorsWithFrequency(anchors, 1, -5.0).empty()
                && Roll::anchorsWithFrequency(anchors, -1, 440.0).empty()
                && Roll::anchorsWithFrequency(anchors, 99, 440.0).empty();

            // It is on the menu that is actually built, and usable on the
            // first point too: this is about the point, not the segment
            // leading into it, and the first point has no segment.
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1400, 700);
            roll.setPixelsPerSecond(200.0f);
            roll.setFocusedTrack(trackId);
            roll.setFocusedClip(clipId);
            roll.diagnosticRefresh();
            const auto has = [](const std::vector<int>& items, int id)
            {
                return std::find(items.begin(), items.end(), id) != items.end();
            };
            const auto onTheMenu = has(roll.diagnosticAnchorMenuIds(noteId, 1), 9)
                && has(roll.diagnosticEnabledAnchorMenuIds(noteId, 1), 9);
            const auto onTheFirstPointToo =
                has(roll.diagnosticAnchorMenuIds(noteId, 0), 9)
                && has(roll.diagnosticEnabledAnchorMenuIds(noteId, 0), 9);
            // And greyed where there is no point to move.
            const auto greyWithoutAPoint =
                !has(roll.diagnosticEnabledAnchorMenuIds(noteId, 99), 9);
            // The rest of the menu is as it was: the segment shapes are for a
            // segment, so the first point still does not offer them.
            const auto interiorIds = roll.diagnosticAnchorMenuIds(noteId, 1);
            const auto firstIds = roll.diagnosticAnchorMenuIds(noteId, 0);
            const auto menuOtherwiseUnchanged = has(interiorIds, 2)
                && has(interiorIds, 6) && has(interiorIds, 7) && has(interiorIds, 8)
                && !has(firstIds, 2) && !has(firstIds, 6)
                && has(firstIds, 7) && has(firstIds, 8);

            // Through the model, which is what will be heard: the curve is
            // written into the contour as cents away from the note's pitch, so
            // a typed 880 has to arrive there as +2100 and not merely as a dot
            // drawn higher up.
            project.setNotePitchCurve(noteId, moved, true);
            const auto after = noteNow();
            const auto anchorKept = after.pitchControlPoints.size() == moved.size()
                && std::abs(after.pitchControlPoints[1].targetMidi - 81.0f) < 0.01f;
            auto atTheAnchor = 0.0f;
            auto foundAFrame = false;
            for (const auto& point : after.contour)
                if (point.hasManualTarget
                    && std::abs(point.timeSeconds - 0.4) < 0.01)
                {
                    atTheAnchor = point.manualTargetCents;
                    foundAFrame = true;
                }
            const auto wanted = (81.0f - after.midiNote) * 100.0f;
            const auto reachesTheSound = anchorKept && foundAFrame
                && std::abs(atTheAnchor - wanted) < 30.0f;

            // Every id the menu offers has to be one the handler acts on, or
            // an item is there to be clicked and nothing happens.
            auto everyItemIsHandled = !interiorIds.empty() && !firstIds.empty();
            for (const auto ids : { interiorIds, firstIds })
                for (const auto id : ids)
                    if (!Roll::anchorMenuChoiceHandled(id)) everyItemIsHandled = false;

            const auto ok = everyItemIsHandled && readsTheStandardPitches
                && keepsToTheKeyboard
                && refusesNonsense && roundTrips && startedBent
                && onlyThatPointMoved && writesNothingOnNonsense && onTheMenu
                && onTheFirstPointToo && greyWithoutAPoint
                && menuOtherwiseUnchanged && reachesTheSound;
            std::cout << "every_item_is_handled=" << (everyItemIsHandled ? 1 : 0)
                      << "|reads_the_standard_pitches=" << (readsTheStandardPitches ? 1 : 0)
                      << "|keeps_to_the_keyboard=" << (keepsToTheKeyboard ? 1 : 0)
                      << "|refuses_nonsense=" << (refusesNonsense ? 1 : 0)
                      << "|round_trips=" << (roundTrips ? 1 : 0)
                      << "|started_bent=" << (startedBent ? 1 : 0)
                      << "|only_that_point_moved=" << (onlyThatPointMoved ? 1 : 0)
                      << "|writes_nothing_on_nonsense=" << (writesNothingOnNonsense ? 1 : 0)
                      << "|on_the_menu=" << (onTheMenu ? 1 : 0)
                      << "|on_the_first_point_too=" << (onTheFirstPointToo ? 1 : 0)
                      << "|grey_without_a_point=" << (greyWithoutAPoint ? 1 : 0)
                      << "|menu_otherwise_unchanged=" << (menuOtherwiseUnchanged ? 1 : 0)
                      << "|reaches_the_sound=" << (reachesTheSound ? 1 : 0)
                      << "|cents=" << juce::String(atTheAnchor, 1)
                      << "/" << juce::String(wanted, 1)
                      << "|worst_round_trip=" << juce::String(worstRoundTrip, 9)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-utau-overlap")
        {
            // Overlap is the stretch the two notes sound through together.
            // The next note starts sounding a preutterance before its beat and
            // fades in over its overlap, so the note before it goes on
            // sounding until preutterance-minus-overlap after that beat --
            // past its own end whenever the overlap is the longer of the two.
            //
            // The fixture makes that measurable: the second note's sample is
            // all but silent, so anything still heard after its beat is the
            // first note's tail and nothing else.
            I18n strings;
            const auto work = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-ovl-" + juce::Uuid().toDashedString());
            // "bank" cannot be read as a note name; a uuid can, and the folder
            // a sample sits in is taken for its pitch.
            const auto folder = work.getChildFile("bank");
            folder.createDirectory();
            constexpr auto rate = 44100.0;
            const auto writeTone = [&](const juce::File& file, double seconds,
                                       double hertz, float amplitude)
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * seconds));
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                {
                    const auto phase = 2.0 * juce::MathConstants<double>::pi * hertz
                        * static_cast<double>(index) / rate;
                    buffer.setSample(0, index, amplitude * std::sin(
                        static_cast<float>(phase)));
                }
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            };
            writeTone(folder.getChildFile("tone.wav"), 2.0, 220.0, 0.5f);
            // Silent, so anything at all after its beat is the first note
            // and the fade can be read where it truly ends rather than where
            // it crosses a threshold chosen to clear this note's own level.
            writeTone(folder.getChildFile("quiet.wav"), 1.0, 220.0, 0.0f);
            folder.getChildFile("oto.ini").replaceWithText(
                "tone.wav=aa,0,100,1000,80,40\n"
                "quiet.wav=bb,0,100,0,80,40\n");
            backend::UtauRenderer::invalidateVoicebankCache();

            // Where the first note is last heard, as a time after the second
            // note's beat.  Negative means it stopped before that beat.  The
            // first note always runs 0..1.
            const auto tailEndsAt = [&](double secondBeat, double secondLength,
                                        double preutterance, double overlap,
                                        bool shaped = false)
            {
                ProjectModel project;
                const auto clipId = project.addAudioFile(
                    folder.getChildFile("tone.wav"), 2.0, 0.0, {});
                const auto trackId = project.snapshot().tracks.front().id;
                project.setTrackCompose(trackId, true);
                project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
                project.setTrackVoicebankDirectory(trackId, folder);
                const auto first = project.addNote(clipId, 0.0, 1.0, 60.0f);
                const auto second = project.addNote(clipId, secondBeat,
                                                    secondLength, 60.0f);
                project.setNoteLabel(first, "aa");
                project.setNoteLabel(second, "bb");
                project.setNoteUtauTimingOverrides(second, true, preutterance, overlap);
                if (shaped)
                {
                    // The shape a note gets the moment a lyric is typed into
                    // it, written through the same call the window makes.
                    PianoRollComponent seeding(project, strings);
                    seeding.setBounds(0, 0, 1400, 700);
                    seeding.setPixelsPerSecond(200.0f);
                    seeding.setFocusedTrack(trackId);
                    seeding.setFocusedClip(clipId);
                    seeding.diagnosticRefresh();
                    seeding.ensureDefaultEnvelope(first);
                    seeding.ensureDefaultEnvelope(second);
                }
                AudioEngine engine;
                engine.setUtauRenderNoteSelection({ first, second });
                engine.syncProject(project.snapshot());
                for (int spin = 0; spin < 600 && !engine.renderProgress(); ++spin)
                    juce::Thread::sleep(5);
                for (int spin = 0; spin < 1200 && engine.renderProgress(); ++spin)
                    juce::Thread::sleep(50);
                juce::Thread::sleep(200);
                auto output = work.getChildFile("out.wav");
                output.deleteFile();
                juce::String error;
                if (!engine.exportWav(output, error)) return -99.0;
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                auto reader = std::unique_ptr<juce::AudioFormatReader>(
                    formats.createReaderFor(output));
                if (reader == nullptr) return -99.0;
                juce::AudioBuffer<float> buffer(
                    static_cast<int>(reader->numChannels),
                    static_cast<int>(reader->lengthInSamples));
                reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
                const auto outputRate = reader->sampleRate;
                reader.reset();
                output.deleteFile();
                // Three least-significant bits of the sixteen written, so
                // silence reads as silence and the fade is measured where the
                // mixer actually stops it.
                auto last = -1;
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                    if (std::abs(buffer.getSample(0, index)) > 1.0e-4f) last = index;
                if (last < 0) return -99.0;
                return static_cast<double>(last) / outputRate - secondBeat;
            };

            // Within half a cycle of the tone throughout: the last sample
            // over the floor is the last peak, not the last instant.
            constexpr auto slack = 0.01;

            // Overlap longer than the preutterance: the tail reaches past the
            // beat by the difference.
            const auto reachIn = tailEndsAt(1.0, 0.5, 0.03, 0.20);
            const auto wantedReachIn = 0.20 - 0.03;
            const auto reachesIntoTheNextNote =
                std::abs(reachIn - wantedReachIn) < slack;
            // Overlap shorter than the preutterance: it stops before the beat.
            const auto stopShort = tailEndsAt(1.0, 0.5, 0.20, 0.03);
            const auto wantedStopShort = 0.03 - 0.20;
            const auto stopsShortWhenOverlapIsSmall =
                std::abs(stopShort - wantedStopShort) < slack;
            // And the number does something: a larger overlap has to move the
            // tail, which is the whole of the complaint.
            const auto movesWithTheNumber = reachIn > stopShort + 0.1;

            // A preutterance of zero, which is ordinary: the next note starts
            // sounding exactly on its beat, which is exactly where this one
            // ends.  The two are still contiguous and the overlap still says
            // they cross -- "starts before this one ends" is not the question.
            const auto noLeadIn = tailEndsAt(1.0, 0.5, 0.0, 0.09);
            const auto wantedNoLeadIn = 0.09;
            const auto crossesWithNoLeadIn =
                std::abs(noLeadIn - wantedNoLeadIn) < slack;

            // And with the shape a note carries the moment a lyric is typed
            // into it.  An envelope holds its last gain past its last point,
            // and that gain is silence, so an envelope pinned to the note's
            // own end silences the tail however long the note was rendered --
            // which is every note anyone has actually sung.
            const auto shapedTail = tailEndsAt(1.0, 0.5, 0.0, 0.09, true);
            const auto shapedTailSurvives =
                std::abs(shapedTail - wantedNoLeadIn) < slack;

            // A rest between them: the next note starts sounding after this
            // one has finished, nothing crosses, and the note must not go on
            // ringing into the gap.  It ends at its own end, bar the short
            // fade every unjoined note closes with.
            const auto acrossAGap = tailEndsAt(1.3, 0.5, 0.05, 0.20);
            // Every note is rendered 20 ms past its end and closes with a
            // short fade inside that, so its last sound is at 1.020.
            const auto wantedAcrossAGap = 1.0 + 0.020 - 1.3;
            const auto silentAcrossAGap =
                std::abs(acrossAGap - wantedAcrossAGap) < slack;

            // An overlap typed longer than the note it belongs to stops at
            // that note's end; past there the note after it owns the seam.
            const auto outsized = tailEndsAt(1.0, 0.1, 0.03, 2.0);
            const auto wantedOutsized = 0.1;
            const auto heldToTheNextNote =
                std::abs(outsized - wantedOutsized) < slack;

            // And the roll draws it there.  What is heard and what is drawn
            // have to agree about where a note stops: the same clamp was in
            // both, so the line on screen ended at the bar line too.
            // The drawn envelope's last point, so the shape on screen and the
            // shape the mixer applies end in the same place.
            std::vector<AmplitudeEnvelopePoint> drawnEnvelope;
            const auto drawnEndFor = [&](double preutterance, double overlap)
            {
                ProjectModel shown;
                const auto shownClip = shown.addAudioFile(
                    folder.getChildFile("tone.wav"), 2.0, 0.0, {});
                const auto shownTrack = shown.snapshot().tracks.front().id;
                shown.setTrackCompose(shownTrack, true);
                shown.setTrackPitchAlgorithm(shownTrack, PitchAlgorithm::utau);
                shown.setTrackVoicebankDirectory(shownTrack, folder);
                const auto shownFirst = shown.addNote(shownClip, 0.0, 1.0, 60.0f);
                const auto shownSecond = shown.addNote(shownClip, 1.0, 0.5, 60.0f);
                shown.setNoteLabel(shownFirst, "aa");
                shown.setNoteLabel(shownSecond, "bb");
                shown.setNoteUtauTimingOverrides(shownSecond, true,
                                                 preutterance, overlap);
                PianoRollComponent roll(shown, strings);
                roll.setBounds(0, 0, 1400, 700);
                roll.setPixelsPerSecond(200.0f);
                roll.setFocusedTrack(shownTrack);
                roll.setFocusedClip(shownClip);
                roll.ensureDefaultEnvelope(shownFirst);
                roll.ensureDefaultEnvelope(shownSecond);
                roll.diagnosticRefresh();
                drawnEnvelope = roll.diagnosticDrawnEnvelope(shownFirst);
                return roll.diagnosticSoundingSpan(shownFirst).second;
            };
            const auto drawnEnd = drawnEndFor(0.03, 0.20);
            // The case with no lead-in as well: it is the one the drawing and
            // the mixer both used to leave out.
            const auto drawnBare = drawnEndFor(0.0, 0.09);
            const auto drawnFollowsTheOverlap =
                std::abs(drawnEnd - (1.0 + wantedReachIn)) < 0.001
                && std::abs(drawnBare - (1.0 + wantedNoLeadIn)) < 0.001;
            // drawnBare was the last one measured, so this is its envelope.
            const auto envelopeEnd = drawnEnvelope.empty()
                ? -99.0 : drawnEnvelope.back().timeSeconds;
            // Envelope times run from the note's own start, and the note is
            // a second long, so its sounding end is 1 + the tail.
            const auto shapeEndsWhereTheSoundDoes =
                std::abs(envelopeEnd - (1.0 + wantedNoLeadIn)) < 0.001;

            work.deleteRecursively();
            const auto ok = reachesIntoTheNextNote && stopsShortWhenOverlapIsSmall
                && movesWithTheNumber && crossesWithNoLeadIn && shapedTailSurvives
                && silentAcrossAGap && heldToTheNextNote && drawnFollowsTheOverlap
                && shapeEndsWhereTheSoundDoes;
            const auto report = [](double got, double want)
            {
                return juce::String(got, 4) + " (want " + juce::String(want, 4) + ")";
            };
            std::cout << "reaches_into_the_next_note=" << (reachesIntoTheNextNote ? 1 : 0)
                      << "|stops_short_when_overlap_is_small="
                      << (stopsShortWhenOverlapIsSmall ? 1 : 0)
                      << "|moves_with_the_number=" << (movesWithTheNumber ? 1 : 0)
                      << "|crosses_with_no_lead_in=" << (crossesWithNoLeadIn ? 1 : 0)
                      << "|shaped_tail_survives=" << (shapedTailSurvives ? 1 : 0)
                      << "|silent_across_a_gap=" << (silentAcrossAGap ? 1 : 0)
                      << "|held_to_the_next_note=" << (heldToTheNextNote ? 1 : 0)
                      << "|drawn_follows_the_overlap=" << (drawnFollowsTheOverlap ? 1 : 0)
                      << "|drawn_end=" << report(drawnEnd, 1.0 + wantedReachIn)
                      << "|drawn_bare=" << report(drawnBare, 1.0 + wantedNoLeadIn)
                      << "|shape_ends_where_the_sound_does="
                      << (shapeEndsWhereTheSoundDoes ? 1 : 0)
                      << "|envelope_end=" << report(envelopeEnd, 1.0 + wantedNoLeadIn)
                      << "|reach_in=" << report(reachIn, wantedReachIn)
                      << "|stop_short=" << report(stopShort, wantedStopShort)
                      << "|no_lead_in=" << report(noLeadIn, wantedNoLeadIn)
                      << "|shaped=" << report(shapedTail, wantedNoLeadIn)
                      << "|gap=" << report(acrossAGap, wantedAcrossAGap)
                      << "|outsized=" << report(outsized, wantedOutsized)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-note-stp")
        {
            // STP moves the whole oto entry along the recording before a note
            // of it is read: the same lengths, a different piece of audio.
            // The only way to see that is to listen to what came out, so the
            // fixture is a recording whose two halves do not sound alike.
            I18n strings;
            const auto work = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-stp-" + juce::Uuid().toDashedString());
            // The folder a sample sits in is read as its pitch when the name
            // looks like one, and a uuid is full of letter-digit pairs that
            // do: the source pitch, and with it the whole render, changed from
            // run to run.  "bank" cannot be read as a note.
            const auto folder = work.getChildFile("bank");
            folder.createDirectory();
            const auto wav = folder.getChildFile("tone.wav");
            constexpr auto rate = 44100.0;
            constexpr auto lowHz = 220.0;
            constexpr auto highHz = 880.0;
            {
                juce::AudioBuffer<float> buffer(1, static_cast<int>(rate * 2.0));
                for (int index = 0; index < buffer.getNumSamples(); ++index)
                {
                    const auto time = static_cast<double>(index) / rate;
                    // Two seconds: a low tone, then a high one.  Which of them
                    // comes out says which part of the file was read.
                    const auto hertz = time < 1.0 ? lowHz : highHz;
                    const auto phase = 2.0 * juce::MathConstants<double>::pi * hertz
                        * (time < 1.0 ? time : time - 1.0);
                    buffer.setSample(0, index, 0.5f * std::sin(static_cast<float>(phase)));
                }
                juce::WavAudioFormat format;
                std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    format.createWriterFor(stream.get(), rate, 1, 16, {}, 0));
                if (writer != nullptr)
                {
                    stream.release();
                    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
                }
            }
            // The entry describes the first half second, which is all inside
            // the low tone.  cutoff counts back from the end of the file.
            folder.getChildFile("oto.ini").replaceWithText(
                "tone.wav=aa,0,100,1500,80,40\n");
            backend::UtauRenderer::invalidateVoicebankCache();

            ProjectModel project;
            const auto clipId = project.addAudioFile(wav, 2.0, 0.0, {});
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackCompose(trackId, true);
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            project.setTrackVoicebankDirectory(trackId, folder);
            const auto noteId = project.addNote(clipId, 0.5, 0.5, 60.0f);
            project.setNoteLabel(noteId, "aa");

            // The menu is where this is reached from, and only on a UTAU
            // track: off one there is no oto entry to move.
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1200, 600);
            roll.setFocusedTrack(trackId);
            roll.setFocusedClip(clipId);
            roll.diagnosticRefresh();
            const auto utauIds = roll.diagnosticNoteMenuIds(noteId);
            const auto inTheUtauMenu = std::find(utauIds.begin(), utauIds.end(), 19)
                != utauIds.end();
            const auto plainIds = PianoRollComponent::noteMenuItemsFor(false);
            const auto notInThePlainMenu =
                std::find(plainIds.begin(), plainIds.end(), 19) == plainIds.end();

            // What came out, rendered through the engine so the whole chain is
            // walked: the note's field, the spec the engine builds, the entry
            // the renderer shifts, the audio.
            // One engine for all four, as the running application has: an
            // engine that has already rendered this clip will hand back what
            // it rendered unless the note's key says the note has changed, so
            // a fresh engine each time would never put that to the test.
            AudioEngine engine;
            // UTAU rendering is selection-driven: with nothing selected no
            // note is scheduled and the mix comes out silent.
            engine.setUtauRenderNoteSelection({ noteId });
            // Whether each change actually put the engine to work.  A clip
            // already rendered is handed back untouched unless the note's key
            // says the note has changed, so this is the difference between a
            // new STP being heard and being quietly ignored.
            auto everyChangeRerendered = true;
            const auto renderedHz = [&](double stpMilliseconds)
            {
                project.setNotesUtauStp({ noteId }, stpMilliseconds / 1000.0);
                engine.syncProject(project.snapshot());
                // Polled tightly, because a short render can be over before a
                // lazy poll notices it started.
                auto started = false;
                for (int spin = 0; spin < 600 && !started; ++spin)
                {
                    started = engine.renderProgress().has_value();
                    if (!started) juce::Thread::sleep(5);
                }
                if (!started) everyChangeRerendered = false;
                for (int spin = 0; spin < 1200 && engine.renderProgress(); ++spin)
                    juce::Thread::sleep(50);
                juce::Thread::sleep(200);
                auto output = folder.getChildFile("out.wav");
                output.deleteFile();
                juce::String error;
                if (!engine.exportWav(output, error)) return -1.0;
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                auto reader = std::unique_ptr<juce::AudioFormatReader>(
                    formats.createReaderFor(output));
                if (reader == nullptr) return -1.0;
                juce::AudioBuffer<float> buffer(
                    static_cast<int>(reader->numChannels),
                    static_cast<int>(reader->lengthInSamples));
                reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
                const auto outputRate = reader->sampleRate;
                reader.reset();
                output.deleteFile();
                // The loudest tenth of a second, and how often it crosses
                // zero: a tone crosses twice a cycle, so that is its pitch.
                const auto window = static_cast<int>(outputRate * 0.1);
                if (buffer.getNumSamples() <= window) return -1.0;
                auto bestStart = 0;
                auto bestEnergy = -1.0;
                for (int start = 0; start + window < buffer.getNumSamples();
                     start += window / 4)
                {
                    auto energy = 0.0;
                    for (int index = start; index < start + window; ++index)
                    {
                        const auto value = buffer.getSample(0, index);
                        energy += static_cast<double>(value) * value;
                    }
                    if (energy > bestEnergy) { bestEnergy = energy; bestStart = start; }
                }
                if (bestEnergy <= 1.0e-6) return 0.0;
                // The period of that window, by autocorrelation: a zero-crossing
                // count reads whatever the render leaves on top of the tone,
                // and read 70 Hz off a 220 Hz one.
                std::vector<double> block(static_cast<std::size_t>(window));
                auto mean = 0.0;
                for (int index = 0; index < window; ++index)
                    mean += buffer.getSample(0, bestStart + index);
                mean /= static_cast<double>(window);
                for (int index = 0; index < window; ++index)
                    block[static_cast<std::size_t>(index)] =
                        buffer.getSample(0, bestStart + index) - mean;
                const auto shortest = static_cast<int>(outputRate / 1200.0);
                const auto longest = std::min(window / 2,
                    static_cast<int>(outputRate / 100.0));
                std::vector<double> scores;
                auto bestScore = -1.0;
                for (auto lag = shortest; lag <= longest; ++lag)
                {
                    auto sum = 0.0;
                    for (int index = 0; index + lag < window; ++index)
                        sum += block[static_cast<std::size_t>(index)]
                            * block[static_cast<std::size_t>(index + lag)];
                    const auto score = sum / static_cast<double>(window - lag);
                    scores.push_back(score);
                    bestScore = std::max(bestScore, score);
                }
                // The shortest lag that correlates about as well as the best
                // one.  Every multiple of a period correlates just as highly,
                // so taking the largest reads the tone an octave or two low.
                if (bestScore <= 0.0) return 0.0;
                for (std::size_t index = 0; index < scores.size(); ++index)
                    if (scores[index] >= bestScore * 0.9)
                        return outputRate / static_cast<double>(shortest
                            + static_cast<int>(index));
                return 0.0;
            };

            const auto near = [](double value, double wanted)
            {
                return value > wanted * 0.85 && value < wanted * 1.15;
            };
            const auto plainHz = renderedHz(0.0);
            const auto readsTheLowTone = near(plainHz, lowHz);
            // Far enough along to land in the high half.
            const auto shiftedHz = renderedHz(1200.0);
            const auto stpMovesTheAudio = near(shiftedHz, highHz);
            // Backwards from there, to the start of the file again.
            const auto backHz = renderedHz(-5000.0);
            const auto negativeGoesBack = near(backHz, lowHz);
            // And it cannot be pushed off the end: the furthest it goes is the
            // last half second the entry still fits in, which is high.
            const auto farHz = renderedHz(1'000'000.0);
            const auto heldInsideTheFile = near(farHz, highHz);

            // Saved and reopened, or it would be gone next session.
            project.setNotesUtauStp({ noteId }, 0.0375);
            const auto saved = folder.getChildFile("stp.hjpx");
            juce::String saveError;
            const auto wrote = project.save(saved, saveError);
            ProjectModel reopened;
            juce::String loadError;
            const auto read = wrote && reopened.load(saved, loadError);
            auto survivesSaving = false;
            if (read)
                for (const auto& track : reopened.snapshot().tracks)
                    for (const auto& clip : track.clips)
                        for (const auto& note : clip.notes)
                            if (std::abs(note.utauStpSeconds - 0.0375) < 1.0e-9)
                                survivesSaving = true;

            // One undoable step, and it goes back to what it was.
            project.setNotesUtauStp({ noteId }, 0.2);
            project.undo();
            auto undoneInOneStep = false;
            for (const auto& track : project.snapshot().tracks)
                for (const auto& clip : track.clips)
                    for (const auto& note : clip.notes)
                        if (note.id == noteId)
                            undoneInOneStep =
                                std::abs(note.utauStpSeconds - 0.0375) < 1.0e-9;

            work.deleteRecursively();
            const auto ok = inTheUtauMenu && notInThePlainMenu && readsTheLowTone
                && stpMovesTheAudio && negativeGoesBack && heldInsideTheFile
                && everyChangeRerendered && survivesSaving && undoneInOneStep;
            std::cout << "in_the_utau_menu=" << (inTheUtauMenu ? 1 : 0)
                      << "|not_in_the_plain_menu=" << (notInThePlainMenu ? 1 : 0)
                      << "|reads_the_low_tone=" << (readsTheLowTone ? 1 : 0)
                      << "|stp_moves_the_audio=" << (stpMovesTheAudio ? 1 : 0)
                      << "|negative_goes_back=" << (negativeGoesBack ? 1 : 0)
                      << "|held_inside_the_file=" << (heldInsideTheFile ? 1 : 0)
                      << "|every_change_rerendered=" << (everyChangeRerendered ? 1 : 0)
                      << "|survives_saving=" << (survivesSaving ? 1 : 0)
                      << "|undone_in_one_step=" << (undoneInOneStep ? 1 : 0)
                      << "|hz=" << juce::String(plainHz, 1)
                      << "/" << juce::String(shiftedHz, 1)
                      << "/" << juce::String(backHz, 1)
                      << "/" << juce::String(farHz, 1)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-track-toggle-tips")
        {
            // The five switches on a track row are painted, not built out of
            // buttons, so nothing about them carried a name: hovering said
            // nothing and C M S A N had to be guessed at.  A tooltip needs
            // two things here -- the panel has to be a TooltipClient at all,
            // and it has to work out which switch the pointer is on, because
            // JUCE asks the whole component and never says where.
            I18n strings;
            ProjectModel project;
            const auto first = project.addTrack("one", true);
            project.addTrack("two", true);

            TrackListComponent list(project, strings);
            list.setRowHeight(96.0f);
            list.setBounds(0, 0, 280, 400);

            // Without this the text below could be perfect and never reach a
            // screen: JUCE looks for a TooltipClient and gives up otherwise.
            auto* client = dynamic_cast<juce::TooltipClient*>(&list);
            const auto offersTooltips = client != nullptr;

            const auto at = [](int x, int y)
            {
                return juce::Point<float>(static_cast<float>(x), static_cast<float>(y));
            };
            const auto hover = [&list, &at](int x, int y)
            {
                const auto where = at(x, y);
                list.mouseMove(juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &list, &list, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false));
            };
            const auto click = [&list, &at](int x, int y)
            {
                const auto where = at(x, y);
                list.mouseDown(juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, &list, &list, juce::Time::getCurrentTime(),
                    where, juce::Time::getCurrentTime(), 1, false));
            };
            const auto tip = [client]
            {
                return client != nullptr ? client->getTooltip() : juce::String();
            };

            struct Switches { bool compose, muted, solo, smooth, normalize; };
            const auto readSwitches = [&project](const juce::String& id)
            {
                Switches out { false, false, false, false, false };
                for (const auto& track : project.snapshot().tracks)
                    if (track.id == id)
                        out = { track.compose, track.muted, track.solo,
                                track.smoothOverlaps, track.normalizeVolume };
                return out;
            };
            using Toggle = TrackListComponent::TrackToggle;
            const auto flipped = [](const Switches& before, const Switches& after)
            {
                auto changes = 0;
                auto which = Toggle::none;
                if (before.compose != after.compose) { ++changes; which = Toggle::compose; }
                if (before.muted != after.muted) { ++changes; which = Toggle::muted; }
                if (before.solo != after.solo) { ++changes; which = Toggle::solo; }
                if (before.smooth != after.smooth)
                { ++changes; which = Toggle::smoothOverlaps; }
                if (before.normalize != after.normalize)
                { ++changes; which = Toggle::normalizeVolume; }
                return changes == 1 ? which : Toggle::none;
            };

            // The switches are painted in 28-pixel boxes at 10, 41, 72, 103
            // and 134, in the band 34..56 of a row.  Under the ruler, that is
            // where the pointer has to be answered.
            constexpr auto rowTop = 24;
            constexpr auto band = rowTop + 45;
            const std::array<int, 5> boxes { 10, 41, 72, 103, 134 };

            // The heart of it: what the tooltip says is what the click does.
            // Read off the model rather than off the rule, so a tooltip that
            // named the wrong switch would show up as a different switch
            // moving.
            std::vector<juce::String> tips;
            auto namesWhatItFlips = true;
            for (const auto left : boxes)
            {
                const auto x = left + 14;
                hover(x, band);
                const auto shown = tip();
                tips.push_back(shown);
                const auto before = readSwitches(first);
                click(x, band);
                const auto* key = TrackListComponent::tooltipKeyFor(
                    flipped(before, readSwitches(first)));
                if (key == nullptr || shown.isEmpty() || shown != strings.text(key))
                    namesWhatItFlips = false;
            }
            const auto allFiveSpeak = tips.size() == 5
                && std::none_of(tips.begin(), tips.end(),
                    [](const juce::String& text) { return text.isEmpty(); });
            auto allDifferent = tips.size() == 5;
            for (std::size_t a = 0; a + 1 < tips.size(); ++a)
                for (auto b = a + 1; b < tips.size(); ++b)
                    if (tips[a] == tips[b]) allDifferent = false;

            // And it lines up with what is drawn.  The boxes are 28 wide with
            // a three-pixel gutter, and a tooltip that spilled into the
            // gutter would be describing a switch the pointer is not on.
            auto edgesLineUp = true;
            for (const auto left : boxes)
            {
                hover(left, band);
                if (tip().isEmpty()) edgesLineUp = false;
                hover(left + 27, band);
                if (tip().isEmpty()) edgesLineUp = false;
                hover(left - 1, band);
                if (tip().isNotEmpty()) edgesLineUp = false;
                hover(left + 28, band);
                if (tip().isNotEmpty()) edgesLineUp = false;
            }
            hover(24, rowTop + 33);
            const auto quietAboveTheBand = tip().isEmpty();
            hover(24, rowTop + 56);
            const auto quietBelowTheBand = tip().isEmpty();
            hover(200, band);
            const auto quietOverTheFaders = tip().isEmpty();

            // Every row answers for itself, not for the first one.
            hover(24, rowTop + 96 + 45);
            const auto secondRowAnswers = tip() == strings.text("track.tip.compose");
            hover(24, rowTop + 96 * 2 + 45);
            const auto quietBelowTheTracks = tip().isEmpty();

            // And now off the picture rather than off a literal, which
            // could drift away from the paint without anything noticing.  A
            // fresh panel, so no row carries the selection highlight: the
            // switches are then the only thing painted over the row colour in
            // this band, and each one is a run of pixels that is not it.
            juce::Image shot(juce::Image::ARGB, 280, 400, true);
            {
                TrackListComponent painted(project, strings);
                painted.setRowHeight(96.0f);
                painted.setBounds(0, 0, 280, 400);
                juce::Graphics graphics(shot);
                painted.paint(graphics);
            }
            std::vector<juce::Range<int>> runs;
            {
                // Near the top of the band: below the rounded corners and
                // above the letter, which on a lit switch is drawn in the row
                // colour and would split the run in two.
                const auto scanY = rowTop + 36;
                const auto rowColour = shot.getPixelAt(4, scanY);
                auto start = -1;
                for (int x = 0; x < 166; ++x)
                {
                    const auto painted = shot.getPixelAt(x, scanY) != rowColour;
                    if (painted && start < 0) start = x;
                    if (!painted && start >= 0)
                    {
                        runs.push_back(juce::Range<int>(start, x));
                        start = -1;
                    }
                }
                if (start >= 0) runs.push_back(juce::Range<int>(start, 166));
            }
            const auto fiveBoxesDrawn = runs.size() == 5;
            auto everyBoxDrawnAnswers = fiveBoxesDrawn;
            for (const auto& run : runs)
            {
                hover(run.getStart() + run.getLength() / 2, band);
                if (tip().isEmpty()) everyBoxDrawnAnswers = false;
            }
            auto gapsBetweenAreQuiet = fiveBoxesDrawn;
            for (std::size_t index = 0; index + 1 < runs.size(); ++index)
            {
                hover((runs[index].getEnd() + runs[index + 1].getStart()) / 2, band);
                if (tip().isNotEmpty()) gapsBetweenAreQuiet = false;
            }

            // Leaving forgets, or the last switch hovered would keep
            // answering after the pointer had gone somewhere else.  On a
            // switch that is answering, or leaving it proves nothing: over
            // the empty space there is no tooltip to lose.
            hover(24, band);
            const auto answeringBeforeLeaving = tip().isNotEmpty();
            {
                const auto where = at(24, band);
                list.mouseExit(juce::MouseEvent(
                    juce::Desktop::getInstance().getMainMouseSource(), where,
                    juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    &list, &list, juce::Time::getCurrentTime(), where,
                    juce::Time::getCurrentTime(), 1, false));
            }
            const auto forgottenOnLeaving = tip().isEmpty();

            // A key with a column missing comes back as the key itself, which
            // would put "track.tip.solo" on screen in that language.
            const std::array<const char*, 5> keys {
                "track.tip.compose", "track.tip.mute", "track.tip.solo",
                "track.tip.smooth", "track.tip.normalize" };
            auto everyLanguageFilled = true;
            for (const auto language : { I18n::Language::zhCN, I18n::Language::zhTW,
                                         I18n::Language::jaJP, I18n::Language::koKR,
                                         I18n::Language::enUS })
            {
                I18n other;
                other.setLanguage(language);
                for (const auto* key : keys)
                {
                    const auto text = other.text(key);
                    if (text.isEmpty() || text == juce::String(key))
                        everyLanguageFilled = false;
                }
            }

            const auto ok = offersTooltips && namesWhatItFlips && allFiveSpeak
                && allDifferent && edgesLineUp && quietAboveTheBand
                && quietBelowTheBand && quietOverTheFaders && secondRowAnswers
                && quietBelowTheTracks && answeringBeforeLeaving
                && forgottenOnLeaving && everyLanguageFilled
                && fiveBoxesDrawn && everyBoxDrawnAnswers && gapsBetweenAreQuiet;
            std::cout << "offers_tooltips=" << (offersTooltips ? 1 : 0)
                      << "|names_what_it_flips=" << (namesWhatItFlips ? 1 : 0)
                      << "|all_five_speak=" << (allFiveSpeak ? 1 : 0)
                      << "|all_different=" << (allDifferent ? 1 : 0)
                      << "|edges_line_up=" << (edgesLineUp ? 1 : 0)
                      << "|quiet_above=" << (quietAboveTheBand ? 1 : 0)
                      << "|quiet_below=" << (quietBelowTheBand ? 1 : 0)
                      << "|quiet_over_the_faders=" << (quietOverTheFaders ? 1 : 0)
                      << "|second_row_answers=" << (secondRowAnswers ? 1 : 0)
                      << "|quiet_below_the_tracks=" << (quietBelowTheTracks ? 1 : 0)
                      << "|answering_before_leaving=" << (answeringBeforeLeaving ? 1 : 0)
                      << "|forgotten_on_leaving=" << (forgottenOnLeaving ? 1 : 0)
                      << "|every_language_filled=" << (everyLanguageFilled ? 1 : 0)
                      << "|five_boxes_drawn=" << (fiveBoxesDrawn ? 1 : 0)
                      << "|every_box_drawn_answers=" << (everyBoxDrawnAnswers ? 1 : 0)
                      << "|gaps_between_are_quiet=" << (gapsBetweenAreQuiet ? 1 : 0)
                      << "|boxes=" << static_cast<int>(runs.size())
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-track-area-menu")
        {
            // Right-clicking the space around the tracks offers to make one.
            // Two halves: the rule for what counts as that space, and the
            // component actually asking for the menu when a press lands there.
            constexpr auto ruler = 24;
            constexpr auto row = 96;

            // The rule, on a list of three tracks.
            const auto onTheRuler = TrackListComponent::rowIndexAt(10, ruler, row, 3) < 0;
            const auto onTheFirstRow = TrackListComponent::rowIndexAt(ruler + 5, ruler, row, 3) == 0;
            const auto onTheLastRow =
                TrackListComponent::rowIndexAt(ruler + row * 3 - 1, ruler, row, 3) == 2;
            const auto pastTheLastRow =
                TrackListComponent::rowIndexAt(ruler + row * 3 + 1, ruler, row, 3) < 0;
            // An empty project is all empty space, which is the case that
            // matters most: a project with no tracks needs a way to get one.
            const auto anEmptyListIsAllSpace =
                TrackListComponent::rowIndexAt(ruler + 5, ruler, row, 0) < 0;

            // And the component, through its real mouseDown.
            I18n strings;
            ProjectModel project;
            project.addTrack("one", true);
            TrackListComponent list(project, strings);
            list.setRowHeight(static_cast<float>(row));
            list.setBounds(0, 0, 300, ruler + row * 4);
            list.resized();
            auto menusAsked = 0;
            list.onEmptyAreaMenu = [&menusAsked](juce::Point<int>) { ++menusAsked; };

            list.diagnosticRightClick(150, ruler + row + 20);   // under the one track
            const auto asksBelowTheTracks = menusAsked == 1;
            list.diagnosticRightClick(150, 8);                  // on the ruler
            const auto asksOnTheRuler = menusAsked == 2;
            menusAsked = 0;
            list.diagnosticRightClick(150, ruler + 20);         // on the track itself
            const auto leavesTheTrackAlone = menusAsked == 0;

            // The menu's first item makes a track, and it is the same call
            // the Track menu makes, so there is one way a track comes into being.
            const auto before = project.snapshot().tracks.size();
            project.addTrack(strings.text("track.compose"), true);
            const auto makesOne = project.snapshot().tracks.size() == before + 1;
            const auto named = strings.text("track.newHere").isNotEmpty()
                && strings.text("track.delete").isNotEmpty();

            // The second item removes the selected track.  Deleting it has to
            // move the selection somewhere real: left pointing at a track that
            // no longer exists, every "selected track" item stays enabled and
            // silently does nothing, this one included -- so a second delete
            // would look available and remove nothing.
            std::vector<TrackData> three(3);
            three[0].id = "a";
            three[1].id = "b";
            three[2].id = "c";
            const auto downwards = MainComponent::selectionAfterRemoving(three, "a") == "b"
                && MainComponent::selectionAfterRemoving(three, "b") == "c";
            // The last one has nothing after it, so the selection steps back.
            const auto backwardsAtTheEnd =
                MainComponent::selectionAfterRemoving(three, "c") == "b";
            // The only track leaves nothing to select.
            std::vector<TrackData> alone(1);
            alone[0].id = "a";
            const auto nothingLeft =
                MainComponent::selectionAfterRemoving(alone, "a").isEmpty();
            // A track that is not there at all: say so rather than inventing a
            // selection, which would move it off a perfectly good track.
            const auto leavesAStranger =
                MainComponent::selectionAfterRemoving(three, "z") == "z";

            // And it really removes it, through the model the menu calls.
            const auto standing = project.snapshot().tracks.size();
            project.removeTrack(project.snapshot().tracks.front().id);
            const auto removesOne = project.snapshot().tracks.size() == standing - 1;

            const auto ok = onTheRuler && onTheFirstRow && onTheLastRow && pastTheLastRow
                && anEmptyListIsAllSpace && asksBelowTheTracks && asksOnTheRuler
                && leavesTheTrackAlone && makesOne && named
                && downwards && backwardsAtTheEnd && nothingLeft && leavesAStranger
                && removesOne;
            std::cout << "space_below_the_tracks_is_not_a_row=" << (pastTheLastRow ? 1 : 0)
                      << "|rows_are_still_rows="
                      << (onTheFirstRow && onTheLastRow ? 1 : 0)
                      << "|the_ruler_is_not_a_row=" << (onTheRuler ? 1 : 0)
                      << "|an_empty_list_is_all_space=" << (anEmptyListIsAllSpace ? 1 : 0)
                      << "|a_right_click_there_asks_for_the_menu="
                      << (asksBelowTheTracks && asksOnTheRuler ? 1 : 0)
                      << "|a_right_click_on_a_track_does_not="
                      << (leavesTheTrackAlone ? 1 : 0)
                      << "|the_item_makes_a_track=" << (makesOne && named ? 1 : 0)
                      << "|deleting_removes_the_track=" << (removesOne ? 1 : 0)
                      << "|the_selection_moves_to_the_next="
                      << (downwards && backwardsAtTheEnd ? 1 : 0)
                      << "|the_last_track_leaves_none=" << (nothingLeft ? 1 : 0)
                      << "|a_track_not_there_moves_nothing=" << (leavesAStranger ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-stretch-items")
        {
            // The stretch picker configures the Signalsmith stretcher's
            // analysis clock, and NSF-HiFiGAN reads the same value as a splice
            // order.  Nobody else receives it: mld5, mld3, WORLD and llsm2
            // stretch from the time map inside their own renderers, and their
            // request structs have no such field, so every item there rendered
            // byte-identical audio while the diagnostic label moved.  Offer it
            // only where it lands.
            const auto items = [](int pitchItem)
            {
                return MainComponent::stretchAlgorithmItemsFor(pitchItem);
            };
            // Pitch picker ids: 1 mld5, 2 nsf-hifigan, 3 WORLD,
            //                   4 vslib, 5 mld3, 6 llsm2.
            const auto neural = items(2) == std::vector<int> { 1, 2, 5 };
            const auto signalsmith = items(4) == std::vector<int> { 1, 3, 4 };
            const auto ownStretchers = items(1) == std::vector<int>{1}
                && items(3) == std::vector<int>{1}
                && items(5) == std::vector<int>{1} && items(6) == std::vector<int>{1};
            // A UTAU item id, which is none of the above.
            const auto utau = items(7) == std::vector<int>{1} && items(0) == std::vector<int>{1};

            // The two orders NSF names are its own; the three Signalsmith
            // clocks are vslib's.  Neither list may carry the other's, or the
            // picker is once again offering a setting the backend drops.
            const auto neuralOrders = items(2);
            const auto stretcherClocks = items(4);
            const auto lacks = [](const std::vector<int>& list, int item)
            {
                return std::find(list.begin(), list.end(), item) == list.end();
            };
            const auto keepsThemApart = lacks(neuralOrders, 3) && lacks(neuralOrders, 4)
                && lacks(stretcherClocks, 2) && lacks(stretcherClocks, 5);
            // Melodyne Hybrid is the shared default and heads every list that
            // exists, so a picker losing its selection can fall back to id 1.
            const auto sharedDefault = neuralOrders.front() == 1 && stretcherClocks.front() == 1;

            const auto ok = neural && signalsmith && ownStretchers && utau
                && keepsThemApart && sharedDefault;
            std::cout << "nsf_offers_its_two_orders=" << (neural ? 1 : 0)
                      << "|vslib_offers_its_three_clocks=" << (signalsmith ? 1 : 0)
                      << "|other_backends_offer_native_stretch="
                      << (ownStretchers ? 1 : 0)
                      << "|utau_offers_native_stretch=" << (utau ? 1 : 0)
                      << "|the_two_lists_stay_apart=" << (keepsThemApart ? 1 : 0)
                      << "|melodyne_hybrid_heads_both=" << (sharedDefault ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-render-order")
        {
            MainComponent window;
            const auto pickerWorks = window.diagnosticRenderOrderPicker();
            // stretchSpliceThenPitch: clips joined by a Melodyne pitch join are
            // decoded as one phrase and cut up afterwards, instead of being
            // decoded apart and spliced.  Two things carry that -- which clips
            // are grouped, and that the one pitch line glides across the seam
            // rather than stepping the way two independent renders would.
            const auto recording = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile("hachi-order-smoke.wav");
            const auto clipAt = [&recording](double start, double sourceOffset,
                                             double length, float midi,
                                             bool joinsNext, bool joinsPrevious)
            {
                ClipData clip;
                clip.id = juce::Uuid().toDashedString();
                clip.sourceFile = recording;
                clip.startSeconds = start;
                clip.sourceOffsetSeconds = sourceOffset;
                clip.sourceDurationSeconds = length;
                clip.durationSeconds = length;
                clip.glideConnectedToNext = joinsNext;
                clip.glideConnectedFromPrevious = joinsPrevious;
                NoteData note;
                note.id = juce::Uuid().toDashedString();
                note.startSeconds = 0.0;
                note.durationSeconds = length;
                note.midiNote = midi;
                note.sourceMidiCenter = midi;
                clip.notes.push_back(note);
                return clip;
            };
            // A recording for the clips to point at: glideChains asks whether
            // the file is really there, so a made-up path would group nothing.
            {
                constexpr auto rate = 44100.0;
                juce::AudioBuffer<float> tone(1, static_cast<int>(rate * 2.0));
                for (int sample = 0; sample < tone.getNumSamples(); ++sample)
                    tone.setSample(0, sample, static_cast<float>(
                        0.3 * std::sin(2.0 * juce::MathConstants<double>::pi
                                       * 220.0 * sample / rate)));
                recording.deleteFile();
                juce::WavAudioFormat wav;
                if (auto stream = recording.createOutputStream())
                    if (auto writer = std::unique_ptr<juce::AudioFormatWriter>(
                            wav.createWriterFor(stream.release(), rate, 1, 16, {}, 0)))
                        writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
            }

            const auto first = clipAt(0.0, 0.0, 0.5, 60.0f, true, false);
            const auto second = clipAt(0.5, 0.5, 0.5, 72.0f, false, true);
            std::vector<const ClipData*> pair { &first, &second };
            const auto chains = AudioEngine::glideChains(pair);
            const auto groupsThePair = chains.size() == 1 && chains.front().size() == 2
                && chains.front()[0] == &first && chains.front()[1] == &second;

            // Marked, but its source range does not continue the first clip's:
            // a different take, not a split of one continuous one.
            const auto elsewhere = clipAt(0.5, 1.4, 0.5, 72.0f, false, true);
            std::vector<const ClipData*> apart { &first, &elsewhere };
            const auto leavesADifferentTake = AudioEngine::glideChains(apart).empty();

            // A join with nothing on the other side of it is not a phrase.
            std::vector<const ClipData*> lonely { &first };
            const auto leavesALoneClip = AudioEngine::glideChains(lonely).empty();

            // Neither is a pair with no join marked between them.
            const auto plainFirst = clipAt(0.0, 0.0, 0.5, 60.0f, false, false);
            const auto plainSecond = clipAt(0.5, 0.5, 0.5, 72.0f, false, false);
            std::vector<const ClipData*> unmarked { &plainFirst, &plainSecond };
            const auto leavesUnmarkedClips = AudioEngine::glideChains(unmarked).empty();

            // The one request over the pair: it spans both, and its pitch line
            // leaves the seam at the first clip's note and arrives at the
            // second's, instead of stepping between them in one frame.
            TrackData track;
            track.pitchAlgorithm = PitchAlgorithm::nsfHifigan;
            track.renderOrder = RenderOrder::stretchSpliceThenPitch;
            const auto request = AudioEngine::mergedRequestFor(pair, track, {}, {});
            const auto coversBoth = std::abs(request.targetDurationSeconds - 1.0) < 1.0e-6
                && std::abs(request.sourceDurationSeconds - 1.0) < 1.0e-6;
            auto monotonic = !request.timeMap.empty();
            for (std::size_t index = 1; index < request.timeMap.size(); ++index)
                if (request.timeMap[index].targetSeconds < request.timeMap[index - 1].targetSeconds
                    || request.timeMap[index].sourceSeconds < request.timeMap[index - 1].sourceSeconds)
                    monotonic = false;
            constexpr auto seamFrame = static_cast<std::size_t>(100);  // 0.5 s at 5 ms
            const auto atSeam = seamFrame < request.targetMidi.size()
                ? request.targetMidi[seamFrame] : 0.0f;
            const auto afterSeam = seamFrame + 40 < request.targetMidi.size()
                ? request.targetMidi[seamFrame + 40] : 0.0f;
            const auto glidesTheSeam = std::abs(atSeam - 60.0f) < 0.5f
                && std::abs(afterSeam - 72.0f) < 0.5f;

            // End to end: the engine decides to decode one phrase under this
            // order, and none at all under the one that splices.
            ProjectData project;
            TrackData composed;
            composed.id = juce::Uuid().toDashedString();
            composed.compose = true;
            composed.pitchAlgorithm = PitchAlgorithm::nsfHifigan;
            composed.renderOrder = RenderOrder::stretchSpliceThenPitch;
            composed.clips = { first, second };
            project.tracks.push_back(composed);
            AudioEngine engine;
            engine.prepareToPlay(512, 48'000.0);
            engine.syncProject(project);
            const auto mergedPhrases = engine.diagnosticMergedPhraseCount();
            project.tracks.front().renderOrder = RenderOrder::processThenSplice;
            engine.syncProject(project);
            const auto splicedPhrases = engine.diagnosticMergedPhraseCount();
            recording.deleteFile();

            const auto ok = pickerWorks && groupsThePair && leavesADifferentTake && leavesALoneClip
                && leavesUnmarkedClips && coversBoth && monotonic && glidesTheSeam
                && mergedPhrases == 1 && splicedPhrases == 0;
            std::cout << "ui_picker_visible_and_wired=" << (pickerWorks ? 1 : 0)
                      << "|groups_a_joined_pair=" << (groupsThePair ? 1 : 0)
                      << "|leaves_a_different_take=" << (leavesADifferentTake ? 1 : 0)
                      << "|leaves_a_lone_clip=" << (leavesALoneClip ? 1 : 0)
                      << "|leaves_unmarked_clips=" << (leavesUnmarkedClips ? 1 : 0)
                      << "|one_request_covers_both=" << (coversBoth && monotonic ? 1 : 0)
                      << "|pitch_glides_the_seam=" << (glidesTheSeam ? 1 : 0)
                      << " (" << atSeam << " -> " << afterSeam << ")"
                      << "|phrases_merged=" << mergedPhrases
                      << "|phrases_spliced=" << splicedPhrases
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-llsm2-length")
        {
            // LLSM2 refuses a render outright rather than doing it badly, and
            // RenderService then falls back to mld5 -- audible, so the refusal
            // is invisible unless something checks for it.  A held note past
            // the old six-second bound has to come back through LLSM2 itself.
            constexpr auto sampleRate = 44100.0;
            constexpr auto framePeriodMs = 5.0;
            const auto sung = [](double seconds)
            {
                // A voiced tone: a sawtooth has the harmonics LLSM2's layer-1
                // analysis needs to find an F0 and fit a glottal source to.
                const auto count = static_cast<int>(seconds * sampleRate);
                juce::AudioBuffer<float> buffer(1, count);
                for (int sample = 0; sample < count; ++sample)
                {
                    const auto phase = std::fmod(220.0 * sample / sampleRate, 1.0);
                    buffer.setSample(0, sample, static_cast<float>(0.4 * (2.0 * phase - 1.0)));
                }
                return buffer;
            };
            const auto renderSeconds = [&](double seconds)
            {
                const auto source = sung(seconds);
                const auto frames = static_cast<std::size_t>(seconds * 1000.0 / framePeriodMs) + 2;
                const std::vector<float> from(frames, 57.0f);
                const std::vector<float> to(frames, 60.0f);
                const std::vector<float> flat(frames, 0.0f);
                return backend::Llsm2Renderer::render(
                    source, source.getNumSamples(), sampleRate, framePeriodMs,
                    from, to, flat, flat, {});
            };
            const auto sounds = [](const juce::AudioBuffer<float>& buffer)
            {
                return buffer.getNumSamples() > 0 && buffer.getMagnitude(0, buffer.getNumSamples()) > 0.005f;
            };
            // Ten seconds: past the old bound, well inside the new one.
            const auto heldNote = renderSeconds(10.0);
            const auto rendersAHeldNote = sounds(heldNote);
            // And past the new one it still refuses, so the bound is a bound
            // and not just a larger number nothing enforces.
            const auto absurd = renderSeconds(130.0);
            const auto stillBounded = absurd.getNumSamples() == 0;

            std::cout << "a_ten_second_note_renders=" << (rendersAHeldNote ? 1 : 0)
                      << "|samples=" << heldNote.getNumSamples()
                      << "|past_the_cap_is_refused=" << (stillBounded ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(rendersAHeldNote && stillBounded ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 1 && arguments[0] == "--smoke-glide-seam")
        {
            // Melodyne joins two elements with followingJoin.joinsPitches; the
            // importer reads that into the notes on either side.  A seam like
            // that is crossfaded by the mixer, so the neural decoder must not
            // fade its own 3 ms edge into it as well -- two fades over one
            // seam is a dip at every join.  What decides it is one rule, used
            // by the fade and by the guard alike.
            const auto clipAt = [](double start, double length, bool joinsNext,
                                   bool joinsPrevious)
            {
                ClipData clip;
                clip.startSeconds = start;
                clip.durationSeconds = length;
                NoteData note;
                note.connectedToPrevious = joinsPrevious;
                note.connectedToNext = joinsNext;
                clip.notes.push_back(note);
                return clip;
            };
            // Back to back, and both sides say they are joined.
            const auto left = clipAt(0.0, 1.0, true, false);
            const auto right = clipAt(1.0, 1.0, false, true);
            const auto joined = AudioEngine::clipsJoinAt(left, right, true)
                && AudioEngine::clipsJoinAt(right, left, false);

            // Touching but not marked: an ordinary abutment, which still wants
            // the decoder's own edge fade.
            const auto plainLeft = clipAt(0.0, 1.0, false, false);
            const auto plainRight = clipAt(1.0, 1.0, false, false);
            const auto unmarked = !AudioEngine::clipsJoinAt(plainLeft, plainRight, true)
                && !AudioEngine::clipsJoinAt(plainRight, plainLeft, false);

            // Marked but a long way apart: Melodyne places the two elements of
            // a join exactly back to back, so a gap means it is not one.
            const auto farLeft = clipAt(0.0, 1.0, true, false);
            const auto farRight = clipAt(1.5, 1.0, false, true);
            const auto apart = !AudioEngine::clipsJoinAt(farLeft, farRight, true)
                && !AudioEngine::clipsJoinAt(farRight, farLeft, false);

            // Within the two-millisecond tolerance either way.
            const auto nudged = clipAt(1.0015, 1.0, false, true);
            const auto tolerant = AudioEngine::clipsJoinAt(left, nudged, true);
            const auto wide = clipAt(1.004, 1.0, false, true);
            const auto bounded = !AudioEngine::clipsJoinAt(left, wide, true);

            // A clip with no notes says nothing about its seams.
            ClipData bare;
            bare.startSeconds = 1.0;
            bare.durationSeconds = 1.0;
            const auto silentOnBare = !AudioEngine::clipsJoinAt(bare, left, false);

            std::cout << "a_marked_touching_seam_is_a_join=" << (joined ? 1 : 0)
                      << "|an_unmarked_one_is_not=" << (unmarked ? 1 : 0)
                      << "|a_marked_gap_is_not=" << (apart ? 1 : 0)
                      << "|the_tolerance_is_two_ms="
                      << (tolerant && bounded ? 1 : 0)
                      << "|a_clip_with_no_notes_is_not=" << (silentOnBare ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(joined && unmarked && apart && tolerant
                                      && bounded && silentOnBare ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-envelope-base")
        {
            // 包络基础值: scales the whole amplitude envelope without reshaping
            // it, on any track type, persists through save/load, and undoes in
            // one step.  A shared amplitude concept, not a UTAU-only command.
            I18n strings;
            const auto onBothMenus = []
            {
                const auto utau = PianoRollComponent::noteMenuItemsFor(true);
                const auto plain = PianoRollComponent::noteMenuItemsFor(false);
                const auto has = [](const std::vector<int>& v, int id)
                { return std::find(v.begin(), v.end(), id) != v.end(); };
                return has(utau, 23) && has(plain, 23);
            }();

            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-envbase-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=e\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto noteId = project.snapshot().tracks.front()
                .clips.front().notes.front().id;
            const auto baseOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front()
                    .notes.front().amplitudeEnvelopeBasePercent;
            };
            const auto startedAt100 = std::abs(baseOf() - 100.0f) < 1.0e-6f;
            project.setNotesAmplitudeEnvelopeBase({ noteId }, 150.0f);
            const auto setTo150 = std::abs(baseOf() - 150.0f) < 1.0e-4f;

            // The scale is applied in the dB domain: 150% is +3.52 dB.
            const std::vector<AmplitudeEnvelopePoint> flat { { 0.0, 0.0f }, { 0.5, 0.0f } };
            const auto scaled = scaledAmplitudeEnvelope(flat, 150.0f);
            const auto scaledUp = scaled.size() == 2
                && std::abs(scaled[0].gainDb - static_cast<float>(20.0 * std::log10(1.5))) < 0.01f;
            const auto roundTrip = unscaledAmplitudeEnvelope(scaled, 150.0f);
            const auto backToZero = roundTrip.size() == 2
                && std::abs(roundTrip[0].gainDb) < 0.01f;

            // Save and reload: the base survives.
            const auto hjpx = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-envbase-" + juce::Uuid().toDashedString() + ".hjpx");
            juce::String saveError;
            const auto saved = project.save(hjpx, saveError);
            ProjectModel reloaded;
            juce::String loadError;
            const auto loadedOk = saved && reloaded.load(hjpx, loadError);
            hjpx.deleteFile();
            const auto persisted = loadedOk
                && std::abs(reloaded.snapshot().tracks.front().clips.front()
                    .notes.front().amplitudeEnvelopeBasePercent - 150.0f) < 1.0e-4f;

            // One undo step returns it.
            project.undo();
            const auto undoes = std::abs(baseOf() - 100.0f) < 1.0e-6f;

            const auto ok = onBothMenus && startedAt100 && setTo150 && scaledUp
                && backToZero && persisted && undoes;
            std::cout << "on_both_menus=" << (onBothMenus ? 1 : 0)
                      << "|started_at_100=" << (startedAt100 ? 1 : 0)
                      << "|set_to_150=" << (setTo150 ? 1 : 0)
                      << "|scaled_up_in_db=" << (scaledUp ? 1 : 0)
                      << "|round_trip_back=" << (backToZero ? 1 : 0)
                      << "|persisted=" << (persisted ? 1 : 0)
                      << "|undoes=" << (undoes ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-nsf-utau-phrase")
        {
            // The whole-phrase entry point on the one NSF-HiFiGAN renderer:
            // resolve samples, plan, synthesise, overlap-mix.  Without the ONNX
            // model it must warn gracefully (never crash or fall back), and it
            // must be the shared UtauRenderRequest the editor already builds.
            backend::UtauRenderRequest request;
            request.voicebankDirectory = juce::File::getSpecialLocation(
                juce::File::tempDirectory).getChildFile(
                    "hachi-novb-" + juce::Uuid().toDashedString());
            request.targetDurationSeconds = 1.0;
            backend::UtauNoteRenderSpec n1;
            n1.alias = "a"; n1.startSeconds = 0.0; n1.durationSeconds = 0.5; n1.midiNote = 60.0f;
            backend::UtauNoteRenderSpec rest;
            rest.alias = "R"; rest.startSeconds = 0.5; rest.durationSeconds = 0.2; rest.midiNote = 60.0f;
            request.notes = { n1, rest };
            backend::OrtExecutionConfig exec;
            const auto out = backend::renderNsfUtauPhrase(request, juce::File{}, exec);
            const auto warnsNoModel = out.warning.containsIgnoreCase("model")
                && out.buffer.getNumSamples() == 0;
            const auto namedNativeBackend = out.backend.containsIgnoreCase("nsf-hifigan")
                && out.backend.containsIgnoreCase("native");

            const auto ok = warnsNoModel && namedNativeBackend;
            std::cout << "warns_when_model_absent=" << (warnsNoModel ? 1 : 0)
                      << "|native_backend_named=" << (namedNativeBackend ? 1 : 0)
                      << "|backend=" << out.backend << "|warning=" << out.warning
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-nsf-utau-synth")
        {
            // The synthesis entry point composes plan + F0 + neural decode.
            // Without the ONNX model pack it must report a clear error, never
            // fail silently or fall back to another engine.
            using hachi::backend::NsfUtauSampleTiming;
            using hachi::backend::buildNsfUtauNotePlan;
            using hachi::backend::synthesizeNsfUtauNote;
            const auto media = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-nsfutau-" + juce::Uuid().toDashedString() + ".wav");
            {
                constexpr auto rate = 44100.0;
                juce::AudioBuffer<float> tone(1, static_cast<int>(rate * 1.0));
                for (int i = 0; i < tone.getNumSamples(); ++i)
                    tone.setSample(0, i, static_cast<float>(
                        0.2 * std::sin(2.0 * juce::MathConstants<double>::pi
                                       * 220.0 * i / rate)));
                media.deleteFile();
                juce::WavAudioFormat wav;
                if (auto stream = media.createOutputStream())
                    if (auto writer = std::unique_ptr<juce::AudioFormatWriter>(
                            wav.createWriterFor(stream.release(), rate, 1, 16, {}, 0)))
                        writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
            }
            NsfUtauSampleTiming timing;
            timing.offsetSeconds = 0.1; timing.endSeconds = 0.9;
            timing.consonantSeconds = 0.08; timing.preutteranceSeconds = 0.05;
            timing.overlapSeconds = 0.03; timing.fileSeconds = 1.0;
            const auto plan = buildNsfUtauNotePlan(timing, 1.0, 0.5, 1.0, 0.1);
            backend::OrtExecutionConfig exec;
            // No model directory configured -> unavailable, must be reported.
            const auto synth = synthesizeNsfUtauNote(media, plan, 60.0f, {},
                juce::File{}, exec);
            media.deleteFile();
            const auto reportedNotSilent = !synth.usedModel
                && synth.audio.getNumSamples() == 0
                && synth.error.isNotEmpty();
            const auto namesTheModel = synth.error.containsIgnoreCase("model");

            const auto ok = reportedNotSilent && namesTheModel;
            std::cout << "graceful_when_model_absent=" << (reportedNotSilent ? 1 : 0)
                      << "|error_names_model=" << (namesTheModel ? 1 : 0)
                      << "|error=" << synth.error
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-nsf-utau-f0")
        {
            // The note's pitch is the NSF model's F0 input, so the pitch line
            // and vibrato land here as per-frame target MIDI, never a resample.
            // The lead-in before the beat holds the head pitch.
            using hachi::backend::NsfUtauPitchPoint;
            using hachi::backend::buildNsfUtauTargetMidi;
            // A note at MIDI 60 that bends up to +200 cents (one whole tone) by
            // its middle and back.  Output starts 0.05 s before the beat.
            std::vector<NsfUtauPitchPoint> curve {
                { 0.0, 0.0f }, { 0.25, 200.0f }, { 0.5, 0.0f } };
            const auto frames = buildNsfUtauTargetMidi(60.0f, curve, 5.0, 0.55, -0.05);
            const auto frameAt = [&](double outputTime)
            {
                const auto idx = juce::jlimit(0, static_cast<int>(frames.size()) - 1,
                    static_cast<int>(std::lround(outputTime / 0.005)));
                return frames[static_cast<std::size_t>(idx)];
            };
            // Output t=0 is note-local -0.05 (lead-in): holds head pitch 60.
            const auto leadInHoldsHead = std::abs(frameAt(0.0) - 60.0f) < 1.0e-3f;
            // Output t=0.05 is note-local 0.0: pitch 60.
            const auto startAtNote = std::abs(frameAt(0.05) - 60.0f) < 0.02f;
            // Output t=0.30 is note-local 0.25: peak +200 cents = MIDI 62.
            const auto peakBend = std::abs(frameAt(0.30) - 62.0f) < 0.05f;
            // Output t=0.55 is note-local 0.50: back to 60.
            const auto returns = std::abs(frameAt(0.55) - 60.0f) < 0.05f;
            // A note with no pitch curve is flat at its own MIDI.
            const auto flat = buildNsfUtauTargetMidi(67.0f, {}, 5.0, 0.3, 0.0);
            const auto flatHolds = !flat.empty()
                && std::abs(flat.front() - 67.0f) < 1.0e-6f
                && std::abs(flat.back() - 67.0f) < 1.0e-6f;

            const auto ok = leadInHoldsHead && startAtNote && peakBend && returns
                && flatHolds;
            std::cout << "lead_in_holds_head=" << (leadInHoldsHead ? 1 : 0)
                      << "|start_at_note=" << (startAtNote ? 1 : 0)
                      << "|peak_bend_2st=" << (peakBend ? 1 : 0)
                      << "|returns=" << (returns ? 1 : 0)
                      << "|no_curve_flat=" << (flatHolds ? 1 : 0)
                      << "|peak=" << frameAt(0.30)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-nsf-utau-mix")
        {
            // Native overlap crossfade mixing (the wavtool equivalent, done
            // directly rather than through the UTAU path): two touching notes
            // cross over the second note's head overlap, and a rest holds its
            // place without adding audio.
            using hachi::backend::NsfUtauMixNote;
            using hachi::backend::mixNsfUtauNotes;
            constexpr auto rate = 48000.0;
            const auto flat = [](double seconds, float value)
            {
                juce::AudioBuffer<float> b(1, static_cast<int>(rate * seconds));
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, value);
                return b;
            };
            std::vector<NsfUtauMixNote> notes;
            notes.push_back({ flat(0.50, 1.0f), 0.0, 0.0, false });   // 0.00..0.50
            notes.push_back({ flat(0.50, 1.0f), 0.45, 0.10, false }); // fades in 0.45..0.55
            const auto mix = mixNsfUtauNotes(notes, 1.0, rate, 1);
            const auto sampleAt = [&](double t)
            {
                return mix.getSample(0, juce::jlimit(0, mix.getNumSamples() - 1,
                    static_cast<int>(std::lround(t * rate))));
            };
            // The mix is peak-normalised (as UTAU's wavtool is), so test the
            // shape relatively: the two single-note plateaus sit at the same
            // level, and the equal-power crossover stays near that level (no
            // 2x sum bump) rather than at an absolute value.
            const auto steady1 = sampleAt(0.20);   // note 1 only
            const auto steady2 = sampleAt(0.80);   // note 2 only
            // Inside the shared sounding region (note 1's 0.45..0.50 tail under
            // note 2's fade-in), where both notes really overlap.
            const auto mid = sampleAt(0.475);
            const auto beforeOverlap = steady1 > 0.05f;
            const auto afterOverlap = std::abs(steady2 - steady1) < 0.05f * steady1;
            const auto midOverlap = mid > 0.8f * steady1 && mid < 1.5f * steady1;
            const auto noClip = mix.getMagnitude(0, 0, mix.getNumSamples()) <= 0.99f;

            // A rest between two notes contributes no audio but the mix still
            // covers the timeline.
            std::vector<NsfUtauMixNote> withRest;
            withRest.push_back({ flat(0.30, 0.5f), 0.0, 0.0, false });
            withRest.push_back({ {}, 0.30, 0.0, true });
            withRest.push_back({ flat(0.30, 0.5f), 0.60, 0.0, false });
            const auto restMix = mixNsfUtauNotes(withRest, 1.0, rate, 1);
            const auto restSilent = std::abs(restMix.getSample(0,
                static_cast<int>(0.45 * rate))) < 1.0e-4f;
            const auto restCovers = restMix.getNumSamples()
                == static_cast<int>(std::ceil(1.0 * rate));

            const auto ok = beforeOverlap && midOverlap && afterOverlap && noClip
                && restSilent && restCovers;
            std::cout << "before_overlap_note1=" << (beforeOverlap ? 1 : 0)
                      << "|mid_overlap_crossfades=" << (midOverlap ? 1 : 0)
                      << "|after_overlap_note2=" << (afterOverlap ? 1 : 0)
                      << "|no_clip=" << (noClip ? 1 : 0)
                      << "|rest_silent=" << (restSilent ? 1 : 0)
                      << "|rest_covers_timeline=" << (restCovers ? 1 : 0)
                      << "|steady1=" << steady1 << "|mid=" << mid
                      << "|steady2=" << steady2 << "|mid475=" << sampleAt(0.475)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-nsf-utau-plan")
        {
            // Native NSF-HiFiGAN UTAU note planning: the OTO timing becomes a
            // pure time warp (pitch is the model's F0 input, never baked into
            // the map), the consonant stretches by velocity, the vowel fills
            // the note, and the lead-in sits before the beat.  No model needed.
            using hachi::backend::NsfUtauSampleTiming;
            using hachi::backend::buildNsfUtauNotePlan;
            NsfUtauSampleTiming timing;
            timing.offsetSeconds = 0.10;     // region starts 100 ms in
            timing.endSeconds = 0.90;        // region ends at 900 ms
            timing.consonantSeconds = 0.08;  // 80 ms consonant
            timing.preutteranceSeconds = 0.05;
            timing.overlapSeconds = 0.03;
            timing.fileSeconds = 1.20;

            // A 0.5 s note starting at 1.0 s, neutral velocity (scale 1.0),
            // 0.1 s tail for the next note's overlap.
            const auto plan = buildNsfUtauNotePlan(timing, 1.0, 0.5, 1.0, 0.1);
            const auto valid = plan.valid;
            const auto leadInBeforeBeat = std::abs(
                plan.soundStartOffsetSeconds + 0.05) < 1.0e-6;
            const auto sourceRegion = std::abs(plan.sourceStartSeconds - 0.10) < 1.0e-6
                && std::abs(plan.sourceEndSeconds - 0.90) < 1.0e-6;
            const auto output = std::abs(plan.outputSeconds - (0.05 + 0.5 + 0.1)) < 1.0e-6;
            // Map is monotonic in both axes and never resamples for pitch: the
            // last anchor reaches the region end.
            auto monotonic = plan.timeMap.size() >= 2;
            for (std::size_t i = 1; i < plan.timeMap.size(); ++i)
                monotonic = monotonic
                    && plan.timeMap[i].targetSeconds > plan.timeMap[i-1].targetSeconds
                    && plan.timeMap[i].sourceSeconds >= plan.timeMap[i-1].sourceSeconds;
            const auto vowelReachesEnd = !plan.timeMap.empty()
                && std::abs(plan.timeMap.back().sourceSeconds - 0.90) < 1.0e-6
                && std::abs(plan.timeMap.front().sourceSeconds - 0.10) < 1.0e-6;

            // A faster velocity (scale < 1) shortens the consonant's output
            // span, reaching the vowel sooner.
            const auto fast = buildNsfUtauNotePlan(timing, 1.0, 0.5, 0.5, 0.1);
            auto consonantShorter = fast.valid && fast.timeMap.size() >= 3
                && plan.timeMap.size() >= 3
                && fast.timeMap[1].targetSeconds < plan.timeMap[1].targetSeconds + 1.0e-9;

            const auto ok = valid && leadInBeforeBeat && sourceRegion && output
                && monotonic && vowelReachesEnd && consonantShorter;
            std::cout << "valid=" << (valid ? 1 : 0)
                      << "|lead_in_before_beat=" << (leadInBeforeBeat ? 1 : 0)
                      << "|source_region=" << (sourceRegion ? 1 : 0)
                      << "|output_len=" << (output ? 1 : 0)
                      << "|map_monotonic_no_pitch_resample=" << (monotonic ? 1 : 0)
                      << "|vowel_reaches_end=" << (vowelReachesEnd ? 1 : 0)
                      << "|velocity_shortens_consonant=" << (consonantShorter ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-render-capability")
        {
            // Editing never hides a feature; the difference shows only at
            // render, as a warning naming the edit the backend cannot honour.
            // A UTAU track warns about nothing (its backend honours all of it);
            // the same edits on a non-UTAU track are named, one per feature.
            ProjectModel project;
            const auto ust = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-cap-" + juce::Uuid().toDashedString() + ".ust");
            ust.replaceWithText("[#VERSION]\r\nUST Version1.2\r\n[#SETTING]\r\n"
                                "Tempo=120.00\r\nTracks=1\r\nProjectName=c\r\n"
                                "[#0000]\r\nLength=960\r\nLyric=a\r\nNoteNum=60\r\n"
                                "[#TRACKEND]\r\n");
            juce::String ustError;
            juce::StringArray ustWarnings;
            const auto built = project.addUstFile(ust, ustError, ustWarnings);
            ust.deleteFile();
            if (!built)
            {
                std::cout << "built=0|error=" << ustError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            const auto noteId = project.snapshot().tracks.front()
                .clips.front().notes.front().id;
            // A UTAU flag on the note: an edit only the UTAU backend renders.
            project.setNoteUtauFlags(noteId, "g-5");

            // On a UTAU track, nothing is unrenderable.
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            const auto onUtau = AudioEngine::renderCapabilityWarnings(project.snapshot());
            const auto utauSilent = onUtau.isEmpty();

            // The identical edit on an NSF track is named as unrenderable.
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::nsfHifigan);
            const auto onNsf = AudioEngine::renderCapabilityWarnings(project.snapshot());
            const auto nsfWarns = onNsf.size() == 1
                && onNsf[0].contains("flags");

            const auto ok = utauSilent && nsfWarns;
            std::cout << "utau_backend_silent=" << (utauSilent ? 1 : 0)
                      << "|nsf_names_the_flag=" << (nsfWarns ? 1 : 0)
                      << "|utau_count=" << onUtau.size()
                      << "|nsf_count=" << onNsf.size()
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-asset-register-dir")
        {
            // Register an existing on-disk folder as a material folder (the
            // "register into <name>" path, e.g. a "tt" folder), list its
            // members, and confirm reading bare audio derives timing in memory
            // without writing any oto.ini or HJM sidecar into the folder.
            I18n strings;
            const juce::File folder(arguments[1].unquoted());
            const auto propsFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-tt-" + juce::Uuid().toDashedString() + ".settings");
            juce::PropertiesFile::Options options;
            options.applicationName = "hachi-asset-tt-smoke";
            options.filenameSuffix = "settings";
            options.folderName = propsFile.getParentDirectory().getFullPathName();
            options.storageFormat = juce::PropertiesFile::storeAsXML;
            juce::PropertiesFile properties(propsFile, options);

            // Snapshot which sidecars/oto exist before registering, so we can
            // prove registration + reading created none.
            juce::Array<juce::File> before;
            folder.findChildFiles(before, juce::File::findFiles, false, "*.hjm");
            const auto otoBefore = folder.getChildFile("oto.ini").existsAsFile();

            AssetManagerComponent manager(strings, properties);
            const auto index = manager.diagnosticRegisterFolder(folder);
            const auto members = manager.diagnosticMembersOf(index);

            juce::Array<juce::File> after;
            folder.findChildFiles(after, juce::File::findFiles, false, "*.hjm");
            const auto otoAfter = folder.getChildFile("oto.ini").existsAsFile();
            const auto wroteNothing = after.size() == before.size()
                && otoAfter == otoBefore;

            const auto ok = index >= 0 && members.size() > 0 && wroteNothing;
            std::cout << "registered=" << (index >= 0 ? 1 : 0)
                      << "|members=" << members.size()
                      << "|wrote_no_sidecar=" << (wroteNothing ? 1 : 0)
                      << "|folder=" << folder.getFileName()
                      << std::endl;
            propsFile.deleteFile();
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-asset-manager")
        {
            // The material manager: a folder is registered, audio added to it
            // gets its parameters detected at once, and lyrics assemble into an
            // ordered material sequence by pinyin -- the from-scratch workflow.
            I18n strings;
            const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("hachi-assets-" + juce::Uuid().toDashedString());
            root.createDirectory();
            const auto writeTone = [](const juce::File& file, double seconds)
            {
                constexpr auto rate = 44100.0;
                juce::AudioBuffer<float> tone(1, static_cast<int>(rate * seconds));
                for (int sample = 0; sample < tone.getNumSamples(); ++sample)
                    tone.setSample(0, sample, static_cast<float>(
                        0.2 * std::sin(2.0 * juce::MathConstants<double>::pi
                                       * 200.0 * sample / rate)));
                file.deleteFile();
                juce::WavAudioFormat wav;
                if (auto stream = file.createOutputStream())
                    if (auto writer = std::unique_ptr<juce::AudioFormatWriter>(
                            wav.createWriterFor(stream.release(), rate, 1, 16, {}, 0)))
                        writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
            };
            // A source "voicebank" folder named in pinyin, and a loose file.
            const auto bank = root.getChildFile("bank");
            bank.createDirectory();
            for (const auto* stem : { "wo", "xiang", "yao" })
                writeTone(bank.getChildFile(juce::String(stem) + ".wav"), 0.4);
            const auto loose = root.getChildFile("loose.wav");
            writeTone(loose, 0.4);
            const auto scratch = root.getChildFile("scratch");
            scratch.createDirectory();

            const auto propsFile = root.getChildFile("props.settings");
            juce::PropertiesFile::Options options;
            options.applicationName = "hachi-asset-smoke";
            options.filenameSuffix = "settings";
            options.folderName = propsFile.getParentDirectory().getFullPathName();
            options.storageFormat = juce::PropertiesFile::storeAsXML;
            juce::PropertiesFile properties(propsFile, options);

            AssetManagerComponent manager(strings, properties);
            // Register the bank folder and read its members.
            const auto bankIndex = manager.diagnosticRegisterFolder(bank);
            const auto bankMembers = manager.diagnosticMembersOf(bankIndex);
            const auto registeredBank = bankIndex >= 0 && bankMembers.size() == 3;

            // A from-scratch folder: add the loose file, parameters detected.
            const auto scratchIndex = manager.diagnosticRegisterFolder(scratch);
            const auto added = manager.diagnosticAddAudioToFolder(scratchIndex,
                { loose.getFullPathName() });
            const auto scratchMember = scratch.getChildFile("loose.wav");
            const auto detectedParams = added == 1
                && SampleSettings::sidecarFor(scratchMember).existsAsFile();

            // 活字印刷: assemble "我想要" from the bank folder.
            int matched = 0, missing = 0;
            juce::String assembleError;
            const auto assembled = manager.diagnosticAssembleLyrics(bankIndex,
                juce::String::fromUTF8("\xe6\x88\x91\xe6\x83\xb3\xe8\xa6\x81"),
                "song", matched, missing, assembleError);
            juce::Array<juce::File> ordered;
            if (assembled != juce::File{})
                assembled.findChildFiles(ordered, juce::File::findFiles, false, "*.wav");
            const auto assembledInOrder = assembled != juce::File{}
                && matched == 3 && missing == 0 && ordered.size() == 3;
            auto firstIsWo = false;
            if (!ordered.isEmpty())
            {
                ordered.sort();
                firstIsWo = ordered[0].getFileName().startsWith("001_")
                    && ordered[0].getFileName().contains("wo");
            }
            // The assembled sequence is itself registered for reuse next time.
            if (assembled != juce::File{}) manager.diagnosticRegisterFolder(assembled);
            const auto reusable = manager.diagnosticFolderCount() >= 3;

            // 另存为 OTO: the bank folder's native parameters export to one
            // oto.ini, and reading OTO earlier wrote no HJM sidecar (OTO stays
            // authoritative, converted to native annotation only in memory).
            const auto otoOut = root.getChildFile("exported-oto.ini");
            juce::String otoError;
            const auto otoRows = manager.diagnosticExportFolderAsOto(bankIndex,
                otoOut, otoError);
            const auto exportedOto = otoRows == 3 && otoOut.existsAsFile();
            auto bankHasNoHjm = true;
            for (const auto& stem : { "wo", "xiang", "yao" })
                if (SampleSettings::sidecarFor(
                        bank.getChildFile(juce::String(stem) + ".wav")).existsAsFile())
                    bankHasNoHjm = false;

            // Native parameter editing on a from-scratch material writes back
            // to its HJM sidecar (never an oto.ini), and the change round-trips.
            juce::String nativeError;
            const auto nativeSaved = AssetManagerComponent::diagnosticWriteNativeTiming(
                scratchMember, 20.0, 30.0, 40.0, 25.0, 15.0, nativeError);
            const auto reread = SampleSettings::loadOrDerive(scratchMember, ProjectData{});
            const auto nativeRoundTrips = nativeSaved && !reread.empty()
                && std::abs(reread.front().regionStartSeconds - 0.02) < 1.0e-4
                && !scratch.getChildFile("oto.ini").existsAsFile();

            root.deleteRecursively();
            const auto ok = registeredBank && detectedParams && assembledInOrder
                && firstIsWo && reusable && exportedOto && bankHasNoHjm
                && nativeRoundTrips;
            std::cout << "registered_bank=" << (registeredBank ? 1 : 0)
                      << "|detected_params_on_add=" << (detectedParams ? 1 : 0)
                      << "|assembled_in_order=" << (assembledInOrder ? 1 : 0)
                      << "|first_is_wo=" << (firstIsWo ? 1 : 0)
                      << "|assembly_reusable=" << (reusable ? 1 : 0)
                      << "|exported_oto=" << (exportedOto ? 1 : 0)
                      << "|read_wrote_no_hjm=" << (bankHasNoHjm ? 1 : 0)
                      << "|native_edit_round_trips=" << (nativeRoundTrips ? 1 : 0)
                      << "|matched=" << matched << "|missing=" << missing
                      << std::endl;
            setApplicationReturnValue(ok ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-gap-menu")
        {
            // The silence between two notes is a thing you can right-click.
            // Closing it draws the two together and pulls the rest of the
            // track forward; opening a note into it pushes the rest back.
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();

            const auto notesOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front().notes;
            };
            const auto clipLength = [&project]
            {
                return project.snapshot().tracks.front().clips.front().durationSeconds;
            };
            const auto startOf = [&notesOf](const juce::String& id)
            {
                for (const auto& note : notesOf())
                    if (note.id == id) return note.startSeconds;
                return -1.0;
            };

            // The fixture is a note every half second, each half a second
            // long, so shorten one to open a silence behind it.
            constexpr std::size_t before = 3;
            const auto beforeId = notesOf()[before].id;
            const auto nextId = notesOf()[before + 1].id;
            const auto laterId = notesOf()[before + 3].id;
            project.resizeNote(beforeId, notesOf()[before].startSeconds, 0.2);
            roll.diagnosticRefresh();
            const auto gapFrom = notesOf()[before].startSeconds + 0.2;
            const auto gapTo = startOf(nextId);
            const auto gap = gapTo - gapFrom;

            // Found by pointing at it, and only there: over a note there is
            // no gap, and neither is there past the last note.
            const auto pointAt = [&roll](double seconds)
            {
                return juce::Point<float>(
                    roll.diagnosticEdgeX(seconds), roll.diagnosticNoteY(0));
            };
            const auto found = roll.diagnosticGapAt(pointAt((gapFrom + gapTo) / 2));
            const auto overANote = roll.diagnosticGapAt(
                pointAt(notesOf()[before].startSeconds + 0.1));
            const auto namesTheNeighbours = found.startsWith(beforeId + ".." + nextId);
            const auto onlyInTheSilence = overANote.isEmpty();

            // Closing it: the two notes meet and the rest of the track follows.
            const auto laterWas = startOf(laterId);
            const auto lengthWas = clipLength();
            roll.diagnosticCloseGapAt(pointAt((gapFrom + gapTo) / 2));
            roll.diagnosticRefresh();
            const auto met = std::abs(startOf(nextId) - gapFrom) < 1.0e-9;
            const auto restCameForward = std::abs(startOf(laterId)
                                                  - (laterWas - gap)) < 1.0e-9;
            const auto trackShrank = std::abs(clipLength()
                                              - (lengthWas - gap)) < 1.0e-9;
            const auto beforeStayed = std::abs(startOf(beforeId)
                - notesOf()[before].startSeconds) < 1.0e-9;

            // And opening a note into a silence.  The two are touching now,
            // so one has to be reopened -- shorter than before, or shortening
            // it to the length it already has leaves them touching still.
            project.resizeNote(beforeId, startOf(beforeId), 0.1);
            roll.diagnosticRefresh();
            const auto reopenedFrom = startOf(beforeId) + 0.1;
            const auto laterBefore = startOf(laterId);
            const auto lengthBefore = clipLength();
            const auto countBefore = notesOf().size();
            const auto added = roll.diagnosticInsertNoteAt(
                pointAt(reopenedFrom + (startOf(nextId) - reopenedFrom) / 2));
            roll.diagnosticRefresh();
            const auto quarterBar = roll.diagnosticQuarterBarSeconds(beforeId);
            auto placed = added.isNotEmpty() && notesOf().size() == countBefore + 1;
            for (const auto& note : notesOf())
                if (note.id == added)
                    placed = placed
                        && std::abs(note.startSeconds - reopenedFrom) < 1.0e-9
                        && std::abs(note.durationSeconds - quarterBar) < 1.0e-9
                        && std::abs(note.midiNote
                                    - notesOf()[before].midiNote) < 1.0e-6;
            const auto restWentBack = std::abs(startOf(laterId)
                                               - (laterBefore + quarterBar)) < 1.0e-9;
            const auto trackGrew = std::abs(clipLength()
                                            - (lengthBefore + quarterBar)) < 1.0e-9;

            std::cout << "names_the_neighbours=" << (namesTheNeighbours ? 1 : 0)
                      << "|only_in_the_silence=" << (onlyInTheSilence ? 1 : 0)
                      << "|closing_draws_them_together=" << (met ? 1 : 0)
                      << "|the_rest_came_forward=" << (restCameForward ? 1 : 0)
                      << "|the_track_shrank=" << (trackShrank ? 1 : 0)
                      << "|what_was_in_front_stayed=" << (beforeStayed ? 1 : 0)
                      << "|the_note_lands_in_the_silence=" << (placed ? 1 : 0)
                      << "|the_rest_went_back=" << (restWentBack ? 1 : 0)
                      << "|the_track_grew=" << (trackGrew ? 1 : 0)
                      << "|gap=" << juce::String(gap, 4)
                      << " quarter_bar=" << juce::String(quarterBar, 4)
                      << " found=[" << found << "]" << std::endl;
            setApplicationReturnValue(namesTheNeighbours && onlyInTheSilence && met
                                      && restCameForward && trackShrank
                                      && beforeStayed && placed && restWentBack
                                      && trackGrew ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-insert-gap")
        {
            // A silence opened in front of a note.  What was before it stays,
            // the note itself and everything after move later by the amount
            // asked for, and the track grows by it -- the inverse of the
            // timing delete, which closes one up.
            I18n strings;
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = project.snapshot().tracks.front().id;
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            PianoRollComponent roll(project, strings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();

            const auto clipOf = [&project]
            {
                return project.snapshot().tracks.front().clips.front();
            };
            const auto layout = [&clipOf]
            {
                std::vector<std::pair<juce::String, double>> times;
                for (const auto& note : clipOf().notes)
                    times.emplace_back(note.id, note.startSeconds);
                return times;
            };
            const auto startOf = [&clipOf](const juce::String& id)
            {
                for (const auto& note : clipOf().notes)
                    if (note.id == id) return note.startSeconds;
                return -1.0;
            };

            // Not the first note, so there is something in front to hold still.
            constexpr std::size_t probe = 3;
            const auto noteId = clipOf().notes[probe].id;
            const auto before = layout();
            const auto wasDuration = clipOf().durationSeconds;
            const auto at = startOf(noteId);

            // Offered in every UTAU mode, and nowhere else.
            auto offeredEverywhere = true;
            for (const auto mode : { UtauMode::classic, UtauMode::jie, UtauMode::mou })
            {
                project.setTrackUtauMode(trackId, mode);
                roll.diagnosticRefresh();
                offeredEverywhere = offeredEverywhere
                    && roll.diagnosticGapItemsEnabled(noteId);
            }
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::world);
            roll.diagnosticRefresh();
            offeredEverywhere = offeredEverywhere
                && !roll.diagnosticGapItemsEnabled(noteId);
            project.setTrackPitchAlgorithm(trackId, PitchAlgorithm::utau);
            roll.diagnosticRefresh();

            // The one-click amount is a quarter bar, and the dialog counts in
            // 128ths of a quarter note: a quarter bar of 4/4 is 128 of them.
            const auto unit = roll.diagnosticGapUnitSeconds(noteId);
            const auto quarterBar = roll.diagnosticQuarterBarSeconds(noteId);
            const auto unitsPerQuarterBar = quarterBar / unit;
            const auto unitsAreRight = std::abs(unitsPerQuarterBar - 128.0) < 1.0e-6;

            project.insertGapBeforeNote(noteId, quarterBar);
            auto beforeHeld = true, fromHereMoved = true;
            for (const auto& entry : before)
            {
                const auto now = startOf(entry.first);
                const auto want = entry.second >= at - 1.0e-9
                    ? entry.second + quarterBar : entry.second;
                (entry.second >= at - 1.0e-9 ? fromHereMoved : beforeHeld)
                    = (entry.second >= at - 1.0e-9 ? fromHereMoved : beforeHeld)
                      && std::abs(now - want) < 1.0e-9;
            }
            const auto sameCount = layout().size() == before.size();
            const auto grew = std::abs(clipOf().durationSeconds
                                       - (wasDuration + quarterBar)) < 1.0e-9;

            // And a length given in units lands on exactly that many.
            const auto second = clipOf().notes[probe].id;
            const auto beforeUnits = startOf(second);
            project.insertGapBeforeNote(second, 5 * unit);
            const auto byUnits = std::abs(startOf(second)
                                          - (beforeUnits + 5 * unit)) < 1.0e-9;
            // Nothing is inserted for a length of nothing.
            const auto settled = startOf(second);
            project.insertGapBeforeNote(second, 0.0);
            const auto zeroIsInert = std::abs(startOf(second) - settled) < 1.0e-9;

            std::cout << "offered_in_every_utau_mode=" << (offeredEverywhere ? 1 : 0)
                      << "|unit_is_a_128th_of_a_quarter=" << (unitsAreRight ? 1 : 0)
                      << "|before_it_held=" << (beforeHeld ? 1 : 0)
                      << "|from_there_on_moved=" << (fromHereMoved ? 1 : 0)
                      << "|no_note_added_or_lost=" << (sameCount ? 1 : 0)
                      << "|the_track_grew=" << (grew ? 1 : 0)
                      << "|a_length_in_units_lands=" << (byUnits ? 1 : 0)
                      << "|zero_does_nothing=" << (zeroIsInert ? 1 : 0)
                      << "|unit=" << juce::String(unit, 6)
                      << " quarter_bar=" << juce::String(quarterBar, 6)
                      << " units_per_quarter_bar="
                      << juce::String(unitsPerQuarterBar, 3)
                      << std::endl;
            setApplicationReturnValue(offeredEverywhere && unitsAreRight
                                      && beforeHeld && fromHereMoved
                                      && sameCount && grew && byUnits
                                      && zeroIsInert ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-ripple-delete")
        {
            // Deleting a note along with the time it took up: what followed
            // moves back to meet what came before, and the clip loses that
            // much length.  The inverse of pasting a phrase in, and what
            // undoes one.
            I18n cutStrings;
            ProjectModel cutProject;
            juce::String cutError;
            if (!cutProject.load(juce::File(arguments[1].unquoted()), cutError))
            {
                std::cout << "loaded=0|error=" << cutError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto firstClip = [&cutProject]
            {
                return cutProject.snapshot().tracks.front().clips.front();
            };
            const auto clip = firstClip();
            const auto clipDuration = clip.durationSeconds;
            const auto layout = [&firstClip]
            {
                std::vector<std::pair<juce::String, double>> times;
                const auto now = firstClip();
                for (const auto& note : now.notes)
                    times.emplace_back(note.id, note.startSeconds);
                return times;
            };
            const auto startOf = [&](const juce::String& id)
            {
                const auto now = firstClip();
                for (const auto& note : now.notes)
                    if (note.id == id) return note.startSeconds;
                return -1.0;
            };
            const auto before = layout();
            // Notes are half a second each from zero.  Take the two at 1.5 and
            // 2.0, so the stretch removed is a whole second.
            juce::String cutA, cutB, ahead, behind;
            for (const auto& entry : before)
            {
                if (std::abs(entry.second - 1.5) < 1.0e-9) cutA = entry.first;
                if (std::abs(entry.second - 2.0) < 1.0e-9) cutB = entry.first;
                if (std::abs(entry.second - 2.5) < 1.0e-9) ahead = entry.first;
                if (std::abs(entry.second - 1.0) < 1.0e-9) behind = entry.first;
            }
            const auto behindStart = startOf(behind);
            cutProject.removeNotesRippling({ cutA, cutB });
            const auto after = layout();

            const auto span = 1.0;
            const auto gone = startOf(cutA) < 0.0 && startOf(cutB) < 0.0;
            const auto twoFewer = after.size() == before.size() - 2;
            // What followed has closed up against what came before.
            const auto closedUp = ahead.isNotEmpty()
                && std::abs(startOf(ahead) - (2.5 - span)) < 1.0e-9;
            const auto behindStayed = behind.isNotEmpty()
                && std::abs(startOf(behind) - behindStart) < 1.0e-9;
            // Everything after moved by the same amount, not just the first.
            auto allShifted = true;
            for (const auto& entry : before)
            {
                if (entry.first == cutA || entry.first == cutB) continue;
                const auto now = startOf(entry.first);
                const auto want = entry.second >= 2.5 - 1.0e-9
                    ? entry.second - span : entry.second;
                allShifted = allShifted && std::abs(now - want) < 1.0e-9;
            }
            const auto shorter = firstClip().durationSeconds
                <= clipDuration - span + 1.0e-9;

            std::cout << "notes_before=" << before.size()
                      << "|notes_after=" << after.size()
                      << "|clip=" << juce::String(clipDuration, 3) << "->"
                      << juce::String(firstClip().durationSeconds, 3)
                      << "|deleted=" << (gone ? 1 : 0)
                      << "|two_fewer=" << (twoFewer ? 1 : 0)
                      << "|closed_up=" << (closedUp ? 1 : 0)
                      << "|before_it_stayed=" << (behindStayed ? 1 : 0)
                      << "|all_after_moved_alike=" << (allShifted ? 1 : 0)
                      << "|clip_shorter=" << (shorter ? 1 : 0) << std::endl;
            setApplicationReturnValue(
                gone && twoFewer && closedUp && behindStayed && allShifted
                    && shorter && before.size() > 10 ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-paste-notes")
        {
            // Three things, measured in this order because each one changes
            // the clip for the next: where a paste decides to go, what a paste
            // onto its own track does to what it lands on, and what a paste
            // carried to another track replaces.
            I18n pasteStrings;
            ProjectModel pasteProject;
            juce::String pasteError;
            if (!pasteProject.load(juce::File(arguments[1].unquoted()), pasteError))
            {
                std::cout << "loaded=0|error=" << pasteError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            // By value, every time: the snapshot comes back by value and
            // front() is a call, so anything reaching through it into the
            // temporary is dangling before it can be read.
            const auto firstClip = [&pasteProject]
            {
                return pasteProject.snapshot().tracks.front().clips.front();
            };
            const auto clip = firstClip();
            const auto clipId = clip.id;
            const auto clipStart = clip.startSeconds;
            const auto layout = [&firstClip]
            {
                std::vector<std::pair<juce::String, double>> times;
                const auto now = firstClip();
                for (const auto& note : now.notes)
                    times.emplace_back(note.id, note.startSeconds);
                return times;
            };
            const auto startOf = [&](const juce::String& id)
            {
                const auto now = firstClip();
                for (const auto& note : now.notes)
                    if (note.id == id) return clipStart + note.startSeconds;
                return -1.0;
            };
            // Two notes, half a second each, held relative to the first.
            const auto phraseLength = 1.0;
            std::vector<NoteData> phrase;
            for (int index = 0; index < 2; ++index)
            {
                NoteData note;
                note.startSeconds = index * 0.5;
                note.durationSeconds = 0.5;
                note.midiNote = 70.0f + index;
                note.label = "pasted";
                phrase.push_back(note);
            }

            // 1. Where it goes when nothing says otherwise.
            constexpr auto playhead = 7.25;
            constexpr auto origin = 3.5;
            constexpr auto selected = 9.0;
            const auto across = MainComponent::pasteTargetSeconds(
                false, playhead, origin, selected, std::nullopt, std::nullopt);
            const auto within = MainComponent::pasteTargetSeconds(
                true, playhead, origin, selected, std::nullopt, std::nullopt);
            const auto withinBare = MainComponent::pasteTargetSeconds(
                true, playhead, origin, std::nullopt, std::nullopt, std::nullopt);
            const auto askedWithin = MainComponent::pasteTargetSeconds(
                true, playhead, origin, selected, 1.25, std::nullopt);
            const auto negative = MainComponent::pasteTargetSeconds(
                false, playhead, -4.0, std::nullopt, std::nullopt, std::nullopt);
            const auto ruleHolds = std::abs(across - origin) < 1.0e-9
                && std::abs(within - selected) < 1.0e-9
                && std::abs(withinBare - playhead) < 1.0e-9
                && std::abs(askedWithin - 1.25) < 1.0e-9
                && negative == 0.0;

            // 1b. The pointer wins over the selection and the playhead, on
            //     either track.  It has to: after a copy the selection is the
            //     notes that were copied, so pasting at the selection put the
            //     copy exactly on top of its own original -- and where notes
            //     are placed rather than pushed along, that looked like the
            //     paste had done nothing at all.
            constexpr auto pointer = 5.75;
            const auto pointedWithin = MainComponent::pasteTargetSeconds(
                true, playhead, origin, selected, std::nullopt, pointer);
            const auto pointedAcross = MainComponent::pasteTargetSeconds(
                false, playhead, origin, selected, std::nullopt, pointer);
            // An explicit request still outranks it, which is what Shift does.
            const auto askedBeatsPointer = MainComponent::pasteTargetSeconds(
                true, playhead, origin, selected, 1.25, pointer);
            const auto pointerRuleHolds =
                std::abs(pointedWithin - pointer) < 1.0e-9
                && std::abs(pointedAcross - pointer) < 1.0e-9
                && std::abs(askedBeatsPointer - 1.25) < 1.0e-9;

            // 2. Onto its own track: in, not over.  Notes are half a second
            // each from zero, so this lands on the one at 1.5.
            const auto beforeRipple = layout();
            const auto landOn = clipStart + 1.5;
            juce::String landedOn, neighbour;
            for (const auto& entry : beforeRipple)
            {
                const auto at = clipStart + entry.second;
                if (std::abs(at - landOn) < 1.0e-9) landedOn = entry.first;
                if (std::abs(at - (landOn - 0.5)) < 1.0e-9) neighbour = entry.first;
            }
            const auto neighbourStart = startOf(neighbour);
            const auto rippled = pasteProject.insertNotes(clipId, phrase, landOn);
            const auto afterRipple = layout();
            const auto pushedAlong = landedOn.isNotEmpty()
                && std::abs(startOf(landedOn) - (landOn + phraseLength)) < 1.0e-9;
            const auto neighbourStayed = neighbour.isNotEmpty()
                && std::abs(startOf(neighbour) - neighbourStart) < 1.0e-9;
            const auto wentInFront = rippled.size() == 2
                && std::abs(startOf(rippled.front()) - landOn) < 1.0e-9;
            const auto grew = afterRipple.size() == beforeRipple.size() + 2;

            // 3. Carried to another track: over, not in.  A stretch that ends
            // exactly where one note begins and begins exactly where another
            // ends, so finding two rather than four says touching is not
            // overlapping.
            const auto beforeOverwrite = layout();
            const auto from = clipStart + 12.0;
            const auto to = from + phraseLength;
            const auto hit = pasteProject.notesOverlapping(clipId, from, to);
            auto hitTheRightTwo = hit.size() == 2;
            for (const auto& id : hit)
            {
                const auto at = startOf(id);
                hitTheRightTwo = hitTheRightTwo
                    && (std::abs(at - from) < 1.0e-9
                        || std::abs(at - (from + 0.5)) < 1.0e-9);
            }
            pasteProject.removeNotes(hit);
            const auto inserted = pasteProject.insertNotes(clipId, phrase, from);
            const auto after = layout();
            const auto insertedTwo = inserted.size() == 2;
            const auto landedRight = insertedTwo
                && std::abs(startOf(inserted.front()) - from) < 1.0e-9
                && std::abs(startOf(inserted.back()) - (from + 0.5)) < 1.0e-9;
            // Two out, two in, and everything not replaced still where it was.
            auto untouched = after.size() == beforeOverwrite.size();
            for (const auto& entry : beforeOverwrite)
            {
                if (std::find(hit.begin(), hit.end(), entry.first) != hit.end())
                    continue;
                const auto still = std::find_if(after.begin(), after.end(),
                    [&entry](const auto& candidate)
                    {
                        return candidate.first == entry.first;
                    });
                untouched = untouched && still != after.end()
                    && std::abs(still->second - entry.second) < 1.0e-9;
            }

            std::cout << "across_tracks=" << juce::String(across, 3)
                      << "|within_track=" << juce::String(within, 3)
                      << "|within_bare=" << juce::String(withinBare, 3)
                      << "|paste_target_rule=" << (ruleHolds ? 1 : 0)
                      << "|pointer_wins_over_selection=" << (pointerRuleHolds ? 1 : 0)
                      << "|pushed_along=" << (pushedAlong ? 1 : 0)
                      << "|neighbour_stayed=" << (neighbourStayed ? 1 : 0)
                      << "|went_in_front=" << (wentInFront ? 1 : 0)
                      << "|piece_grew=" << (grew ? 1 : 0)
                      << "|overlapping=" << hit.size()
                      << "|notes_before=" << beforeOverwrite.size()
                      << "|notes_after=" << after.size()
                      << "|found_what_it_lands_on=" << (hitTheRightTwo ? 1 : 0)
                      << "|pasted_both=" << (insertedTwo ? 1 : 0)
                      << "|landed_where_asked=" << (landedRight ? 1 : 0)
                      << "|rest_unmoved=" << (untouched ? 1 : 0) << std::endl;
            setApplicationReturnValue(
                ruleHolds && pointerRuleHolds && pushedAlong && neighbourStayed && wentInFront && grew
                    && hitTheRightTwo && insertedTwo && landedRight && untouched
                    && beforeRipple.size() > 30 ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-consonant-edge")
        {
            // The consonant ends where the note starts, whatever the consonant
            // velocity: velocity decides where it begins, never where it ends.
            // Measured on the boundary the roll actually draws.
            I18n edgeStrings;
            ProjectModel edgeProject;
            juce::String edgeError;
            if (!edgeProject.load(juce::File(arguments[1].unquoted()), edgeError))
            {
                std::cout << "loaded=0|error=" << edgeError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto trackId = edgeProject.snapshot().tracks.front().id;
            edgeProject.setTrackUtauMode(trackId, UtauMode::jie);
            PianoRollComponent edgeRoll(edgeProject, edgeStrings);
            edgeRoll.setBounds(0, 0, 1600, 900);
            edgeRoll.resized();
            constexpr std::size_t probe = 2;
            const auto noteId = edgeProject.snapshot()
                .tracks.front().clips.front().notes[probe].id;
            const auto noteStart = edgeProject.snapshot()
                .tracks.front().clips.front().notes[probe].startSeconds;
            auto worst = 0.0;
            auto measured = 0;
            for (const auto velocity : { 20, 50, 100, 150, 200, 300 })
            {
                edgeProject.setNotesUtauConsonantVelocity({ noteId }, velocity);
                edgeRoll.diagnosticRefresh();
                const auto edges = edgeRoll.diagnosticRegionEdges(probe);
                if (edges[0] < 0.0) continue;
                const auto& note = edgeRoll.diagnosticNotes()[probe];
                ++measured;
                worst = std::max(worst, std::abs(edges[0] - note.start));
                std::cout << "  v" << velocity
                          << " sounding=" << juce::String(note.soundingStart, 4)
                          << " consonant_ends=" << juce::String(edges[0], 4)
                          << " note_starts=" << juce::String(note.start, 4)
                          << " off_by=" << juce::String(edges[0] - note.start, 4)
                          << std::endl;
            }
            edgeProject.setNotesUtauConsonantVelocity(
                { noteId }, inheritedUtauConsonantVelocity);
            edgeRoll.diagnosticRefresh();

            // And while the handle is being dragged, which is where it went
            // wrong: the block follows the drag at once, so the boundaries
            // inside it have to as well.  Lengthening is the worse direction,
            // so both are tried.
            auto worstDragging = 0.0;
            auto dragged = 0;
            const auto settled = edgeRoll.diagnosticNotes()[probe];
            const auto settledLeadIn = settled.start - settled.soundingStart;
            for (const auto factor : { 0.4, 0.7, 1.0, 1.6, 2.5 })
            {
                edgeRoll.diagnosticBeginConsonantDrag(noteId, settledLeadIn * factor);
                const auto edges = edgeRoll.diagnosticRegionEdges(probe);
                if (edges[0] < 0.0) continue;
                ++dragged;
                worstDragging = std::max(worstDragging,
                                         std::abs(edges[0] - settled.start));
                std::cout << "  dragging to " << juce::String(settledLeadIn * factor, 4)
                          << " consonant_ends=" << juce::String(edges[0], 4)
                          << " off_by=" << juce::String(edges[0] - settled.start, 4)
                          << std::endl;
            }

            // And when the note is made longer or shorter: the consonant
            // takes no part in that, so neither its length nor where it ends
            // may move.
            const auto noteDuration = edgeProject.snapshot()
                .tracks.front().clips.front().notes[probe].durationSeconds;
            auto worstLength = 0.0;
            auto worstEndByLength = 0.0;
            auto lengths = 0;
            auto firstOnset = -1.0;
            for (const auto factor : { 0.5, 1.0, 2.0, 4.0 })
            {
                edgeProject.resizeNote(noteId, edgeProject.snapshot()
                    .tracks.front().clips.front().notes[probe].startSeconds,
                    noteDuration * factor);
                edgeRoll.diagnosticRefresh();
                const auto edges = edgeRoll.diagnosticRegionEdges(probe);
                if (edges[0] < 0.0) continue;
                const auto& note = edgeRoll.diagnosticNotes()[probe];
                const auto onset = edges[0] - note.soundingStart;
                if (firstOnset < 0.0) firstOnset = onset;
                ++lengths;
                worstLength = std::max(worstLength, std::abs(onset - firstOnset));
                worstEndByLength = std::max(worstEndByLength,
                                            std::abs(edges[0] - note.start));
                std::cout << "  note x" << factor
                          << " onset=" << juce::String(onset, 4)
                          << " ends=" << juce::String(edges[0], 4)
                          << " note_starts=" << juce::String(note.start, 4)
                          << " off_by=" << juce::String(edges[0] - note.start, 4)
                          << std::endl;
            }
            edgeProject.resizeNote(noteId, edgeProject.snapshot()
                .tracks.front().clips.front().notes[probe].startSeconds, noteDuration);

            // And while the note is being resized, before anything is
            // settled: the block follows the drag, so the consonant inside it
            // has to stay the length it was and keep ending on the note start.
            const auto noteStartLocal = edgeProject.snapshot()
                .tracks.front().clips.front().notes[probe].startSeconds;
            auto worstResizing = 0.0;
            auto resizes = 0;
            auto firstResizeOnset = -1.0;
            for (const auto factor : { 0.5, 1.0, 2.0, 4.0 })
            {
                edgeRoll.diagnosticBeginResizeDrag(noteId, noteStartLocal,
                                                   noteDuration * factor);
                const auto edges = edgeRoll.diagnosticRegionEdges(probe);
                if (edges[0] < 0.0) continue;
                const auto& note = edgeRoll.diagnosticNotes()[probe];
                const auto onset = edges[0] - (note.start - settledLeadIn);
                if (firstResizeOnset < 0.0) firstResizeOnset = onset;
                ++resizes;
                worstResizing = std::max(worstResizing,
                                         std::abs(onset - firstResizeOnset));
                worstResizing = std::max(worstResizing,
                                         std::abs(edges[0] - note.start));
                std::cout << "  resizing x" << factor
                          << " onset=" << juce::String(onset, 4)
                          << " ends=" << juce::String(edges[0], 4)
                          << " off_by=" << juce::String(edges[0] - note.start, 4)
                          << std::endl;
            }

            const auto ends = measured >= 5 && worst < 1.0e-6;
            const auto holdsWhileDragging = dragged >= 4 && worstDragging < 1.0e-6;
            const auto holdsWhileResizeDrag = resizes >= 3 && worstResizing < 1.0e-6;
            const auto holdsWhileResizing = lengths >= 3 && worstLength < 1.0e-6
                && worstEndByLength < 1.0e-6;
            std::cout << "note_start=" << juce::String(noteStart, 4)
                      << "|velocities_measured=" << measured
                      << "|worst_off_by=" << juce::String(worst, 5)
                      << "|drags_measured=" << dragged
                      << "|worst_off_by_dragging=" << juce::String(worstDragging, 5)
                      << "|ends_at_the_note=" << (ends ? 1 : 0)
                      << "|holds_while_dragging=" << (holdsWhileDragging ? 1 : 0)
                      << "|lengths_measured=" << lengths
                      << "|worst_onset_change=" << juce::String(worstLength, 5)
                      << "|holds_while_resizing=" << (holdsWhileResizing ? 1 : 0)
                      << "|resize_drags_measured=" << resizes
                      << "|holds_during_resize_drag=" << (holdsWhileResizeDrag ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                ends && holdsWhileDragging && holdsWhileResizing
                    && holdsWhileResizeDrag ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-consonant-reset")
        {
            // The reset brings a stranded consonant back to the front of the
            // note.  That is all it may do: the velocity is a setting, and the
            // two movable lines are somebody's work on the vowel.  It used to
            // throw the split away with the pin, which lost that work.
            I18n resetStrings;
            ProjectModel resetProject;
            juce::String resetError;
            if (!resetProject.load(juce::File(arguments[1].unquoted()), resetError))
            {
                std::cout << "loaded=0|error=" << resetError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            // The four regions have to be on, or there are no boundaries to
            // watch and this would pass without measuring anything.
            resetProject.setTrackUtauMode(
                resetProject.snapshot().tracks.front().id, UtauMode::jie);
            PianoRollComponent resetRoll(resetProject, resetStrings);
            resetRoll.setBounds(0, 0, 1600, 900);
            resetRoll.resized();
            constexpr std::size_t probe = 2;
            const auto noteId = resetProject.snapshot()
                .tracks.front().clips.front().notes[probe].id;
            const auto stateOf = [&]
            {
                const auto note = resetProject.snapshot()
                    .tracks.front().clips.front().notes[probe];
                return std::make_tuple(note.utauPreutteranceOverrideEnabled,
                                       note.utauConsonantVelocity,
                                       note.utauJieSplitSet);
            };
            const auto leadIn = [&]
            {
                resetRoll.diagnosticRefresh();
                const auto& note = resetRoll.diagnosticNotes()[probe];
                return note.start - note.soundingStart;
            };

            const auto voicebankLeadIn = leadIn();
            // What the voicebank gives at the velocity used below.  The reset
            // does not touch the velocity -- that is a setting, chosen on
            // purpose -- so this, not the velocity-100 lead-in, is where the
            // consonant belongs afterwards.
            constexpr auto chosenVelocity = 30;
            const auto trackData = resetProject.snapshot().tracks.front();
            const auto noteLabel = trackData.clips.front().notes[probe].label;
            const auto noteMidi = trackData.clips.front().notes[probe].midiNote;
            const auto atChosen = backend::UtauRenderer::sampleTiming(
                trackData.voicebankDirectory, noteLabel, noteMidi, chosenVelocity,
                utauModeUsesRegions(trackData.utauMode));
            const auto expected = atChosen ? atChosen->preutteranceSeconds : -1.0;
            // Put the consonant somewhere unusable, the way a stray drag does.
            resetProject.setNoteUtauTimingOverrides(noteId, true, 0.40, 0.05);
            resetProject.setNotesUtauConsonantVelocity({ noteId }, chosenVelocity);
            resetProject.setNotesUtauJieSplit({ noteId }, 0.4, 0.6, 0.8);
            const auto [pinnedBefore, velocityBefore, splitBefore] = stateOf();
            const auto strandedLeadIn = leadIn();
            const auto edgesBefore = resetRoll.diagnosticRegionEdges(probe);

            resetRoll.resetConsonant(noteId);
            const auto [pinnedAfter, velocityAfter, splitAfter] = stateOf();
            const auto restoredLeadIn = leadIn();
            const auto edgesAfter = resetRoll.diagnosticRegionEdges(probe);
            const auto noteStart = resetRoll.diagnosticNotes()[probe].start;

            const auto wasStranded = pinnedBefore && splitBefore
                && velocityBefore == chosenVelocity
                && std::abs(strandedLeadIn - voicebankLeadIn) > 0.01;
            const auto pinUndone = !pinnedAfter;
            // The velocity is a setting and survives, and so does the split.
            const auto keptTheVelocity = velocityAfter == chosenVelocity;
            const auto keptTheSplit = splitAfter;
            // The two movable lines have to be where they were, to the
            // millisecond -- the span they are measured against moved under
            // them when the lead-in came back.
            const auto movedSecond = std::abs(edgesAfter[1] - edgesBefore[1]);
            const auto movedThird = std::abs(edgesAfter[2] - edgesBefore[2]);
            const auto linesHeld = edgesBefore[1] > 0.0 && edgesBefore[2] > 0.0
                && movedSecond < 1.0e-6 && movedThird < 1.0e-6;
            // And the consonant now ends on the note's own start.
            const auto atTheNoteFront = std::abs(edgesAfter[0] - noteStart) < 1.0e-6;
            const auto backWhereItBelongs = expected > 0.0
                && std::abs(restoredLeadIn - expected) < 1.0e-9;
            std::cout << "voicebank_lead_in=" << juce::String(voicebankLeadIn, 4)
                      << "|at_chosen_velocity=" << juce::String(expected, 4)
                      << "|stranded_lead_in=" << juce::String(strandedLeadIn, 4)
                      << "|restored_lead_in=" << juce::String(restoredLeadIn, 4)
                      << "|velocity=" << velocityBefore << "->" << velocityAfter
                      << "|vowel_lines_moved_by=" << juce::String(movedSecond, 6)
                      << "," << juce::String(movedThird, 6)
                      << "|was_stranded=" << (wasStranded ? 1 : 0)
                      << "|pin_undone=" << (pinUndone ? 1 : 0)
                      << "|velocity_kept=" << (keptTheVelocity ? 1 : 0)
                      << "|split_kept=" << (keptTheSplit ? 1 : 0)
                      << "|vowel_lines_held=" << (linesHeld ? 1 : 0)
                      << "|consonant_at_the_note_front=" << (atTheNoteFront ? 1 : 0)
                      << "|back_where_it_belongs=" << (backWhereItBelongs ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                wasStranded && pinUndone && keptTheVelocity && keptTheSplit
                    && linesHeld && atTheNoteFront && backWhereItBelongs ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-region-split")
        {
            // The onset is a consonant.  It is pronounced at its own pace, so
            // making the note longer must lengthen what follows it and leave
            // it alone.
            const juce::File bank(arguments[1].unquoted());
            const auto alias = arguments[2].unquoted();
            const auto timing = backend::UtauRenderer::sampleTiming(
                bank, alias, 62.0f, 100, true);
            if (!timing || !timing->hasRegions)
            {
                std::cout << "no_regions" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const std::array<double, 3> byHand { 0.2, 0.5, 0.8 };
            const auto report = [&](double seconds, const std::array<double, 3>* manual,
                                    int velocity = 100)
            {
                const auto at = backend::UtauRenderer::sampleTiming(
                    bank, alias, 62.0f, velocity, true);
                const auto leadIn = at ? at->preutteranceSeconds : 0.0;
                const auto split = backend::UtauRenderer::regionSplit(
                    timing->regionSeconds, seconds, velocity, leadIn, manual);
                std::cout << (manual ? "manual" : "auto") << " " << seconds << "s v"
                          << velocity << " ->";
                for (const auto value : split.seconds)
                    std::cout << " " << juce::String(value, 4);
                std::cout << "   (lead-in " << juce::String(leadIn, 4) << ")"
                          << std::endl;
                return std::pair<double, double> { split.seconds[0], leadIn };
            };
            std::cout << "source =";
            for (const auto value : timing->regionSeconds)
                std::cout << " " << juce::String(value, 4);
            std::cout << std::endl;
            const auto autoShort = report(0.40, nullptr).first;
            const auto autoLong = report(1.60, nullptr).first;
            const auto handShort = report(0.40, &byHand).first;
            const auto handLong = report(1.60, &byHand).first;
            // The end of the onset is the note start, so its length is the
            // lead-in -- at any consonant velocity.  Velocity decides where
            // the consonant begins, never where it finishes.
            const auto slow = report(0.80, nullptr, 50);
            const auto quick = report(0.80, nullptr, 200);
            const auto endsAtTheNote =
                std::abs(slow.first - slow.second) < 1.0e-9
                && std::abs(quick.first - quick.second) < 1.0e-9
                && std::abs(slow.second - quick.second) > 0.005;
            // Moving the boundaries of the other three regions must leave
            // the consonant exactly where it was -- including the very first
            // move, which is what turns a note from planned to hand-placed.
            auto worstByHand = 0.0;
            const auto planned = report(0.80, nullptr).first;
            const std::array<std::array<double, 3>, 4> placements {{
                {{ 0.10, 0.40, 0.70 }}, {{ 0.30, 0.50, 0.60 }},
                {{ 0.05, 0.80, 0.90 }}, {{ 0.20, 0.25, 0.95 }} }};
            for (const auto& placement : placements)
            {
                const auto byHand = report(0.80, &placement).first;
                worstByHand = std::max(worstByHand, std::abs(byHand - planned));
            }
            const auto handsOffTheConsonant = worstByHand < 1.0e-9;

            const auto autoFixed = std::abs(autoShort - autoLong) < 1.0e-9;
            const auto handFixed = std::abs(handShort - handLong) < 1.0e-9;
            // And it is really the lead-in, not merely equal to itself:
            // pinning it to nothing would pass the test above while losing the
            // consonant altogether.
            const auto leadIn = timing->preutteranceSeconds;
            const auto isNatural = std::abs(autoShort - leadIn) < 1.0e-4
                && std::abs(handShort - leadIn) < 1.0e-4;
            // And the rest really does grow, or nothing was being stretched.
            const auto restGrows = std::abs((1.60 - handLong) - (0.40 - handShort))
                > 1.0;
            std::cout << "planned_onset=" << juce::String(planned, 4)
                      << "|worst_by_hand=" << juce::String(worstByHand, 6)
                      << "|hands_off_the_consonant=" << (handsOffTheConsonant ? 1 : 0)
                      << "|auto_onset_fixed=" << (autoFixed ? 1 : 0)
                      << "|manual_onset_fixed=" << (handFixed ? 1 : 0)
                      << "|onset_is_the_lead_in=" << (isNatural ? 1 : 0)
                      << "|ends_at_the_note_at_any_velocity="
                      << (endsAtTheNote ? 1 : 0)
                      << "|rest_absorbs_the_stretch=" << (restGrows ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                handsOffTheConsonant && autoFixed && handFixed && isNatural
                    && endsAtTheNote && restGrows ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-vibrato-presets")
        {
            // The dropdown reads a preset line into the seven fields and
            // writes them back out again, and the saved list has to survive
            // that round trip -- including saving twice under one name, which
            // is asking to overwrite rather than to collect two.
            // Every shipped preset, so a typo in the list is caught here
            // rather than by someone choosing it and getting nothing.
            auto readsSeven = !PianoRollComponent::vibratoBuiltInPresets().isEmpty();
            for (const auto& entry : PianoRollComponent::vibratoBuiltInPresets())
            {
                const auto numbers = PianoRollComponent::vibratoPresetValues(entry);
                readsSeven = readsSeven && numbers.size() == 7;
                for (const auto& number : numbers)
                    readsSeven = readsSeven && number.isNotEmpty()
                        && number.containsOnly("-0123456789.");
            }
            const auto values = PianoRollComponent::vibratoPresetValues(
                PianoRollComponent::vibratoBuiltInPresets()[0]);
            readsSeven = readsSeven && values[0] == "65" && values[1] == "180"
                && values[2] == "35" && values[6] == "0";
            // The one asked for by name, which has to arrive intact.
            const auto mineShipped = PianoRollComponent::vibratoPresetValues(
                PianoRollComponent::vibratoBuiltInPresets()[3]);
            const auto shippedMine = mineShipped.size() == 7
                && mineShipped[0] == "77" && mineShipped[1] == "153"
                && mineShipped[2] == "35" && mineShipped[3] == "20"
                && mineShipped[4] == "7" && mineShipped[5] == "153"
                && mineShipped[6] == "13.7";

            const auto mine = juce::String::fromUTF8("我的颤音");
            const juce::StringArray first { "70", "150", "40", "10", "15", "0", "0" };
            const juce::StringArray second { "80", "140", "45", "10", "15", "0", "0" };
            const auto afterOne = PianoRollComponent::vibratoPresetsWith({}, mine, first);
            const auto afterTwo = PianoRollComponent::vibratoPresetsWith(
                afterOne, mine, second);
            const auto other = juce::String::fromUTF8("另一个");
            const auto afterOther = PianoRollComponent::vibratoPresetsWith(
                afterTwo, other, first);

            const auto count = [](const juce::String& saved)
            {
                auto lines = juce::StringArray::fromLines(saved);
                lines.removeEmptyStrings();
                return lines.size();
            };
            const auto roundTrip = PianoRollComponent::vibratoPresetValues(
                juce::StringArray::fromLines(afterTwo)[0]);
            const auto keptOne = count(afterOne) == 1;
            const auto replaced = count(afterTwo) == 1
                && roundTrip.size() == 7 && roundTrip[0] == "80" && roundTrip[2] == "45";
            const auto added = count(afterOther) == 2;
            // A name with no comma in it is not a preset, and must not be read
            // as one -- that is what an empty box looks like.
            const auto ignoresRubbish =
                PianoRollComponent::vibratoPresetValues("just a name").isEmpty();

            std::cout << "reads_seven=" << (readsSeven ? 1 : 0)
                      << "|kept_one=" << (keptOne ? 1 : 0)
                      << "|same_name_replaces=" << (replaced ? 1 : 0)
                      << "|other_name_adds=" << (added ? 1 : 0)
                      << "|ignores_a_bare_name=" << (ignoresRubbish ? 1 : 0)
                      << "|ships_the_custom_one=" << (shippedMine ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                readsSeven && keptOne && replaced && added && ignoresRubbish
                    && shippedMine ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-follow-rule")
        {
            // Repeating a phrase to tune it: the view must keep the whole
            // phrase in front of you.  Paging on the playhead alone threw the
            // start of it off the left-hand edge, and it had to be dragged
            // back to before the phrase could be heard a second time.
            constexpr auto viewWidth = 1600;
            constexpr auto margin = 58;
            // A phrase ending near the right edge -- the case that pages.
            const std::optional<juce::Range<int>> phrase(juce::Range<int>(944, 1544));
            const auto at = [&](int viewLeft, int playheadX,
                                std::optional<juce::Range<int>> run)
            {
                return MainComponent::followViewPosition(viewLeft, viewWidth,
                                                         playheadX, margin, run);
            };
            const auto wholePhraseVisible = [&](int viewLeft)
            {
                return phrase->getStart() >= viewLeft
                    && phrase->getEnd() <= viewLeft + viewWidth;
            };

            // What the old rule did here, and still does with nothing
            // selected: page on the playhead, wherever that leaves the phrase.
            const auto unbounded = at(0, 1550, std::nullopt);
            const auto oldLosesTheStart = unbounded.has_value()
                && !wholePhraseVisible(*unbounded);

            // What it does now: whatever it decides, the phrase stays whole
            // in front of you.  Here it decides to do nothing at all, the
            // phrase already being in view -- which is the point.
            const auto bounded = at(0, 1550, phrase);
            const auto after = bounded ? *bounded : 0;
            const auto keepsPhrase = wholePhraseVisible(after);

            // And then nothing more, at any point in the phrase, however many
            // times it is played.
            auto settled = keepsPhrase;
            for (auto playhead = phrase->getStart();
                 playhead <= phrase->getEnd() && settled; playhead += 25)
                settled = !at(after, playhead, phrase).has_value();

            // Scrolled away from it, it is fetched back -- once -- and then
            // left alone, so the holding still above is not simply never
            // moving.
            const auto fetched = at(2400, 1550, phrase);
            const auto fetchesItBack = fetched.has_value()
                && wholePhraseVisible(*fetched)
                && !at(*fetched, phrase->getEnd(), phrase).has_value();

            // Longer than the window: paged, or it could not be followed.
            const std::optional<juce::Range<int>> longRun(juce::Range<int>(0, 5000));
            const auto pagesWhenLong = at(0, viewWidth - 20, longRun).has_value();
            // Control: mid-window with nothing selected nothing moves, so the
            // paging above is the edge rather than a rule that always fires.
            const auto quietInTheMiddle = !at(0, viewWidth / 2, std::nullopt).has_value();

            std::cout << "old_rule_gives=" << (unbounded ? *unbounded : -1)
                      << "|new_rule_gives=" << (bounded ? *bounded : -1)
                      << "|old_loses_the_start=" << (oldLosesTheStart ? 1 : 0)
                      << "|new_keeps_the_phrase=" << (keepsPhrase ? 1 : 0)
                      << "|then_holds_still=" << (settled ? 1 : 0)
                      << "|fetches_it_back=" << (fetchesItBack ? 1 : 0)
                      << "|pages_when_longer=" << (pagesWhenLong ? 1 : 0)
                      << "|quiet_in_the_middle=" << (quietInTheMiddle ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                oldLosesTheStart && keepsPhrase && settled && fetchesItBack
                    && pagesWhenLong && quietInTheMiddle ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-play-until")
        {
            // Playing a selection has to finish with the selection: the
            // transport used to carry on through the rest of the piece, and
            // the view went with it.
            I18n untilStrings;
            ProjectModel untilProject;
            juce::String untilError;
            if (!untilProject.load(juce::File(arguments[1].unquoted()), untilError))
            {
                std::cout << "loaded=0|error=" << untilError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            AudioEngine engine;
            engine.syncProject(untilProject.snapshot());
            constexpr auto rate = 48'000.0;
            constexpr auto block = 512;
            engine.prepareToPlay(block, rate);
            juce::AudioBuffer<float> scratch(2, block);
            // Five seconds of blocks against a stop point two seconds in, so
            // a transport that ignores it has room to show that it has rather
            // than merely running out of loop.
            const auto runFrom = [&](double from, double until)
            {
                engine.setPosition(from);
                engine.setPlayUntil(until);
                engine.play();
                for (int index = 0; index < 470 && engine.isPlaying(); ++index)
                {
                    scratch.clear();
                    juce::AudioSourceChannelInfo info(&scratch, 0, block);
                    engine.getNextAudioBlock(info);
                }
                const auto stoppedAt = engine.position();
                engine.stop();
                return stoppedAt;
            };
            const auto blockSeconds = static_cast<double>(block) / rate;
            const auto limited = runFrom(10.0, 12.0);
            const auto free = runFrom(10.0, 0.0);
            // Stopped where it was told, within the block it was told in.
            const auto stoppedOnTime = limited >= 12.0 - 0.01
                && limited <= 12.0 + blockSeconds + 0.01;
            // And without the limit the same run goes well past there, so the
            // first result is the limit working rather than the piece ending
            // or the loop running out.
            const auto ranOnWithout = free > 12.0 + 1.0;
            std::cout << "block_seconds=" << blockSeconds
                      << "|stopped_at=" << limited
                      << "|without_limit_reached=" << free
                      << "|stopped_on_time=" << (stoppedOnTime ? 1 : 0)
                      << "|ran_on_without_limit=" << (ranOnWithout ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(stoppedOnTime && ranOnWithout ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-flags-field")
        {
            // A long global Flags value did not fit the field it lives in, and
            // a value you cannot see all of is one you cannot check.  Clicking
            // into it opens it out; leaving folds it back.
            I18n flagStrings;
            ProjectModel flagProject;
            juce::String flagError;
            if (!flagProject.load(juce::File(arguments[1].unquoted()), flagError))
            {
                std::cout << "loaded=0|error=" << flagError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto flags = arguments[2].unquoted();
            const auto trackId = flagProject.snapshot().tracks.front().id;
            flagProject.setTrackUtauGlobalFlags(trackId, flags);
            TrackListComponent panel(flagProject, flagStrings);
            panel.setBounds(0, 0, 280, 600);
            panel.setSelectedTrack(trackId);
            panel.resized();

            const auto folded = panel.diagnosticFlagsFieldBounds();
            const auto textWidth = panel.diagnosticFlagsTextWidth();
            panel.diagnosticOpenFlagsField(true);
            const auto opened = panel.diagnosticFlagsFieldBounds();
            const auto openTextHeight = panel.diagnosticFlagsTextHeight();
            panel.diagnosticOpenFlagsField(false);
            const auto refolded = panel.diagnosticFlagsFieldBounds();

            // And once through the path a click really takes, which needs the
            // panel on the desktop for focus to be grantable at all.
            panel.addToDesktop(juce::ComponentPeer::windowIsTemporary);
            panel.setVisible(true);
            panel.diagnosticFocusFlagsField();
            const auto focused = panel.diagnosticFlagsFieldHasFocus();
            const auto byFocus = panel.diagnosticFlagsFieldBounds();
            panel.removeFromDesktop();

            // The premise: this value really is too long for the folded field,
            // or opening it out would be proving nothing.
            const auto didNotFit = textWidth > folded.getWidth();
            const auto grew = opened.getWidth() > folded.getWidth();
            // Every line of it on screen at once.
            const auto showsAll = openTextHeight <= opened.getHeight();
            const auto foldsBack = refolded == folded;
            std::cout << "text_width=" << textWidth
                      << "|folded=" << folded.getWidth() << "x" << folded.getHeight()
                      << "|opened=" << opened.getWidth() << "x" << opened.getHeight()
                      << "|open_text_height=" << openTextHeight
                      << "|did_not_fit_folded=" << (didNotFit ? 1 : 0)
                      << "|opened_wider=" << (grew ? 1 : 0)
                      << "|shows_every_line=" << (showsAll ? 1 : 0)
                      << "|folds_back=" << (foldsBack ? 1 : 0)
                      << "|focus_taken=" << (focused ? 1 : 0)
                      << "|by_focus=" << byFocus.getWidth() << "x" << byFocus.getHeight()
                      << "|focus_opens_it="
                      << (focused && byFocus == opened ? 1 : 0) << std::endl;
            setApplicationReturnValue(
                didNotFit && grew && showsAll && foldsBack
                    && focused && byFocus == opened ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-vertical-drag")
        {
            // A note that does not sit on the grid -- because the piece was
            // played in, or the unit was changed afterwards -- must not be
            // pulled onto it just because its pitch was adjusted.
            I18n upStrings;
            ProjectModel upProject;
            juce::String upError;
            if (!upProject.load(juce::File(arguments[1].unquoted()), upError))
            {
                std::cout << "loaded=0|error=" << upError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent upRoll(upProject, upStrings);
            upRoll.setBounds(0, 0, 1600, 900);
            upRoll.resized();
            // The last note: the ones before it are hemmed in by their
            // neighbours, and a nudge that never lands leaves the test with
            // nothing to prove.
            const auto probe = upProject.snapshot()
                .tracks.front().clips.front().notes.size() - 1;
            const auto noteId = upProject.snapshot()
                .tracks.front().clips.front().notes[probe].id;
            const auto startedAt = upProject.snapshot()
                .tracks.front().clips.front().notes[probe].startSeconds;
            // Off the grid by a hair, which is the whole point of the test.
            constexpr auto nudge = 0.013;
            upProject.moveUtauNotes({ noteId }, nudge, 0);
            upRoll.diagnosticRefresh();
            const auto noteNow = [&]
            {
                const auto& note = upProject.snapshot()
                    .tracks.front().clips.front().notes[probe];
                return std::pair<double, float> { note.startSeconds, note.midiNote };
            };
            const auto before = noteNow();
            const auto reallyOffGrid = std::abs(before.first - startedAt - nudge) < 1.0e-9;
            const auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto block = upRoll.diagnosticHitBounds(probe);
            const auto at = block.getCentre();
            const auto press = [&](juce::Point<float> where)
            {
                return juce::MouseEvent(source, where,
                    juce::ModifierKeys::leftButtonModifier,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &upRoll, &upRoll, juce::Time::getCurrentTime(), at,
                    juce::Time::getCurrentTime(), 1, true);
            };
            // Straight down, three rows or so.
            upRoll.mouseDown(press(at));
            upRoll.mouseDrag(press(at + juce::Point<float>(0.0f, 60.0f)));
            const auto verticalDelta = upRoll.diagnosticPreviewMoveDelta();
            upRoll.mouseUp(press(at + juce::Point<float>(0.0f, 60.0f)));
            const auto after = noteNow();

            // Sideways as well, where snapping is still wanted: checked on the
            // preview, since whether the move can be committed depends on the
            // neighbours rather than on the rule under test.
            const auto sideways = upRoll.diagnosticHitBounds(probe).getCentre();
            const auto press2 = [&](juce::Point<float> where)
            {
                return juce::MouseEvent(source, where,
                    juce::ModifierKeys::leftButtonModifier,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &upRoll, &upRoll, juce::Time::getCurrentTime(), sideways,
                    juce::Time::getCurrentTime(), 1, true);
            };
            upRoll.mouseDown(press2(sideways));
            upRoll.mouseDrag(press2(sideways + juce::Point<float>(60.0f, 0.0f)));
            const auto sidewaysDelta = upRoll.diagnosticPreviewMoveDelta();
            upRoll.mouseUp(press2(sideways + juce::Point<float>(60.0f, 0.0f)));

            const auto stayedInTime = std::abs(after.first - before.first) < 1.0e-9
                && std::abs(verticalDelta) < 1.0e-12;
            const auto pitchChanged = after.second != before.second;
            const auto sidewaysStillSnaps = std::abs(sidewaysDelta) > 1.0e-6;
            std::cout << "start=" << before.first << "->" << after.first
                      << "|midi=" << before.second << "->" << after.second
                      << "|vertical_delta=" << verticalDelta
                      << "|sideways_delta=" << sidewaysDelta
                      << "|stayed_in_time=" << (stayedInTime ? 1 : 0)
                      << "|pitch_changed=" << (pitchChanged ? 1 : 0)
                      << "|sideways_still_snaps=" << (sidewaysStillSnaps ? 1 : 0)
                      << "|really_off_grid=" << (reallyOffGrid ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                reallyOffGrid && stayedInTime && pitchChanged
                    && sidewaysStillSnaps ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-scroll-sync")
        {
            // The ruler and the roll are scrolled together, and while playing
            // both are pointed at the playhead.  That only works if a place in
            // the piece means the same thing to both: they measure from
            // different origins -- the roll starts 58 pixels in, behind the
            // keyboard -- and one slider drives both scales, so the scales
            // have to move together too.
            I18n syncStrings;
            ProjectModel syncProject;
            juce::String syncError;
            if (!syncProject.load(juce::File(arguments[1].unquoted()), syncError))
            {
                std::cout << "loaded=0|error=" << syncError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent syncRoll(syncProject, syncStrings);
            TimelineComponent syncRuler(syncProject);
            syncRoll.setBounds(0, 0, 1600, 900);
            syncRuler.setBounds(0, 0, 1600, 200);
            auto scalesAgree = true;
            auto roundTrips = true;
            auto mirrorHolds = true;
            auto rawCopyWouldBeWrong = false;
            juce::String detail;
            for (const auto zoom : { 140.0f, 600.0f, 2000.0f, 8000.0f })
            {
                syncRoll.setPixelsPerSecond(zoom);
                syncRuler.setPixelsPerSecond(zoom);
                constexpr auto when = 10.0;
                // A second of piece has to be the same number of pixels wide
                // in both, or nothing can translate between them.  The ruler
                // used to stop at 600 while the roll went on to 8000.
                const auto rollSpan = syncRoll.pixelForSeconds(when + 1.0)
                    - syncRoll.pixelForSeconds(when);
                const auto rulerSpan = syncRuler.pixelForSeconds(when + 1.0)
                    - syncRuler.pixelForSeconds(when);
                if (rollSpan != rulerSpan)
                {
                    scalesAgree = false;
                    detail += "zoom " + juce::String(zoom) + ": "
                        + juce::String(rollSpan) + " vs " + juce::String(rulerSpan) + "; ";
                }
                // And the same instant sits at a different pixel in each, so
                // handing one's scroll position to the other unconverted --
                // which is what used to happen -- cannot be right.
                if (syncRoll.pixelForSeconds(when) != syncRuler.pixelForSeconds(when))
                    rawCopyWouldBeWrong = true;
                if (std::abs(syncRoll.secondsForPixel(
                        syncRoll.pixelForSeconds(when)) - when) > 0.001
                    || std::abs(syncRuler.secondsForPixel(
                        syncRuler.pixelForSeconds(when)) - when) > 0.001)
                    roundTrips = false;
                // Putting the ruler on the roll's instant and reading it back
                // has to land where it started.  Drifting a pixel each time
                // would have the two creeping apart while playing, with the
                // follow forever correcting them.
                for (const auto pixel : { 58, 500, 5000, 50000 })
                {
                    const auto mirrored = syncRuler.pixelForSeconds(
                        syncRoll.secondsForPixel(pixel));
                    const auto back = syncRoll.pixelForSeconds(
                        syncRuler.secondsForPixel(mirrored));
                    if (std::abs(back - pixel) > 1) mirrorHolds = false;
                }
            }
            std::cout << "scales_agree=" << (scalesAgree ? 1 : 0)
                      << "|round_trips=" << (roundTrips ? 1 : 0)
                      << "|mirror_holds=" << (mirrorHolds ? 1 : 0)
                      << "|raw_pixel_copy_would_be_wrong="
                      << (rawCopyWouldBeWrong ? 1 : 0)
                      << (detail.isEmpty() ? juce::String() : "|detail=" + detail)
                      << std::endl;
            setApplicationReturnValue(
                scalesAgree && roundTrips && mirrorHolds
                    && rawCopyWouldBeWrong ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-edit-follow")
        {
            // The playhead follows where an edit begins, so that it is always
            // worth looking at and a zoom has something real to hold still.
            // A right click is asking a question, not editing, and must leave
            // it where it was.
            I18n followStrings;
            ProjectModel followProject;
            juce::String followError;
            if (!followProject.load(juce::File(arguments[1].unquoted()), followError))
            {
                std::cout << "loaded=0|error=" << followError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent followRoll(followProject, followStrings);
            followRoll.setBounds(0, 0, 1600, 900);
            followRoll.resized();
            followRoll.diagnosticRefresh();
            auto reported = -1.0;
            auto calls = 0;
            followRoll.onEditPosition = [&](double seconds)
            {
                reported = seconds;
                ++calls;
            };
            const auto block = followRoll.diagnosticHitBounds(4);
            const auto at = block.getCentre();
            const auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto press = [&](juce::ModifierKeys mods)
            {
                return juce::MouseEvent(source, at, mods,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &followRoll, &followRoll, juce::Time::getCurrentTime(), at,
                    juce::Time::getCurrentTime(), 1, false);
            };
            followRoll.mouseDown(press(juce::ModifierKeys::leftButtonModifier));
            const auto afterLeft = reported;
            const auto leftCalls = calls;
            followRoll.mouseUp(press(juce::ModifierKeys::leftButtonModifier));
            reported = -1.0;
            followRoll.mouseDown(press(juce::ModifierKeys::rightButtonModifier));
            const auto rightCalls = calls - leftCalls;
            followRoll.mouseUp(press(juce::ModifierKeys::rightButtonModifier));

            // Drawing a box round some notes chooses them, not a place.  The
            // playhead is put back where it stood, or the next paste lands at
            // the corner the box was started from.
            constexpr auto settled = 5.5;
            followRoll.setPlayheadSeconds(settled);
            const auto empty = juce::Point<float>(
                followRoll.diagnosticHitBounds(4).getX() - 40.0f, 860.0f);
            const auto sweep = [&](juce::Point<float> where, bool dragged)
            {
                return juce::MouseEvent(source, where,
                    juce::ModifierKeys::leftButtonModifier,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &followRoll, &followRoll, juce::Time::getCurrentTime(), empty,
                    juce::Time::getCurrentTime(), 1, dragged);
            };
            reported = -1.0;
            followRoll.mouseDown(sweep(empty, false));
            const auto duringSweep = reported;
            followRoll.mouseDrag(sweep(empty + juce::Point<float>(320.0f, -120.0f), true));
            followRoll.mouseUp(sweep(empty + juce::Point<float>(320.0f, -120.0f), true));
            const auto afterSweep = reported;
            // A press that never became a box is still someone putting the
            // playhead down, and keeps where it was put.
            reported = -1.0;
            followRoll.mouseDown(sweep(empty, false));
            followRoll.mouseUp(sweep(empty, false));
            const auto afterTap = reported;

            const auto sweepMoved = std::abs(duringSweep - settled) > 0.01;
            const auto sweepGaveItBack = std::abs(afterSweep - settled) < 1.0e-9;
            const auto tapKeptIt = std::abs(afterTap - duringSweep) < 1.0e-9;
            // Where the roll itself says that pixel is, so the check does not
            // quietly repeat the conversion under test.
            const auto expected = followRoll.secondsForPixel(
                static_cast<int>(std::lround(at.x)));
            const auto followed = leftCalls == 1
                && std::abs(afterLeft - expected) < 0.002;
            std::cout << "pressed_at_px=" << at.x
                      << "|expected_seconds=" << expected
                      << "|reported_seconds=" << afterLeft
                      << "|left_calls=" << leftCalls
                      << "|right_calls=" << rightCalls
                      << "|sweep_moved_it=" << (sweepMoved ? 1 : 0)
                      << "|sweep_gave_it_back=" << (sweepGaveItBack ? 1 : 0)
                      << "|tap_kept_it=" << (tapKeptIt ? 1 : 0)
                      << "|followed_the_edit=" << (followed ? 1 : 0)
                      << "|right_click_left_it=" << (rightCalls == 0 ? 1 : 0)
                      << std::endl;
            setApplicationReturnValue(
                followed && rightCalls == 0 && sweepMoved && sweepGaveItBack
                    && tapKeptIt ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-note-drag-threshold")
        {
            // Placing a lyric means double clicking a note, and a press that
            // shifts by a pixel or two on the way must leave it alone.  A real
            // drag still has to move it, or the guard would just be a way of
            // breaking the editor.
            I18n gateStrings;
            ProjectModel gateProject;
            juce::String gateError;
            if (!gateProject.load(juce::File(arguments[1].unquoted()), gateError))
            {
                std::cout << "loaded=0|error=" << gateError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent gateRoll(gateProject, gateStrings);
            gateRoll.setBounds(0, 0, 1600, 900);
            gateRoll.resized();
            gateRoll.diagnosticRefresh();
            // Pressed one pixel inside the bottom edge of the block, which is
            // where the failure actually lives: rounding the cursor to a row
            // means a couple of pixels of hand shake lands on the next pitch.
            // The notes here abut, so sideways is not a fair test -- the model
            // refuses a move that would collide -- and the gate is the same
            // code either way.
            const auto block = gateRoll.diagnosticHitBounds(2);
            const auto centre = juce::Point<float>(block.getCentreX(),
                                                   block.getBottom() - 1.0f);
            const auto startOf = [&gateProject]
            {
                const auto snapshot = gateProject.snapshot();
                return snapshot.tracks.front().clips.front().notes[2].startSeconds;
            };
            const auto midiOf = [&gateProject]
            {
                const auto snapshot = gateProject.snapshot();
                return snapshot.tracks.front().clips.front().notes[2].midiNote;
            };
            const auto source = juce::Desktop::getInstance().getMainMouseSource();
            const auto press = [&](juce::Point<float> at, juce::Point<float> from,
                                   int clicks, bool dragged)
            {
                return juce::MouseEvent(source, at, juce::ModifierKeys::leftButtonModifier,
                    juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                    &gateRoll, &gateRoll, juce::Time::getCurrentTime(), from,
                    juce::Time::getCurrentTime(), clicks, dragged);
            };
            const auto gesture = [&](juce::Point<float> travel)
            {
                gateRoll.mouseDown(press(centre, centre, 1, false));
                gateRoll.mouseDrag(press(centre + travel, centre, 1, true));
                gateRoll.mouseUp(press(centre + travel, centre, 1, true));
            };
            const auto beforeStart = startOf();
            const auto beforeMidi = midiOf();
            gesture({ 2.0f, 3.0f });
            const auto afterWobbleStart = startOf();
            const auto afterWobbleMidi = midiOf();
            gesture({ 0.0f, 60.0f });
            const auto afterDragMidi = midiOf();
            const auto held = std::abs(afterWobbleStart - beforeStart) < 1.0e-9
                && afterWobbleMidi == beforeMidi;
            const auto moved = afterDragMidi != beforeMidi;
            std::cout << "start=" << beforeStart
                      << "|after_wobble_start=" << afterWobbleStart
                      << "|midi=" << beforeMidi
                      << "|after_wobble_midi=" << afterWobbleMidi
                      << "|after_drag_midi=" << afterDragMidi
                      << "|wobble_ignored=" << (held ? 1 : 0)
                      << "|real_drag_moved=" << (moved ? 1 : 0) << std::endl;
            setApplicationReturnValue(held && moved ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-roll-drag")
        {
            // Two things a partial repaint can get wrong: leaving the old
            // drawing behind, and hit regions going stale now that they are no
            // longer rebuilt by every paint.  Both are checked here.
            I18n dragStrings;
            ProjectModel dragProject;
            juce::String dragError;
            if (!dragProject.load(juce::File(arguments[1].unquoted()), dragError))
            {
                std::cout << "loaded=0|error=" << dragError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent dragRoll(dragProject, dragStrings);
            dragRoll.setBounds(0, 0, 1600, 900);
            dragRoll.resized();
            dragRoll.setShowEnvelope(true);
            dragRoll.diagnosticRefresh();
            // Never painted at this point: if the hits were still built by the
            // paint these would be empty, and every click would miss.
            const auto notes = dragRoll.diagnosticNotes();
            const auto hitsBeforePaint = dragRoll.diagnosticHitCount();
            // The first note starts at zero, where every zoom puts it at the
            // same pixel; ask about one that has somewhere to move.
            const auto xAtDefaultZoom = dragRoll.diagnosticHitX(3);
            dragRoll.setPixelsPerSecond(600.0f);
            const auto xAtCloseZoom = dragRoll.diagnosticHitX(3);
            dragRoll.setPixelsPerSecond(140.0f);

            const auto paintClipped = [&dragRoll](juce::Image& target,
                                                  juce::Rectangle<int> area)
            {
                juce::Graphics g(target);
                g.reduceClipRegion(area);
                dragRoll.paintEntireComponent(g, true);
            };
            juce::Image partial(juce::Image::ARGB, 1600, 900, true);
            juce::Image whole(juce::Image::ARGB, 1600, 900, true);
            const auto full = juce::Rectangle<int>(0, 0, 1600, 900);
            paintClipped(partial, full);
            paintClipped(whole, full);

            // Take hold of a note near its left edge and drag it to the right,
            // which is the case where the note ends up furthest from the
            // cursor and so the band has to reach furthest.
            const auto target = notes.size() > 2 ? notes[2].id : juce::String();
            const auto delta = 0.9;
            dragRoll.diagnosticBeginMoveDrag(target, delta);
            const auto cursor = 58.0f + static_cast<float>(
                (notes.size() > 2 ? notes[2].start : 0.0) + delta) * 140.0f;
            const auto area = dragRoll.diagnosticDragRepaintArea(cursor);
            { juce::Graphics g(whole); dragRoll.paintEntireComponent(g, true); }
            // Narrowing the clip nudges the rasteriser on its own, so the band
            // is measured against wider and wider versions of itself.  Noise
            // stays flat as the band grows; a band that was too small does not.
            auto worstApart = 0;
            const auto staleAgainst = [&](juce::Rectangle<int> clip, double baseDelta,
                                          int& minX, int& maxX)
            {
                juce::Image probe(juce::Image::ARGB, 1600, 900, true);
                { juce::Graphics g(probe); g.reduceClipRegion(full);
                  dragRoll.diagnosticBeginMoveDrag(target, baseDelta);
                  dragRoll.paintEntireComponent(g, true);
                  dragRoll.diagnosticBeginMoveDrag(target, delta); }
                paintClipped(probe, clip);
                auto count = 0;
                minX = 1600; maxX = -1;
                for (int y = 0; y < 900; ++y)
                    for (int x = 0; x < 1600; ++x)
                    {
                        const auto left = probe.getPixelAt(x, y);
                        const auto right = whole.getPixelAt(x, y);
                        // The pitch line is one path down the length of the
                        // piece, so moving a note changes the path and the
                        // painter can lay it down a step of antialiasing
                        // differently at vertices far from the note -- three
                        // pixels of it here, 9 of 255 in one channel, on a
                        // hundred-note project.  Clipping alone was measured
                        // to change nothing at all, so the floor is not the
                        // clip; it is the path.  A band that really missed
                        // something leaves a piece of a note behind, which is
                        // nothing like this: it differs by whole colours.
                        const auto apart = std::max({
                            std::abs(left.getRed() - right.getRed()),
                            std::abs(left.getGreen() - right.getGreen()),
                            std::abs(left.getBlue() - right.getBlue()),
                            std::abs(left.getAlpha() - right.getAlpha()) });
                        worstApart = std::max(worstApart, static_cast<int>(apart));
                        if (apart > 24)
                        {
                            ++count;
                            minX = std::min(minX, x); maxX = std::max(maxX, x);
                        }
                    }
                return count;
            };
            auto minX = 0, maxX = 0;
            const auto trails = staleAgainst(area, 0.0, minX, maxX);
            auto wideMinX = 0, wideMaxX = 0;
            const auto trailsWider = staleAgainst(area.expanded(256, 0), 0.0,
                                                  wideMinX, wideMaxX);
            const auto timeClipped = [&](juce::Rectangle<int> clip)
            {
                juce::Image probe(juce::Image::ARGB, 1600, 900, true);
                const auto started = juce::Time::getHighResolutionTicks();
                for (int index = 0; index < 5; ++index) paintClipped(probe, clip);
                return juce::Time::highResolutionTicksToSeconds(
                    juce::Time::getHighResolutionTicks() - started) * 1000.0 / 5.0;
            };
            std::cout << "notes=" << notes.size()
                      << "|paint_full_ms=" << timeClipped(full)
                      << "|paint_band_ms=" << timeClipped(area)
                      << "|hits_before_any_paint=" << hitsBeforePaint
                      << "|hit_x_140=" << xAtDefaultZoom
                      << "|hit_x_600=" << xAtCloseZoom
                      << "|band=" << area.getX() << ".." << area.getRight()
                      << "|trail_pixels=" << trails
                      << "|worst_channel_apart=" << worstApart
                      << "|trail_box=" << minX << ".." << maxX
                      << "|trail_pixels_band_plus_256=" << trailsWider
                      << std::endl;
            // The band is right when repainting only it leaves nothing behind
            // that a full repaint would have changed -- allowing for the clip
            // noise measured above, which is there whatever the band covers.
            setApplicationReturnValue(
                hitsBeforePaint == notes.size() && trails == 0 ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-playhead-band")
        {
            // The playhead is a one pixel line, and repainting the whole roll
            // for it thirty times a second is a full window repaint per tick.
            // It repaints its own strip instead -- which has to cover where the
            // line was as well as where it is going, or the old one stays.
            I18n bandStrings;
            ProjectModel bandProject;
            juce::String bandError;
            if (!bandProject.load(juce::File(arguments[1].unquoted()), bandError))
            {
                std::cout << "loaded=0|error=" << bandError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent bandRoll(bandProject, bandStrings);
            bandRoll.setBounds(0, 0, 1600, 900);
            bandRoll.resized();
            bandRoll.diagnosticRefresh();
            const auto full = juce::Rectangle<int>(0, 0, 1600, 900);
            const auto paintInto = [&bandRoll](juce::Image& target,
                                               juce::Rectangle<int> clip)
            {
                juce::Graphics g(target);
                g.reduceClipRegion(clip);
                bandRoll.paintEntireComponent(g, true);
            };
            // Both instants are on screen, so no edge mark is drawn in either
            // and the strip is the whole of what changes.
            bandRoll.setPlayheadSeconds(2.0);
            juce::Image partial(juce::Image::ARGB, 1600, 900, true);
            paintInto(partial, full);
            bandRoll.setPlayheadSeconds(3.0);
            const auto band = bandRoll.diagnosticPlayheadBand();
            paintInto(partial, band);
            juce::Image whole(juce::Image::ARGB, 1600, 900, true);
            paintInto(whole, full);
            auto stale = 0;
            for (int y = 0; y < 900; ++y)
                for (int x = 0; x < 1600; ++x)
                    if (partial.getPixelAt(x, y) != whole.getPixelAt(x, y)) ++stale;
            // A strip, not the window: repainting everything would pass the
            // check above while being the thing that was wrong.
            const auto narrow = band.getWidth() < 400;
            std::cout << "band=" << band.getX() << ".." << band.getRight()
                      << "|stale_pixels=" << stale
                      << "|covers_the_move=" << (stale == 0 ? 1 : 0)
                      << "|stays_a_strip=" << (narrow ? 1 : 0) << std::endl;
            setApplicationReturnValue(stale == 0 && narrow ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-roll-cull")
        {
            // Skipping a note that is off screen must change nothing that is
            // on screen.  The same strip is painted twice: once with the clip
            // set to the window, which culls, and once with a clip wide enough
            // that nothing in the strip is culled at all.  The two have to
            // agree pixel for pixel.
            I18n cullStrings;
            ProjectModel cullProject;
            juce::String cullError;
            if (!cullProject.load(juce::File(arguments[1].unquoted()), cullError))
            {
                std::cout << "loaded=0|error=" << cullError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent cullRoll(cullProject, cullStrings);
            cullRoll.setBounds(0, 0, 1600, 900);
            cullRoll.resized();
            cullRoll.setShowEnvelope(true);
            // The canvas only grows to the length of the piece once the layout
            // has been built; without this both paints share one clip and the
            // comparison proves nothing.
            cullRoll.diagnosticRefresh();
            const auto wide = std::min(cullRoll.getWidth(), 6000);
            const auto paintInto = [&cullRoll](int width)
            {
                juce::Image image(juce::Image::ARGB, width, 900, true);
                juce::Graphics g(image);
                cullRoll.paintEntireComponent(g, true);
                return image;
            };
            const auto compare = [](const juce::Image& left, const juce::Image& right)
            {
                auto differing = 0;
                for (int y = 0; y < 900; ++y)
                    for (int x = 0; x < 1600; ++x)
                        if (left.getPixelAt(x, y) != right.getPixelAt(x, y)) ++differing;
                return differing;
            };
            const auto culled = paintInto(1600);
            // The clip width itself nudges the rasteriser, so comparing a
            // narrow paint with a wide one cannot separate a skipped note from
            // an antialiased edge.  Same width, culling switched off, is the
            // only pair where the skip is the sole variable.
            const auto timePaint = [&paintInto]
            {
                const auto started = juce::Time::getHighResolutionTicks();
                for (int index = 0; index < 3; ++index) paintInto(1600);
                return juce::Time::highResolutionTicksToSeconds(
                    juce::Time::getHighResolutionTicks() - started) * 1000.0 / 3.0;
            };
            const auto withCull = timePaint();
            cullRoll.diagnosticSetCulling(false);
            const auto uncelled = paintInto(1600);
            const auto withoutCull = timePaint();
            cullRoll.diagnosticSetCulling(true);
            const auto differing = compare(culled, uncelled);
            // A zero above proves nothing unless the switch does something, so
            // the cost with and without it is reported beside the difference.
            std::cout << "notes=" << cullRoll.diagnosticNotes().size()
                      << "|canvas=" << cullRoll.getWidth()
                      << "|same_width_control=" << compare(culled, paintInto(1600))
                      << "|culled_vs_uncelled=" << differing
                      << "|paint_culled_ms=" << withCull
                      << "|paint_uncelled_ms=" << withoutCull
                      << std::endl;
            (void) wide;
            setApplicationReturnValue(differing == 0 ? 0 : 4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-roll-timing")
        {
            // What the editor costs per edit and per frame, off screen.  The
            // paint is clipped to a window-sized image so off-screen notes
            // count for as little here as they do on screen.
            I18n timingStrings;
            ProjectModel timingProject;
            juce::String timingError;
            if (!timingProject.load(juce::File(arguments[1].unquoted()), timingError))
            {
                std::cout << "loaded=0|error=" << timingError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto repeats = arguments.size() >= 3
                ? std::max(1, arguments[2].getIntValue()) : 5;
            PianoRollComponent timingRoll(timingProject, timingStrings);
            timingRoll.setBounds(0, 0, 1600, 900);
            timingRoll.resized();
            if (arguments.size() >= 4 && arguments[3].getIntValue() != 0)
                timingRoll.setShowEnvelope(true);
            const auto noteCount = timingRoll.diagnosticNotes().size();
            const auto elapsed = [](juce::int64 from)
            {
                return juce::Time::highResolutionTicksToSeconds(
                    juce::Time::getHighResolutionTicks() - from) * 1000.0;
            };
            auto started = juce::Time::getHighResolutionTicks();
            for (int index = 0; index < repeats; ++index) timingRoll.diagnosticRefresh();
            const auto layoutMs = elapsed(started) / repeats;
            juce::Image canvas(juce::Image::ARGB, 1600, 900, true);
            started = juce::Time::getHighResolutionTicks();
            for (int index = 0; index < repeats; ++index)
            {
                juce::Graphics g(canvas);
                timingRoll.paintEntireComponent(g, true);
            }
            const auto paintMs = elapsed(started) / repeats;
            std::cout << "notes=" << noteCount
                      << "|canvas=" << timingRoll.getWidth() << "x" << timingRoll.getHeight()
                      << "|layout_ms=" << layoutMs
                      << "|paint_ms=" << paintMs << std::endl;
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-envelope-presets")
        {
            // These buttons say what they do by drawing it, and the drawing is
            // the one thing a text report cannot check.  This lays them out at
            // the size the parameter bar hands them and writes a PNG.
            struct Preset { const char* text; double attack; double release; float plateauEnd; };
            const Preset presets[] = {
                { "\xe6\xa0\x87\xe5\x87\x86", 0.005, 0.035, 0.0f },
                { "\xe6\xb8\x90\xe5\xbc\xb1", 0.005, 0.035, -0.92f },
                { "\xe6\x9f\x94\xe8\xb5\xb7", 0.015, 0.035, 0.0f },
                { "\xe7\x9f\xad\xe6\x94\xb6", 0.005, 0.005, 0.0f }
            };
            struct Strip final : public juce::Component
            {
                void paint(juce::Graphics& g) override { g.fillAll(Palette::panel); }
            };
            HachiLookAndFeel presetLookAndFeel;
            juce::LookAndFeel::setDefaultLookAndFeel(&presetLookAndFeel);
            Strip strip;
            strip.setBounds(0, 0, 4 * 46 + 3 * 3, 30);
            std::array<EnvelopePresetButton, 4> buttons;
            for (int index = 0; index < 4; ++index)
            {
                auto& button = buttons[static_cast<std::size_t>(index)];
                button.configure(juce::String::fromUTF8(presets[index].text),
                                 presets[index].attack, presets[index].release,
                                 presets[index].plateauEnd);
                button.setBounds(index * 49, 0, 46, 30);
                strip.addAndMakeVisible(button);
            }
            const auto shot = strip.createComponentSnapshot(strip.getLocalBounds(), true, 3.0f);
            juce::File out(arguments[1].unquoted());
            out.deleteFile();
            juce::PNGImageFormat png;
            std::unique_ptr<juce::FileOutputStream> stream(out.createOutputStream());
            const auto written = stream != nullptr && png.writeImageToStream(shot, *stream);
            stream.reset();
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
            std::cout << "written=" << (written ? 1 : 0)
                      << "|size=" << shot.getWidth() << "x" << shot.getHeight() << std::endl;
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-piano-roll")
        {
            // Builds the roll off screen for a saved project and reports what
            // each note ended up with.  Reaching it through a window needs a
            // mouse, which left this geometry unverifiable.
            I18n rollStrings;
            ProjectModel rollProject;
            juce::String rollError;
            if (!rollProject.load(juce::File(arguments[1].unquoted()), rollError))
            {
                std::cout << "loaded=0\nerror=" << rollError << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            PianoRollComponent roll(rollProject, rollStrings);
            roll.setBounds(0, 0, 1600, 900);
            roll.resized();
            // Optional: apply an envelope preset to every note first, so the
            // shape the toolbar buttons write can be measured.
            if (arguments.size() >= 5)
            {
                roll.selectAllNotes();
                const auto applied = roll.applyEnvelopePreset(arguments[2].getDoubleValue() / 1000.0,
                                         arguments[3].getDoubleValue() / 1000.0,
                                         static_cast<float>(arguments[4].getDoubleValue()));
                // The model announces changes asynchronously and nothing is
                // pumping a message loop here, so bring the roll up to date.
                roll.diagnosticRefresh();
                std::cout << "preset_applied=" << applied << '\n';
            }
            std::cout << "loaded=1\n";
            for (const auto& note : roll.diagnosticNotes())
            {
                std::cout << "note=" << note.label
                          << " utau=" << (note.utau ? 1 : 0)
                          << " nominal=" << note.start << ".." << note.end
                          << " sounding=" << note.soundingStart << ".."
                          << note.soundingEnd
                          << " envelope=";
                for (const auto& point : note.envelope)
                    std::cout << "(" << point.timeSeconds << "," << point.gainDb << ")";
                std::cout << '\n';
            }
            std::cout << std::flush;
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--render-project")
        {
            // Headless render of a UTAU project through the same AudioEngine +
            // RenderService the GUI uses, so a consistency check renders exactly
            // what the app would.  Args: project.hjpx  out.wav  [voicebankDir]
            //   [resamplerExe]  [hifiganModelDir]
            ProjectModel project;
            juce::String loadError;
            if (!project.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cout << "loaded=0|error=" << loadError << std::endl;
                setApplicationReturnValue(2);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            auto data = project.snapshot();
            if (arguments.size() >= 4 && arguments[3].isNotEmpty())
            {
                const juce::File vb(arguments[3].unquoted());
                for (auto& track : data.tracks)
                    if (track.pitchAlgorithm == PitchAlgorithm::utau)
                        track.voicebankDirectory = vb;
            }
            // Optional 7th arg: solo one track by 0-based index (mute the rest),
            // so a single-track reference bounce can be matched exactly.
            if (arguments.size() >= 7 && arguments[6].isNotEmpty())
            {
                const auto soloIndex = arguments[6].getIntValue();
                for (std::size_t t = 0; t < data.tracks.size(); ++t)
                    data.tracks[t].muted = (static_cast<int>(t) != soloIndex);
            }
            // Optional 8th arg: render order override -- "splice" =
            // stretchSpliceThenPitch (先拼接后合成), "process" = processThenSplice
            // (先合成后拼接).  Lets a consistency check exercise either NSF order.
            if (arguments.size() >= 8 && arguments[7].isNotEmpty())
            {
                const auto order = arguments[7].toLowerCase();
                const auto ro = order.startsWith("splice")
                    ? RenderOrder::stretchSpliceThenPitch
                    : RenderOrder::processThenSplice;
                for (auto& track : data.tracks) track.renderOrder = ro;
            }
            cliAudioEngine = std::make_unique<AudioEngine>();
            if (arguments.size() >= 5 && arguments[4].isNotEmpty())
                cliAudioEngine->setUtauResamplerFile(juce::File(arguments[4].unquoted()));
            if (arguments.size() >= 6 && arguments[5].isNotEmpty())
                cliAudioEngine->setHifiganModelDirectory(juce::File(arguments[5].unquoted()));
            // UTAU rendering is selection-driven; a headless full render selects
            // every note so the whole song is synthesised, not just a marquee.
            cliAudioEngine->selectEveryUtauNote(data);
            cliAudioEngine->syncProject(data);
            const auto deadline = juce::Time::getMillisecondCounter() + 1'800'000;
            for (;;)
            {
                const auto progress = cliAudioEngine->renderProgress();
                if (!progress.has_value()) break;
                if (juce::Time::getMillisecondCounter() > deadline)
                {
                    std::cout << "rendered=0|error=timeout|progress=" << *progress << std::endl;
                    setApplicationReturnValue(4);
                    juce::MessageManager::callAsync([this] { quit(); });
                    return;
                }
                juce::Thread::sleep(200);
            }
            juce::String exportError;
            const auto ok = cliAudioEngine->exportWav(juce::File(arguments[2].unquoted()),
                exportError);
            std::cout << "rendered=" << (ok ? 1 : 0)
                      << "|output=" << arguments[2]
                      << "|backend=" << cliAudioEngine->activeRenderBackends()
                      << "|warning=" << cliAudioEngine->activeRenderWarnings()
                      << (ok ? juce::String() : "|error=" + exportError) << std::endl;
            setApplicationReturnValue(ok ? 0 : 3);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--inspect-project")
        {
            ProjectModel project;
            juce::String error;
            if (!project.load(juce::File(arguments[1].unquoted()), error))
            {
                std::cout << "loaded=0|error=" << error << std::endl;
                setApplicationReturnValue(2);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto data = project.snapshot();
            std::cout << "loaded=1|bpm=" << data.bpm
                      << "|duration=" << data.durationSeconds()
                      << "|tracks=" << data.tracks.size() << std::endl;
            for (const auto& track : data.tracks)
            {
                std::size_t notes = 0;
                for (const auto& clip : track.clips) notes += clip.notes.size();
                const auto algo = track.pitchAlgorithm == PitchAlgorithm::utau ? "utau"
                    : track.pitchAlgorithm == PitchAlgorithm::nsfHifigan ? "nsf-hifigan"
                    : track.pitchAlgorithm == PitchAlgorithm::world ? "world"
                    : track.pitchAlgorithm == PitchAlgorithm::llsm2 ? "llsm2"
                    : track.pitchAlgorithm == PitchAlgorithm::mld5 ? "mld5"
                    : track.pitchAlgorithm == PitchAlgorithm::mld3 ? "mld3" : "other";
                std::cout << "track|name=" << track.name
                          << "|pitchAlgo=" << algo
                          << "|voicebank=" << track.voicebankDirectory.getFullPathName()
                          << "|vbExists=" << (track.voicebankDirectory.isDirectory() ? 1 : 0)
                          << "|clips=" << track.clips.size()
                          << "|notes=" << notes
                          << "|flags=" << track.utauGlobalFlags << std::endl;
            }
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--inspect-settings")
        {
            I18n diagnosticStrings;
            juce::AudioDeviceManager diagnosticDevices;
            juce::PropertiesFile::Options options;
            options.applicationName = "HachiShifterSettingsSmoke";
            options.filenameSuffix = "settings";
            options.folderName = "HachiShifterNextSmoke";
            options.storageFormat = juce::PropertiesFile::storeAsXML;
            juce::PropertiesFile diagnosticProperties(options);
            SettingsComponent settings(diagnosticStrings, diagnosticDevices,
                                       diagnosticProperties, [] {});
            settings.setBounds(0, 0, 720, 570);
            settings.resized();
            const auto tabs = settings.diagnosticTabCount();
            const auto pageChildren = settings.diagnosticCurrentPageChildCount();
            const auto inferenceDevices = settings.diagnosticInferenceDeviceCount();
            std::cout << "tabs=" << tabs << '\n'
                      << "page_children=" << pageChildren << '\n'
                      << "inference_devices=" << inferenceDevices << '\n'
                      << "width=" << settings.getWidth() << '\n'
                      << "height=" << settings.getHeight() << std::endl;
            if (tabs != 5 || pageChildren <= 0 || inferenceDevices != 9)
                setApplicationReturnValue(5);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--inspect-analysis")
        {
            auto config = backend::AnalysisService::configFromEnvironment();
            if (arguments.size() >= 2) config.gameModelDirectory = juce::File(arguments[1]);
            if (arguments.size() >= 3) config.fcpeModelPath = juce::File(arguments[2]);
            if (arguments.size() >= 4)
                config.performanceMode = arguments[3].equalsIgnoreCase("small");
            const auto status = backend::AnalysisService::status(config);
            std::cout << "requested=" << status.requestedBackend << '\n'
                      << "active=" << backend::AnalysisService::backendText(status) << '\n'
                      << "game_variant=" << (status.performanceMode ? "small" : "large") << '\n'
                      << "game_ready=" << (status.gameModelReady ? 1 : 0) << '\n'
                      << "game_path=" << status.gameModelDirectory.getFullPathName() << '\n'
                      << "fcpe_ready=" << (status.fcpeModelReady ? 1 : 0) << '\n'
                      << "fcpe_path=" << status.fcpeModelPath.getFullPathName() << '\n'
                      << "onnx_runtime=" << (status.onnxRuntimeReady ? 1 : 0) << '\n'
                      << "inference_requested=" << status.requestedInference << '\n'
                      << "inference_active=" << status.activeInference << '\n'
                      << "message=" << status.message << std::endl;
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--inspect-fcpe")
        {
            juce::String error;
            auto frames = backend::FcpeAnalyzer::analyse(
                juce::File(arguments[1]), juce::File(arguments[2]),
                { backend::InferenceBackend::cpu, -1,
                  std::max(1, juce::SystemStats::getNumCpus()) }, error);
            std::vector<float> voiced;
            for (const auto& frame : frames)
                if (frame.voiced) voiced.push_back(frame.midi);
            auto medianMidi = 0.0f;
            if (!voiced.empty())
            {
                const auto middle = voiced.begin()
                    + static_cast<std::ptrdiff_t>(voiced.size() / 2);
                std::nth_element(voiced.begin(), middle, voiced.end());
                medianMidi = *middle;
            }
            std::cout << "frames=" << frames.size() << '\n'
                      << "voiced_frames=" << voiced.size() << '\n'
                      << "median_midi=" << medianMidi << '\n'
                      << "error=" << error << std::endl;
            if (frames.empty()) setApplicationReturnValue(6);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 3 && arguments[0] == "--smoke-export")
        {
            cliAudioEngine = std::make_unique<AudioEngine>();
            const auto input = juce::File(arguments[1]);
            const auto duration = cliAudioEngine->probeDuration(input);
            if (!duration)
            {
                std::cerr << "error=audio_read" << std::endl;
                setApplicationReturnValue(2);
            }
            else
            {
                ProjectModel model;
                (void) model.addAudioFile(input, *duration);
                auto data = model.snapshot();
                if (!data.tracks.empty()) data.tracks.front().compose = false;
                cliAudioEngine->syncProject(data);
                juce::String error;
                if (cliAudioEngine->exportWav(juce::File(arguments[2]), error))
                    std::cout << "duration=" << *duration << '\n'
                              << "output=" << arguments[2] << std::endl;
                else
                {
                    std::cerr << "error=" << error << std::endl;
                    setApplicationReturnValue(3);
                }
            }
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 4 && arguments[0] == "--smoke-mld5")
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            auto reader = std::unique_ptr<juce::AudioFormatReader>(
                formats.createReaderFor(juce::File(arguments[1])));
            if (reader == nullptr)
            {
                std::cerr << "error=audio_read" << std::endl;
                setApplicationReturnValue(2);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto sourceDuration = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
            const auto shift = arguments[2].getFloatValue();
            const auto stretch = juce::jlimit(0.25, 4.0, arguments[3].getDoubleValue());
            backend::Mld5FileRenderRequest request;
            request.sourceFile = juce::File(arguments[1]);
            request.sourceDurationSeconds = sourceDuration;
            request.targetDurationSeconds = sourceDuration * stretch;
            const auto frames = std::max(2, static_cast<int>(std::ceil(
                request.targetDurationSeconds / 0.005)) + 1);
            request.sourceMidi.assign(static_cast<std::size_t>(frames), 60.0f);
            request.targetMidi.assign(static_cast<std::size_t>(frames), 60.0f + shift);
            const auto outputFile = arguments.size() >= 5 ? juce::File(arguments[4]) : juce::File();
            const auto formant = arguments.size() >= 6 ? arguments[5].getFloatValue() : 0.0f;
            const auto gain = arguments.size() >= 7 ? arguments[6].getFloatValue() : 1.0f;
            const auto breath = arguments.size() >= 8 ? arguments[7].getFloatValue() : 0.0f;
            request.formantSemitones.assign(static_cast<std::size_t>(frames), formant);
            request.noteGain.assign(static_cast<std::size_t>(frames), gain);
            request.breath.assign(static_cast<std::size_t>(frames), breath);
            if (arguments.size() >= 9)
            {
                const auto backendName = arguments[8].toLowerCase();
                request.pitchBackend = backendName.contains("nsf")
                    ? backend::PitchRenderBackend::nsfHifigan
                    : backendName == "world" ? backend::PitchRenderBackend::world
                    : backendName.contains("vslib") ? backend::PitchRenderBackend::vslib
                     : backendName == "mld3" ? backend::PitchRenderBackend::mld3
                     : backendName == "mld5" ? backend::PitchRenderBackend::mld5
                     : backendName == "llsm2" ? backend::PitchRenderBackend::llsm2
                     : backend::PitchRenderBackend::llsm2;
            }
            const auto tension = arguments.size() >= 10 ? arguments[9].getFloatValue() : 0.0f;
            request.tension.assign(static_cast<std::size_t>(frames), tension);
            if (arguments.size() >= 11)
                request.stretchAlgorithm = juce::jlimit(0, 3, arguments[10].getIntValue());
            cliRenderService = std::make_unique<backend::RenderService>();
            cliRenderService->renderMld5File(std::move(request), [this, outputFile](backend::RenderedAudio result)
            {
                if (result.buffer.getNumSamples() <= 0)
                {
                    std::cerr << "error=render_empty" << std::endl;
                    setApplicationReturnValue(3);
                }
                else
                {
                    double squareSum = 0.0;
                    for (int channel = 0; channel < result.buffer.getNumChannels(); ++channel)
                        for (int sample = 0; sample < result.buffer.getNumSamples(); ++sample)
                        {
                            const auto value = result.buffer.getSample(channel, sample);
                            squareSum += static_cast<double>(value) * value;
                        }
                    const auto count = std::max(1, result.buffer.getNumChannels()
                                                  * result.buffer.getNumSamples());
                    std::cout << "sample_rate=" << result.sampleRate << '\n'
                              << "backend=" << result.backend << '\n'
                              << "channels=" << result.buffer.getNumChannels() << '\n'
                              << "samples=" << result.buffer.getNumSamples() << '\n'
                              << "rms=" << std::sqrt(squareSum / static_cast<double>(count)) << std::endl;
                    if (outputFile != juce::File())
                    {
                        outputFile.deleteFile();
                        auto stream = outputFile.createOutputStream();
                        juce::WavAudioFormat wav;
                        auto writer = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(
                            stream.release(), result.sampleRate,
                            static_cast<unsigned int>(result.buffer.getNumChannels()), 24, {}, 0));
                        if (writer == nullptr
                            || !writer->writeFromAudioSampleBuffer(result.buffer, 0,
                                                                   result.buffer.getNumSamples()))
                        {
                            std::cerr << "error=audio_write" << std::endl;
                            setApplicationReturnValue(4);
                        }
                        else std::cout << "output=" << outputFile.getFullPathName() << std::endl;
                    }
                }
                quit();
            });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-utau-selection")
        {
            ProjectModel model;
            juce::String loadError;
            if (!model.load(juce::File(arguments[1].unquoted()), loadError))
            {
                std::cerr << "error=project_load\nmessage=" << loadError << std::endl;
                setApplicationReturnValue(2);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }

            auto data = model.snapshot();
            TrackData selectedTrack;
            ClipData selectedClip;
            NoteData selectedNote;
            auto found = false;
            for (const auto& track : data.tracks)
            {
                if (track.pitchAlgorithm != PitchAlgorithm::utau) continue;
                for (const auto& clip : track.clips)
                    for (const auto& note : clip.notes)
                        if (note.label.trim().isNotEmpty())
                        {
                            selectedTrack = track;
                            selectedClip = clip;
                            selectedNote = note;
                            found = true;
                            break;
                        }
                if (found) break;
            }
            if (!found)
            {
                std::cerr << "error=no_labeled_utau_note" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }

            // Keep the original note timing so this exercises the same local
            // timeline-offset path used by marquee rendering, while bounding
            // the offline verification file to the selected phrase.
            selectedClip.durationSeconds = std::min(selectedClip.durationSeconds,
                selectedNote.startSeconds + selectedNote.durationSeconds + 0.3);
            selectedTrack.clips = { selectedClip };
            data.tracks = { selectedTrack };
            cliAudioEngine = std::make_unique<AudioEngine>();
            if (arguments.size() >= 3)
                cliAudioEngine->setUtauResamplerFile(juce::File(arguments[2].unquoted()));
            cliAudioEngine->setUtauRenderNoteSelection({ selectedNote.id });
            cliAudioEngine->syncProject(data);

            auto sawProgress = false;
            auto lastProgress = -1.0;
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15'000.0;
            while (juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                if (const auto progress = cliAudioEngine->renderProgress())
                {
                    sawProgress = true;
                    lastProgress = *progress;
                }
                else break;
                juce::Thread::sleep(10);
            }

            const auto ready = cliAudioEngine->hasPlayableRenderedAudio();
            auto rms = 0.0;
            auto outputSamples = juce::int64(0);
            auto output = juce::File::createTempFile("hachi-utau-selection.wav");
            juce::String exportError;
            if (ready && cliAudioEngine->exportWav(output, exportError))
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                if (auto reader = std::unique_ptr<juce::AudioFormatReader>(
                        formats.createReaderFor(output)))
                {
                    outputSamples = reader->lengthInSamples;
                    juce::AudioBuffer<float> buffer(
                        static_cast<int>(reader->numChannels),
                        static_cast<int>(reader->lengthInSamples));
                    reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
                    auto squareSum = 0.0;
                    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                        {
                            const auto value = buffer.getSample(channel, sample);
                            squareSum += static_cast<double>(value) * value;
                        }
                    const auto count = std::max(1, buffer.getNumChannels()
                                                   * buffer.getNumSamples());
                    rms = std::sqrt(squareSum / static_cast<double>(count));
                }
            }
            output.deleteFile();
            std::cout << "track=" << selectedTrack.name << '\n'
                      << "compose=" << (selectedTrack.compose ? 1 : 0) << '\n'
                      << "voicebank=" << selectedTrack.voicebankDirectory.getFullPathName() << '\n'
                      << "note_id=" << selectedNote.id << '\n'
                      << "alias=" << selectedNote.label << '\n'
                      << "note_start=" << selectedNote.startSeconds << '\n'
                      << "saw_progress=" << (sawProgress ? 1 : 0) << '\n'
                      << "last_progress=" << lastProgress << '\n'
                      << "ready=" << (ready ? 1 : 0) << '\n'
                      << "backend=" << cliAudioEngine->activeRenderBackends() << '\n'
                      << "samples=" << outputSamples << '\n'
                      << "rms=" << rms << '\n'
                      << "export_error=" << exportError << std::endl;
            if (!ready || rms <= 1.0e-5) setApplicationReturnValue(4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--smoke-utau-voicebank")
        {
            backend::UtauRenderRequest request;
            request.voicebankDirectory = juce::File(arguments[1].unquoted());
            if (arguments.size() >= 3)
                request.resamplerExecutable = juce::File(arguments[2].unquoted());
            const auto alias = arguments.size() >= 4 ? arguments[3].unquoted() : juce::String("a");
            request.targetDurationSeconds = 1.0;
            request.bpm = 120.0;
            request.notes = { { alias, {}, 0.08, 0.75, 60.0f, 1.0f } };
            if (arguments.size() >= 5)
            {
                request.notes.front().durationSeconds = 0.36;
                request.notes.push_back({ arguments[4].unquoted(), {},
                    0.44, 0.42, 62.0f, 1.0f });
            }
            const auto consonantVelocity = arguments.size() >= 6
                ? arguments[5].getIntValue() : 100;
            const auto globalFlags = arguments.size() >= 7
                ? arguments[6].unquoted() : juce::String{};
            const auto preutteranceMs = arguments.size() >= 8
                ? std::optional<double>(arguments[7].getDoubleValue()) : std::nullopt;
            const auto overlapMs = arguments.size() >= 9
                ? std::optional<double>(arguments[8].getDoubleValue()) : std::nullopt;
            for (auto& note : request.notes)
            {
                note.consonantVelocity = consonantVelocity;
                note.flags = globalFlags;
                if (preutteranceMs)
                {
                    note.preutteranceOverrideEnabled = true;
                    note.preutteranceSeconds = std::max(0.0, *preutteranceMs / 1000.0);
                }
                if (overlapMs)
                {
                    note.overlapOverrideEnabled = true;
                    note.overlapSeconds = *overlapMs / 1000.0;
                }
            }
            const auto firstStart = juce::Time::getMillisecondCounterHiRes();
            const auto result = backend::UtauRenderer::render(request);
            const auto firstMilliseconds = juce::Time::getMillisecondCounterHiRes() - firstStart;
            const auto cachedStart = juce::Time::getMillisecondCounterHiRes();
            const auto cachedResult = backend::UtauRenderer::render(request);
            const auto cachedMilliseconds = juce::Time::getMillisecondCounterHiRes() - cachedStart;
            auto squareSum = 0.0;
            for (int channel = 0; channel < result.buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < result.buffer.getNumSamples(); ++sample)
                {
                    const auto value = result.buffer.getSample(channel, sample);
                    squareSum += static_cast<double>(value) * value;
                }
            const auto count = std::max(1, result.buffer.getNumChannels()
                                          * result.buffer.getNumSamples());
            const auto rms = std::sqrt(squareSum / static_cast<double>(count));
            const auto timing = backend::UtauRenderer::sampleTiming(
                request.voicebankDirectory, alias, request.notes.front().midiNote,
                consonantVelocity);
            std::cout << "voicebank=" << request.voicebankDirectory.getFullPathName() << '\n'
                      << "alias=" << alias << '\n'
                      << "notes=" << request.notes.size() << '\n'
                      << "velocity=" << consonantVelocity << '\n'
                      << "flags=" << globalFlags << '\n'
                      << "preutterance_override_ms="
                      << (preutteranceMs ? *preutteranceMs : -1.0) << '\n'
                      << "overlap_override_ms="
                      << (overlapMs ? *overlapMs : 0.0) << '\n'
                      << "display_preutterance="
                      << (timing ? timing->preutteranceSeconds : -1.0) << '\n'
                      << "display_consonant="
                      << (timing ? timing->consonantSeconds : -1.0) << '\n'
                      << "backend=" << result.backend << '\n'
                      << "samples=" << result.buffer.getNumSamples() << '\n'
                      << "rms=" << rms << '\n'
                      << "first_ms=" << firstMilliseconds << '\n'
                      << "cached_ms=" << cachedMilliseconds << '\n'
                      << "warning=" << result.warning << std::endl;
            if (rms <= 1.0e-5 || result.buffer.getNumSamples() <= 0
                || cachedResult.buffer.getNumSamples() <= 0)
                setApplicationReturnValue(4);
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (!arguments.isEmpty() && arguments[0] == "--smoke-utau")
        {
            auto voicebank = juce::File::createTempFile("hachi-utau-smoke");
            voicebank.deleteFile();
            if (!voicebank.createDirectory())
            {
                std::cerr << "error=temp_directory" << std::endl;
                setApplicationReturnValue(2);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            const auto sampleFile = voicebank.getChildFile("a.wav");
            constexpr auto rate = 44'100.0;
            juce::AudioBuffer<float> source(1, static_cast<int>(rate * 0.8));
            for (int index = 0; index < source.getNumSamples(); ++index)
            {
                const auto time = static_cast<double>(index) / rate;
                const auto edge = std::min({ 1.0, time / 0.02, (0.8 - time) / 0.02 });
                source.setSample(0, index, static_cast<float>(
                    0.28 * std::max(0.0, edge) * std::sin(
                        juce::MathConstants<double>::twoPi * 261.625565 * time)));
            }
            auto stream = sampleFile.createOutputStream();
            juce::WavAudioFormat wav;
            auto writer = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(
                stream.release(), rate, 1, 24, {}, 0));
            if (writer == nullptr
                || !writer->writeFromAudioSampleBuffer(source, 0, source.getNumSamples()))
            {
                std::cerr << "error=sample_write" << std::endl;
                setApplicationReturnValue(3);
                juce::MessageManager::callAsync([this] { quit(); });
                return;
            }
            writer.reset();
            SampleRegionSetting row;
            row.name = "a";
            row.regionEndSeconds = 0.8;
            row.alignmentSeconds = 0.08;
            row.fixedDurationSeconds = 0.15;
            row.overlapSeconds = 0.03;
            row.melodyneOriginalPitchCenterCents = 6000.0;
            juce::String sidecarError;
            SampleSettings::save(sampleFile, { row }, sidecarError);
            backend::UtauRenderRequest request;
            request.voicebankDirectory = voicebank;
            request.targetDurationSeconds = 1.2;
            request.notes = {
                { "a", "", 0.10, 0.38, 60.0f, 1.0f },
                { "a", "g-5Y0", 0.55, 0.50, 67.0f, 1.0f }
            };
            auto result = backend::UtauRenderer::render(request);
            auto squareSum = 0.0;
            for (int channel = 0; channel < result.buffer.getNumChannels(); ++channel)
                for (int sample = 0; sample < result.buffer.getNumSamples(); ++sample)
                {
                    const auto value = result.buffer.getSample(channel, sample);
                    squareSum += static_cast<double>(value) * value;
                }
            const auto count = std::max(1, result.buffer.getNumChannels()
                                          * result.buffer.getNumSamples());
            const auto rms = std::sqrt(squareSum / static_cast<double>(count));
            std::cout << "backend=" << result.backend << '\n'
                      << "sample_rate=" << result.sampleRate << '\n'
                      << "samples=" << result.buffer.getNumSamples() << '\n'
                      << "rms=" << rms << '\n'
                      << "warning=" << result.warning << std::endl;
            if (rms <= 1.0e-5 || result.buffer.getNumSamples() <= 0)
                setApplicationReturnValue(4);
            if (arguments.size() >= 2)
            {
                const auto output = juce::File(arguments[1]);
                output.deleteFile();
                auto outputStream = output.createOutputStream();
                auto outputWriter = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(
                    outputStream.release(), result.sampleRate, 2, 24, {}, 0));
                if (outputWriter != nullptr)
                    outputWriter->writeFromAudioSampleBuffer(result.buffer, 0,
                                                              result.buffer.getNumSamples());
            }
            sampleFile.deleteFile();
            SampleSettings::sidecarFor(sampleFile).deleteFile();
            voicebank.deleteRecursively();
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--inspect-midi")
        {
            ProjectModel model;
            juce::String error;
            if (!model.addMidiFile(juce::File(arguments[1]), error))
            {
                std::cerr << "error=" << error << std::endl;
                setApplicationReturnValue(2);
            }
            else
            {
                const auto project = model.snapshot();
                std::size_t notes = 0;
                for (const auto& track : project.tracks)
                    for (const auto& clip : track.clips) notes += clip.notes.size();
                std::cout << "project=" << project.name << '\n'
                          << "bpm=" << project.bpm << '\n'
                          << "tracks=" << project.tracks.size() << '\n'
                          << "notes=" << notes << std::endl;
            }
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--inspect-audio")
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const auto file = juce::File(arguments[1]);
            auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
            if (reader == nullptr || reader->sampleRate <= 0.0)
            {
                std::cerr << "error=audio_read" << std::endl;
                setApplicationReturnValue(2);
            }
            else
            {
                ProjectModel model;
                const auto clipId = model.addAudioFile(file,
                    static_cast<double>(reader->lengthInSamples) / reader->sampleRate);
                juce::String analysisError;
                const auto analysisConfig = backend::AnalysisService::configFromEnvironment();
                auto analysis = backend::AnalysisService::analyse(
                    file, analysisConfig, analysisError);
                const auto analysisBackend = backend::AnalysisService::backendText(analysis.status);
                (void) model.setClipNotesIfEmpty(clipId, std::move(analysis.notes));
                const auto data = model.snapshot();
                std::size_t notes = 0;
                for (const auto& track : data.tracks)
                    for (const auto& clip : track.clips) notes += clip.notes.size();
                std::cout << "tracks=" << data.tracks.size() << '\n'
                          << "notes=" << notes << '\n'
                          << "analysis=" << (analysisError.isEmpty() ? analysisBackend : "skipped")
                          << '\n' << "analysis_warning=" << analysis.warning
                          << std::endl;
            }
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        if (arguments.size() >= 2 && arguments[0] == "--inspect-mpd")
        {
            juce::String error;
            const auto imported = backend::MelodyneImporter::importProject(juce::File(arguments[1]), error);
            if (!imported)
            {
                std::cerr << "error=" << error << std::endl;
                setApplicationReturnValue(2);
            }
            else
            {
                std::size_t clips = 0;
                std::size_t notes = 0;
                std::size_t mappedClips = 0;
                std::size_t sourceTimePoints = 0;
                std::size_t pitchPoints = 0;
                std::size_t unvoicedPitchPoints = 0;
                std::size_t flatNotes = 0;
                auto flatTargetMaximumDeviation = 0.0f;
                auto minModulation = 2.0f;
                auto maxModulation = 0.0f;
                auto minAttackSpeed = std::numeric_limits<float>::max();
                auto maxAttackSpeed = 0.0f;
                for (const auto& track : imported->project.tracks)
                {
                    clips += track.clips.size();
                    for (const auto& clip : track.clips)
                    {
                        if (!clip.sourceTimeMap.empty()) ++mappedClips;
                        sourceTimePoints += clip.sourceTimeMap.size();
                        for (const auto& note : clip.notes)
                        {
                            ++notes;
                            minModulation = std::min(minModulation, note.modulation);
                            maxModulation = std::max(maxModulation, note.modulation);
                            minAttackSpeed = std::min(minAttackSpeed, note.attackSpeed);
                            maxAttackSpeed = std::max(maxAttackSpeed, note.attackSpeed);
                            pitchPoints += note.contour.size();
                            unvoicedPitchPoints += static_cast<std::size_t>(std::count_if(
                                note.contour.begin(), note.contour.end(),
                                [](const auto& point) { return !point.voiced; }));
                            if (note.modulation <= 1.0e-4f)
                            {
                                ++flatNotes;
                                for (const auto& point : note.contour)
                                    if (point.voiced)
                                        flatTargetMaximumDeviation = std::max(
                                            flatTargetMaximumDeviation,
                                            std::abs(renderedPitchCents(note, point)));
                            }
                        }
                    }
                }
                std::cout << "project=" << imported->project.name << '\n'
                          << "bpm=" << imported->project.bpm << '\n'
                          << "beat_origin=" << imported->project.beatOriginSeconds << '\n'
                          << "tracks=" << imported->project.tracks.size() << '\n'
                          << "clips=" << clips << '\n'
                          << "notes=" << notes << '\n'
                          << "mapped_clips=" << mappedClips << '\n'
                          << "source_time_points=" << sourceTimePoints << '\n'
                          << "pitch_points=" << pitchPoints << '\n'
                          << "unvoiced_pitch_points=" << unvoicedPitchPoints << '\n'
                          << "flat_notes=" << flatNotes << '\n'
                          << "flat_target_max_deviation_cents="
                          << flatTargetMaximumDeviation << '\n'
                          << "modulation_min=" << (notes > 0 ? minModulation : 0.0f) << '\n'
                          << "modulation_max=" << (notes > 0 ? maxModulation : 0.0f) << '\n'
                          << "attack_speed_min=" << (notes > 0 ? minAttackSpeed : 0.0f) << '\n'
                          << "attack_speed_max=" << (notes > 0 ? maxAttackSpeed : 0.0f) << '\n'
                          << "missing=" << imported->missingFiles.size() << std::endl;
            }
            juce::MessageManager::callAsync([this] { quit(); });
            return;
        }
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
        if (!arguments.isEmpty())
            // Unquoted like every other path taken from the command line:
            // a quoted argument keeps its quotes here, and the file simply
            // was not found.
            mainWindow->openFile(juce::File(arguments[0].unquoted()));
    }

    void shutdown() override
    {
        mainWindow.reset();
        cliRenderService.reset();
        cliAudioEngine.reset();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->requestClose([this] { quit(); });
            return;
        }
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name, Palette::background, DocumentWindow::allButtons, false)
        {
            setUsingNativeTitleBar(true);
            startupLog("MainWindow: constructed, creating editor");
            std::cerr << "startup: creating editor" << std::endl;
            setContentOwned(new MainComponent(), true);
            startupLog("MainWindow: editor created");
            std::cerr << "startup: editor created" << std::endl;
            startupLog("MainWindow: setResizable begin");
            setResizable(true, false);
            startupLog("MainWindow: setResizeLimits begin");
            setResizeLimits(900, 560, 8192, 8192);
            startupLog("MainWindow: centreWithSize begin");
            centreWithSize(1280, 760);
            startupLog("MainWindow: addToDesktop begin");
            addToDesktop(getDesktopWindowStyleFlags());
            startupLog("MainWindow: native peer created");
            if (auto* editor = dynamic_cast<MainComponent*>(getContentComponent()))
                editor->applyRenderingPreference();
            setVisible(true);
            startupLog("MainWindow: visible");
            juce::MessageManager::callAsync([] { startupLog("Application: message loop responsive"); });
            std::cerr << "startup: window visible" << std::endl;
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void openFile(const juce::File& file)
        {
            if (auto* component = dynamic_cast<MainComponent*>(getContentComponent()))
                component->openExternalFile(file);
        }



        void requestClose(std::function<void()> approved)
        {
            if (auto* component = dynamic_cast<MainComponent*>(getContentComponent()))
                component->requestClose(std::move(approved));
            else if (approved)
                approved();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<backend::RenderService> cliRenderService;
    std::unique_ptr<AudioEngine> cliAudioEngine;
};
}

START_JUCE_APPLICATION(hachi::HachiShifterApplication)
