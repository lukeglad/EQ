#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Builds a monochrome Drawable from Tabler Icons' exact outline SVG path
    // data (MIT licensed, github.com/tabler/tabler-icons) -- innerPaths is
    // just the <path> elements, wrapped in the SAME 24x24 viewBox Tabler
    // itself uses, so the fetched `d` coordinates need no conversion. Stroke
    // width dropped from Tabler's own default of 2 to 1.5 (in that same
    // 24x24 space) so it reads thin/delicate once scaled down to our small
    // icon size, matching this UI's other stroke weights, rather than
    // Tabler's bolder default at full size.
    std::unique_ptr<juce::Drawable> makeTablerIcon (const juce::String& innerPaths)
    {
        const juce::String svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
            "fill=\"none\" stroke=\"black\" stroke-width=\"1.5\" "
            "stroke-linecap=\"round\" stroke-linejoin=\"round\">" + innerPaths + "</svg>";

        if (auto xml = juce::XmlDocument::parse (svg))
        {
            auto drawable = juce::Drawable::createFromSVG (*xml);

            // The SVG string above bakes in stroke="black", which no palette
            // token can reach -- recolour to ink() here so every icon follows
            // the theme (a no-op while ink() IS black, but keeps the icons
            // from silently vanishing when the palette flips dark).
            if (drawable != nullptr)
                drawable->replaceColour (juce::Colours::black, EQLookAndFeel::ink());

            return drawable;
        }
        return nullptr;
    }

    // The 4 built-in presets -- these ship with the plugin (or are
    // hand-authored core presets the user wants permanently available) and
    // should never be removable via the right-click-delete gesture, unlike
    // any preset the user saves themselves later. Checked in two places
    // (showPresetMenu() skips wiring up the delete callback at all, and
    // confirmAndDeletePreset() re-checks as a second line of defence) so
    // there's no path that can delete one of these by accident.
    // Menu id for "Save as...", kept clear of the 1-based preset indices
    // that share the same menu.
    constexpr int kSaveAsItemId = 9001;

    bool isProtectedPreset (const juce::String& name)
    {
        static const char* const kProtected[] = { "default", "low cut", "vocal presence", "telephone" };
        for (auto* p : kProtected)
            if (name == p)
                return true;
        return false;
    }

    // Parses a typed frequency value, accepting an optional "k" shorthand for
    // kHz (e.g. "2.5k" or "2.5k Hz" -> 2500). String::getFloatValue() already
    // reads the leading numeric portion of a string and ignores whatever
    // trailing text follows it (so this works whether the user fully retyped
    // the field or just edited the number inside the existing "697 Hz" text).
    float parseFreqInput (const juce::String& text)
    {
        const auto t = text.trim().toLowerCase();
        const float value = t.getFloatValue();
        return t.containsChar ('k') ? value * 1000.0f : value;
    }
}

