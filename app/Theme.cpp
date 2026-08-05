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

/* ---- the two systemic type rules (documented at length in Theme.h) ------
 * They live here, in the one funnel every font in the app passes through,
 * so that the ~130 existing call sites are corrected without a single
 * panel edit -- and so that a panel written tomorrow cannot reintroduce
 * 6px letterspaced caps by accident.
 *
 * floorSize: nothing below Type::kMinSize (8px). Call sites asking for 6
 * or 7 get 8. The floor is a property of the design system, not of the
 * caller.
 *
 * taperTrack: tracking helps display caps and hurts small text, so it is
 * capped as size drops. Applied to the monospace faces only; cond() is
 * display type at 11px and up, where the wide tracking is the look. */
static float floorSize (float size)
{
    return juce::jmax (size, Type::kMinSize);
}

static float taperTrack (float size, float track)
{
    const float cap = size <  9.0f ? 0.05f
                    : size < 10.0f ? 0.09f
                    : size < 12.0f ? 0.14f
                                   : track;
    return juce::jmin (track, cap);
}

static juce::Font make (const juce::Typeface::Ptr& tf, float size, float track)
{
    return juce::Font (juce::FontOptions (tf).withHeight (size)
                                             .withKerningFactor (track));
}

juce::Font Type::mono (float size, float track)
{
    const float s = floorSize (size);
    return make (plexMono(), s, taperTrack (s, track));
}

juce::Font Type::monoMedium (float size, float track)
{
    const float s = floorSize (size);
    return make (plexMonoMedium(), s, taperTrack (s, track));
}

/* Display face: floor only. Tracking passes through -- masthead 30/.20,
 * panel titles 11/.22 and the stencil sizes are meant to be wide. */
juce::Font Type::cond (float size, float track)
{
    return make (plexCondBold(), floorSize (size), track);
}

/* ---- the command modifier, per platform (see Theme.h) -------------------
 * The Mac glyph is U+2318 PLACE OF INTEREST SIGN, written as its UTF-8 bytes
 * so this file stays pure ASCII on disk and compiles the same whatever the
 * compiler decides the source charset is. Windows and Linux spell it out;
 * "CTRL+" carries its own separator so the key cap reads "CTRL+Z" while the
 * Mac cap stays the tight two-glyph form. */
juce::String modKeyGlyph()
{
   #if defined (_WIN32)
    return "CTRL+";
   #elif defined (__APPLE__)
    return juce::String::fromUTF8 ("\xe2\x8c\x98");
   #else
    return "CTRL+";
   #endif
}

juce::String modKeyWord()
{
   #if defined (__APPLE__) && ! defined (_WIN32)
    return "cmd";
   #else
    return "ctrl";
   #endif
}

} // namespace morgue
