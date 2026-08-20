#pragma once

#include <JuceHeader.h>

//==============================================================================
/**
    A single, self-contained parametric-EQ band.

    One instance == one filter stage. The processor owns six of these and runs
    the audio through them in series. Putting a band in its own class means the
    filter / coefficient / smoothing logic is written ONCE and reused six times,
    instead of six copy-pasted blocks of filter code.

    A little DSP background (this whole class is built on one idea):
    - A "biquad" is a 2nd-order IIR filter. It has 5 coefficients (b0, b1, b2,
      a1, a2). Those five numbers fully define what the filter does. Every one
      of our five filter types is a biquad; only the coefficient VALUES differ.
    - We don't derive those numbers by hand. juce::dsp::IIR::Coefficients has
      factory helpers (makePeakFilter, makeLowShelf, ...) that do the textbook
      math for us and hand back a ready-made coefficient set.
    - "2nd-order" == 12 dB/octave slope, which is exactly what the spec's
      high-pass and low-pass ask for.
*/
class EQBand
{
public:
    /** The filter shapes a band can take. The integer values MUST match
        the order of the choices in the "type" parameter (see the processor).
        New types get APPENDED, never inserted -- the APVTS state stores the
        real index, so appending keeps every saved session/preset loading
        with the type it was saved with. */
    enum class FilterType
    {
        Bell = 0,   // peaking: boost/cut a bump around a centre frequency
        LowShelf,   // boost/cut everything below a frequency
        HighShelf,  // boost/cut everything above a frequency
        HighPass,   // remove lows below the cutoff, 12 dB/oct
        LowPass,    // remove highs above the cutoff, 12 dB/oct
        Notch       // cut a narrow slice around the centre frequency (no gain)
    };

    EQBand() = default;

    /** Allocate filter state and arm the smoothers. Call from prepareToPlay. */
    void prepare (const juce::dsp::ProcessSpec& spec);

    /** Clear the filter's memory (its internal delay elements). */
    void reset();

    /** Push the latest values from the parameters. Cheap: it just hands new
        TARGETS to the smoothers and lets them glide there. Called once per
        block from processBlock. slopeIndex is the raw 0-based "slope" choice
        parameter value (0=12, 1=24, 2=36, 3=48 dB/oct) -- see stageCountFor()
        for how it maps to an actual stage count; only Low-Pass/High-Pass
        cascade at all, every other type always runs a single stage
        regardless of what's passed in. */
    void setParameters (FilterType type, float freqHz, float gainDb, float q, int slopeIndex, bool active);

    /** Advance the smoothers by one block and rebuild the biquad coefficients
        ONCE for the whole block from the freshly-smoothed values. */
    void updateCoefficients (int numSamples);

    /** Run the audio through this band's filter, in place. No-op if inactive. */
    void process (juce::dsp::AudioBlock<float>& block);

    //==========================================================================
    /** Shared coefficient factory. It is STATIC so the UI can build the exact
        same coefficients to draw the response curve, without having to touch a
        live audio object. This keeps the filter math in exactly one place. */
    static juce::dsp::IIR::Coefficients<float>::Ptr
        makeCoefficients (double sampleRate, FilterType type,
                          float freqHz, float gainDb, float q);

    /** The knob's raw range per filter type. For Bell/Low-Shelf/High-Shelf
        this is still literal resonant Q. For Low-Pass/High-Pass the stored
        0.1..1.0 value is no longer fed to the filter directly -- see
        stageQFor() -- it's a 0..1 BLEND FACTOR between a gentle, non-
        peaking knee and the steepest knee a Butterworth cascade can give
        with zero combined passband boost, so turning "Q" up sharpens the
        cutoff instead of adding resonance. Shared by every write path that
        can set Q (creation, scroll-wheel, typed edit, type-switch) so the
        stored/displayed number always matches what stageQFor() expects. */
    static float maxQFor (FilterType type) noexcept
    {
        return (type == FilterType::LowPass || type == FilterType::HighPass) ? 1.0f : 10.0f;
    }

    /** For Low-Pass/High-Pass, the ACTUAL Q to use for cascade stage
        `stageIndex` (0-based, < stageCount). Reinterprets the stored "q"
        (see maxQFor()) as a 0..1 blend between a gentle identical-Q knee
        (0.5, well below any peaking threshold) and the exact per-stage
        Butterworth Q table for this stage count -- the unique set of
        per-stage Qs that makes an Nth-order cascade maximally flat (zero
        combined peaking) no matter how high any ONE stage's own Q climbs
        (an 8th-order/48 dB-oct cascade needs one stage around Q 2.56, yet
        the full 4-stage product still has zero ripple). Every other type,
        or a single-stage Low-Pass/High-Pass, just returns q unchanged --
        stageIndex/stageCount are ignored when type isn't LP/HP. */
    static float stageQFor (FilterType type, int stageIndex, int stageCount, float q) noexcept;

    /** Highest number of cascaded biquad stages a band can run (4 -> 48
        dB/oct on Low-Pass/High-Pass). */
    static constexpr int maxSlopeStages = 4;

    /** How many identical biquad stages the "slope" choice parameter (index
        0-3, i.e. 12/24/36/48 dB/oct) actually maps to for this band's TYPE.
        Only Low-Pass/High-Pass cascade at all -- every other type always
        runs a single stage regardless of the stored slope index, same
        "ignore this control for types it doesn't apply to" convention
        already used for gain on Low-Pass/High-Pass. */
    static int stageCountFor (FilterType type, int slopeIndex) noexcept
    {
        if (type != FilterType::LowPass && type != FilterType::HighPass)
            return 1;
        return juce::jlimit (1, maxSlopeStages, slopeIndex + 1);
    }

private:
    using Filter       = juce::dsp::IIR::Filter<float>;
    using Coefficients = juce::dsp::IIR::Coefficients<float>;

    // Up to maxSlopeStages identical biquads cascaded in series -- Low-Pass/
    // High-Pass with a steeper "slope" setting run more of these (each one
    // 12 dB/oct); every other type only ever uses stages[0]. All stages
    // share the SAME coefficients (the "simple" cascade approach: repeating
    // one Q rather than a true multi-pole Butterworth Q table per stage --
    // easier to reason about, and the composite curve maths matches this
    // exactly since identical stages just multiply the single-stage dB
    // figure by the stage count). ProcessorDuplicator wraps a MONO filter
    // and transparently runs one copy per channel, all sharing a single
    // coefficient set (`filter.state`) -- that's how one biquad becomes
    // stereo (or N-channel) processing.
    std::array<juce::dsp::ProcessorDuplicator<Filter, Coefficients>, maxSlopeStages> stages;
    int stageCount = 1;

    double     sampleRate = 44100.0;
    FilterType type       = FilterType::Bell;
    bool       active     = false;

    //==========================================================================
    // Smoothing: every parameter move is RAMPED, never applied instantly, so
    // the coefficients evolve gradually and we never hear "zipper noise" (the
    // buzzy stair-stepping you get when a value jumps in discrete steps).
    //
    // Frequency and Q use MULTIPLICATIVE smoothing: we hear pitch and Q
    // logarithmically, so a geometric glide (100->200 feels the same size as
    // 200->400) sounds natural. Both are always > 0, which is required for
    // multiplicative smoothing (it can't ramp through or to zero).
    //
    // Gain uses LINEAR smoothing, in decibels: dB is already a log scale, and
    // gain legitimately passes through 0 dB and goes negative, which
    // multiplicative smoothing cannot represent.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedFreq;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>         smoothedGainDb;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> smoothedQ;

    JUCE_LEAK_DETECTOR (EQBand)
};
