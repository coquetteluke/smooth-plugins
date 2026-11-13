#pragma once
// A number that changes if any reading the analyser gives changes by even one bit. Proves the window's extra read-only data does
// not alter detection. The expected value is the one 1.5.0 and 1.5.1 gave: since 1.7.0 the lone voice reading applies only to
// a sound with no tuning of its own, so these clean clips, in both Sources, read exactly as they did before 1.6.0 (which had
// changed the Single Voice half, 0x96fd1a25267dfa4b). The Full Mix chord clips are pinned by expectedChordsInMix173 below as well.
// Standalone: clang++ -std=c++17 -O2 -Isrc -Itests -x c++ -DFINGERPRINT_MAIN tests/ReadingFingerprint.h -o build/ReadingFingerprint
#include "KeyCorpus.h"
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace fingerprint
{
inline constexpr std::uint64_t expectedBefore = 0x7630f100ad4cf801ull;

inline void mix (std::uint64_t& h, const void* data, size_t n)
{
    const auto* b = static_cast<const unsigned char*> (data);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
}

inline std::uint64_t readingFingerprint()
{
    using namespace corpus;
    using namespace smoothkey;
    std::uint64_t h = 14695981039346656037ull;
    const double sr = 48000.0;
    const auto ts = timbres();
    const auto ps = progressions();
    for (int tonic : { 0, 7, 10 })
        for (int style = 0; style < 2; ++style)
            for (auto response : { Response::stable, Response::fast })
            {
                const auto& prog = ps[(size_t) (tonic + style) % ps.size()];
                const auto clip = chordClip (tonic, prog, ts[(size_t) (tonic + style) % ts.size()], 77u + (unsigned) tonic, sr, 16.0);
                KeyAnalyser a;
                a.configure (sr);
                a.setResponse (response);
                a.setSource (style == 0 ? Source::mix : Source::voice);
                for (size_t pos = 0; pos + 480 <= clip.size(); pos += 480)
                {
                    a.push (clip.data() + pos, 480);
                    const auto& r = a.reading();
                    const std::int32_t ints[] = { r.key, r.minor ? 1 : 0, r.altKey, r.altMinor ? 1 : 0, r.signal ? 1 : 0 };
                    mix (h, ints, sizeof ints);
                    mix (h, &r.confidence, sizeof r.confidence);
                    mix (h, &r.tuningCents, sizeof r.tuningCents);
                    mix (h, &r.tempoBpm, sizeof r.tempoBpm);
                    mix (h, r.chroma.data(), sizeof (float) * r.chroma.size());
                }
            }
    return h;
}

// Chords in Full Mix must read exactly as they did before the lone voice reading was added: every reading, confidence, tuning and
// chroma value over 96 chord clips (12 keys, 8 progressions, every timbre, Stable and Fast in turn), 30 s each. The value 1.5.1
// gave (commit f0f323d, whose detection is 1.5.0's) held through 1.7.2. 1.7.3 reads the top log bin instead of drawing it on past
// the last bin (key-dsp-r2-1), which changes the clips tuned more than about 24 cents sharp, so the pin is now 1.7.3's value, and
// with that one change taken out (setDrawPastTopBin) the clips still give 1.5.1's value bit for bit.
inline constexpr std::uint64_t expectedChordsInMix = 0x6d8feb3810100d9full;      // 1.5.1 to 1.7.2: now the top bin drawn on
inline constexpr std::uint64_t expectedChordsInMix173 = 0xecdc4ad4cc3a786aull;   // 1.7.3: the top bin read

// The 96 clips read both ways side by side, 1.7.3's and 1.7.2's top bin, with what differs between them clip by clip.
struct ChordsInMixRun
{
    std::uint64_t read = 0, drawnOn = 0;  // the fingerprint each way
    int clips = 0, changed = 0;           // clips, and clips where any reading differs by any bit
    int keyDiffers = 0;                   // clips where the key on show differs at any moment
    int finalDiffers = 0;                 // clips that end on a different key
    int rightBefore = 0, rightAfter = 0;  // clips that end on their own key, top bin drawn on and read
    int rightThenWrong = 0;               // clips that ended on their own key with it drawn on and do not now
    int stepsKeyDiffers = 0, steps = 0;   // 10 ms steps where the key on show differs, of all steps
    double maxTuningCents = -100.0;       // the sharpest tuning read in a changed clip, at its end
    double minTuningCents = 100.0;        // the least sharp tuning read in a changed clip, at its end
    double maxConfidenceMove = 0.0;       // the largest confidence difference at any moment
    std::string changedClips;             // which clips changed: key and progression
};

inline ChordsInMixRun chordsInMixCompare()
{
    using namespace corpus;
    using namespace smoothkey;
    ChordsInMixRun run;
    run.read = run.drawnOn = 14695981039346656037ull;
    const auto ts = timbres();
    const auto ps = progressions();
    const auto mixReading = [] (std::uint64_t& h, const Reading& r)
    {
        const std::int32_t ints[] = { r.key, r.minor ? 1 : 0, r.altKey, r.altMinor ? 1 : 0, r.signal ? 1 : 0 };
        mix (h, ints, sizeof ints);
        mix (h, &r.confidence, sizeof r.confidence);
        mix (h, &r.tuningCents, sizeof r.tuningCents);
        mix (h, r.chroma.data(), sizeof (float) * r.chroma.size());
    };
    for (int tonic = 0; tonic < 12; ++tonic)
        for (size_t pi = 0; pi < ps.size(); ++pi)
        {
            const auto clip = chordClip (tonic, ps[pi], ts[(size_t) (tonic + (int) pi) % ts.size()], clipSeed (4242, tonic, (int) pi, 0), 48000.0, 30.0);
            KeyAnalyser a, b;
            for (auto* x : { &a, &b })
            {
                x->configure (48000.0);
                x->setResponse ((tonic + (int) pi) % 2 ? Response::fast : Response::stable);
                x->setSource (Source::mix);
            }
            b.setDrawPastTopBin (true);
            std::uint64_t ha = 14695981039346656037ull, hb = ha;
            bool keyDiffers = false;
            for (size_t pos = 0; pos + 480 <= clip.size(); pos += 480)
            {
                a.push (clip.data() + pos, 480);
                b.push (clip.data() + pos, 480);
                const auto& ra = a.reading();
                const auto& rb = b.reading();
                mixReading (run.read, ra);
                mixReading (run.drawnOn, rb);
                mixReading (ha, ra);
                mixReading (hb, rb);
                ++run.steps;
                if (ra.key != rb.key || ra.minor != rb.minor) { ++run.stepsKeyDiffers; keyDiffers = true; }
                run.maxConfidenceMove = std::max (run.maxConfidenceMove, (double) std::abs (ra.confidence - rb.confidence));
            }
            const auto& ra = a.reading();
            const auto& rb = b.reading();
            const int want = keyIndex (tonic, ps[pi].minor);
            const int gotA = ra.key < 0 ? -1 : keyIndex (ra.key, ra.minor), gotB = rb.key < 0 ? -1 : keyIndex (rb.key, rb.minor);
            ++run.clips;
            run.changed += ha != hb ? 1 : 0;
            run.keyDiffers += keyDiffers ? 1 : 0;
            run.finalDiffers += gotA != gotB ? 1 : 0;
            run.rightAfter += gotA == want ? 1 : 0;
            run.rightBefore += gotB == want ? 1 : 0;
            run.rightThenWrong += gotB == want && gotA != want ? 1 : 0;
            if (ha != hb)
            {
                run.maxTuningCents = std::max (run.maxTuningCents, (double) rb.tuningCents);
                run.minTuningCents = std::min (run.minTuningCents, (double) rb.tuningCents);
                static const char* names[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
                run.changedClips += (run.changedClips.empty() ? "" : ", ") + std::string (names[tonic]) + " " + ps[pi].name;
            }
        }
    return run;
}

inline std::uint64_t chordsInMixFingerprint() { return chordsInMixCompare().read; }
}

#ifdef FINGERPRINT_MAIN
int main()
{
    std::printf ("0x%llxull\n", (unsigned long long) fingerprint::readingFingerprint());
    const auto run = fingerprint::chordsInMixCompare();
    std::printf ("chords in Full Mix 0x%llxull, top bin drawn on 0x%llxull\n", (unsigned long long) run.read, (unsigned long long) run.drawnOn);
    std::printf ("clips %d, changed %d, key differs at any moment %d, final key differs %d, right drawn on %d, right read %d, right then wrong %d, steps key differs %d of %d, tuning in changed clips %.1f to %.1f cents\n",
                 run.clips, run.changed, run.keyDiffers, run.finalDiffers, run.rightBefore, run.rightAfter, run.rightThenWrong, run.stepsKeyDiffers, run.steps, run.minTuningCents, run.maxTuningCents);
    std::printf ("largest confidence difference %.6f; changed clips: %s\n", run.maxConfidenceMove, run.changedClips.c_str());
    return 0;
}
#endif
