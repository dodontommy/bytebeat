/* Chrome.cpp -- see Chrome.h.
 *
 * Geometry and copy follow MORGUE_UI_SPEC.md sections 3, 4 and 13; where the
 * spec is silent the HTML frame "01 RACK" (and the transport bars of the
 * other frames) is followed exactly. */

#include "Chrome.h"
#include "AudioEngine.h"
#include "Session.h"
#include "bytebeat.h"
#include "engine.h"

/* std::abs on an int lives in <cstdlib>; <cmath> only promises the floating
 * overloads. libc++ happens to declare both from either header, MSVC does
 * not. std::make_unique is <memory>. */
#include <cmath>
#include <cstdlib>
#include <memory>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

/* HTML literals with no spec token (values verbatim from the mockup). */
static const juce::Colour TRANSPORT_EDGE { 0xff2a2927 };  // transport border-top / divider
static const juce::Colour INK_MID        { 0xffc9c4b8 };  // locker row name (idle)
static const juce::Colour EXPORT_BG      { 0xff161513 };  // EXPORT... plate bg

static int textW (const juce::Font& f, const juce::String& s)
{
    return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
}

/* ======================================================================== */
/*  TitleBar                                                                 */
/* ======================================================================== */

TitleBar::TitleBar()
{
    setTooltip (U8 ("MORGUE \xe2\x80\x94 drag to move the window, double-click to zoom. "
                    "The circles are close, minimise and zoom."));
}

static Rectangle<int> titleCircle (int i)
{
    return { 10 + i * 15, 8, 9, 9 };            // gap 6 between 9px circles
}

/* Which window this bar belongs to. The console is always inside a
 * ResizableWindow (Main.cpp's MainWindow); the null case is the offscreen
 * snapshot path, where there is no window to control. */
static juce::ResizableWindow* ownerWindow (juce::Component* c)
{
    return dynamic_cast<juce::ResizableWindow*> (c->getTopLevelComponent());
}

int TitleBar::controlAt (juce::Point<int> p) const
{
    for (int i = 0; i < NumControls; ++i)
        if (titleCircle (i).expanded (2).contains (p))
            return i;
    return -1;
}

void TitleBar::performControl (int which)
{
    auto* win = ownerWindow (this);

    switch (which)
    {
        case Close:
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            break;

        case Minimise:
            /* There is no native title bar to do this for us. The peer is the
             * only thing that knows how, and it exists on every platform JUCE
             * targets. */
            if (win != nullptr)
                if (auto* peer = win->getPeer())
                    peer->setMinimised (true);
            break;

        case Zoom:
            /* setFullScreen is JUCE's name for "as big as the work area lets
             * you be": maximise on Windows, zoom on macOS. Both toggle back to
             * the size the window had before. */
            if (win != nullptr)
                win->setFullScreen (! win->isFullScreen());
            break;

        default:
            break;
    }
}

void TitleBar::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::TRANSPORT);
    g.fillRect (b);
    g.setColour (C::HAIRLINE);
    g.fillRect (b.getX(), b.getBottom() - 1, b.getWidth(), 1);

    /* three 9px circle outlines. The hovered one brightens to INK_DIM -- a
     * hairline changing colour, which is the whole vocabulary this design
     * allows for "this is a control". No fill, no glyph, no gradient. */
    for (int i = 0; i < NumControls; ++i)
    {
        g.setColour (i == hoverControl ? C::INK_DIM : C::EDGE);
        g.drawEllipse (titleCircle (i).toFloat().reduced (0.5f), 1.0f);
    }

    // masthead: 11px condensed 700, .34em (HTML title bar)
    Rectangle<int> r = b.withTrimmedRight (10).withTrimmedBottom (1);
    r.removeFromLeft (49 + 12);                  // circles end at 49, gap 12
    const juce::Font mast = Type::cond (11.0f, 0.34f);
    g.setColour (C::INK);
    g.setFont (mast);
    g.drawText ("MORGUE", r.removeFromLeft (textW (mast, "MORGUE") + 2),
                Justification::centredLeft);

    // serial right (increments with the active panel, spec section 3). Taken
    // out of the run first so the path below can have the rest and be
    // ellipsised into it rather than drawn straight through the serial.
    g.setColour (C::INK_FAINT);
    const juce::Font serialF = Type::mono (9.0f, 0.14f);
    g.setFont (serialF);
    g.drawText (serial, r.removeFromRight (textW (serialF, serial) + 2),
                Justification::centredRight);
    r.removeFromRight (12);

    /* Session path: the REAL one, from the engine. This used to be the
     * hardcoded string "~/MORGUE/session.conf", which on Windows named a
     * directory that does not exist. Windows paths are also long, so the
     * middle is ellipsised rather than silently painted over the serial. */
    r.removeFromLeft (12);
    g.setFont (Type::mono (9.0f, 0.06f));
    g.drawText (morgue::sessionFileDisplay(), r, Justification::centredLeft, true);
}

