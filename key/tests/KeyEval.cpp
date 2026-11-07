// Development tool, not part of the plug-in. Runs the DEV corpus only, so tuning never sees the held out seed.
// Build: clang++ -std=c++17 -O2 -Isrc -Itests tests/KeyEval.cpp -o build/KeyEval
#include "KeyCorpus.h"
#include "PassLimits.h"
#include <chrono>
#include <cstdlib>
#include <map>

using namespace corpus;
using smoothkey::KeyAnalyser;

int main (int argc, char** argv)
{
    const int every = argc > 1 ? std::atoi (argv[1]) : 3;        // keep it quick: every Nth tonic
    KeyAnalyser::Knobs knobs;
    bool chordsOnly = false, verbose = false;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i]; const double v = std::atof (argv[i + 1]);
        if (k == "bass") knobs.bassWeight = v;
        else if (k == "open") knobs.openingWeight = v;
        else if (k == "power") knobs.chromaPower = v;
        else if (k == "decay") knobs.harmonicDecay = v;
        else if (k == "tau") knobs.tauStable = v;
        else if (k == "soft") knobs.softness = v;
        else if (k == "first") knobs.firstStable = v;
        else if (k == "settle") knobs.settleSeconds = v;
        else if (k == "chordsonly") chordsOnly = v > 0.0;
        else if (k == "guard") knobs.relativeGuard = v;
        else if (k == "back") knobs.returnGuard = v;
        else if (k == "share") knobs.outsideShare = v;
        else if (k == "outfast") knobs.outsideFast = v;
        else if (k == "recentfast") knobs.recentFast = v;
        else if (k == "outstable") knobs.outsideStable = v;
        else if (k == "taufast") knobs.tauFast = v;
        else if (k == "dwellfast") knobs.dwellFast = v;
        else if (k == "holdfast") knobs.holdFast = v;
        else if (k == "marginfast") knobs.marginFast = v;
        else if (k == "firstfast") knobs.firstFast = v;
        else if (k == "lead") knobs.firstLeadStable = v;
        else if (k == "cad") knobs.cadenceWeight = v;
        else if (k == "cadfull") knobs.cadenceFull = v;
        else if (k == "home") knobs.homeChordWeight = v;
        else if (k == "lead") knobs.firstLeadStable = v;
        else if (k == "verbose") verbose = v > 0.0;
    }
    const double sr = 48000.0;
    const auto start = std::chrono::steady_clock::now();
    KeyAnalyser a;
    a.setKnobs (knobs);
    a.configure (sr);
    Summary all;
    std::map<std::string, Summary> byProg, byTimbre;
    const auto ts = timbres();
    const auto ps = progressions();
    for (int tonic = 0; tonic < 12; tonic += every)
        for (size_t p = 0; p < ps.size(); ++p)
            for (size_t ti = 0; ti < ts.size(); ++ti)
            {
                a.reset();
                a.setResponse (smoothkey::Response::stable);
                a.setSource (smoothkey::Source::mix);
                const auto clip = chordClip (tonic, ps[p], ts[ti], clipSeed (limits::devSeed, tonic, (int) p, (int) ti), sr, 24.0);
                const int want = keyIndex (tonic, ps[p].minor);
                const auto tr = analyse (a, clip, sr, want);
                if (verbose && (tr.changesAfterEight > 0 || tr.finalKey != want))
                {
                    std::printf ("  %s/%s want %d:", ps[p].name, ts[ti].name, want);
                    for (auto& h : tr.history) std::printf (" %.1f=%d", h.first, h.second);
                    std::printf ("\n");
                }
                all.add (tr, want); byProg[ps[p].name].add (tr, want); byTimbre[ts[ti].name].add (tr, want);
            }
    std::printf ("CHORDS exact %.1f%% related %.1f%% settle median %.1f p90 %.1f bounces %d moved %.1f%% conf right %.2f wrong %.2f (n=%d)\n",
                 all.exactPercent(), all.relatedPercent(), all.percentile (0.5), all.percentile (0.9), all.bounces, all.movedPercent(), all.meanConfCorrect(), all.meanConfWrong(), all.total);
    for (auto& [n, s] : byProg) std::printf ("  %-14s exact %5.1f related %5.1f\n", n.c_str(), s.exactPercent(), s.relatedPercent());
    for (auto& [n, s] : byTimbre) std::printf ("  %-20s exact %5.1f related %5.1f\n", n.c_str(), s.exactPercent(), s.relatedPercent());

    for (auto response : { smoothkey::Response::stable, smoothkey::Response::fast })
    {
        std::mt19937 rng (limits::devSeed);
        std::vector<double> times;
        int followed = 0, total = 0;
        for (int i = 0; i < 24; ++i)
        {
            const size_t p1 = rng() % ps.size();
            size_t p2 = rng() % ps.size();
            const int from = i % 12, to = (from + 1 + (int) (rng() % 11)) % 12;
            const size_t timbre = rng() % ts.size();
            auto first = chordClip (from, ps[p1], ts[timbre], clipSeed (limits::devSeed, i, 1, 1), sr, 24.0);
            const auto second = chordClip (to, ps[p2], ts[timbre], clipSeed (limits::devSeed, i, 2, 2), sr, 30.0);
            first.insert (first.end(), second.begin(), second.end());
            a.reset();
            a.setResponse (response);
            a.setSource (smoothkey::Source::mix);
            const int want = keyIndex (to, ps[p2].minor);
            const auto tr = analyse (a, first, sr, want);
            ++total;
            if (tr.settle >= 0.0 && tr.finalKey == want) { ++followed; times.push_back (std::max (0.0, tr.settle - 24.0)); }
            else if (verbose) { std::printf ("  change %d>%s want %d:", from, ps[p2].name, want); for (auto& h : tr.history) std::printf (" %.1f=%d", h.first, h.second); std::printf ("\n"); }
        }
        std::sort (times.begin(), times.end());
        std::printf ("KEYCHANGE %s followed %.1f%% median %.1f worst %.1f\n", response == smoothkey::Response::fast ? "fast" : "stable", 100.0 * followed / total, times.empty() ? -1.0 : times[times.size() / 2], times.empty() ? -1.0 : times.back());
    }
    {
        Summary fast;
        for (int tonic = 0; tonic < 12; tonic += every)
            for (size_t p = 0; p < ps.size(); ++p)
                for (size_t ti = 0; ti < ts.size(); ++ti)
                {
                    a.reset();
                    a.setResponse (smoothkey::Response::fast);
                    a.setSource (smoothkey::Source::mix);
                    const auto clip = chordClip (tonic, ps[p], ts[ti], clipSeed (limits::devSeed, tonic, (int) p, (int) ti), sr, 24.0);
                    const int want = keyIndex (tonic, ps[p].minor);
                    fast.add (analyse (a, clip, sr, want), want);
                }
        std::printf ("FAST exact %.1f%% related %.1f%% settle median %.1f p90 %.1f bounces %d\n", fast.exactPercent(), fast.relatedPercent(), fast.percentile (0.5), fast.percentile (0.9), fast.bounces);
    }
    if (! chordsOnly) for (auto src : { smoothkey::Source::mix, smoothkey::Source::voice })
    {
        Summary mel;
        auto mts = ts; mts.push_back (vocalTimbre());
        std::map<std::string, Summary> byT;
        for (int tonic = 0; tonic < 12; tonic += every)
            for (int minor = 0; minor < 2; ++minor)
                for (size_t ti = 0; ti < mts.size(); ++ti)
                {
                    a.reset();
                    a.setResponse (smoothkey::Response::stable);
                    a.setSource (src);
                    const auto clip = melodyClip (tonic, minor == 1, mts[ti], clipSeed (limits::devSeed + 7, tonic, minor, (int) ti), sr, 30.0);
                    const int want = keyIndex (tonic, minor == 1);
                    const auto tr = analyse (a, clip, sr, want);
                    mel.add (tr, want); byT[mts[ti].name].add (tr, want);
                }
        std::printf ("MELODY %s exact %.1f%% related %.1f%% settle median %.1f conf right %.2f wrong %.2f (n=%d)\n", src == smoothkey::Source::mix ? "mix" : "voice",
                     mel.exactPercent(), mel.relatedPercent(), mel.percentile (0.5), mel.meanConfCorrect(), mel.meanConfWrong(), mel.total);
        for (auto& [n, s] : byT) std::printf ("  %-20s exact %5.1f related %5.1f\n", n.c_str(), s.exactPercent(), s.relatedPercent());
    }
    std::printf ("took %.1f s\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
}
