/* SurvivorPanel.cpp -- see SurvivorPanel.h.
 *
 * Layout, top to bottom:
 *   header band            24
 *   BANK label row         20
 *   LANE STRIP            220   6 rows x 34 under a 16px column caption row
 *   GLOBAL row             28   CYCLE / ACTIVE / the HARD hint / ALL STOP+CLEAR
 *   DETAIL label row       20
 *   DETAIL transport       74   ARM PLAY STOP (132x74) | CLEAR + ->ARR | STATE
 *   DETAIL controls        30   SRC chip · COMMIT LANE · LENGTH chips
 *   LOOP BUFFER label      20
 *   LOOP BUFFER          flex   SOCKET box: envelope, slice grid, playhead
 *   LOOP CONTROL label     20
 *   knob row              123   six 76px EngravedKnobs
 *
 * Every read of the engine happens in sync(). paint() reads `snap` and the
 * envelope cache and nothing else.
 */

#include "SurvivorPanel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    const int   sliceTable[5] = { 1, 2, 4, 8, 16 };
    const char* const sliceTxt[5] = { "1/1", "1/2", "1/4", "1/8", "1/16" };

    int sliceIndex (int s)
    {
        for (int i = 0; i < 5; ++i) if (sliceTable[i] == s) return i;
        return 0;
    }

    /* "88 200" -- thousands grouped with a space, per the HTML data block. */
    juce::String groupedInt (unsigned v)
    {
        juce::String s ((juce::int64) v);
        for (int i = s.length() - 3; i > 0; i -= 3)
            s = s.substring (0, i) + " " + s.substring (i);
        return s;
    }

    juce::String barsWord (int bars)
    {
        return juce::String (bars) + (bars == 1 ? " BAR" : " BARS");
    }

    /* The CURRENT bar length in frames, from the same three published values
     * the engine derives it from. Used only to notice that the tempo has
     * moved since a loop was captured -- nothing is resampled. */
    unsigned barLenFramesNow()
    {
        int rate  = atomic_load (&bb.rate);              if (rate  < 1) rate  = 44100;
        int bpm   = atomic_load (&bb.gctl[GCTL_BPM]);    if (bpm   < 1) bpm   = 90;
        int beats = atomic_load (&bb.gctl[GCTL_BEATS]);  if (beats < 1) beats = 4;
        unsigned beatLen = (unsigned) (((juce::int64) rate * 60LL) / bpm);
        if (beatLen < 1u) beatLen = 1u;
        return beatLen * (unsigned) beats;
    }

    /* A flat well with a fill from the left. Used for the level cell, the
     * capture-progress bar and the peak column -- three different real
     * numbers, one drawing, no gradients. */
    void paintWell (juce::Graphics& g, Rectangle<int> r, float frac,
                    juce::Colour fill, juce::Colour border = C::HAIRLINE)
    {
        if (r.getWidth() <= 0 || r.getHeight() <= 0) return;
        g.setColour (C::TROUGH);
        g.fillRect (r);
        const int w = (int) (juce::jlimit (0.0f, 1.0f, frac) * (float) (r.getWidth() - 2));
        if (w > 0)
        {
            g.setColour (fill);
            g.fillRect (r.getX() + 1, r.getY() + 1, w, r.getHeight() - 2);
        }
        g.setColour (border);
        g.drawRect (r, 1);
    }
} // namespace

/* ======================================================================== */
/*  shared vocabulary                                                        */
/* ======================================================================== */

juce::String SurvivorPanel::slotTag (int slot)
{
    return slot == 0 ? juce::String ("MASTER") : "L" + juce::String (slot);
}

juce::String SurvivorPanel::srcName (int src)
{
    if (src >= 0 && src < BB_NLAYER)  return "V" + juce::String (src + 1).paddedLeft ('0', 2);
    if (src == BB_LOOP_SRC_LICKS)     return "LICKS";
    if (src == BB_LOOP_SRC_DRY)       return "DRY";
    if (src == BB_LOOP_SRC_LIVE)      return "LIVE";
    if (src == BB_LOOP_SRC_MASTER)    return "MASTER";
    return "?";
}

juce::String SurvivorPanel::srcBlurb (int src)
{
    if (src >= 0 && src < BB_NLAYER)
        return "ONE VOICE, POST-FADER";
    if (src == BB_LOOP_SRC_LICKS)  return "THE SAMPLER BUS";
    if (src == BB_LOOP_SRC_DRY)    return "VOICES + SAMPLER, PRE-RETURN";
    if (src == BB_LOOP_SRC_LIVE)   return U8 ("EVERYTHING YOU PLAY \xc2\xb7 NO LOOPER, EVER");
    if (src == BB_LOOP_SRC_MASTER) return U8 ("THE WHOLE BUS \xc2\xb7 INCLUDING THE OTHER LOOPERS");
    return "";
}

juce::String SurvivorPanel::laneName (int lane)
{
    if (lane >= 0 && lane < BB_NLAYER) return "V" + juce::String (lane + 1).paddedLeft ('0', 2);
    if (lane == BB_NLAYER)     return "LICKS";
    if (lane == BB_NLAYER + 1) return "MASS";
    return "?";
}

juce::String SurvivorPanel::actionWord (int action)
{
    switch (action & LBC_ACTION)
    {
        case LBC_ARM:   return "ARM";
        case LBC_PLAY:  return "PLAY";
        case LBC_STOP:  return "STOP";
        case LBC_CLEAR: return "CLEAR";
        default:        return {};
    }
}

/* The state signal. Five words, three fills, three lamp luminances -- see the
 * table in the header. Nothing here is carried by hue alone. */
SurvivorPanel::StateVis SurvivorPanel::stateVis (const SlotSnap& s)
{
    StateVis v;
    switch (s.status)
    {
        case LOOP_ARMED:
            v.word = "ARMED";  v.ink = C::AMBER;      v.lamp = C::AMBER;
            v.fill = C::PANEL_ALT;                    break;

        case LOOP_RECORDING:
            v.word = "REC";    v.ink = C::INK_BRIGHT; v.lamp = C::BLOOD_HOT;
            v.fill = C::BLOOD_DEEP;                   break;

        case LOOP_PLAYING:
            if (s.mute)
            {
                v.word = "MUTED";   v.ink = C::INK_GHOST; v.lamp = C::LAMP_DEAD;
            }
            else if (s.overdub)
            {
                v.word = "OVERDUB"; v.ink = C::BLOOD_HOT; v.lamp = C::BLOOD_HOT;
            }
            else
            {
                v.word = "PLAY";    v.ink = C::INK;       v.lamp = C::LAMP_SOUNDING;
            }
            v.fill = C::PANEL_ALT;
            break;

        default:
            if (s.frames > 0) { v.word = "HELD";  v.ink = C::INK_DIM;   }
            else              { v.word = "EMPTY"; v.ink = C::INK_GHOST; }
            v.lamp = C::LAMP_DEAD;
            v.fill = C::PANEL;
            break;
    }
    return v;
}

bool SurvivorPanel::canCommit (int slot) const
{
    const SlotSnap& s = snap.s[slot];
    return s.frames > 0 && s.status != LOOP_ARMED && s.status != LOOP_RECORDING;
}

bool SurvivorPanel::drifted (int slot) const
{
    const SlotSnap& s = snap.s[slot];
    return s.frames > 0 && s.barlen > 0 && snap.barLen > 0
        && std::abs ((int) snap.barLen - s.barlen) > 1;
}

/* Fraction through the current CYCLE, or -1 when it cannot be known.
 *
 * The engine publishes the cycle LENGTH but not its origin, so the only
 * honest source for "how far through the cycle are we" is a layer that is
 * actually playing one: a satellite whose recorded length IS the cycle has
 * its playhead at exactly that position. With no such layer the queue chip
 * draws its word and no progress -- it does not guess. */
float SurvivorPanel::cycleFrac() const
{
    const int cyc = snap.cycleBars;
    if (cyc <= 0) return -1.0f;
    for (int n = 1; n < BB_NLOOP; ++n)
    {
        const SlotSnap& s = snap.s[n];
        if (s.status != LOOP_PLAYING || s.frames == 0 || s.barlen <= 0) continue;
        if (s.heldBars() != cyc) continue;
        return juce::jlimit (0.0f, 1.0f, (float) s.pos / (float) s.frames);
    }
    return -1.0f;
}

/* ======================================================================== */
/*  construction                                                             */
/* ======================================================================== */

