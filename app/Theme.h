/* Theme.h -- the visual language of MORGUE.
 *
 * This file carries the design-handoff spec:
 *   design_handoff_morgue_gui/MORGUE_UI_SPEC.md section 1 (colour tokens)
 *   and section 2 (type scale).
 *
 * Rules of the look (spec section 0, unchanged): no gradients, no rounded
 * corners (knob faces are circles), no shadows, every rule exactly 1px (2px
 * only for the active tab underline), monospace only, one accent colour
 * (BLOOD), integer-aligned fillRect painting.
 *
 * =====================================================================
 * LEGIBILITY PASS -- READ THIS BEFORE YOU CHANGE A COLOUR
 * =====================================================================
 *
 * The complaint this pass answers: "SUPER hard to see ... I like how dark
 * it is". So the fix is NOT a lighter UI. The ground stays black. What
 * changed is the INK and the STRUCTURE: contrast now comes from brighter
 * text and visible rules against a still-black field, not from lifting
 * panels toward grey. The ramp was also widened DOWNWARD -- SOCKET and
 * TROUGH went darker -- so a well reads as a hole, not as a slightly
 * different shade of the same nothing.
 *
 * ---- the two metrics used here --------------------------------------
 *   cr    WCAG 2.x contrast ratio. Used for anything with an edge you
 *         must see: text on a ground, a 1px rule on a ground.
 *   dL*   CIE L* difference. Used for large adjacent FILLS (panel next to
 *         header band). WCAG ratios are useless down here -- two surfaces
 *         can sit at 1.03:1 and still be 1.5 L* apart or 0.3 L* apart, and
 *         only the L* figure predicts whether the eye finds the edge.
 *         Rule of thumb used throughout: dL* >= 1.2 is a visible fill
 *         step; dL* >= 3 is an obvious one.
 *
 * ---- targets this palette is built to -------------------------------
 *   body / metadata text .................. cr >= 4.5:1 on its ground
 *   large or display text ................. cr >= 3.0:1
 *   disabled text ......................... cr ~ 3.0:1 (readable, clearly
 *                                           subordinate -- never invisible)
 *   1px structural rule (HAIRLINE) ........ cr >= 2.4:1
 *   control edge (EDGE), armed border ..... cr >= 3.0:1
 *   adjacent surface fills ................ dL* >= 1.2
 *
 * ---- surface ramp, measured (L*, and the step to the rung above) -----
 *   TROUGH        #020202   0.55          meter/fader well -- the floor
 *   SOCKET        #040403   1.08  (+0.53) recessed wells, editor, scope
 *   MANUAL_BG     #070706   1.90  (+0.82) field-manual page
 *   DISABLED_BG   #080807   2.17  (+0.27) disabled plate = a hole, not a box
 *   GROUND        #0a0a0a   2.74  (+0.57) window ground (UNCHANGED)
 *   PANEL         #0f0e0d   4.02  (+1.28) panel body, lane field
 *   PANEL_ALT     #131211   5.52  (+1.51) tab strip, status bar, toolbars
 *   TRANSPORT     #171513   6.91  (+1.39) title bar, transport bar
 *   RAISED        #1b1917   8.91  (+2.00) header bands
 *   PLATE_LOW     #1e1c19  10.38  (+1.47) idle plate, toggle-off
 *   PLATE         #222019  12.24  (+1.86) idle button plate
 *   CONTROL       #24211b  12.87  (+0.63) knob face
 *   PLATE_HOVER   #2b2822  16.24  (+3.37) hovered / action plate
 *   TAB_ACTIVE_BG #2e2b26  17.68  (+1.44) active tab, selected locker row
 *
 *   Before this pass every surface in the app lived between L* 1.65 and
 *   9.27 with steps of 0.3-0.9 -- i.e. below the threshold at which the
 *   eye resolves an edge, which is why "structure is invisible". The ramp
 *   now spans L* 0.55 - 17.68 and no adjacent rung is closer than ~1.2,
 *   while the darkest surface got darker and GROUND did not move at all.
 *
 *   Two boundaries are deliberately still shallow -- PANEL/GROUND (dL*
 *   1.28) and PANEL_ALT/PANEL (dL* 1.51). Those two are always separated
 *   by a drawn HAIRLINE, so the rule carries the edge and the fill only
 *   has to hint at it. Do not "fix" them by lifting PANEL.
 *
 * ---- ink ramp, measured on PANEL (cr) --------------------------------
 *   INK_BRIGHT  #f4ece2  L* 93.8   -- text on blood/oxide fills
 *   INK         #ded9ce  L* 86.8  13.70:1   primary text, knob cut
 *   INK_DIM     #b3ada0  L* 70.9   8.63:1   labels
 *   INK_FAINT   #8a8478  L* 55.3   5.19:1   metadata  (was 2.52:1)
 *   INK_GHOST   #625e56  L* 40.0   2.99:1   disabled  (was 1.68:1)
 *
 *   Steps are ~16 L* apart, so each rung is unmistakably a rung. The old
 *   ramp put INK_FAINT at 2.52:1 and INK_GHOST at 1.68:1 and then used
 *   INK_FAINT for every footer, serial, axis label and instruction in the
 *   app -- that one token was most of the complaint. Measured on every
 *   ground it is actually drawn on:
 *     INK_FAINT on  SOCKET 5.52  PANEL 5.19  PANEL_ALT 5.04  TRANSPORT 4.90
 *                   RAISED 4.72  PLATE 4.39  CONTROL 4.32
 *   It clears 4.5:1 on every ground that carries running metadata. The two
 *   exceptions are PLATE and CONTROL, which are control faces, not text
 *   grounds -- do not set metadata type on a button plate or a knob face.
 *   Use INK_DIM (7.30 on PLATE, 7.19 on CONTROL) there.
 *
 * ---- rules, measured on PANEL (cr) -----------------------------------
 *   HAIRLINE_FAINT #322f2b  1.45:1   table row separators (was 1.06:1 --
 *                                    and byte-identical to PLATE, i.e. a
 *                                    literal 1.00:1 on the ground it was
 *                                    most often drawn against. Now 1.31:1
 *                                    on PLATE.)
 *   HAIRLINE_DIM   #423f39  1.84:1   inner sub-rules      (was 1.14:1)
 *   HAIRLINE       #565249  2.48:1   all borders/rules    (was 1.24:1)
 *   EDGE           #6a665c  3.37:1   raised control border(was 1.68:1)
 *   The three-level hairline system is kept and re-spaced ~7-8 L* apart,
 *   so FAINT/DIM/HAIRLINE now actually read as three weights. Discipline
 *   is unchanged: 1px, flat, no glow, no second pixel.
 *
 * ---- accents, and why they still mean something ----------------------
 *   BLOOD      #bb2d1e  L* 42.1  cr 3.22 on PANEL, 3.46 on TROUGH
 *   BLOOD_DEEP #551a12  L* 18.9  armed plate fill
 *   BLOOD_HOT  #fa5636  L* 59.3  cr 5.94 on PANEL -- lamps, playhead, CLIP
 *   OXIDE      #ab7135  L* 52.7  cr 4.72 on PANEL -- fills, strokes, tags
 *   OXIDE_DIM  #7d5228  L* 38.8  oxide borders
 *   OXIDE_INK  #e2b87e  L* 77.3  cr 10.46 -- USE THIS FOR OXIDE-COLOURED
 *                                TEXT. OXIDE itself is a fill/stroke token
 *                                and only just clears 4.5:1.
 *   AMBER      #f2ba45  L* 78.7  cr 10.91 -- warn, >-6dB, CPU warn
 *   GREEN_FAINT#9aa96a  L* 66.7  cr 7.58 -- "STREAM OK" / "COMPILED" only
 *
 *   ONE ACCENT still means one accent: BLOOD/BLOOD_DEEP/BLOOD_HOT are for
 *   armed, live and danger. Nothing else. A control that does not do
 *   anything must not be painted in it.
 *
 * ---- colour-blind rule (this is a constraint, not a nicety) ----------
 *   Under deuteranopia/protanopia BLOOD and OXIDE both collapse toward
 *   the same olive-brown, so hue alone cannot carry state. Two defences,
 *   both of which you must preserve:
 *
 *   (a) The luminance ladder is monotonic where state is a scale. The
 *       lamp triple reads correctly in pure greyscale:
 *         LAMP_DEAD L* 28.0  <  LAMP_SOUNDING L* 42.0  <  BLOOD_HOT L* 59.3
 *       and BLOOD (42.1) sits 17.2 L* below BLOOD_HOT and 10.6 below OXIDE.
 *
 *   (b) ARMED and DISABLED differ on all three channels in luminance
 *       alone, so an armed control is never mistakable for a dead one:
 *         fill    BLOOD_DEEP  vs DISABLED_BG   dL* 16.7
 *         border  BLOOD       vs HAIRLINE_DIM  dL* 15.3
 *         text    ARMED_TEXT  vs INK_GHOST     dL* 51.2
 *
 *   Therefore: never encode a state as "blood instead of oxide" and
 *   nothing else. Pair the hue with a fill change, a border weight, or a
 *   different glyph/word. A readout whose string never changes and whose
 *   only signal is its colour (the old CLIP indicator) is a bug.
 *
 * ---- type: there is now a floor, and it is enforced ------------------
 *   See struct Type at the bottom of this file. Short version: the mono
 *   faces clamp to Type::kMinSize (8px) and taper their tracking as size
 *   drops, inside Theme.cpp's make(). Call sites asking for 6px or 7px
 *   get 8px. Letterspaced caps at 6-8px was the least legible combination
 *   available and it was carrying most of the app's data.
 * =====================================================================
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace morgue
{

/* UTF-8 literal helper: JUCE's String(const char*) is not UTF-8 aware, and
 * the spec copy is full of middle dots, arrows and dashes. Wrap every
 * literal that carries a multi-byte character. */
