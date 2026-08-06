/* MixerPanel.cpp -- see MixerPanel.h.
 *
 * Everything drawn here is live. The panel pulls ONE snapshot of the return
 * system per 30 Hz frame through the bb_engine_ret_* accessors, diffs it,
 * and repaints only what moved. Writes go straight back through the same
 * accessors, which are single atomic stores taking effect next period --
 * slot 0's LEVEL/P0/P1 and send column 0 redirect onto the legacy
 * verb_level / verb_size / verb_tone / layer[].send storage inside the
 * engine, so this file never has to know that slot 0 is special.
 *
 * The only atomics touched directly are the meter sources (bb.ret[r].peak,
 * bb.layer[L].peak, the sink ring) which are read-and-clear max-holds with
 * no accessor, and bb.gain / bb.mute / bb.panic / bb.layer[L].* which the
 * console has always owned.
 */

#include "MixerPanel.h"
#include "bytebeat.h"
#include "engine.h"
#include "ret.h"
#include "rack.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    /* HTML frame 06 literals with no Theme token */
    const juce::Colour STRIP_BG  { 0xff0a0a09 };   // voice / LICKS strip body
    const juce::Colour MASTER_BG { 0xff100f0e };   // master strip body
    const juce::Colour INK_NAME  { 0xffc9c4b8 };   // un-muted strip name/value

    /* ---- channel strip stack (every rule 1px, every block integer) ------ */
    constexpr int kHeaderH   = 22;
    constexpr int kSendLblY  = 26;    // 9px "SEND" / "> A" row
    constexpr int kSendKnobY = 38;    // 26px knob + value + focus name
    constexpr int kFpY       = 68;    // 10px 8-cell send fingerprint
    constexpr int kFpH       = 10;
    constexpr int kSendRuleY = 82;
    constexpr int kFaderTop  = 84;
    constexpr int kBottomH   = 62;    // value 12 + dB 9 + M 16 + route 14

    /* ---- drag law, shared by every grid cell and the rack level bar -----
     * Vertical drag, 2 units per pixel, cmd/ctrl for a quarter of that.
     * Matches TroughFader's direction and EngravedKnob's fine modifier. */
    int dragged (int startVal, int startY, const juce::MouseEvent& e)
    {
        const int dy = startY - e.getPosition().y;
        return startVal + (e.mods.isCommandDown() ? dy / 4 : dy * 2);
    }

    /* Post-gain master program peak, straight off the engine's sink ring
     * (what REC writes and the device plays). Read-only peek behind the
     * write cursor; the file/net read cursors are untouched. */
    float masterPeak()
    {
        const unsigned w = atomic_load_explicit (&bb.sink_w, memory_order_acquire);
        int peak = 0;
        for (unsigned i = 1; i <= 2048; ++i)         // ~46 ms at 44.1 kHz
        {
            int v = bb.sink[(w - i) & BB_SINK_MASK];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        return (float) peak / 32768.0f;
    }

    /* read-and-clear max-hold with a UI-side fall, the console's meter idiom */
    std::function<float()> peakSource (std::function<int()> take)
    {
        return [take, held = std::make_shared<float> (0.0f)]() -> float
        {
            const float v = (float) take() / 32768.0f;
            *held = juce::jmax (v, *held * 0.82f);
            return *held < 0.004f ? 0.0f : *held;
        };
    }

    juce::String levelDb (int v, int full)
    {
        if (v <= 0) return U8 ("\xe2\x88\x92\xe2\x88\x9e");
        const float dB = 20.0f * std::log10 ((float) v / (float) full);
        const float r  = std::round (dB * 10.0f) / 10.0f;
        if (r >= 0.0f) return "+" + juce::String (r, 1);
        return U8 ("\xe2\x88\x92") + juce::String (-r, 1);
    }

    /* gain reduction, Q8 (256 = none), as a signed dB string */
    juce::String grText (int gr)
    {
        if (gr >= 256) return "0.0";
        if (gr <= 0)   return U8 ("\xe2\x88\x92\xe2\x88\x9e");
        const float dB = 20.0f * std::log10 ((float) gr / 256.0f);
        return U8 ("\xe2\x88\x92") + juce::String (-dB, 1);
    }

    bool typeIsKnown (int t) { return t > RET_NONE && t < RET_NTYPE; }

    /* bb_ret_name is UI-owned and engine-persisted (precedent: bb_expr[]).
     * Written only from the message thread, read by the session writer on the
     * same thread, so a plain memset+copy is the whole story. */
    void writeRetName (int slot, const juce::String& s)
    {
        const juce::String t = s.trim();
        std::memset (bb_ret_name[slot], 0, (size_t) BB_RET_NAME);
        if (t.isNotEmpty()) t.copyToUTF8 (bb_ret_name[slot], BB_RET_NAME);
    }
} // namespace

/* ======================================================================== */
/*  small shared vocabulary                                                  */
/* ======================================================================== */

juce::String MixerPanel::slotTag (int slot)
{
    return juce::String::charToString ((juce::juce_wchar) ('A' + juce::jlimit (0, BB_NRET - 1, slot)));
}

juce::String MixerPanel::srcName (int src)
{
    if (src >= 0 && src < BB_NLAYER)
        return "V" + juce::String (src + 1).paddedLeft ('0', 2);
    if (src == BB_RET_SRC_LICKS) return "LICKS";
    if (src == BB_RET_SRC_DRY)   return "DRY";
    if (src == BB_RET_SRC_WET)   return "WET";
    if (src == BB_RET_SRC_MASS)  return "MASS";
    return "?";
}

juce::String MixerPanel::divName (int division)
{
    static const char* const d[11] = { "FREE", "1/32", "1/16T", "1/16", "1/8T",
                                       "1/8", "1/4T", "1/4", "1/2", "1/1", "2/1" };
    return d[juce::jlimit (0, 10, division)];
}

juce::String MixerPanel::typeName (int slot) const
{
    const int t = snap.pod.type[slot];
    if (t == RET_NONE) return "EMPTY";
    if (! typeIsKnown (t))
        return "TYPE " + juce::String (t).paddedLeft ('0', 2);
    return juce::String (ret_type_name[t]).toUpperCase();
}

juce::String MixerPanel::slotTitle (int slot) const
{
    juce::String n = snap.name[slot];
    return slotTag (slot) + " " + (n.isNotEmpty() ? n : typeName (slot));
}

juce::Colour MixerPanel::slotInk (int slot) const
{
    if (snap.pod.nodeLoop[slot]) return C::BLOOD_HOT;
    if (! snap.pod.live[slot])   return C::INK_GHOST;
    return INK_NAME;
}

/* ======================================================================== */
/*  Strip -- 8 voices + LICKS + MASTER                                       */
/* ======================================================================== */

MixerPanel::Strip::Strip (MixerPanel& owner, Kind k, int layerIndex, int sendSource)
    : kind (k), layer (layerIndex), src (sendSource), panel (owner)
{
    route = kind == Kind::Master ? "OUT 1-2" : "MASTER";

    meter = std::make_unique<MeterComponent>();
    addAndMakeVisible (*meter);

    if (kind == Kind::Voice)
    {
        name = "V" + juce::String (layer + 1).paddedLeft ('0', 2);

        fader = std::make_unique<TroughFader> (name);
        fader->setTooltip (U8 ("LEVEL \xe2\x80\x94 voice ") + juce::String (layer + 1)
                           + U8 (" into the master bus. 0\xe2\x80\x93" "256."));
        fader->onChange = [this] (int v)
        {
            atomic_store (&bb.layer[layer].ctl[LCTL_LEVEL], v);
            repaint();
        };
        addAndMakeVisible (*fader);

        muteBtn = std::make_unique<PlateButton> ("M", false);
        muteBtn->setTooltip (U8 ("MUTE \xe2\x80\x94 silence voice ")
                             + juce::String (layer + 1) + ".");
        muteBtn->onToggle = [this] (bool on)
        {
            atomic_store (&bb.layer[layer].on, on ? 0 : 1);   // armed = muted
            repaint();
        };
        addAndMakeVisible (*muteBtn);

        meter->source = peakSource ([L = layer] { return atomic_exchange (&bb.layer[L].peak, 0); });
    }
    else if (kind == Kind::Master)
    {
        name = "MASTER";
        hot  = true;

        fader = std::make_unique<TroughFader> ("MASTER");
        fader->setTooltip (U8 ("MASTER \xe2\x80\x94 master gain. 0\xe2\x80\x93" "256."));
        fader->onChange = [this] (int v) { atomic_store (&bb.gain, v); repaint(); };
        addAndMakeVisible (*fader);

        muteBtn = std::make_unique<PlateButton> ("M", false);
        muteBtn->setTooltip (U8 ("MUTE \xe2\x80\x94 silence the master output. Mirrors RUN."));
        muteBtn->onToggle = [this] (bool on)
        {
            if (on) atomic_store (&bb.mute, 1);
            else  { atomic_store (&bb.mute, 0); atomic_store (&bb.panic, 0); }
            repaint();
        };
        addAndMakeVisible (*muteBtn);

        meter->source = masterPeak;
    }
    else
    {
        /* engine gap: the sampler bus has no level, mute or peak. The strip
         * is drawn at its documented value and its trough stays empty --
         * only the SEND is live. */
        name = "LICKS"; value = 200; drawnDb = "\xe2\x88\x92" "4.7";
    }

    /* the one live send knob: this source into the FOCUSED return */
    sendKnob = std::make_unique<EngravedKnob> (juce::String(), 26, 0, 255, 0);
    sendKnob->setShowText (false);
    sendKnob->onChange = [this] (int v)
    {
        bb_engine_ret_send (src, panel.focus, v);
        repaint();
    };
    addAndMakeVisible (*sendKnob);
}

