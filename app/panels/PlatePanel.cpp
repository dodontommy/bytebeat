/* PlatePanel.cpp -- see PlatePanel.h for why any of this exists.
 *
 * The panel is three columns of one idea: a SOURCE, a CHAIN that is applied to
 * its own output N times, and a CONTACT SHEET of every generation so the rung
 * can be CHOSEN rather than a number guessed. Underneath it, a watched folder
 * that turns the physical round trip into ledger records.
 *
 * Everything is custom-drawn (spec rule 0: there is no LookAndFeel work here
 * and none is introduced). The chain rows, the sheet cells and the intake list
 * are painted by this component and hit-tested by hand rather than being made
 * of child components, because sixty-four child components that each own a
 * juce::Image is a lot of machinery for a grid of rectangles.
 */

#include "PlatePanel.h"

#include "Session.h"
#include "Ledger.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    constexpr int kRail      = 300;   // left column
    constexpr int kFooterH   = 26;
    constexpr int kChainRowH = 24;
    constexpr int kIntakeRowH = 34;
    constexpr int kDetailH   = 96;
    constexpr int kSheetHeadH = 20;

    constexpr const char* kSerial = "N.72-0427";

    int textW (const juce::Font& f, const juce::String& s)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
    }

    juce::String hex8 (juce::uint32 v)
    {
        return juce::String::toHexString ((int) v).paddedLeft ('0', 8).toUpperCase();
    }

    /* A small filled triangle, the same shape the shared ComboBox drawing
     * already uses. Drawn rather than typed: U+25B2 is not in IBM Plex Mono
     * and a missing glyph in a reorder control is a control the player cannot
     * find. */
    void triangle (juce::Graphics& g, Rectangle<int> r, bool up)
    {
        juce::Path p;
        const float cx = (float) r.getCentreX();
        const float t = (float) r.getY() + 2.0f, b = (float) r.getBottom() - 2.0f;
        if (up) p.addTriangle (cx, t, cx - 4.0f, b, cx + 4.0f, b);
        else    p.addTriangle (cx, b, cx - 4.0f, t, cx + 4.0f, t);
        g.fillPath (p);
    }

    juce::String humanSize (juce::int64 bytes)
    {
        if (bytes >= 1024 * 1024)
            return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
        return juce::String ((bytes + 1023) / 1024) + " KB";
    }

    juce::String ffmpegExe()
    {
        const juce::String env = juce::SystemStats::getEnvironmentVariable ("MORGUE_FFMPEG", {});
        return env.isNotEmpty() ? env : juce::String ("ffmpeg");
    }

    /* The operator vocabulary, mirroring tools/degrade.py's OPS table and
     * DEFAULT_ORDER. It is duplicated here only as LABELS and DEFAULTS -- not
     * one ffmpeg string lives in this file, which is the whole point of the
     * script being the engine. If the script gains an operator, adding a line
     * here surfaces it; a recipe naming an operator this build does not know
     * still runs, because the script owns the chain. */
    struct OpDef { const char* name; int amount; bool on; const char* note; };

    const OpDef kOpDefs[] =
    {
        { "skew",     40,  false, "OBJECT MOVED MID-PASS"      },
        { "drift",    70,  true,  "REGISTRATION DRIFT"         },
        { "resample", 96,  true,  "GENERATION SOFTENING"       },
        { "blur",     60,  true,  "SCANNER MTF"                },
        { "flash",    0,   false, "LIGHT DRAGGED ACROSS GLASS" },
        { "exposure", 120, true,  "HIGH EXPOSURE / CLIPPING"   },
        { "crush",    70,  true,  "TONAL CRUSH TO B/W"         },
        { "toner",    0,   false, "TONER BLOOM"                },
        { "mono",     0,   false, "DESATURATE"                 },
        { "grain",    46,  true,  "SEEDED GRAIN"               },
        { "requant",  90,  true,  "MJPEG REQUANTISATION"       },
    };
    constexpr int kNumOps = (int) (sizeof (kOpDefs) / sizeof (kOpDefs[0]));
}

/* ======================================================================== */
/*  Watcher -- the INTAKE folder                                             */
/*                                                                          */
/*  See the header for why this is a poll and not a filesystem notification. */
/*  Everything in here runs on the intake TimeSliceThread: the directory     */
/*  walk, the SHA-256, the ledger append and the ffmpeg proof render. Only   */
/*  the finished Scan crosses to the message thread.                         */
/* ======================================================================== */
class PlatePanel::Watcher : public juce::TimeSliceClient
{
public:
    /* Constructed from PlatePanel's constructor, i.e. on the message thread --
     * which matters, because building a juce::Component::SafePointer from a
     * raw pointer installs a WeakReference master on the component and that is
     * not a thread-safe operation. Build it ONCE, here, and copy it on the
     * watcher thread (copying is just an atomic refcount bump). Getting this
     * backwards gives you a race that fires perhaps one time in a thousand
     * ingests, which is the worst possible frequency for a bug. */
    explicit Watcher (PlatePanel& o) : owner (o), ownerRef (&o) {}

    int useTimeSlice() override
    {
        const juce::File dir = PlatePanel::intakeDir();
        if (! dir.isDirectory())
        {
            dir.createDirectory();
            return 2000;
        }

        /* The register answers from an empty set until its own bootstrap
         * parse finishes. Ingesting before then would re-accession every file
         * that is already in the ledger, so wait -- a second of latency on a
         * folder a human walks to is not a cost. */
        if (! Ledger::shared().isLoaded())
            return 1000;

        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false))
        {
            if (f.isHidden() || ! PlatePanel::isImageFile (f))
                continue;

            const juce::String key = f.getFullPathName();
            Seen& s = seen[key];
            if (s.done)
                continue;

            const juce::int64 size = f.getSize();
            const juce::int64 mtime = f.getLastModificationTime().toMilliseconds();

            if (size != s.size || mtime != s.mtime)
            {
                /* Still being written, or seen for the first time. Remember
                 * and come back. */
                s.size = size;
                s.mtime = mtime;
                s.stable = 0;
                continue;
            }

            if (++s.stable < 2)         // two consecutive identical polls
                continue;

            s.done = true;
            ingest (f);
            return 400;                 // one file per slice; keep the poll snappy
        }
        return 1500;
    }

private:
    struct Seen { juce::int64 size = -1, mtime = -1; int stable = 0; bool done = false; };

    void ingest (const juce::File& f)
    {
        /* Already in the register (a previous session accessioned it): adopt
         * nothing, just show it. */
        Record existing;
        if (Ledger::shared().findByFile (f, existing))
        {
            deliver (existing, f);
            return;
        }

        Record proto;
        proto.tool = "PLATE/INTAKE";
        proto.extra.set ("x-ingest", "watched-folder");

        {
            /* Consume the armed link, if there is one. This is the round trip
             * closing: the thing that just landed on the glass is a photograph
             * of the plate whose serial is sitting here. */
            const juce::ScopedLock sl (owner.linkLock);
            if (owner.linkSerial.isNotEmpty())
            {
                proto.derivedFrom.add (owner.linkSerial);
                owner.linkSerial.clear();
            }
        }

        /* renameToSerial is FALSE, deliberately. The brief is "leave the
         * original untouched": what lands in INTAKE is the only copy of a
         * physical event that cannot be repeated, and a rename is a write. The
         * record carries the sha256, so identity survives the player moving or
         * renaming it later anyway. */
        Record r = Ledger::shared().adopt (f, Kind::Scn, f.getFileName(), false, proto);

        deliver (r, f);
    }

    /* A proof thumbnail, so the panel can show what arrived without decoding
     * a 200 MB TIFF on the message thread. One ffmpeg call; ffmpeg reads a
     * file and writes a file, which is the only shape available (JUCE's
     * ChildProcess cannot write to a child's stdin). */
    static juce::File proofFor (const Record& r, const juce::File& src)
    {
        const juce::File out = PlatePanel::proofsDir().getChildFile (r.serial + ".jpg");
        if (out.existsAsFile())
            return out;

        PlatePanel::proofsDir().createDirectory();

        juce::StringArray args;
        args.add (ffmpegExe());
        args.add ("-nostdin"); args.add ("-v"); args.add ("error"); args.add ("-y");
        args.add ("-i"); args.add (src.getFullPathName());
        args.add ("-vf"); args.add ("scale=260:-1:flags=bilinear");
        args.add ("-frames:v"); args.add ("1");
        args.add ("-q:v"); args.add ("4");
        args.add (out.getFullPathName());

        juce::ChildProcess proc;
        if (proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            proc.waitForProcessToFinish (60000);

        return out.existsAsFile() ? out : juce::File();
    }

    void deliver (const Record& r, const juce::File& src)
    {
        Scan s;
        s.serial = r.serial;
        s.name   = src.getFileName();
        s.file   = src;
        s.parent = r.derivedFrom.joinIntoString (" ");

        const juce::File proof = proofFor (r, src);
        if (proof != juce::File())
            s.proof = juce::ImageFileFormat::loadFrom (proof);

        juce::Component::SafePointer<PlatePanel> safe = ownerRef;
        juce::MessageManager::callAsync ([safe, s]
        {
            if (safe != nullptr) safe->scanArrived (s);
        });
    }

    PlatePanel& owner;
    juce::Component::SafePointer<PlatePanel> ownerRef;
    std::map<juce::String, Seen> seen;
};

