# Punch

Makes a vocal hit harder and sit forward. Shapes the start and the tail of a sound, adds fast compression, evens out level and raises it, and holds the result under an output ceiling. Attack and Sustain follow the shape of the sound, not how loud it is, so there is no threshold to set. All the code is written from scratch.

## How to load it

In your host: Audio Units > Luke Garito > Punch. AU code `aufx SPun Luke`. Mono and stereo. Latency is 2 ms: 88 samples at 44.1 kHz, 96 at 48 kHz, 192 at 96 kHz, and the same when bypassed.

## Controls

- **Punch** (0 to 100 %, default 0): amount of fast compression, level matched. The threshold follows the loudest recent level, so it does the same job on a quiet track as on a loud one. Up to 36 dB below that peak at 10 to 1 at the top of the control.
- **Attack** (-15 to +15 dB, default 0): boosts or cuts the start of a sound. Full effect for about the first 5 ms, gone by about 30 ms.
- **Sustain** (-15 to +15 dB, default 0): boosts or cuts the tail, as a sound dies away. Starts after about 15 ms. A steady sound has no tail, so it is left alone.
- **Steady Notes** (default off): Sustain and Punch for a voice. Off, both are exactly what they were. With Punch, Attack and Sustain at 0 it changes nothing at all. A session saved before Steady Notes existed (0.3.x) loads with it off, and a session saved since keeps the value it was saved with. (0.6.0 turned it on in a new instance; 0.6.1 put that back, see the CHANGELOG.) On:
  - **Sustain follows the note.** A note is a sound with a pitch. While it has its pitch it may sag by 6 dB before Sustain acts, measured from the highest it stood in the last 100 ms, so a held vowel stays still, also one that starts loud and settles lower, and only a note that is really dying away is shaped. The tail is what comes right after the pitch stops: a boost stays on it for about 70 ms and is gone by about 105 ms, a cut stays for about 110 ms and is gone by 130 ms. A breath, a mouth sound, the room or the start of the next phrase after that is not the word's tail and is left alone. A boost also leaves bright sounds such as an S alone. A sound right after a voice with no note of its own gets nothing from Sustain. Where there is no voice (no note for a second, or none at all, as on drums) Sustain works as with Steady Notes off. A pitch heard after such a rest takes that out over 20 ms, so the first vowel of a phrase is not stepped. A sharp start there counts as a new sound, which is what keeps a phrase start after a breath or a pause still, except within 110 ms of the end of a louder sound's peak: a soft hit right after a loud one on a drum track gets the loud one's tail, as with Steady Notes off (since 0.6.3; 0.6.2 gave it none).
  - **Punch reads a held note by its level, not by every peak**, and its level match is learned on the voice and then held, so a held note keeps one gain from the first word of a phrase on. The match is given to a sound by how near it is to the level Punch works at, so the room and a far-off breath are not lifted with the voice.
  - **Attack leaves a held note alone.** Inside a note that is sounding, the peaks jump by a few dB now and then while the level stays put. A start there has to rise 4 dB before Attack acts. The first sound after a pause is shaped exactly as before.
  - With every other control at its default it changes nothing at all.
- **Mix** (0 to 100 %, default 100): blend with the dry signal, which is delayed by the same 2 ms.
- **Bypass**: fades to the input, delayed by the same 2 ms. Takes Gain and Ceiling out too. The fade goes between the finished sound and the input, so on the way it never goes over the ceiling or over the bypassed level.
- **Gain** (-12 to +24 dB, default 0): level in front of everything, for quiet takes. It moves dry and wet alike, so Mix 0 still carries it. Attack, Sustain and Punch ignore level, so it does not change them. It does drive Impact harder. Moved while playing, it is taken as the new level from the start: Attack, Sustain and Punch do not react to the move, and Impact goes straight to the new level's reduction.
- **Impact** (0 to 100 %, default 0): the one control for evening out a performance and raising it. Slower than Punch, so it rides the level of phrases instead of changing the shape of each sound. More Impact means a lower threshold (-6 to -36 dBFS), a higher ratio (1 to 1 up to 6 to 1) and more makeup (up to 24 dB). It reacts in about 5 ms. It lets go in about 90 ms after a short peak and about 500 ms after a level that lasted. Its makeup fades out below about -50 dBFS and is gone at -70 dBFS, so near silence isn't lifted.
- **Ceiling** (-24 to 0 dBFS, default -1): the most the output can reach while Ceiling On is on. Last in the chain, after Mix.
- **Ceiling On** (default off): off keeps the first version's behaviour, with no limit on the output. All the new presets turn it on. Impact raises the level, so turn this on when you use Impact.

