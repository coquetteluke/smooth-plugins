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
//   7. Score all 24 keys: treble against a key profile, bass against a profile that favours tonic and fifth, time spent on
//      each key's home chord, and a tie break between relative keys from the opening chord, because Am F C G and C G Am F
//      contain exactly the same notes in the same amounts.
//      A major chord falling a fifth to the next chord (a dominant resolving) also adds to the key it lands on.
//      When recent notes clearly fall outside the key on show, the old memory is mostly let go, so a real key change is
//      followed in seconds without making the memory short for everything else.
//      The chords since the current section began also count: which key they belong to, and which chord is home, the one
//      the section opened on and stays on longest. This follows a move to a close key without waiting for its notes. Stable
//      gives it less weight, and counts the opening chord only while the section's chords all fit the key.
//   8. Only move the reading when a new key has led for a set time by a set margin, so it cannot flicker.
//
// A lone voice (Source Single Voice, or Full Mix when only one line sounds at a time) is read differently, since 1.6.0:
//   - The grid stays at A440 unless the sound clearly sits on one offset. An instrument recorded flat does; a singer who is
//     off key does not, and following his average moved the note names about from one phrase to the next.
//   - Only held notes count: a frame whose strongest note stands clear and is the note of the frame before. Glides,
//     consonants and speech like rap fill the spectrum too, but they are not notes of the key.
//   - Held notes are remembered for about a minute, not a chord loop's length, as a tune takes that long to show its notes.
//   - They are scored against a profile in which every note of the scale counts about the same (Temperley's revision of
//     the listener ratings), and notes outside the scale count against the key. The listener ratings favour tonic, fifth
//     and third so much that a tune that sits on its fifth was read as the key a fifth up.

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
    float tempoBpm = 0.0f;        // 0 until a steady beat is found
    std::array<float, 12> chroma {};
    // For the window only. Nothing in the analyser reads these back.
    std::array<float, 24> candidates {}; // how likely each key looks now: 0 to 11 major from C, then 12 to 23 minor
    std::array<float, 12> liveNotes {};  // the notes sounding in about the last fifth of a second
};

class KeyAnalyser
{
public:
    // Settings a build or a test can vary. The defaults are what ships.
    struct Knobs
    {
        double tauStable = 14.0, tauFast = 12.0;         // memory of the chroma, seconds
        double dwellStable = 4.0, dwellFast = 1.5;      // a new key must lead this long
        double marginStable = 0.04, marginFast = 0.03; // and by this much score
        double firstStable = 7.7, firstFast = 3.0;      // earliest first reading, seconds of audio
        double firstLeadStable = 0.25, firstLeadFast = 0.03;
        double firstRelativeLeadStable = 0.1;           // Stable's first reading also waits until it leads its own relative by this much
        double settleSeconds = 18.0;                    // the dwell and margin reach full size after this much audio
        double bassWeight = 0.45;
        double openingWeight = 0.4;
        double openingBassWeight = 0.1; // more trust in the opening when a bass line says where the chords are, not for a lone tune
        double openingBassShare = 0.35; // the share of bass notes that earns all of that, from none at 0.15
        double relativeGuard = 2.0;
        double returnGuard = 4.0;
        double outsideShare = 0.30;                     // share of recent notes outside the shown key that counts as outside
        double outsideStable = 3.0, outsideFast = 1.5;  // and how long that must last before the old memory is let go
        double recentStable = 4.0, recentFast = 2.5;    // memory of the recent notes used for that test, seconds
        double holdStable = 12.0, holdFast = 6.0;       // after a move the reading stays put at least this long
        double homeChordWeight = 0.5;
        double cadenceWeight = 0.15;
        double cadenceTau = 40.0;
        double cadenceFull = 1.0;
        double openingSeconds = 1.5;
        double openingTau = 60.0;
        double openingBassTau = 0.0;    // and it is remembered this much longer. Above 0 made more Stable readings move after 8 s
        double sectionLookback = 1.0;   // on a key change, 1 finds where the new notes began and hears that chord as a new opening
        double sectionMaxGap = 2.6;     // seconds of notes that fit both keys that can sit inside the new section
        double sectionShare = 0.15;     // a frame with this share of notes outside the old key belongs to the new section
        double sectionSkip = 1;         // frames left out after the boundary, as the 0.7 s window still holds the old chord
        double sectionBassShare = 0.2;  // needs at least this share of bass notes, so a lone tune is left alone
        double sectionNovel = 1.0;      // 1 also takes the first chord change not heard lately as the start of the section
        double sectionOnMove = 1.0;     // 1 also looks back when the reading moves with no outside notes having let go
        double transitionMemory = 40.0; // a chord change heard within this long is not new
        double sectionOnLoop = 3;       // this many new chord changes after a settled loop start a new section. 0 is off
        double loopLetGo = 0.5;         // how much of the time on each chord is kept when that happens
        double loopNotes = 1.0;         // and how much of the note memory
        double holdAfterLetGo = 1.0;    // 1 shortens the hold in the seconds after a let go, while the new memory is still filling
        double rebuildOnLetGo = 1.0;    // 1 keeps the notes and chords heard since the new notes began whole when letting go, and drops only what came before
        double harmonicDecay = 0.65;
        double oddHarmonicNotes = 1.0;  // 1 also lets each note be one with odd harmonics only, as on a clarinet or a hollow synth, so its third harmonic is not heard as a fifth
        double oddPenalty = 0.05;       // how much more a note must explain to be heard as odd harmonics only, so chord tones are not read as harmonics of a missing root
        double outsideWeight = 1.2;     // takes off this much times the share of notes heard that fall outside a key's scale
        // Harmony since the section began: which key its chords belong to, and which chord is home (the one the section opens
        // on and the one it stays on longest). 0 is off. Stable has it at a lower weight since 1.5.0. Giving credit to the chord
        // a dominant resolves to as well made more loops of i iv VII III read as their relative major, so it is left out.
        double harmonyFast = 1.0, harmonyStable = 0.7;
        double harmonyFit = 1.0;        // takes off this much times the share of the section's time on chords outside the key
        double harmonyFirst = 0.3;      // the section opens on the key's home chord
        double harmonyHome = 0.2;       // times the share of the section's time on the home chord
        double harmonyRamp = 1.0;       // reaches full weight after this many chord changes in the section
        // Stable only: the opening chord counts less when the section's chords leave the key, by this much times the share of time
        // outside it, so one chord of four outside takes it away. Without this, ii V I I loops were read as the key of their ii
        // chord, and the reading went there and back.
        double harmonyFirstFitStable = 4.0;
        // A lone voice. See the note at the top of this file.
        double voiceModel = 1.0;        // 1 reads a lone voice by its held notes. 0 is the reading before 1.6.0, for comparison
        double tuneClarityLow = 0.12;   // how tightly the spectral peaks sit on one offset (0 spread evenly, 1 all on one): at or under
        double tuneClarityHigh = 0.22;  // the first the grid stays at A440, from the second up it follows the sound fully
        double tuneClarityAfter = 1.0;  // seconds heard before that is judged, as the first frames hold too few peaks
        double heldShare = 0.45;        // a held note: the frame's strongest note has this share of all notes heard in it
        double heldFull = 3.0;          // and counts in full once it has been the strongest for this many more frames (about half a second)
        double tauVoice = 120.0;        // memory of held notes, seconds: a tune takes a verse and a chorus to show its notes
        double recentVoice = 3.0;       // the recent held notes, for noticing a key change: about this many seconds of them
        double voiceOutsideWeight = 2.0; // takes off this much times the share of held notes outside a key's seven notes
        double voiceOutsideShare = 0.5; // share of recent held notes outside the shown key that means the key has changed
        double voiceOutsideSeconds = 1.0; // and for how many more seconds of held notes that must last
        double voiceOutsideHeld = 2.0;  // with at least this many seconds of recent held notes,
        double voiceOutsideFit = 0.8;   // of which this share must sit inside some other key's seven notes
        double voiceKeep = 0.0;         // how much of the older held notes is kept when the key is let go
        double voiceMargin = 2.0;       // a new key must lead the shown one by this many times the usual margin
        double voiceHome = 1.0;         // how much the listener ratings count in choosing between a key and its relative. 0: the even profile alone
        double voiceMinHeld = 2.5;      // seconds of held notes in memory before a first reading
        double voiceFitLow = 0.70;      // share of the held notes inside the best key's seven notes: at or under this there is no key
        double voiceFitHigh = 0.80;     // to trust, as in rap or speech, and from this up the reading is trusted fully
        double loneFrames = 5;          // Full Mix: two pitch classes sounding through this many frames in a row is a chord
        double loneShare = 0.10;        // each with at least this share of the frame
        double loneTau = 6.0;           // how long the share of such frames is averaged over, seconds
        double loneEnter = 0.20;        // under this share the sound is one line at a time and is read as a lone voice
        double loneLeave = 0.45;        // over this it is music with chords again
        double loneWait = 0.45;         // under this share Full Mix holds back its first reading until it knows which it is hearing
        double loneInMix = 1.0;         // 0 turns that off: Full Mix then always reads chords and a bass line
        double chromaPower = 0.75;      // compression of the long term chroma
        double softness = 14.0;         // turns score gaps into a probability
        int iterations = 30;
    };

