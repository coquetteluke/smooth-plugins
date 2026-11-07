# Key

Listens to audio and shows the key, scale, tempo and reference pitch, live. Audio passes through bit identical, with no latency. It also writes the key to a shared file so other Smooth plug-ins can read it.

## Load it

Audio Units > Luke Garito > Key. Works on mono and stereo. Best on a mix bus or a beat for chords, or on a vocal with Source set to Single Voice.

## Controls

All are AU parameters, so they can be automated and set by name.

| Control | Values | What it does |
|---|---|---|
| Mode | Listen, Manual | Listen detects. Manual uses Manual Key and Manual Scale, at confidence 100%, and does not analyse |
| Response | Fast, Stable | Stable first reads after about 8 s and never bounces between candidates. Fast first reads after about 3 s and may move more |
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

Full numbers in `tests/results.txt`. Synthetic test music only: it doesn't prove accuracy on real songs or a real voice. Limits are in `tests/PassLimits.h`, set before results. The final numbers use a seed not seen before the final run; two earlier held out seeds were each run once and the detector was then changed using the dev seed only.

Latency 0 at all rates. Output bit identical over 720 blocks, including NaN and huge samples, listening, paused, manual and bypassed. No malloc on the audio thread, checked with the system hook.

| Test, all 24 keys, 5 timbres | Limit | Result |
|---|---|---|
| Chords exact | at least 90% | 95.4% |
| Chords exact, relative or a fifth away | at least 98% | 100% |
| Median time to correct, Stable | up to 8 s | 7.85 s |
| Median time to correct, Fast | up to 5 s | 3.07 s |
| Stable bounces, A to B and back | 0 | 0 |
| Stable clips moving after 8 s | up to 1% | **1.46%, FAIL** |
| Key change followed in 30 s, Stable | at least 90% | **79.2%, FAIL** |
| Key change median, Stable | up to 15 s | 14.06 s |
| Key change followed in 30 s, Fast | at least 90% | **79.2%, FAIL** |
| Key change median, Fast | up to 8 s | **10.30 s, FAIL** |
| Confidence after 10 s silence, Stable | up to 0.10 | 0.023 |
| Reference pitch error, -40 to +45 cents | up to 1 Hz | 0.525 Hz |
| Tempo within 1 BPM, half or double counting | at least 90% | 100% (58.3% at exactly the played tempo) |

Other results:

- Melody only, Single Voice: 95.8% exact, 100% within related keys. Full Mix: 91.7% and 93.8%. These tunes land on the tonic often, which real melodies may not.
- Key changes all ended on the right key or its relative. The misses landed on the relative, which has the same notes.
- A loop heard from its second chord: 66.3% exact, 100% within related keys. With the opening chord gone, some loops can't be told from their relative. This is the main weakness to expect on real songs joined midway.
- Silence from the start: no key and no file. White noise: no key.
- key.json: 13470 writes raced against 115342 reads with no partial file seen. A blocked path shows the note and audio stays bit identical.
- Window created, painted and destroyed 20 times offline, and 30 more times while audio ran, with audio bit identical throughout.
- Every detection reading is bit identical to 1.0.0 over 12 clips, checked by a fingerprint in `tests/ReadingFingerprint.h`.
