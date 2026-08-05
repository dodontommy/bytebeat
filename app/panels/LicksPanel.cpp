/* LicksPanel.cpp -- see LicksPanel.h. All R1 engine wiring preserved:
 * bb_engine_sampler_set/clear/loaded, mute/solo/level/choke atomics, the
 * tri-state gate cycle, keys 1-8, and the quiet sync() with isUserDragging
 * guards. Geometry: toolbar 28, step header 24, 8 equal-flex slot rows (head
 * 300, 16-column StepCell grid with 3px padding, right gutter 140).
 *
 * LEGIBILITY PASS, the two things worth knowing:
 *   SCANNING. A 16-column grid of identical cells behind 16 identical 1px
 *   rules cannot be counted. Every fourth rule is now HAIRLINE against
 *   HAIRLINE_DIM for the rest, so the bar reads as four beats, and the
 *   playhead column is marked by fill + ink + a rule rather than by a tint
 *   you have to look for.
 *   TYPE. The PIT/VEL/LVL captions were 6px type set into a 6px box -- a face
 *   that clipped its own descenders, naming the controls under it. Every box
 *   in this file is now sized with Type::rowH(). */

#include "LicksPanel.h"
#include "AudioEngine.h"
#include "Session.h"
#include "bytebeat.h"
#include "engine.h"
#include "gen.h"

#include <cmath>
#include <cstdlib>          // calloc, below
#include <functional>
#include <memory>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

/* row geometry (spec section 7) */
static constexpr int kHeadW   = 300;   // row head / step-header left gutter
static constexpr int kGutW    = 140;   // right gutter (CHOKE + meter)
static constexpr int kToolbarH = 28;
static constexpr int kStepHdrH = 24;   // was 22: an 8px floor needs rowH(8)=13

/* Beat emphasis. Sixteen identical 1px column rules is a picket fence, and
 * counting to step 11 in it is the scanning problem the brief names. Every
 * fourth rule (the downbeat) is drawn at HAIRLINE, the rest at HAIRLINE_DIM
 * -- 2.48:1 against 1.84:1, so the bar divides into four readable groups
 * without adding a single new colour or a second pixel of width. */
static bool isBeatEdge (int stepIndexAfter) { return (stepIndexAfter % 4) == 0; }

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
        /* PLATE_LOW is a control face, not a text ground: Theme.h says use
         * INK_DIM there rather than the metadata inks. */
        g.setColour (grp ? C::OXIDE_INK : C::INK_DIM);
        g.setFont (Type::nano());
        g.drawText (grp ? "G" + juce::String (group) : U8 ("\xe2\x80\x94"),
                    getLocalBounds(), Justification::centred);
    }

private:
    int group = 0;
};
} // namespace

/* ======================================================================== */
/*  ToolTag -- toolbar action plate. There is now exactly one style,         */
/*  because there is now exactly one kind of tag: one that does something.   */
/*  The INERT style existed solely for the PATTERN B/C/D tags, which could   */
/*  not be clicked (mouseDown was guarded on the style) and named pattern    */
/*  banks the engine does not have. Tag and style are both gone.            */
/* ======================================================================== */
class LicksPanel::ToolTag : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    explicit ToolTag (const juce::String& t) : text (t)
    {
        setRepaintsOnMouseActivity (true);
    }

    std::function<void()> onClick;

    int idealWidth() const
    {
        return textW (Type::label(), text) + 18;   // padding 8 + 1px borders
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onClick)
            onClick();
    }

    void paint (juce::Graphics& g) override
    {
        const bool hov = isMouseOver();
        g.setColour (hov ? C::PLATE_HOVER : C::PLATE);
        g.fillRect (getLocalBounds());
        g.setColour (hov ? C::EDGE : C::HAIRLINE);
        g.drawRect (getLocalBounds(), 1);
        g.setColour (hov ? C::INK : C::INK_DIM);
        g.setFont (Type::label());
        g.drawText (text, getLocalBounds(), Justification::centred);
    }

