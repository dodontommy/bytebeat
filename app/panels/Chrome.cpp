/* Chrome.cpp -- see Chrome.h.
 *
 * Geometry and copy follow MORGUE_UI_SPEC.md sections 3, 4 and 13; where the
 * spec is silent the HTML frame "01 RACK" (and the transport bars of the
 * other frames) is followed exactly. */

#include "Chrome.h"
#include "AudioEngine.h"
#include "Session.h"
#include "Ledger.h"
#include "bytebeat.h"
#include "engine.h"

/* std::abs on an int lives in <cstdlib>; <cmath> only promises the floating
 * overloads. libc++ happens to declare both from either header, MSVC does
 * not. std::make_unique is <memory>. */
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

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
    "SURVIVOR", "MIXER", "HW/SYNC", "EXHUME", "PLATE", "EXPORT"
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

/* ---- the group vocabulary ----------------------------------------------
 * The first eight are morgue::Kind, in the order a piece of material moves
 * through the instrument: grown, captured, recorded, acquired, scanned,
 * plated, exported, filed as a recipe. The last three are for material the
 * register has never heard of, which on any real machine is most of it on
 * day one -- files placed here by hand, and everything grown before
 * ACCESSION existed. They are classified by shape, not invented into the
 * register; nothing here writes a ledger record. */
enum LockerGroup
{
    LG_SPC = 0, LG_CLIP, LG_REC, LG_ACQ, LG_SCN, LG_PLT, LG_EXP, LG_RCP,
    LG_AUDIO, LG_SESSION, LG_OTHER, LG_COUNT
};

static_assert (LG_COUNT <= 16, "Locker::collapsed[] is sized 16");

namespace
{
    struct LockerGroupDef { const char* tag; const char* desc; };

    const LockerGroupDef lockerGroups[LG_COUNT] =
    {
        { "SPC",  "GROWN SPECIMENS"      },
        { "CLIP", "ARRANGEMENT CAPTURES" },
        { "REC",  "MASTER RECORDINGS"    },
        { "ACQ",  "ACQUIRED AUDIO"       },
        { "SCN",  "CAPTURED IMAGES"      },
        { "PLT",  "RENDERED PLATES"      },
        { "EXP",  "EXPORTS"              },
        { "RCP",  "RECIPES"              },
        { "AUD",  "UNFILED AUDIO"        },
        { "SES",  "SESSION FILES"        },
        { "OTH",  "OTHER MATERIAL"       },
    };

    int groupForKind (Kind k) noexcept
    {
        switch (k)
        {
            case Kind::Spc:  return LG_SPC;
            case Kind::Clip: return LG_CLIP;
            case Kind::Rec:  return LG_REC;
            case Kind::Acq:  return LG_ACQ;
            case Kind::Scn:  return LG_SCN;
            case Kind::Plt:  return LG_PLT;
            case Kind::Exp:  return LG_EXP;
            case Kind::Rcp:  return LG_RCP;
            default:         return LG_OTHER;
        }
    }

    bool isAudioName (const juce::String& n)
    {
        return n.endsWithIgnoreCase (".wav")  || n.endsWithIgnoreCase (".aif")
            || n.endsWithIgnoreCase (".aiff") || n.endsWithIgnoreCase (".mp3")
            || n.endsWithIgnoreCase (".ogg")  || n.endsWithIgnoreCase (".flac");
    }

    bool isSessionName (const juce::String& n)
    {
        return n.endsWithIgnoreCase (".conf") || n.endsWithIgnoreCase (".morgue")
            || n.endsWithIgnoreCase (".ledger");
    }

    /* AudioEngine::WavRecorder names its output "%Y-%m-%d_%H-%M-%S.wav" (see
     * AudioEngine.cpp:251), which carries no prefix at all -- so a recording
     * is recognised by the only thing it has, its stamp. Checked structurally
     * rather than by regex because this runs once per file per walk. */
    bool looksLikeRecStamp (const juce::String& n)
    {
        if (n.length() < 19 || ! n.endsWithIgnoreCase (".wav")) return false;
        auto d = [&n] (int i) { return juce::CharacterFunctions::isDigit (n[i]); };
        return d (0) && d (1) && d (2) && d (3) && n[4] == '-'
            && d (5) && d (6) && n[7] == '-' && d (8) && d (9) && n[10] == '_';
    }

    /* The register is the authority. The filename prefix is only consulted
     * when there is no record -- which is exactly the case the prefixes were
     * invented for, and exactly the case in which they collide (see the note
     * at the top of Ledger.h about SPC-%04X). Being in the right GROUP is a
     * weaker claim than having an identity, so a guess is safe here in a way
     * that printing a guessed serial would not be. */
    int classifyName (const juce::String& n, const Record* rec)
    {
        if (rec != nullptr && rec->kind != Kind::Unknown)
            return groupForKind (rec->kind);

        if (n.startsWithIgnoreCase ("SPC-"))  return LG_SPC;
        if (n.startsWithIgnoreCase ("CLIP-")) return LG_CLIP;
        if (n.startsWithIgnoreCase ("REC-"))  return LG_REC;
        if (n.startsWithIgnoreCase ("ACQ-"))  return LG_ACQ;
        if (n.startsWithIgnoreCase ("SCN-"))  return LG_SCN;
        if (n.startsWithIgnoreCase ("PLT-"))  return LG_PLT;
        if (n.startsWithIgnoreCase ("EXP-"))  return LG_EXP;
        if (n.startsWithIgnoreCase ("RCP-"))  return LG_RCP;

        if (isSessionName (n))     return LG_SESSION;
        if (looksLikeRecStamp (n)) return LG_REC;
        if (isAudioName (n))       return LG_AUDIO;
        return LG_OTHER;
    }