void MixerPanel::Strip::update (const juce::String& newName, bool newMuted,
                                bool newHot, int newValue)
{
    if (name == newName && muted == newMuted && hot == newHot && value == newValue)
        return;
    name = newName; muted = newMuted; hot = newHot; value = newValue;
    repaint();
}

Rectangle<int> MixerPanel::Strip::fingerprintCell (int slot) const
{
    const int inner = getWidth() - 10;
    const int cw    = juce::jmax (5, (inner - (BB_NRET - 1)) / BB_NRET);
    const int total = cw * BB_NRET + (BB_NRET - 1);
    const int x0    = (getWidth() - total) / 2;
    return { x0 + slot * (cw + 1), kFpY, cw, kFpH };
}

void MixerPanel::Strip::mouseDown (const juce::MouseEvent& e)
{
    for (int r = 0; r < BB_NRET; ++r)
        if (fingerprintCell (r).expanded (0, 2).contains (e.getPosition()))
        {
            if (! panel.snap.pod.live[r] && panel.snap.pod.type[r] == RET_NONE)
                panel.createMenu (r, this);
            else
                panel.setFocus (r);
            return;
        }
}

void MixerPanel::Strip::resized()
{
    const int w = getWidth(), h = getHeight();
    const int cx = w / 2;
    const int fy = kFaderTop + 8;
    const int fh = juce::jmax (0, h - kBottomH - 8 - fy);

    /* fader trough 16 wide + 3px cap overhang each side; 6px gap; 8px meter */
    if (fader) fader->setBounds (cx - 18, fy, 22, fh);
    meter->setBounds (cx + 7, fy, 8, fh);

    const int inner = w - 10;
    if (muteBtn) muteBtn->setBounds (5, h - 38, inner, 16);
    if (sendKnob) sendKnob->setBounds (5, kSendKnobY, 26, 26);
}

juce::String MixerPanel::Strip::dbText() const
{
    if (drawnDb) return U8 (drawnDb);
    if (muted) return U8 ("\xe2\x88\x92\xe2\x88\x9e");
    return levelDb (value, 256);
}

void MixerPanel::Strip::paint (juce::Graphics& g)
{
    const int w = getWidth(), h = getHeight();
    const int inner = w - 10;
    const MixerPanel::Snap& s = panel.snap;
    const int f = panel.focus;

    g.setColour (kind == Kind::Master ? MASTER_BG : STRIP_BG);
    g.fillRect (0, 0, w, h);

    const juce::Colour nameFg = muted ? C::INK_FAINT
                              : kind == Kind::Master ? C::INK_BRIGHT : INK_NAME;

    /* -- header 22: 5px lamp + name ------------------------------------- */
    g.setColour (C::RAISED);
    g.fillRect (0, 0, w, kHeaderH);
    g.setColour (C::HAIRLINE);
    g.fillRect (0, kHeaderH - 1, w, 1);
    g.setColour (muted ? C::HAIRLINE : hot ? C::BLOOD_HOT : C::LAMP_SOUNDING);
    g.fillRect (6, 8, 5, 5);
    g.setColour (nameFg);
    g.setFont (Type::mono (9.0f, 0.10f));
    g.drawText (name, 16, 0, w - 20, kHeaderH - 1, Justification::centredLeft, true);

    /* -- SEND: one knob, into the focused return ------------------------- */
    const bool liveTarget = s.pod.live[f] != 0;
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (7.0f, 0.10f));
    g.drawText ("SEND", 5, kSendLblY, inner, 9, Justification::centredLeft);
    g.setColour (liveTarget ? (s.pod.nodeLoop[f] ? C::BLOOD_HOT : C::OXIDE) : C::INK_GHOST);
    g.drawText (U8 ("\xe2\x86\x92") + slotTag (f), 5, kSendLblY, inner, 9,
                Justification::centredRight);

    const int amt = s.pod.send[src][f];
    g.setColour (amt > 0 && liveTarget ? INK_NAME : C::INK_FAINT);
    g.setFont (Type::mono (10.0f, 0.04f));
    g.drawText (juce::String (amt).paddedLeft ('0', 3), 36, kSendKnobY + 1,
                inner - 31, 12, Justification::centredLeft);
    g.setColour (liveTarget ? C::OXIDE : C::INK_GHOST);
    g.setFont (Type::nano (7.0f));
    g.drawText (panel.typeName (f), 36, kSendKnobY + 14, inner - 31, 9,
                Justification::centredLeft, true);

    /* -- FINGERPRINT: this source into all eight slots ------------------- */
    for (int r = 0; r < BB_NRET; ++r)
    {
        const Rectangle<int> c = fingerprintCell (r);
        const int a = s.pod.send[src][r];
        const bool liveR = s.pod.live[r] != 0;

        g.setColour (C::SOCKET);
        g.fillRect (c);
        if (a > 0)
        {
            const int fillH = juce::jmax (1, juce::roundToInt ((c.getHeight() - 2) * a / 255.0f));
            g.setColour (! liveR ? C::LAMP_DEAD
                       : s.pod.nodeLoop[r] ? C::BLOOD : C::OXIDE);
            g.fillRect (c.getX() + 1, c.getBottom() - 1 - fillH, c.getWidth() - 2, fillH);
        }
        g.setColour (r == f ? C::INK_DIM : C::HAIRLINE_DIM);
        g.drawRect (c, 1);
        if (r == f)                                   // focus mark under the cell
        {
            g.setColour (C::BLOOD);
            g.fillRect (c.getX(), c.getBottom() + 1, c.getWidth(), 1);
        }
    }
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kSendRuleY, w, 1);

    /* -- fader area: LICKS has no engine level, so its trough stays empty - */
    if (fader == nullptr)
    {
        const int fy = kFaderTop + 8;
        const int fh = juce::jmax (0, h - kBottomH - 8 - fy);
        if (fh > 4)
        {
            const Rectangle<int> trough (w / 2 - 15, fy, 16, fh);
            g.setColour (C::TROUGH);
            g.fillRect (trough);
            g.setColour (C::HAIRLINE);
            g.drawRect (trough, 1);
            const int fillH = juce::roundToInt ((fh - 2) * value / 256.0f);
            g.setColour (C::LAMP_DEAD);
            g.fillRect (trough.getX() + 1, trough.getBottom() - 1 - fillH, 14, fillH);
        }
    }

    /* -- value 10px, dB 7px --------------------------------------------- */
    const int liveVal = fader ? fader->value() : value;
    g.setColour (nameFg);
    g.setFont (Type::mono (10.0f, 0.04f));
    g.drawText (juce::String (liveVal).paddedLeft ('0', 3),
                5, h - 62, inner, 12, Justification::centred);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (7.0f));
    g.drawText (dbText(), 5, h - 49, inner, 9, Justification::centred);

    /* -- M plate: painted dead where the engine has no control ----------- */
    if (muteBtn == nullptr)
    {
        const Rectangle<int> r (5, h - 38, inner, 16);
        g.setColour (C::PLATE_LOW);
        g.fillRect (r);
        g.setColour (C::LAMP_DEAD);
        g.drawRect (r, 1);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText ("M", r, Justification::centred);
    }

    /* -- route tag 14 tall ---------------------------------------------- */
    const Rectangle<int> rt (5, h - 19, inner, 14);
    g.setColour (C::PANEL_ALT);
    g.fillRect (rt);
    g.setColour (C::HAIRLINE);
    g.drawRect (rt, 1);
    g.setColour (C::TAB_INACTIVE_FG);
    g.setFont (Type::mono (7.0f, 0.08f));
    g.drawText (route, rt, Justification::centred);

    /* strip divider */
    g.setColour (C::HAIRLINE);
    g.fillRect (w - 1, 0, 1, h);
}

/* ======================================================================== */
/*  RetRack -- eight slot rows                                               */
/* ======================================================================== */

namespace { constexpr int kRackCapH = 14; }

MixerPanel::RetRack::RetRack (MixerPanel& owner) : panel (owner)
{
    for (int r = 0; r < BB_NRET; ++r)
    {
        auto* m = new MeterComponent();
        m->setHorizontal (true);
        m->source = peakSource ([r] { return atomic_exchange (&bb.ret[r].peak, 0); });
        meters.add (m);
        addAndMakeVisible (m);
    }
}

void MixerPanel::RetRack::refresh()
{
    /* an empty slot draws no meter trough: a control that cannot move should
     * not be painted as though it could */
    for (int r = 0; r < BB_NRET; ++r)
        meters[r]->setVisible (panel.snap.pod.type[r] != RET_NONE);
    repaint();
}

int MixerPanel::RetRack::rowH() const
{
    return juce::jmax (18, (getHeight() - kRackCapH) / BB_NRET);
}

