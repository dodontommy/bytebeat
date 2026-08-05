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
#include "Theme.h"
#include "Primitives.h"

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

    static constexpr int numTabs = 8;
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

/* ---- LOCKER (spec section 4) -------------------------------------------- */
class Locker : public juce::Component,
               public juce::SettableTooltipClient,
               private juce::ListBoxModel
{
public:
    Locker();
    void refresh();                             // re-scan the session dir
    void setContextHint (const juce::String&);  // per-tab: "DRAG -> LANE" etc.
    juce::File selectedFile() const;            // selected row; File() when none

    void resized() override;
    void paint (juce::Graphics&) override;

    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    juce::String getTooltipForRow (int row) override;

private:
    void growSpecimen();                        // background render into the session dir

    juce::ListBox list { "locker", this };
    juce::Array<juce::File> files;
    juce::String contextHint;
    std::unique_ptr<PlateButton> growBtn;
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