    juce::String baseNameOf (const juce::String& storedPath)
    {
        const int i = juce::jmax (storedPath.lastIndexOfChar ('/'),
                                  storedPath.lastIndexOfChar ('\\'));
        return i >= 0 ? storedPath.substring (i + 1) : storedPath;
    }

    /* Clearance is the one thing in this panel allowed near the accent, and
     * only at its worst. CLEARED is deliberately NOT green: GREEN_FAINT has
     * exactly one use in the spec and "everything is fine" is not a state
     * this console announces. */
    juce::Colour clearanceColour (Clearance c)
    {
        switch (c)
        {
            case Clearance::PersonalOnly: return C::BLOOD_HOT;
            case Clearance::Review:       return C::AMBER;
            case Clearance::Cleared:      return C::INK_DIM;
            default:                      return C::INK_FAINT;
        }
    }

    /* The one registration glyph this design owns, drawn as two hairlines. */
    void paintRegistrationCross (juce::Graphics& g, int x, int y, int arm = 3)
    {
        g.fillRect (x - arm, y, arm * 2 + 1, 1);
        g.fillRect (x, y - arm, 1, arm * 2 + 1);
    }

    constexpr int kScanMaxDepth = 6;      // ACQ/<identifier>/<file> needs 2
    constexpr int kScanMaxDirs  = 512;
    constexpr int kScanMaxFiles = 20000;
    constexpr int kScanIdleMs   = 2000;

    constexpr int kLockerRowH   = 28;
    constexpr int kLockerFilterH = 26;
}

/* ======================================================================== */
/*  Locker::Scanner -- the walk, and the rules for when it happens again     */
/* ======================================================================== */
/*
 * THE CACHE, AND WHEN IT IS WRONG.
 *
 * A locker with thousands of files across ACQ/<identifier>/ subdirectories is
 * tens of thousands of stat calls. That cannot happen on the message thread
 * (it runs the 30 Hz engine sync and every paint routine) and it must not
 * happen at 30 Hz on any thread. So the listing is a cache, and the whole
 * design question is when to invalidate it.
 *
 * Two triggers, and both are cheap:
 *
 *   EXPLICIT.  refresh() is a poke, not a scan. Everything that knowingly
 *   writes into the locker -- a finished GROW, REC stopping, an ARRANGE
 *   capture -- already called it, and those call sites are unchanged. Pokes
 *   coalesce: ten in a row cost one extra pass, not ten.
 *
 *   OBSERVED.  Everything else writes into this directory behind our back:
 *   EXHUME's download workers, the visual wing's ffmpeg output, the player
 *   with a file manager. So every kScanIdleMs the thread takes a SIGNATURE --
 *   the modification time of the root and of each directory the last walk
 *   found, plus the ledger's size, mtime and record count. That is one stat
 *   per DIRECTORY (a handful), not per file (thousands), and adding or
 *   removing an entry is exactly what bumps a directory's mtime on NTFS, APFS
 *   and ext4 alike. The ledger is in the signature because a row shows the
 *   register's serial and origin: a record landing while nothing on disk
 *   moved still has to re-letter the list.
 *
 * The signature cannot see a file appearing inside a directory that did not
 * exist at the last walk -- but creating that directory bumps its parent,
 * which is in the set, so the next pass finds both. The one case it misses is
 * a file whose CONTENTS changed with no entry added or removed, which changes
 * nothing this panel paints except the size, and the explicit poke covers the
 * writers we own.
 *
 * The thread sleeps in wait(), so notify() from refresh() wakes it at once;
 * idle cost is a handful of stats every two seconds on a background-priority
 * thread. Nothing here touches a JUCE Component: results go back through
 * MessageManager::callAsync and a Component::SafePointer, the shape
 * growSpecimen() established below.
 */
class Locker::Scanner final : public juce::Thread
{
public:
    Scanner (Locker& o, juce::File ledgerFileIn)
        : juce::Thread ("MORGUE LOCKER SCAN"),
          safe (&o),
          ledgerFile (std::move (ledgerFileIn))
    {}

    void poke() { dirty.store (true); notify(); }

    void run() override
    {
        while (! threadShouldExit())
        {
            const bool forced = dirty.exchange (false);
            const juce::uint64 sig = signature();

            if (forced || sig != lastSignature)
            {
                scan();
                /* Re-take it AFTER the walk: the walk is what discovers the
                 * directory set the signature is computed over, and on the
                 * first pass that set was empty. Without this the second pass
                 * would always see a "change" and walk again. */
                lastSignature = signature();
            }

            wait (kScanIdleMs);
        }
    }

private:
    juce::uint64 signature()
    {
        juce::uint64 h = 0xcbf29ce484222325ULL;
        auto mix = [&h] (juce::int64 v)
        {
            for (int b = 0; b < 8; ++b)
            {
                h ^= (juce::uint64) (((juce::uint64) v >> (b * 8)) & 0xff);
                h *= 0x100000001b3ULL;
            }
        };

        const juce::File root = morgue::morgueDir();
        mix (root.isDirectory() ? root.getLastModificationTime().toMilliseconds() : -1);
        mix ((juce::int64) watched.size());

        for (const auto& d : watched)
            mix (d.getLastModificationTime().toMilliseconds());

        const bool haveLedger = ledgerFile.existsAsFile();
        mix (haveLedger ? ledgerFile.getLastModificationTime().toMilliseconds() : -1);
        mix (haveLedger ? ledgerFile.getSize() : -1);
        mix ((juce::int64) Ledger::shared().size());
        return h;
    }

