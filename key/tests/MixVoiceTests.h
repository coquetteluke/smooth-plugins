#pragma once
// Music with an off key voice on top. The question: does Key read the music right when the off key singer of KeyCorpus.h
// (sungClip) sits on top of it, and how does that compare with the music alone and with the singer alone? One table: music
// alone, music with the voice 6 dB under it, as loud as it and 6 dB over it, the voice alone. PluginTest checks the rows
// where the music is at least as loud as the voice; tests/MixVoiceEval.cpp prints the same lines and the whole table on any seed.
#include "VoiceTests.h"
#include "PassLimits.h"
#include <cstdio>
#include <string>
#include <vector>

namespace mixvoicetests
{
// Limits. Written on 2026-11-11 before any of these clips was run on any seed, and taken from what Key already claims for
// chords in PassLimits.h, not from what these clips showed. They hold in Source Full Mix, Response Stable, where the music
// is at least as loud as the voice: music alone, voice 6 dB under, voice as loud. A louder voice is reported, not checked.
// These clips are a minute long, the chord tests 24 s, so the two flicker limits are a little harder to meet here.
// Reported on limits::heldOutSeed + 31, run once.
namespace mixlimits
{
    constexpr double exactPercent = limits::chordExactPercent;          // clips that end on the exact key
    constexpr double relatedPercent = limits::chordRelatedPercent;      // exact, relative, or a fifth away in the same scale
    constexpr double medianSettle = limits::chordMedianSettleStable;    // seconds until the exact key is shown and kept to the end
    constexpr double movedPercent = limits::stableClipsWithAnyChange;   // clips whose reading moves after 8 s
    constexpr int bounces = limits::stableBounces;                      // A to B and back to A, over all clips
    // No chord limit says how long a wrong reading may be shown with confidence. The nearest claim is the one for rap: the
    // share of the time with a reading a follower would act on (confidence 0.5 or more) when it should not. Here: with the
    // wrong 7 notes.
    constexpr double wrongConfidentPercent = limits::rapConfidentPercent;
}

using smoothkey::KeyAnalyser;
using smoothkey::Response;
using smoothkey::Source;
using voicetests::notesOf;

// The chord clip of the chord tests (same seed, same tempo and style), but at A440 whatever the timbre: the singer is made
// at A440, and a singer follows his track. The two detuned timbres keep the spread between their own voices.
inline std::vector<float> musicClip (int tonic, const corpus::Progression& prog, const corpus::Timbre& t, unsigned seed, double sr, double seconds)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    const double chordLen = 1.5 + uni (rng);
    const int style = uni (rng) < 0.5 ? 0 : 1;
    std::vector<float> out ((size_t) (seconds * sr));
    corpus::renderNotes (out, corpus::chordNotes (tonic, prog, seconds, chordLen, style), t, 0.0, sr, seed + 1);
    return out;
}

inline double rmsOf (const std::vector<float>& x)
{
    double e = 0.0;
    for (auto v : x) e += (double) v * v;
    return std::sqrt (e / (double) std::max<size_t> (x.size(), 1));
}

// voiceDb: the level of the voice against the music, both measured as RMS over the whole clip. The sum peaks at 0.5.
inline std::vector<float> mixClip (const std::vector<float>& music, const std::vector<float>& voice, double voiceDb)
{
    const double gain = rmsOf (music) / std::max (rmsOf (voice), 1.0e-12) * std::pow (10.0, voiceDb / 20.0);
    std::vector<double> sum (music.size());
    double top = 1.0e-9;
    for (size_t i = 0; i < sum.size(); ++i)
    {
        sum[i] = music[i] + gain * (i < voice.size() ? voice[i] : 0.0f);
        top = std::max (top, std::abs (sum[i]));
    }
    std::vector<float> out (music.size());
    for (size_t i = 0; i < sum.size(); ++i) out[i] = (float) (sum[i] * 0.5 / top);
    return out;
}

inline std::vector<float> peakToHalf (std::vector<float> x)
{
    float top = 1.0e-9f;
    for (auto v : x) top = std::max (top, std::abs (v));
    for (auto& v : x) v *= 0.5f / top;
    return x;
}