Rectangle<int> MixerPanel::RetRack::rowArea (int slot) const
{
    return { 0, kRackCapH + slot * rowH(), getWidth(), rowH() };
}

Rectangle<int> MixerPanel::RetRack::levelArea (int slot) const
{
    const Rectangle<int> r = rowArea (slot);
    const int xGr = r.getRight() - 4 - 34;
    const int xMute = xGr - 4 - 18;
    const int xMeter = xMute - 4 - 46;
    const int xLevel = juce::jmax (26, xMeter - 6 - 78);
    return { xLevel, r.getCentreY() - 6, juce::jmax (10, xMeter - 6 - xLevel), 12 };
}

Rectangle<int> MixerPanel::RetRack::muteArea (int slot) const
{
    const Rectangle<int> r = rowArea (slot);
    const int xGr = r.getRight() - 4 - 34;
    return { xGr - 4 - 18, r.getCentreY() - 7, 18, 14 };
}

int MixerPanel::RetRack::rowAt (juce::Point<int> p) const
{
    if (p.y < kRackCapH) return -1;
    const int r = (p.y - kRackCapH) / rowH();
    return r >= 0 && r < BB_NRET ? r : -1;
}

void MixerPanel::RetRack::resized()
{
    for (int r = 0; r < BB_NRET; ++r)
    {
        const Rectangle<int> row = rowArea (r);
        const int xGr = row.getRight() - 4 - 34;
        const int xMute = xGr - 4 - 18;
        meters[r]->setBounds (xMute - 4 - 46, row.getCentreY() - 3, 46, 6);
    }
}

void MixerPanel::RetRack::mouseDown (const juce::MouseEvent& e)
{
    const int slot = rowAt (e.getPosition());
    if (slot < 0) return;

    const MixerPanel::Snap& s = panel.snap;

    if (e.mods.isPopupMenu())
    {
        if (s.pod.type[slot] == RET_NONE) panel.createMenu (slot, this);
        else                              panel.rowMenu (slot, this);
        return;
    }

    if (s.pod.type[slot] == RET_NONE)            // empty row: create here
    {
        panel.setFocus (slot);
        panel.createMenu (slot, this);
        return;
    }

    panel.setFocus (slot);

    if (muteArea (slot).contains (e.getPosition()))
    {
        bb_engine_ret_mute (slot, s.pod.mute[slot] ? 0 : 1);
        return;
    }
    if (levelArea (slot).contains (e.getPosition()))
    {
        dragSlot = slot;
        dragStartY = e.getPosition().y;
        dragStartVal = s.pod.level[slot];
        return;
    }
}

void MixerPanel::RetRack::mouseDrag (const juce::MouseEvent& e)
{
    if (dragSlot < 0) return;
    bb_engine_ret_level (dragSlot, juce::jlimit (0, 256, dragged (dragStartVal, dragStartY, e)));
}

void MixerPanel::RetRack::mouseUp (const juce::MouseEvent&) { dragSlot = -1; }

void MixerPanel::RetRack::mouseMove (const juce::MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    if (r != hoverSlot) { hoverSlot = r; repaint(); }
}

void MixerPanel::RetRack::mouseExit (const juce::MouseEvent&)
{
    if (hoverSlot >= 0) { hoverSlot = -1; repaint(); }
}

juce::String MixerPanel::RetRack::getTooltip()
{
    if (hoverSlot < 0) return {};
    const MixerPanel::Snap& s = panel.snap;
    if (s.pod.type[hoverSlot] == RET_NONE)
        return U8 ("SLOT ") + slotTag (hoverSlot)
             + U8 (" \xe2\x80\x94 empty. Click to create a return here. An empty "
                   "slot costs the engine nothing: it is not rendered at all.");

    juce::String t = panel.slotTitle (hoverSlot)
        + U8 (" \xe2\x80\x94 drag the bar for level 0\xe2\x80\x93" "256 (cmd-drag fine), "
              "M to mute. Muting FREEZES the tail: a muted return is skipped "
              "entirely, exactly like level 0. Right-click to rename or destroy.");
    if (s.pod.gr[hoverSlot] < 256)
        t += U8 ("  LIMITING ") + grText (s.pod.gr[hoverSlot]) + " dB.";
    if (s.pod.nodeLoop[hoverSlot])
        t += U8 ("  THIS RETURN IS INSIDE A FEEDBACK LOOP (") + s.loopPath + ").";
    return t;
}

void MixerPanel::RetRack::paint (juce::Graphics& g)
{
    const MixerPanel::Snap& s = panel.snap;
    const int w = getWidth();

    g.setColour (C::PANEL);
    g.fillRect (getLocalBounds());

    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (7.0f, 0.16f));
    g.drawText ("RETURN RACK", 4, 0, w - 8, kRackCapH, Justification::centredLeft);
    g.setColour (C::INK_FAINT);
    g.drawText (juce::String (s.pod.active) + "/" + juce::String (BB_NRET) + " LIVE",
                4, 0, w - 8, kRackCapH, Justification::centredRight);
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kRackCapH - 1, w, 1);

    for (int r = 0; r < BB_NRET; ++r)
    {
        const Rectangle<int> row = rowArea (r);
        const bool focused = r == panel.focus;
        const bool empty   = s.pod.type[r] == RET_NONE;

        g.setColour (focused ? C::TAB_ACTIVE_BG : (r & 1) ? C::PANEL : STRIP_BG);
        g.fillRect (row);
        if (focused)
        {
            g.setColour (C::BLOOD);
            g.fillRect (row.getX(), row.getY(), 2, row.getHeight());
        }
        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

        /* slot letter tag */
        const Rectangle<int> tag (5, row.getCentreY() - 8, 17, 16);
        g.setColour (empty ? C::DISABLED_BG
                   : s.pod.nodeLoop[r] ? C::BLOOD_DEEP : C::PLATE_LOW);
        g.fillRect (tag);
        g.setColour (empty ? C::HAIRLINE_DIM
                   : s.pod.nodeLoop[r] ? C::BLOOD : C::HAIRLINE);
        g.drawRect (tag, 1);
        g.setColour (empty ? C::INK_GHOST
                   : s.pod.nodeLoop[r] ? C::ARMED_TEXT : C::INK_DIM);
        g.setFont (Type::mono (9.0f, 0.06f));
        g.drawText (slotTag (r), tag, Justification::centred);

        const int xGr    = row.getRight() - 4 - 34;
        const Rectangle<int> lvl = levelArea (r);
        const int nameW = juce::jmax (20, lvl.getX() - 26 - 6);

        if (empty)
        {
            g.setColour (C::INK_GHOST);
            g.setFont (Type::nano (7.0f));
            g.drawText (s.pod.pending[r] ? U8 ("WORKING\xe2\x80\xa6")
                                         : U8 ("EMPTY \xc2\xb7 CLICK TO CREATE"),
                        26, row.getY(), row.getWidth() - 30, row.getHeight(),
                        Justification::centredLeft, true);
            continue;
        }

        /* name + type */
        const juce::String nm = s.name[r].isNotEmpty() ? s.name[r] : panel.typeName (r);
        g.setColour (s.pod.mute[r] ? C::INK_FAINT : panel.slotInk (r));
        g.setFont (Type::mono (9.0f, 0.06f));
        g.drawText (nm, 26, row.getCentreY() - 11, nameW, 11,
                    Justification::centredLeft, true);
        g.setColour (typeIsKnown (s.pod.type[r]) ? C::OXIDE : C::AMBER);
        g.setFont (Type::nano (7.0f));
        juce::String sub = panel.typeName (r);
        if (s.pod.division[r] > 0) sub += U8 (" \xc2\xb7 ") + divName (s.pod.division[r]);
        if (s.pod.pending[r])      sub = U8 ("RECONFIGURING\xe2\x80\xa6");
        g.drawText (sub, 26, row.getCentreY() + 1, nameW, 10,
                    Justification::centredLeft, true);

        /* level bar */
        g.setColour (C::TROUGH);
        g.fillRect (lvl);
        const int fillW = juce::roundToInt ((lvl.getWidth() - 2) * s.pod.level[r] / 256.0f);
        g.setColour (s.pod.mute[r] || s.pod.level[r] == 0 ? C::LAMP_DEAD : C::BLOOD);
        g.fillRect (lvl.getX() + 1, lvl.getY() + 1, fillW, lvl.getHeight() - 2);
        g.setColour (C::HAIRLINE);
        g.drawRect (lvl, 1);
        g.setColour (s.pod.mute[r] ? C::INK_FAINT : C::INK);
        g.setFont (Type::nano (7.0f));
        g.drawText (juce::String (s.pod.level[r]).paddedLeft ('0', 3),
                    lvl.reduced (3, 0), Justification::centredRight);

        /* M plate */
        const Rectangle<int> m = muteArea (r);
        g.setColour (s.pod.mute[r] ? C::BLOOD_DEEP : C::PLATE_LOW);
        g.fillRect (m);
        g.setColour (s.pod.mute[r] ? C::BLOOD : C::EDGE);
        g.drawRect (m, 1);
        g.setColour (s.pod.mute[r] ? C::ARMED_TEXT : C::INK_DIM);
        g.setFont (Type::mono (8.0f));
        g.drawText ("M", m, Justification::centred);

        /* GR / FB column: the safety readout, never hidden */
        const int gr = s.pod.gr[r];
        const Rectangle<int> grBar (xGr, row.getCentreY() + 1, 34, 5);
        if (s.pod.nodeLoop[r])
        {
            g.setColour (C::BLOOD_HOT);
            g.setFont (Type::nano (7.0f));
            g.drawText ("FB", xGr, row.getCentreY() - 10, 34, 9, Justification::centredLeft);
        }
        if (gr < 256)
        {
            const bool hard = gr < 64;                      // below about -12 dB
            g.setColour (hard ? C::BLOOD_HOT : C::AMBER);
            g.setFont (Type::nano (7.0f));
            g.drawText ("LIM", xGr, row.getCentreY() - 10, 34, 9, Justification::centredRight);
            g.setColour (C::TROUGH);
            g.fillRect (grBar);
            const int rw = juce::roundToInt ((grBar.getWidth() - 2) * (256 - gr) / 256.0f);
            g.setColour (hard ? C::BLOOD_HOT : C::AMBER);
            g.fillRect (grBar.getRight() - 1 - rw, grBar.getY() + 1, rw, grBar.getHeight() - 2);
            g.setColour (C::HAIRLINE_DIM);
            g.drawRect (grBar, 1);
        }
    }

    g.setColour (C::HAIRLINE);
    g.fillRect (w - 1, 0, 1, getHeight());
}

