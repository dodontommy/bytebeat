/* Primitives.cpp -- see Primitives.h. Frame 10 of the design handoff is the
 * authority for every colour and measurement here. All painting is
 * integer-aligned fillRect so hairlines never soften; no gradients, no
 * rounded corners, no shadows. */

#include "Primitives.h"

#include <cmath>
#include <utility>          // std::move

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

/* ======================================================================== */
/*  MorgueLookAndFeel                                                        */
/* ======================================================================== */

MorgueLookAndFeel::MorgueLookAndFeel()
{
    setColour (juce::TextEditor::backgroundColourId, C::SOCKET);
    setColour (juce::TextEditor::textColourId, C::INK);
    setColour (juce::TextEditor::outlineColourId, C::HAIRLINE);
    setColour (juce::TextEditor::focusedOutlineColourId, C::BLOOD);
    setColour (juce::TextEditor::highlightColourId, C::BLOOD.withAlpha (0.55f));
    setColour (juce::TextEditor::highlightedTextColourId, C::INK_BRIGHT);
    setColour (juce::CaretComponent::caretColourId, C::BLOOD_HOT);
    setColour (juce::ComboBox::backgroundColourId, C::SOCKET);
    setColour (juce::ComboBox::textColourId, C::INK);
    setColour (juce::ComboBox::outlineColourId, C::HAIRLINE);
    setColour (juce::ComboBox::arrowColourId, C::INK_DIM);
    setColour (juce::PopupMenu::backgroundColourId, C::PANEL);
    setColour (juce::PopupMenu::textColourId, C::INK_DIM);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, C::PLATE_HOVER);
    setColour (juce::PopupMenu::highlightedTextColourId, C::INK);
    setColour (juce::Label::textColourId, C::INK_DIM);
    setColour (juce::ListBox::backgroundColourId, C::PANEL);
    setColour (juce::ScrollBar::thumbColourId, C::EDGE);
    setColour (juce::TooltipWindow::backgroundColourId, C::RAISED);
    setColour (juce::TooltipWindow::textColourId, C::INK_DIM);
    setColour (juce::TooltipWindow::outlineColourId, C::HAIRLINE);
}

juce::Typeface::Ptr MorgueLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    // Anything JUCE tries to draw with a default face lands on Plex Mono.
    return Type::mono (f.getHeight()).getTypefacePtr();
}

void MorgueLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                              const juce::Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds();
    g.setColour (down || highlighted ? C::PLATE_HOVER : C::PLATE);
    g.fillRect (r);
    g.setColour (C::EDGE);
    g.drawRect (r, 1);
}

juce::Font MorgueLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return Type::mono (10.0f, 0.16f);
}

void MorgueLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool,
                                      int, int, int, int, juce::ComboBox& box)
{
    Rectangle<int> r (0, 0, w, h);
    g.setColour (C::SOCKET);
    g.fillRect (r);
    g.setColour (box.hasKeyboardFocus (false) ? C::EDGE : C::HAIRLINE);
    g.drawRect (r, 1);

    // the small down triangle, drawn as three shrinking 1px rules
    int ax = w - 14, ay = h / 2 - 1;
    g.setColour (C::INK_DIM);
    g.fillRect (ax,     ay,     7, 1);
    g.fillRect (ax + 1, ay + 1, 5, 1);
    g.fillRect (ax + 2, ay + 2, 3, 1);
    g.fillRect (ax + 3, ay + 3, 1, 1);
}

juce::Font MorgueLookAndFeel::getComboBoxFont (juce::ComboBox&) { return Type::mono (10.0f); }

void MorgueLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (6, 1, box.getWidth() - 24, box.getHeight() - 2);
    label.setFont (Type::mono (10.0f));
}

void MorgueLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.setColour (C::PANEL);
    g.fillRect (0, 0, w, h);
    g.setColour (C::HAIRLINE);
    g.drawRect (0, 0, w, h, 1);
}

void MorgueLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const Rectangle<int>& area,
                                           bool isSeparator, bool isActive, bool isHighlighted,
                                           bool isTicked, bool, const juce::String& text,
                                           const juce::String&, const juce::Drawable*,
                                           const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (C::HAIRLINE_DIM);
        g.fillRect (area.getX() + 4, area.getCentreY(), area.getWidth() - 8, 1);
        return;
    }
    if (isHighlighted && isActive)
    {
        g.setColour (C::PLATE_HOVER);
        g.fillRect (area);
    }
    g.setColour (! isActive ? C::INK_GHOST
                : isTicked  ? C::BLOOD_HOT
                : isHighlighted ? C::INK : C::INK_DIM);
    g.setFont (Type::mono (10.0f));
    g.drawText (text, area.reduced (10, 0), Justification::centredLeft);
}

juce::Font MorgueLookAndFeel::getPopupMenuFont() { return Type::mono (10.0f); }

void MorgueLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (C::SOCKET);
    g.fillRect (0, 0, w, h);
}

void MorgueLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& te)
{
    g.setColour (te.hasKeyboardFocus (true) ? C::BLOOD : C::HAIRLINE);
    g.drawRect (0, 0, w, h, 1);
}

void MorgueLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y,
                                       int w, int h, bool vertical, int thumbStart,
                                       int thumbSize, bool mouseOver, bool mouseDown)
{
    g.setColour (C::SOCKET);
    g.fillRect (x, y, w, h);
    g.setColour (mouseDown || mouseOver ? C::INK_FAINT : C::EDGE);
    if (vertical) g.fillRect (x + 2, thumbStart, w - 4, thumbSize);
    else          g.fillRect (thumbStart, y + 2, thumbSize, h - 4);
}

void MorgueLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    g.setColour (C::RAISED);
    g.fillRect (0, 0, w, h);
    g.setColour (C::HAIRLINE);
    g.drawRect (0, 0, w, h, 1);
    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (10.0f));
    g.drawFittedText (text, Rectangle<int> (0, 0, w, h).reduced (8, 4),
                      Justification::centredLeft, 6);
}

juce::Rectangle<int> MorgueLookAndFeel::getTooltipBounds (const juce::String& text,
                                                          juce::Point<int> screenPos,
                                                          juce::Rectangle<int> parentArea)
{
    const juce::Font f = Type::mono (10.0f);
    int w = juce::jmin (360, (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, text)) + 18);
    int lines = juce::jmax (1, (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, text) / (float) (w - 18)));
    int h = 8 + lines * 13;
    return Rectangle<int> (screenPos.x + 12, screenPos.y + 18, w, h)
             .constrainedWithin (parentArea);
}

/* ======================================================================== */
/*  EngravedKnob                                                             */
/* ======================================================================== */

EngravedKnob::EngravedKnob (juce::String label, int diameterPx, int min, int max, int defaultValue)
    : labelText (std::move (label)), dia (diameterPx), defaultVal (defaultValue)
{
    setName (labelText);
    setSliderStyle (juce::Slider::RotaryHorizontalDrag);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setRange ((double) min, (double) max, 1.0);
    juce::Slider::setValue ((double) defaultValue, juce::dontSendNotification);
    setRepaintsOnMouseActivity (true);
    onValueChange = [this]
    {
        if (onChange) onChange (value());
        repaint();
    };
    setSize (juce::jmax (dia + 8, 44), idealHeight());
}

int  EngravedKnob::value() const noexcept       { return juce::roundToInt (getValue()); }
void EngravedKnob::setValueQuiet (int v)
{
    if (v == value()) return;
    juce::Slider::setValue ((double) v, juce::dontSendNotification);
    repaint();
}

void EngravedKnob::setLabelText (const juce::String& s)
{
    if (labelText == s) return;
    labelText = s;
    repaint();
}

void EngravedKnob::setRole (const juce::String& r)
{
    setUnused (r == "UNUSED");
    setLabelText (r);
}

