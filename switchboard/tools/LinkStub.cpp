// A stand-in plug-in instance with a link, for end-to-end tests of link readers.
// Usage: LinkStub --dir D [--track T] [--plugin P] [--seconds N]
// Hosts the test processor of tests/TestProcessor.h (Gain, On, Mode, Voices, Plain) with a smooth::Link writing into D,
// under a real JUCE message loop, so the card, heartbeat and inbox behave exactly as in a plug-in. Prints "READY <id>" once the
// card holds the track name, then serves until SIGTERM or SIGINT (card and inbox removed) or N seconds pass.
#include <juce_audio_processors/juce_audio_processors.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include "TestProcessor.h"

namespace
{
volatile std::sig_atomic_t stopRequested = 0;
void onSignal (int) { stopRequested = 1; }

int usage()
{
    std::fprintf (stderr, "usage: LinkStub --dir <absolute folder> [--track <name>] [--plugin <name>] [--seconds <n>]\n");
    return 2;
}
} // namespace

int main (int argc, char** argv)
{
    juce::String dir, track, plugin = "Link Stub";
    double seconds = -1.0;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (i + 1 >= argc)
            return usage();
        const auto v = juce::String::fromUTF8 (argv[++i]);
        if (a == "--dir") dir = v;
        else if (a == "--track") track = v;
        else if (a == "--plugin") plugin = v;
        else if (a == "--seconds") seconds = v.getDoubleValue();
        else return usage();
    }
    if (! dir.startsWithChar ('/'))
        return usage();

    // The link reads its folder from the environment, as in every test.
    setenv ("SMOOTH_LINK_DIR", dir.toRawUTF8(), 1);
    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT, onSignal);
    std::signal (SIGPIPE, SIG_IGN);

    juce::ScopedJuceInitialiser_GUI gui;
    {
        auto info = linktest::TestProcessor::testInfo();
        info.plugin = plugin;
        info.subtype = "SLst";
        linktest::TestProcessor p (info);
        if (! p.link.enabled() || ! p.link.card().existsAsFile())
        {
            std::fprintf (stderr, "LinkStub: no card in %s\n", dir.toRawUTF8());
            return 1;
        }
        p.link.setTrackName (track);
        p.link.pollNow();
        std::printf ("READY %s\n", p.link.id().toRawUTF8());
        std::fflush (stdout);

        auto* mm = juce::MessageManager::getInstance();
        const double end = seconds > 0.0 ? juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0 : 0.0;
        while (stopRequested == 0 && (end == 0.0 || juce::Time::getMillisecondCounterHiRes() < end))
            mm->runDispatchLoopUntil (50);
    }
    return 0;
}