/* ======================================================================== */
/*  construction                                                             */
/* ======================================================================== */

PlatePanel::PlatePanel()
{
    setTooltip (U8 ("PLATE \xe2\x80\x94 generation loss. A chain of operators applied to "
                    "its own output, N times, seeded. Drop an image, set PASSES, "
                    "press RUN, then pick the generation off the contact sheet."));

    for (int i = 0; i < kNumOps; ++i)
        ops.push_back ({ kOpDefs[i].name, kOpDefs[i].amount, kOpDefs[i].on,
                         juce::String (kOpDefs[i].note) });

    seed = (juce::uint32) juce::Random::getSystemRandom().nextInt()
         ^ (juce::uint32) juce::Time::getHighResolutionTicks();
    if (seed == 0) seed = 0x9F3C21ABu;

    auto plate = [this] (PlateButton& b, const char* tip)
    {
        b.setTooltip (U8 (tip));
        b.setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    };

    plate (runBtn, "RUN \xe2\x80\x94 apply the chain to its own output PASSES times, "
                   "writing every generation to disk. Runs on a worker; the console "
                   "keeps playing.");
    runBtn.onToggle = [this] (bool) { startRun(); };

    plate (stopBtn, "STOP \xe2\x80\x94 kill the render. The generations already written "
                    "stay: they are real plates, not a partial file.");
    stopBtn.onToggle = [this] (bool) { cancelRun(); };

    plate (pickBtn, "PICK \xe2\x80\x94 choose a source image from disk.");
    pickBtn.onToggle = [this] (bool) { pickSource(); };

    plate (lockerBtn, "< LOCKER \xe2\x80\x94 take the LOCKER's selected row as the source.");
    lockerBtn.onToggle = [this] (bool) { takeLockerSelection(); };

    plate (rollBtn, "ROLL \xe2\x80\x94 a new master seed. Every operator's randomness "
                    "derives from it, so the same seed and chain always give the "
                    "same ladder.");
    rollBtn.onToggle = [this] (bool) { rollSeed(); };

    plate (keepBtn, "KEEP \xe2\x80\x94 accession the selected generation into the "
                    "register as a PLT record, derived from the source and the recipe.");
    keepBtn.onToggle = [this] (bool) { keepSelected(); };

    plate (revealBtn, "REVEAL \xe2\x80\x94 show the selected generation in the file browser.");
    revealBtn.onToggle = [this] (bool) { revealSelected(); };

    linkBtn.setOxideStyle (true);
    plate (linkBtn, "LINK NEXT \xe2\x80\x94 arm the round trip. Select a generation, arm "
                    "this, then print/photocopy/scan it: the next file to land in "
                    "INTAKE is accessioned as derived from that serial. The ledger is "
                    "append-only, so the link is made before the trip, not after it.");
    linkBtn.onToggle = [this] (bool on)
    {
        const juce::ScopedLock sl (linkLock);
        if (! on) { linkSerial.clear(); return; }

        juce::String target;
        if (selected >= 0 && selected < (int) gens.size())
            target = gens[(size_t) selected].serial;
        if (target.isEmpty()) target = plateSerial;
        linkSerial = target;
    };

    passesKnob.setTooltip (U8 ("PASSES \xe2\x80\x94 how many times the chain is applied to "
                               "its own output. 1\xe2\x80\x93""64."));
    passesKnob.setShowText (false);
    passesKnob.onChange = [this] (int v) { passes = juce::jlimit (1, 64, v); repaint(); };
    addAndMakeVisible (passesKnob);

    jitterKnob.setTooltip (U8 ("JITTER \xe2\x80\x94 how far each pass may deviate from the "
                               "last. 0 = every pass identical; the loss still compounds."));
    jitterKnob.setShowText (false);
    jitterKnob.onChange = [this] (int v) { jitter = juce::jlimit (0, 255, v); repaint(); };
    addAndMakeVisible (jitterKnob);

    seedField.setMultiLine (false);
    seedField.setReturnKeyStartsNewLine (false);
    seedField.setFont (Type::mono (12.0f, 0.06f));
    seedField.setInputRestrictions (8, "0123456789abcdefABCDEF");
    seedField.setColour (juce::TextEditor::textColourId, C::INK);
    seedField.setColour (juce::TextEditor::highlightColourId, C::BLOOD_DEEP);
    seedField.setJustification (Justification::centredLeft);
    seedField.setText (hex8 (seed), juce::dontSendNotification);
    seedField.setTooltip (U8 ("SEED \xe2\x80\x94 32-bit master seed, hex. Written into the "
                              "recipe and the ledger record; the ladder is reproducible "
                              "from it."));
    seedField.onReturnKey  = [this] { applySeedField(); };
    seedField.onFocusLost  = [this] { applySeedField(); };
    addAndMakeVisible (seedField);

    setWantsKeyboardFocus (true);

    intakeDir().createDirectory();
    platesDir().createDirectory();

    watcher = std::make_unique<Watcher> (*this);
    intakeThread.startThread (juce::Thread::Priority::background);
    intakeThread.addTimeSliceClient (watcher.get());

    /* RUN is drawn dead until there is something to degrade. The plate that
     * cannot do anything should look like it cannot do anything -- the
     * alternative is a button that reports an error for a mistake the UI could
     * have prevented. */
    runBtn.setEnabled (false);
    setStatus ("NO SOURCE. DROP AN IMAGE, OR PICK ONE.");
}

PlatePanel::~PlatePanel()
{
    /* Stop the watcher FIRST and explicitly: it holds a reference to this
     * object and calls into it, and a TimeSliceThread that is still running
     * while the members below are being destroyed is a use-after-free that
     * only shows up on a slow machine. */
    intakeThread.removeTimeSliceClient (watcher.get());
    intakeThread.stopThread (4000);
    watcher.reset();

    /* A ladder still rendering: kill the child rather than leaving an orphan
     * ffmpeg chain running after the console is gone. The worker holds its own
     * shared_ptr to RunState, so it is safe for it to finish on its own. */
    cancelRun();
}

/* ======================================================================== */
/*  paths and tools                                                          */
/* ======================================================================== */

juce::File PlatePanel::intakeDir()  { return morgue::morgueDir().getChildFile ("INTAKE"); }
juce::File PlatePanel::platesDir()  { return morgue::morgueDir().getChildFile ("PLATES"); }
juce::File PlatePanel::proofsDir()  { return platesDir().getChildFile ("PROOFS"); }

bool PlatePanel::isImageFile (const juce::File& f)
{
    const juce::String e = f.getFileExtension().toLowerCase();
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tif" || e == ".tiff"
        || e == ".bmp" || e == ".webp" || e == ".ppm" || e == ".pgm";
}