void EngravedKnob::setSubLabel (const juce::String& s)  { if (subLabel != s)  { subLabel = s;  repaint(); } }
void EngravedKnob::setValueText (const juce::String& s) { if (valueText != s) { valueText = s; repaint(); } }
void EngravedKnob::setUnused (bool b)                   { if (unused != b)    { unused = b;    repaint(); } }
void EngravedKnob::setMidiBound (bool b)                { if (midiBound != b) { midiBound = b; repaint(); } }
void EngravedKnob::setDefaultValue (int v)              { defaultVal = v; }
void EngravedKnob::setDiameter (int px)                 { dia = px; repaint(); }
void EngravedKnob::setShowText (bool b)                 { showText = b; repaint(); }

int EngravedKnob::idealHeight() const
{
    if (! showText) return dia;
    const bool big = dia >= 76;
    int h = dia + 4 + (big ? 13 : 11);                       // label row
    h += big ? 15 : 13;                                       // value row
    if (subLabel.isNotEmpty()) h += 10;                       // sub-note row
    return h;
}

void EngravedKnob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onLearnRequest) onLearnRequest();
        return;
    }
    dragging = true;
    dragStartVal = value();
}

void EngravedKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const int perUnit = e.mods.isCommandDown() ? 8 : 2;      // fine mode: 1u / 8px
    int nv = dragStartVal + e.getDistanceFromDragStartX() / perUnit;
    juce::Slider::setValue ((double) nv, juce::sendNotificationSync);
}

void EngravedKnob::mouseUp (const juce::MouseEvent&)   { dragging = false; repaint(); }

void EngravedKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    juce::Slider::setValue ((double) defaultVal, juce::sendNotificationSync);
}

void EngravedKnob::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (w.deltaY == 0.0f) return;
    juce::Slider::setValue (getValue() + (w.deltaY > 0 ? 1 : -1), juce::sendNotificationSync);
}

void EngravedKnob::mouseEnter (const juce::MouseEvent&) { hovered = true;  repaint(); }
void EngravedKnob::mouseExit  (const juce::MouseEvent&) { hovered = false; repaint(); }

void EngravedKnob::paint (juce::Graphics& g)
{
    const bool big = dia >= 76;
    const bool hot = ! unused && (hovered || dragging || midiBound);

    Rectangle<int> full = getLocalBounds();
    Rectangle<int> face (full.getCentreX() - dia / 2, full.getY(), dia, dia);
    if (! showText)
        face.setY (full.getCentreY() - dia / 2);

    // face + ring
    g.setColour (C::CONTROL);
    g.fillEllipse (face.toFloat());
    g.setColour (unused ? C::KNOB_UNUSED_RING : hot ? C::BLOOD : C::EDGE);
    g.drawEllipse (face.toFloat().reduced (0.5f), 1.0f);

    // inner inset ring
    const int inset = big ? 16 : 9;
    g.setColour (C::KNOB_INNER);
    g.drawEllipse (face.toFloat().reduced ((float) inset), 1.0f);

    // the cut: length 15 (26 on 76px), starting top+4, sweep -135..+135
    const double lo = getMinimum(), hi = getMaximum();
    const float norm = hi > lo ? (float) ((getValue() - lo) / (hi - lo)) : 0.0f;
    const float ang  = juce::degreesToRadians (-135.0f + 270.0f * norm);
    const float cx = (float) face.getCentreX(), cy = (float) face.getCentreY();
    const float rOut = dia / 2.0f - 4.0f;
    const float rIn  = rOut - (big ? 26.0f : 15.0f);
    g.setColour (unused ? C::INK_GHOST : hot ? C::BLOOD_HOT : C::INK);
    g.drawLine (cx + rIn  * std::sin (ang), cy - rIn  * std::cos (ang),
                cx + rOut * std::sin (ang), cy - rOut * std::cos (ang),
                big ? 2.0f : 1.0f);

    if (! showText) return;

    // text stack under the face
    int y = face.getBottom() + 4;
    const juce::Colour labelFg = unused ? C::LAMP_SOUNDING : C::INK_DIM;
    const juce::Colour valueFg = unused ? C::INK_GHOST : C::INK;

    g.setColour (labelFg);
    g.setFont (big ? Type::monoMedium (10.0f, 0.16f) : Type::mono (8.0f, 0.10f));
    g.drawText (labelText, full.withY (y).withHeight (big ? 13 : 11), Justification::centred);
    y += big ? 13 : 11;

    g.setColour (valueFg);
    g.setFont (big ? Type::mono (12.0f, 0.04f) : Type::mono (10.0f, 0.04f));
    juce::String vs = valueText.isNotEmpty()
                        ? valueText
                        : juce::String (value()).paddedLeft ('0', 3);
    g.drawText (vs, full.withY (y).withHeight (big ? 15 : 13), Justification::centred);
    y += big ? 15 : 13;

    if (subLabel.isNotEmpty())
    {
        g.setColour (C::INK_GHOST);
        g.setFont (Type::nano (7.0f));
        g.drawText (subLabel, full.withY (y).withHeight (10), Justification::centred);
    }
}

