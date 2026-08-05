/* LicksPanel.cpp -- see LicksPanel.h. All R1 engine wiring preserved:
 * bb_engine_sampler_set/clear/loaded, mute/solo/level/choke atomics, the
 * tri-state gate cycle, keys 1-8, and the quiet sync() with isUserDragging
 * guards. The geometry is spec section 7 / HTML frame "03 GRAIN LICKS":
 * toolbar 28, step header 22, 8 equal-flex slot rows (head 300, 16-column
 * StepCell grid with 3px padding, right gutter 140). */

#include "LicksPanel.h"
#include "AudioEngine.h"
#include "bytebeat.h"
#include "engine.h"
#include "gen.h"

#include <cmath>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

/* row geometry (spec section 7) */
static constexpr int kHeadW   = 300;   // row head / step-header left gutter
static constexpr int kGutW    = 140;   // right gutter (CHOKE + meter)
static constexpr int kToolbarH = 28;
static constexpr int kStepHdrH = 22;

static int textW (const juce::Font& f, const juce::String& s)
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
}

/* grid column edges: 16 equal-flex columns between the gutters */
static int colEdge (int gridX, int gridW, int i)
{
    return gridX + (i * gridW) / 16;
}

/* ======================================================================== */
/*  ChokeTag -- the G1..G4 oxide tag ("--" when none). Click cycles the      */
/*  group 0->1->2->3->4->0, right-click clears. Engine field: SMP_CTL_CHOKE. */
/* ======================================================================== */
namespace
{
class ChokeTag : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    std::function<void (int)> onChange;

    void setGroupQuiet (int g)
    {
        g = juce::jlimit (0, 4, g);
        if (g != group) { group = g; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        group = e.mods.isPopupMenu() ? 0 : (group + 1) % 5;
        if (onChange) onChange (group);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const bool grp = group > 0;
        g.setColour (grp ? C::OXIDE_PLATE : C::PLATE_LOW);
        g.fillRect (getLocalBounds());
        g.setColour (grp ? C::OXIDE_DIM : C::HAIRLINE);
        g.drawRect (getLocalBounds(), 1);
        g.setColour (grp ? C::OXIDE_INK : C::INK_GHOST);
        g.setFont (Type::mono (8.0f));
        g.drawText (grp ? "G" + juce::String (group) : U8 ("\xe2\x80\x94"),
                    getLocalBounds(), Justification::centred);
    }

private:
    int group = 0;
};
} // namespace

/* ======================================================================== */
/*  ToolTag -- toolbar tag plate (HTML: padding 2 7, 9px .12em).             */
/*  ACTIVE  = PATTERN A   (#1b1a17 / #3a3833 / INK)                          */
/*  IDLE    = fill action (#131211 / #232220 / INK_DIM, hover PLATE_HOVER)   */
/*  INERT   = pattern B-D (#131211 / #232220 / #6b6760; engine has one bank) */
/* ======================================================================== */
class LicksPanel::ToolTag : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    enum Style { ACTIVE, IDLE, INERT };

    ToolTag (const juce::String& t, Style s) : text (t), style (s)
    {
        setRepaintsOnMouseActivity (true);
    }

    std::function<void()> onClick;

    int idealWidth() const
    {
        return textW (Type::mono (9.0f, 0.12f), text) + 16;   // padding 7 + 1px borders
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (style != INERT && ! e.mods.isPopupMenu() && onClick)
            onClick();
    }

    void paint (juce::Graphics& g) override
    {
        juce::Colour bg, bd, fg;
        const bool hov = style == IDLE && isMouseOver();
        switch (style)
        {
            case ACTIVE: bg = C::PLATE_HOVER; bd = C::EDGE; fg = C::INK; break;
            case IDLE:   bg = hov ? C::PLATE_HOVER : C::PLATE_LOW;
                         bd = C::HAIRLINE;
                         fg = hov ? C::INK : C::INK_DIM; break;
            default:     bg = C::PLATE_LOW; bd = C::HAIRLINE; fg = C::TAB_INACTIVE_FG; break;
        }
        g.setColour (bg);
        g.fillRect (getLocalBounds());
        g.setColour (bd);
        g.drawRect (getLocalBounds(), 1);
        g.setColour (fg);
        g.setFont (Type::mono (9.0f, 0.12f));
        g.drawText (text, getLocalBounds(), Justification::centred);
    }