struct Event { double t; int key; bool lone; float confidence; };

struct MixTrace
{
    int finalKey = -1;
    double settleNotes = -1.0, settleExact = -1.0;       // from when the right 7 notes (the exact key) are shown and kept to the end
    double rightConfident = 0.0, wrongConfident = 0.0;   // share of the clip at confidence 0.5 or more with the right, the wrong 7 notes
    int changes = 0;                                     // changes of reading after the first
    int movedAfterEight = 0, bounces = 0;                // as the chord tests count them
    double loneShare = 0.0, loneFrom = -1.0;             // share of the clip read as a lone voice, and when that began
    std::vector<Event> history;                          // every change of reading or of the lone voice flag
};

inline MixTrace trace (KeyAnalyser& a, const std::vector<float>& audio, double sr, int want)
{
    MixTrace tr;
    int last = -1, before = -1;
    bool lastLone = false;
    long n = 0, right = 0, wrong = 0, lone = 0;
    double sinceNotes = -1.0, sinceExact = -1.0;
    for (int pos = 0; pos < (int) audio.size(); pos += 512)
    {
        a.push (audio.data() + pos, std::min (512, (int) audio.size() - pos));
        const auto& r = a.reading();
        const int k = r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0);
        const double t = (pos + 512) / sr;
        const bool rightNotes = k >= 0 && notesOf (k) == notesOf (want);
        if (rightNotes) { if (sinceNotes < 0.0) sinceNotes = t; } else sinceNotes = -1.0;
        if (k == want) { if (sinceExact < 0.0) sinceExact = t; } else sinceExact = -1.0;
        if (k >= 0 && r.confidence >= 0.5f) { if (rightNotes) ++right; else ++wrong; }
        if (r.loneVoice) { ++lone; if (tr.loneFrom < 0.0) tr.loneFrom = t; }
        if (k != last || r.loneVoice != lastLone) tr.history.push_back ({ t, k, r.loneVoice, r.confidence });
        if (k != last)
        {
            if (last >= 0) { ++tr.changes; if (t > 8.0) ++tr.movedAfterEight; }
            if (k == before && last >= 0) ++tr.bounces;
            before = last;
            last = k;
        }
        lastLone = r.loneVoice;
        ++n;
        tr.finalKey = k;
    }
    tr.settleNotes = sinceNotes;
    tr.settleExact = sinceExact;
    const double frames = (double) std::max (n, 1L);
    tr.rightConfident = (double) right / frames; tr.wrongConfident = (double) wrong / frames; tr.loneShare = (double) lone / frames;
    return tr;
}

struct Row
{
    std::string label;      // what was played
    bool checked = false;   // the music is at least as loud as the voice, in Full Mix: the limits above apply
    int total = 0, exact = 0, rightNotes = 0, related = 0, moved = 0, bounces = 0, changed = 0, changes = 0;
    int lone = 0, loneRight = 0, plainRight = 0;         // clips read as a lone voice at some point, and right 7 notes at the end in each group
    double rightConfident = 0.0, wrongConfident = 0.0, loneShare = 0.0;
    std::vector<double> settleNotes, settleExact;        // 1e9 when not kept to the end

