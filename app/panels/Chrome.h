/* Chrome.h -- the window furniture (spec sections 3, 4, 13):
 *
 *  TitleBar      26px  three 9px circles, masthead, session path, serial
 *  StageTabs     30px  8 custom tabs + INFO cell (NOT a TabbedComponent)
 *  Locker        236px file archive over the session dir (ListBoxModel)
 *  Scope         198px master bus, 30 Hz, from the engine ring
 *  TransportBar  60px  RUN/CUT/REC/? + BPM/BEATS/BARS/GAIN + per-tab context
 *  StatusBar     20px  BAR/STEP/CPU/CLIP(800ms hold)/rate/buffer
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <memory>
#include <vector>
#include "Theme.h"
#include "Primitives.h"
#include "Ledger.h"        // the LOCKER paints real serials, not row indices

class AudioEngine;

namespace morgue
{

/* ---- title bar (26) -----------------------------------------------------
 * The three circles are the window's only controls -- there is no native
 * title bar behind them. They are laid out in the macOS traffic-light order
 * (close / minimise / zoom) and they do all three jobs on every platform;
 * previously only the first was wired, which left a Windows player with a
 * window they could neither minimise nor maximise. Double-clicking the bar
 * zooms, as it does in both window managers. */
class TitleBar : public juce::Component,
                 public juce::SettableTooltipClient
{
public:
    enum Control { Close = 0, Minimise = 1, Zoom = 2, NumControls = 3 };

    TitleBar();
    void setSerial (const juce::String&);      // per-panel serial (spec section 3)
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    int  controlAt (juce::Point<int>) const;   // -1 = none
    void performControl (int);

    juce::ComponentDragger dragger;
    juce::String serial { SerialNo::RACK };
    int hoverControl = -1;
    bool draggingWindow = false;
};

/* ---- stage tab strip (30) ----------------------------------------------- */
class StageTabs : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    StageTabs();

    /* EXPORT stays LAST on purpose. Main.cpp treats the final index specially
     * -- it is a sheet drawn over the previous stage rather than a workspace of
     * its own -- so new workspaces are inserted BEFORE it and this constant is
     * the only number that moves. */
    static constexpr int numTabs = 10;
    static const char* tabName (int i);

    void setCurrent (int idx);                 // no callback
    int  current() const noexcept { return cur; }
    std::function<void (int)> onSelect;
    std::function<void()> onInfo;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    int tabWidth (int i) const;
    int tabAt (int x) const;                   // -1 none, numTabs = INFO cell
    int cur = 0, hover = -1;
};

/* ---- LOCKER (spec section 4) --------------------------------------------
 *
 * The archive, and the one panel where the provenance discipline is visible.
 * Four things changed here and every one of them was a defect, not a wish.
 *
 *  RECURSIVE.  refresh() used to be a flat, NON-recursive findChildFiles over
 *  the session root, and that listing WAS the library. Acquired material
 *  lands in ACQ/<identifier>/ and was therefore completely invisible to the
 *  instrument that is supposed to play it -- the acquisition pipeline had
 *  nowhere to deliver to. The walk is now breadth-first with a depth cap and
 *  it happens on the LOCKER's own worker thread; the message thread never
 *  touches the filesystem. Locker::Scanner in Chrome.cpp carries the
 *  invalidation rules, written out, because "when do we re-walk" is the only
 *  interesting question a cache asks.
 *
 *  GROUPED.  A thousand files sorted by mtime is not an archive, it is a
 *  pile. Rows are grouped by kind (SPC / CLIP / REC / ACQ / SCN / PLT / ...)
 *  under collapsible headers, newest-first inside each group.
 *
 *  REAL SERIALS.  The serial column used to paint juce::String(row + 1) -- a
 *  ListBox row index, in a column that reads exactly like an accession
 *  number, in a list sorted newest-first, so the number beside a specimen
 *  changed every time the player grew another one. It now asks
 *  morgue::Ledger for the record and prints the true serial with the human
 *  origin above it. With no record it prints an em dash: a missing serial is
 *  information, an invented one is a lie, and a bare hash is neither.
 *
 *  A DRAG SOURCE.  Captured and acquired material has to reach the panels
 *  that can chew on it. The LOCKER is a juce::DragAndDropContainer and its
 *  rows emit their absolute path, which is exactly what LicksPanel's
 *  itemDropped() already reads. The drag image is taken to the desktop
 *  (ListBoxModel::mayDragToExternalWindows() defaults true), which is not
 *  cosmetic: JUCE hit-tests a desktop drag image against the whole display,
 *  so a target OUTSIDE this component -- every stage panel is a sibling, not
 *  a child -- can be found at all. A drag that leaves the window entirely
 *  becomes a real file drag into Explorer/Finder/another DAW, copy only,
 *  never move, because the register's `file` field points at this copy.
 */
