#pragma once

#include <array>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EQCurveDisplay.h"
#include "EQLookAndFeel.h"

//==============================================================================
class EQAudioProcessorEditor : public juce::AudioProcessorEditor,
                               private juce::Timer,
                               private juce::ValueTree::Listener
{
public:
    explicit EQAudioProcessorEditor (EQAudioProcessor&);
    ~EQAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;   // refresh the text readout
    void selectionChanged();         // re-point the per-band controls at the new band
    void updateReadout();

    // juce::ValueTree::Listener -- fires for ANY apvts parameter change
    // (band edits, in/out gain, bypass, ...), regardless of which control
    // caused it (mouse drag, typed edit, host automation, undo/redo). Used
    // purely to flag the currently-loaded preset as modified; see
    // presetModified and updatePresetNameLabel().
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;

    // Hover hints. These are Component's own mouseEnter/mouseExit virtuals
    // (Component already IS a MouseListener -- naming it again as a base
    // class is an ambiguous-base error, not a second listener), and this
    // editor registers itself on each hintable control via registerHint, so
    // hovering one pushes its explanation into the readout pill and leaving
    // clears it. Events for the editor's own bounds land here too; the
    // eventComponent simply isn't in the map, so nothing is shown.
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void registerHint (juce::Component& c, juce::String text);
    std::map<juce::Component*, juce::String> hints;

    // Which band the readout pill is currently SHOWING -- the hovered band
    // if there is one, otherwise the selected band. Typed edits write here
    // rather than to the selection, so what you edit is always what you can
    // see. Frozen while the cursor is over the pill (or mid-edit) so moving
    // from a band's dot up to the pill doesn't flip it back underneath you.
    int displayedBand = -1;

    // Sets presetNameLabel's text to the current preset's name (or "a fresh
    // start" if none is loaded), appending " *" if presetModified -- the
    // single place that ever writes presetNameLabel's text, so the asterisk
    // logic can't drift out of sync with whichever call site changed it.
    void updatePresetNameLabel();

    using APVTS = juce::AudioProcessorValueTreeState;

    EQAudioProcessor& processorRef;

    // Scoped to this editor instance only (set/cleared in ctor/dtor), so this
    // monochrome restyle can't leak into any other plugin sharing the process.
    EQLookAndFeel lookAndFeel;

    // The design-space canvas EVERY control lives on (each one is added via
    // content.addAndMakeVisible, not the editor's). Its SIZE follows the
    // window (window / scale -- see the REFLOW notes below); the transform
    // it carries is the fixed-ish UI scale, so chrome renders the same at
    // any window size and the graph absorbs the rest. It paints nothing
    // itself (the editor's own paint() fills the window background); it's
    // purely the transform carrier. Declared before every child component
    // so it destructs LAST -- children detach from a still-alive parent.
    struct ContentComponent : juce::Component
    {
        explicit ContentComponent (EQAudioProcessorEditor& e) : owner (e) {}
            EQAudioProcessorEditor& owner;
    };
    ContentComponent content { *this };

    // REFLOW: the design-space size is now VARIABLE -- window size divided
    // by the fixed kUiScale. The chrome (top bar, pills, fonts) renders
    // pixel-identical at any window size because the transform never
    // changes; every extra design-space unit goes to the graph, which was
    // already fully fluid (freqToX/gainToY map into its own bounds).
    // designWidth/Height are the DEFAULT design size only -- the fallback
    // window size (and legacy-session height derivation) comes from them.
    //
    // kUiScale is 1.3 because that IS the look everything was tuned at:
    // the old fixed-canvas design defaulted to 936x701, i.e. 720x539 shown
    // at 1.3x. Pinning the transform there keeps every metric visually
    // unchanged. A future "UI scale" setting would simply make this a
    // variable -- the machinery is already shaped for it.
    static constexpr int   designWidth = 720, designHeight = 539;
    static constexpr float kUiScale    = 1.3f;

    // Custom constrainer whose only addition is knowing WHEN a resize is a
    // real user corner-drag: ResizableCornerComponent calls resizeStart()/
    // resizeEnd() exclusively around drags, and hosts' programmatic size
    // pushes never do. resized() only RECORDS the window size while a drag
    // is active -- without this, FL Studio's stale size push on window
    // reopen (a perfectly valid-looking width) overwrote the remembered
    // value before the deferred re-apply could read it, so the size reset
    // on every reopen no matter what.
    struct SizeMemoryConstrainer : juce::ComponentBoundsConstrainer
    {
        void resizeStart() override { userDragActive = true; }
        void resizeEnd()   override { userDragActive = false; }
        bool userDragActive = false;
    };
    SizeMemoryConstrainer sizeConstrainer;

    EQCurveDisplay curve;

    //==========================================================================
    // Freq/gain/Q readout, split into THREE separate editable labels (was
    // one combined "697 Hz  1.9 dB  Q 1.00" label) so each value can be
    // double-clicked and typed in directly. Not bound via attachments (the
    // typed text needs parsing/clamping first) -- writes go through
    // setBandParamFromEditor() with proper begin/end change gestures, same
    // host-automation-safe path as every other control here. Freq accepts an
    // optional "k" suffix (e.g. "2.5k") as shorthand for kHz.
    // The selected band's freq/gain/Q readout, as a pill floating ON the
    // graph -- same visual language as the graph's dB-range bubble (fill +
    // hairline border + rounded ends), which is what lets it stay legible
    // over the analyzer where bare text wouldn't. Replaces the whole row-1
    // panel that used to sit under the graph; the freed height went to the
    // graph itself.
    //
    // A real Component holding real Labels, NOT painted text, specifically
    // to keep the values double-click-to-type editable (see
    // setupEditableLabel) -- that's the one thing the graph's own painted
    // overlays can't do. The labels are its children, so
    // layoutReadoutLabels() packs them inside its LOCAL bounds and the
    // whole group moves as one.
    struct ReadoutPill final : juce::Component
    {
        // The graph behind this is a vertical gradient, so filling with the
        // flat background token made the pill read as a darker patch laid
        // on top of it. The editor pushes the correct surface shade for
        // this pill's own height each tick (see EQLookAndFeel::surfaceAt).
        void setSurfaceColour (juce::Colour c)
        {
            if (c != surfaceColour) { surfaceColour = c; repaint(); }
        }

        // Doubles as a HINT line: while set, the pill shows this text
        // instead of the band values, so hovering a control explains it
        // where you're already looking. The pill never resizes to fit one
        // (a control that changes width as the cursor moves reads as
        // twitchy) -- paint() shrinks the text to fit instead.
        // Deliberately NOT used for band dots: hovering a band shows that
        // band's own values instead, which is more useful there.
        void setHint (const juce::String& newHint)
        {
            if (newHint == hint)
                return;

            // Never blank the values out from under an in-progress typed
            // edit -- hiding a Label mid-edit destroys its TextEditor and
            // silently loses whatever was half-typed into it. Dropping the
            // hint fails safe: you keep seeing the value you're editing.
            if (newHint.isNotEmpty())
                for (auto* child : getChildren())
                    if (auto* l = dynamic_cast<juce::Label*> (child))
                        if (l->isBeingEdited())
                            return;

            hint = newHint;

            // The value labels step aside while a hint is showing.
            for (auto* child : getChildren())
                child->setVisible (hint.isEmpty());

            repaint();
        }

        bool hasHint() const noexcept { return hint.isNotEmpty(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);

            // Full h/2 stadium ends, SETTLED by eye at the 48px height: a
            // 12px cap (guide-panel family) was built and compared, and the
            // capsule looked better -- don't re-suggest capping it.
            const float radius = r.getHeight() * 0.5f;

            // FROSTED GLASS: what the graph is drawing underneath shows
            // through, blurred. There's no backdrop-blur in JUCE, so the
            // frost is built by hand: snapshot the patch of the graph
            // behind this pill at QUARTER resolution, then stretch it back
            // up with bilinear resampling -- the interpolation IS the blur
            // (a real gaussian would cost more and look the same at this
            // radius). The snapshot renders in design space, same
            // coordinate system this paint runs in, so the window's resize
            // transform scales both together and nothing shears at the rim.
            //
            // The editor re-repaints this pill every tick while visible
            // (see timerCallback) -- the frost is a live view, not a still:
            // the analyzer breathes behind the numbers.
            bool frosted = false;
            if (curveBelow != nullptr)
            {
                const auto areaInSource = curveBelow->getLocalArea (getParentComponent(), getBounds());
                const auto snap = curveBelow->createComponentSnapshot (areaInSource, false, 0.25f);

                if (snap.isValid())
                {
                    juce::Graphics::ScopedSaveState save (g);
                    juce::Path rp;
                    rp.addRoundedRectangle (r, radius);
                    g.reduceClipRegion (rp);
                    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                    g.drawImage (snap, getLocalBounds().toFloat());

                    // Tint over the frost: same gradient-matched surface
                    // shade as the old opaque fill, at glass opacity --
                    // enough body that 14px text stays comfortably legible
                    // over a blurred bright curve.
                    g.setColour (surfaceColour.withAlpha (0.55f));
                    g.fillRoundedRectangle (r, radius);

                    // A faint catch-light along the inner top edge -- the
                    // one flourish that makes it read as GLASS rather than
                    // as a translucent rectangle.
                    g.setColour (EQLookAndFeel::ink().withAlpha (0.10f));
                    g.drawLine (r.getX() + radius * 0.6f, r.getY() + 1.5f,
                                r.getRight() - radius * 0.6f, r.getY() + 1.5f, 1.0f);

                    frosted = true;
                }
            }

            if (! frosted)
            {
                // No frost source wired (or a snapshot failed): the original
                // opaque fill, so the pill NEVER paints as a hole.
                g.setColour (surfaceColour);
                g.fillRoundedRectangle (r, radius);
            }

            g.setColour (EQLookAndFeel::ink().withAlpha (0.40f));
            g.drawRoundedRectangle (r, radius, 1.0f);

            if (hint.isNotEmpty())
            {
                // 14px full-strength ink: EXACTLY what the three value
                // labels render at (they set uiFont(14) and draw in plain
                // ink -- see setupReadoutLabel), so a hint reads as the same
                // text swapped out, not as a different element borrowing the
                // pill. A smaller, dimmer treatment was tried first and the
                // double step-down was the whole reason it looked foreign.
                //
                // Every hint is written to fit at this size (28 characters,
                // the pill's own reservation -- see registerHint), so the
                // loop below is a safety net that shouldn't fire. If one
                // ever does overflow, shrinking beats the silent ellipsis
                // drawText would otherwise apply.
                const auto area = getLocalBounds().reduced (10, 0);
                float h = 14.0f;
                auto font = EQLookAndFeel::uiFont (h);

                while (h > 8.5f
                       && juce::GlyphArrangement::getStringWidth (font, hint) > (float) area.getWidth())
                {
                    h -= 0.5f;
                    font = EQLookAndFeel::uiFont (h);
                }

                g.setColour (EQLookAndFeel::ink());
                g.setFont (font);
                g.drawText (hint, area, juce::Justification::centred);
            }
        }

        juce::Colour surfaceColour { EQLookAndFeel::background() };

        // The curve display beneath this pill, wired once by the editor --
        // the pill's ONE piece of knowledge about the outside world, used
        // two ways: its content shows through the frost (see paint), and
        // clicks that land on a grabbable target under the glass are
        // forwarded back down to it (below). Null = opaque, no forwarding.
        EQCurveDisplay* curveBelow = nullptr;

        // CLICK-THROUGH: a band dragged under this pill would otherwise be
        // unreachable -- the pill sits on top, so once the drag releases
        // there, every later click lands here and the curve never sees it.
        // On mouseDown, if the point sits on a dot or dynamic arrow, the
        // whole gesture (down/drag/up, coordinate-translated) is forwarded
        // to the curve instead -- grabbing straight through the glass.
        // Clicks on plain background still die here on purpose: passing
        // them through would let a click on the pill's padding CREATE a
        // band. Children (labels, shape, dyn toggle) take their clicks
        // before the pill ever sees them, so none of this affects them.
        void mouseDown (const juce::MouseEvent& e) override
        {
            forwardingDrag = curveBelow != nullptr
                          && curveBelow->hasGrabbableAt (e.getEventRelativeTo (curveBelow).position);
            if (forwardingDrag)
                curveBelow->mouseDown (e.getEventRelativeTo (curveBelow));
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (forwardingDrag)
                curveBelow->mouseDrag (e.getEventRelativeTo (curveBelow));
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (forwardingDrag)
            {
                curveBelow->mouseUp (e.getEventRelativeTo (curveBelow));
                forwardingDrag = false;
            }
        }

    private:
        juce::String hint;
        bool forwardingDrag = false;
    };

    // The selected band's filter shape, living IN the readout pill rather
    // than on the graph. It's the only band control that opens a menu, and a
    // menu wants a predictable anchor -- at the dot it appeared wherever
    // that band happened to sit. Filter type is also a settled structural
    // property, which is the category that acts on the SELECTED band here;
    // the momentary ones (mute, listen, delete) stayed at the dot and still
    // act on whatever is HOVERED. Two targeting rules, on purpose.
    //
    // Deliberately NOT registered with registerHint: a hint blanks the
    // pill's children, so hovering a control inside the pill would hide the
    // control, fire mouseExit, clear the hint, show it again -- a flicker
    // loop. The glyph is a literal picture of the response instead.
    struct ShapeControl final : juce::Component
    {
        void setType (EQBand::FilterType t)
        {
            if (t != type) { type = t; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            // Dimmed when the pill is reporting a band that ISN'T the
            // selected one (see updateReadout) -- the values you're reading
            // and the band this button would act on have diverged, so it
            // says so rather than quietly targeting the other one.
            const float alpha = ! isEnabled()          ? 0.25f
                              : isMouseOver()          ? 1.00f
                                                       : 0.75f;

            g.setColour (EQLookAndFeel::ink().withAlpha (alpha));
            EQCurveDisplay::drawTypeGlyph (g, type, getLocalBounds().toFloat().reduced (1.0f));
        }

        // Component doesn't repaint on hover by itself.
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { repaint(); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (isEnabled() && onClick != nullptr && getLocalBounds().contains (e.getPosition()))
                onClick();
        }

        std::function<void()> onClick;

    private:
        EQBand::FilterType type = EQBand::FilterType::Bell;
    };

    ReadoutPill readoutPill;

    // The make-dynamic toggle: a WORDED button, not a glyph. Two curve
    // icons side by side (the shape bell and a twin-bell dynamics mark)
    // were tried first and read as the same mark at 15px -- and neither
    // said what it did. Words cost the row nothing here, since the row was
    // mostly empty anyway.
    //
    // State follows the action strip's own convention (the power cell dims
    // when muted): BRIGHT while dynamics are on, dimmed while off, faint
    // when the band's type has no gain to modulate. Not hint-registered,
    // same flicker-loop reason as ShapeControl.
    struct DynControl final : juce::Component
    {
        // Room the capsule swells into (see setMagnet) -- the component's
        // bounds are padded by this much on every side, and the RESTING
        // visual insets back by the same amount, so layout anchors and the
        // idle look are exactly what they were before the magnet existed.
        static constexpr int kMagnetPad = 4;

        void setOn (bool shouldBeOn)
        {
            if (shouldBeOn != on) { on = shouldBeOn; repaint(); }
        }

        // Magnetic proximity swell, same language as the band dots: the
        // editor pushes cursor-distance-derived 0..1 every tick (a child
        // component can't see the cursor until it's already inside, so the
        // approach has to be fed from outside).
        void setMagnet (float m)
        {
            if (std::abs (m - magnet) > 0.005f) { magnet = m; repaint(); }
        }

        // Width the label needs, so the pill's control row can centre it
        // without hard-coding a guess that drifts if the wording changes.
        int idealWidth() const
        {
            return juce::roundToInt (juce::GlyphArrangement::getStringWidth (
                       EQLookAndFeel::uiFont (12.0f), label())) + 20;
        }

        void paint (juce::Graphics& g) override
        {
            // Brightness rides the magnet continuously (rest 0.55 up to the
            // old hover value 0.85), so the button starts waking as the
            // cursor approaches instead of flipping state at the border.
            const float pull = isEnabled() ? magnet : 0.0f;
            const float base = ! isEnabled() ? 0.25f
                             : on            ? 1.00f
                                             : 0.55f + 0.30f * pull;

            // Gentler swell than the dots' 2.5px: this chip lives INSIDE
            // the readout pill, and at full pull the larger growth crowded
            // the pill's own bottom hairline.
            auto r = getLocalBounds().toFloat().reduced ((float) kMagnetPad + 0.5f - 1.2f * pull);
            const float radius = r.getHeight() * 0.5f;

            // Engaged reads as a FILLED chip (ink ground, background text)
            // -- the same swap the popup menus use for a highlighted row,
            // so "this is on" looks the same everywhere in the plugin.
            if (on && isEnabled())
            {
                g.setColour (EQLookAndFeel::ink().withAlpha (isMouseOver() ? 1.0f : 0.88f));
                g.fillRoundedRectangle (r, radius);
                g.setColour (EQLookAndFeel::background());
            }
            else
            {
                g.setColour (EQLookAndFeel::ink().withAlpha (base * 0.45f));
                g.drawRoundedRectangle (r, radius, 1.0f);
                g.setColour (EQLookAndFeel::ink().withAlpha (base));
            }

            g.setFont (EQLookAndFeel::uiFont (12.0f));
            g.drawText (label(), getLocalBounds(), juce::Justification::centred);
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { repaint(); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (isEnabled() && onClick != nullptr && getLocalBounds().contains (e.getPosition()))
                onClick();
        }

        std::function<void()> onClick;

    private:
        // Lower case, matching every other worded control in this UI (the
        // "presets" label, the hint line).
        juce::String label() const { return on ? "dynamic" : "make dynamic"; }

        bool on = false;
        float magnet = 0.0f;
    };

    // Deletes the SELECTED band -- the X that used to be the hover strip's
    // last cell, moved to the pill's right side to mirror the shape glyph
    // on its left (the strip keeps only the momentary acts: mute, listen).
    // Same enable rule as its neighbours: live only when the pill's
    // displayed band IS the selection, so it can never delete a band whose
    // numbers you aren't looking at. Right-clicking a dot still deletes
    // too -- one removeBand path, two triggers.
    // Not hint-registered, same flicker-loop reason as ShapeControl.
    struct DeleteControl final : juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            const float alpha = ! isEnabled() ? 0.25f
                              : isMouseOver() ? 1.00f
                                              : 0.75f;

            // The X ink itself, centred as a square inside the slot -- the
            // slot matches the shape glyph's (see resized()), but the mark
            // shouldn't stretch to the slot's 15x12.
            const auto r = getLocalBounds().toFloat()
                               .withSizeKeepingCentre (9.0f, 9.0f);
            g.setColour (EQLookAndFeel::ink().withAlpha (alpha));

            juce::Path x;
            x.startNewSubPath (r.getTopLeft());
            x.lineTo          (r.getBottomRight());
            x.startNewSubPath (r.getTopRight());
            x.lineTo          (r.getBottomLeft());
            g.strokePath (x, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { repaint(); }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (isEnabled() && onClick != nullptr && getLocalBounds().contains (e.getPosition()))
                onClick();
        }

        std::function<void()> onClick;
    };

    // Declared AFTER their parent so they detach from a live pill on
    // teardown.
    ShapeControl  shapeControl;
    DynControl    dynControl;
    DeleteControl deleteControl;

    juce::Label freqLabel, gainLabel, qLabel;
    void setupEditableLabel (juce::Label&, std::function<void()> onCommit);
    void setBandParamFromEditor (juce::StringRef name, float value);

    // Fixed reserved area for the three value labels, computed ONCE in
    // resized() -- its width is sized generously (worst-case text) so the
    // dropdown/icons around it never move, but the labels THEMSELVES are
    // packed tightly against their own actual current text width inside it
    // (see layoutReadoutLabels()), not split into equal/static sub-boxes.
    // That's what keeps the gap between "Hz"/"dB"/"Q" consistently small
    // regardless of how long any one value's text happens to be right now.
    juce::Rectangle<int> readoutZone;
    void layoutReadoutLabels();

    // Original pill/slider toggle design (rounded track + sliding thumb),
    // reused here for bypass exactly as it looked before — just relocated.
    // Component bounds are DELIBERATELY bigger than the visual pill (see
    // glowMargin) so the bypassed-state glow has room to spread outward
    // instead of being clipped at the component's own edge (JUCE clips a
    // component's painting to its own rectangle by default) -- hitTest() is
    // overridden so the enlarged bounds don't also enlarge the clickable area.
    // (The per-band controls that used to fill this panel's right side --
    // mute/power, listen, delete -- and the 12/24/36/48 slope selector all
    // live ON the graph now, as hover-revealed strips floating above the
    // band's own dot. Owned entirely by EQCurveDisplay; see its hover
    // strips. Row 1 is just the type dropdown + readout.)

    //==========================================================================
    // Top bar (new this pass) — reserved space above the graph for a preset
    // browser (name + prev/next arrows) and undo/redo, laid out now with
    // real (but currently no-op) components rather than throwaway placeholder
    // drawing, since the actual preset/undo-redo logic is coming as a
    // follow-up step and will just wire onClick handlers onto these same
    // buttons instead of replacing them. One shared icon-button class, since
    // these have no per-instance state or bespoke painting beyond "draw
    // this SVG".
    class SmallIconButton : public juce::Button
    {
    public:
        explicit SmallIconButton (const juce::String& tablerSvgPaths);
        void paintButton (juce::Graphics&, bool, bool) override;
    private:
        std::unique_ptr<juce::Drawable> icon;
    };

    // Plain Label with a click callback -- juce::Label has no built-in
    // onClick (that's only Button), so clicking the preset name to open the
    // full preset list menu needs this tiny override instead.
    class PresetNameLabel : public juce::Label
    {
    public:
        std::function<void()> onClicked;       // left click -> full preset menu
        std::function<void()> onRightClicked;  // right click -> save shortcut

        // Right-click is caught in mouseDOWN, not mouseUp: by the time the
        // button is released the modifier flags may no longer report it.
        // The flag then suppresses the normal click on the matching mouseUp
        // so one gesture can't fire both menus.
        void mouseDown (const juce::MouseEvent& e) override
        {
            handledAsRightClick = e.mods.isPopupMenu();
            if (handledAsRightClick)
            {
                if (onRightClicked != nullptr)
                    onRightClicked();
                return;
            }
            juce::Label::mouseDown (e);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (handledAsRightClick)
            {
                handledAsRightClick = false;
                return;
            }
            juce::Label::mouseUp (e);
            if (onClicked != nullptr)
                onClicked();
        }

    private:
        bool handledAsRightClick = false;
    };

    PresetNameLabel   presetNameLabel;
    SmallIconButton   prevPresetButton, nextPresetButton;
    SmallIconButton   undoButton, redoButton;
    // (There's no save BUTTON any more -- saving lives in the preset menu
    // itself, reached by clicking the preset name, plus a right-click
    // shortcut on that same name. Preset browsing and preset saving were
    // split across the bar before, and the right-hand cluster had become a
    // grab-bag of unrelated icons.)
    SmallIconButton   helpButton;  // rightmost in the top bar -- opens guideOverlay

    // In-plugin quick reference, shown over the whole editor when the "?"
    // in the top bar is clicked. Deliberately SHORT -- it covers only the
    // interactions that aren't discoverable by poking at the graph (hover
    // strips, hold-to-listen, click-to-type), and links out to the website
    // for full specs rather than trying to be a manual.
    class GuideOverlay final : public juce::Component
    {
    public:
        GuideOverlay();
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;

        // Where the site link points. The deep page doesn't exist yet --
        // update this one string once it does.
        static constexpr const char* siteUrl = "https://audioflower.art";

    private:
        juce::Rectangle<int> panelBounds() const;
        juce::HyperlinkButton siteLink;
    };

    GuideOverlay guideOverlay;

    // One row in the preset popup menu. Not auto-triggered (see the base
    // class ctor arg) so left vs. right click can be told apart manually:
    // left click selects/loads the preset as normal (triggerMenuItem(), same
    // as a plain addItem() row would); right click dismisses the menu and
    // asks to delete it instead, via onDeleteRequested.
    class PresetMenuItem : public juce::PopupMenu::CustomComponent
    {
    public:
        PresetMenuItem (juce::String name, bool ticked, std::function<void()> onContextMenu);
        void paint (juce::Graphics&) override;
        void getIdealSize (int& idealWidth, int& idealHeight) override;
        void mouseUp (const juce::MouseEvent&) override;
    private:
        juce::String presetName;
        bool isTicked;

        // Right-clicking a row used to delete it outright. It now opens a
        // Rename/Delete menu -- one more click for a delete, but rename had
        // nowhere else to live that wasn't a second undiscoverable shortcut.
        // Set for EVERY row including factory ones, which show the menu with
        // its items disabled (see showPresetRowMenu) rather than silently
        // swallowing the click.
        std::function<void()> onContextMenu;
    };

    // Preset browsing state -- presetNames is the sorted file list (refreshed
    // at construction), currentPresetIndex tracks which one prev/next last
    // loaded (wraps at either end).
    juce::StringArray presetNames;
    int               currentPresetIndex = -1;

    // True once ANY parameter has changed since the current preset was
    // loaded/saved (see valueTreePropertyChanged()) -- shows as a trailing
    // " *" on the name label (see updatePresetNameLabel()) so it's obvious
    // the live state no longer matches what's on disk.
    bool presetModified = false;
    void refreshPresetList();
    void loadPresetAtIndex (int index);
    void showPresetMenu();
    void showPresetContextMenu(); // right-click on the preset name
    void confirmAndDeletePreset (const juce::String& name);
    void showSavePresetDialog();
    void showPresetRowMenu (const juce::String& name);      // right-click a menu row

    // Width of one PLAIN TEXT menu row. Must stay in step with
    // EQLookAndFeel::getIdealPopupMenuItemSize -- a menu centred from a
    // width that disagrees with its real one lands off by half the error.
    static int plainMenuRowWidth (const juce::String& text);

    // Opens `menu` centred under the preset name. THE one way any of the
    // three preset menus is shown, so they can't drift apart in either
    // alignment or size (see the .cpp for why both depend on this).
    void showMenuUnderPresetName (juce::PopupMenu& menu, int menuWidth,
                                  std::function<void (int)> onResult);
    void showRenamePresetDialog (const juce::String& oldName);

    // THE one gate every new preset name passes through, for both "save as"
    // and "rename" (oldName empty means save). Rejects factory names, asks
    // before replacing an existing preset, then commits and re-points the
    // label. Keeping it single means the two paths can't drift apart on
    // which names they let through.
    void applyPresetName (const juce::String& newName, const juce::String& oldName);
    void confirmReplacePreset (const juce::String& name, std::function<void()> onConfirmed);

    // One-button informational dialog; onDismissed re-opens whatever asked,
    // so a rejected name is a correction rather than a dead end.
    void showMessage (const juce::String& title, const juce::String& message,
                      std::function<void()> onDismissed);

    // A modal AlertWindow is NOT a child of this editor, so it inherits
    // neither the LookAndFeel nor any colour -- every one of them needs the
    // same dozen setColour calls, which lived duplicated in each dialog
    // until there were four of them.
    void styleAlert (juce::AlertWindow& aw);

    //==========================================================================
    // Global controls (fixed parameters, so their attachments live forever).
    // A SINGLE trim pill floating on the graph -- the last control to leave
    // the bottom bar (which no longer exists; the graph fills its space).
    // Replaces two large rotary knobs: they were the only circles left in a
    // UI built from hairline pills, glyphs and monospace numbers, and were
    // the biggest things on screen for a set-and-forget control.
    //
    // One pill covers BOTH trims: its caption ("In"/"Out") is a switch --
    // click it to flip which trim the pill edits AND which signal the meter
    // shows. Everything else about the pill follows the mode.
    //
    // Bound via juce::ParameterAttachment (NOT SliderAttachment -- there's
    // no Slider here), one per parameter, so drags are proper host gestures
    // that land in undo history like every other control, and host/undo
    // changes push back into the display. Vertical drag adjusts; double-
    // click types. The value Label deliberately does NOT intercept clicks
    // (but its editor does), so the pill itself receives every drag and
    // double-click while typing still works normally.
    //
    // Defaults to monitoring OUT -- the trim reached for most often, and
    // the one whose level actually matters when bouncing.
    class GainPill final : public juce::Component
    {
    public:
        GainPill (EQAudioProcessor& proc);

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

        int idealWidth() const;

        // Magnetic proximity swell -- see DynControl::setMagnet, identical
        // contract. kMagnetPad pads this pill's bounds the same way.
        static constexpr int kMagnetPad = 4;
        void setMagnet (float m)
        {
            if (std::abs (m - magnet) > 0.005f) { magnet = m; repaint(); }
        }

        // See ReadoutPill::setSurfaceColour -- same reason, and this pill
        // sits near the BOTTOM of the graph where the flat token reads too
        // light rather than too dark.
        void setSurfaceColour (juce::Colour c)
        {
            if (c != surfaceColour) { surfaceColour = c; repaint(); }
        }

    private:
        juce::Colour surfaceColour { EQLookAndFeel::background() };

        float magnet = 0.0f;

        // The un-padded rect the pill VISUALLY occupies at rest -- every
        // internal layout measures from this, not getLocalBounds(), so the
        // magnet padding never shifts the caption or value.
        juce::Rectangle<int> visualBounds() const
        {
            return getLocalBounds().reduced (kMagnetPad);
        }

        bool monitoringOutput = true;    // false = In, true = Out (the default)
        juce::Rectangle<int> captionBounds() const;
        void setMode (bool wantOutput);
        void showValue (float newValue);
        void commitTypedText();

        juce::RangedAudioParameter& activeParam() const
        {
            return monitoringOutput ? outParam : inParam;
        }
        juce::ParameterAttachment& activeAttachment()
        {
            return monitoringOutput ? outAttachment : inAttachment;
        }

        EQAudioProcessor& processorRef;
        juce::RangedAudioParameter& inParam;
        juce::RangedAudioParameter& outParam;
        juce::ParameterAttachment inAttachment, outAttachment;

        juce::Label valueLabel;
        float valueAtDragStart = 0.0f;
        bool dragging = false;
        bool captionHot = false;        // cursor over the In/Out switch
    };

    GainPill gainPill;

    // (The dB range selector no longer lives here -- it's back ON the graph
    // as a click-for-menu bubble in the top-left corner, owned entirely by
    // EQCurveDisplay. See its rangeBubbleBounds()/showRangeMenu().)

    // "AudioFlower" brand mark, set in Indie Flower (embedded as a binary
    // resource -- see EQ.jucer's Resources group -- rather than looked up
    // by system font name, so it renders correctly on any machine, not just
    // ones that happen to have the font installed). Sits in row 2's
    // leftover middle space, centred the same way the freq/gain/Q readout
    // is centred in row 1. Tagged with a component ID so EQLookAndFeel::
    // getLabelFont() can recognise and exempt it from the house-font
    // override every other label gets.
    juce::Label logoLabel;
    juce::Typeface::Ptr logoTypeface;

    // Bypass toggle (row 2, right side, grouped with dbRangeBox). A real
    // APVTS parameter ("bypass"), also reported to the host as this plugin's
    // native bypass via EQAudioProcessor::getBypassParameter().
    // Global bypass -- just another SmallIconButton (the Tabler "power"
    // glyph, the same vocabulary a band's mute uses), so it renders at
    // exactly the size and inset as its top-bar neighbours. It briefly had
    // a bespoke class with an oversized glow; that made it visibly larger
    // than everything beside it and stopped it fitting the 20px bar.
    SmallIconButton bypassButton;
    std::unique_ptr<APVTS::ButtonAttachment> bypassAtt;

    // Bypass-dim fade: 0 = normal white background, 1 = fully dimmed. Polled
    // and stepped each timer tick (not driven directly by the button click)
    // since "bypass" is a real host-automatable parameter that can change
    // without a click here at all. Pushed into curve.setBackgroundDim() too,
    // so the graph darkens in lockstep with the control section.
    float bypassDim = 0.0f;

    // Readout-pill fade-in: 0 = fully transparent, 1 = fully visible. Only
    // reset to 0 in selectionChanged() when the pill goes from HIDDEN to
    // visible (the first band ever placed, or right after the previously-
    // selected band was deleted) -- switching selection between two bands
    // that are both already on screen doesn't refade. Stepped toward 1 each
    // timer tick, same pattern as bypassDim above.
    float rowFadeAlpha = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQAudioProcessorEditor)
};