/* ======================================================================== */
/*  SendMatrix -- 11 sources x 8 slots                                       */
/* ======================================================================== */

namespace
{
    constexpr int kMatCapH = 14;      // "SEND MATRIX" caption
    constexpr int kMatHdrH = 14;      // column letters
    constexpr int kMatLblW = 36;      // row labels
}

MixerPanel::SendMatrix::SendMatrix (MixerPanel& owner) : panel (owner) {}

Rectangle<int> MixerPanel::SendMatrix::cell (int src, int slot) const
{
    const int cw = juce::jmax (8, (getWidth() - kMatLblW - 4) / BB_NRET);
    const int rh = juce::jmax (8, (getHeight() - kMatCapH - kMatHdrH) / BB_RET_NSRC);
    return { kMatLblW + slot * cw, kMatCapH + kMatHdrH + src * rh, cw - 1, rh - 1 };
}

Rectangle<int> MixerPanel::SendMatrix::header (int slot) const
{
    const int cw = juce::jmax (8, (getWidth() - kMatLblW - 4) / BB_NRET);
    return { kMatLblW + slot * cw, kMatCapH, cw - 1, kMatHdrH };
}

bool MixerPanel::SendMatrix::hit (juce::Point<int> p, int& src, int& slot) const
{
    for (int s = 0; s < BB_RET_NSRC; ++s)
        for (int r = 0; r < BB_NRET; ++r)
            if (cell (s, r).expanded (0, 1).contains (p)) { src = s; slot = r; return true; }
    return false;
}

void MixerPanel::SendMatrix::mouseDown (const juce::MouseEvent& e)
{
    for (int r = 0; r < BB_NRET; ++r)
        if (header (r).contains (e.getPosition()))
        {
            if (panel.snap.pod.type[r] == RET_NONE) panel.createMenu (r, this);
            else                                    panel.setFocus (r);
            return;
        }

    int s = -1, r = -1;
    if (! hit (e.getPosition(), s, r)) return;
    panel.setFocus (r);
    if (e.mods.isPopupMenu()) { bb_engine_ret_send (s, r, 0); return; }
    dragging = true; dragSrc = s; dragSlot = r;
    dragStartY = e.getPosition().y;
    dragStartVal = panel.snap.pod.send[s][r];
}

void MixerPanel::SendMatrix::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    bb_engine_ret_send (dragSrc, dragSlot,
                        juce::jlimit (0, 255, dragged (dragStartVal, dragStartY, e)));
}

void MixerPanel::SendMatrix::mouseUp (const juce::MouseEvent&) { dragging = false; }

void MixerPanel::SendMatrix::mouseDoubleClick (const juce::MouseEvent& e)
{
    int s = -1, r = -1;
    if (hit (e.getPosition(), s, r)) bb_engine_ret_send (s, r, 0);
}

void MixerPanel::SendMatrix::mouseMove (const juce::MouseEvent& e)
{
    int s = -1, r = -1;
    hit (e.getPosition(), s, r);
    if (s != hoverSrc || r != hoverSlot) { hoverSrc = s; hoverSlot = r; repaint(); }
}

void MixerPanel::SendMatrix::mouseExit (const juce::MouseEvent&)
{
    if (hoverSrc >= 0) { hoverSrc = hoverSlot = -1; repaint(); }
}

juce::String MixerPanel::SendMatrix::getTooltip()
{
    if (hoverSrc < 0 || hoverSlot < 0) return {};
    juce::String t = srcName (hoverSrc) + U8 (" \xe2\x86\x92 ") + panel.slotTitle (hoverSlot)
        + U8 ("  ") + juce::String (panel.snap.pod.send[hoverSrc][hoverSlot])
        + U8 ("/255 \xe2\x80\x94 drag up/down (cmd fine), double-click or right-click "
              "to zero.");
    if (hoverSrc == BB_RET_SRC_WET)
        t += U8 ("  WET is the whole return bus one frame ago: this row IS master "
                 "feedback, and any value above zero puts every live return inside "
                 "a loop.");
    else if (hoverSrc == BB_RET_SRC_DRY)
        t += U8 ("  DRY is the master tap taken after the voices, the sampler and the "
                 "wells and before any return output, so it cannot make a loop by "
                 "itself.");
    else if (hoverSrc == BB_RET_SRC_MASS)
        t += U8 ("  MASS is the GRAIN MASS well bus, tapped exactly where LICKS is: "
                 "this is how a specimen reaches the chamber.");
    return t;
}

void MixerPanel::SendMatrix::paint (juce::Graphics& g)
{
    const MixerPanel::Snap& s = panel.snap;
    const int w = getWidth();

    g.setColour (C::PANEL);
    g.fillRect (getLocalBounds());
    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (7.0f, 0.16f));
    g.drawText ("SEND MATRIX", 4, 0, w - 8, kMatCapH, Justification::centredLeft);
    g.setColour (C::INK_FAINT);
    g.drawText (juce::String (BB_RET_NSRC) + U8 ("\xc3\x97") + juce::String (BB_NRET),
                4, 0, w - 8, kMatCapH, Justification::centredRight);
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kMatCapH - 1, w, 1);

    /* column headers: slot letters, click to focus */
    for (int r = 0; r < BB_NRET; ++r)
    {
        const Rectangle<int> hd = header (r);
        const bool empty = s.pod.type[r] == RET_NONE;
        g.setColour (r == panel.focus ? C::TAB_ACTIVE_BG : C::PANEL_ALT);
        g.fillRect (hd);
        g.setColour (empty ? C::INK_GHOST
                   : s.pod.nodeLoop[r] ? C::BLOOD_HOT
                   : r == panel.focus ? C::INK : C::INK_DIM);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (slotTag (r), hd, Justification::centred);
        if (r == panel.focus)
        {
            g.setColour (C::BLOOD);
            g.fillRect (hd.getX(), hd.getBottom() - 1, hd.getWidth(), 1);
        }
    }

    for (int src = 0; src < BB_RET_NSRC; ++src)
    {
        const Rectangle<int> c0 = cell (src, 0);
        const bool wetRow = src == BB_RET_SRC_WET;

        g.setColour (wetRow ? C::BLOOD_HOT : src >= BB_NLAYER ? C::OXIDE : C::INK_FAINT);
        g.setFont (Type::nano (7.0f));
        g.drawText (srcName (src), 3, c0.getY(), kMatLblW - 6, c0.getHeight(),
                    Justification::centredRight);

        for (int r = 0; r < BB_NRET; ++r)
        {
            const Rectangle<int> c = cell (src, r);
            const int amt = s.pod.send[src][r];
            const bool empty = s.pod.type[r] == RET_NONE;
            const bool hover = src == hoverSrc && r == hoverSlot;

            g.setColour (r == panel.focus ? C::PLATE_LOW : C::SOCKET);
            g.fillRect (c);
            if (amt > 0)
            {
                const int fw = juce::jmax (1, juce::roundToInt ((c.getWidth() - 2) * amt / 255.0f));
                g.setColour (empty ? C::LAMP_DEAD : wetRow ? C::BLOOD : C::OXIDE);
                g.fillRect (c.getX() + 1, c.getY() + 1, fw, c.getHeight() - 2);
            }
            g.setColour (hover ? C::EDGE : C::HAIRLINE_DIM);
            g.drawRect (c, 1);
        }
    }

    g.setColour (C::HAIRLINE);
    g.fillRect (w - 1, 0, 1, getHeight());
}

/* ======================================================================== */
/*  LinkGrid -- 8 x 8, rows FROM, columns TO                                 */
/* ======================================================================== */

namespace
{
    constexpr int kLnkCapH = 14;
    constexpr int kLnkHdrH = 14;
    constexpr int kLnkLblW = 22;
}

MixerPanel::LinkGrid::LinkGrid (MixerPanel& owner) : panel (owner) {}

