/* SurvivorPanel.cpp -- see SurvivorPanel.h. Engine wiring preserved from
 * the original console; geometry and copy follow spec section 9 and the
 * "05 SURVIVOR" HTML frame exactly. Row 1 is the three 150x74 stencil
 * buttons + BUFFER/SOURCE/CAPTURE data block + LOOP OUT meter; row 2 is
 * the real loop-buffer waveform in a SOCKET box; row 3 is the six 76px
 * knobs (the largest in the app). */

#include "SurvivorPanel.h"
#include "bytebeat.h"
#include "engine.h"

#include <cmath>

/* The loop-buffer waveform reads the real engine buffer through
 * bb_engine_loop_buffer() (engine.h): read-only, UI-thread; torn reads are
 * cosmetic exactly like bb.scope. */

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

static const int sliceTable[] = { 1, 2, 4, 8, 16 };

/* "88 200" -- thousands grouped with a space, per the HTML data block. */
static juce::String groupedInt (unsigned v)
{
    juce::String s ((juce::int64) v);
    for (int i = s.length() - 3; i > 0; i -= 3)
        s = s.substring (0, i) + " " + s.substring (i);
    return s;
}

/* Real loop-out peak for the 140x8 meter: window of the loop buffer around
 * the published play position, scaled by the wet mix. Zero (empty trough)
 * whenever the loop is not sounding or the buffer accessor is absent. */
static float loopOutPeak()
{
    if (atomic_load (&bb.loop_status) != LOOP_PLAYING) return 0.0f;
    const unsigned frames = atomic_load (&bb.loop_frames);
    if (frames == 0) return 0.0f;
    const int16_t* buf = bb_engine_loop_buffer();

    const unsigned pos = atomic_load (&bb.loop_pos) % frames;
    const unsigned n = juce::jmin (1024u, frames);
    int peak = 0;
    for (unsigned i = 0; i < n; ++i)
    {
        int s = (int) buf[(pos + frames - i) % frames];
        if (s < 0) s = -s;
        if (s > peak) peak = s;
    }
    const float wet = (float) juce::jlimit (0, 256, atomic_load (&bb.loop_mix)) / 256.0f;
    return juce::jlimit (0.0f, 1.0f, ((float) peak / 32768.0f) * wet);
}

SurvivorPanel::SurvivorPanel()
    : arm ("ARM", true), play ("PLAY", true), clearBtn ("CLEAR", false, false)
{
    arm.setStencilText (true);
    play.setStencilText (true);
    clearBtn.setStencilText (true);

    arm.setTooltip (U8 ("ARM \xe2\x80\x94 capture the pre-master bus into the loop, "
                        "starting at the next bar boundary. 1\xe2\x80\x93""4 bars."));
    arm.setSubLine ("IDLE");
    arm.onToggle = [] (bool on) {
        if (on) bb_engine_loop_command (LOOP_CMD_ARM);
        else    bb_engine_loop_command (LOOP_CMD_CLEAR);
    };
    play.setTooltip (U8 ("PLAY \xe2\x80\x94 play the captured phrase as a loop under "
                         "the live voices. Latches until CLEAR."));
    play.setSubLine ("LOOP IDLE");
    play.onToggle = [] (bool) { bb_engine_loop_command (LOOP_CMD_PLAY); };
    clearBtn.setTooltip (U8 ("CLEAR \xe2\x80\x94 wipe the loop buffer and return to idle. "
                             "Momentary."));
    clearBtn.setSubLine ("WIPE BUFFER");
    clearBtn.onToggle = [] (bool) { bb_engine_loop_command (LOOP_CMD_CLEAR); };

    addAndMakeVisible (arm);
    addAndMakeVisible (play);
    addAndMakeVisible (clearBtn);

    loopOutMeter.setHorizontal (true);
    loopOutMeter.source = [] { return loopOutPeak(); };
    addAndMakeVisible (loopOutMeter);

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
    mk ("MIX",   0, 256, 256, U8 ("DRY \xe2\x86\x94 LOOP"),
        U8 ("MIX \xe2\x80\x94 dry vs loop balance. 0\xe2\x80\x93""256."))
        ->onChange = [] (int v) { atomic_store (&bb.loop_mix, v); };
    mk ("FB",    0, 256, 192, "RETENTION",
        U8 ("FB \xe2\x80\x94 how much of the loop survives each overdub pass. 0\xe2\x80\x93""256."))
        ->onChange = [] (int v) { atomic_store (&bb.loop_feedback, v); };
    mk ("OD",    0, 1,   0,   "OVERDUB",
        U8 ("OD \xe2\x80\x94 overdub live input into the loop. OFF/ON."))
        ->onChange = [] (int v) { atomic_store (&bb.loop_overdub, v); };
    mk ("HALF",  0, 2,   LOOP_RATE_NORMAL, U8 ("\xc2\xbd / 1\xc3\x97 / 2\xc3\x97"),
        U8 ("HALF \xe2\x80\x94 loop playback rate. \xc2\xbd, 1\xc3\x97, 2\xc3\x97."))
        ->onChange = [] (int v) { atomic_store (&bb.loop_rate, v); };
    mk ("REV",   0, 1,   0,   "REVERSE",
        U8 ("REV \xe2\x80\x94 play the loop backwards. OFF/ON."))
        ->onChange = [] (int v) { atomic_store (&bb.loop_reverse, v); };
    mk ("SLICE", 0, 4,   0,   "STUTTER",
        U8 ("SLICE \xe2\x80\x94 stutter grid, repeats a fraction of the phrase. 1/1\xe2\x80\x93""1/16."))
        ->onChange = [] (int v) {
            atomic_store (&bb.loop_slice, sliceTable[juce::jlimit (0, 4, v)]);
        };

    startTimerHz (30);
}

