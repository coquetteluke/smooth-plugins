#pragma once
// The five characters. Each is a set of behaviours, not a preset: the moves add to the Pitch and Formant Shift controls, and
// Character Amount scales all of them together.
namespace smoothmorph
{
struct Character
{
    const char* name;
    double pitchSemitones;   // added to Pitch
    double formantSemitones; // added to Formant Shift. 2^(st/12) is the ratio on frequency
    double robot;            // 1: output pitch is flattened to Robot Note and formant peaks are sharpened
    double monster;          // 1: full growl (subharmonic from alternating grains, jitter and shimmer)
};

inline constexpr int numCharacters = 5;
inline constexpr Character characters[numCharacters] = {
    { "Natural",  0.0,  0.0, 0.0, 0.0 },
    { "Deep",    -5.0, -3.0, 0.0, 0.0 },  // pitch down 5 semitones, formants x 0.84
    { "Child",    7.0,  4.0, 0.0, 0.0 },  // pitch up 7 semitones, formants x 1.26
    { "Robot",    0.0,  0.0, 1.0, 0.0 },  // pitch constant at Robot Note, formants in place and sharper
    { "Monster", -8.0, -5.0, 0.0, 1.0 },  // pitch down 8 semitones, formants x 0.75, growl
};
} // namespace smoothmorph
