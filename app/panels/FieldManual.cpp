/* FieldManual.cpp -- see FieldManual.h.
 *
 * This is the screen a confused player opens, so it is the screen that has to
 * be right. Two kinds of correction in this pass:
 *
 * IT MUST NOT DOCUMENT THINGS THAT DO NOT EXIST. Removed: the key caps for
 * M ("arm motion capture on the last touched knob") and MOD+Z ("undo last
 * edit") -- Main.cpp:396-398 states outright that the engine has neither, and
 * keyPressed returns false for both, while the caps were drawn in the same
 * plate style as the eleven that work. Removed: "Learn any knob by
 * right-click" from the HW/SYNC card -- EngravedKnob::onLearnRequest has no
 * subscriber anywhere in the app. Removed: the footer's "2 835 HEADLESS
 * REGRESSION CHECKS PASSING", a hardcoded string presented as a measurement,
 * which was hand-reconciled once and would have gone quietly false. The
 * EXPORT card now says the feature is not built, because it is not.
 *
 * IT MUST DOCUMENT THE THINGS THAT DO. Added: the EXHUME and PLATE cards --
 * the console grew two workspaces and its own map did not mention either --
 * and the P key, which drives the GRAIN MASS wells and was never listed.
 * The key descriptions now say WHERE a key applies, because R / O / A / Z / P
 * are only live while GRAIN MASS is the visible stage (Main.cpp:389-394).
 *
 * Geometry (flex-safe at any window size):
 *   header 64
 *   body   left flex (ZONES row 20 + 2x7 card grid, 1px gaps, right rule)
 *          | right 400 (GOLDEN RULES, KEY REFERENCE)
 *   footer 26
 */

#include "FieldManual.h"
#include "Session.h"

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

/* ---- the zones of the console. Every claim below is checked against the
 * code that implements it; if a line here cannot be traced to a call site,
 * it does not belong on this screen. ------------------------------------- */

struct ZoneCard { const char* n; const char* name; Badge::Kind k; const char* badge; const char* desc; };

/* Two placeholders are substituted when the card is drawn, because neither
 * answer is known at compile time and both were wrong on Windows:
 *   %DIR%  the console's real directory (Session.h)
 *   %MOD%  the command modifier as this keyboard spells it (Theme.h) */
static const ZoneCard zones[] =
{
    { "01", "LOCKER",      Badge::LIVE,    "LIVE",
      "Specimen archive of %DIR% \xe2\x80\x94 recorded WAVs and project patches. Serial-tagged. Drag to a sampler slot or timeline lane." },
    { "02", "RACK",        Badge::LIVE,    "LIVE",
      "The voice station. 8 bytebeat layers: source, expression text, p0\xe2\x80\x93p7, post chain, 16-step sequencer. RETURN compiles without a glitch." },
    { "03", "ARRANGE",     Badge::CANVAS,  "CANVAS",
      "Song timeline. Bars across, lanes down. Clips, one armed lane at a time for recording, automation. Playhead is the engine clock." },
    { "04", "GRAIN MASS",  Badge::LIVE,    "LIVE",
      "Four sample wells. Load, play, pitch, reverse, loop. Mixes on top of the engine in real time. Keys act on the selected well." },
    { "05", "GRAIN LICKS", Badge::LIVE,    "LIVE",
      "Step sampler. One sample per slot, one 16-step pattern per slot, choke groups. This is how you make a beat." },
    { "06", "SURVIVOR",    Badge::LIVE,    "LIVE",
      "Master phrase looper. ARM captures 1\xe2\x80\x93""4 bars at the next bar boundary \xe2\x80\x94 every layer and its tails." },
    { "07", "MIXER",       Badge::PARTIAL, "PARTIAL",
      "Per-voice levels, mutes and meters." },
    { "08", "HW/SYNC",     Badge::LIVE,    "LIVE",
      "MIDI in: notes play the focused voice, CC 1 (mod wheel) writes its p0. The routing is fixed; there is no MIDI learn." },
    { "09", "EXHUME",      Badge::LIVE,    "LIVE",
      "Acquisition. Searches archive.org, resolves a storage node itself and pulls the audio down into %DIR%." },
    { "10", "PLATE",       Badge::LIVE,    "LIVE",
      "The visual wing. Generation loss by repeated ffmpeg passes, with lineage: every plate remembers what it was made from." },
    { "11", "TRANSPORT",   Badge::LIVE,    "LIVE",
      "RUN is master on. CUT is instant silence. REC writes a real WAV to %DIR%. BPM / BEATS / BARS / GAIN are live." },
    { "12", "SCOPE",       Badge::LIVE,    "LIVE",
      "Oscilloscope of the master bus, pre-gain and pre-mute, at 30 Hz. If the line is flat, nothing is sounding." },
    { "13", "STATUS",      Badge::LIVE,    "LIVE",
      "Bar, step, CPU against budget, and the CLIP lamp when the master hits the rails." },
    { "14", "EXPORT",      Badge::PLANNED, "NOT BUILT",
      "Stem export does not exist: the engine has no offline renderer. To capture audio, arm REC on the transport \xe2\x80\x94 it writes the master bus to %DIR%." },
};
static constexpr int kNumZones = (int) (sizeof zones / sizeof zones[0]);
static constexpr int kZoneRows = (kNumZones + 1) / 2;

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

