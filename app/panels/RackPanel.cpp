/* RackPanel.cpp -- see RackPanel.h. All engine wiring preserved verbatim:
 * applyRack/rollVoice/bb_publish/bb_custom, role inference from bytecode,
 * step gates, the isUserDragging sync guards. Geometry is the RACK pixel
 * pass: spec section 5 + HTML frame "01 RACK".
 *
 * Spec section 15 contract notes honoured here:
 *   - RETURN compiles hot; caret and focus survive both the compile and the
 *     30 Hz sync (the editor owns its buffer; sync only overwrites it when
 *     the ENGINE changed the text and the editor is not focused).
 *   - Compile failure prints one deadpan line in the editor footer and the
 *     previous program keeps running (bb_publish already guarantees that).
 *   - The lock lane views one of the 16 engine lock lanes; the viewed lane
 *     follows the last-touched knob (p0-p7 / post chain).
 */

#include "RackPanel.h"
#include "bytebeat.h"
#include "engine.h"
#include "rack.h"
#include "gen.h"

#include <cmath>
#include <cstdio>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    /* HTML supporting literal: expression text / sounding-voice number. */
    const juce::Colour inkCode { 0xffc9c4b8 };

    juce::String mdot()  { return U8 (" · "); }   // " · "

    int textW (const juce::Font& f, const juce::String& s)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
    }

    /* 1px border drawn as four fillRects so hairlines never soften. */
    void frameRect (juce::Graphics& g, Rectangle<int> r, juce::Colour c)
    {
        g.setColour (c);
        g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
        g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);
        g.fillRect (r.getX(), r.getY() + 1, 1, r.getHeight() - 2);
        g.fillRect (r.getRight() - 1, r.getY() + 1, 1, r.getHeight() - 2);
    }

    /* One slot of a 16-column flex row with gap 2 -- shared by the step row
     * and the lock lane so their columns stay perfectly aligned. */
    Rectangle<int> stepSlotRect (Rectangle<int> row, int i)
    {
        const int n = 16, gap = 2;
        const int w = row.getWidth() + gap;
        const int x0 = row.getX() + (i * w) / n;
        const int x1 = row.getX() + ((i + 1) * w) / n - gap;
        return { x0, row.getY(), juce::jmax (1, x1 - x0), row.getHeight() };
    }

    void splitLines (const juce::String& t, juce::StringArray& out)
    {
        int start = 0;
        const int n = t.length();
        for (int i = 0; i <= n; ++i)
            if (i == n || t[i] == '\n')
            {
                out.add (t.substring (start, i));
                start = i + 1;
            }
    }

    /* The engine's tokenizer and the session file are single-line: a raw
     * '\n' must never reach bb_publish or bb_expr. The editor keeps its
     * line structure; this is the flatten at the compile boundary. */
    juce::String flattenExpr (const juce::String& s)
    {
        return s.replaceCharacter ('\n', ' ');
    }

    /* lock lane -> layer ctl mapping (mirrors the TUI's LOCK_LCTL table) */
    const int lockCtl[8] = { LCTL_LEVEL, LCTL_DRIVE, LCTL_TONE, LCTL_CRUSH,
                             LCTL_SPC_TIME, LCTL_SPC_FB, LCTL_SPC_MIX, LCTL_DECAY };
    const char* const lockCtlName[8] = { "LEVEL", "DRIVE", "TONE", "CRUSH",
                                         "SP-TIME", "SP-FB", "SP-MIX", "DECAY" };
} // namespace

/* ======================================================================== */
/*  VoicePlate -- 30x26 numbered plate, 4px lamp under the number.          */
/*  selected = BLOOD_DEEP/BLOOD/INK_BRIGHT + BLOOD_HOT lamp ·               */
/*  sounding = PLATE/#2a2927/#c9c4b8 + #4a4842 lamp · silent = INK_FAINT.   */
/* ======================================================================== */
class RackPanel::VoicePlate : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    explicit VoicePlate (int index) : idx (index)
    {
        setMouseClickGrabsKeyboardFocus (false);
        setTooltip (juce::String::formatted ("VOICE %02d", idx + 1)
                    + U8 (" — focus this layer for editing and MIDI; the lamp "
                          "shows it is sounding. SHIFT-click toggles it "
                          "on/off. Keys 1–8, SHIFT+1–8."));
    }

    std::function<void (int)> onSelect;
    std::function<void (int)> onToggle;     // shift-click: layer on/off

    void setStates (bool isSelected, bool isSounding)
    {
        if (sel == isSelected && snd == isSounding)
            return;
        sel = isSelected;
        snd = isSounding;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isShiftDown())
        {
            if (onToggle != nullptr)
                onToggle (idx);
        }
        else if (onSelect != nullptr)
            onSelect (idx);
    }

    void paint (juce::Graphics& g) override
    {
        Rectangle<int> b = getLocalBounds();
        const juce::Colour bg  = sel ? C::BLOOD_DEEP : C::PLATE;
        const juce::Colour bd  = sel ? C::BLOOD      : C::LAMP_DEAD;
        const juce::Colour fg  = sel ? C::INK_BRIGHT : (snd ? inkCode : C::INK_FAINT);
        const juce::Colour dot = sel ? C::BLOOD_HOT  : (snd ? C::LAMP_SOUNDING : C::HAIRLINE);

        g.setColour (bg);
        g.fillRect (b);
        frameRect (g, b, bd);

        g.setColour (fg);
        g.setFont (Type::mono (10.0f, 0.04f));
        g.drawText (juce::String::formatted ("%02d", idx + 1),
                    b.withTrimmedBottom (8), Justification::centred);

        g.setColour (dot);
        g.fillRect (b.getCentreX() - 2, b.getBottom() - 8, 4, 4);
    }

private:
    int idx;
    bool sel = false, snd = false;
};

/* ======================================================================== */
/*  SourceCell -- 22-tall "NN NAME" generator cell in the SOURCE grid.      */
/*  idle #101010/#1c1b19, num INK_GHOST, name INK_DIM ·                     */
/*  selected BLOOD_DEEP/BLOOD, num BLOOD_HOT, name ARMED_TEXT.              */
/* ======================================================================== */
class RackPanel::SourceCell : public juce::Component,
                              public juce::SettableTooltipClient
{
public:
    SourceCell (int index, const juce::String& nm)
        : idx (index), name (nm.toUpperCase())
    {
        setMouseClickGrabsKeyboardFocus (false);
    }

    std::function<void (int)> onSelect;

    void setSelected (bool s)
    {
        if (sel == s)
            return;
        sel = s;
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onSelect != nullptr)
            onSelect (idx);
    }

    void paint (juce::Graphics& g) override
    {
        Rectangle<int> b = getLocalBounds();
        g.setColour (sel ? C::BLOOD_DEEP : C::DISABLED_BG);
        g.fillRect (b);
        frameRect (g, b, sel ? C::BLOOD : C::HAIRLINE_DIM);

        Rectangle<int> r = b.reduced (6, 0);
        g.setColour (sel ? C::BLOOD_HOT : C::INK_GHOST);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (juce::String::formatted ("%02d", idx + 1),
                    r.removeFromLeft (13), Justification::centredLeft);
        r.removeFromLeft (5);
        g.setColour (sel ? C::ARMED_TEXT : C::INK_DIM);
        g.setFont (Type::mono (9.0f, 0.10f));
        g.drawText (name, r, Justification::centredLeft);
    }

private:
    int idx;
    juce::String name;
    bool sel = false;
};