/* ======================================================================== */
/*  PlateButton                                                              */
/* ======================================================================== */

PlateButton::PlateButton (const juce::String& text, bool withLamp, bool toggles)
    : juce::Button (text), lamp (withLamp)
{
    setClickingTogglesState (toggles);
    setTriggeredOnMouseDown (true);
}

void PlateButton::setToggleStateQuiet (bool on)
{
    setToggleState (on, juce::dontSendNotification);
}

void PlateButton::setLamp (bool b)          { lamp = b; repaint(); }
void PlateButton::setLampUnderText (bool b) { lampUnder = b; repaint(); }
void PlateButton::setEngagedStyle (bool b)  { engagedStyle = b; repaint(); }
void PlateButton::setOxideStyle (bool b)    { oxideStyle = b; repaint(); }
void PlateButton::setStencilText (bool b, float size) { stencil = b; stencilSize = size; repaint(); }
void PlateButton::setSubLine (const juce::String& s)  { subLine = s; repaint(); }

void PlateButton::clicked()
{
    if (onToggle) onToggle (getToggleState());
}

void PlateButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const bool on = getToggleState();
    Rectangle<int> r = getLocalBounds();

    juce::Colour bg, bd, fg, dot;
    if (! isEnabled())
    {
        bg = C::DISABLED_BG; bd = C::HAIRLINE_DIM; fg = C::INK_GHOST; dot = C::HAIRLINE_FAINT;
    }
    else if (on && engagedStyle)                               // CUT engaged
    {
        bg = C::BLOOD; bd = C::BLOOD_HOT; fg = C::INK_BRIGHT; dot = C::INK_BRIGHT;
    }
    else if (on && oxideStyle)
    {
        bg = C::OXIDE_PLATE; bd = C::OXIDE_DIM; fg = C::OXIDE_INK; dot = C::OXIDE_INK;
    }
    else if (on)                                               // armed / live
    {
        bg = C::BLOOD_DEEP; bd = C::BLOOD; fg = C::ARMED_TEXT; dot = C::BLOOD_HOT;
    }
    else if (highlighted || down)                              // action / hover
    {
        bg = C::PLATE_HOVER; bd = C::EDGE; fg = C::INK; dot = C::LAMP_DEAD;
    }
    else if (getClickingTogglesState())                        // toggle-off
    {
        bg = C::PLATE_LOW; bd = C::EDGE; fg = C::TAB_INACTIVE_FG; dot = C::LAMP_DEAD;
    }
    else                                                       // idle action plate
    {
        bg = C::PLATE; bd = C::EDGE; fg = C::INK_DIM; dot = C::LAMP_DEAD;
    }

    /* SURVIVOR 150x74 stencil plates (HTML frame 05, spec section 9):
     * idle plate is the #161513 literal, the stencil word sits on top and
     * the 8px status sub-line below carries the 6px lamp. */
    if (stencil)
    {
        static const juce::Colour STENCIL_IDLE_BG { 0xff161513 };
        if (isEnabled() && ! on && ! (highlighted || down))
            bg = STENCIL_IDLE_BG;
        if (isEnabled() && ! on)
            bd = C::EDGE;

        g.setColour (bg);
        g.fillRect (r);
        g.setColour (bd);
        g.drawRect (r, 1);

        /* word: armed #f0e6dc, idle INK for toggles, INK_DIM for CLEAR */
        const juce::Colour word = ! isEnabled() ? C::INK_GHOST
                                : on            ? C::INK_BRIGHT
                                : getClickingTogglesState() ? C::INK
                                                            : C::INK_DIM;
        /* column: word block + gap 6 + 8px sub row, centred */
        const int blockH = 24 + 6 + 8;
        const int top = r.getY() + (r.getHeight() - blockH) / 2;
        g.setColour (word);
        g.setFont (Type::stencil (stencilSize, 0.20f));
        g.drawText (getButtonText(), r.getX(), top, r.getWidth(), 24,
                    Justification::centred);

        if (subLine.isNotEmpty())
        {
            const juce::Font sf = Type::mono (8.0f, 0.14f);
            const juce::Colour subFg = on   ? C::BLOOD_HOT
                                     : lamp ? C::TAB_INACTIVE_FG
                                            : C::INK_FAINT;
            const int tw = (int) std::ceil (
                juce::GlyphArrangement::getStringWidth (sf, subLine));
            const int lampW = lamp ? 6 + 6 : 0;       // 6px lamp + gap 6
            int x = r.getX() + (r.getWidth() - lampW - tw) / 2;
            const int subY = top + 24 + 6;
            if (lamp)
            {
                g.setColour (on ? C::BLOOD_HOT : C::LAMP_DEAD);
                g.fillRect (x, subY + 1, 6, 6);
                x += 12;
            }
            g.setColour (subFg);
            g.setFont (sf);
            g.drawText (subLine, x, subY, tw + 2, 8, Justification::centredLeft);
        }
        return;
    }

    g.setColour (bg);
    g.fillRect (r);
    g.setColour (bd);
    g.drawRect (r, 1);

    const juce::Font f = Type::mono (10.0f, 0.16f);
    g.setFont (f);
    g.setColour (fg);

    if (lampUnder)
    {
        // transport style: word on top, 5px lamp centred underneath
        Rectangle<int> tr = r.reduced (0, 4);
        int lampH = 5;
        g.drawText (getButtonText(), tr.withTrimmedBottom (lampH + 4), Justification::centred);
        g.setColour (dot);
        g.fillRect (r.getCentreX() - 2, tr.getBottom() - lampH, 5, lampH);
    }
    else if (lamp)
    {
        Rectangle<int> tr = r;
        int lampSz = stencil ? 6 : 5;
        g.setColour (dot);
        g.fillRect (r.getX() + 7, r.getCentreY() - lampSz / 2, lampSz, lampSz);
        g.setColour (fg);
        g.drawText (getButtonText(),
                    tr.withTrimmedLeft (7 + lampSz + 5),
                    subLine.isEmpty() ? Justification::centredLeft : Justification::centredLeft);
    }
    else
    {
        g.drawText (getButtonText(),
                    subLine.isEmpty() ? r : r.withTrimmedBottom (12),
                    Justification::centred);
    }

    if (subLine.isNotEmpty())
    {
        g.setColour (on ? C::BLOOD_HOT : C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (subLine, r.withTop (r.getBottom() - 14).reduced (0, 2),
                    Justification::centred);
    }
}

