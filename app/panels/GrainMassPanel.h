/* GrainMassPanel.h -- GRAIN MASS, the 4-well sampler (spec section 8,
 * serial N.72-0420, LIVE).
 *
 * 2x2 grid of SampleWell components separated by 1px HAIRLINE. Each well
 * loads a real file into its SamplerVoice (AudioEngine) and plays it over
 * the engine. Keys: 1-4 select, P play/stop, A/Z pitch, R reverse, O loop.
 * Double-click a well (or drop an audio file on it) to load.
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

private:
    class SampleWell;

    void timerCallback() override;
    void select (int well);                     // focus a well (keys act on it)
    void loadInto (int well);                   // async chooser -> SamplerVoice
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
