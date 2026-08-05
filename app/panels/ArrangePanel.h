/* ArrangePanel.h -- the MORGUE playlist (spec section 6, R2 v1 LIVE).
 *
 * The R2 arrangement timeline, wired to the engine song API (engine.h):
 * a 64-bar song of up to ARR_MAX_CLIPS clips on 10 lanes (8 voices, LICKS,
 * MASS), scheduled in ABSOLUTE BARS against the engine's monotonic bar
 * counter. The panel owns the EDIT MODEL (a std::vector<ArrClip>); every
 * edit republishes the whole song through bb_engine_song_publish -- the
 * engine's published snapshot is what actually plays, and clip audio
 * lifetime stays here via bb_engine_clip_create/release.
 *
 * Everything on this panel is wired. SELECT / TRIM modes, ARM LANE,
 * per-lane CAPTURE (N bars into a CLIP-<LANE>-<NNNN>.wav plus a placed
 * clip), BARS stepper, PLACE (drop the LOCKER selection at the playhead),
 * LOOP CLIP, ruler click = seek, clip move / re-lane / trim / delete /
 * loop, session rehydration, plus the two transport plates below.
 *
 * PLAY SONG / STOP SONG  -- bb_engine_song_play/_playing: the timeline's
 *   own transport, independent of master RUN. STOP is a MUTE, not a pause:
 *   the clip windows keep tracking the bar grid while stopped, so PLAY
 *   drops in wherever the song has got to. Use the ruler to move it.
 * REC: WHOLE MIX / REC: OVERDUB -- bb_engine_rec_src with BB_REC_MASTER /
 *   BB_REC_LIVE. OVERDUB prints everything EXCEPT the arrangement's clip
 *   playback, so a loop of arranged material can be played over and only
 *   the new layer is captured instead of the backing stacking up on every
 *   pass. Applies to REC and to the network sink.
 *
 * The DRAW / SLIP / CONSOLIDATE chips and the automation lane's ARM
 * CAPTURE tag are gone rather than drawn disabled: they sat on the same
 * grid, in the same plate grammar, as the working plates and were not
 * hit-tested anywhere. The automation lane itself stays -- it plots real
 * lock-lane state -- but it is a READ-ONLY display and now says so; the
 * locks are edited on the RACK lock lane.
 *
 * Tempo stretches nothing: clip audio always plays 1:1 at the device rate
 * from its window start; a BPM change moves the bar grid under the audio.
 *
 * The view is a 32-bar window into the 64-bar song; it jumps (never
 * animates) in 32-bar pages to keep the playhead visible.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Theme.h"
#include "Primitives.h"
#include "engine.h"

#include <memory>
#include <vector>

namespace morgue
{

/* ======================================================================== */
/*  ClipComponent -- the clip visual grammar (spec section 6).               */
/*                                                                          */
/*  Inset 2 vertically (the parent applies it when placing the clip),       */
/*  1px border, a filled title bar across the top (Type::rowH of the 8px    */
/*  floor, so the name is not clipped), a 14px striped waveform strip at    */
/*  the bottom. Colourways -- fill / border:                                */
/*    AUDIO    PLATE / EDGE   ·  RECORDED BLOOD_DEEP / BLOOD                */
/*    PATTERN  CONTROL / OXIDE_DIM                                          */
/*  The title itself is INK_BRIGHT in all three: it sits on the filled      */
/*  title bar, i.e. on the BORDER colour, which is a mid tone.              */
/*  ArrangePanel paints its clips through the static helper.                */
/* ======================================================================== */
class ClipComponent : public juce::Component
{
public:
    enum Kind { AUDIO = 0, RECORDED, PATTERN };

    explicit ClipComponent (Kind k = AUDIO, juce::String title = {});
    void setClip (Kind, const juce::String& title);

    static void paintClip (juce::Graphics&, juce::Rectangle<int> area,
                           Kind, const juce::String& title);

    void paint (juce::Graphics&) override;

private:
    Kind kind = AUDIO;
    juce::String title;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipComponent)
};

/* ======================================================================== */
/*  ArrangePanel -- the live R2 timeline (spec section 6).                   */
/* ======================================================================== */
class ArrangePanel : public juce::Component, private juce::Timer
{
public:
    ArrangePanel();
    ~ArrangePanel() override;

    /* ---- Main.cpp wiring --------------------------------------------- */
    /* Selected LOCKER row for the PLACE plate; juce::File() when none.    */
    std::function<juce::File()> getLockerSelection;
    /* Fired after a capture writes CLIP-<LANE>-<NNNN>.wav into the session dir
     * so the LOCKER list can re-scan. Message thread.                     */
    std::function<void()> onLockerRefresh;