    void add (const MixTrace& t, int want)
    {
        ++total;
        const bool right = notesOf (t.finalKey) == notesOf (want) && t.finalKey >= 0;
        if (t.finalKey == want) ++exact;
        if (right) ++rightNotes;
        if (corpus::isRelated (t.finalKey, want)) ++related;
        if (t.movedAfterEight > 0) ++moved;
        if (t.changes > 0) ++changed;
        changes += t.changes;
        bounces += t.bounces;
        if (t.loneShare > 0.0) { ++lone; if (right) ++loneRight; } else if (right) ++plainRight;
        rightConfident += t.rightConfident; wrongConfident += t.wrongConfident; loneShare += t.loneShare;
        settleNotes.push_back (t.settleNotes >= 0.0 ? t.settleNotes : 1.0e9);
        settleExact.push_back (t.settleExact >= 0.0 ? t.settleExact : 1.0e9);
    }
    void merge (const Row& o)
    {
        total += o.total; exact += o.exact; rightNotes += o.rightNotes; related += o.related; moved += o.moved; bounces += o.bounces;
        changed += o.changed; changes += o.changes; lone += o.lone; loneRight += o.loneRight; plainRight += o.plainRight;
        rightConfident += o.rightConfident; wrongConfident += o.wrongConfident; loneShare += o.loneShare;
        settleNotes.insert (settleNotes.end(), o.settleNotes.begin(), o.settleNotes.end());
        settleExact.insert (settleExact.end(), o.settleExact.begin(), o.settleExact.end());
    }
    double of (double x) const { return total ? 100.0 * x / total : 0.0; }
    double exactPercent() const { return of (exact); }
    double rightPercent() const { return of (rightNotes); }
    double relatedPercent() const { return of (related); }
    double movedPercent() const { return of (moved); }
    double changedPercent() const { return of (changed); }
    double lonePercent() const { return of (lone); }
    double rightConfidentPercent() const { return of (rightConfident); }   // of all the time in all the clips
    double wrongConfidentPercent() const { return of (wrongConfident); }
    double loneSharePercent() const { return of (loneShare); }
    double changesPerClip() const { return total ? (double) changes / total : 0.0; }
    // -1: not kept to the end in half the clips or more
    static double shown (double seconds) { return seconds > 1.0e8 ? -1.0 : seconds; }
    double medianSettleNotes() const { return shown (voicetests::medianOf (settleNotes)); }
    double medianSettleExact() const { return shown (voicetests::medianOf (settleExact)); }
};

struct Options
{
    Source source = Source::mix;
    std::vector<double> levels { -6.0, 0.0, 6.0 };   // the voice against the music, dB
    bool musicAlone = true, voiceAlone = true;
    int tonicStep = 1;
    double seconds = 60.0;                           // as the lone voice tests, so the numbers compare
    std::FILE* log = nullptr;                        // when set: a line for every clip that went wrong in some way, with its readings
};

inline std::string levelLabel (double db)
{
    if (db == 0.0) return "music with the off key singer as loud as it";
    char text[96];
    std::snprintf (text, sizeof text, "music with the off key singer %g dB %s it", std::abs (db), db < 0.0 ? "under" : "over");
    return text;
}

// Every key, major and minor, each over the four progressions of its mode, the five timbres in turn: 96 clips of music.
// The singer on top is the one the lone voice test (voicetests::runSung) hears on the same seed, in the same key.
// Rows come back in this order: music alone, one for each level, the voice alone (24 clips).
inline std::vector<Row> runTable (KeyAnalyser& a, unsigned seed, const Options& o = {})
{
    const double sr = 48000.0;
    const auto ts = corpus::timbres();
    const auto ps = corpus::progressions();
    const std::string src = o.source == Source::voice ? ", Single Voice: " : ", Full Mix: ";
    std::vector<Row> rows;
    Row music, alone;
    music.label = "music alone" + src; music.checked = o.source == Source::mix;
    alone.label = "off key singer alone" + src;
    for (double db : o.levels) { Row r; r.label = levelLabel (db) + src; r.checked = o.source == Source::mix && db <= 0.0; rows.push_back (r); }

    const auto run = [&] (Row& row, const std::vector<float>& clip, int want, const char* what, double db)
    {
        a.reset();
        a.setResponse (Response::stable);
        a.setSource (o.source);
        const auto tr = trace (a, clip, sr, want);
        row.add (tr, want);
        if (o.log != nullptr && (tr.finalKey != want || tr.changes > 0 || tr.wrongConfident > 0.0 || (tr.loneShare > 0.0 && db < 100.0)))
        {
            std::fprintf (o.log, "  seed %u %s", seed, what);
            if (db > -100.0 && db < 100.0) std::fprintf (o.log, " voice %+g dB", db);
            std::fprintf (o.log, " want %d:", want);
            for (const auto& e : tr.history) std::fprintf (o.log, " %.1f=%d%s(%.2f)", e.t, e.key, e.lone ? "L" : "", e.confidence);
            std::fprintf (o.log, " | wrong and sure %.0f%% of the time, lone voice %.0f%%\n", 100.0 * tr.wrongConfident, 100.0 * tr.loneShare);
        }
    };

    for (int tonic = 0; tonic < 12; tonic += o.tonicStep)
        for (int minor = 0; minor < 2; ++minor)
        {
            const int want = corpus::keyIndex (tonic, minor == 1);
            const auto voice = corpus::sungClip (tonic, minor == 1, corpus::clipSeed (seed, tonic, minor, 5), sr, o.seconds);
            char what[160];
            if (o.voiceAlone)
            {
                std::snprintf (what, sizeof what, "singer alone, tonic %d %s", tonic, minor ? "minor" : "major");
                run (alone, voice, want, what, 1000.0);
            }
            for (int v = 0; v < 4; ++v)
            {
                const int p = minor * 4 + v;
                const int ti = (tonic + v + minor) % (int) ts.size();
                const auto clip = musicClip (tonic, ps[(size_t) p], ts[(size_t) ti], corpus::clipSeed (seed, tonic, p, ti), sr, o.seconds);
                std::snprintf (what, sizeof what, "tonic %d %s, %s, %s", tonic, minor ? "minor" : "major", ps[(size_t) p].name, ts[(size_t) ti].name);
                if (o.musicAlone) run (music, peakToHalf (clip), want, what, -1000.0);
                for (size_t i = 0; i < o.levels.size(); ++i) run (rows[i], mixClip (clip, voice, o.levels[i]), want, what, o.levels[i]);
            }
        }
    if (o.musicAlone) rows.insert (rows.begin(), music);
    if (o.voiceAlone) rows.push_back (alone);
    return rows;
}

