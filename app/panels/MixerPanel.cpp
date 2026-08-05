/* MixerPanel.cpp -- see MixerPanel.h. Engine wiring preserved from the
 * original console: fader/mute/master atomics with the 30 Hz sync pull,
 * isUserDragging guards and setValueQuiet. SEND A and the RETURN A strip
 * (the CHAMBER) are live; everything the engine cannot do yet (inserts,
 * sends B-D, pan, return B, solo) is the R4/R5 drawn state from the
 * handoff mock -- painted, never wired. */

#include "MixerPanel.h"
#include "bytebeat.h"
#include "engine.h"
#include "rack.h"

#include <cmath>
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

    /* strip stack geometry (spec section 10; heights include the 1px
     * HAIRLINE_DIM rule that closes each block) */
    constexpr int kHeaderH    = 22;
    constexpr int kInsLabelY  = 27;    // "INSERTS" 7px row
    constexpr int kInsSlotY   = 38;    // 4 slots, 15 tall, 1px gaps
    constexpr int kInsRuleY   = 106;
    constexpr int kSendKnobY  = 112;   // four 20px knobs
    constexpr int kSendLblY   = 133;
    constexpr int kSendRuleY  = 145;
    constexpr int kPanLblY    = 151;   // 6px label
    constexpr int kPanTroughY = 160;   // 8px trough
    constexpr int kPanRuleY   = 173;
    constexpr int kFaderTop   = 174;   // flex fader area, padding 8
    constexpr int kBottomH    = 65;    // value 12 + 3 + dB 9 + 3 + M/S 16
                                       //   + 3 + route 14 + pad 5

    /* R4 send amounts, exactly the mock's formula:
     * amt(strip, send) = [0.2, 0.55, 0, 0.35][(strip + send) % 4] */
    constexpr float kSendAmt[4] = { 0.20f, 0.55f, 0.00f, 0.35f };

    /* R4 insert chains per strip (nullptr = empty slot) */
    const char* const kInserts[12][4] = {
        { "DRIVE",     "BITCRUSH",  nullptr, nullptr },   // V01
        { "WAVEFOLD",  "LP FILTER", "DELAY", nullptr },   // V02
        { "HP FILTER", nullptr,     nullptr, nullptr },   // V03
        { "DRIVE",     "FEEDBACK",  nullptr, nullptr },   // V04
        { "PITCH-SH",  nullptr,     nullptr, nullptr },   // V05
        { "REVERB",    nullptr,     nullptr, nullptr },   // V06
        { nullptr,     nullptr,     nullptr, nullptr },   // V07
        { "GLITCH",    "CRUSH",     nullptr, nullptr },   // V08
        { "DRIVE",     nullptr,     nullptr, nullptr },   // LICKS
        { "CHAMBER",   nullptr,     nullptr, nullptr },   // RETURN A (live)
        { "FB DELAY",  nullptr,     nullptr, nullptr },   // RETURN B
        { "TONE",      "LIMITER",   nullptr, nullptr },   // MASTER (blood)
    };

    /* R5 pan positions per strip (label, handle % across the trough) */
    const char* const kPanLabel[12] =
        { "L12", "C", "R08", "L24", "C", "R16", "C", "L04", "C", "C", "C", "C" };
    constexpr int kPanPos[12] = { 38, 50, 58, 26, 50, 66, 50, 46, 50, 50, 50, 50 };

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
} // namespace

/* ======================================================================== */
/*  Strip                                                                    */
/* ======================================================================== */

