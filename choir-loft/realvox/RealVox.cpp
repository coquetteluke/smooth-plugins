// Real vocal measurements for Choir Loft. No ground truth exists on a real take, so everything is measured from the input itself.
// Build: see CMakeLists.txt in this folder. The analysis code is this file plus tests/Analysis.h from this repo, whichever Choir Loft
// source tree the processor comes from, so two versions are measured the same way.
//
//   RealVox measure <records file> <take.wav> ...      renders each take and writes one record per measured thing
//   RealVox report <records file>                       pooled numbers
//   RealVox compare <new records> <old records>         both, and the pass rule below
//   RealVox demo <take.wav> <out.wav> <from s> <secs>   default preset, lead on, a slice of the take, 24 bit
//
// Renders: the default preset with Input Type left at its default, and Key, Scale and Reference A4 set to the take's own key and tuning
// (found from its pitch), voice 1 hard left and voice 2 hard right and the lead at -60 dB, so each
// voice is on its own channel (pan changes nothing but output gains). A second render with Consonants and Breath at 0, where any sound from a
// voice is singing, measures breaths.
//
// What is measured, all from the input (the reference pitch track is tests/Analysis.h referenceTrack, two sided, 5 ms frames):
//  - Phrase start: a voiced stretch of 150 ms or more after 250 ms or more without pitch. Word start: the same after 50 to 245 ms.
//    The onset sample is placed as in the say corpus test (testCorpusStarts).
//  - On its note, at each moment, against the part of the lead the voice reads: the harmony a third (voice 1) or a fifth (voice 2) up the key
//    of the note the lead is on (within 35 cents of a note of the key; between two, either one), as far off it as the lead is off its note or
//    not at all, within 50 cents. Or the note the voice settles on, if it settles: its median pitch 100 to 250 ms into the stretch, with 70%
//    of at least 15 readings within 50 cents of it.
//  - Time to steady level: from the onset, the first moment the voice is within 3 dB of its own steady level against the part of the lead it
//    reads, and on its note, held 20 ms. Minus latency and minus the voice's lag at that moment. Missed if that never happens before the
//    stretch ends.
//  - Wrong first note: in the voice's first 100 ms of audible sound (no quieter than 12 dB under its steady level), 20 ms or more in a row
//    off its note and more than 50 cents from the lead itself (a voice reading the lead unshifted is heard as the lead).
//  - Kind of start, from the input 80 to 10 ms before the onset: clean (silence before), consonant (over half the energy above 2.5 kHz),
//    fry (low pulses: correlation of 0.4 or more at a period of 8 to 33 ms), breathy (anything else that is not silent).
//  - Breath leak (Consonants and Breath 0): 10 ms frames before a phrase start, from 80 ms after the last voiced stretch (at most 1.5 s back)
//    to 30 ms before the onset, input above the take's noise floor + 6 dB: a voice sings in a frame when it is over -20 dB against the input,
//    as in testNoTrigger. Share of frames, longest run, and energy against the input.
//  - In phrase leak: the same in gaps of 50 to 245 ms between voiced stretches.
//  - Dropouts: frames from 150 ms into a voiced stretch of 300 ms or more to 50 ms before its end, where a voice is more than 12 dB under its
//    steady level.
//
// Pass rule, committed before any change to the plug-in was measured with it and before anything ran on the held out takes (the measures
// were shaped on 1.4.1 renders of the dev takes only). A new version passes against an old one on a set of takes when
//  targets, each strictly better:
//   T1 breath leak share lower
//   T2 phrase start wrong first notes fewer, or both none
//   T3 breathy starts (phrase and word together), time to steady level, 90th percentile lower
//  and no worse on any other (resolution: the scan runs in 2 ms steps):
//   O1 phrase starts missed share at most +0.002       O2 phrase starts median at most +1 ms     O3 phrase starts 90th percentile at most +2 ms
//   O4 clean, consonant and fry phrase starts each: median at most +1 ms and 90th percentile at most +2 ms; breathy starts median at most +1 ms
//   O5 word starts: median at most +1 ms, 90th percentile at most +2 ms, missed share at most +0.002, wrong first note share at most +0.005
//   O6 dropout share at most +0.002                    O7 in phrase leak share at most +0.002
//   O8 breath leak longest run no longer               O9 breath leak energy at most +0.5 dB
//
// 1.5.2: that rule stays as written, and at the default it sits under its own noise (one version against itself, with the take slid by a
// few ms, fails 9 or 10 of its rules). Beside it there is a rule that can be decided: every one of the same measures no worse than the
// reference by more than that measure's own noise, the noise measured from slid and reseeded runs of one version. See decidable.py in
// this folder; the measured noise is in NOISE-default.txt and NOISE-plain.txt.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "../tests/Analysis.h"
#include "PitchRule.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr double SR = 48000.0;
constexpr int block = 128;
constexpr double hopSec = 0.005;

std::vector<float> loadMono (const juce::File& f)
{
    juce::AudioFormatManager m;
    m.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (m.createReaderFor (f));
    std::vector<float> out;
    if (reader == nullptr) return out;
    if (std::abs (reader->sampleRate - SR) > 1.0) { std::fprintf (stderr, "%s is not 48 kHz\n", f.getFullPathName().toRawUTF8()); return out; }
    juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&b, 0, b.getNumSamples(), 0, true, true);
    out.assign ((size_t) b.getNumSamples(), 0.0f);
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) out[(size_t) i] += b.getSample (c, i) / (float) b.getNumChannels();
    return out;
}

// REALVOX_SEED=<n>: another run of every random choice of the plug-in (1.5.2 on; an older tree has no such option and ignores it). With
// REALVOX_SHIFT this gives the noise floor of every number: see decidable.py.
template <typename O> auto seedIfThere (O& o, unsigned seed, int) -> decltype (o.randomSeed, void()) { o.randomSeed = seed; }
template <typename O> void seedIfThere (O&, unsigned, long) {}

std::unique_ptr<PluginProcessor> makeProcessor()
{
    PluginProcessor::Options o;
    o.keyFilePath = "/nonexistent/choir-loft-realvox.json";
    o.hostLatencyUpdates = false;
    if (const char* seed = std::getenv ("REALVOX_SEED")) seedIfThere (o, (unsigned) std::atoi (seed), 0);
    return std::make_unique<PluginProcessor> (o);
}

void setParam (PluginProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// Settings from the environment, for every mode: REALVOX_PRESET=<factory preset number>, then REALVOX_SET="id=value,id=value" in each
// parameter's own units (inputType=3 is Wide Range). An id this build does not have is reported and skipped.
void applyEnvironment (PluginProcessor& p)
{
    if (const char* preset = std::getenv ("REALVOX_PRESET")) p.setCurrentProgram (std::atoi (preset));
    if (const char* set = std::getenv ("REALVOX_SET"))
    {
        juce::StringArray items;
        items.addTokens (set, ",", "");
        for (const auto& item : items)
        {
            const auto id = item.upToFirstOccurrenceOf ("=", false, false).trim();
            if (id.isEmpty()) continue;
            if (auto* prm = p.apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (item.fromFirstOccurrenceOf ("=", false, false).getFloatValue()));
            else std::fprintf (stderr, "no parameter %s in this build\n", id.toRawUTF8());
        }
    }
}

void prepare (PluginProcessor& p)
{
    p.releaseResources();
    auto layout = p.getBusesLayout();
    if (! layout.inputBuses.isEmpty()) layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    p.setBusesLayout (layout);
    p.setRateAndBufferSizeDetails (SR, block);
    p.prepareToPlay (SR, block);
}

struct Render
{
    std::array<std::vector<float>, 2> ch;
    std::array<std::vector<float>, 2> lagMs;   // per block, as the processor reports it
    int latency = 0;

    double lagSamplesAt (int v, long at) const
    {
        const auto& l = lagMs[(size_t) v];
        const size_t b = (size_t) std::max (0L, at) / (size_t) block;
        return (b < l.size() ? l[b] : (l.empty() ? 0.0f : l.back())) * 0.001 * SR;
    }
};

Render render (const std::vector<float>& x, bool singingOnly, int key, int scale, double referenceHz, const std::vector<float>* right = nullptr)
{
    auto p = makeProcessor();
    applyEnvironment (*p);
    setParam (*p, "reference", (float) referenceHz);
    setParam (*p, "key", (float) key);
    setParam (*p, "scale", (float) scale);
    setParam (*p, "leadLevel", -60.0f);
    setParam (*p, "voice1Pan", -100.0f);
    setParam (*p, "voice2Pan", 100.0f);
    if (singingOnly) { setParam (*p, "consonants", 0.0f); setParam (*p, "breath", 0.0f); }
    prepare (*p);
    Render r;
    r.latency = p->getLatencySamples();
    const int n = (int) x.size();
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (auto& c : r.ch) c.assign ((size_t) n, 0.0f);
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy_n ((c == 1 && right != nullptr ? right->data() : x.data()) + pos, len, buf.getWritePointer (c));
        midi.clear();
        p->processBlock (buf, midi);
        for (int c = 0; c < 2; ++c) std::copy_n (buf.getReadPointer (c), len, r.ch[(size_t) c].data() + pos);
        const auto rd = p->getReadings();
        for (int v = 0; v < 2; ++v) r.lagMs[(size_t) v].push_back (rd.voiceLagMs[(size_t) v]);
    }
    return r;
}

double rmsOf (const std::vector<float>& s, long from, long len)
{
    const long a = std::max (0L, from), b = std::min ((long) s.size(), from + len);
    if (b <= a) return 0.0;
    double e = 0.0;
    for (long i = a; i < b; ++i) e += (double) s[(size_t) i] * s[(size_t) i];
    return std::sqrt (e / (double) (b - a));
}
double rmsAround (const std::vector<float>& s, long centre, int half) { return rmsOf (s, centre - half, 2L * half); }
double toDb (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }

// Pitch within 8 semitones of an expected one, shortest strong period first. 0 if nothing periodic. Same rule as the onset test's freePitch.
// The rule is in PitchRule.h, so the plug-in's tests can check it.
double pitchAround (const std::vector<float>& y, long centre, double aroundHz) { return realvox::pitchAround (y, centre, aroundHz, SR); }

struct Stretch { int a = 0, b = 0; long onset = 0, end = 0; };

