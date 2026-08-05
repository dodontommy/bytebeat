/* HwSyncPanel.cpp -- see HwSyncPanel.h. The MidiInput open/start/stop
 * lifecycle, engine note/CC calls and the enable-failure fallback are
 * preserved. LAST NOTE / LAST CC / STREAM OK are real telemetry; the value
 * gutter of the live CC 001 row reads the focused voice's p0 straight from
 * the engine. Everything else in the matrix and the CLOCK/OUT block is the
 * exact R8 planned/unmapped state from the spec -- no fake data. */

#include "HwSyncPanel.h"
#include "bytebeat.h"
#include "engine.h"

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

/* ---- geometry, spec section 12 / HTML frame 08 -------------------------- */
namespace
{
    constexpr int kRow1H   = 104;   // row 1 incl. its bottom hairline
    constexpr int kClockW  = 340;   // right CLOCK/OUT block
    constexpr int kLabelRH = 20;    // CC MATRIX label row
    constexpr int kHeadRH  = 24;    // matrix column header
    constexpr int kSrcW    = 150;   // SOURCE gutter
    constexpr int kValW    = 90;    // VALUE gutter
    constexpr int kNCols   = 14;    // p0-p7 DRIVE TONE CRUSH SPACE LEVEL LOOP MIX
    constexpr int kNRows   = 10;

    const char* colName (int j)
    {
        static const char* fixed[6] = { "DRIVE", "TONE", "CRUSH",
                                        "SPACE", "LEVEL", "LOOP MIX" };
        static char p[8][3] = { "p0", "p1", "p2", "p3", "p4", "p5", "p6", "p7" };
        return j < 8 ? p[j] : fixed[j - 8];
    }

    const char* noteName (int n)
    {
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        return n >= 0 ? names[n % 12] : "-";
    }

    int textW (const juce::Font& f, const juce::String& s)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
    }
} // namespace

HwSyncPanel::HwSyncPanel() : enable ("ENABLE", true)
{
    deviceBox.addItem ("NO DEVICE", 1);
    {
        const auto devices = juce::MidiInput::getAvailableDevices();
        for (int i = 0; i < devices.size(); ++i)
            deviceBox.addItem (devices[i].name, i + 2);   // id = device index + 2
        midiNames = devices;
    }
    deviceBox.setSelectedId (1, juce::dontSendNotification);
    deviceBox.setTooltip (U8 ("INPUT DEVICE \xe2\x80\x94 selects the CoreMIDI input "
                              "port. Notes trigger the focused voice, CC writes p0."));
    deviceBox.onChange = [this]
    {
        if (enable.getToggleState())
            openSelectedDevice();
    };
    addAndMakeVisible (&deviceBox);

    enable.setTooltip (U8 ("ENABLE \xe2\x80\x94 opens the selected MIDI input. "
                           "Notes \xe2\x86\x92 focused voice \xc2\xb7 CC \xe2\x86\x92 p0."));
    enable.onToggle = [this] (bool on)
    {
        if (! on)
        {
            midiInput.reset();
            return;
        }
        openSelectedDevice();
    };
    addAndMakeVisible (&enable);

    startTimerHz (15);   // telemetry line, STREAM OK expiry, live p0 bar
}

void HwSyncPanel::openSelectedDevice()
{
    midiInput.reset();
    const int idx = deviceBox.getSelectedId() - 2;
    if (idx >= 0 && idx < midiNames.size())
    {
        juce::String ident = midiNames[idx].identifier;
        midiInput = juce::MidiInput::openDevice (ident, this);
        if (midiInput == nullptr)
        {
            enable.setToggleStateQuiet (false);
            return;
        }
        midiInput->start();
    }
    else
    {
        enable.setToggleStateQuiet (false);
    }
}

