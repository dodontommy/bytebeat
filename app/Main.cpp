/* Main.cpp -- MORGUE application shell (spec section 3 window skeleton).
 *
 * Vertical stack: TitleBar 26 / StageTabs 30 / body (left column 236 =
 * LOCKER over SCOPE 198, main stage flex) / Transport 60 / Status 20.
 * 1440x900 default, min 1180x760. A 30 Hz timer pulls engine state into
 * every panel (sync()); the engine is the single source of truth.
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

#include "bytebeat.h"
#include "engine.h"
#include "AudioEngine.h"
#include "Session.h"
#include "Theme.h"
#include "Primitives.h"
#include "panels/Chrome.h"
#include "panels/RackPanel.h"
#include "panels/ArrangePanel.h"
#include "panels/LicksPanel.h"
#include "panels/GrainMassPanel.h"
#include "panels/SurvivorPanel.h"
#include "panels/MixerPanel.h"
#include "panels/HwSyncPanel.h"
#include "panels/ExportSheet.h"
#include "panels/FieldManual.h"

using namespace morgue;

/* ---- session root ------------------------------------------------------
 * Everything that asks morgue::morgueDir() where it lives gets the answer
 * from bb_config_path(), and bb_config_path() invents an XDG-style fallback
 * the first time it is called with no root planted -- and then keeps it.
 * The LOCKER scans its directory in its own constructor, which runs before
 * any constructor body of the component that owns it, so the root has to be
 * planted before the first member is built, not in MainComponent's body.
 * That is what this one-line member exists for: declared first, it runs
 * first, and every panel after it sees the same directory the title bar
 * prints. Idempotent, so the screenshot path may call it too. */
static void plantSessionRoot()
{
    static bool done = false;
    if (done) return;
    done = true;

    bb_engine_set_defaults();
    const juce::File dir = juce::File::getSpecialLocation (
        juce::File::userHomeDirectory).getChildFile ("MORGUE");
    bb_config_set_root (dir.getFullPathName().toRawUTF8());
}

struct SessionRoot { SessionRoot() { plantSessionRoot(); } };