/* tools/degrade.py, wherever this build was run from.
 *
 * The script is the engine, so not finding it is a real failure and is
 * reported as one rather than silently degrading to a C++ reimplementation of
 * the ffmpeg chains -- which is exactly the duplication the script exists to
 * prevent. MORGUE_DEGRADE overrides everything, for a player who keeps the
 * checkout somewhere this walk cannot reach. */
juce::File PlatePanel::findScript()
{
    const juce::String env = juce::SystemStats::getEnvironmentVariable ("MORGUE_DEGRADE", {});
    if (env.isNotEmpty() && juce::File::isAbsolutePath (env))
    {
        const juce::File f (env);
        if (f.existsAsFile()) return f;
    }

    auto walkUp = [] (juce::File d) -> juce::File
    {
        for (int i = 0; i < 8 && d != juce::File(); ++i)
        {
            const juce::File c = d.getChildFile ("tools").getChildFile ("degrade.py");
            if (c.existsAsFile()) return c;
            const juce::File parent = d.getParentDirectory();
            if (parent == d) break;
            d = parent;
        }
        return {};
    };

    juce::File f = walkUp (juce::File::getSpecialLocation (
                               juce::File::currentExecutableFile).getParentDirectory());
    if (f != juce::File()) return f;

    f = walkUp (juce::File::getCurrentWorkingDirectory());
    if (f != juce::File()) return f;

    const juce::File inSession = morgue::morgueDir().getChildFile ("tools")
                                                    .getChildFile ("degrade.py");
    return inSession.existsAsFile() ? inSession : juce::File();
}

/* WORKER THREAD ONLY: this execs candidate interpreters.
 *
 * Returned as a String, not a File, because a bare "python3" is resolved
 * through PATH by execvp/CreateProcess and there is nothing to be gained by
 * pretending to know where it lives. The exit-code check is what filters out
 * Windows' python.exe App Execution Alias, which exists on PATH, prints an
 * advert and exits non-zero. */
static juce::String resolvePython()
{
    const juce::String env = juce::SystemStats::getEnvironmentVariable ("MORGUE_PYTHON", {});
    juce::StringArray candidates;
    if (env.isNotEmpty()) candidates.add (env);
    candidates.addArray ({ "python3", "python", "py" });

    for (const auto& c : candidates)
    {
        juce::ChildProcess proc;
        juce::StringArray args { c, "--version" };
        if (! proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            continue;
        if (! proc.waitForProcessToFinish (8000))
        {
            proc.kill();
            continue;
        }
        if (proc.getExitCode() == 0)
            return c;
    }
    return {};
}

juce::String PlatePanel::recipeText (const std::vector<PlateOp>& chain,
                                     juce::uint32 s, int nPasses, int nJitter)
{
    /* The same `key value` grammar as session.conf and ACCESSION.ledger, for
     * the same reason: this project has one config dialect and a second would
     * be a second parser. degrade.py reads exactly this. */
    juce::String t;
    t << "# MORGUE -- degradation recipe, written by the PLATE panel.\n"
      << "# Plain text, edit it if you like. `degrade.py ops` lists the operators.\n"
      << "version 1\n"
      << "seed " << hex8 (s) << "\n"
      << "passes " << nPasses << "\n"
      << "jitter " << nJitter << "\n\n";
    for (const auto& o : chain)
        t << "op " << o.name.paddedRight (' ', 9) << " "
          << juce::String (o.amount).paddedLeft (' ', 3) << " "
          << (o.on ? "1" : "0") << "\n";
    return t;
}

/* ======================================================================== */
/*  geometry                                                                 */
/* ======================================================================== */

Rectangle<int> PlatePanel::railArea() const
{
    return { 0, headerBandH, kRail, juce::jmax (0, getHeight() - headerBandH - kFooterH) };
}

Rectangle<int> PlatePanel::sheetArea() const
{
    const int x = kRail + 1;
    return { x, headerBandH + kSheetHeadH,
             juce::jmax (0, getWidth() - x),
             juce::jmax (0, getHeight() - headerBandH - kSheetHeadH - kFooterH - kDetailH) };
}

Rectangle<int> PlatePanel::detailArea() const
{
    return { kRail + 1, juce::jmax (0, getHeight() - kFooterH - kDetailH),
             juce::jmax (0, getWidth() - kRail - 1), kDetailH };
}

Rectangle<int> PlatePanel::sourceWellArea() const
{
    return { 8, headerBandH + 18, kRail - 16, 108 };
}

Rectangle<int> PlatePanel::chainRowArea (int i) const
{
    const int top = headerBandH + 18 + 108 + 24 + 8 + 18 + 52 + 18;
    return { 0, top + i * kChainRowH, kRail, kChainRowH };
}

Rectangle<int> PlatePanel::chainAmountArea (int i) const
{
    Rectangle<int> r = chainRowArea (i);
    return { r.getX() + 118, r.getY() + 9, 104, 6 };
}

Rectangle<int> PlatePanel::progressArea() const
{
    const int top = chainRowArea (kNumOps - 1).getBottom() + 8 + 26;
    return { 8, top, kRail - 16, 14 };
}

Rectangle<int> PlatePanel::intakeArea() const
{
    const int top = progressArea().getBottom() + 8 + 18;
    return { 0, top, kRail, juce::jmax (0, railArea().getBottom() - top) };
}

Rectangle<int> PlatePanel::intakeRowArea (int i) const
{
    Rectangle<int> a = intakeArea();
    return { a.getX(), a.getY() + i * kIntakeRowH, a.getWidth(), kIntakeRowH };
}

/* The sheet is a real contact sheet: EVERY generation at once, sized to fit,
 * with no scrolling. Scrolling would defeat the purpose -- the whole reason
 * the ladder is printed rather than summarised is so the eye can find the rung
 * where the picture stopped being a photograph, and it cannot do that through
 * a letterbox. Cells are chosen by trying every column count and keeping the
 * one that yields the largest cell. */
void PlatePanel::sheetGrid (int& cols, int& rows, int& cw, int& ch) const
{
    const Rectangle<int> a = sheetArea();
    const int n = juce::jmax (1, (int) gens.size());
    const int gap = 6, label = 11;

    cols = 1; rows = n; cw = 0; ch = 0;
    double best = -1.0;

    for (int c = 1; c <= n; ++c)
    {
        const int r = (n + c - 1) / c;
        const int w = (a.getWidth() - gap * (c + 1)) / c;
        const int h = (a.getHeight() - gap * (r + 1)) / r;
        if (w < 24 || h < 24 + label) continue;
        const double score = juce::jmin ((double) w / 4.0, (double) (h - label) / 3.0);
        if (score > best)
        {
            best = score; cols = c; rows = r; cw = w; ch = h;
        }
    }
    if (best < 0.0)                    // degenerate window: one tiny row
    {
        cols = juce::jmax (1, a.getWidth() / 40);
        rows = (n + cols - 1) / cols;
        cw = juce::jmax (8, (a.getWidth() - gap * (cols + 1)) / cols);
        ch = juce::jmax (8, (a.getHeight() - gap * (rows + 1)) / juce::jmax (1, rows));
    }
}

Rectangle<int> PlatePanel::cellArea (int i) const
{
    int cols, rows, cw, ch;
    sheetGrid (cols, rows, cw, ch);
    const Rectangle<int> a = sheetArea();
    const int gap = 6;
    const int cx = i % cols, cy = i / cols;
    return { a.getX() + gap + cx * (cw + gap), a.getY() + gap + cy * (ch + gap), cw, ch };
}

void PlatePanel::resized()
{
    const int y0 = headerBandH + 18 + 108 + 4;
    pickBtn  .setBounds (8, y0, 84, 18);
    lockerBtn.setBounds (96, y0, 84, 18);

    const int py = y0 + 24 + 8 + 18;
    passesKnob.setBounds (12, py + 4, 32, 32);
    jitterKnob.setBounds (56, py + 4, 32, 32);
    seedField .setBounds (104, py + 12, 96, 20);
    rollBtn   .setBounds (208, py + 12, 60, 20);

    const int ry = chainRowArea (kNumOps - 1).getBottom() + 8;
    runBtn .setBounds (8, ry, 120, 22);
    stopBtn.setBounds (132, ry, 72, 22);

    Rectangle<int> d = detailArea();
    const int by = d.getBottom() - 28;
    keepBtn  .setBounds (d.getX() + 10, by, 84, 20);
    revealBtn.setBounds (d.getX() + 98, by, 84, 20);
    linkBtn  .setBounds (d.getX() + 186, by, 96, 20);
}

/* ======================================================================== */
/*  actions                                                                  */
/* ======================================================================== */

juce::String PlatePanel::armedLink() const
{
    const juce::ScopedLock sl (linkLock);
    return linkSerial;
}

void PlatePanel::setStatus (const juce::String& s, bool alert)
{
    status = s;
    statusIsAlert = alert;
    repaint();
}

void PlatePanel::applySeedField()
{
    const juce::String t = seedField.getText().trim();
    const juce::uint32 v = (juce::uint32) t.getHexValue64();
    seed = (v == 0) ? 0x9F3C21ABu : v;
    seedField.setText (hex8 (seed), juce::dontSendNotification);
    repaint();
}

void PlatePanel::rollSeed()
{
    seed = (juce::uint32) juce::Random::getSystemRandom().nextInt()
         ^ (juce::uint32) juce::Time::getHighResolutionTicks();
    if (seed == 0) seed = 0x9F3C21ABu;
    seedField.setText (hex8 (seed), juce::dontSendNotification);
    repaint();
}

void PlatePanel::setSource (const juce::File& f)
{
    if (! f.existsAsFile() || ! isImageFile (f))
    {
        setStatus ("NOT AN IMAGE: " + f.getFileName(), true);
        return;
    }

    source = f;
    sourceSerial.clear();
    sourceThumb = juce::Image();

    Record r;
    if (Ledger::shared().findByFile (f, r))
        sourceSerial = r.serial;

    /* Decoding a source preview on the message thread is the one blocking
     * read left, and it is bounded: JUCE decodes progressive JPEG and PNG at
     * a few tens of milliseconds for anything a scanner produces. A 200 MB
     * TIFF is the exception, and JUCE cannot decode TIFF at all -- for those
     * the well shows the file's name and size and no picture, which is honest
     * rather than a fabricated preview. */
    sourceThumb = juce::ImageFileFormat::loadFrom (f);

    sourceMeta = humanSize (f.getSize());
    if (sourceThumb.isValid())
        sourceMeta = juce::String (sourceThumb.getWidth()) + U8 (" \xc3\x97 ")
                   + juce::String (sourceThumb.getHeight()) + U8 (" \xc2\xb7 ") + sourceMeta;

    gens.clear();
    selected = -1;
    plateSerial.clear();
    recipeSerial.clear();
    recipeDigest.clear();
    runBtn.setEnabled (true);
    setStatus ("SOURCE SET" + (sourceSerial.isNotEmpty() ? " / " + sourceSerial
                                                         : juce::String()));
}

void PlatePanel::pickSource()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Source plate",
        intakeDir().isDirectory() ? intakeDir() : morgue::morgueDir(),
        "*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.webp");

    juce::Component::SafePointer<PlatePanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        const juce::File f = fc.getResult();
        if (f != juce::File()) safe->setSource (f);
    });
}

