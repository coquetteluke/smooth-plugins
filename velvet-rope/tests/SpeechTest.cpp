// Real speech, not synthetic signals. macOS `say` renders several voices to files, then resonant boosts are added at
// known frequencies. The plug-in has to take the boost back out, and has to leave the untouched voice alone.
// The limits below were written before the first run on speech. Speech from `say` is still not a singer: it has less
// sustain and vibrato than a sung vocal, so these numbers are evidence, not proof of how it sounds.

#include "DspKit.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sys/stat.h>

#ifndef SPEECH_DIR
 #define SPEECH_DIR "build/speech"
#endif

static const char* const speechText =
    "She sells seashells by the seashore, and the shells she sells are surely seashells. "
    "Every evening I sing the same slow song, and the sound rises and falls across the room. "
    "Please leave the door open, because the sharp bright voices echo through the hall. "
    "We were all waiting, hoping the morning would bring something easy and warm. "
    "Touch the cold steel, feel it ring, and listen to how long the high notes stay.";

static std::vector<float> readFloatWav (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    std::vector<char> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    std::vector<float> out;

    if (bytes.size() < 44 || std::memcmp (bytes.data(), "RIFF", 4) != 0)
        return out;

    size_t pos = 12;
    int channels = 1;

    while (pos + 8 <= bytes.size())
    {
        uint32_t size = 0;
        std::memcpy (&size, bytes.data() + pos + 4, 4);

        if (std::memcmp (bytes.data() + pos, "fmt ", 4) == 0)
        {
            uint16_t c = 1;
            std::memcpy (&c, bytes.data() + pos + 10, 2);
            channels = c;
        }
        else if (std::memcmp (bytes.data() + pos, "data", 4) == 0)
        {
            const size_t frames = std::min ((size_t) size, bytes.size() - pos - 8) / (4 * (size_t) channels);
            out.resize (frames);
            for (size_t i = 0; i < frames; ++i)
                std::memcpy (&out[i], bytes.data() + pos + 8 + i * 4 * (size_t) channels, 4);
            break;
        }

        pos += 8 + size + (size & 1);
    }

    return out;
}

/** Renders the voice with `say` the first time, normalised to -18 dB RMS while speaking so every voice is compared alike. */
static std::vector<float> loadVoice (const std::string& voice)
{
    mkdir (SPEECH_DIR, 0755);
    const std::string path = std::string (SPEECH_DIR) + "/" + voice + ".wav";
    auto x = readFloatWav (path);

    if (x.empty())
    {
        const std::string cmd = "say -v " + voice + " -o '" + path + "' --file-format=WAVE --data-format=LEF32@48000 \"" + speechText + "\"";
        if (std::system (cmd.c_str()) != 0)
            return {};
        x = readFloatWav (path);
    }

    // Level over the loud half of 50 ms frames, so pauses do not count.
    std::vector<double> frames;
    for (size_t pos = 0; pos + 2400 <= x.size(); pos += 2400)
    {
        double sum = 0.0;
        for (size_t i = pos; i < pos + 2400; ++i) sum += (double) x[i] * (double) x[i];
        frames.push_back (sum / 2400.0);
    }

    if (frames.empty())
        return {};

    std::sort (frames.begin(), frames.end());
    double loud = 0.0;
    for (size_t i = frames.size() / 2; i < frames.size(); ++i) loud += frames[i];
    loud /= (double) (frames.size() - frames.size() / 2);
    const auto scale = (float) (std::pow (10.0, -18.0 / 20.0) / std::sqrt (std::max (loud, 1.0e-12)));
    for (auto& v : x) v *= scale;
    return x;
}

// Third-octave bands from 200 Hz to 11 kHz: the spectrum as an ear would roughly weigh it.
static std::vector<double> thirdOctaveCentres()
{
    std::vector<double> c;
    for (double f = 200.0; f < 11000.0; f *= std::pow (2.0, 1.0 / 3.0)) c.push_back (f);
    return c;
}

struct Numbers { double removed = 0, residual = 0, boost = 0, collateral = 0, lsdBefore = 0, lsdAfter = 0; };

