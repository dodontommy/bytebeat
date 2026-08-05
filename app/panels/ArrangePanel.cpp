/* ArrangePanel.cpp -- see ArrangePanel.h. Spec section 6 geometry:
 * toolbar 30, ruler 22 (120 gutter + 32 bar columns), ten 42px lanes,
 * automation lane 96. R2 v1: the timeline is LIVE -- the panel owns the
 * clip edit model and republishes it to the engine on every edit; the
 * engine's published song is what plays. Playhead, lane lamps and the
 * automation plot remain real engine state only. DRAW / SLIP /
 * CONSOLIDATE / automation ARM CAPTURE stay planned chrome (R3). */

#include "ArrangePanel.h"
#include "Session.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>          // std::move
#include <vector>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    constexpr int kGutterW   = 120;   // lane head / ruler / scale gutter
    constexpr int kToolbarH  = 30;
    constexpr int kRulerH    = 22;
    constexpr int kLaneH     = 42;
    constexpr int kNumLanes  = ARR_LANES;  // 8 voices + LICKS + MASS
    constexpr int kAutoH     = 96;
    constexpr int kAutoHeadH = 20;
    constexpr int kBars      = 32;    // visible window of the playlist
    constexpr int kSongBars  = 64;    // the whole timeline
    constexpr int kTrimZone  = 6;     // px edge zones in TRIM mode

    /* ctl-lock lane naming/indexing (bytebeat.h LOCK_* order past the params) */
    constexpr int kCtlIdx[8] = { LCTL_LEVEL, LCTL_DRIVE, LCTL_TONE, LCTL_CRUSH,
                                 LCTL_SPC_TIME, LCTL_SPC_FB, LCTL_SPC_MIX, LCTL_DECAY };
    const char* const kCtlName[8] = { "LEVEL", "DRIVE", "TONE", "CRUSH",
                                      "SP-TIME", "SP-FB", "SP-MIX", "DECAY" };

    int textW (const juce::Font& f, const juce::String& s)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
    }

    juce::String laneLabel (int lane)
    {
        if (lane < BB_NLAYER) return "V" + juce::String (lane + 1).paddedLeft ('0', 2);
        return lane == BB_NLAYER ? "LICKS" : "MASS";
    }
}

/* ======================================================================== */
/*  ClipComponent                                                            */
/* ======================================================================== */

ClipComponent::ClipComponent (Kind k, juce::String t)
    : kind (k), title (std::move (t))
{
    setInterceptsMouseClicks (false, false);   // the panel owns interaction
}

void ClipComponent::setClip (Kind k, const juce::String& t)
{
    kind = k;
    title = t;
    repaint();
}

void ClipComponent::paintClip (juce::Graphics& g, Rectangle<int> area,
                               Kind k, const juce::String& title)
{
    juce::Colour bg, bd, fg;
    switch (k)
    {
        case RECORDED: bg = juce::Colour (0xff1d1210); bd = C::BLOOD;
                       fg = C::ARMED_TEXT; break;
        case PATTERN:  bg = C::CONTROL;                bd = C::OXIDE_DIM;
                       fg = C::OXIDE_INK;  break;
        default:       bg = C::PLATE;                  bd = C::EDGE;
                       fg = C::INK_DIM;    break;
    }

    g.setColour (bg);
    g.fillRect (area);
    g.setColour (bd);
    g.drawRect (area, 1);

    Rectangle<int> inner = area.reduced (1);

    // filled 8px title bar across the top
    Rectangle<int> bar = inner.removeFromTop (8);
    g.setColour (bd);
    g.fillRect (bar);
    g.setColour (fg);
    g.setFont (Type::nano (7.0f));
    g.drawText (title, bar.withTrimmedLeft (3), Justification::centredLeft, false);

    // 14px striped waveform strip at the bottom (1px stripes every 3px)
    Rectangle<int> wave = inner.removeFromBottom (14);
    wave.removeFromLeft (2);
    wave.removeFromRight (2);
    wave.removeFromBottom (2);
    g.setColour (fg.withAlpha (0.5f));
    for (int x = wave.getX(); x < wave.getRight(); x += 3)
        g.fillRect (x, wave.getY(), 1, wave.getHeight());
}

void ClipComponent::paint (juce::Graphics& g)
{
    paintClip (g, getLocalBounds(), kind, title);
}

/* ======================================================================== */
/*  ArrangePanel -- construction / wiring                                    */
/* ======================================================================== */