SurvivorPanel::SurvivorPanel()
{
    strip = std::make_unique<LaneStrip> (*this);
    addAndMakeVisible (*strip);

    /* ---- global row ---------------------------------------------------- */
    allStopBtn.setTooltip (U8 ("ALL STOP \xe2\x80\x94 hard stop every looper, at the "
                               "next period. Buffers are kept."));
    allStopBtn.onToggle = [this] (bool)
    {
        bb_engine_loop_panic();
        setNote ("ALL STOP -- SIX LOOPERS SILENCED, BUFFERS KEPT", C::AMBER);
    };
    addAndMakeVisible (allStopBtn);

    allClearBtn.setTooltip (U8 ("ALL CLEAR \xe2\x80\x94 wipe every looper. Hard: it does "
                                "not wait for a boundary."));
    allClearBtn.onToggle = [this] (bool)
    {
        for (int n = 0; n < BB_NLOOP; ++n)
            bb_engine_loop_cmd (n, LBC_CLEAR | LBC_HARD);
        setNote ("ALL CLEAR -- SIX LOOPERS WIPED", C::AMBER);
    };
    addAndMakeVisible (allClearBtn);

    /* ---- detail transport ---------------------------------------------- */
    armBtn.setStencilText (true);
    playBtn.setStencilText (true);
    stopBtn.setStencilText (true);
    armBtn.onFire   = [this] (bool hard) { fire (focusSlot, LBC_ARM,   hard); };
    playBtn.onFire  = [this] (bool hard) { fire (focusSlot, LBC_PLAY,  hard); };
    stopBtn.onFire  = [this] (bool hard) { fire (focusSlot, LBC_STOP,  hard); };
    clearBtn.onFire = [this] (bool hard) { fire (focusSlot, LBC_CLEAR, hard); };
    addAndMakeVisible (armBtn);
    addAndMakeVisible (playBtn);
    addAndMakeVisible (stopBtn);
    addAndMakeVisible (clearBtn);

    commitBtn.onToggle = [this] (bool) { commit (focusSlot); };
    addAndMakeVisible (commitBtn);

    srcBtn.onToggle = [this] (bool) { srcMenu (focusSlot, &srcBtn); };
    addAndMakeVisible (srcBtn);

    for (int i = 0; i < 5; ++i)
    {
        auto* b = new PlateButton ("--", false, false);
        b->onToggle = [this, i] (bool)
        {
            bb_engine_loop_ctl (focusSlot, L2C_BARS, lenVal[i]);
            refreshDetail();
            repaint();
        };
        addAndMakeVisible (b);
        lenBtns.add (b);
    }

    /* ---- knobs ---------------------------------------------------------- */
    auto mk = [this] (const juce::String& nm, int lo, int hi, int def,
                      const juce::String& sub, const juce::String& tip)
    {
        auto* k = new EngravedKnob (nm, 76, lo, hi, def);
        k->setSubLabel (sub);
        k->setTooltip (tip);
        addAndMakeVisible (k);
        knobs.add (k);
        return k;
    };
    mk ("LEVEL", 0, 256, 200, "INTO MASTER",
        U8 ("LEVEL \xe2\x80\x94 this looper into the master bus. 0\xe2\x80\x93""256. "
            "On MASTER this knob is MIX, a dry/loop crossfade."))
        ->onChange = [this] (int v) { bb_engine_loop_ctl (focusSlot, L2C_LEVEL, v); };

    mk ("FB", 0, 256, 160, "RETENTION",
        U8 ("FB \xe2\x80\x94 how much of the loop survives each overdub pass. "
            "0\xe2\x80\x93""256. Unity accretes offset; 160 is the default for a reason."))
        ->onChange = [this] (int v) { bb_engine_loop_ctl (focusSlot, L2C_FEEDBACK, v); };

    mk ("OD", 0, 1, 0, "OVERDUB",
        U8 ("OD \xe2\x80\x94 write the source into the loop while it plays. OFF/ON. "
            "Committing to ARRANGE turns this off."))
        ->onChange = [this] (int v) { bb_engine_loop_ctl (focusSlot, L2C_OVERDUB, v); };

    mk ("HALF", 0, 2, LOOP_RATE_NORMAL, U8 ("\xc2\xbd / 1\xc3\x97 / 2\xc3\x97"),
        U8 ("HALF \xe2\x80\x94 loop playback rate. \xc2\xbd, 1\xc3\x97, 2\xc3\x97."))
        ->onChange = [this] (int v) { bb_engine_loop_ctl (focusSlot, L2C_RATE, v); };

    mk ("REV", 0, 1, 0, "REVERSE",
        U8 ("REV \xe2\x80\x94 play the loop backwards. OFF/ON."))
        ->onChange = [this] (int v) { bb_engine_loop_ctl (focusSlot, L2C_REVERSE, v); };

    mk ("SLICE", 0, 4, 0, "STUTTER",
        U8 ("SLICE \xe2\x80\x94 stutter grid, repeats a fraction of the phrase. "
            "1/1\xe2\x80\x93""1/16."))
        ->onChange = [this] (int v)
        { bb_engine_loop_ctl (focusSlot, L2C_SLICE, sliceTable[juce::jlimit (0, 4, v)]); };

    for (int n = 0; n < BB_NLOOP; ++n)
    {
        recFrom[n]    = -1.0f;
        lastStatus[n] = LOOP_OFF;
        env[n].mag.assign ((size_t) kEnvCols, 0.0f);
    }

    refreshDetail();
}

/* ======================================================================== */
/*  geometry                                                                 */
/* ======================================================================== */

SurvivorPanel::Geom SurvivorPanel::geom() const
{
    Geom g;
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);

    g.bankLabel = b.removeFromTop (20);
    g.bank      = b.removeFromTop (juce::jmin (LaneStrip::idealHeight(),
                                               juce::jmax (0, b.getHeight() - 120)));
    g.global      = b.removeFromTop (28);
    g.detailLabel = b.removeFromTop (20);
    g.detailTop   = b.removeFromTop (juce::jmin (74, juce::jmax (0, b.getHeight())));
    g.detailCtl   = b.removeFromTop (juce::jmin (30, juce::jmax (0, b.getHeight())));

    const int kIdeal = knobs.isEmpty() ? 123 : knobs[0]->idealHeight();
    const int kh = juce::jlimit (0, juce::jmax (0, b.getHeight() - 60), kIdeal);
    g.knobRow   = b.removeFromBottom (kh);
    g.knobLabel = b.removeFromBottom (juce::jmin (20, juce::jmax (0, b.getHeight())));

    g.waveLabel = b.removeFromTop (juce::jmin (20, juce::jmax (0, b.getHeight())));
    g.wave      = b;
    return g;
}

void SurvivorPanel::resized()
{
    const Geom g = geom();

    strip->setBounds (g.bank);

    /* global row: readouts painted from the left, two plates at the right */
    Rectangle<int> gl = g.global.reduced (10, 3);
    allClearBtn.setBounds (gl.removeFromRight (86).withSizeKeepingCentre (86, 20));
    gl.removeFromRight (6);
    allStopBtn.setBounds  (gl.removeFromRight (86).withSizeKeepingCentre (86, 20));

    /* detail transport: three stencil plates, then CLEAR / ->ARR stacked */
    Rectangle<int> dt = g.detailTop;
    const int by = dt.getY() + juce::jmax (0, (dt.getHeight() - 74) / 2);
    const int bw = 132, bh = 74;
    armBtn.setBounds  (dt.getX() + 16,                 by, bw, bh);
    playBtn.setBounds (dt.getX() + 16 + (bw + 10),     by, bw, bh);
    stopBtn.setBounds (dt.getX() + 16 + 2 * (bw + 10), by, bw, bh);

    const int sx = dt.getX() + 16 + 3 * (bw + 10);
    clearBtn.setBounds  (sx, by,      104, 34);
    commitBtn.setBounds (sx, by + 40, 104, 34);

    /* detail controls: SRC chip, then the LENGTH chips (LANE is a readout
     * with its own right-click menu on the strip, drawn in paintDetail) */
    Rectangle<int> dc = g.detailCtl.reduced (16, 4);
    srcBtn.setBounds (dc.removeFromLeft (juce::jmin (128, dc.getWidth())));
    dc.removeFromLeft (10);
    dc.removeFromLeft (juce::jmin (150, dc.getWidth()));      // LANE readout
    dc.removeFromLeft (juce::jmin (58,  dc.getWidth()));      // "LENGTH" caption
    for (auto* b : lenBtns)
    {
        b->setBounds (dc.removeFromLeft (juce::jmin (66, dc.getWidth())));
        dc.removeFromLeft (4);
    }

    /* knob row: six equal cells, knob column centred in each */
    Rectangle<int> field = g.knobRow.reduced (20, 0);
    const int n = knobs.size();
    for (int i = 0; i < n && field.getHeight() > 0; ++i)
    {
        const int x0 = field.getX() + i       * field.getWidth() / n;
        const int x1 = field.getX() + (i + 1) * field.getWidth() / n;
        const int kh = juce::jmin (knobs[i]->idealHeight(), field.getHeight());
        const int kw = juce::jmin (x1 - x0, 160);
        knobs[i]->setBounds (x0 + (x1 - x0 - kw) / 2,
                             field.getY() + (field.getHeight() - kh) / 2, kw, kh);
    }
}

/* ======================================================================== */
/*  the 30 Hz pull                                                           */
/* ======================================================================== */

