/* ExportSheet.cpp -- see ExportSheet.h. Spec section 11 + HTML frame
 * "07 EXPORT", drawn pixel-exactly.
 *
 * Sheet is a fixed 720 x 338 (1px border + 28 header + 308 body), centred.
 * Body: left pane 437 (label row 20 + 12 track rows of 24) | 1px divider |
 * right pane 280 (label row 20, then padded 10: RANGE / FORMAT segments,
 * TAIL and DESTINATION fields, two info lines, bottom CANCEL / RENDER bar).
 * All state here is local UI state -- the engine has no stem renderer (R6
 * planned), so RENDER deliberately performs nothing and no live data is
 * faked anywhere on the sheet.
 */

#include "ExportSheet.h"
#include "Session.h"

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    /* supporting literals from the EXPORT frame (not in the token table) */
    const juce::Colour SCRIM    (0xff050505);   // 72% overlay ink
    const juce::Colour DEST_INK (0xffc9c4b8);   // destination path text
    const juce::Colour STEM_INK (0xffc9c4b8);   // checked stem name

    /* fixed sheet geometry (HTML frame, border-box) */
    constexpr int kSheetW   = 720;
    constexpr int kSheetH   = 338;              // 1 + 28 + 308 + 1
    constexpr int kHeaderH  = 28;
    constexpr int kLabelH   = 20;
    constexpr int kRowH     = 24;
    constexpr int kLeftW    = 437;              // + 1px divider + 280 = 718
    constexpr int kRightW   = 280;
    constexpr int kPad      = 10;
    constexpr int kFieldW   = kRightW - 2 * kPad;   // 260
    constexpr int kSegGap   = 2;
    constexpr int kSegW     = (kFieldW - 2 * kSegGap) / 3;  // 85 (last takes 86)
    constexpr int kBtnH     = 30;
    constexpr int kBtnW     = (kFieldW - kSegGap) / 2;      // 129

    /* the exact planned stems list from the HTML frame (stemDef): MASTER
     * first, per-stem sizes, V05 unchecked, V06 muted with no size. An
     * unchecked row prints its size as an em dash, exactly as drawn. */
    struct Stem { const char* name; const char* size; bool checkedByDefault;
                  double mb; };
    const Stem kStems[12] = {
        { "MASTER \xc2\xb7 0418-full.wav",     "48.2 MB", true,  48.2 },
        { "V01 THUMP",                         "12.1 MB", true,  12.1 },
        { "V02 METAL",                         "12.1 MB", true,  12.1 },
        { "V03 DUST",                          "12.1 MB", true,  12.1 },
        { "V04 BONE",                          "12.1 MB", true,  12.1 },
        { "V05 SIREN",                         "12.1 MB", false, 12.1 },
        { "V06 TAR (MUTED)",                   "\xe2\x80\x94", false, 0.0 },
        { "V07 HISS",                          "12.1 MB", true,  12.1 },
        { "V08 CHOKE",                         "12.1 MB", true,  12.1 },
        { "LICKS PATTERN A",                   "12.1 MB", true,  12.1 },
        { "RETURN A \xc2\xb7 REVERB",          "12.1 MB", true,  12.1 },
        { "RETURN B \xc2\xb7 FEEDBACK DELAY",  "12.1 MB", true,  12.1 },
    };

    const char* const kRangeNames[3]  = { "LOOP", "SONG", "SEL" };
    const char* const kFormatNames[3] = { "WAV 24", "FLAC", "MP3" };
}

ExportSheet::ExportSheet()
{
    setInterceptsMouseClicks (true, true);      // swallow input: modal
    for (int i = 0; i < kTracks; ++i)
        checked[i] = kStems[i].checkedByDefault;   // exactly as drawn
}

/* ---- geometry ----------------------------------------------------------- */

Rectangle<int> ExportSheet::sheetBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (kSheetW, kSheetH);
}

Rectangle<int> ExportSheet::trackRowBounds (int i) const
{
    Rectangle<int> s = sheetBounds();
    return { s.getX() + 1, s.getY() + 1 + kHeaderH + kLabelH + i * kRowH,
             kLeftW, kRowH };
}