Rectangle<int> MixerPanel::LinkGrid::cell (int from, int to) const
{
    const int cw = juce::jmax (8, (getWidth() - kLnkLblW - 4) / BB_NRET);
    const int rh = juce::jmax (8, (getHeight() - kLnkCapH - kLnkHdrH) / BB_NRET);
    return { kLnkLblW + to * cw, kLnkCapH + kLnkHdrH + from * rh, cw - 1, rh - 1 };
}

bool MixerPanel::LinkGrid::hit (juce::Point<int> p, int& from, int& to) const
{
    for (int f = 0; f < BB_NRET; ++f)
        for (int t = 0; t < BB_NRET; ++t)
            if (cell (f, t).expanded (0, 1).contains (p)) { from = f; to = t; return true; }
    return false;
}

void MixerPanel::LinkGrid::mouseDown (const juce::MouseEvent& e)
{
    int f = -1, t = -1;
    if (! hit (e.getPosition(), f, t)) return;
    panel.setFocus (t);
    if (e.mods.isPopupMenu()) { bb_engine_ret_link (f, t, 0); return; }
    dragging = true; dragFrom = f; dragTo = t;
    dragStartY = e.getPosition().y;
    dragStartVal = panel.snap.pod.link[f][t];
}

void MixerPanel::LinkGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    bb_engine_ret_link (dragFrom, dragTo,
                        juce::jlimit (0, 256, dragged (dragStartVal, dragStartY, e)));
}

void MixerPanel::LinkGrid::mouseUp (const juce::MouseEvent&) { dragging = false; }

void MixerPanel::LinkGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    int f = -1, t = -1;
    if (hit (e.getPosition(), f, t)) bb_engine_ret_link (f, t, 0);
}

void MixerPanel::LinkGrid::mouseMove (const juce::MouseEvent& e)
{
    int f = -1, t = -1;
    hit (e.getPosition(), f, t);
    if (f != hoverFrom || t != hoverTo) { hoverFrom = f; hoverTo = t; repaint(); }
}

void MixerPanel::LinkGrid::mouseExit (const juce::MouseEvent&)
{
    if (hoverFrom >= 0) { hoverFrom = hoverTo = -1; repaint(); }
}

juce::String MixerPanel::LinkGrid::getTooltip()
{
    if (hoverFrom < 0 || hoverTo < 0) return {};
    const MixerPanel::Snap& s = panel.snap;
    juce::String t = slotTag (hoverFrom) + U8 (" \xe2\x86\x92 ") + slotTag (hoverTo)
        + "  " + juce::String (s.pod.link[hoverFrom][hoverTo])
        + U8 ("/256 \xe2\x80\x94 drag up/down (cmd fine), double-click or right-click "
              "to cut. Every link is delayed by exactly one sample, so slot order "
              "does not change the sound.");
    if (hoverFrom == hoverTo)
        t += U8 ("  THE DIAGONAL is the freeze cell: a return feeding itself. At 256 "
                 "it never decays -- it is held at the limiter's ceiling, not the "
                 "master's clipper.");
    else if (s.pod.edgeLoop[hoverFrom][hoverTo])
        t += U8 ("  THIS LINK CLOSES A LOOP (") + s.loopPath + ").";
    return t;
}

void MixerPanel::LinkGrid::paint (juce::Graphics& g)
{
    const MixerPanel::Snap& s = panel.snap;
    const int w = getWidth();

    g.setColour (C::PANEL);
    g.fillRect (getLocalBounds());
    g.setColour (s.pod.anyLoop ? C::BLOOD_HOT : C::INK_DIM);
    g.setFont (Type::mono (7.0f, 0.16f));
    g.drawText ("LINK GRID", 4, 0, w - 8, kLnkCapH, Justification::centredLeft);
    g.setColour (C::INK_FAINT);
    g.drawText (U8 ("ROWS FROM \xc2\xb7 COLS TO"), 4, 0, w - 8, kLnkCapH,
                Justification::centredRight);
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kLnkCapH - 1, w, 1);

    /* column letters */
    for (int t = 0; t < BB_NRET; ++t)
    {
        const Rectangle<int> c = cell (0, t);
        const Rectangle<int> hd (c.getX(), kLnkCapH, c.getWidth(), kLnkHdrH);
        g.setColour (t == panel.focus ? C::TAB_ACTIVE_BG : C::PANEL_ALT);
        g.fillRect (hd);
        g.setColour (s.pod.type[t] == RET_NONE ? C::INK_GHOST
                   : s.pod.nodeLoop[t] ? C::BLOOD_HOT : C::INK_DIM);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (slotTag (t), hd, Justification::centred);
    }

    for (int f = 0; f < BB_NRET; ++f)
    {
        const Rectangle<int> c0 = cell (f, 0);
        g.setColour (s.pod.type[f] == RET_NONE ? C::INK_GHOST
                   : s.pod.nodeLoop[f] ? C::BLOOD_HOT : C::INK_FAINT);
        g.setFont (Type::nano (7.0f));
        g.drawText (slotTag (f), 2, c0.getY(), kLnkLblW - 5, c0.getHeight(),
                    Justification::centredRight);

        for (int t = 0; t < BB_NRET; ++t)
        {
            const Rectangle<int> c = cell (f, t);
            const int amt = s.pod.link[f][t];
            const bool diag = f == t;
            const bool dead = s.pod.type[f] == RET_NONE || s.pod.type[t] == RET_NONE;
            const bool loop = s.pod.edgeLoop[f][t] != 0;
            const bool hover = f == hoverFrom && t == hoverTo;

            g.setColour (t == panel.focus ? C::PLATE_LOW : C::SOCKET);
            g.fillRect (c);

            /* the diagonal always carries its registration cross: it is the
             * one cell that is a feedback loop all by itself */
            if (diag && amt == 0)
            {
                g.setColour (C::INK_GHOST);
                g.fillRect (c.getCentreX(), c.getY() + 2, 1, c.getHeight() - 4);
                g.fillRect (c.getX() + 2, c.getCentreY(), c.getWidth() - 4, 1);
            }

            if (amt > 0)
            {
                const int fw = juce::jmax (1, juce::roundToInt ((c.getWidth() - 2) * amt / 256.0f));
                g.setColour (dead ? C::LAMP_DEAD : loop ? C::BLOOD : C::OXIDE);
                g.fillRect (c.getX() + 1, c.getY() + 1, fw, c.getHeight() - 2);
                if (amt >= 256 && ! dead)               // unity: the wall
                {
                    g.setColour (C::BLOOD_HOT);
                    g.fillRect (c.getRight() - 2, c.getY() + 1, 1, c.getHeight() - 2);
                }
            }

            g.setColour (hover ? C::EDGE
                       : loop && amt > 0 ? C::BLOOD
                       : diag ? C::HAIRLINE : C::HAIRLINE_DIM);
            g.drawRect (c, 1);
        }
    }

    g.setColour (C::HAIRLINE);
    g.fillRect (w - 1, 0, 1, getHeight());
}

/* ======================================================================== */
/*  Inspector -- the focused return                                          */
/* ======================================================================== */

MixerPanel::Inspector::Inspector (MixerPanel& owner) : panel (owner)
{
    nameEd.setMultiLine (false);
    nameEd.setReturnKeyStartsNewLine (false);
    nameEd.setFont (Type::mono (10.0f, 0.06f));
    nameEd.setInputRestrictions (BB_RET_NAME - 1);
    nameEd.setColour (juce::TextEditor::textColourId, C::INK);
    nameEd.setColour (juce::TextEditor::backgroundColourId, C::SOCKET);
    nameEd.setColour (juce::TextEditor::highlightColourId, C::BLOOD_DEEP);
    nameEd.setJustification (Justification::centredLeft);
    nameEd.setTooltip (U8 ("NAME \xe2\x80\x94 what this return is called on every strip "
                           "and in the session file. Up to 15 characters; empty falls "
                           "back to the effect type."));
    nameEd.onReturnKey = [this] { commitName(); juce::Component::unfocusAllComponents(); };
    nameEd.onFocusLost = [this] { commitName(); };
    addAndMakeVisible (nameEd);

    typeBtn = std::make_unique<PlateButton> ("EMPTY", false, false);
    typeBtn->setTooltip (U8 ("TYPE \xe2\x80\x94 what this slot is. Changing it fades the "
                             "slot out, waits two render epochs and clears its state: "
                             "about 100 ms, and the tail fades first rather than "
                             "clicking."));
    typeBtn->onToggle = [this] (bool) { panel.createMenu (panel.focus, typeBtn.get()); };
    addAndMakeVisible (*typeBtn);

    destroyBtn = std::make_unique<PlateButton> ("DESTROY", false, false);
    destroyBtn->setTooltip (U8 ("DESTROY \xe2\x80\x94 empty this slot. Its sends and links "
                                "are left alone, so re-creating a return here brings "
                                "the routing back."));
    destroyBtn->onToggle = [this] (bool) { bb_engine_ret_destroy (panel.focus); };
    addAndMakeVisible (*destroyBtn);

    for (int p = 0; p < BB_RET_NPARAM; ++p)
    {
        auto* k = new EngravedKnob (juce::String(), 32, 0, 255, 0);
        k->onChange = [this, p] (int v) { bb_engine_ret_param (panel.focus, p, v); };
        params.add (k);
        addAndMakeVisible (k);
    }

    syncKnob = std::make_unique<EngravedKnob> ("SYNC", 26, 0, 10, 0);
    syncKnob->setTooltip (U8 ("SYNC \xe2\x80\x94 clock division for the types that have a "
                              "time: FREE leaves TIME in milliseconds, anything else "
                              "locks it to the step clock."));
    syncKnob->onChange = [this] (int v) { bb_engine_ret_sync (panel.focus, v); };
    addAndMakeVisible (*syncKnob);
}

