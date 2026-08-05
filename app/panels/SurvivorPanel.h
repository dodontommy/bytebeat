/* SurvivorPanel.h -- SURVIVOR, THE LOOP BANK (spec section 9, serial
 * N.72-0421, LIVE).
 *
 * =====================================================================
 * WHAT CHANGED AND WHY
 * =====================================================================
 * This panel used to be ONE looper's controls: three big plates, one
 * waveform, six knobs, all hardwired to bb.loop_* -- the master phrase
 * looper, which captures the whole finished bus. You cannot build layers
 * with it, because everything you play after a capture is recorded on top
 * of a bus that already contains that capture.
 *
 * The engine now carries SIX loopers (engine.h, THE LOOP BANK). Slot 0 IS
 * the master phrase looper -- same buffer, same DSP, same atomics, reached
 * through the bank API by an alias. Slots 1..5 are additive satellites that
 * record BB_LOOP_SRC_LIVE by default: the bus at the INPUT of the loop
 * stage, which contains no looper's playback at all. So this panel is now a
 * six-lane strip over a detail block for one focused slot.
 *
 * =====================================================================
 * LEGIBILITY IS THE WHOLE JOB HERE
 * =====================================================================
 * A looper is played by glance, on stage, from across a room, while your
 * hands are somewhere else. Five states have to be told apart instantly and
 * they must survive greyscale (Theme.h's colour-blind rule: never encode
 * state as a hue and nothing else). So every lane row carries a STENCIL
 * WORD as its primary signal, and hue/fill/lamp only confirm it:
 *
 *   EMPTY    INK_GHOST word, dead lamp, panel fill      nothing captured
 *   HELD     INK_DIM word,   dead lamp, panel fill      audio, not playing
 *   ARMED    AMBER word,     amber lamp, PANEL_ALT      waiting for a boundary
 *   REC      INK_BRIGHT word on a BLOOD_DEEP ROW FILL   writing the buffer
 *   OVERDUB  BLOOD_HOT word, blood lamp, PANEL_ALT      playing AND writing
 *   PLAY     INK word,       sounding lamp, PANEL_ALT   playing
 *   MUTED    INK_GHOST word, dead lamp, PANEL_ALT       playing, not heard
 *
 * Five different WORDS, three different FILLS, three different LAMP
 * luminances. No pair of states differs by hue alone.
 *
 * =====================================================================
 * WHY THERE IS A FOCUSED SLOT
 * =====================================================================
 * Every slot carries eleven controls. Six slots is sixty-six, and thirty-six
 * of them are 76px knobs -- that is a punch card, not an instrument. So the
 * panel splits the way the MIXER split for the return bus:
 *
 *   THE LANE STRIP carries everything you reach for WHILE PLAYING and one
 *   thing more: source, state, resolved length, the queued command, the
 *   position, level, mute, ARM/PLAY/STOP/CLEAR and the ARRANGE commit. All
 *   six rows, always, no mode.
 *
 *   THE DETAIL BLOCK carries the mangling -- level/mix, feedback, overdub,
 *   half/double, reverse, slice -- plus the capture length and the source
 *   picker, for ONE focused slot. Clicking anywhere on a row focuses it, and
 *   any transport press focuses the row it came from, so the block always
 *   shows the looper you just touched.
 *
 * =====================================================================
 * EVERY INDICATOR ON THIS PANEL IS ENGINE STATE
 * =====================================================================
 * The audit that removed the app's decorative meters applies here in full.
 * Nothing on this panel animates unless the engine moved:
 *
 *   position well   the slot's real buffer (bb_engine_loop_slot_buffer)
 *                   drawn as a peak envelope, with the playhead at
 *                   bb_engine_loop_pos. An empty slot draws an empty well
 *                   and says EMPTY.
 *   capture bar     while RECORDING the engine publishes no write cursor,
 *                   so the bar is the TRANSPORT's own progress between the
 *                   bar the capture started on (latched from a real status
 *                   edge) and the resolved target -- a clock, not a level.
 *                   It is labelled in bars, not drawn as audio.
 *   peak            bb_engine_loop_peak(), read-and-clear, decayed here.
 *   queue chip      bb_engine_loop_pending(), with its progress taken from
 *                   the transport (bar quantum) or from a PLAYING reference
 *                   layer of cycle length (cycle quantum). When no reference
 *                   exists the chip draws the word and NO progress rather
 *                   than a guess.
 *   CYCLE readout   bb_engine_loop_cycle_bars(), 0 = none established yet.
 *
 * =====================================================================
 * SYNC CONTRACT (spec section 15)
 * =====================================================================
 * sync() is the only place the engine is read. It pulls ONE snapshot of all
 * six slots at 30 Hz through the bb_engine_loop_* accessors, pushes it into
 * the children (skipping anything the user is dragging), and repaints only
 * when something moved. No control here stores truth; the lane strip paints
 * from the snapshot and never touches an atomic in paint().
 *
 * =====================================================================
 * WIRING (Main.cpp)
 * =====================================================================
 * commitToArrange is the bridge from jamming to a finished piece: a slot's
 * -> ARR cell snapshots the loop into a clip buffer (bb_engine_loop_clip,
 * which FREEZES the slot by turning its overdub off -- said on the button,
 * not hidden) and hands it to ARRANGE. Ownership transfers only if the
 * callback returns true; otherwise this panel releases the buffer.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "Theme.h"
#include "Primitives.h"
#include "bytebeat.h"
#include "engine.h"

namespace morgue
{

/* ======================================================================== */
/*  HardPlate -- a PlateButton that reports the MODIFIER it was hit with.    */
/*                                                                          */
/*  Every transport action in the bank is quantised; right-click or the      */
/*  command/ctrl modifier makes it HARD (LBC_HARD, skip the quantum). JUCE   */
/*  hands the modifiers to Button::clicked(const ModifierKeys&), which       */
/*  PlateButton does not override, so this is a two-line subclass rather     */
/*  than a mouse handler that would have to re-implement plate hit testing.  */
/*  These are ACTION plates: their lit state comes from sync(), never from   */
/*  the click, because the engine decides when a command lands.              */
/* ======================================================================== */
class HardPlate : public PlateButton
{
public:
    HardPlate (const juce::String& text, bool withLamp = false)
        : PlateButton (text, withLamp, false) {}