ArrangePanel::ArrangePanel()
{
    formats.registerBasicFormats();

    for (PlateButton* b : { &selectBtn, &trimBtn, &armBtn, &captureBtn,
                            &barsBtn, &placeBtn, &loopBtn })
        addAndMakeVisible (*b);

    selectBtn.setTooltip (U8 ("SELECT - move clips. Drag a clip to another bar or "
                              "lane; snaps to whole bars."));
    trimBtn.setTooltip   (U8 ("TRIM - resize clips. Drag a clip's left or right "
                              "edge; whole bars, minimum 1."));
    armBtn.setTooltip    (U8 ("ARM LANE - arm the focused lane for capture. One "
                              "lane at a time; the MASS lane cannot be armed."));
    captureBtn.setTooltip (U8 ("CAPTURE - record the armed lane's output for the "
                               "set bars, starting at the next bar boundary. "
                               "Click again to cancel."));
    barsBtn.setTooltip   (U8 ("BARS - capture length in whole bars. "
                              "Cycles 1 / 2 / 4 / 8."));
    placeBtn.setTooltip  (U8 ("PLACE - place the selected LOCKER file on the "
                              "focused lane at the playhead bar. Audio plays 1:1 "
                              "at the device rate; tempo stretches nothing."));
    loopBtn.setTooltip   (U8 ("LOOP CLIP - repeat the selected clip's audio "
                              "inside its window instead of going silent."));

    selectBtn.onToggle = [this] (bool) { mode = ModeSelect; refreshToolbarState(); };
    trimBtn.onToggle   = [this] (bool) { mode = ModeTrim;   refreshToolbarState(); };

    armBtn.onToggle = [this] (bool on)
    {
        if (focusedLane >= kNumLanes - 1)          // MASS lane: engine refuses
        {
            armBtn.setToggleStateQuiet (false);
            return;
        }
        if (on)                        armedLane = focusedLane;
        else if (armedLane == focusedLane) armedLane = -1;
        refreshToolbarState();
        repaint();
    };

    captureBtn.onToggle = [this] (bool) { startOrCancelCapture(); };

    barsBtn.onToggle = [this] (bool)
    {
        barsChoice = barsChoice >= 8 ? 1 : barsChoice * 2;
        refreshToolbarState();
    };

    placeBtn.onToggle = [this] (bool) { placeLockerFile(); };

    loopBtn.onToggle = [this] (bool on)
    {
        if (selected >= 0 && selected < (int) clips.size())
        {
            clips[(size_t) selected].loop = on ? 1 : 0;
            publish();
            repaint();
        }
        else
            loopBtn.setToggleStateQuiet (false);
    };

    rehydrateFromSession();     // no-op on a blank engine; Main re-signals
    refreshToolbarState();
    startTimerHz (30);
}

ArrangePanel::~ArrangePanel()
{
    if (recActive)
    {
        /* the render thread may still write into recBuf for one more period
         * after the cancel; leak the buffer rather than free it under the
         * engine's pen (app teardown only). */
        bb_engine_arr_cancel();
        recBuf.release();
    }
    for (ArrClip& c : clips)
        if (c.audio != nullptr)
            bb_engine_clip_release (c.audio);
}

/* ======================================================================== */
/*  Engine truth helpers                                                     */
/* ======================================================================== */

float ArrangePanel::playheadBarF()
{
    const int seq = atomic_load (&bb.seq_pos);
    if (seq < 0)
        return -1.0f;                      // step clock idle: nothing to show
    const unsigned bar = atomic_load (&bb.bar);
    return (float) bar + (float) seq / (float) BB_STEPS;
}

unsigned ArrangePanel::barLenFrames()
{
    /* same math as the engine's loop clock */
    int rate  = atomic_load (&bb.rate);              if (rate  <= 0) rate  = 48000;
    int bpm   = atomic_load (&bb.gctl[GCTL_BPM]);    if (bpm   <= 0) bpm   = 90;
    int beats = atomic_load (&bb.gctl[GCTL_BEATS]);  if (beats <= 0) beats = 4;
    unsigned beat = (unsigned) (((long) rate * 60L) / bpm);
    if (beat < 1) beat = 1;
    return beat * (unsigned) beats;
}

/* ======================================================================== */
/*  Geometry                                                                 */
/* ======================================================================== */

ArrangePanel::Geom ArrangePanel::geom() const
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    Geom G;
    G.toolbar  = b.removeFromTop (kToolbarH);
    G.ruler    = b.removeFromTop (kRulerH);
    G.autoLane = b.removeFromBottom (kAutoH);
    G.lanes    = b;
    return G;
}

ArrangePanel::ToolbarSlots ArrangePanel::toolbarSlots (Rectangle<int> bar) const
{
    const juce::Font liveF    = Type::mono (10.0f, 0.16f);   // PlateButton face
    const juce::Font plannedF = Type::mono (9.0f, 0.14f);    // painted chrome
    const int chipH = 18;
    const int cy = bar.getY() + (bar.getHeight() - chipH) / 2;
    int x = bar.getX() + 8;

    ToolbarSlots s;
    auto next = [&] (const juce::Font& f, const char* text)
    {
        const int w = textW (f, text) + 16;
        Rectangle<int> r (x, cy, w, chipH);
        x += w + 3;
        return r;
    };

    s.select = next (liveF,    "SELECT");
    s.draw   = next (plannedF, "DRAW");
    s.trim   = next (liveF,    "TRIM");
    s.slip   = next (plannedF, "SLIP");

    x += 3;                                              // group gap 6
    s.divider = Rectangle<int> (x, bar.getY() + (bar.getHeight() - 18) / 2, 1, 18);
    x += 1 + 6;

    s.arm         = next (liveF,    "ARM LANE");
    s.capture     = next (liveF,    "CAPTURE");
    s.bars        = next (liveF,    "8 BARS");           // fixed at widest text
    s.loopClip    = next (liveF,    "LOOP CLIP");
    s.consolidate = next (plannedF, "CONSOLIDATE");
    s.place       = next (liveF,    "PLACE");
    s.rightOfChips = x;
    return s;
}

void ArrangePanel::resized()
{
    const ToolbarSlots s = toolbarSlots (geom().toolbar);
    selectBtn .setBounds (s.select);
    trimBtn   .setBounds (s.trim);
    armBtn    .setBounds (s.arm);
    captureBtn.setBounds (s.capture);
    barsBtn   .setBounds (s.bars);
    loopBtn   .setBounds (s.loopClip);
    placeBtn  .setBounds (s.place);
}