    void scan()
    {
        const juce::File root = morgue::morgueDir();

        /* One snapshot of the register, then O(1) probes per file. Asking
         * Ledger::findByFile() per file would be O(files x records) with the
         * data lock taken and released thousands of times; all() copies the
         * vector once under one lock and we index it here. */
        const std::vector<Record> recs = Ledger::shared().all();
        juce::HashMap<juce::String, int> byRel, byBase;
        for (int i = 0; i < (int) recs.size(); ++i)
        {
            const juce::String& p = recs[(size_t) i].file;
            if (p.isEmpty()) continue;
            byRel.set (p, i);
            /* Last writer wins, matching Ledger::findByFile()'s newest-first
             * basename fallback: a name that really was reused resolves to
             * the most recent accession, which is the one the LOCKER shows. */
            byBase.set (baseNameOf (p).toLowerCase(), i);
        }

        std::vector<juce::File> dirs;
        std::vector<Entry> found;
        bool hitCap = false;

        if (root.isDirectory())
        {
            struct Node { juce::File dir; int depth; };
            std::vector<Node> queue;
            queue.push_back ({ root, 0 });
            dirs.push_back (root);

            for (size_t qi = 0; qi < queue.size() && ! hitCap; ++qi)
            {
                if (threadShouldExit()) return;
                const Node node = queue[qi];      // by value: the vector grows below

                /* FollowSymlinks::no is not paranoia. The session root is a
                 * directory the player owns and can drop anything into,
                 * including a link back to one of its own ancestors, and a
                 * loop there would spin this thread until the machine was
                 * restarted. The depth and count caps are the same argument
                 * made twice. */
                for (const auto& f : node.dir.findChildFiles (
                         juce::File::findFilesAndDirectories, false, "*",
                         juce::File::FollowSymlinks::no))
                {
                    if (threadShouldExit()) return;
                    if (f.isHidden()) continue;

                    if (f.isDirectory())
                    {
                        if (node.depth + 1 <= kScanMaxDepth
                            && (int) queue.size() < kScanMaxDirs)
                        {
                            queue.push_back ({ f, node.depth + 1 });
                            dirs.push_back (f);
                        }
                        continue;
                    }

                    if ((int) found.size() >= kScanMaxFiles) { hitCap = true; break; }

                    Entry e;
                    e.file     = f;
                    e.name     = f.getFileName();
                    e.size     = f.getSize();
                    e.modified = f.getLastModificationTime().toMilliseconds();
                    e.patch    = isSessionName (e.name);

                    const juce::String rel =
                        f.getRelativePathFrom (root).replaceCharacter ('\\', '/');
                    e.subdir = rel.upToLastOccurrenceOf ("/", false, false);
                    if (e.subdir == rel) e.subdir.clear();       // lives in the root

                    const Record* rec = nullptr;
                    if (byRel.contains (rel))
                        rec = &recs[(size_t) byRel[rel]];
                    else if (byBase.contains (e.name.toLowerCase()))
                        rec = &recs[(size_t) byBase[e.name.toLowerCase()]];

                    if (rec != nullptr)
                    {
                        e.serial    = rec->serial;
                        e.origin    = rec->origin;
                        e.clearance = (int) rec->clearance;
                    }

                    e.group  = classifyName (e.name, rec);
                    e.search = (e.name + " " + e.origin + " " + e.serial + " "
                                + e.subdir).toLowerCase();
                    found.push_back (std::move (e));
                }
            }
        }

        /* Group first, then newest-first inside the group -- the evidence-log
         * order the flat list had, kept, but no longer the ONLY structure. */
        std::stable_sort (found.begin(), found.end(),
                          [] (const Entry& a, const Entry& b)
        {
            if (a.group != b.group)       return a.group < b.group;
            if (a.modified != b.modified) return a.modified > b.modified;
            return a.name.compareIgnoreCase (b.name) < 0;
        });

        watched = std::move (dirs);

        /* Retry any ledger line an append could not deliver -- a failed write
         * leaves the record in memory and its line on a queue, so a session
         * degrades to "your provenance is not durable YET" rather than losing
         * it. flushPending() is documented WORKER-THREAD-ONLY, this is a
         * worker, and the LOCKER is the one panel that reports the backlog,
         * so it is also the right one to clear it. Free when the queue is
         * empty: one lock and an empty-vector test. */
        if (Ledger::shared().pendingCount() > 0)
            Ledger::shared().flushPending();

        auto payload = std::make_shared<std::vector<Entry>> (std::move (found));
        const int  pending = Ledger::shared().pendingCount();
        const bool cap = hitCap;
        auto sp = safe;

        juce::MessageManager::callAsync ([sp, payload, pending, cap]
        {
            if (sp == nullptr) return;
            sp->adoptScan (std::move (*payload), pending, cap);
        });
    }

    juce::Component::SafePointer<Locker> safe;   // made on the message thread
    juce::File ledgerFile;                       // resolved on the message thread
    std::vector<juce::File> watched;             // scanner-thread only
    juce::uint64 lastSignature = 0;
    std::atomic<bool> dirty { true };            // the first pass always walks
};

/* ======================================================================== */

