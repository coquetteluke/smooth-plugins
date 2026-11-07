#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace ui
{
    /** The hero. Two views that share one pitch scale: on the left each voice is a line through time, and each line runs into its singer on the stage on the right.
        Left and right on the stage is pan. Up and down is the note, the same as on the left, and a singer's size is its Level. Drag a singer to move it.

        The lines are kept in one picture that is written like a tape loop: only the new columns are drawn, and the picture is shown in two pieces.
        Nothing here touches the audio. It reads the processor's pitch history and its readings. */
    class StageView final : public juce::Component
    {
    public:
        explicit StageView (PluginProcessor&);
        ~StageView() override;

        static constexpr int singers = PluginProcessor::maxVoices + 1;   // the voices, then the lead
        static constexpr int leadIndex = PluginProcessor::maxVoices;

        /** One display frame. Reads what is new, draws it, asks for a repaint only of what changed. */
        void advance (double dtSeconds);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

        /** Marks a singer, from hovering its strip. -1 for none. */
        void setHighlight (int singer);

        /** Brings every singer back, the lead too. What the All button does. */
        void hearAll();
        bool isHeard (int singer) const noexcept  { return heard[(size_t) singer]; }
        juce::Component& getAllButton();

        /** The note a voice is singing now, or would sing, as text. Filled in by advance. */
        const juce::String& noteFor (int singer) const noexcept  { return state[(size_t) singer].label; }
        /** True when the voice is sounding now and not just showing where it would be. */
        bool isLive() const noexcept  { return live; }
        /** True if the last frame drew anything. When nothing is moving the window can look less often. */
        bool isMoving() const noexcept  { return moving; }

        std::function<void (const juce::String&)> onHint;

        int getPaintCount() const noexcept        { return paintCount; }
        juce::Point<float> singerPosition (int singer) const;
        float semitonePixels() const noexcept;

    private:
        struct Singer
        {
            float pitch = 60.0f, target = 60.0f, pan = 0.0f, level = -6.0f, loud = 0.0f;
            bool shown = false, known = false;
            juce::String label = "C4";
            juce::Image sprite;                 // the dot and its note, at screen pixels
            juce::String spriteLabel;
            float spriteSize = 0.0f, spriteAlpha = 0.0f;
        };

        // Where things are, in window units.
        juce::Rectangle<int> timeArea() const noexcept;
        juce::Rectangle<int> stageArea() const noexcept;
        float yFor (float pitch) const noexcept;
        float xForPan (float pan) const noexcept;
        float diameterFor (const Singer&) const noexcept;
        int singerAt (juce::Point<float>) const;

        void updateSingers (const PluginProcessor::Readings&, double dtSeconds);
        void restPitches (std::array<float, singers>&, std::array<bool, singers>&) const;
        bool updateRange();
        void updateKey();

        void rebuild (float scale);
        void drawBackground();
        void clearColumns (juce::Graphics&, int fromColumn, int toColumn);
        void drawLines (juce::Graphics&, long long fromPoint, long long toPoint);
        int columnFor (long long point) const noexcept;
        void refreshSprite (Singer&, juce::Colour, float scale);

        void showHintFor (int singer);
        void startDrag (int singer);
        void repaintStage();

        juce::RangedAudioParameter* panParam (int singer) const;
        juce::RangedAudioParameter* intervalParam (int singer) const;

        PluginProcessor& processor;
        juce::SharedResourcePointer<Assets> assets;

        std::array<Singer, singers> state;
        std::array<std::atomic<float>*, PluginProcessor::maxVoices> levelValue {}, panValue {}, intervalValue {};
        std::atomic<float>* modeValue, *keyValue, *scaleValue, *followValue, *chordRootValue, *chordTypeValue, *leadPanValue, *leadLevelValue, *bypassValue;

        juce::Image background, ring;
        float cachedScale = 0.0f;
        bool needsRebuild = true;

        static constexpr int visiblePoints = 1600;      // 8 seconds at one point every 5 ms
        long long drawnTo = 0, lastSung = -1000000, lastRead = -1;
        bool started = false;
        int ringWidth = 0, ringHeight = 0;

        int rangeLow = 48, rangeHigh = 72;
        float linesLow = 1000.0f, linesHigh = -1000.0f;  // the pitches the lines cover. Rows above and below do not change as the lines scroll, so they are not repainted
        juce::Rectangle<int> bandShown;
        bool moving = true;
        std::uint16_t laneMask = 0x0FFF;
        int laneRoot = 0, shownMode = -1;
        int framesToRangeCheck = 0;

        bool live = false, bypassed = false, emptyShown = true;
        double sinceVoiced = 1000.0;
        float lastLeadPitch = -1.0f;

        class AllButton;
        std::unique_ptr<AllButton> allButton;
        std::array<bool, singers> heard;
        std::array<std::atomic<float>*, singers> onValue {};

        int hovered = -1, highlight = -1, pressed = -1, dragging = -1;   // pressed, then dragging once the mouse has really moved
        int dragInterval = 0;
        juce::Point<float> pressedAt;
        float dragStartY = 0.0f;
        bool dragMovesInterval = false;
        int paintCount = 0;
        float spriteScale = 0.0f;
        std::array<juce::uint32, 2> lastStamp {};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageView)
    };
}
