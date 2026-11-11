#pragma once
// Phrase starts. Included by PluginTest.cpp, inside its namespace. Limits are the onset* and noTrigger* ones in PassLimits.h.
//
// Luke on 1.3.1, on his own vocals in Logic: "choir loft misses when vocals start pretty often". The old onset test only covered a unison
// voice on a clean synthetic start. This one makes the starts a real singer makes: hard and soft, scoops from below, breathy, creaky,
// consonants into the vowel (s, t, m, l), vibrato from the first moment, short first notes, notes after a short gap, room reverb, a quiet
// take and a bright one. Each at every Input Type, at three pitches in its range, at the default preset, at Tight, and at the default
// with Humanize 0. The two default voices (a third and a fifth up) are panned hard apart, with the lead off, so one stereo render gives
// each voice on its own channel. Pan changes nothing but the output gains.
//
// Per voice and start:
//  - sounded: within onsetOnNoteCents of its note and within onsetLevelDb of its own steady level, held onsetHoldMs, before the note ends.
//    Level is the voice against the part of the lead it is reading (latency plus its lag), against the same ratio in the body of the
//    phrase's last note. So a soft start that the lead itself fades into is not counted late, only a voice that lags its lead.
//  - time to sound: from when the lead is on its note, minus the lag the voice is set to stand behind the lead at that moment.
//  - wrong first note: before it sounds, a stretch of onsetWrongMs or longer, no quieter than onsetWrongAudibleDb against its steady level,
//    on a pitch more than onsetWrongCents from its note, from the lead's own pitch (a voice reading the lead unshifted through a consonant
//    doubles the lead, which is heard as the lead), and, while the lead is still scooping, from its note bent by the lead's scoop.
// Pitch of the voice is read by correlation in the test, sharing no code with the plug-in.

