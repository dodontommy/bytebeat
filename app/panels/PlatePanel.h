/* PlatePanel.h -- PLATE, the visual wing (serial N.72-0427).
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS FOR
 *
 * The reference processes are the player's own, verbatim:
 *
 *     distorted images created by repeated photo copying at high exposures
 *     scanning a photo held up high above the scanner
 *     dragging your phone flash along with the scanner light
 *
 * Not one of those is a filter. Every one is a PHYSICAL PROCESS with a
 * feedback loop in it -- the machine reads its own previous output, and the
 * loss compounds because generation 40 was made from generation 39 and not
 * from the original. This is the visual counterpart of the analog audio loops
 * that already run through this instrument: signal goes out into the world,
 * something abuses it, it comes back in worse and more interesting.
 *
 * Two consequences shape the whole design and are worth stating before the
 * code:
 *
 *   MORGUE ORCHESTRATES, IT DOES NOT REIMPLEMENT. There is no raster code
 *   here. ffmpeg has scale, eq, curves, noise, gblur, unsharp, rotate, pad,
 *   crop, convolution, erosion, hue and geq, plus mjpeg quantiser control,
 *   and between them they cover every one of those three processes. What
 *   MORGUE owns is the recipe, the seed, the pass count, the LINEAGE and the
 *   UI. And it does not own the ffmpeg command construction twice: this panel
 *   shells out to tools/degrade.py, the same script the player can run in a
 *   terminal tonight, exactly as tools/exhume.py is the acquisition wing. One
 *   implementation, one place to fix a filter string, testable without a GUI.
 *
 *   THE PHYSICAL ROUND TRIP IS THE POINT. The strongest version of this is
 *   not "degrade an image". It is: MORGUE degrades it, he PRINTS it, he
 *   photocopies it, he scans it, it comes back in, MORGUE degrades it
 *   further. So ingest and lineage are not supporting features -- they are
 *   half the instrument. A 40th-generation plate has to walk back to the scan
 *   it came from, and the photocopy of a MORGUE render has to be attachable
 *   to the render it is a photocopy of, or the archive cannot describe the
 *   only thing that actually happened.
 *
 * ---------------------------------------------------------------------------
 * INTAKE IS A WATCHED FOLDER, AND THAT IS THE RIGHT ANSWER
 *
 * <session root>/INTAKE. MORGUE does not talk to the scanner.
 *
 * This is not a fallback for lack of a scanner SDK. WIA on Windows,
 * ImageCaptureCore on macOS and SANE on Linux are three disjoint, driver-flaky
 * APIs with three different threading models, and shipping all three would buy
 * exactly nothing here -- because every abuse in the brief REQUIRES a human
 * standing at the machine. Lid off. Object moving mid-pass. A phone in the
 * other hand as a second light source. He is already at the glass with his
 * finger on his scanner software's own button; a "SCAN" plate in this panel
 * would be a worse version of the button he is already pressing, and it could
 * not hold the lid open for him. A watched folder is what a darkroom drop box
 * is: the correct interface to a physical process a machine cannot perform.
 *
 * The polling is deliberate too. Filesystem-notification APIs
 * (ReadDirectoryChangesW, FSEvents, inotify) fire when a file is CREATED, not
 * when it is finished, and a 200 MB TIFF being written by scanner software
 * arrives as a create event followed by ninety seconds of writes. So: poll on
 * a juce::TimeSliceThread, and ingest a file only once its size AND its
 * modification time have been identical across two consecutive polls. Half a
 * scan is not a specimen.
 *
 * ---------------------------------------------------------------------------
 * LINKING THE ROUND TRIP -- WHY "LINK NEXT" AND NOT "SET PARENT"
 *
 * When a photocopy of a MORGUE plate comes back through the scanner, it needs
 * derived_from pointing at the plate it is a copy of. The obvious design is to
 * ingest it and then edit its parent afterwards. That design is wrong twice:
 * the ledger is APPEND-ONLY (Ledger.h -- every diff is a pure addition, which
 * is the property the cross-machine merge depends on), so there is no edit to
 * make; and it asks the player to remember, later, which of eleven scans was
 * the copy of which plate.
 *
 * So the link is armed BEFORE the trip, not repaired after it. Select the
 * generation, press LINK NEXT, walk to the copier, and the next thing that
 * lands in INTAKE is accessioned with that serial as its parent. That is the
 * order the physical work actually happens in. (Anything that needs fixing
 * after the fact is fixed by editing ACCESSION.ledger in a text editor, which
 * the format is explicitly designed to allow.)
 *
 * ---------------------------------------------------------------------------
 * THREADING
 *
 * Nothing here is within a mile of the audio thread; bb_engine_render() does
 * not know this panel exists. But everything this panel does is slow --
 * ffmpeg, SHA-256, fsync, JPEG decode, directory walks -- and the message
 * thread runs the 30 Hz engine sync and every paint routine.
 *
 *   the ladder      juce::Thread::launch + a juce::Component::SafePointer +
 *                   MessageManager::callAsync, in the shape Chrome.cpp's
 *                   growSpecimen() established. Progress arrives as one
 *                   callAsync per pass, parsed off degrade.py's stdout.
 *   INTAKE          its own juce::TimeSliceThread. Polls, hashes, accessions
 *                   and renders proof thumbnails there; only the finished
 *                   record crosses to the message thread.
 *   thumbnails      decoded on the worker into juce::Image and handed over
 *                   whole. Decoding sixty-four JPEGs inside paint() would
 *                   stall the console for a second every repaint.
 *
 * The run worker never touches `this`. It captures a shared_ptr to a small
 * RunState that outlives the panel if it has to, so a player who closes the
 * console mid-ladder gets a cancelled child process and not a crash.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "Theme.h"
#include "Primitives.h"

namespace morgue
{

/* One operator in the chain. Amounts are 0..255 because that is the range
 * every knob in MORGUE turns through, and because the recipe file this panel
 * writes has to be the same file degrade.py reads. */
