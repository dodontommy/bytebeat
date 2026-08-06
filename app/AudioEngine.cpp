/* AudioEngine.cpp -- the JUCE<->engine seam implementation. */

#include "AudioEngine.h"
#include "Session.h"
#include "bytebeat.h"
#include "engine.h"

#include <juce_audio_basics/juce_audio_basics.h>

/* std::fill_n is <algorithm>, not <vector>. libc++ drags it in through the
 * container headers, so this compiled on macOS by luck; MSVC's STL does not,
 * and the same is true of any header a translation unit merely hopes is
 * already included. Name what you use. */
#include <algorithm>
#include <cstdint>

void EngineCAPlayback::audioDeviceIOCallbackWithContext (const float* const*,
                                                         int,
                                                         float* const* out,
                                                         int nOut,
                                                         int nframes,
                                                         const juce::AudioIODeviceCallbackContext&)
{
    if (nOut <= 0 || out == nullptr || nframes <= 0)
        return;

    /* The engine clamps its interleave to 8 channels internally; render and
     * read with the SAME clamped count so the stride always matches (a
     * 9-16-channel device would otherwise read a garbled interleave).
     * Channels past the clamp duplicate the last engine channel. */
    const int ch = juce::jmin (nOut, 8);

    /* Hard-realtime rule: never allocate here. The scratch buffer was
     * preallocated in audioDeviceAboutToStart, sized for the buffer size the
     * device advertised then.
     *
     * That advertised size is not a promise. WASAPI in shared mode and ASIO
     * drivers under a control-panel change both hand the callback a bigger
     * block at runtime without stopping the stream first, and CoreAudio does
     * it too when the aggregate device is reconfigured. The old code answered
     * that by writing silence for the whole buffer and returning -- which also
     * skipped the bar-sync edge detector below, so an oversized buffer did not
     * merely drop out, it froze every armed well until the size came back
     * down. Render in as many passes as the scratch we own allows instead: no
     * allocation, no lock, correct audio, and the clock keeps running. */
    const int capFrames = ch > 0 ? (int) (scratch.size() / (size_t) ch) : 0;
    if (capFrames <= 0)
    {
        for (int c = 0; c < nOut; ++c)
            if (out[c] != nullptr)
                std::fill_n (out[c], (size_t) nframes, 0.0f);
        return;
    }

    constexpr float scale = 1.0f / 32768.0f;
    int16_t* s = scratch.data();

    for (int done = 0; done < nframes; )
    {
        const int blk = juce::jmin (capFrames, nframes - done);
        std::fill_n (s, (size_t) blk * (size_t) ch, (int16_t) 0);

        bb_engine_render (s, blk, ch);

        for (int c = 0; c < nOut; ++c)
        {
            if (out[c] == nullptr)
                continue;
            const int sc = juce::jmin (c, ch - 1);
            for (int i = 0; i < blk; ++i)
                out[c][done + i] = s[i * ch + sc] * scale;
        }
        done += blk;
    }

    /* Nothing is mixed on top of the engine here any more, and that is the
     * point of this file's current shape.
     *
     * GRAIN MASS used to live in these last twenty lines: a bar-counter edge
     * detector that fired armed wells, and a loop that added four SamplerVoice
     * objects into `out` -- AFTER bb_engine_render() above had already filled
     * bb.sink. Everything this instrument does with finished audio reads
     * bb.sink: the WAV recorder, the master meter, the scope, the loop bank.
     * So the wells were audible and invisible to all four, and REC could not
     * record what you were hearing. The wells are inside the engine now, and
     * the callback's whole job is to hand the device's buffer to the engine
     * and copy the result back. */
}

void EngineCAPlayback::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    if (dev == nullptr)
        return;

    bb_engine_init (dev->getCurrentSampleRate());

    /* Preallocate the interleaved scratch on the message thread: buffer
     * size x 8 channels (the engine's channel clamp) with margin, so the
     * callback never touches the allocator. */
    const size_t frames = (size_t) juce::jmax (64, dev->getCurrentBufferSizeSamples());
    scratch.resize (juce::jmax (scratch.size(), frames * 8 * 2));

    /* The wells used to be told the device rate here so they could resample
     * against it. bb_engine_init() above already stored that rate, and the
     * wells read it from the engine, so there is one copy of it now instead
     * of two that could disagree after a device change. */
}

void EngineCAPlayback::audioDeviceStopped() {}

/* ======================================================================== */
/*  WAV recorder -- drains the engine's own sink ring to disk                */
/* ======================================================================== */

bool WavRecorder::start()
{
    if (active) return false;

    const juce::File dir = morgue::morgueDir();
    dir.createDirectory();

    /* The stamp is second-granular, so stopping and starting REC inside one
     * second lands on a name that already exists. FileOutputStream APPENDS to
     * an existing file (the same trap Main.cpp's screenshot writer and
     * ArrangePanel's capture writer both note and side-step with deleteFile),
     * which would splice a second RIFF header into the middle of the first
     * recording and corrupt both. Take the next free -02, -03 ... instead of
     * deleting a recording the player made moments ago, and only then clear
     * the way for a clean, non-appending stream. */
    const juce::String stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    juce::File file = dir.getChildFile (stamp + ".wav");
    for (int n = 2; file.existsAsFile() && n <= 99; ++n)
        file = dir.getChildFile (stamp + "-" + juce::String (n).paddedLeft ('0', 2) + ".wav");

    file.deleteFile();                 // FileOutputStream appends to existing files
    auto stream = file.createOutputStream();
    if (stream == nullptr) return false;

    target = file;
    out.reset (stream.release());
    framesWritten = 0;
    laps = 0;
    ringRead = atomic_load (&bb.sink_w);

    // RIFF header (sizes patched on stop). Mono int16 at the ENGINE rate:
    // the sink ring is filled at the device rate bb_engine_init stored.
    const int rate = atomic_load (&bb.rate);
    out->write ("RIFF", 4);
    out->writeInt (0);
    out->write ("WAVE", 4);
    out->write ("fmt ", 4);
    out->writeInt (16);
    out->writeShort (1);
    out->writeShort (1);
    out->writeInt (rate);
    out->writeInt (rate * 2);   // byte rate = rate * blockAlign (2, mono int16)
    out->writeShort (2);
    out->writeShort (16);
    out->write ("data", 4);
    out->writeInt (0);

    active = true;
    return true;
}

void WavRecorder::service()
{
    if (!active) return;
    unsigned w = atomic_load (&bb.sink_w);

    /* Ring-lap guard: if a message-thread stall (> ~22 s) let the audio
     * thread lap our cursor, the oldest unread cells were overwritten.
     * Skip to the oldest still-valid sample instead of splicing garbage. */
    if (w - ringRead > BB_SINK_LEN)
    {
        ringRead = w - BB_SINK_LEN;
        ++laps;
    }

    while (ringRead != w)
    {
        int16_t s = bb.sink[ringRead & BB_SINK_MASK];
        out->writeShort ((short) s);
        ringRead++;
        framesWritten++;
    }
}

void WavRecorder::stop()
{
    if (!active) return;
    service();

    // patch sizes in the header we wrote at start
    const auto pos = out->getPosition();
    const int dataSize = (int) (pos - 44);
    out->setPosition (4);   out->writeInt (36 + dataSize);
    out->setPosition (40);  out->writeInt (dataSize);
    out->setPosition (pos);

    active = false;
    out.reset();
}
