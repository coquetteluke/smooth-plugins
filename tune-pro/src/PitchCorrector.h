#pragma once
// Decides what pitch the voice should come out at: target note, Retune Speed, Flex Tune, Humanize, Natural Vibrato, the vibrato
// generator, Classic and Transpose. All pitch is in semitones on the MIDI scale, using the reference pitch for A4. Runs on a 1 ms tick.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace smoothtune
{
// Every MIDI note that is down or held by the pedal: one bit per note, safe to copy around on the audio thread.
struct HeldNotes
{
    std::array<std::uint64_t, 2> bits { 0, 0 };

    void set (int note, bool on) noexcept
    {
        if (note < 0 || note > 127) return;
        const std::uint64_t mask = std::uint64_t (1) << (note & 63);
        if (on) bits[(size_t) (note >> 6)] |= mask; else bits[(size_t) (note >> 6)] &= ~mask;
    }
    bool has (int note) const noexcept { return note >= 0 && note <= 127 && ((bits[(size_t) (note >> 6)] >> (note & 63)) & 1) != 0; }
    bool any() const noexcept { return bits[0] != 0 || bits[1] != 0; }
    void clear() noexcept { bits = { 0, 0 }; }

    std::uint16_t pitchClasses() const noexcept
    {
        std::uint16_t m = 0;
        for (int n = 0; n < 128; ++n)
            if (has (n)) m = (std::uint16_t) (m | (1u << (n % 12)));
        return m;
    }

    int nearest (double p) const noexcept
    {
        int best = -1;
        double distance = 1.0e9;
        for (int n = 0; n < 128; ++n)
            if (has (n) && std::abs ((double) n - p) < distance) { distance = std::abs ((double) n - p); best = n; }
        return best;
    }
};

class PitchCorrector
{
public:
    struct Settings
    {
        double retuneMs = 20.0;       // 0 is truly instant
        double humanize = 0.0;        // 0 to 1: how much the sustained part of a note is left alone
        double flexCents = 0.0;       // how far a note may drift before correction applies
        double naturalVibrato = 0.0;  // -1 removes vibrato, 0 leaves it, +1 doubles it
        double transpose = 0.0;       // semitones, after correction
        double referenceHz = 440.0;
        std::uint16_t allowed = 0x0FFF;  // bit per pitch class from C: may be a target
        std::uint16_t passed = 0;        // bit per pitch class: a target, but left uncorrected
        bool classic = false;            // the plain fast snap: no Flex Tune, Humanize or vibrato shaping
        bool midiTargets = false;        // held notes are the targets; with none held, no correction
        bool midiAllOctaves = true;      // a held note counts in every octave
        double vibratoCents = 0.0;       // added vibrato, half of peak to peak
        double vibratoHz = 5.5;
        double vibratoDelayMs = 300.0;   // from the start of a note until it starts to fade in
    };

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        correction = 0.0; target = -1; pendingTarget = -1; pastFor = 0.0; farFor = 0.0; evidence = 0.0; recentCount = 0; recentAt = 0; recent.fill (0.0); haveAverage = false; steadyFor = 0.0; holdAverage = 0.0; age = 0.0; slowPitch = 0.0; output = 0.0; input = 0.0;
        offCents = 0.0; identity = true; hasPitch = false; silentFor = 0.0; vibratoPhase = 0.0;
    }

    // period in input samples, 0 when not voiced. tickSeconds is the time since the last call. held only matters with midiTargets.
    void update (double period, const Settings& s, const HeldNotes& held, double tickSeconds) noexcept
    {
        if (period <= 0.0)
        {
            // A dropout of a few ms in the middle of a note must not restart the note's correction.
            silentFor += tickSeconds;
            if (silentFor > 0.04)
                reset();
            return;
        }
        silentFor = 0.0;
        const double hz = sr / period;
        const double p = 69.0 + 12.0 * std::log2 (hz / s.referenceHz);
        input = p;

        // Is this a held note? The voice has stayed within 35 cents of its own average over the last tenth of a second for 150 ms.
        // Speech and rap slide through the notes and never do; a note with vibrato does.
        if (! haveAverage) { holdAverage = p; haveAverage = true; steadyFor = 0.0; }
        holdAverage += (1.0 - std::exp (-tickSeconds / 0.1)) * (p - holdAverage);
        steadyFor = std::abs (p - holdAverage) < 0.35 ? steadyFor + tickSeconds : 0.0;
        const bool haveTarget = chooseTarget (p, s, held, tickSeconds);
        recentAt = (recentAt + 1) % recentSize;
        recent[(size_t) recentAt] = p;
        recentCount = std::min (recentCount + 1, recentSize - 1);
        hasPitch = haveTarget;
        age += tickSeconds;

        double wanted = 0.0;
        if (haveTarget)
        {
            const double drift = p - (double) target;
            offCents = drift * 100.0;
            if (s.midiTargets || ! isPassed (target, s.passed))
            {
                const double free = s.classic ? 0.0 : s.flexCents * 0.01;
                wanted = -(drift - std::clamp (drift, -free, free));
            }
        }
        else
        {
            offCents = 0.0;
        }

        const double sustain = smoothstep (0.06, 0.25, age);
        const double humanizeNow = s.classic ? 0.0 : s.humanize;
        const double retuneNow = s.classic ? 0.0 : s.retuneMs;
        const double tau = 0.001 * (retuneNow + humanizeNow * sustain * humanizeMaxMs);
        // Letting go of the last MIDI note or removing every note relaxes rather than jumps, unless the setting is instant.
        if (tau < 0.0005)
            correction = wanted;
        else
            correction += (1.0 - std::exp (-tickSeconds / tau)) * (wanted - correction);

        // Natural Vibrato works on what is left once the note centre is taken out.
        double vibrato = 0.0;
        if (s.naturalVibrato != 0.0 && ! s.classic)
        {
            slowPitch += (1.0 - std::exp (-tickSeconds / 0.1)) * (p - slowPitch);
            vibrato = s.naturalVibrato * (p - slowPitch) * smoothstep (0.08, 0.25, age);
        }
        else
        {
            slowPitch = p;
        }

        // The vibrato generator fades in over 150 ms after its delay, as a singer's would.
        double added = 0.0;
        if (s.vibratoCents > 0.0 && ! s.classic)
        {
            const double start = 0.001 * s.vibratoDelayMs;
            const double fade = smoothstep (start, start + vibratoFadeSeconds, age);
            vibratoPhase += tickSeconds * s.vibratoHz;
            vibratoPhase -= std::floor (vibratoPhase);
            added = 0.01 * s.vibratoCents * fade * std::sin (2.0 * M_PI * vibratoPhase);
        }
        else
        {
            vibratoPhase = 0.0;
        }

        output = p + correction + vibrato + added + s.transpose;
        identity = std::abs (output - p) < 0.001;
    }

    bool isIdentity() const noexcept { return identity; }
    bool active() const noexcept { return hasPitch; }

    // Wanted output period in input samples.
    double outputPeriod (const Settings& s) const noexcept
    {
        return sr / (s.referenceHz * std::pow (2.0, (output - 69.0) / 12.0));
    }

    // Readings
    double centsOffTarget() const noexcept { return hasPitch && target >= 0 ? offCents : 0.0; }
    double correctionCents() const noexcept { return (output - input) * 100.0; }
    int targetNote() const noexcept { return hasPitch ? target : -1; }
    double inputPitch() const noexcept { return input; }
    double outputPitch() const noexcept { return output; }

    // Nearest note the mask allows. Exposed for tests.
    static int nearestAllowed (double p, std::uint16_t allowed) noexcept
    {
        const int centre = (int) std::lround (p);
        for (int distance = 0; distance <= 12; ++distance)
        {
            const int up = centre + distance, down = centre - distance;
            const bool hasUp = isAllowed (up, allowed), hasDown = isAllowed (down, allowed);
            if (hasUp && hasDown)
                return std::abs ((double) up - p) < std::abs ((double) down - p) ? up : down;
            if (hasUp) return up;
            if (hasDown) return down;
        }
        return centre;
    }

    static constexpr double humanizeMaxMs = 400.0;
    static constexpr double vibratoFadeSeconds = 0.15;

private:
    static bool isAllowed (int note, std::uint16_t mask) noexcept { return (mask >> (((note % 12) + 12) % 12)) & 1; }
    static bool isPassed (int note, std::uint16_t mask) noexcept { return isAllowed (note, mask); }

    static double smoothstep (double a, double b, double x) noexcept
    {
        const double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    bool chooseTarget (double p, const Settings& s, const HeldNotes& held, double tickSeconds) noexcept
    {
        bool targetStillGood;
        int nearest;
        if (s.midiTargets)
        {
            if (! held.any()) { target = -1; return false; }
            if (s.midiAllOctaves)
            {
                const auto mask = held.pitchClasses();
                nearest = nearestAllowed (p, mask);
                targetStillGood = target >= 0 && isAllowed (target, mask);
            }
            else
            {
                nearest = held.nearest (p);
                targetStillGood = held.has (target);
            }
        }
        else
        {
            if (s.allowed == 0) { target = -1; return false; }
            nearest = nearestAllowed (p, s.allowed);
            targetStillGood = target >= 0 && isAllowed (target, s.allowed);
        }

        if (targetStillGood && weighMode == 2 && ! s.classic)
        {
            // The note's own average decides, not the moment: see noteMean.
            const double tau = std::clamp (0.5 * age, meanFastest, meanSlowest);
            noteMean += (1.0 - std::exp (-tickSeconds / tau)) * (p - noteMean);
            int to = -1;
            if (nearest != target)
            {
                const double past = std::abs (p - (double) target) - std::abs (p - (double) nearest);
                const double before = recent[(size_t) ((recentAt + recentSize - std::min (recentCount, moveTicks)) % recentSize)];
                const bool jumped = recentCount > 0 && (p - before) * (nearest > target ? 1.0 : -1.0) >= 0.45;
                if (past > 0.70 || (jumped && past > 0.24))
                    to = nearest;
            }
            if (to < 0)
            {
                const int nearestMean = s.midiTargets && ! s.midiAllOctaves ? held.nearest (noteMean)
                                                                             : nearestAllowed (noteMean, s.midiTargets ? held.pitchClasses() : s.allowed);
                if (nearestMean >= 0 && nearestMean != target && std::abs (noteMean - (double) target) - std::abs (noteMean - (double) nearestMean) > 0.24)
                    to = nearestMean;
            }
            if (to >= 0)
            {
                target = to;
                startNote (p);
            }
            pastFor = 0.0; evidence = 0.0; pendingTarget = -1;
            return true;
        }
        if (! targetStillGood)
        {
            target = nearest;
            startNote (p);
        }
        else if (nearest != target)
        {
            // How long the voice has been nearer another note than the target, in a row (see slideSeconds)
            farFor += tickSeconds;
            // Past the half way point by 12 cents before changing, so a note on the line does not flip.
            const double past = std::abs (p - (double) target) - std::abs (p - (double) nearest);
            if (past > 0.24)
            {
                // That alone still flips on a singer who holds a note near the line and wavers by more than 12 cents: the target went
                // back and forth, the voice was pulled down, then up, then down, and on average was left where it was (real takes,
                // 2026-11-10: 17 of 727 held notes at 98 Hz and over left uncorrected this way with the song's key set). So the note
                // being held is kept until the voice has stayed on the far side for holdSeconds in a row, or is plainly on the other
                // note, 35 cents past the half way point, or has just moved there in one go: 45 cents or more towards the new note within
                // the last 60 ms. A step moves like that; a vibrato of +/-20 cents moves 34 cents in 60 ms at most. So a step is taken
                // as fast as it always was, and only a waver has to wait.
                pastFor = nearest == pendingTarget ? pastFor + tickSeconds : tickSeconds;
                evidence = nearest == pendingTarget ? evidence + (past - 0.24) * tickSeconds : (past - 0.24) * tickSeconds;
                pendingTarget = nearest;
                const double before = recent[(size_t) ((recentAt + recentSize - std::min (recentCount, moveTicks)) % recentSize)];
                const bool jumped = recentCount > 0 && (p - before) * (nearest > target ? 1.0 : -1.0) >= 0.45;
                // Only a held note is held on to: a voice that slides, as speech does, changes target where it always did. (Holding on
                // through a slide made the corrections bigger: real speech, worst 1% of 20 ms frames, level change 2.86 to 3.23 dB.)
                // Tried on 2026-11-11 and left switched off (weighMode 0 is 1.6.0's rule): on Luke's takes 17 of 739 held notes are still
                // pulled one way and then the other and come out where they were sung, and 3 end on the wrong note; in nearly all of them
                // his pitch crosses the line between two notes of the scale inside the note and the target changes 1 to 8 times. Two rules
                // that make the target wait were measured on the 11 working takes (engine traces, 714 held notes): adding up the cents
                // past the 12 cent mark until they reach 0.8, 1.5 or 2.5 cent seconds (mode 1), and letting the note's own average decide
                // (mode 2). Both left fewer notes as sung (10 to 7) and put more on the wrong note (3 to 8, 10, 12 and 12). A target that
                // waits is late when he really has moved. So neither ships.
                // A held note that slides slowly onto the next note is not a waver either (real take 09 at 97.0 s: his D3 slid down at
                // about 13 cents per 100 ms and settled on C#3 +20 cents; the 12 cent mark came 90 ms after the line and the 80 ms after
                // that, so most of the held note came out on D3, a wrong note). A voice that has been on the far side of the line for
                // slideSeconds in a row is on the other note: a 5.5 Hz waver across the line spends about 80 ms of each cycle there.
                const bool decided = weighTargets ? evidence >= targetEvidence
                                                  : (steadyFor < steadySeconds || pastFor >= holdSeconds || farFor >= slideSeconds);
                if (! holdTargets || s.classic || past > 0.70 || jumped || decided)
                {
                    target = nearest;
                    startNote (p);
                    pastFor = 0.0;
                    evidence = 0.0;
                    pendingTarget = -1;
                }
            }
            else
            {
                pastFor = 0.0;
                evidence = 0.0;
                pendingTarget = -1;
            }
        }
        else
        {
            pastFor = 0.0;
            farFor = 0.0;
            evidence = 0.0;
            pendingTarget = -1;
        }
        return true;
    }

    void startNote (double p) noexcept
    {
        farFor = 0.0;
        age = 0.0;
        slowPitch = p;
        noteMean = p;
    }

    double sr = 48000.0;
    double correction = 0.0, age = 0.0, slowPitch = 0.0, output = 0.0, input = 0.0, offCents = 0.0, silentFor = 0.0, vibratoPhase = 0.0;
    int target = -1, pendingTarget = -1;
    double pastFor = 0.0, farFor = 0.0, evidence = 0.0;
#ifndef TARGET_EVIDENCE
 #define TARGET_EVIDENCE 0.015
#endif
#ifndef WEIGH_TARGETS
 #define WEIGH_TARGETS 0
#endif
    static constexpr int weighMode = WEIGH_TARGETS;   // 0: 1.6.0's rule. 1: cents past the line added up. 2: the note's own average decides.
    static constexpr bool weighTargets = weighMode == 1;
    // Mode 2: an average of the voice since the target was chosen, over the newer half of the note so far, 30 ms at least, 120 at most
    static constexpr double meanFastest = 0.03, meanSlowest = 0.12;
    double noteMean = 0.0;
    static constexpr double targetEvidence = TARGET_EVIDENCE;   // semitone seconds
    // The last 64 readings of the voice's pitch, one a tick, for telling a step from a waver
    static constexpr int recentSize = 64, moveTicks = 60;
    std::array<double, recentSize> recent {};
    int recentAt = 0, recentCount = 0;
    static constexpr bool holdTargets = true;
    static constexpr double holdSeconds = 0.08, steadySeconds = 0.15, slideSeconds = 0.12;
    double holdAverage = 0.0, steadyFor = 0.0;
    bool haveAverage = false;
    bool identity = true, hasPitch = false;
};
} // namespace smoothtune
