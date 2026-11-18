#include "FontOutlines.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <cstring>
#include <map>

namespace keyui
{
    namespace
    {
        constexpr CGFloat outlineSize = 1000.0;

        void applyElement (void* info, const CGPathElement* e)
        {
            auto& sink = *static_cast<OutlineSink*> (info);
            const auto* p = e->points;

            switch (e->type)
            {
                case kCGPathElementMoveToPoint:         sink.moveTo ((float) p[0].x, (float) p[0].y); break;
                case kCGPathElementAddLineToPoint:      sink.lineTo ((float) p[0].x, (float) p[0].y); break;
                case kCGPathElementAddQuadCurveToPoint: sink.quadTo ((float) p[0].x, (float) p[0].y, (float) p[1].x, (float) p[1].y); break;
                case kCGPathElementAddCurveToPoint:     sink.cubicTo ((float) p[0].x, (float) p[0].y, (float) p[1].x, (float) p[1].y, (float) p[2].x, (float) p[2].y); break;
                case kCGPathElementCloseSubpath:        sink.close(); break;
                default: break;
            }
        }
    }

    struct VariableFont::Impl
    {
        CGFontRef base = nullptr;
        CFStringRef weightAxis = nullptr;
        mutable std::map<int, CTFontRef> instances;

        CTFontRef instance (int weight) const
        {
            auto found = instances.find (weight);
            if (found != instances.end())
                return found->second;

            CTFontRef font = nullptr;

            if (base != nullptr && weightAxis != nullptr)
            {
                const double value = (double) weight;
                auto number = CFNumberCreate (nullptr, kCFNumberDoubleType, &value);
                const void* keys[] = { weightAxis };
                const void* values[] = { number };
                auto variations = CFDictionaryCreate (nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

                if (auto varied = CGFontCreateCopyWithVariations (base, variations))
                {
                    font = CTFontCreateWithGraphicsFont (varied, outlineSize, nullptr, nullptr);
                    CGFontRelease (varied);
                }

                CFRelease (variations);
                CFRelease (number);
            }

            if (font == nullptr && base != nullptr)
                font = CTFontCreateWithGraphicsFont (base, outlineSize, nullptr, nullptr);

            instances[weight] = font;
            return font;
        }
    };

    VariableFont::VariableFont (const void* data, std::size_t size) : impl (new Impl())
    {
        auto bytes = CFDataCreate (nullptr, static_cast<const UInt8*> (data), (CFIndex) size);
        auto provider = CGDataProviderCreateWithCFData (bytes);
        impl->base = CGFontCreateWithDataProvider (provider);
        CGDataProviderRelease (provider);
        CFRelease (bytes);

        if (impl->base == nullptr)
            return;

        if (auto axes = CGFontCopyVariationAxes (impl->base))
        {
            // Jost has one axis, weight. Variations are asked for by the axis's name.
            if (CFArrayGetCount (axes) > 0)
            {
                auto axis = static_cast<CFDictionaryRef> (CFArrayGetValueAtIndex (axes, 0));
                if (auto name = static_cast<CFStringRef> (CFDictionaryGetValue (axis, kCGFontVariationAxisName)))
                    impl->weightAxis = static_cast<CFStringRef> (CFRetain (name));
            }

            CFRelease (axes);
        }
    }

    VariableFont::~VariableFont()
    {
        for (auto& entry : impl->instances)
            if (entry.second != nullptr)
                CFRelease (entry.second);

        if (impl->weightAxis != nullptr) CFRelease (impl->weightAxis);
        if (impl->base != nullptr) CGFontRelease (impl->base);
        delete impl;
    }

    float VariableFont::outline (const char* utf8, int weight, OutlineSink& sink) const
    {
        auto font = impl->instance (weight);
        if (font == nullptr || utf8 == nullptr || *utf8 == 0)
            return 0.0f;

        auto text = CFStringCreateWithBytes (nullptr, reinterpret_cast<const UInt8*> (utf8), (CFIndex) std::strlen (utf8), kCFStringEncodingUTF8, false);
        if (text == nullptr)
            return 0.0f;

        const void* keys[] = { kCTFontAttributeName };
        const void* values[] = { font };
        auto attributes = CFDictionaryCreate (nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        auto attributed = CFAttributedStringCreate (nullptr, text, attributes);
        auto line = CTLineCreateWithAttributedString (attributed);
        const float width = (float) CTLineGetTypographicBounds (line, nullptr, nullptr, nullptr);

        auto runs = CTLineGetGlyphRuns (line);

        for (CFIndex r = 0; r < CFArrayGetCount (runs); ++r)
        {
            auto run = static_cast<CTRunRef> (CFArrayGetValueAtIndex (runs, r));
            auto runFont = static_cast<CTFontRef> (CFDictionaryGetValue (CTRunGetAttributes (run), kCTFontAttributeName));

            for (CFIndex g = 0; g < CTRunGetGlyphCount (run); ++g)
            {
                CGGlyph glyph = 0;
                CGPoint position {};
                CTRunGetGlyphs (run, CFRangeMake (g, 1), &glyph);
                CTRunGetPositions (run, CFRangeMake (g, 1), &position);
                const auto place = CGAffineTransformMakeTranslation (position.x, position.y);

                if (auto path = CTFontCreatePathForGlyph (runFont, glyph, &place))
                {
                    CGPathApply (path, &sink, applyElement);
                    CGPathRelease (path);
                }
            }
        }

        CFRelease (line);
        CFRelease (attributed);
        CFRelease (attributes);
        CFRelease (text);
        return width;
    }
}