class Locker : public juce::Component,
               public juce::SettableTooltipClient,
               public juce::DragAndDropContainer,
               private juce::ListBoxModel
{
public:
    Locker();
    ~Locker() override;

    void refresh();                             // invalidate; the walk is async
    void setContextHint (const juce::String&);  // per-tab: "DRAG -> LANE" etc.
    juce::File selectedFile() const;            // selected row; File() when none

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    /* One file the scan found, already joined to its ledger record. Built on
     * the worker thread and handed over whole, so nothing here is ever
     * computed inside a paint routine. */
    struct Entry
    {
        juce::File   file;
        juce::String name;          // the name on disk
        juce::String subdir;        // dir part relative to the root; "" = root
        juce::String serial;        // ledger serial, empty when unregistered
        juce::String origin;        // ledger origin (what a human calls it)
        juce::String search;        // lowercased haystack for the text filter
        juce::int64  size = 0;
        juce::int64  modified = 0;
        int          group = 0;     // index into the group table in Chrome.cpp
        int          clearance = -1;// (int) morgue::Clearance; -1 = unregistered
        bool         patch = false; // session.conf / *.morgue / the ledger itself
    };

    /* A visible list row: a group header (entry < 0) or one Entry. */
    struct Row { int group = 0; int entry = -1; int count = 0; };

    /* One key/value line of the provenance block, pre-rendered on selection. */
    struct DetailLine { juce::String key, value; juce::Colour colour; };

    class Scanner;                  // the worker thread; see Chrome.cpp

    // ---- ListBoxModel ----
    int  getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    juce::String getTooltipForRow (int row) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;

    // ---- DragAndDropContainer ----
    bool shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails&,
                                               juce::StringArray& files,
                                               bool& canMoveFiles) override;

    void growSpecimen();                        // background render into the session dir
    void adoptScan (std::vector<Entry>, int pendingWrites, bool truncatedWalk);
    void rebuildRows();                         // filter + collapse -> visible rows
    void updateDetail();                        // selection -> the provenance block
    void selectFileRow (const juce::File&);     // re-point the list at a path
    void showKindMenu();
    bool filterActive() const;
    int  detailHeight() const;
    const Entry* entryForRow (int row) const;
    const Entry* entryForFile (const juce::File&) const;
    void paintDetail (juce::Graphics&, juce::Rectangle<int>);

    juce::ListBox list { "locker", this };
    std::vector<Entry> entries;                 // everything the last walk found
    std::vector<Row>   rows;                    // what the ListBox shows
    std::vector<DetailLine> detailLines;

    juce::TextEditor filterBox;
    std::unique_ptr<PlateButton> kindBtn;       // kind filter; opens a menu
    std::unique_ptr<PlateButton> growBtn;

    juce::String contextHint;
    juce::String filterText;                    // lowercased; split on spaces (AND)
    juce::String detailTitle;
    juce::File   selectedPath;                  // the selected FILE, survives rebuilds
    int  kindFilter = -1;                       // -1 = ALL, else a group index
    int  visibleCount = 0;                      // entries passing the filter
    int  pendingWrites = 0;                     // Ledger::pendingCount(), per scan
    bool scanned = false;                       // one walk has completed
    bool truncated = false;                     // the walk hit its file cap
    bool detailIsRecord = false;                // the selection is in the register
    bool collapsed[16] = { false };             // by group index (11 groups today)

    std::unique_ptr<Scanner> scanner;
    std::atomic<bool> growing { false };
};

/* ---- SCOPE (spec section 4): the engine's live master bus --------------- */
class Scope : public juce::Component, private juce::Timer
{
public:
    Scope();
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }
};

/* ---- TRANSPORT (60, spec section 13) ------------------------------------ */
class TransportBar : public juce::Component
{
public:
    TransportBar();
    void resized() override;
    void paint (juce::Graphics&) override;
    void sync();                                // 30 Hz engine pull
    void setContextTab (int tabIndex);          // right-side context zone

    PlateButton run, cut, rec, info;
    juce::OwnedArray<EngravedKnob> knobs;       // BPM BEATS BARS GAIN
    std::function<void()> onExport;             // MIXER context EXPORT... plate

private:
    void applyContext();                        // show/hide per-tab children
    int contextTab = 0;
    float masterPeak = 0.0f;                    // post-gain sink peak (sync)
    MeterComponent masterMeter;                 // RACK context: master meter
    std::unique_ptr<juce::Button> exportBtn;    // MIXER context (local class)
};

/* ---- STATUS (20, spec section 13) --------------------------------------- */
class StatusBar : public juce::Component, private juce::Timer
{
public:
    explicit StatusBar (AudioEngine&);
    void paint (juce::Graphics&) override;

    /* The right-hand hint doubles as the console's one notice channel. Empty
     * restores "PRESS ? FOR A MAP OF THIS CONSOLE"; anything else replaces it
     * in BLOOD_HOT. A failed device open and a failed session write are the
     * two things the player must be told about and had no way of learning. */
    void setAlert (const juce::String&);

private:
    void timerCallback() override;
    AudioEngine& audio;
    juce::uint32 lastClipMs = 0;                // CLIP holds 800 ms
    juce::String alert;
};

} // namespace morgue