int ExportSheet::rightPaneX() const
{
    return sheetBounds().getX() + 1 + kLeftW + 1;
}

int ExportSheet::contentY() const
{
    return sheetBounds().getY() + 1 + kHeaderH + kLabelH + kPad;
}

Rectangle<int> ExportSheet::rangeSegBounds (int i) const
{
    return { rightPaneX() + kPad + i * (kSegW + kSegGap), contentY() + 14,
             i == 2 ? kFieldW - 2 * (kSegW + kSegGap) : kSegW, 22 };
}

Rectangle<int> ExportSheet::formatSegBounds (int i) const
{
    return rangeSegBounds (i).translated (0, 46);
}

Rectangle<int> ExportSheet::tailFieldBounds() const
{
    return { rightPaneX() + kPad, contentY() + 106, kFieldW, 22 };
}

Rectangle<int> ExportSheet::destFieldBounds() const
{
    return { rightPaneX() + kPad, contentY() + 152, kFieldW, 22 };
}

Rectangle<int> ExportSheet::cancelBounds() const
{
    Rectangle<int> s = sheetBounds();
    return { rightPaneX() + kPad, s.getBottom() - 1 - kPad - kBtnH,
             kBtnW, kBtnH };
}

Rectangle<int> ExportSheet::renderBounds() const
{
    return cancelBounds().translated (kBtnW + kSegGap, 0);
}

/* ---- interaction (local UI state only; R6 planned) ---------------------- */

void ExportSheet::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();

    if (! sheetBounds().contains (p))           // scrim click = cancel
    {
        if (onCancel) onCancel();
        return;
    }

    if (e.mods.isPopupMenu())
        return;

    if (cancelBounds().contains (p))
    {
        if (onCancel) onCancel();
        return;
    }

    if (renderBounds().contains (p))
        return;     // R6 PLANNED: the engine has no stem renderer. The sheet
                    // itself is the planned state -- deliberately no fake
                    // progress, no dialog, no file writes.

    for (int i = 0; i < kTracks; ++i)
        if (trackRowBounds (i).contains (p))
        {
            checked[i] = ! checked[i];
            repaint (sheetBounds());
            return;
        }

    for (int i = 0; i < 3; ++i)
        if (rangeSegBounds (i).contains (p))
        {
            rangeSel = i;
            repaint (sheetBounds());
            return;
        }

    // FORMAT: WAV 24 is the only live choice; FLAC / MP3 are drawn disabled.
}

juce::String ExportSheet::getTooltip()
{
    const auto p = getMouseXYRelative();
    const auto dash = U8 (" \xe2\x80\x94 ");

    for (int i = 0; i < kTracks; ++i)
        if (trackRowBounds (i).contains (p))
            return U8 (kStems[i].name) + dash
                 + "includes this stem in the render. One file per checked track.";

    if (rangeSegBounds (0).contains (p))
        return "LOOP" + dash + "sets the export range to the current loop.";
    if (rangeSegBounds (1).contains (p))
        return "SONG" + dash + "sets the export range to the arranged song.";
    if (rangeSegBounds (2).contains (p))
        return "SEL" + dash + "sets the export range to the current selection.";

    if (formatSegBounds (0).contains (p))
        return "WAV 24" + dash + "output format: 24-bit WAV.";
    if (formatSegBounds (1).contains (p))
        return "FLAC" + dash + "output format. Planned, disabled.";
    if (formatSegBounds (2).contains (p))
        return "MP3" + dash + "output format. Planned, disabled.";

    if (tailFieldBounds().contains (p))
        return "TAIL" + dash + "extra time rendered after the range for FX tails. Seconds.";
    if (destFieldBounds().contains (p))
        return "DESTINATION" + dash + "folder the stems are written to.";

    if (cancelBounds().contains (p))
        return "CANCEL" + dash + "closes this sheet. No files are written.";
    if (renderBounds().contains (p))
        return "RENDER" + dash + "offline stem render to the destination. R6 planned; writes nothing yet.";

    return {};
}

/* ---- painting ----------------------------------------------------------- */

