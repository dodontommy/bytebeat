/* HwSyncPanel.cpp -- see HwSyncPanel.h. The MidiInput open/start/stop
 * lifecycle, engine note/CC calls and the enable-failure fallback are
 * unchanged. Every figure drawn here is read from a live source: the link
 * state from midiInput, LAST NOTE / LAST CC from the MIDI thread, and the p0
 * bar straight out of bb.layer[focus].param[0]. */

#include "HwSyncPanel.h"
#include "bytebeat.h"
#include "engine.h"

#include <cmath>            // std::ceil, used by the local textW helper
#include <memory>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    constexpr int kInputH  = 110;   // INPUT block incl. its bottom hairline
    constexpr int kLabelRH = 20;    // label rows

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
    /* Do not name a platform MIDI stack here. This build runs on Windows and
     * the old copy said "CoreMIDI"; the panel simply opens whatever
     * juce::MidiInput::getAvailableDevices() hands back, on any platform. */
    deviceBox.setTooltip (U8 ("INPUT DEVICE \xe2\x80\x94 the MIDI input port to open. "
                              "Notes play the focused voice; CC 1 writes its p0."));
    deviceBox.onChange = [this]
    {
        if (enable.getToggleState())
            openSelectedDevice();
    };
    addAndMakeVisible (&deviceBox);

    enable.setTooltip (U8 ("ENABLE \xe2\x80\x94 opens the selected MIDI input. "
                           "Notes \xe2\x86\x92 focused voice \xc2\xb7 CC 1 \xe2\x86\x92 p0."));
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
    /* MIDI input thread: read the focused layer from the engine's own atomic
     * (bb.focus, kept current by RackPanel::selectLayer). Calling
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
    Rectangle<int> block = b.removeFromTop (kInputH);
    block.removeFromBottom (1);                    // row hairline
    Rectangle<int> left = block.reduced (12);
    left.removeFromTop (Type::rowH (10.0f) + 8);   // INPUT DEVICE label + gap
    Rectangle<int> field = left.removeFromTop (28);
    field = field.withWidth (juce::jmin (field.getWidth(), 520));
    enable.setBounds (field.removeFromRight (96));
    field.removeFromRight (8);
    deviceBox.setBounds (field);
}

void HwSyncPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "HW / SYNC",
                     U8 ("MIDI IN \xc2\xb7 NOTES + MOD WHEEL"),
                     juce::String (SerialNo::HWSYNC),
                     Badge::LIVE, "LIVE");

    const bool linkOpen = midiInput != nullptr;

    /* ---- INPUT block ---------------------------------------------------- */
    Rectangle<int> block = b.removeFromTop (kInputH);
    g.setColour (C::HAIRLINE);
    g.fillRect (block.getX(), block.getBottom() - 1, block.getWidth(), 1);
    block.removeFromBottom (1);

    Rectangle<int> left = block.reduced (12);
    g.setColour (C::INK_DIM);
    g.setFont (Type::label());
    g.drawText ("INPUT DEVICE", left.removeFromTop (Type::rowH (10.0f)),
                Justification::centredLeft);
    left.removeFromTop (8 + 28 + 10);              // gap, field row, gap

    /* LINK STATE. State is a word first and a colour second -- the old panel
     * had nothing at all that said whether the port was actually open, and a
     * lamp on its own would fail in greyscale (Theme.h colour-blind rule). */
    {
        Rectangle<int> line = left.removeFromTop (Type::rowH (10.0f));
        const juce::Font f = Type::micro();
        g.setFont (f);

        int x = line.getX();
        auto seg = [&g, &f, &line, &x] (const juce::String& t, juce::Colour c)
        {
            g.setColour (c);
            g.drawText (t, Rectangle<int> (x, line.getY(),
                                           juce::jmax (0, line.getRight() - x),
                                           line.getHeight()),
                        Justification::centredLeft);
            x += textW (f, t);
        };

        const juce::String state = linkOpen ? "LINK OPEN" : "LINK CLOSED";
        g.setColour (linkOpen ? C::GREEN_FAINT : C::LAMP_DEAD);
        g.fillRect (x, line.getCentreY() - 3, 6, 6);
        x += 6 + 8;
        seg (state, linkOpen ? C::GREEN_FAINT : C::INK_FAINT);
        x += 18;

        seg ("CH ", C::INK_FAINT);
        seg ("ALL", C::INK_DIM);
        x += 18;

        const int note = lastNote.load();
        seg ("LAST NOTE ", C::INK_FAINT);
        if (note >= 0)
            seg (juce::String (noteName (note)) + juce::String (note / 12 - 1)
                     + " v" + juce::String (lastNoteVel.load()),
                 C::INK_DIM);
        else
            seg (U8 ("\xe2\x80\x94"), C::INK_DIM);
        x += 18;

        const int cc = lastCc.load();
        seg ("LAST CC ", C::INK_FAINT);
        if (cc >= 0)
            seg (juce::String (cc).paddedLeft ('0', 3) + " v"
                     + juce::String (lastCcVal.load()),
                 C::INK_DIM);
        else
            seg (U8 ("\xe2\x80\x94"), C::INK_DIM);
        x += 18;

        const bool streaming = linkOpen
            && lastMsgMs.load() != 0
            && juce::Time::getMillisecondCounter() - lastMsgMs.load() < 2000;
        if (streaming)
            seg ("STREAM OK", C::GREEN_FAINT);
    }

    /* ---- ROUTING: the two things MIDI actually does --------------------- */
    Rectangle<int> lr = b.removeFromTop (kLabelRH);
    paintLabelRow (g, lr, "ROUTING",
                   U8 ("NOTES + CC 001 \xc2\xb7 FIXED ROUTING"));
    g.setColour (C::HAIRLINE);
    g.fillRect (lr.getX(), lr.getBottom() - 1, lr.getWidth(), 1);

    const int focus = focusProvider ? juce::jlimit (0, BB_NLAYER - 1, focusProvider())
                                    : 0;
    const int p0 = juce::jlimit (0, 255, (int) atomic_load (&bb.layer[focus].param[0]));
    const juce::String voice = juce::String (focus + 1).paddedLeft ('0', 2);

    Rectangle<int> rows = b.reduced (12, 10);
    const int rowHeight = Type::rowH (11.0f) + 10;

    /* Row 1: notes. Row 2: the mod wheel, with the value it is driving right
     * now behind a real bar -- this is the one number the old 140-cell matrix
     * carried, at a size you can read from the keyboard. */
    auto routeRow = [&] (Rectangle<int> r, const juce::String& srcText,
                         const juce::String& dstText)
    {
        const juce::Font sf = Type::monoMedium (11.0f, 0.06f);
        const juce::Font df = Type::body();
        g.setFont (sf);
        g.setColour (C::INK);
        const int sw = juce::jmax (textW (sf, srcText) + 16, 132);
        g.drawText (srcText, r.removeFromLeft (sw), Justification::centredLeft);
        g.setColour (C::INK_GHOST);
        g.setFont (df);
        g.drawText (U8 ("\xe2\x86\x92"), r.removeFromLeft (22), Justification::centredLeft);
        g.setColour (C::INK_DIM);
        g.drawText (dstText, r, Justification::centredLeft, true);
    };

    routeRow (rows.removeFromTop (rowHeight),
              "NOTE ON / OFF",
              "plays the focused voice, VOICE " + voice
                  + U8 (" \xc2\xb7 velocity is the note velocity"));

    g.setColour (C::HAIRLINE_FAINT);
    g.fillRect (rows.getX(), rows.getY(), rows.getWidth(), 1);

    {
        Rectangle<int> r = rows.removeFromTop (rowHeight);
        Rectangle<int> bar = r.removeFromRight (juce::jmin (260, r.getWidth() / 3));
        routeRow (r, U8 ("CC 001 \xc2\xb7 MOD WHEEL"),
                  "writes p0 of VOICE " + voice);

        /* live p0 readout: number and bar, both from the engine atomic */
        Rectangle<int> num = bar.removeFromRight (44);
        g.setColour (C::INK);
        g.setFont (Type::data());
        g.drawText (juce::String (p0).paddedLeft ('0', 3), num,
                    Justification::centredRight);

        bar.removeFromRight (10);
        Rectangle<int> trough (bar.getX(), bar.getCentreY() - 4, bar.getWidth(), 8);
        g.setColour (C::TROUGH);
        g.fillRect (trough);
        g.setColour (C::HAIRLINE);
        g.drawRect (trough, 1);
        const int fw = (trough.getWidth() - 2) * p0 / 255;
        if (fw > 0)
        {
            g.setColour (C::BLOOD);
            g.fillRect (trough.getX() + 1, trough.getY() + 1, fw, 6);
        }
    }

    g.setColour (C::HAIRLINE_FAINT);
    g.fillRect (rows.getX(), rows.getY(), rows.getWidth(), 1);

    /* Row 3: the honest statement about every other controller. This is a
     * fact about the engine (engine.c:954 returns early for cc != 1), not a
     * roadmap note, so it is stated once in running type instead of being
     * drawn as 139 empty cells. */
    {
        Rectangle<int> r = rows.removeFromTop (rowHeight);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::body());
        g.drawText (U8 ("Every other controller is ignored, deliberately: a "
                        "sustain pedal or a bank-select sweep will not stomp "
                        "the patch."),
                    r, Justification::centredLeft, true);
    }
}

} // namespace morgue
