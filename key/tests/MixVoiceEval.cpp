// Development tool, not part of the plug-in. Music with the off key singer on top (MixVoiceTests.h) on any seed: first the
// same lines PluginTest prints, from the same runner, then the whole table.
// Build: clang++ -std=c++17 -O2 -Isrc -Itests tests/MixVoiceEval.cpp -o build/MixVoiceEval
// Run: build/MixVoiceEval [seed 5000] [seeds 1] [source 0] [extra 0] [level X] [alone 1] [verbose 0] [step 1] [knob value ...]
//   seeds N    this many seeds in a row from seed, and one table over all of them
//   source 1   Source Single Voice (information only; 0 is Full Mix, which is what PluginTest checks)
//   extra 1    also the voice 12 and 18 dB over the music
//   level X    the voice X dB against the music, in place of the usual three levels; may be given more than once
//   alone 0    leaves out the music alone and the singer alone
//   verbose 1  a line for every clip that went wrong in some way: L marks a reading taken as a lone voice
//   knobs      voiceModel 0 is the reading before 1.6.0, which never takes anything for a lone voice; offKeyEnter, offKeyLeave;
//              any other lone voice knob by its name in KeyAnalyser::Knobs (voiceSureSigma, voiceChangeEvidence, ...)
//   nolevels 1 no music with the voice on top: with alone 1 (the default) only the music alone and the singer alone
//   nomusic 1  leaves out the music alone (with nolevels 1: the singer alone only, 24 clips a seed, about 20 s)
// A minute of audio takes most of a second, so one seed of Full Mix is about five minutes.
#include "MixVoiceTests.h"
#include <cmath>
#include <cstdlib>

using namespace mixvoicetests;

static void table (const char* title, const std::vector<Row>& rows)
{
    std::printf ("\n%s\n", title);
    std::printf ("%-58s %5s %6s %6s %7s %8s %8s %7s %7s %8s %8s %7s %7s %6s %6s %9s %9s\n", "", "clips", "exact", "right", "related", "settle", "settle", "sure,", "sure,", "changes", "clips", "moved", "bounces", "lone", "lone", "right if", "right if");
    std::printf ("%-58s %5s %6s %6s %7s %8s %8s %7s %7s %8s %8s %7s %7s %6s %6s %9s %9s\n", "", "", "%", "notes%", "%", "notes s", "exact s", "right%", "wrong%", "a clip", "changed%", ">8 s %", "", "clips%", "time%", "lone", "not lone");
    for (const auto& r : rows)
    {
        char loneRight[24] = "-", plainRight[24] = "-";
        if (r.lone > 0) std::snprintf (loneRight, sizeof loneRight, "%d of %d", r.loneRight, r.lone);
        if (r.total - r.lone > 0) std::snprintf (plainRight, sizeof plainRight, "%d of %d", r.plainRight, r.total - r.lone);
        std::printf ("%-58s %5d %6.1f %6.1f %7.1f %8.1f %8.1f %7.1f %7.1f %8.2f %8.1f %7.1f %7d %6.1f %6.1f %9s %9s\n", r.label.c_str(), r.total, r.exactPercent(), r.rightPercent(), r.relatedPercent(),
                     r.medianSettleNotes(), r.medianSettleExact(), r.rightConfidentPercent(), r.wrongConfidentPercent(), r.changesPerClip(), r.changedPercent(), r.movedPercent(), r.bounces,
                     r.lonePercent(), r.loneSharePercent(), loneRight, plainRight);
    }
    std::fflush (stdout);
}

