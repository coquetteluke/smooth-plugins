// Build against the branch harmonic-shifter (or main, for 1.4.0): clang++ -O2 -std=c++17 -I tests/prototype -I tests -I src tests/prototype/EngineHarness.cpp -framework Accelerate -o build/EngineHarness
// Run: build/EngineHarness h (high notes), d (dev high glides), n (dev held notes), f (formants), g (dev vowels at the third set's pitches), wav <file> [l]
// Also: a (non-harmonic energy on steady voices), rand <seed> (held notes on random plausible vowels), styled (dev vowels with breath and darker sources)
// Runs the plug-in's TuneEngine directly on the test voices, to read the same numbers without JUCE.
#include <random>
#include "PrototypeAnalysis.h"
#include "TuneEngine.h"
using namespace smoothtune;
struct Score { double move = 0, moveReal = 0, floor = 0, level = 0, flatter = 0, peakFlatter = 0, peakSharper = -100, peakFlatterCrossed = 0; };
double peakOverAround (const std::vector<double>& spec, size_t f1)
{
    double sum = 0.0; int count = 0;
    for (size_t hz = (size_t) (0.6 * (double) f1); hz <= (size_t) (1.5 * (double) f1); hz += 5) { sum += spec[hz]; ++count; }
    return spec[f1] - sum / count;
}
std::vector<float> runEngine (const std::vector<float>& in, int shift, int mode, double lowHz = 65.0, double highHz = 1300.0, double throat = 1.0, bool preserve = true, double flex = 100, int type = 3)
{
    TuneEngine e; e.prepare (SR, 1); e.configure (type, lowHz, highHz, mode);
    TuneEngine::Shared sh; sh.settings.retuneMs = 0; sh.settings.humanize = 0; sh.settings.flexCents = flex; sh.settings.transpose = shift; sh.formantPreserve = preserve; sh.throatRate = throat; sh.voicingLimit = 0.3f;
    const int lat = e.getLatency();
    std::vector<float> out (in.size(), 0.0f);
    for (size_t i = 0; i < in.size() + lat; ++i)
    {
        const float x = i < in.size() ? in[i] : 0.0f; float w[2], d[2];
        e.process (x, x, sh, w, d);
        if (i >= (size_t) lat) out[i - lat] = w[0];
    }
    return out;
}
int gMode = 1;
void scoreCase (Score& s, const voice::Vowel& v, double centre, int shift, const VStyle& st = {})
{
    const long from = (long) (0.3 * SR);
    const auto f0 = glideF0 (centre);
    const auto in = styledVoice (f0, v, st);
    const double r = std::pow (2.0, shift / 12.0);
    std::vector<double> f0c (f0.size()); for (size_t i = 0; i < f0.size(); ++i) f0c[i] = f0[i] * r;
    const auto control = styledVoice (f0c, v, st);
    const auto out = runEngine (in, shift, gMode);
    const long n = (long) in.size() - from - 4000;
    const double smoothing = std::max (centre, centre * r);
    const auto want = smoothSpectrumDb (control.data() + from, n, SR, smoothing), inS = smoothSpectrumDb (in.data() + from, n, SR, smoothing), got = smoothSpectrumDb (out.data() + from, n, SR, smoothing);
    double mv = 0;
    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
    {
        const double against = shapeScale (inS, got, band.first, band.second), real = shapeScale (want, got, band.first, band.second), noise = shapeScale (want, inS, band.first, band.second);
        s.move = std::max (s.move, std::abs (against - 1.0)); s.moveReal = std::max (s.moveReal, std::abs (real - 1.0)); s.floor = std::max (s.floor, std::abs (noise - 1.0)); mv = std::max (mv, std::abs (against - 1.0));
    }
    const auto fine = smoothSpectrumDb (control.data() + from, n, SR, 50.0), fineOut = smoothSpectrumDb (out.data() + from, n, SR, 50.0);
    const size_t f1 = (size_t) v.f[0];
    const double flatten = (fine[f1] - fine[f1 / 2]) - (fineOut[f1] - fineOut[f1 / 2]);
    const double pf = peakOverAround (fine, f1) - peakOverAround (fineOut, f1);
    s.peakFlatter = std::max (s.peakFlatter, pf); s.peakSharper = std::max (s.peakSharper, -pf);
    if (v.f[0] >= 1.02 * centre * r * std::sqrt (0.5)) s.peakFlatterCrossed = std::max (s.peakFlatterCrossed, pf);
    s.flatter = std::max (s.flatter, flatten);
    const double lvl = toDb (rms (out.data() + from, n)) - toDb (rms (in.data() + from, n));
    s.level = std::max (s.level, std::abs (lvl));
    if (getenv ("DUMP")) std::printf ("    %s %.0f %+d: move %.2f%% flat %.2f peak %.2f level %+.2f\n", v.name, centre, shift, 100 * mv, flatten, pf, lvl);
    if (getenv ("FINE")) for (size_t hz = 300; hz <= 1300; hz += 50) std::printf ("      %zu real %.1f out %.1f\n", hz, fine[hz], fineOut[hz]);
}
std::vector<float> wav (const char* path)
{
    FILE* f = fopen (path, "rb"); std::vector<char> b; char buf[65536]; size_t n;
    while ((n = fread (buf, 1, sizeof buf, f)) > 0) b.insert (b.end(), buf, buf + n); fclose (f);
    size_t at = 12; std::vector<float> x;
    while (at + 8 <= b.size()) { const unsigned sz = *(unsigned*) &b[at + 4]; if (std::string (&b[at], 4) == "data") { const short* d = (const short*) &b[at + 8]; for (unsigned i = 0; i < sz / 2; ++i) x.push_back (d[i] / 32768.0f); break; } at += 8 + sz + (sz & 1); }
    return x;
}
int main (int argc, char** argv)
{
    if (std::string (argv[1]) == "ramp")
    {
        // a vowel falling and rising fast, as speech does, corrected at instant speed to the nearest note
        const double rate = atof (argv[2]); std::vector<double> f0 ((size_t) (3.0 * SR));
        for (size_t i = 0; i < f0.size(); ++i) { const double t = i / SR; const double u = std::fmod (t * rate / 6.0, 2.0); const double tri = u < 1 ? u : 2 - u; f0[i] = 150.0 * std::pow (2.0, (6.0 * tri - 3.0) / 12.0); }
        const auto x = styledVoice (f0, voice::vowelA, {});
        const auto out = runEngine (x, 0, gMode, 98.0, 1000.0, 1.0, true, 0.0, 1);
        const int frame = 960; std::vector<double> ch;
        for (int at = 4800; at + frame < (int) x.size(); at += frame) { const double d = toDb (rms (out.data() + at, frame)) - toDb (rms (x.data() + at, frame)); ch.push_back (std::abs (d)); if (getenv ("SHOW")) std::printf ("  t %.2f: %+.2f dB\n", at / SR, d); }
        std::sort (ch.begin(), ch.end()); std::printf ("ramp %.0f st/s: 99th %.2f median %.2f max %.2f\n", rate, ch[(size_t) (0.99 * (ch.size() - 1))], ch[ch.size() / 2], ch.back());
        return 0;
    }
    if (std::string (argv[1]) == "wav")
    {
        const auto x = wav (argv[2]); const bool low = argc > 3 && argv[3][0] == 'l';
        const auto out = runEngine (x, 0, gMode, low ? 65.0 : 98.0, low ? 600.0 : 1000.0, 1.0, true, 0.0, low ? 2 : 1);
        const int frame = 960; std::vector<double> ch;
        for (int at = 0; at + frame < (int) x.size(); at += frame)
        {
            const double a = rms (x.data() + at, frame), b2 = rms (out.data() + at, frame);
            if (a > 1e-3) { const double d = toDb (b2) - toDb (a); ch.push_back (std::abs (d)); if (std::abs (d) > (getenv ("SHOWDB") ? atof (getenv ("SHOWDB")) : 6) && getenv ("SHOW")) std::printf ("  t %.2f: in %.1f dB, out %.1f dB\n", at / SR, toDb (a), toDb (b2)); }
        }
        double jx = 0, jo = 0; size_t at = 0;
        for (size_t i = 1; i < x.size(); ++i) { jx = std::max (jx, (double) std::abs (x[i] - x[i - 1])); const double d2 = std::abs (out[i] - out[i - 1]); if (d2 > jo) { jo = d2; at = i; } }
        std::printf ("jump ratio %.2f at %.4f s (out %.3f %.3f, in %.3f %.3f)\n", jo / jx, at / SR, out[at - 1], out[at], x[at - 1], x[at]);
        std::sort (ch.begin(), ch.end()); std::printf ("99th %.2f median %.2f\n", ch[(size_t) (0.99 * (ch.size() - 1))], ch[ch.size() / 2]);
        if (getenv ("OUTRAW")) { FILE* o = fopen (getenv ("OUTRAW"), "wb"); fwrite (out.data(), 4, out.size(), o); fclose (o); }
        return 0;
    }
    const std::string t = argc > 1 ? argv[1] : "h";
    const bool word = t == "rand" || t == "styled";
    if (getenv ("MODE")) gMode = atoi (getenv ("MODE"));
    if (! word && t.find ('f') != std::string::npos)
    {
        Score s, other;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU }) for (double c : { 110.0, 165.0 }) for (int sh : { 3, -3, 1, -1 }) scoreCase (s, *v, c, sh);
        for (auto* v : { &voice::vowelA, &voice::vowelE }) for (int sh : { 3, -3 }) scoreCase (s, *v, 247.0, sh);
        for (auto* v : { &voice::vowelO, &voice::vowelAe }) for (double c : { 130.0, 196.0 }) for (int sh : { 3, -3, 2, -2 }) scoreCase (other, *v, c, sh);
        std::printf ("formants: move %.2f%% other %.2f%% F1 flatter %.2f level %.2f\n", 100 * s.move, 100 * other.move, std::max (s.flatter, other.flatter), std::max (s.level, other.level));
    }
    if (t == "one") { Score s; const voice::Vowel* vs[] = { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }; scoreCase (s, *vs[atoi (argv[2])], atof (argv[3]), atoi (argv[4])); return 0; }
    if (! word && t.find ('g') != std::string::npos)
    {
        // dev vowels at the pitches and shifts the third held out set uses, to see whether anything there differs from the dev tests
        Score lo, hi;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe })
        {
            for (double c : { 123.0, 185.0 }) for (int sh : { 4, -4, 2, -2, 1, -1 }) scoreCase (lo, *v, c, sh);
            for (double c : { 233.0, 311.0 }) for (int sh : { 4, -4, 2, -2, 1, -1 }) scoreCase (hi, *v, c, sh);
        }
        std::printf ("dev at 123/185: move %.2f%% level %.2f; at 233/311: move %.2f%% level %.2f\n", 100 * lo.move, lo.level, 100 * hi.move, hi.level);
    }
    if (! word && t.find ('d') != std::string::npos)
    {
        Score h;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }) for (double c : { 440.0, 500.0, 600.0, 660.0 }) for (int sh : { 3, -3, 2, -2 }) scoreCase (h, *v, c, sh);
        std::printf ("dev high glides: crossed flatter %.2f dB (9), all %.2f, sharper %.2f (6), move %.2f%% (floor %.2f + 2), level %.2f\n", h.peakFlatterCrossed, h.peakFlatter, h.peakSharper, 100 * h.move, 100 * h.floor, h.level);
    }
    if (! word && t.find ('n') != std::string::npos)
    {
        double sum = 0; int count = 0;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }) for (double c : { 440.0, 500.0, 600.0, 660.0 }) for (int sh : { 3, -3, 2, -2 })
        {
            const double r = std::pow (2.0, sh / 12.0), moved = c * r;
            const auto in = styledVoice (voice::steady (c, 1.6, SR), *v, {});
            const auto out = runEngine (in, sh, gMode);
            const auto real = styledVoice (voice::steady (moved, 1.6, SR), *v, {});
            const long from = (long) (0.5 * SR), n = (long) (1.0 * SR);
            std::vector<double> d; double mean = 0;
            for (double hz = moved; hz <= 4000.0; hz += moved) { d.push_back (harmonicDb (out.data() + from, n, hz) - harmonicDb (real.data() + from, n, hz)); mean += d.back(); }
            mean /= d.size(); double s2 = 0; for (double x : d) s2 += (x - mean) * (x - mean);
            sum += std::sqrt (s2 / d.size()); ++count;
        }
        std::printf ("dev held notes: mean %.2f dB (3.5)\n", sum / count);
    }
    if (word)
    {
        // Held notes on more dev voices: plausible vowels drawn at random (rand, seed argv[2]) or the dev vowels with breath and darker
        // sources (styled). Neither is a held out set: nothing here comes from a published table.
        struct HC { voice::Vowel v; VStyle st; double c; };
        std::vector<HC> cases;
        static std::vector<std::string> names;
        if (t == "rand")
        {
            std::mt19937 gen ((unsigned) (argc > 2 ? atoi (argv[2]) : 1) * 7919u);
            std::uniform_real_distribution<double> u (0.0, 1.0);
            names.resize (40);
            for (int i = 0; i < 40; ++i)
            {
                voice::Vowel v;
                v.f[0] = 250 + 600 * u (gen); v.f[1] = std::max (v.f[0] + 250, 700 + 1800 * u (gen)); v.f[2] = std::max (v.f[1] + 250, 2200 + 900 * u (gen));
                v.f[3] = std::max (v.f[2] + 300, 3300 + 800 * u (gen)); v.f[4] = std::max (v.f[3] + 400, 4300 + 800 * u (gen));
                v.bw[0] = 45 + 70 * u (gen); v.bw[1] = 60 + 70 * u (gen); v.bw[2] = 90 + 80 * u (gen); v.bw[3] = 120 + 100 * u (gen); v.bw[4] = 150 + 120 * u (gen);
                names[(size_t) i] = "r" + std::to_string (i); v.name = names[(size_t) i].c_str();
                VStyle st { -60.0 + 22.0 * u (gen), u (gen) < 0.5 ? 0.0 : 2000.0 + 3000.0 * u (gen) };
                cases.push_back ({ v, st, std::round (392.0 * std::pow (2.0, u (gen))) });
            }
        }
        else
            for (VStyle st : { VStyle { -40.0, 0.0 }, VStyle { -60.0, 2500.0 }, VStyle { -45.0, 4000.0 } })
                for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe })
                    for (double c : { 440.0, 587.0, 740.0 }) cases.push_back ({ *v, st, c });
        double sum = 0; int count = 0;
        for (const auto& hc : cases)
            for (int sh : { 3, -3, 2, -2 })
            {
                const double r = std::pow (2.0, sh / 12.0), moved = hc.c * r;
                const auto in = styledVoice (voice::steady (hc.c, 1.6, SR), hc.v, hc.st);
                const auto out = runEngine (in, sh, gMode);
                const auto real = styledVoice (voice::steady (moved, 1.6, SR), hc.v, hc.st);
                const long from = (long) (0.5 * SR), n = (long) (1.0 * SR);
                std::vector<double> d; double mean = 0;
                for (double hz = moved; hz <= 4000.0; hz += moved) { d.push_back (harmonicDb (out.data() + from, n, hz) - harmonicDb (real.data() + from, n, hz)); mean += d.back(); }
                mean /= d.size(); double s2 = 0; for (double x : d) s2 += (x - mean) * (x - mean);
                sum += std::sqrt (s2 / d.size()); ++count;
                if (getenv ("DUMP")) std::printf ("  %s %.0f %+d: %.2f\n", hc.v.name, hc.c, sh, std::sqrt (s2 / d.size()));
            }
        std::printf ("%s held notes: mean %.2f dB over %d (3.5)\n", t.c_str(), sum / count, count);
    }
    if (! word && t.find ('a') != std::string::npos)
    {
        // Non-harmonic energy on steady voices corrected by 30 cents, as testArtifacts reads it (limit -35 dB).
        auto nonHarmonic = [] (const float* x, int n, double hz)
        {
            int order = 1; while ((1 << order) < n) ++order; --order; const int size = 1 << order;
            FFTSetup setup = vDSP_create_fftsetup (order, kFFTRadix2);
            std::vector<float> buf ((size_t) size), re ((size_t) size / 2), im ((size_t) size / 2);
            for (int i = 0; i < size; ++i) buf[(size_t) i] = x[i] * (float) (0.5 - 0.5 * std::cos (2.0 * M_PI * i / (size - 1)));
            DSPSplitComplex sp { re.data(), im.data() };
            vDSP_ctoz ((const DSPComplex*) buf.data(), 2, &sp, 1, size / 2);
            vDSP_fft_zrip (setup, &sp, 1, order, kFFTDirection_Forward);
            vDSP_destroy_fftsetup (setup);
            const double binHz = SR / size; double total = 0, off = 0;
            for (int k = (int) (50.0 / binHz); k < (int) (16000.0 / binHz) && k < size / 2; ++k)
            {
                const double f = k * binHz, e = (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
                total += e; if (std::abs (f - std::round (f / hz) * hz) > std::max (4.0 * binHz, 4.0)) off += e;
            }
            return 10.0 * std::log10 (std::max (off, 1e-30) / std::max (total, 1e-30));
        };
        struct C { double hz; int type; double low, high; };
        for (const C c : { C { 220.0, 1, 98.0, 1000.0 }, C { 440.0, 3, 65.0, 1300.0 }, C { 659.26, 3, 65.0, 1300.0 } })
        {
            const auto x = styledVoice (voice::steady (c.hz * std::pow (2.0, 30.0 / 1200.0), 3.0, SR), voice::vowelA, {});
            const auto out = runEngine (x, 0, gMode, c.low, c.high, 1.0, true, 0, c.type);
            std::printf ("non-harmonic, %.0f Hz corrected by 30 cents: %.1f dB (-35)\n", c.hz, nonHarmonic (out.data() + (long) (0.5 * SR), 1 << 16, c.hz));
        }
    }
    if (! word && t.find ('h') != std::string::npos)
    {
        Score h;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO }) for (double c : { 440.0, 660.0 }) for (int sh : { 3, -3 }) scoreCase (h, *v, c, sh);
        std::printf ("high notes: move %.2f%% (floor %.2f + 2), peak flatter %.2f dB (9), sharper %.2f (6), level %.2f\n", 100 * h.move, 100 * h.floor, h.peakFlatter, h.peakSharper, h.level);
    }
    return 0;
}
