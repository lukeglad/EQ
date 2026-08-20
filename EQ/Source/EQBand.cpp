#include "EQBand.h"

namespace
{
    // Exact per-stage Q for stage `stageIndex` (0-based) of an Nth-order
    // (== stageCount 2nd-order sections) Butterworth cascade -- the pole-
    // angle formula every textbook Butterworth Q table comes from.
    float butterworthStageQ (int stageCount, int stageIndex) noexcept
    {
        const float theta = juce::MathConstants<float>::pi * (float) (2 * stageIndex + 1)
                           / (4.0f * (float) stageCount);
        return 1.0f / (2.0f * std::cos (theta));
    }
}

float EQBand::stageQFor (FilterType type, int stageIndex, int stageCount, float q) noexcept
{
    if (type != FilterType::LowPass && type != FilterType::HighPass)
        return q; // Bell/shelves: q is still literal resonance, unchanged

    constexpr float gentleQ = 0.5f;
    const float qMin = 0.1f, qMax = maxQFor (type);
    const float t = juce::jlimit (0.0f, 1.0f, (q - qMin) / (qMax - qMin));
    const float steepQ = butterworthStageQ (stageCount, stageIndex);
    return gentleQ + t * (steepQ - gentleQ);
}

//==============================================================================
void EQBand::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    for (auto& s : stages)
        s.prepare (spec);

    // ~50 ms glide: long enough to be inaudible as a step, short enough that
    // the control still feels immediate. reset() ties the ramp length to the
    // sample rate so it lasts 50 ms regardless of 44.1k / 48k / 96k.
    constexpr double rampSeconds = 0.05;
    smoothedFreq  .reset (sampleRate, rampSeconds);
    smoothedGainDb.reset (sampleRate, rampSeconds);
    smoothedQ     .reset (sampleRate, rampSeconds);

    // Valid starting points. Freq and Q must be > 0 for multiplicative
    // smoothing; these get overwritten by real parameter values on block 1.
    smoothedFreq  .setCurrentAndTargetValue (1000.0f);
    smoothedGainDb.setCurrentAndTargetValue (0.0f);
    smoothedQ     .setCurrentAndTargetValue (1.0f);

    reset();

    // Give every stage a valid initial coefficient set so the first block is
    // well-defined even before setParameters() is ever called.
    auto initial = makeCoefficients (sampleRate, type, 1000.0f, 0.0f, 1.0f);
    for (auto& s : stages)
        *s.state = *initial;
}

void EQBand::reset()
{
    for (auto& s : stages)
        s.reset();
}

//==============================================================================
void EQBand::setParameters (FilterType newType, float freqHz, float gainDb, float q, int slopeIndex, bool isActive)
{
    // Off -> on transition: flush the filter's delay registers. While a band
    // is off, process() skips it entirely, freezing its internal state with
    // whatever audio was passing through when it was last active -- turning
    // it back on later would discharge that stale energy as a burst ringing
    // at the filter's own frequency. Inaudible-to-subtle in the mix, but the
    // POST analyzer (which alone sees it -- the filter itself creates it)
    // caught it and painted a phantom boost-ribbon spike at a freshly
    // re-added band's frequency. Just clearing floats: RT-safe.
    if (isActive && ! active)
        reset();

    type       = newType;
    active     = isActive;
    stageCount = stageCountFor (type, slopeIndex);

    // Hand the smoothers their new targets; they glide there over ~50 ms.
    // Nothing snaps here — this is the whole point of routing through
    // SmoothedValue instead of writing the raw values straight into the filter.
    smoothedFreq  .setTargetValue (juce::jmax (20.0f, freqHz));
    smoothedGainDb.setTargetValue (gainDb);
    smoothedQ     .setTargetValue (juce::jmax (0.1f,  q));
}