Read only, for a host to read by name. They never write automation and are never saved:

- **Input Level**: peak before Gain, -60 to +24 dBFS.
- **Output Level**: peak after everything, -60 to +24 dBFS.
- **Impact Reduction**: how far Impact is pulling down, before its makeup, in dB.
- **Ceiling Reduction**: how far the ceiling is pulling down, in dB.

Not on the panel, fixed by design: 15 ms peak hold for the shaper and 8 ms for Punch, which is what keeps steady low tones clean. Punch releases in about 8 ms after its hold. Both channels share one gain, so the stereo image stays put. Steady Notes hears pitch the way a pitch tracker does: every 5 ms, the last 20 ms against the same 20 ms one period earlier, for periods from 1100 Hz down to 60 Hz. A note has to keep its pitch for 80 ms to count, and is over when the pitch stops or the level has sunk 24 dB under the note's own.

## The window

The window is called Punch, and is the name this plug-in will carry after the rename. The build still installs as Punch.

- **The big picture is the last hit**, 400 ms of it. White is the sound as it came in. Orange is how it goes out. Dark orange under the white line is what the settings took off. Orange above the white line is what they added.
- The time scale is stretched at the start, because that is where a hit happens: the first 30 ms take close to half the width.
- **Attack sits under the start of the hit and Sustain under its tail.** Hovering over either one lights the part of the hit it works on.
- **Across the top, the last few seconds** as a row of bars. The bright bars are the hit shown below.
- **On the right, In and Out** on the same level scale as the picture, then **how far Impact and the Ceiling are pulling down**, hanging from the top, with large numbers under them.
- **The Ceiling is also a line on the picture** while it is on. Drag it, or double-click it to put it back.
- Before anything has played, the picture is an example hit, and says so. The controls still move it. When playback stops, moving a control redraws the last hit with the new settings, and says so. Both run the sound level through the real detector at the display's rate, so they are a close drawing, not the exact result. Against the real result on bursts, the average error is 0.1 dB for Attack or Sustain alone and 1.7 dB with everything on.
- Gain and Mix are under Advanced. When it is closed, the row says if either has moved.
- Double-click anything to reset it. Click a value to type one. The window can be resized, and its size is saved.

Every control is still a real AU parameter with the same ID, name and range, so automation reaches them all, and the window moves with them. A hit is the level at least doubling against 20 to 32 ms earlier, above about -45 dBFS, and no sooner than 350 ms after the last one, so one can be read before the next arrives. The drawn levels are smoothed by a few milliseconds to take out the ripple of a voice's own cycles. A cut at the very start of a hit reads about 2.6 dB short because of that: Attack -9 draws as -6.4.

## Presets

Punch, Attack, Sustain, Mix:

- **Neutral**: 0, 0, 0, 100.
- **Vocal Forward**: 30, +2.5, 0, 100.
- **Soft Consonants**: 20, -5, +1.5, 100.
- **Dry And Close**: 10, +1.5, -7, 100.
- **Longer Tails**: 15, 0, +6, 100.
- **Drum Snap**: 25, +8, -3, 100.
- **Parallel Weight**: 85, 0, +3, 40.

These add Gain, Impact and Ceiling, with Ceiling On:

