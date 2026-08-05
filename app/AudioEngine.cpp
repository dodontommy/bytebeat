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

    // Bar-synced well starts: on the engine clock's bar transition, fire
    // every armed voice together, rewound, in this same buffer. Atomics
    // only -- the deadline is untouched.
    const unsigned barNow = (unsigned) atomic_load_explicit (&bb.bar,
                                                             memory_order_relaxed);
    if (barNow != barSeen)
    {
        if (barSeen != ~0u && voices != nullptr)
            for (int v = 0; v < numVoices; ++v)
                if (voices[v] != nullptr && voices[v]->syncPending())
                    voices[v]->fireSync();
        barSeen = barNow;
    }

    // Mix the sampler voices over the engine.
    if (voices != nullptr)
        for (int v = 0; v < numVoices; ++v)
            if (voices[v] != nullptr)
                voices[v]->mixInto (out, nOut, nframes);
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

    /* Tell the sampler voices the real device rate (resampling base). */
    if (voices != nullptr)
        for (int v = 0; v < numVoices; ++v)
            if (voices[v] != nullptr)
                voices[v]->setOutputRate (dev->getCurrentSampleRate());
}

void EngineCAPlayback::audioDeviceStopped() {}

/* ======================================================================== */
/*  Sampler voices                                                           */
/* ======================================================================== */

SamplerVoice::SamplerVoice() {}
SamplerVoice::~SamplerVoice() {}

bool SamplerVoice::loadFile (const juce::File& f)
{
    std::lock_guard<std::mutex> lock (mu);

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr)
        return false;

    int chans = juce::jmin (2, (int) reader->numChannels);
    buf.setSize (chans, (int) reader->lengthInSamples, false, false, true);
    buf.clear();
    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
    bufRate = reader->sampleRate;
    name = f.getFileNameWithoutExtension();
    pos = 0.0;
    return true;
}

bool SamplerVoice::hasData() const noexcept { return buf.getNumSamples() > 0; }
juce::String SamplerVoice::getName() const noexcept { return name; }

void SamplerVoice::play()
{
    /* A finished non-looping one-shot leaves pos parked past the end; a
     * bare `playing = true` would be swallowed by mixInto immediately.
     * Rewind out-of-range positions so PLAY always sounds. Message-thread
     * lock; the audio thread only try-locks so it is never blocked. */
    {
        std::lock_guard<std::mutex> lock (mu);
        const int len = buf.getNumSamples();
        if (pos < 0 || pos >= len)
            pos = (reverse.load (std::memory_order_relaxed) && len > 1)
                      ? (double) (len - 1) : 0.0;
    }
    playing.store (true, std::memory_order_relaxed);
}

void SamplerVoice::mixInto (float* const* out, int nOut, int nframes)
{
    if (! playing.load (std::memory_order_relaxed))
        return;

    std::unique_lock<std::mutex> lock (mu, std::try_to_lock);
    if (!lock.owns_lock())
        return;   // UI is mid-load; don't stall the audio thread

    int chans = buf.getNumChannels();
    int len = buf.getNumSamples();
    if (len <= 0 || chans <= 0)
        return;   // buffer inspected only under the same mutex loadFile holds

    const bool rev = reverse.load (std::memory_order_relaxed);
    const bool lp  = loop.load (std::memory_order_relaxed);

    // consume a bar-synced rewind under the same lock that guards pos
    if (retrig.exchange (false, std::memory_order_relaxed))
        pos = (rev && len > 1) ? (double) (len - 1) : 0.0;

    // pitch by resampling: ratio = samples of file per sample of output
    double fileRate = bufRate > 0 ? bufRate : 44100.0;
    double devRate  = outRate.load (std::memory_order_relaxed);
    if (devRate <= 0) devRate = 44100.0;
    double ratio = (double) rate.load (std::memory_order_relaxed) * (fileRate / devRate);

    /* The LEVEL knob is the sole authority -- no hidden focus attenuation,
     * so a well's loudness never changes because you clicked elsewhere. */
    float g = gain.load (std::memory_order_relaxed);

    for (int i = 0; i < nframes; ++i)
    {
        if (pos < 0 || pos >= len)
        {
            if (lp)
            {
                pos = 0.0;
                if (rev && len > 1) pos = len - 1;
            }
            else
            {
                playing.store (false, std::memory_order_relaxed);
                break;
            }
        }

        int idx = juce::jlimit (0, len - 1, (int) pos);
        float sampleL = buf.getSample (0, idx);
        float sampleR = chans > 1 ? buf.getSample (1, idx) : sampleL;

        if (nOut >= 2)
        {
            if (out[0] != nullptr) out[0][i] += sampleL * g;
            if (out[1] != nullptr) out[1][i] += sampleR * g;
        }
        else if (nOut >= 1 && out[0] != nullptr)
        {
            out[0][i] += (sampleL + sampleR) * 0.5f * g;
        }

        pos += rev ? -ratio : ratio;
    }

    posNorm.store (juce::jlimit (0.0, 1.0, pos / (double) juce::jmax (1, len)),
                   std::memory_order_relaxed);
}

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