void TitleBar::setSerial (const juce::String& s)
{
    if (serial == s) return;
    serial = s;
    repaint();
}

void TitleBar::mouseDown (const juce::MouseEvent& e)
{
    /* A press on a control is a press on that control, not the start of a
     * window drag -- otherwise the smallest tremor while clicking close
     * dragged the window instead. A maximised window is not draggable either;
     * dragging one would move it off its own work area. */
    auto* win = ownerWindow (this);
    draggingWindow = controlAt (e.getPosition()) < 0
                     && (win == nullptr || ! win->isFullScreen());

    if (draggingWindow)
        if (auto* top = getTopLevelComponent())
            dragger.startDraggingComponent (top, e.getEventRelativeTo (top));
}

void TitleBar::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingWindow)
        return;
    if (auto* top = getTopLevelComponent())
        dragger.dragComponent (top, e.getEventRelativeTo (top), nullptr);
}

void TitleBar::mouseUp (const juce::MouseEvent& e)
{
    draggingWindow = false;

    const int c = controlAt (e.getPosition());
    if (c >= 0 && e.mouseWasClicked())
        performControl (c);
}

void TitleBar::mouseMove (const juce::MouseEvent& e)
{
    const int c = controlAt (e.getPosition());
    if (c != hoverControl) { hoverControl = c; repaint(); }
}

void TitleBar::mouseExit (const juce::MouseEvent&)
{
    if (hoverControl != -1) { hoverControl = -1; repaint(); }
}

void TitleBar::mouseDoubleClick (const juce::MouseEvent& e)
{
    // the title-bar double-click zoom both window managers give you for free
    if (controlAt (e.getPosition()) < 0)
        performControl (Zoom);
}

/* ======================================================================== */
/*  StageTabs                                                                */
/* ======================================================================== */

static const char* const stageTabNames[StageTabs::numTabs] =
{
    "RACK", "ARRANGE", "GRAIN LICKS", "GRAIN MASS",
    "SURVIVOR", "MIXER", "HW/SYNC", "EXPORT"
};

static int infoCellWidth()
{
    // "INFO / ?", 9px .14em, padding 0 12
    return textW (Type::mono (9.0f, 0.14f), "INFO / ?") + 24;
}

StageTabs::StageTabs()
{
    setRepaintsOnMouseActivity (true);
    setTooltip (U8 ("STAGES \xe2\x80\x94 the eight workspaces of this console. Click to switch."));
}

const char* StageTabs::tabName (int i)
{
    return (i >= 0 && i < numTabs) ? stageTabNames[i] : "";
}

void StageTabs::setCurrent (int idx)
{
    idx = juce::jlimit (0, numTabs - 1, idx);
    if (cur == idx) return;
    cur = idx;
    repaint();
}

int StageTabs::tabWidth (int i) const
{
    // padding 0 15 each side
    return textW (Type::tab(), stageTabNames[i]) + 30;
}

int StageTabs::tabAt (int x) const
{
    int cx = 0;
    for (int i = 0; i < numTabs; ++i)
    {
        cx += tabWidth (i);
        if (x < cx) return i;
    }
    if (x >= getWidth() - infoCellWidth()) return numTabs;
    return -1;
}

void StageTabs::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL_ALT);
    g.fillRect (b);
    g.setColour (C::HAIRLINE);
    g.fillRect (b.getX(), b.getBottom() - 1, b.getWidth(), 1);

    const int th = b.getHeight() - 1;            // tabs sit above the strip rule
    int x = 0;
    for (int i = 0; i < numTabs; ++i)
    {
        const int w = tabWidth (i);
        Rectangle<int> t (x, 0, w, th);

        const bool active = i == cur;
        if (active)
        {
            g.setColour (C::TAB_ACTIVE_BG);
            g.fillRect (t);
            g.setColour (C::BLOOD);              // the only 2px rule in the app
            g.fillRect (t.getX(), t.getBottom() - 2, w, 2);
        }
        else if (i == hover)
        {
            g.setColour (C::PLATE_HOVER);
            g.fillRect (t);
        }

        g.setColour (C::HAIRLINE);               // border-right per tab
        g.fillRect (t.getRight() - 1, t.getY(), 1, th);

        g.setColour (active ? C::INK : C::TAB_INACTIVE_FG);
        g.setFont (Type::tab());
        g.drawText (stageTabNames[i], t, Justification::centred);
        x += w;
    }

    // INFO / ? cell far right, left border, 9px .14em
    Rectangle<int> ic (getWidth() - infoCellWidth(), 0, infoCellWidth(), th);
    g.setColour (C::HAIRLINE);
    g.fillRect (ic.getX(), ic.getY(), 1, th);
    g.setColour (hover == numTabs ? C::INK : C::TAB_INACTIVE_FG);
    g.setFont (Type::mono (9.0f, 0.14f));
    g.drawText ("INFO / ?", ic, Justification::centred);
}

