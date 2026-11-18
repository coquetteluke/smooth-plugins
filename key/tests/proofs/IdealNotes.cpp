// Proof tool, not part of the plug-in. Prints the notes the made up off key singer of KeyCorpus.h actually sings after a change
// of key: for each note its start, the pitch class nearest to the pitch he lands on (his own miss and his drift included, at A440)
// and its length. This is what an ideal reader with no noise would have to go on. tests/proofs/ideal_reader.py reads it.
// Build: clang++ -std=c++17 -O2 -Isrc -Itests tests/proofs/IdealNotes.cpp -o build/IdealNotes
// Run:   build/IdealNotes <semitones up> <first seed> <number of seeds>      (the clips of runStepUp in VoiceTests.h)
#include "KeyCorpus.h"
#include <cstdio>
#include <cstdlib>

// The same draws in the same order as corpus::sungClip, without making the sound.
static void sungNotes (int tonic, bool minor, unsigned seed, double seconds, double offset, const char* tag)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    std::normal_distribution<double> gauss (0.0, 1.0);
    const auto notes = corpus::melodyNotes (tonic, minor, seconds, rng);
    const double twoPi = 6.283185307179586476925;
    const double driftPeriod = 20.0 + 20.0 * uni (rng), driftPhase = twoPi * uni (rng);
    for (const auto& n : notes)
    {
        const double target = n.midi - 12.0 + std::clamp (25.0 * gauss (rng), -60.0, 60.0) * 0.01;
        (void) uni (rng); (void) uni (rng); // scoop, vibrato rate
        const double mid = n.start + 0.5 * n.length;
        const double landed = target + 0.2 * std::sin (twoPi * mid / driftPeriod + driftPhase);
        const int pc = (((int) std::lround (landed)) % 12 + 12) % 12;
        std::printf ("%s %.3f %d %.3f\n", tag, offset + n.start, pc, n.length);
    }
}

int main (int argc, char** argv)
{
    const int semitones = argc > 1 ? std::atoi (argv[1]) : 2;
    const unsigned first = argc > 2 ? (unsigned) std::atol (argv[2]) : 4000u;
    const int count = argc > 3 ? std::atoi (argv[3]) : 8;
    const double half = 45.0;
    for (int s = 0; s < count; ++s)
        for (int tonic = 0; tonic < 12; ++tonic)
            for (int minor = 0; minor < 2; ++minor)
            {
                char tag[64];
                const int oldSet = minor ? (tonic + 3) % 12 : tonic;
                std::snprintf (tag, sizeof tag, "N %u %d %d %d", first + (unsigned) s, tonic, minor, oldSet);
                sungNotes (tonic, minor == 1, corpus::clipSeed (first + (unsigned) s, tonic, minor, 6), half, 0.0, tag);
                sungNotes ((tonic + semitones) % 12, minor == 1, corpus::clipSeed (first + (unsigned) s, tonic, minor, 7), half, half, tag);
            }
    return 0;
}
