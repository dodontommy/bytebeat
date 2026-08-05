/* SurvivorPanel.h -- the master phrase looper (spec section 9,
 * serial N.72-0421, LIVE).
 *
 * Fully engine-wired: bb_engine_loop_command (ARM/PLAY/CLEAR) and the
 * loop_mix/feedback/overdub/rate/reverse/slice atomics with the
 * {1,2,4,8,16} slice table. CLEAR is momentary (never latches).
 *
 * Layout (spec section 9 / HTML frame 05):
 *   header band 24
 *   row 1 (120): three 150x74 stencil buttons - divider - BUFFER/SOURCE/
 *                CAPTURE data block - right: LOOP OUT meter 140x8
 *   row 2 (210): loop buffer waveform in a SOCKET box, slice grid,
 *                BLOOD_HOT loop position
 *   row 3 (flex): six 76px EngravedKnobs MIX/FB/OD/HALF/REV/SLICE
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Primitives.h"

namespace morgue
{

class SurvivorPanel : public juce::Component, private juce::Timer
{
public:
    SurvivorPanel();
    void resized() override;
    void sync();
    void paint (juce::Graphics&) override;

private:
    /* repaint-only: the live loop waveform/playhead redraw (like Scope).
     * The 30 Hz engine pull is driven solely by MainComponent -> sync(). */
    void timerCallback() override { repaint(); }

    PlateButton arm, play, clearBtn;
    MeterComponent loopOutMeter;                // 140x8, real loop-out peak
    juce::OwnedArray<EngravedKnob> knobs;       // MIX FB OD HALF REV SLICE
};

} // namespace morgue
