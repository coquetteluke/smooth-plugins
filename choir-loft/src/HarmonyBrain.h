#pragma once
// Decides what pitch each voice sings: the modes, the glide between notes, and what makes each voice its own singer (drift, vibrato,
// humanize). Pitch is in semitones on the MIDI scale, using the reference pitch for A4. Runs on a 1 ms tick. No allocation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "MidiChord.h"
#include "Scales.h"

namespace smoothharmony
{
enum HarmonyMode { modeScale = 0, modeMidi, modeFixed, modeChord, modeChannel, numModes };

inline constexpr const char* modeNames[numModes] = { "Scale Interval", "MIDI Chord", "Fixed Interval", "Chord Tones", "MIDI Channel" };

// One voice's personality: everything that keeps it from being a copy of the others. Part of it is fixed for the voice (where it tends to sit,
// how far behind the lead it stands) and part is chosen fresh for each note, the way a person never sings a note the same way twice.
class Singer
{
public:
    static constexpr double driftFullCents = 20.0, driftFullMs = 40.0;
    static constexpr double vibratoFullCents = 60.0;
    static constexpr double humanizeDetuneCents = 8.0, humanizeBiasCents = 5.0, humanizeScoopCents = 70.0, humanizeSettleCents = 18.0, humanizeLagMs = 18.0;
    static constexpr double settleSeconds = 0.18, vibratoDelay = 0.18, vibratoRamp = 0.35;
    static constexpr double levelWanderFull = 0.17;   // about +/-1.5 dB at Drift 100

    void prepare (int index, double tickSeconds) noexcept
    {
        voice = index;
        tick = tickSeconds;
        reset();
    }

    void reset() noexcept
    {
        random = 0x9E3779B9u * (std::uint32_t) (voice + 1) + 12345u + seedOffset;
        clock = 0.0;
        // Each voice wanders on its own slow, unrelated sines. Bounded, so the range of Drift is exact, and never periodic in practice.
        for (int i = 0; i < 3; ++i)
        {
            pitchPhase[(size_t) i] = nextRandom() * M_PI;
            lagPhase[(size_t) i] = nextRandom() * M_PI;
            levelPhase[(size_t) i] = nextRandom() * M_PI;
        }
        vibratoPhase = nextRandom() * M_PI;
        scoop = settle = detune = extraLagMs = 0.0;
        scoopSeconds = 0.045;
        glideScale = 1.0;
        delayNow = vibratoDelay;
        depthScale = 1.0;
        humanizeNow = 0.0;
        age = 1.0e6;
    }

    // A new note, or a new phrase. direction: +1 going up, -1 going down, 0 starting from nothing.
    void noteEvent (double humanize, int direction) noexcept
    {
        static constexpr double tendency[8] = { 0.4, -0.8, 0.9, -0.5, 0.6, -0.3, -0.9, 0.2 };   // where this singer tends to sit, sharp or flat
        humanizeNow = humanize;
        detune = humanize * (humanizeBiasCents * tendency[voice] + humanizeDetuneCents * nextRandom());
        // In from below, less so when the line is coming down.
        scoop = -humanize * humanizeScoopCents * (0.7 + 0.3 * nextRandom()) * (direction < 0 ? 0.55 : 1.0);
        scoopSeconds = 0.035 + humanize * 0.04 * (0.5 + 0.5 * nextRandom());
        // The rest of the way to where the note ends up takes a while, from either side.
        settle = humanize * humanizeSettleCents * nextRandom();
        extraLagMs = humanize * humanizeLagMs * (0.5 + 0.5 * nextRandom());
        glideScale = 1.0 + humanize * 0.9 * (0.5 + 0.5 * nextRandom());
        delayNow = vibratoDelay + humanize * 0.15 * (0.5 + 0.5 * nextRandom());
        depthScale = 1.0 + humanize * 0.3 * nextRandom();
        age = 0.0;
    }

    void update (double drift, double vibratoAmount, double vibratoHz) noexcept
    {
        clock += tick;
        age += tick;
        static constexpr double pitchHz[3] = { 0.19, 0.43, 0.83 }, lagHz[3] = { 0.13, 0.31, 0.67 }, levelHz[3] = { 0.23, 0.53, 1.07 }, weight[3] = { 0.5, 0.3, 0.2 };
        static constexpr double standsBehindMs[8] = { 2.0, 16.0, 8.0, 26.0, 12.0, 21.0, 5.0, 31.0 };
        const double spread = 1.0 + 0.11 * voice;
        double w = 0.0, l = 0.0, a = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            w += weight[i] * std::sin (2.0 * M_PI * pitchHz[i] * spread * clock + pitchPhase[(size_t) i]);
            l += weight[i] * std::sin (2.0 * M_PI * lagHz[i] * spread * clock + lagPhase[(size_t) i]);
            a += weight[i] * std::sin (2.0 * M_PI * levelHz[i] * spread * clock + levelPhase[(size_t) i]);
        }
        scoop *= std::exp (-tick / scoopSeconds);
        settle *= std::exp (-tick / settleSeconds);

