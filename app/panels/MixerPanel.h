/* MixerPanel.h -- MIXER stage (spec section 10, serial N.72-0422).
 *
 * 12 strips: V01-V08, LICKS, RETURN A, RETURN B, MASTER (flex 1.5).
 * LIVE wiring: per-layer fader -> bb.layer[L].ctl[LCTL_LEVEL], mute ->
 * bb.layer[L].on, master fader -> bb.gain, master M -> bb.mute (mirrors
 * RUN), master meter <- bb.sink ring (post-gain program).
 *
 * SEND A and RETURN A are LIVE: send A -> bb.layer[L].send (LICKS ->
 * bb.smp_send), RETURN A fader -> bb.verb_level, its SIZE/TONE knobs ->
 * bb.verb_size / bb.verb_tone, its meter <- bb.verb_peak. That strip IS
 * the CHAMBER, the engine's master reverb bus.
 *
 * INSERTS / SENDS B-D / PAN and the RETURN B strip remain the R4/R5
 * drawn state, reproduced exactly from the design handoff mock -- the
 * engine has no inserts, other sends, pan, per-layer solo or a sampler
 * bus level yet. Meters without an engine source stay empty (never fake
 * live data).
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "Theme.h"
#include "Primitives.h"

namespace morgue
{

class MixerPanel : public juce::Component
{
public:
    MixerPanel();
    void resized() override;
    void sync();                      // 30 Hz pull, driven by MainComponent
    void paint (juce::Graphics&) override;

private:
    /* One strip of the 12 (spec section 10 strip stack: header 22,
     * INSERTS, SENDS, PAN, fader area, value/dB/M/S/route). */
    class Strip : public juce::Component
    {
    public:
        enum class Kind { Voice, Licks, Return, Master };

        Strip (Kind k, int stripIndex, int layerIndex);

        void resized() override;
        void paint (juce::Graphics&) override;

        /* 30 Hz pull target; repaints only when something changed */
        void update (const juce::String& newName, bool newMuted,
                     bool newHot, int newValue);

        const Kind kind;
        const int  strip;                 // 0..11: send phase + routing
        const int  layer;                 // engine layer for Voice, else -1

        /* live children (meter on all; without a source it stays an
         * empty trough) */
        std::unique_ptr<TroughFader>    fader;
        std::unique_ptr<PlateButton>    muteBtn;
        std::unique_ptr<MeterComponent> meter;
        std::unique_ptr<EngravedKnob>   sendA;      // Voice / LICKS strips
        std::unique_ptr<EngravedKnob>   retSize;    // RETURN A only
        std::unique_ptr<EngravedKnob>   retTone;    // RETURN A only

    private:
        juce::String dbText() const;

        juce::String name;
        bool muted = false, hot = false;
        int  value = 0;                   // engine value, or drawn level

        /* R4/R5 drawn state (handoff mock, reproduced exactly) */
        const char* inserts[4] { nullptr, nullptr, nullptr, nullptr };
        const char* panLabel = "C";
        int         panPos   = 50;
        const char* route    = "MASTER";
        const char* drawnDb  = nullptr;   // printed dB on drawn strips

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Strip)
    };

    juce::OwnedArray<Strip> strips;
    juce::Rectangle<int> stripsArea, footerArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};

} // namespace morgue
