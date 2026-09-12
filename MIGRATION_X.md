# HachiShifter-x integration

## Baseline and scope

Destination: `next`, baseline `2871d55`. Source: the supplied `HachiShifter-x`
source snapshot (no independent Git history). This is a source integration;
comments and bundled smoke tests do not by themselves establish passing tests.

## Source differences reviewed before integration

- Adds UTAU classic / Jie / Mou modes, UST parsing, external resampler and
  internal wavtool-style mixing, voicebank selection, OTO and region editors.
- Adds sparse pitch anchors with segment shapes and Bezier handles, amplitude
  and flag curves, vibrato parameters/presets/baking, note context menus,
  view toggles, selection playback, reference tracks and tempo changes.
- Adds project persistence for those fields, per-note render caching, rendered
  UTAU waveform snapshots, region/track export and command-line smoke checks.
- Refactors NSF phrase scheduling into `glideChains` / `mergedRequestFor`.
  The NSF model implementation itself is unchanged from the destination.
- UTAU edits are score/voicebank oriented. Insertion and merge can ripple
  subsequent notes; lyrics select a sample; negative pitch-anchor times address
  preutterance. These are not the rules for editing an existing recording.
- The source uses mono 16-bit WAV export and enables overlap smoothing by
  default. Neither is a required part of the feature migration.

## Melodyne compatibility requirements

1. Preserve imported clip start, duration, selected source range and time map.
   Never extend a source selection to the end of its file to fill silence.
2. Preserve each note's pitch centre, source centre, local contour and expression.
   A connection is not permission to concatenate incompatible relative contours.
3. UTAU ripple editing and voicebank/lyric substitution stay on UTAU tracks.
4. Pitch-point editing must retain measured source F0; source-edit mode must
   continue to use source coordinates instead of project coordinates.
5. Preserve `next` overlap-smoothing default (off), and stereo 24-bit WAV export.
6. Continuous phrase slices must finish on error as well as success; unreadable
   members must not leave orphaned pending slices.
7. Render-order controls must select a supported backend, never silently route
   non-NSF tracks to NSF.
8. Melodyne projects may represent a consonant with no clear pitch line as a
   full note immediately before its vowel.  When importing or converting to
   UTAU-compatible timing, treat that note as the following vowel's consonant
   candidate, merge it into the vowel's consonant region and adjust the target
   preutterance instead of rendering it as an independent pitched vowel.
9. Melodyne consonant/vowel boundaries map to UTAU preutterance semantics.
   Melodyne note amplitude decay may inform UTAU-style overlap gain/envelope.
   Melodyne does not expose UTAU fixed/stretch region boundaries, so do not
   invent them as imported facts; any UTAU stretch planning must be a visible,
   editable conversion suggestion derived later from OTO and target duration.
10. The self-developed Melodyne importer and merged renderer continue in
    parallel, but incomplete reverse-engineered algorithms stay disabled by
    default and must not appear in normal user-facing algorithm pickers.
11. Melodyne-native import/render integration should prefer detecting and
    calling the user's installed, licensed Melodyne through a supported host/API
    path.  In the WSL development environment, Windows installations are
    discoverable through `/mnt/...`; no Melodyne DLLs or licence material are
    copied into this repository.

## UI migration status

- Pitch draw, line and point tools are visible in ordinary audio tracks and
  UTAU tracks.
- Note-menu vibrato items are shared between UTAU and ordinary audio tracks;
  UTAU-only items remain limited to timing, OTO/STP, region flags, lyric entry
  and gap/ripple operations.
- Vibrato handles and real-line display are no longer gated on UTAU tracks.
- `mld3` remains loadable from project data for compatibility, but is hidden
  from the normal toolbar and Melodyne-import default algorithm settings until
  its importer/render path is fully validated.
- `next` defaults are restored: new/legacy-missing tracks use
  `smoothOverlaps=false`, and project WAV export writes stereo 24-bit audio.
- `/mnt/c` probing currently finds Melodyne 5/4 executables, VST3 and core DLL
  candidates.  This is discovery only; no DLL or licence is loaded.
- `MelodyneProvider` now gates the import button and merged-render scheduler.
  Both native provider operations and self-developed experimental operations
  remain disabled until an actual supported host/API contract is implemented.
- The importer exposes a conservative consonant-candidate mapping helper, but
  it does not yet rewrite imported notes.  Full mapping into UTAU preutterance,
  overlap gain and editable conversion suggestions remains the next task.
- First-time MPD import now seeds the existing per-audio HJM sidecar through
  `SampleSettings::save()`.  Melodyne note range, pitch centres, drift,
  modulation, formant, amplitude, sibilance and consonant/attack timing are
  stored there; subsequent audio and UTAU workflows consume the same project
  annotation format rather than introducing a second annotation file.
- Sidecar seeding now maps note-local target time back through the clip's source
  time map.  A conservative adjacent pitchless-note rule folds such a note into
  the following voiced note's HJM region, extending its fixed/consonant portion
  and carrying the source overlap.  The source Melodyne notes remain intact in
  the in-memory import result; this conversion only affects the first generated
  material annotation.
- When an HJM sidecar already exists, first-time Melodyne import compares the
  generated rows with the existing rows.  A mismatch asks whether to use the
  new Melodyne-derived annotation, retain the existing annotation, or cancel;
  the old file is never overwritten silently.
- VST3 probing is now implemented through JUCE's plugin host boundary.  In the
  current WSL process it correctly reports the Windows VST3 as discovered but
  not hostable; a Windows build can proceed to plugin description and instance
  creation.  This does not claim ARA support or traditional MPD compatibility.

## Validation strategy

- Keep the original NSF model regression tests and integrate the new UTAU,
  UST, pitch-point, menu, waveform, envelope and OTO checks.
- Test native and UTAU editing separately: overlap, resize, split, merge,
  source-F0 preservation and save/reload.
- Check the imported source tree/build registration and syntax before a full
  application build. Build/test results and outstanding limits are recorded
  after verification, not inferred from file sizes or changed audio hashes.
