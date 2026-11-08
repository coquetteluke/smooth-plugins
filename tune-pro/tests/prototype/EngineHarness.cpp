// Build against the branch harmonic-shifter (or main, for 1.4.0): clang++ -O2 -std=c++17 -I tests/prototype -I tests -I src tests/prototype/EngineHarness.cpp -framework Accelerate -o build/EngineHarness
// Run: build/EngineHarness h (high notes), d (dev high glides), n (dev held notes), f (formants), g (dev vowels at the third set's pitches), b (big shifts),
//      3, 4, 5 (the third, fourth and fifth held out sets, already run, so regression tests), v (vowel change), one3 / one5 <voice> <centre> <shift>, wav <file> [l].
//      Several letters run several groups. DUMP=1 prints each case, BANDS=1 each band, SHOWLVL=1 the level every 0.2 s, FROM=<s> starts the reading later.
// Runs the plug-in's TuneEngine directly on the test voices, to read the same numbers without JUCE.
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
int gMode = 1, gType = 3;
void scoreCase (Score& s, const voice::Vowel& v, double centre, int shift, const VStyle& st = {})
{
    const long from = (long) ((getenv ("FROM") ? atof (getenv ("FROM")) : 0.3) * SR);
    const auto f0 = glideF0 (centre);
    const auto in = styledVoice (f0, v, st);
    const double r = std::pow (2.0, shift / 12.0);
    std::vector<double> f0c (f0.size()); for (size_t i = 0; i < f0.size(); ++i) f0c[i] = f0[i] * r;
    const auto control = styledVoice (f0c, v, st);
    const auto out = runEngine (in, shift, gMode, 65.0, gType == 2 ? 600.0 : 1300.0, 1.0, true, 100, gType);
    const long n = (long) in.size() - from - 4000;
    const double smoothing = std::max (centre, centre * r);
    const auto want = smoothSpectrumDb (control.data() + from, n, SR, smoothing), inS = smoothSpectrumDb (in.data() + from, n, SR, smoothing), got = smoothSpectrumDb (out.data() + from, n, SR, smoothing);
    double mv = 0;
    for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
    {
        const double against = shapeScale (inS, got, band.first, band.second), real = shapeScale (want, got, band.first, band.second), noise = shapeScale (want, inS, band.first, band.second);
        if (getenv ("BANDS")) std::printf ("      band %.0f-%.0f: against input %.4f, against real %.4f, floor %.4f\n", band.first, band.second, against, real, noise);
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
    if (getenv ("SHOWLVL")) for (long at = 0; at + 9600 <= (long) in.size(); at += 9600) std::printf ("      t %.1f f0 %.0f out-in %+.2f real-in %+.2f\n", at / SR, f0[(size_t) at], toDb (rms (out.data() + at, 9600)) - toDb (rms (in.data() + at, 9600)), toDb (rms (control.data() + at, 9600)) - toDb (rms (in.data() + at, 9600)));
    if (getenv ("DUMP")) std::printf ("    %s %.0f %+d: move %.2f%% flat %.2f peak %.2f level %+.2f real %+.2f\n", v.name, centre, shift, 100 * mv, flatten, pf, lvl, toDb (rms (control.data() + from, n)) - toDb (rms (in.data() + from, n)));
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
    if (getenv ("MODE")) gMode = atoi (getenv ("MODE"));
    if (t.find ('f') != std::string::npos)
    {
        Score s, other;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU }) for (double c : { 110.0, 165.0 }) for (int sh : { 3, -3, 1, -1 }) scoreCase (s, *v, c, sh);
        for (auto* v : { &voice::vowelA, &voice::vowelE }) for (int sh : { 3, -3 }) scoreCase (s, *v, 247.0, sh);
        for (auto* v : { &voice::vowelO, &voice::vowelAe }) for (double c : { 130.0, 196.0 }) for (int sh : { 3, -3, 2, -2 }) scoreCase (other, *v, c, sh);
        std::printf ("formants: move %.2f%% other %.2f%% F1 flatter %.2f level %.2f\n", 100 * s.move, 100 * other.move, std::max (s.flatter, other.flatter), std::max (s.level, other.level));
    }
    if (t == "one3")
    {
        struct H { voice::Vowel v; VStyle st; int type; };
        const H hs[] {
            { { { 427, 2034, 2684, 3500, 4300 }, { 50, 95, 125, 160, 210 }, "ih men" }, { -55.0, 3500.0 }, 2 },
            { { { 768, 1333, 2522, 3500, 4300 }, { 80, 95, 135, 160, 210 }, "ah men" }, { -42.0, 0.0 }, 2 },
            { { { 469, 1122, 2434, 3500, 4300 }, { 55, 80, 120, 160, 210 }, "uu men" }, { -60.0, 6000.0 }, 2 },
            { { { 474, 1379, 1710, 3500, 4300 }, { 60, 85, 100, 160, 210 }, "er men" }, { -50.0, 0.0 }, 2 },
            { { { 437, 2761, 3372, 4100, 4900 }, { 50, 115, 145, 190, 230 }, "i women" }, { -48.0, 0.0 }, 3 },
            { { { 753, 1426, 2933, 4100, 4900 }, { 85, 105, 150, 190, 230 }, "uh women" }, { -58.0, 4500.0 }, 3 },
            { { { 459, 1105, 2735, 4100, 4900 }, { 55, 90, 140, 190, 230 }, "u women" }, { -40.0, 2500.0 }, 3 } };
        const H& h = hs[atoi (argv[2])];
        gType = h.type; Score s; scoreCase (s, h.v, atof (argv[3]), atoi (argv[4]), h.st);
        std::printf ("%s %s %s: move %.2f%% level %+.2f\n", h.v.name, argv[3], argv[4], 100 * s.move, s.level);
        return 0;
    }
    if (t == "one5")
    {
        struct H { voice::Vowel v; VStyle st; };
        const H hs[] {
            { { { 820, 1530, 2450, 3400, 4300 }, { 100, 105, 140, 170, 220 }, "open front rounded" }, { -50.0, 0.0 } },
            { { { 750, 940, 2500, 3400, 4300 }, { 95, 90, 140, 170, 220 }, "open back" }, { -38.0, 3500.0 } },
            { { { 700, 760, 2500, 3400, 4300 }, { 90, 85, 140, 170, 220 }, "open back rounded" }, { -56.0, 0.0 } },
            { { { 600, 1170, 2500, 3400, 4300 }, { 85, 95, 135, 170, 220 }, "turned v" }, { -44.0, 2500.0 } },
            { { { 585, 1710, 2400, 3400, 4300 }, { 85, 100, 135, 170, 220 }, "oe" }, { -60.0, 4500.0 } } };
        const H& h = hs[atoi (argv[2])];
        Score s; scoreCase (s, h.v, atof (argv[3]), atoi (argv[4]), h.st);
        std::printf ("%s %s %s: crossed flatter %.2f sharper %.2f move %.2f%% level %+.2f\n", h.v.name, argv[3], argv[4], s.peakFlatterCrossed, s.peakSharper, 100 * s.move, s.level);
        return 0;
    }
    if (t == "one") { Score s; const voice::Vowel* vs[] = { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }; scoreCase (s, *vs[atoi (argv[2])], atof (argv[3]), atoi (argv[4])); return 0; }
    if (t.find ('g') != std::string::npos)
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
    if (t.find ('3') != std::string::npos)
    {
        // The third held out set (Hillenbrand), copied from testHeldOutThird. It has been run, so it is a regression test now, not a held out one.
        struct H { voice::Vowel v; VStyle st; int type; std::vector<double> centres; };
        const std::vector<H> hs {
            { { { 427, 2034, 2684, 3500, 4300 }, { 50, 95, 125, 160, 210 }, "ih men" }, { -55.0, 3500.0 }, 2, { 123.0, 185.0 } },
            { { { 768, 1333, 2522, 3500, 4300 }, { 80, 95, 135, 160, 210 }, "ah men" }, { -42.0, 0.0 }, 2, { 123.0, 185.0 } },
            { { { 469, 1122, 2434, 3500, 4300 }, { 55, 80, 120, 160, 210 }, "uu men" }, { -60.0, 6000.0 }, 2, { 123.0, 185.0 } },
            { { { 474, 1379, 1710, 3500, 4300 }, { 60, 85, 100, 160, 210 }, "er men" }, { -50.0, 0.0 }, 2, { 123.0, 185.0 } },
            { { { 437, 2761, 3372, 4100, 4900 }, { 50, 115, 145, 190, 230 }, "i women" }, { -48.0, 0.0 }, 3, { 233.0, 311.0 } },
            { { { 753, 1426, 2933, 4100, 4900 }, { 85, 105, 150, 190, 230 }, "uh women" }, { -58.0, 4500.0 }, 3, { 233.0, 311.0 } },
            { { { 459, 1105, 2735, 4100, 4900 }, { 55, 90, 140, 190, 230 }, "u women" }, { -40.0, 2500.0 }, 3, { 233.0, 311.0 } } };
        Score s;
        for (const auto& h : hs) for (double c : h.centres) for (int sh : { 4, -4, 2, -2, 1, -1 }) { gType = h.type; scoreCase (s, h.v, c, sh, h.st); }
        gType = 3;
        std::printf ("set 3: move %.2f%% (4) level %.2f dB (1.5)\n", 100 * s.move, s.level);
    }
    if (t.find ('5') != std::string::npos)
    {
        // The fifth held out set's singing voices (Catford), copied from testHeldOutFifth. Run already, so a regression test now.
        struct H { voice::Vowel v; VStyle st; };
        const std::vector<H> hs {
            { { { 820, 1530, 2450, 3400, 4300 }, { 100, 105, 140, 170, 220 }, "open front rounded" }, { -50.0, 0.0 } },
            { { { 750, 940, 2500, 3400, 4300 }, { 95, 90, 140, 170, 220 }, "open back" }, { -38.0, 3500.0 } },
            { { { 700, 760, 2500, 3400, 4300 }, { 90, 85, 140, 170, 220 }, "open back rounded" }, { -56.0, 0.0 } },
            { { { 600, 1170, 2500, 3400, 4300 }, { 85, 95, 135, 170, 220 }, "turned v" }, { -44.0, 2500.0 } },
            { { { 585, 1710, 2400, 3400, 4300 }, { 85, 100, 135, 170, 220 }, "oe" }, { -60.0, 4500.0 } } };
        Score s;
        for (const auto& h : hs) for (double c : { 415.0, 494.0, 554.0, 784.0 }) for (int sh : { 3, -3, 2, -2 }) scoreCase (s, h.v, c, sh, h.st);
        std::printf ("set 5 singing: crossed flatter %.2f dB (9), sharper %.2f (6), move %.2f%% (floor %.2f + 2), level %.2f\n", s.peakFlatterCrossed, s.peakSharper, 100 * s.move, 100 * s.floor, s.level);
    }
    if (t.find ('v') != std::string::npos)
    {
        // testVowelChange: a vowel change in the middle of a glide, read 150 ms to 1.15 s after it against a real voice with the new vowel.
        double worst = 0.0;
        const std::pair<const voice::Vowel*, const voice::Vowel*> pairs[] = { { &voice::vowelA, &voice::vowelI }, { &voice::vowelI, &voice::vowelA }, { &voice::vowelU, &voice::vowelE }, { &voice::vowelO, &voice::vowelAe } };
        for (const auto& pr : pairs) for (double centre : { 147.0, 220.0 }) for (int shift : { 3, -3 })
        {
            const double moved = std::pow (2.0, shift / 12.0);
            auto f0 = glideF0 (centre); std::vector<double> f0m (f0.size()); for (size_t i = 0; i < f0.size(); ++i) f0m[i] = f0[i] * moved;
            const auto before = voice::synth (f0, *pr.first, SR), after = voice::synth (f0, *pr.second, SR);
            std::vector<float> in (before.size());
            const size_t at = (size_t) (3.0 * SR), fade = (size_t) (0.03 * SR);
            for (size_t i = 0; i < in.size(); ++i) { const double u = i < at ? 0.0 : i >= at + fade ? 1.0 : (double) (i - at) / (double) fade; in[i] = (float) ((1.0 - u) * before[i] + u * after[i]); }
            const auto out = runEngine (in, shift, gMode, 65.0, 600.0, 1.0, true, 100, 2);
            const auto real = voice::synth (f0m, *pr.second, SR);
            const long from = (long) (3.15 * SR), n = (long) (1.0 * SR);
            const double smoothing = std::max (centre, centre * moved);
            const auto want = smoothSpectrumDb (real.data() + from, n, SR, smoothing), got = smoothSpectrumDb (out.data() + from, n, SR, smoothing);
            const auto& v = *pr.second;
            double m = 0.0;
            for (auto band : { std::pair<double, double> { 0.7 * v.f[0], 1.3 * v.f[1] }, std::pair<double, double> { 0.8 * v.f[1], 1.2 * v.f[3] } })
                m = std::max (m, std::abs (shapeScale (want, got, band.first, band.second) - 1.0));
            worst = std::max (worst, m);
            if (getenv ("DUMP")) std::printf ("    %s to %s %.0f %+d: %.2f%%\n", pr.first->name, v.name, centre, shift, 100 * m);
        }
        std::printf ("vowel change: worst move %.2f%% (4)\n", 100 * worst);
    }
    if (t.find ('b') != std::string::npos)
    {
        // testFormants' big shifts: +/-7 semitones at 165 and 247 Hz, read against a real voice (limit 8%), and the peak (limit 9 dB).
        Score s;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO }) for (double c : { 165.0, 247.0 }) for (int sh : { 7, -7 }) scoreCase (s, *v, c, sh);
        std::printf ("big shifts: move against a real voice %.2f%% (8), peak flatter %.2f dB (9), level %.2f\n", 100 * s.moveReal, s.peakFlatter, s.level);
    }
    if (t.find ('4') != std::string::npos)
    {
        // The fourth held out set's speaking voices (children, Peterson and Barney), copied from testHeldOutFourth. Run already: a regression test.
        struct H { voice::Vowel v; VStyle st; };
        const std::vector<H> hs {
            { { { 530, 2730, 3600, 4400, 5300 }, { 75, 120, 170, 220, 260 }, "ih children" }, { -50.0, 3500.0 } },
            { { { 1010, 2320, 3320, 4400, 5300 }, { 120, 130, 170, 220, 260 }, "ae children" }, { -38.0, 0.0 } },
            { { { 560, 1410, 3310, 4400, 5300 }, { 80, 110, 160, 220, 260 }, "oo children" }, { -56.0, 2000.0 } },
            { { { 560, 1820, 2160, 4400, 5300 }, { 80, 115, 140, 220, 260 }, "er children" }, { -44.0, 5500.0 } } };
        Score s;
        for (const auto& h : hs) for (double c : { 277.0, 330.0 }) for (int sh : { 4, -4, 2, -2, 1, -1 }) scoreCase (s, h.v, c, sh, h.st);
        std::printf ("set 4 speaking: move %.2f%% (4) level %.2f dB (1.5)\n", 100 * s.move, s.level);
    }
    if (t.find ('d') != std::string::npos)
    {
        Score h;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelI, &voice::vowelU, &voice::vowelO, &voice::vowelAe }) for (double c : { 440.0, 500.0, 600.0, 660.0 }) for (int sh : { 3, -3, 2, -2 }) scoreCase (h, *v, c, sh);
        std::printf ("dev high glides: crossed flatter %.2f dB (9), all %.2f, sharper %.2f (6), move %.2f%% (floor %.2f + 2), level %.2f\n", h.peakFlatterCrossed, h.peakFlatter, h.peakSharper, 100 * h.move, 100 * h.floor, h.level);
    }
    if (t.find ('n') != std::string::npos)
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
    if (t.find ('h') != std::string::npos)
    {
        Score h;
        for (auto* v : { &voice::vowelA, &voice::vowelE, &voice::vowelO }) for (double c : { 440.0, 660.0 }) for (int sh : { 3, -3 }) scoreCase (h, *v, c, sh);
        std::printf ("high notes: move %.2f%% (floor %.2f + 2), peak flatter %.2f dB (9), sharper %.2f (6), level %.2f\n", 100 * h.move, 100 * h.floor, h.peakFlatter, h.peakSharper, h.level);
    }
    return 0;
}
