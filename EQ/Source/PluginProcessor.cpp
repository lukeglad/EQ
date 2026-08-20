#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
juce::String EQAudioProcessor::bandParamID (int band, juce::StringRef name)
{
    return "b" + juce::String (band) + "_" + name;
}

juce::AudioProcessorValueTreeState::ParameterLayout EQAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    // A logarithmic frequency range: setSkewForCentre puts 1 kHz at the middle
    // of the control's travel, matching how we hear pitch (each octave gets
    // equal space). Without the skew, 1 kHz would sit ~5% from the left.
    auto makeFreqRange = []
    {
        NormalisableRange<float> r (20.0f, 20000.0f, 1.0f);
        r.setSkewForCentre (1000.0f);
        return r;
    };

    // A gently logarithmic Q range with Q = 1 near the middle.
    auto makeQRange = []
    {
        NormalisableRange<float> r (0.1f, 10.0f, 0.001f);
        r.setSkewForCentre (1.0f);
        return r;
    };

    // Sensible spread of default centre frequencies across the spectrum.
    const std::array<float, numBands> defaultFreqs { 80.0f, 200.0f, 500.0f, 1500.0f, 4000.0f, 10000.0f };

    for (int b = 0; b < numBands; ++b)
    {
        const auto n = String (b + 1);

        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { bandParamID (b, "on"), 1 }, "Band " + n + " On", false));

        // The choice ORDER must match EQBand::FilterType's integer values.
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { bandParamID (b, "type"), 1 }, "Band " + n + " Type",
            StringArray { "Bell", "Low Shelf", "High Shelf", "High-Pass", "Low-Pass", "Notch" }, 0));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { bandParamID (b, "freq"), 1 }, "Band " + n + " Freq",
            makeFreqRange(), defaultFreqs[(size_t) b]));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { bandParamID (b, "gain"), 1 }, "Band " + n + " Gain",
            NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { bandParamID (b, "q"), 1 }, "Band " + n + " Q",
            makeQRange(), 1.0f));

        // Distinct from "on": "on" controls whether the band is PLACED (has
        // a point/handle at all); "mute" bypasses its DSP processing while
        // leaving the point and its settings exactly where they were --
        // deleting a band clears "on" (and removes the point); muting never
        // touches "on".
        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { bandParamID (b, "mute"), 1 }, "Band " + n + " Mute", false));

        // Only meaningful for Low-Pass/High-Pass (see EQBand::stageCountFor())
        // -- every other type ignores this and always runs a single biquad
        // stage, same convention as "gain" being ignored/hidden for those two
        // types. Choice order matches stage count directly: index 0 = 1
        // stage (12 dB/oct) ... index 3 = 4 stages (48 dB/oct).
        layout.add (std::make_unique<AudioParameterChoice> (
            ParameterID { bandParamID (b, "slope"), 1 }, "Band " + n + " Slope",
            StringArray { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" }, 0));

        // Per-band dynamics. "dynrange" is what the graph's second (hollow)
        // handle drags: how far the band's gain travels at full dynamic
        // action -- NEGATIVE compresses (louder input pulls the band down),
        // POSITIVE expands (louder input pushes it up). Threshold is
        // deliberately NOT a parameter: it's automatic, adapting to the
        // band's own recent level (see processBlock), matching Pro-Q's
        // auto mode. Ignored (like "gain") for types without gain.
        layout.add (std::make_unique<AudioParameterBool> (
            ParameterID { bandParamID (b, "dynon"), 1 }, "Band " + n + " Dynamic", false));

        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { bandParamID (b, "dynrange"), 1 }, "Band " + n + " Dyn Range",
            NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f));
    }

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "inGain", 1 }, "Input Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "outGain", 1 }, "Output Gain",
        NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