    std::function<void (bool hard)> onFire;

    /* PlateButton overrides the no-argument clicked(), which HIDES the
     * modifier-carrying overload for name lookup in this class. Pull it back
     * in so the compiler is not warning about a hidden virtual that we are
     * deliberately not using. */
    using PlateButton::clicked;

    void clicked (const juce::ModifierKeys& m) override
    {
        if (onFire) onFire (m.isPopupMenu() || m.isCommandDown());
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HardPlate)
};

/* ======================================================================== */
/*  SurvivorPanel                                                           */
/* ======================================================================== */
class SurvivorPanel : public juce::Component
{
public:
    SurvivorPanel();

    /* ---- Main.cpp wiring ------------------------------------------------
     * Hand a finished loop to the ARRANGE timeline. `audio` is a fresh clip
     * buffer from bb_engine_loop_clip(); `bars` is the bar count it was
     * RECORDED at (frame 0 is a downbeat by construction, so it is placed
     * with no offset and loop = 1); `lane` is the slot's stored commit lane
     * (L2C_LANE) and `slot` is 0..BB_NLOOP-1 so the receiver can name the
     * clip LOOP-<slot>-<NNNN>.
     *
     * Return TRUE if the timeline took ownership -- it must then release the
     * buffer with bb_engine_clip_release() when the clip is deleted. Return
     * FALSE (or leave the function unset) and this panel releases it. */
    std::function<bool (ArrClipBuf* audio, unsigned bars, int lane, int slot)> commitToArrange;