    KeyAnalyser()
        : fft (fftSize), window ((size_t) fftSize), ring ((size_t) fftSize), re ((size_t) fftSize), im ((size_t) fftSize),
          mag ((size_t) fftSize / 2 + 1), firTaps ((size_t) maxTaps), firHistory ((size_t) maxTaps * 2),
          logFreqs ((size_t) numLog), logSpec ((size_t) numLog), logClean ((size_t) numLog), notes ((size_t) numNotes),
          atoms ((size_t) numAtoms), wtv ((size_t) numAtoms), wtw ((size_t) (numAtoms * numAtoms)), dict ((size_t) (numAtoms * numLog)),
          shortFft (tempoFft), shortWindow ((size_t) tempoFft), shortRe ((size_t) tempoFft), shortIm ((size_t) tempoFft),
          lastShort ((size_t) tempoFft / 2), onsets ((size_t) numOnsets), onsetWork ((size_t) numOnsets)
    {
        const double twoPi = 6.283185307179586476925;
        hannSum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / fftSize);
            hannSum += window[(size_t) i];
        }
        for (int i = 0; i < tempoFft; ++i)
            shortWindow[(size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / tempoFft);
        for (int i = 0; i < numLog; ++i)
            logFreqs[(size_t) i] = 440.0 * std::pow (2.0, ((lowestNote + i / (double) binsPerNote) - 69.0) / 12.0);
        buildProfiles();
        buildDictionary();
        configure (48000.0);
        reset();
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
        resetTempo();
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
        trebleMemory.fill (0.0); bassMemory.fill (0.0); opening.fill (0.0); chordMemory.fill (0.0); recentMemory.fill (0.0);
        heldMemory.fill (0.0); heldRecent.fill (0.0); heldSeconds = 0.0; voiceFit = 0.0; lastTopPc = -1; heldRun = 0;
        for (auto& f : loneHistory) f.fill (0.0);
        lonePos = loneCount = 0; chordShare = 1.0; lone = false;
        tunWeight = 0.0;
        for (auto& f : frameHistory) f.fill (0.0);
        for (auto& f : frameTreble) f.fill (0.0);
        for (auto& f : frameBass) f.fill (0.0);
        frameChord.fill (-1); frameWeights.fill (0.0);
        historyPos = historyCount = 0;
        openingBorn = 0.0;
        for (auto& row : transitionHeard) row.fill (-1.0e9);
        changePos = changeCount = 0;
        heldPos = heldCount = 0;
        freshRun = repeatRun = 0; loopSettled = false;
        outsideSeconds = 0.0; sinceLetGo = 1.0e9;
        scores.fill (0.0); probs.fill (0.0);
        heardSeconds = openingHeard = silenceSeconds = 0.0;
        presence = 0.0; contrast = 0.0;
        tunRe = tunIm = 0.0; tuning = 0.0;
        loudest = 0.0;
        candidate = -1; dwell = 0.0; previousShown = -1; lastMove = -1000.0;
        cadence.fill (0.0); heldChord = pendingChord = -1; pendingFrames = 0;
        current = Reading {};
        resetTempo();
        smoothedConfidence = 0.0;
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
            if (++tempoCounter >= tempoHop)
            {
                tempoCounter = 0;
                tempoFrame();
            }
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
    static constexpr int numHistory = 160;                     // about 27 s of the notes in each frame, to look back at a key change
    static constexpr int numChanges = 64;                      // recent chord changes
    static constexpr int tempoFft = 512, tempoHop = 256, numOnsets = 1024; // about 20 s of onset curve
    static constexpr int maxDecimation = 32, maxTaps = maxDecimation * 32 + 1;
    static constexpr int lowestNote = 33, numNotes = 64;       // A1 to C7 can be sounding notes
    static constexpr int numAtoms = 2 * numNotes;              // each note with all its harmonics, then with odd ones only
    static constexpr int binsPerNote = 3, topNote = 112;       // log spectrum runs up to about 5.3 kHz
    static constexpr int numLog = (topNote - lowestNote) * binsPerNote + 1;
    static constexpr double maxHz = 5400.0;
    static constexpr double silenceRms = 0.001;                // -60 dBFS
    static constexpr double newStartAfter = 8.0;               // this much silence and the next audio counts as a new start
    struct HeldChord { int chord = -1; double start = 0.0; bool fresh = false; }; // a chord as it was settled on, and whether the change to it was new

    void buildProfiles() noexcept
    {
        // Listener ratings of how well each scale step fits a key (Krumhansl and Kessler, 1982).
        static constexpr double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static constexpr double minor[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        // Where bass notes tend to sit: mostly tonic and fifth, then fourth, then the other chord roots of the key.
        static constexpr double bassMajor[12] = { 6.0, 0.3, 1.5, 0.3, 1.2, 3.0, 0.3, 4.0, 0.3, 1.8, 0.3, 0.6 };
        static constexpr double bassMinor[12] = { 6.0, 0.3, 0.8, 1.8, 0.3, 2.6, 0.3, 3.4, 1.8, 0.3, 1.6, 0.3 };
        // For a lone voice: Temperley's revision of those ratings (1999). Every scale note counts about the same, the seventh as
        // much as the fourth, so a tune that sits on its fifth or third is not read as the key a fifth up.
        static constexpr double voiceMajor[12] = { 5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0 };
        static constexpr double voiceMinor[12] = { 5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0 };
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
                voiceProfile[(size_t) k][(size_t) i] = isMinor ? voiceMinor[step] : voiceMajor[step];
            }
            standardise (voiceProfile[(size_t) k]);
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
        activeAtoms = knobs.oddHarmonicNotes > 0.0 ? numAtoms : numNotes;
        for (int atom = 0; atom < activeAtoms; ++atom)
        {
            const int n = atom % numNotes;
            const bool oddOnly = atom >= numNotes;
            float* col = dict.data() + atom * numLog;
            double amp = 1.0;
            for (int h = 1; h <= 10; ++h, amp *= knobs.harmonicDecay)
            {
                if (oddOnly && h % 2 == 0) continue;
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
        for (int a = 0; a < activeAtoms; ++a)
            for (int b = a; b < activeAtoms; ++b)
            {
                double s = 0.0;
                const float* x = dict.data() + a * numLog;
                const float* y = dict.data() + b * numLog;
                for (int i = 0; i < numLog; ++i) s += (double) x[i] * y[i];
                wtw[(size_t) (a * numAtoms + b)] = wtw[(size_t) (b * numAtoms + a)] = s;
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
        tunRe *= decay; tunIm *= decay; tunWeight *= decay;
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
            tunWeight += weight;
            tunRe += weight * std::cos (angle);
            tunIm += weight * std::sin (angle);
        }
        tuning = std::atan2 (tunIm, tunRe) / twoPi; // semitones, -0.5 to 0.5
        // Peaks spread all round the semitone have no offset to follow: an off key singer, or noise. The grid then stays at A440.
        // Times exactly 1 from tuneClarityHigh up, so music with a clear tuning reads bit for bit as before.
        if (knobs.voiceModel > 0.0 && heardSeconds >= knobs.tuneClarityAfter && tunWeight > 0.0)
        {
            const double clarity = std::hypot (tunRe, tunIm) / tunWeight;
            if (clarity < knobs.tuneClarityHigh)
            {
                const double x = std::clamp ((clarity - knobs.tuneClarityLow) / (knobs.tuneClarityHigh - knobs.tuneClarityLow), 0.0, 1.0);
                tuning *= x * x * (3.0 - 2.0 * x);
            }
        }
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
        for (int n = 0; n < activeAtoms; ++n)
        {
            const float* col = dict.data() + n * numLog;
            double s = 0.0;
            for (int i = 0; i < numLog; ++i) s += (double) col[i] * logClean[(size_t) i];
            wtv[(size_t) n] = s;
            atoms[(size_t) n] = s;
            scale = std::max (scale, s);
        }
        if (scale <= 0.0) { std::fill (notes.begin(), notes.end(), 0.0); return; }
        const double floorValue = scale * 1.0e-9;
        for (int it = 0; it < knobs.iterations; ++it)
            for (int n = 0; n < activeAtoms; ++n)
            {
                const double* row = wtw.data() + n * numAtoms;
                double denom = floorValue + (n >= numNotes ? knobs.oddPenalty * scale : 0.0);
                for (int m = 0; m < activeAtoms; ++m) denom += row[m] * atoms[(size_t) m];
                atoms[(size_t) n] *= wtv[(size_t) n] / denom;
            }
        // Both kinds of the same note are the same note.
        for (int n = 0; n < numNotes; ++n)
            notes[(size_t) n] = atoms[(size_t) n] + (activeAtoms > numNotes ? atoms[(size_t) (n + numNotes)] : 0.0);
    }

    // Tempo: how much the short spectrum rises at each moment, then which steady pulse best fits that curve.
    void tempoFrame() noexcept
    {
        for (int i = 0; i < tempoFft; ++i)
        {
            const double s = ring[(size_t) ((ringPos - tempoFft + i + fftSize) & (fftSize - 1))];
            shortRe[(size_t) i] = s * shortWindow[(size_t) i];
            shortIm[(size_t) i] = 0.0;
        }
        shortFft.forward (shortRe.data(), shortIm.data());
        double rise = 0.0;
        for (int k = 1; k < tempoFft / 2; ++k)
        {
            const double m = std::log1p (1000.0 * std::sqrt (shortRe[(size_t) k] * shortRe[(size_t) k] + shortIm[(size_t) k] * shortIm[(size_t) k]));
            rise += std::max (0.0, m - lastShort[(size_t) k]);
            lastShort[(size_t) k] = m;
        }
        onsets[(size_t) onsetPos] = rise;
        onsetPos = (onsetPos + 1) % numOnsets;
        if (onsetCount < numOnsets) ++onsetCount;
        const double rate = outRate / tempoHop;
        if (++tempoEstimateCounter >= (int) rate && onsetCount >= (int) (6.0 * rate))
        {
            tempoEstimateCounter = 0;
            estimateTempo (rate);
        }
    }

    void estimateTempo (double rate) noexcept
    {
        const double twoPi = 6.283185307179586476925;
        const int count = std::min (onsetCount, (int) (14.0 * rate));
        double mean = 0.0;
        for (int i = 0; i < count; ++i) mean += onsets[(size_t) ((onsetPos - 1 - i + 2 * numOnsets) % numOnsets)];
        mean /= count;
        double top = 0.0;
        for (int i = 0; i < count; ++i) top = std::max (top, onsets[(size_t) ((onsetPos - 1 - i + 2 * numOnsets) % numOnsets)]);
        // A steady tone only ripples. It takes a real attack to count as a beat.
        if (top - mean < 20.0) { current.tempoBpm = 0.0f; return; }
        for (int i = 0; i < count; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (twoPi * (i + 0.5) / count);
            onsetWork[(size_t) i] = (onsets[(size_t) ((onsetPos - count + i + 2 * numOnsets) % numOnsets)] - mean) * w;
        }
        const auto strength = [&] (double bpm)
        {
            const double step = twoPi * bpm / 60.0 / rate, cr = std::cos (step), ci = std::sin (step);
            double pr = 1.0, pi = 0.0, a = 0.0, b = 0.0;
            for (int i = 0; i < count; ++i)
            {
                a += onsetWork[(size_t) i] * pr;
                b += onsetWork[(size_t) i] * pi;
                const double nr = pr * cr - pi * ci;
                pi = pr * ci + pi * cr;
                pr = nr;
            }
            return std::sqrt (a * a + b * b);
        };
        double best = 0.0, bestBpm = 0.0, total = 0.0;
        int tried = 0;
        for (double bpm = 60.0; bpm <= 240.0; bpm += 0.5)
        {
            // A real pulse also shows at two and four times its rate. A mild preference for the middle of the range settles half or double.
            const double fit = strength (bpm) + strength (bpm * 2.0) + 0.5 * strength (bpm * 4.0);
            const double octaves = std::log2 (bpm / 115.0);
            const double score = fit * std::exp (-0.5 * octaves * octaves);
            total += score;
            ++tried;
            if (score > best) { best = score; bestBpm = bpm; }
        }
        // No pulse stands out: show nothing rather than a guess.
        if (best < 2.2 * total / tried) { current.tempoBpm = 0.0f; return; }
        for (double bpm = bestBpm - 0.5; bpm <= bestBpm + 0.5; bpm += 0.05)
        {
            const double fit = strength (bpm) + strength (bpm * 2.0) + 0.5 * strength (bpm * 4.0);
            const double octaves = std::log2 (bpm / 115.0);
            const double score = fit * std::exp (-0.5 * octaves * octaves);
            if (score > best) { best = score; bestBpm = bpm; }
        }
        current.tempoBpm = (float) bestBpm;
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
            // A single voice has no bass line, so anything below A2 is rumble or spill and is left out.
            treble[(size_t) pc] += a * (voice ? (midi >= 45 ? 1.0 : 0.0) : std::clamp ((midi - 48.0) / 7.0, 0.0, 1.0));
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
            const double recentDecay = std::exp (-hopSeconds / (response == Response::fast ? knobs.recentFast : knobs.recentStable));
            recentMemory[(size_t) i] = recentMemory[(size_t) i] * recentDecay + frameWeight * (t + b);
            current.liveNotes[(size_t) i] += (float) ((frameWeight * (t + b) - (double) current.liveNotes[(size_t) i]) * (1.0 - std::exp (-hopSeconds / 0.2)));
        }
        heardSeconds += hopSeconds;
        sinceLetGo += hopSeconds;
        hearVoice (frameWeight);
        rememberFrame (treble, bass, total, frameWeight);
        checkForKeyChange();

        // Which chord is this frame closest to? Music tends to spend longest on its home chord.
        if (total > 1.0e-12 && ! voice)
        {
            std::array<double, 12> frame {};
            for (int i = 0; i < 12; ++i) frame[(size_t) i] = treble[(size_t) i] + bass[(size_t) i];
            standardise (frame);
            int bestChord = -1;
            double bestMatch = 0.55;
            for (int k = 0; k < 24; ++k)
            {
                const double m = correlate (frame, triadProfile[(size_t) k]);
                if (m > bestMatch) { bestMatch = m; bestChord = k; }
            }
            for (auto& c : chordMemory) c *= decay;
            if (bestChord >= 0) chordMemory[(size_t) bestChord] += frameWeight;
            frameChord[(size_t) ((historyPos - 1 + numHistory) % numHistory)] = bestChord;
        }

        if (openingHeard < knobs.openingSeconds && total > 1.0e-12)
        {
            for (int i = 0; i < 12; ++i)
                opening[(size_t) i] += frameWeight * (treble[(size_t) i] + 1.5 * bass[(size_t) i]) / total;
            openingHeard += hopSeconds;
        }

        trackChords (treble, bass);

        presence += (1.0 - presence) * (1.0 - std::exp (-hopSeconds / 0.4));
        scoreKeys();
        updateReading();
        publishChroma();
    }

    // What a lone voice needs. Runs on every frame whatever the source, so the answer is ready the moment it is wanted.
    void hearVoice (double frameWeight) noexcept
    {
        // The strongest note of the frame, from A2 up as in Single Voice, and each pitch class's share.
        std::array<double, 12> share {};
        double total = 0.0, top = 0.0;
        int topNote = -1;
        for (int n = 45 - lowestNote; n < numNotes; ++n)
        {
            const double a = notes[(size_t) n];
            total += a;
            share[(size_t) ((lowestNote + n) % 12)] += a;
            if (a > top) { top = a; topNote = n; }
        }
        const int topPc = topNote >= 0 && total > 1.0e-12 ? (lowestNote + topNote) % 12 : -1;

        // A held note: it stands clear in this frame and was the strongest note of the frame before too.
        // The longer it has been the strongest, the more it counts: a singer means the notes he stays on.
        const bool clear = topPc >= 0 && top >= knobs.heldShare * total;
        heldRun = clear ? (topPc == lastTopPc ? heldRun + 1 : 1) : 0;
        lastTopPc = clear ? topPc : -1;
        const bool held = heldRun >= 2;
        const double heldWeight = frameWeight * std::min (1.0, (heldRun - 1) / std::max (1.0, knobs.heldFull));
        const double decay = std::exp (-hopSeconds / knobs.tauVoice);
        for (auto& x : heldMemory) x *= decay;
        heldNow = 0.0;
        if (held)
        {
            heldMemory[(size_t) topPc] += heldWeight;
            // The recent notes are counted in seconds of held notes, not of time, so that sparse singing and a rap verse between
            // sung lines do not empty them.
            heldNow = heldWeight * hopSeconds;
            const double recentDecay = std::exp (-heldNow / knobs.recentVoice);
            for (auto& x : heldRecent) x *= recentDecay;
            heldRecent[(size_t) topPc] += heldWeight;
        }

        // One line or chords? Chord notes sound together for a second or more. In a tune the second strongest pitch class is the
        // note before or after, and is gone again within a few frames.
        auto& now = loneHistory[(size_t) lonePos];
        for (int i = 0; i < 12; ++i) now[(size_t) i] = total > 1.0e-12 ? share[(size_t) i] / total : 0.0;
        lonePos = (lonePos + 1) % maxLoneFrames;
        loneCount = std::min (loneCount + 1, maxLoneFrames);
        const int frames = std::clamp ((int) knobs.loneFrames, 1, maxLoneFrames);
        if (loneCount >= frames)
        {
            int sustained = 0;
            for (int i = 0; i < 12; ++i)
            {
                bool through = true;
                for (int f = 0; f < frames && through; ++f)
                    through = loneHistory[(size_t) ((lonePos - 1 - f + 2 * maxLoneFrames) % maxLoneFrames)][(size_t) i] >= knobs.loneShare;
                if (through) ++sustained;
            }
            chordShare += ((sustained >= 2 ? 1.0 : 0.0) - chordShare) * (1.0 - std::exp (-hopSeconds / knobs.loneTau));
        }
        if (knobs.loneInMix > 0.0 && knobs.voiceModel > 0.0)
        {
            if (! lone && chordShare < knobs.loneEnter) lone = true;
            else if (lone && chordShare > knobs.loneLeave) lone = false;
        }
        else lone = false;
    }

    // Read as a lone voice: Source Single Voice, or one line at a time heard in Full Mix.
    bool voicePath() const noexcept { return knobs.voiceModel > 0.0 && (source == Source::voice || lone); }

    static bool inSignature (int pitchClass, int key) noexcept
    {
        const int home = key < 12 ? key : (key - 12 + 3) % 12; // a minor key has the notes of its relative major
        const int step = (pitchClass - home + 12) % 12;
        return step == 0 || step == 2 || step == 4 || step == 5 || step == 7 || step == 9 || step == 11;
    }

    void rememberFrame (const std::array<double, 12>& treble, const std::array<double, 12>& bass, double total, double weight) noexcept
    {
        auto& f = frameHistory[(size_t) historyPos];
        for (int i = 0; i < 12; ++i)
            f[(size_t) i] = total > 1.0e-12 ? weight * (treble[(size_t) i] + 1.5 * bass[(size_t) i]) / total : 0.0;
        for (int i = 0; i < 12; ++i)
        {
            frameTreble[(size_t) historyPos][(size_t) i] = total > 1.0e-12 ? weight * treble[(size_t) i] / total : 0.0;
            frameBass[(size_t) historyPos][(size_t) i] = total > 1.0e-12 ? weight * bass[(size_t) i] / total : 0.0;
        }
        frameChord[(size_t) historyPos] = -1;
        frameWeights[(size_t) historyPos] = weight;
        historyPos = (historyPos + 1) % numHistory;
        historyCount = std::min (historyCount + 1, numHistory);
    }

    const std::array<double, 12>& frameBack (int framesAgo) const noexcept
    {
        return frameHistory[(size_t) ((historyPos - 1 - framesAgo + 2 * numHistory) % numHistory)];
    }

    // Minor counts both the natural and the raised sixth and seventh, as minor music uses both.
    double shareOutsideShownKey (const std::array<double, 12>& v) const noexcept
    {
        double inside = 0.0, total = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const int step = (i - current.key + 12) % 12;
            const bool member = current.minor ? (step == 0 || step == 2 || step == 3 || step == 5 || step == 7 || step >= 8)
                                              : (step == 0 || step == 2 || step == 4 || step == 5 || step == 7 || step == 9 || step == 11);
            total += v[(size_t) i];
            if (member) inside += v[(size_t) i];
        }
        return total > 1.0e-9 ? 1.0 - inside / total : 0.0;
    }

    // How many frames back the notes outside the shown key began, allowing short stretches of notes that fit both keys. -1 if none.
    int newNotesStart() const noexcept
    {
        int start = -1, gap = 0;
        const int maxGap = (int) (knobs.sectionMaxGap / hopSeconds);
        for (int back = 0; back < historyCount; ++back)
        {
            if (shareOutsideShownKey (frameBack (back)) > knobs.sectionShare) { start = back; gap = 0; }
            else if (++gap > maxGap) break;
        }
        return start;
    }

    // A lone tune has no chords to start a section on. Its notes wander over a section's first moments without saying much.
    bool hasBassLine() const noexcept
    {
        double t = 0.0, b = 0.0;
        for (int i = 0; i < 12; ++i) { t += trebleMemory[(size_t) i]; b += bassMemory[(size_t) i]; }
        return b > knobs.sectionBassShare * (t + b);
    }

    // A new key usually arrives on its home chord, or close to it, and that is what tells a loop from the relative key with the
    // same notes. Look back for where the outside notes began, and hear the chord there as the opening of a new section.
    void hearSectionStart() noexcept
    {
        if (! hasBassLine()) return;
        int start = -1, gap = 0;
        const int maxGap = (int) (knobs.sectionMaxGap / hopSeconds);
        for (int back = 0; back < historyCount; ++back)
        {
            if (shareOutsideShownKey (frameBack (back)) > knobs.sectionShare) { start = back; gap = 0; }
            else if (++gap > maxGap) break;
        }
        int skip = (int) knobs.sectionSkip;

        // A loop repeats its chord changes, so the first change not heard lately marks where something new began, even when
        // that chord still fits the old key. Only trusted when older changes were being repeated, so music that never
        // repeats is left alone.
        if (knobs.sectionNovel > 0.0)
        {
            const double limit = heardSeconds - (historyCount - 1) * hopSeconds;
            double oldestNew = -1.0;
            int repeats = 0;
            bool anchored = false, inRun = false;
            for (int n = 0; n < changeCount; ++n)
            {
                const auto& c = chordChanges[(size_t) ((changePos - 1 - n + 2 * numChanges) % numChanges)];
                // The new section's own loop may already be repeating: walk back past that to its first new changes.
                if (c.fresh) { oldestNew = c.time; repeats = 0; inRun = true; }
                else if (inRun && ++repeats >= 2) { anchored = true; break; }
            }
            if (oldestNew < limit) anchored = false;
            if (anchored && oldestNew >= 0.0)
            {
                const int back = (int) ((heardSeconds - oldestNew) / hopSeconds);
                if (back > start) { start = back; skip = 0; }
            }
        }
        if (start < 0) return;
        const int frames = (int) (knobs.openingSeconds / hopSeconds);
        opening.fill (0.0);
        for (int n = 0; n < frames; ++n)
        {
            const int back = start - skip - n;
            if (back < 0) break;
            const auto& f = frameBack (back);
            for (int i = 0; i < 12; ++i) opening[(size_t) i] += f[(size_t) i];
        }
        openingBorn = heardSeconds - start * hopSeconds;
    }

    // Notes that keep falling outside the key on show mean the key has changed. Keep only a little of the old memory.
    void checkForKeyChange() noexcept
    {
        if (current.key < 0) { outsideSeconds = 0.0; return; }
        if (voicePath())
        {
            // A singer strays outside the key for a note or a phrase without the song having moved. Only when most of the held
            // notes of the last while are outside it is the old memory let go.
            double inside = 0.0, total = 0.0;
            const int shown = current.key + (current.minor ? 12 : 0);
            for (int i = 0; i < 12; ++i) { total += heldRecent[(size_t) i]; if (inSignature (i, shown)) inside += heldRecent[(size_t) i]; }
            // And only when those notes make a key of their own: a rap verse or a spoken line leaves the key where it was.
            double recentFit = 0.0;
            for (int k = 0; k < 12 && total > 1.0e-12; ++k)
            {
                double in = 0.0;
                for (int i = 0; i < 12; ++i) if (inSignature (i, k)) in += heldRecent[(size_t) i];
                recentFit = std::max (recentFit, in / total);
            }
            const bool outside = total * hopSeconds >= knobs.voiceOutsideHeld && recentFit >= knobs.voiceOutsideFit && 1.0 - inside / total > knobs.voiceOutsideShare;
            outsideSeconds = outside ? outsideSeconds + heldNow : 0.0; // also counted in seconds of held notes
            if (outsideSeconds < knobs.voiceOutsideSeconds) return;
            outsideSeconds = 0.0;
            for (int i = 0; i < 12; ++i) heldMemory[(size_t) i] = heldRecent[(size_t) i] + knobs.voiceKeep * std::max (0.0, heldMemory[(size_t) i] - heldRecent[(size_t) i]);
            sinceLetGo = 0.0;
            lastMove = -1000.0;
            return;
        }
        const bool outside = shareOutsideShownKey (recentMemory) > knobs.outsideShare;
        outsideSeconds = outside ? outsideSeconds + hopSeconds : 0.0;
        if (outsideSeconds < (response == Response::fast ? knobs.outsideFast : knobs.outsideStable))
            return;
        outsideSeconds = 0.0;
        const int start = knobs.rebuildOnLetGo > 0.0 ? newNotesStart() : -1;
        if (start >= 0)
        {
            // What was heard since the new notes began stays whole. Only the part of each memory from before then is mostly dropped.
            const double decay = std::exp (-hopSeconds / (response == Response::fast ? knobs.tauFast : knobs.tauStable));
            std::array<double, 12> t {}, b {};
            std::array<double, 24> c {};
            double w = 1.0;
            for (int back = 0; back <= start; ++back)
            {
                const int at = (historyPos - 1 - back + 2 * numHistory) % numHistory;
                for (int i = 0; i < 12; ++i) { t[(size_t) i] += w * frameTreble[(size_t) at][(size_t) i]; b[(size_t) i] += w * frameBass[(size_t) at][(size_t) i]; }
                if (frameChord[(size_t) at] >= 0) c[(size_t) frameChord[(size_t) at]] += w * frameWeights[(size_t) at];
                w *= decay;
            }
            for (int i = 0; i < 12; ++i)
            {
                trebleMemory[(size_t) i] = t[(size_t) i] + 0.15 * std::max (0.0, trebleMemory[(size_t) i] - t[(size_t) i]);
                bassMemory[(size_t) i] = b[(size_t) i] + 0.15 * std::max (0.0, bassMemory[(size_t) i] - b[(size_t) i]);
            }
            for (int k = 0; k < 24; ++k)
                chordMemory[(size_t) k] = c[(size_t) k] + 0.15 * std::max (0.0, chordMemory[(size_t) k] - c[(size_t) k]);
        }
        else
        {
            for (auto& x : trebleMemory) x *= 0.15;
            for (auto& x : bassMemory) x *= 0.15;
            for (auto& x : chordMemory) x *= 0.15;
        }
        cadence.fill (0.0);
        opening.fill (0.0); // the opening chord belonged to the old key
        openingHeard = knobs.openingSeconds;
        if (knobs.sectionLookback > 0.0) hearSectionStart();
        sinceLetGo = 0.0;
        lastMove = -1000.0;
    }

    // Follows the chord sounding now, and notes each major chord that falls a fifth to the next chord.
    void trackChords (const std::array<double, 12>& treble, const std::array<double, 12>& bass) noexcept
    {
        const double fade = std::exp (-hopSeconds / knobs.cadenceTau);
        for (auto& c : cadence) c *= fade;
        if (source == Source::voice) return;

        std::array<double, 12> frame {};
        for (int i = 0; i < 12; ++i) frame[(size_t) i] = treble[(size_t) i] + bass[(size_t) i];
        double best = 0.0;
        int chord = -1;
        for (int k = 0; k < 24; ++k)
        {
            const int root = k % 12, third = (root + (k >= 12 ? 3 : 4)) % 12, fifth = (root + 7) % 12;
            const double s = frame[(size_t) root] * 1.2 + frame[(size_t) third] + frame[(size_t) fifth] + bass[(size_t) root];
            if (s > best) { best = s; chord = k; }
        }
        if (chord < 0) return;
        if (chord == pendingChord) ++pendingFrames;
        else { pendingChord = chord; pendingFrames = 1; }
        if (pendingFrames < 2 || chord == heldChord) return;

        {
            auto& h = heldHistory[(size_t) heldPos];
            h.chord = chord;
            h.start = heardSeconds - (pendingFrames - 1) * hopSeconds;
            h.fresh = heldChord < 0 || heardSeconds - transitionHeard[(size_t) heldChord][(size_t) chord] > knobs.transitionMemory;
            heldPos = (heldPos + 1) % numChanges;
            heldCount = std::min (heldCount + 1, numChanges);
        }

        if (heldChord >= 0 && heldChord < 12 && (heldChord % 12 + 5) % 12 == chord % 12)
            cadence[(size_t) chord] += 1.0;
        if (heldChord >= 0)
        {
            auto& heard = transitionHeard[(size_t) heldChord][(size_t) chord];
            auto& change = chordChanges[(size_t) changePos];
            change.time = heardSeconds - (pendingFrames - 1) * hopSeconds;
            change.fresh = heardSeconds - heard > knobs.transitionMemory;
            heard = heardSeconds;
            const bool fresh = change.fresh;
            changePos = (changePos + 1) % numChanges;
            changeCount = std::min (changeCount + 1, numChanges);
            // A loop had settled, and now several changes in a row are new: the song has moved on, maybe with no new notes at all.
            if (fresh) ++freshRun;
            else if (++repeatRun >= 2) { freshRun = 0; repeatRun = 0; loopSettled = true; }
            if (fresh && freshRun >= 2) repeatRun = 0;
            if (knobs.sectionOnLoop > 0.0 && loopSettled && freshRun >= (int) knobs.sectionOnLoop && hasBassLine())
            {
                loopSettled = false;
                hearSectionStart();
                for (auto& c : chordMemory) c *= knobs.loopLetGo;
                for (auto& x : trebleMemory) x *= knobs.loopNotes;
                for (auto& x : bassMemory) x *= knobs.loopNotes;
                cadence.fill (0.0);
                if (knobs.loopNotes < 1.0) { sinceLetGo = 0.0; lastMove = -1000.0; }
            }
        }
        heldChord = chord;
    }

    const HeldChord& heldBack (int n) const noexcept { return heldHistory[(size_t) ((heldPos - 1 - n + 2 * numChanges) % numChanges)]; }

    // Is this chord (0 to 11 major triads, 12 to 23 minor) one of the key's own? Minor also has the major chord on its fifth.
    static bool chordInKey (int chord, int key) noexcept
    {
        const int t = key % 12, root = (chord % 12 - t + 12) % 12;
        const bool chordMinor = chord >= 12;
        if (key < 12) return chordMinor ? (root == 2 || root == 4 || root == 9) : (root == 0 || root == 5 || root == 7);
        return chordMinor ? (root == 0 || root == 5 || root == 7) : (root == 3 || root == 8 || root == 10 || root == 7);
    }

    // Harmony since the current section began. A section starts at the first run of chord changes not heard lately, walking
    // back past the new loop's own repeats, as in hearSectionStart. For each key: the share of the section's time on chords
    // outside it, whether the section opened on its home chord, and the share of time on that chord.
    void hearHarmony() noexcept
    {
        harmony.fill (0.0);
        if (heldCount < 2 || ! hasBassLine()) return;
        int start = -1, repeats = 0;
        bool inRun = false;
        for (int n = 0; n < heldCount; ++n)
        {
            if (heldBack (n).fresh) { start = n; repeats = 0; inRun = true; }
            else if (inRun && ++repeats >= 2) break;
        }
        if (start < 0) start = heldCount - 1;
        const double changes = start;
        const double ramp = knobs.harmonyRamp > 0.0 ? std::clamp (changes / knobs.harmonyRamp, 0.0, 1.0) : 1.0;
        if (ramp <= 0.0) return;
        std::array<double, 24> time {};
        double total = 0.0;
        for (int n = start; n >= 0; --n)
        {
            const auto& h = heldBack (n);
            const double end = n == 0 ? heardSeconds : heldBack (n - 1).start;
            const double d = std::max (0.0, end - h.start);
            time[(size_t) h.chord] += d;
            total += d;
        }
        if (total <= 0.0) return;
        const int first = heldBack (start).chord;
        for (int k = 0; k < 24; ++k)
        {
            double outside = 0.0;
            for (int c = 0; c < 24; ++c)
                if (time[(size_t) c] > 0.0 && ! chordInKey (c, k)) outside += time[(size_t) c];
            double e = -knobs.harmonyFit * outside / total;
            if (first == k)
            {
                const double needFit = response == Response::stable ? knobs.harmonyFirstFitStable : 0.0;
                e += knobs.harmonyFirst * std::max (0.0, 1.0 - needFit * outside / total);
            }
            e += knobs.harmonyHome * time[(size_t) k] / total;
            harmony[(size_t) k] = ramp * e;
        }
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

        if (voicePath())
        {
            const auto h = standardised (heldMemory, knobs.chromaPower, nullptr);
            double heldTotal = 0.0;
            for (auto x : heldMemory) heldTotal += x;
            heldSeconds = heldTotal * hopSeconds;
            double bestFit = 0.0;
            for (int k = 0; k < 24; ++k)
            {
                // The 7 notes come from the even profile, the same for a key and its relative. Which of the two is home comes
                // from the listener ratings, which are about exactly that: where a tune rests.
                const int r = relativeOf (k);
                double s = correlate (h, voiceProfile[(size_t) k]);
                if (knobs.voiceHome > 0.0)
                    s = std::max (s, correlate (h, voiceProfile[(size_t) r]))
                        + knobs.voiceHome * 0.5 * (correlate (h, keyProfile[(size_t) k]) - correlate (h, keyProfile[(size_t) r]));
                if (heldTotal > 1.0e-12)
                {
                    double outside = 0.0;
                    for (int i = 0; i < 12; ++i) if (! inSignature (i, k)) outside += heldMemory[(size_t) i];
                    s -= knobs.voiceOutsideWeight * outside / heldTotal;
                    bestFit = std::max (bestFit, 1.0 - outside / heldTotal);
                }
                scores[(size_t) k] = s;
            }
            // Held notes that fit no key's seven notes are speech, rap or a song between keys: no reading to trust.
            voiceFit = std::clamp ((bestFit - knobs.voiceFitLow) / (knobs.voiceFitHigh - knobs.voiceFitLow), 0.0, 1.0);
            double best = -1.0e9, sum = 0.0;
            for (auto s : scores) best = std::max (best, s);
            for (int k = 0; k < 24; ++k) { probs[(size_t) k] = std::exp (knobs.softness * (scores[(size_t) k] - best)); sum += probs[(size_t) k]; }
            for (auto& p : probs) p /= sum;
            return;
        }

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
        const double openingAge = std::max (0.0, heardSeconds - openingBorn - knobs.openingSeconds);
        const double bassShare = std::clamp ((rawBass / std::max (rawTreble + rawBass, 1.0e-12) - 0.15) / (knobs.openingBassShare - 0.15), 0.0, 1.0);
        const double openingGain = source == Source::voice ? 0.0 : (knobs.openingWeight + knobs.openingBassWeight * bassShare) * std::exp (-openingAge / (knobs.openingTau + knobs.openingBassTau * bassShare));
        const double bassGain = source == Source::voice ? 0.0 : knobs.bassWeight * bassScale;

        double chordTotal = 0.0;
        for (auto c : chordMemory) chordTotal += c;
        const double chordGain = source == Source::voice || chordTotal <= 1.0e-9 ? 0.0 : knobs.homeChordWeight / chordTotal;

        double heardTotal = 0.0;
        for (int i = 0; i < 12; ++i) heardTotal += trebleMemory[(size_t) i] + bassMemory[(size_t) i];
        const double harmonyGain = source == Source::voice ? 0.0 : (response == Response::fast ? knobs.harmonyFast : knobs.harmonyStable);
        if (harmonyGain > 0.0) hearHarmony();
        for (int k = 0; k < 24; ++k)
        {
            double s = correlate (t, keyProfile[(size_t) k]);
            if (harmonyGain > 0.0) s += harmonyGain * harmony[(size_t) k];
            s += chordGain * chordMemory[(size_t) k];
            if (knobs.outsideWeight > 0.0 && heardTotal > 1.0e-12)
            {
                // The listener profiles barely mind a note from outside the scale. One clear outside note is what tells keys a fifth apart.
                double outside = 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const int step = (i - k % 12 + 12) % 12;
                    const double away = k < 12 ? ((step == 0 || step == 2 || step == 4 || step == 5 || step == 7 || step == 9 || step == 11) ? 0.0 : 1.0)
                                               : ((step == 0 || step == 2 || step == 3 || step == 5 || step == 7 || step == 8 || step == 10 || step == 11) ? 0.0 : (step == 9 ? 0.5 : 1.0));
                    outside += away * (trebleMemory[(size_t) i] + bassMemory[(size_t) i]);
                }
                s -= knobs.outsideWeight * outside / heardTotal;
            }
            if (bassGain > 0.0) s += bassGain * correlate (b, bassProfile[(size_t) k]);
            // The opening chord helps choose between a key and its relative, which share the same notes.
            if (openingGain > 0.0) s += openingGain * 0.5 * (correlate (o, triadProfile[(size_t) k]) - correlate (o, triadProfile[(size_t) relativeOf (k)]));
            if (source != Source::voice) s += knobs.cadenceWeight * std::min (1.0, cadence[(size_t) k] / knobs.cadenceFull);
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
        if (voicePath())
            return voiceFit * voiceFit * (3.0 - 2.0 * voiceFit) * std::clamp (heldSeconds / std::max (knobs.voiceMinHeld, 1.0e-9), 0.0, 1.0);
        // Flat chroma, such as from noise, gives no trust whatever the scores say.
        const double x = std::clamp ((contrast - 0.35) / 0.25, 0.0, 1.0);
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

        for (size_t k = 0; k < 24; ++k)
            current.candidates[k] = (float) (probs[k] * gate * presence);

        if (shown < 0)
        {
            const double first = stable ? knobs.firstStable : knobs.firstFast;
            // Wait while the leader is close to any key other than its own relative. Give up waiting at twice the normal time.
            double rival = -1.0e9;
            for (int k = 0; k < 24; ++k)
                if (k != top && k != relativeOf (top)) rival = std::max (rival, scores[(size_t) k]);
            const double needLead = stable ? knobs.firstLeadStable : knobs.firstLeadFast;
            const double needRelative = stable ? knobs.firstRelativeLeadStable : 0.0;
            const bool clear = (scores[(size_t) top] - rival >= needLead && scores[(size_t) top] - scores[(size_t) relativeOf (top)] >= needRelative)
                               || heardSeconds >= first * 2.0;
            // In Full Mix, a sound that looks like one line but is not yet known to be one gets no reading as chords meanwhile.
            const bool undecided = knobs.voiceModel > 0.0 && knobs.loneInMix > 0.0 && source == Source::mix && ! lone && chordShare < knobs.loneWait;
            if (heardSeconds >= first && gate > 0.5 && clear && ! undecided && (! voicePath() || heldSeconds >= knobs.voiceMinHeld))
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
            const double settled = std::clamp (std::min (heardSeconds, sinceLetGo) / knobs.settleSeconds, 0.25, 1.0);
            // A move to the relative key adds no new notes, so it needs much stronger evidence than a move to anything else.
            const double guard = top == relativeOf (shown) ? knobs.relativeGuard : 1.0;
            const double need = (stable ? knobs.dwellStable : knobs.dwellFast) * settled * guard;
            // Going back to the key just left needs a much clearer lead, which is what stops a reading bouncing.
            const double back = top == previousShown ? knobs.returnGuard : 1.0;
            // A lone voice gives thinner evidence than chords and a bass line, so a new key must lead it by more.
            const double margin = (stable ? knobs.marginStable : knobs.marginFast) * settled * guard * back * (voicePath() ? knobs.voiceMargin : 1.0);
            const double holdFor = (stable ? knobs.holdStable : knobs.holdFast) * (knobs.holdAfterLetGo > 0.0 ? std::clamp (sinceLetGo / knobs.settleSeconds, 0.25, 1.0) : 1.0);
            const bool holding = heardSeconds - lastMove < holdFor;
            if (! holding && dwell >= need && scores[(size_t) top] - scores[(size_t) shown] >= margin && gate > 0.5)
            {
                // A new set of notes won without ever going far outside the old key, so no section start has been heard yet.
                if (knobs.sectionOnMove > 0.0 && top != relativeOf (shown) && sinceLetGo > knobs.settleSeconds && heardSeconds - openingBorn > knobs.settleSeconds)
                {
                    hearSectionStart();
                    scoreKeys();
                    if (scores[(size_t) relativeOf (top)] > scores[(size_t) top]) top = relativeOf (top);
                }
                lastMove = heardSeconds;
                previousShown = shown;
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
            // For a lone voice the confidence is in the 7 notes, which the key and its relative share: nothing a tune holds tells
            // those two apart for sure, and what follows this reading needs the notes.
            const double sure = voicePath() ? probs[(size_t) shown] + probs[(size_t) relativeOf (shown)] : probs[(size_t) shown];
            const double target = sure * gate * presence;
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
        for (auto& c : current.candidates) c *= (float) f;
        const float noteFade = (float) std::exp (-seconds / 0.2);
        for (auto& n : current.liveNotes) n *= noteFade;
    }

    // After a long silence the next audio may be another song. Weaken the old memory and listen for an opening chord again.
    void startAfresh() noexcept
    {
        for (auto& x : trebleMemory) x *= 0.25;
        for (auto& x : bassMemory) x *= 0.25;
        for (auto& x : chordMemory) x *= 0.25;
        for (auto& x : heldMemory) x *= 0.25;
        opening.fill (0.0);
        openingHeard = 0.0;
       
        cadence.fill (0.0);
        heldChord = pendingChord = -1;
        pendingFrames = 0;
        heardSeconds = std::min (heardSeconds, knobs.openingSeconds);
        openingBorn = 0.0;
        historyCount = 0;
        changeCount = 0;
        heldPos = heldCount = 0;
        freshRun = repeatRun = 0; loopSettled = false;
    }

    void resetTempo() noexcept
    {
        std::fill (lastShort.begin(), lastShort.end(), 0.0);
        std::fill (onsets.begin(), onsets.end(), 0.0);
        onsetPos = onsetCount = tempoCounter = tempoEstimateCounter = 0;
    }

    Knobs knobs;
    Fft fft;
    std::vector<double> window, ring, re, im, mag;
    std::vector<float> firTaps, firHistory;
    std::vector<double> logFreqs, logSpec, logClean, notes, atoms, wtv, wtw;
    int activeAtoms = numNotes;
    std::vector<float> dict;
    Fft shortFft;
    std::vector<double> shortWindow, shortRe, shortIm, lastShort, onsets, onsetWork;
    int onsetPos = 0, onsetCount = 0, tempoCounter = 0, tempoEstimateCounter = 0;
    std::array<std::array<double, 12>, 24> keyProfile {}, bassProfile {}, triadProfile {};

    double hostRate = 48000.0, outRate = 12000.0, hopSeconds = 0.17, binHz = 1.46, hannSum = 1.0;
    int decimation = 4, numTaps = 129, firPos = 0, decimCounter = 0;
    int ringPos = 0, hopCounter = 0, windowFilled = 0, maxBin = 3000;

    Response response = Response::stable;
    Source source = Source::mix;

    std::array<double, 12> trebleMemory {}, bassMemory {}, opening {}, recentMemory {};
    static constexpr int maxLoneFrames = 12;
    std::array<double, 12> heldMemory {}, heldRecent {};                       // a lone voice's held notes: about the last minute, and the recent ones
    std::array<std::array<double, 12>, maxLoneFrames> loneHistory {};         // each pitch class's share of the last frames
    std::array<std::array<double, 12>, 24> voiceProfile {};
    int lastTopPc = -1, heldRun = 0, lonePos = 0, loneCount = 0;
    double heldSeconds = 0.0, heldNow = 0.0, voiceFit = 0.0, chordShare = 1.0, tunWeight = 0.0;
    bool lone = false;
    std::array<std::array<double, 12>, numHistory> frameHistory {}, frameTreble {}, frameBass {};
    std::array<int, numHistory> frameChord {};
    std::array<double, numHistory> frameWeights {};
    int historyPos = 0, historyCount = 0;
    double openingBorn = 0.0;
    struct ChordChange { double time = 0.0; bool fresh = false; };
    std::array<HeldChord, numChanges> heldHistory {};
    int heldPos = 0, heldCount = 0;
    std::array<double, 24> harmony {};
    std::array<ChordChange, numChanges> chordChanges {};
    std::array<std::array<double, 24>, 24> transitionHeard {};
    int changePos = 0, changeCount = 0, freshRun = 0, repeatRun = 0;
    bool loopSettled = false;
    double outsideSeconds = 0.0, sinceLetGo = 1.0e9;
    std::array<double, 24> scores {}, probs {}, chordMemory {};
    double heardSeconds = 0.0, openingHeard = 0.0, silenceSeconds = 0.0, presence = 0.0, contrast = 0.0;
    double tunRe = 0.0, tunIm = 0.0, tuning = 0.0, loudest = 0.0;
    int candidate = -1, previousShown = -1;
    double lastMove = -1000.0;
    std::array<double, 24> cadence {};
    int heldChord = -1, pendingChord = -1, pendingFrames = 0;
    double dwell = 0.0, smoothedConfidence = 0.0;
    Reading current;
};
} // namespace smoothkey
