// Development tool, not part of the plug-in. Runs the analyser over a recording, as fast as the CPU allows, and prints the reading over
// time: what the plug-in would show if that file were played into it. The file is read in place, nothing is copied.
// Build: clang++ -std=c++17 -O2 -Isrc tests/VoxEval.cpp -o build/VoxEval
// Run:   build/VoxEval <file.wav>[+<file.wav>...] [stable|fast] [mix|voice] [knob value ...] [dump 1]   (files joined by + play in a row)
// Output: one line per 50 ms, "S <seconds> <key 0 to 23 or -1> <confidence> <the key scoring highest now> <runner up> <margin> <unsure> <lone voice> <tuning clarity>", then "CHG <seconds> <key name> <confidence>" per change.
// With "dump 1": one line per analysis frame with the notes heard, for looking into a wrong reading.
#include "KeyAnalyser.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>

using smoothkey::KeyAnalyser;

static bool readWav (const char* path, std::vector<float>& out, double& rate)
{
    FILE* f = std::fopen (path, "rb");
    if (f == nullptr) return false;
    std::fseek (f, 0, SEEK_END);
    const long size = std::ftell (f);
    std::fseek (f, 0, SEEK_SET);
    std::vector<unsigned char> b ((size_t) size);
    const bool ok = std::fread (b.data(), 1, (size_t) size, f) == (size_t) size;
    std::fclose (f);
    if (! ok || size < 12 || std::memcmp (b.data(), "RIFF", 4) != 0 || std::memcmp (b.data() + 8, "WAVE", 4) != 0) return false;
    auto u16 = [&] (long p) { return (unsigned) (b[(size_t) p] | (b[(size_t) p + 1] << 8)); };
    auto u32 = [&] (long p) { return (uint32_t) b[(size_t) p] | ((uint32_t) b[(size_t) p + 1] << 8) | ((uint32_t) b[(size_t) p + 2] << 16) | ((uint32_t) b[(size_t) p + 3] << 24); };
    unsigned format = 0, channels = 0, bits = 0;
    rate = 0.0;
    for (long p = 12; p + 8 <= size;)
    {
        const long len = (long) u32 (p + 4);
        if (std::memcmp (b.data() + p, "fmt ", 4) == 0)
        {
            format = u16 (p + 8); channels = u16 (p + 10); rate = (double) u32 (p + 12); bits = u16 (p + 22);
            if (format == 0xfffe && len >= 26) format = u16 (p + 32);
        }
        else if (std::memcmp (b.data() + p, "data", 4) == 0 && channels > 0 && bits > 0)
        {
            const long bytes = (long) bits / 8, frames = std::min (len, size - p - 8) / (bytes * (long) channels);
            out.resize ((size_t) frames);
            for (long i = 0; i < frames; ++i)
            {
                double sum = 0.0;
                for (unsigned c = 0; c < channels; ++c)
                {
                    const long q = p + 8 + (i * (long) channels + (long) c) * bytes;
                    if (format == 3 && bits == 32) { float v; std::memcpy (&v, b.data() + q, 4); sum += v; }
                    else if (bits == 16) sum += (int16_t) u16 (q) / 32768.0;
                    else if (bits == 24) sum += ((int32_t) ((uint32_t) b[(size_t) q] << 8 | (uint32_t) b[(size_t) q + 1] << 16 | (uint32_t) b[(size_t) q + 2] << 24) >> 8) / 8388608.0;
                    else if (bits == 32) sum += (int32_t) u32 (q) / 2147483648.0;
                }
                out[(size_t) i] = (float) (sum / channels);
            }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

// Any setting of the analyser by its name in KeyAnalyser::Knobs.
static bool setKnob (KeyAnalyser::Knobs& k, const std::string& name, double v)
{
    using K = KeyAnalyser::Knobs;
    static const std::pair<const char*, double K::*> table[] = {
        { "tauStable", &K::tauStable }, { "tauFast", &K::tauFast }, { "dwellStable", &K::dwellStable }, { "dwellFast", &K::dwellFast },
        { "marginStable", &K::marginStable }, { "marginFast", &K::marginFast }, { "firstStable", &K::firstStable }, { "firstFast", &K::firstFast },
        { "firstLeadStable", &K::firstLeadStable }, { "firstRelativeLeadStable", &K::firstRelativeLeadStable }, { "settleSeconds", &K::settleSeconds },
        { "bassWeight", &K::bassWeight }, { "relativeGuard", &K::relativeGuard }, { "returnGuard", &K::returnGuard }, { "outsideShare", &K::outsideShare },
        { "outsideStable", &K::outsideStable }, { "outsideFast", &K::outsideFast }, { "recentStable", &K::recentStable }, { "recentFast", &K::recentFast },
        { "holdStable", &K::holdStable }, { "holdFast", &K::holdFast }, { "outsideWeight", &K::outsideWeight }, { "chromaPower", &K::chromaPower },
        { "softness", &K::softness }, { "harmonicDecay", &K::harmonicDecay },
        { "voiceModel", &K::voiceModel }, { "heldShare", &K::heldShare }, { "heldFull", &K::heldFull }, { "tauVoice", &K::tauVoice }, { "voiceOutsideWeight", &K::voiceOutsideWeight }, { "voiceChangeEvidence", &K::voiceChangeEvidence }, { "voiceChangeFit", &K::voiceChangeFit }, { "voiceChangeOld", &K::voiceChangeOld }, { "voiceChangeShared", &K::voiceChangeShared }, { "voiceChangeFull", &K::voiceChangeFull }, { "voiceChangeWindow", &K::voiceChangeWindow }, { "voiceChangeWatch", &K::voiceChangeWatch }, { "voiceInheritEvidence", &K::voiceInheritEvidence }, { "voiceInheritHeld", &K::voiceInheritHeld }, { "voiceMoveUnsure", &K::voiceMoveUnsure }, { "voiceSureSigma", &K::voiceSureSigma }, { "voiceSureEvents", &K::voiceSureEvents }, { "voiceRefSeconds", &K::voiceRefSeconds }, { "voiceChangeSlack", &K::voiceChangeSlack }, { "voiceChangeEvents", &K::voiceChangeEvents }, { "voiceMoveEvidence", &K::voiceMoveEvidence }, { "voiceUnsureMargin", &K::voiceUnsureMargin }, { "voiceSureHeld", &K::voiceSureHeld }, { "voiceMinHeld", &K::voiceMinHeld }, { "voiceFitLow", &K::voiceFitLow }, { "voiceFitHigh", &K::voiceFitHigh }, { "voiceMargin", &K::voiceMargin }, { "voiceHome", &K::voiceHome }, { "offKeyEnter", &K::offKeyEnter }, { "offKeyLeave", &K::offKeyLeave }, { "offKeyLeaveSeconds", &K::offKeyLeaveSeconds }, { "clarityTau", &K::clarityTau },
    };
    for (const auto& t : table)
        if (name == t.first) { k.*(t.second) = v; return true; }
    return false;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: VoxEval <file.wav> [stable|fast] [mix|voice] [knob value ...]\n"); return 2; }
    std::vector<float> x;
    double rate = 0.0;
    {
        // Several files joined with '+' are played one after the other, as one take (same sample rate), read in place.
        const std::string all = argv[1];
        for (size_t from = 0; from <= all.size();)
        {
            const size_t to = std::min (all.find ('+', from), all.size());
            std::vector<float> part;
            double r = 0.0;
            const std::string one = all.substr (from, to - from);
            if (! readWav (one.c_str(), part, r) || (rate > 0.0 && r != rate)) { std::fprintf (stderr, "cannot read %s\n", one.c_str()); return 1; }
            rate = r;
            x.insert (x.end(), part.begin(), part.end());
            from = to + 1;
        }
    }
    KeyAnalyser::Knobs knobs;
    auto response = smoothkey::Response::stable;
    auto source = smoothkey::Source::mix;
    bool dump = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string k = argv[i];
        if (k == "stable") { response = smoothkey::Response::stable; continue; }
        if (k == "fast") { response = smoothkey::Response::fast; continue; }
        if (k == "mix") { source = smoothkey::Source::mix; continue; }
        if (k == "voice") { source = smoothkey::Source::voice; continue; }
        if (i + 1 >= argc) { std::fprintf (stderr, "knob %s needs a value\n", k.c_str()); return 2; }
        const double v = std::atof (argv[++i]);
        if (k == "dump") dump = v > 0.0;
        else if (! setKnob (knobs, k, v)) { std::fprintf (stderr, "unknown knob %s\n", k.c_str()); return 2; }
    }
    KeyAnalyser a;
    a.setKnobs (knobs);
    a.configure (rate);
    a.reset();
    a.setResponse (response);
    a.setSource (source);
    const int chunk = (int) (rate / 20.0);
    int last = -2;
    std::string changes;
    double heard = 0.0;
    for (size_t pos = 0; pos < x.size(); pos += (size_t) chunk)
    {
        const int n = (int) std::min ((size_t) chunk, x.size() - pos);
        a.push (x.data() + pos, n);
        const auto& r = a.reading();
        const int idx = r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0);
        const double t = (double) (pos + (size_t) n) / rate;
        int lead = -1;   // the key that scores highest now, whatever is shown
        for (int k = 0; k < 24; ++k) if (r.candidates[(size_t) k] > 0.0f && (lead < 0 || r.candidates[(size_t) k] > r.candidates[(size_t) lead])) lead = k;
        std::printf ("S %.2f %d %.3f %d %d %.3f %d %d %.3f\n", t, idx, (double) r.confidence, lead, r.altKey < 0 ? -1 : r.altKey + (r.altMinor ? 12 : 0), (double) r.margin, r.unsure ? 1 : 0, r.loneVoice ? 1 : 0, a.clarityNow());
        if (idx != last)
        {
            char line[96];
            std::snprintf (line, sizeof line, "CHG %.2f %s%s %.2f\n", t, idx < 0 ? "none" : smoothkey::keyNames[idx % 12], idx < 0 ? "" : (idx >= 12 ? " minor" : " major"), (double) r.confidence);
            changes += line;
            last = idx;
        }
        if (dump && a.secondsHeard() != heard && a.heldNoteNow() >= 0)
            std::printf ("H %.2f %d %.3f\n", t, a.heldNoteNow(), a.heldWeightNow());
        if (dump && a.secondsHeard() != heard)
        {
            heard = a.secondsHeard();
            std::printf ("F %.2f", t);
            for (float c : r.chroma) std::printf (" %.2f", (double) c);
            std::printf (" | tune %.0f\n", (double) r.tuningCents);
        }
    }
    std::fputs (changes.c_str(), stdout);
    std::printf ("PEAKS");
    for (int i = 1; i < 12; ++i) std::printf (" %.2f", a.changeCountPeak (i));   // highest change count toward the set i semitones above the shown one
    std::printf ("\n");
    std::printf ("KEYCHANGES %d\n", a.voiceKeyChanges());   // moves made because a lone voice's song changed key
    return 0;
}