// The lines for results.txt. check (ok, what, measured, unit) and info (what, value, unit) are TestKit's in PluginTest, and
// printf in MixVoiceEval, so both print the same thing.
template <typename Check, typename Info>
void report (const std::vector<Row>& rows, Check&& check, Info&& info)
{
    for (const auto& r : rows)
    {
        const auto line = [&] (bool ok, const std::string& what, double value, const char* unit)
        {
            if (r.checked) check (ok, r.label + what, value, unit); else info (r.label + what, value, unit);
        };
        info (r.label + "clips, every key, one minute each", r.total, "");
        line (r.exactPercent() >= mixlimits::exactPercent, "ends on the exact key", r.exactPercent(), "%");
        line (r.relatedPercent() >= mixlimits::relatedPercent, "ends on the exact key, its relative or a fifth away", r.relatedPercent(), "%");
        info (r.label + "ends on the right 7 notes", r.rightPercent(), "%");
        line (r.medianSettleExact() >= 0.0 && r.medianSettleExact() <= mixlimits::medianSettle, "median time until the exact key is shown and kept (-1: never, in half the clips)", r.medianSettleExact(), "s");
        info (r.label + "median time until the right 7 notes are shown and kept (-1: never, in half the clips)", r.medianSettleNotes(), "s");
        // Reported, not checked. The two flicker limits were first drafted here as checks, straight from the chord tests. Those
        // run 24 s clips; over these one minute clips the music ALONE, whose reading is bit identical to 1.5.0, already moves
        // after 8 s on 2.9% of clips (dev seeds 5000 to 5003, limit 1%). So as checks they would have judged 1.5.0 on long
        // clips, not a voice on top of music. Changed before the held out seed was run; the numbers are printed for the next pass.
        info (r.label + "clips whose reading moves after 8 s (chord tests, 24 s clips: limit 1)", r.movedPercent(), "%");
        info (r.label + "bounces, A to B and back to A (chord tests, 24 s clips: limit 0)", r.bounces, "");
        line (r.wrongConfidentPercent() <= mixlimits::wrongConfidentPercent, "share of the time at confidence 0.5 or more on the wrong 7 notes", r.wrongConfidentPercent(), "%");
        info (r.label + "share of the time at confidence 0.5 or more on the right 7 notes", r.rightConfidentPercent(), "%");
        info (r.label + "changes of reading after the first, a clip", r.changesPerClip(), "");
        info (r.label + "clips read as a lone voice at some point", r.lonePercent(), "%");
    }
}
} // namespace mixvoicetests