MixerPanel::Strip::Strip (Kind k, int stripIndex, int layerIndex)
    : kind (k), strip (stripIndex), layer (layerIndex)
{
    for (int i = 0; i < 4; ++i) inserts[i] = kInserts[strip][i];
    panLabel = kPanLabel[strip];
    panPos   = kPanPos[strip];
    route    = kind == Kind::Master ? "OUT 1-2"
             : kind == Kind::Return ? "MASTER"
             : strip < 4            ? "G1" : "MASTER";

    /* every strip gets the 8px meter trough; only strips with a real
     * engine source ever fill it */
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

        /* live meter: the engine max-holds bb.layer[L].peak per period;
         * read-and-clear here, decay on the UI side for the fall */
        meter->source = [held = std::make_shared<float> (0.0f), L = layer]() -> float
        {
            const int pk = atomic_exchange (&bb.layer[L].peak, 0);
            const float v = (float) pk / 32768.0f;
            *held = juce::jmax (v, *held * 0.82f);
            return *held < 0.004f ? 0.0f : *held;
        };
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
    else if (kind == Kind::Licks)
    {
        /* engine gap: no sampler bus level/mute/peak -- drawn strip,
         * except the live CHAMBER send */
        name = "LICKS"; value = 200; drawnDb = "\xe2\x88\x92" "4.7";
    }
    else if (strip == 9)   // RETURN A: the CHAMBER, live
    {
        name = "RET A CHAMBER";

        fader = std::make_unique<TroughFader> ("RET A");
        fader->setTooltip (U8 ("RETURN A \xe2\x80\x94 the CHAMBER (master reverb) "
                               "into the master bus. 0\xe2\x80\x93" "256. "
                               "0 closes the room entirely."));
        fader->onChange = [this] (int v)
        {
            atomic_store (&bb.verb_level, v);
            repaint();
        };
        addAndMakeVisible (*fader);

        retSize = std::make_unique<EngravedKnob> (juce::String(), 20, 0, 255, 172);
        retSize->setShowText (false);
        retSize->setTooltip (U8 ("SIZE \xe2\x80\x94 how long the chamber holds a "
                                 "sound. 0\xe2\x80\x93" "255."));
        retSize->onChange = [] (int v) { atomic_store (&bb.verb_size, v); };
        addAndMakeVisible (*retSize);

        retTone = std::make_unique<EngravedKnob> (juce::String(), 20, 0, 255, 96);
        retTone->setShowText (false);
        retTone->setTooltip (U8 ("TONE \xe2\x80\x94 damping inside the chamber: "
                                 "low is a dark cavern, high keeps the air. "
                                 "0\xe2\x80\x93" "255."));
        retTone->onChange = [] (int v) { atomic_store (&bb.verb_tone, v); };
        addAndMakeVisible (*retTone);

        meter->source = [held = std::make_shared<float> (0.0f)]() -> float
        {
            const int pk = atomic_exchange (&bb.verb_peak, 0);
            const float v = (float) pk / 32768.0f;
            *held = juce::jmax (v, *held * 0.82f);
            return *held < 0.004f ? 0.0f : *held;
        };
    }
    else // RETURN B: R4 drawn
    {
        name    = "RET B DLY";
        value   = 164;
        drawnDb = "\xe2\x88\x92" "7.7";
    }

    if (kind == Kind::Voice || kind == Kind::Licks)
    {
        sendA = std::make_unique<EngravedKnob> (juce::String(), 20, 0, 255, 0);
        sendA->setShowText (false);
        if (kind == Kind::Voice)
        {
            sendA->setTooltip (U8 ("SEND A \xe2\x80\x94 voice ")
                               + juce::String (layer + 1)
                               + U8 (" into the CHAMBER return. 0\xe2\x80\x93"
                                     "255. Post-fader."));
            sendA->onChange = [this] (int v)
            { atomic_store (&bb.layer[layer].send, v); };
        }
        else
        {
            sendA->setTooltip (U8 ("SEND A \xe2\x80\x94 the sampler bus into the "
                                   "CHAMBER return. 0\xe2\x80\x93" "255."));
            sendA->onChange = [] (int v) { atomic_store (&bb.smp_send, v); };
        }
        addAndMakeVisible (*sendA);
    }
}

void MixerPanel::Strip::update (const juce::String& newName, bool newMuted,
                                bool newHot, int newValue)
{
    if (name == newName && muted == newMuted && hot == newHot && value == newValue)
        return;
    name = newName; muted = newMuted; hot = newHot; value = newValue;
    repaint();
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
    const int mw = (inner - 2) / 2;
    if (muteBtn) muteBtn->setBounds (5, h - 38, mw, 16);

    /* live knobs sit exactly where the send row paints their slots */
    if (sendA)   sendA->setBounds (5, kSendKnobY, 20, 20);
    if (retSize) retSize->setBounds (5, kSendKnobY, 20, 20);
    if (retTone) retTone->setBounds (5 + juce::roundToInt ((inner - 20) / 3.0f),
                                     kSendKnobY, 20, 20);
}