private:
    juce::String text;
    Style style;
};

/* ======================================================================== */
/*  LickSlotRow -- one sampler slot (spec: "8 slot rows, equal flex ~76").   */
/*  Head 300: index 16, name + meta stacked, M/S 18x18, divider, PIT/VEL/    */
/*  LVL 26px knobs. Grid: 16 StepCells, 3px padding, HAIRLINE_DIM columns.  */
/*  Gutter 140: CHOKE label + group tag + 6x26 vertical meter.               */
/* ======================================================================== */
class LicksPanel::LickSlotRow : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    LickSlotRow (LicksPanel& p, int slotIndex) : panel (p), idx (slotIndex)
    {
        setOpaque (true);
        setTooltip ("SLOT " + juce::String (idx + 1)
                    + U8 (" \xe2\x80\x94 double-click or drag a file to load, right-click clears. "
                          "Keys 1\xe2\x80\x93""8 focus a slot."));

        mute.setTooltip (U8 ("MUTE \xe2\x80\x94 silences this slot."));
        addAndMakeVisible (mute);
        mute.onToggle = [this] (bool on)
        { atomic_store (&bb.sampler[idx].mute, on ? 1 : 0); };

        solo.setTooltip (U8 ("SOLO \xe2\x80\x94 only soloed slots sound."));
        addAndMakeVisible (solo);
        solo.onToggle = [this] (bool on)
        { atomic_store (&bb.sampler[idx].solo, on ? 1 : 0); };

        /* PIT/VEL move all 16 steps RELATIVELY (delta from the last shown
         * knob value, clamped per step) so per-step motion -- the factory
         * kick's pitch fall, session-restored patterns -- is shifted, never
         * flattened. When the steps agree the knob shows the common value. */
        pit.setShowText (false);
        pit.setTooltip (U8 ("PIT \xe2\x80\x94 semitone trim. Shifts all 16 steps of this "
                            "slot together, keeping per-step motion. \xe2\x88\x92""12 to +12."));
        addAndMakeVisible (pit);
        pit.onChange = [this] (int v)
        {
            const int d = v - pitShown;
            pitShown = v;
            if (d == 0) return;
            for (int st = 0; st < BB_STEPS; ++st)
            {
                const int cur = atomic_load (&bb.sampler[idx].pitch[st]);
                atomic_store (&bb.sampler[idx].pitch[st],
                              bb_clampi (cur + d, -12, 12));
            }
        };

        vel.setShowText (false);
        vel.setTooltip (U8 ("VEL \xe2\x80\x94 velocity trim. Shifts all 16 steps of this "
                            "slot together, keeping per-step motion. 0\xe2\x80\x93""255."));
        addAndMakeVisible (vel);
        vel.onChange = [this] (int v)
        {
            const int d = v - velShown;
            velShown = v;
            if (d == 0) return;
            for (int st = 0; st < BB_STEPS; ++st)
            {
                const int cur = atomic_load (&bb.sampler[idx].vel[st]);
                atomic_store (&bb.sampler[idx].vel[st],
                              bb_clampi (cur + d, 0, 255));
            }
        };

        lvl.setShowText (false);
        lvl.setTooltip (U8 ("LVL \xe2\x80\x94 slot mix level into the master bus. 0\xe2\x80\x93""256."));
        addAndMakeVisible (lvl);
        lvl.onChange = [this] (int v)
        { atomic_store (&bb.sampler[idx].ctl[SMP_CTL_LEVEL], v); };

        choke.setTooltip (U8 ("CHOKE \xe2\x80\x94 firing this slot silences the other slots "
                              "in its group. Click cycles the group, right-click clears. "
                              "Groups 1\xe2\x80\x93""4."));
        addAndMakeVisible (choke);
        choke.onChange = [this] (int gp)
        { atomic_store (&bb.sampler[idx].ctl[SMP_CTL_CHOKE], gp); };

        /* live meter: the engine max-holds bb.sampler[s].peak per period;
         * read-and-clear here, decay on the UI side for the fall */
        meter.source = [held = std::make_shared<float> (0.0f), s = idx]() -> float
        {
            const int pk = atomic_exchange (&bb.sampler[s].peak, 0);
            const float v = (float) pk / 32768.0f;
            *held = juce::jmax (v, *held * 0.82f);
            return *held < 0.004f ? 0.0f : *held;
        };
        addAndMakeVisible (meter);

        for (int st = 0; st < BB_STEPS; ++st)
        {
            auto* c = cells.add (new StepCell (st));
            c->setShowIndex (false);
            c->setTooltip (U8 ("STEP ") + juce::String (st + 1)
                           + U8 (" \xe2\x80\x94 click cycles OFF \xe2\x86\x92 HIT \xe2\x86\x92 "
                                 "ACCENT, right-click clears, drag paints."));
            c->onEdit = [this] (int step, StepCell::State s)
            {
                atomic_store (&bb.sampler[idx].gate[step], (int) s);
                panel.focusSlot (idx);
            };
            addAndMakeVisible (c);
        }
    }

    void update (int play, bool foc, bool load,
                 const juce::String& nm, const juce::String& mt)
    {
        mute.setToggleStateQuiet (atomic_load (&bb.sampler[idx].mute) != 0);
        solo.setToggleStateQuiet (atomic_load (&bb.sampler[idx].solo) != 0);
        /* pull PIT/VEL only when the 16 steps agree; a mixed lane keeps the
         * knob where it is (the knob is a relative trim, and pretending
         * step 0 is the lane value would lie about the pattern) */
        if (! pit.isUserDragging())
        {
            const int p0v = atomic_load (&bb.sampler[idx].pitch[0]);
            bool same = true;
            for (int st = 1; st < BB_STEPS && same; ++st)
                same = atomic_load (&bb.sampler[idx].pitch[st]) == p0v;
            if (same) pit.setValueQuiet (p0v);
            pitShown = pit.value();
        }
        if (! vel.isUserDragging())
        {
            const int v0 = atomic_load (&bb.sampler[idx].vel[0]);
            bool same = true;
            for (int st = 1; st < BB_STEPS && same; ++st)
                same = atomic_load (&bb.sampler[idx].vel[st]) == v0;
            if (same) vel.setValueQuiet (v0);
            velShown = vel.value();
        }
        if (! lvl.isUserDragging())
            lvl.setValueQuiet (atomic_load (&bb.sampler[idx].ctl[SMP_CTL_LEVEL]));
        choke.setGroupQuiet (atomic_load (&bb.sampler[idx].ctl[SMP_CTL_CHOKE]));

        for (int st = 0; st < BB_STEPS; ++st)
        {
            const int gv = bb_clampi (atomic_load (&bb.sampler[idx].gate[st]), 0, 2);
            cells[st]->setState ((StepCell::State) gv);
            cells[st]->setPlayhead (st == play);
        }

        bool rp = false;
        if (focused != foc) { focused = foc; rp = true; }
        if (loaded != load) { loaded = load; rp = true; }
        if (name != nm)     { name = nm;     rp = true; }
        if (meta != mt)     { meta = mt;     rp = true; }
        if (rp) repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.x >= kHeadW) return;
        if (e.mods.isPopupMenu()) { panel.clearSlot (idx); return; }
        panel.focusSlot (idx);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.x >= kHeadW) return;
        panel.focusSlot (idx);
        panel.loadFile (idx);
    }

    void resized() override
    {
        const int W = getWidth(), H = getHeight();

        /* head, right-anchored inside 300 with 8px padding (HTML gap 8) */
        const int kx = kHeadW - 8 - (26 * 3 + 6 * 2);       // knob block: 3 faces, gap 6
        const int ky = (H - 33) / 2;                        // 26 face + 1 + 6 label
        pit.setBounds (kx,      ky, 26, 26);
        vel.setBounds (kx + 32, ky, 26, 26);
        lvl.setBounds (kx + 64, ky, 26, 26);
        divX = kx - 9;                                      // 1px divider, 8px gaps
        const int sy = (H - 18) / 2;
        solo.setBounds (divX - 8 - 18, sy, 18, 18);
        mute.setBounds (divX - 8 - 38, sy, 18, 18);
        idxR  = { 8, 0, 16, H };
        nameR = { 32, (H - 22) / 2, mute.getX() - 8 - 32, 12 };
        metaR = { 32, nameR.getBottom() + 2, nameR.getWidth(), 8 };

        /* grid: 16 equal-flex columns, 1px HAIRLINE_DIM separator, 3px padding */
        const int gridW = W - kHeadW - kGutW;
        for (int st = 0; st < BB_STEPS; ++st)
        {
            const int l = colEdge (kHeadW, gridW, st);
            const int r = colEdge (kHeadW, gridW, st + 1);
            cells[st]->setBounds (Rectangle<int> (l, 0, r - l - 1, H - 1).reduced (3));
        }

        /* right gutter: CHOKE label, group tag, 6x26 meter (padding 8) */
        const int gx = W - kGutW;
        chokeLabW = textW (Type::mono (7.0f, 0.08f), "CHOKE");
        chokeLabR = { gx + 8, 0, chokeLabW, H };
        choke.setBounds (gx + 8 + chokeLabW + 6, (H - 14) / 2, 26, 14);
        meter.setBounds (W - 8 - 6, (H - 26) / 2, 6, 26);
    }

    void paint (juce::Graphics& g) override
    {
        const int W = getWidth(), H = getHeight();

        g.setColour (C::PANEL);
        g.fillRect (0, 0, W, H);

        /* rules: head border-right, gutter border-left, column separators,
         * row bottom (all 1px; spec rule 0.2) */
        g.setColour (C::HAIRLINE);
        g.fillRect (kHeadW - 1, 0, 1, H);
        g.fillRect (W - kGutW, 0, 1, H);
        g.fillRect (divX, (H - 24) / 2, 1, 24);
        g.setColour (C::HAIRLINE_DIM);
        const int gridW = W - kHeadW - kGutW;
        for (int st = 0; st < BB_STEPS; ++st)
            g.fillRect (colEdge (kHeadW, gridW, st + 1) - 1, 0, 1, H);
        g.fillRect (0, H - 1, W, 1);

        /* index (locker rule: BLOOD_HOT when selected) */
        g.setFont (Type::mono (9.0f));
        g.setColour (focused ? C::BLOOD_HOT : C::INK_FAINT);
        g.drawText (juce::String (idx + 1).paddedLeft ('0', 2), idxR,
                    Justification::centredLeft);

        /* name + meta stacked */
        g.setFont (Type::mono (10.0f, 0.06f));
        g.setColour (loaded ? C::INK : C::INK_GHOST);
        g.drawText (loaded ? name : U8 ("\xe2\x80\x94 EMPTY \xe2\x80\x94"),
                    nameR, Justification::centredLeft, true);
        g.setFont (Type::mono (7.0f, 0.08f));
        g.setColour (C::INK_FAINT);
        g.drawText (loaded ? meta : "DOUBLE-CLICK OR DRAG TO LOAD",
                    metaR, Justification::centredLeft, true);

        /* 6px knob sub-notes (spec section 2: knob sub-notes may be 6px) */
        g.setFont (Type::nano (6.0f));
        g.setColour (C::INK_FAINT);
        g.drawText ("PIT", pit.getX(), pit.getBottom() + 1, 26, 6, Justification::centred);
        g.drawText ("VEL", vel.getX(), vel.getBottom() + 1, 26, 6, Justification::centred);
        g.drawText ("LVL", lvl.getX(), lvl.getBottom() + 1, 26, 6, Justification::centred);

        /* CHOKE label */
        g.setFont (Type::mono (7.0f, 0.08f));
        g.setColour (C::INK_FAINT);
        g.drawText ("CHOKE", chokeLabR, Justification::centredLeft);
    }

