#pragma once
// Slicing with no JUCE and no audio thread rules broken: analyse() runs once per sample on a background thread and may
// allocate. pickTransients(), gridSlices() and isValid() allocate nothing and are bounded, so the audio thread can call them.
#include <algorithm>
#include <cmath>
#include <vector>

namespace slicecore
{
    constexpr int kMaxSlots = 32;
    constexpr int kMaxCandidates = 2048;

    struct Candidate { int pos = 0; float strength = 0.0f; };

    // Slice i plays start[i] up to start[i + 1]. start[count] is the end of the last slice. Slices never overlap.
    struct SliceTable
    {
        int count = 0;
        int start[kMaxSlots + 1] {};

        bool operator== (const SliceTable& o) const noexcept
        {
            if (count != o.count) return false;
            for (int i = 0; i <= count && count > 0; ++i) if (start[i] != o.start[i]) return false;
            return true;
        }
        bool operator!= (const SliceTable& o) const noexcept { return ! (*this == o); }
    };

    struct Analysis
    {
        std::vector<Candidate> onsets; // strongest first
        int firstAudible = 0;          // first frame above -40 dB re the sample's peak
    };

    inline bool isValid (const SliceTable& t, int frames) noexcept
    {
        if (frames <= 0) return t.count == 0;
        if (t.count < 1 || t.count > kMaxSlots || t.start[t.count] != frames || t.start[0] < 0) return false;
        for (int i = 0; i < t.count; ++i) if (t.start[i] >= t.start[i + 1]) return false;
        return true;
    }

    inline SliceTable wholeSample (int frames) noexcept
    {
        SliceTable t;
        if (frames > 0) { t.count = 1; t.start[0] = 0; t.start[1] = frames; }
        return t;
    }

    // Exact: boundary i is floor (i * frames / parts), computed in 64 bits.
    inline SliceTable gridSlices (int frames, int parts) noexcept
    {
        SliceTable t;
        if (frames <= 0) return t;
        parts = std::max (1, std::min ({ parts, kMaxSlots, frames }));
        t.count = parts;
        for (int i = 0; i <= parts; ++i) t.start[i] = (int) ((long long) i * frames / parts);
        return t;
    }

    // Sensitivity 0 to 100 sets how weak an onset may be and still cut. 100 takes every candidate, 0 only the strongest.
    inline float thresholdFor (float sensitivity) noexcept
    {
        const float a = 1.0f - std::min (1.0f, std::max (0.0f, sensitivity * 0.01f));
        return a * a;
    }

    // Takes candidates strongest first, so a limit of 32 slices keeps the 32 strongest and Min Slice keeps the stronger of two
    // close onsets. At most kMaxCandidates * kMaxSlots simple steps. A loud lead-in before the first cut becomes its own slice,
    // and silence before the first cut is dropped.
    inline SliceTable pickTransients (const Candidate* c, int numCandidates, int firstAudible, int frames, float sensitivity, int minGapFrames) noexcept
    {
        SliceTable t;
        if (frames <= 0) return t;

        const float thr = thresholdFor (sensitivity);
        const int gap = std::max (1, minGapFrames);
        int cuts[kMaxSlots];
        int num = 0;
        const int leadRoom = std::min (gap, frames / 2); // room kept for a lead-in, decided after the first pass

        for (int i = 0; i < numCandidates && num < kMaxSlots; ++i)
        {
            if (c[i].strength < thr) break;
            const int pos = c[i].pos;
            if (pos < 0 || pos >= frames) continue;
            if (frames - pos < std::min (gap, 64)) continue;
            bool clear = true;
            for (int k = 0; k < num && clear; ++k) clear = std::abs (cuts[k] - pos) >= gap;
            if (clear) cuts[num++] = pos;
        }

        std::sort (cuts, cuts + num);
        if (num == 0) return wholeSample (frames);

        // Sound before the first cut stays playable. Only silence is dropped.
        const int margin = std::max (1, std::min (leadRoom, 64));
        const bool leadIn = cuts[0] > 0 && firstAudible + margin < cuts[0];
        int out = 0;
        if (leadIn) t.start[out++] = 0;
        for (int i = 0; i < num && out < kMaxSlots; ++i) t.start[out++] = cuts[i];
        t.count = out;
        t.start[out] = frames;
        return t;
    }

