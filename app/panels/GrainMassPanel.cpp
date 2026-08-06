/* GrainMassPanel.cpp -- see GrainMassPanel.h.
 *
 * Spec section 8 + HTML frame "04 GRAIN MASS". Double-click, a file drop from
 * the desktop, or a drag out of the LOCKER loads through the engine's well
 * pool; PLAY/STOP/R/O plates and the PITCH and LEVEL knobs write straight to
 * bb.well[i]; keys 1-4/P/A/Z/R/O act on the selected well. The waveform is the
 * real decoded file (peaks computed once at load); nothing is faked.
 *
 * THE WELLS ARE IN THE ENGINE NOW. They used to be four JUCE SamplerVoice
 * objects that mixed into the device buffers after bb_engine_render() had
 * returned, which meant they reached the speakers and nothing else: REC did
 * not record them, SURVIVOR could not loop them, the master meter and the
 * scope could not see them, and ARRANGE's MASS lane refused to capture because
 * there was no engine-side bus to point at. This panel is now a control
 * surface over bb.well[], exactly as LicksPanel is over bb.sampler[]: the
 * decode happens here on the message thread, because the engine has no WAV
 * reader and must never see a path, and the audio happens there.
 *
 * LEGIBILITY PASS. Removed: the GRAIN and ERASE "knobs". They were a paint
 * lambda -- a CONTROL circle, a dead ring and a pointer frozen at -135
 * degrees -- called twice per well, so the panel carried eight knob faces
 * that were not components, had no hit test, and could never move. Their
 * caption ("GRAIN/ERASE: PLANNED", 7px OXIDE) sat 70px to the right of them.
 * Also removed: the footer's "VISION: SLICING - TAPE ERASER - CROSS-MOD WITH
 * VOICES", three features that do not exist drawn as a status line.
 *
 * Added instead: each well states its own state as a WORD (EMPTY / LOADED /
 * PLAYING) beside its lamp, an empty well says how to fill it in the middle
 * of the empty space, and the selected well carries a full EDGE frame -- it
 * is where every key press lands, so it needs the strongest cue here. */

#include "GrainMassPanel.h"
#include "AudioEngine.h"
#include "Session.h"
#include "bytebeat.h"
#include "engine.h"

#include <cmath>
#include <cstdlib>          // calloc -- bb_engine_well_set takes the buffer
#include <memory>
#include <vector>

namespace morgue
{

using juce::Rectangle;
using juce::Justification;

namespace
{
    /* The playhead. The engine publishes it as 0..65536 over the whole
     * sample, once per period whether or not the well is sounding.
     *
     * This replaces a compile-time detection idiom that tested whether
     * SamplerVoice had a positionNorm() accessor and drew no playhead if it
     * did not. The accessor had in fact existed for some time, so the
     * detection had only ever taken its true branch: a std::void_t template
     * pair, an <utility> include and a stopgap comment, all to test a
     * condition that could not be false. Scaffolding outlives the thing it
     * was holding up unless someone takes it down. */
    double playheadNorm (int well)
    {
        return juce::jlimit (0.0, 1.0,
                             (double) atomic_load (&bb.well[well].pos) / 65536.0);
    }

    int textW (const juce::Font& f, const juce::String& s)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, s));
    }

    /* One predicate for both drop routes -- the desktop file drag and the
     * LOCKER's internal drag -- so a file the panel accepts from Explorer can
     * never be one it silently refuses from the console's own browser. */
    bool isAudioPath (const juce::String& p)
    {
        return p.endsWithIgnoreCase (".wav")  || p.endsWithIgnoreCase (".aif")
            || p.endsWithIgnoreCase (".aiff") || p.endsWithIgnoreCase (".mp3")
            || p.endsWithIgnoreCase (".ogg")  || p.endsWithIgnoreCase (".flac");
    }

    /* The PITCH knob is semitones and so is the engine control, so there is no
     * conversion left to do. There used to be a rate<->semitone pair here,
     * because the JUCE voice stored a raw float rate; that is also why the A/Z
     * keys multiplied by 1.122f and escaped the knob's own range. See
     * keyPressed(). */
    float rateOf (int semis) { return (float) std::pow (2.0, semis / 12.0); }
}

/* ======================================================================== */
/*  SampleWell -- one quarter of the 2x2 grid                                */
/* ======================================================================== */