void MixerPanel::Inspector::commitName()
{
    writeRetName (panel.focus, nameEd.getText());
}

void MixerPanel::Inspector::beginRename()
{
    nameEd.grabKeyboardFocus();
    nameEd.selectAll();
}

void MixerPanel::Inspector::resized()
{
    const int w = getWidth();
    const int typeW = juce::jlimit (72, 110, w / 3);

    nameEd.setBounds (6, 20, juce::jmax (40, w - 12 - typeW - 6), 18);
    typeBtn->setBounds (w - 6 - typeW, 20, typeW, 18);

    /* eight knobs, four across, two down */
    const int colW = juce::jmax (44, (w - 12) / 4);
    const int rowH = params[0]->idealHeight() + 6;
    for (int p = 0; p < BB_RET_NPARAM; ++p)
    {
        const int cx = 6 + (p % 4) * colW;
        const int cy = 48 + (p / 4) * rowH;
        params[p]->setBounds (cx, cy, colW - 4, params[p]->idealHeight());
    }

    const int bottomY = 48 + 2 * rowH + 6;
    syncKnob->setBounds (6, bottomY, 60, syncKnob->idealHeight());
    destroyBtn->setBounds (w - 6 - 76, bottomY + 4, 76, 18);
    grArea   = { 74, bottomY + 6, juce::jmax (20, w - 74 - 90), 8 };
    statArea = { 74, bottomY + 18, juce::jmax (20, w - 74 - 90), 10 };
}

void MixerPanel::Inspector::refresh()
{
    const MixerPanel::Snap& s = panel.snap;
    const int f = panel.focus;
    const int t = s.pod.type[f];
    const bool known = typeIsKnown (t);

    if (! nameEd.hasKeyboardFocus (true))
    {
        const juce::String want = s.name[f];
        if (nameEd.getText() != want) nameEd.setText (want, juce::dontSendNotification);
    }
    nameEd.setEnabled (t != RET_NONE);

    /* The knob LABELS come from ret_param_name and only change when the
     * focused slot or its type changes; the VALUES change constantly. Split
     * the two, or the limiter riding its gain rebuilds nine labels and nine
     * tooltips thirty times a second for a picture that did not move. */
    if (f != lastFocus || t != lastType)
    {
        lastFocus = f; lastType = t;

        typeBtn->setButtonText (t == RET_NONE ? U8 ("CREATE\xe2\x80\xa6")
                                              : panel.typeName (f) + U8 ("\xe2\x80\xa6"));
        typeBtn->setOxideStyle (t != RET_NONE);
        destroyBtn->setEnabled (t != RET_NONE);

        for (int p = 0; p < BB_RET_NPARAM; ++p)
        {
            const char* lbl = known ? ret_param_name[t][p] : nullptr;
            params[p]->setLabelText (lbl != nullptr ? juce::String (lbl).toUpperCase()
                                                    : juce::String ("UNUSED"));
            params[p]->setSubLabel ("P" + juce::String (p));
            params[p]->setUnused (lbl == nullptr);
            params[p]->setEnabled (lbl != nullptr);
            params[p]->setDefaultValue (known ? (int) ret_param_def[t][p] : 0);
            params[p]->setTooltip (lbl != nullptr
                ? juce::String (lbl).toUpperCase() + U8 (" \xe2\x80\x94 ")
                    + panel.typeName (f) + U8 (" parameter ") + juce::String (p)
                    + U8 (". 0\xe2\x80\x93" "255. Double-click for the type's default.")
                : U8 ("UNUSED \xe2\x80\x94 this effect type does not use knob ")
                    + juce::String (p) + ".");
        }

        syncKnob->setEnabled (t != RET_NONE);
        syncKnob->setUnused (t == RET_NONE);
    }

    for (int p = 0; p < BB_RET_NPARAM; ++p)
        if (! params[p]->isUserDragging())
            params[p]->setValueQuiet (s.pod.param[f][p]);

    syncKnob->setValueText (divName (s.pod.division[f]));
    if (! syncKnob->isUserDragging())
        syncKnob->setValueQuiet (s.pod.division[f]);

    repaint();
}

void MixerPanel::Inspector::paint (juce::Graphics& g)
{
    const MixerPanel::Snap& s = panel.snap;
    const int f = panel.focus;
    const int w = getWidth();

    g.setColour (C::PANEL);
    g.fillRect (getLocalBounds());

    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (7.0f, 0.16f));
    g.drawText ("RETURN " + slotTag (f), 6, 0, w - 12, 14, Justification::centredLeft);
    g.setColour (s.pod.pending[f] ? C::AMBER : C::INK_FAINT);
    g.drawText (s.pod.pending[f] ? U8 ("RECONFIGURING\xe2\x80\xa6")
                                 : "SLOT " + juce::String (f + 1) + "/" + juce::String (BB_NRET),
                6, 0, w - 12, 14, Justification::centredRight);

    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (6, 43, w - 12, 1);

    /* GR meter: the limiter, drawn whether or not it is working */
    const int gr = s.pod.gr[f];
    g.setColour (C::INK_FAINT);
    g.setFont (Type::nano (7.0f));
    g.drawText ("GR", grArea.getX() - 22, grArea.getY() - 1, 20, 9,
                Justification::centredRight);
    g.setColour (C::TROUGH);
    g.fillRect (grArea);
    if (gr < 256)
    {
        const int rw = juce::roundToInt ((grArea.getWidth() - 2) * (256 - gr) / 256.0f);
        g.setColour (gr < 64 ? C::BLOOD_HOT : C::AMBER);
        g.fillRect (grArea.getRight() - 1 - rw, grArea.getY() + 1, rw, grArea.getHeight() - 2);
    }
    g.setColour (C::HAIRLINE);
    g.drawRect (grArea, 1);

    juce::String stat;
    if (s.pod.type[f] == RET_NONE)   stat = U8 ("EMPTY SLOT \xc2\xb7 COSTS NOTHING");
    else if (s.pod.nodeLoop[f])      stat = U8 ("IN LOOP ") + s.loopPath;
    else if (gr < 256)               stat = U8 ("LIMITING ") + grText (gr) + " dB";
    else                             stat = U8 ("LEVEL ") + juce::String (s.pod.level[f])
                                          + U8 (" \xc2\xb7 ") + levelDb (s.pod.level[f], 256) + " dB";
    g.setColour (s.pod.nodeLoop[f] ? C::BLOOD_HOT : gr < 256 ? C::AMBER : C::INK_FAINT);
    g.setFont (Type::nano (7.0f));
    g.drawText (stat, statArea, Justification::centredLeft, true);
}

/* ======================================================================== */
/*  MixerPanel                                                               */
/* ======================================================================== */

MixerPanel::MixerPanel()
{
    for (int L = 0; L < BB_NLAYER; ++L)
        strips.add (new Strip (*this, Strip::Kind::Voice, L, L));
    strips.add (new Strip (*this, Strip::Kind::Licks,  -1, BB_RET_SRC_LICKS));
    strips.add (new Strip (*this, Strip::Kind::Master, -1, BB_RET_SRC_DRY));
    for (auto* s : strips) addAndMakeVisible (s);

    rack      = std::make_unique<RetRack> (*this);
    matrix    = std::make_unique<SendMatrix> (*this);
    links     = std::make_unique<LinkGrid> (*this);
    inspector = std::make_unique<Inspector> (*this);
    addAndMakeVisible (*rack);
    addAndMakeVisible (*matrix);
    addAndMakeVisible (*links);
    addAndMakeVisible (*inspector);

    addBtn = std::make_unique<PlateButton> ("+ RET", false, false);
    addBtn->setTooltip (U8 ("NEW RETURN \xe2\x80\x94 create an effect in the lowest empty "
                            "slot. Eight slots exist from launch; creating one is a "
                            "type change, not an allocation, so it can be done while "
                            "the instrument plays."));
    addBtn->onToggle = [this] (bool)
    {
        for (int r = 0; r < BB_NRET; ++r)
            if (snap.pod.type[r] == RET_NONE) { setFocus (r); createMenu (r, addBtn.get()); return; }
    };
    addAndMakeVisible (*addBtn);

    panicBtn = std::make_unique<PlateButton> ("FB PANIC", true, false);
    panicBtn->setTooltip (U8 ("FB PANIC \xe2\x80\x94 zero every link and every return level "
                              "in one gesture. The voices keep playing; only the return "
                              "bus is killed. This is the escape from a runaway loop, "
                              "and it is deliberately not in a menu."));
    panicBtn->onToggle = [this] (bool) { bb_engine_ret_panic(); };
    addAndMakeVisible (*panicBtn);

    sync();
}

