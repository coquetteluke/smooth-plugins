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
        { "voiceModel", &K::voiceModel }, { "heldShare", &K::heldShare }, { "heldFull", &K::heldFull }, { "tauVoice", &K::tauVoice }, { "voiceOutsideWeight", &K::voiceOutsideWeight }, { "voiceChangeEvidence", &K::voiceChangeEvidence }, { "voiceChangeFit", &K::voiceChangeFit }, { "voiceChangeOld", &K::voiceChangeOld }, { "voiceChangeShared", &K::voiceChangeShared }, { "voiceChangeFull", &K::voiceChangeFull }, { "voiceChangeWindow", &K::voiceChangeWindow }, { "voiceChangeWatch", &K::voiceChangeWatch }, { "voiceInheritEvidence", &K::voiceInheritEvidence }, { "voiceInheritHeld", &K::voiceInheritHeld }, { "voiceMoveUnsure", &K::voiceMoveUnsure }, { "voiceSureSigma", &K::voiceSureSigma }, { "voiceRefSeconds", &K::voiceRefSeconds }, { "voiceChangeSlack", &K::voiceChangeSlack }, { "voiceMoveEvidence", &K::voiceMoveEvidence }, { "voiceUnsureMargin", &K::voiceUnsureMargin }, { "voiceSureHeld", &K::voiceSureHeld }, { "voiceMinHeld", &K::voiceMinHeld }, { "voiceFitLow", &K::voiceFitLow }, { "voiceFitHigh", &K::voiceFitHigh }, { "voiceMargin", &K::voiceMargin }, { "voiceHome", &K::voiceHome }, { "offKeyEnter", &K::offKeyEnter }, { "offKeyLeave", &K::offKeyLeave }, { "offKeyLeaveSeconds", &K::offKeyLeaveSeconds }, { "clarityTau", &K::clarityTau },
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
    std::string only;   // "only sung|rap|step1|step2" runs just that part
    KeyAnalyser::Knobs knobs;
    if (argc % 2 == 0) { std::fprintf (stderr, "arguments come in pairs: name value\n"); return 2; }
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        const double v = std::atof (argv[i + 1]);
        if (k == "seed") seed = (unsigned) v;
        else if (k == "step") step = (int) v;
        else if (k == "only") { only = argv[i + 1]; }
        else if (! setKnob (knobs, k, v)) { std::fprintf (stderr, "unknown knob %s\n", k.c_str()); return 2; }
    }
    KeyAnalyser a;
    a.setKnobs (knobs);
    a.configure (48000.0);
    for (auto source : { Source::voice, Source::mix })
    {
        const char* tag = source == Source::voice ? "Single Voice" : "Full Mix";
        if (! only.empty() && only != "sung" && only != "rap") break;
        if (only == "rap") { const auto r = runRap (a, source, seed + 1); std::printf ("%-12s rap %d clips: steady %.1f%% (limit %.0f), never read %d, mean confidence %.2f, time at 0.5 or more %.1f%% (limit %.0f)\n", tag, r.total, r.steadyPercent(), limits::rapSteadyPercent, r.neverRead, r.meanConfidence(), r.confidentPercent(), limits::rapConfidentPercent); continue; }
        const auto s = runSung (a, source, seed, limits::sungReferenceHz, step);
        std::printf ("%-12s sung %d clips: right notes %.1f%% (limit %.0f), exact %.1f%%, median settle %s (limit %.0f s), notes change more than once %.1f%% (limit %.0f), reference near 440 %.1f%% (limit %.0f)\n",
                     tag, s.total, s.rightPercent(), limits::sungRightNotesPercent, s.exactPercent(),
                     s.medianSettle() > 1.0e8 ? "never" : std::to_string (s.medianSettle()).c_str(), limits::sungMedianSettle, s.movingPercent(), limits::sungMovingPercent,
                     s.referencePercent(), limits::sungReferencePercent);
        std::printf ("%-12s sung: leave the right notes after showing them %.1f%% (limit %.0f)\n", tag, s.leftPercent(), limits::sungLeftPercent);
        if (only == "sung") continue;
        const auto r = runRap (a, source, seed + 1);
        std::printf ("%-12s rap %d clips: steady %.1f%% (limit %.0f), never read %d, mean confidence %.2f, time at 0.5 or more %.1f%% (limit %.0f)\n", tag, r.total, r.steadyPercent(), limits::rapSteadyPercent, r.neverRead, r.meanConfidence(), r.confidentPercent(), limits::rapConfidentPercent);
    }
    if (only == "sungtrace")
    {
        // Full Mix, per sung clip that shows the right 7 notes and then leaves them: every change of reading, and whether it was read as a lone voice.
        const double sr = 48000.0;
        int left = 0, total = 0;
        for (int tonic = 0; tonic < 12; tonic += step)
            for (int minor = 0; minor < 2; ++minor)
            {
                const auto clip = corpus::sungClip (tonic, minor == 1, corpus::clipSeed (seed, tonic, minor, 5), sr, 60.0);
                a.reset(); a.setResponse (Response::stable); a.setSource (Source::mix);
                const int want = notesOf (corpus::keyIndex (tonic, minor == 1));
                std::string line;
                int last = -1; bool wasRight = false, leftIt = false, lone = false;
                for (int pos = 0; pos < (int) clip.size(); pos += 512)
                {
                    a.push (clip.data() + pos, std::min (512, (int) clip.size() - pos));
                    const auto& r = a.reading();
                    const int k = r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0);
                    if (k != last || r.loneVoice != lone)
                    {
                        char b[64]; std::snprintf (b, sizeof b, " %.1f:%d(%d)%s c%.2f", (pos + 512) / sr, k, notesOf (k), r.loneVoice ? "L" : "M", a.tuningClarity());
                        line += b; last = k; lone = r.loneVoice;
                    }
                    if (k >= 0) { if (notesOf (k) == want) wasRight = true; else if (wasRight) leftIt = true; }
                }
                ++total;
                if (leftIt) { ++left; std::printf ("LEFT %d%s want %d:%s\n", tonic, minor ? "m" : "M", want, line.c_str()); }
            }
        std::printf ("left %d of %d\n", left, total);
        return 0;
    }
    if (only == "steptrace" || only == "steptrace2")
    {
        // Per clip: when the new 7 notes first show after the change at 45 s, the final key, and every change of reading.
        const int semis = only == "steptrace" ? 1 : 2;
        const double sr = 48000.0, half = 45.0;
        for (int tonic = 0; tonic < 12; tonic += step)
            for (int minor = 0; minor < 2; ++minor)
            {
                auto clip = corpus::sungClip (tonic, minor == 1, corpus::clipSeed (seed + 1 + semis, tonic, minor, 6), sr, half);
                const auto second = corpus::sungClip ((tonic + semis) % 12, minor == 1, corpus::clipSeed (seed + 1 + semis, tonic, minor, 7), sr, half);
                clip.insert (clip.end(), second.begin(), second.end());
                a.reset(); a.setResponse (Response::stable); a.setSource (Source::voice);
                std::printf ("CLIP %d%s -> %d:", tonic, minor ? "m" : "M", notesOf (corpus::keyIndex ((tonic + semis) % 12, minor == 1)));
                int last = -1, sureWrong = 0;
                for (int pos = 0; pos < (int) clip.size(); pos += 512)
                {
                    a.push (clip.data() + pos, std::min (512, (int) clip.size() - pos));
                    const auto& r = a.reading();
                    const int k = r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0);
                    if (k != last) { std::printf (" %.1f:%d(%d)", (pos + 512) / sr, k, notesOf (k)); last = k; }
                    const double t = (pos + 512) / sr;
                    if (t >= half + 30.0 && k >= 0 && r.confidence >= 0.5f && notesOf (k) != notesOf (corpus::keyIndex ((tonic + semis) % 12, minor == 1))) ++sureWrong;
                    if (t >= half + 30.0 && t < half + 30.0 + 512 / sr)
                        std::printf (" | at +30 s: %d conf %.2f%s alt %d |", notesOf (k), (double) r.confidence, r.unsure ? " unsure" : "", notesOf (r.altKey < 0 ? -1 : r.altKey + (r.altMinor ? 12 : 0)));
                }
                std::printf (" sureWrongFrames %d alt %d", sureWrong, notesOf (a.reading().altKey < 0 ? -1 : a.reading().altKey + (a.reading().altMinor ? 12 : 0)));
                std::printf (" conf %.2f%s\n", (double) a.reading().confidence, a.reading().unsure ? " unsure" : "");
            }
        return 0;
    }
    if (only.empty() || only == "step1") {
    const auto u = runStepUp (a, seed + 2, step);
    std::printf ("Single Voice up a semitone, %d clips: followed in 30 s %.1f%% (limit %.0f), median %.1f s, ends on the new notes %.1f%%\n", u.total, u.followedPercent(),
                 limits::voiceStepUpPercent, medianOf (u.times), u.endRightPercent());
    }
    if (only.empty() || only == "step2") {
    const auto u2 = runStepUp (a, seed + 3, step, 45.0, 2);
    std::printf ("Single Voice up two semitones, %d clips: followed in 30 s or not sure from then on %.1f%%, followed in 30 s %.1f%%, median %.1f s, ends on the new notes %.1f%%\n", u2.total, u2.honestPercent(), u2.followedPercent(),
                 medianOf (u2.times), u2.endRightPercent());
    }
    return 0;
}