class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent() : licks (audio), mass (audio), status (audio)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
        setSize (1440, 900 - 26);   // window is 1440x900 incl. title bar

        // boot the instrument. The session lives at <root>/session.conf (the
        // path the title bar prints; the same dir REC writes to) and is
        // loaded before the first-run decision, exactly like the TUI. The
        // root itself was planted by the sessionRoot member, above -- see
        // plantSessionRoot() for why it cannot happen here.
        hadSession = bb_config_load() == 1;
        bb_engine_init (44100);

        addAndMakeVisible (titleBar);
        addAndMakeVisible (tabs);
        addAndMakeVisible (locker);
        addAndMakeVisible (scope);
        addAndMakeVisible (rack);
        addChildComponent (arrange);
        addChildComponent (licks);
        addChildComponent (mass);
        addChildComponent (survivor);
        addChildComponent (mixer);
        addChildComponent (hwsync);
        addChildComponent (exportSheet);
        addAndMakeVisible (transport);
        addAndMakeVisible (status);
        addChildComponent (manual);

        tabs.onSelect = [this] (int t) { selectTab (t); };
        tabs.onInfo   = [this] { showHelp (! manual.isVisible()); };
        exportSheet.onCancel = [this] { selectTab (prevTab); };
        manual.onDismiss = [this] { showHelp (false); };

        // notes/CC land on the RACK-focused voice
        hwsync.focusProvider = [this] { return rack.focusedLayer(); };

        // ARRANGE wiring: PLACE reads the LOCKER selection, captures re-scan
        // the LOCKER, and the session's song meta is rehydrated (paths ->
        // decoded clip audio) now that bb_config_load has run. Members
        // construct before the ctor body, so the panel's own constructor-time
        // rehydrate saw an empty song -- this call is the real one.
        arrange.getLockerSelection = [this] { return locker.selectedFile(); };
        arrange.onLockerRefresh    = [this] { locker.refresh(); };
        arrange.rehydrateFromSession();

        // publish the session's programs (or the defaults); first run only
        // puts the noise groove on the table when no session was restored
        ExprError e;
        for (int L = 0; L < BB_NLAYER; ++L)
            if (!bb_publish (L, bb_expr[L][0] ? bb_expr[L] : "0", &e))
                bb_publish (L, "0", &e);
        if (! hadSession)
            bb_engine_first_run();
        else
            bb_engine_demo_kit_samples();   // session persists patterns, not
                                            // sample memory: rearm slots 0-2

        // the ? plate and the ? key both open the field manual
        transport.info.onToggle = [this] (bool on) { showHelp (on); };
        manual.setVisible (false);

        // REC: toggle the WAV recorder
        transport.rec.onToggle = [this] (bool on) {
            if (on)
            {
                if (! audio.recorder().start())
                    transport.rec.setToggleStateQuiet (false);
            }
            else
            {
                audio.recorder().stop();
                locker.refresh();   // the finalized <root>/*.wav appears at once
            }
        };

        // MIXER-context EXPORT... plate opens the export sheet (spec section 13)
        transport.onExport = [this] { selectTab (7); };

        audio.start();

        /* A device that would not open used to fail in total silence. The
         * status bar is the console's notice channel; say so there. */
        if (audio.deviceError().isNotEmpty())
            status.setAlert ("AUDIO: " + audio.deviceError().toUpperCase());

        // land the user right on the expression box
        juce::Timer::callAfterDelay (400, [this] { rack.grabExprFocus(); });

        /* Baseline the autosave fingerprint AFTER the session has loaded and
         * the first-run kit is on the table, so a freshly booted console is
         * not immediately considered dirty and does not rewrite a session
         * identical to the one it just read. */
        lastFingerprint = sessionFingerprint();

        /* A first run has no session file at all, and the kit bb_engine_first_run
         * just put on the table is worth keeping. Start dirty so the file
         * exists within the first few seconds rather than only if the window
         * is closed politely. */
        if (! hadSession)
        {
            dirty        = true;
            lastChangeMs = juce::Time::getMillisecondCounterHiRes();
        }

        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        audio.recorder().stop();
        audio.stop();
        saveSession (Reason::quit);   // the session autosaves (golden rule 6)
        bb_engine_shutdown();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    /* ---- autosave -------------------------------------------------------
     * Golden rule 6, printed in the field manual, is "The session autosaves.
     * Nothing you build here is lost, including the mistakes." Until now the
     * GUI made that promise and kept it exactly once, in this destructor: a
     * crash, a power cut, a Task Manager kill or a forced restart took the
     * entire session with it, and on a fresh machine that is an hour of work
     * for a window that never got closed politely.
     *
     * So: notice changes, wait for the player to stop making them, and write.
     *   - the fingerprint below is compared four times a second and is the
     *     same state bb_config_save() writes, so nothing that persists can
     *     change without being noticed and nothing that does not persist
     *     (playhead, meters, peaks) can make the console write pointlessly;
     *   - a write happens 5 s after the LAST change, so a knob sweep is one
     *     write and not two hundred;
     *   - and never more often than once a minute, so a player who leans on
     *     a control for ten minutes still costs the disk ten writes.
     * Focus-loss and quit both flush immediately, ignoring both timers,
     * because both mean the player has stopped playing.
     *
     * This stays on the message thread deliberately. A full save is ~400
     * fprintf calls into a buffered stdio stream, well under a millisecond,
     * and bb_config_save() renames a fully-flushed temporary over the live
     * file, so a save interrupted by the power going out leaves the previous
     * session intact rather than a half-written one. A background thread
     * would buy nothing and would have to be serialised against the UI
     * writing the very state it was reading. */
    enum class Reason { idle, focusLost, quit };

    void saveSession (Reason why)
    {
        /* Quitting always writes, dirty or not -- that is exactly what this
         * component did before, and it costs a millisecond on the way out. */
        if (why != Reason::quit && ! dirty)
            return;

        const bool ok = bb_config_save() == 0;

        lastSaveMs      = juce::Time::getMillisecondCounterHiRes();
        lastFingerprint = sessionFingerprint();
        dirty           = false;

        /* The return value was thrown away here for the whole life of the
         * GUI. A full disk, a read-only home directory or a path the engine
         * cannot create all report failure, and the player has to be told:
         * the one thing worse than losing a session is being told it was
         * saved. */
        if (! ok)
            status.setAlert ("SESSION SAVE FAILED: " + morgue::sessionFileDisplay());
        else if (audio.deviceError().isEmpty())
            status.setAlert ({});       // a good write clears a stale notice
    }

    /* Called by the window when it stops being the active one. */
    void flushSessionOnFocusLoss() { saveSession (Reason::focusLost); }

    void selectTab (int t)
    {
        t = juce::jlimit (0, StageTabs::numTabs - 1, t);
        if (t != 7) prevTab = t;

        juce::Component* stagePanels[7] = { &rack, &arrange, &licks, &mass,
                                            &survivor, &mixer, &hwsync };
        for (int i = 0; i < 7; ++i)
            stagePanels[i]->setVisible (i == (t == 7 ? prevTab : t));

        // EXPORT: the sheet overlays the dimmed console (spec section 11)
        exportSheet.setVisible (t == 7);
        if (t == 7) exportSheet.toFront (false);

        tabs.setCurrent (t);
        transport.setContextTab (t);

        /* title-bar serial increments with the active panel (spec section 3;
         * ARRANGE carries the RACK serial exactly as HTML frame 02 does, and
         * the EXPORT sheet leaves the underlying panel's serial in place) */
        static const char* const tabSerials[7] = {
            SerialNo::RACK, SerialNo::RACK, SerialNo::LICKS, SerialNo::MASS,
            SerialNo::SURVIVOR, SerialNo::MIXER, SerialNo::HWSYNC
        };
        titleBar.setSerial (tabSerials[t == 7 ? prevTab : t]);

        locker.setContextHint (t == 1 ? juce::String::fromUTF8 ("DRAG \xe2\x86\x92 LANE")
                             : t == 2 ? juce::String::fromUTF8 ("DRAG \xe2\x86\x92 SLOT")
                                      : juce::String());   // empty = the real dir
        currentTab = t;
    }

    /* --screenshot=NAME: select a named view before the snapshot. */
    void selectView (const juce::String& name)
    {
        static const char* names[8] = { "rack", "arrange", "licks", "mass",
                                        "survivor", "mixer", "hwsync", "export" };
        for (int i = 0; i < 8; ++i)
            if (name == names[i]) { selectTab (i); return; }
        if (name == "manual")
            showHelp (true);
    }

    void showHelp (bool on)
    {
        manual.setVisible (on);
        transport.info.setToggleStateQuiet (on);
        if (on) manual.toFront (true);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress (juce::KeyPress::F1Key) || key.getTextCharacter() == '?')
        {
            showHelp (! manual.isVisible());
            return true;
        }
        if (key == juce::KeyPress (juce::KeyPress::escapeKey))
        {
            if (manual.isVisible()) { showHelp (false); return true; }
            // the EXPORT sheet is modal: ESC dismisses it, not the master
            if (exportSheet.isVisible()) { selectTab (prevTab); return true; }
            // ESC = CUT (panic), spec section 15
            const bool cut = atomic_load (&bb.panic) == 0;
            atomic_store (&bb.panic, cut ? 1 : 0);
            transport.cut.setToggleStateQuiet (cut);
            return true;
        }
        if (key == juce::KeyPress (juce::KeyPress::spaceKey))
        {
            // SPACE = RUN toggle
            const bool muted = atomic_load (&bb.mute) != 0;
            if (muted) { atomic_store (&bb.mute, 0); atomic_store (&bb.panic, 0); }
            else       atomic_store (&bb.mute, 1);
            return true;
        }
        if (key.getModifiers().isCommandDown()
            && (key.getKeyCode() == 'R' || key.getKeyCode() == 'r'))
        {
            const bool on = ! transport.rec.getToggleState();
            transport.rec.setToggleState (on, juce::sendNotification);
            return true;
        }

        const juce::juce_wchar ch = key.getTextCharacter();

        // SHIFT+1-8 = toggle a RACK layer on/off (the TUI's shift+digit).
        // Works from any tab -- layer on/off is engine-global state.
        if (key.getModifiers().isShiftDown()
            && ! key.getModifiers().isCommandDown()
            && ! key.getModifiers().isAltDown()
            && ! key.getModifiers().isCtrlDown())
        {
            int L = -1;
            const int kc = key.getKeyCode();
            if (kc >= '1' && kc <= '8')
                L = kc - '1';
            else
            {
                // US-layout shifted digits, for hosts whose keyCode carries
                // the shifted character instead of the digit key
                static const juce::juce_wchar shifted[8]
                    = { '!', '@', '#', '$', '%', '^', '&', '*' };
                for (int i = 0; i < 8; ++i)
                    if (ch == shifted[i])
                        L = i;
            }
            if (L >= 0)
            {
                rack.toggleVoice (L);
                return true;
            }
        }

        // 1-8 = focus voice (spec section 15). On GRAIN LICKS the digits
        // focus a sampler slot, on GRAIN MASS 1-4 select a well -- each
        // panel keeps its own map when its stage is up.
        if (ch >= '1' && ch <= '8' && ! key.getModifiers().isAnyModifierKeyDown())
        {
            if (currentTab == 2) return licks.keyPressed (key);
            if (currentTab == 3) return mass.keyPressed (key);
            rack.focusVoice ((int) (ch - '1'));
            return true;
        }

        // R / O / A / Z (and P) drive the sample wells while MASS is up.
        if (currentTab == 3 && ! key.getModifiers().isAnyModifierKeyDown())
        {
            const auto lc = (juce::juce_wchar) juce::CharacterFunctions::toLowerCase (ch);
            if (lc == 'r' || lc == 'o' || lc == 'a' || lc == 'z' || lc == 'p')
                return mass.keyPressed (key);
        }

        // M = arm motion capture (R3) and cmd-Z = undo are in the spec key
        // map but the engine has neither feature yet -- nothing is faked.
        return false;
    }

    void resized() override
    {
        auto r = getLocalBounds();

        titleBar.setBounds (r.removeFromTop (26));
        tabs.setBounds (r.removeFromTop (30));

        status.setBounds (r.removeFromBottom (20));
        transport.setBounds (r.removeFromBottom (60));

        /* The left column is 236 wide with a 198 SCOPE under the LOCKER at
         * the design size. Both are proportions of a window that is no longer
         * guaranteed to be 1440x900 -- a scaled Windows display can hand us a
         * good deal less -- so cap the column at a third of the width and the
         * SCOPE at half the column, and the stage keeps a usable share of a
         * small window instead of being squeezed to nothing. At and above the
         * design size both caps are inactive and the layout is unchanged. */
        auto left = r.removeFromLeft (juce::jmin (236, juce::jmax (120, r.getWidth() / 3)));
        scope.setBounds (left.removeFromBottom (
            juce::jmin (198, juce::jmax (0, left.getHeight() / 2))));
        locker.setBounds (left);

        // main stage (the spec section 3 divider is painted by the left
        // column's own right edge at x=235 -- panels draw no second line)
        stageArea = r;
        for (juce::Component* p : { (juce::Component*) &rack, (juce::Component*) &arrange,
                                    (juce::Component*) &licks, (juce::Component*) &mass,
                                    (juce::Component*) &survivor, (juce::Component*) &mixer,
                                    (juce::Component*) &hwsync })
            p->setBounds (stageArea);

        // the EXPORT sheet's scrim dims the ENTIRE console (spec section 11)
        exportSheet.setBounds (getLocalBounds());

        manual.setBounds (getLocalBounds());
    }

