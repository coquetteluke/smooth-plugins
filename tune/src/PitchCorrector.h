#pragma once
// Decides what pitch the voice should come out at: target note, Retune Speed, Flex Amount, Humanize, Natural Vibrato, Transpose.
// All pitch is in semitones on the MIDI scale, using the reference pitch for A4. It runs on a 1 ms tick.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace smoothtune
{
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
    };

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        correction = 0.0; target = -1; pendingTarget = -1; pastFor = 0.0; farFor = 0.0; recentCount = 0; recentAt = 0; recent.fill (0.0);
        haveAverage = false; freshSlow = true; steadyFor = 0.0; holdAverage = 0.0; age = 0.0; slowPitch = 0.0; output = 0.0; input = 0.0;
        offCents = 0.0; identity = true; hasPitch = false; silentFor = 0.0;
    }

    // period in input samples, 0 when not voiced. tickSeconds is the time since the last call.
    void update (double period, const Settings& s, double tickSeconds) noexcept
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

        if (s.allowed == 0)
        {
            // Every note removed: nothing to aim at, so nothing to do.
            finishWithout (p, s);
            return;
        }

        // Is this a held note? The voice has stayed within 35 cents of its own average over the last tenth of a second for 150 ms.
        // Speech and rap slide through the notes and never do; a note with vibrato does.
        if (! haveAverage) { holdAverage = p; haveAverage = true; steadyFor = 0.0; }
        holdAverage += (1.0 - std::exp (-tickSeconds / 0.1)) * (p - holdAverage);
        steadyFor = std::abs (p - holdAverage) < 0.35 ? steadyFor + tickSeconds : 0.0;
        chooseTarget (p, s, tickSeconds);
        recentAt = (recentAt + 1) % recentSize;
        recent[(size_t) recentAt] = p;
        recentCount = std::min (recentCount + 1, recentSize - 1);
        hasPitch = true;

        const double drift = p - (double) target;
        offCents = drift * 100.0;
        age += tickSeconds;

        double wanted = 0.0;
        if (! isPassed (target, s.passed))
        {
            const double free = s.flexCents * 0.01;
            wanted = -(drift - std::clamp (drift, -free, free));
        }

        const double sustain = smoothstep (0.06, 0.25, age);
        const double tau = 0.001 * (s.retuneMs + s.humanize * sustain * humanizeMaxMs);
        if (tau < 0.0005)
            correction = wanted;
        else
            correction += (1.0 - std::exp (-tickSeconds / tau)) * (wanted - correction);

        // Natural Vibrato works on the vibrato that is left after correction, once the note centre is taken out. Up to 1.2.0 it worked on
        // the voice as sung, so where the correction had already taken the vibrato out, -100 put it back upside down: Robot Flat (Retune 0,
        // Natural Vibrato -100) kept 94% of a +/-40 cent vibrato, inverted, and on Luke's takes its held notes sat 15 to 18 cents off where
        // Hard Tune's sit 3 to 4.
        const double corrected = p + correction;
        if (freshSlow) { slowPitch = corrected; freshSlow = false; }   // a new note starts its average where it comes out
        double vibrato = 0.0;
        if (s.naturalVibrato != 0.0)
        {
            slowPitch += (1.0 - std::exp (-tickSeconds / 0.1)) * (corrected - slowPitch);
            vibrato = s.naturalVibrato * (corrected - slowPitch) * smoothstep (0.08, 0.25, age);
        }
        else
        {
            slowPitch = corrected;
        }

        output = p + correction + vibrato + s.transpose;
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
    double correctionCents() const noexcept { return hasPitch ? (output - input) * 100.0 : 0.0; }
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

