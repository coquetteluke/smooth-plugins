#pragma once
// Keys and scales as a bit per pitch class, with bit 0 = C.
#include <array>
#include <cstdint>

namespace smoothtune
{
inline constexpr const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

enum ScaleType
{
    scaleChromatic = 0, scaleMajor, scaleMinor, scaleHarmonicMinor, scaleMelodicMinor, scaleMajorPentatonic, scaleMinorPentatonic,
    scaleBlues, scaleDorian, scaleMixolydian, scaleCustom, numScales
};

inline constexpr const char* scaleNames[numScales] = { "Chromatic", "Major", "Minor", "Harmonic Minor", "Melodic Minor",
                                                       "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Mixolydian", "Custom" };

// Semitones from the root, written the way a musician reads them. Bit n = n semitones up.
constexpr std::uint16_t intervals (std::initializer_list<int> list)
{
    std::uint16_t m = 0;
    for (int i : list) m = (std::uint16_t) (m | (1u << i));
    return m;
}

inline std::uint16_t scalePattern (int scale) noexcept
{
    switch (scale)
    {
        case scaleMajor:           return intervals ({ 0, 2, 4, 5, 7, 9, 11 });
        case scaleMinor:           return intervals ({ 0, 2, 3, 5, 7, 8, 10 });
        case scaleHarmonicMinor:   return intervals ({ 0, 2, 3, 5, 7, 8, 11 });
        case scaleMelodicMinor:    return intervals ({ 0, 2, 3, 5, 7, 9, 11 });
        case scaleMajorPentatonic: return intervals ({ 0, 2, 4, 7, 9 });
        case scaleMinorPentatonic: return intervals ({ 0, 3, 5, 7, 10 });
        case scaleBlues:           return intervals ({ 0, 3, 5, 6, 7, 10 });
        case scaleDorian:          return intervals ({ 0, 2, 3, 5, 7, 9, 10 });
        case scaleMixolydian:      return intervals ({ 0, 2, 4, 5, 7, 9, 10 });
        default:                   return 0x0FFF;
    }
}

// Mask of pitch classes in absolute terms, bit 0 = C. Custom is handled by the caller.
inline std::uint16_t scaleMask (int root, int scale) noexcept
{
    const std::uint32_t pattern = scalePattern (scale);
    const int r = ((root % 12) + 12) % 12;
    return (std::uint16_t) (((pattern << r) | (pattern >> (12 - r))) & 0x0FFFu);
}
} // namespace smoothtune