private:
    LicksPanel& panel;
    int idx;

    PlateButton  mute { "M" }, solo { "S" };
    EngravedKnob pit { "PIT", 26, -12, 12, 0 };
    EngravedKnob vel { "VEL", 26, 0, 255, 200 };
    int pitShown = 0, velShown = 200;      // last knob value the trim saw
    EngravedKnob lvl { "LVL", 26, 0, 256, 220 };
    ChokeTag     choke;
    MeterComponent meter;
    juce::OwnedArray<StepCell> cells;

    bool focused = false, loaded = false;
    juce::String name, meta;
    int divX = 0, chokeLabW = 0;
    Rectangle<int> idxR, nameR, metaR, chokeLabR;
};

/* ======================================================================== */
/*  LicksPanel                                                               */
/* ======================================================================== */

LicksPanel::LicksPanel (AudioEngine& a) : audio (a)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    for (int s = 0; s < BB_SAMPLER; ++s)
    {
        addAndMakeVisible (rows.add (new LickSlotRow (*this, s)));
        names.add ({});
        metas.add ({});
    }

    /* toolbar: PATTERN A live; B-D drawn only -- the engine holds a single
     * pattern bank (do not fake more) */
    static const char* patNames[] = { "A", "B", "C", "D" };
    for (int i = 0; i < 4; ++i)
    {
        auto* t = patternTags.add (new ToolTag (patNames[i],
                                                i == 0 ? ToolTag::ACTIVE : ToolTag::INERT));
        t->setTooltip (i == 0
            ? U8 ("PATTERN A \xe2\x80\x94 the live pattern. The engine holds one "
                  "16-step pattern per slot.")
            : "PATTERN " + juce::String (patNames[i])
                + U8 (" \xe2\x80\x94 pattern bank. Planned; the engine plays bank A only."));
        addAndMakeVisible (t);
    }

    auto* euc = fillTags.add (new ToolTag ("EUCLID", ToolTag::IDLE));
    euc->setTooltip (U8 ("FILL EUCLID \xe2\x80\x94 spreads pulses evenly over the focused "
                         "slot's 16 steps. Each press adds one pulse, 1\xe2\x80\x93""16."));
    euc->onClick = [this] { fillEuclid(); };
    addAndMakeVisible (euc);

    auto* rnd = fillTags.add (new ToolTag ("RAND", ToolTag::IDLE));
    rnd->setTooltip (U8 ("FILL RAND \xe2\x80\x94 rolls a random gate pattern for the "
                         "focused slot. About 4 in 10 steps hit."));
    rnd->onClick = [this] { fillRand(); };
    addAndMakeVisible (rnd);

    auto* clr = fillTags.add (new ToolTag ("CLEAR", ToolTag::IDLE));
    clr->setTooltip (U8 ("FILL CLEAR \xe2\x80\x94 wipes all 16 gates of the focused slot."));
    clr->onClick = [this] { fillClear(); };
    addAndMakeVisible (clr);

    sync();
    setSize (1204, 764);
}