void PlatePanel::takeLockerSelection()
{
    if (! getLockerSelection)
    {
        setStatus ("LOCKER NOT WIRED", true);
        return;
    }
    const juce::File f = getLockerSelection();
    if (f == juce::File())
    {
        setStatus ("NOTHING SELECTED IN THE LOCKER", true);
        return;
    }
    setSource (f);
}

void PlatePanel::cancelRun()
{
    auto st = run;
    if (st == nullptr) return;
    st->cancelled.store (true);
    const juce::ScopedLock sl (st->lock);
    if (st->proc != nullptr)
        st->proc->kill();
}

void PlatePanel::startRun()
{
    if (running.load())
    {
        setStatus ("ALREADY RENDERING", true);
        return;
    }
    if (! source.existsAsFile())
    {
        setStatus ("NO SOURCE. DROP AN IMAGE, OR PICK ONE.", true);
        return;
    }

    const juce::File script = findScript();
    if (script == juce::File())
    {
        setStatus ("tools/degrade.py NOT FOUND \xe2\x80\x94 SET MORGUE_DEGRADE", true);
        return;
    }

    RunSpec spec;
    spec.script  = script;
    spec.source  = source;
    spec.passes  = juce::jlimit (1, 64, passes);
    spec.jitter  = juce::jlimit (0, 255, jitter);
    spec.seedHex = hex8 (seed);
    spec.ops     = ops;
    spec.plateSerial  = Ledger::shared().mint (Kind::Plt);
    spec.sourceSerial = sourceSerial;
    spec.outDir  = platesDir().getChildFile (spec.plateSerial);

    gens.clear();
    selected = -1;
    plateSerial  = spec.plateSerial;
    recipeSerial.clear();
    recipeDigest.clear();
    runDir = spec.outDir;
    passDone.store (0);
    passTotal.store (spec.passes);
    running.store (true);
    runBtn.setToggleStateQuiet (true);
    setStatus ("RENDERING " + spec.plateSerial);

    run = std::make_shared<RunState>();
    auto st = run;
    juce::Component::SafePointer<PlatePanel> safe (this);

    /* The growSpecimen() shape: a detached worker, a SafePointer, and every
     * touch of the UI going back through the message thread. The worker never
     * dereferences the panel -- everything it needs is in `spec`, and
     * everything it must be able to kill is in `st`. */
    juce::Thread::launch ([safe, spec, st]
    {
        auto report = [safe] (const juce::String& msg, bool alert)
        {
            juce::MessageManager::callAsync ([safe, msg, alert]
            {
                if (safe != nullptr) safe->setStatus (msg, alert);
            });
        };

        const juce::String python = resolvePython();
        if (python.isEmpty())
        {
            report ("NO PYTHON ON PATH \xe2\x80\x94 SET MORGUE_PYTHON", true);
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr) { safe->running.store (false);
                                       safe->runBtn.setToggleStateQuiet (false); }
            });
            return;
        }

        spec.outDir.createDirectory();
        const juce::File recipeFile = spec.outDir.getChildFile ("PLATE.recipe");
        recipeFile.replaceWithText (PlatePanel::recipeText (spec.ops, (juce::uint32)
                                        spec.seedHex.getHexValue64(),
                                        spec.passes, spec.jitter));

        juce::StringArray args;
        args.add (python);
        args.add (spec.script.getFullPathName());
        args.add ("run");
        args.add ("--src");    args.add (spec.source.getFullPathName());
        args.add ("--out");    args.add (spec.outDir.getFullPathName());
        args.add ("--recipe"); args.add (recipeFile.getFullPathName());
        args.add ("--passes"); args.add (juce::String (spec.passes));
        args.add ("--seed");   args.add (spec.seedHex);
        args.add ("--jitter"); args.add (juce::String (spec.jitter));

        {
            const juce::ScopedLock sl (st->lock);
            st->proc = std::make_unique<juce::ChildProcess>();
            if (! st->proc->start (args, juce::ChildProcess::wantStdOut
                                         | juce::ChildProcess::wantStdErr))
            {
                st->proc.reset();
                report ("COULD NOT START " + python, true);
                juce::MessageManager::callAsync ([safe]
                {
                    if (safe != nullptr) { safe->running.store (false);
                                           safe->runBtn.setToggleStateQuiet (false); }
                });
                return;
            }
        }

        /* Read the child's stdout line by line. One line per pass, flushed by
         * the script, is the entire progress protocol -- and it is one-way by
         * necessity, because juce::ChildProcess exposes no way to write to a
         * child's stdin at all. */
        juce::String buffered, tail;
        juce::String digest;
        for (;;)
        {
            char raw[1024];
            const int n = st->proc->readProcessOutput (raw, (int) sizeof raw);
            if (n > 0)
            {
                buffered += juce::String::fromUTF8 (raw, n);
                for (;;)
                {
                    const int nl = buffered.indexOfChar ('\n');
                    if (nl < 0) break;
                    const juce::String line = buffered.substring (0, nl).trim();
                    buffered = buffered.substring (nl + 1);
                    if (line.isEmpty()) continue;

                    tail = line;
                    if (line.startsWith ("PASS "))
                    {
                        const juce::StringArray t
                            = juce::StringArray::fromTokens (line, " ", "");
                        if (t.size() >= 3)
                        {
                            const int done = t[1].getIntValue();
                            const int total = t[2].getIntValue();
                            juce::MessageManager::callAsync ([safe, done, total]
                            {
                                if (safe == nullptr) return;
                                safe->passDone.store (done);
                                safe->passTotal.store (juce::jmax (1, total));
                                safe->repaint();
                            });
                        }
                    }
                    else if (line.startsWith ("DIGEST "))
                        digest = line.fromFirstOccurrenceOf (" ", false, false).trim();
                }
                continue;
            }
            if (! st->proc->isRunning())
                break;
            juce::Thread::sleep (25);
        }

        const bool cancelled = st->cancelled.load();
        int code = 1;
        {
            const juce::ScopedLock sl (st->lock);
            if (st->proc != nullptr)
            {
                st->proc->waitForProcessToFinish (5000);
                code = st->proc->getExitCode();
            }
        }

        if (cancelled)
        {
            report ("CANCELLED \xe2\x80\x94 " + spec.plateSerial
                    + " KEPT WHAT IT HAD RENDERED", true);
        }
        else if (code != 0)
        {
            report ("DEGRADE FAILED: " + (tail.isNotEmpty() ? tail
                                                            : juce::String ("exit ")
                                                              + juce::String (code)), true);
        }

        /* Collect whatever is on disk -- a cancelled run still produced real
         * plates, and throwing them away because the player pressed STOP would
         * be the console deciding its own output was worthless. */
        std::vector<Gen> found;
        for (const auto& f : spec.outDir.findChildFiles (juce::File::findFiles, false,
                                                         "gen-*.jpg"))
        {
            Gen g;
            g.index = f.getFileNameWithoutExtension()
                       .fromLastOccurrenceOf ("-", false, false).getIntValue();
            g.full = f;
            const juce::File th = spec.outDir.getChildFile (
                "thumb-" + juce::String (g.index).paddedLeft ('0', 3) + ".jpg");
            g.thumb = juce::ImageFileFormat::loadFrom (th.existsAsFile() ? th : f);
            found.push_back (g);
        }
        std::sort (found.begin(), found.end(),
                   [] (const Gen& a, const Gen& b) { return a.index < b.index; });

        juce::String rcpSerial, pltSerial;

        if (! cancelled && code == 0 && ! found.empty())
        {
            /* The recipe becomes a record of its own (RCP), because a recipe
             * IS reproducible material: seed, chain, pass count. The plate
             * then names both its parents -- the image it came from and the
             * instructions that made it -- which is what makes the row in the
             * ledger a sentence rather than a filename. */
            Record rp;
            rp.tool = "degrade.py";
            rp.extra.set ("x-seed", spec.seedHex);
            rp.extra.set ("x-passes", juce::String (spec.passes));
            rp.extra.set ("x-jitter", juce::String (spec.jitter));
            if (digest.isNotEmpty()) rp.extra.set ("x-recipe-digest", digest);
            if (spec.sourceSerial.isNotEmpty()) rp.derivedFrom.add (spec.sourceSerial);

            const Record rr = Ledger::shared().adopt (
                recipeFile, Kind::Rcp,
                "RECIPE / " + juce::String (spec.passes) + " PASSES / SEED "
                    + spec.seedHex,
                /*renameToSerial*/ true, rp);
            rcpSerial = rr.serial;

            Record pp;
            pp.serial = spec.plateSerial;      // adopt() keeps a serial already set
            pp.tool = "degrade.py";
            pp.extra.set ("x-seed", spec.seedHex);
            pp.extra.set ("x-generation", juce::String (found.back().index));
            pp.extra.set ("x-passes", juce::String (spec.passes));
            if (digest.isNotEmpty()) pp.extra.set ("x-recipe-digest", digest);
            if (spec.sourceSerial.isNotEmpty()) pp.derivedFrom.add (spec.sourceSerial);
            if (rcpSerial.isNotEmpty())         pp.derivedFrom.add (rcpSerial);

            /* renameToSerial is FALSE: gen-NNN.jpg is a position on a ladder
             * and the manifest, the thumbnails and the sheet all refer to it
             * by that name. The run directory is already named after the
             * serial, so the artefact is still self-identifying. */
            const Record pr = Ledger::shared().adopt (
                found.back().full, Kind::Plt,
                "PLATE / GEN " + juce::String (found.back().index).paddedLeft ('0', 3)
                    + " / " + spec.source.getFileName(),
                false, pp);
            pltSerial = pr.serial;
        }

        juce::MessageManager::callAsync ([safe, found, digest, rcpSerial, pltSerial,
                                          cancelled, code]
        {
            if (safe == nullptr) return;
            safe->running.store (false);
            safe->runBtn.setToggleStateQuiet (false);
            safe->gens = found;
            safe->recipeDigest = digest;
            if (rcpSerial.isNotEmpty()) safe->recipeSerial = rcpSerial;
            if (! found.empty())
            {
                safe->selected = (int) found.size() - 1;
                if (pltSerial.isNotEmpty())
                    safe->gens.back().serial = pltSerial;
            }
            if (! cancelled && code == 0)
                safe->setStatus (juce::String ((int) found.size()) + " GENERATIONS / "
                                 + safe->plateSerial);
            if (safe->onLockerRefresh) safe->onLockerRefresh();
            safe->repaint();
        });
    });
}