class GrainMassPanel::SampleWell : public juce::Component,
                                   public juce::SettableTooltipClient
{
public:
    SampleWell (GrainMassPanel& p, AudioEngine& a, int idx)
        : owner (p), audio (a), index (idx)
    {
        setTooltip (U8 ("WELL 0") + juce::String (index + 1)
                    + U8 (" \xe2\x80\x94 double-click or drop an audio file to load a specimen. "
                          "WAV/AIFF/MP3/OGG/FLAC."));

        auto prep = [this] (juce::Component& c)
        {
            addAndMakeVisible (c);
            c.setWantsKeyboardFocus (false);    // the panel owns the key map
        };
        prep (play); prep (stopB); prep (rev); prep (loop); prep (pitch);
        prep (level);

        play.setTooltip (U8 ("PLAY \xe2\x80\x94 sound this well into the master bus. "
                             "REC records it and SURVIVOR can loop it. Key P."));
        play.onToggle = [this] (bool on)
        {
            owner.select (index);
            if (on && ! bb_engine_well_loaded (index)) { play.setToggleStateQuiet (false); return; }
            atomic_store (&bb.well[index].play, on ? 1 : 0);
        };

        stopB.setTooltip (U8 ("STOP \xe2\x80\x94 silence this well. Key P."));
        stopB.onToggle = [this] (bool)
        {
            owner.select (index);
            atomic_store (&bb.well[index].play, 0);
            atomic_store (&bb.well[index].arm,  0);
        };

        rev.setTooltip (U8 ("REVERSE \xe2\x80\x94 play the specimen backwards. The head "
                            "turns round where it stands. Key R."));
        rev.onToggle = [this] (bool on)
        {
            owner.select (index);
            atomic_store (&bb.well[index].reverse, on ? 1 : 0);
        };

        loop.setOxideStyle (true);
        loop.setTooltip (U8 ("LOOP \xe2\x80\x94 repeat the specimen until stopped. Key O."));
        loop.onToggle = [this] (bool on)
        {
            owner.select (index);
            atomic_store (&bb.well[index].loop, on ? 1 : 0);
        };

        pitch.setShowText (false);              // 32px face; label painted below
        pitch.setTooltip (U8 ("PITCH \xe2\x80\x94 playback rate of this well, in semitones. "
                              "-24 to +24. Keys A / Z."));
        pitch.onChange = [this] (int st)
        {
            owner.select (index);
            atomic_store (&bb.well[index].ctl[WELL_CTL_PITCH],
                          juce::jlimit (-24, 24, st));
        };

        level.setShowText (false);              // 32px face; label painted below
        level.setTooltip (U8 ("LEVEL \xe2\x80\x94 this well into the master bus. "
                              "0\xe2\x80\x93" "256, 256 is unity; 128 is the shipped level."));
        level.onChange = [this] (int v)
        {
            owner.select (index);
            atomic_store (&bb.well[index].ctl[WELL_CTL_LEVEL],
                          juce::jlimit (0, 256, v));
        };
    }

    void setSelected (bool b)
    {
        if (selected != b) { selected = b; repaint(); }
    }

    /* Decode the loaded file once (message thread, at load time) into a
     * fixed min/max peak table plus the duration/rate header meta. */
    void analyse (const juce::File& f)
    {
        mins.assign ((size_t) kBins, 0.0f);
        maxs.assign ((size_t) kBins, 0.0f);
        hasPeaks = false;
        metaText.clear();

        std::unique_ptr<juce::AudioFormatReader> r (audio.getFormats().createReaderFor (f));
        if (r == nullptr || r->lengthInSamples <= 0 || r->sampleRate <= 0.0)
        {
            repaint();
            return;
        }

        const juce::int64 len = r->lengthInSamples;
        const int nCh = juce::jmin (2, (int) r->numChannels);
        const int block = 1 << 15;
        juce::AudioBuffer<float> tmp (juce::jmax (1, nCh), block);

        for (juce::int64 done = 0; done < len;)
        {
            const int n = (int) juce::jmin ((juce::int64) block, len - done);
            if (! r->read (&tmp, 0, n, done, true, nCh > 1))
                break;
            for (int i = 0; i < n; ++i)
            {
                float s = tmp.getSample (0, i);
                if (nCh > 1)
                    s = 0.5f * (s + tmp.getSample (1, i));
                const int bin = (int) ((done + i) * (juce::int64) kBins / len);
                auto& lo = mins[(size_t) juce::jlimit (0, kBins - 1, bin)];
                auto& hi = maxs[(size_t) juce::jlimit (0, kBins - 1, bin)];
                lo = juce::jmin (lo, s);
                hi = juce::jmax (hi, s);
            }
            done += n;
        }
        hasPeaks = true;

        const double sec = (double) len / r->sampleRate;
        juce::String dur;
        if (sec < 60.0)
            dur = juce::String (sec, 1) + "S";
        else
            dur = juce::String ((int) sec / 60) + ":"
                + juce::String ((int) sec % 60).paddedLeft ('0', 2);
        metaText = dur + U8 (" \xc2\xb7 ") + juce::String (r->sampleRate / 1000.0, 1) + "K";
        repaint();
    }

    /* 30 Hz engine pull (spec section 15): quiet writes, drag-guarded. The
     * engine is the single source of truth, so `play` is READ back rather
     * than remembered -- a non-looping well clears it from the audio thread
     * when it runs out, which is what un-latches the plate. */
    void sync()
    {
        const WellSlot& w = bb.well[index];
        const bool has      = bb_engine_well_loaded (index) != 0;
        const bool sounding = has && atomic_load (&w.play) != 0;
        const bool rv       = atomic_load (&w.reverse) != 0;
        const bool lp       = atomic_load (&w.loop) != 0;
        const int  st       = atomic_load (&w.ctl[WELL_CTL_PITCH]);
        const int  lv       = atomic_load (&w.ctl[WELL_CTL_LEVEL]);

        /* transport plates present the WELL's state: an empty well draws
         * every plate idle (HTML frame 04 empty well), even though the
         * well keeps its loop/reverse preference for the next load */
        if (! play.isUserDragging())  play.setToggleStateQuiet (sounding);
        if (! rev.isUserDragging())   rev.setToggleStateQuiet (has && rv);
        if (! loop.isUserDragging())  loop.setToggleStateQuiet (has && lp);
        if (! pitch.isUserDragging()) pitch.setValueQuiet (st);
        if (! level.isUserDragging()) level.setValueQuiet (lv);

        if (sounding || sounding != lastPlaying || has != lastHas
            || rv != lastRev || lp != lastLoop || st != lastSemis)
            repaint();

        lastPlaying = sounding; lastHas = has;
        lastRev = rv; lastLoop = lp; lastSemis = st;
    }

    /* The engine holds no juce::String, so the specimen's name lives here,
     * beside the peak table that is also panel-side and message-thread-only.
     * NOT setName(): juce::Component already has one, and quietly shadowing a
     * base-class member is how a component ends up with two names. */
    void setSpecimenName (const juce::String& n) { name = n; }

    void mouseDown (const juce::MouseEvent&) override        { owner.select (index); }
    void mouseDoubleClick (const juce::MouseEvent&) override { owner.select (index);
                                                               owner.loadInto (index); }

    void resized() override
    {
        auto b = getLocalBounds();
        b.removeFromTop (24);                                // header
        auto ctl = b.removeFromBottom (56);                  // control row
        const int by = ctl.getY() + 16;                      // 24 tall, centred in 56
        play .setBounds (8,   by, 44, 24);
        stopB.setBounds (55,  by, 44, 24);
        rev  .setBounds (102, by, 30, 24);
        loop .setBounds (135, by, 30, 24);
        pitch.setBounds (186, ctl.getY() + 6, 32, 32);       // knob+label centred in 56
        level.setBounds (228, ctl.getY() + 6, 32, 32);
    }

    void paint (juce::Graphics& g) override
    {
        const bool has      = bb_engine_well_loaded (index) != 0;
        const bool sounding = has && atomic_load (&bb.well[index].play) != 0;

        Rectangle<int> b = getLocalBounds();
        g.setColour (C::PANEL);
        g.fillRect (b);

        /* ---- header 24: WELL NN · filename · duration/rate · STATE ----- */
        Rectangle<int> head = b.removeFromTop (24);
        g.setColour (selected ? C::RAISED : C::PANEL_ALT);
        g.fillRect (head);
        g.setColour (C::HAIRLINE);
        g.fillRect (head.getX(), head.getBottom() - 1, head.getWidth(), 1);

        const juce::Font hf = Type::micro();
        g.setFont (hf);
        const juce::String wl = "WELL 0" + juce::String (index + 1);
        g.setColour (selected ? C::INK_DIM : C::INK_FAINT);
        g.drawText (wl, head.getX() + 8, head.getY(), textW (hf, wl) + 2,
                    head.getHeight(), Justification::centredLeft);

        /* STATE, as a word AND a lamp. Colour alone would not survive
         * greyscale (Theme.h colour-blind rule), and the old header conveyed
         * the whole of a well's state through one 5px square. The lamp triple
         * is the documented monotonic luminance ladder. */
        const juce::String stateWord = sounding ? "PLAYING" : has ? "LOADED" : "EMPTY";
        const juce::Colour stateInk  = sounding ? C::BLOOD_HOT
                                     : has      ? C::INK_DIM
                                                : C::INK_FAINT;
        const juce::Colour lampCol   = sounding ? C::BLOOD_HOT
                                     : has      ? C::LAMP_SOUNDING
                                                : C::LAMP_DEAD;
        const int lampX = head.getRight() - 8 - 6;
        g.setColour (lampCol);
        g.fillRect (lampX, head.getCentreY() - 3, 6, 6);

        const juce::Font sf = Type::micro();
        const int sw = textW (sf, stateWord) + 2;
        g.setFont (sf);
        g.setColour (stateInk);
        g.drawText (stateWord, lampX - 6 - sw, head.getY(), sw, head.getHeight(),
                    Justification::centredRight);

        /* header meta: duration/rate when loaded. An empty well says EMPTY in
         * the state slot and says how to fill it in the plot, so it does not
         * need a third notice here. */
        int metaR = lampX - 6 - sw - 10;
        if (has && metaText.isNotEmpty())
        {
            const juce::Font mf = Type::nano();
            g.setFont (mf);
            g.setColour (C::INK_FAINT);
            const int mw = textW (mf, metaText) + 2;
            g.drawText (metaText, metaR - mw, head.getY(), mw, head.getHeight(),
                        Justification::centredRight);
            metaR -= mw + 8;
        }

        g.setFont (Type::monoMedium (10.0f, 0.04f));
        g.setColour (has ? (selected ? C::INK : C::INK_DIM) : C::INK_FAINT);
        const int nameX = head.getX() + 8 + textW (hf, wl) + 10;
        g.drawText (has ? name : U8 ("\xe2\x80\x94 EMPTY \xe2\x80\x94"),
                    nameX, head.getY(), juce::jmax (0, metaR - nameX),
                    head.getHeight(), Justification::centredLeft, true);

        /* ---- control row 56 (children live here; chrome painted now) --- */
        Rectangle<int> ctl = b.removeFromBottom (56);
        g.setColour (C::HAIRLINE);
        g.fillRect (ctl.getX(), ctl.getY(), ctl.getWidth(), 1);

        g.setColour (C::HAIRLINE);                           // divider after O
        g.fillRect (ctl.getX() + 175, ctl.getY() + 13, 1, 30);

        // PITCH + LEVEL labels (live) under the knobs the children paint
        g.setColour (C::INK_DIM);
        g.setFont (Type::nano());
        g.drawText ("PITCH", ctl.getX() + 186, ctl.getY() + 40, 32, Type::rowH (8.0f),
                    Justification::centred);
        g.drawText ("LEVEL", ctl.getX() + 228, ctl.getY() + 40, 32, Type::rowH (8.0f),
                    Justification::centred);

        /* The GRAIN and ERASE knobs are gone. They were never components --
         * a paint lambda drew a CONTROL circle, a dead ring and a pointer
         * frozen at -135 degrees, twice per well, eight fake knobs on the
         * panel, with a 7px "GRAIN/ERASE: PLANNED" caption stranded 70px away
         * from them. A knob that cannot be turned is not a control, and the
         * space is better spent on the readout that is real. */

        // RATE: the live playback rate of this well, from its semitone control
        const float rate = rateOf (atomic_load (&bb.well[index].ctl[WELL_CTL_PITCH]));
        Rectangle<int> rateR (ctl.getX() + 270, ctl.getY() + 12,
                              juce::jmax (0, ctl.getWidth() - 270 - 8), 16);
        g.setFont (Type::micro());
        g.setColour (C::INK_FAINT);
        g.drawText ("RATE", rateR, Justification::centredLeft);
        g.setFont (Type::monoMedium (13.0f, 0.04f));
        g.setColour (has ? C::INK : C::INK_GHOST);
        g.drawText (juce::String (rate, 2) + U8 ("\xc3\x97"),
                    rateR.withTrimmedLeft (44), Justification::centredLeft);
        g.setFont (Type::nano());
        g.setColour (C::INK_FAINT);
        g.drawText ("A / Z", rateR.translated (0, 18), Justification::centredLeft);

        /* ---- waveform area: SOCKET, 8px margin, HAIRLINE_DIM border ---- */
        Rectangle<int> plot = b.reduced (8);
        if (plot.getWidth() < 4 || plot.getHeight() < 4)
            return;
        g.setColour (C::SOCKET);
        g.fillRect (plot);
        g.setColour (selected ? C::EDGE : C::HAIRLINE_DIM);
        g.drawRect (plot, 1);

        Rectangle<int> inner = plot.reduced (1);
        const int cy = inner.getCentreY();
        g.setColour (C::HAIRLINE_DIM);                       // centre line
        g.fillRect (inner.getX(), cy, inner.getWidth(), 1);

        if (has && hasPeaks)
        {
            // the real specimen: one 1px column per pixel, min..max
            const int iw = inner.getWidth();
            const int halfH = juce::jmax (1, inner.getHeight() / 2 - 2);
            g.setColour (C::INK);
            for (int x = 0; x < iw; ++x)
            {
                const int bin = juce::jlimit (0, kBins - 1, x * kBins / juce::jmax (1, iw));
                const float lo = juce::jlimit (-1.0f, 1.0f, mins[(size_t) bin]);
                const float hi = juce::jlimit (-1.0f, 1.0f, maxs[(size_t) bin]);
                const int y0 = cy - juce::roundToInt (hi * (float) halfH);
                const int y1 = cy - juce::roundToInt (lo * (float) halfH);
                g.fillRect (inner.getX() + x, y0, 1, juce::jmax (1, y1 - y0));
            }
        }

        if (sounding)
        {
            const double pn = playheadNorm (index);
            const int px = inner.getX()
                         + juce::roundToInt (pn * (double) juce::jmax (0, inner.getWidth() - 1));
            g.setColour (C::BLOOD_HOT);                      // 1px play position
            g.fillRect (px, plot.getY() + 1, 1, plot.getHeight() - 2);
        }

        /* An empty well must say so, in the middle of the space it is empty
         * in -- not in a 7px corner note in the palette's dimmest ink, which
         * is where this used to live. */
        if (! has)
        {
            Rectangle<int> mid = inner.withSizeKeepingCentre (
                inner.getWidth(), Type::rowH (10.0f) + Type::rowH (8.0f) + 4);
            g.setColour (C::INK_FAINT);
            g.setFont (Type::label());
            g.drawText ("DOUBLE-CLICK OR DROP A FILE",
                        mid.removeFromTop (Type::rowH (10.0f)), Justification::centred);
            mid.removeFromTop (4);
            g.setColour (C::INK_GHOST);
            g.setFont (Type::nano());
            g.drawText (U8 ("WAV \xc2\xb7 AIFF \xc2\xb7 MP3 \xc2\xb7 OGG \xc2\xb7 FLAC"),
                        mid, Justification::centred);
        }

        /* Selection frame, drawn last so it sits over the plot edge. The
         * selected well is where every key press lands, so it gets the
         * strongest structural cue on the panel: a full EDGE border (3.37:1)
         * against the HAIRLINE gaps the grid is otherwise made of. */
        if (selected)
        {
            g.setColour (C::EDGE);
            g.drawRect (getLocalBounds(), 1);
        }
    }

    GrainMassPanel& owner;
    AudioEngine& audio;
    const int index;
    bool selected = false;

    PlateButton play  { "PLAY", false, true  };
    PlateButton stopB { "STOP", false, false };
    PlateButton rev   { "R",    false, true  };
    PlateButton loop  { "O",    false, true  };
    EngravedKnob pitch { "PITCH", 32, -24, 24, 0 };
    EngravedKnob level { "LEVEL", 32, 0, 256, 128 };

    static constexpr int kBins = 512;
    std::vector<float> mins, maxs;
    bool hasPeaks = false;
    juce::String metaText;
    juce::String name;

    bool lastPlaying = false, lastHas = false, lastRev = false, lastLoop = false;
    int lastSemis = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleWell)
};