/* ======================================================================== */
/*  ExpressionEditor -- the SOCKET code well (spec section 5).              */
/*  12px mono, line-height 1.5, INK_FAINT line numbers, 8x15 BLOOD block    */
/*  cursor, footer with byte count / compile age / VM op budget.            */
/*  RETURN compiles; shift-RETURN inserts a line. The buffer, caret and     */
/*  focus are owned here so nothing else can steal or stomp them.           */
/* ======================================================================== */
class RackPanel::ExpressionEditor : public juce::Component,
                                    public juce::SettableTooltipClient
{
public:
    ExpressionEditor()
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (true);
        setTooltip (U8 ("EXPRESSION — bytebeat program for the focused voice. "
                        "RETURN compiles hot, shift-RETURN inserts a line; a failed "
                        "compile keeps the previous program running."));
    }

    std::function<void (const juce::String&)> onCompile;

    const juce::String& text() const noexcept { return txt; }

    /* engine pull / layer switch; keeps the caret (clamped) and focus */
    void setTextQuiet (const juce::String& t)
    {
        if (txt == t)
            return;
        txt = t;
        caret = juce::jmin (caret, txt.length());
        ensureCaretVisible();
        repaint();
    }

    /* footer status pushed by the panel (engine truth, 30 Hz) */
    void setStatus (juce::Time compileStamp, const juce::String& deadpanReject,
                    int vmOpsUsed, int vmOpsBudget)
    {
        if (stamp == compileStamp && reject == deadpanReject
            && ops == vmOpsUsed && budget == vmOpsBudget)
        {
            tick();
            return;
        }
        stamp  = compileStamp;
        reject = deadpanReject;
        ops    = vmOpsUsed;
        budget = vmOpsBudget;
        shownAge = -1;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const juce::ModifierKeys mods = k.getModifiers();
        const int code = k.getKeyCode();

        if (code == juce::KeyPress::returnKey)
        {
            if (mods.isShiftDown())
            {
                insertText ("\n");
                return true;
            }
            if (onCompile != nullptr)
                onCompile (txt);
            return true;
        }
        if (code == juce::KeyPress::backspaceKey)
        {
            if (caret > 0)
            {
                txt = txt.substring (0, caret - 1) + txt.substring (caret);
                --caret;
                edited();
            }
            return true;
        }
        if (code == juce::KeyPress::deleteKey)
        {
            if (caret < txt.length())
            {
                txt = txt.substring (0, caret) + txt.substring (caret + 1);
                edited();
            }
            return true;
        }

        int line = 0, col = 0;
        locate (caret, line, col);

        if (code == juce::KeyPress::leftKey)
        {
            caret = mods.isCommandDown() ? posFor (line, 0)
                                         : juce::jmax (0, caret - 1);
            edited();
            return true;
        }
        if (code == juce::KeyPress::rightKey)
        {
            caret = mods.isCommandDown() ? posFor (line, lineLength (line))
                                         : juce::jmin (txt.length(), caret + 1);
            edited();
            return true;
        }
        if (code == juce::KeyPress::upKey)    { caret = posFor (line - 1, col); edited(); return true; }
        if (code == juce::KeyPress::downKey)  { caret = posFor (line + 1, col); edited(); return true; }
        if (code == juce::KeyPress::homeKey)  { caret = posFor (line, 0); edited(); return true; }
        if (code == juce::KeyPress::endKey)   { caret = posFor (line, lineLength (line)); edited(); return true; }

        if (mods.isCommandDown() || mods.isCtrlDown())
            return false;                       // cmd-R / cmd-Z etc. fall through

        const juce::juce_wchar c = k.getTextCharacter();
        if (c >= 32 && c < 127)                 // the language is plain ASCII
        {
            insertText (juce::String::charToString (c));
            return true;
        }
        return false;                           // ESC etc. reach the console keys
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const float cw  = charWidth();
        const int   gut = gutterWidth();
        const int line = firstLine + (e.y - padY) / lineH;
        const int col  = scrollCol
                       + juce::roundToInt ((float) (e.x - (padX + gut)) / cw);
        caret = posFor (line, juce::jmax (0, col));
        edited();
    }

    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override   { repaint(); }

    void paint (juce::Graphics& g) override
    {
        Rectangle<int> b = getLocalBounds();
        g.setColour (C::SOCKET);
        g.fillRect (b);

        const juce::Font f = Type::code();
        const float cw  = charWidth();
        const int   gut = gutterWidth();
        const int textX = padX + gut;
        const int availW = juce::jmax (8, getWidth() - textX - padX);
        const int cols  = juce::jmax (4, (int) std::floor ((float) availW / cw));
        const int rows  = visibleRows();

        juce::StringArray ls;
        splitLines (txt, ls);

        for (int r = 0; r < rows; ++r)
        {
            const int li = firstLine + r;
            if (li >= ls.size())
                break;
            const int y = padY + r * lineH;
            g.setFont (f);
            g.setColour (C::INK_FAINT);
            g.drawText (juce::String::formatted ("%02d", li + 1),
                        padX, y, gut, lineH, Justification::centredLeft);
            g.setColour (inkCode);
            g.drawText (ls[li].substring (scrollCol, scrollCol + cols),
                        textX, y, availW, lineH, Justification::centredLeft);
        }

        // 8x15 BLOOD block cursor (spec section 5)
        int cl = 0, cc = 0;
        locate (caret, cl, cc);
        if (cl >= firstLine && cl < firstLine + rows && cc >= scrollCol)
        {
            const int bx = textX + juce::roundToInt ((float) (cc - scrollCol) * cw);
            const int ly = padY + (cl - firstLine) * lineH;
            if (bx + 8 <= getWidth() - 2)
            {
                g.setColour (C::BLOOD);
                g.fillRect (bx, ly + (lineH - 15) / 2, 8, 15);
                if (cl < ls.size() && cc < ls[cl].length())
                {
                    g.setColour (C::INK_BRIGHT);
                    g.setFont (f);
                    g.drawText (ls[cl].substring (cc, cc + 1),
                                bx, ly, (int) std::ceil (cw), lineH,
                                Justification::centredLeft);
                }
            }
        }

        // footer: byte count / compile age (or the deadpan reject) / op budget
        const int fy = getHeight() - padY - footH;
        const juce::Font ff = Type::mono (9.0f, 0.08f);
        g.setFont (ff);

        const juce::String left = juce::String (ls.size()) + " LINES" + mdot()
                                + juce::String ((int) txt.getNumBytesAsUTF8()) + " BYTES";
        g.setColour (C::INK_FAINT);
        g.drawText (left, padX, fy, getWidth() - 2 * padX, footH,
                    Justification::centredLeft);

        juce::String midText;
        juce::Colour midCol = C::GREEN_FAINT;
        if (reject.isNotEmpty())
        {
            midText = reject;
            midCol  = C::BLOOD_HOT;
        }
        else if (stamp != juce::Time())
        {
            juce::int64 secs = (juce::int64) (juce::Time::getCurrentTime() - stamp).inSeconds();
            if (secs < 0)
                secs = 0;
            midText = juce::String::formatted ("COMPILED %02d:%02d:%02d AGO",
                                               (int) (secs / 3600),
                                               (int) ((secs / 60) % 60),
                                               (int) (secs % 60));
        }
        if (midText.isNotEmpty())
        {
            g.setColour (midCol);
            g.drawText (midText, padX + textW (ff, left) + 12, fy,
                        juce::jmax (0, getWidth() - padX - (padX + textW (ff, left) + 12)),
                        footH, Justification::centredLeft);
        }
        if (budget > 0)
        {
            g.setColour (C::INK_FAINT);
            g.drawText ("VM OPS " + juce::String (ops) + "/" + juce::String (budget),
                        0, fy, getWidth() - padX, footH, Justification::centredRight);
        }
    }