        vibratoPhase += 2.0 * M_PI * vibratoHz * vibratoRateScale (voice) * tick;
        if (vibratoPhase > 2.0 * M_PI) vibratoPhase -= 2.0 * M_PI;
        const double vibratoShape = std::clamp ((age - delayNow) / vibratoRamp, 0.0, 1.0);

        cents = drift * driftFullCents * w + detune + scoop + settle
              + vibratoAmount * vibratoFullCents * depthScale * vibratoShape * std::sin (vibratoPhase);
        lagMs = drift * driftFullMs * (0.5 + 0.5 * l) + extraLagMs + humanizeNow * standsBehindMs[voice];
        level = 1.0 + drift * levelWanderFull * a;
    }

    double pitchCents() const noexcept { return cents; }
    double lagMilliseconds() const noexcept { return lagMs; }
    double levelWander() const noexcept { return level; }
    // 1.6.3: no longer used for the glide (see HarmonyBrain::update); still drawn, so every other choice of Humanize stays the same.
    double glideStretch() const noexcept { return glideScale; }
    // 1.6.3: a rising note change that glides. The voice already comes in from below on the glide; the scoop from below on top of it held
    // it under its note for longer than a falling change, whose scoop works against its glide (round 6: a rising whole tone landed up to
    // 104 ms after the lead, a falling one 92). Now the scoop of a rising glide comes from above, the same size: like a falling change, it
    // works against the glide.
    void scoopAgainstRisingGlide() noexcept { scoop = -scoop; }
    static double vibratoRateScale (int v) noexcept
    {
        static constexpr double rateScale[8] = { 1.0, 0.96, 1.04, 0.93, 1.02, 0.98, 1.06, 0.95 };
        return rateScale[v];
    }

    // -1 to 1, the same sequence every run, so a bounce always comes out the same.
    double nextRandom() noexcept
    {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return (double) (random & 0xFFFFFFu) / 8388607.5 - 1.0;
    }

private:
    int voice = 0;
    double tick = 0.001, clock = 0.0, age = 1.0e6;
    std::uint32_t random = 1;
public:
    std::uint32_t seedOffset = 0;   // measuring tools only: another run of the same random choices. 0 in the plug-in.
private:
    std::array<double, 3> pitchPhase {}, lagPhase {}, levelPhase {};
    double vibratoPhase = 0.0, scoop = 0.0, settle = 0.0, detune = 0.0, extraLagMs = 0.0, cents = 0.0, lagMs = 0.0, level = 1.0;
    double scoopSeconds = 0.045, glideScale = 1.0, delayNow = vibratoDelay, depthScale = 1.0, humanizeNow = 0.0;
};

// 1.5.0, the Human control. What it adds to one voice, all of it scaled by the amount, none of it reaching the sound at 0, and none of it
// drawing from the Singer's own random numbers, so the sound at 0 is the sound before this existed, bit for bit.
// What was measured on Luke's own takes (realvox, RealVox robot) is what it goes after, in this order:
//  - inside a held note a voice sat 4 times stiller than he does (5 cents of spread against 20), and what movement it had was one steady
//    5 Hz sine. So: a slow wander and a faster flutter of the voice's own, both random, a part of the lead's own movement on the note,
//    smoothed and by a different amount for each voice, and a vibrato whose rate and depth no longer hold still.
//  - a voice's level followed the lead's exactly (curves alike 0.81, against 0.13 for Luke singing a part twice). So: a level wander of
//    its own, and the voice keeps part of its own level through the lead's quick swells and dips (the processor does that, see holdsLevel).
//  - the two voices moved as one block (0.42 in common, against 0.12). All of the above is per voice, from its own random numbers.
//  - voices ended within 4 ms of each other (Luke's two passes: 40 ms) and started 6 ms apart. So: each voice holds its level by its own
//    amount as a note dies away, and (1.5.2) holds the end of a note for its own time. (Letting go of a note early was tried and taken
//    out: the tracker's short gaps are mended after the fact, and a voice that had let go stayed off.) From 1.5.0 to 1.5.3 each note also
//    got its own shift in how far behind the lead the voice stands; 1.5.4 took that out, see noteEvent.
// Note changes were left alone: the voices already take about as long over one as he does (70 ms against 76).
class HumanVoice
{
public:
    static constexpr double slowCents = 12.8, fastCents = 7.5, levelDb = 0.65, rateShare = 0.08, depthShare = 0.3;
    static constexpr double slowAboutSeconds = 0.1;   // 1.5.3: where the lead's slow movement on a note ends and its quick movement begins
    // At Human 100, how much of the lead's quick level movement a voice does not copy
    static double holdsLevel (int v) noexcept
    {
        // 1.5.3: voice 2 from 0.3 to 0.6. Its level curve was still 0.71 alike the lead's on the working set and 0.79 on the first session's
        // takes, where the goal for the two voices together is 0.65 or less (they read 0.59 and 0.68 in 1.5.2).
        static constexpr double share[8] = { 0.7, 0.6, 0.55, 0.25, 0.6, 0.35, 0.5, 0.4 };
        return share[v];
    }