/* ======================================================================== */
/*  GrainMassPanel                                                           */
/* ======================================================================== */

GrainMassPanel::GrainMassPanel (AudioEngine& e) : audio (e)
{
    for (int i = 0; i < 4; ++i)
    {
        wells[(size_t) i] = std::make_unique<SampleWell> (*this, audio, i);
        addAndMakeVisible (*wells[(size_t) i]);
    }
    wells[0]->setSelected (true);

    playAll.setTooltip (U8 ("PLAY ALL \xe2\x80\x94 arm every loaded well; they all "
                            "start together, rewound, on the next bar of the "
                            "engine clock. Click while armed to cancel."));
    playAll.setMouseClickGrabsKeyboardFocus (false);
    playAll.onToggle = [this] (bool)
    {
        bool anyPending = false;
        for (int i = 0; i < BB_NWELL; ++i)
            anyPending = anyPending || atomic_load (&bb.well[i].arm) != 0;
        for (int i = 0; i < BB_NWELL; ++i)
        {
            if (anyPending)                        atomic_store (&bb.well[i].arm, 0);
            else if (bb_engine_well_loaded (i))    atomic_store (&bb.well[i].arm, 1);
        }
    };
    addAndMakeVisible (playAll);

    stopAll.setTooltip (U8 ("STOP ALL \xe2\x80\x94 silence every well now and "
                            "cancel any armed start."));
    stopAll.setMouseClickGrabsKeyboardFocus (false);
    stopAll.onToggle = [this] (bool)
    {
        for (int i = 0; i < BB_NWELL; ++i)
        {
            atomic_store (&bb.well[i].arm,  0);
            atomic_store (&bb.well[i].play, 0);
        }
    };
    addAndMakeVisible (stopAll);

    startTimerHz (30);
    setWantsKeyboardFocus (true);
}