namespace onsets
{
inline const voice::Vowel nasalM { { 250, 1100, 2300, 3400, 4500 }, { 100, 200, 250, 300, 400 }, "m" };
inline const voice::Vowel liquidL { { 360, 1300, 2700, 3400, 4500 }, { 80, 150, 200, 250, 300 }, "l" };

enum NoiseKind { aspiration = 0, hiss, burst };

// One stretch of the lead. Levels are linear and against a full voice, ramped across the piece.
struct Piece
{
    double sec = 0.0;
    int midi = -1;                       // the note meant, -1 for none
    double ampFrom = 0.0, ampTo = 0.0;   // voiced part
    double noiseFrom = 0.0, noiseTo = 0.0;
    int noise = aspiration;
    const voice::Vowel* vowel = &voice::vowelA;
    double scoopCents = 0.0, scoopSec = 0.0;
    double vibratoCents = 0.0;
    bool creak = false;                  // vocal fry: irregular pulses around 55 Hz, alternately strong and weak. Not a note.
    double hissHz = 4500.0;
};

inline Piece silence (double sec) { Piece p; p.sec = sec; return p; }
inline Piece note (double sec, int midi, const voice::Vowel& v, double from = 1.0, double to = 1.0)
{
    Piece p; p.sec = sec; p.midi = midi; p.vowel = &v; p.ampFrom = from; p.ampTo = to; return p;
}

struct Lead
{
    std::vector<float> x;
    std::vector<float> voiced;             // the voiced part alone: what a voice's level is measured against, so breath noise in the lead does not count
    std::vector<double> f0, noteHz, amp;   // per sample: true pitch (0 when not voiced), the note meant (0 for none), voiced level
};

struct Res
{
    double a1 = 0, a2 = 0, b0 = 1, y1 = 0, y2 = 0;
    void set (double hz, double bw)   // keeps its state, so a vowel can change under a sound
    {
        const double r = std::exp (-M_PI * bw / SR), c = 2.0 * r * std::cos (2.0 * M_PI * hz / SR);
        a1 = c; a2 = -r * r; b0 = 1.0 - c + r * r;
    }
    double process (double x) { const double y = b0 * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

// The voiced part from the same glottal flow model as VoiceSynth.h, but with level, noise, vowel and pitch moving piece by piece.
inline Lead build (const std::vector<Piece>& pieces, double levelDb, unsigned seed, bool bright = false, bool reverb = false)
{
    size_t n = 0;
    for (const auto& p : pieces) n += (size_t) std::lround (p.sec * SR);
    Lead L;
    L.x.assign (n, 0.0f); L.f0.assign (n, 0.0); L.noteHz.assign (n, 0.0); L.amp.assign (n, 0.0);
    std::vector<double> V (n, 0.0), N (n, 0.0), vGain (n, 0.0), nGain (n, 0.0);
    std::mt19937 gen (seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    std::uniform_real_distribution<double> uni (-1.0, 1.0);

    constexpr int over = 4;
    const double osr = SR * over, w = 2.0 * M_PI * 0.42 * SR / osr, cw = std::cos (w), sw = std::sin (w);
    const double al1 = sw / (2.0 * 0.541), al2 = sw / (2.0 * 1.307);
    const double bA[3] = { (1 - cw) / 2 / (1 + al1), (1 - cw) / (1 + al1), (1 - cw) / 2 / (1 + al1) }, aA[2] = { -2 * cw / (1 + al1), (1 - al1) / (1 + al1) };
    const double bB[3] = { (1 - cw) / 2 / (1 + al2), (1 - cw) / (1 + al2), (1 - cw) / 2 / (1 + al2) }, aB[2] = { -2 * cw / (1 + al2), (1 - al2) / (1 + al2) };
    double z1a = 0, z2a = 0, z1b = 0, z2b = 0;
    Res vr[5], nr[5];
    // s and sh: second order high pass
    double h1 = 0, h2 = 0, hx1 = 0, hx2 = 0;
    double phase = 0.0, lastFlow = 0.0, creakScale = 1.0, creakAmp = 1.0, vibPhase = 0.0;
    bool creakHigh = false;
    constexpr double tp = 0.42, tn = 0.16;

    size_t at = 0;
    for (const auto& p : pieces)
    {
        const size_t len = (size_t) std::lround (p.sec * SR);
        for (int i = 0; i < 5; ++i) { vr[i].set (p.vowel->f[i], p.vowel->bw[i]); nr[i].set (p.vowel->f[i], p.vowel->bw[i]); }
        const double hw = 2.0 * M_PI * p.hissHz / SR, hc = std::cos (hw), halpha = std::sin (hw) / (2.0 * 0.9), ha0 = 1.0 + halpha;
        const double hb0 = (1.0 + hc) * 0.5 / ha0, hb1 = -(1.0 + hc) / ha0, ha1 = -2.0 * hc / ha0, ha2 = (1.0 - halpha) / ha0;
        const double hz0 = p.midi >= 0 ? noteHz (p.midi) : 0.0;
        const size_t start = at;
        double vSum = 0.0, nSum = 0.0;
        for (size_t k = 0; k < len; ++k, ++at)
        {
            const double t = (double) k / SR, frac = len > 1 ? (double) k / (double) (len - 1) : 0.0;
            const double a = p.ampFrom + (p.ampTo - p.ampFrom) * frac;
            const double nz = p.noiseFrom + (p.noiseTo - p.noiseFrom) * frac;
            double f = 0.0;
            if (p.creak) f = 55.0 * creakScale;
            else if (hz0 > 0.0)
            {
                double c = p.vibratoCents * std::sin (vibPhase);
                if (t < p.scoopSec) { const double q = 1.0 - t / p.scoopSec; c += p.scoopCents * q * q; }
                f = hz0 * std::pow (2.0, c / 1200.0);
            }
            vibPhase += 2.0 * M_PI * 5.5 / SR;
            double y = 0.0;
            for (int o = 0; o < over; ++o)
            {
                double x = 0.0;
                if (f > 0.0)
                {
                    phase += f / osr;
                    if (phase >= 1.0)
                    {
                        phase -= 1.0;
                        if (p.creak) { creakScale = 1.0 + 0.3 * uni (gen); creakHigh = ! creakHigh; creakAmp = creakHigh ? 1.0 : 0.45; }
                    }
                    double flow = 0.0;
                    if (phase < tp) flow = 0.5 * (1.0 - std::cos (M_PI * phase / tp));
                    else if (phase < tp + tn) flow = std::cos (0.5 * M_PI * (phase - tp) / tn);
                    x = (flow - lastFlow) * osr / SR * (p.creak ? creakAmp : 1.0);
                    lastFlow = flow;
                }
                const double ya = bA[0] * x + z1a; z1a = bA[1] * x - aA[0] * ya + z2a; z2a = bA[2] * x - aA[1] * ya;
                y = bB[0] * ya + z1b; z1b = bB[1] * ya - aB[0] * y + z2b; z2b = bB[2] * ya - aB[1] * y;
            }
            for (auto& r : vr) y = r.process (y);
            V[at] = y;
            vGain[at] = f > 0.0 ? a : 0.0;
            L.amp[at] = f > 0.0 && ! p.creak ? a : 0.0;
            L.f0[at] = f > 0.0 && a > 0.0 ? f : 0.0;
            L.noteHz[at] = hz0;
            if (f > 0.0) vSum += y * y;

            const double white = gauss (gen);
            double e = white;
            if (p.noise == aspiration) { for (auto& r : nr) e = r.process (e); }
            else if (p.noise == hiss)
            {
                const double out = hb0 * white + hb1 * hx1 + hb0 * hx2 - ha1 * h1 - ha2 * h2;
                hx2 = hx1; hx1 = white; h2 = h1; h1 = out; e = out;
            }
            N[at] = e;
            nGain[at] = nz;
            nSum += e * e;
        }
        // Each piece's voiced and noise parts at unit level, so the levels above mean the same thing for every vowel and noise
        const double vr0 = std::sqrt (vSum / std::max<size_t> (len, 1)), nr0 = std::sqrt (nSum / std::max<size_t> (len, 1));
        for (size_t k = start; k < at; ++k)
        {
            vGain[k] = vr0 > 1.0e-12 ? vGain[k] / vr0 : 0.0;
            nGain[k] = nr0 > 1.0e-12 ? nGain[k] / nr0 : 0.0;
        }
    }
    // Gains move over about 3 ms, so a change of piece is not a click of its own
    const double k3 = 1.0 - std::exp (-1.0 / (0.003 * SR));
    double gv = 0.0, gn = 0.0;
    std::vector<double> mix (n, 0.0), voicedOnly (n, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        gv += k3 * (vGain[i] - gv);
        gn += k3 * (nGain[i] - gn);
        voicedOnly[i] = gv * V[i];
        mix[i] = voicedOnly[i] + gn * N[i];
    }
    double wetGain = 0.0;   // set on the whole lead, then used for the voiced part too
    const auto colour = [&] (std::vector<double>& sig)
    {
        if (bright)
        {
            // A bright singer or a bright condenser: +10 dB shelf above about 3 kHz
            double low = 0.0;
            const double kl = 1.0 - std::exp (-2.0 * M_PI * 3000.0 / SR);
            for (auto& s : sig) { low += kl * (s - low); s = s + 2.16 * (s - low); }
        }
        if (reverb)
        {
            // A small room: four combs and two all passes, RT60 0.6 s, wet 10 dB under the dry (set on the whole lead, used for both)
            const int combMs[4] = { 297, 371, 411, 437 };
            std::vector<double> wet (n, 0.0);
            for (int c = 0; c < 4; ++c)
            {
                const int d = (int) (combMs[c] * 0.0001 * SR);
                const double g = std::pow (10.0, -3.0 * (combMs[c] * 0.0001) / 0.6);
                std::vector<double> line ((size_t) d, 0.0);
                int pos = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    const double outv = line[(size_t) pos];
                    line[(size_t) pos] = sig[i] + g * outv;
                    pos = (pos + 1) % d;
                    wet[i] += 0.25 * outv;
                }
            }
            for (const double apMs : { 5.0, 1.7 })
            {
                const int d = (int) (apMs * 0.001 * SR);
                std::vector<double> line ((size_t) d, 0.0);
                int pos = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    const double b = line[(size_t) pos], in = wet[i] + 0.7 * b;
                    line[(size_t) pos] = in;
                    pos = (pos + 1) % d;
                    wet[i] = b - 0.7 * in;
                }
            }
            if (&sig == &mix)
            {
                double dryE = 0.0, wetE = 0.0;
                for (size_t i = 0; i < n; ++i) { dryE += sig[i] * sig[i]; wetE += wet[i] * wet[i]; }
                wetGain = std::sqrt (dryE / std::max (wetE, 1.0e-20)) * std::pow (10.0, -10.0 / 20.0);
            }
            for (size_t i = 0; i < n; ++i) sig[i] += wetGain * wet[i];
        }
    };
    colour (mix);
    colour (voicedOnly);
    // Level: the last note's body at levelDb
    long lastFrom = -1, lastTo = -1;
    for (long i = (long) n - 1; i >= 0; --i)
        if (L.f0[(size_t) i] > 0.0) { if (lastTo < 0) lastTo = i; lastFrom = i; }
        else if (lastTo >= 0) break;
    double e = 0.0;
    for (long i = lastFrom; i <= lastTo && lastFrom >= 0; ++i) e += mix[(size_t) i] * mix[(size_t) i];
    const double rmsLast = std::sqrt (e / std::max (1L, lastTo - lastFrom + 1));
    const double g = std::pow (10.0, levelDb / 20.0) / std::max (rmsLast, 1.0e-12);
    L.voiced.assign (n, 0.0f);
    for (size_t i = 0; i < n; ++i) { L.x[i] = (float) (g * mix[i]); L.voiced[i] = (float) (g * voicedOnly[i]); }
    return L;
}

// A note as the lead sings it: where its voicing is first audible, where it is on its note, where it ends. Input samples.
struct Run { long onset = -1, arrive = -1, end = -1; int midi = -1; };

inline std::vector<Run> runsOf (const Lead& L)
{
    std::vector<Run> runs;
    const long n = (long) L.x.size();
    long i = 0;
    while (i < n)
    {
        if (L.noteHz[(size_t) i] <= 0.0) { ++i; continue; }
        const double hz = L.noteHz[(size_t) i];
        long j = i;
        double top = 0.0;
        while (j < n && L.noteHz[(size_t) j] == hz) { top = std::max (top, L.amp[(size_t) j]); ++j; }
        Run r;
        r.end = j - 1;
        r.midi = (int) std::lround (69.0 + 12.0 * std::log2 (hz / 440.0));
        for (long k = i; k < j; ++k)
            if (L.f0[(size_t) k] > 0.0 && L.amp[(size_t) k] >= 0.25 * top) { r.onset = k; break; }
        for (long k = std::max (r.onset, i); k < j && r.onset >= 0; ++k)
            if (L.f0[(size_t) k] > 0.0 && std::abs (cents (L.f0[(size_t) k], hz)) <= 50.0) { r.arrive = k; break; }
        if (r.onset >= 0 && r.arrive >= 0) runs.push_back (r);
        i = j;
    }
    return runs;
}

inline int diatonic (int midi, int steps)   // C major, steps up the scale; midi must be in the scale
{
    static const int major[7] = { 0, 2, 4, 5, 7, 9, 11 };
    const int pc = midi % 12, oct = midi / 12;
    int idx = 0;
    for (int i = 0; i < 7; ++i) if (major[i] == pc) idx = i;
    const int j = idx + steps;
    const int o = j >= 0 ? j / 7 : -((6 - j) / 7);
    return (oct + o) * 12 + major[((j % 7) + 7) % 7];
}

// Which runs of a kind are measured: the phrase's first note, and for the gap kinds the note after the gap as well.
struct Kind { const char* name; std::vector<int> measured; };

inline Lead makeKind (int kind, int midi, unsigned seed, std::vector<int>& measured)
{
    measured = { 0 };
    const int up = diatonic (midi, 1), down = diatonic (midi, -1), third = diatonic (midi, 2);
    std::vector<Piece> ps { silence (0.4) };
    double level = -18.0;
    bool bright = false, reverb = false;
    switch (kind)
    {
        case 0: ps.push_back (note (0.6, midi, voice::vowelA)); break;                                   // hard
        case 1: ps.push_back (note (0.15, midi, voice::vowelO, 0.0, 1.0)); ps.push_back (note (0.5, midi, voice::vowelO)); break; // soft
        case 2: { auto p = note (0.6, midi, voice::vowelE); p.scoopCents = -100.0; p.scoopSec = 0.08; ps.push_back (p); break; }   // scoop 1 st
        case 3: { auto p = note (0.6, midi, voice::vowelA); p.scoopCents = -200.0; p.scoopSec = 0.12; ps.push_back (p); break; }   // scoop 2 st
        case 4:                                                                                          // breathy
        {
            Piece air = silence (0.1); air.noiseFrom = 0.05; air.noiseTo = 0.3; air.vowel = &voice::vowelA; ps.push_back (air);
            auto in = note (0.08, midi, voice::vowelA, 0.0, 1.0); in.noiseFrom = 0.3; in.noiseTo = 0.25; ps.push_back (in);
            auto body = note (0.5, midi, voice::vowelA); body.noiseFrom = 0.25; body.noiseTo = 0.06; ps.push_back (body);
            break;
        }
        case 5: { Piece fry = silence (0.08); fry.creak = true; fry.ampFrom = 0.3; fry.ampTo = 0.5; ps.push_back (fry); ps.push_back (note (0.6, midi, voice::vowelA)); break; } // creaky
        case 6: { Piece s = silence (0.09); s.noise = hiss; s.noiseFrom = s.noiseTo = 0.5; s.vowel = &voice::vowelI; ps.push_back (s); ps.push_back (note (0.6, midi, voice::vowelI)); break; } // s
        case 7:                                                                                          // t
        {
            Piece b = silence (0.006); b.noise = burst; b.noiseFrom = 0.8; b.noiseTo = 0.4; ps.push_back (b);
            Piece asp = silence (0.03); asp.noiseFrom = 0.3; asp.noiseTo = 0.15; ps.push_back (asp);
            ps.push_back (note (0.6, midi, voice::vowelA));
            break;
        }
        case 8: ps.push_back (note (0.08, midi, nasalM, 0.3, 0.3)); ps.push_back (note (0.6, midi, voice::vowelO)); break;     // m
        case 9: ps.push_back (note (0.06, midi, liquidL, 0.5, 0.5)); ps.push_back (note (0.6, midi, voice::vowelA)); break;    // l
        case 10: { auto p = note (0.7, midi, voice::vowelO); p.vibratoCents = 50.0; ps.push_back (p); break; }                  // vibrato
        case 11: ps.push_back (note (0.15, midi, voice::vowelE)); ps.push_back (note (0.6, up, voice::vowelE)); break;          // 150 ms
        case 12: ps.push_back (note (0.25, midi, voice::vowelA)); ps.push_back (note (0.6, down, voice::vowelA)); break;        // 250 ms
        case 13: ps.push_back (note (0.4, midi, voice::vowelA)); ps.push_back (silence (0.1)); ps.push_back (note (0.6, third, voice::vowelE)); measured = { 1 }; break; // gap
        case 14: reverb = true; ps.push_back (note (0.4, midi, voice::vowelA)); ps.push_back (silence (0.1)); ps.push_back (note (0.6, third, voice::vowelE)); measured = { 0, 1 }; break; // reverb
        case 15: level = -40.0; ps.push_back (note (0.6, midi, voice::vowelA)); break;                   // quiet
        case 16: bright = true; ps.push_back (note (0.6, midi, voice::vowelI)); break;                    // bright
        default: break;
    }
    ps.push_back (silence (0.25));
    return build (ps, level, seed, bright, reverb);
}

inline const char* kindNames[] = { "hard start", "soft start", "scoop 1 st", "scoop 2 st", "breathy start", "creaky start", "s into vowel",
                                   "t into vowel", "m into vowel", "l into vowel", "vibrato at once", "first note 150 ms", "first note 250 ms",
                                   "after 100 ms gap", "room reverb", "quiet take", "bright take" };
constexpr int numKinds = 17;

struct Out { std::array<std::vector<float>, 2> ch; std::array<std::vector<float>, 2> lagMs; int block = 128; int latency = 0; };

inline Out run (PluginProcessor& p, const std::vector<float>& x)
{
    Out o;
    o.latency = p.getLatencySamples();
    const int n = (int) x.size();
    juce::AudioBuffer<float> buf (2, o.block);
    juce::MidiBuffer midi;
    for (auto& c : o.ch) c.assign ((size_t) n, 0.0f);
    for (int pos = 0; pos < n; pos += o.block)
    {
        const int len = std::min (o.block, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy_n (x.data() + pos, len, buf.getWritePointer (c));
        midi.clear();
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c) std::copy_n (buf.getReadPointer (c), len, o.ch[(size_t) c].data() + pos);
        const auto r = p.getReadings();
        for (int v = 0; v < 2; ++v) o.lagMs[(size_t) v].push_back (r.voiceLagMs[(size_t) v]);
    }
    return o;
}

// 0 default, 1 Tight, 2 default with Humanize 0. Voices hard left and right, lead off.
inline std::unique_ptr<PluginProcessor> setup (int variant, int type, bool consonantsOff = false)
{
    auto p = make();
    if (variant == 1) p->setCurrentProgram (tightPreset);
    if (variant == 2) setParam (*p, "humanize", 0.0f);
    setParam (*p, "inputType", (float) type);
    setParam (*p, "leadLevel", -60.0f);
    setParam (*p, vid (0, "Pan"), -100.0f);
    setParam (*p, vid (1, "Pan"), 100.0f);
    if (consonantsOff) { setParam (*p, "consonants", 0.0f); setParam (*p, "breath", 0.0f); }
    prepare (*p, 2, SR, 128);
    return p;
}
inline const char* variantNames[] = { "default", "Tight", "default, Humanize 0" };

inline double rmsAround (const std::vector<float>& s, long centre, int half)
{
    const long a = std::max (0L, centre - half), b = std::min ((long) s.size(), centre + half);
    if (b <= a) return 0.0;
    double e = 0.0;
    for (long i = a; i < b; ++i) e += (double) s[(size_t) i] * s[(size_t) i];
    return std::sqrt (e / (double) (b - a));
}

inline double lagSamplesAt (const Out& o, int v, long at)
{
    const size_t b = (size_t) std::max (0L, at) / (size_t) o.block;
    const auto& l = o.lagMs[(size_t) v];
    return (b < l.size() ? l[b] : (l.empty() ? 0.0f : l.back())) * 0.001 * SR;
}

// Voice against the part of the lead it reads, in dB, at an output sample
inline double relativeDb (const Out& o, int v, const std::vector<float>& x, long at)
{
    const int half = (int) (0.005 * SR);
    const long in = at - o.latency - (long) std::lround (lagSamplesAt (o, v, at));
    return toDb (rmsAround (o.ch[(size_t) v], at, half)) - toDb (rmsAround (x, in, half));
}

// Pitch anywhere within 8 semitones of an expected one, shortest strong period first. 0 if nothing periodic.
inline double freePitch (const std::vector<float>& y, long centre, double aroundHz)
{
    const double lo = aroundHz * std::pow (2.0, -8.0 / 12.0), hi = aroundHz * std::pow (2.0, 8.0 / 12.0);
    const int tMin = std::max (4, (int) std::floor (SR / hi)), tMax = (int) std::ceil (SR / lo);
    const int W = 2 * tMax, stride = std::max (1, tMax / 150);
    const long start = centre - (W + tMax) / 2;
    if (start < 0 || start + W + tMax + 2 >= (long) y.size()) return 0.0;
    const float* a = y.data() + start;
    double ea = 0.0;
    for (int i = 0; i < W; i += stride) ea += (double) a[i] * a[i];
    std::vector<double> r ((size_t) tMax + 3, -2.0);
    double best = -2.0;
    for (int lag = tMin - 1; lag <= tMax + 1; ++lag)
    {
        double c = 0.0, eb = 0.0;
        for (int i = 0; i < W; i += stride) { c += (double) a[i] * a[i + lag]; eb += (double) a[i + lag] * a[i + lag]; }
        const double den = std::sqrt (ea * eb);
        r[(size_t) lag] = den > 1.0e-12 ? c / den : 0.0;
        if (lag >= tMin && lag <= tMax) best = std::max (best, r[(size_t) lag]);
    }
    if (best < 0.5) return 0.0;
    for (int lag = tMin; lag <= tMax; ++lag)
        if (r[(size_t) lag] >= 0.9 * best && r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1])
        {
            const double p0 = r[(size_t) lag - 1], p1 = r[(size_t) lag], p2 = r[(size_t) lag + 1], den = p0 - 2.0 * p1 + p2;
            return SR / (lag + (std::abs (den) > 1.0e-12 ? 0.5 * (p0 - p2) / den : 0.0));
        }
    return 0.0;
}

struct Result { bool sounded = false, wrong = false; double ms = 1.0e9, lagMs = 0.0, past = 1.0e9, wrongCents = 0.0; };

// The voice's steady level against the lead, in the body of the lead's last note
inline double steadyDb (const Out& o, int v, const Lead& L, const Run& last)
{
    std::vector<double> d;
    for (long i = last.arrive + (long) (0.25 * SR); i < last.end - (long) (0.05 * SR); i += (long) (0.005 * SR))
    {
        const long at = i + o.latency + (long) std::lround (lagSamplesAt (o, v, i + o.latency));
        d.push_back (relativeDb (o, v, L.voiced, at));
    }
    return d.empty() ? 0.0 : median (d);
}

// One voice at one start. target: its note. bentByScoop: whether the lead's own bend counts as a right note before the lead arrives.
inline Result measure (const Out& o, int v, const Lead& L, const Run& r, double targetHz, double steady)
{
    Result res;
    const auto& y = o.ch[(size_t) v];
    const long hop = (long) (0.002 * SR), hold = (long) (limits::onsetHoldMs * 0.001 * SR);
    const long from = r.onset + o.latency - (long) (0.01 * SR);
    const long until = std::min ((long) y.size() - 1, r.end + o.latency + (long) (0.1 * SR));
    struct Frame { long at; double lvl; bool onNote; };
    std::vector<Frame> frames;
    long okFrom = -1, found = -1;
    for (long at = from; at < until; at += hop)
    {
        const double lvl = relativeDb (o, v, L.voiced, at) - steady;
        const double hz = pitchNear (y.data(), (long) y.size(), at, targetHz, SR, 0.06, 2.5);
        const bool onNote = hz > 0.0 && std::abs (cents (hz, targetHz)) <= limits::onsetOnNoteCents;
        frames.push_back ({ at, lvl, onNote });
        if (onNote && lvl >= limits::onsetLevelDb)
        {
            if (okFrom < 0) okFrom = at;
            if (at - okFrom >= hold) { found = okFrom; break; }
        }
        else okFrom = -1;
    }
    if (found >= 0)
    {
        const double lagS = lagSamplesAt (o, v, found);
        res.lagMs = 1000.0 * lagS / SR;
        res.ms = 1000.0 * (double) (found - (r.arrive + o.latency)) / SR;
        res.past = res.ms - res.lagMs;
        res.sounded = found <= r.end + o.latency + (long) lagS;
    }

    // Another note first
    long wrongRun = 0;
    for (const auto& f : frames)
    {
        if (found >= 0 && f.at >= found) break;
        // Only once the voice is reading this note: before that it is still finishing whatever came before
        const long in = f.at - o.latency - (long) std::lround (lagSamplesAt (o, v, f.at));
        if (in < r.onset) continue;
        bool wrongHere = false;
        if (f.lvl >= limits::onsetWrongAudibleDb && ! f.onNote)
        {
            const double hz = freePitch (y, f.at, targetHz);
            if (hz > 0.0)
            {
                const double leadHz = in >= 0 && in < (long) L.f0.size() ? L.f0[(size_t) in] : 0.0;
                const double meant = in >= 0 && in < (long) L.noteHz.size() ? L.noteHz[(size_t) in] : 0.0;
                const bool offNote = std::abs (cents (hz, targetHz)) > limits::onsetWrongCents;
                const bool offLead = leadHz <= 0.0 || std::abs (cents (hz, leadHz)) > limits::onsetWrongCents;
                bool offBend = true;
                if (in < r.arrive && leadHz > 0.0 && meant > 0.0)
                    offBend = std::abs (cents (hz, targetHz * leadHz / meant)) > limits::onsetWrongCents;
                wrongHere = offNote && offLead && offBend;
                if (wrongHere) res.wrongCents = cents (hz, targetHz);
            }
        }
        wrongRun = wrongHere ? wrongRun + hop : 0;
        if ((double) wrongRun >= limits::onsetWrongMs * 0.001 * SR) res.wrong = true;
    }
    return res;
}

struct TypePitches { int type; int midi[3]; };
inline const TypePitches grid[] = { { soprano, { 64, 69, 74 } }, { altoTenor, { 50, 57, 64 } }, { lowMale, { 40, 45, 52 } }, { wideRange, { 43, 57, 69 } } };
inline unsigned seedFor (int kind, int pitchIndex) { return 100u + (unsigned) (kind * 7 + pitchIndex); }

inline void testPhraseStarts()
{
    const char* typeNames[] = { "Soprano", "Alto/Tenor", "Low Male", "Wide Range" };
    const int steps[2] = { 2, 4 };   // default voices: Interval +4 and +7, a third and a fifth up the scale

    for (int variant = 0; variant < 3; ++variant)
    {
        int totalMisses = 0, totalWrong = 0, totalStarts = 0;
        double worstP95 = 0.0, worstAny = 0.0;
        const char* worstP95Kind = "";
        std::string worstAnyText;
        for (int kind = 0; kind < numKinds; ++kind)
        {
            std::vector<double> past;
            int misses = 0, wrong = 0, starts = 0;
            std::string notes;
            for (const auto& tp : grid)
            {
                for (int pi = 0; pi < 3; ++pi)
                {
                    std::vector<int> measured;
                    const Lead L = makeKind (kind, tp.midi[pi], seedFor (kind, pi), measured);
                    const auto runs = runsOf (L);
                    auto p = setup (variant, tp.type);
                    const Out o = run (*p, L.x);
                    for (int v = 0; v < 2; ++v)
                    {
                        const double steady = steadyDb (o, v, L, runs.back());
                        for (int m : measured)
                        {
                            if (m >= (int) runs.size()) { check (false, std::string ("phrase start test could not find a note: ") + kindNames[kind]); continue; }
                            const Run& r = runs[(size_t) m];
                            const double target = noteHz (diatonic (r.midi, steps[v]));
                            const auto res = measure (o, v, L, r, target, steady);
                            ++starts;
                            char where[160];
                            std::snprintf (where, sizeof where, "%s, %s, note %d, voice %d%s", kindNames[kind], typeNames[tp.type], r.midi, v + 1, m > 0 ? ", second note" : "");
                            if (std::getenv ("SH_ONSET_LIST") != nullptr)
                                std::printf ("start %s, %s: %s %.1f ms past lag %.1f, wrong %d\n", variantNames[variant], where, res.sounded ? "sounds" : "missed", res.ms, res.lagMs, res.wrong ? 1 : 0);
                            if (! res.sounded)
                            {
                                ++misses;
                                if (notes.size() < 600) notes += std::string ("      missed: ") + where + "\n";
                            }
                            else
                            {
                                past.push_back (res.past);
                                if (res.past > worstAny) { worstAny = res.past; worstAnyText = where; }
                            }
                            if (res.wrong)
                            {
                                ++wrong;
                                if (notes.size() < 600) notes += std::string ("      wrong first note (") + std::to_string ((int) std::lround (res.wrongCents)) + " cents): " + where + "\n";
                            }
                        }
                    }
                }
            }
            const double p95 = past.empty() ? 1.0e9 : percentile (past, 95.0);
            std::printf ("INFO  phrase starts, %s, %s: %d starts, missed %d, wrong first note %d, time to sound past lag median %.1f p95 %.1f worst %.1f ms\n",
                         variantNames[variant], kindNames[kind], starts, misses, wrong,
                         past.empty() ? -1.0 : median (past), past.empty() ? -1.0 : p95, past.empty() ? -1.0 : *std::max_element (past.begin(), past.end()));
            if (! notes.empty()) std::printf ("%s", notes.c_str());
            totalMisses += misses; totalWrong += wrong; totalStarts += starts;
            if (p95 > worstP95) { worstP95 = p95; worstP95Kind = kindNames[kind]; }
        }
        const std::string name = std::string ("phrase starts, ") + variantNames[variant];
        check (totalMisses <= limits::onsetMisses, name + ": first notes where a voice never sounded, of " + std::to_string (totalStarts), totalMisses);
        check (totalWrong <= limits::onsetWrongNotes, name + ": voices that sang another note first", totalWrong);
        check (worstP95 <= limits::onsetSoundP95Ms, name + ": time to sound past the voice's lag, 95th percentile, worst kind (" + worstP95Kind + ")", worstP95, "ms");
        check (worstAny <= limits::onsetSoundWorstMs, name + ": time to sound past the voice's lag, worst start (" + worstAnyText + ")", worstAny, "ms");
    }
}

// The say corpus: every voiced stretch of 150 ms or longer after 100 ms or more without pitch, by the two sided reference track.
inline void testCorpusStarts()
{
    const int steps[2] = { 2, 4 };
    const double rate = SR / std::max (1, (int) std::lround (SR / 12000.0));
    const int tauMax = (int) (rate / 60.0) + 1, W = tauMax + 40;
    const double hopSec = 0.005;
    for (int variant = 0; variant < 3; ++variant)
    {
        int stretches = 0, sounded = 0;
        std::vector<double> past;
        for (const char* name : { "speech_samantha", "speech_daniel", "speech_kathy", "sung_goodnews", "held_fred", "held_kathy" })
        {
            const auto wav = loadWav (name);
            if (wav.getNumSamples() == 0) { check (false, std::string ("could not load ") + name); continue; }
            const std::vector<float> in (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());
            const auto track = referenceTrack (in, SR, hopSec);
            auto p = setup (variant, wideRange);
            const Out o = run (*p, in);
            const auto refAt = [&] (long inputSample) -> double
            {
                const int f = (int) std::lround ((inputSample / SR * rate - tauMax - 0.5 * W) / (hopSec * rate));
                return f >= 0 && f < (int) track.size() ? track[(size_t) f] : 0.0;
            };
            const auto onNote = [&] (const std::vector<float>& y, long at, double leadHz, int v)
            {
                // The harmony of the scale notes either side of the lead, since speech sits between notes
                const double m = 69.0 + 12.0 * std::log2 (leadHz / 440.0);
                for (int cand = (int) std::floor (m) - 1; cand <= (int) std::ceil (m) + 1; ++cand)
                {
                    static const bool inC[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
                    if (cand < 0 || ! inC[cand % 12] || std::abs (cand - m) > 1.5) continue;
                    const double t = noteHz (diatonic (cand, steps[v]));
                    const double hz = pitchNear (y.data(), (long) y.size(), at, t, SR, 0.06, 2.5);
                    if (hz > 0.0 && std::abs (cents (hz, t)) <= limits::onsetOnNoteCents) return true;
                }
                return false;
            };
            for (int v = 0; v < 2; ++v)
            {
                // Steady level: every voiced frame past the first 150 ms of its stretch
                std::vector<double> d;
                int voicedFor = 0;
                for (int f = 0; f < (int) track.size(); ++f)
                {
                    voicedFor = track[(size_t) f] > 0.0 ? voicedFor + 1 : 0;
                    if (voicedFor < 30) continue;
                    const long i = (long) (referenceTime (f, SR, hopSec) * SR);
                    const long at = i + o.latency + (long) std::lround (lagSamplesAt (o, v, i + o.latency));
                    if (at < (long) in.size()) d.push_back (relativeDb (o, v, in, at));
                }
                const double steady = d.empty() ? 0.0 : median (d);

                for (int f = 20; f + 30 < (int) track.size(); ++f)
                {
                    if (track[(size_t) f] <= 0.0) continue;
                    bool startHere = true;
                    for (int k = 1; k <= 20 && startHere; ++k) if (track[(size_t) f - k] > 0.0) startHere = false;
                    for (int k = 0; k < 30 && startHere; ++k) if (track[(size_t) f + k] <= 0.0) startHere = false;
                    if (! startHere) continue;
                    int last = f;
                    while (last + 1 < (int) track.size() && track[(size_t) last + 1] > 0.0) ++last;
                    // The first voiced frame's window starts about a period after the voice does
                    const long onset = (long) ((referenceTime (f, SR, hopSec) - 0.5 * W / rate - 1.0 / track[(size_t) f]) * SR);
                    const long end = (long) (referenceTime (last, SR, hopSec) * SR);
                    ++stretches;
                    const long hop = (long) (0.002 * SR), hold = (long) (limits::onsetHoldMs * 0.001 * SR);
                    long okFrom = -1, found = -1;
                    for (long at = onset + o.latency - (long) (0.01 * SR); at < std::min ((long) in.size() - 1, end + o.latency + (long) (0.1 * SR)); at += hop)
                    {
                        const long inAt = at - o.latency - (long) std::lround (lagSamplesAt (o, v, at));
                        const double leadHz = refAt (inAt);
                        const bool ok = leadHz > 0.0 && relativeDb (o, v, in, at) - steady >= limits::onsetLevelDb && onNote (o.ch[(size_t) v], at, leadHz, v);
                        if (ok) { if (okFrom < 0) okFrom = at; if (at - okFrom >= hold) { found = okFrom; break; } }
                        else okFrom = -1;
                    }
                    if (std::getenv ("SH_CORPUS_LIST") != nullptr)
                        std::printf ("corpus %s variant %d voice %d onset %.3f s length %.0f ms: %s %.1f ms\n", name, variant, v + 1, onset / SR, 1000.0 * (end - onset) / SR,
                                     found >= 0 ? "sounds after" : "never", found >= 0 ? 1000.0 * (double) (found - onset - o.latency) / SR - 1000.0 * lagSamplesAt (o, v, found) / SR : 0.0);
                    if (found >= 0 && found <= end + o.latency + (long) lagSamplesAt (o, v, found))
                    {
                        ++sounded;
                        past.push_back (1000.0 * (double) (found - onset - o.latency) / SR - 1000.0 * lagSamplesAt (o, v, found) / SR);
                    }
                }
            }
        }
        const double share = stretches > 0 ? (double) sounded / stretches : 0.0;
        const std::string name = std::string ("say corpus starts, ") + variantNames[variant];
        info (name + ": voiced stretches times voices", stretches);
        check (share >= limits::onsetCorpusSounded, name + ": share where the voice sounds", share);
        check (! past.empty() && median (past) <= limits::onsetCorpusMedianMs, name + ": median time to sound past the lag", past.empty() ? 1.0e9 : median (past), "ms");
        if (! past.empty()) info (name + ": 90th percentile time to sound past the lag", percentile (past, 90.0), "ms");
    }
}

// Breaths, s and sh, noise and room tone must not make the voices sing. Consonants and Breath 0, so any sound from a voice is singing.
inline void testNoTrigger()
{
    const int n = (int) (2.0 * SR);
    struct Sound { const char* name; std::vector<float> x; };
    std::vector<Sound> sounds;
    std::mt19937 gen (77);
    std::normal_distribution<double> gauss (0.0, 1.0);
    const auto scaleTo = [] (std::vector<float>& x, double db)
    {
        double e = 0.0; long c = 0;
        for (float s : x) if (s != 0.0f) { e += (double) s * s; ++c; }
        const double g = std::pow (10.0, db / 20.0) / std::max (1.0e-12, std::sqrt (e / std::max (1L, c)));
        for (auto& s : x) s = (float) (s * g);
    };
    {
        std::vector<float> x ((size_t) n);
        for (auto& s : x) s = (float) gauss (gen);
        scaleTo (x, -30.0); sounds.push_back ({ "white noise at -30 dBFS", x });
        auto loud = x; scaleTo (loud, -12.0); sounds.push_back ({ "white noise at -12 dBFS", loud });
    }
    for (int vowelIndex = 0; vowelIndex < 2; ++vowelIndex)
    {
        // Breaths: noise through a vowel, swelling in and out, 0.6 s each with a short pause
        const auto& vw = vowelIndex == 0 ? voice::vowelA : voice::vowelU;
        Res r[5];
        for (int i = 0; i < 5; ++i) r[i].set (vw.f[i], vw.bw[i]);
        std::vector<float> x ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            double e = gauss (gen);
            for (auto& rr : r) e = rr.process (e);
            const double t = std::fmod ((double) i / SR, 0.7);
            const double env = t < 0.6 ? std::sin (M_PI * t / 0.6) : 0.0;
            x[(size_t) i] = (float) (e * env);
        }
        scaleTo (x, -30.0);
        sounds.push_back ({ vowelIndex == 0 ? "breaths through an open vowel" : "breaths through a closed vowel", x });
    }
    for (const double corner : { 4500.0, 2200.0 })
    {
        // s and sh: high passed noise, 150 ms bursts every 300 ms
        const double hw = 2.0 * M_PI * corner / SR, hc = std::cos (hw), al = std::sin (hw) / (2.0 * 0.9), a0 = 1.0 + al;
        const double b0 = (1.0 + hc) * 0.5 / a0, b1 = -(1.0 + hc) / a0, a1 = -2.0 * hc / a0, a2 = (1.0 - al) / a0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        std::vector<float> x ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const double w = gauss (gen), y = b0 * w + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = w; y2 = y1; y1 = y;
            const double t = std::fmod ((double) i / SR, 0.3);
            x[(size_t) i] = t < 0.15 ? (float) (y * std::sin (M_PI * t / 0.15)) : 0.0f;
        }
        scaleTo (x, -24.0);
        sounds.push_back ({ corner > 3000.0 ? "s sounds" : "sh sounds", x });
    }
    {
        // Room tone: low heavy noise, the way a quiet room and a mic preamp sound
        std::vector<float> x ((size_t) n);
        double a = 0.0, b = 0.0;
        for (auto& s : x) { const double w = gauss (gen); a += 0.02 * (w - a); b += 0.3 * (w - b); s = (float) (8.0 * a + 0.3 * b); }
        scaleTo (x, -55.0); sounds.push_back ({ "room tone at -55 dBFS", x });
    }