/* ---- the keys, every one of which is checked against Main.cpp's keyPressed
 * and the panel that handles it. Two caps were removed here: M ("arm motion
 * capture") and %MOD%Z ("undo last edit"). Main.cpp:396-398 says in as many
 * words that the engine has neither feature, and keyPressed returns false for
 * both -- so those caps were drawn in the same PLATE_LOW/EDGE/INK style as
 * the working ones and did nothing at all. RACK does have an undo stack, but
 * it is reachable only through its STEP BACK plate and only for sculpt
 * nudges, which is not what a global undo key would mean. --------------- */

static const char* const keyRows[][2] =
{
    { "?",                    "Open / close this map" },
    { "RETURN",               "RACK: compile the expression" },
    { "1 \xe2\x80\x93 8",     "Focus voice 01\xe2\x80\x93""08 \xc2\xb7 with SHIFT: toggle it on/off. On GRAIN LICKS the digits focus a slot; on GRAIN MASS 1\xe2\x80\x93""4 select a well" },
    { "SPACE",                "RUN / mute master" },
    { "ESC",                  "CUT \xe2\x80\x94 panic silence. Closes this map first" },
    { "%MOD%R",               "Toggle REC" },
    { "P",                    "GRAIN MASS: play / stop the selected well" },
    { "R",                    "GRAIN MASS: reverse the selected well" },
    { "O",                    "GRAIN MASS: loop the selected well" },
    { "A / Z",                "GRAIN MASS: pitch the selected well up / down" },
    { "DRAG",                 "Knobs respond to horizontal drag; %MODW%-drag is fine mode" },
};
static constexpr int kNumKeys = (int) (sizeof keyRows / sizeof keyRows[0]);

/* The manual's copy is written once and printed on whatever machine is
 * running it. The placeholders are resolved here: the key cap said Cmd on a
 * Windows keyboard that has no Cmd key, and the LOCKER card named a
 * directory that does not exist on that machine.
 *   %DIR%   the console's real directory
 *   %MOD%   the modifier as a key cap    ("CTRL+" / U+2318)
 *   %MODW%  the modifier inside a sentence ("ctrl" / "cmd") */
static juce::String manualText (const char* raw)
{
    return U8 (raw).replace ("%DIR%",  morgueDirDisplay())
                   .replace ("%MODW%", modKeyWord())
                   .replace ("%MOD%",  modKeyGlyph());
}

/* The rule bodies and the header/footer frame rules used to be hardcoded
 * literals lifted from the HTML mock (#c9c4b8 and #2a2927). A literal carries
 * no measurement, so it cannot be checked, and it drifts off the ramp the
 * moment the ramp moves -- which is exactly what happened here: the frame
 * rule ended up dimmer than HAIRLINE. They are tokens at the call sites now
 * (C::INK_DIM 8.63:1, C::HAIRLINE 2.48:1 on PANEL) rather than file-scope
 * copies, because a file-scope copy of an inline header constant is a static
 * initialisation order question nobody needs to answer. */

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

