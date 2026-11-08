# Changelog

## 1.5.0, 2026-11-08

Stable follows a key change more often and sooner, by reading the chords as Fast has since 1.4.0. Fast is unchanged: every Fast reading is bit identical to 1.4.0. Run on the new held out seed 20261112, after the expected result was committed (7e219ab), and on the two before it:

| Stable | 1.4.0, 20261111 | 1.5.0, 20261111 | 1.4.0, 20261110 | 1.5.0, 20261110 | 1.5.0, 20261112 | Limit |
|---|---|---|---|---|---|---|
| Key change followed in 30 s | 83.3% | **95.8%** | 100% | 100% | 100% | at least 90% |
| Key change median | 10.13 s | 8.26 s | 10.82 s | 8.60 s | 8.43 s | up to 15 s |
| Key change slowest | 24.5 s | 18.3 s | 22.1 s | 19.5 s | 25.0 s | none |
| Key change ended on the right key or its relative | 95.8% | 100% | 100% | 100% | 100% | none |
| Chords exact, no key change | 99.6% | 99.6% | 99.4% | 99.4% | 99.8% | at least 90% |
| Clips moving after 8 s | 0.42% | 0% | 0.83% | 0% | 0% | up to 1% |
| Bounces, A to B and back | 0 | 0 | 0 | 0 | 0 | 0 |
| Chords median time to correct | 7.85 s | 7.85 s | 7.85 s | 7.85 s | 7.85 s | up to 8 s |
| Loop heard from its second chord, exact | 65.0% | 63.8% | 65.0% | 65.6% | 61.9% | none |

Bold now passes. Every test passes on all three seeds. Fast on 20261112: followed 100%, median 5.18 s, chords exact 100%, 13 bounces. 1.4.0 was not run on 20261112, so that seed has no before column.

On dev seeds 1018 to 1023, where it was tuned: Stable followed 94.4% to 97.9%, median 10.8 s to 8.8 s, chords moving after 8 s 0.6% to 0%. On unseen dev seeds 1024 to 1029: followed 91.7% to 97.2%, median 11.2 s to 8.8 s, chords exact 99.3% to 99.6%, moving after 8 s 0.6% to 0.1%, bounces 0 both. The lowest single unseen seed followed 21 of 24, so another seed could still dip under 90%.

- Stable now also reads the chords since the current section began, at 0.7 of Fast's weight. It mostly fixes moves to the relative minor's loop and to a close key, which add too few new notes to let go of the old memory.
- In Stable only, the section's opening chord counts only while the section's chords all fit the key: one chord of four outside the key takes its credit away. Without that, ii V I I loops were read as the key of their ii chord, and Stable went there and back (2 bounces on dev at full weight).
- Tried on dev seeds: without the opening chord rule, full weight gave 2 bounces and 0.6 gave 1; 0.3 to 0.5 followed the same share but slower. With the rule, full weight read 2 chord clips wrong. The rule at 8 instead of 4 gave the same results; at 2, one ii V I I clip moved after 8 s.
- `tests/KeyEval.cpp` takes `stableonly 1`, and runs chords on every seed asked for. New detection fingerprint.

## 1.4.0, 2026-11-08

Fast follows a key change sooner and more often, by reading the chords as well as the notes. Stable is unchanged: every Stable reading is bit identical to 1.3.0. Both versions run on the new held out seed 20261111, which nothing was tuned on, and on 20261110:

| Fast | 1.3.0, 20261111 | 1.4.0, 20261111 | 1.3.0, 20261110 | 1.4.0, 20261110 | Limit |
|---|---|---|---|---|---|
| Key change followed in 30 s | 83.3% | **91.7%** | 87.5% | **100%** | at least 90% |
| Key change median | 7.23 s | 4.50 s | 9.11 s | **6.38 s** | up to 8 s |
| Key change slowest | 15.9 s | 14.2 s | 21.9 s | 17.8 s | none |
| Key change ended on the right key or its relative | 95.8% | 100% | 100% | 100% | none |
| Chords exact, no key change | 98.75% | 100% | 97.50% | 99.17% | none |
| Chords exact, relative or a fifth away | 99.17% | 100% | 98.75% | 99.58% | none |
| Bounces, A to B and back | 32 | 8 | 39 | 19 | none |
| Chords median time to correct | 3.07 s | 3.07 s | 3.07 s | 3.07 s | up to 5 s |

Bold now passes. On dev seeds 1000 to 1005, where it was tuned: followed 94.4% to 96.5%, median 8.1 s to 5.2 s. On unseen dev seeds 1012 to 1017: 93.8% to 95.8%, median 8.4 s to 5.2 s, chords exact 98.07% to 99.25%, bounces 213 to 62.

New fail, not from this change: on seed 20261111 Stable follows 83.3% of key changes within 30 s, against 90%. 1.3.0 gives exactly the same on that seed, as Stable did not change. On 20261110 Stable follows 100%.

