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

    // Exact: boundary i is from + floor (i * length / parts), computed in 64 bits, where length runs from the Slice Start to the end.
    inline SliceTable gridSlices (int frames, int parts, int from = 0) noexcept
    {
        SliceTable t;
        if (frames <= 0) return t;
        from = std::max (0, std::min (from, frames - 1));
        const int length = frames - from;
        parts = std::max (1, std::min ({ parts, kMaxSlots, length }));
        t.count = parts;
        for (int i = 0; i <= parts; ++i) t.start[i] = from + (int) ((long long) i * length / parts);
        return t;
    }

    // Cuts every stepFrames from the Slice Start. Stops at the 32 slots, and the last slice runs on to the end of the sample.
    inline SliceTable noteSlices (int frames, double stepFrames, int from = 0) noexcept
    {
        SliceTable t;
        if (frames <= 0) return t;
        from = std::max (0, std::min (from, frames - 1));
        stepFrames = std::max (1.0, stepFrames);
        int count = 0;
        for (int i = 0; i < kMaxSlots; ++i)
        {
            const double at = (double) from + (double) i * stepFrames;
            if (at >= (double) frames) break;
            t.start[count++] = (int) std::llround (at);
        }
        // Rounding can land a last cut on the end frame.
        while (count > 1 && t.start[count - 1] >= frames) --count;
        t.count = count;
        t.start[count] = frames;
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
    inline SliceTable pickTransients (const Candidate* c, int numCandidates, int firstAudible, int frames, float sensitivity, int minGapFrames, int from = 0) noexcept
    {
        SliceTable t;
        if (frames <= 0) return t;
        from = std::max (0, std::min (from, frames - 1));

        const float thr = thresholdFor (sensitivity);
        const int gap = std::max (1, minGapFrames);
        int cuts[kMaxSlots];
        int num = 0;
        const int leadRoom = std::min (gap, frames / 2); // room kept for a lead-in, decided after the first pass

        for (int i = 0; i < numCandidates && num < kMaxSlots; ++i)
        {
            if (c[i].strength < thr) break;
            const int pos = c[i].pos;
            if (pos < from || pos >= frames) continue;
            if (frames - pos < std::min (gap, 64)) continue;
            bool clear = true;
            for (int k = 0; k < num && clear; ++k) clear = std::abs (cuts[k] - pos) >= gap;
            if (clear) cuts[num++] = pos;
        }

        std::sort (cuts, cuts + num);
        if (num == 0)
        {
            t.count = 1;
            t.start[0] = from;
            t.start[1] = frames;
            return t;
        }

        // Sound before the first cut stays playable. Only silence is dropped.
        const int margin = std::max (1, std::min (leadRoom, 64));
        const bool leadIn = cuts[0] > from && std::max (firstAudible, from) + margin < cuts[0];
        int out = 0;
        if (leadIn) t.start[out++] = from;
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

        // Energy in short blocks (0.25 ms), summed, so the level of any window on that grid is two lookups. The high band is the
        // sample to sample difference, scaled to the broad level over the file, as above.
        const int blk = std::max (1, (int) std::lround (rate * 0.00025));
        const int numBlocks = frames / blk;
        std::vector<double> cumE ((size_t) numBlocks + 1, 0.0), cumD ((size_t) numBlocks + 1, 0.0);
        for (int q = 0; q < numBlocks; ++q)
        {
            double e = 0.0, h = 0.0;
            for (int i = q * blk; i < (q + 1) * blk; ++i)
            {
                e += (double) x[i] * x[i];
                const double diff = (double) x[i] - (i > 0 ? (double) x[i - 1] : 0.0);
                h += diff * diff;
            }
            cumE[(size_t) q + 1] = cumE[(size_t) q] + e;
            cumD[(size_t) q + 1] = cumD[(size_t) q] + h;
        }
        const auto inBlocks = [rate, blk] (double m) { return std::max (1, (int) std::lround (m * 0.001 * rate / blk)); };
        const int b1 = inBlocks (1.0), b2 = inBlocks (2.0), b10 = inBlocks (10.0);
        const double dScale = std::sqrt (highScale);
        // RMS over blocks [a, a + n), the part inside the file
        const auto lvl = [&] (const std::vector<double>& cum, int a, int n)
        {
            const int from = std::max (0, a), to = std::min (numBlocks, a + n);
            return to > from ? std::sqrt (std::max (0.0, cum[(size_t) to] - cum[(size_t) from]) / (double) ((to - from) * blk)) : 0.0;
        };

        // The room: the 10th percentile of the 10 ms levels. Zero for a file with digital silence in it. A real recording never goes
        // under its room, so sound has to stand out of it to count.
        double room = 0.0;
        {
            std::vector<double> levels;
            for (int q = 0; q + b10 <= numBlocks; q += b10) levels.push_back (lvl (cumE, q, b10));
            if (! levels.empty())
            {
                // In 20 s of dense singing the 10th percentile is the quietest singing, not the room, so it is held to 9 dB over the
                // 2nd percentile.
                const auto tenth = levels.begin() + (long) (levels.size() / 10);
                std::nth_element (levels.begin(), tenth, levels.end());
                room = *tenth;
                const auto second = levels.begin() + (long) (levels.size() / 50);
                std::nth_element (levels.begin(), second, levels.end());
                room = std::min (room, 2.8 * *second);
            }
        }

        // S frames, 10 ms each: 60% or more of the energy above 4 kHz, and that part 15 dB or more over its own floor and above -60 dBFS.
        // A run of two or more is an S. A cut inside one would start a slice in the middle of a hiss.
        const int w10 = std::max (1, ms (10.0));
        std::vector<unsigned char> sFrame ((size_t) (frames / w10 + 1), 0);
        {
            const double w = 2.0 * 3.14159265358979323846 * std::min (4000.0, 0.45 * rate) / rate, cw = std::cos (w), alpha = std::sin (w) / (2.0 * 0.707), a0 = 1.0 + alpha;
            const double b0 = (1.0 + cw) * 0.5 / a0, b1 = -(1.0 + cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
            double z1 = 0.0, z2 = 0.0;
            std::vector<double> hfE, allE;
            double eh = 0.0, et = 0.0;
            for (int i = 0; i < frames; ++i)
            {
                const double v = x[i], y = b0 * v + z1;
                z1 = b1 * v - a1 * y + z2;
                z2 = b2 * v - a2 * y;
                eh += y * y; et += v * v;
                if ((i + 1) % w10 == 0) { hfE.push_back (eh); allE.push_back (et); eh = et = 0.0; }
            }
            if (! hfE.empty())
            {
                std::vector<double> sorted (hfE);
                // The floor of the part above 4 kHz: its 10th percentile, held to 6 dB over its 2nd percentile, as for the room below,
                // so dense singing does not lift it over the start of a soft S.
                const auto tenth = sorted.begin() + (long) (sorted.size() / 10);
                std::nth_element (sorted.begin(), tenth, sorted.end());
                double hfFloor = *tenth;
                const auto second = sorted.begin() + (long) (sorted.size() / 50);
                std::nth_element (sorted.begin(), second, sorted.end());
                hfFloor = std::min (hfFloor, 4.0 * *second);
                const double sMinE = std::max (hfFloor * 31.6227766, 1.0e-6 * (double) w10);
                for (size_t f = 0; f < hfE.size() && f < sFrame.size(); ++f)
                    sFrame[f] = hfE[f] >= sMinE && allE[f] > 0.0 && hfE[f] >= 0.6 * allE[f] ? 1 : 0;
            }
        }
        // Where the S that holds position p starts, or -1 when p is not more than 10 ms inside a run of two or more S frames (frames are on a 10 ms grid)
        const auto sStartAround = [&] (int p)
        {
            const int f = p / w10;
            if (f < 0 || f >= (int) sFrame.size() || ! sFrame[(size_t) f]) return -1;
            int a = f, b = f;
            while (a > 0 && sFrame[(size_t) a - 1]) --a;
            while (b + 1 < (int) sFrame.size() && sFrame[(size_t) b + 1]) ++b;
            if (b - a + 1 < 2 || p <= a * w10 + ms (10.0)) return -1;
            return a * w10;
        };

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
            const float before = peakIn (coarse - ms (190.0), coarse - ms (160.0));
            const float thresh = std::max (0.01f * following, 1.6f * before);

            // Walk back from the coarse point to the last place where 10 ms in a row stay under the threshold, so a zero crossing
            // of a low voice does not stop the walk, and a soft H, TH or P before the vowel is kept. Over a loud sound the threshold
            // is above the sound, and the coarse point stands, because it is already exact there.
            int pos = coarse;
            if (following > 2.0f * thresh)
            {
                const int hold = std::max (1, ms (10.0));
                const int back = std::max (0, coarse - ms (160.0));
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
            // Then the cut is held against the sound of its own slice, the way it is heard. ref is the loudest 10 ms of the 200 ms
            // after the cut; 30 dB under it is quiet, 20 dB under it is sound. A clean cut:
            // - rises: the loudest 10 ms of the next 50 ms is 3.5 dB or more over the 30 ms before, broad or high band;
            // - leaves nothing off: where the level was quiet in the 160 ms before the cut and then came up to sound, the cut is no
            //   more than 3 ms after that (a soft S, F, H or breath before the vowel belongs to the slice);
            // - starts on sound: within 12 ms the level passes 40 dB under ref and 6 dB over the room;
            // - is not more than 10 ms inside an S.
            // A cut that is not clean moves to the nearest clean place around where its sound begins. Where there is none, it is
            // not an onset that can be cut cleanly, and it is left out. Positions on the block grid, 0.25 ms.
            const auto refAt = [&] (int t)
            {
                double r = 0.0;
                for (int q = 0; q < 20; ++q) r = std::max (r, lvl (cumE, t + q * b10, b10));
                return r;
            };
            const auto riseAt = [&] (int t)
            {
                double after = 0.0, afterH = 0.0;
                for (int q = 0; q < 5; ++q) { after = std::max (after, lvl (cumE, t + q * b10, b10)); afterH = std::max (afterH, dScale * lvl (cumD, t + q * b10, b10)); }
                const double fl = std::max (2.0 * room, 1.0e-7); // the room counted 6 dB high, so a rise is never read from noise
                const double bb = lvl (cumE, t - 3 * b10, 3 * b10), bh = dScale * lvl (cumD, t - 3 * b10, 3 * b10);
                return 20.0 * std::log10 (std::max ((after + fl) / (bb + fl), (afterH + fl) / (bh + fl)));
            };
            // Where the sound this cut leaves off came up (-1 when nothing is left off), against the cut's own ref
            const auto leftOffFrom = [&] (int t, double r)
            {
                int quiet = -1;
                for (int u = t - inBlocks (160.0); u < t; u += b1)
                    if (u >= 0 && lvl (cumE, u, b2) < 0.0316 * r) quiet = u;
                if (quiet < 0) return -1;
                for (int u = quiet; u < t; u += b1)
                    if (lvl (cumE, u, b2) >= 0.1 * r) return t - u > inBlocks (3.0) ? u : -1;
                return -1;
            };
            const auto deadLead = [&] (int t, double r)
            {
                const double audible = std::max (0.01 * r, 2.0 * room);
                for (int u = t; u < t + inBlocks (200.0); u += b1)
                    if (lvl (cumE, u, b2) >= audible) return u - t;
                return 1 << 30;
            };
            const auto clean = [&] (int t)
            {
                if (t < 0 || t >= numBlocks) return false;
                const double r = refAt (t);
                return r > 0.0 && riseAt (t) >= 3.5 && leftOffFrom (t, r) < 0 && deadLead (t, r) <= inBlocks (12.0) && sStartAround (t * blk) < 0;
            };
            // The cut goes onto the block grid (at most 0.25 ms earlier), so it is exactly where it was judged.
            const int t0 = pos / blk;
            if (pos >= ms (2.0)) pos = t0 * blk;
            if (pos >= ms (2.0) && ! clean (t0))
            {
                const double r = refAt (t0);
                int ideal = t0;
                const int from = leftOffFrom (t0, r), sAt = sStartAround (t0 * blk), lead = deadLead (t0, r);
                if (from >= 0) ideal = from - b1;
                else if (sAt >= 0)
                {
                    // the S frame starts on the 10 ms grid; the S itself starts where it comes up to 30 dB under the slice
                    ideal = sAt / blk;
                    while (ideal < t0 && lvl (cumE, ideal, b2) < std::max (0.0316 * r, 2.0 * room)) ideal += b1;
                    ideal -= b1;
                }
                else if (lead < (1 << 30)) ideal = t0 + lead - b1;
                const int lo = std::min (ideal, t0) - inBlocks (20.0), hi = std::max (ideal, t0) + inBlocks (20.0);
                int best = -1;
                for (int dist = 0; best < 0 && (ideal - dist >= lo || ideal + dist <= hi); ++dist)
                {
                    if (ideal - dist >= lo && clean (ideal - dist)) best = ideal - dist;
                    else if (dist > 0 && ideal + dist <= hi && clean (ideal + dist)) best = ideal + dist;
                }
                if (best < 0) continue;
                pos = best * blk;
            }
            if (pos < ms (2.0)) pos = 0;

            // How loud the sound this onset begins gets, so that a soft S or H leading into a vowel counts as the start it is.
            double ahead = 0.0;
            for (int j = k; j < std::min (numHops, k + 200); j += afterHops)
                ahead = std::max (ahead, mean (sumBroad, j, j + afterHops));
            const double level = ahead / loudest;
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