/* section label row: label() caps, bottom hairline. */
static void drawMicroRow (juce::Graphics& g, Rectangle<int> row, const char* text,
                          int padX, bool topRule)
{
    if (topRule)
    {
        g.setColour (C::HAIRLINE);
        g.fillRect (row.getX(), row.getY(), row.getWidth(), 1);
    }
    g.setColour (C::INK_DIM);
    g.setFont (Type::label());
    g.drawText (text, row.reduced (padX, 0).withTrimmedBottom (1), Justification::centredLeft);
    g.setColour (C::HAIRLINE);
    g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
}

void FieldManualOverlay::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::MANUAL_BG);
    g.fillRect (b);

    /* body() is 11px now, so the 15px line box it used to be set in would
     * clip. Type::rowH() is the design system's answer for that: 18. */
    const juce::Font bodyF   = Type::body();
    const int        lineH   = Type::rowH (11.0f);

    /* ---- header 64: masthead, serial/rev, dismiss hint ------------------ */
    Rectangle<int> hdr = b.removeFromTop (64);
    g.setColour (C::HAIRLINE);
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
    const juce::Font metaF = Type::micro();
    const int metaBaseline = juce::roundToInt (
        (float) (contentBottom - 4) - metaF.getDescent());  // extra 4px pad-bottom
    g.setColour (C::INK_FAINT);
    g.setFont (metaF);
    g.drawSingleLineText (U8 ("MAP OF THIS CONSOLE \xc2\xb7 N.72-0418 \xc2\xb7 REV 12"),
                          hdr.getX() + 20 + mastW + 16, metaBaseline);
    g.setColour (C::INK_DIM);
    g.drawSingleLineText ("ESC / ? TO DISMISS", hdr.getRight() - 20, metaBaseline,
                          Justification::right);

    /* ---- footer 26: product line + where this console keeps its things ---
     * The right slot used to read "2 835 HEADLESS REGRESSION CHECKS PASSING":
     * a hardcoded string printed as though it were measured, which nothing
     * re-counts and which goes false silently. What belongs in a footer the
     * player reads when lost is the answer to "where did my recording go". */
    Rectangle<int> foot = b.removeFromBottom (26);
    g.setColour (C::PANEL);
    g.fillRect (foot);
    g.setColour (C::HAIRLINE);
    g.fillRect (foot.getX(), foot.getY(), foot.getWidth(), 1);
    Rectangle<int> fr = foot.withTrimmedTop (1).reduced (20, 0);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    /* Split the band rather than drawing both strings across all of it: on
     * Windows the console directory is a full path, and two overlapping
     * centred-left/centred-right draws is how footers become mush. */
    Rectangle<int> fRight = fr.removeFromRight (fr.getWidth() / 2);
    g.drawText (U8 ("MORGUE \xc2\xb7 INSTRUMENT FOR DARK NOISE, EXPERIMENTALISM, AMBIENCE"),
                fr, Justification::centredLeft, true);
    g.drawText (U8 ("EVERYTHING LANDS IN ") + morgueDirDisplay(),
                fRight, Justification::centredRight, true);

    /* ---- body: left = zones, right 400 = rules + keys ------------------- */
    Rectangle<int> body  = b;
    Rectangle<int> right = body.removeFromRight (400);

    // left column carries the 1px divider on its right edge
    g.setColour (C::HAIRLINE);
    g.fillRect (body.getRight() - 1, body.getY(), 1, body.getHeight());
    Rectangle<int> left = body.withTrimmedRight (1);

    drawMicroRow (g, left.removeFromTop (20), "ZONES", 20, false);

    /* zone card grid: 2 cols x kZoneRows, 1px HAIRLINE_DIM gaps, cards PANEL */
    {
        Rectangle<int> grid = left;
        g.setColour (C::HAIRLINE_DIM);
        g.fillRect (grid);

        const int gw = grid.getWidth();
        const int gh = grid.getHeight();
        const int c0 = (gw - 1) / 2;                 // col widths, 1px gap
        const int c1 = gw - 1 - c0;

        const juce::Font idxF  = Type::micro();
        const juce::Font nameF = Type::stencil (14.0f, 0.18f);
        const int idxW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (idxF, "01"));
        const int titleH = Type::rowH (14.0f);       // 22

        for (int i = 0; i < kNumZones; ++i)
        {
            const int row = i / 2, col = i % 2;
            // row edges: kZoneRows tracks + 1px gaps, remainder spread by rounding
            const int e0 = juce::roundToInt ((float) row       * (gh + 1) / (float) kZoneRows);
            const int e1 = juce::roundToInt ((float) (row + 1) * (gh + 1) / (float) kZoneRows);
            Rectangle<int> card (grid.getX() + (col == 0 ? 0 : c0 + 1),
                                 grid.getY() + e0,
                                 col == 0 ? c0 : c1,
                                 e1 - e0 - 1);
            if (card.getHeight() < 4) continue;

            g.setColour (C::PANEL);
            g.fillRect (card);

            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (card);
            Rectangle<int> inner = card.reduced (14, 8);

            // title line: index, stencil name, badge right
            Rectangle<int> tl = inner.removeFromTop (titleH);
            const int bl = baselineIn (nameF, tl.getY(), titleH);
            g.setColour (C::BLOOD);
            g.setFont (idxF);
            g.drawSingleLineText (zones[i].n, tl.getX(), bl);
            g.setColour (C::INK);
            g.setFont (nameF);
            g.drawSingleLineText (zones[i].name, tl.getX() + idxW + 8, bl);
            const int bw = StatusBadge::idealWidth (zones[i].badge);
            StatusBadge::paintBadge (g, tl.removeFromRight (bw).withSizeKeepingCentre (bw, 14),
                                     zones[i].k, zones[i].badge);

            // wrapping description, INK_DIM at 8.63:1
            inner.removeFromTop (4);
            drawWrapped (g, bodyF, C::INK_DIM,
                         wrapText (bodyF, manualText (zones[i].desc), inner.getWidth()),
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
        juce::StringArray lines = wrapText (bodyF, manualText (goldenRules[i]), textW);
        Rectangle<int> row = right.removeFromTop (lines.size() * lineH + 18);

        g.setColour (C::BLOOD);
        g.setFont (Type::micro());
        g.drawSingleLineText (juce::String::formatted ("%02d", i + 1),
                              row.getX() + 16,
                              baselineIn (Type::micro(), row.getY() + 9, lineH));
        drawWrapped (g, bodyF, C::INK_DIM, lines, textX, row.getY() + 9, lineH);

        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);
    }

    /* ---- right column: KEY REFERENCE ------------------------------------ */
    drawMicroRow (g, right.removeFromTop (20), "KEY REFERENCE", 16, true);

    {
        const int kh = right.getHeight();
        const juce::Font capF = Type::label();
        for (int i = 0; i < kNumKeys; ++i)
        {
            const int e0 = juce::roundToInt ((float) i       * kh / (float) kNumKeys);
            const int e1 = juce::roundToInt ((float) (i + 1) * kh / (float) kNumKeys);
            Rectangle<int> row (right.getX(), right.getY() + e0, right.getWidth(), e1 - e0);
            if (row.getHeight() < 4) continue;

            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (row);

            g.setColour (C::HAIRLINE_FAINT);
            g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

            // key-cap plate: min-width 64, pad 2/6, PLATE_LOW on EDGE border
            const juce::String key = manualText (keyRows[i][0]);
            const int capW = juce::jmax (68, juce::roundToInt (
                                 juce::GlyphArrangement::getStringWidth (capF, key)) + 14);
            Rectangle<int> cap = Rectangle<int> (row.getX() + 16, 0, capW, Type::rowH (10.0f) + 4)
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
            juce::StringArray lines = wrapText (bodyF, manualText (keyRows[i][1]), dw);
            const int blockH = lines.size() * lineH;
            drawWrapped (g, bodyF, C::INK_DIM, lines, dx,
                         row.getCentreY() - blockH / 2, lineH);
        }
    }
}

} // namespace morgue