void measureTake (const juce::File& file, std::ofstream& out)
{
    auto x = loadMono (file);
    const std::string take = file.getFileNameWithoutExtension().toStdString();
    if (x.empty()) { std::fprintf (stderr, "could not load %s\n", take.c_str()); out << "E " << take << " load\n"; return; }
    // REALVOX_SHIFT=<samples> of silence in front: the plug-in's random choices then fall elsewhere, for the noise floor of every number
    if (const char* sh = std::getenv ("REALVOX_SHIFT")) x.insert (x.begin(), (size_t) std::max (0, std::atoi (sh)), 0.0f);

    // Reference pitch, with gaps of up to 10 ms filled
    auto track = measure::referenceTrack (x, SR, hopSec);
    const int F = (int) track.size();
    for (int f = 1; f + 1 < F; ++f)
        if (track[(size_t) f] <= 0.0 && track[(size_t) f - 1] > 0.0)
        {
            int g = f;
            while (g < F && track[(size_t) g] <= 0.0 && g - f < 3) ++g;
            if (g < F && g - f <= 2 && track[(size_t) g] > 0.0)
                for (int k = f; k < g; ++k) track[(size_t) k] = track[(size_t) f - 1];
        }
    const double rate = 12000.0;
    const int tauMax = (int) (rate / 60.0) + 1, W = tauMax + 40;
    const auto frameTime = [&] (int f) { return measure::referenceTime (f, SR, hopSec); };
    const auto leadHzAt = [&] (long inputSample) -> double
    {
        const int f = (int) std::lround ((inputSample / SR * rate - tauMax - 0.5 * W) / (hopSec * rate));
        return f >= 0 && f < F ? track[(size_t) f] : 0.0;
    };

    // Noise floor: 10th percentile of 10 ms frames
    const int frame = (int) (0.01 * SR);
    std::vector<double> levels;
    for (long i = 0; i + frame < (long) x.size(); i += frame) levels.push_back (rmsOf (x, i, frame));
    const double floorRms = std::max (std::pow (10.0, -90.0 / 20.0), levels.empty() ? 0.0 : measure::percentile (levels, 10.0));
    const double breathMin = std::max (floorRms * 2.0, std::pow (10.0, -65.0 / 20.0));

    // Voiced stretches. Pitch the reference finds near the noise floor (hum, room) is not singing: a stretch counts only if it is 12 dB or more
    // over the floor, and the frames of one that is not are taken as without pitch.
    std::vector<Stretch> stretches;
    for (int f = 0; f < F; ++f)
    {
        if (track[(size_t) f] <= 0.0) continue;
        int g = f;
        while (g + 1 < F && track[(size_t) g + 1] > 0.0) ++g;
        Stretch s;
        s.a = f; s.b = g;
        s.onset = (long) ((frameTime (f) - 0.5 * W / rate - 1.0 / track[(size_t) f]) * SR);
        s.end = (long) ((frameTime (g) + 0.5 * W / rate) * SR);
        if (rmsOf (x, s.onset, s.end - s.onset) >= floorRms * 3.98) stretches.push_back (s);
        else for (int k = f; k <= g; ++k) track[(size_t) k] = 0.0;
        f = g;
    }

    if (std::getenv ("REALVOX_STRETCHES") != nullptr)
        for (const auto& s : stretches)
            std::printf ("stretch %.3f s %4d ms level %.1f dBFS\n", s.onset / SR, 5 * (s.b - s.a + 1), toDb (rmsOf (x, s.onset, s.end - s.onset)));

    std::printf ("  %s: %.1f s, %d voiced stretches, floor %.1f dBFS, rendering\n", take.c_str(), x.size() / SR, (int) stretches.size(), toDb (floorRms));
    std::fflush (stdout);
    // The take's key, as a singer would set it: pitch classes of every voiced frame against the Krumhansl and Kessler profiles. Major and its
    // relative minor share their notes, so a mix up between them changes nothing.
    // Tuning first: the circular mean of every voiced frame's distance from the nearest semitone, so a singer a little flat of A440 throughout is
    // not read as between notes.
    double tuneCents = 0.0;
    {
        double cs = 0.0, sn = 0.0;
        for (double hz : track)
            if (hz > 0.0) { const double a = 2.0 * M_PI * (12.0 * std::log2 (hz / 440.0)); cs += std::cos (a); sn += std::sin (a); }
        tuneCents = 100.0 * std::atan2 (sn, cs) / (2.0 * M_PI);
    }
    const double referenceHz = 440.0 * std::pow (2.0, tuneCents / 1200.0);
    const auto semitoneOf = [&] (double hz) { return 69.0 + 12.0 * std::log2 (hz / referenceHz); };
    int key = 0, scale = 1;
    {
        static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        double hist[12] = {};
        for (double hz : track) if (hz > 0.0) hist[(((int) std::lround (semitoneOf (hz))) % 12 + 12) % 12] += 1.0;
        double best = -1.0e9;
        for (int k = 0; k < 12; ++k)
            for (int m = 0; m < 2; ++m)
            {
                const double* prof = m == 0 ? major : minor;
                double mh = 0, mp = 0;
                for (int i = 0; i < 12; ++i) { mh += hist[i] / 12.0; mp += prof[i] / 12.0; }
                double c = 0, eh = 0, ep = 0;
                for (int i = 0; i < 12; ++i) { const double a = hist[(i + k) % 12] - mh, b = prof[i] - mp; c += a * b; eh += a * a; ep += b * b; }
                const double r = c / std::sqrt (eh * ep + 1.0e-20);
                if (r > best) { best = r; key = k; scale = m == 0 ? 1 : 2; }
            }
    }
    out << "K " << take << " " << key << " " << scale << " " << referenceHz << "\n";
    const Render y = render (x, false, key, scale, referenceHz);
    const Render z = render (x, true, key, scale, referenceHz);
    // Notes of that scale, for telling whether a lead note picks one harmony or sits between two
    static const int majorSteps[7] = { 0, 2, 4, 5, 7, 9, 11 }, minorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };
    bool inScale[12] = {};
    for (int i = 0; i < 7; ++i) inScale[(key + (scale == 1 ? majorSteps[i] : minorSteps[i])) % 12] = true;
    // The note or notes of the key a lead pitch is on: the nearest when within 35 cents of it, else the ones either side
    const auto leadNotes = [&] (double m)
    {
        std::vector<int> notes;
        const int n = (int) std::lround (m);
        if (std::abs (m - n) <= 0.35 && inScale[((n % 12) + 12) % 12]) { notes.push_back (n); return notes; }
        int below = (int) std::floor (m), above = (int) std::ceil (m);
        if (above == below) ++above;
        while (! inScale[((below % 12) + 12) % 12]) --below;
        while (! inScale[((above % 12) + 12) % 12]) ++above;
        notes.push_back (below);
        notes.push_back (above);
        return notes;
    };
    // The default voices: a third and a fifth up the scale (Interval +4 and +7)
    const auto harmonyOf = [&] (int note, int v)
    {
        int steps = v == 0 ? 2 : 4;
        while (steps > 0) { ++note; while (! inScale[((note % 12) + 12) % 12]) ++note; --steps; }
        return note;
    };
    const long L = y.latency;
    out << "L " << take << " " << L << "\n";

    const int half = (int) (0.005 * SR);
    const auto relDb = [&] (const Render& r, int v, long at)
    {
        const long in = at - L - (long) std::lround (r.lagSamplesAt (v, at));
        return toDb (rmsAround (r.ch[(size_t) v], at, half)) - toDb (rmsAround (x, in, half));
    };

    // Each voice's steady level against the lead: every voiced frame past the first 150 ms of its stretch
    double steady[2] = { 0.0, 0.0 };
    for (int v = 0; v < 2; ++v)
    {
        std::vector<double> d;
        for (const auto& s : stretches)
            for (int f = s.a + 30; f <= s.b; ++f)
            {
                const long i = (long) (frameTime (f) * SR);
                const long at = i + L + (long) std::lround (y.lagSamplesAt (v, i + L));
                if (at < (long) x.size()) d.push_back (relDb (y, v, at));
            }
        steady[v] = d.empty() ? 0.0 : measure::median (d);
        out << "V " << take << " " << v + 1 << " steady " << steady[v] << "\n";
    }

    // Starts
    const long hop2 = (long) (0.002 * SR), hold = (long) (0.020 * SR);
    for (size_t si = 0; si < stretches.size(); ++si)
    {
        const auto& s = stretches[si];
        if (s.b - s.a + 1 < 30) continue;
        const int gapFrames = si == 0 ? s.a : s.a - stretches[si - 1].b - 1;
        const bool phrase = gapFrames >= 50;
        if (! phrase && gapFrames < 10) continue;
        const long onset = s.onset;
        if (onset - (long) (0.1 * SR) < 0) continue;

        // Kind of start
        const long preFrom = onset - (long) (0.08 * SR), preLen = (long) (0.07 * SR);
        const double preRms = rmsOf (x, preFrom, preLen);
        std::string kind = "clean";
        if (preRms > std::max (floorRms * 3.16, std::pow (10.0, -60.0 / 20.0)))
        {
            smoothharmony::Biquad hp;
            hp.highPass (2500.0, SR, 0.707);
            double eh = 0.0, et = 0.0;
            for (long i = preFrom - 200; i < preFrom + preLen; ++i)
            {
                const double v = x[(size_t) i], h = hp.process (v);
                if (i >= preFrom) { eh += h * h; et += v * v; }
            }
            if (eh > 0.5 * et) kind = "consonant";
            else
            {
                double bestR = 0.0;
                const long len = preLen - 1600;
                for (int lag = 400; lag <= 1600 && len > 200; lag += 4)
                {
                    double c = 0.0, ea = 0.0, eb = 0.0;
                    for (long i = 0; i < len; i += 2)
                    {
                        const double a = x[(size_t) (preFrom + i)], b = x[(size_t) (preFrom + i + lag)];
                        c += a * b; ea += a * a; eb += b * b;
                    }
                    bestR = std::max (bestR, c / std::sqrt (ea * eb + 1.0e-20));
                }
                kind = bestR >= 0.4 ? "fry" : "breathy";
            }
        }

        const double firstHz = track[(size_t) s.a];
        for (int v = 0; v < 2; ++v)
        {
            // The note it settles on, if it settles: its pitch from 100 to 250 ms into the stretch, at the moment it reads (median), steady if 70% of
            // at least 15 readings are within 50 cents of it. A voice already on that note counts as on its note, early or not.
            std::vector<double> voiceHz;
            for (long t = onset + (long) (0.1 * SR); t <= onset + (long) (0.25 * SR); t += (long) (0.005 * SR))
            {
                const double lead = leadHzAt (t);
                if (lead <= 0.0) continue;
                const long at = t + L + (long) std::lround (y.lagSamplesAt (v, t + L));
                const double hz = pitchAround (y.ch[(size_t) v], at, lead * 1.33);
                if (hz > 0.0) voiceHz.push_back (hz);
            }
            double settled = 0.0;
            if ((int) voiceHz.size() >= 15)
            {
                const double m = measure::median (voiceHz);
                int near = 0;
                for (double q : voiceHz) if (std::abs (measure::cents (q, m)) <= 50.0) ++near;
                if (near >= 0.7 * (double) voiceHz.size()) settled = m;
            }
            const double onsetSec = onset / SR;
            const char* traceAt = std::getenv ("REALVOX_TRACE");
            const bool tracing = traceAt != nullptr && std::abs (std::atof (traceAt) - onset / SR) < 0.003;
            if (tracing) std::printf ("trace %s %.3f voice %d settled %.1f firstHz %.1f steady %.1f\n", take.c_str(), onsetSec, v + 1, settled, firstHz, steady[v]);

            long okFrom = -1, found = -1, firstAudible = -1, wrongRun = 0;
            bool wrong = false;
            const long until = std::min ((long) x.size() - 1, std::min (s.end + L + (long) (0.1 * SR), onset + L + (long) (0.6 * SR)));
            for (long at = onset + L - (long) (0.01 * SR); at < until; at += hop2)
            {
                const long in = at - L - (long) std::lround (y.lagSamplesAt (v, at));
                double lead = leadHzAt (in);
                if (lead <= 0.0) lead = firstHz;
                const double lvl = relDb (y, v, at) - steady[v];
                double hz = 0.0;
                bool onNote = false;
                if (lvl >= -12.0)
                {
                    const double m = semitoneOf (lead);
                    double around = 0.0;
                    for (int c : leadNotes (m)) around = std::max (around, referenceHz * std::pow (2.0, (harmonyOf (c, v) - 69) / 12.0));
                    hz = pitchAround (y.ch[(size_t) v], at, around);
                    if (hz > 0.0)
                    {
                        // On its note: the harmony of the note the lead is on (either one, when the lead is between two), as far as the lead itself is off
                        // that note or not at all, within 50 cents; or the note it settles on.
                        for (int c : leadNotes (m))
                        {
                            const double off = 100.0 * (semitoneOf (hz) - harmonyOf (c, v)), bend = 100.0 * (m - c);
                            if (off >= std::min (0.0, bend) - 50.0 && off <= std::max (0.0, bend) + 50.0) onNote = true;
                        }
                        if (settled > 0.0 && std::abs (measure::cents (hz, settled)) <= 50.0) onNote = true;
                    }
                }
                if (tracing) std::printf ("  out %+6.1f ms  in %+6.1f ms  lead %6.1f  voice %6.1f  lvl %6.1f  lag %4.1f %s\n", 1000.0 * (at - onset - L) / SR,
                                          1000.0 * (in - onset) / SR, leadHzAt (in), hz, lvl, 1000.0 * y.lagSamplesAt (v, at) / SR, onNote ? "on" : "");
                if (found < 0)
                {
                    if (onNote && lvl >= -3.0)
                    {
                        if (okFrom < 0) okFrom = at;
                        if (at - okFrom >= hold) found = okFrom;
                    }
                    else okFrom = -1;
                }
                if (in >= onset && lvl >= -12.0 && firstAudible < 0) firstAudible = at;
                if (firstAudible >= 0 && at < firstAudible + (long) (0.1 * SR))
                {
                    const bool offNote = hz > 0.0 && ! onNote && std::abs (measure::cents (hz, lead)) > 50.0;
                    wrongRun = offNote ? wrongRun + hop2 : 0;
                    if (wrongRun >= hold) wrong = true;
                }
                if (found >= 0 && firstAudible >= 0 && at >= firstAudible + (long) (0.1 * SR)) break;
            }
            bool sounded = false;
            double past = 0.0;
            if (found >= 0)
            {
                const double lagS = y.lagSamplesAt (v, found);
                past = 1000.0 * (double) (found - onset - L) / SR - 1000.0 * lagS / SR;
                sounded = found <= s.end + L + (long) lagS;
            }
            char line[256];
            std::snprintf (line, sizeof line, "S %s %.3f %s %s %d measured %d %.2f %d\n", take.c_str(), onsetSec, phrase ? "phrase" : "word", kind.c_str(), v + 1,
                           sounded ? 1 : 0, past, wrong ? 1 : 0);
            out << line;
        }
    }

    // Breath leak before phrase starts, and leak in short gaps inside phrases, on the singing only render
    for (size_t si = 0; si < stretches.size(); ++si)
    {
        const auto& s = stretches[si];
        if (s.b - s.a + 1 < 30) continue;
        const int gapFrames = si == 0 ? s.a : s.a - stretches[si - 1].b - 1;
        const bool phrase = gapFrames >= 50;
        if (! phrase && gapFrames < 10) continue;
        long from = si == 0 ? 0 : stretches[si - 1].end + (long) (0.08 * SR);
        if (phrase) from = std::max (from, s.onset - (long) (1.5 * SR));
        const long to = s.onset - (long) (0.03 * SR);
        for (int v = 0; v < 2; ++v)
        {
            int frames = 0, singing = 0, run = 0, longest = 0;
            double ein = 0.0, eout = 0.0;
            for (long t = from; t + frame <= to; t += frame)
            {
                const double in = rmsOf (x, t, frame);
                if (in < breathMin) { run = 0; continue; }
                const double o = rmsOf (z.ch[(size_t) v], t + L, frame);
                ++frames;
                ein += in * in; eout += o * o;
                if (o > 0.1 * in) { ++singing; longest = std::max (longest, ++run); }
                else run = 0;
            }
            if (frames == 0) continue;
            char line[256];
            std::snprintf (line, sizeof line, "%s %s %.3f %d %d %d %d %.6g %.6g\n", phrase ? "B" : "U", take.c_str(), s.onset / SR, v + 1, frames, singing, longest * 10, ein, eout);
            out << line;
        }
    }

    // Dropouts
    for (int v = 0; v < 2; ++v)
    {
        int frames = 0, drops = 0;
        for (const auto& s : stretches)
        {
            if (s.b - s.a + 1 < 60) continue;
            for (int f = s.a + 30; f <= s.b - 10; ++f)
            {
                const long i = (long) (frameTime (f) * SR);
                const long at = i + L + (long) std::lround (y.lagSamplesAt (v, i + L));
                if (at >= (long) x.size()) continue;
                ++frames;
                if (relDb (y, v, at) - steady[v] < -12.0) ++drops;
            }
        }
        out << "D " << take << " " << v + 1 << " " << frames << " " << drops << "\n";
    }
    out.flush();
}

// Pooled numbers from a records file
struct Times { std::vector<double> past; int measured = 0, missed = 0, wrong = 0, unmeasured = 0; };
struct Summary
{
    std::map<std::string, Times> times;   // "phrase", "word", "phrase clean" ...
    long breathFrames = 0, breathSinging = 0, breathLongest = 0, uFrames = 0, uSinging = 0, dFrames = 0, drops = 0;
    double breathIn = 0.0, breathOut = 0.0;
    std::map<std::string, int> latency;
    int takes = 0;
};

Summary readRecords (const std::string& path)
{
    Summary s;
    std::ifstream in (path);
    std::string line;
    while (std::getline (in, line))
    {
        std::istringstream ls (line);
        std::string tag, take;
        ls >> tag >> take;
        if (tag == "L") { int l = 0; ls >> l; s.latency[take] = l; ++s.takes; }
        else if (tag == "S")
        {
            double sec = 0.0, past = 0.0; std::string kindOf, kind, state; int voice = 0, sounded = 0, wrong = 0;
            ls >> sec >> kindOf >> kind >> voice >> state >> sounded >> past >> wrong;
            for (const std::string key : { kindOf, kindOf + " " + kind, "any " + kind })
            {
                auto& t = s.times[key];
                if (state != "measured") { ++t.unmeasured; continue; }
                ++t.measured;
                if (! sounded) ++t.missed; else t.past.push_back (past);
                if (wrong) ++t.wrong;
            }
        }
        else if (tag == "B" || tag == "U")
        {
            double sec = 0.0, ein = 0.0, eout = 0.0; int voice = 0, frames = 0, singing = 0, longest = 0;
            ls >> sec >> voice >> frames >> singing >> longest >> ein >> eout;
            if (tag == "B") { s.breathFrames += frames; s.breathSinging += singing; s.breathLongest = std::max<long> (s.breathLongest, longest); s.breathIn += ein; s.breathOut += eout; }
            else { s.uFrames += frames; s.uSinging += singing; }
        }
        else if (tag == "D") { int voice = 0, frames = 0, drops = 0; ls >> voice >> frames >> drops; s.dFrames += frames; s.drops += drops; }
    }
    return s;
}

double med (const Times& t) { return t.past.empty() ? std::nan ("") : measure::median (t.past); }
double p90 (const Times& t) { return t.past.empty() ? std::nan ("") : measure::percentile (t.past, 90.0); }
double missedShare (const Times& t) { return t.measured > 0 ? (double) t.missed / t.measured : 0.0; }
double breathShare (const Summary& s) { return s.breathFrames > 0 ? (double) s.breathSinging / s.breathFrames : 0.0; }
double breathDb (const Summary& s) { return 10.0 * std::log10 (std::max (s.breathOut, 1.0e-30) / std::max (s.breathIn, 1.0e-30)); }
double uShare (const Summary& s) { return s.uFrames > 0 ? (double) s.uSinging / s.uFrames : 0.0; }
double dShare (const Summary& s) { return s.dFrames > 0 ? (double) s.drops / s.dFrames : 0.0; }

void printSummary (const Summary& s, const char* label)
{
    std::printf ("%s: %d takes\n", label, s.takes);
    for (const auto& [l, n] : s.latency) std::printf ("  latency %s %.2f ms\n", l.c_str(), 1000.0 * n / SR);
    for (const auto& [key, t] : s.times)
        std::printf ("  %-18s measured %4d (unmeasured %3d)  missed %3d  wrong first note %3d  time to steady level past latency and lag: median %6.1f  p90 %6.1f ms\n",
                     key.c_str(), t.measured, t.unmeasured, t.missed, t.wrong, med (t), p90 (t));
    std::printf ("  breath leak: %ld frames, voice sings in %.4f, longest %ld ms, energy %.1f dB against the input\n", s.breathFrames, breathShare (s), s.breathLongest, breathDb (s));
    std::printf ("  in phrase leak: %ld frames, voice sings in %.4f\n", s.uFrames, uShare (s));
    std::printf ("  dropouts: %ld frames, %.4f\n", s.dFrames, dShare (s));
}

int compare (const std::string& newPath, const std::string& oldPath)
{
    const Summary n = readRecords (newPath), o = readRecords (oldPath);
    printSummary (n, ("new " + newPath).c_str());
    printSummary (o, ("old " + oldPath).c_str());
    int fails = 0;
    const auto rule = [&] (bool ok, const std::string& what, double nv, double ov)
    {
        if (! ok) ++fails;
        std::printf ("%s  %s: new %.4f, old %.4f\n", ok ? "PASS" : "FAIL", what.c_str(), nv, ov);
    };
    const auto T = [&] (const char* k) -> Times { auto it = n.times.find (k); return it == n.times.end() ? Times() : it->second; };
    const auto To = [&] (const char* k) -> Times { auto it = o.times.find (k); return it == o.times.end() ? Times() : it->second; };
    const auto lessOrNan = [] (double a, double b, double slack) { return std::isnan (a) && std::isnan (b) ? true : a <= b + slack; };

    rule (breathShare (n) < breathShare (o), "T1 breath leak share lower", breathShare (n), breathShare (o));
    rule (T ("phrase").wrong < To ("phrase").wrong || (T ("phrase").wrong == 0 && To ("phrase").wrong == 0), "T2 phrase start wrong first notes fewer, or both none", T ("phrase").wrong, To ("phrase").wrong);
    rule (p90 (T ("any breathy")) < p90 (To ("any breathy")), "T3 breathy starts, phrase and word, 90th percentile lower (ms)", p90 (T ("any breathy")), p90 (To ("any breathy")));
    rule (missedShare (T ("phrase")) <= missedShare (To ("phrase")) + 0.002, "O1 phrase starts missed share", missedShare (T ("phrase")), missedShare (To ("phrase")));
    rule (lessOrNan (med (T ("phrase")), med (To ("phrase")), 1.0), "O2 phrase starts median (ms)", med (T ("phrase")), med (To ("phrase")));
    rule (lessOrNan (p90 (T ("phrase")), p90 (To ("phrase")), 2.0), "O3 phrase starts 90th percentile (ms)", p90 (T ("phrase")), p90 (To ("phrase")));
    for (const char* k : { "phrase clean", "phrase consonant", "phrase fry" })
    {
        rule (lessOrNan (med (T (k)), med (To (k)), 1.0), std::string ("O4 ") + k + " median (ms)", med (T (k)), med (To (k)));
        rule (lessOrNan (p90 (T (k)), p90 (To (k)), 2.0), std::string ("O4 ") + k + " 90th percentile (ms)", p90 (T (k)), p90 (To (k)));
    }
    rule (lessOrNan (med (T ("phrase breathy")), med (To ("phrase breathy")), 1.0), "O4 phrase breathy median (ms)", med (T ("phrase breathy")), med (To ("phrase breathy")));
    rule (lessOrNan (med (T ("word")), med (To ("word")), 1.0), "O5 word starts median (ms)", med (T ("word")), med (To ("word")));
    rule (lessOrNan (p90 (T ("word")), p90 (To ("word")), 2.0), "O5 word starts 90th percentile (ms)", p90 (T ("word")), p90 (To ("word")));
    rule (missedShare (T ("word")) <= missedShare (To ("word")) + 0.002, "O5 word starts missed share", missedShare (T ("word")), missedShare (To ("word")));
    const auto wrongShare = [] (const Times& t) { return t.measured > 0 ? (double) t.wrong / t.measured : 0.0; };
    rule (wrongShare (T ("word")) <= wrongShare (To ("word")) + 0.005, "O5 word starts wrong first note share", wrongShare (T ("word")), wrongShare (To ("word")));
    rule (dShare (n) <= dShare (o) + 0.002, "O6 dropout share", dShare (n), dShare (o));
    rule (uShare (n) <= uShare (o) + 0.002, "O7 in phrase leak share", uShare (n), uShare (o));
    rule (n.breathLongest <= o.breathLongest, "O8 breath leak longest run (ms)", (double) n.breathLongest, (double) o.breathLongest);
    rule (breathDb (n) <= breathDb (o) + 0.5, "O9 breath leak energy (dB)", breathDb (n), breathDb (o));
    std::printf ("%s  %d of the rules failed\n", fails ? "RESULT FAIL" : "RESULT PASS", fails);
    return fails ? 1 : 0;
}