void StageTabs::mouseDown (const juce::MouseEvent& e)
{
    int t = tabAt (e.x);
    if (t == numTabs)
    {
        if (onInfo) onInfo();
        return;
    }
    if (t >= 0 && t != cur)
    {
        cur = t;
        repaint();
        if (onSelect) onSelect (cur);
    }
}

void StageTabs::mouseMove (const juce::MouseEvent& e)
{
    int t = tabAt (e.x);
    if (t != hover) { hover = t; repaint(); }
}

void StageTabs::mouseExit (const juce::MouseEvent&)
{
    if (hover != -1) { hover = -1; repaint(); }
}

/* ======================================================================== */
/*  Locker                                                                   */
/* ======================================================================== */

Locker::Locker()
{
    setTooltip (U8 ("LOCKER \xe2\x80\x94 specimen archive of ") + morgue::morgueDirDisplay()
                + U8 (". Click a row to select it."));
    list.setRowHeight (26);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);

    growBtn = std::make_unique<PlateButton> ("GROW", false, false);
    growBtn->setTooltip (U8 ("GROW \xe2\x80\x94 render the FOCUSED VOICE as a "
                             "self-looping specimen in ") + morgue::morgueDirDisplay()
                         + U8 (": its expression, "
                               "knobs and post chain, drifting slowly, 4 bars at "
                               "the current tempo. Design the voice, then grow it. "
                               "Loop it in a GRAIN MASS well."));
    growBtn->onToggle = [this] (bool) { growSpecimen(); };
    addAndMakeVisible (*growBtn);

    refresh();
}

/* Render off the message thread -- the synthesis is a second or two of CPU
 * and must never stall the console. The engine call uses a private VM
 * context, so it is safe while the instrument plays. */
void Locker::growSpecimen()
{
    if (growing.exchange (true)) return;

    const juce::File dir = morgue::morgueDir();
    dir.createDirectory();
    const juce::String path = dir.getFullPathName();
    const unsigned seed = (unsigned) juce::Time::getMillisecondCounter()
                        ^ (unsigned) juce::Random::getSystemRandom().nextInt();

    const int focused = bb_clampi (atomic_load (&bb.focus), 0, BB_NLAYER - 1);

    juce::Component::SafePointer<Locker> safe (this);
    juce::Thread::launch ([safe, path, seed, focused]
    {
        char name[64] = { 0 };
        // the DIRECTED path: grow the voice the player designed
        int ok = bb_engine_render_specimen_voice (path.toRawUTF8(), seed, 4,
                                                  focused, name, sizeof name);
        // an empty layer has no voice to grow; fall back to raw material
        if (ok != 0)
            ok = bb_engine_render_specimen (path.toRawUTF8(), seed, 4,
                                            name, sizeof name);
        juce::MessageManager::callAsync ([safe, ok]
        {
            if (safe == nullptr) return;
            safe->growing.store (false);
            if (ok == 0) safe->refresh();
        });
    });
}

/* newest first, like the evidence log in the mockup */
struct LockerFileCmp
{
    static int compareElements (const juce::File& a, const juce::File& b)
    {
        const juce::int64 ta = a.getLastModificationTime().toMilliseconds();
        const juce::int64 tb = b.getLastModificationTime().toMilliseconds();
        return ta < tb ? 1 : ta > tb ? -1 : a.getFileName().compare (b.getFileName());
    }
};

void Locker::refresh()
{
    files.clear();
    const juce::File dir = morgue::morgueDir();
    if (dir.isDirectory())
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false))
            if (! f.isHidden())
                files.add (f);
    LockerFileCmp cmp;
    files.sort (cmp);
    list.updateContent();
    repaint();
}

