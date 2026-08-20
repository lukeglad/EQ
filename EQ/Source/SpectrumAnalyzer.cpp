#include "SpectrumAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

//==============================================================================
SpectrumAnalyzer::SpectrumAnalyzer (int fftOrderToUse)
    : fftOrder (fftOrderToUse),
      fftSize  (1 << fftOrder),
      numBins  (fftSize / 2),
      hopSize  (fftSize / 4),
      fft      (fftOrder),
      window   ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann),
      circularBuffer ((size_t) fftSize, 0.0f),
      fftData         ((size_t) (2 * fftSize), 0.0f),
      workData        ((size_t) (2 * fftSize), 0.0f),
      rawDb                ((size_t) numBins, floorDb),
      octaveDb             ((size_t) numBins, floorDb),
      smoothedDb           ((size_t) numBins, floorDb),
      releaseRatePerSecond ((size_t) numBins, 0.0f)
{
}

void SpectrumAnalyzer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    // Clear everything so a fresh start doesn't briefly show the last signal.
    std::fill (rawDb.begin(), rawDb.end(), floorDb);
    std::fill (octaveDb.begin(), octaveDb.end(), floorDb);
    std::fill (smoothedDb.begin(), smoothedDb.end(), floorDb);
    std::fill (releaseRatePerSecond.begin(), releaseRatePerSecond.end(), 0.0f);
    std::fill (circularBuffer.begin(), circularBuffer.end(), 0.0f);
    writePos = 0;
    samplesSinceLastHop = 0;
    lastBlockMs = 0;
    lastUpdateMs = 0;
    wasStarved = false;
    nextFFTBlockReady.store (false);
}

//==============================================================================
// --- audio thread -----------------------------------------------------------
void SpectrumAnalyzer::pushSample (float s) noexcept
{
    circularBuffer[(size_t) writePos] = s;
    writePos = (writePos + 1) % fftSize;

    // Publish every hopSize samples (75% overlap) rather than waiting for a
    // full non-overlapping window — see the class comment for why this is
    // what makes the display move fluidly instead of stepping between
    // snapshots. Skipped (dropped, harmless for a display) if the message
    // thread hasn't consumed the previous window yet.
    if (++samplesSinceLastHop >= hopSize)
    {
        samplesSinceLastHop = 0;

        if (! nextFFTBlockReady.load())
        {
            // writePos now points at the OLDEST sample in the buffer (the next
            // one that would be overwritten) — copy from there, wrapping once,
            // to get the last fftSize samples out in correct time order.
            const int firstPartLen = fftSize - writePos;
            std::memcpy (fftData.data(), circularBuffer.data() + writePos,
                        sizeof (float) * (size_t) firstPartLen);
            std::memcpy (fftData.data() + firstPartLen, circularBuffer.data(),
                        sizeof (float) * (size_t) writePos);
            nextFFTBlockReady.store (true);
        }
    }
}

void SpectrumAnalyzer::pushBlock (const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numCh      = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numCh <= 0)
        return;

    const float* left  = buffer.getReadPointer (0);
    const float* right = numCh > 1 ? buffer.getReadPointer (1) : nullptr;

    // Mono-sum L+R (or pass mono straight through) — an analyzer only needs one
    // channel and this halves the FFT work.
    for (int i = 0; i < numSamples; ++i)
        pushSample (right != nullptr ? 0.5f * (left[i] + right[i]) : left[i]);
}