    /* Call once AFTER bb_config_load(): pulls the loaded song meta from
     * bb_engine_song_get, re-decodes each clip's source WAV (audio is not
     * persisted), republishes. Missing files stay silent ghost clips.     */
    void rehydrateFromSession();

    /* 30 Hz engine pull (capture status, playhead paging, button states).
     * The panel also self-drives this from an internal timer, so wiring
     * it into the MainComponent sync fan-out is optional; it is
     * idempotent and cheap.                                               */
    void sync();

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override { sync(); }

    /* ---- geometry (spec section 6, fixed) ----------------------------- */
    struct Geom
    {
        juce::Rectangle<int> toolbar, ruler, lanes, autoLane;
        int laneX() const noexcept { return ruler.getX() + 120; }
        int laneW() const noexcept { return juce::jmax (1, ruler.getWidth() - 120); }
    };
    Geom geom() const;

    struct ToolbarSlots
    {
        juce::Rectangle<int> song, dividerA, select, trim, dividerB,
                             arm, capture, bars, loopClip, place,
                             dividerC, recSrc;
        int rightOfChips = 0;
    };
    ToolbarSlots toolbarSlots (juce::Rectangle<int> bar) const;

    /* ---- painting ------------------------------------------------------ */
    void paintToolbar    (juce::Graphics&, juce::Rectangle<int>);
    void paintRuler      (juce::Graphics&, juce::Rectangle<int>);
    void paintLanes      (juce::Graphics&, juce::Rectangle<int>);
    void paintAutomation (juce::Graphics&, juce::Rectangle<int>);

    /* playhead in absolute bars (fractional); < 0 = step clock idle */
    static float playheadBarF();
    /* current bar length in device frames, from BPM x BEATS x rate */
    static unsigned barLenFrames();

    juce::Rectangle<int> clipRect (const ArrClip&, const Geom&) const;
    int  clipAt (juce::Point<int>, const Geom&) const;   /* index or -1 */
    int  laneAt (int y, const Geom&) const;              /* index or -1 */
    float barAt (int x, const Geom&) const;              /* absolute, fractional */

    /* ---- edit model ---------------------------------------------------- */
    void publish();                                  /* model -> engine     */
    void deleteClip (int index);                     /* publish, then release */
    void addClip (int lane, unsigned startBar, unsigned lenBars,
                  ArrClipBuf* audio, const juce::String& name,
                  const juce::String& path);

    /* ---- capture / place ----------------------------------------------- */
    void startOrCancelCapture();
    void finishCapture();                            /* on ARR_REC_DONE     */
    void placeLockerFile();
    bool decodeMonoFile (const juce::File&, std::vector<int16_t>& out,
                         int& rateOut);
    static bool writeWav16 (const juce::File&, const int16_t*, unsigned n, int rate);
    juce::File morgueDir() const;
    juce::File nextCaptureFile (int lane) const;

    void refreshToolbarState();

    /* ---- state --------------------------------------------------------- */
    enum ToolMode { ModeSelect = 0, ModeTrim };

    std::vector<ArrClip> clips;                      /* the edit model      */
    int  selected    = -1;
    int  focusedLane = 0;
    int  armedLane   = -1;
    ToolMode mode    = ModeSelect;
    int  viewOffset  = 0;                            /* 0 or 32             */
    int  barsChoice  = 2;                            /* CAPTURE length 1/2/4/8 */

    /* capture in flight (engine writes recBuf; UI owns it, preallocated on
     * the message thread; never freed while a capture could still write) */
    std::unique_ptr<int16_t[]> recBuf;
    unsigned recCap       = 0;
    bool     recActive    = false;
    int      recLane      = -1;
    int      recBarsReq   = 0;
    int      recRate      = 0;
    unsigned recStartBar  = 0;
    int      lastRecStatus = ARR_REC_IDLE;

    /* clip drag */
    enum DragKind { DragNone = 0, DragMove, DragTrimL, DragTrimR };
    DragKind dragKind = DragNone;
    int      dragClip = -1;
    int      dragGrabOff = 0;                        /* bars into the clip  */
    unsigned dragOrigStart = 0, dragOrigLen = 1;

    /* toolbar plates -- every one of these reaches the engine */
    PlateButton songBtn    { "PLAY SONG", true,  true  };
    PlateButton selectBtn  { "SELECT",    false, true  };
    PlateButton trimBtn    { "TRIM",      false, true  };
    PlateButton armBtn     { "ARM LANE",  false, true  };
    PlateButton captureBtn { "CAPTURE",   false, false };
    PlateButton barsBtn    { "2 BARS",    false, false };
    PlateButton placeBtn   { "PLACE",     false, false };
    PlateButton loopBtn    { "LOOP CLIP", false, true  };
    PlateButton recSrcBtn  { "REC: WHOLE MIX", true, true };

    juce::AudioFormatManager formats;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangePanel)
};

} // namespace morgue