LicksPanel::~LicksPanel() = default;

void LicksPanel::sync()
{
    static const char* kit[] = { "kick", "snare", "hat" };
    const int play = atomic_load (&bb.seq_pos);

    for (int s = 0; s < BB_SAMPLER; ++s)
    {
        const bool loaded = bb_engine_sampler_loaded (s) != 0;
        if (loaded && names[s].isEmpty())
        {
            names.set (s, (s < 3) ? juce::String (kit[s]) : juce::String ("(sample)"));
            metas.set (s, "FACTORY ONE-SHOT");
        }
        if (! loaded && names[s].isNotEmpty())
        {
            names.set (s, {});
            metas.set (s, {});
        }
        rows[s]->update (play, s == slot, loaded, names[s], metas[s]);
    }

    if (play != lastPlay)
    {
        lastPlay = play;
        repaint (stepHeaderRect);
    }
}

void LicksPanel::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    toolbarRect    = b.removeFromTop (kToolbarH);
    stepHeaderRect = b.removeFromTop (kStepHdrH);
    rowsRect       = b;

    layoutToolbar();

    for (int s = 0; s < BB_SAMPLER; ++s)
    {
        const int y0 = rowsRect.getY() + (s * rowsRect.getHeight()) / 8;
        const int y1 = rowsRect.getY() + ((s + 1) * rowsRect.getHeight()) / 8;
        rows[s]->setBounds (0, y0, getWidth(), y1 - y0);
    }
}

