#pragma once
// Keys, scales and chords as a bit per pitch class, with bit 0 = C. Plain functions, so the tests can walk every note.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>

namespace smoothharmony
{
inline constexpr const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

enum ScaleType
{
    scaleChromatic = 0, scaleMajor, scaleMinor, scaleHarmonicMinor, scaleMelodicMinor, scaleMajorPentatonic, scaleMinorPentatonic,
    scaleBlues, scaleDorian, scaleMixolydian, numScales
};

inline constexpr const char* scaleNames[numScales] = { "Chromatic", "Major", "Minor", "Harmonic Minor", "Melodic Minor",
                                                       "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Mixolydian" };

enum ChordType { chordMajor = 0, chordMinor, chordSeven, chordMajorSeven, chordMinorSeven, chordSus2, chordSus4, chordDiminished, chordAugmented, chordFifth, numChords };

inline constexpr const char* chordNames[numChords] = { "Major", "Minor", "Dominant 7", "Major 7", "Minor 7", "Sus 2", "Sus 4", "Diminished", "Augmented", "Fifth" };

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

inline std::uint16_t chordPattern (int chord) noexcept
{
    switch (chord)
    {
        case chordMinor:      return intervals ({ 0, 3, 7 });
        case chordSeven:      return intervals ({ 0, 4, 7, 10 });
        case chordMajorSeven: return intervals ({ 0, 4, 7, 11 });
        case chordMinorSeven: return intervals ({ 0, 3, 7, 10 });
        case chordSus2:       return intervals ({ 0, 2, 7 });
        case chordSus4:       return intervals ({ 0, 5, 7 });
        case chordDiminished: return intervals ({ 0, 3, 6 });
        case chordAugmented:  return intervals ({ 0, 4, 8 });
        case chordFifth:      return intervals ({ 0, 7 });
        default:              return intervals ({ 0, 4, 7 });
    }
}

// A pattern moved up to its root, in absolute pitch classes.
inline std::uint16_t rotateMask (std::uint16_t pattern, int root) noexcept
{
    const std::uint32_t p = pattern;
    const int r = ((root % 12) + 12) % 12;
    return (std::uint16_t) (((p << r) | (p >> (12 - r))) & 0x0FFFu);
}

inline std::uint16_t scaleMask (int root, int scale) noexcept { return rotateMask (scalePattern (scale), root); }
inline std::uint16_t chordMask (int root, int chord) noexcept { return rotateMask (chordPattern (chord), root); }

inline bool inMask (int note, std::uint16_t mask) noexcept { return ((mask >> (((note % 12) + 12) % 12)) & 1) != 0; }

inline int nearestInMask (double p, std::uint16_t mask) noexcept
{
    const int centre = (int) std::lround (p);
    for (int distance = 0; distance <= 12; ++distance)
    {
        const int up = centre + distance, down = centre - distance;
        const bool hasUp = inMask (up, mask), hasDown = inMask (down, mask);
        if (hasUp && hasDown)
            return std::abs ((double) up - p) < std::abs ((double) down - p) ? up : down;
        if (hasUp) return up;
        if (hasDown) return down;
    }
    return centre;
}

inline int popCount (std::uint16_t mask) noexcept
{
    int n = 0;
    for (int i = 0; i < 12; ++i) n += (mask >> i) & 1;
    return n;
}

// Interval name a number of semitones stands for: 0 unison, 1 second, 2 third and so on, plus sevens for each octave.
inline int scaleSteps (int semitones) noexcept
{
    static constexpr int steps[12] = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };
    const int size = std::abs (semitones);
    const int count = steps[size % 12] + 7 * (size / 12);
    return semitones < 0 ? -count : count;
}

// The note a voice sings for a lead on a scale note. In a seven note scale the interval is counted in scale steps, so a major third on
// the third degree comes out as a minor third. In other scales there are no such names, so it goes to the scale note nearest that many semitones.
inline int harmonyNote (int leadNote, int semitones, std::uint16_t mask) noexcept
{
    const int size = popCount (mask);
    if (size == 0 || size == 12 || semitones == 0)
        return leadNote + semitones;
    if (size == 7)
    {
        int steps = scaleSteps (semitones), note = leadNote;
        const int direction = steps < 0 ? -1 : 1;
        for (steps = std::abs (steps); steps > 0; --steps)
        {
            note += direction;
            while (! inMask (note, mask)) note += direction;
        }
        return note;
    }
    const int aim = leadNote + semitones, direction = semitones < 0 ? -1 : 1;
    for (int distance = 0; distance <= 12; ++distance)
    {
        // Ties go away from the lead, so an interval never collapses onto the lead's own note when another is as close.
        const int far = aim + direction * distance, near = aim - direction * distance;
        if (inMask (far, mask)) return far;
        if (inMask (near, mask)) return near;
    }
    return aim;
}

inline const char* intervalName (int semitones) noexcept
{
    static constexpr const char* names[8] = { "Unison", "2nd", "3rd", "4th", "5th", "6th", "7th", "Octave" };
    const int steps = std::abs (scaleSteps (semitones));
    if (steps == 0) return names[0];
    if (steps % 7 == 0) return names[7];
    return names[steps % 7];
}
} // namespace smoothharmony