/* ---- focus ------------------------------------------------------------- */

void MixerPanel::setFocus (int slot)
{
    slot = juce::jlimit (0, BB_NRET - 1, slot);
    if (slot == focus) return;
    focus = slot;
    refreshChildren();
    repaint();
}

/* ---- menus ------------------------------------------------------------- */

void MixerPanel::createMenu (int slot, juce::Component* target)
{
    juce::PopupMenu m;
    m.addSectionHeader ("SLOT " + slotTag (slot));
    for (int t = RET_CHAMBER; t < RET_NTYPE; ++t)
        m.addItem (t, juce::String (ret_type_name[t]).toUpperCase(),
                   true, snap.pod.type[slot] == t);
    if (snap.pod.type[slot] != RET_NONE)
    {
        m.addSeparator();
        m.addItem (100, "DESTROY");
    }

    juce::PopupMenu::Options opts = juce::PopupMenu::Options().withMinimumWidth (150);
    opts = target != nullptr ? opts.withTargetComponent (target) : opts.withMousePosition();

    juce::Component::SafePointer<MixerPanel> safe (this);
    m.showMenuAsync (opts, [safe, slot] (int result)
    {
        if (safe == nullptr || result == 0) return;

        if (result == 100) { bb_engine_ret_destroy (slot); safe->setFocus (slot); return; }

        /* THE TYPE DEFAULTS ARE THE UI'S JOB, and deliberately so.
         * bb_engine_ret_create() only changes the type: it must not write
         * ret_param_def, because bb_config_load() also drives type changes
         * and would have its loaded parameters stamped over. Applying them
         * here -- on an explicit user create, and only when the type really
         * changed -- is what makes a new return audible and characteristic
         * the moment it appears, which ret.c's own comment asks for.
         *
         * The knobs mean different things under a different type, so they
         * are always reset. The LEVEL is only filled in when it is zero: a
         * user who already set a level for this slot keeps it, and slot 0's
         * level (which IS bb.verb_level) is never raised behind their back
         * by re-picking the type it already had. */
        const int prevType = safe->snap.pod.type[slot];
        if (bb_engine_ret_create (slot, result) != 0) return;
        if (result != prevType)
        {
            for (int p = 0; p < BB_RET_NPARAM; ++p)
                bb_engine_ret_param (slot, p, (int) ret_param_def[result][p]);
            bb_engine_ret_sync (slot, 0);
            if (bb_engine_ret_level_get (slot) <= 0)
                bb_engine_ret_level (slot, (int) ret_level_def[result]);
        }
        safe->setFocus (slot);
    });
}

void MixerPanel::rowMenu (int slot, juce::Component* target)
{
    juce::PopupMenu m;
    m.addSectionHeader (slotTitle (slot));
    m.addItem (1, "RENAME");
    m.addItem (2, "CHANGE TYPE");
    m.addSeparator();
    m.addItem (3, "CLEAR SENDS INTO THIS RETURN");
    m.addItem (4, "CLEAR LINKS TOUCHING THIS RETURN");
    m.addSeparator();
    m.addItem (5, "DESTROY");

    juce::Component::SafePointer<MixerPanel> safe (this);
    juce::Component::SafePointer<juce::Component> tgt (target);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target)
                                               .withMinimumWidth (230),
                     [safe, tgt, slot] (int result)
    {
        if (safe == nullptr || result == 0) return;
        safe->setFocus (slot);
        switch (result)
        {
            case 1: safe->inspector->beginRename(); break;
            case 2: safe->createMenu (slot, tgt.getComponent()); break;
            case 3: for (int s = 0; s < BB_RET_NSRC; ++s) bb_engine_ret_send (s, slot, 0); break;
            case 4: for (int q = 0; q < BB_NRET; ++q)
                    { bb_engine_ret_link (q, slot, 0); bb_engine_ret_link (slot, q, 0); } break;
            case 5: bb_engine_ret_destroy (slot); break;
            default: break;
        }
    });
}

/* ---- the 30 Hz pull ---------------------------------------------------- */

void MixerPanel::sync()
{
    /* ---- 1. the channel strips (unchanged console wiring) -------------- */
    for (auto* s : strips)
    {
        if (s->kind == Strip::Kind::Voice)
        {
            const int L = s->layer;
            const bool on = atomic_load (&bb.layer[L].on) != 0;
            s->muteBtn->setToggleStateQuiet (! on);       // plate armed = muted
            s->fader->setMuted (! on);
            if (! s->fader->isUserDragging())
                s->fader->setValueQuiet (atomic_load_explicit (&bb.layer[L].ctl[LCTL_LEVEL],
                                                               memory_order_relaxed));
            juce::String nm = "V" + juce::String (L + 1).paddedLeft ('0', 2);
            if (! bb_custom[L])
                nm << " " << juce::String (rack_src_name (bb_rack[L].src)).toUpperCase();
            s->update (nm, ! on, atomic_load (&bb.focus) == L, s->fader->value());
        }
        else if (s->kind == Strip::Kind::Master)
        {
            const bool muted = atomic_load (&bb.mute) != 0
                            || atomic_load (&bb.panic) != 0;
            s->muteBtn->setToggleStateQuiet (muted);
            s->fader->setMuted (muted);
            if (! s->fader->isUserDragging())
                s->fader->setValueQuiet (atomic_load (&bb.gain));
            s->update ("MASTER", muted, true, s->fader->value());
        }
    }

    /* ---- 2. one snapshot of the whole return system -------------------- */
    Snap n;
    Snap::Pod& p = n.pod;

    for (int r = 0; r < BB_NRET; ++r)
    {
        p.type[r]     = bb_engine_ret_type_get (r);
        p.level[r]    = juce::jlimit (0, 256, bb_engine_ret_level_get (r));
        p.mute[r]     = bb_engine_ret_mute_get (r) ? 1 : 0;
        p.division[r] = juce::jlimit (0, 10, bb_engine_ret_sync_get (r));
        p.gr[r]       = juce::jlimit (0, 256, atomic_load_explicit (&bb.ret[r].gr,
                                                                    memory_order_relaxed));
        p.pending[r]  = bb_engine_ret_pending (r) ? 1 : 0;
        p.live[r]     = typeIsKnown (p.type[r]) ? 1 : 0;
        for (int q = 0; q < BB_RET_NPARAM; ++q)
            p.param[r][q] = juce::jlimit (0, 255, bb_engine_ret_param_get (r, q));
        n.name[r] = juce::String::fromUTF8 (bb_ret_name[r]).trim();   // NUL-terminated
    }

    /* the worst gain reduction on the bus drives the dock's warning line */
    p.worstGr = 256;
    for (int r = 0; r < BB_NRET; ++r)
        if (p.live[r] && p.gr[r] < p.worstGr) p.worstGr = p.gr[r];

    for (int s = 0; s < BB_RET_NSRC; ++s)
        for (int r = 0; r < BB_NRET; ++r)
            p.send[s][r] = juce::jlimit (0, 255, bb_engine_ret_send_get (s, r));

    for (int a = 0; a < BB_NRET; ++a)
        for (int b = 0; b < BB_NRET; ++b)
            p.link[a][b] = juce::jlimit (0, 256, bb_engine_ret_link_get (a, b));

    p.active = juce::jlimit (0, BB_NRET, atomic_load_explicit (&bb.ret_active,
                                                               memory_order_relaxed));

    /* ---- 3. cycles ----------------------------------------------------
     * Adjacency over live slots only. A link q->r is an edge. A nonzero WET
     * send into r is an edge from EVERY live slot into r, because the WET
     * row is the summed return bus one frame old -- including r itself.
     * Transitive closure over eight nodes is 512 operations; an edge sits on
     * a cycle when its head can reach its tail again. */
    bool adj[BB_NRET][BB_NRET] = {};
    for (int q = 0; q < BB_NRET; ++q)
        for (int r = 0; r < BB_NRET; ++r)
            if (p.live[q] && p.live[r]
                && (p.link[q][r] > 0 || p.send[BB_RET_SRC_WET][r] > 0))
                adj[q][r] = true;

    bool reach[BB_NRET][BB_NRET];
    for (int a = 0; a < BB_NRET; ++a)
        for (int b = 0; b < BB_NRET; ++b) reach[a][b] = adj[a][b];
    for (int k = 0; k < BB_NRET; ++k)
        for (int a = 0; a < BB_NRET; ++a)
            if (reach[a][k])
                for (int b = 0; b < BB_NRET; ++b)
                    if (reach[k][b]) reach[a][b] = true;

    for (int r = 0; r < BB_NRET; ++r)
    {
        p.nodeLoop[r] = reach[r][r] ? 1 : 0;
        if (p.nodeLoop[r]) p.anyLoop = 1;
    }
    for (int a = 0; a < BB_NRET; ++a)
        for (int b = 0; b < BB_NRET; ++b)
            p.edgeLoop[a][b] = (adj[a][b] && reach[b][a]) ? 1 : 0;

    /* shortest cycle through the lowest slot that has one, as "A>C>A" */
    for (int start = 0; start < BB_NRET && n.loopPath.isEmpty(); ++start)
    {
        if (! p.nodeLoop[start]) continue;
        int prev[BB_NRET]; bool seen[BB_NRET] = {};
        for (int i = 0; i < BB_NRET; ++i) prev[i] = -1;
        int queue[BB_NRET + 1], head = 0, tail = 0, closed = -1;
        queue[tail++] = start; seen[start] = true;
        while (head < tail && closed < 0)
        {
            const int cur = queue[head++];
            for (int nx = 0; nx < BB_NRET && closed < 0; ++nx)
            {
                if (! adj[cur][nx]) continue;
                if (nx == start) { closed = cur; break; }
                if (! seen[nx]) { seen[nx] = true; prev[nx] = cur; queue[tail++] = nx; }
            }
        }
        if (closed < 0) continue;

        /* walk the tree back from the node that closed the cycle. The walk
         * ends AT `start` (prev[start] is -1), so the reversed chain already
         * begins with start and only the closing hop has to be appended --
         * prefixing start as well would print A>A>C>A. */
        int chain[BB_NRET + 1], nlen = 0;
        for (int c = closed; c >= 0 && nlen < BB_NRET; c = prev[c]) chain[nlen++] = c;
        juce::String path;
        for (int i = nlen - 1; i >= 0; --i)
        {
            if (path.isNotEmpty()) path += U8 ("\xe2\x86\x92");
            path += slotTag (chain[i]);
        }
        path += U8 ("\xe2\x86\x92") + slotTag (start);
        n.loopPath = path;
    }

    /* ---- 4. diff, then repaint ---------------------------------------- */
    bool changed = std::memcmp (&n.pod, &snap.pod, sizeof (Snap::Pod)) != 0
                || n.loopPath != snap.loopPath;
    for (int r = 0; r < BB_NRET && ! changed; ++r)
        changed = n.name[r] != snap.name[r];

    if (changed)
    {
        snap = n;
        refreshChildren();
        repaint();
    }

    /* the send knobs pull every frame: they are the one control whose engine
     * value can move without the snapshot changing (the user is dragging it) */
    for (auto* s : strips)
        if (s->sendKnob && ! s->sendKnob->isUserDragging())
            s->sendKnob->setValueQuiet (snap.pod.send[s->src][focus]);
}

