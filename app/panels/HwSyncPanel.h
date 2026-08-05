/* HwSyncPanel.h -- HW / SYNC: MIDI in, CC routing (spec section 12,
 * serial N.72-0424, LIVE input; CC matrix + clock are R8 PLANNED).
 *
 * Live: device open/start/stop, notes -> bb_engine_note_on/off of the
 * focused voice, CC -> bb_engine_cc (engine maps CC to p0). The focused
 * voice is supplied by Main via focusProvider (RACK owns focus).
 *
 * Layout (spec section 12, HTML frame "08 HW SYNC"):
 *   header band 24
 *   row 1 (104): left INPUT DEVICE combo + 90x26 ENABLE, telemetry line;
 *                right 340 CLOCK/OUT block (R8 PLANNED, painted)
 *   row 2 (flex): 20px label row, 24px matrix header, 10 equal-flex rows.
 * Only the CC 001 -> p0 mapping is live today; every other matrix cell is
 * drawn in the exact unmapped state. Right-click learn is R8 (not built).
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