void LicksPanel::layoutToolbar()
{
    const juce::Font lab = Type::mono (9.0f, 0.12f);
    const int tagY = toolbarRect.getY() + (kToolbarH - 17) / 2;

    int x = toolbarRect.getX() + 10;
    patLabelR = { x, toolbarRect.getY(), textW (lab, "PATTERN"), kToolbarH };
    x = patLabelR.getRight() + 8;
    for (auto* t : patternTags)
    {
        t->setBounds (x, tagY, t->idealWidth(), 17);
        x += t->idealWidth() + 2;
    }
    x += 6;                                                 // group gap 8 (2 already)
    tbDividerR = { x, toolbarRect.getY() + (kToolbarH - 16) / 2, 1, 16 };
    x += 1 + 8;
    fillLabelR = { x, toolbarRect.getY(), textW (lab, "FILL"), kToolbarH };
    x = fillLabelR.getRight() + 8;
    for (auto* t : fillTags)
    {
        t->setBounds (x, tagY, t->idealWidth(), 17);
        x += t->idealWidth() + 2;
    }
}

void LicksPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "GRAIN LICKS",
                     U8 ("STEP SAMPLER \xc2\xb7 8 SLOTS \xc3\x97 16 STEPS"),
                     juce::String (SerialNo::LICKS)
                         + U8 (" \xc2\xb7 CLOCK: bb.seq_pos \xc2\xb7 RENDERS TO MASTER BUS"),
                     Badge::LIVE, "LIVE");

    /* toolbar 28: labels + tags (tags are children) + right hints */
    g.setColour (C::PANEL_ALT);
    g.fillRect (toolbarRect);
    g.setColour (C::HAIRLINE);
    g.fillRect (toolbarRect.getX(), toolbarRect.getBottom() - 1,
                toolbarRect.getWidth(), 1);
    g.fillRect (tbDividerR);

    const juce::Font lab = Type::mono (9.0f, 0.12f);
    g.setFont (lab);
    g.setColour (C::INK_FAINT);
    g.drawText ("PATTERN", patLabelR, Justification::centredLeft);
    g.drawText ("FILL",    fillLabelR, Justification::centredLeft);

    const juce::Font hintF = Type::mono (8.0f, 0.12f);
    g.setFont (hintF);
    const juce::String h1 ("ONE-SHOT RESETS pos=0");
    const juce::String h2 = U8 ("CHOKE GROUPS 1\xe2\x80\x93""4");
    const int w2 = textW (hintF, h2);
    const int w1 = textW (hintF, h1);
    const int x2 = toolbarRect.getRight() - 10 - w2;
    g.drawText (h2, x2, toolbarRect.getY(), w2, kToolbarH, Justification::centredLeft);
    g.drawText (h1, x2 - 12 - w1, toolbarRect.getY(), w1, kToolbarH,
                Justification::centredLeft);

    /* step header 22: 300 gutter, 16 numbered columns (current tinted
     * #1b1a17), 140 right gutter */
    const int hy = stepHeaderRect.getY();
    const int gridW = getWidth() - kHeadW - kGutW;
    g.setFont (Type::mono (8.0f));
    for (int st = 0; st < BB_STEPS; ++st)
    {
        const int l = colEdge (kHeadW, gridW, st);
        const int r = colEdge (kHeadW, gridW, st + 1);
        if (st == lastPlay)
        {
            g.setColour (C::PLATE_HOVER);
            g.fillRect (l, hy, r - l - 1, kStepHdrH);
        }
        g.setColour (C::HAIRLINE_DIM);
        g.fillRect (r - 1, hy, 1, kStepHdrH);
        g.setColour (st == lastPlay ? C::INK_DIM : C::INK_FAINT);
        g.drawText (juce::String (st + 1).paddedLeft ('0', 2),
                    l, hy, r - l - 1, kStepHdrH, Justification::centred);
    }
    g.setColour (C::HAIRLINE);
    g.fillRect (kHeadW - 1, hy, 1, kStepHdrH);              // left gutter border-right
    g.fillRect (getWidth() - kGutW, hy, 1, kStepHdrH);      // right gutter border-left
    g.fillRect (stepHeaderRect.getX(), stepHeaderRect.getBottom() - 1,
                stepHeaderRect.getWidth(), 1);
}