void PlatePanel::keepSelected()
{
    if (selected < 0 || selected >= (int) gens.size())
    {
        setStatus ("SELECT A GENERATION FIRST", true);
        return;
    }
    const Gen g = gens[(size_t) selected];
    if (g.serial.isNotEmpty())
    {
        setStatus ("ALREADY ACCESSIONED: " + g.serial);
        return;
    }

    Record proto;
    proto.tool = "degrade.py";
    proto.extra.set ("x-seed", hex8 (seed));
    proto.extra.set ("x-generation", juce::String (g.index));
    proto.extra.set ("x-passes", juce::String (passes));
    if (recipeDigest.isNotEmpty()) proto.extra.set ("x-recipe-digest", recipeDigest);
    if (sourceSerial.isNotEmpty()) proto.derivedFrom.add (sourceSerial);
    if (recipeSerial.isNotEmpty()) proto.derivedFrom.add (recipeSerial);

    const juce::String origin = "PLATE / GEN "
        + juce::String (g.index).paddedLeft ('0', 3) + " / " + source.getFileName();
    const int idx = selected;
    const juce::File file = g.full;

    setStatus ("ACCESSIONING GEN " + juce::String (g.index).paddedLeft ('0', 3) + U8 (" \xe2\x80\xa6"));

    juce::Component::SafePointer<PlatePanel> safe (this);
    juce::Thread::launch ([safe, file, origin, proto, idx]
    {
        /* adopt() hashes and fsyncs: WORKER THREAD ONLY, as Ledger.h says. */
        const Record r = Ledger::shared().adopt (file, Kind::Plt, origin, false, proto);
        juce::MessageManager::callAsync ([safe, r, idx]
        {
            if (safe == nullptr) return;
            if (idx >= 0 && idx < (int) safe->gens.size())
                safe->gens[(size_t) idx].serial = r.serial;
            safe->setStatus (r.serial + " ACCESSIONED");
            if (safe->onLockerRefresh) safe->onLockerRefresh();
        });
    });
}

void PlatePanel::revealSelected()
{
    if (selected >= 0 && selected < (int) gens.size())
        gens[(size_t) selected].full.revealToUser();
    else if (runDir.isDirectory())
        runDir.revealToUser();
    else
        intakeDir().revealToUser();
}