inline juce::String U8 (const char* s) { return juce::String::fromUTF8 (s); }

/* ---- the command modifier, spelled the way the player's keyboard spells it
 * JUCE already maps ModifierKeys::commandModifier onto the correct physical
 * key -- Cmd on macOS, Ctrl on Windows and Linux -- so the code that reads
 * modifiers needs no #if at all. The printed copy does: a Windows player has
 * no Cmd key, and U+2318 is not on any cap they own (nor in IBM Plex Mono),
 * so the field manual's key cap and the knob tooltips would both be lying.
 *   modKeyGlyph() is the key-cap form   (U+2318 / "CTRL+")
 *   modKeyWord()  is the running-prose form ("cmd" / "ctrl"), which sits
 *                 inside sentences such as "cmd-drag fine". */
juce::String modKeyGlyph();
juce::String modKeyWord();

/* ---- spec section 1: colour tokens --------------------------------------
 * Every name here is load-bearing -- panels reference these by name, so
 * names are never removed or repurposed, only re-valued. The trailing
 * figures are the measurements from the header comment: L* for surfaces
 * (how light the fill is) and cr for anything with an edge (contrast
 * against the ground it is normally drawn on). If you change a value,
 * re-measure and update the number, or the next person is flying blind. */
namespace C
{
    /* -- surfaces, darkest to lightest. Ground stays black; the ramp was
     *    widened in both directions. See the table in the header comment. */
    inline const juce::Colour TROUGH         { 0xff020202 };  // L*  0.55  fader/meter trough
    inline const juce::Colour SOCKET         { 0xff040403 };  // L*  1.08  recessed wells, code editor, scope
    inline const juce::Colour MANUAL_BG      { 0xff070706 };  // L*  1.90  field-manual ground
    inline const juce::Colour DISABLED_BG    { 0xff080807 };  // L*  2.17  disabled plate bg (recessed, not raised)
    inline const juce::Colour GROUND         { 0xff0a0a0a };  // L*  2.74  window ground -- UNCHANGED
    inline const juce::Colour PANEL          { 0xff0f0e0d };  // L*  4.02  panel body, lane field
    inline const juce::Colour PANEL_ALT      { 0xff131211 };  // L*  5.52  tab strip, status bar, toolbars
    inline const juce::Colour TRANSPORT      { 0xff171513 };  // L*  6.91  title bar, transport bar
    inline const juce::Colour RAISED         { 0xff1b1917 };  // L*  8.91  header bands
    inline const juce::Colour PLATE_LOW      { 0xff1e1c19 };  // L* 10.38  idle plate, toggle-off variant
    inline const juce::Colour PLATE          { 0xff222019 };  // L* 12.24  idle button plate
    inline const juce::Colour CONTROL        { 0xff24211b };  // L* 12.87  knob face
    inline const juce::Colour PLATE_HOVER    { 0xff2b2822 };  // L* 16.24  hovered/action plate (dL* 4.0 over PLATE)
    inline const juce::Colour TAB_ACTIVE_BG  { 0xff2e2b26 };  // L* 17.68  active tab / selected locker row