Locker::Locker()
{
    setTooltip (U8 ("LOCKER \xe2\x80\x94 specimen archive of ") + morgue::morgueDirDisplay()
                + U8 (", read all the way down. Grouped by kind; click a header "
                      "to fold it. Type to filter on name, origin or serial. "
                      "Click a row to select it and read its provenance; drag it "
                      "onto a GRAIN LICKS slot, or out of the window into another "
                      "application."));
    list.setRowHeight (kLockerRowH);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setMultipleSelectionEnabled (false);
    addAndMakeVisible (list);

    /* The text filter. ESCAPE and RETURN are both bound deliberately: an
     * unhandled key in a TextEditor propagates to MainComponent::keyPressed,
     * where ESCAPE is CUT (master panic) and SPACE is RUN. Typing the name of
     * a specimen must not silence the instrument. */
    filterBox.setMultiLine (false);
    filterBox.setReturnKeyStartsNewLine (false);
    filterBox.setFont (Type::mono (9.0f, 0.06f));
    filterBox.setJustification (Justification::centredLeft);
    filterBox.setColour (juce::TextEditor::textColourId, C::INK);
    filterBox.setColour (juce::TextEditor::backgroundColourId, C::SOCKET);
    filterBox.setColour (juce::TextEditor::outlineColourId, C::HAIRLINE);
    filterBox.setColour (juce::TextEditor::focusedOutlineColourId, C::EDGE);
    filterBox.setColour (juce::TextEditor::highlightColourId, C::BLOOD_DEEP);
    filterBox.setColour (juce::TextEditor::highlightedTextColourId, C::INK_BRIGHT);
    filterBox.setColour (juce::CaretComponent::caretColourId, C::BLOOD_HOT);
    filterBox.setTextToShowWhenEmpty ("FILTER", C::INK_GHOST);
    filterBox.setTooltip (U8 ("FILTER \xe2\x80\x94 matches the file name, the register's "
                              "origin and the serial. Several words all have to match. "
                              "ESC clears it."));
    filterBox.onTextChange = [this]
    {
        filterText = filterBox.getText().trim().toLowerCase();
        rebuildRows();
        resized();
        repaint();
    };
    filterBox.onEscapeKey = [this] { filterBox.setText ({}, true); };
    filterBox.onReturnKey = [this] { list.grabKeyboardFocus(); };
    addAndMakeVisible (filterBox);

    kindBtn = std::make_unique<PlateButton> ("ALL", false, false);
    kindBtn->setTooltip (U8 ("KIND \xe2\x80\x94 show one class of material only: "
                             "grown specimens, arrangement captures, master "
                             "recordings, acquisitions, scans, plates."));
    kindBtn->onToggle = [this] (bool) { showKindMenu(); };
    addAndMakeVisible (*kindBtn);

    growBtn = std::make_unique<PlateButton> ("GROW", false, false);
    growBtn->setTooltip (U8 ("GROW \xe2\x80\x94 render the FOCUSED VOICE as a "
                             "self-looping specimen in ") + morgue::morgueDirDisplay()
                         + U8 (": its expression, "
                               "knobs and post chain, drifting slowly, 4 bars at "
                               "the current tempo. Design the voice, then grow it. "
                               "Loop it in a GRAIN MASS well."));
    growBtn->onToggle = [this] (bool) { growSpecimen(); };
    addAndMakeVisible (*growBtn);

    /* Resolve the ledger's path HERE, on the message thread, and hand the
     * File to the worker. Ledger::file() memoises into a mutable member, and
     * this call is guaranteed to be the first one: the LOCKER is a
     * MainComponent member, so it is constructed before the constructor body
     * that calls Ledger::bootstrap() and starts the register's own worker. */
    scanner = std::make_unique<Scanner> (*this, Ledger::shared().file());
    scanner->startThread (juce::Thread::Priority::background);
}

Locker::~Locker()
{
    /* Join before anything else is destroyed. After stopThread() returns
     * there is no scanner thread, so nothing can be copying our SafePointer
     * while ~Component clears it, and any callAsync already queued finds a
     * null SafePointer and does nothing. */
    if (scanner != nullptr)
        scanner->stopThread (3000);
    scanner.reset();
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

/* ---- invalidation ------------------------------------------------------ */

void Locker::refresh()
{
    /* Not a scan: an invalidation. The walk happens on the LOCKER's own
     * worker and lands in adoptScan(). Callers (a finished GROW, REC
     * stopping, an ARRANGE capture) may fire this as often as they like --
     * overlapping pokes coalesce into one extra pass. */
    if (scanner != nullptr)
        scanner->poke();
}

void Locker::adoptScan (std::vector<Entry> found, int pending, bool truncatedWalk)
{
    entries       = std::move (found);
    scanned       = true;
    truncated     = truncatedWalk;
    pendingWrites = pending;

    /* A selection is a path, not a row number, so it survives a rescan that
     * renumbers everything -- which is the whole reason the old row-index
     * "serial" was untenable in the first place. */
    if (selectedPath != juce::File() && entryForFile (selectedPath) == nullptr)
        selectedPath = juce::File();

    rebuildRows();
    selectFileRow (selectedPath);
    updateDetail();
    resized();
    repaint();
}

/* ---- filter, grouping, selection ---------------------------------------- */

bool Locker::filterActive() const
{
    return filterText.isNotEmpty() || kindFilter >= 0;
}

void Locker::rebuildRows()
{
    rows.clear();
    visibleCount = 0;

    juce::StringArray terms;
    terms.addTokens (filterText, " ", "");
    terms.removeEmptyStrings();

    for (int gi = 0; gi < LG_COUNT; ++gi)
    {
        if (kindFilter >= 0 && kindFilter != gi) continue;

        /* entries is already sorted group-major, newest-first, so one pass
         * per group preserves that order without a second sort. */
        std::vector<int> hits;
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const Entry& e = entries[(size_t) i];
            if (e.group != gi) continue;

            bool ok = true;
            for (const auto& t : terms)
                if (! e.search.contains (t)) { ok = false; break; }
            if (ok) hits.push_back (i);
        }
        if (hits.empty()) continue;

        Row header;
        header.group = gi;
        header.entry = -1;
        header.count = (int) hits.size();
        rows.push_back (header);

        if (! collapsed[gi])
            for (int i : hits)
            {
                Row r;
                r.group = gi;
                r.entry = i;
                rows.push_back (r);
            }

        visibleCount += (int) hits.size();
    }

    list.updateContent();
}