static double bandAround (const Spectrum& s, double f, double q)
{
    const double half = f / (2.0 * q);
    return s.band (f - half, f + half);
}

/** Largest change in a third-octave band further than an octave from f. */
static double collateralFrom (const Spectrum& a, const Spectrum& b, double f)
{
    double worst = 0.0;
    for (double c : thirdOctaveCentres())
    {
        if (c > f / 2.0 && c < f * 2.0) continue;
        const double change = bandChange (a, b, c / 1.1225, c * 1.1225);
        if (std::abs (change) > std::abs (worst)) worst = change;
    }
    return worst;
}

/** Mean absolute third-octave difference between two spectra, 500 Hz to 10 kHz. How far the tone colour is from clean. */
static double colourDistance (const Spectrum& a, const Spectrum& b)
{
    double sum = 0.0; int n = 0;
    for (double c : thirdOctaveCentres())
    {
        if (c < 500.0 || c > 10000.0) continue;
        sum += std::abs (bandChange (a, b, c / 1.1225, c * 1.1225));
        ++n;
    }
    return sum / n;
}

static Numbers measureBoost (const std::vector<float>& clean, double f, double q, double gainDb, const Params& p)
{
    const auto boosted = peakFilter (clean, kRate, f, q, gainDb);
    const auto out = render (mono (boosted), p)[0];
    const auto c = spectrumOf (clean, kRate, 0.2), b = spectrumOf (boosted, kRate, 0.2), o = spectrumOf (out, kRate, 0.2);
    Numbers n;
    n.boost      = bandAround (b, f, q) - bandAround (c, f, q);
    n.residual   = bandAround (o, f, q) - bandAround (c, f, q);
    n.removed    = bandAround (b, f, q) - bandAround (o, f, q);
    n.collateral = collateralFrom (b, o, f);
    n.lsdBefore  = colourDistance (c, b);
    n.lsdAfter   = colourDistance (c, o);
    return n;
}

/** How much the gain flickers: mean change per frame, dB, in bins being reduced by at least 1 dB. */
static double gainFlicker (const std::vector<float>& x, const Params& p, double lowHz, double highHz)
{
    std::vector<float> previous;
    double sum = 0.0; long count = 0;
    watchFrames (mono (x), p, [&] (const ResonanceEngine& e, double t)
    {
        if (previous.empty()) previous.assign ((size_t) e.getNumBins(), 0.0f);
        const int first = (int) (lowHz / e.getBinHz()), last = (int) (highHz / e.getBinHz());
        for (int k = first; k <= last; ++k)
        {
            const float r = e.getReductionDb (0, k);
            if (t > 0.2 && std::max (r, previous[(size_t) k]) >= 1.0f)
            {
                sum += std::abs (r - previous[(size_t) k]);
                ++count;
            }
            previous[(size_t) k] = r;
        }
    });
    return count > 0 ? sum / (double) count : 0.0;
}

static double crestDb (const std::vector<float>& x)
{
    double pk = 0.0, sum = 0.0;
    for (auto v : x) { pk = std::max (pk, (double) std::abs (v)); sum += (double) v * (double) v; }
    return 20.0 * std::log10 (pk / std::sqrt (sum / (double) x.size()));
}