void SurvivorPanel::sync()
{
    Snap n;
    n.cycleBars = bb_engine_loop_cycle_bars();
    n.active    = atomic_load (&bb.loop_active);
    n.rate      = atomic_load (&bb.rate);
    if (n.rate < 1) n.rate = 44100;
    n.barLen    = barLenFramesNow();

    const float pos = transportPositionBars();
    if (pos >= 0.0f)
    {
        n.bar  = (unsigned) pos;
        n.barF = pos - std::floor (pos);
    }

    for (int i = 0; i < BB_NLOOP; ++i)
    {
        SlotSnap& s = n.s[i];
        s.status   = bb_engine_loop_status (i);
        s.pending  = bb_engine_loop_pending (i);
        s.bars     = juce::jmax (0, bb_engine_loop_ctl_get (i, L2C_BARS));
        s.level    = juce::jlimit (0, 256, bb_engine_loop_ctl_get (i, L2C_LEVEL));
        s.feedback = juce::jlimit (0, 256, bb_engine_loop_ctl_get (i, L2C_FEEDBACK));
        s.overdub  = bb_engine_loop_ctl_get (i, L2C_OVERDUB) > 0;
        s.rate     = juce::jlimit (0, 2, bb_engine_loop_ctl_get (i, L2C_RATE));
        s.reverse  = bb_engine_loop_ctl_get (i, L2C_REVERSE) > 0;
        s.slice    = sliceTable[sliceIndex (bb_engine_loop_ctl_get (i, L2C_SLICE))];
        s.lane     = juce::jlimit (0, ARR_LANES - 1, bb_engine_loop_ctl_get (i, L2C_LANE));
        s.frames   = bb_engine_loop_frames (i);
        s.pos      = bb_engine_loop_pos (i);
        s.barlen   = bb_engine_loop_barlen (i);

        /* SRC and MUTE are the two controls slot 0 refuses. The engine
         * documents the refusal twice and not identically -- as a MASTER
         * sentinel in one place and as -1 in the other -- so the panel takes
         * either and draws the truth: MASTER records everything, and it has
         * no mute (MIX at 0 is its silence). */
        const int rawSrc = bb_engine_loop_ctl_get (i, L2C_SRC);
        s.src = (rawSrc < 0 || rawSrc >= BB_LOOP_NSRC)
                    ? (i == 0 ? BB_LOOP_SRC_MASTER : BB_LOOP_SRC_LIVE) : rawSrc;
        s.mute = (i == 0) ? 0 : (bb_engine_loop_ctl_get (i, L2C_MUTE) > 0);

        /* slot 0 has no per-slot barlen (its length is bars x the bar it was
         * captured at); fall back to the current bar so heldBars() still
         * reports something true for the master phrase looper. */
        if (i == 0 && s.barlen <= 0 && n.barLen > 0)
            s.barlen = (int) n.barLen;

        /* peak: read-and-clear, exactly once, then decayed here */
        const float pk = (float) juce::jlimit (0, 32767, bb_engine_loop_peak (i)) / 32768.0f;
        peakUi[i] = juce::jmax (pk, peakUi[i] * 0.80f);
        if (peakUi[i] < 0.0005f) peakUi[i] = 0.0f;

        /* capture progress: latch the transport position on the real edge */
        if (s.status == LOOP_RECORDING && lastStatus[i] != LOOP_RECORDING)
        {
            recFrom[i] = pos;
            recBars[i] = (i == 0) ? juce::jlimit (1, 4, s.bars)
                       : (s.bars > 0 ? s.bars
                                     : (n.cycleBars > 0 ? n.cycleBars : BB_LOOP_DEF_BARS));
        }
        if (s.status != LOOP_RECORDING)
            recFrom[i] = -1.0f;
        lastStatus[i] = s.status;
    }

    const bool moving = [&]
    {
        for (int i = 0; i < BB_NLOOP; ++i)
            if (n.s[i].status == LOOP_PLAYING || n.s[i].status == LOOP_RECORDING
                || n.s[i].status == LOOP_ARMED || n.s[i].pending != 0)
                return true;
        return false;
    }();

    const bool changed = std::memcmp (&n, &snap, sizeof (Snap)) != 0;
    snap = n;

    /* envelopes: rebuild when the recorded length changed, and at ~4 Hz while
     * a slot is being written into. paint() never walks a loop buffer. */
    ++envTick;
    for (int i = 0; i < BB_NLOOP; ++i)
    {
        const SlotSnap& s = snap.s[i];
        const bool writing = s.status == LOOP_RECORDING
                          || (s.status == LOOP_PLAYING && s.overdub);
        if (s.frames != env[i].frames || (writing && (envTick % 8) == 0))
            rebuildEnv (i);
    }

    bool noteExpired = false;
    if (noteTicks > 0 && --noteTicks == 0)
    {
        note.clear();
        noteExpired = true;
    }

    refreshDetail();

    if (moving || changed || noteExpired)
        repaint();
}

/* Push the snapshot into the detail children. Dragged controls are skipped;
 * everything else is overwritten from the engine every frame. */
void SurvivorPanel::refreshDetail()
{
    const int f = juce::jlimit (0, BB_NLOOP - 1, focusSlot);
    const SlotSnap& s = snap.s[f];

    TextKey key;
    key.slot    = f;
    key.status  = s.status;
    key.src     = s.src;
    key.bars    = s.bars;
    key.held    = s.heldBars();
    key.mute    = s.mute;
    key.overdub = s.overdub;
    key.commit  = (canCommit (f) && commitToArrange != nullptr) ? 1 : 0;
    if (std::memcmp (&key, &builtFor, sizeof (TextKey)) != 0)
    {
        builtFor = key;
        rebuildDetailText (f);
    }

    armBtn.setToggleStateQuiet  (s.status == LOOP_ARMED || s.status == LOOP_RECORDING);
    playBtn.setToggleStateQuiet (s.status == LOOP_PLAYING);

    for (int i = 0; i < lenBtns.size(); ++i)
        lenBtns[i]->setToggleStateQuiet (lenBtns[i]->isVisible() && s.bars == lenVal[i]);

    if (! knobs[0]->isUserDragging()) knobs[0]->setValueQuiet (s.level);
    if (! knobs[1]->isUserDragging()) knobs[1]->setValueQuiet (s.feedback);
    if (! knobs[2]->isUserDragging()) knobs[2]->setValueQuiet (s.overdub);
    if (! knobs[3]->isUserDragging()) knobs[3]->setValueQuiet (s.rate);
    if (! knobs[4]->isUserDragging()) knobs[4]->setValueQuiet (s.reverse);
    if (! knobs[5]->isUserDragging()) knobs[5]->setValueQuiet (sliceIndex (s.slice));

    knobs[2]->setValueText (knobs[2]->value() ? "ON" : "OFF");
    static const char* const half[3] = { "\xc2\xbd", "NORM", "2\xc3\x97" };
    knobs[3]->setValueText (juce::String::fromUTF8 (half[juce::jlimit (0, 2, knobs[3]->value())]));
    knobs[4]->setValueText (knobs[4]->value() ? "ON" : "OFF");
    knobs[5]->setValueText (sliceTxt[juce::jlimit (0, 4, knobs[5]->value())]);
}

/* The strings. Rebuilt only when what they describe changed -- see the note
 * on refreshDetail() in the header. */
void SurvivorPanel::rebuildDetailText (int f)
{
    const SlotSnap& s = snap.s[f];
    const bool master = (f == 0);
    const bool commitable = canCommit (f) && commitToArrange != nullptr;

    armBtn.setSubLine  (s.status == LOOP_ARMED     ? "WAITING"
                      : s.status == LOOP_RECORDING ? "CAPTURING"
                      : master                     ? "NEXT BAR" : "NEXT CYCLE");
    armBtn.setTooltip (master
        ? U8 ("ARM \xe2\x80\x94 capture the whole bus into the master phrase looper, "
              "starting at the next bar boundary.")
        : U8 ("ARM \xe2\x80\x94 destructive re-take. The capture starts on the CYCLE so "
              "every layer shares a downbeat; HARD is ignored on ARM, because a loop "
              "that is not a whole number of bars is what makes a committed clip "
              "drift against the grid."));

    playBtn.setSubLine (s.status == LOOP_PLAYING
                        ? (s.mute ? "MUTED" : s.overdub ? "OVERDUBBING" : "LOOPING")
                        : (s.frames > 0 ? "HOLDS " + barsWord (s.heldBars()) : "EMPTY"));
    playBtn.setTooltip (U8 ("PLAY \xe2\x80\x94 lands on the next bar and RE-PHASES the loop "
                            "to the transport, so a layer brought back mid-cycle drops in "
                            "where it belongs. Right-click or ") + modKeyWord()
                        + U8 ("-click = HARD."));

    stopBtn.setSubLine (master ? "AT ONCE" : "NEXT BAR");
    stopBtn.setTooltip (U8 ("STOP \xe2\x80\x94 silence this looper and keep its buffer. "
                            "Lands on the next bar; right-click or ") + modKeyWord()
                        + U8 ("-click for a hard stop."));

    clearBtn.setSubLine (s.frames > 0 ? "WIPE " + barsWord (s.heldBars()) : "EMPTY");
    clearBtn.setTooltip (U8 ("CLEAR \xe2\x80\x94 wipe the buffer. Lands on the cycle; "
                             "right-click or ") + modKeyWord() + U8 ("-click to wipe now."));

    commitBtn.setButtonText (U8 ("\xe2\x86\x92 ARR"));
    commitBtn.setEnabled (commitable);
    commitBtn.setSubLine (commitable ? "FREEZES OD" : "NOTHING YET");
    commitBtn.setTooltip (commitable
        ? U8 ("\xe2\x86\x92 ARR \xe2\x80\x94 place this loop on ARRANGE lane ")
            + laneName (s.lane) + U8 (" as a ") + barsWord (s.heldBars())
            + U8 (" clip. It FREEZES the loop: the slot's overdub is turned off so the "
                  "buffer can be copied. Right-click the row's \xe2\x86\x92" "ARR cell to "
                  "choose the lane.")
        : U8 ("\xe2\x86\x92 ARR \xe2\x80\x94 needs a finished loop and a wired ARRANGE: not "
              "while the slot is armed, recording or empty."));

    srcBtn.setEnabled (! master);
    srcBtn.setButtonText ("SRC: " + srcName (s.src));
    srcBtn.setTooltip (master
        ? U8 ("SOURCE \xe2\x80\x94 MASTER is pinned: the phrase looper bounces the whole "
              "bus, the other five loopers included. That pin is what keeps a session "
              "using only this slot bit-identical to the engine before the bank.")
        : U8 ("SOURCE \xe2\x80\x94 what this looper records: ") + srcBlurb (s.src)
            + U8 (". Click to change. GRAIN MASS is inside the master bus now, so LIVE "
                  "and DRY both carry the wells."));

    /* length chips: slot 0 is 1..4 bars, a satellite is FOLLOW/1/2/4/8 */
    static const int satVals[5] = { 0, 1, 2, 4, 8 };
    for (int i = 0; i < lenBtns.size(); ++i)
    {
        PlateButton* b = lenBtns[i];
        if (master)
        {
            const bool used = i < 4;
            b->setVisible (used);
            lenVal[i] = used ? i + 1 : 1;
            if (used) b->setButtonText (juce::String (i + 1) + " BAR");
        }
        else
        {
            b->setVisible (true);
            lenVal[i] = satVals[i];
            b->setButtonText (i == 0 ? "FOLLOW" : juce::String (satVals[i]) + " BAR");
        }
        b->setTooltip (i == 0 && ! master
            ? U8 ("FOLLOW \xe2\x80\x94 take the length from the CYCLE, i.e. from the first "
                  "loop recorded this session. With no cycle yet: 4 bars, and that capture "
                  "sets the cycle.")
            : U8 ("LENGTH \xe2\x80\x94 capture this many whole bars. The engine refuses a "
                  "capture that is not a whole number of bars."));
    }

    knobs[0]->setLabelText (master ? "MIX" : "LEVEL");
    knobs[0]->setSubLabel  (master ? U8 ("DRY \xe2\x86\x94 LOOP") : "INTO MASTER");
    knobs[0]->setTooltip (master
        ? U8 ("MIX \xe2\x80\x94 dry vs loop crossfade on the master phrase looper. "
              "0\xe2\x80\x93""256.")
        : U8 ("LEVEL \xe2\x80\x94 this looper into the master bus. 0\xe2\x80\x93""256."));
}