//==============================================================================
EQAudioProcessor::EQAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undoManager, "PARAMETERS", createParameterLayout()),
      analyzer (12), analyzerLow (13), analyzerPost (12), analyzerPostLow (13)
{
    // Cache the atomic value pointers ONCE. getRawParameterValue does a string
    // lookup, which we never want to do per-block on the audio thread.
    for (int b = 0; b < numBands; ++b)
    {
        pOn[(size_t) b]   = apvts.getRawParameterValue (bandParamID (b, "on"));
        pType[(size_t) b] = apvts.getRawParameterValue (bandParamID (b, "type"));
        pFreq[(size_t) b] = apvts.getRawParameterValue (bandParamID (b, "freq"));
        pGain[(size_t) b] = apvts.getRawParameterValue (bandParamID (b, "gain"));
        pQ[(size_t) b]    = apvts.getRawParameterValue (bandParamID (b, "q"));
        pMute[(size_t) b] = apvts.getRawParameterValue (bandParamID (b, "mute"));
        pSlope[(size_t) b] = apvts.getRawParameterValue (bandParamID (b, "slope"));
        pDynOn[(size_t) b]    = apvts.getRawParameterValue (bandParamID (b, "dynon"));
        pDynRange[(size_t) b] = apvts.getRawParameterValue (bandParamID (b, "dynrange"));
    }

    pInGain  = apvts.getRawParameterValue ("inGain");
    pOutGain = apvts.getRawParameterValue ("outGain");
    pBypass  = apvts.getRawParameterValue ("bypass");

    ensureFactoryPresetsExist();
}

EQAudioProcessor::~EQAudioProcessor()
{
}

//==============================================================================
void EQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels      = (juce::uint32) juce::jmax (1, getTotalNumOutputChannels());

    for (auto& band : bands)
        band.prepare (spec);

    inputGain.prepare (spec);
    outputGain.prepare (spec);

    // 20 ms internal ramp on the trims, so gain moves are click-free too.
    inputGain.setRampDurationSeconds (0.02);
    outputGain.setRampDurationSeconds (0.02);

    analyzer.prepare (sampleRate);
    analyzerLow.prepare (sampleRate);
    analyzerPost.prepare (sampleRate);
    analyzerPostLow.prepare (sampleRate);

    listenFilter.prepare (spec);
    listenScratch.setSize (juce::jmax (1, getTotalNumOutputChannels()), samplesPerBlock);

    // Dynamics detectors: mono, one per band. Coefficients are (re)built per
    // block in processBlock (the band's freq/Q move under them); state is
    // reset here so a stale envelope can't trigger a gain jump on transport
    // start after a sample-rate change.
    juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) samplesPerBlock, 1 };
    for (auto& d : dynDetectors)
    {
        d.filter.prepare (monoSpec);
        d.filter.reset();
        d.envDb    = -80.0f;
        d.threshDb = -60.0f;
    }
    for (auto& g : dynGainDb)
        g.store (0.0f);
    dynMonoScratch.setSize (1, samplesPerBlock);
}

void EQAudioProcessor::releaseResources()
{
}

bool EQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Only mono or stereo, and input layout must match output layout.
    const auto& mainOutput = layouts.getMainOutputChannelSet();
    const auto& mainInput  = layouts.getMainInputChannelSet();

    if (mainOutput != juce::AudioChannelSet::mono()
        && mainOutput != juce::AudioChannelSet::stereo())
        return false;

    return mainInput == mainOutput;
}

void EQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    if (pBypass->load() > 0.5f)
        return; // true bypass: leave the buffer exactly as the host handed it to us

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();

    for (int ch = totalNumInputChannels; ch < totalNumOutputChannels; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numSamples = buffer.getNumSamples();

    // --- Per-band dynamics detection ---------------------------------------
    // Runs on the RAW input, before the input trim -- the auto-threshold is
    // adaptive (it tracks each band's own recent level), so a static trim
    // offset washes out of the trigger within seconds either way, and
    // detecting first keeps the whole dynamics computation ahead of the
    // APVTS -> DSP hand-off that needs its result.
    //
    // The detectors read a MONO SUM: dynamics shouldn't treat a hard-panned
    // source differently from a centred one, and mono halves the filter work.
    {
        bool anyDyn = false;
        for (int b = 0; b < numBands; ++b)
            if (pDynOn[(size_t) b]->load() > 0.5f) { anyDyn = true; break; }

        // The mono sum is only built if some band actually needs it.
        if (anyDyn)
        {
            dynMonoScratch.setSize (1, numSamples, false, false, true);
            dynMonoScratch.copyFrom (0, 0, buffer, 0, 0, numSamples);
            for (int ch = 1; ch < totalNumInputChannels; ++ch)
                dynMonoScratch.addFrom (0, 0, buffer, ch, 0, numSamples);
            if (totalNumInputChannels > 1)
                dynMonoScratch.applyGain (1.0f / (float) totalNumInputChannels);
        }

        const float blockDur = (float) numSamples / (float) getSampleRate();

        for (int b = 0; b < numBands; ++b)
        {
            auto& det = dynDetectors[(size_t) b];

            const bool on   = pOn[(size_t) b]->load()   > 0.5f;
            const bool mute = pMute[(size_t) b]->load() > 0.5f;
            const auto type = static_cast<EQBand::FilterType> ((int) pType[(size_t) b]->load());
            const bool hasGain = type == EQBand::FilterType::Bell
                              || type == EQBand::FilterType::LowShelf
                              || type == EQBand::FilterType::HighShelf;

            const bool dynActive = on && ! mute && hasGain
                                && pDynOn[(size_t) b]->load() > 0.5f;

            if (! dynActive)
            {
                dynGainDb[(size_t) b].store (0.0f);
                continue;
            }

            // Bandpass at the band's own freq/Q -- the same "slice of the
            // spectrum this band cares about" listen mode auditions. Rebuilt
            // per block (not per sample), same cost policy as EQBand's own
            // coefficient updates.
            det.filter.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (
                getSampleRate(), pFreq[(size_t) b]->load(),
                juce::jmax (0.1f, pQ[(size_t) b]->load()));

            // Peak of the filtered block. The samples themselves are never
            // kept -- level is all the dynamics need.
            float peak = 0.0f;
            const float* mono = dynMonoScratch.getReadPointer (0);
            for (int i = 0; i < numSamples; ++i)
                peak = juce::jmax (peak, std::abs (det.filter.processSample (mono[i])));

            const float peakDb = juce::Decibels::gainToDecibels (peak, -80.0f);

            // Attack/release envelope at block rate (~3-11 ms steps at
            // typical buffer sizes). EQBand's 50 ms gain smoothing rides on
            // top, so these are the FEEL of the dynamics, not the de-zipper.
            const float tau   = peakDb > det.envDb ? 0.015f : 0.150f;
            const float coeff = std::exp (-blockDur / tau);
            det.envDb = coeff * det.envDb + (1.0f - coeff) * peakDb;

            // Auto-threshold: an average of the envelope, i.e. "this band's
            // typical recent level" -- ASYMMETRIC on purpose. It climbs
            // fast (~0.3 s) and falls slowly (~4 s), which makes the
            // dynamics respond to CHANGES: a transient or a suddenly-loud
            // passage overshoots and triggers, but a passage that stays
            // loud stops triggering within a second as the threshold
            // catches up -- rather than compressing continuously from cold
            // start onward. Only adapts while signal is actually present,
            // so silence can't drag it to the floor and make the first
            // note back slam the full range.
            if (det.envDb > -70.0f)
            {
                const float tau2   = det.envDb > det.threshDb ? 0.3f : 4.0f;
                const float tCoeff = std::exp (-blockDur / tau2);
                det.threshDb = juce::jmax (-70.0f,
                    tCoeff * det.threshDb + (1.0f - tCoeff) * det.envDb);
            }

            // Soft knee: overshoot above the auto-threshold maps smoothly
            // onto 0..1 across 12 dB (smoothstep), then scales the range.
            // Louder than typical -> the band moves toward its full range,
            // whichever direction the range points.
            const float overshoot = det.envDb - det.threshDb;
            const float t = juce::jlimit (0.0f, 1.0f, overshoot / 12.0f);
            const float fraction = t * t * (3.0f - 2.0f * t);

            dynGainDb[(size_t) b].store (fraction * pDynRange[(size_t) b]->load());
        }
    }

    // --- APVTS -> DSP hand-off ---------------------------------------------
    // Read the atomics the host/UI have written and pass them to each band as
    // smoothing targets. We only STORE targets here; the actual coefficient
    // rebuild happens per-band just below. Dynamic bands get the detector's
    // current contribution ADDED to their static gain -- EQBand never knows
    // dynamics exist, it just sees a gain target that moves with the music.
    for (int b = 0; b < numBands; ++b)
    {
        const bool on     = pOn[(size_t) b]->load()   > 0.5f;
        const bool mute   = pMute[(size_t) b]->load() > 0.5f;
        const bool active = on && ! mute;

        bands[(size_t) b].setParameters (
            static_cast<EQBand::FilterType> ((int) pType[(size_t) b]->load()),
            pFreq[(size_t) b]->load(),
            pGain[(size_t) b]->load() + dynGainDb[(size_t) b].load(),
            pQ[(size_t) b]->load(),
            (int) pSlope[(size_t) b]->load(),
            active);
    }

    // Wrap the buffer once; every stage processes this same block in place.
    juce::dsp::AudioBlock<float> block (buffer);

    // Signal flow: input trim -> band 1 -> ... -> band 6 -> output trim.
    inputGain.setGainDecibels (pInGain->load());
    {
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        inputGain.process (ctx);
    }

    // Feed both spectrum analyzers PRE-EQ: after the input trim, before any
    // band touches the signal. Lock-free push, no work done on this thread
    // beyond copying samples into a fifo (same tap point for both instances).
    analyzer.pushBlock (buffer);
    analyzerLow.pushBlock (buffer);

    // "Listen" audition: capture the pre-band signal now (before any band
    // touches it), so it's available to bandpass-and-substitute at the very
    // end of this function, ONLY if a band is actively being auditioned.
    const int listenBand = listenBandIndex.load();
    if (listenBand >= 0 && listenBand < numBands)
    {
        listenScratch.setSize (buffer.getNumChannels(), numSamples, false, false, true);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            listenScratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);
    }

    for (int b = 0; b < numBands; ++b)
    {
        bands[(size_t) b].updateCoefficients (numSamples); // advance smoothing + rebuild coeffs
        bands[(size_t) b].process (block);                 // filter in place (or skip if inactive)
    }

    outputGain.setGainDecibels (pOutGain->load());
    {
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        outputGain.process (ctx);
    }

    // Post-EQ tap: after every band AND the output trim, so this reflects the
    // exact final signal leaving the plugin (the TRUE EQ'd signal, regardless
    // of whether "listen" is about to override the actual output below --
    // the analyzer should keep showing the real curve, not the temporary
    // audition slice). Same lock-free push as the pre-EQ taps above.
    analyzerPost.pushBlock (buffer);
    analyzerPostLow.pushBlock (buffer);

    // "Listen" audition override: replace the FINAL output with a bandpass
    // slice of the pre-band signal, centred on the auditioned band's own
    // freq/Q (narrow Q = narrow slice, matching that band's actual shape).
    // This is the LAST thing that happens -- it only ever overwrites the
    // buffer that's about to be returned to the host, never anything the
    // bands/analyzers above already computed, so turning it off leaves the
    // real EQ'd output completely unaffected.
    if (listenBand >= 0 && listenBand < numBands)
    {
        const float freq = pFreq[(size_t) listenBand]->load();
        const float q    = pQ[(size_t) listenBand]->load();
        *listenFilter.state = *juce::dsp::IIR::Coefficients<float>::makeBandPass (
            getSampleRate(), freq, q);

        juce::dsp::AudioBlock<float> listenBlock (listenScratch);
        juce::dsp::ProcessContextReplacing<float> listenCtx (listenBlock);
        listenFilter.process (listenCtx);

        // Manual gain, not the shared `outputGain` object -- that one already
        // advanced its internal ramp once this block processing `buffer`;
        // calling it a second time on a different buffer would double-advance
        // its smoothing state incorrectly. A momentary monitoring utility
        // doesn't need ramped gain.
        const float linearGain = juce::Decibels::decibelsToGain (pOutGain->load());
        listenScratch.applyGain (0, 0, numSamples, linearGain);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, 0, listenScratch, ch, 0, numSamples);
    }
}