juce::File Locker::selectedFile() const
{
    const int row = list.getSelectedRow();
    return (row >= 0 && row < files.size()) ? files.getReference (row)
                                            : juce::File();
}

void Locker::setContextHint (const juce::String& s)
{
    if (contextHint == s) return;
    contextHint = s;
    repaint();
}

void Locker::resized()
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (22);                       // header band (painted)
    Rectangle<int> foot = b.removeFromBottom (20);
    b.removeFromRight (1);                      // right-edge divider stays visible
    list.setBounds (b);
    growBtn->setBounds (foot.getRight() - 55, foot.getY() + 2, 50, 16);
}

void Locker::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    juce::String right = contextHint.isNotEmpty() ? contextHint
                                                  : morgue::morgueDirDisplay();
    paintHeaderBand (g, b.removeFromTop (22), "LOCKER", {}, right);

    // footer 20: count + PLANNED note, 8px .12em INK_FAINT, padding 0 8
    Rectangle<int> foot = b.removeFromBottom (20);
    g.setColour (C::HAIRLINE);
    g.fillRect (foot.getX(), foot.getY(), foot.getWidth(), 1);
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.12f));
    g.drawText (juce::String (files.size()) + " SPECIMENS",
                foot.reduced (8, 0), Justification::centredLeft);

    if (files.isEmpty())
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (9.0f, 0.10f));
        g.drawText ("NO SPECIMENS IN " + morgue::morgueDirDisplay(), b,
                    Justification::centred, true);
    }

    // right-edge divider against the main stage (spec section 3)
    g.setColour (C::HAIRLINE);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
}

int Locker::getNumRows() { return files.size(); }

void Locker::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= files.size()) return;
    const juce::File& f = files.getReference (row);

    if (selected)                                // bg #191816; no row separators
    {
        g.setColour (C::TAB_ACTIVE_BG);
        g.fillRect (0, 0, w, h);
    }

    // padding 0 8, gap 8: serial 52 / name flex / meta right
    Rectangle<int> r (0, 0, w, h);
    r.removeFromLeft (8);
    r.removeFromRight (8);

    g.setColour (selected ? C::BLOOD_HOT : C::INK_GHOST);
    g.setFont (Type::mono (8.0f, 0.06f));
    g.drawText (juce::String (row + 1).paddedLeft ('0', 4),
                r.removeFromLeft (52), Justification::centredLeft);
    r.removeFromLeft (8);

    // meta right, 8px INK_FAINT: PATCH for project files, else size
    const bool patch = f.hasFileExtension ("morgue") || f.hasFileExtension ("conf");
    juce::String meta = patch ? juce::String ("PATCH")
                              : juce::File::descriptionOfSizeInBytes (f.getSize())
                                    .replace (" ", "").toUpperCase();
    g.setColour (C::INK_FAINT);
    const juce::Font metaFont = Type::mono (8.0f);
    g.setFont (metaFont);
    const int mw = textW (metaFont, meta) + 2;
    g.drawText (meta, r.removeFromRight (mw), Justification::centredRight);
    r.removeFromRight (8);

    // name 10px, ellipsised; OXIDE when a .morgue patch
    g.setColour (selected ? C::INK_BRIGHT
                          : (f.hasFileExtension ("morgue") ? C::OXIDE : INK_MID));
    g.setFont (Type::mono (10.0f));
    g.drawText (f.getFileName(), r, Justification::centredLeft, true);
}

juce::String Locker::getTooltipForRow (int row)
{
    if (row < 0 || row >= files.size()) return {};
    return files.getReference (row).getFileName()
         + U8 (" \xe2\x80\x94 specimen in ") + morgue::morgueDirDisplay()
         + U8 (". Click to select.");
}

/* ======================================================================== */
/*  Scope                                                                    */
/* ======================================================================== */

Scope::Scope() { startTimerHz (30); }

