/* ArrangePanel.cpp -- see ArrangePanel.h. Spec section 6 geometry:
 * toolbar 30, ruler 22 (120 gutter + 32 bar columns), ten 42px lanes,
 * automation lane 96. The timeline is LIVE -- the panel owns the clip edit
 * model and republishes it to the engine on every edit; the engine's
 * published song is what plays. Playhead, lane lamps and the automation
 * plot are real engine state only. Every control on the toolbar reaches
 * the engine; nothing here is painted chrome. */

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
        case RECORDED: bg = C::BLOOD_DEEP; bd = C::BLOOD;     fg = C::ARMED_TEXT; break;
        case PATTERN:  bg = C::CONTROL;    bd = C::OXIDE_DIM; fg = C::OXIDE_INK; break;
        default:       bg = C::PLATE;      bd = C::EDGE;      fg = C::INK_DIM;   break;
    }

    g.setColour (bg);
    g.fillRect (area);
    g.setColour (bd);
    g.drawRect (area, 1);

    Rectangle<int> inner = area.reduced (1);

    /* Filled title bar across the top. It is 13 tall, not 8: the title is
     * set at the 8px floor and Type::rowH(8) = 13, and a glyph in a box
     * shorter than that is clipped rather than merely tight.
     * The title is INK_BRIGHT in every colourway, because the bar is filled
     * with the BORDER colour -- the old fg-on-bd pairings ran as low as
     * 2.2:1 (INK_DIM on EDGE). INK_BRIGHT clears 5:1 on all three. */
    Rectangle<int> bar = inner.removeFromTop (juce::jmin (inner.getHeight(),
                                                          Type::rowH (Type::kMinSize)));
    g.setColour (bd);
    g.fillRect (bar);
    g.setColour (C::INK_BRIGHT);
    g.setFont (Type::nano());
    /* names run to ARR_NAME_MAX (48) and a clip can be one bar wide, so
     * ellipsise rather than cutting a word in half without saying so */
    g.drawText (title, bar.withTrimmedLeft (3).withTrimmedRight (2),
                Justification::centredLeft, true);

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

    for (PlateButton* b : { &songBtn, &selectBtn, &trimBtn, &armBtn, &captureBtn,
                            &barsBtn, &placeBtn, &loopBtn, &recSrcBtn })
        addAndMakeVisible (*b);

    songBtn.setTooltip (U8 ("PLAY SONG / STOP SONG - the timeline's own transport, "
                            "separate from the master RUN. Stopping is a MUTE, not a "
                            "pause: the clips keep tracking the bar grid while "
                            "stopped, so PLAY drops in wherever the song has got to "
                            "rather than resuming from where you stopped it. Click "
                            "the ruler to move the song to a bar."));
    recSrcBtn.setTooltip (U8 ("WHAT REC PRINTS. WHOLE MIX prints everything you "
                              "hear, arrangement included. OVERDUB prints everything "
                              "EXCEPT the arranged clips - loop a section, play over "
                              "it, and only the new layer is captured instead of the "
                              "backing being printed again on every pass. Also "
                              "applies to the network sink."));

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

    songBtn.onToggle = [this] (bool on)
    {
        bb_engine_song_play (on ? 1 : 0);
        refreshToolbarState();
        repaint();
    };

    recSrcBtn.onToggle = [this] (bool on)
    {
        bb_engine_rec_src (on ? BB_REC_LIVE : BB_REC_MASTER);
        refreshToolbarState();
    };

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
    /* Reading bb.bar and bb.seq_pos as two independent loads tears at every
     * bar boundary and makes the playhead appear to jump back a bar on each
     * loop pass. See morgue::transportPositionBars(). */
    return transportPositionBars();
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

/* Three groups, in the order you use them: the song's transport, the edit
 * tools, then capture. Every slot is sized at its WIDEST text so a plate
 * whose label changes with state (PLAY/STOP SONG, n BARS, REC source) does
 * not resize the row underneath the pointer. */