    void prepare (int index, double tickSeconds) noexcept
    {
        voice = index;
        tick = tickSeconds;
        reset();
    }

    void reset() noexcept
    {
        random = 0xC2B2AE35u * (std::uint32_t) (voice + 1) + 0x27D4EB2Fu + seedOffset;
        slow = slowMean = fast = fastMean = level = rate = depth = followed = leadMean = slowAbout = 0.0;
        landed = 1.0;
        cents = 0.0;
        gain = 1.0;
    }

    // A new note or a new phrase
    void noteEvent() noexcept
    {
        // 1.5.4: Human no longer moves a voice's place behind the lead. Up to 1.5.3 each note drew a shift of 3 ms earlier to 7 ms later,
        // and over a held note a voice fell up to 20 ms behind (voice 2), given back over the next note. A voice hears the lead through
        // its lag, so a lag that drops just after a note change makes it late by the drop: it decides on the new note through the old
        // lag, and then its read point jumps forward. On the first session's takes at Human 100 that left voice 2 landing 85 to 90 ms
        // into 4 held notes (7 late notes against 3 at Human 0, 5 allowed), and on the working set the shifts cost up to 0.0016 of the
        // time on note from 98 to 130 Hz with one run of random numbers (0.001 allowed). The two numbers are still drawn and set aside,
        // so every later random choice of this voice stays where it was.
        nextRandom();
        nextRandom();
        // 1.5.2: this note's own end. How long the voice holds on after the lead lets go, and how long it takes to die away.
        endHoldDraw = 0.7 + 0.3 * nextRandom();
        endFadeDraw = 0.5 + 0.5 * nextRandom();
        leadMean = 0.0;
        slowAbout = 0.0;
        // A note's first moments already carry Humanize's scoop and settle. The wander comes back in over the next tenth of a second, so
        // the two do not pile up and push a voice off its note as it lands.
        // 1.5.2: further out, and back over 150 ms. At Human 100 two held notes of the working set went wrong at their start for this:
        // a voice landing 20 ms later on its note than at Human 0, or 80 cents under it for 30 ms where the lead itself was sliding.
        landed *= 0.1;
    }

    // amount: 0 to 1, above 0. leadOffNote: how far the lead is from the note this voice hears it on, in semitones, or 0 when there is none.
    // followShare: how much of the lead's movement is left to follow, 1 less what Follow Lead already copies.
    void update (double amount, double leadOffNote, double followShare) noexcept
    {
        // 1.5.1: half of what 1.5.0 followed. What the voices take from the lead they take together, and at 100 the two default voices
        // still moved together 0.32 where two passes of Luke move 0.12.
        // 1.5.2: a fifth less again, so the share the two voices have in common (0.27 to 0.31 over the runs of the working set, 0.30
        // allowed) sits clear of its limit, and a voice does not follow the lead under its note when the lead slides into one.
        // 1.5.3: twice as much, now that each voice follows its own part of the lead (see below), so what it follows is not shared.
        // 1.5.4: half as much again, so a voice moves more with the lead without parting from the other voice. (A wider wander of its own
        // parts the two: with the slow wander at 14 cents the interval between them, spread inside 0.6 s, read 15.9 cents on the suite's
        // held notes, 15 allowed.)
        static constexpr double followGain[8] = { 0.156, 0.108, 0.132, 0.09, 0.12, 0.102, 0.144, 0.084 };
        static constexpr double followSeconds[8] = { 0.03, 0.055, 0.04, 0.07, 0.045, 0.06, 0.035, 0.075 };
        // The wander turns over within a note, as Luke's does, and has its long average taken out, so it moves the voice about its note
        // and not off it.
        walk (slow, 0.3);
        walk (fast, 0.04);
        walk (level, 0.25);
        walk (rate, 2.0);
        walk (depth, 2.0);
        slowMean += tick / 1.5 * (slow - slowMean);
        fastMean += tick / 0.3 * (fast - fastMean);
        // 1.5.4: back over 80 ms (was 150). The 40 cent reach (HarmonyBrain::update) now keeps the wander from piling onto a scoop as a
        // voice lands, which is what this was for, and a voice held still for 150 ms of every note: with the voices' lag no longer moving,
        // the pitch spread inside a note on the first session's takes read 9.94 cents at Human 100 (10 or more wanted); with 80 ms, 10.71.
        landed += tick / 0.08 * (1.0 - landed);

        // The lead's movement about where it has been sitting on this note, not how far off the note it is: a voice that copied a flat
        // lead would be flat too. Smoothed, since a singer follows what they hear and never its fastest part.
        leadMean += tick / 0.2 * (leadOffNote - leadMean);
        // 1.5.3: each voice follows its own part of that movement. Voice 1 (and every odd numbered one) the slow part, voice 2 (and every
        // even numbered one) what is left, the quick part. Two singers both following the same movement of the lead move together: at
        // Human 100 the two voices shared 0.27 to 0.34 of their movement over four runs of the working set with other random numbers
        // (0.30 allowed). Split, each still follows the lead, and the two parts hardly overlap.
        const double about = leadOffNote - leadMean;
        slowAbout += tick / slowAboutSeconds * (about - slowAbout);
        const double part = voice % 2 == 0 ? slowAbout : about - slowAbout;
        followed += tick / followSeconds[voice] * (part - followed);

        cents = std::clamp (amount * (landed * (slowCents * (slow - slowMean) + fastCents * (fast - fastMean)) + 100.0 * followGain[voice] * followShare * followed), -35.0, 35.0);
        gain = std::pow (10.0, amount * levelDb * level / 20.0);
        amountNow = amount;
    }