int main (int argc, char** argv)
{
    const bool verbose = argc > 1 && std::strcmp (argv[1], "-v") == 0;
    const std::vector<std::string> voices { "Samantha", "Daniel", "Karen", "Moira" };
    std::map<std::string, std::vector<float>> speech;

    section ("Real speech rendered by macOS say, 48 kHz, normalised to -18 dB RMS on the loud half");
    for (const auto& v : voices)
    {
        speech[v] = loadVoice (v);
        check (speech[v].size() > 5 * 48000, "%-9s rendered, %.1f s", v.c_str(), (double) speech[v].size() / kRate);
        if (speech[v].empty()) return finishAndReturn();
        const auto s = spectrumOf (speech[v], kRate, 0.2);
        info ("%-9s level by band: 200-1k %.1f, 1k-3k %.1f, 3k-6k %.1f, 6k-10k %.1f, 10k-20k %.1f dB", v.c_str(),
              s.band (200, 1000), s.band (1000, 3000), s.band (3000, 6000), s.band (6000, 10000), s.band (10000, 20000));
    }

    section ("Clean voice, no boost: it should be left mostly alone. Depth 12, other controls default");
    Params harsh; harsh.depthDb = 12.0f;
    if (argc > 1 && std::strcmp (argv[1], "-s") == 0)
    {
        double worstClean = 0, meanClean = 0, deltaWorst = -100;
        for (const auto& v : voices)
        {
            const auto out = render (mono (speech[v]), harsh)[0];
            const auto before = spectrumOf (speech[v], kRate, 0.2), after = spectrumOf (out, kRate, 0.2);
            const double w = worstThirdOctave (before, after, 1000, 8500);
            meanClean += w / 4; worstClean = std::min (worstClean, w);
            std::vector<float> diff (out.size()); for (size_t i = 0; i < out.size(); ++i) diff[i] = speech[v][i] - out[i];
            deltaWorst = std::max (deltaWorst, rmsDb (diff, 9600) - rmsDb (speech[v], 9600));
        }
        std::printf ("clean worst %.2f mean %.2f delta %.1f | removed@+12:", worstClean, meanClean, deltaWorst);
        for (double f : { 1250.0, 2000.0, 3150.0, 4500.0, 6300.0 })
        {
            double m = 0, l = 99, e = 0;
            for (const auto& v : voices) { const auto n = measureBoost (speech[v], f, 6.0, 12.0, harsh); m += n.removed / 4; l = std::min (l, n.removed); e += n.lsdAfter / 4; }
            std::printf (" %.0f:%.1f/%.1f(c%.2f)", f, m, l, e);
        }
        std::printf ("\n");
        return 0;
    }
    if (argc > 1 && std::strcmp (argv[1], "-d") == 0)
    {
        for (const auto& v : voices)
        {
            const auto before = spectrumOf (speech[v], kRate, 0.2), after = spectrumOf (render (mono (speech[v]), harsh)[0], kRate, 0.2);
            std::printf ("%-9s", v.c_str());
            for (double c : thirdOctaveCentres()) std::printf (" %5.0f:%+.1f(%.0f)", c, bandChange (before, after, c / 1.1225, c * 1.1225), before.band (c / 1.1225, c * 1.1225));
            std::printf ("\n   fine:");
            for (double f = 7000.0; f < 12500.0; f += 250.0) std::printf (" %.2f:%.0f/%+.1f", f / 1000.0, before.band (f, f + 250.0), bandChange (before, after, f, f + 250.0));
            std::printf ("\n");
        }
        return 0;
    }
    for (const auto& v : voices)
    {
        const auto& x = speech[v];
        const auto out = render (mono (x), harsh)[0];
        Params d = harsh; d.delta = true;
        const auto deltaOut = render (mono (x), d)[0];
        const auto before = spectrumOf (x, kRate, 0.2), after = spectrumOf (out, kRate, 0.2);
        const double overall = rmsDb (out, 9600) - rmsDb (x, 9600);
        const double worst = worstThirdOctave (before, after, 1000, 11000);
        const double delta = rmsDb (deltaOut, 9600) - rmsDb (x, 9600);
        const double crest = crestDb (out) - crestDb (x);
        check (std::abs (overall) <= 1.0 && std::abs (worst) <= 2.5 && delta <= -18.0 && std::abs (crest) <= 1.0,
               "%-9s overall %+.2f dB (limit 1), worst 3rd octave %+.2f dB (limit 2.5), Delta %.1f dB below the voice (limit 18), crest factor %+.2f dB (limit 1)",
               v.c_str(), overall, worst, -delta, crest);
    }

    section ("Boost added at a known frequency, Q 6. Removed should be at least half the boost, without digging past the clean voice");
    for (double gain : { 9.0, 12.0, 15.0 })
        for (double f : { 1250.0, 2000.0, 3150.0, 4500.0, 6300.0 })
        {
            double removed = 0, residual = 0, collateral = 0, before = 0, after = 0, worstRemoved = 1.0e9, worstResidual = -1.0e9, worstCollateral = 0, boost = 0;
            for (const auto& v : voices)
            {
                const auto n = measureBoost (speech[v], f, 6.0, gain, harsh);
                if (verbose)
                    info ("  %-9s +%.0f at %.0f: boost in band %+.1f, removed %.1f, left %+.1f, elsewhere %+.2f, colour error %.2f to %.2f dB", v.c_str(), gain, f, n.boost, n.removed, n.residual, n.collateral, n.lsdBefore, n.lsdAfter);
                removed += n.removed; residual += n.residual; collateral += n.collateral; before += n.lsdBefore; after += n.lsdAfter; boost += n.boost;
                worstRemoved = std::min (worstRemoved, n.removed);
                worstResidual = std::max (worstResidual, n.residual);
                if (std::abs (n.collateral) > std::abs (worstCollateral)) worstCollateral = n.collateral;
            }
            const double count = (double) voices.size();
            const bool ok = worstRemoved >= 0.5 * (boost / count) && worstCollateral > -1.5 && worstCollateral < 1.5 && after < before;
            check (ok, "+%.0f dB at %4.0f Hz, in band %+.1f: removed mean %.1f, least %.1f (goal %.1f), left %+.1f, elsewhere worst %+.2f (limit 1.5), colour error %.2f to %.2f dB",
                   gain, f, boost / count, removed / count, worstRemoved, 0.5 * boost / count, residual / count, worstCollateral, before / count, after / count);
        }

    section ("Stack: all four voices together in stereo with a shared +12 dB resonance at 3150 Hz");
    {
        const size_t n = std::min ({ speech["Samantha"].size(), speech["Daniel"].size(), speech["Karen"].size(), speech["Moira"].size() });
        std::vector<float> l (n), r (n);
        for (size_t i = 0; i < n; ++i)
        {
            l[i] = 0.5f * (0.8f * speech["Samantha"][i] + 0.3f * speech["Daniel"][i] + 0.6f * speech["Karen"][i] + 0.5f * speech["Moira"][i]);
            r[i] = 0.5f * (0.3f * speech["Samantha"][i] + 0.8f * speech["Daniel"][i] + 0.5f * speech["Karen"][i] + 0.6f * speech["Moira"][i]);
        }
        const auto bl = peakFilter (l, kRate, 3150.0, 6.0, 12.0), br = peakFilter (r, kRate, 3150.0, 6.0, 12.0);
        for (bool midSide : { false, true })
        {
            Params p = harsh; p.midSide = midSide;
            const auto out = render ({ bl, br }, p);
            const auto cl = spectrumOf (l, kRate, 0.2), bs = spectrumOf (bl, kRate, 0.2), os = spectrumOf (out[0], kRate, 0.2);
            const double removed = bandAround (bs, 3150.0, 6.0) - bandAround (os, 3150.0, 6.0);
            const double collateral = collateralFrom (bs, os, 3150.0);
            check (removed >= 3.0 && std::abs (collateral) < 1.5, "%s: boost %+.1f dB in band, removed %.1f dB (goal 3), elsewhere worst %+.2f dB (limit 1.5)",
                   midSide ? "Mid/Side" : "Stereo  ", bandAround (bs, 3150.0, 6.0) - bandAround (cl, 3150.0, 6.0), removed, collateral);
        }
    }

    section ("Gain movement on real speech, with +12 dB at 3150 Hz");
    for (const auto& v : voices)
    {
        const auto boosted = peakFilter (speech[v], kRate, 3150.0, 6.0, 12.0);
        const double flicker = gainFlicker (boosted, harsh, 1000.0, 11000.0);
        check (flicker <= 0.5, "%-9s mean gain change per 5.3 ms frame where active: %.3f dB (limit 0.5)", v.c_str(), flicker);
    }

    return finishAndReturn();
}
