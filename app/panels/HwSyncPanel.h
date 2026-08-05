/* HwSyncPanel.h -- HW / SYNC: MIDI input (spec section 12, serial N.72-0424).
 *
 * Everything on this panel is live. What it does, exactly:
 *   note on  -> bb_engine_note_on  (focused voice, RACK owns focus)
 *   note off -> bb_engine_note_off (focused voice)
 *   CC 1     -> bb_engine_cc       (mod wheel, scaled 0-127 -> 0-255, into p0
 *                                   of the focused voice; engine.c:954 drops
 *                                   every other controller on purpose)
 *
 * WHAT WAS REMOVED IN THE LEGIBILITY PASS, and why it is not coming back as
 * a greyed-out version of itself:
 *   - the 10x14 CC MATRIX. 140 intersections of which exactly one was ever
 *     filled, eight of ten source cells blank, nine of ten value gutters
 *     drawing an empty bar and no number. It was not clickable -- this panel
 *     has no mouseDown -- so it was a large expensive-looking grid carrying
 *     one number. That one number is now a readable row.
 *   - CLK IN / CLK OUT / MIDI OUT plates. Painted in the ordinary idle plate
 *     style, wired to nothing. Nothing in the engine reads MIDI clock.
 *   - "FOOTSWITCH -> ARM" / "24 PPQN" footnotes, describing that clock.
 *   - "RIGHT-CLICK ANY KNOB -> LEARN - SAVED IN SESSION". Both halves false:
 *     EngravedKnob::onLearnRequest has no subscriber anywhere in the app and
 *     there is no learn state to save.
 *
 * Layout:
 *   header band 24
 *   INPUT block  -- device combo + ENABLE plate, live link state, telemetry
 *   ROUTING      -- what MIDI does here, in full-size type, and the live p0
 *                   value of the focused voice with a real bar behind it.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "Theme.h"
#include "Primitives.h"

namespace morgue
{

class HwSyncPanel : public juce::Component,
                    public juce::MidiInputCallback,
                    private juce::Timer
{
public:
    HwSyncPanel();
    void resized() override;
    void paint (juce::Graphics&) override;

    void handleIncomingMidiMessage (juce::MidiInput* src,
                                    const juce::MidiMessage& msg) override;

    /* Main wires this to rack.focusedLayer() so notes/CC hit the focused
     * voice (AGENTS.md contract). Defaults to layer 0. */
    std::function<int()> focusProvider;

private:
    void timerCallback() override;
    void openSelectedDevice();

    juce::ComboBox deviceBox;
    PlateButton enable;
    std::unique_ptr<juce::MidiInput> midiInput;
    juce::Array<juce::MidiDeviceInfo> midiNames;

    // live message telemetry (UI-thread reads, MIDI-thread writes)
    std::atomic<int> lastNote { -1 }, lastNoteVel { 0 };
    std::atomic<int> lastCc { -1 }, lastCcVal { 0 };
    std::atomic<juce::uint32> lastMsgMs { 0 };
};

} // namespace morgue
