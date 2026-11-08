# Key

Listens to audio and shows the key, scale, tempo and reference pitch, live. Audio passes through bit identical, with no latency. It also writes the key to a shared file so other Smooth plug-ins can read it.

## Load it

Audio Units > Luke Garito > Key. Works on mono and stereo. Best on a mix bus or a beat for chords, or on a vocal with Source set to Single Voice.

## Controls

All are AU parameters, so they can be automated and set by name.

| Control | Values | What it does |
|---|---|---|
| Mode | Listen, Manual | Listen detects. Manual uses Manual Key and Manual Scale, at confidence 100%, and does not analyse |
| Response | Fast, Stable | Stable first reads after about 8 s, later when two keys are close, and never bounces between candidates. Fast first reads after about 3 s and may move more |
| Source | Full Mix, Single Voice | Full Mix uses the bass line and chord movement. Single Voice ignores those and anything below A2, such as rumble or spill |
| Listening | on, off | Off holds the reading and stops analysing |
| Reset | press | Forgets everything and starts listening again |
| Manual Key | C to B | The key in Manual mode. Automate this to follow planned key changes |
| Manual Scale | Major, Minor | The scale in Manual mode |
| Relative Swap | on, off | Shows and writes the relative key of the one detected, for when it picks C major and you hear A minor |
| Tempo Scale | As Detected, Half, Double | Halves or doubles the tempo shown |
| Bypass | on, off | Audio is unchanged either way. Bypassed, it does not listen |

Readings, which ignore writes and never write automation: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo.

## Presets

Whole Song (Stable, Full Mix). Quick Check (Fast, Full Mix). Vocal Track (Stable, Single Voice). Vocal Quick Check (Fast, Single Voice). Set Key By Hand (Manual).

## Window

A sheet of paper with the key on it. See `docs/window/`.

