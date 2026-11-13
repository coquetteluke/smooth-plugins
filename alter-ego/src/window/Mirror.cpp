#include "Mirror.h"

using namespace ui;

namespace
{
    constexpr float floorDb = -68.0f, topDb = -12.0f;
    constexpr float tiltPerOctave = 4.0f;      // voices fall off towards the top, and the humps up there are half the story
    constexpr float tiltFromHz = 200.0f;
    constexpr float attack = 0.8f, release = 0.34f;

    juce::String noteOf (double hz)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int midi = juce::roundToInt (69.0 + 12.0 * std::log2 (hz / 440.0));
        return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }
}

// ---- the note at the top of each side. Its own opaque little window, so a new number repaints nothing else --------

class Mirror::Readout final : public juce::Component
{
public:
    Readout (const char* nameText, juce::Colour noteColour, bool alignRight)
        : name (nameText), colour (noteColour), right (alignRight)
    {
        setOpaque (true);
        setInterceptsMouseClicks (false, false);
    }

    void setNoteColour (juce::Colour c)
    {
        if (c != colour)
        {
            colour = c;
            repaint();
        }
    }

    void show (double hz)
    {
        const juce::String newNote = hz > 0.0 ? noteOf (hz) : juce::String();
        const juce::String newHz = hz > 0.0 ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String ("No pitch");
        if (newNote != note || newHz != rate)
        {
            note = newNote;
            rate = newHz;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (tone::well);
        auto b = getLocalBounds().toFloat();
        const auto align = right ? juce::Justification::centredRight : juce::Justification::centredLeft;
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawText (name, b.removeFromTop (18.0f), align, false);
        g.setColour (colour);
        g.setFont (font (Weight::medium, 34.0f));
        g.drawText (note, b.removeFromTop (40.0f), align, false);
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f, true));
        g.drawText (rate, b, align, false);
    }

private:
    juce::String name, note { "" }, rate;
    juce::Colour colour;
    bool right;
};

// ---- the mirror ---------------------------------------------------------------------------------------------------

Mirror::Mirror (PluginProcessor& p) : processorRef (p)
{
    setOpaque (true);
    setTitle ("Your voice and the voice coming out");
    setComponentID ("mirror");
    hint = "You on the left, what comes out on the right.";
    youReadout = std::make_unique<Readout> ("You", tone::you, false);
    egoReadout = std::make_unique<Readout> ("Alter Ego", tone::acid, true);
    addAndMakeVisible (*youReadout);
    addAndMakeVisible (*egoReadout);
    youReadout->show (0.0);
    egoReadout->show (0.0);
    youPath.preallocateSpace (numPoints * 3 + 8);
    egoPath.preallocateSpace (numPoints * 3 + 8);
    prepareAnalysis (processorRef.getTap().sampleRate());
    // Only audio that comes in from now on is news. Starting from -1, a window opened with the host stopped took what the read still held
    // from the last window as new, and showed that voice and the engine's last readings (1.2.6).
    lastPosition = processorRef.getTap().position();
}

Mirror::~Mirror() = default;

void Mirror::prepareAnalysis (double sampleRate)
{
    analysedRate = sampleRate;
    const int wanted = sampleRate <= 50000.0 ? 4096 : 8192;   // long enough to part the harmonics of a very low voice

    if (wanted != fftSize)
    {
        fftSize = wanted;
        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) fftSize)));
        hann.resize ((size_t) fftSize);
        grabbed.resize ((size_t) fftSize);
        spectrum.assign ((size_t) fftSize * 2, 0.0f);
        double sum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            hann[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sum += hann[(size_t) i];
        }
        for (auto& w : hann)
            w = (float) (w * 2.0 / sum);   // a sine of level 1 reads as 1, whatever the size
    }

    const double hzPerBin = sampleRate / fftSize;
    const double step = std::log2 (highestHz / lowestHz) / (numPoints - 1);

    for (int i = 0; i < numPoints; ++i)
    {
        const double hz = lowestHz * std::pow (2.0, step * i);
        const double low = hz * std::pow (2.0, -0.5 * step) / hzPerBin, high = hz * std::pow (2.0, 0.5 * step) / hzPerBin;
        auto& band = bands[(size_t) i];
        band.from = juce::jlimit (1, fftSize / 2 - 1, (int) std::ceil (low));
        band.to = juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (high));
        // Wider than a bin: the loudest in the band. Narrower: read between the two bins either side.
        band.weight = (float) (hz / hzPerBin - std::floor (hz / hzPerBin));
        band.low = (float) juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (hz / hzPerBin));
        band.high = (float) juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (hz / hzPerBin) + 1);
        band.tilt = hz > tiltFromHz ? tiltPerOctave * (float) std::log2 (hz / tiltFromHz) : 0.0f;
    }
}

