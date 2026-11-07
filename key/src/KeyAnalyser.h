#pragma once
// Key and scale detection. No JUCE and no threads in here, so it can be tested directly.
// Everything is allocated in the constructor. push() and idle() never allocate.
//
// Steps, once per hop (about 0.17 s):
//   1. Low-pass and decimate to about 11 to 12 kHz, whatever the host rate is.
//   2. Hann window of 8192 samples (about 0.7 s), own FFT.
//   3. Estimate how far the music sits from A440 from the spectral peaks, so a detuned recording still lands on semitones.
//   4. Map the spectrum to three bins per semitone and remove the local floor, which drops most noise.
//   5. Solve for which notes explain the spectrum, each note being a stack of harmonics (non-negative least squares).
//      This removes most of the false fifths and thirds that a note's own harmonics would add to a plain chromagram.
//   6. Fold notes into a treble chroma and a bass chroma, kept in a leaky memory.
//   7. Score all 24 keys: treble against a key profile, bass against a profile that favours tonic and fifth, and a small
//      tie break for the opening chord, because Am F C G and C G Am F contain exactly the same notes in the same amounts.
//   8. Only move the reading when a new key has led for a set time by a set margin, so it cannot flicker.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace smoothkey
{
inline constexpr const char* keyNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

enum class Response { fast = 0, stable = 1 };
enum class Source { mix = 0, voice = 1 };

// Radix 2 complex FFT, written here rather than taken from a library. Tables are built once.
class Fft
{
public:
    explicit Fft (int size) : n (size), cosTable ((size_t) size / 2), sinTable ((size_t) size / 2), reversed ((size_t) size)
    {
        const double twoPi = 6.283185307179586476925;
        for (int i = 0; i < n / 2; ++i)
        {
            cosTable[(size_t) i] = std::cos (twoPi * i / n);
            sinTable[(size_t) i] = -std::sin (twoPi * i / n);
        }
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reversed[(size_t) i] = r;
        }
    }

    void forward (double* re, double* im) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int r = reversed[(size_t) i];
            if (r > i) { std::swap (re[i], re[r]); std::swap (im[i], im[r]); }
        }
        for (int size = 2; size <= n; size *= 2)
        {
            const int half = size / 2, step = n / size;
            for (int start = 0; start < n; start += size)
                for (int j = 0; j < half; ++j)
                {
                    const double c = cosTable[(size_t) (j * step)], s = sinTable[(size_t) (j * step)];
                    const int a = start + j, b = a + half;
                    const double tr = re[b] * c - im[b] * s, ti = re[b] * s + im[b] * c;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
        }
    }

private:
    int n;
    std::vector<double> cosTable, sinTable;
    std::vector<int> reversed;
};

struct Reading
{
    int key = -1;                 // 0 to 11 from C, or -1 when no key has been established
    bool minor = false;
    float confidence = 0.0f;      // 0 to 1
    int altKey = -1;              // the runner up
    bool altMinor = false;
    bool signal = false;          // audio above the silence gate in this frame
    float tuningCents = 0.0f;     // how far the music sits from A440
    std::array<float, 12> chroma {};
};

class KeyAnalyser
{
public:
    // Settings a build or a test can vary. The defaults are what ships.
    struct Knobs
    {
        double tauStable = 22.0, tauFast = 7.0;         // memory of the chroma, seconds
        double dwellStable = 4.0, dwellFast = 1.0;      // a new key must lead this long
        double marginStable = 0.04, marginFast = 0.015; // and by this much score
        double firstStable = 6.0, firstFast = 2.0;      // earliest first reading, seconds of audio
        double settleSeconds = 12.0;                    // the dwell and margin reach full size after this much audio
        double bassWeight = 0.45;
        double openingWeight = 0.4;
        double relativeGuard = 2.0;
        double openingSeconds = 1.5;
        double openingTau = 60.0;
        double harmonicDecay = 0.65;
        double chromaPower = 0.75;      // compression of the long term chroma
        double softness = 14.0;         // turns score gaps into a probability
        int iterations = 30;
    };

