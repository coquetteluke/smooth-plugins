#pragma once
// A number that changes if any reading the analyser gives changes by even one bit. Proves the window's extra read-only data does
// not alter detection. The expected value is from the analyser as last changed on purpose: 1.6.0, for the lone voice reading. Half of
// these clips run in Single Voice, which 1.6.0 reads differently; the Full Mix half is pinned to 1.5.1 by expectedChordsInMix below.
// Standalone: clang++ -std=c++17 -O2 -Isrc -Itests -x c++ -DFINGERPRINT_MAIN tests/ReadingFingerprint.h -o build/ReadingFingerprint
#include "KeyCorpus.h"
#include <cstdint>
#include <cstring>

namespace fingerprint
{
inline constexpr std::uint64_t expectedBefore = 0x96fd1a25267dfa4bull;

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
// chroma value over 96 chord clips (12 keys, 8 progressions, every timbre, Stable and Fast in turn), 30 s each. The expected
// value is from 1.5.1 (commit f0f323d), whose detection is 1.5.0's.
inline constexpr std::uint64_t expectedChordsInMix = 0x6d8feb3810100d9full;

inline std::uint64_t chordsInMixFingerprint()
{
    using namespace corpus;
    using namespace smoothkey;
    std::uint64_t h = 14695981039346656037ull;
    const auto ts = timbres();
    const auto ps = progressions();
    for (int tonic = 0; tonic < 12; ++tonic)
        for (size_t pi = 0; pi < ps.size(); ++pi)
        {
            const auto clip = chordClip (tonic, ps[pi], ts[(size_t) (tonic + (int) pi) % ts.size()], clipSeed (4242, tonic, (int) pi, 0), 48000.0, 30.0);
            KeyAnalyser a;
            a.configure (48000.0);
            a.setResponse ((tonic + (int) pi) % 2 ? Response::fast : Response::stable);
            a.setSource (Source::mix);
            for (size_t pos = 0; pos + 480 <= clip.size(); pos += 480)
            {
                a.push (clip.data() + pos, 480);
                const auto& r = a.reading();
                const std::int32_t ints[] = { r.key, r.minor ? 1 : 0, r.altKey, r.altMinor ? 1 : 0, r.signal ? 1 : 0 };
                mix (h, ints, sizeof ints);
                mix (h, &r.confidence, sizeof r.confidence);
                mix (h, &r.tuningCents, sizeof r.tuningCents);
                mix (h, r.chroma.data(), sizeof (float) * r.chroma.size());
            }
        }
    return h;
}
}

#ifdef FINGERPRINT_MAIN
int main()
{
    std::printf ("0x%llxull\n", (unsigned long long) fingerprint::readingFingerprint());
    std::printf ("chords in Full Mix 0x%llxull\n", (unsigned long long) fingerprint::chordsInMixFingerprint());
    return 0;
}
#endif