/* ======================================================================== */
/*  commands                                                                 */
/* ======================================================================== */

void SurvivorPanel::setNote (const juce::String& s, juce::Colour ink)
{
    note = s;
    noteInk = ink;
    noteTicks = 90;                     // ~3 s at the 30 Hz sync
    repaint();
}

void SurvivorPanel::fire (int slot, int action, bool hard)
{
    slot = juce::jlimit (0, BB_NLOOP - 1, slot);
    bb_engine_loop_cmd (slot, action | (hard ? LBC_HARD : 0));

    /* Say what the engine will actually do with it, because the two quanta
     * are not the same and HARD is ignored on ARM. */
    juce::String when;
    if (slot == 0)
        when = (action == LBC_ARM) ? U8 (" \xc2\xb7 CAPTURES ON THE NEXT BAR")
                                   : U8 (" \xc2\xb7 AT ONCE");
    else if (action == LBC_ARM)
        when = U8 (" \xc2\xb7 ON THE CYCLE (HARD IS IGNORED ON ARM)");
    else if (hard)
        when = U8 (" \xc2\xb7 HARD, AT THE NEXT PERIOD");
    else
        when = (action == LBC_CLEAR) ? U8 (" \xc2\xb7 ON THE CYCLE")
                                     : U8 (" \xc2\xb7 ON THE NEXT BAR");

    setNote (slotTag (slot) + " " + actionWord (action) + when,
             action == LBC_CLEAR ? C::AMBER : C::INK_DIM);
    setFocus (slot);
}

void SurvivorPanel::commit (int slot)
{
    slot = juce::jlimit (0, BB_NLOOP - 1, slot);

    if (commitToArrange == nullptr)
    {
        setNote ("COMMIT REFUSED -- ARRANGE IS NOT WIRED", C::AMBER);
        return;
    }
    if (! canCommit (slot))
    {
        setNote (slotTag (slot) + " HAS NOTHING FINISHED TO COMMIT", C::AMBER);
        return;
    }

    unsigned bars = 0;
    ArrClipBuf* buf = bb_engine_loop_clip (slot, &bars);     // freezes overdub
    if (buf == nullptr || bars == 0)
    {
        if (buf != nullptr) bb_engine_clip_release (buf);
        setNote ("COMMIT REFUSED -- THE ENGINE WOULD NOT SNAPSHOT " + slotTag (slot),
                 C::AMBER);
        return;
    }

    const int lane = snap.s[slot].lane;
    if (commitToArrange (buf, bars, lane, slot))
        setNote (slotTag (slot) + U8 (" \xe2\x86\x92 ARRANGE LANE ") + laneName (lane)
                     + U8 (" \xc2\xb7 ") + barsWord ((int) bars)
                     + U8 (" \xc2\xb7 OVERDUB IS NOW OFF"),
                 C::GREEN_FAINT);
    else
    {
        bb_engine_clip_release (buf);   // ARRANGE did not take it
        setNote ("COMMIT REFUSED -- ARRANGE HAD NO ROOM FOR THE CLIP", C::AMBER);
    }
    refreshDetail();
}

void SurvivorPanel::setFocus (int slot)
{
    slot = juce::jlimit (0, BB_NLOOP - 1, slot);
    if (slot == focusSlot) return;
    focusSlot = slot;
    refreshDetail();
    repaint();
}

void SurvivorPanel::srcMenu (int slot, juce::Component* target)
{
    if (slot == 0)
    {
        setNote ("MASTER RECORDS THE WHOLE BUS -- ITS SOURCE IS PINNED", C::AMBER);
        return;
    }

    const int cur = snap.s[slot].src;
    juce::PopupMenu m;
    m.addSectionHeader (slotTag (slot) + " RECORDS");
    m.addItem (1 + BB_LOOP_SRC_LIVE,
               U8 ("LIVE  \xe2\x80\x94 EVERYTHING YOU PLAY, NO LOOPER"),
               true, cur == BB_LOOP_SRC_LIVE);
    m.addItem (1 + BB_LOOP_SRC_DRY,
               U8 ("DRY   \xe2\x80\x94 VOICES + SAMPLER, PRE-RETURN"),
               true, cur == BB_LOOP_SRC_DRY);
    m.addItem (1 + BB_LOOP_SRC_LICKS,
               U8 ("LICKS \xe2\x80\x94 THE SAMPLER BUS"),
               true, cur == BB_LOOP_SRC_LICKS);
    m.addSeparator();
    for (int L = 0; L < BB_NLAYER; ++L)
        m.addItem (1 + L, srcName (L) + U8 ("   \xe2\x80\x94 ONE VOICE, POST-FADER"),
                   true, cur == L);
    m.addSeparator();
    /* This used to read "GRAIN MASS CANNOT BE RECORDED / IT IS MIXED OUTSIDE
     * THE ENGINE". It no longer is: the wells sum beside the LICKS bus, so
     * they are already in LIVE and DRY and there is nothing to warn about.
     * They get no source of their own -- a well is not a performance layer
     * you would loop in isolation, and every id here is persisted as an
     * integer in the session's `loopn` line. */
    m.addSectionHeader ("GRAIN MASS IS IN LIVE AND DRY");

    juce::PopupMenu::Options opts = juce::PopupMenu::Options().withMinimumWidth (260);
    opts = target != nullptr ? opts.withTargetComponent (target) : opts.withMousePosition();

    juce::Component::SafePointer<SurvivorPanel> safe (this);
    m.showMenuAsync (opts, [safe, slot] (int result)
    {
        if (safe == nullptr || result <= 0) return;
        bb_engine_loop_ctl (slot, L2C_SRC, result - 1);
        safe->setFocus (slot);
        safe->setNote (slotTag (slot) + " RECORDS " + srcName (result - 1)
                           + U8 (" \xc2\xb7 ") + srcBlurb (result - 1),
                       C::INK_DIM);
    });
}

void SurvivorPanel::laneMenu (int slot, juce::Component* target)
{
    const int cur = snap.s[slot].lane;
    juce::PopupMenu m;
    m.addSectionHeader (slotTag (slot) + " COMMITS TO LANE");
    for (int l = 0; l < ARR_LANES; ++l)
        m.addItem (1 + l, laneName (l), true, cur == l);

    juce::PopupMenu::Options opts = juce::PopupMenu::Options().withMinimumWidth (160);
    opts = target != nullptr ? opts.withTargetComponent (target) : opts.withMousePosition();

    juce::Component::SafePointer<SurvivorPanel> safe (this);
    m.showMenuAsync (opts, [safe, slot] (int result)
    {
        if (safe == nullptr || result <= 0) return;
        bb_engine_loop_ctl (slot, L2C_LANE, result - 1);
        safe->setNote (slotTag (slot) + U8 (" COMMITS TO ARRANGE LANE ")
                           + laneName (result - 1), C::INK_DIM);
    });
}

/* ======================================================================== */
/*  waveform envelope                                                        */
/* ======================================================================== */