/* ======================================================================== */
/*  TroughFader                                                              */
/* ======================================================================== */

TroughFader::TroughFader (juce::String name) : juce::Slider (name)
{
    setSliderStyle (juce::Slider::LinearVertical);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setRange (0.0, 256.0, 1.0);
    onValueChange = [this]
    {
        if (onChange) onChange (value());
        repaint();
    };
}

int  TroughFader::value() const noexcept { return juce::roundToInt (getValue()); }
void TroughFader::setValueQuiet (int v)
{
    if (v == value()) return;
    juce::Slider::setValue ((double) v, juce::dontSendNotification);
    repaint();
}

void TroughFader::setMuted (bool b) { if (muted != b) { muted = b; repaint(); } }

void TroughFader::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;
    dragStartY = e.getMouseDownY();
    dragStartVal = value();
}

void TroughFader::mouseDrag (const juce::MouseEvent& e)
{
    const int h = juce::jmax (1, getHeight());
    int nv = dragStartVal - juce::roundToInt (256.0f * (float) e.getDistanceFromDragStartY() / (float) h);
    juce::Slider::setValue ((double) nv, juce::sendNotificationSync);
}

void TroughFader::mouseUp (const juce::MouseEvent&) { dragging = false; }

void TroughFader::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (w.deltaY == 0.0f) return;
    juce::Slider::setValue (getValue() + (w.deltaY > 0 ? 8 : -8), juce::sendNotificationSync);
}