void ExportSheet::paint (juce::Graphics& g)
{
    const auto dot = U8 (" \xc2\xb7 ");

    // console at 22% opacity under a 72% scrim (HTML layer stack, no blur):
    // GROUND at 78% leaves the console showing through at 22%, then the
    // #050505 scrim at 72% on top.
    g.setColour (C::GROUND.withAlpha (0.78f));
    g.fillRect (getLocalBounds());
    g.setColour (SCRIM.withAlpha (0.72f));
    g.fillRect (getLocalBounds());

    const Rectangle<int> s = sheetBounds();
    g.setColour (C::PANEL);
    g.fillRect (s);
    g.setColour (C::EDGE);
    g.drawRect (s, 1);

    // ---- header 28: RAISED, "+  STEM EXPORT ... N.72-0426 · REV 03" ------
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
    g.setColour (C::INK);
    g.setFont (Type::panelTitle());
    g.drawText ("STEM EXPORT", hr, Justification::centredLeft);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.14f));
    g.drawText (juce::String (SerialNo::EXPORT) + dot + "REV 03",
                hr, Justification::centredRight);

    // ---- left pane: TRACKS TO RENDER, 12 rows of 24 ----------------------
    const int bodyTop = s.getY() + 1 + kHeaderH;
    const int leftX   = s.getX() + 1;

    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (8.0f, 0.16f));
    g.drawText ("TRACKS TO RENDER",
                Rectangle<int> (leftX + kPad, bodyTop, kLeftW - 2 * kPad, kLabelH - 1),
                Justification::centredLeft);
    g.setColour (C::HAIRLINE);
    g.fillRect (leftX, bodyTop + kLabelH - 1, kLeftW, 1);

    for (int i = 0; i < kTracks; ++i)
    {
        const Rectangle<int> row = trackRowBounds (i);
        const bool on = checked[i];

        // 11px checkbox, 5px tick (unchecked: PANEL box on an EDGE border)
        const Rectangle<int> box (row.getX() + kPad, row.getY() + 6, 11, 11);
        g.setColour (on ? C::BLOOD_DEEP : C::PANEL);
        g.fillRect (box);
        g.setColour (on ? C::BLOOD : C::EDGE);
        g.drawRect (box, 1);
        if (on)
        {
            g.setColour (C::BLOOD_HOT);
            g.fillRect (box.getX() + 3, box.getY() + 3, 5, 5);
        }

        g.setColour (on ? STEM_INK : C::INK_FAINT);
        g.setFont (Type::mono (9.0f, 0.08f));
        g.drawText (U8 (kStems[i].name),
                    Rectangle<int> (box.getRight() + 8, row.getY(), 300, kRowH - 1),
                    Justification::centredLeft);

        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText (on ? U8 (kStems[i].size) : U8 ("\xe2\x80\x94"),
                    row.reduced (kPad, 0).withTrimmedBottom (1),
                    Justification::centredRight);

        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
    }

    // ---- divider ---------------------------------------------------------
    g.setColour (C::HAIRLINE);
    g.fillRect (leftX + kLeftW, bodyTop, 1, kSheetH - 2 - kHeaderH);

    // ---- right pane 280: RENDER SETTINGS ---------------------------------
    const int rx = rightPaneX();
    const int cx = rx + kPad;
    const int cy = contentY();

    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (8.0f, 0.16f));
    g.drawText ("RENDER SETTINGS",
                Rectangle<int> (rx + kPad, bodyTop, kRightW - 2 * kPad, kLabelH - 1),
                Justification::centredLeft);
    g.setColour (C::HAIRLINE);
    g.fillRect (rx, bodyTop + kLabelH - 1, kRightW, 1);

    const auto groupLabel = [&] (const char* text, int y)
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.12f));
        g.drawText (text, Rectangle<int> (cx, y, kFieldW, 10),
                    Justification::centredLeft);
    };

    // RANGE: LOOP / SONG / SEL (selection = local UI state)
    groupLabel ("RANGE", cy);
    for (int i = 0; i < 3; ++i)
    {
        const Rectangle<int> seg = rangeSegBounds (i);
        const bool active = (i == rangeSel);
        g.setColour (active ? C::PLATE_HOVER : C::PLATE_LOW);
        g.fillRect (seg);
        g.setColour (active ? C::EDGE : C::HAIRLINE);
        g.drawRect (seg, 1);
        g.setColour (active ? C::INK : C::TAB_INACTIVE_FG);
        g.setFont (Type::mono (9.0f));
        g.drawText (kRangeNames[i], seg, Justification::centred);
    }

    // FORMAT: WAV 24 active; FLAC / MP3 disabled
    groupLabel ("FORMAT", cy + 46);
    for (int i = 0; i < 3; ++i)
    {
        const Rectangle<int> seg = formatSegBounds (i);
        const bool active = (i == 0);
        g.setColour (active ? C::PLATE_HOVER : C::DISABLED_BG);
        g.fillRect (seg);
        g.setColour (active ? C::EDGE : C::HAIRLINE_DIM);
        g.drawRect (seg, 1);
        g.setColour (active ? C::INK : C::INK_GHOST);
        g.setFont (Type::mono (9.0f));
        g.drawText (kFormatNames[i], seg, Justification::centred);
    }

    // TAIL field
    groupLabel ("TAIL", cy + 92);
    {
        const Rectangle<int> f = tailFieldBounds();
        g.setColour (C::SOCKET);
        g.fillRect (f);
        g.setColour (C::HAIRLINE);
        g.drawRect (f, 1);
        g.setColour (C::INK);
        g.setFont (Type::mono (10.0f));
        g.drawText ("4.0 s", f.reduced (8, 0), Justification::centredLeft);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f));
        g.drawText ("FX TAILS INCLUDED", f.reduced (8, 0), Justification::centredRight);
    }

    // DESTINATION field
    groupLabel ("DESTINATION", cy + 138);
    {
        const Rectangle<int> f = destFieldBounds();
        g.setColour (C::SOCKET);
        g.fillRect (f);
        g.setColour (C::HAIRLINE);
        g.drawRect (f, 1);
        g.setColour (DEST_INK);
        g.setFont (Type::mono (9.0f));
        /* Planned chrome, but it must name a directory that could exist on
         * this machine: "~/MORGUE/stems/0418/" is not a path on Windows. */
        const juce::String sep = morgue::pathSep();
        g.drawText (morgue::morgueDirDisplay() + sep + "stems" + sep + "0418" + sep,
                    f.reduced (8, 0), Justification::centredLeft, true);
    }

    // two info lines (count/size track the checkboxes and the per-stem sizes)
    {
        int n = 0;
        double sum = 0.0;
        for (int i = 0; i < kTracks; ++i)
            if (checked[i]) { ++n; sum += kStems[i].mb; }
        const int mb = juce::roundToInt (sum);

        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.08f));
        g.drawText (juce::String (n) + " FILES" + dot + "EST " + juce::String (mb) + " MB",
                    Rectangle<int> (cx, cy + 184, kFieldW, 10), Justification::centredLeft);
        g.drawText ("OFFLINE RENDER" + dot + "FASTER THAN REALTIME",
                    Rectangle<int> (cx, cy + 197, kFieldW, 10), Justification::centredLeft);
    }

    // ---- bottom bar: CANCEL / RENDER -------------------------------------
    const Rectangle<int> cancel = cancelBounds();
    const Rectangle<int> render = renderBounds();

    g.setColour (C::HAIRLINE);
    g.fillRect (rx, cancel.getY() - 1 - kPad, kRightW, 1);

    g.setColour (C::PLATE_LOW);
    g.fillRect (cancel);
    g.setColour (C::EDGE);
    g.drawRect (cancel, 1);
    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (10.0f, 0.16f));
    g.drawText ("CANCEL", cancel, Justification::centred);

    g.setColour (C::BLOOD_DEEP);
    g.fillRect (render);
    g.setColour (C::BLOOD);
    g.drawRect (render, 1);
    g.setColour (C::INK_BRIGHT);
    g.drawText ("RENDER", render, Justification::centred);
}

} // namespace morgue