void SurvivorPanel::rebuildEnv (int slot)
{
    Env& e = env[slot];
    e.frames = snap.s[slot].frames;
    std::fill (e.mag.begin(), e.mag.end(), 0.0f);
    if (e.frames == 0) return;

    unsigned len = 0;
    const int16_t* buf = bb_engine_loop_slot_buffer (slot, &len);
    if (buf == nullptr || len == 0) { e.frames = 0; return; }

    const unsigned n = juce::jmin (e.frames, len);
    for (int c = 0; c < kEnvCols; ++c)
    {
        const unsigned a = (unsigned) ((juce::uint64) c       * n / (unsigned) kEnvCols);
        const unsigned b = (unsigned) ((juce::uint64) (c + 1) * n / (unsigned) kEnvCols);
        if (b <= a) continue;
        const unsigned step = juce::jmax (1u, (b - a) / 16u);   // <= 16 probes/column
        int peak = 0;
        for (unsigned i = a; i < b; i += step)
        {
            int s = (int) buf[i];
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
        e.mag[(size_t) c] = (float) peak / 32768.0f;
    }
}

void SurvivorPanel::drawEnv (juce::Graphics& g, Rectangle<int> box, int slot,
                             juce::Colour col) const
{
    const Env& e = env[slot];
    if (e.frames == 0 || box.getWidth() <= 0 || box.getHeight() <= 2) return;

    const int cy = box.getCentreY();
    const float half = (float) box.getHeight() / 2.0f - 1.0f;
    g.setColour (col);
    for (int x = 0; x < box.getWidth(); ++x)
    {
        const int c = juce::jlimit (0, kEnvCols - 1, x * kEnvCols / box.getWidth());
        const int h = juce::jmax (1, (int) (e.mag[(size_t) c] * half));
        g.fillRect (box.getX() + x, cy - h, 1, h * 2);
    }
}

/* ======================================================================== */
/*  paint                                                                    */
/* ======================================================================== */

void SurvivorPanel::paint (juce::Graphics& g)
{
    const Geom gm = geom();

    g.setColour (C::GROUND);
    g.fillRect (getLocalBounds());

    paintHeaderBand (g, getLocalBounds().removeFromTop (headerBandH),
                     "SURVIVOR",
                     U8 ("THE LOOP BANK \xc2\xb7 SIX BAR-SYNCED LOOPERS \xc2\xb7 "
                         "SLOT 0 IS THE MASTER PHRASE LOOPER"),
                     juce::String (SerialNo::SURVIVOR)
                         + U8 (" \xc2\xb7 SATELLITES RECORD LIVE: NO LOOPER IN THE SOURCE"),
                     Badge::LIVE, "LIVE");

    paintLabelRow (g, gm.bankLabel, "LOOP BANK",
                   U8 ("CLICK A ROW TO FOCUS \xc2\xb7 RIGHT-CLICK OR ")
                       + modKeyWord().toUpperCase() + U8 ("-CLICK A TRANSPORT CELL = HARD"));
    g.setColour (C::HAIRLINE);
    g.fillRect (gm.bankLabel.getX(), gm.bankLabel.getBottom() - 1,
                gm.bankLabel.getWidth(), 1);

    paintGlobal (g, gm);
    paintDetail (g, gm);
}

void SurvivorPanel::paintGlobal (juce::Graphics& g, const Geom& gm)
{
    Rectangle<int> r = gm.global;
    g.setColour (C::PANEL_ALT);
    g.fillRect (r);
    g.setColour (C::HAIRLINE);
    g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);

    Rectangle<int> t = r.reduced (10, 0).withTrimmedRight (190);

    /* CYCLE is the number that explains why a FOLLOW slot recorded the length
     * it did, so it is on screen whenever the bank is. */
    const int cyc = snap.cycleBars;
    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    g.drawText ("CYCLE", t.removeFromLeft (44), Justification::centredLeft);
    g.setColour (cyc > 0 ? C::INK : C::INK_GHOST);
    g.setFont (Type::label());
    g.drawText (cyc > 0 ? barsWord (cyc) : U8 ("\xe2\x80\x94 NOT SET"),
                t.removeFromLeft (78), Justification::centredLeft);

    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    g.drawText ("ACTIVE", t.removeFromLeft (52), Justification::centredLeft);
    g.setColour (snap.active > 0 ? C::INK : C::INK_GHOST);
    g.setFont (Type::label());
    g.drawText (juce::String (snap.active) + "/5", t.removeFromLeft (48),
                Justification::centredLeft);

    /* the transient line: refusals, commits, what a command is waiting for */
    if (note.isNotEmpty())
    {
        g.setColour (noteInk);
        g.setFont (Type::micro());
        g.drawText (note, t, Justification::centredLeft, true);
    }
    else
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::nano());
        g.drawText (U8 ("ARM AND CLEAR LAND ON THE CYCLE \xc2\xb7 PLAY AND STOP LAND ON "
                        "THE BAR \xc2\xb7 HARD LANDS AT ONCE"),
                    t, Justification::centredLeft, true);
    }
}

