/* AudioEngine.h -- the JUCE side of the seam + the console's helpers.
 *
 * engine.c is the instrument and knows nothing about JUCE. This class owns
 * JUCE's AudioDeviceManager, an audio callback that pushes every hardware
 * buffer straight into bb_engine_render(), a WAV recorder fed from the
 * engine's own sink ring, and the sampler voices that mix on top.
 */

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/* One hardware output voice for the sampler. UI loads files; the audio
 * thread reads the loaded buffer under a short lock. Control flags cross
 * the UI/audio boundary as relaxed atomics (the audio thread also writes
 * `playing` when a one-shot ends). */
class SamplerVoice
{
public:
    SamplerVoice();
    ~SamplerVoice();

    bool loadFile (const juce::File&);
    bool hasData() const noexcept;
    juce::String getName() const noexcept;

    void play();   // rewinds a finished one-shot so PLAY always sounds
    void stop()  { playing.store (false, std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return playing.load (std::memory_order_relaxed); }

    float getRate() const noexcept { return rate.load (std::memory_order_relaxed); }
    bool getReverse() const noexcept { return reverse.load (std::memory_order_relaxed); }
    bool getLoop() const noexcept { return loop.load (std::memory_order_relaxed); }
    void setRate (float r)        { rate.store (r, std::memory_order_relaxed); }
    void setReverse (bool b)      { reverse.store (b, std::memory_order_relaxed); }
    void setLoop (bool b)         { loop.store (b, std::memory_order_relaxed); }
    void setGain (float g)        { gain.store (g, std::memory_order_relaxed); }
    float getGain() const noexcept { return gain.load (std::memory_order_relaxed); }
    void setFocused (bool b)      { focused.store (b, std::memory_order_relaxed); }
    void setOutputRate (double r) { outRate.store (r, std::memory_order_relaxed); }

    /* Bar-synced start: the UI arms a well; the audio callback fires every
     * pending well together on the next bar transition of the engine clock,
     * rewound to the top of its sample. All atomics -- no locks. */
    void armSyncStart()            { syncPend.store (true,  std::memory_order_relaxed); }
    void cancelSyncStart()         { syncPend.store (false, std::memory_order_relaxed); }
    bool syncPending() const noexcept { return syncPend.load (std::memory_order_relaxed); }
    void fireSync()                                        // audio thread only
    {
        syncPend.store (false, std::memory_order_relaxed);
        retrig.store (true, std::memory_order_relaxed);
        playing.store (true, std::memory_order_relaxed);
    }

    /* Current play position 0..1 (mirrored out of mixInto, relaxed). */
    double positionNorm() const noexcept { return posNorm.load (std::memory_order_relaxed); }

    void mixInto (float* const* out, int nOut, int nframes);

private:
    std::mutex mu;
    juce::AudioBuffer<float> buf;
    juce::String name;
    double bufRate = 0.0;

    double pos = 0.0;                       // guarded by mu
    std::atomic<float> rate    { 1.0f };
    std::atomic<bool>  reverse { false };
    std::atomic<bool>  loop    { true };
    std::atomic<bool>  playing { false };
    std::atomic<bool>  focused { false };
    std::atomic<float> gain    { 0.5f };
    std::atomic<double> outRate { 44100.0 };
    std::atomic<double> posNorm { 0.0 };
    std::atomic<bool>  syncPend { false };  // armed for bar-synced start
    std::atomic<bool>  retrig   { false };  // rewind consumed in mixInto
};

/* Audio thread callback: render the engine, then mix sampler voices over it. */
class EngineCAPlayback final : public juce::AudioIODeviceCallback
{
public:
    void attachVoices (SamplerVoice** v, int n) { voices = v; numVoices = n; }

    void audioDeviceIOCallbackWithContext (const float* const* in,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    std::vector<int16_t> scratch;           // preallocated in audioDeviceAboutToStart;
                                            //   NEVER resized on the audio thread
    SamplerVoice** voices = nullptr;
    int numVoices = 0;
    unsigned barSeen = ~0u;                 // engine bar counter edge detector
};

/* WAV recorder: drains the engine's sink ring and writes real audio. */
class WavRecorder
{
public:
    bool start();
    void service();   // call every UI frame
    void stop();
    bool isActive() const noexcept { return active; }
    unsigned frames() const noexcept { return framesWritten; }
    unsigned dropouts() const noexcept { return laps; }   // ring laps skipped
    juce::File currentFile() const { return target; }     // what stop() finalizes

private:
    bool active = false;
    unsigned framesWritten = 0;
    unsigned ringRead = 0;
    unsigned laps = 0;
    juce::File target;
    std::unique_ptr<juce::OutputStream> out;
};

class AudioEngine
{
public:
    AudioEngine()
    {
        /* initialise() RETURNS the error; the fifth argument is the preferred
         * default device NAME, not an out-parameter. Handing it an empty
         * String and dropping the return value -- which is what this call used
         * to do -- meant a device that refused to open failed in total
         * silence: no sound, no message, nothing in the console to explain it.
         * Windows makes that far more likely than macOS ever did (WASAPI
         * exclusive mode already held by another app, a disabled endpoint, no
         * ASIO driver at all), so the error is kept and the status bar prints
         * it. */
        deviceErr = engine.initialise (0, 2, nullptr, true, {}, nullptr);
        if (deviceErr.isEmpty() && engine.getCurrentAudioDevice() == nullptr)
            deviceErr = "no audio output device";
        formats.registerBasicFormats();
        for (int i = 0; i < 4; ++i)
        {
            voices[i].reset (new SamplerVoice());
            voicePtrs[(size_t) i] = voices[(size_t) i].get();
        }
        callback.attachVoices (voicePtrs.data(), 4);
    }

    void start()  { engine.addAudioCallback (&callback); }
    void stop()   { engine.removeAudioCallback (&callback); }

    juce::AudioDeviceManager& getManager() { return engine; }

    /* Empty when the output opened. Otherwise JUCE's own explanation, which
     * the console surfaces rather than swallowing (see the constructor). */
    juce::String deviceError() const { return deviceErr; }

    WavRecorder& recorder() { return wav; }
    juce::AudioFormatManager& getFormats() { return formats; }
    SamplerVoice* voice (int i) { return (i >= 0 && i < 4) ? voices[(size_t) i].get() : nullptr; }
    int numVoices() const noexcept { return 4; }

private:
    juce::AudioDeviceManager engine;
    juce::String deviceErr;                   // empty = the device opened
    juce::AudioFormatManager formats;
    EngineCAPlayback callback;
    WavRecorder wav;
    std::array<std::unique_ptr<SamplerVoice>, 4> voices;
    std::array<SamplerVoice*, 4> voicePtrs;   // stable for the audio callback
};