void HwSyncPanel::handleIncomingMidiMessage (juce::MidiInput* src, const juce::MidiMessage& m)
{
    (void) src;
    /* CoreMIDI input thread: read the focused layer from the engine's own
     * atomic (bb.focus, kept current by RackPanel::selectLayer). Calling
     * focusProvider() here would read a plain int across threads --
     * cross-thread UI is atomics only. focusProvider stays for paint(). */
    const int target = bb_clampi (atomic_load (&bb.focus), 0, BB_NLAYER - 1);
    if (m.isNoteOn())
    {
        lastNote = m.getNoteNumber();
        lastNoteVel = m.getVelocity();
        bb_engine_note_on (target, m.getNoteNumber(), m.getVelocity());
    }
    else if (m.isNoteOff())
        bb_engine_note_off (target);
    else if (m.isController())
    {
        lastCc = m.getControllerNumber();
        lastCcVal = m.getControllerValue();
        bb_engine_cc (target, m.getControllerNumber(), m.getControllerValue());
    }
    lastMsgMs = juce::Time::getMillisecondCounter();
    // repaint is driven by the 15 Hz timer; no per-message message-thread spam
}

void HwSyncPanel::timerCallback()
{
    repaint();
}

void HwSyncPanel::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    Rectangle<int> row1 = b.removeFromTop (kRow1H);
    row1.removeFromBottom (1);                     // row hairline
    row1.removeFromRight (kClockW + 1);            // clock block + divider
    Rectangle<int> left = row1.reduced (10);
    left.removeFromTop (10 + 8);                   // INPUT DEVICE label + gap
    Rectangle<int> field = left.removeFromTop (26);
    enable.setBounds (field.removeFromRight (90));
    field.removeFromRight (6);
    deviceBox.setBounds (field);
}

void HwSyncPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "HW / SYNC",
                     U8 ("MIDI IN \xc2\xb7 CC MATRIX \xc2\xb7 CLOCK"),
                     juce::String (SerialNo::HWSYNC) + U8 (" \xc2\xb7 CORE MIDI"),
                     Badge::LIVE, "LIVE");

    /* ---- row 1 (104): INPUT DEVICE | CLOCK/OUT --------------------------- */
    Rectangle<int> row1 = b.removeFromTop (kRow1H);
    g.setColour (C::HAIRLINE);
    g.fillRect (row1.getX(), row1.getBottom() - 1, row1.getWidth(), 1);
    row1.removeFromBottom (1);

    Rectangle<int> clock = row1.removeFromRight (kClockW);
    g.setColour (C::HAIRLINE);
    g.fillRect (row1.getRight() - 1, row1.getY(), 1, row1.getHeight());
    row1.removeFromRight (1);

    // left: label / field row (components) / telemetry line
    Rectangle<int> left = row1.reduced (10);
    g.setColour (C::INK_DIM);
    g.setFont (Type::micro());
    g.drawText ("INPUT DEVICE", left.removeFromTop (10), Justification::centredLeft);
    left.removeFromTop (8 + 26 + 8);               // gap, field row, gap

    {
        Rectangle<int> data = left.removeFromTop (12);
        const juce::Font f = Type::mono (9.0f, 0.08f);
        g.setFont (f);
        int x = data.getX();
        auto seg = [&g, &f, &data, &x] (const juce::String& t, juce::Colour c)
        {
            g.setColour (c);
            g.drawText (t, Rectangle<int> (x, data.getY(),
                                           juce::jmax (0, data.getRight() - x),
                                           data.getHeight()),
                        Justification::centredLeft);
            x += textW (f, t);
        };

        const int note = lastNote.load();
        const int cc = lastCc.load();

        seg ("CH ", C::INK_FAINT);
        seg ("ALL", C::INK_DIM);
        x += 18;
        seg ("LAST NOTE ", C::INK_FAINT);
        if (note >= 0)
            seg (juce::String (noteName (note)) + juce::String (note / 12 - 1)
                     + " v" + juce::String (lastNoteVel.load()),
                 C::INK_DIM);
        else
            seg (U8 ("\xe2\x80\x94"), C::INK_DIM);
        x += 18;
        seg ("LAST CC ", C::INK_FAINT);
        if (cc >= 0)
            seg (juce::String (cc).paddedLeft ('0', 3) + U8 (" \xe2\x86\x92 ")
                     + juce::String (juce::jlimit (0, 255, lastCcVal.load() * 2)),
                 C::INK_DIM);
        else
            seg (U8 ("\xe2\x80\x94"), C::INK_DIM);
        x += 18;

        const bool streaming = midiInput != nullptr
            && lastMsgMs.load() != 0
            && juce::Time::getMillisecondCounter() - lastMsgMs.load() < 2000;
        if (streaming)
            seg ("STREAM OK", C::GREEN_FAINT);
    }

    // right 340: CLOCK/OUT, R8 planned (painted; nothing here is live)
    {
        Rectangle<int> ck = clock.reduced (10);
        Rectangle<int> lab = ck.removeFromTop (10);
        g.setColour (C::INK_DIM);
        g.setFont (Type::micro());
        g.drawText ("CLOCK / OUT", lab, Justification::centredLeft);
        const int tw = textW (Type::micro(), "CLOCK / OUT");
        const int bw = StatusBadge::idealWidth ("R8 PLANNED");
        StatusBadge::paintBadge (g,
                                 Rectangle<int> (lab.getX() + tw + 8,
                                                 lab.getCentreY() - 6, bw, 12),
                                 Badge::PARTIAL, "R8 PLANNED");

        ck.removeFromTop (8);
        Rectangle<int> pr = ck.removeFromTop (24);
        const char* plates[3] = { "CLK IN", "CLK OUT", "MIDI OUT" };
        const int cw = (pr.getWidth() - 8) / 3;    // three flex:1, gap 4
        for (int i = 0; i < 3; ++i)
        {
            const int x0 = pr.getX() + i * (cw + 4);
            const int x1 = (i == 2) ? pr.getRight() : x0 + cw;
            Rectangle<int> p (x0, pr.getY(), x1 - x0, pr.getHeight());
            g.setColour (C::PLATE_LOW);
            g.fillRect (p);
            g.setColour (C::HAIRLINE);
            g.drawRect (p, 1);
            g.setColour (C::TAB_INACTIVE_FG);
            g.setFont (Type::mono (9.0f));
            g.drawText (plates[i], p, Justification::centred);
        }

        ck.removeFromTop (8);
        Rectangle<int> fn = ck.removeFromTop (10);
        const juce::Font ff = Type::mono (8.0f, 0.08f);
        g.setFont (ff);
        g.setColour (C::INK_FAINT);
        int x = fn.getX();
        for (const char* note : { "FOOTSWITCH \xe2\x86\x92 ARM", "24 PPQN" })
        {
            const juce::String t = U8 (note);
            g.drawText (t, Rectangle<int> (x, fn.getY(),
                                           juce::jmax (0, fn.getRight() - x),
                                           fn.getHeight()),
                        Justification::centredLeft);
            x += textW (ff, t) + 14;
        }
    }

    /* ---- row 2 (flex): CC matrix ----------------------------------------- */
    Rectangle<int> lr = b.removeFromTop (kLabelRH);
    paintLabelRow (g, lr,
                   U8 ("CC MATRIX \xc2\xb7 ROWS = CC \xc2\xb7 COLUMNS = TARGET"),
                   U8 ("RIGHT-CLICK ANY KNOB \xe2\x86\x92 LEARN \xc2\xb7 SAVED IN SESSION"));
    g.setColour (C::HAIRLINE);
    g.fillRect (lr.getX(), lr.getBottom() - 1, lr.getWidth(), 1);

    // shared column geometry (integer-aligned)
    const int mx = b.getX();
    const int gx0 = mx + kSrcW;                   // first target column
    const int gx1 = b.getRight() - kValW;         // VALUE gutter
    auto colX = [gx0, gx1] (int j) { return gx0 + j * (gx1 - gx0) / kNCols; };

    // matrix header (24)
    {
        Rectangle<int> hd = b.removeFromTop (kHeadRH);
        g.setColour (C::PANEL);
        g.fillRect (hd);
        g.setColour (C::HAIRLINE);
        g.fillRect (hd.getX(), hd.getBottom() - 1, hd.getWidth(), 1);
        Rectangle<int> hi = hd.withTrimmedBottom (1);

        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.12f));
        g.drawText ("SOURCE", hi.withWidth (kSrcW).reduced (8, 0),
                    Justification::centredLeft);
        g.setColour (C::HAIRLINE);
        g.fillRect (gx0 - 1, hi.getY(), 1, hi.getHeight());

        g.setFont (Type::nano (7.0f));
        for (int j = 0; j < kNCols; ++j)
        {
            g.setColour (C::INK_DIM);
            g.drawText (colName (j),
                        Rectangle<int> (colX (j), hi.getY(),
                                        colX (j + 1) - colX (j), hi.getHeight()),
                        Justification::centred);
            g.setColour (C::HAIRLINE_DIM);
            g.fillRect (colX (j + 1) - 1, hi.getY(), 1, hi.getHeight());
        }

        g.setColour (C::HAIRLINE);
        g.fillRect (gx1, hi.getY(), 1, hi.getHeight());
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText ("VALUE",
                    Rectangle<int> (gx1 + 1, hi.getY(), kValW - 1, hi.getHeight())
                        .reduced (8, 0),
                    Justification::centredLeft);
    }

    // 10 equal-flex rows; only CC 001 -> p0 is live today
    const int focus = focusProvider ? juce::jlimit (0, BB_NLAYER - 1, focusProvider())
                                    : 0;
    const int p0 = juce::jlimit (0, 255, (int) atomic_load (&bb.layer[focus].param[0]));

    const int my = b.getY();
    const int mh = b.getHeight();
    auto rowY = [my, mh] (int i) { return my + i * mh / kNRows; };

    for (int i = 0; i < kNRows; ++i)
    {
        Rectangle<int> row (mx, rowY (i), b.getWidth(), rowY (i + 1) - rowY (i));
        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
        Rectangle<int> ri = row.withTrimmedBottom (1);

        // source cell
        Rectangle<int> src = ri.withWidth (kSrcW);
        g.setColour (C::PANEL);
        g.fillRect (src);
        g.setColour (C::HAIRLINE);
        g.fillRect (gx0 - 1, ri.getY(), 1, ri.getHeight());
        if (i == 0)
        {
            g.setColour (C::INK);
            g.setFont (Type::mono (9.0f));
            g.drawText ("CC 001", src.reduced (8, 0), Justification::centredLeft);
            g.setColour (C::INK_FAINT);
            g.setFont (Type::nano (7.0f));
            g.drawText ("MOD WHEEL", src.reduced (8, 0), Justification::centredRight);
        }
        else if (i == kNRows - 1)
        {
            g.setColour (C::INK_GHOST);
            g.setFont (Type::mono (9.0f));
            g.drawText (U8 ("\xe2\x80\x94 UNMAPPED / LEARN\xe2\x80\xa6"),
                        src.reduced (8, 0), Justification::centredLeft);
        }

        // intersection cells: 9x9 square, mapped = BLOOD/BLOOD_HOT
        for (int j = 0; j < kNCols; ++j)
        {
            g.setColour (C::HAIRLINE_FAINT);
            g.fillRect (colX (j + 1) - 1, ri.getY(), 1, ri.getHeight());

            const int cw = colX (j + 1) - colX (j);
            Rectangle<int> sq (colX (j) + (cw - 9) / 2,
                               ri.getY() + (ri.getHeight() - 9) / 2, 9, 9);
            if (i == 0 && j == 0)                          // the live mapping
            {
                g.setColour (C::BLOOD);
                g.fillRect (sq);
                g.setColour (C::BLOOD_HOT);
                g.drawRect (sq, 1);
            }
            else
            {
                g.setColour (C::HAIRLINE_DIM);
                g.drawRect (sq, 1);
            }
        }

        // value gutter: 5px bar + numeric (live row only carries data)
        g.setColour (C::HAIRLINE);
        g.fillRect (gx1, ri.getY(), 1, ri.getHeight());
        Rectangle<int> val (gx1 + 1 + 8, ri.getY(), kValW - 1 - 16, ri.getHeight());
        Rectangle<int> num = val.removeFromRight (18);
        val.removeFromRight (6);
        Rectangle<int> bar (val.getX(), ri.getCentreY() - 2, val.getWidth(), 5);
        g.setColour (C::PANEL);
        g.fillRect (bar);
        g.setColour (C::HAIRLINE);
        g.drawRect (bar, 1);
        if (i == 0)
        {
            const int fw = (bar.getWidth() - 2) * p0 / 255;
            g.setColour (C::BLOOD);
            g.fillRect (bar.getX() + 1, bar.getY() + 1, fw, 3);
            g.setColour (C::INK_DIM);
            g.setFont (Type::mono (8.0f));
            g.drawText (juce::String (p0).paddedLeft ('0', 3), num,
                        Justification::centredLeft);
        }
    }
}

} // namespace morgue
