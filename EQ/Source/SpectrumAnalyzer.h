#pragma once

#include <atomic>
#include <vector>
#include <JuceHeader.h>

//==============================================================================
/**
    Real-time pre-EQ spectrum analyzer.

    Split cleanly across two threads so the audio thread never blocks:

    - AUDIO thread calls pushBlock() — a lock-free write into a circular
      buffer. Windows OVERLAP (new FFT triggered every hopSize samples, a
      quarter of the window) rather than waiting for a fresh non-overlapping
      block each time — this is what makes the display move fluidly instead
      of visibly stepping between snapshots every ~43 ms. Each hop publishes
      the last fftSize samples (in correct time order) into a shared buffer
      and flips one atomic flag. No locks, no allocation, no blocking.

    - MESSAGE thread calls update() from the editor's ~30 Hz timer: if a new
      block is ready it copies it out, applies a Hann window + FFT, converts
      to dBFS per bin, smooths ACROSS FREQUENCY (fractional-octave averaging,
      for that rounded/musical look instead of every harmonic showing as a
      separate spike), then smooths ACROSS TIME. Attack and release are
      deliberately asymmetric in more than just speed:
        - ATTACK stays plain exponential smoothing directly ON THE DB VALUE.
          Rising in linear amplitude instead was tried and reverted — near low
          levels dB is extremely sensitive to small amplitude changes, so
          ordinary frame-to-frame FFT noise kept triggering "fast attack"
          almost every frame, reading as chaotic/jittery rather than musical.
        - RELEASE converts to linear amplitude just for its own step: it
          subtracts a straight-line amount of amplitude per second (not
          exponential), then converts back to dB. The per-second amount is
          captured PER BIN from each peak's own level (see
          releaseRatePerSecond), so a quiet peak decays proportionally slower
          than a loud one instead of everything sharing one full-scale rate.
          A steady linear-amplitude decay looks, once back on the (logarithmic)
          dB scale, like it eases down slowly at first and then falls away
          faster — which is what a reference analyzer's (Pro-Q's) calmer
          motion during transients turned out to be. (A hard peak-hold-then-
          release was also tried and rejected — the discrete pause read as
          unsmooth rather than calm.)
        - RELEASE is additionally capped to a maximum dB-per-second fall rate,
          derived PER BIN from that bin's own captured `rate` (i.e. its own
          last peak level) -- NOT a shared/global constant. The linear-
          amplitude decay above implies an ACCELERATING dB fall as amplitude
          nears zero (dB is logarithmic): most of a fall is a gentle ease-down,
          then the last stretch plunges steeply and hits the floor as a sudden
          hard clamp -- a visible slope discontinuity ("slams flat" right as
          it drops out of frame). Capping the per-frame dB drop turns that
          plunge-then-clamp into one smooth glide instead. Deriving the cap
          per-bin (rather than one shared ceiling) matters most right when
          audio stops: every bin falls via this same branch then, and a SHARED
          cap would make them all glide down at an identical, indistinguishable
          rate -- exactly wrong, since a bin that peaked loud should keep
          audibly falling (and take longer to reach the floor) while a bin
          that peaked quiet settles sooner, same independent-per-bin behavior
          as during normal playback.

    Everything the display reads (the smoothed dB curve) is touched ONLY on the
    message thread. The sole cross-thread state is the circular-buffer hand-off:
    `fftData` plus one `std::atomic<bool>`, gated so the two threads never touch
    it at the same time.

    Owned by the processor, so the audio thread always has a valid target
    whether or not the editor is open.

    FFT size is a CONSTRUCTOR parameter (not a fixed constant) so the processor
    can run two instances side by side: a "fast" one (small window, responsive,
    used for mids/highs) and a "slow" one (bigger window, finer bin spacing,
    used only below a crossover frequency for sharper bass detail). See
    EQAudioProcessor::analyzer / analyzerLow and EQCurveDisplay::analyzerDb()
    for how the two get blended for display.
*/
class SpectrumAnalyzer
{
public:
    explicit SpectrumAnalyzer (int fftOrderToUse);

    /** Sample rate needed to map FFT bins -> Hz. Also clears stale data so a
        fresh transport start doesn't flash the previous signal. */
    void prepare (double sampleRate);

