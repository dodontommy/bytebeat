/* Theme.h -- the visual language of MORGUE.
 *
 * This file carries the design-handoff spec verbatim:
 *   design_handoff_morgue_gui/MORGUE_UI_SPEC.md section 1 (colour tokens)
 *   and section 2 (type scale).
 *
 * Rules of the look (spec section 0): no gradients, no rounded corners
 * (knob faces are circles), no shadows, every rule exactly 1px (2px only
 * for the active tab underline), monospace only, one accent colour
 * (BLOOD), integer-aligned fillRect painting.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace morgue
{

/* UTF-8 literal helper: JUCE's String(const char*) is not UTF-8 aware, and
 * the spec copy is full of middle dots, arrows and dashes. Wrap every
 * literal that carries a multi-byte character. */
inline juce::String U8 (const char* s) { return juce::String::fromUTF8 (s); }

/* ---- spec section 1: colour tokens (exact hex, spec names) -------------- */
namespace C
{
    inline const juce::Colour GROUND         { 0xff0a0a0a };  // window ground
    inline const juce::Colour PANEL          { 0xff0c0b0a };  // panel body, lane field
    inline const juce::Colour PANEL_ALT      { 0xff0e0d0c };  // tab strip, status bar, toolbars
    inline const juce::Colour RAISED         { 0xff151412 };  // header bands
    inline const juce::Colour TRANSPORT      { 0xff121110 };  // title bar, transport bar
    inline const juce::Colour SOCKET         { 0xff060606 };  // recessed wells, code editor, scope
    inline const juce::Colour CONTROL        { 0xff171614 };  // knob face
    inline const juce::Colour PLATE          { 0xff141312 };  // idle button plate
    inline const juce::Colour PLATE_LOW      { 0xff131211 };  // idle plate, toggle-off variant
    inline const juce::Colour PLATE_HOVER    { 0xff1b1a17 };  // hovered/action plate
    inline const juce::Colour HAIRLINE       { 0xff232220 };  // all borders/rules
    inline const juce::Colour HAIRLINE_DIM   { 0xff1c1b19 };  // inner sub-rules
    inline const juce::Colour HAIRLINE_FAINT { 0xff141312 };  // table row separators
    inline const juce::Colour EDGE           { 0xff3a3833 };  // raised control border
    inline const juce::Colour INK            { 0xffded9ce };  // primary text, knob cut
    inline const juce::Colour INK_BRIGHT     { 0xfff0e6dc };  // text on blood
    inline const juce::Colour INK_DIM        { 0xff8a8579 };  // labels
    inline const juce::Colour INK_FAINT      { 0xff55524b };  // metadata
    inline const juce::Colour INK_GHOST      { 0xff3a3833 };  // disabled
    inline const juce::Colour BLOOD          { 0xff8b1e14 };  // armed border, fader fill, meters
    inline const juce::Colour BLOOD_DEEP     { 0xff2a0d09 };  // armed plate background
    inline const juce::Colour BLOOD_HOT      { 0xffc2301f };  // lamps, playhead, CLIP
    inline const juce::Colour OXIDE          { 0xff8a5a2b };  // sends, automation, PLANNED tags
    inline const juce::Colour OXIDE_DIM      { 0xff6b4a2a };  // oxide borders
    inline const juce::Colour OXIDE_INK      { 0xffc9a06a };  // text on oxide
    inline const juce::Colour AMBER          { 0xffb8862b };  // warn, meter above -6dB, CPU warn
    inline const juce::Colour GREEN_FAINT    { 0xff7c8a5a };  // one use: "STREAM OK" / "COMPILED"

