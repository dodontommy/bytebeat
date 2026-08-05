/* Theme.cpp -- embedded IBM Plex typefaces + the type scale. See Theme.h. */

#include "Theme.h"
#include <juce_graphics/juce_graphics.h>
#include "BinaryData.h"

namespace morgue
{

/* The three embedded faces, created once. No system-font fallback: the
 * TTFs are compiled into the binary (spec section 2). */
static juce::Typeface::Ptr plexMono()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (
        BinaryData::IBMPlexMonoRegular_ttf, (size_t) BinaryData::IBMPlexMonoRegular_ttfSize);
    return t;
}

static juce::Typeface::Ptr plexMonoMedium()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (
        BinaryData::IBMPlexMonoMedium_ttf, (size_t) BinaryData::IBMPlexMonoMedium_ttfSize);
    return t;
}

static juce::Typeface::Ptr plexCondBold()
{
    static juce::Typeface::Ptr t = juce::Typeface::createSystemTypefaceFor (
        BinaryData::IBMPlexSansCondensedBold_ttf,
        (size_t) BinaryData::IBMPlexSansCondensedBold_ttfSize);
    return t;
}

static juce::Font make (const juce::Typeface::Ptr& tf, float size, float track)
{
    return juce::Font (juce::FontOptions (tf).withHeight (size)
                                             .withKerningFactor (track));
}

juce::Font Type::mono       (float size, float track) { return make (plexMono(),       size, track); }
juce::Font Type::monoMedium (float size, float track) { return make (plexMonoMedium(), size, track); }
juce::Font Type::cond       (float size, float track) { return make (plexCondBold(),   size, track); }

} // namespace morgue