- **Even Lead**: 0, +1.5, 0, 100. Gain 0, Impact 40, Ceiling -1.
- **Rap Upfront**: 20, +3, -2, 100. Gain 0, Impact 55, Ceiling -1.
- **Quiet Take Lift**: 0, 0, 0, 100. Gain +12, Impact 35, Ceiling -3.
- **Spoken Word**: 0, 0, -3, 100. Gain 0, Impact 45, Ceiling -3.
- **Full Level Hook**: 15, +2, 0, 100. Gain 0, Impact 70, Ceiling -0.5.
- **Parallel Impact**: 0, 0, 0, 50. Gain 0, Impact 90, Ceiling -1.
- **Ceiling Only**: 0, 0, 0, 100. Gain 0, Impact 0, Ceiling -1.

For a lead vocal, added in 0.5.0, all with Steady Notes on and, since 0.5.1, all with Ceiling On at -1 dBFS (Punch, Attack, Sustain, Mix, then what else is set):

- **Lead Gentle**: 12, +1.5, 0, 100. Ceiling -1, Ceiling On. A little edge on each word, nothing else moved.
- **Lead Upfront**: 20, +2.5, 0, 100. Impact 35, Ceiling -1, Ceiling On. Forward and evened out. Raises the level by about 8 dB.
- **Lead Rap**: 15, +2.5, -2, 100. Impact 50, Ceiling -1, Ceiling On. Tight and loud. Raises the level by 10 to 12 dB.
- **Lead Soft Consonants**: 10, -5, +1.5, 100. Ceiling -1, Ceiling On. Softens hard starts.
- **Lead Long Tails**: 10, 0, +6, 100. Ceiling -1, Ceiling On. Lifts the end of each word, not the held note and not the breath after it.
- **Lead Dry And Close**: 10, +1.5, -6, 100. Ceiling -1, Ceiling On. Shortens the end of each word.

Steady Notes is off in the 14 presets above these, exactly as before, and their numbers in the list have not changed. Turn it on by hand with any of them that uses Sustain or Punch.

The preset you chose is saved with the session (since 0.6.2), so a session reopens naming it, and the arrows step from it. A session saved before 0.6.2 does not say which preset it was: it shows "Choose a preset", sounds exactly as it was saved, and the arrows go to the first or the last preset. Choosing a preset while playing goes straight to its sound, with no burst or dip on the way.

### Which preset on a lead vocal

**Load Lead Gentle.** It is the lightest of the six lead presets, and all six meet every limit of the real vocal checks on every one of Luke's working set takes, sung and rap: no level change, held notes left still, S, breaths and mouth sounds not raised. If the take also needs evening out and raising, use Lead Upfront (or Lead Rap for rap) and turn the next plug-in's input down by what it adds.

Two things the lead presets did on a take that is already hot. Found after 0.5.0 shipped, on the first session's 11 takes, which peak at 0.0 dBFS (the Rode takes they were tuned on peak at -8 to -18 dBFS):

- Fixed in 0.5.1. Lead Gentle and Lead Dry And Close had no ceiling, and Attack +1.5 raises a peak. On 3 of those 11 takes (06, 08, 10) the output went up to 0.8 and 1.0 dB over 0 dBFS. All six lead presets now have Ceiling On at -1 dBFS. On the same three takes the output now peaks at -1.5 to -1.8 dBFS, with no sample over 0 dBFS. On the Rode takes they peak 6 dB or more under the ceiling, so it does nothing there.
- Lead Upfront and Lead Rap use Impact, which works from a fixed level. On those hot takes it pushed the vowels down harder, so S came up against them by up to 3.6 and 5.9 dB (limit 2) on 7 of the 11, and consonant peaks by up to 4.0 and 4.7 dB on 5. Not tried yet: turning Gain down first so the peaks sit near -8 dBFS, where these presets were tuned. A de-esser after Punch would also catch it.
- Lead Soft Consonants and Lead Long Tails met every limit on all 11 of those takes too.

How the 14 older presets did on the same takes (Rode session, 11 working set takes, numbers from 0.3.1, which is how they still sound):