private:
    void timerCallback() override
    {
        bb_engine_reclaim();
        audio.recorder().service();
        rack.sync();
        mixer.sync();
        survivor.sync();
        transport.sync();
        licks.sync();
        arrange.sync();

        // the autosave runs at 4 Hz, not 30: see saveSession()
        if (++autosaveTick >= 8)
        {
            autosaveTick = 0;
            serviceAutosave();
        }
    }

    void serviceAutosave()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();

        const juce::uint64 fp = sessionFingerprint();
        if (fp != lastFingerprint)
        {
            lastFingerprint = fp;
            lastChangeMs    = now;
            dirty           = true;
        }

        if (dirty
            && now - lastChangeMs >= 5000.0        // the player has stopped
            && now - lastSaveMs   >= 60000.0)      // and the floor has passed
            saveSession (Reason::idle);
    }

    /* A 64-bit FNV-1a over exactly the state bb_config_save() writes, and
     * nothing else. Deliberately NOT over the transport's live counters (t,
     * k, bar, seq_pos), the meters or the peaks -- those change every single
     * frame and would make the console consider itself dirty forever, which
     * is the same as having no dirty flag at all. */
    static void hash (juce::uint64& h, juce::uint64 v)
    {
        for (int b = 0; b < 8; ++b)
        {
            h ^= (juce::uint64) ((v >> (b * 8)) & 0xff);
            h *= 0x100000001b3ULL;
        }
    }

    static void hashText (juce::uint64& h, const char* s)
    {
        for (; s != nullptr && *s != '\0'; ++s)
        {
            h ^= (juce::uint64) (unsigned char) *s;
            h *= 0x100000001b3ULL;
        }
        hash (h, 0);                              // terminator, so "ab"+"c" != "a"+"bc"
    }

    juce::uint64 sessionFingerprint()
    {
        juce::uint64 h = 0xcbf29ce484222325ULL;

        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.req_rate));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.gain));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.focus));

        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_bars));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_mix));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_feedback));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_overdub));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_rate));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_reverse));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.loop_slice));

        for (int i = 0; i < GCTL_COUNT; ++i)
            hash (h, (juce::uint64) (unsigned) atomic_load (&bb.gctl[i]));

        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.verb_size));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.verb_tone));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.verb_level));
        hash (h, (juce::uint64) (unsigned) atomic_load (&bb.smp_send));

        for (int L = 0; L < BB_NLAYER; ++L)
        {
            Layer& ly = bb.layer[L];
            hash (h, (juce::uint64) (unsigned) atomic_load (&ly.on));
            hash (h, (juce::uint64) (unsigned) atomic_load (&ly.mode));
            hash (h, (juce::uint64) (unsigned) atomic_load (&ly.seq_on));
            hash (h, (juce::uint64) (unsigned) atomic_load (&ly.send));

            for (int i = 0; i < BB_NPARAM; ++i)
                hash (h, (juce::uint64) (unsigned) atomic_load (&ly.param[i]));
            for (int i = 0; i < LCTL_COUNT; ++i)
                hash (h, (juce::uint64) (unsigned) atomic_load (&ly.ctl[i]));
            for (int i = 0; i < BB_STEPS; ++i)
            {
                hash (h, (juce::uint64) (unsigned) atomic_load (&ly.seq_gate[i]));
                hash (h, (juce::uint64) (unsigned) atomic_load (&ly.seq_pitch[i]));
                hash (h, (juce::uint64) (unsigned) atomic_load (&ly.seq_ratchet[i]));
                hash (h, (juce::uint64) (unsigned) atomic_load (&ly.seq_prob[i]));
            }
            hash (h, (juce::uint64) atomic_load (&ly.motion_mask));
            for (int k = 0; k < BB_LOCK_COUNT; ++k)
                for (int i = 0; i < BB_STEPS; ++i)
                    hash (h, (juce::uint64) (unsigned) atomic_load (&ly.seq_lock[k][i]));

            hashText (h, bb_expr[L]);
            hash (h, (juce::uint64) bb_rack[L].src);
            hash (h, (juce::uint64) bb_rack[L].body);
            hash (h, (juce::uint64) bb_rack[L].space);
            hash (h, (juce::uint64) (unsigned) bb_custom[L]);
        }

        for (int s = 0; s < BB_SAMPLER; ++s)
        {
            SamplerSlot& sl = bb.sampler[s];
            hash (h, (juce::uint64) (unsigned) atomic_load (&sl.on));
            hash (h, (juce::uint64) (unsigned) atomic_load (&sl.mute));
            hash (h, (juce::uint64) (unsigned) atomic_load (&sl.solo));
            for (int i = 0; i < SMP_CTL_COUNT; ++i)
                hash (h, (juce::uint64) (unsigned) atomic_load (&sl.ctl[i]));
            for (int i = 0; i < BB_STEPS; ++i)
            {
                hash (h, (juce::uint64) (unsigned) atomic_load (&sl.gate[i]));
                hash (h, (juce::uint64) (unsigned) atomic_load (&sl.pitch[i]));
                hash (h, (juce::uint64) (unsigned) atomic_load (&sl.vel[i]));
            }
        }

        /* The song. bb_engine_song_get copies into a caller array; the
         * scratch is a member rather than a local because ARR_MAX_CLIPS
         * ArrClips is ~55 KB and this runs four times a second. */
        const int n = bb_engine_song_get (songScratch.get(), ARR_MAX_CLIPS);
        hash (h, (juce::uint64) (unsigned) n);
        for (int c = 0; c < n; ++c)
        {
            const ArrClip& cl = songScratch[(size_t) c];
            hash (h, (juce::uint64) (unsigned) cl.lane);
            hash (h, (juce::uint64) cl.start_bar);
            hash (h, (juce::uint64) cl.len_bars);
            hash (h, (juce::uint64) (unsigned) cl.loop);
            hash (h, (juce::uint64) (unsigned) cl.gain);
            hashText (h, cl.name);
            hashText (h, cl.path);
        }

        return h;
    }

    SessionRoot       sessionRoot;    // MUST be first: see plantSessionRoot()
    MorgueLookAndFeel lnf;
    AudioEngine       audio;

    TitleBar          titleBar;
    StageTabs         tabs;
    Locker            locker;
    Scope             scope;
    RackPanel         rack;
    ArrangePanel      arrange;
    LicksPanel        licks;
    GrainMassPanel    mass;
    SurvivorPanel     survivor;
    MixerPanel        mixer;
    HwSyncPanel       hwsync;
    ExportSheet       exportSheet;
    TransportBar      transport;
    StatusBar         status;
    FieldManualOverlay manual;

    juce::Rectangle<int> stageArea;
    int currentTab = 0, prevTab = 0;
    bool hadSession = false;

    /* ---- autosave bookkeeping (see saveSession) ---- */
    std::unique_ptr<ArrClip[]> songScratch { new ArrClip[ARR_MAX_CLIPS]() };
    juce::uint64 lastFingerprint = 0;
    double lastChangeMs = 0.0;
    double lastSaveMs   = -60000.0;   // so the first settled edit may write at once
    int  autosaveTick = 0;
    bool dirty = false;
};

