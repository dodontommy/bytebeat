/* FieldManual.cpp -- see FieldManual.h.
 *
 * Pixel pass of spec section 14 + the "09 FIELD MANUAL" frame of
 * MORGUE GUI.dc.html. All card / rule / key copy is the exact manualDef /
 * rules / keys data from the HTML frame. The footer regression count is
 * the real suite size (2,835 per HANDOFF.md; the mock predates R1's 15
 * step-sampler checks).
 *
 * Frame geometry (1440x900 reference, all bands flex-safe):
 *   header 64  (border-bottom #2a2927)
 *   body       left flex (ZONES row 20 + 2x6 card grid, 1px #1c1b19 gaps,
 *              border-right HAIRLINE) | right 400 (GOLDEN RULES, KEY REF)
 *   footer 26  (border-top #2a2927, bg PANEL)
 */

#include "FieldManual.h"

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

FieldManualOverlay::FieldManualOverlay()
{
    setInterceptsMouseClicks (true, true);
    setOpaque (true);
}

void FieldManualOverlay::mouseDown (const juce::MouseEvent&)
{
    if (onDismiss) onDismiss();
}

/* ---- exact copy from the HTML frame (manualDef), except card 05: the
 * step sampler shipped in R1, so GRAIN LICKS is badged LIVE like its own
 * header band (the mock predates R1) ------------------------------------- */

struct ZoneCard { const char* n; const char* name; Badge::Kind k; const char* badge; const char* desc; };

static const ZoneCard zones[12] =
{
    { "01", "LOCKER",      Badge::LIVE,    "LIVE",
      "Specimen archive of ~/MORGUE \xe2\x80\x94 recorded WAVs and project patches. Serial-tagged. Drag to a sampler slot or timeline lane." },
    { "02", "RACK",        Badge::LIVE,    "LIVE",
      "The voice station. 8 bytebeat layers: source, expression text, p0\xe2\x80\x93p7, post chain, 16-step sequencer. RETURN compiles without a glitch." },
    { "03", "ARRANGE",     Badge::CANVAS,  "CANVAS",
      "Song timeline. Bars across, lanes down. Clips, per-lane recording, automation. Playhead is the engine clock." },
    { "04", "GRAIN MASS",  Badge::LIVE,    "LIVE",
      "Four sample wells. Load, play, pitch, reverse, loop. Mixes on top of the engine in real time." },
    { "05", "GRAIN LICKS", Badge::LIVE,    "LIVE",
      "Step sampler. One sample per slot, 16 steps, choke groups. This is how you make a beat." },
    { "06", "SURVIVOR",    Badge::LIVE,    "LIVE",
      "Master phrase looper. ARM captures one bar at the next boundary \xe2\x80\x94 every layer and its tails." },
    { "07", "MIXER",       Badge::PARTIAL, "PARTIAL",
      "Levels, mutes, meters. Inserts and sends A\xe2\x80\x93" "D route into shared return buses." },
    { "08", "HW/SYNC",     Badge::LIVE,    "LIVE",
      "MIDI in: notes trigger the focused voice, CC drives its parameters. Learn any knob by right-click." },
    { "09", "TRANSPORT",   Badge::LIVE,    "LIVE",
      "RUN is master on. CUT is instant silence. REC writes a real WAV to ~/MORGUE. BPM / BEATS / BARS / GAIN are live." },
    { "10", "SCOPE",       Badge::LIVE,    "LIVE",
      "Oscilloscope of the master bus, pre-gain and pre-mute, at 30 Hz. If the line is flat, nothing is sounding." },
    { "11", "STATUS",      Badge::LIVE,    "LIVE",
      "Bar, step, CPU against budget, and the CLIP lamp when the master hits the rails." },
    { "12", "EXPORT",      Badge::PLANNED, "PLANNED",
      "Stem render: one WAV per voice, slot and return, plus master and full song." },
};

/* ---- exact copy from the HTML frame (rules) ----------------------------- */