private:
    juce::String text;
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

        /* head, right-anchored inside 300 with 8px padding (HTML gap 8).
         * The knob block is sized from Type::rowH() rather than a guessed
         * leading: the captions used to be 6px type set into a 6px box, which
         * clipped its own descenders, and the type floor is now 8px. */
        const int capH = Type::rowH (8.0f);                 // 13
        const int kx = kHeadW - 8 - (26 * 3 + 6 * 2);       // knob block: 3 faces, gap 6
        const int ky = (H - (26 + 2 + capH)) / 2;
        pit.setBounds (kx,      ky, 26, 26);
        vel.setBounds (kx + 32, ky, 26, 26);
        lvl.setBounds (kx + 64, ky, 26, 26);
        divX = kx - 9;                                      // 1px divider, 8px gaps
        const int sy = (H - 18) / 2;
        solo.setBounds (divX - 8 - 18, sy, 18, 18);
        mute.setBounds (divX - 8 - 38, sy, 18, 18);
        idxR  = { 8, 0, 18, H };
        const int nameH = Type::rowH (10.0f);               // 16
        const int metaH = Type::rowH (8.0f);                // 13
        nameR = { 34, (H - (nameH + metaH)) / 2, mute.getX() - 8 - 34, nameH };
        metaR = { 34, nameR.getBottom(), nameR.getWidth(), metaH };

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
        chokeLabW = textW (Type::nano(), "CHOKE");
        chokeLabR = { gx + 8, 0, chokeLabW, H };
        choke.setBounds (gx + 8 + chokeLabW + 6, (H - 16) / 2, 28, 16);
        meter.setBounds (W - 8 - 6, (H - 26) / 2, 6, 26);
    }

    void paint (juce::Graphics& g) override
    {
        const int W = getWidth(), H = getHeight();

        g.setColour (C::PANEL);
        g.fillRect (0, 0, W, H);

        /* The focused row's head is lifted a surface step. Focus used to be
         * carried by the index turning BLOOD_HOT and nothing else -- state by
         * colour alone, which Theme.h calls a bug -- and at 8 rows the one
         * red glyph was easy to lose. The fill is the primary cue now; the
         * accent on the index is the confirmation. */
        if (focused)
        {
            g.setColour (C::PANEL_ALT);
            g.fillRect (0, 0, kHeadW - 1, H);
        }

        /* rules: head border-right, gutter border-left, column separators,
         * row bottom (all 1px; spec rule 0.2) */
        g.setColour (C::HAIRLINE);
        g.fillRect (kHeadW - 1, 0, 1, H);
        g.fillRect (W - kGutW, 0, 1, H);
        g.fillRect (divX, (H - 24) / 2, 1, 24);
        const int gridW = W - kHeadW - kGutW;
        for (int st = 0; st < BB_STEPS; ++st)
        {
            g.setColour (isBeatEdge (st + 1) ? C::HAIRLINE : C::HAIRLINE_DIM);
            g.fillRect (colEdge (kHeadW, gridW, st + 1) - 1, 0, 1, H);
        }
        g.setColour (C::HAIRLINE);
        g.fillRect (0, H - 1, W, 1);

        /* index */
        g.setFont (Type::monoMedium (10.0f, 0.04f));
        g.setColour (focused ? C::BLOOD_HOT : C::INK_FAINT);
        g.drawText (juce::String (idx + 1).paddedLeft ('0', 2), idxR,
                    Justification::centredLeft);

        /* name + meta stacked */
        g.setFont (Type::monoMedium (10.0f, 0.04f));
        g.setColour (loaded ? C::INK : C::INK_FAINT);
        g.drawText (loaded ? name : U8 ("\xe2\x80\x94 EMPTY \xe2\x80\x94"),
                    nameR, Justification::centredLeft, true);
        g.setFont (Type::nano());
        g.setColour (loaded ? C::INK_FAINT : C::INK_GHOST);
        g.drawText (loaded ? meta : "DOUBLE-CLICK OR DRAG TO LOAD",
                    metaR, Justification::centredLeft, true);

        /* knob captions: 8px floor in a rowH(8) box (was 6px in a 6px box,
         * the worst size/colour/box combination in the app) */
        g.setFont (Type::nano());
        g.setColour (C::INK_FAINT);
        const int capH = Type::rowH (8.0f);
        g.drawText ("PIT", pit.getX(), pit.getBottom() + 2, 26, capH, Justification::centred);
        g.drawText ("VEL", vel.getX(), vel.getBottom() + 2, 26, capH, Justification::centred);
        g.drawText ("LVL", lvl.getX(), lvl.getBottom() + 2, 26, capH, Justification::centred);

        /* CHOKE label */
        g.setFont (Type::nano());
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

    /* The toolbar carries the three FILL actions and nothing else. There is
     * no PATTERN group: the engine holds one 16-step pattern per slot, so a
     * bank selector would have been a one-item choice next to three tags that
     * could not be clicked. */
    auto* euc = fillTags.add (new ToolTag ("EUCLID"));
    euc->setTooltip (U8 ("FILL EUCLID \xe2\x80\x94 spreads pulses evenly over the focused "
                         "slot's 16 steps. Each press adds one pulse, 1\xe2\x80\x93""16."));
    euc->onClick = [this] { fillEuclid(); };
    addAndMakeVisible (euc);

    auto* rnd = fillTags.add (new ToolTag ("RAND"));
    rnd->setTooltip (U8 ("FILL RAND \xe2\x80\x94 rolls a random gate pattern for the "
                         "focused slot. About 4 in 10 steps hit."));
    rnd->onClick = [this] { fillRand(); };
    addAndMakeVisible (rnd);

    auto* clr = fillTags.add (new ToolTag ("CLEAR"));
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
    const int tagH = Type::rowH (10.0f) + 4;                // 20
    const int tagY = toolbarRect.getY() + (kToolbarH - tagH) / 2;

    int x = toolbarRect.getX() + 10;
    fillLabelR = { x, toolbarRect.getY(), textW (Type::label(), "FILL"), kToolbarH };
    x = fillLabelR.getRight() + 10;
    for (auto* t : fillTags)
    {
        t->setBounds (x, tagY, t->idealWidth(), tagH);
        x += t->idealWidth() + 4;
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

    /* toolbar 28: FILL label + the three action tags (children) + hints */
    g.setColour (C::PANEL_ALT);
    g.fillRect (toolbarRect);
    g.setColour (C::HAIRLINE);
    g.fillRect (toolbarRect.getX(), toolbarRect.getBottom() - 1,
                toolbarRect.getWidth(), 1);

    g.setFont (Type::label());
    g.setColour (C::INK_DIM);
    g.drawText ("FILL", fillLabelR, Justification::centredLeft);

    const juce::Font hintF = Type::micro();
    g.setFont (hintF);
    g.setColour (C::INK_FAINT);
    const juce::String h1 = U8 ("FILL ACTS ON THE FOCUSED SLOT");
    const juce::String h2 = U8 ("CHOKE GROUPS 1\xe2\x80\x93""4");
    const int w2 = textW (hintF, h2);
    const int w1 = textW (hintF, h1);
    const int x2 = toolbarRect.getRight() - 10 - w2;
    g.drawText (h2, x2, toolbarRect.getY(), w2, kToolbarH, Justification::centredLeft);
    g.drawText (h1, x2 - 16 - w1, toolbarRect.getY(), w1, kToolbarH,
                Justification::centredLeft);

    /* step header: 300 gutter, 16 numbered columns, 140 right gutter.
     *
     * The playhead column is the thing you track while playing, so it is
     * marked three ways at once and none of them is hue alone: the header
     * cell fills PLATE_HOVER (dL* 4.0 over PLATE), its number goes OXIDE_INK
     * at 10.46:1, and a 1px OXIDE_DIM rule runs along the bottom of the cell
     * pointing down the column. That matches the StepCell playhead tint
     * exactly, so the header and the grid read as one moving marker. */
    const int hy = stepHeaderRect.getY();
    const int gridW = getWidth() - kHeadW - kGutW;
    for (int st = 0; st < BB_STEPS; ++st)
    {
        const int l = colEdge (kHeadW, gridW, st);
        const int r = colEdge (kHeadW, gridW, st + 1);
        const bool here = (st == lastPlay);
        if (here)
        {
            g.setColour (C::PLATE_HOVER);
            g.fillRect (l, hy, r - l - 1, kStepHdrH - 1);
            g.setColour (C::OXIDE_DIM);
            g.fillRect (l, hy + kStepHdrH - 2, r - l - 1, 1);
        }
        g.setColour (isBeatEdge (st + 1) ? C::HAIRLINE : C::HAIRLINE_DIM);
        g.fillRect (r - 1, hy, 1, kStepHdrH);
        g.setFont (here ? Type::monoMedium (9.0f, 0.05f) : Type::micro());
        g.setColour (here ? C::OXIDE_INK : isBeatEdge (st) ? C::INK_DIM : C::INK_FAINT);
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
    /* Open on the console's own directory rather than the home folder: that
     * is where REC, GROW and the ARRANGE captures put things, so it is where
     * the player's specimens actually are. */
    chooser = std::make_unique<juce::FileChooser> (
        "Load a specimen",
        morgue::morgueDir().isDirectory()
            ? morgue::morgueDir()
            : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
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

/* An internal drag carries a path in its description. The guard has to be
 * "is this an absolute path" and not "does it start with a slash": a Windows
 * path begins with a drive letter, or with the doubled separator of a UNC
 * share, so the old test was false for every real file on the platform and
 * the LOCKER could never be dragged onto a slot at all. On the Mac it was
 * merely redundant. juce::File::isAbsolutePath knows the rules for
 * whichever platform it was compiled for, and the check still matters --
 * juce::File's constructor asserts on a relative path, so a description that
 * is not one must be rejected before it reaches it. */
static bool isDroppableFile (const juce::String& p)
{
    return p.isNotEmpty() && juce::File::isAbsolutePath (p)
        && juce::File (p).existsAsFile();
}

bool LicksPanel::isInterestedInDragSource (const SourceDetails& d)
{
    const juce::String p = d.description.toString();
    return isAudioPath (p) && isDroppableFile (p);
}

void LicksPanel::itemDropped (const SourceDetails& d)
{
    const juce::String p = d.description.toString();
    if (! isDroppableFile (p)) return;
    const int s = slotAtY (d.localPosition.y);
    focusSlot (s);
    loadPath (s, juce::File (p));
}

} // namespace morgue
