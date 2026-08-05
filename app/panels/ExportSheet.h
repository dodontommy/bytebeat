/* ExportSheet.h -- STEM EXPORT, tab 8 (spec section 11, serial N.72-0426,
 * R6 PLANNED).
 *
 * A modal sheet 720 wide, centred over the dimmed console (console at 22%
 * opacity under a 72% GROUND scrim -- no blur). Entirely a pixel drawing of
 * the planned state: no real rendering exists in the engine. Checkbox
 * toggling and the RANGE selection are local UI state only; FORMAT is
 * fixed on WAV 24 (FLAC / MP3 drawn disabled); RENDER performs nothing
 * (R6 planned). CANCEL -- or a click on the scrim -- fires onCancel, which
 * Main.cpp wires to restore the previous tab.
 *
 * Main.cpp opens/closes the sheet via open()/close() (or setVisible):
 * the EXPORT tab and the MIXER-context EXPORT... transport button
 * (spec section 13) both lead here.
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

    /* show/hide API for Main.cpp (tab 8 select, MIXER EXPORT... button) */
    void open()   { setVisible (true); toFront (false); }
    void close()  { setVisible (false); }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

private:
    static constexpr int kTracks = 12;

    bool checked[kTracks];      // local UI state: stems to render
    int  rangeSel = 0;          // 0 LOOP / 1 SONG / 2 SEL (local UI state)

    /* geometry -- everything derives from the fixed 720x336 sheet */
    juce::Rectangle<int> sheetBounds() const;
    juce::Rectangle<int> trackRowBounds (int i) const;
    juce::Rectangle<int> rangeSegBounds (int i) const;
    juce::Rectangle<int> formatSegBounds (int i) const;
    juce::Rectangle<int> tailFieldBounds() const;
    juce::Rectangle<int> destFieldBounds() const;
    juce::Rectangle<int> cancelBounds() const;
    juce::Rectangle<int> renderBounds() const;
    int rightPaneX() const;
    int contentY() const;
};

} // namespace morgue