private:
    static constexpr int padX = 10, padY = 8, lineH = 18, footH = 12;

    void tick()   // repaint only when the displayed AGE second rolls over
    {
        if (stamp == juce::Time() || reject.isNotEmpty())
            return;
        const juce::int64 age = (juce::int64) (juce::Time::getCurrentTime() - stamp).inSeconds();
        if (age != shownAge)
        {
            shownAge = age;
            repaint();
        }
    }

    float charWidth() const
    {
        return juce::GlyphArrangement::getStringWidth (Type::code(), "0");
    }
    int gutterWidth() const
    {
        return textW (Type::code(), "00") + juce::roundToInt (charWidth());
    }
    int visibleRows() const
    {
        return juce::jmax (1, (getHeight() - padY * 2 - footH) / lineH);
    }
    int visibleCols() const
    {
        const int availW = getWidth() - (padX + gutterWidth()) - padX;
        return juce::jmax (8, (int) std::floor ((float) availW / charWidth()));
    }

    void locate (int pos, int& line, int& col) const
    {
        line = 0;
        col = 0;
        for (int i = 0; i < pos && i < txt.length(); ++i)
        {
            if (txt[i] == '\n') { ++line; col = 0; }
            else ++col;
        }
    }

    int posFor (int line, int col) const
    {
        juce::StringArray ls;
        splitLines (txt, ls);
        line = juce::jlimit (0, ls.size() - 1, line);
        col  = juce::jlimit (0, ls[line].length(), col);
        int pos = 0;
        for (int i = 0; i < line; ++i)
            pos += ls[i].length() + 1;
        return pos + col;
    }

    int lineLength (int line) const
    {
        juce::StringArray ls;
        splitLines (txt, ls);
        return line >= 0 && line < ls.size() ? ls[line].length() : 0;
    }

    void insertText (const juce::String& s)
    {
        if ((int) (txt.getNumBytesAsUTF8() + s.getNumBytesAsUTF8()) >= BB_EXPR_MAX)
            return;                              // engine buffer is the budget
        txt = txt.substring (0, caret) + s + txt.substring (caret);
        caret += s.length();
        edited();
    }

    void edited()
    {
        ensureCaretVisible();
        repaint();
    }

    void ensureCaretVisible()
    {
        int line = 0, col = 0;
        locate (caret, line, col);
        const int rows = visibleRows();
        if (line < firstLine)              firstLine = line;
        if (line >= firstLine + rows)      firstLine = line - rows + 1;
        const int cols = visibleCols();
        if (col < scrollCol)               scrollCol = col;
        if (col > scrollCol + cols - 1)    scrollCol = col - cols + 1;
        firstLine = juce::jmax (0, firstLine);
        scrollCol = juce::jmax (0, scrollCol);
    }

    juce::String txt;
    int caret = 0, firstLine = 0, scrollCol = 0;

    juce::Time   stamp;
    juce::String reject;
    int ops = 0, budget = 0;
    juce::int64 shownAge = -1;
};

/* ======================================================================== */
/*  LockLane -- the 16-slot parameter-lock lane, 40 tall, OXIDE fill from   */
/*  the bottom by value; a stored -1 (live knob) draws an empty slot.       */
/*  Drag sets a step; right-click returns it to the live knob.              */
/* ======================================================================== */
class RackPanel::LockLane : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    LockLane()
    {
        for (int i = 0; i < BB_STEPS; ++i)
            vals[i] = -1;
        setMouseClickGrabsKeyboardFocus (false);
        setTooltip (U8 ("LOCK LANE — per-step lock for the last-touched knob; "
                        "drag sets a step, right-click returns it to the live knob. "
                        "0–max of the target control."));
    }

    std::function<void (int step, int value)> onEdit;   // -1 clears to live knob

    bool isUserDragging() const noexcept { return isMouseButtonDown(); }

    void setValues (const int* v, int maxValue)     // 30 Hz pull; quiet
    {
        bool changed = false;
        const int m = juce::jmax (1, maxValue);
        if (m != maxV) { maxV = m; changed = true; }
        for (int i = 0; i < BB_STEPS; ++i)
            if (vals[i] != v[i]) { vals[i] = v[i]; changed = true; }
        if (changed)
            repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override { edit (e); }
    void mouseDrag (const juce::MouseEvent& e) override { edit (e); }

    void paint (juce::Graphics& g) override
    {
        Rectangle<int> row = getLocalBounds();
        for (int i = 0; i < BB_STEPS; ++i)
        {
            Rectangle<int> slot = stepSlotRect (row, i);
            g.setColour (C::PANEL);
            g.fillRect (slot);
            frameRect (g, slot, C::HAIRLINE_DIM);
            if (vals[i] >= 0)
            {
                Rectangle<int> inner = slot.reduced (1);
                const int fh = juce::roundToInt ((float) inner.getHeight()
                                * (float) juce::jmin (vals[i], maxV) / (float) maxV);
                g.setColour (C::OXIDE);
                g.fillRect (inner.getX(), inner.getBottom() - fh,
                            inner.getWidth(), fh);
            }
        }
    }

private:
    void edit (const juce::MouseEvent& e)
    {
        if (getWidth() < BB_STEPS)
            return;
        const int i = juce::jlimit (0, BB_STEPS - 1,
                                    (e.x * BB_STEPS) / (getWidth() + 2));
        if (e.mods.isPopupMenu())
        {
            vals[i] = -1;
            if (onEdit != nullptr)
                onEdit (i, -1);
            repaint();
            return;
        }
        Rectangle<int> inner = stepSlotRect (getLocalBounds(), i).reduced (1);
        const float fr = 1.0f - (float) (e.y - inner.getY())
                              / (float) juce::jmax (1, inner.getHeight());
        const int v = juce::jlimit (0, maxV, juce::roundToInt (fr * (float) maxV));
        vals[i] = v;
        if (onEdit != nullptr)
            onEdit (i, v);
        repaint();
    }

    int vals[BB_STEPS];
    int maxV = 255;
};

/* ======================================================================== */
/*  RackPanel                                                               */
/* ======================================================================== */