void Scope::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::SOCKET);
    g.fillRect (b);

    // border-top: the divider against the LOCKER above
    g.setColour (C::HAIRLINE);
    g.fillRect (b.getX(), b.getY(), b.getWidth(), 1);
    b.removeFromTop (1);

    paintHeaderBand (g, b.removeFromTop (22), "SCOPE", {}, U8 ("MASTER \xc2\xb7 30Hz"));

    Rectangle<int> foot = b.removeFromBottom (18);
    Rectangle<int> plot = b.reduced (8);

    // centre-zero hairline
    g.setColour (C::HAIRLINE);
    g.fillRect (plot.getX(), plot.getCentreY(), plot.getWidth(), 1);

    // waveform: 1px INK polyline from the engine ring (min/max per column)
    constexpr unsigned len = 0x2000u;
    const unsigned wr = atomic_load_explicit (&bb.scope_w, memory_order_relaxed);
    const unsigned avail = juce::jmin (len, wr);
    const unsigned start = wr - avail;
    const int cols = plot.getWidth();

    int peak = 0;
    if (avail >= 2 && cols > 1)
    {
        const float ymid = (float) plot.getCentreY() + 0.5f;
        const float amp  = (float) (plot.getHeight() - 4) / 2.0f / 32768.0f;
        juce::Path p;
        for (int px = 0; px < cols; ++px)
        {
            const unsigned i0 = start + (unsigned) ((juce::uint64) px * avail / (unsigned) cols);
            unsigned i1 = start + (unsigned) ((juce::uint64) (px + 1) * avail / (unsigned) cols);
            if (i1 <= i0) i1 = i0 + 1;
            int lo = 32767, hi = -32768;
            for (unsigned i = i0; i < i1; ++i)
            {
                const int v = bb.scope[i & BB_SCOPE_MASK];
                if (v < lo) lo = v;
                if (v > hi) hi = v;
                if (std::abs (v) > peak) peak = std::abs (v);
            }
            const float x = (float) (plot.getX() + px) + 0.5f;
            if (px == 0) p.startNewSubPath (x, ymid - (float) hi * amp);
            else         p.lineTo          (x, ymid - (float) hi * amp);
            p.lineTo (x, ymid - (float) lo * amp);
        }
        g.setColour (C::INK);
        g.strokePath (p, juce::PathStrokeType (1.0f));
    }

    // footer 18: PRE-GAIN  PRE-MUTE ... PK -N.NdB
    g.setColour (C::INK_FAINT);
    const juce::Font ff = Type::mono (8.0f, 0.10f);
    g.setFont (ff);
    Rectangle<int> fr = foot.reduced (8, 0);
    g.drawText ("PRE-GAIN", fr.removeFromLeft (textW (ff, "PRE-GAIN") + 2),
                Justification::centredLeft);
    fr.removeFromLeft (10);
    g.drawText ("PRE-MUTE", fr.removeFromLeft (textW (ff, "PRE-MUTE") + 2),
                Justification::centredLeft);

    juce::String pk ("PK ");
    if (peak > 0)
    {
        const float dB = 20.0f * std::log10 ((float) peak / 32768.0f);
        pk << juce::String (dB, 1) << "dB";
    }
    else
        pk << U8 ("\xe2\x88\x92\xe2\x88\x9e") << "dB";   // −∞
    g.setColour (C::INK_DIM);
    g.drawText (pk, fr, Justification::centredRight);

    // right-edge divider against the main stage (spec section 3)
    g.setColour (C::HAIRLINE);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
}

/* ======================================================================== */
/*  TransportBar                                                             */
/* ======================================================================== */

namespace
{
    /* the MIXER-context EXPORT... action plate (HTML frame 06 transport) */
    class ExportPlate : public juce::Button
    {
    public:
        ExportPlate() : juce::Button ("EXPORT") {}

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            Rectangle<int> b = getLocalBounds();
            g.setColour (over ? C::PLATE_HOVER : EXPORT_BG);
            g.fillRect (b);
            g.setColour (C::EDGE);
            g.drawRect (b, 1);
            g.setColour (C::INK);
            g.setFont (Type::mono (10.0f, 0.16f));
            g.drawText (U8 ("EXPORT\xe2\x80\xa6"), b, Justification::centred);
        }
    };
}

static const char* const transportKnobLabels[4] = { "BPM", "BEATS", "BARS", "GAIN" };