    double worstShare = 0.0, worstRun = 0.0;
    std::string worstShareText, worstRunText;
    const char* typeNames[] = { "Soprano", "Alto/Tenor", "Low Male", "Wide Range" };
    for (const auto& s : sounds)
        for (int variant = 0; variant < 2; ++variant)
            for (int type : { soprano, altoTenor, lowMale, wideRange })
            {
                auto p = setup (variant, type, true);
                const Out o = run (*p, s.x);
                const int frame = (int) (0.01 * SR);
                if (std::getenv ("SH_NOTRIG_LIST") != nullptr)
                {
                    double worst = 0.0;
                    for (int v = 0; v < 2; ++v)
                    {
                        int singing = 0, frames = 0;
                        for (int at = o.latency + frame; at + frame < n; at += frame)
                        {
                            const double in = rms (s.x.data() + at - o.latency, frame), out = rms (o.ch[(size_t) v].data() + at, frame);
                            ++frames; if (in > 1.0e-6 && out > in * 0.1) ++singing;
                        }
                        worst = std::max (worst, (double) singing / std::max (1, frames));
                    }
                    std::printf ("notrigger %s, %s, %s: %.3f\n", s.name, variantNames[variant], typeNames[type], worst);
                }
                for (int v = 0; v < 2; ++v)
                {
                    int singing = 0, frames = 0, runNow = 0, longest = 0;
                    for (int at = o.latency + frame; at + frame < n; at += frame)
                    {
                        const double in = rms (s.x.data() + at - o.latency, frame);
                        const double out = rms (o.ch[(size_t) v].data() + at, frame);
                        ++frames;
                        const bool sings = in > 1.0e-6 && out > in * 0.1;
                        if (sings) { ++singing; longest = std::max (longest, ++runNow); }
                        else runNow = 0;
                    }
                    const double share = (double) singing / std::max (1, frames), runMs = 10.0 * longest;
                    const std::string where = std::string (s.name) + ", " + variantNames[variant] + ", " + typeNames[type] + ", voice " + std::to_string (v + 1);
                    if (share > worstShare) { worstShare = share; worstShareText = where; }
                    if (runMs > worstRun) { worstRun = runMs; worstRunText = where; }
                }
            }
    check (worstShare <= limits::noTriggerShare, "breaths, s, sh, noise and room tone: share of time a voice sings, worst (" + (worstShareText.empty() ? std::string ("none") : worstShareText) + ")", worstShare);
    check (worstRun <= limits::noTriggerRunMs, "  and its longest stretch, worst (" + (worstRunText.empty() ? std::string ("none") : worstRunText) + ")", worstRun, "ms");
}
} // namespace onsets