void EQBand::updateCoefficients (int numSamples)
{
    // Advance every smoother across the WHOLE block in one call (skip) rather
    // than sample-by-sample. skip() returns the value the parameter has reached
    // at the end of the block, and we use that single value to build ONE set of
    // coefficients for the entire block.
    //
    // Why once per block and not per sample? Building biquad coefficients runs
    // trig, divides and a sqrt — dozens of FLOPs. A block is only ~1-10 ms,
    // far shorter than our 50 ms glide, so per-block updates already sound
    // perfectly smooth while costing roughly 100x less than rebuilding the
    // coefficients on every single sample.
    const float f = smoothedFreq  .skip (numSamples);
    const float g = smoothedGainDb.skip (numSamples);
    const float q = smoothedQ     .skip (numSamples);

    // Inactive bands still advance their smoothers above, so that when the band
    // is switched back on its values have kept tracking and it never "jumps".
    if (! active)
        return;

    // Each stage gets its OWN Q via stageQFor() -- for Low-Pass/High-Pass
    // with more than one stage these differ per stage (the Butterworth
    // table), not identical copies any more. Copy INTO each shared state
    // object (*state = *newCoefs) rather than reassigning the Ptr, so every
    // channel of every duplicator reads from its own stable object and all
    // of them update together and in lock-step. Updates all maxSlopeStages
    // entries (not just the currently-active stageCount) since it's cheap
    // and avoids ever leaving a stale coefficient set sitting in a stage
    // that a later slope change might reactivate.
    for (int i = 0; i < maxSlopeStages; ++i)
    {
        const float qi = stageQFor (type, i, stageCount, q);
        auto newCoefs = makeCoefficients (sampleRate, type, f, g, qi);
        *stages[(size_t) i].state = *newCoefs;
    }
}

void EQBand::process (juce::dsp::AudioBlock<float>& block)
{
    if (! active)
        return; // true bypass: the audio passes untouched to the next band

    juce::dsp::ProcessContextReplacing<float> context (block);
    for (int i = 0; i < stageCount; ++i)
        stages[(size_t) i].process (context);
}

//==============================================================================
juce::dsp::IIR::Coefficients<float>::Ptr
EQBand::makeCoefficients (double sampleRate, FilterType type,
                          float freqHz, float gainDb, float q)
{
    // Keep the frequency safely below Nyquist (sampleRate / 2). A biquad whose
    // corner sits at or above Nyquist produces garbage / NaNs.
    freqHz = juce::jlimit (20.0f, (float) (sampleRate * 0.5) - 1.0f, freqHz);
    // Sane-range failsafe only -- NOT the old literal Low-Pass/High-Pass
    // resonance cap. Multi-stage Butterworth per-stage Qs legitimately run
    // above 1.0 (a 48 dB/oct cascade needs one stage around Q 2.56) and
    // still combine to zero peaking; clamping to maxQFor() here would
    // silently break that cancellation. Every caller already hands this a
    // safe value -- literal q for Bell/shelves, or stageQFor()'s output for
    // Low-Pass/High-Pass -- this just guards against something absurd/NaN.
    q = juce::jlimit (0.1f, 20.0f, q);

    // The peak/shelf helpers want a LINEAR gain factor, not decibels:
    // 0 dB -> 1.0, +6 dB -> ~2.0, -6 dB -> ~0.5.
    const float gainFactor = juce::Decibels::decibelsToGain (gainDb);

    using C = juce::dsp::IIR::Coefficients<float>;
    switch (type)
    {
        case FilterType::LowShelf:  return C::makeLowShelf  (sampleRate, freqHz, q, gainFactor);
        case FilterType::HighShelf: return C::makeHighShelf (sampleRate, freqHz, q, gainFactor);
        case FilterType::HighPass:  return C::makeHighPass  (sampleRate, freqHz, q);
        case FilterType::LowPass:   return C::makeLowPass   (sampleRate, freqHz, q);
        case FilterType::Notch:     return C::makeNotch     (sampleRate, freqHz, q);
        case FilterType::Bell:
        default:                    return C::makePeakFilter (sampleRate, freqHz, q, gainFactor);
    }
}