juce::String MixerPanel::Strip::dbText() const
{
    if (drawnDb) return U8 (drawnDb);
    if (muted || value <= 0) return U8 ("\xe2\x88\x92\xe2\x88\x9e");
    const float dB = 20.0f * std::log10 ((float) value / 256.0f);
    const float r  = std::round (dB * 10.0f) / 10.0f;
    if (r >= 0.0f) return "+" + juce::String (r, 1);
    return U8 ("\xe2\x88\x92") + juce::String (-r, 1);
}

void MixerPanel::Strip::paint (juce::Graphics& g)
{
    const int w = getWidth(), h = getHeight();
    const int inner = w - 10;

    g.setColour (kind == Kind::Master ? MASTER_BG
               : kind == Kind::Return ? C::PANEL : STRIP_BG);
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

    /* -- INSERTS: 4 slots 15 tall (R4 drawn) ---------------------------- */
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (7.0f, 0.10f));
    g.drawText ("INSERTS", 5, kInsLabelY, inner, 9, Justification::centredLeft);
    for (int i = 0; i < 4; ++i)
    {
        const Rectangle<int> slot (5, kInsSlotY + i * 16, inner, 15);
        const bool loaded = inserts[i] != nullptr;
        const bool blood  = loaded && kind == Kind::Master;
        g.setColour (blood ? C::BLOOD_DEEP : loaded ? C::OXIDE_PLATE : C::DISABLED_BG);
        g.fillRect (slot);
        g.setColour (blood ? C::BLOOD : loaded ? C::OXIDE_DIM : C::HAIRLINE_DIM);
        g.drawRect (slot, 1);
        g.setColour (blood ? C::ARMED_TEXT : loaded ? C::OXIDE_INK : C::INK_GHOST);
        g.setFont (Type::nano());
        g.drawText (loaded ? juce::String (inserts[i]) : U8 ("\xe2\x80\x94"),
                    slot.reduced (4, 0), Justification::centredLeft, true);
    }
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kInsRuleY, w, 1);

    /* -- SENDS row: A is a LIVE knob on voice/LICKS strips; on RETURN A
     * the row carries the chamber's own SIZE and TONE (live). Everything
     * else is the R4 drawn state, exactly the mock's formula. ------------ */
    static const char* const sendName[4] = { "A", "B", "C", "D" };
    const bool chamber = retSize != nullptr;
    for (int j = 0; j < 4; ++j)
    {
        const bool liveHere = (j == 0 && sendA != nullptr)
                           || (chamber && j < 2);
        const float amt = (chamber && j >= 2) ? 0.0f
                        : kSendAmt[(strip + j) % 4];
        const int kx = 5 + juce::roundToInt (j * (inner - 20) / 3.0f);

        if (! liveHere)
        {
            g.setColour (C::CONTROL);
            g.fillEllipse ((float) kx, (float) kSendKnobY, 20.0f, 20.0f);
            g.setColour (amt > 0.4f ? C::OXIDE_DIM : C::LAMP_DEAD);
            g.drawEllipse ((float) kx + 0.5f, (float) kSendKnobY + 0.5f, 19.0f, 19.0f, 1.0f);

            const float a  = juce::degreesToRadians (-135.0f + 270.0f * amt);
            const float cx = (float) kx + 10.0f, cy = (float) kSendKnobY + 10.0f;
            const float sn = std::sin (a), cs = std::cos (a);
            g.setColour (amt > 0.4f ? C::OXIDE_INK : amt > 0.0f ? C::INK_DIM : C::INK_GHOST);
            g.drawLine (cx + sn * 1.0f, cy - cs * 1.0f, cx + sn * 8.0f, cy - cs * 8.0f, 1.0f);
        }

        const char* lbl = chamber ? (j == 0 ? "SIZE" : j == 1 ? "TONE"
                                                     : "\xe2\x80\x94")
                                  : sendName[j];
        g.setColour (liveHere ? C::INK_DIM
                   : amt > 0.4f ? C::OXIDE : C::INK_FAINT);
        g.setFont (Type::mono (6.0f));
        g.drawText (U8 (lbl), kx - 3, kSendLblY, 26, 7, Justification::centred);
    }
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kSendRuleY, w, 1);

    /* -- PAN: 6px label + 8px trough with centre tick (R5 drawn) -------- */
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (6.0f, 0.10f));
    g.drawText ("PAN " + juce::String (panLabel), 5, kPanLblY, inner, 7,
                Justification::centredLeft);
    g.setColour (C::PANEL);
    g.fillRect (5, kPanTroughY, inner, 8);
    g.setColour (C::HAIRLINE);
    g.drawRect (5, kPanTroughY, inner, 8, 1);
    g.fillRect (5 + inner / 2, kPanTroughY + 1, 1, 6);
    g.setColour (C::OXIDE);
    g.fillRect (6 + juce::roundToInt ((inner - 5) * panPos / 100.0f),
                kPanTroughY + 1, 3, 6);
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (0, kPanRuleY, w, 1);

    /* -- fader area: drawn trough on the strips with no engine level ---- */
    if (fader == nullptr)
    {
        const int cxp = w / 2;
        const int fy = kFaderTop + 8;
        const int fh = juce::jmax (0, h - kBottomH - 8 - fy);
        if (fh > 4)
        {
            const Rectangle<int> trough (cxp - 15, fy, 16, fh);
            g.setColour (C::TROUGH);
            g.fillRect (trough);
            g.setColour (C::HAIRLINE);
            g.drawRect (trough, 1);
            const int fillH = juce::roundToInt ((fh - 2) * value / 256.0f);
            g.setColour (muted ? C::LAMP_DEAD : C::BLOOD);
            g.fillRect (trough.getX() + 1, trough.getBottom() - 1 - fillH, 14, fillH);
            const int capY = juce::jlimit (fy, fy + fh - 3,
                                           trough.getBottom() - 1 - fillH - 1);
            g.setColour (C::INK);
            g.fillRect (trough.getX() - 3, capY, 22, 3);
        }
    }

    /* -- value 10px, dB 7px --------------------------------------------- */
    const int liveVal = fader ? fader->value() : value;
    g.setColour (nameFg);
    g.setFont (Type::mono (10.0f, 0.04f));
    g.drawText (juce::String (liveVal).paddedLeft ('0', 3),
                5, h - 65, inner, 12, Justification::centred);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (7.0f));
    g.drawText (dbText(), 5, h - 50, inner, 9, Justification::centred);

    /* -- M/S plates 16 tall: painted where the engine has no control ---- */
    const int mw = (inner - 2) / 2;
    auto idlePlate = [&] (Rectangle<int> r, const char* txt)
    {
        g.setColour (C::PLATE_LOW);
        g.fillRect (r);
        g.setColour (C::LAMP_DEAD);
        g.drawRect (r, 1);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText (txt, r, Justification::centred);
    };
    if (muteBtn == nullptr)
        idlePlate ({ 5, h - 38, mw, 16 }, "M");           // LICKS / returns (R4)
    idlePlate ({ 5 + mw + 2, h - 38, inner - mw - 2, 16 }, "S");   // solo (R5)

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
/*  MixerPanel                                                               */
/* ======================================================================== */

