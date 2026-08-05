/* LicksPanel.h -- GRAIN LICKS, the R1 step sampler (spec section 7,
 * serial N.72-0419, LIVE).
 *
 * 8 one-shot slots, 16-step patterns on the engine's clock (bb.seq_pos),
 * rendered to the master bus. Layout per spec section 7:
 *   toolbar 28  -- PATTERN A/B/C/D plates, FILL EUCLID / RAND / CLEAR, hints
 *   header 22   -- 300 gutter, 16 numbered columns (current tinted), 140 gutter
 *   8 slot rows -- equal flex: row head 300 (index 16, name + meta, M/S 18x18,
 *                  divider, three 26px knobs PIT/VEL/LVL), 16-column StepCell
 *                  grid with 3px padding, right gutter 140 (CHOKE tag + meter)
 *
 * Everything writes the bb.sampler atomics; the engine is the source of
 * truth and sync() (30 Hz, driven by MainComponent) pulls state back with
 * isUserDragging guards. Keys 1-8 focus a slot.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Primitives.h"

class AudioEngine;

namespace morgue
{

class LicksPanel : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   public juce::DragAndDropTarget
{
public:
    explicit LicksPanel (AudioEngine& a);
    ~LicksPanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void sync();                                   // 30 Hz engine pull (MainComponent)
    bool keyPressed (const juce::KeyPress&) override;

    // "DOUBLE-CLICK OR DRAG TO LOAD" -- files dropped onto a slot row
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    // internal drags (LOCKER "DRAG -> SLOT") whose description is an absolute path
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class ToolTag;      // toolbar pattern/fill tag, exact HTML colours
    class LickSlotRow;  // one slot: head 300 / 16 StepCells / gutter 140

    void focusSlot (int s);
    void loadFile (int s);                         // async chooser -> loadPath
    void loadPath (int s, const juce::File&);      // decode + publish to the engine
    void clearSlot (int s);
    void fillEuclid();                             // gen_euclid on the focused slot
    void fillRand();
    void fillClear();
    int  slotAtY (int y) const;
    void layoutToolbar();

    AudioEngine& audio;
    juce::OwnedArray<LickSlotRow> rows;
    juce::OwnedArray<ToolTag> patternTags;         // A live; B-D drawn only (engine
                                                   //   holds a single pattern bank)
    juce::OwnedArray<ToolTag> fillTags;            // EUCLID / RAND / CLEAR
    juce::StringArray names, metas;                // UI-side slot identity
    std::unique_ptr<juce::FileChooser> chooser;
    bool chooserOpen = false;
    int  slot = 0;                                 // focused slot (keys 1-8)
    int  lastPlay = -1;                            // bb.seq_pos as of last sync
    int  euclidK[8] { };                           // per-slot euclid density memory

    juce::Rectangle<int> toolbarRect, stepHeaderRect, rowsRect;
    juce::Rectangle<int> patLabelR, fillLabelR, tbDividerR;
};

} // namespace morgue