- In Fast, the chords since the current section began now count. A section starts at the first run of chord changes not heard lately. For each key: how much of the section's time is on chords outside it, whether the section opened on its home chord, and how long it stays there. A move to a close key adds only one or two new notes, but its chords show it within a chord or two, so Fast no longer waits for its 12 s memory to fade.
- Tried and left out: also crediting the chord a dominant resolves to (made more i iv VII III loops read as the relative major), and taking the chord before the first new change as the section's first.
- Tuned on dev seeds 1000 to 1005 only. New detection fingerprint. `PluginTest --seed N` runs the accuracy tests on another seed; `tests/results-20261110.txt` is the run on 20261110.

## 1.3.0, 2026-11-07

Steadier and more often right. 1.2.0 and 1.3.0 were run on the same new held out seed, 20261110, which nothing was tuned on:

| | 1.2.0 | 1.3.0 | Limit |
|---|---|---|---|
| Stable clips moving after 8 s | 2.50% | **0.83%** | up to 1% |
| Chords exact | 97.5% | 99.4% | at least 90% |
| Key change followed in 30 s, Stable | 95.8% | 100% | at least 90% |
| Key change followed in 30 s, Fast | 87.5% | 87.5% | at least 90% |
| Key change median, Fast | 10.30 s | 9.11 s | up to 8 s |
| Melody only, Full Mix | 95.8% | 100% | none |
| Chords at 44.1 and 96 kHz | 97.9% | 100% | at least 90% |

Bold now passes. Fast still misses its two limits. Most of its misses are a minor loop of i, iv, VII, III, which is the same chords as vi, ii, V, I in the relative major, and Fast drifts to that reading later on.

- A note can now be heard as odd harmonics only, as from a clarinet or a hollow synth. Before, its third harmonic was heard as a fifth above it, which pulled readings to the key a fifth up.
- Notes clearly outside a key's scale now count against that key. The listener profiles barely minded them, and one outside note is often all that tells two keys a fifth apart.
- Stable's first reading waits while the leader is close to another key, its relative included, instead of guessing at 8 s and correcting later. Most clips still read at 7.9 s.
- Tuned on dev seeds 1000 to 1005 only, now with all 12 keys per seed. New detection fingerprint.
- The analysis thread does about four times the note solving work. It runs off the audio thread, so audio is unchanged.

## 1.2.0, 2026-11-07

Better at following a key change. Both versions were run on the same new held out seed, 20261109, which nothing was tuned on:

| | 1.1.1 | 1.2.0 | Limit |
|---|---|---|---|
| Key change followed in 30 s, Stable | 75.0% | **91.7%** | at least 90% |
| Key change median, Stable | 12.0 s | 10.1 s | up to 15 s |
| Key change followed in 30 s, Fast | 62.5% | 83.3% | at least 90% |
| Key change median, Fast | 9.62 s | **6.21 s** | up to 8 s |
| Stable clips moving after 8 s | 1.67% | 2.29% | up to 1% |
| Chords exact | 95.4% | 97.9% | at least 90% |
| Loop heard from its second chord | 67.5% | 66.3% | none |

Bold now passes. Two limits still fail, and one of them got worse: 11 of 480 Stable clips move after 8 s, against 8 before.

- When the notes leave the key, it finds where the new notes began. What was heard since then stays whole, and only what came before is mostly dropped. Before, everything was cut to 15%, the new notes too.
- The chord at that point is heard as the opening of the new section, which helps tell the new key from its relative.
- A chord change not heard lately, after a settled loop, also counts as a new section.
- Right after a key change the reading may correct itself sooner, while the new memory fills.
- A little more trust in the opening chord when there is a bass line. More than this made Stable readings move after 8 s.
- Tuned on dev seeds 1000 to 1002 only. Every detection reading has a new fingerprint, as this changes detection on purpose.
- Test fix: the window test waited a fixed 100 ms for the Manual key and failed most runs, in 1.1.1 too. It now waits up to 2 s.

## 1.1.1, 2026-11-07

Renamed to Key, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

New window, in a light theme: paper white, black type, one red. The key and scale fill most of the window, so they read from across a room, beside a circle of fifths whose ink shows how likely each key looks right now, with how sure it is in the middle. A keyboard shows the notes of the key with the home note in red, and dots for the notes sounding now. Tuning goes red when the music is 1 Hz or more from A 440. In Manual, click the circle or the keyboard, or type a key. Advanced holds Manual Key, Manual Scale, Response and Bypass. The note about key.json failing to save stays, in red in the footer. Resizable, with the size and open section saved in the session. Audio stays bit identical and every detection reading stays bit identical to 1.0.0: the analyser only gained two read-only outputs for the window.

## 1.0.0

First version. Live key, scale and tempo detection with bit identical pass-through and no latency. Analysis on one background thread fed by a lock-free queue. Big readout window. Controls: Mode, Response, Source, Listening, Reset, Manual Key, Manual Scale, Relative Swap, Tempo Scale, Bypass. Readings exposed as read-only parameters: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo. Writes key.json for the other plug-ins. Five factory presets.

Known misses against the limits set before testing: key changes followed within 30 s (79.2% against 90%), Fast median key change time (10.30 s against 8 s), Stable clips moving after 8 s (1.46% against 1%). See README.md.