int demo (const juce::File& in, const juce::File& outFile, double fromSec, double seconds)
{
    const auto x = loadMono (in);
    if (x.empty()) return 1;
    auto p = makeProcessor();
    applyEnvironment (*p);
    prepare (*p);
    const int n = (int) x.size();
    juce::AudioBuffer<float> all (2, n), buf (2, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy_n (x.data() + pos, len, buf.getWritePointer (c));
        midi.clear();
        p->processBlock (buf, midi);
        for (int c = 0; c < 2; ++c) all.copyFrom (c, pos, buf, c, 0, len);
    }
    // Line the slice up with the input: the plug-in reports its latency, a host would take it off
    const int lat = p->getLatencySamples();
    const int from = (int) (fromSec * SR), len = std::min ((int) (seconds * SR), n - from - lat);
    juce::AudioBuffer<float> slice (2, len);
    for (int c = 0; c < 2; ++c) slice.copyFrom (c, 0, all, c, from + lat, len);
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (outFile.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), SR, 2, 24, {}, 0));
    if (w == nullptr) return 1;
    os.release();
    w->writeFromAudioSampleBuffer (slice, 0, len);
    std::printf ("wrote %s, %.1f s, peak %.1f dBFS\n", outFile.getFullPathName().toRawUTF8(), len / SR, toDb (slice.getMagnitude (0, len)));
    return 0;
}

// =====================================================================================================================================
// Held notes (added for 1.5.0). Luke: "it misses low notes a lot".
//
//   RealVox notes <records file> <take.wav> ...        every held note of each take: did each voice sing it, and if not, why
//   RealVox notesreport <records file>                  the table by lead pitch and by cause
//   RealVox notescompare <new records> <old records>    both tables and the pass rule below
//
// Render: the default preset, Reference A4 at its default of 440 (the takes are in their songs' keys at A440, and that is what Luke gets in
// Logic), Key and Scale found from the take's pitch on the A440 grid (TAKES.txt has no key filled in), Consonants and Breath 0 so anything
// a voice puts out is singing, voice 1 hard left and voice 2 hard right, lead off. REALVOX_SET changes parameters, for example
// REALVOX_SET="inputType=3" is Wide Range. REALVOX_SHIFT=<samples> puts that much silence in front of the take: the plug-in's random
// choices then fall elsewhere, so two runs of one version give the noise floor of every number here.
//
// A held note: inside a voiced stretch of the reference track, 200 ms or more where the lead's pitch stays within 70 cents of the note's
// own average so far (one or two frames outside it are passed over), with at most a fifth of it hiss (half or more of the energy above
// 2.5 kHz: the reference track reads a pitch of 500 Hz and up into some S and T sounds, and those are not notes). Its pitch is the median. Three bands: under 98 Hz (below Alto/Tenor's
// range), 98 to 130 Hz, 130 Hz and up.
//
// Each voice, every 5 ms of the note, at the moment that voice plays that part of the lead (latency plus the voice's lag):
//   silent    more than 12 dB under the voice's steady level against the lead
//   on note   the harmony of the note the lead is on (a third up for voice 1, a fifth up for voice 2, in the key), within 50 cents, or as
//             far off it as the lead is off its own note. The same test as the phrase start measure.
//   octave    on note once its pitch is halved or doubled
//   unshifted within 50 cents of the lead itself
//   other     some other pitch, or audible with no pitch found
// One verdict per voice and note, the first that applies:
//   missed    silent for half the note or more and on note for under a quarter of it
//   octave    30 ms or more in a row an octave off
//   wrong     on note for under half of the note past its first 80 ms
//   dropout   after it first sang the note, silent for 30 ms or more in a row, not counting the note's last 30 ms
//   late      first on its note (held 20 ms) more than 80 ms into the note, the limit past which a listener hears a note skipped
//             (onsetSoundWorstMs in tests/PassLimits.h)
//   ok
// And every pitched moment of the lead, held or not (rap has few held notes): stretches of 100 ms or more, without their first 40 ms and
// last 20 ms, without moments more than 20 dB under the take's usual sung level and without hiss as above. Share of that time each voice is on its note, silent,
// an octave off, unshifted or elsewhere, with what the tracker was doing whenever the voice was not on its note. A voice gliding between
// two notes is not on either, so this share is under 1 even when nothing is wrong: it is for comparing bands and versions.
// Cause of each verdict that is not ok, from a copy of the plug-in's own tracker run on the take with the same Input Type and Tracking,
// read every ms (12 ms later than the reference frame, about what its window lags). Each ms the tracker is either right (voiced, within
// 100 cents of the reference), an octave or a twelfth high or low, on another pitch, or not voiced for one of its own reasons: quiet (its
// -60 dBFS gate), no dip or rough (not periodic enough for the Tracking setting: fry, breathy), hiss, even or residual (the two start
// checks from 1.4.1 and 1.4.2), votes (waiting for three steady readings). The cause is
//   range       the note is under the lowest pitch the Input Type tracks (what the tracker did instead is kept too)
//   downstream  the tracker was right for 90% of the note or more (97% for a dropout; for a late note, 80% of its first 100 ms), so the
//               fault is after it: the voicing look up, the level match, the glide
//   otherwise   whatever the tracker was doing most while it was not right (for a late note, in its first 100 ms)
//
// Pass rule for a fix, committed with this code before any change to the plug-in was measured with it (the measures were shaped on 1.4.4
// renders of the working set only). The slack on each "no worse" rule is the noise floor: 1.4.4 against itself with REALVOX_SHIFT=1009 and
// 2003 on the working set moved the ok share by 0.0015 (98 to 130 Hz) and 0.0004 (130 Hz and up), the on note share of time by 0.0006
// and 0.0008, the octave share of time by 0.0001, and the octave and late counts not at all (6, 6, 6 and 0, 0, 0). Slack: 0.002 and
// 0.001 on the ok shares, 0.001 on the shares of time, 0.0005 on the octave share of time, and on the counts one held note, which is 2
// voice notes, since no count moved.
//   H1 target: under 98 Hz, share of voice notes that are ok, higher by 0.20 or more
//   H2 target: under 98 Hz, share of time on note, higher by 0.20 or more
//   H3 98 to 130 Hz: ok share no lower than old minus 0.002         H4 130 Hz and up: ok share no lower than old minus 0.001
//   H5 98 to 130 Hz: share of time on note no lower than old minus 0.001      H6 130 Hz and up: the same
//   H7 octave verdicts, all bands: no more than old plus 2           H8 octave share of time, all bands: no more than old plus 0.0005
//   H9 late verdicts, 98 Hz and up: no more than old plus 2
//   H10 latency of every take the same as old
// and the 1.4.2 rule above still holds as far as it guards (O1 to O9; T1 to T3 were that version's targets and are printed, not required).
// =====================================================================================================================================

struct Note { int a = 0, b = 0; double hz = 0.0; bool first = false; };

// The dry take and what is found in it, the same way measureTake does, on the A440 grid.
struct Dry
{
    std::string take;
    std::vector<float> x;
    std::vector<double> track;
    std::vector<Stretch> stretches;
    std::vector<Note> notes;
    std::vector<char> hissy;      // per frame: half or more of its energy is above 2.5 kHz. The reference track finds a "pitch" of 500 Hz and up
                                  // in some S and T sounds; those are consonants, not notes, and are left out of the notes and the every moment table.
    double floorRms = 0.0;
    double referenceHz = 440.0;
    int key = 0, scale = 1;
    bool inScale[12] = {};
    static constexpr double rate = 12000.0;
    static constexpr int tauMax = (int) (rate / 60.0) + 1, W = tauMax + 40;

    int frames() const { return (int) track.size(); }
    double frameTime (int f) const { return measure::referenceTime (f, SR, hopSec); }
    long frameSample (int f) const { return (long) (frameTime (f) * SR); }
    int frameOf (long inputSample) const { return (int) std::lround ((inputSample / SR * rate - tauMax - 0.5 * W) / (hopSec * rate)); }
    double leadHzAt (long inputSample) const
    {
        const int f = frameOf (inputSample);
        return f >= 0 && f < frames() ? track[(size_t) f] : 0.0;
    }
    double semitoneOf (double hz) const { return 69.0 + 12.0 * std::log2 (hz / referenceHz); }
    double noteHz (double note) const { return referenceHz * std::pow (2.0, (note - 69.0) / 12.0); }
    std::vector<int> leadNotes (double m) const
    {
        std::vector<int> out;
        const int n = (int) std::lround (m);
        if (std::abs (m - n) <= 0.35 && inScale[((n % 12) + 12) % 12]) { out.push_back (n); return out; }
        int below = (int) std::floor (m), above = (int) std::ceil (m);
        if (above == below) ++above;
        while (! inScale[((below % 12) + 12) % 12]) --below;
        while (! inScale[((above % 12) + 12) % 12]) ++above;
        out.push_back (below);
        out.push_back (above);
        return out;
    }
    int harmonyOf (int note, int v) const
    {
        int steps = v == 0 ? 2 : 4;
        while (steps > 0) { ++note; while (! inScale[((note % 12) + 12) % 12]) ++note; --steps; }
        return note;
    }
    double searchAround (double leadHz, int v) const
    {
        double around = 0.0;
        for (int c : leadNotes (semitoneOf (leadHz))) around = std::max (around, noteHz (harmonyOf (c, v)));
        return around;
    }
    bool onNote (double voiceHz, double leadHz, int v) const
    {
        const double m = semitoneOf (leadHz);
        for (int c : leadNotes (m))
        {
            const double off = 100.0 * (semitoneOf (voiceHz) - harmonyOf (c, v)), bend = 100.0 * (m - c);
            if (off >= std::min (0.0, bend) - 50.0 && off <= std::max (0.0, bend) + 50.0) return true;
        }
        return false;
    }
    // Cents from the harmony note nearest to where the voice is, for a lead at leadHz
    double centsOffHarmony (double voiceHz, double leadHz, int v) const
    {
        double best = 1.0e9;
        for (int c : leadNotes (semitoneOf (leadHz)))
        {
            const double off = 100.0 * (semitoneOf (voiceHz) - harmonyOf (c, v));
            if (std::abs (off) < std::abs (best)) best = off;
        }
        return best;
    }

    bool load (const juce::File& file)
    {
        x = loadMono (file);
        take = file.getFileNameWithoutExtension().toStdString();
        if (x.empty()) return false;
        if (const char* sh = std::getenv ("REALVOX_SHIFT")) x.insert (x.begin(), (size_t) std::max (0, std::atoi (sh)), 0.0f);

        track = measure::referenceTrack (x, SR, hopSec);
        const int F = frames();
        for (int f = 1; f + 1 < F; ++f)
            if (track[(size_t) f] <= 0.0 && track[(size_t) f - 1] > 0.0)
            {
                int g = f;
                while (g < F && track[(size_t) g] <= 0.0 && g - f < 3) ++g;
                if (g < F && g - f <= 2 && track[(size_t) g] > 0.0)
                    for (int k = f; k < g; ++k) track[(size_t) k] = track[(size_t) f - 1];
            }
        const int frame = (int) (0.01 * SR);
        std::vector<double> levels;
        for (long i = 0; i + frame < (long) x.size(); i += frame) levels.push_back (rmsOf (x, i, frame));
        floorRms = std::max (std::pow (10.0, -90.0 / 20.0), levels.empty() ? 0.0 : measure::percentile (levels, 10.0));
        for (int f = 0; f < F; ++f)
        {
            if (track[(size_t) f] <= 0.0) continue;
            int g = f;
            while (g + 1 < F && track[(size_t) g + 1] > 0.0) ++g;
            Stretch s;
            s.a = f; s.b = g;
            s.onset = (long) ((frameTime (f) - 0.5 * W / rate - 1.0 / track[(size_t) f]) * SR);
            s.end = (long) ((frameTime (g) + 0.5 * W / rate) * SR);
            if (rmsOf (x, s.onset, s.end - s.onset) >= floorRms * 3.98) stretches.push_back (s);
            else for (int k = f; k <= g; ++k) track[(size_t) k] = 0.0;
            f = g;
        }
        // Key on the A440 grid
        static const double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        double hist[12] = {};
        for (double hz : track) if (hz > 0.0) hist[(((int) std::lround (semitoneOf (hz))) % 12 + 12) % 12] += 1.0;
        double best = -1.0e9;
        for (int k = 0; k < 12; ++k)
            for (int m = 0; m < 2; ++m)
            {
                const double* prof = m == 0 ? major : minor;
                double mh = 0, mp = 0;
                for (int i = 0; i < 12; ++i) { mh += hist[i] / 12.0; mp += prof[i] / 12.0; }
                double c = 0, eh = 0, ep = 0;
                for (int i = 0; i < 12; ++i) { const double a = hist[(i + k) % 12] - mh, b = prof[i] - mp; c += a * b; eh += a * a; ep += b * b; }
                const double r = c / std::sqrt (eh * ep + 1.0e-20);
                if (r > best) { best = r; key = k; scale = m == 0 ? 1 : 2; }
            }
        static const int majorSteps[7] = { 0, 2, 4, 5, 7, 9, 11 }, minorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };
        for (int i = 0; i < 7; ++i) inScale[(key + (scale == 1 ? majorSteps[i] : minorSteps[i])) % 12] = true;
        {
            smoothharmony::Biquad hp;
            hp.highPass (2500.0, SR, 0.707);
            std::vector<float> high (x.size());
            for (size_t i = 0; i < x.size(); ++i) high[i] = (float) hp.process (x[i]);
            hissy.assign ((size_t) F, 0);
            for (int f = 0; f < F; ++f)
            {
                const double all = rmsOf (x, frameSample (f) - 240, 480), h = rmsOf (high, frameSample (f) - 240, 480);
                hissy[(size_t) f] = h * h >= 0.5 * all * all ? 1 : 0;
            }
        }
        findNotes();
        return true;
    }

    void findNotes()
    {
        for (const auto& s : stretches)
        {
            int f = s.a;
            while (f <= s.b)
            {
                double sum = std::log2 (track[(size_t) f]);
                int n = 1, g = f;
                const auto out = [&] (int h) { return std::abs (1200.0 * (std::log2 (track[(size_t) h]) - sum / n)) > 70.0; };
                while (g + 1 <= s.b)
                {
                    int h = g + 1;
                    while (h <= s.b && h - g <= 2 && out (h)) ++h;
                    if (h > s.b || out (h)) break;
                    g = h;
                    sum += std::log2 (track[(size_t) g]);
                    ++n;
                }
                int hiss = 0;
                for (int k = f; k <= g; ++k) hiss += hissy[(size_t) k];
                if (g - f + 1 >= 40 && hiss <= (g - f + 1) / 5)
                {
                    Note nt;
                    nt.a = f; nt.b = g; nt.first = f == s.a;
                    std::vector<double> hz (track.begin() + f, track.begin() + g + 1);
                    nt.hz = measure::median (hz);
                    notes.push_back (nt);
                }
                f = g + 1;
            }
        }
    }
};

const char* bandOf (double hz) { return hz < 98.0 ? "under98" : (hz < 130.0 ? "98to130" : "over130"); }

// The plug-in's own tracker, run on the take the way the processor runs it, read once a ms.
struct TrackerTrace { std::vector<unsigned char> why, rule; std::vector<float> hz, dip, coarseHz, halfFine, halfEven; double lowHz = 0.0, highHz = 0.0; };

// The tracker's probe came with 1.5.1; a 1.5.0 tree builds without it (and without CHOIR_OLD, so its own range set up is used)
template <typename T> auto probeIfThere (const T& t, double hz, float& fine, float& even, int) -> decltype (t.probe (hz, fine, even), void()) { t.probe (hz, fine, even); }
template <typename T> void probeIfThere (const T&, double, float&, float&, long) {}
template <typename T> auto startRuleIfThere (const T& t, int) -> decltype (t.startRule()) { return t.startRule(); }
template <typename T> int startRuleIfThere (const T&, long) { return 0; }

TrackerTrace traceTracker (const std::vector<float>& x, int inputType, float tracking)
{
    TrackerTrace t;
#ifdef REALVOX_OLD_TREE
    static const double ranges[4][2] = { { 150.0, 1300.0 }, { 98.0, 1000.0 }, { 65.0, 600.0 }, { 65.0, 1300.0 } };
    t.lowHz = ranges[std::clamp (inputType, 0, 3)][0];
    t.highHz = ranges[std::clamp (inputType, 0, 3)][1];
#else
    PluginProcessor::trackingRange (inputType, t.lowHz, t.highHz);
#endif
    smoothharmony::PitchTracker tr;
    tr.prepare (SR);
#ifdef REALVOX_OLD_TREE
    tr.setRange (t.lowHz, t.highHz);
#else
    PluginProcessor::prepareTrackerLike (tr, inputType);
#endif
    tr.setVoicingLimit (0.10f + 0.004f * tracking);
    const int tickSamples = (int) std::lround (SR * 0.001);
    for (size_t i = 0; i < x.size(); ++i)
    {
        tr.push (x[i]);
        if ((int) ((i + 1) % (size_t) tickSamples) == 0)
        {
            const double p = tr.period();
#ifdef REALVOX_OLD_TREE
            t.why.push_back (p > 0.0 ? 0 : 3);
#else
            t.why.push_back ((unsigned char) (p > 0.0 ? 0 : std::max (1, tr.why())));
#endif
            t.hz.push_back (p > 0.0 ? (float) (SR / p) : 0.0f);
            t.dip.push_back (tr.difference());
            t.coarseHz.push_back (tr.coarsePeriod() > 0.0 ? (float) (SR / tr.coarsePeriod()) : 0.0f);
#ifndef REALVOX_OLD_TREE
            // How well the sound repeats at half the coarse period, and how its even harmonics do: what the tracker asks before a low start
            float hf = 1.0f, he = 1.0f;
            if (tr.coarsePeriod() > 0.0) probeIfThere (tr, 2.0 * SR / tr.coarsePeriod(), hf, he, 0);
            t.halfFine.push_back (hf);
            t.halfEven.push_back (he);
#endif
            t.rule.push_back ((unsigned char) startRuleIfThere (tr, 0));
        }
    }
    return t;
}

// Tracker states for the cause: 0 right, 1 to 7 the tracker's own reasons, 8 octave or twelfth high, 9 low, 10 another pitch
constexpr int numStates = 11;
const char* const stateNames[numStates] = { "right", "quiet", "nodip", "rough", "hiss", "even", "residual", "votes", "highoctave", "lowoctave", "otherpitch" };