    double pitchCents() const noexcept { return cents; }
    double levelGain() const noexcept { return gain; }
    // Each voice's vibrato also runs at its own speed (1.5.1), up to a tenth off, so two voices' vibratos do not stay in step
    double vibratoRateScale() const noexcept
    {
        static constexpr double own[8] = { -0.09, 0.11, -0.05, 0.07, 0.1, -0.07, 0.04, -0.11 };
        return 1.0 + amountNow * (rateShare * rate + own[voice]);
    }
    double vibratoDepthScale() const noexcept { return 1.0 + amountNow * depthShare * depth; }
    // Added to how far behind the lead the voice stands, for this note: a little earlier or later
    // Each singer lets go of a note at a slightly different moment (1.5.2). At Human 100 a voice holds its last period for up to this
    // long after the lead's note ends, by its own amount and a little differently every note, then dies away over its own time. Two
    // passes of Luke end 40 ms apart; before this the voices ended 4 to 6 ms apart, because a voice ended where the lead's note was
    // called over and nothing but its place behind the lead could move that.
    double endHoldMs() const noexcept
    {
        // 1.5.4: voice 1 0 ms (was 5) and voice 2 34 ms (was 24), with voice 1 dying away over 4 ms (was 10) and voice 2 over 14 (was 18),
        // so no voice holds on longer than before. The ends of the two voices used to be apart partly because voice 2 had fallen behind
        // over the note (see noteEvent); that is gone, and a held end moves no read point while anything is being heard.
        static constexpr double holds[8] = { 0.0, 34.0, 12.0, 20.0, 8.0, 22.0, 10.0, 18.0 };
        return amountNow * holds[voice] * endHoldDraw;
    }
    double endFadeMs() const noexcept
    {
        static constexpr double fades[8] = { 4.0, 14.0, 14.0, 12.0, 16.0, 10.0, 18.0, 14.0 };
        return amountNow * fades[voice] * endFadeDraw;
    }

private:
    // A random walk that stays about one unit wide: pulled back to 0 over the given time
    void walk (double& x, double seconds) noexcept
    {
        x += -x * tick / seconds + std::sqrt (6.0 * tick / seconds) * nextRandom();
        x = std::clamp (x, -2.5, 2.5);
    }

    double nextRandom() noexcept
    {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return (double) (random & 0xFFFFFFu) / 8388607.5 - 1.0;
    }

    int voice = 0;
    double tick = 0.001;
    std::uint32_t random = 1;
public:
    std::uint32_t seedOffset = 0;   // measuring tools only: another run of the same random choices. 0 in the plug-in.
private:
    double slow = 0.0, slowMean = 0.0, fast = 0.0, fastMean = 0.0, level = 0.0, rate = 0.0, depth = 0.0, followed = 0.0, leadMean = 0.0;
    double cents = 0.0, gain = 1.0, amountNow = 0.0, landed = 1.0;
    double endHoldDraw = 0.85, endFadeDraw = 0.75, slowAbout = 0.0;
};

class HarmonyBrain
{
public:
    static constexpr int maxVoices = 8;

    struct Settings
    {
        int mode = modeScale;
        int voices = 2;
        std::uint16_t scale = 0x0FFF;   // bit per pitch class from C
        std::uint16_t chord = 0x0091;
        std::array<int, maxVoices> interval {};
        std::array<double, maxVoices> drift {};
        double glideMs = 20.0;
        double followLead = 0.25;       // 0 to 1: how much of the lead's own pitch wander the voices copy, in modes with a set note
        double humanize = 0.0;          // 0 to 1
        double human = 0.0;             // 0 to 1. 0 is the sound before 1.5.0, exactly
        double vibrato = 0.0;           // 0 to 1
        double vibratoHz = 5.5;
        double referenceHz = 440.0;
        bool chordHold = false;
    };