    /* -- rules and edges. 1px, flat, no glow. Three weights, ~7-8 L* apart. */
    inline const juce::Colour HAIRLINE_FAINT { 0xff322f2b };  // cr 1.45 on PANEL, 1.31 on PLATE -- table row separators
    inline const juce::Colour HAIRLINE_DIM   { 0xff423f39 };  // cr 1.84 on PANEL -- inner sub-rules
    inline const juce::Colour HAIRLINE       { 0xff565249 };  // cr 2.48 on PANEL -- all borders/rules
    inline const juce::Colour EDGE           { 0xff6a665c };  // cr 3.37 on PANEL, 2.80 on CONTROL -- raised control border

    /* -- ink. Four rungs, ~16 L* apart, ordered and each step perceptible. */
    inline const juce::Colour INK_BRIGHT     { 0xfff4ece2 };  // L* 93.8  text on blood/oxide fills
    inline const juce::Colour INK            { 0xffded9ce };  // cr 13.70 on PANEL -- primary text, knob cut
    inline const juce::Colour INK_DIM        { 0xffb3ada0 };  // cr  8.63 on PANEL -- labels
    inline const juce::Colour INK_FAINT      { 0xff8a8478 };  // cr  5.19 on PANEL, 4.72 on RAISED -- metadata
    inline const juce::Colour INK_GHOST      { 0xff625e56 };  // cr  2.99 on PANEL -- disabled (subordinate, not invisible)