int trackerStateAt (const TrackerTrace& t, const Dry& d, long inputSample)
{
    const long tick = (inputSample + (long) (0.012 * SR)) / (long) std::lround (SR * 0.001);
    if (tick < 0 || tick >= (long) t.hz.size()) return 1;
    const double ref = d.leadHzAt (inputSample), hz = t.hz[(size_t) tick];
    if (hz <= 0.0) return std::clamp ((int) t.why[(size_t) tick], 1, 7);
    if (ref <= 0.0) return 10;
    const double c = measure::cents (hz, ref);
    if (std::abs (c) <= 100.0) return 0;
    if (std::abs (c - 1200.0) <= 100.0 || std::abs (c - 1902.0) <= 100.0) return 8;
    if (std::abs (c + 1200.0) <= 100.0 || std::abs (c + 1902.0) <= 100.0) return 9;
    return 10;
}

struct RenderInfo { int inputType = 1; float tracking = 50.0f; };
RenderInfo renderInfo()
{
    auto p = makeProcessor();
    applyEnvironment (*p);
    RenderInfo r;
    r.inputType = (int) p->apvts.getRawParameterValue ("inputType")->load();
    r.tracking = p->apvts.getRawParameterValue ("tracking")->load();
    return r;
}

double steadyLevel (const Dry& d, const Render& r, int v)
{
    const long L = r.latency;
    const int half = (int) (0.005 * SR);
    std::vector<double> rel;
    for (const auto& s : d.stretches)
        for (int f = s.a + 30; f <= s.b; ++f)
        {
            const long i = d.frameSample (f);
            const long at = i + L + (long) std::lround (r.lagSamplesAt (v, i + L));
            if (at < (long) d.x.size())
                rel.push_back (toDb (rmsAround (r.ch[(size_t) v], at, half)) - toDb (rmsAround (d.x, at - L - (long) std::lround (r.lagSamplesAt (v, at)), half)));
        }
    return rel.empty() ? 0.0 : measure::median (rel);
}

void notesTake (const juce::File& file, std::ofstream& out)
{
    Dry d;
    if (! d.load (file)) { std::fprintf (stderr, "could not load %s\n", file.getFullPathName().toRawUTF8()); return; }
    double under98 = 0.0;
    for (const auto& s : d.stretches) for (int f = s.a; f <= s.b; ++f) if (d.track[(size_t) f] < 98.0) under98 += hopSec;
    std::printf ("  %s: %.1f s, %d held notes, key %d scale %d, %.1f s of pitch under 98 Hz, rendering\n", d.take.c_str(), d.x.size() / SR, (int) d.notes.size(), d.key, d.scale, under98);
    std::fflush (stdout);

    // What the plug-in is fed: the dry take, or with REALVOX_FEED=<folder> the file of the same name there (the take through whatever sits
    // in front of Choir Loft in a chain, left and right), less REALVOX_FEED_DELAY samples of that plug-in's latency. The notes it is judged
    // against are always the dry take's.
    std::vector<float> feedL = d.x, feedR = d.x, feedMono = d.x;
    if (const char* dir = std::getenv ("REALVOX_FEED"))
    {
        juce::AudioFormatManager m;
        m.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (m.createReaderFor (juce::File (dir).getChildFile (file.getFileName())));
        if (reader == nullptr || std::abs (reader->sampleRate - SR) > 1.0) { std::fprintf (stderr, "no 48 kHz %s in %s\n", file.getFileName().toRawUTF8(), dir); return; }
        juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&b, 0, b.getNumSamples(), 0, true, true);
        const int delay = std::getenv ("REALVOX_FEED_DELAY") != nullptr ? std::atoi (std::getenv ("REALVOX_FEED_DELAY")) : 0;
        for (size_t i = 0; i < d.x.size(); ++i)
        {
            const int at = (int) i + delay;
            feedL[i] = at < b.getNumSamples() ? b.getSample (0, at) : 0.0f;
            feedR[i] = at < b.getNumSamples() ? b.getSample (b.getNumChannels() > 1 ? 1 : 0, at) : 0.0f;
            feedMono[i] = 0.5f * (feedL[i] + feedR[i]);
        }
        std::printf ("    fed from %s, %d channels, %d samples of delay taken off, level %.1f dB against the dry take\n", dir, b.getNumChannels(), delay,
                     toDb (rmsOf (feedMono, 0, (long) feedMono.size())) - toDb (rmsOf (d.x, 0, (long) d.x.size())));
    }
    const Render z = render (feedL, true, d.key, d.scale, 440.0, &feedR);
    const RenderInfo info = renderInfo();
    const TrackerTrace tr = traceTracker (feedMono, info.inputType, info.tracking);
    const long L = z.latency;
    out << "L " << d.take << " " << L << "\n";
    out << "R " << d.take << " " << info.inputType << " " << tr.lowHz << " " << tr.highHz << "\n";
    const int half = (int) (0.005 * SR);
    double steady[2];
    for (int v = 0; v < 2; ++v) steady[v] = steadyLevel (d, z, v);

    // What a voice is doing while it plays frame f of the lead: S silent, N on note, O an octave off, U unshifted, W another pitch, X no pitch found
    const auto classify = [&] (int f, int v, double* hzOut = nullptr, double* lvlOut = nullptr)
    {
        const long i = d.frameSample (f);
        const long at = i + L + (long) std::lround (z.lagSamplesAt (v, i + L));
        if (at + half >= (long) d.x.size()) return 'S';
        const long in = at - L - (long) std::lround (z.lagSamplesAt (v, at));
        const double lead = d.track[(size_t) f];
        const double lvl = toDb (rmsAround (z.ch[(size_t) v], at, half)) - toDb (rmsAround (d.x, in, half)) - steady[v];
        char c = 'S';
        double hz = 0.0;
        if (lvl >= -12.0)
        {
            const double around = d.searchAround (lead, v);
            hz = pitchAround (z.ch[(size_t) v], at, around);
            if (hz > 0.0 && d.onNote (hz, lead, v)) c = 'N';
            else
            {
                const double up = pitchAround (z.ch[(size_t) v], at, around * 2.0), down = pitchAround (z.ch[(size_t) v], at, around * 0.5);
                if ((up > 0.0 && d.onNote (up * 0.5, lead, v)) || (down > 0.0 && d.onNote (down * 2.0, lead, v))) c = 'O';
                else if (hz > 0.0 && std::abs (measure::cents (hz, lead)) <= 50.0) c = 'U';
                else c = hz > 0.0 ? 'W' : 'X';
            }
        }
        if (hzOut != nullptr) *hzOut = hz;
        if (lvlOut != nullptr) *lvlOut = lvl;
        return c;
    };

    // Every pitched moment of the lead, not only held notes: stretches of 100 ms or more, without their first 40 ms and last 20 ms, and
    // without moments more than 20 dB under the take's usual sung level (tails that die away under the plug-in's -60 dBFS gate are not notes)
    {
        std::vector<double> sung;
        for (const auto& s : d.stretches) for (int f = s.a; f <= s.b; ++f) sung.push_back (toDb (rmsAround (d.x, d.frameSample (f), half)));
        const double usual = sung.empty() ? -30.0 : measure::median (sung);
        out << "U " << d.take << " " << usual << "\n";
        std::map<std::string, std::array<std::array<int, 6>, 2>> frameCounts;
        std::map<std::string, std::array<std::array<int, numStates>, 2>> frameCauses;
        static const char order[] = "SNOUWX";
        for (const auto& s : d.stretches)
        {
            if (s.b - s.a + 1 < 20) continue;
            for (int f = s.a + 8; f <= s.b - 4; ++f)
            {
                if (toDb (rmsAround (d.x, d.frameSample (f), half)) < usual - 20.0 || d.hissy[(size_t) f]) continue;
                const std::string band = bandOf (d.track[(size_t) f]);
                const int st = trackerStateAt (tr, d, d.frameSample (f));
                for (int v = 0; v < 2; ++v)
                {
                    const char c = classify (f, v);
                    ++frameCounts[band][(size_t) v][(size_t) (std::strchr (order, c) - order)];
                    if (c != 'N') ++frameCauses[band][(size_t) v][(size_t) st];
                    if (c != 'N' && v == 0 && std::getenv ("REALVOX_DUMP") != nullptr)
                        std::printf ("dump %s %.3f s  lead %.1f Hz  level %.1f dBFS  voice %c  tracker %s\n", d.take.c_str(), d.frameTime (f), d.track[(size_t) f],
                                     toDb (rmsAround (d.x, d.frameSample (f), half)), c, stateNames[st]);
                }
            }
        }
        for (const auto& [band, counts] : frameCounts)
            for (int v = 0; v < 2; ++v)
            {
                out << "T " << d.take << " " << band << " " << v + 1;
                for (int k = 0; k < 6; ++k) out << " " << counts[(size_t) v][(size_t) k];
                out << "\n";
                out << "TC " << d.take << " " << band << " " << v + 1;
                for (int k = 0; k < numStates; ++k) out << " " << frameCauses[band][(size_t) v][(size_t) k];
                out << "\n";
            }
    }

    const char* traceAt = std::getenv ("REALVOX_TRACE");
    for (const auto& nt : d.notes)
    {
        const double sec = d.frameTime (nt.a);
        const bool tracing = traceAt != nullptr && std::abs (std::atof (traceAt) - sec) < 0.006;
        // The tracker over the note
        int states[numStates] = {}, first[numStates] = {}, ticks = 0, firstTicks = 0;
        const long from = d.frameSample (nt.a), to = d.frameSample (nt.b);
        for (long s = from; s <= to; s += (long) std::lround (SR * 0.001))
        {
            const int st = trackerStateAt (tr, d, s);
            ++states[st]; ++ticks;
            if (s < from + (long) (0.1 * SR)) { ++first[st]; ++firstTicks; }
            if (tracing && (s - from) % (long) (0.005 * SR) == 0)
                std::printf ("  tracker %+7.1f ms  ref %6.1f  tracker %6.1f  %s\n", 1000.0 * (s - from) / SR, d.leadHzAt (s),
                             tr.hz[(size_t) std::min ((long) tr.hz.size() - 1, (s + (long) (0.012 * SR)) / 48)], stateNames[st]);
        }
        const auto dominant = [] (const int* c)
        {
            int best = 1;
            for (int i = 2; i < numStates; ++i) if (c[i] > c[best]) best = i;
            return c[best] > 0 ? best : 0;
        };

        for (int v = 0; v < 2; ++v)
        {
            const int n = nt.b - nt.a + 1;
            std::string seq ((size_t) n, 'S');
            for (int f = nt.a; f <= nt.b; ++f)
            {
                double hz = 0.0, lvl = 0.0;
                const char c = classify (f, v, &hz, &lvl);
                seq[(size_t) (f - nt.a)] = c;
                if (tracing) std::printf ("  voice %d %+7.1f ms  lead %6.1f  voice %6.1f  lvl %6.1f  %c\n", v + 1, 5.0 * (f - nt.a), d.track[(size_t) f], hz, lvl, c);
            }
            const auto count = [&] (char c, int fromFrame = 0) { int k = 0; for (int i = fromFrame; i < n; ++i) if (seq[(size_t) i] == c) ++k; return k; };
            const auto longestRun = [&] (char c, int fromFrame, int toFrame)
            {
                int run = 0, best = 0;
                for (int i = fromFrame; i < toFrame; ++i) { run = seq[(size_t) i] == c ? run + 1 : 0; best = std::max (best, run); }
                return best;
            };
            int firstOn = -1;
            for (int i = 0; i + 3 < n && firstOn < 0; ++i)
                if (seq[(size_t) i] == 'N' && seq[(size_t) i + 1] == 'N' && seq[(size_t) i + 2] == 'N' && seq[(size_t) i + 3] == 'N') firstOn = i;
            const int nS = count ('S'), nN = count ('N'), body = n - 16;
            std::string verdict = "ok";
            if (nS >= 0.5 * n && nN < 0.25 * n) verdict = "missed";
            else if (longestRun ('O', 0, n) >= 6) verdict = "octave";
            else if (count ('N', 16) < 0.5 * body) verdict = "wrong";
            else if (firstOn >= 0 && longestRun ('S', firstOn, n - 6) >= 6) verdict = "dropout";
            else if (firstOn < 0 || firstOn > 16) verdict = "late";

            std::string cause = "none";
            if (verdict != "ok")
            {
                const double rightAll = ticks > 0 ? (double) states[0] / ticks : 0.0, rightFirst = firstTicks > 0 ? (double) first[0] / firstTicks : 0.0;
                if (nt.hz < tr.lowHz) cause = std::string ("range/") + stateNames[dominant (states)];
                else if (verdict == "late") cause = rightFirst >= 0.8 ? "downstream" : stateNames[dominant (first)];
                else cause = rightAll >= (verdict == "dropout" ? 0.97 : 0.9) ? "downstream" : stateNames[dominant (states)];
            }
            char line[512];
            std::snprintf (line, sizeof line, "N %s %.3f %d %.1f %s %d %d %s %d %d %d %d %d %d %d %s %.3f\n", d.take.c_str(), sec, 5 * n, nt.hz, bandOf (nt.hz), nt.first ? 1 : 0, v + 1,
                           verdict.c_str(), n, nS, nN, count ('O'), count ('U'), count ('W') + count ('X'), firstOn < 0 ? -1 : 5 * firstOn, cause.c_str(),
                           ticks > 0 ? (double) states[0] / ticks : 0.0);
            out << line;
        }
    }
    out.flush();
}

struct NoteTable
{
    std::map<std::string, std::map<std::string, int>> verdicts;   // band, verdict
    std::map<std::string, std::map<std::string, int>> causes;     // band, "verdict cause"
    std::map<std::string, int> total, latency;
    std::map<std::string, double> seconds;
    std::map<std::string, std::array<long, 6>> frames;            // band: silent, on note, octave, unshifted, other pitch, no pitch
    std::map<std::string, std::array<long, numStates>> frameCauses;
    long framesOf (const std::string& band) const { auto f = frames.find (band); long n = 0; if (f != frames.end()) for (long k : f->second) n += k; return n; }
    double onNoteShare (const std::string& band) const { auto f = frames.find (band); const long n = framesOf (band); return n > 0 ? (double) f->second[1] / n : 0.0; }
    double frameShare (const std::string& band, int k) const { auto f = frames.find (band); const long n = framesOf (band); return n > 0 ? (double) f->second[(size_t) k] / n : 0.0; }
    int count (const std::string& band, const std::string& verdict) const
    {
        auto b = verdicts.find (band);
        if (b == verdicts.end()) return 0;
        auto v = b->second.find (verdict);
        return v == b->second.end() ? 0 : v->second;
    }
    int totalOf (const std::string& band) const { auto t = total.find (band); return t == total.end() ? 0 : t->second; }
    double okShare (const std::string& band) const { return totalOf (band) > 0 ? (double) count (band, "ok") / totalOf (band) : 0.0; }
};

NoteTable readNotes (const std::string& path)
{
    NoteTable t;
    std::ifstream in (path);
    std::string line;
    while (std::getline (in, line))
    {
        std::istringstream ls (line);
        std::string tag, take;
        ls >> tag >> take;
        if (tag == "L") { int l = 0; ls >> l; t.latency[take] = l; }
        if (tag == "T" || tag == "TC")
        {
            std::string band; int voice = 0;
            ls >> band >> voice;
            for (const std::string b : { band, std::string ("all") })
            {
                long k = 0;
                std::istringstream copy (line);
                std::string skip; copy >> skip >> skip >> skip >> skip;
                if (tag == "T") { auto& f = t.frames[b]; for (int i = 0; i < 6 && (copy >> k); ++i) f[(size_t) i] += k; }
                else { auto& f = t.frameCauses[b]; for (int i = 0; i < numStates && (copy >> k); ++i) f[(size_t) i] += k; }
            }
        }
        if (tag != "N") continue;
        double sec = 0.0, hz = 0.0; int ms = 0, first = 0, voice = 0; std::string band, verdict, cause;
        int n, nS, nN, nO, nU, nW, firstOn;
        ls >> sec >> ms >> hz >> band >> first >> voice >> verdict >> n >> nS >> nN >> nO >> nU >> nW >> firstOn >> cause;
        for (const std::string b : { band, std::string ("all") })
        {
            ++t.verdicts[b][verdict];
            ++t.total[b];
            t.seconds[b] += 0.001 * ms;
            if (verdict != "ok") ++t.causes[b][cause];
        }
    }
    return t;
}

const char* const bandNames[] = { "under98", "98to130", "over130", "all" };
const char* const verdictNames[] = { "ok", "missed", "octave", "wrong", "dropout", "late" };

void printNotes (const NoteTable& t, const char* label)
{
    std::printf ("%s\n", label);
    for (const auto& [take, l] : t.latency) { std::printf ("  latency %.2f ms (%s", 1000.0 * l / SR, take.c_str()); break; }
    bool same = true;
    for (const auto& [take, l] : t.latency) same = same && l == t.latency.begin()->second;
    std::printf ("%s)\n", same ? ", every take the same" : ", NOT the same on every take");
    std::printf ("  %-9s %7s %7s %7s %7s %7s %7s %7s %8s\n", "band", "notes", "ok", "missed", "octave", "wrong", "dropout", "late", "ok share");
    for (const char* b : bandNames)
    {
        std::printf ("  %-9s %7d", b, t.totalOf (b));
        for (const char* v : verdictNames) std::printf (" %7d", t.count (b, v));
        std::printf (" %8.4f\n", t.okShare (b));
    }
    std::printf ("  (notes are voice notes: each held note counts once for each of the two voices)\n");
    std::printf ("  every pitched moment of the lead (voice seconds), what the voice does while it plays it:\n");
    std::printf ("  %-9s %8s %8s %8s %8s %9s %8s\n", "band", "seconds", "on note", "silent", "octave", "unshifted", "other");
    for (const char* b : bandNames)
        std::printf ("  %-9s %8.1f %8.4f %8.4f %8.4f %9.4f %8.4f\n", b, 0.005 * t.framesOf (b), t.onNoteShare (b), t.frameShare (b, 0), t.frameShare (b, 2), t.frameShare (b, 3),
                     t.frameShare (b, 4) + t.frameShare (b, 5));
    for (const char* b : bandNames)
    {
        auto c = t.frameCauses.find (b);
        if (c == t.frameCauses.end()) continue;
        long bad = 0;
        for (long k : c->second) bad += k;
        std::printf ("  not on note, %s (%.1f s), what the tracker was doing:", b, 0.005 * bad);
        std::vector<std::pair<long, std::string>> sorted;
        for (int i = 0; i < numStates; ++i) if (c->second[(size_t) i] > 0) sorted.push_back ({ c->second[(size_t) i], stateNames[i] });
        std::sort (sorted.rbegin(), sorted.rend());
        for (const auto& [k, name] : sorted) std::printf ("  %s %.0f%%", name.c_str(), 100.0 * k / std::max (1L, bad));
        std::printf ("\n");
    }
    for (const char* b : bandNames)
    {
        auto c = t.causes.find (b);
        if (c == t.causes.end()) continue;
        int bad = 0;
        for (const auto& [name, k] : c->second) bad += k;
        std::printf ("  causes, %s (%d not ok):", b, bad);
        std::vector<std::pair<int, std::string>> sorted;
        for (const auto& [name, k] : c->second) sorted.push_back ({ k, name });
        std::sort (sorted.rbegin(), sorted.rend());
        for (const auto& [k, name] : sorted) std::printf ("  %s %d (%.0f%%)", name.c_str(), k, 100.0 * k / std::max (1, bad));
        std::printf ("\n");
    }
}

