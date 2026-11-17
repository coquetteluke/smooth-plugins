// Link Probe: the test processor of tests/TestProcessor.h as a real JUCE Audio Unit (aufx SLpr Luke), with its link built from
// the JucePlugin_ macros exactly as a plug-in does it. Never installed: LinkCheck loads it straight from the build folder with
// --load, so LinkCheck's own checks run against a known good link (the positive control to Punch's negative one).
#include "TestProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new linktest::TestProcessor (smooth::Link::Info::fromBuild ({ "gain" }));
}