void SurvivorPanel::paintDetail (juce::Graphics& g, const Geom& gm)
{
    const int f = juce::jlimit (0, BB_NLOOP - 1, focusSlot);
    const SlotSnap& s = snap.s[f];
    const StateVis vis = stateVis (s);
    const bool master = (f == 0);

    paintLabelRow (g, gm.detailLabel,
                   "LOOPER " + slotTag (f),
                   master ? U8 ("BOUNCES THE WHOLE BUS \xc2\xb7 THE OTHER FIVE INCLUDED")
                          : U8 ("RECORDS ") + srcName (s.src) + U8 (" \xc2\xb7 ")
                                + srcBlurb (s.src));
    g.setColour (C::HAIRLINE);
    g.fillRect (gm.detailLabel.getX(), gm.detailLabel.getBottom() - 1,
                gm.detailLabel.getWidth(), 1);

    /* ---- transport band ------------------------------------------------- */
    Rectangle<int> row = gm.detailTop;
    g.setColour (C::PANEL_ALT);
    g.fillRect (row);

    const int dx = row.getX() + 16 + 3 * 142 + 104 + 14;
    g.setColour (C::HAIRLINE);
    g.fillRect (dx, row.getY() + 6, 1, juce::jmax (0, row.getHeight() - 12));

    /* LOOP OUT: the focused slot's real peak, read-and-clear, decayed here */
    const int meterW = 140;
    const int meterX = row.getRight() - 16 - meterW;
    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    g.drawText ("LOOP OUT",
                Rectangle<int> (meterX, row.getCentreY() - 11 - Type::rowH (9.0f),
                                meterW, Type::rowH (9.0f)),
                Justification::centredRight);
    paintWell (g, Rectangle<int> (meterX, row.getCentreY() + 3, meterW, 8),
               peakUi[f], peakUi[f] > 0.5f ? C::AMBER : C::BLOOD);

    /* state block: the word is the signal, the lamp only confirms it */
    const int blockX = dx + 16;
    const int blockW = juce::jmax (0, meterX - 16 - blockX);
    Rectangle<int> block (blockX, row.getY() + juce::jmax (0, (row.getHeight() - 68) / 2),
                          blockW, juce::jmin (68, row.getHeight()));

    Rectangle<int> wordRow = block.removeFromTop (juce::jmin (Type::rowH (26.0f),
                                                              block.getHeight()));
    g.setColour (vis.lamp);
    g.fillRect (wordRow.getX(), wordRow.getCentreY() - 5, 10, 10);
    g.setColour (vis.ink);
    g.setFont (Type::stencil (26.0f, 0.14f));
    g.drawText (vis.word, wordRow.withTrimmedLeft (18), Justification::centredLeft);

    /* line 1: what the buffer holds, or what the capture is doing */
    juce::String detail;
    if (s.status == LOOP_RECORDING)
    {
        const int tgt = juce::jmax (1, recBars[f]);
        const float done = (recFrom[f] >= 0.0f && snap.barF >= 0.0f)
            ? juce::jlimit (0.0f, (float) tgt,
                            ((float) snap.bar + snap.barF) - recFrom[f])
            : 0.0f;
        detail = "CAPTURING " + juce::String (done, 2) + " / " + barsWord (tgt);
    }
    else if (s.status == LOOP_ARMED)
    {
        const int tgt = master ? juce::jlimit (1, 4, s.bars)
                      : (s.bars > 0 ? s.bars
                                    : (snap.cycleBars > 0 ? snap.cycleBars : BB_LOOP_DEF_BARS));
        detail = "WILL CAPTURE " + barsWord (tgt)
               + (master ? U8 (" \xc2\xb7 STARTS ON THE NEXT BAR")
                         : (s.bars > 0 ? U8 (" \xc2\xb7 STARTS ON THE CYCLE")
                                       : U8 (" \xc2\xb7 FOLLOWING THE CYCLE")));
    }
    else if (s.frames > 0)
    {
        detail = barsWord (s.heldBars()) + U8 (" \xc2\xb7 ")
               + juce::String ((double) s.frames / (double) snap.rate, 3) + " s"
               + U8 (" \xc2\xb7 ") + groupedInt (s.frames) + " SMP";
    }
    else
        detail = master ? U8 ("BUFFER EMPTY \xc2\xb7 ARM SNAPS TO THE NEXT BAR")
                        : U8 ("BUFFER EMPTY \xc2\xb7 ARM SNAPS TO THE CYCLE");

    g.setColour (C::INK_DIM);
    g.setFont (Type::micro());
    g.drawText (detail, block.removeFromTop (juce::jmin (Type::rowH (9.0f), block.getHeight()))
                            .withTrimmedLeft (18),
                Justification::centredLeft, true);

    /* line 2: the tempo-drift warning, or the routing summary */
    Rectangle<int> l2 = block.removeFromTop (juce::jmin (Type::rowH (8.0f), block.getHeight()))
                             .withTrimmedLeft (18);
    if (drifted (f))
    {
        g.setColour (C::AMBER);
        g.setFont (Type::nano());
        g.drawText (U8 ("TEMPO MOVED SINCE CAPTURE \xc2\xb7 ")
                        + juce::String (s.barlen) + U8 (" \xe2\x86\x92 ")
                        + juce::String ((int) snap.barLen)
                        + U8 (" FRAMES/BAR \xc2\xb7 NOTHING IS RESAMPLED; A COMMIT GOES IN "
                              "AT ITS RECORDED ") + barsWord (s.heldBars()),
                    l2, Justification::centredLeft, true);
    }
    else
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::nano());
        g.drawText (U8 ("SOURCE: ") + srcName (s.src) + U8 (" \xc2\xb7 ")
                        + srcBlurb (s.src) + U8 (" \xc2\xb7 COMMITS TO LANE ")
                        + laneName (s.lane),
                    l2, Justification::centredLeft, true);
    }

    /* ---- control row: captions behind the chips ------------------------- */
    Rectangle<int> dc = gm.detailCtl;
    g.setColour (C::PANEL);
    g.fillRect (dc);
    g.setColour (C::HAIRLINE);
    g.fillRect (dc.getX(), dc.getBottom() - 1, dc.getWidth(), 1);

    Rectangle<int> c = dc.reduced (16, 4);
    c.removeFromLeft (juce::jmin (128, c.getWidth()));       // SRC chip sits here
    c.removeFromLeft (10);
    Rectangle<int> laneBox = c.removeFromLeft (juce::jmin (150, c.getWidth()));
    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    g.drawText ("LANE " + laneName (s.lane), laneBox, Justification::centredLeft, true);
    g.setColour (C::INK_FAINT);
    g.drawText ("LENGTH", c.removeFromLeft (juce::jmin (58, c.getWidth())),
                Justification::centredLeft);

    /* ---- loop buffer ---------------------------------------------------- */
    int slice = s.slice;
    if (slice != 1 && slice != 2 && slice != 4 && slice != 8 && slice != 16) slice = 1;

    paintLabelRow (g, gm.waveLabel,
                   "LOOP BUFFER " + slotTag (f),
                   "SLICE GRID 1/" + juce::String (slice) + " SHOWN");
    g.setColour (C::HAIRLINE);
    g.fillRect (gm.waveLabel.getX(), gm.waveLabel.getBottom() - 1,
                gm.waveLabel.getWidth(), 1);

    Rectangle<int> box = gm.wave.reduced (10);
    if (box.getHeight() > 4 && box.getWidth() > 4)
    {
        g.setColour (C::SOCKET);
        g.fillRect (box);
        g.setColour (C::HAIRLINE_DIM);
        g.drawRect (box, 1);
        Rectangle<int> inner = box.reduced (1);

        if (s.frames > 0)
            drawEnv (g, inner, f, s.status == LOOP_PLAYING ? C::INK : C::INK_DIM);

        g.setColour (C::HAIRLINE_DIM);
        g.fillRect (inner.getX(), inner.getCentreY(), inner.getWidth(), 1);
        g.setColour (C::HAIRLINE);
        for (int i = 1; i < slice; ++i)
            g.fillRect (inner.getX() + i * inner.getWidth() / slice,
                        inner.getY(), 1, inner.getHeight());

        if (s.frames > 0 && s.status == LOOP_PLAYING)
        {
            const unsigned pos = s.pos % s.frames;
            g.setColour (C::BLOOD_HOT);
            g.fillRect (inner.getX() + (int) ((juce::uint64) pos
                                              * (unsigned) inner.getWidth() / s.frames),
                        inner.getY(), 1, inner.getHeight());
        }
        else if (s.status == LOOP_RECORDING)
        {
            /* No write cursor is published, so this is the TRANSPORT's
             * progress through the capture, labelled in bars. It is a clock,
             * not a level, and it says so. */
            const int tgt = juce::jmax (1, recBars[f]);
            const float done = (recFrom[f] >= 0.0f && snap.barF >= 0.0f)
                ? juce::jlimit (0.0f, 1.0f,
                                (((float) snap.bar + snap.barF) - recFrom[f]) / (float) tgt)
                : 0.0f;
            g.setColour (C::BLOOD_DEEP);
            g.fillRect (inner.getX(), inner.getY(),
                        (int) (done * (float) inner.getWidth()), inner.getHeight());
            g.setColour (C::BLOOD_HOT);
            g.fillRect (inner.getX() + (int) (done * (float) inner.getWidth()),
                        inner.getY(), 2, inner.getHeight());
            g.setColour (C::INK_BRIGHT);
            g.setFont (Type::label());
            g.drawText ("CAPTURING " + barsWord (tgt) + U8 (" \xc2\xb7 TRANSPORT CLOCK"),
                        inner, Justification::centred);
        }
        else if (s.frames == 0)
        {
            g.setColour (C::INK_FAINT);
            g.setFont (Type::label());
            g.drawText (master
                        ? U8 ("NOTHING CAPTURED \xc2\xb7 ARM SNAPS TO THE NEXT BAR")
                        : U8 ("NOTHING CAPTURED \xc2\xb7 ARM SNAPS TO THE CYCLE"),
                        inner, Justification::centred);
        }
    }

    paintLabelRow (g, gm.knobLabel, "LOOP CONTROL",
                   (master ? juce::String ("MIX") : juce::String ("LEVEL"))
                       + U8 (" \xc2\xb7 FB \xc2\xb7 OD \xc2\xb7 HALF \xc2\xb7 REV \xc2\xb7 SLICE"));
    g.setColour (C::HAIRLINE);
    g.fillRect (gm.knobLabel.getX(), gm.knobLabel.getBottom() - 1,
                gm.knobLabel.getWidth(), 1);
}

/* ======================================================================== */
/*  LaneStrip                                                                */
/* ======================================================================== */

namespace
{
    enum Cell { CellNone = -1, CellRow = 0, CellSrc, CellLevel, CellMute,
                CellArm, CellPlay, CellStop, CellClear, CellArr };
}

SurvivorPanel::LaneStrip::LaneStrip (SurvivorPanel& owner) : panel (owner) {}

Rectangle<int> SurvivorPanel::LaneStrip::rowArea (int slot) const
{
    return { 0, kHeadH + slot * kRowH, getWidth(), kRowH };
}

SurvivorPanel::LaneStrip::Cols SurvivorPanel::LaneStrip::cols (Rectangle<int> row) const
{
    Cols c;
    Rectangle<int> r = row.reduced (8, 0);

    c.tag   = r.removeFromLeft (juce::jmin (60,  r.getWidth())); r.removeFromLeft (6);
    c.state = r.removeFromLeft (juce::jmin (100, r.getWidth())); r.removeFromLeft (6);
    c.src   = r.removeFromLeft (juce::jmin (62,  r.getWidth())); r.removeFromLeft (6);
    c.len   = r.removeFromLeft (juce::jmin (78,  r.getWidth())); r.removeFromLeft (6);

    /* the transport cells are anchored to the RIGHT edge so they never move
     * when the window resizes -- muscle memory is the point of a lane strip */
    c.arr = r.removeFromRight (juce::jmin (48, r.getWidth())); r.removeFromRight (8);
    for (int i = 3; i >= 0; --i)
    {
        c.tr[i] = r.removeFromRight (juce::jmin (44, r.getWidth()));
        r.removeFromRight (2);
    }
    r.removeFromRight (8);
    c.mute  = r.removeFromRight (juce::jmin (22,  r.getWidth())); r.removeFromRight (4);
    c.level = r.removeFromRight (juce::jmin (112, r.getWidth())); r.removeFromRight (8);

    c.queue = r.removeFromLeft (juce::jmin (96, r.getWidth())); r.removeFromLeft (6);
    c.pos   = r;
    return c;
}

int SurvivorPanel::LaneStrip::rowAt (juce::Point<int> p) const
{
    if (p.y < kHeadH) return -1;
    const int n = (p.y - kHeadH) / kRowH;
    return (n >= 0 && n < BB_NLOOP) ? n : -1;
}

void SurvivorPanel::LaneStrip::paintCell (juce::Graphics& g, Rectangle<int> r,
                                          const juce::String& text, bool lit,
                                          bool enabled, bool oxide, bool hover) const
{
    if (r.getWidth() <= 0) return;
    Rectangle<int> box = r.withSizeKeepingCentre (r.getWidth(), juce::jmin (22, r.getHeight()));

    juce::Colour bg, bd, fg;
    if (! enabled)                { bg = C::DISABLED_BG; bd = C::HAIRLINE_DIM; fg = C::INK_GHOST; }
    else if (lit && oxide)        { bg = C::OXIDE_PLATE; bd = C::OXIDE_DIM;    fg = C::OXIDE_INK; }
    else if (lit)                 { bg = C::BLOOD_DEEP;  bd = C::BLOOD;        fg = C::ARMED_TEXT; }
    else if (hover)               { bg = C::PLATE_HOVER; bd = C::EDGE;         fg = C::INK; }
    else                          { bg = C::PLATE;       bd = C::EDGE;         fg = C::INK_DIM; }

    g.setColour (bg); g.fillRect (box);
    g.setColour (bd); g.drawRect (box, 1);
    g.setColour (fg);
    g.setFont (Type::micro());
    g.drawText (text, box, Justification::centred, false);
}