Rectangle<int> ArrangePanel::clipRect (const ArrClip& c, const Geom& G) const
{
    const int laneX = G.laneX(), laneW = G.laneW();
    const int relS = juce::jlimit (0, kBars, (int) c.start_bar - viewOffset);
    const int relE = juce::jlimit (0, kBars,
                                   (int) (c.start_bar + c.len_bars) - viewOffset);
    const int x0 = laneX + relS * laneW / kBars;
    const int x1 = laneX + relE * laneW / kBars;
    const int y  = G.lanes.getY() + c.lane * kLaneH;
    return Rectangle<int> (x0, y + 2, juce::jmax (0, x1 - x0), kLaneH - 1 - 4);
}

int ArrangePanel::laneAt (int y, const Geom& G) const
{
    if (y < G.lanes.getY())
        return -1;
    const int L = (y - G.lanes.getY()) / kLaneH;
    if (L >= kNumLanes || G.lanes.getY() + (L + 1) * kLaneH > G.lanes.getBottom())
        return -1;
    return L;
}

float ArrangePanel::barAt (int x, const Geom& G) const
{
    return (float) viewOffset
         + (float) (x - G.laneX()) * (float) kBars / (float) G.laneW();
}

int ArrangePanel::clipAt (juce::Point<int> p, const Geom& G) const
{
    if (p.x < G.laneX())
        return -1;
    const int lane = laneAt (p.y, G);
    if (lane < 0)
        return -1;
    const float bf = barAt (p.x, G);
    for (int i = (int) clips.size(); --i >= 0;)          // topmost drawn last
    {
        const ArrClip& c = clips[(size_t) i];
        if (c.lane == lane
            && bf >= (float) c.start_bar
            && bf <  (float) (c.start_bar + c.len_bars))
            return i;
    }
    return -1;
}

/* ======================================================================== */
/*  Edit model -> engine                                                     */
/* ======================================================================== */

void ArrangePanel::publish()
{
    bb_engine_song_publish (clips.empty() ? nullptr : clips.data(),
                            (int) clips.size());
}

void ArrangePanel::addClip (int lane, unsigned startBar, unsigned lenBars,
                            ArrClipBuf* audio, const juce::String& name,
                            const juce::String& path)
{
    if ((int) clips.size() >= ARR_MAX_CLIPS)
    {
        if (audio != nullptr)
            bb_engine_clip_release (audio);
        return;
    }

    ArrClip c;
    std::memset (&c, 0, sizeof (c));
    c.lane      = juce::jlimit (0, kNumLanes - 1, lane);
    c.start_bar = (unsigned) juce::jlimit (0, kSongBars - 1, (int) startBar);
    c.len_bars  = (unsigned) juce::jlimit (1, kSongBars, (int) lenBars);
    c.loop      = 0;
    c.gain      = 256;
    c.audio     = audio;
    std::snprintf (c.name, sizeof (c.name), "%s", name.toRawUTF8());
    std::snprintf (c.path, sizeof (c.path), "%s", path.toRawUTF8());

    clips.push_back (c);
    selected = (int) clips.size() - 1;
    publish();
    refreshToolbarState();
    repaint();
}

void ArrangePanel::deleteClip (int index)
{
    if (index < 0 || index >= (int) clips.size())
        return;
    ArrClipBuf* audio = clips[(size_t) index].audio;
    clips.erase (clips.begin() + index);
    if (selected == index)     selected = -1;
    else if (selected > index) --selected;
    if (dragClip == index)     { dragKind = DragNone; dragClip = -1; }
    else if (dragClip > index) --dragClip;

    publish();                                  // engine lets go of the clip
    if (audio != nullptr)
        bb_engine_clip_release (audio);         // retire only after publish
    refreshToolbarState();
    repaint();
}

/* ======================================================================== */
/*  Session rehydration                                                      */
/* ======================================================================== */

void ArrangePanel::rehydrateFromSession()
{
    std::vector<ArrClip> old = std::move (clips);
    clips.clear();

    ArrClip meta[ARR_MAX_CLIPS];
    const int n = bb_engine_song_get (meta, ARR_MAX_CLIPS);
    clips.assign (meta, meta + juce::jlimit (0, ARR_MAX_CLIPS, n));

    for (ArrClip& c : clips)
    {
        if (c.audio != nullptr || c.path[0] == '\0')
            continue;                            // nothing to decode
        juce::File f (juce::String::fromUTF8 (c.path));
        std::vector<int16_t> mono;
        int rate = 0;
        if (f.existsAsFile() && decodeMonoFile (f, mono, rate) && ! mono.empty())
            c.audio = bb_engine_clip_create (mono.data(), (unsigned) mono.size(), rate);
        /* decode failure: the clip stays a silent ghost, name intact */
    }

    publish();

    /* release old buffers the new model no longer references */
    for (const ArrClip& o : old)
    {
        if (o.audio == nullptr)
            continue;
        bool live = false;
        for (const ArrClip& c : clips)
            if (c.audio == o.audio) { live = true; break; }
        if (! live)
            bb_engine_clip_release (o.audio);
    }

    selected = -1;
    dragKind = DragNone;
    dragClip = -1;
    refreshToolbarState();
    repaint();
}

/* ======================================================================== */
/*  Capture                                                                  */
/* ======================================================================== */

juce::File ArrangePanel::morgueDir() const
{
    /* One answer for the whole console, from the engine (see Session.h). The
     * captures have to land in the directory the LOCKER scans and the title
     * bar prints, and rebuilding "$HOME/MORGUE" here was only ever the same
     * directory by coincidence. */
    return morgue::morgueDir();
}

juce::File ArrangePanel::nextCaptureFile (int lane) const
{
    const juce::File dir = morgueDir();
    const juce::String label = laneLabel (lane);
    for (int i = 1; i <= 9999; ++i)
    {
        char num[8];
        std::snprintf (num, sizeof (num), "%04d", i);
        juce::File f = dir.getChildFile ("CLIP-" + label + "-"
                                         + juce::String (num) + ".wav");
        if (! f.existsAsFile())
            return f;
    }
    return dir.getChildFile ("CLIP-" + label + "-OVER.wav");
}