GrainMassPanel::~GrainMassPanel() = default;

void GrainMassPanel::timerCallback()
{
    for (auto& w : wells)
        w->sync();

    bool anyPending = false;
    for (int i = 0; i < BB_NWELL; ++i)
        anyPending = anyPending || atomic_load (&bb.well[i].arm) != 0;
    playAll.setToggleStateQuiet (anyPending);   // lamp = armed, clears on fire
}

Rectangle<int> GrainMassPanel::gridArea() const
{
    Rectangle<int> b = getLocalBounds();
    b.removeFromTop (headerBandH);
    b.removeFromBottom (26);                                 // footer
    return b;
}

Rectangle<int> GrainMassPanel::wellRect (int i) const
{
    const Rectangle<int> b = gridArea();
    const int lw = juce::jmax (0, (b.getWidth() - 1) / 2);
    const int rw = juce::jmax (0, b.getWidth() - 1 - lw);
    const int th = juce::jmax (0, (b.getHeight() - 1) / 2);
    const int bh = juce::jmax (0, b.getHeight() - 1 - th);
    return { (i % 2 == 0) ? b.getX() : b.getX() + lw + 1,
             (i / 2 == 0) ? b.getY() : b.getY() + th + 1,
             (i % 2 == 0) ? lw : rw,
             (i / 2 == 0) ? th : bh };
}