//==============================================================================
EQAudioProcessorEditor::EQAudioProcessorEditor (EQAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), curve (p),
      // Tabler Icons "chevron-left"/"chevron-right"/"arrow-back-up"/
      // "arrow-forward-up" -- exact path data as fetched from
      // github.com/tabler/tabler-icons/blob/main/icons/outline/.
      prevPresetButton ("<path d=\"M15 6l-6 6l6 6\" />"),
      nextPresetButton ("<path d=\"M9 6l6 6l-6 6\" />"),
      undoButton ("<path d=\"M9 14l-4 -4l4 -4\" />"
                  "<path d=\"M5 10h11a4 4 0 1 1 0 8h-1\" />"),
      redoButton ("<path d=\"M15 14l4 -4l-4 -4\" />"
                  "<path d=\"M19 10h-11a4 4 0 1 0 0 8h1\" />"),
      // Tabler Icons "question-mark" -- exact path data as fetched from
      // github.com/tabler/tabler-icons/blob/main/icons/outline/question-mark.svg.
      // The bare glyph, NOT "help-circle": the ringed version read as a
      // heavier, different-shaped object next to the flat outline icons
      // around it.
      helpButton ("<path d=\"M8 8a3.5 3 0 0 1 3.5 -3h1a3.5 3 0 0 1 3.5 3a3 3 0 0 1 -2 3a3 4 0 0 0 -2 4\" />"
                  "<path d=\"M12 19l0 .01\" />"),
      // Declared after the icon buttons in the header, so initialised here.
      gainPill (p),
      // Tabler Icons "power" -- the same glyph a band's own mute uses in
      // the graph's action strip, so "on/off" reads identically everywhere.
      bypassButton ("<path d=\"M7 6a7.75 7.75 0 1 0 10 0\" />"
                    "<path d=\"M12 4l0 8\" />")
{
    // Monochrome colours + the custom rotary-knob drawing both live in
    // EQLookAndFeel now, so any future knob gets this style automatically.
    setLookAndFeel (&lookAndFeel);

    // The design-space canvas every control below is added onto (see the
    // header) -- the editor's only direct child besides the corner resizer.
    addAndMakeVisible (content);

    // Top bar.
    presetNameLabel.setJustificationType (juce::Justification::centred);
    presetNameLabel.setColour (juce::Label::textColourId, EQLookAndFeel::ink());
    content.addAndMakeVisible (presetNameLabel);
    content.addAndMakeVisible (prevPresetButton);
    content.addAndMakeVisible (nextPresetButton);
    content.addAndMakeVisible (undoButton);
    content.addAndMakeVisible (redoButton);
    content.addAndMakeVisible (helpButton);
    helpButton.onClick = [this]
    {
        const bool show = ! guideOverlay.isVisible();
        guideOverlay.setVisible (show);

        // toFront() every time, not just once at construction: JUCE paints
        // and hit-tests children in insertion order, so anything added
        // after the overlay would otherwise sit on top of it and the panel
        // would open invisibly behind the graph.
        if (show)
            guideOverlay.toFront (true);
    };

    // Doesn't load a preset here -- the live APVTS state at editor construction
    // is whatever the DAW session already had (via setStateInformation) or
    // whatever the user was last editing; forcibly loading "Init" every time
    // the editor window is reopened would silently wipe that out. The label
    // just starts on a neutral placeholder until the user actually browses.
    updatePresetNameLabel();
    refreshPresetList();

    // Fires for every parameter change regardless of source (mouse, typed
    // edit, host automation, undo/redo) -- see valueTreePropertyChanged().
    processorRef.apvts.state.addListener (this);

    prevPresetButton.onClick = [this]
    {
        if (presetNames.isEmpty()) return;
        const int n = presetNames.size();
        loadPresetAtIndex (((currentPresetIndex < 0 ? 0 : currentPresetIndex) - 1 + n) % n);
    };
    nextPresetButton.onClick = [this]
    {
        if (presetNames.isEmpty()) return;
        const int n = presetNames.size();
        loadPresetAtIndex (((currentPresetIndex < 0 ? 0 : currentPresetIndex) + 1) % n);
    };
    presetNameLabel.onClicked      = [this] { showPresetMenu(); };
    presetNameLabel.onRightClicked = [this] { showPresetContextMenu(); };

    content.addAndMakeVisible (curve);
    curve.onSelectionChanged = [this] { selectionChanged(); };
    curve.onHintChanged = [this] (juce::String h) { readoutPill.setHint (h); };

    // Added AFTER the curve so it paints on top of it (JUCE draws children
    // in insertion order) -- the pill floats over the graph.
    content.addAndMakeVisible (readoutPill);
    readoutPill.curveBelow = &curve; // frost backdrop + click-through (see ReadoutPill)

    // Child of the PILL, so it rides along with the pill's fade and is
    // hidden automatically whenever a hint takes the pill over.
    readoutPill.addAndMakeVisible (shapeControl);
    shapeControl.onClick = [this]
    {
        if (displayedBand >= 0)
            curve.showTypeMenu (displayedBand, shapeControl.getScreenBounds());
    };

    readoutPill.addAndMakeVisible (deleteControl);
    deleteControl.onClick = [this]
    {
        if (displayedBand >= 0)
            curve.removeBand (displayedBand);
    };

    readoutPill.addAndMakeVisible (dynControl);
    dynControl.onClick = [this]
    {
        if (displayedBand < 0)
            return;

        auto& a = processorRef.apvts;
        const bool wasOn = a.getRawParameterValue (
            EQAudioProcessor::bandParamID (displayedBand, "dynon"))->load() > 0.5f;

        setBandParamFromEditor ("dynon", wasOn ? 0.0f : 1.0f);

        // First engage spawns the handle at a small COMPRESSION range, so
        // the ring appears visibly separated from the dot and immediately
        // grabbable -- at range 0 it would sit exactly under the dot, which
        // wins the overlap (see dynHandleAtPosition). -3 dB is also simply
        // the common case. Re-engaging keeps whatever range was set before,
        // same as Pro-Q.
        const float range = a.getRawParameterValue (
            EQAudioProcessor::bandParamID (displayedBand, "dynrange"))->load();
        if (! wasOn && std::abs (range) < 0.05f)
            setBandParamFromEditor ("dynrange", -3.0f);
    };

    // --- selected-band panel (row 1) -----------------------------------------
    // (The band-shape dropdown no longer lives here -- it's the SHAPE cell
    // of the on-graph action strip, which opens the type menu; see
    // EQCurveDisplay::showTypeMenu(), which also carries the old
    // dropdown's switch-to-HP/LP Q-clamp side effect. Row 1 is just the
    // freq/gain/Q readout now.)

    // Freq/gain/Q, each independently double-click-editable. selectAll() on
    // editor-show so the user can just start typing over the whole value
    // instead of having to manually clear "697 Hz" first.
    setupEditableLabel (freqLabel, [this]
    {
        const float v = juce::jlimit (20.0f, 20000.0f, parseFreqInput (freqLabel.getText()));
        setBandParamFromEditor ("freq", v);
    });
    setupEditableLabel (gainLabel, [this]
    {
        const int b = curve.getSelectedBand();
        if (b < 0) return;
        const int t = (int) processorRef.apvts.getRawParameterValue (
            EQAudioProcessor::bandParamID (b, "type"))->load();
        if (t < 0 || t > 2) return; // High-Pass/Low-Pass have no gain -- nothing to write
        const float v = juce::jlimit (-18.0f, 18.0f, gainLabel.getText().getFloatValue());
        setBandParamFromEditor ("gain", v);
    });
    setupEditableLabel (qLabel, [this]
    {
        const int b = curve.getSelectedBand();
        float maxQ = 10.0f;
        if (b >= 0)
        {
            const int t = (int) processorRef.apvts.getRawParameterValue (
                EQAudioProcessor::bandParamID (b, "type"))->load();
            maxQ = EQBand::maxQFor (static_cast<EQBand::FilterType> (t));
        }
        const float v = juce::jlimit (0.1f, maxQ, qLabel.getText().getFloatValue());
        setBandParamFromEditor ("q", v);
    });

    // (Mute/power, listen, and delete no longer live in this row -- they're
    // the on-graph ACTION strip floating above a hovered band dot, next to
    // the slope strip. See EQCurveDisplay's hover strips.)

    // Slope buttons -- radio-style (only one active at a time), so clicking
    // doesn't toggle its OWN state, it just writes the parameter and lets
    // (The 12/24/36/48 slope selector no longer lives in this row -- it
    // appears ON the graph, floating above a hovered High-Pass/Low-Pass
    // band dot. See EQCurveDisplay's slope strip.)

    // --- global controls, all floating ON the graph now ---------------------
    // Added after `curve` so they paint on top of it.
    content.addAndMakeVisible (gainPill);

    // (The dB range selector lives ON the graph now -- see EQCurveDisplay's
    // range bubble -- not as a ComboBox down here.)

    undoButton.onClick = [this] { processorRef.undo(); };
    redoButton.onClick = [this] { processorRef.redo(); };
    undoButton.setEnabled (processorRef.canUndo());
    redoButton.setEnabled (processorRef.canRedo());

    // "AudioFlower" brand mark, centred in row 2's leftover middle space.
    // Embedded as a binary resource (see EQ.jucer's Resources group) rather
    // than looked up by system font name -- a name lookup only works on a
    // machine that happens to have Indie Flower installed, which won't be
    // true once this plugin is used anywhere else.
    logoTypeface = juce::Typeface::createSystemTypefaceFor (
        BinaryData::IndieFlowerRegular_ttf, (size_t) BinaryData::IndieFlowerRegular_ttfSize);
    logoLabel.setComponentID ("logoLabel"); // see EQLookAndFeel::getLabelFont()
    logoLabel.setText ("AudioFlower", juce::dontSendNotification);
    logoLabel.setFont (juce::Font (juce::FontOptions (26.0f).withTypeface (logoTypeface)));
    logoLabel.setColour (juce::Label::textColourId, EQLookAndFeel::ink());
    logoLabel.setJustificationType (juce::Justification::centred);
    logoLabel.setAlpha (0.10f);

    // Purely decorative -- it must never take a click. Its bounds span the
    // graph's FULL width at the same height as the In/Out pills and the
    // bypass toggle, and it's added after them, so without this it sits on
    // top as an invisible blanket and swallows every interaction in that
    // strip (which is exactly what happened when it became a watermark).
    logoLabel.setInterceptsMouseClicks (false, false);
    content.addAndMakeVisible (logoLabel);

    // Bypass — a real, host-automatable/native-bypass-reporting parameter
    // (see EQAudioProcessor::getBypassParameter()), grouped with dbRangeBox
    // on the right of row 2.
    bypassButton.setClickingTogglesState (true);
    content.addAndMakeVisible (bypassButton);
    bypassAtt = std::make_unique<APVTS::ButtonAttachment> (processorRef.apvts, "bypass", bypassButton);

   #if JUCE_DEBUG
    // So the debug-only hi-res capture shortcut can receive key events at
    // all (see keyPressed). Release builds don't take focus, exactly as
    // before.
    setWantsKeyboardFocus (true);
   #endif

    // Hints shown in the readout pill while these are hovered. All lower
    // case, matching the top bar's own "presets" label.
    //
    // Budget is 28 CHARACTERS, not the pill's raw pixel width: the pill is
    // sized from the worst-case value text ("20.00 kHz  " + "-18.0 dB  " +
    // "Q 10.00"), which is 28 monospace characters at 14px. Stay at or
    // under that and a hint can render at the same size as the numbers it
    // replaces; go over and it has to shrink to fit (see ReadoutPill::paint)
    // and visibly stops matching them.
    registerHint (undoButton,       "undo the last change");
    registerHint (redoButton,       "redo the last change");
    registerHint (bypassButton,     "bypass the plugin");
    registerHint (helpButton,       "open the quick guide");
    registerHint (presetNameLabel,  "click to browse");
    registerHint (prevPresetButton, "previous preset");
    registerHint (nextPresetButton, "next preset");
    registerHint (gainPill,         "drag to set, toggle in/out");


    // Added LAST so it sits above every other child (JUCE paints in
    // insertion order); hidden until the "?" is clicked.
    content.addChildComponent (guideOverlay);

    selectionChanged();   // set the panel to its initial (no selection) state

    startTimerHz (30);

    // User-resizable, aspect-locked to the design space -- the window
    // resizes by SCALE, not reflow: resized() stretches `content` (which
    // every control lives on) with one transform, so the absolute-pixel
    // layout survives untouched at any size. This replaces the earlier
    // fixed setScaleFactor(1.2f) approach; the default below started as
    // that same 20%-up look (864) and was bumped to 1.3x (936x701) at the
    // user's request ("default to a little bigger") -- just the starting
    // point for the corner-drag resizer either way. The chosen size is
    // remembered on the PROCESSOR (survives closing/reopening the editor)
    // and rides along in the saved session state (survives project reload
    // -- see getStateInformation), though never in preset files.
    // Our own constrainer (see SizeMemoryConstrainer in the header) instead
    // of setResizeLimits' built-in default -- installed BEFORE setResizable
    // so the corner resizer is created already wired to it. Limits/aspect
    // are set directly on it (setResizeLimits only talks to the default).
    // Free aspect (no setFixedAspectRatio any more): width and height each
    // do their own thing, and the graph absorbs whatever the chrome doesn't
    // need. Mins keep the top bar + readout pill + a usable slice of graph;
    // the max is generous on purpose -- more graph is the whole point.
    sizeConstrainer.setSizeLimits (540, 404, 2400, 1500);
    setConstrainer (&sizeConstrainer);
    setResizable (true, true);
    {
        const int w = processorRef.lastEditorWidth > 0 ? processorRef.lastEditorWidth : 936;
        // Height remembered independently since reflow; a session saved
        // before that has only the width, so derive the old locked-aspect
        // height from it -- the window comes back exactly as it looked.
        const int h = processorRef.lastEditorHeight > 0
                    ? processorRef.lastEditorHeight
                    : juce::roundToInt ((double) w * designHeight / (double) designWidth);
        setSize (w, h);
    }

    // Some hosts impose their own (stale) frame size on the editor during
    // the attach sequence, right after this constructor -- clobbering the
    // size we just restored. One deferred re-apply after the open sequence
    // settles wins that exchange: it runs from the message queue, so it
    // fires after the host's own synchronous sizing, and the wrapper then
    // propagates OUR size back up to the host window. SafePointer in case
    // the editor is torn down before the queue drains.
    juce::Component::SafePointer<EQAudioProcessorEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr)
            return;

        const int w = safeThis->processorRef.lastEditorWidth;
        const int h = safeThis->processorRef.lastEditorHeight;
        const int wantH = h >= 404 ? h
                        : juce::roundToInt ((double) w * designHeight / (double) designWidth);
        if (w >= 540 && (w != safeThis->getWidth() || wantH != safeThis->getHeight()))
            safeThis->setSize (w, wantH);
    });
}

EQAudioProcessorEditor::~EQAudioProcessorEditor()
{
    processorRef.apvts.state.removeListener (this);
    stopTimer();
    setLookAndFeel (nullptr);
}

//==============================================================================
void EQAudioProcessorEditor::setupEditableLabel (juce::Label& label, std::function<void()> onCommit)
{
    // Left-aligned, not centred: the gap between values is embedded as
    // literal trailing spaces in each label's own text (see updateReadout()),
    // matching exactly how the original single combined label produced its
    // spacing ("697 Hz" + "  " + "1.9 dB" + ...). With CENTRED justification,
    // trailing spaces get split half-before/half-after the visible glyphs
    // instead of purely trailing after them -- left alignment keeps the
    // gap where it's supposed to be, after the number.
    label.setJustificationType (juce::Justification::centredLeft);
    label.setColour (juce::Label::textColourId, EQLookAndFeel::ink());
    // Label::setFont() overrides whatever the LookAndFeel would otherwise
    // supply, so this has to name the typeface explicitly too.
    label.setFont (EQLookAndFeel::uiFont (14.0f));

    // Label reserves its own internal border by default (a few px each
    // side) that our tight-packed width math in layoutReadoutLabels() didn't
    // account for, so the box came out a hair too narrow for its own text --
    // JUCE's default response to that is to horizontally SQUISH the font to
    // cram it in, rather than clip it (that's the "squished text" bug).
    // Zeroing the border and disabling that auto-squish (never scale below
    // 100%) means our own explicit width math is the only thing that matters.
    label.setBorderSize (juce::BorderSize<int> (0));
    label.setMinimumHorizontalScale (1.0f);

    // Double-click to edit (not single-click, which stays reserved for
    // point-selection elsewhere on the graph); commits on Return OR losing
    // focus (doesn't discard on blur) -- matches how most plugin numeric
    // fields behave.
    label.setEditable (false, true, false);
    label.onEditorShow = [&label]
    {
        if (auto* ed = label.getCurrentTextEditor())
            ed->selectAll(); // start typing over the whole value immediately
    };
    label.onTextChange = std::move (onCommit);

    // Child of the readout PILL (which floats on the graph), not the
    // canvas -- so the three values move as one group with their backdrop.
    readoutPill.addAndMakeVisible (label);
}


void EQAudioProcessorEditor::registerHint (juce::Component& c, juce::String text)
{
    hints[&c] = std::move (text);
    c.addMouseListener (this, true); // true: also fires for the control's children
}

