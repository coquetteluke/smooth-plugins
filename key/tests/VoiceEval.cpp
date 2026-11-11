// Development tool, not part of the plug-in. The lone voice tests of VoiceTests.h on a DEV seed, so tuning never sees the seed
// PluginTest reports. Build: clang++ -std=c++17 -O2 -Isrc -Itests tests/VoiceEval.cpp -o build/VoiceEval
// Run: build/VoiceEval [seed 2000] [step 1] [knob value ...]   knobs by their name in KeyAnalyser::Knobs, see setKnob below
#include "VoiceTests.h"
#include "PassLimits.h"
#include <cstdlib>
#include <string>

using namespace voicetests;

static bool setKnob (KeyAnalyser::Knobs& k, const std::string& name, double v)
{
    using K = KeyAnalyser::Knobs;
    static const std::pair<const char*, double K::*> table[] = {
        { "voiceModel", &K::voiceModel }, { "heldShare", &K::heldShare }, { "heldFull", &K::heldFull }, { "tauVoice", &K::tauVoice }, { "recentVoice", &K::recentVoice }, { "voiceOutsideWeight", &K::voiceOutsideWeight }, { "voiceTriggerHeld", &K::voiceTriggerHeld }, { "voiceTriggerLead", &K::voiceTriggerLead }, { "voiceTriggerSeconds", &K::voiceTriggerSeconds }, { "voiceConfirmHeld", &K::voiceConfirmHeld }, { "voiceConfirmMax", &K::voiceConfirmMax }, { "voiceOutsideFit", &K::voiceOutsideFit }, { "voiceConfirmSlack", &K::voiceConfirmSlack }, { "voiceSureHeld", &K::voiceSureHeld }, { "voiceWeighSeconds", &K::voiceWeighSeconds }, { "voiceRestSeconds", &K::voiceRestSeconds }, { "voiceMinHeld", &K::voiceMinHeld }, { "voiceFitLow", &K::voiceFitLow }, { "voiceFitHigh", &K::voiceFitHigh }, { "voiceMargin", &K::voiceMargin }, { "voiceHome", &K::voiceHome }, { "offKeyEnter", &K::offKeyEnter }, { "offKeyLeave", &K::offKeyLeave }, { "offKeyLeaveSeconds", &K::offKeyLeaveSeconds }, { "clarityTau", &K::clarityTau },
        { "dwellStable", &K::dwellStable }, { "marginStable", &K::marginStable }, { "holdStable", &K::holdStable }, { "firstStable", &K::firstStable },
        { "firstLeadStable", &K::firstLeadStable }, { "relativeGuard", &K::relativeGuard }, { "returnGuard", &K::returnGuard },
    };
    for (const auto& t : table)
        if (name == t.first) { k.*(t.second) = v; return true; }
    return false;
}

int main (int argc, char** argv)
{
    unsigned seed = 2000;
    int step = 1;
    KeyAnalyser::Knobs knobs;
    if (argc % 2 == 0) { std::fprintf (stderr, "arguments come in pairs: name value\n"); return 2; }
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const double v = std::atof (argv[i + 1]);
        if (k == "seed") seed = (unsigned) v;
        else if (k == "step") step = (int) v;
        else if (! setKnob (knobs, k, v)) { std::fprintf (stderr, "unknown knob %s\n", k.c_str()); return 2; }
    }
    KeyAnalyser a;
    a.setKnobs (knobs);
    a.configure (48000.0);
    for (auto source : { Source::voice, Source::mix })
    {
        const char* tag = source == Source::voice ? "Single Voice" : "Full Mix";
        const auto s = runSung (a, source, seed, limits::sungReferenceHz, step);
        std::printf ("%-12s sung %d clips: right notes %.1f%% (limit %.0f), exact %.1f%%, median settle %s (limit %.0f s), notes change more than once %.1f%% (limit %.0f), reference near 440 %.1f%% (limit %.0f)\n",
                     tag, s.total, s.rightPercent(), limits::sungRightNotesPercent, s.exactPercent(),
                     s.medianSettle() > 1.0e8 ? "never" : std::to_string (s.medianSettle()).c_str(), limits::sungMedianSettle, s.movingPercent(), limits::sungMovingPercent,
                     s.referencePercent(), limits::sungReferencePercent);
        const auto r = runRap (a, source, seed + 1);
        std::printf ("%-12s sung: leave the right notes after showing them %.1f%% (limit %.0f)\n", tag, s.leftPercent(), limits::sungLeftPercent);
        std::printf ("%-12s rap %d clips: steady %.1f%% (limit %.0f), never read %d, mean confidence %.2f, time at 0.5 or more %.1f%% (limit %.0f)\n", tag, r.total, r.steadyPercent(), limits::rapSteadyPercent, r.neverRead, r.meanConfidence(), r.confidentPercent(), limits::rapConfidentPercent);
    }
    const auto u = runStepUp (a, seed + 2, step);
    std::printf ("Single Voice up a semitone, %d clips: followed in 30 s %.1f%% (limit %.0f), median %.1f s, ends on the new notes %.1f%%\n", u.total, u.followedPercent(),
                 limits::voiceStepUpPercent, medianOf (u.times), u.endRightPercent());
    const auto u2 = runStepUp (a, seed + 3, step, 45.0, 2);
    std::printf ("Single Voice up two semitones, %d clips: followed in 30 s %.1f%% (limit %.0f), median %.1f s, ends on the new notes %.1f%%\n", u2.total, u2.followedPercent(),
                 limits::voiceStepUp2Percent, medianOf (u2.times), u2.endRightPercent());
    return 0;
}