private:
    static bool isAllowed (int note, std::uint16_t mask) noexcept { return (mask >> (((note % 12) + 12) % 12)) & 1; }
    static bool isPassed (int note, std::uint16_t mask) noexcept { return isAllowed (note, mask); }

    static double smoothstep (double a, double b, double x) noexcept
    {
        const double t = std::clamp ((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    // From Tune Pro 1.6.0 and 1.7.1 (4e1cddb, 029c00d, 9b355f6, 1d9780f), the corrector Tune shares with it.
    void chooseTarget (double p, const Settings& s, double tickSeconds) noexcept
    {
        const int nearest = nearestAllowed (p, s.allowed);
        if (target < 0 || ! isAllowed (target, s.allowed))
        {
            target = nearest;
            startNote (p);
            pastFor = 0.0;
            pendingTarget = -1;
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
                // back and forth, the voice was pulled down, then up, then down, and on average was left where it was (Tune Pro, Luke's
                // real takes, 2026-11-10). So the note being held is kept until the voice has stayed on the far side for holdSeconds in
                // a row, or is plainly on the other note, 35 cents past the half way point, or has just moved there in one go: 45 cents
                // or more towards the new note within the last 60 ms. A step moves like that; a vibrato of +/-20 cents moves 34 cents in
                // 60 ms at most. So a step is taken as fast as it always was, and only a waver has to wait. Only a held note is held on
                // to: a voice that slides, as speech does, changes target where it always did.
                // A held note that slides slowly onto the next note is not a waver either (Tune Pro, real take 09 at 97.0 s: a D3 sliding
                // down at about 13 cents per 100 ms). A voice that has been on the far side of the line for slideSeconds in a row is on
                // the other note: a 5.5 Hz waver across the line spends about 80 ms of each cycle there.
                pastFor = nearest == pendingTarget ? pastFor + tickSeconds : tickSeconds;
                pendingTarget = nearest;
                const double before = recent[(size_t) ((recentAt + recentSize - std::min (recentCount, moveTicks)) % recentSize)];
                const bool jumped = recentCount > 0 && (p - before) * (nearest > target ? 1.0 : -1.0) >= 0.45;
                const bool decided = steadyFor < steadySeconds || pastFor >= holdSeconds || farFor >= slideSeconds;
                if (past > 0.70 || jumped || decided)
                {
                    target = nearest;
                    startNote (p);
                    pastFor = 0.0;
                    pendingTarget = -1;
                }
            }
            else
            {
                pastFor = 0.0;
                pendingTarget = -1;
            }
        }
        else
        {
            pastFor = 0.0;
            farFor = 0.0;
            pendingTarget = -1;
        }
    }

    void startNote (double p) noexcept
    {
        farFor = 0.0;
        age = 0.0;
        slowPitch = p;
        freshSlow = true;
    }

    void finishWithout (double p, const Settings& s) noexcept
    {
        hasPitch = false;
        target = -1;
        correction = 0.0;
        output = p + s.transpose;
        identity = s.transpose == 0.0;
    }

    double sr = 48000.0;
    double correction = 0.0, age = 0.0, slowPitch = 0.0, output = 0.0, input = 0.0, offCents = 0.0, silentFor = 0.0;
    int target = -1, pendingTarget = -1;
    double pastFor = 0.0, farFor = 0.0;
    // The last 64 readings of the voice's pitch, one a tick, for telling a step from a waver
    static constexpr int recentSize = 64, moveTicks = 60;
    std::array<double, recentSize> recent {};
    int recentAt = 0, recentCount = 0;
    // slideSeconds: Tune Pro took 0.12 at its Retune 3 ms default; at Tune's default Retune 20 ms the glide onto the new note adds about
    // 35 ms, and 0.12 put the slow slide test at 156 ms against its 150 (Tune 1.1.1: 122). 0.10 reads 136 ms, and a 5.5 Hz waver across the
    // line still spends less than that (about 82 ms) on the far side (PluginTest hold).
    static constexpr double holdSeconds = 0.08, steadySeconds = 0.15, slideSeconds = 0.10;
    double holdAverage = 0.0, steadyFor = 0.0;
    bool haveAverage = false, freshSlow = true;
    bool identity = true, hasPitch = false;
};
} // namespace smoothtune
