#pragma once
#include "Theme.h"
#include "../PluginProcessor.h"

/** The hero's left half. A map of voices: left and right is Pitch, up and down is Formant Shift, one semitone is the same distance
    either way. You are the mark in the middle, and the dot is the voice, including whatever the Character adds.

    Everything that only moves with a parameter sits in one cached image. Only the disc under the dot moves with the sound: it sits
    where the voice really is right now (vibrato and Robot swing it about) and grows with the level going out. */
class VoicePad final : public juce::Component,
                       public ui::Hinted
{
public:
    explicit VoicePad (PluginProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** One frame. liveShift is how far the voice really is from you in semitones, level is 0 to 1. Returns true while anything is moving. */
    bool advance (bool voiced, double liveShift, float level);

    /** Shows where a Character would land, while the mouse is on its chip. -1 for none. */
    void showCharacter (int index);

    juce::Point<float> voiceAt() const noexcept  { return pointFor (voicePitch(), voiceFormant()); }
    juce::Point<float> pointFor (double pitch, double formant) const noexcept;
    double voicePitch() const noexcept;
    double voiceFormant() const noexcept;
    float liveX() const noexcept  { return discCentre.x; }
    float discSize() const noexcept  { return discRadius; }

    int getPaintCount() const noexcept  { return paints; }
    double getPaintSeconds() const noexcept  { return paintSeconds; }
    double getPaintedArea() const noexcept  { return paintedArea; }

private:
    static constexpr double pitchReach = 24.0, formantReach = 12.0;
    static constexpr float dotRadius = 12.0f;

    void drawLayer (juce::Graphics&) const;
    void changed();
    void moveTo (juce::Point<float>, bool snap);
    juce::Rectangle<float> map() const noexcept;
    juce::Rectangle<int> liveBounds() const;

    PluginProcessor& processorRef;
    juce::RangedAudioParameter& pitchParam;
    juce::RangedAudioParameter& formantParam;

    float pitch = 0.0f, formant = 0.0f, amount = 1.0f;
    int character = 0, shownCharacter = -1;
    bool bypassed = false, dragging = false;
    juce::Point<float> grabOffset;

    juce::ParameterAttachment pitchAttachment, formantAttachment, characterAttachment, amountAttachment, bypassAttachment;

    juce::Image layer;
    float layerScale = 0.0f;
    bool layerValid = false;

    static constexpr int trailLength = 16;
    std::array<juce::Point<float>, trailLength> trail {};
    int trailAt = 0;
    double liveSmooth = 0.0;
    float levelSmooth = 0.0f;
    juce::Point<float> discCentre;
    float discRadius = 0.0f;
    juce::Rectangle<int> paintedLive;
    bool anyTrail = false;

    int paints = 0;
    double paintSeconds = 0.0, paintedArea = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoicePad)
};