namespace onsets
{
// For looking into one start: PluginTest onsettrace <kind> <inputType> <midi> <variant>. Prints the voices frame by frame.
inline void trace (int kind, int type, int midi, int variant, double spanSec = 0.2, double stepSec = 0.004)
{
    std::vector<int> measured;
    int pitchIndex = 0;
    for (int i = 0; i < 3; ++i) if (grid[type].midi[i] == midi) pitchIndex = i;
    const Lead L = makeKind (kind, midi, seedFor (kind, pitchIndex), measured);
    const auto runs = runsOf (L);
    auto p = setup (variant, type);
    const int lat = p->getLatencySamples();
    // Per block readings too
    const int n = (int) L.x.size(), block = 128;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midiBuf;
    Out o; o.latency = lat;
    for (auto& c : o.ch) c.assign ((size_t) n, 0.0f);
    std::vector<PluginProcessor::Readings> reads;
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy_n (L.x.data() + pos, len, buf.getWritePointer (c));
        p->processBlock (buf, midiBuf);
        for (int c = 0; c < 2; ++c) std::copy_n (buf.getReadPointer (c), len, o.ch[(size_t) c].data() + pos);
        const auto r = p->getReadings();
        reads.push_back (r);
        for (int v = 0; v < 2; ++v) o.lagMs[(size_t) v].push_back (r.voiceLagMs[(size_t) v]);
    }
    const Run& r = runs[(size_t) measured.back()];
    const int steps[2] = { 2, 4 };
    std::printf ("kind %s, type %d, note %d, onset %.1f ms arrive %.1f ms end %.1f ms (input), latency %.2f ms\n", kindNames[kind], type, r.midi,
                 1000.0 * r.onset / SR, 1000.0 * r.arrive / SR, 1000.0 * r.end / SR, 1000.0 * lat / SR);
    double steady[2];
    for (int v = 0; v < 2; ++v) steady[v] = steadyDb (o, v, L, runs.back());
    std::printf ("  t(out, from lead onset)  inputHz  detHz  | per voice: lag  lvl  onNoteHz  freeHz  noteReading gain\n");
    for (long at = r.onset + lat - (long) (0.02 * SR); at < r.onset + lat + (long) (spanSec * SR); at += (long) (stepSec * SR))
    {
        const auto& rd = reads[(size_t) (at / block)];
        const long in = at - lat;
        std::printf ("  %6.1f  %7.1f  %7.1f |", 1000.0 * (at - r.onset - lat) / SR, in >= 0 ? (std::getenv ("SH_LEADPITCH") != nullptr ? pitchNear (L.x.data(), (long) n, in, L.f0[(size_t) in], SR, 0.06, 2.5) : L.f0[(size_t) in]) : 0.0, rd.detectedHz);
        for (int v = 0; v < 2; ++v)
        {
            const double target = noteHz (diatonic (r.midi, steps[v]));
            const double hz = pitchNear (o.ch[(size_t) v].data(), (long) n, at, target, SR, 0.06, 2.5);
            const double fh = freePitch (o.ch[(size_t) v], at, target);
            std::printf ("  %5.1f %6.1f %7.1f %7.1f %3d %6.2f |", lagSamplesAt (o, v, at) * 1000.0 / SR, relativeDb (o, v, L.voiced, at) - steady[v], hz, fh, rd.voiceNote[(size_t) v], std::getenv ("SH_WANTED") != nullptr ? (double) rd.voicePitch[(size_t) v] : (double) rd.voiceGain[(size_t) v]);
        }
        std::printf ("\n");
    }
}
} // namespace onsets

