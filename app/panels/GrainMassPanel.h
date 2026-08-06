/* GrainMassPanel.h -- GRAIN MASS, the 4-well sampler (spec section 8,
 * serial N.72-0420, LIVE).
 *
 * 2x2 grid of SampleWell components separated by 1px HAIRLINE. Each well
 * decodes a real file on the message thread and publishes it to the engine's
 * well pool (bb_engine_well_set); the audio is summed INSIDE bb_engine_render,
 * so REC records it and SURVIVOR loops it. Keys: 1-4 select, P play/stop, A/Z
 * pitch, R reverse, O loop. Double-click a well, drop an audio file on it, or
 * drag one out of the LOCKER to load.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Primitives.h"
#include <array>
#include <memory>

class AudioEngine;

namespace morgue
{

class GrainMassPanel : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       public juce::DragAndDropTarget,
                       private juce::Timer
{
public:
    explicit GrainMassPanel (AudioEngine& e);
    ~GrainMassPanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /* The LOCKER's internal drag. GRAIN LICKS has always accepted one; the
     * wells accepted only a drag from the desktop, so the same file behaved
     * differently depending on which browser you dragged it out of. */
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class SampleWell;

    void timerCallback() override;
    void select (int well);                     // focus a well (keys act on it)
    void loadInto (int well);                   // async chooser -> engine pool
    void loadFileInto (int well, const juce::File&);
    juce::Rectangle<int> gridArea() const;
    juce::Rectangle<int> wellRect (int i) const;
    int slotAt (juce::Point<int>) const;

    AudioEngine& audio;
    int slot = 0;                               // selected well
    PlateButton playAll { "PLAY ALL", true,  true  };   // lamp = armed
    PlateButton stopAll { "STOP ALL", false, false };
    std::unique_ptr<juce::FileChooser> chooser;
    std::array<std::unique_ptr<SampleWell>, 4> wells;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainMassPanel)
};

} // namespace morgue