void SurvivorPanel::LaneStrip::paint (juce::Graphics& g)
{
    g.setColour (C::PANEL);
    g.fillRect (getLocalBounds());

    /* column captions -- a strip with eight columns and no headings is a
     * table you have to learn instead of read */
    Rectangle<int> head (0, 0, getWidth(), kHeadH);
    g.setColour (C::RAISED);
    g.fillRect (head);
    g.setColour (C::HAIRLINE);
    g.fillRect (0, kHeadH - 1, getWidth(), 1);

    const Cols c = cols (head);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::nano());
    g.drawText ("SLOT",   c.tag,   Justification::centredLeft);
    g.drawText ("STATE",  c.state, Justification::centredLeft);
    g.drawText ("SRC",    c.src,   Justification::centred);
    g.drawText ("LENGTH", c.len,   Justification::centred);
    g.drawText ("QUEUE",  c.queue, Justification::centred);
    if (c.pos.getWidth() > 60)
        g.drawText ("POSITION", c.pos, Justification::centredLeft);
    g.drawText ("LEVEL",  c.level, Justification::centred);
    g.drawText ("M",      c.mute,  Justification::centred);
    g.drawText ("ARM",    c.tr[0], Justification::centred);
    g.drawText ("PLAY",   c.tr[1], Justification::centred);
    g.drawText ("STOP",   c.tr[2], Justification::centred);
    g.drawText ("CLEAR",  c.tr[3], Justification::centred);
    g.drawText ("ARR",    c.arr,   Justification::centred);

    for (int n = 0; n < BB_NLOOP; ++n)
        paintRow (g, n);
}

void SurvivorPanel::LaneStrip::paintRow (juce::Graphics& g, int n)
{
    const SlotSnap& s = panel.snap.s[n];
    const StateVis vis = stateVis (s);
    const Rectangle<int> row = rowArea (n);
    const Cols c = cols (row);
    const bool master = (n == 0);
    const bool focused = (panel.focusSlot == n);

    g.setColour (vis.fill);
    g.fillRect (row);
    g.setColour (C::HAIRLINE_FAINT);
    g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

    /* focus is a structural mark, not a colour: the accent belongs to armed,
     * live and danger, and a focused row is none of those */
    if (focused)
    {
        g.setColour (C::INK_DIM);
        g.fillRect (row.getX(), row.getY(), 3, row.getHeight());
    }

    /* ---- tag ------------------------------------------------------------ */
    g.setColour (focused ? C::INK_BRIGHT : C::INK_DIM);
    g.setFont (Type::label());
    g.drawText (slotTag (n), c.tag.withTrimmedLeft (8), Justification::centredLeft);

    /* ---- state: lamp + stencil word ------------------------------------- */
    g.setColour (vis.lamp);
    g.fillRect (c.state.getX(), c.state.getCentreY() - 4, 8, 8);
    g.setColour (vis.ink);
    g.setFont (Type::stencil (15.0f, 0.12f));
    g.drawText (vis.word, c.state.withTrimmedLeft (14), Justification::centredLeft);

    /* ---- source --------------------------------------------------------- */
    paintCell (g, c.src, srcName (s.src), false, ! master, false,
               hoverSlot == n && hoverCell == CellSrc);

    /* ---- length: the RESOLVED bar count, never the raw field ------------ */
    {
        juce::String txt;
        juce::Colour ink = C::INK_DIM;
        if (s.frames > 0 && s.heldBars() > 0)
        {
            /* the star says "this length came from the CYCLE, not from the
             * bars field" -- without it a player who set 4 and got 8 has no
             * way to find out why, which is the legibility hazard the whole
             * FOLLOW default carries */
            txt = juce::String (s.heldBars()) + " BAR";
            if (s.bars == 0 && n != 0) txt += "*";
            if (panel.drifted (n)) { txt += U8 (" \xe2\x89\xa0"); ink = C::AMBER; }
        }
        else if (s.frames > 0)
        {
            /* a capture that the engine had to truncate below one bar: say
             * how long it really is rather than rounding it to "0 BAR" */
            txt = juce::String ((double) s.frames / (double) panel.snap.rate, 2) + " s";
            ink = C::AMBER;
        }
        else if (s.bars > 0)
        {
            txt = juce::String (s.bars) + " BAR";
            ink = C::INK_FAINT;
        }
        else
        {
            const int cyc = panel.snap.cycleBars;
            txt = cyc > 0 ? juce::String (cyc) + " BAR*" : "FOLLOW";
            ink = C::INK_FAINT;
        }
        g.setColour (ink);
        g.setFont (Type::micro());
        g.drawText (txt, c.len, Justification::centred);
    }

    /* ---- queue: what is latched, and how far off the boundary is -------- */
    {
        int action = s.pending & LBC_ACTION;
        bool cycleQ = (action == LBC_ARM || action == LBC_CLEAR);
        if (master)
        {
            /* slot 0 is not bar-latched: only its ARM waits, inside
             * loop_process(), for the next bar. Drawing a countdown on its
             * PLAY/STOP would be a lie about two different behaviours. */
            action = (s.status == LOOP_ARMED) ? LBC_ARM : 0;
            cycleQ = false;
        }
        if (action != 0)
        {
            Rectangle<int> chip = c.queue.withSizeKeepingCentre (c.queue.getWidth(),
                                                                 juce::jmin (22, c.queue.getHeight()));
            const float frac = cycleQ ? panel.cycleFrac()
                                      : (panel.snap.barF >= 0.0f ? panel.snap.barF : -1.0f);
            g.setColour (C::OXIDE_PLATE);
            g.fillRect (chip);
            if (frac >= 0.0f)
            {
                g.setColour (C::OXIDE_DIM);
                g.fillRect (chip.getX() + 1, chip.getBottom() - 3,
                            (int) (frac * (float) (chip.getWidth() - 2)), 2);
            }
            g.setColour (C::OXIDE_DIM);
            g.drawRect (chip, 1);
            g.setColour (C::AMBER);
            g.setFont (Type::micro());
            g.drawText (actionWord (action) + (cycleQ ? U8 (" \xc2\xb7 CYC")
                                                      : U8 (" \xc2\xb7 BAR")),
                        chip, Justification::centred);
        }
    }

    /* ---- position: the real buffer and the real playhead ---------------- */
    if (c.pos.getWidth() > 8)
    {
        Rectangle<int> well = c.pos.withSizeKeepingCentre (c.pos.getWidth() - 6,
                                                           juce::jmin (24, c.pos.getHeight() - 6));
        g.setColour (C::SOCKET);
        g.fillRect (well);
        g.setColour (C::HAIRLINE_DIM);
        g.drawRect (well, 1);
        Rectangle<int> inner = well.reduced (1);

        if (s.status == LOOP_RECORDING)
        {
            const int tgt = juce::jmax (1, panel.recBars[n]);
            const float done = (panel.recFrom[n] >= 0.0f && panel.snap.barF >= 0.0f)
                ? juce::jlimit (0.0f, 1.0f,
                                (((float) panel.snap.bar + panel.snap.barF)
                                     - panel.recFrom[n]) / (float) tgt)
                : 0.0f;
            g.setColour (C::BLOOD_DEEP);
            g.fillRect (inner.getX(), inner.getY(),
                        (int) (done * (float) inner.getWidth()), inner.getHeight());
            g.setColour (C::BLOOD_HOT);
            g.fillRect (inner.getX() + (int) (done * (float) inner.getWidth()),
                        inner.getY(), 2, inner.getHeight());
        }
        else if (s.frames > 0)
        {
            panel.drawEnv (g, inner, n,
                           s.status == LOOP_PLAYING ? (s.mute ? C::INK_GHOST : C::INK)
                                                    : C::LAMP_SOUNDING);
            if (s.slice > 1)
            {
                g.setColour (C::HAIRLINE_FAINT);
                for (int i = 1; i < s.slice; ++i)
                    g.fillRect (inner.getX() + i * inner.getWidth() / s.slice,
                                inner.getY(), 1, inner.getHeight());
            }
            if (s.status == LOOP_PLAYING)
            {
                const unsigned p = s.pos % s.frames;
                g.setColour (C::BLOOD_HOT);
                g.fillRect (inner.getX() + (int) ((juce::uint64) p
                                                  * (unsigned) inner.getWidth() / s.frames),
                            inner.getY(), 1, inner.getHeight());
            }
        }
        else
        {
            g.setColour (C::INK_GHOST);
            g.setFont (Type::nano());
            g.drawText (master ? U8 ("NOTHING CAPTURED \xc2\xb7 ARM SNAPS TO THE NEXT BAR")
                               : U8 ("NOTHING CAPTURED \xc2\xb7 ARM SNAPS TO THE CYCLE"),
                        inner, Justification::centred, true);
        }

        /* peak column, hard against the right of the well */
        Rectangle<int> pk (well.getRight() + 2, well.getY(), 3, well.getHeight());
        if (pk.getRight() <= c.pos.getRight())
        {
            g.setColour (C::TROUGH);
            g.fillRect (pk);
            const int h = (int) (juce::jlimit (0.0f, 1.0f, panel.peakUi[n])
                                 * (float) pk.getHeight());
            if (h > 0)
            {
                g.setColour (panel.peakUi[n] > 0.5f ? C::AMBER : C::BLOOD);
                g.fillRect (pk.getX(), pk.getBottom() - h, pk.getWidth(), h);
            }
        }
    }

    /* ---- level ---------------------------------------------------------- */
    {
        Rectangle<int> lv = c.level;
        Rectangle<int> num = lv.removeFromRight (juce::jmin (30, lv.getWidth()));
        Rectangle<int> well = lv.withSizeKeepingCentre (juce::jmax (0, lv.getWidth() - 4),
                                                        juce::jmin (12, lv.getHeight()));
        const int val = (dragSlot == n) ? dragVal : s.level;
        paintWell (g, well, (float) juce::jlimit (0, 256, val) / 256.0f,
                   s.mute ? C::LAMP_DEAD : C::BLOOD,
                   dragSlot == n ? C::EDGE : C::HAIRLINE);
        g.setColour (s.mute ? C::INK_GHOST : C::INK_DIM);
        g.setFont (Type::micro());
        g.drawText (juce::String (val), num, Justification::centredRight);
    }

    /* ---- mute ----------------------------------------------------------- */
    if (master)
    {
        /* the master looper has no mute in the engine -- MIX at 0 is its
         * silence -- so the cell is a dash, not a dead button */
        g.setColour (C::INK_GHOST);
        g.setFont (Type::micro());
        g.drawText (U8 ("\xe2\x80\x94"), c.mute, Justification::centred);
    }
    else
        paintCell (g, c.mute, "M", s.mute != 0, true, true,
                   hoverSlot == n && hoverCell == CellMute);

    /* ---- transport ------------------------------------------------------ */
    const bool armLit  = s.status == LOOP_ARMED || s.status == LOOP_RECORDING;
    const bool playLit = s.status == LOOP_PLAYING;
    paintCell (g, c.tr[0], "ARM",   armLit,  true, false, hoverSlot == n && hoverCell == CellArm);
    paintCell (g, c.tr[1], "PLAY",  playLit, true, false, hoverSlot == n && hoverCell == CellPlay);
    paintCell (g, c.tr[2], "STOP",  false,   true, false, hoverSlot == n && hoverCell == CellStop);
    paintCell (g, c.tr[3], "CLEAR", false,   s.frames > 0 || s.status != LOOP_OFF, false,
               hoverSlot == n && hoverCell == CellClear);
    paintCell (g, c.arr, U8 ("\xe2\x86\x92" "ARR"), false,
               panel.canCommit (n) && panel.commitToArrange != nullptr, true,
               hoverSlot == n && hoverCell == CellArr);
}