void TroughFader::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    Rectangle<int> trough (b.getCentreX() - 8, b.getY(), 16, b.getHeight());

    g.setColour (C::TROUGH);
    g.fillRect (trough);
    g.setColour (C::HAIRLINE);
    g.drawRect (trough, 1);

    const int innerH = trough.getHeight() - 2;
    const int fillH = juce::roundToInt (innerH * value() / 256.0f);
    g.setColour (muted ? C::LAMP_DEAD : C::BLOOD);
    g.fillRect (trough.getX() + 1, trough.getBottom() - 1 - fillH, 14, fillH);

    // 3px INK cap overhanging 3px each side
    int capY = juce::jlimit (b.getY(), b.getBottom() - 3, trough.getBottom() - 1 - fillH - 1);
    g.setColour (C::INK);
    g.fillRect (trough.getX() - 3, capY, trough.getWidth() + 6, 3);
}

/* ======================================================================== */
/*  MeterComponent                                                           */
/* ======================================================================== */

MeterComponent::MeterComponent()
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void MeterComponent::setHorizontal (bool b) { horizontal = b; repaint(); }

void MeterComponent::timerCallback()
{
    float nv = source ? juce::jlimit (0.0f, 1.0f, source()) : 0.0f;
    if (nv != level)
    {
        level = nv;
        repaint();
    }
}

void MeterComponent::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::TROUGH);
    g.fillRect (b);
    g.setColour (C::HAIRLINE);
    g.drawRect (b, 1);

    if (level <= 0.0f) return;

    constexpr float minus6dB = 0.501187f;
    const bool rails = level >= 0.995f;

    if (horizontal)
    {
        const int w = b.getWidth() - 2;
        const int fill = juce::roundToInt (w * level);
        const int amberFrom = juce::roundToInt (w * minus6dB);
        g.setColour (C::BLOOD);
        g.fillRect (b.getX() + 1, b.getY() + 1, juce::jmin (fill, amberFrom), b.getHeight() - 2);
        if (fill > amberFrom)
        {
            g.setColour (rails ? C::BLOOD_HOT : C::AMBER);
            g.fillRect (b.getX() + 1 + amberFrom, b.getY() + 1, fill - amberFrom, b.getHeight() - 2);
        }
    }
    else
    {
        const int h = b.getHeight() - 2;
        const int fill = juce::roundToInt (h * level);
        const int amberFrom = juce::roundToInt (h * minus6dB);
        g.setColour (C::BLOOD);
        g.fillRect (b.getX() + 1, b.getBottom() - 1 - juce::jmin (fill, amberFrom),
                    b.getWidth() - 2, juce::jmin (fill, amberFrom));
        if (fill > amberFrom)
        {
            g.setColour (rails ? C::BLOOD_HOT : C::AMBER);
            g.fillRect (b.getX() + 1, b.getBottom() - 1 - fill,
                        b.getWidth() - 2, fill - amberFrom);
        }
    }
}

/* ======================================================================== */
/*  StatusBadge / SerialTag                                                  */
/* ======================================================================== */