    //==========================================================================
    // --- audio thread ---
    /** Push one block of audio (mono-summed internally). Lock-free, RT-safe. */
    void pushBlock (const juce::AudioBuffer<float>& buffer) noexcept;

    //==========================================================================
    // --- message thread (editor timer) ---
    /** Pull any pending block, FFT + smooth it, and decay toward the floor if
        the audio has gone silent/stopped. Call at the UI refresh rate. */
    void update();

    /** Smoothed level in dBFS at an arbitrary frequency (interpolated between
        the two nearest bins). Message thread only. */
    float getDbForFrequency (float freqHz) const noexcept;

    /** Smoothed level in dBFS across a frequency RANGE — averages (in power,
        i.e. RMS) every FFT bin whose centre falls within [freqLo, freqHi].
        On a log x-axis, one screen pixel at high frequencies can span many
        linear FFT bins; querying a single bin (or interpolating between two)
        makes the display look spiky there, since each bin still carries its
        own independent noise. Averaging every bin the pixel actually covers
        is what turns that into a smooth line. Falls back to the single-point
        interpolated value when the range covers less than one bin (typical
        at low frequencies, where a pixel spans less than a bin already).
        Message thread only. */
    float getDbForFrequencyRange (float freqLo, float freqHi) const noexcept;

    static constexpr float floorDb = -100.0f; // bottom of the analyzer's range

private:
    void pushSample (float s) noexcept;

    // FFT size is now set PER INSTANCE (constructor arg), not a fixed
    // constant — see the class comment for why (dual fast/slow analyzers).
    // These four are derived once at construction and never change.
    const int fftOrder;
    const int fftSize;   // 1 << fftOrder
    const int numBins;   // fftSize / 2

    // 75% overlap (hop = 1/4 window): a new spectrum every ~11 ms @ 48 kHz for
    // the fast (order-12) instance instead of ~43 ms, using mostly the same
    // samples as the previous one — that shared content is what makes
    // consecutive frames flow into each other instead of jumping between
    // unrelated snapshots. The slow instance publishes proportionally less
    // often (bigger hop), which is fine — bass content moves slower anyway.
    const int hopSize;   // fftSize / 4

    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;

    // --- the ONLY audio <-> message shared state ---------------------------
    std::vector<float> circularBuffer; // audio thread only, size fftSize
    std::vector<float> fftData;        // published by audio thread, size 2*fftSize
    int writePos = 0;                                  // audio thread only
    int samplesSinceLastHop = 0;                        // audio thread only
    std::atomic<bool> nextFFTBlockReady { false };

    // --- message-thread-only working state ---------------------------------
    std::vector<float> workData;              // size 2*fftSize
    std::vector<float> rawDb;                 // size numBins, straight from this frame's FFT
    std::vector<float> octaveDb;              // size numBins, rawDb after frequency-domain smoothing
    std::vector<float> smoothedDb;            // size numBins, octaveDb after time-domain smoothing (what the display reads)

    // Each bin's OWN release slope (linear amplitude per second), captured
    // fresh at the moment of its most recent peak — NOT a fixed constant
    // calibrated to full scale. A fixed full-scale-calibrated rate turned out
    // to be wildly oversized for anything below roughly -20 dBFS (i.e. most
    // real audio), wiping quieter content out in a frame or two instead of
    // the intended gentle multi-second fall. Scaling the rate to each peak's
    // own amplitude means "releaseTimeToZeroSeconds" is only the reference
    // duration for a genuine 0 dBFS peak; a peak at -40 dBFS decays
    // proportionally (~100x) more slowly in absolute amplitude terms.
    std::vector<float> releaseRatePerSecond;  // size numBins

    juce::uint32 lastBlockMs  = 0;
    juce::uint32 lastUpdateMs = 0; // for measuring the real dt between update() calls
    double sampleRate = 44100.0;

    // Tracks the PREVIOUS call's starved state so update() can detect the
    // exact frame playback stops (rather than every starved frame) -- see
    // its use there: every bin's releaseRatePerSecond gets re-synchronized
    // ONCE, right at that transition, so the whole spectrum decays to the
    // floor together as one shape instead of each bin continuing to fall on
    // whatever stale, independently-timed rate it last captured from real
    // audio content (which is what made the stop-decay look like it was
    // "falling apart" instead of settling as one cohesive silhouette).
    bool wasStarved = false;

