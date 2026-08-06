/* MixerPanel.h -- MIXER stage (spec section 10, serial N.72-0422).
 *
 * Rebuilt around the RETURN BUS (RETURN BUS IMPLEMENTATION CONTRACT v1,
 * which supersedes DESIGN_SPEC.md R4 "FX INSERTS + SENDS"). The R4/R5 drawn
 * state that used to fill this panel -- INSERTS, SENDS B-D, PAN, SOLO, the
 * dead RETURN B strip -- is GONE. Every control on this panel now moves an
 * engine value; nothing is painted that cannot be touched.
 *
 * THE PROBLEM THIS LAYOUT SOLVES. The engine exposes a 12 x 8 send matrix
 * (8 voices + LICKS + DRY master + WET feedback row + the GRAIN MASS well bus,
 * into 8 return slots) plus an 8 x 8 return->return link matrix. Ninety-six
 * send knobs and sixty-four link knobs is not a mixer, it is a punch card. So:
 *
 * NOTE, AND IT IS THE NEXT JOB ON THIS PANEL: MASS is a source in the matrix
 * but it has NO CHANNEL STRIP, so the only way to dial a well into a return is
 * to click a cell in the small grid in the routing dock. Every other source
 * has a strip with a fader, a meter and a live send knob. That asymmetry is
 * the thing the console condense is meant to remove -- one strip grammar over
 * voices, sampler slots, wells and returns -- and the wells can now hold up
 * their end of it, having a real 0..256 level and a real peak since they moved
 * into the engine.
 *
 *   FOCUSED RETURN. Exactly one return slot is focused at a time. Every
 *   channel strip carries ONE live send knob, and it sends into the focused
 *   return -- the strip's SEND label carries the focused slot's letter and
 *   name so it can never be ambiguous. Under the knob sits an 8-cell
 *   FINGERPRINT: this source's send into every slot, at a glance, click to
 *   focus. One knob per strip, full matrix reachable in one click.
 *
 *   SEND MATRIX. The whole 12 x 8 grid, drawn small, in the routing dock.
 *   Every cell is editable in place (drag vertical), every column header
 *   focuses its slot. It is the overview, not the primary editor.
 *
 *   LINK GRID. 8 x 8, rows FROM, columns TO, with the diagonal marked by a
 *   registration cross because the diagonal is the freeze/regeneration cell
 *   and the single most dangerous control on the panel. Every edge that
 *   sits on a CYCLE is drawn in blood, the cycle is named in the footer
 *   ("LOOP A>C>A") and the dock header goes red -- the user sees the loop
 *   exists before it screams.
 *
 *   RETURN RACK. Eight slot rows: letter, name, type, level, mute, meter,
 *   gain-reduction. Empty slots are drawn as empty and click to create.
 *
 *   INSPECTOR. The focused return only: name field, type picker, the shared
 *   parameter knobs (labels pulled from ret_param_name, greyed where the
 *   type does not use the knob), the sync division, GR meter, DESTROY.
 *
 * SAFETY IS VISIBLE, NOT ADVISORY. bb.ret[r].gr is the limiter's gain
 * reduction (Q8, 256 = none). Any slot below 256 lights an amber LIM tag in
 * its rack row and its GR bar; below -12 dB the tag goes blood. The footer
 * carries FB PANIC (bb_engine_ret_panic) at all times, because a runaway
 * loop is a hearing-safety event and the escape must not be in a menu.
 *
 * SYNC CONTRACT (spec section 15). sync() pulls ONE snapshot of the whole
 * return system from the engine at 30 Hz through the bb_engine_ret_*
 * accessors, diffs it against the last one, and only then repaints. No
 * control on this panel stores truth: dragged controls are skipped
 * (isUserDragging), everything else is overwritten from the engine every
 * frame. Meters read bb.ret[r].peak / bb.layer[L].peak / the sink ring
 * directly at 30 Hz, read-and-clear, and decay on the UI side.
 *
 * ENGINE GAPS, still drawn honestly: the LICKS strip has no bus level, mute
 * or peak in the engine (its CHAMBER-era send is live), and there is no pan
 * and no per-voice solo. Those strips keep an empty trough rather than a
 * fake number. That is why the header badge is PARTIAL.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "Theme.h"
#include "Primitives.h"
#include "bytebeat.h"

namespace morgue
{

class MixerPanel : public juce::Component
{
public:
    MixerPanel();
    void resized() override;
    void sync();                      // 30 Hz pull, driven by MainComponent
    void paint (juce::Graphics&) override;

private:
    /* ==================================================================== */
    /*  The 30 Hz snapshot.                                                  */
    /*                                                                       */
    /*  Everything this panel draws is copied out of the engine ONCE per     */
    /*  frame into here, through the bb_engine_ret_* accessors (which        */
    /*  redirect slot 0's LEVEL/P0/P1 and send column 0 onto the legacy      */
    /*  verb_* / layer[].send atomics). Children paint from this and never   */
    /*  touch an atomic in paint(), so a repaint can never disagree with     */
    /*  itself across two controls. `pod` is plain ints so one memcmp says   */
    /*  whether anything moved.                                             */
    /* ==================================================================== */
    struct Snap
    {
        struct Pod
        {
            int type    [BB_NRET];
            int level   [BB_NRET];
            int mute    [BB_NRET];
            int division[BB_NRET];              // 0 = free, 1..10 = clocked
            int gr      [BB_NRET];              // Q8, 256 = no reduction
            int pending [BB_NRET];              // create/destroy in flight
            int param   [BB_NRET][BB_RET_NPARAM];
            int send    [BB_RET_NSRC][BB_NRET];
            int link    [BB_NRET][BB_NRET];
            int live    [BB_NRET];              // type is a type we can run
            int nodeLoop[BB_NRET];              // slot sits on a cycle
            int edgeLoop[BB_NRET][BB_NRET];     // this link closes a cycle
            int active;                         // bb.ret_active
            int anyLoop;
            int worstGr;
        };
        Pod pod {};
        juce::String name[BB_NRET];             // bb_ret_name, or the type
        juce::String loopPath;                  // "A>C>A", shortest cycle
    };

    Snap snap;
    int  focus = 0;                             // focused return slot 0..7

    /* what the strip send-knob tooltips were last built for, so they are not
     * rebuilt on every frame the limiter moves */
    juce::String stripTipTitle;
    bool stripTipLive = false;

    /* helpers shared by every child (all read `snap`, never the engine) */
    static juce::String slotTag (int slot);     // "A".."H"
    static juce::String srcName (int src);      // "V01".."WET"
    static juce::String divName (int division); // "FREE", "1/16", "1/1"...
    juce::String  typeName (int slot) const;    // type word, or "TYPE nn"
    juce::String  slotTitle (int slot) const;   // "A CHAMBER"
    juce::Colour  slotInk (int slot) const;     // blood on a loop, else ink
    void setFocus (int slot);
    void createMenu (int slot, juce::Component* target);
    void rowMenu (int slot, juce::Component* target);
    void refreshChildren();                     // push snapshot -> controls

    /* ==================================================================== */
    /*  Channel strip: 8 voices + LICKS + MASS + MASTER. Returns are NOT      */
    /*  strips any more -- they live in the rack, because eight of them would */
    /*  eat the console.                                                      */
    /* ==================================================================== */
    class Strip : public juce::Component
    {
    public:
        enum class Kind { Voice, Licks, Mass, Master };

        Strip (MixerPanel& owner, Kind k, int layerIndex, int sendSource);

        void resized() override;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        void update (const juce::String& newName, bool newMuted,
                     bool newHot, int newValue);

        const Kind kind;
        const int  layer;                 // engine layer for Voice, else -1
        const int  src;                   // BB_RET_SRC_* this strip feeds

        std::unique_ptr<TroughFader>    fader;
        std::unique_ptr<PlateButton>    muteBtn;
        std::unique_ptr<MeterComponent> meter;
        std::unique_ptr<EngravedKnob>   sendKnob;   // -> the FOCUSED return

    private:
        juce::String dbText() const;
        juce::Rectangle<int> fingerprintCell (int slot) const;

        MixerPanel& panel;
        juce::String name;
        bool muted = false, hot = false;
        int  value = 0;
        const char* route = "MASTER";
        const char* drawnDb = nullptr;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Strip)
    };

    /* ==================================================================== */
    /*  Return rack: eight slot rows. Level, mute, meter, GR, create,        */
    /*  destroy, focus.                                                      */
    /* ==================================================================== */
    class RetRack : public juce::Component,
                    public juce::TooltipClient
    {
    public:
        explicit RetRack (MixerPanel& owner);
        void refresh();                   // meter visibility follows the slots
        void resized() override;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        juce::String getTooltip() override;

        int  rowH() const;
        juce::Rectangle<int> rowArea (int slot) const;
        bool isUserDragging() const noexcept { return dragSlot >= 0; }

    private:
        juce::Rectangle<int> levelArea (int slot) const;
        juce::Rectangle<int> muteArea  (int slot) const;
        int rowAt (juce::Point<int>) const;

        MixerPanel& panel;
        juce::OwnedArray<MeterComponent> meters;
        int dragSlot = -1, dragStartY = 0, dragStartVal = 0;
        int hoverSlot = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RetRack)
    };

    /* ==================================================================== */
    /*  Send matrix: 11 sources x 8 slots, editable in place.                */
    /* ==================================================================== */
    class SendMatrix : public juce::Component,
                       public juce::TooltipClient
    {
    public:
        explicit SendMatrix (MixerPanel& owner);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        juce::String getTooltip() override;
        bool isUserDragging() const noexcept { return dragging; }

    private:
        juce::Rectangle<int> cell (int src, int slot) const;
        juce::Rectangle<int> header (int slot) const;
        bool hit (juce::Point<int>, int& src, int& slot) const;

        MixerPanel& panel;
        bool dragging = false;
        int  dragSrc = -1, dragSlot = -1, dragStartY = 0, dragStartVal = 0;
        int  hoverSrc = -1, hoverSlot = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SendMatrix)
    };

    /* ==================================================================== */
    /*  Link grid: 8 x 8, rows FROM, columns TO. Cycles in blood.            */
    /* ==================================================================== */
    class LinkGrid : public juce::Component,
                     public juce::TooltipClient
    {
    public:
        explicit LinkGrid (MixerPanel& owner);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        juce::String getTooltip() override;
        bool isUserDragging() const noexcept { return dragging; }

    private:
        juce::Rectangle<int> cell (int from, int to) const;
        bool hit (juce::Point<int>, int& from, int& to) const;

        MixerPanel& panel;
        bool dragging = false;
        int  dragFrom = -1, dragTo = -1, dragStartY = 0, dragStartVal = 0;
        int  hoverFrom = -1, hoverTo = -1;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinkGrid)
    };

    /* ==================================================================== */
    /*  Inspector: the focused return's type, name and parameter knobs.      */
    /* ==================================================================== */
    class Inspector : public juce::Component
    {
    public:
        explicit Inspector (MixerPanel& owner);
        void resized() override;
        void paint (juce::Graphics&) override;
        void refresh();                       // pull from panel.snap

        void beginRename();

    private:
        void commitName();

        MixerPanel& panel;
        juce::TextEditor nameEd;
        std::unique_ptr<PlateButton> typeBtn, destroyBtn;
        juce::OwnedArray<EngravedKnob> params;
        std::unique_ptr<EngravedKnob> syncKnob;
        juce::Rectangle<int> grArea, statArea;
        int lastFocus = -1, lastType = -1;   // what the knob labels were built for

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Inspector)
    };

    juce::OwnedArray<Strip> strips;
    std::unique_ptr<RetRack>    rack;
    std::unique_ptr<SendMatrix> matrix;
    std::unique_ptr<LinkGrid>   links;
    std::unique_ptr<Inspector>  inspector;
    std::unique_ptr<PlateButton> addBtn, panicBtn;

    juce::Rectangle<int> stripsArea, dockArea, dockLabelArea, footerArea;
    juce::Rectangle<int> rackArea, matrixArea, linkArea, inspectorArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};

} // namespace morgue
