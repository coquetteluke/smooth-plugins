#pragma once
// Synthetic test music: chord progressions and melodies in every key, in several timbres. Nothing here ships.
#include "KeyAnalyser.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace corpus
{
inline constexpr double twoPi = 6.283185307179586476925;

struct Timbre
{
    const char* name;
    std::vector<double> partials;  // amplitude of harmonic 1, 2, 3 ...
    int voices;                    // detuned copies of each note
    double detuneCents;            // spread between the outer voices and the centre
    double snrDb;                  // white noise, 200 means none
    bool offsetTuning;             // whole clip sits up to +/-30 cents from A440
    double vibratoCents;           // voice like wobble
};

inline std::vector<double> stack (int n, double power, bool oddOnly = false)
{
    std::vector<double> v;
    for (int h = 1; h <= n; ++h) v.push_back ((oddOnly && h % 2 == 0) ? 0.0 : 1.0 / std::pow ((double) h, power));
    return v;
}

// A sung vowel has strong middle partials, not a falling line.
inline std::vector<double> vowel()
{
    return { 0.6, 1.0, 0.8, 0.35, 0.25, 0.3, 0.25, 0.12, 0.08, 0.05 };
}

inline std::vector<Timbre> timbres()
{
    return {
        { "bright stack",       stack (12, 1.0),       1, 0.0,  200.0, false, 0.0 },
        { "mellow with noise",  stack (6, 2.0),        1, 0.0,  20.0,  false, 0.0 },
        { "hollow with noise",  stack (11, 1.0, true), 1, 0.0,  12.0,  false, 0.0 },
        { "detuned",            stack (10, 1.2),       3, 10.0, 200.0, true,  0.0 },
        { "detuned with noise", stack (8, 1.5),        2, 14.0, 15.0,  true,  0.0 },
    };
}

inline Timbre vocalTimbre() { return { "vowel with vibrato", vowel(), 1, 0.0, 30.0, true, 35.0 }; }

struct Chord { int root; int kind; }; // kind: 0 major, 1 minor, 2 diminished

struct Progression { const char* name; bool minor; std::vector<Chord> chords; };

inline std::vector<Progression> progressions()
{
    return {
        { "I IV V I",     false, { { 0, 0 }, { 5, 0 }, { 7, 0 }, { 0, 0 } } },
        { "I vi IV V",    false, { { 0, 0 }, { 9, 1 }, { 5, 0 }, { 7, 0 } } },
        { "ii V I I",     false, { { 2, 1 }, { 7, 0 }, { 0, 0 }, { 0, 0 } } },
        { "I V vi IV",    false, { { 0, 0 }, { 7, 0 }, { 9, 1 }, { 5, 0 } } },
        { "i iv V i",     true,  { { 0, 1 }, { 5, 1 }, { 7, 0 }, { 0, 1 } } },
        { "i VI III VII", true,  { { 0, 1 }, { 8, 0 }, { 3, 0 }, { 10, 0 } } },
        { "i iv VII III", true,  { { 0, 1 }, { 5, 1 }, { 10, 0 }, { 3, 0 } } },
        { "i VII VI V",   true,  { { 0, 1 }, { 10, 0 }, { 8, 0 }, { 7, 0 } } },
    };
}

struct Note { double start, length; int midi; double amp; };

// Fast additive synth: one rotating phasor per partial, unless the note has vibrato.
inline void renderNotes (std::vector<float>& out, const std::vector<Note>& notes, const Timbre& t, double tuningCents, double sr, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> phase (0.0, twoPi);
    std::vector<double> mix (out.size(), 0.0);
    for (const auto& note : notes)
    {
        const int start = (int) (note.start * sr);
        const int len = (int) (note.length * sr);
        const int attack = (int) (0.008 * sr), release = (int) (0.04 * sr);
        for (int v = 0; v < t.voices; ++v)
        {
            const double spread = t.voices > 1 ? -1.0 + 2.0 * v / (t.voices - 1) : 0.0;
            const double midi = note.midi + (tuningCents + spread * t.detuneCents) * 0.01;
            const double f0 = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
            const double vibRate = 5.0 + 0.8 * phase (rng) / twoPi;
            const double vibPhase = phase (rng);
            for (size_t p = 0; p < t.partials.size(); ++p)
            {
                const double amp = t.partials[p] * note.amp / t.voices;
                if (amp <= 0.0) continue;
                const double f = f0 * (double) (p + 1);
                if (f > sr * 0.45) break;
                double ph = phase (rng);
                if (t.vibratoCents > 0.0)
                {
                    const double w = twoPi * f / sr;
                    for (int i = 0; i < len && start + i < (int) mix.size(); ++i)
                    {
                        const double sec = i / sr;
                        const double wob = std::pow (2.0, t.vibratoCents * 0.01 / 12.0 * std::sin (twoPi * vibRate * sec + vibPhase) * std::min (1.0, sec * 4.0));
                        ph += w * wob;
                        const double env = std::min ({ 1.0, i / (double) attack, (len - i) / (double) release });
                        mix[(size_t) (start + i)] += amp * env * std::sin (ph);
                    }
                    continue;
                }
                const double w = twoPi * f / sr, cr = std::cos (w), ci = std::sin (w);
                double re = std::cos (ph), im = std::sin (ph);
                const double decay = std::exp (-1.0 / (0.9 * sr));
                double level = 1.0;
                for (int i = 0; i < len && start + i < (int) mix.size(); ++i)
                {
                    const double env = std::min ({ 1.0, i / (double) attack, (len - i) / (double) release }) * (0.55 + 0.45 * level);
                    level *= decay;
                    mix[(size_t) (start + i)] += amp * env * im;
                    const double nr = re * cr - im * ci;
                    im = re * ci + im * cr;
                    re = nr;
                }
            }
        }
    }
    double energy = 0.0;
    for (auto x : mix) energy += x * x;
    const double rms = std::sqrt (energy / std::max<size_t> (mix.size(), 1));
    if (t.snrDb < 150.0)
    {
        std::normal_distribution<double> gauss (0.0, rms * std::pow (10.0, -t.snrDb / 20.0));
        for (auto& x : mix) x += gauss (rng);
    }
    double top = 1.0e-9;
    for (auto x : mix) top = std::max (top, std::abs (x));
    const double gain = 0.5 / top;
    for (size_t i = 0; i < mix.size(); ++i) out[i] = (float) (mix[i] * gain);
}

inline int foldInto (int pc, int low)
{
    int m = low + ((pc - low) % 12 + 12) % 12;
    return m;
}

// startAt: which chord of the loop comes first. 0 is the usual start, on the first chord written above.
inline std::vector<Note> chordNotes (int tonic, const Progression& prog, double seconds, double chordLen, int style, int startAt = 0)
{
    std::vector<Note> notes;
    int index = startAt;
    for (double t = 0.0; t < seconds; t += chordLen, ++index)
    {
        const auto& c = prog.chords[(size_t) index % prog.chords.size()];
        const int root = (tonic + c.root) % 12;
        const int third = (root + (c.kind == 0 ? 4 : 3)) % 12;
        const int fifth = (root + (c.kind == 2 ? 6 : 7)) % 12;
        notes.push_back ({ t, chordLen, foldInto (root, 36), 0.8 });
        const int tones[3] = { foldInto (root, 55), foldInto (third, 55), foldInto (fifth, 55) };
        if (style == 0)
        {
            for (int m : tones) notes.push_back ({ t, chordLen, m, 0.45 });
        }
        else
        {
            // Broken chord: tones in turn, ringing to the end of the chord.
            const double step = chordLen / 6.0;
            for (int i = 0; i < 6; ++i)
                notes.push_back ({ t + i * step, chordLen - i * step, tones[i % 3] + (i >= 3 ? 12 : 0), 0.4 });
        }
    }
    return notes;
}

// A made up tune: mostly steps within the scale, phrases of eight notes that land on tonic, fifth or third.
inline std::vector<Note> melodyNotes (int tonic, bool minor, double seconds, std::mt19937& rng)
{
    static constexpr int majorScale[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minorScale[7] = { 0, 2, 3, 5, 7, 8, 10 };
    const int* scale = minor ? minorScale : majorScale;
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    const int base = 60 + (tonic > 7 ? tonic - 12 : tonic);
    std::vector<Note> notes;
    double t = 0.0;
    int degree = 0, inPhrase = 0;
    while (t < seconds)
    {
        const bool phraseEnd = inPhrase == 7;
        if (inPhrase == 0)
        {
            const double u = uni (rng);
            degree = u < 0.5 ? 0 : (u < 0.8 ? 4 : 2);
        }
        else if (phraseEnd)
        {
            const double u = uni (rng);
            const int target = u < 0.6 ? 0 : (u < 0.85 ? 4 : 2);
            const int octave = (int) std::floor (degree / 7.0);
            degree = octave * 7 + target;
        }
        else
        {
            const double u = uni (rng);
            int move = 0;
            if (u < 0.35) move = 1; else if (u < 0.7) move = -1; else if (u < 0.8) move = 2; else if (u < 0.9) move = -2; else if (u < 0.95) move = 4; else move = -3;
            degree = std::clamp (degree + move, -3, 11);
        }
        const int d = ((degree % 7) + 7) % 7;
        const int oct = (int) std::floor (degree / 7.0);
        int pitch = base + 12 * oct + scale[d];
        // In minor the seventh is sometimes raised on the way home.
        if (minor && d == 6 && uni (rng) < 0.4) pitch += 1;
        const double len = phraseEnd ? 0.7 + 0.5 * uni (rng) : 0.22 + 0.32 * uni (rng);
        notes.push_back ({ t, len * 0.92, pitch, 0.8 });
        t += len;
        if (phraseEnd) t += 0.15 + 0.3 * uni (rng);
        inPhrase = (inPhrase + 1) % 8;
    }
    return notes;
}

inline int keyIndex (int tonic, bool minor) { return tonic + (minor ? 12 : 0); }

// Exact, relative or a fifth away in the same scale.
inline bool isRelated (int got, int want)
{
    if (got < 0) return false;
    if (got == want) return true;
    const bool gotMinor = got >= 12, wantMinor = want >= 12;
    const int gt = got % 12, wt = want % 12;
    if (gotMinor == wantMinor) return gt == (wt + 7) % 12 || gt == (wt + 5) % 12;
    return wantMinor ? gt == (wt + 3) % 12 : gt == (wt + 9) % 12;
}

struct Trace
{
    int finalKey = -1;
    double settle = -1.0;       // first time the reading is correct and stays correct to the end
    double firstCorrect = -1.0;
    int changesAfterEight = 0;  // reading moves after 8 s
    int bounces = 0;            // A to B and back to A
    double finalConfidence = 0.0;
    std::vector<std::pair<double, int>> history; // every change of reading
};

// Runs a whole clip through the analyser in small pieces and records the reading over time.
inline Trace analyse (smoothkey::KeyAnalyser& a, const std::vector<float>& audio, double sr, int want)
{
    Trace tr;
    int last = -1, before = -1;
    const int chunk = 512;
    double correctSince = -1.0;
    for (int pos = 0; pos < (int) audio.size(); pos += chunk)
    {
        a.push (audio.data() + pos, std::min (chunk, (int) audio.size() - pos));
        const auto& r = a.reading();
        const int k = r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0);
        const double t = (pos + chunk) / sr;
        if (k == want)
        {
            if (correctSince < 0.0) correctSince = t;
            if (tr.firstCorrect < 0.0) tr.firstCorrect = t;
        }
        else correctSince = -1.0;
        if (k != last)
        {
            tr.history.push_back ({ t, k });
            if (last >= 0 && t > 8.0) ++tr.changesAfterEight;
            if (k == before && last >= 0) ++tr.bounces;
            before = last;
            last = k;
        }
        tr.finalKey = k;
        tr.finalConfidence = r.confidence;
    }
    tr.settle = correctSince;
    return tr;
}

struct Summary
{
    int total = 0, exact = 0, related = 0, bounces = 0, moved = 0;
    std::vector<double> settles;
    double confCorrect = 0.0, confWrong = 0.0; int nWrong = 0;

    void add (const Trace& t, int want)
    {
        ++total;
        if (t.finalKey == want) { ++exact; confCorrect += t.finalConfidence; if (t.settle >= 0.0) settles.push_back (t.settle); }
        else { confWrong += t.finalConfidence; ++nWrong; }
        if (isRelated (t.finalKey, want)) ++related;
        bounces += t.bounces;
        if (t.changesAfterEight > 0) ++moved;
    }
    double exactPercent() const { return total ? 100.0 * exact / total : 0.0; }
    double relatedPercent() const { return total ? 100.0 * related / total : 0.0; }
    double movedPercent() const { return total ? 100.0 * moved / total : 0.0; }
    double percentile (double q) const
    {
        if (settles.empty()) return -1.0;
        auto s = settles;
        std::sort (s.begin(), s.end());
        return s[(size_t) std::min ((double) s.size() - 1, q * (double) s.size())];
    }
    double meanConfCorrect() const { return exact ? confCorrect / exact : 0.0; }
    double meanConfWrong() const { return nWrong ? confWrong / nWrong : 0.0; }
};

// One clip of a chord progression. Each key, progression and timbre gets its own tempo, style and tuning from the seed.
inline std::vector<float> chordClip (int tonic, const Progression& prog, const Timbre& t, unsigned seed, double sr, double seconds, int startAt = 0)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    const double chordLen = 1.5 + uni (rng);
    const int style = uni (rng) < 0.5 ? 0 : 1;
    const double tuning = t.offsetTuning ? -30.0 + 60.0 * uni (rng) : 0.0;
    std::vector<float> out ((size_t) (seconds * sr));
    renderNotes (out, chordNotes (tonic, prog, seconds, chordLen, style, startAt), t, tuning, sr, seed + 1);
    return out;
}

inline std::vector<float> melodyClip (int tonic, bool minor, const Timbre& t, unsigned seed, double sr, double seconds)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    const double tuning = t.offsetTuning ? -30.0 + 60.0 * uni (rng) : 0.0;
    std::vector<float> out ((size_t) (seconds * sr));
    renderNotes (out, melodyNotes (tonic, minor, seconds, rng), t, tuning, sr, seed + 1);
    return out;
}

inline unsigned clipSeed (unsigned base, int a, int b, int c) { return base * 31u + (unsigned) a * 1009u + (unsigned) b * 101u + (unsigned) c * 7u; }
} // namespace corpus
