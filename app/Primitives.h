/* Primitives.h -- the shared parts bin (spec section 16 + HTML frame 10).
 *
 * Every control state MORGUE draws lives here: MorgueLookAndFeel (kills all
 * JUCE default drawing), EngravedKnob, PlateButton, TroughFader,
 * MeterComponent, StatusBadge, SerialTag, StepCell, and the header-band
 * paint helper.
 *
 * Sync contract (spec section 15): every engine-wired control carries
 *   setValueQuiet()/setToggleStateQuiet()  -- for the 30 Hz engine pull
 *   isUserDragging()                       -- the pull skips dragged controls
 *   onChange / onToggle                    -- UI -> engine writes
 * The engine is the single source of truth; these controls are windows.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>       // every control below carries a std::function

#include "Theme.h"

namespace morgue
{

/* ======================================================================== */
/*  MorgueLookAndFeel -- no default JUCE drawing anywhere (spec rule 0.1).   */
/*  Flat plates, hairline rules, embedded mono type. No gradients, no       */
/*  rounded corners, no shadows.                                            */
/* ======================================================================== */
class MorgueLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MorgueLookAndFeel();

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // buttons
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    // combo box (SOCKET field with a small triangle; spec section 12 look)
    void drawComboBox (juce::Graphics&, int w, int h, bool down,
                       int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // popup menu
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override;

    // text editor
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;

    // scrollbars: a plain plate in a trough
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h,
                        bool vertical, int thumbStart, int thumbSize,
                        bool mouseOver, bool mouseDown) override;

    // tooltips: flat plate, hairline border, mono type
    void drawTooltip (juce::Graphics&, const juce::String& text, int w, int h) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& text,
                                           juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
};

/* ======================================================================== */
/*  EngravedKnob : Slider  (frame 10 KNOB row; spec sections 5/9/15)         */
/*                                                                          */
/*  Face: CONTROL circle, 1px EDGE ring (BLOOD when hovered/dragging/       */
/*  MIDI-bound), a 1px INK cut 15 long starting at top+4 rotating           */
/*  -135..+135 about the centre, inner 1px KNOB_INNER ring inset 9.         */
/*  76px SURVIVOR variant: 2px cut 26 long, inner ring inset 16.            */
/*  Below the face: role label (8px), 000-padded value (10px; 12px on 76),  */
/*  optional 7px INK_GHOST sub-label ("pN" / sub-note).                     */
/*                                                                          */
/*  Interaction (spec section 15): horizontal drag 1 unit / 2px; cmd-drag   */
/*  fine 1 unit / 8px; double-click = default; right-click = MIDI learn     */
/*  hook; scroll = +-1.                                                     */
/* ======================================================================== */
class EngravedKnob : public juce::Slider
{
public:
    /* label = role text under the face; diameterPx one of 20/26/32/34/40/44/76 */
    explicit EngravedKnob (juce::String label, int diameterPx = 44,
                           int min = 0, int max = 255, int defaultValue = 0);

    // ---- engine-sync contract ----
    void setValueQuiet (int v);                 // 30 Hz pull; no callback
    int  value() const noexcept;                // current int value
    bool isUserDragging() const noexcept { return dragging; }
    std::function<void (int)> onChange;         // UI -> engine
    std::function<void()> onLearnRequest;       // right-click (R8 MIDI learn)

    // ---- presentation ----
    void setLabelText (const juce::String&);    // role label (8px under face)
    void setRole (const juce::String& r);       // alias; "UNUSED" => dead state
    void setSubLabel (const juce::String&);     // 7px INK_GHOST line ("p3", "STUTTER")
    void setValueText (const juce::String&);    // text readout override (NORM / OFF / 1/8);
                                                //   empty => zero-padded number
    void setUnused (bool);                      // frame 10 UNUSED state colours
    void setMidiBound (bool);                   // blood ring while bound
    void setDefaultValue (int);                 // double-click target
    void setDiameter (int px);
    int  diameter() const noexcept { return dia; }
    void setShowText (bool);                    // false: face only (transport pairs
                                                //   draw label/value beside the knob)
    int  idealHeight() const;                   // face + text rows