| preset | for | what the takes showed |
|---|---|---|
| Neutral | anything | changes nothing: the output is the input 2 ms later |
| Even Lead | a lead vocal | meets every limit on all 11 takes; raises the level by 9 to 10 dB |
| Rap Upfront | a lead vocal, rap or sung | meets every limit on 9 of 11 takes (S +2.2 dB on one, held swing 1.7 dB on one); raises the level by 10 to 12 dB |
| Ceiling Only | a safety net | does nothing until a peak reaches -1 dBFS |
| Vocal Forward | a voice that has no de-esser after it | S comes up 2.2 to 3.5 dB against the vowels on all 11 takes; nothing else fails |
| Soft Consonants | a voice, nearly | S +2.0 to +2.2 dB on 4 takes, held notes wobble on 2. Lead Soft Consonants is the version that passes |
| Dry And Close | drums and short sounds | held notes swing 5.9 to 6.8 dB on every take. Lead Dry And Close is the version for a voice |
| Longer Tails | drums and short sounds | held notes swing 3.4 to 5.7 dB and S comes up 2.7 to 4.3 dB on every take. Lead Long Tails is the version for a voice |
| Spoken Word | speech with few held notes | held notes swing 2.5 to 3.0 dB on every take |
| Drum Snap | drums | raises peaks by up to 6.4 dB |
| Parallel Weight | parallel use under a dry copy | breaths come up 3.5 to 11.1 dB and S 3.3 to 6.7 dB against the voice |
| Parallel Impact | parallel use under a dry copy | breaths come up 6.2 to 8.6 dB on 6 takes |
| Full Level Hook | a loud hook with a de-esser after it | S comes up 2.1 to 4.5 dB, consonant peaks 3.2 to 4.5 dB |
| Quiet Take Lift | a quiet take only | adds 14 to 18 dB; the room comes up to about -39 dBFS |

## On Luke's real takes

Measured with `~/smooth-plugins/_shared/realvox` on the Rode session (17 takes of Luke singing and rapping whole songs; 11 are the working set, 6 were kept blind). Every limit was written before the run it was first used in. Full numbers, the blind set and the first session: `~/smooth-plugins/_vox/rode/work/punch/REPORT.md`.

On the working set, 0.5.1, with Steady Notes on (limit in brackets):

- Sustain +9 and -9: the 20 to 150 ms after a phrase ends +5.6 to +7.8 and -6.3 to -9.0 dB (at least 4.5 of the 9). Held vowels 0.00 dB (1.0 at the 95th percentile, 0.2 on average). First 15 ms of phrase starts 0.00 dB (3.0 at the 90th percentile).
- Sustain +6: breaths against the voice -0.1 to +1.3 dB (+3), mouth sounds -0.2 to 0.0 dB (+3), S -0.2 to 0.0 dB (+2).
- Punch 50 and 100: level swing added to held vowels at most 0.56 and 0.64 dB (1.5). Without Steady Notes 1.68 and 4.10. Level against the dry within 0.21 and 0.57 dB (1.0).
- Attack +9 and -9: phrase starts 8.85 to 9.00 dB, held vowels 0.00 dB (1.0). Without Steady Notes 1.88 on one take.
- All six lead presets meet every limit on all 11 takes.
- 0.5.1 fixed the one Steady Notes miss of the blind set: on take 15, Sustain +9 gave held vowels +0.25 dB on average (limit 0.2). A held note that starts loud and settles lower got a short burst of Sustain when it dipped a little: on working take 04 at 149.86 s it was 7.6 dB for 30 ms. Now 0.00. It also fixed Punch 50 with Steady Notes settling 1.75 dB low on the first session's take 01 (limit 1.0): now 0.20 dB.

With Steady Notes off every one of these numbers is what it was in 0.3.1 and 0.4.0: Sustain moves held vowels by 7.2 to 9.0 dB, and so on. That is how Sustain has always sounded, and the 14 older presets still sound that way.

## Signal flow

```
input -> Gain -+-> louder of the two channels -> peak hold -+-> start detector (fast against slow rise) --> Attack --+
               |                                            +-> tail detector (fast against slow fall) ---> Sustain -+
               |                                            +-> Punch: threshold from recent peak, level match ------+
               |                                            +-> Impact: fixed threshold, two speed release, makeup --+
               |                                                                                                     |
               |                                                                              sum, in dB, smoothed <-+
               |                                                                                     |
               +-> 2 ms delay ----------------------------------------------------------------> Mix and Bypass set how much of this gain is used
                                                                                                     |
                                                                                                     +-> Ceiling -> output
```