    struct Voice
    {
        bool on = false;                // this voice is singing or would sing if the lead were voiced
        double pitch = 0.0;             // semitones, what it should sing right now, everything included
        double target = 0.0;            // semitones, without the singer's drift, vibrato and humanize: what the tests measure against
        int note = -1;                  // the note it is on, -1 if none
    };

    std::uint32_t seedOffset = 0;   // measuring tools only, see PluginProcessor::Options::randomSeed

    void prepare (double sampleRate, double tickSeconds) noexcept
    {
        sr = sampleRate;
        tick = tickSeconds;
        riseSmoothing = 1.0 - std::exp (-tick / 0.005);   // pitch smoothed over 5 ms for its slope
        scoopRelease = std::exp (-tick / 0.02);
        for (int v = 0; v < maxVoices; ++v)
        {
            singers[(size_t) v].seedOffset = humans[(size_t) v].seedOffset = seedOffset * 0x85EBCA6Bu;
            singers[(size_t) v].prepare (v, tick);
            humans[(size_t) v].prepare (v, tick);
        }
        reset();
    }

    void reset() noexcept
    {
        chord.clearAll();
        split.fill (FollowSplit {});
        hasLead = false;
        silentFor = 1.0;
        startAge = 1.0e6;
        scoopFollow = 0.0;
        risingPitch = 0.0;
        leadScaleNote = -1;
        leadChromaticNote = -1;
        leadPitch = 0.0;
        lastMode = -1;
        for (int v = 0; v < maxVoices; ++v)
        {
            singers[(size_t) v].reset();
            humans[(size_t) v].reset();
            voices[(size_t) v] = {};
            remains[(size_t) v] = 0.0;
            noteHeld[(size_t) v] = 0.0;
            lastDiscrete[(size_t) v] = 1000000;
            sounding[(size_t) v] = false;
            phraseWaiting[(size_t) v] = false;
            voiceScaleNote[(size_t) v] = -1;
            voiceChromaticNote[(size_t) v] = -1;
            midiHeld[(size_t) v] = -1;
            midiAge[(size_t) v] = 0;
        }
    }

    MidiChord& midi() noexcept { return chord; }

    // What each voice reads of the lead right now, from the processor (1.6.1): whether it is still singing an earlier stretch of pitch than
    // the one going on now, and whether it has come to the one going on now.
    struct Heard
    {
        std::array<bool, maxVoices> lastStretch {}, inNew {};
    };