    // ---- interaction ----
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void paint (juce::Graphics&) override;

private:
    juce::String labelText, subLabel, valueText;
    int dia = 44;
    int defaultVal = 0;
    bool dragging = false, hovered = false, unused = false, midiBound = false;
    bool showText = true;
    int dragStartVal = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngravedKnob)
};

/* ======================================================================== */
/*  PlateButton : Button  (frame 10 SWITCH PLATE row)                        */
/*                                                                          */
/*  Flat rect, 1px border, no radius, optional 5px lamp.                    */
/*  States: idle PLATE/EDGE/INK_DIM (toggle-off PLATE_LOW, dead lamp) ·     */
/*  hover PLATE_HOVER · armed BLOOD_DEEP/BLOOD/ARMED_TEXT + BLOOD_HOT lamp  */
/*  · engaged (CUT) solid BLOOD/#c2301f/INK_BRIGHT · oxide variant          */
/*  OXIDE_PLATE/OXIDE_DIM/OXIDE_INK · disabled DISABLED_BG/HAIRLINE_DIM/    */
/*  INK_GHOST.                                                              */
/* ======================================================================== */
class PlateButton : public juce::Button
{
public:
    /* toggles=false => action plate (never latches; onToggle fires with
     * the current state, which stays false unless set from sync()). */
    explicit PlateButton (const juce::String& text, bool withLamp = false,
                          bool toggles = true);

    // ---- engine-sync contract ----
    void setToggleStateQuiet (bool on);         // 30 Hz pull; no callback
    std::function<void (bool)> onToggle;        // UI -> engine (new state)
    bool isUserDragging() const noexcept { return isMouseButtonDown(); }

    // ---- presentation ----
    void setLamp (bool);
    void setLampUnderText (bool);               // transport 64x34 plates
    void setEngagedStyle (bool);                // CUT: on = solid BLOOD
    void setOxideStyle (bool);                  // loop 'O' / choke tags
    void setStencilText (bool, float size = 22.0f); // SURVIVOR 150x74 buttons
    void setSubLine (const juce::String&);      // 8px status line under the word

    void clicked() override;
    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

private:
    bool lamp = false, lampUnder = false, engagedStyle = false, oxideStyle = false;
    bool stencil = false;
    float stencilSize = 22.0f;
    juce::String subLine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlateButton)
};

/* ======================================================================== */
/*  TroughFader : Slider  (frame 10 FADER; spec section 10.5)                */
/*                                                                          */
/*  16px trough TROUGH #080807 with HAIRLINE border, BLOOD fill from the    */
/*  bottom (LAMP_DEAD #2a2927 when muted), 3px INK cap overhanging 3px      */
/*  each side. Range 0..256. Vertical drag; wheel +-8.                      */
/* ======================================================================== */
class TroughFader : public juce::Slider
{
public:
    explicit TroughFader (juce::String name = {});

    // ---- engine-sync contract ----
    void setValueQuiet (int v);
    int  value() const noexcept;
    bool isUserDragging() const noexcept { return dragging; }
    std::function<void (int)> onChange;

    void setMuted (bool);                       // dead fill colour

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void paint (juce::Graphics&) override;

private:
    bool dragging = false, muted = false;
    int dragStartY = 0, dragStartVal = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TroughFader)
};

/* ======================================================================== */
/*  MeterComponent  (frame 10 METER; spec section 10.5)                      */
/*                                                                          */
/*  8px column (or horizontal strip): TROUGH background, HAIRLINE border,   */
/*  BLOOD body, AMBER above -6 dB, BLOOD_HOT segment at the rails. 30 Hz.   */
/*  `source` returns a linear peak 0..1; with no source the trough stays    */
/*  empty -- never fake live data.                                          */
/* ======================================================================== */
class MeterComponent : public juce::Component, private juce::Timer
{
public:
    MeterComponent();

    std::function<float()> source;              // linear peak 0..1, pulled at 30 Hz
    void setHorizontal (bool);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    bool horizontal = false;
    float level = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterComponent)
};

/* ======================================================================== */
/*  StatusBadge  (spec section 1 badge rules)                                */
/*  Bordered 8px caps tag: LIVE blood, PARTIAL/CANVAS oxide, PLANNED dead.  */
/* ======================================================================== */
class StatusBadge : public juce::Component
{
public:
    explicit StatusBadge (Badge::Kind k = Badge::LIVE, juce::String text = "LIVE");
    void set (Badge::Kind, const juce::String& text);