void PlatePanel::scanArrived (const Scan& s)
{
    for (auto& e : scans)
        if (e.serial == s.serial) { e = s; repaint(); return; }

    scans.insert (scans.begin(), s);
    if (scans.size() > 64) scans.resize (64);

    setStatus ("INTAKE: " + s.serial + " / " + s.name
               + (s.parent.isNotEmpty() ? U8 (" \xe2\x86\x90 ") + s.parent
                                        : juce::String()));
    if (s.parent.isNotEmpty())
        linkBtn.setToggleStateQuiet (false);     // the armed link was consumed
    if (onLockerRefresh) onLockerRefresh();
    repaint();
}

void PlatePanel::sync()
{
    const bool r = running.load();
    const int p = passDone.load();
    if (r != lastPaintedRunning || p != lastPaintedPass)
    {
        lastPaintedRunning = r;
        lastPaintedPass = p;
        repaint (progressArea().expanded (4, 4));
    }
}

/* ======================================================================== */
/*  input                                                                    */
/* ======================================================================== */

void PlatePanel::mouseDown (const juce::MouseEvent& e)
{
    const juce::Point<int> p = e.getPosition();

    for (int i = 0; i < (int) ops.size(); ++i)
    {
        const Rectangle<int> row = chainRowArea (i);
        if (! row.contains (p)) continue;

        if (Rectangle<int> (row.getX() + 8, row.getY() + 6, 12, 12).contains (p))
        {
            ops[(size_t) i].on = ! ops[(size_t) i].on;
            repaint (row);
            return;
        }
        if (Rectangle<int> (row.getRight() - 44, row.getY(), 18, kChainRowH).contains (p))
        {
            if (i > 0) std::swap (ops[(size_t) i], ops[(size_t) i - 1]);
            repaint();
            return;
        }
        if (Rectangle<int> (row.getRight() - 24, row.getY(), 18, kChainRowH).contains (p))
        {
            if (i + 1 < (int) ops.size())
                std::swap (ops[(size_t) i], ops[(size_t) i + 1]);
            repaint();
            return;
        }
        if (chainAmountArea (i).expanded (0, 8).contains (p))
        {
            dragRow = i;
            dragStartAmount = ops[(size_t) i].amount;
            dragStartX = p.x;
            return;
        }
        return;
    }

    for (int i = 0; i < (int) gens.size(); ++i)
        if (cellArea (i).contains (p))
        {
            selected = i;
            repaint();
            return;
        }

    for (int i = 0; i < (int) scans.size(); ++i)
    {
        if (! intakeArea().contains (p)) break;
        if (intakeRowArea (i).contains (p))
        {
            intakeSel = i;
            setSource (scans[(size_t) i].file);
            return;
        }
    }
}

void PlatePanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragRow < 0 || dragRow >= (int) ops.size()) return;

    /* 1 unit per 2 px, cmd-drag 1 per 8 -- the same feel as every knob in the
     * console (spec section 15), so the muscle memory transfers. */
    const int div = e.mods.isCommandDown() ? 8 : 2;
    const int v = juce::jlimit (0, 255,
                                dragStartAmount + (e.getPosition().x - dragStartX) / div);
    if (v != ops[(size_t) dragRow].amount)
    {
        ops[(size_t) dragRow].amount = v;
        repaint (chainRowArea (dragRow));
    }
}

void PlatePanel::mouseUp (const juce::MouseEvent&) { dragRow = -1; }

void PlatePanel::mouseMove (const juce::MouseEvent& e)
{
    const juce::Point<int> p = e.getPosition();
    int hr = -1, hc = -1;
    for (int i = 0; i < (int) ops.size(); ++i)
        if (chainRowArea (i).contains (p)) { hr = i; break; }
    for (int i = 0; i < (int) gens.size(); ++i)
        if (cellArea (i).contains (p)) { hc = i; break; }
    if (hr != hoverRow || hc != hoverCell)
    {
        hoverRow = hr; hoverCell = hc;
        repaint();
    }
}

void PlatePanel::mouseExit (const juce::MouseEvent&)
{
    if (hoverRow != -1 || hoverCell != -1)
    {
        hoverRow = hoverCell = -1;
        repaint();
    }
}

void PlatePanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int i = 0; i < (int) gens.size(); ++i)
        if (cellArea (i).contains (e.getPosition()))
        {
            selected = i;
            gens[(size_t) i].full.startAsProcess();     // open in the system viewer
            repaint();
            return;
        }
}

bool PlatePanel::keyPressed (const juce::KeyPress& key)
{
    if (gens.empty()) return false;

    const int n = (int) gens.size();
    if (key.getKeyCode() == juce::KeyPress::leftKey)
    {
        selected = juce::jlimit (0, n - 1, (selected < 0 ? n : selected) - 1);
        repaint(); return true;
    }
    if (key.getKeyCode() == juce::KeyPress::rightKey)
    {
        selected = juce::jlimit (0, n - 1, selected + 1);
        repaint(); return true;
    }
    return false;
}

bool PlatePanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isImageFile (juce::File (f)))
            return true;
    return false;
}

void PlatePanel::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (isImageFile (juce::File (f)))
        {
            setSource (juce::File (f));
            return;
        }
}

/* ======================================================================== */
/*  painting                                                                 */
/* ======================================================================== */

/* Registration crosses at the corners (spec section 1 motifs). Four 7px
 * hairline crosses inset 6, INK_GHOST. They mark the plate area the way a
 * printer's crop marks mark a sheet, which is exactly what the sheet is. */
void PlatePanel::paintRegistration (juce::Graphics& g, Rectangle<int> r)
{
    if (r.getWidth() < 40 || r.getHeight() < 40) return;
    g.setColour (C::INK_GHOST);
    const int inset = 6, arm = 4;
    const int xs[2] = { r.getX() + inset, r.getRight() - inset - 1 };
    const int ys[2] = { r.getY() + inset, r.getBottom() - inset - 1 };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
        {
            g.fillRect (xs[i] - arm, ys[j], arm * 2 + 1, 1);
            g.fillRect (xs[i], ys[j] - arm, 1, arm * 2 + 1);
        }
}

void PlatePanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "PLATE",
                     U8 ("GENERATION LOSS \xc2\xb7 FFMPEG \xc2\xb7 tools/degrade.py"),
                     juce::String (kSerial) + U8 (" \xc2\xb7 INTAKE: ")
                         + intakeDir().getFileName(),
                     Badge::LIVE, "LIVE");

    paintRail (g);
    paintChain (g);
    paintSheet (g);
    paintDetail (g);
    paintIntake (g);

    // the rail / stage divider
    g.setColour (C::HAIRLINE);
    g.fillRect (kRail, headerBandH, 1, getHeight() - headerBandH - kFooterH);

    /* ---- footer 26: status, then the deadpan note ---------------------- */
    Rectangle<int> foot = getLocalBounds().removeFromBottom (kFooterH);
    g.setColour (C::PANEL_ALT);
    g.fillRect (foot);
    g.setColour (C::HAIRLINE);
    g.fillRect (foot.getX(), foot.getY(), foot.getWidth(), 1);

    g.setFont (Type::mono (8.0f, 0.12f));
    g.setColour (statusIsAlert ? C::BLOOD_HOT : C::INK_FAINT);
    g.drawText (status, foot.reduced (10, 0), Justification::centredLeft, true);

    g.setColour (C::OXIDE);
    g.drawText (U8 ("PRINT IT \xc2\xb7 COPY IT \xc2\xb7 SCAN IT BACK INTO INTAKE \xc2\xb7 "
                    "ARM LINK NEXT BEFORE YOU WALK OVER"),
                foot.reduced (10, 0), Justification::centredRight);
}