const Locker::Entry* Locker::entryForRow (int row) const
{
    if (row < 0 || row >= (int) rows.size()) return nullptr;
    const int e = rows[(size_t) row].entry;
    if (e < 0 || e >= (int) entries.size()) return nullptr;
    return &entries[(size_t) e];
}

const Locker::Entry* Locker::entryForFile (const juce::File& f) const
{
    if (f == juce::File()) return nullptr;
    for (const auto& e : entries)
        if (e.file == f)
            return &e;
    return nullptr;
}

void Locker::selectFileRow (const juce::File& f)
{
    if (f != juce::File())
        for (int i = 0; i < (int) rows.size(); ++i)
            if (const Entry* e = entryForRow (i))
                if (e->file == f) { list.selectRow (i, true, true); return; }

    list.deselectAllRows();
}

juce::File Locker::selectedFile() const
{
    /* Answer from the register of what the last walk saw, not from a stat:
     * ARRANGE asks this on a button press and a disappeared file should read
     * as "nothing selected", not as a path that no longer resolves. */
    return entryForFile (selectedPath) != nullptr ? selectedPath : juce::File();
}

void Locker::selectedRowsChanged (int lastRowSelected)
{
    /* Only a FILE row moves the selection. A group header is a fold control,
     * and listBoxItemClicked() puts the file selection back immediately --
     * leaving `selectedPath` alone here is what makes that possible. */
    if (const Entry* e = entryForRow (lastRowSelected))
        selectedPath = e->file;

    updateDetail();
    resized();
    repaint();
}

void Locker::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= (int) rows.size()) return;
    if (rows[(size_t) row].entry >= 0) return;          // a file: normal selection

    const int gi = rows[(size_t) row].group;
    collapsed[gi] = ! collapsed[gi];
    rebuildRows();
    selectFileRow (selectedPath);                       // folding is not deselecting
    updateDetail();
    resized();
    repaint();
}

void Locker::showKindMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "ALL", true, kindFilter < 0);
    m.addSeparator();

    for (int i = 0; i < LG_COUNT; ++i)
    {
        int n = 0;
        for (const auto& e : entries)
            if (e.group == i) ++n;

        m.addItem (100 + i,
                   juce::String (lockerGroups[i].tag) + U8 ("  \xc2\xb7  ")
                       + lockerGroups[i].desc + "  " + juce::String (n),
                   n > 0 || kindFilter == i,
                   kindFilter == i);
    }

    juce::Component::SafePointer<Locker> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetComponent (kindBtn.get())
                         .withMinimumWidth (210),
                     [safe] (int result)
    {
        if (safe == nullptr || result == 0) return;
        safe->kindFilter = (result == 1) ? -1 : result - 100;
        safe->kindBtn->setButtonText (safe->kindFilter < 0
                                          ? juce::String ("ALL")
                                          : juce::String (lockerGroups[safe->kindFilter].tag));
        safe->rebuildRows();
        safe->updateDetail();
        safe->resized();
        safe->repaint();
    });
}

/* ---- drag source -------------------------------------------------------- */

juce::var Locker::getDragSourceDescription (const juce::SparseSet<int>& rowsToDescribe)
{
    if (rowsToDescribe.isEmpty()) return {};

    /* The absolute path, which is precisely what LicksPanel::itemDropped()
     * reads out of the description (LicksPanel.cpp:776). A group header
     * returns void, and ListBox then does not start a drag at all. */
    if (const Entry* e = entryForRow (rowsToDescribe[0]))
        return e->file.getFullPathName();

    return {};
}

bool Locker::shouldDropFilesWhenDraggedExternally (
        const juce::DragAndDropTarget::SourceDetails& details,
        juce::StringArray& files, bool& canMoveFiles)
{
    const juce::String p = details.description.toString();
    if (p.isEmpty() || ! juce::File::isAbsolutePath (p)) return false;
    if (! juce::File (p).existsAsFile()) return false;

    files.add (p);

    /* COPY, never move. The ledger's `file` field points at this path; a
     * move would silently break every record that names it, and the LOCKER
     * would go on showing a serial for a file that had walked out of the
     * building. */
    canMoveFiles = false;
    return true;
}

/* ---- provenance --------------------------------------------------------- */

