/* ExportSheet.h -- STEM EXPORT, tab 9 (spec section 11, serial N.72-0426).
 *
 * WHAT THIS SHEET IS, AND WHY IT IS EMPTY
 *
 * The engine has no offline renderer. Not a slow one, not a partial one --
 * none. Everything that used to be drawn here (a twelve-row track list with
 * invented file sizes, a RANGE picker, a FORMAT picker with two permanently
 * disabled segments, a TAIL field that was a string literal in a box shaped
 * like an editable field, and a RENDER button painted in the reserved BLOOD
 * accent whose click handler returned immediately) was a picture of a feature
 * that does not exist. The sizes summed into a live-looking "N FILES / EST
 * NNN MB" readout that recomputed as you ticked boxes, so the fabrication
 * animated -- which is what made it read as real telemetry.
 *
 * There is no honest partial version of that. An export sheet's entire
 * content IS the promise, so a "planned" export sheet with greyed controls is
 * the same lie in a lighter ink. What is left is a notice: what the app
 * cannot do, what it can do instead today (REC writes a real WAV), and one
 * control -- CLOSE -- which works.
 *
 * Main.cpp opens/closes the sheet via open()/close() (or setVisible); the
 * EXPORT tab leads here. CANCEL, a scrim click, or ESC fire onCancel, which
 * Main.cpp wires to restore the previous tab. That contract is unchanged.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Primitives.h"

namespace morgue
{

class ExportSheet : public juce::Component,
                    public juce::SettableTooltipClient
{
public:
    ExportSheet();

    std::function<void()> onCancel;

    /* show/hide API for Main.cpp (tab select) */
    void open()   { setVisible (true); toFront (false); }
    void close()  { setVisible (false); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    /* The body is wrapped against real glyph widths, and the sheet is sized
     * to whatever that comes to, so paint() and the hit tests agree without
     * either of them guessing a height. */
    juce::StringArray bodyLines() const;         // "" entries are paragraph gaps
    juce::Rectangle<int> sheetBounds() const;
    juce::Rectangle<int> closeBounds() const;
};

} // namespace morgue
