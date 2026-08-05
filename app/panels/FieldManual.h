/* FieldManual.h -- the full-window field manual overlay (spec section 14).
 *
 * Opaque #080807 sheet: 64px stencil header, 2-column grid of 12 zone
 * cards, GOLDEN RULES + KEY REFERENCE right column (400), 26px footer.
 * Toggled by '?' / F1 / the transport ? plate; ESC dismisses (handled in
 * Main). Static content, no animation. Swallows all mouse input; any
 * click dismisses.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Primitives.h"

namespace morgue
{

class FieldManualOverlay : public juce::Component
{
public:
    FieldManualOverlay();
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    /* show/hide for Main.cpp (equivalent to setVisible, kept explicit). */
    void show()  { setVisible (true); toFront (true); }
    void hide()  { setVisible (false); }

    std::function<void()> onDismiss;
};

} // namespace morgue