void EQAudioProcessorEditor::mouseEnter (const juce::MouseEvent& e)
{
    // eventComponent may be a CHILD of the registered control (see the
    // addMouseListener flag above), so walk up until a registered ancestor
    // is found.
    for (auto* c = e.eventComponent; c != nullptr; c = c->getParentComponent())
        if (auto it = hints.find (c); it != hints.end())
        {
            readoutPill.setHint (it->second);
            return;
        }
}

void EQAudioProcessorEditor::mouseExit (const juce::MouseEvent&)
{
    readoutPill.setHint ({});
}

#if JUCE_DEBUG
bool EQAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress ('s', juce::ModifierKeys::ctrlModifier
                                  | juce::ModifierKeys::shiftModifier, 0))
    {
        saveHiResSnapshot (3);
        return true;
    }

    return false; // everything else falls through to the host as usual
}

void EQAudioProcessorEditor::saveHiResSnapshot (int scale)
{
    // Renders the editor and every child offscreen at `scale`, including the
    // content canvas's own transform -- so the result is the exact layout on
    // screen, just rasterised bigger.
    const auto image = createComponentSnapshot (getLocalBounds(), false, (float) scale);

    if (! image.isValid())
        return;

    // getNonexistentSibling() means repeated captures never overwrite each
    // other: EQ-hires.png, EQ-hires (2).png, and so on.
    const auto file = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                          .getChildFile ("EQ-hires.png")
                          .getNonexistentSibling();

    if (auto stream = file.createOutputStream())
    {
        juce::PNGImageFormat png;
        png.writeImageToStream (image, *stream);
    }
}
#endif

void EQAudioProcessorEditor::setBandParamFromEditor (juce::StringRef name, float value)
{
    // The DISPLAYED band (see updateReadout), not the selected one -- what
    // you're looking at is what you edit.
    const int b = displayedBand;
    if (b < 0) return;

    if (auto* prm = processorRef.apvts.getParameter (EQAudioProcessor::bandParamID (b, name)))
    {
        // New transaction boundary -- same reasoning as EQCurveDisplay::
        // beginGesture(): this writes directly, not through a
        // ParameterAttachment, so it doesn't get one automatically.
        processorRef.beginNewTransaction();
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (value));
        prm->endChangeGesture();
    }
}

void EQAudioProcessorEditor::refreshPresetList()
{
    presetNames = processorRef.getPresetNames();
}

void EQAudioProcessorEditor::updatePresetNameLabel()
{
    const bool hasRealPreset = (currentPresetIndex >= 0 && currentPresetIndex < presetNames.size());
    const auto baseName = hasRealPreset ? presetNames[currentPresetIndex] : juce::String ("presets");

    // The " *" only makes sense for a REAL loaded preset (it means "differs
    // from what's on disk") -- the "presets" placeholder state has no file to
    // differ from, so it never gets the asterisk even after edits.
    presetNameLabel.setText ((presetModified && hasRealPreset) ? baseName + " *" : baseName,
                             juce::dontSendNotification);
    resized(); // presetNameLabel's own width depends on its text
}

void EQAudioProcessorEditor::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    // Any parameter write -- mouse drag, typed edit, host automation,
    // undo/redo -- lands here. loadPresetAtIndex()/showSavePresetDialog()'s
    // callback both reset presetModified back to false AFTER calling into
    // apvts (which itself fires this listener many times during the load/
    // save), so those two don't get stuck flagged as modified.
    if (! presetModified)
    {
        presetModified = true;
        updatePresetNameLabel();
    }
}

void EQAudioProcessorEditor::loadPresetAtIndex (int index)
{
    if (index < 0 || index >= presetNames.size())
        return;

    if (processorRef.loadPreset (presetNames[index]))
    {
        currentPresetIndex = index;
        presetModified = false; // freshly loaded -- matches disk exactly
        updatePresetNameLabel();
        repaint();

        // A whole-state preset load isn't itself undoable (replaceState()
        // swaps the tree directly, bypassing the per-property undo path
        // every other write in this plugin goes through -- see
        // EQAudioProcessor::loadPreset()/apvts.replaceState()'s own comment
        // about clearing undo history), so reflect that immediately rather
        // than leaving stale enabled-looking undo/redo icons around.
        undoButton.setEnabled (processorRef.canUndo());
        redoButton.setEnabled (processorRef.canRedo());
    }
}

void EQAudioProcessorEditor::styleAlert (juce::AlertWindow& aw)
{
    // A modal AlertWindow isn't a child of this editor, so it inherits
    // neither the LookAndFeel nor any colour from it -- both have to be set
    // explicitly, same situation as the popup menus elsewhere in this UI.
    aw.setLookAndFeel (&lookAndFeel);
    aw.setColour (juce::AlertWindow::backgroundColourId, EQLookAndFeel::background());
    aw.setColour (juce::AlertWindow::textColourId, EQLookAndFeel::ink());
    aw.setColour (juce::AlertWindow::outlineColourId, EQLookAndFeel::ink());
    aw.setColour (juce::TextEditor::backgroundColourId, EQLookAndFeel::background());
    aw.setColour (juce::TextEditor::textColourId, EQLookAndFeel::ink());
    aw.setColour (juce::TextEditor::outlineColourId, EQLookAndFeel::ink());
    aw.setColour (juce::TextEditor::focusedOutlineColourId, EQLookAndFeel::ink());
    aw.setColour (juce::TextButton::buttonColourId, EQLookAndFeel::background());
    aw.setColour (juce::TextButton::textColourOffId, EQLookAndFeel::ink());
    aw.setColour (juce::ComboBox::outlineColourId, EQLookAndFeel::ink());
}

void EQAudioProcessorEditor::showMessage (const juce::String& title, const juce::String& message,
                                          std::function<void()> onDismissed)
{
    auto* aw = new juce::AlertWindow (title, message, juce::AlertWindow::NoIcon);
    styleAlert (*aw);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey),
                            juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [cb = std::move (onDismissed)] (int)
        {
            if (cb != nullptr)
                cb();
        }), true);

    aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
}

void EQAudioProcessorEditor::confirmReplacePreset (const juce::String& name,
                                                   std::function<void()> onConfirmed)
{
    auto* aw = new juce::AlertWindow ("Replace Preset",
                                      "\"" + name + "\" already exists. Replace it?",
                                      juce::AlertWindow::NoIcon);
    styleAlert (*aw);

    // Cancel is the RETURN key here, unlike the save/rename dialogs -- this
    // one is reached by accident (you just typed a name that happens to
    // collide) and the destructive answer shouldn't be one blind Enter away.
    aw->addButton ("Replace", 1);
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::returnKey),
                                juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [cb = std::move (onConfirmed)] (int result)
        {
            if (result == 1 && cb != nullptr)
                cb();
        }), true);

    aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
}

void EQAudioProcessorEditor::showRenamePresetDialog (const juce::String& oldName)
{
    if (isProtectedPreset (oldName))
        return; // factory presets keep their names -- same rule as delete

    auto* aw = new juce::AlertWindow ("Rename Preset",
                                      "New name for \"" + oldName + "\":",
                                      juce::AlertWindow::NoIcon);
    styleAlert (*aw);
    aw->addTextEditor ("name", oldName);
    aw->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis = juce::Component::SafePointer<EQAudioProcessorEditor> (this), aw, oldName] (int result)
        {
            if (result != 1 || safeThis == nullptr)
                return;

            // Read the text NOW (aw is deleted on dismissal), then hand off
            // asynchronously -- applyPresetName can open another dialog, and
            // entering a modal state from inside a modal callback is exactly
            // the nesting worth not relying on.
            const auto typed = aw->getTextEditorContents ("name");
            juce::MessageManager::callAsync ([safeThis, typed, oldName]
            {
                if (safeThis != nullptr)
                    safeThis->applyPresetName (typed, oldName);
            });
        }), true);

    aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
}

void EQAudioProcessorEditor::applyPresetName (const juce::String& rawName, const juce::String& oldName)
{
    const auto name     = rawName.trim();
    const bool isRename = oldName.isNotEmpty();

    // Nothing to do -- an empty box, or a rename to the name it already has.
    if (name.isEmpty() || (isRename && name == oldName))
        return;

    // Re-opens whichever dialog we came from, so a rejected name is a
    // correction rather than a dead end that loses what was typed.
    auto reopen = [safeThis = juce::Component::SafePointer<EQAudioProcessorEditor> (this),
                   oldName, isRename]
    {
        juce::MessageManager::callAsync ([safeThis, oldName, isRename]
        {
            if (safeThis == nullptr)
                return;

            if (isRename) safeThis->showRenamePresetDialog (oldName);
            else          safeThis->showSavePresetDialog();
        });
    };

    // Factory presets are read-only by NAME: they can't be deleted, so
    // letting a save quietly overwrite one would be a hole in the same rule.
    // ensureFactoryPresetsExist() only restores them when the folder is
    // completely empty, so a clobbered factory preset is effectively gone.
    if (isProtectedPreset (name))
    {
        showMessage ("Factory Preset",
                     "\"" + name + "\" is a factory preset and can't be replaced. "
                     "Choose a different name.",
                     reopen);
        return;
    }

    // SafePointer, not a raw `this`: when a replace confirmation is needed
    // this runs from that dialog's callback, and a modal AlertWindow is a
    // top-level window -- closing the plugin editor underneath it leaves it
    // standing, so the button can still be clicked after we're gone.
    auto commit = [safeThis = juce::Component::SafePointer<EQAudioProcessorEditor> (this),
                   name, oldName, isRename]
    {
        if (safeThis == nullptr)
            return;

        auto& self = *safeThis;

        // Capture the loaded preset by NAME, not index: the list is sorted,
        // so saving or renaming can reshuffle every index around it.
        const auto loadedName = (self.currentPresetIndex >= 0
                              && self.currentPresetIndex < self.presetNames.size())
                              ? self.presetNames[self.currentPresetIndex] : juce::String();

        if (isRename)
        {
            if (! self.processorRef.renamePreset (oldName, name))
                return;
        }
        else
        {
            self.processorRef.savePresetAs (name);
        }

        self.refreshPresetList();

        if (isRename)
        {
            // Renaming the LOADED preset follows it; renaming any other one
            // leaves what's loaded alone (just possibly at a new index).
            // indexOf returns -1 when it's gone, which is the right value.
            self.currentPresetIndex = self.presetNames.indexOf (loadedName == oldName ? name : loadedName);

            // Renaming SOME OTHER preset ONTO the loaded one's name replaced
            // that file's contents (the overwrite was confirmed above). The
            // label still reads the same, but what's loaded no longer matches
            // what's on disk -- so it's modified, not clean.
            if (loadedName == name && loadedName != oldName)
                self.presetModified = true;
        }
        else
        {
            self.currentPresetIndex = self.presetNames.indexOf (name);
            self.presetModified = false; // just saved -- matches disk exactly
        }

        self.updatePresetNameLabel();
        self.repaint();
    };

    if (processorRef.presetExists (name))
        confirmReplacePreset (name, std::move (commit));
    else
        commit();
}