TransportBar::TransportBar()
    : run ("RUN", true), cut ("CUT", true), rec ("REC", true), info ("?", false)
{
    run.setLampUnderText (true);
    cut.setLampUnderText (true);
    cut.setEngagedStyle (true);
    rec.setLampUnderText (true);

    run.setTooltip (U8 ("RUN \xe2\x80\x94 keep the engine audible. Toggle to mute the output."));
    run.onToggle = [] (bool on)
    {
        if (on) { atomic_store (&bb.mute, 0); atomic_store (&bb.panic, 0); }
        else    atomic_store (&bb.mute, 1);
    };
    addAndMakeVisible (run);

    cut.setTooltip (U8 ("CUT \xe2\x80\x94 instant master silence (panic). The loop survives."));
    cut.onToggle = [] (bool on) { atomic_store (&bb.panic, on ? 1 : 0); };
    addAndMakeVisible (cut);

    rec.setTooltip (U8 ("REC \xe2\x80\x94 record the master output to ")
                    + morgue::morgueDirDisplay() + morgue::pathSep()
                    + U8 ("*.wav."));
    rec.onToggle = [] (bool) {};   // wired from Main
    addAndMakeVisible (rec);

    info.setTooltip (U8 ("? \xe2\x80\x94 a map of this console."));
    addAndMakeVisible (info);

    auto mk = [this] (const juce::String& nm, int lo, int hi, int def, const juce::String& tip)
    {
        auto* k = new EngravedKnob (nm, 34, lo, hi, def);
        k->setShowText (false);                  // label/value drawn beside
        k->setTooltip (tip);
        addAndMakeVisible (k);
        knobs.add (k);
        return k;
    };
    mk ("BPM",   30, 240, 90,  U8 ("BPM \xe2\x80\x94 tempo. 30\xe2\x80\x93""240."));
    mk ("BEATS",  1, 16,  4,   U8 ("BEATS \xe2\x80\x94 beats per bar. 1\xe2\x80\x93""16."));
    mk ("BARS",   1, 16,  2,   U8 ("BARS \xe2\x80\x94 bars in the loop. 1\xe2\x80\x93""16."));
    mk ("GAIN",   0, 256, 180, U8 ("GAIN \xe2\x80\x94 master gain. 0\xe2\x80\x93""256."));

    knobs[0]->setValueQuiet (atomic_load (&bb.gctl[GCTL_BPM]));
    knobs[1]->setValueQuiet (atomic_load (&bb.gctl[GCTL_BEATS]));
    knobs[2]->setValueQuiet (atomic_load (&bb.gctl[GCTL_BARS]));
    knobs[3]->setValueQuiet (atomic_load (&bb.gain));

    knobs[0]->onChange = [] (int v) { atomic_store (&bb.gctl[GCTL_BPM], v); };
    knobs[1]->onChange = [] (int v) { atomic_store (&bb.gctl[GCTL_BEATS], v); };
    knobs[2]->onChange = [] (int v) { atomic_store (&bb.gctl[GCTL_BARS], v); };
    knobs[3]->onChange = [] (int v) { atomic_store (&bb.gain, v); };

    // RACK context: master meter, post-gain (the sink ring the WAV sees)
    masterMeter.setHorizontal (true);
    masterMeter.source = [this] { return masterPeak; };
    addAndMakeVisible (masterMeter);

    // MIXER context: EXPORT... plate
    exportBtn = std::make_unique<ExportPlate>();
    exportBtn->setTooltip (U8 ("EXPORT \xe2\x80\x94 open the stem render sheet."));
    exportBtn->onClick = [this] { if (onExport) onExport(); };
    addAndMakeVisible (*exportBtn);

    applyContext();
}

void TransportBar::resized()
{
    // padding 0 12; plates 64x34 gap 4; ? 52x34; divider; knob pairs
    run.setBounds  (12,  13, 64, 34);
    cut.setBounds  (80,  13, 64, 34);
    rec.setBounds  (148, 13, 64, 34);
    info.setBounds (216, 13, 52, 34);

    // divider at x = 282 (painted); pairs from 297: knob 34 + 8 + text 52 + 16
    int x = 297;
    for (auto* k : knobs)
    {
        k->setBounds (x, 13, 34, 34);
        x += 34 + 8 + 52 + 16;
    }

    masterMeter.setBounds (getWidth() - 12 - 120, 33, 120, 7);

    const int ew = textW (Type::mono (10.0f, 0.16f), U8 ("EXPORT\xe2\x80\xa6")) + 24;
    exportBtn->setBounds (getWidth() - 12 - ew, 17, ew, 26);
}