RackPanel::RackPanel()
    : bodyBtn ("BODY", true), spaceBtn ("SPACE", true),
      rollBtn ("ROLL", false, false), mutateBtn ("MUTATE", false, false)
{
    /* ---- voice strip -------------------------------------------------- */
    for (int i = 0; i < BB_NLAYER; ++i)
    {
        auto* v = new VoicePlate (i);
        v->onSelect = [this] (int L) { selectLayer (L); };
        v->onToggle = [this] (int L) { toggleVoice (L); };
        addAndMakeVisible (v);
        voicePlates.add (v);
    }

    rollBtn.setTooltip   (U8 ("ROLL — replaces the focused voice with a freshly "
                              "rolled one: expression, knobs, pattern. Action."));
    mutateBtn.setTooltip (U8 ("MUTATE — mutates the focused voice in place, "
                              "keeping its character. Action."));
    bodyBtn.setTooltip   (U8 ("BODY — wraps the source in a low-pass body stage "
                              "and recompiles without a glitch. Toggle."));
    spaceBtn.setTooltip  (U8 ("SPACE — wraps the voice in a feedback-delay space "
                              "stage and recompiles without a glitch. Toggle."));

    rollBtn.onToggle   = [this] (bool) { rollVoice (false); };
    mutateBtn.onToggle = [this] (bool) { rollVoice (true); };
    bodyBtn.onToggle   = [this] (bool on) {
        applyRack (bb_rack[layer].src, on ? 1 : 0, bb_rack[layer].space, false);
    };
    spaceBtn.onToggle  = [this] (bool on) {
        applyRack (bb_rack[layer].src, bb_rack[layer].body, on ? 1 : 0, false);
    };

    for (auto* b : { &bodyBtn, &spaceBtn, &rollBtn, &mutateBtn })
    {
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    /* ---- SOURCE grid (the Rack source table, expanded) ---------------- */
    for (int i = 0; i < rack_nsrc(); ++i)
    {
        auto* s = new SourceCell (i, rack_src_name (i));
        s->setTooltip (juce::String (rack_src_name (i)).toUpperCase()
                       + U8 (" — ") + rack_src_desc (i)
                       + " Audition on select; no-glitch swap.");
        s->onSelect = [this] (int src) {
            applyRack (src, bb_rack[layer].body, bb_rack[layer].space, true);
        };
        addAndMakeVisible (s);
        sourceCells.add (s);
    }

    /* ---- expression editor -------------------------------------------- */
    expr = std::make_unique<ExpressionEditor>();
    expr->onCompile = [this] (const juce::String& t) { compileExpression (t); };
    addAndMakeVisible (*expr);

    /* ---- patch morgue -------------------------------------------------- */
    for (int i = 0; i < rack_npatch(); ++i)
    {
        auto* c = new SourceCell (i, rack_patch (i)->name);
        c->setTooltip (juce::String (rack_patch (i)->name)
                       + U8 (" — a complete voice: ") + rack_patch (i)->src
                       + U8 (" source, envelope, post chain and chamber send. "
                             "Click to load it onto the focused voice."));
        c->onSelect = [this] (int p) { applyPatch (p); };
        addAndMakeVisible (c);
        patchCells.add (c);
    }

    /* ---- VOICE DESIGN: the five macros --------------------------------- */
    {
        struct MDef { const char* nm; const char* tip; };
        const MDef md[MACRO_COUNT] = {
            { "PITCH",  "PITCH — every pitch-carrying knob of this voice, "
                        "scaled together. 128 is the voice as designed." },
            { "MOTION", "MOTION — how fast the voice's internal pattern "
                        "moves. Down is glacial, up is frantic." },
            { "DIRT",   "DIRT — noise, drive and bitcrush together. "
                        "Down is clean, up is rotten." },
            { "DARK",   "DARK — every filter in the voice, inverted. "
                        "Up closes the lid." },
            { "ROOM",   "ROOM — resonance, echo and the chamber send. "
                        "Up puts the voice in the building." },
        };
        for (int a = 0; a < MACRO_COUNT; ++a)
        {
            auto* k = new EngravedKnob (md[a].nm, 40, 0, 255, 128);
            k->setMouseClickGrabsKeyboardFocus (false);
            k->setTooltip (U8 (md[a].tip));
            const int axis = a;
            k->onChange = [this, axis] (int v) { applyMacroAxis (axis, v); };
            addAndMakeVisible (k);
            designKnobs.add (k);
        }

        struct SDef { const char* nm; int axis, dir; const char* tip; };
        const SDef sd[6] = {
            { "DARKER",   MACRO_DARK,  +1, "DARKER — push the voice down into the floor." },
            { "CALMER",   MACRO_MOTION,-1, "CALMER — slow the voice's internal movement." },
            { "TIGHTER",  MACRO_ROOM,  -1, "TIGHTER — dry the voice out, close the room." },
            { "BRIGHTER", MACRO_DARK,  -1, "BRIGHTER — open the filters back up." },
            { "BUSIER",   MACRO_MOTION,+1, "BUSIER — speed the voice's internal movement." },
            { "HUGER",    MACRO_ROOM,  +1, "HUGER — more ring, more echo, more chamber." },
        };
        for (const auto& s : sd)
        {
            auto* b2 = new PlateButton (s.nm, false, false);
            b2->setMouseClickGrabsKeyboardFocus (false);
            b2->setTooltip (U8 (s.tip) + U8 (" Nudges, never gambles; STEP BACK undoes."));
            const int axis = s.axis, dir = s.dir;
            b2->onToggle = [this, axis, dir] (bool) { sculptNudge (axis, dir); };
            addAndMakeVisible (b2);
            sculptBtns.add (b2);
        }
        stepBackBtn = std::make_unique<PlateButton> ("STEP BACK", false, false);
        stepBackBtn->setMouseClickGrabsKeyboardFocus (false);
        stepBackBtn->setTooltip (U8 ("STEP BACK — undo the last sculpt nudge."));
        stepBackBtn->onToggle = [this] (bool) { popUndo(); };
        addAndMakeVisible (*stepBackBtn);
    }

    /* ---- p0-p7 -------------------------------------------------------- */
    for (int i = 0; i < BB_NPARAM; ++i)
    {
        auto* k = new EngravedKnob ("UNUSED", 44);
        k->setSubLabel ("p" + juce::String (i));
        k->setMouseClickGrabsKeyboardFocus (false);
        k->setTooltip (juce::String::formatted ("p%d", i)
                       + U8 (" — expression parameter; role inferred from the "
                             "compiled bytecode. 0–255; drag side-to-side, "
                             "cmd-drag fine, double-click default, scroll ±1."));
        const int p = i;
        k->onChange = [this, p] (int v) {
            atomic_store (&bb.layer[layer].param[p], v);
            setLockView (LOCK_P0 + p);
        };
        addAndMakeVisible (k);
        paramKnobs.add (k);
    }

    /* ---- post chain ---------------------------------------------------- */
    struct ChainDef { const char* nm; int ctl; int lock; const char* tip; };
    const ChainDef chain[6] = {
        { "DRIVE", LCTL_DRIVE,    LOCK_DRIVE,
          "DRIVE — post-chain saturation for this voice. 0–255." },
        { "TONE",  LCTL_TONE,     LOCK_TONE,
          "TONE — post-chain tone filter cutoff. 0–255." },
        { "CRUSH", LCTL_CRUSH,    LOCK_CRUSH,
          "CRUSH — bit/rate crush amount. 0–255." },
        { "TIME",  LCTL_SPC_TIME, LOCK_SPC_TIME,
          "TIME — SPACE delay time. 0–255; clocked when SP-SYNC is set." },
        { "FBACK", LCTL_SPC_FB,   LOCK_SPC_FB,
          "FBACK — SPACE feedback amount. 0–255." },
        { "MIX",   LCTL_SPC_MIX,  LOCK_SPC_MIX,
          "MIX — SPACE wet/dry balance. 0–255." },
    };
    for (const ChainDef& cd : chain)
    {
        auto* k = new EngravedKnob (cd.nm, 40);
        k->setMouseClickGrabsKeyboardFocus (false);
        k->setTooltip (U8 (cd.tip));
        const int c = cd.ctl, lk = cd.lock;
        k->onChange = [this, c, lk] (int v) {
            atomic_store (&bb.layer[layer].ctl[c], v);
            setLockView (lk);
        };
        addAndMakeVisible (k);
        chainKnobs.add (k);
    }

    /* ---- sequencer + lock lane ---------------------------------------- */
    for (int i = 0; i < BB_STEPS; ++i)
    {
        auto* c = new StepCell (i);
        c->setMouseClickGrabsKeyboardFocus (false);
        c->setTooltip (U8 ("STEP ") + juce::String (i + 1)
                       + U8 (" \xe2\x80\x94 click cycles OFF \xe2\x86\x92 HIT \xe2\x86\x92 "
                             "ACCENT, right-click clears, drag paints."));
        addAndMakeVisible (c);
        steps.add (c);
        c->onEdit = [this] (int step, StepCell::State s) {
            atomic_store (&bb.layer[layer].seq_gate[step], (int) s);
        };
    }

    lockLane = std::make_unique<LockLane>();
    lockLane->onEdit = [this] (int step, int v) {
        atomic_store (&bb.layer[layer].seq_lock[lockView][step], v);
    };
    addAndMakeVisible (*lockLane);

    rollVoice (false);
    sync();
}

RackPanel::~RackPanel() = default;

void RackPanel::grabExprFocus()
{
    if (expr != nullptr)
        expr->grabKeyboardFocus();
}

void RackPanel::focusVoice (int L)
{
    selectLayer (L);
}

void RackPanel::toggleVoice (int L)
{
    if (L < 0 || L >= BB_NLAYER)
        return;
    atomic_store (&bb.layer[L].on,
                  atomic_load (&bb.layer[L].on) != 0 ? 0 : 1);
    sync();
}

void RackPanel::selectLayer (int L)
{
    if (L < 0 || L >= BB_NLAYER || L == layer)
        return;
    layer = L;
    atomic_store (&bb.focus, L);
    rejectLine.clear();
    if (! macroState[L].captured)
        captureMacroBase (L);
    else
        for (int a = 0; a < MACRO_COUNT; ++a)
            designKnobs[a]->setValueQuiet (macroState[L].val[a]);
    loadExprFromEngine (true);
    sync();
    repaint();
}

void RackPanel::applyRack (int src, int body, int space, bool reseed)
{
    if (src < 0) src = 0;
    bb_rack[layer].src = (unsigned char) src;
    bb_rack[layer].body = (unsigned char) body;
    bb_rack[layer].space = (unsigned char) space;
    bb_rack[layer].mode = (unsigned char) rack_src_mode (src);
    bb_custom[layer] = 0;

    Layer* l = &bb.layer[layer];
    atomic_store (&l->mode, bb_rack[layer].mode);
    atomic_store (&l->on, 1);

    RackBuild b;
    rack_build (&bb_rack[layer], &b);
    int params[BB_NPARAM] = { 0 };
    rack_seed_params (&b, params);
    if (reseed)
        for (int p = 0; p < BB_NPARAM; ++p)
            atomic_store (&l->param[p], params[p]);

    snprintf (bb_expr[layer], BB_EXPR_MAX, "%s", b.expr);
    ExprError er;
    if (! bb_publish (layer, bb_expr[layer], &er))
        bb_publish (layer, "0", &er);

    bodyBtn.setToggleStateQuiet (body != 0);
    spaceBtn.setToggleStateQuiet (space != 0);
    compiledAt[layer] = juce::Time::getCurrentTime();
    rejectLine.clear();
    appliedPatch[layer] = -1;
    captureMacroBase (layer);
    loadExprFromEngine (true);
    sync();
}

void RackPanel::rollVoice (bool mutate)
{
    Voice v;
    unsigned seed = (unsigned) juce::Time::getMillisecondCounter() ^ 0x9E3779B9u;
    seed = mutate ? gen_mutate (seed, 0x1234u + (unsigned) layer, &v)
                  : gen_roll (seed, &v);

    Layer* l = &bb.layer[layer];
    atomic_store (&l->mode, v.mode);
    atomic_store (&l->on, 1);
    atomic_store (&l->seq_on, v.seq_on);
    for (int p = 0; p < BB_NPARAM; ++p)
        atomic_store (&l->param[p], v.p[p]);
    for (int c = 0; c < LCTL_COUNT; ++c)
        atomic_store (&l->ctl[c], v.ctl[c]);
    for (int i = 0; i < BB_STEPS; ++i)
    {
        atomic_store (&l->seq_gate[i], v.gate[i]);
        atomic_store (&l->seq_pitch[i], v.pitch[i]);
        atomic_store (&l->seq_ratchet[i], v.ratchet[i]);
        atomic_store (&l->seq_prob[i], v.prob[i]);
        for (int k = 0; k < BB_LOCK_COUNT; ++k)
            atomic_store (&l->seq_lock[k][i], v.lock[k][i]);
    }
    atomic_store (&l->motion_mask, v.motion_mask);

    bb_rack[layer] = v.rack;
    bb_custom[layer] = v.custom;
    snprintf (bb_expr[layer], BB_EXPR_MAX, "%s", v.expr);

    ExprError er;
    if (! bb_publish (layer, bb_expr[layer], &er))
        bb_publish (layer, "0", &er);

    compiledAt[layer] = juce::Time::getCurrentTime();
    rejectLine.clear();
    appliedPatch[layer] = -1;
    captureMacroBase (layer);
    loadExprFromEngine (true);
    sync();
}

/* ======================================================================== */
/*  Patch morgue + VOICE DESIGN macros + directed sculpting                  */
/* ======================================================================== */

void RackPanel::applyPatch (int idx)
{
    const RackPatch* pt = rack_patch (idx);

    int src = -1;
    for (int s = 0; s < rack_nsrc(); ++s)
        if (juce::String (rack_src_name (s)) == pt->src) { src = s; break; }
    if (src < 0)
        return;

    applyRack (src, pt->body, pt->space, true);   // seeds, publishes, custom=0

    Layer* l = &bb.layer[layer];
    for (int o = 0; o < pt->nset && o < RACK_PATCH_SET; ++o)
        atomic_store (&l->param[pt->set[o].idx % BB_NPARAM], (int) pt->set[o].val);
    atomic_store (&l->ctl[LCTL_DECAY],    (int) pt->decay);
    atomic_store (&l->ctl[LCTL_DRIVE],    (int) pt->drive);
    atomic_store (&l->ctl[LCTL_TONE],     (int) pt->tone);
    atomic_store (&l->ctl[LCTL_CRUSH],    (int) pt->crush);
    atomic_store (&l->ctl[LCTL_SPC_TIME], (int) pt->spc_t);
    atomic_store (&l->ctl[LCTL_SPC_FB],   (int) pt->spc_fb);
    atomic_store (&l->ctl[LCTL_SPC_MIX],  (int) pt->spc_mix);
    atomic_store (&l->ctl[LCTL_SPC_SYNC], pt->space ? 7 : 0);
    atomic_store (&l->seq_on, (int) pt->seq);
    atomic_store (&l->send,   (int) pt->send);

    appliedPatch[layer] = idx;
    captureMacroBase (layer);
    sync();
}

void RackPanel::captureMacroBase (int L)
{
    Macro& m = macroState[L];
    for (int i = 0; i < BB_NPARAM; ++i)
        m.baseP[i] = atomic_load (&bb.layer[L].param[i]);
    for (int i = 0; i < LCTL_COUNT; ++i)
        m.baseCtl[i] = atomic_load (&bb.layer[L].ctl[i]);
    m.baseSend = atomic_load (&bb.layer[L].send);
    for (int a = 0; a < MACRO_COUNT; ++a)
        m.val[a] = 128;
    m.captured = true;
    if (L == layer)
        for (int a = 0; a < MACRO_COUNT; ++a)
            designKnobs[a]->setValueQuiet (128);
}

/* What does each p-knob of the current voice MEAN? Slot kinds when the rack
 * built the expression; bytecode roles mapped onto kinds when it is custom.
 * Returns a KV_* kind per param, or -1 for "not a target". */
static void macroKinds (int L, int kinds[BB_NPARAM])
{
    for (int i = 0; i < BB_NPARAM; ++i) kinds[i] = -1;

    if (! bb_custom[L])
    {
        RackBuild b;
        rack_build (&bb_rack[L], &b);
        for (int s = 0; s < b.nslot; ++s)
            kinds[b.slot[s].pidx % BB_NPARAM] = b.slot[s].kind;
        return;
    }

    Program* pr = atomic_load (&bb.layer[L].prog);
    if (pr == nullptr) return;
    for (int i = 0; i < BB_NPARAM; ++i)
    {
        if ((pr->used_p & (1u << (unsigned) i)) == 0) continue;
        switch (pr->role[i])
        {
            case ROLE_MUL:    kinds[i] = KV_MUL;    break;
            case ROLE_PERIOD: kinds[i] = KV_PERIOD; break;
            case ROLE_RESON:  kinds[i] = KV_RESON;  break;
            case ROLE_SHIFT:  kinds[i] = KV_SHIFT;  break;
            case ROLE_MASK:   kinds[i] = KV_MASK;   break;
            case ROLE_CUT:    kinds[i] = KV_CUT;    break;
            case ROLE_Q:      kinds[i] = KV_Q;      break;
            case ROLE_LEVEL:  kinds[i] = KV_AMOUNT; break;
            case ROLE_DELAY:  kinds[i] = KV_TIME;   break;
            default: break;
        }
    }
}

void RackPanel::applyMacroAxis (int axis, int v)
{
    Macro& m = macroState[layer];
    if (! m.captured)
        captureMacroBase (layer);
    m.val[axis] = juce::jlimit (0, 255, v);

    const float t = (float) (m.val[axis] - 128) / 127.0f;   // -1..1

    /* reach-to-rails blend around the captured base */
    auto blend = [] (int base, float tt) -> int
    {
        const float r = tt >= 0 ? base + tt * (255.0f - base)
                                : base + tt * (float) base;
        return juce::jlimit (0, 255, juce::roundToInt (r));
    };
    /* exponential scale for pitch-like multipliers, +-1.5 octaves */
    auto scale = [] (int base, float tt) -> int
    {
        return juce::jlimit (1, 255,
            juce::roundToInt ((float) juce::jmax (1, base)
                              * std::pow (2.0f, tt * 1.5f)));
    };

    int kinds[BB_NPARAM];
    macroKinds (layer, kinds);
    Layer* l = &bb.layer[layer];

    for (int i = 0; i < BB_NPARAM; ++i)
    {
        const int k = kinds[i], base = m.baseP[i];
        if (k < 0) continue;
        switch (axis)
        {
        case MACRO_PITCH:
            if (k == KV_MUL || k == KV_RESON)
                atomic_store (&l->param[i], scale (base, t));
            else if (k == KV_PERIOD)                       // bigger = lower
                atomic_store (&l->param[i], scale (base, -t));
            break;
        case MACRO_MOTION:
            if (k == KV_SHIFT)                             // smaller = faster
                atomic_store (&l->param[i],
                              juce::jlimit (0, 31,
                                  base - juce::roundToInt (t * 4.0f)));
            else if (k == KV_AMOUNT)
                atomic_store (&l->param[i], blend (base, t));
            break;
        case MACRO_DIRT:
            if (k == KV_NOISE)                             // smaller = louder
                atomic_store (&l->param[i], blend (base, -t));
            else if (k == KV_MASK)
                atomic_store (&l->param[i], blend (base, t));
            break;
        case MACRO_DARK:
            if (k == KV_CUT)
                atomic_store (&l->param[i], blend (base, -t));
            break;
        case MACRO_ROOM:
            if (k == KV_Q)
                atomic_store (&l->param[i], blend (base, t));
            break;
        default: break;
        }
    }

    /* the post chain and the chamber are targets too */
    switch (axis)
    {
    case MACRO_DIRT:
        atomic_store (&l->ctl[LCTL_DRIVE], blend (m.baseCtl[LCTL_DRIVE], t));
        atomic_store (&l->ctl[LCTL_CRUSH], blend (m.baseCtl[LCTL_CRUSH], t));
        break;
    case MACRO_DARK:
        atomic_store (&l->ctl[LCTL_TONE],
                      juce::jlimit (1, 255, blend (m.baseCtl[LCTL_TONE], -t)));
        break;
    case MACRO_ROOM:
        atomic_store (&l->ctl[LCTL_SPC_MIX], blend (m.baseCtl[LCTL_SPC_MIX], t));
        atomic_store (&l->ctl[LCTL_SPC_FB],  blend (m.baseCtl[LCTL_SPC_FB],  t));
        atomic_store (&l->send,              blend (m.baseSend, t));
        break;
    default: break;
    }
}

void RackPanel::pushUndo()
{
    UndoStep u;
    u.layer = layer;
    for (int i = 0; i < BB_NPARAM; ++i)
        u.p[i] = atomic_load (&bb.layer[layer].param[i]);
    for (int i = 0; i < LCTL_COUNT; ++i)
        u.ctl[i] = atomic_load (&bb.layer[layer].ctl[i]);
    u.send = atomic_load (&bb.layer[layer].send);
    for (int a = 0; a < MACRO_COUNT; ++a)
        u.mval[a] = macroState[layer].val[a];
    undoStack.push_back (u);
    if (undoStack.size() > 32)
        undoStack.erase (undoStack.begin());
}

void RackPanel::popUndo()
{
    if (undoStack.empty())
        return;
    const UndoStep u = undoStack.back();
    undoStack.pop_back();

    Layer* l = &bb.layer[u.layer];
    for (int i = 0; i < BB_NPARAM; ++i) atomic_store (&l->param[i], u.p[i]);
    for (int i = 0; i < LCTL_COUNT; ++i) atomic_store (&l->ctl[i], u.ctl[i]);
    atomic_store (&l->send, u.send);
    for (int a = 0; a < MACRO_COUNT; ++a)
        macroState[u.layer].val[a] = u.mval[a];
    if (u.layer == layer)
        for (int a = 0; a < MACRO_COUNT; ++a)
            designKnobs[a]->setValueQuiet (u.mval[a]);
    sync();
}

void RackPanel::sculptNudge (int axis, int dir)
{
    pushUndo();
    const int jitter = sculptRng.nextInt (13) - 6;             // -6..+6
    const int nv = juce::jlimit (0, 255,
                       macroState[layer].val[axis] + dir * 26 + jitter);
    designKnobs[axis]->setValueQuiet (nv);
    applyMacroAxis (axis, nv);
}

/* RETURN in the editor: compile hot. On success the text becomes engine
 * truth (bb_expr) and the layer is marked custom; on failure bb_expr and the
 * running program are untouched and one deadpan line lands in the footer. */
void RackPanel::compileExpression (const juce::String& text)
{
    char buf[BB_EXPR_MAX];
    snprintf (buf, BB_EXPR_MAX, "%s", flattenExpr (text).toRawUTF8());

    ExprError e;
    if (bb_publish (layer, buf, &e))
    {
        atomic_store (&bb.layer[layer].on, 1);
        bb_custom[layer] = 1;
        snprintf (bb_expr[layer], BB_EXPR_MAX, "%s", buf);
        compiledAt[layer] = juce::Time::getCurrentTime();
        rejectLine.clear();
        appliedPatch[layer] = -1;
        captureMacroBase (layer);
        sync();
    }
    else
    {
        rejectLine = juce::String ("REJECTED") + mdot()
                   + "COL " + juce::String (e.col + 1) + mdot()
                   + juce::String (e.msg).toUpperCase() + mdot()
                   + "PREVIOUS PROGRAM RETAINED";
    }
    expr->grabKeyboardFocus();
}

void RackPanel::loadExprFromEngine (bool force)
{
    if (expr == nullptr)
        return;
    const juce::String src = bb_expr[layer][0] != 0
                           ? juce::String::fromUTF8 (bb_expr[layer])
                           : juce::String ("0");
    if (force)
    {
        expr->setTextQuiet (src);
        return;
    }
    /* Engine truth is newline-free; the editor may hold the same program
     * split across lines. Only overwrite when they genuinely differ. */
    if (! expr->hasKeyboardFocus (true) && flattenExpr (expr->text()) != src)
        expr->setTextQuiet (src);
}

void RackPanel::setLockView (int lane)
{
    if (lane < 0 || lane >= BB_LOCK_COUNT || lane == lockView)
        return;
    lockView = lane;
    syncLockLane();
    repaint (rcLockFoot);
}

void RackPanel::syncLockLane()
{
    if (lockLane == nullptr || lockLane->isUserDragging())
        return;
    Layer* l = &bb.layer[layer];
    int v[BB_STEPS];
    for (int i = 0; i < BB_STEPS; ++i)
        v[i] = atomic_load (&l->seq_lock[lockView][i]);
    lockLane->setValues (v, lockLaneMax (lockView));
}

int RackPanel::lockLaneMax (int lane) const
{
    if (lane < BB_NPARAM)
        return 255;
    return bb_lctl_info[lockCtl[lane - LOCK_LEVEL]].hi;
}

juce::String RackPanel::lockLaneName (int lane) const
{
    if (lane < BB_NPARAM)
    {
        juce::String n = "p" + juce::String (lane);
        Program* pr = atomic_load (&bb.layer[layer].prog);
        if (pr != nullptr && (pr->used_p & (1u << (unsigned) lane)) != 0)
        {
            const juce::String role (expr_role_name (pr->role[lane]));
            if (role.isNotEmpty())
                n << " " << role.toUpperCase();
        }
        return n;
    }
    return lockCtlName[lane - LOCK_LEVEL];
}

void RackPanel::sync()
{
    Layer* l = &bb.layer[layer];

    /* expression text: only when the ENGINE changed it and the editor is
     * not focused -- caret and focus survive the pull (spec section 15) */
    loadExprFromEngine (false);

    for (int i = 0; i < voicePlates.size(); ++i)
        voicePlates[i]->setStates (i == layer,
                                   atomic_load (&bb.layer[i].on) != 0);

    for (int i = 0; i < sourceCells.size(); ++i)
        sourceCells[i]->setSelected (i == (int) bb_rack[layer].src);
    for (int i = 0; i < patchCells.size(); ++i)
        patchCells[i]->setSelected (i == appliedPatch[layer]);

    if (! bodyBtn.isUserDragging())
        bodyBtn.setToggleStateQuiet (bb_rack[layer].body != 0);
    if (! spaceBtn.isUserDragging())
        spaceBtn.setToggleStateQuiet (bb_rack[layer].space != 0);

    /* knobs; roles inferred from the compiled bytecode */
    Program* pr = atomic_load (&l->prog);
    for (int i = 0; i < BB_NPARAM; ++i)
    {
        if (! paramKnobs[i]->isUserDragging())
            paramKnobs[i]->setValueQuiet (atomic_load (&l->param[i]));
        juce::String role ("UNUSED");
        if (pr != nullptr && (pr->used_p & (1u << (unsigned) i)) != 0)
        {
            role = juce::String (expr_role_name (pr->role[i])).toUpperCase();
            if (role.isEmpty())
                role = "MISC";
        }
        paramKnobs[i]->setRole (role);
    }

    static const int chainCtl[6] = { LCTL_DRIVE, LCTL_TONE, LCTL_CRUSH,
                                     LCTL_SPC_TIME, LCTL_SPC_FB, LCTL_SPC_MIX };
    for (int i = 0; i < 6; ++i)
        if (! chainKnobs[i]->isUserDragging())
            chainKnobs[i]->setValueQuiet (atomic_load (&l->ctl[chainCtl[i]]));

    /* editor footer readout (age ticks internally, one repaint per second) */
    if (expr != nullptr)
        expr->setStatus (compiledAt[layer], rejectLine,
                         pr != nullptr ? pr->n : 0, EXPR_CODE_MAX);

    const int play = atomic_load (&bb.seq_pos);
    for (int i = 0; i < BB_STEPS; ++i)
    {
        const int gate = atomic_load (&l->seq_gate[i]);
        steps[i]->setState ((StepCell::State) juce::jlimit (0, 2, gate), false);
        steps[i]->setPlayhead (i == play);
    }

    syncLockLane();

    /* the lock footer names program-dependent things; repaint on change */
    const juce::String foot = lockLaneName (lockView)
        + juce::String ((int) ((atomic_load (&l->motion_mask) >> lockView) & 1u));
    if (foot != lockFootCache)
    {
        lockFootCache = foot;
        repaint (rcLockFoot);
    }
}

void RackPanel::triggerFocused (int midiNote, int vel)
{
    bb_engine_note_on (layer, midiNote, vel);
}

/* ======================================================================== */
/*  layout (spec section 5)                                                 */
/* ======================================================================== */

void RackPanel::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);

    /* ---- voice strip 46: VOICE 34 · plates 30x26 gap 3 · | · ROLL
     *      MUTATE · | · BODY SPACE · right hints (painted) --------------- */
    rcStrip = b.removeFromTop (46);
    {
        Rectangle<int> s = rcStrip.reduced (10, 0);
        const int y = s.getCentreY() - 13;
        s.removeFromLeft (34 + 10);
        int x = s.getX();
        for (auto* v : voicePlates)
        {
            v->setBounds (x, y, 30, 26);
            x += 33;
        }
        x += 7;                                  // last gap 3 -> group gap 10
        rcStripDivA = { x, y, 1, 26 };
        x += 11;

        const juce::Font pf = Type::mono (10.0f, 0.16f);
        const int wr = textW (pf, "ROLL") + 24;
        rollBtn.setBounds (x, y, wr, 26);
        x += wr + 4;
        const int wm = textW (pf, "MUTATE") + 24;
        mutateBtn.setBounds (x, y, wm, 26);
        x += wm + 10;
        rcStripDivB = { x, y, 1, 26 };
        x += 11;
        const int wb = textW (pf, "BODY") + 31;  // 5px lamp + gap 6 + pad 20
        bodyBtn.setBounds (x, y, wb, 26);
        x += wb + 4;
        const int ws = textW (pf, "SPACE") + 31;
        spaceBtn.setBounds (x, y, ws, 26);
    }

    /* ---- SOURCE column 222: head 20 · 2-col grid of 22-cells · foot 22 - */
    rcSourceCol = b.removeFromLeft (222);
    {
        Rectangle<int> sc = rcSourceCol;

        /* PATCH MORGUE on top: composing starts from a sound */
        rcPatchHead = sc.removeFromTop (20);
        const int prows = (patchCells.size() + 1) / 2;
        Rectangle<int> pgrid = sc.removeFromTop (4 + prows * 23)
                                 .withTrimmedRight (1).reduced (6, 0);
        const int pcw = (pgrid.getWidth() - 1) / 2;
        for (int i = 0; i < patchCells.size(); ++i)
            patchCells[i]->setBounds (pgrid.getX() + (i % 2) * (pcw + 1),
                                      pgrid.getY() + 4 + (i / 2) * 23, pcw, 22);
        sc.removeFromTop (2);

        rcSourceHead = sc.removeFromTop (20);
        rcSourceFoot = sc.removeFromBottom (22);
        Rectangle<int> grid = sc.withTrimmedRight (1).reduced (6);
        const int cw = (grid.getWidth() - 1) / 2;
        for (int i = 0; i < sourceCells.size(); ++i)
            sourceCells[i]->setBounds (grid.getX() + (i % 2) * (cw + 1),
                                       grid.getY() + (i / 2) * 23, cw, 22);
    }

    /* ---- expression: head 20 · socket 122 · 1px rule ------------------- */
    Rectangle<int> mid = b;
    rcExprHead = mid.removeFromTop (20);
    expr->setBounds (mid.removeFromTop (122));
    mid.removeFromTop (1);                       // block border-bottom

    /* ---- VOICE DESIGN: head 20 · five macros + sculpt cluster ---------- */
    rcDesignHead = mid.removeFromTop (20);
    rcDesignArea = mid.removeFromTop (designKnobs[0]->idealHeight() + 12);
    {
        Rectangle<int> d = rcDesignArea.reduced (8, 6);
        for (int a = 0; a < designKnobs.size(); ++a)
            designKnobs[a]->setBounds (d.getX() + a * 62, d.getY(),
                                       56, d.getHeight());

        Rectangle<int> sculpt = d.withTrimmedLeft (designKnobs.size() * 62 + 16);
        const int rowH = 20, gap = 4;
        const int top = sculpt.getY()
                      + juce::jmax (0, (sculpt.getHeight() - rowH * 2 - gap) / 2);
        const int bw = 84;
        for (int i = 0; i < sculptBtns.size(); ++i)
        {
            const int col = i % 3, row = i / 3;
            sculptBtns[i]->setBounds (sculpt.getX() + col * (bw + gap),
                                      top + row * (rowH + gap), bw, rowH);
        }
        stepBackBtn->setBounds (sculpt.getX() + 3 * (bw + gap) + 8,
                                top, 84, rowH * 2 + gap);
    }
    mid.removeFromTop (1);                       // block border-bottom

    /* ---- p0-p7: head 20 · knob row (padding 8 6) · 1px rule ------------ */
    rcParamHead = mid.removeFromTop (20);
    rcParamArea = mid.removeFromTop (paramKnobs[0]->idealHeight() + 16);
    {
        Rectangle<int> prow = rcParamArea.reduced (6, 8);
        const int ih = paramKnobs[0]->idealHeight();
        for (int i = 0; i < paramKnobs.size(); ++i)
        {
            const int x0 = prow.getX() + (i * prow.getWidth()) / 8;
            const int x1 = prow.getX() + ((i + 1) * prow.getWidth()) / 8;
            paramKnobs[i]->setBounds (x0, prow.getY(), x1 - x0, ih);
        }
    }
    mid.removeFromTop (1);                       // block border-bottom

    /* ---- bottom: POST 420 | sequencer ---------------------------------- */
    rcPostCol = mid.removeFromLeft (420);
    {
        Rectangle<int> pc = rcPostCol;
        rcPostHead = pc.removeFromTop (20);
        rcPostFoot = pc.removeFromBottom (20);
        Rectangle<int> crow = pc.withTrimmedRight (1).reduced (6, 0);
        const int ih = chainKnobs[0]->idealHeight();
        const int y = crow.getCentreY() - ih / 2;
        for (int i = 0; i < chainKnobs.size(); ++i)
        {
            const int x0 = crow.getX() + (i * crow.getWidth()) / 6;
            const int x1 = crow.getX() + ((i + 1) * crow.getWidth()) / 6;
            chainKnobs[i]->setBounds (x0, y, x1 - x0, ih);
        }
    }

    Rectangle<int> seq = mid;
    rcSeqHead = seq.removeFromTop (20);
    Rectangle<int> sa = seq.reduced (10, 8);
    Rectangle<int> stepRow = sa.removeFromTop (38);
    for (int i = 0; i < steps.size(); ++i)
        steps[i]->setBounds (stepSlotRect (stepRow, i));
    sa.removeFromTop (6);
    lockLane->setBounds (sa.removeFromTop (40));
    sa.removeFromTop (6);
    rcLockFoot = sa.removeFromTop (12);
}