    // lead: semitones lined up with the output, 0 when there is none. voiced: the tracker has a pitch right now.
    // voiceLead: the same for each voice, later by how far that voice stands behind the lead. A voice hears the lead late, so it moves late.
    // reattack: the lead has just come in again much louder, as a new word does over the room's tail of the last one.
    // heardBy: see Heard. Without it every voice takes a new phrase's note choices the moment the phrase is called, as before 1.6.1.
    void update (double lead, const std::array<double, maxVoices>& voiceLead, bool voiced, const Settings& s, bool reattack = false,
                 const Heard* heardBy = nullptr) noexcept
    {
        chord.setHold (s.chordHold);
        if (s.mode == modeMidi) chord.assignChord (s.voices);
        else if (s.mode == modeChannel) chord.assignChannels (s.voices);

        // A phrase starts after silence. A new start inside a phrase is pitch coming back after a consonant or a breath, or a reattack.
        bool phraseStart = false, freshStart = false;
        if (voiced && lead > 0.0)
        {
            if (! hasLead)
            {
                phraseStart = true;
                leadScaleNote = leadChromaticNote = -1;
                voiceScaleNote.fill (-1);
                voiceChromaticNote.fill (-1);
            }
            else if (silentFor >= freshGapSeconds || reattack)
            {
                freshStart = true;
            }
            silentFor = 0.0;
            hasLead = true;
            leadPitch = lead;
        }
        else
        {
            silentFor += tick;
            if (silentFor > 0.04)
                hasLead = false;
        }

        // Into a note the lead often scoops up. Holding a scale note there sings another note first and then glides to the right one. So while
        // the lead is still rising from a start, the voices bend with it; once the rise stops they let go within about 20 ms.
        // Note changes in a start's first moments are taken at once, without a glide: the tracker's first ms are its least sure, and a voice
        // gliding in from the note before a consonant or a breath is heard as late. Glides are for notes sung legato.
        startedThisTick = phraseStart || freshStart;
        if (phraseStart || freshStart)
        {
            startAge = 0.0;
            scoopFollow = 1.0;
            risingPitch = leadPitch;
        }
        else if (hasLead)
        {
            startAge += tick;
            const double before = risingPitch;
            risingPitch += riseSmoothing * (leadPitch - risingPitch);
            const bool rising = (risingPitch - before) / tick > scoopRiseSemitonesPerSecond;
            if (! rising || startAge > scoopLongestSeconds)
                scoopFollow *= scoopRelease;
        }
        const double startFollow = hasLead ? scoopFollow : 0.0;

        if (hasLead)
        {
            leadScaleNote = followWithMargin (leadScaleNote, leadPitch, s.scale);
            leadChromaticNote = followWithMargin (leadChromaticNote, leadPitch, 0x0FFF);
        }

        for (int v = 0; v < maxVoices; ++v)
        {
            auto& out = voices[(size_t) v];
            // What this voice has heard of the lead so far.
            const double heard = voiced && voiceLead[(size_t) v] > 0.0 ? voiceLead[(size_t) v] : leadPitch;
            const double p = heard;
            const int oldChromatic = voiceChromaticNote[(size_t) v];
            if (hasLead)
            {
                const double margin = startAge < startSettleSeconds + 0.001 * singers[(size_t) v].lagMilliseconds() ? 0.06 : 0.24;
                voiceScaleNote[(size_t) v] = followWithMargin (voiceScaleNote[(size_t) v], p, s.scale, margin);
                voiceChromaticNote[(size_t) v] = followWithMargin (voiceChromaticNote[(size_t) v], p, 0x0FFF, margin);
            }
            const int scaleNote = voiceScaleNote[(size_t) v], chromaticNote = voiceChromaticNote[(size_t) v];
            const bool inCount = v < s.voices;
            const int interval = s.interval[(size_t) v];

            int discrete = 1000000;       // the part that moves in steps: a note, or the interval in Fixed
            double wanted = 0.0, fine = 0.0;
            bool wantsPitch = inCount && hasLead;
            bool on = inCount;
            static constexpr double copiesLess[8] = { 0.2, 0.5, 0.35, 0.7, 0.3, 0.6, 0.45, 0.8 };
            const double setFollow = std::clamp (s.followLead, 0.0, 1.0) * (1.0 - s.humanize * copiesLess[v]);
            const double follow = std::max (startFollow, setFollow);

            switch (s.mode)
            {
                case modeFixed:
                    discrete = interval;
                    wanted = p + interval;
                    out.note = hasLead ? chromaticNote + interval : -1;
                    break;
                case modeMidi:
                case modeChannel:
                {
                    // A chord change reaches each voice when that voice would have heard it.
                    const int played = chord.noteFor (v);
                    if (played == midiHeld[(size_t) v])
                    {
                        midiAge[(size_t) v] = 0;
                    }
                    else if (++midiAge[(size_t) v] > (int) (singers[(size_t) v].lagMilliseconds() * 0.001 / tick))
                    {
                        midiHeld[(size_t) v] = played;
                        midiAge[(size_t) v] = 0;
                    }
                    const int note = midiHeld[(size_t) v];
                    on = inCount && note >= 0;
                    wantsPitch = on && hasLead;
                    if (note >= 0 && hasLead)
                    {
                        const int folded = foldNear (note, p);
                        discrete = folded;
                        wanted = folded;
                        fine = follow * std::clamp (p - chromaticNote, -1.0, 1.0);
                        out.note = folded;
                    }
                    else out.note = note;
                    break;
                }
                case modeChord:
                    if (hasLead)
                    {
                        const int note = nearestInMask ((double) (chromaticNote + interval), s.chord);
                        discrete = note;
                        wanted = note;
                        fine = follow * std::clamp (p - chromaticNote, -1.0, 1.0);
                        out.note = note;
                    }
                    else out.note = -1;
                    break;
                default:
                    if (hasLead)
                    {
                        const int note = harmonyNote (scaleNote, interval, s.scale);
                        discrete = note;
                        wanted = note;
                        fine = follow * std::clamp (p - scaleNote, -1.0, 1.0);
                        out.note = note;
                    }
                    else out.note = -1;
                    break;
            }

            // 1.5.3, Human only: what Follow Lead copies of the lead's small movement about its note, Human splits between the voices the way
            // its own follow is split: by its amount it takes the quick part out of voice 1 (and every odd numbered voice) and the slow part
            // out of voice 2 (and every even numbered one). Copied whole by both, it moved the two voices together. Only movement within
            // 18 cents of the note is split: a slide from one note to the next is copied whole by both, or a voice lags the lead through it
            // and lands off its note (split whole, voice 1 sang off its note for 600 more 5 ms frames of the working set; within 25 cents,
            // 270). While the lead rises out of a start the voices still bend with all of it. At Human 0 none of this runs.
            if (s.human > 0.0 && s.mode != modeFixed && hasLead && follow > 0.0 && setFollow > 0.0)
            {
                auto& sp = split[(size_t) v];
                const double offNow = fine / follow;
                if (discrete != sp.note) { sp.mean = sp.slow = offNow; sp.note = discrete; }
                sp.mean += tick / splitMeanSeconds * (offNow - sp.mean);
                sp.slow += tick / HumanVoice::slowAboutSeconds * (offNow - sp.slow);
                const double part = std::clamp (v % 2 == 0 ? offNow - sp.slow : sp.slow - sp.mean, -splitReach, splitReach);
                fine -= setFollow * s.human * part;
            }

            auto& singer = singers[(size_t) v];
            auto& person = humans[(size_t) v];
            const bool humanOn = s.human > 0.0;
            const bool wasSounding = sounding[(size_t) v];

            // choir-dsp-r3-5 (1.6.1): a voice standing further behind the lead than the breath before a phrase still hears, and sings, the
            // last phrase's last note when the tracker calls the new phrase. The new phrase's scoop, detune, settle and place behind the lead
            // were drawn then and bent that old note's tail, and by the time the voice reached its new note the scoop had died away for tens
            // of ms. Such a voice now takes them when what it hears is of the new phrase. A voice that is past the old phrase when the new
            // one is called (every voice at the default settings: none stands further behind than the 40 ms a phrase needs before it) takes
            // them at once, as before.
            bool phraseStartHere = phraseStart;
            if (heardBy != nullptr)
            {
                auto& waiting = phraseWaiting[(size_t) v];
                if (phraseStart)
                {
                    waiting = heardBy->lastStretch[(size_t) v] && ! heardBy->inNew[(size_t) v];
                    phraseStartHere = ! waiting;
                }
                else if (waiting && heardBy->inNew[(size_t) v])
                {
                    waiting = false;
                    phraseStartHere = true;
                }
                else if (waiting && ! hasLead)
                {
                    waiting = false;   // the new phrase is over before this voice reached it
                }
            }

            if (wantsPitch)
            {
                const double aim = wanted + fine;
                // (A voice waiting for the new phrase changes note while it hears the breath; its note's choices come when it reaches the phrase.)
                const bool waitsForPhrase = phraseWaiting[(size_t) v];
                if (humanOn && (phraseStartHere
                                || (! waitsForPhrase && (! wasSounding || discrete != lastDiscrete[(size_t) v] || s.mode != lastMode
                                                         || (s.mode == modeFixed && chromaticNote != oldChromatic)))))
                    person.noteEvent();
                if (phraseStartHere || (! wasSounding && ! waitsForPhrase))
                {
                    remains[(size_t) v] = 0.0;
                    singer.noteEvent (s.humanize, 0);
                }
                else if ((discrete != lastDiscrete[(size_t) v] || s.mode != lastMode)
                         && (startAge < startSettleSeconds + 0.001 * singer.lagMilliseconds() || noteHeld[(size_t) v] < glideFromSeconds))
                {
                    remains[(size_t) v] = 0.0;
                }
                else if (discrete != lastDiscrete[(size_t) v] || s.mode != lastMode)
                {
                    // Keep going from where the voice actually is, then let the difference die away. That is the glide.
                    remains[(size_t) v] = lastPitch[(size_t) v] - aim;
                    singer.noteEvent (s.humanize, remains[(size_t) v] > 0.0 ? -1 : 1);
                    if (s.glideMs >= 0.5 && remains[(size_t) v] < 0.0)
                        singer.scoopAgainstRisingGlide();
                }
                else if (s.mode == modeFixed && chromaticNote != oldChromatic)
                {
                    singer.noteEvent (s.humanize, chromaticNote < oldChromatic ? -1 : 1);
                }
                // 1.6.3: the glide takes the Glide setting, no longer stretched by Humanize (up to 45% at its default, 90% at 100). Stretched, a
                // voice gliding a whole tone took up to 60 ms to come within 50 cents of its note, heard as late on Luke's takes (round 6).
                remains[(size_t) v] = s.glideMs < 0.5 ? 0.0 : remains[(size_t) v] * std::exp (-tick * 1000.0 / s.glideMs);
                out.target = aim + remains[(size_t) v];
                noteHeld[(size_t) v] = discrete != lastDiscrete[(size_t) v] ? 0.0 : noteHeld[(size_t) v] + tick;
                lastDiscrete[(size_t) v] = discrete;
                lastPitch[(size_t) v] = out.target;
            }
            else
            {
                remains[(size_t) v] = 0.0;
            }
            sounding[(size_t) v] = wantsPitch;

            if (humanOn)
            {
                // How far the lead is from the note this voice hears it on. Fixed Interval already copies all of the lead, so nothing is left to follow.
                const double off = ! wantsPitch || s.mode == modeFixed ? 0.0 : std::clamp (p - (s.mode == modeScale ? scaleNote : chromaticNote), -1.0, 1.0);
                person.update (s.human, off, 1.0 - std::clamp (s.followLead, 0.0, 1.0));
                singer.update (s.drift[(size_t) v], s.vibrato * person.vibratoDepthScale(), s.vibratoHz * person.vibratoRateScale());
                // 1.5.4: Human's own movement never takes a voice further than 40 cents from its note, counting the glide still to go and
                // Humanize's scoop, detune and vibrato; where those already reach that far it only ever brings the voice back. Its wander
                // used to push voices that were landing or scooping over the 50 cent line. Measured with two runs of random numbers, time on
                // the note from 98 to 130 Hz at Human 100: 0.9569 and 0.9563 without this, 0.9577 and 0.9571 with it (Human 0: 0.9582, 0.9574).
                const double others = 100.0 * remains[(size_t) v] + singer.pitchCents();
                const double own = std::clamp (person.pitchCents(), std::min (0.0, -humanReachCents - others), std::max (0.0, humanReachCents - others));
                out.pitch = out.target + (singer.pitchCents() + own) * 0.01;
            }
            else
            {
                singer.update (s.drift[(size_t) v], s.vibrato, s.vibratoHz);
                out.pitch = out.target + singer.pitchCents() * 0.01;
            }
            humanActive = humanOn;
            out.on = on;
        }
        lastMode = s.mode;
    }