class MorgueApplication final : public juce::JUCEApplication
{
public:
    MorgueApplication() {}

    const juce::String getApplicationName() override       { return "MORGUE"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String& cmdLine) override
    {
        // --screenshot            -> morgue_render.png (default view)
        // --screenshot=NAME       -> select that view, write morgue_NAME.png
        //   NAME: rack arrange licks mass survivor mixer hwsync export manual
        juce::String shotView;
        for (const auto& tok : juce::StringArray::fromTokens (cmdLine, true))
        {
            if (tok == "--screenshot") grabScreenshot = true;
            else if (tok.startsWith ("--screenshot="))
            {
                grabScreenshot = true;
                shotView = tok.fromFirstOccurrenceOf ("=", false, false)
                              .trim().toLowerCase();
            }
        }

        mainWindow.reset (new MainWindow (getApplicationName()));

        if (grabScreenshot)
        {
            if (shotView.isNotEmpty())
                mainWindow->content().selectView (shotView);

            // Give the window a beat to lay out, then snapshot offscreen and
            // walk away. No screen-recording permission involved -- MORGUE
            // renders its own control room.
            juce::Timer::callAfterDelay (1200, [this, shotView]
            {
                if (auto* mw = mainWindow.get())
                {
                    juce::Image snap = mw->createComponentSnapshot (mw->getLocalBounds());
                    juce::PNGImageFormat fmt;
                    const juce::String fname = shotView.isNotEmpty()
                        ? "morgue_" + shotView + ".png" : juce::String ("morgue_render.png");
                    const juce::File outFile (juce::File::getCurrentWorkingDirectory()
                                                 .getChildFile (fname));
                    outFile.deleteFile();   // FileOutputStream appends to existing files
                    juce::FileOutputStream out (outFile);
                    if (out.openedOk() && fmt.writeImageToStream (snap, out))
                        DBG ("screenshot: " + outFile.getFullPathName());
                    systemRequestedQuit();
                }
            });
        }
    }