    /* Supporting literals the spec / HTML use repeatedly (frame 10 + panels). */
    inline const juce::Colour LAMP_DEAD       { 0xff2a2927 };  // dead lamp, muted fader fill, PLANNED border
    inline const juce::Colour LAMP_SOUNDING   { 0xff4a4842 };  // sounding-voice lamp / disabled label
    inline const juce::Colour TAB_ACTIVE_BG   { 0xff191816 };  // active tab / selected locker row
    inline const juce::Colour TAB_INACTIVE_FG { 0xff6b6760 };  // inactive tab text
    inline const juce::Colour KNOB_INNER      { 0xff201f1c };  // knob inner inset ring
    inline const juce::Colour KNOB_UNUSED_RING{ 0xff252420 };  // UNUSED knob ring
    inline const juce::Colour ARMED_TEXT      { 0xffe8ddd4 };  // text on armed (BLOOD_DEEP) plates
    inline const juce::Colour HIT_BG          { 0xff332f2a };  // step cell HIT fill
    inline const juce::Colour HIT_BD          { 0xff4a4640 };  // step cell HIT border
    inline const juce::Colour CELL_NUM_OFF    { 0xff2f2e2b };  // step index on OFF cell
    inline const juce::Colour CELL_NUM_ACC    { 0xffe8c9c4 };  // step index on ACCENT cell
    inline const juce::Colour TROUGH          { 0xff080807 };  // fader/meter trough
    inline const juce::Colour DISABLED_BG     { 0xff101010 };  // disabled plate bg
    inline const juce::Colour OXIDE_PLATE     { 0xff1a1512 };  // oxide-loaded slot / 'O' engaged plate
    inline const juce::Colour MANUAL_BG       { 0xff080807 };  // field-manual ground
}

/* ---- status badge colour rules (spec section 1, bottom) ----------------- */
struct Badge
{
    enum Kind { LIVE, PARTIAL, CANVAS, PLANNED };

    static juce::Colour border (Kind k)
    {
        switch (k)
        {
            case LIVE:    return C::BLOOD;
            case PARTIAL:
            case CANVAS:  return C::OXIDE_DIM;
            default:      return C::LAMP_DEAD;      // #2a2927
        }
    }
    static juce::Colour text (Kind k)
    {
        switch (k)
        {
            case LIVE:    return C::BLOOD_HOT;
            case PARTIAL:
            case CANVAS:  return C::OXIDE;
            default:      return C::INK_FAINT;
        }
    }
};

/* ---- spec section 3: fixed per-panel serials ---------------------------- */
namespace SerialNo
{
    inline constexpr const char* RACK     = "N.72-0418";
    inline constexpr const char* LICKS    = "N.72-0419";
    inline constexpr const char* MASS     = "N.72-0420";
    inline constexpr const char* SURVIVOR = "N.72-0421";
    inline constexpr const char* MIXER    = "N.72-0422";
    inline constexpr const char* HWSYNC   = "N.72-0424";
    inline constexpr const char* EXPORT   = "N.72-0426";
}

/* ---- spec section 2: type scale -----------------------------------------
 * IBM Plex Mono 400/500 everywhere; IBM Plex Sans Condensed 700 for stencil.
 * Both shipped as binary resources (app/fonts, juce_add_binary_data).
 * Tracking values are CSS letter-spacing ems, mapped through
 * Font::withExtraKerningFactor. */
struct Type
{
    // raw faces (embedded typefaces; track = extra kerning in em)
    static juce::Font mono       (float size, float track = 0.0f);  // Plex Mono 400
    static juce::Font monoMedium (float size, float track = 0.0f);  // Plex Mono 500
    static juce::Font cond       (float size, float track = 0.0f);  // Plex Sans Condensed 700

    // named roles, spec section 2
    static juce::Font masthead()    { return cond (30.0f, 0.20f); }        // caps
    static juce::Font panelTitle()  { return cond (11.0f, 0.24f); }        // caps
    static juce::Font stencil (float size, float track = 0.20f)            // e.g. SURVIVOR 22
                                    { return cond (size, track); }
    static juce::Font tab()         { return mono (10.0f, 0.18f); }        // caps
    static juce::Font label()       { return monoMedium (9.0f, 0.16f); }   // caps
    static juce::Font micro()       { return mono (8.0f, 0.10f); }         // caps
    static juce::Font nano (float size = 7.0f)
                                    { return mono (size, 0.06f); }         // cell nums, notes
    static juce::Font data()        { return mono (15.0f, 0.04f); }        // readouts
    static juce::Font code()        { return mono (12.0f, 0.0f); }         // expression, lh 1.5
    static juce::Font body()        { return mono (10.0f, 0.0f); }         // manual text, lh 1.5

    /* transitional helper for panels awaiting their pixel pass: a caps
     * label face at an arbitrary size. */
    static juce::Font caps (float size) { return mono (size, 0.10f); }
};

} // namespace morgue
