#pragma once
// A lone voice: made up off key singing and rap (KeyCorpus.h) through the analyser. PluginTest checks the results against
// PassLimits.h; tests/VoiceEval.cpp prints them while developing. What counts for a tuner is the 7 notes, so a key and its
// relative are the same answer here.
#include "KeyCorpus.h"

namespace voicetests
{
using smoothkey::KeyAnalyser;
using smoothkey::Response;
using smoothkey::Source;

inline int notesOf (int key) { return key < 0 ? -1 : (key < 12 ? key : (key - 12 + 3) % 12); } // as the major key with the same 7 notes

struct VoiceTrace
{
    int finalKey = -1;
    double settle = -1.0;         // from when the right 7 notes are shown and kept to the end
    double firstRight = -1.0;
    int changes = 0, noteChanges = 0; // after the first reading: every change, and changes of the 7 notes
    bool everRead = false;
    double meanConfidence = 0.0, finalHz = 440.0;
};

// from: only what happens after this many seconds counts for firstRight
inline VoiceTrace trace (KeyAnalyser& a, const std::vector<float>& audio, double sr, int want, double from = 0.0)
{
    VoiceTrace tr;
    int last = -1;
    long n = 0;
    double since = -1.0, conf = 0.0;
    for (int pos = 0; pos < (int) audio.size(); pos += 512)
    {
        a.push (audio.data() + pos, std::min (512, (int) audio.size() - pos));
        const auto& r = a.reading();
        const int k = r.key < 0 ? -1 : r.key + (r.minor ? 12 : 0);
        const double t = (pos + 512) / sr;
        if (k >= 0 && notesOf (k) == notesOf (want))
        {
            if (since < 0.0) since = t;
            if (tr.firstRight < 0.0 && t >= from) tr.firstRight = t;
        }
        else since = -1.0;
        if (k != last)
        {
            if (last >= 0) { ++tr.changes; if (notesOf (k) != notesOf (last)) ++tr.noteChanges; }
            last = k;
        }
        if (k >= 0) tr.everRead = true;
        conf += r.confidence; ++n;
        tr.finalKey = k;
        tr.finalHz = 440.0 * std::pow (2.0, r.tuningCents / 1200.0);
    }
    tr.settle = since;
    tr.meanConfidence = n > 0 ? conf / n : 0.0;
    return tr;
}

inline double medianOf (std::vector<double> v) { if (v.empty()) return -1.0; std::sort (v.begin(), v.end()); return v[v.size() / 2]; }

struct SungResult
{
    int total = 0, rightNotes = 0, exact = 0, moving = 0, referenceNear = 0;
    std::vector<double> settles; // 1e9 when the right notes were not kept to the end
    double rightPercent() const { return total ? 100.0 * rightNotes / total : 0.0; }
    double exactPercent() const { return total ? 100.0 * exact / total : 0.0; }
    double movingPercent() const { return total ? 100.0 * moving / total : 0.0; }
    double referencePercent() const { return total ? 100.0 * referenceNear / total : 0.0; }
    double medianSettle() const { return medianOf (settles); }
};

// Every key, major and minor, one minute each.
inline SungResult runSung (KeyAnalyser& a, Source source, unsigned seed, double referenceHz, int tonicStep = 1, double seconds = 60.0)
{
    SungResult res;
    const double sr = 48000.0;
    for (int tonic = 0; tonic < 12; tonic += tonicStep)
        for (int minor = 0; minor < 2; ++minor)
        {
            a.reset();
            a.setResponse (Response::stable);
            a.setSource (source);
            const int want = corpus::keyIndex (tonic, minor == 1);
            const auto tr = trace (a, corpus::sungClip (tonic, minor == 1, corpus::clipSeed (seed, tonic, minor, 5), sr, seconds), sr, want);
            ++res.total;
            if (notesOf (tr.finalKey) == notesOf (want)) ++res.rightNotes;
            if (tr.finalKey == want) ++res.exact;
            if (tr.noteChanges > 1) ++res.moving;
            if (std::abs (tr.finalHz - 440.0) <= referenceHz) ++res.referenceNear;
            res.settles.push_back (tr.settle >= 0.0 ? tr.settle : 1.0e9);
        }
    return res;
}

struct RapResult
{
    int total = 0, steady = 0, neverRead = 0;
    double confidence = 0.0;
    double steadyPercent() const { return total ? 100.0 * steady / total : 0.0; }
    double meanConfidence() const { return total ? confidence / total : 0.0; }
};

inline RapResult runRap (KeyAnalyser& a, Source source, unsigned seed, int clips = 12, double seconds = 60.0)
{
    RapResult res;
    const double sr = 48000.0;
    for (int i = 0; i < clips; ++i)
    {
        a.reset();
        a.setResponse (Response::stable);
        a.setSource (source);
        const auto tr = trace (a, corpus::rapClip (corpus::clipSeed (seed, i, 0, 9), sr, seconds), sr, 0);
        ++res.total;
        if (tr.changes <= (int) std::ceil (seconds / 60.0)) ++res.steady;
        if (! tr.everRead) ++res.neverRead;
        res.confidence += tr.meanConfidence;
    }
    return res;
}

struct StepResult
{
    int total = 0, followed = 0, endRight = 0;
    std::vector<double> times;
    double followedPercent() const { return total ? 100.0 * followed / total : 0.0; }
    double endRightPercent() const { return total ? 100.0 * endRight / total : 0.0; }
};

// The same singer goes up a semitone half way, as a last chorus often does.
inline StepResult runStepUp (KeyAnalyser& a, unsigned seed, int tonicStep = 1, double half = 45.0)
{
    StepResult res;
    const double sr = 48000.0;
    for (int tonic = 0; tonic < 12; tonic += tonicStep)
        for (int minor = 0; minor < 2; ++minor)
        {
            auto clip = corpus::sungClip (tonic, minor == 1, corpus::clipSeed (seed, tonic, minor, 6), sr, half);
            const auto second = corpus::sungClip ((tonic + 1) % 12, minor == 1, corpus::clipSeed (seed, tonic, minor, 7), sr, half);
            clip.insert (clip.end(), second.begin(), second.end());
            a.reset();
            a.setResponse (Response::stable);
            a.setSource (Source::voice);
            const int want = corpus::keyIndex ((tonic + 1) % 12, minor == 1);
            const auto tr = trace (a, clip, sr, want, half);
            ++res.total;
            if (tr.firstRight >= 0.0 && tr.firstRight - half <= 30.0) { ++res.followed; res.times.push_back (tr.firstRight - half); }
            if (notesOf (tr.finalKey) == notesOf (want)) ++res.endRight;
        }
    return res;
}
} // namespace voicetests