bool ArrangePanel::writeWav16 (const juce::File& f, const int16_t* data,
                               unsigned n, int rate)
{
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    if (os == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (
        wav.createWriterFor (os,                 // writer takes the stream
                             juce::AudioFormatWriterOptions{}
                                 .withSampleRate ((double) rate)
                                 .withNumChannels (1)
                                 .withBitsPerSample (16)));
    if (w == nullptr)
        return false;

    juce::AudioBuffer<float> fb (1, (int) n);
    for (unsigned i = 0; i < n; ++i)
        fb.setSample (0, (int) i, (float) data[i] / 32768.0f);
    return w->writeFromAudioSampleBuffer (fb, 0, (int) n);
}

void ArrangePanel::startOrCancelCapture()
{
    const int st = atomic_load (&bb.arr_rec_status);
    if (recActive || st != ARR_REC_IDLE)
    {
        bb_engine_arr_cancel();
        recActive = false;
        recLane = -1;
        refreshToolbarState();
        repaint();
        return;
    }

    if (armedLane < 0 || armedLane >= kNumLanes - 1)   // needs an armed lane; MASS refused
        return;
    if ((int) clips.size() >= ARR_MAX_CLIPS)
        return;

    const unsigned bl = barLenFrames();
    const unsigned cap = (unsigned) barsChoice * bl;
    if (cap == 0)
        return;

    recBuf.reset (new int16_t[cap]());                 // message-thread malloc
    recCap = cap;
    if (bb_engine_arr_arm (armedLane, barsChoice, recBuf.get(), recCap) == 0)
    {
        recActive   = true;
        recLane     = armedLane;
        recBarsReq  = barsChoice;
        recRate     = atomic_load (&bb.rate);
        recStartBar = atomic_load (&bb.bar) + 1;       // provisional; fixed on
                                                       // the RECORDING edge
        refreshToolbarState();
        repaint();
    }
}

void ArrangePanel::finishCapture()
{
    recActive = false;
    const unsigned frames = juce::jmin (atomic_load (&bb.arr_rec_frames), recCap);
    bb_engine_arr_cancel();                            // consume DONE -> IDLE
    const int lane = recLane;
    recLane = -1;

    if (frames == 0 || lane < 0 || recBuf == nullptr)
        return;

    const juce::File dir = morgueDir();
    dir.createDirectory();
    const juce::File f = nextCaptureFile (lane);
    const int rate = recRate > 0 ? recRate : atomic_load (&bb.rate);
    if (! writeWav16 (f, recBuf.get(), frames, rate))
        return;

    ArrClipBuf* buf = bb_engine_clip_create (recBuf.get(), frames, rate);
    if (buf == nullptr)
        return;

    addClip (lane, recStartBar,
             (unsigned) juce::jmax (1, recBarsReq),
             buf, f.getFileNameWithoutExtension(), f.getFullPathName());

    if (onLockerRefresh)
        onLockerRefresh();
}

/* ======================================================================== */
/*  PLACE (LOCKER -> lane)                                                   */
/* ======================================================================== */

bool ArrangePanel::decodeMonoFile (const juce::File& f,
                                   std::vector<int16_t>& out, int& rateOut)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0
        || r->lengthInSamples > 0x7fffffff / 2)
        return false;

    const int len = (int) r->lengthInSamples;
    juce::AudioBuffer<float> tmp (2, len);
    tmp.clear();
    r->read (&tmp, 0, len, 0, true, true);

    out.resize ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        const float m = r->numChannels > 1
                ? (tmp.getSample (0, i) + tmp.getSample (1, i)) * 0.5f
                : tmp.getSample (0, i);
        const int32_t v = (int32_t) (m * 32767.0f);
        out[(size_t) i] = (int16_t) juce::jlimit (-32768, 32767, v);
    }
    rateOut = (int) r->sampleRate;
    return true;
}

void ArrangePanel::placeLockerFile()
{
    if (! getLockerSelection)
        return;
    const juce::File f = getLockerSelection();
    if (! f.existsAsFile())
        return;
    if ((int) clips.size() >= ARR_MAX_CLIPS)
        return;

    std::vector<int16_t> mono;
    int rate = 0;
    if (! decodeMonoFile (f, mono, rate) || mono.empty())
        return;

    ArrClipBuf* buf = bb_engine_clip_create (mono.data(), (unsigned) mono.size(), rate);
    if (buf == nullptr)
        return;

    /* the clip plays 1:1 at the device rate from its window start; the
     * window just has to be long enough at the CURRENT tempo */
    const unsigned bl = barLenFrames();
    const unsigned len = (unsigned) juce::jlimit (1, kSongBars,
        (int) ((mono.size() + bl - 1) / bl));
    const int startBar = juce::jlimit (0, kSongBars - 1,
                                       (int) atomic_load (&bb.bar));

    addClip (focusedLane, (unsigned) startBar, len, buf,
             f.getFileNameWithoutExtension(), f.getFullPathName());
}

/* ======================================================================== */
/*  sync -- 30 Hz engine pull                                                */
/* ======================================================================== */

void ArrangePanel::refreshToolbarState()
{
    selectBtn.setToggleStateQuiet (mode == ModeSelect);
    trimBtn  .setToggleStateQuiet (mode == ModeTrim);
    armBtn   .setToggleStateQuiet (armedLane >= 0 && armedLane == focusedLane);
    loopBtn  .setToggleStateQuiet (selected >= 0 && selected < (int) clips.size()
                                   && clips[(size_t) selected].loop != 0);
    loopBtn  .setEnabled (selected >= 0);
    captureBtn.setToggleStateQuiet (recActive);
    barsBtn.setButtonText (juce::String (barsChoice)
                           + (barsChoice == 1 ? " BAR" : " BARS"));
}

