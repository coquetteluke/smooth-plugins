#pragma once
#include <cstddef>

// JUCE can only use a variable font's default weight, and a reading meant for across a room needs a heavy one.
// So this asks CoreText for the letter shapes at any weight, with kerning. It knows nothing about JUCE.
namespace keyui
{
    struct OutlineSink
    {
        virtual ~OutlineSink() = default;
        virtual void moveTo (float x, float y) = 0;
        virtual void lineTo (float x, float y) = 0;
        virtual void quadTo (float cx, float cy, float x, float y) = 0;
        virtual void cubicTo (float c1x, float c1y, float c2x, float c2y, float x, float y) = 0;
        virtual void close() = 0;
    };

    class VariableFont
    {
    public:
        VariableFont (const void* data, std::size_t size);
        ~VariableFont();

        // Sends the outline of the text to the sink, at a type size of 1000, y going up. Returns its width.
        float outline (const char* utf8, int weight, OutlineSink&) const;

    private:
        struct Impl;
        Impl* impl;

        VariableFont (const VariableFont&) = delete;
        VariableFont& operator= (const VariableFont&) = delete;
    };
}