static const char* const goldenRules[6] =
{
    "If it makes no sound: press RUN, or release CUT.",
    "If a knob reads UNUSED, the current expression ignores it. Change the expression, not the knob.",
    "RETURN compiles. Nothing is committed until you press it, and nothing glitches when you do.",
    "CUT silences the master but never wipes the SURVIVOR loop.",
    "REC captures the master bus post-everything. What you hear is what lands on disk.",
    "The session autosaves. Nothing you build here is lost, including the mistakes.",
};

/* ---- exact copy from the HTML frame (keys) ------------------------------ */

static const char* const keyRows[12][2] =
{
    { "?",                    "Open / close this map" },
    { "RETURN",               "Compile the expression" },
    { "1 \xe2\x80\x93 8",     "Focus voice 01\xe2\x80\x93""08 \xc2\xb7 with SHIFT: toggle it on/off" },
    { "SPACE",                "RUN / mute master" },
    { "ESC",                  "CUT \xe2\x80\x94 panic silence" },
    { "R",                    "Reverse focused sample" },
    { "O",                    "Loop focused sample" },
    { "A / Z",                "Sample pitch up / down" },
    { "M",                    "Arm motion capture on the last touched knob" },
    { "\xe2\x8c\x98Z",        "Undo last edit" },
    { "\xe2\x8c\x98R",        "Toggle REC" },
    { "DRAG",                 "Knobs respond to horizontal drag; \xe2\x8c\x98-drag is fine mode" },
};

/* Rule-body ink from the HTML frame (rules text #c9c4b8 -- between INK and
 * INK_DIM; no Theme token, local literal per the frame). */
static const juce::Colour RULE_INK { 0xffc9c4b8 };
/* Header/footer frame rule from the HTML frame: #2a2927 (== C::LAMP_DEAD). */
static const juce::Colour FRAME_RULE { 0xff2a2927 };

/* Greedy word wrap against real glyph widths. */
static juce::StringArray wrapText (const juce::Font& f, const juce::String& text, int maxW)
{
    juce::StringArray words = juce::StringArray::fromTokens (text, " ", {});
    juce::StringArray lines;
    juce::String line;
    for (const auto& w : words)
    {
        juce::String cand = line.isEmpty() ? w : line + " " + w;
        if (juce::GlyphArrangement::getStringWidth (f, cand) > (float) maxW && line.isNotEmpty())
        {
            lines.add (line);
            line = w;
        }
        else
        {
            line = cand;
        }
    }
    if (line.isNotEmpty())
        lines.add (line);
    return lines;
}

/* Baseline of a CSS-style line box: font vertically centred in lineH. */
static int baselineIn (const juce::Font& f, int boxTop, int lineH)
{
    return boxTop + juce::roundToInt ((lineH - f.getHeight()) * 0.5f + f.getAscent());
}

static void drawWrapped (juce::Graphics& g, const juce::Font& f, juce::Colour c,
                         const juce::StringArray& lines, int x, int top, int lineH)
{
    g.setColour (c);
    g.setFont (f);
    for (int i = 0; i < lines.size(); ++i)
        g.drawSingleLineText (lines[i], x, baselineIn (f, top + i * lineH, lineH));
}

/* 20px micro label row: 8px .18em INK_DIM, bottom hairline. */
static void drawMicroRow (juce::Graphics& g, Rectangle<int> row, const char* text,
                          int padX, bool topRule)
{
    if (topRule)
    {
        g.setColour (C::HAIRLINE);
        g.fillRect (row.getX(), row.getY(), row.getWidth(), 1);
    }
    g.setColour (C::INK_DIM);
    g.setFont (Type::mono (8.0f, 0.18f));
    g.drawText (text, row.reduced (padX, 0).withTrimmedBottom (1), Justification::centredLeft);
    g.setColour (C::HAIRLINE);
    g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
}