    // Fractional-octave smoothing width, applied ACROSS FREQUENCY once per new
    // FFT frame (not per pixel — see EQCurveDisplay for the separate per-pixel
    // bin averaging). History: 1/24-octave originally (1/6 even earlier,
    // which blended individual harmonics into a couple of soft bumps and lost
    // real detail) -> briefly widened to 1/12 to fix a "blocky/faceted" look,
    // but that traded away the busy peaks-and-notches texture the user
    // actually wants visible (a flat, over-smoothed blob is the opposite
    // problem from "blocky") -> narrowed to 1/32, tighter than the original
    // 1/24, specifically to bring more individual peaks back. Applied as
    // +/- 1/64 octave around each bin.
    static constexpr float octaveSmoothingFraction = 1.0f / 32.0f;

    // Floor on the octave-smoothing window, as an absolute Hz BANDWIDTH
    // (shared by every instance) rather than a fixed bin count — see the
    // usage site in applyOctaveSmoothing() for why a fixed bin count broke
    // once a second, finer-resolution analyzer instance was introduced (it
    // let that instance reveal narrow spectral notches the coarser instance
    // smooths away, producing a visible dip in the crossfade zone between
    // them). History: ~58.6 Hz implicitly -> 150 Hz -> briefly 250 Hz (same
    // "fix blocky" attempt as the octave-fraction widen above) -> 100 Hz.
    // Narrowed further to 15 Hz: at the (now much narrower) 1/32-octave
    // setting, this floor was the actual dominant term almost everywhere
    // below ~4.6 kHz (its own fixed Hz width easily exceeds the octave-
    // proportional window's width down there), forcing a much wider average
    // than the octave setting alone would apply and flattening real detail
    // into visible plateaus (e.g. around 400 Hz) well above where the floor
    // is actually needed (very low bass, where the octave-proportional
    // window alone collapses to almost nothing). 15 Hz only meaningfully
    // takes over below roughly 700 Hz now, rather than up to ~4.6 kHz.
    static constexpr float minAverageBandwidthHz = 15.0f;

    void applyOctaveSmoothing();

    // Attack: fast exponential rise, applied directly to the dB value (NOT in
    // linear amplitude — see the class comment for why). Converted to a
    // per-call coefficient from the measured dt so transients register
    // immediately.
    static constexpr float attackTimeSeconds = 0.05f;  // 50 ms

    // Release: reference decay time — how long a full-scale (0 dBFS) peak takes
    // to fall to silence. Each bin's actual per-second linear-amplitude rate is
    // this scaled to its own peak level (releaseRatePerSecond), so a quieter
    // peak decays proportionally slower and every peak takes a comparable
    // amount of time relative to its own height, rather than everything using
    // one absolute rate calibrated to full scale.
    static constexpr float releaseTimeToZeroSeconds = 0.8f;

    // Multiplier on the per-bin terminal-glide dB/sec cap (see update()) --
    // the cap alone (peakDbAboveFloor / releaseTimeToZeroSeconds, i.e. that
    // peak's own AVERAGE rate over its whole fall) still let the very end of
    // the fall visibly speed up, since the underlying linear-amplitude decay
    // is fastest near zero. Tightened to 0.5 (half the average rate) so the
    // tail glides down noticeably slower instead of accelerating into the
    // floor -- 1.0 would be the original (uncapped-feeling) speed.
    static constexpr float tailFallCapFactor = 0.5f;

    // (No global fall-rate cap here — the per-frame dB-drop cap that prevents
    // the tail from plunging-then-clamping is derived PER BIN, live, from that
    // bin's own `releaseRatePerSecond` entry in update() -- see the class
    // comment for why a shared constant was wrong: it made every bin's
    // terminal glide toward the floor look identical, most visibly right when
    // audio stops, when every bin is falling via this same code path at once.)

    // If no fresh audio arrives for this long (e.g. transport stopped and the
    // host quit calling processBlock), drive the display down to the floor
    // instead of freezing on the last frame.
    static constexpr juce::uint32 starvationMs = 150;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzer)
};