void Mirror::measure (long long position)
{
    auto& tap = processorRef.getTap();

    const int smoothFromPoint = juce::roundToInt (std::log2 (1500.0 / lowestHz) / std::log2 (highestHz / lowestHz) * (numPoints - 1));

    const auto analyse = [this, smoothFromPoint] (std::array<float, numPoints>& wanted)
    {
        // Silence needs no maths. A host playing an empty track costs next to nothing.
        float peak = 0.0f;
        for (auto v : grabbed)
            peak = juce::jmax (peak, std::abs (v));
        if (peak < 1.0e-5f)
        {
            wanted.fill (0.0f);
            return;
        }

        std::array<float, numPoints> raw;

        for (int i = 0; i < fftSize; ++i)
            spectrum[(size_t) i] = grabbed[(size_t) i] * hann[(size_t) i];
        std::fill (spectrum.begin() + fftSize, spectrum.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (spectrum.data(), true);

        for (int i = 0; i < numPoints; ++i)
        {
            const auto& band = bands[(size_t) i];
            float level = 0.0f;
            if (band.to >= band.from)
            {
                for (int k = band.from; k <= band.to; ++k)
                    level = juce::jmax (level, spectrum[(size_t) k]);
            }
            else
            {
                const float a = spectrum[(size_t) (int) band.low], b = spectrum[(size_t) (int) band.high];
                level = a + (b - a) * band.weight;
            }
            const float db = 20.0f * std::log10 (level + 1.0e-7f) + band.tilt;
            raw[(size_t) i] = juce::jlimit (0.0f, 1.0f, (db - floorDb) / (topDb - floorDb));
        }

        // Above the harmonics a band is only noise from one look to the next. A little smoothing up there leaves the humps and loses the fuzz.
        for (int i = 0; i < numPoints; ++i)
        {
            const float above = juce::jlimit (0.0f, 1.0f, (float) (i - smoothFromPoint) / 12.0f);
            const float around = 0.25f * raw[(size_t) juce::jmax (0, i - 1)] + 0.5f * raw[(size_t) i] + 0.25f * raw[(size_t) juce::jmin (numPoints - 1, i + 1)];
            wanted[(size_t) i] = raw[(size_t) i] + (around - raw[(size_t) i]) * above;
        }
    };

    tap.readInput (position, grabbed.data(), fftSize);
    analyse (youWanted);

    tap.readOutput (position, grabbed.data(), fftSize);
    float peak = 0.0f;
    for (int i = fftSize - fftSize / 4; i < fftSize; ++i)
        peak = juce::jmax (peak, std::abs (grabbed[(size_t) i]));
    outputLevel = juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (peak + 1.0e-6f) + 54.0f) / 48.0f);
    analyse (egoWanted);
}