void SurvivorPanel::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);

    /* row 1 (120): 150x74 buttons at padding 16, gap 12 (HTML frame 05) */
    Rectangle<int> row1 = b.removeFromTop (120);
    const int by = row1.getY() + (120 - 74) / 2;
    arm.setBounds      (row1.getX() + 16,  by, 150, 74);
    play.setBounds     (row1.getX() + 178, by, 150, 74);
    clearBtn.setBounds (row1.getX() + 340, by, 150, 74);

    /* right: LOOP OUT label (painted) over the 140x8 meter, gap 4 */
    loopOutMeter.setBounds (row1.getRight() - 16 - 140, row1.getCentreY() + 3, 140, 8);

    /* row 2 (210) is painted; row 3 (flex): label row 20 + six equal cells,
     * padding 0 20, knob column centred in the cell */
    b.removeFromTop (210);
    Rectangle<int> field = b.withTrimmedTop (20).reduced (20, 0);
    const int n = knobs.size();
    for (int i = 0; i < n; ++i)
    {
        const int x0 = field.getX() + i       * field.getWidth() / n;
        const int x1 = field.getX() + (i + 1) * field.getWidth() / n;
        const int kh = juce::jmin (knobs[i]->idealHeight(), field.getHeight());
        const int kw = juce::jmin (x1 - x0, 160);
        knobs[i]->setBounds (x0 + (x1 - x0 - kw) / 2,
                             field.getY() + (field.getHeight() - kh) / 2,
                             kw, kh);
    }
}

void SurvivorPanel::sync()
{
    const int st = atomic_load (&bb.loop_status);
    arm.setToggleStateQuiet (st == LOOP_ARMED || st == LOOP_RECORDING);
    play.setToggleStateQuiet (st == LOOP_PLAYING);

    const int bars = juce::jlimit (1, 4, atomic_load (&bb.loop_bars));
    arm.setSubLine (st == LOOP_ARMED     ? U8 ("ARMED \xc2\xb7 ") + juce::String (bars)
                                             + (bars == 1 ? " BAR" : " BARS")
                  : st == LOOP_RECORDING ? "CAPTURING"
                                         : "IDLE");
    play.setSubLine (st == LOOP_PLAYING ? "LOOPING" : "LOOP IDLE");

    if (! knobs[0]->isUserDragging()) knobs[0]->setValueQuiet (atomic_load (&bb.loop_mix));
    if (! knobs[1]->isUserDragging()) knobs[1]->setValueQuiet (atomic_load (&bb.loop_feedback));
    if (! knobs[2]->isUserDragging()) knobs[2]->setValueQuiet (atomic_load (&bb.loop_overdub));
    if (! knobs[3]->isUserDragging()) knobs[3]->setValueQuiet (atomic_load (&bb.loop_rate));
    if (! knobs[4]->isUserDragging()) knobs[4]->setValueQuiet (atomic_load (&bb.loop_reverse));
    const int sl = atomic_load (&bb.loop_slice);
    int idx = 0;
    for (int i = 0; i < 5; ++i) if (sliceTable[i] == sl) idx = i;
    if (! knobs[5]->isUserDragging()) knobs[5]->setValueQuiet (idx);

    /* text values (spec section 9: HALF/REV/SLICE render text, not ints;
     * OD is a switch in the engine, so it reads OFF/ON) */
    knobs[2]->setValueText (knobs[2]->value() ? "ON" : "OFF");
    static const char* half[] = { "\xc2\xbd", "NORM", "2\xc3\x97" };
    knobs[3]->setValueText (juce::String::fromUTF8 (half[juce::jlimit (0, 2, knobs[3]->value())]));
    knobs[4]->setValueText (knobs[4]->value() ? "ON" : "OFF");
    static const char* sliceTxt[] = { "1/1", "1/2", "1/4", "1/8", "1/16" };
    knobs[5]->setValueText (sliceTxt[juce::jlimit (0, 4, knobs[5]->value())]);
}

void SurvivorPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::GROUND);                    // stage ground (HTML frame 05)
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "SURVIVOR",
                     U8 ("MASTER PHRASE LOOPER \xc2\xb7 CAPTURES PRE-MASTER BUS + TAILS"),
                     juce::String (SerialNo::SURVIVOR)
                         + U8 (" \xc2\xb7 ARM SNAPS TO NEXT BAR BOUNDARY"),
                     Badge::LIVE, "LIVE");

    const int st = atomic_load (&bb.loop_status);
    const unsigned frames = atomic_load (&bb.loop_frames);

    /* -------- row 1 (120): band + divider + data block + LOOP OUT ------- */
    Rectangle<int> row1 = b.removeFromTop (120);
    g.setColour (C::PANEL_ALT);
    g.fillRect (row1);
    g.setColour (C::HAIRLINE);
    g.fillRect (row1.getX(), row1.getBottom() - 1, row1.getWidth(), 1);

    const int dx = row1.getX() + 502;           // 16 + 3*(150+12)
    g.setColour (C::HAIRLINE);
    g.fillRect (dx, row1.getCentreY() - 38, 1, 76);

    /* LOOP OUT label above the meter (8px .12em INK_FAINT, right-aligned) */
    const int meterX = row1.getRight() - 16 - 140;
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.12f));
    g.drawText ("LOOP OUT", Rectangle<int> (meterX, row1.getCentreY() - 11, 140, 10),
                Justification::centredRight);

    /* 3-line data block: label INK_FAINT + gap 10 + value (9px .10em) */
    const int dataX = dx + 13;                  // divider + gap 12
    const int dataW = juce::jmax (0, meterX - 12 - dataX);
    const juce::Font dataFont = Type::mono (9.0f, 0.10f);

    auto dataLine = [&] (int rowIdx, const juce::String& label,
                         const juce::String& value, juce::Colour valueFg)
    {
        Rectangle<int> line (dataX, row1.getCentreY() - 24 + rowIdx * 18, dataW, 12);
        g.setFont (dataFont);
        g.setColour (C::INK_FAINT);
        g.drawText (label, line, Justification::centredLeft);
        const int lw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (dataFont, label));
        g.setColour (valueFg);
        g.drawText (value, line.withTrimmedLeft (lw + 10), Justification::centredLeft);
    };

    int rate = atomic_load (&bb.rate);
    if (rate < 1) rate = 44100;

    if (frames > 0)
    {
        const int bars = juce::jlimit (1, 4, atomic_load (&bb.loop_bars));
        dataLine (0, "BUFFER",
                  juce::String (bars) + (bars == 1 ? " BAR" : " BARS")
                      + U8 (" \xc2\xb7 ") + juce::String ((double) frames / rate, 3) + " s"
                      + U8 (" \xc2\xb7 ") + groupedInt (frames) + " SMP",
                  C::INK);
    }
    else
        dataLine (0, "BUFFER", U8 ("\xe2\x80\x94 EMPTY \xe2\x80\x94"), C::INK_FAINT);

    dataLine (1, "SOURCE", U8 ("PRE-MASTER \xc2\xb7 8 LAYERS + SAMPLER + FX TAILS"),
              C::INK_DIM);

    if (st == LOOP_ARMED)
    {
        /* countdown to the capture bar, from the published loop clock */
        int bpm   = atomic_load (&bb.gctl[GCTL_BPM]);   if (bpm   < 1) bpm   = 90;
        int beats = atomic_load (&bb.gctl[GCTL_BEATS]); if (beats < 1) beats = 4;
        unsigned beatLen = (unsigned) (((long) rate * 60L) / bpm);
        if (beatLen < 1) beatLen = 1;
        const unsigned barLen = beatLen * (unsigned) beats;
        const unsigned remain = barLen - (atomic_load (&bb.k) % barLen);
        dataLine (2, "CAPTURE",
                  "AT BAR " + juce::String (atomic_load (&bb.bar) + 1u).paddedLeft ('0', 3)
                      + U8 (" \xc2\xb7 IN ") + juce::String ((double) remain / rate, 1) + " s",
                  C::BLOOD_HOT);
    }
    else if (st == LOOP_RECORDING)
        dataLine (2, "CAPTURE", "CAPTURING", C::BLOOD_HOT);
    else if (st == LOOP_PLAYING)
        dataLine (2, "CAPTURE", "PLAYING", C::INK_DIM);
    else
        dataLine (2, "CAPTURE", "IDLE", C::INK_FAINT);

    /* -------- row 2 (210): loop buffer waveform in a SOCKET box --------- */
    Rectangle<int> row2 = b.removeFromTop (210);

    int slice = atomic_load (&bb.loop_slice);
    if (slice != 1 && slice != 2 && slice != 4 && slice != 8 && slice != 16)
        slice = 1;

    Rectangle<int> lr = row2.removeFromTop (20);
    paintLabelRow (g, lr, U8 ("LOOP BUFFER \xc2\xb7 CAPTURED PHRASE"),
                   "SLICE GRID 1/" + juce::String (slice) + " SHOWN");
    g.setColour (C::HAIRLINE);
    g.fillRect (lr.getX(), lr.getBottom() - 1, lr.getWidth(), 1);
    g.fillRect (row2.getX(), row2.getBottom() - 1, row2.getWidth(), 1);

    Rectangle<int> box = row2.withTrimmedBottom (1).reduced (10);
    g.setColour (C::SOCKET);
    g.fillRect (box);
    g.setColour (C::HAIRLINE_DIM);
    g.drawRect (box, 1);

    Rectangle<int> inner = box.reduced (1);

    /* waveform: the real loop buffer, one point per 2px (HTML stroke
     * #c9c4b8, no token -- literal from the frame) */
    if (frames > 0)
    {
        {
            const int16_t* buf = bb_engine_loop_buffer();
            const float cy = (float) inner.getCentreY();
            const float half = inner.getHeight() / 2.0f - 2.0f;
            juce::Path p;
            bool first = true;
            for (int x = 0; x <= inner.getWidth(); x += 2)
            {
                const unsigned i = (unsigned) ((juce::uint64) x * frames
                                               / (unsigned) juce::jmax (1, inner.getWidth()));
                const float s = (float) buf[juce::jmin (i, frames - 1u)] / 32768.0f;
                const float px = (float) (inner.getX() + x);
                const float py = cy - s * half;
                if (first) { p.startNewSubPath (px, py); first = false; }
                else         p.lineTo (px, py);
            }
            g.setColour (juce::Colour (0xffc9c4b8));
            g.strokePath (p, juce::PathStrokeType (1.0f));
        }
    }

    /* centre-zero line, slice grid, loop position (HTML paint order) */
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (inner.getX(), inner.getCentreY(), inner.getWidth(), 1);
    g.setColour (C::HAIRLINE);
    for (int i = 1; i < slice; ++i)
        g.fillRect (inner.getX() + i * inner.getWidth() / slice,
                    inner.getY(), 1, inner.getHeight());
    if (frames > 0)
    {
        const unsigned pos = atomic_load (&bb.loop_pos) % frames;
        g.setColour (C::BLOOD_HOT);
        g.fillRect (inner.getX() + (int) ((juce::uint64) pos * (unsigned) inner.getWidth() / frames),
                    inner.getY(), 1, inner.getHeight());
    }

    /* -------- row 3 (flex): label row over the six 76px knobs ----------- */
    Rectangle<int> lr3 = b.removeFromTop (20);
    paintLabelRow (g, lr3, "LOOP CONTROL",
                   U8 ("MIX \xc2\xb7 FB \xc2\xb7 OD \xc2\xb7 HALF \xc2\xb7 REV \xc2\xb7 SLICE"));
    g.setColour (C::HAIRLINE);
    g.fillRect (lr3.getX(), lr3.getBottom() - 1, lr3.getWidth(), 1);
}

} // namespace morgue