ArrangePanel::ToolbarSlots ArrangePanel::toolbarSlots (Rectangle<int> bar) const
{
    const juce::Font liveF = Type::mono (10.0f, 0.16f);   // PlateButton face
    const int chipH = 18;
    const int cy = bar.getY() + (bar.getHeight() - chipH) / 2;
    int x = bar.getX() + 8;

    ToolbarSlots s;
    /* pad 16; a lamp plate needs 7 + 5 + 5 on the left as well (see
     * PlateButton::paintButton), so it gets pad 30 */
    auto slot = [&] (const char* text, int pad)
    {
        const int w = textW (liveF, text) + pad;
        Rectangle<int> r (x, cy, w, chipH);
        x += w + 3;
        return r;
    };
    auto next     = [&] (const char* text) { return slot (text, 16); };
    auto nextLamp = [&] (const char* text) { return slot (text, 30); };
    auto divider = [&]
    {
        x += 3;                                          // group gap 6
        Rectangle<int> r (x, bar.getY() + (bar.getHeight() - 18) / 2, 1, 18);
        x += 1 + 6;
        return r;
    };

    s.song     = nextLamp ("STOP SONG");                 // widest of the pair
    s.dividerA = divider();

    s.select   = next ("SELECT");
    s.trim     = next ("TRIM");
    s.dividerB = divider();

    s.arm      = next ("ARM LANE");
    s.capture  = next ("CAPTURE");
    s.bars     = next ("8 BARS");
    s.loopClip = next ("LOOP CLIP");
    s.place    = next ("PLACE");
    s.dividerC = divider();

    s.recSrc   = nextLamp ("REC: WHOLE MIX");
    s.rightOfChips = x;
    return s;
}