// Any lone voice setting of the analyser by its name in KeyAnalyser::Knobs.
static bool setKnob (KeyAnalyser::Knobs& k, const std::string& name, double v)
{
    using K = KeyAnalyser::Knobs;
    static const std::pair<const char*, double K::*> table[] = {
        { "voiceModel", &K::voiceModel }, { "offKeyEnter", &K::offKeyEnter }, { "offKeyLeave", &K::offKeyLeave }, { "offKeyLeaveSeconds", &K::offKeyLeaveSeconds },
        { "heldShare", &K::heldShare }, { "heldFull", &K::heldFull }, { "tauVoice", &K::tauVoice }, { "voiceOutsideWeight", &K::voiceOutsideWeight },
        { "voiceChangeEvidence", &K::voiceChangeEvidence }, { "voiceChangeFit", &K::voiceChangeFit }, { "voiceChangeOld", &K::voiceChangeOld },
        { "voiceChangeShared", &K::voiceChangeShared }, { "voiceChangeFull", &K::voiceChangeFull }, { "voiceChangeWindow", &K::voiceChangeWindow },
        { "voiceChangeWatch", &K::voiceChangeWatch }, { "voiceInheritEvidence", &K::voiceInheritEvidence }, { "voiceInheritHeld", &K::voiceInheritHeld },
        { "voiceMoveUnsure", &K::voiceMoveUnsure }, { "voiceSureSigma", &K::voiceSureSigma }, { "voiceSureEvents", &K::voiceSureEvents }, { "voiceRefSeconds", &K::voiceRefSeconds },
        { "voiceChangeSlack", &K::voiceChangeSlack }, { "voiceChangeEvents", &K::voiceChangeEvents }, { "voiceMoveEvidence", &K::voiceMoveEvidence }, { "voiceUnsureMargin", &K::voiceUnsureMargin },
        { "voiceSureHeld", &K::voiceSureHeld }, { "voiceMinHeld", &K::voiceMinHeld }, { "voiceFitLow", &K::voiceFitLow }, { "voiceFitHigh", &K::voiceFitHigh },
        { "voiceMargin", &K::voiceMargin }, { "voiceHome", &K::voiceHome }, { "clarityTau", &K::clarityTau },
    };
    for (const auto& t : table)
        if (name == t.first) { k.*(t.second) = v; return true; }
    return false;
}

int main (int argc, char** argv)
{
    unsigned seed = 5000;
    int seeds = 1;
    bool extra = false, verbose = false, ownLevels = false;
    Options o;
    KeyAnalyser::Knobs knobs;
    if (argc % 2 == 0) { std::fprintf (stderr, "arguments come in pairs: name value\n"); return 2; }
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const double v = std::atof (argv[i + 1]);
        if (k == "seed") seed = (unsigned) v;
        else if (k == "seeds") seeds = (int) v;
        else if (k == "source") o.source = v > 0.0 ? Source::voice : Source::mix;
        else if (k == "extra") extra = v > 0.0;
        else if (k == "level") { if (! ownLevels) o.levels.clear(); ownLevels = true; o.levels.push_back (v); }
        else if (k == "alone") o.musicAlone = o.voiceAlone = v > 0.0;
        else if (k == "verbose") verbose = v > 0.0;
        else if (k == "step") o.tonicStep = (int) v;
        else if (k == "nolevels") { if (v > 0.0) { o.levels.clear(); ownLevels = true; } }
        else if (k == "nomusic") o.musicAlone = ! (v > 0.0);
        else if (! setKnob (knobs, k, v)) { std::fprintf (stderr, "unknown name %s\n", k.c_str()); return 2; }
    }
    if (extra) { o.levels.push_back (12.0); o.levels.push_back (18.0); }
    if (verbose) o.log = stdout;
    KeyAnalyser a;
    a.setKnobs (knobs);
    a.configure (48000.0);

    // TestKit's check and info, without its failure count.
    const auto check = [] (bool ok, const std::string& what, double measured, const char* unit) { std::printf ("%s  %s = %.3f %s\n", ok ? "PASS" : "FAIL", what.c_str(), measured, unit); };
    const auto info = [] (const std::string& what, double value, const char* unit) { std::printf ("INFO  %s = %.3f %s\n", what.c_str(), value, unit); };

    std::vector<Row> all;
    for (int s = 0; s < seeds; ++s)
    {
        const unsigned sd = seed + (unsigned) s;
        if (verbose) std::printf ("clips that went wrong in some way, seed %u (time=key, 0 to 11 major from C, 12 to 23 minor, -1 none; L lone voice; confidence in brackets)\n", sd);
        const auto rows = runTable (a, sd, o);
        std::printf ("seed %u\n", sd);
        report (rows, check, info);
        table (("seed " + std::to_string (sd)).c_str(), rows);
        if (all.empty()) all = rows; else for (size_t i = 0; i < rows.size(); ++i) all[i].merge (rows[i]);
    }
    if (seeds > 1) table (("seeds " + std::to_string (seed) + " to " + std::to_string (seed + (unsigned) seeds - 1) + " together").c_str(), all);
    return 0;
}