    /* -- the one accent: armed, live, danger. Nothing else gets to use it. */
    inline const juce::Colour BLOOD          { 0xffbb2d1e };  // cr 3.22 on PANEL, 3.46 on TROUGH -- armed border, fader/meter fill
    inline const juce::Colour BLOOD_DEEP     { 0xff551a12 };  // L* 18.9, dL* 6.6 over PLATE -- armed plate background
    inline const juce::Colour BLOOD_HOT      { 0xfffa5636 };  // cr 5.94 on PANEL -- lamps, playhead, CLIP

    /* -- rusted oxide secondary. OXIDE is a FILL/STROKE token; for oxide
     *    coloured TEXT use OXIDE_INK, which clears 4.5:1 on every ground. */
    inline const juce::Colour OXIDE          { 0xffab7135 };  // cr 4.72 on PANEL -- sends, automation, tags
    inline const juce::Colour OXIDE_DIM      { 0xff7d5228 };  // L* 38.8  oxide borders
    inline const juce::Colour OXIDE_INK      { 0xffe2b87e };  // cr 10.46 on PANEL -- oxide-coloured text

    /* -- warn / ok. AMBER sits above INK_DIM so a warning outranks a label. */
    inline const juce::Colour AMBER          { 0xfff2ba45 };  // cr 10.91 on PANEL -- warn, meter above -6dB, CPU warn
    inline const juce::Colour GREEN_FAINT    { 0xff9aa96a };  // cr  7.58 on PANEL -- "STREAM OK" / "COMPILED"

