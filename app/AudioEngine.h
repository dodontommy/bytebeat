/* AudioEngine.h -- the JUCE side of the seam + the console's helpers.
 *
 * engine.c is the instrument and knows nothing about JUCE. This class owns
 * JUCE's AudioDeviceManager, an audio callback that pushes every hardware
 * buffer straight into bb_engine_render(), and a WAV recorder fed from the
 * engine's own sink ring. Nothing mixes on top of the engine: everything you
 * can hear goes through bb_engine_render(), which is the only way everything
 * you can hear also ends up in the recording.
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

/* SamplerVoice IS GONE, and its absence is the fix.
 *
 * It was one hardware output voice per GRAIN MASS well, holding the decoded
 * file behind a mutex and adding itself into the device's float buffers from
 * mixInto(), called after bb_engine_render() had returned. That put the wells
 * downstream of bb.sink, which is the ring the WAV recorder drains, the master
 * meter measures, the scope reads and the loop bank captures -- so a well
 * reached the speakers and none of those four. REC did not record it, SURVIVOR
 * could not loop it, and ARRANGE's MASS lane refused to capture for the honest
 * reason that there was no engine-side bus to tap.
 *
 * The wells now live in engine.c as bb.well[], summed inside the render loop
 * beside the LICKS bus (see the block comment on WellSlot in bytebeat.h). Two
 * things improved on the way past: the audio thread no longer try-locks a
 * std::mutex and silently drops a whole buffer when the UI is mid-load, and
 * PLAY ALL fires on the transport's own downbeat instead of at the top of
 * whichever device buffer first noticed the bar counter move.
 *
 * If a JUCE-side mixer is ever wanted again -- it should not be -- read R1's
 * note in DESIGN_SPEC.md first. It predicted this exact bug in advance. */

/* Audio thread callback: hand every device buffer to the engine. Nothing is
 * mixed on top; if it were, it would not be in the recording. */
class EngineCAPlayback final : public juce::AudioIODeviceCallback
{
public:
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
    }

    void start()  { engine.addAudioCallback (&callback); }
    void stop()   { engine.removeAudioCallback (&callback); }

    juce::AudioDeviceManager& getManager() { return engine; }

    /* Empty when the output opened. Otherwise JUCE's own explanation, which
     * the console surfaces rather than swallowing (see the constructor). */
    juce::String deviceError() const { return deviceErr; }

    WavRecorder& recorder() { return wav; }

    /* Still here, and still needed: GRAIN MASS decodes a specimen on the
     * message thread through this before handing the frames to the engine,
     * which owns no file format reader of its own. */
    juce::AudioFormatManager& getFormats() { return formats; }

private:
    juce::AudioDeviceManager engine;
    juce::String deviceErr;                   // empty = the device opened
    juce::AudioFormatManager formats;
    EngineCAPlayback callback;
    WavRecorder wav;
};