void ArrangePanel::sync()
{
    const int st = atomic_load (&bb.arr_rec_status);
    if (recActive)
    {
        if (st == ARR_REC_RECORDING && lastRecStatus != ARR_REC_RECORDING)
            recStartBar = atomic_load (&bb.bar);       // capture began this bar
        if (st == ARR_REC_DONE)
            finishCapture();
        else if (st == ARR_REC_IDLE && lastRecStatus != ARR_REC_IDLE)
        {
            recActive = false;                          // cancelled elsewhere
            recLane = -1;
        }
    }
    lastRecStatus = st;

    /* page the 32-bar view to keep the playhead visible -- a jump, never an
     * animation. Held still while the user is dragging a clip. */
    if (dragKind == DragNone)
    {
        const float ph = playheadBarF();
        if (ph >= 0.0f)
            viewOffset = juce::jlimit (0, kSongBars - kBars,
                                       ((int) ph / kBars) * kBars);
    }

    refreshToolbarState();
    repaint();
}

/* ======================================================================== */
/*  Mouse                                                                    */
/* ======================================================================== */

void ArrangePanel::mouseDown (const juce::MouseEvent& e)
{
    const Geom G = geom();
    const juce::Point<int> p = e.getPosition();

    /* ruler click = seek */
    if (G.ruler.contains (p) && p.x >= G.laneX())
    {
        bb_engine_song_seek (juce::jlimit (0, kSongBars - 1, (int) barAt (p.x, G)));
        return;
    }

    if (! G.lanes.contains (p))
        return;

    const int lane = laneAt (p.y, G);

    /* lane head click = focus */
    if (p.x < G.laneX())
    {
        if (lane >= 0)
        {
            focusedLane = lane;
            refreshToolbarState();
            repaint();
        }
        return;
    }

    const int ci = clipAt (p, G);

    if (e.mods.isPopupMenu())                          // right-click deletes
    {
        if (ci >= 0)
            deleteClip (ci);
        return;
    }

    selected = ci;
    dragKind = DragNone;
    dragClip = -1;

    if (ci >= 0)
    {
        const ArrClip& c = clips[(size_t) ci];
        focusedLane = c.lane;
        dragClip = ci;
        dragOrigStart = c.start_bar;
        dragOrigLen   = c.len_bars;

        if (mode == ModeSelect)
        {
            dragKind = DragMove;
            dragGrabOff = juce::jmax (0, (int) barAt (p.x, G) - (int) c.start_bar);
        }
        else                                            // TRIM: 6px edge zones
        {
            const Rectangle<int> r = clipRect (c, G);
            if      (p.x <= r.getX() + kTrimZone)       dragKind = DragTrimL;
            else if (p.x >= r.getRight() - kTrimZone)   dragKind = DragTrimR;
        }
    }

    refreshToolbarState();
    repaint();
}

void ArrangePanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragKind == DragNone || dragClip < 0 || dragClip >= (int) clips.size())
        return;

    const Geom G = geom();
    ArrClip& c = clips[(size_t) dragClip];
    const int barU = juce::jlimit (0, kSongBars - 1,
                                   (int) std::floor (barAt (e.x, G)));
    bool changed = false;

    if (dragKind == DragMove)
    {
        const unsigned ns = (unsigned) juce::jlimit (0, kSongBars - 1,
                                                     barU - dragGrabOff);
        const int nl = laneAt (e.y, G);
        if (ns != c.start_bar) { c.start_bar = ns; changed = true; }
        if (nl >= 0 && nl != c.lane) { c.lane = nl; changed = true; }
    }
    else if (dragKind == DragTrimL)
    {
        const unsigned end = dragOrigStart + dragOrigLen;
        const unsigned ns = (unsigned) juce::jlimit (0, (int) end - 1, barU);
        if (ns != c.start_bar)
        {
            c.start_bar = ns;
            c.len_bars  = end - ns;
            changed = true;
        }
    }
    else                                                // DragTrimR
    {
        const unsigned nl = (unsigned) juce::jlimit (1, kSongBars,
                                                     barU - (int) c.start_bar + 1);
        if (nl != c.len_bars) { c.len_bars = nl; changed = true; }
    }

    if (changed)
    {
        publish();
        repaint();
    }
}

void ArrangePanel::mouseUp (const juce::MouseEvent&)
{
    dragKind = DragNone;
    dragClip = -1;
}

void ArrangePanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    const Geom G = geom();
    const int ci = clipAt (e.getPosition(), G);
    if (ci < 0)
        return;
    clips[(size_t) ci].loop = clips[(size_t) ci].loop ? 0 : 1;
    selected = ci;
    publish();
    refreshToolbarState();
    repaint();
}

/* ======================================================================== */
/*  Painting                                                                 */
/* ======================================================================== */

void ArrangePanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();
    g.setColour (C::PANEL);
    g.fillRect (b);

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "ARRANGE",
                     U8 ("MORGUE PLAYLIST \xc2\xb7 64 BARS"),
                     U8 ("SNAP 1 BAR \xc2\xb7 ZOOM 2 BAR/IN"),
                     Badge::PARTIAL, "PARTIAL");

    paintToolbar (g, b.removeFromTop (kToolbarH));

    Rectangle<int> ruler = b.removeFromTop (kRulerH);
    Rectangle<int> autoLane = b.removeFromBottom (kAutoH);
    Rectangle<int> lanes = b;

    paintRuler (g, ruler);
    paintLanes (g, lanes);
    paintAutomation (g, autoLane);

    // the engine clock playhead: 1px BLOOD_HOT rule spanning ruler + lanes
    // (paintAutomation continues it through the automation plot)
    const float ph = playheadBarF();
    if (ph >= (float) viewOffset && ph < (float) (viewOffset + kBars))
    {
        const float frac = (ph - (float) viewOffset) / (float) kBars;
        const int laneX = ruler.getX() + kGutterW;
        const int laneW = juce::jmax (1, ruler.getWidth() - kGutterW);
        const int px = laneX + juce::jmin (laneW - 1, (int) (frac * (float) laneW));
        const int bottom = juce::jmin (lanes.getBottom(),
                                       lanes.getY() + kNumLanes * kLaneH);
        g.setColour (C::BLOOD_HOT);
        g.fillRect (px, ruler.getY(), 1, bottom - ruler.getY());
    }
}

void ArrangePanel::paintToolbar (juce::Graphics& g, Rectangle<int> bar)
{
    g.setColour (C::PANEL_ALT);
    g.fillRect (bar);
    g.setColour (C::HAIRLINE);
    g.fillRect (bar.getX(), bar.getBottom() - 1, bar.getWidth(), 1);

    const ToolbarSlots s = toolbarSlots (bar);

    /* planned chrome: DRAW / SLIP / CONSOLIDATE stay drawn idle (R3);
     * the live plates are child PlateButtons on the same grid. */
    const juce::Font chipFont = Type::mono (9.0f, 0.14f);
    auto plannedChip = [&] (Rectangle<int> r, const char* text)
    {
        g.setColour (C::PLATE_LOW);
        g.fillRect (r);
        g.setColour (C::HAIRLINE);
        g.drawRect (r, 1);
        g.setColour (C::TAB_INACTIVE_FG);
        g.setFont (chipFont);
        g.drawText (text, r, Justification::centred);
    };
    plannedChip (s.draw, "DRAW");
    plannedChip (s.slip, "SLIP");
    plannedChip (s.consolidate, "CONSOLIDATE");

    g.setColour (C::HAIRLINE);
    g.fillRect (s.divider);

    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.14f));
    g.drawText (U8 ("MULTITRACK REC ROUTES PER-VOICE OUT \xe2\x86\x92 LANE"),
                bar.withTrimmedRight (8)
                   .withTrimmedLeft (s.rightOfChips + 6 - bar.getX()),
                Justification::centredRight, false);
}

void ArrangePanel::paintRuler (juce::Graphics& g, Rectangle<int> ruler)
{
    g.setColour (C::PANEL);
    g.fillRect (ruler);
    g.setColour (C::HAIRLINE);
    g.fillRect (ruler.getX(), ruler.getBottom() - 1, ruler.getWidth(), 1);
    g.fillRect (ruler.getX() + kGutterW - 1, ruler.getY(), 1, ruler.getHeight() - 1);

    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f, 0.12f));
    g.drawText ("BAR", ruler.withWidth (kGutterW).reduced (8, 0),
                Justification::centredLeft);

    const int laneX = ruler.getX() + kGutterW;
    const int laneW = juce::jmax (1, ruler.getWidth() - kGutterW);
    for (int i = 0; i < kBars; ++i)
    {
        // absolute bar number every 4th (the view pages through 64 bars)
        if (i % 4 == 0)
        {
            const int x0 = laneX + i * laneW / kBars;
            g.setColour (C::INK_FAINT);
            g.setFont (Type::mono (8.0f));
            g.drawText (juce::String (viewOffset + i + 1),
                        Rectangle<int> (x0 + 3, ruler.getY(), 24, ruler.getHeight() - 1),
                        Justification::centredLeft, false);
        }
        // column rule at the right edge; heavier every 4th
        const int xr = laneX + (i + 1) * laneW / kBars - 1;
        g.setColour ((i + 1) % 4 == 0 ? C::LAMP_DEAD : C::HAIRLINE_DIM);
        g.fillRect (xr, ruler.getY(), 1, ruler.getHeight() - 1);
    }
}