/* ---- hit testing -------------------------------------------------------- */

void SurvivorPanel::LaneStrip::mouseDown (const juce::MouseEvent& e)
{
    const int n = rowAt (e.getPosition());
    if (n < 0) return;
    panel.setFocus (n);

    const Cols c = cols (rowArea (n));
    const juce::Point<int> p = e.getPosition();
    const bool hard = e.mods.isPopupMenu() || e.mods.isCommandDown();

    if (c.src.contains (p))   { panel.srcMenu (n, this); return; }

    if (c.mute.contains (p) && n != 0)
    {
        const int v = panel.snap.s[n].mute ? 0 : 1;
        bb_engine_loop_ctl (n, L2C_MUTE, v);
        panel.setNote (slotTag (n) + (v ? " MUTED -- ITS PLAYHEAD KEEPS RUNNING"
                                        : " UNMUTED"), C::INK_DIM);
        return;
    }

    if (c.level.contains (p))
    {
        dragSlot = n;
        dragStartX = p.x;
        dragStartVal = panel.snap.s[n].level;
        dragVal = dragStartVal;
        return;
    }

    if (c.tr[0].contains (p)) { panel.fire (n, LBC_ARM,   hard); return; }
    if (c.tr[1].contains (p)) { panel.fire (n, LBC_PLAY,  hard); return; }
    if (c.tr[2].contains (p)) { panel.fire (n, LBC_STOP,  hard); return; }
    if (c.tr[3].contains (p))
    {
        /* the cell is drawn dead when there is nothing to wipe, and a cell
         * that is drawn dead must not fire -- that is the whole contract of
         * drawing it dead */
        const SlotSnap& sl = panel.snap.s[n];
        if (sl.frames > 0 || sl.status != LOOP_OFF) panel.fire (n, LBC_CLEAR, hard);
        return;
    }

    if (c.arr.contains (p))
    {
        /* right-click picks the destination lane; left-click commits to it */
        if (e.mods.isPopupMenu()) panel.laneMenu (n, this);
        else                      panel.commit (n);
    }
}

void SurvivorPanel::LaneStrip::mouseDrag (const juce::MouseEvent& e)
{
    if (dragSlot < 0) return;
    const Cols c = cols (rowArea (dragSlot));
    const int w = juce::jmax (1, c.level.getWidth() - 34);
    dragVal = juce::jlimit (0, 256, dragStartVal
                                        + (e.getPosition().x - dragStartX) * 256 / w);
    bb_engine_loop_ctl (dragSlot, L2C_LEVEL, dragVal);
    repaint();
}

void SurvivorPanel::LaneStrip::mouseUp (const juce::MouseEvent&)
{
    dragSlot = -1;
    repaint();
}

void SurvivorPanel::LaneStrip::mouseMove (const juce::MouseEvent& e)
{
    const int n = rowAt (e.getPosition());
    int cell = CellNone;
    if (n >= 0)
    {
        const Cols c = cols (rowArea (n));
        const juce::Point<int> p = e.getPosition();
        cell = c.src.contains (p)    ? CellSrc
             : c.mute.contains (p)   ? CellMute
             : c.level.contains (p)  ? CellLevel
             : c.tr[0].contains (p)  ? CellArm
             : c.tr[1].contains (p)  ? CellPlay
             : c.tr[2].contains (p)  ? CellStop
             : c.tr[3].contains (p)  ? CellClear
             : c.arr.contains (p)    ? CellArr
                                     : CellRow;
    }
    if (n != hoverSlot || cell != hoverCell)
    {
        hoverSlot = n;
        hoverCell = cell;
        repaint();
    }
}

void SurvivorPanel::LaneStrip::mouseExit (const juce::MouseEvent&)
{
    hoverSlot = -1;
    hoverCell = CellNone;
    repaint();
}

juce::String SurvivorPanel::LaneStrip::getTooltip()
{
    if (hoverSlot < 0) return {};
    const int n = hoverSlot;
    const SlotSnap& s = panel.snap.s[n];
    const juce::String tag = slotTag (n);
    const juce::String hardHint =
        U8 (" Right-click or ") + modKeyWord() + U8 ("-click = HARD: skip the quantum.");

    switch (hoverCell)
    {
        case CellSrc:
            return n == 0
                ? U8 ("MASTER records the whole bus, the other five loopers included. "
                      "Its source is pinned -- that pin is the bit-exactness argument.")
                : tag + U8 (" records ") + srcName (s.src) + U8 (" \xe2\x80\x94 ")
                      + srcBlurb (s.src) + U8 (". Click to change. GRAIN MASS is inside "
                                               "the master bus, so LIVE and DRY carry it.");
        case CellLevel:
            return n == 0
                ? U8 ("MIX \xe2\x80\x94 dry vs loop crossfade for the master phrase looper. Drag.")
                : tag + U8 (" LEVEL into the master bus. Drag.");
        case CellMute:
            return tag + U8 (" MUTE \xe2\x80\x94 silence the layer WITHOUT stopping it. The "
                             "playhead keeps running, so it comes back in sync.");
        case CellArm:
            return n == 0
                ? U8 ("ARM \xe2\x80\x94 capture the whole bus into the master phrase looper, "
                      "starting at the next bar.")
                : tag + U8 (" ARM \xe2\x80\x94 destructive re-take. The capture starts on the "
                            "CYCLE so every layer shares a downbeat. HARD is ignored on ARM: "
                            "a loop that is not whole bars is what makes a clip drift.");
        case CellPlay:
            return tag + U8 (" PLAY \xe2\x80\x94 lands on the next bar and RE-PHASES to the "
                             "transport, so a layer brought back mid-cycle drops in where it "
                             "belongs.") + hardHint;
        case CellStop:
            return tag + U8 (" STOP \xe2\x80\x94 lands on the next bar. The buffer is kept.")
                       + hardHint;
        case CellClear:
            return tag + U8 (" CLEAR \xe2\x80\x94 wipe the buffer. Lands on the cycle.")
                       + hardHint;
        case CellArr:
            return tag + U8 (" \xe2\x86\x92 ARRANGE lane ") + laneName (s.lane)
                       + U8 (". Committing FREEZES the loop: its overdub is turned off so "
                             "the buffer can be copied. Right-click to choose the lane.");
        default:
            return tag + U8 (" \xe2\x80\x94 click to focus. ")
                       + (s.frames > 0 ? barsWord (s.heldBars()) + U8 (" captured.")
                                       : juce::String (U8 ("Empty.")))
                       + U8 (" A starred length came from the CYCLE rather than from this "
                             "slot's own bars setting; \xe2\x89\xa0 means the tempo has moved "
                             "since the capture.");
    }
}

} // namespace morgue