//==============================================================================
juce::AudioProcessorEditor* EQAudioProcessor::createEditor()
{
    return new EQAudioProcessorEditor (*this);
}

bool EQAudioProcessor::hasEditor() const
{
    return true;
}

//==============================================================================
const juce::String EQAudioProcessor::getName() const           { return JucePlugin_Name; }
bool EQAudioProcessor::acceptsMidi() const                     { return false; }
bool EQAudioProcessor::producesMidi() const                    { return false; }
bool EQAudioProcessor::isMidiEffect() const                    { return false; }
double EQAudioProcessor::getTailLengthSeconds() const          { return 0.0; }

int EQAudioProcessor::getNumPrograms()                          { return 1; }
int EQAudioProcessor::getCurrentProgram()                       { return 0; }
void EQAudioProcessor::setCurrentProgram (int index)           { juce::ignoreUnused (index); }
const juce::String EQAudioProcessor::getProgramName (int index) { juce::ignoreUnused (index); return {}; }
void EQAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void EQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Serialise the whole parameter tree so presets / session recall work.
    if (auto xml = apvts.copyState().createXml())
    {
        // Editor window size rides along in the SESSION state only -- added
        // to the XML here, after copyState(), so it never leaks into preset
        // files (savePresetAs() serialises the tree itself, not this) and a
        // preset can never resize the window. Stripped back out before
        // replaceState() below, keeping the live APVTS tree parameter-pure.
        if (lastEditorWidth > 0)
            xml->setAttribute ("editorWidth",  lastEditorWidth);
            xml->setAttribute ("editorHeight", lastEditorHeight);

        copyXmlToBinary (*xml, destData);
    }
}

void EQAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // See getStateInformation: recover the remembered window size,
            // then strip it so the APVTS tree stays parameter-pure. Sessions
            // saved before this attribute existed just leave the default.
            lastEditorWidth  = xml->getIntAttribute ("editorWidth",  lastEditorWidth);
            lastEditorHeight = xml->getIntAttribute ("editorHeight", lastEditorHeight);
            xml->removeAttribute ("editorWidth");
            xml->removeAttribute ("editorHeight");

            apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }
}

//==============================================================================
juce::File EQAudioProcessor::getPresetsDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Audio")
                   .getChildFile ("Presets")
                   .getChildFile ("AudioFlower")
                   .getChildFile ("EQ");
    dir.createDirectory();
    return dir;
}

juce::StringArray EQAudioProcessor::getPresetNames() const
{
    juce::StringArray names;
    for (const auto& f : getPresetsDirectory().findChildFiles (
             juce::File::findFiles, false, "*.xml"))
        names.add (f.getFileNameWithoutExtension());
    names.sort (true);
    return names;
}

bool EQAudioProcessor::loadPreset (const juce::String& name)
{
    const auto file = getPresetsDirectory().getChildFile (name + ".xml");
    if (auto xml = juce::XmlDocument::parse (file))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            return true;
        }
    }
    return false;
}

void EQAudioProcessor::savePresetAs (const juce::String& name)
{
    const auto file = getPresetsDirectory().getChildFile (name + ".xml");
    if (auto xml = apvts.copyState().createXml())
        xml->writeTo (file);
}

void EQAudioProcessor::deletePreset (const juce::String& name)
{
    getPresetsDirectory().getChildFile (name + ".xml").deleteFile();
}

bool EQAudioProcessor::presetExists (const juce::String& name) const
{
    return getPresetsDirectory().getChildFile (name + ".xml").existsAsFile();
}

bool EQAudioProcessor::renamePreset (const juce::String& oldName, const juce::String& newName)
{
    if (oldName.isEmpty() || newName.isEmpty() || oldName == newName)
        return false;

    const auto dir  = getPresetsDirectory();
    const auto from = dir.getChildFile (oldName + ".xml");
    const auto to   = dir.getChildFile (newName + ".xml");

    if (! from.existsAsFile())
        return false;

    // The overwrite prompt happens in the editor, before we get here.
    if (to.existsAsFile())
        to.deleteFile();

    return from.moveFileTo (to);
}

void EQAudioProcessor::ensureFactoryPresetsExist()
{
    // The four factory presets ship EMBEDDED in the binary (Resources/*.xml,
    // registered in the .jucer) and are written to the user's presets folder
    // here, on construction, if they aren't already there.
    //
    // They used to exist only as files hand-saved on the developer's own
    // machine, defined nowhere in code -- which meant anyone else installing
    // the plugin got no presets at all, and the UI's "protected preset" rule
    // (see PluginEditor.cpp's isProtectedPreset) was guarding files that
    // weren't guaranteed to exist. Embedding them is what makes that rule
    // mean something on a machine other than the one they were authored on.
    //
    // CREATE-IF-MISSING, never overwrite: a factory preset deleted outside
    // the plugin (in Finder) comes back the next time an instance is
    // created, but nothing here can clobber a file that's already there.
    //
    // Looked up by ORIGINAL FILENAME rather than by BinaryData's mangled
    // symbol name -- the mangling is Projucer's business (spaces become
    // underscores today, and "default.xml" happens to survive as a legal
    // identifier), and a future preset whose name mangles awkwardly would
    // otherwise break this silently.
    auto writeIfMissing = [] (const juce::String& filename)
    {
        const auto dest = getPresetsDirectory().getChildFile (filename);

        if (dest.existsAsFile())
            return;

        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        {
            if (filename != BinaryData::originalFilenames[i])
                continue;

            int size = 0;
            if (const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
                data != nullptr && size > 0)
            {
                dest.replaceWithData (data, (size_t) size);
            }

            return;
        }

        jassertfalse; // named a preset that isn't actually embedded
    };

    for (auto* name : { "default.xml", "low cut.xml", "vocal presence.xml", "telephone.xml" })
        writeIfMissing (name);
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EQAudioProcessor();
}