There is one delayed copy of the input and one gain. Dry and wet are the same samples, so Mix can't comb filter.

Attack and Sustain compare envelopes with each other as ratios. Every envelope scales with the input, so the ratios do not change with level.

The ceiling adds no latency. The sample going into the 2 ms delay comes out 2 ms later, so its level at the current gain is the look ahead. A final exact step catches what the look ahead can't see, such as a lone spike at an onset while Attack's boost is still arriving. A signal under the ceiling passes untouched.

## What was measured

All offline, in `tests/PluginTest.cpp`, with results in `tests/results.txt`. Pass limits were written before the first run. All tests use synthetic signals. **This does not prove how it sounds on a real voice.** For that see "On Luke's real takes" above.

Decaying burst: instant onset, 100 ms decay time. Start means the first 3 ms, tail means 60 to 300 ms after the onset.

- Attack +9 and -9 dB on a 1 kHz burst: start +8.97 and -8.97 dB, tail 0.00 dB.
- Sustain +9 and -9 dB on a 1 kHz burst: tail +8.96 and -8.96 dB, start 0.00 dB.
- The same on 200 Hz, 4 kHz and noise bursts: within 0.03 dB of those.
- Level independence at -6, -18, -30 and -42 dBFS: 0.000 dB spread for Attack and Sustain, start and tail.
- 44.1, 48 and 96 kHz: 0.002 dB spread or less.
- Steady 60 Hz, 100 Hz, 1 kHz and 9 kHz with Attack and Sustain at maximum: nulls fully against the input.
- Punch 25, 50, 75, 100 on a 300 Hz burst train (12.1 dB crest factor): crest factor down 1.49, 3.34, 4.68, 5.35 dB. RMS level change 0.03 dB or less.
- The same on a noise burst train (14.0 dB crest factor): down 1.43, 3.21, 4.45, 5.02 dB. RMS level change 0.03 dB or less.
- Punch 100 on a steady 100 Hz tone: level change -0.02 dB, everything other than level at -113 dB.
- Punch 50 across 36 dB of input level: crest reduction spread 0.001 dB.
- Mix 50 against the average of Mix 0 and Mix 100: difference -144 dB. Broadband response at Mix 50 spreads 1.2 dB. A control with a dry path misaligned by 20 samples reads 8.3 dB, so the measurement can see a comb.
- Mix 0 with every control on, and neutral settings at Mix 100 and 50: nulls fully against the delayed input.
- Impulse delay equals the reported latency at 44.1, 48, 88.2 and 96 kHz. Bypass nulls against the delayed input.
- Any block size from 1 to 4096 gives the same output, and preparing again gives a clean restart.
- Damaged saved state (NaN, infinity, huge, negative, garbage, cut off): controls stay in range and output stays finite. A dropped value goes back to its default. Those tests fail when the guard is taken out.
- Bad input samples (NaN, infinity, huge values): replaced or clamped on the way in, and the plug-in recovers fully afterwards.

Added in 0.2.0, with limits written before the first run:

- Same as 0.1.0: all seven first presets, and a state saved by 0.1.0 with none of the new values, match numbers recorded from the 0.1.0 code before any change. Largest difference in any 10 ms: 0.000 dB.
- Gain -12, +6, +12, +24 at neutral: level moves by exactly that, to 0.000 dB. Attack and Sustain with Gain +12 move 0.000 dB against no Gain.
- Gain +18 into Impact 50 on a -30 dBFS tone: 11.6 dB more out, so it drives the compressor.
- Impact 25, 50, 75, 100 on a 24 dB gap between loud and quiet stretches: gap shrinks by 3.8, 9.8, 16.1, 18.3 dB.
- Impact 25, 50, 100 on a -24 dBFS tone: level rises 6.0, 12.0, 14.0 dB. On -40 dBFS at 100: 24.0 dB. On -6 dBFS at 100: -1.0 dB.
- Impact 50 and 100 on a speech shaped test signal (18.1 dB level spread): spread shrinks by 5.5 and 9.6 dB. This is a synthetic signal, not a voice.
- Impact 100 on steady 65 Hz, 100 Hz and 1 kHz: the gain is flat, nothing but level changes.
- Impact 100, jump from -30 to -6 dBFS: gain within 0.1 dB of settled 25 ms later.
- Impact 100 recovery: 184 ms after a 50 ms peak, 948 ms after a 1.5 s hold.
- Impact 100 on noise at -80 dBFS: lifted 0.0 dB.
- Ceiling -1, -6, -12, -24 dBFS on hot hits with all boosts on, a lone onset spike, and at 44.1 and 96 kHz: at most 0.000 dB over. Sample peaks, not true peaks.
- Ceiling -6 on a -12 dBFS tone: nulls against the input. Steady 60 Hz, 100 Hz, 1 kHz, 6 dB over a -12 ceiling: lands on the ceiling, gain flat.
- Ceiling hard onset: largest gain move in one sample 0.002 dB, or 0.36 dB with Attack +12. Back within 1 dB 165 ms after a hit.
- Those ceiling tests were checked by breaking the code: without the look ahead five checks fail, and without the exact last step the onset spike goes 12.8 dB over.
- Latency with Gain, Impact and Ceiling on: 88, 96 and 192 samples at 44.1, 48 and 96 kHz, the same as before.
- Readings, read through their value strings: Input Level -12.0 and Output Level -6.0 with Gain +6 on -12 dBFS, Impact Reduction -10.7 at Impact 50 on -6 dBFS, Ceiling Reduction -6.0 at Ceiling -12 on -6 dBFS. Zero automation writes during audio.
- Every preset from Even Lead on (the seven of 0.2.0 and, since 0.5.1, the six lead presets): Ceiling On, under its ceiling on hot material, and raises a -30 dBFS tone or leaves it as it was (-0.02 to +20.3 dB, limit -0.1). With 0.5.0's Steady Notes level match the four lead presets without Impact took the tone down 0.14 to 0.19 dB.

Added in 0.3.0, with limits written before the first run:

- Audio unchanged: real speech from macOS `say`, two voices under four settings, through the installed AU with `tests/AuRender.cpp` and `tests/null/render.sh`. All eight results are byte for byte the same as 0.2.0, with hashes in `tests/null/`.
- Window open, ticking and painting against window closed, on real speech with everything on: difference exactly 0.
- Before and after on the display, at neutral: the same in every bin, so they line up in time.
- The drawn shape on a burst: Attack +9 draws +9.0 at the start and 0.0 in the tail. Sustain -9 draws -9.0 in the tail and 0.0 at the start.
- Hits: 5 bursts make 5 hits at -30 dBFS, a steady tone makes 1, silence and a -60 dBFS tone make none, real speech makes about 2 a second.
- 60 windows opened, painted and closed, 30 of them with audio running on another thread. One opens and paints before the host has prepared the plug-in. Frames and paints write no parameter and no automation. Ten seconds of silence from the host moves nothing.
- Audio CPU through `_shared/bin/AuBench`: 0.6 to 0.7% default before and after. With Steady Notes on, the pitch sense adds to it: over Luke's takes the worst block of a lead preset took 2.2 to 3.3% of one core, against 0.4 to 1.1% at the default up to 0.5.1. Window CPU: see `docs/window-cpu.md`, measured with `build/RenderWindow_artefacts/Release/RenderWindow --live`.
- The pictures in `docs/window/` come from `RenderWindow`, with real speech going through the real processor. None of this has been opened in a real host window yet.

Steady Notes on a made-up sung line (0.5.0): a held note at 196 Hz with vibrato and a 3 dB swing, the breathy sound left after it, a breath a quarter of a second later, a mouth sound, a quieter second note that drops 5 dB half way (two syllables on one breath), a breath straight after it, and a room that hisses and hums at 120 Hz under everything.