    static int  idealWidth (const juce::String& text);
    static void paintBadge (juce::Graphics&, juce::Rectangle<int>,
                            Badge::Kind, const juce::String& text);

    void paint (juce::Graphics&) override;

private:
    Badge::Kind kind;
    juce::String label;
};

/* ======================================================================== */
/*  SerialTag  -- evidence-tag accession string, 8px INK_FAINT.              */
/* ======================================================================== */
class SerialTag : public juce::Component
{
public:
    explicit SerialTag (juce::String text = {});
    void setText (const juce::String&);
    void paint (juce::Graphics&) override;

private:
    juce::String text;
};

/* ======================================================================== */
/*  StepCell -- tri-state sequencer plate (frame 10 STEP CELL; spec 5/15)    */
/*                                                                          */
/*  OFF PANEL/HAIRLINE · HIT HIT_BG/HIT_BD + 6px INK dot · ACCENT           */
/*  BLOOD/BLOOD_HOT + INK_BRIGHT dot · playhead tint PLATE_HOVER/OXIDE_DIM  */
/*  with OXIDE_INK dot. 6px index bottom-right.                             */
/*  Click cycles OFF->HIT->ACCENT->OFF; right-click clears; click-drag      */
/*  paints the starting state across sibling cells.                         */
/* ======================================================================== */
class StepCell : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    enum State { OFF = 0, HIT = 1, ACCENT = 2 };

    explicit StepCell (int index);

    void setState (State s, bool notify = false);   // notify => fires onEdit
    State getState() const noexcept { return st; }
    int  index() const noexcept { return idx; }
    void setPlayhead (bool);
    void setShowIndex (bool);
    bool isUserDragging() const noexcept { return isMouseButtonDown(); }

    std::function<void (int step, State s)> onEdit; // user edits only

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void paint (juce::Graphics&) override;

private:
    int idx;
    State st = OFF;
    State paintState = OFF;                          // drag-paint state
    bool playhead = false, showIndex = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StepCell)
};

/* ======================================================================== */
/*  Header band (spec section 3): every panel begins with a 22-24px band.    */
/*  RAISED bg, bottom HAIRLINE, padding 0 8-10, contents:                    */
/*    + · TITLE · subtitle ... right: serial, status badge                   */
/*  The '+' is the one registration glyph (INK_GHOST). Nothing else.         */
/* ======================================================================== */
inline constexpr int headerBandH = 24;

void paintHeaderBand (juce::Graphics&, juce::Rectangle<int> band,
                      const juce::String& title,
                      const juce::String& subtitle,
                      const juce::String& serial,
                      Badge::Kind badgeKind,
                      const juce::String& badgeText);

/* variant with no badge (e.g. LOCKER / SCOPE headers) */
void paintHeaderBand (juce::Graphics&, juce::Rectangle<int> band,
                      const juce::String& title,
                      const juce::String& subtitle,
                      const juce::String& rightText);

/* 20px label row used inside panels: 9px .16em INK_DIM caps left,
 * 8px INK_FAINT right hint. Transparent background. */
void paintLabelRow (juce::Graphics&, juce::Rectangle<int> row,
                    const juce::String& left, const juce::String& right = {});

/* ---- transport position, read without tearing ---------------------------
 *
 * bb.bar and bb.seq_pos are two separate relaxed atomics, stored back to back
 * at the end of every render period (engine.c). Reading them as two
 * independent loads is a torn read: the audio thread can have published the
 * new bar but not yet the new step, so a caller sees bar N+1 combined with
 * step 15 of bar N and computes a position nearly a whole bar ahead -- which
 * on screen looks like the playhead lurching forward and then snapping back
 * every time a bar turns over. It is most obvious when you are watching one
 * bar boundary repeatedly, which is exactly what overdubbing a loop is.
 *
 * Until the engine publishes the two as one packed value, close the window
 * here: read the bar, read the step, read the bar again, and if it moved
 * underneath us take the step again -- the two stores are adjacent, so one
 * retry catches it. Returns the position in bars as a float, or -1 when the
 * step clock is idle and there is nothing to show. */
float transportPositionBars();

} // namespace morgue