void PlatePanel::paintRail (juce::Graphics& g)
{
    /* ---- SOURCE ---- */
    paintLabelRow (g, { 0, headerBandH, kRail, 18 }, "SOURCE",
                   sourceSerial.isNotEmpty() ? sourceSerial : juce::String());

    const Rectangle<int> well = sourceWellArea();
    g.setColour (C::SOCKET);
    g.fillRect (well);
    g.setColour (C::HAIRLINE_DIM);
    g.drawRect (well, 1);

    if (sourceThumb.isValid())
    {
        g.drawImageWithin (sourceThumb, well.getX() + 1, well.getY() + 1,
                           well.getWidth() - 2, well.getHeight() - 20,
                           juce::RectanglePlacement::centred
                               | juce::RectanglePlacement::onlyReduceInSize);
    }
    else
    {
        g.setColour (C::INK_GHOST);
        g.setFont (Type::mono (9.0f, 0.10f));
        g.drawText (source == juce::File() ? U8 ("\xe2\x80\x94 NO SOURCE \xe2\x80\x94")
                                           : juce::String ("NO PREVIEW"),
                    well.withTrimmedBottom (20), Justification::centred);
    }

    g.setFont (Type::mono (8.0f, 0.10f));
    g.setColour (C::INK_DIM);
    g.drawText (source == juce::File() ? juce::String ("DROP AN IMAGE HERE")
                                       : source.getFileName(),
                well.getX() + 4, well.getBottom() - 19, well.getWidth() - 8, 10,
                Justification::centredLeft, true);
    g.setColour (C::INK_FAINT);
    g.drawText (sourceMeta, well.getX() + 4, well.getBottom() - 9,
                well.getWidth() - 8, 9, Justification::centredLeft, true);

    /* ---- LADDER params ---- */
    const int py = headerBandH + 18 + 108 + 4 + 24 + 8;
    paintLabelRow (g, { 0, py, kRail, 18 }, "LADDER",
                   juce::String (passes) + U8 (" \xc3\x97 \xc2\xb7 JITTER ")
                       + juce::String (jitter).paddedLeft ('0', 3));

    /* The two knobs draw faces only (setShowText(false)); their captions and
     * values are painted here so the whole parameter block sits on one
     * baseline grid instead of each knob choosing its own. */
    g.setFont (Type::mono (7.0f, 0.10f));
    g.setColour (C::INK_DIM);
    g.drawText ("PASSES " + juce::String (passes).paddedLeft ('0', 2),
                4, py + 18 + 36, 48, 9, Justification::centred);
    g.drawText ("JIT " + juce::String (jitter).paddedLeft ('0', 3),
                52, py + 18 + 36, 44, 9, Justification::centred);
    g.drawText ("SEED", 104, py + 18 + 2, 96, 9, Justification::centredLeft);

    /* the seed field's own well (the editor draws its text; this is the frame) */
    const Rectangle<int> sf (104, py + 18 + 12, 96, 20);
    g.setColour (C::SOCKET);
    g.fillRect (sf.expanded (0, 0));
    g.setColour (C::HAIRLINE);
    g.drawRect (sf, 1);
}

void PlatePanel::paintChain (juce::Graphics& g)
{
    const Rectangle<int> first = chainRowArea (0);
    paintLabelRow (g, { 0, first.getY() - 18, kRail, 18 }, "CHAIN",
                   U8 ("DRAG \xc2\xb7 REORDER"));

    for (int i = 0; i < (int) ops.size(); ++i)
    {
        const PlateOp& o = ops[(size_t) i];
        const Rectangle<int> row = chainRowArea (i);

        if (i == hoverRow)
        {
            g.setColour (C::PLATE_HOVER);
            g.fillRect (row);
        }
        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

        // enable box: a 12px plate, blood when the operator is in the chain
        const Rectangle<int> box (row.getX() + 8, row.getY() + 6, 12, 12);
        g.setColour (o.on ? C::BLOOD_DEEP : C::PLATE_LOW);
        g.fillRect (box);
        g.setColour (o.on ? C::BLOOD : C::EDGE);
        g.drawRect (box, 1);
        if (o.on)
        {
            g.setColour (C::BLOOD_HOT);
            g.fillRect (box.getX() + 4, box.getY() + 4, 4, 4);
        }

        g.setFont (Type::mono (9.0f, 0.14f));
        g.setColour (o.on ? C::INK : C::INK_GHOST);
        g.drawText (o.name.toUpperCase(), row.getX() + 26, row.getY(), 90, kChainRowH,
                    Justification::centredLeft);

        // amount: a hairline trough with a blood fill, the fader grammar at 6px
        const Rectangle<int> amt = chainAmountArea (i);
        g.setColour (C::TROUGH);
        g.fillRect (amt);
        g.setColour (C::HAIRLINE);
        g.drawRect (amt, 1);
        const int fill = juce::roundToInt ((amt.getWidth() - 2) * (o.amount / 255.0));
        g.setColour (o.on ? C::BLOOD : C::LAMP_DEAD);
        g.fillRect (amt.getX() + 1, amt.getY() + 1, fill, amt.getHeight() - 2);

        g.setFont (Type::nano (7.0f));
        g.setColour (o.on ? C::INK_DIM : C::INK_GHOST);
        g.drawText (juce::String (o.amount).paddedLeft ('0', 3),
                    amt.getRight() + 6, row.getY(), 24, kChainRowH,
                    Justification::centredLeft);

        g.setColour (i == hoverRow ? C::INK_DIM : C::INK_GHOST);
        triangle (g, { row.getRight() - 44, row.getY() + 6, 18, 12 }, true);
        triangle (g, { row.getRight() - 24, row.getY() + 6, 18, 12 }, false);

        if (i == hoverRow)
        {
            g.setColour (C::OXIDE);
            g.setFont (Type::nano (7.0f));
            g.drawText (o.note, row.getX() + 26, row.getY(), row.getWidth() - 26 - 50,
                        kChainRowH, Justification::centredRight, true);
        }
    }

    /* ---- progress trough ---- */
    const Rectangle<int> pr = progressArea();
    g.setColour (C::TROUGH);
    g.fillRect (pr);
    g.setColour (C::HAIRLINE);
    g.drawRect (pr, 1);

    const int total = juce::jmax (1, passTotal.load());
    const int done = juce::jlimit (0, total, passDone.load());
    const bool live = running.load();
    if (done > 0)
    {
        const int w = juce::roundToInt ((pr.getWidth() - 2) * (done / (double) total));
        g.setColour (live ? C::BLOOD : C::LAMP_SOUNDING);
        g.fillRect (pr.getX() + 1, pr.getY() + 1, w, pr.getHeight() - 2);
    }
    g.setFont (Type::nano (7.0f));
    g.setColour (live ? C::ARMED_TEXT : C::INK_FAINT);
    g.drawText (live ? "PASS " + juce::String (done) + " / " + juce::String (total)
                     : (gens.empty() ? juce::String ("IDLE")
                                     : juce::String ((int) gens.size()) + " GENERATIONS"),
                pr, Justification::centred);
}