bool LicksPanel::keyPressed (const juce::KeyPress& key)
{
    const int k = key.getKeyCode();
    if (k >= '1' && k <= '8') { focusSlot (k - '1'); return true; }
    return false;
}

void LicksPanel::focusSlot (int s)
{
    s = juce::jlimit (0, BB_SAMPLER - 1, s);
    grabKeyboardFocus();
    if (slot == s) return;
    slot = s;
    sync();
}

int LicksPanel::slotAtY (int y) const
{
    if (rowsRect.getHeight() <= 0) return slot;
    return juce::jlimit (0, BB_SAMPLER - 1,
                         ((y - rowsRect.getY()) * 8) / rowsRect.getHeight());
}

/* ---- FILL: EUCLID / RAND / CLEAR act on the focused slot ---------------- */

void LicksPanel::fillEuclid()
{
    int gate[BB_STEPS];
    euclidK[slot] = euclidK[slot] % BB_STEPS + 1;           // TUI convention: cycle 1..16
    gen_euclid (BB_STEPS, euclidK[slot], gate);
    for (int st = 0; st < BB_STEPS; ++st)
        atomic_store (&bb.sampler[slot].gate[st], gate[st]);
    sync();
}

void LicksPanel::fillRand()
{
    auto& rng = juce::Random::getSystemRandom();
    for (int st = 0; st < BB_STEPS; ++st)
    {
        const bool on = rng.nextInt (100) < 38;             // TUI density
        const int  gv = on ? (rng.nextInt (100) < 25 ? SMP_GATE_ACCENT : SMP_GATE_ON)
                           : SMP_GATE_OFF;
        atomic_store (&bb.sampler[slot].gate[st], gv);
    }
    sync();
}

