#include "ModuleRow.h"

using namespace fxui;

namespace
{
    constexpr float blockGap = 6.0f, pad = 10.0f;
    constexpr float switchW = 26.0f, switchH = 14.0f;
    const char* const watchedIds[] = {
        "pitchOctave", "pitchSemitones", "motionPattern", "throatLength", "harmonyVoices", "harmonyInterval1", "harmonyInterval2",
        "doublerDetune", "vocoderWave", "distType", "filterType", "ringFrequency", "delayTime", "reverbType",
    };

    juce::String signedText (int v) { return (v > 0 ? "+" : "") + juce::String (v); }
}

ModuleRow::ModuleRow (PluginProcessor& p) : processor (p)
{
    setTitle ("Modules");
    hint = "Click a block for its controls. Click its switch to turn it on or off.";

    for (size_t i = 0; i < switches.size(); ++i)
    {
        auto& param = *processor.apvts.getParameter (modules()[i].onId);
        switches[i] = std::make_unique<juce::ParameterAttachment> (param, [this, i] (float v)
        {
            const bool on = v >= 0.5f;
            if (on != onNow[i])
            {
                onNow[i] = on;
                repaint();
                if (onSwitched != nullptr)
                    onSwitched();
            }
        }, nullptr);
        switches[i]->sendInitialUpdate();
    }

    for (auto* id : watchedIds)
        watched.push_back (processor.apvts.getRawParameterValue (id));
    seen.resize (watched.size(), -1.0e9f);
    bypassParam = processor.apvts.getRawParameterValue ("bypass");
    updateSummaries();
}

void ModuleRow::resized()
{
    repaint();
}

juce::Rectangle<float> ModuleRow::blockBounds (int i) const
{
    const float w = ((float) getWidth() - blockGap * (float) (numModules - 1)) / (float) numModules;
    return { (w + blockGap) * (float) i, 0.0f, w, (float) (getHeight() - tabDepth) };
}

juce::Rectangle<float> ModuleRow::switchBounds (int i) const
{
    const auto b = blockBounds (i);
    return { b.getX() + pad, b.getY() + pad, switchW, switchH };
}

// A larger target than the switch you see, so it is easy to hit.
juce::Rectangle<float> ModuleRow::switchHitArea (int i) const
{
    const auto b = blockBounds (i);
    return b.withHeight (pad * 2.0f + switchH).withWidth (pad * 2.0f + switchW);
}

int ModuleRow::blockAt (juce::Point<float> p) const
{
    for (int i = 0; i < numModules; ++i)
    {
        auto b = blockBounds (i);
        if (i == selected)
            b = b.withBottom ((float) getHeight());
        if (b.contains (p))
            return i;
    }
    return -1;
}

void ModuleRow::setSelected (int module)
{
    if (module != selected)
    {
        selected = module;
        repaint();
    }
}