/* ======================================================================== */
/*  chrome (everything the children do not paint themselves)                */
/* ======================================================================== */

void RackPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "RACK",
                     U8 ("VOICE STATION · 8-LAYER BYTEBEAT"),
                     juce::String ("SPEC ") + SerialNo::RACK + U8 (" · REV 11"),
                     Badge::LIVE, "LIVE");

    /* ---- voice strip ---------------------------------------------------- */
    g.setColour (C::PANEL_ALT);
    g.fillRect (rcStrip);
    g.setColour (C::HAIRLINE);
    g.fillRect (rcStrip.getX(), rcStrip.getBottom() - 1, rcStrip.getWidth(), 1);
    g.fillRect (rcStripDivA);
    g.fillRect (rcStripDivB);

    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.14f));
    g.drawText ("VOICE", rcStrip.getX() + 10, rcStrip.getY(), 34,
                rcStrip.getHeight() - 1, Justification::centredLeft);
    {
        const juce::Font hf = Type::mono (8.0f, 0.10f);
        Rectangle<int> hr = rcStrip.reduced (10, 0).withTrimmedBottom (1);
        const juce::String h2 = U8 ("FOCUS → MIDI NOTE / CC");
        const juce::String h1 = U8 ("GATE ENV · DC-BLOCK ALWAYS ON");
        g.setFont (hf);
        g.setColour (C::INK_DIM);
        g.drawText (h2, hr.removeFromRight (textW (hf, h2)), Justification::centredRight);
        hr.removeFromRight (14);
        g.setColour (C::INK_FAINT);
        g.drawText (h1, hr, Justification::centredRight);
    }

    /* ---- SOURCE column: patch morgue over the raw generators ----------- */
    paintLabelRow (g, rcPatchHead,
                   juce::String ("PATCH MORGUE") + mdot() + "KNOWN GOOD",
                   juce::String (patchCells.size()) + " VOICES");
    g.setColour (C::HAIRLINE);
    g.fillRect (rcPatchHead.getX(), rcPatchHead.getBottom() - 1,
                rcPatchHead.getWidth(), 1);

    paintLabelRow (g, rcSourceHead,
                   juce::String ("SOURCE") + mdot()
                   + juce::String (sourceCells.size()) + " GENERATORS");
    g.setColour (C::HAIRLINE);
    g.fillRect (rcSourceHead.getX(), rcSourceHead.getBottom() - 1,
                rcSourceHead.getWidth(), 1);
    g.fillRect (rcSourceCol.getRight() - 1, rcSourceCol.getY(),
                1, rcSourceCol.getHeight());
    g.fillRect (rcSourceFoot.getX(), rcSourceFoot.getY(), rcSourceFoot.getWidth(), 1);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.10f));
    g.drawText (U8 ("AUDITION ON SELECT · NO-GLITCH SWAP"),
                rcSourceFoot.reduced (8, 0), Justification::centredLeft);

    /* ---- expression rows ------------------------------------------------ */
    paintLabelRow (g, rcExprHead,
                   juce::String ("EXPRESSION") + mdot()
                   + juce::String::formatted ("VOICE %02d", layer + 1) + mdot()
                   + "BYTEBEAT VM",
                   U8 ("RETURN = COMPILE · CURSOR HELD"));
    g.setColour (C::HAIRLINE);
    g.fillRect (rcExprHead.getX(), rcExprHead.getBottom() - 1, rcExprHead.getWidth(), 1);
    if (expr != nullptr)
        g.fillRect (rcExprHead.getX(), expr->getBottom(), rcExprHead.getWidth(), 1);

    /* ---- VOICE DESIGN rows ---------------------------------------------- */
    paintLabelRow (g, rcDesignHead,
                   U8 ("VOICE DESIGN · FIVE HANDS ON WHATEVER IS LOADED"),
                   U8 ("128 = AS DESIGNED · SCULPT NUDGES, NEVER GAMBLES"));
    g.setColour (C::HAIRLINE);
    g.fillRect (rcDesignHead.getX(), rcDesignHead.getBottom() - 1,
                rcDesignHead.getWidth(), 1);
    g.fillRect (rcDesignArea.getX(), rcDesignArea.getBottom(),
                rcDesignArea.getWidth(), 1);

    /* ---- p0-p7 rows ----------------------------------------------------- */
    paintLabelRow (g, rcParamHead,
                   U8 ("EXPRESSION PARAMETERS · p0–p7 · "
                       "ROLE INFERRED FROM BYTECODE"),
                   U8 ("0–255 · DRAG SIDE-TO-SIDE · "
                       "RIGHT-CLICK → LEARN (R8)"));
    g.setColour (C::HAIRLINE);
    g.fillRect (rcParamHead.getX(), rcParamHead.getBottom() - 1, rcParamHead.getWidth(), 1);
    g.fillRect (rcParamArea.getX(), rcParamArea.getBottom(), rcParamArea.getWidth(), 1);

    /* ---- post chain ------------------------------------------------------ */
    paintLabelRow (g, rcPostHead, U8 ("POST CHAIN · PER-VOICE DIRT"));
    g.setColour (C::HAIRLINE);
    g.fillRect (rcPostHead.getX(), rcPostHead.getBottom() - 1, rcPostHead.getWidth(), 1);
    g.fillRect (rcPostCol.getRight() - 1, rcPostCol.getY(), 1, rcPostCol.getHeight());
    g.fillRect (rcPostFoot.getX(), rcPostFoot.getY(), rcPostFoot.getWidth(), 1);

    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.10f));
    g.drawText (U8 ("DRIVE → TONE → CRUSH → SPACE(T/FB/MIX)"),
                rcPostFoot.reduced (10, 0), Justification::centredLeft);
    {
        const juce::String tag = "R4 INSERTS PLANNED";
        const juce::Font tf = Type::mono (8.0f, 0.10f);
        const int tw = textW (tf, tag) + 10;
        Rectangle<int> tr = rcPostFoot.reduced (10, 0)
                                .removeFromRight (tw)
                                .withSizeKeepingCentre (tw, 14);
        frameRect (g, tr, C::OXIDE_DIM);
        g.setColour (C::OXIDE);
        g.setFont (tf);
        g.drawText (tag, tr, Justification::centred);
    }

    /* ---- sequencer head -------------------------------------------------- */
    {
        Rectangle<int> hr = rcSeqHead.reduced (10, 0);
        g.setColour (C::INK_DIM);
        g.setFont (Type::label());
        const juce::String t1 = U8 ("SEQUENCER · 1 BAR / 16TH");
        g.drawText (t1, hr, Justification::centredLeft);
        const int w1 = textW (Type::label(), t1);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (U8 ("CLICK CELL → OFF / HIT / ACCENT"),
                    hr.withTrimmedLeft (w1 + 10), Justification::centredLeft);
        g.drawText (U8 ("EUCLID · PROB · RATCHET"),
                    hr, Justification::centredRight);
        g.setColour (C::HAIRLINE);
        g.fillRect (rcSeqHead.getX(), rcSeqHead.getBottom() - 1, rcSeqHead.getWidth(), 1);
    }

    /* ---- lock footer ------------------------------------------------------ */
    {
        Layer* l = &bb.layer[layer];
        const bool smooth = ((atomic_load (&l->motion_mask) >> lockView) & 1u) != 0;
        const juce::String left =
            juce::String::formatted ("LOCK LANE %02d/%02d", lockView + 1, BB_LOCK_COUNT)
            + mdot() + lockLaneName (lockView)
            + mdot() + (smooth ? "MOTION: SMOOTH" : "MOTION: STEP");
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (left, rcLockFoot, Justification::centredLeft);
        g.setColour (C::INK_DIM);
        g.drawText ("M = CAPTURE MOTION (R3)", rcLockFoot, Justification::centredRight);
    }
}

} // namespace morgue