void Locker::updateDetail()
{
    detailLines.clear();
    detailTitle.clear();
    detailIsRecord = false;

    const Entry* e = entryForFile (selectedPath);
    if (e == nullptr) return;

    auto add = [this] (const char* k, const juce::String& v,
                       juce::Colour c = C::INK_DIM)
    {
        if (v.isNotEmpty())
            detailLines.push_back ({ juce::String (k), v, c });
    };

    Record r;
    detailIsRecord = Ledger::shared().findByFile (e->file, r);

    if (! detailIsRecord)
    {
        /* Say so plainly. An unregistered file is the normal case for a
         * locker that predates ACCESSION, and pretending otherwise -- by
         * printing the filename in the serial column, or a row index, which
         * is what this panel used to do -- is how a provenance chain grows
         * an edge that points at nothing. */
        detailTitle = e->name;
        add ("SERIAL",   U8 ("\xe2\x80\x94  NOT IN THE REGISTER"), C::INK_FAINT);
        add ("PATH",     e->subdir.isNotEmpty() ? e->subdir + "/" + e->name : e->name);
        add ("SIZE",     juce::File::descriptionOfSizeInBytes (e->size).toUpperCase());
        add ("MODIFIED", juce::Time (e->modified).formatted ("%Y-%m-%d %H:%M"));
        add ("",         "PLACED HERE BY HAND, OR MADE BEFORE THE REGISTER EXISTED.",
             C::INK_FAINT);
        return;
    }

    detailTitle = r.origin;
    add ("SERIAL",  r.serial, C::INK);
    add ("KIND",    juce::String (kindDescription (r.kind)));
    add ("CREATOR", r.creator);
    add ("DATE",    r.date);
    add ("SOURCE",  r.source.isNotEmpty() ? r.source : r.sourceId);

    /* Licence and claimant on one line and never apart: archive.org licence
     * metadata is uploader-supplied, so "CC0" without a claimant is a rumour
     * with a URL attached (Ledger.h says this at length). */
    if (r.licence.isNotEmpty())
        add ("LICENCE", r.declaredBy.isNotEmpty()
                            ? r.licence + U8 ("  \xc2\xb7  SAID BY ") + r.declaredBy
                            : r.licence + U8 ("  \xc2\xb7  UNATTRIBUTED CLAIM"));
    else if (r.declaredBy.isNotEmpty())
        add ("LICENCE", U8 ("NONE DECLARED  \xc2\xb7  BY ") + r.declaredBy, C::INK_FAINT);

    /* The effective clearance, which folds the whole ancestry in and is
     * nearly always worse than the record's own field. That is the number
     * that decides whether the thing can ship, so it is the one on screen. */
    const Clearance eff = Ledger::shared().effectiveClearance (r.serial);
    add ("CLEARANCE", juce::String (clearanceTag (eff)), clearanceColour (eff));
    if (eff != r.clearance)
        add ("", U8 ("INHERITED \xe2\x80\x94 THIS RECORD ITSELF READS ")
                 + juce::String (clearanceTag (r.clearance)), C::INK_FAINT);
    add ("NOTE", r.note, C::INK_FAINT);
    add ("TOOL", r.tool, C::INK_FAINT);

    const std::vector<Record> chain = Ledger::shared().ancestry (r.serial);
    if (chain.empty())
        add ("FROM", U8 ("\xe2\x80\x94  ORIGINAL ACCESSION"), C::INK_FAINT);
    else
        for (size_t i = 0; i < chain.size(); ++i)
            add (i == 0 ? "FROM" : "",
                 chain[i].origin.isNotEmpty()
                     ? chain[i].serial + U8 ("  \xc2\xb7  ") + chain[i].origin
                     : chain[i].serial,
                 clearanceColour (chain[i].clearance));

    add ("ACCESSIONED", r.utc, C::INK_FAINT);
}

int Locker::detailHeight() const
{
    if (detailLines.empty()) return 0;

    const int chrome = 22 + kLockerFilterH + 20;      // header + filter + footer
    const int room   = getHeight() - chrome - 90;     // the list keeps 90px
    if (room < 60) return 0;

    const int want = 1 + 20 + 15 + (int) detailLines.size() * 12 + 6;
    return juce::jmin (want, room);
}

void Locker::paintDetail (juce::Graphics& g, Rectangle<int> b)
{
    g.setColour (C::PANEL_ALT);
    g.fillRect (b);
    g.setColour (C::HAIRLINE);
    g.fillRect (b.getX(), b.getY(), b.getWidth(), 1);
    b.removeFromTop (1);

    // registration crosses, the one corner mark this design owns
    g.setColour (C::INK_GHOST);
    paintRegistrationCross (g, b.getX() + 5, b.getY() + 5);
    paintRegistrationCross (g, b.getRight() - 6, b.getY() + 5);

    paintLabelRow (g, b.removeFromTop (20), "PROVENANCE",
                   detailIsRecord ? juce::String ("REGISTERED")
                                  : juce::String ("UNREGISTERED"));

    Rectangle<int> body = b.reduced (10, 0);

    // the human name, 10px, ellipsised
    g.setColour (C::INK);
    g.setFont (Type::mono (10.0f));
    g.drawText (detailTitle, body.removeFromTop (15), Justification::centredLeft, true);

    const juce::Font kf = Type::mono (7.0f, 0.12f);
    const juce::Font vf = Type::mono (8.0f, 0.02f);

    for (const auto& line : detailLines)
    {
        if (body.getHeight() < 12) break;           // draw only what fits
        Rectangle<int> lr = body.removeFromTop (12);

        Rectangle<int> kr = lr.removeFromLeft (62);
        if (line.key.isNotEmpty())
        {
            g.setColour (C::INK_FAINT);
            g.setFont (kf);
            g.drawText (line.key, kr, Justification::centredLeft, true);
        }
        lr.removeFromLeft (4);

        g.setColour (line.colour);
        g.setFont (vf);
        g.drawText (line.value, lr, Justification::centredLeft, true);
    }
}

/* ---- layout and paint --------------------------------------------------- */

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

    Rectangle<int> fr = b.removeFromTop (kLockerFilterH).reduced (8, 4);
    kindBtn->setBounds (fr.removeFromRight (46));
    fr.removeFromRight (6);
    filterBox.setBounds (fr);

    Rectangle<int> foot = b.removeFromBottom (20);
    growBtn->setBounds (foot.getRight() - 55, foot.getY() + 2, 50, 16);

    b.removeFromBottom (detailHeight());
    b.removeFromRight (1);                      // right-edge divider stays visible
    list.setBounds (b);
}

