#pragma once

#include "../PluginProcessor.h"
#include "Theme.h"

namespace ui
{
    /** The hero: the stage from straight above. Left to right is where a singer is panned. Going back from the front is how far behind the lead they sing,
        read live from the sound. Each double has a tape mark where its path is centred and a lane it can wander in, sized by the settings.
        Its shoulders turn with the pitch it is drifting to, and a pool of light under it is as big as what it is singing. Drag a singer to move them. */
    class StageView final : public juce::Component,
                            public Hinted
    {
    public:
        static constexpr int leadIndex = DuoEngine::maxDoubles;   // the singers: doubles 0, 1, 2, then the lead

        explicit StageView (PluginProcessor&);

        /** One display frame: reads the sound and the settings, and repaints only what moved. */
        void advance (double dtSeconds);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        /** True while anything is still moving, so the window knows to keep its frame rate up. */
        bool isMoving() const noexcept         { return live || ! settled; }
        bool isLive() const noexcept           { return live; }
        int getPaintCount() const noexcept     { return paintCount; }
        juce::Point<float> singerAt (int s) const noexcept  { return { singers[(size_t) s].x, singers[(size_t) s].y }; }
        float markY (int s) const noexcept     { return singers[(size_t) s].markY; }
        float zoneHalfHeight (int s) const noexcept  { return singers[(size_t) s].zoneHalf; }
        bool isOnStage (int s) const noexcept  { return singers[(size_t) s].onStage; }
        /** Where a double's path is centred right now, in ms, from the settings. */
        double restMs (int v) const;
        float xForPan (float pan) const noexcept  { return centreX() + pan * spanX(); }
        float yForMs (double ms, float x) const noexcept;
        /** Lights a singer as if the mouse were over it, for the offline pictures. */
        void setHighlight (int s)              { hovered = s; updateHint(); repaint(); }

        /** Called when a drag on the stage changes something, so the window wakes up at once. */
        std::function<void()> onActivity;

    private:
        struct Singer
        {
            float x = 0.0f, y = 0.0f;             // where it is drawn now
            float markX = 0.0f, markY = 0.0f;     // where its path is centred
            float zoneHalf = 0.0f;                // how far it can wander up and down the stage, in pixels
            float gain = 1.0f, level = 0.0f;      // its fader, and how loud it is right now
            float splitReach = 0.0f;              // a double with no partner is spread to both sides, this far in pixels
            float angle = 0.0f, size = 1.0f;      // shoulders turned by the pitch drift, size set by Timbre
            double lastMs = 0.0;
            bool onStage = false, off = false;
            float drawnX = 0.0f, drawnY = 0.0f, drawnPool = 0.0f, drawnAngle = 0.0f;   // what is on screen
            juce::Rectangle<float> drawnBounds;
        };

        float centreX() const noexcept         { return (float) getWidth() * 0.5f; }
        float spanX() const noexcept           { return (float) getWidth() * 0.5f - 104.0f; }
        float frontY (float x) const noexcept;
        float depth() const noexcept;
        juce::Rectangle<float> boundsOf (const Singer&, bool isLead) const;
        int singerAtPoint (juce::Point<float>) const;
        void readSettings();
        void markZonesDirty()                  { zonesDirty = true; repaint(); }
        void rebuildFloor (float scale);
        void rebuildZones (float scale);
        void drawSinger (juce::Graphics&, const Singer&, int index) const;
        juce::Image makeSprite (int index, int angleStep, float size) const;
        void updateHint();

        PluginProcessor& processor;
        std::array<Singer, DuoEngine::maxDoubles + 1> singers;

        // The settings the picture was last worked out from.
        std::array<float, 16> settingsSeen {};
        int doublesCount = 2;
        bool stereo = true, bypassed = false;

        // The stage itself, then the marks and zones on top of it. Both are only redrawn when they change.
        juce::Path bodyPath;
        juce::Image floorImage, zonesImage, leadTag;
        mutable std::array<std::array<juce::Image, 15>, DuoEngine::maxDoubles + 1> sprites;   // each singer at each angle it can turn to, drawn when first needed
        float layerScale = 0.0f;
        bool zonesDirty = true;

        bool live = false, settled = false;
        double silentFor = 10.0;
        int paintCount = 0, hovered = -1;

        // A drag
        enum class Axis { undecided, across, along };
        int dragged = -1;
        Axis dragAxis = Axis::undecided;
        float widthAtStart = 0.0f, looseAtStart = 0.0f, panAtStart = 0.0f;
        bool widthGesture = false, looseGesture = false, panGesture = false;
        juce::ParameterAttachment widthAttachment, looseAttachment, leadPanAttachment;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StageView)
    };
}