- Sustain +9 and -9: the held note moves 0.00 dB. The 20 to 150 ms after the voice stops move +7.8 and -5.6 dB. The first 15 ms of the next note, the breath a quarter of a second later and the room on its own move 0.00 dB.
- Sustain +6 and -6: the quieter syllable on the same breath moves 0.00 dB. At +6 the breath straight after the note moves +0.1 dB over its whole length and the mouth sound 0.00 dB.
- The same at 44.1 and 96 kHz: held note 0.00 dB, after the voice stops at least +4.5 dB.
- The pitch sense finds a voice of 82 to 880 Hz within 10 to 30 ms and loses it 25 to 30 ms after it ends. White noise: a pitch 0 % of the time.
- Punch 50 and 100 on a held note whose peaks move while its level does not: 0.2 and 0.3 dB of level swing added with Steady Notes, 1.3 and 1.5 dB without. Level within 0.4 dB of the input.
- A held note that starts loud, settles 10 dB lower and keeps a 3 dB swing (0.5.1): at Sustain +9, -9 and +6, and +9 at 44.1 and 96 kHz, the settled note moves at most 0.02 dB in any 10 ms and 0.00 dB on the whole (limits 1.0 and 0.2). 0.5.0 moved it by up to 5.8 dB.
- Attack +9 and -9 on a held note whose peaks jump: the peaks jump 2.8 dB while the level stays put. Around the jump the level moves 0.0 dB with Steady Notes, 2.5 and 2.4 dB without. The first 3 ms of the note still move 9.0 dB.
- Steady Notes off: the detector gives 0.4.0's gain sample for sample at all 14 older presets and at both extremes, on speech and bursts (a copy of 0.4.0's detector is kept in `tests/PunchEngine040.h` for this).
- Steady Notes and saved state (0.6.0, kept in 0.6.1 with the default back to off): with every other control at its default, Steady Notes on and off give the same output sample for sample on two real voices and the sung line (0 samples differ). A session saved before 0.4.0 loads with it off, into a new instance and into one that had it on; sessions saved with it off and on load off and on; a damaged value goes back to the default. State saved by 0.1.0 with the Vocal Forward values still sounds as it did (0.000 dB).
- Steady Notes on, noise bursts with no pitch (0.6.1), Sustain +9 and -9: every tail moves as with Steady Notes off, within 0.000 dB (limit 1.0), from the first burst, and after a sung note for every burst 2 s and more after it. 0.6.0 and earlier left them untouched (9.0 dB off).
- All 20 preset names fit in the preset box with ", edited" after them: the widest is 198 points, the room is 240.

Added in 0.6.2, with limits written before the first run (results in `tests/results.txt`, `PluginTest phase2` runs only these):

- Moving a control while playing: a held sung note, and one with 1.2 s of room in the middle. Each control that feeds the gain (Punch 50 and 0, 50 and 85, 50 and 51, Attack, Sustain, Mix, Gain 0 and +12, Impact 40 and 0, 40 and 90, Ceiling, Ceiling On, Bypass, Steady Notes) moved both ways, with Steady Notes on and off, in the held note and in the pause, against renders that had the one setting or the other from the start: at most 0.35 dB past either setting's own level (limit 1.0; 0.6.1 up to 24.4 dB), and from 0.5 s after the move within 0.51 dB of the new setting (limit 1.0; 0.6.1 up to 25.0 dB). Eight preset changes while playing, among them Parallel Weight to Neutral and Lead Gentle to Lead Upfront: at most 0.63 dB (0.6.1 up to 25.3 dB).
- Steady Notes on, Punch 50 nudged to 51 on a held note: within 0.14 dB of the input (limit 1.0; 0.6.1 7.6 dB low).
- Steady Notes, Sustain +9 and -9, the first note after 1.5 s of silence: the gain moves 0.000 dB within any 1 ms from 40 to 300 ms (limit 1.0; 0.6.1 7.8 dB).
- Steady Notes on with no pitch, a soft noise hit 80 ms after a loud one: within 0.000 dB of Steady Notes off at Sustain +9 and -9 (limit 1.0; 0.6.2 3.5 and 5.7 dB). A soft start 200 ms after a loud one, once the level is down to the room, with the voice rising out of it 25 ms later (rap take 16 at 1.28 s, made up): the first 15 ms from the rise move 0.001 dB (limit 1.0), where Steady Notes off moves them 9 dB.
- Gain +12 into Ceiling -1, Bypass switched during a tone: the output never over the ceiling or the input, 0.000 dB (0.6.1 0.5 and 1.8 dB over).
- The default and the six lead presets against states and renders made by the installed 0.6.1 (`tests/baseline`): the default exactly the same, Lead Gentle and Lead Upfront to float rounding, the four that use Sustain changed only by the Sustain fix at the first note after a rest (with it taken out, all seven null bit for bit).
- The host's Reset: play, reset, play gives what a new instance gives; it allocates nothing; the one Reset that comes with the host's bypass switching on changes nothing.