StatusBadge::StatusBadge (Badge::Kind k, juce::String text)
    : kind (k), label (std::move (text))
{
    setInterceptsMouseClicks (false, false);
}

void StatusBadge::set (Badge::Kind k, const juce::String& text)
{
    kind = k;
    label = text;
    repaint();
}

int StatusBadge::idealWidth (const juce::String& text)
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (
               Type::mono (8.0f, 0.14f), text)) + 14;
}

void StatusBadge::paintBadge (juce::Graphics& g, Rectangle<int> r,
                              Badge::Kind k, const juce::String& text)
{
    g.setColour (Badge::border (k));
    g.drawRect (r, 1);
    g.setColour (Badge::text (k));
    g.setFont (Type::mono (8.0f, 0.14f));
    g.drawText (text, r, Justification::centred);
}

void StatusBadge::paint (juce::Graphics& g)
{
    paintBadge (g, getLocalBounds(), kind, label);
}

SerialTag::SerialTag (juce::String t) : text (std::move (t))
{
    setInterceptsMouseClicks (false, false);
}

void SerialTag::setText (const juce::String& t) { text = t; repaint(); }

void SerialTag::paint (juce::Graphics& g)
{
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.10f));
    g.drawText (text, getLocalBounds(), Justification::centredRight);
}

/* ======================================================================== */
/*  StepCell                                                                 */
/* ======================================================================== */

StepCell::StepCell (int index) : idx (index) {}

void StepCell::setState (State s, bool notify)
{
    if (s == st && ! notify) return;
    st = s;
    if (notify && onEdit) onEdit (idx, st);
    repaint();
}

void StepCell::setPlayhead (bool b)  { if (playhead != b) { playhead = b; repaint(); } }
void StepCell::setShowIndex (bool b) { showIndex = b; repaint(); }

void StepCell::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        paintState = OFF;
        setState (OFF, true);
        return;
    }
    paintState = st == OFF ? HIT : st == HIT ? ACCENT : OFF;
    setState (paintState, true);
}

void StepCell::mouseDrag (const juce::MouseEvent& e)
{
    // paint the starting state across sibling cells (spec section 15)
    auto* parent = getParentComponent();
    if (parent == nullptr) return;
    auto pos = e.getEventRelativeTo (parent).getPosition();
    if (auto* c = dynamic_cast<StepCell*> (parent->getComponentAt (pos)))
        if (c != this)
            c->setState (paintState, true);
}

void StepCell::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();

    juce::Colour bg, bd, dot, num;
    switch (st)
    {
        default:
        case OFF:    bg = C::PANEL;  bd = C::HAIRLINE;  dot = juce::Colour(); num = C::CELL_NUM_OFF; break;
        case HIT:    bg = C::HIT_BG; bd = C::HIT_BD;    dot = C::INK;         num = C::INK_FAINT;    break;
        case ACCENT: bg = C::BLOOD;  bd = C::BLOOD_HOT; dot = C::INK_BRIGHT;  num = C::CELL_NUM_ACC; break;
    }
    if (playhead && st == OFF)
    {
        bg = C::PLATE_HOVER; bd = C::OXIDE_DIM; dot = C::OXIDE_INK; num = C::INK_FAINT;
    }

    g.setColour (bg);
    g.fillRect (b);
    g.setColour (bd);
    g.drawRect (b, 1);

    if (st != OFF || playhead)
    {
        g.setColour (dot);
        g.fillRect (b.getCentreX() - 3, b.getCentreY() - 3, 6, 6);
    }

    if (showIndex)
    {
        g.setColour (num);
        g.setFont (Type::nano (6.0f));
        g.drawText (juce::String (idx + 1).paddedLeft ('0', 2),
                    b.reduced (2, 1), Justification::bottomRight);
    }
}

/* ======================================================================== */
/*  Header band + label row                                                  */
/* ======================================================================== */