    /* -- supporting literals the spec / HTML use repeatedly (frame 10 + panels).
     *    The lamp triple DEAD < SOUNDING < BLOOD_HOT is monotonic in
     *    luminance (28.0 / 42.0 / 59.3), so lamp state survives greyscale. */
    inline const juce::Colour LAMP_DEAD       { 0xff454239 };  // L* 28.0  dead lamp, muted fader fill
    inline const juce::Colour LAMP_SOUNDING   { 0xff666359 };  // L* 42.0  sounding-voice lamp
    inline const juce::Colour TAB_INACTIVE_FG { 0xff837d71 };  // cr 4.58 on PANEL_ALT -- inactive tab text
    inline const juce::Colour KNOB_INNER      { 0xff121110 };  // dL* -7.8 under CONTROL -- knob inner inset ring
    inline const juce::Colour KNOB_UNUSED_RING{ 0xff4a463f };  // cr 1.71 on CONTROL -- UNUSED knob ring (was 1.16)
    inline const juce::Colour ARMED_TEXT      { 0xfff0e4da };  // cr 10.90 on BLOOD_DEEP -- text on armed plates
    inline const juce::Colour HIT_BG          { 0xff423c33 };  // dL* 15.3 over PLATE_LOW -- step cell HIT fill
    inline const juce::Colour HIT_BD          { 0xff726c62 };  // cr 2.10 on HIT_BG -- step cell HIT border
    inline const juce::Colour CELL_NUM_OFF    { 0xff767065 };  // cr 3.93 on PANEL, 3.46 on PLATE_LOW -- step index on OFF cell
    inline const juce::Colour CELL_NUM_ACC    { 0xfff4dcd6 };  // cr 4.58 on BLOOD -- step index on ACCENT cell
    inline const juce::Colour OXIDE_PLATE     { 0xff2e2013 };  // dL* 9.6 over PANEL -- oxide-loaded slot / 'O' engaged plate
}