void PlatePanel::paintSheet (juce::Graphics& g)
{
    const Rectangle<int> head (kRail + 1, headerBandH, getWidth() - kRail - 1, kSheetHeadH);
    g.setColour (C::PANEL_ALT);
    g.fillRect (head);
    g.setColour (C::HAIRLINE);
    g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

    g.setFont (Type::label());
    g.setColour (C::INK_DIM);
    g.drawText ("CONTACT SHEET", head.getX() + 10, head.getY(), 160, kSheetHeadH,
                Justification::centredLeft);

    g.setFont (Type::mono (8.0f, 0.10f));
    g.setColour (C::INK_FAINT);
    juce::String right = plateSerial;
    if (! gens.empty())
        right << U8 (" \xc2\xb7 GEN 000\xe2\x80\x93")
              << juce::String (gens.back().index).paddedLeft ('0', 3);
    if (recipeDigest.isNotEmpty())
        right << U8 (" \xc2\xb7 ") << recipeDigest;
    g.drawText (right, head.reduced (10, 0), Justification::centredRight, true);

    const Rectangle<int> a = sheetArea();
    g.setColour (C::SOCKET);
    g.fillRect (a);
    paintRegistration (g, a);

    if (gens.empty())
    {
        g.setColour (C::INK_GHOST);
        g.setFont (Type::mono (9.0f, 0.10f));
        g.drawText (running.load() ? "RENDERING\xe2\x80\xa6"
                                   : U8 ("NO LADDER \xe2\x80\x94 SET A SOURCE AND PRESS RUN"),
                    a, Justification::centred);
        return;
    }

    for (int i = 0; i < (int) gens.size(); ++i)
    {
        const Gen& gen = gens[(size_t) i];
        const Rectangle<int> c = cellArea (i);
        if (! c.intersects (a)) continue;

        const Rectangle<int> img = c.withTrimmedBottom (11);

        g.setColour (C::PANEL);
        g.fillRect (img);
        if (gen.thumb.isValid())
            g.drawImageWithin (gen.thumb, img.getX() + 1, img.getY() + 1,
                               img.getWidth() - 2, img.getHeight() - 2,
                               juce::RectanglePlacement::centred
                                   | juce::RectanglePlacement::onlyReduceInSize);

        const bool sel = (i == selected);
        g.setColour (sel ? C::BLOOD : (i == hoverCell ? C::EDGE : C::HAIRLINE));
        g.drawRect (img, 1);

        g.setFont (Type::nano (7.0f));
        g.setColour (sel ? C::INK : C::INK_FAINT);
        g.drawText (juce::String (gen.index).paddedLeft ('0', 3),
                    c.getX(), c.getBottom() - 10, c.getWidth() / 2, 9,
                    Justification::centredLeft);

        if (gen.serial.isNotEmpty())
        {
            g.setColour (C::OXIDE);          // accessioned: it has a real serial
            g.drawText ("ACC", c.getX() + c.getWidth() / 2, c.getBottom() - 10,
                        c.getWidth() / 2 - 2, 9, Justification::centredRight);
        }
    }
}

void PlatePanel::paintDetail (juce::Graphics& g)
{
    const Rectangle<int> d = detailArea();
    g.setColour (C::PANEL_ALT);
    g.fillRect (d);
    g.setColour (C::HAIRLINE);
    g.fillRect (d.getX(), d.getY(), d.getWidth(), 1);

    const juce::String armed = armedLink();
    paintLabelRow (g, { d.getX(), d.getY() + 1, d.getWidth(), 18 }, "SPECIMEN",
                   armed.isNotEmpty() ? U8 ("LINK ARMED \xe2\x86\x92 ") + armed
                                      : juce::String());

    const juce::Font f = Type::mono (8.0f, 0.10f);
    g.setFont (f);
    int y = d.getY() + 20;

    auto line = [&] (const juce::String& k, const juce::String& v, juce::Colour vc)
    {
        g.setColour (C::INK_FAINT);
        g.drawText (k, d.getX() + 10, y, 78, 11, Justification::centredLeft);
        g.setColour (vc);
        g.drawText (v, d.getX() + 92, y, d.getWidth() - 102, 11,
                    Justification::centredLeft, true);
        y += 11;
    };

    if (selected < 0 || selected >= (int) gens.size())
    {
        g.setColour (C::INK_GHOST);
        g.drawText (U8 ("NO GENERATION SELECTED \xe2\x80\x94 CLICK A FRAME ON THE SHEET"),
                    d.getX() + 10, y, d.getWidth() - 20, 11, Justification::centredLeft);
        return;
    }

    const Gen& gen = gens[(size_t) selected];
    line ("GENERATION", juce::String (gen.index).paddedLeft ('0', 3)
                         + " OF " + juce::String (gens.back().index), C::INK);
    line ("SERIAL", gen.serial.isNotEmpty() ? gen.serial
                                            : U8 ("\xe2\x80\x94 NOT ACCESSIONED \xe2\x80\x94"),
          gen.serial.isNotEmpty() ? C::INK : C::INK_GHOST);
    line ("FILE", gen.full.getFileName() + U8 ("  \xc2\xb7  ")
                    + humanSize (gen.full.getSize()), C::INK_DIM);
    line ("RECIPE", (recipeSerial.isNotEmpty() ? recipeSerial + U8 (" \xc2\xb7 ")
                                               : juce::String())
                    + "SEED " + hex8 (seed) + U8 (" \xc2\xb7 ")
                    + juce::String (passes) + " PASSES", C::INK_DIM);

    /* The chain, printed as a sentence. This is the field the whole panel is
     * for: it is what a player reads in November when they cannot remember
     * which of forty plates was the one. */
    juce::String chain;
    for (const auto& o : ops)
        if (o.on && (o.amount > 0 || o.name == "requant"))
            chain << (chain.isEmpty() ? "" : U8 (" \xe2\x86\x92 "))
                  << o.name.toUpperCase() << " " << o.amount;
    line ("CHAIN", chain, C::OXIDE);

    if (gen.serial.isNotEmpty())
    {
        const std::vector<Record> chainUp = Ledger::shared().ancestry (gen.serial);
        juce::String anc;
        for (const auto& r : chainUp)
            anc << (anc.isEmpty() ? "" : U8 (" \xe2\x86\x90 ")) << r.serial;
        line ("DERIVED FROM", anc.isEmpty() ? U8 ("\xe2\x80\x94") : anc, C::INK_DIM);
    }
    else if (sourceSerial.isNotEmpty())
    {
        line ("WILL DERIVE", sourceSerial
                             + (recipeSerial.isNotEmpty() ? U8 (" \xc2\xb7 ") + recipeSerial
                                                          : juce::String()), C::INK_GHOST);
    }
}

void PlatePanel::paintIntake (juce::Graphics& g)
{
    const Rectangle<int> a = intakeArea();
    paintLabelRow (g, { 0, a.getY() - 18, kRail, 18 }, "INTAKE",
                   juce::String ((int) scans.size()) + " SCANS");

    g.setColour (C::SOCKET);
    g.fillRect (a);

    if (scans.empty())
    {
        g.setColour (C::INK_GHOST);
        g.setFont (Type::nano (7.0f));
        g.drawText (U8 ("WATCHING \xe2\x80\xa6 DROP A SCAN INTO"),
                    a.getX() + 8, a.getY() + 8, a.getWidth() - 16, 10,
                    Justification::centredLeft);
        g.drawText (intakeDir().getFullPathName(),
                    a.getX() + 8, a.getY() + 20, a.getWidth() - 16, 10,
                    Justification::centredLeft, true);
        return;
    }

    const int rows = juce::jmax (0, a.getHeight() / kIntakeRowH);
    for (int i = 0; i < juce::jmin (rows, (int) scans.size()); ++i)
    {
        const Scan& s = scans[(size_t) i];
        const Rectangle<int> row = intakeRowArea (i);

        if (i == intakeSel)
        {
            g.setColour (C::TAB_ACTIVE_BG);
            g.fillRect (row);
        }
        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (row.getX(), row.getBottom() - 1, row.getWidth(), 1);

        const Rectangle<int> th (row.getX() + 8, row.getY() + 3, 36, kIntakeRowH - 7);
        g.setColour (C::PANEL);
        g.fillRect (th);
        if (s.proof.isValid())
            g.drawImageWithin (s.proof, th.getX(), th.getY(), th.getWidth(), th.getHeight(),
                               juce::RectanglePlacement::centred
                                   | juce::RectanglePlacement::onlyReduceInSize);
        g.setColour (C::HAIRLINE);
        g.drawRect (th, 1);

        g.setFont (Type::mono (8.0f, 0.10f));
        g.setColour (C::INK);
        g.drawText (s.serial, row.getX() + 52, row.getY() + 4, row.getWidth() - 60, 10,
                    Justification::centredLeft);
        g.setFont (Type::nano (7.0f));
        g.setColour (s.parent.isNotEmpty() ? C::OXIDE : C::INK_FAINT);
        g.drawText (s.parent.isNotEmpty() ? U8 ("\xe2\x86\x90 ") + s.parent : s.name,
                    row.getX() + 52, row.getY() + 16, row.getWidth() - 60, 10,
                    Justification::centredLeft, true);
    }
}

} // namespace morgue
