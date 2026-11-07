#pragma once
// Decides what pitch the voice should come out at: target note, Retune Speed, Flex Amount, Humanize, Natural Vibrato, Transpose.
// Smooth Tune FX adds one thing to the copy from Smooth Tune: held MIDI notes, as played, can stand in for the scale.
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
        const std::array<bool, 128>* heldNotes = nullptr; // when set, the targets are exactly these notes, in the octaves they were played
    };

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        correction = 0.0; target = -1; age = 0.0; slowPitch = 0.0; output = 0.0; input = 0.0;
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

        const bool asPlayed = s.heldNotes != nullptr;
        if (! asPlayed && s.allowed == 0)
        {
            // Every note removed: nothing to aim at, so nothing to do.
            finishWithout (p, s);
            return;
        }

        const int nearest = asPlayed ? nearestHeld (p, *s.heldNotes) : nearestAllowed (p, s.allowed);
        const bool targetGood = target >= 0 && (asPlayed ? (target < 128 && (*s.heldNotes)[(size_t) target]) : isAllowed (target, s.allowed));
        if (! targetGood)
        {
            target = nearest;
            startNote (p);
        }
        else if (nearest != target)
        {
            // Past the half way point by 12 cents before changing, so a note on the line does not flip.
            if (std::abs (p - (double) target) - std::abs (p - (double) nearest) > 0.24)
            {
                target = nearest;
                startNote (p);
            }
        }
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

        // Natural Vibrato works on what is left once the note centre is taken out.
        double vibrato = 0.0;
        if (s.naturalVibrato != 0.0)
        {
            slowPitch += (1.0 - std::exp (-tickSeconds / 0.1)) * (p - slowPitch);
            vibrato = s.naturalVibrato * (p - slowPitch) * smoothstep (0.08, 0.25, age);
        }
        else
        {
            slowPitch = p;
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

    // Nearest of the held notes, however far away. One held note is always the target. With none held, it returns the rounded pitch.
    static int nearestHeld (double p, const std::array<bool, 128>& held) noexcept
    {
        int best = -1;
        double bestGap = 1.0e9;
        for (int n = 0; n < 128; ++n)
            if (held[(size_t) n] && std::abs ((double) n - p) < bestGap)
            {
                bestGap = std::abs ((double) n - p);
                best = n;
            }
        return best >= 0 ? best : (int) std::lround (p);
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

    void startNote (double p) noexcept
    {
        age = 0.0;
        slowPitch = p;
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
    int target = -1;
    bool identity = true, hasPitch = false;
};
} // namespace smoothtune