    const Voice& voice (int v) const noexcept { return voices[(size_t) v]; }
    // Human only: this voice's own end for the note it is on. 0 with Human at 0.
    double endHoldMs (int v) const noexcept { return humanActive ? humans[(size_t) v].endHoldMs() : 0.0; }
    double endFadeMs (int v) const noexcept { return humanActive ? humans[(size_t) v].endFadeMs() : 0.0; }
    double lagMilliseconds (int v) const noexcept { return singers[(size_t) v].lagMilliseconds(); }
    bool startedNow() const noexcept { return startedThisTick; }   // a phrase or a fresh start inside one began on the last update
    double levelWander (int v) const noexcept
    {
        const double wander = singers[(size_t) v].levelWander();
        return humanActive ? wander * humans[(size_t) v].levelGain() : wander;
    }
    bool humanOn() const noexcept { return humanActive; }
    Singer& singer (int v) noexcept { return singers[(size_t) v]; }
    int leadNote() const noexcept { return hasLead ? leadChromaticNote : -1; }

    double periodFor (double semitones, double referenceHz) const noexcept
    {
        return sr / (referenceHz * std::pow (2.0, (semitones - 69.0) / 12.0));
    }

    // Out of reach for the shifter: moved by octaves to stay within two octaves of the lead.
    static int foldNear (int note, double lead) noexcept
    {
        int folded = note;
        while ((double) folded - lead > 24.0) folded -= 12;
        while (lead - (double) folded > 24.0) folded += 12;
        return folded;
    }

private:
    // Past the half way point by 12 cents before changing, so a note on the line does not flip. While a start settles, by 3 cents: a lead
    // that starts on the line, as a vibrato from the first moment can, should not hold its first reading's side for 20 ms.
    static int followWithMargin (int current, double p, std::uint16_t mask, double margin = 0.24) noexcept
    {
        const int nearest = nearestInMask (p, mask);
        if (current < 0 || ! inMask (current, mask))
            return nearest;
        if (nearest != current && std::abs (p - (double) current) - std::abs (p - (double) nearest) > margin)
            return nearest;
        return current;
    }

