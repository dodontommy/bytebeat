/* ExportSheet.cpp -- see ExportSheet.h.
 *
 * A centred notice sheet over the dimmed console. Nothing on it is a control
 * except CLOSE, and nothing on it is a number: there is no renderer, so there
 * is nothing to measure and nothing to promise. The copy names the one thing
 * that DOES capture audio today (transport REC) and names the real directory
 * it writes to, asked of Session.h rather than hardcoded.
 */

#include "ExportSheet.h"
#include "Session.h"

#include <cmath>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    /* 72% overlay ink over the console (HTML layer stack, no blur) */
    const juce::Colour SCRIM (0xff050505);

    constexpr int kSheetW  = 620;
    constexpr int kHeaderH = 28;
    constexpr int kPad     = 20;
    constexpr int kBtnH    = 30;
    constexpr int kBtnW    = 120;

    /* Body type: body() is 11px with a 1.5 line box. Type::rowH() is the
     * design system's minimum glyph box for a size -- use it rather than
     * inventing a leading, which is how the old sheet ended up setting 9px
     * names into 8px rows. */
    juce::Font titleFont() { return Type::monoMedium (13.0f, 0.10f); }
    juce::Font bodyFont()  { return Type::body(); }
    int        bodyLineH() { return Type::rowH (11.0f); }        // 18

    /* %DIR% is the console's real directory. Never print a hardcoded
     * "~/MORGUE" -- it is not a path on this platform. */
    const char* const kParas[] =
    {
        "MORGUE cannot render stems. The engine has no offline renderer, so "
        "this sheet has no track list, no format picker and no RENDER button: "
        "every one of them would be a picture of a feature that is not here.",

        "TO CAPTURE AUDIO NOW, arm REC on the transport. It writes a real WAV "
        "of the master bus -- post-gain, post-mute, exactly what you hear -- "
        "into %DIR%. One file of everything, not one file per voice.",

        "WHEN STEM EXPORT IS BUILT it will write one file per voice, sampler "
        "slot and return bus, plus the master. Until then this sheet stays "
        "empty on purpose, because an empty sheet is honest and a greyed-out "
        "one is not.",
    };
    constexpr int kNumParas = (int) (sizeof kParas / sizeof kParas[0]);

    juce::String resolve (const char* raw)
    {
        return U8 (raw).replace ("%DIR%", morgueDirDisplay());
    }

    /* Greedy word wrap against real glyph widths. */
    void wrapInto (juce::StringArray& out, const juce::Font& f,
                   const juce::String& text, int maxW)
    {
        juce::String line;
        for (const auto& w : juce::StringArray::fromTokens (text, " ", {}))
        {
            const juce::String cand = line.isEmpty() ? w : line + " " + w;
            if (juce::GlyphArrangement::getStringWidth (f, cand) > (float) maxW
                && line.isNotEmpty())
            {
                out.add (line);
                line = w;
            }
            else
            {
                line = cand;
            }
        }
        if (line.isNotEmpty())
            out.add (line);
    }
} // namespace

ExportSheet::ExportSheet()
{
    setInterceptsMouseClicks (true, true);      // swallow input: modal
}

/* ---- geometry ----------------------------------------------------------- */

juce::StringArray ExportSheet::bodyLines() const
{
    const int textW = kSheetW - 2 - 2 * kPad;
    const juce::Font f = bodyFont();

    juce::StringArray lines;
    for (int i = 0; i < kNumParas; ++i)
    {
        if (i > 0) lines.add ({});             // paragraph gap = one blank line
        wrapInto (lines, f, resolve (kParas[i]), textW);
    }
    return lines;
}

Rectangle<int> ExportSheet::sheetBounds() const
{
    const int h = 1 + kHeaderH
                + kPad
                + Type::rowH (13.0f)                        // title line
                + 12
                + bodyLines().size() * bodyLineH()
                + 20
                + kBtnH
                + kPad + 1;
    return getLocalBounds().withSizeKeepingCentre (kSheetW, h);
}

Rectangle<int> ExportSheet::closeBounds() const
{
    const Rectangle<int> s = sheetBounds();
    return { s.getRight() - 1 - kPad - kBtnW,
             s.getBottom() - 1 - kPad - kBtnH, kBtnW, kBtnH };
}

/* ---- interaction: one control, and it works ----------------------------- */