//==============================================================================
// --- message thread ---------------------------------------------------------
void SpectrumAnalyzer::update()
{
    const juce::uint32 now = juce::Time::getMillisecondCounter();

    if (nextFFTBlockReady.load())
    {
        // Copy the published block out FAST, then release the flag so the audio
        // thread can keep producing while we do the (slower) FFT on our copy.
        std::memcpy (workData.data(), fftData.data(), sizeof (float) * (size_t) fftSize);
        nextFFTBlockReady.store (false);

        std::fill (workData.begin() + fftSize, workData.end(), 0.0f);
        window.multiplyWithWindowingTable (workData.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (workData.data());

        // Bin magnitudes -> dBFS. The 2/fftSize factor lands a full-scale sine
        // near the top of the range; exact calibration isn't critical for a
        // visual analyzer, the SHAPE and peak frequency are what matter.
        for (int b = 0; b < numBins; ++b)
            rawDb[(size_t) b] = juce::Decibels::gainToDecibels (
                workData[(size_t) b] * 2.0f / (float) fftSize, floorDb);

        applyOctaveSmoothing(); // rawDb -> octaveDb (frequency-domain smoothing)

        lastBlockMs = now;
    }

    // No fresh audio for a while == silence/stopped transport: aim everything
    // at the floor so the display settles to the bottom instead of freezing.
    const bool starved = (now - lastBlockMs) > starvationMs;

    // The MOMENT playback stops (not every starved frame after) -- snapshot
    // every bin's CURRENT value and give it a fresh release rate sized to
    // reach the floor in exactly releaseTimeToZeroSeconds from THIS instant.
    // Without this, each bin keeps falling on whatever rate it last captured
    // from a real (asynchronous, per-bin) musical peak, so bins finish their
    // fall at scattered times -- this makes them all start and finish
    // together, so the whole spectrum decays as one shape. Quiet bins still
    // aren't wiped out instantly (the rate is still proportional to that
    // bin's own value at the snapshot), only WHEN each bin's fall begins is
    // now synchronized, not how fast it falls.
    if (starved && ! wasStarved)
        for (int b = 0; b < numBins; ++b)
            releaseRatePerSecond[(size_t) b] =
                juce::Decibels::decibelsToGain (smoothedDb[(size_t) b], floorDb) / releaseTimeToZeroSeconds;
    wasStarved = starved;

    // dt measured from the actual elapsed time since the last update() call,
    // so both the attack coefficient and the linear release rate stay correct
    // regardless of the UI timer's real rate/jitter.
    const float dt = lastUpdateMs == 0
                        ? (1.0f / 30.0f)
                        : juce::jlimit (0.001f, 0.2f, (float) (now - lastUpdateMs) / 1000.0f);
    lastUpdateMs = now;

    // Attack: plain exponential smoothing directly ON THE DB VALUE — see the
    // class comment for why this stays in dB rather than amplitude (doing it
    // in amplitude made ordinary FFT frame-to-frame noise trigger "fast
    // attack" almost constantly, which read as chaotic rather than musical).
    const float attackCoeff = 1.0f - std::exp (-dt / attackTimeSeconds);

    for (int b = 0; b < numBins; ++b)
    {
        const float target = starved ? floorDb : octaveDb[(size_t) b];
        float& val  = smoothedDb[(size_t) b];
        float& rate = releaseRatePerSecond[(size_t) b];

        if (target > val)
        {
            // Rising / new peak: attack in dB as before, then capture THIS
            // peak's own release slope — proportional to its own amplitude,
            // not a fixed full-scale constant — ready for whenever release
            // starts (which may be the very next frame).
            val += attackCoeff * (target - val);
            rate = juce::Decibels::decibelsToGain (val, floorDb) / releaseTimeToZeroSeconds;
        }
        else
        {
            // Falling: straight-line decay in amplitude at THIS bin's own
            // captured rate (not recalculated here — it stays fixed for the
            // whole fall, only refreshed at the next peak).
            const float amp       = juce::Decibels::decibelsToGain (val, floorDb);
            const float targetAmp = juce::Decibels::decibelsToGain (target, floorDb);
            const float newAmp    = juce::jmax (targetAmp, amp - rate * dt);
            float newVal = juce::Decibels::gainToDecibels (newAmp, floorDb);

            // Cap the per-frame dB drop so the tail doesn't plunge then hit
            // the floor as a sudden hard clamp (the linear-amplitude decay
            // above accelerates once expressed in dB — see class comment).
            // The cap itself is derived from THIS BIN's own captured `rate`
            // (i.e. its own last peak level), NOT a shared constant — a
            // fixed global cap would make every bin's terminal glide look
            // identical, which is exactly wrong when audio stops: a bin that
            // peaked loud should keep falling (and take longer to reach the
            // floor) while a bin that peaked quiet settles sooner, same as
            // during normal playback. `rate` already encodes that bin's own
            // peak (rate = peakAmp / releaseTimeToZeroSeconds), so inverting
            // it recovers that peak's dB range above the floor for free.
            const float peakAmp          = rate * releaseTimeToZeroSeconds;
            const float peakDbAboveFloor = juce::Decibels::gainToDecibels (peakAmp, floorDb) - floorDb;
            const float maxBinFallDbPerSecond = juce::jmax (1.0f, peakDbAboveFloor / releaseTimeToZeroSeconds) * tailFallCapFactor;

            newVal = juce::jmax (newVal, val - maxBinFallDbPerSecond * dt);
            val = newVal;
        }
    }
}

void SpectrumAnalyzer::applyOctaveSmoothing()
{
    // For each bin, average every bin within +/- 1/48 octave of it (i.e. a
    // 1/24-octave-wide window centred on it) — a frequency-PROPORTIONAL
    // smoothing width, not a fixed number of bins, so the effect scales
    // correctly across the spectrum: a few neighbours at low frequencies,
    // many more at high frequencies (where far more linear FFT bins pack
    // into the same musical interval). This is what blends closely-spaced
    // harmonics into rounded bumps instead of a forest of separate spikes.
    static const float ratio = std::pow (2.0f, octaveSmoothingFraction * 0.5f);

    for (int b = 0; b < numBins; ++b)
    {
        const float centreFreq = (float) b * (float) sampleRate / (float) fftSize;

        if (centreFreq <= 0.0f)
        {
            octaveDb[(size_t) b] = rawDb[(size_t) b];
            continue;
        }

        int loBin = juce::jlimit (0, numBins - 1,
            (int) std::floor (centreFreq / ratio * (float) fftSize / (float) sampleRate));
        int hiBin = juce::jlimit (0, numBins - 1,
            (int) std::ceil  (centreFreq * ratio * (float) fftSize / (float) sampleRate));

        // The octave-proportional window above is narrower, in absolute Hz,
        // at low frequencies than at high ones — near the bottom of the
        // spectrum it can collapse to just 1-2 bins, leaving raw per-bin
        // noise showing through as a slight wobble instead of being smoothed.
        // Widen it (symmetrically, re-clamped) to a minimum bin count so
        // there's always some real averaging happening, even down low.
        //
        // The floor is defined as a fixed Hz BANDWIDTH (minAverageBandwidthHz),
        // not a fixed bin count, and converted to bins per-instance here. This
        // matters now that two differently-sized FFT instances share this
        // code (see the dual fast/slow analyzer setup): a fixed BIN count
        // would cover half the Hz range on the finer-resolution ("slow")
        // instance, letting it reveal narrow spectral notches at much fuller
        // depth than the coarser ("fast") instance smooths away — visible as
        // a sharp dip right in the crossfade zone between the two. Sharing
        // the same absolute Hz floor keeps both instances comparably smooth.
        const int minBinsToAverage = juce::jmax (1,
            (int) std::ceil (minAverageBandwidthHz / ((float) sampleRate / (float) fftSize)));
        if (hiBin - loBin + 1 < minBinsToAverage)
        {
            const int extra = minBinsToAverage - (hiBin - loBin + 1);
            // Clamp loBin so loBin..loBin+minBinsToAverage-1 always stays
            // fully in range — clamping loBin and hiBin separately could
            // shrink the window right back below the minimum near the very
            // top of the spectrum.
            loBin = juce::jlimit (0, numBins - minBinsToAverage, loBin - extra / 2);
            hiBin = loBin + minBinsToAverage - 1;
        }

        // Average in power, same reasoning as getDbForFrequencyRange: dB
        // values shouldn't be averaged directly, since that under-weights
        // the loudest bin in the window.
        double sumSquares = 0.0;
        for (int k = loBin; k <= hiBin; ++k)
        {
            const float amp = juce::Decibels::decibelsToGain (rawDb[(size_t) k], floorDb);
            sumSquares += (double) amp * (double) amp;
        }

        const float meanSquare = (float) (sumSquares / (double) (hiBin - loBin + 1));
        octaveDb[(size_t) b] = juce::Decibels::gainToDecibels (std::sqrt (meanSquare), floorDb);
    }
}

float SpectrumAnalyzer::getDbForFrequency (float freqHz) const noexcept
{
    if (sampleRate <= 0.0)
        return floorDb;

    // Which (fractional) FFT bin does this frequency fall on, and interpolate
    // between the two nearest bins for a smooth read across the log axis.
    const float binF = freqHz * (float) fftSize / (float) sampleRate;
    if (binF <= 0.0f)
        return smoothedDb[0];

    const int b0 = (int) binF;
    if (b0 >= numBins - 1)
        return smoothedDb[(size_t) (numBins - 1)];

    const float frac = binF - (float) b0;
    return smoothedDb[(size_t) b0]
         + frac * (smoothedDb[(size_t) (b0 + 1)] - smoothedDb[(size_t) b0]);
}

float SpectrumAnalyzer::getDbForFrequencyRange (float freqLo, float freqHi) const noexcept
{
    if (sampleRate <= 0.0)
        return floorDb;

    if (freqHi < freqLo)
        std::swap (freqLo, freqHi);

    const float binLo = freqLo * (float) fftSize / (float) sampleRate;
    const float binHi = freqHi * (float) fftSize / (float) sampleRate;

    // Less than one bin covered (typical at low frequencies, where the log
    // axis spaces pixels out further than the FFT's bin spacing) — a range
    // average would just be a single point anyway, so use the cheaper
    // interpolated lookup instead.
    if (binHi - binLo < 1.0f)
        return getDbForFrequency (0.5f * (freqLo + freqHi));

    const int b0 = juce::jlimit (0, numBins - 1, (int) std::floor (binLo));
    const int b1 = juce::jlimit (0, numBins - 1, (int) std::ceil  (binHi));

    // Average in POWER (amplitude-squared), not in dB directly — dB is
    // logarithmic, so naively averaging dB values under-weights the loudest
    // bins in the range. Converting to linear amplitude, squaring, averaging,
    // then converting back is the physically correct way to combine several
    // frequency bins' energy into one representative level.
    double sumSquares = 0.0;
    int count = 0;
    for (int b = b0; b <= b1; ++b)
    {
        const float amp = juce::Decibels::decibelsToGain (smoothedDb[(size_t) b], floorDb);
        sumSquares += (double) amp * (double) amp;
        ++count;
    }

    const float meanSquare = (float) (sumSquares / juce::jmax (1, count));
    const float rmsAmp     = std::sqrt (meanSquare);
    return juce::Decibels::gainToDecibels (rmsAmp, floorDb);
}
