/* RackPanel.h -- the voice station (spec section 5, serial N.72-0418, LIVE).
 *
 * Fully engine-wired: voice focus, source rack, ROLL/MUTATE, expression
 * compile via bb_publish, p0-p7 with bytecode-inferred roles, post chain,
 * 16-step sequencer + parameter-lock lane. The 30 Hz sync() pulls engine
 * truth into every control with isUserDragging guards.
 *
 * Geometry per spec section 5 / HTML frame "01 RACK":
 *   header band 24 | voice strip 46 | SOURCE column 222 |
 *   expression 20+122 | p0-p7 20+knob row | POST 420 wide | sequencer flex.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "Theme.h"
#include "Primitives.h"
#include "bytebeat.h"

namespace morgue
{

class RackPanel : public juce::Component
{
public:
    RackPanel();
    ~RackPanel() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    void sync();                            // 30 Hz pull: engine -> console
    void grabExprFocus();
    int  focusedLayer() const noexcept { return layer; }
    void focusVoice (int L);                // keys 1-8 (spec section 15)
    void toggleVoice (int L);               // shift+1-8 / shift-click: layer on/off
    void triggerFocused (int midiNote, int vel);

private:
    /* RACK-only sub-components, defined in RackPanel.cpp. */
    class VoicePlate;                       // 30x26 numbered plate + 4px lamp
    class SourceCell;                       // 22-tall "NN NAME" generator cell
    class ExpressionEditor;                 // SOCKET code well, block cursor
    class LockLane;                         // 16-slot oxide parameter-lock lane

    void applyRack (int src, int body, int space, bool reseed);
    void rollVoice (bool mutate);
    void selectLayer (int L);
    void applyPatch (int idx);              // patch morgue: whole-voice apply

    /* ---- VOICE DESIGN: five fixed perceptual macros -------------------
     * PITCH / MOTION / DIRT / DARK / ROOM. Each drives whichever knobs of
     * the CURRENT voice carry that meaning -- slot kinds when the rack
     * built the expression, bytecode roles when it is custom. Macros are
     * write-gestures around a captured base: the engine stays the single
     * source of truth and the p-knobs visibly move as a macro turns. */
    enum { MACRO_PITCH = 0, MACRO_MOTION, MACRO_DIRT, MACRO_DARK,
           MACRO_ROOM, MACRO_COUNT };
    void captureMacroBase (int L);          // snapshot params/post as base
    void applyMacroAxis (int axis, int v);  // recompute that axis's targets
    void sculptNudge (int axis, int dir);   // directed mutation, +-1 dir
    void pushUndo();
    void popUndo();
    void compileExpression (const juce::String& text);
    void loadExprFromEngine (bool force);
    void setLockView (int lane);            // follows the last-touched knob
    void syncLockLane();
    juce::String lockLaneName (int lane) const;
    int  lockLaneMax (int lane) const;

    juce::OwnedArray<VoicePlate>   voicePlates;
    juce::OwnedArray<SourceCell>   sourceCells, patchCells;
    juce::OwnedArray<EngravedKnob> paramKnobs, chainKnobs, designKnobs;
    juce::OwnedArray<PlateButton>  sculptBtns;
    juce::OwnedArray<StepCell>     steps;
    std::unique_ptr<ExpressionEditor> expr;
    std::unique_ptr<LockLane>      lockLane;
    std::unique_ptr<PlateButton>   stepBackBtn;
    PlateButton bodyBtn, spaceBtn, rollBtn, mutateBtn;

    int layer = 0;
    int lockView = 0;                       // viewed lock lane, 0-based

    /* per-layer macro state: knob positions + the base they scale around */
    struct Macro
    {
        int  val[MACRO_COUNT] { 128, 128, 128, 128, 128 };
        int  baseP[BB_NPARAM] {};
        int  baseCtl[LCTL_COUNT] {};
        int  baseSend = 0;
        bool captured = false;
    };
    Macro macroState[BB_NLAYER];
    int   appliedPatch[BB_NLAYER] { -1, -1, -1, -1, -1, -1, -1, -1 };

    struct UndoStep
    {
        int layer;
        int p[BB_NPARAM];
        int ctl[LCTL_COUNT];
        int send;
        int mval[MACRO_COUNT];
    };
    std::vector<UndoStep> undoStack;
    juce::Random sculptRng;

    /* per-layer compile bookkeeping for the editor footer readout */
    juce::Time   compiledAt[8];
    juce::String rejectLine;                // one deadpan line; empty = ok
    juce::String lockFootCache;             // lock-footer change detector

    /* layout rects computed in resized(), painted in paint() */
    juce::Rectangle<int> rcStrip, rcStripDivA, rcStripDivB,
                         rcSourceCol, rcSourceHead, rcSourceFoot,
                         rcPatchHead,
                         rcExprHead, rcDesignHead, rcDesignArea,
                         rcParamHead, rcParamArea,
                         rcPostCol, rcPostHead, rcPostFoot,
                         rcSeqHead, rcLockFoot;
};

} // namespace morgue