    KeyAnalyser()
        : fft (fftSize), window ((size_t) fftSize), ring ((size_t) fftSize), re ((size_t) fftSize), im ((size_t) fftSize),
          mag ((size_t) fftSize / 2 + 1), firTaps ((size_t) maxTaps), firHistory ((size_t) maxTaps * 2),
          logFreqs ((size_t) numLog), logSpec ((size_t) numLog), logClean ((size_t) numLog), notes ((size_t) numNotes),
          wtv ((size_t) numNotes), wtw ((size_t) (numNotes * numNotes)), dict ((size_t) (numNotes * numLog))
    {
        const double twoPi = 6.283185307179586476925;
        hannSum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / fftSize);
            hannSum += window[(size_t) i];
        }
        for (int i = 0; i < numLog; ++i)
            logFreqs[(size_t) i] = 440.0 * std::pow (2.0, ((lowestNote + i / (double) binsPerNote) - 69.0) / 12.0);
        buildProfiles();
        buildDictionary();
        configure (48000.0);
    }

    void setKnobs (const Knobs& k) noexcept { knobs = k; buildDictionary(); }

    void configure (double sampleRate) noexcept
    {
        hostRate = sampleRate > 1000.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
        decimation = std::clamp ((int) std::lround (hostRate / 11500.0), 1, maxDecimation);
        outRate = hostRate / decimation;
        numTaps = decimation * 32 + 1;
        const double twoPi = 6.283185307179586476925, pi = twoPi * 0.5, cutoff = 0.45 / decimation;
        double sum = 0.0;
        for (int i = 0; i < numTaps; ++i)
        {
            const double x = i - (numTaps - 1) * 0.5;
            const double sinc = std::abs (x) < 1.0e-9 ? 2.0 * cutoff : std::sin (twoPi * cutoff * x) / (pi * x);
            const double u = (double) i / (numTaps - 1);
            const double w = 0.42 - 0.5 * std::cos (twoPi * u) + 0.08 * std::cos (2.0 * twoPi * u);
            firTaps[(size_t) i] = (float) (sinc * w);
            sum += sinc * w;
        }
        for (int i = 0; i < numTaps; ++i)
            firTaps[(size_t) i] = (float) (firTaps[(size_t) i] / sum);
        std::fill (firHistory.begin(), firHistory.end(), 0.0f);
        firPos = 0;
        decimCounter = 0;
        std::fill (ring.begin(), ring.end(), 0.0);
        ringPos = 0;
        hopCounter = 0;
        windowFilled = 0;
        hopSeconds = hop / outRate;
        binHz = outRate / fftSize;
        maxBin = std::min (fftSize / 2 - 1, (int) (maxHz / binHz));
    }

    double getHopSeconds() const noexcept { return hopSeconds; }
    double getOutputRate() const noexcept { return outRate; }

    // Forget everything heard so far. The reading goes back to no key.
    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0);
        ringPos = 0; hopCounter = 0; windowFilled = 0;
        trebleMemory.fill (0.0); bassMemory.fill (0.0); opening.fill (0.0);
        scores.fill (0.0); probs.fill (0.0);
        heardSeconds = openingHeard = silenceSeconds = 0.0;
        presence = 0.0; contrast = 0.0;
        tunRe = tunIm = 0.0; tuning = 0.0;
        loudest = 0.0;
        candidate = -1; dwell = 0.0;
        current = Reading {};
        smoothedConfidence = 0.0;
        aged = false;
    }

    void setResponse (Response r) noexcept { response = r; }
    void setSource (Source s) noexcept { source = s; }

    // Mono samples at the host rate. Must already be finite.
    void push (const float* x, int count) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            firHistory[(size_t) firPos] = x[i];
            firHistory[(size_t) (firPos + numTaps)] = x[i];
            firPos = (firPos + 1 == numTaps) ? 0 : firPos + 1;
            if (++decimCounter < decimation)
                continue;
            decimCounter = 0;

            // firPos is now the oldest sample, so firHistory[firPos .. firPos + numTaps) runs oldest to newest.
            const float* h = firHistory.data() + firPos;
            const float* t = firTaps.data();
            float y = 0.0f;
            for (int k = 0; k < numTaps; ++k)
                y += h[k] * t[k];

            ring[(size_t) ringPos] = y;
            ringPos = (ringPos + 1) & (fftSize - 1);
            if (windowFilled < fftSize) ++windowFilled;
            if (++hopCounter >= hop)
            {
                hopCounter = 0;
                processFrame();
            }
        }
    }

    // Time passing with no audio at all, for a host that stops calling the plug-in. The reading fades as in silence.
    void idle (double seconds) noexcept
    {
        fade (seconds);
    }

    const Reading& reading() const noexcept { return current; }
    double secondsHeard() const noexcept { return heardSeconds; }