bool Mirror::advance()
{
    auto& tap = processorRef.getTap();
    const double rate = tap.sampleRate();
    if (rate != analysedRate)
        prepareAnalysis (rate);

    const auto position = tap.position();
    const bool newAudio = position != lastPosition && position > 0;
    lastPosition = position;
    const bool nowBypassed = processorRef.apvts.getRawParameterValue ("bypass")->load (std::memory_order_relaxed) >= 0.5f;
    if (nowBypassed != bypassed)
    {
        // Bypassed, the right side is you as well, so it wears your colour.
        bypassed = nowBypassed;
        layerValid = false;
        pathsValid = false;
        egoReadout->setNoteColour (bypassed ? tone::you : tone::acid);
        repaint();
    }

    // A host with big buffers delivers less often than this draws, so a frame or two with nothing new doesn't mean it stopped.
    staleFrames = newAudio ? 0 : juce::jmin (1000, staleFrames + 1);
    const bool fresh = staleFrames < 8;

    if (newAudio)
    {
        measure (position);
    }
    else if (! fresh)
    {
        youWanted.fill (0.0f);
        egoWanted.fill (0.0f);
        outputLevel = 0.0f;
    }

    // Each side moves the same way, so the same voice on both sides stays the same shape.
    float move = 0.0f, loudest = 0.0f;
    for (int i = 0; i < numPoints; ++i)
    {
        const auto step = [] (float& shown, float wanted)
        {
            shown += (wanted - shown) * (wanted > shown ? attack : release);
            if (shown < 0.002f)
                shown = 0.0f;
        };
        step (youShape[(size_t) i], youWanted[(size_t) i]);
        step (egoShape[(size_t) i], egoWanted[(size_t) i]);
        move = juce::jmax (move, std::abs (youShape[(size_t) i] - youPainted[(size_t) i]), std::abs (egoShape[(size_t) i] - egoPainted[(size_t) i]));
        loudest = juce::jmax (loudest, youShape[(size_t) i], egoShape[(size_t) i]);
    }

    const auto& engine = processorRef.getEngine();
    const double inNow = fresh ? (double) engine.detectedHz() : 0.0;
    const double outNow = fresh ? (bypassed ? inNow : (double) engine.outputHz()) : 0.0;
    // Voiced is the engine's word, not the output's level: a soft take with Output turned down lost both readouts, the disc and the line
    // under -54 dBFS though the engine was tracking it (1.2.6). Such a voice still gets a small disc.
    voiced = inNow > 0.0 && outNow > 0.0;
    if (voiced)
        outputLevel = juce::jmax (outputLevel, 0.08f);
    quietVoiceFrames = voiced ? 0 : juce::jmin (1000, quietVoiceFrames + 1);
    if (voiced)
        liveShift = 12.0 * std::log2 (outNow / inNow);

    const auto glide = [this] (double& shown, double wanted)
    {
        if (! voiced)
            shown = 0.0;
        else if (shown <= 0.0)
            shown = wanted;
        else
            shown = std::pow (2.0, std::log2 (shown) + 0.75 * (std::log2 (wanted) - std::log2 (shown)));
    };
    glide (youHz, inNow);
    glide (egoHz, outNow);

    bridgeYou = youHz > 0.0 ? yFor (youHz) : -1.0f;
    bridgeEgo = egoHz > 0.0 ? yFor (egoHz) : -1.0f;

    const bool bridgeMoved = std::abs (bridgeYou - paintedBridgeYou) > 0.25f || std::abs (bridgeEgo - paintedBridgeEgo) > 0.25f;
    const bool shapeMoved = move * reach > 0.25f;

    // A silent window says what to do, and only asks for the picture to change when that flips.
    quietFrames = loudest > 0.02f ? 0 : juce::jmin (1000, quietFrames + 1);
    const bool nowHeard = quietFrames < 15;
    if (nowHeard != heard)
    {
        heard = nowHeard;
        layerValid = false;
        repaint (plot.getSmallestIntegerContainer());
    }

    if (shapeMoved || bridgeMoved)
    {
        pathsValid = false;
        youPainted = youShape;
        egoPainted = egoShape;
        paintedBridgeYou = bridgeYou;
        paintedBridgeEgo = bridgeEgo;
        repaint (plot.getSmallestIntegerContainer().expanded (1));
    }

    // Numbers change at a calm pace. They are the most expensive thing here and nobody can read them any faster.
    if (--readoutCounter <= 0)
    {
        readoutCounter = 4;
        // Gaps between words are not news either: the last note stays until the voice has really gone. Nor is 1 Hz of wobble.
        const auto update = [] (Readout& readout, double hz, double& shown, bool gone)
        {
            if (hz > 0.0 ? std::abs (hz - shown) >= 1.0 : (gone && shown > 0.0))
            {
                shown = hz;
                readout.show (hz);
            }
        };
        const bool gone = quietVoiceFrames >= 15;
        update (*youReadout, youHz, youShownHz, gone);
        update (*egoReadout, egoHz, egoShownHz, gone);
    }

    return loudest > 0.0f || shapeMoved || bridgeMoved;   // a host playing silence still looks in, at the slow rate
}

float Mirror::yFor (double hz) const noexcept
{
    const double p = std::log2 (juce::jlimit (lowestHz, highestHz, hz) / lowestHz) / std::log2 (highestHz / lowestHz);
    return plot.getBottom() - (float) p * plot.getHeight();
}

void Mirror::resized()
{
    auto b = getLocalBounds().reduced (18, 16);
    auto head = b.removeFromTop (78);
    youReadout->setBounds (head.removeFromLeft (head.getWidth() / 2));
    egoReadout->setBounds (head);
    b.removeFromTop (10);
    plot = b.toFloat();
    axisX = plot.getCentreX();
    reach = plot.getWidth() * 0.5f - gutter * 0.5f;
    layerValid = false;
    pathsValid = false;
}