void EQAudioProcessorEditor::showPresetRowMenu (const juce::String& name)
{
    // Factory presets get the menu too, with both items DISABLED rather than
    // no menu at all. Right-clicking one used to dismiss the list and do
    // nothing, with no explanation -- indistinguishable from a dropped click.
    // Dimmed rows say "these exist, not for this preset"; drawPopupMenuItem
    // already renders an inactive row at half alpha, so this needs no new
    // drawing. showRenamePresetDialog/confirmAndDeletePreset still re-check
    // the name themselves, so this is presentation, not the enforcement.
    const bool locked = isProtectedPreset (name);

    juce::PopupMenu menu;
    menu.addItem (1, "Rename...", ! locked);
    menu.addItem (2, "Delete...", ! locked);

    showMenuUnderPresetName (menu,
        juce::jmax (plainMenuRowWidth ("Rename..."), plainMenuRowWidth ("Delete...")),
        [this, name] (int result)
        {
            if (result == 1)      showRenamePresetDialog (name);
            else if (result == 2) confirmAndDeletePreset (name);
        });
}

void EQAudioProcessorEditor::showSavePresetDialog()
{
    // Raw pointer + deleteWhenDismissed=true (not a smart pointer member) --
    // JUCE's own convention for a one-shot modal AlertWindow: the modal
    // manager owns its lifetime from here on, deleting it once dismissed.
    auto* aw = new juce::AlertWindow ("Save Preset", "Enter a name for this preset:",
                                      juce::AlertWindow::NoIcon);

    // Match the plugin's own monochrome look -- a modal AlertWindow doesn't
    // inherit the editor's LookAndFeel automatically (it's not a child of the
    // editor's component tree), so it needs both the LookAndFeel assigned
    // AND its own colours set explicitly, same pattern already used for the
    // dropdowns elsewhere in this UI.
    styleAlert (*aw);

    // The BASE name (currentPresetIndex's own name, or blank if nothing's
    // loaded), not presetNameLabel.getText() -- that may carry a trailing
    // " *" right now (see updatePresetNameLabel()), or the "presets" empty-
    // state placeholder, neither of which should get suggested as the new
    // preset's name.
    const auto baseName = (currentPresetIndex >= 0 && currentPresetIndex < presetNames.size())
                         ? presetNames[currentPresetIndex] : juce::String();
    aw->addTextEditor ("name", baseName);
    aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis = juce::Component::SafePointer<EQAudioProcessorEditor> (this), aw] (int result)
        {
            if (result != 1 || safeThis == nullptr)
                return;

            // Read the text NOW (aw is deleted on dismissal), then hand off
            // asynchronously -- applyPresetName may open a confirm dialog,
            // and entering a modal state from inside a modal callback is
            // exactly the nesting worth not relying on.
            const auto typed = aw->getTextEditorContents ("name");
            juce::MessageManager::callAsync ([safeThis, typed]
            {
                if (safeThis != nullptr)
                    safeThis->applyPresetName (typed, {});
            });
        }), true);

    // Centre over the PLUGIN window, not the screen -- positioned after
    // enterModalState, once the window has its real laid-out size (centring
    // a window that hasn't sized itself yet lands off-centre).
    aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
}

EQAudioProcessorEditor::PresetMenuItem::PresetMenuItem (juce::String name, bool ticked,
                                                        std::function<void()> onContextMenuIn)
    : juce::PopupMenu::CustomComponent (false),
      presetName (std::move (name)), isTicked (ticked),
      onContextMenu (std::move (onContextMenuIn))
{
}

void EQAudioProcessorEditor::PresetMenuItem::paint (juce::Graphics& g)
{
    // Solid black highlight / white text on hover -- matching
    // EQLookAndFeel::drawPopupMenuItem()'s own highlight treatment (driven
    // by the monochrome ColourScheme's highlightedFill/highlightedText) so
    // this custom-drawn menu doesn't read as a different, subtler style
    // next to the band-shape/dB-range dropdowns' menus.
    if (isItemHighlighted())
    {
        g.setColour (EQLookAndFeel::ink());
        g.fillRect (getLocalBounds());
        g.setColour (EQLookAndFeel::background());
    }
    else
    {
        g.setColour (EQLookAndFeel::ink());
    }

    g.setFont (EQLookAndFeel::uiFont (14.0f));
    const auto text = (isTicked ? juce::String (juce::CharPointer_UTF8 ("\xE2\x9C\x93 ")) : juce::String())
                     + presetName;
    // Centred, matching EQLookAndFeel::drawPopupMenuItem()'s treatment of
    // every other menu (type menu, dB range) -- this menu uses its own
    // custom-drawn rows (see PresetMenuItem) instead of going through that
    // override, so it needs the same centred alignment set explicitly here.
    g.drawText (text, getLocalBounds().reduced (8, 0), juce::Justification::centred);
}

void EQAudioProcessorEditor::PresetMenuItem::getIdealSize (int& idealWidth, int& idealHeight)
{
    // Sized from the actual text (worst-case: reserve room for the tick
    // prefix every row, even unticked ones, so the menu doesn't visibly
    // widen/shift depending on which preset happens to be current) rather
    // than a fixed guessed width -- same "hug the real text" approach the
    // band-shape dropdown's own box width already uses, instead of the
    // wide fixed 180px this used before.
    const auto font = EQLookAndFeel::uiFont (14.0f);
    const int tickW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (
        font, juce::String (juce::CharPointer_UTF8 ("\xE2\x9C\x93 "))));
    idealWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, presetName)) + tickW + 16;
    idealHeight = 22;
}

void EQAudioProcessorEditor::PresetMenuItem::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        // Right-click: close the menu and offer Rename/Delete for this row
        // instead of selecting/loading it.
        juce::PopupMenu::dismissAllActiveMenus();
        if (onContextMenu != nullptr)
            onContextMenu();
    }
    else
    {
        triggerMenuItem(); // normal left-click select/load, same as a plain addItem() row
    }
}

int EQAudioProcessorEditor::plainMenuRowWidth (const juce::String& text)
{
    // Mirrors EQLookAndFeel::getIdealPopupMenuItemSize exactly.
    return juce::roundToInt (juce::GlyphArrangement::getStringWidth (EQLookAndFeel::uiFont (14.0f), text))
         + 16 + EQLookAndFeel::kMenuMarkZone * 2;
}

void EQAudioProcessorEditor::showMenuUnderPresetName (juce::PopupMenu& menu, int menuWidth,
                                                      std::function<void (int)> onResult)
{
    menu.setLookAndFeel (&lookAndFeel);

    // withTargetScreenArea, NOT withTargetComponent -- and that choice does
    // two things at once:
    //
    // 1. SIZE. A menu only scales with its target when a target COMPONENT is
    //    set (see PopupMenu::MenuWindow's ctor: no target component means the
    //    scale factor stays 1). Every other menu in this plugin -- the type
    //    menu, the dB range menu -- is anchored by screen area and therefore
    //    renders unscaled, so a menu anchored by component would come out
    //    visibly LARGER than its neighbours at the default 1.3x window scale.
    //
    // 2. POSITION. JUCE aligns a menu's LEFT edge to the target area's left
    //    edge, so a component target (or a zero-width area) anchors the menu
    //    rather than centring it -- it hangs off to the right of a label
    //    narrower than itself. Handing it an area the SAME WIDTH the menu
    //    will actually be makes the two left edges coincide, which is the
    //    same as centring them on each other.
    //
    // +4 is JUCE's own border (getPopupMenuBorderSize, 2 per side), which the
    // per-row ideal widths don't include.
    const auto area = presetNameLabel.getScreenBounds()
                          .withSizeKeepingCentre (menuWidth + 4, presetNameLabel.getHeight());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (area), std::move (onResult));
}

void EQAudioProcessorEditor::showPresetMenu()
{
    refreshPresetList(); // pick up anything saved/added since the last open

    juce::PopupMenu menu;

    // Saving lives HERE rather than as its own top-bar icon, so every
    // preset operation -- browse, load, save, right-click-delete -- shares
    // one entry point.
    menu.addItem (kSaveAsItemId, "Save as...");
    menu.addSeparator();

    for (int i = 0; i < presetNames.size(); ++i)
    {
        const auto name = presetNames[i];
        menu.addCustomItem (i + 1,
            std::make_unique<PresetMenuItem> (name, i == currentPresetIndex,
                [this, name]
                {
                    // Deferred: we were called from PresetMenuItem's own
                    // mouseUp right after dismissAllActiveMenus(), and opening
                    // a new menu while that teardown is still in flight is
                    // asking for trouble.
                    juce::MessageManager::callAsync (
                        [safeThis = juce::Component::SafePointer<EQAudioProcessorEditor> (this), name]
                        {
                            if (safeThis != nullptr)
                                safeThis->showPresetRowMenu (name);
                        });
                }));
    }

    // The menu is at least as wide as its "Save as..." row; the preset rows
    // are CustomComponents, so their width follows PresetMenuItem::
    // getIdealSize() instead (name + a tick prefix reserved on every row).
    const auto font = EQLookAndFeel::uiFont (14.0f);
    const int tickW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (
        font, juce::String (juce::CharPointer_UTF8 ("\xE2\x9C\x93 "))));

    int menuWidth = plainMenuRowWidth ("Save as...");
    for (auto& name : presetNames)
        menuWidth = juce::jmax (menuWidth,
            juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, name)) + tickW + 16);

    showMenuUnderPresetName (menu, menuWidth,
        [this] (int result)
        {
            if (result == kSaveAsItemId)
                showSavePresetDialog();
            else if (result > 0)
                loadPresetAtIndex (result - 1);
        });
}

