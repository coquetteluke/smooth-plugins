# Changelog

## 1.3.1, 2026-11-07

Renamed to Choir Loft, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.3.0

Up to 8 singers, and click to choose who you hear.

Voices 5 to 8, each with Interval, Level, Pan, Drift, Throat and Tone, and its own colour and card. Voices keeps its range of 1 to 4 and a new More Voices adds up to 4 on top, because a host stores automation of Voices as a position in its range, and widening it would have moved old automation. The window shows them as one row, 1 to 8.

Click a singer on the stage, or a voice's name on its card, to hear it or not. New parameters: Voice 1 to 8 On and Lead On. All brings everyone back. Off singers and their lines dim, fade over 15 ms and stop using CPU. A click with no movement switches, a drag moves. A double-click on a singer no longer resets it, because two quick clicks now mean off and on again.

Above 4 voices, the voices together turn down so 8 stay about as loud as 4: -3 dB at 8. New presets: Six Part Stack, Eight Part Choir, MIDI Eight Part Choir, after the old ones, which keep their places. No limiter.

Defaults and every old preset are bit for bit the same as 1.2.0. Sessions from before open with everyone on and their own voice count.

The window now says Choir Loft, Luke's final name. The build and AU name are unchanged for now.

## 1.2.0

The custom window. The display shows each voice as a line through time, running into its singer on a stage: up and down is the note, left and right is pan, size is Level. Drag a singer to pan it or change its interval. Voice strips with the note, the interval in words, Level, Pan, Drift and Tone. Everything else is behind Advanced. Replaces the generic window.

Every parameter keeps its ID, name and range. The sound is unchanged, bit for bit against 1.1.0 on 5 presets, with and without the window open. The processor now also stores a level per 5 ms for the display, lock free. New in saved state: window width and whether Advanced is open, nothing else.

## 1.1.0

More like separate people. Luke heard 1.0.0 on his voice and found it robotic. Nobody has heard this version yet.
New controls: Voice 1 to 4 Tone, Consonant Scatter, Breath. All old parameter IDs stay. Latency is the same.
New defaults: Drift 30, Humanize 50, Glide 30, Follow Lead 8, Consonants 22, wider Throat, per voice Tone. Presets updated. New preset Tight, for exact tuning within the 1.0.0 limits.
Humanize now also covers: settling off target, each voice's own tendency, standing behind the lead, a slower glide, softer starts, its own vibrato.
Each voice hears the lead late, so note changes do not land together. Drift also moves level a little.
Consonants can be each voice's own, later and softer, and breaths fade. A voice now sings to the end of the vowel it is reading.
With Humanize above 0, grains read between neighbouring periods, and read position is nudged per grain.

## 1.0.0

First version. Up to four formant preserving harmony voices from one lead vocal, with the lead delay matched.
Modes: Scale Interval, MIDI Chord, Fixed Interval, Chord Tones, MIDI Channel.
Controls: Mode, Key, Scale, Follow Key, Voices, per voice Interval, Level, Pan, Drift and Throat, Humanize, Vibrato, Vibrato Rate, Lead Level, Lead Pan, Harmony Level, Mix, Glide, Follow Lead, Consonants, Chord Root, Chord Type, Chord Hold, Input Type, Tracking, Reference A4, Bypass.
Readings: Detected Pitch, Lead Note, Voice 1 to 4 Note, Voices Sounding, Active Key. Ten factory presets. Generic window.