struct PlateOp
{
    juce::String name;      // "resample", "crush", ... (degrade.py's vocabulary)
    int          amount = 0;
    bool         on = false;
    juce::String note;      // the one-line description printed in the row
};

class PlatePanel : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   public juce::SettableTooltipClient
{
public:
    PlatePanel();
    ~PlatePanel() override;

    /* Main.cpp wires these, exactly as it does for ArrangePanel. */
    std::function<juce::File()> getLockerSelection;   // the LOCKER's selected row
    std::function<void()>       onLockerRefresh;      // re-scan after a run

    /* Cheap enough for the 30 Hz pull: it reads two atomics and repaints only
     * when something changed. Nothing here is polled off the engine. */
    void sync();

    void setSource (const juce::File&);

    void resized() override;
    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    /* The formats ffmpeg will decode and the console will show. Used by the
     * INTAKE watcher and by the drop target, so they agree by construction. */
    static bool isImageFile (const juce::File&);

private:
    /* ---- the watched folder ----
     * A nested class, so it reaches the panel's private state (the armed link)
     * without any of it having to be public. */
    class Watcher;

    /* ---- one rung of the ladder ---- */
    struct Gen
    {
        int          index = 0;
        juce::File   full;              // gen-NNN.jpg, the real plate
        juce::Image  thumb;             // decoded off the message thread
        juce::String serial;            // set once this rung is accessioned
    };

    /* ---- one row of the INTAKE list ---- */
    struct Scan
    {
        juce::String serial;
        juce::String name;
        juce::File   file;
        juce::Image  proof;
        juce::String parent;            // derived_from, when the link was armed
    };

    /* Everything a run needs, copied by value into the worker. The worker
     * must never dereference the panel. */
    struct RunSpec
    {
        juce::File   script, source, outDir;
        juce::String plateSerial, sourceSerial, seedHex;
        int          passes = 12;
        int          jitter = 24;
        std::vector<PlateOp> ops;
    };

    /* Shared with the worker so that CANCEL can reach the child process
     * without the worker having to reach back into the panel. */
    struct RunState
    {
        juce::CriticalSection lock;
        std::unique_ptr<juce::ChildProcess> proc;
        std::atomic<bool> cancelled { false };
    };

    /* ---- geometry ---- */
    juce::Rectangle<int> railArea() const;
    juce::Rectangle<int> sheetArea() const;
    juce::Rectangle<int> detailArea() const;
    juce::Rectangle<int> sourceWellArea() const;
    juce::Rectangle<int> chainRowArea (int i) const;
    juce::Rectangle<int> chainAmountArea (int i) const;
    juce::Rectangle<int> progressArea() const;
    juce::Rectangle<int> intakeArea() const;
    juce::Rectangle<int> intakeRowArea (int i) const;
    juce::Rectangle<int> cellArea (int i) const;
    void   sheetGrid (int& cols, int& rows, int& cw, int& ch) const;