    double sr = 48000.0, tick = 0.001;
    MidiChord chord;
    std::array<Singer, maxVoices> singers;
    std::array<HumanVoice, maxVoices> humans;
    struct FollowSplit { double mean = 0.0, slow = 0.0; int note = -1000000; };
    std::array<FollowSplit, maxVoices> split {};
    static constexpr double splitMeanSeconds = 0.2, splitReach = 0.18, humanReachCents = 40.0;
    bool humanActive = false;
    std::array<Voice, maxVoices> voices {};
    std::array<double, maxVoices> remains {}, lastPitch {}, noteHeld {};
    std::array<int, maxVoices> lastDiscrete {}, voiceScaleNote {}, voiceChromaticNote {}, midiHeld {}, midiAge {};
    std::array<bool, maxVoices> sounding {}, phraseWaiting {};
    // A voice looks back by its lag, and the processor rewrites a start's first 30 ms of pitch, so settling lasts 50 ms plus the voice's lag.
    static constexpr double startSettleSeconds = 0.05, freshGapSeconds = 0.01;
    // A note held for less than this was an ornament or a misreading: the next note starts clean instead of gliding over from it.
    static constexpr double glideFromSeconds = 0.08;
    static constexpr double scoopRiseSemitonesPerSecond = 3.0, scoopLongestSeconds = 0.25;
    double scoopFollow = 0.0, risingPitch = 0.0, riseSmoothing = 0.2, scoopRelease = 0.95;
    bool hasLead = false, startedThisTick = false;
    double silentFor = 1.0, leadPitch = 0.0, startAge = 1.0e6;
    int leadScaleNote = -1, leadChromaticNote = -1, lastMode = -1;
};
} // namespace smoothharmony
