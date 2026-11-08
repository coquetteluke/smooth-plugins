#pragma once
// A number that changes if any reading the analyser gives changes by even one bit. Proves the window's extra read-only data does
// not alter detection. The expected value is from the analyser as last changed on purpose: 1.3.0, for odd harmonic notes, notes outside the scale and a surer first reading.
// Standalone: clang++ -std=c++17 -O2 -Isrc -Itests -x c++ -DFINGERPRINT_MAIN tests/ReadingFingerprint.h -o build/ReadingFingerprint
#include "KeyCorpus.h"
#include <cstdint>
#include <cstring>

namespace fingerprint
{
inline constexpr std::uint64_t expectedBefore = 0xe3d6ee170b9f476aull;

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
}

#ifdef FINGERPRINT_MAIN
int main() { std::printf ("0x%llxull\n", (unsigned long long) fingerprint::readingFingerprint()); return 0; }
#endif