    /* ---- painting ---- */
    void paintRail (juce::Graphics&);
    void paintChain (juce::Graphics&);
    void paintSheet (juce::Graphics&);
    void paintDetail (juce::Graphics&);
    void paintIntake (juce::Graphics&);
    static void paintRegistration (juce::Graphics&, juce::Rectangle<int>);

    /* ---- actions ---- */
    void pickSource();
    void takeLockerSelection();
    void startRun();
    void cancelRun();
    void keepSelected();                // accession one rung as a PLT record
    void revealSelected();
    void rollSeed();
    void applySeedField();
    void setStatus (const juce::String&, bool alert = false);

    /* The armed link, read under the lock. The watcher thread clears it the
     * moment it consumes it, and paintDetail() prints it -- a juce::String is
     * copy-on-write and assignment is not atomic, so this is a lock and not a
     * bare read. */
    juce::String armedLink() const;

    /* ---- helpers (may block: worker threads only where noted) ---- */
    static juce::File findScript();            // tools/degrade.py
    static juce::File intakeDir();
    static juce::File platesDir();
    static juce::File proofsDir();
    static juce::String recipeText (const std::vector<PlateOp>&,
                                    juce::uint32 seed, int passes, int jitter);

    /* called from the watcher thread's completion, on the message thread */
    void scanArrived (const Scan&);

    /* ---- state ---- */
    juce::File   source;
    juce::Image  sourceThumb;
    juce::String sourceMeta;                   // "2480 x 3508 - 4.1 MB"
    juce::String sourceSerial;

    std::vector<PlateOp> ops;
    juce::uint32 seed = 0;
    int passes = 12;
    int jitter = 24;

    std::vector<Gen> gens;
    std::vector<Scan> scans;
    juce::File   runDir;
    juce::String plateSerial, recipeSerial, recipeDigest;
    int selected = -1;
    int intakeSel = -1;

    std::shared_ptr<RunState> run;
    std::atomic<bool> running { false };
    std::atomic<int>  passDone { 0 };
    std::atomic<int>  passTotal { 0 };
    int lastPaintedPass = -1;
    bool lastPaintedRunning = false;

    juce::String status;
    bool statusIsAlert = false;

    /* The armed link: the next INTAKE arrival is accessioned as derived from
     * this serial. Read from the watcher thread, written from the message
     * thread, so it is guarded rather than merely atomic (juce::String is
     * copy-on-write and assignment is not atomic). */
    mutable juce::CriticalSection linkLock;
    juce::String linkSerial;

    /* ---- drag state for the in-row amount sliders ---- */
    int dragRow = -1, dragStartAmount = 0, dragStartX = 0;
    int hoverRow = -1, hoverCell = -1;

    /* ---- children ---- */
    PlateButton  runBtn    { "RUN",       true,  false };
    PlateButton  stopBtn   { "STOP",      false, false };
    PlateButton  pickBtn   { "PICK...",   false, false };
    PlateButton  lockerBtn { "< LOCKER",  false, false };
    PlateButton  rollBtn   { "ROLL",      false, false };
    PlateButton  keepBtn   { "KEEP",      false, false };
    PlateButton  revealBtn { "REVEAL",    false, false };
    PlateButton  linkBtn   { "LINK NEXT", true,  true  };
    EngravedKnob passesKnob { "PASSES", 32, 1, 64, 12 };
    EngravedKnob jitterKnob { "JITTER", 32, 0, 255, 24 };
    juce::TextEditor seedField;
    std::unique_ptr<juce::FileChooser> chooser;

    /* Declared BEFORE the thread so that the thread outlives the client it
     * calls -- members are destroyed in reverse declaration order, and a
     * TimeSliceThread still running against a destroyed client is a use-after
     * -free that only shows up on a slow machine. The destructor stops the
     * thread explicitly as well; this is the belt to that pair of braces. */
    std::unique_ptr<Watcher> watcher;
    juce::TimeSliceThread    intakeThread { "MORGUE INTAKE" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlatePanel)
};

} // namespace morgue