void LicksPanel::fillClear()
{
    for (int st = 0; st < BB_STEPS; ++st)
        atomic_store (&bb.sampler[slot].gate[st], SMP_GATE_OFF);
    sync();
}

/* ---- loading ------------------------------------------------------------ */

void LicksPanel::loadFile (int s)
{
    if (chooserOpen) return;
    chooser = std::make_unique<juce::FileChooser> (
        "Load a specimen",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac");
    chooserOpen = true;

    juce::Component::SafePointer<LicksPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
                          [safe, s] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        safe->chooserOpen = false;
        juce::File f = fc.getResult();
        if (f.existsAsFile())
            safe->loadPath (s, f);
    });
}

void LicksPanel::loadPath (int s, const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (audio.getFormats().createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0
        || r->lengthInSamples > 0x7fffffff / 2)
        return;

    const int len = (int) r->lengthInSamples;
    juce::AudioBuffer<float> tmp (2, len);
    r->read (&tmp, 0, len, 0, true, true);
    int16_t* mono = (int16_t*) calloc ((size_t) len, sizeof (int16_t));
    if (mono == nullptr) return;

    for (int i = 0; i < len; ++i)
    {
        const float m = tmp.getNumChannels() > 1
                ? (tmp.getSample (0, i) + tmp.getSample (1, i)) * 0.5f
                : tmp.getSample (0, i);
        const int32_t v = (int32_t) (m * 32767.0f);
        mono[i] = (int16_t) juce::jlimit (-32768, 32767, v);
    }

    /* bb_engine_sampler_set takes ownership of mono (frees it on failure) */
    if (bb_engine_sampler_set (s, mono, len, (int) r->sampleRate) != 0)
    {
        atomic_store (&bb.sampler[s].on, 1);
        names.set (s, f.getFileNameWithoutExtension());
        metas.set (s, juce::String ((int) r->sampleRate) + U8 (" Hz \xc2\xb7 ")
                          + juce::String (len) + " SMP");
        sync();
    }
}

void LicksPanel::clearSlot (int s)
{
    bb_engine_sampler_clear (s);
    atomic_store (&bb.sampler[s].on, 0);
    for (int st = 0; st < BB_STEPS; ++st)
        atomic_store (&bb.sampler[s].gate[st], SMP_GATE_OFF);
    names.set (s, {});
    metas.set (s, {});
    sync();
}

/* ---- drag to load ------------------------------------------------------- */

static bool isAudioPath (const juce::String& p)
{
    return p.endsWithIgnoreCase (".wav") || p.endsWithIgnoreCase (".aif")
        || p.endsWithIgnoreCase (".aiff") || p.endsWithIgnoreCase (".mp3")
        || p.endsWithIgnoreCase (".ogg") || p.endsWithIgnoreCase (".flac");
}

bool LicksPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioPath (f))
            return true;
    return false;
}

void LicksPanel::filesDropped (const juce::StringArray& files, int, int y)
{
    for (const auto& p : files)
    {
        if (! isAudioPath (p)) continue;
        const int s = slotAtY (y);
        focusSlot (s);
        loadPath (s, juce::File (p));
        return;
    }
}

bool LicksPanel::isInterestedInDragSource (const SourceDetails& d)
{
    const juce::String p = d.description.toString();
    return p.startsWithChar ('/') && isAudioPath (p)
        && juce::File (p).existsAsFile();
}

void LicksPanel::itemDropped (const SourceDetails& d)
{
    const juce::String p = d.description.toString();
    if (! (p.startsWithChar ('/') && juce::File (p).existsAsFile())) return;
    const int s = slotAtY (d.localPosition.y);
    focusSlot (s);
    loadPath (s, juce::File (p));
}

} // namespace morgue