    void shutdown() override { mainWindow = nullptr; }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String&) override {}

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name,
                              morgue::C::GROUND,
                              DocumentWindow::allButtons)
        {
            // the in-content TitleBar (spec section 3) replaces the JUCE one
            setUsingNativeTitleBar (false);
            setTitleBarHeight (0);
            main = new MainComponent();
            setContentOwned (main, true);
            // border resizer, not the corner grip: the hatched triangle is
            // not part of the design (README: "Assets: none")
            setResizable (true, false);

            /* The design is drawn at 1440x900 with a 1180x760 floor, which is
             * fine on the 16" panel it was drawn on and impossible on a great
             * many Windows laptops: a 1920x1080 screen at the 150% scaling
             * Windows ships by default reports a 1280x680 work area, so both
             * the requested size AND the minimum were larger than the space
             * available and the window opened with its transport bar under the
             * taskbar and no way to drag it back. Ask the display what it has
             * and take the smaller of the two, for the minimum as well as the
             * opening size, so the floor can never exceed the ceiling. */
            juce::Rectangle<int> work (0, 0, 1440, 900);
            if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                work = d->userArea;

            const int w = juce::jmin (1440, work.getWidth());
            const int h = juce::jmin (900,  work.getHeight());

            setResizeLimits (juce::jmin (1180, w), juce::jmin (760, h), 10000, 10000);
            centreWithSize (w, h);
            setVisible (true);
            setWantsKeyboardFocus (true);
            tooltips.reset (new juce::TooltipWindow (this, 600));
        }

        /* The content is owned by the base class and dies with it; drop our
         * observing pointer first so a late activeWindowStatusChanged during
         * teardown cannot reach through it. */
        ~MainWindow() override { main = nullptr; }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        /* Alt-tabbing away, clicking another app, locking the screen: every
         * one of them means the player has stopped playing, and any of them
         * may be the last thing that happens before the machine dies. Flush
         * the session here rather than hoping for a polite quit. */
        void activeWindowStatusChanged() override
        {
            if (main != nullptr && ! isActiveWindow())
                main->flushSessionOnFocusLoss();
        }

        MainComponent& content() { return *main; }

    private:
        MainComponent* main = nullptr;   // owned via setContentOwned
        std::unique_ptr<juce::TooltipWindow> tooltips;
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
    bool grabScreenshot = false;
};

START_JUCE_APPLICATION (MorgueApplication)