void Locker::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    juce::String right = contextHint.isNotEmpty() ? contextHint
                                                  : morgue::morgueDirDisplay();
    paintHeaderBand (g, b.removeFromTop (22), "LOCKER", {}, right);

    // filter band: PANEL_ALT with a bottom hairline; the controls sit in it
    Rectangle<int> fr = b.removeFromTop (kLockerFilterH);
    g.setColour (C::PANEL_ALT);
    g.fillRect (fr);
    g.setColour (C::HAIRLINE);
    g.fillRect (fr.getX(), fr.getBottom() - 1, fr.getWidth(), 1);

    // footer 20: count left, GROW right, 8px .12em INK_FAINT, padding 0 8
    Rectangle<int> foot = b.removeFromBottom (20);
    g.setColour (C::HAIRLINE);
    g.fillRect (foot.getX(), foot.getY(), foot.getWidth(), 1);

    /* "N SPECIMENS" verbatim when nothing is filtered -- the footer this
     * panel has always had. A filter turns it into shown/total, because a
     * count that silently means something else is worse than no count. */
    juce::String count;
    if (! scanned)          count = "SCANNING";
    else if (filterActive()) count = juce::String (visibleCount) + "/"
                                   + juce::String ((int) entries.size()) + " SPECIMENS";
    else                     count = juce::String ((int) entries.size()) + " SPECIMENS";

    const juce::Font ff = Type::mono (8.0f, 0.12f);
    g.setColour (C::INK_FAINT);
    g.setFont (ff);
    Rectangle<int> fbar = foot.reduced (8, 0);
    g.drawText (count, fbar.removeFromLeft (textW (ff, count) + 2),
                Justification::centredLeft);

    /* Two things the player is otherwise never told. Undelivered ledger
     * writes mean the session's provenance is not durable yet; a truncated
     * walk means the list on screen is not the whole locker. Both are amber
     * warnings, not accents -- nothing here is armed or dangerous. */
    juce::String warn;
    if (truncated)         warn = "WALK TRUNCATED";
    else if (pendingWrites > 0) warn = juce::String (pendingWrites) + " UNWRITTEN";
    if (warn.isNotEmpty())
    {
        Rectangle<int> wr = fbar.withTrimmedRight (55);
        g.setColour (C::AMBER);
        g.drawText (warn, wr, Justification::centredRight, true);
    }

    // the provenance block, above the footer
    Rectangle<int> det = b.removeFromBottom (detailHeight());
    if (! det.isEmpty())
        paintDetail (g, det);

    if (rows.empty())
    {
        g.setColour (C::INK_FAINT);
        g.setFont (Type::mono (9.0f, 0.10f));
        juce::String msg;
        if (! scanned)             msg = "READING " + morgue::morgueDirDisplay();
        else if (filterActive())   msg = "NOTHING MATCHES THAT";
        else                       msg = "NO SPECIMENS IN " + morgue::morgueDirDisplay();
        g.drawText (msg, b.reduced (8, 0), Justification::centred, true);
    }

    // right-edge divider against the main stage (spec section 3)
    g.setColour (C::HAIRLINE);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
}

int Locker::getNumRows() { return (int) rows.size(); }