int GrainMassPanel::slotAt (juce::Point<int> p) const
{
    for (int i = 0; i < 4; ++i)
        if (wellRect (i).contains (p)) return i;
    return slot;
}

void GrainMassPanel::resized()
{
    for (int i = 0; i < 4; ++i)
        wells[(size_t) i]->setBounds (wellRect (i));

    Rectangle<int> foot = getLocalBounds().removeFromBottom (26);
    playAll.setBounds (foot.getX() + 8,  foot.getY() + 4, 72, 18);
    stopAll.setBounds (foot.getX() + 84, foot.getY() + 4, 72, 18);
}

void GrainMassPanel::select (int w)
{
    w = juce::jlimit (0, 3, w);
    if (slot != w)
    {
        wells[(size_t) slot]->setSelected (false);
        slot = w;
        wells[(size_t) slot]->setSelected (true);
        repaint();                  // the footer names the selected well
    }
    if (! hasKeyboardFocus (true))
    {
        juce::Component::SafePointer<GrainMassPanel> safe (this);
        juce::Timer::callAfterDelay (10, [safe]
        {
            if (safe != nullptr) safe->grabKeyboardFocus();
        });
    }
}

void GrainMassPanel::mouseDown (const juce::MouseEvent& e)
{
    select (slotAt (e.getPosition()));
}