MixerPanel::MixerPanel()
{
    for (int L = 0; L < BB_NLAYER; ++L)
        strips.add (new Strip (Strip::Kind::Voice, L, L));
    strips.add (new Strip (Strip::Kind::Licks,   8, -1));
    strips.add (new Strip (Strip::Kind::Return,  9, -1));
    strips.add (new Strip (Strip::Kind::Return, 10, -1));
    strips.add (new Strip (Strip::Kind::Master, 11, -1));

    for (auto* s : strips)
        addAndMakeVisible (s);

    sync();
}

void MixerPanel::sync()
{
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
            if (s->sendA && ! s->sendA->isUserDragging())
                s->sendA->setValueQuiet (atomic_load_explicit (&bb.layer[L].send,
                                                               memory_order_relaxed));
            juce::String nm = "V" + juce::String (L + 1).paddedLeft ('0', 2);
            if (! bb_custom[L])
                nm << " " << juce::String (rack_src_name (bb_rack[L].src)).toUpperCase();
            s->update (nm, ! on, atomic_load (&bb.focus) == L, s->fader->value());
        }
        else if (s->kind == Strip::Kind::Licks)
        {
            if (s->sendA && ! s->sendA->isUserDragging())
                s->sendA->setValueQuiet (atomic_load (&bb.smp_send));
        }
        else if (s->kind == Strip::Kind::Return && s->fader != nullptr)
        {
            /* RETURN A -- the CHAMBER */
            const int lvl = atomic_load (&bb.verb_level);
            if (! s->fader->isUserDragging())
                s->fader->setValueQuiet (lvl);
            s->fader->setMuted (lvl == 0);
            if (s->retSize && ! s->retSize->isUserDragging())
                s->retSize->setValueQuiet (atomic_load (&bb.verb_size));
            if (s->retTone && ! s->retTone->isUserDragging())
                s->retTone->setValueQuiet (atomic_load (&bb.verb_tone));
            s->update ("RET A CHAMBER", lvl == 0, false, s->fader->value());
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
        /* LICKS / RETURN strips are the static R4 drawing -- nothing to pull */
    }
}