void paintHeaderBand (juce::Graphics& g, Rectangle<int> band,
                      const juce::String& title, const juce::String& subtitle,
                      const juce::String& serial, Badge::Kind badgeKind,
                      const juce::String& badgeText)
{
    g.setColour (C::RAISED);
    g.fillRect (band);
    g.setColour (C::HAIRLINE);
    g.fillRect (band.getX(), band.getBottom() - 1, band.getWidth(), 1);

    Rectangle<int> r = band.reduced (10, 0).withTrimmedBottom (1);

    // the one registration glyph
    g.setColour (C::INK_GHOST);
    g.setFont (Type::mono (10.0f));
    g.drawText ("+", r.removeFromLeft (10), Justification::centredLeft);
    r.removeFromLeft (4);

    // right side first: badge, then serial
    if (badgeText.isNotEmpty())
    {
        int bw = StatusBadge::idealWidth (badgeText);
        Rectangle<int> br = r.removeFromRight (bw).withSizeKeepingCentre (bw, 14);
        StatusBadge::paintBadge (g, br, badgeKind, badgeText);
        r.removeFromRight (8);
    }
    if (serial.isNotEmpty())
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.10f));
        int sw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (
                     Type::mono (8.0f, 0.10f), serial)) + 4;
        g.drawText (serial, r.removeFromRight (sw), Justification::centredRight);
        r.removeFromRight (8);
    }

    // title (condensed 700, 11px, .24em)
    g.setColour (C::INK);
    g.setFont (Type::panelTitle());
    int tw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (Type::panelTitle(), title)) + 6;
    g.drawText (title, r.removeFromLeft (juce::jmin (tw, r.getWidth())), Justification::centredLeft);

    if (subtitle.isNotEmpty())
    {
        r.removeFromLeft (8);
        g.setColour (C::INK_DIM);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (subtitle, r, Justification::centredLeft);
    }
}

void paintHeaderBand (juce::Graphics& g, Rectangle<int> band,
                      const juce::String& title, const juce::String& subtitle,
                      const juce::String& rightText)
{
    g.setColour (C::RAISED);
    g.fillRect (band);
    g.setColour (C::HAIRLINE);
    g.fillRect (band.getX(), band.getBottom() - 1, band.getWidth(), 1);

    Rectangle<int> r = band.reduced (10, 0).withTrimmedBottom (1);

    g.setColour (C::INK_GHOST);
    g.setFont (Type::mono (10.0f));
    g.drawText ("+", r.removeFromLeft (10), Justification::centredLeft);
    r.removeFromLeft (4);

    if (rightText.isNotEmpty())
    {
        /* Claim only the width the text needs, capped at half the band, and
         * ellipsise inside it. The right hint used to be drawn across the
         * whole remaining band, which was invisible while it said "~/MORGUE"
         * but runs straight under the title the moment it says
         * "C:\Users\somebody\MORGUE" instead. */
        const juce::Font rf = Type::mono (8.0f, 0.10f);
        const int want = (int) std::ceil (
            juce::GlyphArrangement::getStringWidth (rf, rightText)) + 2;
        Rectangle<int> rr = r.removeFromRight (
            juce::jmin (want, juce::jmax (0, r.getWidth() / 2)));
        g.setColour (C::INK_FAINT);
        g.setFont (rf);
        g.drawText (rightText, rr, Justification::centredRight, true);
    }

    g.setColour (C::INK);
    g.setFont (Type::panelTitle());
    int tw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (Type::panelTitle(), title)) + 6;
    g.drawText (title, r.removeFromLeft (juce::jmin (tw, r.getWidth())), Justification::centredLeft);

    if (subtitle.isNotEmpty())
    {
        r.removeFromLeft (8);
        g.setColour (C::INK_DIM);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (subtitle, r, Justification::centredLeft);
    }
}

void paintLabelRow (juce::Graphics& g, Rectangle<int> row,
                    const juce::String& left, const juce::String& right)
{
    g.setColour (C::INK_DIM);
    g.setFont (Type::label());
    g.drawText (left, row.reduced (10, 0), Justification::centredLeft);
    if (right.isNotEmpty())
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (right, row.reduced (10, 0), Justification::centredRight);
    }
}

} // namespace morgue