int compareNotes (const std::string& newPath, const std::string& oldPath)
{
    const NoteTable n = readNotes (newPath), o = readNotes (oldPath);
    printNotes (n, ("new " + newPath).c_str());
    printNotes (o, ("old " + oldPath).c_str());
    int fails = 0;
    const auto rule = [&] (bool ok, const char* what, double nv, double ov)
    {
        if (! ok) ++fails;
        std::printf ("%s  %s: new %.4f, old %.4f\n", ok ? "PASS" : "FAIL", what, nv, ov);
    };
    const auto countFrom98 = [] (const NoteTable& t, const char* v) { return t.count ("98to130", v) + t.count ("over130", v); };
    const auto octaveShare = [] (const NoteTable& t) { return t.frameShare ("all", 2); };
    rule (n.okShare ("under98") >= o.okShare ("under98") + 0.20, "H1 under 98 Hz, ok share higher by 0.20 or more", n.okShare ("under98"), o.okShare ("under98"));
    rule (n.onNoteShare ("under98") >= o.onNoteShare ("under98") + 0.20, "H2 under 98 Hz, share of time on note higher by 0.20 or more", n.onNoteShare ("under98"), o.onNoteShare ("under98"));
    rule (n.okShare ("98to130") >= o.okShare ("98to130") - 0.002, "H3 98 to 130 Hz, ok share no lower than old minus 0.002", n.okShare ("98to130"), o.okShare ("98to130"));
    rule (n.okShare ("over130") >= o.okShare ("over130") - 0.001, "H4 130 Hz and up, ok share no lower than old minus 0.001", n.okShare ("over130"), o.okShare ("over130"));
    rule (n.onNoteShare ("98to130") >= o.onNoteShare ("98to130") - 0.001, "H5 98 to 130 Hz, share of time on note no lower than old minus 0.001", n.onNoteShare ("98to130"), o.onNoteShare ("98to130"));
    rule (n.onNoteShare ("over130") >= o.onNoteShare ("over130") - 0.001, "H6 130 Hz and up, share of time on note no lower than old minus 0.001", n.onNoteShare ("over130"), o.onNoteShare ("over130"));
    rule (n.count ("all", "octave") <= o.count ("all", "octave") + 2, "H7 octave verdicts, no more than old plus 2", n.count ("all", "octave"), o.count ("all", "octave"));
    rule (octaveShare (n) <= octaveShare (o) + 0.0005, "H8 octave share of time, no more than old plus 0.0005", octaveShare (n), octaveShare (o));
    rule (countFrom98 (n, "late") <= countFrom98 (o, "late") + 2, "H9 late verdicts from 98 Hz up, no more than old plus 2", countFrom98 (n, "late"), countFrom98 (o, "late"));
    bool sameLatency = n.latency.size() == o.latency.size();
    for (const auto& [take, l] : n.latency) { auto it = o.latency.find (take); sameLatency = sameLatency && it != o.latency.end() && it->second == l; }
    rule (sameLatency, "H10 latency of every take the same as old (samples, first take)", n.latency.empty() ? 0.0 : n.latency.begin()->second, o.latency.empty() ? 0.0 : o.latency.begin()->second);
    std::printf ("%s  %d of the held note rules failed\n", fails ? "RESULT FAIL" : "RESULT PASS", fails);
    return fails ? 1 : 0;
}

// =====================================================================================================================================
// Bit for bit: RealVox hash <take.wav> ... renders each take at the plug-in's untouched defaults (stereo in and out, 128 sample blocks),
// or at REALVOX_PRESET and REALVOX_SET, and prints a 64 bit hash of every output sample. With REALVOX_RAW=<folder> the output is also
// written there as raw 32 bit floats, left then right interleaved, for RealVox diff <a> <b>.
// =====================================================================================================================================
int hashTakes (int argc, char** argv)
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    for (int a = 2; a < argc; ++a)
    {
        const auto file = cwd.getChildFile (argv[a]);
        const auto x = loadMono (file);
        if (x.empty()) return 1;
        auto p = makeProcessor();
        applyEnvironment (*p);
        prepare (*p);
        const int n = (int) x.size();
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        std::uint64_t h = 1469598103934665603ull;
        std::vector<float> raw;
        const char* rawDir = std::getenv ("REALVOX_RAW");
        if (rawDir != nullptr) raw.reserve ((size_t) n * 2);
        float peak = 0.0f;
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = std::min (block, n - pos);
            buf.setSize (2, len, false, false, true);
            for (int c = 0; c < 2; ++c) std::copy_n (x.data() + pos, len, buf.getWritePointer (c));
            midi.clear();
            p->processBlock (buf, midi);
            for (int i = 0; i < len; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    const float s = buf.getSample (c, i);
                    std::uint32_t bits;
                    std::memcpy (&bits, &s, 4);
                    for (int k = 0; k < 4; ++k) { h ^= (bits >> (8 * k)) & 0xFFu; h *= 1099511628211ull; }
                    peak = std::max (peak, std::abs (s));
                    if (rawDir != nullptr) raw.push_back (s);
                }
        }
        const char* preset = std::getenv ("REALVOX_PRESET");
        const char* set = std::getenv ("REALVOX_SET");
        float dryPeak = 0.0f;
        for (float v : x) dryPeak = std::max (dryPeak, std::abs (v));
        std::printf ("H %s preset %s set %s latency %d samples %d hash %016llx peak %.2f dBFS (the dry take %.2f, so %+.2f dB)\n", file.getFileNameWithoutExtension().toRawUTF8(),
                     preset != nullptr ? p->getProgramName (std::atoi (preset)).toRawUTF8() : "default", set != nullptr ? set : "none", p->getLatencySamples(), n,
                     (unsigned long long) h, toDb (peak), toDb (dryPeak), toDb (peak) - toDb (dryPeak));
        std::fflush (stdout);
        if (rawDir != nullptr)
        {
            const auto outFile = juce::File (rawDir).getChildFile (file.getFileNameWithoutExtension() + ".f32");
            outFile.deleteFile();
            juce::FileOutputStream os (outFile);
            os.write (raw.data(), raw.size() * sizeof (float));
        }
    }
    return 0;
}

int diffRaw (const juce::File& a, const juce::File& b)
{
    juce::MemoryBlock ma, mb;
    if (! a.loadFileAsData (ma) || ! b.loadFileAsData (mb)) { std::fprintf (stderr, "could not read both files\n"); return 2; }
    const size_t n = std::min (ma.getSize(), mb.getSize()) / (2 * sizeof (float));
    const float* pa = (const float*) ma.getData();
    const float* pb = (const float*) mb.getData();
    long differing = 0, firstAt = -1, lastAt = -1;
    double worst = 0.0, sumD = 0.0, sumA = 0.0;
    std::vector<int> perSecond (n / (size_t) SR + 1, 0);
    for (size_t i = 0; i < n; ++i)
    {
        const double dl = (double) pa[2 * i] - pb[2 * i], dr = (double) pa[2 * i + 1] - pb[2 * i + 1];
        sumD += dl * dl + dr * dr;
        sumA += (double) pb[2 * i] * pb[2 * i] + (double) pb[2 * i + 1] * pb[2 * i + 1];
        if (dl != 0.0 || dr != 0.0)
        {
            ++differing;
            if (firstAt < 0) firstAt = (long) i;
            lastAt = (long) i;
            worst = std::max ({ worst, std::abs (dl), std::abs (dr) });
            ++perSecond[i / (size_t) SR];
        }
    }
    if (ma.getSize() != mb.getSize()) std::printf ("lengths differ: %zu and %zu bytes\n", ma.getSize(), mb.getSize());
    if (differing == 0) { std::printf ("IDENTICAL %s: %zu samples a side, bit for bit\n", a.getFileNameWithoutExtension().toRawUTF8(), n); return 0; }
    int seconds = 0;
    for (int k : perSecond) if (k > 0) ++seconds;
    std::printf ("DIFFERENT %s: %ld of %zu samples differ (%.3f%%), in %d of %zu seconds, first at %.3f s, last at %.3f s, largest %.1f dBFS, difference %.1f dB against the old render\n",
                 a.getFileNameWithoutExtension().toRawUTF8(), differing, n, 100.0 * differing / n, seconds, perSecond.size(), firstAt / SR, lastAt / SR, toDb (worst),
                 10.0 * std::log10 (std::max (sumD, 1.0e-30) / std::max (sumA, 1.0e-30)));
    if (std::getenv ("REALVOX_DIFF_SECONDS") != nullptr)
    {
        std::printf ("  seconds with a difference:");
        for (size_t s = 0; s < perSecond.size(); ++s) if (perSecond[s] > 0) std::printf (" %zu", s);
        std::printf ("\n");
    }
    return 1;
}

// =====================================================================================================================================
// Robot or human (added for 1.5.0). Luke: "the voices sound robotic like really robotic".
//
//   RealVox robot <records file> <take.wav> ...     how each voice moves against how Luke himself moves, note by note
//   RealVox robotreport <records file>              pooled numbers
//
// Same render as the held notes (singing only, voices apart), plus the default render for the consonants. Everything is measured the same
// way on the lead and on a voice: same estimator, same window length (3 periods of the lead), same frames.
//   P  held notes of 400 ms or more, their body (120 ms in to 60 ms before the end), per voice: pitch spread (SD in cents) of lead and voice,
//      split at 150 ms into slow and fast movement; how far the voice's movement follows the lead's (correlation); the voice's average
//      against its harmony note and the lead's against its own note (cents); strongest steady wobble from 3 to 8 Hz (rate, depth, and how
//      much of the movement it is: 1 is a pure sine); spread of the voice's level against the lead's (dB) and how alike the two level
//      curves are (correlation).
//   Q  the same notes, voice 1 against voice 2: correlation of their pitch movement, and the spread of the interval between them (cents).
//   G  note changes inside a stretch (150 cents or more, 120 ms or less between two held notes): time from 10% to 90% of the way, lead and
//      each voice, and how long after the lead the voice passes half way.
//   E  phrase starts: when the lead and each voice first reach a tenth of the level they hold 100 to 250 ms in. F: the same at the end of
//      a stretch followed by 250 ms or more without pitch.
//   Z  how far a voice's spectrum shape sits from the lead's, as a scale on frequency (1.00 is the same throat).
//   X  S sounds (default render): how alike two voices' S are at the best lag within 60 ms, each voice's against the lead's, and level.
// A real double of the same singer, for comparison: Luke sang with the track, so a chorus he sang twice lines up with itself at one fixed
// lag. The lag (8 s or more) at which most of the take's pitch matches itself within 70 cents is found, and where at least 60% of 2 s
// matches, the two passes are compared the way a voice is compared with the lead: DN held notes (average pitch apart, movement in
// common, spread of the interval, level), DO starts and DE ends (how far apart in time), ZD spectrum shape.
// =====================================================================================================================================

double meanOf (const std::vector<double>& v) { double s = 0; int n = 0; for (double x : v) if (std::isfinite (x)) { s += x; ++n; } return n > 0 ? s / n : std::nan (""); }
double sdOf (const std::vector<double>& v)
{
    const double m = meanOf (v);
    double s = 0; int n = 0;
    for (double x : v) if (std::isfinite (x)) { s += (x - m) * (x - m); ++n; }
    return n > 1 ? std::sqrt (s / n) : std::nan ("");
}
double corrOf (const std::vector<double>& a, const std::vector<double>& b)
{
    double ma = 0, mb = 0; int n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (std::isfinite (a[i]) && std::isfinite (b[i])) { ma += a[i]; mb += b[i]; ++n; }
    if (n < 8) return std::nan ("");
    ma /= n; mb /= n;
    double c = 0, ea = 0, eb = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (std::isfinite (a[i]) && std::isfinite (b[i])) { c += (a[i] - ma) * (b[i] - mb); ea += (a[i] - ma) * (a[i] - ma); eb += (b[i] - mb) * (b[i] - mb); }
    return ea > 1.0e-12 && eb > 1.0e-12 ? c / std::sqrt (ea * eb) : std::nan ("");
}
double validShare (const std::vector<double>& v) { int n = 0; for (double x : v) if (std::isfinite (x)) ++n; return v.empty() ? 0.0 : (double) n / v.size(); }
std::vector<double> movingAverage (const std::vector<double>& v, int width)
{
    std::vector<double> out (v.size(), std::nan (""));
    const int half = width / 2;
    for (int i = 0; i < (int) v.size(); ++i)
    {
        double s = 0; int n = 0;
        for (int k = std::max (0, i - half); k <= std::min ((int) v.size() - 1, i + half); ++k) if (std::isfinite (v[(size_t) k])) { s += v[(size_t) k]; ++n; }
        if (n >= half) out[(size_t) i] = s / n;
    }
    return out;
}
std::vector<double> minus (const std::vector<double>& a, const std::vector<double>& b)
{
    std::vector<double> out (a.size(), std::nan (""));
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (std::isfinite (a[i]) && std::isfinite (b[i])) out[i] = a[i] - b[i];
    return out;
}

// Strongest steady wobble from 3 to 8 Hz in a pitch series at 5 ms a frame: rate, depth in cents, and its share of the movement (1 is a pure sine)
void wobble (const std::vector<double>& series, double& rateHz, double& depth, double& regular)
{
    rateHz = depth = regular = std::nan ("");
    const double sd = sdOf (series);
    if (! std::isfinite (sd) || series.size() < 84) return;
    depth = 0.0;
    for (double hz = 3.0; hz <= 8.01; hz += 0.25)
    {
        const double dpt = measure::sineDepth (series, hopSec, hz);
        if (dpt > depth) { depth = dpt; rateHz = hz; }
    }
    regular = sd > 1.0e-9 ? std::min (1.5, depth / (std::sqrt (2.0) * sd)) : std::nan ("");
}