void ArrangePanel::paintLanes (juce::Graphics& g, Rectangle<int> area)
{
    const Geom G = geom();

    bool licksOn = false;
    for (int s = 0; s < BB_SAMPLER; ++s)
        if (atomic_load_explicit (&bb.sampler[s].on, memory_order_relaxed) != 0
            && atomic_load_explicit (&bb.sampler[s].mute, memory_order_relaxed) == 0)
            { licksOn = true; break; }

    for (int L = 0; L < kNumLanes; ++L)
    {
        Rectangle<int> lane (area.getX(), area.getY() + L * kLaneH,
                             area.getWidth(), kLaneH);
        if (lane.getBottom() > area.getBottom())
            break;

        g.setColour (C::HAIRLINE_DIM);
        g.fillRect (lane.getX(), lane.getBottom() - 1, lane.getWidth(), 1);

        // lane head, 120 wide: 5px arm lamp, name, kind tag
        Rectangle<int> head = lane.withWidth (kGutterW).withTrimmedBottom (1);
        g.setColour (C::HAIRLINE);
        g.fillRect (head.getRight() - 1, lane.getY(), 1, lane.getHeight());

        juce::String name = laneLabel (L), kindTag;
        bool on = false;
        if (L < BB_NLAYER)
        {
            kindTag = "VOICE";
            on = atomic_load_explicit (&bb.layer[L].on, memory_order_relaxed) != 0;
        }
        else if (L == BB_NLAYER)
        {
            kindTag = "STEP";
            on = licksOn;
        }
        else
        {
            kindTag = "SMPL";
            on = false;                    // GUI-side sampler; no engine state
        }

        const bool armed = (L == armedLane);
        g.setColour (armed ? C::BLOOD_HOT
                           : on ? C::LAMP_SOUNDING : C::LAMP_DEAD);
        g.fillRect (head.getX() + 6, head.getCentreY() - 2, 5, 5);
        g.setColour (L == focusedLane ? C::INK
                                      : on ? C::INK_DIM : C::INK_FAINT);
        g.setFont (Type::mono (9.0f, 0.08f));
        g.drawText (name, head.withTrimmedLeft (17), Justification::centredLeft, false);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::nano (7.0f));
        g.drawText (kindTag, head.withTrimmedRight (6), Justification::centredRight, false);
    }

    /* ---- clips (edit model; drawn in model order, later on top) --------- */
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (Rectangle<int> (G.laneX(), area.getY(),
                                        juce::jmax (0, area.getRight() - G.laneX()),
                                        juce::jmin (area.getHeight(),
                                                    kNumLanes * kLaneH)));

    for (int i = 0; i < (int) clips.size(); ++i)
    {
        const ArrClip& c = clips[(size_t) i];
        if ((int) c.start_bar >= viewOffset + kBars
            || (int) (c.start_bar + c.len_bars) <= viewOffset
            || c.lane < 0 || c.lane >= kNumLanes)
            continue;

        const Rectangle<int> r = clipRect (c, G);
        if (r.getWidth() < 1 || r.getBottom() > area.getBottom())
            continue;

        const juce::String title = juce::String::fromUTF8 (c.name);
        if (c.audio == nullptr)
        {
            /* silent ghost: source file missing -- INK_GHOST body, name intact */
            g.setColour (C::DISABLED_BG);
            g.fillRect (r);
            g.setColour (C::INK_GHOST);
            g.drawRect (r, 1);
            g.setFont (Type::nano (7.0f));
            g.drawText (title, r.reduced (4, 0), Justification::centredLeft, false);
        }
        else
        {
            const ClipComponent::Kind kind = title.startsWith ("CLIP-")
                                                 ? ClipComponent::RECORDED
                                                 : ClipComponent::AUDIO;
            ClipComponent::paintClip (g, r, kind, title);
            if (i == selected)
            {
                g.setColour (kind == ClipComponent::RECORDED ? C::BLOOD_HOT
                                                             : C::BLOOD);
                g.drawRect (r, 1);
            }
        }
        if (c.audio == nullptr && i == selected)
        {
            g.setColour (C::BLOOD);
            g.drawRect (r, 1);
        }
    }

    /* ---- capture in flight: the growing clip outline, in blood ---------- */
    if (recActive && lastRecStatus == ARR_REC_RECORDING
        && recLane >= 0 && recLane < kNumLanes)
    {
        const unsigned bl = barLenFrames();
        const float bars = (float) atomic_load (&bb.arr_rec_frames)
                         / (float) (bl > 0 ? bl : 1);
        const float relS = (float) ((int) recStartBar - viewOffset);
        const float relE = relS + juce::jmax (0.05f, bars);
        const int laneX = G.laneX(), laneW = G.laneW();
        const int x0 = laneX + (int) (juce::jlimit (0.0f, (float) kBars, relS)
                                      * (float) laneW / (float) kBars);
        const int x1 = laneX + (int) (juce::jlimit (0.0f, (float) kBars, relE)
                                      * (float) laneW / (float) kBars);
        if (x1 > x0)
        {
            Rectangle<int> r (x0, area.getY() + recLane * kLaneH + 2,
                              x1 - x0, kLaneH - 1 - 4);
            g.setColour (C::BLOOD_DEEP);
            g.fillRect (r);
            g.setColour (C::BLOOD);
            g.drawRect (r, 1);
        }
    }

    // ground below the ten lanes
    const int lanesBottom = area.getY() + kNumLanes * kLaneH;
    if (lanesBottom < area.getBottom())
    {
        g.setColour (C::GROUND);
        g.fillRect (area.getX(), lanesBottom,
                    area.getWidth(), area.getBottom() - lanesBottom);
    }
}

