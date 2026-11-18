#pragma once
// Melodic patterns for the Motion module. Each step moves the note by a number of scale steps and octaves, so a pattern stays in key.
// All 16 steps long. Own patterns, written from what this kind of feature does in general.
#include <array>
#include <cstdint>

namespace tunefx
{
struct MotionStep { signed char degrees; signed char octaves; };
using MotionPattern = std::array<MotionStep, 16>;

inline constexpr const char* motionPatternNames[] = { "Trill Up", "Trill Down", "Octave Up", "Octave Down", "Arp Up", "Arp Down", "Stairs", "Ostinato" };
inline constexpr int numMotionPatterns = 8;

inline const MotionPattern& motionPattern (int index) noexcept
{
    static constexpr MotionPattern patterns[numMotionPatterns] = {
        {{ {0,0},{1,0},{0,0},{1,0},{0,0},{1,0},{0,0},{1,0},{0,0},{1,0},{0,0},{1,0},{0,0},{1,0},{0,0},{1,0} }},
        {{ {0,0},{-1,0},{0,0},{-1,0},{0,0},{-1,0},{0,0},{-1,0},{0,0},{-1,0},{0,0},{-1,0},{0,0},{-1,0},{0,0},{-1,0} }},
        {{ {0,0},{0,1},{0,0},{0,1},{0,0},{0,1},{0,0},{0,1},{0,0},{0,1},{0,0},{0,1},{0,0},{0,1},{0,0},{0,1} }},
        {{ {0,0},{0,-1},{0,0},{0,-1},{0,0},{0,-1},{0,0},{0,-1},{0,0},{0,-1},{0,0},{0,-1},{0,0},{0,-1},{0,0},{0,-1} }},
        {{ {0,0},{2,0},{4,0},{0,1},{0,0},{2,0},{4,0},{0,1},{0,0},{2,0},{4,0},{0,1},{0,0},{2,0},{4,0},{0,1} }},
        {{ {0,1},{4,0},{2,0},{0,0},{0,1},{4,0},{2,0},{0,0},{0,1},{4,0},{2,0},{0,0},{0,1},{4,0},{2,0},{0,0} }},
        {{ {0,0},{1,0},{2,0},{3,0},{4,0},{3,0},{2,0},{1,0},{0,0},{1,0},{2,0},{3,0},{4,0},{3,0},{2,0},{1,0} }},
        {{ {0,0},{0,0},{4,0},{0,0},{2,0},{0,0},{4,0},{2,0},{0,0},{0,0},{4,0},{0,0},{5,0},{4,0},{2,0},{1,0} }},
    };
    return patterns[index < 0 || index >= numMotionPatterns ? 0 : index];
}

// The note that many scale steps from a note. Mask has a bit per pitch class from C. With an empty mask, a step is a semitone.
inline int walkScale (int note, int degrees, std::uint16_t mask) noexcept
{
    if (mask == 0 || degrees == 0)
        return note + degrees;
    const int direction = degrees > 0 ? 1 : -1;
    int left = degrees > 0 ? degrees : -degrees, at = note;
    for (int guard = 0; left > 0 && guard < 96; ++guard)
    {
        at += direction;
        if ((mask >> (((at % 12) + 12) % 12)) & 1)
            --left;
    }
    return at;
}
} // namespace tunefx