private:
    static constexpr int fftSize = 8192, hop = 2048;
    static constexpr int maxDecimation = 32, maxTaps = maxDecimation * 32 + 1;
    static constexpr int lowestNote = 33, numNotes = 64;       // A1 to C7 can be sounding notes
    static constexpr int binsPerNote = 3, topNote = 112;       // log spectrum runs up to about 5.3 kHz
    static constexpr int numLog = (topNote - lowestNote) * binsPerNote + 1;
    static constexpr double maxHz = 5400.0;
    static constexpr double silenceRms = 0.001;                // -60 dBFS
    static constexpr double newStartAfter = 8.0;               // this much silence and the next audio counts as a new start

    void buildProfiles() noexcept
    {
        // Listener ratings of how well each scale step fits a key (Krumhansl and Kessler, 1982).
        static constexpr double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static constexpr double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        // Where bass notes tend to sit: mostly tonic and fifth, then fourth, then the other chord roots of the key.
        static constexpr double bassMajor[12] = { 6.0, 0.3, 1.5, 0.3, 1.2, 3.0, 0.3, 4.0, 0.3, 1.8, 0.3, 0.6 };
        static constexpr double bassMinor[12] = { 6.0, 0.3, 0.8, 1.8, 0.3, 2.6, 0.3, 3.4, 1.8, 0.3, 1.6, 0.3 };
        static constexpr double triadMajor[12] = { 2.0, 0, 0, 0, 1.0, 0, 0, 1.0, 0, 0, 0, 0 };
        static constexpr double triadMinor[12] = { 2.0, 0, 0, 1.0, 0, 0, 0, 1.0, 0, 0, 0, 0 };
        for (int k = 0; k < 24; ++k)
        {
            const int tonic = k % 12;
            const bool isMinor = k >= 12;
            for (int i = 0; i < 12; ++i)
            {
                const int step = (i - tonic + 12) % 12;
                keyProfile[(size_t) k][(size_t) i] = isMinor ? minor[step] : major[step];
                bassProfile[(size_t) k][(size_t) i] = isMinor ? bassMinor[step] : bassMajor[step];
                triadProfile[(size_t) k][(size_t) i] = isMinor ? triadMinor[step] : triadMajor[step];
            }
            standardise (keyProfile[(size_t) k]);
            standardise (bassProfile[(size_t) k]);
            standardise (triadProfile[(size_t) k]);
        }
    }

    static void standardise (std::array<double, 12>& v) noexcept
    {
        double mean = 0.0, norm = 0.0;
        for (auto x : v) mean += x;
        mean /= 12.0;
        for (auto& x : v) { x -= mean; norm += x * x; }
        norm = std::sqrt (norm);
        if (norm > 0.0) for (auto& x : v) x /= norm;
    }

    // Correlation of a chroma with a standardised profile.
    static double correlate (const std::array<double, 12>& standard, const std::array<double, 12>& profile) noexcept
    {
        double s = 0.0;
        for (int i = 0; i < 12; ++i) s += standard[(size_t) i] * profile[(size_t) i];
        return s;
    }

    void buildDictionary() noexcept
    {
        std::fill (dict.begin(), dict.end(), 0.0f);
        for (int n = 0; n < numNotes; ++n)
        {
            float* col = dict.data() + n * numLog;
            double amp = 1.0;
            for (int h = 1; h <= 10; ++h, amp *= knobs.harmonicDecay)
            {
                const double pos = 12.0 * std::log2 ((double) h) * binsPerNote + n * binsPerNote;
                const int centre = (int) std::lround (pos);
                for (int d = -1; d <= 1; ++d)
                {
                    const int b = centre + d;
                    if (b >= 0 && b < numLog)
                        col[b] = std::max (col[b], (float) (amp * (d == 0 ? 1.0 : 0.5)));
                }
            }
            double norm = 0.0;
            for (int b = 0; b < numLog; ++b) norm += (double) col[b] * col[b];
            norm = std::sqrt (norm);
            for (int b = 0; b < numLog; ++b) col[b] = (float) (col[b] / norm);
        }
        for (int a = 0; a < numNotes; ++a)
            for (int b = a; b < numNotes; ++b)
            {
                double s = 0.0;
                const float* x = dict.data() + a * numLog;
                const float* y = dict.data() + b * numLog;
                for (int i = 0; i < numLog; ++i) s += (double) x[i] * y[i];
                wtw[(size_t) (a * numNotes + b)] = wtw[(size_t) (b * numNotes + a)] = s;
            }
    }

    double interpolatedMag (double p) const noexcept
    {
        const int i = std::clamp ((int) p, 0, maxBin);
        const double f = p - i;
        return mag[(size_t) i] * (1.0 - f) + mag[(size_t) std::min (i + 1, maxBin + 1)] * f;
    }

    void estimateTuning() noexcept
    {
        const double twoPi = 6.283185307179586476925;
        double top = 0.0;
        const int lo = (int) (80.0 / binHz), hi = std::min (maxBin - 1, (int) (2500.0 / binHz));
        for (int k = lo; k < hi; ++k) top = std::max (top, mag[(size_t) k]);
        if (top <= 0.0) return;
        const double decay = std::exp (-hopSeconds / 15.0);
        tunRe *= decay; tunIm *= decay;
        const double gate = top * 0.05;
        for (int k = lo; k < hi; ++k)
        {
            const double m = mag[(size_t) k];
            if (m < gate || m < mag[(size_t) k - 1] || m <= mag[(size_t) k + 1]) continue;
            const double a = std::log (mag[(size_t) k - 1] + 1.0e-12), b = std::log (m + 1.0e-12), c = std::log (mag[(size_t) k + 1] + 1.0e-12);
            const double denom = a - 2.0 * b + c;
            const double shift = std::abs (denom) > 1.0e-9 ? std::clamp (0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
            const double freq = (k + shift) * binHz;
            const double semis = 12.0 * std::log2 (freq / 440.0);
            const double angle = twoPi * (semis - std::floor (semis));
            const double weight = m / top;
            tunRe += weight * std::cos (angle);
            tunIm += weight * std::sin (angle);
        }
        tuning = std::atan2 (tunIm, tunRe) / twoPi; // semitones, -0.5 to 0.5
    }

    void buildLogSpectrum() noexcept
    {
        const double ratio = std::pow (2.0, tuning / 12.0);
        const double halfWidth = std::pow (2.0, 1.0 / (12.0 * binsPerNote * 2.0)) - 1.0;
        for (int i = 0; i < numLog; ++i)
        {
            const double p = logFreqs[(size_t) i] * ratio / binHz;
            const double w = p * halfWidth;
            double v = std::max (interpolatedMag (p - w), interpolatedMag (p + w));
            const int a = (int) std::ceil (p - w), b = std::min ((int) std::floor (p + w), maxBin);
            for (int k = std::max (a, 0); k <= b; ++k) v = std::max (v, mag[(size_t) k]);
            logSpec[(size_t) i] = v;
        }
        // Take off the local floor, one octave wide. Tonal peaks stand clear of it and noise mostly does not.
        const int radius = 6 * binsPerNote;
        double sum = 0.0; int count = 0, l = 0, r = -1;
        for (int i = 0; i < numLog; ++i)
        {
            while (r < std::min (i + radius, numLog - 1)) { ++r; sum += logSpec[(size_t) r]; ++count; }
            while (l < i - radius) { sum -= logSpec[(size_t) l]; ++l; --count; }
            logClean[(size_t) i] = std::max (0.0, logSpec[(size_t) i] - 1.3 * sum / count);
        }
    }

    void solveNotes() noexcept
    {
        double scale = 0.0;
        for (int n = 0; n < numNotes; ++n)
        {
            const float* col = dict.data() + n * numLog;
            double s = 0.0;
            for (int i = 0; i < numLog; ++i) s += (double) col[i] * logClean[(size_t) i];
            wtv[(size_t) n] = s;
            notes[(size_t) n] = s;
            scale = std::max (scale, s);
        }
        if (scale <= 0.0) { std::fill (notes.begin(), notes.end(), 0.0); return; }
        const double floorValue = scale * 1.0e-9;
        for (int it = 0; it < knobs.iterations; ++it)
            for (int n = 0; n < numNotes; ++n)
            {
                const double* row = wtw.data() + n * numNotes;
                double denom = floorValue;
                for (int m = 0; m < numNotes; ++m) denom += row[m] * notes[(size_t) m];
                notes[(size_t) n] *= wtv[(size_t) n] / denom;
            }
    }

    void processFrame() noexcept
    {
        double energy = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const double s = ring[(size_t) ((ringPos + i) & (fftSize - 1))];
            energy += s * s;
            re[(size_t) i] = s * window[(size_t) i];
            im[(size_t) i] = 0.0;
        }
        // Compare with the part of the window that has audio in it, so the first frames are not called silence.
        const double rms = std::sqrt (energy / std::max (windowFilled, 1));
        const bool hasSignal = rms > silenceRms && std::isfinite (rms);
        current.signal = hasSignal;
        if (! hasSignal)
        {
            fade (hopSeconds);
            return;
        }

        if (silenceSeconds >= newStartAfter)
            startAfresh();
        silenceSeconds = 0.0;

        fft.forward (re.data(), im.data());
        const double scale = 2.0 / hannSum;
        for (int k = 0; k <= maxBin + 1; ++k)
            mag[(size_t) k] = scale * std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);

        estimateTuning();
        buildLogSpectrum();
        solveNotes();

        std::array<double, 12> treble {}, bass {};
        const bool voice = source == Source::voice;
        for (int n = 0; n < numNotes; ++n)
        {
            const double a = notes[(size_t) n];
            const int midi = lowestNote + n;
            const int pc = midi % 12;
            const double bassShare = voice ? 0.0 : std::clamp ((55.0 - midi) / 7.0, 0.0, 1.0);
            treble[(size_t) pc] += a * (voice ? 1.0 : std::clamp ((midi - 48.0) / 7.0, 0.0, 1.0));
            bass[(size_t) pc] += a * bassShare;
        }
        double total = 0.0;
        for (int i = 0; i < 12; ++i) total += treble[(size_t) i] + bass[(size_t) i];

        // Quiet frames count for less, measured against the recent loudest.
        loudest = std::max (rms, loudest * std::exp (-hopSeconds / 10.0));
        const double frameWeight = std::sqrt (std::min (1.0, rms / std::max (loudest, 1.0e-9)));

        const double memoryTau = response == Response::fast ? knobs.tauFast : knobs.tauStable;
        const double decay = std::exp (-hopSeconds / memoryTau);
        for (int i = 0; i < 12; ++i)
        {
            const double t = total > 1.0e-12 ? treble[(size_t) i] / total : 0.0;
            const double b = total > 1.0e-12 ? bass[(size_t) i] / total : 0.0;
            trebleMemory[(size_t) i] = trebleMemory[(size_t) i] * decay + frameWeight * t;
            bassMemory[(size_t) i] = bassMemory[(size_t) i] * decay + frameWeight * b;
        }
        heardSeconds += hopSeconds;

        if (openingHeard < knobs.openingSeconds && total > 1.0e-12)
        {
            for (int i = 0; i < 12; ++i)
                opening[(size_t) i] += frameWeight * (treble[(size_t) i] + 1.5 * bass[(size_t) i]) / total;
            openingHeard += hopSeconds;
        }

        presence += (1.0 - presence) * (1.0 - std::exp (-hopSeconds / 0.4));
        scoreKeys();
        updateReading();
        publishChroma();
    }

    void scoreKeys() noexcept
    {
        auto standardised = [] (const std::array<double, 12>& in, double power, double* sumOut)
        {
            std::array<double, 12> out {};
            double sum = 0.0;
            for (int i = 0; i < 12; ++i) { out[(size_t) i] = std::pow (std::max (in[(size_t) i], 0.0), power); sum += out[(size_t) i]; }
            if (sumOut != nullptr) *sumOut = sum;
            standardise (out);
            return out;
        };

        double trebleSum = 0.0, bassSum = 0.0, rawTreble = 0.0, rawBass = 0.0;
        for (int i = 0; i < 12; ++i) { rawTreble += trebleMemory[(size_t) i]; rawBass += bassMemory[(size_t) i]; }
        const auto t = standardised (trebleMemory, knobs.chromaPower, &trebleSum);
        const auto b = standardised (bassMemory, knobs.chromaPower, &bassSum);
        const auto o = standardised (opening, 1.0, nullptr);

        // How far the chroma is from flat: share of energy in its seven strongest notes, 0 when flat and 1 when only seven sound.
        std::array<double, 12> sorted = trebleMemory;
        std::sort (sorted.begin(), sorted.end(), std::greater<double>());
        double topSeven = 0.0;
        for (int i = 0; i < 7; ++i) topSeven += sorted[(size_t) i];
        contrast = rawTreble > 1.0e-12 ? std::clamp ((topSeven / rawTreble - 7.0 / 12.0) / (5.0 / 12.0), 0.0, 1.0) : 0.0;

        const double bassScale = std::min (1.0, rawBass / std::max (0.1 * (rawTreble + rawBass), 1.0e-12));
        const double openingAge = std::max (0.0, heardSeconds - knobs.openingSeconds);
        const double openingGain = source == Source::voice ? 0.0 : knobs.openingWeight * std::exp (-openingAge / knobs.openingTau);
        const double bassGain = source == Source::voice ? 0.0 : knobs.bassWeight * bassScale;

        for (int k = 0; k < 24; ++k)
        {
            double s = correlate (t, keyProfile[(size_t) k]);
            if (bassGain > 0.0) s += bassGain * correlate (b, bassProfile[(size_t) k]);
            // The opening chord only helps choose between a key and its relative, which share the same notes. It says
            // nothing about which set of notes this is, so a song that opens away from home does not mislead the rest.
            if (openingGain > 0.0) s += openingGain * 0.5 * (correlate (o, triadProfile[(size_t) k]) - correlate (o, triadProfile[(size_t) relativeOf (k)]));
            scores[(size_t) k] = s;
        }
        double best = -1.0e9;
        for (auto s : scores) best = std::max (best, s);
        double sum = 0.0;
        for (int k = 0; k < 24; ++k) { probs[(size_t) k] = std::exp (knobs.softness * (scores[(size_t) k] - best)); sum += probs[(size_t) k]; }
        for (auto& p : probs) p /= sum;
    }

    double confidenceGate() const noexcept
    {
        // Flat chroma, such as from noise, gives no trust whatever the scores say.
        const double x = std::clamp ((contrast - 0.22) / 0.2, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    static int relativeOf (int k) noexcept { return k < 12 ? 12 + (k + 9) % 12 : (k - 12 + 3) % 12; }

    void updateReading() noexcept
    {
        int top = 0, runnerUp = 0;
        double best = -1.0e9, second = -1.0e9;
        for (int k = 0; k < 24; ++k)
        {
            if (scores[(size_t) k] > best) { second = best; runnerUp = top; best = scores[(size_t) k]; top = k; }
            else if (scores[(size_t) k] > second) { second = scores[(size_t) k]; runnerUp = k; }
        }

        const bool stable = response == Response::stable;
        int shown = current.key < 0 ? -1 : current.key + (current.minor ? 12 : 0);
        const double gate = confidenceGate();

        if (shown < 0)
        {
            const double first = stable ? knobs.firstStable : knobs.firstFast;
            if (heardSeconds >= first && gate > 0.5)
            {
                shown = top;
                candidate = -1;
                dwell = 0.0;
            }
        }
        else if (top == shown)
        {
            candidate = -1;
            dwell = 0.0;
        }
        else
        {
            if (top == candidate) dwell += hopSeconds;
            else { candidate = top; dwell = hopSeconds; }
            // Early on the memory holds little, so a first reading is provisional and may move quickly.
            const double settled = std::clamp (heardSeconds / knobs.settleSeconds, 0.25, 1.0);
            // A move to the relative key adds no new notes, so it needs much stronger evidence than a move to anything else.
            const double guard = top == relativeOf (shown) ? knobs.relativeGuard : 1.0;
            const double need = (stable ? knobs.dwellStable : knobs.dwellFast) * settled * guard;
            const double margin = (stable ? knobs.marginStable : knobs.marginFast) * settled * guard;
            if (dwell >= need && scores[(size_t) top] - scores[(size_t) shown] >= margin && gate > 0.5)
            {
                shown = top;
                candidate = -1;
                dwell = 0.0;
            }
        }

        if (shown >= 0)
        {
            current.key = shown % 12;
            current.minor = shown >= 12;
            const int alt = (top == shown) ? runnerUp : top;
            current.altKey = alt % 12;
            current.altMinor = alt >= 12;
            const double target = probs[(size_t) shown] * gate * presence;
            smoothedConfidence += (target - smoothedConfidence) * (1.0 - std::exp (-hopSeconds / 0.8));
            current.confidence = (float) std::clamp (smoothedConfidence, 0.0, 1.0);
        }
        current.tuningCents = (float) (tuning * 100.0);
    }

    void publishChroma() noexcept
    {
        double top = 0.0;
        for (int i = 0; i < 12; ++i) top = std::max (top, trebleMemory[(size_t) i] + bassMemory[(size_t) i]);
        for (int i = 0; i < 12; ++i)
            current.chroma[(size_t) i] = top > 0.0 ? (float) ((trebleMemory[(size_t) i] + bassMemory[(size_t) i]) / top) : 0.0f;
    }

    void fade (double seconds) noexcept
    {
        silenceSeconds += seconds;
        const double tau = response == Response::fast ? 1.0 : 2.5;
        const double f = std::exp (-seconds / tau);
        presence *= f;
        smoothedConfidence *= f;
        if (smoothedConfidence < 0.005) smoothedConfidence = 0.0;
        current.confidence = (float) smoothedConfidence;
        current.signal = false;
    }

    // After a long silence the next audio may be another song. Weaken the old memory and listen for an opening chord again.
    void startAfresh() noexcept
    {
        for (auto& x : trebleMemory) x *= 0.25;
        for (auto& x : bassMemory) x *= 0.25;
        opening.fill (0.0);
        openingHeard = 0.0;
        heardSeconds = std::min (heardSeconds, knobs.openingSeconds);
    }

    Knobs knobs;
    Fft fft;
    std::vector<double> window, ring, re, im, mag;
    std::vector<float> firTaps, firHistory;
    std::vector<double> logFreqs, logSpec, logClean, notes, wtv, wtw;
    std::vector<float> dict;
    std::array<std::array<double, 12>, 24> keyProfile {}, bassProfile {}, triadProfile {};

    double hostRate = 48000.0, outRate = 12000.0, hopSeconds = 0.17, binHz = 1.46, hannSum = 1.0;
    int decimation = 4, numTaps = 129, firPos = 0, decimCounter = 0;
    int ringPos = 0, hopCounter = 0, windowFilled = 0, maxBin = 3000;

    Response response = Response::stable;
    Source source = Source::mix;

    std::array<double, 12> trebleMemory {}, bassMemory {}, opening {};
    std::array<double, 24> scores {}, probs {};
    double heardSeconds = 0.0, openingHeard = 0.0, silenceSeconds = 0.0, presence = 0.0, contrast = 0.0;
    double tunRe = 0.0, tunIm = 0.0, tuning = 0.0, loudest = 0.0;
    int candidate = -1;
    double dwell = 0.0, smoothedConfidence = 0.0;
    bool aged = false;
    Reading current;
};
} // namespace smoothkey