void ArrangePanel::paintAutomation (juce::Graphics& g, Rectangle<int> a)
{
    g.setColour (C::PANEL);
    g.fillRect (a);
    g.setColour (C::LAMP_DEAD);                       // heavier separating rule
    g.fillRect (a.getX(), a.getY(), a.getWidth(), 1);
    a.removeFromTop (1);

    /* ---- real engine state: the focused voice's lock-lane motion ------- */
    const int F = juce::jlimit (0, BB_NLAYER - 1, atomic_load (&bb.focus));
    Layer* l = &bb.layer[F];

    int target = -1;
    for (int t = 0; t < BB_LOCK_COUNT && target < 0; ++t)
        for (int s = 0; s < BB_STEPS; ++s)
            if (atomic_load_explicit (&l->seq_lock[t][s], memory_order_relaxed) >= 0)
                { target = t; break; }

    juce::String paramLabel = U8 ("\xe2\x80\x94 EMPTY \xe2\x80\x94");
    juce::String modeLabel  = U8 ("MODE: \xe2\x80\x94");
    bool smooth = false;
    if (target >= 0)
    {
        if (target < BB_NPARAM)
        {
            paramLabel = "p" + juce::String (target);
            Program* pr = atomic_load (&l->prog);
            if (pr != nullptr && (pr->used_p & (1u << (unsigned) target)) != 0)
                paramLabel << " " << expr_role_name (pr->role[target]);
        }
        else
            paramLabel = kCtlName[target - BB_NPARAM];

        smooth = ((atomic_load (&l->motion_mask) >> (unsigned) target) & 1u) != 0;
        modeLabel = smooth ? "MODE: SMOOTH" : "MODE: STEP";
    }

    /* ---- header row: voice + parameter + mode + ARM CAPTURE ------------ */
    Rectangle<int> head = a.removeFromTop (kAutoHeadH - 1);
    g.setColour (C::HAIRLINE);
    g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

    const juce::Font f8 = Type::mono (8.0f, 0.14f);
    int x = head.getX() + 8;
    auto seg = [&] (const juce::String& text, juce::Colour fg)
    {
        g.setColour (fg);
        g.setFont (f8);
        g.drawText (text, x, head.getY(), textW (f8, text) + 2,
                    head.getHeight() - 1, Justification::centredLeft, false);
        x += textW (f8, text) + 10;
    };

    seg (U8 ("MOTION \xc2\xb7 AUTOMATION LANE"), C::INK_DIM);
    seg ("V" + juce::String (F + 1).paddedLeft ('0', 2)
             + U8 (" \xc2\xb7 ") + paramLabel, C::INK);
    seg (modeLabel, C::INK_FAINT);

    {   // ARM CAPTURE tag (R3 chrome, drawn as the mockup shows)
        const juce::String tag ("ARM CAPTURE");
        const int w = textW (f8, tag) + 10;
        Rectangle<int> r (x, head.getY() + (head.getHeight() - 1 - 12) / 2, w, 12);
        g.setColour (C::BLOOD);
        g.drawRect (r, 1);
        g.setColour (C::BLOOD_HOT);
        g.setFont (f8);
        g.drawText (tag, r, Justification::centred, false);
        x += w + 10;
    }

    g.setColour (C::INK_FAINT);
    g.setFont (f8);
    g.drawText (U8 ("R3 \xc2\xb7 STEP | SMOOTH \xc2\xb7 CLICK-DRAG TO DRAW"),
                head.withTrimmedRight (8).withTrimmedLeft (x - head.getX()),
                Justification::centredRight, false);

    /* ---- body: 120 gutter with 255/128/000 scale, then the plot -------- */
    Rectangle<int> gutter = a.removeFromLeft (kGutterW);
    g.setColour (C::HAIRLINE);
    g.fillRect (gutter.getRight() - 1, gutter.getY(), 1, gutter.getHeight());
    g.setColour (C::INK_FAINT);
    g.setFont (Type::mono (8.0f));
    Rectangle<int> gin = gutter.withTrimmedRight (1).reduced (6, 4);
    g.drawText ("255", gin, Justification::topLeft,    false);
    g.drawText ("128", gin, Justification::centredLeft, false);
    g.drawText ("000", gin, Justification::bottomLeft, false);

    Rectangle<int> plot = a;
    g.setColour (C::HAIRLINE_DIM);
    g.fillRect (plot.getX(), plot.getY() + plot.getHeight() / 2, plot.getWidth(), 1);

    if (target >= 0 && plot.getWidth() > 1 && plot.getHeight() > 9)
    {
        // one 16-step motion cycle per bar, tiled across the 32-bar window
        const int n = juce::jlimit (1, BB_STEPS, atomic_load (&l->ctl[LCTL_STEPS]));
        float lo = 0.0f, hi = 255.0f;
        if (target >= BB_NPARAM)
        {
            const CtlInfo& ci = bb_lctl_info[kCtlIdx[target - BB_NPARAM]];
            lo = (float) ci.lo;
            hi = (float) ci.hi;
        }
        float sv[BB_STEPS];
        for (int s = 0; s < n; ++s)
        {
            int v = atomic_load_explicit (&l->seq_lock[target][s], memory_order_relaxed);
            if (v < 0)
                v = (target < BB_NPARAM)
                        ? atomic_load (&l->param[target])
                        : atomic_load (&l->ctl[kCtlIdx[target - BB_NPARAM]]);
            sv[s] = juce::jlimit (0.0f, 1.0f, ((float) v - lo) / juce::jmax (1.0f, hi - lo));
        }

        const float px0 = (float) plot.getX();
        const float pw  = (float) plot.getWidth();
        const float py0 = (float) plot.getY() + 4.0f;
        const float ph  = (float) plot.getHeight() - 8.0f;
        const int total = kBars * n;
        auto X = [&] (int gs) { return px0 + pw * (float) gs / (float) total; };
        auto Y = [&] (float v) { return py0 + (1.0f - v) * ph; };

        juce::Path curve;
        if (smooth)
        {
            for (int gs = 0; gs <= total; ++gs)
            {
                const float y = Y (sv[gs % n]);
                if (gs == 0) curve.startNewSubPath (X (gs), y);
                else         curve.lineTo (X (gs), y);
            }
        }
        else
        {
            for (int gs = 0; gs < total; ++gs)
            {
                const float y = Y (sv[gs % n]);
                if (gs == 0) curve.startNewSubPath (X (gs), y);
                else         curve.lineTo (X (gs), y);
                curve.lineTo (X (gs + 1), y);
            }
        }
        g.setColour (C::OXIDE);
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    }

    // playhead continues through the automation plot
    const float phb = playheadBarF();
    if (phb >= (float) viewOffset && phb < (float) (viewOffset + kBars))
    {
        const float frac = (phb - (float) viewOffset) / (float) kBars;
        const int px = plot.getX()
                     + juce::jmin (plot.getWidth() - 1,
                                   (int) (frac * (float) plot.getWidth()));
        g.setColour (C::BLOOD_HOT);
        g.fillRect (px, plot.getY(), 1, plot.getHeight());
    }
}

} // namespace morgue