void Locker::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) rows.size()) return;
    const Row& r = rows[(size_t) row];

    /* ---- a group header: tag, description, count, fold state ---- */
    if (r.entry < 0)
    {
        g.setColour (C::PANEL_ALT);
        g.fillRect (0, 0, w, h);
        g.setColour (C::HAIRLINE);
        g.fillRect (0, 0, w, 1);

        Rectangle<int> band (0, 0, w, h);
        band.removeFromLeft (8);
        band.removeFromRight (8);

        // the fold marker: a 4px triangle, the same one the combo boxes use
        {
            const float cx = (float) band.getX() + 2.0f;
            const float cy = (float) band.getCentreY();
            juce::Path p;
            if (collapsed[r.group]) p.addTriangle (cx - 2, cy - 4, cx + 3, cy, cx - 2, cy + 4);
            else                    p.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
            g.setColour (C::INK_GHOST);
            g.fillPath (p);
        }
        band.removeFromLeft (13);

        const juce::Font cf = Type::mono (8.0f, 0.10f);
        const juce::String cnt (r.count);
        g.setColour (C::INK_FAINT);
        g.setFont (cf);
        g.drawText (cnt, band.removeFromRight (textW (cf, cnt) + 2),
                    Justification::centredRight);
        band.removeFromRight (8);

        const juce::Font tf = Type::label();          // 9px .16em, caps
        const juce::String tag (lockerGroups[r.group].tag);
        g.setColour (C::INK_DIM);
        g.setFont (tf);
        g.drawText (tag, band.removeFromLeft (textW (tf, tag) + 2),
                    Justification::centredLeft);
        band.removeFromLeft (8);

        g.setColour (C::INK_GHOST);
        g.setFont (Type::mono (8.0f, 0.10f));
        g.drawText (lockerGroups[r.group].desc, band, Justification::centredLeft, true);
        return;
    }

    /* ---- a file: two lines, name over serial ---- */
    const Entry& e = entries[(size_t) r.entry];

    if (selected)                                // bg #191816; no row separators
    {
        g.setColour (C::TAB_ACTIVE_BG);
        g.fillRect (0, 0, w, h);
    }

    /* A 1px rule at the left edge for material that cannot ship. This is the
     * only place BLOOD appears in the list, and it means exactly one thing:
     * do not release this. REVIEW gets amber, the warn colour. Everything
     * else gets nothing -- a mark that is always on is not a mark. */
    if (e.clearance == (int) Clearance::PersonalOnly)
    {
        g.setColour (C::BLOOD);
        g.fillRect (0, 0, 1, h);
    }
    else if (e.clearance == (int) Clearance::Review)
    {
        g.setColour (C::AMBER);
        g.fillRect (0, 0, 1, h);
    }

    Rectangle<int> body (0, 0, w, h);
    body.removeFromLeft (8);
    body.removeFromRight (8);

    Rectangle<int> l1 = body.removeFromTop (15);
    Rectangle<int> l2 = body.removeFromTop (12);

    // line 1 right: PATCH for project files, else the size
    const juce::String meta = e.patch
        ? juce::String ("PATCH")
        : juce::File::descriptionOfSizeInBytes (e.size).replace (" ", "").toUpperCase();
    const juce::Font metaFont = Type::mono (8.0f);
    g.setColour (C::INK_FAINT);
    g.setFont (metaFont);
    g.drawText (meta, l1.removeFromRight (textW (metaFont, meta) + 2),
                Justification::centredRight);
    l1.removeFromRight (6);

    /* line 1 left: what a HUMAN calls it. The register's origin when there is
     * one -- an acquisition's real title beats "ACQ-260805-K7J4QWMR.wav" and
     * beats the archive.org filename it arrived under. */
    const juce::String title = e.origin.isNotEmpty() ? e.origin : e.name;
    g.setColour (selected ? C::INK_BRIGHT : (e.patch ? C::OXIDE : INK_MID));
    g.setFont (Type::mono (10.0f));
    g.drawText (title, l1, Justification::centredLeft, true);

    /* line 2 right: where it actually lives, when that is not the root --
     * the structure the old flat listing threw away -- or the name on disk
     * when the line above is showing the register's title instead. */
    juce::String where = e.subdir;
    if (where.isEmpty() && e.origin.isNotEmpty()) where = e.name;
    if (where.isNotEmpty())
    {
        const juce::Font wf = Type::nano (7.0f);
        g.setColour (C::INK_GHOST);
        g.setFont (wf);
        const int ww = juce::jmin (textW (wf, where) + 2,
                                   juce::jmax (0, l2.getWidth() / 2));
        g.drawText (where, l2.removeFromRight (ww), Justification::centredRight, true);
        l2.removeFromRight (6);
    }

    /* line 2 left: the TRUE serial. This column used to be String(row + 1) --
     * a list index in a list sorted newest-first, so it renumbered every time
     * a file landed. With no record it is an em dash, never a fabricated
     * number and never a bare hash. */
    const juce::Font serF = Type::mono (8.0f, 0.06f);
    g.setColour (e.serial.isEmpty() ? C::INK_GHOST
                                    : (selected ? C::BLOOD_HOT : C::INK_FAINT));
    g.setFont (serF);
    g.drawText (e.serial.isNotEmpty() ? e.serial : U8 ("\xe2\x80\x94"),
                l2, Justification::centredLeft, true);
}

juce::String Locker::getTooltipForRow (int row)
{
    if (row < 0 || row >= (int) rows.size()) return {};
    const Row& r = rows[(size_t) row];

    if (r.entry < 0)
        return juce::String (lockerGroups[r.group].desc) + U8 (" \xe2\x80\x94 ")
             + juce::String (r.count) + U8 (" in this locker. Click to fold.");

    const Entry& e = entries[(size_t) r.entry];
    juce::String t = e.name;
    if (e.origin.isNotEmpty()) t << U8 (" \xe2\x80\x94 ") << e.origin;
    if (e.subdir.isNotEmpty()) t << U8 (" \xc2\xb7 ") << e.subdir;
    t << U8 (" \xc2\xb7 ") << (e.serial.isNotEmpty() ? e.serial
                                                     : juce::String ("not in the register"));
    if (e.clearance == (int) Clearance::PersonalOnly)
        t << U8 (" \xc2\xb7 PERSONAL ONLY: never in a release.");
    else if (e.clearance == (int) Clearance::Review)
        t << U8 (" \xc2\xb7 REVIEW: a human has to check this one.");
    t << U8 (". Click to read its provenance; drag it onto a GRAIN LICKS slot.");
    return t;
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
            /* One coherent read: bb.bar and bb.seq_pos are published as two
             * separate stores, so loading them independently shows a bar and a
             * step from either side of a boundary. */
            const float posF = transportPositionBars();
            const unsigned bar = posF < 0.0f ? (unsigned) atomic_load (&bb.bar)
                                             : (unsigned) posF;
            const int sp = posF < 0.0f ? -1
                         : (int) ((posF - (float) bar) * (float) BB_STEPS + 0.5f);
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
                "SPECIMENS ARE ACQUIRED UNVERIFIED \xc2\xb7 CLEAR BEFORE RELEASE", // EXHUME
                "EVERY PASS IS SEEDED \xc2\xb7 THE LADDER IS REPRODUCIBLE",   // PLATE
                ""                                                // EXPORT (sheet covers)
            };
            /* This array is sized [numTabs] and indexed by the selected tab, so
             * it MUST have exactly numTabs initialisers. A short list leaves the
             * tail null and the note[0] test below dereferences it -- selecting
             * the new tab would crash rather than draw nothing. */
            static_assert (sizeof contextNote / sizeof contextNote[0]
                               == (size_t) StageTabs::numTabs,
                           "contextNote must have one entry per tab");
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