void GrainMassPanel::loadFileInto (int well, const juce::File& f)
{
    if (! f.existsAsFile() || well < 0 || well >= BB_NWELL) return;

    /* Decode HERE, on the message thread. The engine owns no file format
     * reader and must never be handed a path -- the same division LICKS uses
     * (LicksPanel::loadPath). The frames go over as mono int16 because the
     * whole engine bus is mono: a stereo specimen folds to (L+R)/2, which is
     * a real change from the JUCE wells and is also what every recording of
     * one has always sounded like, the sink having been mono throughout. */
    std::unique_ptr<juce::AudioFormatReader> r (audio.getFormats().createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0
        || r->lengthInSamples > 0x7fffffff / 2)
        return;                    // the length guard the wells never had

    const int len = (int) r->lengthInSamples;
    juce::AudioBuffer<float> tmp (2, len);

    /* CLEAR IT, AND CHECK THE READ. juce::AudioBuffer's constructor allocates
     * without zeroing, and AudioFormatReader::read() returns false WITHOUT
     * zeroing the destination when the underlying decode fails part way. A
     * truncated or damaged file therefore leaves uninitialised heap in `tmp`,
     * and the conversion below turns that into full-scale int16 -- which is
     * then published, and (this panel auto-plays a fresh load) sent straight
     * into the master bus, the recorder and every looper. The deleted
     * SamplerVoice::loadFile cleared its buffer; ArrangePanel's decoder still
     * does; analyse() forty lines above checks the return value. */
    tmp.clear();
    if (! r->read (&tmp, 0, len, 0, true, true))
        return;

    int16_t* mono = (int16_t*) calloc ((size_t) len, sizeof (int16_t));
    if (mono == nullptr) return;

    for (int i = 0; i < len; ++i)
    {
        const float m = tmp.getNumChannels() > 1
                ? (tmp.getSample (0, i) + tmp.getSample (1, i)) * 0.5f
                : tmp.getSample (0, i);
        const int32_t v = (int32_t) (m * 32767.0f);
        mono[i] = (int16_t) juce::jlimit (-32768, 32767, v);
    }

    /* bb_engine_well_set takes ownership of `mono` UNCONDITIONALLY -- it frees
     * it on every failure path too, so there is nothing to clean up here. */
    if (bb_engine_well_set (well, mono, len, (int) r->sampleRate) != 0)
    {
        wells[(size_t) well]->setSpecimenName (f.getFileNameWithoutExtension());
        wells[(size_t) well]->analyse (f);
        atomic_store (&bb.well[well].play, 1);
        select (well);
    }
}

