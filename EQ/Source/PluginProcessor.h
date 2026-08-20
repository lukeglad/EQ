#pragma once

#include <array>
#include <JuceHeader.h>
#include "EQBand.h"
#include "SpectrumAnalyzer.h"

//==============================================================================
class EQAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int numBands = 6;

    //==========================================================================
    EQAudioProcessor();
    ~EQAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==========================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==========================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Backs undo/redo for every APVTS parameter change (band edits, mute,
    // bypass, gain trims -- everything). Declared BEFORE apvts since it's
    // passed into that constructor by reference (construction order follows
    // declaration order, not the constructor initialiser list's order).
    // Owned here (not by the editor) so undo history survives the editor
    // being closed and reopened, same reasoning as the analyzers below.
    juce::UndoManager undoManager;

    // THE single source of truth for every parameter. The audio thread reads
    // its atomic values here; the editor attaches its controls to the same
    // tree. Because every control is an APVTS parameter, every one of them is
    // automatable in FL Studio for free.
    juce::AudioProcessorValueTreeState apvts;

    // Declares all parameters (IDs, ranges, defaults). Static so it can run
    // inside the constructor's initialiser list.
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Builds a consistent parameter ID like "b2_freq". Shared by DSP and UI so
    // the two never disagree about a name.
    static juce::String bandParamID (int band, juce::StringRef name);

    // Spectrum analyzers. Owned here so the audio thread always has a valid
    // target; the editor reads their smoothed curves when open.
    // PRE-EQ: two instances at different FFT sizes: "analyzer" (fast, order
    // 12, used for mids/highs) and "analyzerLow" (slow, order 13, finer bin
    // spacing, used only below a crossover frequency for sharper bass
    // detail). See EQCurveDisplay::analyzerDb() for how the two get blended.
    // POST-EQ: mirrors the same fast+slow pair — "analyzerPost" (order 12) /
    // "analyzerPostLow" (order 13) — so pre and post read the low end at the
    // SAME resolution and only diverge where the EQ is actually doing
    // something (a single-instance post was tried first and made the two
    // curves genuinely disagree in the low end even with no EQ shaping there,
    // since they were then different-resolution measurements of the same
    // signal). See EQCurveDisplay::analyzerDb()/analyzerPostDb() for how each
    // pair gets blended.
    SpectrumAnalyzer& getAnalyzer()         noexcept { return analyzer; }
    SpectrumAnalyzer& getAnalyzerLow()      noexcept { return analyzerLow; }
    SpectrumAnalyzer& getAnalyzerPost()     noexcept { return analyzerPost; }
    SpectrumAnalyzer& getAnalyzerPostLow()  noexcept { return analyzerPostLow; }

    // Reports the "bypass" parameter as the host's native/generic bypass, so
    // hosts that offer their own bypass UI/automation lane can drive ours.
    juce::AudioProcessorParameter* getBypassParameter() const override
    {
        return apvts.getParameter ("bypass");
    }

    // The dB the dynamics are CURRENTLY adding to a band's gain (0 when that
    // band isn't dynamic, isn't active, or simply isn't triggering) --
    // published every block so the editor can draw the live moving curve.
    // Same lock-free pattern as the analyzers: audio thread stores, UI reads.
    float getDynamicGainDb (int band) const noexcept
    {
        return dynGainDb[(size_t) juce::jlimit (0, numBands - 1, band)].load();
    }

    // "Listen" (audition) mode: -1 = off, otherwise the band index currently
    // being auditioned. NOT an APVTS parameter -- deliberately plain/atomic
    // view-ish state, since it's a temporary monitoring aid, not something
    // that should be host-automatable or saved in a preset (see processBlock
    // for how it overrides the final output with a bandpass slice centred on
    // that band's own freq/Q).
    int  getListenBand() const noexcept        { return listenBandIndex.load(); }
    void setListenBand (int band) noexcept     { listenBandIndex.store (band); }

    //==========================================================================
    // Undo/redo, backed by undoManager (see its declaration above apvts).
    // Every attachment-driven control (mute button, type dropdown, bypass,
    // in/out sliders) already gets this for free — JUCE's own
    // ParameterAttachment::beginGesture() calls undoManager->
    // beginNewTransaction() automatically whenever one of those widgets
    // starts a user gesture, since we passed &undoManager into the APVTS
    // constructor. The manual (non-attachment) parameter writes -- graph
    // drag/scroll, typed-value commits, band delete -- bypass that class
    // entirely, so THEY call beginNewTransaction() explicitly through here
    // (see EQCurveDisplay::beginGesture()/removeBand() and
    // PluginEditor::setBandParamFromEditor()) at the same points they already
    // call beginChangeGesture(), so one drag/typed-edit/delete still becomes
    // exactly one undo step, not one per intermediate value.
    void beginNewTransaction() { undoManager.beginNewTransaction(); }
    bool canUndo() const       { return undoManager.canUndo(); }
    bool canRedo() const       { return undoManager.canRedo(); }
    void undo()                { undoManager.undo(); }
    void redo()                { undoManager.redo(); }

    //==========================================================================
    // Presets: whole-state save/load, same serialisation this plugin already
    // uses for session recall (getStateInformation/setStateInformation) --
    // apvts.copyState()/replaceState() with a plain XML file on disk instead
    // of the host's binary chunk. A handful of factory presets are written
    // into the folder once, the first time it's empty (see
    // ensureFactoryPresetsExist(), called from the constructor) -- built by
    // actually setting parameters through the normal normalised-value API
    // rather than hand-written XML, since some ranges (freq) are skewed and
    // hand-computing a skewed normalised value is exactly the kind of thing
    // that silently breaks.
    static juce::File getPresetsDirectory();
    juce::StringArray getPresetNames() const; // sorted base names, no extension
    bool loadPreset (const juce::String& name);      // false if the file's missing/unreadable
    void savePresetAs (const juce::String& name);    // overwrites if it already exists
    void deletePreset (const juce::String& name);    // no-op if the file doesn't exist

    // Asks DISK, not the editor's cached name list -- another instance of the
    // plugin (or Finder) can add a preset behind our back, and an overwrite
    // prompt that misses those would silently clobber one.
    bool presetExists (const juce::String& name) const;

    // Moves the file. False if the source is missing or the move fails; the
    // CALLER is responsible for having confirmed any overwrite first (an
    // existing target is deleted here, since File::moveFileTo won't replace
    // one on every platform).
    bool renamePreset (const juce::String& oldName, const juce::String& newName);

    // The editor's last window width, remembered here so reopening the
    // editor keeps the size the user dragged it to (the corner resizer --
    // see the editor's ctor/resized()). Lives on the processor because the
    // editor is destroyed on close, AND rides along in the session state
    // (see getStateInformation -- session only, never preset files) so it
    // survives everything the band settings themselves survive: project
    // save/reload, DAW-managed plugin reloads. A genuinely fresh instance
    // (removed and re-added with no state) starts at the default, exactly
    // like the parameters do. Keeping this instance-only was tried first
    // and the size visibly reset far too often. 0 = never set.
    //
    // Height became independent when the editor went free-aspect (reflow:
    // extra window area feeds the graph). Sessions saved before that carry
    // only editorWidth; the editor derives the old locked-aspect height
    // from it in that case.
    int lastEditorWidth  = 0;
    int lastEditorHeight = 0;

private:
    void ensureFactoryPresetsExist(); // called once from the constructor

    //==========================================================================
    std::array<EQBand, numBands> bands;

    // Broadband input / output trims. juce::dsp::Gain ramps internally, so
    // these are click-free without any extra work from us.
    juce::dsp::Gain<float> inputGain, outputGain;

    // Atomic pointers cached once, so processBlock does zero string lookups on
    // the audio thread. APVTS stores bool as 0/1 and choice as its index, both
    // as floats.
    std::array<std::atomic<float>*, numBands> pOn {}, pType {}, pFreq {}, pGain {}, pQ {}, pMute {}, pSlope {},
                                              pDynOn {}, pDynRange {};
    std::atomic<float>* pInGain  = nullptr;
    std::atomic<float>* pOutGain = nullptr;
    std::atomic<float>* pBypass  = nullptr;

    SpectrumAnalyzer analyzer;         // pre-EQ,  fast: order 12 (4096), mids/highs
    SpectrumAnalyzer analyzerLow;      // pre-EQ,  slow: order 13 (8192), low end only
    SpectrumAnalyzer analyzerPost;     // post-EQ, fast: order 12 (4096), mids/highs
    SpectrumAnalyzer analyzerPostLow;  // post-EQ, slow: order 13 (8192), low end only

    // "Listen" audition filter: a bandpass centred on whatever band is
    // currently being auditioned, applied to a SEPARATE copy of the pre-band
    // signal (listenScratch) and swapped in as the final output ONLY while
    // active -- never touches the real per-band processing above, so turning
    // it off leaves the actual EQ'd output completely unaffected.
    std::atomic<int> listenBandIndex { -1 };

    //==========================================================================
    // Per-band dynamics ("make dynamic", Pro-Q style, auto-threshold only).
    //
    // Each dynamic band runs a MONO bandpass detector centred on its own
    // freq/Q over the raw input (the same "how much energy is in this band's
    // region" measurement listen mode makes -- but per band, always on, and
    // never rendered, so no scratch buffer: the filtered samples are peak-
    // measured and thrown away). The block peak feeds an attack/release
    // envelope, the envelope feeds a SLOW average that acts as the automatic
    // threshold ("louder than this band's recent typical level" is what
    // triggers), and the overshoot maps through a soft knee to a fraction of
    // dynRange. That fraction * range is added to the band's static gain --
    // negative range compresses, positive expands. EQBand's own 50 ms gain
    // smoothing is the de-zipper, so block-rate updates never click.
    struct DynDetector
    {
        juce::dsp::IIR::Filter<float> filter;   // mono bandpass at band freq/Q
        float envDb    = -80.0f;                // attack/release envelope
        float threshDb = -60.0f;                // slow auto-threshold average
    };
    std::array<DynDetector, numBands> dynDetectors;
    std::array<std::atomic<float>, numBands> dynGainDb {}; // published for the editor
    juce::AudioBuffer<float> dynMonoScratch;    // mono sum the detectors read
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                   juce::dsp::IIR::Coefficients<float>> listenFilter;
    juce::AudioBuffer<float> listenScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQAudioProcessor)
};
