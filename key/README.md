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
| Source | Full Mix, Single Voice | Full Mix uses the bass line and chord movement, and reads a lone tune (one note at a time, no chords) as a single voice by itself. Single Voice ignores chords and anything below A2, such as rumble or spill |
| Listening | on, off | Off holds the reading and stops analysing |
| Reset | press | Forgets everything and starts listening again |
| Manual Key | C to B | The key in Manual mode. Automate this to follow planned key changes |
| Manual Scale | Major, Minor | The scale in Manual mode |
| Relative Swap | on, off | Shows and writes the relative key of the one detected, for when it picks C major and you hear A minor |
| Tempo Scale | As Detected, Half, Double | Halves or doubles the tempo shown |
| Bypass | on, off | Audio is unchanged either way. Bypassed, it does not listen |
| Lock | on, off | Holds the key on show at confidence 100% until turned off, through Reset, silence and reloading the session. It goes on listening underneath. Switched on with no key yet, it holds the first reading at 50% or more |

Readings, which ignore writes and never write automation: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo. Each one's text reads back as the value it shows ("None", "C#", "Minor", "78"), and Confidence is a whole percent.

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
- Double-click a control to put it back. A one line hint explains whatever is under the mouse, and follows it as it changes.
- The footer says what key.json holds from this Key and when it was written, such as `key.json: E major, locked, 3 min ago`: what Choir Loft and the tuners follow. It says `not sure` under 50%, which they ignore, `by hand` in Manual, and `changed by another Key` in red when another one wrote over it. Hover it for the folder. If key.json can't be written, a red note says so in its place.
- Resize from the corner. The size and whether Advanced is open are saved with the session. Since 1.7.2 so is the preset last chosen, only to name it (a session from before names none). Nothing else was added to saved state.

The window sets only real parameters, with begin and end gestures, and automation moves it. It reads the processor without locks and never touches the audio thread.

Type is Jost, under the SIL Open Font License in `assets/fonts/OFL.txt`. It is a variable font and JUCE can only use its default weight, so `src/editor/FontOutlines.cpp` gets the letter shapes at any weight from CoreText.

