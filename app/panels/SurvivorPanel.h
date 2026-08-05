/* SurvivorPanel.h -- the master phrase looper (spec section 9,
 * serial N.72-0421, LIVE).
 *
 * Fully engine-wired: bb_engine_loop_command (ARM/PLAY/CLEAR) and the
 * loop_mix/feedback/overdub/rate/reverse/slice atomics with the
 * {1,2,4,8,16} slice table. CLEAR is momentary (never latches).
 *
 * Layout (spec section 9 / HTML frame 05):
 *   header band 24
 *   row 1 (120): three 150x74 stencil buttons - divider - LOOP STATE block
 *                - right: LOOP OUT meter 140x8
 *   row 2 (210): loop buffer waveform in a SOCKET box, slice grid,
 *                BLOOD_HOT loop position
 *   row 3 (flex): six 76px EngravedKnobs MIX/FB/OD/HALF/REV/SLICE
 *
 * The LOOP STATE block is the point of the legibility pass here. This panel
 * is played live, by glance, and the state used to be the third value on the
 * third line of a 9px key/value block: IDLE / ARMED / RECORDING / PLAYING is
 * now a stencil word as tall as the buttons next to it, with the detail
 * (bar counts, the countdown to the capture bar, the buffer length) under
 * it and a lamp beside it. The word carries the state; the lamp only
 * confirms it, so nothing here depends on telling two hues apart.
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