    void resized() override;
    void sync();                       // 30 Hz engine pull (MainComponent)
    void paint (juce::Graphics&) override;

private:
    /* ==================================================================== */
    /*  The 30 Hz snapshot. Everything drawn is copied out of the engine     */
    /*  ONCE per frame; children paint from this and never read an atomic    */
    /*  in paint(), so two controls can never disagree inside one repaint.   */
    /* ==================================================================== */
    struct SlotSnap
    {
        int status   = LOOP_OFF;
        int pending  = 0;              // LBC_* action, 0 = none queued
        int src      = BB_LOOP_SRC_LIVE;
        int bars     = 0;              // RAW field: 0 = FOLLOW the cycle
        int level    = 0;
        int feedback = 0;
        int overdub  = 0;
        int rate     = LOOP_RATE_NORMAL;
        int reverse  = 0;
        int slice    = 1;
        int mute     = 0;
        int lane     = 0;
        unsigned frames = 0;
        unsigned pos    = 0;
        int barlen   = 0;              // bar length in frames AT CAPTURE

        /* resolved, not raw: the bar count the buffer actually holds */
        int heldBars() const noexcept
        {
            return (frames > 0 && barlen > 0) ? (int) (frames / (unsigned) barlen) : 0;
        }
    };

    struct Snap
    {
        SlotSnap s[BB_NLOOP];
        int cycleBars = 0;             // 0 = no cycle established
        int active    = 0;             // live satellites
        unsigned bar  = 0;             // transport bar
        float barF    = -1.0f;         // fraction through it, < 0 = clock idle
        unsigned barLen = 0;           // bar length in frames RIGHT NOW
        int rate = 44100;
    };

    Snap snap;
    int  focusSlot = 0;

    /* peak, decayed on the UI side: bb_engine_loop_peak() clears on read, so
     * it is read exactly ONCE per sync per slot and held here. */
    float peakUi[BB_NLOOP] = {};

    /* capture progress. The engine publishes no write cursor while a slot is
     * RECORDING, so the bar is driven by the transport between the bar the
     * capture really started on (latched on the observed status edge) and the
     * resolved target. `recFrom < 0` = no capture seen. */
    float recFrom[BB_NLOOP] = {};
    int   recBars[BB_NLOOP] = {};
    int   lastStatus[BB_NLOOP] = {};

    /* one transient line, for refusals and commits. Counts down in sync(). */
    juce::String note;
    juce::Colour noteInk { C::INK_FAINT };
    int noteTicks = 0;
    void setNote (const juce::String&, juce::Colour ink);

    /* ==================================================================== */
    /*  Waveform envelope cache. The buffers are up to 2 MB each and there   */
    /*  are six of them, so paint() must never walk one: sync() rebuilds a   */
    /*  fixed-column peak envelope when the recorded length changes, and at  */
    /*  a few Hz while a slot is being written into (RECORDING / OVERDUB).   */
    /* ==================================================================== */
    static constexpr int kEnvCols = 512;
    struct Env
    {
        unsigned frames = 0;
        std::vector<float> mag;        // kEnvCols peak magnitudes, 0..1
    };
    Env env[BB_NLOOP];
    int  envTick = 0;
    void rebuildEnv (int slot);
    void drawEnv (juce::Graphics&, juce::Rectangle<int> box, int slot,
                  juce::Colour) const;

    /* ---- shared vocabulary (all read `snap`, never the engine) ---------- */
    struct StateVis { juce::String word; juce::Colour ink, lamp, fill; };
    static StateVis stateVis (const SlotSnap&);
    static juce::String slotTag (int slot);        // "MASTER", "L1".."L5"
    static juce::String srcName (int src);         // "V01".."LIVE"/"MASTER"
    static juce::String srcBlurb (int src);        // one line for the tooltip
    static juce::String laneName (int lane);       // ARRANGE lane label
    static juce::String actionWord (int action);   // "ARM"/"PLAY"/"STOP"/"CLEAR"

    bool  canCommit (int slot) const;
    bool  drifted (int slot) const;                // barlen moved since capture
    float cycleFrac() const;                       // -1 = not derivable

    /* ---- commands ------------------------------------------------------- */
    void fire (int slot, int action, bool hard);
    void commit (int slot);
    void setFocus (int slot);
    void srcMenu (int slot, juce::Component* target);
    void laneMenu (int slot, juce::Component* target);