Limits to know about:

- Sustain follows how fast a sound dies away. +9 dB gives 9.0 dB on a 30 ms decay, 7.5 on 300 ms and 2.6 on 1 second. Slower decays read as steady sounds.
- Without Steady Notes, Sustain moves a held sung note: one swinging 3 dB at 5 Hz moves by 7.3 dB at Sustain +9. With it, 0.0 dB, and what follows the note still gets its gain.
- Steady Notes hears a note by its pitch. Where there is no voice, Sustain is what it is with Steady Notes off, except that a sharp start more than 110 ms after the end of a louder sound's peak counts as a new sound (0.6.3; up to 0.6.2 every sharp start did, so a soft hit right after a loud one got no tail): on a track with no pitch at all, such as drums, from the start, and after a voice once no note has been heard for 1 s (coming in fully by 1.5 s). Up to 0.6.0 it did nothing at all to a sound with no pitch. Before the first note of a vocal take there has been no note either, so a breath or a mouth sound there gets Sustain as with Steady Notes off (on the working set, Sustain +6: breaths at most 1.3 dB, mouth sounds at most 0.8 dB against the voice, limit 3). A room's hum has a pitch too, and is kept from counting as a note by its level: a note can only start within 6 dB of where the level is, and is over once it has sunk 24 dB under its own level.
- With Steady Notes on, the very first phrase after the plug-in starts is where Punch's level match is learned, at the old speed. From the second phrase on a held note keeps one gain. On a perfectly steady held note that first match can lock about 2 dB low and is trimmed back with a 3 s time (2.0 dB low after 3 s, 1.2 dB after 5 s).
- With Steady Notes on, a boost from Sustain lets go about 105 ms after the pitch stops and a cut about 130 ms after. A long reverb tail after a note is shaped for that long and no longer.
- The picture of the last hit is drawn from levels, and a level has no pitch. With Steady Notes on, a preview treats the hit's body as the note until it has sagged 6 dB, and draws the tail after that the way a voice gets it (a cut for about 130 ms, a boost for about 105 ms). It cannot show a breath or an S being spared. Up to 0.5.1 it drew no Sustain at all with Steady Notes on.
- Level match needs a moment. At Punch 50, the first hit after a long silence is 2.7 dB quieter than the input, the second 0.6 dB, the fifth 0.0 dB.
- At high Punch, the level match lifts quiet tails a lot, including any noise in them.
- Boosting Attack raises peaks. With Ceiling On off there is no limit on the output, so watch the level after it.
- The ceiling works on sample peaks, so a peak between samples can land slightly over it. Leave a little room if the next stage is a converter or a lossy encoder.
- Impact is a fixed threshold, so unlike the rest of the plug-in it depends on input level. A very quiet take needs Gain first.
- Impact's makeup lifts anything above about -50 dBFS, including room noise and breaths, by up to 24 dB at the top of the control.
- A lone spike with a big Attack boost is held by the exact last step, which is a hard edge for that one moment. Only that case uses it hard.
- Output is also clamped at +/-16 (+24 dBFS) as a last safety, even with Ceiling On off.