void ExportSheet::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (! sheetBounds().contains (p))           // scrim click = dismiss
    {
        if (onCancel) onCancel();
        return;
    }

    if (e.mods.isPopupMenu())
        return;

    if (closeBounds().contains (p) && onCancel)
        onCancel();
}

juce::String ExportSheet::getTooltip()
{
    if (closeBounds().contains (getMouseXYRelative()))
        return U8 ("CLOSE \xe2\x80\x94 returns to the previous panel. "
                   "Nothing here writes a file.");
    return {};
}

/* ---- painting ----------------------------------------------------------- */

void ExportSheet::paint (juce::Graphics& g)
{
    // console at 22% under a 72% scrim (HTML layer stack, no blur)
    g.setColour (C::GROUND.withAlpha (0.78f));
    g.fillRect (getLocalBounds());
    g.setColour (SCRIM.withAlpha (0.72f));
    g.fillRect (getLocalBounds());

    const Rectangle<int> s = sheetBounds();
    g.setColour (C::PANEL);
    g.fillRect (s);
    g.setColour (C::EDGE);
    g.drawRect (s, 1);

    /* ---- header 28 ------------------------------------------------------ */
    const Rectangle<int> hdr (s.getX() + 1, s.getY() + 1, kSheetW - 2, kHeaderH);
    g.setColour (C::RAISED);
    g.fillRect (hdr);
    g.setColour (C::HAIRLINE);
    g.fillRect (hdr.getX(), hdr.getBottom() - 1, hdr.getWidth(), 1);

    Rectangle<int> hr = hdr.withTrimmedBottom (1).reduced (kPad, 0);
    g.setColour (C::INK_GHOST);
    g.setFont (Type::mono (10.0f));
    g.drawText ("+", hr.removeFromLeft (10), Justification::centredLeft);
    hr.removeFromLeft (10);

    /* The badge goes on the SHEET, not on a control: the whole screen is the
     * thing that does not exist, so there is exactly one honesty signal and
     * it sits at the top. */
    {
        const int bw = StatusBadge::idealWidth ("NOT BUILT");
        StatusBadge::paintBadge (g, hr.removeFromRight (bw).withSizeKeepingCentre (bw, 14),
                                 Badge::PLANNED, "NOT BUILT");
        hr.removeFromRight (10);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::micro());
        const juce::String ser (SerialNo::EXPORT);
        const int sw = (int) std::ceil (
            juce::GlyphArrangement::getStringWidth (Type::micro(), ser)) + 4;
        g.drawText (ser, hr.removeFromRight (sw), Justification::centredRight);
        hr.removeFromRight (10);
    }

    g.setColour (C::INK);
    g.setFont (Type::panelTitle());
    g.drawText ("STEM EXPORT", hr, Justification::centredLeft);

    /* ---- body ----------------------------------------------------------- */
    Rectangle<int> body = s.withTrimmedTop (1 + kHeaderH).reduced (kPad, 0);
    body.removeFromTop (kPad);

    g.setColour (C::INK);
    g.setFont (titleFont());
    g.drawText ("NO STEM RENDERER IN THIS BUILD",
                body.removeFromTop (Type::rowH (13.0f)), Justification::centredLeft);
    body.removeFromTop (12);

    {
        const juce::Font bf = bodyFont();
        const int lh = bodyLineH();
        const juce::StringArray lines = bodyLines();
        g.setFont (bf);
        for (int i = 0; i < lines.size(); ++i)
        {
            if (lines[i].isEmpty()) continue;
            /* The first word of each paragraph carries the instruction, so the
             * paragraphs read at INK_DIM (8.63:1) rather than the metadata
             * ink the old sheet set its entire body in. */
            g.setColour (C::INK_DIM);
            g.drawText (lines[i],
                        Rectangle<int> (body.getX(), body.getY() + i * lh,
                                        body.getWidth(), lh),
                        Justification::centredLeft);
        }
        body.removeFromTop (lines.size() * lh);
    }

    /* ---- CLOSE: the only control on the sheet --------------------------- */
    const Rectangle<int> close = closeBounds();
    g.setColour (C::HAIRLINE);
    g.fillRect (s.getX() + 1, close.getY() - 20, kSheetW - 2, 1);

    g.setColour (C::PLATE);
    g.fillRect (close);
    g.setColour (C::EDGE);
    g.drawRect (close, 1);
    g.setColour (C::INK_DIM);
    g.setFont (Type::label());
    g.drawText ("CLOSE", close, Justification::centred);
}

} // namespace morgue