void FieldManualOverlay::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::MANUAL_BG);
    g.fillRect (b);

    const juce::Font bodyF   = Type::body();               // mono 10, lh 1.5 -> 15px lines
    const int        lineH   = 15;

    /* ---- header 64: masthead, serial/rev, dismiss hint ------------------ */
    Rectangle<int> hdr = b.removeFromTop (64);
    g.setColour (FRAME_RULE);
    g.fillRect (hdr.getX(), hdr.getBottom() - 1, hdr.getWidth(), 1);

    // flex-end content, padding 0 20px 10px (above the 1px rule)
    const int contentBottom = hdr.getBottom() - 1 - 10;    // masthead text bottom
    const juce::Font mastF = Type::masthead();
    g.setColour (C::INK);
    g.setFont (mastF);
    const int mastBaseline = juce::roundToInt ((float) contentBottom - mastF.getDescent());
    g.drawSingleLineText ("FIELD MANUAL", hdr.getX() + 20, mastBaseline);

    const int mastW = juce::roundToInt (
        juce::GlyphArrangement::getStringWidth (mastF, "FIELD MANUAL"));
    const juce::Font metaF = Type::mono (9.0f, 0.16f);
    const int metaBaseline = juce::roundToInt (
        (float) (contentBottom - 4) - metaF.getDescent());  // extra 4px pad-bottom
    g.setColour (C::INK_FAINT);
    g.setFont (metaF);
    g.drawSingleLineText (U8 ("MAP OF THIS CONSOLE \xc2\xb7 N.72-0418 \xc2\xb7 REV 11"),
                          hdr.getX() + 20 + mastW + 16, metaBaseline);
    g.setColour (C::INK_DIM);
    g.drawSingleLineText ("ESC / ? TO DISMISS", hdr.getRight() - 20, metaBaseline,
                          Justification::right);

    /* ---- footer 26: product line + regression count --------------------- */
    Rectangle<int> foot = b.removeFromBottom (26);
    g.setColour (C::PANEL);
    g.fillRect (foot);
    g.setColour (FRAME_RULE);
    g.fillRect (foot.getX(), foot.getY(), foot.getWidth(), 1);
    Rectangle<int> fr = foot.withTrimmedTop (1).reduced (20, 0);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.14f));
    g.drawText (U8 ("MORGUE \xc2\xb7 INSTRUMENT FOR DARK NOISE, EXPERIMENTALISM, AMBIENCE"),
                fr, Justification::centredLeft);
    g.drawText ("2 835 HEADLESS REGRESSION CHECKS PASSING", fr, Justification::centredRight);

    /* ---- body: left = zones, right 400 = rules + keys ------------------- */
    Rectangle<int> body  = b;
    Rectangle<int> right = body.removeFromRight (400);

    // left column carries the 1px divider on its right edge
    g.setColour (C::HAIRLINE);
    g.fillRect (body.getRight() - 1, body.getY(), 1, body.getHeight());
    Rectangle<int> left = body.withTrimmedRight (1);

    drawMicroRow (g, left.removeFromTop (20), "ZONES", 20, false);

    /* zone card grid: 2 cols x 6 rows, 1px HAIRLINE_DIM gaps, cards PANEL */
    {
        Rectangle<int> grid = left;
        g.setColour (C::HAIRLINE_DIM);
        g.fillRect (grid);

        const int gw = grid.getWidth();
        const int gh = grid.getHeight();
        const int c0 = (gw - 1) / 2;                 // col widths, 1px gap
        const int c1 = gw - 1 - c0;

        const juce::Font idxF  = Type::mono (9.0f);
        const juce::Font nameF = Type::stencil (13.0f, 0.20f);
        const int idxW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (idxF, "01"));

        for (int i = 0; i < 12; ++i)
        {
            const int row = i / 2, col = i % 2;
            // row edges: 6 tracks + 5 gaps of 1px, remainder spread by rounding
            const int e0 = juce::roundToInt ((float) row       * (gh + 1) / 6.0f);
            const int e1 = juce::roundToInt ((float) (row + 1) * (gh + 1) / 6.0f);
            Rectangle<int> card (grid.getX() + (col == 0 ? 0 : c0 + 1),
                                 grid.getY() + e0,
                                 col == 0 ? c0 : c1,
                                 e1 - e0 - 1);
            if (card.getHeight() < 4) continue;

            g.setColour (C::PANEL);
            g.fillRect (card);

            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (card);
            Rectangle<int> inner = card.reduced (14, 10);

            // title line (16): index, stencil name, badge right
            Rectangle<int> tl = inner.removeFromTop (16);
            const int bl = baselineIn (nameF, tl.getY(), 16);
            g.setColour (C::BLOOD);
            g.setFont (idxF);
            g.drawSingleLineText (zones[i].n, tl.getX(), bl);
            g.setColour (C::INK);
            g.setFont (nameF);
            g.drawSingleLineText (zones[i].name, tl.getX() + idxW + 8, bl);
            const int bw = StatusBadge::idealWidth (zones[i].badge);
            StatusBadge::paintBadge (g, tl.removeFromRight (bw).withSizeKeepingCentre (bw, 13),
                                     zones[i].k, zones[i].badge);

            // one-line (wrapping) description, 10px INK_DIM, lh 1.5
            inner.removeFromTop (5);
            drawWrapped (g, bodyF, C::INK_DIM,
                         wrapText (bodyF, U8 (zones[i].desc), inner.getWidth()),
                         inner.getX(), inner.getY(), lineH);
        }
    }

    /* ---- right column: GOLDEN RULES ------------------------------------- */
    drawMicroRow (g, right.removeFromTop (20), "GOLDEN RULES", 16, false);

    for (int i = 0; i < 6; ++i)
    {
        // row: pad 9/16, number col 14, gap 10, text flex, bottom faint rule
        const int textX = right.getX() + 16 + 14 + 10;
        const int textW = right.getRight() - 16 - textX;
        juce::StringArray lines = wrapText (bodyF, goldenRules[i], textW);
        Rectangle<int> row = right.removeFromTop (lines.size() * lineH + 19);

        g.setColour (C::BLOOD);
        g.setFont (Type::mono (9.0f));
        g.drawSingleLineText (juce::String::formatted ("%02d", i + 1),
                              row.getX() + 16,
                              baselineIn (Type::mono (9.0f), row.getY() + 9, lineH));
        drawWrapped (g, bodyF, RULE_INK, lines, textX, row.getY() + 9, lineH);

        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
    }

    /* ---- right column: KEY REFERENCE ------------------------------------ */
    drawMicroRow (g, right.removeFromTop (20), "KEY REFERENCE", 16, true);

    {
        const int kh = right.getHeight();
        const juce::Font capF = Type::mono (9.0f, 0.10f);
        for (int i = 0; i < 12; ++i)
        {
            const int e0 = juce::roundToInt ((float) i       * kh / 12.0f);
            const int e1 = juce::roundToInt ((float) (i + 1) * kh / 12.0f);
            Rectangle<int> row (right.getX(), right.getY() + e0, right.getWidth(), e1 - e0);
            if (row.getHeight() < 4) continue;

            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (row);

            g.setColour (C::HAIRLINE_FAINT);
            g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

            // key-cap plate: min-width 64, pad 2/6, PLATE_LOW on EDGE border
            const juce::String key = U8 (keyRows[i][0]);
            const int capW = juce::jmax (64, juce::roundToInt (
                                 juce::GlyphArrangement::getStringWidth (capF, key)) + 12);
            Rectangle<int> cap = Rectangle<int> (row.getX() + 16, 0, capW, 18)
                                     .withCentre ({ row.getX() + 16 + capW / 2, row.getCentreY() });
            g.setColour (C::PLATE_LOW);
            g.fillRect (cap);
            g.setColour (C::EDGE);
            g.drawRect (cap, 1);
            g.setColour (C::INK);
            g.setFont (capF);
            g.drawText (key, cap, Justification::centred);

            // description, 10px INK_DIM, wraps and centres in the row
            const int dx = cap.getRight() + 10;
            const int dw = row.getRight() - 16 - dx;
            juce::StringArray lines = wrapText (bodyF, U8 (keyRows[i][1]), dw);
            const int blockH = lines.size() * lineH;
            drawWrapped (g, bodyF, C::INK_DIM, lines, dx,
                         row.getCentreY() - blockH / 2, lineH);
        }
    }
}

} // namespace morgue
