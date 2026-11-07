// Offline tests. Run: build/PluginTest_artefacts/Release/PluginTest | tee tests/results.txt
#define SMOOTH_COUNT_ALLOCS
#include "TestKit.h"
#include "PluginProcessor.h"

using namespace testkit;

static void testGain()
{
    PluginProcessor p;
    prepare (p, 2, 48000.0, 512);
    p.apvts.getParameter ("gain")->setValueNotifyingHost (p.apvts.getParameter ("gain")->convertTo0to1 (-6.0f));
    std::vector<float> in (48000);
    sine (in.data(), 48000, 1000.0, 48000.0, 0.5);
    auto out = render (p, in, 512);
    const double change = rmsDb (out, 0, 24000) - toDb (rms (in.data() + 24000, 24000));
    check (std::abs (change + 6.0) < 0.05, "Gain -6 dB measures", change, "dB");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    runGenericChecks ([] { return std::make_unique<PluginProcessor>(); });
    testGain();
    return finish();
}