void GrainMassPanel::loadInto (int well)
{
    /* Open on the console's own directory: REC, GROW and the ARRANGE
     * captures all write there, so that is where the specimens are. */
    chooser = std::make_unique<juce::FileChooser> (
        "Load a specimen",
        morgue::morgueDir().isDirectory()
            ? morgue::morgueDir()
            : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac");

    // slot captured by value at launch time; SafePointer guards teardown
    const int s = well;
    juce::Component::SafePointer<GrainMassPanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
                          [safe, s] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        safe->loadFileInto (s, fc.getResult());
    });
}

bool GrainMassPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioPath (f))
            return true;
    return false;
}

void GrainMassPanel::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (files.isEmpty()) return;
    loadFileInto (slotAt ({ x, y }), juce::File (files[0]));
}

bool GrainMassPanel::keyPressed (const juce::KeyPress& key)
{
    const int k = key.getKeyCode();
    if (slot < 0 || slot >= BB_NWELL) return false;
    WellSlot& w = bb.well[slot];

    if (k >= '1' && k <= '4')
    {
        select (k - '1');
        return true;
    }
    if (key.getTextCharacter() == 'p')
    {
        const bool sounding = atomic_load (&w.play) != 0;
        if (! sounding && ! bb_engine_well_loaded (slot)) return true;
        atomic_store (&w.play, sounding ? 0 : 1);
        return true;
    }
    if (key.getTextCharacter() == 'r')
    {
        atomic_store (&w.reverse, atomic_load (&w.reverse) ? 0 : 1);
        return true;
    }
    if (key.getTextCharacter() == 'o')
    {
        atomic_store (&w.loop, atomic_load (&w.loop) ? 0 : 1);
        return true;
    }

    /* A and Z are ONE SEMITONE, and they respect the knob's range.
     *
     * They used to multiply a raw float rate by 1.122f. That constant is
     * 2^(2/12) -- a whole tone, twice the step the tooltip and the knob both
     * promise -- and SamplerVoice::setRate stored it without a clamp, so a
     * thirteenth press pushed the well past +24 semitones while the knob face
     * sat frozen at 24, the only place the -24..+24 limit was ever applied.
     * An integer semitone control makes the step exact, the range real, and
     * the keys and the knob the same control. */
    if (key.getTextCharacter() == 'a' || key.getTextCharacter() == 'z')
    {
        const int step = key.getTextCharacter() == 'a' ? 1 : -1;
        atomic_store (&w.ctl[WELL_CTL_PITCH],
                      juce::jlimit (-24, 24,
                                    atomic_load (&w.ctl[WELL_CTL_PITCH]) + step));
        return true;
    }

    return false;
}

