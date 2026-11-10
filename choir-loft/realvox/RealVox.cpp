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

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "../tests/Analysis.h"

#include <cstdio>
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

std::unique_ptr<PluginProcessor> makeProcessor()
{
    PluginProcessor::Options o;
    o.keyFilePath = "/nonexistent/choir-loft-realvox.json";
    o.hostLatencyUpdates = false;
    return std::make_unique<PluginProcessor> (o);
}

void setParam (PluginProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
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

Render render (const std::vector<float>& x, bool singingOnly, int key, int scale, double referenceHz)
{
    auto p = makeProcessor();
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
        for (int c = 0; c < 2; ++c) std::copy_n (x.data() + pos, len, buf.getWritePointer (c));
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
double pitchAround (const std::vector<float>& y, long centre, double aroundHz)
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

struct Stretch { int a = 0, b = 0; long onset = 0, end = 0; };

void measureTake (const juce::File& file, std::ofstream& out)
{
    const auto x = loadMono (file);
    const std::string take = file.getFileNameWithoutExtension().toStdString();
    if (x.empty()) { std::fprintf (stderr, "could not load %s\n", take.c_str()); out << "E " << take << " load\n"; return; }

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
    if (mode == "demo" && argc >= 6) return demo (cwd.getChildFile (argv[2]), cwd.getChildFile (argv[3]), std::atof (argv[4]), std::atof (argv[5]));
    std::fprintf (stderr, "usage: RealVox measure <records> <wav>... | report <records> | compare <new> <old> | demo <wav> <out.wav> <from s> <secs>\n");
    return 2;
}