namespace onsets
{
// PluginTest trackertrace <kind> <inputType> <midi> <fromMs> <toMs>: the tracker alone on that lead, every ms, input time.
inline void trackerTrace (int kind, int type, int midi, double fromMs, double toMs)
{
    int pitchIndex = 0;
    for (int i = 0; i < 3; ++i) if (grid[type].midi[i] == midi) pitchIndex = i;
    std::vector<int> measured;
    Lead L;
    if (kind >= 100)
    {
        // 100: breaths through a closed vowel, 101: through an open vowel, as in testNoTrigger, at -30 dBFS
        const auto& vw = kind == 100 ? voice::vowelU : voice::vowelA;
        Res r[5];
        for (int i = 0; i < 5; ++i) r[i].set (vw.f[i], vw.bw[i]);
        std::mt19937 gen (77);
        std::normal_distribution<double> gauss (0.0, 1.0);
        const int n = (int) (2.0 * SR);
        L.x.assign ((size_t) n, 0.0f); L.f0.assign ((size_t) n, 0.0); L.amp.assign ((size_t) n, 0.0); L.noteHz.assign ((size_t) n, 0.0);
        double e2 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            double e = gauss (gen);
            for (auto& rr : r) e = rr.process (e);
            const double t = std::fmod ((double) i / SR, 0.7);
            L.x[(size_t) i] = (float) (e * (t < 0.6 ? std::sin (M_PI * t / 0.6) : 0.0));
            e2 += (double) L.x[(size_t) i] * L.x[(size_t) i];
        }
        const double g = std::pow (10.0, -30.0 / 20.0) / std::sqrt (e2 / n);
        for (auto& v : L.x) v = (float) (v * g);
    }
    else L = makeKind (kind, midi, seedFor (kind, pitchIndex), measured);
    static const double lows[] = { 150.0, 98.0, 65.0, 65.0 }, highs[] = { 1300.0, 1000.0, 600.0, 1300.0 };
    PitchTracker t;
    t.prepare (SR);
    t.setRange (lows[type], highs[type]);
    t.setVoicingLimit (0.30f);
    for (size_t i = 0; i < L.x.size(); ++i)
    {
        t.push (L.x[i]);
        const double ms = 1000.0 * (double) i / SR;
        if (ms >= fromMs && ms < toMs && i % 48 == 0)
        {
            static const char* whys[] = { "voiced", "quiet", "nodip", "rough", "hiss", "even", "residual", "votes" };
            std::printf ("%7.1f true %6.1f amp %4.2f | voiced %d period %6.1f Hz coarse %6.1f Hz dip %5.3f fric %d %s\n", ms, L.f0[i], L.amp[i], t.isVoiced() ? 1 : 0,
                         t.period() > 0 ? SR / t.period() : 0.0, t.coarsePeriod() > 0 ? SR / t.coarsePeriod() : 0.0, t.difference(), t.isFricative() ? 1 : 0, whys[t.why()]);
            if (t.startRule() != 0) std::printf ("          start rule took a %s period\n", t.startRule() == 1 ? "shorter" : "longer");
            if (std::getenv ("SH_PROBE") != nullptr && L.f0[i] > 0.0)
            {
                // SH_PROBE=<r>: a candidate period of r true periods. Its dip at full rate, at twice and three times it, its even dip, and the true period's dip
                const double r = std::atof (std::getenv ("SH_PROBE")), hz = L.f0[i] / r;
                float f[4], e[4];
                t.probe (hz, f[0], e[0]); t.probe (hz / 2.0, f[1], e[1]); t.probe (hz / 3.0, f[2], e[2]); t.probe (L.f0[i], f[3], e[3]);
                std::printf ("          candidate %.0f Hz: dip %5.3f, at 2x %5.3f, at 3x %5.3f, even %5.3f | true period dip %5.3f even %5.3f\n", hz, f[0], f[1], f[2], e[0], f[3], e[3]);
            }
        }
    }
}
} // namespace onsets