// Pitch between two limits over a window of a set length, shortest strong period first. 0 if nothing periodic.
double pitchWide (const std::vector<float>& y, long centre, double loHz, double hiHz, int win)
{
    const int tMin = std::max (4, (int) std::floor (SR / hiHz)), tMax = (int) std::ceil (SR / loHz);
    const long start = centre - (win + tMax) / 2;
    if (start < 0 || start + win + tMax + 2 >= (long) y.size()) return 0.0;
    const float* a = y.data() + start;
    double ea = 0.0;
    for (int i = 0; i < win; i += 2) ea += (double) a[i] * a[i];
    std::vector<double> r ((size_t) tMax + 3, -2.0);
    double best = -2.0;
    for (int lag = tMin - 1; lag <= tMax + 1; ++lag)
    {
        double c = 0.0, eb = 0.0;
        for (int i = 0; i < win; i += 2) { c += (double) a[i] * a[i + lag]; eb += (double) a[i + lag] * a[i + lag]; }
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

double centsOf (double hz) { return hz > 0.0 ? 1200.0 * std::log2 (hz / 440.0) : std::nan (""); }

// First (or last) moment a signal's 10 ms level is a tenth of a reference level or more, searched in 1 ms steps. -1 if never.
long levelEdge (const std::vector<float>& s, long from, long to, double refRms, bool last)
{
    const long step = (long) (0.001 * SR), len = (long) (0.01 * SR);
    long found = -1;
    for (long t = from; t <= to; t += step)
        if (rmsOf (s, t - len / 2, len) >= 0.1 * refRms) { found = t; if (! last) break; }
    return found;
}

struct Double2 { long lag = 0; std::vector<char> inRegion; double matchedSec = 0.0; };

// Where the take repeats itself: one lag for the whole take, and the frames where 2 s around them match
Double2 findDouble (const Dry& d)
{
    Double2 out;
    const int F = d.frames();
    std::vector<float> semis ((size_t) F, -1000.0f);
    for (int f = 0; f < F; ++f) if (d.track[(size_t) f] > 0.0) semis[(size_t) f] = (float) (12.0 * std::log2 (d.track[(size_t) f] / 440.0));
    int bestLag = 0, bestCount = 0;
    for (int lag = 1600; lag < F - 1600; lag += 2)
    {
        int count = 0;
        for (int f = 0; f + lag < F; f += 2)
            if (semis[(size_t) f] > -900.0f && semis[(size_t) (f + lag)] > -900.0f && std::abs (semis[(size_t) f] - semis[(size_t) (f + lag)]) < 0.7f) ++count;
        if (count > bestCount) { bestCount = count; bestLag = lag; }
    }
    if (bestCount < 600) return out;   // under 6 s in common
    for (int lag = bestLag - 1; lag <= bestLag + 1; lag += 2)
    {
        int count = 0;
        for (int f = 0; f + lag < F; f += 2)
            if (semis[(size_t) f] > -900.0f && semis[(size_t) (f + lag)] > -900.0f && std::abs (semis[(size_t) f] - semis[(size_t) (f + lag)]) < 0.7f) ++count;
        if (count > bestCount) { bestCount = count; bestLag = lag; }
    }
    out.lag = bestLag;
    out.inRegion.assign ((size_t) F, 0);
    for (int f = 200; f + bestLag + 200 < F; ++f)
    {
        int voicedA = 0, match = 0;
        for (int k = f - 200; k <= f + 200; k += 2)
        {
            if (semis[(size_t) k] > -900.0f) ++voicedA;
            if (semis[(size_t) k] > -900.0f && semis[(size_t) (k + bestLag)] > -900.0f && std::abs (semis[(size_t) k] - semis[(size_t) (k + bestLag)]) < 0.7f) ++match;
        }
        if (voicedA >= 40 && match >= 0.6 * voicedA) { out.inRegion[(size_t) f] = 1; out.matchedSec += hopSec; }
    }
    return out;
}

void robotTake (const juce::File& file, std::ofstream& out)
{
    Dry d;
    if (! d.load (file)) { std::fprintf (stderr, "could not load %s\n", file.getFullPathName().toRawUTF8()); return; }
    std::printf ("  %s: %.1f s, %d held notes, rendering\n", d.take.c_str(), d.x.size() / SR, (int) d.notes.size());
    std::fflush (stdout);
    const Render y = render (d.x, false, d.key, d.scale, 440.0);
    const Render z = render (d.x, true, d.key, d.scale, 440.0);
    const long L = z.latency, N = (long) d.x.size();
    const int half = (int) (0.005 * SR);
    out << "L " << d.take << " " << L << "\n";
    char line[768];

    struct Body { std::vector<double> lead, voice[2], leadDb, voiceDb[2]; };
    const auto bodyOf = [&] (const Note& nt, const std::vector<float>& leadSig, long leadShift, bool voicesToo)
    {
        Body b;
        for (int f = nt.a + 24; f <= nt.b - 12; ++f)
        {
            const long i = d.frameSample (f);
            const double ref = d.track[(size_t) f];
            const double hz = measure::pitchNear (leadSig.data(), (long) leadSig.size(), i + leadShift, leadShift == 0 ? ref : d.leadHzAt (i + leadShift) > 0.0 ? d.leadHzAt (i + leadShift) : ref, SR, 0.06, 3.0);
            b.lead.push_back (centsOf (hz));
            b.leadDb.push_back (toDb (rmsAround (leadSig, i + leadShift, half)));
            if (! voicesToo) continue;
            for (int v = 0; v < 2; ++v)
            {
                const long at = i + L + (long) std::lround (z.lagSamplesAt (v, i + L));
                double fine = 0.0;
                if (at + half < N)
                {
                    const double coarse = pitchAround (z.ch[(size_t) v], at, d.searchAround (ref, v));
                    if (coarse > 0.0 && d.onNote (coarse, ref, v))
                        fine = measure::pitchNear (z.ch[(size_t) v].data(), N, at, coarse, SR, 0.06, 3.0, 3.0 / ref);
                }
                b.voice[v].push_back (centsOf (fine));
                b.voiceDb[v].push_back (at + half < N ? toDb (rmsAround (z.ch[(size_t) v], at, half)) : std::nan (""));
            }
        }
        return b;
    };
    const auto split = [] (const std::vector<double>& s, double& slow, double& fast)
    {
        const auto avg = movingAverage (s, 31);
        slow = sdOf (avg);
        fast = sdOf (minus (s, avg));
    };

    // P and Q: held notes
    for (const auto& nt : d.notes)
    {
        const int n = nt.b - nt.a + 1;
        if (n < 80) continue;
        const Body b = bodyOf (nt, d.x, 0, true);
        if (validShare (b.lead) < 0.7) continue;
        const double sec = d.frameTime (nt.a);
        double slowL, fastL, rateL, depthL, regL;
        split (b.lead, slowL, fastL);
        if (n >= 120) wobble (b.lead, rateL, depthL, regL); else rateL = depthL = regL = std::nan ("");
        const double m = d.semitoneOf (nt.hz);
        double leadOff = 1.0e9;
        for (int c : d.leadNotes (m)) if (std::abs (100.0 * (m - c)) < std::abs (leadOff)) leadOff = 100.0 * (m - c);
        for (int v = 0; v < 2; ++v)
        {
            if (validShare (b.voice[v]) < 0.7) continue;
            double slowV, fastV, rateV, depthV, regV;
            split (b.voice[v], slowV, fastV);
            if (n >= 120) wobble (b.voice[v], rateV, depthV, regV); else rateV = depthV = regV = std::nan ("");
            const double meanVoiceHz = 440.0 * std::pow (2.0, meanOf (b.voice[v]) / 1200.0);
            const double meanLeadHz = 440.0 * std::pow (2.0, meanOf (b.lead) / 1200.0);
            const double voiceOff = d.centsOffHarmony (meanVoiceHz, meanLeadHz, v);
            const auto levelDiff = minus (b.voiceDb[v], b.leadDb);
            std::snprintf (line, sizeof line, "P %s %.3f %d %.1f %d %.2f %.2f %.2f %.2f %.2f %.2f %.3f %.2f %.2f %.2f %.2f %.3f %.2f %.2f %.3f %.3f %.3f\n", d.take.c_str(), sec, 5 * n, nt.hz, v + 1,
                           sdOf (b.lead), sdOf (b.voice[v]), fastL, fastV, slowL, slowV, corrOf (b.voice[v], b.lead), voiceOff, leadOff,
                           rateL, depthL, regL, rateV, depthV, regV, sdOf (levelDiff), corrOf (b.voiceDb[v], b.leadDb));
            out << line;
        }
        if (validShare (b.voice[0]) >= 0.7 && validShare (b.voice[1]) >= 0.7)
        {
            std::snprintf (line, sizeof line, "Q %s %.3f %d %.1f %.3f %.2f\n", d.take.c_str(), sec, 5 * n, nt.hz, corrOf (b.voice[0], b.voice[1]), sdOf (minus (b.voice[0], b.voice[1])));
            out << line;
        }
    }

    // G: note changes
    const auto glide = [&] (const std::vector<float>& sig, long tFrom, long tTo, long shift, double c1, double c2, double lo, double hi, int win, double& ms, long& t50)
    {
        std::vector<std::pair<long, double>> pts;
        for (long t = tFrom; t <= tTo; t += (long) (0.002 * SR))
        {
            const double hz = pitchWide (sig, t + shift, lo, hi, win);
            if (hz > 0.0) pts.push_back ({ t, (centsOf (hz) - c1) / (c2 - c1) });
        }
        ms = std::nan (""); t50 = -1;
        int i50 = -1;
        for (int i = 0; i + 3 < (int) pts.size() && i50 < 0; ++i)
            if (pts[(size_t) i].second >= 0.5 && pts[(size_t) i + 1].second >= 0.5 && pts[(size_t) i + 2].second >= 0.5 && pts[(size_t) i + 3].second >= 0.5) i50 = i;
        if (i50 <= 0) return;
        int i10 = i50, i90 = i50;
        while (i10 >= 0 && pts[(size_t) i10].second > 0.1) --i10;
        while (i90 < (int) pts.size() && pts[(size_t) i90].second < 0.9) ++i90;
        if (i10 < 0 || i90 >= (int) pts.size()) return;
        ms = 1000.0 * (double) (pts[(size_t) i90].first - pts[(size_t) i10].first) / SR;
        t50 = pts[(size_t) i50].first;
    };
    const auto voiceMedian = [&] (const Note& nt, int v)
    {
        std::vector<double> c;
        for (int f = nt.a + 16; f <= nt.b - 6; ++f)
        {
            const long i = d.frameSample (f);
            const long at = i + L + (long) std::lround (z.lagSamplesAt (v, i + L));
            if (at + half >= N) continue;
            const double hz = pitchAround (z.ch[(size_t) v], at, d.searchAround (d.track[(size_t) f], v));
            if (hz > 0.0 && d.onNote (hz, d.track[(size_t) f], v)) c.push_back (centsOf (hz));
        }
        return (int) c.size() >= 8 ? measure::median (c) : std::nan ("");
    };
    for (size_t k = 0; k + 1 < d.notes.size(); ++k)
    {
        const auto& n1 = d.notes[k];
        const auto& n2 = d.notes[k + 1];
        if (n2.a - n1.b > 24 || n2.a <= n1.b) continue;
        bool sameStretch = true;
        for (int f = n1.b; f <= n2.a; ++f) if (d.track[(size_t) f] <= 0.0) sameStretch = false;
        const double interval = measure::cents (n2.hz, n1.hz);
        if (! sameStretch || std::abs (interval) < 150.0) continue;
        const long tFrom = d.frameSample (n1.b) - (long) (0.15 * SR), tTo = d.frameSample (n2.a) + (long) (0.2 * SR);
        const int win = (int) (2.5 * SR / std::min (n1.hz, n2.hz));
        double leadMs; long lead50;
        glide (d.x, tFrom, tTo, 0, centsOf (n1.hz), centsOf (n2.hz), std::min (n1.hz, n2.hz) * 0.84, std::max (n1.hz, n2.hz) * 1.19, win, leadMs, lead50);
        if (! std::isfinite (leadMs)) continue;
        for (int v = 0; v < 2; ++v)
        {
            const double c1 = voiceMedian (n1, v), c2 = voiceMedian (n2, v);
            if (! std::isfinite (c1) || ! std::isfinite (c2) || std::abs (c2 - c1) < 80.0) continue;
            const double h1 = 440.0 * std::pow (2.0, c1 / 1200.0), h2 = 440.0 * std::pow (2.0, c2 / 1200.0);
            const long shift = L + (long) std::lround (z.lagSamplesAt (v, d.frameSample (n2.a) + L));
            double voiceMs; long voice50;
            glide (z.ch[(size_t) v], tFrom, tTo + (long) (0.1 * SR), shift, c1, c2, std::min (h1, h2) * 0.84, std::max (h1, h2) * 1.19, win, voiceMs, voice50);
            std::snprintf (line, sizeof line, "G %s %.3f %.0f %.1f %d %.1f %.1f %.0f\n", d.take.c_str(), d.frameTime (n2.a), interval, leadMs, v + 1, voiceMs,
                           voice50 >= 0 ? 1000.0 * (voice50 - lead50) / SR : std::nan (""), c2 - c1);
            out << line;
        }
    }

    // E and F: starts and ends
    for (size_t si = 0; si < d.stretches.size(); ++si)
    {
        const auto& s = d.stretches[si];
        if (s.b - s.a + 1 < 60) continue;
        const int gapBefore = si == 0 ? s.a : s.a - d.stretches[si - 1].b - 1;
        const int gapAfter = si + 1 == d.stretches.size() ? 1000 : d.stretches[si + 1].a - s.b - 1;
        if (gapBefore >= 50 && s.onset - (long) (0.1 * SR) >= 0)
        {
            const long a = s.onset + (long) (0.1 * SR), len = (long) (0.15 * SR);
            const long tl = levelEdge (d.x, s.onset - (long) (0.04 * SR), s.onset + (long) (0.3 * SR), rmsOf (d.x, a, len), false);
            double tv[2];
            for (int v = 0; v < 2; ++v)
            {
                const long shift = L + (long) std::lround (z.lagSamplesAt (v, s.onset + L));
                const long t = levelEdge (z.ch[(size_t) v], s.onset + L - (long) (0.04 * SR), s.onset + L + (long) (0.3 * SR), rmsOf (z.ch[(size_t) v], a + shift, len), false);
                tv[v] = t < 0 ? std::nan ("") : 1000.0 * (t - L - s.onset) / SR;
            }
            std::snprintf (line, sizeof line, "E %s %.3f %.1f %.1f %.1f\n", d.take.c_str(), s.onset / SR, tl < 0 ? std::nan ("") : 1000.0 * (tl - s.onset) / SR, tv[0], tv[1]);
            out << line;
        }
        if (gapAfter >= 50 && s.end + (long) (0.5 * SR) < N)
        {
            const long a = s.end - (long) (0.2 * SR), len = (long) (0.15 * SR);
            const long tl = levelEdge (d.x, s.end - (long) (0.1 * SR), s.end + (long) (0.4 * SR), rmsOf (d.x, a, len), true);
            double tv[2];
            for (int v = 0; v < 2; ++v)
            {
                const long shift = L + (long) std::lround (z.lagSamplesAt (v, s.end + L));
                const long t = levelEdge (z.ch[(size_t) v], s.end + L - (long) (0.1 * SR), s.end + L + (long) (0.4 * SR), rmsOf (z.ch[(size_t) v], a + shift, len), true);
                tv[v] = t < 0 ? std::nan ("") : 1000.0 * (t - L - s.end) / SR;
            }
            std::snprintf (line, sizeof line, "F %s %.3f %.1f %.1f %.1f\n", d.take.c_str(), s.end / SR, tl < 0 ? std::nan ("") : 1000.0 * (tl - s.end) / SR, tv[0], tv[1]);
            out << line;
        }
    }

    // Z: spectrum shape of each voice against the lead, and of voice 2 against voice 1
    {
        const auto lead = measure::smoothSpectrumDb (d.x.data(), N, SR, 250.0);
        const auto v1 = measure::smoothSpectrumDb (z.ch[0].data(), N, SR, 250.0), v2 = measure::smoothSpectrumDb (z.ch[1].data(), N, SR, 250.0);
        out << "Z " << d.take << " 1 " << measure::shapeScale (lead, v1, 300.0, 3500.0) << "\n";
        out << "Z " << d.take << " 2 " << measure::shapeScale (lead, v2, 300.0, 3500.0) << "\n";
        out << "Z " << d.take << " 12 " << measure::shapeScale (v1, v2, 300.0, 3500.0) << "\n";
    }

    // X: S sounds on the default render
    {
        const auto highOf = [] (const std::vector<float>& s)
        {
            smoothharmony::Biquad hp;
            hp.highPass (3500.0, SR, 0.707);
            std::vector<float> o (s.size());
            for (size_t i = 0; i < s.size(); ++i) o[i] = (float) hp.process (s[i]);
            return o;
        };
        const auto xh = highOf (d.x), y1 = highOf (y.ch[0]), y2 = highOf (y.ch[1]);
        const int frame = (int) (0.01 * SR);
        std::vector<std::pair<long, long>> regions;
        long runFrom = -1;
        for (long i = 0; i + frame < N; i += frame)
        {
            const double all = rmsOf (d.x, i, frame), high = rmsOf (xh, i, frame);
            const bool s = all > std::pow (10.0, -50.0 / 20.0) && high * high >= 0.6 * all * all;
            if (s && runFrom < 0) runFrom = i;
            if (! s && runFrom >= 0) { if (i - runFrom >= 4 * frame) regions.push_back ({ runFrom, i }); runFrom = -1; }
        }
        const auto best = [] (const float* a, const float* b, int n, int minLag, int maxLag)
        {
            double ea = 0.0;
            for (int i = 0; i < n; i += 2) ea += (double) a[i] * a[i];
            double top = 0.0;
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                double c = 0.0, eb = 0.0;
                for (int i = 0; i < n; i += 2) { c += (double) a[i] * b[i + lag]; eb += (double) b[i + lag] * b[i + lag]; }
                const double den = std::sqrt (ea * eb);
                if (den > 1.0e-12) top = std::max (top, std::abs (c) / den);
            }
            return top;
        };
        const size_t stepBy = std::max<size_t> (1, regions.size() / 60);
        for (size_t r = 0; r < regions.size(); r += stepBy)
        {
            const long s = regions[r].first, e = std::min (regions[r].second, regions[r].first + (long) (0.08 * SR));
            const int n = (int) (e - s), reach = (int) (0.06 * SR), far = (int) (0.12 * SR);
            if (s - reach < 0 || e + L + far + reach + 8 >= N) continue;
            const double c12 = best (y1.data() + s + L + reach, y2.data() + s + L + reach, n, -reach, reach);
            const double c1 = best (xh.data() + s, y1.data() + s + L, n, 0, far), c2 = best (xh.data() + s, y2.data() + s + L, n, 0, far);
            const double in = rmsOf (xh, s, e - s);
            std::snprintf (line, sizeof line, "X %s %.3f %.3f %.3f %.3f %.1f %.1f\n", d.take.c_str(), s / SR, c12, c1, c2,
                           toDb (rmsOf (y1, s + L, e - s + reach)) - toDb (in), toDb (rmsOf (y2, s + L, e - s + reach)) - toDb (in));
            out << line;
        }
    }

    // A real double: the take against itself where it repeats
    const Double2 dbl = findDouble (d);
    std::snprintf (line, sizeof line, "DD %s %.3f %.1f\n", d.take.c_str(), dbl.lag * hopSec, dbl.matchedSec);
    out << line;
    if (dbl.lag > 0 && dbl.matchedSec >= 6.0)
    {
        const long lagSamples = (long) std::lround (dbl.lag * hopSec * SR);
        for (const auto& nt : d.notes)
        {
            const int n = nt.b - nt.a + 1;
            if (n < 80 || ! dbl.inRegion[(size_t) nt.a] || ! dbl.inRegion[(size_t) nt.b]) continue;
            const Body a = bodyOf (nt, d.x, 0, false), b = bodyOf (nt, d.x, lagSamples, false);
            // the second pass must be on the same note for the body
            int same = 0;
            for (size_t i = 0; i < a.lead.size(); ++i) if (std::isfinite (a.lead[i]) && std::isfinite (b.lead[i]) && std::abs (a.lead[i] - b.lead[i]) < 150.0) ++same;
            if (same < 0.7 * (double) a.lead.size()) continue;
            std::vector<double> bl = b.lead;
            for (size_t i = 0; i < bl.size(); ++i) if (std::isfinite (a.lead[i]) && std::isfinite (bl[i]) && std::abs (a.lead[i] - bl[i]) >= 150.0) bl[i] = std::nan ("");
            std::snprintf (line, sizeof line, "DN %s %.3f %d %.1f %.2f %.3f %.2f %.2f %.3f\n", d.take.c_str(), d.frameTime (nt.a), 5 * n, nt.hz, meanOf (bl) - meanOf (a.lead),
                           corrOf (a.lead, bl), sdOf (minus (bl, a.lead)), sdOf (minus (b.leadDb, a.leadDb)), corrOf (a.leadDb, b.leadDb));
            out << line;
        }
        for (size_t si = 0; si < d.stretches.size(); ++si)
        {
            const auto& s = d.stretches[si];
            if (s.b - s.a + 1 < 60 || ! dbl.inRegion[(size_t) s.a]) continue;
            const int gapBefore = si == 0 ? s.a : s.a - d.stretches[si - 1].b - 1;
            const int gapAfter = si + 1 == d.stretches.size() ? 1000 : d.stretches[si + 1].a - s.b - 1;
            for (size_t sj = si + 1; sj < d.stretches.size(); ++sj)
            {
                const auto& t = d.stretches[sj];
                const int gapBeforeT = t.a - d.stretches[sj - 1].b - 1;
                const int gapAfterT = sj + 1 == d.stretches.size() ? 1000 : d.stretches[sj + 1].a - t.b - 1;
                if (gapBefore >= 50 && gapBeforeT >= 50 && std::abs ((t.onset - s.onset) - lagSamples) < (long) (0.15 * SR) && s.onset - (long) (0.1 * SR) >= 0)
                {
                    const long len = (long) (0.15 * SR);
                    const long ta = levelEdge (d.x, s.onset - (long) (0.04 * SR), s.onset + (long) (0.3 * SR), rmsOf (d.x, s.onset + (long) (0.1 * SR), len), false);
                    const long tb = levelEdge (d.x, t.onset - (long) (0.04 * SR), t.onset + (long) (0.3 * SR), rmsOf (d.x, t.onset + (long) (0.1 * SR), len), false);
                    if (ta >= 0 && tb >= 0) { std::snprintf (line, sizeof line, "DO %s %.3f %.1f\n", d.take.c_str(), s.onset / SR, 1000.0 * (tb - ta - lagSamples) / SR); out << line; }
                }
                if (gapAfter >= 50 && gapAfterT >= 50 && std::abs ((t.end - s.end) - lagSamples) < (long) (0.15 * SR) && t.end + (long) (0.5 * SR) < N)
                {
                    const long len = (long) (0.15 * SR);
                    const long ta = levelEdge (d.x, s.end - (long) (0.1 * SR), s.end + (long) (0.4 * SR), rmsOf (d.x, s.end - (long) (0.2 * SR), len), true);
                    const long tb = levelEdge (d.x, t.end - (long) (0.1 * SR), t.end + (long) (0.4 * SR), rmsOf (d.x, t.end - (long) (0.2 * SR), len), true);
                    if (ta >= 0 && tb >= 0) { std::snprintf (line, sizeof line, "DE %s %.3f %.1f\n", d.take.c_str(), s.end / SR, 1000.0 * (tb - ta - lagSamples) / SR); out << line; }
                }
            }
        }
        // Spectrum shape of the second pass against the first, over the matched part
        int fa = -1, fb = -1;
        for (int f = 0; f < d.frames(); ++f) if (dbl.inRegion[(size_t) f]) { if (fa < 0) fa = f; fb = f; }
        if (fa >= 0)
        {
            const long sa = d.frameSample (fa), len = std::min (d.frameSample (fb) - sa, N - lagSamples - sa - 1);
            if (len > (long) (4.0 * SR))
            {
                const auto a = measure::smoothSpectrumDb (d.x.data() + sa, len, SR, 250.0), b = measure::smoothSpectrumDb (d.x.data() + sa + lagSamples, len, SR, 250.0);
                out << "ZD " << d.take << " " << measure::shapeScale (a, b, 300.0, 3500.0) << "\n";
            }
        }
    }
    out.flush();
}

struct Pool
{
    std::map<std::string, std::vector<double>> v;
    void add (const std::string& k, double x) { if (std::isfinite (x)) v[k].push_back (x); }
    double med (const std::string& k) const { auto it = v.find (k); return it == v.end() || it->second.empty() ? std::nan ("") : measure::median (it->second); }
    double pct (const std::string& k, double p) const { auto it = v.find (k); return it == v.end() || it->second.empty() ? std::nan ("") : measure::percentile (it->second, p); }
    double mean (const std::string& k) const { auto it = v.find (k); return it == v.end() ? std::nan ("") : meanOf (it->second); }
    int n (const std::string& k) const { auto it = v.find (k); return it == v.end() ? 0 : (int) it->second.size(); }
};

Pool readRobot (const std::string& path)
{
    Pool p;
    std::ifstream in (path);
    std::string lineText;
    const auto num = [] (std::istringstream& ls) { std::string s; ls >> s; return s == "nan" || s.empty() ? std::nan ("") : std::atof (s.c_str()); };
    while (std::getline (in, lineText))
    {
        std::istringstream ls (lineText);
        std::string tag, take;
        ls >> tag >> take;
        if (tag == "P")
        {
            const double sec = num (ls), ms = num (ls), hz = num (ls); const int v = (int) num (ls);
            juce::ignoreUnused (sec, ms, hz);
            const double sdL = num (ls), sdV = num (ls), fastL = num (ls), fastV = num (ls), slowL = num (ls), slowV = num (ls), corr = num (ls), voiceOff = num (ls), leadOff = num (ls);
            const double rateL = num (ls), depthL = num (ls), regL = num (ls), rateV = num (ls), depthV = num (ls), regV = num (ls), lvlSd = num (ls), lvlCorr = num (ls);
            if (v == 1) { p.add ("lead sd", sdL); p.add ("lead fast", fastL); p.add ("lead slow", slowL); p.add ("lead off", std::abs (leadOff)); p.add ("lead wobble rate", rateL); p.add ("lead wobble depth", depthL); p.add ("lead wobble share", regL); }
            p.add ("voice sd", sdV); p.add ("voice fast", fastV); p.add ("voice slow", slowV); p.add ("voice follows lead", corr);
            p.add ("voice off", std::abs (voiceOff)); p.add ("voice off signed", voiceOff);
            p.add (v == 1 ? "voice 1 off signed" : "voice 2 off signed", voiceOff);
            p.add ("voice wobble rate", rateV); p.add ("voice wobble depth", depthV); p.add ("voice wobble share", regV);
            p.add ("voice level sd", lvlSd); p.add ("voice level corr", lvlCorr);
        }
        else if (tag == "Q") { num (ls); num (ls); num (ls); p.add ("voices move together", num (ls)); p.add ("interval sd", num (ls)); }
        else if (tag == "G")
        {
            num (ls); const double cents = num (ls), leadMs = num (ls); const int v = (int) num (ls); const double voiceMs = num (ls), delay = num (ls);
            juce::ignoreUnused (cents);
            if (v == 1) p.add ("lead glide", leadMs);
            p.add ("voice glide", voiceMs); p.add ("voice glide delay", delay);
            if (std::isfinite (voiceMs)) p.add ("lead glide where voice measured", leadMs);
        }
        else if (tag == "E")
        {
            num (ls); const double tl = num (ls), t1 = num (ls), t2 = num (ls);
            p.add ("start voice 1 after lead", t1 - tl); p.add ("start voice 2 after lead", t2 - tl); p.add ("start voices apart", std::abs (t1 - t2));
            p.add ("start voice after lead", t1 - tl); p.add ("start voice after lead", t2 - tl);
        }
        else if (tag == "F")
        {
            num (ls); const double tl = num (ls), t1 = num (ls), t2 = num (ls);
            p.add ("end voice 1 after lead", t1 - tl); p.add ("end voice 2 after lead", t2 - tl); p.add ("end voices apart", std::abs (t1 - t2));
        }
        else if (tag == "Z") { std::string which; ls >> which; p.add ("shape " + which, num (ls)); }
        else if (tag == "ZD") p.add ("double shape", num (ls));
        else if (tag == "X") { num (ls); p.add ("s voices alike", num (ls)); p.add ("s voice like lead", num (ls)); p.add ("s voice like lead", num (ls)); p.add ("s level", num (ls)); p.add ("s level", num (ls)); }
        else if (tag == "DD") { num (ls); p.add ("double seconds", num (ls)); }
        else if (tag == "DN")
        {
            num (ls); num (ls); num (ls);
            const double apart = num (ls), corr = num (ls), isd = num (ls), lsd = num (ls), lcorr = num (ls);
            p.add ("double apart", std::abs (apart)); p.add ("double moves together", corr); p.add ("double interval sd", isd); p.add ("double level sd", lsd); p.add ("double level corr", lcorr);
        }
        else if (tag == "DO") { num (ls); p.add ("double start", num (ls)); }
        else if (tag == "DE") { num (ls); p.add ("double end", num (ls)); }
    }
    return p;
}

double spreadAboutMedian (const Pool& p, const std::string& k)
{
    auto it = p.v.find (k);
    if (it == p.v.end() || it->second.empty()) return std::nan ("");
    const double m = measure::median (it->second);
    std::vector<double> a;
    for (double x : it->second) a.push_back (std::abs (x - m));
    return measure::median (a);
}

void printRobot (const Pool& p, const char* label)
{
    std::printf ("%s\n", label);
    std::printf ("  held notes of 400 ms or more: %d voice notes\n", p.n ("voice sd"));
    std::printf ("  pitch spread inside a note, median SD: lead %.1f cents, voice %.1f cents\n", p.med ("lead sd"), p.med ("voice sd"));
    std::printf ("    fast part (under 150 ms): lead %.1f, voice %.1f.  slow part: lead %.1f, voice %.1f\n", p.med ("lead fast"), p.med ("voice fast"), p.med ("lead slow"), p.med ("voice slow"));
    std::printf ("  voice movement follows the lead's, median correlation: %.2f.  voice 1 and voice 2 move together: %.2f\n", p.med ("voice follows lead"), p.med ("voices move together"));
    std::printf ("  interval between the two voices, spread inside a note, median SD: %.1f cents (90th percentile %.1f)\n", p.med ("interval sd"), p.pct ("interval sd", 90.0));
    std::printf ("  note average against its note: voice median %.1f cents, 95th percentile %.1f (signed mean %.1f; voice 1 %.1f, voice 2 %.1f); lead against its own note median %.1f, 95th percentile %.1f\n",
                 p.med ("voice off"), p.pct ("voice off", 95.0), p.mean ("voice off signed"), p.mean ("voice 1 off signed"), p.mean ("voice 2 off signed"), p.med ("lead off"), p.pct ("lead off", 95.0));
    std::printf ("  steady wobble 3 to 8 Hz (notes of 600 ms or more, %d): lead depth %.1f cents at %.2f Hz, share of its movement %.2f; voice depth %.1f cents at %.2f Hz, share %.2f\n",
                 p.n ("voice wobble depth"), p.med ("lead wobble depth"), p.med ("lead wobble rate"), p.med ("lead wobble share"), p.med ("voice wobble depth"), p.med ("voice wobble rate"), p.med ("voice wobble share"));
    std::printf ("  level against the lead inside a note: spread %.2f dB median, level curves alike %.2f\n", p.med ("voice level sd"), p.med ("voice level corr"));
    std::printf ("  note changes (%d voice changes): 10 to 90%% time, lead %.0f ms median (all %d lead changes %.0f), voice %.0f ms; voice passes half way %.0f ms after the lead\n",
                 p.n ("voice glide"), p.med ("lead glide where voice measured"), p.n ("lead glide"), p.med ("lead glide"), p.med ("voice glide"), p.med ("voice glide delay"));
    std::printf ("  phrase starts (%d): voice 1 %.1f ms after the lead, voice 2 %.1f ms (median); the two voices apart %.1f ms median, %.1f at the 90th percentile; both voices, 90th percentile after the lead %.1f ms\n",
                 p.n ("start voices apart"), p.med ("start voice 1 after lead"), p.med ("start voice 2 after lead"), p.med ("start voices apart"), p.pct ("start voices apart", 90.0), p.pct ("start voice after lead", 90.0));
    std::printf ("  ends (%d): voice 1 %.1f ms after the lead's end, voice 2 %.1f ms; the two voices apart %.1f ms median\n", p.n ("end voices apart"), p.med ("end voice 1 after lead"), p.med ("end voice 2 after lead"), p.med ("end voices apart"));
    std::printf ("  spectrum shape against the lead (1.00 is the same throat): voice 1 %.3f, voice 2 %.3f, voice 2 against voice 1 %.3f\n", p.med ("shape 1"), p.med ("shape 2"), p.med ("shape 12"));
    std::printf ("  S sounds (%d): two voices alike %.2f, a voice like the lead's %.2f, level against the lead's %.1f dB\n", p.n ("s voices alike"), p.med ("s voices alike"), p.med ("s voice like lead"), p.med ("s level"));
    std::printf ("  a real double (Luke singing the same part twice, %d notes in %d takes):\n", p.n ("double apart"), p.n ("double shape"));
    std::printf ("    note averages apart %.1f cents median (95th percentile %.1f); movement in common %.2f; interval spread inside a note %.1f cents; level spread %.2f dB, level curves alike %.2f\n",
                 p.med ("double apart"), p.pct ("double apart", 95.0), p.med ("double moves together"), p.med ("double interval sd"), p.med ("double level sd"), p.med ("double level corr"));
    std::printf ("    starts (%d) apart %.1f ms (median distance from their median); ends (%d) apart %.1f ms; spectrum shape %.3f\n", p.n ("double start"), spreadAboutMedian (p, "double start"),
                 p.n ("double end"), spreadAboutMedian (p, "double end"), p.med ("double shape"));
}

// =====================================================================================================================================
// Limits for the Human option (1.5.0), committed before any of it existed. Luke likes the robot sound and wants a human one as well, and
// it must not sound out of tune or sloppy. So: Human at 0 is the old sound bit for bit (RealVox hash), and at any setting above 0, on the
// working set, measured by RealVox robot and RealVox notes with REALVOX_SET="human=<setting>" against the same build at human=0:
//   U1 in tune: a voice's average over a held note against its harmony note, median 10 cents at most (the robot reads 4.5; humanOffMaxCents)
//   U2 and its 95th percentile no more than the robot's plus 10 cents (the robot's 69.7 is where Luke himself sings outside the key)
//   U3 steady: pitch spread inside a note, median, no more than the lead's own (20.1 cents on this set): a voice never moves more than Luke does
//   U4 the chord: the interval between the two voices inside a note, median SD 15 cents at most and 90th percentile 35 at most
//      (the robot: 6.1 and 19.6; Luke against Luke singing the same part again: 25.7)
//   U5 starts: each voice's median start no later than 60 ms after the lead (humanLateMaxMs; the robot: 46 and 52), and the 90th
//      percentile of both no later than 80 ms (onsetSoundWorstMs; the robot: 73)
//   U6 note changes: 10 to 90% time, median, 120 ms at most (the robot 70, Luke 76), passing half way no later than 40 ms after the lead (20)
//   U7 ends: the two voices no further apart than 40 ms, median (Luke's two passes: 40; the robot: 4)
//   U8 level: a voice's level against the lead inside a note, spread no more than 1.21 dB median (Luke's two passes; the robot 0.71)
//   U9 held notes: rules H3 to H9 of the held note check against the same build at human=0 (RealVox notescompare)
// What more human has to mean at Human 100, also set before tuning (each is the robot's number moved a clear part of the way to Luke's):
//   V1 pitch spread inside a note, median, 10 cents or more (robot 5.3, Luke 20.1)
//   V2 the two voices' movement in common, median correlation 0.30 or less (robot 0.42, Luke's two passes 0.12)
//   V3 level curves alike, median correlation 0.65 or less (robot 0.81, Luke's two passes 0.13)
//   V4 ends, the two voices apart, median 8 ms or more (robot 4, Luke's two passes 40)
// =====================================================================================================================================
int humanLimits (const std::string& newPath, const std::string& robotPath, bool full)
{
    const Pool n = readRobot (newPath), r = readRobot (robotPath);
    printRobot (n, ("this setting: " + newPath).c_str());
    printRobot (r, ("human 0: " + robotPath).c_str());
    int fails = 0;
    const auto rule = [&] (bool ok, const char* what, double value, double limit)
    {
        if (! ok) ++fails;
        std::printf ("%s  %s: %.2f, limit %.2f\n", ok ? "PASS" : "FAIL", what, value, limit);
    };
    rule (n.med ("voice off") <= 10.0, "U1 note average against its note, median (cents)", n.med ("voice off"), 10.0);
    rule (n.pct ("voice off", 95.0) <= r.pct ("voice off", 95.0) + 10.0, "U2 note average against its note, 95th percentile (cents)", n.pct ("voice off", 95.0), r.pct ("voice off", 95.0) + 10.0);
    rule (n.med ("voice sd") <= n.med ("lead sd"), "U3 pitch spread inside a note, median, no more than the lead's (cents)", n.med ("voice sd"), n.med ("lead sd"));
    rule (n.med ("interval sd") <= 15.0, "U4 interval between the voices, spread inside a note, median (cents)", n.med ("interval sd"), 15.0);
    rule (n.pct ("interval sd", 90.0) <= 35.0, "U4 and its 90th percentile (cents)", n.pct ("interval sd", 90.0), 35.0);
    rule (n.med ("start voice 1 after lead") <= 60.0, "U5 voice 1 starts after the lead, median (ms)", n.med ("start voice 1 after lead"), 60.0);
    rule (n.med ("start voice 2 after lead") <= 60.0, "U5 voice 2 starts after the lead, median (ms)", n.med ("start voice 2 after lead"), 60.0);
    rule (n.pct ("start voice after lead", 90.0) <= 80.0, "U5 both voices, 90th percentile (ms)", n.pct ("start voice after lead", 90.0), 80.0);
    rule (n.med ("voice glide") <= 120.0, "U6 note change 10 to 90% time, median (ms)", n.med ("voice glide"), 120.0);
    rule (n.med ("voice glide delay") <= 40.0, "U6 voice passes half way after the lead, median (ms)", n.med ("voice glide delay"), 40.0);
    rule (n.med ("end voices apart") <= 40.0, "U7 ends, the two voices apart, median (ms)", n.med ("end voices apart"), 40.0);
    rule (n.med ("voice level sd") <= 1.21, "U8 level against the lead inside a note, spread, median (dB)", n.med ("voice level sd"), 1.21);
    if (full)
    {
        rule (n.med ("voice sd") >= 10.0, "V1 pitch spread inside a note at Human 100, median, at least (cents)", n.med ("voice sd"), 10.0);
        rule (n.med ("voices move together") <= 0.30, "V2 the two voices' movement in common at Human 100, at most", n.med ("voices move together"), 0.30);
        rule (n.med ("voice level corr") <= 0.65, "V3 level curves alike at Human 100, at most", n.med ("voice level corr"), 0.65);
        rule (n.med ("end voices apart") >= 8.0, "V4 ends apart at Human 100, median, at least (ms)", n.med ("end voices apart"), 8.0);
    }
    std::printf ("%s  %d of the Human limits failed (U9 is RealVox notescompare)\n", fails ? "RESULT FAIL" : "RESULT PASS", fails);
    return fails ? 1 : 0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const std::string mode = argc > 1 ? argv[1] : "";
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    if (mode == "measure" && argc >= 4)
    {
        std::ofstream out (argv[2]);
        for (int i = 3; i < argc; ++i) measureTake (cwd.getChildFile (argv[i]), out);
        printSummary (readRecords (argv[2]), argv[2]);
        return 0;
    }
    if (mode == "report" && argc >= 3) { printSummary (readRecords (argv[2]), argv[2]); return 0; }
    if (mode == "compare" && argc >= 4) return compare (argv[2], argv[3]);
    if ((mode == "notes" || mode == "robot") && argc >= 4)
    {
        std::ofstream out (argv[2]);
        for (int i = 3; i < argc; ++i) { if (mode == "notes") notesTake (cwd.getChildFile (argv[i]), out); else robotTake (cwd.getChildFile (argv[i]), out); }
        if (mode == "notes") printNotes (readNotes (argv[2]), argv[2]); else printRobot (readRobot (argv[2]), argv[2]);
        return 0;
    }
    if (mode == "notesreport" && argc >= 3) { printNotes (readNotes (argv[2]), argv[2]); return 0; }
    if (mode == "notescompare" && argc >= 4) return compareNotes (argv[2], argv[3]);
    if (mode == "humanlimits" && argc >= 4) return humanLimits (argv[2], argv[3], argc >= 5 && std::string (argv[4]) == "full");
    if (mode == "robotreport" && argc >= 3) { printRobot (readRobot (argv[2]), argv[2]); return 0; }
    if (mode == "tracker" && argc >= 5)
    {
        // What the plug-in's tracker says each ms of a stretch of a take, beside the reference: RealVox tracker <take.wav> <from s> <to s>
        Dry d;
        if (! d.load (cwd.getChildFile (argv[2]))) return 1;
        const RenderInfo info = renderInfo();
        const TrackerTrace tr = traceTracker (d.x, info.inputType, info.tracking);
        for (long ms = (long) (1000.0 * std::atof (argv[3])); ms <= (long) (1000.0 * std::atof (argv[4])) && ms < (long) tr.hz.size(); ++ms)
        {
            std::printf ("%8.3f s  tracker %7.1f Hz  %-9s  dip %.3f at %6.1f Hz  reference (12 ms earlier) %7.1f Hz  level %6.1f dBFS", 0.001 * ms, tr.hz[(size_t) ms],
                         tr.hz[(size_t) ms] > 0.0f ? "voiced" : stateNames[std::clamp ((int) tr.why[(size_t) ms], 1, 7)], tr.dip[(size_t) ms], tr.coarseHz[(size_t) ms],
                         d.leadHzAt ((ms - 12) * 48), toDb (rmsAround (d.x, (ms - 12) * 48, 240)));
            if ((size_t) ms < tr.halfFine.size()) std::printf ("  half period: dip %.3f even %.3f", tr.halfFine[(size_t) ms], tr.halfEven[(size_t) ms]);
            if ((size_t) ms < tr.rule.size()) std::printf ("  start rule %d", (int) tr.rule[(size_t) ms]);
            std::printf ("\n");
        }
        return 0;
    }
    if (mode == "calls" && argc >= 3)
    {
        // When the plug-in's tracker calls each start, with no render: RealVox calls <take.wav> ...
        // One line per phrase or word start (the same starts as measure): the take, the onset, the gap before it, the ms from the onset to
        // the first moment the tracker is voiced within a semitone of the reference and stays so for 8 ms, the ms to its first voiced
        // moment at any pitch from 30 ms before the onset, what that first pitch was against the reference (right, highoctave, lowoctave,
        // otherpitch), and how many ms the tracker was voiced in the unpitched part before the start that the leak measures read (from
        // 80 ms after the last voiced stretch to 30 ms before the onset).
        const RenderInfo info = renderInfo();
        for (int a = 2; a < argc; ++a)
        {
            Dry d;
            if (! d.load (cwd.getChildFile (argv[a]))) return 1;
            // With REALVOX_FEED the tracker hears the file of the same name from that folder (left and right mixed), less
            // REALVOX_FEED_DELAY samples, and is judged against the dry take's notes, as in notes.
            std::vector<float> heard = d.x;
            if (const char* dir = std::getenv ("REALVOX_FEED"))
            {
                juce::AudioFormatManager m;
                m.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (m.createReaderFor (juce::File (dir).getChildFile (juce::File (argv[a]).getFileName())));
                if (reader == nullptr) { std::fprintf (stderr, "no %s in %s\n", argv[a], dir); return 1; }
                juce::AudioBuffer<float> fb ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&fb, 0, fb.getNumSamples(), 0, true, true);
                const int delay = std::getenv ("REALVOX_FEED_DELAY") != nullptr ? std::atoi (std::getenv ("REALVOX_FEED_DELAY")) : 0;
                for (size_t i = 0; i < heard.size(); ++i)
                {
                    const int at = (int) i + delay;
                    heard[i] = at < fb.getNumSamples() ? 0.5f * (fb.getSample (0, at) + fb.getSample (fb.getNumChannels() > 1 ? 1 : 0, at)) : 0.0f;
                }
            }
            const TrackerTrace tr = traceTracker (heard, info.inputType, info.tracking);
            const long ticks = (long) tr.hz.size();
            // Every pitched frame of the lead (5 ms), by its pitch: what the tracker says of it. T <take> <band> <frames> <right> <octave or
            // twelfth high> <low> <another pitch> <no pitch>
            {
                long counts[3][5] = {};
                for (int f = 0; f < d.frames(); ++f)
                {
                    const double hz = d.track[(size_t) f];
                    if (hz <= 0.0 || d.hissy[(size_t) f]) continue;
                    const int st = trackerStateAt (tr, d, d.frameSample (f));
                    ++counts[hz < 98.0 ? 0 : hz < 130.0 ? 1 : 2][st == 0 ? 0 : st == 8 ? 1 : st == 9 ? 2 : st == 10 ? 3 : 4];
                    // REALVOX_CALLS_FRAMES=<Hz>: every frame under that pitch, for a frame by frame diff of two versions
                    if (const char* under = std::getenv ("REALVOX_CALLS_FRAMES"))
                        if (hz < std::atof (under)) std::printf ("F %s %.3f %.1f %s\n", d.take.c_str(), d.frameTime (f), hz, stateNames[st]);
                }
                static const char* const bands[3] = { "under98", "98to130", "over130" };
                for (int bnd = 0; bnd < 3; ++bnd)
                    std::printf ("T %s %s %ld %ld %ld %ld %ld %ld\n", d.take.c_str(), bands[bnd], counts[bnd][0] + counts[bnd][1] + counts[bnd][2] + counts[bnd][3] + counts[bnd][4],
                                 counts[bnd][0], counts[bnd][1], counts[bnd][2], counts[bnd][3], counts[bnd][4]);
            }
            for (size_t si = 0; si < d.stretches.size(); ++si)
            {
                const auto& s = d.stretches[si];
                if (s.b - s.a + 1 < 30) continue;
                const int gapFrames = si == 0 ? s.a : s.a - d.stretches[si - 1].b - 1;
                if (gapFrames < 10) continue;
                const long onsetMs = (long) std::lround (1000.0 * s.onset / SR);
                const auto refAt = [&] (long ms)
                {
                    // the reference where it has a pitch, else the stretch's first pitch
                    const double r = d.leadHzAt ((ms - 12) * 48);
                    return r > 0.0 ? r : d.track[(size_t) s.a];
                };
                long right = -1, any = -1;
                int firstState = 0;
                for (long ms = std::max (0L, onsetMs - 30); ms < std::min (ticks, onsetMs + 300); ++ms)
                {
                    if (tr.hz[(size_t) ms] <= 0.0f) continue;
                    const double c = measure::cents (tr.hz[(size_t) ms], refAt (ms));
                    if (any < 0)
                    {
                        any = ms;
                        firstState = std::abs (c) <= 100.0 ? 0 : (std::abs (c - 1200.0) <= 100.0 || std::abs (c - 1902.0) <= 100.0) ? 8
                                     : (std::abs (c + 1200.0) <= 100.0 || std::abs (c + 1902.0) <= 100.0) ? 9 : 10;
                    }
                    if (std::abs (c) > 100.0) continue;
                    bool holds = true;
                    for (long k = ms; k < std::min (ticks, ms + 8) && holds; ++k)
                        holds = tr.hz[(size_t) k] > 0.0f && std::abs (measure::cents (tr.hz[(size_t) k], refAt (k))) <= 100.0;
                    if (holds) { right = ms; break; }
                }
                long from = si == 0 ? 0 : d.stretches[si - 1].end + (long) (0.08 * SR);
                if (gapFrames >= 50) from = std::max (from, s.onset - (long) (1.5 * SR));
                int before = 0;
                for (long ms = from / 48 + 12; ms < (s.onset - (long) (0.03 * SR)) / 48 + 12 && ms < ticks; ++ms)
                    if (ms >= 0 && tr.hz[(size_t) ms] > 0.0f) ++before;
                std::printf ("C %s %.3f %d %ld %ld %s %d\n", d.take.c_str(), s.onset / SR, gapFrames * 5, right < 0 ? 999L : right - onsetMs,
                             any < 0 ? 999L : any - onsetMs, stateNames[firstState], before);
            }
        }
        return 0;
    }
    if (mode == "onsetshape" && argc >= 5)
    {
        // The shape of real starts of one kind, for building a synthetic start like them: RealVox onsetshape <S records> <kind> <take.wav> ...
        // For every start of that kind in the records (clean, consonant, fry, breathy): the level every 10 ms from 80 ms before the onset to
        // 150 ms after, against the note's level 100 to 250 ms in; the reference pitch against the note's (cents); and the tracker's
        // difference (0 is a pure repeat, 1 is noise). Prints the median and the quartiles of each, and the note's pitch.
        std::map<std::string, std::vector<double>> wantOnsets;
        {
            std::ifstream in (argv[2]);
            std::string line;
            while (std::getline (in, line))
            {
                std::istringstream ls (line);
                std::string tag, take, onset, pw, kind, voice;
                ls >> tag >> take >> onset >> pw >> kind >> voice;
                if (tag == "S" && kind == argv[3] && voice == "1") wantOnsets[take].push_back (std::atof (onset.c_str()));
            }
        }
        const RenderInfo info = renderInfo();
        constexpr int steps = 24;   // -80 ms to +150 ms in 10 ms steps
        std::vector<double> level[steps], cents[steps], dip[steps], noteHz, before;
        for (int a = 4; a < argc; ++a)
        {
            Dry d;
            if (! d.load (cwd.getChildFile (argv[a]))) return 1;
            const TrackerTrace tr = traceTracker (d.x, info.inputType, info.tracking);
            for (const auto& s : d.stretches)
            {
                bool wanted = false;
                for (double t : wantOnsets[d.take]) if (std::abs (t - s.onset / SR) < 0.002) wanted = true;
                if (! wanted || s.onset < (long) (0.1 * SR) || s.onset + (long) (0.3 * SR) >= (long) d.x.size()) continue;
                const double body = rmsOf (d.x, s.onset + (long) (0.1 * SR), (long) (0.15 * SR));
                std::vector<double> hz;
                for (int f = s.a + 20; f <= std::min (s.b, s.a + 50); ++f) if (d.track[(size_t) f] > 0.0) hz.push_back (d.track[(size_t) f]);
                if (hz.empty() || body <= 0.0) continue;
                const double note = measure::median (hz);
                noteHz.push_back (note);
                before.push_back (toDb (rmsOf (d.x, s.onset - (long) (0.08 * SR), (long) (0.07 * SR)) / body));
                for (int k = 0; k < steps; ++k)
                {
                    const long at = s.onset + (long) ((k - 8) * 0.01 * SR);
                    level[k].push_back (toDb (rmsOf (d.x, at, (long) (0.01 * SR)) / body));
                    const double ref = d.leadHzAt (at + 240);
                    if (ref > 0.0) cents[k].push_back (measure::cents (ref, note));
                    const long tick = (at + 240) / 48 + 12;
                    if (tick >= 0 && tick < (long) tr.dip.size()) dip[k].push_back (tr.dip[(size_t) tick]);
                }
            }
        }
        if (noteHz.empty()) { std::printf ("no starts of that kind\n"); return 1; }
        std::printf ("%d %s starts. Note: median %.0f Hz, quartiles %.0f and %.0f. Level 80 to 10 ms before the onset against the note: median %.1f dB, quartiles %.1f and %.1f\n",
                     (int) noteHz.size(), argv[3], measure::median (noteHz), measure::percentile (noteHz, 25.0), measure::percentile (noteHz, 75.0),
                     measure::median (before), measure::percentile (before, 25.0), measure::percentile (before, 75.0));
        std::printf ("   ms   level dB (25%%, median, 75%%)   pitch cents (25%%, median, 75%%, share with pitch)   tracker difference (25%%, median, 75%%)\n");
        for (int k = 0; k < steps; ++k)
        {
            std::printf ("%+5d  %6.1f %6.1f %6.1f   ", (k - 8) * 10, measure::percentile (level[k], 25.0), measure::median (level[k]), measure::percentile (level[k], 75.0));
            if (cents[k].size() >= 3) std::printf ("%7.0f %7.0f %7.0f  %4.2f   ", measure::percentile (cents[k], 25.0), measure::median (cents[k]), measure::percentile (cents[k], 75.0), (double) cents[k].size() / (double) level[k].size());
            else std::printf ("      -       -       -  %4.2f   ", (double) cents[k].size() / (double) level[k].size());
            std::printf ("%5.2f %5.2f %5.2f\n", measure::percentile (dip[k], 25.0), measure::median (dip[k]), measure::percentile (dip[k], 75.0));
        }
        return 0;
    }
    if (mode == "lowcensus" && argc >= 3)
    {
        // Where the tracker calls a pitch under 98 Hz: how much of it is on reference pitch under 98 Hz, on higher reference pitch, and on
        // no reference pitch at all, with the dips it saw there. RealVox lowcensus <take.wav> ...
        for (int i = 2; i < argc; ++i)
        {
            Dry d;
            if (! d.load (cwd.getChildFile (argv[i]))) continue;
            const RenderInfo info = renderInfo();
            const TrackerTrace tr = traceTracker (d.x, info.inputType, info.tracking);
            std::vector<double> dipsLow, dipsNone, dipsHigh;
            for (long ms = 12; ms < (long) tr.hz.size(); ++ms)
            {
                if (tr.hz[(size_t) ms] <= 0.0f || tr.hz[(size_t) ms] >= 98.0f) continue;
                const double ref = d.leadHzAt ((ms - 12) * 48);
                (ref <= 0.0 ? dipsNone : (ref < 98.0 ? dipsLow : dipsHigh)).push_back (tr.dip[(size_t) ms]);
            }
            const auto show = [] (const char* name, const std::vector<double>& v)
            {
                std::printf ("    %-28s %6.2f s  dip median %.3f  75th %.3f  90th %.3f\n", name, 0.001 * v.size(), v.empty() ? 0.0 : measure::median (v),
                             v.empty() ? 0.0 : measure::percentile (v, 75.0), v.empty() ? 0.0 : measure::percentile (v, 90.0));
            };
            std::printf ("  %s: tracker under 98 Hz while the reference says\n", d.take.c_str());
            show ("under 98 Hz", dipsLow);
            show ("98 Hz or more", dipsHigh);
            show ("no pitch", dipsNone);
        }
        return 0;
    }
    if (mode == "wanted" && argc >= 5)
    {
        // What the note logic asks of voices 1 and 2 through a stretch of a take, block by block: RealVox wanted <take.wav> <from s> <to s>
        Dry d;
        if (! d.load (cwd.getChildFile (argv[2]))) return 1;
        auto p = makeProcessor();
        applyEnvironment (*p);
        setParam (*p, "key", (float) d.key);
        setParam (*p, "scale", (float) d.scale);
        prepare (*p);
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        const long from = (long) (std::atof (argv[3]) * SR), to = std::min ((long) d.x.size(), (long) (std::atof (argv[4]) * SR));
        for (long pos = 0; pos + block <= to; pos += block)
        {
            for (int c = 0; c < 2; ++c) std::copy_n (d.x.data() + pos, block, buf.getWritePointer (c));
            p->processBlock (buf, midi);
            if (pos < from) continue;
            const auto r = p->getReadings();
            std::printf ("%8.3f s  lead %6.1f Hz note %3d   voice 1 note %3d pitch %7.2f lag %5.1f ms   voice 2 note %3d pitch %7.2f lag %5.1f ms\n", pos / SR, r.detectedHz, r.leadNote,
                         r.voiceNote[0], r.voicePitch[0], r.voiceLagMs[0], r.voiceNote[1], r.voicePitch[1], r.voiceLagMs[1]);
        }
        return 0;
    }
    if (mode == "hash" && argc >= 3) return hashTakes (argc, argv);
    if (mode == "diff" && argc >= 4) return diffRaw (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]));
    if (mode == "demo" && argc >= 6) return demo (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]), std::atof (argv[4]), std::atof (argv[5]));
    std::fprintf (stderr, "usage: RealVox measure <records> <wav>... | report <records> | compare <new> <old> | demo <wav> <out.wav> <from s> <secs>\n"
                          "       RealVox notes <records> <wav>... | notesreport <records> | notescompare <new> <old>\n"
                          "       RealVox robot <records> <wav>... | robotreport <records> | humanlimits <records> <records at human 0> [full]\n"
                          "       RealVox hash <wav>... | diff <a.f32> <b.f32> | tracker <wav> <from s> <to s> | lowcensus <wav>...\n");
    return 2;
}