    /* refreshDetail() runs every frame and only touches things that are cheap
     * and idempotent (toggle states, knob values). The strings -- labels,
     * sub-lines, tooltips -- are rebuilt only when the thing they describe
     * changed, because PlateButton::setSubLine() repaints unconditionally and
     * 30 Hz of identical string building is how a static panel ends up
     * repainting itself forever. */
    void refreshDetail();
    void rebuildDetailText (int slot);

    /* what the detail strings were last built for */
    struct TextKey
    {
        int slot = -1, status = -1, src = -1, bars = -1, held = -1,
            mute = -1, overdub = -1, commit = -1;
    };
    TextKey builtFor;

    /* ==================================================================== */
    /*  LaneStrip -- the six rows. Custom drawn and hit tested, like the     */
    /*  MIXER's return rack: thirty transport cells as child buttons would   */
    /*  cost thirty components and would still not give us the modifier      */
    /*  without subclassing every one of them.                              */
    /* ==================================================================== */
    class LaneStrip : public juce::Component,
                      public juce::TooltipClient
    {
    public:
        explicit LaneStrip (SurvivorPanel& owner);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp   (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        juce::String getTooltip() override;

        bool isUserDragging() const noexcept { return dragSlot >= 0; }

        static constexpr int kHeadH = 16;          // column caption row
        static constexpr int kRowH  = 34;
        static int idealHeight() { return kHeadH + kRowH * BB_NLOOP; }

    private:
        /* One row's columns. Everything in this component is laid out here
         * and nowhere else, so the caption row, the painting and the hit
         * testing can never drift apart. */
        struct Cols
        {
            juce::Rectangle<int> tag, state, src, len, queue, pos, level, mute;
            juce::Rectangle<int> tr[4];            // ARM PLAY STOP CLEAR
            juce::Rectangle<int> arr;
        };
        Cols cols (juce::Rectangle<int> row) const;
        juce::Rectangle<int> rowArea (int slot) const;
        int rowAt (juce::Point<int>) const;

        void paintRow (juce::Graphics&, int slot);
        void paintCell (juce::Graphics&, juce::Rectangle<int>, const juce::String&,
                        bool lit, bool enabled, bool oxide, bool hover) const;

        SurvivorPanel& panel;
        int dragSlot = -1, dragStartX = 0, dragStartVal = 0, dragVal = 0;
        int hoverSlot = -1, hoverCell = -1;        // hoverCell: see Cell enum

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LaneStrip)
    };

    std::unique_ptr<LaneStrip> strip;

    /* ---- global row ----------------------------------------------------- */
    PlateButton allStopBtn { "ALL STOP",  false, false };
    PlateButton allClearBtn { "ALL CLEAR", false, false };

    /* ---- detail block (the focused slot) -------------------------------- */
    HardPlate armBtn   { "ARM",  true };
    HardPlate playBtn  { "PLAY", true };
    HardPlate stopBtn  { "STOP", false };
    HardPlate clearBtn { "CLEAR", false };
    PlateButton commitBtn { "ARR", false, false };   // text set in the ctor (UTF-8)
    PlateButton srcBtn    { "SRC", false, false };
    juce::OwnedArray<PlateButton> lenBtns;        // FOLLOW / 1 / 2 / 4 / 8
    juce::OwnedArray<EngravedKnob> knobs;         // LEVEL FB OD HALF REV SLICE

    /* what each length chip currently WRITES into L2C_BARS. Slot 0's legal
     * range is 1..4 and a satellite's is 0(FOLLOW)/1/2/4/8, so the chips are
     * re-labelled and re-valued on every focus change rather than being two
     * different sets of buttons. */
    int lenVal[5] = { 0, 1, 2, 4, 8 };

    /* ---- geometry -------------------------------------------------------- */
    struct Geom
    {
        juce::Rectangle<int> bankLabel, bank, global,
                             detailLabel, detailTop, detailCtl,
                             waveLabel, wave, knobLabel, knobRow;
    };
    Geom geom() const;

    void paintDetail (juce::Graphics&, const Geom&);
    void paintGlobal (juce::Graphics&, const Geom&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurvivorPanel)
};

} // namespace morgue
