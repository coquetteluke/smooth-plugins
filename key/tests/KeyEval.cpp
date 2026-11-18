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
    bool chordsOnly = false, verbose = false, fastOnly = false, stableOnly = false, allClips = false;
    unsigned seed = limits::devSeed;       // tune on dev seeds only: 1000, 1001, 1002 ...
    int seeds = 1;                         // key changes are only 24 clips a seed, so tuning looks at several
    int parts = 31;                        // 1 chords, 2 key changes, 4 Fast chords, 8 melody, 16 loop from its second chord
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
        else if (k == "fastonly") fastOnly = v > 0.0;
        else if (k == "stableonly") stableOnly = v > 0.0;
        else if (k == "all") allClips = v > 0.0;
        else if (k == "seed") seed = (unsigned) v;
        else if (k == "seeds") seeds = (int) v;
        else if (k == "parts") parts = (int) v;
        else if (k == "opentau") knobs.openingTau = v;
        else if (k == "section") knobs.sectionLookback = v;
        else if (k == "gap") knobs.sectionMaxGap = v;
        else if (k == "skip") knobs.sectionSkip = v;
        else if (k == "secshare") knobs.sectionShare = v;
        else if (k == "novel") knobs.sectionNovel = v;
        else if (k == "onmove") knobs.sectionOnMove = v;
        else if (k == "onloop") knobs.sectionOnLoop = v;
        else if (k == "loopgo") knobs.loopLetGo = v;
        else if (k == "openbasstau") knobs.openingBassTau = v;
        else if (k == "secbass") knobs.sectionBassShare = v;
        else if (k == "openbass") knobs.openingBassWeight = v;
        else if (k == "openshare") knobs.openingBassShare = v;
        else if (k == "loopnotes") knobs.loopNotes = v;
        else if (k == "holdafter") knobs.holdAfterLetGo = v;
        else if (k == "rebuild") knobs.rebuildOnLetGo = v;
        else if (k == "odd") knobs.oddHarmonicNotes = v;
        else if (k == "oddpen") knobs.oddPenalty = v;
        else if (k == "firstrel") knobs.firstRelativeLeadStable = v;
        else if (k == "outw") knobs.outsideWeight = v;
        else if (k == "holdstable") knobs.holdStable = v;
        else if (k == "hfast") knobs.harmonyFast = v;
        else if (k == "hstable") knobs.harmonyStable = v;
        else if (k == "hfit") knobs.harmonyFit = v;
        else if (k == "hfirst") knobs.harmonyFirst = v;
        else if (k == "hhome") knobs.harmonyHome = v;
        else if (k == "voicemodel") knobs.voiceModel = v;
        else if (k == "voicehome") knobs.voiceHome = v;
        else if (k == "hramp") knobs.harmonyRamp = v;
        else if (k == "hfirstfit") knobs.harmonyFirstFitStable = v;
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
    // Chords run on every seed asked for, like the key changes, so a few clips moving after 8 s can be counted over more clips.
    if (parts & 1) for (int sd = 0; sd < seeds; ++sd) for (int tonic = 0; tonic < 12; tonic += every)
        for (size_t p = 0; p < ps.size(); ++p)
            for (size_t ti = 0; ti < ts.size(); ++ti)
            {
                a.reset();
                a.setResponse (smoothkey::Response::stable);
                a.setSource (smoothkey::Source::mix);
                const auto clip = chordClip (tonic, ps[p], ts[ti], clipSeed (seed + (unsigned) sd, tonic, (int) p, (int) ti), sr, 24.0);
                const int want = keyIndex (tonic, ps[p].minor);
                const auto tr = analyse (a, clip, sr, want);
                if (verbose && (tr.changesAfterEight > 0 || tr.finalKey != want))
                {
                    std::printf ("  seed %u %s/%s want %d:", seed + (unsigned) sd, ps[p].name, ts[ti].name, want);
                    for (auto& h : tr.history) std::printf (" %.1f=%d", h.first, h.second);
                    std::printf ("\n");
                }
                all.add (tr, want); byProg[ps[p].name].add (tr, want); byTimbre[ts[ti].name].add (tr, want);
            }
    if (parts & 1) std::printf ("CHORDS exact %.1f%% related %.1f%% settle median %.1f p90 %.1f bounces %d moved %.1f%% conf right %.2f wrong %.2f (n=%d)\n",
                 all.exactPercent(), all.relatedPercent(), all.percentile (0.5), all.percentile (0.9), all.bounces, all.movedPercent(), all.meanConfCorrect(), all.meanConfWrong(), all.total);
    if (parts & 1) for (auto& [n, s] : byProg) std::printf ("  %-14s exact %5.1f related %5.1f\n", n.c_str(), s.exactPercent(), s.relatedPercent());
    if (parts & 1) for (auto& [n, s] : byTimbre) std::printf ("  %-20s exact %5.1f related %5.1f\n", n.c_str(), s.exactPercent(), s.relatedPercent());

    if (parts & 2) for (auto response : { smoothkey::Response::stable, smoothkey::Response::fast })
    {
        if (fastOnly && response == smoothkey::Response::stable) continue;
        if (stableOnly && response == smoothkey::Response::fast) continue;
        std::vector<double> times;
        int followed = 0, total = 0;
        for (int sd = 0; sd < seeds; ++sd)
        {
            const unsigned kcSeed = seed + (unsigned) sd;
            std::mt19937 rng (kcSeed);
            int seedFollowed = 0;
            std::vector<double> seedTimes;
            for (int i = 0; i < 24; ++i)
            {
                const size_t p1 = rng() % ps.size();
                size_t p2 = rng() % ps.size();
                const int from = i % 12, to = (from + 1 + (int) (rng() % 11)) % 12;
                const size_t timbre = rng() % ts.size();
                auto first = chordClip (from, ps[p1], ts[timbre], clipSeed (kcSeed, i, 1, 1), sr, 24.0);
                const auto second = chordClip (to, ps[p2], ts[timbre], clipSeed (kcSeed, i, 2, 2), sr, 30.0);
                first.insert (first.end(), second.begin(), second.end());
                a.reset();
                a.setResponse (response);
                a.setSource (smoothkey::Source::mix);
                const int want = keyIndex (to, ps[p2].minor);
                const auto tr = analyse (a, first, sr, want);
                ++total;
                const bool good = tr.settle >= 0.0 && tr.finalKey == want;
                if (good) { ++followed; ++seedFollowed; times.push_back (std::max (0.0, tr.settle - 24.0)); seedTimes.push_back (std::max (0.0, tr.settle - 24.0)); }
                if (verbose && (! good || allClips || (response == smoothkey::Response::fast && tr.settle - 24.0 > 8.0)))
                {
                    std::printf ("  %s seed %u: %s %d>%s %s want %d:", good ? "slow" : "MISS", kcSeed, ps[p1].name, from, ps[p2].name, ts[timbre].name, want);
                    for (auto& h : tr.history) std::printf (" %.1f=%d", h.first, h.second);
                    std::printf ("\n");
                }
            }
            std::sort (seedTimes.begin(), seedTimes.end());
            std::printf ("  seed %u %s followed %d of 24, median %.1f\n", kcSeed, response == smoothkey::Response::fast ? "fast" : "stable", seedFollowed, seedTimes.empty() ? -1.0 : seedTimes[seedTimes.size() / 2]);
        }
        std::sort (times.begin(), times.end());
        std::printf ("KEYCHANGE %s followed %.1f%% median %.1f worst %.1f\n", response == smoothkey::Response::fast ? "fast" : "stable", 100.0 * followed / total, times.empty() ? -1.0 : times[times.size() / 2], times.empty() ? -1.0 : times.back());
    }
    if (parts & 16)
    {
        Summary rotated;
        for (int tonic = 0; tonic < 12; tonic += std::max (every, 3))
            for (size_t p = 0; p < ps.size(); ++p)
                for (size_t ti = 0; ti < ts.size(); ++ti)
                {
                    a.reset();
                    a.setResponse (smoothkey::Response::stable);
                    a.setSource (smoothkey::Source::mix);
                    const auto clip = chordClip (tonic, ps[p], ts[ti], clipSeed (seed, tonic, (int) p, (int) ti), sr, 24.0, 1);
                    const int want = keyIndex (tonic, ps[p].minor);
                    rotated.add (analyse (a, clip, sr, want), want);
                }
        std::printf ("ROTATED exact %.1f%% related %.1f%% moved %.1f%%\n", rotated.exactPercent(), rotated.relatedPercent(), rotated.movedPercent());
    }
    if (parts & 4)
    {
        Summary fast;
        for (int sd = 0; sd < seeds; ++sd) for (int tonic = 0; tonic < 12; tonic += every)
            for (size_t p = 0; p < ps.size(); ++p)
                for (size_t ti = 0; ti < ts.size(); ++ti)
                {
                    a.reset();
                    a.setResponse (smoothkey::Response::fast);
                    a.setSource (smoothkey::Source::mix);
                    const auto clip = chordClip (tonic, ps[p], ts[ti], clipSeed (seed + (unsigned) sd, tonic, (int) p, (int) ti), sr, 24.0);
                    const int want = keyIndex (tonic, ps[p].minor);
                    fast.add (analyse (a, clip, sr, want), want);
                }
        std::printf ("FAST exact %.1f%% related %.1f%% settle median %.1f p90 %.1f bounces %d\n", fast.exactPercent(), fast.relatedPercent(), fast.percentile (0.5), fast.percentile (0.9), fast.bounces);
    }
    if ((parts & 8) && ! chordsOnly) for (auto src : { smoothkey::Source::mix, smoothkey::Source::voice })
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
                    const auto clip = melodyClip (tonic, minor == 1, mts[ti], clipSeed (seed + 7, tonic, minor, (int) ti), sr, 30.0);
                    const int want = keyIndex (tonic, minor == 1);
                    const auto tr = analyse (a, clip, sr, want);
                    mel.add (tr, want); byT[mts[ti].name].add (tr, want);
                    if (verbose && tr.finalKey != want)
                    {
                        std::printf ("  melody %s want %d:", mts[ti].name, want);
                        for (auto& h : tr.history) std::printf (" %.1f=%d", h.first, h.second);
                        std::printf ("\n");
                    }
                }
        std::printf ("MELODY %s exact %.1f%% related %.1f%% settle median %.1f conf right %.2f wrong %.2f (n=%d)\n", src == smoothkey::Source::mix ? "mix" : "voice",
                     mel.exactPercent(), mel.relatedPercent(), mel.percentile (0.5), mel.meanConfCorrect(), mel.meanConfWrong(), mel.total);
        for (auto& [n, s] : byT) std::printf ("  %-20s exact %5.1f related %5.1f\n", n.c_str(), s.exactPercent(), s.relatedPercent());
    }
    std::printf ("took %.1f s\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
}