CPU of the open window, real window at Retina size. Nothing else building: 1.9, 2.1, 2.1, 2.2% of one core on real speech, and 1.1, 3.1, 3.3, 4.0% on steady test music (the higher ones landed on slow cores, 0.8% of a fast core's work each time). Silent with nothing drawing, with other builds running: 0.5, 0.6, 0.6, 0.8%. Earlier runs on speech, with other builds pushing it onto slow cores: 2.6, 2.6, 2.9, 2.9, 5.0, 5.9, 6.7%, for the same 2.4 to 3.0% of a fast core's work. Audio CPU did not change: 0.4% before and after.

Render every state to PNG with real speech from macOS `say`: `build/RenderWindow_artefacts/Release/RenderWindow docs/window`. Measure the real window's CPU: `build/RenderWindow_artefacts/Release/RenderWindow --live 5 6 speech`.

## Where to put it

On the beat or the mix bus, not on the vocal. Hearing the music, Key ends on the exact key on 99% of clips, shows it after 7.85 s and is sure and right 83% of the time, and an off key voice on top up to as loud as the music leaves that at 95% or better. Hearing a lone off key voice it needs a verse or more, is not sure (since 1.7.1 never, on Luke's takes), and says Not sure with the runner up named. Play the song through once on the music, press Lock, and every follower has the key for the session. The table is in CHANGELOG.md under 1.7.0; rerun it with `build/MixVoiceEval` (first lines of tests/MixVoiceEval.cpp).

Under the reading the window shows the runner up with its confidence and how close it is, and Use to take it as the manual key. In a close call it says Not sure and names both keys, and the big letters go grey. Lock is on the top row, solid black when on. Pictures: `docs/window/runner-up@2x.png`, `docs/window/not-sure@2x.png`, `docs/window/locked@2x.png`.

Proofs of what a lone voice cannot give: `python3 tests/proofs/ideal_reader.py step` and `floor` (needs `build/IdealNotes`, see its first lines).

## Shared file

`~/Library/Application Support/SmoothPlugins/key.json`, one line:

```
{"key":"E","scale":"major","confidence":0.78,"time":1794011292.5,"listening":true,"altKey":"A","altScale":"major","altConfidence":0.41,"margin":0.62,"unsure":false,"locked":false,"source":"heard"}
```

- `key`: C C# D D# E F F# G G# A A# B. `scale`: major or minor. `confidence`: 0 to 1. `time`: Unix seconds of this write. `listening`: true while detecting, false when paused, manual, bypassed or closed.
- `altKey`, `altScale`: the runner up, or null. `altConfidence`: 0 to 1, same scale as confidence. `margin`: 0 a tie, 1 no contest. `unsure`: two keys fit about equally, and confidence is then under 0.5. `locked`: the key is held by Lock. `source`: heard, manual or locked. The first five fields never move; new ones are only ever added after them. While a key is fixed (locked, set by hand, or frozen with Listening off or Bypass) the file is rewritten every 4 minutes, so a follower added later never finds it older than its 10 minute limit (since 1.7.2; before, only a locked key).
- Written from the background thread only, at most about twice a second, only when something changes (key, scale, listening, or confidence by 0.02). Atomic: a temp file in the same folder, then rename. The folder is made if missing.
- A click on Mode or Lock in the window reaches the file after the double-click time (about half a second), so a double-click, which puts the control back, never writes the step between.
- In an offline bounce or freeze, faster than real time, a new reading is written at once. A follower's own polling still runs on wall time, so for a bounce of a song that changes key, set Lock or Manual first.
- The home folder comes from `getpwuid`, not HOME.
- Nothing is written until there is a key. The format has no empty key, so after Reset the last key stays with confidence 0. Treat low confidence as no key.
- `time` is the last change, not a heartbeat: a steady old time can just mean nothing changed.
- With several instances, the last one to write wins. If a write fails, the plug-in carries on, shows a note and retries every 5 s, also when the key goes back to the line already on disk.
- Closing writes `listening` false into its own last line, keeping that line's time, and only if the file still holds that line: since 1.7.2 a Key that closes never writes over another Key's newer line and never makes an old key look new.
- Writing from inside Logic has not been tested. Only offline tests have written this file.
- For tests: if the environment variable `SMOOTH_KEY_JSON` holds an absolute path, the file is written there instead, so a test run cannot change what a live session follows. Logic never sets it.

## Signal flow

Audio in goes straight to audio out, never written to. A cleaned mono copy goes through a lock-free queue to one background thread, which: decimates to about 12 kHz, runs its own FFT over 0.7 s windows, estimates tuning from spectral peaks, works out which notes explain the spectrum (each note as a stack of harmonics), folds them into treble and bass note memories, and scores all 24 keys. The score uses a listener-rated key profile, a bass profile, time on each key's home chord, dominant chords resolving, and the opening chord as a tie break between relative keys. It also reads the chords since the current section began: which key they belong to, and which chord is home, the one the section opened on and stays on longest. Stable gives that less weight than Fast, and trusts the opening chord only while the section's chords all fit the key. The reading moves only after a new key has led for a set time by a set margin. When recent notes clearly leave the key, old memory is mostly dropped. Tempo comes from a separate curve of spectral rises, matched against steady pulses.

A lone voice is read differently, since 1.6.0. It applies in Single Voice, and in Full Mix once the sound is one line at a time (two pitch classes sounding together through about a second is a chord; a tune never does that for long):

- The grid stays at A440 unless the sound clearly sits on one offset. An instrument recorded flat does. A singer who is off key does not, and following his average moved the note names from phrase to phrase. Reference Pitch then shows 440.
- Only held notes count: the strongest note of a frame, when it stands clear and was the strongest in the frame before too, and it counts in full once it has lasted about half a second. Glides, consonants and rap leave no held notes.
- Held notes are remembered for about two minutes, as a tune takes a verse and a chorus to show its seven notes.
- They are scored against a profile in which every note of the scale counts about the same (Temperley's revision of the listener ratings), and notes outside the scale count against a key. The listener ratings favour tonic, fifth and third so much that a tune sitting on its fifth was read as the key a fifth up.
- When the held notes fit no key's seven notes (rap, speech) there is no reading, or the confidence stays low. Confidence, for a lone voice, is how sure the seven notes are: the key and its relative together.
- A new key must lead by twice the usual margin, and the old key is only let go when about three seconds of held notes in a row sit mostly outside it and make a key of their own.
- Sure, for a lone voice (confidence 0.5 or more, what Choir Loft and the tuners act on), since 1.7.1 also needs the held notes counted as notes, not seconds: each held note once however long, the notes that tell the shown key from every rival must pass a one sided 1% sign test, (a - b) / sqrt (a + b) of 2.33 or more. Two or three long notes no longer make it sure. On Luke's lone voice that test is never passed, so on a vocal track Key informs and says Not sure, and Lock or Manual decides. A key change move needs the same kind of count at 2.0.

After 8 s of silence, or of a host that stops calling, the next audio may be another song: old memory is weakened and the reading may move as early as at the start. Since 1.7.2 every timer keeps the age it had at the pause, so a key change heard minutes before no longer holds the reading still afterwards. A steady offset with no notes (a DC offset) counts as silence. A host Reset (a stop, a locate, the start of a bounce) lets go of the audio not analysed yet; the reading, Lock and key.json stay.

Why not plain profile correlation: a note's own harmonics add false fifths and thirds, and relative keys have the same notes, so correlation alone can't separate them.

## Measured

Full numbers in `tests/results.txt`, and on the previous held out seeds in `tests/results-20261111.txt` and `tests/results-20261110.txt` (`PluginTest --seed N`). Synthetic test music only: it doesn't prove accuracy on real songs or a real voice. Limits are in `tests/PassLimits.h`, set before results. 1.5.0's numbers use a seventh held out seed, 20261112, run once at the end after the expected result was committed. The 1.5.0 change was tuned on dev seeds 1018 to 1023 only and checked on unseen dev seeds 1024 to 1029. Earlier versions are compared in `CHANGELOG.md`.

Latency 0 at all rates. Output bit identical over 720 blocks, including NaN and huge samples, listening, paused, manual and bypassed. No malloc on the audio thread, checked with the system hook.

Every test below passes on all three seeds. Since 1.6.0 one newer test fails, see the lone voice table further down.

| Test, all 24 keys, 5 timbres | Limit | 20261112 | 20261111 | 20261110 |
|---|---|---|---|---|
| Chords exact | at least 90% | 99.8% | 99.6% | 99.4% |
| Chords exact, relative or a fifth away | at least 98% | 100% | 100% | 100% |
| Median time to correct, Stable | up to 8 s | 7.85 s | 7.85 s | 7.85 s |
| Median time to correct, Fast | up to 5 s | 3.07 s | 3.07 s | 3.07 s |
| Stable bounces, A to B and back | 0 | 0 | 0 | 0 |
| Stable clips moving after 8 s | up to 1% | 0% | 0% | 0% |
| Key change followed in 30 s, Stable | at least 90% | 100% | 95.8% | 100% |
| Key change median, Stable | up to 15 s | 8.43 s | 8.26 s | 8.60 s |
| Key change followed in 30 s, Fast | at least 90% | 100% | 91.7% | 100% |
| Key change median, Fast | up to 8 s | 5.18 s | 4.50 s | 6.38 s |
| Confidence after 10 s silence, Stable | up to 0.10 | 0.023 | | |
| Reference pitch error, -40 to +45 cents | up to 1 Hz | 0.525 Hz | | |
| Tempo within 1 BPM, half or double counting | at least 90% | 100% (58.3% at exactly the played tempo) | | |

A lone voice, since 1.6.0. First on Luke's real takes (Rode session, 2026-11-10, each in its song's real key at A440, 9 sung and 2 rap takes of the working set, through the built plug-in). Limits were set before any take was run:

| Real takes, Stable | 1.5.1 Full Mix | 1.6.0 Full Mix | 1.5.1 Single Voice | 1.6.0 Single Voice | Limit |
|---|---|---|---|---|---|
| Sung takes ending on the right 7 notes | 2 of 9 | 6 of 9 | 3 of 9 | 6 of 9 | 80% |
| Share of the singing with the right 7 notes shown | 16.5% | 59.4% | 16.7% | 59.6% | 70% |
| Share two or more notes off | 39.6% | 19.5% | 34.6% | 19.2% | 5% |
| Changes of the reading per minute | 2.85 | 0.65 | 2.16 | 0.58 | 0.5 |
| Median time until the right notes are shown and kept | never | 127.7 s | never | 128.1 s | 20 s |
| Rap takes with at most one change a minute | 1 of 2 | 2 of 2 | 1 of 2 | 2 of 2 | all |

So on a real untrained voice it is right most of the time and steady, but slow: expect the right notes after one to two minutes of singing, not after the first line. The two takes it still ends one note off on are a key and its neighbour a fifth away that his notes do not tell apart. Full report: `~/smooth-plugins/_vox/rode/work/key/REPORT.md`.

Then on a made up off key singer and rap (`tests/KeyCorpus.h`, seed 20261112): ends on the right 7 notes 100% in both sources (limit 90), median 15.5 s (limit 20), clips whose notes change more than once 4.2% (limit 10), Reference Pitch within 2 Hz of 440 in 100% (limit 90), rap clips with at most one change a minute 100% (limit 90), 10 of 12 rap clips never show a key. Fails: when that singer goes up a semitone, the new notes are shown within 30 s in 8.3% of clips, against 80%. That is the price of not letting go of the key when a singer strays for a phrase; `CHANGELOG.md` has the trade in numbers.

Other results:

- Melody only, on 20261112, since 1.6.0: Single Voice 99.3% exact, Full Mix 97.9%, both 100% within related keys, median 9.2 s and 11.6 s. Before 1.6.0: Single Voice 100% exact, Full Mix 100%. On 20261111: 97.9% and 93.8%, 100% within related keys. These tunes land on the tonic often, which real melodies may not.
- Fixed in 1.5.0: on 20261111 Stable followed only 83.3% of key changes within 30 s, against 90%, in 1.3.0 and 1.4.0. Stable now reads the chords too and follows 95.8% there. On unseen dev seeds 1024 to 1029 the lowest single seed still followed 21 of 24 (87.5%), so another seed could still dip under 90%.
- Stable's remaining misses on dev seeds are mostly the minor loop i, iv, VII, III read as its relative major, and one move to the key a fifth up whose loop starts on a chord the old key also had.
- Fast passed both its key change limits for the first time in 1.4.0, on both held out seeds. Its remaining misses are mostly a loop that starts on a chord the old key also led into, so the new section's first chord is not seen as new, and the reading lands on the relative key.
- Before 1.4.0, Fast followed 87.5% of key changes within 30 s, against 90%, with a 9.11 s median against 8, on 20261110. Most misses are a minor loop of i, iv, VII, III, the same chords as vi, ii, V, I in the relative major, which Fast drifts to later. Fast's median moved between 6.2 and 10.3 s from one seed to the next, so one seed says little about it. Looked at on 2026-11-08 before the harmony reading, with no change kept:
  - Where the time goes, on dev seeds 1000 to 1005: key changes to a far key (2 or 3 notes shared) settle in 4.5 to 6.4 s; to a close key (5 or 6 notes shared) in 9.6 to 9.9 s, and those hold most of the misses. A close key adds one or two new notes, too few for the 30% outside share, so the old memory is never let go and Fast waits for it to fade.
  - Test chords last 1.5 to 2.5 s, so a loop of four takes about 8 s. Telling a key from the one a fifth away often needs the whole loop, so a median of 8 s sits close to what the music shows.
  - Tried, each for Fast only: hold, dwell, margin, memory and recent memory lengths, outside share, waiting until the new key leads its close neighbours, waiting after a let go, and letting go on one strong outside note. None moved the median reliably: results swing by 0.4 s between neighbouring settings.
  - The best, letting go on one outside note with 6% of the recent notes, followed 93.8% to 95.8% on six unseen dev seeds (1006 to 1011), median 8.3 s both, but on clips with no key change Fast read the right key 98.1% to 95.8% and moved back and forth 82 to 122 times. At 8% that cost went away and so did the gain (94.4%). Not kept.
  - `tests/KeyEval.cpp` now takes `fastonly 1` to skip Stable and `all 1` with `verbose 1` to print every clip's readings.
- Key changes all ended on the right key or its relative.
- Harmony reading, Fast since 1.4.0 and Stable since 1.5.0: for each key, how much of the current section's time is on chords outside it, whether the section opened on its home chord, and how long it stays there. A section starts at the first run of chord changes not heard lately. Results before and after are in `CHANGELOG.md`.
- A loop heard from its second chord: 61.9% exact on 20261112 (63.8% on 20261111, 1.4.0 65.0%), 100% within related keys. With the opening chord gone, some loops can't be told from their relative. This is the main weakness to expect on real songs joined midway.
- Silence from the start: no key and no file. White noise: no key.
- key.json: 13573 writes raced against 110816 reads with no partial file seen. A blocked path shows the note and audio stays bit identical.
- Window created, painted and destroyed 20 times offline, and 30 more times while audio ran, with audio bit identical throughout.
- Every detection reading is bit identical to the saved 1.6.0 fingerprint over 12 clips, in `tests/ReadingFingerprint.h`, so a window change cannot alter detection unnoticed. Every Full Mix reading over 96 chord clips is bit identical to 1.7.3, so the lone voice reading cannot alter chords unnoticed; with 1.7.3's one change to the reading of music taken out (the top log bin, see the CHANGELOG), they are bit identical to 1.5.1.

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

Result, run once on 2026-11-08 after the above was committed (b4071dd), with the same tests as PluginTest:

| | 1.3.0, 20261111 | new, 20261111 | 1.3.0, 20261110 | new, 20261110 |
|---|---|---|---|---|
| Fast key change followed in 30 s | 83.3% | 91.7% | 87.5% | 100% |
| Fast key change median | 7.23 s | 4.50 s | 9.11 s | 6.38 s |
| Fast chords exact | 98.75% | 100% | 97.50% | 99.17% |
| Fast bounces | 32 | 8 | 39 | 19 |
| Stable key change followed in 30 s | 83.3% | 83.3% | 100% | 100% |

As expected: both Fast limits pass on both seeds, Stable and the melody and loop-from-its-second-chord tests are identical. Not expected: on 20261111 Stable follows only 83.3% of key changes, against 90%, in both versions. That is a fail the seed shows, not one this change made. Shipped as 1.4.0.

## Held out check for harmony in Stable, written before it was run

Written on 2026-11-08, before seed 20261112 was run. Version under test: this commit, which turns the harmony reading on in Stable too (`harmonyStable` 0.7, Fast keeps 1.0) and, in Stable only, counts the section's opening chord only while all its chords fit the key (`harmonyFirstFitStable` 4: one chord of four outside the key takes the opening chord's credit away). Without that second part, ii V I I loops were read as the key of their ii chord and Stable went there and back. Fast's code path is unchanged, so every Fast reading should be bit identical to 1.4.0.

Tuned on new dev seeds 1018 to 1023 only. Checked once on unseen dev seeds 1024 to 1029, 1.4.0 against this version: Stable key change followed 91.7% to 97.2%, median 11.2 s to 8.8 s; Stable chords exact 99.3% to 99.6%, moving after 8 s 0.6% to 0.1%, bounces 0 and 0 (every second tonic, 1440 clips). The lowest single unseen seed still followed only 21 of 24 (87.5%), so one seed can still fail.

Also run before this, on seeds already used: 20261111 now passes everything (Stable followed 83.3% to 95.8%, median 10.13 s to 8.26 s, moving after 8 s 0.42% to 0%). 20261110 passes everything (Stable 100% both, median 10.82 s to 8.60 s, moving after 8 s 0.83% to 0%). Fast identical on both.

What I expect on 20261112, run once with PluginTest:

- Stable key change followed in 30 s: about 92 to 100%, likely a pass, with roughly a 1 in 10 chance of 21 of 24 or fewer, a fail.
- Stable key change median: about 8 to 10 s, a pass (limit 15).
- Stable chords: exact about 99 to 100%, moving after 8 s 0 to 0.4%, bounces 0. Median time to correct about 7.85 s, a pass (limit 8).
- Fast: the same as 1.4.0 would give on this seed. Fast has never been run on 20261112, so its key change limits (90%, 8 s) could fail on this seed alone; on dev it followed 95.8% to 97.2%.
- Melody and loop from its second chord: melody unchanged (it has no bass line, so no harmony reading), loop from its second chord within a couple of points of 1.4.0.

The result will be reported whatever it is.

Result, run once on 2026-11-08 after the above was committed (7e219ab), with PluginTest: every test passes. Stable key change followed 100% within 30 s, median 8.43 s. Fast 100%, median 5.18 s. Chords exact 99.8%, relative or a fifth away 100%, Stable median time to correct 7.85 s, moving after 8 s 0%, bounces 0. Fast chords exact 100%, 13 bounces. Melody 100% in both sources. Loop from its second chord 61.9% exact, 100% related (1.4.0 was not run on this seed). As expected, except that the Stable key changes landed at the top of the range. Shipped as 1.5.0.