void TransportBar::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::TRANSPORT);
    g.fillRect (b);
    g.setColour (TRANSPORT_EDGE);                // HTML: border-top #2a2927
    g.fillRect (b.getX(), b.getY(), b.getWidth(), 1);

    // divider between plates and knobs: 1px x 38, #2a2927
    g.fillRect (282, 11, 1, 38);

    // knob readouts: 8px .14em label over 15px value, right of each knob
    for (int i = 0; i < knobs.size(); ++i)
    {
        auto* k = knobs.getUnchecked (i);
        Rectangle<int> t (k->getRight() + 8, 14, 52, 31);
        g.setColour (C::INK_DIM);
        g.setFont (Type::mono (8.0f, 0.14f));
        g.drawText (transportKnobLabels[i], t.removeFromTop (11), Justification::centredLeft);
        const int v = k->value();
        juce::String vs = (i == 1 || i == 2) ? juce::String (v).paddedLeft ('0', 2)
                                             : juce::String (v);
        g.setColour (C::INK);
        g.setFont (Type::data());
        g.drawText (vs, t, Justification::centredLeft);
    }

    /* per-tab context zone at the right (spec section 13 + each HTML frame) */
    Rectangle<int> ctx = b.reduced (12, 0);
    switch (contextTab)
    {
        case 0:                                  // RACK: master meter + labels
        {
            g.setColour (C::INK_FAINT);
            g.setFont (Type::mono (8.0f, 0.12f));
            g.drawText (U8 ("MASTER \xc2\xb7 POST-GAIN"),
                        Rectangle<int> (ctx.getRight() - 200, 6, 200, 11),
                        Justification::centredRight);

            juce::String db;
            if (masterPeak > 0.0001f)
                db = juce::String (20.0f * std::log10 (masterPeak), 1);
            else
                db = U8 ("\xe2\x88\x92\xe2\x88\x9e");     // −∞
            g.setFont (Type::mono (8.0f, 0.10f));
            g.drawText ("L " + db + " / R " + db + " dBFS",
                        Rectangle<int> (ctx.getRight() - 200, 43, 200, 11),
                        Justification::centredRight);
            break;
        }
        case 1:                                  // ARRANGE: song position
        {
            const unsigned bar = atomic_load (&bb.bar);
            const int sp = atomic_load (&bb.seq_pos);
            juce::String song ("SONG ");
            song << juce::String (bar).paddedLeft ('0', 3) << ":";
            if (sp >= 0)
                song << juce::String (sp / 4 + 1).paddedLeft ('0', 2) << ":"
                     << juce::String (sp % 4 + 1);
            else
                song << "--:-";
            g.setColour (C::INK_DIM);
            g.setFont (Type::mono (9.0f, 0.10f));
            g.drawText (song, Rectangle<int> (ctx.getRight() - 200, 17, 200, 12),
                        Justification::centredRight);
            g.setColour (C::INK_FAINT);
            g.setFont (Type::mono (8.0f, 0.06f));
            g.drawText (U8 ("SCRUB \xe2\x86\x92 bb.k"),
                        Rectangle<int> (ctx.getRight() - 200, 31, 200, 11),
                        Justification::centredRight);
            break;
        }
        case 5:                                  // MIXER: STEM EXPORT + plate
        {
            g.setColour (C::INK_FAINT);
            g.setFont (Type::mono (8.0f, 0.12f));
            g.drawText ("STEM EXPORT",
                        Rectangle<int> (0, 0, exportBtn->getX() - 10, getHeight()),
                        Justification::centredRight);
            break;
        }
        default:                                 // deadpan note (per-frame copy)
        {
            static const char* const contextNote[StageTabs::numTabs] =
            {
                "", "",
                "SLOT TRIGGERS SYNC TO GLOBAL 16TH GRID",         // GRAIN LICKS
                "",                                               // GRAIN MASS
                "CUT DOES NOT WIPE THE LOOP",                     // SURVIVOR
                "",
                "NOTES TRIGGER THE FOCUSED VOICE \xc2\xb7 TRANSPOSE BY NOTE", // HW/SYNC
                ""                                                // EXPORT (sheet covers)
            };
            const char* note = (contextTab >= 0 && contextTab < StageTabs::numTabs)
                                 ? contextNote[contextTab] : "";
            if (note[0] != 0)
            {
                g.setColour (C::INK_FAINT);
                g.setFont (Type::mono (8.0f, 0.12f));
                g.drawText (juce::String::fromUTF8 (note), ctx, Justification::centredRight);
            }
            break;
        }
    }
}