/* ---- LOCKER drag ----------------------------------------------------------
 * The LOCKER is a juce::DragAndDropContainer and packages the absolute path
 * of the row you dragged (Chrome.cpp). GRAIN MASS implemented only
 * FileDragAndDropTarget, which is the interface for a drag out of Explorer or
 * Finder, so a drag from the console's OWN locker sailed over the wells and
 * did nothing -- while the identical file dragged from a file manager landed.
 * GRAIN LICKS has had the internal target all along; this is the same pair. */
bool GrainMassPanel::isInterestedInDragSource (const SourceDetails& d)
{
    const juce::String p = d.description.toString();
    return isAudioPath (p) && juce::File::isAbsolutePath (p)
        && juce::File (p).existsAsFile();
}

void GrainMassPanel::itemDropped (const SourceDetails& d)
{
    const juce::String p = d.description.toString();
    if (! isAudioPath (p) || ! juce::File::isAbsolutePath (p)) return;
    loadFileInto (slotAt (d.localPosition), juce::File (p));
}

void GrainMassPanel::paint (juce::Graphics& g)
{
    Rectangle<int> b = getLocalBounds();

    paintHeaderBand (g, b.removeFromTop (headerBandH),
                     "GRAIN MASS",
                     U8 ("4 WELLS \xc2\xb7 WAV/AIFF/MP3/OGG/FLAC"),
                     juce::String (SerialNo::MASS) + U8 (" \xc2\xb7 DOUBLE-CLICK A WELL TO LOAD"),
                     Badge::LIVE, "LIVE");

    /* the 1px gaps between the four wells read as hairlines */
    g.setColour (C::HAIRLINE);
    g.fillRect (gridArea());

    /* ---- footer 26: mixing note · key reference · vision line ---------- */
    Rectangle<int> foot = b.removeFromBottom (26);
    g.setColour (C::PANEL_ALT);
    g.fillRect (foot);
    g.setColour (C::HAIRLINE);
    g.fillRect (foot.getX(), foot.getY(), foot.getWidth(), 1);

    const juce::Font ff = Type::micro();
    g.setFont (ff);
    g.setColour (C::INK_FAINT);
    const juce::String mixNote = U8 ("PLAY ALL FIRES ON THE NEXT BAR");
    int fx = foot.getX() + 164;                 // after the PLAY/STOP ALL plates
    g.drawText (mixNote, fx, foot.getY(), textW (ff, mixNote) + 2, foot.getHeight(),
                Justification::centredLeft);
    fx += textW (ff, mixNote) + 14;
    g.drawText (U8 ("P = PLAY \xc2\xb7 R = REVERSE \xc2\xb7 O = LOOP \xc2\xb7 A / Z = PITCH"),
                fx, foot.getY(), foot.getWidth(), foot.getHeight(),
                Justification::centredLeft);

    /* The old right slot carried "VISION: SLICING - TAPE ERASER - CROSS-MOD
     * WITH VOICES" in OXIDE: three features that do not exist, drawn as a
     * status line on the instrument's face. What belongs in that slot is the
     * one thing the player needs to know before pressing a key -- which well
     * the keys are going to act on. */
    g.setColour (C::INK_DIM);
    g.drawText ("KEYS ACT ON WELL 0" + juce::String (slot + 1),
                foot.reduced (10, 0), Justification::centredRight);
}

} // namespace morgue