void EQAudioProcessorEditor::showPresetContextMenu()
{
    // Right-click shortcut on the preset name -- straight to saving,
    // without going through the full browse list.
    juce::PopupMenu menu;
    menu.addItem (kSaveAsItemId, "Save as...");

    showMenuUnderPresetName (menu, plainMenuRowWidth ("Save as..."),
        [this] (int result)
        {
            if (result == kSaveAsItemId)
                showSavePresetDialog();
        });
}

void EQAudioProcessorEditor::confirmAndDeletePreset (const juce::String& name)
{
    if (isProtectedPreset (name))
        return; // second line of defence -- showPresetMenu() shouldn't even wire this up for these

    auto* aw = new juce::AlertWindow ("Delete Preset",
                                      "Delete \"" + name + "\"? This cannot be undone.",
                                      juce::AlertWindow::NoIcon);
    styleAlert (*aw);
    aw->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [safeThis = juce::Component::SafePointer<EQAudioProcessorEditor> (this), name] (int result)
    {
        if (result == 1 && safeThis != nullptr)
        {
            auto& self = *safeThis;

            // Capture the loaded preset by NAME before the list changes --
            // deleting shifts every index after it, so the stored index is
            // meaningless afterwards.
            const auto loadedName = (self.currentPresetIndex >= 0
                                  && self.currentPresetIndex < self.presetNames.size())
                                  ? self.presetNames[self.currentPresetIndex] : juce::String();
            const bool wasCurrentlyLoaded = (loadedName == name);

            self.processorRef.deletePreset (name);
            self.refreshPresetList();

            // The deleted preset can no longer be what's actually loaded --
            // fall back to the neutral placeholder rather than leaving the
            // label pointing at a file that no longer exists.
            if (wasCurrentlyLoaded)
            {
                self.currentPresetIndex = -1;
                self.presetModified = false;
                self.updatePresetNameLabel();
                self.repaint();
            }
            else
            {
                // Something else went; re-point at whatever IS loaded, since
                // its index has almost certainly shifted.
                self.currentPresetIndex = self.presetNames.indexOf (loadedName);
            }
        }
    }), true);

    // Centre over the plugin window, same as the save dialog (see
    // showSavePresetDialog()) -- the two should appear in the same place.
    aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
}

void EQAudioProcessorEditor::layoutReadoutLabels()
{
    // Packs freq/gain/Q tightly against their OWN current text width, left
    // to right inside the fixed readoutZone reserved in resized(). No extra
    // gap added between boxes here -- the visual gap is embedded as literal
    // trailing spaces in freq/gain's own text (see updateReadout()), exactly
    // matching the original single combined label's spacing convention.
    // readoutZone's own position/width never changes here, so the dropdown/
    // icons around it stay fixed regardless of how tightly the three values
    // end up packed on any given frame.
    const auto font = EQLookAndFeel::uiFont (14.0f);

    // Measure each label's own actual (tight) width first, so the group as
    // a WHOLE can be re-centred in readoutZone below -- readoutZone itself
    // is a fixed worst-case reservation (see resized()), so packing tight
    // widths from its left edge alone would leave the group visibly
    // off-centre (shifted left) any time the real text is shorter than the
    // worst case, e.g. "4.65 kHz" vs. the reserved "20.00 kHz".
    auto tightWidth = [&] (juce::Label& l)
    {
        return l.isBeingEdited() ? l.getWidth()
                                  : juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, l.getText())) + 2;
    };

    const int wFreq = tightWidth (freqLabel);
    const int wGain = tightWidth (gainLabel);
    const int wQ    = tightWidth (qLabel);

    auto zone = readoutZone.withSizeKeepingCentre (wFreq + wGain + wQ, readoutZone.getHeight());

    auto placeTight = [&] (juce::Label& l, int w)
    {
        if (! l.isBeingEdited())
            l.setBounds (zone.removeFromLeft (w));
        else
            zone.removeFromLeft (w); // leave its own bounds untouched mid-edit
    };

    placeTight (freqLabel, wFreq);
    placeTight (gainLabel, wGain);
    placeTight (qLabel, wQ);
}

//==============================================================================
void EQAudioProcessorEditor::timerCallback()
{
    // Keep both floating pills matched to the graph's background gradient
    // at their own heights, including through the bypass fade -- a flat
    // fill made them read as patches sitting on the surface rather than
    // part of it (darker near the top, lighter near the bottom).
    {
        const auto graph = curve.getBounds();
        const auto base  = EQLookAndFeel::background()
                               .interpolatedWith (EQLookAndFeel::dimmedBackground(), bypassDim);

        auto surfaceFor = [&] (const juce::Component& c)
        {
            return EQLookAndFeel::surfaceAt (base,
                (float) (c.getBounds().getCentreY() - graph.getY())
                    / (float) juce::jmax (1, graph.getHeight()));
        };

        readoutPill.setSurfaceColour (surfaceFor (readoutPill));
        gainPill.setSurfaceColour (surfaceFor (gainPill));
    }

    // Magnetic proximity for the pill controls, same language as the band
    // dots: full pull inside the control, fading out over ~26px of
    // approach. Fed from here because a child component can't see the
    // cursor until it's already inside its own bounds; getMouseXYRelative()
    // handles the content transform for free.
    {
        auto feed = [] (auto& comp)
        {
            const auto visual = comp.getLocalBounds().toFloat()
                                    .reduced ((float) std::decay_t<decltype(comp)>::kMagnetPad);
            const auto mp = comp.getMouseXYRelative().toFloat();

            const juce::Point<float> nearest (juce::jlimit (visual.getX(), visual.getRight(),  mp.x),
                                              juce::jlimit (visual.getY(), visual.getBottom(), mp.y));
            const float d = nearest.getDistanceFrom (mp); // 0 inside the visual rect

            comp.setMagnet (1.0f - juce::jlimit (0.0f, 1.0f, d / 26.0f));
        };

        feed (dynControl);
        feed (gainPill);
    }

    // The readout pill's frosted backdrop is a live view of the graph
    // beneath it (analyzer, curve) -- repaint it every tick so the frost
    // animates with them, not only when its own text changes.
    if (readoutPill.isVisible())
        readoutPill.repaint();

    updateReadout();      // keep the numbers live while dragging / automating

    // Reflect real undo/redo availability -- SmallIconButton dims itself
    // automatically when disabled (see its paintButton()), so this is the
    // only wiring needed to keep the icons from looking clickable when
    // there's nothing to undo/redo.
    undoButton.setEnabled (processorRef.canUndo());
    redoButton.setEnabled (processorRef.canRedo());

    // If the band currently being auditioned gets muted (mute can be toggled
    // from here, or automated by the host), snap listen mode off instead of
    // leaving it stuck engaged on a band that's no longer actually
    // processing -- polled here since mute isn't driven through a callback
    // we could otherwise hook.
    {
        const int lb = processorRef.getListenBand();
        if (lb >= 0)
        {
            const bool muted = processorRef.apvts.getRawParameterValue (
                EQAudioProcessor::bandParamID (lb, "mute"))->load() > 0.5f;
            if (muted)
            {
                processorRef.setListenBand (-1);
            }
        }
    }

    // Step the bypass-dim fade toward its target every tick (30 Hz), rather
    // than snapping it in the button's click handler -- "bypass" is a real
    // APVTS parameter that can also change via host automation, not just a
    // click here, so polling it is what keeps the fade correct either way.
    // ~0.12/tick at 30 Hz reaches the far end in a few hundred ms.
    const bool bypassed = processorRef.apvts.getRawParameterValue ("bypass")->load() > 0.5f;
    const float target = bypassed ? 1.0f : 0.0f;
    if (bypassDim != target)
    {
        constexpr float step = 0.12f;
        bypassDim = target > bypassDim ? juce::jmin (target, bypassDim + step)
                                       : juce::jmax (target, bypassDim - step);
        curve.setBackgroundDim (bypassDim);
        lookAndFeel.setDimAmount (bypassDim); // so the dropdowns' own white background blends too
        repaint();
    }

    // Step the readout pill's fade-in toward fully visible every tick, same
    // pattern as bypassDim above. Applied as the pill's own alpha (not just
    // a repaint hook) since Component::setAlpha() is what actually fades a
    // component's painting -- and it fades the pill's backdrop and its
    // three labels together, as one object.
    if (rowFadeAlpha < 1.0f)
    {
        constexpr float step = 0.15f;
        rowFadeAlpha = juce::jmin (1.0f, rowFadeAlpha + step);

        readoutPill.setAlpha (rowFadeAlpha);
        repaint();
    }
}

void EQAudioProcessorEditor::selectionChanged()
{
    const int b = curve.getSelectedBand();


    // Pill visibility + its fade now live in updateReadout(), since they
    // depend on hover state (and on whether a hint is showing), not just
    // on the selection.
    juce::ignoreUnused (b);
    updateReadout();
}