void MixerPanel::refreshChildren()
{
    /* The knob tooltips and the greyed state depend ONLY on which return is
     * focused and what it is. refreshChildren() runs whenever any engine
     * value moves -- including the limiter's gain reduction, which moves
     * every frame while a loop is being held -- so rebuilding ten tooltip
     * strings here unconditionally would allocate thirty times a second for
     * nothing. */
    const juce::String tgt = slotTitle (focus);
    const bool liveTgt = snap.pod.live[focus] != 0;
    if (tgt != stripTipTitle || liveTgt != stripTipLive)
    {
        stripTipTitle = tgt;
        stripTipLive  = liveTgt;
        for (auto* s : strips)
        {
            s->sendKnob->setUnused (! liveTgt);
            s->sendKnob->setTooltip (
                U8 ("SEND \xe2\x80\x94 ") + srcName (s->src) + U8 (" into RETURN ") + tgt
                + U8 (". 0\xe2\x80\x93" "255, post-fader. Click a cell in the row below "
                      "the knob to point every strip at a different return."));
        }
    }
    for (auto* s : strips)
    {
        if (! s->sendKnob->isUserDragging())
            s->sendKnob->setValueQuiet (snap.pod.send[s->src][focus]);
        s->repaint();
    }

    bool anyEmpty = false;
    for (int r = 0; r < BB_NRET; ++r) anyEmpty |= snap.pod.type[r] == RET_NONE;
    addBtn->setEnabled (anyEmpty);
    panicBtn->setLamp (snap.pod.anyLoop != 0);

    rack->refresh();
    matrix->repaint();
    links->repaint();
    inspector->refresh();
}

/* ---- layout ------------------------------------------------------------ */

void MixerPanel::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    footerArea = b.removeFromBottom (26);

    /* the routing dock takes the bottom 42% of what is left, clamped so the
     * channel strips never lose their fader */
    dockArea = b.removeFromBottom (juce::jlimit (200, 330, b.getHeight() * 42 / 100));
    stripsArea = b;

    dockLabelArea = dockArea.removeFromTop (20);
    {
        const int bw = 74;
        panicBtn->setBounds (dockLabelArea.getRight() - 8 - bw, dockLabelArea.getY() + 2, bw, 16);
        addBtn->setBounds (dockLabelArea.getRight() - 8 - bw - 6 - 56,
                           dockLabelArea.getY() + 2, 56, 16);
    }

    Rectangle<int> d = dockArea;
    const int W = d.getWidth();
    rackArea      = d.removeFromLeft (juce::jlimit (180, 344, W * 28 / 100));
    matrixArea    = d.removeFromLeft (juce::jlimit (140, 256, W * 21 / 100));
    linkArea      = d.removeFromLeft (juce::jlimit (140, 248, W * 20 / 100));
    inspectorArea = d;

    rack->setBounds (rackArea);
    matrix->setBounds (matrixArea);
    links->setBounds (linkArea);
    inspector->setBounds (inspectorArea);

    /* 9 strips flex 1 + master flex 1.4 -> integer edges at W*u/10.4 */
    const double SW = (double) stripsArea.getWidth();
    int ex[11];
    for (int i = 0; i <= 10; ++i)
    {
        const double units = i <= 9 ? (double) i : 10.4;
        ex[i] = stripsArea.getX() + juce::roundToInt (SW * units / 10.4);
    }
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (ex[i], stripsArea.getY(),
                              ex[i + 1] - ex[i], stripsArea.getHeight());
}

/* ---- paint ------------------------------------------------------------- */

void MixerPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "MIXER",
                     U8 ("8 VOICES + SAMPLER + 8 RETURN SLOTS + MASTER"),
                     juce::String (SerialNo::MIXER)
                         + U8 (" \xc2\xb7 SENDS 0\xe2\x80\x93" "255 \xc2\xb7 EVERY LINK ONE SAMPLE OLD"),
                     Badge::PARTIAL, "PARTIAL");

    /* -- dock label row: the routing header, and where danger is reported - */
    g.setColour (C::RAISED);
    g.fillRect (dockLabelArea);
    g.setColour (C::HAIRLINE);
    g.fillRect (dockLabelArea.getX(), dockLabelArea.getY(), dockLabelArea.getWidth(), 1);
    g.fillRect (dockLabelArea.getX(), dockLabelArea.getBottom() - 1,
                dockLabelArea.getWidth(), 1);

    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (9.0f, 0.16f));
    g.drawText ("RETURN BUS", dockLabelArea.getX() + 10, dockLabelArea.getY(),
                200, dockLabelArea.getHeight(), Justification::centredLeft);

    const int warnX = dockLabelArea.getX() + 116;
    const int warnW = juce::jmax (40, dockLabelArea.getWidth() - 116 - 160);
    juce::String warn;
    juce::Colour warnFg = C::INK_FAINT;
    if (snap.pod.anyLoop)
    {
        warn = U8 ("FEEDBACK LOOP ") + snap.loopPath;
        warnFg = C::BLOOD_HOT;
        if (snap.pod.worstGr < 256)
            warn += U8 ("  \xc2\xb7  LIMITING ") + grText (snap.pod.worstGr) + " dB";
    }
    else if (snap.pod.worstGr < 256)
    {
        warn = U8 ("LIMITING ") + grText (snap.pod.worstGr) + " dB";
        warnFg = C::AMBER;
    }
    else
    {
        warn = U8 ("NO FEEDBACK PATH \xc2\xb7 EVERY LINK IS ONE SAMPLE OLD");
    }
    g.setColour (warnFg);
    g.setFont (Type::mono (8.0f, 0.10f));
    g.drawText (warn, warnX, dockLabelArea.getY(), warnW, dockLabelArea.getHeight(),
                Justification::centredLeft, true);

    /* dock column dividers are drawn by each child's own right edge */
    g.setColour (C::PANEL);
    g.fillRect (dockArea);

    /* -- footer 26 ------------------------------------------------------- */
    g.setColour (C::PANEL_ALT);
    g.fillRect (footerArea);
    g.setColour (C::HAIRLINE);
    g.fillRect (footerArea.getX(), footerArea.getY(), footerArea.getWidth(), 1);

    g.setFont (Type::mono (8.0f, 0.12f));
    g.setColour (C::INK_FAINT);
    g.drawText (U8 ("FOCUS ") + slotTitle (focus)
                    + U8 ("  \xc2\xb7  RETURNS ") + juce::String (snap.pod.active)
                    + "/" + juce::String (BB_NRET)
                    + U8 ("  \xc2\xb7  SEND KNOB ON EVERY STRIP FEEDS THE FOCUSED RETURN"),
                footerArea.getX() + 10, footerArea.getY() + 1,
                footerArea.getWidth() - 20, footerArea.getHeight() - 1,
                Justification::centredLeft, true);

    g.setColour (C::OXIDE);
    g.drawText (U8 ("NO PAN \xc2\xb7 NO SOLO \xc2\xb7 NO LICKS BUS LEVEL: NO ENGINE"),
                footerArea.withTrimmedRight (10).withTrimmedTop (1),
                Justification::centredRight);
}

} // namespace morgue
