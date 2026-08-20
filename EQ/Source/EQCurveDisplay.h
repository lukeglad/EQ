#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>

//==============================================================================
/**
    The interactive EQ curve.

    - Draws the COMBINED magnitude response of every "on" band by summing each
      band's response (in dB) across the frequency axis.
    - Draws one draggable handle per on-band. Drag horizontally = frequency,
      vertically = gain, scroll-wheel = Q.
    - It never talks to the DSP objects directly. It reads and writes the SAME
      AudioProcessorValueTreeState parameters the audio thread uses, so a mouse
      drag is identical to a host automation move (and is recorded as one).
*/
class EQCurveDisplay : public juce::Component,
                       private juce::Timer
{
public:
    explicit EQCurveDisplay (EQAudioProcessor&);
    ~EQCurveDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Live cursor-frequency readout in the Hz axis row (see paint()) --
    // these just track where the mouse is; the 30Hz timer repaint picks the
    // position up, so no extra repaint plumbing.
    // The immediate repaint() here (not just the 30Hz timer) is what makes
    // cursor-driven drawing -- the readout, the handles' magnetic swell --
    // track at INPUT rate instead of stepping at 30Hz: repaints coalesce to
    // the display's own refresh, so this only costs extra paints while the
    // mouse is actually moving over the graph, and nothing while idle.
    void mouseMove (const juce::MouseEvent& e) override
    {
        hoverPos = e.position;
        repaint();
    }
    void mouseExit (const juce::MouseEvent&) override { hoverPos = { -1.0f, -1.0f }; repaint(); }

    int  getSelectedBand() const noexcept { return selectedBand; }
    void setSelectedBand (int band);

    // Editor listens to this to refresh the text readout / panel.
    std::function<void()> onSelectionChanged;

    // The band-shape glyph (bell / shelf / pass / notch), drawn as a mini
    // response curve. Public + static because the shape control now lives in
    // the editor's readout pill, not on this graph -- see the strip comment
    // in the .cpp. Forwards to the drawing code shared with the type menu's
    // own rows, so all three places draw the identical glyph.
    static void drawTypeGlyph (juce::Graphics&, EQBand::FilterType, juce::Rectangle<float>);

    // Opens the filter-type menu for `band`, anchored at `screenAnchor`
    // (screen coords). The anchor is passed in rather than derived here
    // because the control that opens this is no longer on this component.
    void showTypeMenu (int band, juce::Rectangle<int> screenAnchor);

    // Public because the readout pill's delete control (the X beside the
    // values -- editor-side) removes the selected band through here; also
    // still what right-clicking a dot and the old strip cell used. One
    // deletion path, whatever the trigger.
    void removeBand (int band);

    // True while a band is being AUDITIONED by holding the headphones cell
    // (momentary -- see listenHoldActive). Everything that isn't the sound
    // gets out of the way for the duration: the readout pill hides (editor
    // side), and the strip drops to just the band's frequency, since sweeping
    // by ear is the whole point of the gesture and the icons are furniture
    // you can't click while the button is already held.
    bool isAuditioning() const noexcept { return listenHoldActive; }

    // True when `pos` (in this component's coords) lands on something a
    // click can grab: a band dot or a dynamic-range arrow. The readout pill
    // uses this to decide whether a click on ITS glass should be forwarded
    // down here (see ReadoutPill's mouse handlers) -- targets stay
    // reachable under the frost, while clicks on plain pill background
    // stay dead instead of falling through and CREATING a band.
    bool hasGrabbableAt (juce::Point<float> pos) const
    {
        return bandAtPosition (pos) >= 0 || dynHandleAtPosition (pos) >= 0;
    }

    // True while any band drag is live (dot or dynamic arrow). The editor
    // uses it to let a drag override the readout's cursor-over-pill freeze
    // -- a drag forwarded through the pill would otherwise show the OLD
    // band's numbers the whole time.
    bool isDraggingBand() const noexcept
    {
        return draggingBand >= 0 || draggingDynBand >= 0;
    }

    // Which band the cursor is currently over (or whose hover strip it's
    // inside), -1 for none. The editor shows THIS band's values in the
    // readout pill when set, so you can inspect a band without selecting
    // it -- see PluginEditor::displayedBand.
    int getHoverBand() const { return hoverStripBand(); }

    // Pushed whenever the cursor enters or leaves one of this graph's own
    // hoverable controls (the action-strip cells, the dB-range bubble) --
    // the editor routes it into the readout pill, which doubles as a hint
    // line. Empty string means "nothing hovered". Band DOTS deliberately
    // don't emit hints; they show their values instead (see getHoverBand).
    std::function<void (juce::String)> onHintChanged;

    // A small thin black "+" crosshair, generated procedurally (no external
    // image asset) and applied via setMouseCursor() in the constructor --
    // scoped to this component alone, so every OTHER control (knobs,
    // dropdowns, bypass toggle, etc.) keeps the normal OS arrow cursor.
    static juce::MouseCursor makeCrosshairCursor();

    //==========================================================================
    // Vertical-scale (view-only) control. This used to be a hand-painted
    // overlay drawn directly on the graph; it's now a real juce::ComboBox
    // owned by PluginEditor, living in the bottom control row instead. These
    // three let that external control read/drive the same state the graph's
    // own gainToY/yToGain still depend on for its axis mapping.
    static juce::StringArray getRangePresetChoices();
    int  getRangePresetIndex() const noexcept { return rangePresetIndex; }
    void setRangePresetIndex (int index);

private:
    // The dB range selector's on-graph "bubble" (top-left corner): a pill
    // showing the current range, with the same magnetic proximity swell as
    // the band handles; clicking it pops the 6/12/30 dB menu. Replaces the
    // ComboBox that used to sit in the editor's bottom row.
    juce::Rectangle<float> rangeBubbleBounds() const noexcept { return { 10.0f, 8.0f, 48.0f, 17.0f }; }
    void showRangeMenu();

    // Hover-revealed strips floating above a band's dot -- the band's
    // controls live ON the graph, not in the bottom panel. Invisible until
    // the cursor nears a dot, then:
    // the ACTION strip appears just above it (or below, for dots under the
    // graph's centre line): the band's SHAPE glyph (click opens the type
    // menu, which also nests each pass filter's slope), power/mute,
    // headphones/listen, X/delete. hoverStripBand() decides WHICH band's
    // strip is live: the nearest hovered ON dot, or the band whose strip
    // rect the cursor is already inside (so the pointer can travel dot ->
    // strip without it vanishing en route). Clicks are hit-tested in
    // mouseDown() BEFORE band grab/create logic.
    //
    // (A second SLOPE strip of 12/24/36/48 knee glyphs used to stack above
    // this one for HP/LP bands -- eight floating glyphs at once read as
    // cluttered and cryptic, and its knees looked near-identical to the
    // shape glyph. Slope moved into the type menu instead.)
    int hoverStripBand() const;

    // Works out what (if anything) under the cursor deserves a hint, and
    // fires onHintChanged when that changes. Called from timerCallback --
    // deliberately NOT from paint(), which must stay side-effect free.
    void updateHint();

    // Steps the vertical range up one preset while a drag pushes a gain
    // value past the visible edge -- see the definition for the full rules.
    void expandRangeToShow (float gainDb);

    // Advances verticalRangeDb toward the current preset each tick (display
    // -only smoothing; the preset index is the real state). In the .cpp
    // because kRangePresets lives in its anonymous namespace.
    void tickRangeGlide();

    // THE one way the hint line is written -- both the polled hover hints
    // (updateHint) and the type menu's row names go through here, so
    // lastHint always reflects what the pill is actually showing and the
    // two can't get out of step.
    void pushHint (juce::String h);
    juce::String lastHint;

    // True while the type menu is open: it owns the hint line, and the
    // polled hover hint stands down (the cursor is over the menu's own
    // window by then, which would otherwise read as "nothing hovered").
    bool menuOwnsHint = false;
    juce::Rectangle<float> actionStripRect (int band) const;

    // The type menu (Bell/shelves/HP/LP/Notch), opened by the readout
    // pill's shape control. Replaces the row-1
    // ComboBox; choice names come from the "type" parameter itself, so the
    // list can never drift from what the DSP actually offers. The old
    // dropdown's Q-clamp side effect (HP/LP cap Q at maxQFor) migrated
    // into the selection callback.

public:

    // Purely cosmetic background dim (0 = normal white, 1 = fully dimmed),
    // driven by PluginEditor's bypass-fade animation so the graph's own
    // background darkens in lockstep with the control section's. Only the
    // background blends -- the curve/points/labels stay at full contrast.
    void setBackgroundDim (float amount);


private:
    // ~30 Hz: pull/FFT/smooth every analyzer instance (message-thread work),
    // then repaint — which also keeps the curve live vs. automation +
    // parameter smoothing.
    void timerCallback() override
    {
        proc.getAnalyzer().update();
        proc.getAnalyzerLow().update();
        proc.getAnalyzerPost().update();
        proc.getAnalyzerPostLow().update();

        // selectedBand is plain UI state, not an APVTS parameter -- undo/
        // redo can turn the selected band's "on" back off directly via the
        // ValueTree without ever going through removeBand()/setSelectedBand
        // (those are only called by OUR OWN click/delete code), leaving
        // selectedBand stale: still pointing at a real index, so row 1 stays
        // "selected" and visible, but showing nothing real since the band it
        // points to isn't actually on any more. Self-heals here every tick
        // instead of only being correct for direct delete-button/right-click
        // deletions.
        if (selectedBand >= 0 && ! bandOn (selectedBand))
            setSelectedBand (-1);

        // Hover strips fade (action + slope; see hoverStripAlpha): ease toward
        // shown while a band's strips are live, toward hidden otherwise --
        // slightly faster in than out, so it feels responsive appearing and
        // gentle leaving.
        {
            const int sb = hoverStripBand();
            if (sb >= 0)
                hoverStripShownBand = sb;

            updateSlopeExpansion (sb);


            const float target = sb >= 0 ? 1.0f : 0.0f;
            const float rate   = target > hoverStripAlpha ? 0.30f : 0.22f;
            hoverStripAlpha += (target - hoverStripAlpha) * rate;

            if (sb < 0 && hoverStripAlpha < 0.02f)
                hoverStripShownBand = -1;
        }

        // Listen-wall glide (see listenWallQ's declaration): chase the real
        // Q so the walls slide between wheel-notch steps instead of
        // jumping. Log-domain lerp -- Q is multiplicative, so this glides
        // evenly across the whole range. Snap (not glide) when the
        // listened band changes, including engage from idle.
        {
            const int lb = proc.getListenBand();
            if (lb != listenWallBand)
            {
                listenWallBand = lb;
                listenWallQ    = lb >= 0 ? bandQ (lb) : -1.0f;
            }
            else if (lb >= 0 && listenWallQ > 0.0f)
            {
                listenWallQ *= std::pow (bandQ (lb) / listenWallQ, 0.35f);
            }
        }

        tickRangeGlide();

        updateHint();
        repaint();
    }

    // The graph's actual background colour right now (white, blended toward
    // darkgrey by backgroundDim while bypassed). Anything drawn in plain
    // white specifically to LOOK LIKE background -- unselected point fill,
    // the pre/post diff-ribbon's "erase toward background" colours -- should
    // use this instead of a hardcoded juce::Colours::white, or those bits
    // stay stark white and stand out once the real background dims.
    juce::Colour currentBackgroundColour() const;

    // Blends the fast (mids/highs) and slow (low-end, finer bin spacing)
    // PRE-EQ analyzer instances into one dB reading for a frequency range,
    // crossfading smoothly around a crossover frequency so there's no seam
    // between them, then applies the cosmetic display tilt.
    float analyzerDb (float freqLo, float freqHi) const;

    // Same tilt, applied to the single POST-EQ analyzer instance (no dual-FFT
    // blend needed there — see EQAudioProcessor::analyzerPost).
    float analyzerPostDb (float freqLo, float freqHi) const;

    //==========================================================================
    // Axis mapping. X is logarithmic in frequency (equal space per octave);
    // Y is linear in decibels, 0 dB at the vertical centre. Both map across
    // the FULL component bounds — grid/curve run edge-to-edge (matching
    // Pro-Q's own layout) rather than stopping short for a label margin.
    // Axis labels are drawn ON TOP of the plot instead (see drawAxisLabel()).
    float freqToX (float freqHz) const;
    float xToFreq (float x) const;
    float gainToY (float gainDb) const;
    float yToGain (float y) const;

    // Draws one axis label (bare text, no backdrop) at the given box.
    void drawAxisLabel (juce::Graphics& g, const juce::String& text,
                       juce::Rectangle<int> box, juce::Justification just) const;

    juce::Point<float> pointForBand (int band) const;
    int   bandAtPosition (juce::Point<float> pos) const;

    // The band whose DYNAMIC handle (the hollow ring at static gain + range)
    // is under `pos`, or -1. Loses to bandAtPosition's dot on ties, so a
    // range dragged to 0 leaves the static dot grabbable.
    int   dynHandleAtPosition (juce::Point<float> pos) const;

    // Click-to-create / right-click-to-remove band management.
    int   findUnusedBandNearest (float freqHz) const; // -1 if all six are placed
    void  createBandAt (int band, juce::Point<float> pos);

    //==========================================================================
    // Small typed reads of the shared parameter state.
    bool  bandOn   (int b) const;
    bool  bandMuted (int b) const;
    EQBand::FilterType bandType (int b) const;
    float bandFreq (int b) const;
    float bandGain (int b) const;
    float bandQ    (int b) const;
    int   bandSlope (int b) const; // raw 0-based "slope" choice index -- see EQBand::stageCountFor()
    bool  bandIsPass (int b) const; // High-Pass or Low-Pass -- the types that carry a slope

    // How many cells the band's action strip currently shows: 4 while the
    // slope picker is expanded, 3 for a pass band at rest (slope + mute +
    // listen), 2 for everything else (mute + listen).
    int stripCellCount (int b) const;

    // Which slope choice the expanded picker's cell `cell` stands for. A
    // High-Pass reads 12..48 left to right, a Low-Pass 48..12 -- so the two
    // pickers are true mirror images and each one's STEEP end sits on the
    // side its own filter falls toward, where that band's collapsed handle
    // already lives. THE one place this mapping exists: paint, the click
    // dispatch and the hint all go through it, so they can't disagree.
    int slopeForPickerCell (int band, int cell) const;

    // The strip's slope cell EXPANDS on hover into the four slope choices,
    // replacing mute/listen until the cursor leaves the strip -- a hover-
    // reveal spin on the old always-on slope strip (which was rejected as
    // "cluttered and cryptic": eight floating glyphs at once). The type
    // menu's submenu remains the other way to set slope. Managed from the
    // timer via updateSlopeExpansion(); -1 = collapsed.
    int  slopeExpandedBand = -1;
    void updateSlopeExpansion (int liveStripBand);
    static bool typeHasGain (EQBand::FilterType t);
    bool  bandDynOn (int b) const;
    float bandDynRange (int b) const;

    // Where a band's dynamic handle sits: its freq, at static gain + range,
    // clamped to the visible graph exactly like pointForBand.
    juce::Point<float> dynHandlePoint (int b) const;

    // True when this band draws dynamic furniture at all: on, unmuted,
    // dynamics engaged, and a type that has gain to modulate.
    bool bandShowsDynamics (int b) const;

    // Parameter writes routed through the host (so they automate + undo).
    void setBandParam (int b, juce::StringRef name, float value);
    void beginGesture (int b, juce::StringRef name);
    void endGesture   (int b, juce::StringRef name);

    //==========================================================================
    EQAudioProcessor& proc;

    // Cursor position while the mouse is over this component, (-1,-1)
    // otherwise (see mouseMove/mouseExit above). Drives the axis-row
    // frequency readout AND the handles' magnetic proximity swell, both in
    // paint().
    juce::Point<float> hoverPos { -1.0f, -1.0f };

    // Smoothed visibility for the hover strips (action + slope together --
    // see timerCallback()) -- a hard show/hide read as a jump, so they ease
    // in/out instead. hoverStripShownBand latches the LAST live band so the
    // fade-OUT keeps drawing at that band's position after hoverStripBand()
    // has already gone back to -1.
    float hoverStripAlpha    = 0.0f;
    int   hoverStripShownBand = -1;



    // True while the action strip's headphones cell is being HELD --
    // listen/audition is momentary ("click and hold to use the feature,
    // once you unclick you snap out"), engaged in mouseDown and released in
    // mouseUp (and defensively in the destructor, so the audition can't
    // stick on if the editor closes mid-hold). While held, DRAGGING sweeps
    // the band's frequency (the classic hunt-the-resonance workflow --
    // "you need to be able to sweep the band frequency"): listenHoldBand is
    // the band being swept, and listenHoldXOffset is the gap between where
    // the hold started and the band's own x at that moment, subtracted
    // during the drag so the frequency doesn't jump to the cursor on the
    // first movement. The freq write is wrapped in one gesture spanning the
    // whole hold (opened in mouseDown, closed in mouseUp), so a sweep
    // records as a single automation move, same as a normal dot drag.
    bool  listenHoldActive  = false;
    int   listenHoldBand    = -1;
    float listenHoldXOffset = 0.0f;

    // Display-smoothed Q for the listen "walls" (the bandwidth markers +
    // vignette edges): the REAL Q parameter steps a full 1.25x per wheel
    // notch (deliberate -- see mouseWheelMove), which made the walls jump
    // in ticks while auditioning; drawing them from this glided copy
    // instead makes them chase each step smoothly ("i want the isolation
    // walls to move smoothly"). Updated in timerCallback() (log-domain
    // lerp, since Q is a multiplicative parameter); snapped, not glided,
    // whenever the listened band changes so engaging never animates in
    // from a stale value. -1 = inactive.
    float listenWallQ    = -1.0f;
    int   listenWallBand = -1;

    int selectedBand = -1;
    int draggingBand = -1;

    // Band whose dynamic-range handle is being dragged (-1 = none). Vertical
    // only: the handle is locked to its band's frequency.
    int draggingDynBand = -1;

    static constexpr float minFreq   = 20.0f;
    static constexpr float maxFreq   = 20000.0f;
    // Was 10px (kept the curve/gridlines shy of the top/bottom edge) — set to
    // 0 so the vertical extremes (+/-range) sit flush against the true
    // top/bottom edge, matching how the frequency axis already runs flush
    // left/right with no equivalent margin.
    static constexpr float vMargin   = 0.0f;

    // Vertical range shown on the graph. This is a VIEW preference only — it
    // is plain component state, not an APVTS parameter, so it is never saved
    // with the plugin and simply reinitialises to the 12 dB default every
    // time a fresh editor is constructed (matching Pro-Q's behaviour).
    int   rangePresetIndex = 1;          // index into kRangePresets; 1 == 12 dB
    float verticalRangeDb  = 12.0f;      // current +/- range mapped to the graph's height

    float backgroundDim = 0.0f;          // 0 = normal, 1 = fully dimmed (bypassed) -- see setBackgroundDim()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQCurveDisplay)
};