void EQAudioProcessorEditor::updateReadout()
{
    // Which band the pill shows: the HOVERED one when there is one, else
    // the selection -- so you can inspect a band without selecting it.
    // Frozen while the cursor is over the pill or a value is being typed,
    // otherwise moving from a band's dot up to the pill would flip the
    // reading back to the selected band mid-interaction.
    const bool pillBusy = readoutPill.isMouseOverOrDragging (true)
                       || freqLabel.isBeingEdited()
                       || gainLabel.isBeingEdited()
                       || qLabel.isBeingEdited();

    // A live drag beats the cursor-over-pill freeze: a drag FORWARDED
    // through the pill (grabbing a dot under the glass) keeps the cursor
    // over the pill the whole time, and the freeze would pin the readout
    // to whatever band it showed before the grab.
    if (curve.isDraggingBand())
    {
        displayedBand = curve.getSelectedBand();
    }
    else if (! pillBusy)
    {
        const int hovered = curve.getHoverBand();
        displayedBand = hovered >= 0 ? hovered : curve.getSelectedBand();
    }

    // Visible whenever there's something to say -- a band to report on, or
    // a hint to show. (Visibility used to live in selectionChanged(), but
    // it now depends on hover state too, which changes every tick.)
    //
    // EXCEPT while auditioning: holding the headphones is a listen-and-sweep
    // gesture, so everything that isn't the sound clears out of the way --
    // the pill here, the strip's icons on the graph side. Releasing brings
    // it all straight back (the pill re-fades in, see below).
    const bool wasVisible = readoutPill.isVisible();
    const bool shouldShow = (displayedBand >= 0 || readoutPill.hasHint())
                         && ! curve.isAuditioning();
    readoutPill.setVisible (shouldShow);

    if (shouldShow && ! wasVisible)
    {
        rowFadeAlpha = 0.0f;          // fade in, same as before
        readoutPill.setAlpha (0.0f);
    }

    const int b = displayedBand;

    // The glyph shows whatever the pill is reporting, but is only LIVE when
    // that's also the selected band -- clicking it would otherwise open a
    // type menu for a band whose numbers you aren't looking at.
    shapeControl.setEnabled (b >= 0 && b == curve.getSelectedBand());

    if (b >= 0)
        shapeControl.setType ((EQBand::FilterType) (int) processorRef.apvts
            .getRawParameterValue (EQAudioProcessor::bandParamID (b, "type"))->load());

    if (b < 0)
    {
        freqLabel.setText ("--", juce::dontSendNotification);
        gainLabel.setText ("--", juce::dontSendNotification);
        qLabel.setText ("--", juce::dontSendNotification);
        return;
    }

    auto& a = processorRef.apvts;
    const float f = a.getRawParameterValue (EQAudioProcessor::bandParamID (b, "freq"))->load();
    const float g = a.getRawParameterValue (EQAudioProcessor::bandParamID (b, "gain"))->load();
    const float q = a.getRawParameterValue (EQAudioProcessor::bandParamID (b, "q"))->load();
    const int   t = (int) a.getRawParameterValue (EQAudioProcessor::bandParamID (b, "type"))->load();
    const bool  hasGain = (t >= 0 && t <= 2);

    // Trailing "  " (two spaces) on freq/gain match exactly how the
    // original single combined label produced its spacing -- with left
    // alignment (see setupEditableLabel), this puts the gap purely AFTER the
    // number, reproducing the old look exactly instead of approximating it
    // with external padding/gaps. Q is last, so it needs no trailing space.
    const juce::String fs = (f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz"
                                          : juce::String (f, 0) + " Hz") + "  ";
    const juce::String gs = (hasGain ? juce::String (g, 1) + " dB" : "-- dB") + "  ";
    const juce::String qs = "Q " + juce::String (q, 2);

    // Skip refreshing whichever label the user currently has open for
    // editing -- overwriting it every tick would clobber their in-progress
    // typing before they've committed it.
    if (! freqLabel.isBeingEdited()) freqLabel.setText (fs, juce::dontSendNotification);
    if (! gainLabel.isBeingEdited()) gainLabel.setText (gs, juce::dontSendNotification);
    if (! qLabel.isBeingEdited())    qLabel.setText (qs, juce::dontSendNotification);

    layoutReadoutLabels(); // re-pack tightly now that the text may have changed

    // Gain isn't meaningful for High-Pass/Low-Pass -- don't let it be
    // double-clicked into editing when there's nothing real to type in.
    gainLabel.setEditable (false, hasGain, false);

    // Dynamics: live only for the selected band (same rule as the shape
    // glyph beside it) AND only for types with gain to modulate -- a
    // dynamic High-Pass has nothing to move.
    dynControl.setEnabled (hasGain && b == curve.getSelectedBand());
    deleteControl.setEnabled (b >= 0 && b == curve.getSelectedBand());
    dynControl.setOn (a.getRawParameterValue (
        EQAudioProcessor::bandParamID (b, "dynon"))->load() > 0.5f);
}

//==============================================================================
void EQAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Blends toward a light-medium grey while bypassed (see timerCallback's
    // bypassDim fade) -- only the flat background colour changes; every
    // control still paints itself at full contrast on top of this. This is
    // the only thing the editor paints directly -- everything else lives on
    // the design-space canvas (see ContentComponent) or on the graph.
    g.fillAll (EQLookAndFeel::background().interpolatedWith (EQLookAndFeel::dimmedBackground(), bypassDim));
}

void EQAudioProcessorEditor::resized()
{
    // Remember the chosen size for this plugin instance (read back by the
    // ctor next time the editor opens), then scale the design-space canvas
    // to fill the window -- ONE transform, no per-control layout changes.
    // Width alone determines the scale; the aspect-ratio constrainer keeps
    // height locked to it.
    //
    // ONLY corner-drag resizes get remembered (see SizeMemoryConstrainer)
    // -- any size arriving outside a drag is the host's doing, and FL
    // pushes its own stale size at the editor on every window reopen; a
    // plausibility guard alone (>= min width) wasn't enough because that
    // push IS plausible, and it kept overwriting the user's choice before
    // the ctor's deferred re-apply could read it back.
    if (sizeConstrainer.userDragActive && getWidth() >= 540 && getHeight() >= 404)
    {
        processorRef.lastEditorWidth  = getWidth();
        processorRef.lastEditorHeight = getHeight();
    }

    // REFLOW (see kUiScale): the transform is pinned, the design-space SIZE
    // varies -- window / scale, ceiled so the scaled canvas always covers
    // the window (a floor could leave a sub-pixel sliver of bare window at
    // the right/bottom edges). Chrome metrics below are untouched design-
    // space pixels, exactly as they've always rendered; the graph takes the
    // rest.
    //
    // ADAPTIVE AT THE SMALL END ONLY: at and above ~806x624 the scale is a
    // constant kUiScale (1.3) and the chrome is rock-still through any
    // resize. Below that it eases down smoothly, bottoming out at 1.0 by
    // 620x480 -- at a small window the chrome shrinks ~23% AND the design
    // space grows, so the graph gains room from both directions at once
    // ("when it's super small everything seems a bit big"). Keyed on the
    // more-limiting axis so a short-and-wide window still relaxes.
    const float scale = juce::jlimit (1.0f, kUiScale,
                                      juce::jmin ((float) getWidth()  / 620.0f,
                                                  (float) getHeight() / 480.0f));

    const int cw = (int) std::ceil ((float) getWidth()  / scale);
    const int ch = (int) std::ceil ((float) getHeight() / scale);

    guideOverlay.setBounds (0, 0, cw, ch);

    content.setBounds (0, 0, cw, ch);
    content.setTransform (juce::AffineTransform::scale (scale));

    auto r = juce::Rectangle<int> (0, 0, cw, ch).reduced (8);

    // Top bar (new this pass): a fixed-height strip above the graph, pushed
    // in from the same outer margin as everything else. Undo/redo pinned to
    // the left edge, prev/preset-name/next pinned as one small centred group
    // -- same "edges pinned, middle content centred between them" convention
    // already used for row 1 of the bottom control section.
    constexpr int topBarH = 20;
    auto topBar = r.removeFromTop (topBarH);
    r.removeFromTop (6); // gap between the top bar and the graph below it

    auto topBarIcons = topBar; // undo/redo consume from a COPY, so the
                               // preset group below can still centre in the
                               // bar's FULL width, not just the leftover
                               // space after undo/redo -- same distinction
                               // that mattered for row 1's readout centring.
    topBarIcons.removeFromLeft (10); // nudges undo/redo in from the left edge
    constexpr int topIconW = 16, topIconGap = 3;
    undoButton.setBounds (topBarIcons.removeFromLeft (topIconW).withSizeKeepingCentre (14, 14));
    topBarIcons.removeFromLeft (topIconGap);
    redoButton.setBounds (topBarIcons.removeFromLeft (topIconW).withSizeKeepingCentre (14, 14));

    // Save pinned to the right edge, mirroring undo/redo's left-edge inset --
    // consumed from its own copy of topBar for the same reason undo/redo use
    // one: the preset group stays centred in the bar's FULL width regardless.
    auto topBarSave = topBar;
    topBarSave.removeFromRight (10);
    helpButton.setBounds (topBarSave.removeFromRight (topIconW).withSizeKeepingCentre (14, 14));
    topBarSave.removeFromRight (topIconGap);
    // Bypass lives up here with the other GLOBAL controls now (undo, redo,
    // presets, save) -- the graph is left holding only per-band and view
    // controls. Same 14x14 as its neighbours; its old white glow was
    // dropped to make that fit, since the whole-window background wash on
    // bypass is already an unmissable signal on its own.
    bypassButton.setBounds (topBarSave.removeFromRight (topIconW).withSizeKeepingCentre (14, 14));

    const auto presetFont = EQLookAndFeel::uiFont (14.0f);
    const int presetNameW = juce::roundToInt (
        juce::GlyphArrangement::getStringWidth (presetFont, presetNameLabel.getText())) + 12;
    constexpr int presetArrowW = 16, presetGap = 4;
    const int presetGroupW = presetArrowW + presetGap + presetNameW + presetGap + presetArrowW;
    auto presetGroup = topBar.withSizeKeepingCentre (presetGroupW, topBar.getHeight());
    prevPresetButton.setBounds (presetGroup.removeFromLeft (presetArrowW).withSizeKeepingCentre (14, 14));
    presetGroup.removeFromLeft (presetGap);
    nextPresetButton.setBounds (presetGroup.removeFromRight (presetArrowW).withSizeKeepingCentre (14, 14));
    presetGroup.removeFromRight (presetGap);
    presetNameLabel.setBounds (presetGroup);

    // NO bottom control section at all any more -- the migration is
    // complete. Row 1 (the selected-band panel) went first; row 2's In/Out
    // knobs became pills, its brand mark became a watermark, and its bypass
    // pill moved, all onto the graph below. `r` is now the graph, edge to
    // edge under the top bar.

    // --- readout pill: floats ON the graph -------------------------------
    // Worst-case width reserved from each field's own longest realistic
    // text, so the pill never changes size as values change (the labels
    // pack tightly and re-centre INSIDE it -- see layoutReadoutLabels()).
    // freq/gain include the same trailing "  " their real displayed text
    // will have (see updateReadout()), so this reservation stays big enough.
    const auto comboFont = EQLookAndFeel::uiFont (14.0f);
    const int freqW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (comboFont, "20.00 kHz  ")) + 2;
    const int gainW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (comboFont, "-18.0 dB  ")) + 2;
    const int qW    = juce::roundToInt (juce::GlyphArrangement::getStringWidth (comboFont, "Q 10.00")) + 2;
    const int readoutW = freqW + gainW + qW;

    // Top-centre of the graph: balances the dB-range bubble in the opposite
    // corner, and the top strip is the emptiest part of the graph in
    // practice (curves sit around the 0 dB centre line, the analyzer fills
    // upward from the bottom). `r` is still the graph's own rect here --
    // the pill is positioned in canvas space over it, then the graph takes
    // that same full rect below, so the two deliberately overlap.
    // TWO ROWS, same width as before: values on top, band controls beneath.
    // Height rather than width is where the room comes from -- the pill is
    // already as wide as the worst-case value text, and widening it further
    // would push the numbers off the graph's centre line.
    constexpr int pillH = 48, pillPadX = 14, pillTop = 8;
    constexpr int shapeW = 15, shapeH = 12;

    readoutPill.setBounds (juce::Rectangle<int> (readoutW + pillPadX * 2, pillH)
                               .withCentre ({ r.getCentreX(), r.getY() + pillTop + pillH / 2 }));

    auto inner = readoutPill.getLocalBounds().reduced (pillPadX, 0);

    // Labels pack inside the TOP half of the pill's own local bounds --
    // they and the shape glyph are both its children.
    readoutZone = inner.removeFromTop (pillH / 2);

    // Control row: shape glyph pinned LEFT, make-dynamic centred in the row
    // itself (not in what's left after the glyph) so it sits on the pill's
    // true centre line, under the values. The right side is deliberately
    // empty -- reserved space, not a gap to fill.
    constexpr int dynH = 17;
    // +20 off the row's left edge: hard against it, the glyph sat too close
    // to the pill's rounded end.
    shapeControl.setBounds (inner.getX() + 20, inner.getY() + (inner.getHeight() - shapeH) / 2,
                            shapeW, shapeH);

    // Delete mirrors the shape glyph EXACTLY: the same 15x12 slot, the
    // same +20 inset from its rounded end, the same vertical line -- a
    // smaller bespoke square was tried and read as sitting tighter to the
    // border than the shape does, because its ink centre landed nearer its
    // edge. Identical slots make the two true mirrors by construction.
    // -3: nudge-tuned by eye against the values row, same treatment as the
    // make-dynamic chip's -2.
    deleteControl.setBounds (inner.getRight() - 21 - shapeW,
                             inner.getY() + (inner.getHeight() - shapeH) / 2 - 3,
                             shapeW, shapeH);
    // Nudged 2px up from the row's centre: the button is taller than the
    // shape glyph beside it, so centring both on the same line left it
    // sitting visibly low against the values above.
    dynControl.setBounds (inner.withSizeKeepingCentre (dynControl.idealWidth() + DynControl::kMagnetPad * 2,
                                                       dynH + DynControl::kMagnetPad * 2)
                               .translated (0, -2));

    layoutReadoutLabels();

    // --- the graph fills EVERYTHING below the top bar ---------------------
    // The bottom control bar is gone; its last inhabitants (In/Out trim,
    // the brand mark, bypass) float on the graph as pills/watermark below,
    // positioned in canvas space over `r` before the curve claims it.
    curve.setBounds (r);

    // Bottom strip of the graph, sitting just above the Hz axis labels
    // (which the curve draws at its own getHeight()-10): the trim pill at
    // the left, the brand mark centred. (Bypass used to sit at the right of
    // this strip; it moved to the top bar with the other global controls.)
    constexpr int gainPillH = 22, bottomInset = 16, sideInset = 10;
    const int gainPillY = r.getBottom() - bottomInset - gainPillH;

    // Bounds padded by kMagnetPad on every side; the VISUAL pill still
    // sits exactly at (sideInset, gainPillY) -- see GainPill::visualBounds.
    gainPill.setBounds (r.getX() + sideInset - GainPill::kMagnetPad,
                        gainPillY - GainPill::kMagnetPad,
                        gainPill.idealWidth() + GainPill::kMagnetPad * 2,
                        gainPillH + GainPill::kMagnetPad * 2);

    // Brand mark, now a WATERMARK on the graph (it was centred in the old
    // bottom row). Centred on the graph's full width so it can't drift when
    // either side's controls change, same reasoning as the readout pill.
    logoLabel.setBounds (juce::Rectangle<int> (r.getWidth(), 26)
                             .withCentre ({ r.getCentreX(), gainPillY + gainPillH / 2 }));
}