void ModuleRow::updateSummaries()
{
    const auto raw = [this] (const char* id) { return processor.apvts.getRawParameterValue (id)->load (std::memory_order_relaxed); };
    const auto choice = [this] (const char* id) { return processor.apvts.getParameter (id)->getCurrentValueAsText(); };

    const int shift = 12 * juce::roundToInt (raw ("pitchOctave")) + juce::roundToInt (raw ("pitchSemitones"));
    summaries[0] = shift == 0 ? juce::String ("No shift") : signedText (shift) + (std::abs (shift) == 1 ? " note" : " notes");
    summaries[1] = choice ("motionPattern");
    summaries[2] = "Length " + juce::String (juce::roundToInt (raw ("throatLength"))) + "%";
    const int a = juce::roundToInt (raw ("harmonyInterval1")), b = juce::roundToInt (raw ("harmonyInterval2"));
    summaries[3] = juce::roundToInt (raw ("harmonyVoices")) == 1 ? signedText (a) + " and " + signedText (b) : signedText (a) + " notes";
    summaries[4] = juce::String (juce::roundToInt (raw ("doublerDetune"))) + " cents";
    summaries[5] = choice ("vocoderWave");
    summaries[6] = choice ("distType");
    summaries[7] = choice ("filterType");
    const float hz = raw ("ringFrequency");
    summaries[8] = hz >= 1000.0f ? juce::String (hz * 0.001f, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
    summaries[9] = choice ("delayTime");
    summaries[10] = choice ("reverbType");
}

bool ModuleRow::refresh()
{
    bool changed = false;
    for (size_t i = 0; i < watched.size(); ++i)
    {
        const float v = watched[i]->load (std::memory_order_relaxed);
        if (v != seen[i])
        {
            seen[i] = v;
            changed = true;
        }
    }

    const bool nowBypassed = bypassParam->load (std::memory_order_relaxed) >= 0.5f;
    if (nowBypassed != bypassed)
    {
        bypassed = nowBypassed;
        changed = true;
        if (onSwitched != nullptr)
            onSwitched();
    }

    if (changed)
    {
        updateSummaries();
        repaint();
    }
    return changed;
}

void ModuleRow::updateHint()
{
    if (hovered < 0)
        hint = "Click a block for its controls. Click its switch to turn it on or off.";
    else
    {
        const juce::String name (modules()[(size_t) hovered].name);
        hint = hoverOnSwitch ? "Turns " + name + (onNow[(size_t) hovered] ? " off." : " on.")
                             : "Shows the controls for " + name + ".";
    }
    if (onHint != nullptr)
        onHint (hint);
}

void ModuleRow::mouseMove (const juce::MouseEvent& e)
{
    const int now = blockAt (e.position);
    const bool onSwitch = now >= 0 && switchHitArea (now).contains (e.position);
    if (now != hovered || onSwitch != hoverOnSwitch)
    {
        hovered = now;
        hoverOnSwitch = onSwitch;
        updateHint();
        repaint();
    }
    setMouseCursor (now >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void ModuleRow::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    hoverOnSwitch = false;
    updateHint();
    repaint();
}

void ModuleRow::pressSwitch (int module)
{
    const auto i = (size_t) module;
    const bool turningOn = ! onNow[i];
    switches[i]->setValueAsCompleteGesture (turningOn ? 1.0f : 0.0f);

    // Turning one on brings its controls up, because that is what you want next.
    if (turningOn && onSelect != nullptr)
        onSelect (module);
}

void ModuleRow::mouseDown (const juce::MouseEvent& e)
{
    const int hit = blockAt (e.position);
    if (hit < 0)
        return;

    if (switchHitArea (hit).contains (e.position))
        pressSwitch (hit);
    else if (onSelect != nullptr)
        onSelect (hit);

    updateHint();
}

void ModuleRow::paint (juce::Graphics& g)
{
    const float dim = bypassed ? 0.4f : 1.0f;

    for (int i = 0; i < numModules; ++i)
    {
        const auto& m = modules()[(size_t) i];
        const bool on = onNow[(size_t) i], sel = i == selected, hov = i == hovered;
        auto b = blockBounds (i);

        const auto fill = on ? (hov ? m.colour.brighter (0.15f) : m.colour)
                             : (sel ? panelTint (m.colour, false) : (hov ? tone::lifted : tone::raised));
        g.setColour (fill.withMultipliedAlpha (dim));

        if (sel)
        {
            // Reaches down and joins the panel, so it is plain which block the panel belongs to.
            juce::Path tab;
            tab.addRoundedRectangle (b.getX(), b.getY(), b.getWidth(), (float) getHeight(), 8.0f, 8.0f, true, true, false, false);
            g.fillPath (tab);
        }
        else
        {
            g.fillRoundedRectangle (b, 8.0f);
        }

        // The switch.
        const auto sw = switchBounds (i);
        const float knob = switchH - 4.0f;
        const auto ink = (on ? tone::black : m.colour.interpolatedWith (tone::black, 0.18f)).withMultipliedAlpha (dim);
        if (on)
        {
            g.setColour (ink);
            g.fillRoundedRectangle (sw, switchH * 0.5f);
            g.setColour (m.colour.withMultipliedAlpha (dim));
            g.fillEllipse (sw.getRight() - 2.0f - knob, sw.getY() + 2.0f, knob, knob);
        }
        else
        {
            g.setColour (ink.withMultipliedAlpha (hov && hoverOnSwitch ? 1.0f : 0.65f));
            g.drawRoundedRectangle (sw.reduced (0.5f), switchH * 0.5f, 1.0f);
            g.fillEllipse (sw.getX() + 2.0f, sw.getY() + 2.0f, knob, knob);
        }

        // Name, then what it is set to.
        auto text = b.withTrimmedLeft (pad).withTrimmedRight (4.0f);
        text.removeFromTop (pad + switchH + 8.0f);
        g.setColour (ink);
        g.setFont (font (Weight::medium, 15.0f));
        g.drawText (m.name, text.removeFromTop (20.0f), juce::Justification::centredLeft, false);
        g.setColour (on ? tone::black.withAlpha (0.72f * dim) : tone::muted.withMultipliedAlpha (dim));
        g.setFont (font (Weight::regular, 12.0f));
        g.drawText (summaries[(size_t) i], text.removeFromTop (17.0f), juce::Justification::centredLeft, true);
    }
}