void MixerPanel::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    footerArea = b.removeFromBottom (26);
    stripsArea = b;

    /* 11 strips flex 1 + master flex 1.5 -> integer edges at W*u/12.5 */
    const double W = (double) stripsArea.getWidth();
    int ex[13];
    for (int i = 0; i <= 12; ++i)
    {
        const double units = i <= 11 ? (double) i : 12.5;
        ex[i] = stripsArea.getX() + juce::roundToInt (W * units / 12.5);
    }
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (ex[i], stripsArea.getY(),
                              ex[i + 1] - ex[i], stripsArea.getHeight());
}

void MixerPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "MIXER",
                     U8 ("8 VOICES + SAMPLER + RETURNS A\xe2\x80\x93" "D + MASTER"),
                     juce::String (SerialNo::MIXER)
                         + U8 (" \xc2\xb7 FADER 0\xe2\x80\x93" "256 \xc2\xb7 INTEGER-FIRST DSP"),
                     Badge::PARTIAL, "PARTIAL");

    /* -- footer 26: returns / groups / R4-R5 interactions ---------------- */
    g.setColour (C::PANEL_ALT);
    g.fillRect (footerArea);
    g.setColour (C::HAIRLINE);
    g.fillRect (footerArea.getX(), footerArea.getY(), footerArea.getWidth(), 1);

    const juce::Font ff = Type::mono (8.0f, 0.12f);
    g.setFont (ff);
    g.setColour (C::INK_FAINT);
    int fx = footerArea.getX() + 10;
    const juce::String docs[2] = {
        U8 ("RETURNS: A CHAMBER (LIVE) \xc2\xb7 B FEEDBACK DELAY \xc2\xb7 C PITCH-GLITCH \xc2\xb7 D WAVEFOLD"),
        U8 ("GROUPS: G1 VOICES 01\xe2\x80\x93" "04 \xc2\xb7 G2 SAMPLER"),
    };
    for (const auto& t : docs)
    {
        const int tw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (ff, t));
        g.drawText (t, fx, footerArea.getY() + 1, tw + 2, footerArea.getHeight() - 1,
                    Justification::centredLeft);
        fx += tw + 14;
    }
    g.setColour (C::OXIDE);
    g.drawText (U8 ("R4/R5 \xc2\xb7 INSERT PICKER ON CLICK \xc2\xb7 LEARN ON RIGHT-CLICK"),
                footerArea.withTrimmedRight (10).withTrimmedTop (1),
                Justification::centredRight);
}

} // namespace morgue