/* ---- status badge colour rules (spec section 1, bottom) -----------------
 * The badge is how the app tells you whether a thing is real, so it is the
 * one label that must never be the hardest thing on screen to read. Two
 * changes in this pass:
 *   PARTIAL/CANVAS text is OXIDE_INK, not OXIDE -- badges sit on RAISED,
 *     where OXIDE only reaches 4.29:1 and OXIDE_INK reaches 9.51:1.
 *   PLANNED border is HAIRLINE, not LAMP_DEAD -- a badge box you cannot
 *     see is not a box. LAMP_DEAD stays what its name says: a dead lamp.
 * Measured on RAISED: LIVE 5.40:1 / PARTIAL 9.51:1 / PLANNED 4.72:1. */
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
            default:      return C::HAIRLINE;
        }
    }
    static juce::Colour text (Kind k)
    {
        switch (k)
        {
            case LIVE:    return C::BLOOD_HOT;
            case PARTIAL:
            case CANVAS:  return C::OXIDE_INK;
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
 * Font::withExtraKerningFactor.
 *
 * =====================================================================
 * THE FLOOR, AND WHY IT IS ENFORCED HERE INSTEAD OF AT CALL SITES
 * =====================================================================
 * Before this pass the 6-9px band carried almost all of the app's data:
 * step numbers at 6px, knob sub-labels at 7px, ~77 sites at 8px, every
 * panel footer, every axis label. Letterspaced caps at 6-8px is the least
 * legible combination this typeface offers, and it was the default.
 *
 * Two systemic rules, applied inside Theme.cpp's make() so that ~130 call
 * sites are fixed without any panel having to touch its font calls:
 *
 *   1. SIZE FLOOR. The mono faces clamp to kMinSize (8px). A call site
 *      asking for Type::nano(6.0f) gets 8px. This is deliberate: the
 *      floor belongs to the design system, not to whoever last edited a
 *      panel. If you genuinely need smaller, you need less text instead.
 *
 *   2. TRACKING TAPER. Tracking helps display caps and hurts small text --
 *      at 7-8px the extra gap breaks the word shape faster than it opens
 *      the counters. So tracking is capped by size:
 *            < 9px -> 0.05em      < 10px -> 0.09em
 *           < 12px -> 0.14em      otherwise -> as requested
 *      The taper applies to the MONO faces only. cond() is display type
 *      (masthead, panel titles, stencil) where wide tracking is the point
 *      and the sizes are 11px and up, so it passes through untouched.
 *
 * WHAT THIS DOES TO LAYOUT -- important for the panel passes. Plex Mono
 * advances 0.6em/char, so per-character width is size*(0.6 + track).
 * Trading tracking for size makes the roles BIGGER WITHOUT MAKING THEM
 * WIDER, which is the whole trick:
 *      label()  9px/.16 = 6.84px per char  ->  10px/.10 = 7.00   (+2%)
 *      micro()  8px/.10 = 5.60             ->   9px/.05 = 5.85   (+4%)
 *      nano()   7px/.06 = 4.62             ->   8px/.02 = 4.96   (+7%)
 *      tab()   10px/.18 = 7.80             ->  10px/.12 = 7.20   (-8%)
 * Horizontal layouts are safe. VERTICAL boxes are not: a role that grew
 * 1px needs its row to grow ~2px. Type::rowH() gives the minimum glyph
 * box for a size -- a 7px glyph in an 8px box (which several panels do
 * today) clips its own descenders.
 * ===================================================================== */
struct Type
{
    /* Hard minimum for the mono faces, enforced in Theme.cpp make(). */
    static constexpr float kMinSize = 8.0f;

    /* Minimum glyph box for a given face size: size * 1.55, rounded up.
     * rowH(8)=13  rowH(9)=14  rowH(10)=16  rowH(11)=18  rowH(15)=24.
     * Use this when sizing a row, cell or caption strip -- text set into a
     * box shorter than this is clipped, not merely tight. */
    static constexpr float kMinRowFactor = 1.55f;
    static int rowH (float size)
    {
        return (int) std::ceil (juce::jmax (size, kMinSize) * kMinRowFactor);
    }

    // raw faces (embedded typefaces; track = extra kerning in em).
    // mono/monoMedium apply the size floor and the tracking taper.
    // cond applies the floor only -- display type keeps its tracking.
    static juce::Font mono       (float size, float track = 0.0f);  // Plex Mono 400
    static juce::Font monoMedium (float size, float track = 0.0f);  // Plex Mono 500
    static juce::Font cond       (float size, float track = 0.0f);  // Plex Sans Condensed 700

    // named roles, spec section 2. Sizes/tracking re-cut for legibility;
    // see the width table above -- these are near width-neutral.
    static juce::Font masthead()    { return cond (30.0f, 0.20f); }        // caps
    static juce::Font panelTitle()  { return cond (11.0f, 0.22f); }        // caps
    static juce::Font stencil (float size, float track = 0.20f)            // e.g. SURVIVOR 22
                                    { return cond (size, track); }
    static juce::Font tab()         { return mono (10.0f, 0.12f); }        // caps  (was 10/.18)
    static juce::Font label()       { return monoMedium (10.0f, 0.10f); }  // caps  (was  9/.16)
    static juce::Font micro()       { return mono (9.0f, 0.05f); }         // caps  (was  8/.10)
    static juce::Font nano (float size = kMinSize)
                                    { return mono (size, 0.02f); }         // cell nums, notes (was 7/.06)
    static juce::Font data()        { return mono (15.0f, 0.04f); }        // readouts
    static juce::Font code()        { return mono (12.0f, 0.0f); }         // expression, lh 1.5 -- editor metrics depend on this, do not move
    static juce::Font body()        { return mono (11.0f, 0.0f); }         // manual text, lh 1.5 (was 10)

    /* transitional helper for panels awaiting their pixel pass: a caps
     * label face at an arbitrary size. Tracking reduced to .06 -- the
     * taper will trim it further below 9px. */
    static juce::Font caps (float size) { return mono (size, 0.06f); }
};

} // namespace morgue
