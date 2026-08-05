/* Main.cpp -- MORGUE application shell (spec section 3 window skeleton).
 *
 * Vertical stack: TitleBar 26 / StageTabs 30 / body (left column 236 =
 * LOCKER over SCOPE 198, main stage flex) / Transport 60 / Status 20.
 * 1440x900 default, min 1180x760. A 30 Hz timer pulls engine state into
 * every panel (sync()); the engine is the single source of truth.
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include "bytebeat.h"
#include "engine.h"
#include "AudioEngine.h"
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

class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent() : licks (audio), mass (audio), status (audio)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
        setSize (1440, 900 - 26);   // window is 1440x900 incl. title bar

        // boot the instrument. The session lives at ~/MORGUE/session.conf
        // (the path the title bar prints; the same dir REC writes to) and is
        // loaded before the first-run decision, exactly like the TUI.
        bb_engine_set_defaults();
        {
            const juce::File dir = juce::File::getSpecialLocation (
                juce::File::userHomeDirectory).getChildFile ("MORGUE");
            bb_config_set_root (dir.getFullPathName().toRawUTF8());
        }
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
                locker.refresh();   // the finalized ~/MORGUE/*.wav appears at once
            }
        };

        // MIXER-context EXPORT... plate opens the export sheet (spec section 13)
        transport.onExport = [this] { selectTab (7); };

        audio.start();

        // land the user right on the expression box
        juce::Timer::callAfterDelay (400, [this] { rack.grabExprFocus(); });
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
        audio.recorder().stop();
        audio.stop();
        bb_config_save();          // the session autosaves (golden rule 6)
        bb_engine_shutdown();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

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
                                      : juce::String ("~/MORGUE"));
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

        auto left = r.removeFromLeft (236);
        scope.setBounds (left.removeFromBottom (198));
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
    }

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
            setResizeLimits (1180, 760, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            setWantsKeyboardFocus (true);
            tooltips.reset (new juce::TooltipWindow (this, 600));
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
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