//==============================================================================
EQAudioProcessorEditor::GainPill::GainPill (EQAudioProcessor& proc)
    : processorRef (proc),
      inParam  (*proc.apvts.getParameter ("inGain")),
      outParam (*proc.apvts.getParameter ("outGain")),
      // Both attachments live for the pill's whole life; each only writes to
      // the display when ITS parameter is the one being shown. That keeps
      // host automation and undo/redo of the hidden trim from stomping the
      // visible value, while still syncing the moment you switch modes.
      inAttachment  (inParam,  [this] (float v) { if (! monitoringOutput) showValue (v); }, &proc.undoManager),
      outAttachment (outParam, [this] (float v) { if (monitoringOutput)   showValue (v); }, &proc.undoManager)
{
    valueLabel.setJustificationType (juce::Justification::centredRight);
    valueLabel.setColour (juce::Label::textColourId, EQLookAndFeel::ink());
    valueLabel.setFont (EQLookAndFeel::uiFont (13.0f));
    valueLabel.setBorderSize (juce::BorderSize<int> (0));
    valueLabel.setMinimumHorizontalScale (1.0f);

    // Editing is driven by OUR double-click (see mouseDoubleClick), not the
    // Label's own click handling -- the label must not swallow the drags
    // that adjust the value. Its editor DOES still receive clicks (second
    // arg), so typing and selecting inside it work normally.
    valueLabel.setInterceptsMouseClicks (false, true);
    valueLabel.setEditable (false, false, false);
    valueLabel.onEditorShow = [this]
    {
        if (auto* ed = valueLabel.getCurrentTextEditor())
        {
            ed->setJustification (juce::Justification::centredRight);
            ed->setFont (EQLookAndFeel::uiFont (13.0f));
            ed->setColour (juce::TextEditor::backgroundColourId, EQLookAndFeel::background());
            ed->setColour (juce::TextEditor::textColourId, EQLookAndFeel::ink());
            ed->setColour (juce::TextEditor::highlightColourId, EQLookAndFeel::ink().withAlpha (0.25f));
            ed->selectAll();
        }
    };
    valueLabel.onTextChange = [this] { commitTypedText(); };

    addAndMakeVisible (valueLabel);
    outAttachment.sendInitialUpdate();
}

int EQAudioProcessorEditor::GainPill::idealWidth() const
{
    // Sized from the WIDEST caption plus the widest value the range can
    // produce, so the pill never resizes -- not when the value changes, and
    // not when you flip between In and Out.
    const auto font = EQLookAndFeel::uiFont (13.0f);
    const int capW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, "Out"));
    const int valW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, "-24.0 dB"));
    return capW + valW + 28; // caption gap + the pill's own left/right padding
}

juce::Rectangle<int> EQAudioProcessorEditor::GainPill::captionBounds() const
{
    // The clickable In/Out switch: the caption text plus a little breathing
    // room, measured on the WIDER of the two words so the hit zone doesn't
    // change size when the mode flips.
    const auto font = EQLookAndFeel::uiFont (13.0f);
    const int capW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, "Out"));
    return visualBounds().reduced (10, 0).removeFromLeft (capW + 4);
}

void EQAudioProcessorEditor::GainPill::resized()
{
    // paint() draws the caption itself; only the value needs bounds.
    valueLabel.setBounds (visualBounds().reduced (10, 0)
                              .withTrimmedLeft (captionBounds().getWidth() + 4));
}

void EQAudioProcessorEditor::GainPill::setMode (bool wantOutput)
{
    if (monitoringOutput == wantOutput)
        return;

    monitoringOutput = wantOutput;

    // Pull the newly-visible trim's current value, and drop the meter to
    // silence so the previous signal's level doesn't decay away misleadingly
    // under the new label.
    showValue (activeParam().convertFrom0to1 (activeParam().getValue()));
    repaint();
}

void EQAudioProcessorEditor::GainPill::paint (juce::Graphics& g)
{
    // Same pill treatment as the readout pill and the graph's dB-range
    // bubble -- one visual language for every floating control. (A bare-text
    // version was tried and reverted: over the analyzer's busy low end the
    // container is what keeps the value readable.)
    // The capsule swells toward the cursor (magnet, fed by the editor's
    // timer) and its border brightens on the same ramp -- the caption and
    // value stay put (they measure from visualBounds()), so only the
    // container breathes, same as the dots.
    // Same gentle 1.2px swell as the make-dynamic chip -- the two pill
    // controls should feel like one physics.
    auto r = getLocalBounds().toFloat().reduced ((float) kMagnetPad + 0.5f - 1.2f * magnet);
    const float radius = r.getHeight() * 0.5f;

    g.setColour (surfaceColour);
    g.fillRoundedRectangle (r, radius);
    g.setColour (EQLookAndFeel::ink().withAlpha (
        isMouseOverOrDragging() ? 0.65f : 0.40f + 0.25f * magnet));
    g.drawRoundedRectangle (r, radius, 1.0f);


    // Caption doubles as the In/Out switch -- brightens on hover to say so.
    g.setColour (EQLookAndFeel::ink().withAlpha (captionHot ? 0.95f : 0.55f));
    g.setFont (EQLookAndFeel::uiFont (13.0f));
    g.drawText (monitoringOutput ? "Out" : "In", captionBounds(), juce::Justification::centredLeft);
}