void ArrangePanel::resized()
{
    const ToolbarSlots s = toolbarSlots (geom().toolbar);
    songBtn   .setBounds (s.song);
    selectBtn .setBounds (s.select);
    trimBtn   .setBounds (s.trim);
    armBtn    .setBounds (s.arm);
    captureBtn.setBounds (s.capture);
    barsBtn   .setBounds (s.bars);
    loopBtn   .setBounds (s.loopClip);
    placeBtn  .setBounds (s.place);
    recSrcBtn .setBounds (s.recSrc);
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
    /* Both of these are engine truth (and both survive a session reload),
     * so the plates are pulled from the engine, never from a local flag.
     * The WORD changes with the state as well as the plate colour -- state
     * carried by hue alone does not survive greyscale (Theme.h). */
    const bool songOn = bb_engine_song_playing() != 0;
    songBtn.setToggleStateQuiet (songOn);
    songBtn.setButtonText (songOn ? "STOP SONG" : "PLAY SONG");

    const bool overdub = bb_engine_rec_src_get() == BB_REC_LIVE;
    recSrcBtn.setToggleStateQuiet (overdub);
    recSrcBtn.setButtonText (overdub ? "REC: OVERDUB" : "REC: WHOLE MIX");

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

    /* The right slot used to advertise a zoom that does not exist. It now
     * prints the window the view is actually showing, which changes as the
     * playhead pages, so it is a readout rather than a claim. */
    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "ARRANGE",
                     U8 ("MORGUE PLAYLIST \xc2\xb7 64 BARS"),
                     juce::String ("SNAP 1 BAR")
                       + U8 (" \xc2\xb7 VIEW ")
                       + juce::String (viewOffset + 1).paddedLeft ('0', 2)
                       + U8 ("\xe2\x80\x93")
                       + juce::String (viewOffset + kBars).paddedLeft ('0', 2),
                     Badge::LIVE, "LIVE");

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

    /* group rules only -- every plate on this bar is a child PlateButton */
    g.setColour (C::HAIRLINE);
    g.fillRect (s.dividerA);
    g.fillRect (s.dividerB);
    g.fillRect (s.dividerC);

    /* the right slot carries a count of the model, not a promise */
    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    g.drawText (juce::String ((int) clips.size())
                    + (clips.size() == 1 ? " CLIP" : " CLIPS"),
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

    g.setColour (C::INK_DIM);
    g.setFont (Type::micro());
    g.drawText ("BAR", ruler.withWidth (kGutterW).reduced (8, 0),
                Justification::centredLeft);
    g.setColour (C::INK_FAINT);
    g.drawText (U8 ("CLICK \xe2\x86\x92 SEEK"),
                ruler.withWidth (kGutterW).reduced (8, 0),
                Justification::centredRight);

    const int laneX = ruler.getX() + kGutterW;
    const int laneW = juce::jmax (1, ruler.getWidth() - kGutterW);
    for (int i = 0; i < kBars; ++i)
    {
        // absolute bar number every 4th (the view pages through 64 bars)
        if (i % 4 == 0)
        {
            const int x0 = laneX + i * laneW / kBars;
            g.setColour (C::INK_DIM);              // this is the axis, not metadata
            g.setFont (Type::micro());
            g.drawText (juce::String (viewOffset + i + 1),
                        Rectangle<int> (x0 + 3, ruler.getY(), 26, ruler.getHeight() - 1),
                        Justification::centredLeft, false);
        }
        // column rule at the right edge; heavier every 4th
        const int xr = laneX + (i + 1) * laneW / kBars - 1;
        g.setColour ((i + 1) % 4 == 0 ? C::HAIRLINE : C::HAIRLINE_DIM);
        g.fillRect (xr, ruler.getY(), 1, ruler.getHeight() - 1);
    }

    /* A 1px rule is the right weight down the lanes but it is not enough to
     * FIND the playhead on a 32-bar ruler, so the ruler carries a 5px block
     * at the same x. Same colour, same meaning, findable at a glance. */
    const float ph = playheadBarF();
    if (ph >= (float) viewOffset && ph < (float) (viewOffset + kBars))
    {
        const float frac = (ph - (float) viewOffset) / (float) kBars;
        const int px = laneX + juce::jmin (laneW - 1, (int) (frac * (float) laneW));
        g.setColour (C::BLOOD_HOT);
        g.fillRect (px - 2, ruler.getY(), 5, 4);
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
        g.setFont (L == focusedLane ? Type::label() : Type::micro());
        g.drawText (name, head.withTrimmedLeft (17), Justification::centredLeft, false);
        g.setColour (C::INK_FAINT);
        g.setFont (Type::nano());
        g.drawText (kindTag, head.withTrimmedRight (6), Justification::centredRight, false);

        /* the focused lane is where PLACE and ARM LANE land, so it gets a
         * mark you can find without comparing two greys */
        if (L == focusedLane)
        {
            g.setColour (C::INK);
            g.fillRect (head.getX(), head.getY(), 2, head.getHeight());
        }
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
            /* Silent ghost: the source file did not come back. Drawn as a
             * recess (DISABLED_BG is below PANEL now) with the name still
             * readable, and SAID in words -- a clip that makes no sound is
             * not something to work out from a shade of grey. */
            g.setColour (C::DISABLED_BG);
            g.fillRect (r);
            g.setColour (C::HAIRLINE_DIM);
            g.drawRect (r, 1);
            g.setFont (Type::nano());
            g.setColour (C::INK_FAINT);
            g.drawText (title, r.reduced (4, 0), Justification::centredLeft, true);
            if (r.getWidth() > 90)
            {
                g.setColour (C::AMBER);
                g.drawText ("FILE MISSING", r.reduced (4, 0),
                            Justification::centredRight, false);
            }
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
    g.setColour (C::EDGE);                            // heavier separating rule
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
                paramLabel << " "
                           << juce::String (expr_role_name (pr->role[target])).toUpperCase();
        }
        else
            paramLabel = kCtlName[target - BB_NPARAM];

        smooth = ((atomic_load (&l->motion_mask) >> (unsigned) target) & 1u) != 0;
        modeLabel = smooth ? "MODE: SMOOTH" : "MODE: STEP";
    }

    /* ---- header row: voice + parameter + mode + where to edit it ------- */
    Rectangle<int> head = a.removeFromTop (kAutoHeadH - 1);
    g.setColour (C::HAIRLINE);
    g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

    const juce::Font f8 = Type::micro();
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

    /* This lane is a DISPLAY. It plots the focused voice's lock lane, which
     * is real engine state, but there is no edit gesture on it -- there was
     * an ARM CAPTURE tag here, painted in the reserved accent, that was not
     * hit-tested anywhere, and a "CLICK-DRAG TO DRAW" instruction for a
     * gesture with no handler. Both are gone; what replaces them is where
     * the edit actually lives. */
    g.setColour (C::INK_FAINT);
    g.setFont (f8);
    g.drawText (U8 ("READ-ONLY \xc2\xb7 SET LOCKS ON THE RACK LOCK LANE"),
                head.withTrimmedRight (8).withTrimmedLeft (x - head.getX()),
                Justification::centredRight, false);

    /* ---- body: 120 gutter carrying the target's own scale, then plot --- */
    Rectangle<int> gutter = a.removeFromLeft (kGutterW);
    g.setColour (C::HAIRLINE);
    g.fillRect (gutter.getRight() - 1, gutter.getY(), 1, gutter.getHeight());
    /* the scale is the TARGET's range, not a fixed 0-255: a ctl lock lane
     * runs over its own CtlInfo lo..hi and the axis has to say so */
    float lo = 0.0f, hi = 255.0f;
    if (target >= BB_NPARAM && target < BB_LOCK_COUNT)
    {
        const CtlInfo& ci = bb_lctl_info[kCtlIdx[target - BB_NPARAM]];
        lo = (float) ci.lo;
        hi = (float) ci.hi;
    }
    g.setColour (C::INK_FAINT);
    g.setFont (Type::micro());
    Rectangle<int> gin = gutter.withTrimmedRight (1).reduced (6, 4);
    auto axis = [] (float v) { return juce::String (juce::roundToInt (v)).paddedLeft ('0', 3); };
    g.drawText (axis (hi),               gin, Justification::topLeft,     false);
    g.drawText (axis ((lo + hi) * 0.5f), gin, Justification::centredLeft, false);
    g.drawText (axis (lo),               gin, Justification::bottomLeft,  false);

    Rectangle<int> plot = a;
    g.setColour (C::HAIRLINE_FAINT);
    g.fillRect (plot.getX(), plot.getY() + plot.getHeight() / 2, plot.getWidth(), 1);

    /* The pattern is one bar long and is repeated across all 32 visible
     * bars, so without bar rules the curve reads as a texture rather than a
     * shape. A rule every 4 bars, matching the ruler, gives the eye
     * something to segment it against. */
    for (int i = 4; i < kBars; i += 4)
    {
        const int bx = plot.getX() + i * plot.getWidth() / kBars;
        g.setColour (C::HAIRLINE_FAINT);
        g.fillRect (bx, plot.getY(), 1, plot.getHeight());
    }

    if (target >= 0 && plot.getWidth() > 1 && plot.getHeight() > 9)
    {
        // one 16-step motion cycle per bar, tiled across the 32-bar window
        const int n = juce::jlimit (1, BB_STEPS, atomic_load (&l->ctl[LCTL_STEPS]));
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
        /* 1px, not 1.5: the same shape is drawn 32 times across this lane,
         * and at 1.5px the repeats merge into a band. */
        g.setColour (C::OXIDE);
        g.strokePath (curve, juce::PathStrokeType (1.0f));
    }
    else if (target < 0)
    {
        /* nothing locked on this voice: say so where the curve would be,
         * rather than leaving an empty framed box that looks broken */
        g.setColour (C::INK_GHOST);
        g.setFont (Type::micro());
        g.drawText (U8 ("NO STEP LOCKS ON THIS VOICE"), plot,
                    Justification::centred, false);
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