namespace onsets
{
// PluginTest corpustrace <file> <fromSec> <variant>: both voices and the readings on a say file, Wide Range, every 4 ms for 0.2 s (output time).
inline void corpusTrace (const char* name, double fromSec, int variant)
{
    const auto wav = loadWav (name);
    const std::vector<float> in (wav.getReadPointer (0), wav.getReadPointer (0) + wav.getNumSamples());
    auto p = setup (variant, wideRange);
    const int lat = p->getLatencySamples(), n = (int) in.size(), block = 128;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midiBuf;
    std::vector<PluginProcessor::Readings> reads;
    std::array<std::vector<float>, 2> out;
    for (auto& c : out) c.assign ((size_t) n, 0.0f);
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy_n (in.data() + pos, len, buf.getWritePointer (c));
        p->processBlock (buf, midiBuf);
        for (int c = 0; c < 2; ++c) std::copy_n (buf.getReadPointer (c), len, out[(size_t) c].data() + pos);
        reads.push_back (p->getReadings());
    }
    const auto track = referenceTrack (in, SR, 0.005);
    const double rate = 12000.0;
    const int tauMax = (int) (rate / 60.0) + 1, W = tauMax + 40;
    for (long at = (long) (fromSec * SR) + lat; at < (long) ((fromSec + 0.2) * SR) + lat && at < n; at += (long) (0.004 * SR))
    {
        const auto& rd = reads[(size_t) (at / block)];
        const long i = at - lat;
        const int f = (int) std::lround ((i / SR * rate - tauMax - 0.5 * W) / 60.0);
        const double ref = f >= 0 && f < (int) track.size() ? track[(size_t) f] : 0.0;
        std::printf ("%7.3f s in | ref %6.1f det %6.1f | v1 note %3d lag %4.1f gain %4.2f pitch %7.1f | v2 note %3d lag %4.1f gain %4.2f pitch %7.1f\n", i / SR, ref, rd.detectedHz,
                     rd.voiceNote[0], rd.voiceLagMs[0], rd.voiceGain[0], freePitch (out[0], at, rd.voiceNote[0] > 0 ? noteHz (rd.voiceNote[0]) : 200.0),
                     rd.voiceNote[1], rd.voiceLagMs[1], rd.voiceGain[1], freePitch (out[1], at, rd.voiceNote[1] > 0 ? noteHz (rd.voiceNote[1]) : 200.0));
    }
}
} // namespace onsets