void EQAudioProcessorEditor::GainPill::mouseMove (const juce::MouseEvent& e)
{
    const bool nowHot = captionBounds().contains (e.getPosition());
    if (nowHot != captionHot)
    {
        captionHot = nowHot;
        setMouseCursor (nowHot ? juce::MouseCursor::PointingHandCursor
                               : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EQAudioProcessorEditor::GainPill::mouseExit (const juce::MouseEvent&)
{
    captionHot = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void EQAudioProcessorEditor::GainPill::mouseDown (const juce::MouseEvent& e)
{
    // A click on the caption flips modes instead of starting a drag.
    if (captionBounds().contains (e.getPosition()))
    {
        setMode (! monitoringOutput);
        return;
    }

    valueAtDragStart = activeParam().convertFrom0to1 (activeParam().getValue());
    dragging = false;
}

void EQAudioProcessorEditor::GainPill::mouseDrag (const juce::MouseEvent& e)
{
    if (captionBounds().contains (e.getMouseDownPosition()))
        return; // drag that began on the switch -- not a value edit

    if (! dragging)
    {
        // One gesture per drag, opened on first real movement (not on
        // mouseDown) so a plain click doesn't write an empty automation move.
        dragging = true;
        activeAttachment().beginGesture();
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    // Vertical drag, ~0.15 dB per pixel -- the full range takes a long,
    // deliberate pull, which suits a trim you set once rather than ride.
    // Up = louder, matching every fader ever made.
    const auto range = activeParam().getNormalisableRange();
    const float target = juce::jlimit (range.start, range.end,
                                       valueAtDragStart - (float) e.getDistanceFromDragStartY() * 0.15f);
    activeAttachment().setValueAsPartOfGesture (target);
}

void EQAudioProcessorEditor::GainPill::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
    {
        dragging = false;
        activeAttachment().endGesture();
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }
}

void EQAudioProcessorEditor::GainPill::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! captionBounds().contains (e.getPosition()))
        valueLabel.showEditor();
}

void EQAudioProcessorEditor::GainPill::showValue (float newValue)
{
    if (! valueLabel.isBeingEdited())
        valueLabel.setText (juce::String (newValue, 1) + " dB", juce::dontSendNotification);
}

void EQAudioProcessorEditor::GainPill::commitTypedText()
{
    // getFloatValue() reads the leading number and ignores trailing text,
    // so "-3.5 dB" or a bare "-3.5" both work.
    const auto range = activeParam().getNormalisableRange();
    const float typed = juce::jlimit (range.start, range.end, valueLabel.getText().getFloatValue());

    activeAttachment().setValueAsCompleteGesture (typed);
    showValue (typed); // normalise the text back (e.g. "-3" -> "-3.0 dB")
}

//==============================================================================
namespace
{
    // The guide's content, as (heading, body) pairs. Deliberately covers
    // only what poking at the graph WON'T teach you -- the hover strips,
    // the hold-to-listen gesture, click-to-type -- rather than restating
    // what an EQ is. Full specs live on the site (see siteUrl).
    struct GuideSection { const char* heading; const char* body; };

    const GuideSection kGuideSections[]
    {
        { "Bands",
          "Click anywhere on the graph to add one. Drag to move, scroll to "
          "change Q, right-click to remove." },
        { "Band controls",
          "Hover a band's dot and mute + listen appear above it. The "
          "band's shape and delete sit in the readout pill up top -- click "
          "the shape to change filter type; pass filters carry their slope "
          "in a submenu. Right-clicking a dot also deletes it." },
        { "Dynamics",
          "make dynamic (in the pill) lets a band's gain follow the music. "
          "Drag the arrows to set how far it moves -- down to compress, up "
          "to expand. Threshold is automatic." },
        { "Listen",
          "Hold the headphones to solo that band's frequency, and drag while "
          "holding to sweep it. Release to snap back." },
        { "Automation",
          "every control is a host parameter -- tweak one, then tools, "
          "last tweaked, create automation clip." },
        { "Values",
          "Double-click any number to type it. The dB range sits top-left; "
          "the In/Out trim bottom-left -- click its label to switch which "
          "one you're setting." }
    };

    // Guide panel metrics, shared by panelBounds()/paint()/resized() so the
    // three can't disagree about where anything sits.
    constexpr int kGuideW        = 430;
    constexpr int kGuidePadX     = 26, kGuidePadY = 22;
    constexpr int kGuideTitleH   = 22, kGuideTitleGap = 14;
    constexpr int kGuideHeadingH = 17, kGuideSectionGap = 14;
    constexpr int kGuideFooterH  = 46;   // "Full specs..." caption + the link

    // TRUE wrapped height of a body paragraph at a given width. Estimating
    // this from raw string width divided by column width was the first
    // approach and it was wrong in both directions -- word wrapping never
    // packs as tightly as the division implies, and the fudge factor added
    // to compensate left big holes between sections.
    int guideBodyHeight (const juce::String& text, int width)
    {
        juce::AttributedString as;
        as.append (text, EQLookAndFeel::uiFont (11.0f));

        juce::TextLayout layout;
        layout.createLayout (as, (float) width);
        return (int) std::ceil (layout.getHeight());
    }

    // The panel sizes itself to its content, so editing the sections above
    // can never make the text overflow or leave dead space.
    int guidePanelHeight()
    {
        const int textW = kGuideW - kGuidePadX * 2;
        int h = kGuidePadY * 2 + kGuideTitleH + kGuideTitleGap + kGuideFooterH;

        for (const auto& section : kGuideSections)
            h += kGuideHeadingH + guideBodyHeight (section.body, textW) + kGuideSectionGap;

        return h - kGuideSectionGap; // no trailing gap after the last section
    }
}

EQAudioProcessorEditor::GuideOverlay::GuideOverlay()
{
    siteLink.setButtonText ("audioflower.art");
    siteLink.setURL (juce::URL (siteUrl));
    siteLink.setFont (EQLookAndFeel::uiFont (13.0f), false, juce::Justification::centred);

    // The one place the copper accent is spent outside its three graph
    // roles -- a link is genuinely interactive, and it appears only inside
    // this overlay, so the accent's meaning on the graph stays intact.
    siteLink.setColour (juce::HyperlinkButton::textColourId, EQLookAndFeel::accent());
    addAndMakeVisible (siteLink);
}

juce::Rectangle<int> EQAudioProcessorEditor::GuideOverlay::panelBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (kGuideW, guidePanelHeight());
}

void EQAudioProcessorEditor::GuideOverlay::resized()
{
    auto footer = panelBounds().reduced (kGuidePadX, kGuidePadY).removeFromBottom (kGuideFooterH);
    footer.removeFromTop (20); // the caption line paint() draws above it
    siteLink.setBounds (footer);
}

void EQAudioProcessorEditor::GuideOverlay::mouseDown (const juce::MouseEvent& e)
{
    // Click anywhere outside the panel to dismiss. The overlay covers the
    // whole editor, so it also swallows clicks that would otherwise land on
    // the graph behind it -- which is the point: nothing behind should be
    // reachable while this is up.
    if (! panelBounds().contains (e.getPosition()))
        setVisible (false);
}

void EQAudioProcessorEditor::GuideOverlay::paint (juce::Graphics& g)
{
    // Scrim: darkens whatever's behind so the panel reads as the only live
    // thing on screen.
    g.fillAll (juce::Colours::black.withAlpha (0.55f));

    // Panel, in the same language as every other floating surface here --
    // background fill, hairline ink border, rounded.
    const auto panel = panelBounds().toFloat();
    g.setColour (EQLookAndFeel::background());
    g.fillRoundedRectangle (panel, 10.0f);
    g.setColour (EQLookAndFeel::ink().withAlpha (0.40f));
    g.drawRoundedRectangle (panel, 10.0f, 1.0f);

    auto body = panelBounds().reduced (kGuidePadX, kGuidePadY);

    // Reserve the footer FIRST so flowing content can never run into it --
    // it used to be drawn at a fixed offset from the panel's bottom while
    // the sections flowed from the top, and the two overlapped.
    auto footer = body.removeFromBottom (kGuideFooterH);

    g.setColour (EQLookAndFeel::ink());
    g.setFont (EQLookAndFeel::uiFont (15.0f));
    g.drawText ("Quick guide", body.removeFromTop (kGuideTitleH), juce::Justification::centredLeft);
    body.removeFromTop (kGuideTitleGap);

    for (const auto& section : kGuideSections)
    {
        g.setColour (EQLookAndFeel::ink().withAlpha (0.95f));
        g.setFont (EQLookAndFeel::uiFont (12.0f));
        g.drawText (section.heading, body.removeFromTop (kGuideHeadingH), juce::Justification::centredLeft);

        g.setColour (EQLookAndFeel::ink().withAlpha (0.55f));
        g.setFont (EQLookAndFeel::uiFont (11.0f));
        g.drawFittedText (section.body,
                          body.removeFromTop (guideBodyHeight (section.body, body.getWidth())),
                          juce::Justification::topLeft, 6);

        body.removeFromTop (kGuideSectionGap);
    }

    g.setColour (EQLookAndFeel::ink().withAlpha (0.45f));
    g.setFont (EQLookAndFeel::uiFont (11.0f));
    g.drawText ("Full specs and the detailed guide:",
                footer.removeFromTop (18), juce::Justification::centred);
}

EQAudioProcessorEditor::SmallIconButton::SmallIconButton (const juce::String& tablerSvgPaths)
    : juce::Button ("")
{
    icon = makeTablerIcon (tablerSvgPaths);
}

void EQAudioProcessorEditor::SmallIconButton::paintButton (juce::Graphics& g, bool, bool)
{
    float alpha = isEnabled() ? 1.0f : 0.35f;

    // Dim while toggled ON -- the same "engaged reads dimmer" convention a
    // band's power glyph uses for mute. A no-op for the buttons that never
    // toggle (undo/redo/save/help/preset arrows), which is all of them
    // except bypass.
    if (getToggleState()) alpha *= 0.5f;
    if (icon != nullptr)
        icon->drawWithin (g, getLocalBounds().toFloat().reduced (1.5f),
                          juce::RectanglePlacement::centred, alpha);
}