void TransportBar::sync()
{
    const bool muted = atomic_load (&bb.mute) != 0 || atomic_load (&bb.panic) != 0;
    run.setToggleStateQuiet (! muted);
    cut.setToggleStateQuiet (atomic_load (&bb.panic) != 0);
    if (! knobs[0]->isUserDragging()) knobs[0]->setValueQuiet (atomic_load (&bb.gctl[GCTL_BPM]));
    if (! knobs[1]->isUserDragging()) knobs[1]->setValueQuiet (atomic_load (&bb.gctl[GCTL_BEATS]));
    if (! knobs[2]->isUserDragging()) knobs[2]->setValueQuiet (atomic_load (&bb.gctl[GCTL_BARS]));
    if (! knobs[3]->isUserDragging()) knobs[3]->setValueQuiet (atomic_load (&bb.gain));

    // post-gain master peak from the sink ring (what REC and the wire see)
    {
        constexpr unsigned n = 4096;
        const unsigned w = atomic_load_explicit (&bb.sink_w, memory_order_relaxed);
        const unsigned avail = juce::jmin (n, w);
        int peak = 0;
        for (unsigned i = w - avail; i != w; ++i)
        {
            const int v = std::abs ((int) bb.sink[i & BB_SINK_MASK]);
            if (v > peak) peak = v;
        }
        masterPeak = (float) peak / 32768.0f;
    }

    repaint();                                   // readouts live in paint()
}

void TransportBar::applyContext()
{
    masterMeter.setVisible (contextTab == 0);
    exportBtn->setVisible (contextTab == 5);
}

void TransportBar::setContextTab (int t)
{
    if (contextTab == t) return;
    contextTab = t;
    applyContext();
    repaint();
}

/* ======================================================================== */
/*  StatusBar                                                                */
/* ======================================================================== */

StatusBar::StatusBar (AudioEngine& a) : audio (a)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void StatusBar::setAlert (const juce::String& text)
{
    if (alert == text) return;
    alert = text;
    repaint();
}

void StatusBar::timerCallback()
{
    if (atomic_load (&bb.clipping) != 0)
        lastClipMs = juce::Time::getMillisecondCounter();
    repaint();
}

void StatusBar::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL_ALT);
    g.fillRect (b);
    g.setColour (C::HAIRLINE);
    g.fillRect (b.getX(), b.getY(), b.getWidth(), 1);

    // padding 0 10, gap 16, all 9px .12em
    Rectangle<int> r = b.reduced (10, 0).withTrimmedTop (1);
    const juce::Font f = Type::mono (9.0f, 0.12f);
    g.setFont (f);

    auto seg = [&] (const juce::String& text, juce::Colour col)
    {
        g.setColour (col);
        g.drawText (text, r.removeFromLeft (textW (f, text) + 2),
                    Justification::centredLeft);
        r.removeFromLeft (14);                   // + the 2px slop = gap 16
    };

    seg ("BAR " + juce::String (atomic_load (&bb.bar)).paddedLeft ('0', 3),
         C::TAB_INACTIVE_FG);

    const int sp = atomic_load (&bb.seq_pos);
    seg ("STEP " + (sp >= 0 ? juce::String (sp + 1).paddedLeft ('0', 2)
                            : juce::String ("--")) + "/16",
         C::TAB_INACTIVE_FG);

    // CPU: INK_DIM < 60%, AMBER 60-85%, BLOOD_HOT above
    const int cpu = atomic_load (&bb.cpu_us);
    const int bud = atomic_load (&bb.budget_us);
    const int pct = bud > 0 ? juce::roundToInt (100.0f * (float) cpu / (float) bud) : 0;
    seg ("CPU " + juce::String (pct) + "%",
         pct < 60 ? C::INK_DIM : pct <= 85 ? C::AMBER : C::BLOOD_HOT);

    // CLIP: INK_GHOST until it fires, then BLOOD_HOT holding 800 ms
    const bool clipLit = lastClipMs != 0
        && juce::Time::getMillisecondCounter() - lastClipMs < 800;
    seg ("CLIP", clipLit ? C::BLOOD_HOT : C::INK_GHOST);

    // device rate / buffer, one segment with the only middot
    int rate = 0, bufSmp = 0;
    if (auto* dev = audio.getManager().getCurrentAudioDevice())
    {
        rate = (int) dev->getCurrentSampleRate();
        bufSmp = dev->getCurrentBufferSizeSamples();
    }
    seg (juce::String (rate) + " Hz " + U8 ("\xc2\xb7") + " "
             + juce::String (bufSmp) + " SMP",
         C::TAB_INACTIVE_FG);

    /* The right-hand slot is the hint until something goes wrong, then it is
     * the notice. There is nowhere else in this console for a device that
     * would not open or a session that would not write to say so, and both
     * used to fail without a word. */
    g.setColour (alert.isNotEmpty() ? C::BLOOD_HOT : C::INK_FAINT);
    g.setFont (f);
    g.drawText (alert.isNotEmpty() ? alert
                                   : juce::String ("PRESS ? FOR A MAP OF THIS CONSOLE"),
                r, Justification::centredRight, true);
}

} // namespace morgue