void Mirror::buildPaths()
{
    if (pathsValid)
        return;

    const float height = plot.getHeight() / (float) (numPoints - 1);
    const float youEdge = axisX - gutter * 0.5f, egoEdge = axisX + gutter * 0.5f;

    const auto build = [&] (juce::Path& path, const std::array<float, numPoints>& shape, float edge, float direction)
    {
        path.clear();
        path.startNewSubPath (edge, plot.getBottom());
        float last = 0.0f;
        for (int i = 0; i < numPoints; ++i)
        {
            const float x = shape[(size_t) i] * reach;
            // Runs of nearly the same width are one edge, not many.
            if (i == 0 || i == numPoints - 1 || std::abs (x - last) > 0.3f)
            {
                path.lineTo (edge + direction * x, plot.getBottom() - (float) i * height);
                last = x;
            }
        }
        path.lineTo (edge, plot.getY());
        path.closeSubPath();
    };

    build (youPath, youShape, youEdge, -1.0f);
    build (egoPath, egoShape, egoEdge, 1.0f);
    pathsValid = true;
}

void Mirror::drawLayer (juce::Graphics& g) const
{
    const auto b = getLocalBounds().toFloat();
    g.fillAll (tone::base);
    g.setColour (tone::well);
    g.fillRoundedRectangle (b, 14.0f);

    // Rungs up the ladder, written in the gap between the two voices.
    const std::pair<double, const char*> rungs[] = { { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" }, { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" } };
    g.setFont (font (Weight::regular, 11.0f));

    for (const auto& rung : rungs)
    {
        const float y = yFor (rung.first);
        g.setColour (tone::text.withAlpha (0.1f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.5f, reach, 1.0f));
        g.fillRect (juce::Rectangle<float> (axisX + gutter * 0.5f, y - 0.5f, reach, 1.0f));
        g.setColour (tone::faint);
        g.drawText (rung.second, juce::Rectangle<float> (axisX - gutter * 0.5f, y - 8.0f, gutter, 16.0f), juce::Justification::centred, false);
    }

    // The two edges of the mirror.
    g.setColour (tone::you.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<float> (axisX - gutter * 0.5f - 1.0f, plot.getY(), 1.0f, plot.getHeight()));
    g.setColour ((bypassed ? tone::you : tone::acid).withAlpha (0.5f));
    g.fillRect (juce::Rectangle<float> (axisX + gutter * 0.5f, plot.getY(), 1.0f, plot.getHeight()));

    if (! heard)
    {
        // Direction, written where the voice would be.
        const auto note = juce::Rectangle<float> (plot.getWidth(), 56.0f).withCentre (plot.getCentre());
        g.setColour (tone::well);
        g.fillRect (note);
        g.setColour (tone::muted);
        g.setFont (font (Weight::regular, 13.0f));
        g.drawFittedText ("Play a voice and it\nshows up in the mirror.", note.toNearestInt(), juce::Justification::centred, 2, 1.0f);
    }
}

void Mirror::paint (juce::Graphics& g)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const float scale = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());

    if (! layerValid || std::abs (scale - layerScale) > 0.001f || layer.isNull())
    {
        const int w = juce::jmax (1, juce::roundToInt ((float) getWidth() * scale)), h = juce::jmax (1, juce::roundToInt ((float) getHeight() * scale));
        if (layer.isNull() || layer.getWidth() != w || layer.getHeight() != h)
            layer = juce::Image (juce::Image::RGB, w, h, false);
        juce::Graphics ig (layer);
        ig.addTransform (juce::AffineTransform::scale (scale));
        drawLayer (ig);
        layerScale = scale;
        layerValid = true;
    }

    g.drawImageTransformed (layer, juce::AffineTransform::scale (1.0f / scale));

    buildPaths();
    g.setColour (tone::you);
    g.fillPath (youPath);
    g.setColour (bypassed ? tone::you : tone::acid);
    g.fillPath (egoPath);

    // The note going in, joined to the note coming out.
    if (bridgeYou >= 0.0f && bridgeEgo >= 0.0f)
    {
        const juce::Point<float> from (axisX - gutter * 0.5f, bridgeYou), to (axisX + gutter * 0.5f, bridgeEgo);
        g.setColour (tone::text);
        g.drawLine ({ from, to }, 1.5f);
        g.fillEllipse (from.x - 3.5f, from.y - 3.5f, 7.0f, 7.0f);
        g.fillEllipse (to.x - 3.5f, to.y - 3.5f, 7.0f, 7.0f);
    }

    ++paints;
    paintSeconds += (juce::Time::getMillisecondCounterHiRes() - started) * 0.001;
    const auto clip = g.getClipBounds();
    paintedArea += (double) clip.getWidth() * clip.getHeight();
}