    // Finds where sounds begin. Coarse stage: energy after a point against energy before it, in 1 ms steps, broad and high passed,
    // so that both a vowel and a consonant show. Fine stage: walks back from there to where the sound rises out of what came before.
    inline Analysis analyse (const float* x, int frames, double rate)
    {
        Analysis result;
        if (x == nullptr || frames <= 0 || ! (rate > 0.0)) return result;

        const int hop = std::max (1, (int) std::lround (rate * 0.001));
        const int numHops = frames / hop + 1;
        const auto ms = [rate] (double m) { return (int) std::lround (m * 0.001 * rate); };

        double peak = 0.0;
        for (int i = 0; i < frames; ++i) peak = std::max (peak, (double) std::abs (x[i]));
        if (peak <= 1.0e-6) { result.firstAudible = frames; return result; }

        result.firstAudible = frames;
        for (int i = 0; i < frames; ++i) if (std::abs (x[i]) >= 0.01 * peak) { result.firstAudible = i; break; }

        std::vector<double> sumBroad ((size_t) numHops + 1, 0.0), sumHigh ((size_t) numHops + 1, 0.0);
        for (int k = 0; k < numHops; ++k)
        {
            double b = 0.0, h = 0.0;
            const int from = k * hop, to = std::min (frames, from + hop);
            for (int i = from; i < to; ++i)
            {
                b += (double) x[i] * x[i];
                const double diff = (double) x[i] - (i > 0 ? (double) x[i - 1] : 0.0);
                h += diff * diff;
            }
            sumBroad[(size_t) k + 1] = sumBroad[(size_t) k] + b;
            sumHigh[(size_t) k + 1] = sumHigh[(size_t) k] + h;
        }
        const double highScale = sumHigh[(size_t) numHops] > 0.0 ? sumBroad[(size_t) numHops] / sumHigh[(size_t) numHops] : 1.0;

        const auto mean = [numHops] (const std::vector<double>& s, int a, int b)
        {
            a = std::max (0, a); b = std::min (numHops, b);
            return b > a ? (s[(size_t) b] - s[(size_t) a]) / (double) (b - a) : 0.0;
        };

        constexpr int afterHops = 10, beforeHops = 30, spacingHops = 12;
        double loudest = 1.0e-12;
        for (int k = 0; k < numHops; ++k) loudest = std::max (loudest, mean (sumBroad, k, k + afterHops));
        const double floorE = loudest * 1.0e-4; // -40 dB re the loudest 10 ms

        std::vector<float> rise ((size_t) numHops, 0.0f);
        for (int k = 0; k < numHops; ++k)
        {
            const double broad = 10.0 * std::log10 ((mean (sumBroad, k, k + afterHops) + floorE) / (mean (sumBroad, k - beforeHops, k) + floorE));
            const double high = 10.0 * std::log10 ((highScale * mean (sumHigh, k, k + afterHops) + floorE) / (highScale * mean (sumHigh, k - beforeHops, k) + floorE));
            rise[(size_t) k] = (float) std::max (broad, 0.8 * high);
        }

        std::vector<Candidate> found;
        for (int k = 0; k < numHops; ++k)
        {
            const float d = rise[(size_t) k];
            if (d < 3.0f) continue;
            bool isPeak = true;
            for (int j = std::max (0, k - spacingHops); j <= std::min (numHops - 1, k + spacingHops) && isPeak; ++j)
                if (rise[(size_t) j] > d || (rise[(size_t) j] == d && j < k)) isPeak = false;
            if (! isPeak) continue;

            const int coarse = std::min (frames - 1, k * hop);

            // Level of what follows and of what came before, as peaks.
            const auto peakIn = [&] (int a, int b)
            {
                a = std::max (0, a); b = std::min (frames, b);
                float m = 0.0f;
                for (int i = a; i < b; ++i) m = std::max (m, std::abs (x[i]));
                return m;
            };
            const float following = peakIn (coarse, coarse + ms (200.0));
            const float before = peakIn (coarse - ms (110.0), coarse - ms (80.0));
            const float thresh = std::max (0.01f * following, 1.6f * before);

            // Walk back from the coarse point to the last place where 5 ms in a row stay under the threshold, so a zero crossing
            // of a low voice does not stop the walk, and a soft H or P before the vowel is kept. Over a loud sound the threshold
            // is above the sound, and the coarse point stands, because it is already exact there.
            int pos = coarse;
            if (following > 2.0f * thresh)
            {
                const int hold = std::max (1, ms (5.0));
                const int back = std::max (0, coarse - ms (80.0));
                const int ahead = std::min (frames - 1, coarse + ms (4.0));
                int i = coarse;
                while (i < ahead && std::abs (x[i]) < thresh) ++i;
                while (i > back)
                {
                    int quietRun = 0, j = i - 1;
                    while (j >= back && quietRun < hold && std::abs (x[j]) < thresh) { ++quietRun; --j; }
                    if (quietRun >= hold) break;
                    if (j < back) { i = coarse; break; }
                    i = j;
                }
                pos = std::min (i, ahead);
            }
            if (pos < ms (2.0)) pos = 0;

            const double level = mean (sumBroad, k, k + afterHops) / loudest;
            const float strength = (float) (std::sqrt (std::min (1.0, (double) d / 24.0)) * (0.3 + 0.7 * std::pow (level, 0.2)));
            found.push_back ({ pos, strength });
        }

        // Strongest first, positions kept 5 ms apart, list capped, strongest scaled to 1.
        std::stable_sort (found.begin(), found.end(), [] (const Candidate& a, const Candidate& b) { return a.strength > b.strength; });
        const float top = found.empty() ? 1.0f : std::max (1.0e-6f, found.front().strength);
        const int minSpace = std::max (1, ms (5.0));
        for (const auto& c : found)
        {
            if ((int) result.onsets.size() >= kMaxCandidates) break;
            bool clear = true;
            for (const auto& kept : result.onsets) if (std::abs (kept.pos - c.pos) < minSpace) { clear = false; break; }
            if (clear) result.onsets.push_back ({ c.pos, std::round (c.strength / top * 65535.0f) / 65535.0f }); // exact when saved as a whole number
        }
        return result;
    }
}