- Key and scale in heavy black type, as large as they fit: the letters are about 40% of the window's height, and every key is the same size so the reading does not jump.
- Circle of fifths, major outside and minor inside. The darker a key, the more likely it looks right now. Red is the key on show. The middle says how sure it is.
- Keyboard, one octave, every note named. Notes of the key are solid, the home note is red and bold, and dots grow with the notes sounding now.
- Relative key with its Swap button, and the other spelling (Bb for A#).
- Tuning, red when the music is 1 Hz or more from A 440, with how many cents. Tempo, with Half, Normal and Double.
- Top row: Mode, Listening, Reset, Source, and what it is doing now. Advanced, in the footer: Manual Key, Manual Scale, Response, Bypass. Controls that do nothing in Manual dim but still work.
- In Manual, click a key on the circle or a note on the keyboard, or click the big letters and type a key such as F# minor, Bb or Am. Manual Key can also be typed, dragged or scrolled.
- Double-click a control to put it back. A one line hint explains whatever is under the mouse. If key.json can't be written, a small red note says so in the footer.
- Resize from the corner. The size and whether Advanced is open are saved with the session. Nothing else was added to saved state.

The window sets only real parameters, with begin and end gestures, and automation moves it. It reads the processor without locks and never touches the audio thread.

Type is Jost, under the SIL Open Font License in `assets/fonts/OFL.txt`. It is a variable font and JUCE can only use its default weight, so `src/editor/FontOutlines.cpp` gets the letter shapes at any weight from CoreText.

CPU of the open window, real window at Retina size. Nothing else building: 1.9, 2.1, 2.1, 2.2% of one core on real speech, and 1.1, 3.1, 3.3, 4.0% on steady test music (the higher ones landed on slow cores, 0.8% of a fast core's work each time). Silent with nothing drawing, with other builds running: 0.5, 0.6, 0.6, 0.8%. Earlier runs on speech, with other builds pushing it onto slow cores: 2.6, 2.6, 2.9, 2.9, 5.0, 5.9, 6.7%, for the same 2.4 to 3.0% of a fast core's work. Audio CPU did not change: 0.4% before and after.

Render every state to PNG with real speech from macOS `say`: `build/RenderWindow_artefacts/Release/RenderWindow docs/window`. Measure the real window's CPU: `build/RenderWindow_artefacts/Release/RenderWindow --live 5 6 speech`.

## Shared file

`~/Library/Application Support/SmoothPlugins/key.json`, one line:

```
{"key":"C","scale":"major","confidence":0.87,"time":1794011292.5,"listening":true}
```

- `key`: C C# D D# E F F# G G# A A# B. `scale`: major or minor. `confidence`: 0 to 1. `time`: Unix seconds of this write. `listening`: true while detecting, false when paused, manual, bypassed or closed.
- Written from the background thread only, at most about twice a second, only when something changes (key, scale, listening, or confidence by 0.02). Atomic: a temp file in the same folder, then rename. The folder is made if missing.
- The home folder comes from `getpwuid`, not HOME.
- Nothing is written until there is a key. The format has no empty key, so after Reset the last key stays with confidence 0. Treat low confidence as no key.
- `time` is the last change, not a heartbeat: a steady old time can just mean nothing changed.
- With several instances, the last one to write wins. If a write fails, the plug-in carries on, shows a note and retries every 5 s.
- Writing from inside Logic has not been tested. Only offline tests have written this file.

## Signal flow

Audio in goes straight to audio out, never written to. A cleaned mono copy goes through a lock-free queue to one background thread, which: decimates to about 12 kHz, runs its own FFT over 0.7 s windows, estimates tuning from spectral peaks, works out which notes explain the spectrum (each note as a stack of harmonics), folds them into treble and bass note memories, and scores all 24 keys. The score uses a listener-rated key profile, a bass profile, time on each key's home chord, dominant chords resolving, and the opening chord as a tie break between relative keys. The reading moves only after a new key has led for a set time by a set margin. When recent notes clearly leave the key, old memory is mostly dropped. Tempo comes from a separate curve of spectral rises, matched against steady pulses.

Why not plain profile correlation: a note's own harmonics add false fifths and thirds, and relative keys have the same notes, so correlation alone can't separate them.

## Measured

Full numbers in `tests/results.txt`. Synthetic test music only: it doesn't prove accuracy on real songs or a real voice. Limits are in `tests/PassLimits.h`, set before results. 1.3.0's numbers use a fifth held out seed, 20261110, run at the end. The detector was tuned on dev seeds 1000 to 1005 only. Earlier seeds and how 1.1.1 compares on this one are in `CHANGELOG.md`.

Latency 0 at all rates. Output bit identical over 720 blocks, including NaN and huge samples, listening, paused, manual and bypassed. No malloc on the audio thread, checked with the system hook.

| Test, all 24 keys, 5 timbres | Limit | Result |
|---|---|---|
| Chords exact | at least 90% | 99.4% |
| Chords exact, relative or a fifth away | at least 98% | 100% |
| Median time to correct, Stable | up to 8 s | 7.85 s |
| Median time to correct, Fast | up to 5 s | 3.07 s |
| Stable bounces, A to B and back | 0 | 0 |
| Stable clips moving after 8 s | up to 1% | 0.83% |
| Key change followed in 30 s, Stable | at least 90% | 100% |
| Key change median, Stable | up to 15 s | 10.82 s |
| Key change followed in 30 s, Fast | at least 90% | **87.5%, FAIL** |
| Key change median, Fast | up to 8 s | **9.11 s, FAIL** |
| Confidence after 10 s silence, Stable | up to 0.10 | 0.023 |
| Reference pitch error, -40 to +45 cents | up to 1 Hz | 0.525 Hz |
| Tempo within 1 BPM, half or double counting | at least 90% | 100% (58.3% at exactly the played tempo) |

Other results:

- Melody only, Single Voice: 98.6% exact, 100% within related keys. Full Mix: 100% and 100%. These tunes land on the tonic often, which real melodies may not.
- Still failing: Fast follows 87.5% of key changes within 30 s, against 90%, with a 9.11 s median against 8. Most misses are a minor loop of i, iv, VII, III, the same chords as vi, ii, V, I in the relative major, which Fast drifts to later. Fast's median moved between 6.2 and 10.3 s from one seed to the next, so one seed says little about it. Looked at again on 2026-11-08, with no change kept:
  - Where the time goes, on dev seeds 1000 to 1005: key changes to a far key (2 or 3 notes shared) settle in 4.5 to 6.4 s; to a close key (5 or 6 notes shared) in 9.6 to 9.9 s, and those hold most of the misses. A close key adds one or two new notes, too few for the 30% outside share, so the old memory is never let go and Fast waits for it to fade.
  - Test chords last 1.5 to 2.5 s, so a loop of four takes about 8 s. Telling a key from the one a fifth away often needs the whole loop, so a median of 8 s sits close to what the music shows.
  - Tried, each for Fast only: hold, dwell, margin, memory and recent memory lengths, outside share, waiting until the new key leads its close neighbours, waiting after a let go, and letting go on one strong outside note. None moved the median reliably: results swing by 0.4 s between neighbouring settings.
  - The best, letting go on one outside note with 6% of the recent notes, followed 93.8% to 95.8% on six unseen dev seeds (1006 to 1011), median 8.3 s both, but on clips with no key change Fast read the right key 98.1% to 95.8% and moved back and forth 82 to 122 times. At 8% that cost went away and so did the gain (94.4%). Not kept.
  - `tests/KeyEval.cpp` now takes `fastonly 1` to skip Stable and `all 1` with `verbose 1` to print every clip's readings.
- Key changes all ended on the right key or its relative.
- A loop heard from its second chord: 65.0% exact, 100% within related keys. With the opening chord gone, some loops can't be told from their relative. This is the main weakness to expect on real songs joined midway.
- Silence from the start: no key and no file. White noise: no key.
- key.json: 13470 writes raced against 115342 reads with no partial file seen. A blocked path shows the note and audio stays bit identical.
- Window created, painted and destroyed 20 times offline, and 30 more times while audio ran, with audio bit identical throughout.
- Every detection reading is bit identical to the saved 1.3.0 fingerprint over 12 clips, in `tests/ReadingFingerprint.h`, so a window change cannot alter detection unnoticed.

## Held out check for the harmony change, written before it was run

Written on 2026-11-08, before seed 20261111 was run. Version under test: the working tree on top of commit 3cd71b8, which adds a harmony reading to Fast only (`hearHarmony` in `src/KeyAnalyser.h`, defaults harmonyFast 1.0, harmonyFit 1.0, harmonyFirst 0.3, harmonyHome 0.2, harmonyRamp 1.0). Stable is unchanged.

Tuned on dev seeds 1000 to 1005 only. Checked once on unseen dev seeds 1012 to 1017: Fast followed 95.8% of key changes (1.3.0: 93.8%), median 5.2 s (8.4 s), Fast chords exact 99.25% (98.07%), bounces 62 (213).

What I expect on 20261111, run once with the same tests as PluginTest:

- Stable: every number identical to 1.3.0 on the same seed, as Stable does not use the new reading.
- Fast key change followed in 30 s: about 92 to 97%, so likely a pass, but one seed is only 24 clips and 1.3.0 fell from 94% on dev to 87.5% on 20261110.
- Fast key change median: about 5 to 6.5 s, a pass.
- Fast chords with no key change: exact about the same as 1.3.0 or up to a point higher, fewer bounces.
- Loop heard from its second chord and melody tests: identical, as they run in Stable.

1.3.0 and the new version will both be run on 20261111 and on 20261110, and the numbers reported whatever they are.
